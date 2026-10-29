/* simcheck.mjs -- plays every demo of the wiki's entries through tools/wiki/mockup/sim.js, headless, and checks that
   each one shows what its title claims, on the sim's own state rather than on pictures.

   usage: node tools/wiki/simcheck.mjs [--pack DIR] [--null-layout] [--casting NAME|none] [--side N] [--iface 0|1]
                                       [--no-scripts] [--trace]
     exit 0 when every demo and rule passes, 1 otherwise

   The demos play with the sidebar of art/wiki/side/fixture.json, the layout the wiki falls back to, so each `build`
   input plays out as its trip through the sidebar (sim.js trip) and is checked as one; --null-layout plays them with
   no sidebar, where a `build` arms at once. Each demo plays in every context: the entry's own types and each casting
   of tools/wiki/casting-fixture.json (identity, shifted, unbound), each at Interface Type 1 and 0; --casting and
   --iface keep one of each. A demo is checked on its claims in every context, except where the casting leaves one of
   its casts unbound: there it must stop under the badge on the tick decision 12 names, and only that is checked. With
   `identity` every demo must play exactly as with the entry's own types. The rule cases name their layout and
   context themselves.

   THE UNITS' SCRIPTS: when gfx packs built by tools/wiki/gfx.py hold them (tools/wiki/mockup/gfx/, gitignored, read
   from the game), every context is played again with them, through serve.py's host (cobhost.mjs), as the game's wiki
   plays its demos; --no-scripts keeps the plays without. Under --pack each type's script comes from the mod's own
   stage packs first (DIR/stage/, which sidepacks.py writes), then the gfx packs. Every play must end on its own end,
   never stopped by a wait that ran past its max (sim.js UNTILS), and with scripts no type may be quarantined (the
   host's notes).

   --pack DIR plays the demos in a mod's pack instead (tools/wiki/sidepacks.py writes them to the gitignored
   tools/wiki/mockup/gfx/side/<mod>/; DIR holds its side.json): with the pack's layout, one context per layout side
   with that side's casting, at Interface Type 1 and 0 (--side and --iface keep one). Every claim that names a unit
   type names the type the context casts in that unit's place (`r.ty`), a claim that reads the solar collector's
   cells reads the cast's footprint (`r.fp`: the sites over it and its ring), and the trip's pages are the ones the
   pack's layout holds for the cast type (packPath). The rule cases are not played: they name the fixture's layout
   and castings, which a pack does not change, so the plain run is where they are checked.

   Imported, it runs nothing and exports ENTRY_RULES, the rule cases per entry id, which call WikiSim.create and
   WikiSim.play through the module object sim.js exports; tools/wiki/simparity.mjs records both, each with its
   layout, and plays them through the DLL's sim that way.

   A demo is a scene and inputs, so a change to the rules, the figures or the map can quietly turn a demo into one
   that no longer shows its claim; this is what notices. The checks name the entry's deposits by their cell
   rectangles, so moving a deposit in the entry means moving it here too. */
import fs from 'node:fs';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import { cobHost, scripts, sounds, soundSource } from './cobhost.mjs';
const WikiSim = createRequire(import.meta.url)('./mockup/sim.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const TRACE = process.argv.includes('--trace');
const FIXTURE = JSON.parse(fs.readFileSync(`${ROOT}art/wiki/side/fixture.json`, 'utf8'));
const CASTINGS = JSON.parse(fs.readFileSync(`${ROOT}tools/wiki/casting-fixture.json`, 'utf8')).castings;
const { TRIP, TRIP_DOWN, TRIP_UP } = WikiSim;
const arg = k => { const i = process.argv.indexOf(k); return i >= 0 ? process.argv[i + 1] : undefined; };
/* the pack under --pack (its side.json, whose layout and castings the demos play with) and the layout the demos play
   in: set by main() from the command line, so that importing this module for ENTRY_RULES reads no argument of the
   importer's */
let PACK = null, PACK_DIR = null, LAYOUT = FIXTURE;
const ifaces = () => { const i = arg('--iface'); return i === undefined ? [1, 0] : [Number(i)]; };
/* the contexts every demo plays in: {label, layout, ctx, casting name}. The fixture layout has one side, which each
   fixture casting answers; a pack gives one context per layout side, with that side's casting (index-aligned with
   layout.sides), and interface. */
function contexts() {
  const cs = baseContexts();
  if (process.argv.includes('--no-scripts')) return cs;
  const subdirs = d => fs.existsSync(d) ? fs.readdirSync(d).sort().map(x => `${d}/${x}`) : [];
  const packs = [...(PACK_DIR ? subdirs(`${PACK_DIR}/stage`) : []), ...subdirs(`${ROOT}tools/wiki/mockup/gfx`)];
  if (!Object.keys(scripts(packs)).length) return cs;
  const host = cobHost();
  return [...cs, ...cs.map(c => ({ ...c, label: `${c.label}+scripts`, ctx: { ...c.ctx, cob: host.forPacks(packs), snd: soundSource(packs) } }))];
}
function baseContexts() {
  if (PACK) {
    const want = arg('--side'), sides = PACK.layout.sides.map((s, k) => k).filter(k => want === undefined || k === Number(want));
    if (PACK.casting.length !== PACK.layout.sides.length) throw new Error(`the pack has ${PACK.casting.length} castings for ${PACK.layout.sides.length} sides`);
    if (!sides.length) throw new Error(`no side ${want} in the pack`);
    return sides.flatMap(k => ifaces().map(i => ({ label: `${PACK.layout.sides[k].name}/${i}`, name: null, layout: LAYOUT,
      ctx: { side: k, casting: PACK.casting[k], iface: i } })));
  }
  const pick = arg('--casting');
  const names = pick === undefined ? [null, ...Object.keys(CASTINGS)] : [pick === 'none' ? null : pick];
  for (const n of names) if (n !== null && !CASTINGS[n]) throw new Error(`no casting ${n} in casting-fixture.json`);
  return names.flatMap(n => ifaces().map(i => ({ label: `${n || 'authored'}/${i}`, name: n, layout: LAYOUT,
    ctx: { side: 0, casting: n ? CASTINGS[n][0] : null, iface: i } })));
}

// the quickbuild entry's deposits and vents, in cells: [x, z, w, h]
const DEP = { A: [13, 15, 5, 5], B: [22, 3, 3, 3], E: [3, 10, 3, 3], S: [15, 1, 3, 3], D: [26, 15, 3, 3],
  black: [2, 2, 3, 3], grey: [3, 18, 3, 3] };
const VENT = [28, 9];

/* the entry's first scene, every demo's that names no `on`, and the entry with that scene edited by `f` (`alone`: the
   only scene left, for an edit to its map size, which every other scene must share) */
const first = e => e.scenes[Object.keys(e.scenes)[0]];
const withFirst = (e, f, alone = false) => ({ ...e, scenes: { ...(alone ? {} : e.scenes), [Object.keys(e.scenes)[0]]: f(first(e)) } });

const foot = u => [Math.round(u.x / 16 - u.fx / 2), Math.round(u.z / 16 - u.fz / 2), u.fx, u.fz];
// how many cells of rectangle r the footprint of u (a view unit, ghost or queued site) covers
function covers(u, r) {
  const [x, z, w, h] = foot(u);
  return Math.max(0, Math.min(x + w, r[0] + r[2]) - Math.max(x, r[0])) * Math.max(0, Math.min(z + h, r[1] + r[3]) - Math.max(z, r[1]));
}
const whole = (u, r) => covers(u, r) === r[2] * r[3];
const holds = (u, [cx, cz]) => covers(u, [cx, cz, 1, 1]) === 1;
const cellIn = (x, z, r) => x >= r[0] * 16 && x < (r[0] + r[2]) * 16 && z >= r[1] * 16 && z < (r[1] + r[3]) * 16;

/* the type the context casts in place of the entry's unit `t`, and that type's footprint: the casting's for a bound
   cast, else t's own (no casting, or the cast unbound), as sim.js binder renames them */
const castOf = (entry, casting, t) => casting && entry.units[t] ? casting.casts[entry.units[t].cast] : null;
const typeIn = (entry, casting) => t => { const k = castOf(entry, casting, t); return k ? k.type : t; };
const footprintIn = (entry, casting) => t => { const k = castOf(entry, casting, t); return k ? k.footprint : entry.units[t].footprint; };
/* A play's frames: `all`, one a tick, and `frames`, indexed by the demo's clock -- the tick its inputs are written in --
   each the frame of the tick the clock reached that value, so frames[t] shows the inputs of t and a wait's held ticks
   are in `all` alone. Without a wait the two are the same. */
function run(entry, demo, layout = LAYOUT, ctx) {
  const sim = WikiSim.play(entry, demo, layout, ctx), ids = [...WikiSim.sceneOf(entry, demo).units, ...((demo.scene && demo.scene.units) || [])].map(u => u.id);
  const all = [], frames = [];
  const snap = () => {
    const f = { t: sim.tick, v: sim.view(), o: Object.fromEntries(ids.map(id => [id, sim.orders(id)])) };
    all.push(f);
    if (!(f.v.clock in frames)) frames[f.v.clock] = f;
  };
  snap();
  while (!sim.done) { sim.step(); snap(); }
  return { sim, all, frames, last: all[all.length - 1], log: sim.log, layout: all[0].v.side ? layout : null, iface: sim.iface,
    ty: typeIn(entry, ctx && ctx.casting), fp: footprintIn(entry, ctx && ctx.casting) };
}
// the tick a build input's building is armed on: its own without the sidebar, the trip's release with it
const armedAt = (r, e) => e.at + (r.layout ? TRIP_UP : 0);
// the ticks of the presses of k, an action or a button, in the data's order
const presses = (demo, k) => demo.inputs.filter(e => e.down === k).map(e => e.at);
/* the ticks the clicks of action k land on: an `order` lands at its press at type 1 and at its release at type 0
   (sim.js leftClick); every other press acts on its press. A check reads the frame after it. */
const lands = (r, demo, k) => k !== 'order' || r.iface ? presses(demo, k)
  : presses(demo, k).map(t => demo.inputs.find(e => e.up === k && e.at >= t).at);
const fresh = (v, type) => v.units.filter(u => u.type === type && !['mex', 'geo'].includes(u.id));
const kinds = os => (os || []).map(o => o.k + (o.type ? ':' + o.type : o.target ? ':' + o.target : '')).join(' ');

const QUICKBUILD = {
  snap(r, d, ok) {
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === r.ty('ARMMEX') && whole(f.v.ghost, DEP.B) && !cellIn(f.v.cursor.x, f.v.cursor.z, DEP.B)),
      'the ghost covered deposit B while the cursor was off it');
    const m = fresh(r.last.v, r.ty('ARMMEX'));
    ok(m.length === 1 && whole(m[0], DEP.B) && m[0].built === 1, `${r.ty('ARMMEX')} built on B (${m.map(u => foot(u))})`);
  },
  alt(r, d, ok) {
    const held = r.frames.filter(f => f.v.keys.includes('alt') && f.v.ghost);
    ok(held.length && held.every(f => covers(f.v.ghost, DEP.B) === 0), 'with Alt held the ghost stayed off the deposit');
    ok(r.frames.some(f => !f.v.keys.includes('alt') && f.v.ghost && whole(f.v.ghost, DEP.B)), 'without Alt it snapped onto B');
    const m = fresh(r.last.v, r.ty('ARMMEX'));
    ok(m.length === 1 && covers(m[0], DEP.B) === 0 && m[0].built === 1, `${r.ty('ARMMEX')} built where clicked, off B (${m.map(u => foot(u))})`);
  },
  click(r, d, ok) {
    ok(r.frames.every(f => f.v.mode === 'contextual'), 'no build menu used');
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === r.ty('ARMMEX') && whole(f.v.ghost, DEP.B)), `hovering B showed the ${r.ty('ARMMEX')} ghost`);
    const m = fresh(r.last.v, r.ty('ARMMEX'));
    ok(m.length === 1 && whole(m[0], DEP.B) && m[0].built === 1, `the right-click built ${r.ty('ARMMEX')} on B`);
  },
  bestfit(r, d, ok) {
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === r.ty('ARMMOHO') && whole(f.v.ghost, DEP.A)), `over A the ghost is ${r.ty('ARMMOHO')}`);
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === r.ty('ARMMEX') && whole(f.v.ghost, DEP.S)), `over the wedged deposit S the ghost is ${r.ty('ARMMEX')}`);
    const [c1, c2] = lands(r, d, 'order'), f1 = r.frames[c1 + 1], f2 = r.frames[c2 + 1];
    ok(kinds(f1.o.ck) === `build:${r.ty('ARMMEX')}` && kinds(f1.o.ack) === 'guard:ck', `first click: ck ${kinds(f1.o.ck)}, ack ${kinds(f1.o.ack)}`);
    ok(kinds(f2.o.ack) === `build:${r.ty('ARMMOHO')}` && kinds(f2.o.ck) === 'guard:ack', `second click: ack ${kinds(f2.o.ack)}, ck ${kinds(f2.o.ck)}`);
    const mex = fresh(r.last.v, r.ty('ARMMEX')), moho = fresh(r.last.v, r.ty('ARMMOHO'));
    ok(mex.length === 1 && whole(mex[0], DEP.S) && mex[0].built === 1, `${r.ty('ARMMEX')} built on S`);
    ok(moho.length === 1 && whole(moho[0], DEP.A) && moho[0].built === 1, `${r.ty('ARMMOHO')} built on A`);
  },
  queue(r, d, ok) {
    const n = Math.max(...r.frames.map(f => (f.o.ck || []).filter(o => o.k === 'build').length));
    ok(n === 3, `${r.ty('ARMCK')} held three queued builds (${n})`);
    ok(r.frames.every(f => f.v.mode === 'contextual'), 'the mode stayed on, no build menu');
    const after = r.frames[lands(r, d, 'order')[2] + 1];
    ok(after.v.queued.length + fresh(after.v, r.ty('ARMMEX')).length === 3, 'three sites queued or started after the third click');
    ok(fresh(r.last.v, r.ty('ARMMEX')).filter(u => u.built === 1).length >= 2, `${r.ty('ARMCK')} worked through them in order`);
  },
  cancel(r, d, ok) {
    const [q, plain, c] = lands(r, d, 'order').map(t => r.frames[t + 1]);
    ok(kinds(q.o.ck) === `build:${r.ty('ARMMEX')}` && q.v.queued.length === 1, 'Shift+right-click queued it');
    ok(JSON.stringify(plain.o.ck) === JSON.stringify(q.o.ck), 'a plain repeat click left the queue unchanged');
    ok(c.o.ck.length === 0 && c.v.queued.length === 0, 'Shift+right-click on the site cancelled it');
    ok(fresh(r.last.v, r.ty('ARMMEX')).length === 0 && r.last.o.ck.length === 0, 'nothing built, queue still empty at the end');
  },
  helpers(r, d, ok) {
    const f = r.frames[lands(r, d, 'order')[1] + 1];
    ok(f.v.units.filter(u => u.selected).length === 3, 'the box selected all three');
    ok(kinds(f.o.ack) === `build:${r.ty('ARMMOHO')}`, `${r.ty('ARMACK')} builds the Moho (${kinds(f.o.ack)})`);
    ok(kinds(f.o.ck) === 'guard:ack', `${r.ty('ARMCK')} guards ${r.ty('ARMACK')} (${kinds(f.o.ck)})`);
    ok(kinds(f.o.pw) === 'move', `the Peewee kept its move (${kinds(f.o.pw)})`);
    const moho = fresh(r.last.v, r.ty('ARMMOHO'));
    ok(moho.length === 1 && whole(moho[0], DEP.A) && moho[0].built === 1, 'the Moho was built on A');
    ok(r.frames.some(x => x.v.lines.some(l => l.kind === 'guard')), 'a guard line was drawn');
  },
  upgrade(r, d, ok) {
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === r.ty('ARMMOHO') && whole(f.v.ghost, DEP.D)), `over the ${r.ty('ARMMEX')} the ghost is ${r.ty('ARMMOHO')}`);
    const f = r.frames[lands(r, d, 'order')[0] + 1];
    ok(kinds(f.o.ack) === `reclaim:mex build:${r.ty('ARMMOHO')}`, `the order is reclaim then build (${kinds(f.o.ack)})`);
    const gone = r.log.find(e => e.what === 'reclaimed' && e.id === 'mex'), start = r.log.find(e => e.what === 'started' && e.type === r.ty('ARMMOHO'));
    ok(gone && start && gone.t <= start.t, `the ${r.ty('ARMMEX')} was reclaimed before the Moho started`);
    const moho = fresh(r.last.v, r.ty('ARMMOHO'));
    ok(moho.length === 1 && whole(moho[0], DEP.D) && moho[0].built === 1 && !r.last.v.units.some(u => u.id === 'mex'), `${r.ty('ARMMOHO')} built in its place`);
  },
  geo(r, d, ok) {
    ok(r.frames.some(f => f.v.ghost && f.v.ghost.type === r.ty('ARMGEO') && holds(f.v.ghost, VENT)), `over the vent the ghost is ${r.ty('ARMGEO')}`);
    const g = fresh(r.last.v, r.ty('ARMGEO'));
    ok(g.length === 1 && holds(g[0], VENT) && g[0].built === 1, `${r.ty('ARMGEO')} built on the vent`);
  },
  metalmap(r, d, ok) {
    const [c1] = presses(d, 'order'), f1 = r.frames[c1], l1 = lands(r, d, 'order')[0];
    ok(f1.v.map.metal.every(v => v === f1.v.map.surfaceMetal && v > 0), 'the metal scene: metal ground everywhere');
    ok(!f1.v.ghost && f1.v.cursor.shape === 'move', 'over metal ground: no ghost, a move cursor');
    ok(kinds(r.frames[l1 + 1].o.ck) === 'move', `the click there is a move (${kinds(r.frames[l1 + 1].o.ck)})`);
    // the demo waits for the walk instead of ending on a timed tail: the constructor has stopped at its place
    const ck = r.last.v.units.find(u => u.id === 'ck'), mv = r.frames[l1 + 1].o.ck[0];
    ok(r.last.o.ck.length === 0 && Math.hypot(ck.x - mv.x, ck.z - mv.z) <= 32,
      `${r.ty('ARMCK')} had arrived by the end (${Math.round(ck.x)}, ${Math.round(ck.z)})`);
  },
  fog(r, d, ok) {
    const near = r.frames.filter(f => f.v.ghost && Math.abs(f.v.cursor.x - 104) <= 2 && Math.abs(f.v.cursor.z - 88) <= 2);
    ok(near.length && near.every(f => covers(f.v.ghost, DEP.black) === 0 && f.v.map.fog[2 * 40 + 2] === 2), 'beside the unexplored deposit the ghost did not snap');
    const f = r.frames[presses(d, 'lmb')[0]];
    ok(f.v.ghost && whole(f.v.ghost, DEP.grey) && f.v.map.fog[19 * 40 + 4] === 1, 'beside the grey-fogged deposit it snapped onto it');
    const o = r.frames[presses(d, 'lmb')[0] + 1].o.ck;
    ok(o.length === 1 && o[0].type === r.ty('ARMMEX') && whole({ x: (o[0].ox + 1.5) * 16, z: (o[0].oz + 1.5) * 16, fx: 3, fz: 3 }, DEP.grey), 'the order is on the grey deposit');
  },
};

/* rules no quickbuild demo shows, checked on its entry's scenes: the classifier compares with SurfaceMetal alone
   (tagpu_resource.c distinct), so a richer patch joined to broad metal ground, a band from edge to edge, is refused
   like the ground; and only a played demo ends */
const RULES = {
  'a richer patch inside a broad metal band is not a deposit'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'],
      metal: [{ rect: [34, 0, 6, 30], v: 40 }, { rect: [35, 10, 3, 3], v: 200 }] } });
    s.input({ move: [36 * 16 + 8, 11 * 16 + 8] });
    return !s.view().ghost && s.view().cursor.shape === 'move';
  },
  /* the cursor's animation (tagpu_wikisim.h): it begins on the first tick that shows its sequence -- for a change the
     player makes between two ticks, the tick the sim stands at -- and arming a building over the arrow installs no
     cursor, both being cursornormal (0x499254, 0x4992A9), so the arrow's animation runs on */
  'a cursor the player changes between ticks begins on the tick the view shows'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'] } });
    for (let i = 0; i < 3; i++) s.step();
    const before = s.view().cursor;
    s.input({ build: 'ARMMEX' });
    const now = s.view().cursor; s.step(); const next = s.view().cursor;
    return before.shape === 'move' && now.shape === 'build' && now.since === 3 && next.since === 3;
  },
  'arming a building over the arrow runs the arrow on'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: [] } });
    for (let i = 0; i < 3; i++) s.step();
    s.input({ build: 'ARMMEX' }); s.step();
    const armed = s.view().cursor;
    s.input({ build: null }); s.step();
    const back = s.view().cursor;
    return armed.shape === 'build' && armed.since === 0 && back.shape === 'arrow' && back.since === 0;
  },
  'a Try-it sim is never done'(entry) {
    const s = WikiSim.create(entry, entry.demos[0]);
    for (let i = 0; i <= entry.demos[0].end + 10; i++) s.step();
    return s.done === false;
  },
  /* tagpu_place.c's PLACE_LIST_CAP holds for every append, not only a gesture's: a Shift move, a Shift Quick Build
     and a Shift assist each stop at 1024 orders, and an upgrade's reclaim and build go in together or not at all */
  'every Shift append stops at 1024 orders'(entry) {
    // each sim is done with before the next is made: simparity.mjs replays one at a time
    const full = (id, n, extra = {}) => WikiSim.create(withFirst(entry, g => ({ ...g,
      units: g.units.map(u => u.id !== id ? u : { ...u, orders: Array.from({ length: n }, () => ({ move: [304, 176] })) }) })),
      { scene: { selected: [id], ...extra } });
    const rmb = (s, at) => { s.input({ move: at }); s.input({ down: 'shift' }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' }); s.input({ up: 'shift' }); };
    const last = s => s.orders(s.view().units.find(u => u.selected).id).slice(-1)[0];
    const n = s => s.orders(s.view().units.find(u => u.selected).id).length;
    // over plain ground: no Quick Build choice, a move
    const move = full('ck', 1023);
    rmb(move, [568, 184]); const m1 = n(move) === 1024 && last(move).k === 'move' && last(move).x === 568;
    rmb(move, [584, 200]); const m2 = n(move) === 1024 && last(move).x === 568;
    // over deposit B: Quick Build's ARMMEX
    const quick = full('ck', 1023);
    rmb(quick, [376, 72]); const q1 = n(quick) === 1024 && last(quick).k === 'build' && last(quick).type === 'ARMMEX';
    // over an unfinished extractor of the player's: assist it
    const assist = full('ck', 1023, { units: [{ id: 'half', type: 'ARMMEX', at: [120, 248], built: 0.5 }] });
    rmb(assist, [120, 248]); const a1 = n(assist) === 1024 && last(assist).k === 'build';
    rmb(assist, [120, 248]); const a2 = n(assist) === 1024;
    // over the finished extractor: ARMACK's upgrade, two orders
    const tight = full('ack', 1023);
    rmb(tight, [440, 264]); const u1 = n(tight) === 1023 && last(tight).k === 'move';
    const roomy = full('ack', 1022);
    rmb(roomy, [440, 264]); const u2 = n(roomy) === 1024 && kinds(roomy.orders('ack').slice(-2)) === 'reclaim:mex build:ARMMOHO';
    return m1 && m2 && q1 && a1 && a2 && u1 && u2;
  },
  /* the heading (research/notes/ingame-wiki.md, "The simulation's contract"): a unit vector, turned at once, computed
     with + - * / and sqrt only. Each expectation is the rule's own arithmetic, so it is exact. */
  'a step faces its direction'(entry) {
    // every step of a move along a clear straight line faces from where it starts to the target, and the tick it
    // arrives on (no step) keeps the last step's facing
    const s = WikiSim.create(entry, { scene: { selected: ['pw'] } }), pw = () => s.view().units.find(u => u.id === 'pw');
    let p = pw(), ok = p.hx === 0 && p.hz === 1, steps = 0, arrived = 0;
    s.input({ move: [100, 60] }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' });
    while (s.orders('pw').length && steps < 400) {
      const dx = 100 - p.x, dz = 60 - p.z, d = Math.sqrt(dx * dx + dz * dz);
      s.step(); steps++;
      const q = pw();
      if (d > 0) ok = ok && q.hx === dx / d && q.hz === dz / d;
      else { arrived++; ok = ok && q.hx === p.hx && q.hz === p.hz; }
      p = q;
    }
    return ok && steps > 100 && arrived === 1 && p.x === 100 && p.z === 60;
  },
  'a builder in range faces its target'(entry) {
    // ARMCK stands 32 world units from the unfinished extractor's footprint, inside its BuildDistance of 40
    const s = WikiSim.create(entry, { scene: { selected: ['b'], units: [{ id: 'half', type: 'ARMMEX', at: [120, 248], built: 0.5 },
      { id: 'b', type: 'ARMCK', at: [170, 290] }] } });
    const b = () => s.view().units.find(u => u.id === 'b'), dx = 120 - 170, dz = 248 - 290, d = Math.sqrt(dx * dx + dz * dz);
    s.input({ move: [120, 248] }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' });
    const before = b();
    s.step();
    const after = b(), half = s.view().units.find(u => u.id === 'half');
    return before.hx === 0 && before.hz === 1 && after.x === 170 && after.z === 290 && half.built > 0.5 &&
      after.hx === dx / d && after.hz === dz / d;
  },
  'a building faces (0, 1)'(entry) {
    // a scene's facing turns a mobile unit only
    const s = WikiSim.create(entry, { scene: { selected: ['ck'], units: [{ id: 'turned', type: 'ARMMEX', at: [120, 248], facing: [1, 0] }] } });
    s.input({ move: [376, 72] }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' });
    let raised = null;
    for (let i = 0; i < 400 && !raised; i++) { s.step(); raised = fresh(s.view(), 'ARMMEX').find(u => u.id !== 'turned'); }
    const turned = s.view().units.find(u => u.id === 'turned'), scene = s.view().units.filter(u => u.id === 'geo' || u.id === 'mex');
    return !!raised && raised.hx === 0 && raised.hz === 1 && turned.hx === 0 && turned.hz === 1 &&
      scene.length === 2 && scene.every(u => u.hx === 0 && u.hz === 1);
  },
  "a scene's facing is normalised"(entry) {
    const s = WikiSim.create(entry, { scene: { units: [{ id: 'f', type: 'ARMPW', at: [200, 200], facing: [-2, 7] }] } });
    const f = s.view().units.find(u => u.id === 'f'), d = Math.sqrt(53);
    return f.hx === -2 / d && f.hz === 7 / d;
  },
  /* MOVEMENT (research/notes/ingame-wiki.md, "How units move"): bodies never overlap, a unit walks round what stands
     in its way, and a unit whose place is taken stops close by. Each case also holds every tick to `apart`. */
  'a unit walks round a structure in its way'(entry) {
    // the Peewee's straight line to the far side of the geothermal plant crosses it
    const s = WikiSim.create(entry, { scene: { selected: ['pw'] } }), pw = () => s.view().units.find(u => u.id === 'pw');
    s.input({ move: [272, 32] }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' });
    let ok = apart(s.view()), beside = false, t = 0;
    for (; t < 600 && s.orders('pw').length; t++) {
      s.step();
      const p = pw();
      ok = ok && apart(s.view());
      if (p.z > 64 && p.z < 128 && (p.x <= 224 || p.x >= 320)) beside = true;
    }
    return ok && beside && pw().x === 272 && pw().z === 32;
  },
  'a unit walks round a unit that stands'(entry) {
    // an idle constructor stands on the Peewee's straight line, at its middle
    const s = WikiSim.create(entry, { scene: { selected: ['pw'], units: [{ id: 'post', type: 'ARMCK', at: [200, 300] }] } });
    const pw = () => s.view().units.find(u => u.id === 'pw');
    s.input({ move: [144, 424] }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' });
    let ok = apart(s.view()), t = 0;
    for (; t < 600 && s.orders('pw').length; t++) { s.step(); ok = ok && apart(s.view()); }
    const post = s.view().units.find(u => u.id === 'post');
    return ok && pw().x === 144 && pw().z === 424 && post.x === 200 && post.z === 300;
  },
  'two units sent to one place: the second stops close by'(entry) {
    // a move spreads its units 24 apart, closer than two 32-wide bodies: the one there first takes its place, and
    // the other stops beside it with its move ended
    const s = WikiSim.create(entry, { scene: { selected: ['ck', 'ack'] } }), at = id => s.view().units.find(u => u.id === id);
    s.input({ move: [400, 400] }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' });
    const goal = s.orders('ack')[0];
    let ok = apart(s.view()), t = 0;
    for (; t < 1200 && (s.orders('ck').length || s.orders('ack').length); t++) { s.step(); ok = ok && apart(s.view()); }
    const ck = at('ck'), ack = at('ack'), off = Math.hypot(ack.x - goal.x, ack.z - goal.z);
    return ok && t < 1200 && ck.x === 388 && ck.z === 400 && off > 0 && off <= 32;
  },
  'a builder whose build distance is under its half width reaches its site and builds'(entry) {
    // ARMCK's BuildDistance cut to 4, under its 16 half width: in range only by touching the site, whose edge stops it
    const short = { ...entry, units: { ...entry.units, ARMCK: { ...entry.units.ARMCK, buildDistance: 4 } } };
    const s = WikiSim.create(short, { scene: { selected: ['ck'] } });
    s.input({ move: [376, 72] }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' });
    let ok = s.orders('ck')[0] && s.orders('ck')[0].k === 'build', raised = null, t = 0;
    for (; t < 1500 && !(raised && raised.built === 1); t++) {
      s.step();
      ok = ok && apart(s.view());
      raised = fresh(s.view(), 'ARMMEX')[0] || null;
    }
    return ok && t < 1500;
  },
  'a short-range builder sends a unit off its site, and builds'(entry) {
    // BuildDistance 4, under the 16 half width: it arrives only by its body, and must claim there for the Peewee
    // standing across the extractor's site (cells 22..24 x 3..5) to step off
    const short = { ...entry, units: { ...entry.units, ARMCK: { ...entry.units.ARMCK, buildDistance: 4 } } };
    return shortBuild(short, { selected: ['ck'], units: [{ id: 'stand', type: 'ARMPW', at: [396, 72] }] });
  },
  'a short-range builder over its site\'s edge steps off it, and builds'(entry) {
    // a second constructor stands 8 world units over the site's west edge (352) when it is ordered there
    const short = { ...entry, units: { ...entry.units, ARMCK: { ...entry.units.ARMCK, buildDistance: 4 } } };
    return shortBuild(short, { selected: ['b2'], units: [{ id: 'b2', type: 'ARMCK', at: [344, 72] }] });
  },
  'a scene unit put over another starts at the nearest clear place'(entry) {
    // a second Peewee authored on the first starts two cells off, the nearest whole-cell offset that clears a 32-wide
    // body, and of the four at that distance the one first by z, then x: (0, -2) cells
    const s = WikiSim.create(entry, { scene: { units: [{ id: 'twin', type: 'ARMPW', at: [256, 176] }] } });
    const twin = s.view().units.find(u => u.id === 'twin'), pw = s.view().units.find(u => u.id === 'pw');
    return apart(s.view()) && pw.x === 256 && pw.z === 176 && twin.x === 256 && twin.z === 144;
  },
  'a build waits for the units on its site, which step off it'(entry) {
    // a Peewee stands across the extractor's site; the constructor's build starts once it has stepped off
    const s = WikiSim.create(entry, { scene: { selected: ['ck'], units: [{ id: 'stand', type: 'ARMPW', at: [396, 72] }] } });
    s.input({ move: [376, 72] }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' });
    const o = s.orders('ck')[0], site = o && o.k === 'build' ? [o.ox * 16, o.oz * 16, (o.ox + 3) * 16, (o.oz + 3) * 16] : null;
    const on = u => site && u.x + 16 > site[0] && u.x - 16 < site[2] && u.z + 16 > site[1] && u.z - 16 < site[3];
    let ok = !!site && on(s.view().units.find(u => u.id === 'stand')), raised = null, t = 0;
    for (; t < 900 && !(raised && raised.built === 1); t++) {
      s.step();
      ok = ok && apart(s.view());
      raised = fresh(s.view(), 'ARMMEX')[0] || null;
    }
    return ok && t < 900 && !on(s.view().units.find(u => u.id === 'stand'));
  },
};

/* a short-range builder's case: the scene's selected builder ordered to Quick Build the extractor at (376, 72); true
   when it is ordered, the extractor is finished within 1500 ticks, and no two bodies overlap on any tick (a unit over
   the site overlaps no body: the site is one only once started, and the build waits for it to be clear) */
function shortBuild(entry, scene) {
  const s = WikiSim.create(entry, { scene });
  s.input({ move: [376, 72] }); s.input({ down: 'rmb' }); s.input({ up: 'rmb' });
  const id = scene.selected[0], o = s.orders(id)[0];
  let ok = !!o && o.k === 'build', raised = null, t = 0;
  for (; t < 1500 && !(raised && raised.built === 1); t++) {
    s.step();
    raised = fresh(s.view(), 'ARMMEX')[0] || null;
    ok = ok && apart(s.view());
  }
  return ok && t < 1500;
}

/* no two bodies overlap: each unit's footprint box about its centre, touching allowed */
function apart(v) {
  const r = u => [u.x - u.fx * 8, u.z - u.fz * 8, u.x + u.fx * 8, u.z + u.fz * 8];
  for (let i = 0; i < v.units.length; i++) for (let j = i + 1; j < v.units.length; j++) {
    const a = r(v.units[i]), b = r(v.units[j]);
    if (a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3]) return false;
  }
  return true;
}

/* ---- buildshapes: the placement gesture (tagpu_place.c). Its checks read the queue of ARMCK ('ck') around each
   release and the preview just before it; the expected sites are tagpu_placegeo.c's for the demo's press and release
   cells, written out here by hand from its formulas so that a change to the port shows as a failure. ---- */
const cellOf = s => [s.x / 16 - s.fx / 2, s.z / 16 - s.fz / 2];
const key = c => c.join(',');
const builds = os => (os || []).filter(o => o.k === 'build').map(o => ({ type: o.type, c: [o.ox, o.oz] }));
const ups = (d, k) => d.inputs.filter(e => e.up === k).map(e => e.at);
const downs = (d, k) => d.inputs.filter(e => e.down === k).map(e => e.at);
// the build orders the inputs of tick u added to ck's queue, in order: the entries of the queue after them that were not
// in it before (a head that finished on that tick only shortens the queue)
function added(r, u, id = 'ck') {
  const before = new Set(builds(r.frames[u - 1].o[id]).map(b => b.type + key(b.c)));
  return builds(r.frames[u].o[id]).filter(b => !before.has(b.type + key(b.c))).map(b => b.c);
}
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const show = cs => cs.map(key).join(' ');
const preview = f => f.v.sites.map(cellOf);
// a site of f x f cells at c overlapping the rectangle r = [x, z, w, h]
const hits = (c, f, r) => c[0] < r[0] + r[2] && r[0] < c[0] + f && c[1] < r[1] + r[3] && r[1] < c[1] + f;
// place_ring(2, 2, tx, tz, 5, 5): three a side, right side down, bottom left, left side up, top right
const ring5 = (tx, tz) => [[tx + 5, tz], [tx + 5, tz + 2], [tx + 5, tz + 4], [tx + 3, tz + 5], [tx + 1, tz + 5], [tx - 1, tz + 5],
  [tx - 2, tz + 3], [tx - 2, tz + 1], [tx - 2, tz - 1], [tx, tz - 2], [tx + 2, tz - 2], [tx + 4, tz - 2]];
/* place_ring(f, f, tx, tz, w, h) for any square footprint f round a w x h building: 1 + h / f sites down the right
   side, 1 + w / f along the bottom, then up the left side and along the top, each side from a corner. A cast's solar
   collector need not be stock's 5 x 5 (TA Zero's GOK and CORE ones are 3 x 3), so the ring checks read this; on the
   5 x 5 it must give ring5's sites, written out by hand above, which RING_SELF_CHECK below holds it to. */
function ringOf(f, [tx, tz, w, h]) {
  const down = 1 + Math.trunc(h / f), across = 1 + Math.trunc(w / f), out = [];
  for (let k = 0; k < down; k++) out.push([tx + w, tz + k * f]);
  for (let k = 0; k < across; k++) out.push([tx + w - f - k * f, tz + h]);
  for (let k = 0; k < down; k++) out.push([tx - f, tz + h - f - k * f]);
  for (let k = 0; k < across; k++) out.push([tx + k * f, tz - f]);
  return out;
}
const RING_SELF_CHECK = JSON.stringify(ringOf(2, [20, 9, 5, 5])) === JSON.stringify(ring5(20, 9)) &&
  JSON.stringify(ringOf(2, [10, 15, 5, 5])) === JSON.stringify(ring5(10, 15));
if (!RING_SELF_CHECK) throw new Error('ringOf does not give ring5 on the 5 x 5 solar collector');
/* the cells of a building of f x f centred at world (x, z): its first cell is 0x4197D0's rounding, floor((v - 8f + 8)
   / 16), as sim.js `origin` places a scene unit and a build cursor's site. The scene's solar collector stands at
   (360, 184): cells 20..24 x 9..13 for stock's 5 x 5. */
const cellsAt = (x, z, f) => [Math.floor((x - 8 * f + 8) / 16), Math.floor((z - 8 * f + 8) / 16), f, f];
const solarOf = r => { const f = r.fp('ARMSOLAR')[0]; return cellsAt(360, 184, f); };

const BUILDSHAPES = {
  clicks(r, d, ok) {
    const [plain, s1, s2] = downs(d, 'lmb'), f = t => r.frames[t];
    ok(f(plain).v.mode === 'contextual' && same(added(r, plain), [[10, 5]]), `a plain click placed ${r.ty('ARMLLT')} at 10,5 and disarmed (${f(plain).v.mode})`);
    ok(f(s1 + 3).v.mode === `build:${r.ty('ARMLLT')}` && same(added(r, s1 + 3), [[13, 5]]), 'the first Shift-click queued 13,5 and stayed armed');
    ok(f(s2 + 3).v.mode === `build:${r.ty('ARMLLT')}` && same(added(r, s2 + 3), [[16, 5]]), 'the second Shift-click queued 16,5 and stayed armed');
    const off = ups(d, 'shift')[0];
    ok(f(off - 1).v.mode === `build:${r.ty('ARMLLT')}` && f(off).v.mode === 'contextual', 'letting go of Shift after placing disarmed');
    ok(r.log.filter(e => e.what === 'finished' && e.type === r.ty('ARMLLT')).length >= 2, `${r.ty('ARMCK')} built at least two of them`);
  },
  line(r, d, ok) {
    const [u1, u2] = ups(d, 'lmb'), want = [[4, 3], [6, 4], [8, 4], [10, 5], [12, 5], [14, 6], [16, 6]];
    const p = r.frames[u1 - 1];
    ok(p.v.shape === 'line' && same(preview(p), want) && p.v.sites.every(s => s.state === 'place'), `the preview is the line 4,3..16,6 (${show(preview(p))})`);
    ok(want.every((c, i) => !i || c[0] - want[i - 1][0] === 2) && new Set(want.map(c => c[1])).size > 1, 'one footprint along x, the slope followed on z');
    ok(same(added(r, u1), want), `the release queued those seven in order (${show(added(r, u1))})`);
    const off = ups(d, 'shift')[0];
    ok(r.frames[off].v.mode === 'contextual', 'letting go of Shift after the line disarmed');
    const g = r.frames.filter(f => f.t > downs(d, 'lmb')[1] && f.t < u2 && f.v.keys.includes('lmb') && !f.v.keys.includes('shift'));
    ok(g.length && g.every(f => !f.v.sites.length && f.v.ghost), 'with Shift up mid-drag nothing previews but the single square');
    ok(added(r, u2).length === 0 && r.last.v.mode === `build:${r.ty('ARMLLT')}`, `releasing after Shift ordered nothing and kept ${r.ty('ARMLLT')} armed`);
  },
  axis(r, d, ok) {
    const u = ups(d, 'lmb')[0], p = r.frames[u - 1], cs = preview(p);
    ok(p.v.shape === 'axis' && cs.length === 10 && cs.every((c, i) => c[1] === 10 && c[0] === 12 + 2 * i), `ten sites held to row 10 (${show(cs)})`);
    ok(Math.floor(p.v.cursor.z / 16) !== 10, 'while the cursor stood off that row');
    /* the sites over the solar collector, from its cells: 20, 22 and 24 on stock's 5 x 5 at cells 20..24, the rest
       12..18 and 26..30 */
    const SOL = solarOf(r), row = Array.from({ length: 10 }, (_, i) => [12 + 2 * i, 10]);
    const over = row.filter(c => hits(c, 2, SOL)), clear = row.filter(c => !hits(c, 2, SOL));
    const red = p.v.sites.filter(s => s.state === 'refused').map(cellOf);
    ok(over.length && same(red, over), `the ${over.length} over the solar collector are red (${show(red)})`);
    const a = added(r, u);
    ok(same(a, clear), `the red sites were skipped (${show(a)})`);
  },
  rect(r, d, ok) {
    const u = ups(d, 'lmb')[0], p = r.frames[u - 1];
    const want = [[6, 3], [8, 3], [10, 3], [12, 3], [12, 5], [10, 5], [8, 5], [6, 5], [6, 7], [8, 7], [10, 7], [12, 7]];
    ok(p.v.shape === 'rect' && same(preview(p), want), `the preview is a 4 x 3 serpentine (${show(preview(p))})`);
    ok(p.v.sites.every((s, i) => s.order === i + 1), 'the preview numbers the sites 1..12 in that order');
    ok(same(added(r, u), want), 'the release queued them in serpentine order');
    // the demo waits for two pieces to finish (gen-buildshapes.mjs `built`)
    const done = r.log.filter(e => e.what === 'finished').map(e => [e.ox, e.oz]);
    const least = 2;
    ok(done.length >= least && same(done, want.slice(0, done.length)), `${r.ty('ARMCK')} built them in that order (${done.length} done, ${least} wanted)`);
  },
  hollow(r, d, ok) {
    const u = ups(d, 'lmb')[0], p = r.frames[u - 1], a = added(r, u);
    ok(p.v.shape === 'hollow' && a.length === 14 && new Set(a.map(key)).size === 14, `fourteen sites queued (${a.length})`);
    ok(a.every(c => c[0] === 4 || c[0] === 12 || c[1] === 15 || c[1] === 21) && a.every(c => c[0] >= 4 && c[0] <= 12 && c[1] >= 15 && c[1] <= 21),
      'all on the edge of 4,15..12,21, none inside');
    ok(same(a[0], [12, 19]) && same(a[a.length - 1], [12, 21]), 'walked round from beside the press back to it');
  },
  spacing(r, d, ok) {
    const [u1, u2] = ups(d, 'lmb'), arm2 = armedAt(r, d.inputs.filter(e => 'build' in e)[1]);
    const pitch = cs => cs.slice(1).map((c, i) => c[0] - cs[i][0]);
    const steps = [];
    for (const f of r.frames.filter(f => f.t < u1 && f.v.sites.length)) {
      const k = `${f.v.spacing}:${f.v.sites.length}`;
      if (steps[steps.length - 1] !== k) steps.push(k);
    }
    ok(same(steps.slice(-3), ['0:7', '1:5', '2:4']), `mid-drag Alt+Z spread the line at once: 7, 5, 4 sites (${steps.join(' ')})`);
    const a1 = added(r, u1);
    ok(same(a1, [[4, 4], [8, 4], [12, 4], [16, 4]]) && pitch(a1).every(p => p === 2 + 2), `queued at pitch footprint + 2 (${show(a1)})`);
    ok(r.frames[arm2].v.spacing === 2, `armed again, ${r.ty('ARMDRAG')} kept its spacing`);
    const p2 = r.frames[u2 - 1];
    ok(p2.v.spacing === 0 && p2.v.sites.length === 7, 'Alt+X twice brought it back to 0');
    const a2 = added(r, u2);
    ok(a2.length === 7 && pitch(a2).every(p => p === 2) && a2.every(c => c[1] === 7), `then queued at pitch footprint + 0 (${show(a2)})`);
  },
  ring(r, d, ok) {
    const c = downs(d, 'lmb')[0], p = r.frames[c - 1], want = ringOf(2, solarOf(r));
    ok(p.v.shape === 'ring' && same(preview(p), want) && p.v.sites.every(s => s.state === 'place'), `Shift+Ctrl over the solar collector previews its ring (${show(preview(p))})`);
    ok(same(added(r, c + 3), want), 'a press and release without a drag ordered it');
    ok(r.log.some(e => e.what === 'finished' && e.type === r.ty('ARMDRAG')), `${r.ty('ARMCK')} built a piece of the ring`);
  },
  ringqueued(r, d, ok) {
    // the near solar collector is queued with the cursor at (200, 280): cells 10..14 x 15..19 for stock's 5 x 5
    const near = cellsAt(200, 280, r.fp('ARMSOLAR')[0]), c = downs(d, 'lmb')[2], p = r.frames[c - 1], want = ringOf(2, near);
    // the demo's own builder `b`, selected, far from both solar collectors
    ok(builds(p.o.b).some(b => b.type === r.ty('ARMSOLAR') && same(b.c, near.slice(0, 2))) && !p.v.units.some(u => u.type === r.ty('ARMSOLAR') && u.id !== 'sol'),
      `the solar collector at ${key(near.slice(0, 2))} is queued and not started, nor the far one`);
    ok(p.v.shape === 'ring' && same(preview(p), want), `Shift+Ctrl over it previews its ring (${show(preview(p))})`);
    const q = builds(r.frames[c + 3].o.b);
    ok(same(added(r, c + 3, 'b'), want) && q.length === 2 + want.length && q[0].type === r.ty('ARMSOLAR') && q[1].type === r.ty('ARMSOLAR'),
      'the click queued the ring after the two solar collectors');
  },
  toggle(r, d, ok) {
    const [u1, u2, u3] = ups(d, 'lmb');
    ok(same(added(r, u1), [[29, 3], [31, 3], [33, 3], [35, 3], [37, 3]]), 'a row of five queued');
    const p = r.frames[u2 - 1];
    ok(same(preview(p), [[37, 3], [35, 3], [33, 3]]) && p.v.sites.every(s => s.state === 'remove' && s.order === null), 'dragging back over three of them shows them yellow');
    ok(same(builds(r.frames[u2].o.ck).map(b => b.c), [[29, 3], [31, 3]]), 'the release took those three off and added nothing');
    ok(same(builds(r.frames[u3].o.ck).map(b => b.c), [[29, 3]]), 'a Shift-click on 31,3 took it off too');
  },
  reshape(r, d, ok) {
    const p0 = downs(d, 'lmb')[0], u = ups(d, 'lmb')[0], seen = [];
    for (const f of r.frames.filter(f => f.t > p0 && f.t < u)) {
      const k = `${f.v.shape}:${f.v.sites.length}`;
      if (seen[seen.length - 1] !== k) seen.push(k);
    }
    ok(same(seen.slice(-3), ['line:5', 'rect:15', 'hollow:12']), `one drag: a line, Alt a rectangle, Ctrl hollow (${seen.slice(-3).join(' ')})`);
    ok(added(r, u).length === 12, 'the hollow one was ordered');
    const rmb = downs(d, 'cancel')[0];
    ok(r.frames[rmb - 1].v.sites.length > 0 && r.frames[rmb].v.mode === 'contextual' && !r.frames[rmb].v.sites.length, 'the right-click ended the drag and disarmed');
    ok(!r.frames.some(f => builds(f.o.ck).some(b => b.c[1] >= 17)), 'the cancelled drag ordered nothing');
  },
};

/* rules no buildshapes demo shows, on its entry's scene */
const S2 = (cx, cz, f = 2) => [(cx + f / 2) * 16, (cz + f / 2) * 16];
function gesture(s, keys, from, to) {
  s.input({ move: from }); for (const k of keys) s.input({ down: k });
  s.input({ down: 'lmb' }); s.input({ move: to });
  const v = s.view(); s.input({ up: 'lmb' }); for (const k of keys) s.input({ up: k });
  return v;
}
// the entry with a 1 x 1 stand-in ARMCK can build: the entry's buildings are all 2 x 2 or larger, and neighbouring 1 x 1
// sites are the ones 16 world units apart, inside 0x43AFC0's reach of each other
function withTiny(entry, e0 = entry) {
  const e = { ...e0, units: { ...entry.units, TINY: { footprint: [1, 1], buildTime: 10, from: 'a 1 x 1 stand-in' } } };
  e.units.ARMCK = { ...e.units.ARMCK, builds: [...e.units.ARMCK.builds, 'TINY'] };
  return e;
}
// a queued row of 1 x 1 at cells 10..12 of row 5, then a Shift-drag over it from `from` to `to` on that row: the preview
// before the release as 'cell:state', and ck's build orders after it
function overRow(entry, from, to) {
  const s = WikiSim.create(withTiny(entry), { scene: {} }); s.input({ build: 'TINY' });
  gesture(s, ['shift'], S2(10, 5, 1), S2(12, 5, 1));
  const queued = builds(s.orders('ck')).map(b => key(b.c));
  s.input({ build: 'TINY' });                                 // letting go of Shift after placing disarmed
  const v = gesture(s, ['shift'], S2(from, 5, 1), S2(to, 5, 1));
  return { queued, preview: v.sites.map(x => `${cellOf(x)[0]}:${x.state}`), after: builds(s.orders('ck')).map(b => key(b.c)) };
}
const BUILDSHAPES_RULES = {
  'a drag from beside a queued 1 x 1 row takes off each order at its own site and places the site beside it'(entry) {
    const a = overRow(entry, 9, 12), b = overRow(entry, 12, 9);
    return same(a.queued, ['10,5', '11,5', '12,5']) &&
      same(a.preview, ['9:place', '10:remove', '11:remove', '12:remove']) && same(a.after, ['9,5']) &&
      same(b.preview, ['12:remove', '11:remove', '10:remove', '9:place']) && same(b.after, ['9,5']);
  },
  'Shift+Ctrl over a building takes the ring before the drag, as tagpu_place.c evaluates'(entry) {
    const s = WikiSim.create(entry, entry.demos[0]); s.input({ build: 'ARMDRAG' });
    s.input({ move: S2(12, 10) }); s.input({ down: 'shift' }); s.input({ down: 'ctrl' }); s.input({ down: 'lmb' });
    s.input({ move: S2(16, 10) }); const before = s.view().shape;
    s.input({ move: [360, 184] }); return before === 'axis' && s.view().shape === 'ring';
  },
  'the ring ignores spacing; spacing clamps to 0..16 and needs a building armed and Alt'(entry) {
    const s = WikiSim.create(entry, entry.demos[0]);
    s.input({ down: 'alt' }); s.input({ down: 'z' }); s.input({ up: 'z' });
    if (s.view().spacing !== null) return false;
    s.input({ build: 'ARMDRAG' });
    for (let i = 0; i < 20; i++) s.input({ down: 'z' });      // an auto-repeat: downs without ups
    if (s.view().spacing !== 16) return false;
    s.input({ up: 'z' }); s.input({ up: 'alt' }); s.input({ down: 'z' }); s.input({ up: 'z' });
    if (s.view().spacing !== 16) return false;
    s.input({ move: [360, 184] }); s.input({ down: 'shift' }); s.input({ down: 'ctrl' });
    const ring = s.view().sites.map(cellOf);
    s.input({ down: 'alt' }); for (let i = 0; i < 20; i++) s.input({ down: 'x' });
    return s.view().spacing === 0 && same(ring, ring5(20, 9));
  },
  'a ring leaves out the sites that would take a queued order off, and takes nothing off'(entry) {
    const s = WikiSim.create(entry, entry.demos[0]); s.input({ build: 'ARMDRAG' });
    s.input({ down: 'shift' }); s.input({ move: S2(25, 9) }); s.input({ down: 'lmb' }); s.input({ up: 'lmb' });
    s.input({ move: [360, 184] }); s.input({ down: 'ctrl' });
    const cs = s.view().sites.map(cellOf);
    s.input({ down: 'lmb' }); s.input({ up: 'lmb' });
    const q = builds(s.orders('ck')).map(b => key(b.c));
    return cs.length === 11 && !cs.some(c => same(c, [25, 9])) && q.length === 12 && q[0] === '25,9';
  },
  'a gesture places at most 256 sites'(entry) {
    const e = withTiny(entry, withFirst(entry, g => ({ ...g, map: { w: 64, h: 64, surfaceMetal: 0 } }), true));
    const s = WikiSim.create(e, { scene: {} }); s.input({ build: 'TINY' });
    const v = gesture(s, ['shift', 'alt'], [24, 24], [62 * 16, 62 * 16]);
    return v.sites.length === 256 && builds(s.orders('ck')).length === 256;
  },
  'a queue stops growing at 1024 orders'(entry) {
    const e = withFirst(entry, g => ({ ...g, units: g.units.map(u => u.id !== 'ck' ? u
      : { ...u, orders: Array.from({ length: 1020 }, () => ({ move: [120, 200] })) }) }));
    const s = WikiSim.create(e, { scene: {} }); s.input({ build: 'ARMDRAG' });
    gesture(s, ['shift'], S2(4, 3), S2(16, 3));
    return s.orders('ck').length === 1024 && builds(s.orders('ck')).length === 4;
  },
};

/* ---- the sidebar (research/notes/ingame-wiki.md, *The left sidebar on the stage*; sim.js "the sidebar's rules" and
   "the build trip") ----
   Every build input of a demo played with a layout is a trip: it is never locked on the fixture, it arms its
   building on the release at t + TRIP_UP and not before, the cursor is back where it stood at t + TRIP, and it goes
   through the pages written here. A path is the page shown at the build's tick, then each pager press up to the
   button's own press: '1' presses no pager, '1<4' is one PREV from page 1 to page 4, '1>2>3' two NEXTs. They are the
   fixture's: ARMCK's page 1 holds ARMMEX and ARMSOLAR, page 3 ARMDRAG and page 4 ARMLLT, so from page 1 ARMLLT is one
   PREV and ARMDRAG two NEXTs (the tie goes to NEXT), and a second ARMLLT or ARMDRAG starts on the page the first left
   shown. */
const TRIP_PATHS = {
  quickbuild: { snap: ['1'], alt: ['1'], fog: ['1'] },
  buildshapes: { clicks: ['1<4', '4'], line: ['1<4', '4'], axis: ['1<4'], rect: ['1>2>3'], hollow: ['1>2>3'],
    spacing: ['1>2>3', '3'], ring: ['1>2>3'], ringqueued: ['1', '1>2>3'], toggle: ['1<4'], reshape: ['1>2>3'] },
};
const gadgetsOf = (L, v) => L.screens[v.side.screen].gadgets;
// the pages a trip from tick t shows up to the button's press, each change marked by the pager pressed just before it
function tripPath(r, t) {
  const at = i => r.frames[t + i].v;
  let path = String(at(0).side.page);
  for (let i = 1; i <= TRIP_DOWN; i++) {
    const was = at(i - 1), now = at(i);
    if (now.side.page === was.side.page) continue;
    const pressed = gadgetsOf(r.layout, was).find((g, k) => was.side.state[k] === 1 && (g.role === 'prev' || g.role === 'next'));
    path += (pressed ? (pressed.role === 'next' ? '>' : '<') : '?') + now.side.page;
  }
  return path;
}
/* the page path a pack's layout gives the build input e of a played demo: from the page the shown builder shows at
   e's tick to the first of its pages holding a button of the type cast for e's unit that is neither greyed nor
   hidden, the nearer way round with a tie to NEXT -- the rule TRIP_PATHS spells out for the fixture, read here off
   the pack's own pages. '?' when no page holds the button: the trip cannot be planned there. */
function packPath(r, e) {
  const v = r.frames[e.at].v, L = r.layout, shown = v.units.find(u => u.id === v.side.unit);
  const T = shown && L.sides[v.side.index].types[shown.type], pages = T ? T.pages : [], n = pages.length, want = r.ty(e.build);
  const p = 1 + pages.findIndex(name => L.screens[name].gadgets.some(g => g.role === 'build' && g.unit === want && !g.grey && !g.hidden));
  if (!p || !v.side.page) return '?';
  const c = v.side.page, kNext = ((p - c) % n + n) % n, kPrev = ((c - p) % n + n) % n, next = kNext <= kPrev;
  let path = String(c);
  for (let j = 0, page = c; j < Math.min(kNext, kPrev); j++) {
    page = next ? (page === n ? 1 : page + 1) : (page < 2 ? n : page - 1);
    path += (next ? '>' : '<') + page;
  }
  return path;
}
function tripChecks(r, d, ok, paths) {
  const bs = d.inputs.filter(e => 'build' in e);
  ok(r.frames.every(f => f.v.locked === null), 'no build is locked on this layout');
  if (PACK) paths = bs.map(e => packPath(r, e));
  ok(bs.length === (paths || []).length, `a page path is written for each of the ${bs.length} build inputs`);
  bs.forEach((e, n) => {
    const t = e.at, v = i => r.frames[t + i].v, T = r.ty(e.build), what = `the ${T} build at ${t}`;
    const before = Array.from({ length: TRIP_UP }, (_, i) => v(i).mode), after = Array.from({ length: TRIP - TRIP_UP + 1 }, (_, i) => v(TRIP_UP + i).mode);
    ok(before.every(m => m === v(0).mode) && after.every(m => m === `build:${T}`),
      `${what}: armed from t + ${TRIP_UP}, by the release, and not before (${v(TRIP_UP - 1).mode}, then ${v(TRIP_UP).mode})`);
    ok(v(TRIP).cursor.x === v(0).cursor.x && v(TRIP).cursor.z === v(0).cursor.z, `${what}: the cursor is back where it stood at t + ${TRIP}`);
    const path = tripPath(r, t);
    ok(paths && path === paths[n], `${what}: the pages ${path} (want ${paths && paths[n]})`);
    const b = gadgetsOf(r.layout, v(TRIP_DOWN)).findIndex(g => g.role === 'build' && g.unit === T);
    ok(b >= 0 && v(TRIP_DOWN).side.state[b] === 1 && v(TRIP_UP - 1).side.state[b] === 1 && v(TRIP_UP).side.state[b] === 0,
      `${what}: its button shows pressed from t + ${TRIP_DOWN} to the release`);
  });
}

/* the sidebar's rules on the fixture, which no demo shows: the page router, a button's press and release, the
   pagers and tabs, STOP, the inert buttons and the minimap, the clamped map drag, the longest trip, a locked build */
const SIDE_W = 128;
// the centre of the gadget named `name` on screen `screen`, in stage units
function centre(L, screen, name) {
  const s = L.screens[screen], g = s.gadgets.find(g => g.name === name);
  return [-SIDE_W + g.rect[0] + g.rect[2] / 2, s.y + g.rect[1] + g.rect[3] / 2];
}
const press = (s, at, b = 'lmb') => { s.input({ move: at }); s.input({ down: b }); s.input({ up: b }); };
const stateOf = (v, name) => v.side.state[FIXTURE.screens[v.side.screen].gadgets.findIndex(g => g.name === name)];
const selIds = v => v.units.filter(u => u.selected).map(u => u.id).join(' ');
const fixtureWith = (types, screens) => ({ ...FIXTURE, sides: [{ ...FIXTURE.sides[0], types: { ...FIXTURE.sides[0].types, ...types } }],
  screens: { ...FIXTURE.screens, ...screens } });
const withOrders = (entry, ids) => withFirst(entry, g => ({ ...g,
  units: g.units.map(u => ids.includes(u.id) ? { ...u, orders: [{ move: [120, 120] }, { move: [200, 120] }] } : u) }));
/* stand-ins for STOP's rules: a static unit with canstop, as turrets and factories have it, and a mobile one without,
   neither a builder */
const withStoppers = entry => ({ ...entry, units: { ...entry.units,
  TURRET: { footprint: [2, 2], buildTime: 10, canstop: 1, from: 'a static stand-in with canstop' },
  ROVER: { footprint: [2, 2], buildTime: 10, maxVelocity: 1, from: 'a mobile stand-in without canstop' } } });
const STOPPERS = [{ id: 'tur', type: 'TURRET', at: [120, 136] }, { id: 'rov', type: 'ROVER', at: [200, 136] }];
// ARMCK1 of the fixture with its gadgets replaced, and a gadget put just before or after the one named n
const G1 = FIXTURE.screens.ARMCK1.gadgets;
const ck1With = gadgets => fixtureWith({}, { ARMCK1: { ...FIXTURE.screens.ARMCK1, gadgets } });
const beside = (n, g, first) => G1.flatMap(x => x.name !== n ? [x] : first ? [g, x] : [x, g]);
const rectOf = n => G1.find(g => g.name === n).rect;
const rectCentre = r => [-SIDE_W + r[0] + r[2] / 2, FIXTURE.screens.ARMCK1.y + r[1] + r[3] / 2];
const geoAt = (rect, extra = {}) => ({ name: 'ARMGEO', role: 'build', unit: 'ARMGEO', rect, ...extra });
const SIDE_RULES = {
  'the page router: nothing selected shows MAIN2, one ARMCK its page 1, ARMCK with ARMACK or a Peewee alone GEN'(entry) {
    const side = sel => WikiSim.create(entry, { scene: { selected: sel } }, FIXTURE).view().side;
    const none = side([]), ck = side(['ck']), two = side(['ck', 'ack']), pw = side(['pw']);
    return none.screen === 'ARMMAIN2' && none.unit === null && ck.screen === 'ARMCK1' && ck.unit === 'ck' && ck.page === 1 &&
      two.screen === 'ARMGEN' && two.unit === null && pw.screen === 'ARMGEN' && pw.unit === 'pw' && pw.page === 0;
  },
  'ORDERS shows GEN with ORDERS current, and BUILD goes back to the page kept'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE);
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMNEXT'));
    const p2 = s.view().side;
    // a tab acts on the press
    s.input({ move: centre(FIXTURE, 'ARMCK2', 'ARMORDERS') }); s.input({ down: 'lmb' });
    const gen = s.view();
    s.input({ up: 'lmb' });
    press(s, centre(FIXTURE, 'ARMGEN', 'ARMBUILD'));
    const back = s.view();
    return p2.screen === 'ARMCK2' && gen.side.screen === 'ARMGEN' && gen.side.unit === 'ck' &&
      stateOf(gen, 'ARMORDERS') === 1 && stateOf(gen, 'ARMBUILD') === 0 &&
      back.side.screen === 'ARMCK2' && back.side.page === 2 && stateOf(back, 'ARMBUILD') === 1;
  },
  'a build button pressed, dragged off and released does nothing; released inside it arms'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE), mex = centre(FIXTURE, 'ARMCK1', 'ARMMEX');
    s.input({ move: mex }); s.input({ down: 'lmb' });
    const held = s.view();
    s.input({ move: centre(FIXTURE, 'ARMCK1', 'ARMSOLAR') });
    const off = s.view();
    s.input({ move: [200, 200] }); s.input({ up: 'lmb' });
    const out = s.view();
    press(s, mex);
    const armed = s.view();
    return held.mode === 'contextual' && stateOf(held, 'ARMMEX') === 1 && stateOf(off, 'ARMMEX') === 0 &&
      out.mode === 'contextual' && selIds(out) === 'ck' && !out.drag && armed.mode === 'build:ARMMEX' && stateOf(armed, 'ARMMEX') === 0;
  },
  // 0x41AA00 never tests which button released a structure's button
  'the right button arms a building too'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE);
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMMEX'), 'rmb');
    return s.view().mode === 'build:ARMMEX';
  },
  /* the in-play handler's cancel: a right press it gets disarms whatever the point (0x4993AC -> 0x499100), and it gets
     every press outside the screen's window (the pump's poll 0x4AB5D0) -- the minimap, the column's rows below the
     screen, the band under a short map */
  'a right press off the screen disarms a building armed and orders nothing'(entry) {
    const off = at => {
      const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE);
      press(s, centre(FIXTURE, 'ARMCK1', 'ARMMEX'));
      s.input({ move: at }); s.input({ down: 'rmb' });
      const v = s.view();
      s.input({ up: 'rmb' });
      return v.mode === 'contextual' && s.view().mode === 'contextual' && s.orders('ck').length === 0 &&
        selIds(s.view()) === 'ck' && s.view().side.screen === 'ARMCK1';
    };
    const MH = first(entry).map.h * 16;
    return off([-SIDE_W + 60, 128 + 352 + 20]) && off([-SIDE_W + 64, 64]) && off([60, MH + 8]);
  },
  /* a left press off the screen reaches stock's click 0x498F70 (place_button returns 0 off the viewport), which places
     the building once at the point clamped to the view, snapped: the sim's clamp is onto the map, and the deposit at
     cells 2..3 of rows 26..27 is where ARMMEX snaps from the clamped corner. Shift keeps it armed; the release adds
     nothing; the minimap stays inert (decision 6) */
  'a left press off the screen with a building armed places it once at the clamped cursor'(entry) {
    const run = (at, shift) => {
      const s = WikiSim.create(entry, { scene: { selected: ['ck'], metal: [{ rect: [2, 26, 2, 2], v: 200 }] } }, FIXTURE);
      press(s, centre(FIXTURE, 'ARMCK1', 'ARMMEX'));
      if (shift) s.input({ down: 'shift' });
      s.input({ move: [200, 300] }); s.input({ move: at }); s.input({ down: 'lmb' });
      const pressed = s.orders('ck').map(o => `${o.type}@${o.ox},${o.oz}`).join(' '), armed = s.view().mode;
      s.input({ up: 'lmb' });
      const released = s.orders('ck').length;
      if (shift) s.input({ up: 'shift' });
      return { pressed, armed, released, after: s.view().mode };
    };
    const MH = first(entry).map.h * 16, below = run([-SIDE_W + 60, 128 + 352 + 20], false), held = run([-SIDE_W + 60, 128 + 352 + 20], true);
    const band = run([40, MH + 8], false), mini = run([-SIDE_W + 64, 64], false);
    return below.pressed === 'ARMMEX@1,26' && below.armed === 'contextual' && below.released === 1 &&
      held.pressed === 'ARMMEX@1,26' && held.armed === 'build:ARMMEX' && held.released === 1 && held.after === 'contextual' &&
      band.pressed === 'ARMMEX@1,26' && band.armed === 'contextual' &&
      mini.pressed === '' && mini.armed === 'build:ARMMEX' && mini.released === 0;
  },
  /* a right press on the screen is the GUI's alone (0x4AB5D0 pops it), so the handler's cancel never sees it: an empty
     spot or a greyed button keeps the building armed, and a live build button arms its own on the right release, as
     the left one would (0x41AA00 reads which button only for a factory's queue, 0x41AC09) */
  'a right press on the screen keeps a building armed, and a build button arms its own on the release'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE);
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMMEX'));
    press(s, [-SIDE_W + 2, 128 + 2], 'rmb');
    const empty = s.view().mode;
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMSOLAR'), 'rmb');
    const grey = s.view().mode;
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMNEXT'));
    s.input({ move: centre(FIXTURE, 'ARMCK2', 'ARMGEO') }); s.input({ down: 'rmb' });
    const held = s.view().mode;
    s.input({ up: 'rmb' });
    return empty === 'build:ARMMEX' && grey === 'build:ARMMEX' && held === 'build:ARMMEX' && s.view().mode === 'build:ARMGEO' &&
      s.orders('ck').length === 0;
  },
  'PREV from page 1 shows the last page'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE);
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMPREV'));
    const v = s.view().side;
    return v.screen === 'ARMCK4' && v.page === 4;
  },
  /* greyed: IGPATCH, which names no unit; ARMSOLAR, a unit this entry does not define (owner question Q-O1, the
     recommended answer); every order button but STOP */
  'a greyed button and an order button are inert'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE), first = s.view();
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMSOLAR'));
    const solar = s.view();
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMPREV'));
    const before = s.view(), patch = centre(FIXTURE, 'ARMCK4', 'IGPATCH');
    s.input({ move: patch }); s.input({ down: 'lmb' });
    const down = s.view();
    s.input({ up: 'lmb' });
    const grey = s.view();
    press(s, centre(FIXTURE, 'ARMCK4', 'ARMORDERS'));
    const gen = s.view();
    press(s, centre(FIXTURE, 'ARMGEN', 'ARMMOVE'));
    const order = s.view();
    const still = (a, b) => a.side.screen === b.side.screen && a.side.page === b.side.page && same(a.side.state, b.side.state) && a.mode === b.mode;
    return stateOf(first, 'ARMSOLAR') === 2 && still(solar, first) && stateOf(before, 'IGPATCH') === 2 && still(down, before) &&
      still(grey, before) && grey.mode === 'contextual' && stateOf(gen, 'ARMMOVE') === 2 && still(order, gen) && s.orders('ck').length === 0;
  },
  'a press on the minimap does nothing'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE), before = s.view();
    press(s, [-SIDE_W + 64, 64]); press(s, [-SIDE_W + 64, 64], 'rmb');
    const v = s.view();
    return same(v.side.state, before.side.state) && v.side.screen === 'ARMCK1' && selIds(v) === 'ck' && v.mode === 'contextual' &&
      !v.drag && s.orders('ck').length === 0 && v.cursor.shape === 'arrow';
  },
  /* STOP's greyed state is the OR of FBI canstop over the selection (0x41B2E0, 0x41A120), not mobility: the stand-ins
     are a static unit with canstop, as a turret or a factory has it, and a mobile one without */
  'STOP is live while a selected unit has canstop, greyed otherwise, and a greyed STOP takes no click'(entry) {
    // each sim is done with before the next is made: simparity.mjs replays one at a time
    const e = withStoppers(entry), sim = sel => WikiSim.create(e, { scene: { selected: sel, units: STOPPERS } }, FIXTURE);
    const stop = v => v.side && v.side.screen === 'ARMGEN' ? stateOf(v, 'ARMSTOP') : null;
    const ck = sim(['ck']);
    press(ck, centre(FIXTURE, 'ARMCK1', 'ARMORDERS'));
    const ckStop = stop(ck.view());
    const geo = sim(['geo']);
    geo.input({ build: 'ARMMEX' });
    press(geo, centre(FIXTURE, 'ARMGEN', 'ARMSTOP'));
    const greyed = geo.view();
    const states = [['geo'], ['tur'], ['rov'], ['geo', 'rov'], ['geo', 'tur'], ['rov', 'ck']].map(sel => stop(sim(sel).view()));
    return ckStop === 0 && same(states, [2, 0, 2, 2, 0, 0]) && greyed.mode === 'build:ARMMEX' && stop(greyed) === 2;
  },
  // 0x419BE0's STOP branch: main+0x2CC3 = 1 leaves the build mode, and 0x48CF30 stops every selected unit
  'STOP disarms an armed build and empties every selected unit\'s orders, canstop or not'(entry) {
    const e = withOrders(withStoppers(entry), ['ck', 'pw', 'ack']);
    const units = STOPPERS.map(u => ({ ...u, orders: [{ move: [120, 120] }] }));
    const s = WikiSim.create(e, { scene: { selected: ['ck'], units } }, FIXTURE);
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMMEX'));
    const armed = s.view();
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMORDERS'));
    const gen = s.view(), had = s.orders('ck').length;
    press(s, centre(FIXTURE, 'ARMGEN', 'ARMSTOP'));
    const stopped = s.view(), left = ['ck', 'pw', 'ack'].map(id => s.orders(id).length);
    // a mixed selection: the Peewee and the stand-in without canstop are stopped with the rest
    const m = WikiSim.create(e, { scene: { selected: ['ck', 'pw', 'rov'], units } }, FIXTURE);
    press(m, centre(FIXTURE, 'ARMGEN', 'ARMSTOP'));
    const mixed = ['ck', 'pw', 'rov', 'ack', 'tur'].map(id => m.orders(id).length);
    return armed.mode === 'build:ARMMEX' && gen.side && gen.side.screen === 'ARMGEN' && gen.mode === 'build:ARMMEX' && had === 2 &&
      stopped.mode === 'contextual' && same(left, [0, 2, 2]) && same(mixed, [0, 0, 0, 2, 1]);
  },
  /* the build button's arm (0x41AB89, 0x41AB9C) writes nothing to main+0x2CC6, so the Shift-placement bit a Shift-click
     set stays set through a new arm, and the Shift's release disarms (0x4992D8..0x49930F); without a placement in the
     hold the building stays armed */
  'a building armed after a Shift-placement in the same Shift hold is disarmed when Shift is released'(entry) {
    // each sim is done with before the next is made: simparity.mjs replays one at a time
    const hold = place => {
      const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE);
      press(s, centre(FIXTURE, 'ARMCK1', 'ARMMEX'));
      s.input({ down: 'shift' });
      if (place) { s.input({ move: [120, 400] }); s.input({ down: 'lmb' }); s.input({ up: 'lmb' }); }
      const placed = s.orders('ck').length;
      press(s, centre(FIXTURE, 'ARMCK1', 'ARMNEXT'));
      press(s, centre(FIXTURE, 'ARMCK2', 'ARMGEO'));
      const armed = s.view().mode;
      s.input({ up: 'shift' });
      return { placed, armed, after: s.view().mode };
    };
    const placed = hold(true), none = hold(false);
    return placed.placed === 1 && placed.armed === 'build:ARMGEO' && placed.after === 'contextual' &&
      none.placed === 0 && none.armed === 'build:ARMGEO' && none.after === 'build:ARMGEO';
  },
  /* a placement drag released over the sidebar: the handler still gets the release (IdleTick, 0x4999CB..0x499A04) and
     tagpu_place.c commits, but the build cursor 0x4197D0 runs only in the viewport, so the shape is the one the map
     last showed -- A to B -- not one stretched to the map's edge; the building stays armed until Shift's release */
  'a placement drag released over the sidebar orders the shape the map last showed'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, FIXTURE);
    press(s, centre(FIXTURE, 'ARMCK1', 'ARMMEX'));
    s.input({ down: 'shift' });
    s.input({ move: [120, 400] }); s.input({ down: 'lmb' }); s.input({ move: [200, 400] });
    const onMap = s.view().sites.map(t => t.x).join(' ');
    s.input({ move: [-60, 400] });
    const overSide = s.view().sites.map(t => t.x).join(' ');
    s.input({ up: 'lmb' });
    const orders = s.orders('ck').map(o => o.ox).join(' '), armed = s.view().mode;
    s.input({ up: 'shift' });
    // ARMMEX is 3 x 3: the cursor's sites start at cells 6 and 12, so the line is cells 6, 9 and 12
    return onMap === '120 168 216' && overSide === onMap && orders === '6 9 12' && armed === 'build:ARMMEX' &&
      s.view().mode === 'contextual';
  },
  /* the pump 0x4A9FD0 offers a press in file order: a button acting on the release lets the scan go on, so the LAST
     one holding the point takes it; a greyed or an inactive one takes none */
  'where buttons overlap the last one takes the press, and a greyed or inactive one takes none'(entry) {
    const mex = rectOf('ARMMEX'), armOver = gadgets => {
      const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, ck1With(gadgets));
      s.input({ move: rectCentre(mex) }); s.input({ down: 'lmb' });
      const held = s.view().side.state;
      s.input({ up: 'lmb' });
      return { mode: s.view().mode, held: gadgets.filter((g, i) => g.role === 'build' && held[i] === 1).map(g => g.name).join(' ') };
    };
    const after = armOver(beside('ARMMEX', geoAt(mex), false)), before = armOver(beside('ARMMEX', geoAt(mex), true));
    const grey = armOver(beside('ARMMEX', { name: 'IGPATCH', role: 'build', unit: null, grey: true, rect: mex }, false));
    const hidden = armOver(beside('ARMMEX', geoAt(mex, { hidden: true }), false));
    return after.mode === 'build:ARMGEO' && after.held === 'ARMGEO' && before.mode === 'build:ARMMEX' && before.held === 'ARMMEX' &&
      grey.mode === 'build:ARMMEX' && grey.held === 'ARMMEX' && hidden.mode === 'build:ARMMEX' && hidden.held === 'ARMMEX';
  },
  /* a tab acts on its own press and ends the scan, dropping an earlier button's capture: the FIRST tab holding the
     point wins, over a button before or after it */
  'where a tab overlaps, the first tab takes the press, over any button'(entry) {
    const tab = (gadgets, at) => {
      const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, ck1With(gadgets));
      s.input({ move: rectCentre(at) }); s.input({ down: 'lmb' });
      const down = s.view().side.screen;
      s.input({ up: 'lmb' });
      return { down, screen: s.view().side.screen, mode: s.view().mode };
    };
    const ord = rectOf('ARMORDERS'), bld = rectOf('ARMBUILD');
    // both tabs on ORDERS' rect: ORDERS first shows GEN on the press; BUILD first keeps the page
    const [orders, build, ...rest] = G1;
    const ordersFirst = tab([orders, { ...build, rect: ord }, ...rest], ord);
    const buildFirst = tab([{ ...build, rect: ord }, orders, ...rest], ord);
    // a live build button on BUILD's rect, before or after it: BUILD acts and drops any capture, so nothing arms
    const btnBefore = tab(beside('ARMBUILD', geoAt(bld), true), bld), btnAfter = tab(beside('ARMBUILD', geoAt(bld), false), bld);
    return ordersFirst.down === 'ARMGEN' && buildFirst.down === 'ARMCK1' && buildFirst.screen === 'ARMCK1' &&
      btnBefore.screen === 'ARMCK1' && btnBefore.mode === 'contextual' && btnAfter.screen === 'ARMCK1' && btnAfter.mode === 'contextual';
  },
  'a map drag released over the sidebar selects with the box clamped to the map'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: [] } }, FIXTURE);
    s.input({ move: [330, 150] }); s.input({ down: 'lmb' }); s.input({ move: [-60, 200] });
    const drag = s.view();
    s.input({ up: 'lmb' });
    const v = s.view();
    // the box is x 0..330, z 150..200: the Peewee (256, 176) and ARMCK (304, 176), not ARMACK at x 352
    return drag.drag && drag.drag.x1 === 0 && drag.drag.z1 === 200 && selIds(v) === 'pw ck' && v.side.screen === 'ARMGEN';
  },
  'a page three away goes by three NEXT presses, the longest trip (a 7-page variant of the fixture)'(entry) {
    // ARMMEX on page 4 of 7: three pages away by NEXT, four by PREV
    const blank = () => ({ ...FIXTURE.screens.ARMCK4, gadgets: FIXTURE.screens.ARMCK4.gadgets.map(g => g.role === 'build' ?
      { name: 'IGPATCH', role: 'build', unit: null, grey: true, rect: g.rect } : g) });
    const names = ['B1', 'B2', 'B3', 'ARMCK1', 'B5', 'B6', 'B7'];
    const L = fixtureWith({ ARMCK: { pages: names, orders: null } }, Object.fromEntries(names.filter(n => n[0] === 'B').map(n => [n, blank()])));
    const d = { id: 'k3', scene: { selected: ['ck'] }, inputs: [{ at: 1, build: 'ARMMEX' }], end: 1 + TRIP };
    const r = run(entry, d, L), notes = [];
    tripChecks(r, d, (c, what) => notes.push([!!c, what]), ['1>2>3>4']);
    return notes.every(n => n[0]);
  },
  'a build the layout cannot reach locks the demo on its tick'(entry) {
    const L = fixtureWith({}, { ARMCK1: { ...FIXTURE.screens.ARMCK1, gadgets: FIXTURE.screens.ARMCK1.gadgets.map(g => g.unit === 'ARMMEX' ?
      { ...g, unit: null, grey: true } : g) } });
    const s = WikiSim.play(entry, { id: 'locked', scene: { selected: ['ck'] }, inputs: [{ at: 5, build: 'ARMMEX' }], end: 200 }, L);
    const start = s.view().cursor;
    for (let i = 0; i < 300 && !s.done; i++) s.step();
    const v = s.view();
    return s.done && s.tick === 5 && v.locked === 'ARMMEX' && v.mode === 'contextual' && v.cursor.x === start.x && v.cursor.z === start.z;
  },
};

/* ---- the context (sim.js `context`, `resolve`, `binder`; decisions 12-18): the Interface Type's rows, the resolved
   script's data rule, G cells, locks for an unbound cast, and what a context or a casting is refused for. Each case
   names its context. The type-0 rows were MEASURED in game on 2026-10-28 except two [INFERRED] ones (sim.js
   `leftClick`), so these pin the sim to the engine's outcomes. */
const T0 = { side: 0, casting: null, iface: 0 }, T1 = { side: 0, casting: null, iface: 1 };
const click = (s, at, b, mods = []) => { s.input({ move: at }); for (const k of mods) s.input({ down: k }); s.input({ down: b }); s.input({ up: b }); for (const k of mods) s.input({ up: k }); };
const refuses = f => { try { f(); return false; } catch (e) { return e.message; } };
const ident = () => JSON.parse(JSON.stringify(CASTINGS.identity[0]));
const HALF = { id: 'half', type: 'ARMMEX', at: [120, 248], built: 0.5 };
const CTX_RULES = {
  'type 0: an order click lands at its release, where type 1 orders at its press'(entry) {
    const at = (iface, b) => {
      const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, null, { ...T1, iface });
      s.input({ move: [376, 72] }); s.input({ down: b });
      const down = kinds(s.orders('ck'));
      s.input({ up: b });
      return [down, kinds(s.orders('ck'))];
    };
    return same(at(1, 'rmb'), ['build:ARMMEX', 'build:ARMMEX']) && same(at(0, 'lmb'), ['', 'build:ARMMEX']);
  },
  'type 0: a left click moves the mobile selection, selects a unit of the player\'s, and does nothing on empty ground with no mobile unit selected'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['pw'] } }, null, T0);
    click(s, [200, 300], 'lmb');
    const moved = kinds(s.orders('pw')) === 'move' && selIds(s.view()) === 'pw';
    click(s, [304, 176], 'lmb');
    const picked = selIds(s.view()) === 'ck';
    click(s, [352, 176], 'lmb', ['shift']);
    const added = selIds(s.view()) === 'ck ack';
    const b = WikiSim.create(entry, { scene: { selected: ['geo'] } }, null, T0);
    click(b, [200, 300], 'lmb');
    return moved && picked && added && selIds(b.view()) === 'geo' && b.orders('geo').length === 0;
  },
  'type 0: a left click on an unfinished building of the player\'s assists it with a builder selected, else selects it'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck'], units: [HALF] } }, null, T0);
    click(s, HALF.at, 'lmb');
    const helped = kinds(s.orders('ck')) === 'build:ARMMEX' && selIds(s.view()) === 'ck';
    const p = WikiSim.create(entry, { scene: { selected: ['pw'], units: [HALF] } }, null, T0);
    click(p, HALF.at, 'lmb');
    return helped && selIds(p.view()) === 'half' && p.orders('pw').length === 0;
  },
  'type 0: a right press deselects, with Ctrl it keeps the selection, and with a building armed it only disarms'(entry) {
    const s = WikiSim.create(entry, { scene: { selected: ['ck', 'pw'] } }, null, T0);
    click(s, [200, 300], 'rmb', ['ctrl']);
    const kept = selIds(s.view()) === 'pw ck';
    s.input({ build: 'ARMMEX' });
    click(s, [200, 300], 'rmb');
    const disarmed = s.view().mode === 'contextual' && selIds(s.view()) === 'pw ck';
    click(s, [200, 300], 'rmb');
    return kept && disarmed && selIds(s.view()) === '' && s.orders('ck').length === 0 && s.orders('pw').length === 0;
  },
  /* the click window (sim.js CLICK_TIME, CLICK_BOX): a left release is a click when its press is under 25 ticks old
     and the release point under 32 world units from the press point on each axis, else a box, at both types. Every
     release lands beside ck's centre, on it, and every press on the same side of it, so a click there selects ck and
     a box from the press holds nothing and deselects */
  'the click window: a hold of 25 ticks or a release 32 units from the press is a box, a wobble of 5 to 31 is a click, at both types'(entry) {
    const sel = (iface, from, ticks) => {
      const s = WikiSim.create(entry, { scene: { selected: ['pw'] } }, null, { ...T1, iface });
      s.input({ move: from }); s.input({ down: 'lmb' });
      for (let i = 0; i < ticks; i++) s.step();
      s.input({ move: [306, 178] }); s.input({ up: 'lmb' });
      return selIds(s.view());
    };
    return [0, 1].every(i => sel(i, [306, 178], 24) === 'ck' && sel(i, [306, 178], 25) === '' &&
      sel(i, [311, 178], 0) === 'ck' && sel(i, [337, 178], 0) === 'ck' && sel(i, [306, 209], 0) === 'ck' &&
      sel(i, [338, 178], 0) === '' && sel(i, [306, 210], 0) === '');
  },
  'the resolved script: a press of a held button or a release of one not held refuses the demo with its tick'(entry) {
    const d = (inputs) => ({ id: 'rule', scene: { selected: ['ck'] }, inputs, end: 10 });
    const overlap = d([{ at: 1, down: 'select' }, { at: 2, down: 'order' }, { at: 3, up: 'order' }, { at: 4, up: 'select' }]);
    const t1 = !refuses(() => WikiSim.play(entry, overlap, null, T1)), t0 = refuses(() => WikiSim.play(entry, overlap, null, T0));
    const loose = refuses(() => WikiSim.play(entry, d([{ at: 6, up: 'cancel' }]), null, T1));
    return t1 && /at tick 2 the press of lmb/.test(t0) && /at tick 6 the release of rmb/.test(loose);
  },
  'a geothermal plant stands only with a vent under one of its G cells'(entry) {
    // the vent at cell 28,9; with Alt the ghost stands where the cursor puts it, footprint 4 x 4 first cell 28,9 or 27,8
    const ok = (casting, at) => {
      const s = WikiSim.create(entry, { scene: { selected: ['ck'] } }, null, { ...T1, casting });
      s.input({ build: 'ARMGEO' }); s.input({ down: 'alt' }); s.input({ move: at });
      return s.view().ghost.ok;
    };
    const shifted = CASTINGS.shifted[0];
    return ok(null, [480, 176]) && ok(ident(), [480, 176]) && !ok(shifted, [480, 176]) && ok(shifted, [464, 160]);
  },
  'the context: an Interface Type other than 0 or 1, or a side the layout lacks, is refused; without a layout the side is not read'(entry) {
    const d = { scene: { selected: ['ck'] } };
    const s = WikiSim.create(entry, d, FIXTURE, T0), n = WikiSim.create(entry, d, null, { ...T0, side: 5 });
    return !!refuses(() => WikiSim.create(entry, d, null, { ...T0, iface: 2 })) && !!refuses(() => WikiSim.create(entry, d, null, { side: 0, casting: null })) &&
      !!refuses(() => WikiSim.create(entry, d, FIXTURE, { ...T0, side: 1 })) && s.side === 0 && s.iface === 0 && s.view().side.index === 0 &&
      n.side === -1 && !n.view().side;
  },
  'a casting is refused whole when it breaks a bound'(entry) {
    const bad = f => { const c = ident(); f(c.casts); return !!refuses(() => WikiSim.create(entry, { scene: {} }, null, { ...T1, casting: c })); };
    return bad(k => { k.mex.footprint = [17, 3]; }) && bad(k => { k.geo.g = [[4, 0]]; }) && bad(k => { k.mex.g = [[0, 0]]; }) &&
      bad(k => { delete k.wall; }) && bad(k => { k.mex.type = '1234'; }) && bad(k => { k.mex.buildTime = 0; }) &&
      bad(k => { k.mex.type = 'X'.repeat(24); }) && bad(k => { k.builder.builds = ['mex', 'factory']; }) && bad(k => { k.navy = null; }) &&
      !bad(k => { k.mex.type = 'X'.repeat(23); });
  },
  'a scene its bound footprints do not fit refuses the demo'(entry) {
    const c = ident(); c.casts.geo.footprint = [16, 16];
    return /geo's ARMGEO does not fit the scene/.test(refuses(() => WikiSim.create(entry, { scene: {} }, null, { ...T1, casting: c })));
  },
  'the bound entry renames every bound type, keeps the units\' order, and builds by cast'(entry) {
    const c = ident();
    for (const k of Object.values(c.casts)) k.type = `X${k.type}`;
    const b = WikiSim.bindEntry(entry, c), demo = entry.demos.find(d => d.id === 'click');
    const r = run(entry, demo, null, { ...T1, casting: c }), built = fresh(r.last.v, 'XARMMEX');
    return same(Object.keys(b.units), Object.keys(entry.units).map(t => `X${t}`)) && same(b.units.XARMCK.builds, ['XARMMEX', 'XARMGEO']) &&
      b.units.XARMCK.name === 'Construction KBot' && Object.values(b.scenes).every(g => g.units.every(u => u.type[0] === 'X')) &&
      built.length === 1 && built[0].built === 1 && !r.last.v.units.some(u => u.type === 'ARMMEX');
  },
};
const CTX_SHAPES_RULES = {
  'an unbound cast stops the demo: a scene unit of it on tick 0, a build of it on its tick, with the sidebar or without'(entry) {
    const casting = CASTINGS.unbound[0];
    const scene = WikiSim.play(entry, { id: 'u', scene: { units: [{ id: 'w', type: 'ARMDRAG', at: [200, 300] }] }, inputs: [], end: 50 }, FIXTURE, { ...T1, casting });
    const later = layout => {
      const s = WikiSim.play(entry, { id: 'u', inputs: [{ at: 7, build: 'ARMDRAG' }], end: 200 }, layout, { ...T1, casting });
      while (!s.done) s.step();
      return s.tick === 7 && s.view().locked === 'ARMDRAG' && s.view().mode === 'contextual';
    };
    return scene.done && scene.tick === 0 && scene.view().locked === 'ARMDRAG' && later(null) && later(FIXTURE);
  },
};

/* THE RECLAIM SWEEP's demos (research/notes/ingame-wiki-reclaim-plan.md): what each ring marked, who was ordered what,
   and whether reclaim was still armed after it. A frame after an input's tick shows what the input did. */
const fIds = os => (os || []).filter(o => o.k === 'reclaim' && o.feat !== undefined).map(o => o.feat);
const featName = (r, id) => (r.all[0].v.feats.find(f => f.id === id) || {}).name || '';
const rockOrTree = (r, ids, w) => ids.every(id => featName(r, id).startsWith(w));
// the features whose centre the ring holds, and of those the ones of kind w (rock or tree)
const inRing = (r, ring) => r.all[0].v.feats.filter(f => Math.hypot((f.ox + f.fx / 2) * 16 - ring.x, (f.oz + f.fz / 2) * 16 - ring.z) <= ring.r).map(f => f.id);
const RECLAIM = {
  click(r, d, ok) {
    const t = ups(d, 'lmb')[0], f = r.frames[t + 1];
    ok(r.frames[t - 1].v.mode === 'reclaim', 'reclaim armed by the trip to RECLAIM before the click');
    ok(fIds(f.o.a).join() === 'lone' && f.v.mode === 'contextual', `the click ordered the rock and put reclaim away (${fIds(f.o.a)}, ${f.v.mode})`);
    ok(r.log.some(l => l.what === 'reclaimed' && l.id === 'lone') && !r.last.v.feats.some(x => x.id === 'lone'), 'the rock was reclaimed and is gone');
    ok(r.all.every(f => !f.v.reclaim || !f.v.reclaim.ring), 'no ring was drawn');
  },
  kind(r, d, ok) {
    const seq = [];
    for (const f of r.all) if (f.v.reclaim && seq[seq.length - 1] !== f.v.reclaim.kind) seq.push(f.v.reclaim.kind);
    ok(seq.join(' ') === 'all metal energy all metal', `E cycled METAL, ENERGY, ALL and the popup's row picked METAL (${seq.join(' ')})`);
    ok(r.all.filter(f => f.v.reclaim).every(f => f.v.reclaim.popup), 'the popup showed while reclaim was armed');
    ok(r.all.every(f => !(f.o.a || []).length), 'nothing was ordered');
  },
  metal(r, d, ok) { ringCheck(r, d, ok, 'rock', 'tree'); },
  energy(r, d, ok) { ringCheck(r, d, ok, 'tree', 'rock'); },
  shift(r, d, ok) {
    const [u1, u2] = ups(d, 'lmb'), s = ups(d, 'shift')[0], f1 = r.frames[u1 + 1], f2 = r.frames[u2 + 1];
    const q1 = [...fIds(f1.o.a), ...fIds(f1.o.b)], q2 = [...fIds(f2.o.a), ...fIds(f2.o.b)];
    ok(q1.length && f1.v.mode === 'reclaim', `the first ring queued ${q1.length} and reclaim stayed armed`);
    ok(q1.every(id => q2.includes(id) || r.log.some(l => l.what === 'reclaimed' && l.id === id && l.t <= u2)) && q2.length > q1.length - 2,
      `the second ring added to the first's (${q1} then ${q2})`);
    ok(r.frames[s + 1].v.mode === 'contextual', 'letting go of Shift put reclaim away');
  },
  replace(r, d, ok) {
    const t = ups(d, 'lmb')[0], before = r.frames[t - 1], after = r.frames[t + 1];
    ok((before.o.a || []).some(o => o.k === 'move'), 'moves were queued before the ring');
    ok((after.o.a || []).length && (after.o.a || []).every(o => o.k === 'reclaim' && o.feat !== undefined), `the ring replaced it with reclaims (${(after.o.a || []).map(o => o.k)})`);
    ok(rockOrTree(r, fIds(after.o.a), 'rock'), 'rocks only, METAL picked');
  },
  cancel(r, d, ok) {
    const [c1, c2] = presses(d, 'cancel'), [u1, u2] = ups(d, 'lmb');
    ok(r.frames[c1 - 1].v.reclaim && r.frames[c1 - 1].v.reclaim.ring, 'a ring was being dragged');
    ok(r.frames[c1 + 1].v.reclaim && !r.frames[c1 + 1].v.reclaim.ring && r.frames[c1 + 1].v.mode === 'reclaim', 'the cancel ended the ring and kept reclaim armed');
    ok(r.frames[u1 + 1].v.mode === 'reclaim' && !(r.frames[u1 + 1].o.a || []).length, 'its release ordered nothing');
    ok(r.frames[u2 - 1].v.reclaim.ring && !r.frames[u2 - 1].v.reclaim.targets.length, 'the second ring holds nothing');
    ok(r.frames[u2 + 1].v.mode === 'reclaim' && !(r.frames[u2 + 1].o.a || []).length, 'the empty ring ordered nothing and kept reclaim armed');
    ok(r.frames[c2 + 1].v.mode === 'contextual', 'a cancel outside a drag put reclaim away');
  },
};
// a ring of kind w, in a field holding features of kind x too: only w marked, every target ordered once between both
function ringCheck(r, d, ok, w, x) {
  const t = ups(d, 'lmb')[0], pre = r.frames[t - 1].v.reclaim, post = r.frames[t + 1];
  const T = (pre && pre.targets) || [], inside = pre && pre.ring ? inRing(r, pre.ring) : [];
  ok(T.length >= 4 && rockOrTree(r, T, w), `the ring marked ${w}s only (${T})`);
  ok(inside.some(id => featName(r, id).startsWith(x)) && !T.some(id => featName(r, id).startsWith(x)), `the ${x}s inside it were not marked`);
  const a = fIds(post.o.a), b = fIds(post.o.b), got = [...a, ...b].sort();
  ok(got.join() === [...T].sort().join() && a.length && b.length, `every target ordered once, shared between both (${a} / ${b})`);
  ok(post.v.mode === 'contextual', 'released without Shift: reclaim put away');
  ok(r.log.some(l => l.what === 'reclaimed' && T.includes(l.id)), 'a target was reclaimed before the end');
}

const CHECKS = { quickbuild: QUICKBUILD, buildshapes: BUILDSHAPES, reclaim: RECLAIM };
// the sidebar's rules use quickbuild's scene: two builder types and a Peewee
/* THE SOUND the player's own actions make (tagpu_wikisim.h, THE SOUND), which no demo shows: a click that selects, a box
   of several, a placement on a refused site and one on a deposit, with the sounds of the gfx packs under
   tools/wiki/mockup/gfx/ (gitignored: the units' categories are the player's install's), the source simparity.mjs gives
   the C side as well. Without a pack there is no source and the rule says so. */
const GFX_PACKS = () => {
  const d = `${ROOT}tools/wiki/mockup/gfx`;
  return fs.existsSync(d) ? fs.readdirSync(d).sort().map(x => `${d}/${x}`).filter(x => fs.existsSync(`${x}/scene.json`)) : [];
};
const SOUND_RULES = {
  'the player\'s clicks say select, SelectMultipleUnits, notoktobuild and oktobuild, one reply a second'(entry) {
    const snd = soundSource(GFX_PACKS());
    if (!Object.keys(sounds(GFX_PACKS())).length) return 'no gfx pack: no sound source';
    const s = WikiSim.create(entry, { scene: { selected: [] } }, FIXTURE, { ...T1, snd });
    const names = v => v.sound.events.map(([, k, n]) => (k ? `kind${k}` : WikiSim.SND_NAMES[n])).join(' ');
    const steps = n => { for (let i = 0; i < n; i++) { s.step(); s.view(); } };
    click(s, [304, 176], 'lmb');                                   // select ck: heard on the next tick's consumer
    steps(2);
    click(s, [352, 176], 'lmb');                                   // select ack within the gap: queued, then text only
    steps(40);
    s.input({ move: [290, 160] }); s.input({ down: 'lmb' }); s.input({ move: [370, 190] }); s.input({ up: 'lmb' });
    steps(2);
    s.input({ build: 'ARMMEX' });
    click(s, [272, 96], 'lmb', ['alt']);                           // on the geothermal plant, unsnapped: refused
    click(s, [376, 72], 'lmb');                                    // deposit B: placed
    steps(400);
    const v = s.view();
    return `${v.sound.total} events: ${names(v)}`;
  },
  /* MobileBuild's first failure of its site test says cant once an order ("Waiting for target area to clear",
     0x403CDF), and a build order says no ok (MobileBuild never calls 0x438880): the Peewee stands on the site, the
     constructor says cant once while it steps off, then builds */
  'a builder whose site a unit stands on says cant once, no ok, and builds once it is clear'(entry) {
    const snd = soundSource(GFX_PACKS());
    if (!Object.keys(sounds(GFX_PACKS())).length) return 'no gfx pack: no sound source';
    const s = WikiSim.create(entry, { scene: { selected: [] } }, FIXTURE, { ...T1, snd });
    click(s, [304, 176], 'lmb');                                   // select ck
    for (let i = 0; i < 40; i++) s.step();
    s.input({ build: 'ARMMEX' });
    click(s, [256, 176], 'lmb');                                   // on the Peewee
    let cant = 0, ok = 0, seen = 0;
    for (let i = 0; i < 600; i++) {
      s.step();
      const v = s.view();
      for (const [, k] of v.sound.events.slice(Math.max(0, v.sound.events.length - (v.sound.total - seen)))) {
        if (k === 7) cant++;
        if (k === 5) ok++;
      }
      seen = v.sound.total;
    }
    const built = s.view().units.some(u => u.type === 'ARMMEX' && u.built > 0 && u.id !== 'mex');
    return cant === 1 && ok === 0 && built;
  },
};
export const ENTRY_RULES = { quickbuild: { ...RULES, ...SIDE_RULES, ...CTX_RULES, ...SOUND_RULES }, buildshapes: { ...BUILDSHAPES_RULES, ...CTX_SHAPES_RULES } };

/* decision 12 for an unbound cast: the tick and the authored type a demo stops on in this casting -- tick 0 for a
   scene unit of an unbound cast, else the first `build` of one -- or null when it plays to its end */
function expectedLock(entry, d, casting) {
  if (!casting) return null;
  const off = Object.keys(entry.units).filter(t => casting.casts[entry.units[t].cast] === null);
  const scene = [...WikiSim.sceneOf(entry, d).units, ...((d.scene && d.scene.units) || [])].find(u => off.includes(u.type));
  if (scene) return { t: 0, type: scene.type };
  const b = d.inputs.map((e, i) => [e, i]).sort((x, y) => x[0].at - y[0].at || x[1] - y[1]).map(p => p[0]).find(e => off.includes(e.build));
  return b ? { t: b.at, type: b.build } : null;
}
/* THE TURNED-OFF RULE (decision 24): an entry whose own `lockedIn` names the pack's mod is shown in that mod under
   the "Turned off in this mod" badge (wiki.js `locked`, the same list and the same name), which already tells the
   player its demos do not apply there. Its demos still play, and must play to their end without being refused, but
   their claims are not checked in that mod. The rule reads the entry's list alone, so it says nothing about a mod
   the entry does not name, and nothing without a pack: the fixture runs check every claim. */
const turnedOff = entry => !!PACK && Array.isArray(entry.lockedIn) && entry.lockedIn.includes(PACK.mod);
// what a frame shows, for comparing two plays of one demo
const frameKey = f => JSON.stringify([f.t, f.v, f.o]);

function main() {
const packDir = arg('--pack');
if (packDir !== undefined) {
  PACK_DIR = packDir.replace(/\/$/, '');
  PACK = JSON.parse(fs.readFileSync(`${PACK_DIR}/side.json`, 'utf8'));
}
LAYOUT = process.argv.includes('--null-layout') ? null : PACK ? PACK.layout : FIXTURE;
const tree = JSON.parse(fs.readFileSync(`${ROOT}art/wiki/tree.json`, 'utf8'));
const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
const ids = tree.tree.flatMap(leaves);
let failed = 0;
const tally = {};
for (const c of contexts()) {
  tally[c.label] = { pass: 0, fail: 0, locked: [], off: [] };
  for (const id of ids) {
    const entry = JSON.parse(fs.readFileSync(`${ROOT}art/wiki/entries/${id}.json`, 'utf8'));
    for (const d of entry.demos) {
      const notes = [], ok = (cond, what) => notes.push([!!cond, what]), lock = expectedLock(entry, d, c.ctx.casting);
      const off = turnedOff(entry);
      try {
        const r = run(entry, d, c.layout, c.ctx);
        if (TRACE) console.log(`  ${d.id} log:`, r.log.map(e => `${e.t} ${e.what} ${e.type}@${e.ox},${e.oz}`).join('; '));
        // a demo's ticks are its clock's, which a wait holds while the sim's own tick runs on
        ok(!r.last.v.stalled, `no wait ran past its max (clock ${r.last.v.clock}, waiting ${r.last.v.waiting})`);
        const met = r.all.findIndex(f => !apart(f.v));
        ok(met < 0, `no two bodies overlap${met < 0 ? '' : ` (tick ${r.all[met].t})`}`);
        const notes = r.sim.cobNotes().filter(n => n);
        ok(!notes.length, `no type's script quarantined${notes.length ? ` (${notes.join('; ')})` : ''}`);
        if (off) {
          ok(r.last.v.clock === d.end && r.last.v.locked === null, `played to its end, tick ${d.end}, unlocked (tick ${r.last.v.clock}, locked ${r.last.v.locked})`);
        } else if (lock) {
          ok(r.sim.done && r.last.v.clock === lock.t && r.last.v.locked === lock.type,
            `stops on tick ${lock.t} under the badge, ${lock.type} unbound (tick ${r.last.v.clock}, locked ${r.last.v.locked})`);
          ok(r.all.slice(0, -1).every(f => f.v.locked === null), 'no earlier frame is locked');
          tally[c.label].locked.push(`${id}/${d.id}@${lock.t}`);
        } else {
          ok(r.all.every(f => f.v.locked === null), 'no frame is locked');
          const check = (CHECKS[id] || {})[d.id];
          if (!check) ok(false, 'no check for this demo');
          else check(r, d, ok);
          if (r.layout) tripChecks(r, d, ok, (TRIP_PATHS[id] || {})[d.id]);
          // the identity casting binds every cast to the entry's own type with its own figures: nothing may change
          if (c.name === 'identity') {
            const a = run(entry, d, c.layout, { ...c.ctx, casting: null });
            const k = a.all.findIndex((f, i) => !r.all[i] || frameKey(f) !== frameKey(r.all[i]));
            ok(k < 0 && a.all.length === r.all.length, `plays exactly as the entry's own types${k < 0 ? '' : ` (first difference on tick ${k})`}`);
          }
        }
      } catch (e) { ok(false, `threw: ${e.stack}`); }
      const pass = notes.every(n => n[0]);
      if (!pass) failed++;
      if (pass && off) tally[c.label].off.push(`${id}/${d.id}`);
      else tally[c.label][pass ? 'pass' : 'fail']++;
      const why = off ? ` (turned off in ${PACK.mod}: played, claims not checked)` : lock ? ` (stops on tick ${lock.t}: ${lock.type} unbound)` : '';
      console.log(`${!pass ? 'FAIL' : off ? 'OFF ' : 'PASS'} [${c.label}] ${id}/${d.id} -- ${d.title}${why}`);
      for (const [cond, what] of notes) if (!cond || TRACE) console.log(`     ${cond ? 'ok  ' : 'FAIL'} ${what}`);
    }
  }
}
for (const id of PACK ? [] : ids) {
  const entry = JSON.parse(fs.readFileSync(`${ROOT}art/wiki/entries/${id}.json`, 'utf8'));
  for (const [what, check] of Object.entries(ENTRY_RULES[id] || {})) {
    let pass = false;
    try { pass = check(entry); } catch (e) { console.log(e.stack); }
    if (!pass) failed++;
    console.log(`${pass ? 'PASS' : 'FAIL'} rule -- ${what}`);
  }
}
for (const [label, t] of Object.entries(tally))
  console.log(`[${PACK ? `pack ${PACK.mod}${LAYOUT ? '' : ', no sidebar'}` : LAYOUT ? 'fixture' : 'no sidebar'} ${label}] ${t.pass} pass, ${t.fail} fail` +
    `${t.off.length ? `; locked: claims not checked, ${t.off.length} played (${[...new Set(t.off.map(x => x.split('/')[0]))].join(', ')} turned off in ${PACK.mod})` : ''}` +
    `${t.locked.length ? `; stopped under the badge: ${t.locked.join(' ')}` : ''}`);
console.log(failed ? `${failed} check(s) failed` : 'all demos and rules pass');
process.exit(failed ? 1 : 0);   // also ends the scripts' host
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main();
