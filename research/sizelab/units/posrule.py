import numpy as np
from sintab import T
L = lambda k: np.load(f"series/{k}.npy", mmap_mode="r")
live = np.asarray(L("live")); ok = live[1:] & live[:-1] & (np.asarray(L("mover"))[1:] != 0)
ok2 = live[2:] & live[1:-1] & live[:-2] & (np.asarray(L("mover"))[2:] != 0)
h = np.asarray(L("heading")).astype(np.int64); sp = np.asarray(L("speed")).astype(np.int64)
vx = np.asarray(L("vx")).astype(np.int64); vz = np.asarray(L("vz")).astype(np.int64)
T64 = T.astype(np.int64)
sinv = lambda h, s: (T64[((h + 0x20) >> 7) & 511] * s + 0x1000) >> 13
cosv = lambda h, s: (T64[((h + 0x4020) >> 7) & 511] * s + 0x1000) >> 13
ex = ok & (vx[1:] == -sinv(h[1:], sp[1:])) & (vz[1:] == -cosv(h[1:], sp[1:]))
print("velocity rule exact on all live mover unit-ticks:", ex.sum(), "of", ok.sum())
bad = ok & ~ex
print("  misses: speed", np.unique(sp[1:][bad])[:10], "vx", np.unique(vx[1:][bad])[:10], "count", bad.sum())
ml = np.asarray(L("mlayer")).astype(np.int64)
blk = (ml[1:] & 4) != 0
x = np.asarray(L("x")).astype(np.int64); z = np.asarray(L("z")).astype(np.int64)
dx = x[1:] - x[:-1]; dz = z[1:] - z[:-1]
print("blocked bit set:", (ok & blk).sum(), " of which x unchanged:", (ok & blk & (dx == 0)).sum(), " x == +v:", (ok & blk & (dx == vx[1:])).sum())
print("not blocked:", (ok & ~blk).sum(), " x == +v:", (ok & ~blk & (dx == vx[1:])).sum(), " z == +v:", (ok & ~blk & (dz == vz[1:])).sum())
nb = ok & ~blk & ((dx != vx[1:]) | (dz != vz[1:]))
print("not blocked & misses:", nb.sum(), " dx-v sample:", (dx - vx[1:])[nb][:20])
# rule: blocked -> prev else +v
rx = np.where(blk, dx, dx - vx[1:]); rz = np.where(blk, dz, dz - vz[1:])
print("rule (blocked->prev else +v): x misses", (ok & (rx != 0)).sum(), " z misses", (ok & (rz != 0)).sum())
# lin
lx = x[2:] - 2 * x[1:-1] + x[:-2]
print("lin x misses", (ok2 & (lx != 0)).sum())
# blocked: by how much does x move relative to clamp? show sample of dx for blocked moving
bm = ok & blk & (dx != 0)
print("blocked & moved dx sample:", dx[bm][:20], " v:", vx[1:][bm][:20])
