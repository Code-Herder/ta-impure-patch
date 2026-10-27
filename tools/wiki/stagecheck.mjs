/* stagecheck.mjs -- the in-game wiki stage's check: tagpu/ddraw/src/tagpu_wikistage.c's ops against the mockup's
   tools/wiki/mockup/stage.js, call for op.

   usage: node tools/wiki/stagecheck.mjs
     exit 0 when every frame pairs, 1 when one differs, 2 when the C side cannot be built or run

   For every demo of every entry, at tick 0, every 30th tick and its last, stage.js draws sim.js's view into a recording
   context, and a native build of tagpu_wikistage.c (tagpu/ddraw/tests/wikistagetest.c, over tagpu_wikisim.c's view
   copied by tagpu_wikistage_copy) builds the same view's ops; both on the same stage, at three sizes: the mockup's
   stage at 1920 x 1080 and at 1024 x 768 (MEASURED 2026-10-25 in the mockup, the canvas's size and its corner in the
   frame's device pixels), and one small enough that the grid is left out and every label shrinks.

   THE RECORDING turns each canvas call into the shapes the ops draw, by the canvas's own rules: a fillRect a rect, its
   size normalised; an undashed strokeRect a ring, the band half the width either side of its edges; a stroked subpath
   one line a straight piece, dashed by the canvas dash rule from its first point, a corner inside a dash carrying the
   piece into it half the width (a right angle's miter), and a closed subpath on at its end joining round its first
   corner; an arc filled a circle as deep as its radius and stroked a ring; fillText a line of text; drawImage an
   icon. Each op is read back into the same shapes in the canvas's device pixels, and the two lists are paired one for
   one: kinds, colours (rgba in 1/255ths) and strings equal, every coordinate within 1/64 of a pixel. Every op must
   also clip to the canvas.

   THE TEXT'S MEASURE is the DLL's: the C side measures with tagpu_hudtext_width over the face of layout.json's `toast`
   style, and the recording's measureText answers that face's advance at a size of one, which the C side measures
   first for every string a dry run of the recording asks for, times the size. stage.js's choices -- a label shrunk
   to its box, a readout's plate -- are thus checked against the same widths the game will set.

   THE THREE RANGES (research/notes/ingame-wiki-3d-stage-plan.md, section 3). The pairing above is R1, the whitebox
   stage. stage.js names the range of every call (o.layer), so each C op, paired with its call, carries the same name;
   then each frame is drawn again over a stand-in 3D image, three ways -- every model resolved with the ground in the
   image, none resolved, and the ground fallen back -- and that recording, R0 and R2 with the image between them, must
   pair op for op with R1's ops kept by the same rule: R0 and the overlays always, the ground when the image has none,
   a box when its type did not resolve, a whitebox label never. R0 and R2 are thus R1's own ops, in R1's order, and a
   DLL that builds R1 builds them by the names alone -- which the C ops carry (WIKISTAGE_IN_*): each op's range must
   be its call's, tagpu_wikistage_keep must put each op where stage.js draws it under each image, and the image's
   rect must be tagpu_wikistage_image's. */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const WikiSim = require('./mockup/sim.js');
const WikiStage = require('./mockup/stage.js');
const ROOT = new URL('../../', import.meta.url).pathname;
const DDRAW = path.join(ROOT, 'tagpu/ddraw');
const TOL = 1 / 64;
const SRCS = ['src/tagpu_wikistage.c', 'src/tagpu_wikisim.c', 'src/tagpu_wikidata.c', 'src/tagpu_json.c', 'src/tagpu_pak.c',
  'src/tagpu_placegeo.c', 'src/tagpu_hudtext.c'];
// the stages: device pixels a unit, the canvas's corner and size in device pixels
const SIZES = [
  { name: '1920x1080', u: 1.5, x0: 759, y0: 146, w: 1045, h: 532 },
  { name: '1024x768', u: 768 / 720, x0: 409, y0: 104, w: 550, h: 378 },
  { name: 'small', u: 1, x0: 37, y0: 21, w: 200, h: 130 },
];

class CannotRun extends Error {}
function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { maxBuffer: 1 << 30, encoding: 'utf8', ...opts });
  if (r.error || r.status !== 0)
    throw new CannotRun(`${cmd} ${args.join(' ')} failed${r.error ? `: ${r.error.message}` : ` (${r.status})`}\n${r.stderr || ''}`);
  return r.stdout;
}

/* ---- the colours as the canvas parses them, as 0xRRGGBBAA ---- */
function colour(s) {
  let m;
  if ((m = /^#([0-9a-f]{6})$/i.exec(s))) return (parseInt(m[1], 16) * 256 + 255) >>> 0;
  if ((m = /^rgba?\(([^)]*)\)$/.exec(s))) {
    const v = m[1].split(',').map(Number), a = v.length > 3 ? Math.round(v[3] * 255) : 255;
    return (((Math.round(v[0]) << 24) | (Math.round(v[1]) << 16) | (Math.round(v[2]) << 8) | a) >>> 0);
  }
  throw new Error(`a colour the check does not read: ${s}`);
}

/* ---- the recording context ---- */
function recorder(em, asked) {
  const out = [], tags = [];
  let path = [], dash = [], tag = null;
  // every shape recorded carries the range the call that made it was drawn in
  const push = out.push.bind(out);
  out.push = sh => { tags.push(tag); return push(sh); };
  const g = {
    fillStyle: '#000', strokeStyle: '#000', lineWidth: 1, font: '10px sans-serif', textAlign: 'start', textBaseline: 'alphabetic',
    imageSmoothingQuality: 'low', imageSmoothingEnabled: true,
    setTransform(a, b, c, d, e, f) { if (a !== 1 || b || c || d !== 1 || e || f) throw new Error('a transform the check does not read'); },
    setLineDash(d) { dash = d.length % 2 ? [...d, ...d] : [...d]; },
    beginPath() { path = []; },
    moveTo(x, y) { path.push({ pts: [[x, y]], closed: false }); },
    lineTo(x, y) { if (!path.length || path.at(-1).arc) path.push({ pts: [], closed: false }); path.at(-1).pts.push([x, y]); },
    closePath() { if (path.length) path.at(-1).closed = true; },
    arc(cx, cy, r, a0, a1) {
      if (path.length && !path.at(-1).arc) throw new Error('an arc after a line, which the check does not read');
      if (a0 !== 0 || Math.abs(a1 - 2 * Math.PI) > 1e-12) throw new Error('a partial arc');
      path.push({ arc: true, cx, cy, r });
    },
    fill() {
      for (const p of path) {
        if (!p.arc) throw new Error('a filled polygon, which the check does not read');
        if (p.r > 0) out.push({ k: 'circle', cx: p.cx, cy: p.cy, r: p.r, d: p.r, c: colour(g.fillStyle) });
      }
    },
    stroke() {
      for (const p of path) {
        if (p.arc) { if (p.r > 0) out.push({ k: 'circle', cx: p.cx, cy: p.cy, r: p.r + g.lineWidth / 2, d: g.lineWidth, c: colour(g.strokeStyle) }); }
        else stroke(p.closed ? [...p.pts, p.pts[0]] : p.pts, p.closed);
      }
    },
    fillRect(x, y, w, h) {
      if (w < 0) { x += w; w = -w; }
      if (h < 0) { y += h; h = -h; }
      if (w && h) out.push({ k: 'rect', x, y, w, h, c: colour(g.fillStyle) });
    },
    strokeRect(x, y, w, h) {
      if (!dash.length) {
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        const lw = g.lineWidth;
        out.push({ k: 'ring', x: x - lw / 2, y: y - lw / 2, w: w + lw, h: h + lw, d: lw, c: colour(g.strokeStyle) });
      } else stroke([[x, y], [x + w, y], [x + w, y + h], [x, y + h], [x, y]], true);
    },
    measureText(t) {
      const sz = font(g.font).size;
      if (asked) asked.add(t);
      return { width: t === '' ? 0 : em ? em.get(t) * sz : 0 };
    },
    fillText(t, x, y) {
      if (g.textBaseline !== 'middle') throw new Error(`text on the ${g.textBaseline} baseline`);
      if (t === '') return;
      const f = font(g.font);
      out.push({ k: 'text', x, y, align: { left: 0, start: 0, center: 1, right: 2, end: 2 }[g.textAlign], size: f.size,
        face: `${f.family} ${f.weight}`, c: colour(g.fillStyle), t: String(t) });
    },
    drawImage(img, x, y, w, h) { out.push({ k: 'icon', name: img.name, x, y, w, h }); },
  };
  const layer = (range, type) => { tag = { range, type }; };
  const font = s => { const m = /^(\d+) ([\d.e+-]+)px (.+)$/.exec(s); if (!m) throw new Error(`a font the check does not read: ${s}`);
    return { weight: +m[1], size: +m[2], family: m[3] }; };
  /* a polyline stroked by the canvas dash rule: the pattern from its first point, running on round its corners */
  function stroke(pts, closed) {
    const lw = g.lineWidth, c = colour(g.strokeStyle), pat = dash.length ? dash : [Infinity, 0];
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
        if (rem <= 0) { k ^= 1; rem = pat[k]; }
        else if (t >= L && k === 0) open = piece;
      }
      if (i + 2 === pts.length && !closed) open = null;
    }
    if (open) carry(open);
  }
  return { g, out, tags, layer };
}

/* ---- the ops read back as the same shapes, in the canvas's device pixels ---- */
const HOP = { PART: 0, SOLID: 1, DOT: 2, TEXT: 3, LINE: 5 }, RING = 4;
function shapes(fr, sz, face, part, icons) {
  const u = sz.u, X = v => v * u - fr.x0, Y = v => v * u - fr.y0, out = [];
  for (const [kind, flags, pi, x, y, w, h, cx, cy, cw, ch, rgba, size, style, centre, text, rgb] of fr.ops) {
    const clip = [X(cx), Y(cy), cw * u, ch * u];
    if (Math.abs(clip[0]) > TOL || Math.abs(clip[1]) > TOL || Math.abs(clip[2] - fr.w) > TOL || Math.abs(clip[3] - fr.h) > TOL)
      out.push({ k: 'clip', clip });
    else if (kind === HOP.SOLID && !flags) out.push({ k: 'rect', x: X(x), y: Y(y), w: w * u, h: h * u, c: rgba });
    else if (kind === HOP.SOLID && flags === RING) out.push({ k: 'ring', x: X(x), y: Y(y), w: w * u, h: h * u, d: size * u, c: rgba });
    else if (kind === HOP.DOT && flags === RING) out.push({ k: 'circle', cx: X(x) + w * u / 2, cy: Y(y) + h * u / 2, r: w * u / 2, d: size * u, c: rgba });
    else if (kind === HOP.LINE && !flags) out.push({ k: 'line', x0: X(x), y0: Y(y), x1: X(x) + w * u, y1: Y(y) + h * u, w: size * u, c: rgba });
    else if (kind === HOP.TEXT && !flags)
      out.push({ k: 'text', x: X(x), y: Y(y), align: centre, size: size * u, face: face[style], c: ((rgb << 8) | (rgba & 255)) >>> 0, t: text });
    else if (kind === HOP.PART && !flags && icons[part[pi]]) {
      // tagpu_hudlay_expand's icon: the part as large as fits the op's box, centred in it
      const ic = icons[part[pi]], bw = w * u, bh = h * u, f = Math.min(bw / ic.pw, bh / ic.ph), iw = ic.pw * f, ih = ic.ph * f;
      out.push({ k: 'icon', name: part[pi], x: X(x) + (bw - iw) / 2, y: Y(y) + (bh - ih) / 2, w: iw, h: ih });
    } else out.push({ k: `op ${kind}/${flags}` });
  }
  return out;
}

/* the first field two shapes differ in, or null */
function differ(a, b) {
  if (!a || !b || a.k !== b.k) return 'kind';
  for (const f of Object.keys(a)) {
    if (typeof a[f] === 'number' && f !== 'c' && f !== 'align' ? Math.abs(a[f] - b[f]) > TOL : a[f] !== b[f]) return f;
  }
  return null;
}
const show = s => s ? JSON.stringify(s, (k, v) => typeof v === 'number' && k !== 'c' ? +v.toFixed(4) : k === 'c' ? v.toString(16).padStart(8, '0') : v) : 'nothing';

const t0 = Date.now();
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'stagecheck-'));
let code = 0;
try {
  /* ---- the C side, built natively with the sanitizers ---- */
  const exe = path.join(DDRAW, 'tests/wikistagetest-native.exe');   // `.exe` so the tree's *.exe ignore covers it
  run('gcc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Isrc', '-Iinc', '-O1', '-g', '-fsanitize=address,undefined',
    '-fno-sanitize-recover=all', '-o', exe, 'tests/wikistagetest.c', ...SRCS, '-lm'], { cwd: DDRAW });
  const pak = path.join(tmp, 'wiki.bin');
  run('python3', [path.join(ROOT, 'tools/hudpak.py'), '--set', 'wiki', path.join(ROOT, 'art/wiki'), pak]);

  /* ---- the stage's text style, by its index in layout.json, and its face's file ---- */
  const styles = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/hud/layout.json'), 'utf8')).styles;
  const names = Object.keys(styles), style = names.indexOf(WikiStage.FACE.style), S = styles[WikiStage.FACE.style];
  if (style < 0) throw new CannotRun(`layout.json has no style ${WikiStage.FACE.style}`);
  if (S.font !== WikiStage.FACE.family || S.weight !== WikiStage.FACE.weight || S.spacing || S.upper || S.shadow || S.outline)
    throw new CannotRun(`stage.js sets ${WikiStage.FACE.family} ${WikiStage.FACE.weight} plain, layout.json's ${WikiStage.FACE.style} is ${JSON.stringify(S)}`);
  const face = Object.fromEntries(names.map((n, i) => [i, `${styles[n].font} ${styles[n].weight}`]));
  const ttf = path.join(ROOT, 'art/hud/fonts', `${S.font.toLowerCase().replace(/ /g, '')}-${S.weight}.ttf`);

  /* ---- the cursor icons, from the kit: indices for the C side, sizes for both ---- */
  const kitDir = path.join(ROOT, 'art/wiki/kit'), kit = JSON.parse(fs.readFileSync(path.join(kitDir, 'kit.json'), 'utf8')).parts;
  const part = Object.keys(kit), icons = {};
  for (const name of Object.values(WikiStage.CURSOR)) {
    const png = fs.readFileSync(path.join(kitDir, 'parts', `${name}.png`)), pw = png.readUInt32BE(16), ph = png.readUInt32BE(20);
    icons[name] = { img: { name }, pw, ph, w: pw / kit[name].scale, h: ph / kit[name].scale };
  }
  const cursorArgs = ['arrow', 'move', 'build'].map(s => String(part.indexOf(WikiStage.CURSOR[s])));
  const sizeArgs = ['arrow', 'move', 'build'].flatMap(s => { const ic = icons[WikiStage.CURSOR[s]]; return [ic.w, ic.h].map(v => v.toPrecision(17)); });

  /* ---- sim.js's frames, at the C side's ticks ---- */
  const tree = JSON.parse(fs.readFileSync(path.join(ROOT, 'art/wiki/tree.json'), 'utf8'));
  const leaves = n => n.entry ? [n.entry] : (n.children || []).flatMap(leaves);
  const frames = [];
  for (const id of [...new Set(tree.tree.flatMap(leaves))]) {
    const entry = JSON.parse(fs.readFileSync(path.join(ROOT, `art/wiki/entries/${id}.json`), 'utf8'));
    for (const demo of entry.demos) {
      const sim = WikiSim.play(entry, demo);
      for (;;) {
        if (sim.tick % 30 === 0 || sim.done) frames.push({ key: `${id}/${demo.id}/${sim.tick}`, v: sim.view() });
        if (sim.done) break;
        sim.step();
      }
    }
  }
  const draw = (v, sz, em, asked, image) => {
    const r = recorder(em, asked);
    const im = image && { ...image, draw: (g, rect) => r.out.push({ k: 'image', ...rect }) };
    WikiStage.draw(r.g, v, { W: sz.w, H: sz.h, U: sz.u, icons, layer: r.layer, image: im });
    return r;
  };
  // the stand-in images: what a complete 3D frame may carry
  const IMAGES = [
    { name: 'all resolved', ground: true, resolved: () => true },
    { name: 'none resolved', ground: true, resolved: () => false },
    { name: 'ground fallen back', ground: false, resolved: () => true },
  ];
  const RANGES = ['r0', 'ground', 'box', 'label', 'over'];        // WIKISTAGE_IN_*
  const kept = (t, im) => t.range === 'r0' || t.range === 'over' || (t.range === 'ground' && !im.ground) ||
    (t.range === 'box' && !im.resolved(t.type));
  const pair = (js, c, what) => {
    for (let i = 0; i < Math.max(js.length, c.length); i++) {
      const d = differ(js[i], c[i]);
      if (d) return `${what} call ${i} of ${js.length} (C ops ${c.length}), ${d}:\n  stage.js ${show(js[i])}\n  C        ${show(c[i])}`;
    }
    return null;
  };

  /* ---- the measure: every string a dry run asks for, measured by the C side ---- */
  const asked = new Set();
  for (const sz of SIZES) for (const f of frames) draw(f.v, sz, null, asked);
  const strings = [...asked].filter(s => s !== '');
  if (strings.some(s => s.includes('\n'))) throw new CannotRun('a measured string holds a line break');
  fs.writeFileSync(path.join(tmp, 'strings.txt'), strings.join('\n') + '\n');
  const ems = run(exe, ['measure', ttf, String(S.spacing || 0), S.upper ? '1' : '0', path.join(tmp, 'strings.txt')]).trim().split('\n').map(Number);
  if (ems.length !== strings.length) throw new CannotRun(`the C side measured ${ems.length} of ${strings.length} strings`);
  const em = new Map(strings.map((s, i) => [s, ems[i]]));

  /* ---- each size: the C side's frames, paired with stage.js's ---- */
  let nframes = 0, nops = 0, ndiffer = 0, nsplit = 0, nimage = 0;
  for (const sz of SIZES) {
    const rect = [sz.x0 / sz.u, sz.y0 / sz.u, sz.w / sz.u, sz.h / sz.u].map(v => v.toPrecision(17));
    const cout = run(exe, ['ops', pak, ttf, String(S.spacing || 0), S.upper ? '1' : '0', String(style), ...cursorArgs, ...sizeArgs,
      ...rect, sz.u.toPrecision(17)]).trim().split('\n').map(l => JSON.parse(l));
    const cby = new Map(cout.map(f => [`${f.e}/${f.d}/${f.t}`, f]));
    if (cout.length !== frames.length) { code = 1; ndiffer++; console.log(`DIFFER ${sz.name}: the C side drew ${cout.length} frames, stage.js ${frames.length}`); }
    for (const f of frames) {
      const fr = cby.get(f.key);
      if (!fr) { code = 1; ndiffer++; console.log(`DIFFER ${sz.name} ${f.key}: no C frame`); continue; }
      if (fr.x0 !== sz.x0 || fr.y0 !== sz.y0 || fr.w !== sz.w || fr.h !== sz.h) {
        code = 1; ndiffer++; console.log(`DIFFER ${sz.name} ${f.key}: the canvas is ${fr.w}x${fr.h} at ${fr.x0},${fr.y0}`); continue;
      }
      if (fr.dropped || fr.trunc) { code = 1; ndiffer++; console.log(`DIFFER ${sz.name} ${f.key}: dropped ${fr.dropped} ops, truncated ${fr.trunc}`); continue; }
      const r1 = draw(f.v, sz, em, null), c = shapes(fr, sz, face, part, icons);
      nframes++; nops += c.length;
      let bad = pair(r1.out, c, 'R1');
      if (!bad && r1.tags.some(t => !t)) bad = 'R1: a call drawn before stage.js named its range';
      // the C ops' own ranges, which the DLL draws R0 and R2 by: each the range of the call it paired with, a box's
      // with its type
      for (let i = 0; !bad && i < c.length; i++) {
        const t = r1.tags[i], [rng, type] = fr.rng[i] || [];
        if (RANGES[rng] !== t.range || (t.range === 'box' && type !== t.type))
          bad = `R1 op ${i}: C range ${RANGES[rng]}${type ? ` (${type})` : ''}, stage.js ${t.range}${t.type ? ` (${t.type})` : ''}`;
      }
      // R0, the image, R2: R1's ops kept by the rule, the image after R0 and the ground
      for (const im of bad || !f.v || !f.v.map ? [] : IMAGES) {
        const r = draw(f.v, sz, em, null, im), at = r.out.findIndex(sh => sh.k === 'image');
        if (at < 0 || r.out.filter(sh => sh.k === 'image').length !== 1) { bad = `${im.name}: the image is drawn ${at < 0 ? 'never' : 'twice'}`; break; }
        const js = r.out.filter(sh => sh.k !== 'image'), want = c.filter((_, i) => kept(r1.tags[i], im));
        const under = r.tags.slice(0, at).every(t => t.range === 'r0' || t.range === 'ground');
        bad = !under ? `${im.name}: an overlay drawn under the image` : pair(js, want, `R0+R2 (${im.name})`);
        // tagpu_wikistage_keep: under the image, over it or not at all, op for op as stage.js draws it
        const k = IMAGES.indexOf(im), codes = r1.tags.map(t => !kept(t, im) ? '0' : t.range === 'r0' || t.range === 'ground' ? '1' : '2').join('');
        if (!bad && fr.keep[k] !== codes) {
          const i = [...codes].findIndex((ch, j) => ch !== fr.keep[k][j]);
          bad = `${im.name}: tagpu_wikistage_keep puts op ${i} ${'-uo'[+fr.keep[k][i]]}, stage.js ${'-uo'[+codes[i]]}`;
        }
        // the image's rect: the composite's, from the same fit (tagpu_wikistage_image)
        const ir = r.out[at], cr = [fr.img[0] - sz.x0, fr.img[1] - sz.y0, fr.img[2] - fr.img[0], fr.img[3] - fr.img[1]];
        if (!bad && [ir.x, ir.y, ir.w, ir.h].some((v, j) => v !== cr[j]))
          bad = `${im.name}: the image at ${show([ir.x, ir.y, ir.w, ir.h])}, tagpu_wikistage_image ${show(cr)}`;
        if (bad) break;
        nsplit++; nimage += want.length;
      }
      if (bad) { code = 1; ndiffer++; console.log(`DIFFER ${sz.name} ${f.key} ${bad}`); }
    }
  }
  const s = ((Date.now() - t0) / 1000).toFixed(1);
  if (code) console.log(`stagecheck: ${ndiffer} of ${frames.length * SIZES.length} frames DIFFER (${s} s)`);
  else console.log(`stagecheck: all equal -- ${nframes} frames (${frames.length} at each of ${SIZES.length} sizes), ${nops} ops of R1 paired call for op within 1/64 px; R0 and R2 over ${nsplit} stand-in images, ${nimage} ops (${s} s)`);
} catch (x) {
  if (!(x instanceof CannotRun)) throw x;
  console.log(`stagecheck: cannot run: ${x.message}`);
  code = 2;
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
process.exit(code);
