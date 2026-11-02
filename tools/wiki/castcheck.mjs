/* castcheck.mjs -- the castings' sight figures and the layouts' sight tables against what the game was measured to do
   (research/notes/ingame-wiki.md; the players entry's data).

   usage: node tools/wiki/castcheck.mjs
     exit 0 when every check holds, 1 when one fails, 2 when it cannot run (the stock pack is missing)

   THE FIXTURE'S TABLE. art/wiki/side/fixture.json and fixture-tall.json carry the stock game's sight table, which
   tools/wiki/sidepacks.py writes from the stock pack: each must equal the stock pack's own, pass sim.js's checkLayout,
   and hold to the game as measured -- the scout's statistics of the stock file (TABLE1: 8 point visits on 8 offsets;
   TABLE4: 84 on 60; TABLE8: 332 on 212, visited 1 / 2 / 3 / 5 / 7 times on 148 / 32 / 24 / 4 / 4 of them) and M0-P1's
   lit sets, a unit's table plus its own block: at radius index 8 (ARMCOM, CORAK) 213 blocks, counts 1 / 2 / 3 / 5 / 7
   on 149 / 32 / 24 / 4 / 4; at 7 (ARMCK) 177, on 117 / 24 / 28 / 4 / 4; at 6 (ARMSOLAR) 137, counts 1 / 2 / 3 / 5 on
   93 / 24 / 12 / 8 (MEASURED 2026-11-01, the counters read in game on a flat plate). The index is min(trunc(sight /
   32), numtables - 1), checked against the stock pack's sightDistance for those four types.

   THE MODEL HEIGHTS. The stock pack's modelHeight for the four types M0-P1 read the def byte +0x170 of: ARMCOM 39,
   ARMCK 27, ARMSOLAR 38, CORAK 26.

   THE CASTINGS. Every pack present: each cast's sightDistance, radarDistance and sonarDistance integers of 16 bits
   and modelHeight a byte (sim.js checkCasting), and its sight table, when it has one, checkLayout's. Which packs keep
   no table, and why, is the pack's report; the line names them.

   THE CHIPS. sim.js's CHIPS, the stats window's chips a `chip` press names, are the window's two splits and
   art/hud/layout.json's window.measures keys, in that order (tagpu_wikidata.h WIKI_CHIP_*, which simparity holds
   equal to CHIPS). */
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSim = require('./mockup/sim.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const PACKS = path.join(ROOT, 'tools/wiki/mockup/gfx/side');
const FIXTURES = ['fixture.json', 'fixture-tall.json'].map(f => path.join(ROOT, 'art/wiki/side', f));

class CannotRun extends Error {}

let failed = 0;
const say = (ok, what) => { if (!ok) failed++; console.log(`${ok ? 'ok    ' : 'FAILED'} ${what}`); };
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
// {count: blocks} of a list of [dc, dr, n] cells
const histogram = cells => cells.reduce((h, [, , n]) => ({ ...h, [n]: (h[n] || 0) + 1 }), {});

function run() {
  const stockFile = path.join(PACKS, 'stock', 'side.json');
  if (!fs.existsSync(stockFile)) throw new CannotRun(`no stock pack at ${stockFile} (tools/wiki/sidepacks.py stock builds it)`);
  const stock = JSON.parse(fs.readFileSync(stockFile, 'utf8'));
  const sight = stock.layout.sight;
  if (!sight) throw new CannotRun('the stock pack has no sight table: rebuild it with tools/wiki/sidepacks.py stock');

  // the fixtures' table is the stock pack's, and each fixture still a layout sim.js takes
  for (const f of FIXTURES) {
    const L = JSON.parse(fs.readFileSync(f, 'utf8'));
    let why = null;
    try { WikiSim.checkLayout(L); } catch (e) { why = e.message; }
    say(why === null, `${path.basename(f)}: checkLayout${why ? `: ${why}` : ''}`);
    say(same(L.sight, sight), `${path.basename(f)}: its sight table is the stock pack's`);
  }

  // the scout's statistics of the stock file
  const table = idx => sight.tables[idx - 1];
  const visits = cells => cells.reduce((n, c) => n + c[2], 0);
  for (const [idx, v, o] of [[1, 8, 8], [4, 84, 60], [8, 332, 212]])
    say(visits(table(idx)) === v && table(idx).length === o,
      `TABLE${idx}: ${visits(table(idx))} point visits on ${table(idx).length} offsets (the scout: ${v} on ${o})`);
  say(same(histogram(table(8)), { 1: 148, 2: 32, 3: 24, 5: 4, 7: 4 }), `TABLE8 visited ${JSON.stringify(histogram(table(8)))} ` +
    '(the scout: 1 x 148, 2 x 32, 3 x 24, 5 x 4, 7 x 4)');

  // M0-P1's lit sets: the table of the type's index, and its own block once (no table holds the offset 0, 0)
  const casts = stock.casting.flatMap(c => Object.values(c.casts).filter(Boolean));
  const type = t => casts.find(k => k.type === t);
  const MEASURED = { ARMCOM: [8, 213, { 1: 149, 2: 32, 3: 24, 5: 4, 7: 4 }, 39], CORAK: [8, 213, { 1: 149, 2: 32, 3: 24, 5: 4, 7: 4 }, 26],
    ARMCK: [7, 177, { 1: 117, 2: 24, 3: 28, 5: 4, 7: 4 }, 27], ARMSOLAR: [6, 137, { 1: 93, 2: 24, 3: 12, 5: 8 }, 38] };
  for (const [t, [idx, n, hist, height]] of Object.entries(MEASURED)) {
    const k = type(t);
    if (!k) { say(false, `${t}: the stock pack casts no ${t}`); continue; }
    const got = Math.min(Math.trunc(k.sightDistance / 32), sight.numtables - 1);
    say(got === idx, `${t}: sightDistance ${k.sightDistance}, radius index ${got} (M0-P1: ${idx})`);
    const cells = table(got);
    const own = cells.some(([dc, dr]) => dc === 0 && dr === 0);
    const lit = [...cells, ...(own ? [] : [[0, 0, 1]])];
    say(!own && lit.length === n && same(histogram(lit), hist),
      `${t}: lights ${lit.length} blocks, counts ${JSON.stringify(histogram(lit))} (M0-P1: ${n}, ${JSON.stringify(hist)})`);
    say(k.modelHeight === height, `${t}: modelHeight ${k.modelHeight} (M0-P1: ${height})`);
  }

  // every pack's castings and table under sim.js's bounds
  for (const mod of fs.readdirSync(PACKS).sort()) {
    const file = path.join(PACKS, mod, 'side.json');
    if (!fs.existsSync(file)) continue;
    const P = JSON.parse(fs.readFileSync(file, 'utf8'));
    let why = null;
    try {
      WikiSim.checkLayout(P.layout);
      for (const C of P.casting) WikiSim.checkCasting(C);
      for (const C of P.casting) for (const [c, k] of Object.entries(C.casts)) {
        if (!k) continue;
        for (const n of ['sightDistance', 'radarDistance', 'sonarDistance', 'modelHeight', 'activatewhenbuilt'])
          if (!(n in k)) throw new Error(`${C.side} ${c}: no ${n}`);
      }
    } catch (e) { why = e.message; }
    const none = (P.report || []).find(r => r && r.element === 'sight table' && r.refused);
    say(why === null, `pack ${mod}: every cast's sight figures and its layout's sight table read` +
      (why ? `: ${why}` : P.layout.sight ? ` (${P.layout.sight.numtables} tables)` : ` (no table: ${none ? none.refused : 'no report line'})`));
  }

  // the chips: the window's splits, then its measures
  const lay = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/hud/layout.json'), 'utf8')).window;
  const chips = [...lay.split.chips.map(c => c.value), ...lay.measures.map(m => m.k)];
  say(same(chips, WikiSim.CHIPS), `CHIPS ${WikiSim.CHIPS.join(', ')} (art/hud/layout.json: ${chips.join(', ')})`);
}

try {
  run();
  console.log(failed ? `castcheck: ${failed} check(s) FAILED` : 'castcheck: every check holds');
  process.exitCode = failed ? 1 : 0;
} catch (e) {
  if (!(e instanceof CannotRun)) throw e;
  console.error(`castcheck: CANNOT RUN: ${e.message}`);
  process.exitCode = 2;
}
