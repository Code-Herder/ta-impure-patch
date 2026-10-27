/* sim.js -- the in-game wiki's simplified simulation: a scene and its inputs in, a view of the game out.

   research/notes/ingame-wiki.md, "The simulation's contract", is its specification, and the DLL is to implement the
   same rules over the same entry files, tick for tick. The resource rules mirror the DLL's own code,
   tagpu/ddraw/src/tagpu_resource.c, function for function (research/notes/tadr-port/resource-snapping.md describes
   them); what the engine decides (the site test, orders, movement, construction) is simplified. A demo is its scene and its inputs played through this file, so a demo cannot show what
   the rules would not do, and Try it yourself is the same sim taking the player's inputs instead. The placement
   gesture (several buildings in one Shift-drag) mirrors tagpu/ddraw/src/tagpu_place.c, and its shapes are
   tagpu_placegeo.c ported line for line, integer arithmetic included.

   Positions are world units (a footprint cell is 16) and time is ticks, 30 a second. The player is owner 0. It is
   deterministic: no clock, no randomness, units visited in creation order, and only + - * / and Math.sqrt on
   doubles (all correctly rounded) plus Math.trunc (exact), so another implementation can agree with it bit for bit.

   With a sidebar layout (the running mod's left sidebar, or art/wiki/side/fixture.json) the stage also holds the
   game's 128-pixel column left of the map, x in [-128, 0): its page router, its buttons, and a demo's `build` input
   played out as the cursor's trip to the button (research/notes/ingame-wiki-sidebar-plan.md, the owner's decisions).

   Runs in a page (the global WikiSim) and under node (module.exports). */
const WikiSim = (() => {
  const CELL = 16, TPS = 30, RADIUS = 3, ARENA = 64, DRAG = 4, FOLLOW = 48, SPACING = 24;
  // tagpu_placegeo.h's caps, and tagpu_place.c's on one commit; a unit's list never holds more than PLACE_LIST_CAP
  const PLACE_MAX_SITES = 256, PLACE_SPACING_MAX = 16, PLACE_ORDER_BUDGET = 4096, PLACE_LIST_CAP = 1024;
  const KEYS = ['shift', 'alt', 'ctrl', 'z', 'x', 'lmb', 'rmb'];
  const sq = v => v * v;
  const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

  /* ---- the sidebar's layout ----
     The column is the engine's panel width, 128 game pixels, whatever the HUD scale; it is at least 480 rows (the
     PANELSIDE picture) and as tall as its tallest screen. The minimap fills its top 128 x 128 and is never
     clickable. Every bound below is one tagpu_wikisim.h's WIKILAYOUT holds: a layout breaking any of them is refused
     whole (this throws; the caller then gives the fixture), so no index read out of a layout addresses past it.

     The layout: {format: 1, width: 128, height, sides: [{name, prefix, main2, gen}], player_side,
     types: {TYPE: {pages: [screen of page 1, ...], orders: screen or null}},
     screens: {NAME: {y, w, h, gadgets: [{name, role, rect: [x, y, w, h], unit, grey, hidden}]}}}.
     A screen's y is its row in the column and a gadget's rect is in its screen's pixels. role is orders, buildtab,
     build, prev, next, stop or order, as 0x41AA00 tells them apart by name; a build button's unit is its unit type,
     or null (greyed) when the name is none; hidden marks an inactive gadget. Labels, fonts and the panel picture
     are not gadgets here: they are the art's. types holds only the types with a page (def+0x22E != 0). */
  const SIDE_W = 128, SIDE_MIN_H = 480, MINIMAP = 128;
  const SIDE_SCREENS = 48, SIDE_GADGETS = 64, SIDE_PAGES = 7, SIDE_TYPES = 64, SIDE_ROWS = 1024;
  const ROLES = ['orders', 'buildtab', 'build', 'prev', 'next', 'stop', 'order'];
  function checkLayout(L) {
    const no = what => { throw new Error(`the sidebar layout is refused: ${what}`); };
    const obj = v => v !== null && typeof v === 'object' && !Array.isArray(v);
    const name = v => typeof v === 'string' && v.length > 0;
    const int = Number.isInteger;
    if (!obj(L) || L.format !== 1) no('format is not 1');
    if (L.width !== SIDE_W) no(`width is not ${SIDE_W}`);
    if (!obj(L.screens)) no('screens is not an object');
    const names = Object.keys(L.screens);
    // SIDE_SCREENS: WIKILAYOUT's screen table
    if (names.length > SIDE_SCREENS) no(`more than ${SIDE_SCREENS} screens`);
    let rows = SIDE_MIN_H;
    for (const n of names) {
      const s = L.screens[n];
      if (!obj(s) || !int(s.y) || !int(s.w) || !int(s.h) || s.y < 0 || s.w < 1 || s.w > SIDE_W || s.h < 1)
        no(`screen ${n} has no integer y, w and h within the column`);
      // SIDE_ROWS: the composed column's texture is 128 x 1024
      if (s.y + s.h > SIDE_ROWS) no(`screen ${n} ends below row ${SIDE_ROWS}`);
      rows = Math.max(rows, s.y + s.h);
      // SIDE_GADGETS: a screen's gadget table, and view.side.state's length
      if (!Array.isArray(s.gadgets) || s.gadgets.length > SIDE_GADGETS) no(`screen ${n} has no list of at most ${SIDE_GADGETS} gadgets`);
      s.gadgets.forEach((g, i) => {
        const at = `screen ${n} gadget ${i}`;
        if (!obj(g) || !name(g.name) || !ROLES.includes(g.role)) no(`${at} has no name or no known role`);
        // a rect inside its screen: a hit or a pressed frame never reads outside the screen it belongs to
        const r = g.rect;
        if (!Array.isArray(r) || r.length !== 4 || !r.every(int) || r[0] < 0 || r[1] < 0 || r[2] < 1 || r[3] < 1 ||
            r[0] + r[2] > s.w || r[1] + r[3] > s.h) no(`${at}: the rect is not inside the screen`);
        if (g.role === 'build' && !(g.unit === null || name(g.unit))) no(`${at}: a build button's unit is not a name or null`);
        for (const k of ['grey', 'hidden']) if (k in g && typeof g[k] !== 'boolean') no(`${at}: ${k} is not a boolean`);
      });
    }
    if (L.height !== rows) no(`height is not ${rows}, the column's rows`);
    const screen = v => name(v) && own(L.screens, v);
    if (!Array.isArray(L.sides) || !L.sides.length) no('sides is not a non-empty list');
    L.sides.forEach((s, i) => {
      if (!obj(s) || !name(s.name) || typeof s.prefix !== 'string' || !screen(s.main2) || !screen(s.gen))
        no(`side ${i} has no name, prefix, or MAIN2 and GEN screens`);
    });
    if (!int(L.player_side) || L.player_side < 0 || L.player_side >= L.sides.length) no('player_side is not one of the sides');
    if (!obj(L.types)) no('types is not an object');
    // SIDE_TYPES: WIKI_MAX_DEFS, the unit types one entry can name
    if (Object.keys(L.types).length > SIDE_TYPES) no(`more than ${SIDE_TYPES} types`);
    for (const [t, T] of Object.entries(L.types)) {
      // SIDE_PAGES: the page is unit+0x110 bits 23..25, a 3-bit field, so the engine reaches at most 7 numbered pages
      if (!obj(T) || !Array.isArray(T.pages) || T.pages.length > SIDE_PAGES || !T.pages.every(screen))
        no(`type ${t} has no list of at most ${SIDE_PAGES} page screens`);
      if (!(T.orders === null || screen(T.orders))) no(`type ${t}: orders is not a screen or null`);
      // def+0x22E != 0: a type with neither a numbered page nor an orders page is not in `types`
      if (!T.pages.length && T.orders === null) no(`type ${t} has no page`);
    }
    return L;
  }

  /* ---- the build trip: a scripted `build` input played out through the sidebar ----
     FIXED BUDGET: the trip takes TRIP ticks whatever the page, the pagers or the mod; the build button goes down at
     t + TRIP_DOWN and is released at t + TRIP_UP, which is where it arms. Because the arm tick is fixed, `mode` and
     everything else in the world are the same in every mod at every tick; only the cursor and the sidebar differ.
     Row k is the trip through k pager presses (k <= 3, since the nearer way round 7 pages is at most 3): the glide
     to the pager (to the button itself for k = 0), each press held `hold` ticks then `gap` ticks before the next,
     the glide from the pager to the button, and the hover on it until TRIP_DOWN. tagpu_wikisim.h #defines the same
     table; simparity compares them. */
  const TRIP = 90, TRIP_DOWN = 45, TRIP_UP = 60;
  const TRIP_ROWS = [
    { glide: 30, hold: 0, gap: 0, back: 0, hover: 15 },
    { glide: 12, hold: 15, gap: 3, back: 12, hover: 3 },
    { glide: 9, hold: 10, gap: 3, back: 10, hover: 0 },
    { glide: 6, hold: 8, gap: 3, back: 6, hover: 0 },
  ];
  // every row ends on the button's down: a row that does not would arm at another tick in another mod
  TRIP_ROWS.forEach((r, k) => {
    if (r.glide + k * (r.hold + r.gap) + r.back + r.hover !== TRIP_DOWN) throw new Error(`trip row ${k} does not end at ${TRIP_DOWN}`);
  });

  /* ---- tagpu_placegeo.c, line for line. Sites are footprint first cells {cx, cz}. Every value is a small integer,
     so JS doubles hold the C's int64 arithmetic exactly; tdiv and fdiv are C's truncating and its floor division. ---- */
  const tdiv = (a, b) => Math.trunc(a / b);
  const fdiv = (a, b) => { const q = tdiv(a, b); return (a % b !== 0 && a < 0) ? q - 1 : q; };
  // Recoil's (|delta| + 1.4 size) / size, with the delta and the pitch in cells
  const countAlong = (d, pitch) => tdiv(10 * Math.abs(d) + 14 * pitch, 10 * pitch);
  function placeShape(kind, fx, fz, spacing, from, to, cap) {
    const px = fx + spacing, pz = fz + spacing, dx = to.cx - from.cx, dz = to.cz - from.cz;
    const xnum = countAlong(dx, px), znum = countAlong(dz, pz);
    // Recoil's `0 < delta ? size : -size`: a zero delta steps backwards, over one site only
    const sx = dx > 0 ? px : -px, sz = dz > 0 ? pz : -pz, out = [];
    const put = (cx, cz) => { if (out.length < cap) out.push({ cx, cz }); };
    if (kind === 'line' || kind === 'axis') {
      // the other axis follows the slope, each site snapped half up: floor((|D| + 2 i p d) / 2|D|)
      if (Math.abs(dx) > Math.abs(dz)) {
        for (let i = 0; i < xnum && out.length < cap; i++)
          put(from.cx + i * sx, kind === 'axis' ? from.cz : from.cz + fdiv(Math.abs(dx) + 2 * i * px * dz, 2 * Math.abs(dx)));
      } else {
        // dz == 0 here means dx == 0 too: znum is 1 and the one site is `from`
        for (let i = 0; i < znum && out.length < cap; i++)
          put(kind === 'axis' || dz === 0 ? from.cx : from.cx + fdiv(Math.abs(dz) + 2 * i * pz * dx, 2 * Math.abs(dz)), from.cz + i * sz);
      }
      return out;
    }
    if (kind === 'rect') {
      for (let j = 0; j < znum && out.length < cap; j++)
        for (let i = 0; i < xnum && out.length < cap; i++) put(from.cx + ((j & 1) ? xnum - 1 - i : i) * sx, from.cz + j * sz);
      return out;
    }
    if (kind === 'hollow') {
      if (xnum > 1 && znum > 1) {
        for (let j = 0; j < znum - 1; j++) put(from.cx, from.cz + (1 + j) * sz);                    // down the first column
        for (let j = 0; j < xnum - 1; j++) put(from.cx + (1 + j) * sx, from.cz + (znum - 1) * sz);  // along the far row
        for (let j = 0; j < znum - 1; j++) put(from.cx + (xnum - 1) * sx, from.cz + (znum - 2 - j) * sz);  // up the far column
        for (let j = 0; j < xnum - 1; j++) put(from.cx + (xnum - 2 - j) * sx, from.cz);              // back along the first row
      } else if (xnum === 1) { for (let j = 0; j < znum; j++) put(from.cx, from.cz + j * sz); }
      else { for (let j = 0; j < xnum; j++) put(from.cx + j * sx, from.cz); }
      return out;
    }
    return out;
  }
  // a ring of fx x fz sites round the cells [tx, tx+tw) x [tz, tz+th): right side down, bottom left, left side up, top
  // right, each side from a corner, the corners shared one per side; no spacing
  function placeRing(fx, fz, tx, tz, tw, th, cap) {
    const down = 1 + tdiv(th, fz), across = 1 + tdiv(tw, fx), out = [];
    const put = (cx, cz) => { if (out.length < cap) out.push({ cx, cz }); };
    for (let k = 0; k < down; k++) put(tx + tw, tz + k * fz);
    for (let k = 0; k < across; k++) put(tx + tw - fx - k * fx, tz + th);
    for (let k = 0; k < down; k++) put(tx - fx, tz + th - fz - k * fz);
    for (let k = 0; k < across; k++) put(tx + k * fx, tz - fz);
    return out;
  }
  // the footprint's first cell for a centre at world v: 0x4197D0's rounding, floor((v - 8f + 8) / 16), half up
  const origin = (v, f) => Math.floor((v - 8 * f + 8) / CELL);
  // 0x43AFC0's predicate: within 16 world units of the node on both axes, inclusive
  const within = (p, q) => Math.abs(p.x - q.x) <= 16 && Math.abs(p.z - q.z) <= 16;

  /* the entry's scene with the demo's additions: units, metal, vents and fog appended; selected and cursor
     replaced when the demo gives them */
  function merge(entry, demo) {
    const a = entry.scene, b = (demo && demo.scene) || {};
    const cat = k => [...(a[k] || []), ...(b[k] || [])];
    return { map: a.map, metal: cat('metal'), vents: cat('vents'), fog: cat('fog'),
      units: cat('units'), selected: b.selected ?? a.selected ?? [], cursor: b.cursor ?? a.cursor ?? [0, 0] };
  }

  function make(entry, demo, scripted, layout) {
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
    /* FBI canstop, def+0x245 bit 3: the FBI reader 0x42BF40 stores the key's `and 1` at 0x42C90B..0x42C936 with a
       default of 0, so a type whose FBI has no canstop has none (DISASSEMBLED); an entry's absent key reads the same */
    const canStop = t => (def(t).canstop & 1) === 1;
    // a resource building: a mex extracts metal, a geothermal plant needs a vent under its yard
    const kind = t => def(t).extracts > 0 ? 'mex' : def(t).vent ? 'geo' : null;
    const strength = t => kind(t) === 'mex' ? def(t).extracts : def(t).energyMake || 0;
    const canBuild = (u, t) => (def(u.type).builds || []).includes(t);
    const defined = t => typeof t === 'string' && own(D, t);

    /* ---- the sidebar: on or off for the sim's whole life ----
       A demo shows it when its `sidebar` says so, by default when the scene gives the player a mobile builder; Try it
       shows it whenever the scene does, whatever the demo said. Off, nothing below the map rules runs: no column,
       the cursor unclamped, and `build` arms at once. */
    if (scripted && demo && 'sidebar' in demo && typeof demo.sidebar !== 'boolean')
      throw new Error(`demo ${demo.id}: sidebar is not a boolean`);
    const ON = !!layout && (() => {
      const builder = S.units.some(s => (s.owner ?? 0) === 0 && mobile(s.type) && def(s.type).workerTime > 0);
      return scripted && demo && typeof demo.sidebar === 'boolean' ? demo.sidebar : builder;
    })();
    const LAY = ON ? layout : null;
    const typePages = t => ON && own(LAY.types, t) ? LAY.types[t] : null;
    const npages = t => { const T = typePages(t); return T ? T.pages.length : 0; };

    /* ---- the grid ---- */
    const cellOf = v => Math.floor(v / CELL);
    const seen = (cx, cz) => fog[cz * W + cx] !== 2;
    const overlap = (a, b) => a.ox < b.ox + b.fx && b.ox < a.ox + a.fx && a.oz < b.oz + b.fz && b.oz < a.oz + a.fz;
    const box = (t, ox, oz) => ({ ox, oz, fx: fp(t)[0], fz: fp(t)[1] });

    /* ---- units. Each faces along a unit vector (hx, hz); walk says why a vector. ---- */
    let units = [], nextId = 1;
    /* a unit joins the world. With the sidebar it carries its page state, unit+0x110: bp (bit 22, a numbered page
       is shown rather than the orders page) and page (bits 23..25, 1..n). 0x485CD5 creates a unit whose type has
       numbered pages with bit 22 set and page 1; any other unit has neither. */
    function enter(u) {
      if (ON) { const n = npages(u.type); u.bp = n >= 1; u.page = n >= 1 ? 1 : 0; }
      units.push(u);
      return u;
    }
    // a scene unit's "facing": [x, z] as a unit vector, (0, 1) when absent; tagpu_wikidata.c refuses what this throws on
    function facing(f) {
      if (f == null) return [0, 1];
      const two = Array.isArray(f) && f.length === 2 && typeof f[0] === 'number' && typeof f[1] === 'number';
      const d = two ? Math.sqrt(f[0] * f[0] + f[1] * f[1]) : 0;
      if (!(d > 0 && d < Infinity)) throw new Error(`a facing is not two numbers with a length: ${JSON.stringify(f)}`);
      return [f[0] / d, f[1] / d];
    }
    function add(type, owner, x, z, built, id, label, [hx, hz]) {
      const [fx, fz] = fp(type), u = { id: id || `u${nextId++}`, type, owner, built, reclaimed: 0, orders: [], sel: false, label: label || type };
      if (mobile(type)) { Object.assign(u, { x, z, hx, hz }); return enter(u); }
      return place(u, Math.round(x / CELL - fx / 2), Math.round(z / CELL - fz / 2));
    }
    /* a structure at its site. It faces (0, 1), toward the viewer, whatever its scene gives: TA's default heading
       0x8000 is the centre of the spawn's 0x8000 - BuildAngle/2 + rand(BuildAngle) (0x485A40, tagpu_native.c
       ghost_one), and the simulation has no randomness. */
    function place(u, ox, oz) {
      const [fx, fz] = fp(u.type);
      Object.assign(u, { ox, oz, x: (ox + fx / 2) * CELL, z: (oz + fz / 2) * CELL, hx: 0, hz: 1 });
      return enter(u);
    }
    const byId = id => units.find(u => u.id === id);
    const structures = () => units.filter(u => !mobile(u.type));
    const structureAt = (t, ox, oz) => units.find(u => u.type === t && u.ox === ox && u.oz === oz);
    for (const s of S.units) {
      const u = add(s.type, s.owner ?? 0, s.at[0], s.at[1], s.built ?? 1, s.id, s.label, facing(s.facing));
      u.orders = (s.orders || []).slice(0, PLACE_LIST_CAP).map(o => ({ k: 'move', x: o.move[0], z: o.move[1] }));
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
      const { x, z } = mapCursor(), target = unitAt(x, z);
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
    /* gesture: the placement gesture's press site while it lives; shiftPlaced: main+0x2CC6 bit 5, a placement made in
       this Shift hold, so releasing Shift disarms; spacing: empty cells between neighbours, per building type;
       place: the last evaluation of the gesture (tagpu_place.c's s_eval) */
    const st = { cursor: { x: S.cursor[0], z: S.cursor[1] }, held: new Set(), mode: null, drag: null, caption: null, hover: null,
      gesture: null, shiftPlaced: false, spacing: {}, place: null };
    function unitAt(x, z) {
      let hit = null;
      for (const u of units) {
        const [fx, fz] = fp(u.type);
        if (Math.abs(x - u.x) <= fx * CELL / 2 && Math.abs(z - u.z) <= fz * CELL / 2 && (!hit || mobile(u.type) || !mobile(hit.type))) hit = u;
      }
      return hit;
    }

    /* ---- the sidebar's rules; none of them runs with the sidebar off ---- */
    const MW = W * CELL, MH = H * CELL;
    /* where the cursor is: the column left of x = 0, the map, or the dead band under a map shorter than the column.
       The map is closed at its far edges because the stage clamps a pointer beyond them onto exactly W x 16 or
       H x 16, which the map rules have always read as the map. */
    function over() {
      if (!ON) return 'map';
      const { x, z } = st.cursor;
      return x < 0 ? 'side' : x <= MW && z <= MH ? 'map' : 'dead';
    }
    // the cursor the map rules read: clamped onto the map, so a drag released over the column selects up to its edge
    const mapCursor = () => ON ? { x: Math.min(Math.max(st.cursor.x, 0), MW), z: Math.min(Math.max(st.cursor.z, 0), MH) } : st.cursor;
    /* screen: the routed screen's name; shown: the shown unit (main+0x37E9C); cap: the gadget a press captured and the
       button that pressed it (gi+0x64, one slot, so a second press takes it over); downOn: where each mouse button
       went down, which decides whether its release runs the map rules; locked: the type a scripted build could not
       reach in this layout */
    const sb = { screen: null, shown: null, cap: null, downOn: {}, locked: null };
    const screenOf = n => LAY.screens[n];
    // a point of the stage in the column's own pixels is (x + 128, z); the minimap's 128 x 128 there takes no click
    const onMinimap = (x, z) => x + SIDE_W >= 0 && x + SIDE_W < MINIMAP && z >= 0 && z < MINIMAP;
    /* the engine's hit tests (the pump's 0x4AA1F6..0x4AA216, the button handler's 0x4A6B79..0x4A6B95 on the press
       and 0x4A6E84..0x4A6EAC on the release) take the whole pixels x .. x + w - 1 and y .. y + h - 1, DISASSEMBLED.
       The cursor here is a double, and the pixel it stands in is its floor, so that is half-open on the doubles. */
    const inRect = ([gx, gy, gw, gh], px, pz) => px >= gx && px < gx + gw && pz >= gy && pz < gy + gh;
    const inside = (s, g, x, z) => x < 0 && !onMinimap(x, z) && inRect(g.rect, x + SIDE_W, z - s.y);
    /* OWNER QUESTION Q-O1, the recommended answer: a build button is live only when the entry defines its unit and the
       shown builder's list holds it, and greyed otherwise, so a button "comes alive" when an entry adds its unit
       (decision 7's principle). Drawn normal and inert instead would be this function answering true for any named
       unit and act() refusing the undefined ones. */
    const buildLive = (g, shown) => !g.grey && defined(g.unit) && !!shown && canBuild(shown, g.unit);
    /* A gadget's state as the stage draws it: -1 not drawn, 0 normal, 1 pressed or the current tab, 2 greyed (frame 2
       at rest, GUI_ButtonDraw 0x4A5F40). The tabs follow bit 22 as 0x41A120 sets them; every order button but STOP is
       greyed (decision 7: an entry that needs an order adds it to the simulation). */
    function gadgetState(g, shown, pressed) {
      if (g.hidden) return -1;
      const n = shown ? npages(shown.type) : 0;
      switch (g.role) {
        case 'orders': return n >= 1 ? (shown.bp ? 0 : 1) : 2;
        case 'buildtab': return n >= 1 ? (shown.bp ? 1 : 0) : 2;
        case 'build': return buildLive(g, shown) ? +pressed : 2;
        /* STOP is live while some selected unit of the player's has canstop: the router 0x41B2E0 ORs def+0x245 bit 3
           over the controlled player's selected units (0x41B4AC) and 0x41A120 greys STOP when the OR is clear
           (0x41A2F3), DISASSEMBLED. The router's other branch, taken while the shown unit lives, keeps the flags it
           finds; they are this selection's, because a selection change clears the shown unit and so routes through
           the branch that computes them. 0x41A120 never un-greys: that a route to the screen already shown reloads it,
           so a STOP greyed for one selection is live again for the next, is [INFERRED]. */
        case 'stop': return selected().some(u => u.owner === 0 && canStop(u.type)) ? +pressed : 2;
        case 'order': return 2;
        default: return +pressed;
      }
    }
    const live = s => s === 0 || s === 1;
    // a tab (commonattribs 0x10) acts on its press; every other button on its release
    const pressActs = g => g.role === 'orders' || g.role === 'buildtab';
    /* THE GADGET A PRESS AT (x, z) LANDS ON, as the GUI pump 0x4A9FD0 finds it, DISASSEMBLED: it offers the press to
       the gadgets in file order and stops at the first whose handler acts. An inactive gadget is skipped
       (0x4AA1CE..0x4AA1D3) and a greyed one's handler refuses at once (0x4A6B10..0x4A6B19), so neither takes it. A
       button that acts on the release takes the capture and lets the scan go on (0x4A6BAC, 0x49FC50 at 0x4A6BB3), so
       where buttons overlap the LAST one holds it. A tab acts during its own turn: it drops the capture an earlier
       button took (0x4A6C28) and returns 1 (0x4A6CD4), which ends the scan (0x4AA549..0x4AA54E), so the FIRST tab
       holding the point wins over any button before or after it. -1: none. */
    function gadgetAt(name, shown, x, z) {
      const s = screenOf(name);
      let last = -1;
      for (let i = 0; i < s.gadgets.length; i++) {
        const g = s.gadgets[i];
        if (!live(gadgetState(g, shown, false)) || !inside(s, g, x, z)) continue;
        if (pressActs(g)) return i;
        last = i;
      }
      return last;
    }
    const sideStates = () => {
      const s = screenOf(sb.screen), { x, z } = st.cursor;
      return s.gadgets.map((g, i) => gadgetState(g, sb.shown, !!sb.cap && sb.cap.i === i && inside(s, g, x, z)));
    };
    /* THE PAGE ROUTER, 0x41B2E0, run from the frame loop at 0x4969B4: nothing of the player's selected shows the side's
       MAIN2; EXACTLY ONE selected unit whose type has pages shows its numbered page while bit 22 is set, else its
       orders page (<NAME>0.GUI) when the type has one; anything else, two builders of one type included, shows the
       side's GEN, with the unit shown when there is exactly one. GEN and MAIN2 are the player's side's. The sim routes
       after every input and every tick, where the engine re-routes a frame or more later: a known difference of at
       most a frame. A screen change drops the capture, as the engine's page swap reloads the screen and its gadgets. */
    function route() {
      const sel = selected().filter(u => u.owner === 0), side = LAY.sides[LAY.player_side];
      const u = sel.length === 1 ? sel[0] : null, T = u && typePages(u.type);
      // u.page is 1..n for a type with n pages: enter() starts it at 1 and act() is its only other writer, wrapping
      // within 1..n, so T.pages[u.page - 1] is always a page
      const screen = !sel.length ? side.main2 : T && u.bp && u.page > 0 ? T.pages[u.page - 1] : T && T.orders ? T.orders : side.gen;
      if (screen !== sb.screen) sb.cap = null;
      sb.screen = screen;
      sb.shown = u;
    }
    /* A BUTTON ACTS ON THE RELEASE INSIDE IT, as the engine's: 0x4A6AE0 returns 1 for a release inside a captured
       button, 0x4A9E98 stores its index and 0x4AA797 calls the screen's handler 0x41AA00. The tabs, which act on the
       press, come here too. PREV and NEXT step the shown unit's page as 0x41BF10 and 0x41BDE0 do, wrapping within
       1..n, and set bit 22; ORDERS and BUILD clear and set bit 22 (0x41C180). A build button arms its building, the
       old `build` input's effect; a mobile unit's button would queue it at a factory (0x419B00), which no entry has,
       so it does nothing.

       STOP is 0x419BE0's STOP branch (0x419CB6..0x419CF2, called from 0x41AA00 at 0x41ABCC), DISASSEMBLED: it writes
       main+0x2CC3 = 1 (0x419CC2), which replaces an armed build's mode 0x0E, so it disarms; then the order issuer
       0x48CF30 gives every selected unit of the controlled player the STOP order through 0x43AFC0 (0x48D1F4), which
       replaces its orders. The path tests no canstop, so a unit without it is stopped along with the selection's
       others. With Shift held the issuer passes shift = 1 to 0x43AFC0, whose effect on STOP is not established; the
       sim empties the orders either way [INFERRED]. */
    function act(g) {
      const u = sb.shown, n = u ? npages(u.type) : 0;
      if ((g.role === 'prev' || g.role === 'next') && n) {
        u.page = g.role === 'next' ? (u.page === n ? 1 : u.page + 1) : (u.page < 2 ? n : u.page - 1);
        u.bp = true;
      } else if (g.role === 'orders' && n) u.bp = false;
      else if (g.role === 'buildtab' && n) u.bp = true;
      else if (g.role === 'stop') {
        disarm();
        for (const v of selected()) if (v.owner === 0) v.orders = [];
      }
      else if (g.role === 'build' && defined(g.unit) && !mobile(g.unit)) { disarm(); st.mode = g.unit; st.drag = null; }
      route();
    }
    /* a press over the column: a tab drops the capture (0x4A6C28) and acts now; any other button is captured, either
       mouse button alike (0x41AA00 never tests which one arms a structure) */
    function pressSide(b) {
      const { x, z } = st.cursor, i = gadgetAt(sb.screen, sb.shown, x, z);
      if (i < 0) return;
      const g = screenOf(sb.screen).gadgets[i];
      if (pressActs(g)) { sb.cap = null; act(g); }
      else sb.cap = { i, b };
    }
    // the release of the captured button: inside its gadget, still live, it acts; outside, nothing
    function releaseSide() {
      const c = sb.cap, s = screenOf(sb.screen), g = s.gadgets[c.i], { x, z } = st.cursor;
      sb.cap = null;
      if (live(gadgetState(g, sb.shown, false)) && inside(s, g, x, z)) act(g);
    }

    /* THE BUILD TRIP. A data input {build: T} at tick t arms nothing itself: it becomes generated inputs, a glide, the
       pager presses and the button's own press, that go through exactly the path the player's clicks take in Try it.
       They are made when tick t is fed, from the state at t, and the arm is the release at t + TRIP_UP whatever the
       route (TRIP_ROWS says why it is fixed). */
    const gen = [];
    let genNext = 0, feeding = 0;
    const centreOf = (s, g) => ({ x: -SIDE_W + g.rect[0] + g.rect[2] / 2, z: s.y + g.rect[1] + g.rect[3] / 2 });
    /* the plan: the shown unit's first page holding a live T button, the pagers the nearer way round (a tie goes to
       NEXT), and each press's point. Each point is the pager on the page it is pressed on, and every point is
       checked to land on its gadget, so the release at t + TRIP_UP arms T in any layout this accepts. null: T
       cannot be reached here. */
    function planTrip(T) {
      const u = sb.shown, TP = u && typePages(u.type), n = TP ? TP.pages.length : 0;
      // no data input presses a tab, so bp is set in a scripted demo; a build met on the orders page is locked
      if (!n || !u.bp) return null;
      let p = 0, gi = -1;
      for (let k = 1; k <= n && !p; k++) {
        const i = screenOf(TP.pages[k - 1]).gadgets.findIndex(g => g.role === 'build' && g.unit === T && live(gadgetState(g, u, false)));
        if (i >= 0) { p = k; gi = i; }
      }
      if (!p) return null;
      const c = u.page, kNext = ((p - c) % n + n) % n, kPrev = ((c - p) % n + n) % n;
      const dir = kNext <= kPrev ? 'next' : 'prev', k = Math.min(kNext, kPrev), presses = [];
      for (let j = 0, page = c; j < k; j++) {
        const name = TP.pages[page - 1], s = screenOf(name), i = s.gadgets.findIndex(g => g.role === dir && live(gadgetState(g, u, false)));
        if (i < 0) return null;
        const at = centreOf(s, s.gadgets[i]);
        if (gadgetAt(name, u, at.x, at.z) !== i) return null;
        presses.push(at);
        page = dir === 'next' ? (page === n ? 1 : page + 1) : (page < 2 ? n : page - 1);
      }
      const s = screenOf(TP.pages[p - 1]), at = centreOf(s, s.gadgets[gi]);
      if (gadgetAt(TP.pages[p - 1], u, at.x, at.z) !== gi) return null;
      return { k, presses, button: at };
    }
    /* THE GLIDE, exact in both languages: step i of n from a to b is a + trunc((b - a) * (i*i*(3n - 2i)) / (n*n*n)),
       each axis, evaluated in that order. Step n is b itself: the formula reaches b only when b - a is a whole
       number, and the cursor must stand exactly on the button and exactly back where it started. */
    const glideAt = (a, b, i, n) => i === n ? b : a + Math.trunc((b - a) * (i * i * (3 * n - 2 * i)) / (n * n * n));
    function trip(T) {
      const t = feeding, plan = planTrip(T);
      /* OWNER QUESTION Q-O2, the recommended answer: a build this layout cannot reach stops the demo here, holding the
         frame under the entry's "Turned off in this mod" badge (decision 4), rather than playing on unarmed */
      if (!plan) { sb.locked = T; return; }
      const a = { x: st.cursor.x, z: st.cursor.z }, R = TRIP_ROWS[plan.k], B = plan.button;
      const push = (dt, ev) => gen.push({ at: t + dt, ev });
      const glide = (from, to, n, t0) => {
        for (let i = 1; i <= n; i++) push(t0 + i, { move: [glideAt(from.x, to.x, i, n), glideAt(from.z, to.z, i, n)] });
      };
      let cur = a;
      if (plan.k) {
        glide(a, plan.presses[0], R.glide, 0);
        cur = plan.presses[0];
        plan.presses.forEach((p, j) => {
          const d = R.glide + j * (R.hold + R.gap);
          // a mod whose pages move the pager: the cursor steps onto it, before the press of the same tick
          if (p.x !== cur.x || p.z !== cur.z) push(d, { move: [p.x, p.z] });
          push(d, { down: 'lmb' });
          push(d + R.hold, { up: 'lmb' });
          cur = p;
        });
      }
      glide(cur, B, plan.k ? R.back : R.glide, plan.k ? R.glide + plan.k * (R.hold + R.gap) : 0);
      push(TRIP_DOWN, { down: 'lmb' });
      push(TRIP_UP, { up: 'lmb' });
      glide(B, a, TRIP - TRIP_UP, TRIP_UP);
    }
    /* THE DATA RULE, refused here as tagpu_wikidata.c's loader refuses it: with the sidebar on, a build names a type;
       nothing but a caption stands in its trip, the ticks (t, t + TRIP), nor after it in tick t; the demo lasts the
       whole trip; and the left button is up when it starts, as the trip's own presses need it. */
    function checkTrips(script, end) {
      const captionOnly = e => Object.keys(e).every(k => k === 'at' || k === 'caption');
      let lmb = false;
      script.forEach((e, i) => {
        if ('build' in e) {
          const at = `demo ${demo.id}: the build at tick ${e.at}`;
          if (typeof e.build !== 'string' || !e.build) throw new Error(`${at} names no type`);
          if (!Object.keys(e).every(k => k === 'at' || k === 'build' || k === 'caption')) throw new Error(`${at} carries another input`);
          if (lmb) throw new Error(`${at} starts with the left button held`);
          const f = script.slice(i + 1).find(f => f.at < e.at + TRIP && !captionOnly(f));
          if (f) throw new Error(`${at}: an input at tick ${f.at} stands in its trip, which ends at ${e.at + TRIP}`);
          if (!(end >= e.at + TRIP)) throw new Error(`${at}: the demo ends at ${end}, inside its trip`);
        }
        if (e.down === 'lmb') lmb = true;
        if (e.up === 'lmb') lmb = false;
      });
    }

    // the build-menu mode: the ghost is manual_cursor's snapped site (no legality), or where the cursor is, as the build
    // cursor 0x4197D0 rounds it. None while the cursor is off the map, though the gesture's evaluation goes on.
    function hover() { const h = mapHover(); return over() === 'map' ? h : null; }
    function mapHover() {
      const { x, z } = mapCursor(), shift = st.held.has('shift');
      st.place = evaluate();
      if (st.mode) {
        const t = st.mode, [fx, fz] = fp(t), s = st.held.has('alt') ? null : snappedSite(t, x, z, false, shift);
        const ox = s ? s.ox : origin(x, fx), oz = s ? s.oz : origin(z, fz);
        return { kind: 'place', type: t, ox, oz, ok: siteOk(t, ox, oz, null) };
      }
      const c = quickChoice(shift);
      if (c) return { ...c, kind: 'quick' };
      const u = unitAt(x, z);
      return u ? { kind: 'unit', unit: u } : { kind: 'ground' };
    }

    /* ---- the placement gesture, tagpu_place.c function for function ---- */
    // the engine's cursor site: the build cursor's rectangle, snapped as the ghost is
    function cursorSite() {
      const t = st.mode, [fx, fz] = fp(t), { x, z } = mapCursor();
      const s = st.held.has('alt') ? null : snappedSite(t, x, z, false, st.held.has('shift'));
      return s ? { cx: s.ox, cz: s.oz } : { cx: origin(x, fx), cz: origin(z, fz) };
    }
    const siteCentre = (t, s) => centre(t, s.cx, s.cz);
    const orderAt = o => centre(o.type, o.ox, o.oz);
    // TOGGLES: some selected builder has a build order within 16 world units of the site; it compares no building type
    const toggles = (t, s) => builders().some(u => u.orders.some(o => o.k === 'build' && within(siteCentre(t, s), orderAt(o))));
    /* TAKE_OFFS, the take-off rule, the one place a site's action is decided: the preview's colours and the commit's
       removals and appends are both read from what it leaves, so for the same queues the preview shows what the release
       does. A ring takes nothing off: a site a Shift-click would toggle is 'remove', and left out. Every other shape is
       the Shift-click per site against each builder's queue as it stood before the gesture, each queued order taken off
       by at most one site: per builder, every site not refused first takes the build order at its own position, then
       each site that took none takes the first build order within 16 world units that no site has taken -- both in
       shape order, each in queue order. A site that takes an order off from some builder is 'remove'. Own positions go
       first over the whole shape because neighbouring 1 x 1 sites are 16 world units apart: whichever end a drag over a
       queued row starts from, every order of the row is taken off by the site that stands on it. Returns, per selected
       builder, the site that takes each of its orders off (-1 for none); for a ring, no order. */
    function takeOffs(t, sites, ring) {
      const bs = builders();
      if (ring) {
        for (const s of sites) if (s.state !== 'refused' && toggles(t, s)) s.state = 'remove';
        return bs.map(() => []);
      }
      return bs.map(u => {
        const by = u.orders.map(() => -1);
        for (const own of [true, false]) sites.forEach((s, i) => {
          if (s.state === 'refused' || (!own && by.includes(i))) return;
          const at = siteCentre(t, s);
          const k = u.orders.findIndex((o, j) => o.k === 'build' && by[j] < 0 &&
            (own ? orderAt(o).x === at.x && orderAt(o).z === at.z : within(at, orderAt(o))));
          if (k >= 0) { by[k] = i; s.state = 'remove'; }
        });
        return by;
      });
    }
    // CLASSIFY: refused where the site test refuses, else 'place', and then the take-off rule's 'remove'. The sites, and
    // per selected builder the site that takes each order off
    function classify(t, cells, ring) {
      const sites = cells.map(s => ({ cx: s.cx, cz: s.cz, state: siteOk(t, s.cx, s.cz, null) ? 'place' : 'refused' }));
      return { sites, by: takeOffs(t, sites, ring) };
    }
    /* RING_TARGET: the unit under the cursor, any unit (0x48CD80), else a selected builder's queued build whose
       footprint holds the cursor's cell, first in selection order then queue order. As cells. */
    function ringTarget() {
      const { x, z } = mapCursor(), u = unitAt(x, z);
      if (u) { const [tw, th] = fp(u.type); return { tx: origin(u.x, tw), tz: origin(u.z, th), tw, th }; }
      const px = cellOf(x), pz = cellOf(z);
      for (const b of builders()) for (const o of b.orders) {
        if (o.k !== 'build') continue;
        const [tw, th] = fp(o.type);
        if (px >= o.ox && px < o.ox + tw && pz >= o.oz && pz < o.oz + th) return { tx: o.ox, tz: o.oz, tw, th };
      }
      return null;
    }
    /* EVALUATE: the shape the cursor shows, with the modifiers read live. Nothing without Shift (the release would order
       nothing); Shift + Ctrl over a ring target is a ring, before any drag shape; otherwise the gesture's shape from the
       press site to the cursor site: Alt a rectangle, Ctrl held to its axis or hollow. Cut to the order budget; a ring
       leaves out the sites that would take an order off. null: the engine's single square stands. */
    function evaluate() {
      if (!st.mode) return null;
      const shift = st.held.has('shift'), ctrl = st.held.has('ctrl'), alt = st.held.has('alt');
      if (!shift || (!ctrl && !st.gesture)) return null;
      const t = st.mode, [fx, fz] = fp(t), r = ctrl ? ringTarget() : null;
      let kind, cells;
      if (r) { kind = 'ring'; cells = placeRing(fx, fz, r.tx, r.tz, r.tw, r.th, PLACE_MAX_SITES); }
      else if (st.gesture) {
        kind = alt ? (ctrl ? 'hollow' : 'rect') : (ctrl ? 'axis' : 'line');
        cells = placeShape(kind, fx, fz, st.spacing[t] || 0, st.gesture, cursorSite(), PLACE_MAX_SITES);
      } else return null;
      cells = cells.slice(0, Math.floor(PLACE_ORDER_BUDGET / (builders().length || 1)));
      let { sites } = classify(t, cells, kind === 'ring');
      if (kind === 'ring') sites = sites.filter(s => s.state !== 'remove');
      return { kind, type: t, sites };
    }
    /* COMMIT: the stock Shift-clicks of the sites, judged against each builder's queue as it stood before the gesture,
       the sites classified afresh by the same take-off rule the preview's colours come from. Per builder, phase 1: the
       orders the rule gave a site are taken off, and each of those sites adds nothing for that builder; a ring takes
       nothing off, its removing sites skipped. Phase 2: the rest are appended in the shape's order while the queue is
       shorter than PLACE_LIST_CAP. Every selected builder gets them, its build list not consulted, as 0x419670 orders.
       A changed queue sets the Shift-placement bit, so releasing Shift disarms. */
    function commit(p) {
      const t = p.type, ring = p.kind === 'ring', { sites, by } = classify(t, p.sites, ring);
      let changed = 0;
      builders().forEach((u, b) => {
        const toggled = sites.map((s, i) => ring ? s.state === 'remove' : by[b].includes(i));
        for (let j = by[b].length; j-- > 0;) if (by[b][j] >= 0) { u.orders.splice(j, 1); changed++; }
        sites.forEach((s, i) => {
          if (s.state === 'refused' || toggled[i] || u.orders.length >= PLACE_LIST_CAP) return;
          u.orders.push({ k: 'build', type: t, ox: s.cx, oz: s.cz }); changed++;
        });
      });
      if (changed) st.shiftPlaced = true;
    }
    /* THE RELEASE: Shift up first drops the gesture and the building stays armed; a one-site gesture ending at the
       cursor is the stock Shift-click itself; anything else commits the last evaluation. */
    function release() {
      st.gesture = null;
      if (!st.held.has('shift')) return;
      const p = st.place, cur = cursorSite();
      if (p && p.kind !== 'ring' && p.sites.length === 1 && p.sites[0].cx === cur.cx && p.sites[0].cz === cur.cz) return placeClick(true);
      if (p) commit(p);
    }

    /* ---- orders ---- */
    // leaving the build-menu mode ends the gesture and clears the Shift-placement bit with it
    const disarm = () => { st.mode = null; st.gesture = null; st.shiftPlaced = false; };
    /* a click's orders for one unit: they replace its list, or with Shift are appended to it -- all of them or, when
       they would take the list past PLACE_LIST_CAP, none, so an upgrade's reclaim never goes in without its build */
    const assign = (u, os, shift) => {
      if (!shift) u.orders = os;
      else if (u.orders.length + os.length <= PLACE_LIST_CAP) u.orders = u.orders.concat(os);
    };
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
      const c = mapCursor();
      ms.forEach((u, i) => assign(u, [{ k: 'move', x: c.x + (i - (ms.length - 1) / 2) * SPACING, z: c.z }], shift));
    }
    // a right-click: Quick Build's when it has a choice -- on a queued site Shift cancels and a plain click is consumed
    // unchanged -- otherwise the game's own: assist an unfinished building of the player's, or move
    function rightClick(shift) {
      if (st.mode) { disarm(); return; }
      const c = quickChoice(shift);
      if (c) {
        if (c.queued) { if (shift && c.queued === 2) unqueue(c); return; }
        return quickOrder(c, shift);
      }
      const { x, z } = mapCursor(), u = unitAt(x, z);
      if (u && u.owner === 0 && !mobile(u.type) && u.built < 1) {
        for (const b of builders()) assign(b, [{ k: 'build', type: u.type, ox: u.ox, oz: u.oz }], shift);
        return;
      }
      moveOrder(shift);
    }
    /* a build-menu placement, stock's click: the selected builders with that button get it; without Shift the menu mode
       ends. With Shift (0x43AFC0) the first build order of the builder's within 16 world units of the site is taken off
       instead of adding one, and the placement sets the Shift-placement bit: releasing Shift then disarms. */
    function placeClick(shift) {
      const h = mapHover();
      if (!h.ok) return;
      const at = centre(h.type, h.ox, h.oz);
      for (const u of builders()) if (canBuild(u, h.type)) {
        const k = shift ? u.orders.findIndex(o => o.k === 'build' && within(at, centre(o.type, o.ox, o.oz))) : -1;
        if (k >= 0) u.orders.splice(k, 1);
        else assign(u, [{ k: 'build', type: h.type, ox: h.ox, oz: h.oz }], shift);
      }
      if (!shift) disarm();
      else st.shiftPlaced = true;
    }
    function select(shift) {
      const d = st.drag, { x, z } = mapCursor();
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
    /* Spacing follows the design (bar-placement-design.md section 6 and 17.1) until the game's key handler lands: with a
       building armed and Alt down, Z adds one empty cell between neighbours and X takes one away, 0..16, kept per
       building type; every down steps, an auto-repeat included, and the next evaluation shows it. */
    function stepSpacing(k) {
      if (!st.mode || !st.held.has('alt')) return;
      st.spacing[st.mode] = Math.max(0, Math.min(PLACE_SPACING_MAX, (st.spacing[st.mode] || 0) + (k === 'z' ? 1 : -1)));
    }
    /* A scripted demo with the sidebar on plays a `build` out as its trip; Try it, and a demo without the sidebar, arm
       at once (the wiki never sends `build`, and the rule cases use it as their shortcut). */
    const TRIPS = scripted && ON;
    function input(ev) {
      if ('caption' in ev) st.caption = ev.caption || null;
      if ('build' in ev) {
        if (TRIPS) trip(ev.build);
        else { disarm(); st.mode = ev.build || null; st.drag = null; }
      }
      if (ev.move) {
        st.cursor = { x: ev.move[0], z: ev.move[1] };
        const c = mapCursor();
        if (st.drag && sq(c.x - st.drag.x0) + sq(c.z - st.drag.z0) > DRAG * DRAG) st.drag.active = true;
      }
      if (ev.down === 'z' || ev.down === 'x') { st.held.add(ev.down); stepSpacing(ev.down); }
      else if (ev.down && !st.held.has(ev.down)) {
        st.held.add(ev.down);
        const shift = st.held.has('shift'), where = over();
        // a mouse button's press over the column is the column's; over the dead band it is nobody's
        if (ev.down === 'lmb' || ev.down === 'rmb') sb.downOn[ev.down] = where;
        if (where === 'side' && (ev.down === 'lmb' || ev.down === 'rmb')) pressSide(ev.down);
        else if (where === 'map') {
          // with a building armed a left press carrying Shift starts a gesture at the cursor site instead of placing
          if (ev.down === 'lmb') {
            if (st.mode && shift) st.gesture = cursorSite();
            else if (st.mode) placeClick(false);
            else { const c = mapCursor(); st.drag = { x0: c.x, z0: c.z, active: false }; }
          }
          if (ev.down === 'rmb') { st.drag = null; rightClick(shift); }
        }
      }
      if (ev.up && st.held.has(ev.up)) {
        st.held.delete(ev.up);
        // a release runs the map rules only for a press that went down on the map
        if (sb.cap && sb.cap.b === ev.up) releaseSide();
        else if (sb.downOn[ev.up] === 'map') {
          if (ev.up === 'lmb' && st.gesture) release();
          else if (ev.up === 'lmb' && st.drag) { select(st.held.has('shift')); st.drag = null; }
        }
        // 0x4992D8: Shift found up after a placement in this hold disarms, as after a stock Shift-click
        if (ev.up === 'shift') { if (st.shiftPlaced) disarm(); st.shiftPlaced = false; }
      }
      if (ON) route();
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
    /* A step toward (tx, tz), ending `stop` short of it; true once there. The step turns the unit to face along it,
       at once: the simulation has no turn rate.

       THE HEADING IS A VECTOR, NOT AN ANGLE. An angle would come from Math.atan2, which no standard rounds correctly,
       so its last bit differs between JavaScript engines and C libraries, and tagpu_wikisim.c would drift from this
       file. The step's own dx / d and dz / d are + - * / and sqrt, each rounded once, so both simulations hold the
       same doubles. */
    function walk(u, tx, tz, stop) {
      const dx = tx - u.x, dz = tz - u.z, d = Math.sqrt(dx * dx + dz * dz);
      if (d <= stop) return true;
      const s = Math.min(def(u.type).maxVelocity, d - stop);
      u.hx = dx / d; u.hz = dz / d;
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
      // a builder in range works on s, and turns at once to face its centre (unchanged when it stands on the centre)
      const credit = (u, s, how) => {
        const dx = s.x - u.x, dz = s.z - u.z, d = Math.sqrt(dx * dx + dz * dz);
        if (d > 0) { u.hx = dx / d; u.hz = dz / d; }
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
    // a tick's generated inputs come before its data inputs; within a trip's tick a move comes before a press
    const feed = t => {
      feeding = t;
      while (genNext < gen.length && gen[genNext].at <= t) input(gen[genNext++].ev);
      while (next < script.length && script[next].at === t) input(script[next++]);
    };
    const end = scripted && demo && demo.end != null ? demo.end : Infinity;  // a Try-it sim is never done
    if (TRIPS) checkTrips(script, end);

    /* THE CURSOR'S SHAPE, each the cursor the game shows there (tagpu_wikisim.h, WIKI_CURSOR_*): the build-menu mode,
       else a selection holding a mobile unit of the player's, else none. Quick Build's ghost changes nothing here: it
       is drawn from the packet and leaves the engine's cursor as the selection has it. Off the map it is the arrow:
       the GUI shows cursornormal [INFERRED, capture C5]. */
    const cursorShape = () => over() !== 'map' ? 'arrow' : st.mode ? 'build' : selected().some(u => u.owner === 0 && mobile(u.type)) ? 'move' : 'arrow';
    // the sequence a shape shows: the build cursor is the arrow's cursornormal
    const cursorSeq = k => k === 'build' ? 'arrow' : k;
    /* THE SHAPE AFTER EVERY CHANGE, and `now`, the first tick that shows it, as the tick its animation began when it
       shows another sequence. The engine installs a cursor only when its index differs from the installed one
       (0x499254, 0x4992A9 compare main+0x2CBE first), so the arrow and the build cursor, both cursornormal, run on
       through each other. A script's inputs of tick t are shown first by tick t, which the step records; the player's
       stand between two ticks, and the view of the tick the sim stands at shows them. */
    const cursorTick = now => {
      const k = cursorShape();
      if (cursorSeq(k) !== cursorSeq(st.cursorShape)) st.cursorSince = now;
      st.cursorShape = k;
    };

    sim.input = ev => { input(ev); cursorTick(sim.tick); };
    sim.step = () => {
      const t = sim.tick + 1;
      feed(t);
      advance();
      sim.tick = t;
      // a locked build ends the demo on its own tick, which then holds under the entry's badge
      sim.done = sim.tick >= end || sb.locked !== null;
      if (ON) route();
      st.hover = hover();
      cursorTick(t);
    };
    // a unit's orders, for a check: kind, building and site, or the target's id
    sim.orders = id => { const u = byId(id); return u ? u.orders.map(o => ({ ...o })) : null; };
    sim.view = () => {
      // the gesture's sites replace the cursor's single square; none (an empty ring) leaves the square standing
      const p = st.place && st.place.sites.length ? st.place : null;
      const h = st.hover, gh = !p && h && (h.kind === 'place' ? h : h.kind === 'quick' ? { ...h, ok: true } : null);
      let n = 0;
      const sites = p ? p.sites.map(s => ({ ...siteCentre(p.type, s), state: s.state, order: s.state === 'place' ? ++n : null })) : [];
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
      // every change of the shape -- an input, a tick, the scene -- records it (cursorTick)
      const shape = st.cursorShape, since = st.cursorSince;
      return {
        map: { w: W, h: H, cell: CELL, surfaceMetal: SM, metal: metal.slice(), fog: fog.slice(), vents: vents.map(v => ({ x: (v.cx + 0.5) * CELL, z: (v.cz + 0.5) * CELL })) },
        units: units.map(u => ({ id: u.id, type: u.type, owner: u.owner, x: u.x, z: u.z, hx: u.hx, hz: u.hz, fx: fp(u.type)[0], fz: fp(u.type)[1],
          selected: u.sel, built: u.built, reclaimed: u.reclaimed, label: u.label })),
        ghost: gh ? { ...centre(gh.type, gh.ox, gh.oz), ok: gh.ok } : null,
        // the gesture's preview: each site's centre and footprint, 'place' (green), 'refused' (red, skipped) or 'remove'
        // (yellow, takes a queued order off), and its place among the orders the release would add
        sites,
        shape: p ? p.kind : null,
        spacing: st.mode ? st.spacing[st.mode] || 0 : null,
        queued: plans().map(p => ({ ...centre(p.type, p.ox, p.oz), owner: owners.get(`${p.type}@${p.ox},${p.oz}`) })),
        lines,
        cursor: { x: st.cursor.x, z: st.cursor.z, shape, since },
        keys: KEYS.filter(k => st.held.has(k)),
        drag: st.drag && st.drag.active ? { x0: st.drag.x0, z0: st.drag.z0, x1: mapCursor().x, z1: mapCursor().z } : null,
        caption: st.caption,
        pace: PACE,
        mode: st.mode ? `build:${st.mode}` : 'contextual',
        // the column, only with the sidebar on: its rows, the routed screen, the shown unit and its page, each gadget's
        // state, the unit under the cursor over the map (the minimap's high dot); then the build a demo could not reach
        ...(ON ? {
          side: { h: LAY.height, screen: sb.screen, unit: sb.shown ? sb.shown.id : null, page: sb.shown ? sb.shown.page : 0,
            state: sideStates(), under: over() === 'map' ? (unitAt(st.cursor.x, st.cursor.z) || { id: null }).id : null },
          locked: sb.locked,
        } : {}),
      };
    };

    if (ON) route();
    feed(0);
    sim.done = sb.locked !== null;
    st.hover = hover();
    st.cursorShape = cursorShape();
    st.cursorSince = 0;
    return sim;
  }

  // a layout is checked whole before any sim reads it; null or absent is no sidebar, today's stage
  const checked = layout => layout == null ? null : checkLayout(layout);
  return {
    create: (entry, demo, layout) => make(entry, demo, false, checked(layout)),
    play: (entry, demo, layout) => make(entry, demo, true, checked(layout)),
    // the bounds, for the caller that falls back to the fixture on a refused layout
    checkLayout,
    // the trip's fixed budget and its table, which tagpu_wikisim.h #defines and simparity compares
    TRIP, TRIP_DOWN, TRIP_UP, TRIP_ROWS,
    // the shapes alone, for checks against tagpu_placegeo.c
    shapes: { placeShape, placeRing },
  };
})();
if (typeof module !== 'undefined') module.exports = WikiSim;
