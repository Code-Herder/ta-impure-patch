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
const page = more => ({ tab: 0, updates: 0, selected: -1, row: [], empty: '', title: '', meta: '', summary: '', note: '', opt: [], held: '', confirm: '', update_all: 0, ...more });

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
  confirm: 'Play Total Mayhem',
};
/* ---- the showcase look: the mod's own logo, its authors, its website and a description, for the two mods offered for now ---- */
/* The logos are art like every other part: art/mods/logos/, packed into the DLL with the rest of art/mods/ (WIKIPAK); nothing is
   read from the web. A missing file shows the mod's name instead. */
const LOGOS = ART + 'mods/logos/', BANNERS = ART + 'mods/banners/';
const SHOW = {
  mayhem: {
    /* the text is the author's, as given to the owner on 2026-11-08 */
    name: 'Total Mayhem', logo: 'mayhem.png', banner: 'mayhem.png', version: 'Version 11.3.0', by: 'By gamma', site: 'https://mayhem.tauniverse.com/',
    about: [
      'Mayhem, or Total Mayhem, is a mod for the game Total Annihilation.',
      'Total Mayhem takes Total Annihilation, bugfixes and extends it the way Cavedog might have. All units are useful while retaining original TA gameplay. Many new units added (longtime players will recognise some 3rd-party units), including a whole new 3rd tech level tier.. Existing and added units have been bugfixed, rebalanced, re-modeled, re-textured and re-rendered to equal or exceed the standard of the original TA units',
      { section: 'TOTAL MAYHEM\'s features include:' },
      { list: ['An extended builder interface with 12 buttons per page instead of the usual 6, with hotkey overlays',
        'Lots of UX and ease-of-use improvements including hotkeys and shortcut keys',
        'Loads of bugfixes that prevent lockups and crashes, improve unit AI and improve game performance, meaning more units ingame with less lag',
        'endeavors to make all the units useful, open up more strategic options without removing any existing ones',
        'LOADS of added units, including a complete Level 3 tier: experimental kbots, tanks, navy and air, mobile fortresses, nuke bombers, nuke cannons and more'] },
      { section: 'gamma, @gammata, is the main author. Contributors include:' },
      { list: ['thaldren (TA ddraw patch dev)',
        'keeper, or mnhebi - colour mod and music player dev',
        'm1garland - has contributed many 3D models',
        'archdragon - has contributed a number of 3D models',
        'plasmoidthunder - several unit pictures/build button art'] },
      'Credit also to many other unitmakers in the past whose creations many units are based on. The SYs, Xon, Rime and Xpoy for dev work on the TA recorder, the \"megamap\" and many other added features to TA, that thaldren has continued to build on (and rewrite). ProTA, Venom and the team for their balancing work that I have used as a reference point. The team who have helped playtest and give balance feedback on the Mayhem Gang Discord server',
      { link: ['See the #total-mayhem channel on the TA Universe Discord server: ', 'https://discord.gg/Z4hJpVbmyW'] }],
  },
  prota: {
    /* About is the owner's text of 2026-11-08; Mission and Credits sum up the site's pages of those names in our own words and
       link to them for the full text */
    name: 'ProTA', logo: 'prota.png', banner: 'prota.png', version: 'Version 4.8', by: 'By TAG_Venom and the ProTA team', site: 'https://prota.tauniverse.com/',
    pages: [
      { name: 'About', about: [
        'A veteran-led rebalance that makes more units matter while preserving the speed, asymmetry, and battlefield chaos that define classic TA.',
        { section: 'Creators:' },
        { list: ['TAG_Venom', 'TAG_ROCK', 'PRO_rANDY', 'Tiptushi', 'TAG_Metn', 'timmyfred', 'TAG_Sensation', 'FFAxPostal', 'TAG_Dominator',
          'Gnug215', 'OZ_coolnick', 'BTU_Natas', 'TAG_Mastah', 'Gnug115'] }] },
      { name: 'Mission', about: [
        'ProTA sets out to give every unit a reason to be built without changing what Total Annihilation is. The two aims pull against '
          + 'each other, and the mod looks for the ground between them.',
        { section: 'Its eight aims:' },
        { list: ['Every unit useful: each one gets a real job, widening the choices without diluting the game.',
          'The original feel: the pace and the classic strategies stay.',
          'Cheap and quick: a weak unit is made cheaper or stronger, almost never dearer, so armies stay replaceable and the fight over wreck fields stays at the centre.',
          'Familiar units left alone: the units players use most change as little as possible.',
          'Arm and Core asymmetric: the two sides differ, and both stay viable on every map.',
          'Built on experience: balance rests on the games of long-time high-level players; anyone\'s feedback counts when it fits the mod\'s goals and comes with evidence or replays.',
          'Games over spreadsheets: what a change does in live matches weighs more than numbers read from the game files.',
          'A better interface: new hotkeys and a longer build menu, within what the engine allows.'] },
        'The team hopes to make the TA mod that newcomers and veterans alike call their own.',
        { link: ['The full text: ', 'https://prota.tauniverse.com/mission.html'] }] },
      { name: 'Credits', about: [
        'Fine-tuned and play-tested by the players listed under About. The mod was also made possible by:',
        { list: ['gamma: scripting, balance discussions, and Total Mayhem\'s scripts, fixes and models, used with permission',
          'Switeck: many fixes taken from TA Bugfix',
          'ArmouredFish: scripting help and fixes such as sea wrecks that no longer block',
          'Vohvelieläin: model and script editing',
          'Axle1975: recorder fixes, taforever.com, and engine fixes and features',
          'FunkyFr3sh: CnC-DDraw adapted for TA on modern systems, and engine fixes',
          'poosticks: a TA guide used to check unit statistics',
          'Idec Sdawkminn: the TAU guides to unit FBI files and to weapons',
          'zwzsg: the TAU posts behind the non-blocking sea wrecks',
          'SnakeInTheMirror: the colour enhancement pack\'s orange and teal team colours, and a better yellow',
          'PlasmoidThunder: sound category bugs and other findings',
          'TAAN: Evolva models, some used as they are and some reworked',
          'ACE_Beeky: a TAU guide to every unit in TA',
          'Wotan: showed the sceptics what modding could do',
          'Dragun101: promotion, and ProTA in SCTA showmatch streams',
          'Postal: the Total Annihilation Replays YouTube channel, Core Commander knowledge and balance discussions',
          'timmyfred: testing fixes, playtesting and balance discussions',
          'TAG_ROCK: showcasing the gameplay, long playtesting, and funding new 3D models',
          'Tristan: the new units\' 3D models, in Cavedog\'s style'] },
        'And everyone else: the players, casters, map makers, feedback givers and content creators.',
        { link: ['The full text: ', 'https://prota.tauniverse.com/credits.html'] }] }],
  },
};
const PLAY = [opt('Resource snapping radius', '3'), opt('Quick Build', 'On')];
const SHOWCASE_MANAGE = {
  tab: 0, updates: 0, selected: 1, show: SHOW.mayhem,
  row: [row('This folder', 'As it is on disk', BADGE.NONE, '', ACT.RADIO),
        row('Total Mayhem', 'Version 11.3.0', BADGE.ACTIVE, 'Active', ACT.RADIO_ON)],
  opt: PLAY,
};
const SHOWCASE_EXPLORE = {
  tab: 1, updates: 0, selected: 0, show: SHOW.prota,
  row: [row('ProTA', 'Version 4.8, 6 MB', BADGE.NONE, '', ACT.INSTALL)],
  opt: PLAY,
};
const W = (n, broken) => Array.from({ length: n }, (_, i) => broken && i % 2 && i < n - 1 ? ' ' : 'W').join('');
/* tagpu_modslay.c `fullest`: every array full, every text as long as its array holds, a word that cannot break or the most lines */
const fullest = (broken, locked) => ({
  tab: 2, updates: 999, selected: 0, update_all: 1,
  row: Array.from({ length: 16 }, (_, i) => row(W(47, broken), W(47, broken), 1 + i % 4, W(15, broken), 1 + i % 5, 500)),
  title: W(47, broken), meta: W(119, broken), summary: W(399, broken), note: W(199, broken), confirm: W(47, broken),
  opt: Array.from({ length: 8 }, () => opt(W(47, broken), W(15, broken), { reason: W(119, broken), locked: +locked })),
});
const STATES = {
  manage: ['Manage: a selected mod and its play options', MANAGE],
  mutators: ['Manage: a base mod with its mutators, one stale', MUTATORS],
  explore: ['Explore: an Install, a busy row, a failure note', EXPLORE],
  updates: ['Updates: a count and Update all', UPDATES],
  empty: ['An empty list', EMPTY],
  long: ['A long summary, a locked option', SCROLLS],
  showcase_mayhem: ['Showcase: Total Mayhem, installed and running', SHOWCASE_MANAGE],
  showcase_prota: ['Showcase: ProTA, to install from Explore', SHOWCASE_EXPLORE],
  fullest: ['The fullest page, words that cannot break', fullest(false, false)],
  fullest_broken: ['The fullest page, the most lines', fullest(true, false)],
  fullest_locked: ['The fullest page, all locked', fullest(false, true)],
};

/* ---- the state, from the URL ---- */
const RES = [...$('#res').options].map(o => o.value);
const S = { res: '1920x1080', state: 'manage', held: '', page: 0 };
{ const q = new URLSearchParams(location.search);
  if (RES.includes(q.get('res'))) S.res = q.get('res');
  if (q.get('state') in STATES) S.state = q.get('state');
  if (q.get('held')) S.held = q.get('held');
  S.page = Math.max(0, parseInt(q.get('page')) || 0); }
const P = () => page(STATES[S.state][1]);
function syncURL() {
  const q = new URLSearchParams({ res: S.res, state: S.state });
  if (S.held) q.set('held', S.held);
  if (S.page) q.set('page', S.page);
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

/* A play option: the mod decides it, so the row shows its value and takes no click; nothing on this screen is a game setting.
   A locked row's disc lies over the space .sp keeps for it. */
function optRow(o) {
  const inner = (o.locked ? '<span class="sp"></span>' : '') + text2(o.label, o.locked ? o.reason : '', 'l', 'r')
    + `<div class="ctl"><div class="v">${esc(o.value)}</div></div>`;
  const row = `<div class="opt" data-part="row">${inner}</div>`;
  return o.locked ? `<div class="lockrow">${row}<i class="lock"></i></div>` : row;
}
/* The showcase head: the logo centred, then the version and authors, the website (a click opens the player's browser), the description */
function showcase(m) {
  const host = new URL(m.site).host;
  return `<div class="logo"><img src="${LOGOS + m.logo}" alt="${esc(m.name)}" onerror="this.replaceWith(Object.assign(document.createElement('h2'), {className: 'pt', textContent: this.alt}))"></div>`
    + `<div class="pa m c">${esc(m.version)} · ${esc(m.by)}</div>`
    + `<div class="pa m c"><a class="lnk" href="${m.site}" target="_blank" rel="noopener" data-key="link">${esc(host)}</a></div>`
    + `<div class="rule"></div>`
    + (m.banner ? `<div class="banner"><img src="${BANNERS + m.banner}" alt="" onerror="this.parentNode.remove()"></div>` : '')
    + (m.pages ? pageTabs(m.pages) + blocks(m.pages[Math.min(S.page, m.pages.length - 1)].about) : blocks(m.about));
}
/* A mod with more to say than a description splits it into pages, picked by a row of tabs under the banner */
const pageTabs = pages => `<div class="ptabs">${pages.map((g, i) =>
  `<button class="tab" data-key="page:${i}" data-part="${i === Math.min(S.page, pages.length - 1) ? 'tab_selected' : 'tab'}">${esc(g.name)}</button>`).join('')}</div>`;
/* The text blocks: a paragraph, a section's opening line, a list, a line ending in a link */
const blocks = about => about.map(b => typeof b === 'string' ? `<div class="pa s">${esc(b)}</div>`
  : b.section ? `<div class="pa s sec">${esc(b.section)}</div>`
  : b.list ? `<ul class="pa s lst">${b.list.map(t => `<li>${esc(t)}</li>`).join('')}</ul>`
  : b.link ? `<div class="pa s">${esc(b.link[0])}<a class="lnk" href="${b.link[1]}" target="_blank" rel="noopener" data-key="link">${esc(b.link[1].replace(/^https:\/\//, ''))}</a></div>` : '').join('');
function detail(p) {
  let h = '';
  if (p.show) return showcase(p.show) + (p.opt.length ? `<div class="hdr">Play options</div>` + p.opt.map(optRow).join('') : '');
  if (p.title) h += `<h2 class="pt">${esc(p.title)}</h2>`;
  if (p.meta) h += `<div class="pa m">${esc(p.meta)}</div>`;
  if (p.summary) h += `<div class="pa s">${esc(p.summary)}</div>`;
  if (p.note) h += `<div class="pa nt">${esc(p.note)}</div>`;
  if (p.opt.length) h += `<div class="hdr">Play options</div>` + p.opt.map(optRow).join('');
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
  option: ['#win .l'], reason: ['#win .r'], value: ['#win .v'], footer: ['#win .fb'],
};
UIKit.layoutVars(frame, LAY);
const STYLES = fetch(ART + 'hud/layout.json').then(r => r.json()).then(L => UIKit.roleStyles(ROLE, LAY.roles, L.styles));

/* ---- the controls strip, and what a click says ---- */
$('#state').innerHTML = Object.entries(STATES).map(([k, [t]]) => `<option value="${k}">${esc(t)}</option>`).join('');
$('#state').value = S.state; $('#state').onchange = e => { S.state = e.target.value; S.page = 0; render(); syncURL(); };
$('#res').value = S.res; $('#res').onchange = e => { S.res = e.target.value; layout(); syncURL(); };
$('#held').value = S.held; $('#held').onchange = e => { S.held = e.target.value.trim(); render(); syncURL(); };
/* a showcase's page tabs are the one click the mockup follows, so each page can be read */
frame.addEventListener('click', e => {
  const k = e.target.closest('[data-key]');
  $('#last').textContent = k ? `clicked ${k.dataset.key}` : '';
  if (k && k.dataset.key.startsWith('page:')) { S.page = +k.dataset.key.slice(5); render(); syncURL(); }
});
window.addEventListener('resize', layout);

layout(); render();
const KIT = await UIKit.loadKit(ART + 'wiki/kit/');
await STYLES;
(function loop() { UIKit.paintParts(KIT, 'kit', G.gh / 720, G.k); requestAnimationFrame(loop); })();

/* for scripted checks: the page as drawn, in MODSPAGE's fields; set() changes the state and draws it */
window.ModsPage = { state: S, page: P, set(k, v) { S[k] = v; render(); }, ready: true };
})();
