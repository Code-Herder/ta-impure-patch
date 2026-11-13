/* tasim_combat_test.c -- the self-test of the core's combat pieces that stand alone: the projectile record and pool
   and the ballistic pitch solver (src/tasim_proj.h). Driven by tests/tasim_combat_test.py, which builds it natively, under the sanitizers and with
   the DLL's i686 compiler under Wine.

     tasim_combat_test           the rules on hand-checked cases and against literal ports of the engine's
                                 instruction sequences on random input; exit 1 on a failure
     tasim_combat_test digest    one 64-bit FNV-1a digest of every function on arguments from a fixed generator; the
                                 driver requires every build to print the same digest
     tasim_combat_test x87       (the i686 build only) every function that computes in floating point against the
                                 engine's instruction sequence run on the x87 itself, under the game's control word,
                                 on random input: equal, or a difference the residual it names explains */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_fixed.h"
#include "tasim_proj.h"

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
static uint32_t below(uint32_t n) { return (uint32_t)(next() % n); }

static uint64_t fnv = 0xCBF29CE484222325ull;
static void feed(const void* p, size_t n)
{
    const unsigned char* b = p;
    for (size_t i = 0; i < n; ++i)
        fnv = (fnv ^ b[i]) * 0x100000001B3ull;
}

/* ---- the pool ------------------------------------------------------------------------------------------------ */

#define CAP 64

/* 0x49AE20 as the instructions run it (0x49AE20..0x49AF83): the two stack tables, the survivors' scan for each
   registered quarry, the early exit once every one is fixed */
static void engine_compact(TS_PROJ* pool, int32_t* count)
{
    int16_t A[CAP], B[CAP];
    int32_t n = *count, m = 0, dst = -1;
    for (int32_t i = 0; i < *count; ++i) {
        TS_PROJ* r = &pool[i];
        ts_pj_set_u16(r, TS_PJ_STAMP, (uint16_t)i);
        if (r->b[TS_PJ_FLAGS] & 2) { if (dst < 0) dst = i; --n; continue; }
        if (dst < 0) continue;
        const uint32_t q = ts_pj_u32(r, TS_PJ_QUARRY);
        if (q) { B[m] = (int16_t)dst; A[m] = (int16_t)(q - 1); ++m; }
        memcpy(&pool[dst], r, TS_PROJ_SIZE);
        ++dst;
    }
    if (m != 0 && n > 0) {
        int32_t fixed = 0;
        for (int32_t j = 0; j < n; ++j)
            for (int32_t k = 0; k < m; ++k)
                if (ts_pj_u16(&pool[j], TS_PJ_STAMP) == (uint16_t)A[k]) {
                    ts_pj_set_u32(&pool[B[k]], TS_PJ_QUARRY, (uint32_t)j + 1u);
                    if (++fixed == m) goto done;
                }
    }
done:
    *count = n;
}

static void random_pool(TS_PROJ* rec, int32_t count)
{
    for (int32_t i = 0; i < CAP; ++i)
        for (int k = 0; k < TS_PROJ_SIZE; ++k)
            rec[i].b[k] = (unsigned char)next();
    for (int32_t i = 0; i < count; ++i) {
        rec[i].b[TS_PJ_FLAGS] = (unsigned char)((rec[i].b[TS_PJ_FLAGS] & ~2u) | (below(3) == 0 ? 2u : 0u));
        const uint32_t r = below(4);   /* no quarry, a live index, any index of the capacity */
        ts_pj_set_u32(&rec[i], TS_PJ_QUARRY, r == 0 ? 0u : r == 1 ? below((uint32_t)count) + 1u : below(CAP) + 1u);
    }
}

static void pool_rules(void)
{
    static TS_PROJ rec[CAP], ref[CAP];
    TS_PROJPOOL pool;
    CHECK(ts_proj_pool_init(&pool, rec, 0) == -1 && ts_proj_pool_init(&pool, rec, TS_PROJ_CAP_MAX + 1) == -1,
          "the capacity's bounds");
    memset(rec, 0xA5, sizeof rec);
    CHECK(ts_proj_pool_init(&pool, rec, CAP) == 0 && pool.count == 0 && pool.hw == 0, "init");
    int zero = 1;
    for (size_t i = 0; i < sizeof rec; ++i)
        zero &= ((unsigned char*)rec)[i] == 0;
    CHECK(zero, "the load zeroes every record");

    /* the append clears +0x69 bit 1 and +0x4E and nothing else */
    for (int k = 0; k < TS_PROJ_SIZE; ++k)
        rec[0].b[k] = (unsigned char)(0x80 + k);
    rec[0].b[TS_PJ_FLAGS] = 0xFF;
    memcpy(&ref[0], &rec[0], TS_PROJ_SIZE);
    TS_PROJ* p = ts_proj_append(&pool);
    CHECK(p == &rec[0] && pool.count == 1 && pool.hw == 1, "the first append");
    int same = 1;
    for (int k = 0; k < TS_PROJ_SIZE; ++k)
        if (k == TS_PJ_FLAGS) same &= p->b[k] == 0xFD;
        else if (k >= TS_PJ_TARGET && k < TS_PJ_TARGET + 4) same &= p->b[k] == 0;
        else same &= p->b[k] == ref[0].b[k];
    CHECK(same, "an append keeps every byte but +0x69 bit 1 and +0x4E");
    while (ts_proj_append(&pool)) {}
    CHECK(pool.count == CAP && pool.hw == CAP && ts_proj_append(&pool) == NULL, "no append past the capacity");

    /* hand case: records 0..5; 2 and 4 done. 0 has quarry 3 (moves to 2): before the first hole, NOT re-aimed.
       3 has quarry 5 (survives, moves to 3): re-aimed to 3. 5 has quarry 4 (removed): keeps 4. */
    memset(rec, 0, sizeof rec);
    pool.count = 6;
    rec[2].b[TS_PJ_FLAGS] = rec[4].b[TS_PJ_FLAGS] = TS_PJF_DONE;
    for (int i = 0; i < 6; ++i)
        rec[i].b[TS_PJ_POS] = (unsigned char)(10 + i);
    ts_pj_set_u32(&rec[0], TS_PJ_QUARRY, 3 + 1);
    ts_pj_set_u32(&rec[3], TS_PJ_QUARRY, 5 + 1);
    ts_pj_set_u32(&rec[5], TS_PJ_QUARRY, 4 + 1);
    ts_proj_compact(&pool);
    CHECK(pool.count == 4, "the count: %d", pool.count);
    CHECK(rec[0].b[TS_PJ_POS] == 10 && rec[1].b[TS_PJ_POS] == 11 && rec[2].b[TS_PJ_POS] == 13 &&
          rec[3].b[TS_PJ_POS] == 15, "stable and in order");
    CHECK(ts_pj_u32(&rec[0], TS_PJ_QUARRY) == 4, "a survivor before the first hole keeps its quarry index");
    CHECK(ts_pj_u32(&rec[2], TS_PJ_QUARRY) == 4, "a mover whose quarry survived is re-aimed: %u",
          ts_pj_u32(&rec[2], TS_PJ_QUARRY));
    CHECK(ts_pj_u32(&rec[3], TS_PJ_QUARRY) == 5, "a link to a removed quarry keeps its index");
    CHECK(ts_pj_u16(&rec[0], TS_PJ_STAMP) == 0 && ts_pj_u16(&rec[2], TS_PJ_STAMP) == 3 &&
          ts_pj_u16(&rec[3], TS_PJ_STAMP) == 5 && ts_pj_u16(&rec[4], TS_PJ_STAMP) == 4 &&
          ts_pj_u16(&rec[5], TS_PJ_STAMP) == 5, "the stamp on every record, the tail keeping its bytes");

    /* against the engine's sequence on random pools: every byte of every record and the count */
    long differ = 0;
    for (int t = 0; t < 20000; ++t) {
        const int32_t count = (int32_t)below(CAP + 1);
        random_pool(rec, count);
        memcpy(ref, rec, sizeof rec);
        int32_t rc = count;
        pool.count = count;
        ts_proj_compact(&pool);
        engine_compact(ref, &rc);
        differ += rc != pool.count || memcmp(ref, rec, sizeof rec) != 0;
    }
    CHECK(differ == 0, "compaction against the engine's sequence: %ld of 20000 pools differ", differ);
}

static void pool_digest(void)
{
    static TS_PROJ rec[CAP];
    TS_PROJPOOL pool;
    ts_proj_pool_init(&pool, rec, CAP);
    for (int t = 0; t < 20000; ++t) {
        random_pool(rec, (int32_t)below(CAP + 1));
        pool.count = (int32_t)below(CAP + 1);
        ts_proj_compact(&pool);
        while (below(4) && ts_proj_append(&pool)) {}
        feed(rec, sizeof rec);
        feed(&pool.count, sizeof pool.count);
    }
}

/* ---- the ballistic pitch solver --------------------------------------------------------------------------- */

/* one random solver input: half in the ranges play reaches (a target within 2048 px and 255 px of height, a
   weaponvelocity of 100..1500 px a second, gravity 2000..20000, minbarrel -22.5..+12 degrees), half anything */
typedef struct { int32_t dx, dy, dz, v, g; float mb; } PITCHIN;
static PITCHIN pitch_input(void)
{
    PITCHIN a;
    if (next() & 1) {
        a.dx = (int32_t)(below(2u * 2048u << 16) - (2048u << 16));
        a.dz = (int32_t)(below(2u * 2048u << 16) - (2048u << 16));
        a.dy = (int32_t)(below(2u * 255u << 16) - (255u << 16));
        a.v = (int32_t)(below((1500u - 100u) * 65536u / 30u) + 100u * 65536u / 30u);
        a.g = (int32_t)(2000 + below(18000));
        a.mb = (float)(((double)below(1000) / 1000.0 * 34.5 - 22.5) * 3.14159265358979 / 180.0);
    } else {
        a.dx = (int32_t)next(); a.dy = (int32_t)next(); a.dz = (int32_t)next();
        a.v = (int32_t)next();
        const int shift = (int)below(17) + 8;
        a.g = (int32_t)next() >> shift;
        a.mb = (float)((double)(int32_t)next() / 2147483648.0 * 2.0);
    }
    return a;
}

static void pitch_rules(void)
{
    const int32_t v = 655360, g = 8155;
    /* sin 2t = g d / v^2: half the flat range is 15 degrees, 2730.67 units, truncated */
    CHECK(ts_ballistic_pitch(26333337, 0, 0, v, -0.19635f, g) == 2730, "half the flat range: %u",
          ts_ballistic_pitch(26333337, 0, 0, v, -0.19635f, g));
    CHECK(ts_ballistic_pitch(0, 0, 0, v, -0.19635f, g) == TS_PITCH_NONE, "no offset: D = 0, both roots unordered");
    CHECK(ts_ballistic_pitch(2 * 52666674, 0, 0, v, -0.19635f, g) == TS_PITCH_NONE, "past the flat range: E < 0");
    /* minbarrel above the low root: the high one is 75 degrees, past the truncated pi/4 */
    CHECK(ts_ballistic_pitch(26333337, 0, 0, v, 0.3f, g) == TS_PITCH_NONE, "the high root past pi/4");
    /* the flat range's limit, where the two roots meet at 45 degrees: 8192 units exactly is pi/4, and the exe's pi/4
       is below the true one, so the angle at the limit is refused unless it rounds below the truncated constant */
    const uint16_t edge = ts_ballistic_pitch(52666650, 0, 0, v, -0.19635f, g);
    CHECK(edge == TS_PITCH_NONE || edge <= 8192, "the flat range's limit: %u", edge);
}

static void pitch_digest(void)
{
    for (int i = 0; i < 1000000; ++i) {
        const PITCHIN a = pitch_input();
        const uint16_t r = ts_ballistic_pitch(a.dx, a.dy, a.dz, a.v, a.mb, a.g);
        feed(&r, sizeof r);
    }
}

#if defined(__i386__)
/* The engine's instruction sequence 0x49A8C5..0x49AA61 on the x87 under the game's control word 0x27F, transcribed
   from the listing: the frame's doubles at the engine's own local offsets (L0 .. L28), the arguments after them. The
   CRT acos's normal path is its own five instructions and fpatan (0x4E6828..0x4E6836); its special cases (|x| = 1,
   |x| > 1, a NaN) are the answers 0x4E6855..0x4E688F give. `fsubp st(1), st` is written as its bytes DE E9 so the
   assembler cannot swap it. h is ts_hypot, which tests/tasim_fixed_test.c holds to the engine's _hypot on the x87. */
typedef struct {
    double L0, L8, L10, L18, L20, L28;         /* 0x00..0x2F                                                  */
    int32_t dy, g, v, g2;                      /* 0x30, 0x34, 0x38, 0x3C                                      */
    float mb; int32_t pad;                     /* 0x40                                                        */
    double h;                                  /* 0x48                                                        */
    double m2, zero, pi2, pi4, k32768, invpi;  /* 0x50, 0x58, 0x60, 0x68, 0x70, 0x78                          */
    double ans;                                /* 0x80                                                        */
    int32_t refused;                           /* 0x88                                                        */
    uint16_t cw_old, cw_game;                  /* 0x8C, 0x8E                                                  */
    double nan;                                /* 0x90                                                        */
    long double t2;                            /* 0x98: the high root's angle as the x87 holds it             */
} X87PITCH;

#define X87_ACOS                                                                                                  \
    "fld1\n\t" "fld st(1)\n\t" "fabs\n\t" "fcomip st, st(1)\n\t" "fstp st(0)\n\t" "jp 75f\n\t" "jb 70f\n\t"       \
    "fld1\n\t" "fcomip st, st(1)\n\t" "je 72f\n\t" "fld1\n\t" "fchs\n\t" "fcomip st, st(1)\n\t" "je 73f\n\t"       \
    "75:\n\t" "fstp st(0)\n\t" "fld qword ptr [esi+0x90]\n\t" "jmp 79f\n\t"                                       \
    "72:\n\t" "fstp st(0)\n\t" "fldz\n\t" "jmp 79f\n\t"                                                           \
    "73:\n\t" "fstp st(0)\n\t" "fldpi\n\t" "jmp 79f\n\t"                                                          \
    "70:\n\t" "fld1\n\t" "fadd st, st(1)\n\t" "fld1\n\t" "fsub st, st(2)\n\t" "fmulp st(1), st\n\t" "fsqrt\n\t"  \
    "fxch st(1)\n\t" "fpatan\n\t"                                                                                 \
    "79:\n\t"

static uint16_t x87_pitch(const PITCHIN* a, X87PITCH* f)
{
    memset(f, 0, sizeof *f);
    f->dy = a->dy; f->g = a->g; f->v = a->v; f->g2 = (int32_t)((uint32_t)a->g * (uint32_t)a->g); f->mb = a->mb;
    f->h = ts_hypot(a->dx, a->dz);
    f->m2 = -2.0; f->zero = 0.0; f->pi2 = 0x1.921fb54442d11p+0; f->pi4 = 0x1.921fb54442d11p-1;
    f->k32768 = 32768.0; f->invpi = 0x1.45f306dc9c889p-2; f->nan = NAN;
    f->cw_game = 0x027F;
    __asm__ __volatile__(
        ".intel_syntax noprefix\n\t"
        "fnstcw word ptr [esi+0x8C]\n\t" "fldcw word ptr [esi+0x8E]\n\t"
        "fld qword ptr [esi+0x48]\n\t" "fild dword ptr [esi+0x34]\n\t" "fild dword ptr [esi+0x30]\n\t"
        "fstp qword ptr [esi+0x0]\n\t" "fmul qword ptr [esi+0x0]\n\t" "fild dword ptr [esi+0x38]\n\t"
        "fxch st(2)\n\t" "fst qword ptr [esi+0x10]\n\t" "fmul qword ptr [esi+0x10]\n\t" "fxch st(2)\n\t"
        "fst qword ptr [esi+0x8]\n\t" "fld qword ptr [esi+0x0]\n\t" "fxch st(1)\n\t" "fmul qword ptr [esi+0x8]\n\t"
        "fxch st(2)\n\t" "fst qword ptr [esi+0x28]\n\t" "fxch st(1)\n\t" "fmul qword ptr [esi+0x0]\n\t"
        "fxch st(3)\n\t" "fstp qword ptr [esi+0x0]\n\t" "fmul qword ptr [esi+0x50]\n\t" "fld qword ptr [esi+0x0]\n\t"
        "fmul qword ptr [esi+0x0]\n\t" "fxch st(2)\n\t" "fstp qword ptr [esi+0x10]\n\t" "fsubr qword ptr [esi+0x10]\n\t"
        "fild dword ptr [esi+0x3C]\n\t" "fxch st(3)\n\t" "fst qword ptr [esi+0x18]\n\t" "fld qword ptr [esi+0x18]\n\t"
        "fxch st(2)\n\t" "fmul qword ptr [esi+0x10]\n\t" "fxch st(4)\n\t" "fstp qword ptr [esi+0x18]\n\t"
        "fmul qword ptr [esi+0x18]\n\t" "fxch st(1)\n\t" "fadd qword ptr [esi+0x0]\n\t" "fxch st(2)\n\t"
        "fst qword ptr [esi+0x20]\n\t" "fmul qword ptr [esi+0x18]\n\t" "fxch st(1)\n\t" "faddp st(3), st\n\t"
        "fxch st(1)\n\t" "fstp qword ptr [esi+0x18]\n\t" "fmul qword ptr [esi+0x18]\n\t" "fxch st(1)\n\t"
        "fmul qword ptr [esi+0x20]\n\t" "fxch st(1)\n\t" ".byte 0xDE, 0xE9\n\t"
        "fcom qword ptr [esi+0x58]\n\t" "fnstsw ax\n\t" "test ah, 0x1\n\t" "jne 60f\n\t"
        "fsqrt\n\t" "fld qword ptr [esi+0x10]\n\t" "fadd qword ptr [esi+0x28]\n\t" "fld qword ptr [esi+0x18]\n\t"
        "fadd st(0), st\n\t" "fxch st(1)\n\t" "fmul qword ptr [esi+0x0]\n\t" "fxch st(2)\n\t"
        "fst qword ptr [esi+0x28]\n\t" "fxch st(1)\n\t" "fstp qword ptr [esi+0x20]\n\t" "fadd st, st(1)\n\t"
        "fxch st(1)\n\t" "fsub qword ptr [esi+0x28]\n\t" "fxch st(1)\n\t" "fdiv qword ptr [esi+0x20]\n\t"
        "fxch st(1)\n\t" "fdiv qword ptr [esi+0x20]\n\t" "fxch st(1)\n\t" "fst qword ptr [esi+0x28]\n\t"
        "fcomp qword ptr [esi+0x58]\n\t" "fstp qword ptr [esi+0x20]\n\t" "fnstsw ax\n\t" "test ah, 0x41\n\t"
        "jne 20f\n\t"
        "fld qword ptr [esi+0x28]\n\t" "fsqrt\n\t" "fdiv qword ptr [esi+0x8]\n\t" X87_ACOS
        "fstp qword ptr [esi+0x0]\n\t" "jmp 21f\n\t"
        "20:\n\t" "fld qword ptr [esi+0x60]\n\t" "fstp qword ptr [esi+0x0]\n\t"
        "21:\n\t" "fld qword ptr [esi+0x20]\n\t" "fcomp qword ptr [esi+0x58]\n\t" "fnstsw ax\n\t"
        "test ah, 0x41\n\t" "jne 22f\n\t"
        "fld qword ptr [esi+0x20]\n\t" "fsqrt\n\t" "fdiv qword ptr [esi+0x8]\n\t" X87_ACOS "jmp 23f\n\t"
        "22:\n\t" "fld qword ptr [esi+0x60]\n\t"
        "23:\n\t" "fld st(0)\n\t" "fstp tbyte ptr [esi+0x98]\n\t"
        "fld dword ptr [esi+0x40]\n\t" "fcomp qword ptr [esi+0x0]\n\t" "fnstsw ax\n\t" "test ah, 0x1\n\t"
        "je 40f\n\t"
        "fld qword ptr [esi+0x0]\n\t" "fcomp qword ptr [esi+0x68]\n\t" "fnstsw ax\n\t" "test ah, 0x41\n\t"
        "je 40f\n\t"
        "fstp st(0)\n\t" "fld qword ptr [esi+0x0]\n\t" "jmp 50f\n\t"
        "40:\n\t" "fld dword ptr [esi+0x40]\n\t" "fld st(1)\n\t" "fcompp\n\t" "fnstsw ax\n\t" "test ah, 0x41\n\t"
        "jne 60f\n\t"
        "fcom qword ptr [esi+0x68]\n\t" "fnstsw ax\n\t" "test ah, 0x41\n\t" "je 60f\n\t"
        "50:\n\t" "fmul qword ptr [esi+0x70]\n\t" "fmul qword ptr [esi+0x78]\n\t" "fstp qword ptr [esi+0x80]\n\t"
        "mov dword ptr [esi+0x88], 0\n\t" "jmp 90f\n\t"
        "60:\n\t" "fstp st(0)\n\t" "mov dword ptr [esi+0x88], 1\n\t"
        "90:\n\t" "fldcw word ptr [esi+0x8C]\n\t"
        ".att_syntax prefix\n\t"
        : : "S"(f) : "eax", "memory", "cc");
    return f->refused ? (uint16_t)TS_PITCH_NONE : (uint16_t)ts_ftol(f->ans);
}

/* the core's acos (tasim_ballistic.c's crt_acos) for the residual's attribution */
static double core_acos(double x)
{
    if (!(fabs(x) < 1.0)) return x == 1.0 ? 0.0 : x == -1.0 ? 3.14159265358979323846 : NAN;
    return ts_atan2(sqrt((1.0 + x) * (1.0 - x)), x);
}

static int same_d(double a, double b) { return (isnan(a) && isnan(b)) || memcmp(&a, &b, 8) == 0; }

/* Every input on which the core and the x87 answer differently must be R4: the core's low-root angle differs from
   the x87's as a double, or its high-root angle does, or the high root's 64-bit value falls on the other side of
   minbarrel or pi/4 than its double. Any other difference is a port error and fails. */
static int x87_pitch_check(void)
{
    long n = 0, r4 = 0, bad = 0, reached = 0;
    X87PITCH f;
    for (int i = 0; i < 2000000; ++i, ++n) {
        const PITCHIN a = pitch_input();
        const uint16_t e = x87_pitch(&a, &f), c = ts_ballistic_pitch(a.dx, a.dy, a.dz, a.v, a.mb, a.g);
        reached += e != TS_PITCH_NONE;
        if (e == c) continue;
        const double x1 = sqrt(f.L28) / f.L8, x2 = sqrt(f.L20) / f.L8;
        const double t1c = f.L28 > 0.0 ? core_acos(x1) : f.pi2, t2c = f.L20 > 0.0 ? core_acos(x2) : f.pi2;
        const double t2e = (double)f.t2;
        const int explained = !same_d(t1c, f.L0) || !same_d(t2c, t2e) ||
                              ((f.t2 <= (long double)f.pi4) != (t2e <= f.pi4)) ||
                              ((f.t2 > (long double)f.mb) != (t2e > (double)f.mb));
        if (explained) ++r4;
        else if (++bad <= 10)
            printf("FAIL pitch(%d, %d, %d, %d, %a, g %d): x87 %u, core %u\n", a.dx, a.dy, a.dz, a.v, (double)a.mb,
                   a.g, e, c);
    }
    printf("x87: pitch %ld of %ld equal (%ld with a pitch), %ld differ by R4 (fpatan), %ld unexplained\n",
           n - r4 - bad, n, reached, r4, bad);
    return bad != 0;
}
#endif

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "digest")) {
        pool_digest();
        pitch_digest();
        printf("digest %016llx\n", (unsigned long long)fnv);
        return 0;
    }
#if defined(__i386__)
    if (argc > 1 && !strcmp(argv[1], "x87"))
        return x87_pitch_check();
#endif
    pool_rules();
    pitch_rules();
    printf(fails ? "tasim_combat_test: %d failed\n" : "tasim_combat_test: ok\n", fails);
    return fails != 0;
}
