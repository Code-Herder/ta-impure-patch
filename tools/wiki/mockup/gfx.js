/* gfx.js -- the stage in the game's own graphics (gfx=game): a real 3D render of the stage, as the game will draw it
   (research/notes/ingame-wiki-3d-stage-plan.md, section 9). Each frame it renders one view into an offscreen WebGL2
   target of the map at zoom 1, mw*16*ss by mh*16*ss, with tools/tascene-render.js -- tagpu's own terrain, feature and
   unit shaders, extracted from the C sources -- and hands the image to stage.js, which draws it into the map's rect
   between its op ranges R0 and R2. scene.js turns the view into the records drawn here.

   Everything it reads is in gfx/, which tools/wiki/gfx.py resolves from the player's install and .gitignore keeps out
   of the repository. Without gfx/index.json, or while an entry's pack loads, the stage stays the whitebox and the
   controls strip says so.

   What the image holds, in the world passes' order (tagpu_vk.c `world_records`):
   - the terrain from the stage description's tiles and the features (deposits, vents), both under the engine's fog
     grid (scene.js) -- or neither, when the pack's ground did not resolve: the ground falls back whole, and stage.js
     draws its whitebox ground under an image that is clear there;
   - every unit with a resolved model, its Classic silhouette or slant shadow first, then the bodies, a nanoframe in
     the engine's staging and then its wire; units take no fog. A unit whose script poses it (the frame's `poses`,
     sim.js `poses`) draws that pose, composed by pose.js as the game's stage composes it; any other, and every
     ghost, draws the rest pose with Create()'s hides;
   - the ghosts last, alpha 0.40, depth writes off: the placement ghost, the gesture's sites, the queued builds.
   `complete` is false when anything this frame expected was not drawn; stage.js then draws the whitebox whole. */
window.WikiGfx = (() => {
  const BASE = 'gfx/';
  // scene.js's WikiScene is a script-level const, not a property of window, so it is named bare
  const TR = window.TasceneRender, WS = WikiScene;
  let mode = new URLSearchParams(location.search).get('gfx') === 'game' ? 'game' : 'whitebox';
  const SS = Math.max(1, Math.min(4, parseInt(new URLSearchParams(location.search).get('ss') || '2', 10) || 2));
  // ?layers=terrain,features,units: what the image draws, all three unless named -- tools/wiki/stage3dcheck.py
  // renders the terrain and the features apart to class each pixel of the game's capture
  const LAYERS = (q => q === null ? null : new Set(q.split(',')))(new URLSearchParams(location.search).get('layers'));
  const layer = n => !LAYERS || LAYERS.has(n);
  let index = null, state = 'none';     // none, loading, ready, missing
  const stages = {};                    // pack folder -> a Stage, or a promise of one, or an error

  const bin = async u => { const r = await fetch(u); if (!r.ok) throw new Error(`${u}: ${r.status}`); return new Uint8Array(await r.arrayBuffer()); };
  const f32 = b => new Float32Array(b.buffer, b.byteOffset, b.byteLength >> 2);

  function load() {
    if (state !== 'none') return;
    state = 'loading'; note();
    fetch(BASE + 'index.json').then(r => { if (!r.ok) throw new Error(r.status); return r.json(); })
      .then(j => { index = j; state = 'ready'; note(); })
      .catch(() => { state = 'missing'; note(); });
  }
  const on = () => mode === 'game' && state === 'ready';

  /* ---- the control in the mockup's strip, and its one-line notice when gfx/ is absent ---- */
  function note() {
    const el = document.querySelector('#gfx-note'); if (!el) return;
    el.textContent = mode === 'game' && state === 'missing' ? 'game graphics: run tools/wiki/gfx.py'
      : mode === 'game' && state === 'loading' ? 'loading…' : '';
  }
  function control() {
    const sel = document.querySelector('#gfx'); if (!sel) return;
    sel.value = mode;
    sel.onchange = e => { mode = e.target.value; if (mode === 'game') load(); note(); sync(); };
    note();
  }
  // wiki.js owns the URL; a change here re-runs its sync through a harmless replaceState of the same query
  function sync() {
    const q = new URLSearchParams(location.search);
    if (mode === 'game') q.set('gfx', 'game'); else q.delete('gfx');
    history.replaceState(null, '', '?' + q.toString().replace(/=(?=&|$)/g, ''));
  }
  function url(q) { if (mode === 'game') q.set('gfx', 'game'); if (SS !== 2) q.set('ss', SS); }

  /* ---- one entry's stage: its pack, its own WebGL2 context and target ---- */
  async function open(id) {
    const dir = `${BASE}${id}/`, pack = await fetch(dir + 'scene.json').then(r => { if (!r.ok) throw new Error(r.status); return r.json(); });
    const sh = n => fetch(dir + pack.shaders + n).then(r => r.text());
    const [pal, shd, tv, tf, sv, sf, uv, uf] = await Promise.all([bin(dir + pack.palette.file),
      pack.palette.shd ? bin(dir + pack.palette.shd) : new Uint8Array(32 * 256),
      sh('terrain.vert'), sh('terrain.frag'), sh('sprite.vert'), sh('sprite.frag'), sh('unit.vert'), sh('unit.frag')]);
    const ground = pack.ground.ok && pack.map;
    const T = ground ? await Promise.all([bin(dir + pack.map.atlas.file), bin(dir + pack.map.tilemap.file)]) : null;
    const F = ground && pack.features && pack.features.count
      ? await Promise.all([bin(dir + pack.features.atlas.file), bin(dir + pack.features.instances)]) : null;
    /* EACH TYPE'S ARRAYS: what the rest pose draws (meshes, slants, wires, normals, quads), and, from a pack that
       carries every piece with its piece table (gfx.py, *The pieces*), the whole arrays a script's pose is drawn from
       (`full`). The rest pose is the whole arrays less Create()'s hides (pose.js `rest`). */
    const meshes = {}, slants = {}, wires = {}, normals = {}, quads = {}, full = {}, U = pack.units;
    const units = U.atlas ? await bin(dir + U.atlas.file) : null;
    for (const [type, m] of Object.entries(U.meshes || {})) {
      const [s, w, nv, pc] = await Promise.all([bin(dir + m.slantFile), bin(dir + m.wireFile), bin(dir + m.normalsFile),
        m.pieceFile ? bin(dir + m.pieceFile) : null]);
      for (const [owner, file] of Object.entries(m.files)) {
        const key = `${type}.${owner}`;
        const all = { mesh: f32(await bin(dir + file)), quad: f32(await bin(dir + m.quadFiles[owner])), normals: f32(nv),
                      slant: f32(s), wire: f32(w) };
        if (pc) {
          const ids = new Uint16Array(pc.buffer, pc.byteOffset, pc.byteLength >> 1), nb = m.verts, ns = m.slantVerts;
          Object.assign(all, { bp: ids.subarray(0, nb), sp: ids.subarray(nb, nb + ns), wp: ids.subarray(nb + ns) });
          // the piece arrays index the table and run alongside every array, or the pack is not this file's format
          if (ids.length !== nb + ns + m.wireVerts || all.mesh.length !== 7 * nb || all.quad.length !== 8 * nb ||
              all.normals.length !== 15 * nb || all.slant.length !== 3 * ns || all.wire.length !== 3 * m.wireVerts ||
              !m.pieces || ids.some(i => i >= m.pieces.length))
            throw new Error(`${dir}: ${type}'s piece arrays do not match its mesh`);
          full[key] = all;
        }
        const r = pc ? WikiPose.rest(all, m.pieces) : all;
        meshes[key] = r.mesh; slants[key] = r.slant; wires[key] = r.wire; normals[key] = r.normals; quads[key] = r.quad;
      }
    }
    return new Stage(pack, { pal, shd, tv, tf, sv, sf, uv, uf, T, F, units, meshes, slants, wires, normals, quads, full });
  }

  function Stage(pack, d) {
    const [mw, mh] = pack.cells, GW = mw * 16, GH = mh * 16, FW = GW * SS, FH = GH * SS;
    const canvas = document.createElement('canvas');
    canvas.width = FW; canvas.height = FH;
    const gl = canvas.getContext('webgl2', { antialias: false, alpha: true, premultipliedAlpha: true, preserveDrawingBuffer: true });
    if (!gl) throw new Error('no WebGL2 context');
    const ground = !!d.T;
    // the shared textures: the palette (unit 1) and the fog grid (unit 2, rewritten each frame)
    TR.tex2D(gl, 1, gl.RGBA8, 256, 1, gl.RGBA, gl.UNSIGNED_BYTE, d.pal);
    const fogTex = TR.tex2D(gl, 2, gl.RG8, 1, 1, gl.RG, gl.UNSIGNED_BYTE, new Uint8Array([0, 0]));
    let terr = null, tiles = null, spr = null, sprVBO = null, anchors = [];
    if (ground) {
      TR.tex2D(gl, 0, gl.R8, pack.map.atlas.w, pack.map.atlas.h, gl.RED, gl.UNSIGNED_BYTE, d.T[0]);
      terr = TR.classicTerrain(gl, d.tv, d.tf, pack.map.atlas, d.T[0], d.pal);
      tiles = new Uint16Array(d.T[1].buffer, d.T[1].byteOffset, d.T[1].byteLength >> 1);
      if (d.F) {
        TR.tex2D(gl, 5, gl.R8, pack.features.atlas.w, pack.features.atlas.h, gl.RED, gl.UNSIGNED_BYTE, d.F[0]);
        sprVBO = gl.createBuffer();
        spr = TR.classicSprites(gl, d.sv, d.sf, sprVBO, pack.features, d.F[0], d.pal);
        const inst = new Uint16Array(d.F[1].buffer, d.F[1].byteOffset, d.F[1].byteLength >> 1);
        for (let i = 0; i < inst.length; i += 3) anchors[inst[i + 1] * mw + inst[i]] = inst[i + 2];
      }
    }
    let cu = null, ua = null, wa = null;
    if (d.units) {
      TR.tex2D(gl, 6, gl.R8, pack.units.atlas.w, pack.units.atlas.h, gl.RED, gl.UNSIGNED_BYTE, d.units);
      // the bodies with the quad stream (a textured quad mapped and shaded over its own frame, as the game's posed
      // pass does); the wires without it
      ua = TR.unitArray(gl, true); wa = TR.unitArray(gl, false, true);
      cu = TR.classicUnits(gl, TR.unitQuadVS(d.uv), d.uf, pack.units, d.units, d.pal, d.shd, pack.palette.shdRows, pack.palette.neutral[0]);
    }
    gl.activeTexture(gl.TEXTURE0);
    // the target: colour and depth+stencil (the silhouettes blend once a pixel through the stencil)
    const colour = gl.createTexture();
    gl.activeTexture(gl.TEXTURE4); gl.bindTexture(gl.TEXTURE_2D, colour);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, FW, FH, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.activeTexture(gl.TEXTURE0);
    const depth = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, depth);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, FW, FH);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, colour, 0);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, depth);
    if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) throw new Error('the stage target is incomplete');
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    const heights = new Uint8Array(mw * mh);                         // the stage is flat
    const meshTop = {};
    const look = { lane: 'parity', shadowSun: [0, 1, 0], shadowlen: null, airshadow: 'len' };
    let out = new Float32Array(1), qout = new Float32Array(1), wout = new Float32Array(1), lout = new Float32Array(1);
    const [neutral] = pack.palette.neutral;

    /* THE MODEL AS THE ENGINE HOLDS IT. A record's yaw turns the model as the engine holds it, X and Z negated by the
       loader (0x4CB590), as the game's stage turns the arena's nodes; the pack's meshes are in the file's axes, so the
       record's turn is applied after that half turn, which is the turn's own negation. */
    const held = ([cs, sn]) => [-cs, -sn];

    /* THE SCRIPT'S POSE of record `r`, as tagpu_wikiworld.c `pose_of` takes it: a unit's (never a ghost's) posed model
       at its slot in `poses`, with as many primitives as its type has pieces; the arrays pose.js `model` poses from it,
       in the held model turned onto the record's facing, so emitUnits draws them with the yaw [1, 0]. null draws the
       rest pose. */
    function posedOf(r, poses) {
      if (r.ghost || !poses || r.slot < 0 || r.slot >= poses.length) return null;
      const p = poses[r.slot], all = d.full[r.key], table = (pack.units.meshes[r.type] || {}).pieces;
      if (!p || !all || !table || p.n !== table.length) return null;
      return WikiPose.model(all, table, p, r.yaw[0], r.yaw[1]);
    }

    /* THE NANOFRAME WIRE OF ONE RANGE, as the game draws it (tagpu_vk_unit.c `wire_records`, tagpu_posedraw.c WVS):
       every face outline posed as emitUnits poses the body, each end at 1/65536 and on the line grid
       (floor(SS * p), tagpu_line.h), one band of six vertices an edge (tagpu_glsl.h `taBand`, 2 game pixels about
       the segment) that the fragment stage cuts to 0x4CC7AB's walk thickened to SS; the depth and the height carried
       along the segment, one notch nearer than the surface it traces (+0.15), on the flat path in the second
       oscillator's colour. `at` counts vertices; `r` is emitUnits' range, whose yaw is already the held one, or [1, 0]
       for a posed record's edges, which pose.js has turned; `w` the range's wire edges. */
    const BAND = 2.0;
    function wire(r, at, w) {
      const [cs, sn] = r.yaw, [hotX, hotY] = r.hot, wx = r.inst.pos[0], wz = r.inst.pos[1];
      const need = (at + (w.length / 6) * 6) * TR.UVST;
      if (wout.length < need) { const g = new Float32Array(need * 2); g.set(wout); wout = g; }
      if (lout.length < need / TR.UVST * 4) { const g = new Float32Array(need / TR.UVST * 8); g.set(lout); lout = g; }
      const fx = v => Math.floor(v * 65536 + 0.5) / 65536;
      let n = 0;
      for (let i = 0; i + 6 <= w.length; i += 6) {
        const end = [];
        for (let k = 0; k < 2; k++) {
          const x0 = w[i + 3 * k], y0 = w[i + 3 * k + 1], z0 = w[i + 3 * k + 2];
          const mx = fx(Math.fround(x0 * cs + z0 * sn)), my = fx(y0), mz = fx(Math.fround(-x0 * sn + z0 * cs));
          const qx = mx, qy = -mz - my * 0.5;
          const g = [Math.floor((hotX + qx) * SS), Math.floor((hotY + qy) * SS)];
          let md = (2 * my - mz) / 256;
          if (md > 1.8) md = 1.8; else if (md < -1.8) md = -1.8;
          end.push({ c: [(g[0] + 0.5) / SS, (g[1] + 0.5) / SS], enc: r.enc + md + 0.15, y: my, q: [qx, qy] });
        }
        const [A, B] = end, dx = B.c[0] - A.c[0], dy = B.c[1] - A.c[1], len = Math.hypot(dx, dy);
        const u = len > 0 ? [dx / len, dy / len] : [1, 0], nn = [-u[1], u[0]];
        for (const c of [0, 1, 2, 2, 1, 3]) {
          // corners: 0 A-, 1 B-, 2 A+, 3 B+ (taBand's two triangles (A-, B-, A+) and (A+, B-, B+))
          const atB = c === 1 || c === 3, side = c >= 2 ? 1 : -1;
          const t = len > 0 ? (atB ? 1 + BAND / len : -BAND / len) : (atB ? 1 : 0);
          const base = atB ? B.c : A.c, along = atB ? BAND : -BAND;
          const px = base[0] + along * u[0] + BAND * side * nn[0], py = base[1] + along * u[1] + BAND * side * nn[1];
          const mix = (a, b) => a + (b - a) * t;
          const q = (at + n) * TR.UVST, l = (at + n) * 4;
          wout.fill(0, q, q + TR.UVST);
          wout[q] = px; wout[q + 1] = py; wout[q + 2] = mix(A.enc, B.enc);
          wout[q + 3] = -1; wout[q + 4] = -1; wout[q + 5] = r.inst.nano.wire; wout[q + 6] = -1;
          wout[q + 7] = neutral / 31;
          wout[q + 8] = wx + mix(A.q[0], B.q[0]); wout[q + 9] = wz + mix(A.q[1], B.q[1]); wout[q + 10] = mix(A.y, B.y);
          wout[q + 12] = 1;
          lout[l] = A.c[0]; lout[l + 1] = A.c[1]; lout[l + 2] = B.c[0]; lout[l + 3] = B.c[1];
          n++;
        }
      }
      return n;
    }

    /* One frame of the view `v` at `tick`, its units posed by `poses` (sim.js `poses`; null: every unit at rest).
       Returns { complete, ground, resolved }. */
    function render(v, tick, poses) {
      const sc = WS.build(v, pack, tick);
      const fog = sc.fog;
      const f = { GW, GH, zoom: 1, zoomC: [0, 0], depth: pack.depth, vp: [0, 0, GW, GH], SS,
                  fog: fog ? 1 : 0, fogOrg: fog ? fog.org : [0, 0], fogDim: fog ? fog.dim : [1, 1] };
      if (fog) {
        gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, fogTex);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG8, fog.dim[0], fog.dim[1], 0, gl.RG, gl.UNSIGNED_BYTE, fog.grid);
        gl.activeTexture(gl.TEXTURE0);
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
      gl.viewport(0, 0, FW, FH);
      gl.disable(gl.SCISSOR_TEST);
      gl.clearColor(0, 0, 0, 0); gl.clearDepth(1); gl.clearStencil(0);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
      gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LESS); gl.depthMask(true); gl.disable(gl.BLEND);
      const E = [0, 0], W = [GW, GH], r0 = (E[1] >> 4) - 16;
      if (terr && layer('terrain')) {
        const g = terr.gather(tiles, pack.map.tilemap, E, W, 0, 0);
        terr.draw(f, g.n);
      }
      if (spr && layer('features')) {
        const got = TR.gatherFeatures({ lab: false, mirror: false, r0, E, W, vpL: 0, vpT: 0, w16: mw, h16: mh, heights,
          anchors, mapAnchors: anchors, defs: pack.features.defs, atlas: pack.features.atlas, D: pack.depth });
        if (got) {
          // EACH CORNER'S OWN WORLD POINT for the fog lookup, as the game's feature gather gives it (tagpu_feat.c
          // emit_frame: the anchor plus the corner's offset from the projected anchor), so a deposit across a fog
          // edge fades across it; the stage's ground is flat and its camera the map's corner, so that point is the
          // corner's own
          for (const vs of [got.shadow, got.body])
            for (let i = 0; i + TR.SVST <= vs.length; i += TR.SVST) { vs[i + 7] = vs[i]; vs[i + 8] = vs[i + 1]; }
          const all = got.shadow.concat(got.body);
          gl.bindBuffer(gl.ARRAY_BUFFER, sprVBO);
          gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(all), gl.DYNAMIC_DRAW);
          // the map on the tile grid and a black edge, as tagpu_wikiworld.c hands them over: the stage's
          // cells are even (the loader refuses odd ones), so this is the target's own size and the
          // clip at it discards no sample the target holds
          spr.use({ ...f, mapPx: [(mw >> 1) * 32, (mh >> 1) * 32], clip: 0 });
          spr.draw(got.shadow.length / TR.SVST, got.body.length / TR.SVST, got.shadow.length / TR.SVST);
        }
      }
      const recs = sc.units.concat(sc.ghosts);
      let refused = 0;
      if (cu && recs.length && layer('units')) {
        /* THE GAME'S SHADE RULE (tagpu_wikiworld.c `unit_one`, tagpu_posedraw.c VS): a structure, and every ghost, on
           the engine's lit rows from its vertex normals; any other unit on the neutral row. The stage takes the
           Shading option as on, the game's default. A posed record's arrays are its own this frame, under a key of
           its slot's. */
        const meshOf = { ...d.meshes }, slantOf = { ...d.slants }, wireOf = { ...d.wires }, posedKeys = [];
        const inst = recs.map(r => {
          const lit = r.ghost || !(pack.units.meshes[r.type] || {}).mobile, pz = posedOf(r, poses);
          if (!pz) return { ...r, type: r.key, agl: 0, yaw: held(r.yaw), engineShade: { lit, normals: d.normals[r.key], quad: d.quads[r.key] } };
          const key = `${r.key}@${r.slot}`;
          meshOf[key] = pz.mesh; slantOf[key] = pz.slant; wireOf[key] = pz.wire; posedKeys.push(key);
          return { ...r, type: key, agl: 0, yaw: [1, 0], engineShade: { lit, normals: pz.normals, quad: pz.quad } };
        });
        let need = 0;
        for (const i of inst) need += ((meshOf[i.type] || []).length / TR.UNIT_STRIDE + (slantOf[i.type] || []).length / 3) * TR.UVST;
        if (out.length < need) { out = new Float32Array(need); qout = new Float32Array(need / TR.UVST * TR.QVST); }
        const { n } = TR.emitUnits({ instances: inst, meshData: meshOf, slantData: slantOf, meshTop, out, qout, D: pack.depth,
          r0, E, W, vpL: 0, vpT: 0, w16: mw, h16: mh, heights, neutral, dir: pack.palette.neutral[1] }, look);
        // emitUnits caches each key's top; a posed key's arrays are this frame's alone
        for (const k of posedKeys) delete meshTop[k];
        refused = recs.length - look.unitRanges.length;
        gl.bindBuffer(gl.ARRAY_BUFFER, ua.vbo);
        gl.bufferData(gl.ARRAY_BUFFER, out.subarray(0, n * TR.UVST), gl.DYNAMIC_DRAW);
        gl.bindBuffer(gl.ARRAY_BUFFER, ua.qvbo);
        gl.bufferData(gl.ARRAY_BUFFER, qout.subarray(0, n * TR.QVST), gl.DYNAMIC_DRAW);
        TR.unitQuadOff(gl);
        gl.enable(gl.BLEND); gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
        gl.bindVertexArray(ua.vao);
        cu.use(f);
        const P = cu.U;
        /* BACK FACES CULLED for the shadows and the bodies, as the game's body pipeline culls them (tagpu_vk_unit.c,
           BACK FACES CULLED): the engine's rasterisers paint only a face whose vertices run clockwise on its y-down
           screen, and a model closes a surface with two faces wound apart and textured apart, so a turned piece
           shows the face that now runs clockwise. The vertices here are the engine's screen coordinates and the
           target keeps its y with them (the blit flips it), so the engine's clockwise is GL's counter-clockwise, its
           front face. The wire and the ghosts are not culled, as the game's are not. */
        gl.enable(gl.CULL_FACE); gl.cullFace(gl.BACK); gl.frontFace(gl.CCW);
        // the shadows first, as the engine emits them inside each unit's own draw; then the bodies
        cu.silhouettes(look.unitRanges, r => !r.inst.ghost);
        gl.depthMask(true);
        for (const r of look.unitRanges) {
          if (r.inst.ghost || !r.count) continue;
          const nn = r.inst.nano;
          gl.uniform1i(P.uNanoOn, nn ? 1 : 0);
          if (nn) { gl.uniform1f(P.uNanoT, nn.t); gl.uniform3f(P.uNanoC, nn.c[0], nn.c[1], nn.c[2]); }
          gl.drawArrays(gl.TRIANGLES, r.first, r.count);
        }
        gl.uniform1i(P.uNanoOn, 0);
        gl.disable(gl.CULL_FACE);
        // each nanoframe's wire, after every body
        let wn = 0;
        const wr = [];
        for (const r of look.unitRanges) if (!r.inst.ghost && r.inst.nano) { const k = wire(r, wn, wireOf[r.inst.type]); wr.push([wn, k]); wn += k; }
        if (wn) {
          gl.bindVertexArray(wa.vao);
          gl.bindBuffer(gl.ARRAY_BUFFER, wa.vbo);
          gl.bufferData(gl.ARRAY_BUFFER, wout.subarray(0, wn * TR.UVST), gl.DYNAMIC_DRAW);
          gl.bindBuffer(gl.ARRAY_BUFFER, wa.lvbo);
          gl.bufferData(gl.ARRAY_BUFFER, lout.subarray(0, wn * 4), gl.DYNAMIC_DRAW);
          // the line grid: the target's own pixels, SS to a game pixel, the thickness SS (tagpu_line.h `tagpu_line_grid`)
          gl.uniform4i(gl.getUniformLocation(cu.prog, 'uGrid'), (GW * SS) | (SS << 16), GH * SS, FW, FH);
          for (const [a, k] of wr) gl.drawArrays(gl.TRIANGLES, a, k);
          gl.bindVertexArray(ua.vao);
        }
        // the ghost stage: translucent, depth writes off, so ghosts blend with each other
        gl.depthMask(false);
        gl.uniform1f(P.uAlpha, WS.GHOST_ALPHA);
        for (const r of look.unitRanges) if (r.inst.ghost && r.count) gl.drawArrays(gl.TRIANGLES, r.first, r.count);
        gl.uniform1f(P.uAlpha, 1);
        gl.depthMask(true);
        gl.bindVertexArray(null);
        gl.disable(gl.BLEND);
      }
      // the composite's source: the target into this canvas, flipped (the target's row 0 is the map's bottom)
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, fbo);
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
      gl.blitFramebuffer(0, 0, FW, FH, 0, FH, FW, 0, gl.COLOR_BUFFER_BIT, gl.NEAREST);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      const err = gl.getError();
      return { complete: !refused && err === gl.NO_ERROR && !gl.isContextLost(), ground, resolved: sc.resolved,
               refused, glError: err };
    }
    return { canvas, render, pack };
  }

  /* The stage's image for `entry` showing view `v` at `tick`: { canvas, complete, ground, resolved }, or null while the
     game graphics are off or the entry's pack is loading. A pack that fails to load leaves the stage whitebox. `dir`
     is the pack's folder under gfx/: the entry's own by default, or a mod's (side/<mod>/stage/<entry>, gfx.py
     --stages), which holds that mod's units for the casting the view is bound to. `poses` is sim.js `poses()` for the
     view, null to draw every unit at rest. */
  const shown = {};                     // entry id -> the folder its last frame was drawn from
  function frame(entry, v, tick, dir = entry, poses = null) {
    if (!on() || !entry || !index || !index.entries || !index.entries[entry]) return null;
    shown[entry] = dir;
    let st = stages[dir];
    if (!st) {
      stages[dir] = open(dir).then(s => { stages[dir] = s; }, x => { console.error('wiki gfx:', x); stages[dir] = { error: x }; });
      return null;
    }
    if (st instanceof Promise || st.error) return null;
    try {
      const r = st.render(v, tick, poses);
      return { canvas: st.canvas, ...r };
    } catch (x) { console.error('wiki gfx:', x); stages[dir] = { error: x }; return null; }
  }
  // what a scripted check reads: whether an entry's stage has settled -- drawn in 3D, or failed and so whitebox -- or,
  // with the graphics off or gfx/ absent, that nothing is coming
  const ready = entry => { const st = stages[shown[entry] || entry]; return mode !== 'game' || state === 'missing' ||
    (state === 'ready' && !!st && !(st instanceof Promise)); };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', control); else control();
  if (mode === 'game') load();
  return { frame, url, on, ready, state: () => state, SS };
})();
