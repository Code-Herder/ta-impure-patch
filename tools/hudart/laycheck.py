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

# (name, the mockup's URL switches, the pointer in units or None, the button held or "")
CASES = [
    ("4k-150-teams", "res=3840x2160&game=150&hud=150&np=10&nt=2&t=70&stat=0&paused", None, ""),
    ("4k-auto-scroll", "res=3840x2160&game=auto&hud=150&np=16&nt=4&t=100&stat=0&paused", None, ""),
    ("1080-100-one-team", "res=1920x1080&game=off&hud=100&np=6&nt=1&t=20&stat=0", None, ""),
    ("1440-75-hover-held", "res=2560x1440&game=200&hud=75&np=8&nt=2&t=90&stat=0&paused", "row2", "play"),
    ("4k-150-free-for-all", "res=3840x2160&game=150&hud=150&np=6&nt=6&t=60&stat=0&paused", None, ""),
    ("4k-150-2v1v1", "res=3840x2160&game=150&hud=150&np=4&nt=3&t=60&stat=0&paused", None, ""),
]

# the mockup's state and boxes, in units from the stage's top left
COLLECT = r"""() => {
  const hoverRow = HOVER, held = HELD;
  build();
  const u = unit(), s = document.querySelector('#stage').getBoundingClientRect();
  const R = e => { const r = e.getBoundingClientRect(); return [(r.left - s.left) / u, (r.top - s.top) / u, r.width / u, r.height / u]; };
  const st = state();
  if (held) { const b = document.querySelector(`.art.hot[data-act="${held}"]`); HUD.press(b, true); st.held = held; }
  if (hoverRow != null) {
    const row = document.querySelectorAll('#hud-layer .row')[hoverRow], r = R(row);
    row.querySelector('.hov').style.display = 'block';
    st.mx = r[0] + r[2] / 2; st.my = r[1] + r[3] / 2;
  }
  const els = [], ctx = document.createElement('canvas').getContext('2d');
  for (const e of document.querySelectorAll('#hud-layer *')) {
    if (!e.getClientRects().length) continue;
    const dim = !!(e.closest('.row') && e.closest('.row').style.filter);
    if (e.classList.contains('art')) {
      let part = e.dataset.gp || e.dataset.tile;
      if (!part) { const m = /parts\/([^/]+)\.png/.exec(e.style.getPropertyValue('--bi')); part = m && m[1]; }
      els.push({ kind: 'part', part, r: R(e), dim, alpha: Math.round(255 * +(getComputedStyle(e).opacity)) });
    }
    if (e.dataset.act) {
      const k = e.dataset.act + (e.dataset.team != null ? ':' + e.dataset.team : e.dataset.seat != null ? ':' + e.dataset.seat : '');
      els.push({ kind: 'hot', key: k, r: R(e) });
    }
    if (e.dataset.track != null) els.push({ kind: 'hot', key: 'track', r: R(e) });
    if (e.classList.contains('clip')) els.push({ kind: 'clip', r: R(e) });
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
      els.push({ kind: 'text', text: e.textContent, r: R(e), dim, size: parseFloat(cs.fontSize) / u, base,
                 asc: fm.fontBoundingBoxAscent, desc: fm.fontBoundingBoxDescent });
    }
    const bg = e.style.background || e.style.backgroundColor;
    if (bg && !e.classList.contains('art')) {
      const c = getComputedStyle(e).backgroundColor.match(/\d+/g).slice(0, 3).map(Number);
      els.push({ kind: e.style.borderRadius ? 'dot' : 'solid', rgb: c, r: R(e), dim });
    }
  }
  return JSON.stringify({ state: st, els, u });
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


def mockup(port, query, hover, held, tmp):
    cfg = tmp / "pw.json"
    cfg.write_text(json.dumps({"browser": {"contextOptions": {"deviceScaleFactor": 2, "viewport": {"width": 2100, "height": 1400}}}}))
    pw = ["playwright-cli", f"-s=laycheck{os.getpid()}", f"--config={cfg}"]
    url = f"http://127.0.0.1:{port}/tools/hudart/mockup/index.html?{query}"
    run = lambda *a: subprocess.run(pw + list(a), capture_output=True, text=True, cwd=tmp)
    run("open", url)
    try:
        for _ in range(60):
            n = result(run("eval", "() => typeof HUD !== 'undefined' && HUD.layout ? document.querySelectorAll('#hud-layer .art').length : 0").stdout)
            if n:
                break
        code = COLLECT.replace("HOVER", "null" if hover is None else str(int(hover[3:]))).replace("HELD", json.dumps(held))
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
    cc = subprocess.run(["gcc", "-O1", "-std=c99", "-Wall", f"-I{SRC}", str(ROOT / "tools/hudart/laycheck.c"),
                         str(SRC / "tagpu_json.c"), str(SRC / "tagpu_hudlay.c"), "-lm", "-o", str(exe)],
                        capture_output=True, text=True)
    if cc.returncode:
        sys.exit("laycheck: the native build failed\n" + cc.stderr)
    httpd = serve()
    faces = face_metrics()
    bad = 0
    try:
        for name, query, hover, held in CASES:
            dom = mockup(httpd.server_address[1], query, hover, held, tmp)
            sp = tmp / f"{name}.state.json"
            sp.write_text(json.dumps({**dom["state"], "faces": faces}))
            # the baseline is computed at the mockup's CSS pixels, the grid Blink rounds a face's metrics to
            r = subprocess.run([str(exe), str(ROOT / "art/hud"), str(sp), repr(dom["u"])], capture_output=True, text=True)
            if r.returncode:
                sys.exit(f"laycheck: {name}: {r.stderr}")
            ops = json.loads(r.stdout)
            if keep:
                keep.mkdir(parents=True, exist_ok=True)
                (keep / f"{name}.dom.json").write_text(json.dumps(dom, indent=1))
                (keep / f"{name}.ops.json").write_text(r.stdout)
            cs = {"part": [], "text": [], "solid": [], "dot": [], "hot": [], "clip": []}
            for o in ops["ops"]:
                o["r"] = [o["x"], o["y"], o["w"], o["h"]]
                if o["kind"] == "solid" and o["rgba"] == "000000cc":
                    continue                      # the bars' inset edge: a box-shadow in the mockup
                cs[o["kind"]].append(o)
                if "clip" in o:
                    cs["clip"].append({"r": o["clip"]})
            ds = {k: [e for e in dom["els"] if e["kind"] == k] for k in cs}
            keys = {"part": lambda e: e["part"], "text": lambda e: e["text"], "hot": lambda e: e["key"],
                    "solid": lambda e: tuple(e["rgb"]) if "rgb" in e else tuple(int(e["rgba"][i:i + 2], 16) for i in (0, 2, 4)),
                    "dot": lambda e: tuple(e["rgb"]) if "rgb" in e else tuple(int(e["rgba"][i:i + 2], 16) for i in (0, 2, 4)),
                    "clip": lambda e: 0}
            TOL = TOL_CSS / dom["u"]
            print(f"{name}: {len(ops['ops'])} ops, {len(dom['els'])} mockup elements, tolerance {TOL:.3f} units")
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
                if k in ("part", "text", "solid"):
                    dm = sum(1 for c, d in pairs if bool(c.get("dim")) != bool(d.get("dim")))
                    extra += f", dim disagrees {dm}" if dm else ""
                    bad += dm
                if k == "part":
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
    finally:
        httpd.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print("laycheck: " + ("the two agree" if not bad else f"{bad} disagreement(s)"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
