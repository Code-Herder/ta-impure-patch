"""qstats.py -- what the contract's quantization needs: per-tick and per-10-Hz-sample steps of the
whole-wu position and the 8-bit heading, and the linear-extrapolation residuals at 10 Hz."""
import numpy as np, collections
L = lambda k: np.asarray(np.load(f"series/{k}.npy", mmap_mode="r"))
live = L("live"); gt = np.load("series/gametime.npy")
rows = np.flatnonzero(gt % 3 == 0)
lv = live[rows]; ok3 = lv[2:] & lv[1:-1] & lv[:-2]
qx = np.round(L("x")[rows].astype(np.float64) / 65536).astype(np.int64)
qz = np.round(L("z")[rows].astype(np.float64) / 65536).astype(np.int64)
qh = (np.round(L("heading")[rows].astype(np.float64) / 256).astype(np.int64)) % 256
def dist(name, d, mask):
    v = d[mask]; c = collections.Counter(np.clip(v, -9, 9).tolist()); n = len(v)
    print(f"{name:34s} n={n:>8}  zero {100*c[0]/n:5.1f} %  |r|<=1 {100*(np.abs(v)<=1).mean():5.1f} %  |r|<=3 {100*(np.abs(v)<=3).mean():5.1f} %  max|r| {np.abs(v).max()}")
for nm, q in (("x", qx), ("z", qz)):
    dist(f"{nm} step per 10 Hz sample (wu)", q[1:] - q[:-1], lv[1:] & lv[:-1])
    dist(f"{nm} lin residual at 10 Hz (wu)", q[2:] - 2 * q[1:-1] + q[:-2], ok3)
dh = (qh[1:] - qh[:-1] + 128) % 256 - 128
dist("heading step per sample (1/256)", dh, lv[1:] & lv[:-1])
rh = (qh[2:] - 2 * qh[1:-1] + qh[:-2] + 128) % 256 - 128
dist("heading lin residual (1/256)", rh, ok3)
