/* gen-reclaim.mjs -- writes art/wiki/entries/reclaim.json: the units and features tables, the entry scene and its demos.

   usage: node tools/wiki/gen-reclaim.mjs        then node tools/wiki/simcheck.mjs and tools/wiki/stagemap.py build

   The demos are the reclaim sweep of tagpu/ddraw/src/tagpu_sweep.c (research/notes/gpu-status.md section 2.110;
   research/notes/ingame-wiki-reclaim-plan.md) played through sim.js. The unit figures are the stock game's, read once
   from the retail unit files, and the features' from greenworld's feature files (each row's `from`); stagemap.py check
   holds every feature row to its section on the local install. Nothing of the game is copied.

   A demo arms reclaim with a `reclaim` input, which sim.js plays as the cursor's trip to RECLAIM (through ORDERS when
   the page shown has none), and picks a kind off the popup with a `pick`, the trip to that row: both find their button
   on the running mod's own layout, so the same demo plays in every setup. E is pressed as a key once reclaim is armed,
   where RECLAIM is on the screen it was armed from in every setup. The ring's press is the left button at both
   Interface Types, as any armed order's is; its cancel is the `cancel` action. */
import fs from 'node:fs';
import { createRequire } from 'node:module';
const { TRIP, PICK } = createRequire(import.meta.url)('./mockup/sim.js');
const ROOT = new URL('../../', import.meta.url).pathname;
// the world point of cell (cx, cz)'s centre
const C = (cx, cz) => [cx * 16 + 8, cz * 16 + 8];

const units = {
  ARMCK: { cast: 'builder', footprint: [2, 2], buildTime: 5597, workerTime: 80, buildDistance: 40, maxVelocity: 0.8, canstop: 1,
    canreclamate: 1,
    builds: ['ARMSOLAR', 'ARMWIN', 'ARMESTOR', 'ARMMSTOR', 'ARMMEX', 'ARMMAKR', 'ARMLAB', 'ARMVP', 'ARMAP', 'ARMSY', 'ARMLLT', 'ARMRAD',
      'ARMALAB', 'ARMGEO', 'ARMHLT', 'ARMRL', 'ARMDRAG', 'ARMGUARD', 'ARMHP'],
    name: 'Construction KBot', from: 'ARMCK.fbi; build list: the stock build menu, every button in page order' },
};

/* greenworld's rocks and trees, every figure as its section states it (FeatureDef +0xF0 metal, +0xEC energy, +0xFA
   height, +0xFE bit 7 reclaimable); a rock holds metal alone and a tree energy alone */
const features = {
  rock2: { footprint: [3, 3], metal: 100, height: 20, blocking: true, reclaimable: true, from: 'features\\green\\rocks.tdf [rock2]' },
  rock4: { footprint: [2, 2], metal: 75, height: 20, blocking: true, reclaimable: true, from: 'features\\green\\rocks.tdf [rock4]' },
  rock05: { footprint: [2, 2], metal: 50, height: 20, blocking: true, reclaimable: true, from: 'features\\green\\rocks.tdf [rock05]' },
  rock07: { footprint: [1, 1], metal: 50, height: 10, blocking: true, reclaimable: true, from: 'features\\green\\rocks.tdf [rock07]' },
  rock08: { footprint: [1, 1], metal: 25, height: 10, blocking: true, reclaimable: true, from: 'features\\green\\rocks.tdf [rock08]' },
  tree1: { footprint: [1, 1], energy: 250, height: 40, blocking: true, reclaimable: true, from: 'features\\green\\trees.tdf [tree1]' },
  tree2: { footprint: [1, 1], energy: 250, height: 40, blocking: true, reclaimable: true, from: 'features\\green\\trees.tdf [tree2]' },
  tree3: { footprint: [1, 1], energy: 250, height: 40, blocking: true, reclaimable: true, from: 'features\\green\\trees.tdf [tree3]' },
  tree4: { footprint: [1, 1], energy: 250, height: 40, blocking: true, reclaimable: true, from: 'features\\green\\trees.tdf [tree4]' },
  tree5: { footprint: [1, 1], energy: 250, height: 40, blocking: true, reclaimable: true, from: 'features\\green\\trees.tdf [tree5]' },
};

/* the map, 40 x 30 cells of greenworld grass. Two ARMCK stand left of centre, clear of the popup's columns (cells 0..7,
   which it covers beside RECLAIM's row); a lone rock above them; a field of rocks with three trees among it; a grove of
   trees with two rocks among it, bottom right; plain ground bottom left for the empty ring. */
const F = (name, at, id) => ({ id, name, at });
const scene = {
  map: { w: 40, h: 30, surfaceMetal: 0 },
  units: [
    { id: 'a', type: 'ARMCK', at: [184, 168] },
    { id: 'b', type: 'ARMCK', at: [184, 216] },
  ],
  features: [
    F('rock2', [13, 3], 'lone'),
    // the rock field, round cell (21, 10)
    F('rock4', [18, 8], 'r1'), F('rock4', [22, 6], 'r2'), F('rock4', [21, 11], 'r3'),
    F('rock05', [17, 12], 'r4'), F('rock05', [24, 9], 'r5'),
    F('rock07', [20, 14], 'r6'), F('rock07', [25, 13], 'r7'), F('rock08', [19, 6], 'r8'), F('rock08', [16, 9], 'r9'),
    F('tree1', [23, 9], 't1'), F('tree4', [19, 10], 't2'), F('tree2', [16, 11], 't3'),
    // the grove, round cell (31, 21)
    F('tree1', [28, 19], 'g1'), F('tree2', [30, 18], 'g2'), F('tree3', [32, 19], 'g3'), F('tree4', [29, 21], 'g4'),
    F('tree5', [33, 21], 'g5'), F('tree1', [31, 23], 'g6'), F('tree3', [28, 23], 'g7'), F('tree2', [34, 24], 'g8'),
    F('rock07', [31, 21], 'g9'), F('rock4', [26, 21], 'g10'),
  ],
  selected: ['a', 'b'],
  cursor: [300, 260],
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
    // the trip to RECLAIM (sim.js planReclaim): nothing but a caption stands in its TRIP ticks, and it ends where it began
    reclaim() { inputs.push({ at: t, reclaim: true }); t += TRIP; return T; },
    // the trip to the popup's row of a kind (sim.js pickTrip), PICK ticks, ending where it began
    pick(k) { inputs.push({ at: t, pick: k }); t += PICK; return T; },
    say(s) { inputs.push({ at: t, caption: s }); return T; },
    // the demo's clock stops until no unit of the player's holds a build or a reclaim order, at most `max` ticks
    idle(max) { t += 1; inputs.push({ at: t, until: 'idle', max }); t += 1; return T; },
  };
  return T;
}

const WAIT_MAX = 1800, TAIL = 60;
const demos = [];
function demo(id, title, add, fn, tail = TAIL) {
  const T = TL(add.cursor || scene.cursor);
  fn(T);
  demos.push({ id, title, scene: add, inputs: T.inputs, end: T.t + tail });
}

demo('click', 'E and a click', { selected: ['a'] }, T => T
  .say('Press E, or click RECLAIM: reclaim is armed.').reclaim().wait(10)
  .to(C(14, 4), 40).wait(10).click('lmb').say('A click on a rock reclaims it, as in TA.').idle(WAIT_MAX));

demo('kind', 'Pick the kind', { selected: ['a'] }, T => T
  .say('Reclaim armed: the popup beside RECLAIM picks what a ring takes.').reclaim().wait(30)
  .click('e').say('E again: METAL. The tag at the cursor says so too.').wait(45)
  .click('e').say('ENERGY.').wait(40)
  .click('e').say('ALL, rocks and trees alike.').wait(40)
  .say('Or click a row of the popup.').pick('metal').wait(40), 30);

demo('metal', 'A METAL ring', { kind: 'metal' }, T => T
  .say('Two {builder}s, METAL picked. Press and drag a ring.').reclaim().wait(10)
  .to(C(21, 10), 40).wait(8).down('lmb')
  .to(C(26, 10), 50).say('The rocks in the ring are marked. The trees among them are not.').wait(50)
  .up('lmb').say('Released: the rocks are shared out, each {builder} taking the nearest.').wait(240));

demo('energy', 'An ENERGY ring', { kind: 'energy' }, T => T
  .say('ENERGY picked. Ring the grove.').reclaim().wait(10)
  .to(C(31, 21), 50).wait(8).down('lmb')
  .to(C(35, 22), 50).say('Only the trees are marked: the rocks among them stay.').wait(50)
  .up('lmb').say('Released: the trees are shared out between the two.').wait(560));

demo('shift', 'Shift queues another ring', { kind: 'metal' }, T => T
  .say('Hold Shift to queue: reclaim stays armed after a ring.').reclaim().wait(10)
  .down('shift').to(C(18, 9), 40).wait(8).down('lmb').to(C(21, 9), 35).wait(15).up('lmb')
  .say('One ring queued. Another, still holding Shift...').wait(20)
  .to(C(23, 12), 35).wait(8).down('lmb').to(C(26, 12), 35).wait(15).up('lmb')
  .say('...goes after it.').wait(30)
  .up('shift').say('Let go of Shift: reclaim is put away.').wait(150));

demo('replace', 'Without Shift it replaces', { selected: ['a'], kind: 'metal' }, T => T
  .down('shift').to(C(37, 2), 40).wait(5).click('order').to(C(37, 27), 30).wait(5).click('order')
  .to(C(2, 27), 30).wait(5).click('order').up('shift').say('The {builder} has a route of moves queued.')
  .to([300, 260], 30).reclaim().wait(10)
  .to(C(21, 10), 40).wait(8).down('lmb').to(C(25, 10), 45).wait(20)
  .up('lmb').say('A ring without Shift replaces its orders: the moves are gone, the rocks are its work now.').wait(200));

demo('cancel', 'Cancel', { selected: ['a'] }, T => T
  .say('A {cancel} during the drag ends the ring and keeps reclaim armed.').reclaim().wait(10)
  .to(C(21, 10), 40).wait(8).down('lmb').to(C(25, 10), 40).wait(15)
  .down('cancel').wait(3).up('cancel').wait(10).up('lmb').say('Nothing ordered, still armed.').wait(40)
  .to(C(10, 22), 50).wait(8).down('lmb').to(C(14, 23), 40).wait(10).up('lmb')
  .say('A ring with nothing of the kind in it orders nothing either.').wait(50)
  .down('cancel').wait(3).up('cancel').say('A {cancel} outside a drag puts reclaim away.'), 40);

const entry = {
  format: 2, id: 'reclaim',
  title: 'Reclaim Sweep',
  subtitle: 'With RECLAIM armed, drag a ring to reclaim everything of one kind in it, shared out between your constructors.',
  text: [
    'Select your constructors and press E, or click RECLAIM: reclaim is armed. A {select} on a rock, a tree or a wreck reclaims it, as in TA.',
    'A popup beside RECLAIM picks what a ring takes: METAL (rocks and wrecks), ENERGY (trees) or ALL. Press E again to step through them, or click a row. The tag at the cursor shows the kind, and the game keeps your choice for next time.',
    'Press and drag a ring on the map. Everything of the kind inside it is marked as you drag; features of the other kind are not, and a click on one orders nothing. Release, and the marked features are shared out between your selected constructors, each taking the nearest one left to where it will be.',
    'Without Shift a ring replaces your constructors\' orders, as a click does, but keeps their reclaims of anything in the ring. With Shift it adds to them, and reclaim stays armed until you let go of Shift.',
    'A {cancel} during the drag ends the ring and keeps reclaim armed; a ring with nothing of the kind in it orders nothing and keeps it armed too. A {cancel} outside a drag puts reclaim away.',
    'In the stock game E reaches RECLAIM only while its ORDERS page is up; the mods that show the orders on every page let E work from any page.',
  ],
  lockedIn: [],
  units, features, scenes: { green: scene }, demos,
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
fs.writeFileSync(`${ROOT}art/wiki/entries/reclaim.json`, fmt(entry) + '\n');
console.log(demos.map(d => `${d.id}: ${d.inputs.length} inputs, end ${d.end}`).join('\n'));
