/* sideview.mjs -- what the mockup's simulation (tools/wiki/mockup/sim.js) shows at a state the game's wiki stands at,
   for tools/wiki_regression.py: the sidebar's routed screen, page and gadget states, the armed mode and the selected
   units' orders, and optionally the column composed from a pack as the stage draws it (side.js, as wiki.js sideArt).

   usage: node tools/wiki/sideview.mjs --dump FILE [--compose DIR --out PNG]
          node tools/wiki/sideview.mjs --play ENTRY DEMO TICK --layout FILE [--ctx FILE] [--compose DIR --out PNG]
     --dump FILE    Try it yourself as the game played it: the wiki's state dump (tagpu_wiki.json), its scene made with
                    WikiSim.create on the dump's `layout` in the dump's `ctx` (side, interface type, casting) and fed
                    its `try_log` at the logged ticks, up to its tick -- simparity.mjs --try's replay, whose hash the
                    dump's must equal before this view describes the game
     --play         a scripted demo (DEMO counts from 1) on the layout FILE (tagpu_wiki.layout.json; `null` for none),
                    played to TICK or to its end, as tagpu_wiki.shot holds it
     --ctx FILE     the context --play plays in, as the dump's `ctx` writes it ({side, iface, casting}); sim.js's
                    default (side 0, no casting, type 1) without it
     --compose DIR  the column through the pack in DIR (tools/wiki/sidepacks.py's gfx/side/<mod>/), with the entry's
                    minimap base, written to PNG at one pixel a game pixel (alpha 0 where nothing is painted)

   Prints one JSON line: { tick, done, mode, locked, side: { h, screen, unit, type, page, state, y, gadgets },
   selected, orders, cursor } -- `type` is the shown unit's, `y` the screen's top row, and `gadgets` names each state's
   gadget with its role and its rect in the screen's own pixels.
   exit 0, 1 a pack or a state that cannot be composed, 2 bad arguments. */
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSim = require('./mockup/sim.js');
const WikiSide = require('./mockup/side.js');
const ROOT = new URL('../../', import.meta.url).pathname;

const argv = process.argv.slice(2);
const opt = k => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : undefined; };
const entryOf = id => JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));

function fail(code, msg) { console.error(`sideview: ${msg}`); process.exit(code); }

let entry, sim;
if (argv.includes('--dump')) {
  const dump = JSON.parse(fs.readFileSync(opt('--dump'), 'utf8'));
  entry = entryOf(dump.entry);
  const demo = entry.demos.find(d => d.id === dump.demo);
  if (!demo) fail(2, `no demo ${dump.entry}/${dump.demo}`);
  sim = WikiSim.create(entry, demo, dump.layout ?? null, dump.ctx || undefined);
  for (const [t, kind, a, b] of dump.try_log) {
    while (sim.tick < t) sim.step();
    sim.input(kind === 'move' ? { move: [a, b] } : { [kind]: a });
  }
  while (sim.tick < dump.tick) sim.step();
} else if (argv.includes('--play')) {
  const i = argv.indexOf('--play'), [id, n, tick] = argv.slice(i + 1, i + 4);
  const lf = opt('--layout');
  if (!lf) fail(2, '--play needs --layout FILE');
  entry = entryOf(id);
  const demo = entry.demos[Number(n) - 1];
  if (!demo) fail(2, `${id} has no demo ${n}`);
  const cf = opt('--ctx');
  sim = WikiSim.play(entry, demo, JSON.parse(fs.readFileSync(lf, 'utf8')), cf ? JSON.parse(fs.readFileSync(cf, 'utf8')) : undefined);
  while (!sim.done && sim.tick < Number(tick)) sim.step();
} else fail(2, '--dump FILE or --play ENTRY DEMO TICK --layout FILE');

const v = sim.view(), L = sim.layout;
const sel = v.units.filter(u => u.selected);
const out = { tick: sim.tick, done: !!sim.done, mode: v.mode, locked: v.locked ?? null, side: null,
  selected: sel.map(u => u.id), orders: Object.fromEntries(sel.map(u => [u.id, sim.orders(u.id)])), cursor: v.cursor };

if (v.side) {
  const lay = argv.includes('--dump') ? JSON.parse(fs.readFileSync(opt('--dump'), 'utf8')).layout
    : JSON.parse(fs.readFileSync(opt('--layout'), 'utf8'));
  const scr = lay.screens[v.side.screen];
  const shown = v.units.find(u => u.id === v.side.unit);
  out.side = { ...v.side, type: shown ? shown.type : null, y: scr.y, gadgets: scr.gadgets.map(g => ({ name: g.name, role: g.role, unit: g.unit ?? null, rect: g.rect })) };
}

const packDir = opt('--compose');
if (packDir) {
  if (!v.side) fail(1, 'the view has no sidebar to compose');
  const bin = f => { if (!f) return null; const p = path.join(packDir, f); return fs.existsSync(p) ? new Uint8Array(fs.readFileSync(p)) : null; };
  const j = JSON.parse(fs.readFileSync(path.join(packDir, 'side.json'), 'utf8')), A = j.art || {};
  const p = WikiSide.read(j, bin(A.sheet && A.sheet.file), bin(A.palette), bin(A.minimap && A.minimap.seen), bin(A.greyed));
  if (!p.art) fail(1, `${packDir}: no art: ${p.problems.join('; ')}`);
  const b = p.minimap[entry.id];
  const base = b ? bin(b.file) : null;
  const c = WikiSide.compose(p.art, p.layout, v, base, v.map.w * v.map.cell, v.map.h * v.map.cell);
  const rgba = WikiSide.rgba(p.art, c);
  out.composed = { file: opt('--out'), h: c.h, minimap: c.minimap, problems: p.problems };
  fs.writeFileSync(opt('--out'), png(WikiSide.COL, c.h, rgba));
}
console.log(JSON.stringify(out));

// a PNG of w x h RGBA bytes: one IDAT, every row filter 0
function png(w, h, rgba) {
  const chunk = (type, data) => {
    const t = Buffer.from(type, 'latin1'), len = Buffer.alloc(4), crc = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    crc.writeUInt32BE(zlib.crc32(Buffer.concat([t, data])) >>> 0);
    return Buffer.concat([len, t, data, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6;
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
