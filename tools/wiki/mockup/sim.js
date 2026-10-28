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
   played out as the cursor's trip to the button (research/notes/ingame-wiki.md, the owner's decisions 1-12).

   A sim is made for a context, ctx = {side, casting, iface} (the owner's decisions 13-18): the layout's side the
   player plays, that side's casting (the mod's own unit for each of an entry's casts, or null for the entry's own
   types), and the Interface Type, 0 left-click or 1 right-click. An absent ctx is side 0, no casting and type 1, the
   interface the demos are authored in. tagpu_wikisim.c's WIKISIMCTX is the same three.

   Runs in a page (the global WikiSim) and under node (module.exports). */
const WikiSim = (() => {
  const CELL = 16, TPS = 30, RADIUS = 3, ARENA = 64, DRAG = 4, FOLLOW = 48, SPACING = 24;
  /* THE CLICK WINDOW, the same at both Interface Types: a left release on the map is a click when its press is fewer
     than CLICK_TIME ticks old and the release point lies fewer than CLICK_BOX world units from the press point on
     each axis; any other release is a box (0x4993E9..0x499441, DISASSEMBLED). The engine stamps the press with
     0x4B6340 (0x4994BC), GetTickCount times *0x51FBD0+0xE8 over 1000, a rate MEASURED at 30 a second, so its 25
     units are 25 ticks here. It compares the world point's x and z words, main+0x2CAC and +0x2CB4, at the press
     (stored at +0x2C92, +0x2C9A) with the last ones before the release (+0x2C9E, +0x2CA6, rewritten on every other
     message while the box is open, 0x49946A..0x49949B), so a wobble that comes back is a click. DRAG only decides
     when the view draws the box. */
  const CLICK_TIME = 25, CLICK_BOX = 32;
  // tagpu_placegeo.h's caps, and tagpu_place.c's on one commit; a unit's list never holds more than PLACE_LIST_CAP
  const PLACE_MAX_SITES = 256, PLACE_SPACING_MAX = 16, PLACE_ORDER_BUDGET = 4096, PLACE_LIST_CAP = 1024;
  // tagpu_wikicob.h: WIKICOB_UNITS, the units one host spawns over its life, and the INBUILDSTANCE value id
  const COB_UNITS = 1024, COB_INBUILDSTANCE = 5;
  // tagpu_wikistage.h: the stage view's units, its pose pool and one posed model's primitives
  const STAGE_UNITS = 128, STAGE_POSE = 4096, STAGE_PIECES = 256;
  // tagpu_wikisim.h, THE NANO SPRAY: the live particles and the second bursts waiting, at most
  const SPRAY_PARTS = 1024, SPRAY_BURSTS = 256;
  /* A DEMO'S WAIT, {"at", "until", "max"} (research/notes/ingame-wiki-cob-plan.md, decision 10): the demo's clock
     stops at `at` until the state `until` names holds, at most `max` ticks, after which the demo stops and says so.
     UNTILS are the states; "idle": no unit of the player's holds a build or a reclaim order. UNTIL_MAX bounds `max`. */
  const UNTILS = ['idle'], UNTIL_MAX = 3600;
  const KEYS = ['shift', 'alt', 'ctrl', 'z', 'x', 'lmb', 'rmb'];
  const sq = v => v * v;
  const own = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
  const obj = v => v !== null && typeof v === 'object' && !Array.isArray(v);
  const int = Number.isInteger;
  // a name and its terminator fit a C field of n bytes, counted in UTF-8 as the C reads them
  const fitsIn = (v, n) => typeof v === 'string' && new TextEncoder().encode(v).length < n;

  /* ---- the sidebar's layout ----
     The column is the engine's panel width, 128 game pixels, whatever the HUD scale; it is at least 480 rows (the
     PANELSIDE picture) and as tall as its tallest screen. The minimap fills its top 128 x 128 and is never
     clickable. Every bound below is one tagpu_wikisim.h's WIKILAYOUT holds: a layout breaking any of them is refused
     whole (this throws; the caller then gives the fixture), so no index read out of a layout addresses past it.

     The layout: {format: 1, width: 128, height,
     sides: [{name, prefix, main2, gen, types: {TYPE: {pages: [screen of page 1, ...], orders: screen or null}}}],
     screens: {NAME: {y, w, h, gadgets: [{name, role, rect: [x, y, w, h], unit, grey, hidden}]}}}.
     A screen's y is its row in the column and a gadget's rect is in its screen's pixels. role is orders, buildtab,
     build, prev, next, stop or order, as 0x41AA00 tells them apart by name; a build button's unit is its unit type,
     or null (greyed) when the name is none; hidden marks an inactive gadget. Labels, fonts and the panel picture
     are not gadgets here: they are the art's. A side's types are the types with a page (def+0x22E != 0) that side's
     casting names; its pages are read with its own prefix, so one type name may have other pages in another side.
     The sim reads the side ctx.side names, MAIN2, GEN and types alike. */
  const SIDE_W = 128, SIDE_MIN_H = 480, MINIMAP = 128;
  const SIDE_SCREENS = 48, SIDE_GADGETS = 64, SIDE_PAGES = 7, SIDE_TYPES = 64, SIDE_ROWS = 1024, SIDE_SIDES = 8, SIDE_NAME = 32;
  const ROLES = ['orders', 'buildtab', 'build', 'prev', 'next', 'stop', 'order'];
  function checkLayout(L) {
    const no = what => { throw new Error(`the sidebar layout is refused: ${what}`); };
    // SIDE_NAME: WIKILAYOUT's name fields
    const fits = v => fitsIn(v, SIDE_NAME);
    const name = v => fits(v) && v.length > 0;
    if (!obj(L) || L.format !== 1) no('format is not 1');
    if (L.width !== SIDE_W) no(`width is not ${SIDE_W}`);
    if (!obj(L.screens)) no('screens is not an object');
    const names = Object.keys(L.screens);
    // SIDE_SCREENS: WIKILAYOUT's screen table
    if (names.length > SIDE_SCREENS) no(`more than ${SIDE_SCREENS} screens`);
    let rows = SIDE_MIN_H;
    for (const n of names) {
      const s = L.screens[n];
      if (!fits(n)) no(`screen ${n} has a name too long`);
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
    // SIDE_SIDES: WIKILAYOUT's side table
    if (!Array.isArray(L.sides) || !L.sides.length || L.sides.length > SIDE_SIDES) no(`sides is not a list of 1 to ${SIDE_SIDES}`);
    L.sides.forEach((s, i) => {
      if (!obj(s) || !name(s.name) || !fits(s.prefix) || !screen(s.main2) || !screen(s.gen))
        no(`side ${i} has no name, prefix, or MAIN2 and GEN screens`);
      if (!obj(s.types)) no(`side ${i}: types is not an object`);
    });
    // SIDE_TYPES: WIKIPAGES' table, one row per (type, side) over every side
    if (L.sides.reduce((n, s) => n + Object.keys(s.types).length, 0) > SIDE_TYPES) no(`more than ${SIDE_TYPES} types over the sides`);
    L.sides.forEach((s, i) => {
      for (const [t, T] of Object.entries(s.types)) {
        const at = `side ${i} type ${t}`;
        if (!fits(t)) no(`${at} has a name too long`);
        // SIDE_PAGES: the page is unit+0x110 bits 23..25, a 3-bit field, so the engine reaches at most 7 numbered pages
        if (!obj(T) || !Array.isArray(T.pages) || T.pages.length > SIDE_PAGES || !T.pages.every(screen))
          no(`${at} has no list of at most ${SIDE_PAGES} page screens`);
        if (!(T.orders === null || screen(T.orders))) no(`${at}: orders is not a screen or null`);
        // def+0x22E != 0: a type with neither a numbered page nor an orders page is not in `types`
        if (!T.pages.length && T.orders === null) no(`${at} has no page`);
      }
    });
    return L;
  }

  /* ---- casts and castings (research/notes/ingame-wiki.md, the owner's decisions 13-15 and 20) ----
     Each unit of an entry names its cast, its place in the demos; a casting answers each cast with one of the mod's
     types for one side, or null when the rule found none (unbound). The casting is a pack's
     (gfx/side/<mod>/side.json `casting`, index-aligned with the layout's sides) or tools/wiki/casting-fixture.json's.
     Every bound below is one tagpu_wikidata.h's WIKICASTDEF holds; a casting breaking one is refused whole. */
  const CASTS = ['builder', 'advbuilder', 'mex', 'moho', 'geo', 'walker', 'tower', 'wall', 'solar'];   // WIKI_CAST_* order
  const FIGURES = ['buildTime', 'workerTime', 'buildDistance', 'maxVelocity', 'extracts', 'energyMake'];
  // WIKI_FP_MAX: a footprint's side on each axis, the width of a G row's bit mask; WIKI_ID_MAX: a type name's field;
  // WIKI_TITLE_MAX: a display name's field, 31 code-page bytes (def+0x00 is 0x20 bytes) at most 3 UTF-8 bytes each
  const FP_MAX = 16, ID_MAX = 24, TITLE_MAX = 94;
  /* a type's G cells, `g`: [[dx, dz], ...] inside its footprint, the yard cells with bit 0x80 (0x8F, the only yard
     code carrying it, 0x42CF9D..0x42D06B, DISASSEMBLED). `vent` says whether it has one; with no `g`, `vent: true` is
     a yard all G, as stock's ARMGEO is. Refused: a cell off the footprint, or a `vent` that disagrees with `g`. */
  function checkG(at, d, f) {
    if (!('g' in d)) return;
    const [fx, fz] = f;
    if (!Array.isArray(d.g) || !!d.vent !== d.g.length > 0 ||
        !d.g.every(c => Array.isArray(c) && c.length === 2 && int(c[0]) && int(c[1]) && c[0] >= 0 && c[0] < fx && c[1] >= 0 && c[1] < fz))
      throw new Error(`${at}: g is not a list of G cells inside the footprint, held by vent exactly when it is not empty`);
  }
  function checkCasting(C) {
    const no = what => { throw new Error(`the casting is refused: ${what}`); };
    if (!obj(C) || !obj(C.casts)) no('casts is not an object');
    for (const c of Object.keys(C.casts)) if (!CASTS.includes(c)) no(`${c} is not a cast`);
    for (const c of CASTS) {
      if (!own(C.casts, c)) no(`${c} is absent (null is the unbound cast)`);
      const k = C.casts[c];
      if (k === null) continue;
      const at = `cast ${c}`;
      /* a name of digits alone is refused because a JS object lists integer keys first, so the bound entry's types
         would leave definition order, which Quick Build's ties and every unit loop read; no UnitName measured is one */
      if (!obj(k) || !fitsIn(k.type, ID_MAX) || !k.type || /^[0-9]+$/.test(k.type)) no(`${at} has no type name of 1 to ${ID_MAX - 1} bytes, not all digits`);
      if (!fitsIn(k.title, TITLE_MAX)) no(`${at} has no title of at most ${TITLE_MAX - 1} bytes`);
      const f = k.footprint;
      if (!Array.isArray(f) || f.length !== 2 || !f.every(v => int(v) && v >= 1 && v <= FP_MAX)) no(`${at}: the footprint is not two integers 1..${FP_MAX}`);
      // the figures divide and step: a build time of 0 would fill a building in no time at all, and a negative or
      // infinite speed or reach would leave the world's arithmetic
      for (const n of FIGURES) if (n in k && !(typeof k[n] === 'number' && k[n] >= 0 && k[n] < Infinity)) no(`${at}: ${n} is not a finite number >= 0`);
      if (!(k.buildTime > 0)) no(`${at}: buildTime is not above 0`);
      if ('canstop' in k && !int(k.canstop)) no(`${at}: canstop is not an integer`);
      if ('activatewhenbuilt' in k && !int(k.activatewhenbuilt)) no(`${at}: activatewhenbuilt is not an integer`);
      if ('vent' in k && typeof k.vent !== 'boolean') no(`${at}: vent is not a boolean`);
      checkG(at, k, f);
      if ('builds' in k && !(Array.isArray(k.builds) && k.builds.every(b => CASTS.includes(b)))) no(`${at}: builds is not a list of casts`);
    }
    return C;
  }
  /* THE BOUND ENTRY: the entry with every unit of a bound cast replaced by the casting's type, under the type's name,
     with the casting's title (as `name`), footprint, figures, canstop, activatewhenbuilt and G cells, and a build list that holds unit j
     of the entry exactly when the casting's build list names j's cast. A unit of an unbound cast keeps the authored
     definition, marked `unbound`, and its build list keeps only the entry's own units, renamed as they are. The
     units keep their order, so the scene's units and a `build` point at the same definitions as before; the scene's
     types and the inputs' `build` are renamed with them, captions untouched (the sim and its hash keep raw
     captions). Every unit names one of the nine casts and no cast twice; two units bound to one name are refused.
     tagpu_wikisim.c builds the same as WIKISIM's own bdef[] (a field of the sim, so it lives and dies with it). */
  function binder(entry, casting) {
    const K = checkCasting(casting).casts, names = Object.keys(entry.units), to = {}, seen = new Set();
    for (const t of names) {
      const c = entry.units[t].cast;
      if (!CASTS.includes(c)) throw new Error(`unit ${t} names no cast`);
      if (seen.has(c)) throw new Error(`two units name the cast ${c}`);
      seen.add(c);
      to[t] = K[c] ? K[c].type : t;
    }
    if (new Set(names.map(t => to[t])).size !== names.length) throw new Error('the casting binds two units to one type');
    const units = {};
    for (const t of names) {
      const d = entry.units[t], k = K[d.cast];
      if (!k) { units[t] = { ...d, unbound: true, builds: (d.builds || []).filter(b => own(entry.units, b)).map(b => to[b]) }; continue; }
      const b = { cast: d.cast, name: k.title, footprint: [k.footprint[0], k.footprint[1]],
        builds: names.filter(j => (k.builds || []).includes(entry.units[j].cast)).map(j => to[j]) };
      for (const n of FIGURES) if (n in k) b[n] = k[n];
      if ('canstop' in k) b.canstop = k.canstop;
      if ('activatewhenbuilt' in k) b.activatewhenbuilt = k.activatewhenbuilt;
      if (k.vent) b.vent = true;
      if (k.g && k.g.length) b.g = k.g.map(c => [c[0], c[1]]);
      units[to[t]] = b;
    }
    const scene = s => s && s.units ? { ...s, units: s.units.map(u => own(to, u.type) ? { ...u, type: to[u.type] } : u) } : s;
    const demo = d => d && { ...d, scene: scene(d.scene),
      inputs: d.inputs && d.inputs.map(e => typeof e.build === 'string' && own(to, e.build) ? { ...e, build: to[e.build] } : e) };
    const bound = { ...entry, units, scene: scene(entry.scene) };
    if (entry.demos) bound.demos = entry.demos.map(demo);
    return { entry: bound, demo };
  }
  const bindEntry = (entry, casting) => binder(entry, casting).entry;

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

  /* THE CONTEXT, refused as tagpu_wikisim_init refuses it: an Interface Type other than 0 or 1 (the DLL maps the
     engine's int to these two before it starts a demo), and with a layout a side that is not one of its sides. */
  function context(layout, ctx) {
    const c = ctx == null ? { side: 0, casting: null, iface: 1 } : ctx;
    if (!(c.iface === 0 || c.iface === 1)) throw new Error(`the interface type ${c.iface} is not 0 or 1`);
    if (layout && !(int(c.side) && c.side >= 0 && c.side < layout.sides.length)) throw new Error(`side ${c.side} is not one of the layout's sides`);
    if (!(c.casting == null || obj(c.casting))) throw new Error('the casting is not an object or null');
    return { side: layout ? c.side : -1, casting: c.casting ?? null, iface: c.iface, cob: c.cob || null };
  }

  /* ---- actions (decision 18): a demo's press names what it does, and the Interface Type names the button ----
     select is the left button in both types, cancel the right one in both, order the right one at type 1 and the left
     one at type 0. lmb and rmb stay as written. */
  const ACTIONS = ['select', 'order', 'cancel'];
  const button = (a, iface) => a === 'select' ? 'lmb' : a === 'cancel' ? 'rmb' : a === 'order' ? (iface ? 'rmb' : 'lmb') : a;
  /* THE DATA RULE OF A RESOLVED SCRIPT, refused here as tagpu_wikisim_init refuses it: once the actions are buttons,
     a press of a mouse button already held, or a release of one not held, refuses the demo with the input's tick. An
     authored overlap -- the left button held for a box drag while an `order` click lands at type 0 -- therefore fails
     simcheck, which plays every demo at both types, and never reaches a player. The keys are not held to it: a key's
     second down is an auto-repeat (stepSpacing). */
  function resolve(demo, script, iface) {
    const held = new Set();
    return script.map(e => {
      const r = ACTIONS.includes(e.down) || ACTIONS.includes(e.up) ? { ...e } : e;
      if (r !== e && 'down' in r) r.down = button(r.down, iface);
      if (r !== e && 'up' in r) r.up = button(r.up, iface);
      for (const [k, down] of [[r.down, true], [r.up, false]]) {
        if (k !== 'lmb' && k !== 'rmb') continue;
        if (down === held.has(k))
          throw new Error(`demo ${demo.id}: at tick ${e.at} the ${down ? 'press' : 'release'} of ${k} ${down ? 'while it is held' : 'while it is up'}`);
        if (down) held.add(k); else held.delete(k);
      }
      return r;
    });
  }

  function make(entryIn, demoIn, scripted, layout, ctxIn) {
    const ctx = context(layout, ctxIn), IFACE = ctx.iface;
    // the bound entry and demo are this sim's own copies: they live as long as it does, and nothing else holds them
    const B = ctx.casting ? binder(entryIn, ctx.casting) : null;
    const entry = B ? B.entry : entryIn, demo = B ? B.demo(demoIn) : demoIn;
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
    for (const t of defIndex) checkG(`unit ${t}`, D[t], D[t].footprint);
    // a resource building: a mex extracts metal, a geothermal plant needs a vent under a G cell of its yard
    const kind = t => def(t).extracts > 0 ? 'mex' : def(t).vent ? 'geo' : null;
    // footprint cell (dx, dz) of type t is a G cell: one `g` lists, or any cell of a `vent` type with no `g`
    const gCell = (t, dx, dz) => { const d = def(t); return d.g ? d.g.some(c => c[0] === dx && c[1] === dz) : !!d.vent; };
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
    const LAY = ON ? layout : null, SIDE = ON ? LAY.sides[ctx.side] : null;
    const typePages = t => ON && own(SIDE.types, t) ? SIDE.types[t] : null;
    const npages = t => { const T = typePages(t); return T ? T.pages.length : 0; };

    /* ---- the grid ---- */
    const cellOf = v => Math.floor(v / CELL);
    const seen = (cx, cz) => fog[cz * W + cx] !== 2;
    const overlap = (a, b) => a.ox < b.ox + b.fx && b.ox < a.ox + a.fx && a.oz < b.oz + b.fz && b.oz < a.oz + a.fz;
    const box = (t, ox, oz) => ({ ox, oz, fx: fp(t)[0], fz: fp(t)[1] });

    /* ---- the units' scripts: the in-game wiki's COB host, tagpu_wikicob.c, which tagpu_wikisim.c calls in-process
       and this file through ctx.cob (tools/wiki/mockup/cob.js; research/notes/ingame-wiki-cob-plan.md, *The wiki
       host*). Without one every unit stands at rest and every gate reads open. ---- */
    const COB = ctx.cob ? ctx.cob.open(defIndex) : null;
    let cobN = 0;
    // a world position in 16.16, truncated toward zero and held to the int32 range, as tagpu_wikisim.c's fix16
    const fix16 = v => Math.max(-2147483648, Math.min(2147483647, Math.trunc(v * 65536)));
    /* GET's facts (WIKICOBFACT): every unit with a script handle, in creation order -- its handle, owner, centre, the
       build fraction still to go as the float unit+0x104 holds, and HEALTH in percent, 100 since the wiki has no
       damage */
    const facts = () => units.filter(u => u.cob).map(u => [u.cob, u.owner, fix16(u.x), fix16(u.z), Math.fround(1 - u.built), 100]);
    // INBUILDSTANCE, the gate a builder's work waits on (0x438700); open for a unit that runs no script
    const stance = u => !COB || !u.cob || COB.value(u.cob, COB_INBUILDSTANCE) !== 0;
    const cobCall = (u, script, how, args) => { if (COB && u.cob) COB.call(u.cob, script, how, args, facts()); };
    /* the extractor's SetSpeed (0x437840, from every create): the sum over its footprint cells on the map of the cell's
       metal + 1, kept in a 16-bit word and sign-extended (0x437866..0x437902). ExtractsMetal does not enter it. */
    function extractorSpeed(u) {
      const [fx, fz] = fp(u.type);
      let sum = 0;
      for (let z = u.oz; z < u.oz + fz; z++) for (let x = u.ox; x < u.ox + fx; x++)
        if (x >= 0 && x < W && z >= 0 && z < H) sum += (Math.trunc(metal[z * W + x]) & 0xff) + 1;
      return (sum & 0xffff) << 16 >> 16;
    }
    /* A UNIT APPEARS (a scene's spawn, a nanoframe placed): the create's script starts in the engine's order, Create
       run-now (0x485DE6), an extractor's SetSpeed run-later (0x437902), and for a unit made whole whose FBI says
       ActivateWhenBuilt the state setter's Activate (0x48612F). Its handle is the next in spawn order, the host's own
       numbering, so the facts can carry its row while Create runs. */
    function spawn(u) {
      if (!COB || cobN >= COB_UNITS) return;
      u.cob = ++cobN;
      if (COB.spawn(defIndex.indexOf(u.type), facts()) !== u.cob) throw new Error(`the COB host numbered ${u.id} otherwise`);
      if (def(u.type).extracts > 0) cobCall(u, 'SetSpeed', 'args', [extractorSpeed(u)]);
      if (u.built >= 1 && (def(u.type).activatewhenbuilt & 1) === 1) COB.activate(u.cob, 1);
    }

    /* ---- units. Each faces along a unit vector (hx, hz); walk says why a vector. ---- */
    let units = [], nextId = 1;
    /* a unit joins the world. With the sidebar it carries its page state, unit+0x110: bp (bit 22, a numbered page
       is shown rather than the orders page) and page (bits 23..25, 1..n). 0x485CD5 creates a unit whose type has
       numbered pages with bit 22 set and page 1; any other unit has neither. */
    function enter(u) {
      if (ON) { const n = npages(u.type); u.bp = n >= 1; u.page = n >= 1 ? 1 : 0; }
      units.push(u);
      spawn(u);
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
    /* A SCENE ITS TYPES DO NOT FIT IS REFUSED. A scene unit is placed by its centre, so a bound footprint other than
       the authored one can overlap a structure placed before it or leave the placeable interior (cells 1 to w - 2,
       the site test's bound below); the demo is refused for that casting rather than played broken, as
       tagpu_wikisim_init refuses it. Mobile units never block. */
    for (const s of S.units) {
      const u = add(s.type, s.owner ?? 0, s.at[0], s.at[1], s.built ?? 1, s.id, s.label, facing(s.facing));
      u.orders = (s.orders || []).slice(0, PLACE_LIST_CAP).map(o => ({ k: 'move', x: o.move[0], z: o.move[1] }));
      if (mobile(u.type)) continue;
      const b = box(u.type, u.ox, u.oz);
      if (!(b.ox >= 1 && b.oz >= 1 && b.ox + b.fx < W && b.oz + b.fz < H) ||
          units.some(v => v !== u && !mobile(v.type) && overlap(b, box(v.type, v.ox, v.oz))))
        throw new Error(`${u.id}'s ${u.type} does not fit the scene`);
    }
    /* DECISION 12 FOR AN UNBOUND CAST: a scene unit of an unbound type stops the demo on tick 0 under the entry's
       badge, the first such unit in creation order naming it (input() does the same for a `build` on its tick) */
    const unbound = t => defined(t) && D[t].unbound === true;
    const lock0 = S.units.find(s => unbound(s.type));
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
       it), clear of every structure but `ignore` (an upgrade's old building), and for a geothermal plant a vent under
       one of its G cells: 0x47D2E0 refuses a yard holding G (0x47D80F) unless some G cell stands on a feature whose
       FeatureDef +0xFE has bit 0x20 (0x47D684..0x47D711, 0x47D75B..0x47D769), DISASSEMBLED; a vent under any other
       cell of the yard counts for nothing. Mobile units never block: the sim has no collisions. */
    const interior = (t, ox, oz) => ox >= 1 && oz >= 1 && ox + fp(t)[0] < W && oz + fp(t)[1] < H;
    // the vents under a G cell of type t's footprint at (ox, oz), in scene order
    const ventsUnderG = (t, ox, oz) => {
      const [fx, fz] = fp(t);
      return vents.filter(v => v.cx >= ox && v.cx < ox + fx && v.cz >= oz && v.cz < oz + fz && gCell(t, v.cx - ox, v.cz - oz));
    };
    function siteOk(t, ox, oz, ignore) {
      const b = box(t, ox, oz);
      if (!interior(t, ox, oz)) return false;
      for (const u of structures()) if (u !== ignore && overlap(b, box(u.type, u.ox, u.oz))) return false;
      if (kind(t) === 'geo' && !ventsUnderG(t, ox, oz).length) return false;
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
    // geo_anchor: the one explored vent under a G cell of the footprint (tagpu_resource.c tests yard & 0x80 alone);
    // none when no G cell holds one or they hold several
    function ventAnchor(t, ox, oz) {
      const vs = ventsUnderG(t, ox, oz).filter(v => seen(v.cx, v.cz));
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
       went down, which decides whether its release runs the map rules; locked: the type the demo stopped on (lock) */
    const sb = { screen: null, shown: null, cap: null, downOn: {}, locked: null };
    /* the demo stops under the badge: the first lock of the sim names the type, and the tick it falls on finishes as
       any tick does (a trip's data rule leaves only captions after its build there). Its sources: a build the
       layout cannot reach (trip), and a scene unit or a build of an unbound type, with or without the sidebar. */
    const lock = t => { if (sb.locked === null) sb.locked = t; };
    const screenOf = n => LAY.screens[n];
    // a point of the stage in the column's own pixels is (x + 128, z); the minimap's 128 x 128 there holds no gadget
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
       side's GEN, with the unit shown when there is exactly one. GEN and MAIN2 are the player's side's, ctx.side. The
       sim routes after every input and every tick, where the engine re-routes a frame or more later: a known
       difference of at most a frame. A screen change drops the capture, as the engine's page swap reloads the screen
       and its gadgets. */
    function route() {
      const sel = selected().filter(u => u.owner === 0), side = SIDE;
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
       main+0x2CC3 = 1 (0x419CC2), which replaces an armed build's mode 0x0E, so it disarms; it clears main+0x2CC6
       bit 3 (0x419CCE..0x419CD8) and leaves bit 5, the Shift-placement bit, so a placement made earlier in this Shift
       hold still disarms when Shift is released (0x4992D8); then the order issuer 0x48CF30 gives every selected unit
       of the controlled player the STOP order through 0x43AFC0 (0x48D1F4), which replaces its orders. The path tests
       no canstop, so a unit without it is stopped along with the selection's others. With Shift held the issuer
       passes shift = 1 to 0x43AFC0, whose effect on STOP is not established; the sim empties the orders either way
       [INFERRED]. The placement gesture ends with the mode: tagpu_place.c ends it on the first record the handler
       takes in any mode but 14, which comes before any release could commit it. */
    function act(g) {
      const u = sb.shown, n = u ? npages(u.type) : 0;
      if ((g.role === 'prev' || g.role === 'next') && n) {
        u.page = g.role === 'next' ? (u.page === n ? 1 : u.page + 1) : (u.page < 2 ? n : u.page - 1);
        u.bp = true;
      } else if (g.role === 'orders' && n) u.bp = false;
      else if (g.role === 'buildtab' && n) u.bp = true;
      else if (g.role === 'stop') {
        st.mode = null; st.gesture = null;
        for (const v of selected()) if (v.owner === 0) v.orders = [];
      }
      else if (g.role === 'build' && defined(g.unit) && !mobile(g.unit)) arm(g.unit);
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
    /* WHO TAKES A MOUSE PRESS, DISASSEMBLED. IdleTick runs the GUI pump 0x4A9FD0 (0x499992) before the in-play
       handler 0x499200 (0x499A1C), and the pump's poll 0x4AB5D0 pops a record whose point is inside the screen's window,
       its gadget 0 (0x4A1680, the inclusive test 0x4A1920 at 0x4AB63E), and leaves a record outside it that carries a
       button (0x4AB647..0x4AB64D: its wParam's MK_ bits). A press on the screen, on a gadget or not, is the GUI's
       alone; the handler sees only the releases IdleTick copies after the pump (0x4999EF..0x499A04). Every other
       press, the minimap's and the column's rows outside the screen included, reaches the handler, whose
       WM_RBUTTONDOWN calls 0x499100 at 0x4993AC wherever the point is; with an order armed (main+0x2CC3 != 1) that
       is the cancel, and it reads neither the point nor the Interface Type (0x499107..0x49915F). A left press there with
       a building armed is one placement (see input()). A press there with nothing armed depends on the Interface
       Type (0x499162, 0x499567), and the sim does none of it. */
    function onScreen() {
      const s = screenOf(sb.screen), { x, z } = st.cursor;
      return inRect([0, s.y, s.w, s.h], x + SIDE_W, z);
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
    /* feeding: the tick being fed, and between two ticks the one the sim stands at, since every step feeds its tick:
       the clock an input reads */
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
      if (!plan) { lock(T); return; }
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

    /* the build-menu mode: the ghost is manual_cursor's snapped site (no legality), or where the cursor is, as the build
       cursor 0x4197D0 rounds it. None while the cursor is off the map, and the gesture's evaluation stands as the map
       last left it: the in-play handler calls 0x4197D0 only with main+0x2CC6 bit 1, the cursor in the viewport
       (0x499236..0x499241), and tagpu_place.c evaluates only from that call. */
    function hover() { return over() === 'map' ? mapHover() : null; }
    /* the evaluation the stage shows, as tagpu_place.c's current() lets the packet have it: only while the building it
       was made for is still the one armed */
    const placed = () => st.place && st.mode === st.place.type ? st.place : null;
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
       cursor is the stock Shift-click itself; anything else commits the last evaluation. A release over the sidebar
       reaches tagpu_place.c too: IdleTick hands the in-play handler a button's release the GUI pump took
       (0x4999CB..0x499A04, then 0x499A1C), and the route at 0x4993BF passes it to place_button wherever the cursor
       is. Its record is never the evaluated one there (the evaluation stands as the map left it), so it is never the
       stock Shift-click: it commits the evaluation the map last made. */
    function release() {
      st.gesture = null;
      if (!st.held.has('shift')) return;
      const p = placed(), cur = cursorSite();
      if (p && over() === 'map' && p.kind !== 'ring' && p.sites.length === 1 && p.sites[0].cx === cur.cx && p.sites[0].cz === cur.cz)
        return placeClick(true);
      if (p) commit(p);
    }

    /* ---- orders ---- */
    // leaving the build-menu mode ends the gesture and clears the Shift-placement bit with it
    const disarm = () => { st.mode = null; st.gesture = null; st.shiftPlaced = false; };
    /* ARMING a building, the build button's branch of 0x41AA00, DISASSEMBLED: main+0x2CC3 = 0x0E (0x41AB89) and the
       type at main+0x2CC4 (0x41AB9C), and no write to main+0x2CC6. So a Shift-placement made earlier in this Shift
       hold keeps bit 5, and releasing Shift disarms the building armed since as well: 0x4992D8..0x49930F tests bit 5,
       not the mode. tagpu_place.c leaves the bit too (it sets it only on a commit), and ends its gesture when the armed
       type changes (its evaluate, `s_gesture.type != c.type`), so a gesture outlives a re-arm of its own type only. */
    const arm = t => { if (st.mode !== t) st.gesture = null; st.mode = t; st.drag = null; };
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
    /* Quick Build's click, the same at both Interface Types: tagpu_resource.c gates the button that orders, the right
       press at type 1 (0x499100) and the left click at type 0 (0x498F70, `(iface != 0) != right` at line 742). With a
       choice, a queued site is cancelled by Shift and a plain click is consumed unchanged; otherwise the choice is
       ordered. false: no choice, the click is the game's own. */
    function quickClick(shift) {
      const c = quickChoice(shift);
      if (!c) return false;
      if (c.queued) { if (shift && c.queued === 2) unqueue(c); }
      else quickOrder(c, shift);
      return true;
    }
    const unfinishedOwn = u => u.owner === 0 && !mobile(u.type) && u.built < 1;
    // the assist: every selected builder is ordered to build the unfinished building where it stands
    const assist = (u, shift) => { for (const b of builders()) assign(b, [{ k: 'build', type: u.type, ox: u.ox, oz: u.oz }], shift); };
    /* TYPE 1, a right press on the map with nothing armed: Quick Build's, else the game's own, which orders at the
       press (0x4991D5..0x4991F5, MEASURED): assist an unfinished building of the player's, or move */
    function rightClick(shift) {
      if (quickClick(shift)) return;
      const { x, z } = mapCursor(), u = unitAt(x, z);
      if (u && unfinishedOwn(u)) return assist(u, shift);
      moveOrder(shift);
    }
    /* TYPE 0, a left click on the map with nothing armed: the release of a press that went down on the map, inside the
       click window (CLICK_TIME, CLICK_BOX; 0x499421..0x499441 call 0x498F70), so its order lands at the release where
       type 1's lands at the press. In order: Quick Build's choice; else over a unit of the player's, an unfinished
       building with a selected builder is assisted (cursor 6, 0x43EDB6..0x43EE42) and any other is selected as a click
       selects (cursor 0xF, 0x48C7F0); else the selected mobile units of the player's move (0x48CF30, Shift appends),
       and with none nothing happens. MEASURED in game 2026-10-28 (stock ARM skirmish, an ARMCK selected): the move
       order appears after the release, Quick Build takes a deposit and a vent, an unfinished ARMSOLAR is assisted, a
       click on the ARMCK selects it, and the cursor is 14 over ground, a deposit and a vent, 6 over the unfinished
       building and 15 over a finished unit of the player's. [INFERRED] that an unfinished building of the player's with
       no builder selected is selected: 0x43EDB6 gives 0xF only over a finished unit (+0x104 == 0.0), so the engine may
       show another cursor there. [INFERRED] that a unit of another player under the cursor gives the move: no measured
       scene had one. */
    function leftClick(shift) {
      if (quickClick(shift)) return;
      const { x, z } = mapCursor(), u = unitAt(x, z);
      if (u && u.owner === 0) return unfinishedOwn(u) && builders().length ? assist(u, shift) : select(shift);
      moveOrder(shift);
    }
    /* TYPE 0, a right press on the map with nothing armed (0x499172..0x499196, DISASSEMBLED): without Ctrl every unit
       is deselected (0x48BD00), at the press (MEASURED 2026-10-28: the ARMCK was no longer selected before the release).
       With Ctrl it calls 0x41CC60, which hands the mouse to a view mode (main+0x2CDF = 1, the camera follow released,
       the cursor centred) that the stage, having no camera to move, does not model; the selection stays (MEASURED),
       and main+0x2CDF read 0 after the press, so the view mode itself was not seen. */
    function rightPress0() {
      if (!st.held.has('ctrl')) for (const u of units) u.sel = false;
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
    // box: the release that ends a press on the map outside the click window selects what the box from the press holds
    function select(shift, box) {
      const d = st.drag, { x, z } = mapCursor();
      if (box) {
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
        if (unbound(ev.build)) lock(ev.build);
        else if (TRIPS) trip(ev.build);
        else if (ev.build) arm(ev.build);
        else { disarm(); st.drag = null; }
      }
      if (ev.move) {
        st.cursor = { x: ev.move[0], z: ev.move[1] };
        const c = mapCursor();
        if (st.drag && sq(c.x - st.drag.x0) + sq(c.z - st.drag.z0) > DRAG * DRAG) st.drag.active = true;
      }
      if (ev.down === 'z' || ev.down === 'x') { st.held.add(ev.down); stepSpacing(ev.down); }
      else if (ev.down && !st.held.has(ev.down)) {
        st.held.add(ev.down);
        const shift = st.held.has('shift'), where = over(), mouse = ev.down === 'lmb' || ev.down === 'rmb';
        if (mouse) sb.downOn[ev.down] = where;
        if (mouse && where === 'side' && onScreen()) pressSide(ev.down);
        else if (where === 'map') {
          // with a building armed a left press carrying Shift starts a gesture at the cursor site instead of placing
          if (ev.down === 'lmb') {
            if (st.mode && shift) st.gesture = cursorSite();
            else if (st.mode) placeClick(false);
            else { const c = mapCursor(); st.drag = { x0: c.x, z0: c.z, t: feeding, active: false }; }
          }
          // with a building armed a right press cancels it at both types (0x499100, MEASURED at type 1)
          if (ev.down === 'rmb') { st.drag = null; if (st.mode) disarm(); else if (IFACE) rightClick(shift); else rightPress0(); }
        }
        /* OFF THE MAP AND OFF THE SCREEN a left press with a building armed places it once, at the press. The handler
           finds bit 1 clear, so place_button returns 0 at the route 0x4993BF and stock's click 0x498F70 runs; the
           resource module's gate there re-runs the build cursor 0x4197D0 on the point 0x498DA0 clamped to the view's
           edge (0x498E32..), and 0x419670 places that site. Shift keeps the building armed (bit 5, 0x498FB4), and
           without it the press disarms (0x498FC0); no gesture starts, and the release adds nothing. MEASURED
           2026-10-27, Interface Type 1, 1024 x 768: a press at (60, 600), below the screen, placed one site at the
           clamped point (1148, 6416), snapped to (1144, 6424). The sim's clamp onto the map stands for the engine's
           onto the view, because the stage shows the map where the game shows a view. The band under a short map
           has no engine equivalent: the game's scroll keeps its view on the map [INFERRED], so the nearest rule is
           the same clamp. The minimap stays inert (the owner's decision 6), where the game would place at its world
           point. A right press there is the handler's cancel and nothing else. */
        else if (ev.down === 'lmb' && st.mode && !onMinimap(st.cursor.x, st.cursor.z)) placeClick(shift);
        else if (ev.down === 'rmb' && st.mode) disarm();
      }
      if (ev.up && st.held.has(ev.up)) {
        st.held.delete(ev.up);
        // a release runs the map rules only for a press that went down on the map
        if (sb.cap && sb.cap.b === ev.up) releaseSide();
        else if (sb.downOn[ev.up] === 'map') {
          if (ev.up === 'lmb' && st.gesture) release();
          else if (ev.up === 'lmb' && st.drag) {
            // a box is the same at both types; a click is type 1's select and type 0's leftClick
            const d = st.drag, c = mapCursor(), shift = st.held.has('shift');
            const click = feeding - d.t < CLICK_TIME && Math.abs(c.x - d.x0) < CLICK_BOX && Math.abs(c.z - d.z0) < CLICK_BOX;
            if (!click) select(shift, true); else if (IFACE) select(shift); else leftClick(shift);
            st.drag = null;
          }
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
      u.stepped = true;
      u.hx = dx / d; u.hz = dz / d;
      u.x += dx / d * s; u.z += dz / d * s;
      return false;
    }
    function reach(u, b) {  // toward a footprint until within build distance of it; true once in range
      if (rectDist(u, b) <= def(u.type).buildDistance) return true;
      walk(u, (b.ox + b.fx / 2) * CELL, (b.oz + b.fz / 2) * CELL, 0);
      return false;
    }
    /* ---- the nano spray (tagpu_wikisim.h, THE NANO SPRAY), tagpu_wikisim.c's spray functions line for line: int32
       arithmetic through | 0 and Math.imul, each double operation as the C rounds it ---- */
    let parts = [], bursts = [], nemit = 0, partsCut = false;
    // a double truncated toward zero and held to the int32 range, as tagpu_wikisim.c's trunc32
    const trunc32 = v => Math.max(-2147483648, Math.min(2147483647, Math.trunc(v)));
    // lowbias32, a 32-bit finaliser
    const mix32 = x => {
      x ^= x >>> 16; x = Math.imul(x, 0x7feb352d); x ^= x >>> 15; x = Math.imul(x, 0x846ca68b); x ^= x >>> 16;
      return x >>> 0;
    };
    // the emission's key: the builder's id (FNV-1a 32 over its UTF-8 bytes) and its place among the tick's emissions
    const sprayKey = (id, n) => {
      let h = 0x811c9dc5;
      for (const b of new TextEncoder().encode(id)) h = Math.imul(h ^ b, 0x01000193) >>> 0;
      return mix32((h + Math.imul(n, 0x9e3779b9)) >>> 0);
    };
    // THE WIKI'S GENERATOR: draw k (0..5) of particle i of the burst made on `tick`, 0..0x7FFF
    const sprayDraw = (key, tick, i, k) => mix32((mix32((key ^ tick) >>> 0) ^ (i * 8 + k)) >>> 0) & 0x7fff;
    // 0x473B50's box: on each axis the base at lo + 4/11 of the span, the extent to 7/11 of it
    const sprayBox = (lo, hi) => {
      const out = [0, 0, 0, 0, 0, 0];
      for (let k = 0; k < 3; k++) {
        const d = (hi[k] - lo[k]) | 0, q4 = Math.trunc(Math.imul(4, d) / 11), q7 = Math.trunc(Math.imul(7, d) / 11);
        out[k] = (lo[k] + q4) | 0;
        out[3 + k] = (((lo[k] + q7) | 0) - out[k]) | 0;
      }
      return out;
    };
    // ONE BURST of 0x473D50 on `tick`: five particles; the particles it made, those past the pool's bound included
    const sprayBurst = (e, tick) => {
      let made = 0;
      for (let i = 0; i < 5; i++) {
        const at = [[0, 0, 0], [0, 0, 0]];
        for (let k = 0; k < 6; k++) {
          const b = e.box[Math.trunc(k / 3)];
          at[Math.trunc(k / 3)][k % 3] = (b[k % 3] + Math.trunc(sprayDraw(e.key, tick, i, k) * b[3 + k % 3] / 0x8000)) | 0;
        }
        const d = [0, 1, 2].map(k => (at[1][k] - at[0][k]) | 0);
        const len = trunc32(Math.sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        const steps = ((len >>> 18) << 16) >> 16;
        if (!steps) continue;
        made++;
        if (parts.length >= SPRAY_PARTS) { partsCut = true; continue; }
        parts.push([at[0][0], at[0][1], at[0][2], Math.trunc(d[0] / steps), Math.trunc(d[1] / steps), Math.trunc(d[2] / steps),
          tick + steps, 0xa1 + i % 7]);
      }
      return made;
    };
    /* THE UNIT'S SPRAY on tick `t`, in its turn after its build calls: QueryNanoPiece names the piece, the host gives
       its origin, the unit's facing turns it as the stage turns the model (a type that does not move at the heading
       (0, 1)), and the structure's box is the target; a reclaim only every second tick from its first on that
       structure */
    function spray(u, t) {
      const sp = u.spray, [fx, fz] = fp(sp.type), fixed = !mobile(u.type), cs = fixed ? -1 : -u.hz, sn = fixed ? 0 : u.hx;
      if (sp.how === 'reclaim') {
        if (u.sprayAt && u.sprayOn === sp.id && t - u.sprayAt < 2) return;
        u.sprayAt = t;
        u.sprayOn = sp.id;
      }
      const q = u.cob ? COB.query(u.cob, 'QueryNanoPiece', [0, 0, 0, 0], facts()) : { r: 0 };
      const piece = q.r > 0 ? q.io[0] : 0;
      const o = u.cob ? COB.origin(u.cob, piece) : { turned: [0, 0, 0], base: [0, 0, 0] };
      const pt = [
        (((fix16(u.x) + trunc32(cs * o.turned[0] + sn * o.turned[2])) | 0) + o.base[0]) | 0,
        (o.turned[1] + o.base[1]) | 0,
        (((fix16(u.z) - trunc32(-sn * o.turned[0] + cs * o.turned[2])) | 0) - o.base[2]) | 0,
      ];
      const lo = [(fix16(sp.x) - fx * 0x80000) | 0, 0, (fix16(sp.z) - fz * 0x80000) | 0];
      const hi = [(fix16(sp.x) + fx * 0x80000) | 0, COB.top(defIndex.indexOf(sp.type)), (fix16(sp.z) + fz * 0x80000) | 0];
      const point = sprayBox(pt, pt), target = sprayBox(lo, hi);
      const e = { box: sp.how === 'reclaim' ? [target, point] : [point, target], key: sprayKey(u.id, nemit++), due: t + 1 };
      if (!sprayBurst(e, t)) return;
      if (bursts.length < SPRAY_BURSTS) bursts.push(e); else partsCut = true;
    }
    // THE WALKER on tick `t`: every particle moves a step, its colour on, and dies past its end; then the bursts due
    function sprayTick(t) {
      for (const p of parts) {
        const low = (p[7] & 0xf) + 1;
        p[0] = (p[0] + p[3]) | 0; p[1] = (p[1] + p[4]) | 0; p[2] = (p[2] + p[5]) | 0;
        p[7] = (p[7] & 0xfff0) + (low > 7 ? 1 : low);
      }
      parts = parts.filter(p => t <= p[6]);
      const due = bursts.filter(e => e.due === t);
      bursts = bursts.filter(e => e.due !== t);
      for (const e of due) sprayBurst(e, t);
    }

    function advance() {
      const work = new Map(), jobs = new Map();
      // a builder in range works on s, and turns at once to face its centre (unchanged when it stands on the centre)
      const credit = (u, s, how) => {
        const dx = s.x - u.x, dz = s.z - u.z, d = Math.sqrt(dx * dx + dz * dz);
        if (d > 0) { u.hx = dx / d; u.hz = dz / d; }
        jobs.set(u, { s, how });
        /* the build orders' stance wait (0x438700, at the work's start: MobileBuild's work state 0x403E0C and the step
           0x41BA60 never read +0x10F): no progress on a work until the builder's script has set INBUILDSTANCE once */
        if (!(u.stanced && u.work === s)) { if (!stance(u)) return; u.stanced = true; }
        const w = work.get(s) || { build: 0, reclaim: 0 };
        w[how] += def(u.type).workerTime / def(s.type).buildTime / TPS * PACE;
        work.set(s, w);
        // the step made progress: the unit's turn sprays at the structure as it stands now
        u.spray = { how, id: s.id, type: s.type, x: s.x, z: s.z };
      };
      for (const u of units) { u.stepped = false; u.spray = null; }
      nemit = 0;
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
        if (w.build && s.built < 1) {
          s.built = Math.min(1, s.built + w.build);
          if (s.built >= 1) {
            log('finished', s);
            // 0x41B8D0: a structure finished whose FBI says ActivateWhenBuilt is switched on (0x41B9A6)
            if (COB && s.cob && (def(s.type).activatewhenbuilt & 1) === 1) COB.activate(s.cob, 1);
          }
        }
        if (w.reclaim) {
          s.reclaimed = Math.min(1, s.reclaimed + w.reclaim);
          if (s.reclaimed >= 1) { units = units.filter(v => v !== s); if (COB && s.cob) COB.remove(s.cob); log('reclaimed', s); }
        }
      }
      for (const u of units) {  // orders whose work is over end now, so the next one starts on the next tick
        const o = u.orders[0];
        if (!o) continue;
        if (o.k === 'build') { const s = structureAt(o.type, o.ox, o.oz); if (s && s.built >= 1) u.orders.shift(); }
        else if ((o.k === 'reclaim' || o.k === 'guard') && !byId(o.target)) u.orders.shift();
      }
      if (!COB) return;
      /* THE ENGINE'S CALLS OF THE UNIT'S TURN, unit by unit in creation order: its build calls, then its walk class.
         A builder's work is the structure it worked on this tick, waiting on its stance or not; when that changes
         StopBuilding goes for the old one (0x43A251, run-later, no argument: the order's destructor, in the same call
         as the build it finished) and StartBuilding for the new one (0x4385C7, run-later, one word: the target's angle
         from the builder's facing, 0 here since the builder faces it at once). A structure finished or reclaimed this
         tick is no one's work any more. Then the walk class (0x43DA70): a mobile unit that stepped this tick and stood
         the last starts StartMoving and MoveRate1 (no FBI the wiki reads sets moverate1, whose default 2 x MaxVelocity
         keeps its class 1), one that stood this tick and stepped the last StopMoving, both run-now. */
      for (const u of units) {
        const j = jobs.get(u);
        const target = j && units.includes(j.s) && !(j.how === 'build' && j.s.built >= 1) ? j.s : null;
        let ended = null;
        if (u.work && u.work !== target) {
          cobCall(u, 'StopBuilding', 'later'); ended = u.work.id; u.work = null; u.workOrder = null; u.stanced = false;
        }
        if (target && !u.work) { cobCall(u, 'StartBuilding', 'args', [0]); u.work = target; u.workOrder = u.orders[0]; }
        // a work that ended this tick sprays its last step by its own cadence, forgotten only then -- unless this
        // tick's spray was a new work's
        if (u.spray) spray(u, sim.tick + 1);
        if (ended !== null && u.sprayOn === ended) u.sprayAt = 0;
        if (!mobile(u.type)) continue;
        if (u.stepped && !u.moving) { cobCall(u, 'StartMoving', 'now'); cobCall(u, 'MoveRate1', 'now'); }
        else if (!u.stepped && u.moving) cobCall(u, 'StopMoving', 'now');
        u.moving = u.stepped;
      }
    }
    /* AN ORDER THE PLAYER TAKES AWAY ends its work at once: the order's destructor starts StopBuilding when the input
       is handled (0x43A251), before the next tick's slice */
    function dropWork() {
      if (!COB) return;
      for (const u of units) if (u.work && u.orders[0] !== u.workOrder) {
        cobCall(u, 'StopBuilding', 'later'); u.work = null; u.workOrder = null; u.stanced = false; u.sprayAt = 0;
      }
    }

    /* side: the layout side the sidebar shows, -1 with the sidebar off; iface: the Interface Type. Both are the state
       hash's first step with tick and done (tagpu_wikisim.h, tagpu_wikisim_hash). */
    const sim = { tick: 0, done: false, log: [], side: ON ? ctx.side : -1, iface: IFACE };
    const log = (what, u, type) => sim.log.push({ t: sim.tick, what, id: u.id, type: type || u.type, ox: u.ox, oz: u.oz });
    const script = scripted && demo ? resolve(demo, demo.inputs.map((e, i) => [e, i]).sort((a, b) => a[0].at - b[0].at || a[1] - b[1]).map(p => p[0]), IFACE) : [];
    /* THE DATA RULE OF A WAIT, refused here as tagpu_wikidata.c refuses it: `until` one of UNTILS, `max` an integer
       1..UNTIL_MAX, nothing else on the input and no other input on its tick */
    for (const e of script) {
      if (!('until' in e)) continue;
      const at = `demo ${demo.id}: the wait at tick ${e.at}`;
      if (!UNTILS.includes(e.until)) throw new Error(`${at} names no state the sim knows`);
      if (!(int(e.max) && e.max >= 1 && e.max <= UNTIL_MAX)) throw new Error(`${at} has no max of 1 to ${UNTIL_MAX} ticks`);
      if (!Object.keys(e).every(k => k === 'at' || k === 'until' || k === 'max')) throw new Error(`${at} carries another input`);
      if (script.some(f => f !== e && f.at === e.at)) throw new Error(`${at} shares its tick with another input`);
    }
    /* THE DEMO'S CLOCK: the tick its inputs, its trips and its end are written in. It runs with the sim's own tick
       except while a wait holds it (wait: {until, max, from}, from the tick it began); a wait that runs past its max
       stops the demo, `stalled`. A Try-it sim has no script, so its clock is its tick. */
    let next = 0, clock = 0, wait = null, stalled = false;
    const idle = () => !units.some(u => u.owner === 0 && u.orders.some(o => o.k === 'build' || o.k === 'reclaim'));
    // a tick's generated inputs come before its data inputs; within a trip's tick a move comes before a press
    const feed = t => {
      feeding = t;
      while (genNext < gen.length && gen[genNext].at <= t) input(gen[genNext++].ev);
      while (next < script.length && script[next].at === t) {
        const e = script[next++];
        if ('until' in e) wait = { until: e.until, max: e.max, from: sim.tick };
        else input(e);
      }
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

    sim.input = ev => { input(ev); dropWork(); cursorTick(sim.tick); };
    sim.step = () => {
      const t = sim.tick + 1;
      if (wait && idle()) wait = null;
      else if (wait && t - wait.from > wait.max) stalled = true;
      if (!wait) feed(++clock);
      dropWork();
      // the slice: every unit's DoScriptsNow(1), before its orders and its movement (0x48ADEB)
      if (COB) COB.slice(facts());
      advance();
      sprayTick(t);
      sim.tick = t;
      // a lock ends the demo on its own tick, which then holds under the entry's badge
      sim.done = clock >= end || sb.locked !== null || stalled;
      if (ON) route();
      st.hover = hover();
      cursorTick(t);
    };
    // a unit's orders, for a check: kind, building and site, or the target's id
    sim.orders = id => { const u = byId(id); return u ? u.orders.map(o => ({ ...o })) : null; };
    // each type's quarantine note, "" for none (tagpu_wikicob_note); [] without a host
    sim.cobNotes = () => COB ? COB.notes.slice() : [];
    // the host's handle on a unit by id, for the stage's pose and a check; 0 for none
    sim.cobUnit = id => { const u = byId(id); return u && u.cob || 0; };
    /* THE UNITS' POSES, index-aligned with view().units: each unit's posed model as its script leaves it (cob.js
       `model`), null for a unit drawn at rest -- one that runs no script, all without a host, and as the game's
       stage view carries them (tagpu_wikistage.c `tagpu_wikistage_pose`) any past its first WIKISTAGE_UNITS units,
       a model of more than WIKISTAGE_PIECES, or one past what the pool of WIKISTAGE_POSE primitives has left in
       unit order. Not the view, so neither the state hash nor the view the C sim is compared on carries it. */
    sim.poses = () => {
      let left = STAGE_POSE;
      return units.map((u, i) => {
        const m = COB && u.cob && i < STAGE_UNITS ? COB.model(u.cob) : null;
        if (!m || m.n <= 0 || m.n > STAGE_PIECES || m.n > left) return null;
        left -= m.n;
        return m;
      });
    };
    sim.view = () => {
      // the gesture's sites replace the cursor's single square; none (an empty ring) leaves the square standing
      const p = placed() && st.place.sites.length ? st.place : null;
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
        // the column, only with the sidebar on: the layout side it shows (its prefix and PANELSIDE are the stage's), its
        // rows, the routed screen, the shown unit and its page, each gadget's state, the unit under the cursor over the
        // map (the minimap's high dot)
        ...(ON ? {
          side: { index: ctx.side, h: LAY.height, screen: sb.screen, unit: sb.shown ? sb.shown.id : null, page: sb.shown ? sb.shown.page : 0,
            state: sideStates(), under: over() === 'map' ? (unitAt(st.cursor.x, st.cursor.z) || { id: null }).id : null },
        } : {}),
        // the type the demo stopped on (lock), with the sidebar or without: null while it plays
        locked: sb.locked,
        // the demo's clock, a wait holding it (its state, or null), and whether a wait ran past its max
        clock, waiting: wait ? wait.until : null, stalled,
        // the COB host's state hash (tagpu_wikicob_hash) as two uint32 halves, null without a host
        cob: COB ? COB.hash() : null,
        // the nano spray's live particles in the order made, each [x, y, z, its step's x, y, z, end, colour] (16.16
        // world units, x, altitude, z), and whether a bound dropped one
        parts: parts.map(p => p.slice()), partsCut,
        // the second bursts waiting, in the order made, each [its two boxes' twelve words, due, key]
        bursts: bursts.map(e => [...e.box[0], ...e.box[1], e.due, e.key]),
      };
    };

    if (ON) route();
    if (lock0) lock(lock0.type);
    feed(0);
    sim.done = sb.locked !== null;
    st.hover = hover();
    st.cursorShape = cursorShape();
    st.cursorSince = 0;
    return sim;
  }

  // a layout is checked whole before any sim reads it; null or absent is no sidebar: the map alone on the stage,
  // with the placement rules (arm keeping the Shift bit and a same-type gesture, placed) as with one
  const checked = layout => layout == null ? null : checkLayout(layout);
  return {
    create: (entry, demo, layout, ctx) => make(entry, demo, false, checked(layout), ctx),
    play: (entry, demo, layout, ctx) => make(entry, demo, true, checked(layout), ctx),
    // the bounds, for the caller that falls back to the fixture on a refused layout
    checkLayout,
    // the casting's bounds, and the bound entry (its titles name the casts' units for the text's role words)
    checkCasting, bindEntry, CASTS,
    // the button an action presses at an Interface Type, for the keys strip and the captions' button words
    button,
    // the trip's fixed budget and its table, which tagpu_wikisim.h #defines and simparity compares
    TRIP, TRIP_DOWN, TRIP_UP, TRIP_ROWS,
    // the shapes alone, for checks against tagpu_placegeo.c
    shapes: { placeShape, placeRing },
  };
})();
if (typeof module !== 'undefined') module.exports = WikiSim;
