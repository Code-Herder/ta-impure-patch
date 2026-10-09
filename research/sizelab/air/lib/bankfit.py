"""bankfit.py -- the bank angle (+0x64) from 0x43D0D0: rotate (A_s.x, A_s.z) by the heading
(0x4B7173, fsincos), scale by def +0x1A2 (bankscale), atan2 against (gravity << 16) / 0xCCD
(0x4B715A, fpatan), times 10430.37835047, rounded.  Fits the gravity, then scores per type.
"""
import sys
import numpy as np
from airpairs import Pairs, s32
from airseries import AIR_TYPES

P = Pairs(sys.argv[1])
c = P.c
L2 = (c["layer"] & 3) == 2
K = 10430.37835047          # the double at 0x509EF0
TH = 9.587379924285e-05     # the double at 0x509EF8


def rotate_x(ax, az, h):
    hs = np.where(h >= 32768, h - 65536, h).astype(np.float64)
    th = hs * TH
    cs, sn = np.cos(th), np.sin(th)
    xr = ax.astype(np.float64) * cs - az.astype(np.float64) * sn
    zr = az.astype(np.float64) * cs + ax.astype(np.float64) * sn
    xr = np.where(h == 0, ax, np.rint(xr)).astype(np.int64)
    zr = np.where(h == 0, az, np.rint(zr)).astype(np.int64)
    return xr, zr


def bank(ax, az, h, B, q, use="x"):
    xr, zr = rotate_x(ax, az, h)
    v = xr if use == "x" else zr
    val = s32((-v * B) >> 16)
    return np.rint(np.arctan2(val.astype(np.float64), float(q)) * K).astype(np.int64)


B = 0x10000
m = L2
best = []
for G in list(range(8100, 8200)):
    q = (G << 16) // 0xCCD
    pred = bank(c["ax"][m], c["az"][m], c["h"][m], B, q)
    ok = ((pred - c["bank"][m]) % 65536 == 0).mean()
    best.append((ok, G, q))
best.sort(reverse=True)
print("best gravity fits:", best[:5])
G = best[0][1]
q = (G << 16) // 0xCCD
for use in ("x", "z"):
    pred = bank(c["ax"], c["az"], c["h"], B, q, use)
    d = (c["bank"] - pred) % 65536
    d = np.where(d >= 32768, d - 65536, d)
    print(f"rotated {use}: exact {int(((d == 0) & m).sum())}/{int(m.sum())}")
    if use == "x":
        for t in sorted(AIR_TYPES):
            mt = m & (P.typ == t)
            print(f"   {AIR_TYPES[t]:9s} exact {int(((d == 0) & mt).sum())}/{int(mt.sum())}  |d|<=1 {int(((abs(d) <= 1) & mt).sum())}  max|d| {int(abs(d[mt]).max())}")
        bad = m & (d != 0)
        print("misses", int(bad.sum()), "residuals", np.unique(d[bad], return_counts=True))
