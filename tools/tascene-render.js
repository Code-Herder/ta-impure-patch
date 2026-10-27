/* tascene-render.js -- the Classic lane of the tascene lab: tagpu's own shaders over a scene pack, in WebGL2.

   Loaded by tools/tascene-view.html (the lab, which keeps its Classic++ lane) and by the in-game wiki's mockup,
   tools/wiki/mockup/gfx.js, which draws the wiki's stage with it. It is the code the lab's shot md5s hold to the game
   (research/notes/tascene-design.md, *The A/B, measured*), so the wiki's stage and the lab cannot drift: a change
   here moves both, and `tools/tascene shot` on the lab's fixtures says whether it moved the picture.

   What it holds, each the engine's rule as the lab ports it:
   - the GL helpers, the base atlas (every index expanded to its palette colour, alpha 0 at a frame's key) and the
     face-shade multiplier (tagpu_render3do.c `s_shadeK`);
   - the terrain: tagpu_terr.c's gather, one instance a visible 32-px cell, and its instanced draw;
   - the features: tagpu_feat.c's sweep rect, projection and depth keys (and the lab's mirror past the map's edge,
     which shares the sweep), and the sprite draw;
   - the units: one instance's yaw, projection, per-face flat shade and intra-model depth, the structure's slant
     casters, the Classic silhouette shadow through the stencil, and the bodies.

   Every function takes what it reads as arguments; nothing here holds a scene. It throws on a GL error rather
   than reporting one, and its caller says it. A classic script: it defines the global TasceneRender. */
(function (root) {
"use strict";

// ---- the engine's own integer arithmetic (tagpu_terr.c / terrain-depth.md 2).
// `sar 5` is a division TOWARD ZERO, and the column count is a ceil — both are
// reproduced deliberately, not cleaned up, because a cell boundary landing one
// pixel differently is exactly what the parity diff exists to catch.
const div32trunc = (v) => (v + (v < 0 ? 31 : 0)) >> 5;
const TILE_PX = 32;
const UNIT_STRIDE = 7;             // the mesh .bin's own stride: x,y,z,u,v,fc,ck
const ceil32 = (v) => { const q = div32trunc(v); return (v - q * 32) ? q + 1 : q; };
// the unit vertex: x,y,enc, u,v, fc,ck, shade, wx,wz, vy, nx,ny,nz, WX,WY,WZ
const UVST = 17;
// the feature vertex: x,y,enc, u,v, ck,mode, wx,wz (the lab's lane adds its lambert)
const SVST = 9;
// tagpu_render3do.c: the model-space toward-camera axis and the light
const SH_V = [0.0, 0.8944, -0.4472], SH_L = [-0.35, 0.80, -0.49];

// ---- GL helpers -----------------------------------------------------------
function shader(gl, type, src, label) {
  const sh = gl.createShader(type);
  gl.shaderSource(sh, src); gl.compileShader(sh);
  if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS))
    throw new Error(`${label}: ${gl.getShaderInfoLog(sh)}\n---\n` +
                    src.split("\n").map((l, i) => `${i + 1}: ${l}`).join("\n"));
  return sh;
}
function program(gl, vs, fs, label) {
  const p = gl.createProgram();
  gl.attachShader(p, shader(gl, gl.VERTEX_SHADER, vs, label + ".vert"));
  gl.attachShader(p, shader(gl, gl.FRAGMENT_SHADER, fs, label + ".frag"));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS))
    throw new Error(`${label}: ${gl.getProgramInfoLog(p)}`);
  return p;
}
// tagpu's terrain and unit shaders carry the Classic++ shadow map's two
// samplers since G14i (tagpu_glsl.h), even in the Classic lane where uShadowOn
// stays 0: two sampler TYPES left on unit 0 make GL drop the whole draw, so
// their units are named at link, exactly as tagpu_shadow.c names them
function nameShadowUnits(gl, prog) {
  gl.useProgram(prog);
  const c = gl.getUniformLocation(prog, "uShadowCmp"), r = gl.getUniformLocation(prog, "uShadowRaw");
  if (c) gl.uniform1i(c, 12);
  if (r) gl.uniform1i(r, 13);
}
function uniforms(gl, prog, names) {
  const U = {};
  for (const n of names) U[n] = gl.getUniformLocation(prog, n);
  return U;
}
function tex2D(gl, unit, internal, w, h, format, type, data, filter) {
  const t = gl.createTexture();
  gl.activeTexture(gl.TEXTURE0 + unit);
  gl.bindTexture(gl.TEXTURE_2D, t);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, internal, w, h, 0, format, type, data);
  const f = filter || gl.NEAREST;
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, f);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, f);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  return t;
}

// CLASSIC IS tagpu's ONE FULL-COLOUR PATH with assets=0 light=0
// (uRestored 0, uLambert 0), so it samples what the game's Classic samples:
// the BASE ATLAS -- every index expanded to its palette colour, alpha 0 at a
// sprite frame's own key, the game's tagpu_pal_expand over its key plane.
// `rects` are the frames with their keys, `pad` the replicated border each
// one carries; the terrain has no key and passes none. Texels outside every
// frame stay (0,0,0,0), as the game's zeroed mirror leaves them. `pal` is the
// pack's 256 RGBA palette.
function baseAtlas(pal, r8, w, h, rects, pad) {
  const out = new Uint8Array(w * h * 4);
  const put = (i) => {
    const p = r8[i] * 4;
    out[i * 4] = pal[p]; out[i * 4 + 1] = pal[p + 1];
    out[i * 4 + 2] = pal[p + 2]; out[i * 4 + 3] = 255;
  };
  if (!rects) { for (let i = 0; i < w * h; i++) put(i); return out; }
  for (const r of rects) {
    const x0 = Math.max(0, r.x - pad), y0 = Math.max(0, r.y - pad);
    const x1 = Math.min(w, r.x + r.w + pad), y1 = Math.min(h, r.y + r.h + pad);
    for (let y = y0; y < y1; y++)
      for (let x = x0; x < x1; x++)
        if (r8[y * w + x] !== r.key) put(y * w + x);
  }
  return out;
}
// THE FACE-SHADE MULTIPLIER, tagpu_render3do.c's `s_shadeK` in the same
// order of operations: per SHD row, the least-squares slope through the
// origin of the shaded colour against the unshaded one over every channel
// the row does not clip (>= 250), normalised so the neutral row is 1.0
function shadeK(pal, shd, rows, neutral) {
  const raw = new Float64Array(32), k = new Float32Array(32);
  for (let r = 0; r < 32; r++) {
    let sxy = 0, sxx = 0;
    if (r < rows)
      for (let i = 0; i < 256; i++) {
        const y = shd[r * 256 + i] * 4;
        for (let c = 0; c < 3; c++) {
          if (pal[y + c] >= 250) continue;
          sxy += pal[i * 4 + c] * pal[y + c];
          sxx += pal[i * 4 + c] * pal[i * 4 + c];
        }
      }
    raw[r] = sxx > 0 ? sxy / sxx : 1;
  }
  const n = raw[neutral] > 0 ? raw[neutral] : 1;
  for (let r = 0; r < 32; r++) k[r] = raw[r] / n;
  return k;
}

// ---- terrain: tagpu's own shaders and the 32-px grid. tagpu_terr.c's
// vertex shader is INSTANCED: a static unit quad (TAGPU_TERR_QUAD, the
// engine's vertex order, the shared edge on (1,0)-(0,1)) at location 0, and
// per instance the cell's grid column and row and its tile's atlas column and
// row as four unnormalised shorts at location 1 (TAGPU_TERR_ICOMP). The
// shader rebuilds the position, world point and UVs from those and uOrigin,
// uTile0, uTexel — exactly, every term an integer far below 2^24.
//
// `atlas` is the pack's map.atlas (w, h, cols, tiles) and `r8` its indices; the
// base atlas goes to texture unit 16, the program's sampler for it.
function classicTerrain(gl, vs, fs, atlas, r8, pal) {
  const prog = program(gl, vs, fs, "terrain");
  const vao = gl.createVertexArray();
  gl.bindVertexArray(vao);
  const quad = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, quad);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1]),
                gl.STATIC_DRAW);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, 0);
  const inst = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, inst);
  gl.enableVertexAttribArray(1);
  gl.vertexAttribPointer(1, 4, gl.SHORT, false, 8, 0);   // SSCALED, as the game's
  gl.vertexAttribDivisor(1, 1);
  gl.bindVertexArray(null);
  const U = uniforms(gl, prog, ["uGame", "uZoom", "uZoomC", "uDepthScale", "uEnc",
                                "uPal", "uFogGrid", "uFogOrg",
                                "uFogDim", "uFog", "uOrigin", "uTile0", "uTexel",
                                "uLambert", "uRestored", "uBase", "uHDim",
                                "uShadowOn"]);
  nameShadowUnits(gl, prog);
  tex2D(gl, 16, gl.RGBA8, atlas.w, atlas.h, gl.RGBA, gl.UNSIGNED_BYTE,
        baseAtlas(pal, r8, atlas.w, atlas.h, null, 0));
  gl.activeTexture(gl.TEXTURE0);        // tex2D leaves its own unit selected
  let cells = new Int16Array(4 * 4096);
  return {
    prog, vao, U,
    // tagpu_terr.c's gather, line for line: one instance per visible cell.
    // `tm` is the pack's map.tilemap (w, h) and `tiles` its u16 indices; E and W
    // the swept world rect, (vpL, vpT) the viewport's game-pixel corner.
    gather(tiles, tm, E, W, vpL, vpT) {
      const stride = tm.w, mrows = tm.h;
      const tx0 = div32trunc(E[0]), fx = E[0] - tx0 * 32;
      const ty0 = div32trunc(E[1]), fy = E[1] - ty0 * 32;
      const cols = ceil32(W[0] + fx), rows = ceil32(W[1] + fy);
      if (cells.length < cols * rows * 4) cells = new Int16Array(cols * rows * 4);
      let n = 0, skipped = 0, junk = 0;
      for (let r = 0; r < rows; r++) {
        const my = ty0 + r;
        if (my < 0 || my >= mrows) { skipped += cols; continue; }
        for (let c = 0; c < cols; c++) {
          const mx = tx0 + c;
          if (mx < 0 || mx >= stride) { skipped++; continue; }
          const idx = tiles[my * stride + mx];
          if (idx >= atlas.tiles) { junk++; continue; }
          // the quad still spans exactly TILE_PX texels, and its far edge lands
          // ON the guard column, which is a copy of the last real one
          const o = n * 4;
          cells[o] = c; cells[o + 1] = r;
          cells[o + 2] = idx % atlas.cols; cells[o + 3] = (idx / atlas.cols) | 0;
          n++;
        }
      }
      gl.bindBuffer(gl.ARRAY_BUFFER, inst);
      gl.bufferData(gl.ARRAY_BUFFER, cells.subarray(0, n * 4), gl.DYNAMIC_DRAW);
      gl.useProgram(prog);
      // grid cell (0,0)'s screen corner, the map cell it is, the atlas texel
      gl.uniform2f(U.uOrigin, vpL - fx, vpT - fy);
      gl.uniform2f(U.uTile0, tx0, ty0);
      gl.uniform2f(U.uTexel, 1 / atlas.w, 1 / atlas.h);
      return { n, stats: { cols, rows, tx0, ty0, fx, fy, cells: n, skipped, junk } };
    },
    // The draw, unlit: no height grid, so no lambert term. `f` is the frame:
    // GW, GH (the game frame), zoom, zoomC, depth (the pack's), and the fog
    // grid's uniforms (fog: 0 off, else uFog's bits; fogOrg, fogDim), its RG8
    // texture on unit 2.
    draw(f, n) {
      gl.useProgram(prog);
      gl.uniform2f(U.uGame, f.GW, f.GH);
      gl.uniform1f(U.uZoom, f.zoom);
      gl.uniform2f(U.uZoomC, f.zoomC[0], f.zoomC[1]);
      gl.uniform1f(U.uDepthScale, f.depth.scale);
      gl.uniform1f(U.uEnc, f.depth.terrain);
      gl.uniform1i(U.uPal, 1);
      gl.uniform1i(U.uFogGrid, 2);
      gl.uniform2f(U.uFogOrg, f.fogOrg[0], f.fogOrg[1]); gl.uniform2f(U.uFogDim, f.fogDim[0], f.fogDim[1]);
      gl.uniform1i(U.uFog, f.fog);
      gl.uniform1i(U.uLambert, 0);
      gl.uniform1i(U.uRestored, 0); gl.uniform1i(U.uBase, 16);
      gl.uniform2f(U.uHDim, 0, 0); gl.uniform1i(U.uShadowOn, 0);
      gl.bindVertexArray(vao);
      gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, n);
      gl.bindVertexArray(null);
    },
  };
}

// ---- features: tagpu_feat.c's gather -----------------------------------------
// a cell past the map reflects across every edge it is past, as a triangle
// wave, so a view wider than the map still lands on real tiles, and a cell two
// maps out is the map again the right way round.
const reflect = (m, n) => {
  const p = ((m % (2 * n)) + 2 * n) % (2 * n);
  return p < n ? [p, false] : [2 * n - 1 - p, true];
};

// tagpu_feat.c's gather: the engine's own sweep rect, its projection with
// the viewport's corner (the engine's +128/+32) baked in, and the depth keys
// the painter's order implies. Shadows and bodies go to separate buckets
// because the shadow is drawn with depth writes OFF (a shadow occludes
// nothing) — G13a's rule.
//
// The exploration lane keeps all of that and changes only the DEPTH KEYS,
// because it has to: with a per-row heightfield under it, the parity lane's
// global 0.40-0.50 band for flat features would sit behind the ground. `p`:
//   lab, mirror        the lane, and the mirror past the map's edge
//   r0, E, W, vpL, vpT the depth keys' row base, the swept rect, the viewport
//   w16, h16, heights  the map's 16-px cells and its height bytes
//   anchors, mapAnchors cell -> def index, all of them and the map's own
//   defs, atlas        the pack's feature defs and atlas (w, h)
//   D, labD            the pack's depth keys and the lab's
//   lambertAt(col,row) the lab's ground lambert at an anchor
// Returns { shadow, body, mshadow, mbody } (vertex arrays) or null when the
// sweep is empty.
function gatherFeatures(p) {
  const lab = p.lab, D = p.D, labD = p.labD, defs = p.defs, E = p.E, W = p.W;
  const w16 = p.w16, h16 = p.h16, heightBuf = p.heights, vpL = p.vpL, vpT = p.vpT;
  const mirror = p.mirror;
  let r0 = p.r0, nRows = (W[1] >> 4) + 32;
  let c0 = (E[0] >> 4) - 10, nCols = (W[0] >> 4) + 12;
  if (!mirror) {
    if (r0 < 0) { nRows += r0; r0 = 0; }
    if (r0 + nRows > h16 - 1) nRows = h16 - r0 - 1;
    if (c0 < 0) { nCols += c0; c0 = 0; }
    if (c0 + nCols > w16 - 1) nCols = w16 - c0 - 1;
  }
  if (nRows <= 0 || nCols <= 0) return null;
  const flatSpan = nRows * nCols;
  const iw = 1 / p.atlas.w, ih = 1 / p.atlas.h;
  const shadow = [], body = [];
  const quad = (into, rect, sx, sy, enc, mode, wax, waz, lam) => {
    const x0 = sx - rect.hx, y0 = sy - rect.hy;
    const x1 = x0 + rect.w,  y1 = y0 + rect.h;
    const u0 = rect.x * iw,  u1 = (rect.x + rect.w) * iw;
    const v0 = rect.y * ih,  v1 = (rect.y + rect.h) * ih;
    const ck = rect.ck / 255;
    for (const [x, y, u, v] of [[x0, y0, u0, v0], [x1, y0, u1, v0], [x0, y1, u0, v1],
                                [x1, y0, u1, v0], [x1, y1, u1, v1], [x0, y1, u0, v1]]) {
      into.push(x, y, enc, u, v, ck, mode, wax, waz);
      if (lab) into.push(lam);
    }
  };
  // the mirrored ones: the sprite flipped with the terrain on an x
  // reflection and never on a y one (a tree stands up either way), and slot
  // 3 the point each corner is DRAWN at on the tile grid, for the tone
  const mshadow = [], mbody = [];
  const mquad = (into, rect, sx, sy, enc, mode, flip) => {
    const x0 = sx - (flip ? rect.w - rect.hx : rect.hx), y0 = sy - rect.hy;
    const x1 = x0 + rect.w, y1 = y0 + rect.h;
    let u0 = rect.x * iw, u1 = (rect.x + rect.w) * iw;
    if (flip) [u0, u1] = [u1, u0];
    const v0 = rect.y * ih, v1 = (rect.y + rect.h) * ih;
    const ck = rect.ck / 255;
    for (const [x, y, u, v] of [[x0, y0, u0, v0], [x1, y0, u1, v0], [x0, y1, u0, v1],
                                [x1, y0, u1, v0], [x1, y1, u1, v1], [x0, y1, u0, v1]])
      into.push(x, y, enc, u, v, ck, mode, x - vpL + E[0], y - vpT + E[1]);
  };
  for (let row = r0; row < r0 + nRows; row++) {
    const offRow = row < 0 || row >= h16;
    const [srow, flipY] = offRow ? reflect(row, h16) : [row, false];
    for (let col = c0; col < c0 + nCols; col++) {
      const off = offRow || col < 0 || col >= w16;
      const [scol, flipX] = off ? reflect(col, w16) : [col, false];
      // the engine's sweep never reaches the last row or column (the clamps
      // above), so nothing anchored there is drawn — nor mirrored
      if (srow >= h16 - 1 || scol >= w16 - 1) continue;
      // off the map, the MAP's anchor at the cell this one mirrors
      const idx = off ? p.mapAnchors[srow * w16 + scol] : p.anchors[row * w16 + col];
      if (idx === undefined) continue;
      const d = defs[idx];
      if (!d) continue;
      const h00 = heightBuf[srow * w16 + scol];
      const h01 = (scol + 1 < w16) ? heightBuf[srow * w16 + scol + 1] : h00;
      let h10 = h00, h11 = h01;
      if (srow + 1 < h16) {
        h10 = heightBuf[(srow + 1) * w16 + scol];
        h11 = (scol + 1 < w16) ? heightBuf[(srow + 1) * w16 + scol + 1] : h10;
      }
      // the engine's projection: screenY is worldZ minus half the height.
      // A reflection reverses the cells a footprint spans, so a mirrored
      // feature ENDS on the cell its source begins on. The mirrored terrain
      // is the tile ART reflected, and the art has the height baked in, so a
      // y reflection mirrors where the source is DRAWN: the half height is
      // added, not taken off (taken off, a tree on a hill lands the hill's
      // height short of the art it stood on)
      const hx = (d.footx * 16) / 2 | 0, hz = (d.footz * 16) / 2 | 0;
      const lift = (h00 + h01 + h10 + h11) >> 3;
      const wax = flipX ? (col + 1) * 16 - hx : col * 16 + hx;
      const waz = flipY ? (row + 1) * 16 - hz + lift : row * 16 + hz - lift;
      const sx = wax + vpL - E[0], sy = waz + vpT - E[1];
      const rel0 = row - r0;
      let enc;
      if (d.flat) {
        enc = lab ? labD.flat + rel0 * D.rowStep
                  : D.flat0 + D.flatSpan * (rel0 * nCols + (col - c0)) / flatSpan;
      } else {
        let rel = rel0;
        if (rel < 0) rel = 0;
        if (rel > nRows + 8) rel = nRows + 8;
        // the column term breaks ties inside a row. The lab narrows it so a
        // tall feature cannot reach past the NEXT row's ground (rowStep-0.1).
        enc = D.tall0 + rel * D.rowStep +
              (lab ? labD.tallCol : 1.5) * ((col - c0) / nCols);
      }
      const shEnc = enc - (d.flat ? D.shadowFlat : D.shadowTall);
      if (off) {
        if (d.shadow) mquad(mshadow, d.shadow, sx, sy, shEnc, d.shadtrans ? 2 : 1, flipX);
        if (d.body) mquad(mbody, d.body, sx, sy, enc, d.animtrans ? 2 : 1, flipX);
        continue;
      }
      const lam = lab ? p.lambertAt(col, row) : 1;
      if (d.shadow)
        quad(shadow, d.shadow, sx, sy, shEnc, d.shadtrans ? 2 : 1, wax, waz, lam);
      if (d.body)
        quad(body, d.body, sx, sy, enc, d.animtrans ? 2 : 1, wax, waz, lam);
    }
  }
  return { shadow, body, mshadow, mbody };
}

// The Classic sprite program over `vbo` (SVST floats a vertex), and the
// feature base atlas on unit 17 from the R8 atlas on unit 5 (which the caller
// uploads). `feat` is the pack's features (atlas, defs).
function classicSprites(gl, vs, fs, vbo, feat, r8, pal) {
  const prog = program(gl, vs, fs, "sprite");
  const vao = gl.createVertexArray();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  const sizes = [3, 2, 2, 2], offs = [0, 12, 20, 28];
  for (let i = 0; i < 4; i++) {
    gl.enableVertexAttribArray(i);
    gl.vertexAttribPointer(i, sizes[i], gl.FLOAT, false, SVST * 4, offs[i]);
  }
  gl.bindVertexArray(null);
  const U = uniforms(gl, prog, ["uGame", "uZoom", "uZoomC", "uDepthScale",
                                "uFogGrid", "uFogOrg", "uFogDim", "uFog",
                                "uRestored", "uBase"]);
  const rects = [];
  for (const d of Object.values(feat.defs))
    for (const r of [d.body, d.shadow]) if (r) rects.push({ ...r, key: r.ck });
  tex2D(gl, 17, gl.RGBA8, feat.atlas.w, feat.atlas.h, gl.RGBA,
        gl.UNSIGNED_BYTE, baseAtlas(pal, r8, feat.atlas.w, feat.atlas.h, rects,
                                    feat.atlas.pad || 0));
  gl.activeTexture(gl.TEXTURE0);
  return {
    prog, vao, U,
    // the uniforms of a draw; `f` as classicTerrain's draw takes it
    use(f) {
      gl.useProgram(prog);
      gl.uniform2f(U.uGame, f.GW, f.GH);
      gl.uniform1f(U.uZoom, f.zoom);
      gl.uniform2f(U.uZoomC, f.zoomC[0], f.zoomC[1]);
      gl.uniform1f(U.uDepthScale, f.depth.scale);
      gl.uniform1i(U.uFogGrid, 2);
      gl.uniform2f(U.uFogOrg, f.fogOrg[0], f.fogOrg[1]); gl.uniform2f(U.uFogDim, f.fogDim[0], f.fogDim[1]);
      gl.uniform1i(U.uFog, f.fog);
      gl.uniform1i(U.uRestored, 0);
      gl.uniform1i(U.uBase, 17);
      // aLam, the anchor's ground lambert, is not in the parity vertex: the
      // generic value stands in, and light=0 bakes exactly 1.0 there
      gl.vertexAttrib1f(4, 1.0);
    },
    // shadows first with no depth writes, then the bodies
    draw(nShadow, nBody, bodyBase) {
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);   // the shader premultiplies
      gl.bindVertexArray(vao);
      if (nShadow) { gl.depthMask(false); gl.drawArrays(gl.TRIANGLES, 0, nShadow); }
      if (nBody)   { gl.depthMask(true);  gl.drawArrays(gl.TRIANGLES, bodyBase, nBody); }
      gl.bindVertexArray(null);
      gl.disable(gl.BLEND);
    },
  };
}

// ---- units: tagpu_native.c's shaders, tagpu_render3do.c's geometry rule ------

// The unit vertex array over a fresh buffer: slots 6 (normal) and 7 (world) are
// only the lab programs'; the parity program leaves them enabled and unread,
// which GL allows as long as the buffer covers them
function unitArray(gl) {
  const vao = gl.createVertexArray(), vbo = gl.createBuffer();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  const sizes = [3, 2, 2, 1, 2, 1, 3, 3], offs = [0, 12, 20, 28, 32, 40, 44, 56];
  for (let i = 0; i < 8; i++) {
    gl.enableVertexAttribArray(i);
    gl.vertexAttribPointer(i, sizes[i], gl.FLOAT, false, UVST * 4, offs[i]);
  }
  gl.bindVertexArray(null);
  return { vao, vbo };
}

// One unit instance per entry of `p.instances`: yaw the bind pose, project
// with the engine's rule, and take the shade row from the POSED normal —
// rotating before shading is what makes the row the same one the engine's own
// pass would pick.
//
// In the exploration lane slot 3 is left at 1.0 and slot 6 carries the same
// POSED face normal, so LAB_UNIT_FS can light every pixel by (by default)
// the same light with the 32-row quantisation taken out. That is the whole
// "free lighting" half of the undithered decision.
//
// `p`: instances (type, pos, facing in degrees or yaw [cos, sin], shadow, slant,
// agl or alt), meshData and slantData by type, meshTop (a cache of each type's
// height), out (a Float32Array of UVST floats a vertex), D (the pack's depth
// keys), r0 and W (the depth rows' base and the swept size), E, vpL, vpT,
// w16, h16, heights, neutral and dir (the SHD calibration). `L` is the look:
// lane, shadowSun, shadowlen, airshadow. Sets L.unitRanges, each with its
// instance, and returns { n, nSlant }.
function emitUnits(p, L) {
  const lab = L.lane === "explore";
  const D = p.D, E = p.E, W = p.W, w16 = p.w16, h16 = p.h16, heightBuf = p.heights;
  const vpL = p.vpL, vpT = p.vpT, uVerts = p.out, meshTop = p.meshTop;
  const SH_NEUTRAL = p.neutral, SH_DIR = p.dir;
  let r0 = p.r0, nRows = (W[1] >> 4) + 32;
  if (r0 < 0) { nRows += r0; r0 = 0; }
  let n = 0, nAir = 0, nSlant = 0;
  L.unitRanges = [];
  // shadowlen: the shadow's length is a + b*height instead of the parallel
  // light's height/tan(el), done as a vertical scale on the caster in the
  // shadow map. sv = height ONLY along the shadow sun's ground direction
  const cot = Math.hypot(L.shadowSun[0], L.shadowSun[2]) / Math.max(L.shadowSun[1], 1e-3);
  for (const inst of p.instances) {
    const mesh = p.meshData[inst.type];
    if (!mesh) continue;
    if (meshTop[inst.type] === undefined) {
      let top = 0;
      for (let i = 1; i < mesh.length; i += UNIT_STRIDE) if (mesh[i] > top) top = mesh[i];
      meshTop[inst.type] = top;
    }
    const top = meshTop[inst.type];
    const sv = L.shadowlen && top > 0 && cot > 1e-3
             ? (L.shadowlen[0] + L.shadowlen[1] * top) / (top * cot) : 1;
    // scenario facing 0 is TA's default build heading 0x8000, i.e. half a turn.
    // A `yaw` given as [cos, sin] is used as it is: the wiki's facing vector
    // (hx, hz) is the yaw (-hz, hx), the model's +z turned onto (hx, hz)
    let cs, sn;
    if (inst.yaw) { cs = inst.yaw[0]; sn = inst.yaw[1]; }
    else {
      const yaw = (inst.facing + 180) * Math.PI / 180;
      cs = Math.cos(yaw); sn = Math.sin(yaw);
    }
    const wx = inst.pos[0], wz = inst.pos[1];
    const col = Math.min(w16 - 1, Math.max(0, wx >> 4));
    const row = Math.min(h16 - 1, Math.max(0, wz >> 4));
    const gnd = heightBuf[row * w16 + col];         // the engine snaps to terrain
    // ALTITUDE. A ground unit is on the terrain, as the engine puts it; an
    // aircraft is `agl` above it -- the FBI's CruiseAlt, carried in the pack
    // (tascene `unit_facts`). A scenario `height` is an ABSOLUTE altitude, so
    // it is measured from the ground the lab actually has under the unit.
    const agl = Math.max(0, inst.alt !== undefined ? inst.alt - gnd
                                                   : (inst.agl || 0));
    const wy = gnd + agl;
    const hotX = wx - E[0] + vpL;
    const hotY = wz - (wy >> 1) - E[1] + vpT;
    // the SHADOW is at the GROUND under the unit, so it trails the body down
    // the screen by half the altitude -- 0 for anything on the ground
    // (0x459228: bodyY uses altitude>>1, shadowY GetPosHeight()>>1)
    const shadowDy = (wy >> 1) - (gnd >> 1);
    // CLASSIC++ AIRBORNE CASTER (renderers.md §2.2). At the shadow sun's 40°
    // a plane's shadow lands about 1.2x its altitude up-right — the lab
    // throws a Thunder's clean off the frame — so `airshadow` picks one of:
    //   len       shadowlen's own rule extended to the ALTITUDE, so the throw
    //             is a + b*(agl + model height) rather than (agl + h)*cot el:
    //             67 world units for a Thunder at 200 instead of 250
    //   physical  cast from the true altitude and live with it
    //   drop      do not cast at all — draw the Classic silhouette instead,
    //             which is what the engine itself does
    const svAir = agl > 0 && L.shadowlen && cot > 1e-3
                ? (L.shadowlen[0] + L.shadowlen[1] * (agl + top)) / ((agl + top) * cot)
                : sv;
    const airLen = agl > 0 && L.airshadow === "len";
    const shAgl = airLen ? agl * svAir : agl;    // `drop` never reaches the map,
    const shSv  = airLen ? svAir : sv;           // so it keeps the true position
                                                 // and its own lookup reads lit
    let rel = row - r0;
    if (rel < 0) rel = 0;
    if (rel > nRows + 8) rel = nRows + 8;
    // AIRBORNE UNITS ARE A BAND ABOVE EVERY ROW, which is what the engine's
    // second DrawUnit sweep amounts to: site B 0x469BA3 runs over ALL rows
    // with no depth sorting, after the ground units, the features, the
    // weapons and the explosions, so an aircraft and its shadow composite on
    // top of the lot (shadows-cloak.md 4). Within the band they are keyed by
    // the order they appear in, not by row -- site B has no sorting either,
    // so the later one wins -- and a whole rowStep apart, because the
    // intra-model term below spans +-1.8 and would otherwise interleave two
    // aircraft face by face. A row-keyed air band would not fit: the top row
    // is already ~355 of the 512 the depth scale allows.
    // CAPPED: past the depth scale the shared VS clamps `1 - z/scale` to 0 and
    // every aircraft after that pins to the near plane and silently stops
    // sorting. The band starts above the top row (~355 of 512 on a 768-tall
    // view), so there is room for a few dozen; beyond that they share the last
    // key and interleave, which is wrong but visible rather than mysterious.
    const airMax = Math.floor((D.scale - D.unit0) / D.rowStep) - 1;
    const enc = agl > 0
      ? D.unit0 + Math.min(nRows + 12 + nAir++, airMax) * D.rowStep
      : D.unit0 + rel * D.rowStep;
    const wldX = wx, wldZ = wz - (wy >> 1);
    const P = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
    const first = n;
    for (let i = 0; i + 3 * UNIT_STRIDE <= mesh.length; i += 3 * UNIT_STRIDE) {
      for (let t2 = 0; t2 < 3; t2++) {
        const o = i + t2 * UNIT_STRIDE;
        const x = mesh[o], y = mesh[o + 1], z = mesh[o + 2];
        P[t2][0] = x * cs + z * sn;                 // yaw about the up axis
        P[t2][1] = y;
        P[t2][2] = -x * sn + z * cs;
      }
      // per-face flat shading, tagpu_render3do.c line for line
      let shade = lab ? 1.0 : SH_NEUTRAL / 31;
      let ux = 0, uy = 1, uz = 0;               // the lab's normal; level if degenerate
      const e1 = [P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]];
      const e2 = [P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]];
      let nx = e1[1] * e2[2] - e1[2] * e2[1];
      let ny = e1[2] * e2[0] - e1[0] * e2[2];
      let nz = e1[0] * e2[1] - e1[1] * e2[0];
      if (nx * SH_V[0] + ny * SH_V[1] + nz * SH_V[2] < 0) { nx = -nx; ny = -ny; nz = -nz; }
      const nl = Math.hypot(nx, ny, nz);
      if (nl > 1e-6) {
        if (lab) {
          // lit per pixel in LAB_UNIT_FS; the face only supplies its normal,
          // in MAP space (z south), where the terrain's already is and the
          // shadow falls — 3DO z points north, hence the flip. Flat, as a
          // 3DO is — a smooth model would put a different one on each
          // vertex right here and nothing downstream changes
          ux = nx / nl; uy = ny / nl; uz = -nz / nl;
        } else {
          const I = (nx * SH_L[0] + ny * SH_L[1] + nz * SH_L[2]) / nl;
          let rr = SH_NEUTRAL + SH_DIR * Math.floor(I * 12 + 0.5);
          rr = Math.max(0, Math.min(31, rr));
          shade = rr / 31;
        }
      }
      for (let t2 = 0; t2 < 3; t2++) {
        const o = i + t2 * UNIT_STRIDE, q = n * UVST;
        const x = P[t2][0], y = P[t2][1], z = P[t2][2];
        // Intra-model depth. Without it every face of a unit shares one key
        // and GL_LESS keeps whichever drew first — which is how the
        // commander's yellow GLOW panels drew over its own torso. The engine
        // squeezes this into the +-2 gap between row keys.
        let md = (2 * y - z) / 256;
        if (md > 1.8) md = 1.8; else if (md < -1.8) md = -1.8;
        uVerts[q]     = x + hotX;
        uVerts[q + 1] = (-z - y * 0.5) + hotY;
        uVerts[q + 2] = enc + md;
        uVerts[q + 3] = mesh[o + 3]; uVerts[q + 4] = mesh[o + 4];
        uVerts[q + 5] = mesh[o + 5]; uVerts[q + 6] = mesh[o + 6];
        uVerts[q + 7] = shade;
        uVerts[q + 8] = wldX + x;                 // per-vertex, as the engine
        uVerts[q + 9] = wldZ + (-z - y * 0.5);
        uVerts[q + 10] = y;
        uVerts[q + 11] = ux; uVerts[q + 12] = uy; uVerts[q + 13] = uz;
        // the SHADOW position: real x/z, height scaled by sv. The depth pass
        // casts from it and LAB_UNIT_FS looks its own shadow up at it, so a
        // unit is consistent with itself; only the ground sees the real world
        uVerts[q + 14] = wx + x; uVerts[q + 15] = gnd + shAgl + shSv * y;
        uVerts[q + 16] = wz - z;
        n++;
      }
    }
    // THE STRUCTURE SHADOW: the engine's cached slant projection
    // (shadows-cloak.md 3, 0x45A790), a second range after the body's,
    // drawn in its place by silhouettes(). The casters are the pack's
    // (tascene: every face of every visible, cached piece, the selection
    // quad alone skipped); each vertex lands at (x + y/4, -z - y/4) from
    // the posed coordinates snapped to whole units the engine's way (the
    // 16.16 high word of x, y and -z, then an arithmetic shift), flat, so
    // the unit shader takes the flat path and never samples the atlas --
    // the engine flat-fills, and a texture key punches no hole. The same
    // rule as tagpu_native.c's emit_slant, which this lane mirrors.
    const slantFirst = n;
    const cast = p.slantData[inst.type];
    if (inst.slant && inst.shadow && cast && !lab) {
      for (let i = 0; i + 3 <= cast.length; i += 3) {
        const x0 = cast[i], y = cast[i + 1], z0 = cast[i + 2];
        const x = x0 * cs + z0 * sn, z = -x0 * sn + z0 * cs;
        const xi = Math.floor(x), yi = Math.floor(y), nzi = Math.floor(-z);
        const q = yi >> 2;
        const px = xi + q, py = nzi - q;
        let md = (2 * y - z) / 256;
        if (md > 1.8) md = 1.8; else if (md < -1.8) md = -1.8;
        const o = n * UVST;
        uVerts[o]     = px + hotX;
        uVerts[o + 1] = py + hotY;
        uVerts[o + 2] = enc + md;
        uVerts[o + 3] = -1; uVerts[o + 4] = -1;
        uVerts[o + 5] = 0;  uVerts[o + 6] = -1;
        uVerts[o + 7] = SH_NEUTRAL / 31;
        uVerts[o + 8] = wldX + px;
        uVerts[o + 9] = wldZ + py;
        uVerts[o + 10] = y;
        uVerts[o + 11] = 0; uVerts[o + 12] = 1; uVerts[o + 13] = 0;
        uVerts[o + 14] = wx + x; uVerts[o + 15] = gnd + y; uVerts[o + 16] = wz - z;
        n++; nSlant++;
      }
    }
    // the engine draws a unit's shadow inside that unit's own blit, so the
    // silhouette pass needs each unit's own vertex range, offset and gate:
    // a mobile unit's is its body range, a structure's the slant range
    if (n > first)
      L.unitRanges.push({ first, count: slantFirst - first, air: agl > 0,
                          shadow: !!inst.shadow, dy: shadowDy,
                          slant: !!inst.slant,
                          slantFirst, slantCount: n - slantFirst, inst,
                          hot: [hotX, hotY], enc, yaw: [cs, sn] });
  }
  return { n, nSlant };
}

// The Classic unit program, its base atlas on unit 18 (from the R8 atlas on
// unit 6, which the caller uploads) and the shade multipliers on unit 19.
// `units` is the pack's units (atlas), `pal` and `shd` the palette and SHD
// table, `rows` and `neutral` the SHD calibration.
function classicUnits(gl, vs, fs, units, r8, pal, shd, rows, neutral) {
  const prog = program(gl, vs, fs, "unit");
  const U = uniforms(gl, prog, ["uGame", "uOffset", "uZoom", "uZoomC", "uDepthScale",
                                "uPal", "uShadow", "uAlpha",
                                "uWaterT", "uWaterMode", "uDigT",
                                "uScaf", "uScafOn", "uScafP", "uSS", "uZoomF",
                                "uZoomCF", "uFogGrid", "uFogOrg",
                                "uFogDim", "uFog", "uLambert", "uRestored",
                                "uBase", "uShadeK", "uSun", "uAmb", "uNorm",
                                "uShadowOn", "uNanoOn", "uNanoT", "uNanoC"]);
  nameShadowUnits(gl, prog);
  const pad = units.atlas.pad || 0;
  const rects = Object.values(units.atlas.frames || {});
  tex2D(gl, 18, gl.RGBA8, units.atlas.w, units.atlas.h, gl.RGBA,
        gl.UNSIGNED_BYTE, baseAtlas(pal, r8, units.atlas.w, units.atlas.h, rects, pad));
  tex2D(gl, 19, gl.R32F, 32, 1, gl.RED, gl.FLOAT, shadeK(pal, shd, rows, neutral));
  gl.activeTexture(gl.TEXTURE0);
  return {
    prog, U,
    // the CLASSIC unit program's uniforms. `f` as classicTerrain's draw takes
    // it, with the viewport (vpL, vpT, vw, vh) and SS. The lab's Classic++
    // lane borrows them too, when `airshadow=drop` asks it for a Classic
    // silhouette under an aircraft.
    use(f) {
      gl.useProgram(prog);
      gl.uniform2f(U.uGame, f.GW, f.GH);
      gl.uniform2f(U.uOffset, 0, 0);
      gl.uniform1f(U.uZoom, f.zoom);
      gl.uniform2f(U.uZoomC, f.zoomC[0], f.zoomC[1]);
      gl.uniform1f(U.uDepthScale, f.depth.scale);
      gl.uniform1i(U.uPal, 1);
      gl.uniform1i(U.uShadow, 0); gl.uniform1f(U.uAlpha, 1.0);
      // the water/dig tests are `vVY <= threshold`; -1e9 disables both
      gl.uniform1f(U.uWaterT, -1e9); gl.uniform1i(U.uWaterMode, 0);
      gl.uniform1f(U.uDigT, -1e9);
      gl.uniform1i(U.uScaf, 2); gl.uniform1i(U.uScafOn, 0);
      gl.uniform4f(U.uScafP, f.vp[0], f.vp[1], f.vp[2], f.vp[3]);
      gl.uniform1f(U.uSS, f.SS); gl.uniform1f(U.uZoomF, f.zoom);
      gl.uniform2f(U.uZoomCF, f.zoomC[0], f.zoomC[1]);
      gl.uniform1i(U.uFogGrid, 2);
      gl.uniform2f(U.uFogOrg, 0, 0); gl.uniform2f(U.uFogDim, 1, 1);
      gl.uniform1i(U.uFog, 0);
      // light=0: the base, the face-shade multiplier, and a lambert that is
      // exactly 1.0 (ambient 1, no shadow)
      gl.uniform1i(U.uLambert, 0);
      gl.uniform1i(U.uRestored, 0); gl.uniform1i(U.uBase, 18);
      gl.uniform1i(U.uShadeK, 19);
      gl.uniform3f(U.uSun, 0, 1, 0); gl.uniform1f(U.uAmb, 1); gl.uniform1f(U.uNorm, 1);
      gl.uniform1i(U.uShadowOn, 0);
    },
    // THE CLASSIC SILHOUETTE DROP SHADOW, drawn before the bodies as the engine
    // draws it (shadows-cloak.md §3): the unit's own shape filled with palette
    // index 0 and blitted through PALETTE.ALP at (sx+5, groundY) — 5 px right of
    // the body, at the ground under the unit, so an aircraft's trails half its
    // altitude below it.
    //
    // Blended ONCE PER PIXEL through the stencil, not once per surface the view
    // ray crosses: the engine blits ONE blackened copy of the composite, so a
    // pixel the model covers twice is still darkened once — and from above an
    // aircraft is a two-sided shell over its whole area, which is where this
    // showed up in the game. Marking with colour writes off and then blending
    // with the op that ZEROES the mark makes the second fragment on a pixel fail
    // EQUAL 1; both draws see the same depth buffer (writes are off), so they
    // cover the same fragments and leave no mark behind. Clearing per unit keeps
    // two units' shadows stacking the way the engine's two separate blits do.
    // The same scheme as tagpu_native.c, which this lane exists to mirror.
    silhouettes(ranges, want) {
      gl.depthMask(false);
      gl.enable(gl.STENCIL_TEST);
      gl.uniform1i(U.uShadow, 1);
      for (const r of ranges) {
        if (!r.shadow || !want(r)) continue;
        // a structure's shadow is the slant range (emitUnits), a mobile
        // unit's its own body; a structure whose pack carries no casters
        // (built before the slant, or every piece dont-cache) draws none
        const first = r.slant ? r.slantFirst : r.first;
        const count = r.slant ? r.slantCount : r.count;
        if (!count) continue;
        gl.uniform2f(U.uOffset, 5, r.dy);
        gl.stencilFunc(gl.ALWAYS, 1, 0xFF);
        gl.stencilOp(gl.KEEP, gl.KEEP, gl.REPLACE);
        gl.colorMask(false, false, false, false);
        gl.drawArrays(gl.TRIANGLES, first, count);
        gl.stencilFunc(gl.EQUAL, 1, 0xFF);
        gl.stencilOp(gl.KEEP, gl.KEEP, gl.ZERO);
        gl.colorMask(true, true, true, true);
        gl.drawArrays(gl.TRIANGLES, first, count);
      }
      gl.colorMask(true, true, true, true);
      gl.disable(gl.STENCIL_TEST);
      gl.uniform1i(U.uShadow, 0);
      gl.uniform2f(U.uOffset, 0, 0);
    },
  };
}

// ---- the composite: FBO -> canvas, flipped ----------------------------------
// The offscreen holds the game frame with row 0 at the BOTTOM. Canvas NDC +1 is
// the top, so the top of the canvas samples v = 0.
const BLIT_VS = `#version 300 es
precision highp float;
out vec2 vUV;
void main(){
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  vUV = vec2(p.x, 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;

root.TasceneRender = {
  div32trunc, ceil32, TILE_PX, UNIT_STRIDE, UVST, SVST, SH_V, SH_L,
  shader, program, nameShadowUnits, uniforms, tex2D, baseAtlas, shadeK,
  classicTerrain, reflect, gatherFeatures, classicSprites,
  unitArray, emitUnits, classicUnits, BLIT_VS,
};
})(typeof window !== "undefined" ? window : globalThis);
