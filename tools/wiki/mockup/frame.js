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

   Runs in a page (the global WikiFrame) and under node (module.exports), as replay.js does. */
const WikiFrame = (() => {
  const Replay = typeof WikiReplay !== 'undefined' ? WikiReplay : require('./replay.js');
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
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  const snap = v => Math.floor(v + 0.5 + 1 / 64);

  /* The frame's size in game pixels for a view with the map `M` and a column of `sideH` rows (0 without one). */
  function size(M, sideH) {
    const cell = +M.cell || 16, mw = (+M.w || 1) * cell, mh = (+M.h || 1) * cell;
    return { Wg: SIDE_W + mw, Hg: Math.max(mh + 2 * BAR, +sideH || 0), mw, mh };
  }

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

  /* THE WATCHER'S VIEW of game view `gv`: a copy that draws as the game shows a Watch at full map. The fog is all
     clear (the full-map reveal sets the watcher's sight and explored bit in every cell, tagpu_replay.c), and seat 0's
     cursor, ghost, sites, queued sites, drag, lines, sweep, held keys and selection are gone (the viewer is not seat 0,
     sim spec 4.7). The column is the idle MAIN2 of layout side `side` whatever the game script's `sidebar` says: that
     key governs seat 0's recorded inputs, and the frame is the watcher's screen. */
  function view(gv, layout, side) {
    if (!gv || !gv.map) return null;
    const M = gv.map, n = (+M.w || 1) * (+M.h || 1);
    const v = { ...gv, frame: 1, map: { ...M, fog: new Array(n).fill(0) },
      ghost: null, sites: [], queued: [], lines: [], drag: null, reclaim: null, keys: [], cursor: null, spacing: null,
      units: (gv.units || []).map(u => ({ ...u, selected: false })) };
    const sd = idleSide(layout, side);
    if (sd) v.side = sd;
    else delete v.side;
    return v;
  }

  /* The toast the HUD shows at demo tick `rv.tick`, the game's strings (tagpu_hudinput.c): '' for none */
  function toast(rv) {
    const t = rv.toast;
    if (!t || rv.tick - t.at >= TOAST_TICKS) return '';
    if (t.kind === 'removed') return 'Bookmark removed';
    const s = Math.trunc(t.value / TICK);
    return `Bookmark at ${Math.trunc(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  }

  /* THE FRAME RECORD (tagpu_wikiframe.h WIKIFRAMEREC) from replay view `rv` and the game view `gv` it shows, the layout
     side of the controlled seat `side`, `tryIt` (the presses are the player's: no scripted cursor) and in Try it the
     control the press machine holds. Made after the frame's last step or press: a view is only valid until the next.
     The cursor in a demo: `from` the previous trip's control and final tick, or '' (the rest point) before any; `to`
     this trip's, '' before the first; for the track `to_v` the tick under the cursor, which follows a drag as replay.js
     seeks it; step i of n of the glide (THE GLIDE, above), and it stays at `to` until the next trip starts. */
  function record(rv, gv, side, tryIt, held) {
    const g = rv.game, seats = (gv && gv.game && gv.game.seats) || [];
    const rec = {
      on: 1, playing: !g.paused && g.tick < g.end ? 1 : 0, question: rv.question ? 1 : 0, nseat: seats.length,
      side, colour: 0, speed: g.speed, try_it: tryIt ? 1 : 0, playhead: g.playhead, end: g.end,
      seat: seats.map((z, k) => ({ name: z.name, colour: k, lost: z.lost > 0 ? 1 : 0, out: z.out ? 1 : 0, units: z.units,
        m: z.m, mmax: z.mmax, mi: z.mi, mo: z.mo, e: z.e, emax: z.emax, ei: z.ei, eo: z.eo })),
      mark: rv.marks.map(([t, kind, seat]) => ({ t, kind, seat })).concat(rv.books.map(t => ({ t, kind: MK_BOOKMARK, seat: -1 }))),
      held: tryIt ? (Replay.CONTROLS.includes(held) ? held : '') : rv.held || '',
      toast: toast(rv),
      cur: { on: 0, from: '', to: '', from_v: -1, to_v: -1, i: Replay.HUD_DOWN, n: Replay.HUD_DOWN },
    };
    rec.nmark = rec.mark.length;
    const c = rv.cursor;
    if (!tryIt) {
      rec.cur.on = 1;
      if (c) {
        const n = Replay.HUD_DOWN;
        let to_v = c.value;
        if (c.drag) {
          const k = clamp(rv.clock - c.at - n, 0, c.drag.over);
          to_v = c.value + Math.trunc(((c.drag.drag - c.value) * k) / c.drag.over);
        }
        Object.assign(rec.cur, { from: c.from || '', from_v: c.from ? c.fromValue : -1, to: c.to, to_v, i: clamp(rv.clock - c.at, 0, n), n });
      }
    }
    return rec;
  }

  /* the colour of seat colour index `k`: the palette colour of the middle pixel of logos frame k, as the game's HUD
     takes it (tagpu_replay.c logo_colour: pixel (w / 2, h / 2)), or the stand-in without logos */
  function seatColour(art, k) {
    const L = art && art.logos, S = art && art.sheet, pal = art && art.pal;
    if (!L || !S || !pal || !(k >= 0 && k < L.length)) return COLOURS[k] || '#8a8a8a';
    const [sx, sy, w, h] = L[k].f, p = S.px[(sy + (h >> 1)) * S.w + sx + (w >> 1)];
    return `rgb(${pal[p * 4]},${pal[p * 4 + 1]},${pal[p * 4 + 2]})`;
  }

  /* THE STAGE HUD'S STATE, hud.js's (tools/hudart/mockup/index.html state()) from record `rec` in a frame Wg x Hg: a
     screen of the frame's own size at 100 % (Q3), the engine frame the column and both bars, every switch on and
     nothing folded as a fresh Watch, each seat a team of one, no sight, no follow, no forks. The stats window stays
     closed: the DLL's pass has one chart image, the replay HUD's, and the stage must have its own before it opens one.
     `art` the side pack's art for the seats' colours, or null. */
  function hudState(rec, Wg, Hg, art) {
    const k = HUD_H / Hg, T = rec.end / TICK;
    return {
      W: Wg * k, H: HUD_H, frame: { panel: SIDE_W * k, top: BAR * k, bottom: BAR * k },
      t: rec.playhead / TICK, T, playing: !!rec.playing, speed: rec.speed / 10, hud: 1,
      seats: rec.seat.map((z, i) => ({ i, name: z.name, colour: seatColour(art, z.colour), units: z.units, lost: !!z.lost, out: !!z.out,
        cam: false, res: { m: z.m, mmax: z.mmax, e: z.e, emax: z.emax, mi: z.mi, mo: z.mo, ei: z.ei, eo: z.eo } })),
      teams: rec.seat.map((_, i) => [i]), folded: [], scroll: 0, sight: -1, follow: -1,
      markers: rec.mark.map(m => ({ t: m.t / TICK, kind: MK_NAMES[m.kind], seat: m.seat })),
      toggles: { cursors: true, clicks: true, selections: true, chat: true, stats: false },
      toast: rec.toast, stats: { open: false }, ends: T, len: T, fillTo: T,
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

  /* THE CURSOR'S POINT, in HUD units, for record `rec`: each key at its control's box centre on the device pixels the
     box is drawn on, ((snap(x0 px) + snap(x1 px)) / 2) / px; the track at its tick along the playhead's run at the track
     box's centre; leave:no at the question's No; '' and a key no box holds at the rest point. `boxes` the stage HUD's
     controls, each { key, ub: [x, y, w, h] in units, ends } (ends: the track's, from hud.js data-ends), `px` its device
     pixels a unit, `rest` and `no` (the question's No, or null) as { x, y } in units. */
  function point(rec, boxes, px, rest, no) {
    const c = rec.cur, at = key => boxes.filter(b => b.key === key).pop();
    const mid = (a, w) => (snap(a * px) + snap((a + w) * px)) / 2 / px;
    const resolve = (key, v) => {
      if (key === 'leave:no') return no || rest;
      const b = key ? at(key) : null;
      if (!b) return rest;
      const [x, y, w, h] = b.ub;
      if (key !== 'track') return { x: mid(x, w), y: mid(y, h) };
      const x0 = x + b.ends, x1 = x + w - b.ends;
      return { x: x0 + (x1 - x0) * clamp(v, 0, rec.end) / Math.max(1, rec.end), y: mid(y, h) };
    };
    const a = resolve(c.from, c.from_v), b = resolve(c.to, c.to_v);
    return { x: glide(a.x, b.x, c.i, c.n), y: glide(a.y, b.y, c.i, c.n) };
  }

  /* THE PRESS MACHINE, Try it yourself's presses on the stage HUD and on Leave's question, as the game's HUD takes them
     (tagpu_hudinput.c, hud-in-game.md): a left press on a control holds it, and a button acts at its release only when
     the release lands on the same control; the track seeks at the press, again whenever the pointer's tick changes while
     it is held, and at the release; a right press is taken and does nothing; while the question is open, its Yes and
     No act at their release, `outside` (the frame round the dialog) counts as No at once and the dialog's body does
     nothing. A key that is none of the replay's controls (the HUD's containers, uiscale, a row) is taken and does
     nothing, so a press can never reach anything but the stage's replay; no press plays a sound (decision 9). Each call
     returns the presses to make, [control, value] pairs for the replay's press(). `T` is the track's tick under the
     pointer: the page maps the pointer's x on the playhead's run, clamped, and replay.js's seek clamps it again. */
  function Press() {
    let held = '', lastT = -1;
    const button = k => Replay.CONTROLS.includes(k) && k !== 'track';
    return {
      get held() { return held; },
      down(src, key, right, T) {
        if (right || held) return [];
        if (src === 'dialog') {
          if (key === 'outside') return [['leave:no', 0]];
          if (key === 'yes' || key === 'no') held = key;
          return [];
        }
        if (key === 'track') { held = key; lastT = T; return [['track', T]]; }
        if (button(key)) held = key;
        return [];
      },
      drag(T) {
        if (held !== 'track' || T === lastT) return [];
        lastT = T;
        return [['track', T]];
      },
      up(key, T) {
        const h = held;
        held = '';
        if (h === 'track') return T === lastT ? [] : [['track', T]];
        if (!h || key !== h) return [];
        return [[h === 'yes' ? 'leave:yes' : h === 'no' ? 'leave:no' : h, 0]];
      },
    };
  }

  return { size, idleSide, view, toast, record, hudState, seatColour, dialog, glide, point, Press, YESORNO, COLOURS, HUD_H };
})();
if (typeof module !== 'undefined') module.exports = WikiFrame;
