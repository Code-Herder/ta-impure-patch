/* gen-quickbuild.mjs -- writes art/wiki/entries/quickbuild.json: the units table, the entry scene and its demos.

   usage: node tools/wiki/gen-quickbuild.mjs        then node tools/wiki/simcheck.mjs

   A demo's cursor glides with a move every two ticks, so its inputs are generated here rather than written by hand;
   change a demo here and regenerate. The unit figures are the stock game's, read once from the retail unit files
   (each row's `from`); nothing of the game is copied. `canstop` is written only for the types whose FBI sets it:
   the engine reads a missing key as 0 (sim.js canStop), and so does the sim.

   No demo writes `sidebar`, so each takes sim.js's default, decision 2 of
   research/notes/ingame-wiki-sidebar-plan.md: the sidebar is shown when the scene gives the player a mobile builder.
   ARMCK and ARMACK stand in this entry's scene, so every demo here shows it. */
import fs from 'node:fs';
import { createRequire } from 'node:module';
const { TRIP } = createRequire(import.meta.url)('./mockup/sim.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const C = c => c * 16 + 8;                  // a cell's centre in world units
const at = (cx, cz) => [C(cx), C(cz)];

const units = {
  ARMCK: { footprint: [2, 2], buildTime: 5597, workerTime: 80, buildDistance: 40, maxVelocity: 0.8, canstop: 1,
    builds: ['ARMSOLAR', 'ARMWIN', 'ARMESTOR', 'ARMMSTOR', 'ARMMEX', 'ARMMAKR', 'ARMLAB', 'ARMVP', 'ARMAP', 'ARMSY', 'ARMLLT', 'ARMRAD',
      'ARMALAB', 'ARMGEO', 'ARMHLT', 'ARMRL', 'ARMDRAG', 'ARMGUARD', 'ARMHP'],
    name: 'Construction KBot', from: 'ARMCK.fbi; build list gamedata/sidedata.tdf [CANBUILD]' },
  ARMACK: { footprint: [2, 2], buildTime: 13432, workerTime: 160, buildDistance: 60, maxVelocity: 0.6, canstop: 1,
    builds: ['ARMLAB', 'ARMARAD', 'ARMFUS', 'ARMMOHO', 'ARMBRTHA', 'ARMSILO', 'ARMANNI', 'ARMAMD', 'ARMASP'],
    name: 'Adv. Construction Kbot', from: 'ARMACK.fbi; build list gamedata/sidedata.tdf [CANBUILD]' },
  ARMMEX: { footprint: [3, 3], extracts: 0.001, buildTime: 1800, name: 'Metal Extractor', from: 'ARMMEX.fbi' },
  ARMMOHO: { footprint: [5, 5], extracts: 0.003, buildTime: 35750, name: 'Moho Mine', from: 'ARMMOHO.fbi' },
  ARMGEO: { footprint: [4, 4], energyMake: 250, vent: true, buildTime: 13078, name: 'Geothermal Powerplant',
    from: 'ARMGEO.fbi (YardMap all G: one cell on a geothermal feature)' },
  ARMPW: { footprint: [2, 2], buildTime: 1452, maxVelocity: 1.8, canstop: 1, name: 'Peewee', from: 'ARMPW.fbi' },
};

/* the map, 40 x 30 cells, 480 rows: as tall as stock TA's sidebar beside it (decision 9 of
   research/notes/ingame-wiki-sidebar-plan.md). Deposits: A 5x5 (a Moho fits), B C E 3x3, S 3x3 between the map's
   unbuildable top row and a finished ARMGEO (no Moho site over it is legal), D 3x3 under the player's finished
   ARMMEX, R 3x3 beside the broad metal band, one deposit in the black corner and one under grey fog. Vents: a free
   one, and the one under the ARMGEO. The metal ground and the fog cover whole tiles (even cells): the stage's ground
   is drawn in the map's 2 x 2-cell tiles and its fog in the engine's 32-unit blocks, so a rect off that grid has no
   picture (research/notes/ingame-wiki-3d-stage-plan.md, section 4; tools/wiki/stagemap.py check refuses one). R
   keeps two background columns between it and the band, so it stays a deposit of its own. The band runs the map's
   full height: one that stopped short of the bottom edge would be a deposit (sim.js distinct). Every demo plays in
   rows 0..23; rows 24..29 hold only plain ground and the band. */
const scene = {
  map: { w: 40, h: 30, surfaceMetal: 0 },
  metal: [
    { rect: [13, 15, 5, 5], v: 90 }, { rect: [14, 16, 3, 3], v: 170 },   // A
    { rect: [22, 3, 3, 3], v: 140 },                                     // B
    { rect: [9, 7, 3, 3], v: 120 },                                      // C
    { rect: [3, 10, 3, 3], v: 120 },                                     // E
    { rect: [15, 1, 3, 3], v: 130 },                                     // S
    { rect: [26, 15, 3, 3], v: 150 },                                    // D
    { rect: [34, 0, 6, 30], v: 40 },                                     // broad metal ground, edge to edge
    { rect: [29, 19, 3, 3], v: 160 },                                    // R, closed by background cells
    { rect: [2, 2, 3, 3], v: 120 },                                      // in the black corner
    { rect: [3, 18, 3, 3], v: 110 },                                     // under grey fog
  ],
  vents: [at(28, 9), at(16, 5)],
  fog: [{ rect: [0, 0, 8, 8], kind: 'black' }, { rect: [0, 16, 10, 8], kind: 'grey' }],
  units: [
    { id: 'pw', type: 'ARMPW', at: [256, 176] },
    { id: 'ck', type: 'ARMCK', at: [304, 176] },
    { id: 'ack', type: 'ARMACK', at: [352, 176] },
    { id: 'geo', type: 'ARMGEO', at: [272, 96] },
    { id: 'mex', type: 'ARMMEX', at: [440, 264] },
  ],
  selected: [],
  cursor: [304, 140],
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
    path(ps, n) { const each = Math.round(n / ps.length); for (const p of ps) T.to(p, each); return T; },
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
  };
  return T;
}

const demos = [];
function demo(id, title, add, fn, tail) {
  const T = TL(add.cursor || scene.cursor);
  fn(T);
  demos.push({ id, title, scene: add, inputs: T.inputs, end: T.t + tail });
}

demo('snap', 'Snap onto the deposit', { selected: ['ck'] }, T => T
  .say('ARMCK selected. Pick the metal extractor in the build menu.').wait(20).build('ARMMEX').wait(5)
  .to([344, 120], 50).say('Near a deposit the ghost jumps onto it, not under the cursor.').wait(30)
  .to([408, 104], 40).wait(15).to([344, 40], 40).wait(20).to([340, 112], 30).wait(15)
  .click('lmb').say('Click: it is placed on the deposit and ARMCK builds it.'), 130);

demo('alt', 'Hold Alt to place exactly', { selected: ['ck'] }, T => T
  .say('ARMCK selected, metal extractor picked from the build menu.').wait(20).build('ARMMEX').wait(5)
  .to([344, 120], 50).say('The ghost snaps onto the deposit...').wait(35)
  .down('alt').say('...hold Alt and it stays exactly under the cursor.').wait(45)
  .up('alt').say('Let go of Alt and it snaps again.').wait(40)
  .down('alt').say('Hold Alt and click: built exactly where you clicked.').wait(30).click('lmb').wait(5).up('alt'), 60);

demo('click', 'Click a deposit to build', { selected: ['ck'] }, T => T
  .say('ARMCK selected. No build menu: just point at a deposit.').wait(25)
  .to([240, 120], 40).wait(10).to([330, 128], 40).say('Over a deposit the ghost of the extractor appears.').wait(30)
  .to([372, 76], 30).wait(25).click('rmb').say('Right-click builds it. The mode stays on for the next one.'), 130);

demo('bestfit', 'The best extractor that fits', { selected: ['ck', 'ack'], cursor: [330, 150] }, T => T
  .say('ARMCK and ARMACK selected: together they can build both extractors.').wait(25)
  .to([248, 284], 50).say('A Moho mine fits this deposit, and it is the strongest: ARMACK will build it.').wait(55)
  .path([[210, 150], [266, 40]], 60).say('This deposit is wedged against the edge: no room for a Moho, so a metal extractor.').wait(55)
  .click('rmb').say('Right-click: ARMCK builds the extractor, ARMACK helps.').wait(100)
  .to([248, 284], 60).wait(10).click('rmb').say('Back on the big deposit: the Moho mine, and ARMCK helps.'), 300);

demo('queue', 'Queue several with Shift', { selected: ['ck'] }, T => T
  .say('ARMCK selected. Hold Shift to queue.').wait(25).down('shift')
  .to([72, 184], 50).wait(10).click('rmb').say('Shift+right-click: queued. The mode stays on.').wait(20)
  .to([168, 136], 35).wait(10).click('rmb').say('The next deposit gives its own ghost: queued after the first.').wait(20)
  .to([248, 284], 45).wait(10).click('rmb').say('And a third. ARMCK works through them in order.').wait(20).up('shift'), 330);

demo('cancel', 'Cancel a queued one', { selected: ['ck'] }, T => T
  .say('ARMCK selected.').wait(20).to([72, 184], 60).down('shift').wait(5).click('rmb').up('shift')
  .say('Shift+right-click: the extractor is queued.').wait(35)
  .click('rmb').say('A plain right-click on the queued site changes nothing.').wait(40)
  .down('shift').wait(5).click('rmb').up('shift').say('Shift+right-click it again: the plan is cancelled and ARMCK stops.'), 90);

demo('helpers', 'Helpers assist', { selected: ['pw'], cursor: [300, 210] }, T => T
  .say('The Peewee is sent off on a walk.').wait(10).to([60, 360], 12).click('rmb').wait(5)
  .to([400, 140], 25).down('lmb').to([150, 300], 35).up('lmb').say('Box-select it with both constructors.').wait(15)
  .to([248, 284], 35).say('ARMACK can build the Moho mine; ARMCK cannot.').wait(30)
  .click('rmb').say('Right-click: ARMACK builds, ARMCK guards it and helps, the Peewee keeps walking.'), 240);

demo('upgrade', 'Upgrade your extractor', { selected: ['ack'] }, T => T
  .say('ARMACK selected. Point at your finished metal extractor.').wait(25)
  .to([440, 266], 55).say('It offers the stronger Moho mine in its place.').wait(40)
  .click('rmb').say('Right-click: the extractor is reclaimed, then the Moho mine is built.'), 310);

demo('geo', 'Geothermal vents', { selected: ['ck'] }, T => T
  .say('ARMCK selected. Point at a geothermal vent.').wait(25)
  .to([456, 154], 55).say('Over a vent the ghost is a geothermal powerplant.').wait(35)
  .click('rmb').say('Right-click builds it on the vent.'), 285);

demo('metalmap', 'Metal maps', { selected: ['ck'] }, T => T
  .say('ARMCK selected. The right side is uniform metal ground.').wait(25)
  .to([568, 120], 55).say('Broad metal ground is not a deposit: no ghost, the cursor says move.').wait(35)
  .click('rmb').say('Right-click: ARMCK just moves there.').wait(30)
  .to([490, 330], 50).say('A deposit with plain ground around it is one: the ghost appears.').wait(35)
  .click('rmb').say('Right-click builds it.'), 240);

demo('fog', 'Fog', { selected: ['ck'] }, T => T
  .say('ARMCK selected, metal extractor picked from the build menu.').wait(20).build('ARMMEX').wait(5)
  .path([[200, 120], [104, 88]], 70).say('Unexplored ground (black): a deposit you have not seen does not pull the ghost.').wait(60)
  .path([[136, 220], [40, 280]], 60).say('Under grey fog the deposit is known: the ghost snaps onto it.').wait(45)
  .click('lmb').say('Click: placed on the deposit.'), 90);

const entry = {
  format: 1, id: 'quickbuild',
  title: 'Quick Build: Resource Extractors',
  subtitle: 'Extractors and geothermal plants land on their deposit or vent, and a constructor builds one with a single right-click.',
  text: [
    'Placing a metal extractor from the build menu near a metal deposit puts it on the deposit: the ghost jumps to the spot within three squares that covers the most metal. A geothermal powerplant jumps onto a nearby vent the same way.',
    'With a constructor selected you need no build menu at all. Point at a deposit or a vent: the ghost of the best extractor or powerplant your selection can build there appears, and a right-click orders it. Constructors that cannot build it help the ones that can. Broad metal ground that runs across the map is not a deposit, and neither is a patch joined to it, so a right-click there is still a move; place an extractor there from the build menu.',
    'Hold Shift to queue one deposit after another; the mode stays on. Shift+right-click a queued site again to cancel it. Hold Alt to place exactly where the cursor is, with no snapping and no Quick Build.',
    'Point at one of your finished extractors with a constructor that can build a stronger one, such as ARMACK over a metal extractor: a right-click reclaims the old one and builds the Moho mine in its place.',
    'Only deposits you have explored count. Escalation, TA Zero and unrecognised mods turn the feature off; both parts can be switched off separately in Impure\'s options.',
  ],
  lockedIn: ['escalation', 'tazero', 'unknown'],
  pace: { build: 32 },
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
fs.writeFileSync(`${ROOT}art/wiki/entries/quickbuild.json`, fmt(entry) + '\n');
console.log(demos.map(d => `${d.id}: ${d.inputs.length} inputs, end ${d.end}`).join('\n'));
