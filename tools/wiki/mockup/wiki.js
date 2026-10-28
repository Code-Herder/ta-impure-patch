/* wiki.js -- the in-game wiki's mockup: the game frame and Impure's top bar, the wiki window with its tree, demo list and
   entry, and the stage, where stage.js draws what sim.js's view() says. The content is art/wiki/ (tree.json,
   entries/<id>.json) and the behaviour is sim.js's; this file lays out, draws and forwards input. research/notes/ingame-wiki.md is the
   specification the game copies. */
(async () => {
const $ = s => document.querySelector(s);
const ART = '../../../art/wiki/', BACKDROP = '../../../research/notes/assets/hud-art/backdrop.png';
// the layout's numbers, in units, which the DLL reads too (tagpu_wikilay.c): everything below waits for them
const LAY = await fetch(ART + 'layout.json').then(r => { if (!r.ok) throw new Error(`layout.json: ${r.status}`); return r.json(); });
const TICK = 30;                      // simulation ticks a second, the game's
const MODS = ['stock', 'mayhem', 'prota', 'twilight', 'escalation', 'tazero', 'unknown'];
const RES = [...$('#res').options].map(o => o.value);
const KEYCAP = { shift: 'Shift', alt: 'Alt', ctrl: 'Ctrl', z: 'Z', x: 'X', lmb: 'Left-click', rmb: 'Right-click' };
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const esc = s => String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;');

/* ---- the state, from the URL; the URL is kept up to date so a view can be shared ---- */
const SKINS = ['kit', 'whitebox'];
// the sidebar's layout: the detected mod's pack (the fixture where it has none), a fixture by name, or none at all
const SIDEBARS = ['mod', 'fixture', 'tall', 'off'];
/* side: the side asked for by name, matched without case against the sidebar layout's sides (sideIndex), kept as asked
   so a pack that loads after the page still finds it; null or no match is the layout's first side. iface: the
   Interface Type the demos play, 1 right-click (the interface they are authored in) or 0 left-click */
const S = { res: '1920x1080', size: LAY.window.share, mode: 'sp', mod: 'stock', side: null, iface: 1, skin: 'kit', sidebar: 'mod', rewind: true,
  open: false, entry: null, demo: 0, try: false, playing: true, speed: 1, fold: new Set() };
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
  if (q.get('demo')) S.demo = Math.max(0, (+q.get('demo') || 1) - 1); }
function syncURL() {
  const q = new URLSearchParams({ res: S.res, size: S.size, mode: S.mode, mod: S.mod });
  if (S.side) q.set('side', S.side);
  for (const [k, v] of Object.entries({ iface: S.iface, skin: S.skin, sidebar: S.sidebar, rewind: +S.rewind })) q.set(k, v);
  if (S.open) { q.set('open', ''); if (S.entry) { q.set('entry', S.entry); q.set('demo', S.demo + 1); } if (S.try) q.set('try', ''); }
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
  await Promise.all(leaves(TREE.tree).map(id => fetch(`${ART}entries/${id}.json`)
    .then(r => { if (!r.ok) throw new Error(`entries/${id}.json: ${r.status}`); return r.json(); })
    .then(j => { const why = WikiText.checkEntry(j); ENT[id] = why ? { error: `entries/${id}.json: ${why}` } : j; },
      x => { ENT[id] = { error: x.message }; })));
}
const entry = () => (S.entry && ENT[S.entry] && !ENT[S.entry].error) ? ENT[S.entry] : null;
const demo = () => { const e = entry(); return e && Array.isArray(e.demos) ? e.demos[S.demo] || null : null; };
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
/* the minimap's base picture for an entry's stage map, in the mod's own palette and shrink table: the pack's
   art.minimap.bases[entry]; none leaves the minimap to the whitebox. Fetched once per mod and entry */
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
  const c = WikiSide.compose(p.art, p.layout, v, minimapBase(p, S.entry), v.map.w * v.map.cell, v.map.h * v.map.cell);
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
      return `<li><div class="row leaf${S.entry === n.entry ? ' sel' : ''}${e && e.error ? ' missing' : ''}" data-part="row" data-entry="${esc(n.entry)}">`
        + `<span class="dot" data-part="i_dot"><i class="gl">•</i></span><span>${esc(title)}</span>${locked(e) ? '<span class="lock" title="Turned off in this mod"></span>' : ''}</div></li>`;
    }
    const shut = S.fold.has(p);
    return `<li><div class="row cat" data-part="row" data-fold="${p}"><span class="tw" data-part="${shut ? 'i_shut' : 'i_open'}"><i class="gl">${shut ? '▸' : '▾'}</i></span><span>${esc(n.title)}</span></div>`
      + (shut ? '' : walk(n.children, p)) + '</li>';
  }).join('') + '</ul>';
  el.innerHTML = walk(TREE.tree, 't');
}
function renderEntry() {
  const e = entry(), err = S.entry && ENT[S.entry] && ENT[S.entry].error;
  $('#e-title').textContent = e ? fill(e, e.title) || e.id : err ? S.entry : TREE ? 'Choose an entry' : '';
  $('#e-sub').textContent = e ? fill(e, e.subtitle) : '';
  $('#badge').hidden = !locked(e);
  $('#demos').innerHTML = e && Array.isArray(e.demos) && e.demos.length
    ? e.demos.map((d, i) => `<li data-part="row" data-demo="${i}"${i === S.demo ? ' class="sel"' : ''}><span class="n">${i + 1}</span><span>${esc(fill(e, d.title) || d.id)}</span></li>`).join('')
    : `<li class="none">${e ? 'No demos' : ''}</li>`;
  $('#text').innerHTML = err ? `<p class="err">${esc(err)}</p>` : e ? (e.text || []).map(p => `<p>${esc(fill(e, p))}</p>`).join('') : '';
}
function render() { renderTree(); renderEntry(); controls(); syncURL(); }

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
$('#b-wiki').onclick = () => setOpen(!S.open);
$('#wx').onclick = () => setOpen(false);
for (const ev of ['mousedown', 'mouseup', 'click', 'dblclick', 'wheel', 'contextmenu'])
  $('#capture').addEventListener(ev, e => { e.preventDefault(); e.stopPropagation(); }, { passive: false });
$('#win').addEventListener('contextmenu', e => e.preventDefault());
$('#tree').onclick = e => {
  const f = e.target.closest('[data-fold]');
  if (f) { S.fold.has(f.dataset.fold) ? S.fold.delete(f.dataset.fold) : S.fold.add(f.dataset.fold); renderTree(); return; }
  const l = e.target.closest('[data-entry]');
  if (l && l.dataset.entry !== S.entry) { S.try = false; pick(l.dataset.entry, 0); }
};
$('#demos').onclick = e => { const d = e.target.closest('[data-demo]'); if (d) { S.try = false; pick(S.entry, +d.dataset.demo); } };

/* ---- playback: a demo plays when selected, at 30 ticks a second (60 at 2x), and holds its last frame when done;
   Try it yourself creates the same scene with no inputs and hands the stage to the player ---- */
let sim = null, simErr = null, acc = 0, simLayout = null, simCtx = null;
function fail(x) { simErr = x && x.message || String(x); console.error('wiki sim:', x); sim = null; }
function start() {
  sim = null; simErr = null; acc = 0; releaseAll();
  const e = entry(), d = demo();
  if (!d) return;
  if (typeof WikiSim === 'undefined') { simErr = 'sim.js is not loaded'; return; }
  // a demo the layout refuses (an input inside a build's trip, say) shows why on the stage, as a broken entry does
  simLayout = sideLayout(); simCtx = context();
  const make = ctx => S.try ? WikiSim.create(e, d, simLayout, ctx) : WikiSim.play(e, d, simLayout, ctx);
  /* THE UNITS' SCRIPTS run on tools/wiki/serve.py's host, from the gfx pack the stage draws (cob.js). Served any other
     way, or with no pack built, the page plays without them and says why in the console. */
  const cob = typeof WikiCob !== 'undefined' ? WikiCob.cobPage(`tools/wiki/mockup/gfx/${stageDir()}`) : null;
  try { sim = make(cob ? { ...simCtx, cob } : simCtx); }
  catch (x) {
    if (!cob || !/COB host/.test(x && x.message)) fail(x);
    else {
      console.warn('wiki sim: playing without the units\' scripts:', x.message);
      try { sim = make(simCtx); } catch (y) { fail(y); }
    }
  }
  // a type whose script the host refused plays at rest, its gates open (tagpu_wikicob.h)
  for (const n of sim ? sim.cobNotes() : []) if (n) console.warn('wiki sim: a script is quarantined:', n);
  controls(); syncURL();
}
/* a demo holds its last frame once done. A Try-it sim is never done but by a lock -- its scene holds a unit whose cast
   the side leaves unbound (sim.js, decision 12) -- and then holds the same way: the frame under the entry's badge, no
   tick and no input, as a demo stopped at a build the mod cannot reach does */
const held = () => !!sim && sim.done;
function advance(dt) {
  if (!S.open || !sim || !S.playing || held()) { acc = 0; return; }
  acc = Math.min(acc + dt * TICK * S.speed, 8);       // a stalled tab catches up a few ticks, not seconds of them
  while (acc >= 1 && sim && !held()) { try { sim.step(); } catch (x) { fail(x); } acc--; }
}
$('#c-play').onclick = () => { if (held()) start(); else S.playing = !S.playing; controls(); };
$('#c-replay').onclick = () => { S.playing = true; start(); };   // in Try it yourself, the scene afresh
$('#c-x1').onclick = () => { S.speed = 1; controls(); };
$('#c-x2').onclick = () => { S.speed = 2; controls(); };
$('#c-try').onclick = () => { S.try = !S.try; S.playing = true; start(); };
function controls() {
  const has = !!demo(), set = (id, v) => { const el = $(id); if (el.textContent !== v) el.textContent = v; };
  const pause = S.playing && !held();
  $('#c-play').dataset.glyph = pause ? 'i_pause' : 'i_play';
  set('#c-play .gl', pause ? '❚❚' : '▶');
  set('#c-try', S.try ? 'Back to demo' : 'Try it yourself');
  for (const id of ['#c-play', '#c-replay', '#c-x1', '#c-x2', '#c-try']) $(id).disabled = !has;
  $('#c-x1').classList.toggle('on', S.speed === 1); $('#c-x2').classList.toggle('on', S.speed === 2);
  $('#c-try').classList.toggle('on', S.try);
}

/* ---- the stage: a canvas at the resolution's pixels; the map fitted inside it, letterboxed ---- */
const stage = $('#stage');
let FIT = null;                       // the last fit: canvas pixels a world unit, and the map's offset
let overStage = false;                // the player's pointer is over the stage's canvas
function sizeStage() {
  const r = stage.getBoundingClientRect();
  if (!r.width || !r.height) return;                     // the window is closed: keep the last size
  const w = Math.max(1, Math.round(r.width * G.k)), h = Math.max(1, Math.round(r.height * G.k));
  if (stage.width !== w || stage.height !== h) { stage.width = w; stage.height = h; }
}
new ResizeObserver(sizeStage).observe($('#sbox'));

function view() {
  if (!sim) return null;
  try { return sim.view(); } catch (x) { fail(x); return null; }
}
/* the 3D pack the stage draws from: a sim bound to a mod's casting draws from that mod's stage packs, which hold its
   units (gfx.py --stages), where it has one for the entry; any other from the entry's own */
function stageDir() {
  const p = pack();
  return simCtx && simCtx.casting && p && p.stages && p.stages[S.entry] ? `side/${S.mod}/stage/${S.entry}` : S.entry;
}
/* the stage's drawing is stage.js's. Under gfx=game, gfx.js first renders this frame's view in 3D; a COMPLETE image is
   drawn between the op ranges R0 and R2, and anything less leaves the whole whitebox (stage.js, draw()) */
const ICONS = WikiStage.loadIcons(ART + 'kit/');
// the game's own cursors, whenever tools/wiki/gfx.py's pack holds them, in either look: the DLL draws them whenever its
// asset load resolved them, and the kit's icons only where it did not
const CURSORS = WikiStage.loadCursors('gfx/');
let IMG = null;                       // the last frame's 3D answer, for scripted checks
function draw() {
  const g = stage.getContext('2d'), v = view();
  const r = v && window.WikiGfx ? WikiGfx.frame(S.entry, v, sim ? sim.tick : 0, stageDir()) : null;
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
  if ($('#badge').hidden === off) $('#badge').hidden = !off;
  return v;
}

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
function send(ev) { if (!sim || !S.try || sim.done) return; try { sim.input(ev); } catch (x) { fail(x); } }
function releaseAll() { for (const k of down) send({ up: k }); down.clear(); }
// the pointer as a stage point clamped to the content rect: the map, and the sidebar with it. The sim decides what is
// under it (the column, the map, or the dead band under a map shorter than the column)
function world(e) {
  if (!FIT) return null;
  const r = stage.getBoundingClientRect(), px = (e.clientX - r.left) * stage.width / r.width, pz = (e.clientY - r.top) * stage.height / r.height;
  return [clamp((px - FIT.ox) / FIT.s, FIT.x0, FIT.x0 + FIT.cw), clamp((pz - FIT.oz) / FIT.s, 0, FIT.ch)];
}
const BTN = { 0: 'lmb', 2: 'rmb' };
stage.addEventListener('mouseenter', () => { overStage = true; });
stage.addEventListener('mouseleave', () => { overStage = false; });
stage.addEventListener('mousemove', e => { if (S.try) { const w = world(e); if (w) send({ move: w }); } });
stage.addEventListener('mousedown', e => {
  const b = BTN[e.button]; if (!S.try || !b) return;
  e.preventDefault(); const w = world(e); if (w) send({ move: w });
  if (!down.has(b)) { down.add(b); send({ down: b }); }
});
window.addEventListener('mouseup', e => { const b = BTN[e.button]; if (b && down.has(b)) { down.delete(b); send({ up: b }); } });
stage.addEventListener('contextmenu', e => e.preventDefault());
const MOD = { Shift: 'shift', Alt: 'alt', Control: 'ctrl' };
// Z and X with Alt step the armed building's spacing; every down is sent, an auto-repeat included, as the game steps again
const STEP = { KeyZ: 'z', KeyX: 'x' };
window.addEventListener('keydown', e => {
  if (e.key === 'Escape' && S.open) { e.preventDefault(); setOpen(false); return; }
  const z = STEP[e.code];
  if (z && e.altKey && S.open && S.try) { e.preventDefault(); down.add(z); send({ down: z }); return; }
  const m = MOD[e.key]; if (!m || !S.open || !S.try) return;
  e.preventDefault();                  // Alt would otherwise open the browser's menu and keep the key
  if (!down.has(m)) { down.add(m); send({ down: m }); }
});
window.addEventListener('keyup', e => {
  const m = MOD[e.key] || STEP[e.code];
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
  for (const el of document.querySelectorAll('[data-part]')) {
    const cv = el.firstElementChild; if (cv && cv.classList.contains('skin')) cv.remove();
    el._pk = null; el.style.removeProperty('--ink'); el.style.removeProperty('--shade');
  }
  if (S.skin !== 'whitebox' && !KITS[S.skin]) loadKit(S.skin);
  sizeStage();
}
function loadKit(name) {
  const base = `${ART}${name}/`;
  KITS[name] = { loading: true };
  fetch(base + 'kit.json').then(r => { if (!r.ok) throw new Error(`${name}/kit.json: ${r.status}`); return r.json(); }).then(async j => {
    const parts = j.parts, img = {}, light = {};
    await Promise.all(Object.keys(parts).map(n => { const im = new Image(); im.src = `${base}parts/${n}.png`; img[n] = im; return im.decode(); }));
    for (const n in parts) light[n] = faceLight(parts[n], img[n]);
    KITS[name] = { parts, img, light };
  }).catch(x => { console.error('wiki skin:', x); KITS[name] = { error: x.message }; });
}
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
/* every [data-part] box painted with its part at the resolution's pixels, again only when its part, its glyph or its
   size changes. A pressed or active box (.on, .sel, .pressed) shows its _down part, a selected row row_selected; a box
   with data-glyph gets the first of its icons the kit has, unless its part is a whole one with the glyph painted on --
   the controls' play, pause and replay, and the close and wiki buttons -- and a tree row's mark is an icon part of its
   own: no HUD face has these characters, so glyphs.js draws them all as kit icons. The box's text ink follows its
   face: dark on a light face, the style's own colour on a dark one */
function paintParts() {
  const K = KITS[S.skin];
  if (S.skin === 'whitebox' || !K || !K.parts) return;
  const px = G.gh / 720;
  for (const el of document.querySelectorAll('[data-part]')) {
    const w = el.offsetWidth, h = el.offsetHeight; if (!w || !h) continue;
    const base = el.dataset.part, alt = base === 'row' ? 'row_selected' : base + '_down';
    const n = el.matches('.on, .sel, .pressed') && K.parts[alt] ? alt : base, k = K.parts[n]; if (!k) continue;
    const gl = (el.dataset.glyph || '').split(' ').find(q => K.parts[q]);
    const W = Math.max(1, Math.round(w * G.k)), H = Math.max(1, Math.round(h * G.k)), key = `${S.skin}|${n}|${gl}|${W}|${H}`;
    let cv = el.firstElementChild;
    if (!cv || !cv.classList.contains('skin')) { cv = document.createElement('canvas'); cv.className = 'skin'; el.prepend(cv); el._pk = null; }
    if (el._pk === key) continue;
    el._pk = key; cv.width = W; cv.height = H;
    const g = cv.getContext('2d'); g.imageSmoothingQuality = 'high';
    drawPart(g, k, K.img[n], W, H, px);
    if (gl && k.kind !== 'whole') drawIcon(g, K.img[gl], W, H, .6);
    el.style.setProperty('--ink', K.light[n] ? '#1d1d1b' : 'initial');
    el.style.setProperty('--shade', K.light[n] ? 'none' : 'initial');
  }
}
document.addEventListener('pointerdown', e => { const b = e.target.closest('button[data-part]'); if (b) b.classList.add('pressed'); });
window.addEventListener('pointerup', () => { for (const b of document.querySelectorAll('.pressed')) b.classList.remove('pressed'); });

/* ---- text: each of the wiki's text roles in the replay HUD's style nearest it (art/hud/layout.json `styles`: face,
   weight, size in units, colour, letter spacing, case, shadow or outline), the layout's `roles` naming the style. The
   colour and the shadow give way to a face's own ink (--ink, --shade) where the skin's face is light. The elements
   of each role: ---- */
const ROLE = {
  window_title: ['#win .wt'], entry_title: ['#e-title'],
  category: ['#tree .cat'], entry: ['#tree .leaf'], demo: ['#demos li'], demo_num: ['#demos li .n'],
  subtitle: ['#e-sub'], text: ['#text'], caption: ['#caption'],
  control: ['#ctl button'], badge: ['#badge'], keycap: ['.kc'], keys: ['#keys'],
};
/* the layout's other numbers as custom properties of the frame, which wiki.css reads: `--w-` and the number's path,
   a list's items by their index (row.pad -> --w-row-pad-0, --w-row-pad-1) */
(function vars(o, path) {
  for (const [k, v] of Object.entries(o)) {
    const p = [...path, k];
    if (typeof v === 'number') frame.style.setProperty('--w-' + p.join('-'), v);
    else if (v && typeof v === 'object' && !(path.length === 0 && (k === 'geom' || k === 'roles'))) vars(v, p);
  }
})(LAY, []);
fetch('../../../art/hud/layout.json').then(r => r.json()).then(L => {
  const U = n => `calc(${n} * var(--u))`;
  const css = Object.entries(ROLE).map(([n, sel]) => {
    const s = L.styles[LAY.roles[n]];
    const shade = s.outline ? (([a, b, c, d]) => [[a, b], [c, d], [a, d], [c, b]].map(([x, y]) => `${U(x)} ${U(y)} 0 #000`).join(','))(s.outline)
      : s.shadow ? `0 ${U(s.shadow)} 0 rgba(0,0,0,.8)` : 'none';
    return `${sel.join(', ')} { font-family: '${s.font}', sans-serif; font-weight: ${s.weight}; font-size: ${U(s.size)}; font-style: normal;`
      + ` letter-spacing: ${s.spacing}em; text-transform: ${s.upper ? 'uppercase' : 'none'}; color: var(--ink, ${s.colour});`
      + ` text-shadow: var(--shade, ${shade}); }`;
  }).join('\n');
  const st = document.createElement('style'); st.textContent = css; document.head.append(st);
}).catch(x => console.error('wiki styles:', x));

/* ---- the frame loop ---- */
let last = 0;
function loop(t) {
  const dt = last ? Math.min(.25, (t - last) / 1000) : 0; last = t;
  gameTick(dt);
  if (S.open) {
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
window.WikiPage = { state: S, sim: () => sim, image: () => IMG, layout: () => simLayout, context: () => simCtx };
})();
