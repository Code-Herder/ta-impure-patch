/* scene.js -- the in-game wiki's 3D stage: one view of sim.js turned into what the world passes draw. Pure: a view, the
   stage's pack (the scene.json tools/wiki/gfx.py writes) and the simulation's tick in, records out; no GL, no DOM. It
   runs in the page, where gfx.js draws its records with tools/tascene-render.js, and under node.
   tagpu/ddraw/src/tagpu_wikiscene.c is its port (research/notes/ingame-wiki-3d-stage-plan.md, landings 6g and 6h),
   paired with it by tools/wiki/scenecheck.mjs, as stage.js and tagpu_wikistage.c are paired by stagecheck.mjs.

   What it makes, each the engine's rule (the plan, sections 3, 6 and 7):
   - **a model record per unit and per ghost**: the type's mesh for its owner, its anchor (the unit's world point, on
     the flat ground), its yaw from the facing vector, alpha 1 with depth writes, or for a ghost alpha 0.40 without
     them, the Classic shadow kind (a structure's slant, a mobile unit's silhouette; a ghost and a nanoframe none) and
     the nanoframe staging of tagpu_nanostage.h, ported below. A unit being reclaimed is drawn finished: the engine
     reclaims a unit by hitting its hit points and never makes it a nanoframe (the C file says where). The game's
     record carries every position, facing and fraction as a float (tagpu_wikistage.h), so the records here take
     them through Math.fround, and the two sides compute from the same numbers;
   - **the fog grid** in the engine's form: one entry a 32-unit block, two 4-bit corner masks (b0 unexplored, b1 grey)
     whose corners are the centres of this block and its right, lower and lower-right neighbours, the far column and
     row replicating their own block (tagpu_glsl.h, *the clamp stops one cell short of the grid*).
   A type the pack did not resolve has no record: stage.js draws its whitebox box over the image instead. */
const WikiScene = (() => {
  // TA's default heading 0x8000, toward the viewer: the facing (0, 1). Buildings always take it (0x485A40's centre)
  const DEFAULT_FACING = [0, 1];
  const GHOST_ALPHA = 0.40;                      // tagpu_native.c ghost_armed

  /* THE YAW FROM THE VECTOR, with no angle (the plan, section 6): the model's +z turned onto the facing (hx, hz). The
     rotation tascene-render.js's emitUnits applies is P = (x cs + z sn, y, -x sn + z cs), and the world offset of a
     model point is (P.x, -P.z), so the model's +z lands on (sn, -cs); (cs, sn) = (-hz, hx). At (0, 1) that is
     (-1, 0), diag(-1, 1, -1): ghost_one's body rotation for 0x8000. */
  const yaw = (hx, hz) => [-hz, hx];

  /* ---- tagpu_nanostage.h's nanoframe staging, line for line (build-state.md 0x458DD0 / 0x458D30). `nano` is the build
     fraction REMAINING (unit +0x104), `id` the unit's slot and `tick` the simulation's; null for a unit that is not a
     nanoframe. The C arithmetic: unsigned tick * 0x21 / 0x1E, int division toward zero, nano * 255.0f in float. */
  const tri = ph => (ph & 0x10) ? 0xAF - (ph & 0xF) : 0xA0 + (ph & 0xF);
  const idiv = (a, b) => Math.trunc(a / b);
  function nanoState(nano, id, tick) {
    if (!(nano > 0 && nano <= 1)) return null;
    const t30 = idiv((tick >>> 0) * 0x21, 0x1E) >>> 0, t57 = idiv((tick >>> 0) * 0x39, 0x1E) >>> 0;
    const b1 = tri(((id ^ 5) + t30) | 0), b2 = tri(((id ^ 9) + t57) | 0);
    let p = Math.trunc(Math.fround(Math.fround(nano) * 255));
    if (p > 255) p = 255; else if (p < 0) p = 0;
    const B1 = Math.fround(b1 / 255), wire = Math.fround(b2 / 255);
    let t, c;
    if (p >= 236)      { t = idiv((p - 235) * 255, 20);                 c = [-2, B1, -2]; }
    else if (p >= 201) { t = idiv((p - 200) * 255, 35);                 c = [-2, B1, -2]; }
    else if (p >= 116) { t = (idiv((115 - p) * 255, 85) - 1) & 0xFF;   c = [-2, wire, B1]; }
    else if (p >= 31)  { t = (idiv((30 - p) * 255, 85) - 1) & 0xFF;    c = [B1, wire, -1]; }
    else               { t = idiv(p * 255, 30);                         c = [-1, B1, -1]; }
    return { t, c, wire };
  }

  /* ---- the fog grid: `fog` one value a cell (0 clear, 1 or 'grey' seen before, 2 or 'black' unexplored), mw x mh
     cells; the stage's fog rects are whole tiles, so each 32-unit block is uniform and its top-left cell stands for
     it. Returns null when no cell is fogged. */
  function fogGrid(fog, mw, mh) {
    if (!fog || !fog.some(f => f)) return null;
    const bw = mw >> 1, bh = mh >> 1, grid = new Uint8Array(bw * bh * 2);
    const at = (bx, bz) => {
      const f = fog[Math.min(bz, bh - 1) * 2 * mw + Math.min(bx, bw - 1) * 2];
      return f === 2 || f === 'black' ? 1 : f ? 2 : 0;          // 1 = unexplored, 2 = grey
    };
    for (let bz = 0; bz < bh; bz++) for (let bx = 0; bx < bw; bx++) {
      const k = [at(bx, bz), at(bx + 1, bz), at(bx, bz + 1), at(bx + 1, bz + 1)];   // tl, tr, bl, br
      let b0 = 0, b1 = 0;
      k.forEach((v, i) => { if (v === 1) b0 |= 1 << i; else if (v === 2) b1 |= 1 << i; });
      grid[(bz * bw + bx) * 2] = b0; grid[(bz * bw + bx) * 2 + 1] = b1;
    }
    return { grid, dim: [bw, bh], org: [16, 16] };
  }

  const F = Math.fround;
  // a player of the eight, else none (tagpu_wikistage.c `owner8`): a record is made only for one
  const owner8 = o => Number.isInteger(o) && o >= 0 && o < 8 ? o : -1;

  /* One model record, or null: `u` has type, x, z (its world point: a building's footprint centre), and a facing. The
     depth row's key is emitUnits' unit0 + rel * rowStep at the stage's camera (rel the row, from 0), which the C side
     computes as tagpu_wikiscene.c `row_key`. */
  function record(pack, u, owner, extra) {
    const m = pack.units && pack.units.meshes && pack.units.meshes[u.type];
    if (!m || owner < 0 || !m.files[String(owner)]) return null;
    const [hx, hz] = m.mobile && Number.isFinite(u.hx) && Number.isFinite(u.hz) ? [F(u.hx), F(u.hz)] : DEFAULT_FACING;
    const [x, z] = [F(u.x), F(u.z)], mh = pack.cells ? pack.cells[1] : 1;
    const row = Math.min(mh - 1, Math.max(0, z >> 4)), D = pack.depth || { unit0: 1, rowStep: 4 };
    return { type: u.type, key: `${u.type}.${owner}`, owner, pos: [x, z], yaw: yaw(hx, hz), enc: D.unit0 + row * D.rowStep,
             shadow: !!m.shadow, slant: !!m.slant, alpha: 1, ghost: false, nano: null, slot: -1, ...extra };
  }

  /* THE STAGE'S 3D FEATURES (a wreck, a heap) still standing: the pack's list (gfx.py features3d) less the anchors the
     view says were reclaimed. The engine's map spawn (SpawnFeatureOnMap 0x423C50, no position or turn given) puts the
     wreck record at the footprint's centre, 0x423E3D..0x423E68, unturned, 0x423EAC, and the native pass draws it owner
     0, unlit, with no shadow of its own, in the features' depth band 3 + 4 row (tagpu_native.c, the husk gather). */
  function wrecks(v, pack) {
    const gone = new Set((v.gone || []).map(([x, z]) => `${x},${z}`)), out = [];
    const mh = pack.cells ? pack.cells[1] : 1, D = pack.depth || { tall0: 3, rowStep: 4 };
    for (const f of pack.features3d || []) {
      const m = pack.units && pack.units.meshes && pack.units.meshes[f.key];
      if (!m || !m.files['0'] || gone.has(`${f.cell[0]},${f.cell[1]}`)) continue;
      const x = F((2 * f.cell[0] + f.footprint[0]) * 8), z = F((2 * f.cell[1] + f.footprint[1]) * 8);
      const row = Math.min(mh - 1, Math.max(0, z >> 4));
      out.push({ type: f.key, key: `${f.key}.0`, owner: 0, pos: [x, z], yaw: [1, 0], enc: D.tall0 + row * D.rowStep,
                 shadow: false, slant: false, alpha: 1, ghost: false, feature: true, nano: null, slot: -1 });
    }
    return out;
  }

  /* The view `v`, the pack and the tick -> { units, wrecks, ghosts, fog, resolved }. A unit's slot is its index in the view,
     the id its nanoframe's oscillators start from. A unit under construction is a nanoframe and casts no shadow
     (tagpu_native.c, build-state.md 7); one being reclaimed is neither (the engine's reclaim hits its hit points). */
  function build(v, pack, tick) {
    const out = { units: [], wrecks: [], ghosts: [], fog: null, resolved: new Set(Object.keys((pack.units || {}).meshes || {})) };
    if (!v || !v.map) return out;
    out.wrecks = wrecks(v, pack);
    out.fog = fogGrid(v.map.fog, +v.map.w || 0, +v.map.h || 0);
    (v.units || []).forEach((u, i) => {
      const built = u.built == null ? 1 : Math.min(1, Math.max(0, F(+u.built)));
      const nano = built < 1 ? nanoState(F(1 - built), i, tick) : null;
      const r = record(pack, u, owner8(u.owner), { nano, slot: i });
      if (r && nano) r.shadow = false;
      if (r) out.units.push(r);
    });
    /* THE GHOST STAGE, in ghost_record's order (tagpu_native.c): the placement ghost, each gesture site's (whatever
       its colour), then each queued build not yet started; a started one is the nanoframe itself. The engine knows
       which from the order node's own target (`started`); the view carries no such link, so a queued site with a unit
       of its type standing on its exact point is the started one -- exact, because sim.js places a new unit on its
       site's point. Untinted, as the game draws them. */
    const ghost = (g, owner) => record(pack, g, owner, { alpha: GHOST_ALPHA, ghost: true, shadow: false });
    const push = r => { if (r) out.ghosts.push(r); };
    if (v.ghost && v.ghost.type) push(ghost(v.ghost, 0));
    for (const st of v.sites || []) if (st.type) push(ghost(st, 0));
    const standing = new Set((v.units || []).map(u => `${u.type}@${F(u.x)},${F(u.z)}`));
    for (const q of v.queued || [])
      if (q.type && !standing.has(`${q.type}@${F(q.x)},${F(q.z)}`)) push(ghost(q, owner8(q.owner)));
    return out;
  }

  return { build, wrecks, nanoState, fogGrid, yaw, GHOST_ALPHA };
})();
if (typeof module !== 'undefined') module.exports = WikiScene;
