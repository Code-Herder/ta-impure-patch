"""intbank.py -- the bank rule in int64 only (no floating point): sin/cos and atan in Q31 fixed
point by Taylor series after integer argument reduction.  Deterministic by construction in C and
numpy (floor shifts and floor division on int64, all operands bounded below 2**62)."""
import numpy as np
from math import factorial, pi

Q = 31
ONE = 1 << Q
# theta = r * 2pi/65536, r in [-8192, 8191]: x_q31 = (r * TH47) >> 16
TH47 = round(2 * pi / 65536 * (1 << 47))
SINC = [round(((-1) ** k) * ONE / factorial(2 * k + 1)) for k in range(7, -1, -1)]   # x^15 .. x^1
COSC = [round(((-1) ** k) * ONE / factorial(2 * k)) for k in range(7, -1, -1)]       # x^14 .. 1
PI4 = round(pi / 4 * ONE)
PI2 = round(pi / 2 * ONE)
ATC = [round(((-1) ** k) * ONE / (2 * k + 1)) for k in range(14, -1, -1)]           # w^29 .. w
K47 = round(65536 / (2 * pi) * (1 << 16))     # bank = angle_q31 * 65536/2pi


def mulq(a, b):
    return (a * b) >> Q


def _sin_cos_q31(h):
    h = np.asarray(h, np.int64) & 0xFFFF
    n = ((h + 0x2000) >> 14) & 3
    r = ((h + 0x2000) & 0x3FFF) - 0x2000
    x = (r * TH47) >> 16
    x2 = mulq(x, x)
    s = np.full_like(x, SINC[0])
    for c in SINC[1:]:
        s = mulq(s, x2) + c
    s = mulq(s, x)
    c_ = np.full_like(x, COSC[0])
    for c in COSC[1:]:
        c_ = mulq(c_, x2) + c
    sin = np.select([n == 0, n == 1, n == 2, n == 3], [s, c_, -s, -c_])
    cos = np.select([n == 0, n == 1, n == 2, n == 3], [c_, -s, -c_, s])
    return sin, cos


def _atan_q31(u):
    """atan(u / 2**31) for 0 <= u <= 2**31, in Q31."""
    big = u > round(0.41421356 * ONE)
    w = np.where(big, ((u - ONE) << Q) // (u + ONE), u)      # floor division, both signs defined
    w2 = mulq(w, w)
    a = np.full_like(w, ATC[0])
    for c in ATC[1:]:
        a = mulq(a, w2) + c
    a = mulq(a, w)
    return np.where(big, PI4 + a, a)


def bank_int(ax, az, h, scale, q):
    ax = np.clip(np.asarray(ax, np.int64), -(1 << 30), 1 << 30)
    az = np.clip(np.asarray(az, np.int64), -(1 << 30), 1 << 30)
    sin, cos = _sin_cos_q31(h)
    xr = (ax * cos - az * sin + (1 << (Q - 1))) >> Q
    xr = np.where((np.asarray(h, np.int64) & 0xFFFF) == 0, ax, xr)
    val = (-xr * np.asarray(scale, np.int64)) >> 16
    val = ((val + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)
    a = np.abs(val)
    q = np.int64(q)
    swap = a > q
    num, den = np.where(swap, q, a), np.where(swap, a, q)
    u = (num << Q) // np.maximum(den, 1)
    ang = _atan_q31(u)
    ang = np.where(swap, PI2 - ang, ang)
    ang = np.where(val < 0, -ang, ang)
    return (ang * K47 + (1 << 46)) >> 47
