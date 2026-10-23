/* hud.js -- draws the replay HUD from art/hud/layout.json and art/hud/kit.json.

   Every number comes from those two files; this file holds the rules that turn them into rectangles, and
   research/notes/hud-layout.md writes the same rules down for the DLL, which is to implement them over the
   same files. A change to the HUD's look or geometry is a change to the data, not to this file.

   Coordinates are HUD units: one unit is the screen height / 720 x the replay HUD's own scale, which does not
   follow the game's. The host gives the engine's frame (st.frame: the side panel's width, the bars' heights)
   in units; the dock and the column anchor to it and stretch over what it leaves. A part's children are
   placed against its FRAME: for a grid-sliced part, its painted design box and measured wells, mapped to
   the drawn size through its stretch bands; for any other box, the box itself. */
const HUD = (() => {
  let L, K, root;
  const IMG = {}, cache = {};

  async function load(base) {
    root = base;
    [L, K] = await Promise.all([fetch(base + 'layout.json').then(r => r.json()), fetch(base + 'kit.json').then(r => r.json()).then(j => j.parts)]);
    await Promise.all(Object.keys(K).filter(n => ['grid', 'nine', 'tile'].includes(K[n].kind)).map(n => {
      const im = new Image(); im.src = url(n); IMG[n] = im; return im.decode();
    }));
    return { layout: L, kit: K };
  }
  const url = n => `${root}parts/${n}.png`;

  /* ---- the band map: a grid-sliced part drawn at T for design length D. The fixed bands keep their size and the
     stretch bands share the difference; drawn smaller than its fixed bands, the whole part scales. ---- */
  function bandMap(bands, D, T) {
    const S = bands.reduce((a, [p, q]) => a + q - p, 0), k = S > 0 && T >= D - S ? (T - (D - S)) / S : null;
    return x => {
      if (k === null) return x * T / D;
      let y = 0, last = 0;
      for (const [p, q] of bands) {
        if (x <= p) return y + x - last;
        y += p - last;
        if (x <= q) return y + (x - p) * k;
        y += (q - p) * k; last = q;
      }
      return y + x - last;
    };
  }
  function frame(w, h, part) {
    const k = part && K[part];
    if (k && k.kind === 'grid') return { w, h, fx: bandMap(k.xs, k.design[0], w), fy: bandMap(k.ys, k.design[1], h), D: k.design, wells: k.wells };
    return { w, h, fx: x => x, fy: y => y, D: [w, h], wells: {} };
  }
  // the design extent [lo, hi] along one axis of a well, of the union of several, or of the whole frame
  function extent(F, ws, a) {
    if (ws == null) return [0, F.D[a]];
    const list = (Array.isArray(ws) ? ws : [ws]).map(n => { const w = F.wells[n]; if (!w) throw new Error(`no well ${n}`); return w; });
    return [Math.min(...list.map(w => w[a])), Math.max(...list.map(w => w[a] + w[a + 2]))];
  }
  /* THE PLACEMENT RULE. s.in: a well, a list of wells (their union), or {x, y} naming each axis apart; none = the
     whole frame. s.pad [l, t, r, b] moves each edge inward in design units (negative: outward); the edges are then
     mapped to the drawn size. s.padf insets the drawn box by that fraction of its size. s.w / s.h fix a drawn size,
     aligned inside the drawn box by s.ax (l, c, r) and s.ay (t, c, b), centred by default. */
  function place(F, s = {}) {
    const ix = s.in && !Array.isArray(s.in) && typeof s.in === 'object' ? s.in.x : s.in;
    const iy = s.in && !Array.isArray(s.in) && typeof s.in === 'object' ? s.in.y : s.in;
    let [l, r] = extent(F, ix, 0), [t, b] = extent(F, iy, 1);
    const p = s.pad || [0, 0, 0, 0];
    let x0 = F.fx(l + p[0]), x1 = F.fx(r - p[2]), y0 = F.fy(t + p[1]), y1 = F.fy(b - p[3]);
    if (s.padf) { const dx = (x1 - x0) * s.padf, dy = (y1 - y0) * s.padf; x0 += dx; x1 -= dx; y0 += dy; y1 -= dy; }
    if (s.w != null) { x0 = s.ax === 'l' ? x0 : s.ax === 'r' ? x1 - s.w : (x0 + x1 - s.w) / 2; x1 = x0 + s.w; }
    if (s.h != null) { y0 = s.ay === 't' ? y0 : s.ay === 'b' ? y1 - s.h : (y0 + y1 - s.h) / 2; y1 = y0 + s.h; }
    return { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
  }

  /* ---- elements ---- */
  const U = n => `calc(${+n.toFixed(4)} * var(--u))`;
  const rect = b => `left:${U(b.x)};top:${U(b.y)};width:${U(b.w)};height:${U(b.h)}`;
  // a fill's width: one that has begun spans at least one device pixel, so it always shows
  // (tagpu_hudlay_clip, the DLL's same floor)
  const UF = w => w > 0 ? `max(${U(w)}, ${+(1 / (window.devicePixelRatio || 1)).toFixed(4)}px)` : U(0);
  const frect = b => `left:${U(b.x)};top:${U(b.y)};width:${UF(b.w)};height:${U(b.h)}`;
  const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;');
  const WB_GLYPH = { export: '⇩', delete: '✕', rewind: '⟲' };
  const attrs = a => Object.entries(a || {}).map(([k, v]) => ` data-${k}="${esc(v)}"`).join('');
  // a part at box b; `cls` adds classes (hot, pressed, off, face), `data` adds data- attributes
  function art(part, b, inner = '', cls = '', data, style = '') {
    const k = K[part];
    // a part the kit lacks is drawn as whitebox; an icon it lacks, as its stand-in glyph
    if (!k && part.startsWith('i_')) return `<div class="art noicon ${cls}" data-wb="${esc(WB_GLYPH[part.slice(2)] || part.slice(2))}" style="${rect(b)};--wbs:${+(b.h * .9).toFixed(3)}${style}"${attrs(data)}></div>`;
    if (!k) return `<div class="art noart ${cls}" style="${rect(b)}${style}"${attrs(data)}>${inner}</div>`;
    if (k.kind === 'grid' || k.kind === 'nine' || k.kind === 'tile')
      return `<div class="art composed ${cls}" style="${rect(b)}${style}" data-${k.kind === 'tile' ? 'tile' : 'gp'}="${part}"${attrs(data)}>${inner}</div>`;
    return `<div class="art ${k.kind} ${cls}" data-part="${part}" style="${rect(b)};${vars(part)}${style}"${attrs(data)}>${inner}</div>`;
  }
  // the custom property that draws a whole or icon part
  const vars = part => `--bi:url('${url(part)}')`;
  // a held button shows its pressed part, and its face sinks; released, it shows its own part again
  function press(el, down) {
    const part = down ? el.dataset.down : el.dataset.up;
    if (part && !K[part]) { el.classList.toggle('pressed', down); return; }
    if (!part) return;
    if (el.dataset.gp) { el.dataset.gp = part; composeOne(el); }
    else el.style.setProperty('--bi', `url('${url(part)}')`);
    el.classList.toggle('pressed', down);
  }
  const icon = (name, b, cls = '') => art('i_' + name, b, '', cls);
  // text in style `st`: its size shrinks to the box's height / s.fit when the box is lower than the style's line
  /* THE LINE AS THE DLL LAYS IT OUT. The text is drawn unhinted (hud.css), and Blink, which then positions glyphs by
     subpixel, lays the line out on other metrics: a descent that rounds down borrows a pixel from the ascent (canvas
     textRendering shows the two sets), which puts the baseline a pixel higher at about half the sizes. The text box
     is padded by what moves the line back to where the default metrics put it, the ones tagpu_hudlay_baseline
     follows: CSS pixels the line moves down, from the face, its weight and its size in CSS pixels. */
  const shifts = {};
  function lineShift(font, weight, px) {
    const key = `${weight} ${px}px '${font}'`;
    if (!(key in shifts)) {
      const off = tr => {
        const c = document.createElement('canvas').getContext('2d');
        c.textRendering = tr;
        c.font = key;
        const m = c.measureText('M');
        return m.fontBoundingBoxAscent + Math.floor((px - m.fontBoundingBoxAscent - m.fontBoundingBoxDescent) / 2);
      };
      shifts[key] = off('auto') - off('geometricPrecision');
    }
    return shifts[key];
  }
  let cssUnit = 1;      // CSS pixels a unit, the stage's --u, as draw() found it
  function text(html, b, st, s = {}, colour, data) {
    const S = L.styles[st], size = s.fit ? Math.min(S.size, b.h / s.fit) : S.size;
    const dy = lineShift(S.font, S.weight, size * cssUnit);
    // padding one side by twice the shift moves the centred line by it; the box and its clip stay
    const pad = dy ? `;box-sizing:border-box;padding-${dy > 0 ? 'top' : 'bottom'}:${2 * Math.abs(dy)}px` : '';
    const shadow = S.outline ? (([px, py, nx, ny]) => [[px, py], [nx, ny], [px, ny], [nx, py]].map(([x, y]) => `${U(x)} ${U(y)} 0 #000`).join(','))(S.outline)
      : `0 ${U(S.shadow)} 0 rgba(0,0,0,.8)`;
    return `<div class="t${s.align === 'c' ? ' c' : s.align === 'r' ? ' r' : ''}${s.face ? ' face' : ''}"${attrs(data)} style="${rect(b)}${pad};font:${S.weight} ${U(size)}/1 '${S.font}',sans-serif;`
      + `color:${colour || S.colour};text-transform:${S.upper ? 'uppercase' : 'none'};letter-spacing:${S.spacing}em;text-shadow:${shadow}">${html}</div>`;
  }
  const signed = (a, b) => { const g = L.signs; return `<b style="color:${g.pos};font-weight:${g.weight}">+${a}</b>&nbsp;/&nbsp;<b style="color:${g.neg};font-weight:${g.weight}">−${b}</b>`; };
  const k1 = n => Math.round(n).toLocaleString('en');
  // a row's rate: tenths below 100, as the game's incomes are fractional, whole numbers from there
  const rate = n => String(Math.abs(n) < 100 ? Math.round(n * 10) / 10 : Math.round(n));
  // the header's sums: thousands in K from 10 000 on, so a ten-player team's income still fits its recess
  const kK = n => n >= 10000 ? (n / 1000).toFixed(0) + 'K' : k1(n);
  const clock = t => { t = Math.max(0, Math.round(t)); return `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}`; };
  const zero = (w, h) => ({ x: 0, y: 0, w, h });

  // a button: its part, pressed while held (and while on, for a toggle), its icon and text placed on its face
  function button(spec, kind, b, st, on) {
    const F = frame(b.w, b.h);
    let inner = '';
    if (spec.icon) { const n = spec.icon.playing && st.playing ? spec.icon.playing : spec.icon.name; inner += icon(n, place(F, spec.icon), 'face'); }
    if (spec.text) inner += text(spec.text.html, place(F, spec.text), spec.text.style, { ...spec.text, face: true });
    return art(on ? kind.pressed : kind.part, b, inner, `hot${on ? ' pressed' : ''}${spec.off ? ' off' : ''}`,
      { act: spec.act, up: kind.part, down: kind.pressed, ...(on ? { on: 1 } : {}), ...(spec.data || {}) }, spec.off ? `;opacity:${spec.off}` : '');
  }

  /* ---- the dock: the transport plate from the side panel's edge, the timeline housing to the screen's right ---- */
  function dock(st) {
    const D = L.dock, T = D.transport, R = D.rail, y = st.H - st.frame.bottom - D.h;
    const tb = { x: st.frame.panel + T.l, y, w: T.w, h: D.h }, F = frame(T.w, D.h, T.part);
    const bound = { speed: `${st.speed}×`, hud: `${Math.round(st.hud * 100)}%` };
    // the transport plate and the timeline housing are the HUD's whole: a click between their buttons is taken too
    let h = `<div class="box" data-act="${D.act}" style="${rect(tb)}">` + art(T.part, zero(T.w, D.h)) + T.buttons.map(s => button(
      { ...s, text: s.text && { ...s.text, html: bound[s.text.bind] } }, T.button, place(F, { in: s.in, pad: T.button.pad }), st)).join('') + `</div>`;
    // proposed: with forks the housing is the one painted with the Forks bay's pocket at its left end
    const part = st.forks ? L.proposed.forks.rail : R.part;
    const rx = tb.x + T.w + R.gap, rw = st.W - R.r - rx, FR = frame(rw, D.h, part);
    const lane = place(FR, R.markers), tr = place(FR, R.track), p = st.t / st.T, pf = filled(st), M = R.markers, B = M.badge;
    const along = (w, e, f) => e + (w - 2 * e) * f;
    const bay = st.forks ? forksBay(st, FR) : '';
    let marks = st.markers.map(m => {
      const cx = along(lane.w, M.ends, m.t / st.T), s = lane.h, bb = { x: cx - s / 2, y: 0, w: s, h: s };
      const col = m.dot || (m.seat < 0 ? M.bookmark_dot : st.seats[m.seat].colour);
      return `<div class="box${m.act ? ' hot' : ''}"${m.act ? ` data-act="${m.act}"` : ''} style="${rect(bb)}"><div class="box" style="${rect({ x: B.dot, y: B.dot, w: s - 2 * B.dot, h: s - 2 * B.dot })};border-radius:50%;background:${col}"></div>`
        + art(B.part, zero(s, s)) + icon(M.icons[m.kind] || m.kind, { x: B.icon, y: B.icon, w: s - 2 * B.icon, h: s - 2 * B.icon }, 'on_dark') + `</div>`;
    }).join('');
    const fp = R.track.fill.pad, fb = { x: fp[0], y: fp[1], w: tr.w - fp[0] - fp[2], h: tr.h - fp[1] - fp[3] };
    const ph = R.track.playhead, phb = place(frame(tr.w, tr.h), { pad: ph.pad }), phx = along(tr.w, R.track.ends, p);
    h += `<div class="box" data-act="${D.act}" style="${rect({ x: rx, y, w: rw, h: D.h })}">` + art(part, zero(rw, D.h))
      + `<div class="box" style="${rect(lane)}">${marks}</div>`
      + `<div class="box" data-track data-ends="${R.track.ends}" style="${rect(tr)}">`
      + `<div class="clip" data-fill="${fb.w}" style="${frect({ ...fb, w: fb.w * pf })}">${art(R.track.fill.part, zero(fb.w, fb.h))}</div>`
      + (st.ends < st.T ? after(fb, along(tr.w, R.track.ends, st.ends / st.T) - fb.x) : '')
      + art(ph.part, { x: phx - ph.w / 2, y: phb.y, w: ph.w, h: phb.h }, '', '', { playhead: `${tr.w} ${R.track.ends} ${ph.w}` }) + `</div>`
      + text(`${clock(st.t)} / ${clock(st.len ?? st.T)}`, place(FR, R.time), R.time.style, R.time, null, { time: 1 })
      + bay + `</div>`;
    if (st.forks) h += lanes(st, { x: rx, y, w: rw, tx: rx + tr.x, ty: y + tr.y, tw: tr.w, ends: R.track.ends }) + takeover(st, tb);
    return h;
  }

  // the share of the track filled: to the playhead, or to where the timeline watched branched off it (st.fillTo)
  const filled = st => Math.min(st.t, st.fillTo ?? st.T) / st.T;
  // the groove past the replay's end, when a fork runs longer than the replay: shaded, the replay has nothing there
  function after(fb, x) {
    const A = L.proposed.forks.after, n = parseInt(A.colour.slice(1), 16);
    return `<div class="box" style="${rect({ x: fb.x + x, y: fb.y, w: fb.w - x, h: fb.h })};background:rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${A.alpha})"></div>`;
  }

  /* ---- proposed, variation B (layout.json's "proposed"): forks as lanes over the timeline housing, oldest branch on top.
     A lane runs from its branch to its end on the track's own time axis, widened to its minimum to the right first, and
     the playhead's line runs up through every lane. The chain that plays is filled along it, as the track is filled to
     where the chain leaves the replay: the watched lane to the playhead, and each lane before it on the chain (f.chain)
     to where the next one on the chain branches off it, so the path the playing took has no gap. A line from each
     lane's branch says where its history before the branch is: a fork whose parent is a lane drawn above it rises to
     that lane; every other fork, of the replay or of a deleted fork, drops to the track. The parent is always above,
     since a fork branches after its parent does; the line is held to the parent's lane, which a lane clamped at the
     track's right end may not reach. Clicking a lane watches that timeline, clicking the watched one goes back to the
     replay. At the fork limit, while the picker is open, the lane a take-over would replace is outlined (st.replace).
     The lanes show only while the Forks bay is open (st.lanesOpen). The game draws them by the same rules
     (tagpu_hudlay.c, lanes). g: the housing's box and its track's span, in units ---- */
  /* the Forks bay at the housing's left end opens and closes the lanes and counts the forks; it sits in the pocket
     painted for it in the housing (FR, the housing's frame), which spans the marker lane and the track */
  function forksBay(st, FR) {
    const B = L.proposed.forks.bay, n = st.forks.length, open = st.lanesOpen && n > 0, b = place(FR, B);
    return button({ act: 'lanes', icon: { ...B.icon, name: open ? B.icon.open : B.icon.closed }, off: n ? 0 : B.off }, B, b, st, open)
      + text(String(n), { x: b.x, y: b.y + b.h * B.count.y, w: b.w, h: b.h * (1 - B.count.y) }, B.count.style, { align: 'c' });
  }

  // the lane plate's height with its gap above the dock: the column stands on it, since the lanes run under the column
  function lanesH(st) {
    const P = L.proposed && L.proposed.forks, n = st.forks ? st.forks.length : 0;
    return n && st.lanesOpen ? P.plate.gap + P.plate.pad[1] + n * P.lane.h + (n - 1) * P.lane.gap + P.plate.pad[3] : 0;
  }
  function lanes(st, g) {
    const P = L.proposed.forks, Ln = P.lane, Bt = Ln.buttons, n = st.forks.length, pad = P.plate.pad;
    if (!n || !st.lanesOpen) return '';
    const py = g.y - lanesH(st), ph = lanesH(st) - P.plate.gap;
    const X = t => g.tx + g.ends + (g.tw - 2 * g.ends) * t / st.T, lo = g.tx + g.ends, hi = g.tx + g.tw - g.ends;
    let h = art(P.plate.part, { x: g.x, y: py, w: g.w, h: ph }, '', '', { act: 'dock' }), lines = '';
    const span = [];
    st.forks.forEach((f, k) => {
      const ly = py + pad[1] + k * (Ln.h + Ln.gap), bx = X(f.from);
      let x0 = bx, x1 = Math.max(X(f.end), bx + Ln.min_w);
      if (x1 > hi) { x0 = Math.max(lo, x0 - (x1 - hi)); x1 = hi; }
      span.push([x0, x1, ly]);
      const w = x1 - x0, on = st.timeline === k, F = frame(w, Ln.h), nb = Bt.items.length;
      const bw = Bt.w, btx = w - Bt.pad - nb * bw - (nb - 1) * Bt.gap;
      const fp = Ln.fill.pad, fw = w - fp[0] - fp[2], fx = bx - x0;
      // the watched lane fills to the playhead, a lane before it on the chain to where the next one on the chain branches off it
      const next = on ? null : st.forks.find(q => q.chain && q.parent === f.id), stop = next ? X(next.from) : X(f.end);
      let inner = on || f.chain ? `<div class="clip" data-lanefill="${bx} ${stop}" style="${frect({ x: fp[0] + fx, y: fp[1], w: Math.max(0, Math.min(X(st.t), stop) - bx), h: Ln.h - fp[1] - fp[3] })}">`
        + art(Ln.fill.part, zero(fw, Ln.h - fp[1] - fp[3])) + `</div>` : '';
      inner += text(`F${f.id} · ${esc(f.name)} from ${clock(f.from)}`, place(F, { pad: [fx + Ln.label.pad[0], 0, w - btx + Ln.label.pad[0], 0] }), Ln.label.style, Ln.label);
      inner += Bt.items.map((it, j) => button({ act: it.act, icon: { name: it.icon, padf: Bt.icon.padf }, data: { fork: k } }, Bt,
        { x: btx + j * (bw + Bt.gap), y: Bt.pad, w: bw, h: Ln.h - 2 * Bt.pad }, st, st.armed === k && it.act === 'delete')).join('');
      h += art(on ? Ln.pressed : Ln.part, { x: x0, y: ly, w, h: Ln.h }, inner, `hot${on ? ' pressed' : ''}`, { act: 'lane', fork: k, up: Ln.part, down: Ln.pressed, ...(on ? { on: 1 } : {}) });
      if (st.replace === k) { const R = P.replace;
        h += `<div class="box" style="${rect({ x: x0 - R.width, y: ly - R.width, w: w + 2 * R.width, h: Ln.h + 2 * R.width })};box-shadow:inset 0 0 0 ${U(R.width)} ${R.colour};pointer-events:none"></div>`; }
      const B = P.branch, up = st.forks.findIndex(q => q.id === f.parent);
      if (up >= 0 && up < k) { const [ux0, ux1, uy] = span[up], x = Math.min(Math.max(bx, ux0), ux1);
        lines += `<div class="box" style="${rect({ x: x - B.w / 2, y: uy + Ln.h, w: B.w, h: ly - uy - Ln.h })};background:${B.colour};pointer-events:none"></div>`; }
      else lines += `<div class="box" style="${rect({ x: bx - B.w / 2, y: ly + Ln.h, w: B.w, h: g.ty - ly - Ln.h })};background:${B.colour};pointer-events:none"></div>`;
    });
    const Hd = P.playhead;
    return h + lines + `<div class="box" data-lanehead="${g.tx + g.ends} ${g.tw - 2 * g.ends} ${Hd.w}" style="${rect({ x: X(st.t) - Hd.w / 2, y: py, w: Hd.w, h: g.ty - py })};background:${Hd.colour};pointer-events:none"></div>`;
  }

  // Take over at the playhead: a button on the transport plate, as wide as it, level with the lanes; it opens the picker
  function takeover(st, tb) {
    const T = L.proposed.forks.takeover, b = { x: tb.x, y: tb.y - T.gap - T.h, w: tb.w, h: T.h };
    return button({ act: 'takeover', text: { ...T.text, html: T.label } }, T, b, st, st.picker);
  }

  /* the player picker, opened by Take over: every player, the one taken over is played by you from the playhead and the
     others by the computer; a player defeated by then cannot be taken. At the fork limit it says which fork goes */
  function picker(st) {
    const Pk = L.proposed.picker, R = Pk.row, D = L.dock, T = L.proposed.forks.takeover, full = st.forks.length >= L.proposed.forks.max;
    const ty = st.H - st.frame.bottom - D.h - T.gap - T.h, ps = st.seats, wn = full ? 2 : 0;
    const h = Pk.pad + Pk.title.h + ps.length * (R.h + R.gap) + (wn ? Pk.warn.pad[1] + wn * Pk.warn.line + Pk.warn.pad[3] : 0) + Pk.pad;
    const b = { x: st.frame.panel + D.transport.l, y: ty - Pk.gap - h, w: Pk.w, h };
    let inner = text(`Take over at&nbsp;<span data-tot>${clock(st.t)}</span>&nbsp;as`, { x: Pk.pad + Pk.title.pad[0], y: Pk.pad, w: Pk.w - 2 * Pk.pad - Pk.title.pad[0] - Pk.title.pad[2], h: Pk.title.h }, Pk.title.style);
    ps.forEach((p, k) => {
      const y = Pk.pad + Pk.title.h + k * (R.h + R.gap), rb = { x: Pk.pad, y, w: Pk.w - 2 * Pk.pad, h: R.h }, F = frame(rb.w, R.h);
      // a player out of the game cannot be taken: the column's dim rule, the one the game's takeover refuses by too
      // (no unit left at the picker's start)
      const gone = p.out;
      const row = `<div class="box" style="${rect(place(F, { pad: [R.stripe.pad, R.stripe.pad, 0, R.stripe.pad], w: R.stripe.w, ax: 'l' }))};background:${p.colour}"></div>`
        + text(esc(p.name), place(F, { pad: R.name.pad }), R.name.style, R.name) + (gone ? text('defeated', place(F, { pad: R.note.pad }), R.note.style, { align: 'r' }) : '');
      inner += art(R.part, rb, row, gone ? 'off' : 'hot', { act: 'takeas', seat: p.i, up: R.part, down: R.pressed }, gone ? `;opacity:${R.off}` : '');
    });
    if (wn) { const Wr = Pk.warn, y0 = Pk.pad + Pk.title.h + ps.length * (R.h + R.gap) + Wr.pad[1], r = st.forks[st.replace] || st.forks[0];
      [`${L.proposed.forks.max} of ${L.proposed.forks.max} forks kept: this one`, `replaces F${r.id}, the one watched least recently`].forEach((s, k) =>
        inner += text(s, { x: Pk.pad + Wr.pad[0], y: y0 + k * Wr.line, w: Pk.w - 2 * Pk.pad - Wr.pad[0] - Wr.pad[2], h: Wr.line }, Wr.style, {}, Wr.colour)); }
    return `<div class="box" data-act="picker" style="${rect(b)}">` + art(Pk.part, zero(b.w, b.h)) + inner + `</div>`;
  }

  /* ---- proposed, live rewind: a button beside the Impure Config sprocket in the top bar, and the strip it opens under
     the bar over the paused game: the transport, the track up to now, the time, Play from here and Back to now. The
     sprocket and the button are placed in the top bar's own pixels (entry.bar tall), scaled with it ---- */
  function live(st) {
    const E = L.proposed.live.entry, S = L.proposed.live.strip, k = st.frame.top / E.bar, Sp = E.sprocket;
    let h = `<div class="box ex" style="${rect({ x: st.W - (Sp.r + Sp.s) * k, y: Sp.t * k, w: Sp.s * k, h: Sp.s * k })};--wbs:${+(Sp.s * k * .75).toFixed(3)}" data-wb="⚙"></div>`;
    h += button({ act: 'rewind', icon: { name: E.icon.name, padf: E.icon.padf } }, E, { x: st.W - (E.r + E.s) * k, y: E.t * k, w: E.s * k, h: E.s * k }, st, st.live.open);
    if (!st.live.open) return h;
    const x = st.frame.panel + S.l, w = st.W - S.r - x, y = st.frame.top + S.t, ih = S.h - 2 * S.pad;
    let inner = '', cx = S.pad;
    for (const t of S.transport) { inner += button({ act: t.act, icon: t.icon }, S.button, { x: cx, y: S.pad, w: t.w, h: ih }, st); cx += t.w + S.gap; }
    const right = S.actions.reduce((a, q) => a + q.w + S.gap, 0) + S.time.w + S.now.w;
    const tw = w - S.pad - right - cx, Tr = S.track, tb = { x: cx, y: (S.h - Tr.h) / 2, w: tw, h: Tr.h };
    const fp = Tr.fill.pad, fb = { x: fp[0], y: fp[1], w: tw - fp[0] - fp[2], h: Tr.h - fp[1] - fp[3] }, p = st.t / st.T;
    const phb = place(frame(tw, Tr.h), { pad: Tr.playhead.pad }), phx = Tr.ends + (tw - 2 * Tr.ends) * p;
    inner += `<div class="box" data-track data-ends="${Tr.ends}" style="${rect(tb)}">` + art(Tr.part, zero(tw, Tr.h))
      + `<div class="clip" data-fill="${fb.w}" style="${frect({ ...fb, w: fb.w * p })}">${art(Tr.fill.part, zero(fb.w, fb.h))}</div>`
      + art(Tr.playhead.part, { x: phx - Tr.playhead.w / 2, y: phb.y, w: Tr.playhead.w, h: phb.h }, '', '', { playhead: `${tw} ${Tr.ends} ${Tr.playhead.w}` }) + `</div>`;
    cx += tw;
    inner += text('now', { x: cx, y: S.pad, w: S.now.w, h: ih }, S.now.style, S.now); cx += S.now.w;
    inner += text(`${clock(st.t)} / ${clock(st.T)}`, { x: cx, y: S.pad, w: S.time.w, h: ih }, S.time.style, S.time, null, { time: 1 }); cx += S.time.w + S.gap;
    for (const q of S.actions) { inner += button({ act: q.act, text: { ...S.text, html: q.text } }, S.button, { x: cx, y: S.pad, w: q.w, h: ih }, st); cx += q.w + S.gap; }
    return h + `<div class="box" data-act="live" style="${rect({ x, y, w, h: S.h })}">` + art(S.part, zero(w, S.h)) + inner + `</div>`;
  }

  /* Teams are told apart only when one of them has two players: a free-for-all, every player alone, has no
     headers and no team split, while a 2v1v1 gives each lone player a header of its own. */
  function headed(st) { return st.teams.length > 1 && st.teams.some(m => m.length > 1); }

  /* ---- the column: team headers and player rows from the top bar down, as many as fit above the dock and its foot
     plate, from the first item the scroll shows ---- */
  function column(st) {
    const C = L.column, Fo = C.foot, x = st.W - C.r - C.w, top = st.frame.top + C.t;
    const bottom = st.H - st.frame.bottom - L.dock.h - lanesH(st) - C.dock_gap, area = bottom - top - Fo.gap - Fo.h;
    const items = [];
    const hd = headed(st);   // a team without a header cannot be folded, so its rows always show
    st.teams.forEach((m, t) => { if (hd) items.push({ team: t }); if (!hd || !st.folded.includes(t)) m.forEach(i => items.push({ seat: i })); });
    const hOf = (it, k) => it.team != null ? L.header.h + (k ? C.header_gap : 0) : L.row.h;
    // the scroll is held to the last first item from which every item to the end still fits
    const fits = f => { let u = 0; for (let k = f; k < items.length; k++) { u += (k > f ? C.gap : 0) + hOf(items[k], k - f); if (u > area) return false; } return true; };
    let last = 0;
    while (last < items.length - 1 && !fits(last)) last++;
    const first = Math.max(0, Math.min(st.scroll || 0, last)), vis = items.slice(first);
    let used = 0, n = 0;
    while (n < vis.length && used + (n ? C.gap : 0) + hOf(vis[n], n) <= area) { used += (n ? C.gap : 0) + hOf(vis[n], n); n++; }
    n = Math.max(1, n); used = Math.max(used, hOf(vis[0], 0));
    let h = '', y = 0;
    vis.slice(0, n).forEach((it, k) => {
      h += it.team != null ? header(st, it.team, y + (k ? C.header_gap : 0)) : row(st, st.seats[it.seat], y);
      y += hOf(it, k) + C.gap;
    });
    if (n < items.length) {
      const S = C.scroll, Bt = S.button, Tk = S.track, Th = S.thumb, len = used - 2 * Tk.ends - 2 * Th.inset;
      const th = Math.max(Th.min, len * n / items.length), ty = Tk.ends + Th.inset + (last ? (len - th) * first / last : 0);
      h += art(Tk.part, { x: Tk.x, y: Tk.ends, w: Tk.w, h: used - 2 * Tk.ends }, '', '', { act: Tk.act }) + art(Th.part, { x: Th.x, y: ty, w: Th.w, h: th })
        + [[0, Bt.up, Bt.acts[0]], [used - Bt.h, Bt.down, Bt.acts[1]]].map(([by, ic, act]) => { const b = { x: Bt.x, y: by, w: Bt.w, h: Bt.h };
          return art(Bt.part, b, icon(ic, place(frame(b.w, b.h), { padf: Bt.icon_padf })), 'hot', { act }); }).join('');
    }
    const FF = frame(Fo.w, Fo.h, Fo.part), fy = used + Fo.gap;
    h += `<div class="box" style="${rect({ x: C.w - Fo.w, y: fy, w: Fo.w, h: Fo.h })}">` + art(Fo.part, zero(Fo.w, Fo.h))
      + Fo.buttons.map(s => button(s, Fo.button, place(FF, { in: s.in, pad: Fo.button.pad }), st, s.toggle && st.toggles[s.toggle])).join('') + `</div>`;
    // the whole column is the HUD's: a click on a row's body is taken and does nothing, and the wheel scrolls it
    return `<div class="box" data-act="${C.act}" data-first="${first}" data-last="${last}" style="${rect({ x, y: top, w: C.w, h: fy + Fo.h })}">${h}</div>`;
  }

  function header(st, t, y) {
    const Hd = L.header, W = L.column.w, F = frame(W, Hd.h, Hd.part), m = st.teams[t], ps = m.map(i => st.seats[i]);
    const sum = { units: ps.reduce((a, p) => a + p.units, 0), alive: ps.filter(p => !p.lost).length };
    const inc = k => ps.reduce((a, p) => a + p.res[k], 0);
    const fold = st.folded.includes(t);
    return `<div class="box hot" data-act="${Hd.act}" data-team="${t}" style="${rect({ x: 0, y, w: W, h: Hd.h })}">` + art(Hd.part, zero(W, Hd.h))
      + icon(fold ? Hd.fold.closed : Hd.fold.open, place(F, Hd.fold))
      + text(`Team ${t + 1} · ${sum.alive}/${m.length} cmdrs`, place(F, Hd.title), Hd.title.style, Hd.title)
      + text(k1(sum.units), place(F, Hd.sum), Hd.sum.style, Hd.sum)   // a bare number, as a row's: the faces carry no Greek
      + Hd.lines.map(l => text(l.res.toUpperCase(), place(F, { ...Hd.letter, in: l.in }), Hd.letter.style, Hd.letter, L.resource[l.res])
        + text(signed(kK(inc(l.res + 'i')), kK(inc(l.res + 'o'))), place(F, { ...Hd.value, in: l.in }), Hd.value.style, Hd.value)).join('') + `</div>`;
  }

  function row(st, p, y) {
    const Rw = L.row, W = L.column.w, F = frame(W, Rw.h, Rw.part);
    let h = art(Rw.part, zero(W, Rw.h));
    const sb = place(F, Rw.stripe);
    h += `<div class="box" style="${rect(sb)};background:${p.colour};box-shadow:inset 0 0 ${U(Rw.stripe.shade)} rgba(0,0,0,.55)"></div>`;
    h += text(esc(p.name), place(F, { ...Rw.name, pad: p.lost ? Rw.name.lost_pad : Rw.name.pad }), Rw.name.style, Rw.name);
    if (p.lost) h += icon(Rw.skull.icon, place(F, Rw.skull));
    h += text(Math.round(p.units), place(F, Rw.units), Rw.units.style, Rw.units);
    for (const l of Rw.lines) {
      const r = p.res, v = r[l.res], max = r[l.res + 'max'], vb = place(F, { ...Rw.value, in: l.store }), fit = { fit: Rw.value.fit };
      const bb = place(F, { ...Rw.bar, in: l.store }), lb = place(F, { ...Rw.letter, in: l.store });
      h += text(l.res.toUpperCase(), lb, Rw.letter.style, Rw.letter, L.resource[l.res])
        + text(`${k1(v)} / ${k1(max)}`, vb, Rw.value.style, fit)
        + `<div class="box" style="${rect(bb)};background:${Rw.bar.bg};box-shadow:inset 0 0 0 ${U(Rw.bar.edge)} rgba(0,0,0,.8)">`
        + `<i style="position:absolute;left:0;top:0;bottom:0;width:${(100 * (max > 0 ? Math.min(1, v / max) : 0)).toFixed(1)}%;background:${L.resource[l.res]}"></i></div>`
        + text(signed(rate(r[l.res + 'i']), rate(r[l.res + 'o'])), place(F, { ...Rw.income, in: l.inc }), Rw.income.style, Rw.income);
    }
    h += hover(st, p, place(F, Rw.hover));
    // the row is a control of its own, under its hover's buttons: hovering it is what shows them
    return `<div class="box row${p.out ? ' dim' : ''}" data-act="${Rw.act}" data-seat="${p.i}" style="${rect({ x: 0, y, w: W, h: Rw.h })}${p.out ? `;filter:brightness(${L.dim.brightness}) saturate(${L.dim.saturate})` : ''}">${h}</div>`;
  }

  /* the actions over a hovered row: one margin round the strip's edge and between its buttons. Sight and Follow are
     pressed in on the player they are set to (st.sight, st.follow), and a click on the pressed one undoes it */
  function hover(st, p, b) {
    const Hv = L.hover, m = Hv.margin, n = Hv.buttons.length, bw = (b.w - (n + 1) * m) / n;
    return `<div class="box hov" style="${rect(b)}">` + art(Hv.part, zero(b.w, b.h)) + Hv.buttons.map((s, k) => button(
      { act: s.act, icon: { ...Hv.button.icon, name: s.icon }, text: { ...Hv.button.text, html: s.text }, data: { seat: p.i } },
      Hv.button, { x: m + k * (bw + m), y: m, w: bw, h: b.h - 2 * m }, st, st[s.act] === p.i)).join('') + `</div>`;
  }

  /* ---- the stats window: chips in two trays, the chart over the chart and legend recesses. The window is the HUD's
     whole, as the column is: a click on its body is taken and does nothing ---- */
  const measure = k => L.window.measures.find(q => q.k === k) || L.window.measures[0];
  function windowHtml(st) {
    const Wn = L.window, S = st.stats, F = frame(Wn.w, Wn.h, Wn.part), m = measure(S.measure);
    const ch = Wn.chip, tray = s => place(F, { in: s.in });
    const chip = (b, label, on, data, off) => button({ act: 'chip', text: { ...ch.text, html: label }, data, off }, ch, b, st, on);
    let h = art(Wn.part, zero(Wn.w, Wn.h))
      + icon(Wn.grip.icon, place(F, Wn.grip)) + text(`Game stats · ${m.n} ${m.u}`, place(F, Wn.title), Wn.title.style, Wn.title)
      + `<div class="box" data-act="drag" style="${rect(place(F, { in: Wn.title.in }))};cursor:move"></div>`
      + button({ act: Wn.close.act, icon: Wn.close.icon }, Wn.close, place(F, Wn.close), st);
    for (const s of Wn.trays) {
      const tb = tray(s);
      h += text(s.label, place(F, { ...Wn.label, in: s.in }), Wn.label.style, Wn.label);
      let x = tb.x + ch.start;
      for (const q of Wn.measures.filter(q => q.g === s.group)) {
        const w = Wn.chips[q.k];
        h += chip({ x, y: tb.y + (tb.h - ch.h) / 2, w, h: ch.h }, q.n, q.k === m.k, { kind: 'measure', val: q.k });
        x += w + ch.gap;
      }
      if (s.split) {
        let r = tb.x + tb.w - Wn.split.end;
        for (const q of [...Wn.split.chips].reverse()) {
          const b = { x: r - q.w, y: tb.y + (tb.h - ch.h) / 2, w: q.w, h: ch.h };
          h += chip(b, q.text, S.split === q.value, { kind: 'split', val: q.value }, q.value === 'team' && !headed(st) && Wn.split.off);
          r -= q.w + ch.gap;
        }
      }
    }
    const cb = place(F, Wn.chart), lg = place(F, { in: Wn.chart.legend }), cw = place(F, { in: Wn.chart.in[0] });
    const R = lg.x + lg.w - (cw.x + cw.w) + 1;
    h += `<svg data-chart="${cb.w} ${cb.h} ${R}" viewBox="0 0 ${cb.w} ${cb.h}" style="position:absolute;${rect(cb)}">${chart(st, cb.w, cb.h, R)}</svg>`
      + text(`${m.n[0].toUpperCase() + m.n.slice(1)} ${m.u}&nbsp;·&nbsp;<span data-time>${clock(st.t)} / ${clock(st.T)}</span>`, place(F, Wn.foot), Wn.foot.style, Wn.foot);
    const x = Math.max(0, Math.min(st.W - Wn.w, S.x)), y = Math.max(0, Math.min(st.H - Wn.h, S.y)), [dy, blur, a] = Wn.shadow;
    return `<div class="box" id="statwin" data-act="${Wn.act}" style="${rect({ x, y, w: Wn.w, h: Wn.h })};box-shadow:0 ${U(dy)} ${U(blur)} rgba(0,0,0,${a})">${h}</div>`;
  }

  /* a measure's value: metal income in tenths below 100, thousands in k from 10 000 on, whole numbers otherwise */
  const statFormat = (k, v) => k === 'metal' && v < 100 ? v.toFixed(1) : v >= 10000 ? (v / 1000).toFixed(v >= 100000 ? 0 : 1) + 'k' : String(Math.round(v));

  /* ---- the chart: one line per player or team; the part after the playhead faint; the legend ranked at the playhead.
     Sample s is at t0 + s * dt seconds (st.stats.t0, .dt); the minute labels step by the first of axis.every that puts
     them axis.space apart ---- */
  function chart(st, W, H, R) {
    const C = L.chart, TS = L.styles[C.style], S = st.stats, G = S.groups, NS = G.length ? G[0].v.length : 0, DT = S.dt;
    const Lp = C.plot.l, Tp = C.plot.t, pw = W - Lp - R, ph = H - Tp - C.plot.b, X = s => Lp + pw * (S.t0 + s * DT) / st.T;
    const now = Math.max(0, Math.min(NS - 1, Math.round((st.t - S.t0) / DT)));
    const max = nice(Math.max(...G.flatMap(g => g.v))), Y = v => Tp + ph * (1 - v / max), fv = v => statFormat(S.measure, v);
    const dark = c => { const n = parseInt(c.slice(1), 16); return .3 * (n >> 16) + .59 * ((n >> 8) & 255) + .11 * (n & 255) < C.line.dark; };
    const tx = (x, y, s, o) => `<text x="${x.toFixed(1)}" y="${y.toFixed(1)}" font-family="${TS.font}" font-weight="${TS.weight}" letter-spacing="${TS.spacing}em"${TS.upper ? ' style="text-transform:uppercase"' : ''} ${o}>${s}</text>`;
    const A = C.axis, xph = Lp + pw * st.t / st.T;
    let h = `<defs><clipPath id="past"><rect x="0" y="0" width="${xph.toFixed(1)}" height="${H}"/></clipPath></defs>`;
    [0, max / 2, max].forEach(v => { const y = Y(v);
      h += `<line x1="${Lp}" x2="${Lp + pw}" y1="${y}" y2="${y}" stroke="${C.grid.colour}" stroke-opacity="${C.grid.opacity}" stroke-width="${C.grid.width}"/>`
        + tx(Lp - A.gap, y + A.baseline, fv(v), `font-size="${A.size}" fill="${A.colour}" text-anchor="end"`); });
    const step = A.every.find(e => pw * e / st.T >= A.space) || A.every[A.every.length - 1];
    // a minute label nearer the playhead's than the labels' own spacing would lie under it: left out
    for (let t = 0; t <= st.T; t += step) if (Math.abs(Lp + pw * t / st.T - xph) >= A.space)
      h += tx(Lp + pw * t / st.T, Tp + ph + A.below, clock(t), `font-size="${A.size}" fill="${A.colour}" text-anchor="middle"`);
    const Ln = C.line, body = G.map(g => {
      const pts = g.v.map((v, s) => `${X(s).toFixed(1)},${Y(v).toFixed(1)}`).join(' ');
      let l = (dark(g.col) ? `<polyline points="${pts}" fill="none" stroke="${Ln.halo}" stroke-width="${Ln.width + 1}" stroke-opacity="${Ln.halo_opacity}" stroke-linejoin="round"/>` : '')
        + `<polyline points="${pts}" fill="none" stroke="${g.col}" stroke-width="${Ln.width}" stroke-linejoin="round"/>`;
      const s = g.death != null ? Math.round((g.death - S.t0) / DT) : -1;
      if (s >= 0 && s < NS) { const x = X(s), y = Y(g.v[s]), c = Ln.cross;
        l += `<path d="M${x - c},${y - c}l${2 * c},${2 * c}m0,${-2 * c}l${-2 * c},${2 * c}" stroke="${dark(g.col) ? Ln.halo : g.col}" stroke-width="${Ln.cross_w}"/>`; }
      return l; }).join('');
    h += `<g opacity="${Ln.faint}">${body}</g><g clip-path="url(#past)">${body}</g>`
      + `<line x1="${xph}" x2="${xph}" y1="${Tp - C.now.over}" y2="${Tp + ph + C.now.over}" stroke="${C.now.colour}" stroke-width="${C.now.width}"/>`
      + tx(xph, Tp + ph + A.below, clock(st.t), `font-size="${A.size}" fill="${C.now.colour}" text-anchor="middle" font-weight="700"`);
    const Lg = C.legend, lx = W - R + Lg.x + Lg.pad, ly = Tp + Lg.pad, lw = R - Lg.r - 2 * Lg.pad, lh = ph - 2 * Lg.pad;
    const ranked = G.map(g => ({ g, v: NS ? g.v[now] : 0 })).sort((a, b) => b.v - a.v), rh = Math.min(Lg.row, lh / ranked.length), fs = Math.min(Lg.size, rh * Lg.text_h);
    return h + ranked.map((e, k) => { const yy = ly + k * rh;
      return `<rect x="${lx}" y="${yy + rh * Lg.swatch_y}" width="${Lg.swatch}" height="${rh * Lg.swatch_h}" fill="${e.g.col}" stroke="${dark(e.g.col) ? Lg.swatch_edge : 'none'}" stroke-width="${Lg.swatch_edge_w}"/>`
        + tx(lx + Lg.text_x, yy + rh * Lg.text_y, e.g.name, `font-size="${fs}" fill="${Lg.name}"`)
        + tx(lx + lw, yy + rh * Lg.text_y, fv(e.v), `font-size="${fs}" fill="${Lg.value}" text-anchor="end"`); }).join('');
  }
  const nice = v => { if (!(v > 0)) return 1; const p = Math.pow(10, Math.floor(Math.log10(v))), f = v / p; return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * p; };

  /* ---- composing: a grid-sliced or 9-slice part, or a tiled fill, drawn on a canvas of its box's size snapped as
     Chrome snaps the element, each edge rounded half up to a device pixel, which the element then shows pixel for
     pixel. The pieces are tagpu_hudlay_expand's and are sampled as the DLL's GPU samples them (paint), so the game and
     the mockup draw the same texels at the same pixels: a grid part cell by cell, the cells sharing rounded edges so
     no seam can show; a 9-slice part's edges kept at their size in units (whole pixels, floored, never under one)
     unless the box is smaller than both together, when all four shrink by one factor; a tiled fill one piece
     repeating along x at its height's scale. An xtile part's x band is one period of a seamless tile, repeated a
     whole number of times at about its painted length, each copy an equal share of the band with whole-pixel edges;
     a 9-slice's middle row by row at that row's edge scale. Each grid or 9-slice part keeps its pieces (`quads`:
     destination x0, y0, x1, y1 in its pixels, source u0, v0, u1, v1 in the part's), which tools/hudart/laycheck.py
     compares with the DLL's. ---- */
  const copies = (len, period) => period > 0 ? Math.max(1, Math.round(len / period)) : 1;
  function canvas(w, h) { const c = document.createElement('canvas'); c.width = Math.max(1, w); c.height = Math.max(1, h); return c; }
  /* A part's texels as the DLL uploads them, premultiplied to eight bits (tagpu_hudart.c), and its mip chain as the
     DLL blits it: each level half the one above, floored, never under one texel, each texel the bilinear sample of
     the level above at its centre, stored to eight bits (tagpu_vk_hud.c, vkCmdBlitImage with a linear filter). */
  const MIPS = {};
  function mips(n) {
    if (MIPS[n]) return MIPS[n];
    const img = IMG[n];
    let w = img.naturalWidth, h = img.naturalHeight;
    const g = canvas(w, h).getContext('2d');
    g.drawImage(img, 0, 0);
    const s = g.getImageData(0, 0, w, h).data, p = new Float32Array(w * h * 4);
    for (let i = 0; i < s.length; i += 4) {
      const a = s[i + 3];
      p[i] = Math.floor((s[i] * a + 127) / 255); p[i + 1] = Math.floor((s[i + 1] * a + 127) / 255);
      p[i + 2] = Math.floor((s[i + 2] * a + 127) / 255); p[i + 3] = a;
    }
    const L = [{ w, h, p }], o = [0, 0, 0, 0];
    while ((w > 1 || h > 1) && L.length < 16) {
      const nw = w > 1 ? w >> 1 : 1, nh = h > 1 ? h >> 1 : 1, q = new Float32Array(nw * nh * 4), up = L[L.length - 1];
      for (let y = 0; y < nh; y++) for (let x = 0; x < nw; x++) {
        bilinear(up, (x + 0.5) * w / nw, (y + 0.5) * h / nh, false, o);
        for (let c = 0; c < 4; c++) q[(y * nw + x) * 4 + c] = Math.round(o[c]);
      }
      L.push({ w: nw, h: nh, p: q }); w = nw; h = nh;
    }
    return MIPS[n] = L;
  }
  // a level's bilinear sample at texel coordinates (x, y), texel centres at the halves; clamped to the edge, or along x
  // repeating (wrap)
  function bilinear(l, x, y, wrap, o) {
    const tx = x - 0.5, ty = y - 0.5, x0 = Math.floor(tx), y0 = Math.floor(ty), fx = tx - x0, fy = ty - y0;
    const cx = i => wrap ? ((i % l.w) + l.w) % l.w : Math.min(l.w - 1, Math.max(0, i)), cy = j => Math.min(l.h - 1, Math.max(0, j));
    const a = cx(x0), b = cx(x0 + 1), c = cy(y0) * l.w, d = cy(y0 + 1) * l.w, p = l.p;
    for (let k = 0; k < 4; k++)
      o[k] = (p[(c + a) * 4 + k] * (1 - fx) + p[(c + b) * 4 + k] * fx) * (1 - fy) + (p[(d + a) * 4 + k] * (1 - fx) + p[(d + b) * 4 + k] * fx) * fy;
  }
  /* The pieces drawn as the DLL's part shader draws them: a pixel is a piece's when its centre is inside it, its
     coordinates interpolated at that centre, and sampled through a linear filter between the two mip levels its
     footprint falls between, the level of detail the log2 of the larger of the piece's two scales in texels a pixel
     (the sampler: tagpu_vk_hud.c mk_sampler). */
  function paint(n, quads, Wd, Hd, wrap) {
    const L = mips(n), w0 = L[0].w, h0 = L[0].h, out = new ImageData(Math.max(1, Wd), Math.max(1, Hd)), d = out.data;
    const A = [0, 0, 0, 0], B = [0, 0, 0, 0];
    for (const [x0, y0, x1, y1, u0, v0, u1, v1] of quads) {
      const su = (u1 - u0) / (x1 - x0), sv = (v1 - v0) / (y1 - y0);
      const lod = Math.min(L.length - 1, Math.max(0, Math.log2(Math.max(Math.abs(su) * w0, Math.abs(sv) * h0))));
      const l0 = Math.floor(lod), f = lod - l0, l1 = Math.min(L.length - 1, l0 + 1);
      for (let y = Math.max(0, y0); y < Math.min(Hd, y1); y++) for (let x = Math.max(0, x0); x < Math.min(Wd, x1); x++) {
        const u = u0 + (x + 0.5 - x0) * su, v = v0 + (y + 0.5 - y0) * sv;
        bilinear(L[l0], u * L[l0].w, v * L[l0].h, wrap, A);
        if (f > 0) { bilinear(L[l1], u * L[l1].w, v * L[l1].h, wrap, B); for (let k = 0; k < 4; k++) A[k] += (B[k] - A[k]) * f; }
        const i = (y * out.width + x) * 4, a = A[3];
        // the canvas holds unpremultiplied colour
        d[i] = a > 0 ? A[0] * 255 / a : 0; d[i + 1] = a > 0 ? A[1] * 255 / a : 0; d[i + 2] = a > 0 ? A[2] * 255 / a : 0; d[i + 3] = a;
      }
    }
    const c = canvas(Wd, Hd);
    c.getContext('2d').putImageData(out, 0, 0);
    return c.toDataURL();
  }
  function composeGrid(n, Wd, Hd, px) {
    const k = K[n], key = ['g', n, Wd, Hd, px].join('|');
    if (cache[key]) return cache[key];
    const [DW, DH] = k.design, fx = bandMap(k.xs, DW, Wd / px), fy = bandMap(k.ys, DH, Hd / px);
    const bx = [0, ...k.xs.flat(), DW], by = [0, ...k.ys.flat(), DH], quads = [];
    for (let j = 0; j + 1 < by.length; j++) for (let i = 0; i + 1 < bx.length; i++) {
      const dx0 = Math.round(fx(bx[i]) * px), dx1 = Math.round(fx(bx[i + 1]) * px), dy0 = Math.round(fy(by[j]) * px), dy1 = Math.round(fy(by[j + 1]) * px);
      if (!(dx1 > dx0 && dy1 > dy0)) continue;
      const m = k.xtile && i % 2 ? copies(dx1 - dx0, (bx[i + 1] - bx[i]) * px) : 1;
      for (let e = 0; e < m; e++)
        quads.push([dx0 + Math.round((dx1 - dx0) * e / m), dy0, dx0 + Math.round((dx1 - dx0) * (e + 1) / m), dy1, bx[i] / DW, by[j] / DH, bx[i + 1] / DW, by[j + 1] / DH]);
    }
    return cache[key] = { url: paint(n, quads, Wd, Hd, false), quads };
  }
  function composeNine(n, Wd, Hd, px) {
    const k = K[n], img = IMG[n], key = ['n', n, Wd, Hd, px].join('|');
    if (cache[key]) return cache[key];
    const pw = img.naturalWidth, ph = img.naturalHeight, [st, sr, sb, sl] = k.slice, mw = pw - sr - sl;
    const edge = v => v >= 1 ? Math.floor(v) : v > 0 ? 1 : 0;
    let t = st / k.scale * px, r = sr / k.scale * px, b = sb / k.scale * px, l = sl / k.scale * px, f = 1;
    if (l + r > 0) f = Math.min(f, Wd / (l + r));
    if (t + b > 0) f = Math.min(f, Hd / (t + b));
    t = edge(t * f); r = edge(r * f); b = edge(b * f); l = edge(l * f);
    const dx = [0, l, Wd - r, Wd], dy = [0, t, Hd - b, Hd], sx = [0, sl, pw - sr, pw], sy = [0, st, ph - sb, ph];
    let m = [1, 1, 1];
    if (k.xtile) {
      // a row's tile is the painted middle at its edge's scale: the top row and the middle at the top edge's, the bottom row at the bottom's
      const ts = st > 0 && t > 0 ? t / st : 0, bs = sb > 0 && b > 0 ? b / sb : 0, ms = ts > 0 ? ts : bs > 0 ? bs : px * f / k.scale;
      m = [ts > 0 ? ts : ms, ms, bs > 0 ? bs : ms].map(sc => copies(dx[2] - dx[1], mw * sc));
    }
    const quads = [];
    for (let j = 0; j < 3; j++) for (let i = 0; i < 3; i++) {
      if (!(dx[i + 1] > dx[i] && dy[j + 1] > dy[j])) continue;
      const mi = i === 1 ? m[j] : 1;
      for (let e = 0; e < mi; e++)
        quads.push([dx[i] + Math.round((dx[i + 1] - dx[i]) * e / mi), dy[j], dx[i] + Math.round((dx[i + 1] - dx[i]) * (e + 1) / mi), dy[j + 1],
                    sx[i] / pw, sy[j] / ph, sx[i + 1] / pw, sy[j + 1] / ph]);
    }
    return cache[key] = { url: paint(n, quads, Wd, Hd, false), quads };
  }
  // a tiled fill: one piece the box's height, its period the part's width at that height in whole pixels, repeating
  function composeTile(n, Wd, Hd) {
    const img = IMG[n], key = ['t', n, Wd, Hd].join('|');
    if (cache[key]) return cache[key];
    const tw = Math.max(1, Math.round(img.naturalWidth * Math.max(1, Hd) / img.naturalHeight));
    return cache[key] = { url: paint(n, [[0, 0, Wd, Hd, 0, 0, Wd / tw, 1]], Wd, Hd, true), quads: null };
  }
  // one composed part at its snapped size: its box's edges from the layer's corner, which sits on a device pixel
  let layer = null, layerPx = 1;
  function composeOne(e) {
    const d = window.devicePixelRatio || 1, o = layer.getBoundingClientRect(), r = e.getBoundingClientRect();
    const R = v => Math.floor(v * d + 0.5);
    const w = R(r.right - o.left) - R(r.left - o.left), h = R(r.bottom - o.top) - R(r.top - o.top);
    const n = e.dataset.gp || e.dataset.tile;
    const out = e.dataset.tile ? composeTile(n, w, h) : K[n].kind === 'nine' ? composeNine(n, w, h, layerPx) : composeGrid(n, w, h, layerPx);
    e.style.backgroundImage = `url("${out.url}")`;
    if (out.quads) e.dataset.quads = JSON.stringify(out.quads);
  }
  // px: device pixels per HUD unit
  function compose(el, px) {
    layer = el; layerPx = px;
    el.querySelectorAll('[data-gp], [data-tile]').forEach(composeOne);
  }

  /* ---- the toast: a short message in a dark box centred across the screen, above everything; the box is the layout's
     size, not the text's, since the DLL lays the HUD out without the faces' metrics ---- */
  function toast(st) {
    const Ts = L.toast, b = { x: (st.W - Ts.w) / 2, y: st.H * Ts.top, w: Ts.w, h: Ts.h }, n = parseInt(Ts.bg.slice(1), 16);
    return `<div class="box" style="${rect(b)};background:rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${Ts.alpha});box-shadow:inset 0 0 0 ${U(Ts.edge)} ${Ts.edge_colour}">`
      + text(esc(st.toast), zero(b.w, b.h), Ts.style, { align: 'c' }) + `</div>`;
  }

  // the whole HUD for a state; see index.html for the state's fields
  function draw(el, st, px) {
    cssUnit = parseFloat(getComputedStyle(el).getPropertyValue('--u')) || 1;
    el.style.setProperty('--press', L.press_shift);
    el.innerHTML = (st.live ? live(st) : dock(st) + column(st) + (st.stats.open ? windowHtml(st) : '') + (st.forks && st.picker ? picker(st) : ''))
      + (st.toast ? toast(st) : '');
    compose(el, px);
  }
  // what moves while the replay plays, without rebuilding: the fill, the playhead, the clocks and the chart
  function tick(el, st) {
    const p = st.t / st.T, pf = st.live ? p : filled(st), now = `${clock(st.t)} / ${clock(st.len ?? st.T)}`;
    el.querySelectorAll('[data-fill]').forEach(e => { e.style.width = UF(+e.dataset.fill * pf); });
    // a watched lane fills from its branch to the playhead, in the screen units its data carries
    const hd = el.querySelector('[data-lanehead]');
    if (hd) { const [a, w] = hd.dataset.lanehead.split(' ').map(Number), x = a + w * p;
      el.querySelectorAll('[data-lanefill]').forEach(e => { const [b0, b1] = e.dataset.lanefill.split(' ').map(Number); e.style.width = UF(Math.max(0, Math.min(x, b1) - b0)); }); }
    el.querySelectorAll('[data-lanehead]').forEach(e => { const [a, w, hw] = e.dataset.lanehead.split(' ').map(Number); e.style.left = U(a + w * p - hw / 2); });
    el.querySelectorAll('[data-tot]').forEach(e => { const s = clock(st.t); if (e.textContent !== s) e.textContent = s; });
    el.querySelectorAll('[data-playhead]').forEach(e => { const [w, ends, pw] = e.dataset.playhead.split(' ').map(Number); e.style.left = U(ends + (w - 2 * ends) * p - pw / 2); });
    el.querySelectorAll('[data-time]').forEach(e => { if (e.textContent !== now) e.textContent = now; });
    el.querySelectorAll('[data-chart]').forEach(e => { const [w, h, R] = e.dataset.chart.split(' ').map(Number); e.innerHTML = chart(st, w, h, R); });
  }
  return { load, draw, tick, press, get layout() { return L; } };
})();
