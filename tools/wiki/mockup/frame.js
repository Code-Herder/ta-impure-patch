/* frame.js -- the whole game frame on a frame:game entry's stage: the watcher's view of the replay's game, the frame
   record the stage HUD is drawn from, the viewer's cursor among the HUD's controls, Leave's question and Try it
   yourself's presses (research/notes/ingame-wiki-replay-plan.md, decisions 6 and 20-24). It consumes replay.js's view
   and holds no state of its own but the press machine's. The DLL's tagpu_wikiframe.c is to be its port, function for
   function, and tools/wiki/framecheck.mjs is to hold the two equal.

   THE FRAME is a game screen of the map's own size: Wg = 128 + 16 map_w and Hg = max(16 map_h + 64, the column's rows)
   game pixels, the column at x 0..127, the top bar on rows 0..31 and the bottom bar on Hg-32..Hg-1 right of it, and the
   map at the viewport's top left (the camera at 0, 0 at zoom 1). stage.js draws it (frame mode).

   THE GLIDE runs on the viewer's clock. A trip's `at` is the clock tick its input was fed on (replay.js feed), and the
   clock is not the demo tick: it holds while the walker plays the script ahead of the replay (8 steps for an 1800-tick
   game) and while the `seeked` wait holds it. The trip's glide step is therefore clock - at, and its drag position
   clock - at - HUD_DOWN, which put the cursor on the control on the very clock tick the press acts; the demo tick
   would put it there 8 steps early. The toast's `at` is a demo tick (replay.js act stamps R.tick), so the toast's age
   is the demo tick's. Both clocks are in the replay's view, so record() takes neither apart.

   LIVE (a live entry, replay.js's LIVE): the frame is the player's own screen, not a watcher's. The rewind idle or
   open shows the live simulation as the game shows it (its fog, column, selection and gestures); the past shows the
   past simulation's world and fog under the column shown at the open, with nothing selected and no armed mode (view,
   MEASURED by M0, rewind-spec.md 9.1). The HUD is the live one (the rewind button and its strip); the top bar shows
   the replay's eased memo of seat 0's numbers; the wiki's emblem takes the slot left of the rewind button. THE ONE
   CURSOR: a demo's viewer and seat 0 are one hand, so while the rewind is idle and no trip or return glide runs, the
   cursor drawn is the simulation's own (`simcur`), its shape and animation at its world point; a trip that starts
   then starts from it (`@game`), and a press that leaves the rewind idle glides back to it.

   TAKE OVER (a takeover entry, replay.js's TK). Watching, the frame is the watcher's and its HUD the replay HUD with
   the Take over set: the picker (its rows dim by the game's rule, a seat with no unit, tagpu_hudstate.c; the Take over
   button down from its click until the picker closes, tagpu_hudlay.c `takeover`), the Forks bay, the lanes, the
   eviction's warning and its outlined lane, drawn from the record's fork block. Taken over, the frame is a live one,
   Rewind's, with seat h, the seat taken, wherever Rewind reads seat 0: its view, selection, cursor, the badge's colour
   and the top bar's numbers. THE TOP BAR'S SIDE (decision 15 as M0-T #2 MEASURED it, a CORE seat taken on an ARM load):
   the top bar -- its background, labels, font and the badge's place -- takes seat h's side's art (`top_side`), while
   the column, the minimap, PANELSIDE and the bottom bar stay the load's (`side`); every other record's top_side is its
   side. A takeover entry has no sidebar (ingame-wiki-replay-plan.md, entry 3), so the column is the load's idle MAIN2
   in either mode, as the game draws it after a takeover of either side (MEASURED, M0-T #1d).

   PLAYERS (a players entry, replay.js's PL; players-spec.md section 7). THE SCREEN is the entry's `screen` (sw, sh
   cells), not the map: Wg = 128 + 16 sw, Hg = max(16 sh + 64, the column's rows), and the viewport, the world
   between the column and the bars, Wv = 16 sw by Hv = Hg - 64 world units. THE EYE, the viewport's top left in the
   world, is the replay's camera (a centre) less half the viewport, held to [0, mapW - Wv] x [0, mapH - Hv]: the
   engine's eye clamp 0x41C3C0 (engine/input.md), its extent the map's -- not Impure's half-off range, since the stage
   draws no ground past the map. SIGHT is presentation (PD5): under Sight the frame's view takes the viewed seat's lit
   grid at the shown tick for the map's fog and the units drawn, and the replay's last pass for the minimap's fog and
   dots; the top bar is the replay's memo in the viewed seat's side's art, its badge that seat's (MEASURED, m0p-facts.md
   P2: CORE's badge, red, under Sight on seat 1). The record carries the players block and the stats window's series,
   which hudState turns into the stage HUD's Players set and stats window.

   THE OVERLAYS (OQ-B; tagpu_replay.c THE OVERLAYS, tagpu_mark.c gather_overlay) are drawn in every replay view the
   watcher sees: the watcher's view of Watching, of Take over's watching and of a players replay -- every recorded
   seat's cursor, clicks and selection, whatever Sight shows (the recorder's owner answer Q5) -- and the past of
   Rewind and Take over, the human seat's (MEASURED, m0p-facts.md P4: a rewind's past drew the recorded commander's
   selection rectangle and the recorded cursor's arrow in seat 0's colour, and the switches removed each). Each is
   switched by the replay's overlay switches, all on outside a players replay, whose Players set alone presses them.

   Runs in a page (the global WikiFrame) and under node (module.exports), as replay.js does. */
const WikiFrame = (() => {
  const Replay = typeof WikiReplay !== 'undefined' ? WikiReplay : require('./replay.js');
  const Sim = typeof WikiSim !== 'undefined' ? WikiSim : require('./sim.js');
  const TICK = 30;                    // game ticks a second
  const SIDE_W = 128, BAR = 32;       // the engine's column width and bar height, game pixels
  const HUD_H = 720;                  // the HUD unit is the frame's height / 720 at 100 % (decision 21)
  const TOAST_TICKS = 54;             // a toast shows 1.8 s of demo ticks (sim spec 4.7, the host's rule)
  // the marker kinds, tagpu_hudlay.h HUD_MK_*, by hud.js's names
  const MK_BOOKMARK = 2, MK_NAMES = ['death', 'defeat', 'bookmark'];
  // a seat's colour when the pack has no logos: the stage's stand-ins for TA's player colours, by owner
  const COLOURS = ['#2348e0', '#d32b00', '#f4f4f4', '#2f8a1b'];
  /* YESORNO.GUI, the engine's Leave question (retail totala1.hpi, MEASURED with tools/wiki/tavfs.py, 2026-10-31): the
     panel 400 x 100 at (112, 118) of the 640 x 480 GUI space, its title at (25, 25) 350 x 20, Yes at (80, 55) and No at
     (225, 55), each 95 x 20. Its place on a larger screen is [INFERRED]: the GUI space centred. */
  const YESORNO = { x: 112, y: 118, w: 400, h: 100, title: [25, 25, 350, 20], yes: [80, 55, 95, 20], no: [225, 55, 95, 20] };
  /* THE OVERLAY SWITCHES' bits (tagpu_hudlay.h HUD_TOG_*, replay.js TOG), all on outside a players replay; a click
     shows REC_CLICK_SHOW ticks (tagpu_replay.c) */
  const TOG = { cursors: 2, clicks: 4, selections: 8, chat: 16 }, CLICK_SHOW = 15;
  /* THE STATS WINDOW's measures by the record's index (replay.js MEASURES, layout.json window.measures), and its place
     and the team groups' colours where hudState is given no HUD layout: layout.json's window.x, window.y and
     chart.teams, which a page and a check pass from the file itself */
  const MEASURES = Replay.MEASURES, WINDOW = { x: 150, y: 56 }, TEAM_COLOURS = ['#e0c060', '#60b8e0', '#e07878', '#80d070', '#b090e0'];
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const snap = v => Math.floor(v + 0.5 + 1 / 64);
  // the simulation's cursor shapes by their index, tagpu_wikisim.h WIKI_CURSOR_*; a glide's point at seat 0's cursor
  const SHAPES = ['arrow', 'move', 'build', 'reclaim'], AT_GAME = Replay.AT_GAME;
  // the frame record's rewind state and arm by index, tagpu_wikireplay.h WIKI_RW_* and WIKI_ARM_*
  const RW = Replay.RW, ARMS = Replay.ARMS;
  const F32 = Math.fround;
  /* THE FORKS THE STAGE HUD LISTS (tagpu_wikiframe.h WIKIFRAME_FORKS, the recorder's fork limit) and their flags, the
     packet's TAGPU_PK_FORK_*: the one watched, a member of the watched chain, the Delete armed while the lanes are open */
  const FORKS = Replay.FORK_LIMIT, WATCHED = 1, CHAIN = 2, ARMED = 4;
  /* THE INDEXED CONTROLS of the Take over set and the Players set, as tagpu_hudlay.c keys them: a picker row `takeas:<row>` (a row is its
     seat: the record lists the seats in order), a lane's `lane:<k>`, `export:<k>` and `delete:<k>`, k its place in the
     record's fork list; and the Players set's: a hovered row's `jump:<seat>`, `sight:<seat>` and `follow:<seat>`, a
     team header's `fold:<team>` and a stats chip's `chip:<name>`, whose n is its index in sim.js CHIPS (the replay's
     press value). { act, n } for such a key, else null. LANE: the controls on a lane, which name a fork by its place. */
  const IX = { takeas: Sim.SEATS, lane: FORKS, export: FORKS, delete: FORKS, jump: Sim.SEATS, sight: Sim.SEATS,
    follow: Sim.SEATS, fold: Sim.SEATS };
  const LANE = ['lane', 'export', 'delete'];
  function indexed(key) {
    const c = /^chip:([a-z]+)$/.exec(key || '');
    if (c) return Sim.CHIPS.includes(c[1]) ? { act: 'chip', n: Sim.CHIPS.indexOf(c[1]) } : null;
    const m = /^(takeas|lane|export|delete|jump|sight|follow|fold):([0-9])$/.exec(key || '');
    return m && +m[2] < IX[m[1]] ? { act: m[1], n: +m[2] } : null;
  }
  // a replay control's key or an indexed one: what a record names as held or as a cursor's end
  const known = k => Replay.CONTROLS.includes(k) || !!indexed(k);

  /* The frame's size in game pixels for a view with the map `M` and a column of `sideH` rows (0 without one), on a
     players entry's `screen` ([sw, sh] cells; null: the map's): Wg x Hg, the map's extent mw x mh and the viewport
     vw x vh, the world between the column and the bars (THE SCREEN, the header). Without a screen the viewport is as
     wide as the map and as deep as the frame leaves, the map at its top. */
  function size(M, sideH, screen = null) {
    const cell = +M.cell || 16, mw = (+M.w || 1) * cell, mh = (+M.h || 1) * cell;
    const vw = screen ? screen[0] * cell : mw, Hg = Math.max((screen ? screen[1] * cell : mh) + 2 * BAR, +sideH || 0);
    return { Wg: SIDE_W + vw, Hg, mw, mh, vw, vh: Hg - 2 * BAR };
  }
  /* THE EYE of camera `cam` (a view centre, integers) on a frame of size() `sz`: the viewport's top left, held to the
     map less the viewport, the engine's eye clamp 0x41C3C0 (THE EYE, the header). Integers: the camera is (sim.js
     seatCam), and the viewport is whole cells wide and rows deep. */
  const eye = (cam, sz) => ({ x: clamp(cam.x - Math.trunc(sz.vw / 2), 0, Math.max(0, sz.mw - sz.vw)),
    z: clamp(cam.z - Math.trunc(sz.vh / 2), 0, Math.max(0, sz.mh - sz.vh)) });

  /* THE IDLE COLUMN of layout side `side`: its MAIN2 with nothing selected, which is what a watcher's screen shows
     (R0: the viewer controls the spectator record, whose unit list is empty). Each gadget's state is sim.js
     gadgetState(g, null, false)'s: hidden -1; the orders and build tabs, the build buttons, STOP and the orders 2 (greyed,
     nothing selected can use them); anything else 0. framecheck pins this second copy of that rule against a fresh
     sim's MAIN2. null when the layout has no such side. */
  function idleSide(layout, side) {
    const sd = layout && Number.isInteger(side) && side >= 0 ? layout.sides[side] : null, scr = sd && layout.screens[sd.main2];
    if (!scr) return null;
    const GREY = ['orders', 'buildtab', 'build', 'stop', 'order'];
    return { index: side, h: layout.height, screen: sd.main2, unit: null, page: 0, under: null,
      state: scr.gadgets.map(g => g.hidden ? -1 : GREY.includes(g.role) ? 2 : 0) };
  }

  /* THE FRAME'S VIEW of game view `gv`, a copy, by `mode`:
     - 'watch', THE WATCHER'S VIEW, Watching's: the fog all clear (the full-map reveal sets the watcher's sight and
       explored bit in every cell, tagpu_replay.c), and seat 0's cursor, ghost, sites, queued sites, drag, lines, sweep,
       held keys and selection gone (the viewer is not seat 0, sim spec 4.7). The column is the idle MAIN2 of layout
       side `side` whatever the game script's `sidebar` says: that key governs seat 0's recorded inputs, and the frame
       is the watcher's screen.
     - 'player', the live rewind idle or open: the view as the simulation made it, seat 0's fog, column, selection,
       sites, queued sites, drag, lines and sweep (MEASURED, M0 #1: open keeps the column, the selection and the armed
       mode). The armed building's ghost only while `cursor`, the simulation's cursor being the one drawn (record's
       simcur): the game draws it at the pointer (M0 #1), and with the viewer's hand on the HUD no pointer is over the
       world [INFERRED: open with the pointer on the strip is not measured]. `gv` is seat `human`'s view (sim.view), and
       the selection is that seat's alone: a unit's selected flag is set only by its owner's seat, and the view carries
       every seat's.
     - 'past', the live rewind showing the past: the past simulation's world and its fog, seat 0's at T - 1 (MEASURED,
       M0 #2), under the column `gv` carries, which the caller takes from the live view (the column shown at the open
       stays: MEASURED, M0 #2), with no unit under the cursor; nothing selected and no armed mode, so no ghost, sites or
       queued sites, and no seat 0 cursor, drag, lines, sweep or keys (MEASURED: +0x110 bit 4 clear, main+0x2CC3 1).
     A frame view has no cursor of its own: the frame draws the viewer's or the simulation's in its TOP range. Without
     a column of its own a live view takes the idle MAIN2 of `side`, since the game's screen always has one.

     `ov` is what the replay adds to the world it shows: `tick`, the shown tick n (the world's); `clickFrom`, the
     replay's click ring's start (replay.js view().clickFrom: a seek empties the ring); `players`, a players replay's
     view (replay.js view().players) or null; `screen`, a players entry's screen or null. The watcher's view and the
     past's carry THE OVERLAYS (the header), each switched by the replay's switches (all on but in a players replay):
     `ocur`, each seat's cursor { x, z, seat } while it is on the map, in seat order; `oclick`, the clicks
     { x, z, seat, age } made at or after clickFrom with age n + 1 - t in 0..14 (PD12: the game ages a
     click against GameTime + 1, tagpu_replay.c:11284, put with its iteration's tick [INFERRED: the iteration is the one
     the input of tick t feeds]), oldest first; per unit `osel`, its owner seat's selection. The watcher's are every
     seat's, the past's the human seat's alone. With a screen the view carries `eye` { x, z, w, h }: the eye and the
     viewport (THE EYE). Under a players replay's Sight, the watcher's view is the viewed seat's (sightView). */
  function view(gv, layout, side, mode = 'watch', cursor = false, human = 0, ov = {}) {
    if (!gv || !gv.map) return null;
    const M = gv.map, n = (+M.w || 1) * (+M.h || 1), P = ov.players || null, tog = P ? P.tog : TOG.cursors | TOG.clicks | TOG.selections | TOG.chat;
    // a seat whose overlays the view carries: every seat to the watcher, the human seat in the past
    const mine = k => mode === 'watch' || k === human;
    let v;
    if (mode === 'player') v = { ...gv, frame: 1, cursor: null, ghost: cursor ? gv.ghost : null,
      units: (gv.units || []).map(u => u.selected && u.owner !== human ? { ...u, selected: false } : u) };
    else {
      v = { ...gv, frame: 1, ghost: null, sites: [], queued: [], lines: [], drag: null, reclaim: null, keys: [], cursor: null, spacing: null,
        units: (gv.units || []).map(u => ({ ...u, selected: false, osel: !!(tog & TOG.selections) && !!u.selected && mine(u.owner) })) };
      if (mode === 'watch') { v.map = { ...M, fog: new Array(n).fill(0) }; delete v.side; }
      else if (v.side) v.side = { ...v.side, under: null };
      const g = gv.game || {}, t = +ov.tick || 0, from = +ov.clickFrom || 0;
      v.ocur = tog & TOG.cursors ? (g.cursors || []).map((c, k) => ({ x: c.x, z: c.z, seat: k, on: c.on }))
        .filter(c => c.on && mine(c.seat)).map(({ x, z, seat }) => ({ x, z, seat })) : [];
      v.oclick = tog & TOG.clicks ? (g.clicks || []).filter(c => mine(c.seat) && c.tick >= from && t + 1 - c.tick >= 0 && t + 1 - c.tick < CLICK_SHOW)
        .map(c => ({ x: c.x, z: c.z, seat: c.seat, age: t + 1 - c.tick })) : [];
    }
    if (!v.side) {
      const sd = idleSide(layout, side);
      if (sd) v.side = sd;
      else delete v.side;
    }
    if (ov.screen) {
      const sz = size(M, v.side ? v.side.h : 0, ov.screen), e = eye(P ? P.cam : { x: 0, z: 0 }, sz);
      v.eye = { x: e.x, z: e.z, w: sz.vw, h: sz.vh };
    }
    if (P && P.sight >= 0 && mode === 'watch') sightView(v, gv, layout, P, +ov.tick || 0);
    return v;
  }

  /* SIGHT IN THE FRAME'S VIEW (players-spec.md 7.4), seat s = P.sight at the shown tick n, into the watcher's view v:
     1. the lit grid, Sim.sightLit over the layout's sight table, the function the replay's pass calls;
     2. the map's fog, each 16-unit cell its block's: clear where the count is not 0, grey (1) where it is -- the mapped
        mode of the skirmish's True LOS, no black;
     3. the units drawn: seat s's, and any other whose footprint's four corners (x +- 8 fx, z +- 8 fz) meet a lit block
        -- the footprint's corners stand in for 0x465AC0's four model-box points [INFERRED], so a unit may show a
        block earlier or later than in the game at a sight's edge; a radar contact is not drawn on the map (MEASURED,
        m0p-facts.md P5). `slots` keeps each drawn unit's index in gv.units, which the page's poses are aligned with;
     4. the minimap's `mini`: `lit`, the pass's fog bits a byte a block of the (w >> 1) x (h >> 1) grid, 1 lit (the
        pass carries block i as bit i & 7 of byte i >> 3; a block past the pass's is unlit), and `dots` { x, z, owner },
        seat s's units and every unit the pass's contacts name that is still live, at its place now: the dots move
        every tick while the bits keep the pass's (0x466DC0 rebuilds the dots every tick, the pass runs on the
        record's deadline). */
  function sightView(v, gv, layout, P, n) {
    const s = P.sight, M = gv.map, w = +M.w || 1, h = +M.h || 1, gw = w >> 1, gh = h >> 1;
    const lit = Sim.sightLit(gv, layout && layout.sight ? layout.sight : null, s, n);
    const block = (c, r) => c >= 0 && c < gw && r >= 0 && r < gh ? lit[r * gw + c] : 0;
    v.map = { ...v.map, fog: Array.from({ length: w * h }, (_, i) => block((i % w) >> 1, Math.trunc(i / w) >> 1) ? 0 : 1) };
    const at = (x, z) => block(Math.floor(x) >> 5, Math.floor(z) >> 5) > 0;
    const seen = u => u.owner === s || [[-1, -1], [1, -1], [-1, 1], [1, 1]].some(([a, b]) => at(u.x + a * 8 * (+u.fx || 1), u.z + b * 8 * (+u.fz || 1)));
    const slots = [];
    v.units = v.units.filter((u, i) => seen(u) && slots.push(i));
    v.slots = slots;
    const seenLit = new Uint8Array(gw * gh), hex = String(P.pass.fog || '');
    for (let i = 0; i < gw * gh && i < P.pass.blocks && (i >> 3) * 2 + 2 <= hex.length; i++)
      seenLit[i] = parseInt(hex.substr((i >> 3) * 2, 2), 16) >> (i & 7) & 1;
    const con = new Set(P.pass.contacts);
    v.mini = { gw, gh, lit: seenLit, dots: (gv.units || []).filter(u => u.owner === s || con.has(u.id)).map(u => ({ x: u.x, z: u.z, owner: u.owner })) };
  }

  // a game tick as m:ss: a bookmark's from 0, the live strip's from the rewind's first GameTime (tagpu_hudinput.c live_clock)
  const clock = (T, first) => { const s = Math.trunc((T > first ? T - first : 0) / TICK); return `${Math.trunc(s / 60)}:${String(s % 60).padStart(2, '0')}`; };
  /* a game tick as the Take over set's toasts name it, tagpu_hudinput.c clock_at: whole seconds from tick 0 (a takeover
     replay's watching starts there, as its dock's clock), rounded as the dock's clock rounds */
  const clockAt = T => { const s = Math.trunc((Math.max(T, 0) + TICK / 2) / TICK); return `${Math.trunc(s / 60)}:${String(s % 60).padStart(2, '0')}`; };
  /* The toast the HUD shows at demo tick `rv.tick`, the game's strings (tagpu_hudinput.c): '' for none. Each shows
     TOAST_TICKS; Play from here's arm says what the second click does for as long as the arm holds, as the game's
     live_fill keeps it (its snap's seek can outlast a toast's time). `names` names the seats a toast names (Taking over
     NAME); one past them is named ''. */
  function toast(rv, names = []) {
    const t = rv.toast, L = rv.live;
    if (!t || (rv.tick - t.at >= TOAST_TICKS && !(t.kind === 'armed' && L && L.arm === 'armed'))) return '';
    const first = L ? L.first : 0;
    switch (t.kind) {
      case 'removed': return 'Bookmark removed';
      case 'added': return `Bookmark at ${clock(t.value, 0)}`;
      case 'playing_on': return `Playing on from ${clock(t.value, first)}`;
      case 'cannot': return `Cannot play from ${clock(t.value, first)}`;
      case 'armed': return `Click again to play from ${clock(t.value, first)}${t.evict >= 0 ? `, replacing F${t.evict}` : ''}`;
      case 'playing_from': return `Playing from ${clock(t.value, first)}`;
      case 'taking_over': return `Taking over ${t.seat >= 0 && t.seat < names.length ? names[t.seat] : ''} at ${clockAt(t.value)}`;
      case 'cannot_take': return `Cannot take over at ${clockAt(t.value)}`;
      case 'saved': return `F${t.value} saved as a replay of its own`;
      case 'cannot_save': return `Cannot save F${t.value}`;
      case 'click_delete': return `Click again to delete F${t.value}`;
      case 'deleted': return `F${t.value} deleted`;
      case 'camera': return 'Camera was not recorded';
      default: return 'Rewind refused';
    }
  }

  /* THE TAKE OVER BLOCK of a record from a takeover replay's view `T` (rv.tk): the picker (0 closed, 1 asked or seeking,
     2 open), the lanes, the fork list by (R, id) as tagpu_hudstate.c fill_forks lists the game's, each with its flags --
     the watched one, the members of the watched chain, the Delete armed while the lanes are open -- and the ticks the
     stage HUD's track reads */
  function tkBlock(T) {
    const chain = new Set();
    for (let k = T.selected; k >= 0 && k < T.timelines.length && !chain.has(k); k = T.timelines[k].parent) chain.add(k);
    const fork = T.timelines.filter(r => r.id > 0 && r.live).sort((a, b) => a.R - b.R || a.id - b.id).slice(0, FORKS)
      .map(r => ({ id: r.id, from: r.R, end: r.last, parent: r.parent, seat: r.seat >= 0 && r.seat < 0xff ? r.seat : 0xff,
        flags: (r.id === T.selected ? WATCHED : 0) | (chain.has(r.id) ? CHAIN : 0) | (T.lanes && T.armed === r.id ? ARMED : 0) }));
    return { tk: 1, pick: T.pick === 'open' ? 2 : T.pick === 'closed' ? 0 : 1, lanes: T.lanes ? 1 : 0, nfork: fork.length,
      human: T.human, fork_max: FORKS, tk_full: T.tkFull ? 1 : 0, chain_last: T.chainLast, fill_to: T.fillTo, pickR: T.pickR,
      pickEvict: T.pickEvict, fork };
  }
  const NO_TK = { tk: 0, pick: 0, lanes: 0, nfork: 0, human: 0, fork_max: 0, tk_full: 0, chain_last: 0, fill_to: 0, pickR: 0,
    pickEvict: 0, fork: [] };

  /* A PLAYERS ENTRY'S SEATS for its records, from the demo's scene: each seat's team (its own index when the scene
     gives none, a team of one) and whether its camera was recorded (a scene `camera`: sim.js seatCam is null without
     one, the game's "Camera was not recorded") */
  function plSeats(entry, demo) {
    const names = Object.keys(entry.scenes || {}), on = demo && demo.on !== undefined ? demo.on : names[0];
    const sc = (entry.scenes || {})[on] || { seats: [] };
    return (sc.seats || []).map((z, k) => ({ team: Number.isInteger(z.team) ? z.team : k, cam: !!Sim.seatCam(entry, demo, k, 0) }));
  }
  /* THE PLAYERS BLOCK of a players replay's view `P` for a record of `n` seats, `pl` = { seats (plSeats), stat (the
     replay's R.stat) } or null: Sight and Follow (-1 none), the switches, the folded rows, the stats window (its
     measure an index in MEASURES), the samples' count, period and base, each seat's team, the recorded cameras as a bit
     a seat, and each seat's samples of the measure */
  function plBlock(P, n, pl) {
    const zs = pl && pl.seats ? pl.seats : [], stat = pl && pl.stat;
    return { pl: 1, sight: P.sight, follow: P.follow, tog: P.tog, folded: P.folded, stat_open: P.stat.open ? 1 : 0,
      stat_team: P.stat.team ? 1 : 0, stat_measure: P.stat.measure, npt: P.npt, period: P.period, base: P.base,
      team: Array.from({ length: n }, (_, k) => zs[k] ? zs[k].team : k),
      cam: zs.reduce((a, z, k) => a | (z.cam && k < n ? 1 << k : 0), 0),
      series: Array.from({ length: n }, (_, k) => Array.from({ length: P.npt }, (_, s) => stat ? F32(stat(s, k, P.stat.measure)) : 0)) };
  }
  /* THE TOP BAR'S SIDE of replay view `rv` (the record's `topSide`): the human seat's in a takeover's live game, the
     viewed seat's under a players replay's Sight, else `side` (seatSide) */
  function topSide(rv, layout, side) {
    const s = rv.live ? rv.live.human : rv.players && rv.players.sight >= 0 ? rv.players.sight : 0;
    return seatSide(layout, side, s);
  }

  /* A TRIP'S CONTROL as the HUD keys it (THE INDEXED CONTROLS, above): a picker row by its seat `v`, a lane's control by
     the place of the fork `v` in record `rec`'s list, '' (the rest point) when the list does not hold it; a players
     row's Jump, Sight and Follow and a team header's fold by its seat or team `v`, a chip by its name, CHIPS[v]; any
     other control by its name */
  function keyOf(rec, c, v) {
    if (c === 'takeas' || c === 'jump' || c === 'sight' || c === 'follow' || c === 'fold') return `${c}:${v}`;
    if (c === 'chip') return v >= 0 && v < Sim.CHIPS.length ? `chip:${Sim.CHIPS[v]}` : '';
    if (c === 'lane' || c === 'export' || c === 'delete') { const k = rec.fork.findIndex(f => f.id === v); return k >= 0 ? `${c}:${k}` : ''; }
    return c;
  }

  /* THE FRAME RECORD (tagpu_wikiframe.h WIKIFRAMEREC) from replay view `rv` and the game view `gv` it shows, the load's
     layout side `side`, `tryIt` (the presses are the player's: no scripted cursor), in Try it the control the press
     machine holds, and `topSide`, the layout side of the human seat (seatSide), which the record takes only in a
     takeover's live game. Made after the frame's last step or press: a view is only valid until the next.
     The cursor in a demo: `from` the previous trip's control and final tick, or '' (the rest point) before any; `to`
     this trip's, '' before the first; for the track `to_v` the tick under the cursor, which follows a drag as replay.js
     seeks it; step i of n of the glide (THE GLIDE, above), and it stays at `to` until the next trip starts. A Take over
     control's key is its indexed one (keyOf). `watched`: when `to` or else `from` is Back to main recording, the
     timeline watched at that trip's press (replay.js's cursor), the lane `main` was drawn on (pressed); 0 otherwise.
     LIVE: the rewind's state, its span and the GameTime shown, the arm and its start; the top bar's numbers, the
     replay's memo (the eased stored values and the rates last sampled) with the human seat's storage and stored values;
     and THE ONE CURSOR (the header): the human seat's cursor in `sim`, its world point, shape and age, and `simcur`
     while it is the one drawn. The human seat is seat 0 in Rewind and the seat taken over in a takeover's live game,
     whose view `gv` then is (sim.view(h)) and whose colour the badge takes. A trip that starts while the rewind is idle
     starts from it (replay.js feed, `@game`); a press that left the rewind idle, or took a seat over, glides from its
     control back to it over HUD_DOWN clock ticks from `back`; after that glide, and before any trip, the drawn cursor is
     the simulation's. In Try it the simulation's cursor is the player's pointer over the world once a fork is played,
     idle or open (M0 #1), never in the past; before the first fork the world is the recording, whose cursor is not
     drawn. The floats are f32, as the C record carries them, so both sides compose the bar and place the cursor from
     the same values.
     PLAYERS (a players replay, rv.players, with `pl` = { seats, stat }: plSeats' seats and the replay's R.stat): the
     players block -- Sight, Follow, the switches, the folded team rows, the stats window's state, the samples' count,
     period and base, each seat's team and whether its camera was recorded (`cam`, a bit a seat), and `series`, each
     seat's samples of the window's measure as f32 (the measure the game's TAGPU_PK_HUDSTAT carries); every other record
     has none. A players record's top bar is the replay's memo, eased toward the viewed seat's numbers or under the full
     map the watcher's zeros; under Sight its storage and stored values are that seat's, its badge that seat's colour
     and its art that seat's side's (`topSide`, topSide()). */
  function record(rv, gv, side, tryIt, held, topSide = side, pl = null) {
    const g = rv.game, seats = (gv && gv.game && gv.game.seats) || [], L = rv.live || null, T = rv.tk || null;
    const h = L && L.human >= 0 && L.human < Sim.SEATS ? L.human : 0;
    const sh = L && seats[h], gc = L && gv ? gv.cursor : null;
    const P = rv.players || null, vs = P && P.sight >= 0 && P.sight < seats.length ? P.sight : -1, vz = vs >= 0 ? seats[vs] : null;
    const rec = {
      on: 1, playing: !g.paused && g.tick < g.end ? 1 : 0, question: rv.question ? 1 : 0, nseat: seats.length,
      side, colour: L ? h : vs >= 0 ? vs : 0, speed: g.speed, try_it: tryIt ? 1 : 0, playhead: g.playhead, end: g.end,
      seat: seats.map((z, k) => ({ name: z.name, colour: k, lost: z.lost > 0 ? 1 : 0, out: z.out ? 1 : 0, units: z.units,
        m: z.m, mmax: z.mmax, mi: z.mi, mo: z.mo, e: z.e, emax: z.emax, ei: z.ei, eo: z.eo })),
      mark: rv.marks.map(([t, kind, seat]) => ({ t, kind, seat })).concat(rv.books.map(t => ({ t, kind: MK_BOOKMARK, seat: -1 }))),
      held: '',
      toast: toast(rv, seats.map(z => z.name)),
      cur: { on: 0, from: '', to: '', from_v: -1, to_v: -1, watched: 0, i: Replay.HUD_DOWN, n: Replay.HUD_DOWN },
      live: L ? 1 : 0, rw: L ? RW.indexOf(L.rw) : 0, arm: L ? ARMS.indexOf(L.arm) : 0,
      first: L ? L.first : 0, G: L ? L.G : 0, now: L ? L.now : 0, last: L ? L.last : 0, armR: L && L.arm !== 'none' ? L.armR : 0,
      bar: sh ? { e: L.bar.be, m: L.bar.bm, ei: L.bar.rates[0], eo: L.bar.rates[1], mi: L.bar.rates[2], mo: L.bar.rates[3],
        emax: F32(sh.emax), mmax: F32(sh.mmax), raw_e: F32(sh.e), raw_m: F32(sh.m) }
        : P ? { e: P.bar.be, m: P.bar.bm, ei: P.bar.rates[0], eo: P.bar.rates[1], mi: P.bar.rates[2], mo: P.bar.rates[3],
          emax: vz ? F32(vz.emax) : 0, mmax: vz ? F32(vz.mmax) : 0, raw_e: vz ? F32(vz.e) : 0, raw_m: vz ? F32(vz.m) : 0 }
        : { e: 0, m: 0, ei: 0, eo: 0, mi: 0, mo: 0, emax: 0, mmax: 0, raw_e: 0, raw_m: 0 },
      simcur: 0,
      sim: gc ? { x: F32(gc.x), z: F32(gc.z), shape: Math.max(0, SHAPES.indexOf(gc.shape)), age: L.now - gc.since } : { x: 0, z: 0, shape: 0, age: 0 },
      ...(T ? tkBlock(T) : NO_TK),
      top_side: (T && L) || vs >= 0 ? topSide : side,
      ...(P ? plBlock(P, seats.length, pl) : {}),
    };
    rec.nmark = rec.mark.length;
    const c = rv.cursor, n = Replay.HUD_DOWN, idle = !!L && L.rw === 'idle';
    if (tryIt) {
      rec.held = known(held) ? held : '';
      rec.simcur = L && L.selected !== 0 && L.rw !== 'past' ? 1 : 0;
      return rec;
    }
    rec.held = rv.held ? keyOf(rec, rv.held, c.value) : '';
    // a trip's tick under the cursor on clock tick `at`: the press's, or along its drag
    const tick = at => {
      if (!c.drag) return c.value;
      const k = clamp(at - c.at - n, 0, c.drag.over);
      return c.value + Math.trunc(((c.drag.drag - c.value) * k) / c.drag.over);
    };
    // the return glide: this trip's release left the rewind idle or took a seat over (replay.js `back`, a clock tick at
    // or after its start)
    const back = L && c && L.back >= c.at ? L.back : -1;
    if (back >= 0) {
      Object.assign(rec.cur, { from: keyOf(rec, c.to, c.value), from_v: c.to === 'track' ? tick(back) : -1, to: AT_GAME, to_v: -1,
        watched: c.to === 'main' ? c.watched : 0, i: clamp(rv.clock - back, 0, n), n });
      rec.simcur = idle && rv.clock - back >= n ? 1 : 0;
    } else if (c)
      Object.assign(rec.cur, { from: c.from === AT_GAME ? AT_GAME : c.from ? keyOf(rec, c.from, c.fromValue) : '',
        from_v: c.from && c.from !== AT_GAME ? c.fromValue : -1, to: keyOf(rec, c.to, c.value), to_v: tick(rv.clock),
        watched: c.to === 'main' ? c.watched : c.from === 'main' ? c.fromWatched : 0, i: clamp(rv.clock - c.at, 0, n), n });
    else rec.simcur = idle ? 1 : 0;
    rec.cur.on = rec.simcur ? 0 : 1;
    return rec;
  }

  /* The layout side seat `seat` plays on, the top bar's in a takeover's live game: seat 0 the load's `side`; any other
     seat the opponent's, the first layout side other than `side`, which a game entry's rival units are cast by (the
     DLL's rival_of, simparity.mjs's rival); `side` with no layout or no other side. */
  function seatSide(layout, side, seat) {
    if (seat === 0 || !layout || !Array.isArray(layout.sides)) return side;
    const r = layout.sides.findIndex((_, j) => j !== side);
    return r >= 0 ? r : side;
  }

  // the lanes a record's stage HUD draws, in their keys' order: its forks' ids while it watches a takeover's replay with
  // the lanes open (tagpu_wikiframe_lanes)
  const lanes = rec => rec.tk && !rec.live && rec.lanes ? rec.fork.map(f => f.id) : [];

  /* the colour of seat colour index `k`: the palette colour of the middle pixel of logos frame k, as the game's HUD
     takes it (tagpu_replay.c logo_colour: pixel (w / 2, h / 2)), or the stand-in without logos */
  function seatColour(art, k) {
    const L = art && art.logos, S = art && art.sheet, pal = art && art.pal;
    if (!L || !S || !pal || !(k >= 0 && k < L.length)) return COLOURS[k] || '#8a8a8a';
    const [sx, sy, w, h] = L[k].f, p = S.px[(sy + (h >> 1)) * S.w + sx + (w >> 1)];
    return `rgb(${pal[p * 4]},${pal[p * 4 + 1]},${pal[p * 4 + 2]})`;
  }

  // the teams of each seat's team value: a list of seats a team, the teams in the order of their first seats
  function teamsOf(team) {
    const out = [], at = new Map();
    (team || []).forEach((t, i) => { if (!at.has(t)) { at.set(t, out.length); out.push([]); } out[at.get(t)].push(i); });
    return out;
  }
  // teams are told apart only when one of them has two players (hud.js and tagpu_hudlay.c `headed`)
  const headed = teams => teams.length > 1 && teams.some(m => m.length > 1);
  // a sample is data from the recording: one that is not a finite number is drawn as 0 (tagpu_hudstate.c sample)
  const sample = v => v === v && v < 1e30 && v > -1e30 ? v : 0;
  /* THE STATS WINDOW'S GROUPS, tagpu_hudstate_stat_groups' twin (index.html state() makes the mockup's the same way):
     a group a seat, in its colour and with the time of its first commander-death marker (null for none), its samples
     the record's series; or, when the split asks for teams and the teams are told apart (headed, the test the teams
     chip is enabled by), a group a team, `Team N` in the chart's team colours `cols`, its samples the sum of its seats'
     in f32 as the game adds them. `seats`, `teams` and `markers` are the state's, the markers' times in seconds. */
  function statGroups(rec, seats, teams, markers, cols) {
    const npt = rec.npt, S = rec.series;
    if (!(rec.stat_team && headed(teams)))
      return seats.map((z, i) => {
        const d = markers.filter(m => m.kind === 'death' && m.seat === i).map(m => m.t);
        return { name: z.name, col: z.colour, death: d.length ? Math.min(...d) : null,
          v: Array.from({ length: npt }, (_, s) => sample(S[i] ? S[i][s] : 0)) };
      });
    return teams.map((m, t) => ({ name: `Team ${t + 1}`, col: cols[t % cols.length], death: null,
      v: Array.from({ length: npt }, (_, s) => m.reduce((a, i) => F32(a + sample(S[i] ? S[i][s] : 0)), 0)) }));
  }

  /* THE STAGE HUD'S STATE, hud.js's (tools/hudart/mockup/index.html state()) from record `rec` in a frame Wg x Hg: a
     screen of the frame's own size at 100 % (Q3), the engine frame the column and both bars, every switch on and
     nothing folded as a fresh Watch, each seat a team of one, no sight, no follow, no forks (an empty list: the game's
     HUD always draws the Forks bay and Take over, hud.js only for a state with a list). The stats window stays
     closed: the DLL's pass has one chart image, the replay HUD's, and the stage must have its own before it opens one.
     A live record sets the live HUD as the game's present_live does (tagpu_hudstate.c): the button down and the strip
     drawn while the rewind is open or in the past, the track from its first GameTime to G, never playing. A seat is out,
     its row dimmed and its picker row refused, while it has no unit: the game's rule (tagpu_hudstate.c, `out` is units
     == 0), which the takeover refuses by too (tk_bound).
     A TAKEOVER'S WATCHING (a tk record that is not live) sets the Take over set as tagpu_hudstate.c `forks` and
     `present` set the game's from the packet, with the track's start at tick 0 and its axis the end (every chain's last
     lies within it): the time text's second clock the watched chain's last, the fill to fill_to, the playhead held to
     the chain's last, the forks in seconds named by their seats, the watched one, the armed Delete and, while the
     picker is open, the one it would replace by their places, the picker drawn while open, the Take over button down
     while it is asked, sought or open (pickDown), and the lanes as open as the record says. `art` the side pack's art
     for the seats' colours, or null.
     A PLAYERS RECORD (rec.pl) sets the Players set as the game's present does: Sight and Follow on their seats, the
     switches from its bits (Stats from the window's state), the folded team rows, the teams from each seat's team in
     seat order (tagpu_hudstate.c teams joins the mutual alliances in that order, and a team's seats are mutually
     allied), each seat's `cam`, and the stats window at layout.json's place (PD13: the stage has no drag) with its
     measure, its split and its groups (statGroups) at a sample every period / 30 s from base / 30 s. `hud` is the HUD
     layout (art/hud/layout.json, hud.js's) for the window's place and the teams' colours, or null for its values. */
  function hudState(rec, Wg, Hg, art, hud = null) {
    const k = HUD_H / Hg, live = !!rec.live, G = Math.max(rec.G, rec.first), tk = !!rec.tk && !live;
    // live, the strip's track runs from the rewind's first GameTime to G (tagpu_hudstate.c present_live)
    const T = live ? (G - rec.first) / TICK : rec.end / TICK;
    const t = live ? (clamp(rec.now, rec.first, G) - rec.first) / TICK : (tk ? Math.min(rec.playhead, rec.chain_last) : rec.playhead) / TICK;
    const at = flag => rec.fork.findIndex(f => f.flags & flag);
    const pl = !!rec.pl, seats = rec.seat.map((z, i) => ({ i, name: z.name, colour: seatColour(art, z.colour), units: z.units, lost: !!z.lost,
      out: z.units === 0, cam: pl && !!(rec.cam >> i & 1), res: { m: z.m, mmax: z.mmax, e: z.e, emax: z.emax, mi: z.mi, mo: z.mo, ei: z.ei, eo: z.eo } }));
    const markers = rec.mark.map(m => ({ t: m.t / TICK, kind: MK_NAMES[m.kind], seat: m.seat }));
    const teams = pl ? teamsOf(rec.team) : rec.seat.map((_, i) => [i]);
    return {
      W: Wg * k, H: HUD_H, frame: { panel: SIDE_W * k, top: BAR * k, bottom: BAR * k },
      t, T, playing: !live && !!rec.playing, speed: rec.speed / 10, hud: 1,
      seats, teams, folded: [], scroll: 0, sight: -1, follow: -1, markers,
      toggles: { cursors: true, clicks: true, selections: true, chat: true, stats: false },
      toast: rec.toast, stats: { open: false }, ends: T, len: T, fillTo: T, forks: [],
      ...(pl ? {
        sight: rec.sight, follow: rec.follow, folded: teams.map((_, j) => j).filter(j => rec.folded >> j & 1),
        toggles: { cursors: !!(rec.tog & TOG.cursors), clicks: !!(rec.tog & TOG.clicks), selections: !!(rec.tog & TOG.selections),
          chat: !!(rec.tog & TOG.chat), stats: !!rec.stat_open },
        stats: { open: !!rec.stat_open, x: hud ? hud.window.x : WINDOW.x, y: hud ? hud.window.y : WINDOW.y,
          measure: MEASURES[rec.stat_measure] || MEASURES[0], split: rec.stat_team && headed(teams) ? 'team' : 'player',
          groups: statGroups(rec, seats, teams, markers, hud ? hud.chart.teams : TEAM_COLOURS),
          dt: rec.period / TICK, t0: rec.base / TICK },
      } : {}),
      ...(tk ? {
        len: rec.chain_last / TICK, fillTo: rec.fill_to / TICK,
        forks: rec.fork.map(f => ({ id: f.id, name: f.seat < rec.seat.length ? rec.seat[f.seat].name : 'Unknown seat',
          from: f.from / TICK, end: f.end / TICK, parent: f.parent, chain: !!(f.flags & CHAIN) })),
        timeline: at(WATCHED), armed: at(ARMED), replace: rec.pick === 2 ? rec.fork.findIndex(f => f.id === rec.pickEvict) : -1,
        picker: rec.pick === 2, pickDown: rec.pick !== 0, lanesOpen: !!rec.lanes,
      } : {}),
      // the live HUD (hud.js live): its strip while the rewind is open or shows the past, and Play from here's arm
      ...(live ? { live: { open: rec.rw !== 0 }, rw: { arm: rec.arm } } : {}),
    };
  }

  /* LEAVE'S QUESTION in canvas device pixels for the fit `fit` ({ s, ox, oz }: the frame's x 128, y 32 is the canvas
     point (ox, oz)) of a frame Wg x Hg: the panel, the title, Yes and No, each edge snapped as every frame part is. Wg
     and Hg are even (16 a cell, 128 and 64 the frame's parts, the layouts' rows), so the centred GUI space's corner is a
     whole frame pixel. */
  function dialog(fit, Wg, Hg) {
    const gx = YESORNO.x + (Wg - 640) / 2, gy = YESORNO.y + (Hg - 480) / 2;
    const r = (x, y, w, h) => {
      const x0 = snap(fit.ox + (gx + x - SIDE_W) * fit.s), y0 = snap(fit.oz + (gy + y - BAR) * fit.s);
      return { x: x0, y: y0, w: snap(fit.ox + (gx + x + w - SIDE_W) * fit.s) - x0, h: snap(fit.oz + (gy + y + h - BAR) * fit.s) - y0 };
    };
    return { panel: r(0, 0, YESORNO.w, YESORNO.h), title: r(...YESORNO.title), yes: r(...YESORNO.yes), no: r(...YESORNO.no),
      frame: r(-gx, -gy, Wg, Hg) };
  }

  // a point of a glide from a to b at step i of n: the curve of sim.js glideAt, in doubles, since it is a position
  const glide = (a, b, i, n) => i >= n ? b : a + (b - a) * i * i * (3 * n - 2 * i) / (n * n * n);

  /* SEAT 0'S CURSOR in HUD units, for record `rec` in the frame fitted as `fit` (stage.js fit) with its corner at the
     canvas point (fx, fy) and `px` device pixels a unit: the world point's canvas point, as stage.js places a world
     point, from that corner. null for a record that is not live. */
  function gamePoint(rec, fit, fx, fy, px) {
    if (!rec.live) return null;
    return { x: (fit.ox + rec.sim.x * fit.s - fx) / px, y: (fit.oz + rec.sim.z * fit.s - fy) / px };
  }

  /* THE HUD THE TRIP PRESSED, whose controls the cursor's point is resolved among (point's `boxes`): a return glide from
     a picker row (a takeover's) leaves the replay HUD with the picker open, whose rows lie where the seat count and the
     limit's warning put them (tagpu_hudlay.c picker) and nowhere else, so its fork list is placeholders, as many as draw
     the warning when the takeover's picker drew it (tk_full); any other return glide leaves a control of the strip its
     press closed (Back to now, Play from here), so its controls are the HUD's as drawn at that press, the strip open
     over the past with fwd live (`now` at the strip's start) and nothing armed; the controls lie where they lie
     whatever the playhead or the arm. A trip to or from Back to main recording whose press left the lane it is drawn on
     (cur.watched, a fork the list holds and no longer watches) is resolved on the lanes as drawn at that press: open,
     with that fork the watched one. Watching a lane is what draws its Back to main recording and widens the lane to
     hold it (tagpu_hudlay.c lanes); the other flags only fill. A trip to or from the stats window's close whose press
     closed the window is resolved on the HUD with the window open, as drawn at that press: the window stands at the
     layout's place whatever its state, so its close lies where it was. Any other record's are its own HUD's. Its
     boxes only: the HUD drawn is the record's own. */
  function pressed(rec) {
    const c = rec.cur;
    if (rec.pl && !rec.stat_open && (c.to === 'close' || c.from === 'close')) return { ...rec, stat_open: 1 };
    if (rec.tk && !rec.live && c.watched > 0 && (c.to === 'main' || c.from === 'main')) {
      const k = rec.fork.findIndex(f => f.id === c.watched);
      if (k < 0 || (rec.lanes && rec.fork[k].flags & WATCHED)) return rec;
      return { ...rec, lanes: 1, fork: rec.fork.map((f, j) => ({ ...f, flags: j === k ? f.flags | WATCHED : f.flags & ~WATCHED })) };
    }
    if (!rec.live || rec.cur.to !== AT_GAME) return rec;
    if (rec.cur.from.startsWith('takeas:')) {
      const n = rec.tk_full ? rec.fork_max : 0;
      return { ...rec, live: 0, rw: 0, pick: 2, nfork: n,
        fork: Array.from({ length: n }, (_, k) => ({ id: k + 1, from: 0, end: 0, parent: 0, seat: 0, flags: 0 })) };
    }
    return { ...rec, rw: RW.indexOf('past'), now: rec.first, arm: 0 };
  }

  /* THE CURSOR'S POINT, in HUD units, for record `rec`: each key at its control's box centre on the device pixels the
     box is drawn on, ((snap(x0 px) + snap(x1 px)) / 2) / px; the track at its tick along the playhead's run at the track
     box's centre, the run being the game's [0, end], live the strip's [first, G]; leave:no at the question's No; @game at
     seat 0's cursor (gamePoint); '' and a key no box holds at the rest point. `boxes` the stage HUD's controls, each
     { key, ub: [x, y, w, h] in units, ends } (ends: the track's, from hud.js data-ends), `px` its device pixels a unit,
     `rest`, `no` (the question's No, or null) and `game` (gamePoint, or null) as { x, y } in units. */
  function point(rec, boxes, px, rest, no, game) {
    const c = rec.cur, at = key => boxes.filter(b => b.key === key).pop();
    const mid = (a, w) => (snap(a * px) + snap((a + w) * px)) / 2 / px;
    const a0 = rec.live ? rec.first : 0, a1 = rec.live ? Math.max(rec.G, rec.first) : rec.end;
    const resolve = (key, v) => {
      if (key === 'leave:no') return no || rest;
      if (key === AT_GAME) return game || rest;
      const b = key ? at(key) : null;
      if (!b) return rest;
      const [x, y, w, h] = b.ub;
      if (key !== 'track') return { x: mid(x, w), y: mid(y, h) };
      const x0 = x + b.ends, x1 = x + w - b.ends;
      return { x: x0 + (x1 - x0) * (clamp(v, a0, a1) - a0) / Math.max(1, a1 - a0), y: mid(y, h) };
    };
    const a = resolve(c.from, c.from_v), b = resolve(c.to, c.to_v);
    return { x: glide(a.x, b.x, c.i, c.n), y: glide(a.y, b.y, c.i, c.n) };
  }

  /* A FRAME POINT OF THE WORLD, in world units (stage.js's x, z): on the column, x in [-128, 0) and z in [-32, Hg - 32)
     (the column starts at the top bar's first row), or on the map, x in [0, mw) and z in [0, mh). A point on a bar, or
     on the viewport below a map shorter than the column, is none. Try it yourself's world takes a point only here
     (rewind-spec.md 9.6), and the simulation's cursor is drawn only here. */
  const onWorld = (x, z, Hg, mw, mh) =>
    (x >= -SIDE_W && x < 0 && z >= -BAR && z < Hg - BAR) || (x >= 0 && x < mw && z >= 0 && z < mh);

  /* THE PRESS MACHINE, Try it yourself's presses on the stage HUD and on Leave's question, as the game's HUD takes them
     (tagpu_hudinput.c, hud-in-game.md): a left press on a control holds it, and a button acts at its release only when
     the release lands on the same control; the track seeks at the press, again whenever the pointer's tick changes while
     it is held, and at the release; a right press is taken and does nothing; while the question is open, its Yes and
     No act at their release, `outside` (the frame round the dialog) counts as No at once and the dialog's body does
     nothing. A key that is none of the replay's controls (the HUD's containers, uiscale, a row) is taken and does
     nothing, so a press can never reach anything but the stage's replay; no press plays a sound (decision 9). Each call
     returns the presses to make, [control, value] pairs for the replay's press(). `T` is the track's tick under the
     pointer: the page maps the pointer's x on the playhead's run, clamped, and replay.js's seek clamps it again.
     THE INDEXED CONTROLS are buttons too: a picker row's release gives takeas with the row, a players row's Jump,
     Sight and Follow theirs with the seat, a team header's fold with the team and a chip's chip with its index in
     CHIPS, a lane's control's its control with the fork the lane showed. `ids` (lanes()) is the forks of the lanes the pressed frame drew, in their
     keys' order; a lane's control is held only when it names a lane drawn, and acts at its release only when the lanes
     then drawn name the same fork at that place: a list that changed under the pointer between the two -- a deletion's,
     a fork made -- never sends the release to the fork that moved into its place (tagpu_hudinput.c held_lane, the
     game's rule). The id is data: the replay refuses one that is not a live fork. */
  function Press() {
    let held = '', lastT = -1, lane = -1;
    // the controls a key names only with its index: bare, no button (a players row's buttons and a chip among them)
    const plain = ['track', 'takeas', 'lane', 'export', 'delete', 'jump', 'sight', 'follow', 'chip'];
    const button = k => !!indexed(k) || (Replay.CONTROLS.includes(k) && !plain.includes(k));
    const laneAt = (ids, n) => Array.isArray(ids) && n < ids.length ? ids[n] : -1;
    return {
      get held() { return held; },
      down(src, key, right, T, ids = []) {
        if (right || held) return [];
        if (src === 'dialog') {
          if (key === 'outside') return [['leave:no', 0]];
          if (key === 'yes' || key === 'no') held = key;
          return [];
        }
        if (key === 'track') { held = key; lastT = T; return [['track', T]]; }
        const ix = indexed(key);
        // a lane's control is held only on a lane drawn, with the fork it showed
        if (ix && LANE.includes(ix.act) && (lane = laneAt(ids, ix.n)) < 0) return [];
        if (button(key)) held = key;
        return [];
      },
      drag(T) {
        if (held !== 'track' || T === lastT) return [];
        lastT = T;
        return [['track', T]];
      },
      up(key, T, ids = []) {
        const h = held, id = lane;
        held = '';
        lane = -1;
        if (h === 'track') return T === lastT ? [] : [['track', T]];
        if (!h || key !== h) return [];
        const ix = indexed(h);
        if (ix) return !LANE.includes(ix.act) ? [[ix.act, ix.n]] : id >= 0 && laneAt(ids, ix.n) === id ? [[ix.act, id]] : [];
        return [[h === 'yes' ? 'leave:yes' : h === 'no' ? 'leave:no' : h, 0]];
      },
    };
  }

  /* THE REST POINT of a frame view `fv`, the cursor's point before any trip, from the viewport's top left in world units:
     the map's centre, or with a screen the viewport's (the map is larger than it) */
  const restOf = fv => {
    const M = fv.map, cell = +M.cell || 16;
    return fv.eye ? { x: fv.eye.w / 2, z: fv.eye.h / 2 } : { x: (+M.w || 1) * cell / 2, z: (+M.h || 1) * cell / 2 };
  };

  return { size, eye, idleSide, view, toast, record, hudState, seatColour, dialog, glide, gamePoint, pressed, point, onWorld, Press, YESORNO,
    COLOURS, HUD_H, SHAPES, indexed, seatSide, topSide, lanes, plSeats, statGroups, teamsOf, restOf, TOG, CLICK_SHOW };
})();
if (typeof module !== 'undefined') module.exports = WikiFrame;
