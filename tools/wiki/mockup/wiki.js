/* wiki.js -- the in-game wiki's mockup: the game frame and Impure's top bar, the wiki window with its tree, demo list and
   entry, and the stage, where stage.js draws what sim.js's view() says. The content is art/wiki/ (tree.json,
   entries/<id>.json) and the behaviour is sim.js's; this file lays out, draws and forwards input. research/notes/ingame-wiki.md is the
   specification the game copies. An entry with "frame": "game" plays its demos through replay.js on the whole game
   frame (frame.js), with the replay HUD (tools/hudart/mockup/hud.js) on it. */
(async () => {
const $ = s => document.querySelector(s);
const ART = '../../../art/wiki/', BACKDROP = '../../../research/notes/assets/hud-art/backdrop.png';
// the layout's numbers, in units, which the DLL reads too (tagpu_wikilay.c): everything below waits for them
const LAY = await fetch(ART + 'layout.json').then(r => { if (!r.ok) throw new Error(`layout.json: ${r.status}`); return r.json(); });
const TICK = 30;                      // simulation ticks a second, the game's
const MODS = ['stock', 'mayhem', 'prota', 'twilight', 'escalation', 'tazero', 'unknown'];
const RES = [...$('#res').options].map(o => o.value);
const KEYCAP = { shift: 'Shift', alt: 'Alt', ctrl: 'Ctrl', z: 'Z', x: 'X', e: 'E', lmb: 'Left-click', rmb: 'Right-click' };
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const esc = s => String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;');

/* ---- the state, from the URL; the URL is kept up to date so a view can be shared ---- */
const SKINS = ['kit', 'whitebox'];
// the sidebar's layout: the detected mod's pack (the fixture where it has none), a fixture by name, or none at all
const SIDEBARS = ['mod', 'fixture', 'tall', 'off'];
/* side: the side asked for by name, matched without case against the sidebar layout's sides (sideIndex), kept as asked
   so a pack that loads after the page still finds it; null or no match is the layout's first side. iface: the
   Interface Type the demos play, 1 right-click (the interface they are authored in) or 0 left-click. theatre: the
   theatre layout forced on whichever entry is open, for viewing it before an entry declares "frame": "game"; about: the
   theatre's About panel */
const S = { res: '1920x1080', size: LAY.window.share, mode: 'sp', mod: 'stock', side: null, iface: 1, skin: 'kit', sidebar: 'mod', rewind: true,
  open: false, entry: null, demo: 0, try: false, playing: true, speed: 1, fold: new Set(), theatre: false, about: false };
{ const q = new URLSearchParams(location.search);
  if (RES.includes(q.get('res'))) S.res = q.get('res');
  if (q.get('size')) S.size = clamp(Math.round(+q.get('size')) || LAY.window.share, 50, 100);
  if (q.get('mode') === 'mp') S.mode = 'mp';
  if (MODS.includes(q.get('mod'))) S.mod = q.get('mod');
  if (q.get('side')) S.side = q.get('side');
  if (q.get('iface') === '0') S.iface = 0;
  if (q.get('rewind') === '0') S.rewind = false;
  if (SKINS.includes(q.get('skin'))) S.skin = q.get('skin');
  if (SIDEBARS.includes(q.get('sidebar'))) S.sidebar = q.get('sidebar');
  S.open = q.has('open'); S.entry = q.get('entry'); S.try = q.has('try');
  S.theatre = q.has('theatre'); S.about = q.has('about');
  if (q.get('demo')) S.demo = Math.max(0, (+q.get('demo') || 1) - 1); }
function syncURL() {
  const q = new URLSearchParams({ res: S.res, size: S.size, mode: S.mode, mod: S.mod });
  if (S.side) q.set('side', S.side);
  for (const [k, v] of Object.entries({ iface: S.iface, skin: S.skin, sidebar: S.sidebar, rewind: +S.rewind })) q.set(k, v);
  if (S.theatre) q.set('theatre', '');
  if (S.open) { q.set('open', ''); if (S.entry) { q.set('entry', S.entry); q.set('demo', S.demo + 1); } if (S.try) q.set('try', '');
    if (theatre() && S.about) q.set('about', ''); }
  if (window.WikiGfx) WikiGfx.url(q);
  history.replaceState(null, '', '?' + q.toString().replace(/=(?=&|$)/g, ''));
}

/* ---- the frame: the resolution fitted into the browser at its own aspect. G.k is the resolution's pixels a CSS pixel:
   the canvases are drawn at the resolution's pixels, as the game would draw them, and shown at the fitted size ---- */
const frame = $('#frame'), G = { k: 1, cw: 0, ch: 0, gw: 0, gh: 0 };
function layout() {
  const dpr = window.devicePixelRatio || 1;
  const vw = document.documentElement.clientWidth - 34, vh = Math.max(240, innerHeight - $('#controls').offsetHeight - 50);
  let gw, gh, cw, ch;
  if (S.res === 'fit') { cw = vw; ch = vh; gw = Math.round(cw * dpr); gh = Math.round(ch * dpr); }
  else { [gw, gh] = S.res.split('x').map(Number); const k = Math.min(vw / gw, vh / gh); cw = Math.floor(gw * k); ch = Math.floor(gh * k); }
  Object.assign(G, { gw, gh, cw, ch, k: gw / cw });
  frame.style.width = cw + 'px'; frame.style.height = ch + 'px';
  frame.style.setProperty('--u', ch / 720 + 'px');
  frame.style.setProperty('--s', S.size);
  backdrop(); sizeStage();
}

/* the backdrop: the replay HUD mockup's 1280 x 720 frame, its side panel (128 pixels) and bars (32) at the height's
   magnification and its world filling what they leave; the top bar is covered by Impure's, which #topbar draws */
const BD = new Image(); BD.src = BACKDROP;
function backdrop() {
  const cv = $('#bg'), W = G.gw, H = G.gh, k = H / 720;
  cv.width = W; cv.height = H;
  if (!BD.complete || !BD.naturalWidth) { BD.onload = backdrop; return; }
  const g = cv.getContext('2d'); g.fillStyle = '#000'; g.fillRect(0, 0, W, H);
  const wx = 128 * k, wy = 32 * k, ww = W - wx, wh = H - 2 * wy, src = { x: 128, y: 32, w: 1152, h: 656 };
  const c = Math.max(ww / src.w, wh / src.h), sw = ww / c, sh = wh / c;
  g.drawImage(BD, src.x + (src.w - sw) / 2, src.y + (src.h - sh) / 2, sw, sh, wx, wy, ww, wh);
  g.drawImage(BD, 128, 688, 1152, 32, wx, H - wy, ww, wy);
  g.drawImage(BD, 0, 0, 128, 720, 0, 0, wx, H);
}

/* ---- the game behind the window: a clock and two readouts that run unless single player has the wiki open ---- */
const GAME = { t: 754 };
const paused = () => S.open && S.mode === 'sp';
function gameTick(dt) {
  if (!paused()) GAME.t += dt;
  frame.classList.toggle('paused', paused());
  const t = GAME.t, m = 1830 + 900 * Math.sin(t / 23), e = 6100 + 2600 * Math.sin(t / 17 + 1);
  const set = (id, v) => { const el = $(id); if (el.textContent !== v) el.textContent = v; };
  set('#m-v', Math.round(m).toLocaleString('en')); set('#e-v', Math.round(e).toLocaleString('en'));
  $('#m-f s').style.width = (m / 3000 * 100).toFixed(1) + '%'; $('#e-f s').style.width = (e / 10000 * 100).toFixed(1) + '%';
  set('#m-r', '+' + (14.2 + 3 * Math.sin(t / 5)).toFixed(1)); set('#e-r', '+' + Math.round(310 + 60 * Math.sin(t / 7)));
  set('#clock', `${Math.floor(t / 60)}:${String(Math.floor(t % 60)).padStart(2, '0')}`);
}

/* ---- content: the tree, and every entry it names, loaded once ---- */
let TREE = null, treeErr = null;
const ENT = {};                       // id -> the entry, or { error }
const leaves = (ns, out = []) => { for (const n of ns || []) n.entry ? out.push(n.entry) : leaves(n.children, out); return out; };
async function load() {
  try { TREE = await fetch(ART + 'tree.json').then(r => { if (!r.ok) throw new Error(`tree.json: ${r.status}`); return r.json(); }); }
  catch (x) { treeErr = x.message; return; }
  // the entry the URL names is loaded too when the tree does not name it, so an entry can be viewed before it is listed
  const ids = leaves(TREE.tree);
  if (S.entry && /^[a-z0-9_]+$/.test(S.entry) && !ids.includes(S.entry)) ids.push(S.entry);
  await Promise.all(ids.map(id => fetch(`${ART}entries/${id}.json`)
    .then(r => { if (!r.ok) throw new Error(`entries/${id}.json: ${r.status}`); return r.json(); })
    .then(j => { const why = WikiText.checkEntry(j) || WikiSim.checkText(j); ENT[id] = why ? { error: `entries/${id}.json: ${why}` } : j; },
      x => { ENT[id] = { error: x.message }; })));
}
const entry = () => (S.entry && ENT[S.entry] && !ENT[S.entry].error) ? ENT[S.entry] : null;
const demo = () => { const e = entry(); return e && Array.isArray(e.demos) ? e.demos[S.demo] || null : null; };
/* the stage the demo plays on, `<entry>.<scene>`: its packs (gfx.py) and its minimap base are per scene */
const stageId = () => {
  const e = entry(), d = demo();
  if (!e || !e.scenes || typeof e.scenes !== 'object') return null;
  return `${e.id}.${d && typeof d.on === 'string' ? d.on : Object.keys(e.scenes)[0]}`;
};
const locked = e => !!(e && Array.isArray(e.lockedIn) && e.lockedIn.includes(S.mod));
/* every text an entry shows goes through text.js's fillText with the words of the context the sim plays -- the
   side's casting, or the entry's own types without one, and the Interface Type -- and the sim keeps the raw text */
const textWords = e => { const c = context(); return WikiText.words(e, c.casting, c.iface); };
const fill = (e, t) => WikiText.fillText(t, textWords(e));

/* ---- the game's left sidebar: the layout the sims run on and, in the game look, its pictures ----
   The detected mod's layout comes from its pack, which tools/wiki/gfx.py resolves from the player's install into the
   gitignored gfx/side/<mod>/; where the mod has no pack, its layout is refused (WikiSim.checkLayout) or the pack has
   not loaded yet, the fixture art/wiki/side/fixture.json stands in, as it does in the DLL (ingame-wiki.md, *Which
   layout, and when a demo shows it*). `sidebar=` picks a fixture by name or no sidebar instead, for the mockup's own
   checks. The pictures need the game look (gfx=game) and a pack with its sheet; otherwise the column is stage.js's
   whitebox. */
const FIXTURES = {};                  // 'fixture', 'tall' -> the layout, or { error }
const PACKS = {};                     // mod -> 'loading', or WikiSide.load's pack (+ refused), or { error }
const MINIMAPS = {};                  // mod/entry -> the minimap's base: 'loading', bytes or null
async function loadFixtures() {
  await Promise.all([['fixture', 'fixture.json'], ['tall', 'fixture-tall.json']].map(([k, f]) =>
    fetch(`${ART}side/${f}`).then(r => { if (!r.ok) throw new Error(`side/${f}: ${r.status}`); return r.json(); })
      .then(j => { FIXTURES[k] = j; }, x => { FIXTURES[k] = { error: x.message }; })));
}
/* a mod's pack, once: a sim started before it arrived restarts on it, and the page's text and side list follow it.
   The layout and the casting are one source: a casting list that is not index-aligned with the layout's
   sides, or one side's casting the sim's bounds refuse, refuses the pack whole, as a refused layout does. The mod's
   stage packs (gfx.py --stages) are optional: without them a bound demo draws from the entry's own pack */
function loadPack(mod) {
  if (PACKS[mod] || mod === 'unknown') return;
  PACKS[mod] = 'loading';
  WikiSide.load(`gfx/side/${mod}/`).then(async p => {
    try {
      WikiSim.checkLayout(p.layout);
      if (!Array.isArray(p.casting) || p.casting.length !== p.layout.sides.length)
        throw new Error(`the casting lists ${Array.isArray(p.casting) ? p.casting.length : 'no'} sides for the layout's ${p.layout.sides.length}`);
      p.casting.forEach((c, k) => { try { WikiSim.checkCasting(c); } catch (x) { throw new Error(`side ${p.layout.sides[k].name}: ${x.message}`); } });
    } catch (x) { p.refused = x.message; console.error(`wiki sidebar pack ${mod}:`, x.message); }
    for (const m of p.problems) console.warn(`wiki sidebar pack ${mod}:`, m);
    p.stages = await fetch(`gfx/side/${mod}/stage/index.json`).then(r => r.ok ? r.json() : null, () => null)
      .then(j => j && j.entries ? j.entries : {}, () => ({}));
    PACKS[mod] = p;
  }, x => { PACKS[mod] = { error: x.message }; }).then(() => {
    sideNote();
    if (mod !== S.mod) return;
    sideControl(); renderTree(); renderEntry();
    if (TREE && sideLayout() !== simLayout) start();
  });
}
const fixture = k => FIXTURES[k] && !FIXTURES[k].error ? FIXTURES[k] : null;
const pack = () => { const p = S.sidebar === 'mod' && PACKS[S.mod]; return p && p !== 'loading' && !p.error && !p.refused ? p : null; };
function sideLayout() {
  if (S.sidebar === 'off') return null;
  if (S.sidebar === 'tall') return fixture('tall');
  const p = pack();
  return p ? p.layout : fixture('fixture');
}
/* THE CONTEXT the sim plays and the text is filled with: the side, by name, among the sidebar layout's sides (the
   fixture's one side where the fixture stands in, side 0 and nothing to choose without a sidebar); that side's casting
   only when the layout is the mod's own pack, so a trip never looks for a button of a type another layout named; and
   the Interface Type */
const sideIndex = L => {
  if (!L) return 0;
  const want = (S.side || '').toLowerCase(), k = L.sides.findIndex(d => d.name.toLowerCase() === want);
  return k < 0 ? 0 : k;
};
function context() {
  const L = sideLayout(), p = pack(), side = sideIndex(L);
  return { side, casting: p && L === p.layout ? p.casting[side] : null, iface: S.iface };
}
// the Side selector: the sidebar layout's sides by name, the one played selected; disabled without a sidebar
function sideControl() {
  const L = sideLayout(), el = $('#side'), names = L ? L.sides.map(d => d.name) : [];
  const html = names.length ? names.map(n => `<option>${esc(n)}</option>`).join('') : '<option>none</option>';
  if (el.innerHTML !== html) el.innerHTML = html;
  el.disabled = !names.length;
  if (names.length) el.selectedIndex = sideIndex(L);
}
function sideNote() {
  const p = PACKS[S.mod], el = $('#sidebar-note');
  el.textContent = S.sidebar !== 'mod' ? '' : !p || p.error ? `no pack for ${S.mod}: the fixture` : p === 'loading' ? 'loading…'
    : p.refused ? `${S.mod}'s layout refused: the fixture` : p.art ? `${S.mod}'s pack` : `${S.mod}'s pack, no pictures`;
}
const gameLook = () => $('#gfx').value === 'game';
/* the minimap's base picture for a stage, in the mod's own palette and shrink table: the pack's
   art.minimap.bases[<entry>.<scene>]; none leaves the minimap to the whitebox. Fetched once per mod and stage */
function minimapBase(p, id) {
  const b = id && p.minimap && p.minimap[id], key = `${S.mod}/${id}`;
  if (!b) return null;
  if (!(key in MINIMAPS)) {
    MINIMAPS[key] = 'loading';
    fetch(`gfx/side/${S.mod}/${b.file}`).then(r => r.ok ? r.arrayBuffer() : null)
      .then(buf => { MINIMAPS[key] = buf ? new Uint8Array(buf) : null; }, () => { MINIMAPS[key] = null; });
  }
  return MINIMAPS[key] instanceof Uint8Array ? MINIMAPS[key] : null;
}
/* the column in the mod's own pictures, for stage.js's drawSide: composed (side.js) at 128 game pixels wide and drawn
   whole into the column's rect, nearest, as the game cursor is; null leaves stage.js its whitebox */
const SIDECV = document.createElement('canvas');
function sideArt(v, layout) {
  const p = pack();
  if (!v || !v.side || !gameLook() || !p || !p.art || layout !== p.layout) return null;
  const c = WikiSide.compose(p.art, p.layout, v, minimapBase(p, stageId()), v.map.w * v.map.cell, v.map.h * v.map.cell);
  if (SIDECV.width !== WikiSide.COL || SIDECV.height !== c.h) { SIDECV.width = WikiSide.COL; SIDECV.height = c.h; }
  SIDECV.getContext('2d').putImageData(new ImageData(WikiSide.rgba(p.art, c), WikiSide.COL, c.h), 0, 0);
  const pics = p.art.screens[v.side.screen];
  return {
    minimap: c.minimap,
    // a gadget the image leaves to stage.js's whitebox button: its label's text ('' names it by its unit or gadget)
    label: i => !pics || !pics.pics[i] ? '' : pics.pics[i].from === 'label' ? pics.pics[i].text : null,
    draw: (g, r) => { const sm = g.imageSmoothingEnabled; g.imageSmoothingEnabled = false; g.drawImage(SIDECV, 0, 0, WikiSide.COL, c.h, r.x, r.y, r.w, r.h); g.imageSmoothingEnabled = sm; },
  };
}

function renderTree() {
  const el = $('#tree');
  if (treeErr) { el.innerHTML = `<div class="row missing">${esc(treeErr)}</div>`; return; }
  if (!TREE) { el.innerHTML = '<div class="row missing">loading…</div>'; return; }
  const walk = (ns, path) => '<ul>' + (ns || []).map((n, i) => {
    const p = `${path}.${i}`;
    if (n.entry) {
      const e = ENT[n.entry], title = e && !e.error ? fill(e, e.title) || n.entry : n.entry;
      const game = !!e && (e.frame === 'game' || (S.theatre && S.entry === n.entry)), open = S.entry === n.entry && game;
      // a theatre's entry has no demos column: it heads its own demos, the selected row being the demo
      const mark = !game ? '<span class="dot" data-part="i_dot"><i class="gl">•</i></span>'
        : `<span class="tw" data-part="${open ? 'i_open' : 'i_shut'}"><i class="gl">${open ? '▾' : '▸'}</i></span>`;
      const ds = open && Array.isArray(e.demos) ? e.demos : [];
      return `<li><div class="row leaf${S.entry === n.entry && !open ? ' sel' : ''}${e && e.error ? ' missing' : ''}" data-part="row" data-entry="${esc(n.entry)}">`
        + `${mark}<span>${esc(title)}</span>${locked(e) ? '<span class="lock" title="Turned off in this mod"></span>' : ''}</div>`
        + (ds.length ? '<ul>' + ds.map((d, k) => `<li><div class="row${k === S.demo ? ' sel' : ''}" data-part="row" data-demo="${k}"><span>${esc(fill(e, d.title) || d.id)}</span></div></li>`).join('') + '</ul>' : '')
        + '</li>';
    }
    const shut = S.fold.has(p);
    return `<li><div class="row cat" data-part="row" data-fold="${p}"><span class="tw" data-part="${shut ? 'i_shut' : 'i_open'}"><i class="gl">${shut ? '▸' : '▾'}</i></span><span>${esc(n.title)}</span></div>`
      + (shut ? '' : walk(n.children, p)) + '</li>';
  }).join('') + '</ul>';
  el.innerHTML = walk(TREE.tree, 't');
}
function renderEntry() {
  const e = entry(), err = S.entry && ENT[S.entry] && ENT[S.entry].error, ds = e && Array.isArray(e.demos) ? e.demos : [];
  arrange();
  $('#e-title').textContent = e ? fill(e, e.title) || e.id : err ? S.entry : TREE ? 'Choose an entry' : '';
  $('#e-sub').textContent = e ? fill(e, e.subtitle) : '';
  $('#badge').hidden = $('#t-badge').hidden = !locked(e);
  $('#demos').innerHTML = ds.length
    ? ds.map((d, i) => `<li data-part="row" data-demo="${i}"${i === S.demo ? ' class="sel"' : ''}><span class="n">${i + 1}</span><span>${esc(fill(e, d.title) || d.id)}</span></li>`).join('')
    : `<li class="none">${e ? 'No demos' : ''}</li>`;
  $('#t-title').textContent = ds[S.demo] ? fill(e, ds[S.demo].title) || ds[S.demo].id : '';
  $('#text').innerHTML = err ? `<p class="err">${esc(err)}</p>` : e ? (e.text || []).map(p => `<p>${esc(fill(e, p))}</p>`).join('') : '';
}
function render() { renderTree(); renderEntry(); controls(); syncURL(); }

/* ---- the theatre: the page of an entry with "frame": "game" (research/notes/ingame-wiki-replay-plan.md, decisions 6
   and 24). Its parts are the normal page's nodes RE-PARENTED, never copied, so every control and every text stays one
   element: the tree's panel keeps the body's left, renderTree putting the entry's demos under its row, the stage's bezel
   follows it, the controls join the row (#t-row), and the title, subtitle, badge and text go into About (#about). The
   row carries a badge of its own (#t-badge), as the layout's row does. Leaving the theatre puts every node back in the
   normal page's order. ---- */
function theatre() { const e = entry(); return !!e && (e.frame === 'game' || S.theatre); }
/* a text entry (sim.js checkText): a page entry with no demos, laid out with no stage (wiki.css #entry.text) */
const isText = e => !!e && !e.frame && !(Array.isArray(e.demos) && e.demos.length);
function arrange() {
  const on = theatre(), win = $('#win'), wb = $('#win .wb'), sbox = $('#sbox'), row = $('#t-row');
  if (on) {
    $('#p-tree').after(sbox);
    sbox.after(row);
    row.after($('#caption'));
    row.append($('#ctl'));
    $('#about').append($('#e-title'), $('#e-sub'), $('#badge'), $('#p-text'));
  } else if (win.classList.contains('theatre')) {
    $('.srow').append(sbox);
    $('#entry').append($('#e-title'), $('#e-sub'), $('#badge'), $('.srow'), $('#caption'), $('#keys'), $('#ctl'), $('#p-text'));
    wb.append(row);
  }
  win.classList.toggle('theatre', on);
  $('#entry').classList.toggle('text', isText(entry()));
  $('#about').hidden = !(on && S.about);
  sizeStage();
}
$('#c-about').onclick = () => { buttonSound(); S.about = !S.about; arrange(); controls(); syncURL(); };

/* ---- the window ---- */
function setOpen(o) {
  S.open = o; $('#win').hidden = !o; frame.classList.toggle('open', o); $('#b-wiki').classList.toggle('on', o);
  if (o) { if (!S.entry && TREE) pick(leaves(TREE.tree)[0], 0); sizeStage(); }
  releaseAll(); syncURL();
}
function pick(id, k) {
  S.entry = id; S.demo = k; S.try = S.try && !!id; S.playing = true;
  start(); render();
}
// the window opens with About closed
$('#b-wiki').onclick = () => { if (!S.open) { S.about = false; arrange(); controls(); } setOpen(!S.open); };
$('#wx').onclick = () => setOpen(false);
for (const ev of ['mousedown', 'mouseup', 'click', 'dblclick', 'wheel', 'contextmenu'])
  $('#capture').addEventListener(ev, e => { e.preventDefault(); e.stopPropagation(); }, { passive: false });
$('#win').addEventListener('contextmenu', e => e.preventDefault());
$('#tree').onclick = e => {
  const f = e.target.closest('[data-fold]');
  if (f) { buttonSound(); S.fold.has(f.dataset.fold) ? S.fold.delete(f.dataset.fold) : S.fold.add(f.dataset.fold); renderTree(); return; }
  // a demo under its entry, in the theatre
  const d = e.target.closest('[data-demo]');
  if (d) { buttonSound(); S.try = false; pick(S.entry, +d.dataset.demo); return; }
  // an entry picked closes About
  const l = e.target.closest('[data-entry]');
  if (!l) return;
  buttonSound(); S.about = false;
  if (l.dataset.entry !== S.entry) { S.try = false; pick(l.dataset.entry, 0); }
  else { arrange(); controls(); syncURL(); }
};
// a demo's row on the normal page
$('#demos').onclick = e => { const d = e.target.closest('[data-demo]'); if (d) { buttonSound(); S.try = false; pick(S.entry, +d.dataset.demo); } };

/* ---- playback: a demo plays when selected, at 30 ticks a second (60 at 2x), and holds its last frame when done;
   Try it yourself creates the same scene with no inputs and hands the stage to the player. A frame:game entry's demo is
   a replay instead (replay.js `rep`, `sim` stays null): its viewer's script plays, or in Try it yourself the player
   presses the stage HUD's controls ---- */
let sim = null, simErr = null, acc = 0, simLayout = null, simCtx = null;
let rep = null, repTry = false;
const frameEntry = () => { const e = entry(); return !!e && e.frame === 'game'; };
/* THE SOUND (sound.js): the sim's events played after each frame's steps, `heard` of them so far; the root of the stage
   pack's folder holds the sounds.json the sim takes its source from, as the game's sim takes it from the 3D load that
   gives the scripts */
let heard = 0;
// the sounds of the pack the stage draws from; with no stage (no entry, or a text entry) the gfx root, which holds none
const soundRoot = () => { const d = stageDir() || ''; return `gfx/${d.slice(0, d.lastIndexOf('/') + 1)}`; };
// a press on one of the page's controls: the engine's in-game menu controls say `Options` (tagpu_wiki.c button_sound)
const buttonSound = () => { if (typeof WikiSound !== 'undefined') WikiSound.named(soundRoot(), 'Options'); };
/* THE REPLAY'S LIFETIME (replay.js): it owns its simulations and their COB worlds, so it is freed whenever it is
   dropped -- a new start, a failure -- and nothing keeps it after */
function dropReplay() { if (rep) rep.free(); rep = null; }
function fail(x) { simErr = x && x.message || String(x); console.error('wiki sim:', x); sim = null; dropReplay(); }
function start() {
  sim = null; simErr = null; acc = 0; heard = 0; releaseAll(); dropReplay(); frameReset();
  const e = entry(), d = demo();
  if (!d) return;
  if (typeof WikiSim === 'undefined') { simErr = 'sim.js is not loaded'; return; }
  // a demo the layout refuses (an input inside a build's trip, say) shows why on the stage, as a broken entry does
  simLayout = sideLayout(); simCtx = context();
  const replay = frameEntry();
  repTry = replay && S.try;
  const make = ctx => replay ? (S.try ? WikiReplay.create : WikiReplay.play)(e, d, simLayout, ctx)
    : S.try ? WikiSim.create(e, d, simLayout, ctx) : WikiSim.play(e, d, simLayout, ctx);
  /* THE UNITS' SCRIPTS run on tools/wiki/serve.py's host, from the gfx pack the stage draws (cob.js). Served any other
     way, or with no pack built, the page plays without them and says why in the console. */
  const cob = typeof WikiCob !== 'undefined' ? WikiCob.cobPage(`tools/wiki/mockup/gfx/${stageDir()}`) : null;
  const snd = cob && typeof WikiSound !== 'undefined' ? WikiSound.source(soundRoot()) : null;
  const made = r => { if (replay) rep = r; else sim = r; };
  try { made(make(cob ? { ...simCtx, cob, snd } : simCtx)); }
  catch (x) {
    if (!cob || !/COB host/.test(x && x.message)) fail(x);
    else {
      console.warn('wiki sim: playing without the units\' scripts:', x.message);
      try { made(make(simCtx)); } catch (y) { fail(y); }
    }
  }
  // a type whose script the host refused plays at rest, its gates open (tagpu_wikicob.h)
  const shown = sim || (rep && rep.game);
  for (const n of shown ? shown.cobNotes() : []) if (n) console.warn('wiki sim: a script is quarantined:', n);
  controls(); syncURL();
}
/* a demo holds its last frame once done. A Try-it sim is never done but by a lock -- its scene holds a unit whose cast
   the side leaves unbound (sim.js, decision 12) -- and then holds the same way: the frame under the entry's badge, no
   tick and no input, as a demo stopped at a build the mod cannot reach does */
const held = () => rep ? rep.view().done : !!sim && sim.done;
function advance(dt) {
  if (!S.open || !(sim || rep) || !S.playing || held()) { acc = 0; return; }
  acc = Math.min(acc + dt * TICK * S.speed, 8);       // a stalled tab catches up a few ticks, not seconds of them
  if (rep) return advanceReplay();
  while (acc >= 1 && sim && !held()) { try { sim.step(); } catch (x) { fail(x); } acc--; }
}
/* THE REPLAY'S PACING, the DLL's rule: a frame runs whole steps while their summed cost() -- never fewer than the game
   ticks each runs -- stays within WikiReplay.FILL, always at least one, and the demo ticks left over are owed to the
   next frame, so a frame runs at most max(FILL, the most one step runs) game ticks. The pacing changes only when a step
   runs, never what it does: the replay is deterministic in demo ticks. */
function advanceReplay() {
  let spent = 0, n = 0;
  while (acc >= 1 && rep && !held()) {
    const c = rep.cost();
    if (n && spent + c > WikiReplay.FILL) break;
    liveFlush();
    try { rep.step(); } catch (x) { fail(x); return; }
    spent += c; n++; acc--;
  }
}
$('#c-play').onclick = () => { buttonSound(); if (held()) start(); else S.playing = !S.playing; controls(); };
$('#c-replay').onclick = () => { buttonSound(); S.playing = true; start(); };   // in Try it yourself, the scene afresh
$('#c-x1').onclick = () => { buttonSound(); S.speed = 1; controls(); };
$('#c-x2').onclick = () => { buttonSound(); S.speed = 2; controls(); };
$('#c-try').onclick = () => { buttonSound(); S.try = !S.try; S.playing = true; start(); };
function controls() {
  const has = !!demo(), set = (id, v) => { const el = $(id); if (el.textContent !== v) el.textContent = v; };
  const pause = S.playing && !held();
  $('#c-play').dataset.glyph = pause ? 'i_pause' : 'i_play';
  set('#c-play .gl', pause ? '❚❚' : '▶');
  set('#c-try', S.try ? 'Back to demo' : 'Try it yourself');
  for (const id of ['#c-play', '#c-replay', '#c-x1', '#c-x2', '#c-try']) $(id).disabled = !has;
  $('#c-x1').classList.toggle('on', S.speed === 1); $('#c-x2').classList.toggle('on', S.speed === 2);
  $('#c-try').classList.toggle('on', S.try);
  $('#c-about').classList.toggle('on', S.about);
}

/* ---- the stage: a canvas at the resolution's pixels; the map fitted inside it, letterboxed ---- */
const stage = $('#stage'), stageTop = $('#stagetop'), stageHud = $('#stagehud');
let FIT = null;                       // the last fit: canvas pixels a world unit, and the map's offset
let overStage = false;                // the player's pointer is over the stage's canvas
function sizeStage() {
  const r = stage.getBoundingClientRect(), f = frame.getBoundingClientRect();
  if (!r.width || !r.height) return;                     // the window is closed: keep the last size
  // the device pixels the game's stage covers (tagpu_wikistage_canvas): each edge snapped from the frame's corner, so
  // the size is the snapped right edge less the snapped left one, not the width rounded
  const snap = v => Math.floor(v + 0.5 + 1 / 64), x0 = (r.left - f.left) * G.k, y0 = (r.top - f.top) * G.k;
  const w = Math.max(1, snap(x0 + r.width * G.k) - snap(x0)), h = Math.max(1, snap(y0 + r.height * G.k) - snap(y0));
  for (const c of [stage, stageTop]) if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
}
new ResizeObserver(sizeStage).observe($('#sbox'));

function view() {
  if (!sim) return null;
  try { return sim.view(); } catch (x) { fail(x); return null; }
}
// the units' poses for the 3D stage (sim.js `poses`): one request to the COB host after each tick
function poses() {
  if (!sim) return null;
  try { return sim.poses(); } catch (x) { fail(x); return null; }
}
/* the 3D pack the stage draws from: a sim bound to a mod's casting draws from that mod's stage packs, which hold its
   units (gfx.py --stages), where it has one for the stage; any other from the stage's own */
function stageDir() {
  const p = pack(), id = stageId();
  return simCtx && simCtx.casting && p && p.stages && p.stages[id] ? `side/${S.mod}/stage/${id}` : id;
}
/* the stage's drawing is stage.js's. Under gfx=game, gfx.js first renders this frame's view in 3D; a COMPLETE image is
   drawn between the op ranges R0 and R2, and anything less leaves the whole whitebox (stage.js, draw()) */
const ICONS = WikiStage.loadIcons(ART + 'kit/');
// the game's own cursors, whenever tools/wiki/gfx.py's pack holds them, in either look: the DLL draws them whenever its
// asset load resolved them, and the kit's icons only where it did not
const CURSORS = WikiStage.loadCursors('gfx/');
let IMG = null;                       // the last frame's 3D answer, for scripted checks
function draw() {
  if (rep) return drawReplay();
  frameClear();
  const g = stage.getContext('2d'), v = view();
  if (v && typeof WikiSound !== 'undefined') heard = WikiSound.drain(soundRoot(), v, heard, sim && sim.types);
  const r = v && window.WikiGfx ? WikiGfx.frame(stageId(), v, sim ? sim.tick : 0, stageDir(), poses()) : null;
  IMG = r && { complete: r.complete, ground: r.ground, resolved: [...r.resolved], refused: r.refused };
  const image = r && r.complete ? {
    ground: r.ground, resolved: t => r.resolved.has(t),
    draw: (c, b) => { c.imageSmoothingEnabled = true; c.imageSmoothingQuality = 'low'; c.drawImage(r.canvas, b.x, b.y, b.w, b.h); },
  } : null;
  /* EXACTLY ONE CURSOR IN TRY IT YOURSELF: the stage draws its cursor only while the player's pointer is over the
     stage, and hides the player's (#stage.try) only in a frame whose cursor it drew -- a game sequence that ended, or an
     icon not loaded yet, leaves the player's. A demo's scripted cursor is drawn whatever the pointer does. */
  const tryIt = S.try && !!sim && !sim.done;
  FIT = WikiStage.draw(g, v, { W: stage.width, H: stage.height, U: G.gh / 720, icons: ICONS, cursors: CURSORS,
    tick: sim ? sim.tick : 0, cursor: !tryIt || overStage, image, side: { layout: simLayout, art: sideArt(v, simLayout) },
    msg: simErr || (demo() ? 'loading…' : entry() ? 'no demo' : '') });
  stage.classList.toggle('try', tryIt && !!FIT && FIT.cursor);
  // a build this mod's sidebar cannot reach stops the demo under the entry's badge (decision 4)
  const off = locked(entry()) || !!(v && v.locked);
  for (const b of ['#badge', '#t-badge']) if ($(b).hidden === off) $(b).hidden = !off;
  return v;
}

/* ---- the frame: a frame:game entry's replay on the whole game frame (frame.js), its HUD and Leave's question ----
   One frame of the page is, in order: the replay's view; the world shown, copied only while no seek is pending (a
   seek's ticks are never shown: the stage holds the last world drawn) -- a replay's game, a live replay's live
   simulation, or its past simulation while the past is shown, under the live one's column (the column shown at the
   open stays, MEASURED m0-facts.md #2); the frame record; the frame's view of that world (frame.js view: the
   watcher's, the player's or the past's); the fit; the stage HUD laid out by hud.js in #stagehud at 100 % of the frame,
   its controls' boxes, the cursor among them or the simulation's at its world point, and the hint under it; then
   stage.js's frame, the question and the cursor on #stagetop over the HUD. Every frame lays the HUD out again, one
   layout a frame as the DLL builds its ops; a return glide's frame lays out first the HUD its press was made on
   (frame.js pressed), whose controls the cursor leaves from. */
const S_LEFT = 'In the game, this goes back to the Replays browser.';
// the replay HUD's layout, kit and parts, from the repository root the check server serves: no stage HUD until then
let HUD_OK = false;
HUD.load('../../../art/hud/').then(() => { HUD_OK = true; }, x => console.error('wiki stage HUD:', x));
const hudSnap = v => Math.floor(v + 0.5 + 1 / 64);   // hud.js snapPx: a box's edge on the device pixel it is drawn on
/* the frame as last drawn: the game's view and poses held through a seek, the sound's generation and quiet, the
   record, the frame's corner and its HUD's unit (canvas pixels), its controls' boxes, and the press machine with the
   player's pointer for Try it yourself */
let FR = null, PRESS = null, PTR = null, liveMove = null;
function frameReset() {
  FR = { gv: null, lside: null, poses: null, tick: 0, gen: -1, quiet: -1, rec: null, hud: null, boxes: [] };
  PRESS = WikiFrame.Press(); liveMove = null;
}
frameReset();
function frameClear() {
  if (stageHud.firstChild) stageHud.innerHTML = '';
  if (!stageHud.hidden) { stageHud.hidden = true; stageTop.getContext('2d').clearRect(0, 0, stageTop.width, stageTop.height); }
}
/* the bars in the mod's own art (bar.js) under the game look with the pack's art, as the column is, showing the
   record's numbers (a live frame's memo, Watching's zeros), the top bar in the record's top_side's art (the human seat's
   side in a takeover's live game, decision 15); null for the whitebox. Composed again only when the art, the side, the
   top bar's side, the badge, the size or a number changes, as the DLL's bar_bind */
const BARCV = document.createElement('canvas');
let barKey = null;
function barArt(side, colour, Wg, Hg, num, top) {
  const p = pack();
  if (!gameLook() || !p || !p.art || simLayout !== p.layout || typeof WikiBar === 'undefined') return null;
  const key = `${S.mod}|${side}|${top}|${colour}|${Wg}|${Hg}|${JSON.stringify(num)}`;
  if (key !== barKey) {
    const c = WikiBar.compose(p.art, side, colour, Wg, Hg, num, top);
    barKey = key;
    BARCV.width = WikiBar.BAR_W; BARCV.height = WikiBar.BAR_H;
    if (c) BARCV.getContext('2d').putImageData(new ImageData(WikiBar.rgba(p.art, c), WikiBar.BAR_W, WikiBar.BAR_H), 0, 0);
    else barKey = `${key}|none`;
  }
  if (barKey.endsWith('|none')) return null;
  return { draw: (g, part, size, r) => { const sm = g.imageSmoothingEnabled; g.imageSmoothingEnabled = false;
    g.drawImage(BARCV, 0, WikiBar.BAR_H / 2 * part, size, WikiBar.BAR_H / 2, r.x, r.y, r.w, r.h); g.imageSmoothingEnabled = sm; } };
}
/* the GUI's black, GUI colour 0x00 of the side pack's colour table in its palette (tagpu_mark.c GUI_BLACK, which edges the
   game's overlay arrows), or the stand-in black without the pack's table */
function guiBlack(art) {
  const p = art && art.gui && art.pal ? art.gui[0] : -1;
  return p >= 0 ? `rgb(${art.pal[p * 4]},${art.pal[p * 4 + 1]},${art.pal[p * 4 + 2]})` : '#000';
}
function drawReplay() {
  const g = stage.getContext('2d'), W = stage.width, H = stage.height, U = G.gh / 720;
  let rv, mode = 'watch', human = 0;
  try {
    rv = rep.view();
    mode = !rv.live ? 'watch' : rv.live.rw === 'past' ? 'past' : 'player';
    // the human seat's view: seat 0's but in a takeover's live game, the seat taken over's
    human = rv.live ? rv.live.human : 0;
    const shown = mode === 'past' ? rep.past : rep.game;
    if (rv.game.target < 0 && shown) {
      FR.gv = shown.view(human); FR.poses = shown.poses(); FR.tick = shown.tick;
      // the past's column is the live simulation's, which is never stepped while the past is shown
      FR.lside = mode === 'past' && rep.game ? rep.game.view(human).side || null : null;
    }
  } catch (x) { fail(x); frameClear(); WikiStage.draw(g, null, { W, H, U, msg: simErr }); return null; }
  // THE SOUND: what a seek re-simulated is heard already -- each new game or finished seek starts the count at `quiet`
  if (rv.game.gen !== FR.gen || rv.game.quiet !== FR.quiet) { FR.gen = rv.game.gen; FR.quiet = rv.game.quiet; heard = rv.game.quiet; }
  if (FR.gv && rv.game.target < 0 && typeof WikiSound !== 'undefined') heard = WikiSound.drain(soundRoot(), FR.gv, heard, (mode === 'past' ? rep.past : rep.game)?.types);
  const side = simCtx ? simCtx.side : 0, e = entry();
  // a players replay's record carries its seats' teams and cameras and the stats window's samples (frame.js record)
  const pl = rv.players && e ? { seats: WikiFrame.plSeats(e, demo()), stat: rep.stat } : null;
  const rec = FR.gv ? WikiFrame.record(rv, FR.gv, side, repTry, PRESS.held, WikiFrame.topSide(rv, simLayout, side), pl) : null;
  /* THE ONE CURSOR in Try it: after a fork the simulation's follows the player's pointer while it is over the world and
     on no control of the HUD, and is the one drawn there, the browser's hidden (rewind-spec.md 9.5) */
  const onWorld = !!(rec && rec.simcur && repTry && PTR && overStage && liveWorld(PTR));
  const gv = mode === 'past' && FR.gv ? { ...FR.gv, side: FR.lside || undefined } : FR.gv;
  // the world shown with the replay's overlays, at the tick it was copied at, and a players entry's camera and Sight
  const fv = WikiFrame.view(gv, simLayout, side, mode, !!rec && rec.simcur && (!repTry || onWorld), human,
    { tick: FR.tick, clickFrom: rv.clickFrom, players: rv.players || null, screen: rv.players && e ? e.screen : null });
  if (!fv) { frameClear(); FIT = WikiStage.draw(g, null, { W, H, U, msg: 'loading…' }); return null; }
  const C = WikiStage.content(fv), fit = WikiStage.fit(C, W, H, U), Wg = C.w, Hg = C.h;
  const fx = hudSnap(fit.ox + C.x0 * fit.s), fy = hudSnap(fit.oz + C.z0 * fit.s), hpx = Hg * fit.s / WikiFrame.HUD_H;
  const p = pack(), art = gameLook() && p && p.art && simLayout === p.layout ? p.art : null;
  const st = WikiFrame.hudState(rec, Wg, Hg, art, HUD_OK ? HUD.layout : null);
  const units = q => ({ x: (q.x - fx) / hpx, y: (q.y - fy) / hpx });
  const dlg = WikiFrame.dialog(fit, Wg, Hg);
  const game = WikiFrame.gamePoint(rec, fit, fx, fy, hpx);
  let cursor = null;
  stageHud.hidden = false;
  if (HUD_OK) {
    // #stagehud on the frame's corner: the canvas's corner in #sbox plus the frame's, in CSS pixels (device / G.k)
    const sr = stage.getBoundingClientRect(), br = $('#sbox').getBoundingClientRect(), box = $('#sbox');
    Object.assign(stageHud.style, { left: `${sr.left - br.left - box.clientLeft + fx / G.k}px`, top: `${sr.top - br.top - box.clientTop + fy / G.k}px`,
      width: `${Wg * fit.s / G.k}px`, height: `${Hg * fit.s / G.k}px` });
    stageHud.style.setProperty('--u', `${hpx / G.k}px`);
    /* a control's key as tagpu_hudlay.c makes it: its act, and a row's or a Take over row's seat, a lane's place, a
       team header's team or a chip's name (frame.js indexed) */
    const keyOf = e => 'track' in e.dataset ? 'track' : e.dataset.act
      + (e.dataset.seat != null ? `:${e.dataset.seat}` : e.dataset.fork != null ? `:${e.dataset.fork}`
        : e.dataset.team != null ? `:${e.dataset.team}` : e.dataset.act === 'chip' && e.dataset.val != null ? `:${e.dataset.val}` : '');
    const boxes = () => [...stageHud.querySelectorAll('[data-act], [data-track]')].map(e => ({
      key: keyOf(e), off: e.classList.contains('off'),
      ub: (e.dataset.ub || '0 0 0 0').split(' ').map(Number), ends: +e.dataset.ends || 0 }));
    const pr = rec.cur.on ? WikiFrame.pressed(rec) : rec;
    let among = null;
    if (pr !== rec) { HUD.draw(stageHud, WikiFrame.hudState(pr, Wg, Hg, art, HUD.layout), hpx, G.k); among = boxes(); }
    HUD.draw(stageHud, st, hpx, G.k);
    const down = rec.held && [...stageHud.querySelectorAll('.art.hot[data-act]')].find(e => keyOf(e) === rec.held);
    if (down) HUD.press(down, true);
    FR.boxes = boxes();
    const rp = WikiFrame.restOf(fv), rest = units({ x: fit.ox + rp.x * fit.s, y: fit.oz + rp.z * fit.s });
    const no = units({ x: dlg.no.x + dlg.no.w / 2, y: dlg.no.y + dlg.no.h / 2 });
    const simShape = () => ({ shape: WikiFrame.SHAPES[rec.sim.shape] || 'arrow', age: rec.sim.age });
    /* THE ROW UNDER THE CURSOR (a point in units) shows its hover strip, as the game's HUD lays the strip out only on
       the row the pointer is over (tagpu_hudlay.c row): the stage HUD takes no pointer events, so the browser's
       :hover never sees the scripted cursor or the player's pointer */
    const hoverRow = q => {
      if (!q || q.x == null) return;
      const X = Math.floor(q.x * hpx + 0.5), Y = Math.floor(q.y * hpx + 0.5);
      const row = FR.boxes.filter(b => b.key.startsWith('row:') && X >= hudSnap(b.ub[0] * hpx) && X < hudSnap((b.ub[0] + b.ub[2]) * hpx)
        && Y >= hudSnap(b.ub[1] * hpx) && Y < hudSnap((b.ub[1] + b.ub[3]) * hpx)).pop();
      const hov = row && stageHud.querySelector(`.row[data-seat="${row.key.slice(4)}"] > .hov`);
      if (hov) hov.style.visibility = 'visible';
    };
    if (rec.cur.on) {
      const pt = WikiFrame.point(rec, among || FR.boxes, hpx, rest, no, game);
      hoverRow(pt);
      HUD.hint(stageHud, pt.x, pt.y, st);
      cursor = { x: fx + pt.x * hpx, y: fy + pt.y * hpx };
    } else if (rec.simcur && !repTry && game) {
      // a demo's simulation's own cursor, its shape and animation at its world point
      HUD.hint(stageHud, game.x, game.y, st);
      cursor = { x: fx + game.x * hpx, y: fy + game.y * hpx, ...simShape() };
    } else {
      const q = PTR && overStage ? units(PTR) : null;
      hoverRow(q);
      HUD.hint(stageHud, q && q.x, q && q.y, st);
      if (onWorld) cursor = { x: PTR.x, y: PTR.y, ...simShape() };
    }
  }
  FR.rec = rec;
  FR.hud = { fx, fy, hpx, Wg, Hg, dlg, cursor, fit, side_h: fv.side ? fv.side.h : 0,
    bar: { side, top: rec.top_side, colour: rec.colour } };
  // the poses of the units the view draws: under Sight the view leaves some out, and `slots` says which it kept
  const poses = fv.slots && FR.poses ? fv.slots.map(i => FR.poses[i] ?? null) : FR.poses;
  const r = window.WikiGfx ? WikiGfx.frame(stageId(), fv, FR.tick, stageDir(), poses) : null;
  IMG = r && { complete: r.complete, ground: r.ground, resolved: [...r.resolved], refused: r.refused };
  const image = r && r.complete ? {
    ground: r.ground, resolved: t => r.resolved.has(t),
    draw: (c, b) => { c.imageSmoothingEnabled = true; c.imageSmoothingQuality = 'low'; c.drawImage(r.canvas, b.x, b.y, b.w, b.h); },
  } : null;
  FIT = WikiStage.draw(g, fv, { W, H, U, icons: ICONS, cursors: CURSORS, tick: FR.tick, cursor: false, image,
    side: { layout: simLayout, art: sideArt(fv, simLayout) },
    frame: { bar: barArt(side, rec.colour, Wg, Hg, rec.bar, rec.top_side), button: LAY.button, top: stageTop.getContext('2d'),
      question: rec.question ? dlg : null, held: PRESS.held, cursor, live: rec.live,
      // the overlays' seat colours, the HUD's (frame.js seatColour), and the GUI's black their arrows are edged in
      colour: k => WikiFrame.seatColour(art, k), edge: guiBlack(art) } });
  FR.hud.emblems = FIT ? FIT.emblems : null;
  stage.classList.toggle('try', onWorld);
  const off = locked(entry()) || !!(FR.gv && FR.gv.locked);
  for (const b of ['#badge', '#t-badge']) if ($(b).hidden === off) $(b).hidden = !off;
  return { caption: rv.left ? S_LEFT : rv.caption, keys: [] };
}

/* ---- Try it yourself on the frame: the player's presses on the stage HUD and on Leave's question go to the press
   machine (frame.js Press), whose outputs are the replay's presses. A press on the stage is tested first against the
   question's rects while it is open -- Yes, No, its body, then the frame round it, which counts as No -- then against
   the stage HUD's controls, topmost first, each box on the device pixels it is drawn on (hud.js hint's rule), and
   anything else on the stage is taken and does nothing: the viewer is not seat 0, so the world and the column take no
   click. About lies over the stage and takes its own clicks first. ---- */
const frameTry = () => !!rep && repTry && S.try && !held();
const canvasPoint = e => { const r = stage.getBoundingClientRect(); return { x: (e.clientX - r.left) * stage.width / r.width, y: (e.clientY - r.top) * stage.height / r.height }; };
const inBox = (b, q) => q.x >= b.x && q.x < b.x + b.w && q.y >= b.y && q.y < b.y + b.h;
function frameHit(q) {
  const F = FR.hud;
  if (!F || !FR.rec) return null;
  if (FR.rec.question) {
    for (const [k, b] of [['yes', F.dlg.yes], ['no', F.dlg.no], ['dialog', F.dlg.panel], ['outside', F.dlg.frame]])
      if (inBox(b, { x: Math.floor(q.x), y: Math.floor(q.y) })) return { src: 'dialog', key: k };
    return null;
  }
  const X = Math.floor(q.x - F.fx + 0.5), Y = Math.floor(q.y - F.fy + 0.5), px = F.hpx;
  let on = null;
  for (const b of FR.boxes) {
    if (b.off) continue;
    const [x, y, w, h] = b.ub;
    if (X >= hudSnap(x * px) && X < hudSnap((x + w) * px) && Y >= hudSnap(y * px) && Y < hudSnap((y + h) * px)) on = b;
  }
  return on && { src: 'hud', key: on.key };
}
/* the track's tick under canvas point q: its x on the playhead's run as drawn, device pixels, clamped to the run, then
   rounded on the span the track was drawn with -- [0, end], live the strip's [first, G] (tagpu_hudinput.c seek_at,
   live_seek_at); the replay's seek clamps it again */
function trackTick(q) {
  const F = FR.hud, t = FR.boxes.find(b => b.key === 'track');
  if (!F || !t || !FR.rec) return 0;
  const x0 = hudSnap((t.ub[0] + t.ends) * F.hpx), x1 = hudSnap((t.ub[0] + t.ub[2] - t.ends) * F.hpx);
  if (x1 <= x0) return 0;
  const f = clamp((Math.floor(q.x - F.fx + 0.5) - x0) / (x1 - x0), 0, 1), r = FR.rec;
  if (r.live) return r.first + Math.trunc(f * (Math.max(r.G, r.first) - r.first) + 0.5);
  return Math.trunc(f * r.end + 0.5);
}
/* the press machine's outputs made on the replay, the pointer's held move first; Yes ends Try it, and the stage holds
   the replay it left. A live replay whose rewind leaves idle takes no key or button of the world until it is idle
   again (replay.js input), and lets go of seat 0's input at its rebuild, so the page holds none down either */
function pressOut(outs) {
  if (outs.length) liveFlush();
  for (const [c, v] of outs) rep.press(c, v);
  const v = outs.length ? rep.view() : null;
  if (v && v.left) { S.try = false; controls(); syncURL(); }
  if (v && v.live && v.live.rw !== 'idle') down.clear();
}
/* TRY IT YOURSELF'S WORLD on a live frame (rewind-spec.md 9.6): canvas point q as a world point [x, z] through the
   frame's fit when it lies on the column or the map (frame.js onWorld) and on no control of the HUD, else null. The
   replay takes it only after a fork (replay.js input). */
function liveWorld(q) {
  const F = FR.hud, r = FR.rec, M = FR.gv && FR.gv.map;
  if (!F || !F.fit || !r || !r.live || r.question || !M || frameHit(q)) return null;
  const x = (q.x - F.fit.ox) / F.fit.s, z = (q.y - F.fit.oz) / F.fit.s, cell = +M.cell || 16;
  return WikiFrame.onWorld(x, z, F.Hg, (+M.w || 1) * cell, (+M.h || 1) * cell) ? [x, z] : null;
}
const liveTry = () => frameTry() && !!FR.rec && !!FR.rec.live;
// the pointer's last world point, sent once before each replay step and before each press (rewind-spec.md 9.6)
function liveFlush() {
  if (!liveMove || !rep) return;
  const m = liveMove;
  liveMove = null;
  try { rep.input({ move: m }); } catch (x) { fail(x); }
}
stage.addEventListener('mousedown', e => {
  if (!frameTry()) return;
  e.preventDefault();
  const q = canvasPoint(e), h = frameHit(q);
  if (h) { pressOut(PRESS.down(h.src, h.key, e.button !== 0, trackTick(q), FR.rec ? WikiFrame.lanes(FR.rec) : [])); return; }
  const w = liveTry() && liveWorld(q), b = BTN[e.button];
  if (w && b) { liveMove = w; liveFlush(); if (!down.has(b)) { down.add(b); send({ down: b }); } }
});
window.addEventListener('mousemove', e => {
  if (!frameEntry()) return;
  PTR = canvasPoint(e);
  const w = liveTry() && liveWorld(PTR);
  if (w) liveMove = w;
});
window.addEventListener('mouseup', e => {
  if (!frameTry() || !PRESS.held || e.button !== 0) return;
  const q = canvasPoint(e), h = frameHit(q);
  pressOut(PRESS.up(h ? h.key : null, trackTick(q), FR.rec ? WikiFrame.lanes(FR.rec) : []));
});
// while the track is held, a seek each frame the pointer's tick changes, as the game seeks during a drag
function frameDrag() { if (frameTry() && PRESS.held === 'track' && PTR) pressOut(PRESS.drag(trackTick(PTR))); }

/* ---- the strip under the stage: caption and the keys and buttons held ---- */
let shownKeys = null, shownCap = null;
function keysOf(v) {
  const k = v && v.keys;
  if (!k) return [];
  const list = Array.isArray(k) ? k : k instanceof Set ? [...k] : Object.keys(k).filter(n => k[n]);
  return list.map(String);
}
function strip(v) {
  const cap = v && v.caption ? fill(entry(), v.caption) : '';
  if (cap !== shownCap) { $('#caption').textContent = shownCap = cap; }
  const ks = keysOf(v).join('+');
  if (ks !== shownKeys) {
    shownKeys = ks;
    $('#keys').innerHTML = ks ? ks.split('+').map(n => `<span class="kc" data-part="keycap">${esc(KEYCAP[n] || n)}</span>`).join('<span>+</span>') : '';
  }
}

/* ---- Try it yourself: the stage's mouse and the modifier keys go to sim.input; the sidebar's buttons are the sim's ---- */
const down = new Set();                // the buttons and keys sent down and not yet up
function send(ev) {
  if (liveTry()) { if (!ev.move) { liveFlush(); try { rep.input(ev); } catch (x) { fail(x); } } return; }
  if (!sim || !S.try || sim.done) return;
  try { sim.input(ev); } catch (x) { fail(x); }
}
function releaseAll() { for (const k of down) send({ up: k }); down.clear(); }
// the pointer as a stage point clamped to the content rect: the map, and the sidebar with it. The sim decides what is
// under it (the column or the map)
function world(e) {
  if (!FIT) return null;
  const r = stage.getBoundingClientRect(), px = (e.clientX - r.left) * stage.width / r.width, pz = (e.clientY - r.top) * stage.height / r.height;
  return [clamp((px - FIT.ox) / FIT.s, FIT.x0, FIT.x0 + FIT.cw), clamp((pz - FIT.oz) / FIT.s, 0, FIT.ch)];
}
const BTN = { 0: 'lmb', 2: 'rmb' };
stage.addEventListener('mouseenter', () => { overStage = true; });
stage.addEventListener('mouseleave', () => { overStage = false; });
// a frame's stage takes its own (the frame's handlers above): a live frame's world through its fit
stage.addEventListener('mousemove', e => { if (S.try && !frameEntry()) { const w = world(e); if (w) send({ move: w }); } });
stage.addEventListener('mousedown', e => {
  const b = BTN[e.button]; if (!S.try || !b || frameEntry()) return;
  e.preventDefault(); const w = world(e); if (w) send({ move: w });
  if (!down.has(b)) { down.add(b); send({ down: b }); }
});
window.addEventListener('mouseup', e => { const b = BTN[e.button]; if (b && down.has(b)) { down.delete(b); send({ up: b }); } });
stage.addEventListener('contextmenu', e => e.preventDefault());
const MOD = { Shift: 'shift', Alt: 'alt', Control: 'ctrl' };
// Z and X with Alt step the armed building's spacing; every down is sent, an auto-repeat included, as the game steps again
const STEP = { KeyZ: 'z', KeyX: 'x' };
// E is a key to the sidebar's buttons (RECLAIM's quickkey), every down as the GUI takes each; not under Ctrl, where the
// game's key translator pushes another code (tagpu_wiki.c try_key)
const KEY = { KeyE: 'e' };
window.addEventListener('keydown', e => {
  if (e.key === 'Escape' && S.open) { e.preventDefault(); setOpen(false); return; }
  const z = STEP[e.code];
  if (z && e.altKey && S.open && S.try) { e.preventDefault(); down.add(z); send({ down: z }); return; }
  const q = KEY[e.code];
  if (q && !e.ctrlKey && S.open && S.try) { e.preventDefault(); down.add(q); send({ down: q }); return; }
  const m = MOD[e.key]; if (!m || !S.open || !S.try) return;
  e.preventDefault();                  // Alt would otherwise open the browser's menu and keep the key
  if (!down.has(m)) { down.add(m); send({ down: m }); }
});
window.addEventListener('keyup', e => {
  const m = MOD[e.key] || STEP[e.code] || KEY[e.code];
  if (m && down.has(m)) { e.preventDefault(); down.delete(m); send({ up: m }); }
});
window.addEventListener('blur', releaseAll);

/* ---- skins: the whitebox, or the kit of art/wiki/kit/ -- kit.json in art/hud/kit.json's format and its parts as
   PNGs. The layout's `geom` places what the kit's window art leaves room for, in units: the title in its dark title
   groove (wt: left, right, top, height), the close button centred in its close bay (wx: right, top, width, height),
   the body inside its frame (wb), and how deep its panels' frame (pn) and its bezel's (bz) are, each [top, right,
   bottom, left]. MEASURED 2026-10-25 from the kit's window.png, panel.png and bezel.png at its scale: the groove's and
   the bay's dark spans across the title band, and the frames' inner edges where the face's tone begins; the body keeps
   3 units more off the frame. ---- */
const GEOM = LAY.geom;
const KITS = {};                      // name -> { parts, img, light } once loaded
function applySkin() {
  const g = GEOM[S.skin], set = (n, v) => frame.style.setProperty(n, v);
  frame.classList.toggle('kit', S.skin !== 'whitebox');
  ['l', 'r', 't', 'h'].forEach((k, i) => set('--wt-' + k, g.wt[i]));
  ['r', 't', 'w', 'h'].forEach((k, i) => set('--wx-' + k, g.wx[i]));
  for (const n of ['wb', 'pn', 'bz']) ['t', 'r', 'b', 'l'].forEach((k, i) => set(`--${n}-${k}`, g[n][i]));
  set('--gap', g.gap);
  UIKit.dropParts();
  if (S.skin !== 'whitebox' && !KITS[S.skin]) loadKit(S.skin);
  sizeStage();
}
function loadKit(name) {
  KITS[name] = { loading: true };
  UIKit.loadKit(`${ART}${name}/`).then(k => { KITS[name] = k; }).catch(x => { console.error('wiki skin:', x); KITS[name] = { error: x.message }; });
}
function paintParts() {
  const K = KITS[S.skin];
  if (S.skin !== 'whitebox' && K && K.parts) UIKit.paintParts(K, S.skin, G.gh / 720, G.k);
}
/* ---- text: the elements of each of the wiki's text roles (uikit.js roleStyles) ---- */
const ROLE = {
  window_title: ['#win .wt'], entry_title: ['#e-title'],
  category: ['#tree .cat'], entry: ['#tree .leaf'], demo: ['#demos li', '#tree [data-demo]', '#t-title'], demo_num: ['#demos li .n'],
  subtitle: ['#e-sub'], text: ['#text'], caption: ['#caption'],
  control: ['#ctl button', '#t-row button'], badge: ['#badge', '#t-badge'], keycap: ['.kc'], keys: ['#keys'],
};
UIKit.layoutVars(frame, LAY);
fetch('../../../art/hud/layout.json').then(r => r.json()).then(L => UIKit.roleStyles(ROLE, LAY.roles, L.styles)).catch(x => console.error('wiki styles:', x));

/* ---- the frame loop ---- */
let last = 0;
function loop(t) {
  const dt = last ? Math.min(.25, (t - last) / 1000) : 0; last = t;
  gameTick(dt);
  if (S.open) {
    frameDrag();
    advance(dt);
    const v = draw(); strip(v);
    if (held() !== loop.held) { loop.held = held(); controls(); }
  }
  paintParts();                       // last, so a keycap or button made this frame shows its part in it
  requestAnimationFrame(loop);
}

/* ---- the controls strip ---- */
$('#res').value = S.res; $('#res').onchange = e => { S.res = e.target.value; layout(); syncURL(); };
$('#size').value = S.size; $('#sizev').textContent = S.size + ' %';
$('#size').oninput = e => { S.size = +e.target.value; $('#sizev').textContent = S.size + ' %'; frame.style.setProperty('--s', S.size); syncURL(); };
$('#mode').value = S.mode; $('#mode').onchange = e => { S.mode = e.target.value; syncURL(); };
$('#rewind').value = +S.rewind; $('#rewind').onchange = e => { S.rewind = e.target.value === '1'; frame.classList.toggle('norewind', !S.rewind); syncURL(); };
frame.classList.toggle('norewind', !S.rewind);
$('#skin').value = S.skin; $('#skin').onchange = e => { S.skin = e.target.value; applySkin(); syncURL(); };
$('#mod').value = S.mod; $('#mod').onchange = e => { S.mod = e.target.value; loadPack(S.mod); sideNote(); sideControl(); renderTree(); renderEntry(); start(); syncURL(); };
$('#sidebar').value = S.sidebar; $('#sidebar').onchange = e => { S.sidebar = e.target.value; sideNote(); sideControl(); renderTree(); renderEntry(); start(); syncURL(); };
$('#side').onchange = e => { S.side = e.target.value; renderTree(); renderEntry(); start(); syncURL(); };
$('#iface').value = S.iface; $('#iface').onchange = e => { S.iface = +e.target.value; renderTree(); renderEntry(); start(); syncURL(); };
window.addEventListener('resize', layout);

applySkin(); layout(); renderTree(); renderEntry(); controls(); loadPack(S.mod); sideNote(); sideControl();
Promise.all([load(), loadFixtures()]).then(() => {
  sideControl();
  if (S.entry && !(S.entry in ENT)) S.entry = null;
  if (S.open && !S.entry && TREE) S.entry = leaves(TREE.tree)[0] || null;
  const d = entry() && entry().demos; if (d && S.demo >= d.length) S.demo = 0;
  setOpen(S.open); start(); render();
});
requestAnimationFrame(loop);
// the page's state, its running simulation and the sidebar layout it runs on, for scripted checks of the mockup
// and a frame:game entry's replay, its frame record, and the frame as drawn: its corner and the stage HUD's unit in canvas
// pixels, its size in game pixels, Leave's question's rects, the viewer's cursor and the emblems' boxes, in canvas pixels
window.WikiPage = { state: S, sim: () => sim, replay: () => rep, frame: () => FR && FR.rec, frameDrawn: () => FR && FR.hud,
  stageId, image: () => IMG, layout: () => simLayout, context: () => simCtx };
})();
