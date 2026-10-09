"""rules.py -- test the engine rules the counters and integrations follow, against the series."""
import numpy as np
L = lambda k: np.load(f"series/{k}.npy", mmap_mode="r")
gt = np.load("series/gametime.npy").astype(np.int64)
live = L("live"); both = live[1:] & live[:-1]
mover = L("mover")[1:] != 0
x, y, zz = (L(k).astype(np.int64) for k in "xyz")
ml = L("mlayer") & 3
stamp = L("stamp").astype(np.int64)
moved = (x[1:] != x[:-1]) | (y[1:] != y[:-1]) | (zz[1:] != zz[:-1]) | (ml[1:] != ml[:-1])
st_ch = stamp[1:] != stamp[:-1]
m = both & mover
print("unit-ticks live both & mover:", m.sum())
print("moved:", (m & moved).sum(), " stamp changed:", (m & st_ch).sum())
print("moved & stamp unchanged:", (m & moved & ~st_ch).sum(), " stamp changed & not moved:", (m & st_ch & ~moved).sum())
G = np.broadcast_to(gt[1:, None], stamp[1:].shape)
for off in (-2, -1, 0, 1):
    print(f"  stamp == gametime{off:+d} where changed:", (m & st_ch & (stamp[1:] == G + off)).sum())
# reload
for name in ("reload", "fa"):
    r = L(name).astype(np.int64)
    pred = np.maximum(r[:-1] - 1, 0)
    ch = r[1:] != r[:-1]
    ok = r[1:] == pred
    print(f"{name}: changes {(both & ch).sum()}, follow max(prev-1,0) {(both & ok & ch).sum()}, "
          f"unchanged but predicted to drop {(both & ~ch & ~ok).sum()}, other changes {(both & ch & ~ok).sum()}")
# position integration
for p, v in (("x", "vx"), ("y", "vy"), ("z", "vz")):
    P = L(p).astype(np.int64); V = L(v).astype(np.int64)
    d = P[1:] - P[:-1]
    print(f"{p}: changes {(m & (d != 0)).sum()}, == +v(t) {(m & (d != 0) & (d == V[1:])).sum()}, == +v(t-1) {(m & (d != 0) & (d == V[:-1])).sum()}, "
          f"zero with v(t)!=0 {(m & (d == 0) & (V[1:] != 0)).sum()}")
h = L("heading").astype(np.int64); tr = L("turnrate").astype(np.int64)
dh = (h[1:] - h[:-1]) % 65536
dh = np.where(dh >= 32768, dh - 65536, dh)
print(f"heading: changes {(m & (dh != 0)).sum()}, == +turnrate(t) {(m & (dh != 0) & (dh == tr[1:])).sum()}, == +turnrate(t-1) {(m & (dh != 0) & (dh == tr[:-1])).sum()}")
