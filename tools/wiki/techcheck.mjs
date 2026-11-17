/* techcheck.mjs -- the tech tree's drawing: tagpu/ddraw/src/tagpu_wikitechdraw.c's ops against the mockup's
   tools/wiki/mockup/techtree.js `draw`, call for op (research/notes/ingame-wiki-techtree-plan.md, T2).

   usage: node tools/wiki/techcheck.mjs [MOD ...]       (default: every pack under tools/wiki/mockup/gfx/side/)
     exit 0 when every case pairs, 1 when one differs, 2 when the check cannot run

   For every side of each mod's tech tree pack (gfx/side/<mod>/techtree/, gitignored: tools/wiki/sidepacks.py writes it
   from the install, and tools/wiki/assetcheck.py --side proves it equal to the game's own load), a set of CASES: the
   tree fitted to two stages at two scales, with nothing selected and with the commander selected and every link; the
   tree at four times 1:1 on the commander's card with an upgrade (or the last card) selected; and at 1:1 with an icon
   selected; and at a set of tree points (each card's centre, corner and first icon, an icon gap) the hit test, techtree.js
   `hit` against tagpu_wikitech_hit. techtree.js draws each case into a recording context; a native build of tagpu_wikitechdraw.c
   (tagpu/ddraw/tests/wikitechtest.c) draws the same case into ops. The two are paired one for one: kinds, colours
   (rgba in 1/255ths, the canvas's globalAlpha folded in), strings and pictures equal, every coordinate within 1/64 of a
   pixel. A canvas call is read as tools/wiki/stagecheck.mjs reads it: fillRect a rect, an undashed strokeRect a ring
   (the band half the width either side of its edges), a stroked polyline its pieces carried half the width into each
   corner, dashed by the canvas dash rule from its first point; fillText a line of text on the alphabetic baseline; a
   drawImage of a unit's picture its picture, filtered or not.

   THE TEXT'S MEASURE is the DLL's: the tree's three plain styles of art/hud/layout.json (tech_title, tech_body,
   tech_label), measured by the C side with tagpu_hudtext_width; the recording's measureText answers each face's advance
   at a size of one, which the C side measures first for every string a dry run of the recording asks for, times the
   size. So techtree.js's one-line clips and their ellipses are checked against the widths the game will set. */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { TASIM_SRCS } from './tasimsrcs.mjs';   // the shared simulation core tagpu_wikisim.c calls
const require = createRequire(import.meta.url);
const WikiTech = require('./mockup/techtree.js');

const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const DDRAW = path.join(ROOT, 'tagpu/ddraw'), PACKS = path.join(ROOT, 'tools/wiki/mockup/gfx/side');
const SRCS = ['src/tagpu_wikitech.c', 'src/tagpu_wikitechdraw.c', 'src/tagpu_wikistage.c', 'src/tagpu_wikisim.c', ...TASIM_SRCS,
  'src/tagpu_wikidata.c', 'src/tagpu_json.c',
  'src/tagpu_pak.c', 'src/tagpu_placegeo.c', 'src/tagpu_sweepgeo.c', 'src/tagpu_hudtext.c', 'src/tagpu_wikicob.c', 'src/tagpu_cobvm.c',
  'src/tagpu_cob_safe.c', 'src/tagpu_wikiframe.c', 'src/tagpu_wikireplay.c', 'src/tagpu_hudlay.c'];
const TOL = 1 / 64;
// the stage's op list, tagpu_wikistage.h WIKI_MAXOP, read from the header so the budget is the DLL's own
const WIKI_MAXOP = +/#define WIKI_MAXOP\s+(\d+)/.exec(fs.readFileSync(path.join(DDRAW, 'src/tagpu_wikistage.h'), 'utf8'))[1];
const ROLES = ['title', 'body', 'label'];

class CannotRun extends Error {}
function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { encoding: 'utf8', maxBuffer: 1 << 30, ...opts });
  if (r.status !== 0) throw new CannotRun(`${cmd} ${args.slice(0, 2).join(' ')} failed (${r.status}): ${(r.stderr || '').slice(-2000)}`);
  return r.stdout;
}
const colour = (s, ga) => {
  const m = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(s);
  if (!m) throw new Error(`a colour the check does not read: ${s}`);
  const h = m[1].length === 3 ? [...m[1]].map(d => d + d).join('') : m[1];
  const a = h.length === 8 ? parseInt(h.slice(6), 16) / 255 : 1;
  return ((parseInt(h.slice(0, 6), 16) << 8) | Math.round(a * ga * 255)) >>> 0;
};

/* ---- the recording context: techtree.js's calls as shapes ---- */
function recorder(em, asked, W, H, U) {
  const out = [], saved = [];
  let path = [], dash = [];
  const g = {
    canvas: { width: W, height: H }, fillStyle: '#000000', strokeStyle: '#000000', lineWidth: 1, font: '', lineCap: 'butt',
    textBaseline: 'alphabetic', globalAlpha: 1, imageSmoothingEnabled: true, imageSmoothingQuality: 'low',
    shadowColor: '#00000000', shadowBlur: 0, shadowOffsetX: 0, shadowOffsetY: 0,
    save() { saved.push([g.globalAlpha, g.shadowColor, g.shadowBlur, g.shadowOffsetX, g.shadowOffsetY, g.fillStyle]); },
    restore() { [g.globalAlpha, g.shadowColor, g.shadowBlur, g.shadowOffsetX, g.shadowOffsetY, g.fillStyle] = saved.pop(); },
    // a disc: a full arc filled (HOP_DOT, as deep as its radius)
    arc(x, y, r, a0, a1) {
      if (a0 !== 0 || Math.abs(a1 - 2 * Math.PI) > 1e-12) throw new Error('an arc that is not a whole circle');
      path.push({ disc: [x, y, r] });
    },
    // a rounded rectangle: only ever filled with a shadow cast from off the canvas (techtree.js `shadow`)
    roundRect(x, y, w, h, r) { path.push({ round: [x, y, w, h, r] }); },
    fill() {
      for (const p of path) {
        if (p.round) {
          const [x, y, w, h, r] = p.round;
          if (!(colour(g.shadowColor, 1) & 255) || x + w >= 0 || g.shadowOffsetX <= 0) throw new Error('a rounded rectangle that is not a shadow cast from off the canvas');
          // its radius as the op carries it, in 1/256 units (HOPD_RADIUS)
          if (w && h) out.push({ k: 'drop', x: x + g.shadowOffsetX, y, w, h, dy: g.shadowOffsetY, blur: g.shadowBlur,
            r: Math.round(r / U * 256) / 256 * U, c: colour(g.shadowColor, g.globalAlpha) });
          continue;
        }
        if (!p.disc) throw new Error('a filled path that is not a disc');
        const [x, y, r] = p.disc;
        out.push({ k: 'dot', x: x - r, y: y - r, w: 2 * r, h: 2 * r, d: r, c: colour(g.fillStyle, g.globalAlpha) });
      }
    },
    setLineDash(d) { dash = d.length % 2 ? [...d, ...d] : [...d]; },
    beginPath() { path = []; },
    moveTo(x, y) { path.push([[x, y]]); },
    lineTo(x, y) { if (!path.length || path.at(-1).disc) path.push([]); path.at(-1).push([x, y]); },
    stroke() { for (const p of path) if (!p.disc) stroke(p, false); },
    fillRect(x, y, w, h) {
      if (w < 0) { x += w; w = -w; }
      if (h < 0) { y += h; h = -h; }
      if (colour(g.shadowColor, 1) & 255) throw new Error('a shadow cast from a plain rectangle (techtree.js casts from roundRect)');
      if (w && h) out.push({ k: 'rect', x, y, w, h, c: colour(g.fillStyle, g.globalAlpha) });
    },
    strokeRect(x, y, w, h) {
      if (!dash.length) {
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        const lw = g.lineWidth;
        out.push({ k: 'ring', x: x - lw / 2, y: y - lw / 2, w: w + lw, h: h + lw, d: lw, c: colour(g.strokeStyle, g.globalAlpha) });
      } else stroke([[x, y], [x + w, y], [x + w, y + h], [x, y + h], [x, y]], true);
    },
    measureText(t) {
      const f = font(g.font);
      if (asked) asked[f.role].add(t);
      // a string not measured yet is as wide as nothing, so the dry run that asks for it does not clip it
      return { width: t === '' || !em || !em[f.role].has(t) ? 0 : em[f.role].get(t) * f.size };
    },
    fillText(t, x, y) {
      if (g.textBaseline !== 'alphabetic') throw new Error(`text on the ${g.textBaseline} baseline`);
      if (t === '') return;
      const f = font(g.font);
      out.push({ k: 'text', x, y, size: f.size, role: f.role, c: colour(g.fillStyle, g.globalAlpha), t: String(t) });
    },
    drawImage(img, x, y, w, h) {
      out.push({ k: 'pic', name: img.name, x, y, w, h, smooth: +g.imageSmoothingEnabled, a: Math.round(g.globalAlpha * 255) });
    },
  };
  const font = s => {
    const m = /^\d+ ([\d.e+-]+)px role-(\w+)$/.exec(s);
    if (!m) throw new Error(`a font the check does not read: ${s}`);
    return { size: +m[1], role: m[2] };
  };
  // a polyline stroked by the canvas dash rule (stagecheck.mjs's): the pattern from its first point, round its corners
  const DASH_EPS = 1e-9;
  function stroke(pts, closed) {
    const lw = g.lineWidth, c = colour(g.strokeStyle, g.globalAlpha), pat = dash.length ? dash : [Infinity, 0];
    let k = 0, rem = pat[0], open = null;
    const carry = p => { const dx = p.x1 - p.x0, dy = p.y1 - p.y0, l = Math.hypot(dx, dy); p.x1 += dx / l * lw / 2; p.y1 += dy / l * lw / 2; };
    for (let i = 0; i + 1 < pts.length; i++) {
      const [ax, ay] = pts[i], [bx, by] = pts[i + 1], L = Math.hypot(bx - ax, by - ay);
      if (open) { carry(open); open = null; }
      if (!(L > 0)) continue;
      let t = 0;
      while (t < L) {
        const step = Math.min(rem, L - t);
        let piece = null;
        if (k === 0 && step > 0) {
          piece = { k: 'line', x0: ax + (bx - ax) * (t / L), y0: ay + (by - ay) * (t / L), x1: ax + (bx - ax) * ((t + step) / L),
            y1: ay + (by - ay) * ((t + step) / L), w: lw, c };
          out.push(piece);
        }
        t += step; rem -= step;
        if (rem <= DASH_EPS) { k ^= 1; rem = pat[k]; if (L - t <= DASH_EPS) t = L; }
        else if (t >= L - DASH_EPS && k === 0) { open = piece; t = L; }
      }
      if (i + 2 === pts.length && !closed) open = null;
    }
    if (open) carry(open);
  }
  return { g, out };
}

/* ---- the C ops read back as the same shapes, in the canvas's device pixels ---- */
const HOP = { SOLID: 1, DOT: 2, TEXT: 3, LINE: 5, PIC: 10 }, RING = 4, DROP = 8, BASE = 16, INK = 64;
function shapes(fr, u, roleOf) {
  const X = v => v * u - fr.x0, Y = v => v * u - fr.y0, out = [];
  for (const [kind, flags, part, x, y, w, h, cx, cy, cw, ch, rgba, size, style, centre, text, rgb, shade] of fr.ops) {
    if (Math.abs(X(cx)) > TOL || Math.abs(Y(cy)) > TOL || Math.abs(cw * u - fr.w) > TOL || Math.abs(ch * u - fr.h) > TOL)
      out.push({ k: 'clip', clip: [X(cx), Y(cy), cw * u, ch * u] });
    else if (kind === HOP.SOLID && !flags) out.push({ k: 'rect', x: X(x), y: Y(y), w: w * u, h: h * u, c: rgba });
    else if (kind === HOP.SOLID && flags === RING) out.push({ k: 'ring', x: X(x), y: Y(y), w: w * u, h: h * u, d: size * u, c: rgba });
    else if (kind === HOP.SOLID && flags === DROP)
      out.push({ k: 'drop', x: X(x), y: Y(y), w: w * u, h: h * u, dy: size * u, blur: shade * u, r: style / 256 * u, c: rgba });
    else if (kind === HOP.DOT && flags === RING) out.push({ k: 'dot', x: X(x), y: Y(y), w: w * u, h: h * u, d: size * u, c: rgba });
    else if (kind === HOP.LINE && !flags) out.push({ k: 'line', x0: X(x), y0: Y(y), x1: X(x) + w * u, y1: Y(y) + h * u, w: size * u, c: rgba });
    else if (kind === HOP.TEXT && flags === (BASE | INK))
      out.push({ k: 'text', x: X(x), y: Y(y), size: size * u, role: roleOf[style], c: ((rgb << 8) | (rgba & 255)) >>> 0, t: text });
    else if (kind === HOP.PIC && !flags) out.push({ k: 'pic', name: part, x: X(x), y: Y(y), w: w * u, h: h * u, smooth: centre, a: rgba & 255 });
    else out.push({ k: `op ${kind}/${flags}` });
  }
  return out;
}
function differ(a, b) {
  if (!a || !b || a.k !== b.k) return 'kind';
  for (const f of Object.keys(a))
    if (typeof a[f] === 'number' && f !== 'c' && f !== 'name' && f !== 'smooth' && f !== 'a' ? Math.abs(a[f] - b[f]) > TOL : a[f] !== b[f]) return f;
  return null;
}
const show = s => s ? JSON.stringify(s, (k, v) => typeof v === 'number' && k !== 'c' ? +v.toFixed(4) : k === 'c' ? v.toString(16).padStart(8, '0') : v) : 'nothing';

/* ---- the cases of a tree ---- */
function cases(tree) {
  const out = [];
  tree.sides.forEach((sd, side) => {
    const M = WikiTech.model(tree, side), L = WikiTech.layout(M), root = M.root;
    // the hit test's points: each card's centre, its first icon's centre and its corner, a point just past its corner,
    // and an icon gap's
    const hits = [];
    for (const k of M.spine) {
      const [x, y, w, h] = L.pos[k];
      const ix = x + WikiTech.C.PAD, iy = y + WikiTech.C.HEAD + WikiTech.C.IGAP;
      hits.push([x + w / 2, y + h / 2], [ix + WikiTech.C.ICON / 2, iy + WikiTech.C.ICON / 2], [ix, iy], [x + w, y + h],
        [x + w - 1e-6, y + h - 1e-6], [ix + WikiTech.C.ICON + WikiTech.C.IGAP / 2, iy + 1]);
    }
    const up = M.order.find(k => M.units[k].upgrade) || M.spine.at(-1);
    const icon = WikiTech.cardIcons(M, root)[0] || root;
    const [rx, ry] = L.pos[root];
    out.push({ side, u: 2, stage: [40, 30, 600, 340], cam: 'fit', sel: null, all: false, hits });
    out.push({ side, u: 1.5, stage: [40, 30, 800, 400], cam: 'fit', sel: root, all: true });
    // THE BUDGET: the stage at its size in the game (1404 x 842 pixels at 1080p, the dot grid's most), the whole tree
    // with every link -- the most ops a frame of the entry makes, which must fit WIKI_MAXOP (a drop is a DIFFER)
    out.push({ side, u: 1.5, stage: [40, 30, 936, 561], cam: 'fit', sel: root, all: true, budget: true });
    out.push({ side, u: 2, stage: [40, 30, 600, 340], cam: { z: 8, x: rx - 20, y: ry - 20 }, sel: up, all: false });
    out.push({ side, u: 2, stage: [40, 30, 600, 340], cam: { z: 2, x: L.w / 3, y: L.h / 3 }, sel: icon, all: true });
  });
  return out;
}

/* one case drawn by techtree.js, its canvas the C side's */
function draw(tree, c, fr, em, asked, pics) {
  const M = WikiTech.model(tree, c.side), L = WikiTech.layout(M);
  const cam = { z: fr.cam[0], x: fr.cam[1], y: fr.cam[2] };
  if (c.cam === 'fit') {
    const f = WikiTech.fit(L, fr.w, fr.h, c.u);
    for (const [k, i] of [['z', 0], ['x', 1], ['y', 2]])
      if (Math.abs(f[k] - fr.cam[i]) > 1e-9 * Math.max(1, Math.abs(f[k]))) throw new Error(`the fitted camera's ${k}: ${f[k]} != C ${fr.cam[i]}`);
  }
  const r = recorder(em, asked, fr.w, fr.h, c.u);
  WikiTech.ground(r.g, fr.w, fr.h, c.u);
  WikiTech.draw(r.g, M, L, { cam, s: c.u, sel: c.sel, all: c.all, pic: k => pics.get(k) || null, font: role => `400 1px role-${role}` });
  return r.out;
}

const t0 = Date.now();
const mods = process.argv.slice(2).length ? process.argv.slice(2) : fs.existsSync(PACKS) ? fs.readdirSync(PACKS).sort() : [];
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'techcheck-'));
let code = 0;
try {
  const have = mods.filter(m => fs.existsSync(path.join(PACKS, m, 'techtree/tree.json')));
  if (!have.length) throw new CannotRun('no tech tree pack: run tools/wiki/sidepacks.py');
  const exe = path.join(tmp, 'wikitechtest-native');
  run('gcc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc', '-O1', '-g', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all', '-o', exe, 'tests/wikitechtest.c', ...SRCS, '-lm'], { cwd: DDRAW });
  const styles = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/hud/layout.json'), 'utf8')).styles, names = Object.keys(styles);
  const sidx = ROLES.map(r => names.indexOf(`tech_${r}`));
  if (sidx.some(i => i < 0)) throw new CannotRun('art/hud/layout.json lacks a tech_* style');
  const roleOf = Object.fromEntries(ROLES.map((r, i) => [sidx[i], r]));
  const faces = ROLES.map(r => {
    const S = styles[`tech_${r}`];
    return `${path.join(ROOT, 'art/hud/fonts', `${S.font.toLowerCase().replace(/ /g, '')}-${S.weight}.ttf`)}:${S.spacing || 0}:${S.upper ? 1 : 0}`;
  });
  for (const mod of mods) {
    if (!have.includes(mod)) { console.log(`techcheck: ${mod}: no tech tree pack, left out`); continue; }
    const dir = path.join(PACKS, mod, 'techtree'), tree = JSON.parse(fs.readFileSync(path.join(dir, 'tree.json'), 'utf8'));
    const pics = new Map();
    let np = 0;
    for (const [k, u] of Object.entries(tree.units)) if (u.pic) {
      const png = fs.readFileSync(path.join(dir, u.pic));
      pics.set(k, { name: np++, width: png.readUInt32BE(16) });
    }
    const cs = cases(tree), casesFile = path.join(tmp, `${mod}.cases.json`);
    fs.writeFileSync(casesFile, JSON.stringify(cs));
    // the C side's frames first: each case's canvas and camera, which the recording takes
    const draws = (em) => run(exe, ['draw', path.join(dir, 'tree.json'), dir, casesFile, ...faces, sidx.join(',')])
      .trim().split('\n').map(l => JSON.parse(l));
    const frames = draws();
    // the measure: every string a dry run asks for, by the C side. A clip asks for the strings it shortens only once
    // it has the full string's width, so the dry run is repeated with the widths it has until it asks for nothing new.
    const em = { title: new Map(), body: new Map(), label: new Map() };
    for (let round = 0; ; round++) {
      const asked = { title: new Set(), body: new Set(), label: new Set() };
      cs.forEach((c, i) => draw(tree, c, frames[i], em, asked, pics));
      const fresh = ROLES.map(r => [...asked[r]].filter(s => s !== '' && !em[r].has(s)));
      if (!fresh.some(f => f.length)) break;
      if (round > 400) throw new CannotRun('the measure did not settle in 400 rounds');
      ROLES.forEach((r, i) => {
        const strings = fresh[i];
        if (!strings.length) return;
        if (strings.some(t => t.includes('\n'))) throw new CannotRun('a measured string holds a line break');
        const f = path.join(tmp, `${mod}.${r}.txt`);
        fs.writeFileSync(f, strings.join('\n') + '\n');
        const S = styles[`tech_${r}`];
        const v = run(exe, ['measure', faces[i].split(':')[0], String(S.spacing || 0), S.upper ? '1' : '0', f]).trim().split('\n').map(Number);
        if (v.length !== strings.length) throw new CannotRun(`the C side measured ${v.length} of ${strings.length} ${r} strings`);
        strings.forEach((t, j) => em[r].set(t, v[j]));
      });
    }
    let nops = 0, nd = 0, ntext = 0, npic = 0, nhit = 0, most = 0;
    cs.forEach((c, i) => {
      const fr = frames[i];
      if (fr.dropped) { code = 1; nd++; console.log(`DIFFER ${mod} case ${i}: the C side dropped ${fr.dropped} ops`); return; }
      if (c.hits) {
        const M = WikiTech.model(tree, c.side), L = WikiTech.layout(M), at = new Map(M.order.map((k, j) => [k, j]));
        c.hits.forEach(([px, py], j) => {
          const h = WikiTech.hit(M, L, px, py), w = h ? [at.get(h.key), at.get(h.card), +h.icon] : [-1, -1, 0];
          nhit++;
          if (w.join() !== fr.hits[j].join()) {
            code = 1; nd++;
            console.log(`DIFFER ${mod} side ${c.side} hit at ${px},${py}: techtree.js ${JSON.stringify(w)}, C ${JSON.stringify(fr.hits[j])}`);
          }
        });
      }
      const want = draw(tree, c, fr, em, null, pics), got = shapes(fr, c.u, roleOf);
      nops += got.length; ntext += got.filter(s => s.k === 'text').length; npic += got.filter(s => s.k === 'pic').length;
      if (c.budget) most = Math.max(most, fr.ops.length);
      const n = Math.max(want.length, got.length);
      for (let j = 0; j < n; j++) {
        const f = differ(want[j], got[j]);
        if (f) {
          code = 1; nd++;
          console.log(`DIFFER ${mod} case ${i} (${JSON.stringify(c)}) shape ${j} of ${want.length}/${got.length} in ${f}:\n  techtree.js ${show(want[j])}\n  C           ${show(got[j])}`);
          break;
        }
      }
    });
    console.log(`techcheck: ${mod}: ${cs.length} cases over ${tree.sides.length} sides, ${nops} ops (${ntext} texts, ${npic} pictures), ${nhit} hits` +
      `, the game's stage with every link ${most} ops of its ${WIKI_MAXOP}` + (nd ? `, ${nd} DIFFER` : ', every op paired'));
  }
  console.log(`techcheck: ${code ? 'DIFFER' : 'all equal'} -- ${((Date.now() - t0) / 1000).toFixed(1)} s`);
} catch (e) {
  if (!(e instanceof CannotRun)) throw e;
  console.log(`techcheck: CANNOT RUN: ${e.message}`);
  code = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
process.exit(code);
