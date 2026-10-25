#!/usr/bin/env python3
"""laycheck.py -- the DLL's layout rules (tagpu_hudlay.c) against the mockup's (hud.js).

    python3 tools/hudart/laycheck.py [--keep DIR]

For each case below it opens the mockup in a headless browser (playwright-cli, as capture.sh
does), takes the mockup's own state and every element's box in HUD units, runs laycheck.c --
tagpu_hudlay.c compiled natively -- on the same state, and pairs the two sides element by element:
every part by name, every text by its string, every solid fill by its colour, every clickable by
its key, and every text's baseline -- the browser's, measured with a probe on the text's own line,
against tagpu_hudlay_baseline from the faces' metrics. It prints the worst difference per kind and
every element only one side has, and exits 1 when a box or a baseline differs by more than TOL
units or an element is unmatched.

A baseline that differs where Chrome rounds the face's ascent or descent to another whole pixel than
round(em x size) does, and that the same rule on Chrome's own metrics reproduces, is named, not
failed: the line rule is the one checked, and Chrome's rounding
of Oswald's 1.193 em ascent departs from the plain one at about one size in three hundred by an
arithmetic not identified (hud-layout.md, "Text").

The standard library, gcc and playwright-cli; it serves the repository root itself.
"""
import http.server
import json
import math
import os
import re
import shutil
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "tagpu" / "ddraw" / "src"
# Chrome lays out in 1/64 of a CSS pixel and snaps each nested box to it, so the mockup's boxes
# carry up to a few 1/64ths of rounding; past that, a difference is a rule disagreeing.
TOL_CSS = 0.07

# (name, the mockup's URL switches, the element hovered or None -- a column row by its place, "row2",
# or a button with a hint by its key, "chat" -- the button held or "")
CASES = [
    ("4k-150-teams", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&stat=0&paused", None, ""),
    ("4k-auto-scroll", "res=3840x2160&game=auto&hud=150&np=16&nt=4&t=100&stat=0&paused", None, ""),
    ("1080-100-one-team", "res=1920x1080&game=off&hud=100&np=6&nt=1&t=20&stat=0", None, ""),
    ("1440-75-hover-held", "res=2560x1440&game=200&hud=75&np=8&nt=2&t=90&stat=0&paused", "row2", "play"),
    ("4k-150-free-for-all", "res=3840x2160&game=150&hud=150&np=6&nt=6&t=60&stat=0&paused", None, ""),
    ("4k-150-2v1v1", "res=3840x2160&game=150&hud=150&np=4&nt=3&t=60&stat=0&paused", None, ""),
    ("4k-150-scrolled-toast", "res=3840x2160&game=150&hud=150&np=14&nt=2&t=70&stat=0&paused&scroll=3&toast=Camera%20was%20not%20recorded", None, ""),
    ("1440-75-sight-follow", "res=2560x1440&game=200&hud=75&np=8&nt=2&t=90&stat=0&paused&sight=0&follow=0", "row0", ""),
    ("4k-150-stats-units", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&stat=1&sm=units&ss=player&paused", None, ""),
    ("1440-75-stats-teams-metal", "res=2560x1440&game=200&hud=75&np=8&nt=2&t=90&stat=1&sm=metal&ss=team&paused", None, ""),
    ("1080-100-stats-ffa-army-held", "res=1920x1080&game=off&hud=100&np=6&nt=6&t=20&stat=1&sm=army&ss=team&paused", None, "close"),
    ("4k-auto-stats-16-excess-start", "res=3840x2160&game=auto&hud=150&np=16&nt=4&t=0&stat=1&sm=excess&ss=player&paused", None, ""),
    ("4k-150-stats-clamped-energy", "res=3840x2160&game=150&hud=100&np=4&nt=2&t=132&stat=1&sm=energy&ss=player&paused&sx=2000&sy=-40", None, ""),
    # the proposed features: the Forks bay off with no fork, the picker (at the fork limit with its
    # warning), and a watched fork with the lanes closed, whose axis takes in the forks and whose fill
    # stops at the fork's branch
    ("4k-150-prop-no-forks", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&stat=0&nf=0&paused", None, ""),
    ("1440-75-picker", "res=2560x1440&game=200&hud=75&np=8&nt=2&t=90&nf=2&pick&paused", None, ""),
    ("4k-150-picker-full", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&nf=3&pick&paused", None, ""),
    ("1080-100-watched-fork-closed", "res=1920x1080&game=off&hud=100&np=6&nt=1&t=40&nf=3&tl=1&paused", None, ""),
    ("4k-150-picker-held", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&nf=3&pick&paused", None, "takeover"),
    # the lanes open, three forks of which the third by branch point is a fork of the first: its line
    # rises to the first's lane; watched, the chain fills along the first lane to its branch; the
    # lane the picker's takeover replaces outlined; a lane held
    ("4k-150-lanes", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&nf=3&lo&paused", None, ""),
    ("1440-75-lanes-watched", "res=2560x1440&game=200&hud=75&np=8&nt=2&t=90&nf=3&lo&tl=1&paused", None, ""),
    ("1080-100-lanes-picker", "res=1920x1080&game=off&hud=100&np=6&nt=1&t=40&nf=3&lo&pick&paused", None, ""),
    ("4k-150-lanes-held", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&nf=3&lo&paused", None, "lane:0"),
    # the watched lane's Back to main recording, its lane widened by it; and the hints: the foot's first and last
    # buttons (Stats, Chat, the one nearest the screen's right edge), the transport plate's, nearest its left edge,
    # and a lane's buttons, on the lane at the track's right end
    ("1080-100-lanes-back", "res=1920x1080&game=off&hud=100&np=6&nt=1&t=40&nf=3&lo&tl=0&paused", None, ""),
    ("4k-150-lanes-back-right", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&nf=3&lo&tl=2&paused", None, ""),
    ("4k-150-hint-stats", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&stat=0&paused", "stats", ""),
    ("1440-75-hint-chat", "res=2560x1440&game=200&hud=75&np=8&nt=2&t=90&stat=0&paused", "chat", ""),
    ("1080-100-hint-chat", "res=1920x1080&game=off&hud=100&np=6&nt=1&t=20&stat=0", "chat", ""),
    ("4k-150-hint-export", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&nf=3&lo&paused", "export:2", ""),
    ("4k-150-hint-uiscale", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&stat=0&paused", "uiscale", ""),
    # over Take over and the lanes, which are drawn after the transport plate: the hint is drawn last
    ("1080-100-hint-bookmark-lanes", "res=1920x1080&game=off&hud=100&np=6&nt=1&t=40&nf=3&lo&tl=1&paused", "bookmark", ""),
    ("4k-150-hint-speed", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&stat=0&paused", "speed", ""),
    ("1080-100-hint-bookmark", "res=1920x1080&game=off&hud=100&np=6&nt=1&t=20&stat=0", "bookmark", ""),
    ("1440-75-hint-leave", "res=2560x1440&game=200&hud=75&np=8&nt=2&t=90&stat=0&paused", "leave", ""),
    ("1080-100-hint-delete-watched", "res=1920x1080&game=off&hud=100&np=6&nt=1&t=40&nf=3&lo&tl=2&paused", "delete:2", ""),
    # a lane's button, Back to main recording and a picker row held: their pressed parts, which tools/hudart/fit.py
    # measures by these cases
    ("4k-150-lane-button-held", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&nf=3&lo&paused", None, "export:0"),
    ("4k-150-lanes-back-held", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&nf=3&lo&tl=0&paused", None, "main"),
    ("4k-150-picker-row-held", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&nf=3&pick&paused", None, "takeas:0"),
    # at device scale factor 1, the game's 1.5 pixels a unit: boxes on half pixels, which each side snaps
    ("1080-100-lanes-dpr1", "res=1920x1080&game=150&hud=100&np=4&nt=4&t=70&nf=3&lo&paused", None, "", 1),
    # the live HUD: the rewind button alone, then its strip open
    ("4k-150-live", "res=3840x2160&game=150&hud=150&mode=live", None, ""),
    ("1080-100-live-rw", "res=1920x1080&game=off&hud=100&mode=live&rw", None, ""),
    ("1440-75-live-rw", "res=2560x1440&game=200&hud=75&mode=live&rw", None, ""),
    # the strip just opened: the playhead at its end, where fwd is drawn disabled
    ("1080-100-live-open", "res=1920x1080&game=off&hud=100&mode=live&rw&t=132", None, ""),
    # the toast clear of the open strip, and where the strip is closed
    ("1080-100-live-rw-toast", "res=1920x1080&game=off&hud=100&mode=live&rw&toast=Click%20again%20to%20play%20from%200%3A30", None, ""),
    ("4k-150-live-rw-toast", "res=3840x2160&game=150&hud=150&mode=live&rw&toast=Click%20again%20to%20play%20from%200%3A30%2C%20replacing%20F2", None, ""),
    ("1440-75-live-toast", "res=2560x1440&game=200&hud=75&mode=live&toast=Rewind%20refused", None, ""),
]
# a chart point is a float in the game and a double in the mockup, and both write it to a tenth: one
# within 1e-5 of a tenth's half can round either way, so a point may differ by that tenth
TOL_POINT = 0.1

# the mockup's state and boxes, in units from the stage's top left
COLLECT = r"""() => {
  const hoverRow = HOVER, held = HELD;
  build();
  const u = unit(), s = document.querySelector('#stage').getBoundingClientRect();
  // a box in units: the exact one hud.js pinned it from, else (an SVG's own elements) Chrome's
  const R = e => { if (e.dataset && e.dataset.ub) return e.dataset.ub.split(' ').map(Number);
    const r = e.getBoundingClientRect(); return [(r.left - s.left) / u, (r.top - s.top) / u, r.width / u, r.height / u]; };
  // the device pixels it is drawn on, and the ones Chrome painted its box on
  const dev = e => e.dataset.dev ? e.dataset.dev.split(' ').map(Number) : null;
  const d = window.devicePixelRatio || 1, painted = e => { const r = e.getBoundingClientRect();
    return [r.left, r.top, r.right, r.bottom].map((v, i) => Math.round((v - (i % 2 ? s.top : s.left)) * d)); };
  const st = state();
  // a held control by its key: the act, and the index a lane's or a row's key carries
  if (held) { const [act, ix] = held.split(':');
    const b = [...document.querySelectorAll(`.art.hot[data-act="${act}"]`)].find(e => ix == null || [e.dataset.fork, e.dataset.seat, e.dataset.team, e.dataset.val].includes(ix));
    HUD.press(b, true); st.held = held; }
  // the pointer: on the column's row by its place ("row2"), its hover strip shown, or on a control by its key ("chat",
  // "delete:2"), its hint drawn by the hint layer, as the game draws it for the pointer
  if (hoverRow != null && /^row\d+$/.test(hoverRow)) {
    const row = document.querySelectorAll('#hud-layer .row')[+hoverRow.slice(3)], r = R(row);
    row.querySelector('.hov').style.visibility = 'visible';
    st.mx = r[0] + r[2] / 2; st.my = r[1] + r[3] / 2;
  } else if (hoverRow != null) {
    const [act, ix] = hoverRow.split(':');
    const r = R([...document.querySelectorAll(`#hud-layer [data-act="${act}"]`)].find(e => ix == null || [e.dataset.fork, e.dataset.seat].includes(ix)));
    st.mx = r[0] + r[2] / 2; st.my = r[1] + r[3] / 2;
    HUD.hint(document.querySelector('#hud-layer'), st.mx, st.my, st);
  }
  const els = [], ctx = document.createElement('canvas').getContext('2d');
  // the opacity an element is painted with: its own times every ancestor's, as a disabled
  // control's icon and text are dimmed with it
  const opacity = (e) => {
    let a = 1;
    for (let q = e; q && q.id !== 'hud-layer'; q = q.parentElement) a *= +getComputedStyle(q).opacity;
    return Math.round(255 * a);
  };
  for (const e of document.querySelectorAll('#hud-layer *')) {
    if (!e.getClientRects().length || getComputedStyle(e).visibility === 'hidden') continue;
    const dim = !!(e.closest('.row') && e.closest('.row').style.filter);
    if (e.classList.contains('art')) {
      const part = e.dataset.gp || e.dataset.tile;
      els.push({ kind: 'part', part, r: R(e), dim, alpha: opacity(e), quads: e.dataset.quads ? JSON.parse(e.dataset.quads) : null,
                 dev: dev(e), painted: painted(e) });
    }
    if (e.dataset.act && !e.classList.contains('off')) {   // a disabled control takes no input
      const k = e.dataset.act + (e.dataset.val != null ? ':' + e.dataset.val : e.dataset.team != null ? ':' + e.dataset.team : e.dataset.seat != null ? ':' + e.dataset.seat
        : e.dataset.fork != null ? ':' + e.dataset.fork : '');
      els.push({ kind: 'hot', key: k, r: R(e) });
    }
    if (e.dataset.track != null) els.push({ kind: 'hot', key: 'track', r: R(e) });
    if (e.classList.contains('clip')) els.push({ kind: 'clip', r: R(e), dev: dev(e), painted: painted(e) });
    if (e.classList.contains('t')) {
      // the baseline: an empty inline-block on the text's own line sits on it (the text and the probe
      // wrapped in one span, which takes the anonymous flex item's place and lays out as it does)
      const keep = e.innerHTML;
      e.innerHTML = '<span>' + keep + '<i style="display:inline-block;width:0;height:0"></i></span>';
      const base = (e.querySelector('span > i:last-child').getBoundingClientRect().bottom - s.top) / u;
      e.innerHTML = keep;
      // the face's ascent and descent as Blink rounds them, in CSS pixels
      const cs = getComputedStyle(e);
      ctx.font = `${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`;
      const fm = ctx.measureText('M');
      els.push({ kind: 'text', text: e.textContent, r: R(e), dim, alpha: opacity(e), size: parseFloat(cs.fontSize) / u, base,
                 asc: fm.fontBoundingBoxAscent, desc: fm.fontBoundingBoxDescent });
    }
    // a ring: its outer box and its inner one as pinned, and the four boxes Chrome painted its sides on
    if (e.classList.contains('ring')) {
      const c = getComputedStyle(e.firstElementChild).backgroundColor.match(/\d+/g).slice(0, 3).map(Number);
      els.push({ kind: 'ring', rgb: c, r: R(e), dim, dev: dev(e), idev: e.dataset.idev.split(' ').map(Number), painted: painted(e),
                 sides: [...e.children].map(painted) });
      continue;
    }
    const bg = e.style.background || e.style.backgroundColor;
    if (bg && !e.classList.contains('art') && !e.parentElement.classList.contains('ring')) {
      const c = getComputedStyle(e).backgroundColor.match(/\d+/g).slice(0, 3).map(Number);
      els.push({ kind: e.style.borderRadius ? 'dot' : 'solid', rgb: c, r: R(e), dim, dev: dev(e), painted: painted(e) });
    }
  }
  // the stats window's drop shadow, and the chart: its SVG's own numbers, in units from its origin
  const win = document.querySelector('#statwin');
  if (win) {
    const m = getComputedStyle(win).boxShadow.match(/rgba?\(([^)]*)\)\s+(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px/);
    if (m) els.push({ kind: 'shadow', r: R(win), dy: +m[3] / u, blur: +m[4] / u, alpha: Math.round(255 * +(m[1].split(',')[3] || 1)) });
  }
  const hex = c => { const n = parseInt(c.slice(1), 16); return [n >> 16, (n >> 8) & 255, n & 255]; };
  const chart = { groups: [] };
  for (const svg of document.querySelectorAll('#hud-layer svg[data-chart]')) {
    const o = R(svg), A = (e, k) => +e.getAttribute(k);
    const [faint, past] = svg.querySelectorAll(':scope > g:not([data-ring])'), cw = A(svg.querySelector('clipPath rect'), 'width');
    els.push({ kind: 'chart', r: o, alpha: Math.round(255 * +faint.getAttribute('opacity')), clip: o });
    els.push({ kind: 'chart', r: o, alpha: 255, clip: [o[0], o[1], Math.max(0, Math.min(o[2], cw)), o[3]] });
    for (const l of svg.querySelectorAll(':scope > line'))
      els.push({ kind: 'line', rgb: hex(l.getAttribute('stroke')), alpha: Math.round(255 * +(l.getAttribute('stroke-opacity') || 1)), width: A(l, 'stroke-width'),
                 r: [o[0] + A(l, 'x1'), o[1] + A(l, 'y1'), A(l, 'x2') - A(l, 'x1'), A(l, 'y2') - A(l, 'y1')] });
    for (const t of svg.querySelectorAll(':scope > text'))
      els.push({ kind: 'svgtext', text: t.textContent, r: [o[0] + A(t, 'x'), o[1] + A(t, 'y'), 0, 0], size: A(t, 'font-size'),
                 anchor: { start: 0, middle: 1, end: 2 }[t.getAttribute('text-anchor') || 'start'], rgb: hex(t.getAttribute('fill')) });
    // a solid by its own numbers (pinned, its attributes are snapped)
    for (const r of svg.querySelectorAll(':scope > rect')) {
      const [x, y, w, h] = r.dataset.r.split(' ').map(Number), b = r.getBoundingClientRect();
      els.push({ kind: 'solid', rgb: hex(r.getAttribute('fill')), r: [o[0] + x, o[1] + y, w, h], dim: false, dev: r.dataset.dev.split(' ').map(Number),
                 painted: [b.left - s.left, b.top - s.top, b.right - s.left, b.bottom - s.top].map(v => Math.round(v * d)) });
    }
    for (const g of svg.querySelectorAll(':scope > g[data-ring]')) {
      const [x, y, w, h] = g.dataset.ring.split(' ').map(Number), dv = g.dataset.dev.split(' ').map(Number);
      els.push({ kind: 'ring', rgb: hex(g.getAttribute('fill')), r: [o[0] + x, o[1] + y, w, h], dim: false, dev: dv,
                 idev: g.dataset.idev.split(' ').map(Number), painted: dv, sides: [...g.children].map(painted) });
    }
    // the body, in paint order: a group's halo (a dark colour's), its line, its cross
    let halo = false;
    for (const e of faint.children) {
      if (e.tagName === 'polyline' && e.getAttribute('stroke-opacity')) halo = true;   // only the halo has one
      else if (e.tagName === 'polyline') {
        chart.groups.push({ rgb: hex(e.getAttribute('stroke')), dark: halo, cross: null,
                            pts: e.getAttribute('points').split(' ').filter(Boolean).map(q => q.split(',').map(Number)).map(([x, y]) => [o[0] + x, o[1] + y]) });
        halo = false;
      } else if (e.tagName === 'path') {
        const [x, y] = e.getAttribute('d').slice(1).split('l')[0].split(',').map(Number), c = HUD.layout.chart.line.cross;
        chart.groups[chart.groups.length - 1].cross = [o[0] + x + c, o[1] + y + c];
      }
    }
  }
  const hints = [...document.querySelectorAll('#hud-layer > .hint')].map(e => e.textContent);
  return JSON.stringify({ state: st, els, u, d, chart, hints });
}"""


def face_metrics():
    """Each text style's ascent and descent in em, descent positive, read from its face as
    tagpu_hudart.c face_open reads them: OS/2's typo metrics when the face sets USE_TYPO_METRICS,
    hhea's otherwise."""
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


def mockup(port, query, hover, held, tmp, dpr=2):
    cfg = tmp / "pw.json"
    cfg.write_text(json.dumps({"browser": {"contextOptions": {"deviceScaleFactor": dpr, "viewport": {"width": 2100, "height": 1400}}}}))
    pw = ["playwright-cli", f"-s=laycheck{os.getpid()}", f"--config={cfg}"]
    # the mockup's own default, prop=1 and two forks unless the case says otherwise: the game's look
    url = f"http://127.0.0.1:{port}/tools/hudart/mockup/index.html?{query}"
    run = lambda *a: subprocess.run(pw + list(a), capture_output=True, text=True, cwd=tmp)
    run("open", url)
    try:
        for _ in range(60):
            n = result(run("eval", "() => typeof HUD !== 'undefined' && HUD.layout ? document.querySelectorAll('#hud-layer .art').length : 0").stdout)
            if n:
                break
        code = COLLECT.replace("HOVER", json.dumps(hover)).replace("HELD", json.dumps(held))
        r = run("eval", code)
        v = result(r.stdout)
        if not v:
            sys.exit(f"laycheck: the mockup gave nothing for {query}\n{r.stdout[-2000:]}\n{r.stderr}")
        return json.loads(v)
    finally:
        run("close")


def pair(cs, ds, key):
    """Greedy nearest pairing of same-key items; returns (pairs, only C, only DOM)."""
    pairs, left = [], list(ds)
    onlyc = []
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


def main():
    keep = None
    if len(sys.argv) == 3 and sys.argv[1] == "--keep":
        keep = Path(sys.argv[2])
    tmp = Path(tempfile.mkdtemp(prefix="laycheck"))
    exe = tmp / "laycheck"
    cc = subprocess.run(["gcc", "-O1", "-std=c99", "-Wall", f"-I{SRC}", f"-I{SRC.parent / 'inc'}",
                         str(ROOT / "tools/hudart/laycheck.c"), str(SRC / "tagpu_json.c"),
                         str(SRC / "tagpu_hudlay.c"), str(SRC / "tagpu_hudtext.c"), "-lm", "-o", str(exe)],
                        capture_output=True, text=True)
    if cc.returncode:
        sys.exit("laycheck: the native build failed\n" + cc.stderr)
    httpd = serve()
    faces = face_metrics()
    bad = 0
    try:
        for name, query, hover, held, *dpr in CASES:
            dom = mockup(httpd.server_address[1], query, hover, held, tmp, *dpr)
            sp = tmp / f"{name}.state.json"
            sp.write_text(json.dumps({**dom["state"], "faces": faces}))
            # the baseline is computed at the mockup's CSS pixels, the grid Blink rounds a face's metrics to
            r = subprocess.run([str(exe), str(ROOT / "art/hud"), str(sp), repr(dom["u"]), repr(dom["u"] * dom["d"])], capture_output=True, text=True)
            if r.returncode:
                sys.exit(f"laycheck: {name}: {r.stderr}")
            ops = json.loads(r.stdout)
            if keep:
                keep.mkdir(parents=True, exist_ok=True)
                (keep / f"{name}.dom.json").write_text(json.dumps(dom, indent=1))
                (keep / f"{name}.ops.json").write_text(r.stdout)
            cs = {"part": [], "text": [], "solid": [], "ring": [], "dot": [], "hot": [], "clip": [], "svgtext": [], "line": [], "chart": [], "shadow": []}
            for o in ops["ops"]:
                o["r"] = [o["x"], o["y"], o["w"], o["h"]]
                cs[o["kind"]].append(o)
                if "clip" in o and o["kind"] != "chart":     # a chart's clip is compared with its op
                    cs["clip"].append({"r": o["clip"], "snap": o.get("csnap")})
            ds = {k: [e for e in dom["els"] if e["kind"] == k] for k in cs}
            keys = {"part": lambda e: e["part"], "text": lambda e: e["text"], "hot": lambda e: e["key"],
                    "solid": lambda e: tuple(e["rgb"]) if "rgb" in e else tuple(int(e["rgba"][i:i + 2], 16) for i in (0, 2, 4)),
                    "ring": lambda e: tuple(e["rgb"]) if "rgb" in e else tuple(int(e["rgba"][i:i + 2], 16) for i in (0, 2, 4)),
                    "dot": lambda e: tuple(e["rgb"]) if "rgb" in e else tuple(int(e["rgba"][i:i + 2], 16) for i in (0, 2, 4)),
                    "clip": lambda e: 0, "shadow": lambda e: 0,
                    "svgtext": lambda e: e["text"],
                    "line": lambda e: tuple(e["rgb"]) if "rgb" in e else tuple(int(e["rgba"][i:i + 2], 16) for i in (0, 2, 4)),
                    "chart": lambda e: e["alpha"] if "alpha" in e else int(e["rgba"][6:8], 16)}
            TOL = TOL_CSS / dom["u"]
            print(f"{name}: {len(ops['ops'])} ops, {len(dom['els'])} mockup elements, tolerance {TOL:.3f} units")
            # the pointer on a control by its key: the mockup drew one hint, and the DLL one text op of that hint
            if hover is not None and not re.fullmatch(r"row\d+", hover):
                hs = dom.get("hints", [])
                cn = sum(1 for o in ops["ops"] if o["kind"] == "text" and hs and o["text"] == hs[0])
                if len(hs) != 1 or cn != 1:
                    bad += 1
                    print(f"  hint  the mockup drew {hs}, the DLL {cn} text op(s) of it <-- DIFFERS")
                else:
                    print(f"  hint  {hs[0]!r} drawn by both")
            for k in cs:
                pairs, oc, od = pair(cs[k], ds[k], keys[k])
                worst = max((max(abs(a - b) for a, b in zip(c["r"], d["r"])) for c, d in pairs), default=0)
                extra = ""
                if k == "text":
                    sz = max((abs(c["size"] - d["size"]) for c, d in pairs), default=0)
                    def chrome_rounds(c, d):
                        """Chrome's whole-pixel ascent or descent is not round(em x size)."""
                        s = d["size"] * dom["u"]
                        a, de = faces[c["style"]]
                        return (math.floor(a * s + 0.5), math.floor(de * s + 0.5)) != (d["asc"], d["desc"])
                    def chrome_line(d):
                        """The same rule (tagpu_hudlay_baseline) on Chrome's own rounded metrics."""
                        u = dom["u"]
                        s, y, h, A, D = d["size"] * u, d["r"][1] * u, d["r"][3] * u, d["asc"], d["desc"]
                        return (y + (h - s) / 2 + A + math.floor((s - A - D) / 2)) / u
                    off = [(c, d) for c, d in pairs if abs(c["base"] - d["base"]) > TOL]
                    # excused only where Chrome's metrics differ AND the rule on them gives the
                    # browser's baseline: a line rule that is wrong still fails
                    metric = [(c, d) for c, d in off if chrome_rounds(c, d) and abs(chrome_line(d) - d["base"]) <= TOL]
                    rule = [p for p in off if p not in metric]
                    bl = max((abs(c["base"] - d["base"]) for c, d in pairs if (c, d) not in metric), default=0)
                    extra = f", size within {sz:.4f}, baseline within {bl:.4f}"
                    if metric:
                        sizes = sorted({(c["style"], round(d["size"] * dom["u"], 4)) for c, d in metric})
                        extra += f", Chrome's metric rounding moves {len(metric)} by a pixel at {sizes}"
                    if sz > TOL or rule:
                        bad += 1
                        for c, d in sorted(rule, key=lambda p: -abs(p[0]["base"] - p[1]["base"]))[:3]:
                            print(f"        {c['text']!r} ({c['style']}): baseline DLL {c['base']:.3f} mockup {d['base']:.3f}")
                if k == "svgtext":
                    # the anchor, the size and the colour as well as the point
                    att = sum(1 for c, d in pairs if c["centre"] != d["anchor"] or abs(c["size"] - d["size"]) > TOL
                              or c["rgb"] != "%02x%02x%02x" % tuple(d["rgb"]))
                    extra += f", anchor/size/colour disagree {att}" if att else ""
                    bad += att
                if k == "line":
                    att = sum(1 for c, d in pairs if abs(c["width"] - d["width"]) > TOL or abs(int(c["rgba"][6:8], 16) - d["alpha"]) > 1)
                    extra += f", width/opacity disagree {att}" if att else ""
                    bad += att
                if k == "chart":
                    cw = max((max(abs(a - b) for a, b in zip(c["clip"], d["clip"])) for c, d in pairs), default=0)
                    extra += f", clip within {cw:.4f}"
                    bad += cw > TOL
                if k == "shadow":
                    att = sum(1 for c, d in pairs if abs(c["dy"] - d["dy"]) > TOL or abs(c["blur"] - d["blur"]) > TOL
                              or abs(int(c["rgba"][6:8], 16) - d["alpha"]) > 1)
                    extra += f", offset/blur/opacity disagree {att}" if att else ""
                    bad += att
                if k in ("part", "text", "solid", "ring"):
                    dm = sum(1 for c, d in pairs if bool(c.get("dim")) != bool(d.get("dim")))
                    extra += f", dim disagrees {dm}" if dm else ""
                    bad += dm
                if k == "part":
                    # a composed part's pieces: the same, edge for edge in device pixels and in the source
                    def same(c, d):
                        a, b = c.get("quads"), d.get("quads")
                        if a is None or b is None:
                            return a is None and b is None
                        return len(a) == len(b) and all(abs(x - y) <= (1e-3 if i < 4 else 1e-4)
                                                         for qa, qb in zip(a, b) for i, (x, y) in enumerate(zip(qa, qb)))
                    pc = [(c, d) for c, d in pairs if c.get("snap") == d.get("dev") and not same(c, d)]
                    extra += f", pieces disagree {len(pc)}" if pc else ""
                    for c, d in pc[:3]:
                        print(f"        pieces of {c['part']!r}: DLL {len(c.get('quads') or [])} {(c.get('quads') or [])[:2]} "
                              f"mockup {len(d.get('quads') or [])} {(d.get('quads') or [])[:2]}")
                    bad += len(pc)
                if k == "ring":
                    # the inner box's edges, which the four sides share, and the sides Chrome painted on them
                    def sides(d):
                        (x0, y0, x3, y3), (x1, y1, x2, y2) = d["dev"], d["idev"]
                        return [[x0, y0, x3, y1], [x0, y2, x3, y3], [x0, y1, x1, y2], [x2, y1, x3, y2]]
                    ia = [(c, d) for c, d in pairs if c.get("isnap") != d["idev"]]
                    ip = [(c, d) for c, d in pairs if d["sides"] != sides(d)]
                    extra += f", {len(ia)} inner edges a pixel apart" if ia else ""
                    extra += f", {len(ip)} with a side painted off its edges" if ip else ""
                    for c, d in (ia + ip)[:3]:
                        print(f"        {keys[k](c)!r}: DLL inner {c.get('isnap')} pinned {d['idev']} sides painted {d['sides']}")
                    bad += len(ia) + len(ip)
                if k in ("part", "solid", "ring", "dot", "clip"):
                    # the device pixels: the DLL's snapped box, the one hud.js pinned, and the one Chrome painted
                    sa = [(c, d) for c, d in pairs if d.get("dev") is not None and c.get("snap") != d["dev"]]
                    sp = [(c, d) for c, d in pairs if d.get("dev") is not None and d["painted"] != d["dev"]]
                    nd = sum(1 for c, d in pairs if d.get("dev") is None and c.get("snap") is not None)
                    extra += f", {len(sa)} snapped a pixel apart" if sa else ""
                    extra += f", {len(sp)} painted off their pixels" if sp else ""
                    extra += f", {nd} not pinned" if nd else ""
                    for c, d in (sa + sp)[:3]:
                        print(f"        {keys[k](c)!r}: DLL {c.get('snap')} pinned {d['dev']} painted {d['painted']} (units {[round(v, 4) for v in c['r']]})")
                    bad += len(sa) + len(sp) + nd
                if k in ("part", "text"):
                    al = sum(1 for c, d in pairs if abs(c.get("alpha", 255) - d.get("alpha", 255)) > 1)
                    extra += f", opacity disagrees {al}" if al else ""
                    bad += al
                flag = " <-- DIFFERS" if worst > TOL or oc or od else ""
                bad += worst > TOL or bool(oc) or bool(od)
                print(f"  {k:5s} {len(pairs):4d} paired, worst {worst:.4f} units{extra}{flag}")
                for c in oc[:6]:
                    print(f"        only the DLL:    {keys[k](c)!r} at {[round(v, 2) for v in c['r']]}")
                for d in od[:6]:
                    print(f"        only the mockup: {keys[k](d)!r} at {[round(v, 2) for v in d['r']]}")
                for c, d in sorted(pairs, key=lambda p: -max(abs(a - b) for a, b in zip(p[0]['r'], p[1]['r'])))[:3]:
                    if max(abs(a - b) for a, b in zip(c["r"], d["r"])) > TOL:
                        print(f"        {keys[k](c)!r}: DLL {[round(v, 3) for v in c['r']]} mockup {[round(v, 3) for v in d['r']]}")
            # the chart's lines, group by group in paint order: colour, halo, every point, the cross
            cg, dg = ops["chart"]["groups"], dom["chart"]["groups"]
            if cg or dg:
                worst, att = 0, 0
                if len(cg) != len(dg):
                    att += 1
                for c, d in zip(cg, dg):
                    att += c["rgb"] != "%02x%02x%02x" % tuple(d["rgb"]) or bool(c["dark"]) != d["dark"] or len(c["pts"]) != len(d["pts"])
                    att += (c["cross"] is None) != (d["cross"] is None)
                    if c["cross"] and d["cross"]:
                        worst = max(worst, *(abs(a - b) for a, b in zip(c["cross"], d["cross"])))
                    for a, b in zip(c["pts"], d["pts"]):
                        worst = max(worst, abs(a[0] - b[0]), abs(a[1] - b[1]))
                flag = " <-- DIFFERS" if att or worst > TOL_POINT + TOL else ""
                bad += bool(att) or worst > TOL_POINT + TOL
                print(f"  lines {len(cg):4d} groups, {sum(len(c['pts']) for c in cg)} points, worst {worst:.4f} units"
                      f"{f', {att} disagree in colour, halo, length or cross' if att else ''}{flag}")
    finally:
        httpd.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print("laycheck: " + ("the two agree" if not bad else f"{bad} disagreement(s)"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
