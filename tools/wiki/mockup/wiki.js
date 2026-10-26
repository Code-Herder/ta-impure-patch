/* wiki.js -- the in-game wiki's mockup: the game frame and Impure's top bar, the wiki window with its tree, demo list and
   entry, and the stage, where stage.js draws what sim.js's view() says. The content is art/wiki/ (tree.json,
   entries/<id>.json) and the behaviour is sim.js's; this file lays out, draws and forwards input. research/notes/ingame-wiki.md is the
   specification the game copies. */
(() => {
const $ = s => document.querySelector(s);
const ART = '../../../art/wiki/', BACKDROP = '../../../research/notes/assets/hud-art/backdrop.png';
const TICK = 30;                      // simulation ticks a second, the game's
const MODS = ['stock', 'mayhem', 'prota', 'twilight', 'escalation', 'tazero', 'unknown'];
const RES = [...$('#res').options].map(o => o.value);
const KEYCAP = { shift: 'Shift', alt: 'Alt', ctrl: 'Ctrl', z: 'Z', x: 'X', lmb: 'Left-click', rmb: 'Right-click' };
const clamp = (v, a, b) => Math.max(a, Math.min(b, v));
const esc = s => String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;');

/* ---- the state, from the URL; the URL is kept up to date so a view can be shared ---- */
const SKINS = ['kit', 'whitebox'];
const S = { res: '1920x1080', size: 90, mode: 'sp', mod: 'stock', skin: 'kit', rewind: true, open: false, entry: null, demo: 0, try: false,
  playing: true, speed: 1, fold: new Set() };
{ const q = new URLSearchParams(location.search);
  if (RES.includes(q.get('res'))) S.res = q.get('res');
  if (q.get('size')) S.size = clamp(Math.round(+q.get('size')) || 90, 50, 100);
  if (q.get('mode') === 'mp') S.mode = 'mp';
  if (MODS.includes(q.get('mod'))) S.mod = q.get('mod');
  if (q.get('rewind') === '0') S.rewind = false;
  if (SKINS.includes(q.get('skin'))) S.skin = q.get('skin');
  S.open = q.has('open'); S.entry = q.get('entry'); S.try = q.has('try');
  if (q.get('demo')) S.demo = Math.max(0, (+q.get('demo') || 1) - 1); }
function syncURL() {
  const q = new URLSearchParams({ res: S.res, size: S.size, mode: S.mode, mod: S.mod, skin: S.skin, rewind: +S.rewind });
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
    .then(j => { ENT[id] = j; }, x => { ENT[id] = { error: x.message }; })));
}
const entry = () => (S.entry && ENT[S.entry] && !ENT[S.entry].error) ? ENT[S.entry] : null;
const demo = () => { const e = entry(); return e && Array.isArray(e.demos) ? e.demos[S.demo] || null : null; };
const locked = e => !!(e && Array.isArray(e.lockedIn) && e.lockedIn.includes(S.mod));

function renderTree() {
  const el = $('#tree');
  if (treeErr) { el.innerHTML = `<div class="row missing">${esc(treeErr)}</div>`; return; }
  if (!TREE) { el.innerHTML = '<div class="row missing">loading…</div>'; return; }
  const walk = (ns, path) => '<ul>' + (ns || []).map((n, i) => {
    const p = `${path}.${i}`;
    if (n.entry) {
      const e = ENT[n.entry], title = e && !e.error ? e.title || n.entry : n.entry;
      return `<li><div class="row leaf${S.entry === n.entry ? ' sel' : ''}${e && e.error ? ' missing' : ''}" data-part="row" data-entry="${esc(n.entry)}">`
        + `<span class="dot">•</span><span>${esc(title)}</span>${locked(e) ? '<span class="lock" title="Turned off in this mod"></span>' : ''}</div></li>`;
    }
    const shut = S.fold.has(p);
    return `<li><div class="row cat" data-part="row" data-fold="${p}"><span class="tw">${shut ? '▸' : '▾'}</span><span>${esc(n.title)}</span></div>`
      + (shut ? '' : walk(n.children, p)) + '</li>';
  }).join('') + '</ul>';
  el.innerHTML = walk(TREE.tree, 't');
}
function renderEntry() {
  const e = entry(), err = S.entry && ENT[S.entry] && ENT[S.entry].error;
  $('#e-title').textContent = e ? e.title || e.id : err ? S.entry : TREE ? 'Choose an entry' : '';
  $('#e-sub').textContent = e ? e.subtitle || '' : '';
  $('#badge').hidden = !locked(e);
  $('#demos').innerHTML = e && Array.isArray(e.demos) && e.demos.length
    ? e.demos.map((d, i) => `<li data-part="row" data-demo="${i}"${i === S.demo ? ' class="sel"' : ''}><span class="n">${i + 1}</span><span>${esc(d.title || d.id)}</span></li>`).join('')
    : `<li class="none">${e ? 'No demos' : ''}</li>`;
  $('#text').innerHTML = err ? `<p class="err">${esc(err)}</p>` : e ? (e.text || []).map(p => `<p>${esc(p)}</p>`).join('') : '';
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
let sim = null, simErr = null, acc = 0;
function fail(x) { simErr = x && x.message || String(x); console.error('wiki sim:', x); sim = null; }
function start() {
  sim = null; simErr = null; acc = 0; releaseAll(); picked = undefined; menuKey = null;
  const e = entry(), d = demo();
  if (!d) return;
  if (typeof WikiSim === 'undefined') { simErr = 'sim.js is not loaded'; return; }
  try { sim = S.try ? WikiSim.create(e, d) : WikiSim.play(e, d); } catch (x) { fail(x); }
  controls(); syncURL();
}
const held = () => !S.try && sim && sim.done;
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
  set('#c-play', S.playing && !held() ? '❚❚' : '▶');
  set('#c-try', S.try ? 'Back to demo' : 'Try it yourself');
  for (const id of ['#c-play', '#c-replay', '#c-x1', '#c-x2', '#c-try']) $(id).disabled = !has;
  $('#c-x1').classList.toggle('on', S.speed === 1); $('#c-x2').classList.toggle('on', S.speed === 2);
  $('#c-try').classList.toggle('on', S.try);
  $('#stage').classList.toggle('try', S.try && !!sim);
  $('#bmenu').hidden = !S.try;
}

/* ---- the stage: a canvas at the resolution's pixels; the map fitted inside it, letterboxed ---- */
const stage = $('#stage');
let FIT = null;                       // the last fit: canvas pixels a world unit, and the map's offset
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
// the stage's drawing is stage.js's; gfx=game hands an element to gfx.js, and where that returns false the whitebox stands
const ICONS = WikiStage.loadIcons(ART + 'kit/');
function draw() {
  const g = stage.getContext('2d'), v = view();
  FIT = WikiStage.draw(g, v, { W: stage.width, H: stage.height, U: G.gh / 720, icons: ICONS,
    msg: simErr || (demo() ? 'loading…' : entry() ? 'no demo' : ''),
    gx: (phase, c, ...a) => !!(window.WikiGfx && WikiGfx.draw(phase, g, c, ...a)) });
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
  const cap = v ? v.caption || '' : '';
  if (cap !== shownCap) { $('#caption').textContent = shownCap = cap; }
  const ks = keysOf(v).join('+');
  if (ks !== shownKeys) {
    shownKeys = ks;
    $('#keys').innerHTML = ks ? ks.split('+').map(n => `<span class="kc" data-part="keycap">${esc(KEYCAP[n] || n)}</span>`).join('<span>+</span>') : '';
  }
}

/* ---- Try it yourself: the stage's mouse and the modifier keys go to sim.input, the build menu picks a build ---- */
const down = new Set();                // the buttons and keys sent down and not yet up
function send(ev) { if (!sim || !S.try) return; try { sim.input(ev); } catch (x) { fail(x); } }
function releaseAll() { for (const k of down) send({ up: k }); down.clear(); }
function world(e) {
  if (!FIT) return null;
  const r = stage.getBoundingClientRect(), px = (e.clientX - r.left) * stage.width / r.width, pz = (e.clientY - r.top) * stage.height / r.height;
  const W = FIT.mw * FIT.cell, H = FIT.mh * FIT.cell;
  return [clamp((px - FIT.ox) / FIT.s, 0, W), clamp((pz - FIT.oz) / FIT.s, 0, H)];
}
const BTN = { 0: 'lmb', 2: 'rmb' };
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

/* the build menu lists what the selection can build among the entry's own units -- a stock build list names the whole
   side, the entry only the buildings it shows -- and marks the one view().mode says is armed ('build:<TYPE>') */
let picked, menuKey = null;
function buildMenu(v) {
  if (!S.try) return;
  const e = entry(), U = (e && e.units) || {}, sel = (v && v.units || []).filter(u => u.selected);
  const opts = [...new Set(sel.flatMap(u => (U[u.type] && (U[u.type].builds || U[u.type].buildlist)) || []))].filter(t => t in U);
  const mode = v && typeof v.mode === 'string' && v.mode.startsWith('build:') ? v.mode.slice(6) : null;
  const cur = v && typeof v.mode === 'string' ? mode : picked;
  const key = opts.join() + '|' + cur;
  if (key === menuKey) return;
  menuKey = key;
  $('#bmenu').innerHTML = '<div class="h">Build</div>' + (opts.length
    ? opts.map(t => `<button data-part="button" data-build="${esc(t)}"${t === cur ? ' class="on"' : ''}>${esc(t)}</button>`).join('')
      + `<button data-part="button" data-build=""${cur == null ? ' class="on"' : ''}>none</button>`
    : '<div class="e">Select a constructor</div>');
}
$('#bmenu').onclick = e => { const b = e.target.closest('[data-build]'); if (!b) return; picked = b.dataset.build || null; send({ build: picked }); menuKey = null; };

/* ---- skins: the whitebox, or the kit of art/wiki/kit/ -- kit.json in art/hud/kit.json's format and its parts as
   PNGs. GEOM places what the kit's window art leaves room for, in units: the title in its dark title groove (wt: left,
   right, top, height), the close button centred in its close bay (wx: right, top, width, height), the body inside its
   frame (wb), and how deep its panels' frame (pn) and its bezel's (bz) are, each [top, right, bottom, left]. MEASURED
   2026-10-25 from the kit's window.png, panel.png and bezel.png at its scale: the groove's and the bay's dark spans
   across the title band, and the frames' inner edges where the face's tone begins; the body keeps 3 units more off
   the frame. ---- */
const GEOM = {
  whitebox: { wt: [0, 22, 0, 20], wx: [0, 0, 22, 20], wb: [20, 0, 0, 0], gap: 0, pn: [0, 0, 0, 0], bz: [0, 0, 0, 0] },
  kit: { wt: [2.6, 20.4, 2.4, 15.7], wx: [3.5, 3.2, 14, 14], wb: [23, 5.6, 5.6, 5.5], gap: 3, pn: [2.5, 2.6, 2.1, 2.5], bz: [4.1, 4.0, 3.7, 4.1] },
};
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
/* every [data-part] box painted with its part at the resolution's pixels, again only when its part or size changes. A
   pressed or active box (.on, .sel, .pressed) shows its _down part, a selected row row_selected; a box with data-glyph
   gets the first of its icons the kit has, unless its part is a whole one with the glyph painted on. The box's text
   ink follows its face: dark on a light face, the style's own colour on a dark one */
function paintParts() {
  const K = KITS[S.skin];
  if (S.skin === 'whitebox' || !K || !K.parts) return;
  const px = G.gh / 720;
  for (const el of document.querySelectorAll('[data-part]')) {
    const w = el.offsetWidth, h = el.offsetHeight; if (!w || !h) continue;
    const base = el.dataset.part, alt = base === 'row' ? 'row_selected' : base + '_down';
    const n = el.matches('.on, .sel, .pressed') && K.parts[alt] ? alt : base, k = K.parts[n]; if (!k) continue;
    const W = Math.max(1, Math.round(w * G.k)), H = Math.max(1, Math.round(h * G.k)), key = `${S.skin}|${n}|${W}|${H}`;
    let cv = el.firstElementChild;
    if (!cv || !cv.classList.contains('skin')) { cv = document.createElement('canvas'); cv.className = 'skin'; el.prepend(cv); el._pk = null; }
    if (el._pk === key) continue;
    el._pk = key; cv.width = W; cv.height = H;
    const g = cv.getContext('2d'); g.imageSmoothingQuality = 'high';
    drawPart(g, k, K.img[n], W, H, px);
    const gl = (el.dataset.glyph || '').split(' ').find(q => K.parts[q]);
    if (gl && k.kind !== 'whole') drawIcon(g, K.img[gl], W, H, .6);
    el.style.setProperty('--ink', K.light[n] ? '#1d1d1b' : 'initial');
    el.style.setProperty('--shade', K.light[n] ? 'none' : 'initial');
  }
}
document.addEventListener('pointerdown', e => { const b = e.target.closest('button[data-part]'); if (b) b.classList.add('pressed'); });
window.addEventListener('pointerup', () => { for (const b of document.querySelectorAll('.pressed')) b.classList.remove('pressed'); });

/* ---- text: each of the wiki's text roles in the replay HUD's style nearest it (art/hud/layout.json `styles`: face,
   weight, size in units, colour, letter spacing, case, shadow or outline). The colour and the shadow give way to a
   face's own ink (--ink, --shade) where the skin's face is light ---- */
const ROLE = {
  title: ['#win .wt', '#e-title'],                    // the window's title and the entry's: the stats window's title
  header: ['#tree .cat'],                             // a category: the HUD's team header (and the picker's title)
  name: ['#tree .leaf', '#demos li'],                 // an entry or a demo: a player row's name
  num: ['#demos li .n'],                              // a demo's number: the HUD's numbers
  window_foot: ['#e-sub', '#text'],                   // the subtitle and the body text: the stats window's foot, its one mixed-case style
  toast: ['#caption'],                                // the caption, a line written at a moment: the HUD's toast
  face_big: ['#ctl button'],                          // play, replay, 1x/2x, Try it yourself: the transport's labels and Take over replay
  face_chip: ['#bmenu button', '#badge'],             // the build menu's buttons and the badge: the stats window's chips
  face_num_sm: ['.kc'],                               // a keycap: a transport button's small label
  label: ['#bmenu .h', '#bmenu .e', '#keys'],         // the build menu's heading, the keys strip's '+': the HUD's labels
};
fetch('../../../art/hud/layout.json').then(r => r.json()).then(L => {
  const U = n => `calc(${n} * var(--u))`;
  const css = Object.entries(ROLE).map(([n, sel]) => {
    const s = L.styles[n];
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
    const v = draw(); strip(v); buildMenu(v);
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
$('#mod').value = S.mod; $('#mod').onchange = e => { S.mod = e.target.value; renderTree(); renderEntry(); syncURL(); };
window.addEventListener('resize', layout);

applySkin(); layout(); renderTree(); renderEntry(); controls();
load().then(() => {
  if (S.entry && !(S.entry in ENT)) S.entry = null;
  if (S.open && !S.entry && TREE) S.entry = leaves(TREE.tree)[0] || null;
  const d = entry() && entry().demos; if (d && S.demo >= d.length) S.demo = 0;
  setOpen(S.open); start(); render();
});
requestAnimationFrame(loop);
// the page's state and its running simulation, for scripted checks of the mockup
window.WikiPage = { state: S, sim: () => sim };
})();
