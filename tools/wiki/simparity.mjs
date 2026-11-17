/* simparity.mjs -- the in-game wiki's parity check: the DLL's simulation (tagpu/ddraw/src/tagpu_wikisim.c over
   tasim_*.c, tagpu_wikidata.c's loader and tagpu_wikicob.c's COB host) in its three builds, play for play and request
   for request, and its replay layer (tagpu_wikireplay.c) against the mockup's replay.js, tick for tick.

   usage: node tools/wiki/simparity.mjs [-j N | --shard K/N] [--record DIR | --against DIR]
     exit 0 when the three builds answer every request alike, the replay layer agrees with replay.js and every
     malformed case loads; 1 when one does not; 2 when a build cannot be made or run.
     -j N runs the N shards of --shard K/N at once, each its own process, once the three builds are made (once:
       tools/wiki/wikibuild.mjs caches them), and gives the worst exit of them; N defaults to a third of the processors,
       a shard keeping three hosts busy. --shard K/N runs the work items dealt to shard K alone.
   usage: node tools/wiki/simparity.mjs --hash [--layout FILE|null] [--pack DIR] [--side N] [--iface 0|1] [--scripts]
                                            [--frame ENTRY/DEMO@TICK[,...]]
     prints `<entry>/<demo> <hash>` for every demo the tree names: the host's state hash on its last tick
     (tagpu_wikisim_hash), 16 hex digits (a game entry's demo: replay.js's replay played to its end, and the replay's
     hash, tagpu_wikireplay_hash's), the number the game's check must give for the same demo played to its end with the
     same sidebar layout (by default art/wiki/side/fixture.json; `null` for none) in the same context: side N (0 by
     default), Interface Type 0 or 1 (1 by default), and with --pack the casting of side N in DIR's side.json, whose
     layout is then the default (tools/wiki/sidepacks.py writes the packs), and with --scripts the units' scripts and
     sounds, as the game's check plays when its log says they are on: each type's from the first pack that holds it,
     DIR's stage packs first, then the gfx packs under tools/wiki/mockup/gfx/. A demo the host refuses prints `refused`
     and why. The `host` build alone.
   usage: node tools/wiki/simparity.mjs --try DUMP
     prints the host's state hash of Try it yourself fed the inputs the game's state dump logged (tools/wiki_regression.py
     try), the number the dump's own hash must equal. The layout is the dump's `layout` when it records one, else the
     fixture, and the context its `ctx` (side, Interface Type, the casting written whole, and with `cob` the units'
     scripts and sounds from the gfx packs), else the host's default. The `host` build alone.

   THE THREE BUILDS (tools/wiki/wikibuild.mjs): `host`, gcc -O2, the mockup's; `asan`, AddressSanitizer and
   UndefinedBehaviorSanitizer; `wine`, the DLL's own compiler and SSE2 flags (i686-w64-mingw32-gcc) under Wine
   (the prefix tools/wiki/wineprefix.py gives, never the template's), the arithmetic the game runs. Every play is made through the drop-in
   (tools/wiki/mockup/simhost.js) on the tee of the three (tools/wiki/hostchan.cjs `tee`): each request goes to all
   three, the host's answer is the one read, and any build whose answer differs in its text is recorded with the first
   differing path, so a play differs where any request of it did. Each play reads the view, every unit's orders and
   the state hash on every tick -- one request a tick, the drop-in's echo -- and the map when the sim is made and again
   on its last tick, which nothing between them writes. The C replay layer's lines (wikitest `replay`, `replaytrace`)
   and the recorder's rules and constants (`rules`, `constants`) are printed by each build, which must print them
   alike, and compared with replay.js; `malformed` runs on `asan` and `wine`.

   WHAT IS PLAYED. Every demo of every entry the tree names, with the sidebar of art/wiki/side/fixture.json (the
   layout the wiki falls back to), with fixture-tall.json's 768-row column and with none, each in simcheck.mjs's
   contexts: the entry's own types and each casting of tools/wiki/casting-fixture.json (identity, shifted, unbound),
   each at Interface Type 1 and 0, and each again with the units' scripts and sounds when the gfx packs under
   tools/wiki/mockup/gfx/ (gitignored, built by tools/wiki/gfx.py) hold any. THE GAME PASS: in each context every
   game entry under art/wiki/entries/ (frame "game", named by the tree or not) and the players and takeover fixtures,
   each distinct game script once (demos sharing one play the same game), with the fixture casting's rival beside it
   (casting-fixture.json `rivals`), and THE STALL CASE: every seat cut to 200, in stall mode and in refuse mode, where
   it must end starved; with the identity casting, every cast's economy outside a game's bounds too, which must stop
   it under the badge. THE REPLAY PASS: every demo of each as a replay through replay.js on the drop-in, demo tick by
   demo tick against wikitest `replay` -- the replay's view and hash (replayHash, tagpu_wikireplay_hash's twin, a live
   entry's rewind state and P's hash included), the game's view, orders and hash -- and Try it yourself's press trace
   through replay.js's create against `replaytrace`: TRACE on a replay entry, LIVE_TRACE (the live HUD's presses and
   the world's input) on a live one, PLAYERS_TRACE on a players one, TAKEOVER_TRACES on a takeover one. THE FORK,
   COLUMN AND SEAT CASES (forkCases, columnCases, seatCases), whose claims are each case's verdict. THE RELEASE SWEEP:
   each live entry's games with Back to now's release after every tick up to the script's last input, each to its
   end, in two contexts. simcheck.mjs's RULE CASES, the rules no demo shows. The build trip's, the replay layer's and
   the players entry's constants; the recorder's rules (tagpu_recrules.h, which the wiki and the recorder share)
   against replay.js's twins, the refresh grid against tools/replay/tarec.py's is_refresh too. THE REFUSAL CASES:
   data, castings, contexts and layouts each case wants accepted or refused -- the demo's `sidebar`, the build trip's
   data rule, the text's tokens, the actions' data rule at each interface type, a unit's G cells, every bound of the
   casting and of the layout on both sides of it, a scene its bound footprints do not fit, the context's interface type
   and side, every data rule of a game entry (its frame, seats, rival and corpse units, its demos' games and their
   kills, and their viewer's scripts, made into replays) and a text entry's (the loader alone, tagpu_wikidata_entry's
   words) -- each held to its want, text.js's checkEntry refusing only what the loader refuses. THE DATA READ: every
   casting of tools/wiki/casting-fixture.json and of every side pack present under tools/wiki/mockup/gfx/side/
   (tools/wiki/sidepacks.py), and every layout of art/wiki/side/ and of those packs, read by each build's loader
   alike. Last, wikitest's `malformed` cases: data the loader must survive, each loaded as stated.

   THE WORK ITEMS. A run is a list of items, each named by a key: one context's demos, one game entry's game pass or
   replay pass in one context, one fork, column or seat case, one live entry's sweep in one context, one entry's rule
   cases, and the constants, the recorder's rules, the refusals, the data read and the malformed cases as one item
   each. The shards share them longest first (the longest-processing-time rule: each to the shard with the least
   cost dealt so far) by the seconds each took in the last full run, build/simparity-costs.json, which every full -j
   run rewrites; with no such file they are dealt by index round robin. Every item runs in every full run: the costs
   balance the shards and never drop a context, a play or a build.

   THE RECORD (--record DIR) keeps the state hash of every tick of every play, the host's once the three builds
   answered alike, so that a later tree can be held to it (--against DIR). Each shard writes DIR/shard-K-of-N.jsonl.gz:
   a header line {"header": {"commit", "gfx": the digest of every scene.json and .cob under tools/wiki/mockup/gfx/,
   "contexts": the demo pass's context labels}}, then one line a play, {"key", "h": its hashes} or {"key", "refused":
   true}. A key is `pass|context|entry/demo|variant`:
     demos|CTX|E/D|play          the demo, each tick's tagpu_wikisim_hash from tick 0
     game|CTX|E/D|game, stall, starved, econoff     the game pass's four plays
     replay|CTX|E/D|replay       each demo tick's [tagpu_wikireplay_hash, the game's hash or null]
     replay|CTX|E/D|trace, playerstrace, livetrace, tktrace1, tktrace2     the press traces, likewise
     sweep|CTX|E/D|sweep R       the release sweep's game released after R, its hash at its end
     case|-|-|NAME               a fork, column or seat case: its hash after each view, in call order
     rule|-|E|WHAT               a simcheck rule case: its hash after each view, in call order
   A run that differs anywhere writes nothing. --against DIR also holds every key it makes to the record: each must be
   there with the same hashes (the first differing index named), and under -j every recorded key must be made. A record
   of other gfx packs is refused before anything plays.

   What a run costs: THE COST, below. */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import crypto from 'node:crypto';
import { spawn, spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { scripts, sounds } from './gfxpacks.mjs';
import { takeoverFixture } from './takeover-fixture.mjs';
import { playersFixture, playersScratch } from './players-fixture.mjs';
import { build as hostBuild, wineEnv } from './wikibuild.mjs';

const require = createRequire(import.meta.url);
const ROOT = new URL('../../', import.meta.url).pathname;
const SIDE = name => JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/side/${name}.json`), 'utf8'));
const FIXTURE = SIDE('fixture');
const argOf = k => { const i = process.argv.indexOf(k); return i >= 0 ? process.argv[i + 1] : undefined; };
const RECORD = argOf('--record'), AGAINST = argOf('--against');
const TARGETS = ['host', 'asan', 'wine'];
// the parity's plays go to the three builds; --hash and --try are the game's comparisons, the host's answer alone
const PARITY = !process.argv.includes('--hash') && !process.argv.includes('--try');
if (PARITY) process.env.WIKI_HOST_TEE = TARGETS.join(',');
/* the `wine` build's prefix (wikibuild.mjs wineEnv), settled before the first request reaches the tee, which a
   module's load already sends: one it refuses is a check that cannot run. Put in this process's environment, so
   -j's shards check it and none makes one. */
if (PARITY) {
  try { process.env.WINEPREFIX = wineEnv().WINEPREFIX; } catch (e) { console.error(`simparity: ${e.message}`); process.exit(2); }
}
// a record's shard files, and what a shard hands its parent (-j, below): the keys it made and each item's seconds
const shardFile = (dir, k, n) => path.join(dir, `shard-${k}-of-${n}.jsonl.gz`);
const KEYS_OUT = process.env.SIMPARITY_KEYS, COSTS_OUT = process.env.SIMPARITY_COSTS_OUT;
const COSTS = path.join(ROOT, 'build/simparity-costs.json');

class CannotRun extends Error {}
function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { maxBuffer: 1 << 30, encoding: 'utf8', ...opts });
  if (r.error || r.status !== 0)
    throw new CannotRun(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ` (${r.status})`}\n${r.stderr || ''}`);
  return r.stdout;
}

/* THE GFX DIGEST: what the record's hashes rest on besides the tree, every scene.json and script the packs hold */
function gfxDigest() {
  const dir = path.join(ROOT, 'tools/wiki/mockup/gfx'), lines = [];
  const walk = d => {
    for (const f of fs.readdirSync(d, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))) {
      const p = path.join(d, f.name);
      if (f.isDirectory()) walk(p);
      else if (f.name === 'scene.json' || f.name.endsWith('.cob'))
        lines.push(`${path.relative(dir, p)} ${crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex')}`);
    }
  };
  if (fs.existsSync(dir)) walk(dir);
  return crypto.createHash('sha256').update(lines.join('\n')).digest('hex');
}

/* a record's files read whole: {header, plays: Map key -> hashes or 'refused'} */
function readRecord(dir) {
  const files = fs.existsSync(dir) ? fs.readdirSync(dir).filter(f => /^shard-\d+-of-\d+\.jsonl\.gz$/.test(f)).sort() : [];
  if (!files.length) throw new Error(`${dir} holds no record`);
  let header = null;
  const plays = new Map();
  for (const f of files)
    for (const l of zlib.gunzipSync(fs.readFileSync(path.join(dir, f))).toString('utf8').split('\n')) {
      if (!l) continue;
      const o = JSON.parse(l);
      if (o.header) {
        if (header && header.gfx !== o.header.gfx) throw new Error(`${dir}: ${f} was recorded on other gfx packs than its siblings`);
        header ??= o.header;
      } else plays.set(o.key, o.refused ? 'refused' : o.h);
    }
  return { header, plays };
}

/* the plays `got` (key -> hashes) held to the record's: each problem a line; with `whole`, a recorded key not made too */
function holdTo(record, got, whole) {
  const out = [];
  const show1 = v => JSON.stringify(v);
  for (const [k, h] of got) {
    if (!record.plays.has(k)) { out.push(`${k}: not in the record`); continue; }
    const r = record.plays.get(k);
    if (r === 'refused' || h === 'refused') { if (r !== h) out.push(`${k}: the record ${r === 'refused' ? 'refused it' : 'played it'}, this run ${h === 'refused' ? 'refused it' : 'played it'}`); continue; }
    const n = Math.min(r.length, h.length);
    let i = 0;
    while (i < n && JSON.stringify(r[i]) === JSON.stringify(h[i])) i++;
    if (i < n) out.push(`${k}: first differs at [${i}]: the record ${show1(r[i])}, this run ${show1(h[i])}`);
    else if (r.length !== h.length) out.push(`${k}: the record holds ${r.length} hashes, this run ${h.length}`);
  }
  if (whole) for (const k of record.plays.keys()) if (!got.has(k)) out.push(`${k}: recorded, not made by this run`);
  return out;
}

/* a record's header, from its first shard file alone */
function recordHeader(dir) {
  const files = fs.existsSync(dir) ? fs.readdirSync(dir).filter(f => /^shard-\d+-of-\d+\.jsonl\.gz$/.test(f)).sort() : [];
  if (!files.length) return null;
  const l = zlib.gunzipSync(fs.readFileSync(path.join(dir, files[0]))).toString('utf8').split('\n', 1)[0];
  return JSON.parse(l).header ?? null;
}
const GFX_DIGEST = RECORD || AGAINST ? gfxDigest() : null;
if (AGAINST) {
  const h = recordHeader(AGAINST);
  if (!h) { console.error(`simparity: ${AGAINST} holds no record`); process.exit(2); }
  if (h.gfx !== GFX_DIGEST) {
    console.error(`simparity: ${AGAINST} was recorded on other gfx packs (${h.gfx.slice(0, 12)}, these ${GFX_DIGEST.slice(0, 12)}): the record is void`);
    process.exit(2);
  }
}
if (RECORD) fs.mkdirSync(RECORD, { recursive: true });

/* -j N: the shards of --shard K/N at once, each its own process, once the three builds are made; their outputs in
   shard order as each ends, and the worst exit (2 over 1 over 0). The parent reads the costs once and hands every shard
   the same table (SIMPARITY_DEAL), so all deal the items alike whatever writes the cost file meanwhile; each shard hands
   back its items' seconds (SIMPARITY_COSTS_OUT), which become the cost file once every shard has run. With --against
   each shard writes the plays it made (SIMPARITY_KEYS), and the whole run is held to the record here, so a recorded
   key no shard made fails it.

   THE COST (MEASURED 2026-11-11, the reference setup, 32 threads): -j 10, the three builds at once, 5928 plays, 509
   cases, 955 refusal cases, 27 castings and layouts and 21 malformed-data cases: 990 s wall, under a load average of
   50 to 63 from other work, against the record of every play's hashes. */
if (PARITY && !process.argv.includes('--shard')) {
  const N = argOf('-j') === undefined ? Math.max(1, Math.floor(os.availableParallelism() / 3)) : Number(argOf('-j'));
  if (!(Number.isInteger(N) && N >= 1)) { console.error(`simparity: -j ${argOf('-j')} is not a count`); process.exit(2); }
  const base = [];
  for (let i = 2; i < process.argv.length; i++) { if (process.argv[i] === '-j') i++; else base.push(process.argv[i]); }
  try { for (const t of TARGETS) hostBuild(t); } catch (e) { console.error(`simparity: ${e.message}`); process.exit(2); }
  const side = fs.mkdtempSync(path.join(os.tmpdir(), 'simparity-run-'));
  let costs = null;
  try { costs = JSON.parse(fs.readFileSync(COSTS, 'utf8')); } catch (e) { costs = null; }
  const deal = path.join(side, 'deal.json');
  fs.writeFileSync(deal, JSON.stringify(costs));
  // a record is one run's: an earlier run's shards, of another N, would mix into it
  if (RECORD) for (const f of fs.readdirSync(RECORD)) if (/^shard-\d+-of-\d+\.jsonl\.gz$/.test(f)) fs.rmSync(path.join(RECORD, f));
  const t0 = Date.now();
  const outs = new Array(N), codes = new Array(N);
  await Promise.all(Array.from({ length: N }, (_, i) => new Promise(done => {
    const env = { ...process.env, SIMPARITY_DEAL: deal, SIMPARITY_COSTS_OUT: path.join(side, `costs-${i + 1}.json`),
      ...(AGAINST ? { SIMPARITY_KEYS: path.join(side, `keys-${i + 1}.jsonl.gz`) } : {}) };
    const p = spawn(process.execPath, [new URL(import.meta.url).pathname, ...base, '--shard', `${i + 1}/${N}`], { env, stdio: ['ignore', 'pipe', 'pipe'] });
    let out = '';
    p.stdout.on('data', d => { out += d; });
    p.stderr.on('data', d => { out += d; });
    p.on('close', code => { outs[i] = out; codes[i] = code ?? 2; done(); });
  })));
  outs.forEach(o => process.stdout.write(o));
  let code = codes.includes(2) ? 2 : codes.includes(1) ? 1 : 0;
  if (AGAINST && code !== 2) {
    const got = new Map();
    for (let i = 1; i <= N; i++) {
      const f = path.join(side, `keys-${i}.jsonl.gz`);
      if (!fs.existsSync(f)) continue;
      for (const l of zlib.gunzipSync(fs.readFileSync(f)).toString('utf8').split('\n')) if (l) { const o = JSON.parse(l); got.set(o.key, o.refused ? 'refused' : o.h); }
    }
    const bad = holdTo(readRecord(AGAINST), got, true);
    for (const l of bad.slice(0, 50)) console.log(`DIFFER against -- ${l}`);
    if (bad.length > 50) console.log(`DIFFER against -- and ${bad.length - 50} more`);
    console.log(bad.length ? `simparity against ${AGAINST}: ${bad.length} of the record's plays differ or are missing`
      : `simparity against ${AGAINST}: all ${got.size} plays equal to the record, every recorded key made`);
    if (bad.length && code === 0) code = 1;
  }
  // THE COSTS of this run, once every shard ran all its items: the next run's deal
  if (code !== 2) {
    const all = {};
    for (let i = 1; i <= N; i++) Object.assign(all, JSON.parse(fs.readFileSync(path.join(side, `costs-${i}.json`), 'utf8')));
    fs.mkdirSync(path.dirname(COSTS), { recursive: true });
    const tmpCosts = `${COSTS}.${process.pid}`;
    fs.writeFileSync(tmpCosts, JSON.stringify(all, null, 0));
    fs.renameSync(tmpCosts, COSTS);
  }
  if (RECORD && code !== 0) for (let i = 1; i <= N; i++) fs.rmSync(shardFile(RECORD, i, N), { force: true });
  fs.rmSync(side, { recursive: true, force: true });
  console.log(`simparity -j ${N}: ${codes.filter(c => c === 0).length} of ${N} shards passed, ${((Date.now() - t0) / 1000).toFixed(0)} s wall`);
  process.exit(code);
}

const SHARD_ARG = process.argv.includes('--shard') ? process.argv[process.argv.indexOf('--shard') + 1] : '1/1';
const [SHARD, SHARDS] = (/^(\d+)\/(\d+)$/.exec(SHARD_ARG) || []).slice(1).map(Number);
if (!(SHARDS >= 1 && SHARD >= 1 && SHARD <= SHARDS)) {
  console.error(`simparity: --shard ${SHARD_ARG} is not K/N with 1 <= K <= N`);
  process.exit(2);
}

/* the simulation through the drop-in, replay.js and frame.js over it, and the tee's record of every request whose
   builds answered apart */
const WikiSim = require('./mockup/simhost.js');
const TEE = require('./hostchan.cjs');
const WikiReplay = require('./mockup/replay.js');
const WikiFrame = require('./mockup/frame.js');
const WikiText = require('./mockup/text.js');

/* THE PLAYS THIS SHARD MADE, for the record (THE RECORD above): key -> hashes, or 'refused'; a key made twice is a
   scheme that names two plays alike, which would hide one of them */
const PLAYED = new Map();
let dupKeys = 0;
function rec(key, h) {
  if (PLAYED.has(key)) { dupKeys++; console.log(`DIFFER record -- the key ${key} is made twice`); }
  PLAYED.set(key, h);
}

/* ---- 64-bit FNV-1a, its halves in two uint32s so that no BigInt is needed: the replay's hash ---- */
const utf8 = new TextEncoder();
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
   state hash (the host's) as its low and its high 32 bits; then a live replay's block, the past simulation's state hash
   last. A trip from the game is WIKI_REPLAY_AT_GAME, -2. */
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
  const gh = R.game ? R.game.hash() : '0000000000000000';
  h.int(parseInt(gh.slice(8), 16)); h.int(parseInt(gh.slice(0, 8), 16));
  if (v.live) liveHash(R, v, h);
  if (v.players) playersHash(R, v.players, h);
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
// the hash's players block, tagpu_wikireplay.h's order
function playersHash(R, P, h) {
  const b = x => h.int(x ? 1 : 0);
  h.int(P.cam.x); h.int(P.cam.z); h.int(P.follow); h.int(P.jumpReq); h.int(P.sight); h.int(P.sightReq); h.int(P.tog); h.int(P.folded);
  b(P.stat.open); h.int(P.stat.measure); b(P.stat.team); h.int(P.clickFrom); h.int(P.shown); b(P.pend);
  h.int(f32bits(P.bar.be)); h.int(f32bits(P.bar.bm)); for (const x of P.bar.rates) h.int(f32bits(x)); for (const x of P.bar.dt) h.int(x);
  h.int(P.pass.at); h.int(P.pass.blocks);
  for (let i = 0; i < P.pass.fog.length; i += 2) h.byte(parseInt(P.pass.fog.slice(i, i + 2), 16));
  h.int(P.pass.contacts.length); for (const id of P.pass.contacts) h.str(id);
  h.int(P.npt); h.int(P.base); h.int(P.period);
  for (let n = 0; n < P.npt; n++) for (let k = 0; k < P.nseat; k++) for (let m = 0; m < WikiReplay.MEASURES.length; m++) h.int(f32bits(R.stat(n, k, m)));
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
  const ph = R.past ? R.past.hash() : '0000000000000000';
  h.int(parseInt(ph.slice(8), 16)); h.int(parseInt(ph.slice(0, 8), 16));
}

/* A PLAYERS GAME'S SIGHT at the tick the sim stands at, as tests/wikihost.c's wh_sight_out prints it: per seat of the game,
   its lit grid (the host's sightLit over the layout's table S) as its FNV-1a 32 and its lit blocks' count, and its
   contacts (sightContacts) as a string of 0 and 1 in the view's unit order. The view `v` must hold its map. */
function sightOut(sim, v, S) {
  return v.game.seats.map((_, k) => {
    const lit = WikiSim.sightLit(v, S, k, sim.tick);
    let h = 0x811c9dc5, n = 0;
    for (const b of lit) { h = Math.imul(h ^ b, 0x01000193) >>> 0; n += b !== 0 ? 1 : 0; }
    return { lit: h.toString(16).padStart(8, '0'), n, c: WikiSim.sightContacts(v, lit, k).join('') };
  });
}

/* THE UNITS' SCRIPTS AND SOUNDS: every gfx pack tools/wiki/gfx.py built under tools/wiki/mockup/gfx/ (gitignored, read
   from the game), each type's from the first that holds it. The host reads the packs themselves (ctx.packs); the C
   replay layer's runs read the same facts from one cob.json (tests/wikicobfile.h cob_load). Without a pack the demos
   play without them only. The game's sim has both from its one 3D load or neither (tagpu_wiki.c scripts_now). */
const GFX = path.join(ROOT, 'tools/wiki/mockup/gfx');
const PACKS = fs.existsSync(GFX) ? fs.readdirSync(GFX).sort().map(d => path.join(GFX, d)).filter(d => fs.existsSync(path.join(d, 'scene.json'))) : [];
const SCRIPTS = scripts(PACKS);
const SOUNDS = sounds(PACKS);
const COBS = Object.keys(SCRIPTS).length ? [false, true] : [false];
const PACK_DIRS = PACKS.map(d => path.relative(ROOT, d));

/* the first difference between the C replay layer's answer `c` and replay.js's `j`, as a path and the two values; null
   when equal */
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
const show = v => { const s = JSON.stringify(v) ?? String(v); return s.length > 160 ? `${s.slice(0, 157)}...` : s; };
const lines = out => out.split(/\r?\n/).filter(l => l.length);
/* A FRAME VIEW (frame.js view) in the shape of the game's state dump of a players frame (tagpu_wiki.c view_json): the eye
   and the viewport, the cursors, the clicks with their ages, the units drawn with their overlay selection (owner 255
   for none, as WIKISTAGEUNIT holds it), the map's fog a digit a cell, and under Sight the minimap's pass, its grid's bits
   a digit a block and its dots truncated toward zero as the minimap takes them */
const frameView = v => ({
  screen: v.eye ? 1 : 0, eye: v.eye ? [v.eye.x, v.eye.z, v.eye.w, v.eye.h] : [0, 0, 0, 0],
  ocur: (v.ocur || []).map(c => [c.x, c.z, c.seat]), oclick: (v.oclick || []).map(c => [c.x, c.z, c.seat, c.age]),
  units: v.units.map(u => [u.x, u.z, Number.isInteger(u.owner) && u.owner >= 0 ? u.owner : 255, u.osel ? 1 : 0]),
  fog: Array.from(v.map.fog, f => Math.min(9, f)).join(''),
  mini: v.mini ? { gw: v.mini.gw, gh: v.mini.gh, lit: Array.from(v.mini.lit).join(''),
    dots: v.mini.dots.map(d => [Math.trunc(d.x), Math.trunc(d.z), d.owner]) } : null,
});

if (process.argv.includes('--hash')) {
  const arg = k => { const i = process.argv.indexOf(k); return i >= 0 ? process.argv[i + 1] : undefined; };
  const file = arg('--layout'), dir = arg('--pack');
  const pack = dir ? JSON.parse(fs.readFileSync(path.join(dir, 'side.json'), 'utf8')) : null;
  const layout = !file ? (pack ? pack.layout : FIXTURE) : file === 'null' ? null : JSON.parse(fs.readFileSync(file, 'utf8'));
  const side = Number(arg('--side') ?? 0), iface = Number(arg('--iface') ?? 1);
  const stages = dir && fs.existsSync(path.join(dir, 'stage')) ? fs.readdirSync(path.join(dir, 'stage')).sort().map(d => path.join(dir, 'stage', d)) : [];
  // the opponent's casting: the first layout side other than the player's, as the DLL's context gives it
  const rival = pack ? pack.casting[pack.layout.sides.findIndex((x, j) => j !== side)] ?? null : null;
  const ctx = { side, casting: pack ? pack.casting[side] ?? null : null, iface, rival,
    packs: process.argv.includes('--scripts') ? [...stages, ...PACKS].map(d => path.relative(ROOT, path.resolve(d))) : null };
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
          // game, as the human seat sees it (seat 0 but in a takeover's live game), with the top bar of that seat's
          // side (under a players replay's Sight the viewed seat's, frame.js topSide). `pending`: a seek is under way, when the page holds the last world drawn and a record at this tick
          // depends on where the frames fell, so the lane compares none there
          const rv = R.view(), shown = rv.live && rv.live.rw === 'past' ? R.past : R.game;
          const human = rv.live ? rv.live.human : 0;
          const pl = rv.players ? { seats: WikiFrame.plSeats(entry, demo), stat: R.stat } : null;
          const gv = shown ? shown.view(human) : null;
          const rec = WikiFrame.record(rv, gv, side, false, null, WikiFrame.topSide(rv, layout, side), pl);
          // a players frame's view, in the shape of the game's state dump (tagpu_wiki.c view_json): what the players lane
          // compares the record's WIKISTAGEVIEW with
          const mode = !rv.live ? 'watch' : rv.live.rw === 'past' ? 'past' : 'player';
          const view = rv.players && gv ? frameView(WikiFrame.view(gv, layout, side, mode, !!rec.simcur, human,
            { tick: shown.tick, clickFrom: rv.clickFrom, players: rv.players, screen: entry.screen })) : undefined;
          console.log(`frame ${id}/${demo.id}@${rv.tick} ${replayHash(R)} ${JSON.stringify({ ...rec, pending: rv.game.target >= 0, view })}`);
        }
        while (!R.view().done) R.step();
        console.log(`${id}/${demo.id} ${replayHash(R)}`);
        R.free();
        continue;
      }
      let sim;
      try { sim = WikiSim.play(entry, demo, layout, ctx); } catch (e) { console.log(`${id}/${demo.id} refused ${e.message}`); continue; }
      while (!sim.done) sim.steps(4096);
      console.log(`${id}/${demo.id} ${sim.hash()}`);
    }
  }
  // the last line: a reader that does not see it has not been given the whole answer
  console.log('end');
  process.exit(0);
}

/* --try FILE: Try it yourself as the game played it. FILE is the wiki's state dump (tagpu_wiki.json): the entry, the
   demo whose scene Try it took, the tick, and `try_log`, every input the game thread gave its simulation with the
   tick it was given at. The host's create() on the same scene is fed the same inputs at the same ticks and stepped to
   the dump's tick; its state hash is printed, the number the dump's `hash` must equal. */
const tryAt = process.argv.indexOf('--try');
if (tryAt >= 0) {
  const dump = JSON.parse(fs.readFileSync(process.argv[tryAt + 1], 'utf8'));
  const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${dump.entry}.json`), 'utf8'));
  const demo = entry.demos.find(d => d.id === dump.demo);
  if (!demo) { console.error(`simparity --try: no demo ${dump.entry}/${dump.demo}`); process.exit(2); }
  const ctx = dump.ctx ? { ...dump.ctx, packs: dump.ctx.cob ? PACK_DIRS : null } : undefined;
  const sim = WikiSim.create(entry, demo, 'layout' in dump ? dump.layout : FIXTURE, ctx);
  for (const [t, kind, a, b] of dump.try_log) {
    while (sim.tick < t && !sim.done) sim.steps(Math.min(4096, t - sim.tick));
    sim.input(kind === 'move' ? { move: [a, b] } : { [kind]: a });
  }
  while (sim.tick < dump.tick && !sim.done) sim.steps(Math.min(4096, dump.tick - sim.tick));
  console.log(sim.hash());
  process.exit(0);
}

/* ---- the refusal cases: each a scripted flag, a layout, a context (side, interface type and casting) and an entry
   whose first demo the sim is made from; every build must accept it or refuse it alike, and as `want`
   says. ---- */
const CASTINGS = JSON.parse(fs.readFileSync(path.join(ROOT, 'tools/wiki/casting-fixture.json'), 'utf8')).castings;
// the opponent's casting beside each fixture casting: the fixture layout has one side, so the rival is given apart
const RIVALS = JSON.parse(fs.readFileSync(path.join(ROOT, 'tools/wiki/casting-fixture.json'), 'utf8')).rivals;
// every game entry, the tree's or not (frame "game"), in file order
const GAMES = fs.readdirSync(path.join(ROOT, 'art/wiki/entries')).filter(f => f.endsWith('.json')).sort()
  .map(f => [f.slice(0, -5), JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries', f), 'utf8'))])
  .filter(([, e]) => e.frame === 'game');
/* THE DEMO PASS'S CONTEXTS, in the order the shards deal them: the units' scripts off and on, each sidebar layout, each
   casting of casting-fixture.json or none, each Interface Type */
const CONTEXTS = COBS.flatMap(cob => ['fixture', 'fixture-tall', null].flatMap(side => [null, ...Object.keys(CASTINGS)].flatMap(cname =>
  [1, 0].map(iface => ({ cob, side, cname, iface, named: `${side || 'no sidebar'}, ${cname || 'own types'}, type ${iface}${cob ? ', scripts' : ''}` })))));
// the games the game pass and the replay pass play: the entries and the takeover fixture
const PLAYS = [...GAMES, ['players-fixture', playersFixture()], ['players-scratch', playersScratch()], ['takeover-fixture', takeoverFixture()]];
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
/* THE PLAYERS PRESS TRACE, Try it yourself on a players entry's first demo (players-spec.md section 10): presses during
   the walk (nothing); Jump, Follow, Follow again (Free cam) and Follow back on; Sight on Bram, again (the full map) and
   on Bram again; each switch, and Cursors back on; the stats window, a measure, the team split (nothing where the
   teams are not told apart, as in the fixtures, whose seats are each a team of one) and the player split;
   a team row folded, another folded and unfolded; a seek forward with Sight on Ada asked while it is pending (applied
   at its end); the pause, Sight changes while paused (the bar's re-read), a seek back while paused; Jump on Bram (a
   toast where his camera was not recorded); a seat, a chip and a fold outside the door (nothing); close; Leave with a
   Sight press it ignores, then Yes */
const PLAYERS_TRACE = { end: 230, presses: [[5, 'jump', 0], [40, 'jump', 0], [45, 'follow', 0], [50, 'follow', 0], [55, 'follow', 0],
  [60, 'sight', 1], [70, 'sight', 1], [75, 'sight', 1], [80, 'cursors', 0], [81, 'clicks', 0], [82, 'selections', 0], [83, 'chat', 0],
  [84, 'cursors', 0], [90, 'stats', 0], [95, 'chip', 5], [96, 'chip', 1], [97, 'chip', 0], [100, 'fold', 0], [101, 'fold', 1],
  [102, 'fold', 1], [110, 'track', 1180], [111, 'sight', 0], [140, 'play', 0], [145, 'sight', 1], [150, 'sight', 1],
  [155, 'sight', 1], [160, 'rew', 0], [180, 'jump', 1], [185, 'jump', 9], [186, 'chip', 99], [187, 'fold', 7], [190, 'close', 0],
  [200, 'leave', 0], [201, 'sight', 0], [205, 'leave:yes', 0]] };
/* THE LIVE PRESS TRACE, Try it yourself on a live entry's first demo (rewind-spec.md 12.1): the presses as TRACE's
   and the world inputs as [demo tick, 'move', x, z], [demo tick, 'down' | 'up', key], each given through R.input
   after that tick's frame. In order: the rewind refused (an empty span) and a replay HUD's play (nothing); a move
   before any fork (the world takes none); inside the opening's first build trip, the open, rew, a track press
   forward and Play from here on the same tick, while that seek is pending (ignored), fwd near now (Back to now, its
   release logged on the original: it drops the rest of the trip, so the solar is never armed, and the script's next
   build selects its own builder on its tick, tagpu_wikidata.c THE LIVE DATA RULE); the open, a track press, Play from here
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
  // the binding (tagpu_wikisim.c THE BOUND ENTRY): every unit one cast, no cast twice, no two units bound to one type
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
  // the scenes (tagpu_wikidata_entry): a demo on the one its `on` names, the first by default, every map the first one's size
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
  // a unit's G cells, tagpu_wikidata.c's G-cell rows
  text('a G cell inside the footprint', 'accepted', e => { e.units.ARMGEO.g = [[0, 0], [3, 3]]; });
  text('a G cell off the footprint', 'refused', e => { e.units.ARMGEO.g = [[4, 0]]; });
  text('no G cell with vent', 'refused', e => { e.units.ARMGEO.g = []; });
  text('a G cell without vent', 'refused', e => { e.units.ARMMEX.g = [[0, 0]]; });
  text('no G cell without vent', 'accepted', e => { e.units.ARMMEX.g = []; });
  text('a G cell not two integers', 'refused', e => { e.units.ARMGEO.g = [[0.5, 0]]; });
  // the reclaim entry's data (tagpu_wikidata.c scene_feat, the scene's features and kind, the reclaim and pick inputs and trips)
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
  // the casting's reader, tagpu_wikidata_casting; each edit leaves the entry's own types bound to themselves
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
  // THE SIGHT TABLE (tagpu_wikidata.h WIKISIGHT): sparse by index, every bound both ways
  const sightT = L => L.sight.tables;
  // n distinct cells in (dr, dc) order, within the reach
  const cellsN = n => Array.from({ length: n }, (_, i) => [i % 127 - 63, Math.floor(i / 127) - 63, 1]);
  lay('no sight table', 'accepted', L => { delete L.sight; });
  lay('a sight table null', 'accepted', L => { L.sight = null; });
  lay('a sight table storing none', 'accepted', L => { L.sight.tables = []; });
  for (const [n, want] of [[1, 'refused'], [2, 'accepted'], [32767, 'accepted'], [32768, 'refused'], [2.5, 'refused'], ['9', 'refused']])
    lay(`numtables ${JSON.stringify(n)}`, want, L => { L.sight = { numtables: n, tables: [{ idx: 1, cells: [[1, 0, 1]] }] }; });
  lay('numtables absent', 'refused', L => { delete L.sight.numtables; });
  lay('tables absent', 'refused', L => { delete L.sight.tables; });
  for (const [n, want] of [[32, 'accepted'], [33, 'refused']])
    lay(`${n} tables stored`, want, L => { L.sight = { numtables: 100, tables: Array.from({ length: n }, (_, i) => ({ idx: i + 1, cells: [[1, 0, 1]] })) }; });
  lay('a table at idx 0', 'refused', L => { sightT(L)[0].idx = 0; });
  lay('numtables 8 under a table at 8', 'refused', L => { L.sight.numtables = 8; });
  lay('a table at idx numtables', 'refused', L => { sightT(L)[3].idx = 9; });
  lay('tables out of order', 'refused', L => { sightT(L).reverse(); });
  lay('one index stored twice', 'refused', L => { sightT(L)[1].idx = sightT(L)[0].idx; });
  lay('an idx not an integer', 'refused', L => { sightT(L)[0].idx = 3.5; });
  lay('a table a list', 'refused', L => { sightT(L)[0] = sightT(L)[0].cells; });
  lay('a table with no cells', 'refused', L => { delete sightT(L)[0].cells; });
  lay('a table of no cell', 'accepted', L => { sightT(L)[0].cells = []; });
  lay('a cell 63 blocks out', 'accepted', L => { sightT(L)[0].cells = [[-63, -63, 1], [63, 63, 1]]; });
  lay('a cell 64 blocks out', 'refused', L => { sightT(L)[0].cells = [[64, 0, 1]]; });
  lay('a count of 255', 'accepted', L => { sightT(L)[0].cells = [[1, 0, 255]]; });
  lay('a count of 256', 'refused', L => { sightT(L)[0].cells = [[1, 0, 256]]; });
  lay('a count of 0', 'refused', L => { sightT(L)[0].cells = [[1, 0, 0]]; });
  lay('cells out of order', 'refused', L => { sightT(L)[0].cells = [[1, 0, 1], [0, 0, 1]]; });
  lay('one cell twice', 'refused', L => { sightT(L)[0].cells = [[1, 0, 1], [1, 0, 1]]; });
  lay('a cell of two numbers', 'refused', L => { sightT(L)[0].cells = [[1, 0]]; });
  for (const [n, want] of [[32768, 'accepted'], [32769, 'refused']])
    lay(`${n} cells stored`, want, L => {
      L.sight = { numtables: 9, tables: [{ idx: 1, cells: cellsN(16129) }, { idx: 2, cells: cellsN(16129) }, { idx: 3, cells: cellsN(n - 2 * 16129) }] };
    });
  return cases;
}

/* ---- THE GAME ENTRY'S REFUSALS (the loader's game rules, tagpu_wikidata.c game, and tagpu_wikisim_init's): each case the
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
  /* THE SCENE'S SELECTION is of the input seats' units (tagpu_wikisim_init): the script gives seat 1 no
     input, so a unit of seat 1 selected by the scene or by the demo's scene is refused, and accepted once seat 1 has
     one */
  const seat1 = sc => sc.units.find(u => (u.owner ?? 0) === 1).id;
  g('the scene selecting a unit of a seat with no input', 'refused', (e, d, sc) => { sc.selected = [seat1(sc)]; });
  g('the demo\'s scene selecting a unit of a seat with no input', 'refused', (e, d, sc) => { d.scene = { selected: [seat1(sc)] }; });
  g('the scene selecting a unit of a seat with an input', 'accepted', (e, d, sc, G) => {
    sc.selected = [seat1(sc)];
    G.inputs.push({ at: 200, seat: 1, move: [10, 10] });
  });
  g('the demo\'s scene selecting a unit of a seat with an input', 'accepted', (e, d, sc, G) => {
    d.scene = { selected: [seat1(sc)] };
    G.inputs.push({ at: 200, seat: 1, move: [10, 10] });
  });
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
  /* THE SEAT RULE (tagpu_wikidata.c seat_ok): a seat of the scene's two, and 0 alone unless the
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
  // a kill's killer, `by`, a seat of the scene, on any game entry
  for (const [v, want] of [[0, 'accepted'], [1, 'accepted'], [2, 'refused'], [-1, 'refused'], [0.5, 'refused'], ['1', 'refused']])
    g(`a kill by ${JSON.stringify(v)}`, want, (e, d, sc, G) => { G.inputs.push({ at: 300, kill: 'b0', by: v }); });
  // the players entry's data on a replay entry
  g('a camera input', 'refused', (e, d, sc, G) => { G.inputs.push({ at: 300, seat: 0, camera: [300, 300] }); });
  g('a seat\'s camera', 'refused', (e, d, sc) => { sc.seats[0].camera = [300, 300]; });
  g('a seat\'s team', 'refused', (e, d, sc) => { sc.seats[0].team = 0; });
  g('a screen', 'refused', e => { e.screen = [56, 30]; });
  g('a type\'s sight figures', 'accepted', e => { e.units.ARMCK.sightDistance = 0; e.units.ARMCK.radarDistance = -1; e.units.ARMCK.modelHeight = 7; });
  g('a type\'s sightDistance 32768', 'refused', e => { e.units.ARMCK.sightDistance = 32768; });
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

  /* THE VIEWER'S SCRIPT (the loader's viewer_input and viewer_order), each case played as a
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

  /* A LIVE ENTRY (the loader's live rule, the viewer's two sets, the fork parts' rules, THE LIVE DATA RULE;
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

/* ---- A TEXT ENTRY'S REFUSALS (tagpu_wikidata_entry's text branch): the text entry as authored, and
   edited to carry what only a stage reads or a `demos` that is not a list. No demo is played: each side's loader
   alone (`loader`), wiki.js's checkEntry and checkText against tagpu_wikidata_entry, and a refusal's words alike ---- */
function textRefusalCases() {
  const entry = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries/megamap.json'), 'utf8'));
  const cases = [];
  const text = (name, want, edit) => { const e = JSON.parse(JSON.stringify(entry)); edit(e);
    cases.push({ name: `text entry: ${name}`, want, loader: true, scripted: 0, layout: null, side: 0, iface: 1, casting: null, entry: e }); };
  text('as authored', 'accepted', () => {});
  text('demos absent', 'accepted', e => { delete e.demos; });
  text('demos an object', 'refused', e => { e.demos = {}; });
  text('with scenes', 'refused', e => { e.scenes = {}; });
  text('with units', 'refused', e => { e.units = {}; });
  text('with features', 'refused', e => { e.features = {}; });
  text('with a pace', 'refused', e => { e.pace = { build: 2 }; });
  text('with a game frame', 'refused', e => { e.frame = 'game'; });
  return cases;
}

/* ---- THE TAKEOVER ENTRY'S REFUSALS (the loader's takeover rules, kept forks and a part's seat; the viewer's
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
  /* THE SCENE'S SELECTION of a seat's unit: a takeover entry's game holds every seat's input state, so with no input of
     seat 1 it is accepted; a live entry's holds seat 1's only for an input or a part of that seat */
  const sel1 = e => { const sc = Object.values(e.scenes)[0]; sc.selected = [sc.units.find(u => (u.owner ?? 0) === 1).id]; };
  t('the scene selecting a unit of seat 1, no input of seat 1', 'accepted', (e, d, G) => {
    sel1(e); G.inputs = G.inputs.filter(x => (x.seat ?? 0) === 0);
  });
  t('a live entry\'s scene selecting a unit of seat 1, no input of seat 1', 'refused', (e, d, G) => {
    delete e.takeover; e.live = true; delete d.kept; d.inputs = []; G.inputs = G.inputs.filter(x => (x.seat ?? 0) === 0); sel1(e);
  });
  t('a live entry\'s scene selecting a unit of seat 1, a part of seat 1', 'accepted', (e, d, G) => {
    delete e.takeover; e.live = true; delete d.kept; d.inputs = []; G.inputs = G.inputs.filter(x => (x.seat ?? 0) === 0); sel1(e);
    d.forks = [part({ seat: 1 })];
  });
  // the viewer's script: the Take over set and its values, the live HUD's controls beside the replay HUD's
  r('every control but the Players set', 'accepted', [...WikiSim.HUD_CONTROLS.filter(c => !WikiSim.PLAYERS_CONTROLS.includes(c)).map((c, i) => ({ at: 10 + 30 * i, hud: c,
    ...(c === 'track' ? { to: 100 } : c === 'takeas' ? { seat: 1 } : ['lane', 'export', 'delete'].includes(c) ? { fork: 1 } : {}) }))]);
  for (const [v, want] of [[0, 'accepted'], [1, 'accepted'], [2, 'refused'], [-1, 'refused'], [0.5, 'refused'], ['1', 'refused']])
    r(`takeas seat ${JSON.stringify(v)}`, want, [{ at: 10, hud: 'takeas', seat: v }]);
  r('takeas without a seat', 'refused', [{ at: 10, hud: 'takeas' }]);
  // the Players set is a players entry's alone
  for (const c of WikiSim.PLAYERS_CONTROLS)
    r(`the Players set's ${c}`, 'refused', [{ at: 10, hud: c, ...(['jump', 'sight', 'follow'].includes(c) ? { seat: 0 } : c === 'chip' ? { chip: 'metal' } : {}) }]);
  // a kill's `by` on any game entry, a kept fork's too; a camera input a players entry's alone, never a fork's
  k('a kill by seat 1', 'accepted', K => { K[0].inputs = [{ at: 400, kill: 'c0', by: 1 }]; });
  k('a kill by seat 2', 'refused', K => { K[0].inputs = [{ at: 400, kill: 'c0', by: 2 }]; });
  k('a camera input', 'refused', K => { K[0].inputs = [{ at: 400, seat: 1, camera: [600, 400] }]; });
  t('a part\'s camera input', 'refused', (e, d) => { d.forks = [part({ inputs: [{ at: 700, seat: 0, camera: [600, 400] }] })]; });
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
   its verdict, and the three builds answer every call alike. ---- */
function forkCases() {
  const FORK_AT = [2, 301, 601, 901, 1201], AFTER = 300;
  const A = ['fixture, own types, type 1', FIXTURE, { side: 0, iface: 1, casting: null, rival: null }];
  const CTXS = [A, ['fixture, shifted, type 0', FIXTURE, { side: 0, iface: 0, casting: CASTINGS.shifted[0], rival: RIVALS.shifted }]];
  /* the synthetic part: a box over the builder, ARMSOLAR armed and placed, the rival commander killed; it plays on a
     game with the sidebar too: nothing stands in the build's trip (WIKI_TRIP, 90 ticks), and the builder has the
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
         plays on a game without the sidebar, whose builds take no trip through the column (WIKI_TRIP). The release is
         the taken seat's alone (tagpu_wikisim_release), so a unit another seat's recorded click selected stays selected */
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
   tick (tagpu_wikidata.c THE LIVE DATA RULE), and each must arm (the trip lands on the drawn gadget only when centreOf and
   inside agree on DZ), then a click at the centre of every gadget of the builder's pages taken in world z, the
   column's row minus FRAME_BAR, and two clicks over the minimap's rows; with the fixture's column and the tall one,
   at both Interface Types. ---- */
function columnCases() {
  const cases = [], T = WikiSim.TRIP, BAR = 32;
  // a takeover or a players entry's game has no column: its sidebar is false (tagpu_wikidata.c game)
  for (const [id, whole] of GAMES.filter(([, e]) => !e.takeover && !e.players)) {
    const scene = Object.values(whole.scenes)[0];
    for (const [ln, layout] of [['fixture', FIXTURE], ['fixture-tall', SIDE('fixture-tall')]]) {
      const types = layout.sides[0].types;
      const u = scene.units.find(s => (s.owner ?? 0) === 0 && types[s.type] && (whole.units[s.type].builds || []).length);
      if (!u) continue;
      const builds = whole.units[u.type].builds, inputs = [];
      let t = 10;
      // each build after a click on the builder on its tick, as a live entry's build needs (tagpu_wikidata.c THE LIVE DATA RULE)
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
   ticks, and the seated fork's refusals. In the fork pass's two contexts; each case's claims are its verdict. ---- */
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
  /* THE DEATH VISION'S BOUND (tasim_world.c tasim_kill): 24 more of Bram's units, killed 20 on tick 50, 2 on
     60, 1 on 111 and 1 on 112, by a script no players rule holds (a replay entry's) and by a fork part; the engine's
     add keeps the first 20, refuses the three while they are live (a death of tick d is live at an add on t while
     d + 61 >= t) and keeps the last */
  const many = JSON.parse(JSON.stringify(whole)), K = 24;
  many.scenes.field.units.push(...Array.from({ length: K }, (_, i) => ({ id: `k${i}`, type: 'CORAK', owner: 1,
    at: [560 + (i % 6) * 48, 200 + Math.floor(i / 6) * 48] })));
  const killsAt = [...Array(20).fill(50), 60, 60, 111, 112].map((at, i) => ({ at, kill: `k${i}` }));
  const eyes = sim => sim.view().game.deaths.filter(d => d.owner === 1).map(d => d.tick).join();
  const wantEyes = [...Array(20).fill(50), 112].join();
  const scripted = { ...many.demos[0], game: { end: 200, sidebar: false, inputs: killsAt } };
  cases.push({ name: `seat -- (${A[0]}) the death vision's bound, scripted`, layout: A[1], ctx: A[2], run: G => {
    const sim = G({ ...many, demos: [scripted] }, scripted, 'refuse');
    for (let t = 0; t < 130; t++) { sim.step(); sim.view(); }
    const got = eyes(sim);
    return got === wantEyes ? 'the 21st to 23rd live deaths refused, the 24th kept' : `FAIL deaths at ${got}, want ${wantEyes}`;
  } });
  const plain = { ...many.demos[0], game: { end: 200, sidebar: false, inputs: [] } };
  cases.push({ name: `seat -- (${A[0]}) the death vision's bound, a fork part`, layout: A[1], ctx: A[2], run: G => {
    const sim = G({ ...many, demos: [plain] }, plain, 'refuse');
    for (let t = 0; t < 40; t++) sim.step();
    sim.view();
    if (sim.fork(41, killsAt, 'refuse') !== 'ok') return 'FAIL the fork was refused';
    for (let t = 41; t < 130; t++) { sim.step(); sim.view(); }
    const got = eyes(sim);
    return got === wantEyes ? 'the 21st to 23rd live deaths refused, the 24th kept' : `FAIL deaths at ${got}, want ${wantEyes}`;
  } });
  return cases;
}

/* THE PLAYERS ENTRY'S DATA RULES (tagpu_wikidata.c game, viewer_input, viewer_order): the players fixture's first demo
   edited, each rule both ways; `replay` cases play it as a replay, so that the viewer's script is read too */
function playersRefusalCases(pf, page) {
  const clone = v => JSON.parse(JSON.stringify(v));
  const cases = [];
  const CTX = { side: 0, iface: 1, casting: null, rival: null, layout: FIXTURE };
  const sc0 = e => e.scenes[Object.keys(e.scenes)[0]];
  const t = (name, want, edit, replay = false, ctx = {}) => {
    const e = clone(pf);
    e.demos = [e.demos[0]];
    edit(e, e.demos[0], e.demos[0].game, sc0(e));
    cases.push({ name: `players: ${name}`, want, scripted: 2, ...CTX, ...ctx, entry: e, replay });
  };
  const r = (name, want, inputs) => t(`viewer: ${name}`, want, (e, d) => { d.inputs = inputs; }, true);
  const cam = (G, ...c) => { G.inputs = [...G.inputs.filter(x => !('camera' in x)), ...c]; };
  // the entry
  t('the fixture', 'accepted', () => {});
  t('the fixture as a replay', 'accepted', () => {}, true);
  t('bound to identity: no cast has a sight, so each keeps its own type', 'accepted', () => {}, false,
    { casting: CASTINGS.identity[0], rival: RIVALS.identity });
  t('bound to shifted and its rival', 'accepted', () => {}, false, { casting: CASTINGS.shifted[0], rival: RIVALS.shifted });
  {
    // a cast below the sight floor is left unbound in a players game, as one outside the economy is in any game
    const C = clone(CASTINGS.shifted[0]);
    C.casts.builder.sightDistance = 31;
    t('bound to a casting whose builder sees 31', 'accepted', () => {}, false, { casting: C, rival: RIVALS.shifted });
  }
  for (const v of [1, 'true', null]) t(`players ${JSON.stringify(v)}`, 'refused', e => { e.players = v; });
  t('players false, a screen', 'refused', e => { e.players = false; });
  t('players false, a camera', 'refused', (e, d, G) => { e.players = false; delete e.screen; cam(G); });
  t('players false, no camera, team or screen', 'accepted', (e, d, G, sc) => {
    e.players = false; delete e.screen; cam(G);
    for (const s of Object.values(e.scenes)) s.seats = s.seats.map(({ camera, team, ...z }) => z);
  });
  t('players and live', 'refused', e => { e.live = true; });
  t('players and takeover', 'refused', e => { e.takeover = true; });
  t('players, live and takeover false', 'accepted', e => { e.live = false; e.takeover = false; });
  cases.push({ name: 'players: players on a page entry', want: 'refused', scripted: 1, ...CTX, entry: { ...clone(page), players: true } });
  cases.push({ name: 'players: a screen on a page entry', want: 'refused', scripted: 1, ...CTX, entry: { ...clone(page), screen: [56, 30] } });
  t('the sidebar true', 'refused', (e, d, G) => { G.sidebar = true; });
  t('the sidebar absent', 'refused', (e, d, G) => { delete G.sidebar; });
  // the screen: even, within the map, the map as deep as the viewport beside the deepest column, a 4:3 frame
  t('no screen', 'refused', e => { delete e.screen; });
  for (const [v, want] of [[[56, 30], 'accepted'], [[64, 44], 'accepted'], [[58, 30], 'accepted'], [[56, 2], 'accepted'], [[55, 30], 'refused'], [[56, 31], 'refused'],
    [[54, 30], 'refused'], [[66, 30], 'refused'], [[56, 46], 'refused'], [[0, 30], 'refused'], [[56.5, 30], 'refused'], [['56', 30], 'refused'],
    [[56], 'refused'], [[56, 30, 2], 'refused'], [{ w: 56, h: 30 }, 'refused']])
    t(`the screen ${JSON.stringify(v)}`, want, (e, d, G) => {
      // no seat a camera, so that no camera's range decides
      e.screen = v;
      cam(G);
      for (const s of Object.values(e.scenes)) s.seats.forEach(z => { delete z.camera; });
    });
  const map = (w, h) => (e, d, G) => {
    cam(G);
    for (const s of Object.values(e.scenes)) { s.map = { ...s.map, w, h }; s.seats.forEach(z => { z.camera = [8 * 56, 8 * 30]; }); }
  };
  t('a map 64 x 44', 'accepted', map(64, 44));
  t('a map 66 x 44', 'accepted', map(66, 44));
  t('a map 64 x 42, shallower than the viewport beside the deepest column', 'refused', map(64, 42));
  t('a map 64 x 43', 'refused', map(64, 43));
  t('a map 63 x 44', 'refused', map(63, 44));
  t('a map 56 x 44, the screen\'s width', 'accepted', map(56, 44));
  // a seat's camera, inside [8 sw, 16 w - 8 sw] x [8 sh, 16 h - 8 sh] = [448, 576] x [240, 464]
  for (const [v, want] of [[[448, 352], 'accepted'], [[567, 352], 'accepted'], [[447, 352], 'refused'], [[577, 352], 'refused'],
    [[500, 240], 'accepted'], [[500, 464], 'accepted'], [[500, 239], 'refused'], [[500, 465], 'refused'], [[500.5, 352], 'refused'],
    [['500', 352], 'refused'], [[500], 'refused'], [null, 'refused']])
    t(`seat 0's camera ${JSON.stringify(v)}`, want, (e, d, G, sc) => { cam(G); sc.seats[0].camera = v; });
  t('seat 0 with no camera', 'accepted', (e, d, G, sc) => { cam(G); delete sc.seats[0].camera; });
  for (const [v, want] of [[0, 'accepted'], [3, 'accepted'], [4, 'refused'], [-1, 'refused'], [1.5, 'refused'], ['1', 'refused'], [null, 'refused']])
    t(`seat 1's team ${JSON.stringify(v)}`, want, (e, d, G, sc) => { sc.seats[1].team = v; });
  t('both seats one team', 'accepted', (e, d, G, sc) => { sc.seats[0].team = 2; sc.seats[1].team = 2; });
  t('a seat with another key', 'refused', (e, d, G, sc) => { sc.seats[0].colour = 1; });
  /* THE SIGHT FLOOR and the figures' bounds. The fixture layout stores the stock tables 3, 6, 7 and 8 alone, so a sight
     of 32 (radius index 1) refuses the game there and plays with no layout; 32767 is clamped to numtables - 1 = 8 */
  t('ARMCK\'s sightDistance 32, no layout', 'accepted', e => { e.units.ARMCK.sightDistance = 32; }, false, { layout: null });
  for (const [v, want] of [[96, 'accepted'], [95, 'refused'], [191, 'refused'], [192, 'accepted'], [287, 'accepted'], [288, 'accepted']])
    t(`ARMCK's sightDistance ${v}, over tables 3, 6, 7, 8`, want, e => { e.units.ARMCK.sightDistance = v; });
  t('a rival type of sightDistance 160, over tables 3, 6, 7, 8', 'refused', e => { e.units.CORAK.sightDistance = 160; });
  t('a rival type of sightDistance 160, no layout', 'accepted', e => { e.units.CORAK.sightDistance = 160; }, false, { layout: null });
  t('a layout with no sight table', 'accepted', e => { e.units.ARMCK.sightDistance = 40; }, false, { layout: { ...FIXTURE, sight: null } });
  {
    // a cast whose radius index the layout does not store is left unbound, its type playing its authored figures
    const C = clone(CASTINGS.identity[0]);
    C.casts.builder = { ...C.casts.builder, sightDistance: 170 };
    t('bound to a casting whose builder reaches table 5, not stored', 'accepted', () => {}, false, { casting: C, rival: RIVALS.identity });
    t('bound to a casting whose builder reaches table 5, own ARMCK at table 5', 'refused', e => { e.units.ARMCK.sightDistance = 170; }, false,
      { casting: C, rival: RIVALS.identity });
  }
  for (const [k, v, want] of [['sightDistance', 32, 'refused'], ['sightDistance', 31, 'refused'], ['sightDistance', 32767, 'accepted'],
    ['sightDistance', 32768, 'refused'], ['sightDistance', 40.5, 'refused'], ['sightDistance', '300', 'refused'], ['radarDistance', -32768, 'accepted'],
    ['radarDistance', -32769, 'refused'], ['sonarDistance', 32767, 'accepted'], ['modelHeight', 255, 'accepted'], ['modelHeight', 256, 'refused'],
    ['modelHeight', -1, 'refused']])
    t(`ARMCK's ${k} ${JSON.stringify(v)}`, want, e => { e.units.ARMCK[k] = v; });
  t('ARMCK with no sightDistance', 'refused', e => { delete e.units.ARMCK.sightDistance; });
  t('a rival type with no sightDistance', 'refused', e => { delete e.units.CORAK.sightDistance; });
  // camera inputs
  const C0 = (o = {}) => ({ at: 200, seat: 0, camera: [500, 320], over: 60, ...o });
  t('a camera input', 'accepted', (e, d, G) => cam(G, C0()));
  for (const [o, want] of [[{ over: 0 }, 'accepted'], [{ over: 5400 }, 'accepted'], [{ over: 5401 }, 'refused'], [{ over: -1 }, 'refused'],
    [{ over: 1.5 }, 'refused'], [{ at: 200.5 }, 'refused'], [{ at: -1 }, 'refused'], [{ at: 1800 }, 'accepted'], [{ at: 1801 }, 'refused'],
    [{ camera: [447, 320] }, 'refused'], [{ camera: [500, 465] }, 'refused'], [{ camera: [500.5, 320] }, 'refused'],
    [{ camera: [500] }, 'refused'], [{ seat: 1 }, 'accepted'], [{ seat: 2 }, 'refused'], [{ move: [1, 1] }, 'refused'],
    [{ down: 'select' }, 'refused'], [{ kill: 'w1' }, 'refused']])
    t(`a camera input ${JSON.stringify(o)}`, want, (e, d, G) => cam(G, C0(o)));
  t('a camera input with no over', 'accepted', (e, d, G) => { const c = C0(); delete c.over; cam(G, c); });
  t('a camera input with no seat, seat 0\'s', 'accepted', (e, d, G) => { const c = C0(); delete c.seat; cam(G, c); });
  t('a camera input of a seat with no camera', 'refused', (e, d, G, sc) => { delete sc.seats[1].camera; cam(G, C0({ seat: 1 })); });
  t('a camera input on a replay entry', 'refused', (e, d, G, sc) => {
    e.players = false; delete e.screen; for (const s of Object.values(e.scenes)) s.seats = s.seats.map(({ camera, team, ...z }) => z);
    cam(G, C0());
  });
  // a glide never overlaps its seat's previous one; another seat's may
  t('a camera input at the end of its seat\'s glide', 'accepted', (e, d, G) => cam(G, C0(), C0({ at: 260, camera: [448, 352] })));
  t('a camera input inside its seat\'s glide', 'refused', (e, d, G) => cam(G, C0(), C0({ at: 259, camera: [448, 352] })));
  t('a camera input before its seat\'s previous one', 'refused', (e, d, G) => cam(G, C0(), C0({ at: 100, camera: [448, 352], over: 0 })));
  t('a camera input inside another seat\'s glide', 'accepted', (e, d, G) => cam(G, C0(), C0({ at: 230, seat: 1, camera: [500, 300] })));
  // a camera input acts on no input state: seat 1's alone holds no input state for a selection of seat 1's unit
  const sel1 = e => { const sc = sc0(e); sc.selected = [sc.units.find(u => (u.owner ?? 0) === 1).id]; };
  t('the scene selecting a unit of seat 1, a camera input of seat 1', 'refused', (e, d, G) => { sel1(e); cam(G, C0({ seat: 1, camera: [520, 300] })); });
  t('the scene selecting a unit of seat 1, a move of seat 1', 'accepted', (e, d, G) => { sel1(e); G.inputs.push({ at: 300, seat: 1, move: [520, 200] }); });
  /* THE VIEW RULE: Ada's last move is (120, 376) at tick 92, and her view is 448 a side wide and 240 deep; the camera
     glides under the resting pointer */
  t('seat 0\'s camera at x 567', 'accepted', (e, d, G, sc) => { cam(G); sc.seats[0].camera = [567, 352]; });
  t('seat 0\'s camera at x 568', 'refused', (e, d, G, sc) => { cam(G); sc.seats[0].camera = [568, 352]; });
  t('a glide to x 567 after the last move', 'accepted', (e, d, G) => cam(G, C0({ camera: [567, 352], over: 100 })));
  t('a glide to x 568 after the last move', 'refused', (e, d, G) => cam(G, C0({ camera: [568, 352], over: 100 })));
  t('a glide away at the end', 'refused', (e, d, G) => cam(G, C0({ at: 1800, camera: [576, 352], over: 0 })));
  t('a glide away past the end', 'accepted', (e, d, G) => cam(G, C0({ at: 1740, camera: [576, 352], over: 3000 })));
  t('seat 0 with no camera, its moves anywhere', 'accepted', (e, d, G, sc) => { cam(G); delete sc.seats[0].camera; G.inputs.push({ at: 300, seat: 0, move: [1000, 690] }); });
  t('a move outside the view', 'refused', (e, d, G) => { G.inputs.push({ at: 300, seat: 0, move: [1000, 690] }); });
  t('a move just inside the view', 'accepted', (e, d, G, sc) => { cam(G); G.inputs.push({ at: 300, seat: 0, move: [448 + 447, 352 + 239] }); sc.seats[0].camera = [448, 352]; });
  t('a move on the view\'s edge', 'refused', (e, d, G, sc) => { cam(G); G.inputs.push({ at: 300, seat: 0, move: [448 + 448, 352] }); sc.seats[0].camera = [448, 352]; });
  t('a move outside the view, then back on its tick', 'accepted', (e, d, G, sc) => {
    cam(G); sc.seats[0].camera = [448, 352]; G.inputs.push({ at: 300, seat: 0, move: [1000, 690] }, { at: 300, seat: 0, move: [400, 352] });
  });
  t('a move back, then outside the view on its tick', 'refused', (e, d, G, sc) => {
    cam(G); sc.seats[0].camera = [448, 352]; G.inputs.push({ at: 300, seat: 0, move: [400, 352] }, { at: 300, seat: 0, move: [1000, 690] });
  });
  // kills and their killers
  for (const [v, want] of [[0, 'accepted'], [1, 'accepted'], [2, 'refused'], [-1, 'refused'], [0.5, 'refused'], ['0', 'refused'], [null, 'refused']])
    t(`a kill by ${JSON.stringify(v)}`, want, (e, d, G) => { G.inputs.find(x => x.kill === 'w1').by = v; });
  t('a kill with no by', 'accepted', (e, d, G) => { delete G.inputs.find(x => x.kill === 'w1').by; });
  /* A PLAYERS ENTRY'S DEATHS: 21 more of Ada's units, killed at the ticks given; 20 in any 62 ticks at most */
  const deaths = ticks => (e, d, G, sc) => {
    sc.units.push(...ticks.map((_, i) => ({ id: `d${i}`, type: 'ARMCK', at: [40 + 40 * (i % 10), 520 + 40 * Math.floor(i / 10)] })));
    G.inputs.push(...ticks.map((at, i) => ({ at, kill: `d${i}` })));
  };
  const span = (n, from, step) => Array.from({ length: n }, (_, i) => from + i * step);
  t('20 of seat 0\'s units killed on one tick', 'accepted', deaths(span(20, 300, 0)));
  t('21 of seat 0\'s units killed on one tick', 'refused', deaths(span(21, 300, 0)));
  t('21 of seat 0\'s units killed over 61 ticks', 'refused', deaths(span(21, 300, 3).map((v, i) => i === 20 ? 361 : v)));
  t('21 of seat 0\'s units killed over 62 ticks', 'accepted', deaths(span(21, 300, 3).map((v, i) => i === 20 ? 362 : v)));
  t('21 of seat 0\'s units killed 4 ticks apart', 'accepted', deaths(span(21, 300, 4)));
  t('20 of seat 0\'s units and one of seat 1\'s on one tick', 'accepted', (e, d, G, sc) => {
    deaths(span(20, 1200, 0))(e, d, G, sc);
  });
  t('21 of seat 0\'s units on a players false entry', 'accepted', (e, d, G, sc) => {
    e.players = false; delete e.screen; cam(G); for (const s of Object.values(e.scenes)) s.seats = s.seats.map(({ camera, team, ...z }) => z);
    deaths(span(21, 300, 0))(e, d, G, sc);
  });
  // the viewer's script: the replay HUD's controls and the Players set, their values; no other entry's
  const value = c => c === 'track' ? { to: 100 } : ['jump', 'sight', 'follow'].includes(c) ? { seat: 1 } : c === 'chip' ? { chip: 'metal' } : {};
  r('every control of the replay HUD and the Players set', 'accepted', [...WikiSim.REPLAY_CONTROLS, ...WikiSim.PLAYERS_CONTROLS]
    .map((c, i) => ({ at: 10 + 30 * i, hud: c, ...value(c) })));
  for (const c of ['jump', 'sight', 'follow']) {
    for (const [v, want] of [[0, 'accepted'], [1, 'accepted'], [2, 'refused'], [-1, 'refused'], [0.5, 'refused'], ['1', 'refused']])
      r(`${c} seat ${JSON.stringify(v)}`, want, [{ at: 10, hud: c, seat: v }]);
    r(`${c} without a seat`, 'refused', [{ at: 10, hud: c }]);
    r(`${c} with a chip`, 'refused', [{ at: 10, hud: c, seat: 0, chip: 'metal' }]);
  }
  for (const c of WikiSim.CHIPS) r(`chip ${c}`, 'accepted', [{ at: 10, hud: 'chip', chip: c }]);
  for (const v of ['nope', 'Metal', 0, null]) r(`chip ${JSON.stringify(v)}`, 'refused', [{ at: 10, hud: 'chip', chip: v }]);
  r('chip without a chip', 'refused', [{ at: 10, hud: 'chip' }]);
  r('chip with a seat', 'refused', [{ at: 10, hud: 'chip', chip: 'metal', seat: 0 }]);
  for (const c of ['cursors', 'clicks', 'selections', 'chat', 'marks', 'stats', 'close'])
    r(`${c} with a seat`, 'refused', [{ at: 10, hud: c, seat: 0 }]);
  for (const c of ['fold', 'leave:yes', 'takeover', 'takeas', 'lanes', 'live_now', 'rewind'])
    r(`${c}`, 'refused', [{ at: 10, hud: c, ...(c === 'takeas' || c === 'fold' ? { seat: 0 } : {}) }]);
  // the Players set on a replay entry
  for (const c of WikiSim.PLAYERS_CONTROLS)
    t(`the Players set's ${c} on a replay entry`, 'refused', (e, d, G) => {
      e.players = false; delete e.screen; cam(G); for (const s of Object.values(e.scenes)) s.seats = s.seats.map(({ camera, team, ...z }) => z);
      d.inputs = [{ at: 10, hud: c, ...value(c) }];
    }, true);
  return cases;
}

/* ---- THE DATA READ: every casting of the fixture and of each side pack present, and every layout of art/wiki/side/ and
   of those packs ---- */
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

/* ---- THE C RUNS: wikitest's own modes, the replay layer's and the rules', in `tmp`, this process's own folder ---- */
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'simparity-'));
/* the `wine` build's environment, asked for the first time a wine run needs it: a host-only run (--hash) makes no prefix */
const WINE_ENV = () => { try { return wineEnv(); } catch (e) { throw new CannotRun(e.message); } };
let EXES = null;
function wikitest(target, args) {
  EXES ??= Object.fromEntries(TARGETS.map(t => [t, hostBuild(t)]));
  return target === 'wine' ? run('wine', [EXES.wine, ...args], { cwd: tmp, env: WINE_ENV() }) : run(EXES[target], args, { cwd: tmp });
}
/* wikitest `args` on each of `builds`: the first build's lines, once every other printed the same; a build that printed
   otherwise is a difference, its first differing line named */
function everyBuild(args, builds = TARGETS) {
  const outs = builds.map(t => lines(wikitest(t, args)));
  for (let k = 1; k < outs.length; k++) {
    const a = outs[0], b = outs[k];
    let i = 0;
    while (i < a.length && i < b.length && a[i] === b[i]) i++;
    if (i < a.length || i < b.length)
      return { out: a, differs: `wikitest ${args[0]}: ${builds[k]} line ${i + 1}: ${show(b[i] ?? '(none)')}, ${builds[0]} ${show(a[i] ?? '(none)')}` };
  }
  return { out: outs[0], differs: null };
}

/* THE TEE'S DIFFERENCES since `from` (an index of hostchan's list): the first, as a line */
const teeSince = from => {
  const d = TEE.differences;
  if (d.length <= from) return null;
  const x = d[from];
  const n = `${d.length - from} request${d.length - from > 1 ? 's' : ''} apart; the first: ${x.request}`;
  return x.dead ? `a build could not answer: ${x.dead} (${n})` : `${x.target} answered otherwise at ${x.at}: ${show(x.b)}, host ${show(x.a)} (${n})`;
};

let failed = 0, plays = 0, playBad = 0, cases = 0, caseBad = 0, refusals = 0, refuseBad = 0, datas = 0, dataBad = 0, malformed = 0, bad = 0;
const differ = line => { console.log(`DIFFER ${line}`); return false; };

/* ONE PLAY of a sim made by `make`, every tick read: the view, every unit's orders and the state hash, and with `sight`
   (a players game's: the layout's sight table, null for none) the sight of each seat (sightOut); the map once more on
   the last tick. The builds must answer it alike, and a refused sim be refused by every build; it is kept under `key`
   (THE RECORD). `after(sim)`, given the played sim, names a claim of the play's own that failed. Returns whether it
   passed. */
function play(label, make, key, sight, after) {
  const d0 = TEE.differences.length;
  let sim, why = null;
  plays++;
  try { sim = make(); } catch (e) { why = e.message; }
  if (why !== null) {
    const d = teeSince(d0);
    if (d) { playBad++; return differ(`${label}: refused (${why}), and ${d}`); }
    rec(key, 'refused');
    console.log(`equal  ${label}: refused by every build (${why})`);
    return true;
  }
  const map0 = JSON.stringify(sim.view().map), hs = [];
  let ticks = 0, own = null;
  for (;;) {
    const v = sim.view();
    for (const u of v.units) sim.orders(u.id);
    hs.push(sim.hash());
    if (sight !== undefined) sightOut(sim, v, sight);
    ticks++;
    if (sim.done) break;
    sim.step();
  }
  const mapEnd = JSON.stringify(sim.view(0, true).map);
  if (after) own = after(sim);
  sim.free();
  const d = teeSince(d0);
  if (d || mapEnd !== map0 || own) {
    playBad++;
    return differ(`${label}: ${d || (mapEnd !== map0 ? 'the map on the last tick is not the map the sim was made with' : own)}`);
  }
  rec(key, hs);
  console.log(`equal  ${label}: ${ticks} ticks`);
  return true;
}

/* ---- THE CONTEXTS' FILES for the C replay layer: the castings, the rivals, the identity casting with every bound
   cast's economy outside a game's bounds, and the scripts' and sounds' facts as one cob.json ---- */
const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
const ids = [...new Set(tree.tree.flatMap(leaves))];
const ENTRY = id => JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
// the identity casting with every bound cast's economy outside a game's bounds (tagpu_wikidata.c econ_fits): a game leaves each unbound
const ECON_OFF = { ...CASTINGS.identity[0], casts: Object.fromEntries(Object.entries(CASTINGS.identity[0].casts).map(([c, k]) =>
  [c, k && { ...k, metalStorage: -1, energyUse: 1e7 }])) };
let filesMade = false;
function cFiles() {
  if (filesMade) return;
  filesMade = true;
  run('python3', [path.join(ROOT, 'tools/hudpak.py'), '--set', 'wiki', path.join(ROOT, 'art/wiki'), path.join(tmp, 'wiki.bin')]);
  for (const name of Object.keys(CASTINGS)) {
    fs.writeFileSync(path.join(tmp, `casting-${name}.json`), JSON.stringify(CASTINGS[name][0]));
    fs.writeFileSync(path.join(tmp, `rival-${name}.json`), JSON.stringify(RIVALS[name]));
  }
  fs.writeFileSync(path.join(tmp, 'cob.json'), JSON.stringify(Object.fromEntries([...new Set([...Object.keys(SCRIPTS), ...Object.keys(SOUNDS)])].map(n => {
    const t = SCRIPTS[n];
    return [n, { ...(t ? { cob: t.cob, visible: t.visible, offset: t.offset, parent: t.parent, top: t.top } : {}),
                 ...(SOUNDS[n] ? { sound: SOUNDS[n] } : {}) }];
  }))));
}
// a context's ctx as the drop-in takes it: its castings and, with the scripts, the gfx packs
const ctxOf = ({ cob, cname, iface }, rival) => ({ side: 0, casting: cname ? CASTINGS[cname][0] : null, iface,
  rival: rival && cname ? RIVALS[cname] : null, packs: cob ? PACK_DIRS : null });

/* ---- THE WORK ITEMS (the header's), each {key, run}, in a fixed order every shard builds alike ---- */
const ITEMS = [];
const item = (key, f) => ITEMS.push({ key, run: f });

/* THE DEMO PASS: every demo of the tree in one context */
for (const c of CONTEXTS) item(`demos|${c.named}`, () => {
  const layout = c.side && SIDE(c.side), ctx = ctxOf(c, false);
  for (const id of ids) {
    const entry = ENTRY(id);
    for (const demo of entry.demos)
      play(`${id}/${demo.id} (${c.named})`, () => WikiSim.play(entry, demo, layout, ctx), `demos|${c.named}|${id}/${demo.id}|play`);
  }
});

/* THE GAME PASS: a game entry's games in one context, each distinct game once (demos that share their scene and their
   game script play the same game; the replay pass plays every demo), then THE STALL CASE -- every seat's settings cut
   to WIKI_SEAT_MIN so that the builds run dry: in stall mode, and in refuse mode, which must end starved -- and with the
   identity casting every cast's economy outside the game's bounds, which must stop it under the badge */
for (const c of CONTEXTS) for (const [id, whole] of PLAYS) item(`game|${c.named}|${id}`, () => {
  const layout = c.side && SIDE(c.side), gctx = ctxOf(c, true);
  const games = new Set(), entry = { ...whole, demos: whole.demos.filter(d => {
    const k = JSON.stringify([d.on ?? null, d.scene ?? null, d.game]);
    return !games.has(k) && games.add(k);
  }) };
  const cut = { ...entry, scenes: Object.fromEntries(Object.entries(entry.scenes).map(([n, sc]) =>
    [n, { ...sc, seats: sc.seats.map(z => ({ ...z, metal: WIKI_SEAT_MIN, energy: WIKI_SEAT_MIN })) }])) };
  const variants = [[entry, 'refuse', '', 'game', gctx], [cut, 'stall', ' stall', 'stall', gctx], [cut, 'refuse', ' starved', 'starved', gctx]];
  if (c.cname === 'identity') variants.push([entry, 'refuse', ' economy off', 'econoff', { ...gctx, casting: ECON_OFF }]);
  for (const [e, mode, tag, variant, pctx] of variants)
    for (const demo of e.demos) {
      const sight = e.players === true ? (layout && layout.sight) || null : undefined;
      // the cut game in refuse mode ends starved, unless the context locked it first; the economy off stops it
      const after = tag === ' starved' ? s => { const v = s.view(); return v.locked === null && !v.game.starved ? `the game with every seat at ${WIKI_SEAT_MIN} did not starve` : null; }
        : tag === ' economy off' ? s => (s.view().locked === null ? 'the game did not stop under the badge' : null) : null;
      play(`${id}/${demo.id} (${c.named}, game${tag})`, () => WikiSim.game(e, demo, layout, pctx, mode), `game|${c.named}|${id}/${demo.id}|${variant}`, sight, after);
    }
});

/* THE REPLAY PASS: a game entry's demos as replays in one context, through replay.js on the drop-in against the C
   replay layer's lines (wikitest `replay`, every build alike), demo tick by demo tick -- the replay's view and hash,
   the game's view, orders and hash; then THE PRESS TRACE, Try it yourself (create) with the entry's presses against
   `replaytrace` */
/* one replay against the C lines `cout` from `at.i` on, which it consumes; `presses` for Try it, else the viewer's
   script; with `sight` (a players entry's: the layout's sight table, null for none) the shown game's sight each demo
   tick too (sightOut), taken while the view still holds the map it reads */
function compareReplay(cout, at, id, demo, make, named, presses, sight, key) {
  const label = `${id}/${demo.id} (${named})`, d0 = TEE.differences.length;
  let R, why = null;
  plays++;
  try { R = make(); } catch (e) { why = e.message; }
  if (why !== null || (at.i < cout.length && 'refused' in JSON.parse(cout[at.i]))) {
    const c = at.i < cout.length ? JSON.parse(cout[at.i]) : null;
    if (c && c.entry === id && c.demo === demo.id) at.i++;
    if (R) R.free();
    const d = teeSince(d0);
    if (c && 'refused' in c && why !== null && !d) { rec(key, 'refused'); console.log(`equal  ${label}: both refused (${why})`); return; }
    playBad++;
    differ(`${label}: C ${c && 'refused' in c ? `refused (${c.refused})` : 'played'}, replay.js ${why !== null ? `refused (${why})` : 'played'}${d ? `; ${d}` : ''}`);
    while (at.i < cout.length && JSON.parse(cout[at.i]).demo === demo.id && JSON.parse(cout[at.i]).entry === id) at.i++;
    return;
  }
  let first = null, ticks = 0;
  const hs = [];
  for (;;) {
    const rv = R.view(), last = rv.done || (presses && R.tick >= presses.end), g = R.game;
    const v = g ? g.view() : null, sv = v && sight !== undefined ? sightOut(g, v, sight) : undefined;
    if (v && (R.tick === 0 || last)) v.map = g.view(0, true).map;
    else if (v) delete v.map;
    const j = { entry: id, demo: demo.id, t: R.tick, h: replayHash(R), r: rv, gh: g ? g.hash() : null, v,
      o: g ? Object.fromEntries(v.units.map(u => [u.id, g.orders(u.id)])) : null, ...(sv ? { s: sv } : {}) };
    if (at.i >= cout.length) { first = { t: R.tick, at: '(the C replay ended early)', c: null, j: null }; break; }
    const cl = JSON.parse(cout[at.i++]), d = diff(cl, j, '');
    hs.push([cl.h, cl.gh]);
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
  const d = teeSince(d0);
  if (first || d) {
    playBad++;
    if (first) differ(`${label} at demo tick ${first.t}: ${first.at}: C ${show(first.c)}, replay.js ${show(first.j)}`);
    else differ(`${label}: ${d}`);
    while (at.i < cout.length && JSON.parse(cout[at.i]).demo === demo.id && JSON.parse(cout[at.i]).entry === id) at.i++;
  } else { rec(key, hs); console.log(`equal  ${label}: ${ticks} demo ticks`); }
}
for (const c of CONTEXTS) for (const [id, entry] of PLAYS) item(`replay|${c.named}|${id}`, () => {
  cFiles();
  const { side, cname, iface, cob, named } = c, layout = side && SIDE(side), gctx = ctxOf(c, true);
  const file = `game-${id}.json`;
  fs.writeFileSync(path.join(tmp, file), JSON.stringify(entry));
  const args = ['wiki.bin', file, side || '-', cname ? `casting-${cname}.json` : '-', cname ? `rival-${cname}.json` : '-',
    String(iface), cob ? 'cob.json' : '-'];
  const C = everyBuild(['replay', ...args]);
  if (C.differs) { playBad++; differ(`${id} (${named}, replay): ${C.differs}`); }
  const at = { i: 0 };
  const sight = entry.players === true ? (layout && layout.sight) || null : undefined;
  for (const demo of entry.demos)
    compareReplay(C.out, at, id, demo, () => WikiReplay.play(entry, demo, layout, gctx), `${named}, replay`, null, sight, `replay|${named}|${id}/${demo.id}|replay`);
  if (at.i !== C.out.length) { playBad++; differ(`(${named}, replay) the C replay of ${id} played ${C.out.length - at.i} frames more`); }
  // a live entry's Try it takes the live HUD's presses and the world's input: LIVE_TRACE; a takeover entry's its
  // TAKEOVER_TRACES, on its last demo
  const traces = entry.takeover ? TAKEOVER_TRACES.map((t, k) => [t, { ...entry, demos: [entry.demos[entry.demos.length - 1]] }, `tktrace${k + 1}`])
    : [[entry.live ? LIVE_TRACE : entry.players ? PLAYERS_TRACE : TRACE, entry, entry.live ? 'livetrace' : entry.players ? 'playerstrace' : 'trace']];
  for (const [trace, te, variant] of traces) {
    fs.writeFileSync(path.join(tmp, `presses-${id}.json`), JSON.stringify(trace));
    fs.writeFileSync(path.join(tmp, `pressgame-${id}.json`), JSON.stringify(te));
    const T = everyBuild(['replaytrace', ...args.map(a => a === file ? `pressgame-${id}.json` : a), `presses-${id}.json`]);
    if (T.differs) { playBad++; differ(`${id} (${named}, press trace): ${T.differs}`); }
    const tat = { i: 0 };
    compareReplay(T.out, tat, id, te.demos[0], () => WikiReplay.create(te, te.demos[0], layout, gctx), `${named}, press trace`, trace, sight,
      `replay|${named}|${id}/${te.demos[0].id}|${variant}`);
    if (tat.i !== T.out.length) { playBad++; differ(`(${named}, press trace) the C replay of ${id} played ${T.out.length - tat.i} frames more`); }
  }
});

/* THE FORK, COLUMN AND SEAT CASES (forkCases, columnCases, seatCases): each case's game through the drop-in, its hash
   kept after each view; a case's claims are its own verdict */
for (const c of [...forkCases(), ...columnCases(), ...seatCases()]) item(`case|${c.name}`, () => {
  const d0 = TEE.differences.length, hashes = [];
  let verdict;
  // the case's game: each view (as `seat` sees it) followed by the state hash, which the record keeps
  const G = (entry, demo, mode) => {
    const sim = WikiSim.game(entry, demo, c.layout, c.ctx, mode);
    return {
      step() { sim.step(); },
      view(seat = 0) { const v = sim.view(seat); hashes.push(sim.hash()); return v; },
      fork(R, inputs, fmode, seat = 0) {
        try { sim.fork(R, inputs, fmode, seat); return 'ok'; } catch (e) { return 'refused'; }
      },
      release(seat = 0) { sim.release(seat); },
      get fed() { return sim.gameNow().fed; },
      fedOf(seat) { return sim.gameNow(seat).fed; },
      get tick() { return sim.tick; },
    };
  };
  try { verdict = c.run(G); } catch (e) { verdict = `FAIL threw ${e.message}`; }
  cases++;
  const d = teeSince(d0);
  if (d) { caseBad++; differ(`${c.name}: ${d}`); }
  else if (String(verdict).startsWith('FAIL')) { caseBad++; differ(`${c.name}: the builds alike, but the case's claim failed: ${verdict}`); }
  else { rec(`case|-|-|${c.name}`, hashes); console.log(`equal  ${c.name}: ${hashes.length} views (${verdict})`); }
});

/* THE RELEASE SWEEP (simcheck.mjs releaseSweep): a live entry's games with Back to now's release after every tick up to
   the script's last input, then played on to the end, in the fork pass's two contexts; each end's hash kept. That no
   release stops a game is simcheck's claim. */
for (const [named, cname, iface] of [['fixture, own types, type 1', null, 1], ['fixture, shifted, type 0', 'shifted', 0]])
  for (const [id, entry] of GAMES.filter(([, e]) => e.live)) item(`sweep|${named}|${id}`, () => {
    const ctx = { side: 0, iface, casting: cname ? CASTINGS[cname][0] : null, rival: cname ? RIVALS[cname] : null };
    const d0 = TEE.differences.length, hs = [];
    let n = 0;
    for (const demo of entry.demos) {
      const last = Math.max(0, ...demo.game.inputs.map(e => e.at));
      for (let r = 0; r <= last; r++, n++) {
        const sim = WikiSim.game(entry, demo, FIXTURE, ctx, 'refuse');
        while (sim.tick < r && !sim.done) sim.steps(Math.min(4096, r - sim.tick));
        sim.release();
        while (!sim.done) sim.steps(4096);
        hs.push([`sweep|${named}|${id}/${demo.id}|sweep ${r}`, [sim.hash()]]);
        sim.free();
      }
    }
    cases++;
    const d = teeSince(d0);
    if (d) { caseBad++; differ(`release sweep -- ${id} (${named}): ${d}`); }
    else { for (const [k, h] of hs) rec(k, h); console.log(`equal  release sweep -- ${id} (${named}): ${n} releases, each game's end alike`); }
  });

/* simcheck.mjs's RULE CASES, an entry's: each case's sims through the drop-in, the state hash kept after each view;
   the case's verdict is simcheck's to judge, and is shown */
for (const id of ids) item(`rule|${id}`, async () => {
  const { ENTRY_RULES } = await import('./simcheck.mjs');
  const entry = ENTRY(id), { create, play: demoPlay } = WikiSim;
  let hashes;
  const kept = make => (...a) => {
    const sim = make(...a), view = sim.view;
    sim.view = (...v) => { const r = view(...v); hashes.push(sim.hash()); return r; };
    return sim;
  };
  WikiSim.create = kept(create);
  WikiSim.play = kept(demoPlay);
  try {
    for (const [what, check] of Object.entries(ENTRY_RULES[id] || {})) {
      const d0 = TEE.differences.length;
      hashes = [];
      let verdict;
      try { verdict = check(entry); } catch (e) { verdict = `threw ${e.message}`; }
      cases++;
      const d = teeSince(d0);
      if (d) { caseBad++; differ(`rule -- ${what}: ${d}`); }
      else { rec(`rule|-|${id}|${what}`, hashes); console.log(`equal  rule -- ${what}: ${hashes.length} views (verdict: ${verdict})`); }
    }
  } finally { Object.assign(WikiSim, { create, play: demoPlay }); }
});

/* THE CONSTANTS: the build trip's and the replay layer's (wikitest `constants`, every build alike) against replay.js and
   the host's hello; a players entry's: the stats window's chips, the Players set, the deaths' bound and the sight
   table's bounds */
item('constants', () => {
  const C = everyBuild(['constants']);
  cases += 2;
  if (C.differs) { caseBad++; differ(C.differs); return; }
  const [cl, pl] = C.out, c = JSON.parse(cl), W = WikiReplay, S = WikiSim;
  const j = { TRIP: S.TRIP, TRIP_DOWN: S.TRIP_DOWN, TRIP_UP: S.TRIP_UP, PICK: S.PICK, PICK_DOWN: S.PICK_DOWN, PICK_UP: S.PICK_UP,
    TRIP_ROWS: S.TRIP_ROWS,
    REPLAY: { CONTROLS: W.CONTROLS, SPEEDS: W.SPEEDS, SPEED0: W.SPEED0, SEEK: W.SEEK, WALK: W.WALK, STEP: W.STEP, NEAR: W.NEAR,
      BOOKS: W.BOOKS, MARKS: W.MARKS, FILL: W.FILL, HUD_DOWN: W.HUD_DOWN, HUD_UP: W.HUD_UP, DEATH: W.DEATH, DEFEAT: W.DEFEAT,
      GAME_END_MAX: W.GAME_END_MAX } };
  const d = diff(c, j, '');
  if (d) { caseBad++; differ(`the build trip's and the replay's constants: ${d.at}: C ${show(d.c)}, host and replay.js ${show(d.j)}`); }
  else console.log(`equal  the build trip's and the replay's constants: TRIP ${c.TRIP}, down ${c.TRIP_DOWN}, up ${c.TRIP_UP}, ${c.TRIP_ROWS.length} rows; seek ${c.REPLAY.SEEK}, walk ${c.REPLAY.WALK}, ${c.REPLAY.CONTROLS.length} controls`);
  const p = JSON.parse(pl);
  const q = diff(p, { CHIPS: S.CHIPS, PLAYERS_CONTROLS: S.PLAYERS_CONTROLS, PLAYERS_DEATHS: S.PLAYERS_DEATHS,
    PLAYERS_DEATH_SPAN: S.PLAYERS_DEATH_SPAN, SIGHT_MIN: S.SIGHT_MIN, SIGHT_NUMTABLES: S.SIGHT_NUMTABLES, SIGHT_IDX: S.SIGHT_IDX,
    SIGHT_REACH: S.SIGHT_REACH, SIGHT_CELLS: S.SIGHT_CELLS, STAT_MEASURES: W.MEASURES.length, STAT_MEASURE0: W.MEASURE0,
    STAT_PTS: W.STAT_PTS, TOG_ALL: W.TOG_ALL, NOT_ASKED: W.NOT_ASKED, PASS: W.PASS }, '');
  if (q) { caseBad++; differ(`the players entry's constants: ${q.at}: C ${show(q.c)}, host and replay.js ${show(q.j)}`); }
  else console.log(`equal  the players entry's constants: ${p.CHIPS.length} chips, ${p.PLAYERS_CONTROLS.length} controls, ` +
    `${p.PLAYERS_DEATHS} deaths in ${p.PLAYERS_DEATH_SPAN} ticks, sight from ${p.SIGHT_MIN}`);
});
/* ---- the recorder's rules: tagpu_recrules.h's own functions (wikitest `rules`) against replay.js's twins, and the
   refresh grid against tools/replay/tarec.py's is_refresh, which the file checks use; `want` where a case's answer is
   stated elsewhere: test_tareplay.py test_the_grid's, and the wiki's refresh points (rewind-spec §3.4); every build
   alike ---- */
item('rules', () => {
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
  const C = everyBuild(['rules', 'rules.txt']);
  if (C.differs) { cases++; caseBad++; differ(C.differs); return; }
  const got = C.out.map(l => JSON.parse(l));
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
      caseBad++;
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
});

/* THE REFUSALS: each case accepted or refused as it wants, by every build alike; a loader case is the loader alone (a
   text entry's words), and text.js's checkEntry, which wiki.js asks before any sim is made, refuses only what the case
   wants refused */
item('refusals', () => {
  const entry = ENTRY('quickbuild');
  /* the game cases edit a replay entry's first demo: a sidebar-free game whose viewer presses the replay HUD, which
     they turn live themselves (gameRefusalCases, A LIVE ENTRY); a live entry's own data loads alike in the demo plays,
     and a takeover or a players entry's data rules are their fixtures' cases */
  const all = [...refusalCases(entry), ...GAMES.filter(([, game]) => !game.live && !game.takeover && !game.players).flatMap(([, game]) => gameRefusalCases(game, entry)),
    ...takeoverRefusalCases(PLAYS[PLAYS.length - 1][1], entry), ...playersRefusalCases(playersFixture(), entry),
    ...textRefusalCases()];
  for (const c of all) {
    const d0 = TEE.differences.length;
    let got = 'accepted', why = '';
    try {
      const ctx = { side: c.side, casting: c.casting, iface: c.iface, rival: c.rival ?? null };
      if (c.loader) { const bad = WikiSim.checkText(c.entry); if (bad) throw new Error(bad); }
      else if (c.replay) WikiReplay.play(c.entry, c.entry.demos[0], c.layout, ctx).free();
      else if (c.scripted >= 2) WikiSim.game(c.entry, c.entry.demos[0], c.layout, ctx, c.scripted === 3 ? 'stall' : 'refuse').free();
      else (c.scripted ? WikiSim.play : WikiSim.create)(c.entry, c.entry.demos[0], c.layout, ctx).free();
    } catch (e) { got = 'refused'; why = e.message; }
    const text = WikiText.checkEntry(c.entry), d = teeSince(d0);
    refusals++;
    if (got === c.want && !d && (!text || c.want === 'refused')) console.log(`equal  refusal -- ${c.name}: ${got} by every build`);
    else {
      refuseBad++;
      differ(`refusal -- ${c.name}: ${got}${why ? ` (${why})` : ''}; wanted ${c.want}${text ? `; text.js refuses it (${text})` : ''}${d ? `; ${d}` : ''}`);
    }
  }
});

/* THE DATA READ: every casting and layout (dataItems) read by each build's loader alike, a refused one by its words */
item('data', () => {
  for (const it of dataItems()) {
    const d0 = TEE.differences.length;
    let why = null;
    try { if (it.kind === 'casting') WikiSim.checkCasting(it.v); else WikiSim.checkLayout(it.v); } catch (e) { why = e.message; }
    const d = teeSince(d0);
    datas++;
    if (d) { dataBad++; differ(`data -- ${it.name}: ${d}`); }
    else console.log(`equal  data -- ${it.name}: ${why === null ? 'read alike' : `refused alike (${why})`}`);
  }
});

/* THE MALFORMED DATA the loader must survive, built by tests/wikitest.c's `malformed`, on the sanitizers' build and the
   game's */
item('malformed', () => {
  const C = everyBuild(['malformed'], ['asan', 'wine']);
  if (C.differs) { malformed++; bad++; differ(`malformed -- ${C.differs}`); }
  for (const l of C.out) {
    malformed++;
    if (l.startsWith('ok ')) console.log(`equal  malformed -- ${l.slice(3)}`);
    else { bad++; differ(`malformed -- ${l}`); }
  }
});

/* ---- THIS SHARD'S ITEMS, dealt longest first (the header's THE WORK ITEMS) and played in their order ---- */
function dealt() {
  let costs = null;
  try { costs = JSON.parse(fs.readFileSync(process.env.SIMPARITY_DEAL || COSTS, 'utf8')); } catch (e) { costs = null; }
  if (!costs) return ITEMS.filter((it, i) => i % SHARDS === SHARD - 1);
  const known = ITEMS.map(it => costs[it.key]).filter(Number.isFinite);
  const guess = known.length ? known.reduce((a, b) => a + b, 0) / known.length : 1;
  const order = ITEMS.map((it, i) => [Number.isFinite(costs[it.key]) ? costs[it.key] : guess, i]).sort((a, b) => b[0] - a[0] || a[1] - b[1]);
  const load = new Array(SHARDS).fill(0), mine = [];
  for (const [c, i] of order) {
    let k = 0;
    for (let j = 1; j < SHARDS; j++) if (load[j] < load[k]) k = j;
    load[k] += c;
    if (k === SHARD - 1) mine.push(ITEMS[i]);
  }
  return mine;
}
const spent = {};
try {
  if (!COBS.includes(true)) console.log('note   no gfx pack under tools/wiki/mockup/gfx/ holds a script: the demos play without their units\' scripts only');
  for (const it of dealt()) {
    const t0 = process.hrtime.bigint();
    await it.run();
    spent[it.key] = Number(process.hrtime.bigint() - t0) / 1e9;
  }
} catch (e) {
  if (!(e instanceof CannotRun) && !/^hostchan /.test(e.message)) throw e;
  console.error(`simparity: ${e.message}`);
  process.exitCode = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
failed = playBad + caseBad + refuseBad + dataBad + bad + dupKeys;
if (process.exitCode !== 2 && COSTS_OUT) fs.writeFileSync(COSTS_OUT, JSON.stringify(spent));
if (process.exitCode !== 2 && (RECORD || AGAINST)) {
  const body = [...PLAYED].map(([key, h]) => JSON.stringify(h === 'refused' ? { key, refused: true } : { key, h }) + '\n').join('');
  if (RECORD && !failed) {
    const commit = run('git', ['rev-parse', 'HEAD'], { cwd: ROOT }).trim();
    const header = { commit, gfx: GFX_DIGEST, contexts: CONTEXTS.map(c => c.named) };
    fs.writeFileSync(shardFile(RECORD, SHARD, SHARDS), zlib.gzipSync(JSON.stringify({ header }) + '\n' + body, { level: 9 }));
    console.log(`record ${shardFile(RECORD, SHARD, SHARDS)}: ${PLAYED.size} plays`);
  }
  if (AGAINST && KEYS_OUT) fs.writeFileSync(KEYS_OUT, zlib.gzipSync(body));
  else if (AGAINST) {
    // a shard run by hand holds only the plays it made; a run of every item (--shard 1/1) holds the whole record
    const bad1 = holdTo(readRecord(AGAINST), PLAYED, SHARDS === 1);
    for (const l of bad1) console.log(`DIFFER against -- ${l}`);
    if (bad1.length) failed++;
    else console.log(`equal  against ${AGAINST}: ${PLAYED.size} plays`);
  }
}
if (process.exitCode !== 2) {
  if (SHARDS > 1) process.stdout.write(`shard ${SHARD} of ${SHARDS}: `);
  console.log(failed ? `${playBad} of ${plays} plays, ${caseBad} of ${cases} cases, ${refuseBad} of ${refusals} refusal cases, ${dataBad} of ${datas} data read and ${bad} of ${malformed} malformed-data cases differ (builds: ${TARGETS.join(', ')})`
    : `all ${plays} plays and ${cases} cases alike, request for request, all ${refusals} refusal cases as wanted, all ${datas} castings and layouts read alike and all ${malformed} malformed-data cases loaded (builds: ${TARGETS.join(', ')})`);
  process.exitCode = failed ? 1 : 0;
}
