/* tasim_fxgate.c -- explosion lives, the shatter's quads and a shatter body's flight bound (tasim_fxgate.h). Each
   function's engine reading is at its declaration. */
#include <math.h>
#include "tasim_fxgate.h"

uint32_t ts_fx_seq_life(const uint16_t* dur, int32_t n)
{
    uint32_t L = 0;
    if (n <= 0) return 0xFFFFu;
    for (int32_t i = 0; i < n; ++i)
        L += dur[i] ? dur[i] : 1u;
    return L;
}

uint32_t ts_fx_table_b_life(int32_t idx)
{
    if (idx < 0) return 0;
    return idx == 0 ? 24u : 30u;
}

uint32_t ts_fx_life(uint32_t life_a, int32_t idx)
{
    const uint32_t b = ts_fx_table_b_life(idx);
    const uint32_t L = life_a > b ? life_a : b;
    return L ? L : 1u;
}

int32_t ts_shatter_quads(const TS_PRIM* prims, int32_t n, int32_t sel)
{
    int32_t q = 0;
    if (n <= 0) return 0;
    if (sel < -1 || sel >= n) return -1;
    for (int32_t i = 0; i < n; ++i)
        if (prims[i].nverts == 4 && i != sel && !(prims[i].unknown3 & 1u) && !prims[i].texture_missing) ++q;
    return q;
}

/* exact integer square roots of a non-negative int64: sqrt seeds, the loops correct the double's rounding */
static int64_t isqrt_floor(int64_t x)
{
    int64_t r;
    if (x <= 0) return 0;
    r = (int64_t)sqrt((double)x);
    while (r > 0 && r * r > x) --r;
    while ((r + 1) * (r + 1) <= x) ++r;
    return r;
}

static int64_t isqrt_ceil(int64_t x)
{
    const int64_t r = isqrt_floor(x);
    return r * r < x ? r + 1 : r;
}

uint32_t ts_shatter_flight_bound(int32_t g, uint8_t sea, uint8_t hmax, int32_t unit_y, int32_t r_u, uint32_t r3,
                                 int32_t iy)
{
    /* past the premises g < 0x10000, |Y| < 2^32 and |V| < 2^22, so every product below stays under 2^55 */
    const int64_t G = g, S = (int64_t)sea << 16, vb = 0x20000 - (int64_t)g;
    int64_t vy0, ylo, dlo, P, Q, T = 0;
    if (g <= 0 || vb <= G || iy != 0 || r_u < 0 || r3 >= 160u) return TS_SHATTER_SATURATED;
    vy0 = (80 - (int64_t)r3) * 512 + 30 * G;
    ylo = (int64_t)unit_y - r_u < S ? (int64_t)unit_y - r_u : S;
    dlo = 2 * G * (S - ylo);
    P = 2 * G * (((int64_t)hmax + 1 - sea) * 65536);
    Q = 2 * G * ((int64_t)unit_y + r_u - S) + vy0 * vy0;
    for (int arc = 0; arc < TS_SHATTER_ARCS; ++arc) {
        const int64_t a = isqrt_ceil(Q + dlo);
        const int64_t n = (2 * a + G + isqrt_ceil(4 * a * G + G * G + 4 * (Q > 0 ? Q : 0))) / (2 * G) + 1;
        const int64_t qpre = Q + G * G * (n - 1);
        int64_t v;
        T += n;
        if (T >= TS_SHATTER_SATURATED) return TS_SHATTER_SATURATED;
        if (qpre + dlo < vb * vb) return (uint32_t)T;
        v = isqrt_floor(qpre - P + G * G) - G;
        if (v < vb) v = vb;
        Q = qpre - (3 * v * v - 2 * G * v - G * G) / 4;
    }
    return TS_SHATTER_SATURATED;
}
