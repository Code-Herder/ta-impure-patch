/* side.js -- the game's left sidebar in the running mod's own pictures: one view's column composed as the engine
   composes it, in palette indices, from the mod's sidebar pack (tools/wiki/gfx.py writes it into the gitignored
   gfx/side/<mod>/, from the player's own install). stage.js draws the result as one image into the column's rect;
   without a pack, or under the whitebox look, it draws the whitebox column instead (stage.js, drawSide).

   compose() is a pure integer function: art, layout and view in, palette indices and coverage out. The DLL's
   tagpu_wikiside.c is to be its port, compared byte for byte (research/notes/ingame-wiki-sidebar-plan.md and its
   spec, section 5.2). Every frame rect is checked against the sheet when the pack loads (read), so compose never
   reads outside the sheet, and every write is clipped to the column.

   Runs in a page (the global WikiSide) and under node (module.exports), as sim.js does. */
const WikiSide = (() => {
  const COL = 128;                 // the engine's panel width, whatever the HUD scale
  const MINIMAP = 126;             // 0x466780: the minimap picture's long side
  const PRESSED_DX = 1;            // GUI_ButtonDraw 0x4A5F40 moves a pressed button's text one pixel right and down

  /* THE MINIMAP'S PICTURE, 0x466780: the long side 126 pixels, the short one trunc(126 * short / long), centred on the
     short axis inside the 128 x 128 square. The extent is THE STAGE MAP (W, H world units), where the engine uses its
     scroll extent: that extent would cut the stage's bottom rows off the minimap. */
  function geometry(W, H) {
    if (W >= H) { const h = Math.trunc(MINIMAP * H / W); return { w: MINIMAP, h, offx: 0, offy: Math.trunc((MINIMAP - h) / 2) }; }
    const w = Math.trunc(MINIMAP * W / H);
    return { w, h: MINIMAP, offx: Math.trunc((MINIMAP - w) / 2), offy: 0 };
  }
  // a dot's pixel in the picture (0x466DC0): the unit's world point, flat on the stage, scaled by the picture over the
  // extent, each step truncated as the engine's integer arithmetic does
  const dotAt = (x, z, m, W, H) => [Math.trunc(Math.trunc(x) * m.w / W), Math.trunc(Math.trunc(z) * m.h / H)];

  /* INTERIM (spec 2.3, until capture C2 shows how Mayhem and ProTA draw a one-frame picture): pressed is frame 0 moved
     one pixel right and down, greyed is frame 0 under 50 % black. In palette indices, 50 % black is this remap: each
     index to the palette entry nearest (squared RGB distance, the first index on a tie) to half its colour. */
  function nearest(pal, r, g, b) {
    let best = 0, bd = Infinity;
    for (let j = 0; j < 256; j++) {
      const d = (pal[j * 4] - r) ** 2 + (pal[j * 4 + 1] - g) ** 2 + (pal[j * 4 + 2] - b) ** 2;
      if (d < bd) { bd = d; best = j; }
    }
    return best;
  }
  function halfTable(pal) {
    const t = new Uint8Array(256);
    for (let i = 0; i < 256; i++) t[i] = nearest(pal, pal[i * 4] >> 1, pal[i * 4 + 1] >> 1, pal[i * 4 + 2] >> 1);
    return t;
  }

  /* THE COLUMN, 128 x layout.height, for one view (spec 5.2). `art` is a pack read by read(); `base` the minimap's base
     picture (geometry's w x h palette indices: the pack's art.minimap.bases[entry]) or null, when the minimap is left to the
     whitebox (the result's `minimap` false); mapW and mapH the stage map's extent in world units. In order:
     PANELSIDE; the minimap -- the base under the fog, the dots, the camera box; the routed screen -- its panel
     picture, then each drawn gadget's frame for its state. Returns { idx, a, h, minimap }, a pixel painted where a
     is 1. */
  function compose(art, layout, view, base, mapW, mapH) {
    const sv = view.side, H = sv.h, idx = new Uint8Array(COL * H), a = new Uint8Array(COL * H);
    const S = art.sheet;
    // a frame [sx, sy, w, h, hx, hy] with its top left at (dx, dy), clipped to clip = [x0, y0, x1, y1) and the
    // column, skipping the key index (-1: none); `map` remaps each index (the one-frame greyed state)
    const blit = (f, dx, dy, key, clip, map) => {
      const [sx, sy, w, h] = f;
      const x0 = Math.max(dx, clip[0], 0), x1 = Math.min(dx + w, clip[2], COL);
      const y0 = Math.max(dy, clip[1], 0), y1 = Math.min(dy + h, clip[3], H);
      for (let y = y0; y < y1; y++) {
        const src = (sy + y - dy) * S.w + sx - dx, dst = y * COL;
        for (let x = x0; x < x1; x++) {
          const p = S.px[src + x];
          if (p === key) continue;
          idx[dst + x] = map ? map[p] : p; a[dst + x] = 1;
        }
      }
    };
    const ALL = [0, 0, COL, H];
    // a panel picture and PANELSIDE land at their origin minus the hotspot (0x4B0230 passes the origin and
    // CopyGafToContext 0x4B7F90 subtracts the hotspot)
    if (art.panelside) { const f = art.panelside.f; blit(f, -f[4], -f[5], art.panelside.key, ALL); }

    let minimap = false;
    const m = geometry(mapW, mapH), M = view.map;
    if (base && base.length === m.w * m.h && M) {
      minimap = true;
      const mw = M.w | 0, mh = M.h | 0, cols = Math.trunc(mw / 2), rows = Math.trunc(mh / 2), fog = M.fog || [];
      const un = art.minimap.unexplored, seen = art.seen;
      // 0x466C20: each picture pixel takes the fog of its 32-unit cell, read at the cell's first 16-unit cell
      for (let y = 0; y < m.h; y++) for (let x = 0; x < m.w; x++) {
        const c = Math.trunc(x * cols / m.w), r = Math.trunc(y * rows / m.h), f = fog[2 * r * mw + 2 * c] | 0, b = base[y * m.w + x];
        const o = (m.offy + y) * COL + m.offx + x;
        idx[o] = f === 2 ? un : f === 1 ? seen[b] : b; a[o] = 1;
      }
      const pic = [m.offx, m.offy, m.offx + m.w, m.offy + m.h], D = art.dots;
      const dot = (u, f) => { const [px, py] = dotAt(u.x, u.z, m, mapW, mapH); blit(f, m.offx + px - f[4], m.offy + py - f[5], D.key, pic); };
      // radlogo's frame is the owner's colour (PlayerInfo+0x96), as the 3D look's team faces take it; an owner past
      // the frames has no dot
      for (const u of view.units || []) { const f = D.radlogo[u.owner | 0]; if (f) dot(u, f); }
      // the unit under the pointer gets radlogohigh as well (0x466DC0, +0xA8 == main+0x2CBA)
      const hi = sv.under != null && D.high && (view.units || []).find(u => u.id === sv.under);
      if (hi) dot(hi, D.high);
      // 0x466B70: the camera box. The stage shows the whole map, so the box is the picture's border
      const bx = art.minimap.box;
      for (let x = m.offx; x < m.offx + m.w; x++) { idx[m.offy * COL + x] = bx; idx[(m.offy + m.h - 1) * COL + x] = bx; a[m.offy * COL + x] = a[(m.offy + m.h - 1) * COL + x] = 1; }
      for (let y = m.offy; y < m.offy + m.h; y++) { idx[y * COL + m.offx] = bx; idx[y * COL + m.offx + m.w - 1] = bx; a[y * COL + m.offx] = a[y * COL + m.offx + m.w - 1] = 1; }
    }

    const scr = layout.screens[sv.screen], sa = art.screens[sv.screen];
    if (scr && sa) {
      const y0 = scr.y, rect = [0, y0, scr.w, y0 + scr.h], P = sa.panel;
      /* THE PANEL PICTURE (0x4B0230): drawn once at the panel's origin minus its hotspot, not tiled. The pack composes
         a many-framed BackTile, which the engine tiles as a 3 x 3 nine-slice, into this one frame. With no picture the
         engine fills the panel with GUI colour gi+0x8C6, which the pack does not carry: INTERIM, the palette entry
         nearest the whitebox panel's colour (read()'s panelFill). */
      if (P) blit(P.f, -P.f[4], y0 - P.f[5], P.key, rect);
      else for (let y = y0; y < Math.min(y0 + scr.h, H); y++) for (let x = 0; x < scr.w; x++) { idx[y * COL + x] = art.panelFill; a[y * COL + x] = 1; }
      /* THE BUTTONS: GUI_ButtonDraw 0x4A5F40 passes the gadget's corner PLUS the hotspot, so a button's frame lands
         with its top left on the gadget's corner whatever its hotspot. frame(state): 0 at rest; 1 pressed or the
         current tab; greyed, the last frame for attribs 0x100, else min(2, n - 1). A `label` picture paints nothing:
         stage.js draws it with ops over the image. */
      scr.gadgets.forEach((g, i) => {
        const st = sv.state[i], pic = sa.pics[i];
        if (st === -1 || st == null || !pic || pic.from === 'label' || !pic.frames || !pic.frames.length) return;
        const F = pic.frames, n = F.length, [gx, gy] = g.rect;
        if (n === 1) {
          // INTERIM, one frame (see halfTable)
          const d = st === 1 ? PRESSED_DX : 0;
          blit(F[0], gx + d, y0 + gy + d, pic.key, ALL, st === 2 ? art.half : null);
          return;
        }
        const f = st === 0 ? F[0] : st === 1 ? F[1] : (pic.attribs & 0x100) ? F[n - 1] : F[Math.min(2, n - 1)];
        blit(f, gx, y0 + gy, pic.key, ALL);
      });
    }
    return { idx, a, h: H, minimap };
  }

  // the composed column as RGBA through the pack's palette; an unpainted pixel stays transparent
  function rgba(art, c) {
    const out = new Uint8ClampedArray(COL * c.h * 4), pal = art.pal;
    for (let i = 0; i < COL * c.h; i++) if (c.a[i]) {
      const p = c.idx[i] * 4;
      out[i * 4] = pal[p]; out[i * 4 + 1] = pal[p + 1]; out[i * 4 + 2] = pal[p + 2]; out[i * 4 + 3] = 255;
    }
    return out;
  }

  /* A PACK READ AND CHECKED (spec 2.4): side.json with its sheet, palette and seen remap, each a file beside it.
     Every number is data until checked: a frame whose rect leaves the sheet, a picture list that does not match its
     screen's gadgets, a palette or remap of the wrong size refuses that element, which is reported in `problems`
     and falls back as the spec says (a picture to a labelled button; a missing PANELSIDE or panel to nothing; a
     pack without its sheet or palette has no art at all). The layout is returned as found: the caller checks it
     with WikiSim.checkLayout and falls back to the fixture. */
  function read(json, sheet, pal, seen) {
    const problems = [], art = { screens: {} };
    if (!json || json.format !== 1) return { layout: null, art: null, minimap: {}, problems: ['side.json: format is not 1'] };
    const A = json.art || {}, sh = A.sheet || {};
    // the minimap's base per entry: a file beside side.json, w x h palette indices; compose checks its size
    const minimap = {};
    for (const [id, b] of Object.entries((A.minimap && A.minimap.bases) || {}))
      if (b && typeof b.file === 'string' && !b.file.includes('..')) minimap[id] = { file: b.file };
    if (!(Number.isInteger(sh.w) && Number.isInteger(sh.h) && sh.w > 0 && sh.h > 0 && sheet && sheet.length === sh.w * sh.h))
      return { layout: json.layout || null, art: null, minimap, problems: ['the sheet is missing or not w x h bytes'] };
    if (!pal || pal.length !== 1024) return { layout: json.layout || null, art: null, minimap, problems: ['the palette is not 256 x RGBA'] };
    art.sheet = { px: sheet, w: sh.w, h: sh.h };
    art.pal = pal;
    art.half = halfTable(pal);
    // a panel with no picture, INTERIM (compose): stage.js's whitebox panel, #3a3a36, in the mod's palette
    art.panelFill = nearest(pal, 0x3a, 0x3a, 0x36);
    const okFrame = f => Array.isArray(f) && f.length === 6 && f.every(Number.isInteger) && f[0] >= 0 && f[1] >= 0 &&
      f[2] > 0 && f[3] > 0 && f[0] + f[2] <= sh.w && f[1] + f[3] <= sh.h;
    const key = k => Number.isInteger(k) && k >= 0 && k < 256 ? k : -1;
    if (A.panelside && okFrame(A.panelside.f)) art.panelside = { f: A.panelside.f, key: key(A.panelside.key) };
    else problems.push('PANELSIDE: no frame inside the sheet');
    const D = A.dots || {};
    art.dots = { key: key(D.key), radlogo: Array.isArray(D.radlogo) ? D.radlogo.filter(okFrame) : [], high: okFrame(D.high) ? D.high : null };
    if (!art.dots.radlogo.length) problems.push('dots: no radlogo frame inside the sheet');
    const MM = A.minimap || {}, idx = v => Number.isInteger(v) && v >= 0 && v < 256;
    art.minimap = { unexplored: idx(MM.unexplored) ? MM.unexplored : 0, box: idx(MM.box) ? MM.box : 0, interim: !!MM.interim };
    if (seen && seen.length === 256) art.seen = seen;
    else { art.seen = new Uint8Array(256).map((_, i) => i); problems.push('seen.bin: not 256 bytes; the seen fog is the base'); }
    const L = json.layout && json.layout.screens || {};
    for (const [name, s] of Object.entries(A.screens || {})) {
      const gadgets = L[name] && Array.isArray(L[name].gadgets) ? L[name].gadgets : null;
      if (!gadgets) { problems.push(`${name}: art for a screen the layout does not hold`); continue; }
      const out = { panel: null, pics: [] }, P = s.panel;
      if (P && okFrame(P.f)) out.panel = { f: P.f, key: key(P.key) };
      else if (P) problems.push(`${name}: the panel picture is not inside the sheet`);
      const pics = Array.isArray(s.pics) ? s.pics : [];
      if (pics.length !== gadgets.length) problems.push(`${name}: ${pics.length} pictures for ${gadgets.length} gadgets`);
      out.pics = gadgets.map((g, i) => {
        const p = pics[i];
        const text = p && typeof p.text === 'string' ? p.text : '';
        if (!p || p.from === 'label' || !Array.isArray(p.frames) || !p.frames.length) return { from: 'label', text };
        if (!p.frames.every(okFrame)) { problems.push(`${name}/${g.name}: a frame outside the sheet; labelled`); return { from: 'label', text }; }
        return { from: p.from, frames: p.frames, key: key(p.key), attribs: Number.isInteger(p.attribs) ? p.attribs : 0 };
      });
      art.screens[name] = out;
    }
    return { layout: json.layout || null, art, minimap, problems };
  }

  /* A pack from `base` (gfx/side/<mod>/), for a page: { layout, art, minimap, problems, report }, or throws when there is no
     side.json. A missing sheet or palette leaves the layout without art. */
  async function load(base) {
    const r = await fetch(base + 'side.json');
    if (!r.ok) throw new Error(`${base}side.json: ${r.status}`);
    const j = await r.json(), A = j.art || {};
    const bin = f => f ? fetch(base + f).then(q => q.ok ? q.arrayBuffer().then(b => new Uint8Array(b)) : null, () => null) : Promise.resolve(null);
    const [sheet, pal, seen] = await Promise.all([bin(A.sheet && A.sheet.file), bin(A.palette), bin(A.minimap && A.minimap.seen)]);
    return { ...read(j, sheet, pal, seen), report: j.report || [], mod: j.mod };
  }

  return { compose, rgba, read, load, geometry, dotAt, halfTable, COL };
})();
if (typeof module !== 'undefined') module.exports = WikiSide;
