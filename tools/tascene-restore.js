// tascene-restore.js — the GLSL restorer, browser edition (renderers.md §4c).
//
// The shaders are tagpu_restore_glsl.h's, extracted into the pack by `tascene
// build` and compiled here under GLSL ES 3.00; the weights are
// unditherer/weights.py's file. Nothing in here is a second implementation of
// the model: this file is the DRIVER — batching frames into slots, binding the
// weight ranges, sequencing the passes — and tagpu_restoreglsl.c is the same
// driver in C. `restore=glsl` in the viewer runs it on the pack's own R8
// terrain atlas in place of the pack's pre-restored one; `restorediff=1` then
// diffs the two (the Q2 bar: max 1 level, < 0.01 % of bytes).

export const RESTORE_SHADERS = ["restore_fs.vert", "restore_fill.frag", "restore_conv.frag",
                                "restore_out.vert", "restore_out.frag"];
export const SLOT_COLS = 8, SLOT_ROWS = 8;          // frames per batch: 64
export const TILEABLE_THR = 12.0;                    // classical.is_tileable

const ES_PREFIX = "#version 300 es\nprecision highp float;\nprecision highp int;\n" +
                  "precision highp sampler2D;\nprecision highp sampler2DArray;\n";

// ---- the weight file --------------------------------------------------------
export function parseWeights(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]) !== "TAW1")
    throw new Error("weights: not a TAW1 file");
  const depth = dv.getUint32(4, true), ch = dv.getUint32(8, true), ntex = dv.getUint32(12, true);
  const layers = [];
  for (let l = 0; l < depth; l++) {
    const o = 16 + 16 * l;
    layers.push({ offset: dv.getUint32(o, true), jin: dv.getUint32(o + 4, true),
                  kout: dv.getUint32(o + 8, true), kstride: dv.getUint32(o + 12, true) });
  }
  const start = 16 + 16 * depth;
  if (bytes.byteLength !== start + ntex * 16)
    throw new Error(`weights: body is ${bytes.byteLength - start} bytes, header says ${ntex * 16}`);
  // a Float32Array view needs 4-byte alignment; copy when the fetch buffer is not
  const body = new Float32Array(ntex * 4);
  body.set(new Float32Array(bytes.slice(start).buffer));
  const kmax = Math.max(...layers.map((L) => L.kstride)) / 4;      // mat4s in the widest k-block
  return { depth, ch, ntex, layers, body, kmax };
}

// ---- is_tileable, on palette colours, as tagpu_restore.c does it ------------
export function isTileable(atlas, atlasW, ax, ay, w, h, pal) {
  let lr = 0, tb = 0;
  for (let y = 0; y < h; y++) {
    const a = atlas[(ay + y) * atlasW + ax] * 4, b = atlas[(ay + y) * atlasW + ax + w - 1] * 4;
    lr += Math.abs(pal[a] - pal[b]) + Math.abs(pal[a + 1] - pal[b + 1]) + Math.abs(pal[a + 2] - pal[b + 2]);
  }
  for (let x = 0; x < w; x++) {
    const u = atlas[ay * atlasW + ax + x] * 4, d = atlas[(ay + h - 1) * atlasW + ax + x] * 4;
    tb += Math.abs(pal[u] - pal[d]) + Math.abs(pal[u + 1] - pal[d + 1]) + Math.abs(pal[u + 2] - pal[d + 2]);
  }
  return lr / (3 * h) < TILEABLE_THR && tb / (3 * w) < TILEABLE_THR;
}

// every tile of a tagpu-layout atlas as a frame: source and destination share
// the layout, so dx,dy = ax,ay and the out pass paints the 1-texel guard too
export function terrainFrames(map, atlas, pal) {
  const frames = [];
  const pitch = map.atlas.pitch || 32, border = map.atlas.border || 0;
  for (let i = 0; i < map.atlas.tiles; i++) {
    const ax = (i % map.atlas.cols) * pitch + border, ay = Math.floor(i / map.atlas.cols) * pitch + border;
    frames.push({ ax, ay, w: 32, h: 32, wrap: isTileable(atlas, map.atlas.w, ax, ay, 32, 32, pal),
                  dx: ax, dy: ay, border, id: i });
  }
  return frames;
}

// ---- the driver ---------------------------------------------------------------
function compile(gl, type, prefix, body, label) {
  const sh = gl.createShader(type);
  gl.shaderSource(sh, prefix + body);
  gl.compileShader(sh);
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS))
    throw new Error(`${label}: ${gl.getShaderInfoLog(sh)}`);
  return sh;
}

function link(gl, vs, fs, label) {
  const p = gl.createProgram();
  gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS))
    throw new Error(`${label}: ${gl.getProgramInfoLog(p)}`);
  return p;
}

export class Restorer {
  // shaders: {name: body} for RESTORE_SHADERS; weights: parseWeights()'s result
  constructor(gl, shaders, weights, opts = {}) {
    this.gl = gl; this.W = weights;
    this.precision = opts.precision || "fp32";
    this.log = opts.log || (() => {});
    if (!gl.getExtension("EXT_color_buffer_float"))
      throw new Error("restore=glsl needs EXT_color_buffer_float (float render targets); this WebGL2 has none");
    const maxUBO = gl.getParameter(gl.MAX_UNIFORM_BLOCK_SIZE);
    const maxDraw = Math.min(gl.getParameter(gl.MAX_DRAW_BUFFERS), gl.getParameter(gl.MAX_COLOR_ATTACHMENTS));
    const align = gl.getParameter(gl.UNIFORM_BUFFER_OFFSET_ALIGNMENT);
    if (align > 256) throw new Error(`UNIFORM_BUFFER_OFFSET_ALIGNMENT ${align} > 256: the weight file's padding is too small`);
    const kbytes = weights.kmax * 64;                       // one k-block, bytes
    let nk = Math.min(maxDraw, Math.floor(maxUBO / kbytes), 8);
    nk = nk >= 8 ? 8 : nk >= 4 ? 4 : nk >= 2 ? 2 : 1;       // a power of two: divides 16 and 6 (with a spare)
    if (nk < 1) throw new Error(`MAX_UNIFORM_BLOCK_SIZE ${maxUBO} < one k-block (${kbytes} bytes)`);
    if (opts.nk) {
      if (opts.nk > nk) throw new Error(`nk=${opts.nk}: this device allows ${nk} (MAX_UNIFORM_BLOCK_SIZE ${maxUBO}, draw buffers ${maxDraw})`);
      nk = opts.nk;
    }
    this.nk = nk; this.wmax = nk * weights.kmax;
    this.limits = { maxUBO, maxDraw, align, kbytes };

    const prefix = ES_PREFIX + `#define NK ${nk}\n#define WMAX ${this.wmax}\n`;
    const fsVS = compile(gl, gl.VERTEX_SHADER, prefix, shaders["restore_fs.vert"], "restore_fs.vert");
    this.fill = link(gl, fsVS, compile(gl, gl.FRAGMENT_SHADER, prefix, shaders["restore_fill.frag"], "restore_fill.frag"), "fill");
    this.conv = link(gl, fsVS, compile(gl, gl.FRAGMENT_SHADER, prefix, shaders["restore_conv.frag"], "restore_conv.frag"), "conv");
    this.out = link(gl, compile(gl, gl.VERTEX_SHADER, prefix, shaders["restore_out.vert"], "restore_out.vert"),
                    compile(gl, gl.FRAGMENT_SHADER, prefix, shaders["restore_out.frag"], "restore_out.frag"), "out");
    const U = (p, names) => Object.fromEntries(names.map((n) => [n, gl.getUniformLocation(p, n)]));
    this.fillU = U(this.fill, ["uAtlas", "uPal", "uRect", "uSrc", "uSlot"]);
    this.convU = U(this.conv, ["uAct", "uRect", "uJin", "uKStride", "uSlot", "uRelu"]);
    this.outU = U(this.out, ["uAct", "uAtlas", "uPal", "uRect", "uSlot", "uDst"]);
    gl.uniformBlockBinding(this.conv, gl.getUniformBlockIndex(this.conv, "WBlock"), 0);

    // the weights: one buffer, bound WMAX mat4s at a time, so pad the tail
    this.ubo = gl.createBuffer();
    gl.bindBuffer(gl.UNIFORM_BUFFER, this.ubo);
    const padded = new Float32Array(weights.body.length + this.wmax * 16);
    padded.set(weights.body);
    gl.bufferData(gl.UNIFORM_BUFFER, padded, gl.STATIC_DRAW);
    gl.bindBuffer(gl.UNIFORM_BUFFER, null);

    // per-slot tables, SLOT_COLS x SLOT_ROWS texels of RGBA32F
    const slotTex = () => {
      const t = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32F, SLOT_COLS, SLOT_ROWS);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      return t;
    };
    this.rectTex = slotTex(); this.srcTex = slotTex();
    this.rectData = new Float32Array(SLOT_COLS * SLOT_ROWS * 4);
    this.srcData = new Float32Array(SLOT_COLS * SLOT_ROWS * 4);
    this.vao = gl.createVertexArray();
    this.outVAO = gl.createVertexArray();
    this.outVBO = gl.createBuffer();
    gl.bindVertexArray(this.outVAO);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.outVBO);
    for (const [loc, n, off] of [[0, 2, 0], [1, 4, 8], [2, 2, 24]]) {
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, n, gl.FLOAT, false, 32, off);
    }
    gl.bindVertexArray(null);
    this.fbo = gl.createFramebuffer();      // fill and conv: attachments set per draw
    this.destFBO = gl.createFramebuffer();
    this.attached = 0;
    this.act = null; this.actSize = 0;
    this.syncPx = new Uint8Array(4);
    this.stats = null;
  }

  // the ping-pong arrays, sized for the largest slot pitch of the run
  ensureAct(TW, TH) {
    const gl = this.gl;
    if (this.act && this.actSize >= Math.max(TW, TH)) return;
    if (this.act) for (const t of this.act) gl.deleteTexture(t);
    const layers = Math.max(1, this.W.ch / 4);
    const fmt = this.precision === "fp16" ? gl.RGBA16F : gl.RGBA32F;
    const side = Math.max(TW, TH);
    this.act = [0, 1].map(() => {
      const t = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D_ARRAY, t);
      gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, fmt, side, side, layers);
      gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      return t;
    });
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
    this.actSize = side;
    this.actBytes = 2 * side * side * layers * (this.precision === "fp16" ? 8 : 16);
  }

  attach(tex, first, count) {
    // colour attachments 0..count-1 <- layers first..first+count-1 of tex; the
    // rest detached, so a layer we read next is never left on the framebuffer
    const gl = this.gl;
    const bufs = [];
    for (let n = 0; n < count; n++) {
      gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0 + n, tex, 0, first + n);
      bufs.push(gl.COLOR_ATTACHMENT0 + n);
    }
    for (let n = count; n < this.attached; n++) {
      gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0 + n, null, 0, 0);
      bufs.push(gl.NONE);
    }
    this.attached = count;
    gl.drawBuffers(bufs);
  }

  // frames: [{ax, ay, w, h, wrap, dx, dy, border}] read from `atlas` (an R8
  // texture of atlasW x atlasH) and painted into `dest` {tex, w, h} (RGBA8).
  // Two clocks per batch: the wall clock across a gl.finish(), and the GPU's
  // own through EXT_disjoint_timer_query_webgl2 where it exists -- the one
  // that still means something under headless Chrome's virtual time, where
  // performance.now() stands still during synchronous work.
  async restore(frames, atlas, pal, dest) {
    const gl = this.gl, W = this.W, depth = W.depth;
    const timer = gl.getExtension("EXT_disjoint_timer_query_webgl2");
    const tick = () => new Promise((r) => setTimeout(r, 0));
    // the server's clock (tascene serves __now) is the one that keeps running
    // under headless Chrome's virtual time; performance.now() is the fallback
    const wall = async () => {
      try { return parseFloat(await (await fetch("__now", { cache: "no-store" })).text()) * 1000; }
      catch (e) { return performance.now(); }
    };
    const w0 = await wall();
    const t0 = performance.now();
    // batches: same padded size, up to 64 per batch, in the order given
    const groups = new Map();
    for (const f of frames) {
      const pad = f.wrap ? depth : 0;
      const pw = f.w + 2 * pad, ph = f.h + 2 * pad;
      const key = `${pw}x${ph}`;
      if (!groups.has(key)) groups.set(key, { S: Math.max(pw, ph), pw, ph, frames: [] });
      groups.get(key).frames.push(f);
    }
    const batches = [];
    for (const g of groups.values())
      for (let i = 0; i < g.frames.length; i += SLOT_COLS * SLOT_ROWS)
        batches.push({ S: g.S, frames: g.frames.slice(i, i + SLOT_COLS * SLOT_ROWS) });
    const Smax = Math.max(...batches.map((b) => b.S));
    this.ensureAct(SLOT_COLS * Smax, SLOT_ROWS * Smax);

    // state the passes share
    gl.disable(gl.DEPTH_TEST); gl.disable(gl.BLEND); gl.disable(gl.SCISSOR_TEST);
    gl.disable(gl.CULL_FACE);
    gl.colorMask(true, true, true, true);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, atlas);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, pal);
    gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, this.rectTex);
    gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, this.srcTex);

    const times = [];
    let draws = 0, wrapped = 0;
    const outVerts = new Float32Array(SLOT_COLS * SLOT_ROWS * 6 * 8);
    for (const b of batches) {
      const tb = performance.now();
      const query = timer ? gl.createQuery() : null;
      if (query) gl.beginQuery(timer.TIME_ELAPSED_EXT, query);
      const S = b.S, TW = SLOT_COLS * S, TH = SLOT_ROWS * S;
      // per-slot tables
      this.rectData.fill(0); this.srcData.fill(0);
      b.frames.forEach((f, s) => {
        const pad = f.wrap ? depth : 0;
        if (f.wrap) wrapped++;
        this.rectData.set([0, 0, f.w + 2 * pad, f.h + 2 * pad], s * 4);
        this.srcData.set([f.ax, f.ay, f.w, f.h], s * 4);
      });
      gl.bindTexture(gl.TEXTURE_2D, this.rectTex);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, gl.RGBA, gl.FLOAT, this.rectData);
      gl.bindTexture(gl.TEXTURE_2D, this.srcTex);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, gl.RGBA, gl.FLOAT, this.srcData);
      gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, this.rectTex);
      gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, this.srcTex);

      // FILL -> A layer 0
      let src = this.act[0], dst = this.act[1];
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
      gl.viewport(0, 0, TW, TH);
      gl.bindVertexArray(this.vao);
      this.attach(src, 0, 1);
      gl.useProgram(this.fill);
      gl.uniform1i(this.fillU.uAtlas, 0); gl.uniform1i(this.fillU.uPal, 1);
      gl.uniform1i(this.fillU.uRect, 2); gl.uniform1i(this.fillU.uSrc, 3);
      gl.uniform1i(this.fillU.uSlot, S);
      gl.drawArrays(gl.TRIANGLES, 0, 3); draws++;

      // CONV x depth, ping-pong
      gl.useProgram(this.conv);
      gl.uniform1i(this.convU.uAct, 4); gl.uniform1i(this.convU.uRect, 2);
      gl.uniform1i(this.convU.uSlot, S);
      for (let l = 0; l < depth; l++) {
        const L = W.layers[l];
        gl.activeTexture(gl.TEXTURE4); gl.bindTexture(gl.TEXTURE_2D_ARRAY, src);
        gl.uniform1i(this.convU.uJin, L.jin);
        gl.uniform1i(this.convU.uKStride, L.kstride / 4);
        gl.uniform1i(this.convU.uRelu, l < depth - 1 ? 1 : 0);
        for (let g = 0; g < L.kout; g += this.nk) {
          this.attach(dst, g, Math.min(this.nk, L.kout - g));
          gl.bindBufferRange(gl.UNIFORM_BUFFER, 0, this.ubo, (L.offset + g * L.kstride) * 16, this.wmax * 64);
          gl.drawArrays(gl.TRIANGLES, 0, 3); draws++;
        }
        [src, dst] = [dst, src];
      }

      // OUT -> dest, one quad per frame over its bordered cell
      gl.activeTexture(gl.TEXTURE4); gl.bindTexture(gl.TEXTURE_2D_ARRAY, src);
      this.attach(null, 0, 0);
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.destFBO);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, dest.tex, 0);
      gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
      gl.viewport(0, 0, dest.w, dest.h);
      let nv = 0;
      b.frames.forEach((f, s) => {
        const B = f.border || 0;
        const x0 = f.dx - B, y0 = f.dy - B, x1 = f.dx + f.w + B, y1 = f.dy + f.h + B;
        const sc = s % SLOT_COLS, sr = Math.floor(s / SLOT_COLS);
        for (const [x, y] of [[x0, y0], [x1, y0], [x0, y1], [x1, y0], [x1, y1], [x0, y1]]) {
          outVerts.set([x, y, f.dx, f.dy, sc, sr, f.w, f.h], nv * 8); nv++;
        }
      });
      gl.useProgram(this.out);
      gl.uniform1i(this.outU.uAct, 4); gl.uniform1i(this.outU.uAtlas, 0);
      gl.uniform1i(this.outU.uPal, 1); gl.uniform1i(this.outU.uRect, 2);
      gl.uniform1i(this.outU.uSlot, S); gl.uniform2f(this.outU.uDst, dest.w, dest.h);
      gl.bindVertexArray(this.outVAO);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.outVBO);
      gl.bufferData(gl.ARRAY_BUFFER, outVerts.subarray(0, nv * 8), gl.STREAM_DRAW);
      gl.drawArrays(gl.TRIANGLES, 0, nv); draws++;
      gl.bindVertexArray(null);
      if (query) gl.endQuery(timer.TIME_ELAPSED_EXT);
      // the sync point. gl.finish() returns before the GPU is done under
      // Chrome's ANGLE/Vulkan (measured: 5062 tiles "in 20 ms"); a readback
      // cannot, so one texel of the destination is read after every batch
      gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, this.syncPx);
      times.push({ S, n: b.frames.length, ms: performance.now() - tb, gpu_ms: null, query });
    }
    if (timer) {
      // A query result is never available in the task that issued it, and every
      // task boundary costs headless Chrome virtual time -- so all the batches
      // ran back to back above, and the results are collected here after as
      // few boundaries as it takes (the work is already finished).
      const pending = () => times.some((t) => t.query && !gl.getQueryParameter(t.query, gl.QUERY_RESULT_AVAILABLE));
      for (let i = 0; i < 50 && pending(); i++) await tick();
      const disjoint = gl.getParameter(timer.GPU_DISJOINT_EXT);
      for (const t of times) {
        if (!disjoint && gl.getQueryParameter(t.query, gl.QUERY_RESULT_AVAILABLE))
          t.gpu_ms = Number(gl.getQueryParameter(t.query, gl.QUERY_RESULT)) / 1e6;
        gl.deleteQuery(t.query);
        delete t.query;
      }
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.activeTexture(gl.TEXTURE4); gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
    const total = performance.now() - t0;
    const wallTotal = (await wall()) - w0;
    const by = {};
    let gpu = 0, gpuKnown = !!timer;
    for (const t of times) {
      const k = String(t.S);
      by[k] = by[k] || { batches: 0, frames: 0, ms: 0, gpu_ms: 0 };
      by[k].batches++; by[k].frames += t.n; by[k].ms += t.ms;
      if (t.gpu_ms === null) gpuKnown = false; else { by[k].gpu_ms += t.gpu_ms; gpu += t.gpu_ms; }
    }
    const err = gl.getError();
    this.stats = { frames: frames.length, wrapped, batches: batches.length, draws, ms: total,
                   wall_ms: wallTotal, gpu_ms: gpuKnown ? gpu : null, timer: !!timer,
                   first_ms: times.length ? times[0].ms : 0,
                   first_gpu_ms: times.length ? times[0].gpu_ms : null,
                   by_slot: by, nk: this.nk, wmax: this.wmax,
                   precision: this.precision, model: `${W.depth}x${W.ch}`, act_bytes: this.actBytes,
                   limits: this.limits, gl_error: err };
    return this.stats;
  }

  dispose() {
    const gl = this.gl;
    for (const p of [this.fill, this.conv, this.out]) gl.deleteProgram(p);
    gl.deleteBuffer(this.ubo); gl.deleteBuffer(this.outVBO);
    gl.deleteTexture(this.rectTex); gl.deleteTexture(this.srcTex);
    if (this.act) for (const t of this.act) gl.deleteTexture(t);
    gl.deleteFramebuffer(this.fbo); gl.deleteFramebuffer(this.destFBO);
    gl.deleteVertexArray(this.vao); gl.deleteVertexArray(this.outVAO);
  }
}

// read an RGBA8 texture back
export function readTexture(gl, tex, w, h) {
  const fbo = gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
  const out = new Uint8Array(w * h * 4);
  gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, out);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.deleteFramebuffer(fbo);
  return out;
}

// The Q2 measurement: our atlas against the pack's, over every tile's 32x32
// interior (RGB bytes), plus the guard ring checked separately (it must copy
// the edge, in both). `hist[d]` counts interior bytes differing by d.
export function diffAtlas(got, ref, map) {
  const W = map.atlas.w, pitch = map.atlas.pitch || 32, border = map.atlas.border || 0;
  const hist = new Array(256).fill(0);
  let bytes = 0, differing = 0, max = 0, worst = -1, worstN = 0, ring = 0, ringBytes = 0;
  const clamp = (v) => Math.max(0, Math.min(31, v));
  for (let i = 0; i < map.atlas.tiles; i++) {
    const ax = (i % map.atlas.cols) * pitch + border, ay = Math.floor(i / map.atlas.cols) * pitch + border;
    let n = 0;
    for (let y = -border; y < 32 + border; y++)
      for (let x = -border; x < 32 + border; x++) {
        const o = ((ay + y) * W + ax + x) * 4;
        const inside = x >= 0 && y >= 0 && x < 32 && y < 32;
        // the ring is a layout check on OUR bytes: it must copy our own edge
        const e = ((ay + clamp(y)) * W + ax + clamp(x)) * 4;
        for (let c = 0; c < 3; c++) {
          if (inside) {
            const d = Math.abs(got[o + c] - ref[o + c]);
            bytes++;
            if (d) { differing++; n++; hist[d]++; if (d > max) max = d; }
          } else {
            ringBytes++;
            if (got[o + c] !== got[e + c]) ring++;
          }
        }
      }
    if (n > worstN) { worstN = n; worst = i; }
  }
  const pct = 100 * differing / Math.max(1, bytes);
  return { bytes, differing, pct, max, worst, worst_bytes: worstN, ring, ring_bytes: ringBytes,
           hist: hist.slice(1, Math.max(2, max + 1)),
           pass: max <= 1 && pct < 0.01 && ring === 0 };
}
