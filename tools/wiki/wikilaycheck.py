#!/usr/bin/env python3
"""wikilaycheck.py -- the DLL's wiki window (tagpu_wikilay.c) against the mockup's (tools/wiki/mockup/).

    python3 tools/wiki/wikilaycheck.py [--keep DIR]
    python3 tools/wiki/wikilaycheck.py --check-only

First, and alone with --check-only (no browser), the layout's load-time check, tagpu_wikilay_check as the game
runs it, with every casting of every side pack present under tools/wiki/mockup/gfx/side/ (gitignored, built by
tools/wiki/sidepacks.py) besides the entries' own names and the worst castings: the page must build whole and every
caption of a real casting show whole in its strip (wikilaycheck.c --check).

For each case below it opens the mockup headless (playwright-cli, as tools/hudart/laycheck.py
does) at the case's resolution, one CSS pixel a device pixel -- the game's grid -- with the
scrollbars a real browser shows (Playwright's headless default hides them, and a hidden scrollbar
takes no room). It takes the page's state and every element in units, runs wikilaycheck.c --
tagpu_wikilay.c compiled natively over the archive the DLL embeds -- on the same state, and pairs
the two: every part by name, every control by its key, every disc by its colour, every text line by
its string, with its start, its width and its baseline, each panel's scroll limit, and each
scrollbar thumb against the one Chrome paints (found in a screenshot with the thumb recoloured).
It prints the worst difference per kind and every element only one side has, and exits 1 when
anything differs by more than TOL_CSS or is unmatched.

A baseline that differs where Chrome rounds the face's ascent or descent to another whole pixel
than round(em x size) does, and that the same rule on Chrome's own metrics reproduces, is named,
not failed, as laycheck.py does.

A character no HUD face has would be set by the mockup in the browser's fallback face and by the
game as its face's .notdef: the check gives the C layout the fallback face's advances, so the rules
are still compared on what the mockup sets, and it fails naming every such string. The controls'
play, pause and replay and the tree's marks, which no face has, are kit icons (glyphs.js) for that
reason, paired as parts.

The standard library, gcc and playwright-cli; it serves the repository root itself.
"""
import html
import http.server
import json
import math
import os
import shutil
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "tagpu" / "ddraw" / "src"
# Chrome lays out in 1/64 of a CSS pixel and snaps each nested box to it, so the mockup's boxes
# carry up to a few 1/64ths of rounding; past that, a difference is a rule disagreeing.
TOL_CSS = 0.07
# a thumb is painted on whole pixels: the DLL's and Chrome's may round its edge apart by one
TOL_THUMB_PX = 1.0

# (name, resolution, the mockup's URL switches, what is done to the page before it is read):
# fold -- category rows clicked, by their path in the tree; speed -- 1x or 2x pressed; held -- a
# control held down; tick -- the demo paused and stepped to that tick, so its caption and keys
# show; scroll -- each panel's scroll, in pixels, or "max" or "half" of its limit; try -- Try it
# yourself pressed
CASES = [
    ("1080-stock-quickbuild", (1920, 1080), "mod=stock&entry=quickbuild", {}),
    ("1080-esc-quickbuild-locked", (1920, 1080), "mod=escalation&entry=quickbuild", {"tick": 75}),
    ("1440-esc-buildshapes", (2560, 1440), "mod=escalation&entry=buildshapes&demo=3", {"speed": 2, "tick": 60}),
    ("4k-stock-quickbuild-folded", (3840, 2160), "mod=stock&entry=quickbuild", {"fold": ["t.0.0"]}),
    ("4k-esc-buildshapes-scrolled", (3840, 2160), "mod=escalation&entry=buildshapes", {"scroll": {"text": "half"}}),
    ("3440-tazero-quickbuild-held", (3440, 1440), "mod=tazero&entry=quickbuild&demo=5", {"held": "c-try", "tick": 40}),
    ("1080-tazero-buildshapes-locked", (1920, 1080), "mod=tazero&entry=buildshapes&demo=2", {}),
    ("3440-stock-buildshapes-allfolded", (3440, 1440), "mod=stock&entry=buildshapes&rewind=0", {"fold": ["t.0"]}),
    ("1024-esc-quickbuild", (1024, 768), "mod=escalation&entry=quickbuild", {"tick": 100}),
    ("1024-stock-buildshapes-scrolled", (1024, 768), "mod=stock&entry=buildshapes&demo=2",
     {"scroll": {"text": "max"}, "tick": 30}),
    ("1024-esc-quickbuild-scrolled", (1024, 768), "mod=escalation&entry=quickbuild", {"scroll": {"text": 37}}),
    # Try it yourself: the stage across the whole row
    ("1080-stock-quickbuild-try", (1920, 1080), "mod=stock&entry=quickbuild", {"try": True}),
    ("4k-stock-buildshapes-try", (3840, 2160), "mod=stock&entry=buildshapes&demo=2", {"try": True}),
    ("1024-stock-quickbuild-try", (1024, 768), "mod=stock&entry=quickbuild", {"try": True}),
    # Interface Type 0: the button words of the titles, the paragraphs and the caption are the left-click ones
    ("1080-stock-quickbuild-iface0", (1920, 1080), "mod=stock&entry=quickbuild&iface=0", {"tick": 75}),
]

KEYNAME = {"Shift": "shift", "Alt": "alt", "Ctrl": "ctrl", "Z": "z", "X": "x", "E": "e", "Left-click": "lmb", "Right-click": "rmb"}

COLLECT = r"""async () => {
  const A = ACTIONS, PARTS = KITPARTS, CPS = FALLBACK, ROLES = ROLEMAP;
  const raf = () => new Promise(r => requestAnimationFrame(() => r()));
  const S = WikiPage.state, $ = s => document.querySelector(s);
  // everything fetched first: once the page is read, nothing awaits, so the demo playing cannot move on
  // between the elements and the state
  const tree = await fetch('../../../art/wiki/tree.json').then(r => r.json());
  const L = await fetch('../../../art/hud/layout.json').then(r => r.json());
  // the tree in preorder, as tagpu_wikidata.c numbers its nodes: a path each, as the mockup names folds
  const nodes = [], leaves = [];
  (function walk(ns, path, depth) { (ns || []).forEach((n, i) => { const p = `${path}.${i}`; nodes.push({ p, entry: n.entry });
    if (n.entry) leaves.push(n.entry); else walk(n.children, p, depth + 1); }); })(tree.tree, 't', 0);
  for (const p of A.fold || []) $(`[data-fold="${p}"]`).click();
  if (A.speed) $(A.speed === 2 ? '#c-x2' : '#c-x1').click();
  if (A.try) { $('#c-try').click(); await raf(); await raf(); }
  if (A.tick != null) {
    if (S.playing) $('#c-play').click();
    const sim = WikiPage.sim();
    while (sim && sim.tick < A.tick && !sim.done) sim.step();
  }
  if (A.held) $('#' + A.held).dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
  await raf(); await raf();
  const SCR = { tree: '#tree', demos: '#demos', text: '#text' };
  for (const [k, v] of Object.entries(A.scroll || {})) {
    const e = $(SCR[k]), mx = e.scrollHeight - e.clientHeight;
    e.scrollTop = v === 'max' ? mx : v === 'half' ? Math.round(mx / 2) : v;
  }
  await raf(); await raf();

  const f = $('#frame').getBoundingClientRect(), u = f.height / 720;
  const R = r => [(r.left - f.left) / u, (r.top - f.top) / u, r.width / u, r.height / u];
  const B = e => R(e.getBoundingClientRect());
  const client = e => { const r = e.getBoundingClientRect(), sb = e.offsetWidth - e.clientWidth;
    return [(r.left - f.left) / u, (r.top - f.top) / u, (r.width - sb) / u, r.height / u]; };
  const overlapY = (a, k) => a[1] < k[1] + k[3] && a[1] + a[3] > k[1];
  const inter = (a, k) => { const x0 = Math.max(a[0], k[0]), y0 = Math.max(a[1], k[1]), x1 = Math.min(a[0] + a[2], k[0] + k[2]), y1 = Math.min(a[1] + a[3], k[1] + k[3]);
    return [x0, y0, Math.max(0, x1 - x0), Math.max(0, y1 - y0)]; };
  // the box an element is clipped to: its scroller's client box, or the strip that hides its overflow
  const clipOf = e => { const s = e.closest('.scr'); if (s) return client(s); const k = e.closest('#caption, #keys, .wt'); return k ? B(k) : null; };
  const els = [];
  const shown = e => e.getClientRects().length && getComputedStyle(e).visibility !== 'hidden' && !e.closest('[hidden]');
  for (const e of document.querySelectorAll('#frame [data-part]')) {
    if (!shown(e) || !e._pk) continue;
    const r = B(e), k = clipOf(e);
    if (k && !overlapY(r, k)) continue;
    els.push({ kind: 'part', part: e._pk.split('|')[1], r });
    const gl = (e.dataset.glyph || '').split(' ').find(q => PARTS.includes(q));
    if (gl) els.push({ kind: 'part', part: gl, r: [r[0] + r[2] * .2, r[1] + r[3] * .2, r[2] * .6, r[3] * .6] });
  }
  const hot = (e, key, box) => { if (!e || !shown(e)) return; let r = box || B(e); const k = clipOf(e); if (k && e.closest('.scr') !== e) r = inter(r, k);
    if (r[2] > 0 && r[3] > 0) els.push({ kind: 'hot', key, r }); };
  hot($('#b-wiki'), 'wiki'); hot($('#win'), 'window'); hot($('#wx'), 'close');
  for (const [k, sel] of Object.entries(SCR)) hot($(sel), 'scroll:' + k, client($(sel)));
  for (const e of document.querySelectorAll('[data-fold]')) hot(e, 'fold:' + nodes.findIndex(n => n.p === e.dataset.fold));
  for (const e of document.querySelectorAll('[data-entry]')) hot(e, 'entry:' + nodes.findIndex(n => n.entry === e.dataset.entry));
  for (const e of document.querySelectorAll('[data-demo]')) hot(e, 'demo:' + e.dataset.demo);
  for (const [id, key] of [['c-play', 'play'], ['c-replay', 'replay'], ['c-x1', 'x1'], ['c-x2', 'x2'], ['c-try', 'try']])
    if (!$('#' + id).disabled) hot($('#' + id), key);
  hot($('#stage'), 'stage');
  // the discs: a 1 px border round a fill
  for (const e of document.querySelectorAll('#win .lock, #badge i')) {
    if (!shown(e)) continue;
    const r = B(e), k = clipOf(e), p = 1 / u;
    if (k && !overlapY(r, k)) continue;
    els.push({ kind: 'dot', rgb: '5a0808', r }, { kind: 'dot', rgb: 'd01818', r: [r[0] + p, r[1] + p, r[2] - 2 * p, r[3] - 2 * p] });
  }
  // every line of text: its characters grouped by baseline, each character's baseline its box's top plus the
  // ascent of the face it is set in
  const ctx = document.createElement('canvas').getContext('2d');
  const walker = document.createTreeWalker($('#frame'), NodeFilter.SHOW_TEXT);
  for (let t = walker.nextNode(); t; t = walker.nextNode()) {
    const e = t.parentElement;
    if (!e.closest('#win, #b-wiki') || e.closest('.gl') || !shown(e) || !t.data.trim()) continue;
    const cs = getComputedStyle(e);
    ctx.font = `${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`;
    const lines = [], rg = document.createRange();
    for (let i = 0; i < t.data.length; i++) {
      rg.setStart(t, i); rg.setEnd(t, i + 1);
      const rs = rg.getClientRects(); if (!rs.length) continue;
      const m = ctx.measureText(t.data[i]), base = rs[0].top + m.fontBoundingBoxAscent;
      let L = lines.find(l => Math.abs(l.base - base) < 2 && Math.abs(l.top - rs[0].top) < 40);
      if (!L || (L.last !== i - 1)) { L = lines.find(l => l.last === i - 1 && Math.abs(l.base - base) < 2); }
      if (!L) { L = { s: '', base, top: rs[0].top, bot: rs[0].bottom, x0: Infinity, x1: -Infinity, last: i, first: i }; lines.push(L); }
      L.s += t.data[i]; L.last = i;
      if (t.data[i] !== ' ') { L.x0 = Math.min(L.x0, rs[0].left); L.x1 = Math.max(L.x1, rs[0].right);
        if (/^[\x20-\x7e -ÿ]$/.test(t.data[i])) { L.base = base; L.top = Math.min(L.top, rs[0].top); L.bot = Math.max(L.bot, rs[0].bottom); } }
    }
    const k = clipOf(e), fm = ctx.measureText('M');
    for (const L of lines) {
      const s = L.s.trim(); if (!s) continue;
      const box = [(L.x0 - f.left) / u, (L.top - f.top) / u, (L.x1 - L.x0) / u, (L.bot - L.top) / u];
      if (k && !overlapY(box, k)) continue;
      els.push({ kind: 'text', text: s, r: box, base: (L.base - f.top) / u, size: parseFloat(cs.fontSize) / u,
                 asc: fm.fontBoundingBoxAscent, desc: fm.fontBoundingBoxDescent, colour: cs.color, shadow: cs.textShadow });
    }
  }
  // the browser's advance of each code point the HUD faces lack, in em, by style
  const fallback = {};
  for (const sn of new Set(Object.values(ROLES))) {
    const st = L.styles[sn]; fallback[sn] = {};
    ctx.font = `${st.weight} 100px '${st.font}', sans-serif`;
    for (const c of CPS) fallback[sn][c.codePointAt(0)] = ctx.measureText(c).width / 100;
  }
  // the reference string's width at each style's size, from which the C side finds the size Chrome sets the face at
  const sizes = {};
  for (const sn of new Set(Object.values(ROLES))) {
    const st = L.styles[sn], px = Math.fround(st.size * u);
    ctx.font = `${st.weight} ${px}px '${st.font}'`;
    sizes[sn] = { [px.toFixed(4)]: ctx.measureText('Hold Shift to queue one deposit after another').width };
  }
  const scroll = {};
  for (const [k, sel] of Object.entries(SCR)) { const e = $(sel);
    scroll[k] = { top: e.scrollTop / u, max: (e.scrollHeight - e.clientHeight) / u, bar: e.offsetWidth - e.clientWidth,
                  track: [...client(e).slice(0, 2), (e.offsetWidth - e.clientWidth) / u, e.clientHeight / u],
                  page: [e.getBoundingClientRect().right - (e.offsetWidth - e.clientWidth), e.getBoundingClientRect().top, e.clientHeight] }; }
  const sim = WikiPage.sim();
  const state = { W: f.width, H: f.height, bar: 32, rewind: S.rewind, open: S.open, entry: leaves.indexOf(S.entry), demo: S.demo,
    fold: [...S.fold].map(p => nodes.findIndex(n => n.p === p)), scroll: ['tree', 'demos', 'text'].map(k => scroll[k].top),
    playing: S.playing, done: !!(sim && sim.done && !S.try), speed: S.speed, try: S.try, mod: S.mod,
    sim_locked: !!(sim && sim.view().locked), held: A.held ? A.held.replace('c-', '') : '',
    caption: $('#caption').textContent, keys: [...document.querySelectorAll('#keys .kc')].map(e => e.textContent), fallback, sizes,
    iface: S.iface, casting: (WikiPage.context() || {}).casting || null };
  return JSON.stringify({ state, els, u, scroll, frame: [f.left, f.top] });
}"""


def face_metrics():
    """Each text style's ascent and descent in em, descent positive, as tagpu_hudart.c face_open
    reads them (laycheck.py's rule)."""
    lay = json.loads((ROOT / "art/hud/layout.json").read_text())
    out = {}
    for name, st in lay["styles"].items():
        b = (ROOT / "art/hud/fonts" / f"{st['font'].lower().replace(' ', '')}-{st['weight']}.ttf").read_bytes()
        tab = {b[12 + 16 * i:16 + 16 * i].decode("latin-1"): struct.unpack(">I", b[20 + 16 * i:24 + 16 * i])[0]
               for i in range(struct.unpack(">H", b[4:6])[0])}
        upem = struct.unpack(">H", b[tab["head"] + 18:tab["head"] + 20])[0]
        os2 = tab.get("OS/2")
        if os2 is not None and struct.unpack(">H", b[os2 + 62:os2 + 64])[0] & 0x80:
            asc, desc = struct.unpack(">hh", b[os2 + 68:os2 + 72])
        else:
            asc, desc = struct.unpack(">hh", b[tab["hhea"] + 4:tab["hhea"] + 8])
        out[name] = [asc / upem, -desc / upem]
    return out


def cmap(path):
    """The code points a TrueType face maps (formats 4 and 12)."""
    b = path.read_bytes()
    tab = {b[12 + 16 * i:16 + 16 * i].decode("latin-1"): struct.unpack(">I", b[20 + 16 * i:24 + 16 * i])[0]
           for i in range(struct.unpack(">H", b[4:6])[0])}
    c = tab["cmap"]
    cps = set()
    for i in range(struct.unpack(">H", b[c + 2:c + 4])[0]):
        o = c + struct.unpack(">I", b[c + 8 + 8 * i:c + 12 + 8 * i])[0]
        fmt = struct.unpack(">H", b[o:o + 2])[0]
        if fmt == 4:
            n2 = struct.unpack(">H", b[o + 6:o + 8])[0]
            ends = struct.unpack(f">{n2 // 2}H", b[o + 14:o + 14 + n2])
            starts = struct.unpack(f">{n2 // 2}H", b[o + 16 + n2:o + 16 + 2 * n2])
            for a, e in zip(starts, ends):
                cps.update(range(a, e + 1))
        elif fmt == 12:
            for g in range(struct.unpack(">I", b[o + 12:o + 16])[0]):
                a, e, _ = struct.unpack(">III", b[o + 16 + 12 * g:o + 28 + 12 * g])
                cps.update(range(a, e + 1))
    return cps


def lacking():
    """Every character of the wiki's strings that some HUD face lacks: the mockup's own (index.html,
    wiki.js) and the data's."""
    have = None
    for f in (ROOT / "art/hud/fonts").glob("*.ttf"):
        cps = cmap(f)
        have = cps if have is None else have & cps
    text = (ROOT / "tools/wiki/mockup/wiki.js").read_text() + html.unescape((ROOT / "tools/wiki/mockup/index.html").read_text())
    for p in (ROOT / "art/wiki").rglob("*.json"):
        text += p.read_text()
    return sorted({ch for ch in text if ord(ch) > 0x7E and ord(ch) not in have})


def png_rows(path):
    """An 8-bit RGB or RGBA PNG, non-interlaced, as (width, height, rows of bytes, bytes a pixel)."""
    b = path.read_bytes()
    pos, idat, w = 8, b"", 0
    while pos < len(b):
        n, typ = struct.unpack(">I4s", b[pos:pos + 8])
        data = b[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", data[:10])
            bpp = {2: 3, 6: 4}[ctype]
        elif typ == b"IDAT":
            idat += data
        pos += 12 + n
    raw, rows, prev = zlib.decompress(idat), [], bytearray(w * bpp)
    stride = w * bpp
    for y in range(h):
        ft, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            c = prev[i - bpp] if i >= bpp else 0
            bb = prev[i]
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + bb) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + bb) // 2) & 255
            elif ft == 4:
                p = a + bb - c
                pa, pb, pc = abs(p - a), abs(p - bb), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else bb if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, rows, bpp


def result(out):
    """The value of a playwright-cli eval: the lines between `### Result` and the next `###`."""
    lines, on = [], False
    for l in out.splitlines():
        if l.startswith("### "):
            if on:
                break
            on = l.strip() == "### Result"
            continue
        if on:
            lines.append(l)
    return json.loads("\n".join(lines)) if lines else None


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


def serve():
    handler = lambda *a, **k: Quiet(*a, directory=str(ROOT), **k)
    httpd = socketserver.TCPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


def pair(cs, ds, key):
    """Greedy nearest pairing of same-key items; returns (pairs, only C, only DOM)."""
    pairs, left, onlyc = [], list(ds), []
    for c in cs:
        best, bd = None, None
        for d in left:
            if key(d) != key(c):
                continue
            dist = sum(abs(a - b) for a, b in zip(c["r"], d["r"]))
            if bd is None or dist < bd:
                best, bd = d, dist
        if best is None:
            onlyc.append(c)
        else:
            left.remove(best)
            pairs.append((c, best))
    return pairs, onlyc, left


def thumbs(run, tmp, dom):
    """Each scrollbar's thumb as Chrome paints it, in units: the thumb recoloured magenta, the page
    screenshot, and the magenta run down the scrollbar's middle column."""
    shown = {k: v for k, v in dom["scroll"].items() if v["bar"] > 0 and v["max"] > 0}
    if not shown:
        return {}
    # a custom scrollbar keeps the style it was made with: each scroller's is made again, its scroll kept
    run("eval", "() => { const s = document.createElement('style'); s.id = 'wlc-thumb';"
                " s.textContent = '#win ::-webkit-scrollbar-thumb { background: #ff00ff !important; }'; document.head.append(s);"
                " for (const e of document.querySelectorAll('#win .scr')) { const t = e.scrollTop; e.style.overflow = 'hidden';"
                " void e.offsetHeight; e.style.overflow = ''; e.scrollTop = t; } return 1; }")
    shot = tmp / "shot.png"
    run("screenshot", f"--filename={shot}")
    run("eval", "() => { document.getElementById('wlc-thumb').remove(); return 1; }")
    w, h, rows, bpp = png_rows(shot)
    out, u, (fx, fy) = {}, dom["u"], dom["frame"]
    for k, v in shown.items():
        x0, top, ch = v["page"]
        x = int(x0 + v["bar"] / 2)
        ys = [y for y in range(int(top), int(top + ch)) if 0 <= y < h
              and rows[y][x * bpp] > 230 and rows[y][x * bpp + 1] < 40 and rows[y][x * bpp + 2] > 230]
        if ys:
            out[k] = [(min(ys) - fy) / u, (max(ys) + 1 - min(ys)) / u]
    return out


def main():
    keep = None
    if len(sys.argv) == 3 and sys.argv[1] == "--keep":
        keep = Path(sys.argv[2])
    only = sys.argv[1:] == ["--check-only"]
    t0 = time.time()
    tmp = Path(tempfile.mkdtemp(prefix="wikilaycheck"))
    exe, pak = tmp / "wikilaycheck", tmp / "wikipak.bin"
    cc = subprocess.run(["gcc", "-O1", "-std=c99", "-Wall", f"-I{SRC}", f"-I{SRC.parent / 'inc'}",
                         str(ROOT / "tools/wiki/wikilaycheck.c"), *(str(SRC / f) for f in (
                             "tagpu_wikilay.c", "tagpu_wikitext.c", "tagpu_hudlay.c", "tagpu_hudtext.c", "tagpu_wikidata.c", "tagpu_json.c",
                             "tagpu_pak.c", "lodepng.c")), "-lm", "-o", str(exe)], capture_output=True, text=True)
    if cc.returncode:
        sys.exit("wikilaycheck: the native build failed\n" + cc.stderr)
    pk = subprocess.run([sys.executable, str(ROOT / "tools/hudpak.py"), "--set", "wiki", str(ROOT / "art/wiki"), str(pak)],
                        capture_output=True, text=True)
    if pk.returncode:
        sys.exit("wikilaycheck: hudpak failed\n" + pk.stderr)
    packs = sorted(str(p) for p in (ROOT / "tools/wiki/mockup/gfx/side").glob("*/side.json"))
    ck = subprocess.run([str(exe), "--check", str(ROOT / "art/hud"), str(ROOT / "art/wiki"), str(pak), *packs],
                        capture_output=True, text=True)
    print((ck.stdout or ck.stderr).strip())
    if ck.returncode or only:
        shutil.rmtree(tmp, ignore_errors=True)
        return ck.returncode
    faces = face_metrics()
    kitparts = list(json.loads((ROOT / "art/wiki/kit/kit.json").read_text())["parts"])
    roles = json.loads((ROOT / "art/wiki/layout.json").read_text())["roles"]
    fallback = lacking()
    httpd = serve()
    cfg = tmp / "pw.json"
    cfg.write_text(json.dumps({"browser": {"contextOptions": {"deviceScaleFactor": 1, "viewport": {"width": 1200, "height": 900}},
                                           "launchOptions": {"ignoreDefaultArgs": ["--hide-scrollbars"]}}}))
    pw = ["playwright-cli", f"-s=wikilaycheck{os.getpid()}", f"--config={cfg}"]
    run = lambda *a: subprocess.run(pw + list(a), capture_output=True, text=True, cwd=tmp)
    bad, glyphs = 0, set()
    run("open", "about:blank")
    try:
        for name, (gw, gh), query, acts in CASES:
            # the frame fits the page's width less 34 and its height less the controls: a viewport
            # 34 wider than the screen and tall enough draws the frame at the screen's own size
            run("resize", str(gw + 34), str(gh + 900))
            run("goto", f"http://127.0.0.1:{httpd.server_address[1]}/tools/wiki/mockup/index.html?res={gw}x{gh}&open&skin=kit&{query}")
            # the mod's sidebar pack waited for too: a demo started before it arrived restarts on its
            # layout, which can stop it at a build that layout cannot reach, under the badge
            for _ in range(80):
                ok = result(run("eval", "() => !!(window.WikiPage && document.querySelectorAll('#text p').length"
                                " && document.querySelector('#win canvas.skin') && getComputedStyle(document.querySelector('#e-title')).fontFamily.includes('Oswald')"
                                " && document.fonts.status === 'loaded' && !document.querySelector('#sidebar-note').textContent.startsWith('loading'))").stdout)
                if ok:
                    break
                time.sleep(0.1)
            code = (COLLECT.replace("ACTIONS", json.dumps(acts)).replace("KITPARTS", json.dumps(kitparts))
                    .replace("FALLBACK", json.dumps(fallback)).replace("ROLEMAP", json.dumps(roles)))
            r = run("eval", code)
            v = result(r.stdout)
            if not v:
                sys.exit(f"wikilaycheck: the mockup gave nothing for {name}\n{r.stdout[-2000:]}\n{r.stderr}")
            dom = json.loads(v)
            if (dom["state"]["W"], dom["state"]["H"]) != (gw, gh):
                sys.exit(f"wikilaycheck: {name}: the frame is {dom['state']['W']} x {dom['state']['H']}, not the screen's {gw} x {gh}")
            dom["state"]["keys"] = [KEYNAME[k] for k in dom["state"]["keys"]]
            dom["thumbs"] = thumbs(run, tmp, dom)
            sp = tmp / f"{name}.state.json"
            sp.write_text(json.dumps(dom["state"]))
            r = subprocess.run([str(exe), str(ROOT / "art/hud"), str(ROOT / "art/wiki"), str(pak), str(sp)], capture_output=True, text=True)
            if r.returncode:
                sys.exit(f"wikilaycheck: {name}: {r.stderr}")
            ops = json.loads(r.stdout)
            if keep:
                keep.mkdir(parents=True, exist_ok=True)
                (keep / f"{name}.dom.json").write_text(json.dumps(dom, indent=1))
                (keep / f"{name}.ops.json").write_text(r.stdout)
            u = dom["u"]
            TOL = TOL_CSS / u
            print(f"{name}: {len(ops['ops'])} ops, {len(dom['els'])} mockup elements, tolerance {TOL:.3f} units")
            if ops["overflow"]:
                bad += 1
                print("  the DLL's ops overflowed <-- DIFFERS")
            cs = {"part": [], "hot": [], "dot": [], "text": [], "thumb": []}
            for o in ops["ops"]:
                o["r"] = [o["x"], o["y"], o["w"], o["h"]]
                if o["kind"] == "solid" and o["rgba"] == "808078b3":
                    cs["thumb"].append(o)
                elif o["kind"] in cs:
                    cs[o["kind"]].append(o)
            ds = {k: [e for e in dom["els"] if e["kind"] == k] for k in cs}
            for c in cs["dot"]:
                c["rgb"] = c["rgba"][:6]
            keys = {"part": lambda e: e["part"], "hot": lambda e: e["key"], "dot": lambda e: e["rgb"], "text": lambda e: e["text"]}
            for k in ("part", "hot", "dot", "text"):
                pairs, oc, od = pair(cs[k], ds[k], keys[k])
                extra = ""
                if k == "text":
                    # the start and the width of each line, and its baseline
                    for c, d in pairs:
                        c["cmp"] = [c["x"], c["w"]]
                        d["cmp"] = [d["r"][0], d["r"][2]]
                    worst = max((max(abs(a - b) for a, b in zip(c["cmp"], d["cmp"])) for c, d in pairs), default=0)

                    def chrome_rounds(c, d):
                        s = d["size"] * u
                        a, de = faces[c["style"]]
                        return (math.floor(a * s + 0.5), math.floor(de * s + 0.5)) != (round(d["asc"]), round(d["desc"]))
                    off = [(c, d) for c, d in pairs if abs(c["base"] - d["base"]) > TOL]
                    metric = [(c, d) for c, d in off if chrome_rounds(c, d) and abs(abs(c["base"] - d["base"]) * u - 1) <= TOL_CSS]
                    rule = [p for p in off if p not in metric]
                    bl = max((abs(c["base"] - d["base"]) for c, d in pairs if (c, d) not in metric), default=0)
                    sz = max((abs(c["size"] - d["size"]) for c, d in pairs), default=0)
                    extra = f", size within {sz:.4f}, baseline within {bl:.4f}"
                    if metric:
                        extra += f", Chrome's metric rounding moves {len(metric)} by a pixel"
                    # the ink: dark on a light face, the style's own colour otherwise
                    ink = [(c, d) for c, d in pairs if c["ink"] != (d["colour"] == "rgb(29, 29, 27)")]
                    if ink:
                        extra += f", ink disagrees {len(ink)}"
                    for c, d in sorted(rule, key=lambda p: -abs(p[0]["base"] - p[1]["base"]))[:3]:
                        print(f"        {c['text']!r} ({c['style']}): baseline DLL {c['base']:.3f} mockup {d['base']:.3f}")
                    bad += bool(rule) + (sz > TOL) + len(ink)
                    glyphs |= {c["text"] for c, d in pairs if any(ch in fallback for ch in c["text"])}
                else:
                    worst = max((max(abs(a - b) for a, b in zip(c["r"], d["r"])) for c, d in pairs), default=0)
                flag = " <-- DIFFERS" if worst > TOL or oc or od else ""
                bad += worst > TOL or bool(oc) or bool(od)
                print(f"  {k:5s} {len(pairs):4d} paired, worst {worst:.4f} units{extra}{flag}")
                for c in oc[:6]:
                    print(f"        only the DLL:    {keys[k](c)!r} at {[round(x, 2) for x in c['r']]}")
                for d in od[:6]:
                    print(f"        only the mockup: {keys[k](d)!r} at {[round(x, 2) for x in d['r']]}")
                for c, d in sorted(pairs, key=lambda p: -max(abs(a - b) for a, b in zip(p[0].get('cmp', p[0]['r']), p[1].get('cmp', p[1]['r']))))[:3]:
                    if max(abs(a - b) for a, b in zip(c.get("cmp", c["r"]), d.get("cmp", d["r"]))) > TOL:
                        print(f"        {keys[k](c)!r}: DLL {[round(x, 3) for x in c.get('cmp', c['r'])]} mockup {[round(x, 3) for x in d.get('cmp', d['r'])]}")
            # each panel's scroll as drawn and its limit
            sc = []
            for i, pn in enumerate(("tree", "demos", "text")):
                d = dom["scroll"][pn]
                sc.append(max(abs(ops["frame"]["scroll"][i] - d["top"]), abs(ops["frame"]["max"][i] - d["max"])))
            flag = " <-- DIFFERS" if max(sc) > TOL else ""
            bad += max(sc) > TOL
            print(f"  scroll  limits and offsets within {max(sc):.4f} units{flag}")
            # the thumbs, which Chrome paints on whole pixels
            worst, miss = 0, []
            for pn in ("tree", "demos", "text"):
                d = dom["thumbs"].get(pn)
                cands = [c for c in cs["thumb"] if abs(c["x"] - ((dom["scroll"][pn]["page"][0] - dom["frame"][0]) / u)) < TOL]
                if (d is None) != (not cands):
                    miss.append(pn)
                elif d:
                    worst = max(worst, abs(cands[0]["y"] - d[0]), abs(cands[0]["h"] - d[1]))
            flag = " <-- DIFFERS" if miss or worst * u > TOL_THUMB_PX else ""
            bad += bool(miss) or worst * u > TOL_THUMB_PX
            print(f"  thumb {len(dom['thumbs']):4d} painted, worst {worst * u:.2f} px{', only one side: ' + ', '.join(miss) if miss else ''}{flag}")
    finally:
        run("close")
        httpd.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    if glyphs:
        bad += 1
        print(f"the mockup sets {sorted(glyphs)} in the browser's fallback face, which the DLL does not carry: "
              f"the game draws no glyph for them <-- DIFFERS")
    print(f"wikilaycheck: {len(CASES)} cases in {time.time() - t0:.0f} s: "
          + ("the DLL and the mockup agree, every box, line and baseline" if not bad else f"{bad} disagreement(s)"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
