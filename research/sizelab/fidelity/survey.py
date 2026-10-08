#!/usr/bin/env python3
"""survey.py -- what the motion fields are: change rates, step sizes, units, and how the
position, the velocity and the stamps relate, over every live unit-tick of the battle."""
import sys
from pathlib import Path

import numpy as np

d = Path(sys.argv[1])
L = np.load(d / "live.npy").astype(bool)
f = {n: np.load(d / f"f_{n}.npy", mmap_mode="r") for n in
     ("x", "alt", "z", "tx", "hd", "tz", "vx", "vy", "vz", "spd", "w24", "claim", "moved",
      "layer", "fx", "fz", "sx", "sz", "sh", "bucket", "build", "hp", "water", "state", "type",
      "fw", "fh")}
T, S = L.shape
both = L[1:] & L[:-1]                       # live at t-1 and t (the same unit unless recycled)
# a recycled slot: dead in between is excluded by `both`; a death and a create in one tick
# would show as a jump -- count them by type change
tch = (f["type"][1:] != f["type"][:-1]) & both
both &= ~tch
n = both.sum()
print(f"unit-ticks with a previous tick: {n:,}; live unit-ticks {L.sum():,}; type changes {tch.sum()}")


def rate(name, a=None):
    a = f[name] if a is None else a
    ch = (a[1:] != a[:-1]) & both
    return ch.sum()


for name in ("x", "alt", "z", "tx", "hd", "tz", "vx", "vy", "vz", "spd", "w24", "claim", "moved",
             "layer", "fx", "fz", "sx", "sz", "sh", "bucket", "build", "hp", "water", "state"):
    c = rate(name)
    print(f"  {name:<7} changed in {c:>10,} unit-ticks  {100 * c / n:5.1f} %")

st = f["state"]
for bits, lab in ((0x3, "layer bits 0-1"), (0xC, "walk class 2-3"), (0x10000, "fix-up bit 16"),
                  (0x1700, "contact 8-10,12"), (0x10, "selected")):
    a = np.asarray(st) & bits
    print(f"  state {lab:<16} changed {rate('state', a):>10,}")

x = np.asarray(f["x"]).astype(np.int64)
z = np.asarray(f["z"]).astype(np.int64)
y = np.asarray(f["alt"]).astype(np.int64)
dx = (x[1:] - x[:-1]); dz = (z[1:] - z[:-1]); dy = (y[1:] - y[:-1])
mv = both & ((dx != 0) | (dz != 0))
print(f"\nhorizontal moves: {mv.sum():,} unit-ticks ({100 * mv.sum() / n:.1f} %)")
step = np.sqrt((dx[mv] / 65536.0) ** 2 + (dz[mv] / 65536.0) ** 2)
print("  step length wu  p10 %.3f p50 %.3f p90 %.3f p99 %.3f max %.1f" %
      tuple(np.percentile(step, [10, 50, 90, 99]).tolist() + [step.max()]))
fr = (x[L] & 0xFFFF) != 0
print(f"  x fraction nonzero in {100 * fr.mean():.1f} % of live unit-ticks")
# position change against velocity: x[t] - x[t-1] == vx[t] ?
vx = np.asarray(f["vx"]).astype(np.int64)[1:]
vz = np.asarray(f["vz"]).astype(np.int64)[1:]
vy = np.asarray(f["vy"]).astype(np.int64)[1:]
eqx = (dx == vx) & mv
eqz = (dz == vz) & mv
print(f"  moves with dx == vx(t): {eqx.sum():,} ({100 * eqx.sum() / mv.sum():.1f} %), "
      f"dz == vz(t): {100 * eqz.sum() / mv.sum():.1f} %")
ym = both & (dy != 0)
eqy = (dy == vy) & ym
print(f"  altitude changes {ym.sum():,}; dy == vy(t) in {100 * eqy.sum() / max(1, ym.sum()):.1f} %")
# moved stamp == GameTime when moved
ticks = np.load(d / "ticks.npy")
moved = np.asarray(f["moved"]).astype(np.int64)[1:]
lay = (np.asarray(f["layer"])[1:] & 3) != (np.asarray(f["layer"])[:-1] & 3)
mvany = both & ((dx != 0) | (dz != 0) | (dy != 0))
stamp_now = moved == ticks[1:, None]
print(f"  moved == GameTime: {(stamp_now & both).sum():,}; of those with x/z/alt change "
      f"{(stamp_now & mvany).sum():,}; x/z/alt change without the stamp {(mvany & ~stamp_now).sum():,}")
# speed vs |v|
spd = np.asarray(f["spd"]).astype(np.int64)[1:]
hv = np.sqrt((vx / 65536.0) ** 2 + (vz / 65536.0) ** 2)
sel = mv & (spd > 0)
print(f"  speed/65536 vs |vxz|: median ratio {np.median((spd[sel] / 65536.0) / np.maximum(hv[sel], 1e-9)):.4f}")
# heading
hd = np.asarray(f["hd"]).astype(np.int64)
dh = ((hd[1:] - hd[:-1] + 32768) % 65536) - 32768
hc = both & (dh != 0)
ah = np.abs(dh[hc]) * 360 / 65536
print(f"\nheading changes {hc.sum():,}: |step| deg p50 {np.percentile(ah, 50):.2f} p90 "
      f"{np.percentile(ah, 90):.2f} p99 {np.percentile(ah, 99):.2f} max {ah.max():.1f}")
for nm in ("tx", "tz"):
    a = np.asarray(f[nm]).astype(np.int64)
    da = ((a[1:] - a[:-1] + 32768) % 65536) - 32768
    c = both & (da != 0)
    if c.sum():
        aa = np.abs(da[c]) * 360 / 65536
        print(f"  {nm} changes {c.sum():,}: |step| deg p50 {np.percentile(aa, 50):.2f} p99 "
              f"{np.percentile(aa, 99):.2f}; value |deg| p99 "
              f"{np.percentile(np.abs(((a[L] + 32768) % 65536) - 32768) * 360 / 65536, 99):.2f}")
# altitude: hovering / flying share
lay_now = np.asarray(f["layer"]) & 3
for l in range(4):
    print(f"  mover layer {l}: {(L & (lay_now == l)).sum():,} live unit-ticks")
print("  units by type (live at tick 0):", np.bincount(np.asarray(f["type"])[0][L[0]]).nonzero()[0].tolist())
# build fraction and hp
b = np.asarray(f["build"])
bc = both & (b[1:] != b[:-1])
print(f"\nbuild fraction changes {bc.sum():,}; hp changes {rate('hp'):,}")
