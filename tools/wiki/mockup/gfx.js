/* gfx.js -- the stage in the game's own graphics (gfx=game): the ground and metal from map tiles, the vent and deposit
   features, the units' 3DO sprites, TA's cursors. Everything it draws is in gfx/, which tools/wiki/gfx.py renders from
   the local install and .gitignore keeps out of the repository; without gfx/index.json the stage stays on the whitebox.
   stage.js's draw() calls WikiGfx.draw(phase, ...), through the hook wiki.js gives it, at each element it can hand over,
   and draws its whitebox when the call returns false. What each element is in the game: research/notes/ingame-wiki.md, *In game: what each element is
   drawn with*. */
window.WikiGfx = (() => {
  const BASE = 'gfx/';
  // the stage's cursor shapes: TA's cursors.gaf sequences (cursor_ary, exe-reverse-engineering.md)
  const CURSOR = { arrow: 'arrow', move: 'move', build: 'arrow' };
  const TINT = { ok: 'rgb(40,210,70)', bad: 'rgb(230,40,40)' };
  let mode = new URLSearchParams(location.search).get('gfx') === 'game' ? 'game' : 'whitebox';
  let idx = null, state = 'none';       // none, loading, ready, missing
  const img = {};

  function load() {
    if (state !== 'none') return;
    state = 'loading'; note();
    fetch(BASE + 'index.json').then(r => { if (!r.ok) throw new Error(r.status); return r.json(); }).then(j => {
      const files = [];
      const add = (key, rec) => { if (rec && rec.file) files.push([key, rec.file]); };
      for (const [n, r] of Object.entries(j.units || {})) add('u:' + n, r);
      for (const k of ['ground', 'metalground', 'metal', 'vent']) add(k, j[k]);
      for (const [n, r] of Object.entries(j.cursors || {})) add('c:' + n, r);
      return Promise.all(files.map(([k, f]) => new Promise(ok => {
        const im = new Image(); im.onload = () => { img[k] = im; ok(); }; im.onerror = () => ok(); im.src = BASE + f;
      }))).then(() => { idx = j; state = 'ready'; note(); });
    }).catch(() => { state = 'missing'; note(); });
  }
  const on = () => mode === 'game' && state === 'ready';

  /* ---- the control in the mockup's strip, and its one-line notice when gfx/ is absent ---- */
  function note() {
    const el = document.querySelector('#gfx-note'); if (!el) return;
    el.textContent = mode === 'game' && state === 'missing' ? 'game graphics: run tools/wiki/gfx.py'
      : mode === 'game' && state === 'loading' ? 'loading…' : '';
  }
  function control() {
    const sel = document.querySelector('#gfx'); if (!sel) return;
    sel.value = mode;
    sel.onchange = e => { mode = e.target.value; if (mode === 'game') load(); note(); sync(); };
    note();
  }
  // wiki.js owns the URL; a change here re-runs its sync through a harmless replaceState of the same query
  function sync() {
    const q = new URLSearchParams(location.search);
    if (mode === 'game') q.set('gfx', 'game'); else q.delete('gfx');
    history.replaceState(null, '', '?' + q.toString().replace(/=(?=&|$)/g, ''));
  }
  function url(q) { if (mode === 'game') q.set('gfx', 'game'); }

  /* ---- drawing helpers: a sprite at ppu pixels a world unit, drawn at the stage's scale ---- */
  const k = c => c.s / idx.ppu;
  function sprite(g, c, im, rec, x, z, alpha = 1) {
    const f = k(c);
    g.globalAlpha = alpha;
    g.drawImage(im, c.X(x) - rec.anchor[0] * f, c.Z(z) - rec.anchor[1] * f, rec.w * f, rec.h * f);
    g.globalAlpha = 1;
  }
  const tinted = {};
  function tint(name, col) {
    const key = name + '|' + col;
    if (!tinted[key]) {
      const im = img['u:' + name], cv = document.createElement('canvas');
      cv.width = im.width; cv.height = im.height;
      const t = cv.getContext('2d');
      t.drawImage(im, 0, 0); t.globalCompositeOperation = 'source-atop'; t.globalAlpha = .55;
      t.fillStyle = col; t.fillRect(0, 0, cv.width, cv.height);
      tinted[key] = cv;
    }
    return tinted[key];
  }
  // the deposits: each 4-connected patch above surfaceMetal small enough to be a deposit gets the feature's sprite
  // stretched to the patch; broad metal (a metal map's ground, or SurfaceMetal itself above 0) gets the metal tile
  let metalKey = null, metalParts = null;
  function patches(M) {
    const key = M.w + 'x' + M.h + ':' + M.surfaceMetal + ':' + (M.metal || []).join();
    if (key === metalKey) return metalParts;
    const W = M.w, H = M.h, m = M.metal || [], sm = +M.surfaceMetal || 0, seen = new Uint8Array(W * H), out = [];
    for (let i = 0; i < W * H; i++) {
      if (seen[i] || !(+m[i] > sm)) continue;
      const cells = [], st = [i]; seen[i] = 1;
      while (st.length) {
        const c = st.pop(), x = c % W, z = (c / W) | 0; cells.push(c);
        for (const [dx, dz] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const nx = x + dx, nz = z + dz, n = nz * W + nx;
          if (nx >= 0 && nz >= 0 && nx < W && nz < H && !seen[n] && +m[n] > sm) { seen[n] = 1; st.push(n); }
        }
      }
      const xs = cells.map(c => c % W), zs = cells.map(c => (c / W) | 0);
      const x0 = Math.min(...xs), x1 = Math.max(...xs) + 1, z0 = Math.min(...zs), z1 = Math.max(...zs) + 1;
      out.push({ cells, x0, x1, z0, z1, deposit: x1 - x0 <= 6 && z1 - z0 <= 6 });
    }
    metalKey = key; metalParts = out;
    return out;
  }
  function tile(g, c, im, rec) {
    const p = g.createPattern(im, 'repeat'), f = c.s * rec.world / rec.w;
    p.setTransform(new DOMMatrix([f, 0, 0, f, c.X(0), c.Z(0)]));
    return p;
  }

  /* ---- the phases wiki.js hands over; true = drawn here ---- */
  const PH = {
    ground(g, c) {
      const M = c.v.map, cell = c.cell, cp = c.cp, sm = +M.surfaceMetal || 0, m = M.metal || [];
      g.fillStyle = tile(g, c, img.ground, idx.ground);
      g.fillRect(c.X(0), c.Z(0), M.w * cell * c.s, M.h * cell * c.s);
      const mg = img.metalground && tile(g, c, img.metalground, idx.metalground);
      const cellRect = i => [Math.floor(c.X((i % M.w) * cell)), Math.floor(c.Z(((i / M.w) | 0) * cell)), Math.ceil(cp) + 1, Math.ceil(cp) + 1];
      if (mg) {
        g.fillStyle = mg;
        for (let i = 0; i < M.w * M.h; i++) if (+m[i] > 0 && +m[i] <= sm) g.fillRect(...cellRect(i));
        for (const p of patches(M)) if (!p.deposit) for (const i of p.cells) g.fillRect(...cellRect(i));
      }
      if (img.metal) for (const p of patches(M)) if (p.deposit) {
        const r = idx.metal, fx = (p.x1 - p.x0) / r.footprint[0], fz = (p.z1 - p.z0) / r.footprint[1], f = k(c);
        const cx = c.X((p.x0 + p.x1) / 2 * cell), cz = c.Z((p.z0 + p.z1) / 2 * cell);
        g.drawImage(img.metal, cx - r.anchor[0] * f * fx, cz - r.anchor[1] * f * fz, r.w * f * fx, r.h * f * fz);
      }
      return true;
    },
    grid: () => true,                     // the game draws no cell grid
    label: () => true,                    // nor names
    vents(g, c) {
      if (!img.vent) return false;
      for (const vt of c.v.map.vents || []) { const p = Array.isArray(vt) ? { x: vt[0], z: vt[1] } : vt; sprite(g, c, img.vent, idx.vent, +p.x, +p.z); }
      return true;
    },
    // every unit's sprite, back to front (by world z, then x) as the engine's row sweep orders them; the per-unit
    // whitebox then stands down for each unit drawn here
    units(g, c) {
      const us = (c.v.units || []).filter(u => img['u:' + u.type]).sort((a, b) => a.z - b.z || a.x - b.x);
      for (const u of us) one(g, c, u, u.built == null ? 1 : Math.min(1, Math.max(0, +u.built)), Math.min(1, Math.max(0, +u.reclaimed || 0)));
      return true;
    },
    unit: () => true,
    // a gesture's site and the placement ghost: the building's sprite, translucent, as the Vulkan path draws a ghost
    site(g, c, st) {
      const im = img['u:' + st.type]; if (!im) return false;
      sprite(g, c, im, idx.units[st.type], st.x, st.z, .45); return true;
    },
    ghost(g, c, gh) {
      if (!img['u:' + gh.type]) return false;
      sprite(g, c, tint(gh.type, gh.ok !== false ? TINT.ok : TINT.bad), idx.units[gh.type], gh.x, gh.z, .6); return true;
    },
    // TA's cursor at the stage's magnification of the game's pixels, nearest, its hotspot on the point
    cursor(g, c, x, y, shape) {
      const name = CURSOR[shape] || 'arrow', im = img['c:' + name], r = idx.cursors[name];
      if (!im) return false;
      const f = Math.max(1, Math.round(c.U));
      g.imageSmoothingEnabled = false;
      g.drawImage(im, Math.round(x - r.hot[0] * f), Math.round(y - r.hot[1] * f), r.w * f, r.h * f);
      g.imageSmoothingEnabled = true;
      return true;
    },
  };
  // a unit: its sprite. Under construction the unbuilt share is a faint green stand-in for the engine's nano frame and
  // the built share fills from the bottom up in full colour; a reclaim empties it from the top, its lost share faint red
  function one(g, c, u, built, rec) {
    const im = img['u:' + u.type], r = idx.units[u.type];
    if (built >= 1 && rec <= 0) { sprite(g, c, im, r, u.x, u.z); return; }
    sprite(g, c, tint(u.type, built < 1 ? TINT.ok : TINT.bad), r, u.x, u.z, .35);
    const f = k(c), top = c.Z(u.z) - r.anchor[1] * f, h = r.h * f, keep = built < 1 ? built : 1 - rec;
    g.save(); g.beginPath(); g.rect(0, top + h * (1 - keep), c.W, h * keep + 1); g.clip();
    sprite(g, c, im, r, u.x, u.z);
    g.restore();
  }
  function draw(phase, g, c, ...a) {
    if (!on() || !PH[phase]) return false;
    try { return !!PH[phase](g, c, ...a); } catch (x) { console.error('wiki gfx:', x); mode = 'whitebox'; return false; }
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', control); else control();
  if (mode === 'game') load();
  return { draw, url, on };
})();
