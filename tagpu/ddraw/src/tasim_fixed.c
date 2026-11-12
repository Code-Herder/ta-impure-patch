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

double ts_hypot(double x, double y)
{
    return sqrt(x * x + y * y);
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
