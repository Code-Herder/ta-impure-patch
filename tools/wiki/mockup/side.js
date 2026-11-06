/* side.js -- the game's left sidebar in the running mod's own pictures: one view's column composed as the engine
   composes it, in palette indices, from the mod's sidebar pack (tools/wiki/gfx.py writes it into the gitignored
   gfx/side/<mod>/, from the player's own install). stage.js draws the result as one image into the column's rect;
   without a pack, or under the whitebox look, it draws the whitebox column instead (stage.js, drawSide).

   compose() is a pure integer function: art, layout and view in, palette indices and coverage out. The DLL's
   tagpu_wikiside.c is to be its port, compared byte for byte (research/notes/ingame-wiki.md, *The left sidebar on the
   stage*, "How it is drawn"). Every frame rect is checked against the sheet when the pack loads (read), so compose never
   reads outside the sheet, and every write is clipped to the column.

   Runs in a page (the global WikiSide) and under node (module.exports), as sim.js does. */
const WikiSide = (() => {
  const COL = 128;                 // the engine's panel width, whatever the HUD scale
  const MINIMAP = 126;             // 0x466780: the minimap picture's long side
  /* What lies where nothing else is painted -- under and below a screen shorter than the column, in a panel
     picture's transparent pixels: palette index 0. The HUD pass 0x467D70 fills its whole surface with 0
     (SurfaceFill 0x4C6890 at 0x467D87) before PANELSIDE, DISASSEMBLED; all six compat setups' captures show (0, 0, 0)
     there, index 0 of each install's palette (MEASURED 2026-10-27). */
  const CLEAR = 0;

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
  /* THE CAMERA BOX, 0x466B70 (DISASSEMBLED 2026-11-01, 0x466B70..0x466C12): [left, top, right, bottom], inclusive
     column pixels, for the picture `m` (geometry) over the extent W x H and the eye { x, z, w, h } (the eye and the
     viewport in world units, frame.js view's `eye`; null: eye 0 and the viewport the whole extent):
     left = pw eyeX / W + offx, top = ph eyeZ / H + offy, right = pw w / W + left - 1, bottom = ph h / H + top - 1, every
     division truncating as the engine's idiv. The engine's view is in cells (main+0x1423B, +0x1423F) times 16; the
     stage's viewport is its world units [INFERRED: equal for a viewport of whole cells, which a screen's width is].
     With no eye it is the picture's border. */
  function cameraBox(m, W, H, eye) {
    const ex = eye ? eye.x : 0, ez = eye ? eye.z : 0, vw = eye ? eye.w : W, vh = eye ? eye.h : H;
    const left = Math.trunc(m.w * ex / W) + m.offx, top = Math.trunc(m.h * ez / H) + m.offy;
    return [left, top, Math.trunc(m.w * vw / W) + left - 1, Math.trunc(m.h * vh / H) + top - 1];
  }

  // the palette entry nearest (squared RGB distance, the first index on a tie) to a colour
  function nearest(pal, r, g, b) {
    let best = 0, bd = Infinity;
    for (let j = 0; j < 256; j++) {
      const d = (pal[j * 4] - r) ** 2 + (pal[j * 4 + 1] - g) ** 2 + (pal[j * 4 + 2] - b) ** 2;
      if (d < bd) { bd = d; best = j; }
    }
    return best;
  }
  /* THE COLUMN, 128 x layout.height, for one view (ingame-wiki.md, *How it is drawn*). `art` is a pack read by read();
     `base` the minimap's base picture (geometry's w x h palette indices: the pack's art.minimap.bases[stage]) or null,
     when the minimap is left to the whitebox (the result's `minimap` false); mapW and mapH the stage map's extent in
     world units. In order: the engine's black (CLEAR) over the whole column; PANELSIDE; the minimap -- the base under
     the fog, the dots, the camera box; the routed screen -- its panel picture, then each drawn gadget's frame for its
     state. Returns { idx, a, h, minimap }, a pixel painted where a is 1, which the black clear makes every pixel. */
  function compose(art, layout, view, base, mapW, mapH) {
    const sv = view.side, H = sv.h, idx = new Uint8Array(COL * H).fill(CLEAR), a = new Uint8Array(COL * H).fill(1);
    const S = art.sheet;
    // a frame [sx, sy, w, h, hx, hy] with its top left at (dx, dy), clipped to clip = [x0, y0, x1, y1) and the
    // column, skipping the key index (-1: none)
    const blit = (f, dx, dy, key, clip) => {
      const [sx, sy, w, h] = f;
      const x0 = Math.max(dx, clip[0], 0), x1 = Math.min(dx + w, clip[2], COL);
      const y0 = Math.max(dy, clip[1], 0), y1 = Math.min(dy + h, clip[3], H);
      for (let y = y0; y < y1; y++) {
        const src = (sy + y - dy) * S.w + sx - dx, dst = y * COL;
        for (let x = x0; x < x1; x++) {
          const p = S.px[src + x];
          if (p === key) continue;
          idx[dst + x] = p; a[dst + x] = 1;
        }
      }
    };
    const ALL = [0, 0, COL, H];
    /* PANELSIDE lands with its top left at (0, 0) whatever its hotspot: 0x467D70 passes the frame's own hotspot as
       the point (0x467E30..0x467E3C) and CopyGafToContext 0x4B7F90 subtracts it again, DISASSEMBLED. TA Zero's is
       128 x 128 with its hotspot at (64, 64), and its capture shows it at (0, 0) (MEASURED 2026-10-27). It is the
       view's side's: each layout side has its own (<prefix>PANELSIDE), and a side without one has none drawn. */
    const ps = art.panelside[sv.index];
    if (ps) blit(ps.f, 0, 0, ps.key, ALL);

    let minimap = false;
    const m = geometry(mapW, mapH), M = view.map;
    if (base && base.length === m.w * m.h && M) {
      minimap = true;
      const mw = M.w | 0, mh = M.h | 0, cols = Math.trunc(mw / 2), rows = Math.trunc(mh / 2), fog = M.fog || [];
      const un = art.minimap.unexplored, seen = art.seen, mini = view.mini || null;
      // a 32-unit cell's fog: the map's at the cell's first 16-unit cell, or under a players frame's Sight the pass's
      // (frame.js view's `mini`), seen where its block is unlit
      const fogAt = (c, r) => mini ? (c < mini.gw && r < mini.gh && mini.lit[r * mini.gw + c] ? 0 : 1) : fog[2 * r * mw + 2 * c] | 0;
      // 0x466C20: each picture pixel takes the fog of its 32-unit cell
      for (let y = 0; y < m.h; y++) for (let x = 0; x < m.w; x++) {
        const f = fogAt(Math.trunc(x * cols / m.w), Math.trunc(y * rows / m.h)), b = base[y * m.w + x];
        const o = (m.offy + y) * COL + m.offx + x;
        idx[o] = f === 2 ? un : f === 1 ? seen[b] : b; a[o] = 1;
      }
      const pic = [m.offx, m.offy, m.offx + m.w, m.offy + m.h], D = art.dots;
      const dot = (u, f) => { const [px, py] = dotAt(u.x, u.z, m, mapW, mapH); blit(f, m.offx + px - f[4], m.offy + py - f[5], D.key, pic); };
      // radlogo's frame is the owner's colour (PlayerInfo+0x96), as the 3D look's team faces take it; an owner past
      // the frames has no dot. Every unit has one, or under Sight the pass's dots
      for (const u of mini ? mini.dots : view.units || []) { const f = D.radlogo[u.owner | 0]; if (f) dot(u, f); }
      // the unit under the pointer gets radlogohigh as well (0x466DC0, +0xA8 == main+0x2CBA)
      const hi = sv.under != null && D.high && (view.units || []).find(u => u.id === sv.under);
      if (hi) dot(hi, D.high);
      // 0x466B70: the camera box (cameraBox), inside the picture
      const bx = art.minimap.box, [l, t, r, bt] = cameraBox(m, mapW, mapH, view.eye || null);
      const put = (x, y) => { if (x >= pic[0] && x < pic[2] && y >= pic[1] && y < pic[3]) { idx[y * COL + x] = bx; a[y * COL + x] = 1; } };
      for (let x = l; x <= r; x++) { put(x, t); put(x, bt); }
      for (let y = t; y <= bt; y++) { put(l, y); put(r, y); }
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
        const F = pic.frames, n = F.length, [gx, gy, gw, gh] = g.rect;
        /* Pressed asks for frame 1 (0x4A6110..0x4A614B), which a one-frame sequence does not have: 0x4B7F30 returns
           none past the count and 0x4A6176 skips the blit, so a held one-frame button shows the panel under it. */
        const f = st === 0 ? F[0] : st === 1 ? F[1] : (pic.attribs & 0x100) ? F[n - 1] : F[Math.min(2, n - 1)];
        if (f) blit(f, gx, y0 + gy, pic.key, ALL);
        /* GREYED, the engine tints the gadget's whole rect once its frame is drawn, what lies under the frame's
           transparent pixels included (0x4A699B..0x4A69BA, DISASSEMBLED): the grey remap, then the box shader at
           level -20, which the pack composes into one table (gfx.py greyed_table). Not for attribs 0x100, whose last
           frame is the greyed picture, nor for a checkbox (attribs 0x80), as GUI_ButtonDraw decides at
           0x4A607E..0x4A6106. Without the table (the install has no PALETTE.SHD) the frame stands untinted. */
        if (st === 2 && art.greyed && !(pic.attribs & 0x180)) {
          for (let y = Math.max(y0 + gy, 0); y < Math.min(y0 + gy + gh, H); y++)
            for (let x = Math.max(gx, 0); x < Math.min(gx + gw, COL); x++) idx[y * COL + x] = art.greyed[idx[y * COL + x]];
        }
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

  /* A PACK READ AND CHECKED (ingame-wiki.md, *The data format*, a mod's pack): side.json with its sheet, palette,
     seen remap and greyed tint, each a file beside it.
     Every number is data until checked: a frame whose rect leaves the sheet, a picture list that does not match its
     screen's gadgets, a palette or remap of the wrong size refuses that element, which is reported in `problems`
     and falls back as the spec says (a picture to a labelled button; a missing PANELSIDE or panel to nothing; a
     pack without its sheet or palette has no art at all). The layout is returned as found: the caller checks it
     with WikiSim.checkLayout and falls back to the fixture.
     `fonts` ({pack file: bytes}) carries the resource bars' fonts; without it the bars are not read and every side's
     is null, which is also what a pack without `art.bar` gives: bar.js draws no bar for a null side. */
  function read(json, sheet, pal, seen, greyed, fonts) {
    const problems = [], art = { screens: {} };
    if (!json || json.format !== 1) return { layout: null, art: null, minimap: {}, problems: ['side.json: format is not 1'] };
    const A = json.art || {}, sh = A.sheet || {};
    // the minimap's base per stage, `<entry>.<scene>`: a file beside side.json, w x h palette indices; compose checks its size
    const minimap = {};
    for (const [id, b] of Object.entries((A.minimap && A.minimap.bases) || {}))
      if (b && typeof b.file === 'string' && !b.file.includes('..')) minimap[id] = { file: b.file };
    if (!(Number.isInteger(sh.w) && Number.isInteger(sh.h) && sh.w > 0 && sh.h > 0 && sheet && sheet.length === sh.w * sh.h))
      return { layout: json.layout || null, art: null, minimap, problems: ['the sheet is missing or not w x h bytes'] };
    if (!pal || pal.length !== 1024) return { layout: json.layout || null, art: null, minimap, problems: ['the palette is not 256 x RGBA'] };
    art.sheet = { px: sheet, w: sh.w, h: sh.h };
    art.pal = pal;
    // without its tint a greyed button is its frame alone (compose)
    if (greyed && greyed.length === 256) art.greyed = greyed;
    else problems.push('greyed.bin: not 256 bytes; greyed buttons are drawn untinted');
    // a panel with no picture, INTERIM (compose): stage.js's whitebox panel, #3a3a36, in the mod's palette
    art.panelFill = nearest(pal, 0x3a, 0x3a, 0x36);
    const okFrame = f => Array.isArray(f) && f.length === 6 && f.every(Number.isInteger) && f[0] >= 0 && f[1] >= 0 &&
      f[2] > 0 && f[3] > 0 && f[0] + f[2] <= sh.w && f[1] + f[3] <= sh.h;
    const key = k => Number.isInteger(k) && k >= 0 && k < 256 ? k : -1;
    // PANELSIDE, one a layout side, index-aligned with layout.sides; null where that side has none
    const sides = json.layout && Array.isArray(json.layout.sides) ? json.layout.sides : [];
    const PS = Array.isArray(A.panelside) ? A.panelside : [];
    if (PS.length !== sides.length) problems.push(`PANELSIDE: ${PS.length} pictures for ${sides.length} sides`);
    art.panelside = sides.map((sd, k) => {
      const p = PS[k];
      if (p && okFrame(p.f)) return { f: p.f, key: key(p.key) };
      problems.push(`PANELSIDE of ${sd && sd.name}: no frame inside the sheet`);
      return null;
    });
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
    readBar(A, sides, okFrame, key, fonts, art, problems);
    return { layout: json.layout || null, art, minimap, problems };
  }

  /* THE RESOURCE BARS' ART (gfx.py bar_art): art.bar, one a layout side, null or { top, bot: {f, key}, rects, ecol,
     mcol, font: bytes }; art.logos, the badge frames ({f, key}) or null; art.gui, the GUI colour table or null. The
     bounds are gfx.py's, so a pack it wrote passes: every frame inside the sheet; the fifteen rects present, each
     x1 <= x2 and y1 <= y2 with every coordinate in BAR_COORD (bar.js clips what lands outside its picture); the
     colours palette indices; at most BAR_LOGOS badge frames; and the font a .fnt image every glyph of which lies
     inside its bytes (fntProblem). What fails is dropped to null with a problem line. */
  const BAR_RECTS = ['logo', 'energybar', 'energynum', 'energymax', 'energy0', 'metalbar', 'metalnum', 'metalmax',
    'metal0', 'totalunits', 'totaltime', 'energyproduced', 'energyconsumed', 'metalproduced', 'metalconsumed'];
  const BAR_COORD = [-1024, 4096], FNT_MAX = 32, BAR_LOGOS = 32;
  /* why `b` is not a font bar.js may draw with, or null: the engine's .fnt -- rows at byte 0, the signed row offset
     at 2, the first code at 3, a u16 offset per code from 4; a glyph is its width, then rows x width bits. The first
     code must be 0 (a 256-entry table), as tagpu_packet_pub.c holds the engine's own copy to. */
  function fntProblem(b) {
    if (!b || b.length < 4 + 2 * 256) return `${b ? b.length : 0} bytes, short of the 256-code table`;
    const rows = b[0];
    if (rows < 1 || rows > FNT_MAX) return `${rows} rows`;
    if (b[3] !== 0) return `first code ${b[3]}, not 0`;
    for (let c = 0; c < 256; c++) {
      const o = b[4 + 2 * c] | b[5 + 2 * c] << 8;
      if (!o) continue;
      if (o >= b.length) return `glyph ${c} at ${o}, past the file`;
      const w = b[o];
      if (w < 1 || w > FNT_MAX) return `glyph ${c} is ${w} wide`;
      if (o + 1 + Math.ceil(w * rows / 8) > b.length) return `glyph ${c}'s bits run past the file`;
    }
    return null;
  }
  function readBar(A, sides, okFrame, key, fonts, art, problems) {
    const pal = v => Number.isInteger(v) && v >= 0 && v < 256;
    const framed = p => p && okFrame(p.f) ? { f: p.f, key: key(p.key) } : null;
    if (A.gui == null) art.gui = null;
    else if (Array.isArray(A.gui) && A.gui.length === 256 && A.gui.every(pal)) art.gui = Uint8Array.from(A.gui);
    else { art.gui = null; problems.push('gui: not 256 palette indices; no bar is drawn'); }
    art.logos = null;
    if (Array.isArray(A.logos)) {
      const L = A.logos.map(framed);
      if (L.length < 1 || L.length > BAR_LOGOS) problems.push(`logos: ${L.length} frames, not 1..${BAR_LOGOS}; no badge`);
      else if (L.some(f => !f)) problems.push('logos: a frame outside the sheet; no badge');
      else art.logos = L;
    } else if (A.logos != null) problems.push('logos: not a list; no badge');
    const B = Array.isArray(A.bar) && fonts ? A.bar : [];
    art.bar = sides.map((sd, k) => {
      const b = B[k];
      if (b == null) return null;
      const no = why => { problems.push(`bar of ${sd && sd.name}: ${why}; the whitebox bars`); return null; };
      const top = framed(b.top), bot = framed(b.bot);
      if (!top || !bot) return no('PANELTOP or PANELBOT is not inside the sheet');
      const R = b.rects || {}, rects = {};
      for (const n of BAR_RECTS) {
        const r = R[n];
        if (!(Array.isArray(r) && r.length === 4 && r.every(v => Number.isInteger(v) && v >= BAR_COORD[0] && v <= BAR_COORD[1]) &&
              r[0] <= r[2] && r[1] <= r[3])) return no(`rect ${n} is not x1 <= x2, y1 <= y2 inside ${BAR_COORD.join('..')}`);
        rects[n] = r;
      }
      if (!pal(b.ecol) || !pal(b.mcol)) return no('ecol or mcol is not a palette index');
      if (typeof b.font !== 'string' || b.font.includes('..')) return no('no font file');
      const why = fntProblem(fonts[b.font]);
      if (why) return no(`${b.font}: ${why}`);
      return { top, bot, rects, ecol: b.ecol, mcol: b.mcol, font: fonts[b.font] };
    });
    if (Array.isArray(A.bar) && fonts && A.bar.length !== sides.length) problems.push(`bar: ${A.bar.length} for ${sides.length} sides`);
  }

  /* A pack from `base` (gfx/side/<mod>/), for a page: { layout, casting, art, minimap, problems, report }, or throws
     when there is no side.json. The bars' fonts are fetched beside the rest. A missing sheet or palette leaves the
     layout without art. The casting is returned as
     found, index-aligned with layout.sides: the caller checks it with WikiSim.checkCasting, beside the layout. */
  async function load(base) {
    const r = await fetch(base + 'side.json');
    if (!r.ok) throw new Error(`${base}side.json: ${r.status}`);
    const j = await r.json(), A = j.art || {};
    const bin = f => f ? fetch(base + f).then(q => q.ok ? q.arrayBuffer().then(b => new Uint8Array(b)) : null, () => null) : Promise.resolve(null);
    const fontFiles = [...new Set((Array.isArray(A.bar) ? A.bar : [])
      .map(b => b && b.font).filter(f => typeof f === 'string' && !f.includes('..')))];
    const [sheet, pal, seen, greyed, ...fontBytes] = await Promise.all([bin(A.sheet && A.sheet.file), bin(A.palette),
      bin(A.minimap && A.minimap.seen), bin(A.greyed), ...fontFiles.map(bin)]);
    const fonts = Object.fromEntries(fontFiles.map((f, i) => [f, fontBytes[i]]));
    return { ...read(j, sheet, pal, seen, greyed, fonts), casting: j.casting, report: j.report || [], mod: j.mod };
  }

  return { compose, rgba, read, load, geometry, dotAt, cameraBox, fntProblem, COL };
})();
if (typeof module !== 'undefined') module.exports = WikiSide;
