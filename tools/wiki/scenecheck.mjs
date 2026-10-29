/* scenecheck.mjs -- the in-game wiki's 3D scene check: tagpu/ddraw/src/tagpu_wikiscene.c against the mockup's
   tools/wiki/mockup/scene.js and the two gathers of tools/tascene-render.js, record for record.

   usage: node tools/wiki/scenecheck.mjs
     exit 0 when everything pairs, 1 when something differs, 2 when a side cannot be built or run (no pack: run
     tools/wiki/gfx.py first)

   WHAT IS PAIRED (research/notes/ingame-wiki-3d-stage-plan.md, section 9, "The scene, checked op-for-op"):
   - each stage's GROUND, from the mockup's pack (tools/wiki/gfx.py, which resolves the stage descriptions against
     the install): the terrain cells classicTerrain().gather makes at the stage's camera (zoom 1, the eye at the
     map's corner, the whole map) -- column, row and tile, exactly -- and the feature quads gatherFeatures makes
     there -- each corner, the depth key, the mode and the anchor, within 1/64 of a unit -- in their order, both
     buckets. The gathers run as gfx.js runs them, through a recording stand-in for the WebGL context, so the code
     paired is the mockup's own and not a copy of it;
   - every demo's FOG at tick 0, every 30th tick and its last: scene.js fogGrid of sim.js's view against
     tagpu_wikiscene_fog of tagpu_wikisim.c's view as the record carries it (tagpu_wikistage_copy) -- the grid's
     size, its origin and every byte;
   - every demo's MODEL RECORDS at the same ticks: scene.js build of sim.js's view against tagpu_wikiscene_models of
     the record's view, the types the pack resolved handed to both -- each record's type, owner, ghost bit, shadow
     kind, slot, anchor, yaw, depth key and alpha within 1/64, and its nanoframe staging (on or off, the threshold,
     the three colours and the wire's) exactly, units then ghosts in order.
   It catches a wrong placement, a wrong band, a wrong fog corner, a unit turned the wrong way or staged on the wrong
   frame without either renderer. */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSim = require('./mockup/sim.js');
const WikiScene = require('./mockup/scene.js');
require('../tascene-render.js');
const TR = globalThis.TasceneRender;
const ROOT = new URL('../../', import.meta.url).pathname;
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const PACK = path.join(ROOT, 'tools/wiki/mockup/gfx');
const TOL = 1 / 64;
const SRCS = ['src/tagpu_wikiscene.c', 'src/tagpu_wikistage.c', 'src/tagpu_wikisim.c', 'src/tagpu_wikidata.c', 'src/tagpu_json.c',
  'src/tagpu_pak.c', 'src/tagpu_placegeo.c', 'src/tagpu_sweepgeo.c', 'src/tagpu_hudtext.c',
  'src/tagpu_wikicob.c', 'src/tagpu_cobvm.c', 'src/tagpu_cob_safe.c'];

class CannotRun extends Error {}
function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { maxBuffer: 1 << 30, encoding: 'utf8', ...opts });
  if (r.error || r.status !== 0)
    throw new CannotRun(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ` (${r.status})`}\n${r.stderr || ''}`);
  return r.stdout;
}

/* A WebGL2 context that records: every method is a no-op returning a handle, the status queries say yes, and
   bufferData keeps what it was given -- which is how the terrain gather hands over its instances. */
function fakeGL() {
  const got = { buffers: [] };
  const gl = new Proxy({}, {
    get(_, k) {
      if (k === 'got') return got;
      if (k === 'bufferData') return (t, data) => got.buffers.push(data);
      if (k === 'getShaderParameter' || k === 'getProgramParameter') return () => true;
      if (typeof k === 'string' && /^[A-Z_0-9]+$/.test(k)) return 0;
      return () => ({});
    },
  });
  return gl;
}

const u16 = b => new Uint16Array(b.buffer, b.byteOffset, b.byteLength >> 1);

/* one entry's ground, the mockup's way: the pack read as gfx.js reads it */
function mockupGround(dir, pack) {
  const [mw, mh] = pack.cells, GW = mw * 16, GH = mh * 16;
  const gl = fakeGL();
  const atlas = new Uint8Array(pack.map.atlas.w * pack.map.atlas.h);
  const pal = new Uint8Array(1024);
  const terr = TR.classicTerrain(gl, '', '', pack.map.atlas, atlas, pal);
  const tiles = u16(fs.readFileSync(path.join(dir, pack.map.tilemap.file)));
  gl.got.buffers.length = 0;
  const g = terr.gather(tiles, pack.map.tilemap, [0, 0], [GW, GH], 0, 0);
  const inst = gl.got.buffers.at(-1) || new Int16Array(0);
  const cells = [];
  for (let i = 0; i < g.n; i++) {
    const col = inst[i * 4], row = inst[i * 4 + 1], tile = inst[i * 4 + 2] + inst[i * 4 + 3] * pack.map.atlas.cols;
    cells.push([col, row, tile]);
  }
  let shadow = [], body = [];
  const feats = [];
  if (pack.features && pack.features.count) {
    const ib = u16(fs.readFileSync(path.join(dir, pack.features.instances)));
    const anchors = [];
    for (let i = 0; i < ib.length; i += 3) {
      anchors[ib[i + 1] * mw + ib[i]] = ib[i + 2];
      feats.push([ib[i], ib[i + 1], ib[i + 2]]);
    }
    const got = TR.gatherFeatures({ lab: false, mirror: false, r0: (0 >> 4) - 16, E: [0, 0], W: [GW, GH], vpL: 0, vpT: 0,
      w16: mw, h16: mh, heights: new Uint8Array(mw * mh), anchors, mapAnchors: anchors, defs: pack.features.defs,
      atlas: pack.features.atlas, D: pack.depth });
    const quads = vs => {
      const out = [];
      for (let i = 0; i + 6 * TR.SVST <= vs.length; i += 6 * TR.SVST) {
        const a = i, b = i + 4 * TR.SVST;            // corners (x0, y0) and (x1, y1)
        out.push([vs[a], vs[a + 1], vs[b], vs[b + 1], vs[a + 2], vs[a + 6], vs[a + 7], vs[a + 8]]);
      }
      return out;
    };
    if (got) { shadow = quads(got.shadow); body = quads(got.body); }
  }
  return { gw: GW, gh: GH, cells, shadow, body, feats };
}

/* the same ground for the C side, as wikiscenetest.c reads it */
function groundText(id, dir, pack, feats) {
  const [mw, mh] = pack.cells;
  const tiles = u16(fs.readFileSync(path.join(dir, pack.map.tilemap.file)));
  const lines = [`${id} ${mw} ${mh} ${pack.map.atlas.tiles} ${feats.length}`, Array.from(tiles).join(' ')];
  const fr = r => r ? `1 ${r.w} ${r.h} ${r.hx} ${r.hy} ${r.ck} 0` : '0';
  for (const [cx, cz, k] of feats) {
    const d = pack.features.defs[String(k)];
    const flags = (d.animtrans ? 4 : 0) | (d.shadtrans ? 8 : 0);
    lines.push(`${cx} ${cz} ${d.footx} ${d.footz} ${d.height} ${flags} ${fr(d.body)} ${fr(d.shadow)}`);
  }
  return lines.join('\n') + '\n';
}

const near = (a, b) => Math.abs(a - b) <= TOL;
const F = Math.fround;   // the C side prints a float in 9 digits, which reads back as that float

/* one frame's model records: the C side's row [type, owner, ghost, shadow, nanoOn, slot, x, z, cs, sn, enc, alpha,
   nanoT, nanoC0..2, wire] against scene.js's record; null when they pair */
function pairModels(c, js) {
  const want = [...js.units, ...js.ghosts];
  if (c.nu !== js.units.length) return `${c.nu} unit records in C, ${js.units.length} in the mockup`;
  if (c.m.length !== want.length) return `${c.m.length} records in C, ${want.length} in the mockup`;
  for (let i = 0; i < want.length; i++) {
    const r = c.m[i], m = want[i], n = m.nano;
    const kind = !m.shadow ? 0 : m.slant ? 2 : 1;
    const same = r[0] === m.type && r[1] === m.owner && r[2] === +m.ghost && r[3] === kind && r[4] === +!!n &&
      r[5] === m.slot && near(r[6], m.pos[0]) && near(r[7], m.pos[1]) && near(r[8], m.yaw[0]) && near(r[9], m.yaw[1]) &&
      near(r[10], m.enc) && near(r[11], m.alpha) &&
      (!n || (r[12] === n.t && [n.c[0], n.c[1], n.c[2], n.wire].every((x, k) => F(r[13 + k]) === F(x))));
    if (!same) return `record ${i}: C ${show(r)}, mockup ${show({ ...m, files: undefined })}`;
    nmodels++;
    if (n) nnano++;
  }
  return null;
}
const show = v => JSON.stringify(v, (k, x) => typeof x === 'number' ? +x.toFixed(4) : x);

const t0 = Date.now();
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'scenecheck-'));
let code = 0, nmodels = 0, nnano = 0;
try {
  if (!fs.existsSync(path.join(PACK, 'index.json'))) throw new CannotRun(`no pack at ${PACK}: run tools/wiki/gfx.py`);
  const index = JSON.parse(fs.readFileSync(path.join(PACK, 'index.json'), 'utf8'));

  /* ---- the grounds, the mockup's way, and the file the C side reads them from ---- */
  const grounds = {}, packs = {};
  let text = '', types = '';
  for (const id of Object.keys(index.entries).sort()) {
    const dir = path.join(PACK, id), pack = JSON.parse(fs.readFileSync(path.join(dir, 'scene.json'), 'utf8'));
    packs[id] = pack;
    const meshes = Object.entries((pack.units || {}).meshes || {});
    types += `${id} ${meshes.length}\n` + meshes.map(([t, m]) => `${t} ${+!!m.mobile} ${+!!m.slant} ${+!!m.shadow}\n`).join('');
    if (!pack.ground.ok) { console.log(`scenecheck: ${id}: the pack's ground fell back (${pack.ground.miss}) - not paired`); continue; }
    grounds[id] = mockupGround(dir, pack);
    text += groundText(id, dir, pack, grounds[id].feats);
  }
  fs.writeFileSync(path.join(tmp, 'grounds.txt'), text);
  fs.writeFileSync(path.join(tmp, 'types.txt'), types);

  /* ---- the C side, built natively with the sanitizers ---- */
  const exe = path.join(DDRAW, 'tests/wikiscenetest-native.exe');   // `.exe` so the tree's *.exe ignore covers it
  run('gcc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc', '-O1', '-g', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all', '-o', exe, 'tests/wikiscenetest.c', ...SRCS, '-lm'], { cwd: DDRAW });
  const pak = path.join(tmp, 'wiki.bin');
  run('python3', [path.join(ROOT, 'tools/hudpak.py'), '--set', 'wiki', path.join(ROOT, 'art/wiki'), pak]);
  const cout = run(exe, [pak, path.join(tmp, 'grounds.txt'), path.join(tmp, 'types.txt')]).trim().split('\n').map(l => JSON.parse(l));

  /* ---- the grounds paired ---- */
  let ncell = 0, nquad = 0, nground = 0;
  for (const c of cout.filter(l => l.ground)) {
    const js = grounds[c.e], g = c.ground;
    const bad = [];
    if (!js) bad.push('no mockup ground');
    else {
      if (g.refused) bad.push('the C side refused it');
      if (g.gw !== js.gw || g.gh !== js.gh) bad.push(`target ${g.gw}x${g.gh}, the mockup's ${js.gw}x${js.gh}`);
      if (g.cells.length !== js.cells.length) bad.push(`${g.cells.length} cells, the mockup's ${js.cells.length}`);
      g.cells.forEach((cell, i) => {
        if (!bad.length && js.cells[i] && cell.some((v, k) => v !== js.cells[i][k]))
          bad.push(`cell ${i}: C ${show(cell)}, mockup ${show(js.cells[i])}`);
      });
      for (const b of ['shadow', 'body']) {
        if (g[b].length !== js[b].length) { bad.push(`${g[b].length} ${b} quads, the mockup's ${js[b].length}`); continue; }
        g[b].forEach((q, i) => {
          const m = js[b][i], want = [q[0], q[1], q[2], q[3], q[4], q[5], q[8], q[9]];
          if (!bad.length && want.some((v, k) => !near(v, m[k]))) bad.push(`${b} quad ${i}: C ${show(want)}, mockup ${show(m)}`);
        });
        nquad += g[b].length;
      }
      ncell += g.cells.length;
    }
    nground++;
    if (bad.length) { code = 1; console.log(`DIFFER ${c.e} ground: ${bad.slice(0, 4).join('; ')}`); }
  }
  if (nground !== Object.keys(grounds).length) {
    code = 1;
    console.log(`DIFFER: the C side wrote ${nground} grounds, the mockup has ${Object.keys(grounds).length}`);
  }

  /* ---- every demo's fog, at the C side's ticks ---- */
  const cfog = new Map(cout.filter(l => !l.ground).map(l => [`${l.e}/${l.d}/${l.t}`, l]));
  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  let nframes = 0, nfogged = 0;
  for (const id of [...new Set(tree.tree.flatMap(leaves))]) {
    const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
    for (const demo of entry.demos) {
      const sim = WikiSim.play(entry, demo);
      for (;;) {
        if (sim.tick % 30 === 0 || sim.done) {
          const key = `${id}/${demo.id}/${sim.tick}`, c = cfog.get(key), v = sim.view();
          const js = WikiScene.fogGrid(v.map.fog, +v.map.w || 0, +v.map.h || 0);
          let bad = null;
          if (!c) bad = 'no C frame';
          else if (!js !== !c.fog) bad = `fog ${c.fog ? 'on' : 'off'} in C, ${js ? 'on' : 'off'} in the mockup`;
          else if (js) {
            if (c.fog.cols !== js.dim[0] || c.fog.rows !== js.dim[1]) bad = `grid ${c.fog.cols}x${c.fog.rows}, the mockup's ${js.dim}`;
            else if (c.fog.org[0] !== js.org[0] || c.fog.org[1] !== js.org[1]) bad = `origin ${c.fog.org}, the mockup's ${js.org}`;
            else {
              const i = c.fog.grid.findIndex((b, k) => b !== js.grid[k]);
              if (i >= 0) bad = `block ${i >> 1} byte ${i & 1}: C ${c.fog.grid[i]}, mockup ${js.grid[i]}`;
            }
            nfogged++;
          }
          nframes++;
          if (bad) { code = 1; console.log(`DIFFER ${key} fog: ${bad}`); }
          // the stage the demo plays on, `<entry>.<scene>`: the pack the mockup draws it from
          const sid = `${id}.${typeof demo.on === 'string' ? demo.on : Object.keys(entry.scenes)[0]}`;
          if (c && packs[sid]) {
            const why = pairModels(c, WikiScene.build(v, packs[sid], sim.tick));
            if (why) { code = 1; console.log(`DIFFER ${key} models: ${why}`); }
          }
          cfog.delete(key);
        }
        if (sim.done) break;
        sim.step();
      }
    }
  }
  if (cfog.size) { code = 1; console.log(`DIFFER: ${cfog.size} C frames the mockup did not play, e.g. ${[...cfog.keys()][0]}`); }
  const s = ((Date.now() - t0) / 1000).toFixed(1);
  if (code) console.log(`scenecheck: DIFFER (${s} s)`);
  else console.log(`scenecheck: all equal -- ${nground} grounds (${ncell} cells exactly, ${nquad} feature quads within 1/64), ` +
                   `fog on ${nframes} frames (${nfogged} fogged) byte for byte, ${nmodels} model records (${nnano} nanoframes) (${s} s)`);
} catch (x) {
  if (!(x instanceof CannotRun)) throw x;
  console.log(`scenecheck: cannot run: ${x.message}`);
  code = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
process.exit(code);
