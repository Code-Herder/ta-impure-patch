/* simparity.mjs -- the in-game wiki's parity check: the DLL's simulation, tagpu/ddraw/src/tagpu_wikisim.c over
   tagpu_wikidata.c's reading of the archive the DLL embeds, against the mockup's tools/wiki/mockup/sim.js, tick for
   tick and field by field.

   usage: node tools/wiki/simparity.mjs [--wine]
     exit 0 when every demo and every rule case agrees and every malformed case loads, 1 when one does not, 2 when the C side cannot be built or run
   usage: node tools/wiki/simparity.mjs --hash [--layout FILE|null]
     prints `<entry>/<demo> <hash>` for every demo: sim.js's state hash on its last tick, 16 hex digits, the number
     the DLL's tagpu_wikisim_hash must give for the same demo played to its end with the same sidebar layout (by
     default art/wiki/side/fixture.json; `null` for none). Builds nothing.
   usage: node tools/wiki/simparity.mjs --try DUMP
     prints sim.js's state hash of Try it yourself fed the inputs the game's state dump logged (tools/wiki_regression.py
     try), the number the dump's own hash must equal. The layout is the dump's `layout` when it records one, else the
     fixture. Builds nothing.

   It packs art/wiki/ as the Makefile does (tools/hudpak.py --set wiki), builds tagpu/ddraw/tests/wikitest.c with the
   real sources, and plays every demo of every entry the tree names through both, comparing every field of the view,
   every unit's orders and the state hash on every tick; the map, which nothing writes once the scene is laid out, on
   the first and the last tick. It plays them three times: with the sidebar of art/wiki/side/fixture.json, the layout
   the wiki falls back to, with fixture-tall.json's 768-row column, and with none (both C sides read the layouts out
   of the archive). It then plays simcheck.mjs's rule cases -- the rules no demo shows -- through both: each case is
   run against sim.js with every call recorded, `create` and `play` with the layout each was given, the recording is
   replayed through the C sim, and every answer the case read (each view, with the state hash after it, orders, done)
   is compared. For each demo and case it names the first differing tick or call and field. It compares the build
   trip's constants. Then the refusal cases: data, castings and layouts each side must refuse or accept alike -- the
   demo's `sidebar`, the build trip's data rule, the text's tokens, the actions' data rule at each interface type, a
   unit's G cells, every bound of the casting and of the layout on both sides of it -- each made into a sim by
   text.js's checkEntry and sim.js and by the C readers and init. Then the data read back: every casting of
   tools/wiki/casting-fixture.json and of every pack present under tools/wiki/mockup/gfx/side/ (gitignored, built
   by tools/wiki/sidepacks.py), and every layout of art/wiki/side/ and of those packs, read by tagpu_wikidata.c and
   compared field by field with what sim.js's checkCasting and checkLayout accept, a refused casting by its message.
   Last, wikitest's `malformed` cases load data the loader must survive -- a tree or an entry built wrong on
   purpose -- and each must load as stated.

   stateHash is tagpu_wikisim_hash's twin, fed from view() and orders() in the order and the encoding
   tagpu_wikisim.h states.

   By default the C side is built natively with gcc, AddressSanitizer and UndefinedBehaviorSanitizer. --wine builds it
   with the DLL's own compiler and flags (i686-w64-mingw32-gcc, the simulation and the loader on SSE2 as the Makefile
   compiles them)
   and runs it under Wine (WINEPREFIX: the main checkout's wineprefix/), which checks the arithmetic the game runs. */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSim = require('./mockup/sim.js');
const WikiText = require('./mockup/text.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const SIDE = name => JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/side/${name}.json`), 'utf8'));
const FIXTURE = SIDE('fixture');
const WINE = process.argv.includes('--wine');
const SRCS = ['src/tagpu_wikidata.c', 'src/tagpu_json.c', 'src/tagpu_pak.c', 'src/tagpu_placegeo.c'];

/* ---- the state hash: 64-bit FNV-1a, its halves in two uint32s so that no BigInt is needed ---- */
const KEYS = ['shift', 'alt', 'ctrl', 'z', 'x', 'lmb', 'rmb'];               // WIKI_KEY_* order
const ORDER = { move: 0, build: 1, reclaim: 2, guard: 3 };                   // WIKI_ORDER_*
const SHAPE = { line: 1, axis: 2, rect: 3, hollow: 4, ring: 5 };             // PLACE_*, PLACE_NONE 0
const STATE = { place: 0, refused: 1, remove: 2 };                           // WIKI_SITE_*
const utf8 = new TextEncoder(), f64 = new DataView(new ArrayBuffer(8));
function stateHash(sim, v) {
  let hi = 0xcbf29ce4, lo = 0x84222325;
  // h = (h ^ b) * (2^40 + 0x1b3) mod 2^64
  const byte = b => {
    lo = (lo ^ b) >>> 0;
    const t0 = (lo & 0xffff) * 0x1b3, t1 = (lo >>> 16) * 0x1b3 + (t0 >>> 16);
    hi = (Math.imul(hi, 0x1b3) + (t1 >>> 16) + (lo << 8)) >>> 0;
    lo = (((t1 & 0xffff) << 16) | (t0 & 0xffff)) >>> 0;
  };
  const int = n => { for (let k = 0; k < 4; k++) byte((n >>> (8 * k)) & 255); };
  const dbl = x => { f64.setFloat64(0, x, true); for (let k = 0; k < 8; k++) byte(f64.getUint8(k)); };
  const str = s => { const b = utf8.encode(s || ''); int(b.length); for (const c of b) byte(c); };
  int(sim.tick); int(sim.done ? 1 : 0);
  const m = v.map;
  int(m.w); int(m.h); dbl(m.surfaceMetal);
  for (const x of m.metal) dbl(x);
  for (const x of m.fog) int(x);
  int(m.vents.length); for (const p of m.vents) { dbl(p.x); dbl(p.z); }
  int(v.units.length);
  for (const u of v.units) {
    str(u.id); str(u.type); int(u.owner); dbl(u.x); dbl(u.z); dbl(u.hx); dbl(u.hz); int(u.selected ? 1 : 0); dbl(u.built); dbl(u.reclaimed);
    str(u.label);
    const os = sim.orders(u.id);
    int(os.length);
    for (const o of os) {
      int(ORDER[o.k]);
      if (o.k === 'move') { dbl(o.x); dbl(o.z); }
      else if (o.k === 'build') { str(o.type); int(o.ox); int(o.oz); }
      else str(o.target);
    }
  }
  int(v.ghost ? 1 : 0);
  if (v.ghost) { str(v.ghost.type); dbl(v.ghost.x); dbl(v.ghost.z); int(v.ghost.ok ? 1 : 0); }
  int(v.shape ? SHAPE[v.shape] : 0); int(v.sites.length);
  for (const s of v.sites) { dbl(s.x); dbl(s.z); int(STATE[s.state]); }
  dbl(v.cursor.x); dbl(v.cursor.z);
  for (const k of KEYS) int(v.keys.includes(k) ? 1 : 0);
  str(v.mode === 'contextual' ? '' : v.mode.slice('build:'.length));
  int(v.spacing ?? -1);
  int(v.drag ? 1 : 0); if (v.drag) { dbl(v.drag.x0); dbl(v.drag.z0); }
  str(v.caption);
  int(v.side ? 1 : 0);
  if (v.side) {
    const d = v.side;
    int(d.h); str(d.screen); str(d.unit); int(d.page); int(d.state.length); for (const x of d.state) int(x); str(d.under);
  }
  str(v.locked);
  return hi.toString(16).padStart(8, '0') + lo.toString(16).padStart(8, '0');
}

class CannotRun extends Error {}

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { maxBuffer: 1 << 30, encoding: 'utf8', ...opts });
  if (r.error || r.status !== 0)
    throw new CannotRun(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ` (${r.status})`}\n${r.stderr || ''}`);
  return r.stdout;
}

function build(tmp) {
  if (WINE) {
    // the simulation and the loader, which normalises the scenes' facings, on SSE2 as the Makefile compiles them
    const exe = path.join(DDRAW, 'tests/wikitest.exe'), sse = ['src/tagpu_wikisim.c', 'src/tagpu_wikidata.c'];
    const cc = 'i686-w64-mingw32-gcc', flags = ['-Iinc', '-Isrc', '-O2', '-Wall', '-std=c99'];
    const objs = sse.map(src => path.join(tmp, path.basename(src, '.c') + '.o'));
    sse.forEach((src, i) => run(cc, [...flags, '-msse2', '-mfpmath=sse', '-mincoming-stack-boundary=2', '-c', '-o', objs[i], src], { cwd: DDRAW }));
    run(cc, [...flags, '-static', '-o', exe, 'tests/wikitest.c', ...objs, ...SRCS.filter(src => !sse.includes(src))], { cwd: DDRAW });
    const common = run('git', ['rev-parse', '--git-common-dir'], { cwd: ROOT }).trim();
    const env = { ...process.env };
    env.WINEPREFIX ??= path.join(path.resolve(ROOT, common), '..', 'wineprefix');
    env.WINEDEBUG ??= '-all';
    return (args) => run('wine', [exe, ...args], { cwd: tmp, env });
  }
  const exe = path.join(DDRAW, 'tests/wikitest-native.exe');   // `.exe` so the tree's *.exe ignore covers it
  run('gcc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Isrc', '-O1', '-g', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all', '-o', exe, 'tests/wikitest.c', 'src/tagpu_wikisim.c', ...SRCS, '-lm'], { cwd: DDRAW });
  return (args) => run(exe, args, { cwd: tmp });
}

/* the first difference between the C answer `c` and sim.js's `j`, as a path and the two values; null when equal */
function diff(c, j, at) {
  if (typeof c === 'number' && typeof j === 'number') return c === j ? null : { at, c, j };
  if (c === null || j === null || typeof c !== 'object' || typeof j !== 'object') return c === j ? null : { at, c, j };
  if (Array.isArray(c) !== Array.isArray(j)) return { at, c, j };
  if (Array.isArray(c)) {
    if (c.length !== j.length) return { at: `${at}.length`, c: c.length, j: j.length };
    for (let i = 0; i < c.length; i++) { const d = diff(c[i], j[i], `${at}[${i}]`); if (d) return d; }
    return null;
  }
  for (const k of new Set([...Object.keys(c), ...Object.keys(j)])) {
    if (!(k in c) || !(k in j)) return { at: `${at}.${k}`, c: k in c ? c[k] : '(absent)', j: k in j ? j[k] : '(absent)' };
    const d = diff(c[k], j[k], `${at}.${k}`);
    if (d) return d;
  }
  return null;
}
const show = v => { const s = JSON.stringify(v); return s.length > 160 ? `${s.slice(0, 157)}...` : s; };
const lines = out => out.split(/\r?\n/).filter(l => l.length);

if (process.argv.includes('--hash')) {
  const at = process.argv.indexOf('--layout'), file = at >= 0 ? process.argv[at + 1] : null;
  const layout = !file ? FIXTURE : file === 'null' ? null : JSON.parse(fs.readFileSync(file, 'utf8'));
  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  for (const id of new Set(tree.tree.flatMap(leaves))) {
    const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
    for (const demo of entry.demos) {
      const sim = WikiSim.play(entry, demo, layout);
      while (!sim.done) sim.step();
      console.log(`${id}/${demo.id} ${stateHash(sim, sim.view())}`);
    }
  }
  process.exit(0);
}

/* --try FILE: Try it yourself as the game played it. FILE is the wiki's state dump (tagpu_wiki.json): the entry, the
   demo whose scene Try it took, the tick, and `try_log`, every input the game thread gave its simulation with the
   tick it was given at. sim.js's create() on the same scene is fed the same inputs at the same ticks and stepped to
   the dump's tick; its state hash is printed, the number the dump's `hash` must equal. */
const tryAt = process.argv.indexOf('--try');
if (tryAt >= 0) {
  const dump = JSON.parse(fs.readFileSync(process.argv[tryAt + 1], 'utf8'));
  const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${dump.entry}.json`), 'utf8'));
  const demo = entry.demos.find(d => d.id === dump.demo);
  if (!demo) { console.error(`simparity --try: no demo ${dump.entry}/${dump.demo}`); process.exit(2); }
  const sim = WikiSim.create(entry, demo, 'layout' in dump ? dump.layout : FIXTURE);
  for (const [t, kind, a, b] of dump.try_log) {
    while (sim.tick < t) sim.step();
    sim.input(kind === 'move' ? { move: [a, b] } : { [kind]: a });
  }
  while (sim.tick < dump.tick) sim.step();
  console.log(stateHash(sim, sim.view()));
  process.exit(0);
}

/* ---- the refusal cases: each a scripted flag, a layout, a context (interface type and casting) and an entry whose
   first demo the sim is made from; both sides must accept it or refuse it alike, and as `want` says. The C sim takes
   no context: it plays the entry's own types at interface type 1, and the casting's reader and the actions' data
   rule are asked beside it (tests/wikitest.c refuse_case). So the cases with a casting change only what the
   entry does not name (or give the authored types back), and the interface type decides only the data rule. ---- */
const CASTINGS = JSON.parse(fs.readFileSync(path.join(ROOT, 'tools/wiki/casting-fixture.json'), 'utf8')).castings;
function refusalCases(entry) {
  const clone = v => JSON.parse(JSON.stringify(v));
  const T = WikiSim.TRIP, base = { id: 'm', scene: { selected: ['ck'] }, inputs: [{ at: 5, build: 'ARMMEX' }], end: 5 + T };
  const cases = [];
  const demo = (name, want, d, scripted = 1, ctx = {}) =>
    cases.push({ name, want, scripted, layout: FIXTURE, iface: 1, casting: null, ...ctx, entry: { ...entry, demos: [{ ...base, ...d }] } });
  const lay = (name, want, edit) => { const L = clone(FIXTURE); edit(L); cases.push({ name, want, scripted: 1, layout: L, iface: 1, casting: null, entry: { ...entry, demos: [base] } }); };
  const text = (name, want, edit) => { const e = clone(entry); edit(e); cases.push({ name, want, scripted: 1, layout: FIXTURE, iface: 1, casting: null, entry: { ...e, demos: [{ ...base, ...(e.demos && e.demos.base) }] } }); };
  const cast = (name, want, edit) => { const C = clone(CASTINGS.identity[0]); edit(C); cases.push({ name, want, scripted: 1, layout: FIXTURE, iface: 1, casting: C, entry: { ...entry, demos: [base] } }); };
  demo('a build trip with room', 'accepted', {});
  demo('sidebar a string', 'refused', { sidebar: 'yes' });
  demo('sidebar a number', 'refused', { sidebar: 1 });
  demo('sidebar null', 'refused', { sidebar: null });
  demo('sidebar false: no trips, so an input inside one is fine', 'accepted', { sidebar: false, inputs: [{ at: 5, build: 'ARMMEX' }, { at: 6, move: [10, 10] }] });
  demo('sidebar true', 'accepted', { sidebar: true });
  demo('an input inside a trip', 'refused', { inputs: [{ at: 5, build: 'ARMMEX' }, { at: 50, down: 'lmb' }] });
  demo('an input after the build in its tick', 'refused', { inputs: [{ at: 5, build: 'ARMMEX' }, { at: 5, move: [10, 10] }] });
  demo('an input before the build in its tick', 'accepted', { inputs: [{ at: 5, move: [10, 10] }, { at: 5, build: 'ARMMEX' }] });
  demo('a caption inside a trip', 'accepted', { inputs: [{ at: 5, build: 'ARMMEX' }, { at: 30, caption: 'x' }] });
  demo('an input on the trip\'s last tick', 'accepted', { inputs: [{ at: 5, build: 'ARMMEX' }, { at: 5 + T, move: [10, 10] }], end: 200 });
  demo('an input one tick inside the trip\'s end', 'refused', { inputs: [{ at: 5, build: 'ARMMEX' }, { at: 4 + T, up: 'rmb' }], end: 200 });
  demo('a key no input names, inside a trip', 'refused', { inputs: [{ at: 5, build: 'ARMMEX' }, { at: 9, other: 1 }] });
  demo('build null', 'refused', { inputs: [{ at: 5, build: null }] });
  demo('build empty', 'refused', { inputs: [{ at: 5, build: '' }] });
  demo('build a number', 'refused', { inputs: [{ at: 5, build: 7 }] });
  demo('a build carrying a move', 'refused', { inputs: [{ at: 5, build: 'ARMMEX', move: [1, 1] }] });
  demo('a build with the left button held', 'refused', { inputs: [{ at: 1, down: 'lmb' }, { at: 5, build: 'ARMMEX' }] });
  demo('a build after the left button\'s release', 'accepted', { inputs: [{ at: 1, down: 'lmb' }, { at: 2, up: 'lmb' }, { at: 5, build: 'ARMMEX' }] });
  demo('a build with select held', 'refused', { inputs: [{ at: 1, down: 'select' }, { at: 5, build: 'ARMMEX' }] });
  demo('the end inside a trip', 'refused', { end: 4 + T });
  demo('no end', 'accepted', { end: undefined });
  demo('Try it reads no trip rule', 'accepted', { inputs: [{ at: 5, build: 'ARMMEX' }, { at: 6, move: [1, 1] }] }, 0);
  // the data rule of a resolved script: actions become buttons by the interface type, then no press of a held button
  // and no release of one up
  const noBar = (name, want, inputs, iface = 1, scripted = 1) => demo(name, want, { sidebar: false, inputs, end: 100 }, scripted, { iface });
  noBar('order, select and cancel clicks', 'accepted', [{ at: 1, down: 'order' }, { at: 2, up: 'order' }, { at: 3, down: 'select' },
    { at: 4, up: 'select' }, { at: 5, down: 'cancel' }, { at: 6, up: 'cancel' }]);
  noBar('the same clicks at interface type 0', 'accepted', [{ at: 1, down: 'order' }, { at: 2, up: 'order' }, { at: 3, down: 'select' },
    { at: 4, up: 'select' }, { at: 5, down: 'cancel' }, { at: 6, up: 'cancel' }], 0);
  noBar('a press of a held button', 'refused', [{ at: 1, down: 'lmb' }, { at: 2, down: 'lmb' }]);
  noBar('a release of a button up', 'refused', [{ at: 1, up: 'rmb' }]);
  noBar('an order during a box drag at type 1', 'accepted', [{ at: 1, down: 'select' }, { at: 2, down: 'order' }, { at: 3, up: 'order' }, { at: 4, up: 'select' }]);
  noBar('an order during a box drag at type 0', 'refused', [{ at: 1, down: 'select' }, { at: 2, down: 'order' }, { at: 3, up: 'order' }, { at: 4, up: 'select' }], 0);
  noBar('a cancel during an order at type 1', 'refused', [{ at: 1, down: 'order' }, { at: 2, down: 'cancel' }]);
  noBar('a cancel during an order at type 0', 'accepted', [{ at: 1, down: 'order' }, { at: 2, down: 'cancel' }, { at: 3, up: 'cancel' }, { at: 4, up: 'order' }], 0);
  noBar('a press and its release in one input', 'accepted', [{ at: 1, down: 'order', up: 'order' }]);
  noBar('a release first in the file, later in ticks', 'accepted', [{ at: 2, up: 'lmb' }, { at: 1, down: 'lmb' }]);
  noBar('a release first in its tick', 'refused', [{ at: 1, up: 'lmb' }, { at: 1, down: 'lmb' }]);
  noBar('a key pressed twice', 'accepted', [{ at: 1, down: 'shift' }, { at: 2, down: 'shift' }]);
  noBar('Try it reads no data rule', 'accepted', [{ at: 1, down: 'lmb' }, { at: 2, down: 'lmb' }], 1, 0);
  // text.js's tokens: title, subtitle, paragraphs, demo titles and captions
  const caption = c => e => { e.demos = { base: { inputs: [{ at: 0, caption: c }, ...base.inputs] } }; };
  text('every token kind', 'accepted', caption('{builder} {Order} {order} {Select} {select} {Cancel} {cancel} {mex} {geo}'));
  text('a "{" with no "}"', 'refused', caption('Pick the {mex'));
  text('a "}" alone', 'accepted', caption('a } b'));
  text('a name neither cast nor button', 'refused', caption('{dance}'));
  text('a capitalised cast', 'refused', caption('{Builder}'));
  text('an empty token', 'refused', caption('{}'));
  text('a cast no unit has', 'refused', caption('{wall}'));
  text('a "{" after text past U+FFFF', 'refused', caption('\u{1F600} {'));
  text('a token in the title', 'refused', e => { e.title = 'x {X}'; });
  text('a token in the subtitle', 'refused', e => { e.subtitle = '{order'; });
  text('a token in a paragraph', 'refused', e => { e.text = ['fine', 'not {fine}']; });
  text('a token in a demo title', 'refused', e => { e.demos = { base: { title: '{Mex}' } }; });
  text('a cast token in a paragraph', 'accepted', e => { e.text = ['The {advbuilder} builds the {moho}.']; });
  text('a cast token of a unit with no cast', 'refused', e => { delete e.units.ARMPW.cast; e.text = ['{walker}']; });
  // a unit's G cells, sim.js's checkG
  text('a G cell inside the footprint', 'accepted', e => { e.units.ARMGEO.g = [[0, 0], [3, 3]]; });
  text('a G cell off the footprint', 'refused', e => { e.units.ARMGEO.g = [[4, 0]]; });
  text('no G cell with vent', 'refused', e => { e.units.ARMGEO.g = []; });
  text('a G cell without vent', 'refused', e => { e.units.ARMMEX.g = [[0, 0]]; });
  text('no G cell without vent', 'accepted', e => { e.units.ARMMEX.g = []; });
  text('a G cell not two integers', 'refused', e => { e.units.ARMGEO.g = [[0.5, 0]]; });
  // the casting's reader, sim.js's checkCasting; each edit leaves the entry's own types bound to themselves
  const tower = f => C => f(C.casts.tower, C);
  cast('the identity casting', 'accepted', () => {});
  cast('the unbound casting', 'accepted', C => { Object.assign(C, clone(CASTINGS.unbound[0])); });
  cast('casts not an object', 'refused', C => { C.casts = []; });
  cast('a key not a cast', 'refused', C => { C.casts.dance = null; });
  cast('a cast absent', 'refused', C => { delete C.casts.wall; });
  cast('a cast not an object', 'refused', C => { C.casts.tower = 5; });
  cast('a type of 23 bytes', 'accepted', tower(k => { k.type = 'T'.repeat(23); }));
  cast('a type of 24 bytes', 'refused', tower(k => { k.type = 'T'.repeat(24); }));
  cast('an empty type', 'refused', tower(k => { k.type = ''; }));
  cast('a type of digits', 'refused', tower(k => { k.type = '123'; }));
  cast('a type with a digit', 'accepted', tower(k => { k.type = 'ARM1'; }));
  cast('a title of 93 bytes', 'accepted', tower(k => { k.title = 'T'.repeat(93); }));
  cast('a title of 94 bytes', 'refused', tower(k => { k.title = 'T'.repeat(94); }));
  cast('31 code-page bytes at 3 UTF-8 bytes each', 'accepted', tower(k => { k.title = '\u20AC'.repeat(31); }));
  cast('an empty title', 'accepted', tower(k => { k.title = ''; }));
  cast('a title not a string', 'refused', tower(k => { k.title = 7; }));
  cast('a footprint of 16', 'accepted', tower(k => { k.footprint = [16, 16]; }));
  cast('a footprint of 17', 'refused', tower(k => { k.footprint = [17, 2]; }));
  cast('a footprint of 0', 'refused', tower(k => { k.footprint = [2, 0]; }));
  cast('a footprint not integers', 'refused', tower(k => { k.footprint = [2.5, 2]; }));
  cast('a footprint of three', 'refused', tower(k => { k.footprint = [2, 2, 2]; }));
  cast('a figure below 0', 'refused', tower(k => { k.maxVelocity = -1; }));
  cast('a figure a string', 'refused', tower(k => { k.energyMake = '5'; }));
  cast('a figure null', 'refused', tower(k => { k.extracts = null; }));
  cast('a figure 0', 'accepted', tower(k => { k.workerTime = 0; }));
  cast('no buildTime', 'refused', tower(k => { delete k.buildTime; }));
  cast('a buildTime of 0', 'refused', tower(k => { k.buildTime = 0; }));
  cast('canstop absent', 'accepted', tower(k => { delete k.canstop; }));
  cast('canstop 3', 'accepted', tower(k => { k.canstop = 3; }));
  cast('canstop 1.5', 'refused', tower(k => { k.canstop = 1.5; }));
  cast('canstop true', 'refused', tower(k => { k.canstop = true; }));
  cast('vent 1', 'refused', tower(k => { k.vent = 1; }));
  cast('vent false and no g', 'accepted', tower(k => { k.vent = false; }));
  cast('G cells with vent', 'accepted', tower(k => { k.vent = true; k.g = [[0, 0], [1, 1]]; }));
  cast('G cells without vent', 'refused', tower(k => { k.g = [[0, 0]]; }));
  cast('no G cell with vent', 'refused', tower(k => { k.vent = true; k.g = []; }));
  cast('a G cell off the footprint', 'refused', tower(k => { k.vent = true; k.g = [[2, 0]]; }));
  cast('vent false with an empty g', 'accepted', tower(k => { k.vent = false; k.g = []; }));
  cast('builds of casts', 'accepted', tower(k => { k.builds = ['wall', 'wall', 'solar']; }));
  cast('builds naming a type', 'refused', tower(k => { k.builds = ['ARMDRAG']; }));
  cast('builds a string', 'refused', tower(k => { k.builds = 'wall'; }));
  cast('a full record from a pack', 'accepted', tower(k => { Object.assign(k, { workerTime: 0, buildDistance: 0, maxVelocity: 0,
    canstop: 0, extracts: 0, energyMake: 0, vent: false, g: [], builds: [] }); }));
  const screen = (h, gadgets = []) => ({ y: 128, w: 128, h, gadgets });
  const gad = i => ({ name: `G${i}`, role: 'order', rect: [0, 0, 1, 1] });
  const ck1 = L => L.screens.ARMCK1.gadgets;
  const types = L => L.sides[0].types;
  const core = L => { L.sides.push({ ...clone(L.sides[0]), name: 'CORE' }); return L.sides[1].types; };
  lay('the fixture', 'accepted', () => {});
  lay('format 2', 'refused', L => { L.format = 2; });
  lay('width 127', 'refused', L => { L.width = 127; });
  lay('height not the rows', 'refused', L => { L.height = 481; });
  for (const [n, want] of [[48, 'accepted'], [49, 'refused']])
    lay(`${n} screens`, want, L => { for (let i = Object.keys(L.screens).length; i < n; i++) L.screens[`S${i}`] = screen(352); });
  for (const [n, want] of [[64, 'accepted'], [65, 'refused']])
    lay(`${n} gadgets`, want, L => { L.screens.ARMMAIN2.gadgets = Array.from({ length: n }, (_, i) => gad(i)); });
  for (const [n, want] of [[7, 'accepted'], [8, 'refused']])
    lay(`${n} pages`, want, L => { types(L).ARMCK.pages = Array.from({ length: n }, (_, i) => `ARMCK${1 + i % 4}`); });
  for (const [n, want] of [[64, 'accepted'], [65, 'refused']])
    lay(`${n} types in one side`, want, L => { for (let i = Object.keys(types(L)).length; i < n; i++) types(L)[`T${i}`] = { pages: ['ARMCK1'], orders: null }; });
  for (const [n, want] of [[64, 'accepted'], [65, 'refused']])
    lay(`${n} types over two sides`, want, L => {
      const t = core(L);
      for (let i = Object.keys(types(L)).length + Object.keys(t).length; i < n; i++) t[`T${i}`] = { pages: ['ARMCK1'], orders: null };
    });
  lay('one type name in two sides', 'accepted', L => { core(L).ARMCK = { pages: ['ARMCK2'], orders: null }; });
  for (const [n, want] of [[8, 'accepted'], [9, 'refused']])
    lay(`${n} sides`, want, L => { L.sides = Array.from({ length: n }, (_, i) => ({ ...L.sides[0], name: `S${i}` })); });
  for (const [n, want] of [[1024, 'accepted'], [1025, 'refused']])
    lay(`a screen ending at row ${n}`, want, L => { L.screens.TALL = screen(n - 128); L.height = n; });
  lay('no sides', 'refused', L => { L.sides = []; });
  lay('a screen 129 wide', 'refused', L => { L.screens.ARMMAIN2.w = 129; });
  lay('a screen at row -1', 'refused', L => { L.screens.ARMMAIN2.y = -1; });
  lay('a screen not an integer high', 'refused', L => { L.screens.ARMMAIN2.h = 352.5; });
  lay('a rect past its screen\'s right', 'refused', L => { ck1(L)[0].rect = [100, 0, 29, 10]; });
  lay('a rect to its screen\'s right edge', 'accepted', L => { ck1(L)[0].rect = [100, 0, 28, 10]; });
  lay('a rect past its screen\'s bottom', 'refused', L => { ck1(L)[0].rect = [0, 340, 10, 13]; });
  lay('a rect at x -1', 'refused', L => { ck1(L)[0].rect = [-1, 0, 10, 10]; });
  lay('a rect 0 wide', 'refused', L => { ck1(L)[0].rect = [0, 0, 0, 10]; });
  lay('a rect of three numbers', 'refused', L => { ck1(L)[0].rect = [0, 0, 10]; });
  lay('an unknown role', 'refused', L => { ck1(L)[0].role = 'dance'; });
  lay('a gadget with no name', 'refused', L => { ck1(L)[0].name = ''; });
  lay('a build button with no unit key', 'refused', L => { delete ck1(L)[2].unit; });
  lay('a build button whose unit is empty', 'refused', L => { ck1(L)[2].unit = ''; });
  lay('grey not a boolean', 'refused', L => { ck1(L)[2].grey = 1; });
  lay('hidden not a boolean', 'refused', L => { ck1(L)[2].hidden = null; });
  lay('a player_side key is read past', 'accepted', L => { L.player_side = 5; });
  lay('a top-level types key is read past', 'accepted', L => { L.types = 3; });
  lay('a side with no types', 'refused', L => { delete L.sides[0].types; });
  lay('a side\'s types a list', 'refused', L => { L.sides[0].types = []; });
  lay('a side with no type', 'accepted', L => { L.sides[0].types = {}; });
  lay('a side\'s MAIN2 no screen', 'refused', L => { L.sides[0].main2 = 'NOPE'; });
  lay('a side\'s prefix not a string', 'refused', L => { L.sides[0].prefix = 3; });
  lay('a type with neither pages nor orders', 'refused', L => { types(L).ARMCK = { pages: [], orders: null }; });
  lay('a type with an orders page only', 'accepted', L => { types(L).ARMCK = { pages: [], orders: 'ARMGEN' }; });
  lay('a type with no orders key', 'refused', L => { delete types(L).ARMCK.orders; });
  lay('a page no screen', 'refused', L => { types(L).ARMCK.pages[1] = 'NOPE'; });
  lay('a page no screen in a second side', 'refused', L => { core(L).ARMCK.pages[1] = 'NOPE'; });
  lay('a name of 31 bytes', 'accepted', L => { ck1(L)[0].name = 'N'.repeat(31); });
  lay('a name of 32 bytes', 'refused', L => { ck1(L)[0].name = 'N'.repeat(32); });
  lay('a screen named in 32 bytes', 'refused', L => { L.screens['S'.repeat(32)] = screen(10); });
  lay('a type named in 32 bytes', 'refused', L => { types(L)['T'.repeat(32)] = { pages: ['ARMCK1'], orders: null }; });
  lay('a prefix of 32 bytes', 'refused', L => { L.sides[0].prefix = 'P'.repeat(32); });
  lay('a note and keys the schema does not name', 'accepted', L => { L.note = 'x'; L.extra = [1]; ck1(L)[0].colour = 'red'; });
  return cases;
}

/* ---- the data read back: what sim.js accepts of a casting or a layout, in wikitest's `data` shapes ---- */
function castingOut(C) {
  try { WikiSim.checkCasting(C); } catch (e) { return { ok: false, why: e.message }; }
  const casts = WikiSim.CASTS.map(c => {
    const k = C.casts[c];
    if (!k) return null;
    const [fx, fz] = k.footprint, g = new Array(16).fill(0);
    // sim.js's gCell: the `g` it lists, or every cell of a `vent` type with none
    if (k.g) for (const [dx, dz] of k.g) g[dz] |= 1 << dx;
    else if (k.vent) for (let dz = 0; dz < fz; dz++) g[dz] = (1 << fx) - 1;
    const o = { type: k.type, title: k.title, fx, fz };
    for (const n of ['buildTime', 'workerTime', 'buildDistance', 'maxVelocity', 'extracts', 'energyMake']) o[n] = k[n] ?? 0;
    return { ...o, canstop: (k.canstop & 1) === 1 ? 1 : 0, vent: k.vent ? 1 : 0, g,
      builds: (k.builds || []).reduce((m, b) => m | (1 << WikiSim.CASTS.indexOf(b)), 0) };
  });
  return { ok: true, casts };
}
function layoutOut(L) {
  try { WikiSim.checkLayout(L); } catch (e) { return { ok: false, why: e.message }; }
  return { ok: true, height: L.height, screens: Object.keys(L.screens),
    sides: L.sides.map(s => ({ name: s.name, prefix: s.prefix, main2: s.main2, gen: s.gen,
      types: Object.fromEntries(Object.entries(s.types).map(([t, T]) => [t, { pages: T.pages, orders: T.orders }])) })) };
}
function dataItems() {
  const items = [];
  for (const [name, list] of Object.entries(CASTINGS))
    list.forEach((C, i) => items.push({ kind: 'casting', name: `casting-fixture ${name}[${i}]`, v: C }));
  for (const f of fs.readdirSync(path.join(ROOT, 'art/wiki/side')).filter(f => f.endsWith('.json')).sort())
    items.push({ kind: 'layout', name: `art/wiki/side/${f}`, v: SIDE(f.slice(0, -5)) });
  const packs = path.join(ROOT, 'tools/wiki/mockup/gfx/side');
  for (const mod of fs.existsSync(packs) ? fs.readdirSync(packs).sort() : []) {
    const file = path.join(packs, mod, 'side.json');
    if (!fs.existsSync(file)) continue;
    const P = JSON.parse(fs.readFileSync(file, 'utf8'));
    if (P.layout) items.push({ kind: 'layout', name: `pack ${mod} layout`, v: P.layout });
    (Array.isArray(P.casting) ? P.casting : []).forEach((C, i) => items.push({ kind: 'casting', name: `pack ${mod} casting[${i}] ${C && C.side}`, v: C }));
  }
  return items;
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'simparity-'));
let failed = 0, demos = 0, cases = 0, differ = 0, malformed = 0, bad = 0, refusals = 0, refuseBad = 0, datas = 0, dataBad = 0;
try {
  const wikitest = build(tmp);
  run('python3', [path.join(ROOT, 'tools/hudpak.py'), '--set', 'wiki', path.join(ROOT, 'art/wiki'), path.join(tmp, 'wiki.bin')]);

  /* ---- every demo, tick by tick, with each layout ---- */
  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  const ids = [...new Set(tree.tree.flatMap(leaves))];
  for (const side of ['fixture', 'fixture-tall', null]) {
    const layout = side && SIDE(side), named = side || 'no sidebar';
    const cout = lines(wikitest(['demos', 'wiki.bin', side || '-']));
    let at = 0;
    for (const id of ids) {
      const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
      for (const demo of entry.demos) {
        const sim = WikiSim.play(entry, demo, layout);
        let first = null, ticks = 0;
        for (;;) {
          const v = sim.view(), h = stateHash(sim, v);
          if (!(sim.tick === 0 || sim.done)) delete v.map;
          const j = { entry: id, demo: demo.id, t: sim.tick, h, v, o: Object.fromEntries(v.units.map(u => [u.id, sim.orders(u.id)])) };
          if (at >= cout.length) { first = { t: sim.tick, at: '(the C sim ended early)', c: null, j: null }; break; }
          const d = diff(JSON.parse(cout[at++]), j, '');
          if (d && !first) first = { t: sim.tick, ...d };
          ticks++;
          if (sim.done) break;
          sim.step();
        }
        demos++;
        if (first) {
          failed++;
          differ++;
          console.log(`DIFFER ${id}/${demo.id} (${named}) at tick ${first.t}: ${first.at}: C ${show(first.c)}, sim.js ${show(first.j)}`);
          while (at < cout.length && JSON.parse(cout[at]).demo === demo.id && JSON.parse(cout[at]).entry === id) at++;
        } else console.log(`equal  ${id}/${demo.id} (${named}): ${ticks} ticks`);
      }
    }
    if (at !== cout.length) { failed++; differ++; console.log(`DIFFER (${named}) the C sim played ${cout.length - at} frames more`); }
  }

  /* ---- simcheck.mjs's rule cases, recorded on sim.js and replayed through the C sim ---- */
  const { ENTRY_RULES } = await import('./simcheck.mjs');
  const { create, play } = WikiSim;
  let ops, expected;
  const record = (make, scripted) => (entry, demo, layout) => {
    const sim = make(entry, demo, layout);
    ops.push(JSON.stringify({ op: 'create', scripted, layout: layout ?? null }), JSON.stringify({ ...entry, demos: [demo || {}] }));
    const ask = (op, v) => { ops.push(JSON.stringify(op)); expected.push(v); return v; };
    return {
      input(ev) { ops.push(JSON.stringify({ op: 'input', ev })); sim.input(ev); },
      step() { ops.push(JSON.stringify({ op: 'step' })); sim.step(); },
      view: () => {
        const v = ask({ op: 'view' }, sim.view());
        ask({ op: 'hash' }, stateHash(sim, sim.view()));
        return v;
      },
      orders: id => ask({ op: 'orders', id }, sim.orders(id)),
      get done() { return ask({ op: 'done' }, sim.done); },
      get tick() { return ask({ op: 'tick' }, sim.tick); },
      get log() { return sim.log; },
    };
  };
  WikiSim.create = record(create, 0);
  WikiSim.play = record(play, 1);
  for (const id of ids) {
    const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
    for (const [what, check] of Object.entries(ENTRY_RULES[id] || {})) {
      ops = []; expected = [];
      let verdict;
      try { verdict = check(entry); } catch (e) { verdict = `threw ${e.message}`; }
      fs.writeFileSync(path.join(tmp, 'trace.jsonl'), ops.join('\n') + '\n');
      const got = lines(wikitest(['trace', 'trace.jsonl'])).map(l => JSON.parse(l));
      let first = got.length !== expected.length ? { at: 'answers.length', c: got.length, j: expected.length } : null;
      for (let i = 0; !first && i < got.length; i++) { const d = diff(got[i], expected[i], `answer ${i}`); if (d) first = d; }
      cases++;
      if (first) {
        failed++;
        console.log(`DIFFER rule -- ${what}: ${first.at}: C ${show(first.c)}, sim.js ${show(first.j)}`);
      } else console.log(`equal  rule -- ${what}: ${expected.length} answers (sim.js's verdict: ${verdict})`);
    }
  }
  Object.assign(WikiSim, { create, play });

  /* ---- the build trip's constants ---- */
  {
    const c = JSON.parse(wikitest(['constants']));
    const j = { TRIP: WikiSim.TRIP, TRIP_DOWN: WikiSim.TRIP_DOWN, TRIP_UP: WikiSim.TRIP_UP, TRIP_ROWS: WikiSim.TRIP_ROWS };
    const d = diff(c, j, '');
    cases++;
    if (d) { failed++; console.log(`DIFFER the build trip's constants: ${d.at}: C ${show(d.c)}, sim.js ${show(d.j)}`); }
    else console.log(`equal  the build trip's constants: TRIP ${c.TRIP}, down ${c.TRIP_DOWN}, up ${c.TRIP_UP}, ${c.TRIP_ROWS.length} rows`);
  }

  /* ---- refusals: both sides accept or refuse alike, as each case wants ---- */
  {
    const entry = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries/quickbuild.json'), 'utf8'));
    const all = refusalCases(entry);
    fs.writeFileSync(path.join(tmp, 'refuse.jsonl'), all.map(c =>
      JSON.stringify({ name: c.name, scripted: c.scripted, layout: c.layout, iface: c.iface, casting: c.casting }) + '\n' +
      JSON.stringify(c.entry) + '\n').join(''));
    const got = lines(wikitest(['refuse', 'refuse.jsonl']));
    all.forEach((c, i) => {
      let js = 'accepted', why = '';
      try {
        // text.js's checkEntry is the loader's refusal of the text, which wiki.js asks before any sim is made
        const bad = WikiText.checkEntry(c.entry);
        if (bad) throw new Error(bad);
        (c.scripted ? WikiSim.play : WikiSim.create)(c.entry, c.entry.demos[0], c.layout, { side: 0, casting: c.casting, iface: c.iface });
      } catch (e) { js = 'refused'; why = e.message; }
      const cv = (got[i] || '').startsWith('accepted ') ? 'accepted' : (got[i] || '').startsWith('refused ') ? 'refused' : '(no answer)';
      refusals++;
      if (cv === js && js === c.want) console.log(`equal  refusal -- ${c.name}: both ${js}`);
      else { failed++; refuseBad++; console.log(`DIFFER refusal -- ${c.name}: C ${got[i] || '(none)'}; sim.js ${js}${why ? ` (${why})` : ''}; wanted ${c.want}`); }
    });
  }

  /* ---- castings and layouts read back, field by field; a refused casting by its message ---- */
  {
    const items = dataItems();
    fs.writeFileSync(path.join(tmp, 'data.jsonl'), items.map(it =>
      JSON.stringify({ kind: it.kind, name: it.name }) + '\n' + JSON.stringify(it.v) + '\n').join(''));
    const got = lines(wikitest(['data', 'data.jsonl']));
    items.forEach((it, i) => {
      const j = it.kind === 'casting' ? castingOut(it.v) : layoutOut(it.v), c = got[i] ? JSON.parse(got[i]) : null;
      // a layout's refusals are worded by each side on its own; a casting's are sim.js's words
      if (c && !c.ok && !j.ok && it.kind === 'layout') { c.why = j.why; }
      const d = c ? diff(c, j, '') : { at: '(no answer)', c: null, j };
      datas++;
      if (d) { failed++; dataBad++; console.log(`DIFFER data -- ${it.name}: ${d.at}: C ${show(d.c)}, sim.js ${show(d.j)}`); }
      else console.log(`equal  data -- ${it.name}: ${j.ok ? 'read alike' : `both refused (${j.why})`}`);
    });
  }

  /* ---- data the loader must survive, built by tests/wikitest.c's `malformed` ---- */
  for (const l of lines(wikitest(['malformed']))) {
    malformed++;
    if (l.startsWith('ok ')) console.log(`equal  malformed -- ${l.slice(3)}`);
    else { failed++; bad++; console.log(`DIFFER malformed -- ${l}`); }
  }
} catch (e) {
  if (!(e instanceof CannotRun)) throw e;
  console.error(`simparity: ${e.message}`);
  process.exitCode = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
if (process.exitCode !== 2) {
  const variant = WINE ? 'i686 under Wine' : 'native';
  console.log(failed ? `${differ} of ${demos} demo plays, ${failed - differ - bad - refuseBad - dataBad} of ${cases} rule cases, ${refuseBad} of ${refusals} refusal cases, ${dataBad} of ${datas} data read back and ${bad} of ${malformed} malformed-data cases differ (C sim: ${variant})`
    : `all ${demos} demo plays and ${cases} rule cases equal, tick for tick, all ${refusals} refusal cases alike, all ${datas} castings and layouts read alike and all ${malformed} malformed-data cases loaded (C sim: ${variant})`);
  process.exitCode = failed ? 1 : 0;
}
