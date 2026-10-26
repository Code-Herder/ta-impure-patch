/* stage.js -- the in-game wiki's stage: one view of sim.js drawn into a 2D canvas context, as the whitebox the game copies.
   tagpu/ddraw/src/tagpu_wikistage.c builds the same picture as the replay HUD's ops, and tools/wiki/stagecheck.mjs
   pairs this file's canvas calls with that file's ops, call for op, on every demo; a change to the stage is made here
   first and then there. research/notes/ingame-wiki.md, "The stage", says what each element shows.

   EVERYTHING DRAWN HERE HAS AN OP TO BE: a rectangle, a rectangle's stroke (an inset edge), a straight stroke with butt
   ends, a circle or a circle's stroke, a line of text in one HUD face, and a kit icon. So the text is set in the face of
   art/hud/layout.json's `toast` style, which the DLL already carries, and the three cursors are icons of the wiki's kit,
   which tools/wiki/cursors.sh rasterises once from cursorShape below, the shapes this stage drew before.

   Runs in a page (the global WikiStage, which wiki.js calls) and under node (module.exports), as sim.js does. */
const WikiStage = (() => {
  // stand-ins for TA's player colours, by owner
  const TEAM = ['#2348e0', '#d32b00', '#f4f4f4', '#2f8a1b', '#7a3fb0', '#e8d200', '#2a2a2a', '#e07018'];
  // TA's build square colours for a placement site: green placed, red refused (skipped), yellow takes a queued order off
  const SITE = { place: '#2bd248', refused: '#e62828', remove: '#f0d020' };
  const LINE = { move: '#3ccf3c', build: '#f0cc10', guard: '#3a86f0', reclaim: '#e23030' };
  // layout.json's `toast`: Oswald 700, no letter-spacing, no case change, no shadow -- the HUD's one plain line
  const FACE = { style: 'toast', family: 'Oswald', weight: 700 };
  // the kit's cursor icons by the view's cursor shape; each is drawn centred on the pointer
  const CURSOR = { arrow: 'cursor_arrow', move: 'cursor_move', build: 'cursor_build' };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  // a device coordinate rounded to the pixel an edge is drawn on, as the game snaps a part's box (tagpu_hud_snap)
  const snap = v => Math.floor(v + 0.5 + 1 / 64);
  // a view's fields, read defensively: sim.js owns the shape and the stage draws whatever it can find
  const pt = p => Array.isArray(p) ? { x: +p[0], z: +p[1] } : p ? { x: +p.x, z: +p.z } : null;

  /* THE FIT: the map in a W x H canvas at U pixels a unit, letterboxed with a pad of 6 units, its scale s pixels a world
     unit and its corner at (ox, oz). tagpu_wikistage_fit is this, and both of the game's threads call it, so a click on
     the stage and the picture of it cannot disagree. */
  function fit(mw, mh, cell, W, H, U) {
    const pad = 6 * U;
    const s = Math.min((W - 2 * pad) / (mw * cell), (H - 2 * pad) / (mh * cell));
    return { s, ox: (W - mw * cell * s) / 2, oz: (H - mh * cell * s) / 2 };
  }

  /* The view `v` into the context `g` of a W x H canvas, at U pixels a unit; o = { W, H, U, msg, icons, gx }. `msg` is
     what an absent view shows; `icons` the kit's cursor icons by name, each { img, pw, ph, w, h } with w and h in units
     (loadIcons); `gx(phase, c, ...a)` the game graphics' hook (gfx.js), true where it drew an element. Returns the fit,
     with the map's cell and size, or null without a map. */
  function draw(g, v, o) {
    const W = o.W, H = o.H, U = o.U, gx = o.gx || (() => false);
    const font = sz => `${FACE.weight} ${sz}px ${FACE.family}`;
    const measure = (t, sz) => { g.font = font(sz); return g.measureText(t).width; };
    /* a line of text centred on y: the canvas's middle baseline, the HUD's line box centred on a point */
    const text = (t, x, y, sz, align, col) => {
      g.font = font(sz); g.textAlign = align; g.textBaseline = 'middle'; g.fillStyle = col;
      g.fillText(t, x, y);
    };
    g.setTransform(1, 0, 0, 1, 0, 0); g.fillStyle = '#6e6e68'; g.fillRect(0, 0, W, H);
    if (!v || !v.map) {
      if (o.msg) text(o.msg, W / 2, H / 2, Math.round(11 * U), 'center', '#e8e8e2');
      return null;
    }
    const M = v.map, cell = +M.cell || 16, mw = +M.w || 1, mh = +M.h || 1;
    const { s, ox, oz } = fit(mw, mh, cell, W, H, U);
    const X = x => ox + x * s, Z = z => oz + z * s, cp = cell * s;
    const c = { v, X, Z, s, cp, cell, U, W, H };
    const hook = (phase, ...a) => !!gx(phase, c, ...a);

    // the ground: plain cells light, metal darker by how far it stands above surfaceMetal; metal ground at surfaceMetal is
    // tinted once, so a metal map reads as metal everywhere and its richer patch as darker still
    const metal = M.metal || [], sm = +M.surfaceMetal || 0, top = Math.max(sm, ...metal.filter(Number.isFinite));
    if (!hook('ground')) for (let z = 0; z < mh; z++) for (let x = 0; x < mw; x++) {
      const m = +metal[z * mw + x] || 0;
      let L = m > 0 ? 188 : 204;
      if (m > sm && top > sm) L = 176 - 108 * (m - sm) / (top - sm);
      g.fillStyle = `rgb(${L},${L},${L - 4})`;
      g.fillRect(Math.floor(X(x * cell)), Math.floor(Z(z * cell)), Math.ceil(cp) + 1, Math.ceil(cp) + 1);
    }
    // the cell grid, one pixel wide on pixel centres. A row is broken at each column, so no pixel is stroked twice: the
    // pieces are the one path's pixels, and the game draws each piece as its own stroke
    if (cp >= 5 && !hook('grid')) {
      g.strokeStyle = 'rgba(0,0,0,.12)'; g.lineWidth = 1; g.beginPath();
      const cols = [];
      for (let x = 0; x <= mw; x++) cols.push(Math.round(X(x * cell)));
      for (const px of cols) { g.moveTo(px + .5, Z(0)); g.lineTo(px + .5, Z(mh * cell)); }
      for (let z = 0; z <= mh; z++) {
        const pz = Math.round(Z(z * cell)) + .5, end = X(mw * cell);
        let a = X(0);
        for (const px of cols) { if (px > a) { g.moveTo(a, pz); g.lineTo(px, pz); } a = px + 1; }
        if (end > a) { g.moveTo(a, pz); g.lineTo(end, pz); }
      }
      g.stroke();
    }
    // a geothermal vent: a ringed circle with a cross
    if (!hook('vents')) for (const vt of M.vents || []) {
      const p = pt(vt); if (!p) continue;
      const r = cp * .8; g.lineWidth = Math.max(1, 1.5 * U);
      g.fillStyle = '#9a9a94'; g.strokeStyle = '#2a2a28';
      g.beginPath(); g.arc(X(p.x), Z(p.z), r, 0, 2 * Math.PI); g.fill(); g.stroke();
      g.beginPath(); g.arc(X(p.x), Z(p.z), r * .5, 0, 2 * Math.PI); g.stroke();
      g.beginPath(); g.moveTo(X(p.x) - r, Z(p.z)); g.lineTo(X(p.x) + r, Z(p.z)); g.moveTo(X(p.x), Z(p.z) - r); g.lineTo(X(p.x), Z(p.z) + r); g.stroke();
    }
    // fog: grey half over seen-before ground, black over unexplored
    const fog = M.fog || [];
    for (let z = 0; z < mh; z++) for (let x = 0; x < mw; x++) {
      const f = fog[z * mw + x]; if (!f) continue;
      g.fillStyle = f === 2 || f === 'black' ? 'rgba(0,0,0,.9)' : 'rgba(128,128,128,.5)';
      g.fillRect(Math.floor(X(x * cell)), Math.floor(Z(z * cell)), Math.ceil(cp) + 1, Math.ceil(cp) + 1);
    }

    const box = (q, f = 1) => { const p = pt(q), fx = +(q.fx ?? (q.footprint || [])[0]) || 1, fz = +(q.fz ?? (q.footprint || [])[1]) || 1;
      const w = fx * cp * f, h = fz * cp * f; return { x: X(p.x) - w / 2, y: Z(p.z) - h / 2, w, h, cx: X(p.x), cy: Z(p.z) }; };
    // a name on its box. One longer than 1.5 times its box shrinks until it fits, as the HUD fits its text
    const label = (t, b, col) => {
      t = String(t);
      let sz = Math.max(6 * U, Math.min(9 * U, b.h * .3));
      const w = measure(t, sz), max = b.w * 1.5;
      if (w > max) sz = sz * max / w;
      text(t, b.cx, b.cy, sz, 'center', col);
    };
    // order lines, dashed, coloured by kind
    g.setLineDash([5 * U, 3.5 * U]); g.lineWidth = Math.max(1, 1.4 * U);
    for (const l of v.lines || []) {
      const a = pt(l.from), b = pt(l.to); if (!a || !b) continue;
      g.strokeStyle = LINE[l.kind] || '#d8d8d8';
      g.beginPath(); g.moveTo(X(a.x), Z(a.z)); g.lineTo(X(b.x), Z(b.z)); g.stroke();
      g.fillStyle = g.strokeStyle; g.beginPath(); g.arc(X(b.x), Z(b.z), 2 * U, 0, 2 * Math.PI); g.fill();
    }
    g.setLineDash([]);
    // queued build sites: an outlined square in the owner's colour
    for (const q of v.queued || []) {
      const b = box(q); g.lineWidth = Math.max(1, 1.5 * U); g.strokeStyle = TEAM[q.owner | 0] || '#ccc';
      g.setLineDash([3 * U, 2 * U]); g.strokeRect(b.x, b.y, b.w, b.h); g.setLineDash([]);
      if (!hook('label')) label(q.type || '', b, 'rgba(20,20,20,.75)');
    }
    // units: grey boxes at their footprint, a team-coloured edge; construction fills up from the bottom, a reclaim empties
    hook('units');                        // the game's sprites, back to front, under every selection box
    for (const u of v.units || []) {
      const b = box(u), built = u.built == null ? 1 : clamp(+u.built, 0, 1), rec = clamp(+u.reclaimed || 0, 0, 1);
      const bw = Math.max(1.5, 2 * U);
      if (!hook('unit')) {
        if (built < 1) {
          g.fillStyle = 'rgba(150,150,144,.35)'; g.fillRect(b.x, b.y, b.w, b.h);
          g.fillStyle = '#8a8a84'; g.fillRect(b.x, b.y + b.h * (1 - built), b.w, b.h * built);
        } else if (rec > 0) {
          g.fillStyle = 'rgba(150,150,144,.35)'; g.fillRect(b.x, b.y, b.w, b.h);
          g.fillStyle = '#9a7c78'; g.fillRect(b.x, b.y + b.h * rec, b.w, b.h * (1 - rec));
        } else { g.fillStyle = '#8a8a84'; g.fillRect(b.x, b.y, b.w, b.h); }
        g.lineWidth = bw; g.strokeStyle = TEAM[u.owner | 0] || '#ccc'; g.strokeRect(b.x + bw / 2, b.y + bw / 2, b.w - bw, b.h - bw);
        label(u.label || u.type || '', b, '#141414');
      }
      if (u.selected) {
        // the selection box: corner brackets just outside the footprint
        const m = 3 * U, x0 = b.x - m, y0 = b.y - m, x1 = b.x + b.w + m, y1 = b.y + b.h + m, k = Math.min(b.w, b.h) * .35 + m;
        g.strokeStyle = '#f2f6e8'; g.lineWidth = Math.max(1, 1.5 * U); g.beginPath();
        for (const [x, y, dx, dy] of [[x0, y0, 1, 1], [x1, y0, -1, 1], [x0, y1, 1, -1], [x1, y1, -1, -1]]) {
          g.moveTo(x + dx * k, y); g.lineTo(x, y); g.lineTo(x, y + dy * k);
        }
        g.stroke();
      }
    }
    // a placement gesture's sites: a translucent ghost of the building, TA's build square in the site's colour, and its
    // place among the orders the release adds, small in the corner
    for (const st of v.sites || []) {
      if (!pt(st)) continue;
      const b = box(st), col = SITE[st.state] || SITE.place, lw = Math.max(1, 1.5 * U);
      if (!hook('site', st)) { g.fillStyle = 'rgba(138,138,132,.45)'; g.fillRect(b.x + lw, b.y + lw, b.w - 2 * lw, b.h - 2 * lw); }
      g.strokeStyle = col; g.lineWidth = lw; g.strokeRect(b.x + lw / 2, b.y + lw / 2, b.w - lw, b.h - lw);
      if (st.order != null) {
        const sz = Math.max(5 * U, Math.min(7.5 * U, b.h * .32));
        text(String(st.order), b.x + 2.5 * lw, b.y + 2 * lw + sz / 2, sz, 'left', '#101010');
      }
    }
    // the placement ghost: translucent green where it may go, red where it may not
    if (v.ghost && pt(v.ghost)) {
      const b = box(v.ghost), ok = v.ghost.ok !== false;
      if (!hook('ghost', v.ghost)) { g.fillStyle = ok ? 'rgba(40,210,70,.35)' : 'rgba(230,40,40,.35)'; g.fillRect(b.x, b.y, b.w, b.h); }
      g.strokeStyle = ok ? '#2bd248' : '#e62828'; g.lineWidth = Math.max(1, 1.5 * U); g.strokeRect(b.x, b.y, b.w, b.h);
      if (!hook('label')) label(v.ghost.type || '', b, ok ? '#0c3a12' : '#4a0a0a');
    }
    if (v.drag) {
      const d = v.drag, x0 = X(Math.min(d.x0, d.x1)), y0 = Z(Math.min(d.z0, d.z1));
      g.strokeStyle = '#f2f6e8'; g.lineWidth = Math.max(1, U); g.strokeRect(x0, y0, Math.abs(d.x1 - d.x0) * s, Math.abs(d.z1 - d.z0) * s);
    }
    // the cursor at screen size, whatever the map's scale: its kit icon centred on the point, in a box snapped as the
    // game snaps a part's, and the icon as large as fits in it, centred (tagpu_hudlay_expand's icon)
    const cur = pt(v.cursor), ic = cur && o.icons && o.icons[CURSOR[v.cursor.shape] || CURSOR.arrow];
    if (cur && !hook('cursor', X(cur.x), Z(cur.z), v.cursor.shape) && ic && ic.img) {
      const w = ic.w * U, h = ic.h * U, a = X(cur.x) - w / 2, b = Z(cur.z) - h / 2;
      const x0 = snap(a), y0 = snap(b), bw = snap(a + w) - x0, bh = snap(b + h) - y0;
      const f = Math.min(bw / ic.pw, bh / ic.ph), iw = ic.pw * f, ih = ic.ph * f;
      const q = g.imageSmoothingQuality; g.imageSmoothingQuality = 'high';
      g.drawImage(ic.img, x0 + (bw - iw) / 2, y0 + (bh - ih) / 2, iw, ih);
      g.imageSmoothingQuality = q;
    }
    // a readout on a dark plate at the stage's bottom, its text centred in the plate's height
    const readout = (t, right) => {
      const sz = Math.round(8.5 * U), h = 12 * U, tw = measure(t, sz);
      g.fillStyle = 'rgba(30,30,28,.7)';
      g.fillRect(right ? W - tw - 8 * U : 2 * U, H - h - 2 * U, tw + 6 * U, h);
      text(t, right ? W - 5 * U : 5 * U, H - 2 * U - h / 2, sz, right ? 'right' : 'left', '#f0f0ea');
    };
    // construction runs faster than the game's (the entry's pace), and the stage says so
    if (+v.pace > 1) readout(`construction ×${+v.pace}`, true);
    // the armed building's spacing: the mockup's readout, at the stage's bottom left; the game specifies none yet
    if (+v.spacing > 0) readout(`Spacing: ${+v.spacing}`, false);
    return { s, ox, oz, cell, mw, mh };
  }

  /* THE CURSOR SHAPES the icons are rasterised from (tools/wiki/cursors.sh), at U pixels a unit with the pointer at
     (x, y): an arrow, the move arrow with its green fill, or a build crosshair. */
  function cursorShape(g, x, y, shape, U) {
    const k = 1.15 * U;
    g.lineWidth = Math.max(1, U); g.strokeStyle = '#000';
    if (shape === 'build') {
      g.strokeStyle = '#f0f0e8'; g.lineWidth = Math.max(1, 1.5 * U);
      g.beginPath(); g.arc(x, y, 6 * k, 0, 2 * Math.PI);
      g.moveTo(x - 10 * k, y); g.lineTo(x - 3 * k, y); g.moveTo(x + 3 * k, y); g.lineTo(x + 10 * k, y);
      g.moveTo(x, y - 10 * k); g.lineTo(x, y - 3 * k); g.moveTo(x, y + 3 * k); g.lineTo(x, y + 10 * k);
      g.stroke(); return;
    }
    g.fillStyle = shape === 'move' ? '#3ccf3c' : '#f0f0e8';
    g.beginPath(); g.moveTo(x, y); g.lineTo(x, y + 15 * k); g.lineTo(x + 4 * k, y + 11.5 * k); g.lineTo(x + 7 * k, y + 17.5 * k);
    g.lineTo(x + 9.5 * k, y + 16.5 * k); g.lineTo(x + 6.5 * k, y + 10.5 * k); g.lineTo(x + 11 * k, y + 10.5 * k); g.closePath();
    g.fill(); g.stroke();
  }
  /* The icons' boxes in units, the pointer at the middle: wide and tall enough for each shape's stroke and miters (the
     arrow's sharpest corner reaches 1.4 units past its point), so a centred box needs no hotspot of its own */
  const ICON_BOX = { arrow: [30, 44], move: [30, 44], build: [26, 26] };

  /* The kit's cursor icons from `base` (a kit directory: kit.json and parts/), for a page: the object fills in as they
     load, and draw() leaves the cursor out until its icon is there. */
  function loadIcons(base) {
    const icons = {};
    fetch(base + 'kit.json').then(r => r.json()).then(j => {
      for (const name of Object.values(CURSOR)) {
        const k = j.parts[name]; if (!k) continue;
        const img = new Image();
        img.onload = () => { icons[name] = { img, pw: img.naturalWidth, ph: img.naturalHeight, w: img.naturalWidth / k.scale, h: img.naturalHeight / k.scale }; };
        img.src = `${base}parts/${name}.png`;
      }
    }).catch(x => console.error('wiki stage icons:', x));
    return icons;
  }

  return { draw, fit, cursorShape, loadIcons, FACE, CURSOR, ICON_BOX };
})();
if (typeof module !== 'undefined') module.exports = WikiStage;
