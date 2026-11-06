/* castcheck.mjs -- the castings' sight figures and the layouts' sight tables against what the game was measured to do
   (research/notes/ingame-wiki.md; the players entry's data).

   usage: node tools/wiki/castcheck.mjs
     exit 0 when every check holds, 1 when one fails, 2 when it cannot run (the stock pack is missing)

   THE FIXTURE'S TABLE. art/wiki/side/fixture.json and fixture-tall.json carry the stock game's sight table, which
   tools/wiki/sidepacks.py writes from the stock pack: each must equal the stock pack's own and pass sim.js's
   checkLayout. A layout stores only the tables its setup's types reach (tagpu_wikidata.h WIKISIGHT), so the stock
   table holds radius indices 3, 6, 7 and 8 -- ARMDRAG's 100, the 210 of ARMSOLAR and the stock structures, ARMCK's
   235 and the 290 of ARMCOM, CORAK's 275 among them -- and is held to the game as measured on those: TABLE8 to the
   scout's statistics of the stock file (332 point visits on 212 offsets, visited 1 / 2 / 3 / 5 / 7 times on 148 / 32 /
   24 / 4 / 4 of them), and M0-P1's lit sets, a unit's table plus its own block: at radius index 8 (ARMCOM, CORAK) 213
   blocks, counts 1 / 2 / 3 / 5 / 7 on 149 / 32 / 24 / 4 / 4; at 7 (ARMCK) 177, on 117 / 24 / 28 / 4 / 4; at 6
   (ARMSOLAR) 137, counts 1 / 2 / 3 / 5 on 93 / 24 / 12 / 8 (MEASURED 2026-11-01, the counters read in game on a flat
   plate). The index is min(trunc(sight / 32), numtables - 1), checked against the stock pack's sightDistance for
   those four types. Every stored table of the stock pack is held to the scout's statements about the whole stock
   file: TABLE i reaches Chebyshev distance i exactly, its offsets are eight-fold symmetric (both negations and the
   swap) and none is the unit's own block -- which is all that holds TABLE3, which nothing measured counted.
   NO LONGER CHECKED: the scout's TABLE1 (8 visits on 8 offsets) and TABLE4 (84 on 60), which no stock type reaches,
   so no layout stores them; nor its TABLE9, which numtables 9 never reaches.

   THE STAMP. sim.js's sightLit on each of the four alone on a flat map lights exactly that set with those counts,
   centred on the sheared row (z - h / 2) >> 5, h = modelHeight + 1 (the stage's altitude, raised to the sea level + 1),
   and lights nothing within h / 2 of the north edge (0x4826A9).

   THE MODEL HEIGHTS. The stock pack's modelHeight for the four types M0-P1 read the def byte +0x170 of: ARMCOM 39,
   ARMCK 27, ARMSOLAR 38, CORAK 26.

   THE CASTINGS. Every pack present: each cast's sightDistance, radarDistance and sonarDistance integers of 16 bits
   and modelHeight a byte (sim.js checkCasting); its sight table present and checkLayout's (every compat setup keeps
   one, MEASURED 2026-11-02), storing exactly the radius indices its castings' bound casts and the tree's players
   entries' own types reach, sight 32 or more (gfx.py `sight_table`), so every type a players game binds has its table.
   The line gives the tables and cells stored.

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

/* every sightDistance a players entry of the tree gives its own types (gfx.py `entry_sights`): with the castings',
   what a layout's table stores */
function playersSights() {
  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  const out = [];
  for (const id of new Set(tree.tree.flatMap(leaves))) {
    const f = path.join(ROOT, 'art/wiki/entries', `${id}.json`);
    const e = fs.existsSync(f) ? JSON.parse(fs.readFileSync(f, 'utf8')) : {};
    if (e.players !== true) continue;
    for (const d of Object.values(e.units || {})) if (Number.isInteger(d.sightDistance) && d.sightDistance >= 32) out.push(d.sightDistance);
  }
  return out;
}

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

  // the scout's statistics of the stock file, on the one table they cover that a stock type reaches
  const table = idx => (sight.tables.find(T => T.idx === idx) || { cells: null }).cells;
  const visits = cells => cells.reduce((n, c) => n + c[2], 0);
  say(same(sight.tables.map(T => T.idx), [3, 6, 7, 8]), `the stock table stores indices ${sight.tables.map(T => T.idx).join(', ')} ` +
    '(the stock castings reach 3, 6, 7, 8)');
  const t8 = table(8);
  say(!!t8 && visits(t8) === 332 && t8.length === 212, `TABLE8: ${t8 ? visits(t8) : '-'} point visits on ${t8 ? t8.length : '-'} offsets ` +
    '(the scout: 332 on 212)');
  say(!!t8 && same(histogram(t8), { 1: 148, 2: 32, 3: 24, 5: 4, 7: 4 }), `TABLE8 visited ${t8 ? JSON.stringify(histogram(t8)) : '-'} ` +
    '(the scout: 1 x 148, 2 x 32, 3 x 24, 5 x 4, 7 x 4)');
  // the scout's statements about every table of the stock file, on each stored one
  for (const T of sight.tables) {
    const key = new Set(T.cells.map(([dc, dr]) => `${dc},${dr}`));
    const reach = Math.max(...T.cells.map(([dc, dr]) => Math.max(Math.abs(dc), Math.abs(dr))));
    const sym = T.cells.every(([dc, dr]) => [[-dc, dr], [dc, -dr], [dr, dc]].every(([a, b]) => key.has(`${a},${b}`)));
    say(reach === T.idx && sym && !key.has('0,0'), `TABLE${T.idx}: reaches ${reach} blocks, ${sym ? '' : 'not '}eight-fold symmetric, ` +
      `${key.has('0,0') ? 'holds' : 'without'} the own block (the scout: reach ${T.idx}, symmetric, without)`);
  }

  // M0-P1's lit sets: the table of the type's index, and its own block once (no table holds the offset 0, 0)
  const casts = stock.casting.flatMap(c => Object.values(c.casts).filter(Boolean));
  const type = t => casts.find(k => k.type === t);
  const MEASURED = { ARMCOM: [8, 213, { 1: 149, 2: 32, 3: 24, 5: 4, 7: 4 }, 39], CORAK: [8, 213, { 1: 149, 2: 32, 3: 24, 5: 4, 7: 4 }, 26],
    ARMCK: [7, 177, { 1: 117, 2: 24, 3: 28, 5: 4, 7: 4 }, 27], ARMSOLAR: [6, 137, { 1: 93, 2: 24, 3: 12, 5: 8 }, 38] };
  for (const [t, [idx, n, hist, height]] of Object.entries(MEASURED)) {
    const k = type(t);
    if (!k) { say(false, `${t}: the stock pack casts no ${t}`); continue; }
    const got = WikiSim.sightIndex(sight, k.sightDistance), T = WikiSim.sightTable(sight, k.sightDistance);
    say(got === idx && !!T, `${t}: sightDistance ${k.sightDistance}, radius index ${got}${T ? ', stored' : ', NOT stored'} (M0-P1: ${idx})`);
    const cells = T ? T.cells : [];
    const own = cells.some(([dc, dr]) => dc === 0 && dr === 0);
    const lit = [...cells, ...(own ? [] : [[0, 0, 1]])];
    say(!own && lit.length === n && same(histogram(lit), hist),
      `${t}: lights ${lit.length} blocks, counts ${JSON.stringify(histogram(lit))} (M0-P1: ${n}, ${JSON.stringify(hist)})`);
    say(k.modelHeight === height, `${t}: modelHeight ${k.modelHeight} (M0-P1: ${height})`);
    // sim.js's stamp (sightLit) on the type alone on a flat 128 x 128 map: the same set and counts, on the sheared row
    const h = Math.min(255, k.modelHeight + 1), z = 1040 + (h >> 1), one = (zz) => ({ map: { w: 128, h: 128 },
      units: [{ owner: 0, type: t, x: 1040, z: zz, built: 1 }], game: { deaths: [], types: [{ name: t, sightDistance: k.sightDistance,
        radarDistance: 0, sonarDistance: 0, activates: false, modelHeight: k.modelHeight }] } });
    const grid = WikiSim.sightLit(one(z), sight, 0, 0), got2 = [];
    grid.forEach((b, i) => { if (b) got2.push([(i % 64) - 32, Math.floor(i / 64) - 32, b]); });
    const want2 = lit.map(([dc, dr, n]) => [dc, dr, n]).sort((a, b) => a[1] - b[1] || a[0] - b[0]);
    say(same(got2, want2), `${t}: sightLit at z ${z} (h ${h}) lights its table and its own block about row ${(z - (h >> 1)) >> 5}`);
    say(WikiSim.sightLit(one((h >> 1) - 1), sight, 0, 0).every(b => b === 0), `${t}: sightLit within h / 2 of the north edge lights nothing`);
  }

  // every pack's castings and table under sim.js's bounds, its table storing exactly the indices its types reach
  const entrySights = playersSights();
  for (const mod of fs.readdirSync(PACKS).sort()) {
    const file = path.join(PACKS, mod, 'side.json');
    if (!fs.existsSync(file)) continue;
    const P = JSON.parse(fs.readFileSync(file, 'utf8'));
    const S = P.layout.sight;
    let why = null;
    try {
      WikiSim.checkLayout(P.layout);
      for (const C of P.casting) WikiSim.checkCasting(C);
      for (const C of P.casting) for (const [c, k] of Object.entries(C.casts)) {
        if (!k) continue;
        for (const n of ['sightDistance', 'radarDistance', 'sonarDistance', 'modelHeight', 'activatewhenbuilt'])
          if (!(n in k)) throw new Error(`${C.side} ${c}: no ${n}`);
      }
      if (!S) {
        const none = (P.report || []).find(r => r && r.element === 'sight table' && r.refused);
        throw new Error(`no sight table: ${none ? none.refused : 'no report line'}`);
      }
      const sights = [...entrySights, ...P.casting.flatMap(C => Object.values(C.casts).filter(Boolean).map(k => k.sightDistance))];
      const want = [...new Set(sights.map(s => WikiSim.sightIndex(S, s)).filter(i => i > 0))].sort((a, b) => a - b);
      if (!same(S.tables.map(T => T.idx), want)) throw new Error(`its table stores ${S.tables.map(T => T.idx)}, its types reach ${want}`);
    } catch (e) { why = e.message; }
    say(why === null, `pack ${mod}: every cast's sight figures and its layout's sight table read` +
      (why ? `: ${why}` : ` (numtables ${S.numtables}; indices ${S.tables.map(T => T.idx).join(', ')}; ` +
        `${S.tables.reduce((n, T) => n + T.cells.length, 0)} cells)`));
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
