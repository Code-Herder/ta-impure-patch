import numpy as np
L = lambda k: np.load(f"series/{k}.npy", mmap_mode="r")
live = np.asarray(L("live")); ok = live[1:] & live[:-1] & (np.asarray(L("mover"))[1:] != 0)
g = lambda k: np.asarray(L(k)).astype(np.int64)
x, z, vx, vz, ml = g("x"), g("z"), g("vx"), g("vz"), g("mlayer")
fpx, fpz = g("fpx"), g("fpz")
# footprint w/h at +0x7E/+0x80: read from the image via units.npz? use series of S: not stored; recompute
import sim
z_ = sim.load("units.npz")
NT = len(x)
fpw = np.zeros_like(x); fph = np.zeros_like(x)
for i, tk in enumerate(sim.ticks(z_)):
    fpw[i] = sim.field(tk.S, 0x7E, 2); fph[i] = sim.field(tk.S, 0x80, 2)
blk = (ml[1:] & 4) != 0
def clamp(c, cell):
    hi = cell + 0x7FFFF; lo = cell - 0x7FFFF
    return np.where(c > hi, hi, np.where(c < lo, lo, c))
for tn, (FX, FW, FZ, FH) in (("fp(t-1)", (fpx[:-1], fpw[:-1], fpz[:-1], fph[:-1])), ("fp(t)", (fpx[1:], fpw[1:], fpz[1:], fph[1:]))):
    cx = (FW + 2 * FX) << 19; cz = (FH + 2 * FZ) << 19
    for vn, (VX, VZ) in (("v(t)", (vx[1:], vz[1:])), ("v(t-1)", (vx[:-1], vz[:-1]))):
        px = np.where(blk, clamp(x[:-1] + VX, cx), x[:-1] + vx[1:])
        pz = np.where(blk, clamp(z[:-1] + VZ, cz), z[:-1] + vz[1:])
        print(tn, vn, "x misses", (ok & (px != x[1:])).sum(), "z misses", (ok & (pz != z[1:])).sum(),
              " blocked-only x misses", (ok & blk & (px != x[1:])).sum())
