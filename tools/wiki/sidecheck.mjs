/* sidecheck.mjs -- the sidebar composer's check: tagpu/ddraw/src/tagpu_wikiside.c's column against the mockup's
   tools/wiki/mockup/side.js, byte for byte.

   usage: node tools/wiki/sidecheck.mjs [--no-packs] [--js-only]
     exit 0 when every frame is equal, 1 when one differs, 2 when the C side cannot be built or run

   For every demo of every entry, sim.js plays on a sidebar's layout and, at every sampled tick (every 30th and the
   last, every tick a gadget's state changes, every 10th while a unit is under the cursor), side.js composes the view's
   column -- palette indices, coverage, and the RGBA through the palette -- from that sidebar's art and the stage's
   minimap picture; a native build of tagpu_wikiside.c (tagpu/ddraw/tests/wikisidetest.c, over tagpu_wikisim.c's view
   copied by tagpu_wikistage_copy) composes the same frames, and the two must be equal in every byte, with the same
   rows and the same answer on whether the minimap was composed.

   THE ART. Two synthetic packs, generated here from a fixed seed, so the check needs no game data and puts none in the
   repository: one on art/wiki/side/fixture.json's layout, one on fixture-tall.json's. They take every branch of the
   composer: a sheet whose frames are random bytes with the transparent index common in them; a PANELSIDE wider than
   the column with a hotspot it ignores; panel pictures with and without a hotspot, and screens with none (the fill);
   buttons that are labels, one-frame pictures with and without a key, two- and three-frame pictures with every
   attribs case of the greyed rule (0, 0x20, 0x80, 0x100, 0x180), frames larger and smaller than their gadget; radlogo
   frames for some owners only, and radlogohigh; the minimap's colours given and absent (-1, which both sides read as
   0); a pack with its seen remap and greyed tint and one without either; and minimap pictures of the right size, the
   wrong size and none.

   And, unless --no-packs, every real pack under tools/wiki/mockup/gfx/side/<mod>/ that is present (gitignored:
   tools/wiki/sidepacks.py builds them from the player's install), on its own layout: the stronger check, the mods'
   own pictures through both composers.

   THE SIDES. A pack holds every side of its mod, each with its own PANELSIDE (art.panelside, index-aligned with
   layout.sides) and its own casting; the C side is handed every side's PANELSIDE, as WIKISIDE holds them. Every
   context -- each side of each pack with its casting, at Interface Type 1 and 0 -- is played by both sides
   (wikisidetest.c is handed the context, WIKISIMCTX, and composes for the view's side, as the DLL does) and compared
   byte for byte; each sampled frame's view must also show that side, and side.js's PANELSIDE for it must be the one
   that side's pack names.
   --js-only plays every context through side.js alone, building nothing. */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSim = require('./mockup/sim.js');
const WikiSide = require('./mockup/side.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const PACKS = path.join(ROOT, 'tools/wiki/mockup/gfx/side');
const SRCS = ['src/tagpu_wikiside.c', 'src/tagpu_wikistage.c', 'src/tagpu_wikisim.c', 'src/tagpu_wikidata.c', 'src/tagpu_json.c',
  'src/tagpu_pak.c', 'src/tagpu_placegeo.c', 'src/tagpu_sweepgeo.c',
  'src/tagpu_wikicob.c', 'src/tagpu_cobvm.c', 'src/tagpu_cob_safe.c'];
const COL = WikiSide.COL;
// WIKISIDE_FROM_*: where a picture came from; the composer tells only a label from the rest
const FROM = { label: 0, page: 1, commongui: 2, fallback: 3, download: 4, unitpics: 5 };

class CannotRun extends Error {}
function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { maxBuffer: 1 << 30, ...opts });
  if (r.error || r.status !== 0)
    throw new CannotRun(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ` (${r.status})`}\n${r.stderr || ''}`);
  return r.stdout;
}

/* ---- the synthetic packs ---- */
// mulberry32: a fixed sequence for a seed
function prng(seed) {
  let s = seed >>> 0;
  return () => {
    s = (s + 0x6d2b79f5) >>> 0;
    let t = s;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const KEY = 9;                                                    // the transparent index the sheet is full of
function synthetic(name, layout, seed, opts, entries) {
  const r = prng(seed), int = (a, b) => a + Math.floor(r() * (b - a + 1)), pick = a => a[int(0, a.length - 1)];
  const SW = 1024, SH = 512, sheet = new Uint8Array(SW * SH);
  for (let i = 0; i < sheet.length; i++) sheet[i] = r() < 0.25 ? KEY : int(0, 255);
  const bytes = n => Uint8Array.from({ length: n }, () => int(0, 255));
  const frame = (w, h, hx = 0, hy = 0) => [int(0, SW - w), int(0, SH - h), w, h, hx, hy];
  const key = () => pick([KEY, KEY, int(0, 255), -1]);
  const screens = {};
  Object.entries(layout.screens).forEach(([sn, s], k) => {
    const pics = s.gadgets.map((g, i) => {
      const [, , gw, gh] = g.rect, kind = int(0, 9), attribs = pick([0, 0x20, 0x80, 0x100, 0x180]);
      const f = () => frame(Math.max(1, gw + int(-6, 8)), Math.max(1, gh + int(-6, 8)), int(-3, 3), int(-3, 3));
      if (kind === 0) return { from: 'label', text: '' };
      if (kind === 1) return { from: 'label', text: `Lbl${k}.${i}` };
      if (kind === 2) return { from: 'unitpics', frames: [f()], key: -1, attribs };
      if (kind === 3) return { from: 'commongui', frames: [f()], key: KEY, attribs };
      if (kind === 4) return { from: 'page', frames: [f(), f()], key: key(), attribs };
      return { from: pick(['page', 'commongui', 'download', 'fallback']), frames: [f(), f(), f()], key: key(), attribs };
    });
    screens[sn] = { pics };
    if (k % 4 !== 3) screens[sn].panel = { f: frame(int(100, 130), Math.min(SH, int(60, s.h + 20)), int(-4, 4), int(-4, 4)), key: key() };
  });
  const bases = {};
  // a base per stage, `<entry>.<scene>`, every scene of an entry being its first one's size
  entries.flatMap(e => Object.keys(e.scenes).map(n => [`${e.id}.${n}`, e.scenes[n].map])).forEach(([id, map], i) => {
    const m = WikiSide.geometry(map.w * 16, map.h * 16), want = opts.bases[i % opts.bases.length];
    if (want !== 'none') bases[id] = bytes(m.w * m.h - (want === 'short' ? 1 : 0));
  });
  const art = {
    sheet: { file: 'sheet.bin', w: SW, h: SH }, palette: 'pal.bin',
    panelside: layout.sides.map(() => ({ f: frame(129, 300, int(-9, 9), int(-9, 9)), key: KEY })),
    dots: { key: KEY, radlogo: Array.from({ length: opts.radlogo }, () => frame(int(3, 7), int(3, 7), int(0, 3), int(0, 3))),
      high: frame(8, 8, 3, 3) },
    minimap: { unexplored: opts.unexplored, box: opts.box, ...(opts.tables ? { seen: 'seen.bin' } : {}),
      bases: Object.fromEntries(Object.keys(bases).map(id => [id, { file: `bases/${id}.bin` }])) },
    ...(opts.tables ? { greyed: 'greyed.bin' } : {}),
    screens,
  };
  return { name, json: { format: 1, layout, art }, sheet, pal: bytes(1024), seen: opts.tables ? bytes(256) : null,
    greyed: opts.tables ? bytes(256) : null, bases };
}

/* a real pack as side.js's load() reads it, its minimap pictures by stage */
function realPack(mod) {
  const dir = path.join(PACKS, mod), json = JSON.parse(fs.readFileSync(path.join(dir, 'side.json'), 'utf8')), A = json.art || {};
  const bin = f => { if (!f) return null; const p = path.join(dir, f); return fs.existsSync(p) ? new Uint8Array(fs.readFileSync(p)) : null; };
  const bases = {};
  for (const [id, b] of Object.entries((A.minimap && A.minimap.bases) || {})) { const v = bin(b && b.file); if (v) bases[id] = v; }
  return { name: mod, json, sheet: bin(A.sheet && A.sheet.file), pal: bin(A.palette), seen: bin(A.minimap && A.minimap.seen),
    greyed: bin(A.greyed), bases };
}

/* ---- a pack for the C side: the art as the asset loader fills WIKISIDE (wikisidetest.c, art.json), a PANELSIDE per
   layout side ---- */
function writeC(pack, dir) {
  const L = pack.json.layout, A = pack.json.art || {}, D = A.dots || {}, MM = A.minimap || {};
  const intOr = (v, d) => Number.isInteger(v) ? v : d;
  const PS = Array.isArray(A.panelside) ? A.panelside : [];
  const art = {
    sheet: [A.sheet.w, A.sheet.h],
    panelside: L.sides.map((_sd, k) => (PS[k] ? { f: PS[k].f, key: intOr(PS[k].key, -1) } : null)),
    dots: { key: intOr(D.key, -1), radlogo: D.radlogo || [], high: D.high || null },
    unexplored: intOr(MM.unexplored, -1), box: intOr(MM.box, -1),
    screens: Object.entries(L.screens).map(([sn, s]) => {
      const sa = (A.screens || {})[sn] || {};
      return {
        panel: sa.panel ? { f: sa.panel.f, key: intOr(sa.panel.key, -1) } : null,
        pics: s.gadgets.map((g, i) => {
          const p = (sa.pics || [])[i] || { from: 'label' }, label = p.from === 'label' || !Array.isArray(p.frames) || !p.frames.length;
          return { from: label ? FROM.label : FROM[p.from] ?? FROM.page, frames: label ? [] : p.frames, key: intOr(p.key, -1),
            attribs: intOr(p.attribs, 0), text: typeof p.text === 'string' ? p.text : '' };
        }),
      };
    }),
  };
  fs.mkdirSync(path.join(dir, 'bases'), { recursive: true });
  fs.writeFileSync(path.join(dir, 'layout.json'), JSON.stringify(L));
  fs.writeFileSync(path.join(dir, 'art.json'), JSON.stringify(art));
  fs.writeFileSync(path.join(dir, 'sheet.bin'), pack.sheet);
  fs.writeFileSync(path.join(dir, 'pal.bin'), pack.pal);
  if (pack.seen) fs.writeFileSync(path.join(dir, 'seen.bin'), pack.seen);
  if (pack.greyed) fs.writeFileSync(path.join(dir, 'greyed.bin'), pack.greyed);
  for (const [id, b] of Object.entries(pack.bases)) fs.writeFileSync(path.join(dir, 'bases', `${id}.bin`), b);
}

/* the C side's frames, by key */
function readC(buf) {
  const out = new Map();
  let o = 0;
  while (o < buf.length) {
    const n = buf.readUInt16LE(o), key = buf.toString('latin1', o + 2, o + 2 + n);
    o += 2 + n;
    const h = buf.readInt32LE(o), minimap = buf.readInt32LE(o + 4);
    o += 8;
    const f = { h, minimap };
    if (h > 0) {
      const px = COL * h;
      f.idx = buf.subarray(o, o + px); o += px;
      f.a = buf.subarray(o, o + px); o += px;
      f.rgba = buf.subarray(o, o + 4 * px); o += 4 * px;
    }
    out.set(key, f);
  }
  return out;
}

/* sampled: every 30th tick and the last, every tick the routed screen or a gadget's state differs from the tick
   before's (a press, a release, a greyed button coming alive, a page turned), every 10th while a unit is under the
   cursor. `prev` is the tick before's screen and states, null when it had no sidebar */
const sideKey = v => v.side ? `${v.side.screen}|${v.side.state.join()}` : null;
const sampled = (v, prev, tick, done) => tick % 30 === 0 || done ||
  (!!v.side && (prev === null || prev !== sideKey(v) || (v.side.under != null && tick % 10 === 0)));

// the first byte two arrays differ in, as a pixel of the column, or null
function firstDiff(js, c, per) {
  const n = Math.max(js.length, c.length);
  for (let i = 0; i < n; i++) if (js[i] !== c[i]) {
    const px = Math.floor(i / per);
    return `pixel (${px % COL}, ${Math.floor(px / COL)}): side.js ${js[i]}, C ${c[i]}`;
  }
  return null;
}

/* THE CONTEXTS a pack is played in, sim.js's ctx: every side k of its layout with that side's casting (none for a pack
   without castings), at Interface Type 1 and 0 */
function contexts(pack) {
  const L = pack.json.layout, casting = pack.json.casting;
  return L.sides.flatMap((sd, k) => [1, 0].map(iface =>
    ({ side: k, casting: casting ? casting[k] ?? null : null, iface, at: `${sd.name}/${iface}` })));
}

const JS_ONLY = process.argv.includes('--js-only');
const t0 = Date.now();
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'sidecheck-'));
let code = 0;
try {
  /* ---- the C side, built natively with the sanitizers ---- */
  const exe = path.join(DDRAW, 'tests/wikisidetest-native.exe');   // `.exe` so the tree's *.exe ignore covers it
  const pak = path.join(tmp, 'wiki.bin');
  if (!JS_ONLY) {
    run('gcc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc', '-O1', '-g', '-fsanitize=address,undefined',
      '-fno-sanitize-recover=all', '-o', exe, 'tests/wikisidetest.c', ...SRCS, '-lm'], { cwd: DDRAW, encoding: 'utf8' });
    run('python3', [path.join(ROOT, 'tools/hudpak.py'), '--set', 'wiki', path.join(ROOT, 'art/wiki'), pak], { encoding: 'utf8' });
  }

  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  const entries = [...new Set(tree.tree.flatMap(leaves))]
    .map(id => ({ ...JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8')), id }));
  const SIDE = n => JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/side/${n}.json`), 'utf8'));

  const packs = [
    synthetic('synthetic fixture', SIDE('fixture'), 0x51de0001,
      { radlogo: 3, unexplored: -1, box: 194, tables: true, bases: ['good', 'short'] }, entries),
    synthetic('synthetic fixture-tall', SIDE('fixture-tall'), 0x51de0002,
      { radlogo: 8, unexplored: 37, box: -1, tables: false, bases: ['none', 'good'] }, entries),
  ];
  const absent = [];
  if (!process.argv.includes('--no-packs') && fs.existsSync(PACKS))
    for (const mod of fs.readdirSync(PACKS).sort()) {
      if (fs.existsSync(path.join(PACKS, mod, 'side.json'))) packs.push(realPack(mod));
      else absent.push(mod);
    }

  const lines = [];
  let nframes = 0, ndiffer = 0;
  for (const pack of packs) {
    const dir = path.join(tmp, pack.name.replace(/[^a-z0-9-]+/gi, '_'));
    const p = WikiSide.read(pack.json, pack.sheet, pack.pal, pack.seen, pack.greyed), L = pack.json.layout;
    if (!p.art) throw new CannotRun(`${pack.name}: side.js reads no art: ${p.problems.join('; ')}`);
    if (!JS_ONLY) writeC(pack, dir);
    const st = { frames: 0, side: 0, minimap: 0, pressed: 0, greyed: 0, under: 0, bytes: 0 };
    const played = [];
    let bad = 0;
    const differ = (key, why) => { bad++; if (bad <= 10) console.log(`DIFFER ${pack.name} ${key}: ${why}`); };
    for (const ctx of contexts(pack)) {
      const { at } = ctx, k = ctx.side;
      played.push(at);
      const want = Array.isArray((pack.json.art || {}).panelside) ? pack.json.art.panelside[k] : null, got = p.art.panelside[k];
      if (JSON.stringify(want ? want.f : null) !== JSON.stringify(got ? got.f : null)) differ(at, `side.js's PANELSIDE is not side ${k}'s`);
      let C = null;
      if (!JS_ONLY) {
        const cf = path.join(dir, 'ctx.json');
        fs.writeFileSync(cf, JSON.stringify({ side: k, iface: ctx.iface, casting: ctx.casting }));
        // to a file: a pack's frames run to hundreds of megabytes
        const out = path.join(tmp, 'frames.bin'), fd = fs.openSync(out, 'w');
        try { run(exe, ['compose', pak, dir, cf], { stdio: ['ignore', fd, 'pipe'] }); } finally { fs.closeSync(fd); }
        C = readC(fs.readFileSync(out));
      }
      let nctx = 0;
      for (const entry of entries) for (const demo of entry.demos) {
        let sim;
        try { sim = WikiSim.play(entry, demo, L, ctx); } catch (e) {
          const c = C && C.get(`${entry.id}/${demo.id}/0`);
          if (C && (!c || c.h !== -1)) differ(`${at} ${entry.id}/${demo.id}`, `sim.js refuses it (${e.message}), the C side ${c ? 'plays it' : 'gives no frame'}`);
          continue;
        }
        let prev = null;
        for (;;) {
          const v = sim.view(), was = prev;
          prev = sideKey(v);
          if (sampled(v, was, sim.tick, sim.done)) {
            const id = `${entry.id}/${demo.id}/${sim.tick}`, key = `${at} ${id}`, c = C ? C.get(id) : { h: v.side ? v.side.h : 0 };
            st.frames++;
            nctx++;
            if (!c) differ(key, 'no C frame');
            else if (!v.side) { if (c.h !== 0) differ(key, `no sidebar in sim.js's view, ${c.h} rows from the C side`); }
            else if (v.side.index !== k) differ(key, `the view shows side ${v.side.index}`);
            else {
              const sid = `${entry.id}.${typeof demo.on === 'string' ? demo.on : Object.keys(entry.scenes)[0]}`;
              const M = v.map, cell = M.cell || 16, base = pack.bases[sid] || null;
              const js = WikiSide.compose(p.art, L, v, base, M.w * cell, M.h * cell), rgba = WikiSide.rgba(p.art, js);
              st.side++; st.minimap += js.minimap ? 1 : 0; st.under += v.side.under != null ? 1 : 0;
              st.pressed += v.side.state.filter(x => x === 1).length; st.greyed += v.side.state.filter(x => x === 2).length;
              let why = null;
              if (!C) why = js.h !== v.side.h ? `rows: side.js ${js.h}, the view ${v.side.h}` : null;
              else if (c.h !== js.h) why = `rows: side.js ${js.h}, C ${c.h}`;
              else if (!!c.minimap !== js.minimap) why = `minimap: side.js ${js.minimap}, C ${!!c.minimap}`;
              else why = (firstDiff(js.idx, c.idx, 1) && `indices, ${firstDiff(js.idx, c.idx, 1)}`) ||
                (firstDiff(js.a, c.a, 1) && `coverage, ${firstDiff(js.a, c.a, 1)}`) ||
                (firstDiff(rgba, c.rgba, 4) && `RGBA, ${firstDiff(rgba, c.rgba, 4)}`);
              if (why) differ(key, why);
              else st.bytes += 6 * COL * js.h;
            }
          }
          if (sim.done) break;
          sim.step();
        }
      }
      if (C && C.size !== nctx + [...C.values()].filter(f => f.h === -1).length) differ(at, `the C side wrote ${C.size} frames, side.js sampled ${nctx}`);
    }
    nframes += st.frames;
    const what = JS_ONLY ? 'composed by side.js alone' : 'equal';
    if (bad) { code = 1; ndiffer += bad; lines.push(`  ${pack.name}: ${bad} DIFFER`); }
    else lines.push(`  ${pack.name} (${L.height} rows), ${played.join(' ')}${pack.json.casting ? ' with their castings' : ''}: ${st.frames} frames ${what}, ${st.side} with the sidebar (${st.minimap} with the minimap, ${st.under} with a unit under the cursor), ${st.pressed} pressed and ${st.greyed} greyed gadgets drawn${JS_ONLY ? '' : `, ${(st.bytes / 1048576).toFixed(1)} MiB compared`}`);
  }
  const s = ((Date.now() - t0) / 1000).toFixed(1), half = JS_ONLY ? 'side.js alone, the C half not run' : 'byte for byte';
  console.log(code ? `sidecheck: ${ndiffer} of ${nframes} frames DIFFER (${s} s)` : `sidecheck: all ${JS_ONLY ? 'compose' : 'equal'} -- ${packs.length} packs, ${nframes} frames ${half}, every side at both interfaces (${s} s)`);
  for (const l of lines) console.log(l);
  if (absent.length) console.log(`  not present (no side.json): ${absent.join(', ')}`);
} catch (x) {
  if (!(x instanceof CannotRun)) throw x;
  console.log(`sidecheck: cannot run: ${x.message}`);
  code = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
process.exit(code);
