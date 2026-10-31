/* barcheck.mjs -- the game frame's bars (tools/wiki/mockup/bar.js) against the game's own pixels.

   usage: node tools/wiki/barcheck.mjs --js [--pack DIR]
     exit 0 when every check holds, 1 when one fails, 2 when it cannot run

   THE CAPTURE. research/notes/assets/hud-art/backdrop.png is a 1280 x 720 replay frame of our renderer at full map:
   the viewer's record, ARM, colour 4, every number 0 and both storages 0 (MEASURED: the badge matches colour 4's
   frame alone, and the bar the zeros' rules; research/notes/ingame-wiki-replay-plan.md, *What R0 found*). bar.js
   composes the stock pack's ARM bars for that frame -- Wg 1280, Hg 720, the numbers all 0 -- and two bands of the
   capture are compared with them pixel for pixel, through the pack's palette:
   - rows 0..31 at frame x 128..1279, the top bar and PANELSIDE's column 128 beside it. Every pixel must be equal
     except inside x 1237..1262, y 3..28, Impure's sprocket (tagpu_menu.c trigger_rect), which the engine does not
     draw;
   - rows 688..719 at x 128..1279, the bottom bar. Every pixel must be equal except inside x 132..290, y 693..714:
     the capture was taken with the pointer over a unit, and the engine draws that unit's readout there -- a badge,
     the name "Peewee" and its health bar, MEASURED by eye on the capture and by the differing pixels, all 771 of
     which lie in that box -- while the stage draws the bare strip, nothing being hovered.
   A difference anywhere else fails. The stock pack is the one tools/wiki/sidepacks.py builds from the player's
   install (gitignored); without it, or without its bars, the check cannot run (exit 2). It never skips.

   THE READER. side.js read() on synthetic packs (no game data): a well-formed one reads every bar, the badge and
   the GUI table; each damaged one -- a frame outside the sheet, a missing or inverted rect, a coordinate past its
   bound, a colour past 255, a font that is short, has 0 or 33 rows, a first code other than 0, a glyph offset or
   bits past the file, a glyph 0 or 33 wide, too many badge frames, a GUI table of the wrong length -- drops exactly
   that element to null and says so in a problem line, and bar.js composes nothing for a side without a bar.

   The C half (tagpu_wikibar.c against bar.js) needs the DLL's composer and is run without --js; this tree does
   not have it. */
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSide = require('./mockup/side.js');
const WikiBar = require('./mockup/bar.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const CAPTURE = path.join(ROOT, 'research/notes/assets/hud-art/backdrop.png');
const opt = name => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : null; };
const PACK = opt('--pack') || path.join(ROOT, 'tools/wiki/mockup/gfx/side/stock');

class CannotRun extends Error {}

// an 8-bit RGB or RGBA, non-interlaced PNG as { w, h, px: RGB bytes }
function readPng(file) {
  const b = fs.readFileSync(file), idat = [];
  let w = 0, h = 0, bpp = 0;
  for (let o = 8; o < b.length;) {
    const n = b.readUInt32BE(o), type = b.toString('latin1', o + 4, o + 8), d = b.subarray(o + 8, o + 8 + n);
    if (type === 'IHDR') {
      w = d.readUInt32BE(0); h = d.readUInt32BE(4);
      if (d[8] !== 8 || (d[9] !== 2 && d[9] !== 6) || d[12] !== 0) throw new CannotRun(`${file}: not 8-bit RGB(A), non-interlaced`);
      bpp = d[9] === 2 ? 3 : 4;
    } else if (type === 'IDAT') idat.push(d);
    o += 12 + n;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat)), stride = w * bpp, px = new Uint8Array(w * h * 3);
  let prev = new Uint8Array(stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)), cur = new Uint8Array(stride);
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? cur[i - bpp] : 0, up = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      let p = line[i];
      if (f === 1) p += a;
      else if (f === 2) p += up;
      else if (f === 3) p += (a + up) >> 1;
      else if (f === 4) {
        const q = a + up - c, pa = Math.abs(q - a), pb = Math.abs(q - up), pc = Math.abs(q - c);
        p += pa <= pb && pa <= pc ? a : pb <= pc ? up : c;
      }
      cur[i] = p & 0xff;
    }
    for (let x = 0; x < w; x++) for (let k = 0; k < 3; k++) px[(y * w + x) * 3 + k] = cur[x * bpp + k];
    prev = cur;
  }
  return { w, h, px };
}

// a pack directory as side.js reads it, the bars' fonts included
function readPack(dir) {
  const json = JSON.parse(fs.readFileSync(path.join(dir, 'side.json'), 'utf8')), A = json.art || {};
  const bin = f => (f && fs.existsSync(path.join(dir, f)) ? new Uint8Array(fs.readFileSync(path.join(dir, f))) : null);
  const fonts = {};
  for (const b of Array.isArray(A.bar) ? A.bar : []) if (b && typeof b.font === 'string' && !b.font.includes('..')) fonts[b.font] = bin(b.font);
  return { json, ...WikiSide.read(json, bin(A.sheet && A.sheet.file), bin(A.palette), bin(A.minimap && A.minimap.seen), bin(A.greyed), fonts) };
}

/* ---- the capture ---- */
const SPROCKET = [1237, 3, 1262, 28];       // x0, y0, x1, y1 inclusive, frame pixels
const READOUT = [132, 693, 290, 714];
function captureCheck(lines) {
  if (!fs.existsSync(path.join(PACK, 'side.json'))) throw new CannotRun(`no pack at ${PACK} (tools/wiki/sidepacks.py stock builds it)`);
  const p = readPack(PACK);
  if (p.json.mod !== 'stock') throw new CannotRun(`${PACK} is the ${p.json.mod} pack; the capture is retail's`);
  if (!p.art) throw new CannotRun(`side.js reads no art from ${PACK}: ${p.problems.join('; ')}`);
  const side = (p.layout.sides || []).findIndex(s => s.name === 'ARM');
  if (side < 0 || !p.art.bar[side]) throw new CannotRun(`the stock pack has no ARM bar: ${p.problems.join('; ') || 'rebuild it with tools/wiki/sidepacks.py stock'}`);
  const img = readPng(CAPTURE), Wg = 1280, Hg = 720;
  if (img.w !== Wg || img.h !== Hg) throw new CannotRun(`${CAPTURE} is ${img.w} x ${img.h}, not ${Wg} x ${Hg}`);
  const c = WikiBar.compose(p.art, side, 4, Wg, Hg, WikiBar.ZERO);
  if (!c) throw new CannotRun('bar.js composes no bar for the stock ARM side');
  const pal = p.art.pal, inside = (b, x, y) => x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3];
  let failed = 0;
  for (const [name, row0, box, boxName] of [['top bar', 0, SPROCKET, 'the sprocket'], ['bottom bar', Hg - 32, READOUT, 'the hovered unit\'s readout']]) {
    let same = 0, inBox = 0, stray = 0, first = null;
    for (let r = 0; r < 32; r++) for (let x = 0; x < c.w; x++) {
      const fx = WikiBar.COL + x, fy = row0 + r, q = c.idx[(32 * (row0 ? 1 : 0) + r) * WikiBar.BAR_W + x] * 4, o = (fy * Wg + fx) * 3;
      if (pal[q] === img.px[o] && pal[q + 1] === img.px[o + 1] && pal[q + 2] === img.px[o + 2]) same++;
      else if (inside(box, fx, fy)) inBox++;
      else { stray++; first = first || `(${fx}, ${fy}): bar.js ${pal[q]},${pal[q + 1]},${pal[q + 2]}, the capture ${img.px[o]},${img.px[o + 1]},${img.px[o + 2]}`; }
    }
    const total = 32 * c.w;
    if (stray) { failed++; lines.push(`  ${name}: ${stray} pixels DIFFER outside ${boxName}, the first at ${first}`); }
    else lines.push(`  ${name} (rows ${row0}..${row0 + 31}, x 128..${Wg - 1}): ${same} of ${total} pixels equal, the other ${inBox} inside ${boxName}`);
  }
  return failed;
}

/* ---- the reader ---- */
function syntheticPack() {
  const SW = 64, SH = 64, sheet = new Uint8Array(SW * SH).map((_, i) => (i * 7) & 0xff), pal = new Uint8Array(1024).map((_, i) => i & 0xff);
  const rects = {};
  for (const [n, r] of Object.entries({ logo: [132, 5, 152, 25], energybar: [471, 12, 598, 14], energynum: [529, 18, 529, 18],
    energymax: [595, 1, 595, 1], energy0: [468, 1, 468, 1], metalbar: [218, 12, 345, 14], metalnum: [278, 18, 278, 18],
    metalmax: [341, 1, 341, 1], metal0: [215, 1, 215, 1], totalunits: [560, 7, 583, 15], totaltime: [605, 7, 628, 15],
    energyproduced: [609, 5, 609, 5], energyconsumed: [609, 17, 609, 17], metalproduced: [358, 5, 358, 5],
    metalconsumed: [358, 17, 358, 17] })) rects[n] = r;
  // a font: 3 rows, offset 1, first code 0; every code 0x20..0x7e a 2-wide glyph of one byte of bits
  const font = new Uint8Array(4 + 512 + 95 * 2);
  font[0] = 3; font[2] = 1;
  for (let c = 0x20; c <= 0x7e; c++) { const o = 516 + (c - 0x20) * 2; font[4 + 2 * c] = o & 0xff; font[5 + 2 * c] = o >> 8; font[o] = 2; font[o + 1] = 0xa5; }
  const json = { format: 1, layout: { sides: [{ name: 'A' }, { name: 'B' }], screens: {} },
    art: { sheet: { file: 'sheet.bin', w: SW, h: SH }, palette: 'pal.bin', panelside: [{ f: [0, 0, 10, 10, 0, 0], key: 9 }, null], screens: {},
      dots: { radlogo: [[0, 0, 4, 4, 2, 2]], key: 0 },
      bar: [{ top: { f: [0, 0, 40, 32, 1, 0], key: 9 }, bot: { f: [0, 32, 30, 32, 0, 0], key: 9 }, rects, ecol: 208, mcol: 224, font: 'fnt/f.bin' }, null],
      logos: [{ f: [40, 0, 8, 8, 0, 0], key: 9 }, { f: [48, 0, 8, 8, 0, 0], key: 9 }],
      gui: Array.from({ length: 256 }, (_, i) => 255 - i) } };
  return { json, sheet, pal, fonts: { 'fnt/f.bin': font } };
}
function readerCheck(lines) {
  const clone = v => structuredClone(v);
  const read = s => WikiSide.read(s.json, s.sheet, s.pal, null, null, s.fonts);
  let failed = 0;
  const fail = why => { failed++; lines.push(`  reader: ${why}`); };
  const good = syntheticPack(), g = read(good);
  if (!g.art || !g.art.bar[0] || g.art.bar[1] !== null || !g.art.logos || g.art.logos.length !== 2 || !g.art.gui ||
      g.problems.some(m => /bar|logos|gui/.test(m))) fail(`the well-formed pack does not read whole: ${g.problems.join('; ')}`);
  else {
    if (!WikiBar.compose(g.art, 0, 1, 768, 544)) fail('bar.js composes nothing for a side with a bar');
    if (WikiBar.compose(g.art, 1, 1, 768, 544) !== null) fail('bar.js composes a bar for a side without one');
    if (WikiBar.compose(g.art, 0, 1, 128, 544) !== null || WikiBar.compose(g.art, 0, 1, 128 + WikiBar.BAR_W + 1, 544) !== null ||
        WikiBar.compose(g.art, 0, 1, 768, 63) !== null) fail('bar.js composes a frame outside its bounds');
    if (!WikiBar.compose(g.art, 0, 2, 768, 544)) fail('bar.js refuses the bar for a colour past the badge frames (it draws no badge)');
    if (WikiSide.read(good.json, good.sheet, good.pal, null, null).art.bar.some(b => b !== null)) fail('the bars are read with no fonts given');
  }
  const font = mutate => { const s = syntheticPack(); s.fonts['fnt/f.bin'] = mutate(s.fonts['fnt/f.bin']); return s; };
  const json = mutate => { const s = syntheticPack(); mutate(s.json.art); return s; };
  const at = (b, i, v) => { const c = new Uint8Array(b); c[i] = v; return c; };
  const g48 = 4 + 2 * 0x30;   // the table entry of '0'
  const cases = [
    ['PANELTOP outside the sheet', json(a => { a.bar[0].top.f = [40, 40, 40, 32, 0, 0]; }), 'bar'],
    ['PANELBOT missing', json(a => { delete a.bar[0].bot; }), 'bar'],
    ['a rect missing', json(a => { delete a.bar[0].rects.metal0; }), 'bar'],
    ['an inverted rect', json(a => { a.bar[0].rects.energybar = [598, 12, 471, 14]; }), 'bar'],
    ['a coordinate past 4096', json(a => { a.bar[0].rects.logo = [132, 5, 4097, 25]; }), 'bar'],
    ['a coordinate below -1024', json(a => { a.bar[0].rects.logo = [-1025, 5, 152, 25]; }), 'bar'],
    ['a rect of three numbers', json(a => { a.bar[0].rects.logo = [132, 5, 152]; }), 'bar'],
    ['energycolor 256', json(a => { a.bar[0].ecol = 256; }), 'bar'],
    ['metalcolor -1', json(a => { a.bar[0].mcol = -1; }), 'bar'],
    ['a font path climbing out', json(a => { a.bar[0].font = '../f.bin'; }), 'bar'],
    ['a font file absent', json(a => { a.bar[0].font = 'fnt/none.bin'; }), 'bar'],
    ['a font short of its table', font(b => b.subarray(0, 515)), 'bar'],
    ['a font of 0 rows', font(b => at(b, 0, 0)), 'bar'],
    ['a font of 33 rows', font(b => at(b, 0, 33)), 'bar'],
    ['a first code of 1', font(b => at(b, 3, 1)), 'bar'],
    ['a glyph offset past the file', font(b => { const c = at(b, g48, 0xff); c[g48 + 1] = 0xff; return c; }), 'bar'],
    ['a glyph 0 wide', font(b => at(b, 516 + (0x30 - 0x20) * 2, 0)), 'bar'],
    ['a glyph 33 wide', font(b => at(b, 516 + (0x30 - 0x20) * 2, 33)), 'bar'],
    ['a glyph\'s bits past the file', font(b => at(b, 516 + (0x7e - 0x20) * 2, 9)), 'bar'],
    ['33 badge frames', json(a => { a.logos = Array.from({ length: 33 }, () => clone(a.logos[0])); }), 'logos'],
    ['a badge frame outside the sheet', json(a => { a.logos[1].f = [60, 0, 8, 8, 0, 0]; }), 'logos'],
    ['no badge frames', json(a => { a.logos = []; }), 'logos'],
    ['a GUI table of 255', json(a => { a.gui = a.gui.slice(1); }), 'gui'],
    ['a GUI entry of 256', json(a => { a.gui[3] = 256; }), 'gui'],
  ];
  for (const [name, pack, el] of cases) {
    const r = read(pack), A = r.art;
    const dropped = el === 'bar' ? A.bar[0] === null : el === 'logos' ? A.logos === null : A.gui === null;
    const kept = (el === 'bar' || A.bar[0] !== null) && (el === 'logos' || A.logos !== null) && (el === 'gui' || A.gui !== null);
    if (!dropped) fail(`${name}: the ${el} is read`);
    else if (!kept) fail(`${name}: more than the ${el} is dropped`);
    else if (!r.problems.some(m => m.startsWith(el))) fail(`${name}: no problem line for the ${el}`);
    else if (el === 'gui' && WikiBar.compose(A, 0, 1, 768, 544) !== null) fail(`${name}: bar.js composes a bar without the GUI table`);
  }
  // the number rules both ports share
  const rules = [[WikiBar.ftol(-2.9), -2], [WikiBar.ftol(2 ** 31), -(2 ** 31)], [WikiBar.ftol(NaN), -(2 ** 31)],
    [WikiBar.fixed1(0), '0.0'], [WikiBar.fixed1(-0.04), '0.0'], [WikiBar.fixed1(-0.05), '-0.1'], [WikiBar.fixed1(7.25), '7.3'],
    [WikiBar.fixed1(123.44), '123.4']];
  rules.forEach(([got, want], i) => { if (got !== want) fail(`number rule ${i}: ${got}, not ${want}`); });
  if (!failed) lines.push(`  reader: the well-formed pack reads whole; ${cases.length} damaged packs each drop exactly their element with a problem line; ${rules.length} number rules hold`);
  return failed;
}

let code = 0;
try {
  if (!process.argv.includes('--js')) throw new CannotRun('the C half needs tagpu_wikibar.c, which this tree does not have: run with --js');
  const lines = [];
  const failed = captureCheck(lines) + readerCheck(lines);
  console.log(failed ? `barcheck: ${failed} checks FAIL` : 'barcheck: bar.js holds -- the capture\'s two bars and the reader');
  for (const l of lines) console.log(l);
  code = failed ? 1 : 0;
} catch (x) {
  if (!(x instanceof CannotRun)) throw x;
  console.log(`barcheck: cannot run: ${x.message}`);
  code = 2;
}
process.exit(code);
