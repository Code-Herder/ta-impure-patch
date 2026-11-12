/* tasim_fixed_test.c -- the self-test of the core's engine arithmetic (src/tasim_fixed.h), driven by
   tests/tasim_fixed_test.py, which builds it natively, under the sanitizers and with the DLL's i686 compiler under
   Wine.

     tasim_fixed_test            the rules: the sine table's symmetry and its four exact values, the conversions'
                                 ties and bounds, the 64-bit helpers, the turn lookups' known answers; exit 1 on a
                                 failure
     tasim_fixed_test libm       ts_sin, ts_cos and ts_atan2 against the platform's libm on 10^6 arguments each:
                                 every answer within one ulp; the native build's alone, glibc being the reference
     tasim_fixed_test digest     one 64-bit FNV-1a digest of 10^6 calls of each helper on arguments from a fixed
                                 generator; the driver requires every build to print the same digest
     tasim_fixed_test velocity   the ground velocity rule (0x43CC20, research/notes/engine/units.md *Ground
                                 velocity*) against "heading speed vx vz" lines on stdin, values the engine computed:
                                 vx = -turn_x(heading, speed), vz = -turn_z(heading, speed) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_fixed.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                                       printf("\n"); } } while (0)

/* a fixed generator, so every build draws the same arguments (xorshift64*) */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
}

static uint64_t fnv = 0xCBF29CE484222325ull;
static void feed(const void* p, size_t n)
{
    const unsigned char* b = p;
    for (size_t i = 0; i < n; ++i)
        fnv = (fnv ^ b[i]) * 0x100000001B3ull;
}
static void feed_i(int32_t v) { feed(&v, sizeof v); }
static void feed_d(double v) { feed(&v, sizeof v); }

static int64_t ulps(double a, double b)
{
    int64_t ia, ib;
    memcpy(&ia, &a, 8);
    memcpy(&ib, &b, 8);
    if (ia < 0) ia = INT64_MIN - ia;
    if (ib < 0) ib = INT64_MIN - ib;
    return ia > ib ? ia - ib : ib - ia;
}

static void rules(void)
{
    TS_SIN t;
    ts_sin_table(&t);
    CHECK(t.v[0] == 0 && t.v[128] == 8192 && t.v[256] == 0 && t.v[384] == -8192, "the four exact values: %d %d %d %d",
          t.v[0], t.v[128], t.v[256], t.v[384]);
    for (int i = 1; i < 256; ++i) {
        CHECK(t.v[256 - i] == t.v[i], "sin(pi - x) at %d", i);
        CHECK(t.v[256 + i] == -t.v[i], "sin(pi + x) at %d", i);
    }
    for (int i = 0; i < TS_SIN_N; ++i)
        CHECK(t.v[i] == (int16_t)floor(8192.0 * sin(2.0 * 3.141592653589793 * i / TS_SIN_N) + 0.5),
              "the formula on libm at %d", i);

    /* a quarter turn is 0x4000; heading 0 faces +z, so turn_z(0, v) is v and turn_x(0, v) is 0 */
    CHECK(ts_turn_x(&t, 0, 65536) == 0 && ts_turn_z(&t, 0, 65536) == 65536, "heading 0");
    CHECK(ts_turn_x(&t, 0x4000, 65536) == 65536 && ts_turn_z(&t, 0x4000, 65536) == 0, "heading 0x4000");
    CHECK(ts_turn_x(&t, -0x4000, 65536) == -65536, "heading -0x4000");
    CHECK(ts_turn_x(&t, 0x4000 + 0x10000, 65536) == 65536, "the angle wraps at 16 bits");
    /* (8192 * v + 0x1000) >> 13 rounds the half up: v = -1 gives (-8192 + 4096) >> 13 = -1 */
    CHECK(ts_turn_z(&t, 0, -1) == -1 && ts_turn_z(&t, 0, 1) == 1, "the rounding constant");

    CHECK(ts_fistp(0.5) == 0 && ts_fistp(1.5) == 2 && ts_fistp(2.5) == 2 && ts_fistp(-0.5) == 0 &&
          ts_fistp(-1.5) == -2, "fistp rounds ties to even");
    CHECK(ts_fistp(2147483647.0) == INT32_MAX && ts_fistp(2147483648.0) == INT32_MIN &&
          ts_fistp(-2147483648.0) == INT32_MIN && ts_fistp(NAN) == INT32_MIN, "fistp's integer indefinite");
    CHECK(ts_ftol(2.9) == 2 && ts_ftol(-2.9) == -2 && ts_ftol(4294967297.0) == 1 && ts_ftol(1e30) == 0 &&
          ts_ftol(NAN) == 0, "ftol chops and keeps the low dword");

    CHECK(ts_mul_shr(3 << 16, 5 << 16, 16) == 15 << 16, "16.16 product");
    CHECK(ts_mul_shr(-1, 1, 1) == -1 && ts_mul_shr(-5, 1, 64) == -1 && ts_mul_shr(5, 1, 70) == 0,
          "arithmetic shift and the shift past 63");
    uint32_t zero = 0;
    CHECK(ts_shl_div(25 << 16, 16, 0x640000, &zero) == 16384 && zero == 0, "trunc(cap / 100) in 16.16");
    CHECK(ts_shl_div(-7, 0, 2, &zero) == -3 && zero == 0, "truncation toward zero");
    CHECK(ts_shl_div(1, 16, 0, &zero) == 0 && zero == 1, "a zero divisor answers 0 and is counted");

    CHECK(ts_angle(0, 100) == 0 && ts_angle(100, 0) == 16384 && ts_angle(0, -100) == 32768 &&
          ts_angle(-100, 0) == -16384, "the angle's quarter turns");
    int32_t x = 1000, z = 0;
    ts_rotate(16384, &x, &z);
    CHECK(x == 0 && z == 1000, "a quarter turn of (1000, 0): %d %d", x, z);
    x = 1000, z = 0;
    ts_rotate(0, &x, &z);
    CHECK(x == 1000 && z == 0, "angle 0 leaves the point");
    CHECK(ts_hypot(3.0, 4.0) == 5.0, "hypot");
    CHECK(isnan(ts_sin(1e7)) && ts_sin(1e5) == ts_sin(1e5), "the reduction's range");
}

static int libm(void)
{
    int64_t worst[3] = { 0, 0, 0 };
    long differ[3] = { 0, 0, 0 };
    for (int i = 0; i < 1000000; ++i) {
        const double a = ((double)(next() >> 11) / 9007199254740992.0 - 0.5) * 2.0 * 2000.0;
        const double y = ((double)(int64_t)next()) / 4294967296.0, x = ((double)(int64_t)next()) / 4294967296.0;
        const int64_t u[3] = { ulps(ts_sin(a), sin(a)), ulps(ts_cos(a), cos(a)), ulps(ts_atan2(y, x), atan2(y, x)) };
        for (int k = 0; k < 3; ++k) {
            differ[k] += u[k] != 0;
            if (u[k] > worst[k])
                worst[k] = u[k];
        }
    }
    printf("libm: sin %ld differ (worst %lld ulp), cos %ld (%lld), atan2 %ld (%lld) of 1000000\n", differ[0],
           (long long)worst[0], differ[1], (long long)worst[1], differ[2], (long long)worst[2]);
    for (int k = 0; k < 3; ++k)
        CHECK(worst[k] <= 1, "function %d within one ulp", k);
    return fails != 0;
}

static int digest(void)
{
    TS_SIN t;
    ts_sin_table(&t);
    feed(t.v, sizeof t.v);
    for (int i = 0; i < 1000000; ++i) {
        const int32_t dx = (int32_t)next(), dz = (int32_t)next();
        const int32_t sx = (int32_t)(next() >> 40) - (1 << 23), sz = (int32_t)(next() >> 40) - (1 << 23);
        const int16_t a = (int16_t)next();
        feed_i(ts_angle(dx, dz));
        feed_i(ts_angle(sx >> 8, sz >> 8));
        int32_t x = sx, z = sz;
        ts_rotate(a, &x, &z);
        feed_i(x);
        feed_i(z);
        feed_i(ts_turn_x(&t, a, dx));
        feed_i(ts_turn_z(&t, a, dz));
        feed_i(ts_ftol(ts_hypot((double)dx, (double)dz)));
        feed_d(ts_sin(a * 9.587379924285e-05));
        feed_d(ts_cos(a * 9.587379924285e-05));
    }
    printf("digest %016llx\n", (unsigned long long)fnv);
    return 0;
}

static int velocity(void)
{
    TS_SIN t;
    ts_sin_table(&t);
    long n = 0, bad = 0;
    long h, s, vx, vz;
    while (scanf("%ld %ld %ld %ld", &h, &s, &vx, &vz) == 4) {
        ++n;
        const int32_t ex = -ts_turn_x(&t, (int32_t)h, (int32_t)s), ez = -ts_turn_z(&t, (int32_t)h, (int32_t)s);
        if (ex != (int32_t)vx || ez != (int32_t)vz) {
            if (++bad <= 10)
                printf("FAIL heading %ld speed %ld: engine (%ld, %ld), core (%d, %d)\n", h, s, vx, vz, ex, ez);
        }
    }
    printf("velocity: %ld of %ld equal\n", n - bad, n);
    return bad != 0 || n == 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "libm"))
        return libm();
    if (argc > 1 && !strcmp(argv[1], "digest"))
        return digest();
    if (argc > 1 && !strcmp(argv[1], "velocity"))
        return velocity();
    rules();
    printf(fails ? "tasim_fixed_test: %d failed\n" : "tasim_fixed_test: ok\n", fails);
    return fails != 0;
}
