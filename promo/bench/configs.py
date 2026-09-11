"""End-to-end cost per configuration. The clip-resize share is measured inside
the SAME timed calls it is subtracted from, or the subtraction is meaningless."""
import sys, time
from pathlib import Path
import importlib.util
from importlib.machinery import SourceFileLoader
import numpy as np
import torch
import torch.nn.functional as F
sys.path.insert(0, str(Path(__file__).parent))
loader = SourceFileLoader("tamontage", "tools/tamontage")
spec = importlib.util.spec_from_loader("tamontage", loader)
tm = importlib.util.module_from_spec(spec); sys.modules["tamontage"] = tm
loader.exec_module(tm)
if "--no-clamp" in sys.argv:            # configuration A, in this same harness
    _cs = tm.canonical_size
    tm.canonical_size = lambda w, h, native_w=None: _cs(w, h, None)
from drawlist import draw_list, cpu_composite
from exact import ExactGpuCompositor

SHOOT, W, H = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
m = tm.Montage(Path("promo/tacli-promo.json"), "clip", Path(SHOOT + "/v2"),
               Path(SHOOT + "/cache2"), 30.0, aspect=H / W)
g = ExactGpuCompositor()
draw_list(m, 0.0, W, H)

ACC = {"s": 0.0, "jobs": []}
_orig = tm.Clip.frame
def timed(self, t, w, h, disp_w=None):
    t0 = time.perf_counter()
    r = _orig(self, t, w, h, disp_w)
    ACC["s"] += time.perf_counter() - t0
    ACC["jobs"].append((self.native_w, r.height * self.native_w // max(1, r.width) if r.width else 1, w, h))
    return r
tm.Clip.frame = timed

def t_gpu(fn, n=3):
    fn(); torch.cuda.synchronize()
    t0 = time.perf_counter()
    for _ in range(n): fn()
    torch.cuda.synchronize()
    return (time.perf_counter() - t0) / n

N = 3
TOT = {"B": 0.0, "C": 0.0, "D": 0.0}
rows = []
for i in range(0, 58, 2):
    t = float(i)
    draw_list(m, t, W, H)                       # warm, not measured
    ACC["s"] = 0.0; ACC["jobs"] = []
    t0 = time.perf_counter()
    for _ in range(N):
        s_, ti, fl, bg = draw_list(m, t, W, H)
    tdraw = (time.perf_counter() - t0) / N
    tclip = ACC["s"] / N
    jobs = ACC["jobs"][:len(ACC["jobs"]) // N]  # one rep's worth

    t0 = time.perf_counter()
    for _ in range(N): cpu_composite(s_, ti, fl, bg, W, H)
    tcpu = (time.perf_counter() - t0) / N
    tgpu = t_gpu(lambda: g.frame(s_, ti, fl, bg, W, H))

    # the same clip resizes, batched on the GPU at their real sizes
    batch = {}
    for nw, nh, dw, dh in jobs:
        batch.setdefault((nw, max(1, nh), dw, dh), 0)
        batch[(nw, max(1, nh), dw, dh)] += 1
    tgsrc = 0.0
    for (nw, nh, dw, dh), cnt in batch.items():
        src = torch.rand((cnt, 3, nh, nw), device="cuda", dtype=torch.float16)
        tgsrc += t_gpu(lambda: F.interpolate(src, size=(max(1, dh), max(1, dw)),
                                             mode="bilinear", align_corners=False,
                                             antialias=(dw < nw)), 3)
    B = tdraw + tcpu
    C = tdraw + tgpu
    D = max(0.0, tdraw - tclip) + tgsrc + tgpu
    rows.append((t, len(ti), tdraw, tclip, tcpu, tgpu, tgsrc, B, C, D))
    for k, v in (("B", B), ("C", C), ("D", D)): TOT[k] += v * 60

print(f"{'t':>4} {'tiles':>6} | {'draw':>6} {'clip':>6} {'chrome':>7} | "
      f"{'CPUcmp':>7} {'GPUcmp':>7} {'GPUsrc':>7} | {'B':>6} {'C':>6} {'D*':>6}")
for t, n, td, tc, tcp, tg, tgs, B, C, D in rows:
    print(f"{t:4.0f} {n:6d} | {td*1000:6.0f} {tc*1000:6.0f} {(td-tc)*1000:7.0f} | "
          f"{tcp*1000:7.0f} {tg*1000:7.0f} {tgs*1000:7.1f} | "
          f"{B*1000:6.0f} {C*1000:6.0f} {D*1000:6.0f}")
print()
tag = "A (clamp OFF)" if "--no-clamp" in sys.argv else "B (clamp on)"
print(f"  CPU draw + CPU composite  {tag:16s}            {TOT['B']:6.0f} s single-thread")
print(f"  CPU draw + GPU composite  (built and verified)      {TOT['C']:6.0f} s single-thread")
print(f"  GPU source + GPU composite  (* projection only)     {TOT['D']:6.0f} s single-thread")
print("\n  * D excludes PNG decode and host->device upload; it is the ceiling,")
print("    not a measured pipeline. See README.md.")
print(f"\npeak VRAM: {torch.cuda.max_memory_allocated()/1e9:.2f} GB")
