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
     `o`: {cam, s (canvas pixels a unit), sel (the selected unit or null), all (every builder's edge, not only the
     walk's), pic (key -> a loaded image or null), font (role -> CSS font at 1 unit, scaled here)}. The selected unit's
     card, the walk to it and its card's children and parents are lit; every other card is dimmed. */
  const INK = { bg: '#2a2c25', head: '#33352d', rim: '#56594c', text: '#e2e2da', muted: '#9a9d8e',
    edge: '#5b6050', also: '#6a7058', path: '#7fc4e8', kid: '#9fd68a', sel: '#e8c66a', up: '#c98ae0', pic: '#000' };
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
    // the edges first, under the cards: the walk's (each card to its parent card) and, lit or with `all`, every other
    // builder of a card's unit
    const edge = (a, b, style, w) => {
      const [ax, ay, aw] = L.pos[a], [bx, by] = L.pos[b], mx = (ax + aw + bx) / 2;
      g.strokeStyle = style; g.lineWidth = Math.max(1, Math.round(w * z));
      g.beginPath();
      // a line one pixel wide sits on a pixel's centre
      const o = g.lineWidth % 2 ? 0.5 : 0;
      g.moveTo(X(ax + aw), Y(ay + C.EDGE_Y) + o); g.lineTo(X(mx) + o, Y(ay + C.EDGE_Y) + o);
      g.lineTo(X(mx) + o, Y(by + C.EDGE_Y) + o); g.lineTo(X(bx), Y(by + C.EDGE_Y) + o);
      g.stroke();
    };
    const lines = [];
    for (const k of M.spine) {
      if (M.sp[k]) {
        const path = onPath.has(k) && onPath.has(M.sp[k]), kid = M.sp[k] === h || (k === h && pars.has(M.sp[k]));
        lines.push([M.sp[k], k, path ? INK.path : kid ? INK.kid : INK.edge, path ? 1.5 : 1, path || kid ? 2 : focus ? 0 : 1]);
      }
      for (const p of M.units[k].parents) {
        if (p === M.sp[k] || !M.sp.hasOwnProperty(p)) continue;
        const kid = p === h || (k === h && pars.has(p));
        if (kid || o.all) lines.push([p, k, kid ? INK.kid : INK.also, 1, kid ? 2 : 0]);
      }
    }
    lines.sort((a, b) => a[4] - b[4]);
    for (const [a, b, st, w, rank] of lines) { g.globalAlpha = focus && !rank ? 0.35 : 1; edge(a, b, st, w); }
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
    g.textBaseline = 'alphabetic';
    for (const k of M.spine) {
      const [x, y, w, hh] = L.pos[k], u = M.units[k];
      if (X(x + w) < 0 || Y(y + hh) < 0 || X(x) > g.canvas.width || Y(y) > g.canvas.height) continue;
      g.globalAlpha = focus && !lit(k) ? 0.3 : 1;
      const [cx, cy, cw, ch] = box(x, y, w, hh);
      g.fillStyle = INK.bg; g.fillRect(cx, cy, cw, ch);
      g.fillStyle = INK.head; g.fillRect(...box(x, y, w, C.HEAD));
      const rim = k === h ? INK.sel : onPath.has(k) ? INK.path : kids.has(k) || pars.has(k) ? INK.kid : INK.rim;
      const lw = Math.max(1, Math.round((k === h ? 1.5 : 0.75) * z)), ho = lw / 2;
      g.strokeStyle = rim; g.lineWidth = lw;
      g.strokeRect(cx + ho, cy + ho, cw - lw, ch - lw);
      picAt(k, x + C.PAD, y + C.PAD, C.PIC);
      const tx = x + C.PAD * 2 + C.PIC, tw = w - (tx - x) - C.PAD;
      g.fillStyle = INK.text; clip(u.title, tx, y + 10, tw, 'title', 8);
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

  return { model, layout, fit, hit, draw, pathTo, home, cardIcons, isUpgrade, KINDS, C, MARGIN };
})();
if (typeof module !== 'undefined') module.exports = WikiTech;
