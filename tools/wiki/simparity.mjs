/* simparity.mjs -- the in-game wiki's parity check: the DLL's simulation, tagpu/ddraw/src/tagpu_wikisim.c over
   tagpu_wikidata.c's reading of the archive the DLL embeds, against the mockup's tools/wiki/mockup/sim.js, tick for
   tick and field by field.

   usage: node tools/wiki/simparity.mjs [--wine]
     exit 0 when every demo and every rule case agrees, 1 when one differs, 2 when the C side cannot be built or run

   It packs art/wiki/ as the Makefile does (tools/hudpak.py --set wiki), builds tagpu/ddraw/tests/wikitest.c with the
   real sources, and plays every demo of every entry the tree names through both, comparing every field of the view
   and every unit's orders on every tick; the map, which nothing writes once the scene is laid out, on the first and
   the last tick. It then plays simcheck.mjs's rule cases -- the rules no demo shows -- through both: each case is run
   against sim.js with every call recorded, the recording is replayed through the C sim, and every answer the case
   read (each view, orders, done) is compared. For each demo and case it names the first differing tick or call and
   field.

   By default the C side is built natively with gcc, AddressSanitizer and UndefinedBehaviorSanitizer. --wine builds it
   with the DLL's own compiler and flags (i686-w64-mingw32-gcc, the simulation on SSE2 as the Makefile compiles it)
   and runs it under Wine (WINEPREFIX: the main checkout's wineprefix/), which checks the arithmetic the game runs. */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSim = require('./mockup/sim.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const WINE = process.argv.includes('--wine');
const SRCS = ['src/tagpu_wikidata.c', 'src/tagpu_json.c', 'src/tagpu_pak.c', 'src/tagpu_placegeo.c'];

class CannotRun extends Error {}

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { maxBuffer: 1 << 30, encoding: 'utf8', ...opts });
  if (r.error || r.status !== 0)
    throw new CannotRun(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ` (${r.status})`}\n${r.stderr || ''}`);
  return r.stdout;
}

function build(tmp) {
  if (WINE) {
    const exe = path.join(DDRAW, 'tests/wikitest.exe'), sim = path.join(tmp, 'tagpu_wikisim.o');
    const cc = 'i686-w64-mingw32-gcc', flags = ['-Iinc', '-Isrc', '-O2', '-Wall', '-std=c99'];
    run(cc, [...flags, '-msse2', '-mfpmath=sse', '-mincoming-stack-boundary=2', '-c', '-o', sim, 'src/tagpu_wikisim.c'], { cwd: DDRAW });
    run(cc, [...flags, '-static', '-o', exe, 'tests/wikitest.c', sim, ...SRCS], { cwd: DDRAW });
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

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'simparity-'));
let failed = 0, demos = 0, cases = 0, differ = 0;
try {
  const wikitest = build(tmp);
  run('python3', [path.join(ROOT, 'tools/hudpak.py'), '--set', 'wiki', path.join(ROOT, 'art/wiki'), path.join(tmp, 'wiki.bin')]);

  /* ---- every demo, tick by tick ---- */
  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  const ids = [...new Set(tree.tree.flatMap(leaves))];
  const cout = lines(wikitest(['demos', 'wiki.bin']));
  let at = 0;
  for (const id of ids) {
    const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
    for (const demo of entry.demos) {
      const sim = WikiSim.play(entry, demo);
      let first = null, ticks = 0;
      for (;;) {
        const v = sim.view();
        if (!(sim.tick === 0 || sim.done)) delete v.map;
        const j = { entry: id, demo: demo.id, t: sim.tick, v, o: Object.fromEntries(v.units.map(u => [u.id, sim.orders(u.id)])) };
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
        console.log(`DIFFER ${id}/${demo.id} at tick ${first.t}: ${first.at}: C ${show(first.c)}, sim.js ${show(first.j)}`);
        while (at < cout.length && JSON.parse(cout[at]).demo === demo.id && JSON.parse(cout[at]).entry === id) at++;
      } else console.log(`equal  ${id}/${demo.id}: ${ticks} ticks`);
    }
  }
  if (at !== cout.length) { failed++; differ++; console.log(`DIFFER the C sim played ${cout.length - at} frames more`); }

  /* ---- simcheck.mjs's rule cases, recorded on sim.js and replayed through the C sim ---- */
  const { ENTRY_RULES } = await import('./simcheck.mjs');
  const create = WikiSim.create;
  let ops, expected;
  WikiSim.create = (entry, demo) => {
    const sim = create(entry, demo);
    ops.push(JSON.stringify({ op: 'create', scripted: 0 }), JSON.stringify({ ...entry, demos: [demo || {}] }));
    const ask = (op, v) => { ops.push(JSON.stringify(op)); expected.push(v); return v; };
    return {
      input(ev) { ops.push(JSON.stringify({ op: 'input', ev })); sim.input(ev); },
      step() { ops.push(JSON.stringify({ op: 'step' })); sim.step(); },
      view: () => ask({ op: 'view' }, sim.view()),
      orders: id => ask({ op: 'orders', id }, sim.orders(id)),
      get done() { return ask({ op: 'done' }, sim.done); },
      get tick() { return ask({ op: 'tick' }, sim.tick); },
    };
  };
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
  WikiSim.create = create;
} catch (e) {
  if (!(e instanceof CannotRun)) throw e;
  console.error(`simparity: ${e.message}`);
  process.exitCode = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
if (process.exitCode !== 2) {
  const variant = WINE ? 'i686 under Wine' : 'native';
  console.log(failed ? `${differ} of ${demos} demos and ${failed - differ} of ${cases} rule cases differ (C sim: ${variant})`
    : `all ${demos} demos and ${cases} rule cases equal, tick for tick (C sim: ${variant})`);
  process.exitCode = failed ? 1 : 0;
}
