import sys
import numpy as np
from airpairs import Pairs
P = Pairs(sys.argv[1])
c, p = P.c, P.p
air = (c["layer"] & 3) == 2
nz = c["roll"] != 0
print("pairs", P.n, " roll!=0:", nz.sum(), " airborne & roll!=0:", (nz & air).sum(), " landed & roll!=0:", (nz & ~air).sum())
print("roll changed (pairs):", (c["roll"] != p["roll"]).sum(), " airborne:", ((c["roll"] != p["roll"]) & air).sum())
print("layer values", np.unique(c["layer"] & 3, return_counts=True))
ch = (c["roll"] != p["roll"])
print("roll changes by (prev layer, cur layer):", {(a, b): int(((p['layer'] & 3 == a) & (c['layer'] & 3 == b) & ch).sum()) for a in range(4) for b in range(4) if ((p['layer'] & 3 == a) & (c['layer'] & 3 == b) & ch).any()})
print("roll values (nonzero)", np.unique(c["roll"][nz], return_counts=True)[0][:20])
# landings: A_s change on the landing tick
land = ((p["layer"] & 3) == 2) & ~air
tko = ((p["layer"] & 3) != 2) & air
print("landings", land.sum(), " takeoffs", tko.sum())
dec = lambda a: (a * 0xF333) >> 16
for k in ("ax", "ay", "az"):
    same = c[k][land] == p[k][land]
    dd = c[k][land] == dec(p[k][land])
    print(f" landing {k}: unchanged {same.sum()} decayed-once {dd.sum()} (both {(same & dd).sum()})  |prev| max {np.abs(p[k][land]).max()}")
print(" landing bank unchanged", (c['bank'][land] == p['bank'][land]).sum(), "of", land.sum())
print(" landing v zero", ((c['vx'][land] == 0) & (c['vz'][land] == 0) & (c['vy'][land] == 0)).sum(), " speed zero", (c['sp'][land] == 0).sum())
print(" prev speed at landing (wu/tick) max", (p['sp'][land] / 65536).max())
