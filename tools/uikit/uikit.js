/* uikit.js -- what every mockup of an Impure window shares: loading a kit (art/<window>/kit/), painting its parts on the
   page's [data-part] boxes, and setting the window's layout numbers and text roles as CSS. The C half is
   tagpu/ddraw/src/tagpu_uikit.c, which a window's check pairs against the page these rules paint. */
const UIKit = (() => {
  /* a part's face is light when its middle (inside the slices) averages above a grey of 120: its text is then dark. 120 is
     where dark ink (#1d1d1b) out-contrasts the HUD styles' light colours (about #e2dcb0) by more than the light text's
     shadow makes up, put at 15 % */
  function faceLight(k, im) {
    const w = im.naturalWidth, h = im.naturalHeight, [t, r, b, l] = k.slice || [h / 4, w / 4, h / 4, w / 4];
    const c = document.createElement('canvas'); c.width = c.height = 8;
    const g = c.getContext('2d'); g.drawImage(im, l, t, Math.max(1, w - l - r), Math.max(1, h - t - b), 0, 0, 8, 8);
    const d = g.getImageData(0, 0, 8, 8).data; let s = 0, n = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i + 3] > 128) { s += .3 * d[i] + .59 * d[i + 1] + .11 * d[i + 2]; n++; }
    return n > 0 && s / n > 120;
  }
  /* The kit at folder `base`, its parts decoded: { parts, img, light }. */
  async function loadKit(base) {
    const j = await fetch(base + 'kit.json').then(r => { if (!r.ok) throw new Error(`${base}kit.json: ${r.status}`); return r.json(); });
    const parts = j.parts, img = {}, light = {};
    await Promise.all(Object.keys(parts).map(n => { const im = new Image(); im.src = `${base}parts/${n}.png`; img[n] = im; return im.decode(); }));
    for (const n in parts) light[n] = faceLight(parts[n], img[n]);
    return { parts, img, light };
  }
  /* A part drawn into a box of W x H pixels, px pixels a unit, by hud.js's rules: a nine-slice's edges kept at their size
     in units (whole pixels, floored, never under one) unless the box is smaller than two opposite edges, when all four
     shrink by one factor, its middle stretched; an xtile part's middle column one period of a seamless tile, repeated a
     whole number of times at about its painted length (min 1), each row at its edge's scale; a whole part over the box;
     an icon as large as fits, centred. Sampled bilinearly by the canvas, not texel for texel as hud.js's paint() does. */
  const copies = (len, period) => period > 0 ? Math.max(1, Math.round(len / period)) : 1;
  function drawPart(g, k, im, W, H, px) {
    if (k.kind === 'icon') return drawIcon(g, im, W, H, 1);
    if (k.kind !== 'nine') return g.drawImage(im, 0, 0, W, H);
    const pw = im.naturalWidth, ph = im.naturalHeight, [st, sr, sb, sl] = k.slice, mw = pw - sr - sl;
    const edge = v => v >= 1 ? Math.floor(v) : v > 0 ? 1 : 0;
    let t = st / k.scale * px, r = sr / k.scale * px, b = sb / k.scale * px, l = sl / k.scale * px, f = 1;
    if (l + r > 0) f = Math.min(f, W / (l + r));
    if (t + b > 0) f = Math.min(f, H / (t + b));
    t = edge(t * f); r = edge(r * f); b = edge(b * f); l = edge(l * f);
    const dx = [0, l, W - r, W], dy = [0, t, H - b, H], sx = [0, sl, pw - sr, pw], sy = [0, st, ph - sb, ph];
    let m = [1, 1, 1];
    if (k.xtile) {
      const ts = st > 0 && t > 0 ? t / st : 0, bs = sb > 0 && b > 0 ? b / sb : 0, ms = ts > 0 ? ts : bs > 0 ? bs : px * f / k.scale;
      m = [ts > 0 ? ts : ms, ms, bs > 0 ? bs : ms].map(sc => copies(dx[2] - dx[1], mw * sc));
    }
    for (let j = 0; j < 3; j++) for (let i = 0; i < 3; i++) {
      if (!(dx[i + 1] > dx[i] && dy[j + 1] > dy[j] && sx[i + 1] > sx[i] && sy[j + 1] > sy[j])) continue;
      const n = i === 1 ? m[j] : 1;
      for (let e = 0; e < n; e++) {
        const x0 = dx[i] + Math.round((dx[i + 1] - dx[i]) * e / n), x1 = dx[i] + Math.round((dx[i + 1] - dx[i]) * (e + 1) / n);
        g.drawImage(im, sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j], x0, dy[j], x1 - x0, dy[j + 1] - dy[j]);
      }
    }
  }
  function drawIcon(g, im, W, H, f) {
    const s = Math.min(W / im.naturalWidth, H / im.naturalHeight) * f, w = im.naturalWidth * s, h = im.naturalHeight * s;
    g.drawImage(im, (W - w) / 2, (H - h) / 2, w, h);
  }
  /* Every [data-part] box painted with loaded kit K's part at the resolution's pixels (px a unit, k device pixels a CSS
     pixel), again only when its part, its glyph or its size changes. A pressed or active box (.on, .sel, .pressed) shows
     its _down part, a selected row row_selected; a box with data-glyph gets the first of its icons the kit has, unless
     its part is a whole one with the glyph painted on, and a tree row's mark is an icon part of its own: no HUD face has
     these characters, so tools/wiki/mockup/glyphs.js draws them all as kit icons. The box's text ink follows its face:
     dark on a light face, the style's own colour on a dark one. `skin` names the kit in each box's cache key. */
  function paintParts(K, skin, px, k) {
    for (const el of document.querySelectorAll('[data-part]')) {
      const w = el.offsetWidth, h = el.offsetHeight; if (!w || !h) continue;
      const base = el.dataset.part, alt = base === 'row' ? 'row_selected' : base + '_down';
      const n = el.matches('.on, .sel, .pressed') && K.parts[alt] ? alt : base, part = K.parts[n]; if (!part) continue;
      const gl = (el.dataset.glyph || '').split(' ').find(q => K.parts[q]);
      const W = Math.max(1, Math.round(w * k)), H = Math.max(1, Math.round(h * k)), key = `${skin}|${n}|${gl}|${W}|${H}`;
      let cv = el.firstElementChild;
      if (!cv || !cv.classList.contains('skin')) { cv = document.createElement('canvas'); cv.className = 'skin'; el.prepend(cv); el._pk = null; }
      if (el._pk === key) continue;
      el._pk = key; cv.width = W; cv.height = H;
      const g = cv.getContext('2d'); g.imageSmoothingQuality = 'high';
      drawPart(g, part, K.img[n], W, H, px);
      if (gl && part.kind !== 'whole') drawIcon(g, K.img[gl], W, H, .6);
      el.style.setProperty('--ink', K.light[n] ? '#1d1d1b' : 'initial');
      el.style.setProperty('--shade', K.light[n] ? 'none' : 'initial');
    }
  }
  /* drop every painted part, for a change of skin: the boxes are painted anew */
  function dropParts() {
    for (const el of document.querySelectorAll('[data-part]')) {
      const cv = el.firstElementChild; if (cv && cv.classList.contains('skin')) cv.remove();
      el._pk = null; el.style.removeProperty('--ink'); el.style.removeProperty('--shade');
    }
  }
  /* a button holds its _down part while the pointer is down on it */
  document.addEventListener('pointerdown', e => { const b = e.target.closest('button[data-part]'); if (b) b.classList.add('pressed'); });
  window.addEventListener('pointerup', () => { for (const b of document.querySelectorAll('.pressed')) b.classList.remove('pressed'); });

  /* A layout's numbers as custom properties of the frame, which the mockup's CSS reads: `--w-` and the number's path, a
     list's items by their index (row.pad -> --w-row-pad-0, --w-row-pad-1). Its geom and roles are not lengths. */
  function layoutVars(frame, lay) {
    (function vars(o, path) {
      for (const [k, v] of Object.entries(o)) {
        const p = [...path, k];
        if (typeof v === 'number') frame.style.setProperty('--w-' + p.join('-'), v);
        else if (v && typeof v === 'object' && !(path.length === 0 && (k === 'geom' || k === 'roles'))) vars(v, p);
      }
    })(lay, []);
  }
  /* Each text role in the replay HUD's style nearest it (art/hud/layout.json `styles`: face, weight, size in units, colour,
     letter spacing, case, shadow or outline), the layout's `roles` naming the style and `selectors` giving each role's
     elements. The colour and the shadow give way to a face's own ink (--ink, --shade) where the skin's face is light. */
  function roleStyles(selectors, roles, styles) {
    const U = n => `calc(${n} * var(--u))`;
    const css = Object.entries(selectors).map(([n, sel]) => {
      const s = styles[roles[n]];
      const shade = s.outline ? (([a, b, c, d]) => [[a, b], [c, d], [a, d], [c, b]].map(([x, y]) => `${U(x)} ${U(y)} 0 #000`).join(','))(s.outline)
        : s.shadow ? `0 ${U(s.shadow)} 0 rgba(0,0,0,.8)` : 'none';
      return `${sel.join(', ')} { font-family: '${s.font}', sans-serif; font-weight: ${s.weight}; font-size: ${U(s.size)}; font-style: normal;`
        + ` letter-spacing: ${s.spacing}em; text-transform: ${s.upper ? 'uppercase' : 'none'}; color: var(--ink, ${s.colour});`
        + ` text-shadow: var(--shade, ${shade}); }`;
    }).join('\n');
    const st = document.createElement('style'); st.textContent = css; document.head.append(st);
  }
  return { loadKit, paintParts, dropParts, layoutVars, roleStyles };
})();
