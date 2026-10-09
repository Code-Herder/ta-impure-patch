"""bankcheck.py -- the bank rule four ways against the engine on every airborne iteration of the air
war: numpy libm (what frame3 ran), detmath (the format's function), int64 (intbank), and libm with
the results perturbed (precision needed)."""
import sys
import numpy as np
sys.path.insert(0, sys.argv[2])
from airpairs import Pairs
import frame3 as f3, airdefs as D, detmath as dm
from intbank import bank_int

P = Pairs(sys.argv[1])
c = P.c
m = (c["layer"] & 3) == 2
tt = f3.TypeTable(D.DEF, D.GRAVITY, D.SEA)
ax, az, h, typ = c["ax"][m], c["az"][m], c["h"][m], P.typ[m]
sc = tt.bank[typ]
eng = c["bank"][m] & 0xFFFF
lib = f3.bank_angle(ax, az, h, sc, tt.q) & 0xFFFF


def det(ax, az, h, scale, q, eps=0.0):
    hs = f3.s16(h).astype(np.float64) * f3.TH_ROT
    s, co = dm.sincos(hs)
    s, co = s * (1 + eps), co * (1 - eps)
    xr = np.rint(ax.astype(np.float64) * co - az.astype(np.float64) * s).astype(np.int64)
    xr = np.where(h == 0, ax, xr)
    val = f3.s32((-xr * scale) >> 16)
    return np.rint(dm.atan2(val.astype(np.float64), float(q)) * (1 + eps) * f3.K_BANK).astype(np.int64)


d0 = det(ax, az, h, sc, tt.q) & 0xFFFF
it = bank_int(ax, az, h, sc, tt.q) & 0xFFFF
print("airborne", m.sum(), "types", np.unique(typ))
print("libm == engine", (lib == eng).sum(), " detmath == engine", (d0 == eng).sum(), " detmath == libm", (d0 == lib).sum())
print("int64 == engine", (it == eng).sum(), " int64 misses", (it != eng).sum(),
      " residuals", np.unique(((it - eng + 32768) % 65536) - 32768, return_counts=True))
for e in (2.0 ** -40, 2.0 ** -30, 2.0 ** -26, 2.0 ** -24, 2.0 ** -20):
    pe = det(ax, az, h, sc, tt.q, e) & 0xFFFF
    print(f"detmath perturbed by rel {e:.1e}: misses {(pe != eng).sum()}")
# bank values, h==0 share, scale values
print("h==0 iterations", (h == 0).sum(), " |A_s| max", np.abs(ax).max(), np.abs(az).max(), " bank range", c["bank"][m].min(), c["bank"][m].max())
print("+0x68 (roll) nonzero anywhere:", (c["roll"] != 0).sum(), "of", len(c["roll"]))
# the integer rule over the whole s16 heading domain and a grid of A_s: compare to detmath
rng = np.random.default_rng(1)
N = 2_000_000
hh = rng.integers(0, 65536, N)
aa = rng.integers(-(1 << 20), 1 << 20, N)
bb = rng.integers(-(1 << 20), 1 << 20, N)
sc2 = np.full(N, 0x10000)
x1 = det(aa, bb, hh, sc2, tt.q) & 0xFFFF
x2 = bank_int(aa, bb, hh, sc2, tt.q) & 0xFFFF
print("random 2M inputs (|A_s| < 2^20): int64 vs detmath differ", (x1 != x2).sum())
