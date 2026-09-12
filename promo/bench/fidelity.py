"""Does the canonical clamp change the picture, and does it change the JITTER?

Jitter is the thing the supersample ladder exists to control, so a speedup that
buys its time from the ladder has to be judged on the temporal second
difference, not on a still frame.
"""
import os, sys, time
from pathlib import Path
import importlib.util
from importlib.machinery import SourceFileLoader
import numpy as np
from PIL import Image

def load(path, name):
    loader = SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    loader.exec_module(mod)
    return mod

SHOOT, W, H = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
OUT = Path(sys.argv[4]); OUT.mkdir(parents=True, exist_ok=True)
# The "before" is whatever tools/tamontage was at the given rev -- extracted
# here rather than left lying in a scratch directory, so this is re-runnable.
import subprocess, tempfile
REV = os.environ.get("TAMONTAGE_BASE_REV", "HEAD")
_tmp = Path(tempfile.mkdtemp()) / "tamontage_base.py"
_tmp.write_text(subprocess.run(["git", "show", f"{REV}:tools/tamontage"],
                               capture_output=True, text=True, check=True).stdout)
old = load(str(_tmp), "tm_old")
new = load("tools/tamontage", "tm_new")

def mk(mod):
    return mod.Montage(Path("promo/tacli-promo.json"), "clip", Path(SHOOT + "/v2"),
                       Path(SHOOT + "/cache2"), 30.0, aspect=H / W)
mo, mn = mk(old), mk(new)

def strip(m, t):
    """Five consecutive frames at t, as gray float -- enough for two 2nd diffs."""
    fs = []
    for k in range(5):
        im = m.render_frame(t + k / 30.0, W, H)
        fs.append(np.asarray(im.convert("L"), dtype=np.float32))
    return np.stack(fs)

print(f"{'t':>5} {'tile px':>8} {'PSNR':>7} {'>2 off':>7} {'max':>4} "
      f"{'jitter old':>11} {'jitter new':>11}  verdict")
for t in (3.0, 8.0, 14.0, 20.0, 26.0, 30.0, 36.0, 44.0, 51.0, 56.0):
    so, sn = strip(mo, t), strip(mn, t)
    a = np.asarray(mo.render_frame(t, W, H)).astype(np.int16)
    b = np.asarray(mn.render_frame(t, W, H)).astype(np.int16)
    d = np.abs(a - b)
    mse = float((d.astype(np.float64) ** 2).mean())
    psnr = 10 * np.log10(255.0 ** 2 / max(mse, 1e-9))
    jo = float(np.abs(2 * so[1:-1] - so[:-2] - so[2:]).mean())
    jn = float(np.abs(2 * sn[1:-1] - sn[:-2] - sn[2:]).mean())
    cx, cy, cols = mn.camera.at(t)
    px = mn.stage.tile_w * W / (cols * mn.stage.pitch_x)
    verdict = "same" if jn <= jo * 1.05 else ("WORSE %+.0f%%" % ((jn/jo-1)*100))
    print(f"{t:5.1f} {px:8.0f} {psnr:6.1f}dB {float((d.max(axis=2)>2).mean())*100:6.1f}% "
          f"{int(d.max()):4d} {jo:11.3f} {jn:11.3f}  {verdict}")
    if t in (3.0, 14.0, 26.0):
        Image.fromarray(a.astype(np.uint8)).save(OUT / f"old-{t:04.1f}.png")
        Image.fromarray(b.astype(np.uint8)).save(OUT / f"new-{t:04.1f}.png")
