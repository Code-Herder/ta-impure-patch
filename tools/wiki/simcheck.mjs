/* simcheck.mjs -- plays every demo of the wiki's entries through tools/wiki/mockup/sim.js, headless, and checks that
   each one shows what its title claims, on the sim's own state rather than on pictures.

   usage: node tools/wiki/simcheck.mjs [--trace]     exit 0 when every demo and rule passes, 1 otherwise

   A demo is a scene and inputs, so a change to the rules, the figures or the map can quietly turn a demo into one
   that no longer shows its claim; this is what notices. The checks name the entry's deposits by their cell
   rectangles, so moving a deposit in the entry means moving it here too. */
import fs from 'node:fs';
import { createRequire } from 'node:module';
const WikiSim = createRequire(import.meta.url)('./mockup/sim.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const TRACE = process.argv.includes('--trace');

// the quickbuild entry's deposits and vents, in cells: [x, z, w, h]
const DEP = { A: [13, 15, 5, 5], B: [22, 3, 3, 3], E: [3, 10, 3, 3], S: [15, 1, 3, 3], D: [26, 15, 3, 3], R: [29, 19, 3, 3],
  black: [2, 2, 3, 3], grey: [3, 18, 3, 3] };
const VENT = [28, 9];

const foot = u => [Math.round(u.x / 16 - u.fx / 2), Math.round(u.z / 16 - u.fz / 2), u.fx, u.fz];
// how many cells of rectangle r the footprint of u (a view unit, ghost or queued site) covers
function covers(u, r) {
  const [x, z, w, h] = foot(u);
  return Math.max(0, Math.min(x + w, r[0] + r[2]) - Math.max(x, r[0])) * Math.max(0, Math.min(z + h, r[1] + r[3]) - Math.max(z, r[1]));
}
const whole = (u, r) => covers(u, r) === r[2] * r[3];
const holds = (u, [cx, cz]) => covers(u, [cx, cz, 1, 1]) === 1;
const cellIn = (x, z, r) => x >= r[0] * 16 && x < (r[0] + r[2]) * 16 && z >= r[1] * 16 && z < (r[1] + r[3]) * 16;

function run(entry, demo) {
  const sim = WikiSim.play(entry, demo), ids = entry.scene.units.map(u => u.id);
  const frames = [];
  const snap = () => frames.push({ t: sim.tick, v: sim.view(), o: Object.fromEntries(ids.map(id => [id, sim.orders(id)])) });
  snap();
  while (!sim.done) { sim.step(); snap(); }
  return { sim, frames, last: frames[frames.length - 1], log: sim.log };
}
// the first frame after the inputs of tick `at` with this `down`; clicks are where the checks look
const clicks = (demo, k) => demo.inputs.filter(e => e.down === k).map(e => e.at);
const fresh = (v, type) => v.units.filter(u => u.type === type && !['mex', 'geo'].includes(u.id));
const kinds = os => (os || []).map(o => o.k + (o.type ? ':' + o.type : o.target ? ':' + o.target : '')).join(' ');

const CHECKS = {
  snap(r, d, ok) {
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === 'ARMMEX' && whole(f.v.ghost, DEP.B) && !cellIn(f.v.cursor.x, f.v.cursor.z, DEP.B)),
      'the ghost covered deposit B while the cursor was off it');
    const m = fresh(r.last.v, 'ARMMEX');
    ok(m.length === 1 && whole(m[0], DEP.B) && m[0].built === 1, `ARMMEX built on B (${m.map(u => foot(u))})`);
  },
  alt(r, d, ok) {
    const held = r.frames.filter(f => f.v.keys.includes('alt') && f.v.ghost);
    ok(held.length && held.every(f => covers(f.v.ghost, DEP.B) === 0), 'with Alt held the ghost stayed off the deposit');
    ok(r.frames.some(f => !f.v.keys.includes('alt') && f.v.ghost && whole(f.v.ghost, DEP.B)), 'without Alt it snapped onto B');
    const m = fresh(r.last.v, 'ARMMEX');
    ok(m.length === 1 && covers(m[0], DEP.B) === 0 && m[0].built === 1, `ARMMEX built where clicked, off B (${m.map(u => foot(u))})`);
  },
  click(r, d, ok) {
    ok(r.frames.every(f => f.v.mode === 'contextual'), 'no build menu used');
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === 'ARMMEX' && whole(f.v.ghost, DEP.B)), 'hovering B showed the ARMMEX ghost');
    const m = fresh(r.last.v, 'ARMMEX');
    ok(m.length === 1 && whole(m[0], DEP.B) && m[0].built === 1, 'the right-click built ARMMEX on B');
  },
  bestfit(r, d, ok) {
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === 'ARMMOHO' && whole(f.v.ghost, DEP.A)), 'over A the ghost is ARMMOHO');
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === 'ARMMEX' && whole(f.v.ghost, DEP.S)), 'over the wedged deposit S the ghost is ARMMEX');
    const [c1, c2] = clicks(d, 'rmb'), f1 = r.frames[c1 + 1], f2 = r.frames[c2 + 1];
    ok(kinds(f1.o.ck) === 'build:ARMMEX' && kinds(f1.o.ack) === 'guard:ck', `first click: ck ${kinds(f1.o.ck)}, ack ${kinds(f1.o.ack)}`);
    ok(kinds(f2.o.ack) === 'build:ARMMOHO' && kinds(f2.o.ck) === 'guard:ack', `second click: ack ${kinds(f2.o.ack)}, ck ${kinds(f2.o.ck)}`);
    const mex = fresh(r.last.v, 'ARMMEX'), moho = fresh(r.last.v, 'ARMMOHO');
    ok(mex.length === 1 && whole(mex[0], DEP.S) && mex[0].built === 1, 'ARMMEX built on S');
    ok(moho.length === 1 && whole(moho[0], DEP.A) && moho[0].built === 1, 'ARMMOHO built on A');
  },
  queue(r, d, ok) {
    const n = Math.max(...r.frames.map(f => (f.o.ck || []).filter(o => o.k === 'build').length));
    ok(n === 3, `ARMCK held three queued builds (${n})`);
    ok(r.frames.every(f => f.v.mode === 'contextual'), 'the mode stayed on, no build menu');
    const after = r.frames[clicks(d, 'rmb')[2] + 1];
    ok(after.v.queued.length + fresh(after.v, 'ARMMEX').length === 3, 'three sites queued or started after the third click');
    ok(fresh(r.last.v, 'ARMMEX').filter(u => u.built === 1).length >= 2, 'ARMCK worked through them in order');
  },
  cancel(r, d, ok) {
    const [q, plain, c] = clicks(d, 'rmb').map(t => r.frames[t + 1]);
    ok(kinds(q.o.ck) === 'build:ARMMEX' && q.v.queued.length === 1, 'Shift+right-click queued it');
    ok(JSON.stringify(plain.o.ck) === JSON.stringify(q.o.ck), 'a plain repeat click left the queue unchanged');
    ok(c.o.ck.length === 0 && c.v.queued.length === 0, 'Shift+right-click on the site cancelled it');
    ok(fresh(r.last.v, 'ARMMEX').length === 0 && r.last.o.ck.length === 0, 'nothing built, queue still empty at the end');
  },
  helpers(r, d, ok) {
    const f = r.frames[clicks(d, 'rmb')[1] + 1];
    ok(f.v.units.filter(u => u.selected).length === 3, 'the box selected all three');
    ok(kinds(f.o.ack) === 'build:ARMMOHO', `ARMACK builds the Moho (${kinds(f.o.ack)})`);
    ok(kinds(f.o.ck) === 'guard:ack', `ARMCK guards ARMACK (${kinds(f.o.ck)})`);
    ok(kinds(f.o.pw) === 'move', `the Peewee kept its move (${kinds(f.o.pw)})`);
    const moho = fresh(r.last.v, 'ARMMOHO');
    ok(moho.length === 1 && whole(moho[0], DEP.A) && moho[0].built === 1, 'the Moho was built on A');
    ok(r.frames.some(x => x.v.lines.some(l => l.kind === 'guard')), 'a guard line was drawn');
  },
  upgrade(r, d, ok) {
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === 'ARMMOHO' && whole(f.v.ghost, DEP.D)), 'over the ARMMEX the ghost is ARMMOHO');
    const f = r.frames[clicks(d, 'rmb')[0] + 1];
    ok(kinds(f.o.ack) === 'reclaim:mex build:ARMMOHO', `the order is reclaim then build (${kinds(f.o.ack)})`);
    const gone = r.log.find(e => e.what === 'reclaimed' && e.id === 'mex'), start = r.log.find(e => e.what === 'started' && e.type === 'ARMMOHO');
    ok(gone && start && gone.t <= start.t, 'the ARMMEX was reclaimed before the Moho started');
    const moho = fresh(r.last.v, 'ARMMOHO');
    ok(moho.length === 1 && whole(moho[0], DEP.D) && moho[0].built === 1 && !r.last.v.units.some(u => u.id === 'mex'), 'ARMMOHO built in its place');
  },
  geo(r, d, ok) {
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === 'ARMGEO' && holds(f.v.ghost, VENT)), 'over the vent the ghost is ARMGEO');
    const g = fresh(r.last.v, 'ARMGEO');
    ok(g.length === 1 && holds(g[0], VENT) && g[0].built === 1, 'ARMGEO built on the vent');
  },
  metalmap(r, d, ok) {
    const [c1, c2] = clicks(d, 'rmb'), f1 = r.frames[c1], f2 = r.frames[c2];
    ok(!f1.v.ghost && f1.v.cursor.shape === 'move', 'over broad metal ground: no ghost, a move cursor');
    ok(kinds(r.frames[c1 + 1].o.ck) === 'move', `the click there is a move (${kinds(r.frames[c1 + 1].o.ck)})`);
    ok(f2.v.ghost && f2.v.ghost.type === 'ARMMEX' && whole(f2.v.ghost, DEP.R), 'over the separate deposit: the ARMMEX ghost');
    const m = fresh(r.last.v, 'ARMMEX');
    ok(m.length === 1 && whole(m[0], DEP.R) && m[0].built === 1, 'ARMMEX built on the separate deposit');
  },
  fog(r, d, ok) {
    const near = r.frames.filter(f => f.v.ghost && Math.abs(f.v.cursor.x - 104) <= 2 && Math.abs(f.v.cursor.z - 88) <= 2);
    ok(near.length && near.every(f => covers(f.v.ghost, DEP.black) === 0 && f.v.map.fog[2 * 40 + 2] === 2), 'beside the unexplored deposit the ghost did not snap');
    const f = r.frames[clicks(d, 'lmb')[0]];
    ok(f.v.ghost && whole(f.v.ghost, DEP.grey) && f.v.map.fog[19 * 40 + 4] === 1, 'beside the grey-fogged deposit it snapped onto it');
    const o = r.frames[clicks(d, 'lmb')[0] + 1].o.ck;
    ok(o.length === 1 && o[0].type === 'ARMMEX' && whole({ x: (o[0].ox + 1.5) * 16, z: (o[0].oz + 1.5) * 16, fx: 3, fz: 3 }, DEP.grey), 'the order is on the grey deposit');
  },
};

/* rules no demo shows, checked on the quickbuild entry's scene: the classifier compares with SurfaceMetal alone
   (tagpu_resource.c distinct), so a richer patch joined to broad metal ground is refused like the ground; and only a
   played demo ends */
const RULES = {
  'a richer patch inside the broad metal band is not a deposit'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'], metal: [{ rect: [35, 10, 3, 3], v: 200 }] } });
    s.input({ move: [36 * 16 + 8, 11 * 16 + 8] });
    return !s.view().ghost && s.view().cursor.shape === 'move';
  },
  'a Try-it sim is never done'(entry) {
    const s = WikiSim.create(entry, entry.demos[0]);
    for (let i = 0; i <= entry.demos[0].end + 10; i++) s.step();
    return s.done === false;
  },
};

const tree = JSON.parse(fs.readFileSync(`${ROOT}art/wiki/tree.json`, 'utf8'));
const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
let failed = 0;
for (const id of tree.tree.flatMap(leaves)) {
  const entry = JSON.parse(fs.readFileSync(`${ROOT}art/wiki/entries/${id}.json`, 'utf8'));
  for (const d of entry.demos) {
    const notes = [], ok = (c, what) => notes.push([!!c, what]);
    try {
      const r = run(entry, d);
      if (TRACE) console.log(`  ${d.id} log:`, r.log.map(e => `${e.t} ${e.what} ${e.type}@${e.ox},${e.oz}`).join('; '));
      const check = CHECKS[d.id];
      if (!check) ok(false, 'no check for this demo');
      else check(r, d, ok);
    } catch (e) { ok(false, `threw: ${e.stack}`); }
    const pass = notes.every(n => n[0]);
    if (!pass) failed++;
    console.log(`${pass ? 'PASS' : 'FAIL'} ${id}/${d.id} -- ${d.title}`);
    for (const [c, what] of notes) if (!c || TRACE) console.log(`     ${c ? 'ok  ' : 'FAIL'} ${what}`);
  }
}
{
  const entry = JSON.parse(fs.readFileSync(`${ROOT}art/wiki/entries/quickbuild.json`, 'utf8'));
  for (const [what, check] of Object.entries(RULES)) {
    let pass = false;
    try { pass = check(entry); } catch (e) { console.log(e.stack); }
    if (!pass) failed++;
    console.log(`${pass ? 'PASS' : 'FAIL'} rule -- ${what}`);
  }
}
console.log(failed ? `${failed} check(s) failed` : 'all demos and rules pass');
process.exit(failed ? 1 : 0);
