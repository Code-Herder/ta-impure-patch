/* sim.js -- the in-game wiki's simplified simulation: a scene and its inputs in, a view of the game out.

   research/notes/ingame-wiki.md, "The simulation's contract", is its specification, and the DLL is to implement the
   same rules over the same entry files, tick for tick. The resource rules mirror the DLL's own code,
   tagpu/ddraw/src/tagpu_resource.c, function for function (research/notes/tadr-port/resource-snapping.md describes
   them); what the engine decides (the site test, orders, movement, construction) is simplified. A demo is its scene and its inputs played through this file, so a demo cannot show what
   the rules would not do, and Try it yourself is the same sim taking the player's inputs instead.

   Positions are world units (a footprint cell is 16) and time is ticks, 30 a second. The player is owner 0. It is
   deterministic: no clock, no randomness, units visited in creation order, and only + - * / and Math.sqrt on
   doubles (all correctly rounded), so another implementation can agree with it bit for bit.

   Runs in a page (the global WikiSim) and under node (module.exports). */
const WikiSim = (() => {
  const CELL = 16, TPS = 30, RADIUS = 3, ARENA = 64, DRAG = 4, FOLLOW = 48, SPACING = 24;
  const KEYS = ['shift', 'alt', 'ctrl', 'lmb', 'rmb'];
  const sq = v => v * v;

  /* the entry's scene with the demo's additions: units, metal, vents and fog appended; selected and cursor
     replaced when the demo gives them */
  function merge(entry, demo) {
    const a = entry.scene, b = (demo && demo.scene) || {};
    const cat = k => [...(a[k] || []), ...(b[k] || [])];
    return { map: a.map, metal: cat('metal'), vents: cat('vents'), fog: cat('fog'),
      units: cat('units'), selected: b.selected ?? a.selected ?? [], cursor: b.cursor ?? a.cursor ?? [0, 0] };
  }

  function make(entry, demo, scripted) {
    const S = merge(entry, demo), D = entry.units, defIndex = Object.keys(D);
    const W = S.map.w, H = S.map.h, SM = S.map.surfaceMetal || 0, PACE = (entry.pace && entry.pace.build) || 1;
    const metal = new Array(W * H).fill(SM), fog = new Array(W * H).fill(0);
    const fill = (a, [x, z, w, h], v) => {
      for (let j = Math.max(0, z); j < Math.min(H, z + h); j++) for (let i = Math.max(0, x); i < Math.min(W, x + w); i++) a[j * W + i] = v;
    };
    for (const m of S.metal) fill(metal, m.rect, m.v);
    for (const f of S.fog) fill(fog, f.rect, f.kind === 'black' ? 2 : 1);
    const vents = S.vents.map(([x, z]) => ({ cx: Math.floor(x / CELL), cz: Math.floor(z / CELL) }));

    /* ---- definitions ---- */
    const def = t => { const d = D[t]; if (!d) throw new Error(`no unit ${t} in the entry`); return d; };
    const fp = t => def(t).footprint;
    const mobile = t => def(t).maxVelocity > 0;
    // a resource building: a mex extracts metal, a geothermal plant needs a vent under its yard
    const kind = t => def(t).extracts > 0 ? 'mex' : def(t).vent ? 'geo' : null;
    const strength = t => kind(t) === 'mex' ? def(t).extracts : def(t).energyMake || 0;
    const canBuild = (u, t) => (def(u.type).builds || []).includes(t);

    /* ---- the grid ---- */
    const cellOf = v => Math.floor(v / CELL);
    const seen = (cx, cz) => fog[cz * W + cx] !== 2;
    const overlap = (a, b) => a.ox < b.ox + b.fx && b.ox < a.ox + a.fx && a.oz < b.oz + b.fz && b.oz < a.oz + a.fz;
    const box = (t, ox, oz) => ({ ox, oz, fx: fp(t)[0], fz: fp(t)[1] });

    /* ---- units ---- */
    let units = [], nextId = 1;
    function add(type, owner, x, z, built, id, label) {
      const [fx, fz] = fp(type), u = { id: id || `u${nextId++}`, type, owner, built, reclaimed: 0, orders: [], sel: false, label: label || type };
      if (mobile(type)) { u.x = x; u.z = z; return units.push(u), u; }
      return place(u, Math.round(x / CELL - fx / 2), Math.round(z / CELL - fz / 2));
    }
    function place(u, ox, oz) {
      const [fx, fz] = fp(u.type);
      Object.assign(u, { ox, oz, x: (ox + fx / 2) * CELL, z: (oz + fz / 2) * CELL });
      return units.push(u), u;
    }
    const byId = id => units.find(u => u.id === id);
    const structures = () => units.filter(u => !mobile(u.type));
    const structureAt = (t, ox, oz) => units.find(u => u.type === t && u.ox === ox && u.oz === oz);
    for (const s of S.units) {
      const u = add(s.type, s.owner ?? 0, s.at[0], s.at[1], s.built ?? 1, s.id, s.label);
      u.orders = (s.orders || []).map(o => ({ k: 'move', x: o.move[0], z: o.move[1] }));
    }
    for (const id of S.selected) byId(id).sel = true;
    const selected = () => units.filter(u => u.sel);
    const builders = () => selected().filter(u => u.owner === 0 && mobile(u.type) && def(u.type).workerTime > 0);
    // the types the selected builders' build lists offer, in definition order
    function available() {
      const ts = new Set();
      for (const u of builders()) for (const t of def(u.type).builds || []) if (D[t]) ts.add(t);
      return defIndex.filter(t => ts.has(t));
    }

    /* ---- plans: the player's build orders; one is started once its building stands at its site ---- */
    const started = o => !!structureAt(o.type, o.ox, o.oz);
    function plans() {
      const out = [];
      for (const u of units) if (u.owner === 0) for (const o of u.orders)
        if (o.k === 'build' && !started(o) && !out.some(p => p.type === o.type && p.ox === o.ox && p.oz === o.oz))
          out.push({ type: o.type, ox: o.ox, oz: o.oz });
      return out;
    }

    /* THE ENGINE'S SITE TEST (0x47D2E0), simplified: the footprint inside the map's placeable interior, cells 1 to
       w - 2 (tagpu_resource.c's site_known bound; [INFERRED] for the engine from 0x47D820, which answers 0 outside
       it), clear of every structure but `ignore` (an upgrade's old building), and for a geothermal plant one footprint
       cell on a vent, as one G cell over a geothermal feature suffices there. Mobile units never block: the sim has no
       collisions. */
    const interior = (t, ox, oz) => ox >= 1 && oz >= 1 && ox + fp(t)[0] < W && oz + fp(t)[1] < H;
    function siteOk(t, ox, oz, ignore) {
      const b = box(t, ox, oz);
      if (!interior(t, ox, oz)) return false;
      for (const u of structures()) if (u !== ignore && overlap(b, box(u.type, u.ox, u.oz))) return false;
      if (kind(t) === 'geo' && !vents.some(v => v.cx >= ox && v.cx < ox + b.fx && v.cz >= oz && v.cz < oz + b.fz)) return false;
      return true;
    }
    // tagpu_resource.c: site_known (the interior, every footprint cell explored) and fits (that, then the site test)
    function known(t, ox, oz) {
      const [fx, fz] = fp(t);
      if (!interior(t, ox, oz)) return false;
      for (let j = oz; j < oz + fz; j++) for (let i = ox; i < ox + fx; i++) if (!seen(i, j)) return false;
      return true;
    }
    const fits = (t, ox, oz, ignore) => known(t, ox, oz) && siteOk(t, ox, oz, ignore);
    // plans_block: a plan's footprint over the site. Without Shift a click replaces the queues of the selected builders
    // it orders, so their plans do not block it.
    function plansBlock(c, shift) {
      const bs = builders(), replaced = !shift && bs.some(u => canBuild(u, c.type)), b = box(c.type, c.ox, c.oz);
      for (const u of units) if (u.owner === 0 && !(replaced && bs.includes(u)))
        for (const o of u.orders) if (o.k === 'build' && overlap(b, box(o.type, o.ox, o.oz))) return true;
      return false;
    }

    /* ---- the rules of tagpu_resource.c, function for function: site, snapped_site, distinct, same_plan,
       find_cancel, queued, quick_choice. The metal a rule reads is -1 on unexplored ground. ---- */
    const metalAt = (i, j) => seen(i, j) ? metal[j * W + i] : -1;
    function coverage(ox, oz, fx, fz) {
      let n = 0;
      for (let j = oz; j < oz + fz; j++) for (let i = ox; i < ox + fx; i++) if (metalAt(i, j) > SM) n++;
      return n;
    }
    /* SITE: candidates are centre cells within radius r of the seed's cell; a footprint's origin is its centre minus
       half the footprint, rounded down. A mex scores its coverage, the cells above SurfaceMetal; a geothermal plant
       scores 1. The highest score wins and a tie goes to the centre nearest the raw cursor by the (candidate + 0.5)
       metric, the first in scan order on an exact tie. `legal` (Quick Build) skips a candidate the site test or a plan
       refuses unless it is the site of a queued plan, which is decided before whether its ground is free; without it
       (manual placement, the verification) a mex ignores occupancy, so an occupied deposit stays a target, and a
       geothermal plant takes the nearest site that fits. */
    function site(t, sx, sz, rx, rz, r, legal, shift) {
      const [fx, fz] = fp(t), k = kind(t), bx = cellOf(sx), bz = cellOf(sz);
      let best = 0, dist = Infinity, out = null;
      if (!k) return null;
      for (let dz = -r; dz <= r; dz++) for (let dx = -r; dx <= r; dx++) {
        const cx = bx + dx, cz = bz + dz, ox = cx - (fx >> 1), oz = cz - (fz >> 1);
        const tx = cx - rx / CELL + 0.5, tz = cz - rz / CELL + 0.5, d = tx * tx + tz * tz;
        if (ox < 0 || oz < 0 || ox + fx > W || oz + fz > H) continue;
        const n = k === 'mex' ? coverage(ox, oz, fx, fz) : 1;
        if (!n || n < best || (n === best && d >= dist)) continue;
        if (legal) {
          const taken = !fits(t, ox, oz, null), c = { type: t, kind: k, ox, oz };
          if (taken && !known(t, ox, oz)) continue;
          if (taken || plansBlock(c, shift)) {
            if (k === 'mex' && !(c.anchor = distinct(c))) continue;
            if (!queued(c)) continue;
          }
        } else if (k === 'geo' && !fits(t, ox, oz, null)) continue;
        best = n; dist = d; out = { type: t, kind: k, ox, oz };
      }
      return out;
    }
    /* SNAPPED_SITE: a mex winner away from the raw cell must win again in a search of radius max(footprint) around
       itself without legality, the raw cursor still breaking ties, or there is no snap. A deposit clipped by the radius
       therefore gives no offset mex, and occupancy cannot turn a partial target into a complete one. */
    function snappedSite(t, rx, rz, legal, shift) {
      const c = site(t, rx, rz, rx, rz, RADIUS, legal, shift);
      if (!c || c.kind !== 'mex') return c;
      const [fx, fz] = fp(t);
      if (c.ox + (fx >> 1) !== cellOf(rx) || c.oz + (fz >> 1) !== cellOf(rz)) {
        const v = site(t, (c.ox + fx / 2) * CELL, (c.oz + fz / 2) * CELL, rx, rz, Math.max(fx, fz), false, shift);
        if (!v || v.ox !== c.ox || v.oz !== c.oz) return null;
      }
      return c;
    }
    /* DISTINCT: from the first footprint cell above SurfaceMetal in scan order, the eight-connected component of cells
       above it is a deposit when it is closed: it never reaches the edge of the 129-cell arena around that cell, meets
       no unexplored neighbour, has an explored background neighbour, and does not touch both opposite map edges on
       either axis. A broad metal region, above SurfaceMetal or not, is therefore never one, and neither is any patch
       joined to it. Returns the component's anchor (its topmost, then leftmost, cell), which identifies the deposit
       for same_plan. */
    function distinct(c) {
      const [fx, fz] = fp(c.type);
      let sx = -1, sz = -1;
      for (let j = c.oz; j < c.oz + fz && sx < 0; j++) for (let i = c.ox; i < c.ox + fx; i++) if (metalAt(i, j) > SM) { sx = i; sz = j; break; }
      if (sx < 0) return null;
      const got = new Set([sz * W + sx]), todo = [[sx, sz]];
      let head = 0, boundary = false, edges = 0, ax = sx, az = sz;
      while (head < todo.length) {
        const [i, j] = todo[head++];
        if (Math.abs(i - sx) === ARENA || Math.abs(j - sz) === ARENA) return null;
        if (i === 0) edges |= 1;
        if (i === W - 1) edges |= 2;
        if (j === 0) edges |= 4;
        if (j === H - 1) edges |= 8;
        for (let dz = -1; dz <= 1; dz++) for (let dx = -1; dx <= 1; dx++) {
          const ni = i + dx, nj = j + dz, k = nj * W + ni;
          if (ni < 0 || nj < 0 || ni >= W || nj >= H || got.has(k)) continue;
          got.add(k);
          if (!seen(ni, nj)) return null;
          if (metal[k] <= SM) { boundary = true; continue; }
          todo.push([ni, nj]);
          if (nj < az || (nj === az && ni < ax)) { ax = ni; az = nj; }
        }
      }
      return boundary && (edges & 3) !== 3 && (edges & 12) !== 12 ? { ax, az } : null;
    }
    // geo_anchor: the one explored vent under a footprint; none when it touches no vent or several
    function ventAnchor(t, ox, oz) {
      const [fx, fz] = fp(t), vs = vents.filter(v => seen(v.cx, v.cz) && v.cx >= ox && v.cx < ox + fx && v.cz >= oz && v.cz < oz + fz);
      return vs.length && vs.every(v => v.cx === vs[0].cx && v.cz === vs[0].cz) ? vs[0] : null;
    }
    const centre = (t, ox, oz) => { const [fx, fz] = fp(t); return { type: t, x: (ox + fx / 2) * CELL, z: (oz + fz / 2) * CELL, fx, fz }; };
    /* SAME_PLAN: a build order is this choice's plan when it is of the same kind and on the same vent, the same
       deposit (the same anchor), or for an upgrade within 16 world units of the same place on both axes. Its building
       type need not be the one offered now. */
    function samePlan(c, o) {
      if (kind(o.type) !== c.kind) return false;
      if (c.kind === 'geo') { const a = ventAnchor(c.type, c.ox, c.oz), b = ventAnchor(o.type, o.ox, o.oz); return !!a && a === b; }
      if (c.target) { const p = centre(c.type, c.ox, c.oz), q = centre(o.type, o.ox, o.oz); return Math.abs(p.x - q.x) <= 16 && Math.abs(p.z - q.z) <= 16; }
      const a = c.anchor || (c.anchor = distinct(c)), b = distinct({ type: o.type, ox: o.ox, oz: o.oz });
      return !!a && !!b && a.ax === b.ax && a.az === b.az;
    }
    // FIND_CANCEL: a builder's first unstarted matching plan; for an upgrade only a build that is not the head and
    // follows the unstarted reclaim of the same building, which goes with it
    function findCancel(u, c) {
      for (let i = 0; i < u.orders.length; i++) {
        const o = u.orders[i], p = u.orders[i - 1];
        if (o.k !== 'build' || started(o) || !samePlan(c, o)) continue;
        if (!c.target) return { i, n: 1 };
        if (i > 0 && p.k === 'reclaim' && p.target === c.target && !(byId(p.target).reclaimed > 0)) return { i: i - 1, n: 2 };
      }
      return null;
    }
    // QUEUED: 0 no plan of the player's here, 1 a plan, 2 a plan a selected builder can cancel. Each unit's first
    // matching order decides for that unit.
    function queued(c) {
      let q = 0;
      for (const u of units) if (u.owner === 0) for (const o of u.orders) {
        if (o.k !== 'build' || !samePlan(c, o)) continue;
        q = 1;
        if (u.sel && def(u.type).workerTime > 0 && findCancel(u, c)) return 2;
        break;
      }
      return q;
    }
    /* QUICK_CHOICE, the one answer the ghost, the cursor and a right-click all use, so the preview and the command
       cannot disagree. Off in the build-menu mode and while Alt is held. Over a unit, it offers only an upgrade: the unit
       must be a finished resource building the player owns, and the replacement, centred on it, must have a strictly
       higher ExtractsMetal (or EnergyMake), fit once the old one is gone or be queued there already. Over ground, each
       offered type gets snapped_site's legal search and a mex a distinct deposit. Per kind the highest ExtractsMetal
       or EnergyMake that fits wins, the first in definition order on a tie; between a mex and a geothermal plant, the
       one whose centre is nearer the cursor. */
    function quickChoice(shift) {
      if (st.mode || st.held.has('alt')) return null;
      const { x, z } = st.cursor, target = unitAt(x, z);
      let desired = null, old = 0;
      if (target) {
        if (target.owner !== 0 || mobile(target.type) || target.built < 1 || !(desired = kind(target.type))) return null;
        old = strength(target.type);
      }
      const best = {};
      for (const t of available()) {
        const k = kind(t), value = strength(t), [fx, fz] = fp(t);
        if (!k || (desired && k !== desired) || value <= old || (best[k] && value <= best[k].strength)) continue;
        let c;
        if (target) {
          c = { type: t, kind: k, ox: Math.trunc((Math.floor(target.x) - fx * 8 + 8) / CELL), oz: Math.trunc((Math.floor(target.z) - fz * 8 + 8) / CELL), target: target.id };
          if (!fits(t, c.ox, c.oz, target) && (!known(t, c.ox, c.oz) || !queued(c))) continue;
        } else {
          c = snappedSite(t, x, z, true, shift);
          if (!c || (k === 'mex' && !(c.anchor = distinct(c)))) continue;
        }
        c.strength = value;
        c.queued = queued(c);
        if (!c.queued && plansBlock(c, shift)) continue;
        best[k] = c;
      }
      let c = best.mex || best.geo;
      if (best.mex && best.geo) {
        const d = b => { const p = centre(b.type, b.ox, b.oz); return sq(Math.floor(p.x - x)) + sq(Math.floor(p.z - z)); };
        if (d(best.geo) < d(best.mex)) c = best.geo;
      }
      if (!c) return null;
      c.queued = queued(c);
      return c;
    }

    /* ---- what the cursor is over ---- */
    const st = { cursor: { x: S.cursor[0], z: S.cursor[1] }, held: new Set(), mode: null, drag: null, caption: null, hover: null };
    function unitAt(x, z) {
      let hit = null;
      for (const u of units) {
        const [fx, fz] = fp(u.type);
        if (Math.abs(x - u.x) <= fx * CELL / 2 && Math.abs(z - u.z) <= fz * CELL / 2 && (!hit || mobile(u.type) || !mobile(hit.type))) hit = u;
      }
      return hit;
    }
    // the build-menu mode: the ghost is manual_cursor's snapped site (no legality), or where the cursor is
    function hover() {
      const { x, z } = st.cursor, shift = st.held.has('shift');
      if (st.mode) {
        const t = st.mode, [fx, fz] = fp(t), s = st.held.has('alt') ? null : snappedSite(t, x, z, false, shift);
        const ox = s ? s.ox : cellOf(x) - (fx >> 1), oz = s ? s.oz : cellOf(z) - (fz >> 1);
        return { kind: 'place', type: t, ox, oz, ok: siteOk(t, ox, oz, null) };
      }
      const c = quickChoice(shift);
      if (c) return { ...c, kind: 'quick' };
      const u = unitAt(x, z);
      return u ? { kind: 'unit', unit: u } : { kind: 'ground' };
    }

    /* ---- orders ---- */
    const assign = (u, os, shift) => { u.orders = shift ? u.orders.concat(os) : os; };
    /* a Quick Build click (click()): the builders that can build it get the order (an upgrade's reclaim first), the
       other selected builders guard them, shared out in turn, and units that cannot build keep their orders. Shift
       appends, and never adds a Guard to a helper already guarding one of them. */
    function quickOrder(c, shift) {
      const bs = builders(), cap = bs.filter(u => canBuild(u, c.type)), help = bs.filter(u => !canBuild(u, c.type));
      for (const u of cap) {
        const b = { k: 'build', type: c.type, ox: c.ox, oz: c.oz };
        assign(u, c.target ? [{ k: 'reclaim', target: c.target }, b] : [b], shift);
      }
      help.forEach((u, i) => {
        if (shift && u.orders.some(o => o.k === 'guard' && cap.some(p => p.id === o.target))) return;
        assign(u, [{ k: 'guard', target: cap[i % cap.length].id }], shift);
      });
    }
    // unqueue: each selected builder loses the plan find_cancel names, and an upgrade's reclaim with it
    function unqueue(c) {
      for (const u of builders()) { const f = findCancel(u, c); if (f) u.orders.splice(f.i, f.n); }
    }
    // a move: the selected mobile units to points SPACING apart across the cursor, in selection order
    function moveOrder(shift) {
      const ms = selected().filter(u => u.owner === 0 && mobile(u.type));
      ms.forEach((u, i) => assign(u, [{ k: 'move', x: st.cursor.x + (i - (ms.length - 1) / 2) * SPACING, z: st.cursor.z }], shift));
    }
    // a right-click: Quick Build's when it has a choice -- on a queued site Shift cancels and a plain click is consumed
    // unchanged -- otherwise the game's own: assist an unfinished building of the player's, or move
    function rightClick(shift) {
      if (st.mode) { st.mode = null; return; }
      const c = quickChoice(shift);
      if (c) {
        if (c.queued) { if (shift && c.queued === 2) unqueue(c); return; }
        return quickOrder(c, shift);
      }
      const u = unitAt(st.cursor.x, st.cursor.z);
      if (u && u.owner === 0 && !mobile(u.type) && u.built < 1) {
        for (const b of builders()) assign(b, [{ k: 'build', type: u.type, ox: u.ox, oz: u.oz }], shift);
        return;
      }
      moveOrder(shift);
    }
    // a build-menu placement: the selected builders with that button get it; without Shift the menu mode ends
    function placeClick(shift) {
      const h = hover();
      if (!h.ok) return;
      for (const u of builders()) if (canBuild(u, h.type)) assign(u, [{ k: 'build', type: h.type, ox: h.ox, oz: h.oz }], shift);
      if (!shift) st.mode = null;
    }
    function select(shift) {
      const d = st.drag, { x, z } = st.cursor;
      if (d && d.active) {
        const x0 = Math.min(d.x0, x), x1 = Math.max(d.x0, x), z0 = Math.min(d.z0, z), z1 = Math.max(d.z0, z);
        const inBox = units.filter(u => u.owner === 0 && u.x >= x0 && u.x <= x1 && u.z >= z0 && u.z <= z1);
        const pick = inBox.some(u => mobile(u.type)) ? inBox.filter(u => mobile(u.type)) : inBox;
        if (!shift) for (const u of units) u.sel = false;
        for (const u of pick) u.sel = true;
        return;
      }
      const u = unitAt(x, z);
      if (u && u.owner === 0) {
        if (shift) u.sel = !u.sel;
        else { for (const v of units) v.sel = false; u.sel = true; }
      } else if (!shift) for (const v of units) v.sel = false;
    }

    /* ---- input ---- */
    function input(ev) {
      if ('caption' in ev) st.caption = ev.caption || null;
      if ('build' in ev) { st.mode = ev.build || null; st.drag = null; }
      if (ev.move) {
        st.cursor = { x: ev.move[0], z: ev.move[1] };
        if (st.drag && sq(st.cursor.x - st.drag.x0) + sq(st.cursor.z - st.drag.z0) > DRAG * DRAG) st.drag.active = true;
      }
      if (ev.down && !st.held.has(ev.down)) {
        st.held.add(ev.down);
        const shift = st.held.has('shift');
        if (ev.down === 'lmb') { if (st.mode) placeClick(shift); else st.drag = { x0: st.cursor.x, z0: st.cursor.z, active: false }; }
        if (ev.down === 'rmb') { st.drag = null; rightClick(shift); }
      }
      if (ev.up && st.held.has(ev.up)) {
        st.held.delete(ev.up);
        if (ev.up === 'lmb' && st.drag) { select(st.held.has('shift')); st.drag = null; }
      }
      st.hover = hover();
    }

    /* ---- one tick. Units walk in a straight line at MaxVelocity (world units a tick) and stop once within
       BuildDistance of the footprint; a building fills at its builders' summed WorkerTime over its BuildTime (worker
       time a second), times the entry's pace.build, with no cost; a reclaim takes as long as building it would. Guards
       work on whatever their guarded unit works on this tick, so they are resolved after everyone else. ---- */
    function rectDist(u, b) {
      const dx = Math.max(b.ox * CELL - u.x, 0, u.x - (b.ox + b.fx) * CELL), dz = Math.max(b.oz * CELL - u.z, 0, u.z - (b.oz + b.fz) * CELL);
      return Math.sqrt(dx * dx + dz * dz);
    }
    function walk(u, tx, tz, stop) {  // a step toward (tx, tz), ending `stop` short of it; true once there
      const dx = tx - u.x, dz = tz - u.z, d = Math.sqrt(dx * dx + dz * dz);
      if (d <= stop) return true;
      const s = Math.min(def(u.type).maxVelocity, d - stop);
      u.x += dx / d * s; u.z += dz / d * s;
      return false;
    }
    function reach(u, b) {  // toward a footprint until within build distance of it; true once in range
      if (rectDist(u, b) <= def(u.type).buildDistance) return true;
      walk(u, (b.ox + b.fx / 2) * CELL, (b.oz + b.fz / 2) * CELL, 0);
      return false;
    }
    function advance() {
      const work = new Map(), jobs = new Map();
      const credit = (u, s, how) => {
        const w = work.get(s) || { build: 0, reclaim: 0 };
        w[how] += def(u.type).workerTime / def(s.type).buildTime / TPS * PACE;
        work.set(s, w); jobs.set(u, { s, how });
      };
      for (const u of units) {
        const o = u.orders[0];
        if (!o || !mobile(u.type) || o.k === 'guard') continue;
        if (o.k === 'move') { if (walk(u, o.x, o.z, 0)) u.orders.shift(); continue; }
        if (o.k === 'reclaim') {
          const s = byId(o.target);
          if (!s) { u.orders.shift(); continue; }
          if (reach(u, box(s.type, s.ox, s.oz))) credit(u, s, 'reclaim');
          continue;
        }
        let s = structureAt(o.type, o.ox, o.oz);
        if (s && s.built >= 1) { u.orders.shift(); continue; }
        if (!reach(u, box(o.type, o.ox, o.oz))) continue;
        if (!s) {
          if (!siteOk(o.type, o.ox, o.oz, null)) { u.orders.shift(); log('refused', u, o.type); continue; }
          s = place({ id: `u${nextId++}`, type: o.type, owner: u.owner, built: 0, reclaimed: 0, orders: [], sel: false, label: o.type }, o.ox, o.oz);
          log('started', s);
        }
        credit(u, s, 'build');
      }
      for (const u of units) {
        const o = u.orders[0];
        if (!o || o.k !== 'guard') continue;
        const g = byId(o.target);
        if (!g) { u.orders.shift(); continue; }
        const j = jobs.get(g);
        if (j) { if (reach(u, box(j.s.type, j.s.ox, j.s.oz))) credit(u, j.s, j.how); }
        else walk(u, g.x, g.z, FOLLOW);
      }
      for (const [s, w] of work) {
        if (w.build && s.built < 1) { s.built = Math.min(1, s.built + w.build); if (s.built >= 1) log('finished', s); }
        if (w.reclaim) { s.reclaimed = Math.min(1, s.reclaimed + w.reclaim); if (s.reclaimed >= 1) { units = units.filter(v => v !== s); log('reclaimed', s); } }
      }
      for (const u of units) {  // orders whose work is over end now, so the next one starts on the next tick
        const o = u.orders[0];
        if (!o) continue;
        if (o.k === 'build') { const s = structureAt(o.type, o.ox, o.oz); if (s && s.built >= 1) u.orders.shift(); }
        else if ((o.k === 'reclaim' || o.k === 'guard') && !byId(o.target)) u.orders.shift();
      }
    }

    const sim = { tick: 0, done: false, log: [] };
    const log = (what, u, type) => sim.log.push({ t: sim.tick, what, id: u.id, type: type || u.type, ox: u.ox, oz: u.oz });
    const script = scripted && demo ? demo.inputs.map((e, i) => [e, i]).sort((a, b) => a[0].at - b[0].at || a[1] - b[1]).map(p => p[0]) : [];
    let next = 0;
    const feed = t => { while (next < script.length && script[next].at === t) input(script[next++]); };
    const end = scripted && demo && demo.end != null ? demo.end : Infinity;  // a Try-it sim is never done

    sim.input = input;
    sim.step = () => {
      const t = sim.tick + 1;
      feed(t);
      advance();
      sim.tick = t;
      sim.done = sim.tick >= end;
      st.hover = hover();
    };
    // a unit's orders, for a check: kind, building and site, or the target's id
    sim.orders = id => { const u = byId(id); return u ? u.orders.map(o => ({ ...o })) : null; };
    sim.view = () => {
      const h = st.hover, gh = h && (h.kind === 'place' ? h : h.kind === 'quick' ? { ...h, ok: true } : null);
      const lines = [];
      for (const u of units) if (u.sel) {
        let from = [u.x, u.z];
        for (const o of u.orders) {
          const t = o.k === 'move' ? { x: o.x, z: o.z } : o.k === 'build' ? centre(o.type, o.ox, o.oz) : byId(o.target);
          if (!t) continue;
          lines.push({ from, to: [t.x, t.z], kind: o.k });
          from = [t.x, t.z];
        }
      }
      const owners = new Map();
      for (const u of units) for (const o of u.orders) if (o.k === 'build') owners.set(`${o.type}@${o.ox},${o.oz}`, u.owner);
      const shape = st.mode || gh ? 'build' : selected().some(u => u.owner === 0 && mobile(u.type)) ? 'move' : 'arrow';
      return {
        map: { w: W, h: H, cell: CELL, surfaceMetal: SM, metal: metal.slice(), fog: fog.slice(), vents: vents.map(v => ({ x: (v.cx + 0.5) * CELL, z: (v.cz + 0.5) * CELL })) },
        units: units.map(u => ({ id: u.id, type: u.type, owner: u.owner, x: u.x, z: u.z, fx: fp(u.type)[0], fz: fp(u.type)[1],
          selected: u.sel, built: u.built, reclaimed: u.reclaimed, label: u.label })),
        ghost: gh ? { ...centre(gh.type, gh.ox, gh.oz), ok: gh.ok } : null,
        queued: plans().map(p => ({ ...centre(p.type, p.ox, p.oz), owner: owners.get(`${p.type}@${p.ox},${p.oz}`) })),
        lines,
        cursor: { x: st.cursor.x, z: st.cursor.z, shape },
        keys: KEYS.filter(k => st.held.has(k)),
        drag: st.drag && st.drag.active ? { x0: st.drag.x0, z0: st.drag.z0, x1: st.cursor.x, z1: st.cursor.z } : null,
        caption: st.caption,
        pace: PACE,
        mode: st.mode ? `build:${st.mode}` : 'contextual',
      };
    };

    feed(0);
    st.hover = hover();
    return sim;
  }

  return {
    create: (entry, demo) => make(entry, demo, false),
    play: (entry, demo) => make(entry, demo, true),
  };
})();
if (typeof module !== 'undefined') module.exports = WikiSim;
