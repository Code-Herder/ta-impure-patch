/* tasim_fixed_test.c -- the self-test of the core's engine arithmetic (src/tasim_fixed.h), driven by
   tests/tasim_fixed_test.py, which builds it natively, under the sanitizers and with the DLL's i686 compiler under
   Wine.

     tasim_fixed_test            the rules: the sine table's symmetry and its four exact values, the conversions'
                                 ties and bounds, the 64-bit helpers, the turn lookups' known answers, the random
                                 stream's rules and the engine's own recorded stream; exit 1 on a failure
     tasim_fixed_test libm       ts_sin, ts_cos and ts_atan2 against the platform's libm on 10^6 arguments each:
                                 every answer within one ulp; the native build's alone, glibc being the reference
     tasim_fixed_test digest     one 64-bit FNV-1a digest of 10^6 calls of each helper on arguments from a fixed
                                 generator; the driver requires every build to print the same digest
     tasim_fixed_test hypot      ts_hypot of each "a b" line on stdin, printed as the double's bits in hex, for a
                                 model of _hypot to compare
     tasim_fixed_test x87        (the i686 build only) ts_hypot against the engine's _hypot instruction sequence
                                 run on the x87 itself, with its control word: every integer-distance pair
                                 0 < a <= b < 3000 and 10^6 random pairs in each of three ranges; all must be equal
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
    CHECK(ts_hypot(3, 4) == 5.0 && ts_hypot(-5, 12) == 13.0 && ts_hypot(0, -7) == 7.0 && ts_hypot(0, 0) == 0.0,
          "hypot's exact cases");
    /* (20, 99) is 101 apart, and the engine's _hypot lands below it (tasim_fixed.h) */
    CHECK(ts_ftol(ts_hypot(20, 99)) == 100 && ts_ftol(ts_hypot(20 << 16, 99 << 16)) == 101 * 65536 - 1,
          "hypot's (20, 99) truncates to 100: %d", ts_ftol(ts_hypot(20, 99)));
    CHECK(ts_hypot(INT32_MIN, 0) == 2147483648.0, "hypot at INT32_MIN");
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

/* THE ENGINE'S OWN STREAM: 0x51FC88 at the first image after each of 50 runs' applies (exogenous `rngseed`), in the
   C1 calibration reference course-kbot recorded on the engine, five launches of ten runs. A launch carries the
   stream from run to run, so between two runs of one launch the engine stepped it `steps` times, and the next run's
   value is the step's image that many times over; a new launch reseeds it (0x49719D), marked 0. 45 chains, 41 433
   steps the engine computed. */
static const uint32_t engine_seed[50] = {
      99687165u,  845207336u, 1133057430u,   94100422u, 1505044673u, 1305832539u,   44001329u, 2041788027u,
    1345906826u,  737479954u,  789435906u,  790559070u,  497629991u, 1843306177u,  381344885u,  794547071u,
     290087443u, 1126283925u, 1803930493u,  826455849u, 1669135056u,  699592986u, 1294574466u,  839516795u,
    1660451118u,  457012866u, 1946463478u, 1426169649u,  214260561u,  691305922u,  453111647u, 1958568005u,
     482821726u, 1493318040u,  613537346u,  346170123u, 1530846901u, 1345895417u, 1345113096u,  280538065u,
    1509665989u, 1842350370u, 1844424292u, 1961280992u, 1959590281u, 1605069600u, 1449827015u,   80750898u,
     764820887u, 1698689759u,
};
static const int engine_steps[49] = {
    902, 929, 930, 916, 929, 899, 927, 929, 915,   0, 916, 904, 933, 924, 925, 930, 895, 926, 921,   0,
    926, 924, 903, 937, 927, 927, 911, 927, 924,   0, 917, 923, 915, 923, 930, 929, 922, 913, 924,   0,
    895, 922, 928, 920, 923, 924, 922, 923, 924,
};

static void stream(void)
{
    uint32_t s = 12345u;
    CHECK(ts_rand(&s, 1) == 0 && ts_rand(&s, 0) == 0 && ts_rand(&s, -7) == 0 && s == 12345u,
          "n < 2 answers 0 and leaves the state");
    CHECK(ts_rand(&s, 2) == 16807u * 12345u % 2u && s == 16807u * 12345u, "one step of a small state: %u", s);

    ts_seed(&s, 0);
    CHECK(s == 0x66E29573u, "the seed of 0: %08x", s);
    ts_seed(&s, 0x66E29572u);
    CHECK(s == 1u, "the seed's low bit");

    /* Park and Miller's own check: from 1, the 10 000th state of the minimal standard generator is 1043618065 */
    s = 1;
    for (int i = 0; i < 10000; ++i)
        ts_rand(&s, 0x7FFFFFFF);
    CHECK(s == 1043618065u, "the minimal standard's 10 000th state: %u", s);

    /* Schrage's form in exact arithmetic, on states of every size: the engine's seed can carry bit 31 */
    for (int i = 0; i < 100000; ++i) {
        const uint32_t s0 = (uint32_t)next();
        int64_t v = 16807 * (int64_t)(s0 % 127773u) - 2836 * (int64_t)(s0 / 127773u);
        if (v <= 0)
            v += 0x7FFFFFFF;
        const uint32_t n = 2u + (uint32_t)(next() % 100000u);
        s = s0;
        const uint32_t r = ts_rand(&s, (int32_t)n);
        CHECK(s == (uint32_t)v && r == (uint32_t)v % n, "Schrage at %u: %u, want %lld", s0, s, (long long)v);
    }

    for (int i = 0; i < 49; ++i) {
        if (!engine_steps[i])
            continue;
        s = engine_seed[i];
        int k = 0;
        while (s != engine_seed[i + 1] && k < engine_steps[i])
            ts_rand(&s, 2), ++k;
        CHECK(s == engine_seed[i + 1] && k == engine_steps[i], "the engine's run %d to %d: %d steps, want %d", i,
              i + 1, k, engine_steps[i]);
    }
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
        feed_i(ts_ftol(ts_hypot(dx, dz)));
        feed_d(ts_hypot(sx, sz));
        feed_d(ts_sin(a * 9.587379924285e-05));
        feed_d(ts_cos(a * 9.587379924285e-05));
    }
    uint32_t s = 1;
    for (int i = 0; i < 1000000; ++i) {
        feed_i((int32_t)ts_rand(&s, (int32_t)(next() >> 33)));
        feed_i((int32_t)s);
    }
    printf("digest %016llx\n", (unsigned long long)fnv);
    return 0;
}

static int hypot_lines(void)
{
    long a, b;
    while (scanf("%ld %ld", &a, &b) == 2) {
        const double h = ts_hypot((int32_t)a, (int32_t)b);
        uint64_t bits;
        memcpy(&bits, &h, 8);
        printf("%016llx\n", (unsigned long long)bits);
    }
    return 0;
}

#if defined(__i386__)
/* 0x4FB480's sequence on the x87 (tasim_fixed.h, ts_hypot): the control word 0x133F, each fdiv, fmul, faddp and
   fsqrt at the 64-bit precision, each result stored to a double; frexp and ldexp are exact */
static double x87_hypot(int32_t a, int32_t b)
{
    double x = fabs((double)a), y = fabs((double)b);
    double M = y > x ? y : x;
    if (M == 0)
        return 0.0;
    unsigned short old, cw = 0x133F;
    double xp, yp, s, q, r;
    __asm__ __volatile__("fnstcw %0" : "=m"(old));
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    __asm__ __volatile__("fldl %1\n\tfdivl %2\n\tfstpl %0" : "=m"(xp) : "m"(x), "m"(M));
    __asm__ __volatile__("fldl %1\n\tfdivl %2\n\tfstpl %0" : "=m"(yp) : "m"(y), "m"(M));
    __asm__ __volatile__("fldl %1\n\tfmull %1\n\tfldl %2\n\tfmull %2\n\tfaddp\n\tfstpl %0"
                         : "=m"(s) : "m"(xp), "m"(yp));
    __asm__ __volatile__("fldl %1\n\tfsqrt\n\tfstpl %0" : "=m"(q) : "m"(s));
    int e1, e2;
    double m1 = frexp(q, &e1), m2 = frexp(M, &e2);
    __asm__ __volatile__("fldl %1\n\tfmull %2\n\tfstpl %0" : "=m"(r) : "m"(m1), "m"(m2));
    __asm__ __volatile__("fldcw %0" : : "m"(old));
    return ldexp(r, e1 + e2);
}

static int x87(void)
{
    long n = 0, bad = 0, below = 0;
    for (int64_t a = 1; a < 3000; ++a)
        for (int64_t b = a; b < 3000; ++b) {
            const int64_t c2 = a * a + b * b;
            int64_t c = (int64_t)sqrt((double)c2);
            while (c * c > c2)
                --c;
            while ((c + 1) * (c + 1) <= c2)
                ++c;
            if (c * c != c2)
                continue;
            ++n;
            const double e = x87_hypot((int32_t)a, (int32_t)b), t = ts_hypot((int32_t)a, (int32_t)b);
            below += ts_ftol(e) == c - 1;
            if (memcmp(&e, &t, 8) && ++bad <= 10)
                printf("FAIL (%d, %d): x87 %a, core %a\n", (int)a, (int)b, e, t);
        }
    printf("x87: %ld integer-distance pairs, %ld below their distance\n", n, below);
    long total = n;
    const uint32_t lim[3] = { 2048, 1u << 26, 0 };
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 1000000; ++i, ++total) {
            const int32_t a = lim[k] ? (int32_t)(next() % (2 * lim[k] + 1)) - (int32_t)lim[k] : (int32_t)next();
            const int32_t b = lim[k] ? (int32_t)(next() % (2 * lim[k] + 1)) - (int32_t)lim[k] : (int32_t)next();
            const double e = x87_hypot(a, b), t = ts_hypot(a, b);
            if (memcmp(&e, &t, 8) && ++bad <= 10)
                printf("FAIL (%d, %d): x87 %a, core %a\n", a, b, e, t);
        }
    printf("x87: %ld of %ld equal\n", total - bad, total);
    return bad != 0;
}

/* 0x4B715A and 0x4B7173 on the x87 under the game's control word (0x027F: 53-bit precision, round to nearest), for
   the residuals R1 and R2 (tasim_fixed.h): how often the core's fdlibm routines give another integer. Counted and
   printed, never a failure: a difference is a tie case the owner decides on with its proof. */
static int32_t x87_angle(int32_t dx, int32_t dz)
{
    static const double k = 10430.37835047;
    unsigned short old, cw = 0x027F;
    int32_t r;
    __asm__ __volatile__("fnstcw %0" : "=m"(old));
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    __asm__ __volatile__("fildl %1\n\tfildl %2\n\tfpatan\n\tfmull %3\n\tfistpl %0"
                         : "=m"(r) : "m"(dx), "m"(dz), "m"(k));
    __asm__ __volatile__("fldcw %0" : : "m"(old));
    return r;
}

static void x87_rotate(int16_t a, int32_t* p)
{
    static const double k = 9.587379924285e-05;
    unsigned short old, cw = 0x027F;
    if (a == 0)
        return;
    __asm__ __volatile__("fnstcw %0" : "=m"(old));
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    __asm__ __volatile__("fildl 4(%0)\n\tfildl (%0)\n\tfld %%st(1)\n\tfld %%st(1)\n\tfilds %1\n\tfmull %2\n\t"
                         "fsincos\n\tfmul %%st, %%st(5)\n\tfmulp %%st, %%st(4)\n\tfmul %%st, %%st(2)\n\t"
                         "fmulp %%st, %%st(1)\n\tfaddp %%st, %%st(3)\n\tfsubrp %%st, %%st(1)\n\t"
                         "fistpl (%0)\n\tfistpl 4(%0)"
                         : : "r"(p), "m"(a), "m"(k) : "memory");
    __asm__ __volatile__("fldcw %0" : : "m"(old));
}

static void residuals(void)
{
    long angle = 0, rot = 0;
    for (int i = 0; i < 1000000; ++i) {
        const int32_t dx = (int32_t)(next() >> 40) - (1 << 23), dz = (int32_t)(next() >> 40) - (1 << 23);
        angle += x87_angle(dx, dz) != ts_angle(dx, dz);
        const int16_t a = (int16_t)next();
        int32_t p[2] = { dx, dz }, x = dx, z = dz;
        x87_rotate(a, p);
        ts_rotate(a, &x, &z);
        rot += p[0] != x || p[1] != z;
    }
    printf("x87: residuals of 1000000 calls each: angle %ld differ, rotate %ld differ\n", angle, rot);
}
#endif

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
    if (argc > 1 && !strcmp(argv[1], "hypot"))
        return hypot_lines();
#if defined(__i386__)
    if (argc > 1 && !strcmp(argv[1], "x87")) {
        residuals();
        return x87();
    }
#endif
    if (argc > 1 && !strcmp(argv[1], "velocity"))
        return velocity();
    rules();
    stream();
    printf(fails ? "tasim_fixed_test: %d failed\n" : "tasim_fixed_test: ok\n", fails);
    return fails != 0;
}
