"""knownint.py -- the air step's known part (damping and drift limit, 0x43D2F1..0x43D47D) in int64
arithmetic only: |v1| rounded to 24 significant bits (binary32's significand) exactly, then the
compare, the rescale factor and the excess as exact rationals.  Equal to frame3.known_part's
IEEE double/binary32 evaluation except where sqrt rounded twice (53 then 24 bits) lands on a tie."""
import numpy as np
from frame3 import s32, mul_asr16, clip, isqrt64, sinmul, cosmul


def bitlen(a):
    a = np.asarray(a, np.int64)
    out = np.zeros(a.shape, np.int64)
    for k in range(62, -1, -1):
        out = np.where((out == 0) & (a >> k > 0), k + 1, out)
    return out


def round24_sqrt(S):
    """(mant, s): sqrt(S) rounded to nearest-even at 24 significant bits = mant * 2**s."""
    S = np.asarray(S, np.int64)
    b = bitlen(isqrt64(S))
    k = np.maximum(26 - b, 0)                       # scale so r has 26..27 bits
    T = S << (2 * k)
    r = isqrt64(T)
    exact = r * r == T
    n = bitlen(r)
    sh = np.maximum(n - 24, 0)
    mant = r >> sh
    rem = r - (mant << sh)
    half = np.where(sh > 0, np.left_shift(1, np.maximum(sh - 1, 0)), 0)
    up = (sh > 0) & ((rem > half) | ((rem == half) & (~exact | ((mant & 1) == 1))))
    mant = mant + up
    ov = mant >> 24 > 0                              # carried into a 25th bit
    mant = np.where(ov, mant >> 1, mant)
    s = sh + ov - k
    return mant, s


def known_part_int(tt, typ, h, vx, vy, vz):
    d = 0x10000 - (tt.acc[typ] << 16) // tt.maxv[typ]
    v1x, v1y, v1z = mul_asr16(vx, d), mul_asr16(vy, d), mul_asr16(vz, d)
    v1x, v1z = clip(v1x), clip(v1z)
    mant, s = round24_sqrt(v1x * v1x + v1z * v1z)
    brake = tt.brake[typ]
    # over: mant * 2**s > brake
    over = np.where(s >= 0, (mant << np.maximum(s, 0)) > brake, mant > (brake << np.maximum(-s, 0)))
    num = brake << 16
    kf = np.where(s >= 0, num // np.maximum(mant << np.maximum(s, 0), 1), (num << np.maximum(-s, 0)) // np.maximum(mant, 1))
    e = np.where(s >= 0, (mant << np.maximum(s, 0)) - brake, (mant >> np.maximum(-s, 0)) - brake)
    kf, e = np.where(over, s32(kf), 0), np.where(over, s32(e), 0)
    v2x = np.where(over, s32(mul_asr16(v1x, kf) - sinmul(h, e)), v1x)
    v2z = np.where(over, s32(mul_asr16(v1z, kf) - cosmul(h, e)), v1z)
    return v2x, v1y, v2z
