#!/usr/bin/env python3
"""treecheck.py -- the tech tree's stage in the game against the mockup's, pixel for pixel.

usage: tools/wiki/treecheck.py SHOTS
  exit 0 when every shot agrees, 1 when one differs, 2 when a shot cannot be checked

SHOTS is the folder `wiki_regression.py techtree --shots SHOTS` wrote (tt1, tt6): the fitted tree and the fitted tree
with every link, each a frame of the game, and in tree.json its canvas (the stage's rect in device pixels) and the
camera the frame drew. For each, the mockup is opened headless at the same resolution on the same entry -- fitted, with
All links when the shot has them -- and its stage's canvas read back once every picture has loaded; the two canvases are
compared:

  the camera      the mockup's fitted camera against the game's (techcheck pairs the fit; a difference here is the
                  check opening the wrong case, and nothing below would mean anything);
  the pixels      the mean difference per channel over the canvas, and the share of pixels more than LOUD apart.

What may differ is what two rasterisers drawing the same shapes leave between them: Chrome's antialiasing of a
roundRect's corners, a bezierCurveTo's stroke and its dashes against the HUD shader's (tagpu_huddraw.c RRECT, CURVE),
and the text, which Chrome sets in the HUD's faces at its own hinting while the game draws its glyph atlas. A shape the
game drew in the wrong place, colour or size moves both figures well past the bounds, and so does a picture shrunk
by another filter: the cards' small pictures are most of what the two differ by. MEASURED 2026-11-17 at
1920 x 1080, stock: the fitted tree 0.76 apart on average and 0.0040 of its canvas loud, with every link 0.80 and
0.0040 (bounds 2.0 and 0.01).

Needs playwright-cli (a headless Chromium) and Pillow; run it with the venv's Python, which serve.py needs.
"""
import base64
import io
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
LOUD = 48                         # a pixel this far apart in any channel is a different shape, not an edge's shade
MEAN_MAX = 2.0                    # the mean channel difference over the canvas
LOUD_MAX = 0.01                   # the share of the canvas's pixels LOUD apart
CAM_TOL = 1e-3


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def result(out):
    """The value of a playwright-cli eval: the lines between `### Result` and the next `###`."""
    lines, on = [], False
    for ln in out.splitlines():
        if ln.startswith("### "):
            if on:
                break
            on = ln.strip() == "### Result"
            continue
        if on:
            lines.append(ln)
    return json.loads("\n".join(lines)) if lines else None


def compare(game, mock):
    """(mean, loud, diff): the mean channel difference, the share of pixels LOUD apart, and the difference image."""
    a, b = game.convert("RGB"), mock.convert("RGB")
    d = ImageChops.difference(a, b)
    hist = d.histogram()
    n = a.width * a.height
    mean = sum(i * c for k in range(3) for i, c in enumerate(hist[k * 256:(k + 1) * 256])) / (3 * n)
    r, g, b = d.split()
    loud = ImageChops.lighter(ImageChops.lighter(r, g), b).point(lambda p: 255 if p > LOUD else 0).histogram()[255] / n
    return mean, loud, d


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    shots = Path(sys.argv[1])
    try:
        rows = json.loads((shots / "tree.json").read_text())
    except OSError as e:
        print(f"treecheck: no tree.json in {shots}: {e}", file=sys.stderr)
        return 2
    port = free_port()
    srv = subprocess.Popen([sys.executable, str(ROOT / "tools/wiki/serve.py"), "--port", str(port)], cwd=ROOT,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tmp = Path(tempfile.mkdtemp(prefix="treecheck"))
    cfg = tmp / "pw.json"
    cfg.write_text(json.dumps({"browser": {"browserName": "chromium", "launchOptions": {"headless": True},
        "contextOptions": {"deviceScaleFactor": 1, "viewport": {"width": 1200, "height": 900}}}}))
    pw = ["playwright-cli", f"-s=treecheck{os.getpid()}", f"--config={cfg}"]
    run = lambda *a: subprocess.run(pw + list(a), capture_output=True, text=True, cwd=tmp)
    bad = 0
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{port}/tools/wiki/mockup/index.html", timeout=1)
                break
            except OSError:
                time.sleep(0.1)
        run("open", "about:blank")
        for r in rows:
            gw, gh = map(int, r["res"].split("x"))
            q = f"res={gw}x{gh}&open&skin=kit&entry=techtree" + ("&links" if r["links"] else "")
            run("resize", str(gw + 34), str(gh + 900))
            run("goto", f"http://127.0.0.1:{port}/tools/wiki/mockup/index.html?{q}")
            # every picture loaded and drawn, then half a second later the canvas read back
            v, t0 = None, time.monotonic()
            while time.monotonic() - t0 < 120:
                # the stage's data-tech (wiki.js drawTech): the camera drawn and the pictures still loading
                v = result(run("eval", "async () => { const t0 = performance.now(), c = document.querySelector('#stage');"
                               " const st = () => (c.dataset.tech || '').split(',').map(Number);"
                               " const ready = () => st().length === 4 && st()[3] === 0 && (!document.fonts || document.fonts.status === 'loaded');"
                               " while (performance.now() - t0 < 5000 && !ready()) await new Promise(r => setTimeout(r, 100));"
                               " await new Promise(r => setTimeout(r, 500));"
                               " if (!ready()) return null;"
                               " return { cam: st().slice(0, 3), png: c.toDataURL('image/png') }; }").stdout)
                if v:
                    break
            if not v:
                print(f"treecheck: {r['file']}: the mockup's tree never loaded its pictures")
                return 2
            if any(abs(a - b) > CAM_TOL * max(1.0, abs(b)) for a, b in zip(v["cam"], r["cam"])):
                print(f"treecheck: {r['file']}: the mockup's camera {v['cam']} is not the game's {r['cam']}")
                return 2
            mock = Image.open(io.BytesIO(base64.b64decode(v["png"].split(",", 1)[1])))
            x, y, w, h = (round(k) for k in r["cv"])
            game = Image.open(shots / r["file"]).convert("RGB").crop((x, y, x + w, y + h))
            if mock.size != game.size:
                print(f"treecheck: {r['file']}: the mockup's canvas {mock.size} is not the game's {game.size}")
                return 2
            mean, loud, diff = compare(game, mock)
            ok = mean <= MEAN_MAX and loud <= LOUD_MAX
            bad += not ok
            stem = Path(r["file"]).stem
            side = Image.new("RGB", (w, h * 3))
            side.paste(game, (0, 0))
            side.paste(mock.convert("RGB"), (0, h))
            side.paste(diff.point(lambda p: min(255, p * 4)), (0, 2 * h))
            side.save(shots / f"treecheck-{stem}.png")
            print(f"treecheck: {stem}: {mean:.2f} apart on average (at most {MEAN_MAX}), {loud:.4f} of the canvas more than "
                  f"{LOUD} apart (at most {LOUD_MAX}){'' if ok else ' <-- DIFFERS'}; game, mockup and difference in "
                  f"treecheck-{stem}.png")
    finally:
        run("close")
        srv.terminate()
    print(f"treecheck: {len(rows)} shot(s): " + ("the game and the mockup agree" if not bad else f"{bad} differ"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
