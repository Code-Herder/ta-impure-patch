/* mods.js -- the Mods screen's mockup: the page as the game gives it (tagpu_modslay.h's MODSPAGE, field for field), drawn from
   a fixture of states. It lays out and draws only; what a click does is the game's (tagpu_modsui.c), so a click here says the
   control's key. tools/mods/modslaycheck.py reads window.ModsPage and pairs what this draws with tagpu_modslay.c. */
(async () => {
const $ = s => document.querySelector(s);
const ART = '../../../art/';
const LAY = await fetch(ART + 'mods/layout.json').then(r => { if (!r.ok) throw new Error(`layout.json: ${r.status}`); return r.json(); });
const esc = s => String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;');

/* tagpu_modslay.h's enums */
const BADGE = { NONE: 0, ACTIVE: 1, UPDATE: 2, GREY: 3, CONFLICT: 4 };
const ACT = { NONE: 0, RADIO: 1, RADIO_ON: 2, INSTALL: 3, UPDATE: 4, BUSY: 5, CHECK: 6, CHECK_ON: 7 };
const BADGE_PART = ['', 'badge_active', 'badge_update', 'badge_grey', 'badge_conflict'];
const TAB = ['Manage', 'Explore', 'Updates'];

/* ---- the fixture ---- */
const row = (name, detail, badge, badge_text, act, progress = -1) => ({ name, detail, badge, badge_text, act, progress });
/* a play option: the mod's value in force, shown and never set here */
const opt = (label, value, more = {}) => ({ label, value, reason: '', locked: 0, ...more });
const sw = (name, on, more = {}) => ({ name, on: +on, changed: 0, locked: 0, ...more });
const page = more => ({ sw_head: 'Game archives', tab: 0, updates: 0, selected: -1, row: [], empty: '', title: '', meta: '', summary: '', note: '', opt: [], sw: [], held: '', confirm: '', update_all: 0, ...more });

const SHORT = 'Total Mayhem rebalances every unit of both sides and adds several hundred of its own, from cheap scouts to walking fortresses.';
const LONG = SHORT + ' Economies run larger and battles run longer than the original game allows, so a skirmish that took half an hour can '
  + 'hold an evening. It keeps the campaign maps playable and adds its own. A game of it with a stock player is refused.';

const MANAGE = {
  tab: 0, updates: 2, selected: 1,
  row: [row('This folder', 'As it is on disk', BADGE.ACTIVE, 'Active', ACT.RADIO_ON),
        row('Total Mayhem', 'Version 8.1', BADGE.UPDATE, 'Update', ACT.RADIO),
        row('ProTA', 'Version 4.6', BADGE.NONE, '', ACT.RADIO),
        row('TA Twilight', 'Needs the managed patch', BADGE.CONFLICT, 'Conflict', ACT.RADIO),
        row('Escalation', 'Not installed', BADGE.GREY, 'Absent', ACT.NONE)],
  title: 'Total Mayhem', meta: 'By its authors, version 8.1', summary: SHORT,
  note: 'Confirm restarts the game with this mod.',
  opt: [opt('Resource snapping radius', '3'), opt('Quick Build', 'On')],
  sw: [sw('totala1.hpi', 1, { locked: 1 }), sw('totala2.hpi', 1, { locked: 1 }), sw('btdata.ccx', 1), sw('ccdata.ccx', 0, { changed: 1 }), sw('ccmaps.ccx', 1)],
  confirm: 'Play Total Mayhem',
};
const MUTATORS = {
  tab: 0, updates: 1, selected: 3,
  row: [row('This folder', 'As it is on disk', BADGE.NONE, '', ACT.RADIO),
        row('Total Mayhem', 'Version 8.1', BADGE.ACTIVE, 'Active', ACT.RADIO_ON),
        row('Commander skins', 'Mutator, version 1.2', BADGE.NONE, '', ACT.CHECK_ON),
        row('Old commander', 'Mutator, version 0.9', BADGE.GREY, 'Stale', ACT.NONE),
        row('Other skins', 'Mutator, version 2.0', BADGE.CONFLICT, 'Conflict', ACT.NONE),
        row('Quieter guns', 'Mutator, version 1.0', BADGE.NONE, '', ACT.CHECK)],
  title: 'Old commander', meta: 'By its author, version 0.9', summary: 'A plainer commander for both sides.',
  note: 'Needs an update for Total Mayhem: objects3d\\armcom.3do differs from the file it was built on.',
  sw_head: 'Files',
  sw: [sw('objects3d\\armcom.3do', 1), sw('unitpics\\armcom.pcx', 0, { changed: 1 }), sw('Hides anims\\armcom.gaf', 1)],
};
const EXPLORE = {
  tab: 1, updates: 0, selected: 2,
  row: [row('ProTA', 'Version 4.6, 180 MB', BADGE.NONE, '', ACT.INSTALL),
        row('TA Zero', 'Downloading, 64 of 210 MB', BADGE.NONE, '', ACT.BUSY, 305),
        row('TA Twilight', 'Version 2.1, 340 MB', BADGE.GREY, 'Failed', ACT.INSTALL),
        row('Escalation', 'Unpacking', BADGE.NONE, '', ACT.BUSY, -1),
        row('Total Mayhem', 'Installed', BADGE.ACTIVE, 'Installed', ACT.NONE)],
  title: 'TA Twilight', meta: 'By its authors, version 2.1', summary: 'A new side and a long campaign for it, built on the original game.',
  note: 'The download stopped: the file did not match its hash. Nothing was changed; press Install to try again.',
};
const UPDATES = {
  tab: 2, updates: 3, selected: 0,
  row: [row('Total Mayhem', '8.1 to 8.2', BADGE.UPDATE, 'Update', ACT.UPDATE),
        row('ProTA', '4.6 to 4.7', BADGE.UPDATE, 'Update', ACT.UPDATE),
        row('TA Zero', '1.2 to 1.3, downloading', BADGE.UPDATE, 'Update', ACT.BUSY, 720)],
  title: 'Total Mayhem', meta: 'Version 8.2', summary: 'Balance changes to the air units and three new maps. Your saved options are kept.',
  update_all: 1,
};
const EMPTY = { tab: 1, updates: 0, selected: -1, row: [], empty: 'No mods to show. The catalog could not be read; check your connection and open this screen again.' };
const SCROLLS = {
  tab: 0, updates: 0, selected: 0,
  row: [row('Total Mayhem', 'Version 8.1', BADGE.ACTIVE, 'Active', ACT.RADIO_ON)],
  title: 'Total Mayhem', meta: 'By its authors', summary: LONG, note: 'Confirm restarts the game with this mod.',
  opt: [opt('Resource snapping radius', '3'), opt('Quick Build', 'Off', { locked: 1, reason: 'This mod does not allow it.' })],
  sw: Array.from({ length: 24 }, (_, i) => sw(`archive${i}.ufo`, i % 3 !== 0, { changed: +(i % 7 === 0) })),
  confirm: 'Play Total Mayhem',
};
const W = (n, broken) => Array.from({ length: n }, (_, i) => broken && i % 2 && i < n - 1 ? ' ' : 'W').join('');
/* tagpu_modslay.c `fullest`: every array full, every text as long as its array holds, a word that cannot break or the most lines */
const fullest = (broken, locked) => ({
  tab: 2, updates: 999, selected: 0, update_all: 1,
  row: Array.from({ length: 16 }, (_, i) => row(W(47, broken), W(47, broken), 1 + i % 4, W(15, broken), 1 + i % 5, 500)),
  title: W(47, broken), meta: W(119, broken), summary: W(399, broken), note: W(199, broken), confirm: W(47, broken),
  opt: Array.from({ length: 8 }, () => opt(W(47, broken), W(15, broken), { reason: W(119, broken), locked: +locked })),
  sw: Array.from({ length: 40 }, () => sw(W(31, broken), 0, { changed: 1, locked: +locked })),
});
const STATES = {
  manage: ['Manage: a selected mod, a locked option, a changed option', MANAGE],
  mutators: ['Manage: a base mod with its mutators, one stale and its files', MUTATORS],
  explore: ['Explore: an Install, a busy row, a failure note', EXPLORE],
  updates: ['Updates: a count and Update all', UPDATES],
  empty: ['An empty list', EMPTY],
  long: ['A long summary that scrolls the pane', SCROLLS],
  fullest: ['The fullest page, words that cannot break', fullest(false, false)],
  fullest_broken: ['The fullest page, the most lines', fullest(true, false)],
  fullest_locked: ['The fullest page, all locked', fullest(false, true)],
};

/* ---- the state, from the URL ---- */
const RES = [...$('#res').options].map(o => o.value);
const S = { res: '1920x1080', state: 'manage', held: '' };
{ const q = new URLSearchParams(location.search);
  if (RES.includes(q.get('res'))) S.res = q.get('res');
  if (q.get('state') in STATES) S.state = q.get('state');
  if (q.get('held')) S.held = q.get('held'); }
const P = () => page(STATES[S.state][1]);
function syncURL() {
  const q = new URLSearchParams({ res: S.res, state: S.state });
  if (S.held) q.set('held', S.held);
  history.replaceState(null, '', '?' + q);
}

/* ---- the frame: the resolution fitted into the browser at its own aspect; G.k is the resolution's pixels a CSS pixel ---- */
const frame = $('#frame'), G = { k: 1, gh: 720 };
function layout() {
  const dpr = window.devicePixelRatio || 1;
  const vw = document.documentElement.clientWidth - 34, vh = Math.max(240, innerHeight - $('#controls').offsetHeight - 50);
  let gw, gh, cw, ch;
  if (S.res === 'fit') { cw = vw; ch = vh; gw = Math.round(cw * dpr); gh = Math.round(ch * dpr); }
  else { [gw, gh] = S.res.split('x').map(Number); const k = Math.min(vw / gw, vh / gh); cw = Math.floor(gw * k); ch = Math.floor(gh * k); }
  G.gh = gh; G.k = gw / cw;
  frame.style.width = cw + 'px'; frame.style.height = ch + 'px';
  frame.style.setProperty('--u', ch / 720 + 'px');
}

/* ---- the screen: each rule's boxes, in tagpu_modslay.c's order ---- */
const on = (key, p) => key === p.held ? ' on' : '';
const tabs = p => TAB.map((t, i) => {
  const key = `tab:${i}`, label = i === 2 && p.updates > 0 ? `${t} (${p.updates})` : t;
  return `<button class="tab${on(key, p)}" data-key="${key}" data-part="${i === p.tab && key !== p.held ? 'tab_selected' : 'tab'}">${esc(label)}</button>`;
}).join('');
const button = (cls, key, label, p) => `<button class="${cls}${key ? on(key, p) : ''}" ${key ? `data-key="${key}"` : ''} data-part="button">${esc(label)}</button>`;
const text2 = (a, b, ca, cb) => `<div class="t">${a ? `<div class="${ca}">${esc(a)}</div>` : ''}${b ? `<div class="${cb}">${esc(b)}</div>` : ''}</div>`;

function action(r, i, p) {
  const key = `act:${i}`;
  switch (r.act) {
    case ACT.RADIO: case ACT.RADIO_ON:
      return `<i class="radio" data-key="${key}" data-part="${r.act === ACT.RADIO_ON || key === p.held ? 'radio_on' : 'radio_off'}"></i>`;
    case ACT.CHECK: case ACT.CHECK_ON:
      return `<i class="radio" data-key="${key}" data-part="${r.act === ACT.CHECK_ON || key === p.held ? 'check_on' : 'check_off'}"></i>`;
    case ACT.INSTALL: return button('ab', key, 'Install', p);
    case ACT.UPDATE: return button('ab', key, 'Update', p);
    case ACT.BUSY:
      return r.progress >= 0 ? `<i class="track" data-part="progress_track"><i class="fill" data-part="progress_fill" style="width:calc((100% - 2 * var(--w-progress-inset) * var(--u)) * ${r.progress / 1000})"></i></i>` : '';
    default: return '';
  }
}
const listRow = (r, i, p) => `<div class="lrow${i === p.selected || `row:${i}` === p.held ? ' sel' : ''}" data-key="row:${i}" data-part="row">`
  + text2(r.name, r.detail, 'n', 'd')
  + (r.badge ? `<div class="bg" data-part="${BADGE_PART[r.badge]}">${esc(r.badge_text)}</div>` : '')
  + `<div class="act">${action(r, i, p)}</div></div>`;

/* A row of the pane: `ctl` its control's markup; a locked row's disc lies over the space .sp keeps for it */
function paneRow(cls, label, reason, locked, changed, resetKey, ctl, p) {
  const inner = (locked ? '<span class="sp"></span>' : '') + text2(label, locked ? reason : '', cls, 'r')
    + (changed && !locked ? `<i class="mark"></i>${button('rb', resetKey, 'Reset', p)}` : '') + ctl;
  const opt = `<div class="opt" data-part="row">${inner}</div>`;
  return locked ? `<div class="lockrow">${opt}<i class="lock"></i></div>` : opt;
}
const value = o => `<div class="v">${esc(o.value)}</div>`;
const optRow = (o, i, p) => paneRow('l', o.label, o.reason, o.locked, 0, '', `<div class="ctl">${value(o)}</div>`, p);
function swRow(s, i, p) {
  const live = !s.locked;
  const ctl = `<div class="ctl${live ? ' sw' : ''}" ${live ? `data-key="sw:${i}"` : ''}><i class="check" data-part="${s.on || `sw:${i}` === p.held ? 'check_on' : 'check_off'}"></i></div>`;
  return paneRow('a', s.name, '', s.locked, s.changed, `swreset:${i}`, ctl, p);
}
function detail(p) {
  let h = '';
  if (p.title) h += `<h2 class="pt">${esc(p.title)}</h2>`;
  if (p.meta) h += `<div class="pa m">${esc(p.meta)}</div>`;
  if (p.summary) h += `<div class="pa s">${esc(p.summary)}</div>`;
  if (p.note) h += `<div class="pa nt">${esc(p.note)}</div>`;
  if (p.opt.length) h += `<div class="hdr">Play options</div>` + p.opt.map((o, i) => optRow(o, i, p)).join('');
  if (p.sw.length) h += `<div class="hdr">${esc(p.sw_head)}</div>` + p.sw.map((s, i) => swRow(s, i, p)).join('');
  return h;
}
function foot(p) {
  const key = p.confirm ? 'confirm' : '';
  return button('fb', 'back', 'Back', p)
    + (p.update_all ? button('fb right', 'updateall', 'Update all', p) : '')
    + `<button class="fb${p.update_all ? '' : ' right'}${key ? on(key, p) : ''}" ${key ? `data-key="${key}"` : 'disabled'} data-part="button">${esc(p.confirm || 'Confirm')}</button>`;
}

function render() {
  const p = P();
  p.held = S.held;
  $('#wx').classList.toggle('on', p.held === 'close');
  $('#tabs').innerHTML = tabs(p);
  const list = $('#list');
  list.classList.toggle('none', !p.row.length);
  list.innerHTML = p.row.length ? p.row.map((r, i) => listRow(r, i, p)).join('') : (p.empty ? `<div class="empty">${esc(p.empty)}</div>` : '');
  $('#detail').innerHTML = detail(p);
  $('#foot').innerHTML = foot(p);
}

/* ---- text: the elements of each text role (uikit.js roleStyles) ---- */
const ROLE = {
  window_title: ['#win .wt'], tab: ['#win .tab'], name: ['#win .n'], detail: ['#win .d'], act: ['#win .ab'], badge: ['#win .bg'],
  empty: ['#win .empty'], pane_title: ['#win .pt'], meta: ['#win .m'], summary: ['#win .s'], note: ['#win .nt'], header: ['#win .hdr'],
  option: ['#win .l'], reason: ['#win .r'], value: ['#win .v'], reset: ['#win .rb'], archive: ['#win .a'], footer: ['#win .fb'],
};
UIKit.layoutVars(frame, LAY);
const STYLES = fetch(ART + 'hud/layout.json').then(r => r.json()).then(L => UIKit.roleStyles(ROLE, LAY.roles, L.styles));

/* ---- the controls strip, and what a click says ---- */
$('#state').innerHTML = Object.entries(STATES).map(([k, [t]]) => `<option value="${k}">${esc(t)}</option>`).join('');
$('#state').value = S.state; $('#state').onchange = e => { S.state = e.target.value; render(); syncURL(); };
$('#res').value = S.res; $('#res').onchange = e => { S.res = e.target.value; layout(); syncURL(); };
$('#held').value = S.held; $('#held').onchange = e => { S.held = e.target.value.trim(); render(); syncURL(); };
frame.addEventListener('click', e => { const k = e.target.closest('[data-key]'); $('#last').textContent = k ? `clicked ${k.dataset.key}` : ''; });
window.addEventListener('resize', layout);

layout(); render();
const KIT = await UIKit.loadKit(ART + 'wiki/kit/');
await STYLES;
(function loop() { UIKit.paintParts(KIT, 'kit', G.gh / 720, G.k); requestAnimationFrame(loop); })();

/* for scripted checks: the page as drawn, in MODSPAGE's fields; set() changes the state and draws it */
window.ModsPage = { state: S, page: P, set(k, v) { S[k] = v; render(); }, ready: true };
})();
