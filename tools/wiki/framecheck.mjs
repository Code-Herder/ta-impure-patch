/* framecheck.mjs -- the game frame's record, HUD state, cursor and presses: tools/wiki/mockup/frame.js against its
   port, tagpu/ddraw/src/tagpu_wikiframe.c (research/notes/ingame-wiki-replay-plan.md, the presentation's section 6.2).

   usage: node tools/wiki/framecheck.mjs [--js] [--shard K/N]
     exit 0 when every check holds, 1 when one fails, 2 when it cannot run; --js runs the JavaScript halves alone
     (the idle-side pin and the claims), building nothing; --shard K/N plays every Nth context from the Kth, as
     simparity.mjs does, so that shards run side by side

   THE C HALF is tagpu/ddraw/tests/wikiframetest.c, built natively with the sanitizers over tagpu_wikiframe.c,
   tagpu_wikireplay.c, tagpu_wikisim.c, tagpu_hudlay.c and tools/hudart/hudstatejson.c (laycheck.c's reader).

   THE FRAMES. Every demo of every frame:game entry in every context of simparity.mjs's that runs without the units'
   scripts (the layouts fixture, fixture-tall and none x no casting and each of casting-fixture.json's x interface
   types 1 and 0), its viewer's script played through replay.js and through the C replay, at demo tick 0, every 15th
   tick, the last, and each tick the viewer's clock stands at a trip's start, its press or its release; and Try it
   yourself on the entry's first demo, every tick, with TRY's pointer events through both press machines. Per tick,
   C and JS must agree on:
   1. the frame record, tagpu_wikiframe_record's against record()'s, field for field (a float as a float: the record
      carries floats);
   2. the HUD state: hudState() of the record, read by hudstatejson.c as laycheck.c reads the mockup's, against
      tagpu_wikiframe_hudstate's, field for field;
   3. the cursor's point: point() over C's own laid-out HUD controls against tagpu_wikiframe_point, to 1e-9 units,
      with the rest point and the question's No each side computes for the frame fitted into stagecheck.mjs's
      1920 x 1080 stage (stage.js fit and frame.js dialog against tagpu_wikistage_fit and tagpu_wikiframe_dialog).
   The filtered view (frame.js view against tagpu_wikiframe_view) is stagecheck.mjs's frame mode, which draws it.

   THE TABLES. glide() against tagpu_wikiframe_glide to 1e-12; the press machine on PRESSES' event sequences; the
   seat colours of every pack present, seatColour() against tagpu_wikiframe_seat_colour from one before its first
   badge frame to two past its last; and wikiframetest's selftest: tagpu_wikiframe_bound on hostile records (every
   count past its cap, unterminated strings, unknown keys, NaN and infinite floats, out-of-range ticks, indices and
   glides), the HUD built from each, and tagpu_huddraw_partition on synthetic batch lists with groups in lists 0, 3,
   4 and 6.

   THE IDLE SIDE. frame.js idleSide is a second copy of sim.js gadgetState(g, null, false), so it is pinned: for
   every layout side of the fixture layouts and of every pack present, idleSide's states equal those a fresh
   WikiSim.create reports on that side's MAIN2 after one step with nothing selected: the first tree entry's first
   demo with its selection and its inputs taken out. Every MAIN2 of the layouts present has no gadget (the engine's
   MAIN2 is the bare column), which would pin nothing, so the pin is also run with each side's MAIN2 made each screen
   of its layout in turn: the rule then meets every gadget role the layouts hold. tagpu_wikiframe_idle_side's states
   must equal idleSide's on the layouts as they are.

   THE CLAIMS.
   - every frame:game demo's frame is at least 960 units wide, Wg 720 / Hg >= 960 (the HUD fits at 4:3, MEASURED
     at 1024 x 768 in hud-in-game.md), with no column and with every layout side's, the packs' too;
   - Wg - 75 > 129 + the widest PANELTOP of the packs present, for every frame:game demo's map: the wiki emblem
     never covers a readout (the presentation's section 3.4);
   - every viewer `hud` key and every track tick of a trip resolves in C's laid-out HUD: the key has a control box
     and the tick lies on the track. The boxes are laid out in units from the state alone (tagpu_hudlay.c reads the
     state's px only for the hint's hit test), so a key that resolves at the one fit checked resolves on every
     screen's theatre stage;
   - the HUD's chart never appears: no HOP_CHART in any build. */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSim = require('./mockup/sim.js');
const WikiReplay = require('./mockup/replay.js');
const WikiFrame = require('./mockup/frame.js');
const WikiStage = require('./mockup/stage.js');
const WikiSide = require('./mockup/side.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const PACKS = path.join(ROOT, 'tools/wiki/mockup/gfx/side');
const HUDDIR = path.join(ROOT, 'art/hud');
const JS_ONLY = process.argv.includes('--js');
const SHARD_ARG = process.argv.includes('--shard') ? process.argv[process.argv.indexOf('--shard') + 1] : '1/1';
const [SHARD, SHARDS] = (/^(\d+)\/(\d+)$/.exec(SHARD_ARG) || []).slice(1).map(Number);
if (!(SHARDS >= 1 && SHARD >= 1 && SHARD <= SHARDS)) {
  console.error(`framecheck: --shard ${SHARD_ARG} is not K/N with 1 <= K <= N`);
  process.exit(2);
}
// the stage the frame is fitted into, wikiframetest.c's FIT_W, FIT_H and FIT_U: stagecheck.mjs's 1920 x 1080
const FIT = { W: 1045, H: 532, U: 1.5 };
const SIDE = name => JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/side/${name}.json`), 'utf8'));
const FIXTURE = JSON.parse(fs.readFileSync(path.join(ROOT, 'tools/wiki/casting-fixture.json'), 'utf8'));
const CASTINGS = FIXTURE.castings, RIVALS = FIXTURE.rivals;
const GAMES = fs.readdirSync(path.join(ROOT, 'art/wiki/entries')).filter(f => f.endsWith('.json')).sort()
  .map(f => [f.slice(0, -5), JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries', f), 'utf8'))])
  .filter(([, e]) => e.frame === 'game');

/* TRY: Try it yourself's pointer events, [demo tick, op, src (1 the HUD, 2 the question), key, right, T], each made
   after that tick's frame: play pressed and released on it, speed, a press on fwd released on rew (nothing), the
   track pressed, dragged twice (the second at the same tick: nothing) and released elsewhere, a right press and its
   release (nothing), uiscale (no control: nothing), a bookmark, Leave and its question's body (nothing), Yes released
   on No (nothing), the frame outside it (No at once), Leave again and No, Leave a third time and Yes, which leaves */
const TRY = { end: 150, events: [[5, 'down', 1, 'play', false, 0], [6, 'up', 1, 'play', false, 0], [10, 'down', 1, 'speed', false, 0],
  [11, 'up', 1, 'speed', false, 0], [15, 'down', 1, 'fwd', false, 0], [16, 'up', 1, 'rew', false, 0], [20, 'down', 1, 'track', false, 300],
  [21, 'drag', 1, 'track', false, 600], [22, 'drag', 1, 'track', false, 600], [23, 'up', 1, 'play', false, 700],
  [30, 'down', 1, 'bookmark', true, 0], [31, 'up', 1, 'bookmark', false, 0], [35, 'down', 1, 'uiscale', false, 0],
  [36, 'up', 1, 'uiscale', false, 0], [40, 'down', 1, 'bookmark', false, 0], [41, 'up', 1, 'bookmark', false, 0],
  [50, 'down', 1, 'leave', false, 0], [51, 'up', 1, 'leave', false, 0], [55, 'down', 2, 'body', false, 0], [56, 'up', 2, 'body', false, 0],
  [60, 'down', 2, 'yes', false, 0], [61, 'up', 2, 'no', false, 0], [65, 'down', 2, 'outside', false, 0], [66, 'up', 2, 'outside', false, 0],
  [70, 'down', 1, 'leave', false, 0], [71, 'up', 1, 'leave', false, 0], [75, 'down', 2, 'no', false, 0], [76, 'up', 2, 'no', false, 0],
  [80, 'down', 1, 'leave', false, 0], [81, 'up', 1, 'leave', false, 0], [90, 'down', 2, 'yes', false, 0], [91, 'up', 2, 'yes', false, 0]] };

/* PRESSES: the press machine's sequences, [op, src, key, right, T]: a button pressed and released on itself and on
   another; the track pressed, dragged (once to the same tick) and released at another tick and at its last; a right
   press; a second press while one is held; a drag with nothing held; the question's four keys, and Yes released on
   No; keys that are no control (uiscale, a row, leave:yes from the HUD, the empty key); leave:no from the HUD */
const PRESSES = [
  [['down', 1, 'play', false, 0], ['up', 1, 'play', false, 0]],
  [['down', 1, 'speed', false, 0], ['up', 1, 'rew', false, 0], ['up', 1, 'speed', false, 0]],
  [['down', 1, 'track', false, 100], ['drag', 1, '', false, 100], ['drag', 1, '', false, 250], ['up', 1, '', false, 300]],
  [['down', 1, 'track', false, 100], ['drag', 1, '', false, 400], ['up', 1, '', false, 400]],
  [['down', 1, 'play', true, 0], ['up', 1, 'play', false, 0], ['down', 1, 'track', true, 50]],
  [['down', 1, 'fwd', false, 0], ['down', 1, 'rew', false, 0], ['up', 1, 'rew', false, 0], ['up', 1, 'fwd', false, 0]],
  [['drag', 1, '', false, 10], ['up', 1, 'track', false, 10]],
  [['down', 2, 'yes', false, 0], ['up', 2, 'yes', false, 0], ['down', 2, 'no', false, 0], ['up', 2, 'no', false, 0],
    ['down', 2, 'outside', false, 0], ['up', 2, 'outside', false, 0], ['down', 2, 'body', false, 0], ['up', 2, 'body', false, 0],
    ['down', 2, 'yes', false, 0], ['up', 2, 'no', false, 0]],
  [['down', 1, 'uiscale', false, 0], ['up', 1, 'uiscale', false, 0], ['down', 1, 'row:3', false, 0], ['up', 1, 'row:3', false, 0],
    ['down', 1, 'leave:yes', false, 0], ['up', 1, 'leave:yes', false, 0], ['down', 1, '', false, 0], ['up', 1, '', false, 0]],
  [['down', 1, 'leave:no', false, 0], ['up', 1, 'leave:no', false, 0], ['down', 1, 'bookmark', false, 0], ['up', 1, 'bookmark', false, 0]],
];

class CannotRun extends Error {}
function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { maxBuffer: 1 << 30, encoding: 'utf8', ...opts });
  if (r.error || r.status !== 0)
    throw new CannotRun(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ` (${r.status})`}\n${r.stderr || ''}`);
  return r.stdout;
}
const lines = out => out.split('\n').filter(l => l.length).map(l => JSON.parse(l));

// a pack as barcheck.mjs readPack reads it
function readPack(dir) {
  const json = JSON.parse(fs.readFileSync(path.join(dir, 'side.json'), 'utf8')), A = json.art || {};
  const bin = f => (f && fs.existsSync(path.join(dir, f)) ? new Uint8Array(fs.readFileSync(path.join(dir, f))) : null);
  const fonts = {};
  for (const b of Array.isArray(A.bar) ? A.bar : []) if (b && typeof b.font === 'string' && !b.font.includes('..')) fonts[b.font] = bin(b.font);
  return { json, ...WikiSide.read(json, bin(A.sheet && A.sheet.file), bin(A.palette), bin(A.minimap && A.minimap.seen), bin(A.greyed), fonts) };
}
const PACKLIST = fs.existsSync(PACKS) ? fs.readdirSync(PACKS).sort().filter(m => fs.existsSync(path.join(PACKS, m, 'side.json')))
  .map(m => [m, readPack(path.join(PACKS, m))]) : [];

/* the first difference between C's `c` and JS's `j` under `at`, or null: a number equal as a float (the record and
   the state carry floats; a JavaScript NaN is C's null), anything else exactly */
function diff(c, j, at) {
  if (typeof j === 'number' && Number.isNaN(j)) j = null;
  if (typeof c === 'number' && typeof j === 'number') return c === j || Math.fround(c) === Math.fround(j) ? null : { at, c, j };
  if (c === null || j === null || typeof c !== 'object' || typeof j !== 'object') return c === j ? null : { at, c, j };
  if (Array.isArray(c) !== Array.isArray(j)) return { at, c, j };
  for (const k of new Set([...Object.keys(c), ...Object.keys(j)])) {
    const d = diff(c[k], j[k], `${at}.${k}`);
    if (d) return d;
  }
  return null;
}
const show = v => JSON.stringify(v);

/* the trip ticks of a replay view: the clock at a trip's start, its press or its release (wikiframetest.c chosen) */
function chosen(rv) {
  if (rv.tick === 0 || rv.done || rv.tick % 15 === 0) return true;
  const c = rv.cursor;
  if (!c) return false;
  const rel = c.drag ? c.at + WikiReplay.HUD_DOWN + c.drag.over : c.at + WikiReplay.HUD_UP;
  return rv.clock === c.at || rv.clock === c.at + WikiReplay.HUD_DOWN || rv.clock === rel;
}

/* one frame's JS answers, from the replay `R` in its layout and the press machine's held control (Try it) */
function frameJs(R, layout, tryIt, held) {
  const rv = R.view(), gv = R.game ? R.game.view() : null;
  const rec = WikiFrame.record(rv, gv, 0, tryIt, held);
  const sd = WikiFrame.idleSide(layout, 0);
  const M = gv ? gv.map : { w: 1, h: 1 }, cell = +M.cell || 16;
  const { Wg, Hg } = WikiFrame.size(M, sd ? sd.h : 0);
  const fit = WikiStage.fit(WikiStage.content({ map: M, frame: 1, side: sd }), FIT.W, FIT.H, FIT.U);
  const snap = v => Math.floor(v + 0.5 + 1 / 64);
  const fx = snap(fit.ox - 128 * fit.s), fy = snap(fit.oz - 32 * fit.s), px = Math.fround(Hg * fit.s / WikiFrame.HUD_H);
  const dlg = WikiFrame.dialog(fit, Wg, Hg);
  const rest = { x: (fit.ox + (+M.w || 1) * cell / 2 * fit.s - fx) / px, y: (fit.oz + (+M.h || 1) * cell / 2 * fit.s - fy) / px };
  const no = { x: (dlg.no.x + dlg.no.w / 2 - fx) / px, y: (dlg.no.y + dlg.no.h / 2 - fy) / px };
  // the game's HUD draws the record's held control down (tagpu_wikiframe_hudstate); the page presses it on the DOM
  const st = { ...WikiFrame.hudState(rec, Wg, Hg, null), held: rec.held };
  return { t: rv.tick, rec, st, px, rest, no, Wg, Hg };
}

let failed = 0, frames = 0, demos = 0, tables = 0;
const fail = msg => { failed++; console.log(`DIFFER ${msg}`); };

/* ---- the idle side ---- */
function layouts() {
  const out = [['fixture', SIDE('fixture'), null], ['fixture-tall', SIDE('fixture-tall'), null]];
  for (const [mod, p] of PACKLIST) if (p.layout) out.push([`pack ${mod}`, p.layout, p.json.casting || null]);
  return out;
}
// the screen and states a fresh sim shows on layout side `k` after one step with nothing selected, or a refusal
function idleSim(layout, casting, k) {
  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${tree.tree.flatMap(leaves)[0]}.json`), 'utf8'));
  const demo = { ...entry.demos[0], scene: { ...(entry.demos[0].scene || {}), selected: [] }, inputs: [] };
  const sim = WikiSim.create({ ...entry, scenes: Object.fromEntries(Object.entries(entry.scenes).map(([n, sc]) => [n, { ...sc, selected: [] }])) },
    demo, layout, { side: k, casting: casting ? casting[k] ?? null : null, iface: 1 });
  sim.step();
  const v = sim.view();
  if (sim.free) sim.free();
  return v.side && v.side.unit === null ? { screen: v.side.screen, state: v.side.state } : null;
}
function idleCheck(exe, tmp) {
  const all = layouts(), files = [];
  all.forEach(([name, L, casting], i) => {
    for (let k = 0; k < L.sides.length; k++)
      for (const screen of [L.sides[k].main2, ...Object.keys(L.screens)]) {
        const M = { ...L, sides: L.sides.map((sd, j) => j === k ? { ...sd, main2: screen } : sd) };
        const js = WikiFrame.idleSide(M, k), at = `idle ${name} side ${k} with MAIN2 ${screen}`;
        let sim;
        try { sim = idleSim(M, casting, k); } catch (e) { fail(`${at}: sim.js refused it: ${e.message}`); continue; }
        tables++;
        if (!sim || sim.screen !== screen) fail(`${at}: the fresh sim shows ${sim ? sim.screen : 'a selection'}, not that MAIN2`);
        else if (show(js.state) !== show(sim.state)) fail(`${at}: frame.js ${show(js.state)}, sim.js ${show(sim.state)}`);
      }
    const f = path.join(tmp, `layout${i}.json`);
    fs.writeFileSync(f, JSON.stringify(L));
    files.push(f);
  });
  if (!exe) return;
  lines(run(exe, ['idle', ...files])).forEach((c, i) => {
    const [name, L] = all[i];
    for (let k = 0; k <= L.sides.length; k++) {
      // C holds MAIN2 by its index among the layout's screens, in the file's order
      const s = WikiFrame.idleSide(L, k), j = s ? [Object.keys(L.screens).indexOf(s.screen), s.h, ...s.state] : null;
      tables++;
      if (show(c[k]) !== show(j)) fail(`idle ${name} side ${k}: C ${show(c[k])}, frame.js ${show(j)}`);
    }
  });
}

/* ---- the claims the JavaScript makes alone ---- */
function claimsJs() {
  const tops = PACKLIST.flatMap(([, p]) => ((p.art && p.art.bar) || []).filter(b => b && b.top).map(b => b.top.f[2]));
  const widest = tops.length ? Math.max(...tops) : 0;
  // the frame's rows are the map's or the column's, whichever is taller: every layout side's column, each pack's too
  const columns = [['no sidebar', 0], ...layouts().flatMap(([name, L]) => L.sides.map((_, k) => [`${name} side ${k}`, WikiFrame.idleSide(L, k).h]))];
  for (const [id, entry] of GAMES)
    for (const demo of entry.demos) {
      let R;
      try { R = WikiReplay.play(entry, demo, null, { side: 0, iface: 1 }); } catch (e) { fail(`claim ${id}/${demo.id}: replay.js refused it: ${e.message}`); continue; }
      const gv = R.game ? R.game.view() : null;
      R.free();
      if (!gv) continue;
      for (const [name, h] of columns) {
        const { Wg, Hg } = WikiFrame.size(gv.map, h);
        tables++;
        if (!(Wg * 720 / Hg >= 960)) fail(`claim ${id}/${demo.id} (${name}): the frame ${Wg} x ${Hg} is ${Wg * 720 / Hg} units wide, under 960`);
        if (!(Wg - 75 > 129 + widest)) fail(`claim ${id}/${demo.id} (${name}): the wiki emblem at ${Wg - 75} covers PANELTOP's ${129 + widest}`);
      }
    }
  return widest;
}

/* ---- the tables ---- */
function tablesCheck(exe, tmp) {
  const G = [];
  for (const [a, b] of [[0, 1], [-3.25, 917.5], [1e3, 1e-3], [5, 5], [123.456, -78.9]])
    for (const n of [1, 2, 6, 7, 20]) for (let i = -1; i <= n + 1; i++) G.push([a, b, i, n]);
  fs.writeFileSync(path.join(tmp, 'glide.txt'), G.map(g => g.join(' ')).join('\n') + '\n');
  const gc = run(exe, ['glide', path.join(tmp, 'glide.txt')]).trim().split('\n').map(Number);
  G.forEach(([a, b, i, n], k) => {
    const j = WikiFrame.glide(a, b, i, n);
    tables++;
    if (!(Math.abs(gc[k] - j) <= 1e-12 * Math.max(1, Math.abs(j)))) fail(`glide(${a}, ${b}, ${i}, ${n}): C ${gc[k]}, frame.js ${j}`);
  });
  fs.writeFileSync(path.join(tmp, 'press.json'), JSON.stringify(PRESSES));
  lines(run(exe, ['press', path.join(tmp, 'press.json')])).forEach((c, s) => {
    const P = WikiFrame.Press();
    const j = PRESSES[s].map(([op, src, key, right, T]) => {
      const r = op === 'down' ? P.down(src === 2 ? 'dialog' : 'hud', key, right, T) : op === 'drag' ? P.drag(T) : P.up(key, T);
      return r.length ? r[0] : null;
    });
    tables++;
    if (show(c) !== show(j)) fail(`press sequence ${s}: C ${show(c)}, frame.js ${show(j)}`);
  });
  for (const [mod, p] of PACKLIST) {
    const A = p.art;
    if (!A || !A.sheet || !A.pal) continue;
    const dir = path.join(tmp, `colours-${mod}`);
    fs.mkdirSync(dir, { recursive: true });
    // wikisideart.h's shape with the badge alone: the sheet, the palette and the logos
    fs.writeFileSync(path.join(dir, 'art.json'), JSON.stringify({ sheet: [A.sheet.w, A.sheet.h], panelside: [null], bar: [null],
      logos: A.logos, gui: null }));
    fs.writeFileSync(path.join(dir, 'sheet.bin'), A.sheet.px);
    fs.writeFileSync(path.join(dir, 'pal.bin'), A.pal);
    fs.writeFileSync(path.join(dir, 'fnt.bin'), Buffer.alloc(0));
    const c = JSON.parse(run(exe, ['colours', dir]));
    const hex = s => s.startsWith('#') ? s : '#' + s.slice(4, -1).split(',').map(n => (+n).toString(16).padStart(2, '0')).join('');
    const j = [];
    for (let k = -1; k <= (A.logos ? A.logos.length : 0) + 2; k++) j.push(hex(WikiFrame.seatColour(A, k)));
    tables++;
    if (show(c) !== show(j)) fail(`seat colours of ${mod}: C ${show(c)}, frame.js ${show(j)}`);
  }
  const self = spawnSync(exe, ['selftest', HUDDIR], { encoding: 'utf8' });
  for (const l of self.stdout.split('\n').filter(Boolean)) { tables++; if (!l.startsWith('ok')) fail(`selftest: ${l}`); }
  if (self.status !== 0 && self.status !== 1) throw new CannotRun(`wikiframetest selftest failed (${self.status})\n${self.stderr}`);
}

/* ---- the frames ---- */
function framesCheck(exe, tmp) {
  run('python3', [path.join(ROOT, 'tools/hudpak.py'), '--set', 'wiki', path.join(ROOT, 'art/wiki'), path.join(tmp, 'wiki.bin')]);
  for (const name of Object.keys(CASTINGS)) {
    fs.writeFileSync(path.join(tmp, `casting-${name}.json`), JSON.stringify(CASTINGS[name][0]));
    fs.writeFileSync(path.join(tmp, `rival-${name}.json`), JSON.stringify(RIVALS[name]));
  }
  fs.writeFileSync(path.join(tmp, 'try.json'), JSON.stringify(TRY));
  let context = 0;
  for (const side of ['fixture', 'fixture-tall', null]) for (const cname of [null, ...Object.keys(CASTINGS)]) for (const iface of [1, 0]) {
    if (context++ % SHARDS !== SHARD - 1) continue;
    const layout = side && SIDE(side), named = `${side || 'no sidebar'}, ${cname || 'own types'}, type ${iface}`;
    const ctx = { side: 0, casting: cname ? CASTINGS[cname][0] : null, iface, cob: null, snd: null, rival: cname ? RIVALS[cname] : null };
    for (const [id, entry] of GAMES) {
      fs.writeFileSync(path.join(tmp, 'game.json'), JSON.stringify(entry));
      const cout = lines(run(exe, ['frames', path.join(tmp, 'wiki.bin'), path.join(tmp, 'game.json'), side || '-',
        cname ? path.join(tmp, `casting-${cname}.json`) : '-', cname ? path.join(tmp, `rival-${cname}.json`) : '-', String(iface), HUDDIR,
        path.join(tmp, 'try.json')]));
      const js = [];
      const play = (demo, name, make, presses) => {
        let R;
        try { R = make(); } catch (e) { js.push({ e: id, d: name, refused: e.message }); return; }
        const P = WikiFrame.Press();
        for (;;) {
          const rv = R.view();
          if (presses || chosen(rv)) js.push({ e: id, d: name, ...frameJs(R, layout, !!presses, P.held) });
          if (rv.done || (presses && R.tick >= presses.end)) break;
          if (presses)
            for (const [t, op, src, key, right, T] of presses.events) {
              if (t !== R.tick) continue;
              const r = op === 'down' ? P.down(src === 2 ? 'dialog' : 'hud', key, right, T) : op === 'drag' ? P.drag(T) : P.up(key, T);
              for (const [c, v] of r) R.press(c, v);
            }
          R.step();
        }
        R.free();
      };
      for (const demo of entry.demos) play(demo, demo.id, () => WikiReplay.play(entry, demo, layout, ctx), null);
      play(entry.demos[0], `${entry.demos[0].id}/try`, () => WikiReplay.create(entry, entry.demos[0], layout, ctx), TRY);
      // the JS states through C's reader, one line each
      const states = js.filter(f => !f.refused);
      fs.writeFileSync(path.join(tmp, 'states.jsonl'), states.map(f => JSON.stringify({ st: f.st, px: f.px })).join('\n') + '\n');
      const read = lines(run(exe, ['readstates', HUDDIR, path.join(tmp, 'states.jsonl')]));
      states.forEach((f, i) => { f.read = read[i]; });
      let first = null, n = 0;
      const seen = new Set();
      if (cout.length !== js.length) first = `the C side made ${cout.length} frames, frame.js ${js.length}`;
      for (let i = 0; i < Math.min(cout.length, js.length) && !first; i++) {
        const c = cout[i], j = js[i], where = `${id}/${j.d} at demo tick ${j.t}`;
        seen.add(j.d);
        if (c.e !== j.e || c.d !== j.d || ('refused' in c) !== ('refused' in j)) { first = `${where}: C ${c.d} ${c.refused ? 'refused' : 'played'}, frame.js ${j.d} ${j.refused ? 'refused' : 'played'}`; break; }
        if (j.refused) continue;
        n++;
        if (c.t !== j.t) { first = `${j.e}/${j.d}: C's frame ${i} is at demo tick ${c.t}, frame.js's ${j.t}`; break; }
        let d = diff(c.rec, j.rec, 'rec') || diff(c.st, j.read, 'state') || diff(c.px, j.px, 'px');
        if (!d) for (const [k, a, b] of [['rest', c.rest, j.rest], ['no', c.no, j.no]])
          if (Math.abs(a[0] - b.x) > 1e-9 || Math.abs(a[1] - b.y) > 1e-9) { d = { at: k, c: a, j: [b.x, b.y] }; break; }
        if (d) { first = `${where}: ${d.at}: C ${show(d.c)}, frame.js ${show(d.j)}`; break; }
        // C's boxes, its ends and the px are floats, printed to 9 digits: fround gives back the float itself
        const F = Math.fround, boxes = c.boxes.map(([key, x, y, w, h]) => ({ key, ub: [F(x), F(y), F(w), F(h)], ends: F(c.ends) }));
        const pt = j.rec.cur.on ? WikiFrame.point(j.rec, boxes, j.px, j.rest, j.no) : null;
        if (pt && (Math.abs(pt.x - c.pt[0]) > 1e-9 || Math.abs(pt.y - c.pt[1]) > 1e-9)) {
          first = `${where}: the cursor: C (${c.pt}), frame.js (${pt.x}, ${pt.y})`;
          break;
        }
        // the claims on C's laid-out HUD
        const cur = j.rec.cur;
        if (c.unresolved) fail(`claim ${where} (${named}): the key ${c.unresolved} has no control in C's HUD`);
        if (cur.on && cur.to === 'track' && !(cur.to_v >= 0 && cur.to_v <= j.rec.end)) fail(`claim ${where} (${named}): the track tick ${cur.to_v} is off the track`);
        if (c.chart) fail(`claim ${where} (${named}): the stage HUD's build has a chart`);
        if (!(c.st.W >= 960)) fail(`claim ${where} (${named}): the HUD is ${c.st.W} units wide, under 960`);
      }
      frames += n;
      demos += seen.size;
      if (first) fail(`${id} (${named}): ${first}`);
      else console.log(`equal  ${id} (${named}): ${n} frames over ${seen.size} replays`);
    }
  }
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'framecheck-'));
let code = 0;
try {
  let exe = null;
  if (!JS_ONLY) {
    exe = path.join(tmp, 'wikiframetest');
    run('gcc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc', `-I${path.join(ROOT, 'tools/hudart')}`, '-O1', '-g',
      '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-o', exe, 'tests/wikiframetest.c', 'src/tagpu_wikiframe.c',
      'src/tagpu_wikireplay.c', 'src/tagpu_wikisim.c', 'src/tagpu_wikidata.c', 'src/tagpu_json.c', 'src/tagpu_pak.c',
      'src/tagpu_placegeo.c', 'src/tagpu_sweepgeo.c', 'src/tagpu_wikicob.c', 'src/tagpu_cobvm.c', 'src/tagpu_cob_safe.c',
      'src/tagpu_hudlay.c', 'src/tagpu_hudtext.c', 'src/tagpu_huddraw.c', 'src/tagpu_wikistage.c',
      path.join(ROOT, 'tools/hudart/hudstatejson.c'), '-lm'], { cwd: DDRAW });
  }
  if (SHARD === 1) {
    const widest = claimsJs();
    idleCheck(exe, tmp);
    if (exe) tablesCheck(exe, tmp);
    console.log(`tables: ${tables} checks (the widest PANELTOP of ${PACKLIST.length} packs ${widest})`);
  }
  if (exe) framesCheck(exe, tmp);
  code = failed ? 1 : 0;
  console.log(failed ? `framecheck: ${failed} failed` : `framecheck: all equal -- ${frames} frames over ${demos} replays${SHARDS > 1 ? ` (shard ${SHARD_ARG})` : ''}, ${tables} table checks`);
} catch (e) {
  if (!(e instanceof CannotRun)) throw e;
  console.error(`framecheck: cannot run: ${e.message}`);
  code = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
process.exit(code);
