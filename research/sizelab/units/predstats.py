"""predstats.py -- per field: how many unit-ticks each predictor misses (residual != 0)."""
import numpy as np
L = lambda k: np.load(f"series/{k}.npy", mmap_mode="r")
gt = np.load("series/gametime.npy").astype(np.int64)
live = np.asarray(L("live"))
ok3 = live[2:] & live[1:-1] & live[:-2]            # t, t-1, t-2 live (same unit assumed)
mover = np.asarray(L("mover"))[2:] != 0
def sl(a): a = np.asarray(a).astype(np.int64); return a[2:], a[1:-1], a[:-2]
def wrap(d, bits): m = 1 << bits; d = d % m; return np.where(d >= m >> 1, d - m, d)
def report(name, bits, preds, mask=ok3):
    tot = mask.sum()
    s = f"{name:10s} unit-ticks {tot:>9}: "
    for pn, r in preds.items():
        nz = (wrap(r, bits) != 0) & mask
        s += f"{pn} {nz.sum():>9}  "
    print(s)
x0, x1, x2 = sl(L("x")); z0, z1, z2 = sl(L("z")); y0, y1, y2 = sl(L("y"))
vx0, vx1, _ = sl(L("vx")); vz0, vz1, _ = sl(L("vz")); vy0, vy1, _ = sl(L("vy"))
for nm, (a0, a1, a2), v0 in (("x", (x0, x1, x2), vx0), ("z", (z0, z1, z2), vz0), ("y", (y0, y1, y2), vy0)):
    report(nm, 32, {"prev": a0 - a1, "lin": a0 - 2 * a1 + a2, "rule(+v)": a0 - a1 - v0,
                    "best(lin|rule)": np.where((wrap(a0 - 2*a1 + a2, 32) == 0), 0, a0 - a1 - v0)})
h0, h1, h2 = sl(L("heading")); t0, t1, _ = sl(L("turnrate"))
report("heading", 16, {"prev": h0 - h1, "lin": h0 - 2 * h1 + h2, "rule(+tr)": h0 - h1 - t0})
for f in ("pitch", "roll", "vx", "vz", "vy", "speed", "turnrate", "m26", "m28", "aimh", "aimp"):
    a0, a1, a2 = sl(L(f))
    bits = 32 if f in ("vx", "vz", "vy", "speed") else 16
    report(f, bits, {"prev": a0 - a1, "lin": a0 - 2 * a1 + a2})
s0, s1, s2 = sl(L("stamp"))
G = gt[2:, None]
ml0, ml1, _ = sl(L("mlayer"))
moved = (vx0 != 0) | (vy0 != 0) | (vz0 != 0) | ((ml0 & 3) != (ml1 & 3))
report("stamp", 32, {"prev": s0 - s1, "lin": s0 - 2 * s1 + s2, "rule": s0 - np.where(moved, G - 1, s1)}, ok3 & mover)
for f in ("reload", "fa"):
    a0, a1, a2 = sl(L(f))
    report(f, 16, {"prev": a0 - a1, "lin": a0 - 2 * a1 + a2, "rule": a0 - np.maximum(a1 - 1, 0)})
