#!/usr/bin/env python3
"""The in-game wiki's 3D stage in the game against the mockup's, pixel for pixel
(research/notes/ingame-wiki-3d-stage-plan.md, landings 6g and 6h).

    stage3dcheck.py DIR [--gpu vulkan|swiftshader] [--out DIR]

DIR holds the game's captures of the wiki's world target, as tools/wiki_regression.py stage3d
writes them: `<entry>-d<demo>-t<tick>-ss<ss>.game.ppm`, each the stage at one tick of one demo,
before the Gamma curve, gw*ss by gh*ss (tagpu_wikiworld.h, the trigger tagpu_wiki.shot). For each
the mockup renders the same view -- the entry, the demo played from its start to the tick, at the
same ss -- with tools/wiki/mockup/gfx.js, headless (playwright-cli; the real GPU through ANGLE's
Vulkan by default), three times: the whole image (`layers=terrain,features,units`), which is
compared; the units alone (`layers=units`: their bodies, their shadows, the nanoframes and the
ghosts), whose covered pixels are the UNIT pixels; and the features alone (`layers=features`), whose
covered pixels not under a unit are the FEATURE pixels. Every other pixel is a TERRAIN pixel. The
owner's tolerances (the plan's question 8): every terrain pixel equal; at most 0.05 % of the feature
pixels differing; over the unit pixels a mean absolute difference of at most 2/255 a channel, and at
most 0.5 % of them differing by more than 40 on any channel.

With --out, each pair is written beside a difference image (`*.diff.png`: differing pixels white
over the game's image darkened) for review.

Exit 0 when every capture is within the tolerances, 1 when one is not, 2 when it cannot run (no
captures, no pack -- tools/wiki/gfx.py -- or the browser would not render).
"""
import argparse
import base64
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools/wiki"))
import wikilaycheck as wl  # noqa: E402

NAME = re.compile(r"^(?P<entry>[a-z0-9_]+)-d(?P<demo>\d+)-t(?P<tick>\d+)-ss(?P<ss>\d)\.game\.ppm$")
FEATURE_CEILING = 0.0005
UNIT_MEAN = 2.0                 # /255, a channel, over the unit pixels
UNIT_FAR = 40                   # a channel's difference that makes a unit pixel a far one
UNIT_FAR_CEILING = 0.005
GPU_ARGS = {
    "vulkan": ["--use-angle=vulkan", "--enable-features=Vulkan", "--ignore-gpu-blocklist"],
    "swiftshader": ["--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"],
}


class CannotRun(Exception):
    pass


def read_ppm(path):
    from PIL import Image
    data = path.read_bytes()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m or m.group(3) != b"255":
        raise CannotRun(f"{path}: not an 8-bit P6 image")
    w, h = int(m.group(1)), int(m.group(2))
    return Image.frombytes("RGB", (w, h), data[m.end():m.end() + w * h * 3])


class Mockup:
    """One headless browser on the mockup, served from the tree as wikilaycheck.py serves it."""

    def __init__(self, gpu):
        self.httpd = wl.serve()
        self.tmp = Path(tempfile.mkdtemp(prefix="stage3d-"))
        cfg = self.tmp / "pw.json"
        cfg.write_text(json.dumps({"browser": {"browserName": "chromium",
                                               "launchOptions": {"headless": True, "args": GPU_ARGS[gpu]},
                                               "contextOptions": {"deviceScaleFactor": 1,
                                                                  "viewport": {"width": 1920, "height": 1080}}}}))
        self.pw = ["playwright-cli", f"-s=stage3d{os.getpid()}", f"--config={cfg}"]
        self.run("open", "about:blank")

    def run(self, *a):
        return subprocess.run(self.pw + list(a), capture_output=True, text=True, cwd=self.tmp)

    def image(self, entry, demo, tick, ss, layers):
        """The mockup's target for the view, as RGBA, and the tick it reached."""
        from PIL import Image
        port = self.httpd.server_address[1]
        self.run("goto", f"http://127.0.0.1:{port}/tools/wiki/mockup/index.html?res=1920x1080&open&skin=kit&mod=stock"
                         f"&rewind=0&gfx=game&ss={ss}&layers={layers}&entry={entry}&demo={demo}")
        js = ("async () => { const sleep = t => new Promise(r => setTimeout(r, t));"
              " for (let i = 0; i < 400 && !(window.WikiPage && WikiPage.sim() && window.WikiGfx"
              "      && WikiGfx.state() !== 'loading' && WikiGfx.state() !== 'none'); i++) await sleep(50);"
              " if (!window.WikiGfx || WikiGfx.state() !== 'ready') return JSON.stringify({ err: 'gfx ' + (window.WikiGfx && WikiGfx.state()) });"
              " const S = WikiPage.state; if (S.playing) document.querySelector('#c-play').click();"
              " const sim = WikiPage.sim();"
              f" while (sim.tick < {tick} && !sim.done) sim.step();"
              " let r = null;"
              " for (let i = 0; i < 400; i++) { r = WikiGfx.frame(S.entry, sim.view(), sim.tick); if (r && r.complete) break; await sleep(50); }"
              " if (!r || !r.complete) return JSON.stringify({ err: 'no complete image', r: r && { complete: r.complete, refused: r.refused, glError: r.glError } });"
              " return JSON.stringify({ tick: sim.tick, url: r.canvas.toDataURL('image/png'),"
              "   renderer: (() => { const g = document.createElement('canvas').getContext('webgl2');"
              "     const e = g && g.getExtension('WEBGL_debug_renderer_info'); return e ? g.getParameter(e.UNMASKED_RENDERER_WEBGL) : '?'; })() }); }")
        out = wl.result(self.run("eval", js).stdout)
        got = json.loads(out) if out else {"err": "no answer from the page"}
        if "err" in got:
            raise CannotRun(f"the mockup at {entry} demo {demo} tick {tick} ss {ss} ({layers}): {got}")
        png = base64.b64decode(got["url"].split(",", 1)[1])
        return Image.open(io.BytesIO(png)).convert("RGBA"), got["tick"], got["renderer"]

    def close(self):
        self.run("close")
        self.httpd.shutdown()
        shutil.rmtree(self.tmp, ignore_errors=True)


def compare(game, whole, feats, units):
    """The three classes' numbers, and the mask of differing pixels: {terrain: (px, differ),
    features: (px, differ), units: (px, mean abs diff /255, far)}."""
    import numpy as np
    g = np.asarray(game, dtype=np.int16)
    m = np.asarray(whole.convert("RGB"), dtype=np.int16)
    umask = np.asarray(units, dtype=np.uint8)[..., 3] > 0
    fmask = (np.asarray(feats, dtype=np.uint8)[..., 3] > 0) & ~umask
    tmask = ~umask & ~fmask
    ad = np.abs(g - m)
    diff = (ad > 0).any(axis=2)
    nu = int(umask.sum())
    return ({"terrain": (int(tmask.sum()), int((diff & tmask).sum())),
             "features": (int(fmask.sum()), int((diff & fmask).sum())),
             "units": (nu, float(ad[umask].mean()) if nu else 0.0, int(((ad > UNIT_FAR).any(axis=2) & umask).sum()))},
            diff)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("dir", type=Path)
    p.add_argument("--gpu", choices=sorted(GPU_ARGS), default="vulkan")
    p.add_argument("--out", type=Path)
    a = p.parse_args()
    shots = sorted(f for f in a.dir.glob("*.game.ppm") if NAME.match(f.name))
    if not shots:
        print(f"stage3dcheck: cannot run: no <entry>-d<demo>-t<tick>-ss<ss>.game.ppm in {a.dir}")
        return 2
    if not (TREE / "tools/wiki/mockup/gfx/index.json").exists():
        print("stage3dcheck: cannot run: no mockup pack - run tools/wiki/gfx.py")
        return 2
    import numpy as np
    from PIL import Image
    t0 = time.monotonic()
    bad = 0
    mk = Mockup(a.gpu)
    try:
        for f in shots:
            m = NAME.match(f.name)
            entry, demo, tick, ss = m["entry"], int(m["demo"]), int(m["tick"]), int(m["ss"])
            game = read_ppm(f)
            ground, t1, renderer = mk.image(entry, demo, tick, ss, "terrain,features,units")
            feats, _, _ = mk.image(entry, demo, tick, ss, "features")
            units, _, _ = mk.image(entry, demo, tick, ss, "units")
            if t1 != tick:
                raise CannotRun(f"{f.name}: the mockup's demo stopped at tick {t1}")
            if game.size != ground.size:
                print(f"DIFFER {f.name}: the game's target is {game.size}, the mockup's {ground.size}")
                bad += 1
                continue
            got, diff = compare(game, ground, feats, units)
            (nt, dt), (nf, df), (nu, mean, far) = got["terrain"], got["features"], got["units"]
            share = df / nf if nf else 0.0
            fshare = far / nu if nu else 0.0
            ok = dt == 0 and share <= FEATURE_CEILING and mean <= UNIT_MEAN and fshare <= UNIT_FAR_CEILING
            bad += not ok
            print(f"{'ok    ' if ok else 'DIFFER'} {f.name}: terrain {nt} px, {dt} differ; features {nf} px, {df} differ "
                  f"({100 * share:.3f} %, the ceiling {100 * FEATURE_CEILING:.2f} %); units {nu} px, mean "
                  f"{mean:.2f}/255 (the ceiling {UNIT_MEAN:.0f}), {far} past {UNIT_FAR} ({100 * fshare:.3f} %, the ceiling "
                  f"{100 * UNIT_FAR_CEILING:.1f} %)")
            if a.out:
                a.out.mkdir(parents=True, exist_ok=True)
                stem = f.name[:-len(".game.ppm")]
                game.save(a.out / f"{stem}.game.png")
                ground.save(a.out / f"{stem}.mockup.png")
                g = (np.asarray(game, dtype=np.uint16) // 3).astype(np.uint8)
                g[diff] = 255
                Image.fromarray(g).save(a.out / f"{stem}.diff.png")
        print(f"stage3dcheck: the mockup rendered on {renderer}")
    except CannotRun as x:
        print(f"stage3dcheck: cannot run: {x}")
        return 2
    finally:
        mk.close()
    s = time.monotonic() - t0
    if bad:
        print(f"stage3dcheck: DIFFER - {bad} of {len(shots)} capture(s) outside the tolerances ({s:.0f} s)")
        return 1
    print(f"stage3dcheck: all {len(shots)} capture(s) within the tolerances - every terrain pixel equal, the features "
          f"within {100 * FEATURE_CEILING:.2f} %, the units within a mean {UNIT_MEAN:.0f}/255 and "
          f"{100 * UNIT_FAR_CEILING:.1f} % far ({s:.0f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
