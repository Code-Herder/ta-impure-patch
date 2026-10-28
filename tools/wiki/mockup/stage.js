/* stage.js -- the in-game wiki's stage: one view of sim.js drawn into a 2D canvas context, as the whitebox the game copies.
   tagpu/ddraw/src/tagpu_wikistage.c builds the same picture as the replay HUD's ops, and tools/wiki/stagecheck.mjs
   pairs this file's canvas calls with that file's ops, call for op, on every demo; a change to the stage is made here
   first and then there. research/notes/ingame-wiki.md, "The stage", says what each element shows.

   EVERYTHING DRAWN HERE HAS AN OP TO BE: a rectangle, a rectangle's stroke (an inset edge), a straight stroke with butt
   ends, a circle or a circle's stroke, a line of text in one HUD face, a kit icon, and a frame of the game's own
   cursors. So the text is set in the face of art/hud/layout.json's `toast` style, which the DLL already carries, and
   the cursor is the game's sprite, from the player's own archives (loadCursors; the DLL's asset load), or where that
   did not resolve an icon of the wiki's kit, which tools/wiki/cursors.sh rasterises once from cursorShape below. Under
   gfx=game one thing more is drawn, the stage's 3D image (gfx.js), which the game composites from its own world
   target between two op ranges (draw() below says which).

   Runs in a page (the global WikiStage, which wiki.js calls) and under node (module.exports), as sim.js does. */
const WikiStage = (() => {
  // stand-ins for TA's player colours, by owner
  const TEAM = ['#2348e0', '#d32b00', '#f4f4f4', '#2f8a1b', '#7a3fb0', '#e8d200', '#2a2a2a', '#e07018'];
  // TA's build square colours for a placement site: green placed, red refused (skipped), yellow takes a queued order off
  const SITE = { place: '#2bd248', refused: '#e62828', remove: '#f0d020' };
  const LINE = { move: '#3ccf3c', build: '#f0cc10', guard: '#3a86f0', reclaim: '#e23030' };
  // layout.json's `toast`: Oswald 700, no letter-spacing, no case change, no shadow -- the HUD's one plain line
  const FACE = { style: 'toast', family: 'Oswald', weight: 700 };
  // the kit's cursor icons by the view's cursor shape, each drawn centred on the pointer where the game's did not load
  const CURSOR = { arrow: 'cursor_arrow', move: 'cursor_move', build: 'cursor_build' };
  const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
  // a device coordinate rounded to the pixel an edge is drawn on, as the game snaps a part's box (tagpu_hud_snap)
  const snap = v => Math.floor(v + 0.5 + 1 / 64);
  // a view's fields, read defensively: sim.js owns the shape and the stage draws whatever it can find
  const pt = p => Array.isArray(p) ? { x: +p[0], z: +p[1] } : p ? { x: +p.x, z: +p.z } : null;

  /* THE GROUND'S SHADES of a view's map, by cell index: plain cells light, metal darker by how far it stands above
     surfaceMetal; metal ground at surfaceMetal is tinted once, so a metal map reads as metal everywhere and its richer
     patch as darker still. The map's cells and the whitebox minimap's take the same shades. */
  function grounds(M) {
    const metal = M.metal || [], sm = +M.surfaceMetal || 0, top = Math.max(sm, ...metal.filter(Number.isFinite));
    return i => {
      const m = +metal[i] || 0;
      let L = m > 0 ? 188 : 204;
      if (m > sm && top > sm) L = 176 - 108 * (m - sm) / (top - sm);
      return `rgb(${L},${L},${L - 4})`;
    };
  }
  // fog over ground, the map's and the minimap's: grey half over seen-before ground, black over unexplored
  const fogColour = f => f === 2 || f === 'black' ? 'rgba(0,0,0,.9)' : 'rgba(128,128,128,.5)';

  /* ---- THE GAME'S LEFT SIDEBAR (research/notes/ingame-wiki.md, *The left sidebar on the stage*) ----
     The column is x in [-128, 0) of the stage at one game pixel a world unit, whatever the HUD scale (decision 8), and
     v.side.h rows tall. In the game look it is one image (side.js composes it from the mod's pack); otherwise it is
     this whitebox, every element an op: the column, the minimap -- its well, the ground's shades and the fog one
     solid a map cell at the minimap's scale, a dot a unit in its owner's colour, the camera box -- then the routed
     screen's panel and each drawn gadget as a button: a solid, an edge one game pixel deep and its name. A pressed
     button (or the current tab) takes the pressed colour and its name moves one game pixel right and down, as the
     engine moves a pressed button's text (0x4A5F40); a greyed one is drawn whole at opacity 0.45, as one group.
     tagpu_wikistage.c mirrors SIDE_WB. */
  const SIDE_W = 128, MINIMAP = 128;
  const SIDE_WB = { column: '#2e2e2a', panel: '#3a3a36', well: '#1e1e1c', button: '#55554f', edge: '#8a8a80',
    pressed: '#6e6e66', grey: 0.45, label: '#e8e8e2', box: '#f0cc10' };
  const SIDE = typeof WikiSide !== 'undefined' ? WikiSide : require('./side.js');

  /* A GROUP AT ONE OPACITY, the HUD's ops' `grp` (tagpu_hudlay.h, THE GROUPS): fn's calls composited at full opacity,
     then the whole faded once over what was under it. A check's recording context marks it with beginGroup and
     endGroup. A canvas keeps a copy of r's pixels, which must hold everything fn draws, lets fn draw at full opacity,
     and lays the copy back over at 1 - alpha: under (1 - alpha) + drawn alpha, the group's own blend. */
  let groupCanvas = null;
  function group(g, alpha, r, fn) {
    if (typeof g.beginGroup === 'function') { g.beginGroup(alpha); fn(); g.endGroup(); return; }
    const W = g.canvas.width, H = g.canvas.height;
    const x0 = clamp(Math.floor(r.x), 0, W), y0 = clamp(Math.floor(r.y), 0, H);
    const w = clamp(Math.ceil(r.x + r.w), 0, W) - x0, h = clamp(Math.ceil(r.y + r.h), 0, H) - y0;
    if (w <= 0 || h <= 0) return;
    if (!groupCanvas) groupCanvas = document.createElement('canvas');
    if (groupCanvas.width < w || groupCanvas.height < h) { groupCanvas.width = Math.max(w, groupCanvas.width); groupCanvas.height = Math.max(h, groupCanvas.height); }
    const c = groupCanvas.getContext('2d');
    c.globalCompositeOperation = 'copy'; c.drawImage(g.canvas, x0, y0, w, h, 0, 0, w, h);
    fn();
    const a = g.globalAlpha; g.globalAlpha = 1 - alpha;
    g.drawImage(groupCanvas, 0, 0, w, h, x0, y0, w, h);
    g.globalAlpha = a;
  }

  /* The column of view v (v.side: { h, screen, unit, page, state, under }) with `side` = { layout, art }: the layout
     the sim ran on (sim.js, checkLayout), and art = { draw(g, rect), label(i), minimap } when the column is the
     game's image -- drawn whole into the column's rect snapped to device pixels, then the whitebox's minimap when
     the image has none (no base picture for this entry) and a whitebox button over every gadget whose picture is a
     label, label(i) giving that label's text ('' for the gadget's own name) or null for a picture -- or
     null for the whitebox. k carries draw()'s X, Z, s, label and the ground's shades. */
  function drawSide(g, v, side, k) {
    const { X, Z, s, U, label, shade } = k, sv = v.side, L = side.layout || null, art = side.art || null, H = +sv.h || 0;
    const CX = x => X(x - SIDE_W);
    const box = (x, y, w, h) => ({ x: CX(x), y: Z(y), w: w * s, h: h * s, cx: CX(x + w / 2), cy: Z(y + h / 2) });
    const ring = (b, col) => { g.lineWidth = s; g.strokeStyle = col; g.strokeRect(b.x + s / 2, b.y + s / 2, b.w - s, b.h - s); };
    const solid = (b, col) => { g.fillStyle = col; g.fillRect(b.x, b.y, b.w, b.h); };
    if (art) {
      const x0 = snap(X(-SIDE_W)), y0 = snap(Z(0));
      art.draw(g, { x: x0, y: y0, w: snap(X(0)) - x0, h: snap(Z(H)) - y0 });
    } else solid(box(0, 0, SIDE_W, H), SIDE_WB.column);

    if (!art || !art.minimap) {
      // the minimap's picture where the engine puts it (0x466780) over the stage map's extent, as the game look has it
      const M = v.map, cell = +M.cell || 16, mw = +M.w || 1, mh = +M.h || 1, m = SIDE.geometry(mw * cell, mh * cell);
      solid(box(0, 0, MINIMAP, MINIMAP), SIDE_WB.well);
      // one solid a map cell, its edges on the device pixels the picture's own fractions round to
      const cellAt = (cx, cz, col) => {
        const a = snap(CX(m.offx + cx * m.w / mw)), b = snap(CX(m.offx + (cx + 1) * m.w / mw));
        const c = snap(Z(m.offy + cz * m.h / mh)), d = snap(Z(m.offy + (cz + 1) * m.h / mh));
        if (b > a && d > c) { g.fillStyle = col; g.fillRect(a, c, b - a, d - c); }
      };
      for (let z = 0; z < mh; z++) for (let x = 0; x < mw; x++) cellAt(x, z, shade(z * mw + x));
      const fog = M.fog || [];
      for (let z = 0; z < mh; z++) for (let x = 0; x < mw; x++) { const f = fog[z * mw + x]; if (f) cellAt(x, z, fogColour(f)); }
      // a dot 3 game pixels across on the unit's minimap pixel (0x466DC0's point), in its owner's colour
      for (const u of v.units || []) {
        const p = pt(u); if (!p) continue;
        const [px, py] = SIDE.dotAt(p.x, p.z, m, mw * cell, mh * cell);
        g.fillStyle = TEAM[u.owner | 0] || '#ccc';
        g.beginPath(); g.arc(CX(m.offx + px + .5), Z(m.offy + py + .5), 1.5 * s, 0, 2 * Math.PI); g.fill();
      }
      // the camera box: the stage shows the whole map, so it is the picture's border
      ring(box(m.offx, m.offy, m.w, m.h), SIDE_WB.box);
    }

    const scr = L && L.screens[sv.screen];
    if (!scr) return;
    if (!art) solid(box(0, scr.y, scr.w, scr.h), SIDE_WB.panel);
    // a button's name: its unit, or its gadget name without the view's side's prefix (ARMORDERS -> ORDERS)
    const prefix = (L.sides[sv.index] || {}).prefix || '';
    const named = gd => gd.role === 'build' && gd.unit ? gd.unit
      : prefix && gd.name.length > prefix.length && gd.name.startsWith(prefix) ? gd.name.slice(prefix.length) : gd.name;
    scr.gadgets.forEach((gd, i) => {
      const st = sv.state[i];
      const lt = art ? art.label(i) : '';
      if (!(st >= 0) || lt === null) return;
      const [gx, gy, gw, gh] = gd.rect, b = box(gx, scr.y + gy, gw, gh);
      const button = () => {
        solid(b, st === 1 ? SIDE_WB.pressed : SIDE_WB.button);
        ring(b, SIDE_WB.edge);
        // inside the button, two game pixels clear of each edge: neighbouring buttons touch
        label(lt || named(gd), st === 1 ? { ...b, cx: b.cx + s, cy: b.cy + s } : b, SIDE_WB.label, Math.max(s, b.w - 4 * s));
      };
      // the label fits the button's width, but its line may stand 9 units tall in a shorter box
      if (st === 2) group(g, SIDE_WB.grey, { x: b.x - 2, y: b.y - 9 * U - 2, w: b.w + 4, h: b.h + 18 * U + 4 }, button);
      else button();
    });
  }

  /* THE FIT: the stage's content rect c = { x0, w, h } in world units -- the map, and with the game's sidebar the
     128-unit column left of it (ingame-wiki.md, *The left sidebar on the stage*) -- in a W x H canvas at U pixels a
     unit, letterboxed with a pad of 6 units: s pixels a world unit and the world origin at (ox, oz), so X(x) = ox + x s
     holds for the map and the column alike. Without the column (x0 = 0) this is the map's own fit, to the bit.
     tagpu_wikistage_fit is this, and both of the game's threads call it, so a click on the stage and the picture of it
     cannot disagree. */
  function fit(c, W, H, U) {
    const pad = 6 * U;
    const s = Math.min((W - 2 * pad) / c.w, (H - 2 * pad) / c.h);
    return { s, ox: (W - c.w * s) / 2 - c.x0 * s, oz: (H - c.h * s) / 2 };
  }
  /* The content rect of a view: the map's, or with the column (v.side) the column and the map, as tall as the taller.
     The map stays at the top; the band under a map shorter than the column is only R0's background (decision 9). */
  function content(v) {
    const M = v.map, cell = +M.cell || 16, mw = (+M.w || 1) * cell, mh = (+M.h || 1) * cell;
    return v.side ? { x0: -SIDE_W, w: SIDE_W + mw, h: Math.max(mh, +v.side.h || 0) } : { x0: 0, w: mw, h: mh };
  }

  /* The view `v` into the context `g` of a W x H canvas, at U pixels a unit; o = { W, H, U, msg, icons, cursors, tick,
     cursor, image, layer, side }. `msg` is what an absent view shows; `icons` the kit's cursor icons by name, each { img, pw,
     ph, w, h } with w and h in units (loadIcons); `cursors` the game's (loadCursors), and `tick` the simulation's, which
     its animation runs from; `cursor` false leaves the cursor out (Try it yourself while the player's pointer is off
     the stage: the player's own is the one on screen); `side` = { layout, art } for a view with the sidebar (drawSide).
     Returns the fit, with the map's cell and size, the content rect (x0, cw, ch) and `cursor`, true when a cursor was
     drawn, or null without a map.

     THE THREE OP RANGES (research/notes/ingame-wiki-3d-stage-plan.md, section 3). Everything drawn here belongs to one:
     - R0, under the image: the stage's background, which also fills the band under a map shorter than the sidebar
       (and the message of an absent view);
     - side, the game's left sidebar (drawSide), whole in every look: it never overlaps the map's rect, so it is
       drawn under the image;
     - R1, the whole whitebox stage: the ground (cells, grid, vents, fog), then every element below;
     - R2, the overlays drawn over the image: order lines, queued squares, selection brackets, the sites' and the
       ghost's squares and the sites' order numbers, the drag box, the cursor, the readouts, and the whitebox box and
       label of any unit, site or ghost whose model did not resolve.
     Without `o.image` this draws R0 and R1: the whitebox. With it -- `{ draw(g, rect), ground, resolved(type) }`, a
     COMPLETE 3D image of this frame (gfx.js) -- it draws R0, the image into the map's rect, and R2. When the image
     carries no ground (the stage's ground fell back whole) R1's ground is drawn under it, and fog is the whitebox's.
     The choice is the caller's, per frame, so no frame shows half of one and half of the other. R2 is R1 with the
     ground and the resolved boxes left out, in R1's order: `o.layer(range, type)` names the range of every call that
     follows -- 'r0', 'side', 'ground', 'box' (with its unit type), 'label' (R1 only) or 'over' -- which is how
     tools/wiki/stagecheck.mjs pairs R0 and R2 with the DLL's ops as well as R1. */
  function draw(g, v, o) {
    const W = o.W, H = o.H, U = o.U, im = o.image || null;
    const mark = o.layer || (() => {});
    // whether a range is drawn this frame: everything on the whitebox; R0, the overlays, the boxes of what did not
    // resolve and, under an image without its ground, the ground, over the image
    const shows = (range, type) => !im || range === 'r0' || range === 'over' ||
      (range === 'ground' && !im.ground) || (range === 'box' && !im.resolved(type));
    const at = (range, type) => { if (!shows(range, type)) return false; mark(range, type); return true; };
    const font = sz => `${FACE.weight} ${sz}px ${FACE.family}`;
    const measure = (t, sz) => { g.font = font(sz); return g.measureText(t).width; };
    /* a line of text centred on y: the canvas's middle baseline, the HUD's line box centred on a point */
    const text = (t, x, y, sz, align, col) => {
      g.font = font(sz); g.textAlign = align; g.textBaseline = 'middle'; g.fillStyle = col;
      g.fillText(t, x, y);
    };
    mark('r0');
    g.setTransform(1, 0, 0, 1, 0, 0); g.fillStyle = '#6e6e68'; g.fillRect(0, 0, W, H);
    if (!v || !v.map) {
      if (o.msg) text(o.msg, W / 2, H / 2, Math.round(11 * U), 'center', '#e8e8e2');
      return null;
    }
    const M = v.map, cell = +M.cell || 16, mw = +M.w || 1, mh = +M.h || 1, C = content(v);
    const { s, ox, oz } = fit(C, W, H, U);
    const X = x => ox + x * s, Z = z => oz + z * s, cp = cell * s, shade = grounds(M);
    // a name on its box. One longer than `max`, 1.5 times its box unless given, shrinks until it fits, as the HUD fits
    // its text
    const label = (t, b, col, max = b.w * 1.5) => {
      t = String(t);
      let sz = Math.max(6 * U, Math.min(9 * U, b.h * .3));
      const w = measure(t, sz);
      if (w > max) sz = sz * max / w;
      text(t, b.cx, b.cy, sz, 'center', col);
    };
    // the game's left sidebar, before the map: the cursor and the overlays are drawn over it
    if (v.side) { mark('side'); drawSide(g, v, o.side || {}, { X, Z, s, U, label, shade }); }

    if (at('ground')) {
      // the ground, in the shades of grounds()
      for (let z = 0; z < mh; z++) for (let x = 0; x < mw; x++) {
        g.fillStyle = shade(z * mw + x);
        g.fillRect(Math.floor(X(x * cell)), Math.floor(Z(z * cell)), Math.ceil(cp) + 1, Math.ceil(cp) + 1);
      }
      // the cell grid, one pixel wide on pixel centres. A row is broken at each column, so no pixel is stroked
      // twice: the pieces are the one path's pixels, and the game draws each piece as its own stroke
      if (cp >= 5) {
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
      for (const vt of M.vents || []) {
        const p = pt(vt); if (!p) continue;
        const r = cp * .8; g.lineWidth = Math.max(1, 1.5 * U);
        g.fillStyle = '#9a9a94'; g.strokeStyle = '#2a2a28';
        g.beginPath(); g.arc(X(p.x), Z(p.z), r, 0, 2 * Math.PI); g.fill(); g.stroke();
        g.beginPath(); g.arc(X(p.x), Z(p.z), r * .5, 0, 2 * Math.PI); g.stroke();
        g.beginPath(); g.moveTo(X(p.x) - r, Z(p.z)); g.lineTo(X(p.x) + r, Z(p.z)); g.moveTo(X(p.x), Z(p.z) - r); g.lineTo(X(p.x), Z(p.z) + r); g.stroke();
      }
      // fog, in fogColour's
      const fog = M.fog || [];
      for (let z = 0; z < mh; z++) for (let x = 0; x < mw; x++) {
        const f = fog[z * mw + x]; if (!f) continue;
        g.fillStyle = fogColour(f);
        g.fillRect(Math.floor(X(x * cell)), Math.floor(Z(z * cell)), Math.ceil(cp) + 1, Math.ceil(cp) + 1);
      }
    }
    // the 3D image: the map at zoom 1, composited into the map's rect as the game's world target is (bilinear), each
    // edge snapped to the device pixel as the game's composite rect is (tagpu_wikistage_image): the composite's
    // viewport is whole pixels, so the image may sit up to half a pixel from the fit, the same way in both
    if (im) {
      const x0 = snap(X(0)), y0 = snap(Z(0));
      im.draw(g, { x: x0, y: y0, w: snap(X(mw * cell)) - x0, h: snap(Z(mh * cell)) - y0 });
    }

    const box = (q, f = 1) => { const p = pt(q), fx = +(q.fx ?? (q.footprint || [])[0]) || 1, fz = +(q.fz ?? (q.footprint || [])[1]) || 1;
      const w = fx * cp * f, h = fz * cp * f; return { x: X(p.x) - w / 2, y: Z(p.z) - h / 2, w, h, cx: X(p.x), cy: Z(p.z) }; };
    // order lines, dashed, coloured by kind
    mark('over');
    g.setLineDash([5 * U, 3.5 * U]); g.lineWidth = Math.max(1, 1.4 * U);
    for (const l of v.lines || []) {
      const a = pt(l.from), b = pt(l.to); if (!a || !b) continue;
      g.strokeStyle = LINE[l.kind] || '#d8d8d8';
      g.beginPath(); g.moveTo(X(a.x), Z(a.z)); g.lineTo(X(b.x), Z(b.z)); g.stroke();
      g.fillStyle = g.strokeStyle; g.beginPath(); g.arc(X(b.x), Z(b.z), 2 * U, 0, 2 * Math.PI); g.fill();
    }
    g.setLineDash([]);
    // queued build sites: an outlined square in the owner's colour, and on the whitebox its building's name
    for (const q of v.queued || []) {
      const b = box(q);
      mark('over');
      g.lineWidth = Math.max(1, 1.5 * U); g.strokeStyle = TEAM[q.owner | 0] || '#ccc';
      g.setLineDash([3 * U, 2 * U]); g.strokeRect(b.x, b.y, b.w, b.h); g.setLineDash([]);
      if (at('label')) label(q.type || '', b, 'rgba(20,20,20,.75)');
    }
    // units: grey boxes at their footprint, a team-coloured edge; construction fills up from the bottom, a reclaim empties
    for (const u of v.units || []) {
      const b = box(u), built = u.built == null ? 1 : clamp(+u.built, 0, 1), rec = clamp(+u.reclaimed || 0, 0, 1);
      const bw = Math.max(1.5, 2 * U);
      if (at('box', u.type)) {
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
        mark('over');
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
      if (at('box', st.type)) { g.fillStyle = 'rgba(138,138,132,.45)'; g.fillRect(b.x + lw, b.y + lw, b.w - 2 * lw, b.h - 2 * lw); }
      mark('over');
      g.strokeStyle = col; g.lineWidth = lw; g.strokeRect(b.x + lw / 2, b.y + lw / 2, b.w - lw, b.h - lw);
      if (st.order != null) {
        const sz = Math.max(5 * U, Math.min(7.5 * U, b.h * .32));
        text(String(st.order), b.x + 2.5 * lw, b.y + 2 * lw + sz / 2, sz, 'left', '#101010');
      }
    }
    // the placement ghost: translucent green where it may go, red where it may not
    if (v.ghost && pt(v.ghost)) {
      const b = box(v.ghost), ok = v.ghost.ok !== false;
      if (at('box', v.ghost.type)) { g.fillStyle = ok ? 'rgba(40,210,70,.35)' : 'rgba(230,40,40,.35)'; g.fillRect(b.x, b.y, b.w, b.h); }
      mark('over');
      g.strokeStyle = ok ? '#2bd248' : '#e62828'; g.lineWidth = Math.max(1, 1.5 * U); g.strokeRect(b.x, b.y, b.w, b.h);
      if (at('box', v.ghost.type)) label(v.ghost.type || '', b, ok ? '#0c3a12' : '#4a0a0a');
    }
    mark('over');
    if (v.drag) {
      const d = v.drag, x0 = X(Math.min(d.x0, d.x1)), y0 = Z(Math.min(d.z0, d.z1));
      g.strokeStyle = '#f2f6e8'; g.lineWidth = Math.max(1, U); g.strokeRect(x0, y0, Math.abs(d.x1 - d.x0) * s, Math.abs(d.z1 - d.z0) * s);
    }
    /* THE CURSOR: the game's own when the cursors are loaded (loadCursors), as the game draws the player's
       (tagpu_gui_surf.c sharp_cursor) -- the frame its animation shows `o.tick - v.cursor.since` ticks after the shape
       began, at `scale` device pixels an art pixel whatever the map's scale or the HUD's, the frame's hotspot on the
       pointer's device pixel, the floats as the C side's. Else, for a shape whose frames did not load, its kit icon at
       screen size centred on the point, in a box snapped as the game snaps a part's, and the icon as large as fits in
       it, centred (tagpu_hudlay_expand's icon) */
    const cur = o.cursor !== false && pt(v.cursor), cs = cur && o.cursors && o.cursors.sheet ? o.cursors.seq[v.cursor.shape] : null;
    const ic = cur && !(cs && cs.n > 0) && o.icons && o.icons[CURSOR[v.cursor.shape] || CURSOR.arrow];
    let drew = false;
    if (cs && cs.n > 0) {
      const F = o.cursors.frames, f = cursorFrame(F.slice(cs.first, cs.first + cs.n), cs.n, cs.loop, (+o.tick || 0) - (+v.cursor.since || 0));
      if (f >= 0) {
        const q = F[cs.first + f], k = Math.fround(o.cursors.scale), dx = snap(X(cur.x)), dy = snap(Z(cur.z));
        const w = Math.trunc(Math.fround(Math.fround(q.w * k) + 0.5)), h = Math.trunc(Math.fround(Math.fround(q.h * k) + 0.5));
        if (w > 0 && h > 0) {
          const sm = g.imageSmoothingEnabled; g.imageSmoothingEnabled = false;
          g.drawImage(o.cursors.sheet, q.sx, 0, q.w, q.h, dx - Math.trunc(Math.fround(q.hx * k)), dy - Math.trunc(Math.fround(q.hy * k)), w, h);
          g.imageSmoothingEnabled = sm;
          drew = true;
        }
      }
    } else if (cur && ic && ic.img) {
      const w = ic.w * U, h = ic.h * U, a = X(cur.x) - w / 2, b = Z(cur.z) - h / 2;
      const x0 = snap(a), y0 = snap(b), bw = snap(a + w) - x0, bh = snap(b + h) - y0;
      const f = Math.min(bw / ic.pw, bh / ic.ph), iw = ic.pw * f, ih = ic.ph * f;
      const q = g.imageSmoothingQuality; g.imageSmoothingQuality = 'high';
      g.drawImage(ic.img, x0 + (bw - iw) / 2, y0 + (bh - ih) / 2, iw, ih);
      g.imageSmoothingQuality = q;
      drew = true;
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
    return { s, ox, oz, cell, mw, mh, x0: C.x0, cw: C.w, ch: C.h, cursor: drew };
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

  /* THE ENGINE'S CURSOR ANIMATION, tagpu_wikistage_cursor_frame line for line: the frame `elapsed` 30 Hz ticks after
     SetUICursor 0x4AB400 installed a sequence of `n` frames, each up for its `dur` ticks; -1 past the end of one that
     does not loop, where the engine draws nothing. 0x4B8B30 starts at frame 0 with its ticks to count down; 0x4B8BF0
     takes the ticks elapsed off and, at or below zero, steps on and adds the next frame's. A one-frame sequence never
     steps; a looping one is back at its start after its whole length, so only the remainder is run; a length of 0 is
     frame 0. */
  function cursorFrame(frames, n, loop, elapsed) {
    if (n <= 0) return -1;
    if (n === 1 || elapsed <= 0) return 0;
    let total = 0;
    for (let i = 0; i < n; i++) total += frames[i].dur;
    if (total <= 0) return 0;
    if (loop) elapsed %= total;
    let cd = frames[0].dur - elapsed, f = 0;
    while (cd <= 0) {
      if (++f >= n) { if (!loop) return -1; f = 0; }
      cd += frames[f].dur;
    }
    return f;
  }

  /* The game's cursors from `base` (tools/wiki/gfx.py's pack: cursors.json and the sheet cursors.rgba, resolved from the
     player's own archives and never committed), for a page: { sheet, frames, seq, scale }, filled in once both have
     loaded. draw() draws each shape's kit icon until then, and for good when the pack has none, as the game does when
     its cursors did not resolve. `frames` are the sheet's cells, { sx, w, h, hx, hy, dur }; `seq` each shape's
     { first, n, loop }; `scale` the game's default, 1 device pixel an art pixel. */
  function loadCursors(base) {
    const cursors = { sheet: null, frames: [], seq: {}, scale: 1 };
    Promise.all([fetch(base + 'cursors.json').then(r => { if (!r.ok) throw new Error(r.status); return r.json(); }),
      fetch(base + 'cursors.rgba').then(r => { if (!r.ok) throw new Error(r.status); return r.arrayBuffer(); })])
      .then(([j, buf]) => {
        const c = document.createElement('canvas');
        c.width = j.sheet[0]; c.height = j.sheet[1];
        c.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(buf), j.sheet[0], j.sheet[1]), 0, 0);
        Object.assign(cursors, { frames: j.frames, seq: j.seq, sheet: c });
      }).catch(() => {});
    return cursors;
  }

  return { draw, fit, content, cursorShape, cursorFrame, loadIcons, loadCursors, FACE, CURSOR, ICON_BOX, SIDE_WB };
})();
if (typeof module !== 'undefined') module.exports = WikiStage;
