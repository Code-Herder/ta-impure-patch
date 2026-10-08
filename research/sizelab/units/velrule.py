import numpy as np
from sintab import T
L = lambda k: np.load(f"series/{k}.npy", mmap_mode="r")
live = np.asarray(L("live")); ok = live[1:] & live[:-1] & (np.asarray(L("mover"))[1:] != 0)
h = np.asarray(L("heading")).astype(np.int64); sp = np.asarray(L("speed")).astype(np.int64)
vx = np.asarray(L("vx")).astype(np.int64); vz = np.asarray(L("vz")).astype(np.int64); vy = np.asarray(L("vy")).astype(np.int64)
T64 = T.astype(np.int64)
def sinv(h, s):
    return (T64[((h + 0x20) >> 7) & 511] * s + 0x1000) >> 13
def cosv(h, s):
    return (T64[((h + 0x4020) >> 7) & 511] * s + 0x1000) >> 13
m = ok & ((vx[1:] != 0) | (vz[1:] != 0))
print("unit-ticks moving (v != 0):", m.sum(), " vy != 0:", (ok & (vy[1:] != 0)).sum())
for hn, H in (("h(t)", h[1:]), ("h(t-1)", h[:-1])):
    for sn, S in (("s(t)", sp[1:]), ("s(t-1)", sp[:-1])):
        for name, fx, fz in (("(-sin,-cos)", lambda a, b: -sinv(a, b), lambda a, b: -cosv(a, b)),
                             ("(sin,-cos)", lambda a, b: sinv(a, b), lambda a, b: -cosv(a, b)),
                             ("(-sin,cos)", lambda a, b: -sinv(a, b), lambda a, b: cosv(a, b)),
                             ("(sin,cos)", sinv, cosv)):
            ex = (vx[1:] == fx(H, S)); ez = (vz[1:] == fz(H, S))
            print(f"{hn} {sn} {name}: vx exact {(m & ex).sum()}, vz exact {(m & ez).sum()}, both {(m & ex & ez).sum()}")
