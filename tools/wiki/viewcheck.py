#!/usr/bin/env python3
"""viewcheck.py -- the tech tree's unit view in the game against the mockup's, at the same camera.

usage: tools/wiki/viewcheck.py SHOTS
  exit 0 when every view agrees, 1 when one differs, 2 when a side cannot be run

SHOTS is the folder `wiki_regression.py techtree --shots SHOTS` wrote (tt9): each view's crop of the game's
frame and, in views.json, its unit, its camera (yaw, pitch, the distance factor) and its rect. For each, the
mockup's view of the same unit is drawn headless at the same camera (unitview.js `ucam`, Turn off, its script
run on tools/wiki/serve.py's COB host) and read back off its canvas, and the two pictures are compared:

  the silhouette  the game's pixels that are not the view's ground (tagpu_wikilay.c VIEW_RGB) against the
                  mockup's that are not transparent, both less the note's strip along the bottom, as their
                  intersection over their union -- a camera, an axis or a fit that disagrees moves it;
  the colour      the mean difference, per channel, over the pixels both cover whole -- a back face drawn for
                  a front one, a quad mapped across its diagonal or a light that disagrees moves it.

THE BOUNDS are what two rasterisers drawing the same triangles leave between them: the game's target is
twice the view each way, filtered down, and the mockup's canvas is Chrome's own antialiasing on SwiftShader,
so their edges differ by a pixel and the textures by a texel at the edges. MEASURED 2026-11-17 at 1920 x 1080,
stock: ARMCV 0.961 and 5.9, ARMSOLAR 0.991 and 10.3; ARMSOLAR fitted on the faces the bake collapses, which
the pack does not hold, measured 0.248 and 30.0.

Needs playwright-cli (a headless Chromium with SwiftShader's WebGL) and Pillow; run it with the venv's Python,
which serve.py's COB host needs.
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

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
GROUND = (0x12, 0x14, 0x0F)       # tagpu_wikilay.c VIEW_RGB: the view's ground under the picture
GROUND_TOL = 2                    # the ground is one flat colour: a pixel off it by more is the model's
NOTE_STRIP = 0.12                 # the bottom of the view the note's line may cover, left out of both masks
IOU_MIN = 0.90
COLOUR_MAX = 20.0


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
    """(iou, colour, n): the silhouettes' intersection over union and the mean channel difference where both
    cover a pixel whole, over the view less its note's strip."""
    w, h = game.size
    if mock.size != game.size:
        mock = mock.resize(game.size, Image.NEAREST)
    g, m = game.convert("RGB").load(), mock.convert("RGBA").load()
    inter = union = n = 0
    diff = 0.0
    for y in range(int(h * (1 - NOTE_STRIP))):
        for x in range(w):
            gp, mp = g[x, y], m[x, y]
            ing = max(abs(gp[i] - GROUND[i]) for i in range(3)) > GROUND_TOL
            inm = mp[3] > 128
            inter += ing and inm
            union += ing or inm
            if ing and mp[3] == 255:
                diff += sum(abs(gp[i] - mp[i]) for i in range(3)) / 3
                n += 1
    return (inter / union if union else 0.0), (diff / n if n else 999.0), n


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    shots = Path(sys.argv[1])
    try:
        rows = json.loads((shots / "views.json").read_text())
    except OSError as e:
        print(f"viewcheck: no views.json in {shots}: {e}", file=sys.stderr)
        return 2
    port = free_port()
    srv = subprocess.Popen([sys.executable, str(ROOT / "tools/wiki/serve.py"), "--port", str(port)], cwd=ROOT,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tmp = Path(tempfile.mkdtemp(prefix="viewcheck"))
    cfg = tmp / "pw.json"
    cfg.write_text(json.dumps({"browser": {"browserName": "chromium", "launchOptions": {"headless": True, "args": [
        "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"]},
        "contextOptions": {"deviceScaleFactor": 1, "viewport": {"width": 1200, "height": 900}}}}))
    pw = ["playwright-cli", f"-s=viewcheck{os.getpid()}", f"--config={cfg}"]
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
            yaw, pitch, zoom = r["cam"]
            q = f"res={gw}x{gh}&open&skin=kit&entry=techtree&unit={r['unit'].lower()}&ucam={yaw:.6f},{pitch:.6f},{zoom:.6f}"
            if r.get("mod"):
                q += f"&mod={r['mod']}"
            run("resize", str(gw + 34), str(gh + 900))
            run("goto", f"http://127.0.0.1:{port}/tools/wiki/mockup/index.html?{q}")
            # the script's Create settled as the game's was (the lane grabs after three seconds), the canvas read back
            # off the kept drawing buffer
            png = None
            t0 = time.monotonic()
            while time.monotonic() - t0 < 120:
                v = result(run("eval", "async () => { const t0 = performance.now(); let v;"
                               " while (performance.now() - t0 < 5000) { v = document.querySelector('#unit .view');"
                               " if (v && +(v.dataset.ticks || 0) >= 90) break; await new Promise(r => setTimeout(r, 100)); }"
                               " const c = v && v.querySelector('canvas');"
                               " return c && +(v.dataset.ticks || 0) >= 90 ? c.toDataURL('image/png') : null; }").stdout)
                if v:
                    png = v
                    break
            if not png:
                print(f"viewcheck: {r['unit']}: the mockup's view never drew 90 ticks")
                return 2
            mock = Image.open(io.BytesIO(base64.b64decode(png.split(",", 1)[1])))
            game = Image.open(shots / r["file"])
            iou, colour, n = compare(game, mock)
            ok = iou >= IOU_MIN and colour <= COLOUR_MAX
            bad += not ok
            side = Image.new("RGB", (game.width * 2, game.height), GROUND)
            side.paste(game.convert("RGB"), (0, 0))
            side.paste(mock.resize(game.size, Image.NEAREST), (game.width, 0), mock.resize(game.size, Image.NEAREST))
            side.save(shots / f"viewcheck-{r['unit']}.png")
            print(f"viewcheck: {r['unit']} at yaw {yaw:.3f}, pitch {pitch:.3f}, zoom {zoom:.3f}: the silhouettes "
                  f"{iou:.3f} of their union (at least {IOU_MIN}), the colour {colour:.1f} apart over {n} pixels "
                  f"(at most {COLOUR_MAX}){'' if ok else ' <-- DIFFERS'}; side by side in viewcheck-{r['unit']}.png")
    finally:
        run("close")
        srv.terminate()
    print(f"viewcheck: {len(rows)} view(s): " + ("the game and the mockup agree" if not bad else f"{bad} differ"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
