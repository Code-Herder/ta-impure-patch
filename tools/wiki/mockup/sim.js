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
   interface the demos are authored in. tagpu_wikisim.c's WIKISIMCTX is the same three, and the sources of the units'
   scripts (ctx.cob, cob.js) and sounds (ctx.snd: type(name) answers {cat, count}, the type's sounds.tdf category
   and its variant count per kind, or null for a silent type).

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
  // tagpu_wikicob.h: WIKICOB_UNITS, the units one host spawns over its life, and the ACTIVATION and INBUILDSTANCE ids
  const COB_UNITS = 1024, COB_ACTIVATION = 1, COB_INBUILDSTANCE = 5;
  // tagpu_wikistage.h: the stage view's units, its pose pool and one posed model's primitives
  const STAGE_UNITS = 128, STAGE_POSE = 4096, STAGE_PIECES = 256;
  // tagpu_wikisim.h, THE NANO SPRAY: the live particles and the second bursts waiting, at most
  const SPRAY_PARTS = 1024, SPRAY_BURSTS = 256;
  /* tagpu_wikisim.h, THE SOUND: the kind table 0x5086D8's priority and interval in seconds by kind, the notice queue's
     entries, the gap between two replies with sound, the events kept, and the allsound names the wiki plays by
     WIKI_SNDNAME_* */
  const SND_PRIORITY = [0, 10, 9, 4, 4, 5, 3, 8, 3, 4, 3, 2, 7, 7, 7, 7, 4, 10, 10, 10, 10, 10, 10, 10];
  const SND_INTERVAL = [0, 0, 20, 2, 2, 1, 4, 1, 3, 2, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0];
  const SND_KINDS = 24, SND_QUEUE = 8, SND_GAP = 30, SND_LOG = 32, INT_MAX = 2147483647;
  /* a unit's death, an event of a game's: past the kinds, so it indexes no per-kind table and a player that knows no
     death plays nothing for it (tagpu_wikisim.h WIKI_SND_DEATH) */
  const SND_DEATH = SND_KINDS;
  const SND = { select: 1, activate: 3, deactivate: 4, ok: 5, arrived: 6, cant: 7, complete: 8, build: 9, working: 11 };
  const SND_NAMES = ['SelectMultipleUnits', 'oktobuild', 'notoktobuild', 'addbuild', 'buildbutton', 'ordersbutton',
    'nextbuildmenu', 'immediateorders', 'Options', 'specialorders'];
  const SNDNAME = Object.fromEntries(SND_NAMES.map((n, i) => [n, i]));
  /* A DEMO'S WAIT, {"at", "until", "max"} (research/notes/ingame-wiki-cob-plan.md, decision 10): the demo's clock
     stops at `at` until the state `until` names holds, at most `max` ticks, after which the demo stops and says so.
     UNTILS are the states, in tagpu_wikidata.h's WIKI_UNTIL_* order from 1: "idle", no unit of the player's holds a
     build or a reclaim order; "arrived", no unit of the player's holds a move order (each reached its place or
     stopped close by it); "built", the demo's k-th such wait, k buildings of the player's have been finished in the
     demo -- a count, so a building finished before the wait counts too. A demo waits
     on the state its next input or its claim needs instead of timing it to a walk, whose length the units in the
     way decide. UNTIL_MAX bounds `max`. */
  const UNTILS = ['idle', 'arrived', 'built'], UNTIL_MAX = 3600;
  const KEYS = ['shift', 'alt', 'ctrl', 'z', 'x', 'e', 'lmb', 'rmb'];
  /* THE RECLAIM SWEEP's kinds, tagpu_sweep.h's SWEEP_METAL, SWEEP_ENERGY and SWEEP_ALL in order (the popup's rows top
     down); the press travel past which a press is a ring (tagpu_sweep.c DRAG_PX, screen pixels, the stage's world
     units at zoom 1) and the ring's bound (RING_MAX_WU); the popup, in the engine's 1x pixels (POP_X, POP_W, POP_H) */
  const KINDS = ['metal', 'energy', 'all'], RECL_DRAG = 7, RING_MAX = 4096, POP_X = 129, POP_W = 124, POP_H = 72;
  const SWEEP_LIST_CAP = 1024;
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
     screens: {NAME: {y, w, h, gadgets: [{name, role, rect: [x, y, w, h], unit, grey, hidden, key}]}}}.
     A screen's y is its row in the column and a gadget's rect is in its screen's pixels. role is orders, buildtab,
     build, prev, next, stop or order, as 0x41AA00 tells them apart by name; a build button's unit is its unit type,
     or null (greyed) when the name is none; hidden marks an inactive gadget; key is the record's quickkey +0x13A, 1..255,
     absent for none. Labels, fonts and the panel picture
     are not gadgets here: they are the art's. A side's types are the types with a page (def+0x22E != 0) that side's
     casting names; its pages are read with its own prefix, so one type name may have other pages in another side.
     The sim reads the side ctx.side names, MAIN2, GEN and types alike. */
  const SIDE_W = 128, SIDE_MIN_H = 480, MINIMAP = 128;
  const SIDE_SCREENS = 48, SIDE_GADGETS = 64, SIDE_PAGES = 7, SIDE_TYPES = 64, SIDE_ROWS = 768, SIDE_SIDES = 8, SIDE_NAME = 32;
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
      /* SIDE_ROWS: the deepest column, 768 rows, every compatibility setup's: a map grows to the column, and every
         stage description's ground is 768 / CELL rows deep (tools/wiki/stagemap.py GROUND_ROWS) */
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
        if ('key' in g && !(int(g.key) && g.key >= 1 && g.key <= 255)) no(`${at}: key is not 1..255`);
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
    checkSight(L.sight, no);
    return L;
  }
  /* THE SIGHT TABLE of a layout's mod (tagpu_wikidata.h WIKISIGHT, which states the engine's loader and stamp it
     reduces, and why only the tables the setup's types reach are kept): `sight`, absent or null for none, else
     {numtables, tables}: numtables an integer 2..SIGHT_NUMTABLES and tables a list of at most SIGHT_IDX {idx, cells},
     idx strictly increasing within 1..numtables - 1 and cells a count grid, a list of [dc, dr, n] -- the block offset
     (column, row) from the unit's own and the number of ray points on it -- strictly increasing by (dr, dc), |dc|,
     |dr| <= SIGHT_REACH, n 1..255, at most SIGHT_CELLS cells over every table. A unit of sight s lights the cells of
     table sightIndex(S, s) and its own block. */
  const SIGHT_NUMTABLES = 32767, SIGHT_IDX = 32, SIGHT_REACH = 63, SIGHT_CELLS = 32768;
  function checkSight(S, no) {
    if (S === undefined || S === null) return;
    if (!obj(S) || !(int(S.numtables) && S.numtables >= 2 && S.numtables <= SIGHT_NUMTABLES) || !Array.isArray(S.tables) ||
        S.tables.length > SIGHT_IDX) no('the sight table is not {numtables, tables}');
    let n = 0;
    S.tables.forEach((T, i) => {
      if (!obj(T) || !int(T.idx) || T.idx < 1 || T.idx >= SIGHT_NUMTABLES || !Array.isArray(T.cells) || (n += T.cells.length) > SIGHT_CELLS)
        no('the sight table is not {numtables, tables}');
      T.cells.forEach((c, k) => {
        if (!Array.isArray(c) || c.length !== 3 || !c.every(int) || Math.abs(c[0]) > SIGHT_REACH || Math.abs(c[1]) > SIGHT_REACH ||
            c[2] < 1 || c[2] > 255) no(`a sight table's cell is past ${SIGHT_REACH} blocks or counts nothing`);
        if (k && (c[1] < T.cells[k - 1][1] || (c[1] === T.cells[k - 1][1] && c[0] <= T.cells[k - 1][0])))
          no('a sight table\'s cells are not in order by row, then column, each once');
      });
      if (T.idx >= S.numtables || (i && T.idx <= S.tables[i - 1].idx))
        no('a sight table\'s indices are not increasing within 1..numtables - 1');
    });
  }
  /* THE RADIUS INDEX of a sight over a layout's table S, the engine's min(trunc(sight / 32), numtables - 1)
     (0x48230D..0x482366); 0 below SIGHT_MIN or without a table, where no table is read (tagpu_wikidata_sight_index) */
  const sightIndex = (S, s) => s < SIGHT_MIN || !S ? 0 : Math.min(Math.trunc(s / 32), S.numtables - 1);
  // the stored table {idx, cells} a sight stamps with, or null (tagpu_wikidata_sight_table)
  const sightTable = (S, s) => { const i = sightIndex(S, s); return i ? S.tables.find(T => T.idx === i) || null : null; };

  /* THE LIT GRID of `seat` in a game's view v at the shown tick g, over the layout's sight table S (null: none):
     tagpu_wikisim_sight_lit, whose comment states the engine's rule (the emit 0x4827B0, the emitter 0x4825B0, the
     stamp 0x482270) and M0-P1's measurement. A byte a block on the (w >> 1) x (h >> 1) grid, row-major, wrapping as the
     engine's counters do. Each unit of the seat stamps, and each of its deaths with g <= tick + 60; without a table
     every block is 1. */
  const SIGHT_SPAN = 60;
  // the clicks a game keeps (make's `clicks`): the overlay's ring, OV_RING (tagpu_replay.c:484)
  const CLICKS = 64;
  function sightLit(v, S, seat, g) {
    const gw = v.map.w >> 1, gh = v.map.h >> 1, lit = new Uint8Array(gw * gh);
    if (!S) return lit.fill(1);
    const T = new Map(v.game.types.map(t => [t.name, t]));
    const stamp = (x, z, k) => {
      if (!k) return;
      const h = Math.min(255, k.modelHeight + 1), c = Math.floor(x) >> 5, r = (Math.floor(z) - (h >> 1)) >> 5;
      if (c < 0 || c >= gw || r < 0 || r >= gh) return;
      lit[r * gw + c] = (lit[r * gw + c] + 1) & 255;
      const tab = sightTable(S, k.sightDistance);
      if (tab) for (const [dc, dr, n] of tab.cells) {
        const cc = c + dc, rr = r + dr;
        if (cc >= 0 && cc < gw && rr >= 0 && rr < gh) lit[rr * gw + cc] = (lit[rr * gw + cc] + n) & 255;
      }
    };
    for (const u of v.units) if (u.owner === seat) stamp(u.x, u.z, T.get(u.type));
    for (const d of v.game.deaths) if (d.owner === seat && g <= d.tick + SIGHT_SPAN) stamp(d.x, d.z, T.get(d.type));
    return lit;
  }
  /* THE CONTACTS of `seat` in a game's view v, over its lit grid `lit` (sightLit): per view unit 1 when the radar
     pass would mark it, else 0 -- tagpu_wikisim_contacts, whose comment states the rule (0x467440's loops 2 and 5,
     0x467840). The distance is the engine's 64-bit d^2 on 16.16 positions, here in BigInt: dx^2 reaches 2^54. */
  function sightContacts(v, lit, seat) {
    const gw = v.map.w >> 1, gh = v.map.h >> 1, T = new Map(v.game.types.map(t => [t.name, t]));
    const fix = x => BigInt(Math.floor(x * 65536));
    const radars = [];
    for (const r of v.units) {
      const k = T.get(r.type), R = k ? Math.max(k.radarDistance, k.sonarDistance) : 0;
      if (r.owner === seat && r.built >= 1 && k && k.activates && R > 0) radars.push([fix(r.x), fix(r.z), BigInt(R) * BigInt(R)]);
    }
    return v.units.map(u => {
      if (u.owner === seat) return 0;
      const c = Math.floor(u.x) >> 5, r = Math.floor(u.z) >> 5;
      if (c >= 0 && c < gw && r >= 0 && r < gh && lit[r * gw + c]) return 1;
      const ux = fix(u.x), uz = fix(u.z);
      return radars.some(([rx, rz, R2]) => ((ux - rx) * (ux - rx) >> 32n) + ((uz - rz) * (uz - rz) >> 32n) < R2) ? 1 : 0;
    });
  }

  /* ---- casts and castings (research/notes/ingame-wiki.md, the owner's decisions 13-15 and 20) ----
     Each unit of an entry names its cast, its place in the demos; a casting answers each cast with one of the mod's
     types for one side, or null when the rule found none (unbound). The casting is a pack's
     (gfx/side/<mod>/side.json `casting`, index-aligned with the layout's sides) or tools/wiki/casting-fixture.json's.
     Every bound below is one tagpu_wikidata.h's WIKICASTDEF holds; a casting breaking one is refused whole.
     The commander comes last so that each earlier cast keeps its index, and each casting's `builds` its meaning. */
  const CASTS = ['builder', 'advbuilder', 'mex', 'moho', 'geo', 'walker', 'tower', 'wall', 'solar', 'commander'];   // WIKI_CAST_* order
  /* the figures a cast carries and a bound unit takes, from its FBI: the six the world reads (WORLD), each finite and
     >= 0, then the economy's build costs, metal made a second and the two storages, which only a game reads: a casting
     holds them finite, and a game to the economy's bounds where it binds (econFits) */
  const FIGURES = ['buildTime', 'workerTime', 'buildDistance', 'maxVelocity', 'extracts', 'energyMake',
    'buildCostMetal', 'buildCostEnergy', 'metalMake', 'metalStorage', 'energyStorage'];
  const WORLD = FIGURES.slice(0, 6);
  // ECON_MAX: the bound of a signed economy figure, energyUse (a solar collector's is negative), tagpu_wikidata.h's WIKI_ECON_MAX
  const ECON_MAX = 1e7;
  // the economy's figures an entry's unit may carry as a cast does: the five held to >= 0, and energyUse to ECON_MAX
  const ECON = ['buildCostMetal', 'buildCostEnergy', 'metalMake', 'metalStorage', 'energyStorage'];
  /* a cast's economy within the bounds a game's economy rests on (tagpu_wikidata_cast_economy): an absent figure is 0.
     Outside them a game leaves the cast unbound (binder); a page entry reads none of them */
  const econFits = k => ECON.every(n => !(n in k) || k[n] >= 0) && (!('energyUse' in k) || Math.abs(k.energyUse) < ECON_MAX);
  /* THE SIGHT FIGURES (tagpu_wikidata.h WIKICASTDEF's), with their bounds, an absent one 0: SightDistance, RadarDistance
     and SonarDistance as the def's signed words hold them (every engine reader is movsx), and modelHeight, def+0x170's
     byte, the model's height (0x4CB5F0). A players entry's game leaves a cast below SIGHT_MIN unbound (sightFits), and
     refuses an own type below it: radius index 0 is the engine's read before the sight table */
  const SIGHT = [['sightDistance', -32768, 32767], ['radarDistance', -32768, 32767], ['sonarDistance', -32768, 32767],
    ['modelHeight', 0, 255]];
  const SIGHT_MIN = 32;
  /* a type of figures k plays in a players entry's game over the layout's sight table S (null: none): its sight at
     least SIGHT_MIN and, with a table, its radius index stored (tagpu_wikidata_sight_ok). The binder leaves a cast that
     fails it unbound and make refuses an entry's own type that does, so the stamp never reads a table the layout does
     not hold */
  const sightFits = (k, S = null) => {
    const s = 'sightDistance' in k ? k.sightDistance : 0;
    return s >= SIGHT_MIN && (!S || sightTable(S, s) !== null);
  };
  const sightBad = (k, n, lo, hi) => n in k && !(int(k[n]) && k[n] >= lo && k[n] <= hi);
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
  /* A FEATURE'S FIGURES, as an entry's `features` table gives them from its TDF section (tools/wiki/gen-reclaim.mjs):
     footprint [fx, fz] cells, 1..FEAT_FOOT (the reach a feature's centre has from its anchor, tagpu_sweep.c FOOT_MAX);
     metal and energy (FeatureDef +0xF0 and +0xEC), at least 0 and below FEAT_AMOUNT; height (+0xFA), 0..255;
     blocking and reclaimable (+0xFE bit 7) as booleans; object, a 3D feature's model (its section's `object=`, a wreck
     or a heap), 1..31 bytes, which the whitebox draws as a wreck. Every bound is one tagpu_wikidata.c holds. */
  const FEAT_FOOT = 16, FEAT_AMOUNT = 1e7, FEATS_MAX = 256, FEAT_DEFS = 64;
  function checkFeature(n, d) {
    const at = `feature ${n}`;
    if (!fitsIn(n, 32) || !n.length) throw new Error(`${at}: the name is not 1 to 31 bytes`);
    const f = obj(d) && d.footprint;
    if (!Array.isArray(f) || f.length !== 2 || !f.every(v => int(v) && v >= 1 && v <= FEAT_FOOT))
      throw new Error(`${at}: footprint is not two integers 1..${FEAT_FOOT}`);
    for (const k of ['metal', 'energy'])
      if (k in d && !(typeof d[k] === 'number' && d[k] >= 0 && d[k] < FEAT_AMOUNT)) throw new Error(`${at}: ${k} is not 0..${FEAT_AMOUNT}`);
    if ('height' in d && !(int(d.height) && d.height >= 0 && d.height <= 255)) throw new Error(`${at}: height is not 0..255`);
    for (const k of ['blocking', 'reclaimable']) if (k in d && typeof d[k] !== 'boolean') throw new Error(`${at}: ${k} is not a boolean`);
    if ('object' in d && !(typeof d.object === 'string' && d.object.length && fitsIn(d.object, 32)))
      throw new Error(`${at}: object is not 1 to 31 bytes`);
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
      // the world's figures divide and step: a build time of 0 would fill a building in no time at all, and a negative
      // or infinite speed or reach would leave the world's arithmetic; the economy's need only be finite here
      for (const n of WORLD) if (n in k && !(typeof k[n] === 'number' && k[n] >= 0 && k[n] < Infinity)) no(`${at}: ${n} is not a finite number >= 0`);
      for (const n of ECON) if (n in k && !(typeof k[n] === 'number' && Number.isFinite(k[n]))) no(`${at}: ${n} is not a finite number`);
      if ('energyUse' in k && !(typeof k.energyUse === 'number' && Number.isFinite(k.energyUse))) no(`${at}: energyUse is not a finite number`);
      if (!(k.buildTime > 0)) no(`${at}: buildTime is not above 0`);
      if ('canstop' in k && !int(k.canstop)) no(`${at}: canstop is not an integer`);
      if ('canreclamate' in k && !int(k.canreclamate)) no(`${at}: canreclamate is not an integer`);
      if ('activatewhenbuilt' in k && !int(k.activatewhenbuilt)) no(`${at}: activatewhenbuilt is not an integer`);
      for (const [n, lo, hi] of SIGHT) if (sightBad(k, n, lo, hi)) no(`${at}: ${n} is not an integer ${lo}..${hi}`);
      if ('vent' in k && typeof k.vent !== 'boolean') no(`${at}: vent is not a boolean`);
      checkG(at, k, f);
      if ('builds' in k && !(Array.isArray(k.builds) && k.builds.every(b => CASTS.includes(b)))) no(`${at}: builds is not a list of casts`);
      /* the cast's wreck, its FBI's `corpse` feature (null or absent: none): a feature record under checkFeature's
         rules and its section's name. The name is checked first so that checkFeature's messages, which quote it, stay
         within the C reader's message */
      if ('corpse' in k && k.corpse !== null) {
        const f = k.corpse;
        if (!obj(f) || !fitsIn(f.name, 32) || !f.name.length) no(`${at}: corpse is not a feature record with a name of 1 to 31 bytes`);
        try { checkFeature(f.name, f); } catch (e) { no(`${at}: corpse: ${e.message}`); }
      }
    }
    return C;
  }
  /* THE BOUND ENTRY: the entry with every unit of a bound cast replaced by the casting's type, under the type's name,
     with the casting's title (as `name`), footprint, figures, canstop, canreclamate, activatewhenbuilt, sight figures and G cells, and a build list that holds unit j
     of the entry exactly when the casting's build list names j's cast and j is on the unit's own side (a rival or
     not). A unit of an unbound cast -- in a game, also of a cast whose economy is outside the game's bounds
     (econFits), and in a players entry's game of one below the sight floor or whose sight table the layout does not store
     (sightFits) -- keeps the authored definition, marked `unbound`, and its build list keeps only the entry's own units, renamed as they are. The
     units keep their order, so the scene's units and a `build` point at the same definitions as before; the scene's
     types and the inputs' `build` are renamed with them, captions untouched (the sim and its hash keep raw
     captions). Every unit names one of the ten casts and no two units of one side one cast; two units bound to one
     name are refused.

     THE OPPONENT (a game entry's `rival` units, the plan's decision 19): a rival unit binds to `rival`'s cast as a
     player's unit binds to `casting`'s, and keeps its authored definition, not marked unbound, when no rival casting
     is given.

     THE WRECKS, in a game entry only (`game`): a bound unit's corpse is its cast's (`corpse`, the name of a row of the
     bound entry's `features`), and the bound entry's features table is the entry's with each such cast's wreck, in
     unit order, put over the row of its name or appended after the rows; past FEAT_DEFS rows the demo is refused. A
     unit keeping its authored definition keeps its authored corpse. Outside a game entry a cast's corpse stays in the
     casting. tagpu_wikisim.c builds the same as WIKISIM's own bdef[] (a field of the sim, so it lives and dies with it). */
  function binder(entry, casting, rival = null, game = false, sight = null) {
    const K = checkCasting(casting).casts, R = rival ? checkCasting(rival).casts : null;
    const names = Object.keys(entry.units), to = {}, seen = new Set();
    const isRival = t => entry.units[t].rival === true;
    /* the cast record a unit binds to: null for an unbound cast, undefined for a rival with no rival casting. A game's
       economy reads the cast's economy figures, so a game leaves a cast whose figures are outside its bounds unbound */
    const castFor = t => {
      const k = isRival(t) ? (R ? R[entry.units[t].cast] : undefined) : K[entry.units[t].cast];
      return k && game && (!econFits(k) || (entry.players === true && !sightFits(k, sight))) ? null : k;
    };
    for (const t of names) {
      const c = entry.units[t].cast;
      if (!CASTS.includes(c)) throw new Error(`unit ${t} names no cast`);
      if (seen.has(`${isRival(t)}/${c}`)) throw new Error(`two ${isRival(t) ? 'rival ' : ''}units name the cast ${c}`);
      seen.add(`${isRival(t)}/${c}`);
      to[t] = castFor(t) ? castFor(t).type : t;
    }
    if (new Set(names.map(t => to[t])).size !== names.length) throw new Error('the casting binds two units to one type');
    const units = {}, features = game ? { ...(entry.features || {}) } : entry.features;
    for (const t of names) {
      const d = entry.units[t], k = castFor(t);
      if (!k) {
        units[t] = { ...d, ...(k === null ? { unbound: true } : {}), builds: (d.builds || []).filter(b => own(entry.units, b)).map(b => to[b]) };
        continue;
      }
      const b = { cast: d.cast, ...(isRival(t) ? { rival: true } : {}), name: k.title, footprint: [k.footprint[0], k.footprint[1]],
        builds: names.filter(j => isRival(j) === isRival(t) && (k.builds || []).includes(entry.units[j].cast)).map(j => to[j]) };
      for (const n of FIGURES) if (n in k) b[n] = k[n];
      if ('energyUse' in k) b.energyUse = k.energyUse;
      if ('canstop' in k) b.canstop = k.canstop;
      if ('canreclamate' in k) b.canreclamate = k.canreclamate;
      if ('activatewhenbuilt' in k) b.activatewhenbuilt = k.activatewhenbuilt;
      for (const [n] of SIGHT) if (n in k) b[n] = k[n];
      if (k.vent) b.vent = true;
      if (k.g && k.g.length) b.g = k.g.map(c => [c[0], c[1]]);
      if (game && k.corpse) {
        const { name, ...row } = k.corpse;
        features[name] = row;
        b.corpse = name;
      }
      units[to[t]] = b;
    }
    // FEAT_DEFS: tagpu_wikidata.c's table of an entry's features, which the bound one is too
    if (game && Object.keys(features).length > FEAT_DEFS) throw new Error('the casting\'s corpses overflow the features table');
    const scene = s => s && s.units ? { ...s, units: s.units.map(u => own(to, u.type) ? { ...u, type: to[u.type] } : u) } : s;
    const demo = d => d && { ...d, scene: scene(d.scene),
      inputs: d.inputs && d.inputs.map(e => typeof e.build === 'string' && own(to, e.build) ? { ...e, build: to[e.build] } : e) };
    const bound = { ...entry, units, scenes: Object.fromEntries(Object.entries(entry.scenes).map(([n, s]) => [n, scene(s)])) };
    if (game) bound.features = features;
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
  /* THE PICK TRIP, a scripted `pick` of a kind: the glide to the popup's row, its press at PICK_DOWN and release at
     PICK_UP, and the glide back, PICK ticks whatever the layout; the row is where the popup stands when the pick is fed */
  const PICK = 60, PICK_DOWN = 20, PICK_UP = 26;
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
  /* ---- tagpu_sweepgeo.c, line for line: the reclaim sweep's ring, its order and its split. Positions are world
     units and small integers, so a squared distance is exact in a double as the C's uint64 is. ---- */
  const SWEEP_MAX_TARGETS = 1024;
  const dist2 = (ax, az, bx, bz) => sq(ax - bx) + sq(az - bz);
  const sweepInside = (cx, cz, r, x, z) => r >= 0 && Math.abs(x - cx) <= r && Math.abs(z - cz) <= r && dist2(x, z, cx, cz) <= r * r;
  // the targets' indices nearest (cx, cz) first; ties by z, then x, then index: a total order, so one result
  function sweepOrder(t, cx, cz) {
    const idx = t.slice(0, SWEEP_MAX_TARGETS).map((_, i) => i);
    const d = idx.map(i => dist2(t[i].x, t[i].z, cx, cz));
    return idx.sort((a, b) => d[a] - d[b] || t[a].z - t[b].z || t[a].x - t[b].x || a - b);
  }
  /* the ticks a Reclaim order works on a feature, 2 a working call: the handler 0x404AD0 sets the countdown to
     trunc(15 + (energy + metal) / 2) and takes 2 off it a call; no worker-time term enters it. The amounts are the
     FeatureDef's floats (+0xEC, +0xF0), as tagpu_sweepgeo.c's sweep_reclaim_ticks takes them */
  const reclaimCountdown = (energy, metal) => {
    const e = Math.fround(energy), m = Math.fround(metal);
    return Math.floor(15 + ((e > 0 ? e : 0) + (m > 0 ? m : 0)) / 2);
  };
  const sweepReclaimTicks = (energy, metal) => 2 * Math.ceil(reclaimCountdown(energy, metal) / 2);
  /* the split: until no target is left or no builder can take one, the builder with the least load that has room and
     can reach a remaining target (ties: lowest index) takes the remaining target nearest its point; its load grows by
     travel + ticks, travel = max(0, d - reach) / speed. b is [{x, z, speed, reach, load, room}], updated in place;
     returns [{who, seq}] in assignment order. */
  function sweepSplit(b, t) {
    const nt = Math.min(t.length, SWEEP_MAX_TARGETS), taken = new Array(nt).fill(false), stuck = new Array(b.length).fill(false), out = [];
    for (let left = nt; left; ) {
      let best = -1, near = -1, bd = 0;
      for (let i = 0; i < b.length; i++)
        if (b[i].room && !stuck[i] && (best < 0 || b[i].load < b[best].load)) best = i;
      if (best < 0) break;
      const s = b[best];
      for (let k = 0; k < nt; k++) {
        if (taken[k]) continue;
        const dk = dist2(s.x, s.z, t[k].x, t[k].z);
        if (near < 0 || dk < bd) { near = k; bd = dk; }
      }
      const d = Math.sqrt(bd);
      let travel;
      if (!(s.speed > 0)) {
        if (!(d <= s.reach)) { stuck[best] = true; continue; }
        travel = 0;
      } else travel = d > s.reach ? (d - s.reach) / s.speed : 0;
      s.load += travel + t[near].ticks;
      s.x = t[near].x; s.z = t[near].z;
      s.room--;
      taken[near] = true; left--;
      out.push({ who: best, seq: near });
    }
    return out;
  }

  // the footprint's first cell for a centre at world v: 0x4197D0's rounding, floor((v - 8f + 8) / 16), half up
  const origin = (v, f) => Math.floor((v - 8 * f + 8) / CELL);
  // 0x43AFC0's predicate: within 16 world units of the node on both axes, inclusive
  const within = (p, q) => Math.abs(p.x - q.x) <= 16 && Math.abs(p.z - q.z) <= 16;

  /* THE ENTRY'S SCENES (format 2): `scenes` names each, in order, and a demo plays on the one its `on` names, the first
     when it names none. Every scene has the first one's map size, so the stage keeps one size from demo to demo of an
     entry. tagpu_wikidata.c refuses at load what this refuses as a demo starts. */
  const SCENE_NAME = /^[a-z][a-z0-9]{0,15}$/;
  function sceneOf(entry, demo) {
    if (entry.format !== 2) throw new Error('the entry is not format 2');
    const names = obj(entry.scenes) ? Object.keys(entry.scenes) : [];
    if (!names.length) throw new Error('the entry has no scenes');
    for (const n of names) {
      if (!SCENE_NAME.test(n)) throw new Error(`the scene name ${JSON.stringify(n)} is not 1 to 16 of a-z and 0-9, a letter first`);
      const m = obj(entry.scenes[n]) && entry.scenes[n].map, m0 = entry.scenes[names[0]].map;
      if (!obj(m) || m.w !== m0.w || m.h !== m0.h) throw new Error(`the scene ${n}'s map is not the first scene's size`);
    }
    const on = demo && demo.on !== undefined ? demo.on : names[0];
    if (typeof on !== 'string' || !own(entry.scenes, on)) throw new Error(`the demo is on ${JSON.stringify(on)}, no scene of the entry`);
    return entry.scenes[on];
  }

  /* A TEXT ENTRY: a page entry whose `demos` is absent or an empty list. Its page is its title, subtitle, badge and
     text and no stage, so it carries nothing a stage or the asset loader would read: no frame, scenes, units, features
     or pace. A `demos` present and not a list refuses any entry. The message, or null; tagpu_wikidata_entry refuses at
     load what this refuses, in this order and with these words. */
  const TEXT_KEYS = [['frame', 'a frame'], ['scenes', 'scenes'], ['units', 'units'], ['features', 'features'], ['pace', 'a pace']];
  function checkText(entry) {
    if (entry.demos !== undefined && !Array.isArray(entry.demos)) return 'has demos that are not a list';
    if (entry.demos !== undefined && entry.demos.length) return null;
    for (const [k, what] of TEXT_KEYS) if (entry[k] !== undefined) return `is a text entry with ${what}`;
    return null;
  }

  /* the demo's scene with the demo's additions: units, features, metal, vents and fog appended; selected, cursor and
     the reclaim kind replaced when the demo gives them */
  function merge(entry, demo) {
    const a = sceneOf(entry, demo), b = (demo && demo.scene) || {};
    const cat = k => [...(a[k] || []), ...(b[k] || [])];
    return { map: a.map, metal: cat('metal'), vents: cat('vents'), fog: cat('fog'), units: cat('units'),
      features: cat('features'), selected: b.selected ?? a.selected ?? [], cursor: b.cursor ?? a.cursor ?? [0, 0],
      kind: b.kind ?? a.kind ?? 'all' };
  }

  /* THE CONTEXT, refused as tagpu_wikisim_init refuses it: an Interface Type other than 0 or 1 (the DLL maps the
     engine's int to these two before it starts a demo), and with a layout a side that is not one of its sides. `rival`
     is the opponent's casting, read only by a game entry's rival units (binder). */
  function context(layout, ctx) {
    const c = ctx == null ? { side: 0, casting: null, iface: 1 } : ctx;
    if (!(c.iface === 0 || c.iface === 1)) throw new Error(`the interface type ${c.iface} is not 0 or 1`);
    if (layout && !(int(c.side) && c.side >= 0 && c.side < layout.sides.length)) throw new Error(`side ${c.side} is not one of the layout's sides`);
    if (!(c.casting == null || obj(c.casting))) throw new Error('the casting is not an object or null');
    if (!(c.rival == null || obj(c.rival))) throw new Error('the rival casting is not an object or null');
    return { side: layout ? c.side : -1, casting: c.casting ?? null, rival: c.rival ?? null, iface: c.iface, cob: c.cob || null,
      snd: c.snd || null };
  }

  /* ---- THE GAME ENTRY (research/notes/ingame-wiki-replay-plan.md): an entry whose `frame` is "game" shows a small
     game, and each demo's `game` is its game script, played by a game simulation (WikiSim.game) on game ticks. Its
     data rules, refused here as tagpu_wikidata.c refuses them at load: ----
     - `frame` absent or "game"; a game entry has no `pace`.
     - a unit may carry `rival` (a boolean: it is the opponent's, cast by the rival casting) and `corpse` (a row of
       the entry's `features`, its wreck) only in a game entry, and any entry's unit the economy's figures, ECON each
       finite and >= 0, energyUse below ECON_MAX in magnitude.
     - every scene of a game entry carries `seats`, 1..SEATS objects {name, metal, energy} and nothing else, the seat
       being the index: a name of 1..SEAT_NAME - 1 bytes with no "{" (HUDSEAT's name field), metal and energy integers
       SEAT_MIN..SEAT_MAX, the skirmish's settings; every scene unit's `owner` (0 when absent) is one of its seats. A
       page entry's scenes and every demo's scene carry no `seats`.
     - a game entry's demo carries `game` {end, inputs, sidebar}: end an integer 1..GAME_END_MAX, inputs a list,
       sidebar a boolean when given; no demo-level `sidebar`. A page entry's demo carries no `game`.
     - the game script's inputs: each has a numeric `at`; no caption and no wait (game time is the game
       simulation's tick, so nothing may hold it); `seat`, when given, the seat whose cursor gives the input (decision
       16; 0 when absent): an integer 0..the scene's seats - 1, and 0 alone unless the game's sidebar is false, since
       the column is seat 0's and a game whose sidebar may be on gives no other seat an input (an over-approximation:
       an absent sidebar is on when the scene gives seat 0 a builder and the layout has a column); a kill is {at,
       kill} alone, kill naming a unit of the merged scene by its explicit id, at most once and at most GAME_KILLS
       kills, at <= end.
     - in the merged scene no feature's id (f<k + 1> when it has none) is a unit's explicit id, since a unit's corpse
       takes its id; and its features and the kills together, the game script's, every fork part's and every kept
       fork's, are at most FEATS_MAX, the features table a corpse is placed into (an over-approximation of the wrecks
       any chain of forks can make, which tagpu_wikisim_fork holds at run time too).
     - A LIVE ENTRY (`"live": true`, a game entry's alone, a boolean) plays a live skirmish with the rewind, never a
       replay; its viewer presses LIVE_CONTROLS, a replay entry's REPLAY_CONTROLS (checkViewer). Its demo may carry
       `forks`, a list of at most DEMO_FORKS parts {id, from, parent, inputs} and an optional `seat`: the game inputs
       the viewer's fork of that id goes on with, played as `seat` (0 when absent; THE SEAT RULE above holds it). id an
       integer 1..TIMELINES - 1 and no two parts of one id; from an integer 2..end (no fork starts at the original's
       first tick); parent an integer 0..id - 1 (a parent is made before its child: ids only grow); inputs a list of
       game inputs of any seat (the rules above, a unit killed at most once in the part and at most GAME_KILLS kills),
       each as tagpu_wikidata.c reads one (a build of a type of the entry, a reclaim true, a pick a kind, a move two
       numbers, a key one of KEYS or ACTIONS) and at a tick from..end. A demo of any other entry carries no `forks`.
     - A TAKEOVER ENTRY (`"takeover": true`, a game entry's alone, a boolean, never with `live` true) plays a replay
       with the replay HUD's Take over: the picker, the Forks bay and its lanes (replay.js, TAKE OVER). Its viewer
       presses REPLAY_CONTROLS, TAKEOVER_CONTROLS and LIVE_CONTROLS (a takeover turns the replay into a live game);
       its demos' game has the sidebar false, since the column is seat 0's and a takeover may take any seat. Its demo
       may carry `forks` as a live entry's, and `kept`: the forks the recording's file holds when the demo starts,
       at most FORK_LIMIT, each exactly {id, from, parent, seat, end, inputs} as it was played. id k + 1 at index k
       (a kept fork's row is its id); parent an integer 0..id - 1; from a tick of the parent's segment after the
       parent's first (the original's is 1, a kept parent's its from) and at most the parent's end (the original's
       is the game's), a refresh the recorder's writer flags walked from that first (refreshAt), since a fork starts
       only there (tagpu_replay.c takeover_point); seat one of the scene's (THE SEAT RULE); end an integer from..the
       game's end; inputs as a part's, at a tick from..its end. A demo of any other entry carries no `kept`. ---- */
  const SEATS = 4, SEAT_NAME = 32, SEAT_MIN = 200, SEAT_MAX = 10000, GAME_KILLS = 64, GAME_END_MAX = 5400;
  /* a demo's fork parts, at most (tagpu_wikidata.h WIKI_DEMO_FORKS); and the timelines a recording holds, the
     recorder's RECR_TIMELINES, which bounds a part's id: tagpu_wikidata.c reads it from tagpu_recrules.h itself,
     and simparity's refusal cases hold this to that */
  const DEMO_FORKS = 8, TIMELINES = 64;
  /* THE RECORDER'S RULES, the twins of tagpu/ddraw/inc/tagpu_recrules.h, which the recorder runs and the DLL's replay
     layer and loader include (simparity compares them case by case): the refresh interval a recording is written
     with (REC_REFRESH_TICKS) and the live forks a recording keeps (REC_FORK_LIMIT); the refresh grid (refresh_bin,
     reclossy_is_refresh). replay.js plans its forks with them, and checkGame holds a kept fork to them. */
  const REFRESH_TICKS = 300, FORK_LIMIT = 3;
  // the interval of the refresh grid that holds GameTime t: 1 to interval is the first
  const refreshBin = (t, interval) => Math.floor((t + interval - 1) / interval);
  /* whether a segment's iteration at GameTime `tick` is a refresh, `n` its ordinal in the segment from 1 and `prev` the
     GameTime of the segment's last refresh before it: the first two, then the first in each interval other than the last
     refresh's; `interval` 0 makes only the first two */
  const isRefresh = (n, tick, prev, interval) =>
    n <= 2 || (interval !== 0 && refreshBin(tick, interval) !== refreshBin(prev, interval));
  /* the last GameTime up to T that the writer flags a refresh in a segment whose first iteration is `first`, walked from
     it with the ordinal from 1; `first` itself when T is below first + 1. At most T - first + 1 integer steps
     (tagpu_wikidata.c tagpu_wikidata_refresh_at) */
  function refreshAt(first, T) {
    let R0 = first;
    for (let r = first, n = 1, prev = 0; r <= T; r++, n++) if (isRefresh(n, r, prev, REFRESH_TICKS)) { R0 = r; prev = r; }
    return R0;
  }
  // whether the entry is a game entry; anything but "game" in `frame` refuses it
  function isGame(entry) {
    if (!('frame' in entry)) return false;
    if (entry.frame !== 'game') throw new Error(`the entry's frame ${JSON.stringify(entry.frame)} is not "game"`);
    return true;
  }
  /* A GAME SCENE'S SEATS: 1..SEATS of {name, metal, energy}; a players entry's (`players`) also take `camera`, the
     view's centre at the game's first tick, two integers (checkGame holds them to the map), and `team`, an integer
     0..SEATS - 1, absent the seat's own index -- seats of one team are allied to each other */
  function checkSeats(at, seats, players = false) {
    if (!Array.isArray(seats) || seats.length < 1 || seats.length > SEATS) throw new Error(`${at}: seats is not a list of 1 to ${SEATS}`);
    const known = players ? ['name', 'metal', 'energy', 'camera', 'team'] : ['name', 'metal', 'energy'];
    seats.forEach((s, k) => {
      if (!obj(s) || !Object.keys(s).every(n => known.includes(n))) throw new Error(`${at}: seat ${k} is not {name, metal, energy}`);
      if ('team' in s && !(int(s.team) && s.team >= 0 && s.team < SEATS)) throw new Error(`${at}: seat ${k}'s team is not an integer 0..${SEATS - 1}`);
      if ('camera' in s && !(Array.isArray(s.camera) && s.camera.length === 2 && s.camera.every(int)))
        throw new Error(`${at}: seat ${k}'s camera is not two integers`);
      if (!fitsIn(s.name, SEAT_NAME) || !s.name.length || s.name.includes('{')) throw new Error(`${at}: seat ${k}'s name is not 1 to ${SEAT_NAME - 1} bytes without "{"`);
      for (const n of ['metal', 'energy'])
        if (!(int(s[n]) && s[n] >= SEAT_MIN && s[n] <= SEAT_MAX)) throw new Error(`${at}: seat ${k}'s ${n} is not an integer ${SEAT_MIN}..${SEAT_MAX}`);
    });
  }
  /* THE GAME FRAME'S 4:3 BOUND (tagpu_wikidata.h tagpu_wikidata_frame_fits): true when a game entry's map of w x h
     cells makes a frame the replay HUD fits, 3 Wg >= 4 Hg, 960 units (MEASURED at 1024 x 768: hud-in-game.md). The
     frame (frame.js size) is SIDE_W + CELL w wide and at most the map grown beside the tallest column (THE MAP, below)
     and both FRAME_BAR-row bars tall, so the bound holds beside every layout's column. checkGame refuses a game entry
     that fails it. */
  const FRAME_BAR = 32;
  const frameFits = (w, h) => 3 * (SIDE_W + CELL * w) >= 4 * (CELL * Math.max(h, 2 * Math.ceil(SIDE_ROWS / (2 * CELL))) + 2 * FRAME_BAR);
  /* A PLAYERS ENTRY'S SCREEN (tagpu_wikidata.c screen and screen_on_map), `screen` [sw, sh] cells: the game screen its
     recordings were made on, two even integers 2..MAP_MAX (whole 32-unit tiles, the ground's grid), within the first
     scene's map, whose rows are at least the viewport's beside the deepest column, 16 h >= max(16 sh, SIDE_ROWS - 64),
     so the eye's range is never empty. The frame is the screen's, so the 4:3 bound is taken on it (screenFits)
     instead of on the map. The map's sides are even too: the sight grid is (w/2) x (h/2) blocks, halved truncating
     (0x482F1B..0x482F45), so an odd side would leave its last 16-unit column or row with no block. */
  const MAP_MAX = 128;
  const screenFits = (sw, sh) => 3 * (SIDE_W + CELL * sw) >= 4 * Math.max(CELL * sh + 2 * FRAME_BAR, SIDE_ROWS);
  function checkScreen(entry, m) {
    const S = entry.screen;
    if (!(Array.isArray(S) && S.length === 2 && S.every(v => int(v) && v >= 2 && v <= MAP_MAX && v % 2 === 0)))
      throw new Error(`a players entry's screen is not two even integers 2..${MAP_MAX}`);
    if (S[0] > m.w || S[1] > m.h) throw new Error('a players entry\'s screen is larger than its map');
    if (CELL * m.h < Math.max(CELL * S[1], SIDE_ROWS - 2 * FRAME_BAR))
      throw new Error('a players entry\'s map has fewer rows than the viewport\'s beside the deepest column');
    if (!screenFits(S[0], S[1])) throw new Error(`a players entry's screen of ${S[0]} x ${S[1]} makes a frame narrower than 4:3`);
  }
  /* THE CAMERA'S RANGE: a view centre of the entry's screen inside the map of w x h cells, x in [8 sw, 16 w - 8 sw] and
     z in [8 sh, 16 h - 8 sh] (tagpu_wikidata.c cam_ok) */
  const camOk = (entry, m, p) => p[0] >= 8 * entry.screen[0] && p[0] <= CELL * m.w - 8 * entry.screen[0] &&
    p[1] >= 8 * entry.screen[1] && p[1] <= CELL * m.h - 8 * entry.screen[1];
  /* A SEAT'S CAMERA (tagpu_wikidata.h tagpu_wikidata_seat_cam, which states the rule): the centre [x, z] of seat k's
     view at game tick t of a players entry's demo, or null when the seat has none. Each glide is each axis
     a + trunc((b - a) (t - at) / over), in integers; the products stay below 2^53, so the doubles are exact. */
  function seatCam(entry, demo, k, t) {
    const z = sceneOf(entry, demo).seats[k];
    if (!z || !z.camera) return null;
    let a = [z.camera[0], z.camera[1]];
    for (const e of demo.game.inputs) {
      if (!('camera' in e) || (e.seat ?? 0) !== k) continue;
      if (e.at > t) break;
      const over = e.over ?? 0;
      if (t >= e.at + over) { a = [e.camera[0], e.camera[1]]; continue; }
      return [0, 1].map(i => a[i] + Math.trunc((e.camera[i] - a[i]) * (t - e.at) / over));
    }
    return a;
  }
  /* A PLAYERS ENTRY'S DEATHS (tagpu_wikidata.h WIKI_PLAYERS_DEATHS, which states the engine's cap): at most
     PLAYERS_DEATHS kills of one seat's units at ticks within any PLAYERS_DEATH_SPAN consecutive ticks */
  const PLAYERS_DEATHS = 20, PLAYERS_DEATH_SPAN = 62;
  function checkDeaths(at, S, nseat, inputs) {
    const owner = new Map();
    for (const u of S.units) if (typeof u.id === 'string' && u.id && !owner.has(u.id)) owner.set(u.id, u.owner ?? 0);
    for (let k = 0; k < nseat; k++) {
      const t = inputs.filter(e => 'kill' in e && owner.get(e.kill) === k).map(e => e.at).sort((a, b) => a - b);
      for (let i = 0; i + PLAYERS_DEATHS < t.length; i++)
        if (t[i + PLAYERS_DEATHS] <= t[i] + (PLAYERS_DEATH_SPAN - 1))
          throw new Error(`${at}: seat ${k} loses more than ${PLAYERS_DEATHS} units within ${PLAYERS_DEATH_SPAN} ticks from tick ${t[i]}`);
    }
  }
  /* THE VIEW RULE (tagpu_wikidata.c cameras_ok, which states why): for every seat with a camera, at every tick u from
     its first move (tick 0 at the earliest) to the game's end, the point of its last move at or before u, in the order
     the script is fed, lies in its view at u: |x - cx(u)| < 8 sw and |z - cz(u)| < 8 sh */
  function checkCameras(at, entry, demo) {
    const seats = sceneOf(entry, demo).seats, g = demo.game, [sw, sh] = entry.screen;
    seats.forEach((z, k) => {
      if (!z.camera) return;
      const mv = g.inputs.map((e, i) => [e, i]).filter(([e]) => Array.isArray(e.move) && (e.seat ?? 0) === k)
        .sort((a, b) => a[0].at - b[0].at || a[1] - b[1]).map(p => p[0]);
      if (!mv.length || !(mv[0].at <= g.end)) return;
      let m = -1;
      for (let u = Math.max(0, Math.ceil(mv[0].at)); u <= g.end; u++) {
        while (m + 1 < mv.length && mv[m + 1].at <= u) m++;
        const c = seatCam(entry, demo, k, u);
        if (!(Math.abs(mv[m].move[0] - c[0]) < 8 * sw && Math.abs(mv[m].move[1] - c[1]) < 8 * sh))
          throw new Error(`${at}: seat ${k}'s cursor at tick ${u} is outside its camera's view`);
      }
    });
  }
  /* THE LIVE DATA RULE (tagpu_wikidata.c live_orders): in a live entry a seat's release (sim.release) can come between
     any two ticks of a game script and of a fork's part, and it empties that seat's selection an order is given from
     and disarms its reclaim. So each `build` naming a type and each `reclaim` has, on its own tick and before it in
     the order the inputs are fed (by tick, then file order; within one input its move, then its press, then its
     release, as input() reads them), the click of its own seat that selects its builder or reclaimer: a `move`
     aiming that seat's cursor, then `select` pressed, then `select` released. Another seat's click selects nothing
     of this seat's, and a release lets go of one seat alone, so the rule holds seat by seat. The release runs only
     between two ticks, so it cannot fall between that click and the order; the move is on the tick too because a
     trip the release cuts off leaves the cursor wherever it was.
     Whether the click lands on a builder or a reclaimer is the world's, which simcheck's release sweep plays out.
     A live entry has no `pick`: the popup it clicks stands only while reclaim is armed, and with the sidebar's trips a
     reclaim arms only when its trip clicks RECLAIM, ticks after the reclaim's own, while checkTrips keeps every other
     input out of the reclaim's tick after it and out of its TRIP ticks; so a pick is always on a later tick than its
     reclaim, and a release between the two closes the popup the pick needs (pickTrip then stops the demo). Whether
     the sidebar is on is the layout's, which checkGame is not given, so a pick is refused whatever. `w` names the
     demo, or the demo and the fork part. */
  function checkLiveOrders(w, inputs) {
    inputs.forEach((e, i) => {
      if ('pick' in e) throw new Error(`${w}: the pick at tick ${e.at}: a live entry has no pick, since the popup it ` +
        'clicks opens on a later tick than its reclaim and a release between them closes it');
      const what = typeof e.build === 'string' && e.build ? 'build' : 'reclaim' in e ? 'reclaim' : null;
      if (!what) return;
      // step: 1 the cursor aimed, 2 then select pressed, 3 then released, over the inputs fed before the order
      let step = 0;
      for (let j = 0; j < i; j++) {
        const f = inputs[j];
        if (f.at !== e.at || (f.seat ?? 0) !== (e.seat ?? 0)) continue;
        if (f.move && step < 3) step = 1;
        if (f.down === 'select' && step === 1) step = 2;
        if (f.up === 'select' && step === 2) step = 3;
      }
      if (step < 3) throw new Error(`${w}: the ${what} at tick ${e.at} has no click selecting its ` +
        `${what === 'build' ? 'builder' : 'reclaimer'} on its own tick (a move, then select pressed and released), ` +
        `which a live entry's ${what} needs`);
    });
  }
  function checkGame(entry, demo) {
    const game = isGame(entry);
    if (game && 'pace' in entry) throw new Error('a game entry has no pace');
    if ('live' in entry && !(game && typeof entry.live === 'boolean')) throw new Error('live is a game entry\'s, true or false');
    if ('takeover' in entry && !(game && typeof entry.takeover === 'boolean')) throw new Error('takeover is a game entry\'s, true or false');
    if ('players' in entry && !(game && typeof entry.players === 'boolean')) throw new Error('players is a game entry\'s, true or false');
    if ([entry.live, entry.takeover, entry.players].filter(v => v === true).length > 1)
      throw new Error('an entry is a replay, a live game, a takeover\'s replay or a players\' replay, never two');
    const live = game && entry.live === true, tk = game && entry.takeover === true, pl = game && entry.players === true;
    if (!pl && 'screen' in entry) throw new Error('screen is a players entry\'s');
    if (pl && !('screen' in entry)) throw new Error('a players entry has a screen');
    for (const [t, d] of Object.entries(entry.units || {})) {
      for (const [n, lo, hi] of SIGHT) if (sightBad(d, n, lo, hi)) throw new Error(`unit ${t}: ${n} is not an integer ${lo}..${hi}`);
      if (pl && !sightFits(d)) throw new Error(`unit ${t}: a players entry's type has a sightDistance of at least ${SIGHT_MIN}`);
      for (const n of ECON) if (n in d && !(typeof d[n] === 'number' && d[n] >= 0 && d[n] < Infinity)) throw new Error(`unit ${t}: ${n} is not a finite number >= 0`);
      if ('energyUse' in d && !(typeof d.energyUse === 'number' && Math.abs(d.energyUse) < ECON_MAX)) throw new Error(`unit ${t}: energyUse is not a number of magnitude below ${ECON_MAX}`);
      if (('rival' in d || 'corpse' in d) && !game) throw new Error(`unit ${t}: rival and corpse are a game entry's`);
      if ('rival' in d && typeof d.rival !== 'boolean') throw new Error(`unit ${t}: rival is not a boolean`);
      if ('corpse' in d && !(typeof d.corpse === 'string' && obj(entry.features) && own(entry.features, d.corpse)))
        throw new Error(`unit ${t}: corpse names no feature of the entry`);
    }
    for (const [n, s] of Object.entries(entry.scenes)) {
      if (!game) { if ('seats' in s) throw new Error(`the scene ${n}: seats are a game entry's`); continue; }
      checkSeats(`the scene ${n}`, s.seats, pl);
      for (const u of s.units || []) if (!(u.owner == null || (int(u.owner) && u.owner >= 0 && u.owner < s.seats.length)))
        throw new Error(`the scene ${n}: the owner ${u.owner} is no seat of it`);
    }
    // every scene has the first's size (sceneOf), so the first's map decides
    if (game && !pl) {
      const m = entry.scenes[Object.keys(entry.scenes)[0]].map;
      if (!frameFits(m.w, m.h)) throw new Error(`a game entry's map of ${m.w} x ${m.h} makes a frame narrower than 4:3`);
    }
    if (pl) {
      for (const [n, s] of Object.entries(entry.scenes)) {
        if (!(obj(s.map) && int(s.map.w) && int(s.map.h) && s.map.w % 2 === 0 && s.map.h % 2 === 0))
          throw new Error(`the scene ${n}: a players entry's map is not of even cells`);
        s.seats.forEach((z, k) => {
          if (z.camera && !camOk(entry, s.map, z.camera)) throw new Error(`the scene ${n}: seat ${k}'s camera puts its view outside the map`);
        });
      }
      checkScreen(entry, entry.scenes[Object.keys(entry.scenes)[0]].map);
    }
    if (!demo) return game;
    const at = `demo ${demo.id}`;
    if (demo.scene && 'seats' in demo.scene) throw new Error(`${at}: a demo's scene has no seats`);
    if ('forks' in demo && !live && !tk) throw new Error(`${at}: forks are a live or a takeover entry's demo's`);
    if ('kept' in demo && !tk) throw new Error(`${at}: kept is a takeover entry's demo's`);
    if (!game) { if ('game' in demo) throw new Error(`${at}: game is a game entry's`); return game; }
    const g = demo.game, seats = sceneOf(entry, demo).seats;
    if (!obj(g)) throw new Error(`${at} has no game`);
    if ('sidebar' in demo) throw new Error(`${at}: a game entry's demo takes its sidebar in game`);
    if (!Object.keys(g).every(k => ['end', 'inputs', 'sidebar'].includes(k))) throw new Error(`${at}: game holds a key it does not know`);
    if (!(int(g.end) && g.end >= 1 && g.end <= GAME_END_MAX)) throw new Error(`${at}: the game's end is not an integer 1..${GAME_END_MAX}`);
    if ('sidebar' in g && typeof g.sidebar !== 'boolean') throw new Error(`${at}: the game's sidebar is not a boolean`);
    /* a takeover may take any seat, and the column is seat 0's: a takeover entry's game has none, the sidebar false (an
       absent sidebar may be on, THE SEAT RULE's over-approximation) */
    if (tk && g.sidebar !== false) throw new Error(`${at}: a takeover entry's game has the sidebar false, since the column is seat 0's`);
    // every seat of a players entry acts through its cursor (the plan's D16): no column
    if (pl && g.sidebar !== false) throw new Error(`${at}: a players entry's game has the sidebar false, since every seat acts`);
    if (!Array.isArray(g.inputs)) throw new Error(`${at}: the game's inputs are not a list`);
    for (const u of (demo.scene && demo.scene.units) || []) if (!(u.owner == null || (int(u.owner) && u.owner >= 0 && u.owner < seats.length)))
      throw new Error(`${at}: the owner ${u.owner} is no seat of its scene`);
    /* THE SEAT RULE: a game input's seat is one of the scene's, and with a sidebar that may be on it is 0, since the
       column is seat 0's (the sim refuses a column beside another seat's input as well, make) */
    const seatOk = (w, e) => {
      if (!('seat' in e)) return;
      if (!(int(e.seat) && e.seat >= 0 && e.seat < seats.length)) throw new Error(`${w}: seat ${e.seat} is no seat of its scene`);
      if (e.seat !== 0 && g.sidebar !== false)
        throw new Error(`${w}: seat ${e.seat}: the column is seat 0's, so a game whose sidebar is not false gives no other seat an input`);
    };
    const S = merge(entry, demo), ids = new Set(S.units.filter(u => typeof u.id === 'string' && u.id).map(u => u.id));
    S.features.forEach((f, k) => { if (ids.has(f.id ?? `f${k + 1}`)) throw new Error(`${at}: the feature ${f.id ?? `f${k + 1}`} has a unit's id`); });
    const killed = new Set(), camPrev = [];
    for (const e of g.inputs) {
      const w = `${at}: the game input at tick ${obj(e) ? e.at : e}`;
      if (!obj(e) || typeof e.at !== 'number') throw new Error(`${w} has no tick`);
      if ('caption' in e || 'until' in e) throw new Error(`${w} is a caption or a wait, which a game script has not`);
      seatOk(w, e);
      if (!('kill' in e) && 'camera' in e) {
        /* A CAMERA INPUT, a players entry's (tagpu_wikidata.c game): {at, seat, camera, over}, of a seat with a camera,
           to a point of its range, at an integer tick up to the end, never inside the seat's previous glide */
        if (!pl) throw new Error(`${w}: a camera input is a players entry's`);
        if (!Object.keys(e).every(k => ['at', 'seat', 'camera', 'over'].includes(k))) throw new Error(`${w}: a camera input carries another key`);
        if (!(int(e.at) && e.at >= 0 && e.at <= GAME_END_MAX)) throw new Error(`${w}: a camera input's tick is not an integer 0..${GAME_END_MAX}`);
        if (!(Array.isArray(e.camera) && e.camera.length === 2 && e.camera.every(int))) throw new Error(`${w}: a camera is not two integers`);
        if ('over' in e && !(int(e.over) && e.over >= 0 && e.over <= GAME_END_MAX)) throw new Error(`${w}: over is not an integer 0..${GAME_END_MAX}`);
        const k = e.seat ?? 0, prev = camPrev[k];
        if (!seats[k].camera) throw new Error(`${w}: the camera input's seat has no camera`);
        if (!camOk(entry, sceneOf(entry, demo).map, e.camera)) throw new Error(`${w}: the camera input puts its view outside the map`);
        if (!(e.at <= g.end)) throw new Error(`${w}: the camera input is after the game's end`);
        if (prev && e.at < prev.at + (prev.over ?? 0)) throw new Error(`${w}: the camera input is before the end of its seat's previous one`);
        camPrev[k] = e;
        continue;
      }
      if (!('kill' in e)) continue;
      if (!Object.keys(e).every(k => k === 'at' || k === 'kill' || k === 'by')) throw new Error(`${w}: a kill carries another key`);
      if ('by' in e && !(int(e.by) && e.by >= 0 && e.by < seats.length)) throw new Error(`${w}: the kill's by is no seat of its scene`);
      if (typeof e.kill !== 'string' || !ids.has(e.kill)) throw new Error(`${w}: the kill names no unit of the scene`);
      if (killed.has(e.kill)) throw new Error(`${w}: ${e.kill} is killed twice`);
      if (killed.size === GAME_KILLS) throw new Error(`${w}: more than ${GAME_KILLS} kills`);
      if (!(e.at <= g.end)) throw new Error(`${w}: the kill is after the game's end`);
      killed.add(e.kill);
    }
    if (live) checkLiveOrders(at, g.inputs);
    /* a fork's game inputs, of a part or a kept fork, at ticks from..last: each as tagpu_wikidata.c reads one, which a
       part's never reaches a simulation to be checked by; its kills, each of a unit of the scene once in the fork and
       at most GAME_KILLS, counted */
    const forkInputs = (w, inputs, from, last) => {
      if (!Array.isArray(inputs)) throw new Error(`${w}: its inputs are not a list`);
      const kills = new Set();
      for (const e of inputs) {
        const v = `${w}: the input at tick ${obj(e) ? e.at : e}`;
        if (!obj(e) || typeof e.at !== 'number') throw new Error(`${v} has no tick`);
        if ('caption' in e || 'until' in e) throw new Error(`${v} is a caption or a wait, which a game script has not`);
        seatOk(v, e);
        if (!(e.at >= from && e.at <= last)) throw new Error(`${v} is not at a tick ${from}..${last}`);
        if ('kill' in e) {
          if (!Object.keys(e).every(k => k === 'at' || k === 'kill' || k === 'by')) throw new Error(`${v}: a kill carries another key`);
          if ('by' in e && !(int(e.by) && e.by >= 0 && e.by < seats.length)) throw new Error(`${v}: the kill's by is no seat of its scene`);
          if (typeof e.kill !== 'string' || !ids.has(e.kill)) throw new Error(`${v}: the kill names no unit of the scene`);
          if (kills.has(e.kill)) throw new Error(`${v}: ${e.kill} is killed twice`);
          if (kills.size === GAME_KILLS) throw new Error(`${v}: more than ${GAME_KILLS} kills`);
          kills.add(e.kill);
          continue;
        }
        // a fork is a live or a takeover entry's, never a players entry's, which alone has camera inputs
        if ('camera' in e) throw new Error(`${v}: a camera input is a players entry's`);
        const key = k => !(typeof k === 'string' && k !== '') || KEYS.includes(k) || ACTIONS.includes(k);
        if (typeof e.build === 'string' && e.build && !own(entry.units, e.build)) throw new Error(`${v}: no unit type ${e.build}`);
        if ('reclaim' in e && e.reclaim !== true) throw new Error(`${v}: a reclaim is not true`);
        if ('pick' in e && !KINDS.includes(e.pick)) throw new Error(`${v}: a pick names no kind`);
        if ('move' in e && e.move !== null && !(Array.isArray(e.move) && typeof e.move[0] === 'number' && typeof e.move[1] === 'number'))
          throw new Error(`${v}: a move is not two numbers`);
        if (!key(e.down) || !key(e.up)) throw new Error(`${v}: no key ${key(e.down) ? e.up : e.down}`);
      }
      return kills.size;
    };
    let partKills = 0;
    if ('forks' in demo) {
      const F = demo.forks, partIds = [];
      if (!Array.isArray(F) || F.length > DEMO_FORKS) throw new Error(`${at}: forks is not a list of at most ${DEMO_FORKS} parts`);
      for (const p of F) {
        const w = `${at}: the fork part ${obj(p) ? p.id : p}`;
        if (!obj(p) || !['id', 'from', 'parent', 'inputs'].every(k => own(p, k)) || !Object.keys(p).every(k => ['id', 'from', 'parent', 'inputs', 'seat'].includes(k)))
          throw new Error(`${w} is not {id, from, parent, inputs} with an optional seat`);
        if (!(int(p.id) && p.id >= 1 && p.id < TIMELINES) || partIds.includes(p.id)) throw new Error(`${w}: the id is not a new integer 1..${TIMELINES - 1}`);
        partIds.push(p.id);
        if (!(int(p.from) && p.from >= 2 && p.from <= g.end)) throw new Error(`${w}: from is not an integer 2..${g.end}`);
        if (!(int(p.parent) && p.parent >= 0 && p.parent < p.id)) throw new Error(`${w}: the parent is not an integer 0..${p.id - 1}`);
        if ('seat' in p) seatOk(`${w}: the fork's`, p);
        partKills += forkInputs(w, p.inputs, p.from, g.end);
        checkLiveOrders(w, p.inputs);
      }
    }
    // the forks the file holds: a parent's first and end by id, the original's 1 and the game's end
    let keptKills = 0;
    if ('kept' in demo) {
      const K = demo.kept, span = [{ R: 1, end: g.end }];
      if (!Array.isArray(K) || K.length > FORK_LIMIT) throw new Error(`${at}: kept is not a list of at most ${FORK_LIMIT} forks`);
      K.forEach((f, k) => {
        const w = `${at}: the kept fork ${k + 1}`;
        if (!obj(f) || Object.keys(f).length !== 6 || !['id', 'from', 'parent', 'seat', 'end', 'inputs'].every(n => own(f, n)))
          throw new Error(`${w} is not {id, from, parent, seat, end, inputs}`);
        if (f.id !== k + 1) throw new Error(`${w}: its id is not ${k + 1}, its place in the list`);
        if (!(int(f.parent) && f.parent >= 0 && f.parent < f.id)) throw new Error(`${w}: the parent is not an integer 0..${f.id - 1}`);
        const P = span[f.parent];
        if (!(int(f.from) && f.from > P.R && f.from <= P.end && refreshAt(P.R, f.from) === f.from))
          throw new Error(`${w}: from is not a refresh of its parent's segment after its first ${P.R}, up to its end ${P.end}`);
        seatOk(`${w}: the fork's`, f);
        if (!(int(f.end) && f.end >= f.from && f.end <= g.end)) throw new Error(`${w}: the end is not an integer ${f.from}..${g.end}`);
        keptKills += forkInputs(w, f.inputs, f.from, f.end);
        span.push({ R: f.from, end: f.end });
      });
    }
    if (S.features.length + killed.size + partKills + keptKills > FEATS_MAX) throw new Error(`${at}: the scene's features and the kills pass ${FEATS_MAX}`);
    if (pl) { checkDeaths(at, S, seats.length, g.inputs); checkCameras(at, entry, demo); }
    checkViewer(at, demo, g.end, live ? 'live' : tk ? 'takeover' : pl ? 'players' : 'replay', seats.length);
    return game;
  }
  /* THE VIEWER'S SCRIPT of a game entry's demo, its `inputs` on demo time (decision 4): the data rule, refused here as
     tagpu_wikidata.c refuses it at load; replay.js plays it, and the simulation never reads it. `inputs` that is not a
     list reads as empty, as the loader reads one. Each input has a numeric `at` and is exactly one of
     - a caption, {at, caption};
     - the viewer's one wait, {at, until: "seeked", max}: max an integer 1..UNTIL_MAX, and no other input on its tick;
     - a press of the HUD's, {at, hud}: hud one of a live entry's LIVE_CONTROLS, a takeover entry's REPLAY_CONTROLS,
       TAKEOVER_CONTROLS and LIVE_CONTROLS, else of REPLAY_CONTROLS (leave:yes is the player's alone, never a
       script's: the presentation's); for `track` a `to`, an integer 0..the game's end, with `drag` (the same) and
       `over` (an integer 1..UNTIL_MAX) together or neither; for `takeas` a `seat`, the picker's row of that seat, an
       integer 0..the scene's seats - 1; for `lane`, `export` and `delete` a `fork`, the id of the fork whose lane it
       presses, an integer 1..TIMELINES - 1; a players entry's REPLAY_CONTROLS and PLAYERS_CONTROLS, `jump`, `sight`
       and `follow` with a `seat` as `takeas` has, and `chip` with a `chip`, one of CHIPS. The Players set is a
       players entry's alone.
     A press's trip runs from its tick to its release, at + HUD_UP, or for a drag at + HUD_DOWN + over; no other press
     and no wait stands in it, the release's tick included, in the order the inputs are fed (by tick, then file
     order). A demo's `end`, when given, is a number. */
  /* HUD_CONTROLS, every control a script may name, in WIKI_HUD_* order: the replay HUD's eight, then the live HUD's
     own three (rew, fwd and track are both HUDs', and mean the live HUD's in a live game), then the replay HUD's Take
     over set (tagpu_hudinput.c pick_act and lane_act): Take over, a picker row, the Forks bay, a lane, Back to main
     recording, a lane's Save as a replay and its Delete; then the Players set (tagpu_hudinput.c act): a row's
     Jump, Sight and Follow, the four overlay switches, the stats window's button, its close and a chip */
  const HUD_CONTROLS = ['play', 'speed', 'rew', 'fwd', 'track', 'bookmark', 'leave', 'leave:no', 'rewind', 'live_play', 'live_now',
    'takeover', 'takeas', 'lanes', 'lane', 'main', 'export', 'delete', 'jump', 'sight', 'follow', 'cursors', 'clicks',
    'selections', 'chat', 'stats', 'close', 'chip'];
  const REPLAY_CONTROLS = HUD_CONTROLS.slice(0, 8), LIVE_CONTROLS = ['rewind', 'rew', 'fwd', 'track', 'live_play', 'live_now'];
  const TAKEOVER_CONTROLS = HUD_CONTROLS.slice(11, 18), PLAYERS_CONTROLS = HUD_CONTROLS.slice(18);
  /* the stats window's chips a `chip` press names, in tagpu_wikidata.h's WIKI_CHIP_* order: the two splits, then
     art/hud/layout.json's window.measures keys in that file's order (tools/wiki/castcheck.mjs holds the list to it) */
  const CHIPS = ['player', 'team', 'metal', 'energy', 'units', 'army', 'kills', 'losses', 'excess'];
  // the controls each kind of game entry's viewer presses (checkViewer's `set`)
  const PRESS_SETS = { replay: REPLAY_CONTROLS, live: LIVE_CONTROLS,
    takeover: [...new Set([...REPLAY_CONTROLS, ...TAKEOVER_CONTROLS, ...LIVE_CONTROLS])],
    players: [...REPLAY_CONTROLS, ...PLAYERS_CONTROLS] };
  // a HUD press's trip is the popup pick's without its glide back (THE PICK TRIP): pressed at HUD_DOWN, released at HUD_UP
  const HUD_DOWN = PICK_DOWN, HUD_UP = PICK_UP;
  function checkViewer(at, demo, end, set, nseat) {
    if ('end' in demo && demo.end !== null && typeof demo.end !== 'number') throw new Error(`${at}: the end is not a number`);
    const list = Array.isArray(demo.inputs) ? demo.inputs : [];
    const keys = (e, ks) => Object.keys(e).every(k => k === 'at' || ks.includes(k));
    const tick = (e, k) => int(e[k]) && e[k] >= 0 && e[k] <= end;
    for (const e of list) {
      const w = `${at}: the viewer's input at tick ${obj(e) ? e.at : e}`;
      if (!obj(e) || typeof e.at !== 'number') throw new Error(`${w} has no tick`);
      if ('hud' in e) {
        if (e.hud === 'leave:yes') throw new Error(`${w}: leave:yes is the player's, never a demo's`);
        if (!HUD_CONTROLS.includes(e.hud)) throw new Error(`${w}: ${JSON.stringify(e.hud)} is no control a demo presses`);
        if (PLAYERS_CONTROLS.includes(e.hud) && set !== 'players') throw new Error(`${w}: ${e.hud} is a players' replay's control`);
        if (!PRESS_SETS[set].includes(e.hud)) throw new Error(`${w}: ${e.hud} is no control a ${set} demo presses`);
        const value = ['takeas', 'jump', 'sight', 'follow'].includes(e.hud) ? 'seat' : ['lane', 'export', 'delete'].includes(e.hud) ? 'fork'
          : e.hud === 'chip' ? 'chip' : null;
        if (e.hud === 'track' ? !keys(e, ['hud', 'to', 'drag', 'over']) : !keys(e, value ? ['hud', value] : ['hud']))
          throw new Error(`${w}: the press carries a key it does not know`);
        if (value === 'seat' && !(int(e.seat) && e.seat >= 0 && e.seat < nseat)) throw new Error(`${w}: the row's seat is not an integer 0..${nseat - 1}`);
        if (value === 'fork' && !(int(e.fork) && e.fork >= 1 && e.fork < TIMELINES)) throw new Error(`${w}: the fork is not an integer 1..${TIMELINES - 1}`);
        if (value === 'chip' && !CHIPS.includes(e.chip)) throw new Error(`${w}: the chip is not one of ${CHIPS.join(', ')}`);
        if (e.hud !== 'track') continue;
        if (!tick(e, 'to')) throw new Error(`${w}: the track's to is not an integer 0..${end}`);
        if (('drag' in e) !== ('over' in e)) throw new Error(`${w}: drag and over come together`);
        if ('drag' in e && !tick(e, 'drag')) throw new Error(`${w}: the drag is not an integer 0..${end}`);
        if ('over' in e && !(int(e.over) && e.over >= 1 && e.over <= UNTIL_MAX)) throw new Error(`${w}: over is not an integer 1..${UNTIL_MAX}`);
      } else if ('until' in e) {
        if (e.until !== 'seeked') throw new Error(`${w}: the viewer's wait is "seeked"`);
        if (!(int(e.max) && e.max >= 1 && e.max <= UNTIL_MAX)) throw new Error(`${w} has no max of 1 to ${UNTIL_MAX} ticks`);
        if (!keys(e, ['until', 'max'])) throw new Error(`${w}: the wait carries another input`);
        if (list.some(f => f !== e && obj(f) && f.at === e.at)) throw new Error(`${w}: the wait shares its tick with another input`);
      } else if (!('caption' in e) || !keys(e, ['caption'])) throw new Error(`${w} is no caption, wait or HUD press`);
    }
    let release = -Infinity;
    for (const e of list.map((e, i) => [e, i]).sort((a, b) => a[0].at - b[0].at || a[1] - b[1]).map(p => p[0])) {
      if (!('hud' in e) && !('until' in e)) continue;
      if (e.at <= release) throw new Error(`${at}: the ${'hud' in e ? 'press' : 'wait'} at tick ${e.at} stands in another press's trip`);
      if ('hud' in e) release = e.at + ('drag' in e ? HUD_DOWN + e.over : HUD_UP);
    }
  }
  /* a game entry's demo as the game simulation plays it: its game script, each input keeping its `seat` (the input
     state it acts on, 0 when absent), and the game's end and sidebar */
  const gameDemo = d => ({ id: d.id, on: d.on, scene: d.scene, end: d.game.end, ...('sidebar' in d.game ? { sidebar: d.game.sidebar } : {}),
    inputs: d.game.inputs });
  /* THE INPUT SEATS, NIN (ingame-wiki-replay-plan.md decision 16): a game simulation of a takeover entry holds an input
     state for every seat of its scene, since Try it may take over any of them; any other game for every seat its game
     script or a fork part names, as an input's seat or as the seat the fork is played as, 1 + the highest (a kill
     names none, and a camera input acts on no input state), at least 1; a page simulation holds one. checkGame bounds every seat by the scene's, so NIN is at
     most SEATS, and the sim refuses an input of a seat at NIN or above at the door (sim.input, sim.fork).
     tagpu_wikisim.c input_seats. */
  const inputSeats = (entry, d) => entry.takeover === true ? sceneOf(entry, d).seats.length
    : [...d.game.inputs, ...(d.forks || []).flatMap(p => [...p.inputs, { seat: p.seat }])]
      .reduce((n, e) => 'kill' in e || 'camera' in e ? n : Math.max(n, (e.seat ?? 0) + 1), 1);

  /* ---- actions (decision 18): a demo's press names what it does, and the Interface Type names the button ----
     select is the left button in both types, cancel the right one in both, order the right one at type 1 and the left
     one at type 0. lmb and rmb stay as written. */
  const ACTIONS = ['select', 'order', 'cancel'];
  const button = (a, iface) => a === 'select' ? 'lmb' : a === 'cancel' ? 'rmb' : a === 'order' ? (iface ? 'rmb' : 'lmb') : a;
  /* THE DATA RULE OF A RESOLVED SCRIPT, refused here as tagpu_wikisim_init refuses it: once the actions are buttons,
     a press of a mouse button its seat already holds, or a release of one its seat does not hold, refuses the demo
     with the input's tick and seat. Each seat has its own hand, held from nothing or from `from[seat]`, the buttons
     the seat holds when the script takes over (a fork's carried seats). An authored overlap -- the left button held
     for a box drag while an `order` click lands at type 0 -- therefore fails simcheck, which plays every demo at both
     types, and never reaches a player. The keys are not held to it: a key's second down is an auto-repeat
     (stepSpacing). An input already resolved comes back as it was. */
  function resolve(demo, script, iface, from = []) {
    const hands = [];
    return script.map(e => {
      const r = ACTIONS.includes(e.down) || ACTIONS.includes(e.up) ? { ...e } : e;
      if (r !== e && 'down' in r) r.down = button(r.down, iface);
      if (r !== e && 'up' in r) r.up = button(r.up, iface);
      const seat = e.seat ?? 0, held = hands[seat] || (hands[seat] = new Set(from[seat] || []));
      for (const [k, down] of [[r.down, true], [r.up, false]]) {
        if (k !== 'lmb' && k !== 'rmb') continue;
        if (down === held.has(k))
          throw new Error(`demo ${demo.id}: at tick ${e.at} the ${down ? 'press' : 'release'} of ${k}${seat ? ` by seat ${seat}` : ''} ` +
            `${down ? 'while it is held' : 'while it is up'}`);
        if (down) held.add(k); else held.delete(k);
      }
      return r;
    });
  }

  /* `scripted`: false for Try it yourself, true for a demo, 'game' for a game entry's game script (WikiSim.game), whose
     economy runs in `econMode` (THE ECONOMY, below). A game entry plays only as a game, and a game only from a game
     entry, so no page demo can reach a branch of the game's. */
  function make(entryIn, demoGiven, scripted, layout, ctxIn, econMode = 'refuse') {
    const ctx = context(layout, ctxIn), IFACE = ctx.iface, GAME = scripted === 'game';
    sceneOf(entryIn, demoGiven);   // the scenes are checked before the binding reads them
    if (checkGame(entryIn, demoGiven) !== GAME)
      throw new Error(GAME ? 'the entry is not a game entry: its demos play as demos' : 'a game entry\'s demos play as games');
    if (GAME && !demoGiven) throw new Error('a game plays a demo\'s game script');
    if (GAME && econMode !== 'refuse' && econMode !== 'stall') throw new Error(`the economy's mode ${econMode} is not refuse or stall`);
    const demoIn = GAME ? gameDemo(demoGiven) : demoGiven, NIN = GAME ? inputSeats(entryIn, demoGiven) : 1;
    // the bound entry and demo are this sim's own copies: they live as long as it does, and nothing else holds them
    const SIGHTT = layout && layout.sight ? layout.sight : null;
    const B = ctx.casting ? binder(entryIn, ctx.casting, ctx.rival, GAME, SIGHTT) : null;
    const entry = B ? B.entry : entryIn, demo = B ? B.demo(demoIn) : demoIn;
    /* THE SIGHT TABLE a players game stamps with: a type of the bound entry whose radius index the layout's table does
       not store refuses the game -- an entry's own type, since the binder left a cast of one unbound */
    if (GAME && entry.players === true && SIGHTT)
      for (const [t, d] of Object.entries(entry.units))
        if (!sightFits(d, SIGHTT)) throw new Error(`unit ${t}: a players entry's type of sightDistance ${d.sightDistance} ` +
          `reaches sight table ${sightIndex(SIGHTT, d.sightDistance)}, which the layout does not store`);
    const S = merge(entry, demo), D = entry.units, defIndex = Object.keys(D);
    if (!KINDS.includes(S.kind)) throw new Error(`the scene's reclaim kind ${JSON.stringify(S.kind)} is not one of ${KINDS}`);
    const ON = !!layout && (() => {
      const builder = S.units.some(s => (s.owner ?? 0) === 0 && own(D, s.type) && D[s.type].maxVelocity > 0 && D[s.type].workerTime > 0);
      return scripted && demo && typeof demo.sidebar === 'boolean' ? demo.sidebar : builder;
    })();
    // THE COLUMN IS SEAT 0'S (the acting seat, below): a game with the sidebar on gives no other seat an input
    if (ON && NIN > 1) throw new Error(`demo ${demo.id}: the column is seat 0's, so a game with the sidebar gives no other seat an input`);
    /* THE MAP: the scene's, grown downward with the sidebar on by whole 32-unit tiles until it is at least as tall as
       the column, so no band lies under it and every row of the column has the map beside it. The column is at most
       SIDE_ROWS rows (checkLayout), so H is at most SIDE_ROWS / CELL, the rows every stage description's ground holds
       (tools/wiki/stagemap.py GROUND_ROWS). The scene's content is checked against the scene's own rows (SH), so it
       is the same scene in every context; the rules of play read the map. */
    const W = S.map.w, SH = S.map.h, H = ON ? Math.max(SH, 2 * Math.ceil(layout.height / (2 * CELL))) : SH;
    const SM = S.map.surfaceMetal || 0, PACE = (entry.pace && entry.pace.build) || 1;
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
    /* FBI canreclamate, def+0x245 bit 0x400 (the FBI reader 0x42BF40 at 0x42C9EF..0x42CA1A): the resolver's first test
       for a reclaim (0x43F4FD) and what makes RECLAIM live (0x41B509..0x41B512, below); an absent key reads 0, as
       canstop's does [INFERRED: the reader's default] */
    const canReclaim = t => (def(t).canreclamate & 1) === 1;
    for (const t of defIndex) checkG(`unit ${t}`, D[t], D[t].footprint);
    // a resource building: a mex extracts metal, a geothermal plant needs a vent under a G cell of its yard
    const kind = t => def(t).extracts > 0 ? 'mex' : def(t).vent ? 'geo' : null;
    // footprint cell (dx, dz) of type t is a G cell: one `g` lists, or any cell of a `vent` type with no `g`
    const gCell = (t, dx, dz) => { const d = def(t); return d.g ? d.g.some(c => c[0] === dx && c[1] === dz) : !!d.vent; };
    const strength = t => kind(t) === 'mex' ? def(t).extracts : def(t).energyMake || 0;
    const canBuild = (u, t) => (def(u.type).builds || []).includes(t);
    const defined = t => typeof t === 'string' && own(D, t);

    /* ---- THE FEATURES (research/notes/ingame-wiki-reclaim-plan.md): the entry's `features` table names each by its TDF
       section, and a scene places one at its anchor cell, the footprint's top-left, as the TNT and LoadFeature 0x4224B0
       place a map's. A blocking one stands in the way as a structure does; a reclaimable one is a reclaim's target, and
       goes when its reclaim pays out (FeatureDie at 0x423965). The stage draws them from its description; the view
       names the ones still standing. ---- */
    if ('features' in entry && !obj(entry.features)) throw new Error('the entry\'s features is not an object');
    const FD = entry.features || {};
    // FEAT_DEFS: tagpu_wikidata.c's table of an entry's features
    if (Object.keys(FD).length > FEAT_DEFS) throw new Error(`the entry has more than ${FEAT_DEFS} features`);
    for (const [n, d] of Object.entries(FD)) checkFeature(n, d);
    if (S.features.length > FEATS_MAX) throw new Error(`the scene has more than ${FEATS_MAX} features`);
    // gone: the anchor cells of the features reclaimed so far, in order, which the game look's image no longer draws
    const feats = [], gone = [];
    S.features.forEach((f, k) => {
      const at = `feature ${k} of the scene`;
      if (!obj(f) || typeof f.name !== 'string' || !own(FD, f.name)) throw new Error(`${at} names no feature of the entry`);
      if (!Array.isArray(f.at) || f.at.length !== 2 || !f.at.every(int)) throw new Error(`${at}: at is not an anchor cell`);
      const d = FD[f.name], [fx, fz] = d.footprint, [ox, oz] = f.at, id = f.id ?? `f${k + 1}`;
      if (typeof id !== 'string' || !fitsIn(id, ID_MAX) || feats.some(g => g.id === id)) throw new Error(`${at}: the id is not a new name`);
      if (ox < 0 || oz < 0 || ox + fx > W || oz + fz > SH) throw new Error(`${at} is not on the map`);
      const g = { id, name: f.name, ox, oz, fx, fz, x: (ox + fx / 2) * CELL, z: (oz + fz / 2) * CELL, metal: d.metal || 0,
        energy: d.energy || 0, height: d.height || 0, blocking: !!d.blocking, reclaimable: !!d.reclaimable, wreck: 'object' in d };
      if (feats.some(e => e.ox < g.ox + g.fx && g.ox < e.ox + e.fx && e.oz < g.oz + g.fz && g.oz < e.oz + e.fz))
        throw new Error(`${at} overlaps another feature`);
      feats.push(g);
    });
    const featRect = f => [f.ox * CELL, f.oz * CELL, (f.ox + f.fx) * CELL, (f.oz + f.fz) * CELL];
    const featById = id => feats.find(f => f.id === id) || null;
    // the feature whose footprint holds the cell under (x, z), as 0x4815A0 finds one at a position through its anchor
    const featAt = (x, z) => {
      const cx = Math.floor(x / CELL), cz = Math.floor(z / CELL);
      return feats.find(f => cx >= f.ox && cx < f.ox + f.fx && cz >= f.oz && cz < f.oz + f.fz) || null;
    };

    /* ---- the sidebar: on or off for the sim's whole life ----
       A demo shows it when its `sidebar` says so, by default when the scene gives the player a mobile builder; Try it
       shows it whenever the scene does, whatever the demo said. Off, nothing below the map rules runs: no column,
       the cursor unclamped, and `build` arms at once. */
    if (scripted && demo && 'sidebar' in demo && typeof demo.sidebar !== 'boolean')
      throw new Error(`demo ${demo.id}: sidebar is not a boolean`);
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
      noticeActive(u);
    }

    /* ---- THE SOUND (tagpu_wikisim.h), tagpu_wikisim.c's sound functions line for line: each type's category and
       counts from ctx.snd, the time `now` (the tick being stepped, or between ticks the tick the sim stands at), the
       kinds' next times, the notice queue, the last reply's time, and the events, a ring at the count made modulo
       SND_LOG. Without a source nothing of it changes. ---- */
    const SNDT = ctx.snd ? new Map(defIndex.map(t => {
      const got = ctx.snd.type(t);
      return [t, got ? { cat: got.cat, count: Array.from({ length: SND_KINDS }, (_, k) => Math.max(0, got.count[k] | 0)) }
        : { cat: -1, count: new Array(SND_KINDS).fill(0) }];
    })) : null;
    // a demo is a scene of a game under way: its last reply is a gap past (tagpu_wikisim.c init)
    let now = 0, sndLast = -SND_GAP, sndTotal = 0;
    const sndNext = new Array(SND_KINDS).fill(0), sndQ = [], sndRing = new Array(SND_LOG).fill(null);
    const sceneOrders = new Set();   // the orders the scene gave, which say no ok
    let sceneQuiet = true;           // while the scene is laid out its units say nothing (tagpu_wikisim.c snd_quiet)
    // THE WIKI'S GENERATOR for a reply's variant: the unit's id (FNV-1a 32 over its UTF-8 bytes), the kind and the tick
    const sndDraw = (id, kind, tick) => {
      let h = 0x811c9dc5;
      for (const b of new TextEncoder().encode(id)) h = Math.imul(h ^ b, 0x01000193) >>> 0;
      return mix32((mix32((h + Math.imul(kind, 0x9e3779b9)) >>> 0) ^ tick) >>> 0) & 0x7fff;
    };
    function soundEvent(kind, name, cat, variant) {
      sndRing[sndTotal % SND_LOG] = [now, kind, name, cat, variant];
      if (sndTotal < INT_MAX - SND_LOG) sndTotal++;
    }
    /* the viewed seat, whose units' notices play: 0 until a fork takes over another seat, as the game's takeover makes
       the taken seat the viewed one (tagpu_replay.c tk_seats, main+0x2A43) */
    let viewed = 0;
    // 0x47F1A0's play by name: at once, no queue, no cooldown
    function soundNamed(name) { if (SNDT) soundEvent(0, SNDNAME[name], -1, 0); }
    // 0x47F780 then 0x47FAD0: the viewed player's unit, the kind's cooldown, one of its kind queued, a full queue's last
    // dropped, and in before the first of a lower priority
    function notice(u, kind) {
      if (!SNDT || sceneQuiet || u.owner !== viewed) return;
      if (now < sndNext[kind]) return;
      if (sndQ.some(e => e.kind === kind)) return;
      if (sndQ.length === SND_QUEUE) sndQ.pop();
      let k = 0;
      while (k < sndQ.length && SND_PRIORITY[sndQ[k].kind] >= SND_PRIORITY[kind]) k++;
      sndQ.splice(k, 0, { kind, tick: now, unit: u.id, type: u.type, priority: SND_PRIORITY[kind] });
    }
    // ACTIVATION as the state setter 0x48B090 notices it, for a unit whose script runs
    function noticeActive(u) {
      if (!COB || !u.cob || COB.resting(u.cob)) return;
      const on = COB.value(u.cob, COB_ACTIVATION) !== 0 ? 1 : 0;
      if (on === (u.active || 0)) return;
      u.active = on;
      notice(u, on ? SND.activate : SND.deactivate);
    }
    // THE CONSUMER 0x47F680, once a tick after its work
    function soundTick() {
      if (!SNDT || !sndQ.length) return;
      const e = sndQ.shift(), play = now >= sndLast + SND_GAP;
      if (play) sndLast = now;
      const count = SNDT.get(e.type).count[e.kind];
      if (!play || count <= 0) return;
      soundEvent(e.kind, -1, SNDT.get(e.type).cat, Math.trunc(sndDraw(e.unit, e.kind, now) * count / 0x8000));
      sndNext[e.kind] = now + 30 * SND_INTERVAL[e.kind];
    }
    // an order's first run says ok (0x438880 at its handler's start, the one kind-5 push, 0x438899), unless the scene
    // gave it; a build order's never does: MobileBuild, 0x403A20..0x403F6F, does not call 0x438880
    function begin(u, o) {
      if (o === u.begun) return;
      u.begun = o;
      // a feature's Reclaim never says ok: 0x404AD0 calls no 0x438880, where ReclaimUnit does (0x4047C1)
      if (!sceneOrders.has(o) && o.k !== 'build' && o.feat === undefined) notice(u, SND.ok);
    }
    // the cell a footprint f cells wide stands on at world coordinate p, rounded as the engine rounds both the unit's
    // (+0x76/+0x78, 0x48AA00..0x48AA32) and a move's goal (0x44CF60): (P - (f << 19) + 0x80000) >> 20 on 16.16 fixed point
    const footCell = (p, f) => Math.floor((p - f * (CELL / 2) + CELL / 2) / CELL);
    /* the engine's arrival for a move: its goal test 0x44D310 passes when the unit's footprint cell is within
       ((order+0x36 + 4) >> 4) cells of the goal's (0x44CF60's radius, from Move_Ground 0x403232..0x40323F); a click's
       move carries +0x36 = 0 (0x4991E2 pushes type 0 through 0x48CF30 and 0x43AFC0, which 0x43A116 stores there), so the
       radius is 0: the goal's own cell */
    const inGoalCell = (u, x, z) => {
      const [fx, fz] = fp(u.type);
      return footCell(u.x, fx) === footCell(x, fx) && footCell(u.z, fz) === footCell(z, fz);
    };
    /* MobileBuild's reach when the builder's move gave up (event 0x40, 0x403B63..0x403C39): the whole world units
       between it and the site's centre, less trunc(8 x its footprint's diagonal) and trunc(8 x the site's), beyond its
       BuildDistance (def+0x212) */
    const outOfReach = (u, t, ox, oz) => {
      const c = centre(t, ox, oz), dx = u.x - c.x, dz = u.z - c.z;
      const [ufx, ufz] = fp(u.type), [sfx, sfz] = fp(t);
      const uf = Math.sqrt(ufx * ufx + ufz * ufz), sf = Math.sqrt(sfx * sfx + sfz * sfz);
      return Math.floor(Math.sqrt(dx * dx + dz * dz)) - Math.trunc(8 * uf) + Math.trunc(-8 * sf) > def(u.type).buildDistance;
    };

    /* ---- units. Each faces along a unit vector (hx, hz); walk says why a vector. ---- */
    // built: the buildings of the player's finished so far; builtWaits: the "built" waits begun
    let units = [], nextId = 1, built = 0, builtWaits = 0;
    /* a unit joins the world. With the sidebar it carries its page state, unit+0x110: bp (bit 22, a numbered page
       is shown rather than the orders page) and page (bits 23..25, 1..n). 0x485CD5 creates a unit whose type has
       numbered pages with bit 22 set and page 1; any other unit has neither. */
    function enter(u) {
      if (ON) { const n = npages(u.type); u.bp = n >= 1; u.page = n >= 1 ? 1 : 0; }
      if (GAME) u.led = { eP: 0, eR: 0, eG: 0, eD: 0, mP: 0, mR: 0, mG: 0, mD: 0 };
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
    /* A SCENE'S MOBILE UNIT STARTS CLEAR (research/notes/ingame-wiki.md, *How units move*): at its authored centre,
       or, when its body there would overlap a scene structure or a mobile scene unit before it -- a casting's bigger
       units can -- at the nearest centre a whole number of cells away whose body overlaps neither and lies on the scene's map:
       nearest by distance, then by z, then by x, at most SPREAD cells either way. With none it stands where authored,
       and the step rule lets it walk out. The structures stay where authored. Settled before any unit enters, since
       entering starts its script, which may read where it stands. */
    const SPREAD = 8, spot = S.units.map(s => [s.at[0], s.at[1]]);
    {
      const rectAt = (t, x, z) => {
        if (mobile(t)) return [x - fp(t)[0] * CELL / 2, z - fp(t)[1] * CELL / 2, x + fp(t)[0] * CELL / 2, z + fp(t)[1] * CELL / 2];
        const ox = Math.round(x / CELL - fp(t)[0] / 2), oz = Math.round(z / CELL - fp(t)[1] / 2);
        return [ox * CELL, oz * CELL, (ox + fp(t)[0]) * CELL, (oz + fp(t)[1]) * CELL];
      };
      const clash = (a, b) => a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
      S.units.forEach((s, k) => {
        if (!mobile(s.type)) return;
        const free = (x, z) => {
          const a = rectAt(s.type, x, z);
          if (!(a[0] >= 0 && a[1] >= 0 && a[2] <= W * CELL && a[3] <= SH * CELL)) return false;
          return S.units.every((v, m) => (mobile(v.type) && m >= k) || !clash(a, rectAt(v.type, spot[m][0], spot[m][1]))) &&
            feats.every(f => !f.blocking || !clash(a, featRect(f)));
        };
        for (let d2 = 0; d2 <= 2 * SPREAD * SPREAD; d2++)
          for (let j = -SPREAD; j <= SPREAD; j++) for (let i = -SPREAD; i <= SPREAD; i++)
            if (i * i + j * j === d2 && free(s.at[0] + i * CELL, s.at[1] + j * CELL)) {
              spot[k] = [s.at[0] + i * CELL, s.at[1] + j * CELL];
              return;
            }
      });
    }
    for (const [k, s] of S.units.entries()) {
      const u = add(s.type, s.owner ?? 0, spot[k][0], spot[k][1], s.built ?? 1, s.id, s.label, facing(s.facing));
      u.orders = (s.orders || []).slice(0, PLACE_LIST_CAP).map(o => ({ k: 'move', x: o.move[0], z: o.move[1] }));
      for (const o of u.orders) sceneOrders.add(o);
      if (mobile(u.type)) continue;
      const b = box(u.type, u.ox, u.oz);
      if (!(b.ox >= 1 && b.oz >= 1 && b.ox + b.fx < W && b.oz + b.fz < SH) ||
          units.some(v => v !== u && !mobile(v.type) && overlap(b, box(v.type, v.ox, v.oz))) || feats.some(f => overlap(b, f)))
        throw new Error(`${u.id}'s ${u.type} does not fit the scene`);
    }
    sceneQuiet = false;
    /* DECISION 12 FOR AN UNBOUND CAST: a scene unit of an unbound type stops the demo on tick 0 under the entry's
       badge, the first such unit in creation order naming it (input() does the same for a `build` on its tick) */
    const unbound = t => defined(t) && D[t].unbound === true;
    const lock0 = S.units.find(s => unbound(s.type));
    /* SELECTION: one flag per unit (the state hash's item 3). Seat k's selection is the flagged units it owns, and
       every select and deselect of a seat's input touches that seat's own units alone, so an input of one seat never
       changes another's selection. The scene's selection is therefore of the input seats' units alone, each then its
       owner's: a unit of a seat at NIN or above would stay selected for good, as no input of its seat exists to
       deselect it and no release reaches it (tagpu_wikisim_init refuses it alike). */
    for (const id of S.selected) {
      const u = byId(id);
      if (!u) throw new Error(`the scene selects ${id}, which it does not have`);
      if (u.owner >= NIN) {
        const scene = demo && demo.scene && demo.scene.selected != null ? `demo ${demo.id}'s scene`
          : `the scene ${demo && demo.on !== undefined ? demo.on : Object.keys(entryIn.scenes)[0]}`;
        throw new Error(`entry ${entryIn.id}: ${scene} selects ${id} of seat ${u.owner}, which takes no input (seats 0..${NIN - 1} do)`);
      }
      u.sel = true;
    }
    const selected = (k = K) => units.filter(u => u.sel && u.owner === k);
    const deselect = () => { for (const u of units) if (u.owner === K) u.sel = false; };
    const builders = () => selected().filter(u => mobile(u.type) && def(u.type).workerTime > 0);
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
      for (const u of units) if (u.owner === K) for (const o of u.orders)
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
      // a blocking feature in the yard refuses it [INFERRED: the site test 0x47D2E0's feature cells; no entry builds over one]
      for (const f of feats) if (f.blocking && overlap(b, f)) return false;
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
      for (const u of units) if (u.owner === K && !(replaced && bs.includes(u)))
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
      for (const u of units) if (u.owner === K) for (const o of u.orders) {
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
      // Quick Build offers in the contextual mode only, main+0x2CC3 == 1 (tagpu_resource.c's gate): not with reclaim armed
      if (st.mode || st.rmode || st.held.has('alt')) return null;
      const { x, z } = mapCursor(), target = unitAt(x, z);
      let desired = null, old = 0;
      if (target) {
        if (target.owner !== K || mobile(target.type) || target.built < 1 || !(desired = kind(target.type))) return null;
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
    /* A SEAT'S INPUT STATE, IN[k] for each of the NIN input seats (decision 16): its cursor, from the scene's; the keys
       and buttons it holds, and where each button went down (downOn), which decides whether its release runs the map
       rules; the armed building (mode) and the drag; gesture: the placement gesture's press site while it lives;
       shiftPlaced: main+0x2CC6 bit 5, a placement made in this Shift hold, so releasing Shift disarms; spacing: empty
       cells between neighbours, per building type; place: the last evaluation of the gesture (tagpu_place.c's s_eval);
       reclaim armed (main+0x2CC3 == 12, exclusive with a building armed), the sweep's kind and its ring gesture (the
       press point, the last point over the map, past the threshold or not); the hover and the cursor's shape and the
       tick its sequence began; fed, the inputs the seat's script has fed (seat 0's trips too) since the sim was
       made, its kills and waits not counted: the replay layer's clash rule reads it (gameNow), and no hash does; and
       moved, whether the seat has moved its cursor since the sim was made: the game records a seat's cursor from its
       first move (the view's cursors), and no hash reads it either -- it decides nothing, and simparity compares the
       views. */
    const seatInput = () => ({ cursor: { x: S.cursor[0], z: S.cursor[1] }, held: new Set(), downOn: {}, mode: null, drag: null,
      hover: null, gesture: null, shiftPlaced: false, spacing: {}, place: null, rmode: false, kind: S.kind, rg: null,
      cursorShape: null, cursorSince: 0, fed: 0, moved: false });
    const IN = Array.from({ length: NIN }, seatInput);
    /* THE CLICKS, a game's (tagpu_wikisim.h WIKICLICK): each click action of any seat, {tick, seat, x, z} at the seat's
       map cursor, the CLICKS most recent kept, oldest first -- the ring the replay's overlay draws (OV_RING,
       tagpu_replay.c:484). The tick is the one the input is fed on (a script's tick t, or the tick the sim stands at for
       a player's), which the overlay ages a click from. Not hashed: it decides nothing, and simparity compares the
       views. A page demo records none. */
    const clicks = [];
    /* THE ORDERING, the acting seat: st is IN[K], and K is 0 between calls. sim.input and a fed script input act as
       their seat, sim.view(seat) views as it and the step's tail visits each seat in turn, each through asSeat, which
       puts K back before it returns. Nothing the input rules call re-enters input or view -- the COB host and the sound
       source are called by the step and read units, never input state -- so every read of st is the acting seat's.
       THE COLUMN IS SEAT 0'S: a game with the sidebar has NIN 1 (make refuses more), so its router, gadgets, capture
       and trips only ever run with K 0. The caption is the demo's, no seat's. */
    let st = IN[0], K = 0, caption = null;
    const asSeat = (k, f) => {
      const was = K;
      K = k; st = IN[k];
      try { return f(); } finally { K = was; st = IN[was]; }
    };
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
    /* where the cursor is: the column left of x = 0, else the map, which is as tall as the column. The map is closed
       at its far edges because the stage clamps a pointer beyond them onto exactly W x 16 or H x 16, which the map
       rules have always read as the map. */
    function over() {
      return ON && st.cursor.x < 0 ? 'side' : 'map';
    }
    // the cursor the map rules read: clamped onto the map, so a drag released over the column selects up to its edge
    const mapCursor = () => ON ? { x: Math.min(Math.max(st.cursor.x, 0), MW), z: Math.min(Math.max(st.cursor.z, 0), MH) } : st.cursor;
    /* the column, seat 0's: screen: the routed screen's name; shown: the shown unit (main+0x37E9C); cap: the gadget a
       press captured and the button that pressed it (gi+0x64, one slot, so a second press takes it over); and the
       demo's locked: the type the demo stopped on (lock) */
    const sb = { screen: null, shown: null, cap: null, locked: null };
    /* the demo stops under the badge: the first lock of the sim names the type, and the tick it falls on finishes as
       any tick does (a trip's data rule leaves only captions after its build there). Its sources: a build the
       layout cannot reach (trip), and a scene unit or a build of an unbound type, with or without the sidebar. */
    const lock = t => { if (sb.locked === null) sb.locked = t; };
    const screenOf = n => LAY.screens[n];
    /* DZ: in the frame the column starts at the top bar's first row (stage.js drawFrame), FRAME_BAR above the map; a
       page's column starts with the map. 0 on a page, so every page demo's arithmetic is the same doubles. A column's
       row meets a world z only through rowOf and zOf. */
    const DZ = GAME && ON ? -FRAME_BAR : 0;
    const rowOf = z => z - DZ, zOf = r => r + DZ;
    // a point of the stage in the column's own pixels is (x + 128, rowOf(z)); the minimap's 128 x 128 there holds no gadget
    const onMinimap = (x, z) => x + SIDE_W >= 0 && x + SIDE_W < MINIMAP && rowOf(z) >= 0 && rowOf(z) < MINIMAP;
    /* the engine's hit tests (the pump's 0x4AA1F6..0x4AA216, the button handler's 0x4A6B79..0x4A6B95 on the press
       and 0x4A6E84..0x4A6EAC on the release) take the whole pixels x .. x + w - 1 and y .. y + h - 1, DISASSEMBLED.
       The cursor here is a double, and the pixel it stands in is its floor, so that is half-open on the doubles. */
    const inRect = ([gx, gy, gw, gh], px, pz) => px >= gx && px < gx + gw && pz >= gy && pz < gy + gh;
    const inside = (s, g, x, z) => x < 0 && !onMinimap(x, z) && inRect(g.rect, x + SIDE_W, rowOf(z) - s.y);
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
        case 'stop': return selected(0).some(u => canStop(u.type)) ? +pressed : 2;
        /* RECLAIM is live while some selected unit of the player's has canreclamate: the router ORs def+0x245 bit
           0x400 over them (0x41B509..0x41B512, packed into main+0x37EC1 bit 0x20 at 0x41B6DA..0x41B6F7) and 0x41A120
           greys RECLAIM when the bit is clear (0x41A38C..0x41A3AF), DISASSEMBLED. It is a toggle (attribs 64): its
           face shows frame 1 while reclaim is armed, and every disarm resets STOP's radio group, RECLAIM's, so the
           frame follows the mode */
        case 'order': return isReclaim(g) && reclaimers().length ? +(pressed || st.rmode) : 2;
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
      const sel = selected(0), side = SIDE;
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
      // THE BUTTON'S SOUND, 0x41AA00's, and PREV and NEXT's on every press, 0x41C180 -> 0x41BDE0 / 0x41BF10
      // (tagpu_wikisim.c act)
      if (g.role === 'orders') soundNamed('ordersbutton');
      else if (g.role === 'buildtab') soundNamed('buildbutton');
      else if (g.role === 'stop') soundNamed('immediateorders');
      else if (g.role === 'prev' || g.role === 'next') soundNamed('nextbuildmenu');
      else if (g.role === 'build' && defined(g.unit) && !mobile(g.unit)) soundNamed('addbuild');
      if ((g.role === 'prev' || g.role === 'next') && n) {
        u.page = g.role === 'next' ? (u.page === n ? 1 : u.page + 1) : (u.page < 2 ? n : u.page - 1);
        u.bp = true;
      } else if (g.role === 'orders' && n) u.bp = false;
      else if (g.role === 'buildtab' && n) u.bp = true;
      else if (g.role === 'stop') {
        st.mode = null; st.gesture = null; st.rmode = false; st.rg = null;
        for (const v of selected()) v.orders = [];
      }
      else if (g.role === 'build' && defined(g.unit) && !mobile(g.unit)) arm(g.unit);
      else if (isReclaim(g)) toggleReclaim();
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
      return inRect([0, s.y, s.w, s.h], x + SIDE_W, rowOf(z));
    }
    // the release of the captured button: inside its gadget, still live, it acts; outside, nothing
    function releaseSide() {
      const c = sb.cap, s = screenOf(sb.screen), g = s.gadgets[c.i], { x, z } = st.cursor;
      sb.cap = null;
      if (live(gadgetState(g, sb.shown, false)) && inside(s, g, x, z)) act(g);
    }

    /* ---- THE RECLAIM SWEEP (tagpu_sweep.c; research/notes/gpu-status.md section 2.110), rule for rule over the sim's
       features. With reclaim armed a press on the map that travels RECL_DRAG or more is a ring round the press point;
       its release orders the selected reclaimers to take the features of the chosen kind it holds, split nearest-first
       between them. A shorter press is stock's click. A popup off RECLAIM's row picks the kind, and RECLAIM's own key
       cycles it. ---- */
    const reclaimers = () => selected().filter(u => canReclaim(u.type));
    // RECLAIM: an order button whose name holds RECLAIM, the orders handler's strstr (0x419F56)
    const isReclaim = g => g.role === 'order' && g.name.includes('RECLAIM');
    // the routed screen's first RECLAIM, its index, -1 for none (tagpu_sweep.c reclaim_gadget)
    const reclaimIndex = () => ON ? screenOf(sb.screen).gadgets.findIndex(isReclaim) : -1;
    // tagpu_sweep.c kind_ok: the kind's amount is above 0, ALL takes any
    const kindOk = f => st.kind === 'metal' ? f.metal > 0 : st.kind === 'energy' ? f.energy > 0 : true;
    // a target of the kind by the click's own rules: reclaimable and mapped at its centre (0x408090)
    const takes = f => f.reclaimable && kindOk(f) && seen(Math.floor(f.x / CELL), Math.floor(f.z / CELL));
    /* ARMING AND DISARMING, the orders handler's RECLAIM branch (0x419F3D..0x419FAF): the toggle on writes mode 12
       (0x419F8A), off mode 1 (0x419F71); both play specialorders, and neither writes the Shift-placement bit, the cursor
       or an order. Mode 12 replaces a building armed, and its gesture goes with it. */
    function toggleReclaim() {
      if (st.rmode) { st.rmode = false; st.rg = null; }
      else { st.mode = null; st.gesture = null; st.drag = null; st.rmode = true; }
      soundNamed('specialorders');
    }
    /* THE POPUP, tagpu_sweep.c popup_rect at the stage's scale: 124 x 72 at x POP_X of the column (one past the map's
       left edge), its middle centred on RECLAIM's row and held inside the map's rows, while reclaim is armed and RECLAIM
       is on the routed screen, shown and live; its three rows 112 x 18, 21 apart; the tongue to the button's row. In the
       stage's coordinates: x from the map's left edge, z the world's, the button's row taken there through zOf and the
       popup held inside the map beside the column. null for none. */
    function popup() {
      const i = reclaimIndex();
      if (!st.rmode || i < 0) return null;
      const s = screenOf(sb.screen), g = s.gadgets[i];
      if (g.hidden || !live(gadgetState(g, sb.shown, false))) return null;
      const by = zOf(s.y + g.rect[1]), hi = Math.min(MH, zOf(LAY.height)) - POP_H;
      if (hi < 0) return null;
      const y0 = Math.min(Math.max(by + Math.trunc(g.rect[3] / 2) - POP_H / 2, 0), hi), x0 = POP_X - SIDE_W;
      return {
        rect: [x0, y0, POP_W, POP_H],
        rows: [0, 1, 2].map(k => [x0 + 6, y0 + 6 + 21 * k, 112, 18]),
        tongue: [124 - SIDE_W, Math.min(Math.max(by + 4, y0 + 6), y0 + POP_H - 24), 6, 18],
      };
    }
    const onPopup = P => !!P && over() === 'map' && inRect(P.rect, st.cursor.x, st.cursor.z);
    /* A KEY, tagpu_sweep_char then the GUI pump's match: with reclaim armed, RECLAIM's own quickkey on the routed screen
       cycles the kind and the engine never sees it; any other key goes to the pump, which offers it to the screen's
       buttons in file order (0x4A6AE0, matched through tolower and toupper at 0x4A70FB..0x4A7129), and the first live
       one with that quickkey acts as its click does; a greyed one refuses at once (0x4A6B10) and an inactive one is
       skipped. Only E is a key of the sim. */
    const lower = c => c >= 65 && c <= 90 ? c + 32 : c, upper = c => c >= 97 && c <= 122 ? c - 32 : c;
    const keyIs = (g, c) => !!g.key && (c === lower(g.key) || c === upper(g.key));
    function keyPress(k) {
      if (!ON) return;
      const c = k.charCodeAt(0), s = screenOf(sb.screen), i = reclaimIndex();
      if (st.rmode && i >= 0 && live(gadgetState(s.gadgets[i], sb.shown, false)) && keyIs(s.gadgets[i], c)) {
        st.kind = KINDS[(KINDS.indexOf(st.kind) + 1) % KINDS.length];
        return;
      }
      const g = s.gadgets.find(g => !g.hidden && keyIs(g, c) && live(gadgetState(g, sb.shown, false)));
      if (g) { sb.cap = null; act(g); }
    }
    /* A LEFT PRESS ON THE MAP WITH RECLAIM ARMED: on the popup it picks the row under it (between rows nothing) and is
       taken; anywhere else it starts the gesture at the press point (tagpu_sweep_button). */
    function reclaimPress() {
      const P = popup();
      if (onPopup(P)) {
        const r = P.rows.findIndex(q => inRect(q, st.cursor.x, st.cursor.z));
        if (r >= 0) st.kind = KINDS[r];
        return;
      }
      const c = mapCursor();
      st.rg = { x0: c.x, z0: c.z, x1: c.x, z1: c.z, drag: false };
    }
    // the drag follows the pointer while it is over the map (region bit 1), and is a ring once it has gone RECL_DRAG
    function reclaimMove() {
      if (!st.rg || over() !== 'map') return;
      const c = mapCursor();
      st.rg.x1 = c.x; st.rg.z1 = c.z;
      if (Math.sqrt(sq(c.x - st.rg.x0) + sq(c.z - st.rg.z0)) >= RECL_DRAG) st.rg.drag = true;
    }
    // where an order stands, for the split's start: a move's point, a build's site, a reclaim's target; a guard none
    const orderPoint = o => o.k === 'move' ? { x: o.x, z: o.z } : o.k === 'build' ? centre(o.type, o.ox, o.oz) :
      o.k === 'reclaim' ? (o.feat !== undefined ? { x: o.x, z: o.z } : byId(o.target)) : null;
    // the ring of gesture g: its centre, the press point in whole world units, and the drag's length, bounded
    const ringOf = g => ({ x: Math.floor(g.x0), z: Math.floor(g.z0), r: Math.trunc(Math.min(Math.sqrt(sq(g.x1 - g.x0) + sq(g.z1 - g.z0)), RING_MAX)) });
    /* WHICH FEATURES A RING HOLDS (enumerate): every feature whose centre is inside it that `takes` holds, nearest the
       centre first, then by z, then x, then the anchor cell */
    function ringTargets(ring) {
      const t = feats.filter(f => sweepInside(ring.x, ring.z, ring.r, f.x, f.z) && takes(f));
      const d = f => dist2(f.x, f.z, ring.x, ring.z), cell = f => f.oz * W + f.ox;
      return t.sort((a, b) => d(a) - d(b) || a.z - b.z || a.x - b.x || cell(a) - cell(b)).slice(0, SWEEP_MAX_TARGETS);
    }
    // the stock tail of an order click (0x4958C0): with Shift the Shift-placement bit, so releasing Shift disarms;
    // without it mode 1 and the bit cleared
    function clickTail(shift) {
      if (shift) st.shiftPlaced = true;
      else { st.rmode = false; st.rg = null; st.shiftPlaced = false; }
    }
    /* STOCK'S CLICK IN MODE 12, at the release (the route's 2, tagpu_sweep_button): a feature under the pointer that the
       kind takes is ordered to every selected reclaimer -- replacing its orders, or with Shift toggled as 0x43AFC0 does
       (a reclaim of it within 16 world units taken off instead of one added) -- and the click's tail runs. A feature of
       the other kind shows cursornormal and its click orders nothing; nothing reclaimable under the pointer orders
       nothing either and keeps the mode [INFERRED for empty ground]. A unit under the pointer is not modelled. */
    function reclaimClick(shift) {
      const c = mapCursor(), f = featAt(c.x, c.z);
      if (!f || !takes(f)) return;
      for (const u of reclaimers()) {
        const k = shift ? u.orders.findIndex(o => o.k === 'reclaim' && o.feat !== undefined && within(f, o)) : -1;
        if (k >= 0) u.orders.splice(k, 1);
        else assign(u, [{ k: 'reclaim', feat: f.id, x: f.x, z: f.z }], shift);
      }
      clickTail(shift);
    }
    /* THE COMMIT, tagpu_sweep.c commit: the ring's targets; without Shift every order of every reclaimer but the reclaims
       on those targets goes (cancel_others); targets a reclaimer already has queued are dropped (drop_queued); each
       reclaimer's split inputs from its list (builder_inputs: it starts at its last positioned order, its load the
       queued feature reclaims' travel and ticks from where it stands); the split; every assignment appended. Its tail
       runs only when it ordered or took something off, so an empty ring keeps reclaim armed. */
    function sweepCommit(g, shift) {
      const ring = ringOf(g), bs = reclaimers();
      let targets = ringTargets(ring), cancelled = 0;
      if (!bs.length) return;
      const onRing = o => o.k === 'reclaim' && o.feat !== undefined && targets.some(f => f.id === o.feat);
      if (targets.length) {
        if (!shift) for (const u of bs) { const n = u.orders.length; u.orders = u.orders.filter(onRing); cancelled += n - u.orders.length; }
        targets = targets.filter(f => !bs.some(u => u.orders.some(o => o.k === 'reclaim' && o.feat === f.id)));
      }
      let issued = 0;
      if (targets.length) {
        const ins = bs.map(u => {
          // the game's builder_inputs reads every position in whole world units, a 16.16 word >> 16
          const d = def(u.type), b = { x: Math.floor(u.x), z: Math.floor(u.z), speed: d.maxVelocity, reach: d.buildDistance || 0, load: 0, room: 0 };
          let px = b.x, pz = b.z;
          for (const o of u.orders) {
            const p = orderPoint(o);
            if (p) { b.x = Math.floor(p.x); b.z = Math.floor(p.z); }
            const f = o.k === 'reclaim' && o.feat !== undefined ? featById(o.feat) : null;
            if (f) {
              const dd = Math.sqrt(sq(f.x - px) + sq(f.z - pz));
              b.load += (b.speed > 0 && dd > b.reach ? (dd - b.reach) / b.speed : 0) + sweepReclaimTicks(f.energy, f.metal);
              px = f.x; pz = f.z;
            }
          }
          b.room = u.orders.length >= SWEEP_LIST_CAP ? 0 : SWEEP_LIST_CAP - u.orders.length;
          return b;
        });
        for (const a of sweepSplit(ins, targets.map(f => ({ x: f.x, z: f.z, ticks: sweepReclaimTicks(f.energy, f.metal) })))) {
          const f = targets[a.seq];
          bs[a.who].orders.push({ k: 'reclaim', feat: f.id, x: f.x, z: f.z });
          issued++;
        }
      }
      if (issued || cancelled) clickTail(shift);
    }
    /* THE RELEASE of a press that went down on the map with reclaim armed: past the threshold it commits wherever it
       lands; below it, a click over the map, and off the map nothing */
    function reclaimRelease() {
      const g = st.rg, shift = st.held.has('shift');
      st.rg = null;
      if (g.drag) sweepCommit(g, shift);
      else if (over() === 'map') reclaimClick(shift);
    }

    /* THE BUILD TRIP. A data input {build: T} at tick t arms nothing itself: it becomes generated inputs, a glide, the
       pager presses and the button's own press, that go through exactly the path the player's clicks take in Try it.
       They are made when tick t is fed, from the state at t, and the arm is the release at t + TRIP_UP whatever the
       route (TRIP_ROWS says why it is fixed). */
    const gen = [];
    /* feeding: the tick being fed, and between two ticks the one the sim stands at, since every step feeds its tick:
       the clock an input reads */
    let genNext = 0, feeding = 0;
    const centreOf = (s, g) => ({ x: -SIDE_W + g.rect[0] + g.rect[2] / 2, z: zOf(s.y + g.rect[1] + g.rect[3] / 2) });
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
    /* THE RECLAIM TRIP's plan: RECLAIM on the routed screen, live, pressed where it is; else the screen's live ORDERS
       tab (a tab acts on its press) and then RECLAIM on the screen ORDERS routes to, the shown type's orders page or the
       side's GEN; each point checked to land on its gadget. null: RECLAIM cannot be reached here. */
    function planReclaim() {
      const u = sb.shown, find = name => screenOf(name).gadgets.findIndex(g => isReclaim(g) && live(gadgetState(g, u, false)));
      const at = (name, i) => { const s = screenOf(name), p = centreOf(s, s.gadgets[i]); return gadgetAt(name, u, p.x, p.z) === i ? p : null; };
      let i = find(sb.screen);
      if (i >= 0) { const b = at(sb.screen, i); return b ? { k: 0, presses: [], button: b } : null; }
      const s = screenOf(sb.screen), ti = s.gadgets.findIndex(g => g.role === 'orders' && live(gadgetState(g, u, false)));
      const TP = u && typePages(u.type);
      if (ti < 0 || !TP) return null;
      const tab = at(sb.screen, ti), to = TP.orders || SIDE.gen;
      if (!tab || (i = find(to)) < 0) return null;
      const b = at(to, i);
      return b ? { k: 1, presses: [tab], button: b } : null;
    }
    /* THE GLIDE, exact in both languages: step i of n from a to b is a + trunc((b - a) * (i*i*(3n - 2i)) / (n*n*n)),
       each axis, evaluated in that order. Step n is b itself: the formula reaches b only when b - a is a whole
       number, and the cursor must stand exactly on the button and exactly back where it started. */
    const glideAt = (a, b, i, n) => i === n ? b : a + Math.trunc((b - a) * (i * i * (3 * n - 2 * i)) / (n * n * n));
    // T: the type a build trip arms, or null for the reclaim trip
    function trip(T) {
      const t = feeding, plan = T === null ? planReclaim() : planTrip(T);
      /* OWNER QUESTION Q-O2, the recommended answer: a build this layout cannot reach stops the demo here, holding the
         frame under the entry's "Turned off in this mod" badge (decision 4), rather than playing on unarmed; so does
         a RECLAIM it cannot reach */
      if (!plan) { lock(T === null ? 'RECLAIM' : T); return; }
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
    // the pick trip: no popup to click stops the demo as a RECLAIM it cannot reach does
    function pickTrip(k) {
      const t = feeding, P = popup();
      if (!P) { lock('RECLAIM'); return; }
      const r = P.rows[KINDS.indexOf(k)], a = { x: st.cursor.x, z: st.cursor.z };
      const B = { x: r[0] + r[2] / 2, z: r[1] + r[3] / 2 };
      const push = (dt, ev) => gen.push({ at: t + dt, ev });
      for (let i = 1; i <= PICK_DOWN; i++) push(i, { move: [glideAt(a.x, B.x, i, PICK_DOWN), glideAt(a.z, B.z, i, PICK_DOWN)] });
      push(PICK_DOWN, { down: 'lmb' });
      push(PICK_UP, { up: 'lmb' });
      for (let i = 1; i <= PICK - PICK_UP; i++) push(PICK_UP + i, { move: [glideAt(B.x, a.x, i, PICK - PICK_UP), glideAt(B.z, a.z, i, PICK - PICK_UP)] });
    }
    /* THE DATA RULE, refused here as tagpu_wikidata.c's loader refuses it: with the sidebar on, a build names a type;
       nothing but a caption stands in its trip, the ticks (t, t + TRIP), nor after it in tick t; the demo lasts the
       whole trip; and the left button is up when it starts, as the trip's own presses need it. */
    function checkTrips(script, end) {
      const captionOnly = e => Object.keys(e).every(k => k === 'at' || k === 'caption');
      let lmb = false;
      script.forEach((e, i) => {
        if ('build' in e || 'reclaim' in e || 'pick' in e) {
          const what = 'build' in e ? 'build' : 'reclaim' in e ? 'reclaim' : 'pick', at = `demo ${demo.id}: the ${what} at tick ${e.at}`;
          const span = what === 'pick' ? PICK : TRIP;
          if (what === 'build' && (typeof e.build !== 'string' || !e.build)) throw new Error(`${at} names no type`);
          if (what === 'reclaim' && e.reclaim !== true) throw new Error(`${at} is not true`);
          if (what === 'pick' && !KINDS.includes(e.pick)) throw new Error(`${at} names no kind`);
          if (!Object.keys(e).every(k => k === 'at' || k === what || k === 'caption')) throw new Error(`${at} carries another input`);
          if (lmb) throw new Error(`${at} starts with the left button held`);
          const f = script.slice(i + 1).find(f => f.at < e.at + span && !captionOnly(f));
          if (f) throw new Error(`${at}: an input at tick ${f.at} stands in its trip, which ends at ${e.at + span}`);
          if (!(end >= e.at + span)) throw new Error(`${at}: the demo ends at ${end}, inside its trip`);
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
      // stock's `oktobuild`, once a gesture, as tagpu_place.c's commit plays it
      if (changed) { st.shiftPlaced = true; soundNamed('oktobuild'); }
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
    const disarm = () => { st.mode = null; st.gesture = null; st.shiftPlaced = false; st.rmode = false; st.rg = null; };
    /* ARMING a building, the build button's branch of 0x41AA00, DISASSEMBLED: main+0x2CC3 = 0x0E (0x41AB89) and the
       type at main+0x2CC4 (0x41AB9C), and no write to main+0x2CC6. So a Shift-placement made earlier in this Shift
       hold keeps bit 5, and releasing Shift disarms the building armed since as well: 0x4992D8..0x49930F tests bit 5,
       not the mode. tagpu_place.c leaves the bit too (it sets it only on a commit), and ends its gesture when the armed
       type changes (its evaluate, `s_gesture.type != c.type`), so a gesture outlives a re-arm of its own type only. */
    const arm = t => { if (st.mode !== t) st.gesture = null; st.mode = t; st.drag = null; st.rmode = false; st.rg = null; };
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
      const ms = selected().filter(u => mobile(u.type));
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
    const unfinishedOwn = u => u.owner === K && !mobile(u.type) && u.built < 1;
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
      if (u && u.owner === K) return unfinishedOwn(u) && builders().length ? assist(u, shift) : select(shift);
      moveOrder(shift);
    }
    /* TYPE 0, a right press on the map with nothing armed (0x499172..0x499196, DISASSEMBLED): without Ctrl every unit
       is deselected (0x48BD00), at the press (MEASURED 2026-10-28: the ARMCK was no longer selected before the release).
       With Ctrl it calls 0x41CC60, which hands the mouse to a view mode (main+0x2CDF = 1, the camera follow released,
       the cursor centred) that the stage, having no camera to move, does not model; the selection stays (MEASURED),
       and main+0x2CDF read 0 after the press, so the view mode itself was not seen. */
    function rightPress0() {
      if (!st.held.has('ctrl')) deselect();
    }
    /* a build-menu placement, stock's click: the selected builders with that button get it; without Shift the menu mode
       ends. With Shift (0x43AFC0) the first build order of the builder's within 16 world units of the site is taken off
       instead of adding one, and the placement sets the Shift-placement bit: releasing Shift then disarms. */
    function placeClick(shift) {
      const h = mapHover();
      // stock's click 0x498F70: a refused site says `notoktobuild` and keeps the building armed, a placed one `oktobuild`
      if (!h.ok) { soundNamed('notoktobuild'); return; }
      const at = centre(h.type, h.ox, h.oz);
      for (const u of builders()) if (canBuild(u, h.type)) {
        const k = shift ? u.orders.findIndex(o => o.k === 'build' && within(at, centre(o.type, o.ox, o.oz))) : -1;
        if (k >= 0) u.orders.splice(k, 1);
        else assign(u, [{ k: 'build', type: h.type, ox: h.ox, oz: h.oz }], shift);
      }
      soundNamed('oktobuild');
      if (!shift) disarm();
      else st.shiftPlaced = true;
    }
    // box: the release that ends a press on the map outside the click window selects what the box from the press holds
    function select(shift, box) {
      const d = st.drag, { x, z } = mapCursor();
      if (box) {
        const x0 = Math.min(d.x0, x), x1 = Math.max(d.x0, x), z0 = Math.min(d.z0, z), z1 = Math.max(d.z0, z);
        const inBox = units.filter(u => u.owner === K && u.x >= x0 && u.x <= x1 && u.z >= z0 && u.z <= z1);
        const pick = inBox.some(u => mobile(u.type)) ? inBox.filter(u => mobile(u.type)) : inBox;
        if (!shift) deselect();
        for (const u of pick) u.sel = true;
        // the box's sound (0x48C65A..0x48C681): one unit says its select, more the allsound SelectMultipleUnits
        if (pick.length === 1) notice(pick[0], SND.select);
        else if (pick.length > 1) soundNamed('SelectMultipleUnits');
        return;
      }
      const u = unitAt(x, z);
      if (u && u.owner === K) {
        if (shift) u.sel = !u.sel;
        else { deselect(); u.sel = true; }
        // the click's select says it when its unit is left selected (0x48C8B9, 0x48C997)
        if (u.sel) notice(u, SND.select);
      } else if (!shift) deselect();
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
    /* A CLICK ACTION of the acting seat, recorded before the action runs (CLICKS): the left click's -- 0x4993C5..0x4993D0
       and 0x499435..0x499441, here a placement at a press, the sweep's press, the click branch of a release (select at
       type 1, leftClick at type 0) and a placement off the map (perspective-streams E6) -- and the right press's,
       0x499100, every right press on the map: rightClick, rightPress0 and the cancel of an armed building or sweep.
       [INFERRED] a right press over the column is the GUI's and records none; a players game has no column. */
    function clickAction() {
      if (!GAME) return;
      const c = mapCursor();
      clicks.push({ tick: feeding, seat: K, x: c.x, z: c.z });
      if (clicks.length > CLICKS) clicks.shift();
    }
    function input(ev) {
      if ('caption' in ev) caption = ev.caption || null;
      if ('build' in ev) {
        if (unbound(ev.build)) lock(ev.build);
        else if (TRIPS) trip(ev.build);
        else if (ev.build) arm(ev.build);
        else { disarm(); st.drag = null; }
      }
      // a `reclaim` arms reclaim through RECLAIM: a trip to the button with the sidebar on, at once without it
      if (ev.reclaim) { if (TRIPS) trip(null); else if (!st.rmode) toggleReclaim(); }
      // a `pick` clicks the popup's row of that kind: a trip there with the sidebar on, the kind at once without it
      if (ev.pick) { if (TRIPS) pickTrip(ev.pick); else st.kind = ev.pick; }
      if (ev.move) {
        st.cursor = { x: ev.move[0], z: ev.move[1] };
        st.moved = true;
        const c = mapCursor();
        if (st.drag && sq(c.x - st.drag.x0) + sq(c.z - st.drag.z0) > DRAG * DRAG) st.drag.active = true;
        reclaimMove();
      }
      if (ev.down === 'z' || ev.down === 'x') { st.held.add(ev.down); stepSpacing(ev.down); }
      else if (ev.down === 'e') { st.held.add('e'); keyPress('e'); }
      else if (ev.down && !st.held.has(ev.down)) {
        st.held.add(ev.down);
        const shift = st.held.has('shift'), where = over(), mouse = ev.down === 'lmb' || ev.down === 'rmb';
        if (mouse) st.downOn[ev.down] = where;
        /* A RIGHT PRESS WHILE A RECLAIM DRAG'S LEFT PRESS IS HELD, wherever it lands, ends the drag and keeps reclaim
           armed; the window procedure takes it and its release before the engine or the GUI sees either
           (tagpu_sweep_mouse) */
        if (ev.down === 'rmb' && st.rg) { st.rg = null; st.downOn.rmb = null; }
        else if (mouse && where === 'side' && onScreen()) pressSide(ev.down);
        else if (where === 'map') {
          // with a building armed a left press carrying Shift starts a gesture at the cursor site instead of placing
          if (ev.down === 'lmb') {
            if (st.rmode) { clickAction(); reclaimPress(); }
            else if (st.mode && shift) st.gesture = cursorSite();
            else if (st.mode) { clickAction(); placeClick(false); }
            else { const c = mapCursor(); st.drag = { x0: c.x, z0: c.z, t: feeding, active: false }; }
          }
          // with a building armed a right press cancels it at both types (0x499100, MEASURED at type 1)
          // with a building or reclaim armed a right press is the handler's cancel
          if (ev.down === 'rmb') {
            st.drag = null;
            clickAction();
            if (st.mode || st.rmode) disarm();
            else if (IFACE) rightClick(shift); else rightPress0();
          }
        }
        /* OFF THE MAP AND OFF THE SCREEN a left press with a building armed places it once, at the press. The handler
           finds bit 1 clear, so place_button returns 0 at the route 0x4993BF and stock's click 0x498F70 runs; the
           resource module's gate there re-runs the build cursor 0x4197D0 on the point 0x498DA0 clamped to the view's
           edge (0x498E32..), and 0x419670 places that site. Shift keeps the building armed (bit 5, 0x498FB4), and
           without it the press disarms (0x498FC0); no gesture starts, and the release adds nothing. MEASURED
           2026-10-27, Interface Type 1, 1024 x 768: a press at (60, 600), below the screen, placed one site at the
           clamped point (1148, 6416), snapped to (1144, 6424). The sim's clamp onto the map stands for the engine's
           onto the view, because the stage shows the map where the game shows a view. The minimap stays inert (the owner's decision 6), where the game would place at its world
           point. A right press there is the handler's cancel and nothing else. */
        else if (ev.down === 'lmb' && st.mode && !onMinimap(st.cursor.x, st.cursor.z)) { clickAction(); placeClick(shift); }
        else if (ev.down === 'rmb' && (st.mode || st.rmode)) disarm();
      }
      if (ev.up && st.held.has(ev.up)) {
        st.held.delete(ev.up);
        // a release runs the map rules only for a press that went down on the map
        if (sb.cap && sb.cap.b === ev.up) releaseSide();
        else if (st.downOn[ev.up] === 'map') {
          if (ev.up === 'lmb' && st.rg) reclaimRelease();
          else if (ev.up === 'lmb' && st.gesture) release();
          else if (ev.up === 'lmb' && st.drag) {
            // a box is the same at both types; a click is type 1's select and type 0's leftClick
            const d = st.drag, c = mapCursor(), shift = st.held.has('shift');
            const click = feeding - d.t < CLICK_TIME && Math.abs(c.x - d.x0) < CLICK_BOX && Math.abs(c.z - d.z0) < CLICK_BOX;
            if (!click) select(shift, true);
            else { clickAction(); if (IFACE) select(shift); else leftClick(shift); }
            st.drag = null;
          }
        }
        // 0x4992D8: Shift found up after a placement in this hold disarms, as after a stock Shift-click
        if (ev.up === 'shift') { if (st.shiftPlaced) disarm(); st.shiftPlaced = false; }
      }
      if (ON) route();
      st.hover = hover();
    }

    /* ---- one tick. Units walk at MaxVelocity (world units a tick) as MOVEMENT below says, and stop once within
       BuildDistance of the footprint; a building fills at its builders' summed WorkerTime over its BuildTime (worker
       time a second), times the entry's pace.build, with no cost; a reclaim takes as long as building it would. Guards
       work on whatever their guarded unit works on this tick, so they are resolved after everyone else. ---- */

    /* ---- MOVEMENT (research/notes/ingame-wiki.md, *How units move*). A rect is [x0, z0, x1, z1] in world units.
       A mobile unit's BODY is its footprint's box about its centre, a structure's its footprint, and a builder within
       build distance of a site it has not started yet holds the site as a CLAIM, a body of no unit. Bodies overlap
       only with area in common: touching is not overlapping.

       THE STEP RULE, which every step obeys: a step may not make its unit overlap a body it does not overlap already,
       nor take its body off the map. So no step ever puts two bodies together, and a unit that something was put
       over (a scene, a structure started on it) can still walk out.

       A unit with somewhere to go walks straight at it while the straight line is clear of every STILL body -- a
       structure, a claim, a unit that did not step on the last tick -- and otherwise along the PLANNER's path round
       every body where it stands now. A unit walking does not take a straight line from another: a step one of them
       refuses turns 45 and then 90 degrees left and right, and failing all of them the unit waits for the tick. The
       planner counts the walking too, since a unit that walks one tick in two (a guard keeping up with its leader)
       would otherwise put its body on and off every path planned past it. A unit the planner has no nearer place for is STUCK:
       a move ends there, any other order waits.

       A path is walked one LEG at a time, a leg being the walk to one node: the unit keeps its leg until it stands on
       the node, until a body covers the node, until its straight step is refused (a turned step ends the leg
       too), or until its order or target changes, and only then plans again. Planning afresh every tick would dither: from between two nodes the nearer
       one can lie behind. A leg crosses no structure: one starts on a node or within a cell of its node on each axis,
       so the box sweeps only cells the unit's body or the node's already covers, and a corner leg needs both cells
       beside it free.

       THE HEADING IS A VECTOR, NOT AN ANGLE. An angle would come from Math.atan2, which no standard rounds correctly,
       so its last bit differs between JavaScript engines and C libraries, and tagpu_wikisim.c would drift from this
       file. A step's direction is + - * / and sqrt, each rounded once, and a turn multiplies it by Math.SQRT1_2, the
       correctly rounded double the C literal names too, so both simulations hold the same doubles. ---- */
    const ARRIVED = 1, STEPPED = 2, WAITING = 3, STUCK = 4, EDGE = { r: null, u: null };
    const C45 = Math.SQRT1_2, DIAG = CELL * Math.SQRT2;
    // a step's tries, (cos, sin) of its turn: straight on, 45 degrees either way, then 90
    const TURNS = [[1, 0], [C45, C45], [C45, -C45], [0, 1], [0, -1]];
    // the planner's neighbours: the four sides, then the four corners
    const DIRS = [[1, 0], [-1, 0], [0, 1], [0, -1], [1, 1], [1, -1], [-1, 1], [-1, -1]];
    const hits = (a, b) => a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
    const meets = (a, b) => a[0] <= b[2] && b[0] <= a[2] && a[1] <= b[3] && b[1] <= a[3];
    const sameRect = (a, b) => a[0] === b[0] && a[1] === b[1] && a[2] === b[2] && a[3] === b[3];
    const onMap = r => r[0] >= 0 && r[1] >= 0 && r[2] <= W * CELL && r[3] <= H * CELL;
    const rectOf = (t, ox, oz) => [ox * CELL, oz * CELL, (ox + fp(t)[0]) * CELL, (oz + fp(t)[1]) * CELL];
    const bodyAt = (u, x, z) => {
      const ax = fp(u.type)[0] * CELL / 2, az = fp(u.type)[1] * CELL / 2;
      return [x - ax, z - az, x + ax, z + az];
    };
    const body = v => mobile(v.type) ? bodyAt(v, v.x, v.z) : rectOf(v.type, v.ox, v.oz);
    // the distance from (x, z) to rect r, 0 inside it
    function rectDist(x, z, r) {
      const dx = Math.max(r[0] - x, 0, x - r[2]), dz = Math.max(r[1] - z, 0, z - r[3]);
      return Math.sqrt(dx * dx + dz * dz);
    }
    // the gap between rects a and b, 0 when they touch or overlap
    function rectGap(a, b) {
      const dx = Math.max(b[0] - a[2], 0, a[0] - b[2]), dz = Math.max(b[1] - a[3], 0, a[1] - b[3]);
      return Math.sqrt(dx * dx + dz * dz);
    }
    /* the site u's first order claims: a build not started yet that u has reached -- its centre within build distance
       of the site, or its body within a step of it. The second is how a builder whose BuildDistance is under its half
       width arrives (touching the site, or stopped short of it by the site refusing its step), and it must claim
       there too, or what stands on the site is never told to leave it and the build waits for good. */
    function claim(u) {
      const o = u.orders[0];
      if (!o || o.k !== 'build' || started(o)) return null;
      const r = rectOf(o.type, o.ox, o.oz);
      return rectDist(u.x, u.z, r) <= def(u.type).buildDistance || rectGap(body(u), r) <= def(u.type).maxVelocity ? r : null;
    }
    // every body but u's, as {r, u}: the units' in creation order, then the claims in their claimants' order
    function bodies(u) {
      const out = [];
      for (const v of units) if (v !== u) out.push({ r: body(v), u: v });
      for (const f of feats) if (f.blocking) out.push({ r: featRect(f), u: null });
      for (const v of units) if (mobile(v.type)) { const r = claim(v); if (r) out.push({ r, u: null }); }
      return out;
    }
    const still = b => !b.u || !mobile(b.u.type) || !b.u.went;
    // a body that is the target itself: its body (the site, a structure on it or a claim of it; the guarded unit)
    const isTarget = (b, tgt) => !!tgt.body && sameRect(b.r, tgt.body);
    /* A TARGET: the point aimed at (x, z); in range within `stop` of rect r, or touching `body`, the target's own
       body (null for a point); a straight walk ends `clip` short of the point; the planner's goal is within `plan` of
       r, or touching `body`; `order` and `kind` name it for the leg */
    const toPoint = (o, x, z) => ({ x, z, r: [x, z, x, z], stop: 0, clip: 0, plan: CELL, body: null, order: o, kind: 0 });
    const toSite = (o, u, r) => {
      const bd = def(u.type).buildDistance;
      return { x: (r[0] + r[2]) / 2, z: (r[1] + r[3]) / 2, r, stop: bd, clip: 0, plan: bd, body: r, order: o, kind: 1 };
    };
    /* a feature's Reclaim walks until the footprints touch: its goal 0x44D8A0 is the ring of cells round the feature's
       footprint widened by the unit's, which 0x44DCB0 passes when the unit's cell lies on it; BuildDistance is not read */
    const toFeat = (o, f) => { const r = featRect(f); return { x: f.x, z: f.z, r, stop: 0, clip: 0, plan: CELL, body: r, order: o, kind: 1 }; };
    const toUnit = (o, g) => ({ x: g.x, z: g.z, r: [g.x, g.z, g.x, g.z], stop: FOLLOW, clip: FOLLOW, plan: FOLLOW, body: body(g), order: o, kind: 2 });
    const inRange = (x, z, b, tgt, within) => rectDist(x, z, tgt.r) <= within || (!!tgt.body && meets(b, tgt.body));

    // THE STEP RULE: the body u's step to (x, z) would newly overlap, the first in `obs`'s order, or EDGE; null if none
    function refuses(u, x, z, obs) {
      const a = bodyAt(u, x, z), now = body(u);
      if (!onMap(a) && onMap(now)) return EDGE;
      for (const b of obs) if (hits(a, b.r) && !hits(now, b.r)) return b;
      return null;
    }
    /* u steps `len` along the unit vector (hx, hz) -- to (ex, ez) exactly when that is given, the end of a path's leg
       -- or along the first turn of it the step rule lets go. ARRIVED, with no step, when what refuses the straight
       step is the target itself; WAITING when every try is refused. u.turned says whether a turn went. */
    function step(u, hx, hz, len, obs, tgt, ex, ez) {
      for (let k = 0; k < TURNS.length; k++) {
        const c = TURNS[k][0], s = TURNS[k][1];
        const dx = k ? hx * c - hz * s : hx, dz = k ? hx * s + hz * c : hz;
        const x = !k && ex !== undefined ? ex : u.x + dx * len, z = !k && ez !== undefined ? ez : u.z + dz * len;
        const r = refuses(u, x, z, obs);
        if (!r) { u.x = x; u.z = z; u.hx = dx; u.hz = dz; u.stepped = true; u.turned = k > 0; return STEPPED; }
        if (!k && tgt && r !== EDGE && isTarget(r, tgt)) return ARRIVED;
      }
      return WAITING;
    }
    // the segment from (x, z) to (tx, tz) passes through the inside of rect r
    function crosses(x, z, tx, tz, r) {
      let t0 = 0, t1 = 1;
      const p = [x, z], d = [tx - x, tz - z];
      for (let k = 0; k < 2; k++) {
        const lo = r[k], hi = r[k + 2];
        if (d[k] === 0) { if (!(lo < p[k] && p[k] < hi)) return false; continue; }
        let a = (lo - p[k]) / d[k], b = (hi - p[k]) / d[k];
        if (a > b) { const c = a; a = b; b = c; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
      }
      return t0 < t1;
    }
    /* u's body can slide straight to the target's point past every still body but the target's own and those it
       overlaps already, and ends on the map: the segment against each body grown by u's half sides, which is exactly
       where the sliding box would overlap it */
    function clear(u, tgt, obs) {
      const now = body(u), ax = fp(u.type)[0] * CELL / 2, az = fp(u.type)[1] * CELL / 2;
      if (onMap(now) && !onMap(bodyAt(u, tgt.x, tgt.z))) return false;
      for (const b of obs) {
        if (!still(b) || isTarget(b, tgt) || hits(now, b.r)) continue;
        if (crosses(u.x, u.z, tgt.x, tgt.z, [b.r[0] - ax, b.r[1] - az, b.r[2] + ax, b.r[3] + az])) return false;
      }
      return true;
    }
    /* THE PLANNER: A* over the map's cells. Node (i, j) stands u with its body on cells i..i+fx-1, j..j+fz-1; it is
       free when no body covers it but those u overlaps already. A step to a side costs CELL, to a corner DIAG
       and needs both sides beside it free. The search starts from the nodes whose cells u's body touches already --
       one when u stands on a node, up to four between nodes -- each at its distance, and ends at the first node popped that meets the goal; failing one, at the popped node nearest the
       target, if it is nearer than u stands. The heap orders by f, then by push, so every correct heap pops the same
       nodes. The node to head for, or null: no nearer place (STUCK), which is also u standing on a goal node already. */
    function plan(u, tgt, obs) {
      const fx = fp(u.type)[0], fz = fp(u.type)[1], NW = W - fx + 1, NH = H - fz + 1;
      if (NW < 1 || NH < 1) return null;
      const now = body(u), occ = new Uint8Array(W * H), movers = [];
      for (const b of obs) {
        if (hits(now, b.r)) continue;
        if (b.u && mobile(b.u.type)) { movers.push(b.r); continue; }
        for (let j = Math.max(0, b.r[1] / CELL); j < Math.min(H, b.r[3] / CELL); j++)
          for (let i = Math.max(0, b.r[0] / CELL); i < Math.min(W, b.r[2] / CELL); i++) occ[j * W + i] = 1;
      }
      const N = NW * NH, g = new Float64Array(N).fill(Infinity), par = new Int32Array(N).fill(-1);
      const shut = new Uint8Array(N), known = new Uint8Array(N);  // known: 1 free, 2 blocked
      const cx = i => (i + fx / 2) * CELL, cz = j => (j + fz / 2) * CELL;
      const nodeBox = (i, j) => [i * CELL, j * CELL, (i + fx) * CELL, (j + fz) * CELL];
      function free(n) {
        if (!known[n]) {
          const i = n % NW, j = (n - i) / NW, a = nodeBox(i, j);
          let ok = true;
          for (let dj = 0; dj < fz && ok; dj++) for (let di = 0; di < fx && ok; di++) if (occ[(j + dj) * W + i + di]) ok = false;
          for (let k = 0; k < movers.length && ok; k++) if (hits(a, movers[k])) ok = false;
          known[n] = ok ? 1 : 2;
        }
        return known[n] === 1;
      }
      const h = (i, j) => Math.max(0, rectDist(cx(i), cz(j), tgt.r) - tgt.plan);
      const heap = [];
      let seq = 0;
      const less = (a, b) => a[0] < b[0] || (a[0] === b[0] && a[1] < b[1]);
      function push(f, n) {
        heap.push([f, seq++, n]);
        for (let k = heap.length - 1; k > 0;) {
          const p = (k - 1) >> 1;
          if (!less(heap[k], heap[p])) break;
          [heap[k], heap[p]] = [heap[p], heap[k]]; k = p;
        }
      }
      function pop() {
        const top = heap[0], last = heap.pop();
        if (heap.length) {
          heap[0] = last;
          for (let k = 0; ;) {
            const l = 2 * k + 1, r = l + 1;
            let m = k;
            if (l < heap.length && less(heap[l], heap[m])) m = l;
            if (r < heap.length && less(heap[r], heap[m])) m = r;
            if (m === k) break;
            [heap[k], heap[m]] = [heap[m], heap[k]]; k = m;
          }
        }
        return top[2];
      }
      const a = u.x / CELL - fx / 2, b = u.z / CELL - fz / 2, i0 = Math.floor(a), j0 = Math.floor(b);
      for (let j = j0; j <= Math.ceil(b); j++) for (let i = i0; i <= Math.ceil(a); i++) {
        if (i < 0 || j < 0 || i >= NW || j >= NH || !free(j * NW + i)) continue;
        const dx = cx(i) - u.x, dz = cz(j) - u.z, d = Math.sqrt(dx * dx + dz * dz);
        g[j * NW + i] = d;
        push(d + h(i, j), j * NW + i);
      }
      let end = -1, best = -1, bestD = 0;
      while (heap.length) {
        const n = pop();
        if (shut[n]) continue;
        shut[n] = 1;
        const i = n % NW, j = (n - i) / NW;
        if (inRange(cx(i), cz(j), nodeBox(i, j), tgt, tgt.plan)) { end = n; break; }
        const e = rectDist(cx(i), cz(j), tgt.r);
        if (best < 0 || e < bestD) { best = n; bestD = e; }
        for (const [di, dj] of DIRS) {
          const ni = i + di, nj = j + dj, m = nj * NW + ni;
          if (ni < 0 || nj < 0 || ni >= NW || nj >= NH || shut[m] || !free(m)) continue;
          if (di && dj && (!free(j * NW + ni) || !free(nj * NW + i))) continue;
          const c = g[n] + (di && dj ? DIAG : CELL);
          if (c < g[m]) { g[m] = c; par[m] = n; push(c + h(ni, nj), m); }
        }
      }
      if (end < 0) {
        if (best < 0 || !(bestD < rectDist(u.x, u.z, tgt.r))) return null;
        end = best;
      }
      // back to the seed; past it when u stands on it
      let n = end, next = -1;
      while (par[n] >= 0) { next = n; n = par[n]; }
      if (g[n] === 0) n = next;
      if (n < 0) return null;
      const i = n % NW;
      return { x: cx(i), z: cz((n - i) / NW) };
    }
    // u's leg is still its to walk: the same order and target, its node covered by no body u is not on already
    function legHolds(u, tgt, obs) {
      const L = u.leg;
      if (!L || L.order !== tgt.order || L.kind !== tgt.kind) return false;
      const now = body(u), a = bodyAt(u, L.x, L.z);
      for (const b of obs) if (hits(a, b.r) && !hits(now, b.r)) return false;
      return true;
    }
    // u's turn toward tgt: ARRIVED in range, else STEPPED, WAITING (its tries refused) or STUCK (no nearer place)
    function go(u, tgt) {
      if (inRange(u.x, u.z, body(u), tgt, tgt.stop)) { u.leg = null; return ARRIVED; }
      const obs = bodies(u), v = def(u.type).maxVelocity;
      /* the site is a body to the unit heading for it, started, claimed or not: its straight step stops at the site's
         edge and is ARRIVED, so a builder whose BuildDistance is shorter than its half width does not walk onto it
         (claim says how it then claims the site) */
      if (tgt.kind === 1) obs.push({ r: tgt.body, u: null });
      if (clear(u, tgt, obs)) {
        u.leg = null;
        const dx = tgt.x - u.x, dz = tgt.z - u.z, d = Math.sqrt(dx * dx + dz * dz);
        return step(u, dx / d, dz / d, Math.min(v, d - tgt.clip), obs, tgt);
      }
      if (!legHolds(u, tgt, obs)) {
        const w = plan(u, tgt, obs);
        if (!w) { u.leg = null; return STUCK; }
        u.leg = { x: w.x, z: w.z, order: tgt.order, kind: tgt.kind };
      }
      const L = u.leg, dx = L.x - u.x, dz = L.z - u.z, d = Math.sqrt(dx * dx + dz * dz);
      const r = d <= v ? step(u, dx / d, dz / d, d, obs, tgt, L.x, L.z) : step(u, dx / d, dz / d, v, obs, tgt);
      if (r !== STEPPED || u.turned || (u.x === L.x && u.z === L.z)) u.leg = null;
      return r;
    }
    /* A unit on a structure or a claim steps straight away from its centre (the first such in bodies' order), away
       along (0, 1) from the centre itself, and does nothing else on its turn; true when it was on one. A build waits
       for its site to be clear of units, so this is how the units on a claimed site leave it. */
    function evacuate(u) {
      const obs = bodies(u), now = body(u);
      for (const b of obs) {
        if ((b.u && mobile(b.u.type)) || !hits(now, b.r)) continue;
        const dx = u.x - (b.r[0] + b.r[2]) / 2, dz = u.z - (b.r[1] + b.r[3]) / 2, d = Math.sqrt(dx * dx + dz * dz);
        u.leg = null;
        if (d > 0) step(u, dx / d, dz / d, def(u.type).maxVelocity, obs, null);
        else step(u, 0, 1, def(u.type).maxVelocity, obs, null);
        return true;
      }
      return false;
    }
    // toward a footprint until within build distance of it; true once in range
    const reach = (o, u, t, ox, oz) => go(u, toSite(o, u, rectOf(t, ox, oz))) === ARRIVED;
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
    // the nano piece's point: QueryNanoPiece names the piece and the host gives its origin, turned by the unit's facing
    function nanoPoint(u) {
      const fixed = !mobile(u.type), cs = fixed ? -1 : -u.hz, sn = fixed ? 0 : u.hx;
      const q = u.cob ? COB.query(u.cob, 'QueryNanoPiece', [0, 0, 0, 0], facts()) : { r: 0 };
      const piece = q.r > 0 ? q.io[0] : 0;
      const o = u.cob ? COB.origin(u.cob, piece) : { turned: [0, 0, 0], base: [0, 0, 0] };
      return [
        (((fix16(u.x) + trunc32(cs * o.turned[0] + sn * o.turned[2])) | 0) + o.base[0]) | 0,
        (o.turned[1] + o.base[1]) | 0,
        (((fix16(u.z) - trunc32(-sn * o.turned[0] + cs * o.turned[2])) | 0) - o.base[2]) | 0,
      ];
    }
    // one reverse emission from box `target` to point `pt` (0x472200), its second burst due on the next tick
    function emitReverse(u, target, pt, t) {
      const e = { box: [target, sprayBox(pt, pt)], key: sprayKey(u.id, nemit++), due: t + 1 };
      if (!sprayBurst(e, t)) return;
      if (bursts.length < SPRAY_BURSTS) bursts.push(e); else partsCut = true;
    }
    /* A FEATURE'S RECLAIM SPRAYS from the feature's box to the nano piece, twice a call with the same arguments
       (0x404D3B, 0x404D4C): the box from the anchor cell's corner, FootprintX x FootprintZ cells wide and the feature's
       height (+0xFA) tall over the ground (0x404CC9..0x404D37), the stage's ground being 0 */
    function sprayFeature(u, f, t) {
      const pt = nanoPoint(u);
      const lo = [fix16(f.ox * CELL), 0, fix16(f.oz * CELL)], hi = [fix16((f.ox + f.fx) * CELL), fix16(f.height), fix16((f.oz + f.fz) * CELL)];
      const target = sprayBox(lo, hi);
      emitReverse(u, target, pt, t);
      emitReverse(u, target, pt, t);
    }
    function spray(u, t) {
      const sp = u.spray;
      if (sp.how === 'feature') { sprayFeature(u, sp.f, t); return; }
      const [fx, fz] = fp(sp.type);
      if (sp.how === 'reclaim') {
        if (u.sprayAt && u.sprayOn === sp.id && t - u.sprayAt < 2) return;
        u.sprayAt = t;
        u.sprayOn = sp.id;
      }
      const pt = nanoPoint(u);
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

    /* ---- THE GAME (WikiSim.game only; research/notes/ingame-wiki-replay-plan.md, decisions 2, 3, 7 and 18) ----
       The seats, their economy, the kills and the wrecks. A page demo builds none of it: every function below runs only
       when GAME, so no page demo's state or hash can change by it. */
    // the seats of the scene the game plays on, and each one's record (the engine's player record, named below)
    const SEATDATA = GAME ? sceneOf(entry, demo).seats : [];
    /* THE ECONOMY'S STATE, the engine's player record per seat (DISASSEMBLED from 0x401360, the economy pass; the scout
       batch 1 facts, research/notes/engine/): m, e stored (+0x98, +0x8C); bm, be the base storage (+0xE0, +0xDC), the
       seat's settings, which the seat setup 0x496EE0 floors at 200 (0x496F64..0x496F97) and the level start 0x465E30
       also stores as the current (0x465EF5..0x465F1A); mmax, emax the storage (+0xA8, +0xA4), which the player init
       zeroes (0x46478F, 0x464795) until the first pass (a seat's settings are SEAT_MIN or more, so the floor holds by
       data); mi, mo, ei, eo the last pass's income and spending (+0x9C,
       +0xA0, +0x90, +0x94); wm, we the wasted totals (+0xD4, +0xCC); units, the units alive, a nanoframe included
       (+0x144, incremented at each create 0x486187, 0x486322); lost, the commanders lost (+0x106); out, the count gone
       from above 0 to 0; cmdLost, the commander lost: +0x149 bit 0 cleared, so the base storage stops counting; kills and
       losses, the units it killed and lost (+0xFC, +0xFE; `kill`). */
    const econ = SEATDATA.map(z => ({ m: z.metal, e: z.energy, bm: z.metal, be: z.energy,
      mmax: 0, emax: 0, mi: 0, mo: 0, ei: 0, eo: 0, wm: 0, we: 0, units: 0, lost: 0, out: false, cmdLost: false, kills: 0, losses: 0 }));
    /* THE TWO MODES (decision 18): 'refuse', an authored script, stops `starved` at the first pass that cannot pay a
       grant; 'stall', a seat the player plays, carries an unpaid grant as the unit's debt, and the debt refuses that
       unit's requests until a pass pays it, as the engine's builds stall at zero. A fork sets it anew (sim.fork). */
    let STALL = GAME && econMode === 'stall';
    // badKill: a kill found no unit of its id; nkill: the kills made
    let starved = false, badKill = false, nkill = 0;
    /* THE DEATHS, the replay's death vision (tagpu_wikisim.h WIKIDEATH): each kill of a unit the engine's temporary
       sight record add would keep, {tick, owner, x, z, type} at the unit's place on the tick it is fed, in the order
       made, never removed (a seek rebuilds the game, and with it the list). The add's refusal is the engine's
       (tagpu_wikisim.c kill states it): a kill of a seat that already has PLAYERS_DEATHS deaths live at the add,
       tick + SIGHT_SPAN at or past the kill's tick - 1, is not recorded, whatever the script or a fork part kills.
       BOUND: at most FEATS_MAX, which checkGame's data rule holds a game's kills to, its script's and every fork
       part's; one past it is not recorded, in either twin. */
    const deaths = [];
    const liveDeaths = (owner, t) => deaths.filter(d => d.owner === owner && d.tick + SIGHT_SPAN + 1 >= t).length;
    // the seats' unit counts at the end of a step, after the work: `out` once a count goes from above 0 to 0
    function counts() {
      econ.forEach((z, k) => {
        const n = units.filter(u => u.owner === k).length;
        if (z.units > 0 && n === 0) z.out = true;
        z.units = n;
      });
    }
    /* THE BUILD'S REQUEST, 0x4011C0 on the BUILDER's ledger (0x41BA60 charges it at 0x41BC5A, `lea ecx,[ebp+0xbc]`,
       ebp the builder): the request is always added (0x4011D1..0x4011E3), and granted unless either debt is above 0
       (0x4011C0..0x4011F6, 0x4011F8..0x401212); the step applies its progress only on a grant (0x41BC65..0x41BC67) */
    function request(u, e, m) {
      const L = u.led;
      L.eR += e; L.mR += m;
      if (L.eD > 0 || L.mD > 0) return false;
      L.eG += e; L.mG += m;
      return true;
    }
    /* THE PASS, 0x401360, on ticks 1, 30, 60, ...: the level's init seeds the player's next pass +0xF0 with GameTime 0
       (0x46470C..0x464715), and the players phase runs a record's pass when +0xF0 <= GameTime and adds 30
       (0x46507D..0x465092), after GameTime's increment; every seat on the same tick, in seat order, after the units'
       work (research/notes/engine/sim.md, the players phase follows the unit tick). Per unit of the seat in slot order
       (0x401375..0x4013A6; here in creation order, below): a structure (+0x110 bit 29, 0x4013BA) that is active (+0x10E bit 0, 0x4013C5) requests its EnergyUse
       (0x4013ED..0x401421) or, negative, produces it (0x401429..0x401478), and an extractor whose energy was granted
       produces ExtractsMetal x its SetSpeed (0x401480..0x4014C8); a mobile unit active or moving (0x401617..0x401622)
       requests or produces its EnergyUse (0x401641..0x401668); a finished unit (+0x104 == 0.0, 0x4016C5..0x4016D6)
       produces its EnergyMake and MetalMake (0x4016DC..0x401795) and adds its storages (0x40179B..0x4017C5). Then the
       seat's sums: the storage with the base while the commander lives (0x401941, 0x401988..0x4019AA), income the
       produced and spending the REQUESTED (0x4019C6..0x401A2D); per resource the debt paid first, then the grants, from
       what is stored plus produced (0x401A4D..0x401AB1), and what passes the storage wasted (0x401AB3..0x401B27); and
       the settle (0x401B3D..0x401BAC), each unit's unpaid share of its grants and its debt its new debt, the rest of
       its ledger zeroed, the engine's fmul, fsubr and faddp in the engine's order. WHERE THE WIKI DEPARTS: activation
       is a finished unit whose FBI says ActivateWhenBuilt, which the engine sets at a completion (0x41B9A6) and at a
       finished create (0x48612F) [INFERRED: no cast unit's script changes its own], and moving is the step this tick
       (u.stepped) [INFERRED that it agrees with the move-rate class +0x110 & 0xC]; the units are walked in creation
       order, where the engine walks its slots, which the allocator reuses: the sums are the same terms added in
       another order, so only their rounding can differ; MakesMetal, wind, tidal, cloak, the AI's scaling, the
       transfers and the reclaim payouts are not modelled (no cast takes such a unit, no seat is an AI, the owner left
       the payouts out). The ledger is the engine's f32 as a double, as every figure here is. */
    function pass() {
      econ.forEach((z, k) => {
        let sm = 0, se = 0;
        const mine = units.filter(u => u.owner === k);
        for (const u of mine) {
          const d = def(u.type), L = u.led, eu = d.energyUse || 0;
          const active = u.built >= 1 && (d.activatewhenbuilt & 1) === 1;
          if (!mobile(u.type)) {
            if (active) {
              let granted = false;
              if (eu >= 0) { L.eR += eu; if (!(L.eD > 0)) { L.eG += eu; granted = true; } }
              else L.eP += -eu;
              if ((d.extracts || 0) > 0 && granted) L.mP += d.extracts * extractorSpeed(u);
            }
          } else if (active || u.stepped) {
            if (eu >= 0) { L.eR += eu; if (!(L.eD > 0)) L.eG += eu; }
            else L.eP += -eu;
          }
          if (u.built >= 1) {
            L.eP += d.energyMake || 0; L.mP += d.metalMake || 0;
            se += d.energyStorage || 0; sm += d.metalStorage || 0;
          }
        }
        let EP = 0, ER = 0, EG = 0, ED = 0, MP = 0, MR = 0, MG = 0, MD = 0;
        for (const u of mine) {
          const L = u.led;
          EP += L.eP; ER += L.eR; EG += L.eG; ED += L.eD; MP += L.mP; MR += L.mR; MG += L.mG; MD += L.mD;
        }
        z.emax = se + (z.cmdLost ? 0 : z.be);
        z.mmax = sm + (z.cmdLost ? 0 : z.bm);
        z.ei = EP; z.eo = ER; z.mi = MP; z.mo = MR;
        // one resource's settle: [the debt's fraction paid, the grants' fraction paid]
        const settle = (cur, P, D, G, max, waste) => {
          const A = cur + P;
          const fD = D <= A ? 1 : A / D, rest = A - (D <= A ? D : A);
          const fG = G <= rest ? 1 : rest / G;
          let c = rest - (G <= rest ? G : rest), w = 0;
          if (c > max) { w = c - max; c = max; }
          return [fD, fG, c, waste + w];
        };
        const [fDe, fGe, e, we] = settle(z.e, EP, ED, EG, z.emax, z.we);
        const [fDm, fGm, m, wm] = settle(z.m, MP, MD, MG, z.mmax, z.wm);
        z.e = e; z.we = we; z.m = m; z.wm = wm;
        for (const u of mine) {
          const L = u.led;
          L.eD = (L.eG - fGe * L.eG) + (L.eD - fDe * L.eD); L.eP = L.eR = L.eG = 0;
          L.mD = (L.mG - fGm * L.mG) + (L.mD - fDm * L.mD); L.mP = L.mR = L.mG = 0;
        }
        if (!STALL && (fDe < 1 || fGe < 1 || fDm < 1 || fGm < 1)) starved = true;
      });
    }
    /* THE WRECK of a finished unit killed (decision 3), as the destructor's corpse 0x486360(unit, level, smoke)
       (called at 0x486D6F) spawns it through 0x423C50 at the unit's cell +0x76/+0x78 (research/notes/engine/map.md):
       a mobile unit's footprint cell by the engine's own rounding (footCell), a structure's site. In 0x423C50's order:
       the anchor off the map, none (0x4863C1..0x4863CA); the wreck's footprint past the map's far edge, none, equal
       accepted (0x423CEC..0x423CF8, 0x423D20..0x423D24); every standing feature with a cell under it, met row by row,
       destroyed whole (FEATURES_Destroy 0x4246B0 from 0x423D3E..0x423D94) -- here it leaves `feats` and its anchor
       joins `gone`, as a reclaimed one does, so no two features ever overlap; units under it are not read, the wreck
       is put on top. The engine abandons a wreck at an indestructible feature (FeatureDef +0xFF bit 1), which no
       entry's feature is [INFERRED]. The wreck takes the unit's id, which no feature of the scene has (checkGame).
       BOUND: `feats` and `gone` never pass the scene's features plus the kills, which checkGame holds to FEATS_MAX. */
    function corpse(u) {
      const name = typeof def(u.type).corpse === 'string' && own(FD, def(u.type).corpse) ? def(u.type).corpse : null;
      if (name === null) return;
      const d = FD[name], [cfx, cfz] = d.footprint;
      const ox = mobile(u.type) ? footCell(u.x, fp(u.type)[0]) : u.ox, oz = mobile(u.type) ? footCell(u.z, fp(u.type)[1]) : u.oz;
      if (ox < 0 || oz < 0 || ox >= W || oz >= H || ox + cfx > W || oz + cfz > H) { log('nocorpse', u); return; }
      for (let z = oz; z < oz + cfz; z++) for (let x = ox; x < ox + cfx; x++) {
        const f = feats.find(g => x >= g.ox && x < g.ox + g.fx && z >= g.oz && z < g.oz + g.fz);
        if (f) { feats.splice(feats.indexOf(f), 1); gone.push([f.ox, f.oz]); }
      }
      feats.push({ id: u.id, name, ox, oz, fx: cfx, fz: cfz, x: (ox + cfx / 2) * CELL, z: (oz + cfz / 2) * CELL, metal: d.metal || 0,
        energy: d.energy || 0, height: d.height || 0, blocking: !!d.blocking, reclaimable: !!d.reclaimable, wreck: 'object' in d });
      log('corpse', u);
    }
    /* THE DEATH SOUND of a finished unit killed: the destructor explodes it through 0x49B000, which impacts its ExplodeAs
       weapon (def+0x220) at the unit with no victim through 0x499EB0, and on dry ground that plays the weapon's
       soundhit (+0xF6, 0x499FE9) through 0x47F300 (0x499FF2), not sent on the network and gated by the viewed
       player's sight (0x47F3E8..0x47F4A8), with no owner test (DISASSEMBLED, scout batch 2). One event, at once, for a
       unit of any seat: kind SND_DEATH, the type's index as its name and the cell under its centre, held to the map,
       as its variant; the gate is the player's (the viewer's sight is no state of the game's), and the event is made
       whether or not the type has a WAV, so the state does not depend on the load beyond the source being there. */
    function deathSound(u) {
      if (!SNDT) return;
      const cx = Math.min(Math.max(Math.floor(u.x / CELL), 0), W - 1), cz = Math.min(Math.max(Math.floor(u.z / CELL), 0), H - 1);
      soundEvent(SND_DEATH, defIndex.indexOf(u.type), -1, cz * W + cx);
    }
    /* THE UNIT LEAVES, a kill's or a reclaim's: the list, the sidebar's shown unit (route() clears it at the tick's end
       anyway) and its script */
    function removeUnit(u) {
      units = units.filter(v => v !== u);
      if (sb.shown === u) sb.shown = null;
      if (COB && u.cob) COB.remove(u.cob);
    }
    /* A KILL (decision 2, the game script's {at, kill}), fed before its tick's dropWork, COB slice and advance. A
       finished unit leaves its wreck and makes its death sound; an unfinished one neither (Send_UnitDeath stores corpse
       type 0 when +0x104 != 0.0, 0x4865D2..0x4865E5, and the destructor then skips both the explosion,
       0x486D2F..0x486D40, and the corpse, 0x486D55..0x486D5A). A commander counts as its seat's loss. A builder whose
       running build is the unfinished structure killed loses that order and says cant, as MobileBuild answers the
       destructor's death walk 0x489740 (event 8, 0x403A27) with "Construction terminated" (0x5015A4 through 0x47F780) and
       result 8; its queued builds of the site stay [INFERRED: only a running order holds the reference, linked at
       0x403D64]. A building it was making stays as it is; a guard of it and a reclaim of it end by their own rules.
       THE INVARIANT, an ordering: between two ticks the only references to a unit are `units` and the sidebar's shown
       unit -- `work` and `jobs` are made anew in each advance, and orders, the spray and the sounds name units by id --
       and a kill runs between advances, so removeUnit clearing both leaves nothing that reads it; its script goes with
       it, and a script's handle is never given again (spawn). */
    function kill(id, by) {
      const u = byId(id);
      if (!u) { badKill = true; return; }
      nkill++;
      /* THE KILLS AND THE LOSSES (PD6): the victim's seat loses it whatever killed it (+0xFE, ungated at 0x4868C1); `by`,
         a seat of the game other than the victim's, counts a kill when the victim was finished (+0xFC, behind a killer,
         +0x104 == 0 and another player; engine/memory.md). A `by` that is no seat of the game counts nothing. */
      if (econ[u.owner]) econ[u.owner].losses++;
      if (int(by) && by >= 0 && by < econ.length && by !== u.owner && u.built >= 1) econ[by].kills++;
      if (deaths.length < FEATS_MAX && liveDeaths(u.owner, feeding) < PLAYERS_DEATHS)
        deaths.push({ tick: feeding, owner: u.owner, x: u.x, z: u.z, type: u.type });
      if (u.built >= 1) { corpse(u); deathSound(u); }
      const z = econ[u.owner];
      if (z && def(u.type).cast === 'commander') { z.lost++; z.cmdLost = true; }
      if (!mobile(u.type) && u.built < 1)
        for (const v of units) {
          const o = v.orders[0];
          if (v !== u && o && o.k === 'build' && o.type === u.type && o.ox === u.ox && o.oz === u.oz) { v.orders.shift(); notice(v, SND.cant); }
        }
      removeUnit(u);
      log('killed', u);
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
        // a reclaim says working once an order, when its work first goes on (0x4048BE)
        if (how === 'reclaim' && u.orders[0] && u.orders[0] !== u.said) { u.said = u.orders[0]; notice(u, SND.working); }
        /* in a game, the build's request: its increment's part of the structure still unbuilt after this tick's
           builders before it, as the engine's step updates +0x104 call by call, times the built type's build costs
           (0x41BB36..0x41BB5C); refused, no progress and no spray. The wiki's increment is workerTime / 30 /
           buildTime where the engine's step takes floor(WorkerTime / 30), but the increments of a building sum to 1
           in both, so a building costs its build costs */
        if (GAME && how === 'build') {
          const dw = def(u.type).workerTime / def(s.type).buildTime / TPS * PACE, w0 = work.get(s), left = 1 - s.built - (w0 ? w0.build : 0);
          if (left > 0) {
            const d = dw < left ? dw : left;
            if (!request(u, d * (def(s.type).buildCostEnergy || 0), d * (def(s.type).buildCostMetal || 0))) return;
          }
        }
        const w = work.get(s) || { build: 0, reclaim: 0 };
        w[how] += def(u.type).workerTime / def(s.type).buildTime / TPS * PACE;
        work.set(s, w);
        // the step made progress: the unit's turn sprays at the structure as it stands now
        u.spray = { how, id: s.id, type: s.type, x: s.x, z: s.z };
      };
      /* THE RECLAIM HANDLER ON A FEATURE, 0x404AD0, DISASSEMBLED (research/notes/exe-reverse-engineering.md, *The
         reclaim sweep's engine side*): every call looks the feature up at the order's point, and with none says
         "Reclamation failed" (kind 7, 0x404AF8) and ends. The walk until the footprints touch; a walk that gives up ends
         the order silent (result 8). Arrived, the countdown trunc(15 + (energy + metal) / 2) (0x404BBA..0x404BDC) and
         StartBuilding; the stance wait (0x404C5A); the working reply once (0x404C6E); then a working call every 2 ticks
         (0x439E80(2) at 0x404C77) takes 2 off it, and above 15 sprays (0x404CB5). At 0 or below that call ends its state
         (result 1, 0x404C8B) with the next call armed 2 ticks on, and that call pays out and the feature goes (state 5,
         0x404D60: 0x4237D0, FeatureDie) -- after the entry's lookup, so a feature gone in between fails as above. The
         countdown, the next call's tick, the payout due and whether it paid are the unit's (u.rc). */
      const featWork = (u, o) => {
        const f = featById(o.feat), t = sim.tick + 1;
        if (!f) { u.orders.shift(); u.rc = null; notice(u, SND.cant); return; }
        if (!u.rc || u.rc.o !== o) {
          const r = go(u, toFeat(o, f));
          if (r === STUCK) { u.orders.shift(); return; }
          if (r !== ARRIVED) return;
          u.rc = { o, c: reclaimCountdown(f.energy, f.metal), next: t, due: false, paid: false };
        }
        const dx = f.x - u.x, dz = f.z - u.z, d = Math.sqrt(dx * dx + dz * dz);
        if (d > 0) { u.hx = dx / d; u.hz = dz / d; }
        jobs.set(u, { s: f, how: 'feature' });
        if (!(u.stanced && u.work === f)) { if (!stance(u)) return; u.stanced = true; }
        if (u.orders[0] !== u.said) { u.said = u.orders[0]; notice(u, SND.working); }
        if (t < u.rc.next) return;
        u.rc.next = t + 2;
        if (u.rc.due) {
          u.rc.paid = true;
          feats.splice(feats.indexOf(f), 1);
          gone.push([f.ox, f.oz]);
          log('reclaimed', { id: f.id, type: f.name, ox: f.ox, oz: f.oz });
          return;
        }
        u.rc.c -= 2;
        if (u.rc.c <= 0) u.rc.due = true;
        else if (u.rc.c > 15) u.spray = { how: 'feature', f };
      };
      // went: the unit stepped on the last tick, so this tick's planners walk round it only if it stood
      for (const u of units) { u.went = !!u.stepped; u.stepped = false; u.spray = null; u.evacuated = false; u.completed = false; }
      nemit = 0;
      for (const u of units) {
        if (!mobile(u.type)) continue;
        const o = u.orders[0];
        // the order's handler runs wherever the unit stands, so its ok comes before any step off a claim
        if (o) begin(u, o);
        if (evacuate(u)) { u.evacuated = true; continue; }
        if (!o || o.k === 'guard') continue;
        if (o.k === 'move') {
          const r = go(u, toPoint(o, o.x, o.z));
          if (r !== ARRIVED && r !== STUCK) continue;
          // Move_Ground says arrived on its goal test's event 0x20 alone (0x4031EB..0x4031FB); a give-up (0x40) ends the
          // order silent. A stuck move ends where the engine's mover would give up, unless it already stands in the
          // goal's cell
          const arrived = r === ARRIVED || inGoalCell(u, o.x, o.z);
          u.orders.shift();
          if (arrived) notice(u, SND.arrived);
          continue;
        }
        if (o.k === 'reclaim' && o.feat !== undefined) { featWork(u, o); continue; }
        if (o.k === 'reclaim') {
          const s = byId(o.target);
          if (!s) { u.orders.shift(); continue; }
          if (reach(o, u, s.type, s.ox, s.oz)) credit(u, s, 'reclaim');
          continue;
        }
        let s = structureAt(o.type, o.ox, o.oz);
        if (s && s.built >= 1) { u.orders.shift(); continue; }
        const r = go(u, toSite(o, u, rectOf(o.type, o.ox, o.oz)));
        if (r === STUCK && outOfReach(u, o.type, o.ox, o.oz)) {
          // "I can't reach the construction site" (0x403C45) ends the order; a give-up within reach goes on to the site
          // as an arrival does (0x403C59)
          u.orders.shift();
          notice(u, SND.cant);
          continue;
        }
        if (r !== ARRIVED && r !== STUCK) continue;
        if (!s) {
          if (!siteOk(o.type, o.ox, o.oz, null)) { u.orders.shift(); notice(u, SND.cant); log('refused', u, o.type); continue; }
          /* the site waits until no unit stands on it; those that do step off it, its claim (evacuate). MobileBuild's
             site test (0x47DB70 at 0x403CC7) fails on an occupied cell, and its first failure an order says cant
             ("Waiting for target area to clear", 0x403CDF) */
          const rect = rectOf(o.type, o.ox, o.oz);
          if (units.some(v => mobile(v.type) && hits(body(v), rect))) {
            if (u.cant !== o) { u.cant = o; notice(u, SND.cant); }
            continue;
          }
          s = place({ id: `u${nextId++}`, type: o.type, owner: u.owner, built: 0, reclaimed: 0, orders: [], sel: false, label: o.type }, o.ox, o.oz);
          notice(u, SND.build);
          log('started', s);
        }
        credit(u, s, 'build');
      }
      for (const u of units) {
        const o = u.orders[0];
        if (!o || o.k !== 'guard' || u.evacuated) continue;
        const g = byId(o.target);
        if (!g) { u.orders.shift(); continue; }
        // a guarded unit reclaiming a feature is followed: the wiki's guard helps only a structure's work
        const j = jobs.get(g);
        if (j && j.how !== 'feature') { if (reach(o, u, j.s.type, j.s.ox, j.s.oz)) credit(u, j.s, j.how); }
        else go(u, toUnit(o, g));
      }
      for (const [s, w] of work) {
        if (w.build && s.built < 1) {
          s.built = Math.min(1, s.built + w.build);
          s.completed = s.built >= 1;
          if (s.built >= 1) {
            log('finished', s);
            if (s.owner === 0) built++;
            // 0x41B8D0: a structure finished whose FBI says ActivateWhenBuilt is switched on (0x41B9A6)
            if (COB && s.cob && (def(s.type).activatewhenbuilt & 1) === 1) COB.activate(s.cob, 1);
          }
        }
        if (w.reclaim) {
          s.reclaimed = Math.min(1, s.reclaimed + w.reclaim);
          if (s.reclaimed >= 1) { removeUnit(s); log('reclaimed', s); }
        }
      }
      // THE NOTICES OF THE WORK, in creation order: a change of ACTIVATION the completion made, then each builder whose
      // build finished this tick (0x403F39, 0x40422F)
      for (const u of units) noticeActive(u);
      for (const u of units) {
        const j = jobs.get(u);
        if (j && j.how === 'build' && units.includes(j.s) && j.s.completed) notice(u, SND.complete);
      }
      for (const u of units) {  // orders whose work is over end now, so the next one starts on the next tick
        const o = u.orders[0];
        if (!o) continue;
        if (o.k === 'build') { const s = structureAt(o.type, o.ox, o.oz); if (s && s.built >= 1) u.orders.shift(); }
        // a feature's reclaim ends with its payout (result 5); another reclaimer of it finds it gone on its next call
        else if (o.k === 'reclaim' && o.feat !== undefined) { if (u.rc && u.rc.o === o && u.rc.paid) { u.orders.shift(); u.rc = null; } }
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
        const target = j && (j.how === 'feature' ? feats.includes(j.s) : units.includes(j.s) && !(j.how === 'build' && j.s.built >= 1)) ? j.s : null;
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
      // a run-now start or a query in the turns can set ACTIVATION too
      for (const u of units) noticeActive(u);
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
    // the resolved script, which a fork replaces (sim.fork)
    let script = scripted && demo ? resolve(demo, demo.inputs.map((e, i) => [e, i]).sort((a, b) => a[0].at - b[0].at || a[1] - b[1]).map(p => p[0]), IFACE) : [];
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
    /* THE DATA RULE OF A RECLAIM AND A PICK, refused here as tagpu_wikidata.c refuses it whatever plays the demo:
       `reclaim` is true and `pick` one of KINDS (checkTrips holds the trips' ticks with the sidebar on) */
    for (const e of script) {
      if ('reclaim' in e && e.reclaim !== true) throw new Error(`demo ${demo.id}: the reclaim at tick ${e.at} is not true`);
      if ('pick' in e && !KINDS.includes(e.pick)) throw new Error(`demo ${demo.id}: the pick at tick ${e.at} names no kind`);
    }
    /* THE DEMO'S CLOCK: the tick its inputs, its trips and its end are written in. It runs with the sim's own tick
       except while a wait holds it (wait: {until, max, from}, from the tick it began); a wait that runs past its max
       stops the demo, `stalled`. A Try-it sim has no script, so its clock is its tick. */
    let next = 0, clock = 0, wait = null, stalled = false;
    // the wait's state holds (UNTILS)
    const holds = w => w.until === 'built' ? built >= w.need : !units.some(u => u.owner === 0 &&
      u.orders.some(o => w.until === 'idle' ? o.k === 'build' || o.k === 'reclaim' : o.k === 'move'));
    /* a tick's generated inputs come before its data inputs; within a trip's tick a move comes before a press. The
       trips' inputs are seat 0's, which K is here; a script input acts as its own seat, which make and the fork held
       below NIN */
    const feed = t => {
      feeding = t;
      while (genNext < gen.length && gen[genNext].at <= t) { input(gen[genNext++].ev); IN[0].fed++; }
      while (next < script.length && script[next].at === t) {
        const e = script[next++];
        if ('until' in e) wait = { until: e.until, max: e.max, from: sim.tick, need: e.until === 'built' ? ++builtWaits : 0 };
        else if (GAME && 'kill' in e) kill(e.kill, e.by);
        // a seat's camera is recorded data the replay layer reads (seatCam); the simulation never sees it
        else if (GAME && 'camera' in e) {}
        else if (isSeat(e.seat ?? 0)) { const k = e.seat ?? 0; asSeat(k, () => input(e)); IN[k].fed++; }
      }
    };
    const end = scripted && demo && demo.end != null ? demo.end : Infinity;  // a Try-it sim is never done
    if (TRIPS) checkTrips(script, end);

    /* THE CURSOR'S SHAPE, each the cursor the game shows there (tagpu_wikisim.h, WIKI_CURSOR_*): the build-menu mode,
       else a selection holding a mobile unit of the player's, else none. Quick Build's ghost changes nothing here: it
       is drawn from the packet and leaves the engine's cursor as the selection has it. Off the map it is the arrow:
       the GUI shows cursornormal [INFERRED, capture C5]. */
    /* With reclaim armed it is cursorreclamate [INFERRED: the chooser's answer in mode 12 everywhere on the map], but
       cursornormal (the engine's "nothing", 0x13) over the popup and over a feature the resolver would reclaim that is
       not of the chosen kind (tagpu_sweep_cursor). */
    const reclaimCursor = () => {
      const c = mapCursor(), f = featAt(c.x, c.z);
      if (onPopup(popup())) return 'arrow';
      return st.kind !== 'all' && f && f.reclaimable && seen(Math.floor(f.x / CELL), Math.floor(f.z / CELL)) && !kindOk(f) ? 'arrow' : 'reclaim';
    };
    const cursorShape = () => over() !== 'map' ? 'arrow' : st.rmode ? reclaimCursor() : st.mode ? 'build' :
      selected().some(u => mobile(u.type)) ? 'move' : 'arrow';
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

    // a seat of the game's input seats; any other value is none (THE BOUND an index into IN rests on)
    const isSeat = k => int(k) && k >= 0 && k < NIN;
    /* the player's input, as its seat (`seat`, 0 when absent): one of no input seat is ignored */
    sim.input = ev => {
      const k = ev.seat ?? 0;
      if (!isSeat(k)) return;
      now = sim.tick;
      asSeat(k, () => { input(ev); dropWork(); cursorTick(sim.tick); });
    };
    sim.step = () => {
      const t = sim.tick + 1;
      now = t;
      if (wait && holds(wait)) wait = null;
      else if (wait && t - wait.from > wait.max) stalled = true;
      if (!wait) feed(++clock);
      dropWork();
      // the slice: every unit's DoScriptsNow(1), before its orders and its movement (0x48ADEB)
      if (COB) { COB.slice(facts()); for (const u of units) noticeActive(u); }
      advance();
      sprayTick(t);
      soundTick();
      // a game's counts after the work, then on ticks 1, 30, 60, ... its economy's pass
      if (GAME) {
        counts();
        if (t === 1 || t % TPS === 0) pass();
      }
      sim.tick = t;
      /* a lock ends the demo on its own tick, which then holds under the entry's badge; a game ends as well on a pass
         that starved and at a kill that found no unit */
      sim.done = clock >= end || sb.locked !== null || stalled || starved || badKill;
      if (ON) route();
      // the step's per-seat tail: each seat's hover and cursor shape over the world the tick left
      for (let k = 0; k < NIN; k++) asSeat(k, () => { st.hover = hover(); cursorTick(t); });
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
    /* what the acting seat's input shows: the ghost, the gesture's sites and shape, the spacing, the cursor, the keys
       held, the drag and the armed mode */
    const seatView = () => {
      // the gesture's sites replace the cursor's single square; none (an empty ring) leaves the square standing
      const p = placed() && st.place.sites.length ? st.place : null;
      const h = st.hover, gh = !p && h && (h.kind === 'place' ? h : h.kind === 'quick' ? { ...h, ok: true } : null);
      let n = 0;
      return {
        ghost: gh ? { ...centre(gh.type, gh.ox, gh.oz), ok: gh.ok } : null,
        // the gesture's preview: each site's centre and footprint, 'place' (green), 'refused' (red, skipped) or 'remove'
        // (yellow, takes a queued order off), and its place among the orders the release would add
        sites: p ? p.sites.map(s => ({ ...siteCentre(p.type, s), state: s.state, order: s.state === 'place' ? ++n : null })) : [],
        shape: p ? p.kind : null,
        spacing: st.mode ? st.spacing[st.mode] || 0 : null,
        // every change of the shape -- an input, a tick, the scene -- records it (cursorTick)
        cursor: { x: st.cursor.x, z: st.cursor.z, shape: st.cursorShape, since: st.cursorSince },
        keys: KEYS.filter(k => st.held.has(k)),
        drag: st.drag && st.drag.active ? { x0: st.drag.x0, z0: st.drag.z0, x1: mapCursor().x, z1: mapCursor().z } : null,
        mode: st.rmode ? 'reclaim' : st.mode ? `build:${st.mode}` : 'contextual',
      };
    };
    /* THE VIEW as `seat` sees it (0 when absent, and for a seat of no input state): its own input's parts (seatView),
       its order lines, its queued sites and the sweep it has armed; `units[].selected` is every seat's flag, the
       state hash's item 3, which a frame filters by owner. A game with more than one input seat adds the input seats'
       count, the viewed seat and seats 1..NIN - 1's own parts (`inputs`, each seatView with its reclaim armed, kind
       and ring), the state hash's item 13. */
    sim.view = (seat = 0) => asSeat(isSeat(seat) ? seat : 0, () => {
      const lines = [];
      for (const u of units) if (u.sel && u.owner === K) {
        let from = [u.x, u.z];
        for (const o of u.orders) {
          const t = orderPoint(o) || byId(o.target);
          if (!t) continue;
          lines.push({ from, to: [t.x, t.z], kind: o.k });
          from = [t.x, t.z];
        }
      }
      const owners = new Map();
      for (const u of units) for (const o of u.orders) if (o.k === 'build') owners.set(`${o.type}@${o.ox},${o.oz}`, u.owner);
      /* THE SWEEP as tagpu_sweep_fill publishes it: while reclaim is armed, the kind; during a drag past the threshold
         the ring, its targets and the readout's counts and sums (a feature with metal counts as a wreck, one with
         energy alone as a tree); otherwise the feature under the pointer the kind takes, bracketed; and the popup
         with the row under the pointer, -1 for none */
      let reclaim = null;
      if (st.rmode) {
        const P = popup(), R = st.rg && st.rg.drag ? ringOf(st.rg) : null, T = R ? ringTargets(R) : [];
        const c = mapCursor(), hf = !st.rg && over() === 'map' && !onPopup(P) ? featAt(c.x, c.z) : null;
        reclaim = {
          kind: st.kind, ring: R, targets: T.map(f => f.id),
          wrecks: T.filter(f => f.metal > 0).length, trees: T.filter(f => !(f.metal > 0) && f.energy > 0).length,
          metal: T.reduce((a, f) => a + f.metal, 0), energy: T.reduce((a, f) => a + f.energy, 0),
          hover: hf && takes(hf) ? hf.id : null,
          popup: P ? { ...P, hot: onPopup(P) ? P.rows.findIndex(q => inRect(q, st.cursor.x, st.cursor.z)) : -1 } : null,
        };
      }
      return {
        map: { w: W, h: H, cell: CELL, surfaceMetal: SM, metal: metal.slice(), fog: fog.slice(), vents: vents.map(v => ({ x: (v.cx + 0.5) * CELL, z: (v.cz + 0.5) * CELL })) },
        units: units.map(u => ({ id: u.id, type: u.type, owner: u.owner, x: u.x, z: u.z, hx: u.hx, hz: u.hz, fx: fp(u.type)[0], fz: fp(u.type)[1],
          selected: u.sel, built: u.built, reclaimed: u.reclaimed, label: u.label })),
        ...seatView(),
        queued: plans().map(p => ({ ...centre(p.type, p.ox, p.oz), owner: owners.get(`${p.type}@${p.ox},${p.oz}`) })),
        lines,
        caption,
        pace: PACE,
        // the features still standing, each by its id, its section name and its footprint at its anchor cell
        feats: feats.map(f => ({ id: f.id, name: f.name, ox: f.ox, oz: f.oz, fx: f.fx, fz: f.fz, metal: f.metal, energy: f.energy,
          wreck: f.wreck })),
        gone: gone.map(c => [c[0], c[1]]),
        reclaim,
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
        /* the sound, null without a source: each kind's next time, the last reply's time, the queue (each [kind, tick,
           unit id, type, priority], highest priority first), the events made, the last min(total, SND_LOG) events
           oldest first, each [tick, kind, name, category, variant] (tagpu_wikisim.h, WIKISNDEV), and each unit's 1 when
           its first order's taken site has said cant */
        sound: SNDT ? {
          next: sndNext.slice(), last: sndLast, queue: sndQ.map(e => [e.kind, e.tick, e.unit, e.type, e.priority]),
          total: sndTotal, events: Array.from({ length: Math.min(sndTotal, SND_LOG) },
            (_, i) => sndRing[(sndTotal - Math.min(sndTotal, SND_LOG) + i) % SND_LOG].slice()),
          cant: units.map(u => u.orders[0] && u.cant === u.orders[0] ? 1 : 0),
        } : null,
        /* a game's own part, only in a game (tagpu_wikisim.h, item 12 of the hash): each seat's record (its scene's
           name, then the economy's state), the economy's mode, whether a pass starved and a kill found no unit, the
           kills made, and each unit's ledger [eP, eR, eG, eD, mP, mR, mG, mD], index-aligned with `units` */
        ...(GAME ? { game: {
          seats: econ.map((z, k) => ({ name: SEATDATA[k].name, ...z })), mode: STALL ? 'stall' : 'refuse', starved, badKill, nkill,
          ledgers: units.map(u => { const L = u.led; return [L.eP, L.eR, L.eG, L.eD, L.mP, L.mR, L.mG, L.mD]; }),
          // each input seat's cursor at its map cursor, `on` once it has moved and while it is over the map
          cursors: IN.map((I, k) => asSeat(k, () => { const c = mapCursor(); return { x: c.x, z: c.z, on: I.moved && over() === 'map' }; })),
          clicks: clicks.map(c => ({ ...c })),
          deaths: deaths.map(d => ({ ...d })),
          // each type's sight figures (sightLit, sightContacts), in the bound entry's order
          types: defIndex.map(t => ({ name: t, sightDistance: D[t].sightDistance ?? 0, radarDistance: D[t].radarDistance ?? 0,
            sonarDistance: D[t].sonarDistance ?? 0, activates: (D[t].activatewhenbuilt & 1) === 1, modelHeight: D[t].modelHeight ?? 0 })),
        } } : {}),
        // the input seats beyond seat 0, only in a game with more than one (tagpu_wikisim.h, item 13 of the hash)
        ...(GAME && NIN > 1 ? { nin: NIN, viewed, inputs: IN.slice(1).map((_, i) => asSeat(i + 1, () => ({ ...seatView(),
          reclaim: st.rmode ? { kind: st.kind, ring: st.rg && st.rg.drag ? ringOf(st.rg) : null } : null }))) } : {}),
      };
    });
    // whether it is a game (WikiSim.game), whose view and hash carry the game's part
    sim.game = GAME;
    // the bound entry's type names by index, which a death event names its type by (sound.js drain)
    sim.types = defIndex.slice();
    // frees what the sim holds outside itself: its scripts' world in the COB host
    sim.free = () => { if (COB) COB.free(); };
    /* a game's state between ticks, for the replay layer (replay.js) without a whole view: whether it stopped (a lock,
       a pass that starved, a kill that found no unit), the sound events made since it was made, each seat's units,
       commanders lost, whether it is out, its kills and losses, its stored values, storage and last pass's rates (`bar`,
       which a players replay's top bar eases toward under Sight), its metal income, energy income and metal wasted, and
       its army value (army); and of `seat` (0 when absent, and for a seat of no input state) the inputs fed, the stored
       values, storage and last pass's rates, which the live mode's top bar eases and samples (tagpu_wikisim.h WIKIECON's
       m, e, mmax, emax, ei, eo, mi, mo), and the keys and buttons it holds, in KEYS order */
    const barOf = z => ({ e: z.e, m: z.m, emax: z.emax, mmax: z.mmax, ei: z.ei, eo: z.eo, mi: z.mi, mo: z.mo });
    /* THE ARMY VALUE of seat k (tagpu_wikisim_army; tagpu_replay_army_values, tagpu_hudmarks.c sample): the metal cost
       of each of its live, finished units of a mobile type that is not its commander -- the game values a type whose
       BMcode +0x22F is 1 and that is not a commander (+0x245 bit 0x40000) at its metal cost +0x18A, every other at 0 --
       summed in f32 in the units' order, as the game sums its floats */
    function army(k) {
      let a = 0;
      for (const u of units)
        if (u.owner === k && u.built >= 1 && mobile(u.type) && def(u.type).cast !== 'commander')
          a = Math.fround(a + Math.fround(def(u.type).buildCostMetal || 0));
      return a;
    }
    if (GAME) sim.gameNow = (seat = 0) => {
      const k = isSeat(seat) ? seat : 0, z = econ[k];
      return { stopped: sb.locked !== null || starved || badKill, sound: sndTotal,
        seats: econ.map((e, j) => ({ units: e.units, lost: e.lost, out: e.out, kills: e.kills, losses: e.losses, bar: barOf(e),
          mi: e.mi, ei: e.ei, wm: e.wm, army: army(j) })), fed: IN[k].fed,
        bar: barOf(z), keys: KEYS.filter(b => IN[k].held.has(b)) };
    };
    /* THE RELEASE (tagpu_wikisim_release), between two ticks: what the game's rebuild leaves of seat k's input after
       Back to now and in a fork, MEASURED (wiki-replay m0-facts.md, 2026-11-01: a fork taken over at 1201 and a return
       to now, each from the commander selected and a building armed, came up with no unit selected, main+0x2CC3 back
       to 1 and the side's MAIN2 shown). The seat's held keys and buttons, a drag and a placement or reclaim gesture
       are let go without acting [INFERRED: they are the hardware's, which no recording carries]; not input({up}),
       since a release acts (a box select, a placement) and would act on the player's behalf. The seat's selection
       empties and its armed building or reclaim goes; its spacing and the sweep's kind, settings rather than what the
       measurement read, stay. Every other seat's input is untouched.

       Seat 0's release also lets go of the column's gadget capture, routes the column to MAIN2, and drops every
       generated input not yet fed: the rest of a build or reclaim trip the release cuts off (a live entry has no
       pick). The column, its capture and its trips are seat 0's (THE COLUMN IS SEAT 0'S), so another seat's release
       has none of them to drop. The trip was planned from the selection the release empties, so what is left of it
       would press the column MAIN2 now shows; dropped, the release costs the game at most the one order in hand, a
       trip it cuts off or a building or reclaim armed and not yet used, as a player whose click is cut off loses their
       selection and the half-made order with it. This is the one place a timeline's trips are dropped: the fork drops
       them by calling this. A data input is never dropped, and THE LIVE DATA RULE (checkLiveOrders) puts the select of
       a build's builder and of a reclaim's reclaimer on the order's own tick and seat, which a release, between two
       ticks, cannot split, and keeps out of a live entry the pick, whose popup a release closes. A seat of no input
       state has nothing to let go. */
    function letGo(k = 0) {
      if (!isSeat(k)) return;
      asSeat(k, () => {
        if (k === 0) { genNext = gen.length; sb.cap = null; }
        st.held = new Set(); st.downOn = {}; st.drag = null; st.gesture = null; st.place = null; st.shiftPlaced = false; st.rg = null;
        deselect();
        st.mode = null; st.rmode = false;
        if (ON && k === 0) route();
        st.hover = hover();
        cursorTick(sim.tick);
      });
    }
    sim.release = letGo;
    /* THE FORK (research/notes/ingame-wiki-replay-plan.md, decision 8; tagpu_wikisim_fork): the game handed over at R
       to `seat` (0 when absent), the world of R - 1 kept whole, and `inputs` (a fork's part: game inputs of ticks
       R..end, any seat's) its script with the other seats' recorded inputs, its economy in `mode` from here on.
       Refused (it throws, the sim untouched) unless the game stands at R - 1 and has not stopped, `mode` is refuse or
       stall, the seat is one of the input seats, every input is at R or later, neither a caption nor a wait and of an
       input seat, the features standing and gone plus the part's kills stay within FEATS_MAX (THE BOUND the wrecks'
       table rests on), and the new script keeps the held-button rule seat by seat -- the taken seat's hand from
       nothing, every other seat's from the buttons it holds at R - 1, which its carried inputs continue -- and the
       trips' data rule.

       THE ORDERING: called between two ticks with the simulation at R - 1; feed(t) feeds exactly the inputs of tick
       <= t, so every input not yet fed has its tick >= R (one of a tick no feed reaches, below R, is dropped with it).
       Of those the fork drops the taken seat's, the recorded future it replaces, and every kill, since a death after R
       is not fated (decision 8); every other seat's are carried, so they play on from their recorded inputs. The new
       script is the carried inputs and the part's, merged by tick, the carried first on a tie, each in its own order.
       The release lets go of the taken seat alone, as the game's takeover leaves its player (tk_live: every unit
       deselected), and with seat 0 it drops the trips' inputs not yet fed. It comes before the new script is
       installed: an authored part begins with the selection it needs, on the tick of each build or reclaim it makes
       (THE LIVE DATA RULE, checkLiveOrders). The fork then views the taken seat, as the game's takeover does. With
       seat 0 and a script of seat 0 alone, nothing is carried, and the fork is the part alone. */
    if (GAME) sim.fork = (R, inputs, mode, seat = 0) => {
      const at = `demo ${demo.id}: the fork at tick ${R}`;
      if (sim.tick !== R - 1) throw new Error(`${at}: the game stands at tick ${sim.tick}`);
      if (sb.locked !== null || starved || badKill) throw new Error(`${at}: the game has stopped`);
      if (mode !== 'refuse' && mode !== 'stall') throw new Error(`${at}: the economy's mode ${mode} is not refuse or stall`);
      if (!isSeat(seat)) throw new Error(`${at}: seat ${seat} is not one of the game's ${NIN} input seats`);
      if (!Array.isArray(inputs)) throw new Error(`${at}: the part's inputs are not a list`);
      let kills = 0;
      for (const e of inputs) {
        if (!obj(e) || !(e.at >= R)) throw new Error(`${at}: an input of the part is before it`);
        if ('caption' in e || 'until' in e) throw new Error(`${at}: a game input is a caption or a wait`);
        if ('kill' in e) kills++;
        else if (!isSeat(e.seat ?? 0)) throw new Error(`${at}: an input of the part is of seat ${e.seat}, no input seat of the game`);
      }
      if (feats.length + gone.length + kills > FEATS_MAX) throw new Error(`${at}: the features and the kills pass ${FEATS_MAX}`);
      // the recorded future split: the other seats' inputs carried, the taken seat's and every kill dropped
      const carried = script.slice(next).filter(e => e.at >= R && !('kill' in e) && (e.seat ?? 0) !== seat);
      // the part as gameDemo and the binder make a game script, the builds the bound types, in its own order
      const raw = { id: demo.id, inputs };
      const part = (B ? B.demo(raw) : raw).inputs.map((e, i) => [e, i]).sort((a, b) => a[0].at - b[0].at || a[1] - b[1]).map(p => p[0]);
      const merged = [];
      for (let c = 0, q = 0; c < carried.length || q < part.length;)
        merged.push(q === part.length || (c < carried.length && carried[c].at <= part[q].at) ? carried[c++] : part[q++]);
      const fresh = resolve(demo, merged, IFACE, IN.map((I, k) => k === seat ? [] : ['lmb', 'rmb'].filter(b => I.held.has(b))));
      if (TRIPS) checkTrips(fresh, end);
      letGo(seat);
      script = fresh;
      next = 0;
      STALL = mode === 'stall';
      viewed = seat;
      sim.done = clock >= end || sb.locked !== null || stalled || starved || badKill;
    };

    if (ON) route();
    if (lock0) lock(lock0.type);
    // a game's seats counted as the scene stands, then again after the script's tick 0, which may kill
    if (GAME) counts();
    feed(0);
    if (GAME) counts();
    sim.done = sb.locked !== null || badKill;
    for (let k = 0; k < NIN; k++) asSeat(k, () => { st.hover = hover(); st.cursorShape = cursorShape(); st.cursorSince = 0; });
    return sim;
  }

  // a layout is checked whole before any sim reads it; null or absent is no sidebar: the map alone on the stage,
  // with the placement rules (arm keeping the Shift bit and a same-type gesture, placed) as with one
  const checked = layout => layout == null ? null : checkLayout(layout);
  return {
    create: (entry, demo, layout, ctx) => make(entry, demo, false, checked(layout), ctx),
    play: (entry, demo, layout, ctx) => make(entry, demo, true, checked(layout), ctx),
    // a game entry's demo's game script, its economy in `mode` 'refuse' (an authored script) or 'stall' (a seat the
    // player plays)
    game: (entry, demo, layout, ctx, mode = 'refuse') => make(entry, demo, 'game', checked(layout), ctx, mode),
    // the bounds, for the caller that falls back to the fixture on a refused layout
    checkLayout,
    // the casting's bounds, and the bound entry (its titles name the casts' units for the text's role words)
    checkCasting, bindEntry, CASTS,
    // the scene a demo plays on (null: the first), refused as tagpu_wikidata_entry refuses the entry
    sceneOf,
    // a text entry's refusal (null: none), as tagpu_wikidata_entry's
    checkText,
    // the button an action presses at an Interface Type, for the keys strip and the captions' button words
    button,
    // the trip's fixed budget and its table, which tagpu_wikisim.h #defines and simparity compares
    TRIP, TRIP_DOWN, TRIP_UP, TRIP_ROWS, PICK, PICK_DOWN, PICK_UP, KINDS,
    // the allsound names a named sound's `name` indexes (tagpu_wikisim.h WIKI_SNDNAME_*)
    SND_NAMES,
    // a wait's states, tagpu_wikidata.h's WIKI_UNTIL_* from 1
    UNTILS,
    // a game's bounds (tagpu_wikidata.h WIKI_SEATS, WIKI_GAME_KILLS, WIKI_GAME_END_MAX, and the
    // features and kills of WIKI_FEATS_MAX) and its death sound's kind
    SEATS, GAME_KILLS, GAME_END_MAX, FEATS_MAX, SND_DEATH,
    // the game frame's 4:3 bound on a game entry's map (checkGame), and a players entry's on its screen
    frameFits, screenFits,
    // the viewer's script's controls (tagpu_wikidata.h WIKI_HUD_*, in its order), a replay entry's and a live entry's,
    // and a press's trip, for replay.js; the stats window's chips (WIKI_CHIP_*)
    HUD_CONTROLS, REPLAY_CONTROLS, LIVE_CONTROLS, TAKEOVER_CONTROLS, PLAYERS_CONTROLS, CHIPS, HUD_DOWN, HUD_UP,
    // a players entry's seat's camera at a tick (tagpu_wikidata_seat_cam), its deaths' bound and the sight floor
    seatCam, PLAYERS_DEATHS, PLAYERS_DEATH_SPAN, SIGHT_MIN,
    // a layout's sight table's bounds (tagpu_wikidata.h WIKI_SIGHT_*), and the table a sight stamps with
    SIGHT_NUMTABLES, SIGHT_IDX, SIGHT_REACH, SIGHT_CELLS, sightIndex, sightTable,
    // a seat's lit grid and contacts in a game's view (tagpu_wikisim_sight_lit, tagpu_wikisim_contacts), and the
    // clicks the view keeps
    sightLit, sightContacts, SIGHT_SPAN, CLICKS,
    // a live demo's fork parts, at most, and the bound of their ids (the recorder's RECR_TIMELINES)
    DEMO_FORKS, TIMELINES,
    // the recorder's rules (THE RECORDER'S RULES), which replay.js plans its forks with
    REFRESH_TICKS, FORK_LIMIT, refreshBin, isRefresh, refreshAt,
    // the keys and buttons an input names, in tagpu_wikidata.h's WIKI_KEY_* order
    KEYS,
    // the shapes alone, for checks against tagpu_placegeo.c
    shapes: { placeShape, placeRing },
  };
})();
if (typeof module !== 'undefined') module.exports = WikiSim;
