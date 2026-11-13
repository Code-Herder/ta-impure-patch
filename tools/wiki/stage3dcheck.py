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
captures, no pack -- tools/wiki/gfx.py -- or the browser would not render: the reason the page gave,
with its console's errors).
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
from importlib.machinery import SourceFileLoader
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
SLICE_MS = 10000                # Mockup.settle: one eval's wait inside the page, after which it says where it stands
SETTLE_S = 1200                 # Mockup.settle: a page still pending past this broke settled()'s rule
PW_CALL_S = 600                 # Mockup.run: a call that has not returned by then has a stuck browser or page


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


def section(out, name):
    """The lines of a playwright-cli answer between `### <name>` and the next `###`, or ''"""
    m = re.search(rf"^### {name}\n(.*?)(?=^### |\Z)", out, re.M | re.S)
    return m.group(1).strip() if m else ""


class Mockup:
    """One headless browser on the mockup, served by tools/wiki/serve.py, so the page plays its demos with the units'
    scripts as the game's wiki does (without /cob it plays them without).

    EVERY WAIT IS ON THE PAGE'S OWN SIGNAL, never on a time: the page settled (wiki.js settled(): 'pending: <what>'
    only while a load it waits on is in flight, each of which resolves or fails; else 'ready', or why it never will
    be), then the stage's pack (gfx.js settled(), the same rule). A page settles in seconds alone and far later beside
    a gate's other lanes, which play games and drive mockups of their own on the same CPU and GPU, and a tree's first
    page also waits on serve.py building the simulation host: no fixed wait is long enough and none is needed. A slow
    page is waited for, and a page that cannot start is named, with its console's errors. Each eval waits a slice
    inside the page (a playwright-cli call is a process) and says where it stands; the slice decides nothing.
    SETTLE_S and PW_CALL_S only bound a page or a browser that breaks that rule, failing with what it still waited
    for; neither ever passes a picture."""

    def __init__(self, gpu):
        self.httpd = SourceFileLoader("wikiserve", str(Path(__file__).with_name("serve.py"))).load_module().server(quiet=True)
        self.port = self.httpd.server_address[1]
        self.tmp = Path(tempfile.mkdtemp(prefix="stage3d-"))
        cfg = self.tmp / "pw.json"
        cfg.write_text(json.dumps({"browser": {"browserName": "chromium",
                                               "launchOptions": {"headless": True, "args": GPU_ARGS[gpu]},
                                               "contextOptions": {"deviceScaleFactor": 1,
                                                                  "viewport": {"width": 1920, "height": 1080}}}}))
        self.pw = ["playwright-cli", f"-s=stage3d{os.getpid()}", f"--config={cfg}"]
        self.run("open", "about:blank")

    def run(self, *a):
        try:
            return subprocess.run(self.pw + list(a), capture_output=True, text=True, cwd=self.tmp, timeout=PW_CALL_S)
        except subprocess.TimeoutExpired:
            raise CannotRun(f"playwright-cli {a[0]} did not return in {PW_CALL_S} s: the mockup's browser or its page "
                            "is stuck") from None

    def eval(self, js, what):
        """The value the page function `js` returned. An eval that threw is named by its error (a page whose scripts
        did not start has no WikiPage to ask): never an empty answer."""
        r = self.run("eval", js)
        v = wl.result(r.stdout)
        if v is None:
            raise CannotRun(f"{what}: {section(r.stdout, 'Error') or (r.stdout + r.stderr).strip()[-600:] or 'no output'}")
        return v

    def settle(self, status, what):
        """Waits while the page function `status` answers 'pending: ...'; it must then answer 'ready', or CannotRun
        names what it answered."""
        end = time.monotonic() + SETTLE_S
        while (s := self.eval(f"async () => {{ const s = {status}, t0 = performance.now();"
                              f" while (s().startsWith('pending') && performance.now() - t0 < {SLICE_MS})"
                              " await new Promise(r => setTimeout(r, 50)); return s(); }", what)).startswith("pending"):
            if time.monotonic() > end:
                raise CannotRun(f"{what}: still {s} after {SETTLE_S} s: a load that never resolved, which the page's "
                                "settled() rules out")
        if s != "ready":
            raise CannotRun(f"{what}: {s}")

    def console(self):
        """The page's console messages of level error, from the file playwright-cli writes them to, or ''"""
        m = re.search(r"\((\.playwright-cli/console-[^)]+\.log)", self.run("console", "error").stdout)
        f = self.tmp / m.group(1) if m else None
        return f.read_text(errors="replace").strip()[-1500:] if f and f.exists() else ""

    def image(self, entry, demo, tick, ss, layers):
        """The mockup's target for the view, as RGBA, the tick it reached and the renderer's name."""
        from PIL import Image
        what = f"the mockup at {entry} demo {demo} tick {tick} ss {ss} ({layers})"
        try:
            got = json.loads(self.view(entry, demo, tick, ss, layers, what))
            if "err" in got:
                raise CannotRun(f"{what}: {got['err']}")
        except CannotRun as x:
            errors = self.console()
            raise CannotRun(f"{x}{'; the page console: ' + errors if errors else ''}") from None
        png = base64.b64decode(got["url"].split(",", 1)[1])
        return Image.open(io.BytesIO(png)).convert("RGBA"), got["tick"], got["renderer"]

    def view(self, entry, demo, tick, ss, layers, what):
        """THE PAGE, THE DEMO AT THE TICK, THE STAGE'S PACK, THE IMAGE, each step after the one before has settled:
        the page settled; the demo afresh and paused in one task, whatever the settling left it at (a demo plays while
        the page loads), and stepped to the tick, the frame drawn there asking for the stage's pack; the pack settled;
        then the image of the demo this process paused, as JSON."""
        # the navigation's own answer is not read: a page held past playwright-cli's wait for its load (by a
        # synchronous /sim request while serve.py builds the host) still settles, and the settling is waited for
        self.run("goto", f"http://127.0.0.1:{self.port}/tools/wiki/mockup/index.html?res=1920x1080&open&skin=kit"
                         f"&mod=stock&rewind=0&gfx=game&ss={ss}&layers={layers}&entry={entry}&demo={demo}")
        self.settle("() => window.WikiPageFailed ? `the page did not start: ${WikiPageFailed}`"
                    " : !location.pathname.endsWith('/tools/wiki/mockup/index.html') ? `pending: the navigation, at ${location.href}`"
                    " : !window.WikiPage ? 'pending: the page scripts and layout.json'"
                    " : !window.WikiGfx ? 'gfx.js is not loaded' : WikiPage.settled()", what)
        got = json.loads(self.eval(
            "() => { const S = WikiPage.state, $ = s => document.querySelector(s);"
            " if (WikiGfx.state() !== 'ready') return JSON.stringify({ err: `the game graphics are ${WikiGfx.state()}` });"
            " $('#c-replay').click(); if (S.playing) $('#c-play').click();"
            " const sim = WikiPage.sim();"
            " if (!sim || S.playing) return JSON.stringify({ err: `the demo did not restart paused: ${WikiPage.settled()}` });"
            " if (sim.view().cob === null) return JSON.stringify({ err: 'the page plays without the scripts of the units' });"
            f" while (sim.tick < {tick} && !sim.done) sim.step();"
            " window.stage3dSim = sim;"
            " WikiGfx.frame(WikiPage.stageId(), sim.view(), sim.tick, undefined, sim.poses());"
            " return JSON.stringify({ tick: sim.tick }); }", what))
        if "err" in got:
            return json.dumps(got)
        self.settle("() => { const s = WikiGfx.settled(WikiPage.stageId());"
                    " return s === 'pending' ? `pending: the stage ${WikiPage.stageId()} (${WikiGfx.state()})` : s; }", what)
        # a stage that has opened renders synchronously, so an image incomplete now is incomplete for good
        return self.eval(
            "() => { const S = WikiPage.state, sim = window.stage3dSim;"
            " if (sim !== WikiPage.sim() || S.playing) return JSON.stringify({ err: 'the page restarted the demo this process paused' });"
            " const r = WikiGfx.frame(WikiPage.stageId(), sim.view(), sim.tick, undefined, sim.poses());"
            " if (!r) return JSON.stringify({ err: `the stage ${WikiPage.stageId()} drew nothing: its pack did not open` });"
            " if (!r.complete) return JSON.stringify({ err: `an incomplete image: ${r.refused} unit(s) refused, GL error ${r.glError}` });"
            " return JSON.stringify({ tick: sim.tick, url: r.canvas.toDataURL('image/png'),"
            "   renderer: (() => { const g = document.createElement('canvas').getContext('webgl2');"
            "     const e = g && g.getExtension('WEBGL_debug_renderer_info'); return e ? g.getParameter(e.UNMASKED_RENDERER_WEBGL) : '?'; })() }); }",
            what)

    def close(self):
        try:
            self.run("close")
        except CannotRun as x:
            print(f"stage3dcheck: {x}")
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
    try:
        mk = Mockup(a.gpu)
    except CannotRun as x:
        print(f"stage3dcheck: cannot run: {x}")
        return 2
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
