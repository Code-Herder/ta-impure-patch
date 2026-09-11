"""Cost profile across the whole film: where do the 3.5 minutes actually go?"""
import sys, time
from pathlib import Path
import importlib.util
from importlib.machinery import SourceFileLoader
loader = SourceFileLoader("tamontage", "tools/tamontage")
spec = importlib.util.spec_from_loader("tamontage", loader)
tm = importlib.util.module_from_spec(spec); loader.exec_module(tm)

SHOOT, W, H = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
m = tm.Montage(Path("promo/tacli-promo.json"), "clip", Path(SHOOT + "/v2"),
               Path(SHOOT + "/cache2"), 30.0, aspect=H / W)
m.render_frame(0.0, W, H)

tot = 0.0
rows = []
for i in range(0, 58, 2):
    t = float(i)
    m.render_frame(t, W, H)                       # warm the clip LRU for this t
    t0 = time.perf_counter(); m.render_frame(t, W, H); dt = time.perf_counter() - t0
    cx, cy, cols = m.camera.at(t)
    sc = W / (cols * m.stage.pitch_x)
    vis = sum(1 for wn in m.windows
              if wn.alpha(t) > 0 and not (
                  (wn.rect()[0]-cx)*sc + W/2 + wn.rect()[2]*sc < 0 or
                  (wn.rect()[1]-cy)*sc + H/2 + wn.rect()[3]*sc < 0 or
                  (wn.rect()[0]-cx)*sc + W/2 > W or (wn.rect()[1]-cy)*sc + H/2 > H))
    tile_px = m.stage.tile_w * sc
    rows.append((t, cols, vis, tile_px, dt))
    tot += dt * 2 * 30                             # this t stands for 2 s = 60 frames
print(f"{'t':>4} {'cols':>6} {'vis':>5} {'tile px':>8} {'ms/frame':>9}  share of film")
for t, cols, vis, px, dt in rows:
    share = dt * 60 / tot
    print(f"{t:4.0f} {cols:6.2f} {vis:5d} {px:8.1f} {dt*1000:9.1f}  "
          f"{share*100:5.1f}% {'#'*int(share*200)}")
print(f"\nextrapolated single-threaded cost of the film: {tot:.0f} s "
      f"({tot/14:.0f} s across 14 workers)")
