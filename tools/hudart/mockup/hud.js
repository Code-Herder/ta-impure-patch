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
    await Promise.all(Object.keys(K).filter(n => K[n].kind === 'grid' || K[n].kind === 'tile').map(n => {
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
  const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;');
  const attrs = a => Object.entries(a || {}).map(([k, v]) => ` data-${k}="${esc(v)}"`).join('');
  // a part at box b; `cls` adds classes (hot, pressed, off, face), `data` adds data- attributes
  function art(part, b, inner = '', cls = '', data, style = '') {
    const k = K[part];
    if (!k) return `<div class="art noart ${cls}" style="${rect(b)}${style}"${attrs(data)}>${inner}</div>`;
    if (k.kind === 'grid' || k.kind === 'tile')
      return `<div class="art composed ${cls}" style="${rect(b)}${style}" data-${k.kind === 'grid' ? 'gp' : 'tile'}="${part}" data-w="${b.w}" data-h="${b.h}"${attrs(data)}>${inner}</div>`;
    return `<div class="art ${k.kind} ${cls}" data-part="${part}" style="${rect(b)};${vars(part)}${style}"${attrs(data)}>${inner}</div>`;
  }
  // the custom properties that draw a 9-slice, whole or icon part
  function vars(part) {
    const k = K[part];
    let v = `--bi:url('${url(part)}')`;
    if (k.kind === 'nine') { const [t, r, b, l] = k.slice; v += `;--bsl:${t} ${r} ${b} ${l};--bw:${[t, r, b, l].map(x => U(x / k.scale)).join(' ')}`; }
    return v;
  }
  // a held button shows its pressed part, and its face sinks; released, it shows its own part again
  function press(el, down) {
    const part = down ? el.dataset.down : el.dataset.up;
    if (!part || !K[part]) return;
    for (const d of vars(part).split(';')) { const i = d.indexOf(':'); el.style.setProperty(d.slice(0, i), d.slice(i + 1)); }
    el.classList.toggle('pressed', down);
  }
  const icon = (name, b, cls = '') => art('i_' + name, b, '', cls);
  // text in style `st`: its size shrinks to the box's height / s.fit when the box is lower than the style's line
  function text(html, b, st, s = {}, colour, data) {
    const S = L.styles[st], size = s.fit ? Math.min(S.size, b.h / s.fit) : S.size;
    const shadow = S.outline ? (([px, py, nx, ny]) => [[px, py], [nx, ny], [px, ny], [nx, py]].map(([x, y]) => `${U(x)} ${U(y)} 0 #000`).join(','))(S.outline)
      : `0 ${U(S.shadow)} 0 rgba(0,0,0,.8)`;
    return `<div class="t${s.align === 'c' ? ' c' : ''}${s.face ? ' face' : ''}"${attrs(data)} style="${rect(b)};font:${S.weight} ${U(size)}/1 '${S.font}',sans-serif;`
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
    const rx = tb.x + T.w + R.gap, rw = st.W - R.r - rx, FR = frame(rw, D.h, R.part);
    const lane = place(FR, R.markers), tr = place(FR, R.track), p = st.t / st.T, M = R.markers, B = M.badge;
    const along = (w, e, f) => e + (w - 2 * e) * f;
    const marks = st.markers.map(m => {
      const cx = along(lane.w, M.ends, m.t / st.T), s = lane.h, bb = { x: cx - s / 2, y: 0, w: s, h: s };
      const col = m.seat < 0 ? M.bookmark_dot : st.seats[m.seat].colour;
      return `<div class="box" style="${rect(bb)}"><div class="box" style="${rect({ x: B.dot, y: B.dot, w: s - 2 * B.dot, h: s - 2 * B.dot })};border-radius:50%;background:${col}"></div>`
        + art(B.part, zero(s, s)) + icon(M.icons[m.kind], { x: B.icon, y: B.icon, w: s - 2 * B.icon, h: s - 2 * B.icon }) + `</div>`;
    }).join('');
    const fp = R.track.fill.pad, fb = { x: fp[0], y: fp[1], w: tr.w - fp[0] - fp[2], h: tr.h - fp[1] - fp[3] };
    const ph = R.track.playhead, phb = place(frame(tr.w, tr.h), { pad: ph.pad }), phx = along(tr.w, R.track.ends, p);
    h += `<div class="box" data-act="${D.act}" style="${rect({ x: rx, y, w: rw, h: D.h })}">` + art(R.part, zero(rw, D.h))
      + `<div class="box" style="${rect(lane)}">${marks}</div>`
      + `<div class="box" data-track data-ends="${R.track.ends}" style="${rect(tr)}">`
      + `<div class="clip" data-fill="${fb.w}" style="${rect({ ...fb, w: fb.w * p })}">${art(R.track.fill.part, zero(fb.w, fb.h))}</div>`
      + art(ph.part, { x: phx - ph.w / 2, y: phb.y, w: ph.w, h: phb.h }, '', '', { playhead: `${tr.w} ${R.track.ends} ${ph.w}` }) + `</div>`
      + text(`${clock(st.t)} / ${clock(st.T)}`, place(FR, R.time), R.time.style, R.time, null, { time: 1 }) + `</div>`;
    return h;
  }

  /* Teams are told apart only when one of them has two players: a free-for-all, every player alone, has no
     headers and no team split, while a 2v1v1 gives each lone player a header of its own. */
  function headed(st) { return st.teams.length > 1 && st.teams.some(m => m.length > 1); }

  /* ---- the column: team headers and player rows from the top bar down, as many as fit above the dock and its foot
     plate, from the first item the scroll shows ---- */
  function column(st) {
    const C = L.column, Fo = C.foot, x = st.W - C.r - C.w, top = st.frame.top + C.t;
    const bottom = st.H - st.frame.bottom - L.dock.h - C.dock_gap, area = bottom - top - Fo.gap - Fo.h;
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
    return `<div class="box row${p.out ? ' dim' : ''}" style="${rect({ x: 0, y, w: W, h: Rw.h })}${p.out ? `;filter:brightness(${L.dim.brightness}) saturate(${L.dim.saturate})` : ''}">${h}</div>`;
  }

  /* the actions over a hovered row: one margin round the strip's edge and between its buttons. Sight and Follow are
     pressed in on the player they are set to (st.sight, st.follow), and a click on the pressed one undoes it */
  function hover(st, p, b) {
    const Hv = L.hover, m = Hv.margin, n = Hv.buttons.length, bw = (b.w - (n + 1) * m) / n;
    return `<div class="box hov" style="${rect(b)}">` + art(Hv.part, zero(b.w, b.h)) + Hv.buttons.map((s, k) => button(
      { act: s.act, icon: { ...Hv.button.icon, name: s.icon }, text: { ...Hv.button.text, html: s.text }, data: { seat: p.i } },
      Hv.button, { x: m + k * (bw + m), y: m, w: bw, h: b.h - 2 * m }, st, st[s.act] === p.i)).join('') + `</div>`;
  }

  /* ---- the stats window: chips in two trays, the chart over the chart and legend recesses ---- */
  function windowHtml(st) {
    const Wn = L.window, S = st.stats, F = frame(Wn.w, Wn.h, Wn.part), m = S.measure;
    const ch = Wn.chip, tray = s => place(F, { in: s.in });
    const chip = (b, label, on, data, off) => button({ act: 'chip', text: { ...ch.text, html: label }, data, off }, ch, b, st, on);
    let h = art(Wn.part, zero(Wn.w, Wn.h))
      + icon(Wn.grip.icon, place(F, Wn.grip)) + text(`Game stats · ${m.n} ${m.u}`, place(F, Wn.title), Wn.title.style, Wn.title)
      + `<div class="box" data-drag style="${rect(place(F, { in: Wn.title.in }))};cursor:move"></div>`
      + button({ act: Wn.close.act, icon: Wn.close.icon }, Wn.close, place(F, Wn.close), st);
    for (const s of Wn.trays) {
      const tb = tray(s);
      h += text(s.label, place(F, { ...Wn.label, in: s.in }), Wn.label.style, Wn.label);
      let x = tb.x + ch.start;
      for (const q of st.measures.filter(q => q.g === s.group)) {
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
    return `<div class="box" id="statwin" style="${rect({ x, y, w: Wn.w, h: Wn.h })};box-shadow:0 ${U(dy)} ${U(blur)} rgba(0,0,0,${a})">${h}</div>`;
  }

  /* ---- the chart: one line per player or team; the part after the playhead faint; the legend ranked at the playhead ---- */
  function chart(st, W, H, R) {
    const C = L.chart, S = st.stats, G = S.groups, NS = G[0].v.length, DT = st.T / (NS - 1);
    const Lp = C.plot.l, Tp = C.plot.t, pw = W - Lp - R, ph = H - Tp - C.plot.b, X = s => Lp + pw * s * DT / st.T, now = Math.min(NS - 1, Math.round(st.t / DT));
    const max = nice(Math.max(...G.flatMap(g => g.v))), Y = v => Tp + ph * (1 - v / max), fv = v => S.format(v);
    const dark = c => { const n = parseInt(c.slice(1), 16); return .3 * (n >> 16) + .59 * ((n >> 8) & 255) + .11 * (n & 255) < C.line.dark; };
    const tx = (x, y, s, o) => `<text x="${x.toFixed(1)}" y="${y.toFixed(1)}" font-family="${C.font}" font-weight="${C.weight}" letter-spacing="${C.spacing}em"${C.upper ? ' style="text-transform:uppercase"' : ''} ${o}>${s}</text>`;
    const A = C.axis, xph = Lp + pw * st.t / st.T;
    let h = `<defs><clipPath id="past"><rect x="0" y="0" width="${xph.toFixed(1)}" height="${H}"/></clipPath></defs>`;
    [0, max / 2, max].forEach(v => { const y = Y(v);
      h += `<line x1="${Lp}" x2="${Lp + pw}" y1="${y}" y2="${y}" stroke="${C.grid.colour}" stroke-opacity="${C.grid.opacity}" stroke-width="${C.grid.width}"/>`
        + tx(Lp - A.gap, y + A.baseline, fv(v), `font-size="${A.size}" fill="${A.colour}" text-anchor="end"`); });
    for (let t = 0; t <= st.T; t += A.every) h += tx(Lp + pw * t / st.T, Tp + ph + A.below, clock(t), `font-size="${A.size}" fill="${A.colour}" text-anchor="middle"`);
    const Ln = C.line, body = G.map(g => {
      const pts = g.v.map((v, s) => `${X(s).toFixed(1)},${Y(v).toFixed(1)}`).join(' ');
      let l = (dark(g.col) ? `<polyline points="${pts}" fill="none" stroke="${Ln.halo}" stroke-width="${Ln.width + 1}" stroke-opacity="${Ln.halo_opacity}"/>` : '')
        + `<polyline points="${pts}" fill="none" stroke="${g.col}" stroke-width="${Ln.width}" stroke-linejoin="round"/>`;
      if (g.death != null) { const s = Math.round(g.death / DT), x = X(s), y = Y(g.v[s]), c = Ln.cross;
        l += `<path d="M${x - c},${y - c}l${2 * c},${2 * c}m0,${-2 * c}l${-2 * c},${2 * c}" stroke="${dark(g.col) ? Ln.halo : g.col}" stroke-width="${Ln.cross_w}"/>`; }
      return l; }).join('');
    h += `<g opacity="${Ln.faint}">${body}</g><g clip-path="url(#past)">${body}</g>`
      + `<line x1="${xph}" x2="${xph}" y1="${Tp - C.now.over}" y2="${Tp + ph + C.now.over}" stroke="${C.now.colour}" stroke-width="${C.now.width}"/>`
      + tx(xph, Tp + ph + A.below, clock(st.t), `font-size="${A.size}" fill="${C.now.colour}" text-anchor="middle" font-weight="700"`);
    const Lg = C.legend, lx = W - R + Lg.x + Lg.pad, ly = Tp + Lg.pad, lw = R - Lg.r - 2 * Lg.pad, lh = ph - 2 * Lg.pad;
    const ranked = G.map(g => ({ g, v: g.v[now] })).sort((a, b) => b.v - a.v), rh = Math.min(Lg.row, lh / ranked.length), fs = Math.min(Lg.size, rh * Lg.text_h);
    return h + ranked.map((e, k) => { const yy = ly + k * rh;
      return `<rect x="${lx}" y="${yy + rh * Lg.swatch_y}" width="${Lg.swatch}" height="${rh * Lg.swatch_h}" fill="${e.g.col}" stroke="${dark(e.g.col) ? Lg.swatch_edge : 'none'}" stroke-width="${Lg.swatch_edge_w}"/>`
        + tx(lx + Lg.text_x, yy + rh * Lg.text_y, e.g.name, `font-size="${fs}" fill="${Lg.name}"`)
        + tx(lx + lw, yy + rh * Lg.text_y, fv(e.v), `font-size="${fs}" fill="${Lg.value}" text-anchor="end"`); }).join('');
  }
  const nice = v => { if (!(v > 0)) return 1; const p = Math.pow(10, Math.floor(Math.log10(v))), f = v / p; return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * p; };

  /* ---- composing: a grid-sliced part cell by cell at its drawn size, the cells sharing rounded edges so no seam can
     show; a tiled fill scaled to its height inside a three-tile strip, so the resampling stays periodic, then copied
     at whole-pixel steps ---- */
  function composeGrid(n, W, H, px) {
    const k = K[n], img = IMG[n], key = [n, W, H, px].join('|');
    if (cache[key]) return cache[key];
    const [DW, DH] = k.design, fx = bandMap(k.xs, DW, W), fy = bandMap(k.ys, DH, H);
    const bx = [0, ...k.xs.flat(), DW], by = [0, ...k.ys.flat(), DH], sx = img.naturalWidth / DW, sy = img.naturalHeight / DH;
    const c = document.createElement('canvas'); c.width = Math.max(1, Math.round(W * px)); c.height = Math.max(1, Math.round(H * px));
    const g = c.getContext('2d'); g.imageSmoothingQuality = 'high';
    for (let i = 0; i + 1 < bx.length; i++) for (let j = 0; j + 1 < by.length; j++) {
      const dx0 = Math.round(fx(bx[i]) * px), dx1 = Math.round(fx(bx[i + 1]) * px), dy0 = Math.round(fy(by[j]) * px), dy1 = Math.round(fy(by[j + 1]) * px);
      if (dx1 > dx0 && dy1 > dy0) g.drawImage(img, bx[i] * sx, by[j] * sy, (bx[i + 1] - bx[i]) * sx, (by[j + 1] - by[j]) * sy, dx0, dy0, dx1 - dx0, dy1 - dy0);
    }
    return cache[key] = c.toDataURL();
  }
  function composeTile(n, W, H, px) {
    const img = IMG[n], key = ['t', n, W, H, px].join('|');
    if (cache[key]) return cache[key];
    const ch = Math.max(1, Math.round(H * px)), tw = Math.max(1, Math.round(img.naturalWidth * ch / img.naturalHeight));
    const src = document.createElement('canvas'); src.width = 3 * img.naturalWidth; src.height = img.naturalHeight;
    for (let i = 0; i < 3; i++) src.getContext('2d').drawImage(img, i * img.naturalWidth, 0);
    const s = document.createElement('canvas'); s.width = 3 * tw; s.height = ch;
    const sg = s.getContext('2d'); sg.imageSmoothingQuality = 'high'; sg.drawImage(src, 0, 0, 3 * tw, ch);
    const c = document.createElement('canvas'); c.width = Math.max(1, Math.round(W * px)); c.height = ch;
    const g = c.getContext('2d');
    for (let x = 0; x < c.width; x += tw) g.drawImage(s, tw, 0, tw, ch, x, 0, tw, ch);
    return cache[key] = c.toDataURL();
  }
  // px: device pixels per HUD unit
  function compose(el, px) {
    el.querySelectorAll('[data-gp]').forEach(e => { e.style.backgroundImage = `url("${composeGrid(e.dataset.gp, +e.dataset.w, +e.dataset.h, px)}")`; });
    el.querySelectorAll('[data-tile]').forEach(e => { e.style.backgroundImage = `url("${composeTile(e.dataset.tile, +e.dataset.w, +e.dataset.h, px)}")`; });
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
    el.style.setProperty('--press', L.press_shift);
    el.innerHTML = dock(st) + column(st) + (st.stats.open ? windowHtml(st) : '') + (st.toast ? toast(st) : '');
    compose(el, px);
  }
  // what moves while the replay plays, without rebuilding: the fill, the playhead, the clocks and the chart
  function tick(el, st) {
    const p = st.t / st.T, now = `${clock(st.t)} / ${clock(st.T)}`;
    el.querySelectorAll('[data-fill]').forEach(e => { e.style.width = U(+e.dataset.fill * p); });
    el.querySelectorAll('[data-playhead]').forEach(e => { const [w, ends, pw] = e.dataset.playhead.split(' ').map(Number); e.style.left = U(ends + (w - 2 * ends) * p - pw / 2); });
    el.querySelectorAll('[data-time]').forEach(e => { if (e.textContent !== now) e.textContent = now; });
    el.querySelectorAll('[data-chart]').forEach(e => { const [w, h, R] = e.dataset.chart.split(' ').map(Number); e.innerHTML = chart(st, w, h, R); });
  }
  return { load, draw, tick, press, get layout() { return L; } };
})();
