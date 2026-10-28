/* sidedump.mjs -- one sidebar column of a mod's pack, composed by tools/wiki/mockup/side.js itself, as a PNG at one
   pixel a game pixel: what the stage draws for one named state, for comparing with the game's own sidebar.

   usage: node tools/wiki/sidedump.mjs --mod <m> (--screen <name> | --type <T> --page <n>) [options]
     --mod M          the pack tools/wiki/mockup/gfx/side/<M>/ (gitignored; tools/wiki/sidepacks.py builds it)
     --pack DIR       read the pack from DIR instead
     --screen NAME    a screen of the pack's layout, or main2 / gen: the side's MAIN2 or GEN
     --side NAME      the layout side (its name or prefix) the column shows: its MAIN2 and GEN, its types' pages,
                      its PANELSIDE. Without it, the one side whose screens hold the state; a state more than one
                      side holds (main2 and gen in a pack of two sides, a type two sides page) needs it
     --type T --page N  the screen the router shows for one selected T at page N (0: its orders page), from the
                      side's own types: a type's pages follow the side's prefix
     --pressed A,B    these gadgets in state 1: a button held inside, the current tab
     --grey A,B       these gadgets greyed
     --hide A,B       these gadgets not drawn
     --live FILE      each gadget's greyed state from the engine's own gadget snapshot of that screen (`tacli ui`
                      JSON, as the sidebar captures keep it). Its `active` is not read: the redraw loop skips an
                      inactive gadget (0x4A915D) and so leaves the pixels it had when the screen loaded, which is
                      what a capture shows for a gadget the router made inactive (Escalation's ARMUNLOAD)
     --entry ID       the minimap: that entry's base picture in the pack over its stage map, all explored, no units;
                      without it the minimap is left out
     --out FILE       the PNG (default: <the system's temporary folder>/sidedump/<mod>-<screen>.png; it is drawn
                      from the game's files, so never inside the repository)

   Without --live a gadget is drawn at rest, greyed where the layout says `grey`, and not drawn where it says
   `hidden`. The state is set here and not by sim.js: a comparison with the game wants the engine's states, which the
   wiki changes on purpose (decision 7 greys every order but STOP). The pixels are side.js's compose, through the
   pack's palette; a pixel compose leaves unpainted stays transparent in the PNG.

   exit 0 written, 1 the state names something the pack lacks, 2 bad arguments or an unreadable pack. */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSide = require('./mockup/side.js');
const ROOT = new URL('../../', import.meta.url).pathname;

function args(argv) {
  const o = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) throw new Error(`unexpected ${a}`);
    const v = argv[i + 1];
    if (v === undefined || v.startsWith('--')) throw new Error(`${a} needs a value`);
    o[a.slice(2)] = v;
    i++;
  }
  return o;
}
const list = v => (v ? v.split(',').map(s => s.trim()).filter(Boolean) : []);
const same = (a, b) => a.toLowerCase() === b.toLowerCase();

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
  ihdr[8] = 8; ihdr[9] = 6;          // 8 bits a channel, RGBA
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

function readPack(dir) {
  const j = JSON.parse(fs.readFileSync(path.join(dir, 'side.json'), 'utf8')), A = j.art || {};
  const bin = f => { if (!f) return null; const p = path.join(dir, f); return fs.existsSync(p) ? new Uint8Array(fs.readFileSync(p)) : null; };
  const pack = WikiSide.read(j, bin(A.sheet && A.sheet.file), bin(A.palette), bin(A.minimap && A.minimap.seen), bin(A.greyed));
  return { ...pack, dir, json: j };
}

// the screens a layout side routes to: its MAIN2, its GEN, and each of its types' pages and orders page
const screensOf = sd => [sd.main2, sd.gen, ...Object.values(sd.types).flatMap(t => [...t.pages, ...(t.orders ? [t.orders] : [])])];

/* the screen the state names -- a layout screen, the side's main2 / gen, or one selected type's page -- and the index
   of the layout side that shows it: --side's, else the one side that holds it */
function screenOf(o, L) {
  let k = -1;
  if (o.side !== undefined) {
    k = L.sides.findIndex(d => same(d.name, o.side) || same(d.prefix, o.side));
    if (k < 0) throw new Error(`no side ${o.side} (the sides: ${L.sides.map(d => d.name).join(', ')})`);
  }
  const only = (ks, what) => {
    if (k >= 0) { if (!ks.includes(k)) throw new Error(`side ${L.sides[k].name} has no ${what}`); return k; }
    if (ks.length === 1) return ks[0];
    if (!ks.length) throw new Error(`no side has ${what}`);
    throw new Error(`${ks.map(i => L.sides[i].name).join(' and ')} each have ${what}: name one with --side`);
  };
  const all = L.sides.map((d, i) => i);
  if (o.type !== undefined) {
    const has = i => Object.keys(L.sides[i].types).find(t => same(t, o.type));
    const ks = all.filter(has);
    if (!ks.length) throw new Error(`the pack's layout has no pages for ${o.type} (the sides' types: ${L.sides.map(d => `${d.name} ${Object.keys(d.types).join(', ') || 'none'}`).join('; ')})`);
    const i = only(ks, `pages for ${o.type}`), T = has(i), t = L.sides[i].types[T], page = Number(o.page ?? 1);
    if (page === 0) { if (!t.orders) throw new Error(`${T} has no orders page`); return [t.orders, i]; }
    if (!(Number.isInteger(page) && page >= 1 && page <= t.pages.length)) throw new Error(`${T} has pages 1..${t.pages.length}`);
    return [t.pages[page - 1], i];
  }
  const s = o.screen;
  if (s === undefined) throw new Error('--screen or --type is required');
  if (same(s, 'main2') || same(s, 'gen')) {
    const i = only(all, s);
    return [same(s, 'main2') ? L.sides[i].main2 : L.sides[i].gen, i];
  }
  const hit = Object.keys(L.screens).find(n => same(n, s));
  if (!hit) throw new Error(`the pack's layout has no screen ${s} (its screens: ${Object.keys(L.screens).join(', ')})`);
  return [hit, only(all.filter(i => screensOf(L.sides[i]).includes(hit)), `the screen ${hit}`)];
}

function main() {
  let o;
  try { o = args(process.argv.slice(2)); } catch (e) { console.error(`sidedump: ${e.message}`); return 2; }
  if (!o.mod && !o.pack) { console.error('sidedump: --mod or --pack is required'); return 2; }
  const dir = o.pack || path.join(ROOT, 'tools/wiki/mockup/gfx/side', o.mod);
  let p;
  try { p = readPack(dir); } catch (e) { console.error(`sidedump: ${dir}: ${e.message}`); return 2; }
  if (!p.art || !p.layout) { console.error(`sidedump: ${dir}: no art: ${p.problems.join('; ')}`); return 2; }
  for (const m of p.problems) console.warn(`sidedump: pack: ${m}`);
  const L = p.layout;
  let screen, side;
  try { [screen, side] = screenOf(o, L); } catch (e) { console.error(`sidedump: ${e.message}`); return 1; }
  const gadgets = L.screens[screen].gadgets;
  const state = gadgets.map(g => g.hidden ? -1 : g.grey ? 2 : 0);
  const unknown = [];
  if (o.live) {
    const snap = JSON.parse(fs.readFileSync(o.live, 'utf8')).gadgets.filter(g => g.id === 1);
    gadgets.forEach((g, i) => {
      const s = snap.find(q => same(q.name, g.name));
      if (!s) { unknown.push(`${g.name} (not in the snapshot)`); return; }
      if (state[i] !== -1) state[i] = s.grayed ? 2 : 0;
    });
  }
  const set = (names, v) => { for (const n of names) { const i = gadgets.findIndex(g => same(g.name, n)); if (i < 0) unknown.push(n); else state[i] = v; } };
  set(list(o.grey), 2);
  set(list(o.hide), -1);
  set(list(o.pressed), 1);
  if (unknown.length) console.warn(`sidedump: ${screen}: ${unknown.join(', ')}: not a gadget of the screen, or left as the layout has it`);

  let base = null, map = null, W = 0, H = 0;
  if (o.entry) {
    const b = p.minimap[o.entry], e = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/entries', `${o.entry}.json`), 'utf8'));
    const mw = e.scene.map.w, mh = e.scene.map.h;
    if (!b) { console.error(`sidedump: the pack has no minimap for ${o.entry}`); return 1; }
    base = new Uint8Array(fs.readFileSync(path.join(dir, b.file)));
    map = { w: mw, h: mh, cell: 16, fog: [] };
    W = mw * 16; H = mh * 16;
  }
  const view = { map, units: [], side: { index: side, h: L.height, screen, unit: null, page: 0, state, under: null } };
  const c = WikiSide.compose(p.art, L, view, base, W, H);
  const out = o.out || path.join(os.tmpdir(), 'sidedump', `${o.mod || path.basename(dir)}-${screen}.png`);
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(out, png(WikiSide.COL, c.h, WikiSide.rgba(p.art, c)));
  console.log(`sidedump: ${out}: ${L.sides[side].name} ${screen}, ${WikiSide.COL} x ${c.h}, states ${gadgets.map((g, i) => `${g.name}=${state[i]}`).join(' ')}`);
  return 0;
}
process.exitCode = main();
