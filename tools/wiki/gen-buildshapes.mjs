/* gen-buildshapes.mjs -- writes art/wiki/entries/buildshapes.json: the units table, the entry scene and its demos.

   usage: node tools/wiki/gen-buildshapes.mjs        then node tools/wiki/simcheck.mjs

   The demos are the placement gesture of tagpu/ddraw/src/tagpu_place.c (research/notes/tadr-port/quality-of-life.md,
   "What Impure does") played through sim.js. A demo's cursor glides with a move every two ticks, so its inputs are
   generated here; change a demo here and regenerate. The unit figures are the stock game's, read once from the retail
   unit files (each row's `from`); nothing of the game is copied. `canstop` and `activatewhenbuilt` are written only for the
   types whose FBI sets them: the engine reads a missing key as 0 (sim.js canStop), and so does the sim.

   Each unit names its `cast`, its place in the demos: where a side's casting applies, the sim plays that side's type
   for the cast, with its figures, and the authored type plays everywhere else. Text names units by cast tokens
   (`{builder}`, `{tower}`, ...) and buttons by `{order}`, `{select}` and `{cancel}` (capitalised to start a
   sentence), filled for the player's side and interface type when shown; the sim keeps the raw text. A `{` that
   starts no token is refused by both loaders. No `a` or `an` stands before a cast token, since the filled name
   decides the article. An input whose button depends on the interface type names its action (`cancel` for the right
   press that drops a drag); `lmb` stays for a press that is the left button in both types and none of the actions,
   placing an armed building and dragging a shape. research/notes/ingame-wiki.md, *The data format*.

   No demo writes `sidebar`, so each takes sim.js's default, the owner's decision 2
   in research/notes/ingame-wiki.md: the sidebar is shown when the scene gives the player a mobile builder.
   ARMCK stands in this entry's scene, so every demo here shows it. */
import fs from 'node:fs';
import { createRequire } from 'node:module';
const { TRIP } = createRequire(import.meta.url)('./mockup/sim.js');
const ROOT = new URL('../../', import.meta.url).pathname;
// the cursor that puts a footprint of f cells on the site whose first cell is (cx, cz): its centre, which the build
// cursor rounds back to that cell
const S = (cx, cz, f = 2) => [(cx + f / 2) * 16, (cz + f / 2) * 16];

const units = {
  ARMCK: { cast: 'builder', footprint: [2, 2], buildTime: 5597, workerTime: 80, buildDistance: 40, maxVelocity: 0.8, canstop: 1,
    builds: ['ARMSOLAR', 'ARMWIN', 'ARMESTOR', 'ARMMSTOR', 'ARMMEX', 'ARMMAKR', 'ARMLAB', 'ARMVP', 'ARMAP', 'ARMSY', 'ARMLLT', 'ARMRAD',
      'ARMALAB', 'ARMGEO', 'ARMHLT', 'ARMRL', 'ARMDRAG', 'ARMGUARD', 'ARMHP'],
    name: 'Construction KBot', from: 'ARMCK.fbi; build list: the stock build menu, every button in page order' },
  ARMLLT: { cast: 'tower', footprint: [2, 2], buildTime: 4662, canstop: 1, name: 'L.L.T.', from: 'ARMLLT.fbi' },
  ARMDRAG: { cast: 'wall', footprint: [2, 2], buildTime: 520, name: "Dragon's Teeth", from: 'ARMDRAG.fbi' },
  ARMSOLAR: { cast: 'solar', footprint: [5, 5], buildTime: 2495, activatewhenbuilt: 1, name: 'Solar Collector', from: 'ARMSOLAR.fbi' },
};

/* the map, 40 x 30 cells of plain ground, 480 rows: as tall as stock TA's sidebar beside it (the owner's
   decision 9 in research/notes/ingame-wiki.md). ARMCK stands left of centre; a finished ARMSOLAR (cells 20..24 x
   9..13) is the building to ring and the obstacle a line is refused on. Every demo plays in rows 0..23; rows 24..29
   are plain ground. */
const scene = {
  map: { w: 40, h: 30, surfaceMetal: 0 },
  units: [
    { id: 'ck', type: 'ARMCK', at: [120, 200] },
    { id: 'sol', type: 'ARMSOLAR', at: [360, 184] },
  ],
  selected: ['ck'],
  cursor: [240, 120],
};

// a timeline: the cursor glides with a move every 2 ticks, eased at both ends
function TL(start) {
  const inputs = []; let t = 0, cur = start.slice();
  const T = {
    inputs, get t() { return t; },
    wait(n) { t += n; return T; },
    to(p, n) {
      const [x0, z0] = cur, steps = Math.max(1, Math.round(n / 2));
      for (let i = 1; i <= steps; i++) {
        const u = i / steps, e = u * u * (3 - 2 * u);
        inputs.push({ at: t + Math.round(i * n / steps), move: [Math.round(x0 + (p[0] - x0) * e), Math.round(z0 + (p[1] - z0) * e)] });
      }
      t += n; cur = p.slice(); return T;
    },
    down(k) { inputs.push({ at: t, down: k }); return T; },
    up(k) { inputs.push({ at: t, up: k }); return T; },
    click(k) { T.down(k); t += 3; return T.up(k); },
    /* a build input with the sidebar shown plays out as sim.js's trip: the glide to the button, through the pagers
       when they are needed, the press, the arm at t + TRIP_UP and the glide back to where the cursor stood. Nothing
       but a caption may stand in its TRIP ticks (sim.js checkTrips), so the timeline moves past them; the trip ends
       with the cursor where it started, so `cur` still holds. Without the sidebar the build arms at once and the
       timeline is the same. A caption meant for the trip is given before the build, on its tick. */
    build(b) { inputs.push({ at: t, build: b }); t += TRIP; return T; },
    say(s) { inputs.push({ at: t, caption: s }); return T; },
    /* the demo's clock stops until as many buildings of the player's are finished in the demo as there have been
       `built` waits, this one included, at most `max` ticks (sim.js UNTILS); the wait takes a tick of its own, after
       the last input's, which no other input shares */
    built(max) { t += 1; inputs.push({ at: t, until: 'built', max }); t += 1; return T; },
  };
  return T;
}

/* A DEMO ENDS `tail` TICKS AFTER ITS LAST INPUT, on the shape and the first pieces of it, not on the whole queue: the
   builder's script folds its arms and opens them again between two queued builds (StopBuilding, then StartBuilding,
   each waiting on the other's animation), about 270 ticks a piece for stock's Construction KBot, so a whole line would
   run for minutes. A demo whose claim reads pieces finished (clicks, rect, ring: simcheck.mjs) first waits until
   `built` of them are finished, each wait at most WAIT_MAX ticks, and its tail starts after: how long the builder's
   walk and its script take is the demo's own, never a tail sized to them. */
const WAIT_MAX = 1800;
const demos = [];
function demo(id, title, add, fn, tail, built = 0) {
  const T = TL(add.cursor || scene.cursor);
  fn(T);
  for (let i = 0; i < built; i++) T.built(WAIT_MAX);
  demos.push({ id, title, scene: add, inputs: T.inputs, end: T.t + tail });
}

demo('clicks', '{Select} and Shift-{select}', {}, T => T
  .say('One {builder} selected. Pick the {tower} in the build menu.').wait(20).build('ARMLLT').wait(5)
  .to(S(10, 5), 40).wait(10).click('lmb').say('A plain {select} places one and puts the building away.').wait(45)
  .say('Pick it again and hold Shift.').build('ARMLLT').wait(5).down('shift')
  .to(S(13, 5), 30).wait(8).click('lmb').say('Shift-{select}: queued, and the building stays armed.').wait(20)
  .to(S(16, 5), 25).wait(8).click('lmb').wait(35)
  .up('shift').say('Let go of Shift: the building is put away, as in TA.'), 60, 2);

demo('line', 'Line', {}, T => T
  .say('{tower} armed. Hold Shift, press and drag.').build('ARMLLT').wait(5).to(S(4, 3), 40)
  .down('shift').wait(10).down('lmb')
  .to(S(16, 6), 70).say('A line at any angle: each site steps one footprint along and snaps to the grid on its own.').wait(45)
  .up('lmb').say('Release with Shift held: seven towers queued, in order.').wait(30)
  .up('shift').say('Letting go of Shift afterwards puts the building away.').wait(35)
  .say('Armed again: start another line...').build('ARMLLT').wait(5).to(S(4, 17), 40).down('shift').wait(8).down('lmb')
  .to(S(14, 17), 45).wait(15).up('shift').say('...and let go of Shift before the button: the line is gone.').wait(35)
  .up('lmb').say('Released: nothing is ordered, and the building is still armed.'), 60);

demo('axis', 'Straight line', {}, T => T
  .say('{tower} armed. Hold Shift and Ctrl, press and drag.').build('ARMLLT').wait(5).to(S(12, 10), 40)
  .down('shift').down('ctrl').wait(10).down('lmb')
  .to(S(30, 20), 90).say('Ctrl holds the line to its longer axis, wherever the cursor wanders.').wait(35)
  .say('Red: the {solar} stands there. Those sites are skipped.').wait(45)
  .up('lmb').up('ctrl').up('shift').say('Released: the green sites are queued, the red ones are not.'), 120);

demo('rect', 'Filled rectangle', {}, T => T
  .say('{wall} armed. Hold Shift and Alt, press and drag.').build('ARMDRAG').wait(5).to(S(6, 3), 40)
  .down('shift').down('alt').wait(10).down('lmb')
  .to(S(12, 7), 60).say('A filled rectangle. The numbers are the order: rows alternate, so the builder snakes through.').wait(55)
  .up('lmb').up('alt').up('shift').say('Released: twelve queued. The {builder} builds them in that order.'), 60, 2);

demo('hollow', 'Hollow rectangle', { cursor: [300, 330] }, T => T
  .say('{wall} armed. Shift, Alt and Ctrl: a hollow rectangle.').build('ARMDRAG').wait(5).to(S(12, 21), 30)
  .down('shift').down('alt').down('ctrl').wait(10).down('lmb')
  .to(S(4, 15), 60).say('Only the edge: a wall, started at the press and walked round in order.').wait(55)
  .up('lmb').up('ctrl').up('alt').up('shift').say('Released: fourteen pieces queued.'), 200);

demo('spacing', 'Spacing', {}, T => T
  .say('{wall} armed. Shift-drag a line: the pieces touch.').build('ARMDRAG').wait(5).to(S(4, 4), 40)
  .down('shift').wait(8).down('lmb').to(S(16, 4), 50).wait(30)
  .down('alt').wait(8).click('z').say('Hold Alt and press Z: one more empty square between neighbours, at once.').wait(35)
  .click('z').say('Z again: two empty squares. Fewer pieces cover the same drag.').wait(30).up('alt').wait(20)
  .up('lmb').say('Released: four pieces, two squares apart.').wait(15).up('shift').wait(20)
  .say('The {wall} keeps its spacing: the next line starts spread.').build('ARMDRAG').wait(5).to(S(4, 7), 35)
  .down('shift').wait(8).down('lmb').to(S(16, 7), 45).wait(25)
  .down('alt').wait(8).click('x').say('Alt+X takes one away...').wait(30).click('x').say('...and again: back to touching.').wait(25)
  .up('alt').wait(15).up('lmb').say('Released: seven pieces, touching.').wait(10).up('shift'), 150);

demo('ring', 'Ring around a building', {}, T => T
  .say('{wall} armed. Point at the {solar}.').build('ARMDRAG').wait(5).to([360, 184], 45).wait(10)
  .down('shift').down('ctrl').say('Hold Shift and Ctrl over a building: a ring round it.').wait(45)
  .click('lmb').up('ctrl').up('shift').say('Press and release without dragging: the ring is ordered.'), 60, 1);

/* The ring is clicked while the builder still walks to the far solar collector, and the walk outlasts the wall's
   trip for every side's builder only when it is long: TA Zero's hover builders cross the map at 2.25 a tick and start
   building about 144 world units short of a site. So the demo brings its own builder, `b`, of the entry's builder
   cast, into the map's top left corner and selects it, and the far solar collector stands in the bottom right (cells
   33..37 x 18..22), 615 world units away; the scene's own builder stands by. The cursor never leaves the near one
   after queuing it: the trip returns it there. MEASURED 2026-10-28 with the six packs: the ring click lands on tick
   336; TA Zero's GOK and CORE builders start the far one on 349, its ARM builder on 357, and no other side's builder
   starts it before the demo ends on 429. */
demo('ringqueued', 'Ring around a queued build', { units: [{ id: 'b', type: 'ARMCK', at: [24, 40] }], selected: ['b'] }, T => T
  .say('Queue the {solar} twice with Shift-{select}s: one far away, then one near.').build('ARMSOLAR').wait(5).down('shift')
  .to(S(33, 18, 5), 40).wait(5).click('lmb').wait(5).to(S(10, 15, 5), 40).wait(5).click('lmb').wait(5).up('shift')
  .say('The {builder} walks to the first. The second is only a queued square.').wait(5).build('ARMDRAG').wait(5)
  .say('{wall} armed: Shift and Ctrl over the queued {solar}.').down('shift').down('ctrl').wait(35)
  .click('lmb').up('ctrl').up('shift').say('{Select}: the ring is queued round it, before it is even started.'), 90);

/* The row stands near the map's top right (cells 29..38 x 3..4; the last column takes no building), as far from the
   builder as it fits, and the last Shift-{select} comes 45 ticks after the drag back: it takes a queued tower off,
   so it must land before the builder finishes the first one, and a mod's builder walks faster than stock's. */
demo('toggle', 'Placing it again removes it', {}, T => T
  .say('{tower} armed. Shift-drag a row of five.').build('ARMLLT').wait(5).to(S(29, 3), 45)
  .down('shift').wait(8).down('lmb').to(S(37, 3), 45).wait(10).up('lmb').say('Queued. Keep Shift held.').wait(20)
  .down('lmb').to(S(33, 3), 35).say('Drag back over the queue: yellow sites take a queued tower off.').wait(35)
  .up('lmb').say('Released: those three are gone; nothing is added.').wait(25)
  .to(S(31, 3), 15).wait(5).click('lmb').say('A Shift-{select} on a queued site takes it off too, as in TA.').wait(35)
  .up('shift'), 120);

demo('reshape', 'Change the shape mid-drag', {}, T => T
  .say('{wall} armed. Shift-drag: a line.').build('ARMDRAG').wait(5).to(S(4, 4), 40)
  .down('shift').wait(8).down('lmb').to(S(12, 9), 50).wait(30)
  .down('alt').say('Press Alt while dragging: a filled rectangle.').wait(45)
  .down('ctrl').say('Add Ctrl: hollow. The keys are read as you drag.').wait(45)
  .up('lmb').up('ctrl').up('alt').say('Release: what is held then is what is ordered.').wait(30)
  .to(S(28, 17), 40).down('lmb').to(S(36, 20), 40).say('Another drag...').wait(20)
  .click('cancel').say('...and a {cancel} cancels it and puts the building away.').wait(20).up('lmb').wait(10).up('shift'), 180);

const entry = {
  format: 1, id: 'buildshapes',
  title: 'Build Lines, Grids and Rings',
  subtitle: 'With a building armed, Shift-drag lays a whole line, rectangle or wall of it, and Shift+Ctrl rings a building.',
  text: [
    'With a building picked from the build menu, a plain {select} places one and puts the building away; a Shift-{select} queues one and keeps the building armed until you let go of Shift.',
    'Hold Shift, press and drag to place a row: each site steps one footprint along the longer axis, follows the slope on the other and snaps to the grid. Add Ctrl to hold the line to its axis, Alt for a filled rectangle, laid out in a snake so the builder never walks back across it, and Alt+Ctrl for a hollow one. The keys are read while you drag, so the shape changes the moment you press or let go of one. Release the button with Shift still held to order it.',
    "Hold Shift+Ctrl over a building, or over a building your constructor has queued, to ring it; a press and release orders the ring.",
    "Every site shows TA's build square: green is placed, red is refused there and skipped, yellow takes off a building already queued at that spot, as a Shift-{select} on a queued site does in TA. Dragging back over a queued row takes off every one of them. A ring leaves its yellow sites out.",
    'Hold Alt and press Z for one more empty square between neighbours, X for one fewer, from 0 to 16; each building remembers its own. A ring ignores it.',
    'Let go of Shift before the button and nothing is ordered; the building stays armed. A {cancel} cancels the drag and puts the building away. A gesture places at most 256 sites. TA arms a building only with one unit selected, so the order goes to that constructor.',
  ],
  lockedIn: [],
  pace: { build: 24 },
  units, scene, demos,
};
fs.mkdirSync(`${ROOT}art/wiki/entries`, { recursive: true });
// small values on one line, the rest expanded: an input per line
function fmt(v, ind = '') {
  const one = JSON.stringify(v);
  if (one.length <= 110 || v === null || typeof v !== 'object') return one;
  const ni = ind + '  ';
  if (Array.isArray(v)) return `[\n${v.map(x => ni + fmt(x, ni)).join(',\n')}\n${ind}]`;
  return `{\n${Object.entries(v).map(([k, x]) => `${ni}${JSON.stringify(k)}: ${fmt(x, ni)}`).join(',\n')}\n${ind}}`;
}
fs.writeFileSync(`${ROOT}art/wiki/entries/buildshapes.json`, fmt(entry) + '\n');
console.log(demos.map(d => `${d.id}: ${d.inputs.length} inputs, end ${d.end}`).join('\n'));
