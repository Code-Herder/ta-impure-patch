"""CPU vs the exact GPU compositor: time, fidelity, jitter AND sharpness."""
import sys, time
from pathlib import Path
import importlib.util
from importlib.machinery import SourceFileLoader
import numpy as np
from PIL import Image
import torch
sys.path.insert(0, str(Path(__file__).parent))
loader = SourceFileLoader("tamontage", "tools/tamontage")
spec = importlib.util.spec_from_loader("tamontage", loader)
tm = importlib.util.module_from_spec(spec); sys.modules["tamontage"] = tm
loader.exec_module(tm)
from drawlist import draw_list, cpu_composite
from exact import ExactGpuCompositor

SHOOT, W, H = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
TS = [float(x) for x in sys.argv[4].split(",")]
OUT = Path(sys.argv[5]) if len(sys.argv) > 5 else None
m = tm.Montage(Path("promo/tacli-promo.json"), "clip", Path(SHOOT + "/v2"),
               Path(SHOOT + "/cache2"), 30.0, aspect=H / W)
g = ExactGpuCompositor()

def timeit(fn, n=3):
    fn(); torch.cuda.synchronize()
    t0 = time.perf_counter()
    for _ in range(n): fn()
    torch.cuda.synchronize()
    return (time.perf_counter() - t0) / n

def sharp(x):
    return float((np.diff(x, axis=1)**2).mean() + (np.diff(x, axis=0)**2).mean())

print(f"{'t':>5} {'tiles':>6} {'draw':>7} {'CPU':>7} {'GPU':>7} {'x':>6}  "
      f"{'PSNR':>7} {'sharp':>8} {'jitCPU':>7} {'jitGPU':>7}")
for t in TS:
    s_, ti, fl, bg = draw_list(m, t, W, H)
    tdraw = timeit(lambda: draw_list(m, t, W, H))
    tcpu = timeit(lambda: cpu_composite(s_, ti, fl, bg, W, H))
    tgpu = timeit(lambda: g.frame(s_, ti, fl, bg, W, H))
    ci = cpu_composite(s_, ti, fl, bg, W, H)
    gi = Image.fromarray(g.to_numpy(g.frame(s_, ti, fl, bg, W, H)))
    a = np.asarray(ci).astype(np.int16); b = np.asarray(gi).astype(np.int16)
    mse = float(((a - b).astype(np.float64) ** 2).mean())
    psnr = 10*np.log10(255.0**2/max(mse,1e-9))
    cg = np.asarray(ci.convert("L"), np.float32); gg = np.asarray(gi.convert("L"), np.float32)
    dsharp = (sharp(gg)/sharp(cg)-1)*100
    cs, gs = [], []
    for k in range(5):
        s2, t2, f2, b2 = draw_list(m, t + k/30.0, W, H)
        cs.append(np.asarray(cpu_composite(s2,t2,f2,b2,W,H).convert("L"), np.float32))
        gs.append(np.asarray(Image.fromarray(g.to_numpy(g.frame(s2,t2,f2,b2,W,H))).convert("L"), np.float32))
    cs, gs = np.stack(cs), np.stack(gs)
    jc = float(np.abs(2*cs[1:-1]-cs[:-2]-cs[2:]).mean())
    jg = float(np.abs(2*gs[1:-1]-gs[:-2]-gs[2:]).mean())
    print(f"{t:5.1f} {len(ti):6d} {tdraw*1000:6.0f}m {tcpu*1000:6.0f}m {tgpu*1000:6.0f}m "
          f"{tcpu/max(tgpu,1e-6):5.1f}x  {psnr:6.1f}dB {dsharp:+7.1f}% {jc:7.3f} {jg:7.3f}")
    if OUT and t in (26.0, 51.0):
        OUT.mkdir(parents=True, exist_ok=True)
        box = (1500, 800, 2260, 1220)
        o = Image.new("RGB", (760, 848), (255,0,0))
        o.paste(ci.crop(box), (0,0)); o.paste(gi.crop(box), (0,428))
        o.save(OUT / f"exact-{t:04.1f}.png")
