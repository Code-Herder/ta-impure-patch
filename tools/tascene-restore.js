// tascene-restore.js — the GLSL restorer, browser edition (renderers.md §4c).
//
// The shaders are tagpu_restore_glsl.h's, extracted into the pack by `tascene
// build` and compiled here under GLSL ES 3.00; the weights are
// unditherer/weights.py's file. Nothing in here is a second implementation of
// the model: this file is the DRIVER — batching frames into slots, binding the
// weight ranges, sequencing the passes — and tagpu_restoreglsl.c is the same
// driver in C. `restore=glsl` in the viewer runs it on the pack's own R8
// terrain atlas in place of the pack's pre-restored one, and on the feature
// atlas (colour-keyed, non-square frames — the lazy GAF restore's shape);
// `restorediff=1` then diffs both (the Q2 bar: max 1 level, < 0.01 % of bytes;
// for keyed frames over the texels beyond the model's reach of any key).
//
// BATCHING, shared with the C driver byte for byte: a frame's padded edge S is
// max(w, h) + 2·depth if it wraps; frames are taken in the order given, a
// batch holding frames of one SIZE CLASS (the ladder below), up to a square
// slot grid of min(8, floor(ACT_MAX / class)) per side; the batch's slot pitch
// is its largest S. Activations are sized to the largest cols·S of the run,
// never above ACT_MAX.

export const RESTORE_SHADERS = ["restore_fs.vert", "restore_fill.frag", "restore_conv.frag",
                                "restore_out.vert", "restore_out.frag"];
export const SLOT_COLS = 8, SLOT_ROWS = 8;          // the slot tables: 8x8 texels
export const ACT_MAX = 512;                          // activation side cap, texels
export const SIZE_CLASSES = [32, 48, 64, 96, 128, 192, 256, 384, 512];
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

// ---- is_tileable, on palette colours, as tagpu_rglsl_tileable does it -------
// A frame with its colour key on an edge is never wrapped: the reference
// decides tileability on the TELEA-inpainted image, which a shader cannot
// reproduce, and an edge-keyed sprite is exactly the case where that decision
// depends on the inpaint — so both sides zero-pad it. A key in the interior
// only (a hole) leaves the edges real, and the 12-level test stands.
export function isTileable(atlas, atlasW, ax, ay, w, h, pal, key = -1) {
  let lr = 0, tb = 0;
  if (key >= 0) {
    for (let y = 0; y < h; y++)
      if (atlas[(ay + y) * atlasW + ax] === key || atlas[(ay + y) * atlasW + ax + w - 1] === key) return false;
    for (let x = 0; x < w; x++)
      if (atlas[ay * atlasW + ax + x] === key || atlas[(ay + h - 1) * atlasW + ax + x] === key) return false;
  }
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
                  dx: ax, dy: ay, border, key: -1, id: i });
  }
  return frames;
}

// every frame of the pack's feature atlas (scene.features: the defs' body and
// shadow rects, `tascene build_features`), in place — the pack's shelf packs
// frames flush with no border, so border is 0 here where the engine's atlas
// carries 1. Each frame's key is its GAF colour key.
export function featureFrames(features, atlas, pal) {
  const frames = [], seen = new Set();
  const W = features.atlas.w;
  for (const d of Object.values(features.defs || {})) {
    for (const slot of ["body", "shadow"]) {
      const r = d[slot];
      if (!r) continue;
      const k = `${r.x},${r.y}`;
      if (seen.has(k)) continue;
      seen.add(k);
      frames.push({ ax: r.x, ay: r.y, w: r.w, h: r.h,
                    wrap: isTileable(atlas, W, r.x, r.y, r.w, r.h, pal, r.ck),
                    dx: r.x, dy: r.y, border: 0, key: r.ck, id: frames.length, name: d.name });
    }
  }
  return frames;
}

// the batcher, shared with tagpu_restoreglsl.c: -> [{S, cols, frames}]
export function batchFrames(frames, depth) {
  let rest = frames.map((f) => {
    let wrap = !!f.wrap, S = Math.max(f.w, f.h) + (wrap ? 2 * depth : 0);
    if (S > ACT_MAX && wrap) { wrap = false; S = Math.max(f.w, f.h); }    // too big to wrap: zero-pad instead
    if (S > ACT_MAX) throw new Error(`frame ${f.id}: ${f.w}x${f.h} exceeds the ${ACT_MAX}-texel slot`);
    return { f: wrap === !!f.wrap ? f : { ...f, wrap }, S, cls: SIZE_CLASSES.find((c) => c >= S) };
  });
  const batches = [];
  while (rest.length) {
    const cls = rest[0].cls;
    const cols = Math.min(SLOT_COLS, Math.floor(ACT_MAX / cls)), cap = cols * cols;
    const take = [], keep = [];
    for (const p of rest) (p.cls === cls && take.length < cap ? take : keep).push(p);
    batches.push({ S: Math.max(...take.map((p) => p.S)), cols, frames: take.map((p) => p.f) });
    rest = keep;
  }
  return batches;
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
    nk = nk >= 8 ? 8 : nk >= 4 ? 4 : nk >= 2 ? 2 : nk >= 1 ? 1 : 0;   // a power of two: divides 16 and 6 (with a spare)
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
    this.fillU = U(this.fill, ["uAtlas", "uPal", "uRect", "uSrc", "uKey", "uSlot", "uKeyR"]);
    this.convU = U(this.conv, ["uAct", "uRect", "uJin", "uKStride", "uSlot", "uRelu"]);
    this.outU = U(this.out, ["uAct", "uAtlas", "uPal", "uRect", "uKey", "uSlot", "uDst"]);
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
    this.rectTex = slotTex(); this.srcTex = slotTex(); this.keyTex = slotTex();
    this.rectData = new Float32Array(SLOT_COLS * SLOT_ROWS * 4);
    this.srcData = new Float32Array(SLOT_COLS * SLOT_ROWS * 4);
    this.keyData = new Float32Array(SLOT_COLS * SLOT_ROWS * 4);
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

  // the ping-pong arrays, sized for the largest slot grid of the run
  ensureAct(side) {
    const gl = this.gl;
    if (this.act && this.actSize >= side) return;
    if (this.act) for (const t of this.act) gl.deleteTexture(t);
    const layers = Math.max(1, this.W.ch / 4);
    const fmt = this.precision === "fp16" ? gl.RGBA16F : gl.RGBA32F;
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

  // frames: [{ax, ay, w, h, wrap, dx, dy, border, key}] read from `atlas` (an
  // R8 texture of atlasW x atlasH) and painted into `dest` {tex, w, h}
  // (RGBA8, cleared to 0 first: alpha 0 is "not painted", and the hole of a
  // keyed frame). Two clocks per batch: the wall clock across a gl.finish(),
  // and the GPU's own through EXT_disjoint_timer_query_webgl2 where it
  // exists -- the one that still means something under headless Chrome's
  // virtual time, where performance.now() stands still during synchronous
  // work. The 2D bindings of units 0-5 (and unit 4's array binding) are put
  // back as they were, so a viewer that keeps its own textures on them is not
  // disturbed.
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
    const saved = [];
    for (let u = 0; u < 6; u++) {
      gl.activeTexture(gl.TEXTURE0 + u);
      saved.push(gl.getParameter(gl.TEXTURE_BINDING_2D));
    }
    gl.activeTexture(gl.TEXTURE4);
    const savedArray = gl.getParameter(gl.TEXTURE_BINDING_2D_ARRAY);
    const w0 = await wall();
    const t0 = performance.now();
    const batches = batchFrames(frames, depth);
    this.ensureAct(Math.max(...batches.map((b) => b.cols * b.S)));

    // the destination starts as zeros: unpainted, and a diff is deterministic
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.destFBO);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, dest.tex, 0);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    gl.disable(gl.SCISSOR_TEST);
    gl.colorMask(true, true, true, true);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    // state the passes share
    gl.disable(gl.DEPTH_TEST); gl.disable(gl.BLEND); gl.disable(gl.CULL_FACE);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, atlas);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, pal);
    gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, this.rectTex);
    gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, this.srcTex);
    gl.activeTexture(gl.TEXTURE5); gl.bindTexture(gl.TEXTURE_2D, this.keyTex);

    const times = [];
    let draws = 0, wrapped = 0, keyed = 0;
    const outVerts = new Float32Array(SLOT_COLS * SLOT_ROWS * 6 * 8);
    for (const b of batches) {
      const tb = performance.now();
      const query = timer ? gl.createQuery() : null;
      if (query) gl.beginQuery(timer.TIME_ELAPSED_EXT, query);
      const S = b.S, cols = b.cols, TW = cols * S, TH = cols * S;
      // per-slot tables: slot s at (s % cols, s / cols) of the 8x8 table
      this.rectData.fill(0); this.srcData.fill(0); this.keyData.fill(-1);
      b.frames.forEach((f, s) => {
        const pad = f.wrap ? depth : 0;
        const i = (Math.floor(s / cols) * SLOT_COLS + (s % cols)) * 4;
        if (f.wrap) wrapped++;
        if (f.key >= 0) keyed++;
        this.rectData.set([0, 0, f.w + 2 * pad, f.h + 2 * pad], i);
        this.srcData.set([f.ax, f.ay, f.w, f.h], i);
        this.keyData.set([f.key === undefined ? -1 : f.key, 0, 0, 0], i);
      });
      gl.activeTexture(gl.TEXTURE2);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, gl.RGBA, gl.FLOAT, this.rectData);
      gl.activeTexture(gl.TEXTURE3);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, gl.RGBA, gl.FLOAT, this.srcData);
      gl.activeTexture(gl.TEXTURE5);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, SLOT_COLS, SLOT_ROWS, gl.RGBA, gl.FLOAT, this.keyData);

      // FILL -> A layer 0
      let src = this.act[0], dst = this.act[1];
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
      gl.viewport(0, 0, TW, TH);
      gl.bindVertexArray(this.vao);
      this.attach(src, 0, 1);
      gl.useProgram(this.fill);
      gl.uniform1i(this.fillU.uAtlas, 0); gl.uniform1i(this.fillU.uPal, 1);
      gl.uniform1i(this.fillU.uRect, 2); gl.uniform1i(this.fillU.uSrc, 3);
      gl.uniform1i(this.fillU.uKey, 5); gl.uniform1i(this.fillU.uKeyR, depth);
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
        const sc = s % cols, sr = Math.floor(s / cols);
        for (const [x, y] of [[x0, y0], [x1, y0], [x0, y1], [x1, y0], [x1, y1], [x0, y1]]) {
          outVerts.set([x, y, f.dx, f.dy, sc, sr, f.w, f.h], nv * 8); nv++;
        }
      });
      gl.useProgram(this.out);
      gl.uniform1i(this.outU.uAct, 4); gl.uniform1i(this.outU.uAtlas, 0);
      gl.uniform1i(this.outU.uPal, 1); gl.uniform1i(this.outU.uRect, 2);
      gl.uniform1i(this.outU.uKey, 5);
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
      times.push({ S, cols, n: b.frames.length, ms: performance.now() - tb, gpu_ms: null, query });
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
    for (let u = 0; u < 6; u++) {
      gl.activeTexture(gl.TEXTURE0 + u);
      gl.bindTexture(gl.TEXTURE_2D, saved[u]);
    }
    gl.activeTexture(gl.TEXTURE4); gl.bindTexture(gl.TEXTURE_2D_ARRAY, savedArray);
    gl.activeTexture(gl.TEXTURE0);
    const total = performance.now() - t0;
    const wallTotal = (await wall()) - w0;
    const by = {};
    let gpu = 0, gpuKnown = !!timer;
    for (const t of times) {
      const k = String(t.S);
      by[k] = by[k] || { batches: 0, frames: 0, ms: 0, gpu_ms: 0, cols: t.cols };
      by[k].batches++; by[k].frames += t.n; by[k].ms += t.ms;
      if (t.gpu_ms === null) gpuKnown = false; else { by[k].gpu_ms += t.gpu_ms; gpu += t.gpu_ms; }
    }
    const err = gl.getError();
    this.stats = { frames: frames.length, wrapped, keyed, batches: batches.length, draws, ms: total,
                   wall_ms: wallTotal, gpu_ms: gpuKnown ? gpu : null, timer: !!timer,
                   first_ms: times.length ? times[0].ms : 0,
                   first_gpu_ms: times.length ? times[0].gpu_ms : null,
                   by_slot: by, nk: this.nk, wmax: this.wmax,
                   precision: this.precision, model: `${W.depth}x${W.ch}`, act_bytes: this.actBytes,
                   act_side: this.actSize, limits: this.limits, gl_error: err };
    return this.stats;
  }

  dispose() {
    const gl = this.gl;
    for (const p of [this.fill, this.conv, this.out]) gl.deleteProgram(p);
    gl.deleteBuffer(this.ubo); gl.deleteBuffer(this.outVBO);
    gl.deleteTexture(this.rectTex); gl.deleteTexture(this.srcTex); gl.deleteTexture(this.keyTex);
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

// The same measurement for colour-keyed frames in a shelf atlas (`got` and
// `ref` share the layout, `r8` is the source atlas the keys are read from).
// Keyed texels must come back alpha 0 and are not compared; an opaque texel
// farther than `radius` (the model's depth, its receptive radius) from every
// keyed texel of its frame is held to the Q2 bar — `far` — and the rest, the
// band the inpaint stand-in reaches, is reported as `near` and judged by eye
// (renderers.md 4c). Distances wrap for a wrapped frame, as the taps do.
export function diffFrames(got, ref, frames, r8, W, radius) {
  const hist = { far: new Array(256).fill(0), near: new Array(256).fill(0) };
  const far = { bytes: 0, differing: 0, max: 0 }, near = { bytes: 0, differing: 0, max: 0, sum: 0 };
  let keyedTexels = 0, opaqueTexels = 0, alphaBad = 0, ring = 0, ringBytes = 0, worst = -1, worstN = 0;
  for (const f of frames) {
    const { ax, ay, w, h, key } = f, B = f.border || 0;
    const keyed = new Uint8Array(w * h);
    if (key >= 0)
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++)
        keyed[y * w + x] = r8[(ay + y) * W + ax + x] === key ? 1 : 0;
    // Chebyshev distance to the nearest keyed texel, capped at radius + 1
    const dist = (x, y) => {
      if (key < 0) return radius + 1;
      for (let r = 1; r <= radius; r++)
        for (let i = -r; i < r; i++)
          for (const [dx, dy] of [[i, -r], [r, i], [-i, r], [-r, -i]]) {
            let tx = x + dx, ty = y + dy;
            if (f.wrap) { tx = ((tx % w) + w) % w; ty = ((ty % h) + h) % h; }
            else if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            if (keyed[ty * w + tx]) return r;
          }
      return radius + 1;
    };
    let n = 0;
    for (let y = -B; y < h + B; y++)
      for (let x = -B; x < w + B; x++) {
        const o = ((ay + y) * W + ax + x) * 4;
        const inside = x >= 0 && y >= 0 && x < w && y < h;
        if (!inside) {
          const cx = Math.max(0, Math.min(w - 1, x)), cy = Math.max(0, Math.min(h - 1, y));
          const e = ((ay + cy) * W + ax + cx) * 4;
          for (let c = 0; c < 4; c++) { ringBytes++; if (got[o + c] !== got[e + c]) ring++; }
          continue;
        }
        if (keyed[y * w + x]) {
          keyedTexels++;
          if (got[o + 3] !== 0) alphaBad++;
          continue;
        }
        opaqueTexels++;
        if (got[o + 3] !== 255) alphaBad++;
        const band = dist(x, y) > radius ? far : near;
        const hh = band === far ? hist.far : hist.near;
        for (let c = 0; c < 3; c++) {
          const d = Math.abs(got[o + c] - ref[o + c]);
          band.bytes++;
          if (d) { band.differing++; n++; hh[d]++; if (d > band.max) band.max = d; if (band === near) near.sum += d; }
        }
      }
    if (n > worstN) { worstN = n; worst = f.id; }
  }
  const pct = (b) => 100 * b.differing / Math.max(1, b.bytes);
  const farPass = far.max <= 1 && pct(far) < 0.01;
  return {
    frames: frames.length, keyed_frames: frames.filter((f) => f.key >= 0).length,
    keyed_texels: keyedTexels, opaque_texels: opaqueTexels, alpha_bad: alphaBad, radius,
    far: { bytes: far.bytes, differing: far.differing, pct: pct(far), max: far.max,
           hist: hist.far.slice(1, Math.max(2, far.max + 1)), pass: farPass },
    near: { bytes: near.bytes, differing: near.differing, pct: pct(near), max: near.max,
            mean: near.bytes ? near.sum / near.bytes : 0,
            hist: hist.near.slice(1, Math.max(2, near.max + 1)) },
    ring, ring_bytes: ringBytes, worst, worst_bytes: worstN,
    pass: farPass && alphaBad === 0 && ring === 0,
  };
}
