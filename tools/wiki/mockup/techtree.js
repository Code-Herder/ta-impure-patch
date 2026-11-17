/* techtree.js -- the tech tree entry's stage: the running mod's build tree for the player's side, from the side's
   commander through every builder's menu, drawn as a research tree (research/notes/ingame-wiki.md, *The tech tree
   entry*). The data is the mod's pack, gfx/side/<mod>/techtree/tree.json, which tools/wiki/gfx.py `tech_pack` resolves
   from the install: each side's walk and each builder's menu as the sidebar reads them, and each unit's FBI figures.
   This file holds the rules that make a tree of them (model), places the cards (layout), draws them (draw) and finds
   what is under a point (hit). model, layout and hit are pure; draw makes only canvas calls a HUD op list can carry:
   rectangles, lines, images and clipped text.

   Lengths are in wiki units (the screen's height / 720), the window's own; the camera maps them to canvas pixels.

   Runs in a page (the global WikiTech) and under node (module.exports). */
const WikiTech = (() => {
  /* ---- THE MODEL ---- */

  /* AN UPGRADE (or an ability) is what a builder makes that never stands on the field as a unit of its own: a
     non-structure that is 0 x 0 (Escalation's Level 2 Unit Upgrade, Activate Maximum Intel) or born cloaked, stealthed
     and hiding its damage (Mayhem's and Escalation's companion units built onto a moho mine, a geothermal plant, a
     tower: deep-ta-esc.md, *What TA:ESC actually adds*). MEASURED 2026-11-16 over the six supported setups: 19 in
     Escalation, 7 in Mayhem, none in stock, ProTA, Twilight or TA Zero, and no mine (born cloaked, 1 x 1, not hiding
     its damage) among them. */
  const isUpgrade = u => !u.structure && ((u.fp[0] === 0 && u.fp[1] === 0) || (u.cloaked && u.stealth && u.hidedamage));
  const KINDS = ['Commander', 'Factory', 'Builder', 'Economy', 'Defense', 'Support', 'Land', 'Air', 'Sea', 'Upgrade'];

  /* A unit's kind, for the panel's groups. A builder's menu counts without its upgrades, so a tower that builds only its
     own upgrade is a defense, not a factory; a building that makes a resource is economy even when it is armed (an
     Escalation geothermal plant fires). */
  function kindOf(k, u, side, U) {
    if (k === side.commander) return 'Commander';
    if (isUpgrade(u)) return 'Upgrade';
    const menu = (side.menus[k] || []).filter(x => U[x] && !isUpgrade(U[x]));
    if (u.structure && menu.length) return 'Factory';
    if (!u.structure && u.builder && menu.length) return 'Builder';
    if (u.structure) {
      if (u.extracts > 0 || u.energymake > 0 || u.metalmake > 0) return 'Economy';
      if (u.weapons.length) return 'Defense';
      return u.storage[0] > 0 || u.storage[1] > 0 ? 'Economy' : 'Support';
    }
    return u.canfly ? 'Air' : u.water ? 'Sea' : 'Land';
  }

  /* One side's tree: {side, units, order, root, spine, sp}. Each unit gains its kind, its builds and upgrades (its
     menu split), its parents (every builder of the side whose menu holds it), `first` (the builder the walk reached it
     from: the first in walk order whose menu holds it), `unlocks` (what it is the first to reach) and its tier. The
     SPINE is the commander and every builder and factory: the research tree's cards. A card's parent `sp` is the
     nearest spine unit up the `first` chain. */
  function model(tree, si) {
    const side = tree.sides[si], U = tree.units, order = side.order.filter(k => U[k]);
    const menus = {}, parents = {}, first = {};
    for (const k of order) { menus[k] = (side.menus[k] || []).filter(x => U[x]); parents[k] = []; }
    for (const k of order) for (const x of menus[k]) if (parents[x] && !parents[x].includes(k)) parents[x].push(k);
    const seen = new Set([side.commander]);
    for (const k of order) for (const x of menus[k]) if (!seen.has(x)) { seen.add(x); first[x] = k; }
    const units = {};
    for (const k of order) units[k] = { ...U[k], key: k, kind: kindOf(k, U[k], side, U), upgrade: isUpgrade(U[k]) };
    for (const k of order) {
      const u = units[k];
      u.builds = menus[k].filter(x => !units[x].upgrade);
      u.upgrades = menus[k].filter(x => units[x].upgrade);
      u.parents = parents[k];
      u.first = first[k] || null;
      u.unlocks = u.builds.filter(x => first[x] === k);
      // an upgrade is never itself upgradable, whatever its description says ("Orion Upgrade")
      u.upgradable = !u.upgrade && (u.upgrades.length > 0 || /upgrad/i.test(u.description));
    }
    /* THE TIER: the category's LEVELn where the mod writes one (stock's LEVEL1..3), else one more than the builders
       crossed on the way from the commander, at most 4 */
    for (const k of order) {
      const u = units[k], m = /LEVEL(\d)/i.exec(u.category);
      if (m) { u.tier = +m[1]; continue; }
      let t = 1;
      for (let p = u.first; p && p !== side.commander; p = units[p].first) if (units[p].kind === 'Builder') t++;
      u.tier = Math.min(t, 4);
    }
    const spine = order.filter(k => ['Commander', 'Builder', 'Factory'].includes(units[k].kind));
    const inSpine = new Set(spine), sp = {};
    for (const k of spine) { let p = units[k].first; while (p && !inSpine.has(p)) p = units[p].first; sp[k] = p || null; }
    return { side, units, order, root: side.commander, spine, sp };
  }
  // the walk from the commander to k, along `first`
  const pathTo = (M, k) => { const p = []; for (let x = k; x; x = M.units[x].first) p.unshift(x); return p; };
  // the card a unit is shown on: itself when it is one, else the card that first unlocks it
  const home = (M, k) => { let x = k; while (x && !M.sp.hasOwnProperty(x)) x = M.units[x].first; return x; };
  // what a card shows under its header: what it first unlocks that is no card of its own, then its upgrades
  const cardIcons = (M, k) => [...M.units[k].unlocks.filter(x => !M.sp.hasOwnProperty(x)), ...M.units[k].upgrades];

  /* ---- THE LAYOUT, in units ----
     A CARD is W wide; its height is a function of its icon count alone -- the header, the icon rows, the footer line --
     because every text on it is one line, clipped. So the layout is exact before anything is drawn, and the DLL's is
     the same numbers. */
  const C = {
    W: 118, PITCH: 150,          // a card's width, and from one column's left to the next's
    GAP: 6,                      // the least space between two cards of a column
    PAD: 3, HEAD: 30, PIC: 24,   // the header: the build picture, the title, the subtitle and the kind line
    ICON: 13, IGAP: 1, PER: 8,   // the icons under the header, PER a row
    FOOT: 9,                     // the footer line
    EDGE_Y: 15,                  // where an edge meets a card: the header's middle
    R: 2,                        // a card's corner radius
    SEG: 24,                     // the pieces an edge's curve is drawn in
    DASHES: 64,                  // the most pieces a dashed edge is cut in, half of them drawn
    DOT: 16,                     // the stage's dot grid: its pitch, in units of the window, not of the tree
  };
  const cardH = n => C.HEAD + (n ? Math.ceil(n / C.PER) * (C.ICON + C.IGAP) + C.IGAP : 0) + C.FOOT;

  /* A TIDY TREE: each card's column is its depth among the cards; each leaf takes the next row, and a parent sits
     centred over its children but never above the cursor. One cursor runs down the whole tree, and every card starts
     at or below it and moves it past its own bottom plus GAP, so two cards of one column are always GAP apart or more.
     Returns {pos: {k: [x, y, w, h]}, w, h}: the tree's bounds from the first card's corner to the last's. */
  function layout(M) {
    const kids = {}, col = {}, pos = {};
    for (const k of M.spine) if (M.sp[k]) (kids[M.sp[k]] ||= []).push(k);
    for (const k of M.spine) col[k] = M.sp[k] ? col[M.sp[k]] + 1 : 0;
    const h = k => cardH(cardIcons(M, k).length);
    let y = 0;
    const place = k => {
      const ks = kids[k] || [];
      let top;
      if (!ks.length) top = y;
      else {
        const y0 = y;
        ks.forEach(place);
        const a = pos[ks[0]], b = pos[ks[ks.length - 1]];
        top = Math.max(y0, (a[1] + b[1] + b[3]) / 2 - h(k) / 2);
      }
      pos[k] = [col[k] * C.PITCH, top, C.W, h(k)];
      y = Math.max(y, top + h(k) + C.GAP);
    };
    place(M.root);
    let w = 0;
    for (const k of M.spine) w = Math.max(w, pos[k][0] + C.W);
    return { pos, w, h: Math.max(0, y - C.GAP) };
  }

  /* ---- THE CAMERA: canvas pixels = (units - eye) * z, the eye the tree point at the canvas corner ---- */
  const MARGIN = 8;                 // units around the fitted tree
  // the whole tree as large as the canvas allows, centred; s is canvas pixels a unit
  function fit(L, cw, ch, s) {
    const z = Math.max(1e-3, Math.min((cw - 2 * MARGIN * s) / L.w, (ch - 2 * MARGIN * s) / L.h));
    return { z, x: L.w / 2 - cw / 2 / z, y: L.h / 2 - ch / 2 / z };
  }

  /* ---- WHAT IS UNDER A POINT (tree units): {key, card, icon} or null ---- */
  function hit(M, L, px, py) {
    for (const k of M.spine) {
      const [x, y, w, h] = L.pos[k];
      if (px < x || px >= x + w || py < y || py >= y + h) continue;
      const icons = cardIcons(M, k), ix = px - x - C.PAD, iy = py - y - C.HEAD - C.IGAP;
      if (iy >= 0 && ix >= 0) {
        const c = Math.floor(ix / (C.ICON + C.IGAP)), r = Math.floor(iy / (C.ICON + C.IGAP)), i = r * C.PER + c;
        if (c < C.PER && ix % (C.ICON + C.IGAP) < C.ICON && iy % (C.ICON + C.IGAP) < C.ICON && i < icons.length)
          return { key: icons[i], card: k, icon: true };
      }
      return { key: k, card: k, icon: false };
    }
    return null;
  }

  /* ---- THE DRAWING ----
     The look is the research tree's prototype's (tools/wiki/techtree-prototype, commit 3b92ed28): cards with rounded
     corners and a soft shadow, a black line under the header, and each card joined to its children by an S-curve from
     the header's middle; the stage a dot grid. Every length is the prototype's CSS pixel halved, its card being 236 px
     to our 118 units.
     `o`: {cam, s (canvas pixels a unit), sel (the selected unit or null), all (every builder's edge, not only the
     walk's), pic (key -> a loaded image or null), font (role -> CSS font at 1 unit, scaled here)}. The selected unit's
     card, the walk to it and its card's children and parents are lit; every other card is dimmed. */
  const INK = { bg: '#22251e', rim: '#4a4d42', line: '#000000', text: '#d8d8c8', muted: '#8f9384', edge: '#4b5040',
    also: '#6a7058', path: '#7fc4e8', kid: '#9fd68a', sel: '#e8c66a', up: '#d58ae8', pic: '#000000',
    stage: '#151713', dot: '#2a2d25', shadow: '#00000088' };
  const DIM = 0.28;                 // a card the selection does not light (the prototype's opacity)
  /* A COLOUR SEEN THROUGH `a` OVER THE STAGE, as one opaque colour. A card's shapes overlap (the corners' discs over its
     rectangles, the rim under the fill), so drawn translucent each overlap would show darker; mixed first, the card fades
     as one layer, as the prototype's CSS opacity fades it. Under a card is only the stage: the edges run between cards. */
  const mix = (hex, a) => {
    if (a >= 1) return hex;
    const c = parseInt(hex.slice(1, 7), 16), st = parseInt(INK.stage.slice(1), 16);
    let out = 0;
    for (const sh of [16, 8, 0]) out |= Math.round(((c >> sh) & 255) * a + ((st >> sh) & 255) * (1 - a)) << sh;
    return '#' + out.toString(16).padStart(6, '0');
  };
  /* THE GROUND: the stage's fill and its dot grid, fixed to the canvas as the prototype's CSS background is (a pan or a
     zoom moves the tree over it), a dot every DOT units from the canvas's corner, one unit square, whole pixels */
  function ground(g, W, H, s) {
    g.fillStyle = INK.stage; g.fillRect(0, 0, W, H);
    const p = C.DOT * s, d = Math.max(1, Math.round(s));
    g.fillStyle = INK.dot;
    for (let y = 0; Math.round(y) < H; y += p)
      for (let x = 0; Math.round(x) < W; x += p) g.fillRect(Math.round(x), Math.round(y), d, d);
  }
  function draw(g, M, L, o) {
    const { cam, s } = o, z = cam.z;
    /* EVERY EDGE ON A WHOLE PIXEL: a rectangle's corners are rounded to canvas pixels and its size is the difference,
       so a card's rim and an icon's frame stay one crisp line at any zoom instead of smearing across two */
    const X = u => Math.round((u - cam.x) * z), Y = u => Math.round((u - cam.y) * z);
    const box = (x, y, w, h) => [X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y)];
    const sel = o.sel && M.units[o.sel] ? o.sel : null, h = sel && home(M, sel);
    const onPath = new Set(h ? pathTo(M, h) : []), kids = new Set(h ? M.units[h].builds : []);
    const pars = new Set(sel ? M.units[sel].parents : []), focus = !!h;
    const lit = k => k === h || onPath.has(k) || kids.has(k) || pars.has(k);
    g.save();
    g.lineCap = 'butt';
    /* THE EDGES first, under the cards: the walk's (each card to its parent card) and, lit or with `all`, every other
       builder of a card's unit, dashed. Each is the prototype's cubic from the parent's right edge to the child's left at
       the headers' middles, its two handles at the midpoint's x, drawn as SEG straight pieces; two cards level are joined
       by one line, undashed. */
    const edge = (a, b, style, w, dashed) => {
      const [ax, ay, aw] = L.pos[a], [bx, by] = L.pos[b];
      g.strokeStyle = style; g.lineWidth = Math.max(1, Math.round(w * z));
      // a line one pixel wide sits on a pixel's centre
      const o = g.lineWidth % 2 ? 0.5 : 0;
      const x1 = X(ax + aw), y1 = Y(ay + C.EDGE_Y) + o, x2 = X(bx), y2 = Y(by + C.EDGE_Y) + o, mx = (x1 + x2) / 2;
      // an edge wholly off the canvas is not drawn, as a card is not
      if (Math.max(x1, x2) < 0 || Math.min(x1, x2) > g.canvas.width || Math.max(y1, y2) < 0 || Math.min(y1, y2) > g.canvas.height) return;
      /* DASHED BY ITS OWN PIECES: every other one, each a stroke of its own -- as many pieces as dashes of the
         prototype's 2 units fit along it (its x and y extents, at least its length), at least 8 and at most DASHES, so
         a dashed edge is at most DASHES / 2 lines at any zoom (a dash pattern in pixels would be one line a dash, and a
         long edge zoomed in thousands, past what the stage's op list holds) */
      const dl = Math.max(2, Math.round(2 * z));
      const n = !dashed ? C.SEG : 2 * Math.min(C.DASHES / 2, Math.max(4, Math.round((Math.abs(x2 - x1) + Math.abs(y2 - y1)) / (2 * dl))));
      const pt = [[x1, y1]];
      if (y1 === y2 && !dashed) pt.push([x2, y2]);
      else
        for (let i = 1; i <= n; i++) {
          const t = i / n, u = 1 - t, a0 = u * u * u, a1 = 3 * u * u * t, a2 = 3 * u * t * t, a3 = t * t * t;
          pt.push([a0 * x1 + (a1 + a2) * mx + a3 * x2, (a0 + a1) * y1 + (a2 + a3) * y2]);
        }
      if (dashed)
        for (let i = 0; i + 1 < pt.length; i += 2) { g.beginPath(); g.moveTo(...pt[i]); g.lineTo(...pt[i + 1]); g.stroke(); }
      else { g.beginPath(); g.moveTo(...pt[0]); for (const p of pt.slice(1)) g.lineTo(...p); g.stroke(); }
    };
    const lines = [];
    for (const k of M.spine) {
      if (M.sp[k]) {
        const path = onPath.has(k) && onPath.has(M.sp[k]), kid = M.sp[k] === h || (k === h && pars.has(M.sp[k]));
        lines.push([M.sp[k], k, path ? INK.path : kid ? INK.kid : INK.edge, path ? 1.5 : kid ? 1.25 : 1, false, path || kid ? 2 : 1]);
      }
      for (const p of M.units[k].parents) {
        if (p === M.sp[k] || !M.sp.hasOwnProperty(p)) continue;
        const kid = p === h || (k === h && pars.has(p));
        if (kid || o.all) lines.push([p, k, kid ? INK.kid : INK.also, kid ? 1.25 : 1, !kid, kid ? 2 : 0]);
      }
    }
    lines.sort((a, b) => a[5] - b[5]);
    for (const [a, b, st, w, dashed] of lines) edge(a, b, st, w, dashed);
    g.globalAlpha = 1;
    // the cards
    const font = (role, px) => o.font(role).replace(/\d+(\.\d+)?px/, `${px * z}px`);
    /* TEXT TOO SMALL TO READ IS LEFT OUT: a line whose size is under READ canvas pixels would draw as a smear, so the
       card shows its picture, rim and icons alone until the zoom makes the text legible */
    const READ = 6;
    const clip = (t, x, y, w, role, px) => {
      if (px * z < READ) return;
      g.font = font(role, px);
      let s2 = t;
      // cut with three periods: the HUD faces have no ellipsis character
      if (g.measureText(s2).width > w * z) {
        while (s2.length && g.measureText(s2 + '...').width > w * z) s2 = s2.slice(0, -1);
        s2 += '...';
      }
      g.fillText(s2, X(x), Y(y));
    };
    /* A PICTURE: nearest-neighbour when drawn at its own size or larger, so its pixels stay square; filtered at high
       quality when drawn smaller, so it shrinks to a softened likeness rather than to every nth pixel */
    const picAt = (k, x, y, side) => {
      const im = o.pic(k), [bx, by, bw, bh] = box(x, y, side, side);
      g.fillStyle = INK.pic; g.fillRect(bx, by, bw, bh);
      if (im) {
        g.imageSmoothingEnabled = bw < im.width;
        g.imageSmoothingQuality = 'high';
        g.drawImage(im, bx, by, bw, bh);
      }
    };
    /* A ROUNDED RECTANGLE of whole pixels, filled: three rectangles and a disc at each corner (a HUD op list has no
       path, but rectangles and discs) */
    const rrect = (x, y, w, h, r, col) => {
      if (w <= 0 || h <= 0) return;
      r = Math.max(0, Math.min(r, Math.floor(w / 2), Math.floor(h / 2)));
      g.fillStyle = col;
      g.fillRect(x + r, y, w - 2 * r, h);
      g.fillRect(x, y + r, r, h - 2 * r);
      g.fillRect(x + w - r, y + r, r, h - 2 * r);
      if (r > 0)
        for (const [px, py] of [[x + r, y + r], [x + w - r, y + r], [x + r, y + h - r], [x + w - r, y + h - r]]) {
          g.beginPath(); g.arc(px, py, r, 0, 2 * Math.PI); g.fill();
        }
    };
    /* A SOFT SHADOW, CSS's box-shadow 0 1u 3u round the card's rounded box: the canvas casts it from the rounded
       rectangle drawn FAR off the canvas and carried back by its offset, so only the shadow lands (a HOP_SOLID with
       HOPF_DROP and the radius, painted outside the rounded box) */
    const FAR = 1 << 20;
    const shadow = (x, y, w, h, r, a) => {
      g.save();
      g.globalAlpha = a; g.shadowColor = INK.shadow; g.shadowBlur = 3 * z; g.shadowOffsetX = FAR; g.shadowOffsetY = z;
      g.fillStyle = '#000000'; g.beginPath(); g.roundRect(x - FAR, y, w, h, r); g.fill();
      g.restore();
    };
    g.textBaseline = 'alphabetic';
    for (const k of M.spine) {
      const [x, y, w, hh] = L.pos[k], u = M.units[k];
      if (X(x + w) < 0 || Y(y + hh) < 0 || X(x) > g.canvas.width || Y(y) > g.canvas.height) continue;
      const a = focus && !lit(k) ? DIM : 1;
      g.globalAlpha = 1;
      const [cx, cy, cw, ch] = box(x, y, w, hh);
      /* THE CARD: its shadow (the prototype's 0 2px 6px #0008), the selected card's halo (its 2 px ring outside the
         rim), the rim, the fill inside the rim, and the line under the header -- each a rounded rectangle of whole
         pixels, mixed with the stage when the card is dimmed */
      const r = Math.max(1, Math.round(C.R * z)), lw = Math.max(1, Math.round(0.5 * z));
      shadow(cx, cy, cw, ch, r, a);
      if (k === h) { const hw = Math.max(1, Math.round(z)); rrect(cx - hw, cy - hw, cw + 2 * hw, ch + 2 * hw, r + hw, INK.sel); }
      const rim = k === h ? INK.sel : onPath.has(k) ? INK.path : kids.has(k) || pars.has(k) ? INK.kid : INK.rim;
      rrect(cx, cy, cw, ch, r, mix(rim, a));
      rrect(cx + lw, cy + lw, cw - 2 * lw, ch - 2 * lw, r - lw, mix(INK.bg, a));
      g.fillStyle = mix(INK.line, a); g.fillRect(cx + lw, Y(y + C.HEAD), cw - 2 * lw, lw);
      g.globalAlpha = a;
      picAt(k, x + C.PAD, y + C.PAD, C.PIC);
      const tx = x + C.PAD * 2 + C.PIC, tw = w - (tx - x) - C.PAD;
      g.fillStyle = INK.text; clip(u.title, tx, y + 10, tw, 'title', 7);
      g.fillStyle = INK.muted; clip(u.description, tx, y + 18, tw, 'body', 5.5);
      g.fillStyle = u.upgradable ? INK.up : INK.muted;
      clip(`${u.kind.toUpperCase()} · T${u.tier}${u.upgrades.length ? ` · ${u.upgrades.length} UP` : ''}`, tx, y + 26, tw, 'label', 5);
      const icons = cardIcons(M, k);
      icons.forEach((x2, i) => {
        const ix = x + C.PAD + (i % C.PER) * (C.ICON + C.IGAP), iy = y + C.HEAD + C.IGAP + Math.floor(i / C.PER) * (C.ICON + C.IGAP);
        picAt(x2, ix, iy, C.ICON);
        const v = M.units[x2], mark = x2 === sel ? INK.sel : v.upgrade ? INK.up : v.upgrades.length ? INK.up : null;
        if (mark) {
          const [mx, my, mw, mh] = box(ix, iy, C.ICON, C.ICON), ml = Math.max(1, Math.round((x2 === sel ? 1.2 : 0.7) * z));
          g.strokeStyle = mark; g.lineWidth = ml;
          if (v.upgrade && x2 !== sel) g.setLineDash([Math.max(2, Math.round(2 * z)), Math.max(1, Math.round(1.5 * z))]);
          g.strokeRect(mx + ml / 2, my + ml / 2, mw - ml, mh - ml);
          g.setLineDash([]);
        }
      });
      const more = u.builds.length - u.unlocks.length;
      g.fillStyle = INK.muted;
      clip(`${u.unlocks.length} new${more ? ` · ${more} also built elsewhere` : ''}${u.upgrades.length ? ` · ${u.upgrades.length} upgrade${u.upgrades.length > 1 ? 's' : ''}` : ''}`,
        x + C.PAD, y + hh - 2.5, w - 2 * C.PAD, 'label', 5);
    }
    g.restore();
  }

  return { model, layout, fit, hit, draw, ground, pathTo, home, cardIcons, isUpgrade, KINDS, C, MARGIN, INK };
})();
if (typeof module !== 'undefined') module.exports = WikiTech;
