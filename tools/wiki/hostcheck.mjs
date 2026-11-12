/* hostcheck.mjs -- the wiki's simulation host (tagpu/ddraw/tests/wikihost.c, `wikitest serve`) and its channels
   (hostchan.cjs, serve.py's SimRelay, the drop-in mockup/simhost.js) held to the protocol's promises under requests
   no page makes: offline, no game, a few seconds.

   usage: node tools/wiki/hostcheck.mjs [--target host|asan]      (asan by default: every finding fatal)
     exit 0 when every case answers as named, 1 when one does not, 2 when a build cannot be made

   THE LINES: every op with bad handles and bad arguments, bad JSON, a 16 MB line, an empty line, a line holding a NUL
   (inside the first 64 KB and past it), a call stamped for another host, a free of no sim, a sight view past what a
   sim's view holds -- through `wikitest trace`, which must answer each line with exactly one line of JSON.
   THE LIVE SIMS: every entry's sims in each play asked every op with garbage arguments, freed twice, and the tables
   pushed past their bounds (an entry evicted while its sim plays on, 70 sims).
   THE DROP-IN: a packs set and a sim evicted from the host's tables and then used or freed (by hand and by the
   garbage collector), and a host replaced under the drop-in -- serve.py starts a new one after one ends -- whose
   handles must never be read for the old one's.
   THE CHANNELS: a host killed while it writes an answer larger than its pipe, through hostchan.cjs and through
   serve.py's SimRelay (tools/wiki/hostcheck_relay.py, which checks the relay's own refusals too).
   Each such kill waits for a CONDITION, never a time: the host blocked in its pipe write with the answer unread, so
   that it lands mid-answer whatever the load (a host that never blocks in 30 s fails the case rather than passing it).
   The garbage collector's case waits for the finalizers the same way, by running them, not by waiting. */
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import Module from 'node:module';
import { build } from './wikibuild.mjs';

if (typeof globalThis.gc !== 'function') {
  const r = spawnSync(process.execPath, ['--expose-gc', ...process.argv.slice(1)], { stdio: 'inherit' });
  process.exit(r.status ?? 1);
}
const require = createRequire(import.meta.url);
const ROOT = path.resolve(new URL('../..', import.meta.url).pathname);
const argOf = k => { const i = process.argv.indexOf(k); return i >= 0 ? process.argv[i + 1] : undefined; };
const TARGET = argOf('--target') || 'asan';
let EXE;
try { EXE = build(TARGET); } catch (e) { console.error(`hostcheck: ${e.message}`); process.exit(2); }

let failed = 0;
const check = (ok, what, why = '') => {
  if (!ok) failed++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}${ok || !why ? '' : `: ${why}`}`);
};
const entry = f => JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries', f), 'utf8'));
const ENTRIES = fs.readdirSync(path.join(ROOT, 'art/wiki/entries')).filter(f => f.endsWith('.json')).sort();
const LAYOUT = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/side/fixture.json'), 'utf8'));
const sleep = ms => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
const thrown = f => { try { f(); return null; } catch (e) { return e; } };

/* ---- THE LINES ---- */
function lines() {
  const OPS = ['hello', 'entry', 'layout', 'casting', 'packs', 'table', 'text', 'scene', 'seatcam', 'sight', 'sighttable', 'fits', 'new',
    'input', 'step', 'view', 'orders', 'hash', 'now', 'poses', 'notes', 'fork', 'release', 'free', 'batch', 'nope'];
  const reqs = [], want = new Map();
  for (const op of OPS) {
    reqs.push(JSON.stringify({ op }));
    reqs.push(JSON.stringify({ op, sim: 99, entry: 99, layout: 99, table: 99, json: 7, dirs: ['../x'], ctx: { casting: 99, packs: 99 },
      view: { w: 5, h: 5, units: [[1, 2, 0, 9, 1]] }, n: 5000, calls: 3, inputs: 4, ev: 1 }));
    reqs.push(JSON.stringify({ op, sim: -1, entry: 'x', json: { scenes: 1 }, dirs: 'x', n: 0, calls: [{ op }, { op: 'batch', calls: [] }] }));
  }
  const BIG = { seat: 1e300, tick: -1e300, distance: 1e300, w: 1e300, h: 3, R: 1e300, n: 1e300 };
  reqs.push(JSON.stringify({ op: 'sight', ...BIG, contacts: true, view: { w: 1e300, h: -5, units: [[1e300, -1e300, 1e300, 1e300, 1e300]],
    deaths: [[1e300, 1e300, 1e300, 1e300, 1e300]], types: [[1e300, 1e300, 1e300, 1e300, 1e300]] } }));
  reqs.push(JSON.stringify({ op: 'sight', ...BIG, contacts: true, view: { w: 64, h: 64, units: [[1e300, -1e300, 1e300, 0, 1e300], [5, 5, 0, 0, 1]],
    deaths: [[1e300, 1e300, 1e300, 1e300, 0]], types: [[1e300, 1e300, 1e300, 1e300, 1e300]] } }));
  reqs.push(JSON.stringify({ op: 'sighttable', ...BIG }), JSON.stringify({ op: 'sighttable', distance: -1e300 }),
    JSON.stringify({ op: 'fits', ...BIG }), JSON.stringify({ op: 'table', json: { numtables: 1e300, tables: 5 } }));
  reqs.push('{"op":', '[1,2', '"str"', '42', 'null', '{"op":"hello"} trailing', '\t{"op":"fits","w":1e300,"h":-1e300}');
  reqs.push(JSON.stringify({ op: 'batch', calls: OPS.map(op => ({ op, sim: 99 })) }));
  reqs.push('{"op":"hello"}\r');
  const add = (line, ok, what) => { want.set(reqs.length, { ok, what }); reqs.push(line); };
  const FITS = '{"op":"fits","w":40,"h":30}', isFits = a => typeof a.fits === 'boolean';
  add(JSON.stringify({ op: 'hello', pad: 'x'.repeat(1 << 24) }), a => /longer than 16 MB/.test(a.error), 'a line past 16 MB is refused');
  add(JSON.stringify({ op: 'hello', pad: 'x'.repeat((1 << 24) - 40) }), a => a.version === 1, 'a line just under 16 MB is answered');
  add('', a => a.error === 'an empty request', 'an empty line is answered');
  add(FITS, isFits, 'the line after an empty one is answered by itself');
  add('   ', a => typeof a.error === 'string', 'a blank line is answered');
  add('{"op":"hel\0lo"}', a => a.error === 'the request line holds a NUL byte', 'a NUL in a line is refused');
  add(FITS, isFits, 'the line after it is answered by itself');
  add('\0', a => a.error === 'the request line holds a NUL byte', 'a lone NUL is refused');
  add(FITS, isFits, 'the line after it is answered by itself');
  add(`{"op":"hello","pad":"${'y'.repeat(70000)}\0${'z'.repeat(70000)}"}`, a => a.error === 'the request line holds a NUL byte',
    'a NUL 70 000 bytes into a line is refused');
  add(FITS, isFits, 'the line after it is answered by itself');
  add('{"op":"free","sim":424242}', a => JSON.stringify(a) === '{}', 'a free of no sim answers {}');
  add('{"op":"free"}', a => JSON.stringify(a) === '{}', 'a free naming nothing answers {}');
  add('{"op":"fits","w":40,"h":30,"in":"another"}', a => typeof a.error === 'string' && a.host === '', 'a call stamped for another host is refused');
  add('{"op":"fits","w":40,"h":30,"in":7}', a => typeof a.error === 'string' && a.host === '', 'a stamp that is no string is refused');
  add('{"op":"fits","w":40,"h":30,"in":""}', isFits, 'a call stamped for the process is answered');
  add('{"op":"batch","calls":[{"op":"free","sim":7,"in":"x"},{"op":"fits","w":40,"h":30}]}',
    a => a.answers[0].host === '' && typeof a.answers[1].fits === 'boolean', 'a batch\'s calls are checked one by one');
  const view = n => ({ w: 64, h: 64, units: Array.from({ length: n }, () => [5, 5, 0, 0, 1]), deaths: [], types: [[100, 0, 0, 0, 10]] });
  add(JSON.stringify({ op: 'sight', view: view(4 * 1500 + 1), contacts: true }), a => /more units/.test(a.error), 'a sight view past a sim\'s units is refused');
  add(JSON.stringify({ op: 'sight', view: view(4 * 1500), contacts: false }), a => typeof a.lit === 'string', 'a sight view at the bound is answered');
  add(JSON.stringify({ op: 'sight', view: { ...view(1), deaths: Array.from({ length: 257 }, () => [0, 0, 5, 5, 0]) } }),
    a => /more units/.test(a.error), 'a sight view past WIKI_FEATS_MAX deaths is refused');
  add(JSON.stringify({ op: 'sight', view: { ...view(1), types: Array.from({ length: 65 }, () => [100, 0, 0, 0, 10]) } }),
    a => /more units/.test(a.error), 'a sight view past WIKI_MAX_DEFS types is refused');
  add('{"op":"hello"}', a => a.version === 1 && a.instance === '', 'hello names the instance, none in a trace');

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hostcheck-')), file = path.join(dir, 'reqs');
  fs.writeFileSync(file, reqs.join('\n') + '\n');
  const r = spawnSync(EXE, ['trace', file], { cwd: ROOT, maxBuffer: 1 << 28, encoding: 'utf8' });
  fs.rmSync(dir, { recursive: true, force: true });
  const out = r.stdout.split('\n');
  if (out[out.length - 1] === '') out.pop();
  const parsed = out.map(l => { try { return JSON.parse(l); } catch (e) { return undefined; } });
  check(out.length === reqs.length && r.status === 0, `${reqs.length} request lines answered by as many lines`,
    `${out.length} answers, exit ${r.status} ${r.signal || ''}\n${(r.stderr || '').slice(0, 2000)}`);
  check(parsed.every(a => a !== undefined), 'every answer is JSON', `answer ${parsed.indexOf(undefined)}`);
  for (const [i, { ok, what }] of want) {
    let pass = false;
    try { pass = out.length === reqs.length && ok(parsed[i]); } catch (e) { /* a missing member */ }
    check(pass, what, JSON.stringify(parsed[i])?.slice(0, 200));
  }
}

/* ---- THE LIVE SIMS: one host on a pipe, every request answered in order ---- */
function liveSims() {
  const hc = require('./hostchan.cjs');
  const c = hc.open(TARGET);
  const ask = r => JSON.parse(c.request(JSON.stringify(r)));
  const GARBAGE = [{}, { seat: 1e300 }, { seat: -3 }, { seat: 0.5 }, { n: 0 }, { n: 4097 }, { n: 1.5 }, { n: 3, each: ['hash', 'now', 'x', 7], seat: 9 },
    { id: 7 }, { id: 'nope' }, { all: true }, { ev: 'x' }, { ev: { key: 'nope' } }, { ev: { kill: 'u1' } }, { ev: { seat: 1e300, click: [1, 2] } },
    { R: -5, inputs: [] }, { R: 1e300, inputs: [{}], mode: 'stall' }, { R: 0, inputs: 'x' }, { R: 10, inputs: [{ at: 3, kill: 'x' }, 5] },
    { R: 1, inputs: [{ at: 0, seat: 1e300, kill: 'zz' }], seat: 1e300 },
    { want: { view: [0, 1e300, -1, 'x'], orders: true, hash: true, now: [0, 5], poses: true } }, { map: true, seat: 2 }];
  const OPS = ['input', 'step', 'view', 'orders', 'hash', 'now', 'poses', 'notes', 'fork', 'release'];
  let sims = 0, n = 0, freedTwice = true, foreign = true;
  try {
    for (const f of ENTRIES) {
      const e = entry(f);
      const eh = ask({ op: 'entry', json: e }).entry, lh = ask({ op: 'layout', json: LAYOUT }).layout;
      ask({ op: 'scene', entry: eh }); ask({ op: 'seatcam', entry: eh, seat: 1e300, tick: -1e300 }); ask({ op: 'text', json: e });
      for (const play of ['try', 'demo', 'game', 'stall']) {
        ask({ op: 'new', entry: eh, play, layout: lh, ctx: { side: 1e300, iface: -1e300 } });
        const m = ask({ op: 'new', entry: eh, play, layout: lh, ctx: { side: 0, iface: 1 } });
        if (m.error) continue;
        sims++;
        for (const op of OPS) for (const g of GARBAGE) { ask({ op, sim: m.sim, ...g }); n++; }
        ask({ op: 'step', sim: m.sim, n: 4096, each: ['hash'] });
        ask({ op: 'batch', calls: OPS.map(op => ({ op, sim: m.sim, n: 2 })) });
        foreign &&= ask({ op: 'hash', sim: m.sim, in: 'not-the-host' }).host === c.instance && typeof ask({ op: 'hash', sim: m.sim, in: c.instance }).hash === 'string';
        freedTwice &&= JSON.stringify(ask({ op: 'free', sim: m.sim })) === '{}' && JSON.stringify(ask({ op: 'free', sim: m.sim })) === '{}'
          && /^no sim/.test(ask({ op: 'view', sim: m.sim }).error);
      }
    }
    check(sims > 0, `${sims} live sims asked ${n} garbage calls, each answered`);
    check(freedTwice, 'a sim freed twice answers {} both times and is gone');
    check(foreign, 'a live sim\'s call stamped for another host is refused, naming the host');
    // past the tables' bounds: an entry evicted while its sim plays on, 70 sims of which the oldest go
    const e0 = entry('reclaim.json');
    const keep = ask({ op: 'entry', json: e0 }).entry, old = ask({ op: 'new', entry: keep, play: 'demo' }).sim;
    let last;
    for (let i = 0; i < 40; i++) last = ask({ op: 'entry', json: e0 }).entry;
    check(/^no entry/.test(ask({ op: 'scene', entry: keep }).error), 'an entry past its table\'s 32 is evicted');
    check(typeof ask({ op: 'step', sim: old, n: 5 }).tick === 'number', 'the sim made from it still steps');
    const made = [];
    for (let i = 0; i < 70; i++) { made.push(ask({ op: 'new', entry: last, play: 'try' }).sim); ask({ op: 'step', sim: made[i], n: 3 }); }
    check(/^no sim/.test(ask({ op: 'hash', sim: made[0] }).error) && typeof ask({ op: 'hash', sim: made[69] }).hash === 'string',
      'past 64 sims the least recently used goes');
    check(JSON.stringify(ask({ op: 'free', sim: made[0] })) === '{}', 'a free of an evicted sim answers {}');
  } catch (e) {
    check(false, 'the live sims', e.message);
  }
  c.close();
}

/* ---- THE DROP-IN, over a channel whose host can be replaced as serve.py replaces one ---- */
async function dropIn() {
  const hc = require('./hostchan.cjs');
  let cur = null, opened = 0;
  const swap = {
    request(line) { cur ??= (opened++, hc.open(TARGET)); return cur.request(line); },
    replace() { if (cur) cur.close(); cur = null; },
  };
  const real = path.join(ROOT, 'tools/wiki/hostchan.cjs'), kept = require.cache[real], m = new Module(real);
  m.filename = real;
  m.loaded = true;
  m.exports = { ...hc, open: () => swap, tee: () => swap };
  require.cache[real] = m;
  const prevTee = process.env.WIKI_HOST_TEE;
  delete process.env.WIKI_HOST_TEE;
  const S = require('./mockup/simhost.js');
  const e = entry('reclaim.json'), d = e.demos[0], ctx = packs => ({ side: 0, iface: 1, packs });
  try {
    // a packs set evicted from the host's table of 32, then named again
    let s = S.create(e, d, null, ctx(['nopack/0']));
    s.free();
    for (let i = 1; i <= 33; i++) S.create(e, d, null, ctx([`nopack/${i}`])).free();
    let x = thrown(() => S.create(e, d, null, ctx(['nopack/0'])).free());
    check(!x, 'a packs set evicted from the host is sent again', x && x.message);

    // a sim evicted from the host's 64, then freed by hand: the free's answer reaches no other call
    const first = S.create(e, d, null, null), live = [];
    for (let i = 0; i < 70; i++) live.push(S.create(e, d, null, null));
    first.free();
    x = thrown(() => live[69].step());
    check(!x, 'freeing an evicted sim throws into no other call', x && x.message);

    // the same through the finalizer: a dropped sim evicted, then collected; a registry of ours on the same sim says
    // when the collector has seen it go
    let seen = false;
    const ours = new FinalizationRegistry(() => { seen = true; });
    (() => { const s = S.create(e, d, null, null); s.step(); ours.register(s, 0); })();
    for (let i = 0; i < 70; i++) live[i] = S.create(e, d, null, null);
    await collect(() => seen);
    check(seen, 'the dropped sim is collected');
    x = thrown(() => live[69].step());
    check(!x, 'a collected sim\'s free throws into no other call', x && x.message);

    // the host replaced: the old host's sims must throw, never read the new host's objects of their numbers
    for (const s of live) s.free();
    swap.replace();
    const a = S.create(e, d, null, null);
    a.steps(5);
    const before = a.hash();
    swap.replace();
    x = thrown(() => S.checkText(e));
    check(!x, 'a call naming no sim, the first after the host is replaced, is made again on the new one', x && x.message);
    const b = S.create(e, d, null, null);       // made as `a` was, so it may carry `a`'s number in the new host
    b.steps(9);
    x = thrown(() => a.hash());
    check(x && /another host/.test(x.message), 'a sim of a replaced host throws', x ? x.message : `it answered ${a.hash()} (was ${before}, the new sim ${b.hash()})`);
    x = thrown(() => { a.step(); a.view(); });
    check(!!x, 'and throws at every call after');
    check(b.tick === 9 && typeof b.hash() === 'string', 'a sim of the new host plays');
    a.free();
    x = thrown(() => b.step());
    check(!x, 'a replaced host\'s sim freed throws into no other call', x && x.message);
    check(opened === 3, `each replaced host opened once (${opened} opened)`);
  } finally {
    require.cache[real] = kept;
    if (prevTee !== undefined) process.env.WIKI_HOST_TEE = prevTee;
  }
}
// the garbage collector run until `done` holds and then a few rounds more, each between two macrotasks, where the
// finalizers it queued run
async function collect(done) {
  for (let i = 0, after = 0; i < 200 && after < 4; i++) {
    globalThis.gc();
    await new Promise(r => setImmediate(r));
    if (done()) after++;
  }
}

/* ---- THE CHANNELS: a host killed mid-answer ---- */
// the host process under the channel's shell, and whether it is blocked writing into a full pipe
const childOf = pid => +fs.readFileSync(`/proc/${pid}/task/${pid}/children`, 'utf8').trim().split(/\s+/)[0] || 0;
const blockedWriting = pid => { try { return fs.readFileSync(`/proc/${pid}/wchan`, 'utf8').includes('pipe_write'); } catch (e) { return false; } };
// a batch whose answer is many times the 64 KB pipe: the host blocks on it until the answer is read
const BIG_ANSWER = JSON.stringify({ op: 'batch', calls: Array.from({ length: 400 }, () => ({ op: 'hello' })) });

function killMidAnswer() {
  const hc = require('./hostchan.cjs');
  const c = hc.open(TARGET);
  c.request('{"op":"hello"}');
  c.send(BIG_ANSWER);
  let host = 0;
  for (let i = 0; i < 6000 && !(host && blockedWriting(host)); i++) { host = host || childOf(c.pid); sleep(5); }
  check(host && blockedWriting(host), 'the host blocks writing an answer larger than its pipe');
  process.kill(host, 'SIGKILL');
  let x = thrown(() => c.receive());
  check(x && /the host exited/.test(x.message), 'a host killed mid-answer reads as dead, not as an answer', x ? x.message : 'an answer came back');
  x = thrown(() => c.request('{"op":"hello"}'));
  check(x && /the host exited/.test(x.message), 'and every request after throws');
  c.close();

  const t = hc.tee([TARGET, TARGET]);
  const d0 = hc.differences.length;
  t.request('{"op":"hello"}');
  t.close();
  check(hc.differences.length === d0, 'a tee of one build twice answers alike, instance included');
}

function relay() {
  const r = spawnSync('python3', [path.join(ROOT, 'tools/wiki/hostcheck_relay.py')], { cwd: ROOT, encoding: 'utf8', timeout: 600000 });
  process.stdout.write(r.stdout);
  if (r.status !== 0) { failed++; console.log(`FAIL hostcheck_relay.py exited ${r.status}\n${r.stderr}`); }
}

console.log(`hostcheck: the ${TARGET} build`);
lines();
liveSims();
await dropIn();
killMidAnswer();
relay();
console.log(failed ? `${failed} FAILED` : 'all as named');
process.exit(failed ? 1 : 0);
