/* tasim_fixed.c -- the engine's arithmetic the shared simulation core runs; the contract and each helper's engine
   evidence are tasim_fixed.h's.

   The transcendentals follow fdlibm's algorithms and coefficients:
     Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
     Developed at SunPro, a Sun Microsystems, Inc. business.
     Permission to use, copy, modify, and distribute this software is freely granted, provided that this notice
     is preserved.
   Each coefficient is written as its exact hexadecimal double, the bits fdlibm gives beside its decimal. */
#include <math.h>
#include <string.h>
#include "tasim_fixed.h"

static inline uint32_t hi_word(double x)
{
    uint64_t b;
    memcpy(&b, &x, sizeof b);
    return (uint32_t)(b >> 32);
}

static inline uint32_t lo_word(double x)
{
    uint64_t b;
    memcpy(&b, &x, sizeof b);
    return (uint32_t)b;
}

static inline double with_hi(uint32_t hi, uint32_t lo)
{
    const uint64_t b = (uint64_t)hi << 32 | lo;
    double x;
    memcpy(&x, &b, sizeof x);
    return x;
}

/* ---- the conversions ---- */

/* rint under round-to-nearest-even without libm's rint: below 2^52 adding 2^52 leaves a unit in the last place of
   1, so the sum rounds x to an integer exactly as the FPU's nearest mode does, ties to even */
static double round_even(double x)
{
    const double two52 = 4503599627370496.0;
    const double a = fabs(x);
    if (!(a < two52))
        return x;
    return copysign((a + two52) - two52, x);
}

int32_t ts_fistp(double x)
{
    const double r = round_even(x);
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return INT32_MIN;
    return (int32_t)r;
}

int32_t ts_ftol(double x)
{
    if (!(x > -9223372036854775808.0 && x < 9223372036854775808.0))
        return 0;
    return (int32_t)(uint32_t)(uint64_t)(int64_t)x;
}

/* ---- _hypot, emulated (tasim_fixed.h) ----
   Every intermediate is an exact integer times a power of two; a 128-bit integer (two uint64 halves, products
   built from 32-bit pieces, since the i686 build has no 128-bit type) holds each exact value before it is rounded.
   Scaling by a power of two changes no rounding (no value here comes near the subnormals), so each step keeps its
   integer and the exponents are added where the result is built. */

typedef struct { uint64_t hi, lo; } U128;

static U128 mul64(uint64_t a, uint64_t b)
{
    const uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    U128 r;
    r.lo = (mid << 32) | (uint32_t)p00;
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

static U128 shl128(uint64_t v, int k)       /* v << k, 0 <= k < 128, v's bits not past bit 127 */
{
    U128 r;
    if (k == 0) {
        r.hi = 0;
        r.lo = v;
    } else if (k < 64) {
        r.hi = v >> (64 - k);
        r.lo = v << k;
    } else {
        r.hi = v << (k - 64);
        r.lo = 0;
    }
    return r;
}

static int cmp128(U128 a, U128 b)
{
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    return a.lo < b.lo ? -1 : a.lo > b.lo;
}

static U128 sub128(U128 a, U128 b)          /* a - b, a >= b */
{
    U128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}

static int bitlen64(uint64_t v)
{
    int n = 0;
    while (v) {
        ++n;
        v >>= 1;
    }
    return n;
}

/* v (and, with `sticky`, something more below its last bit) rounded to p <= 64 bits, nearest-even: the mantissa,
   and the shift it was taken at (v ~ mant << shift). Each caller gives v more than p + 1 bits, so a sticky tail is
   never dropped unseen. */
static uint64_t round_bits(U128 v, int sticky, int p, int* shift)
{
    const int len = v.hi ? 64 + bitlen64(v.hi) : bitlen64(v.lo);
    if (len <= p) {
        *shift = 0;
        return v.lo;
    }
    const int sh = len - p;
    uint64_t mant, guard, rest;
    if (sh >= 64) {
        mant = v.hi >> (sh - 64);
        guard = sh == 64 ? v.lo >> 63 : (v.hi >> (sh - 65)) & 1;
        rest = sh == 64 ? (v.lo << 1) : ((sh - 65 ? v.hi << (128 - sh + 1) : 0) | v.lo);
    } else {
        mant = (v.lo >> sh) | (sh ? v.hi << (64 - sh) : 0);
        guard = (v.lo >> (sh - 1)) & 1;
        rest = sh > 1 ? v.lo << (65 - sh) : 0;
    }
    *shift = sh;
    if (guard && (rest || sticky || (mant & 1))) {
        ++mant;
        if (p == 64 ? mant == 0 : mant >> p) {
            mant = (uint64_t)1 << (p - 1);
            ++*shift;
        }
    }
    return mant;
}

/* rd53(rd64(v)): an x87 result at the 64-bit precision, then its fstp to a double */
static uint64_t round_store(U128 v, int sticky, int* shift)
{
    int s64, s53;
    const uint64_t m64 = round_bits(v, sticky, 64, &s64);
    U128 w = { 0, m64 };
    const uint64_t m53 = round_bits(w, 0, 53, &s53);
    *shift = s64 + s53;
    return m53;
}

static double pow2(int k)                   /* 2^k, -1022 <= k <= 1023 */
{
    const uint64_t b = (uint64_t)(k + 1023) << 52;
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}

double ts_hypot(int32_t x, int32_t y)
{
    const uint64_t ax = x < 0 ? (uint64_t)-(int64_t)x : (uint64_t)x;
    const uint64_t ay = y < 0 ? (uint64_t)-(int64_t)y : (uint64_t)y;
    const uint64_t M = ay > ax ? ay : ax, n = ay > ax ? ax : ay;
    if (M == 0)
        return 0.0;
    /* the smaller ratio r = n / M = R 2^er, R of 53 bits; the larger is exactly 1. n << 32 < 2^63 and each
       remainder < M <= 2^31, so three 32-bit quotient digits give floor(n 2^96 / M), at least 65 bits since
       n / M >= 2^-31 */
    uint64_t S;                             /* s = S 2^f, s in [1, 2] */
    int f;
    if (n == 0 || n == M) {
        S = (uint64_t)1 << 52;
        f = n == 0 ? -52 : -51;
    } else {
        const uint64_t q1 = (n << 32) / M, r1 = (n << 32) % M;
        const uint64_t q2 = (r1 << 32) / M, r2 = (r1 << 32) % M;
        const uint64_t q3 = (r2 << 32) / M, r3 = (r2 << 32) % M;
        U128 Q = { q1, (q2 << 32) | q3 };
        int sr;
        const uint64_t R = round_store(Q, r3 != 0, &sr);
        const int er = sr - 96;
        /* r^2 rounded to 64 bits: m2 2^e with m2 >= 2^63 and r^2 < 1, so e <= -64 */
        int s2;
        const uint64_t m2 = round_bits(mul64(R, R), 0, 64, &s2);
        const int k = -(s2 + 2 * er);
        if (k >= 128) {
            /* r^2 < 2^-64, under half a 64-bit unit of 1: the sum rounds to 1 */
            S = (uint64_t)1 << 52;
            f = -52;
        } else {
            U128 sum = shl128(1, k);
            sum.lo += m2;
            sum.hi += sum.lo < m2;
            int ss;
            S = round_store(sum, 0, &ss);
            f = ss - k;
        }
        while (S < (uint64_t)1 << 52) {     /* a mantissa of fewer bits than 53 when the sum was exact */
            S <<= 1;
            --f;
        }
    }
    /* q = rd53(rd64(sqrt(s))). c = rd53(sqrt(s)) is IEEE sqrt's, and the two differ only where rd64 lands on the
       53-bit midpoint mid next to c, on sqrt(s)'s side: then the store rounds that tie to even. sqrt(s) is never
       exactly half a 64-bit unit from mid, nor exactly mid (either would make s a square of a 65- or 54-bit
       number, wider than its 53 bits), so the test is strict both ways. With c = C 2^-52 (s in [1, 2], so c in
       [1, 2)) and T = mid 2^63 = (2C + side) 2^10: rd64 gives mid iff |sqrt(s) 2^63 - T| < 1/2, which over the
       integer W = s 2^126 is -T < W - T^2 <= T. */
    const double s = (double)S * pow2(f);
    const double c = sqrt(s);
    uint64_t C = (uint64_t)(c * 4503599627370496.0);
    const U128 s104 = shl128(S, f + 104), c2 = mul64(C, C);
    const int side = cmp128(s104, c2);
    if (side != 0) {
        const uint64_t T = ((C << 1) + (uint64_t)(int64_t)side) << 10;
        const U128 W = shl128(S, f + 126), T2 = mul64(T, T);
        int at_mid;
        if (cmp128(W, T2) >= 0) {
            const U128 d = sub128(W, T2);
            at_mid = d.hi == 0 && d.lo <= T;
        } else {
            const U128 d = sub128(T2, W);
            at_mid = d.hi == 0 && d.lo < T;
        }
        if (at_mid && (C & 1))
            C += (uint64_t)(int64_t)side;
    }
    /* r = rd53(rd64(mant(q) mant(M))) 2^(exp(q) + exp(M)) = rd53(rd64(C M)) 2^-52 */
    int sp;
    const uint64_t P = round_store(mul64(C, M), 0, &sp);
    return (double)P * pow2(sp - 52);
}

/* ---- sin and cos (k_sin.c, k_cos.c, e_rem_pio2.c) ---- */

static const double
    S1 = -0x1.5555555555549p-3, S2 = 0x1.111111110f8a6p-7, S3 = -0x1.a01a019c161d5p-13,
    S4 = 0x1.71de357b1fe7dp-19, S5 = -0x1.ae5e68a2b9cebp-26, S6 = 0x1.5d93a5acfd57cp-33,
    C1 = 0x1.555555555554cp-5, C2 = -0x1.6c16c16c15177p-10, C3 = 0x1.a01a019cb1590p-16,
    C4 = -0x1.27e4f809c52adp-22, C5 = 0x1.1ee9ebdb4b1c4p-29, C6 = -0x1.8fae9be8838d4p-37;

/* sin(x + y) for |x + y| <= pi/4, y the tail of x; `tail` 0 when x was not reduced and y is 0, which fdlibm
   evaluates in a shorter order */
static double k_sin(double x, double y, int tail)
{
    if ((hi_word(x) & 0x7fffffff) < 0x3e400000 && (int)x == 0)
        return x;
    const double z = x * x, v = z * x;
    const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (!tail)
        return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

/* cos(x + y) for |x + y| <= pi/4 */
static double k_cos(double x, double y)
{
    const uint32_t ix = hi_word(x) & 0x7fffffff;
    if (ix < 0x3e400000 && (int)x == 0)
        return 1.0;
    const double z = x * x;
    const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3fd33333)
        return 1.0 - (0.5 * z - (z * r - x * y));
    const double qx = ix > 0x3fe90000 ? 0.28125 : with_hi(ix - 0x00200000, 0);
    const double hz = 0.5 * z - qx, a = 1.0 - qx;
    return a - (hz - (z * r - x * y));
}

static const double
    invpio2 = 0x1.45f306dc9c883p-1,
    pio2_1 = 0x1.921fb54400000p+0, pio2_1t = 0x1.0b4611a626331p-34,
    pio2_2 = 0x1.0b4611a600000p-34, pio2_2t = 0x1.3198a2e037073p-69,
    pio2_3 = 0x1.3198a2e000000p-69, pio2_3t = 0x1.b839a252049c1p-104;

/* x = n pi/2 + y[0] + y[1], |y[0] + y[1]| <= pi/4; n, or a value with bit 30 set past the medium range.
   fdlibm's medium path skips the second and third rounds when x's high word is far from a multiple of pi/2 (its
   npio2_hw table); this always tests the cancellation instead, which takes the same branch wherever the shortcut
   applies, since a high word far from n pi/2 cancels fewer than 16 bits. */
#define REM_OUT_OF_RANGE 0x40000000
static int rem_pio2(double x, double* y)
{
    const uint32_t hx = hi_word(x), ix = hx & 0x7fffffff;
    const int neg = (hx >> 31) != 0;
    if (ix <= 0x3fe921fb) {
        y[0] = x;
        y[1] = 0;
        return 0;
    }
    if (ix < 0x4002d97c) {
        double z;
        if (!neg) {
            z = x - pio2_1;
            if (ix != 0x3ff921fb) {
                y[0] = z - pio2_1t;
                y[1] = (z - y[0]) - pio2_1t;
            } else {
                z -= pio2_2;
                y[0] = z - pio2_2t;
                y[1] = (z - y[0]) - pio2_2t;
            }
            return 1;
        }
        z = x + pio2_1;
        if (ix != 0x3ff921fb) {
            y[0] = z + pio2_1t;
            y[1] = (z - y[0]) + pio2_1t;
        } else {
            z += pio2_2;
            y[0] = z + pio2_2t;
            y[1] = (z - y[0]) + pio2_2t;
        }
        return -1;
    }
    if (ix > 0x413921fb)
        return REM_OUT_OF_RANGE;
    double t = fabs(x);
    const int n = (int)(t * invpio2 + 0.5);
    const double fn = (double)n;
    double r = t - fn * pio2_1, w = fn * pio2_1t;
    const int j = (int)(ix >> 20);
    y[0] = r - w;
    if (j - (int)((hi_word(y[0]) >> 20) & 0x7ff) > 16) {
        t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y[0] = r - w;
        if (j - (int)((hi_word(y[0]) >> 20) & 0x7ff) > 49) {
            t = r;
            w = fn * pio2_3;
            r = t - w;
            w = fn * pio2_3t - ((t - r) - w);
            y[0] = r - w;
        }
    }
    y[1] = (r - y[0]) - w;
    if (neg) {
        y[0] = -y[0];
        y[1] = -y[1];
        return -n;
    }
    return n;
}

double ts_sin(double x)
{
    if ((hi_word(x) & 0x7fffffff) >= 0x7ff00000)
        return x - x;
    double y[2];
    const int n = rem_pio2(x, y);
    if (n == REM_OUT_OF_RANGE)
        return NAN;
    const int tail = n != 0;
    switch (n & 3) {
    case 0: return k_sin(y[0], y[1], tail);
    case 1: return k_cos(y[0], y[1]);
    case 2: return -k_sin(y[0], y[1], tail);
    default: return -k_cos(y[0], y[1]);
    }
}

double ts_cos(double x)
{
    if ((hi_word(x) & 0x7fffffff) >= 0x7ff00000)
        return x - x;
    double y[2];
    const int n = rem_pio2(x, y);
    if (n == REM_OUT_OF_RANGE)
        return NAN;
    switch (n & 3) {
    case 0: return k_cos(y[0], y[1]);
    case 1: return -k_sin(y[0], y[1], 1);
    case 2: return -k_cos(y[0], y[1]);
    default: return k_sin(y[0], y[1], 1);
    }
}

/* ---- atan and atan2 (s_atan.c, e_atan2.c) ---- */

static const double atanhi[4] = { 0x1.dac670561bb4fp-2, 0x1.921fb54442d18p-1, 0x1.f730bd281f69bp-1,
                                  0x1.921fb54442d18p+0 };
static const double atanlo[4] = { 0x1.a2b7f222f65e2p-56, 0x1.1a62633145c07p-55, 0x1.007887af0cbbdp-56,
                                  0x1.1a62633145c07p-54 };
static const double aT[11] = {
    0x1.555555555550dp-2, -0x1.999999998ebc4p-3, 0x1.24924920083ffp-3, -0x1.c71c6fe231671p-4,
    0x1.745cdc54c206ep-4, -0x1.3b0f2af749a6dp-4, 0x1.10d66a0d03d51p-4, -0x1.dde2d52defd9ap-5,
    0x1.97b4b24760debp-5, -0x1.2b4442c6a6c2fp-5, 0x1.0ad3ae322da11p-6,
};

static double ts_atan(double x)
{
    const uint32_t hx = hi_word(x), ix = hx & 0x7fffffff;
    int id;
    if (ix >= 0x44100000) {
        if (ix > 0x7ff00000 || (ix == 0x7ff00000 && lo_word(x) != 0))
            return x + x;
        return (hx >> 31) ? -atanhi[3] - atanlo[3] : atanhi[3] + atanlo[3];
    }
    if (ix < 0x3fdc0000) {
        if (ix < 0x3e200000)
            return x;
        id = -1;
    } else {
        x = fabs(x);
        if (ix < 0x3ff30000) {
            if (ix < 0x3fe60000) {
                id = 0;
                x = (2.0 * x - 1.0) / (2.0 + x);
            } else {
                id = 1;
                x = (x - 1.0) / (x + 1.0);
            }
        } else if (ix < 0x40038000) {
            id = 2;
            x = (x - 1.5) / (1.0 + 1.5 * x);
        } else {
            id = 3;
            x = -1.0 / x;
        }
    }
    const double z = x * x, w = z * z;
    const double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    const double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0)
        return x - x * (s1 + s2);
    const double r = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return (hx >> 31) ? -r : r;
}

static const double pi_o_4 = 0x1.921fb54442d18p-1, pi_o_2 = 0x1.921fb54442d18p+0, pi = 0x1.921fb54442d18p+1,
                    pi_lo = 0x1.1a62633145c07p-53;

double ts_atan2(double y, double x)
{
    const uint32_t hx = hi_word(x), lx = lo_word(x), ix = hx & 0x7fffffff;
    const uint32_t hy = hi_word(y), ly = lo_word(y), iy = hy & 0x7fffffff;
    if ((ix | ((lx | (0u - lx)) >> 31)) > 0x7ff00000 || (iy | ((ly | (0u - ly)) >> 31)) > 0x7ff00000)
        return x + y;
    if (hx == 0x3ff00000 && lx == 0)
        return ts_atan(y);
    const int m = (int)(((hy >> 31) & 1) | ((hx >> 30) & 2));
    if ((iy | ly) == 0) {
        switch (m) {
        case 0:
        case 1: return y;
        case 2: return pi;
        default: return -pi;
        }
    }
    if ((ix | lx) == 0)
        return (hy >> 31) ? -pi_o_2 : pi_o_2;
    if (ix == 0x7ff00000) {
        if (iy == 0x7ff00000) {
            switch (m) {
            case 0: return pi_o_4;
            case 1: return -pi_o_4;
            case 2: return 3.0 * pi_o_4;
            default: return -3.0 * pi_o_4;
            }
        }
        switch (m) {
        case 0: return 0.0;
        case 1: return -0.0;
        case 2: return pi;
        default: return -pi;
        }
    }
    if (iy == 0x7ff00000)
        return (hy >> 31) ? -pi_o_2 : pi_o_2;
    const int k = ((int)iy - (int)ix) >> 20;
    double z;
    if (k > 60)
        z = pi_o_2 + 0.5 * pi_lo;
    else if ((hx >> 31) && k < -60)
        z = 0.0;
    else
        z = ts_atan(fabs(y / x));
    switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
    }
}

/* ---- the engine's helpers ---- */

void ts_sin_table(TS_SIN* t)
{
    for (int i = 0; i < TS_SIN_N; ++i)
        t->v[i] = (int16_t)floor(8192.0 * ts_sin(2.0 * pi * i / TS_SIN_N) + 0.5);
}

/* the double at 0x509EF0 (tasim_fixed.h, ts_angle) */
static const double ANGLE_SCALE = 10430.37835047;
/* the double at 0x509EF8 (tasim_fixed.h, ts_rotate) */
static const double ROTATE_SCALE = 9.587379924285e-05;

int32_t ts_angle(int32_t dx, int32_t dz)
{
    return ts_fistp(ts_atan2((double)dx, (double)dz) * ANGLE_SCALE);
}

void ts_rotate(int16_t a, int32_t* x, int32_t* z)
{
    if (a == 0)
        return;
    const double theta = (double)a * ROTATE_SCALE;
    const double c = ts_cos(theta), s = ts_sin(theta);
    const double fx = (double)*x, fz = (double)*z;
    const double zc = fz * c, xc = fx * c, zs = fz * s, xs = fx * s;
    *x = ts_fistp(xc - zs);
    *z = ts_fistp(zc + xs);
}
