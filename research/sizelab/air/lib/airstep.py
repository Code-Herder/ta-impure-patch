"""airstep.py -- the known part of the air step 0x43D2C0 (damping, then the drift limit), and the
PD remainder it leaves: A_int = v(t) - v2.  Scored on air.npz.
"""
import sys
import numpy as np
from airpairs import Pairs, s32, asr16_mul
from airseries import AIR_TYPES
import airdefs as D


def damp_factor(typ):
    acc, maxv = D.per_type(typ, "acc"), D.per_type(typ, "maxv")
    return 0x10000 - (acc << 16) // np.maximum(maxv, 1)


def known_part(typ, h_prev, vx, vy, vz):
    """v2 = the velocity after the damping (0x43D2F1..0x43D38B) and the drift limit
    (0x43D38E..0x43D47D), before the vertical clamp and the PD term.  Returns v2x, v2y, v2z and
    whether the limit fired."""
    d = damp_factor(typ)
    v1x, v1y, v1z = asr16_mul(vx, d), asr16_mul(vy, d), asr16_mul(vz, d)
    m32 = (np.hypot(v1x.astype(np.float64), v1z.astype(np.float64)) * (1.0 / 65536)).astype(np.float32)
    cap32 = (D.per_type(typ, "brake").astype(np.float64) * (1.0 / 65536)).astype(np.float32)
    over = ~(m32 <= cap32)
    m64, c64 = m32.astype(np.float64), cap32.astype(np.float64)
    with np.errstate(divide="ignore", invalid="ignore"):
        k = np.trunc(np.where(over, c64 / np.where(m64 == 0, 1, m64), 0) * 65536.0).astype(np.int64)
    k = s32(k)
    e = np.trunc((m64 - c64) * 65536.0).astype(np.int64)
    e = np.where(over, s32(e), 0)
    v2x = np.where(over, s32(asr16_mul(v1x, k) - D.sinmul(h_prev, e)), v1x)
    v2z = np.where(over, s32(asr16_mul(v1z, k) - D.cosmul(h_prev, e)), v1z)
    return v2x, v1y, v2z, over


if __name__ == "__main__":
    P = Pairs(sys.argv[1])
    c, p = P.c, P.p
    m = ((c["layer"] & 3) == 2) & ((p["layer"] & 3) == 2)
    v2x, v2y, v2z, over = known_part(P.typ, p["h"], p["vx"], p["vy"], p["vz"])
    Ax, Az = c["vx"] - v2x, c["vz"] - v2z
    acc = D.per_type(P.typ, "acc")
    mag = np.hypot(Ax, Az)
    print("drift limit fired", int((over & m).sum()), "of", int(m.sum()))
    for t in [None] + sorted(AIR_TYPES):
        mt = m if t is None else m & (P.typ == t)
        r = mag[mt] / acc[mt]
        print(f"{'all' if t is None else AIR_TYPES[t]:9s} n {int(mt.sum()):7d}  |A|/acc: max {r.max():.4f}  "
              f">1.0001 {int((r > 1.0001).sum())}  saturated(>0.999) {(r > 0.999).mean():.3f}  "
              f"zero {(mag[mt] == 0).mean():.3f}  over {over[mt].mean():.3f}")
