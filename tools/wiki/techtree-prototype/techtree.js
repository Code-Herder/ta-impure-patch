/* PROTOTYPE, throwaway: four tech-tree variants over one model of the active mod's build menus.
   The model (tree.json, extract.py) is the engine's: each builder's menu as the sidebar reads it, walked breadth-first
   from the side's commander. Everything below is view. */
import { mountStage } from './stage3d.js';

const $ = s => document.querySelector(s);
const DATA = '../mockup/gfx/techtree/';
const VARIANTS = { A: 'Research tree', B: 'Focus', C: 'Build menus', D: 'Tier atlas' };
const Q = new URLSearchParams(location.search);
const S = { variant: VARIANTS[Q.get('variant')] ? Q.get('variant') : 'A', mod: Q.get('mod') || 'stock',
  side: +(Q.get('side') || 0), sel: Q.get('unit') || null };
let M = null;            // the model of the chosen mod and side
const CACHE = {};

/* ---------------- the model ---------------- */
// An upgrade (or an ability) is something a builder makes that never stands on the field as a unit of its own: a
// non-structure that is 0 x 0 (Escalation's Level 2/3 Unit Upgrade, Activate Maximum Intel) or that is born cloaked,
// stealthed and hiding its damage (Mayhem's and Escalation's companion units built onto a moho, a geo, a tower).
const isUpgrade = u => !u.structure && ((u.fp[0] === 0 && u.fp[1] === 0) || (u.cloaked && u.stealth && u.hidedamage));
const upgradable = u => /upgrad/i.test(u.description) && !isUpgrade(u);

function kindOf(u, k, side, U) {
  if (k === side.commander) return 'Commander';
  if (isUpgrade(u)) return 'Upgrade';
  // a tower whose menu holds only its own upgrade is not a factory
  const menu = (side.menus[k] || []).filter(x => U[x] && !isUpgrade(U[x]));
  if (u.structure && menu.length) return 'Factory';
  if (!u.structure && u.builder && menu.length) return 'Builder';
  if (u.structure) {
    if (u.extracts > 0 || u.energymake > 0 || u.metalmake > 0) return 'Economy';
    if (u.weapons.length) return 'Defense';
    if (u.storage[0] > 0 || u.storage[1] > 0) return 'Economy';
    return 'Support';
  }
  if (u.canfly) return 'Air';
  if (u.water) return 'Sea';
  return 'Land';
}
const KINDS = ['Commander', 'Factory', 'Builder', 'Economy', 'Defense', 'Support', 'Land', 'Air', 'Sea', 'Upgrade'];

function tierOf(u, depth) {
  const m = /LEVEL(\d)/i.exec(u.category);
  return m ? +m[1] : null;
}

function buildModel(tree, si) {
  const side = tree.sides[si], U = tree.units;
  const order = side.order.filter(k => U[k]);
  const menus = {}, parents = {}, first = {};
  for (const k of order) { menus[k] = (side.menus[k] || []).filter(x => U[x]); parents[k] = []; }
  for (const k of order) for (const x of menus[k]) if (parents[x] && !parents[x].includes(k)) parents[x].push(k);
  // the first parent: the builder met first in the walk that builds it (the edge the walk took)
  const depth = {}; depth[side.commander] = 0;
  for (const k of order) for (const x of menus[k]) if (!(x in depth)) { depth[x] = depth[k] + 1; first[x] = k; }
  const units = {};
  for (const k of order) {
    const u = U[k];
    units[k] = { ...u, key: k, kind: kindOf(u, k, side, U), depth: depth[k] ?? 0, tier: tierOf(u),
      upgrade: isUpgrade(u), upgradable: upgradable(u) };
  }
  for (const k of order) {
    const u = units[k];
    u.builds = menus[k].filter(x => !units[x].upgrade);
    u.upgrades = menus[k].filter(x => units[x].upgrade);
    u.parents = parents[k];
    u.first = first[k] || null;
    // what this builder is the first in the walk to unlock
    u.unlocks = u.builds.filter(x => first[x] === k);
  }
  // tier: the category's LEVELn if the mod has it, else the number of builders crossed from the commander
  for (const k of order) {
    const u = units[k];
    if (u.tier == null) {
      let t = 1, p = u.first;
      while (p && p !== side.commander) { if (units[p].kind === 'Builder') t++; p = units[p].first; }
      u.tier = Math.min(t, 4);
    }
  }
  return { mod: tree.mod, side, units, order, root: side.commander, base: DATA + tree.mod + '/' };
}
const pathTo = k => { const p = []; for (let x = k; x; x = M.units[x].first) p.unshift(x); return p; };
const pic = k => M.units[k].pic ? M.base + M.units[k].pic : 'data:,';
const fmt = n => n >= 10000 ? Math.round(n / 1000) + 'k' : Math.round(n).toLocaleString('en');

function chip(k, extra = '') {
  const u = M.units[k];
  const n = u.upgrades.length ? `<span class="upn" title="${u.upgrades.length} upgrade(s)">⬆${u.upgrades.length}</span>` : '';
  return `<span class="chip ${u.upgrade ? 'upg' : ''} ${k === S.sel ? 'sel' : ''} ${extra}" data-k="${k}" title="${esc(u.title)} — ${esc(u.description)}">
    <img class="pic" src="${pic(k)}" loading="lazy" alt="">${n}<b>${esc(u.title)}</b></span>`;
}
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
function byKind(keys) {
  const g = {};
  for (const k of keys) (g[M.units[k].kind] ||= []).push(k);
  return KINDS.filter(x => g[x]).map(x => [x, g[x]]);
}

/* ---------------- loading, URL, selection ---------------- */
async function load() {
  CACHE[S.mod] ||= fetch(DATA + S.mod + '/tree.json').then(r => r.ok ? r.json() : Promise.reject(r.status));
  let tree;
  try { tree = await CACHE[S.mod]; } catch (e) {
    $('#view').innerHTML = `<div class="empty">No data for ${S.mod}: run <code>tools/wiki/techtree-prototype/extract.py ${S.mod}</code> (${e}).</div>`;
    return;
  }
  if (S.side >= tree.sides.length) S.side = 0;
  $('#side').innerHTML = tree.sides.map((s, i) => `<option value="${i}">${esc(s.name)}</option>`).join('');
  $('#side').value = S.side;
  M = buildModel(tree, S.side);
  if (S.sel && !M.units[S.sel]) S.sel = null;
  $('#names').innerHTML = M.order.map(k => `<option value="${esc(M.units[k].title)}">${esc(M.units[k].description)}</option>`).join('');
  const nb = M.order.filter(k => M.units[k].kind === 'Builder' || M.units[k].kind === 'Factory').length;
  const nu = M.order.filter(k => M.units[k].upgrade).length;
  $('#stats').textContent = `${M.order.length} units · ${nb} builders & factories · ${nu} upgrades · ${Math.max(...M.order.map(k => M.units[k].depth))} steps deep`;
  render();
}
function syncURL() {
  const q = new URLSearchParams({ variant: S.variant, mod: S.mod, side: S.side });
  if (S.sel) q.set('unit', S.sel);
  history.replaceState(null, '', '?' + q);
}
function select(k, opts = {}) {
  S.sel = k;
  syncURL();
  VIEWS[S.variant].select?.(k, opts);
  renderDetail();
}
function render() {
  $('#mod').value = S.mod;
  $('#wsub').textContent = `${$('#mod').selectedOptions[0].text} · ${M.side.name} · ${VARIANTS[S.variant]}`;
  $('#sw-label').textContent = `${S.variant} (${VARIANTS[S.variant]})`;
  $('#view').innerHTML = '';
  VIEWS[S.variant].mount($('#view'));
  renderDetail();
  syncURL();
}

/* ---------------- the detail panel (shared) ---------------- */
let stage = null;
function renderDetail() {
  const d = $('#detail');
  if (stage) { stage.dispose(); stage = null; }
  if (!S.sel || S.variant === 'B' && false) { d.hidden = true; return; }
  const u = M.units[S.sel];
  d.hidden = false;
  const flags = [u.upgrade && 'upgrade', u.upgradable && 'upgradable', u.cloaked && 'cloaked', u.stealth && 'stealth',
    u.canfly && 'flies', u.water && 'naval'].filter(Boolean);
  const rows = [['Metal', `<span class="m">${fmt(u.metal)}</span>`], ['Energy', `<span class="e">${fmt(u.energy)}</span>`],
    ['Build time', fmt(u.buildtime)], ['Health', fmt(u.health)],
    u.speed && ['Speed', u.speed.toFixed(2)], ['Sight', u.sight], u.radar && ['Radar', u.radar],
    u.workertime && ['Build power', fmt(u.workertime)], u.energymake && ['Energy +', `<span class="e">${u.energymake.toFixed(1)}</span>`],
    u.extracts && ['Extracts', u.extracts.toFixed(4)], u.metalmake && ['Metal +', `<span class="m">${u.metalmake.toFixed(2)}</span>`],
    (u.storage[0] || u.storage[1]) && ['Storage', `<span class="m">${fmt(u.storage[0])}</span> / <span class="e">${fmt(u.storage[1])}</span>`],
    ['Footprint', u.fp.join(' × ')], ['Tier', u.tier]].filter(Boolean);
  const path = pathTo(S.sel);
  d.innerHTML = `
    <div class="dh"><img src="${pic(S.sel)}" alt=""><div><b>${esc(u.title)}</b><small>${esc(u.description)}</small>
      <div>${flags.map(f => `<span class="flag">${f}</span>`).join('')}<span class="kind">${u.kind}</span></div></div>
      <button class="btn x" id="dx" title="Close">✕</button></div>
    <section><div id="stage3d"></div><div class="acts" id="acts"></div></section>
    <section><h4>Figures</h4><dl>${rows.map(([a, b]) => `<dt>${a}</dt><dd>${b}</dd>`).join('')}</dl></section>
    ${u.weapons.length ? `<section><h4>Weapons</h4>${u.weapons.map(w => `<div class="weap"><span>${esc(w.name)}</span>
      <span class="kind">${w.range != null ? `range ${w.range} · dmg ${w.damage}` : ''}</span></div>`).join('')}</section>` : ''}
    <section><h4>How to get it</h4><div class="chips">${path.map(k => chip(k)).join('<span class="kind" style="align-self:center">›</span>')}</div></section>
    ${u.parents.length ? `<section><h4>Built by (${u.parents.length})</h4><div class="chips">${u.parents.map(k => chip(k)).join('')}</div></section>` : ''}
    ${u.upgrades.length ? `<section><h4 style="color:var(--up)">Upgrades (${u.upgrades.length})</h4><div class="chips">${u.upgrades.map(k => chip(k)).join('')}</div></section>` : ''}
    ${u.builds.length ? `<section><h4>Builds (${u.builds.length})</h4>${byKind(u.builds).map(([g, ks]) =>
      `<div class="kind">${g}</div><div class="chips">${ks.map(k => chip(k)).join('')}</div>`).join('')}</section>` : ''}`;
  d.querySelector('#dx').onclick = () => select(null);
  stage = mountStage(d.querySelector('#stage3d'), d.querySelector('#acts'), u, M);
}
$('#detail').addEventListener('click', e => {
  const c = e.target.closest('.chip');
  if (c) select(c.dataset.k, { centre: true });
});

/* ---------------- A: research tree (Factorio) ----------------
   One node per builder and factory; under it, the icons of what it is the first to unlock. Columns are steps from the
   commander. The edges are the walk's; selecting a node lights its path back to the commander and what it unlocks. */
const A = {
  z: 0.8, x: 30, y: 30, all: false,
  mount(host) {
    const el = document.createElement('div'); el.id = 'A';
    host.append(el);
    const spine = M.order.filter(k => ['Commander', 'Builder', 'Factory'].includes(M.units[k].kind));
    // the spine's own parent: the nearest builder up the walk
    const sp = {};
    for (const k of spine) { let p = M.units[k].first; while (p && !spine.includes(p)) p = M.units[p].first; sp[k] = p; }
    const col = {};
    for (const k of spine) col[k] = sp[k] ? col[sp[k]] + 1 : 0;
    const kids = {}; for (const k of spine) if (sp[k]) (kids[sp[k]] ||= []).push(k);
    const W = 300, GAP = 10, pos = {};
    const unl = k => [...M.units[k].unlocks.filter(x => !spine.includes(x)), ...M.units[k].upgrades];
    const world = document.createElement('div'); world.className = 'world';
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    world.append(svg);
    el.append(world);
    this.nodes = {};
    for (const k of spine) {
      const u = M.units[k], n = document.createElement('div');
      n.className = 'node'; n.dataset.k = k;
      n.style.left = col[k] * W + 'px';
      const more = u.builds.length - u.unlocks.length;
      n.innerHTML = `<div class="hd"><img class="pic" src="${pic(k)}" alt=""><div><b>${esc(u.title)}</b><small>${esc(u.description)}</small>
        <span class="kind">${u.kind}${u.tier ? ' · T' + u.tier : ''}</span></div></div>
        <div class="unl">${unl(k).map(x => `<img src="${pic(x)}" data-k="${x}" class="${M.units[x].upgrade ? 'upg' : M.units[x].upgrades.length ? 'hasup' : ''}" title="${esc(M.units[x].title)} — ${esc(M.units[x].description)}${M.units[x].upgrades.length ? ' (upgradable)' : ''}" loading="lazy">`).join('')}</div>
        <div class="ft">${u.unlocks.length} new${more ? ` · +${more} also built elsewhere` : ''}${u.upgrades.length ? ` · ${u.upgrades.length} upgrade(s)` : ''}</div>`;
      world.append(n); this.nodes[k] = n;
    }
    this.edges = [];
    const edge = (a, b, cls) => {
      const p = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      if (cls) p.setAttribute('class', cls);
      svg.append(p); this.edges.push({ a, b, p, cls });
    };
    for (const k of spine) if (sp[k]) edge(sp[k], k);
    // every other builder that also builds a spine node: dashed, shown on focus or with "all edges"
    for (const k of spine) for (const p of M.units[k].parents) if (p !== sp[k] && spine.includes(p)) edge(p, k, 'also');
    const maxc = Math.max(...Object.values(col));
    /* ROWS: a tidy tree over each card's MEASURED height (a wrapped description or footer makes a card taller than its
       icons say), each leaf the next row, a parent centred over its children. One cursor `y` runs down the whole tree
       and every card starts at or below it and moves it past its own bottom plus GAP, so no two cards share a row
       band: GAP is the least space between any two cards of a column, whatever the fonts. */
    this.layout = () => {
      const h = {};
      for (const k of spine) h[k] = this.nodes[k].offsetHeight;
      let y = 0;
      const place = k => {
        const ks = kids[k] || [];
        if (!ks.length) { pos[k] = y; y += h[k] + GAP; return; }
        const y0 = y; ks.forEach(place);
        const top = pos[ks[0]], bot = pos[ks.at(-1)] + h[ks.at(-1)];
        pos[k] = Math.max(y0, (top + bot) / 2 - h[k] / 2);
        if (pos[k] + h[k] + GAP > y) y = pos[k] + h[k] + GAP;
      };
      place(M.root);
      for (const k of spine) this.nodes[k].style.top = pos[k] + 'px';
      // the tree's own bounds: the last column's card, the last row's card (no trailing gap), which fit() frames
      this.bw = maxc * W + 236; this.bh = y - GAP;
      world.style.width = (maxc + 1) * W + 'px'; world.style.height = y + 'px';
      svg.setAttribute('width', (maxc + 1) * W); svg.setAttribute('height', y);
      for (const e of this.edges) {
        const x1 = col[e.a] * W + 236, y1 = pos[e.a] + 28, x2 = col[e.b] * W, y2 = pos[e.b] + 28, mx = (x1 + x2) / 2;
        e.p.setAttribute('d', `M${x1},${y1} C${mx},${y1} ${mx},${y2} ${x2},${y2}`);
      }
    };
    this.layout();
    // a card that changes height after this (a face or a picture arriving and changing the wrapping) lays the tree out
    // again, so the gap holds on the final metrics and not only on the first ones
    let queued = false;
    const relayout = new ResizeObserver(() => {
      if (queued) return;
      queued = true;
      requestAnimationFrame(() => { queued = false; if (this.world === world) { this.layout(); if (this.fitted) this.fit(); } });
    });
    for (const k of spine) relayout.observe(this.nodes[k]);
    this.svgAll = svg;
    el.insertAdjacentHTML('beforeend', `<div class="zoomctl"><button data-z="-">−</button><button data-z="+">+</button>
      <button data-z="fit">Fit</button><button data-z="all" class="${this.all ? 'on' : ''}">All edges</button></div>
      <div class="legend"><span><i style="background:var(--path)"></i>path from the commander</span>
      <span><i style="background:var(--kid)"></i>builds</span><span><i style="background:#6a7058"></i>also built by</span>
      <span><i style="background:var(--up)"></i>upgrade</span><span>drag to pan · wheel to zoom</span></div>`);
    this.el = el; this.world = world; this.spine = spine; this.col = col; this.pos = pos; this.W = W;
    this.apply();
    // pan, zoom, pick
    let drag = null;
    el.addEventListener('pointerdown', e => {
      if (e.target.closest('.zoomctl')) return;
      drag = { x: e.clientX, y: e.clientY, ox: this.x, oy: this.y, moved: false }; el.setPointerCapture(e.pointerId);
    });
    el.addEventListener('pointermove', e => {
      if (!drag) return;
      const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
      if (Math.abs(dx) + Math.abs(dy) > 4) { drag.moved = true; el.classList.add('drag'); }
      if (drag.moved) { this.fitted = false; this.x = drag.ox + dx; this.y = drag.oy + dy; this.apply(); }
    });
    el.addEventListener('pointerup', e => {
      el.classList.remove('drag');
      const was = drag; drag = null;
      if (was && !was.moved) {
        const t = document.elementFromPoint(e.clientX, e.clientY);
        const hit = t && (t.closest('[data-k]'));
        if (hit && el.contains(hit)) select(hit.dataset.k); else if (!t.closest('.zoomctl')) select(null);
      }
    });
    el.addEventListener('wheel', e => {
      e.preventDefault();
      const r = el.getBoundingClientRect(), px = e.clientX - r.left, py = e.clientY - r.top;
      const z = this.clamp(this.z * Math.exp(-e.deltaY * 0.0015));
      this.fitted = false;
      this.x = px - (px - this.x) * z / this.z; this.y = py - (py - this.y) * z / this.z; this.z = z; this.apply();
    }, { passive: false });
    el.querySelector('.zoomctl').addEventListener('click', e => {
      const z = e.target.dataset.z;
      if (z === '+' || z === '-') { this.fitted = false; this.z = this.clamp(this.z * (z === '+' ? 1.25 : 0.8)); this.apply(); }
      if (z === 'fit') this.fit();
      if (z === 'all') { this.all = !this.all; e.target.classList.toggle('on', this.all); this.light(); }
    });
    // the default view is the whole tree, as large as the view allows; it stays fitted as the view resizes (the
    // detail panel opening or closing) until the user pans or zooms
    this.fit();
    this.light();
    new ResizeObserver(() => { if (this.fitted) this.fit(); }).observe(el);
  },
  apply() { this.world.style.transform = `translate(${this.x}px,${this.y}px) scale(${this.z})`; },
  fitZ() {
    const r = this.el.getBoundingClientRect(), M8 = 12;
    return Math.min((r.width - 2 * M8) / this.bw, (r.height - 2 * M8) / this.bh);
  },
  clamp(z) { return Math.min(3, Math.max(Math.min(0.15, this.fitZ()), z)); },
  fit() {
    const r = this.el.getBoundingClientRect();
    if (!r.width || !r.height) return;
    this.z = Math.min(3, this.fitZ());
    this.x = (r.width - this.bw * this.z) / 2; this.y = (r.height - this.bh * this.z) / 2;
    this.fitted = true; this.apply();
  },
  // the spine node a unit lives under: itself, or the builder that first unlocks it
  home(k) { let x = k; while (x && !this.spine.includes(x)) x = M.units[x].first; return x; },
  select(k, opts = {}) {
    this.light();
    if (k && opts.centre) {
      const h = this.home(k), r = this.el.getBoundingClientRect();
      this.z = Math.max(this.z, 0.7); this.fitted = false;
      this.x = r.width / 2 - (this.col[h] * this.W + 118) * this.z; this.y = r.height / 2 - (this.pos[h] + 40) * this.z; this.apply();
    }
  },
  light() {
    const k = S.sel, h = k && this.home(k);
    const onpath = new Set(h ? pathTo(h) : []);
    const kids = new Set(h ? M.units[h].builds : []);
    const pars = new Set(k ? M.units[k].parents : []);
    this.el.classList.toggle('focus', !!h);
    for (const [n, el] of Object.entries(this.nodes)) {
      el.classList.toggle('sel', n === h); el.classList.toggle('onpath', onpath.has(n) && n !== h);
      el.classList.toggle('iskid', kids.has(n) || pars.has(n));
      el.querySelectorAll('img[data-k]').forEach(i => i.style.outline = i.dataset.k === k ? '3px solid var(--hi)' : '');
    }
    for (const e of this.edges) {
      const path = onpath.has(e.a) && onpath.has(e.b) && !e.cls;
      const kid = h && (e.a === h || (pars.has(e.a) && e.b === h));
      e.p.setAttribute('class', path ? 'path' : kid ? 'kid' : e.cls || '');
      e.p.style.display = e.cls === 'also' && !this.all && !kid ? 'none' : '';
    }
  },
};

/* ---------------- B: focus ----------------
   The selected unit in the middle; what builds it on the left, what it builds on the right, the walk from the
   commander as breadcrumbs above. Clicking anything makes it the middle. */
const B = {
  mount(host) {
    if (!S.sel) { S.sel = M.root; syncURL(); }
    const el = document.createElement('div'); el.id = 'B'; host.append(el); this.el = el;
    el.addEventListener('click', e => { const c = e.target.closest('[data-k]'); if (c) select(c.dataset.k); });
    this.draw();
  },
  select() { this.draw(); },
  draw() {
    const k = S.sel || M.root, u = M.units[k];
    const crumbs = pathTo(k).map((x, i, a) => i === a.length - 1 ? `<b>${esc(M.units[x].title)}</b>` : `<a data-k="${x}">${esc(M.units[x].title)}</a>`).join('<span>›</span>');
    this.el.innerHTML = `<div class="crumbs">${crumbs}</div>
      <div class="stagewrap">
        <div class="col left"><h3>Built by (${u.parents.length})</h3>${byKind(u.parents).map(([g, ks]) => `<div class="grp"><div class="kind">${g}</div>${ks.map(x => chip(x)).join('')}</div>`).join('') || '<p class="kind">The root: every game starts with it.</p>'}</div>
        <div class="centre"><img src="${pic(k)}" alt=""><b>${esc(u.title)}</b><p>${esc(u.description)}</p>
          <span class="kind">${u.kind} · tier ${u.tier} · ${fmt(u.metal)} metal · ${fmt(u.energy)} energy</span>
          ${u.upgrades.length ? `<div class="up"><div class="kind" style="color:var(--up)">Upgrades</div>${u.upgrades.map(x => chip(x)).join('')}</div>` : ''}</div>
        <div class="col right"><h3>Builds (${u.builds.length})</h3>${byKind(u.builds).map(([g, ks]) => `<div class="grp"><div class="kind">${g}</div>${ks.map(x => chip(x)).join('')}</div>`).join('') || '<p class="kind">Builds nothing.</p>'}</div>
      </div>`;
  },
};

/* ---------------- C: build menus ----------------
   Miller columns that follow the game's own build menus: the commander's menu, then the menu of whatever builder is
   opened in it, and so on to the right. A builder carries an arrow. */
const C = {
  trail: null,
  mount(host) {
    const el = document.createElement('div'); el.id = 'C'; host.append(el); this.el = el;
    if (!this.trail || this.trail[0] !== M.root || !this.trail.every(k => M.units[k])) this.trail = [M.root];
    el.addEventListener('click', e => {
      const c = e.target.closest('[data-k]'); if (!c) return;
      const k = c.dataset.k, at = +c.closest('.mcol').dataset.i;
      if (M.units[k].builds.length || M.units[k].upgrades.length) this.trail = [...this.trail.slice(0, at + 1), k];
      select(k);
    });
    if (S.sel) this.reveal(S.sel);
    this.draw();
  },
  reveal(k) {
    // open the walk's path to k, unless k already sits in an open menu
    const open = this.trail.some(t => M.units[t].builds.includes(k) || M.units[t].upgrades.includes(k) || t === k);
    if (!open) { const p = pathTo(k); this.trail = M.units[k].builds.length ? p : p.slice(0, -1); }
  },
  select(k, opts) { if (k && opts?.centre) this.reveal(k); this.draw(); },
  draw() {
    this.el.innerHTML = this.trail.map((t, i) => {
      const u = M.units[t];
      const groups = byKind([...u.builds, ...u.upgrades]);
      return `<div class="mcol" data-i="${i}"><div class="hd"><img class="pic" src="${pic(t)}" alt=""><div><b>${esc(u.title)}</b>
        <div class="kind">${u.builds.length} in its menu</div></div></div><div class="bd">${groups.map(([g, ks]) =>
        `<div class="kind">${g}</div><div class="menu">${ks.map(x => chip(x, this.trail[i + 1] === x ? 'open' : '').replace('</b></span>',
          `</b>${M.units[x].builds.length ? '<span class="more">›</span>' : ''}</span>`)).join('')}</div>`).join('')}</div></div>`;
    }).join('');
    this.el.scrollLeft = this.el.scrollWidth;
  },
};

/* ---------------- D: tier atlas ----------------
   Every unit once, on a grid: rows are what it is, columns its tier. Selecting one lights who builds it (blue) and
   what it builds (green) everywhere on the grid. */
const D = {
  mount(host) {
    const el = document.createElement('div'); el.id = 'D'; host.append(el); this.el = el;
    const tiers = [...new Set(M.order.map(k => M.units[k].tier))].sort((a, b) => a - b);
    const cell = {};
    for (const k of M.order) (cell[M.units[k].kind + '|' + M.units[k].tier] ||= []).push(k);
    const rows = KINDS.filter(g => tiers.some(t => cell[g + '|' + t]));
    el.innerHTML = `<table><tr><th></th>${tiers.map(t => `<th>Tier ${t}</th>`).join('')}</tr>${rows.map(g =>
      `<tr><th class="row">${g}</th>${tiers.map(t => `<td>${(cell[g + '|' + t] || []).map(k => {
        const u = M.units[k];
        return `<span class="tile ${u.upgrade ? 'upg' : ''}" data-k="${k}"><img src="${pic(k)}" loading="lazy" alt="">${u.upgrades.length ? `<span class="upn">⬆${u.upgrades.length}</span>` : ''}</span>`;
      }).join('')}</td>`).join('')}</tr>`).join('')}</table><div class="tip" hidden></div>`;
    const tip = el.querySelector('.tip');
    el.addEventListener('mousemove', e => {
      const t = e.target.closest('.tile');
      tip.hidden = !t;
      if (t) { const u = M.units[t.dataset.k]; tip.innerHTML = `<b>${esc(u.title)}</b> — ${esc(u.description)}`; tip.style.left = e.clientX + 14 + 'px'; tip.style.top = e.clientY + 12 + 'px'; }
    });
    el.addEventListener('click', e => { const t = e.target.closest('.tile'); select(t ? t.dataset.k : null); });
    this.light();
    if (S.sel) el.querySelector(`.tile[data-k="${S.sel}"]`)?.scrollIntoView({ block: 'center', inline: 'center' });
  },
  select(k, opts) { this.light(); if (k && opts?.centre) this.el.querySelector(`.tile[data-k="${k}"]`)?.scrollIntoView({ block: 'center', inline: 'center', behavior: 'smooth' }); },
  light() {
    const k = S.sel, u = k && M.units[k];
    const par = new Set(u ? u.parents : []), kid = new Set(u ? [...u.builds, ...u.upgrades] : []);
    this.el.classList.toggle('focus', !!k);
    this.el.querySelectorAll('.tile').forEach(t => {
      const x = t.dataset.k;
      t.classList.toggle('sel', x === k); t.classList.toggle('par', par.has(x)); t.classList.toggle('kid', kid.has(x));
    });
  },
};
const VIEWS = { A, B, C, D };

/* ---------------- controls ---------------- */
$('#mod').value = S.mod;
$('#mod').onchange = e => { S.mod = e.target.value; S.side = 0; S.sel = null; C.trail = null; load(); };
$('#side').onchange = e => { S.side = +e.target.value; S.sel = null; C.trail = null; load(); };
$('#find').addEventListener('change', e => {
  const v = e.target.value.trim().toLowerCase();
  const k = M.order.find(k => M.units[k].title.toLowerCase() === v) || M.order.find(k => M.units[k].title.toLowerCase().includes(v) || k.includes(v));
  if (k) select(k, { centre: true });
  e.target.value = '';
});
const keys = Object.keys(VARIANTS);
const cycle = d => { S.variant = keys[(keys.indexOf(S.variant) + d + keys.length) % keys.length]; render(); };
$('#sw-prev').onclick = () => cycle(-1);
$('#sw-next').onclick = () => cycle(1);
addEventListener('keydown', e => {
  if (e.target.closest('input, textarea, select, [contenteditable]')) return;
  if (e.key === 'ArrowLeft') cycle(-1);
  if (e.key === 'ArrowRight') cycle(1);
  if (e.key === 'Escape') select(null);
});
load();
