/* unitview.js -- the tech tree entry's unit, in 3D: the unit's model posed by its own COB script through the wiki's COB
   host (tools/wiki/serve.py POST /cob, the C the DLL's wiki runs; cob.js), drawn with a free orbit camera in the unit
   panel (research/notes/ingame-wiki.md, *The tech tree entry*). The pack is gfx.py `tech_pack`'s: 7 floats a vertex,
   x y z u v fc ck -- u < 0 a flat palette colour fc, else the palette-indexed atlas at (u, v) with colour key ck.
   Each vertex also carries the pack's quad stream, 8 floats: its point in its quad's frame (2), the quad's fourth corner
   in that frame (2) and the frame's atlas rect (4), x < 0 where the face keeps its uv. A textured quad is mapped per
   pixel through the inverse bilinear map, as the game's unit pass maps it (tagpu_native.c taQuadST), not as two
   triangles that kink the texture along the diagonal; and back faces are culled, as the game's body pipeline culls
   them (tagpu_vk_unit.c), so a surface the model closes with two faces wound apart shows its outside.
   The pose is pose.js's (WikiPose). The orbit camera is the owner's choice (2026-11-16): the game's unit pass draws in
   the engine's fixed projection, so the DLL needs a perspective unit view of its own for it (a gap).

   mount(host, acts, u, base, cobPack) puts the view in `host` and its action buttons in `acts`: `u` the unit's record
   (techtree.js model), `base` the pack's URL with its slash, `cobPack` its path from the repository root for the host.
   Returns {dispose}. Runs in a page (the global WikiUnitView). */
const WikiUnitView = (() => {
const PACKS = {};
  const HINT = 'drag to orbit · wheel to zoom · its own COB script poses it';
  const bin = u => fetch(u).then(r => { if (!r.ok) throw new Error(`${u}: ${r.status}`); return r.arrayBuffer(); }).then(b => new Uint8Array(b));
  const f32 = b => new Float32Array(b.buffer, b.byteOffset, b.byteLength >> 2);
  
  function packOf(base) {
    return PACKS[base] ||= (async () => {
      const pack = await fetch(base + 'scene.json').then(r => r.json());
      const [atlas, pal] = await Promise.all([bin(base + pack.units.atlas.file), bin(base + 'pal.bin')]);
      return { pack, atlas, pal };
    })();
  }
  async function unitArrays(base, m) {
    const [mesh, quad, nv, s, w, pc] = await Promise.all([bin(base + m.files['0']), bin(base + m.quadFiles['0']),
      bin(base + m.normalsFile), bin(base + m.slantFile), bin(base + m.wireFile), m.pieceFile ? bin(base + m.pieceFile) : null]);
    const all = { mesh: f32(mesh), quad: f32(quad), normals: f32(nv), slant: f32(s), wire: f32(w) };
    if (!pc) return { all, full: null };
    const ids = new Uint16Array(pc.buffer, pc.byteOffset, pc.byteLength >> 1), nb = m.verts, ns = m.slantVerts;
    Object.assign(all, { bp: ids.subarray(0, nb), sp: ids.subarray(nb, nb + ns), wp: ids.subarray(nb + ns) });
    return { all, full: all };
  }
  
  const VS = `#version 300 es
  layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUV; layout(location=2) in vec2 aFC;
  layout(location=3) in vec2 aQH; layout(location=4) in vec2 aQAB; layout(location=5) in vec4 aQR;
  uniform mat4 uMVP; out vec2 vUV; flat out vec2 vFC; out vec3 vP; out vec2 vQH; flat out vec2 vQAB; flat out vec4 vQR;
  void main(){ vUV = aUV; vFC = aFC; vP = aPos; vQH = aQH; vQAB = aQAB; vQR = aQR; gl_Position = uMVP * vec4(aPos, 1.0); }`;
  const FS = `#version 300 es
  precision highp float; precision highp usampler2D;
  in vec2 vUV; flat in vec2 vFC; in vec3 vP; in vec2 vQH; flat in vec2 vQAB; flat in vec4 vQR; out vec4 o;
  uniform usampler2D uAtlas; uniform sampler2D uPal; uniform vec2 uAtlasSize; uniform vec3 uLight;
  // tagpu_native.c taQuadST, copied: the frame point h back to the quad's (s,t) through the inverse bilinear map
  vec2 taQuadST(vec2 h, vec2 ab){
    vec2 g = ab - 1.0;
    float k1 = 1.0 + h.x * g.y - h.y * g.x;
    float D = max(k1 * k1 + 4.0 * g.x * h.y, 0.0);
    float qt = 2.0 * h.y / max(k1 + sqrt(D), 1e-6);
    return vec2(h.x / max(1.0 + g.x * qt, 1e-6), qt);
  }
  void main(){
    int idx;
    if (vUV.x < 0.0) idx = int(vFC.x * 255.0 + 0.5);
    else {
      // a quad the bake gave a frame spans its rect by (s,t); every other textured face keeps its triangle's uv
      vec2 uv = vQR.x >= 0.0 ? vQR.xy + (vQR.zw - vQR.xy) * clamp(taQuadST(vQH, vQAB), 0.0, 1.0) : vUV;
      idx = int(texelFetch(uAtlas, ivec2(clamp(uv, 0.0, 0.9999) * uAtlasSize), 0).r);
      if (idx == int(vFC.y * 255.0 + 0.5)) discard;
    }
    vec3 n = normalize(cross(dFdx(vP), dFdy(vP)));
    float l = 0.45 + 0.65 * max(dot(n, uLight), 0.0);
    o = vec4(texelFetch(uPal, ivec2(idx, 0), 0).rgb * l, 1.0);
  }`;
  
  function program(gl) {
    const sh = (t, s) => { const x = gl.createShader(t); gl.shaderSource(x, s); gl.compileShader(x);
      if (!gl.getShaderParameter(x, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(x)); return x; };
    const p = gl.createProgram();
    gl.attachShader(p, sh(gl.VERTEX_SHADER, VS)); gl.attachShader(p, sh(gl.FRAGMENT_SHADER, FS)); gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    return p;
  }
  function mat(yaw, pitch, dist, aspect, c) {
    // perspective * view (orbit about c), column-major
    const f = 1 / Math.tan(0.45), n = dist * 0.05, fr = dist * 10;
    const P = [f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, (fr + n) / (n - fr), -1, 0, 0, 2 * fr * n / (n - fr), 0];
    const cy = Math.cos(yaw), sy = Math.sin(yaw), cp = Math.cos(pitch), sp = Math.sin(pitch);
    const eye = [c[0] + dist * cp * sy, c[1] + dist * sp, c[2] + dist * cp * cy];
    const z = [eye[0] - c[0], eye[1] - c[1], eye[2] - c[2]].map(v => v / dist);
    let x = [z[2], 0, -z[0]]; const xl = Math.hypot(...x) || 1; x = x.map(v => v / xl);
    const y = [z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]];
    const V = [x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
      -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]), -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]), -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1];
    const o = new Float32Array(16);
    for (let i = 0; i < 4; i++) for (let j = 0; j < 4; j++) { let s = 0; for (let k = 0; k < 4; k++) s += P[k * 4 + j] * V[i * 4 + k]; o[i * 4 + j] = s; }
    return o;
  }
  
  function mount(host, acts, u, base, cobPack) {
    let alive = true, raf = 0, tick = 0, world = null, unit = 0, facts = [[1, 0, 0, 0, 0, 100]];
    const canvas = document.createElement('canvas'); host.append(canvas);
    const note = document.createElement('div'); note.className = 'note'; host.append(note);
    const say = t => { note.textContent = t; };
    const world_path = () => cobPack;
    const cam = { yaw: 0.7, pitch: 0.45, dist: 60, c: [0, 10, 0], spin: true };
    say('loading the model…');
    (async () => {
      const { pack, atlas, pal } = await packOf(base);
      const m = pack.units.meshes[u.name];
      if (!alive) return;
      if (!m) { say(`no model: ${pack.units.refused[u.name] || 'not in the pack'}`); return; }
      const { all, full } = await unitArrays(base, m);
      if (!alive) return;
      // THE HELD AXES for the rest pose too: the posed arrays come out of WikiPose.model in the engine's held axes
      // ((x, y, z) -> (-x, y, -z), half a turn about Y), so the rest pose is taken into them as well -- the unit stands
      // the same way posed or not, and the camera is fitted on the bounds of what it draws, as the DLL's view is
      // (tagpu/ddraw/src/tagpu_wikiview.h)
      const restFile = full ? WikiPose.rest(full, m.pieces) : all;
      const restArr = { ...restFile, mesh: restFile.mesh.slice() };
      for (let i = 0; i < restArr.mesh.length; i += 7) { restArr.mesh[i] = -restArr.mesh[i]; restArr.mesh[i + 2] = -restArr.mesh[i + 2]; }
      // frame the camera on the rest pose's bounds
      const lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
      for (let i = 0; i < restArr.mesh.length; i += 7) for (let a = 0; a < 3; a++) {
        lo[a] = Math.min(lo[a], restArr.mesh[i + a]); hi[a] = Math.max(hi[a], restArr.mesh[i + a]); }
      cam.c = [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2];
      cam.dist = Math.max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) * 1.9 + 8;
      const gl = canvas.getContext('webgl2', { antialias: true });
      const prog = program(gl);
      gl.activeTexture(gl.TEXTURE0);
      const ta = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, ta);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8UI, pack.units.atlas.w, pack.units.atlas.h, 0, gl.RED_INTEGER, gl.UNSIGNED_BYTE, atlas);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.activeTexture(gl.TEXTURE1);
      const tp = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, tp);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, pal);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.useProgram(prog);
      gl.uniform1i(gl.getUniformLocation(prog, 'uAtlas'), 0); gl.uniform1i(gl.getUniformLocation(prog, 'uPal'), 1);
      gl.uniform2f(gl.getUniformLocation(prog, 'uAtlasSize'), pack.units.atlas.w, pack.units.atlas.h);
      gl.uniform3f(gl.getUniformLocation(prog, 'uLight'), -0.48, 0.8, 0.36);
      const uMVP = gl.getUniformLocation(prog, 'uMVP');
      const vbo = gl.createBuffer(), qbo = gl.createBuffer(), vao = gl.createVertexArray();
      gl.bindVertexArray(vao); gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
      gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 28, 0);
      gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 28, 12);
      gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 2, gl.FLOAT, false, 28, 20);
      gl.bindBuffer(gl.ARRAY_BUFFER, qbo);
      gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 2, gl.FLOAT, false, 32, 0);
      gl.enableVertexAttribArray(4); gl.vertexAttribPointer(4, 2, gl.FLOAT, false, 32, 8);
      gl.enableVertexAttribArray(5); gl.vertexAttribPointer(5, 4, gl.FLOAT, false, 32, 16);
      gl.enable(gl.DEPTH_TEST);
      /* BACK FACES CULLED, as the game culls them: the engine paints a face only when it runs clockwise on its y-down
         screen, which in the pack's right-handed model space is a face wound counter-clockwise seen from outside
         (MEASURED 2026-11-16: in 262 of stock's 266 rest-pose models most faces wound CCW point away from the
         centre). This camera does not mirror, so CCW stays GL's front face. */
      gl.enable(gl.CULL_FACE); gl.cullFace(gl.BACK); gl.frontFace(gl.CCW);
  
      // the COB host: a world of one type, one finished unit at the origin
      if (full && m.script) {
        try {
          world = WikiCob.cobPage(world_path()).open([u.name]);
          unit = world.spawn(0, facts);
        } catch (e) { world = null; say(`no COB host (serve with tools/wiki/serve.py): ${e.message || e}`); }
      }
      buttons(m);
      let empty = false;
      if (world) say(HINT);
      else if (!full || !m.script) say('no script: the rest pose');
      let last = performance.now(), acc = 0;
      const frame = now => {
        if (!alive) return;
        // a hidden tab stops the frames; on return, play on rather than catch up every tick (one synchronous host call each)
        acc = Math.min(acc + now - last, 100); last = now;
        if (world) while (acc >= 1000 / 30) { acc -= 1000 / 30; tick++; try { world.slice(facts); } catch { world = null; } onTick?.(); }
        if (cam.spin) cam.yaw += 0.004;
        const p = world && world.model(unit);
        const posed = p ? WikiPose.model(full, m.pieces, p, 1, 0) : restArr, arr = posed.mesh;
        // a model with no faces, or a script that hides every piece with faces, leaves nothing to draw: say so
        if (!arr.length !== empty) {
          empty = !arr.length;
          say(empty ? 'nothing to draw: every piece with faces is hidden' : world ? HINT : 'no script: the rest pose');
        }
        const w = canvas.clientWidth * devicePixelRatio | 0, h = canvas.clientHeight * devicePixelRatio | 0;
        if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
        gl.viewport(0, 0, w, h); gl.clearColor(0, 0, 0, 0); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        gl.bindBuffer(gl.ARRAY_BUFFER, qbo); gl.bufferData(gl.ARRAY_BUFFER, posed.quad, gl.DYNAMIC_DRAW);
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo); gl.bufferData(gl.ARRAY_BUFFER, arr, gl.DYNAMIC_DRAW);
        gl.uniformMatrix4fv(uMVP, false, mat(cam.yaw, cam.pitch, cam.dist, w / h, cam.c));
        gl.drawArrays(gl.TRIANGLES, 0, arr.length / 7);
        raf = requestAnimationFrame(frame);
      };
      raf = requestAnimationFrame(frame);
    })().catch(e => say(String(e.message || e)));
  
    // orbit
    let drag = null;
    // a drag takes the camera from the turn: the Turn button goes off with it, and a press turns it on again
    canvas.addEventListener('pointerdown', e => { drag = { x: e.clientX, y: e.clientY, yaw: cam.yaw, pitch: cam.pitch }; canvas.setPointerCapture(e.pointerId);
      cam.spin = false; turnBtn?.classList.remove('on'); });
    canvas.addEventListener('pointermove', e => { if (!drag) return;
      cam.yaw = drag.yaw - (e.clientX - drag.x) * 0.01; cam.pitch = Math.max(-1.4, Math.min(1.4, drag.pitch + (e.clientY - drag.y) * 0.01)); });
    canvas.addEventListener('pointerup', () => { drag = null; });
    canvas.addEventListener('wheel', e => { e.preventDefault(); cam.dist *= Math.exp(e.deltaY * 0.001); }, { passive: false });
  
    // the actions: what the unit's FBI and script say it can do, each one call the engine itself makes
    let onTick = null, turnBtn = null;
    function buttons(m) {
      const call = (s, how, args) => { if (!world) return 0; const r = world.call(unit, s, how, args, facts); return r; };
      const list = [];
      if (u.mobile && !u.canfly) list.push(['Walk', b => {
        const on = !b.classList.contains('on');
        if (on) { call('StartMoving', 'now'); call('MoveRate1', 'now'); } else call('StopMoving', 'now');
        b.classList.toggle('on', on);
      }]);
      if (u.canfly) list.push(['Take off', b => { const on = !b.classList.contains('on'); call(on ? 'Activate' : 'Deactivate', 'now'); world?.activate(unit, on); b.classList.toggle('on', on); }]);
      if (u.weapons.length) {
        let h = 0;
        list.push(['Aim', b => {
          const on = !b.classList.contains('on'); b.classList.toggle('on', on);
          onTick = on ? () => { if (tick % 45 === 0) { h = (h + 12000) & 0xffff; call('AimPrimary', 'args', [h > 32767 ? h - 65536 : h, 0]); } } : null;
          if (on) call('AimPrimary', 'args', [0, 0]);
        }]);
        list.push(['Fire', () => { call('FirePrimary', 'later'); call('Shot1', 'later'); }]);
      }
      if (u.builder) list.push(['Build', b => {
        const on = !b.classList.contains('on');
        if (on) call('StartBuilding', 'args', [0]); else call('StopBuilding', 'later');
        b.classList.toggle('on', on);
      }]);
      if (u.structure && !u.weapons.length) list.push(['On / off', b => { const on = !b.classList.contains('on'); world?.activate(unit, on); b.classList.toggle('on', on); }]);
      list.push(['Turn', b => { cam.spin = !cam.spin; b.classList.toggle('on', cam.spin); }]);
      list.push(['Reset', () => { if (!world) return; try { world.free?.(); } catch {} world = WikiCob.cobPage(world_path()).open([u.name]); unit = world.spawn(0, facts); acts.querySelectorAll('.on').forEach(x => { if (x !== turnBtn) x.classList.remove('on'); }); onTick = null; }]);
      acts.innerHTML = '';
      for (const [t, f] of list) { const b = document.createElement('button'); b.dataset.part = 'button'; b.textContent = t;
        if (t === 'Turn') { turnBtn = b; b.classList.toggle('on', cam.spin); } b.onclick = () => f(b); acts.append(b); }
    }
    return { dispose() { alive = false; cancelAnimationFrame(raf); try { world?.free?.(); } catch {} } };
  }
  return { mount };
})();
