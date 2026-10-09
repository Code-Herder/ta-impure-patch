"""detmath.py -- sin, cos and atan2 from IEEE binary64 + - * / and sqrt only, in a fixed order,
so that the 32-bit writer (x87 at precision control 53, round to nearest, set by the predictor
itself) and a numpy reader compute the same bits.  No libm.

Accuracy target: a few ulp, enough that the bank rule's two roundings to integers (0x4B7173's
fistp, 0x4B715A's fistp) agree with the engine's x87 except when the engine's exact value lies
within ~1e-12 of a half-integer.
"""
import numpy as np

# pi/2 split for Cody-Waite reduction: PIO2_HI has 33 significant bits, so n * PIO2_HI is exact for
# |n| < 2**20; PIO2_LO is the double nearest pi/2 - PIO2_HI.
PIO2_HI = float.fromhex("0x1.921fb54400000p+0")
PIO2_LO = float.fromhex("0x1.0b4611a626331p-34")
PIO2 = float.fromhex("0x1.921fb54442d18p+0")
PIO4 = float.fromhex("0x1.921fb54442d18p-1")
TWO_OVER_PI = float.fromhex("0x1.45f306dc9c883p-1")


def _poly(x, coeffs):
    """Horner, highest degree first, each step one multiply and one add."""
    acc = np.full_like(x, coeffs[0])
    for c in coeffs[1:]:
        acc = acc * x + c
    return acc


# Taylor coefficients as exact-enough doubles: sin r = r - r^3/3! + ... (|r| <= pi/4 + tiny)
_S = [((-1) ** k) / float(np.prod(np.arange(1, 2 * k + 2, dtype=np.float64))) for k in range(13, 0, -1)]
_C = [((-1) ** k) / float(np.prod(np.arange(1, 2 * k + 1, dtype=np.float64))) for k in range(13, 0, -1)]


def _sin_r(r):
    r2 = r * r
    return r + r * (r2 * _poly(r2, _S))


def _cos_r(r):
    r2 = r * r
    return 1.0 + r2 * _poly(r2, _C)


def sincos(x):
    x = np.asarray(x, np.float64)
    n = np.rint(x * TWO_OVER_PI)
    r = (x - n * PIO2_HI) - n * PIO2_LO
    q = n.astype(np.int64) & 3
    s, c = _sin_r(r), _cos_r(r)
    sin = np.select([q == 0, q == 1, q == 2, q == 3], [s, c, -s, -c])
    cos = np.select([q == 0, q == 1, q == 2, q == 3], [c, -s, -c, s])
    return sin, cos


_A = [((-1) ** k) / (2.0 * k + 1) for k in range(30, 0, -1)]


def _atan_small(t):
    t2 = t * t
    return t + t * (t2 * _poly(t2, _A))


def atan(t):
    """atan for any finite t: fold to |t| <= 1, then halve the angle twice
    (atan t = 2 atan(t / (1 + sqrt(1 + t^2)))) to |t| <= tan(pi/16), then the series."""
    t = np.asarray(t, np.float64)
    a = np.abs(t)
    big = a > 1.0
    u = np.where(big, 1.0 / np.where(big, a, 1.0), a)
    u = u / (1.0 + np.sqrt(1.0 + u * u))
    u = u / (1.0 + np.sqrt(1.0 + u * u))
    r = 4.0 * _atan_small(u)
    r = np.where(big, PIO2 - r, r)
    return np.where(t < 0, -r, r)


def atan2(y, x):
    """atan2 for x > 0 (the bank rule's x is (gravity << 16) / 0xCCD), with x <= 0 handled the
    usual way for completeness."""
    y = np.asarray(y, np.float64)
    x = np.asarray(x, np.float64)
    base = atan(y / np.where(x == 0, 1.0, x))
    pi = 2.0 * PIO2
    out = np.where(x > 0, base, np.where(x < 0, np.where(y >= 0, base + pi, base - pi),
                                         np.where(y > 0, PIO2, np.where(y < 0, -PIO2, 0.0))))
    return out
