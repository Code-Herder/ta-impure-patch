/* simparity.mjs -- the in-game wiki's parity check: the DLL's simulation, tagpu/ddraw/src/tagpu_wikisim.c over
   tagpu_wikidata.c's reading of the archive the DLL embeds, against the mockup's tools/wiki/mockup/sim.js, tick for
   tick and field by field.

   usage: node tools/wiki/simparity.mjs [--wine] [--shard K/N]
     exit 0 when every demo and every rule case agrees and every malformed case loads, 1 when one does not, 2 when the C side cannot be built or run;
     --shard K/N (K from 1) plays only every Nth of the demos' contexts, from the Kth, and shard 1 alone the rule, refusal, data and malformed
     cases: N processes, each building its own C side, check together what one does alone
   usage: node tools/wiki/simparity.mjs --hash [--layout FILE|null] [--pack DIR] [--side N] [--iface 0|1] [--scripts]
                                            [--frame ENTRY/DEMO@TICK[,...]]
     prints `<entry>/<demo> <hash>` for every demo the tree names: sim.js's state hash on its last tick, 16 hex digits
     (a game entry's demo: replay.js's replay played to its end, and the replay's hash, tagpu_wikireplay_hash's), the number
     the DLL's tagpu_wikisim_hash must give for the same demo played to its end with the same sidebar layout (by
     default art/wiki/side/fixture.json; `null` for none) in the same context: side N (0 by default), Interface Type
     0 or 1 (1 by default), and with --pack the casting of side N in DIR's side.json, whose layout is then the
     default (tools/wiki/sidepacks.py writes the packs), and with --scripts the units' scripts, as the game's check
     plays when its log says they are on: each type's from the first pack that holds it, DIR's stage packs first, then
     the gfx packs under tools/wiki/mockup/gfx/. A demo sim.js refuses prints `refused` and why. Builds nothing.
   usage: node tools/wiki/simparity.mjs --try DUMP
     prints sim.js's state hash of Try it yourself fed the inputs the game's state dump logged (tools/wiki_regression.py
     try), the number the dump's own hash must equal. The layout is the dump's `layout` when it records one, else the
     fixture, and the context its `ctx` (side, Interface Type, the casting written whole, and with `cob` the units'
     scripts from the gfx packs), else sim.js's default. Builds nothing.

   It packs art/wiki/ as the Makefile does (tools/hudpak.py --set wiki), builds tagpu/ddraw/tests/wikitest.c with the
   real sources, and plays every demo of every entry the tree names through both, comparing every field of the view,
   every unit's orders and the state hash on every tick; the map, which nothing writes once the scene is laid out, on
   the first and the last tick. It plays them with the sidebar of art/wiki/side/fixture.json, the layout the wiki
   falls back to, with fixture-tall.json's 768-row column, and with none (both C sides read the layouts out of the
   archive), each in simcheck.mjs's contexts: the entry's own types and each casting of tools/wiki/casting-fixture.json
   (identity, shifted, unbound), each at Interface Type 1 and 0; a demo one side refuses in a context the other must
   refuse too. In each context it then plays every game entry under art/wiki/entries/ (frame "game", named by the tree
   or not) the same way, each distinct game script (demos sharing one play the same game) through WikiSim.game and
   wikitest `game` with the fixture casting's rival beside it (casting-fixture.json `rivals`), and the stall case:
   every seat cut to 200, in stall mode tick for tick and in refuse mode, where it must end starved; then every demo of
   it as a replay, through replay.js and wikitest `replay`, demo tick by demo tick -- the replay's view and hash
   (replayHash, tagpu_wikireplay_hash's twin, a live entry's rewind state and P's hash included), the game's view,
   orders and hash -- and Try it yourself's press trace through replay.js's create and wikitest `replaytrace`: TRACE
   on a replay entry, LIVE_TRACE (the live HUD's presses and the world's input) on a live one. The fork pass forks
   each distinct game at the refresh ticks (forkCases); the release sweep plays each live entry's games with Back to
   now's release after every tick up to the script's last input, each to its end, through sim.js and wikitest
   `sweep`, in two contexts. It then plays simcheck.mjs's rule cases -- the rules no demo shows -- through both: each case is run
   against sim.js with every call recorded, `create` and `play` with the layout and the context each was given, the
   recording is
   replayed through the C sim, and every answer the case read (each view, with the state hash after it, orders, done)
   is compared. For each demo and case it names the first differing tick or call and field. It compares the build
   trip's and the replay layer's constants, and the recorder's rules (tagpu_recrules.h, which the wiki and the recorder
   share) through wikitest `rules` against replay.js's twins, the refresh grid against tools/replay/tarec.py's is_refresh
   too. Then the refusal cases: data, castings, contexts and layouts each side must refuse or accept
   alike -- the demo's `sidebar`, the build trip's data rule, the text's tokens, the actions' data rule at each
   interface type, a unit's G cells, every bound of the casting and of the layout on both sides of it, the binding of
   an entry to a casting, a scene its bound footprints do not fit, the context's interface type and side, and every
   data rule of a game entry (its frame, seats, rival and corpse units, its demos' games and their kills, and their
   viewer's scripts, made into replays) -- each made into a sim by text.js's checkEntry and sim.js and by the C readers
   and init. Then the data read back: every casting of
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
import { cobHost, scripts, sounds, soundSource } from './cobhost.mjs';

const require = createRequire(import.meta.url);
const WikiSim = require('./mockup/sim.js');
const WikiReplay = require('./mockup/replay.js');
const WikiFrame = require('./mockup/frame.js');
const WikiText = require('./mockup/text.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const SIDE = name => JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/side/${name}.json`), 'utf8'));
const FIXTURE = SIDE('fixture');
const WINE = process.argv.includes('--wine');
const SHARD_ARG = process.argv.includes('--shard') ? process.argv[process.argv.indexOf('--shard') + 1] : '1/1';
const [SHARD, SHARDS] = (/^(\d+)\/(\d+)$/.exec(SHARD_ARG) || []).slice(1).map(Number);
if (!(SHARDS >= 1 && SHARD >= 1 && SHARD <= SHARDS)) {
  console.error(`simparity: --shard ${SHARD_ARG} is not K/N with 1 <= K <= N`);
  process.exit(2);
}
const SRCS = ['src/tagpu_wikidata.c', 'src/tagpu_wikireplay.c', 'src/tagpu_json.c', 'src/tagpu_pak.c', 'src/tagpu_placegeo.c', 'src/tagpu_sweepgeo.c',
  'src/tagpu_wikicob.c', 'src/tagpu_cobvm.c', 'src/tagpu_cob_safe.c'];

/* ---- the state hash: 64-bit FNV-1a, its halves in two uint32s so that no BigInt is needed ---- */
function fnv() {
  let hi = 0xcbf29ce4, lo = 0x84222325;
  // h = (h ^ b) * (2^40 + 0x1b3) mod 2^64
  const byte = b => {
    lo = (lo ^ b) >>> 0;
    const t0 = (lo & 0xffff) * 0x1b3, t1 = (lo >>> 16) * 0x1b3 + (t0 >>> 16);
    hi = (Math.imul(hi, 0x1b3) + (t1 >>> 16) + (lo << 8)) >>> 0;
    lo = (((t1 & 0xffff) << 16) | (t0 & 0xffff)) >>> 0;
  };
  const int = n => { for (let k = 0; k < 4; k++) byte((n >>> (8 * k)) & 255); };
  const str = s => { const b = utf8.encode(s || ''); int(b.length); for (const c of b) byte(c); };
  return { byte, int, str, hex: () => hi.toString(16).padStart(8, '0') + lo.toString(16).padStart(8, '0') };
}
/* THE REPLAY'S HASH, tagpu_wikireplay_hash's twin over replay.js's view, in tagpu_wikireplay.h's order; the game's
   state hash as its low and its high 32 bits; then a live replay's block, the past simulation's state hash last. A
   trip from the game is WIKI_REPLAY_AT_GAME, -2. */
const f32 = new DataView(new ArrayBuffer(4)), f32bits = x => { f32.setFloat32(0, x, true); return f32.getUint32(0, true); };
function replayHash(R) {
  const v = R.view(), g = v.game, h = fnv(), ctl = c => c === WikiReplay.AT_GAME ? -2 : WikiReplay.CONTROLS.indexOf(c);
  const b = x => h.int(x ? 1 : 0);
  h.int(v.tick); b(v.done); h.int(v.clock); h.int(v.waiting ? 4 : 0); b(v.stalled); h.str(v.caption); b(v.failed); h.int(v.ignored);
  h.int(g.tick); h.int(g.end); b(g.paused); h.int(g.speed); h.int(g.acc); h.int(g.target); h.int(g.gen); h.int(g.quiet);
  b(v.cursor);
  if (v.cursor) {
    const c = v.cursor;
    h.int(c.from === null ? -1 : ctl(c.from)); h.int(c.fromValue); h.int(ctl(c.to)); h.int(c.value); h.int(c.at);
    b(c.drag);
    if (c.drag) { h.int(c.drag.drag); h.int(c.drag.over); }
  }
  h.int(v.books.length); for (const t of v.books) h.int(t);
  h.int(v.toast ? WikiReplay.TOASTS.indexOf(v.toast.kind) : -1); h.int(v.toast ? v.toast.at : 0); h.int(v.toast ? v.toast.value : 0);
  b(v.question); b(v.question && v.question.wasPaused); b(v.left);
  h.int(v.walked); h.int(v.marks.length); for (const m of v.marks) for (const x of m) h.int(x);
  const gh = R.game ? stateHash(R.game, R.game.view()) : '0000000000000000';
  h.int(parseInt(gh.slice(8), 16)); h.int(parseInt(gh.slice(0, 8), 16));
  if (v.live) liveHash(R, v, h);
  if (!v.tk) return h.hex();
  // a takeover replay's block, tagpu_wikireplay.h's order
  const T = v.tk;
  h.int(WikiReplay.MODES.indexOf(T.mode)); h.int(T.human); h.int(WikiReplay.PICKS.indexOf(T.pick)); h.int(T.pickR); h.int(T.pickEvict);
  h.int(T.pickParent); h.int(T.pickTl); h.int(T.pickSerial); h.int(T.serial); h.int(T.lanes); h.int(T.armed); b(T.bg); b(T.tkFull);
  h.int(T.saved.length); for (const x of T.saved) h.int(x);
  h.int(T.timelines.length);
  for (const t of T.timelines) { h.int(t.id); h.int(t.parent); h.int(t.R); h.int(t.last); b(t.live); h.int(t.used); h.int(t.mode === 'stall' ? 1 : 0); h.int(t.seat); }
  h.int(T.walker);
  return h.hex();
}
// the hash's live block
function liveHash(R, v, h) {
  const b = x => h.int(x ? 1 : 0);
  const L = v.live;
  h.int(WikiReplay.RW.indexOf(L.rw)); h.int(L.G); h.int(L.now); h.int(WikiReplay.ARMS.indexOf(L.arm)); h.int(L.armR); h.int(L.armEvict);
  h.int(L.dragT); b(L.clash); h.int(L.nlogged); h.int(L.selected); h.int(L.timelines.length);
  for (const t of L.timelines) { h.int(t.id); h.int(t.parent); h.int(t.R); h.int(t.last); b(t.live); h.int(t.used); h.int(t.mode === 'stall' ? 1 : 0); h.int(t.nlog); }
  h.int(v.toast ? v.toast.evict : -1); h.int(L.back); b(L.full);
  h.int(f32bits(L.bar.be)); h.int(f32bits(L.bar.bm)); for (const x of L.bar.rates) h.int(f32bits(x)); h.int(L.bar.dt);
  const ph = R.past ? stateHash(R.past, R.past.view()) : '0000000000000000';
  h.int(parseInt(ph.slice(8), 16)); h.int(parseInt(ph.slice(0, 8), 16));
}
const KEYS = ['shift', 'alt', 'ctrl', 'z', 'x', 'e', 'lmb', 'rmb'];          // WIKI_KEY_* order
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
  int(sim.tick); int(sim.done ? 1 : 0); int(sim.side); int(sim.iface);
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
      else if (o.k === 'reclaim') { str(o.feat ?? o.target); int(o.feat !== undefined ? 1 : 0); if (o.feat !== undefined) { dbl(o.x); dbl(o.z); } }
      else str(o.target);
    }
  }
  int(v.ghost ? 1 : 0);
  if (v.ghost) { str(v.ghost.type); dbl(v.ghost.x); dbl(v.ghost.z); int(v.ghost.ok ? 1 : 0); }
  int(v.shape ? SHAPE[v.shape] : 0); int(v.sites.length);
  for (const s of v.sites) { dbl(s.x); dbl(s.z); int(STATE[s.state]); }
  dbl(v.cursor.x); dbl(v.cursor.z);
  for (const k of KEYS) int(v.keys.includes(k) ? 1 : 0);
  str(v.mode.startsWith('build:') ? v.mode.slice('build:'.length) : '');
  int(v.spacing ?? -1);
  int(v.drag ? 1 : 0); if (v.drag) { dbl(v.drag.x0); dbl(v.drag.z0); }
  str(v.caption);
  int(v.side ? 1 : 0);
  if (v.side) {
    const d = v.side;
    int(d.h); str(d.screen); str(d.unit); int(d.page); int(d.state.length); for (const x of d.state) int(x); str(d.under);
  }
  str(v.locked);
  int(v.clock); int(v.waiting ? WikiSim.UNTILS.indexOf(v.waiting) + 1 : 0); int(v.stalled ? 1 : 0);
  int(v.cob ? 1 : 0);
  if (v.cob) for (const half of [v.cob[1], v.cob[0]]) for (let k = 0; k < 4; k++) byte((half >>> (8 * k)) & 255);
  int(v.parts.length);
  for (const p of v.parts) for (const w of p) int(w);
  int(v.partsCut ? 1 : 0);
  int(v.bursts.length);
  for (const e of v.bursts) for (const w of e) int(w);
  int(v.sound ? 1 : 0);
  if (v.sound) {
    const d = v.sound;
    for (const x of d.next) int(x);
    int(d.last); int(d.queue.length);
    for (const [kind, tick, unit, type, prio] of d.queue) { int(kind); int(tick); str(unit); str(type); int(prio); }
    int(d.total);
    for (const e of d.events) for (const w of e) int(w);
    for (const c of d.cant) int(c);
  }
  int(v.feats.length);
  for (const f of v.feats) { str(f.id); str(f.name); int(f.ox); int(f.oz); }
  int(v.gone.length);
  for (const [x, z] of v.gone) { int(x); int(z); }
  int(v.reclaim ? 1 : 0);
  if (v.reclaim) {
    int(WikiSim.KINDS.indexOf(v.reclaim.kind));
    int(v.reclaim.ring ? 1 : 0);
    if (v.reclaim.ring) { int(v.reclaim.ring.x); int(v.reclaim.ring.z); int(v.reclaim.ring.r); }
  }
  // 12. a game's own part (tagpu_wikisim.h), only in a game
  if (sim.game) {
    const g = v.game;
    int(g.seats.length);
    for (const z of g.seats) {
      int(z.units); int(z.lost); int(z.out ? 1 : 0); int(z.cmdLost ? 1 : 0);
      for (const n of ['m', 'mmax', 'mi', 'mo', 'wm', 'e', 'emax', 'ei', 'eo', 'we']) dbl(z[n]);
    }
    int(g.mode === 'stall' ? 1 : 0); int(g.starved ? 1 : 0); int(g.badKill ? 1 : 0); int(g.nkill);
    for (const L of g.ledgers) for (const x of L) dbl(x);
  }
  // 13. a game's input seats beyond seat 0 (tagpu_wikisim.h), only in a game with more than one
  if (sim.game && v.nin > 1) {
    int(v.nin); int(v.viewed);
    for (const q of v.inputs) {
      int(q.ghost ? 1 : 0);
      if (q.ghost) { str(q.ghost.type); dbl(q.ghost.x); dbl(q.ghost.z); int(q.ghost.ok ? 1 : 0); }
      int(q.shape ? SHAPE[q.shape] : 0); int(q.sites.length);
      for (const s of q.sites) { dbl(s.x); dbl(s.z); int(STATE[s.state]); }
      dbl(q.cursor.x); dbl(q.cursor.z);
      for (const k of KEYS) int(q.keys.includes(k) ? 1 : 0);
      str(q.mode.startsWith('build:') ? q.mode.slice('build:'.length) : '');
      int(q.spacing ?? -1);
      int(q.drag ? 1 : 0); if (q.drag) { dbl(q.drag.x0); dbl(q.drag.z0); }
      int(q.reclaim ? 1 : 0);
      if (q.reclaim) {
        int(WikiSim.KINDS.indexOf(q.reclaim.kind));
        int(q.reclaim.ring ? 1 : 0);
        if (q.reclaim.ring) { int(q.reclaim.ring.x); int(q.reclaim.ring.z); int(q.reclaim.ring.r); }
      }
    }
  }
  return hi.toString(16).padStart(8, '0') + lo.toString(16).padStart(8, '0');
}

/* THE UNITS' SCRIPTS: every gfx pack tools/wiki/gfx.py built under tools/wiki/mockup/gfx/ (gitignored, read from the
   game), each type's script from the first that holds it. sim.js asks them of serve.py's host through cobhost.mjs and
   wikitest reads the same files; without a pack the demos play without scripts only. */
const GFX = path.join(ROOT, 'tools/wiki/mockup/gfx');
const PACKS = fs.existsSync(GFX) ? fs.readdirSync(GFX).sort().map(d => path.join(GFX, d)).filter(d => fs.existsSync(path.join(d, 'scene.json'))) : [];
const SCRIPTS = scripts(PACKS);
/* THE UNITS' SOUNDS, from the same packs, given wherever the scripts are: the game's sim has both from its one 3D load
   or neither (tagpu_wiki.c scripts_now) */
const SOUNDS = sounds(PACKS);
const COBS = Object.keys(SCRIPTS).length ? [false, true] : [false];
let host = null;
const HOST = () => (host ??= cobHost());

class CannotRun extends Error {}

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { maxBuffer: 1 << 30, encoding: 'utf8', ...opts });
  if (r.error || r.status !== 0)
    throw new CannotRun(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ` (${r.status})`}\n${r.stderr || ''}`);
  return r.stdout;
}

/* the C side built into `tmp`, this process's own: shards build at once, so none links over a program another runs */
function build(tmp) {
  if (WINE) {
    // the simulation and the loader, which normalises the scenes' facings, on SSE2 as the Makefile compiles them
    const exe = path.join(tmp, 'wikitest.exe'), sse = ['src/tagpu_wikisim.c', 'src/tagpu_wikidata.c', 'src/tagpu_wikicob.c', 'src/tagpu_sweepgeo.c',
      'src/tagpu_wikireplay.c'];
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
  const exe = path.join(tmp, 'wikitest-native');
  run('gcc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc', '-O1', '-g', '-fsanitize=address,undefined',
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
  const arg = k => { const i = process.argv.indexOf(k); return i >= 0 ? process.argv[i + 1] : undefined; };
  const file = arg('--layout'), dir = arg('--pack');
  const pack = dir ? JSON.parse(fs.readFileSync(path.join(dir, 'side.json'), 'utf8')) : null;
  const layout = !file ? (pack ? pack.layout : FIXTURE) : file === 'null' ? null : JSON.parse(fs.readFileSync(file, 'utf8'));
  const side = Number(arg('--side') ?? 0), iface = Number(arg('--iface') ?? 1);
  const stages = dir && fs.existsSync(path.join(dir, 'stage')) ? fs.readdirSync(path.join(dir, 'stage')).sort().map(d => path.join(dir, 'stage', d)) : [];
  const cob = process.argv.includes('--scripts') ? HOST().forPacks([...stages, ...PACKS]) : null;
  const snd = cob ? soundSource([...stages, ...PACKS]) : null;
  // the opponent's casting: the first layout side other than the player's, as the DLL's context gives it
  const rival = pack ? pack.casting[pack.layout.sides.findIndex((x, j) => j !== side)] ?? null : null;
  const ctx = { side, casting: pack ? pack.casting[side] ?? null : null, iface, cob, snd, rival };
  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  /* --frame: a game entry's demo ticks at which its replay's hash and frame record are printed as well, `frame
     ENTRY/DEMO@TICK HASH RECORD`, the record frame.js's for the viewer's script (tools/wiki_regression.py watch (v2)
     compares the game's dump with it) */
  const frames = (arg('--frame') || '').split(',').filter(Boolean).map(f => /^(\S+?\/\S+?)@(\d+)$/.exec(f))
    .filter(Boolean).map(m => [m[1], Number(m[2])]);
  for (const id of new Set(tree.tree.flatMap(leaves))) {
    const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
    for (const demo of entry.demos) {
      // a game entry's demo plays through the replay layer, its viewer's script to its end, and prints the replay's hash
      if (entry.frame === 'game') {
        let R;
        try { R = WikiReplay.play(entry, demo, layout, ctx); } catch (e) { console.log(`${id}/${demo.id} refused ${e.message}`); continue; }
        const at = frames.filter(([k]) => k === `${id}/${demo.id}`).map(([, t]) => t).sort((a, b) => a - b);
        for (const t of at) {
          while (!R.view().done && R.view().tick < t) R.step();
          // the world shown, as wiki.js draws it: a live replay's past simulation while the past is shown, else its
          // game. `pending`: a seek is under way, when the page holds the last world drawn and a record at this tick
          // depends on where the frames fell, so the lane compares none there
          const rv = R.view(), shown = rv.live && rv.live.rw === 'past' ? R.past : R.game;
          const rec = WikiFrame.record(rv, shown ? shown.view() : null, side, false, null);
          console.log(`frame ${id}/${demo.id}@${rv.tick} ${replayHash(R)} ${JSON.stringify({ ...rec, pending: rv.game.target >= 0 })}`);
        }
        while (!R.view().done) R.step();
        console.log(`${id}/${demo.id} ${replayHash(R)}`);
        R.free();
        continue;
      }
      let sim;
      try { sim = WikiSim.play(entry, demo, layout, ctx); } catch (e) { console.log(`${id}/${demo.id} refused ${e.message}`); continue; }
      while (!sim.done) sim.step();
      console.log(`${id}/${demo.id} ${stateHash(sim, sim.view())}`);
    }
  }
  if (host) host.close();
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
  const ctx = dump.ctx ? { ...dump.ctx, cob: dump.ctx.cob ? HOST().forPacks(PACKS) : null,
                           snd: dump.ctx.cob ? soundSource(PACKS) : null } : undefined;
  const sim = WikiSim.create(entry, demo, 'layout' in dump ? dump.layout : FIXTURE, ctx);
  for (const [t, kind, a, b] of dump.try_log) {
    while (sim.tick < t) sim.step();
    sim.input(kind === 'move' ? { move: [a, b] } : { [kind]: a });
  }
  while (sim.tick < dump.tick) sim.step();
  console.log(stateHash(sim, sim.view()));
  if (host) host.close();
  process.exit(0);
}

/* ---- the refusal cases: each a scripted flag, a layout, a context (side, interface type and casting) and an entry
   whose first demo the sim is made from; both sides must accept it or refuse it alike, and as `want` says
   (tests/wikitest.c refuse_case). ---- */
const CASTINGS = JSON.parse(fs.readFileSync(path.join(ROOT, 'tools/wiki/casting-fixture.json'), 'utf8')).castings;
// the opponent's casting beside each fixture casting: the fixture layout has one side, so the rival is given apart
const RIVALS = JSON.parse(fs.readFileSync(path.join(ROOT, 'tools/wiki/casting-fixture.json'), 'utf8')).rivals;
/* THE TAKEOVER FIXTURE: a takeover entry (`"takeover": true`) on Rewind's field, its four demos the ones
   takeover-spec.md section 9 lays out -- the picker and a seat taken (Bram, seat 1, while Ada's recorded inputs play
   on), the lanes with a kept fork watched and left, a fork saved and two deleted (the watched one after a return to
   the original), and the fourth fork's eviction -- so that the replay layer's Take over is compared in both twins
   demo tick by demo tick. Each click is aimed where its unit stands on its tick in the simulation; a takeover's part
   is quiet through the return glide. */
function takeoverFixture() {
  const base = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries/rewind.json'), 'utf8'));
  // a click selecting what stands at p, and an order to q on the three ticks after, both of `seat`
  const click = (at, seat, p) => [{ at, seat, move: p }, { at, seat, down: 'select' }, { at, seat, up: 'select' }];
  const send = (at, seat, p, q) => [...click(at, seat, p), { at: at + 1, seat, move: q }, { at: at + 2, seat, down: 'order' },
    { at: at + 3, seat, up: 'order' }];
  // the recording: Ada's solar and mex, Bram's commander and walker sent about, Ada's commander moved, b0 killed
  const inputs = [...click(10, 0, [200, 300]), { at: 10, build: 'ARMSOLAR' }, { at: 10, move: [280, 360] }, { at: 10, down: 'lmb' },
    { at: 10, up: 'lmb' }, ...click(80, 0, [140, 330]), { at: 80, build: 'ARMMEX' }, { at: 80, move: [120, 376] },
    { at: 80, down: 'lmb' }, { at: 80, up: 'lmb' }, ...send(200, 1, [520, 120], [460, 200]), ...send(450, 0, [200, 300], [330, 270]),
    ...send(700, 1, [430, 330], [300, 330]), ...send(1200, 1, [460, 200], [560, 160]), { at: 1500, kill: 'b0' }];
  const game = { end: 2700, sidebar: false, inputs };
  const K1 = { id: 1, from: 301, parent: 0, seat: 1, end: 1500, inputs: [...send(340, 1, [460, 200], [300, 120]), ...send(600, 1, [430, 330], [500, 400])] };
  const K2 = { id: 2, from: 601, parent: 0, seat: 0, end: 1800, inputs: send(640, 0, [330, 270], [200, 150]) };
  const K3 = { id: 3, from: 1201, parent: 0, seat: 0, end: 2100, inputs: send(1240, 0, [330, 270], [330, 100]) };
  const at = (t, hud, k = {}) => ({ at: t, hud, ...k });
  const e = { ...base, id: 'takeover-fixture', takeover: true, title: 'Take over a replay', subtitle: 'The replay layer\'s Take over',
    text: ['Take over plays a replay on from any moment, as one of its players.'] };
  delete e.live;
  e.demos = [
    { id: 'takeover', title: 'The picker, a seat chosen', end: 1100, game, inputs: [at(400, 'takeover'), { at: 427, until: 'seeked', max: 120 },
      at(520, 'takeas', { seat: 1 })],
      forks: [{ id: 1, from: 301, parent: 0, seat: 1, inputs: [...send(340, 1, [430, 330], [250, 120]), ...send(420, 1, [460, 200], [600, 300])] }] },
    { id: 'lanes', title: 'The lanes', end: 900, game, kept: [K1], inputs: [at(60, 'lanes'), at(240, 'lane', { fork: 1 }), at(640, 'main'),
      { at: 667, until: 'seeked', max: 120 }] },
    { id: 'save', title: 'A fork saved, two deleted', end: 900, game, kept: [K1, K2], inputs: [at(40, 'play'), at(90, 'lanes'),
      at(160, 'export', { fork: 2 }), at(300, 'delete', { fork: 1 }), at(360, 'delete', { fork: 1 }), at(480, 'lane', { fork: 2 }),
      at(540, 'track', { to: 900 }), { at: 567, until: 'seeked', max: 120 }, at(600, 'delete', { fork: 2 }), at(660, 'delete', { fork: 2 }),
      { at: 687, until: 'seeked', max: 120 }] },
    { id: 'evict', title: 'The fourth fork', end: 1000, game, inputs: [at(40, 'lanes'), at(100, 'lane', { fork: 1 }), at(160, 'main'),
      at(300, 'play'), at(360, 'track', { to: 1100 }), { at: 387, until: 'seeked', max: 120 }, at(420, 'takeover'),
      { at: 447, until: 'seeked', max: 120 }, at(560, 'takeas', { seat: 0 })],
      kept: [{ ...K1, end: 1200 }, { ...K2, seat: 1, end: 1500, inputs: send(640, 1, [430, 330], [500, 400]) }, K3],
      forks: [{ id: 4, from: 901, parent: 0, seat: 0, inputs: send(940, 0, [140, 330], [330, 330]) }] },
  ];
  return e;
}
// every game entry, the tree's or not (frame "game"), in file order
const GAMES = fs.readdirSync(path.join(ROOT, 'art/wiki/entries')).filter(f => f.endsWith('.json')).sort()
  .map(f => [f.slice(0, -5), JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries', f), 'utf8'))])
  .filter(([, e]) => e.frame === 'game');
// the games the game pass and the replay pass play: the entries and the takeover fixture
const PLAYS = [...GAMES, ['takeover-fixture', takeoverFixture()]];
const WIKI_SEAT_MIN = 200;
/* THE PRESS TRACE, Try it yourself's presses as [demo tick, control, value], each made after that tick's frame: one
   during the walk (nothing), the pause and the play, every speed, rew and fwd, a track press and its drag back and
   forth (a backward seek makes the game again), the bookmarks added and removed, Leave's question with the presses it
   ignores, leave:no twice (the second ignored), a track before the start and past the end (clamped), play at the end
   (back to the start), a name that is no control and leave:yes with no question open (ignored), rew near the start
   (clamped), and last Leave, then Yes, which ends the replay (left) */
const TRACE = { end: 150, presses: [[3, 'play', 0], [20, 'play', 0], [30, 'play', 0], [40, 'speed', 0], [41, 'speed', 0],
  [42, 'speed', 0], [43, 'speed', 0], [44, 'speed', 0], [50, 'fwd', 0], [60, 'rew', 0], [70, 'track', 900], [71, 'track', 950],
  [72, 'track', 880], [73, 'track', 880], [80, 'bookmark', 0], [85, 'bookmark', 0], [86, 'bookmark', 0], [90, 'leave', 0],
  [91, 'play', 0], [92, 'speed', 0], [93, 'leave', 0], [94, 'leave:no', 0], [95, 'leave:no', 0], [100, 'track', -50],
  [101, 'track', 99999], [112, 'play', 0], [115, 'bogus', 0], [116, 'leave:yes', 0], [120, 'rew', 0], [130, 'bookmark', 0],
  [140, 'leave', 0], [145, 'leave:yes', 0]] };
/* THE LIVE PRESS TRACE, Try it yourself on a live entry's first demo (rewind-spec.md 12.1): the presses as TRACE's
   and the world inputs as [demo tick, 'move', x, z], [demo tick, 'down' | 'up', key], each given through R.input
   after that tick's frame. In order: the rewind refused (an empty span) and a replay HUD's play (nothing); a move
   before any fork (the world takes none); inside the opening's first build trip, the open, rew, a track press
   forward and Play from here on the same tick, while that seek is pending (ignored), fwd near now (Back to now, its
   release logged on the original: it drops the rest of the trip, so the solar is never armed, and the script's next
   build selects its own builder on its tick, sim.js THE LIVE DATA RULE); the open, a track press, Play from here
   twice (the snap to 301, then fork 1 in stall mode); in fork 1 a click on the builder, a click on the column's
   solar button (DZ: its row less the bar), a placement, the button again and a Shift-drag line; armed again, Shift held across the open (its up logged into L), moves
   over the world while open (logged, the ghost follows) and a click there (ignored), Back to now from the open (a
   close); later a seek to 301, which shows 300 and plans 301, the segment's first (Cannot play), a seek forward and
   the snap to 601 and fork 2 of fork 1; in fork 2 an order; a seek into the original's segment and Play from here,
   fork 3 of the original at 2; in fork 3 a selection and a building armed, then Back to now from the past (the
   release logged); a seek past that release in fork 3 and Play from here, whose arm replaces F1, LRU, and the fork
   that deletes it while fork 2's chain keeps its log; a last open, seek and Back to now. In the fixture's column
   only the builder has pages, at 80 workerTime, so no Try-it fork starves within the game's end: the stall mode's
   ledgers are compared tick by tick here, and its debt in the game pass's stall case. */
const LIVE_TRACE = { end: 1500, presses: [
  [1, 'rewind', 0], [3, 'play', 0], [4, 'move', 300, 300], [50, 'rewind', 0], [51, 'rew', 0], [54, 'track', 30],
  [54, 'live_play', 0], [60, 'fwd', 0], [400, 'rewind', 0], [401, 'track', 350], [410, 'live_play', 0],
  [430, 'live_play', 0], [432, 'move', 140, 330], [433, 'down', 'lmb'], [434, 'up', 'lmb'], [436, 'move', -34, 158],
  [437, 'down', 'lmb'], [438, 'up', 'lmb'], [440, 'move', 72, 312], [441, 'down', 'lmb'], [442, 'up', 'lmb'],
  [444, 'move', -34, 158], [445, 'down', 'lmb'], [446, 'up', 'lmb'], [448, 'down', 'shift'], [449, 'move', 300, 120],
  [450, 'down', 'lmb'], [453, 'move', 500, 120], [456, 'up', 'lmb'], [457, 'up', 'shift'], [460, 'move', -34, 158],
  [461, 'down', 'lmb'], [462, 'up', 'lmb'], [464, 'move', 600, 300], [466, 'down', 'shift'], [468, 'rewind', 0],
  [470, 'move', 620, 320], [472, 'move', 640, 340], [473, 'down', 'lmb'], [475, 'live_now', 0], [850, 'rewind', 0],
  [851, 'track', 301], [860, 'live_play', 0], [862, 'track', 650], [872, 'live_play', 0], [890, 'live_play', 0],
  [892, 'move', 140, 330], [893, 'down', 'lmb'], [894, 'up', 'lmb'], [896, 'move', 400, 400], [897, 'down', 'rmb'],
  [898, 'up', 'rmb'], [920, 'rewind', 0], [921, 'track', 200], [930, 'live_play', 0], [940, 'live_play', 0],
  [942, 'move', 140, 330], [943, 'down', 'lmb'], [944, 'up', 'lmb'], [946, 'move', -34, 158], [947, 'down', 'lmb'],
  [948, 'up', 'lmb'], [950, 'move', 260, 200], [1000, 'rewind', 0], [1001, 'track', 30], [1010, 'live_now', 0],
  [1400, 'rewind', 0], [1401, 'track', 340], [1412, 'live_play', 0], [1430, 'live_play', 0], [1440, 'rewind', 0],
  [1441, 'track', 330], [1450, 'live_now', 0]] };
/* THE TAKEOVER PRESS TRACES, Try it yourself on a takeover entry's last demo, which holds three kept forks (F1 from
   301, seat 1, to 1200; F2 from 601, seat 1, to 1500; F3 from 1201, seat 0, to 2100): presses as TRACE's, values a
   row's seat or a lane's fork, and in live mode world inputs as LIVE_TRACE's. The first: during the walk (nothing);
   a row and a lane with the picker closed and the lanes shut (each ignored); the lanes opened, F1's lane (re-chained),
   play (paused), a seek on F1's chain, F3's lane past its R (sought), Back to main (re-chained); a seek to 0 and Take
   over there (the snap forward to 2, evicting F2, the one never watched); Take over again (closed); Take over then
   rew (closed, moved); a seek, Take over and F1's lane (closed by the switch, sought), Back to main (sought); Take
   over at the limit (evicting F2), then Delete of F2 twice (planned again: two forks left, none evicted); Save twice
   (the second refused); F3's lane and its Delete twice (the watched fork: back to the original first); a row with the
   picker closed and one out of range (ignored); Take over and Bram's row (live, seat 1). Live: a click on Bram's
   commander (his), one on Ada's (nothing of seat 1's), the commander sent; the rewind, a seek to 620 and Play from here
   twice (fork 5, of the takeover's fork 4 at 602, as seat 1); a click, Shift held across the open (its up logged), Back to now; a
   seek in the past and Back to now from there (seat 1's release logged). The second: a seek to 1100 and Take over at
   the limit, every fork unwatched (evicting F1, the earliest), Ada's row (the warning's takeover). */
const TAKEOVER_TRACES = [{ end: 500, presses: [
  [10, 'lanes', 0], [50, 'takeas', 1], [52, 'lane', 1], [53, 'lanes', 0], [55, 'lane', 1], [56, 'play', 0], [58, 'track', 1100],
  [80, 'lane', 3], [100, 'main', 0], [102, 'track', 0], [104, 'takeover', 0], [108, 'takeover', 0], [110, 'takeover', 0],
  [112, 'rew', 0], [114, 'track', 700], [130, 'takeover', 0], [140, 'lane', 1], [160, 'main', 0], [175, 'takeover', 0],
  [178, 'delete', 2], [180, 'delete', 2], [182, 'export', 1], [184, 'export', 1], [186, 'lane', 3], [188, 'delete', 3],
  [190, 'delete', 3], [192, 'takeas', 1], [194, 'takeover', 0], [196, 'takeas', 7], [198, 'takeas', 1],
  [205, 'move', 460, 200], [206, 'down', 'lmb'], [207, 'up', 'lmb'], [210, 'move', 330, 270], [211, 'down', 'lmb'],
  [212, 'up', 'lmb'], [214, 'move', 460, 200], [215, 'down', 'lmb'], [216, 'up', 'lmb'], [218, 'move', 600, 100],
  [219, 'down', 'rmb'], [220, 'up', 'rmb'], [300, 'rewind', 0], [301, 'track', 620], [320, 'live_play', 0], [340, 'live_play', 0],
  [345, 'move', 460, 200], [346, 'down', 'lmb'], [347, 'up', 'lmb'], [348, 'down', 'shift'], [350, 'rewind', 0],
  [352, 'live_now', 0], [400, 'rewind', 0], [401, 'track', 350], [410, 'live_now', 0]] },
{ end: 150, presses: [[50, 'play', 0], [52, 'track', 1100], [80, 'takeover', 0], [110, 'takeas', 0], [115, 'move', 330, 270]] }];
function refusalCases(entry) {
  const clone = v => JSON.parse(JSON.stringify(v));
  const T = WikiSim.TRIP, base = { id: 'm', scene: { selected: ['ck'] }, inputs: [{ at: 5, build: 'ARMMEX' }], end: 5 + T };
  const cases = [];
  const CTX = { side: 0, iface: 1, casting: null };
  const demo = (name, want, d, scripted = 1, ctx = {}) =>
    cases.push({ name, want, scripted, layout: FIXTURE, ...CTX, ...ctx, entry: { ...entry, demos: [{ ...base, ...d }] } });
  const lay = (name, want, edit) => { const L = clone(FIXTURE); edit(L); cases.push({ name, want, scripted: 1, layout: L, ...CTX, entry: { ...entry, demos: [base] } }); };
  const text = (name, want, edit) => { const e = clone(entry); edit(e); cases.push({ name, want, scripted: 1, layout: FIXTURE, ...CTX, entry: { ...e, demos: [{ ...base, ...(e.demos && e.demos.base) }] } }); };
  const cast = (name, want, edit) => { const C = clone(CASTINGS.identity[0]); edit(C); cases.push({ name, want, scripted: 1, layout: FIXTURE, ...CTX, casting: C, entry: { ...entry, demos: [base] } }); };
  // an entry edited by `edit` and bound to the identity casting edited by `cedit`, or to none
  const bind = (name, want, edit, cedit, casting = true) => {
    const e = clone(entry), C = clone(CASTINGS.identity[0]);
    edit(e); cedit(C);
    cases.push({ name, want, scripted: 1, layout: FIXTURE, ...CTX, casting: casting ? C : null, entry: { ...e, demos: [base] } });
  };
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
  // the build trip reads the resolved script: an order held is the left button at type 0, the right one at type 1
  demo('a build with order held at type 0', 'refused', { inputs: [{ at: 1, down: 'order' }, { at: 5, build: 'ARMMEX' }] }, 1, { iface: 0 });
  demo('a build with order held at type 1', 'accepted', { inputs: [{ at: 1, down: 'order' }, { at: 5, build: 'ARMMEX' }] }, 1, { iface: 1 });
  // the context: the interface type, and the side when a layout is given
  demo('interface type 2', 'refused', {}, 1, { iface: 2 });
  demo('interface type -1', 'refused', {}, 1, { iface: -1 });
  demo('side 1 of a one-side layout', 'refused', {}, 1, { side: 1 });
  demo('side -1', 'refused', {}, 1, { side: -1 });
  demo('side 5 with no layout is not read', 'accepted', {}, 1, { side: 5, layout: null });
  demo('side 1 of a two-side layout', 'accepted', {}, 1,
    { side: 1, layout: (L => { L.sides.push({ ...clone(L.sides[0]), name: 'CORE', types: {} }); return L; })(clone(FIXTURE)) });
  // the binding (sim.js binder): every unit one cast, no cast twice, no two units bound to one type
  bind('a unit with no cast, bound', 'refused', e => { delete e.units.ARMPW.cast; }, () => {});
  bind('a unit with no cast, no casting', 'accepted', e => { delete e.units.ARMPW.cast; }, () => {}, false);
  bind('a unit with an unknown cast, bound', 'refused', e => { e.units.ARMPW.cast = 'dance'; }, () => {});
  bind('two units of one cast, bound', 'refused', e => { e.units.ARMPW.cast = 'mex'; }, () => {});
  bind('two units of one cast, no casting', 'accepted', e => { e.units.ARMPW.cast = 'mex'; }, () => {}, false);
  bind('two units bound to one type', 'refused', () => {}, C => { C.casts.walker.type = 'ARMCK'; });
  bind('a bound type the name of an unbound unit', 'refused', () => {}, C => { C.casts.walker = null; C.casts.mex.type = 'ARMPW'; });
  bind('a cast the entry lacks bound to its type', 'accepted', () => {}, C => { C.casts.tower.type = 'ARMCK'; });
  bind('every type renamed', 'accepted', () => {}, C => { for (const k of Object.values(C.casts)) if (k) k.type = `X${k.type}`; });
  // a scene its bound footprints do not fit
  bind('a bound footprint past the interior', 'refused', () => {}, C => { C.casts.geo.footprint = [16, 16]; });
  const mex2 = e => { Object.values(e.scenes)[0].units.push({ id: 'mex2', type: 'ARMMEX', at: [496, 264] }); };
  bind('a second extractor beside the first', 'accepted', mex2, () => {}, false);
  bind('a bound footprint over a structure placed before it', 'refused', mex2, C => { C.casts.mex.footprint = [5, 5]; });
  bind('a bound footprint that still fits', 'accepted', () => {}, C => { C.casts.mex.footprint = [4, 4]; });
  bind('an unbound scene unit locks, never refuses', 'accepted', () => {}, C => { C.casts.geo = null; });
  // the scenes (sim.js sceneOf): a demo on the one its `on` names, the first by default, every map the first one's size
  const scn = (name, want, edit, d = {}) => { const e = clone(entry); edit(e);
    cases.push({ name, want, scripted: 1, layout: FIXTURE, ...CTX, entry: { ...e, demos: [{ ...base, ...d }] } }); };
  const first = e => Object.values(e.scenes)[0], second = e => { e.scenes.two = clone(first(e)); };
  scn('a demo on a second scene', 'accepted', second, { on: 'two' });
  scn('a demo on no scene of the entry', 'refused', second, { on: 'three' });
  scn('a demo on a number', 'refused', second, { on: 1 });
  scn('a demo on null', 'refused', second, { on: null });
  scn('a second scene of another size', 'refused', e => { second(e); e.scenes.two.map = { ...e.scenes.two.map, w: 42 }; });
  scn('a second scene of another size, the demo on the first', 'refused',
    e => { second(e); e.scenes.two.map = { ...e.scenes.two.map, h: 32 }; }, { on: Object.keys(entry.scenes)[0] });
  scn('no scenes', 'refused', e => { e.scenes = {}; });
  scn('scenes an array', 'refused', e => { e.scenes = [first(e)]; });
  scn('a scene name with a capital', 'refused', e => { e.scenes = { Green: first(e) }; });
  scn('a scene name starting with a digit', 'refused', e => { e.scenes = { '2d': first(e) }; });
  scn('a scene name of 17 bytes', 'refused', e => { e.scenes = { ['a'.repeat(17)]: first(e) }; });
  scn('a scene name of 16 bytes', 'accepted', e => { e.scenes = { ['a'.repeat(16)]: first(e) }; });
  scn('format 1', 'refused', e => { e.format = 1; });
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
  /* text.js's caption bound, CAPTION_MAX - 1 bytes: a button at its longer word ({Order} 11, {select} 5), a cast at the
     longer of 93 bytes and the entry's own name -- 5 casts and 2 buttons 481 bytes */
  const seven = '{builder}{mex}{Order}{geo}{select}{moho}{advbuilder}';
  text('a caption whose fill can reach 511 bytes', 'accepted', caption(seven + 'x'.repeat(30)));
  text('a caption whose fill can reach 512 bytes', 'refused', caption(seven + 'x'.repeat(31)));
  text('a caption at the bound by an entry name past 93 bytes', 'accepted', e => { e.units.ARMCK.name = '\u00e9'.repeat(51); caption(seven + 'x'.repeat(21))(e); });
  text('a caption past the bound by an entry name past 93 bytes', 'refused', e => { e.units.ARMCK.name = '\u00e9'.repeat(51); caption(seven + 'x'.repeat(22))(e); });
  text('a long text that is no caption', 'accepted', e => { e.text = [seven.repeat(4)]; });
  // a unit's G cells, sim.js's checkG
  text('a G cell inside the footprint', 'accepted', e => { e.units.ARMGEO.g = [[0, 0], [3, 3]]; });
  text('a G cell off the footprint', 'refused', e => { e.units.ARMGEO.g = [[4, 0]]; });
  text('no G cell with vent', 'refused', e => { e.units.ARMGEO.g = []; });
  text('a G cell without vent', 'refused', e => { e.units.ARMMEX.g = [[0, 0]]; });
  text('no G cell without vent', 'accepted', e => { e.units.ARMMEX.g = []; });
  text('a G cell not two integers', 'refused', e => { e.units.ARMGEO.g = [[0.5, 0]]; });
  // the reclaim entry's data (sim.js checkFeature, the scene's features and kind, the reclaim and pick inputs and trips)
  const ROCK = { footprint: [2, 2], metal: 50, height: 20, blocking: true, reclaimable: true };
  const feats = (f, placed) => e => { e.features = f; first(e).features = placed; };
  scn('a feature placed', 'accepted', feats({ rock: ROCK }, [{ name: 'rock', at: [2, 24] }]));
  scn('features not an object', 'refused', feats([ROCK], []));
  scn('a feature footprint of 16', 'accepted', feats({ rock: { ...ROCK, footprint: [16, 2] } }, []));
  scn('a feature footprint of 17', 'refused', feats({ rock: { ...ROCK, footprint: [17, 2] } }, []));
  scn('a feature metal just below 1e7', 'accepted', feats({ rock: { ...ROCK, metal: 9999999.5 } }, []));
  scn('a feature metal of 1e7', 'refused', feats({ rock: { ...ROCK, metal: 1e7 } }, []));
  scn('a feature energy below 0', 'refused', feats({ rock: { ...ROCK, energy: -1 } }, []));
  scn('a feature height of 256', 'refused', feats({ rock: { ...ROCK, height: 256 } }, []));
  scn('a feature blocking 1', 'refused', feats({ rock: { ...ROCK, blocking: 1 } }, []));
  scn('a feature name of 32 bytes', 'refused', feats({ ['r'.repeat(32)]: ROCK }, []));
  scn('65 features', 'refused', feats(Object.fromEntries(Array.from({ length: 65 }, (_, i) => [`r${i}`, ROCK])), []));
  scn('64 features', 'accepted', feats(Object.fromEntries(Array.from({ length: 64 }, (_, i) => [`r${i}`, ROCK])), []));
  scn('a scene feature naming none', 'refused', feats({ rock: ROCK }, [{ name: 'stone', at: [2, 24] }]));
  scn('a scene feature off the map', 'refused', feats({ rock: ROCK }, [{ name: 'rock', at: [39, 24] }]));
  scn('a scene feature below the scene\'s rows', 'refused', feats({ rock: ROCK }, [{ name: 'rock', at: [2, 29] }]));
  scn('a scene feature over another', 'refused', feats({ rock: ROCK }, [{ name: 'rock', at: [2, 24] }, { name: 'rock', at: [3, 25] }]));
  scn('two scene features under one id', 'refused', feats({ rock: ROCK }, [{ id: 'x', name: 'rock', at: [2, 24] }, { id: 'x', name: 'rock', at: [6, 24] }]));
  scn('a scene feature under a structure', 'refused', feats({ rock: ROCK }, [{ name: 'rock', at: [16, 5] }]));
  scn('a scene kind of metal', 'accepted', e => { first(e).kind = 'metal'; });
  scn('a scene kind of wood', 'refused', e => { first(e).kind = 'wood'; });
  demo('a reclaim trip with room', 'accepted', { inputs: [{ at: 5, reclaim: true }], end: 5 + T });
  demo('a reclaim not true', 'refused', { inputs: [{ at: 5, reclaim: 1 }], end: 5 + T });
  demo('a reclaim not true without the sidebar', 'refused', { sidebar: false, inputs: [{ at: 5, reclaim: 'yes' }], end: 5 + T });
  demo('an input inside a reclaim trip', 'refused', { inputs: [{ at: 5, reclaim: true }, { at: 40, down: 'lmb' }], end: 5 + T });
  demo('the end inside a reclaim trip', 'refused', { inputs: [{ at: 5, reclaim: true }], end: 4 + T });
  demo('a reclaim carrying a build', 'refused', { inputs: [{ at: 5, reclaim: true, build: 'ARMMEX' }], end: 5 + T });
  demo('a pick trip with room', 'accepted', { inputs: [{ at: 5, reclaim: true }, { at: 5 + T, pick: 'metal' }], end: 5 + T + WikiSim.PICK });
  demo('a pick naming no kind', 'refused', { inputs: [{ at: 5, pick: 'wood' }], end: 5 + T });
  demo('an input inside a pick trip', 'refused', { inputs: [{ at: 5, pick: 'all' }, { at: 5 + WikiSim.PICK - 1, move: [1, 1] }], end: 200 });
  demo('an input on a pick trip\'s last tick', 'accepted', { inputs: [{ at: 5, pick: 'all' }, { at: 5 + WikiSim.PICK, move: [1, 1] }], end: 200 });
  demo('a pick with the left button held', 'refused', { inputs: [{ at: 1, down: 'lmb' }, { at: 5, pick: 'energy' }], end: 200 });
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
  cast('canreclamate 1', 'accepted', tower(k => { k.canreclamate = 1; }));
  cast('canreclamate 1.5', 'refused', tower(k => { k.canreclamate = 1.5; }));
  cast('canreclamate true', 'refused', tower(k => { k.canreclamate = true; }));
  cast('activatewhenbuilt 3', 'accepted', tower(k => { k.activatewhenbuilt = 3; }));
  cast('activatewhenbuilt 0.5', 'refused', tower(k => { k.activatewhenbuilt = 0.5; }));
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
    canstop: 0, extracts: 0, energyMake: 0, energyUse: 0, buildCostMetal: 0, buildCostEnergy: 0, metalMake: 0, metalStorage: 0,
    energyStorage: 0, vent: false, g: [], builds: [], corpse: null }); }));
  // the commander and the economy's figures
  cast('the commander absent', 'refused', C => { delete C.casts.commander; });
  cast('the commander unbound', 'accepted', C => { C.casts.commander = null; });
  cast('a builds naming the commander', 'accepted', tower(k => { k.builds = ['commander']; }));
  // the economy's figures need only be finite in a casting: a game holds them to its bounds where it binds
  cast('a build cost below 0', 'accepted', tower(k => { k.buildCostMetal = -1; }));
  cast('a storage below 0', 'accepted', tower(k => { k.metalStorage = -50; }));
  cast('a storage a string', 'refused', tower(k => { k.energyStorage = '50'; }));
  cast('metal made 0.25', 'accepted', tower(k => { k.metalMake = 0.25; }));
  cast('an energyUse of -20', 'accepted', tower(k => { k.energyUse = -20; }));
  cast('an energyUse of -9999999', 'accepted', tower(k => { k.energyUse = -9999999; }));
  cast('an energyUse of 1e7', 'accepted', tower(k => { k.energyUse = 1e7; }));
  cast('an energyUse of -1e7', 'accepted', tower(k => { k.energyUse = -1e7; }));
  cast('an energyUse null', 'refused', tower(k => { k.energyUse = null; }));
  cast('a world figure below 0', 'refused', tower(k => { k.workerTime = -1; }));
  // the corpse: null, or checkFeature's record and its name
  const wreck = () => ({ name: 'armllt_dead', footprint: [2, 2], metal: 72, energy: 0, height: 20, blocking: true,
    reclaimable: true, object: 'armllt_dead' });
  const corpse = f => tower(k => { k.corpse = wreck(); f(k.corpse, k); });
  cast('a corpse null', 'accepted', tower(k => { k.corpse = null; }));
  cast('a corpse record', 'accepted', corpse(() => {}));
  cast('a corpse of a 2D feature', 'accepted', corpse(f => { delete f.object; }));
  cast('a corpse of figures absent', 'accepted', corpse(f => { delete f.metal; delete f.energy; delete f.height; delete f.blocking; }));
  cast('a corpse not an object', 'refused', tower(k => { k.corpse = 'armllt_dead'; }));
  cast('a corpse with no name', 'refused', corpse(f => { delete f.name; }));
  cast('a corpse named in 31 bytes', 'accepted', corpse(f => { f.name = 'n'.repeat(31); }));
  cast('a corpse named in 32 bytes', 'refused', corpse(f => { f.name = 'n'.repeat(32); }));
  cast('a corpse of footprint 17', 'refused', corpse(f => { f.footprint = [17, 1]; }));
  cast('a corpse of no footprint', 'refused', corpse(f => { delete f.footprint; }));
  cast('a corpse of metal 1e7', 'refused', corpse(f => { f.metal = 1e7; }));
  cast('a corpse of energy below 0', 'refused', corpse(f => { f.energy = -1; }));
  cast('a corpse of height 256', 'refused', corpse(f => { f.height = 256; }));
  cast('a corpse blocking 1', 'refused', corpse(f => { f.blocking = 1; }));
  cast('a corpse of an empty object', 'refused', corpse(f => { f.object = ''; }));
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
  for (const [n, want] of [[768, 'accepted'], [769, 'refused']])
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
  lay('a quickkey of 101', 'accepted', L => { ck1(L)[0].key = 101; });
  lay('a quickkey of 255', 'accepted', L => { ck1(L)[0].key = 255; });
  lay('a quickkey of 0', 'refused', L => { ck1(L)[0].key = 0; });
  lay('a quickkey of 256', 'refused', L => { ck1(L)[0].key = 256; });
  lay('a quickkey not an integer', 'refused', L => { ck1(L)[0].key = 'e'; });
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

/* ---- THE GAME ENTRY'S REFUSALS (sim.js checkGame, the C loader's game rules and tagpu_wikisim_init's): each case the
   game entry `game` (art/wiki/entries/watching.json) or the page entry `page` edited, its first demo played as a
   game (scripted 2, WIKI_PLAY_GAME; 3 its stall mode) or as a demo (1) ---- */
function gameRefusalCases(game, page) {
  const clone = v => JSON.parse(JSON.stringify(v));
  const cases = [];
  const CTX = { side: 0, iface: 1, casting: null, rival: null };
  const g = (name, want, edit, scripted = 2, ctx = {}) => {
    const e = clone(game);
    e.demos = [e.demos[0]];
    edit(e, e.demos[0], e.scenes[Object.keys(e.scenes)[0]], e.demos[0].game);
    cases.push({ name: `game: ${name}`, want, scripted, layout: FIXTURE, ...CTX, ...ctx, entry: e });
  };
  const p = (name, want, edit, scripted = 1) => {
    const e = clone(page);
    e.demos = [e.demos[0]];
    edit(e, e.demos[0], e.scenes[Object.keys(e.scenes)[0]]);
    cases.push({ name: `game: ${name}`, want, scripted, layout: FIXTURE, ...CTX, entry: e });
  };
  const bound = { casting: CASTINGS.identity[0], rival: RIVALS.identity };
  const T = WikiSim.TRIP;
  g('the authored game', 'accepted', () => {});
  g('the authored game in stall mode', 'accepted', () => {}, 3);
  g('the authored game with no sidebar layout', 'accepted', () => {}, 2, { layout: null });
  g('bound to identity and its rival', 'accepted', () => {}, 2, bound);
  for (const r of Object.keys(RIVALS)) g(`bound to ${r} and the rival ${r}`, 'accepted', () => {}, 2, { casting: CASTINGS[r][0], rival: RIVALS[r] });
  g('bound with no rival casting: the rival units keep their own', 'accepted', () => {}, 2, { casting: CASTINGS.identity[0] });
  {
    // a cast whose economy is outside a game's bounds: the casting is read, and the game leaves the cast unbound
    const C = clone(CASTINGS.identity[0]);
    C.casts.builder.metalStorage = -1;
    g('bound to a casting whose builder stores -1 metal', 'accepted', () => {}, 2, { casting: C, rival: RIVALS.identity });
  }
  g('played as a demo', 'refused', () => {}, 1);
  g('Try it on it', 'refused', () => {}, 0);
  p('a page entry played as a game', 'refused', () => {}, 2);
  p('a page entry played in stall mode', 'refused', () => {}, 3);
  p('the page entry as a demo', 'accepted', () => {});
  // the entry
  g('frame "page"', 'refused', e => { e.frame = 'page'; });
  g('frame null', 'refused', e => { e.frame = null; });
  g('a pace', 'refused', e => { e.pace = { build: 2 }; });
  // one numbered tab a demo in the theatre's row: at most THEATRE_TABS (tagpu_wikidata.h WIKI_THEATRE_TABS)
  const tabs = n => e => { for (let k = 1; k < n; k++) e.demos.push({ ...clone(game.demos[k % game.demos.length]), id: `tab${k}` }); };
  g(`${WikiSim.THEATRE_TABS} demos`, 'accepted', tabs(WikiSim.THEATRE_TABS));
  g(`${WikiSim.THEATRE_TABS + 1} demos`, 'refused', tabs(WikiSim.THEATRE_TABS + 1));
  /* the game frame's 4:3 bound (tagpu_wikidata_frame_fits): the narrowest map that makes a 4:3 frame accepted and one
     cell narrower refused, at rows under the deepest column's (the map grown beside it decides) and at rows past them
     (the scene's own rows do); every scene's map resized together, as sceneOf asks */
  const resize = (w, h) => e => { for (const sc of Object.values(e.scenes)) sc.map = { ...sc.map, w, h }; };
  for (const h of [30, 60]) {
    let w = 1;
    while (!WikiSim.frameFits(w, h)) w++;
    g(`a map ${w} x ${h}, the narrowest 4:3 frame`, 'accepted', resize(w, h));
    g(`a map ${w - 1} x ${h}, one cell narrower`, 'refused', resize(w - 1, h));
  }
  p('a rival unit on a page entry', 'refused', e => { e.units.ARMCK.rival = false; });
  p('a corpse on a page entry', 'refused', e => { e.features = { w: { footprint: [1, 1] } }; e.units.ARMCK.corpse = 'w'; });
  p('seats on a page entry', 'refused', (e, d, sc) => { sc.seats = [{ name: 'A', metal: 1000, energy: 1000 }]; });
  p('a page entry\'s economy figures', 'accepted', e => { e.units.ARMCK.buildCostMetal = 120; e.units.ARMCK.energyUse = -3; });
  p('a buildCostMetal below 0', 'refused', e => { e.units.ARMCK.buildCostMetal = -1; });
  p('a metalStorage null', 'refused', e => { e.units.ARMCK.metalStorage = null; });
  p('an energyUse of 1e7', 'refused', e => { e.units.ARMCK.energyUse = 1e7; });
  p('an energyUse a string', 'refused', e => { e.units.ARMCK.energyUse = '3'; });
  g('two player units of one cast, bound', 'refused', e => { e.units.ARMCOM2 = { ...e.units.ARMCOM }; }, 2, bound);
  g('two rival units of one cast, bound', 'refused', e => { e.units.CORAK2 = { ...e.units.CORAK }; }, 2, bound);
  // a token names the player's casts only (text.js words and checkEntry, tagpu_wikidata.c tokens)
  g('a token of a cast only a rival unit has', 'refused', e => { e.text = ['The {walker} walks.']; });
  g('a token of a cast the player and the rival both have', 'accepted', e => { e.text = ['The {commander} builds.']; });
  g('rival 1', 'refused', e => { e.units.CORAK.rival = 1; });
  g('rival false', 'accepted', e => { e.units.ARMCK.rival = false; });
  g('a corpse naming no feature', 'refused', e => { e.units.CORAK.corpse = 'nothing'; });
  g('a corpse a number', 'refused', e => { e.units.CORAK.corpse = 3; });
  // the seats
  g('a scene without seats', 'refused', (e, d, sc) => { delete sc.seats; });
  g('no seat', 'refused', (e, d, sc) => { sc.seats = []; });
  g('four seats', 'accepted', (e, d, sc) => { while (sc.seats.length < 4) sc.seats.push({ name: `S${sc.seats.length}`, metal: 1000, energy: 1000 }); });
  g('five seats', 'refused', (e, d, sc) => { while (sc.seats.length < 5) sc.seats.push({ name: `S${sc.seats.length}`, metal: 1000, energy: 1000 }); });
  g('a seat name empty', 'refused', (e, d, sc) => { sc.seats[0].name = ''; });
  g('a seat name of 31 bytes', 'accepted', (e, d, sc) => { sc.seats[0].name = 'n'.repeat(31); });
  g('a seat name of 32 bytes', 'refused', (e, d, sc) => { sc.seats[0].name = 'n'.repeat(32); });
  g('a seat name with a brace', 'refused', (e, d, sc) => { sc.seats[0].name = 'A{d'; });
  g('a seat name a number', 'refused', (e, d, sc) => { sc.seats[0].name = 3; });
  for (const [n, v, want] of [['metal', 199, 'refused'], ['metal', 200, 'accepted'], ['metal', 10000, 'accepted'], ['metal', 10001, 'refused'],
    ['energy', 500.5, 'refused'], ['energy', '1000', 'refused'], ['energy', 199, 'refused']])
    g(`a seat's ${n} ${JSON.stringify(v)}`, want, (e, d, sc) => { sc.seats[1][n] = v; });
  g('a seat without energy', 'refused', (e, d, sc) => { delete sc.seats[1].energy; });
  g('a seat with a colour', 'refused', (e, d, sc) => { sc.seats[1].colour = 2; });
  g('a seat not an object', 'refused', (e, d, sc) => { sc.seats[1] = 'Bram'; });
  g('seats in the demo\'s scene', 'refused', (e, d) => { d.scene = { seats: [{ name: 'A', metal: 1000, energy: 1000 }] }; });
  g('an owner past the seats', 'refused', (e, d, sc) => { sc.units[3].owner = 2; });
  g('a demo unit\'s owner past the seats', 'refused', (e, d) => { d.scene = { units: [{ id: 'x', type: 'CORAK', owner: 2, at: [300, 60] }] }; });
  g('a demo unit of seat 1', 'accepted', (e, d) => { d.scene = { units: [{ id: 'x', type: 'CORAK', owner: 1, at: [300, 60] }] }; });
  g('a feature with a unit\'s id', 'refused', (e, d, sc) => { sc.features = [{ id: 'c0', name: 'armck_dead', at: [1, 1] }]; });
  g('a feature named f1 by default, a unit f1', 'refused', (e, d, sc) => { sc.features = [{ name: 'armck_dead', at: [1, 1] }]; sc.units[0].id = 'f1'; });
  g('a feature named f1 by default, no unit f1', 'accepted', (e, d, sc) => { sc.features = [{ name: 'armck_dead', at: [1, 1] }]; });
  // features plus kills: the authored script kills two; 1 x 1 features in rows 0..4 and 28..29, clear of every unit
  const pebbles = n => Array.from({ length: n }, (_, k) => ({ name: 'pebble', at: k < 200 ? [k % 40, Math.floor(k / 40)] : [k % 40, 28 + Math.floor((k - 200) / 40)] }));
  for (const [n, want] of [[254, 'accepted'], [255, 'refused']])
    g(`${n} features and two kills`, want, (e, d, sc) => { e.features.pebble = { footprint: [1, 1], metal: 1 }; sc.features = pebbles(n); });
  // the demo and its game
  p('game on a page entry\'s demo', 'refused', (e, d) => { d.game = { end: 100, inputs: [] }; });
  g('a game demo without game', 'refused', (e, d) => { delete d.game; });
  g('game null', 'refused', (e, d) => { d.game = null; });
  g('a key game does not know', 'refused', (e, d, sc, G) => { G.speed = 2; });
  for (const [v, want] of [[0, 'refused'], [1500, 'accepted'], [5400, 'accepted'], [5401, 'refused'], [1800.5, 'refused'], ['1800', 'refused']])
    g(`end ${JSON.stringify(v)}`, want, (e, d, sc, G) => { G.end = v; });
  g('no end', 'refused', (e, d, sc, G) => { delete G.end; });
  g('game.sidebar "yes"', 'refused', (e, d, sc, G) => { G.sidebar = 'yes'; });
  g('game.sidebar absent', 'accepted', (e, d, sc, G) => { delete G.sidebar; G.inputs = G.inputs.filter(i => 'kill' in i); });
  g('a demo-level sidebar', 'refused', (e, d) => { d.sidebar = false; });
  g('inputs not a list', 'refused', (e, d, sc, G) => { G.inputs = {}; });
  // the game script's inputs
  g('a caption', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 200, caption: 'x' }); });
  g('a wait', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 200, until: 'idle', max: 60 }); });
  /* THE SEAT RULE (sim.js checkGame, tagpu_wikidata.c seat_ok): a seat of the scene's two, and 0 alone unless the
     game's sidebar is false; then each seat's own hand (resolve, tagpu_wikidata_buttons) */
  g('seat 1', 'accepted', (e, d, sc, G) => { G.inputs.push({ at: 200, seat: 1, move: [10, 10] }); });
  for (const v of [2, 4, -1, 1.5, '0', null])
    g(`seat ${JSON.stringify(v)}`, 'refused', (e, d, sc, G) => { G.inputs.push({ at: 200, seat: v, move: [10, 10] }); });
  g('seat 1 with the sidebar true', 'refused', (e, d, sc, G) => { G.sidebar = true; G.inputs = [{ at: 200, seat: 1, move: [10, 10] }]; });
  g('seat 1 with no sidebar given', 'refused', (e, d, sc, G) => { delete G.sidebar; G.inputs = [{ at: 200, seat: 1, move: [10, 10] }]; });
  g('seat 0 with the sidebar true', 'accepted', (e, d, sc, G) => { G.sidebar = true; G.inputs = [{ at: 200, seat: 0, move: [10, 10] }]; });
  g('seat 0 on a press', 'accepted', (e, d, sc, G) => { G.inputs.push({ at: 200, seat: 0, down: 'select' }, { at: 202, up: 'select' }); });
  g('two seats\' presses overlapping', 'accepted', (e, d, sc, G) => {
    G.inputs.push({ at: 200, down: 'select' }, { at: 201, seat: 1, down: 'select' }, { at: 202, up: 'select' }, { at: 203, seat: 1, up: 'select' });
  });
  g('seat 1 pressing twice', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 200, seat: 1, down: 'select' }, { at: 201, seat: 1, down: 'select' }); });
  g('seat 1 releasing seat 0\'s press', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 200, down: 'select' }, { at: 201, seat: 1, up: 'select' }); });
  g('an input with no tick', 'refused', (e, d, sc, G) => { G.inputs.push({ move: [10, 10] }); });
  g('a kill with a move', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 300, kill: 'b0', move: [1, 1] }); });
  g('a kill with seat 0', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 300, kill: 'b0', seat: 0 }); });
  g('a kill with seat 1', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 300, kill: 'b0', seat: 1 }); });
  g('a kill naming no unit', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 300, kill: 'nobody' }); });
  g('a kill naming a feature', 'refused', (e, d, sc, G) => { sc.features = [{ id: 'rock', name: 'armck_dead', at: [1, 1] }]; G.inputs.push({ at: 300, kill: 'rock' }); });
  g('a kill a number', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 300, kill: 3 }); });
  g('a unit killed twice', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 1600, kill: 'w1' }); });
  g('a kill after the end', 'refused', (e, d, sc, G) => { G.end = 1499; });
  g('a kill at the end', 'accepted', (e, d, sc, G) => { G.end = 1500; });
  g('a kill of a demo scene unit', 'accepted', (e, d, sc, G) => { d.scene = { units: [{ id: 'x', type: 'CORAK', owner: 1, at: [300, 60] }] }; G.inputs.push({ at: 300, kill: 'x' }); });
  g('a kill of the player\'s commander', 'accepted', (e, d, sc, G) => { G.inputs.push({ at: 300, kill: 'c0' }); });
  // 64 kills and 65: the demo's scene adds rival walkers to kill
  for (const [n, want] of [[64, 'accepted'], [65, 'refused']])
    g(`${n} kills`, want, (e, d, sc, G) => {
      d.scene = { units: Array.from({ length: n - 2 }, (_, k) => ({ id: `k${k}`, type: 'CORAK', owner: 1, at: [24 + 32 * (k % 19), 600 - 16] })) };
      d.scene.units.forEach((u, k) => { u.at = [24 + 32 * (k % 19), 72 + 32 * Math.floor(k / 19)]; });
      G.inputs.push(...d.scene.units.map((u, k) => ({ at: 1600 + k, kill: u.id })));
    });
  // a kill inside a build trip, which the trip's rule refuses as another input
  g('a kill inside a trip', 'refused', (e, d, sc, G) => { G.sidebar = true; G.inputs = [{ at: 10, build: 'ARMSOLAR' }, { at: 20, kill: 'c1' }]; });
  g('a kill after a trip', 'accepted', (e, d, sc, G) => { G.sidebar = true; G.inputs = [{ at: 10, build: 'ARMSOLAR' }, { at: 10 + T, kill: 'c1' }]; });

  /* THE VIEWER'S SCRIPT (sim.js checkViewer, the C loader's viewer_input and viewer_order), each case played as a
     replay (tagpu_wikireplay_init, WikiReplay.play): the demo's inputs replaced by `inputs` */
  const R = (name, want, inputs, edit = () => {}) => g(`viewer: ${name}`, want, (e, d, sc, G) => { d.inputs = inputs; edit(e, d, sc, G); });
  const last = () => cases[cases.length - 1];
  const replay = (...a) => { R(...a); last().replay = true; };
  game.demos.forEach(d => g(`the authored demo ${d.id} as a replay`, 'accepted', e => { e.demos = [clone(d)]; }));
  cases.slice(-game.demos.length).forEach(c => { c.replay = true; });
  const UP = WikiReplay.HUD_UP, DOWN = WikiReplay.HUD_DOWN, end = game.demos[0].game.end;
  replay('a caption, a wait and every control', 'accepted', [{ at: 0, caption: 'x' }, ...WikiSim.REPLAY_CONTROLS.map((c, i) =>
    ({ at: 10 + 40 * i, hud: c, ...(c === 'track' ? { to: 100 } : {}) })), { at: 400, until: 'seeked', max: 60 }]);
  replay('no inputs', 'accepted', []);
  replay('inputs not a list: read as none', 'accepted', { a: 1 });
  replay('an unknown control', 'refused', [{ at: 10, hud: 'pause' }]);
  replay('leave:yes', 'refused', [{ at: 10, hud: 'leave:yes' }]);
  replay('a control that is no string', 'refused', [{ at: 10, hud: 3 }]);
  replay('a press with no tick', 'refused', [{ hud: 'play' }]);
  replay('a button with a to', 'refused', [{ at: 10, hud: 'play', to: 5 }]);
  replay('a button with a seat', 'refused', [{ at: 10, hud: 'play', seat: 0 }]);
  replay('a track without to', 'refused', [{ at: 10, hud: 'track' }]);
  for (const [v, want] of [[0, 'accepted'], [end, 'accepted'], [-1, 'refused'], [end + 1, 'refused'], [1.5, 'refused'], ['5', 'refused']])
    replay(`a track to ${JSON.stringify(v)}`, want, [{ at: 10, hud: 'track', to: v }]);
  for (const [v, want] of [[0, 'accepted'], [end, 'accepted'], [-1, 'refused'], [end + 1, 'refused']])
    replay(`a drag to ${v}`, want, [{ at: 10, hud: 'track', to: 100, drag: v, over: 10 }]);
  for (const [v, want] of [[1, 'accepted'], [3600, 'accepted'], [0, 'refused'], [3601, 'refused'], [2.5, 'refused']])
    replay(`a drag over ${v}`, want, [{ at: 10, hud: 'track', to: 100, drag: 200, over: v }]);
  replay('a drag without over', 'refused', [{ at: 10, hud: 'track', to: 100, drag: 200 }]);
  replay('an over without drag', 'refused', [{ at: 10, hud: 'track', to: 100, over: 10 }]);
  replay('a press on the last one\'s release', 'refused', [{ at: 10, hud: 'play' }, { at: 10 + UP, hud: 'play' }]);
  replay('a press after the last one\'s release', 'accepted', [{ at: 10, hud: 'play' }, { at: 11 + UP, hud: 'play' }]);
  replay('a press on a drag\'s release', 'refused', [{ at: 10, hud: 'track', to: 100, drag: 200, over: 30 }, { at: 10 + DOWN + 30, hud: 'play' }]);
  replay('a press after a drag\'s release', 'accepted', [{ at: 10, hud: 'track', to: 100, drag: 200, over: 30 }, { at: 11 + DOWN + 30, hud: 'play' }]);
  replay('two presses on one tick', 'refused', [{ at: 10, hud: 'play' }, { at: 10, hud: 'speed' }]);
  replay('a press inside a trip, listed first', 'refused', [{ at: 20, hud: 'speed' }, { at: 10, hud: 'play' }]);
  replay('a caption inside a trip', 'accepted', [{ at: 10, hud: 'play' }, { at: 15, caption: 'x' }]);
  replay('a wait inside a trip', 'refused', [{ at: 10, hud: 'play' }, { at: 20, until: 'seeked', max: 60 }]);
  replay('a wait after a trip', 'accepted', [{ at: 10, hud: 'play' }, { at: 11 + UP, until: 'seeked', max: 60 }]);
  replay('a wait sharing its tick with a caption', 'refused', [{ at: 10, until: 'seeked', max: 60 }, { at: 10, caption: 'x' }]);
  replay('a wait until idle', 'refused', [{ at: 10, until: 'idle', max: 60 }]);
  for (const [v, want] of [[1, 'accepted'], [3600, 'accepted'], [0, 'refused'], [3601, 'refused']])
    replay(`a wait's max ${v}`, want, [{ at: 10, until: 'seeked', max: v }]);
  replay('a wait with a caption', 'refused', [{ at: 10, until: 'seeked', max: 60, caption: 'x' }]);
  replay('a caption with a move', 'refused', [{ at: 10, caption: 'x', move: [1, 1] }]);
  replay('a caption null', 'accepted', [{ at: 10, caption: null }]);
  replay('an input of at alone', 'refused', [{ at: 10 }]);
  for (const k of [{ move: [10, 10] }, { down: 'lmb' }, { up: 'lmb' }, { build: 'ARMMEX' }, { reclaim: true }, { pick: 'all' }, { kill: 'c1' }, { seat: 0 }])
    replay(`a game input, ${Object.keys(k)[0]}`, 'refused', [{ at: 10, ...k }]);
  replay('a demo end that is no number', 'refused', [], (e, d) => { d.end = '300'; });
  replay('a demo with no end', 'accepted', [], (e, d) => { delete d.end; });
  for (const c of ['rewind', 'live_play', 'live_now']) replay(`a replay demo pressing ${c}`, 'refused', [{ at: 10, hud: c }]);

  /* A LIVE ENTRY (sim.js checkGame's live rule, checkViewer's two sets, the fork parts' rules, THE LIVE DATA RULE;
     tagpu_wikidata.c's entry, viewer_input, forks and live_orders): played as a game, which checks the viewer's script
     and the parts too. The game script turned live has each build and each reclaim after a click on the commander's
     place on its tick, as the live rule asks. */
  const pick = t => [{ at: t, move: [200, 300] }, { at: t, down: 'select' }, { at: t, up: 'select' }];
  const picked = inputs => inputs.flatMap(e => (typeof e.build === 'string' && e.build) || 'reclaim' in e ? [...pick(e.at), e] : [e]);
  const live = (e, d) => { e.live = true; d.inputs = []; d.game.inputs = picked(d.game.inputs); };
  const L = (name, want, edit) => g(`live: ${name}`, want, (e, d, sc, G) => { live(e, d); edit(e, d, sc, G); });
  L('live true', 'accepted', () => {});
  g('live: live false', 'accepted', e => { e.live = false; });
  for (const v of [1, 'true', null]) g(`live: live ${JSON.stringify(v)}`, 'refused', e => { e.live = v; });
  p('live: live true on a page entry', 'refused', e => { e.live = true; });
  p('live: live false on a page entry', 'refused', e => { e.live = false; });
  L('every live control', 'accepted', (e, d) => {
    d.inputs = WikiSim.LIVE_CONTROLS.map((c, i) => ({ at: 10 + 40 * i, hud: c, ...(c === 'track' ? { to: 100, drag: 50, over: 5 } : {}) }));
  });
  for (const c of WikiSim.REPLAY_CONTROLS.filter(c => !WikiSim.LIVE_CONTROLS.includes(c)))
    L(`a live demo pressing ${c}`, 'refused', (e, d) => { d.inputs = [{ at: 10, hud: c }]; });
  L('a live demo pressing leave:yes', 'refused', (e, d) => { d.inputs = [{ at: 10, hud: 'leave:yes' }]; });
  /* THE LIVE DATA RULE: a build or a reclaim after the click selecting its builder or reclaimer on its own tick, in
     the order inputs are fed, and no pick */
  const B = (name, want, inputs) => L(`the live data rule, ${name}`, want, (e, d) => { d.game.inputs.push(...inputs); });
  B('a build after its click', 'accepted', [...pick(1000), { at: 1000, build: 'ARMSOLAR' }]);
  B('a build with no click', 'refused', [{ at: 1000, build: 'ARMSOLAR' }]);
  B('a build after a click on the tick before', 'refused', [...pick(999), { at: 1000, build: 'ARMSOLAR' }]);
  B('a build before its click', 'refused', [{ at: 1000, build: 'ARMSOLAR' }, ...pick(1000)]);
  B('a build after a click with no move', 'refused', [...pick(1000).slice(1), { at: 1000, build: 'ARMSOLAR' }]);
  B('a build after a click aimed after the press', 'refused', [pick(1000)[1], pick(1000)[0], pick(1000)[2], { at: 1000, build: 'ARMSOLAR' }]);
  B('a build after a press not released', 'refused', [...pick(1000).slice(0, 2), { at: 1000, build: 'ARMSOLAR' }, { at: 1001, up: 'select' }]);
  B('a build after an order click', 'refused', [{ at: 1000, move: [200, 300] }, { at: 1000, down: 'order' }, { at: 1000, up: 'order' }, { at: 1000, build: 'ARMSOLAR' }]);
  B('a build after its click in one input', 'accepted', [{ at: 1000, move: [200, 300], down: 'select', up: 'select' }, { at: 1000, build: 'ARMSOLAR' }]);
  B('a build after a move then a click', 'accepted', [{ at: 1000, move: [100, 100] }, ...pick(1000), { at: 1000, build: 'ARMSOLAR' }]);
  B('a build of no type with no click', 'accepted', [{ at: 1000, build: null }]);
  B('a reclaim after its click', 'accepted', [...pick(1000), { at: 1000, reclaim: true }]);
  B('a reclaim with no click', 'refused', [{ at: 1000, reclaim: true }]);
  B('a reclaim after a click on the tick before', 'refused', [...pick(999), { at: 1000, reclaim: true }]);
  B('a reclaim before its click', 'refused', [{ at: 1000, reclaim: true }, ...pick(1000)]);
  B('a pick on its reclaim\'s tick, after it', 'refused', [...pick(1000), { at: 1000, reclaim: true }, { at: 1000, pick: 'metal' }]);
  B('a pick after its reclaim\'s trip', 'refused', [...pick(1000), { at: 1000, reclaim: true }, { at: 1000 + T, pick: 'metal' }]);
  B('a pick with no reclaim', 'refused', [{ at: 1000, pick: 'all' }]);
  // seat by seat: the click is the order's own seat's
  const pickBy = (t, seat) => pick(t).map(e => ({ ...e, seat }));
  B('a seat 1 build after its own click', 'accepted', [...pickBy(1000, 1), { at: 1000, seat: 1, build: 'ARMSOLAR' }]);
  B('a seat 1 build after seat 0\'s click', 'refused', [...pick(1000), { at: 1000, seat: 1, build: 'ARMSOLAR' }]);
  B('a seat 0 build after seat 1\'s click', 'refused', [...pickBy(1000, 1), { at: 1000, build: 'ARMSOLAR' }]);
  B('a seat 1 reclaim after its own click', 'accepted', [...pickBy(1000, 1), { at: 1000, seat: 1, reclaim: true }]);
  g('live: a replay entry\'s build with no click', 'accepted', (e, d) => { d.game.inputs.push({ at: 1000, build: 'ARMSOLAR' }); });
  g('live: a replay entry\'s reclaim with no click', 'accepted', (e, d) => { d.game.inputs.push({ at: 1000, reclaim: true }); });
  g('live: a replay entry\'s pick after its reclaim', 'accepted', (e, d) => { d.game.inputs.push({ at: 1000, reclaim: true }, { at: 1000 + T, pick: 'metal' }); });
  // the fork parts
  const part = (k = {}) => ({ id: 1, from: 301, parent: 0, inputs: [{ at: 301, move: [100, 250] }, { at: 302, down: 'select' }, { at: 304, up: 'select' }], ...k });
  const F = (name, want, forks, edit = () => {}) => L(`forks, ${name}`, want, (e, d, sc, G) => { d.forks = typeof forks === 'function' ? forks(G) : forks; edit(e, d, sc, G); });
  F('one part', 'accepted', [part()]);
  F('none', 'accepted', []);
  F('an object', 'refused', { a: part() });
  F('a string', 'refused', 'x');
  g('forks on a replay entry\'s demo', 'refused', (e, d) => { d.forks = []; });
  p('forks on a page entry\'s demo', 'refused', (e, d) => { d.forks = []; });
  F(`${WikiSim.DEMO_FORKS} parts`, 'accepted', Array.from({ length: WikiSim.DEMO_FORKS }, (_, i) => part({ id: i + 1, parent: i })));
  F(`${WikiSim.DEMO_FORKS + 1} parts`, 'refused', Array.from({ length: WikiSim.DEMO_FORKS + 1 }, (_, i) => part({ id: i + 1, parent: i })));
  F('a part not an object', 'refused', [[1, 301, 0, []]]);
  F('a part without inputs', 'refused', [(({ inputs, ...k }) => k)(part())]);
  F('a part with a key more', 'refused', [part({ end: 900 })]);
  for (const [v, want] of [[0, 'refused'], [1, 'accepted'], [WikiSim.TIMELINES - 1, 'accepted'], [WikiSim.TIMELINES, 'refused'], [1.5, 'refused'], ['1', 'refused']])
    F(`an id ${JSON.stringify(v)}`, want, [part({ id: v })]);
  F('two parts of one id', 'refused', [part(), part({ from: 401, inputs: [] })]);
  for (const [v, want] of [[1, 'refused'], [2, 'accepted'], [end, 'accepted'], [end + 1, 'refused'], [2.5, 'refused']])
    F(`a from of ${v}`, want, [part({ from: v, inputs: [] })]);
  for (const [id, v, want] of [[1, -1, 'refused'], [1, 0, 'accepted'], [3, 2, 'accepted'], [3, 3, 'refused'], [3, 0.5, 'refused']])
    F(`id ${id}, parent ${v}`, want, [part({ id, parent: v })]);
  F('inputs an object', 'refused', [part({ inputs: {} })]);
  const ins = (name, want, inputs) => F(`an input ${name}`, want, [part({ inputs })]);
  ins('before from', 'refused', [{ at: 300, move: [1, 1] }]);
  ins('at the end', 'accepted', [{ at: end, move: [1, 1] }]);
  ins('past the end', 'refused', [{ at: end + 1, move: [1, 1] }]);
  ins('with no tick', 'refused', [{ move: [1, 1] }]);
  ins('a caption', 'refused', [{ at: 400, caption: 'x' }]);
  ins('a wait', 'refused', [{ at: 400, until: 'idle', max: 60 }]);
  ins('of seat 1', 'accepted', [{ at: 400, seat: 1, move: [1, 1] }]);
  ins('of seat 0', 'accepted', [{ at: 400, seat: 0, move: [1, 1] }]);
  ins('of seat 2', 'refused', [{ at: 400, seat: 2, move: [1, 1] }]);
  ins('of seat 1 building with no click of its own', 'refused', [...pick(400), { at: 400, seat: 1, build: 'ARMSOLAR' }]);
  ins('building a type of the entry', 'accepted', [...pick(400), { at: 400, build: 'ARMSOLAR' }]);
  ins('building with no click selecting its builder', 'refused', [{ at: 400, build: 'ARMSOLAR' }]);
  ins('building after a click on the tick before', 'refused', [...pick(399), { at: 400, build: 'ARMSOLAR' }]);
  ins('building no type', 'refused', [{ at: 400, build: 'NOPE' }]);
  ins('reclaiming after its click', 'accepted', [...pick(400), { at: 400, reclaim: true }]);
  ins('reclaiming with no click selecting its reclaimer', 'refused', [{ at: 400, reclaim: true }]);
  ins('reclaiming after a click on the tick before', 'refused', [...pick(399), { at: 400, reclaim: true }]);
  ins('a pick on its reclaim\'s tick', 'refused', [...pick(400), { at: 400, reclaim: true }, { at: 400, pick: 'metal' }]);
  ins('a pick after its reclaim\'s trip', 'refused', [...pick(400), { at: 400, reclaim: true }, { at: 400 + T, pick: 'metal' }]);
  ins('a reclaim not true', 'refused', [{ at: 400, reclaim: 1 }]);
  ins('a pick of no kind', 'refused', [{ at: 400, pick: 'wood' }]);
  ins('a move of one number', 'refused', [{ at: 400, move: 5 }]);
  ins('a key no input names', 'refused', [{ at: 400, down: 'meta' }]);
  ins('an action', 'accepted', [{ at: 400, down: 'order' }, { at: 402, up: 'order' }]);
  ins('a kill of a scene unit', 'accepted', [{ at: 400, kill: 'c1' }]);
  ins('a kill of a unit the game script kills too', 'accepted', [{ at: 1300, kill: 'w1' }]);
  ins('a kill naming no unit', 'refused', [{ at: 400, kill: 'nobody' }]);
  ins('a kill with a move', 'refused', [{ at: 400, kill: 'c1', move: [1, 1] }]);
  ins('a unit killed twice', 'refused', [{ at: 400, kill: 'c1' }, { at: 500, kill: 'c1' }]);
  F('a unit killed in two parts', 'accepted', [part({ inputs: [{ at: 400, kill: 'c1' }] }), part({ id: 2, from: 401, inputs: [{ at: 450, kill: 'c1' }] })]);
  // 64 kills in a part and 65: the demo's scene adds rival walkers to kill
  for (const [n, want] of [[64, 'accepted'], [65, 'refused']])
    L(`forks, ${n} kills in a part`, want, (e, d) => {
      d.scene = { units: Array.from({ length: n }, (_, k) => ({ id: `k${k}`, type: 'CORAK', owner: 1, at: [24 + 32 * (k % 19), 72 + 32 * Math.floor(k / 19)] })) };
      d.forks = [part({ inputs: d.scene.units.map((u, k) => ({ at: 400 + k, kill: u.id })) })];
    });
  // the features plus every kill, the game script's two and the parts': 254 + 2 accepted alone (above)
  for (const [n, want] of [[253, 'accepted'], [254, 'refused']])
    L(`forks, ${n} features, two kills and a part's kill`, want, (e, d, sc) => {
      e.features.pebble = { footprint: [1, 1], metal: 1 };
      sc.features = pebbles(n);
      d.forks = [part({ inputs: [{ at: 400, kill: 'c1' }] })];
    });
  return cases;
}

/* ---- THE TAKEOVER ENTRY'S REFUSALS (sim.js checkGame's takeover rules, kept forks and a part's seat; checkViewer's
   Take over set; the C loader's entry, game, forks, kept and viewer_input): each case the takeover fixture `tk`, or the
   page entry `page`, edited, its first demo (or the one an edit keeps) played as a game, or as a replay where only
   the viewer's script is at stake ---- */
function takeoverRefusalCases(tk, page) {
  const clone = v => JSON.parse(JSON.stringify(v));
  const cases = [];
  const CTX = { side: 0, iface: 1, casting: null, rival: null, layout: FIXTURE };
  // the fixture with its demo `k` alone, edited; `replay` plays it as a replay
  const t = (name, want, edit, k = 1, replay = false) => {
    const e = clone(tk);
    e.demos = [e.demos[k]];
    edit(e, e.demos[0], e.demos[0].game);
    cases.push({ name: `takeover: ${name}`, want, scripted: 2, ...CTX, entry: e, replay });
  };
  const r = (name, want, inputs, k = 1) => t(`viewer: ${name}`, want, (e, d) => { d.inputs = inputs; }, k, true);
  const K1 = tk.demos[1].kept[0], end = tk.demos[1].game.end;
  // the entry
  t('the fixture\'s takeover demo', 'accepted', () => {}, 0);
  t('the fixture\'s lanes demo', 'accepted', () => {});
  t('the fixture\'s evict demo', 'accepted', () => {}, 3);
  for (const v of [1, 'true', null]) t(`takeover ${JSON.stringify(v)}`, 'refused', e => { e.takeover = v; });
  t('takeover false, no kept', 'accepted', (e, d) => { e.takeover = false; delete d.kept; d.inputs = []; });
  t('takeover false, a kept', 'refused', (e, d) => { e.takeover = false; d.inputs = []; });
  t('takeover and live true', 'refused', (e, d) => { e.live = true; d.inputs = []; });
  t('takeover true, live false', 'accepted', e => { e.live = false; });
  cases.push({ name: 'takeover: takeover on a page entry', want: 'refused', scripted: 1, ...CTX, entry: { ...clone(page), takeover: true } });
  cases.push({ name: 'takeover: takeover false on a page entry', want: 'refused', scripted: 1, ...CTX, entry: { ...clone(page), takeover: false } });
  t('the sidebar true', 'refused', (e, d, G) => { G.sidebar = true; });
  t('the sidebar absent', 'refused', (e, d, G) => { delete G.sidebar; });
  // a kept fork, as checkGame and kept() read it
  const k = (name, want, edit) => t(`kept: ${name}`, want, (e, d) => { edit(d.kept, d); });
  k('none', 'accepted', K => { K.length = 0; });
  k('an object', 'refused', (K, d) => { d.kept = { a: K1 }; });
  k('four forks', 'refused', K => { K.push({ ...K1, id: 2 }, { ...K1, id: 3 }, { ...K1, id: 4 }); });
  k('three forks', 'accepted', K => { K.push({ ...K1, id: 2, from: 601, inputs: [] }, { ...K1, id: 3, from: 901, inputs: [] }); });
  k('a key more', 'refused', K => { K[0].colour = 1; });
  k('a key less', 'refused', K => { delete K[0].end; });
  k('a fork not an object', 'refused', K => { K[0] = [1, 301, 0, 1, 1500, []]; });
  for (const [v, want] of [[2, 'refused'], [0, 'refused'], ['1', 'refused']]) k(`the id ${JSON.stringify(v)} at index 0`, want, K => { K[0].id = v; });
  k('ids out of order', 'refused', K => { K.push({ ...K1, id: 3, from: 601, inputs: [] }); });
  for (const [v, want] of [[-1, 'refused'], [1, 'refused'], [0.5, 'refused']]) k(`a parent ${v}`, want, K => { K[0].parent = v; });
  k('a parent the fork before', 'accepted', K => { K.push({ ...K1, id: 2, parent: 1, from: 601, inputs: [] }); });
  // the refresh grid walked from the parent's first: 1, 2, 301, 601, ...; from a kept parent's 301: 301, 302, 601, ...
  for (const [v, want] of [[1, 'refused'], [2, 'accepted'], [3, 'refused'], [300, 'refused'], [301, 'accepted'], [302, 'refused'],
    [601, 'accepted'], [2401, 'accepted'], [2700, 'refused'], [2701, 'refused'], [301.5, 'refused']])
    k(`from ${v}`, want, K => { K[0].from = v; K[0].end = 2700; K[0].inputs = []; });
  for (const [v, want] of [[301, 'refused'], [302, 'accepted'], [303, 'refused'], [601, 'accepted'], [1500, 'refused'], [1501, 'refused']])
    k(`from ${v} of a kept parent from 301 to 1500`, want, K => { K.push({ ...K1, id: 2, parent: 1, from: v, end: 2700, inputs: [] }); });
  k('from past a kept parent\'s end', 'refused', K => { K[0].end = 500; K[0].inputs = []; K.push({ ...K1, id: 2, parent: 1, from: 601, end: 900, inputs: [] }); });
  for (const [v, want] of [[0, 'accepted'], [1, 'accepted'], [2, 'refused'], [-1, 'refused'], [1.5, 'refused'], ['1', 'refused']])
    k(`seat ${JSON.stringify(v)}`, want, K => { K[0].seat = v; });
  for (const [v, want] of [[300, 'refused'], [600, 'accepted'], [end, 'accepted'], [end + 1, 'refused'], [700.5, 'refused']])
    k(`end ${v}`, want, K => { K[0].end = v; K[0].inputs = []; });
  k('end at from', 'accepted', K => { K[0].end = 301; K[0].inputs = []; });
  k('inputs an object', 'refused', K => { K[0].inputs = {}; });
  k('an input before from', 'refused', K => { K[0].inputs = [{ at: 300, seat: 1, move: [1, 1] }]; });
  k('an input past its end', 'refused', K => { K[0].inputs = [{ at: 1501, seat: 1, move: [1, 1] }]; });
  k('an input at its end', 'accepted', K => { K[0].inputs = [{ at: 1500, seat: 1, move: [1, 1] }]; });
  k('an input of seat 2', 'refused', K => { K[0].inputs = [{ at: 400, seat: 2, move: [1, 1] }]; });
  k('an input of the other seat', 'accepted', K => { K[0].inputs = [{ at: 400, seat: 0, move: [1, 1] }]; });
  k('a caption', 'refused', K => { K[0].inputs = [{ at: 400, caption: 'x' }]; });
  k('a build of no type', 'refused', K => { K[0].inputs = [{ at: 400, build: 'NOPE' }]; });
  k('a build without its click', 'accepted', K => { K[0].inputs = [{ at: 400, build: 'ARMSOLAR' }]; });
  k('a kill', 'accepted', K => { K[0].inputs = [{ at: 400, kill: 'c0' }]; });
  k('a unit killed twice', 'refused', K => { K[0].inputs = [{ at: 400, kill: 'c0' }, { at: 500, kill: 'c0' }]; });
  k('a kill naming no unit', 'refused', K => { K[0].inputs = [{ at: 400, kill: 'nobody' }]; });
  // the features plus every kill: the script's one kill and a kept fork's; 1 x 1 features in rows 0..4, clear of every
  // unit and every order
  for (const [n, want] of [[254, 'accepted'], [255, 'refused']])
    t(`${n} features, a kill and a kept fork's kill`, want, e => {
      e.features.pebble = { footprint: [1, 1], metal: 1 };
      Object.values(e.scenes)[0].features = Array.from({ length: n }, (_, i) => ({ name: 'pebble', at: [i % 64, Math.floor(i / 64)] }));
      e.demos[0].kept[0].inputs = [{ at: 400, kill: 'c0' }];
    });
  t('kept on a live entry', 'refused', (e, d) => { delete e.takeover; e.live = true; d.inputs = []; });
  // a fork part's seat, on a takeover entry and a live one
  const part = (o = {}) => ({ id: 2, from: 601, parent: 0, inputs: [], ...o });
  for (const [v, want] of [[0, 'accepted'], [1, 'accepted'], [2, 'refused'], [-1, 'refused'], [1.5, 'refused'], [null, 'refused']])
    t(`a part's seat ${JSON.stringify(v)}`, want, (e, d) => { d.forks = [part({ seat: v })]; });
  t('a part without a seat', 'accepted', (e, d) => { d.forks = [part()]; });
  t('a part with a key more', 'refused', (e, d) => { d.forks = [part({ end: 900 })]; });
  t('a part\'s input of the other seat', 'accepted', (e, d) => { d.forks = [part({ seat: 1, inputs: [{ at: 700, seat: 0, move: [1, 1] }] })]; });
  t('a part\'s build without its click', 'refused', (e, d) => { d.forks = [part({ inputs: [{ at: 700, build: 'ARMSOLAR' }] })]; });
  t('a live entry\'s part of seat 1', 'accepted', (e, d) => { delete e.takeover; e.live = true; delete d.kept; d.inputs = []; d.forks = [part({ seat: 1 })]; });
  t('a live entry\'s part of seat 1 with the sidebar', 'refused', (e, d, G) => {
    delete e.takeover; e.live = true; delete d.kept; d.inputs = []; G.sidebar = true; G.inputs = G.inputs.filter(x => (x.seat ?? 0) === 0);
    d.forks = [part({ seat: 1 })];
  });
  // the viewer's script: the Take over set and its values, the live HUD's controls beside the replay HUD's
  r('every control', 'accepted', [...WikiSim.HUD_CONTROLS.map((c, i) => ({ at: 10 + 30 * i, hud: c,
    ...(c === 'track' ? { to: 100 } : c === 'takeas' ? { seat: 1 } : ['lane', 'export', 'delete'].includes(c) ? { fork: 1 } : {}) }))]);
  for (const [v, want] of [[0, 'accepted'], [1, 'accepted'], [2, 'refused'], [-1, 'refused'], [0.5, 'refused'], ['1', 'refused']])
    r(`takeas seat ${JSON.stringify(v)}`, want, [{ at: 10, hud: 'takeas', seat: v }]);
  r('takeas without a seat', 'refused', [{ at: 10, hud: 'takeas' }]);
  r('takeas with a fork', 'refused', [{ at: 10, hud: 'takeas', seat: 1, fork: 1 }]);
  for (const c of ['lane', 'export', 'delete']) {
    for (const [v, want] of [[1, 'accepted'], [WikiSim.TIMELINES - 1, 'accepted'], [0, 'refused'], [WikiSim.TIMELINES, 'refused'], [1.5, 'refused']])
      r(`${c} fork ${v}`, want, [{ at: 10, hud: c, fork: v }]);
    r(`${c} without a fork`, 'refused', [{ at: 10, hud: c }]);
    r(`${c} with a seat`, 'refused', [{ at: 10, hud: c, fork: 1, seat: 0 }]);
  }
  for (const c of ['takeover', 'lanes', 'main']) r(`${c} with a fork`, 'refused', [{ at: 10, hud: c, fork: 1 }]);
  r('a track with a seat', 'refused', [{ at: 10, hud: 'track', to: 100, seat: 0 }]);
  r('leave:yes', 'refused', [{ at: 10, hud: 'leave:yes' }]);
  // the Take over set is a takeover entry's alone
  const asReplay = e => { delete e.takeover; };
  const asLive = e => { delete e.takeover; e.live = true; };
  for (const c of WikiSim.TAKEOVER_CONTROLS) {
    const k = c === 'takeas' ? { seat: 1 } : ['lane', 'export', 'delete'].includes(c) ? { fork: 1 } : {};
    t(`a replay entry's demo pressing ${c}`, 'refused', (e, d) => { asReplay(e); delete d.kept; d.inputs = [{ at: 10, hud: c, ...k }]; }, 1, true);
    t(`a live entry's demo pressing ${c}`, 'refused', (e, d) => { asLive(e); delete d.kept; d.inputs = [{ at: 10, hud: c, ...k }]; });
  }
  return cases;
}

/* ---- THE FORK PASS (tagpu_wikisim_fork, sim.fork; rewind-spec.md section 12.1): each distinct game of each game
   entry played to R - 1 and forked at every tick R <= 1201 the writer's refresh grid gives the original that a fork
   may start at (from tick 1: 2, 301, 601, 901, 1201; R = 1, the segment's first, takes none), with an empty part and
   with a synthetic one (a box select, a build armed and placed, a kill), in both modes, then played AFTER ticks; in
   two contexts. Then THE HANDOFF, a fork taken while the recorded seat holds Shift and the left button in a
   placement gesture, and the same let go by the release alone (Back to now's); a fork of a fork; and the refusals a part or a call can meet at run time. Each case's claims are
   sim.js's alone (its verdict); the trace makes C answer every call alike. ---- */
function forkCases() {
  const FORK_AT = [2, 301, 601, 901, 1201], AFTER = 300;
  const A = ['fixture, own types, type 1', FIXTURE, { side: 0, iface: 1, casting: null, rival: null }];
  const CTXS = [A, ['fixture, shifted, type 0', FIXTURE, { side: 0, iface: 0, casting: CASTINGS.shifted[0], rival: RIVALS.shifted }]];
  /* the synthetic part: a box over the builder, ARMSOLAR armed and placed, the rival commander killed; it plays on a
     game with the sidebar too: nothing stands in the build's trip (sim.js TRIP, 90 ticks), and the builder has the
     solar on its page in every layout (the fixture's column has no page for the commander) */
  const synthetic = R => [{ at: R, move: [120, 315] }, { at: R + 1, down: 'select' }, { at: R + 3, move: [160, 345] },
    { at: R + 6, up: 'select' }, { at: R + 10, build: 'ARMSOLAR' }, { at: R + 102, seat: 0, move: [300, 420] },
    { at: R + 104, down: 'lmb' }, { at: R + 106, up: 'lmb' }, { at: R + 130, kill: 'c1' }, { at: R + 140, move: [200, 200] }];
  const play = (sim, n) => { for (let i = 0; i < n; i++) sim.step(); };
  const watch = (sim, n) => { for (let i = 0; i < n; i++) { sim.step(); sim.view(); sim.fed; } };
  const cases = [];
  for (const [id, whole] of GAMES) {
    const seen = new Set();
    for (const demo of whole.demos) {
      const key = JSON.stringify([demo.on ?? null, demo.scene ?? null, demo.game]);
      if (seen.has(key)) continue;
      seen.add(key);
      const end = demo.game.end;
      for (const [cn, layout, ctx] of CTXS)
        for (const R of FORK_AT.filter(r => r - 1 + AFTER <= end))
          for (const part of ['empty', 'synthetic'])
            for (const mode of ['refuse', 'stall'])
              cases.push({ name: `fork -- ${id}/${demo.id} (${cn}) at ${R}, ${part} part, ${mode}`, layout, ctx, run: G => {
                const sim = G(whole, demo, 'refuse');
                play(sim, R - 1);
                sim.view();
                if (sim.fork(R, part === 'empty' ? [] : synthetic(R), mode) !== 'ok') return 'FAIL the fork was refused';
                const fed = sim.fed;
                watch(sim, AFTER);
                // an empty part feeds seat 0 nothing more: the recorded future is gone
                if (part === 'empty' && sim.fed !== fed) return `FAIL an empty part fed ${sim.fed - fed} inputs`;
                return `forked at ${R}, played to ${sim.tick}`;
              } });
      /* THE HANDOFF: Shift and the left button held in a placement gesture across R - 1; the fork lets go of both
         without placing, and seat 0's selection and the armed building go. The gesture arms its building straight, so it
         plays on a game without the sidebar, whose builds take no trip through the column (sim.js TRIP). The release is
         the taken seat's alone (sim.js THE RELEASE), so a unit another seat's recorded click selected stays selected */
      const R = 601, held = JSON.parse(JSON.stringify(demo));
      if (!demo.game.sidebar) {
        // the commander picked where it stands on the build's tick, as a live entry's build needs (THE LIVE DATA RULE)
        const probe = WikiSim.game(whole, demo, A[1], A[2], 'refuse');
        while (probe.tick < R - 7) probe.step();
        const c0 = probe.view().units.find(u => u.id === 'c0');
        probe.free();
        held.game.inputs.push({ at: R - 6, move: [c0.x, c0.z] }, { at: R - 6, down: 'select' }, { at: R - 6, up: 'select' },
          { at: R - 6, build: 'ARMSOLAR' }, { at: R - 5, move: [300, 200] }, { at: R - 4, down: 'shift' },
          { at: R - 3, down: 'lmb' }, { at: R - 2, move: [340, 200] }, { at: R + 5, up: 'lmb' }, { at: R + 6, up: 'shift' });
        cases.push({ name: `fork -- ${id}/${demo.id} (${A[0]}) the handoff at ${R}`, layout: A[1], ctx: A[2], run: G => {
          const sim = G(whole, held, 'refuse');
          play(sim, R - 1);
          const v0 = sim.view();
          if (!v0.keys.includes('shift') || !v0.keys.includes('lmb') || v0.mode !== 'build:ARMSOLAR' || !v0.units.some(u => u.id === 'c0' && u.selected))
            return `FAIL nothing held before the fork: ${v0.keys} ${v0.mode}, selected ${v0.units.filter(u => u.selected).map(u => u.id)}`;
          if (sim.fork(R, [], 'refuse') !== 'ok') return 'FAIL the fork was refused';
          const v = sim.view();
          if (v.keys.length || v.mode !== 'contextual' || v.sites.length || v.drag || v.units.some(u => u.selected && u.owner === 0))
            return `FAIL the fork held on: keys ${v.keys}, mode ${v.mode}, ${v.sites.length} sites, selected ${v.units.filter(u => u.selected && u.owner === 0).map(u => u.id)}`;
          watch(sim, 60);
          return 'let go';
        } });
        // THE RELEASE alone, as Back to now calls it: the same held state let go, the recorded script playing on
        cases.push({ name: `fork -- ${id}/${demo.id} (${A[0]}) the release at ${R - 1}`, layout: A[1], ctx: A[2], run: G => {
          const sim = G(whole, held, 'refuse');
          play(sim, R - 1);
          sim.view();
          sim.release();
          const v = sim.view();
          if (v.keys.length || v.mode !== 'contextual' || v.sites.length || v.drag || v.units.some(u => u.selected && u.owner === 0))
            return `FAIL the release held on: keys ${v.keys}, mode ${v.mode}, ${v.sites.length} sites, selected ${v.units.filter(u => u.selected && u.owner === 0).map(u => u.id)}`;
          watch(sim, 60);
          return 'let go';
        } });
      }
      // a fork of a fork, the second in stall mode
      cases.push({ name: `fork -- ${id}/${demo.id} (${A[0]}) a fork of a fork`, layout: A[1], ctx: A[2], run: G => {
        const sim = G(whole, demo, 'refuse');
        play(sim, 300);
        if (sim.fork(301, synthetic(301), 'refuse') !== 'ok') return 'FAIL the first fork was refused';
        watch(sim, 100);
        if (sim.fork(401, synthetic(401), 'stall') !== 'ok') return 'FAIL the second fork was refused';
        watch(sim, 100);
        return 'forked twice';
      } });
      // the refusals at run time, in order, then a fork whose kill finds no unit, which stops the game
      cases.push({ name: `fork -- ${id}/${demo.id} (${A[0]}) the refusals`, layout: A[1], ctx: A[2], run: G => {
        const sim = G(whole, demo, 'refuse');
        play(sim, 100);
        const got = [sim.fork(100, [], 'refuse'), sim.fork(101, [], 'bogus'), sim.fork(101, [{ at: 100, move: [1, 1] }], 'refuse'),
          sim.fork(101, Array.from({ length: WikiSim.FEATS_MAX + 1 }, (_, i) => ({ at: 101 + i, kill: 'c1' })), 'refuse'),
          sim.fork(101, [{ at: 101, down: 'lmb' }, { at: 102, down: 'lmb' }], 'refuse'), sim.fork(101, [{ at: 101, kill: 'nobody' }], 'refuse')];
        sim.step();
        sim.view();
        got.push(sim.fork(102, [], 'refuse'));
        const want = ['refused', 'refused', 'refused', 'refused', 'refused', 'ok', 'refused'];
        return got.join() === want.join() ? 'every refusal refused' : `FAIL ${got.join()} for ${want.join()}`;
      } });
    }
  }
  return cases;
}

/* ---- THE COLUMN PASS (rewind-spec.md section 12.1): DZ's arithmetic. A game with the sidebar forced on, its first
   scene builder of seat 0 selected, a build trip to every type it builds, each after a click on the builder on its
   tick (sim.js THE LIVE DATA RULE), and each must arm (the trip lands on the drawn gadget only when centreOf and
   inside agree on DZ), then a click at the centre of every gadget of the builder's pages taken in world z, the
   column's row minus FRAME_BAR, and two clicks over the minimap's rows; with the fixture's column and the tall one,
   at both Interface Types. ---- */
function columnCases() {
  const cases = [], T = WikiSim.TRIP, BAR = 32;
  // a takeover entry's game has no column: its sidebar is false (sim.js checkGame)
  for (const [id, whole] of GAMES.filter(([, e]) => !e.takeover)) {
    const scene = Object.values(whole.scenes)[0];
    for (const [ln, layout] of [['fixture', FIXTURE], ['fixture-tall', SIDE('fixture-tall')]]) {
      const types = layout.sides[0].types;
      const u = scene.units.find(s => (s.owner ?? 0) === 0 && types[s.type] && (whole.units[s.type].builds || []).length);
      if (!u) continue;
      const builds = whole.units[u.type].builds, inputs = [];
      let t = 10;
      // each build after a click on the builder on its tick, as a live entry's build needs (sim.js THE LIVE DATA RULE)
      const arms = builds.map(b => {
        inputs.push({ at: t, move: u.at }, { at: t, down: 'select' }, { at: t, up: 'select' }, { at: t, build: b },
          { at: t + T, down: 'cancel' }, { at: t + T + 2, up: 'cancel' });
        const a = [t + WikiSim.TRIP_UP + 5, b];
        t += T + 10;
        return a;
      });
      const screens = [...types[u.type].pages, ...(types[u.type].orders ? [types[u.type].orders] : [])];
      const click = (x, z) => { inputs.push({ at: t, move: [x, z] }, { at: t + 1, down: 'select' }, { at: t + 2, up: 'select' }); t += 4; };
      for (const n of screens) {
        const s = layout.screens[n];
        for (const g of s.gadgets) click(-128 + g.rect[0] + g.rect[2] / 2, s.y + g.rect[1] + g.rect[3] / 2 - BAR);
      }
      click(-64, 40);
      click(-64, 100);
      const demo = { ...whole.demos[0], scene: { ...(whole.demos[0].scene || {}), selected: [u.id] },
        game: { end: t + 10, sidebar: true, inputs } };
      for (const iface of [1, 0])
        cases.push({ name: `column -- ${id} (${ln}, type ${iface}): ${builds.length} trips, ${(inputs.length - 6 * builds.length) / 3} clicks`,
          layout, ctx: { side: 0, iface, casting: null, rival: null }, run: G => {
            const sim = G(whole, demo, 'refuse');
            let bad = null;
            sim.view();
            for (let k = 0; k < demo.game.end; k++) {
              sim.step();
              const v = sim.view();
              for (const [at, b] of arms) if (sim.tick === at && v.mode !== `build:${b}` && !bad) bad = `${b} not armed at ${at} (${v.mode}, locked ${v.locked})`;
            }
            return bad ? `FAIL ${bad}` : `${arms.length} armed`;
          } });
    }
  }
  return cases;
}

/* ---- THE SEAT PASS (ingame-wiki-replay-plan.md decision 16): a synthetic two-seat game on Watching's field, the
   sidebar off, Bram's commander given ARMSOLAR to build, both seats acting on the same ticks -- a click on each
   commander, a reclaim armed and a feature clicked, Shift held across ticks with an order given, a box select, a
   building armed and placed, a move, an order every 300 ticks after -- and a kill of b0 at 700; played per tick
   through both, each seat's view and the hash with its item 13 compared. Then the fork at every refresh R <= 1201
   taking seat 0 and seat 1, with an empty part and a synthetic one, in both modes, 300 ticks on: the seat taken is fed
   nothing more after an empty part, the other seat exactly its recorded inputs of those ticks, b0's recorded kill
   never lands, and the fork views the seat taken. At 601 seat 1 holds the left button across R - 1 while seat 0 is
   taken: carried, its release at 605 resolved from the button held. Then the release of seat 1 alone between two
   ticks, and the seated fork's refusals. In the fork pass's two contexts; each case's claims are sim.js's. ---- */
function seatCases() {
  const FORK_AT = [2, 301, 601, 901, 1201], AFTER = 300, END = 1600;
  const A = ['fixture, own types, type 1', FIXTURE, { side: 0, iface: 1, casting: null, rival: null }];
  const CTXS = [A, ['fixture, shifted, type 0', FIXTURE, { side: 0, iface: 0, casting: CASTINGS.shifted[0], rival: RIVALS.shifted }]];
  const whole = JSON.parse(JSON.stringify(GAMES.find(([id]) => id === 'watching')[1]));
  whole.units.CORCOM.builds = ['ARMSOLAR'];
  // each seat's commander, the feature each reclaims, where each places its solar
  const CMD = [[200, 300], [520, 120]], FEAT = [[272, 240], [584, 176]], SOLAR = [[320, 420], [700, 120]];
  const both = (at, f) => [0, 1].map(seat => ({ at, seat, ...f(seat) }));
  const order = t => [...both(t, k => ({ move: [300 + 60 * k + t % 7, 200 + 30 * k] })), ...both(t + 1, () => ({ down: 'order' })),
    ...both(t + 2, () => ({ up: 'order' }))];
  const inputs = [
    ...both(10, k => ({ move: CMD[k] })), ...both(11, () => ({ down: 'select' })), ...both(12, () => ({ up: 'select' })),
    ...both(20, k => ({ move: FEAT[k] })), ...both(21, () => ({ reclaim: true })), ...both(22, () => ({ down: 'select' })),
    ...both(23, () => ({ up: 'select' })),
    ...both(40, () => ({ down: 'shift' })), ...both(41, k => ({ move: k ? [400, 60] : [320, 200] })), ...both(42, () => ({ down: 'order' })),
    ...both(43, () => ({ up: 'order' })), ...both(50, () => ({ up: 'shift' })),
    ...both(60, k => ({ move: k ? [440, 80] : [100, 260] })), ...both(61, () => ({ down: 'select' })),
    ...both(64, k => ({ move: k ? [600, 220] : [260, 380] })), ...both(70, () => ({ up: 'select' })),
    ...both(80, () => ({ build: 'ARMSOLAR' })), ...both(81, k => ({ move: SOLAR[k] })), ...both(82, () => ({ down: 'select' })),
    ...both(83, () => ({ up: 'select' })),
    ...both(100, k => ({ move: k ? [450, 300] : [260, 200] })), ...both(101, () => ({ down: 'order' })), ...both(102, () => ({ up: 'order' })),
    ...[350, 650, 950, 1250].flatMap(order),
    { at: 590, seat: 1, move: [400, 40] }, { at: 598, seat: 1, down: 'select' }, { at: 602, seat: 1, move: [640, 260] },
    { at: 605, seat: 1, up: 'select' },
    { at: 700, kill: 'b0' },
  ];
  const demo = { ...whole.demos[0], inputs: [],
    scene: { features: [{ id: 'rock0', name: 'armck_dead', at: [16, 14] }, { id: 'rock1', name: 'armck_dead', at: [36, 10] }] },
    game: { end: END, sidebar: false, inputs } };
  whole.demos = [demo];
  // the synthetic part of the seat taken: a click on its commander, an order, the other seat's cursor moved, w1 killed
  const synthetic = (R, h) => [{ at: R, seat: h, move: CMD[h] }, { at: R + 1, seat: h, down: 'select' }, { at: R + 2, seat: h, up: 'select' },
    { at: R + 10, seat: h, move: [380, 240] }, { at: R + 11, seat: h, down: 'order' }, { at: R + 12, seat: h, up: 'order' },
    { at: R + 20, seat: 1 - h, move: [300, 300] }, { at: R + 130, kill: 'w1' }];
  // a seat's recorded inputs of ticks a..b, the ones feed counts (no kill)
  const recorded = (k, a, b) => inputs.filter(e => !('kill' in e) && e.seat === k && e.at >= a && e.at <= b).length;
  const watch = (sim, n) => { for (let i = 0; i < n; i++) { sim.step(); sim.view(); sim.view(1); } };
  const sel = (v, k) => v.units.filter(u => u.selected && u.owner === k).map(u => u.id);
  const cases = [];
  for (const [cn, layout, ctx] of CTXS) {
    cases.push({ name: `seat -- two seats (${cn}) played to ${END}`, layout, ctx, run: G => {
      const sim = G(whole, demo, 'refuse');
      let both13 = false, shift = false;
      sim.view();
      for (let t = 1; t <= END; t++) {
        sim.step();
        const v = sim.view(), v1 = sim.view(1);
        sim.fedOf(0); sim.fedOf(1);
        if (t === 13) both13 = sel(v, 0).includes('c0') && sel(v, 1).includes('c1');
        if (t === 45) shift = v.keys.includes('shift') && v.inputs[0].keys.includes('shift') && v1.keys.includes('shift');
      }
      if (!both13) return 'FAIL the two seats\' clicks did not each select their commander at 13';
      if (!shift) return 'FAIL the two seats did not each hold Shift at 45';
      return 'both seats acted';
    } });
    for (const R of FORK_AT)
      for (const h of [0, 1])
        for (const part of ['empty', 'synthetic'])
          for (const mode of ['refuse', 'stall'])
            cases.push({ name: `seat -- (${cn}) the fork at ${R} taking seat ${h}, ${part} part, ${mode}`, layout, ctx, run: G => {
              const sim = G(whole, demo, 'refuse');
              for (let i = 0; i < R - 1; i++) sim.step();
              sim.view();
              if (sim.fork(R, part === 'empty' ? [] : synthetic(R, h), mode, h) !== 'ok') return 'FAIL the fork was refused';
              const fed = [sim.fedOf(0), sim.fedOf(1)];
              watch(sim, AFTER);
              const v = sim.view(), got = [sim.fedOf(0), sim.fedOf(1)], want = recorded(1 - h, R, R - 1 + AFTER);
              if (part === 'empty' && got[h] !== fed[h]) return `FAIL the taken seat ${h} was fed ${got[h] - fed[h]} inputs more`;
              if (got[1 - h] - fed[1 - h] !== want + (part === 'empty' ? 0 : 1))
                return `FAIL seat ${1 - h} was fed ${got[1 - h] - fed[1 - h]} inputs, its recorded ${want}${part === 'empty' ? '' : ' and the part\'s one'}`;
              if (R <= 700 && R - 1 + AFTER >= 700 && !v.units.some(u => u.id === 'b0')) return 'FAIL the recorded kill of b0 after R landed';
              if (v.viewed !== h) return `FAIL the fork views seat ${v.viewed}`;
              return `seat ${h} taken at ${R}, seat ${1 - h} fed ${want} recorded inputs`;
            } });
  }
  // seat 1's left button held across 600 while seat 0 is taken: carried, its release resolved from the button held
  cases.push({ name: `seat -- (${A[0]}) seat 1 holding the left button across the fork at 601`, layout: A[1], ctx: A[2], run: G => {
    const sim = G(whole, demo, 'refuse');
    for (let i = 0; i < 600; i++) sim.step();
    if (!sim.view().inputs[0].keys.includes('lmb')) return 'FAIL seat 1 does not hold the left button at 600';
    if (sim.fork(601, [], 'refuse', 0) !== 'ok') return 'FAIL the fork was refused';
    watch(sim, 4);
    if (!sim.view().inputs[0].keys.includes('lmb')) return 'FAIL seat 1 let go of the left button before its release';
    watch(sim, 1);
    if (sim.view().inputs[0].keys.includes('lmb')) return 'FAIL seat 1\'s recorded release at 605 did not land';
    watch(sim, 60);
    return 'held across the fork, released at 605';
  } });
  // the release of seat 1 alone: seat 0 keeps Shift held, seat 1 lets go
  cases.push({ name: `seat -- (${A[0]}) the release of seat 1 alone at 45`, layout: A[1], ctx: A[2], run: G => {
    const sim = G(whole, demo, 'refuse');
    for (let i = 0; i < 45; i++) sim.step();
    sim.release(1);
    const v = sim.view(), v1 = sim.view(1);
    if (!v.keys.includes('shift') || v1.keys.length || sel(v, 1).length) return `FAIL seat 0 keys ${v.keys}, seat 1 keys ${v1.keys}`;
    watch(sim, 60);
    return 'seat 1 let go, seat 0 held on';
  } });
  // the seated fork's refusals, in order: a seat of no input state, a part's input of one, a seat taken resolved from
  // nothing, a carried hand pressed again or let go twice; then the taken seat's fresh press, which passes
  cases.push({ name: `seat -- (${A[0]}) the seated fork's refusals`, layout: A[1], ctx: A[2], run: G => {
    const sim = G(whole, demo, 'refuse');
    for (let i = 0; i < 100; i++) sim.step();
    const got = [sim.fork(101, [], 'refuse', 2), sim.fork(101, [], 'refuse', -1), sim.fork(101, [], 'refuse', 0.5),
      sim.fork(101, [{ at: 101, seat: 2, move: [1, 1] }], 'refuse', 0), sim.fork(101, [{ at: 101, seat: 1, up: 'select' }], 'refuse', 1)];
    for (let i = 100; i < 600; i++) sim.step();
    sim.view();
    got.push(sim.fork(601, [{ at: 601, seat: 1, down: 'select' }], 'refuse', 0), sim.fork(601, [{ at: 601, seat: 1, up: 'select' }], 'refuse', 0),
      sim.fork(601, [{ at: 601, seat: 1, up: 'select' }], 'refuse', 1),
      sim.fork(601, [{ at: 601, seat: 1, down: 'select' }, { at: 603, seat: 1, up: 'select' }], 'refuse', 1));
    watch(sim, 30);
    const want = ['refused', 'refused', 'refused', 'refused', 'refused', 'refused', 'refused', 'refused', 'ok'];
    return got.join() === want.join() ? 'every refusal refused' : `FAIL ${got.join()} for ${want.join()}`;
  } });
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
    for (const n of ['buildTime', 'workerTime', 'buildDistance', 'maxVelocity', 'extracts', 'energyMake', 'buildCostMetal',
      'buildCostEnergy', 'metalMake', 'metalStorage', 'energyStorage', 'energyUse']) o[n] = k[n] ?? 0;
    const f = k.corpse;
    return { ...o, canstop: (k.canstop & 1) === 1 ? 1 : 0, activatewhenbuilt: (k.activatewhenbuilt & 1) === 1 ? 1 : 0,
      canreclamate: (k.canreclamate & 1) === 1 ? 1 : 0,
      vent: k.vent ? 1 : 0, g,
      builds: (k.builds || []).reduce((m, b) => m | (1 << WikiSim.CASTS.indexOf(b)), 0),
      // WIKIFEATDEF: an absent figure 0, an absent flag false, no object ""
      corpse: f ? { name: f.name, fx: f.footprint[0], fz: f.footprint[1], metal: f.metal ?? 0, energy: f.energy ?? 0,
        height: f.height ?? 0, blocking: f.blocking ? 1 : 0, reclaimable: f.reclaimable ? 1 : 0, object: f.object ?? '' } : null };
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
  for (const [name, C] of Object.entries(RIVALS)) items.push({ kind: 'casting', name: `casting-fixture rivals ${name}`, v: C });
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
  for (const name of Object.keys(CASTINGS)) {
    fs.writeFileSync(path.join(tmp, `casting-${name}.json`), JSON.stringify(CASTINGS[name][0]));
    fs.writeFileSync(path.join(tmp, `rival-${name}.json`), JSON.stringify(RIVALS[name]));
  }
  // the identity casting with every bound cast's economy outside a game's bounds (sim.js econFits): a game leaves each unbound
  const ECON_OFF = { ...CASTINGS.identity[0], casts: Object.fromEntries(Object.entries(CASTINGS.identity[0].casts).map(([c, k]) =>
    [c, k && { ...k, metalStorage: -1, energyUse: 1e7 }])) };
  fs.writeFileSync(path.join(tmp, 'casting-identity-econoff.json'), JSON.stringify(ECON_OFF));
  fs.writeFileSync(path.join(tmp, 'cob.json'), JSON.stringify(Object.fromEntries([...new Set([...Object.keys(SCRIPTS), ...Object.keys(SOUNDS)])].map(n => {
    const t = SCRIPTS[n];
    return [n, { ...(t ? { cob: t.cob, visible: t.visible, offset: t.offset, parent: t.parent, top: t.top } : {}),
                 ...(SOUNDS[n] ? { sound: SOUNDS[n] } : {}) }];
  }))));
  if (!COBS.includes(true)) console.log('note   no gfx pack under tools/wiki/mockup/gfx/ holds a script: the demos play without their units\' scripts only');
  let context = 0;
  for (const cob of COBS) for (const side of ['fixture', 'fixture-tall', null]) for (const cname of [null, ...Object.keys(CASTINGS)]) for (const iface of [1, 0]) {
    if (context++ % SHARDS !== SHARD - 1) continue;
    const layout = side && SIDE(side), named = `${side || 'no sidebar'}, ${cname || 'own types'}, type ${iface}${cob ? ', scripts' : ''}`;
    const ctx = { side: 0, casting: cname ? CASTINGS[cname][0] : null, iface, cob: cob ? HOST().forPacks(PACKS) : null,
                  snd: cob ? soundSource(PACKS) : null };
    let cout = lines(wikitest(['demos', 'wiki.bin', side || '-', cname ? `casting-${cname}.json` : '-', String(iface), cob ? 'cob.json' : '-']));
    let at = 0;
    // one demo through sim.js (`make`) against the C sim's lines from `at` on, which it consumes
    const compare = (id, demo, make, named) => {
      {
        let sim, why = null;
        try { sim = make(); } catch (e) { why = e.message; }
        // a demo refused in this context: the C side must refuse it too, in one line
        if (why !== null || (at < cout.length && 'refused' in JSON.parse(cout[at]))) {
          const c = at < cout.length ? JSON.parse(cout[at]) : null;
          const same = c && c.entry === id && c.demo === demo.id && 'refused' in c && why !== null;
          if (c && c.entry === id && c.demo === demo.id) at++;
          demos++;
          if (same) console.log(`equal  ${id}/${demo.id} (${named}): both refused (${why})`);
          else {
            failed++;
            differ++;
            console.log(`DIFFER ${id}/${demo.id} (${named}): C ${c && 'refused' in c ? `refused (${c.refused})` : 'played'}, sim.js ${why !== null ? `refused (${why})` : 'played'}`);
            while (at < cout.length && JSON.parse(cout[at]).demo === demo.id && JSON.parse(cout[at]).entry === id) at++;
          }
          return null;
        }
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
        return sim;
      }
    };
    for (const id of ids) {
      const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
      for (const demo of entry.demos) compare(id, demo, () => WikiSim.play(entry, demo, layout, ctx), named);
    }
    if (at !== cout.length) { failed++; differ++; console.log(`DIFFER (${named}) the C sim played ${cout.length - at} frames more`); }

    /* ---- THE GAME PASS: every game entry under art/wiki/entries/, named by the tree or not, each demo's game script
       as WikiSim.game and wikitest `game` play it, in this context with the opponent's casting beside it; then THE
       STALL CASE, every seat's settings cut to SEAT_MIN so that the builds run dry: in stall mode tick for tick, and in
       refuse mode, which must end starved on the same pass in both ---- */
    const gctx = { ...ctx, rival: cname ? RIVALS[cname] : null };
    for (const [id, whole] of PLAYS) {
      /* each game once: demos that share their scene and their game script play the same game, tick for tick, in
         both simulations, so the game pass plays the first of them (the replay pass below plays every demo) */
      const games = new Set(), entry = { ...whole, demos: whole.demos.filter(d => {
        const k = JSON.stringify([d.on ?? null, d.scene ?? null, d.game]);
        return !games.has(k) && games.add(k);
      }) };
      const cut = { ...entry, scenes: Object.fromEntries(Object.entries(entry.scenes).map(([n, sc]) =>
        [n, { ...sc, seats: sc.seats.map(z => ({ ...z, metal: WIKI_SEAT_MIN, energy: WIKI_SEAT_MIN })) }])) };
      const plays = [[entry, 'refuse', 2, '', cname], [cut, 'stall', 3, ' stall', cname], [cut, 'refuse', 2, ' starved', cname]];
      // with the identity casting, also every cast's economy outside the game's bounds: both leave each cast unbound
      if (cname === 'identity') plays.push([entry, 'refuse', 2, ' economy off', 'identity-econoff']);
      for (const [e, mode, scripted, tag, cfile] of plays) {
        const pctx = cfile === 'identity-econoff' ? { ...gctx, casting: ECON_OFF } : gctx;
        fs.writeFileSync(path.join(tmp, 'game.json'), JSON.stringify(e));
        cout = lines(wikitest(['game', 'wiki.bin', 'game.json', side || '-', cfile ? `casting-${cfile}.json` : '-',
          cname ? `rival-${cname}.json` : '-', String(iface), cob ? 'cob.json' : '-', String(scripted)]));
        at = 0;
        for (const demo of e.demos) {
          const sim = compare(id, demo, () => WikiSim.game(e, demo, layout, pctx, mode), `${named}, game${tag}`);
          // the cut game in refuse mode ends starved, unless the context locked it first
          if (tag === ' starved' && sim && sim.view().locked === null && !sim.view().game.starved) {
            failed++;
            differ++;
            console.log(`DIFFER ${id}/${demo.id} (${named}, game${tag}): the game with every seat at ${WIKI_SEAT_MIN} did not starve`);
          }
          if (tag === ' economy off' && sim && sim.view().locked === null) {
            failed++;
            differ++;
            console.log(`DIFFER ${id}/${demo.id} (${named}, game${tag}): the game did not stop under the badge`);
          }
        }
        if (at !== cout.length) { failed++; differ++; console.log(`DIFFER (${named}, game${tag}) the C sim played ${cout.length - at} frames more`); }
      }
    }

    /* ---- THE REPLAY PASS: every demo of every game entry through replay.js's play and wikitest `replay`, in this
       context, demo tick by demo tick -- the replay's view and hash, the game's view, orders and hash; then THE PRESS
       TRACE, Try it yourself (create) on the entry's first demo with TRACE's presses ---- */
    // one replay against the C lines from `at` on, which it consumes; `presses` for Try it, else the viewer's script
    const compareReplay = (id, demo, make, named, presses) => {
      let R, why = null;
      try { R = make(); } catch (e) { why = e.message; }
      if (why !== null || (at < cout.length && 'refused' in JSON.parse(cout[at]))) {
        const c = at < cout.length ? JSON.parse(cout[at]) : null;
        if (c && c.entry === id && c.demo === demo.id) at++;
        demos++;
        if (c && 'refused' in c && why !== null) console.log(`equal  ${id}/${demo.id} (${named}): both refused (${why})`);
        else {
          failed++;
          differ++;
          console.log(`DIFFER ${id}/${demo.id} (${named}): C ${c && 'refused' in c ? `refused (${c.refused})` : 'played'}, replay.js ${why !== null ? `refused (${why})` : 'played'}`);
          while (at < cout.length && JSON.parse(cout[at]).demo === demo.id && JSON.parse(cout[at]).entry === id) at++;
        }
        if (R) R.free();
        return;
      }
      let first = null, ticks = 0;
      for (;;) {
        const rv = R.view(), last = rv.done || (presses && R.tick >= presses.end), g = R.game;
        const v = g ? g.view() : null;
        if (v && !(R.tick === 0 || last)) delete v.map;
        const j = { entry: id, demo: demo.id, t: R.tick, h: replayHash(R), r: rv, gh: g ? stateHash(g, g.view()) : null, v,
          o: g ? Object.fromEntries(v.units.map(u => [u.id, g.orders(u.id)])) : null };
        if (at >= cout.length) { first = { t: R.tick, at: '(the C replay ended early)', c: null, j: null }; break; }
        const d = diff(JSON.parse(cout[at++]), j, '');
        if (d && !first) first = { t: R.tick, ...d };
        ticks++;
        if (last) break;
        if (presses) for (const [t, c, x, z] of presses.presses) if (t === R.tick) {
          if (c === 'move') R.input({ move: [x, z] });
          else if (c === 'down' || c === 'up') R.input({ [c]: x });
          else R.press(c, x);
        }
        R.step();
      }
      R.free();
      demos++;
      if (first) {
        failed++;
        differ++;
        console.log(`DIFFER ${id}/${demo.id} (${named}) at demo tick ${first.t}: ${first.at}: C ${show(first.c)}, replay.js ${show(first.j)}`);
        while (at < cout.length && JSON.parse(cout[at]).demo === demo.id && JSON.parse(cout[at]).entry === id) at++;
      } else console.log(`equal  ${id}/${demo.id} (${named}): ${ticks} demo ticks`);
    };
    for (const [id, entry] of PLAYS) {
      fs.writeFileSync(path.join(tmp, 'game.json'), JSON.stringify(entry));
      const args = ['wiki.bin', 'game.json', side || '-', cname ? `casting-${cname}.json` : '-', cname ? `rival-${cname}.json` : '-',
        String(iface), cob ? 'cob.json' : '-'];
      cout = lines(wikitest(['replay', ...args]));
      at = 0;
      for (const demo of entry.demos) compareReplay(id, demo, () => WikiReplay.play(entry, demo, layout, gctx), `${named}, replay`, null);
      if (at !== cout.length) { failed++; differ++; console.log(`DIFFER (${named}, replay) the C replay played ${cout.length - at} frames more`); }
      // a live entry's Try it takes the live HUD's presses and the world's input: LIVE_TRACE; a takeover entry's its
      // TAKEOVER_TRACES, on its last demo
      const traces = entry.takeover ? TAKEOVER_TRACES.map(t => [t, { ...entry, demos: [entry.demos[entry.demos.length - 1]] }])
        : [[entry.live ? LIVE_TRACE : TRACE, entry]];
      for (const [trace, te] of traces) {
        fs.writeFileSync(path.join(tmp, 'presses.json'), JSON.stringify(trace));
        fs.writeFileSync(path.join(tmp, 'pressgame.json'), JSON.stringify(te));
        cout = lines(wikitest(['replaytrace', ...args.map(a => a === 'game.json' ? 'pressgame.json' : a), 'presses.json']));
        at = 0;
        compareReplay(id, te.demos[0], () => WikiReplay.create(te, te.demos[0], layout, gctx), `${named}, press trace`, trace);
        if (at !== cout.length) { failed++; differ++; console.log(`DIFFER (${named}, press trace) the C replay played ${cout.length - at} frames more`); }
      }
    }
  }

  /* ---- THE FORK PASS and THE COLUMN PASS (forkCases, columnCases): each case played on sim.js with every call
     recorded and replayed through wikitest `trace`, every answer compared; a case's own claims are sim.js's alone and
     fail it by its verdict. Case k plays on shard k % N + 1. ---- */
  {
    const all = [...forkCases(), ...columnCases(), ...seatCases()];
    all.forEach((c, k) => {
      if (k % SHARDS !== SHARD - 1) return;
      const ops = [], expected = [];
      let made = 0, verdict;
      // a game simulation whose every call is recorded, as the rule cases' record() records a demo's
      const G = (entry, demo, mode) => {
        const sim = WikiSim.game(entry, demo, c.layout, c.ctx, mode), n = made++;
        ops.push(JSON.stringify({ op: 'create', sim: n, scripted: mode === 'stall' ? 3 : 2, layout: c.layout ?? null, ctx: c.ctx }),
          JSON.stringify({ ...entry, demos: [demo] }));
        const ask = (op, v) => { ops.push(JSON.stringify({ ...op, sim: n })); expected.push(v); return v; };
        return {
          step() { ops.push(JSON.stringify({ op: 'step', sim: n })); sim.step(); },
          // the view as `seat` sees it, and the state hash (seat 0's, with item 13)
          view(seat = 0) {
            const v = ask({ op: 'view', ...(seat ? { seat } : {}) }, sim.view(seat));
            ask({ op: 'hash' }, stateHash(sim, sim.view()));
            return v;
          },
          fork(R, inputs, fmode, seat = 0) {
            let r = 'ok';
            try { sim.fork(R, inputs, fmode, seat); } catch (e) { r = 'refused'; }
            return ask({ op: 'fork', R, inputs, mode: fmode, ...(seat ? { seat } : {}) }, r);
          },
          release(seat = 0) { ops.push(JSON.stringify({ op: 'release', sim: n, ...(seat ? { seat } : {}) })); sim.release(seat); },
          get fed() { return ask({ op: 'fed' }, sim.gameNow().fed); },
          fedOf(seat) { return ask({ op: 'fed', seat }, sim.gameNow(seat).fed); },
          get tick() { return sim.tick; },
        };
      };
      try { verdict = c.run(G); } catch (e) { verdict = `FAIL threw ${e.message}`; }
      fs.writeFileSync(path.join(tmp, 'trace.jsonl'), ops.join('\n') + '\n');
      const got = lines(wikitest(['trace', 'trace.jsonl', 'cob.json'])).map(l => JSON.parse(l));
      let first = got.length !== expected.length ? { at: 'answers.length', c: got.length, j: expected.length } : null;
      for (let i = 0; !first && i < got.length; i++) { const d = diff(got[i], expected[i], `answer ${i}`); if (d) first = d; }
      cases++;
      if (first) { failed++; console.log(`DIFFER ${c.name}: ${first.at}: C ${show(first.c)}, sim.js ${show(first.j)}`); }
      else if (String(verdict).startsWith('FAIL')) { failed++; console.log(`DIFFER ${c.name}: both alike, but sim.js's claim failed: ${verdict}`); }
      else console.log(`equal  ${c.name}: ${expected.length} answers (${verdict})`);
    });
  }

  /* ---- THE RELEASE SWEEP (simcheck.mjs releaseSweep, wikitest sweep): each live entry's games with Back to now's
     release after every tick up to the script's last input, then played on to the end, the hash and the stop compared
     at each end; in the fork pass's two contexts, the last two shards. sim.js's own verdict, that no release stops a
     game, is simcheck's. ---- */
  [['fixture, own types, type 1', null, 1], ['fixture, shifted, type 0', 'shifted', 0]].forEach(([named, cname, iface], k) => {
    if (SHARDS > 1 && SHARD !== SHARDS - k) return;
    const ctx = { side: 0, iface, casting: cname ? CASTINGS[cname][0] : null, rival: cname ? RIVALS[cname] : null };
    for (const [id, entry] of GAMES.filter(([, e]) => e.live)) {
      fs.writeFileSync(path.join(tmp, 'game.json'), JSON.stringify(entry));
      const cout = lines(wikitest(['sweep', 'wiki.bin', 'game.json', 'fixture', cname ? `casting-${cname}.json` : '-',
        cname ? `rival-${cname}.json` : '-', String(iface), '-']));
      let at = 0, n = 0, first = null;
      for (const demo of entry.demos) {
        const last = Math.max(0, ...demo.game.inputs.map(e => e.at));
        for (let r = 0; r <= last; r++, n++) {
          const sim = WikiSim.game(entry, demo, FIXTURE, ctx, 'refuse');
          while (sim.tick < r && !sim.done) sim.step();
          sim.release();
          while (!sim.done) sim.step();
          const v = sim.view();
          const j = { entry: id, demo: demo.id, r, t: sim.tick, stopped: v.locked !== null || v.game.starved || v.game.badKill || v.stalled,
            h: stateHash(sim, v) };
          sim.free();
          const d = at < cout.length ? diff(JSON.parse(cout[at]), j, '') : { at: '(the C sweep ended early)', c: null, j: null };
          at++;
          if (d && !first) first = { demo: demo.id, r, ...d };
        }
      }
      if (!first && at !== cout.length) first = { demo: '', r: '', at: `the C sweep printed ${cout.length - at} lines more`, c: null, j: null };
      cases++;
      if (first) { failed++; console.log(`DIFFER release sweep -- ${id} (${named}) ${first.demo} released after ${first.r}: ${first.at}: C ${show(first.c)}, sim.js ${show(first.j)}`); }
      else console.log(`equal  release sweep -- ${id} (${named}): ${n} releases, each game's end alike`);
    }
  });

  if (SHARD === 1) {
    /* ---- simcheck.mjs's rule cases, recorded on sim.js and replayed through the C sim ---- */
    const { ENTRY_RULES } = await import('./simcheck.mjs');
    const { create, play } = WikiSim;
    /* a case may hold several sims at once (one made, another made, the first asked): each call names its sim by the
       order it was made in, which wikitest's trace keeps apart */
    let ops, expected, made;
    const record = (make, scripted) => (entry, demo, layout, ctx) => {
      const sim = make(entry, demo, layout, ctx), k = made++;
      // a context's script source is wikitest's COB file, named by `cob: true`
      const c = ctx ? { ...ctx, cob: ctx.cob ? true : undefined, snd: ctx.snd ? true : undefined } : null;
      ops.push(JSON.stringify({ op: 'create', sim: k, scripted, layout: layout ?? null, ctx: c }), JSON.stringify({ ...entry, demos: [demo || {}] }));
      const ask = (op, v) => { ops.push(JSON.stringify({ ...op, sim: k })); expected.push(v); return v; };
      return {
        input(ev) { ops.push(JSON.stringify({ op: 'input', sim: k, ev })); sim.input(ev); },
        step() { ops.push(JSON.stringify({ op: 'step', sim: k })); sim.step(); },
        view: () => {
          const v = ask({ op: 'view' }, sim.view());
          ask({ op: 'hash' }, stateHash(sim, sim.view()));
          return v;
        },
        orders: id => ask({ op: 'orders', id }, sim.orders(id)),
        get done() { return ask({ op: 'done' }, sim.done); },
        get side() { return ask({ op: 'side' }, sim.side); },
        get iface() { return ask({ op: 'iface' }, sim.iface); },
        get tick() { return ask({ op: 'tick' }, sim.tick); },
        get log() { return sim.log; },
      };
    };
    WikiSim.create = record(create, 0);
    WikiSim.play = record(play, 1);
    for (const id of ids) {
      const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
      for (const [what, check] of Object.entries(ENTRY_RULES[id] || {})) {
        ops = []; expected = []; made = 0;
        let verdict;
        try { verdict = check(entry); } catch (e) { verdict = `threw ${e.message}`; }
        fs.writeFileSync(path.join(tmp, 'trace.jsonl'), ops.join('\n') + '\n');
        const got = lines(wikitest(['trace', 'trace.jsonl', 'cob.json'])).map(l => JSON.parse(l));
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
      const W = WikiReplay;
      const j = { TRIP: WikiSim.TRIP, TRIP_DOWN: WikiSim.TRIP_DOWN, TRIP_UP: WikiSim.TRIP_UP, PICK: WikiSim.PICK,
        PICK_DOWN: WikiSim.PICK_DOWN, PICK_UP: WikiSim.PICK_UP, TRIP_ROWS: WikiSim.TRIP_ROWS,
        REPLAY: { CONTROLS: W.CONTROLS, SPEEDS: W.SPEEDS, SPEED0: W.SPEED0, SEEK: W.SEEK, WALK: W.WALK, STEP: W.STEP, NEAR: W.NEAR,
          BOOKS: W.BOOKS, MARKS: W.MARKS, FILL: W.FILL, HUD_DOWN: W.HUD_DOWN, HUD_UP: W.HUD_UP, DEATH: W.DEATH, DEFEAT: W.DEFEAT,
          GAME_END_MAX: W.GAME_END_MAX } };
      const d = diff(c, j, '');
      cases++;
      if (d) { failed++; console.log(`DIFFER the build trip's and the replay's constants: ${d.at}: C ${show(d.c)}, sim.js ${show(d.j)}`); }
      else console.log(`equal  the build trip's and the replay's constants: TRIP ${c.TRIP}, down ${c.TRIP_DOWN}, up ${c.TRIP_UP}, ${c.TRIP_ROWS.length} rows; seek ${c.REPLAY.SEEK}, walk ${c.REPLAY.WALK}, ${c.REPLAY.CONTROLS.length} controls`);
    }

    /* ---- the recorder's rules: tagpu_recrules.h's own functions (wikitest `rules`) against replay.js's twins, and the
       refresh grid against tools/replay/tarec.py's is_refresh, which the file checks use; `want` where a case's answer is
       stated elsewhere: test_tareplay.py test_the_grid's, and the wiki's refresh points (rewind-spec §3.4) ---- */
    {
      const W = WikiReplay, T = W.REFRESH_TICKS;
      const span = (a, b) => Array.from({ length: b - a + 1 }, (_, i) => a + i);
      const grid = (f, iv) => [f, f + 1, ...span(1, Math.floor(2000 / iv)).map(k => k * iv + 1).filter(t => t > f + 1 && t <= 2000)];
      const refresh = [
        ...[[1, 500, 0], [2, 501, 500], [3, 502, 501], [3, 511, 501], [3, 512, 501], [5, 535, 511]].map((c, i) =>
          ({ name: `test_the_grid ${c.join(',')} at 30`, args: [...c, 30], want: [1, 1, 0, 1, 1, 0][i] })),
        { name: 'the third iteration on the grid', args: [3, T + 1, 2, T] },
        { name: 'the third iteration before the grid', args: [3, T, 2, T] },
        { name: 'the iteration after a refresh', args: [4, T + 2, T + 1, T] },
        { name: 'interval 0: the third', args: [3, T + 1, 2, 0] },
        { name: 'interval 0: the second', args: [2, T + 1, T, 0] },
      ];
      const walks = [
        { name: 'test_the_grid\'s walk past gaps at 30', iv: 30, ticks: [1, 2, 3, 30, 31, 32, 60, 62, 91], want: [1, 2, 31, 62, 91] },
        ...[1, 2, 300, 301, 302, 601].flatMap(f => [T, 30, 0].map(iv => ({
          name: `a segment from ${f} to 2000 at ${iv}`, iv, ticks: span(f, 2000),
          want: iv ? grid(f, iv) : [f, f + 1] }))),
      ];
      const evicts = [
        { name: 'fewer than the limit live', rows: [[0, 1, 0], [1, 1, 10], [2, 1, 20]], selected: 2, want: { ok: 1, evict: null } },
        { name: 'the limit live, the least recently used selected', rows: [[0, 1, 0], [1, 1, 10], [2, 1, 20], [3, 1, 30]], selected: 1,
          want: { ok: 1, evict: 2 } },
        { name: 'the limit live, the least recently used not selected', rows: [[0, 1, 0], [1, 1, 10], [2, 1, 20], [3, 1, 30]], selected: 3,
          want: { ok: 1, evict: 1 } },
        { name: 'the limit live with a tie, the earliest wins', rows: [[0, 1, 0], [1, 1, 30], [2, 1, 10], [3, 1, 10]], selected: 1,
          want: { ok: 1, evict: 2 } },
        { name: 'a tie whose earliest is selected', rows: [[0, 1, 0], [1, 1, 10], [2, 1, 10], [3, 1, 30]], selected: 1, want: { ok: 1, evict: 2 } },
        { name: 'more than the limit live: refused', rows: [[0, 1, 0], [1, 1, 10], [2, 1, 20], [3, 1, 30], [4, 1, 40]], selected: 4,
          want: { ok: 0, evict: null } },
        { name: 'only the original and the selected', rows: [[0, 1, 0], [1, 1, 10]], selected: 1, want: { ok: 1, evict: null } },
        { name: 'only the original and the selected at limit 1: no candidate, refused', rows: [[0, 1, 0], [1, 1, 10]], selected: 1, limit: 1,
          want: { ok: 0, evict: null } },
        { name: 'ids above 3 with deleted rows between', rows: [[0, 1, 0], [1, 0, 1], [2, 0, 2], [5, 1, 40], [6, 0, 3], [7, 1, 15], [9, 1, 25]],
          selected: 9, want: { ok: 1, evict: 7 } },
        { name: 'the original selected and the least recently used', rows: [[0, 1, 50], [1, 1, 30], [2, 1, 10], [3, 1, 20]], selected: 0,
          want: { ok: 1, evict: 2 } },
        { name: 'no rows', rows: [], selected: 0, want: { ok: 1, evict: null } },
      ];
      fs.writeFileSync(path.join(tmp, 'rules.txt'), [
        ...refresh.map(c => `refresh ${c.args.join(' ')}`),
        ...walks.map(c => `walk ${c.iv} ${c.ticks.join(' ')}`),
        ...evicts.map(c => `evict ${c.selected} ${c.limit ?? W.FORK_LIMIT}${c.rows.map(r => ` ${r.join(':')}`).join('')}`),
      ].join('\n') + '\n');
      const got = lines(wikitest(['rules', 'rules.txt'])).map(l => JSON.parse(l));
      const py = JSON.parse(run('python3', ['-I', '-c', `
import json, sys
sys.path.insert(0, sys.argv[1])
import tarec
cases, out = json.load(sys.stdin), []
for kind, iv, a in cases:
    if kind == 'refresh':
        out.append(int(tarec.is_refresh(*a, iv)))
        continue
    prev, r = 0, []
    for n, t in enumerate(a, 1):
        if tarec.is_refresh(n, t, prev, iv):
            r.append(t)
            prev = t
    out.append(r)
print(json.dumps({'REFRESH_TICKS': tarec.REC_REFRESH_TICKS, 'TIMELINES': tarec.RECR_TIMELINES, 'out': out}))
`, path.join(ROOT, 'tools/replay')], { input: JSON.stringify([...refresh.map(c => ['refresh', c.args[3], c.args.slice(0, 3)]),
        ...walks.map(c => ['walk', c.iv, c.ticks])]) }));
      const judge = (name, answers) => {
        const first = answers.find(([, v]) => v !== undefined && diff(v, answers[0][1], ''));
        cases++;
        if (first) {
          failed++;
          console.log(`DIFFER rules -- ${name}: ${answers.map(([who, v]) => `${who} ${show(v)}`).join(', ')}`);
        } else console.log(`equal  rules -- ${name}: ${show(answers[0][1])}`);
      };
      judge('the constants', [['C', got[0]], ['replay.js', { REFRESH_TICKS: T, FORK_LIMIT: W.FORK_LIMIT, TIMELINES: W.TIMELINES }]]);
      judge('the constants tarec.py holds', [['C', { REFRESH_TICKS: got[0].REFRESH_TICKS, TIMELINES: got[0].TIMELINES }],
        ['tarec.py', { REFRESH_TICKS: py.REFRESH_TICKS, TIMELINES: py.TIMELINES }]]);
      let at = 1;
      refresh.forEach((c, i) => judge(`isRefresh: ${c.name}`, [['C', got[at++]], ['replay.js', W.isRefresh(...c.args) ? 1 : 0],
        ['tarec.py', py.out[i]], ['wanted', c.want]]));
      walks.forEach((c, i) => {
        const j = [];
        let prev = 0;
        c.ticks.forEach((t, k) => { if (W.isRefresh(k + 1, t, prev, c.iv)) { j.push(t); prev = t; } });
        judge(`the grid: ${c.name}`, [['C', got[at++]], ['replay.js', j], ['tarec.py', py.out[refresh.length + i]], ['wanted', c.want]]);
      });
      evicts.forEach(c => {
        const j = W.forkEvict(c.rows.map(([id, live, used]) => ({ id, live, used })), c.selected, c.limit ?? W.FORK_LIMIT);
        judge(`forkEvict: ${c.name}`, [['C', got[at++]], ['replay.js', { ok: j.ok ? 1 : 0, evict: j.evict }], ['wanted', c.want]]);
      });
    }

    /* ---- refusals: both sides accept or refuse alike, as each case wants ---- */
    {
      const entry = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries/quickbuild.json'), 'utf8'));
      /* the game cases edit a replay entry's first demo: a sidebar-free game whose viewer presses the replay HUD, which
         they turn live themselves (gameRefusalCases, A LIVE ENTRY); a live entry's own data loads alike in the demo
         plays */
      const all = [...refusalCases(entry), ...GAMES.filter(([, game]) => !game.live && !game.takeover).flatMap(([, game]) => gameRefusalCases(game, entry)),
        ...takeoverRefusalCases(PLAYS[PLAYS.length - 1][1], entry)];
      fs.writeFileSync(path.join(tmp, 'refuse.jsonl'), all.map(c =>
        JSON.stringify({ name: c.name, scripted: c.scripted, layout: c.layout, side: c.side, iface: c.iface, casting: c.casting,
                         rival: c.rival ?? null, replay: !!c.replay }) + '\n' +
        JSON.stringify(c.entry) + '\n').join(''));
      const got = lines(wikitest(['refuse', 'refuse.jsonl']));
      all.forEach((c, i) => {
        let js = 'accepted', why = '';
        try {
          // text.js's checkEntry is the loader's refusal of the text, which wiki.js asks before any sim is made
          const bad = WikiText.checkEntry(c.entry);
          if (bad) throw new Error(bad);
          const ctx = { side: c.side, casting: c.casting, iface: c.iface, rival: c.rival ?? null };
          if (c.replay) WikiReplay.play(c.entry, c.entry.demos[0], c.layout, ctx).free();
          else if (c.scripted >= 2) WikiSim.game(c.entry, c.entry.demos[0], c.layout, ctx, c.scripted === 3 ? 'stall' : 'refuse');
          else (c.scripted ? WikiSim.play : WikiSim.create)(c.entry, c.entry.demos[0], c.layout, ctx);
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
  }
} catch (e) {
  if (!(e instanceof CannotRun)) throw e;
  console.error(`simparity: ${e.message}`);
  process.exitCode = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
  if (host) host.close();
}
if (process.exitCode !== 2) {
  const variant = WINE ? 'i686 under Wine' : 'native';
  if (SHARDS > 1) process.stdout.write(`shard ${SHARD} of ${SHARDS}${SHARD > 1 ? ', its demo plays alone' : ''}: `);
  console.log(failed ? `${differ} of ${demos} demo plays, ${failed - differ - bad - refuseBad - dataBad} of ${cases} rule cases, ${refuseBad} of ${refusals} refusal cases, ${dataBad} of ${datas} data read back and ${bad} of ${malformed} malformed-data cases differ (C sim: ${variant})`
    : `all ${demos} demo plays and ${cases} rule cases equal, tick for tick, all ${refusals} refusal cases alike, all ${datas} castings and layouts read alike and all ${malformed} malformed-data cases loaded (C sim: ${variant})`);
  process.exitCode = failed ? 1 : 0;
}
