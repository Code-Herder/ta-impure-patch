/* tasim_combat_test.c -- the self-test of the core's combat pieces that stand alone: the projectile record and pool,
   the flight's steps, the burst and the ballistic pitch solver (src/tasim_proj.h), and the damage arithmetic
   (src/tasim_dmg.h). Driven by tests/tasim_combat_test.py, which builds it natively, under the sanitizers and with
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
#include "tasim_dmg.h"
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

/* ---- the flight's steps -------------------------------------------------------------------------------------- */

static void set3(TS_PROJ* p, unsigned off, int32_t x, int32_t y, int32_t z)
{
    ts_pj_set_i32(p, off, x);
    ts_pj_set_i32(p, off + 4, y);
    ts_pj_set_i32(p, off + 8, z);
}

static void flight_rules(void)
{
    static TS_PROJ rec[4];
    TS_PROJPOOL pool;
    TASIM_WEAPONDEF w;
    int32_t out[3];
    const int32_t tgt[3] = { 111, 222, 333 };
    ts_proj_pool_init(&pool, rec, 4);
    memset(&w, 0, sizeof w);
    TS_PROJ* p = &rec[0];
    set3(p, TS_PJ_POS, 0, 0, 0);
    set3(p, TS_PJ_TPOS, 2000 << 16, 0x00051234, 0);
    set3(&rec[2], TS_PJ_POS, 7, 8, 9);

    /* guidance: the target point; a live target; a quarry before the target; a quarry past the capacity is none */
    ts_proj_guidance(p, &w, &pool, NULL, 0, 0, out);
    CHECK(out[0] == 2000 << 16 && out[1] == 0x00051234 && out[2] == 0, "no quarry, no target: the target point");
    ts_proj_guidance(p, &w, &pool, tgt, 0, 0, out);
    CHECK(out[0] == 111 && out[1] == 222 && out[2] == 333, "a live target");
    ts_pj_set_u32(p, TS_PJ_QUARRY, 2 + 1);
    ts_proj_guidance(p, &w, &pool, tgt, 0, 0, out);
    CHECK(out[0] == 7 && out[1] == 8 && out[2] == 9, "the quarry first");
    ts_pj_set_u32(p, TS_PJ_QUARRY, 4 + 1);
    ts_proj_guidance(p, &w, &pool, tgt, 0, 0, out);
    CHECK(out[0] == 111, "a quarry index past the capacity reads as none");
    ts_pj_set_u32(p, TS_PJ_QUARRY, 0);
    /* cruise: 2000 px out flies at height 700 over the target point, y's low word kept */
    w.flags = TS_WF_CRUISE;
    ts_proj_guidance(p, &w, &pool, tgt, 0, 0, out);
    CHECK(out[0] == 2000 << 16 && out[1] == 0x02BC1234 && out[2] == 0 && ts_pj_i32(p, TS_PJ_TAIL + 4) == 0x02BC1234,
          "cruise far: height 700, low word kept: %08x", (unsigned)out[1]);
    /* 1025 px is past 0x400, a sixty-five-thousandth of a px less is not; inside, the target, then the ground or sea */
    set3(p, TS_PJ_TPOS, 1025 << 16, 0, 0);
    ts_proj_guidance(p, &w, &pool, tgt, 0, 0, out);
    CHECK(out[1] == 0x02BC0000, "cruise at 1025 px");
    set3(p, TS_PJ_TPOS, (1025 << 16) - 1, 0, 0);
    ts_proj_guidance(p, &w, &pool, tgt, 0, 0, out);
    CHECK(out[0] == 111, "cruise inside 1025 px: the live target");
    ts_proj_guidance(p, &w, &pool, NULL, 40, 30, out);
    CHECK(out[0] == (1025 << 16) - 1 && out[1] == 40 << 16 && ts_pj_i32(p, TS_PJ_TAIL + 4) == 40 << 16,
          "cruise, no target: the ground above the sea");
    ts_proj_guidance(p, &w, &pool, NULL, -1, 30, out);
    CHECK(out[1] == 30 << 16, "cruise, no target: the sea above the ground");

    /* steer: a target due +x of the projectile wants heading 0xC000 (the angle of p - t = (-dx, 0)) */
    memset(&w, 0, sizeof w);
    w.turnrate = 0x100;
    set3(p, TS_PJ_POS, 0, 0, 0);
    ts_pj_set_u16(p, TS_PJ_HEADING, 0xC080);
    ts_pj_set_u16(p, TS_PJ_PITCH, 0);
    const int32_t east[3] = { 100 << 16, 0, 0 };
    CHECK(ts_proj_steer(p, &w, east) == 1 && ts_pj_u16(p, TS_PJ_HEADING) == 0xC000, "an error below the rate snaps");
    ts_pj_set_u16(p, TS_PJ_HEADING, 0xD000);
    ts_proj_steer(p, &w, east);
    CHECK(ts_pj_u16(p, TS_PJ_HEADING) == 0xCF00, "a negative error turns down by the rate: %04x",
          ts_pj_u16(p, TS_PJ_HEADING));
    ts_pj_set_u16(p, TS_PJ_HEADING, 0xB000);
    ts_proj_steer(p, &w, east);
    CHECK(ts_pj_u16(p, TS_PJ_HEADING) == 0xB100, "a positive error turns up by the rate");
    /* burnblow: an error past 0x6978 detonates; exactly 0x8000 reads as -32768 and snaps */
    w.flags = TS_WF_BURNBLOW;
    ts_pj_set_u16(p, TS_PJ_HEADING, 0x5000);
    CHECK(ts_proj_steer(p, &w, east) == 0 && ts_pj_u16(p, TS_PJ_HEADING) == 0x5000, "a burnblow past 0x6978");
    ts_pj_set_u16(p, TS_PJ_HEADING, 0x4000);
    CHECK(ts_proj_steer(p, &w, east) == 1 && ts_pj_u16(p, TS_PJ_HEADING) == 0xC000, "an error of 0x8000 snaps");
    /* the heading is stored before the pitch detonates: a target straight below wants pitch 0xC000, 0x9000 from 0x3000 */
    const int32_t down[3] = { 0, -(100 << 16), 0 };
    ts_pj_set_u16(p, TS_PJ_HEADING, 0x0100);
    ts_pj_set_u16(p, TS_PJ_PITCH, 0x3000);
    CHECK(ts_proj_steer(p, &w, down) == 0 && ts_pj_u16(p, TS_PJ_HEADING) == 0x0000 &&
          ts_pj_u16(p, TS_PJ_PITCH) == 0x3000, "the pitch detonates after the heading's store: %04x %04x",
          ts_pj_u16(p, TS_PJ_HEADING), ts_pj_u16(p, TS_PJ_PITCH));

    /* the drift: velocity, then wind, then gravity, wrapping */
    set3(p, TS_PJ_POS, 0x7FFFFFFF, 10, -5);
    set3(p, TS_PJ_VEL, 1, -20, 5);
    const int32_t wind[3] = { 3, 0, -4 };
    ts_proj_drift(p, wind, 8155);
    CHECK(ts_pj_i32(p, TS_PJ_POS) == (int32_t)0x80000003u && ts_pj_i32(p, TS_PJ_POS + 4) == -10 &&
          ts_pj_i32(p, TS_PJ_POS + 8) == -4 && ts_pj_i32(p, TS_PJ_VEL + 4) == -20 - 8155 &&
          ts_pj_i32(p, TS_PJ_VEL) == 1, "the drift");
}

static void random_rec(TS_PROJ* p)
{
    for (int k = 0; k < TS_PROJ_SIZE; ++k)
        p->b[k] = (unsigned char)next();
}

static void flight_digest(void)
{
    static TS_PROJ rec[8];
    TS_PROJPOOL pool;
    TASIM_WEAPONDEF w;
    int32_t out[3], t[3], wind[3];
    ts_proj_pool_init(&pool, rec, 8);
    memset(&w, 0, sizeof w);
    for (int i = 0; i < 8; ++i)
        random_rec(&rec[i]);
    for (int i = 0; i < 300000; ++i) {
        TS_PROJ* p = &rec[below(8)];
        random_rec(p);
        ts_pj_set_u32(p, TS_PJ_QUARRY, below(10));
        w.flags = (uint32_t)next();
        w.turnrate = (uint16_t)next();
        for (int k = 0; k < 3; ++k) {
            t[k] = (int32_t)next();
            wind[k] = (int32_t)next() >> 12;
        }
        const int32_t ground = (int32_t)below(300) - 1;
        const int live = (int)below(2);
        const uint8_t sea = (uint8_t)next();
        ts_proj_guidance(p, &w, &pool, live ? t : NULL, ground, sea, out);
        feed(out, sizeof out);
        const int r = ts_proj_steer(p, &w, out);
        feed(&r, sizeof r);
        ts_proj_drift(p, wind, (int32_t)below(20000));
        feed(p->b, TS_PROJ_SIZE);
    }
}

/* ---- the burst --------------------------------------------------------------------------------------------- */

/* a stand-in for the caller's stream and fire point: it logs every call in order, and answers draws from the fixed
   generator as ts_rand's contract does (n < 2 answers 0) */
typedef struct { int32_t log[16]; int n; int fp; TS_PROJ* fp_rec; } BURSTLOG;
static uint32_t log_draw(void* ctx, int32_t n)
{
    BURSTLOG* l = ctx;
    if (l->n < 16) l->log[l->n] = n;
    ++l->n;
    return n < 2 ? 0u : (uint32_t)(next() % (uint32_t)n);
}
static void log_fire_point(void* ctx, TS_PROJ* p)
{
    BURSTLOG* l = ctx;
    ++l->fp;
    l->fp_rec = p;
    ts_pj_set_i32(p, TS_PJ_POS, ts_pj_i32(p, TS_PJ_POS) + 1000);   /* a re-queried point the copy must carry */
}

/* 0x49B776..0x49B9A9 as the instructions run it, on the same bytes: the due test, the re-query, the decrement, the
   append, the rep movs, the death, #125, the count, #126 into the parent, the done bit */
static int32_t engine_burst(TS_PROJ* pool, int32_t* count, int32_t cap, int32_t i, const TASIM_WEAPONDEF* w,
                            uint32_t gt, const TS_SIN* sin, BURSTLOG* l, uint32_t* div0)
{
    unsigned char* ebp = pool[i].b;
    TS_PROJ* ebx = NULL;
    uint16_t ax;
    memcpy(&ax, ebp + 0x60, 2);
    if (ax == 0) return -1;
    uint32_t t42;
    memcpy(&t42, ebp + 0x42, 4);
    if (gt < t42 + (uint32_t)w->burstrate) return -1;
    if (!(w->burstrate < 5 && !(ax & 1))) log_fire_point(l, &pool[i]);
    uint16_t b60;
    memcpy(&b60, ebp + 0x60, 2);
    --b60;
    memcpy(ebp + 0x60, &b60, 2);
    memcpy(&t42, ebp + 0x42, 4);
    t42 += w->burstrate;
    memcpy(ebp + 0x42, &t42, 4);
    if (*count < cap) {
        ebx = &pool[*count];
        ++*count;
        uint16_t f;
        memcpy(&f, ebx->b + 0x69, 2);
        f &= 0xFFFD;
        memcpy(ebx->b + 0x69, &f, 2);
        memset(ebx->b + 0x4E, 0, 4);
    }
    if (ebx) {
        memmove(ebx->b, ebp, 0x6B);
        memcpy(ebx->b + 0x42, &gt, 4);
        uint32_t d;
        if (w->weapontimer) d = gt + w->weapontimer;
        else {
            uint32_t dist, spd;
            memcpy(&dist, ebp + 0x3E, 4);
            memcpy(&spd, ebp + 0x3A, 4);
            if (spd) d = (dist + 0x100000u) / spd + gt;
            else { ++*div0; d = gt; }
        }
        memcpy(ebx->b + 0x46, &d, 4);
        if (w->randomdecay) {
            const uint32_t r = log_draw(l, w->randomdecay);
            memcpy(&d, ebx->b + 0x46, 4);
            d += r - ((uint32_t)w->randomdecay >> 1);
            memcpy(ebx->b + 0x46, &d, 4);
        }
        memset(ebx->b + 0x60, 0, 2);
        if (w->sprayangle) {
            const uint32_t r = log_draw(l, w->sprayangle);
            uint16_t cx, pitch;
            memcpy(&cx, ebp + 0x36, 2);
            cx = (uint16_t)(cx - (uint16_t)(w->sprayangle >> 1));
            memcpy(&pitch, ebp + 0x38, 2);
            const int32_t edi = (int32_t)(r + cx);
            const int32_t esi = ts_turn_z(sin, pitch, w->weaponvelocity);
            const int32_t vx = (int32_t)(0u - (uint32_t)ts_turn_x(sin, edi, esi));
            const int32_t vz = (int32_t)(0u - (uint32_t)ts_turn_z(sin, edi, esi));
            memcpy(ebp + 0x1C, &vx, 4);
            memcpy(ebp + 0x24, &vz, 4);
        }
    }
    memcpy(&b60, ebp + 0x60, 2);
    if (b60 == 0) ebp[0x69] |= 2;
    return ebx ? (int32_t)(ebx - pool) : -1;
}

static void burst_rules(void)
{
    static TS_PROJ rec[4], ref[4];
    TS_PROJPOOL pool;
    TASIM_WEAPONDEF w;
    TS_SIN sin;
    BURSTLOG l;
    uint32_t div0 = 0;
    TS_BURST_IO io = { &l, log_draw, log_fire_point, &div0 };
    ts_sin_table(&sin);
    memset(&w, 0, sizeof w);
    memset(&l, 0, sizeof l);
    ts_proj_pool_init(&pool, rec, 3);
    /* the EMG's shape: burst 3, rate 4 (under 5), spray 1024, a weapontimer; the parent fired at 100 */
    w.burst = 3; w.burstrate = 4; w.sprayangle = 1024; w.weapontimer = 30; w.weaponvelocity = 20 << 16;
    TS_PROJ* p = ts_proj_append(&pool);
    for (int k = 0; k < TS_PROJ_SIZE; ++k)
        p->b[k] = (unsigned char)(k * 7);
    p->b[TS_PJ_FLAGS] = 0;
    ts_pj_set_u16(p, TS_PJ_BURST, 3);
    ts_pj_set_u32(p, TS_PJ_TIME, 100);
    ts_pj_set_u16(p, TS_PJ_HEADING, 0x4000);
    ts_pj_set_u16(p, TS_PJ_PITCH, 0);
    const int32_t vx0 = ts_pj_i32(p, TS_PJ_VEL), vz0 = ts_pj_i32(p, TS_PJ_VEL + 8);
    CHECK(ts_proj_burst(&pool, 0, &w, 103, &sin, &io) == -1 && l.n == 0 && ts_pj_u16(p, TS_PJ_BURST) == 3,
          "not due before +0x42 + burstrate");
    memcpy(&ref[0], p, TS_PROJ_SIZE);
    const int32_t c1 = ts_proj_burst(&pool, 0, &w, 104, &sin, &io);
    CHECK(c1 == 1 && pool.count == 2, "the first copy");
    CHECK(l.fp == 1 && l.n == 1 && l.log[0] == 1024, "an odd count re-queries; the spray's one draw (no decay)");
    CHECK(ts_pj_u16(p, TS_PJ_BURST) == 2 && ts_pj_u32(p, TS_PJ_TIME) == 104, "the parent counts down");
    CHECK(ts_pj_i32(&rec[1], TS_PJ_VEL) == vx0 && ts_pj_i32(&rec[1], TS_PJ_VEL + 8) == vz0 &&
          ts_pj_i32(&rec[1], TS_PJ_POS) == ts_pj_i32(&ref[0], TS_PJ_POS) + 1000,
          "the first copy flies unsprayed, from the re-queried point");
    CHECK(ts_pj_u32(&rec[1], TS_PJ_DEATH) == 134 && ts_pj_u32(&rec[1], TS_PJ_TIME) == 104 &&
          ts_pj_u16(&rec[1], TS_PJ_BURST) == 0, "the copy's death, time and count");
    const int32_t vx1 = ts_pj_i32(p, TS_PJ_VEL), vz1 = ts_pj_i32(p, TS_PJ_VEL + 8);
    CHECK(ts_proj_burst(&pool, 0, &w, 108, &sin, &io) == 2 && l.fp == 1, "an even count under rate 5: no re-query");
    CHECK(ts_pj_i32(&rec[2], TS_PJ_VEL) == vx1 && ts_pj_i32(&rec[2], TS_PJ_VEL + 8) == vz1,
          "the second copy flies with the spray drawn for the first");
    /* the pool is full: no copy and no draw, but the count runs down and the parent ends */
    const int drawn = l.n;
    CHECK(ts_proj_burst(&pool, 0, &w, 112, &sin, &io) == -1 && l.n == drawn && l.fp == 2 &&
          ts_pj_u16(p, TS_PJ_BURST) == 0 && (p->b[TS_PJ_FLAGS] & TS_PJF_DONE), "a full pool: no copy, no draw");
    /* weapontimer 0: (dist + 0x100000) / speed, unsigned; a speed of 0 is D1 */
    ts_proj_pool_init(&pool, rec, 3);
    w.weapontimer = 0; w.sprayangle = 0; w.randomdecay = 9;
    p = ts_proj_append(&pool);
    ts_pj_set_u16(p, TS_PJ_BURST, 2);
    ts_pj_set_u32(p, TS_PJ_TIME, 0);
    ts_pj_set_u32(p, TS_PJ_DIST, 0xFFF80000u);
    ts_pj_set_u32(p, TS_PJ_SPEED, 0x10000);
    l.n = 0;
    ts_proj_burst(&pool, 0, &w, 50, &sin, &io);
    const uint32_t r = ts_pj_u32(&rec[1], TS_PJ_DEATH) - (0x00080000u / 0x10000u + 50u) + 4u;
    CHECK(l.n == 1 && l.log[0] == 9 && r < 9, "the death wraps unsigned, then the decay's draw: %u", r);
    ts_pj_set_u32(p, TS_PJ_SPEED, 0);
    w.randomdecay = 0;
    ts_proj_burst(&pool, 0, &w, 60, &sin, &io);
    CHECK(div0 == 1 && ts_pj_u32(&rec[2], TS_PJ_DEATH) == 60, "D1: a zero speed counted, the weapontimer's death");

    /* against the engine's sequence on random parents, weapons and pools */
    long differ = 0;
    static TS_PROJ a[8], b[8];
    for (int t = 0; t < 200000; ++t) {
        for (int i = 0; i < 8; ++i)
            for (int k = 0; k < TS_PROJ_SIZE; ++k)
                a[i].b[k] = (unsigned char)next();
        const int32_t count = 1 + (int32_t)below(8), i = (int32_t)below((uint32_t)count);
        ts_pj_set_u16(&a[i], TS_PJ_BURST, (uint16_t)below(5));
        if (below(4) == 0) ts_pj_set_u32(&a[i], TS_PJ_SPEED, 0);
        memset(&w, 0, sizeof w);
        w.burstrate = (uint16_t)below(10);
        w.weapontimer = below(2) ? (uint16_t)below(100) : 0;
        w.randomdecay = below(2) ? (uint16_t)below(40) : 0;
        w.sprayangle = below(2) ? (uint16_t)next() : 0;
        w.weaponvelocity = (int32_t)next();
        const uint32_t gt = ts_pj_u32(&a[i], TS_PJ_TIME) + (uint32_t)below(12) - 2u;
        memcpy(b, a, sizeof a);
        const uint64_t seed = rng;
        BURSTLOG la, lb;
        memset(&la, 0, sizeof la);
        memset(&lb, 0, sizeof lb);
        uint32_t da = 0, dbz = 0;
        pool.rec = a; pool.cap = 8; pool.count = count; pool.hw = 8;
        TS_BURST_IO ia = { &la, log_draw, log_fire_point, &da };
        const int32_t ca = ts_proj_burst(&pool, i, &w, gt, &sin, &ia);
        const uint64_t after = rng;
        rng = seed;
        int32_t cb = count;
        const int32_t rb = engine_burst(b, &cb, 8, i, &w, gt, &sin, &lb, &dbz);
        differ += ca != rb || pool.count != cb || memcmp(a, b, sizeof a) != 0 || la.n != lb.n || la.fp != lb.fp ||
                  memcmp(la.log, lb.log, sizeof la.log) != 0 || da != dbz || after != rng;
    }
    CHECK(differ == 0, "the burst against the engine's sequence: %ld of 200000 differ", differ);
}

static void burst_digest(void)
{
    static TS_PROJ a[8];
    TS_PROJPOOL pool;
    TASIM_WEAPONDEF w;
    TS_SIN sin;
    BURSTLOG l;
    uint32_t div0 = 0;
    TS_BURST_IO io = { &l, log_draw, log_fire_point, &div0 };
    ts_sin_table(&sin);
    for (int t = 0; t < 200000; ++t) {
        for (int i = 0; i < 8; ++i)
            for (int k = 0; k < TS_PROJ_SIZE; ++k)
                a[i].b[k] = (unsigned char)next();
        pool.rec = a; pool.cap = 8; pool.count = 1 + (int32_t)below(8); pool.hw = 8;
        const int32_t i = (int32_t)below((uint32_t)pool.count);
        ts_pj_set_u16(&a[i], TS_PJ_BURST, (uint16_t)below(5));
        memset(&w, 0, sizeof w);
        w.burstrate = (uint16_t)below(10);
        w.weapontimer = (uint16_t)(below(2) ? below(100) : 0);
        w.randomdecay = (uint16_t)(below(2) ? below(40) : 0);
        w.sprayangle = (uint16_t)(below(2) ? next() : 0);
        w.weaponvelocity = (int32_t)next();
        memset(&l, 0, sizeof l);
        const uint32_t gt = ts_pj_u32(&a[i], TS_PJ_TIME) + (uint32_t)below(12) - 2u;
        const int32_t c = ts_proj_burst(&pool, i, &w, gt, &sin, &io);
        feed(&c, sizeof c);
        feed(a, sizeof a);
        feed(&l.n, sizeof l.n);
    }
    feed(&div0, sizeof div0);
}

/* ---- damage ------------------------------------------------------------------------------------------------ */

static float rand_float(void)
{
    if (next() & 1) return (float)((double)below(1u << 24) / (double)(1u << 24));   /* a factor or an edge in [0, 1) */
    const uint32_t bits = (uint32_t)next() & 0x7FFFFFFFu;   /* any non-negative float, NaN and infinity included */
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void dmg_rules(void)
{
    CHECK(ts_stricmp("ARMCOM", "armcom") == 0 && ts_stricmp("a", "B") < 0 && ts_stricmp("B", "a") > 0,
          "letters fold");
    CHECK(ts_stricmp("[", "A") < 0 && ts_stricmp("_", "a") < 0 && ts_stricmp("_", "A") < 0,
          "a differing pair is folded before the unsigned compare: '[' and '_' sort below every letter");
    CHECK(ts_stricmp("arm", "armcom") < 0 && ts_stricmp("armcom", "arm") > 0 && ts_stricmp("\xE9", "a") > 0,
          "the end sorts first; bytes past 0x7F are unsigned");

    /* the parser inserts a name equal only without case at its lower bound, so "armcom" before "ARMCOM" */
    const TS_DMGROW rows[] = { { "armcom", 200 }, { "ARMCOM", 100 }, { "corak", 50 }, { "CORE_X", 70000 } };
    CHECK(ts_dmg_base(25, rows, 4, "ArmCom") == 200, "the first row equal without case");
    CHECK(ts_dmg_base(25, rows, 4, "corak") == 50 && ts_dmg_base(25, rows, 4, "core_x") == 70000,
          "a row's full dword");
    CHECK(ts_dmg_base(25, rows, 4, "armc") == 25 && ts_dmg_base(25, rows, 4, "zzz") == 25 &&
          ts_dmg_base(25, rows, 4, "") == 25 && ts_dmg_base(25, NULL, 0, "corak") == 25, "the default");

    CHECK(ts_dmg_scale(100, 0.5f) == 50 && ts_dmg_scale(0x7FFFFFFF, 1.0f) == 0x7FFFFFFF && ts_dmg_scale(-7, 0.5f) == -3,
          "the factor truncates");
    CHECK(ts_vet_level(0) == 0 && ts_vet_level(4) == 0 && ts_vet_level(5) == 1 && ts_vet_level(24) == 4 &&
          ts_vet_level(25) == 5 && ts_vet_level(65535) == 5, "the level");
    CHECK(ts_dmg_shooter(1000, 5) == 1300 && ts_dmg_shooter(20000000, 5) == -16949672 && ts_dmg_shooter(-1000, 2) == -1120,
          "the shooter's level, the product wrapping");
    CHECK(ts_dmg_cheats(7, 0x80) == 14 && ts_dmg_cheats(7, 0x100) == 3 && ts_dmg_cheats(-7, 0x100) == -3 &&
          ts_dmg_cheats(7, 0x180) == 7 && ts_dmg_cheats(7, 0x7E7F) == 7, "the cheat bits 7 and 8 only");
    CHECK(ts_dmg_kind(0x80) == 2 && ts_dmg_kind(0xFFFFFF7Fu) == 1, "the kind");
    CHECK(ts_dmg_angle(100, 0, 0, 0, 0x1000) == 0x3000 && ts_dmg_angle(0, -5, 0, 0, 0) == 0x8000, "the angle");

    /* the hit's word */
    CHECK(ts_hit_word(1000, 1, 0, 0, 0) == 1000 && ts_hit_word(1000, 1, 0, 0, 5) == 800, "the victim's level");
    CHECK(ts_hit_word(1000, 1, 1, 0x8000, 0) == 500 && ts_hit_word(1000, 1, 0, 0x8000, 0) == 1000,
          "armour, a 16.16 factor, only when armoured");
    CHECK(ts_hit_word(29999, 1, 1, 0x10000, 5) == 23999, "below 30 000: armour and the level");
    CHECK(ts_hit_word(30000, 1, 1, 0x8000, 5) == 30000, "kill outright: neither armour nor (B7) the level");
    CHECK(ts_hit_word(40000, 1, 0, 0, 5) == 0x7FFF && ts_hit_word(-40000, 1, 0, 0, 0) == 0x8000,
          "B7 saturates the signed word");
    CHECK(ts_hit_word(40000, 2, 0, 0, 5) == 40000 && ts_hit_word(900, 2, 0, 0, 5) == 720 &&
          ts_hit_word(70000, 2, 0, 0, 0) == 0xFFFF, "a paralyzer's word is unsigned");
    CHECK(ts_hit_word(70000, 10, 1, 0x100, 5) == 0xFFFF && ts_hit_word(-5, 10, 0, 0, 0) == 0 &&
          ts_hit_word(500, 10, 1, 0x100, 5) == 500, "a heal passes untouched, unsigned");
    CHECK(ts_hit_word(-29999999, 1, 0, 0, 0) == 0x7FFF, "the level's product wraps before the divide");

    /* the box distance and the falloff */
    const int32_t u[3] = { 100 << 16, 0, 100 << 16 }, lo[3] = { -(8 << 16), 0, -(8 << 16) },
                  hi[3] = { 8 << 16, 20 << 16, 8 << 16 };
    const int32_t in[3] = { 100 << 16, 10 << 16, 105 << 16 }, off[3] = { 111 << 16, 24 << 16, 100 << 16 },
                  side[3] = { 82 << 16, 0, 100 << 16 };
    CHECK(ts_area_distance(in, u, lo, hi) == 0, "inside the box");
    CHECK(ts_area_distance(off, u, lo, hi) == 5 << 16, "3 px past x, 4 px above: 5 px");
    CHECK(ts_area_distance(side, u, lo, hi) == 10 << 16, "below the box's x");
    CHECK(ts_area_falloff(0, 64, 0.25f) == 1.0f && ts_area_falloff(32, 64, 0.5f) == 0.625f &&
          ts_area_falloff(64, 64, 0.3f) == 0.3f && ts_area_falloff(16, 64, 0.0f) == 0.5625f,
          "the quadratic falloff");
}

static void dmg_digest(void)
{
    char a[6], b[6];
    for (int i = 0; i < 300000; ++i) {
        for (int k = 0; k < 5; ++k) {
            a[k] = (char)(0x40 + below(0x40));
            b[k] = below(4) ? a[k] : (char)(0x40 + below(0x40));
        }
        a[5] = b[5] = 0;
        a[below(6)] = 0;
        const int s = ts_stricmp(a, b);
        const int32_t base = (int32_t)next();
        const unsigned sh = below(16);
        const int32_t amount = (int32_t)next() >> sh;
        const float f = rand_float();
        int32_t r[6];
        r[0] = s;
        r[1] = ts_dmg_scale(base, f);
        r[2] = ts_dmg_shooter(amount, below(8));
        r[3] = ts_dmg_cheats(amount, (uint16_t)next());
        const int kind = (int)below(12), arm = (int)below(2);
        const int32_t armour = (int32_t)next() >> 12;
        r[4] = ts_hit_word(amount, kind, arm, armour, below(8));
        int32_t q[4];
        for (int k = 0; k < 4; ++k) q[k] = (int32_t)next();
        r[5] = ts_dmg_angle(q[0], q[1], q[2], q[3], (uint16_t)next());
        feed(r, sizeof r);
        int32_t at[3], uu[3], bl[3], bh[3];
        for (int k = 0; k < 3; ++k) {
            at[k] = (int32_t)next(); uu[k] = (int32_t)next() >> 4;
            bl[k] = -(int32_t)below(1u << 22); bh[k] = (int32_t)below(1u << 22);
        }
        const int32_t d = ts_area_distance(at, uu, bl, bh);
        const int32_t dpx = (int32_t)below(300) - 20, R = 1 + (int32_t)below(255);
        const float fo = ts_area_falloff(dpx, R, rand_float());
        feed(&d, sizeof d);
        feed(&fo, sizeof fo);
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

/* guidance's cruise distance 0x49B40A..0x49B43D on the x87: fild dx, dy, dz; dx^2 + dy^2, + dz^2; fsqrt; then _ftol
   (stored to a double first: under 0x27F the square root is a double's value). The core must take the same branch:
   far (height 700) or not. */
static int x87_cruise_far(int32_t dx, int32_t dy, int32_t dz)
{
    struct { int32_t dx, dy, dz; uint16_t cw_old, cw_game; double r; } f = { dx, dy, dz, 0, 0x027F, 0.0 };
    __asm__ __volatile__(
        ".intel_syntax noprefix\n\t"
        "fnstcw word ptr [esi+0xC]\n\t" "fldcw word ptr [esi+0xE]\n\t"
        "fild dword ptr [esi+0x0]\n\t" "fild dword ptr [esi+0x4]\n\t" "fild dword ptr [esi+0x8]\n\t"
        "fld st(2)\n\t" "fmul st, st(3)\n\t" "fld st(2)\n\t" "fmul st, st(3)\n\t" "faddp st(1), st\n\t"
        "fld st(1)\n\t" "fmul st, st(2)\n\t" "faddp st(1), st\n\t" "fsqrt\n\t" "fstp qword ptr [esi+0x10]\n\t"
        "fstp st(0)\n\t" "fstp st(0)\n\t" "fstp st(0)\n\t" "fldcw word ptr [esi+0xC]\n\t"
        ".att_syntax prefix\n\t"
        : : "S"(&f) : "memory", "cc");
    return (int16_t)((uint32_t)ts_ftol(f.r) >> 16) > 0x400;
}

static int x87_cruise_check(void)
{
    static TS_PROJ rec[1];
    TS_PROJPOOL pool;
    TASIM_WEAPONDEF w;
    int32_t out[3];
    long bad = 0, far = 0;
    ts_proj_pool_init(&pool, rec, 1);
    memset(&w, 0, sizeof w);
    w.flags = TS_WF_CRUISE;
    for (int i = 0; i < 2000000; ++i) {
        int32_t d[3];
        if (i & 1) {
            for (int k = 0; k < 3; ++k) d[k] = (int32_t)next();   /* squares past 2^53: the rounding the port keeps */
        } else {   /* a radius within 1/8 px of 1025 px, where the branch turns */
            const double r = (1025.0 + ((double)below(65536) / 65536.0 - 0.5) / 4.0) * 65536.0;
            const double a = (double)below(1u << 20) / (double)(1u << 20) * 6.283185307179586;
            const double e = ((double)below(1u << 20) / (double)(1u << 20) - 0.5) * 3.141592653589793;
            d[0] = (int32_t)(r * cos(e) * sin(a));
            d[1] = (int32_t)(r * sin(e));
            d[2] = (int32_t)(r * cos(e) * cos(a));
        }
        TS_PROJ* p = &rec[0];
        set3(p, TS_PJ_POS, d[0], d[1], d[2]);
        set3(p, TS_PJ_TPOS, 0, 0x1234, 0);
        ts_pj_set_u32(p, TS_PJ_QUARRY, 0);
        const int32_t tgt[3] = { 1, 2, 3 };
        ts_proj_guidance(p, &w, &pool, tgt, 0, 0, out);
        const int core = out[0] == 0;
        const int eng = x87_cruise_far(d[0], (int32_t)((uint32_t)d[1] - 0x1234u), d[2]);
        far += eng;
        if (core != eng && ++bad <= 10)
            printf("FAIL cruise (%d, %d, %d): x87 %d, core %d\n", d[0], d[1], d[2], eng, core);
    }
    printf("x87: cruise %ld of 2000000 equal (%ld far)\n", 2000000 - bad, far);
    return bad != 0;
}

/* the damage's three x87 sequences, transcribed from the listing and run under the game's control word 0x27F:
   the factor 0x499D73..0x499D77 (fild DWORD, fmul DWORD), the box distance 0x49A35A..0x49A388 (three filds, the
   squares and sums in the listing's stack order, fsqrt) and the falloff 0x49A3B6..0x49A3E0 (fdivp st(2), st written
   as its bytes DE FA so the assembler cannot swap it). The two _ftol calls take the x87's value, which under 0x27F is
   a double's, stored first. */
typedef struct {
    int32_t i0, i1, i2;       /* 0x00 0x04 0x08                                   */
    float f0;                 /* 0x0C                                             */
    float one;                /* 0x10                                             */
    uint16_t cw_old, cw_game; /* 0x14 0x16                                        */
    double r;                 /* 0x18                                             */
    float rf;                 /* 0x20                                             */
    double l20, l18;          /* 0x28 0x30: the distance's two stored gaps        */
} X87DMG;

static int32_t x87_scale(int32_t base, float factor)
{
    X87DMG f = { base, 0, 0, factor, 1.0f, 0, 0x027F, 0.0, 0.0f, 0.0, 0.0 };
    __asm__ __volatile__(
        ".intel_syntax noprefix\n\t"
        "fnstcw word ptr [esi+0x14]\n\t" "fldcw word ptr [esi+0x16]\n\t"
        "fild dword ptr [esi+0x0]\n\t" "fmul dword ptr [esi+0xC]\n\t" "fstp qword ptr [esi+0x18]\n\t"
        "fldcw word ptr [esi+0x14]\n\t"
        ".att_syntax prefix\n\t"
        : : "S"(&f) : "memory", "cc");
    return ts_ftol(f.r);
}

static int32_t x87_distance(int32_t gx, int32_t gy, int32_t gz)
{
    X87DMG f = { gx, gy, gz, 0.0f, 1.0f, 0, 0x027F, 0.0, 0.0f, 0.0, 0.0 };
    __asm__ __volatile__(
        ".intel_syntax noprefix\n\t"
        "fnstcw word ptr [esi+0x14]\n\t" "fldcw word ptr [esi+0x16]\n\t"
        "fild dword ptr [esi+0x0]\n\t" "fild dword ptr [esi+0x4]\n\t" "fild dword ptr [esi+0x8]\n\t"
        "fxch st(2)\n\t" "fst qword ptr [esi+0x28]\n\t" "fxch st(1)\n\t" "fst qword ptr [esi+0x30]\n\t"
        "fxch st(1)\n\t" "fmul qword ptr [esi+0x28]\n\t" "fxch st(1)\n\t" "fmul qword ptr [esi+0x30]\n\t"
        "fld st(2)\n\t" "fmul st, st(3)\n\t" "fxch st(1)\n\t" "faddp st(2), st\n\t" "faddp st(1), st\n\t"
        "fsqrt\n\t" "fstp qword ptr [esi+0x18]\n\t" "fstp st(0)\n\t"
        "fldcw word ptr [esi+0x14]\n\t"
        ".att_syntax prefix\n\t"
        : : "S"(&f) : "memory", "cc");
    return ts_ftol(f.r);
}

static float x87_falloff(int32_t dpx, int32_t R, float e)
{
    X87DMG f = { dpx, R, 0, e, 1.0f, 0, 0x027F, 0.0, 0.0f, 0.0, 0.0 };
    __asm__ __volatile__(
        ".intel_syntax noprefix\n\t"
        "fnstcw word ptr [esi+0x14]\n\t" "fldcw word ptr [esi+0x16]\n\t"
        "fild dword ptr [esi+0x0]\n\t" "fild dword ptr [esi+0x4]\n\t" "fld dword ptr [esi+0x10]\n\t"
        "fsub dword ptr [esi+0xC]\n\t" "fxch st(1)\n\t" ".byte 0xDE, 0xFA\n\t" "fxch st(1)\n\t"
        "fsub dword ptr [esi+0x10]\n\t" "fxch st(1)\n\t" "fld st(1)\n\t" "fmulp st(2), st\n\t" "fmulp st(1), st\n\t"
        "fadd dword ptr [esi+0xC]\n\t" "fstp dword ptr [esi+0x20]\n\t"
        "fldcw word ptr [esi+0x14]\n\t"
        ".att_syntax prefix\n\t"
        : : "S"(&f) : "memory", "cc");
    return f.rf;
}

static int same_f(float a, float b) { return (isnan(a) && isnan(b)) || memcmp(&a, &b, 4) == 0; }

static int x87_dmg_check(void)
{
    long bad = 0;
    const long N = 2000000;
    for (long i = 0; i < N; ++i) {
        const int32_t base = (next() & 1) ? (int32_t)below(65536) : (int32_t)next();
        const float fac = rand_float();
        const int32_t e = x87_scale(base, fac), c = ts_dmg_scale(base, fac);
        if (e != c && ++bad <= 10) printf("FAIL scale(%d, %a): x87 %d, core %d\n", base, (double)fac, e, c);
    }
    printf("x87: damage factor %ld of %ld equal\n", N - bad, N);
    long bad2 = 0;
    for (long i = 0; i < N; ++i) {
        /* the gaps as the box gives them: inside (0), and any wrap of two 16.16 coordinates */
        int32_t at[3], u[3], lo[3], hi[3], g[3];
        for (int k = 0; k < 3; ++k) {
            at[k] = (next() & 1) ? (int32_t)next() : (int32_t)(below(1u << 28) - (1u << 27));
            u[k] = (int32_t)(below(1u << 28) - (1u << 27));
            lo[k] = -(int32_t)below(64u << 16);
            hi[k] = (int32_t)below(64u << 16);
            const int32_t a = (int32_t)((uint32_t)lo[k] + (uint32_t)u[k]), b = (int32_t)((uint32_t)hi[k] + (uint32_t)u[k]);
            g[k] = at[k] < a ? (int32_t)((uint32_t)a - (uint32_t)at[k])
                 : at[k] > b ? (int32_t)((uint32_t)at[k] - (uint32_t)b) : 0;
        }
        const int32_t e = x87_distance(g[0], g[1], g[2]), c = ts_area_distance(at, u, lo, hi);
        if (e != c && ++bad2 <= 10) printf("FAIL distance (%d, %d, %d): x87 %d, core %d\n", g[0], g[1], g[2], e, c);
    }
    printf("x87: box distance %ld of %ld equal\n", N - bad2, N);
    long bad3 = 0;
    for (long i = 0; i < N; ++i) {
        const int32_t R = 1 + (int32_t)below(1024), dpx = 1 + (int32_t)below((uint32_t)R + 20);
        const float edge = rand_float();
        const float e = x87_falloff(dpx, R, edge), c = ts_area_falloff(dpx, R, edge);
        if (!same_f(e, c) && ++bad3 <= 10)
            printf("FAIL falloff(%d, %d, %a): x87 %a, core %a\n", dpx, R, (double)edge, (double)e, (double)c);
    }
    printf("x87: falloff %ld of %ld equal\n", N - bad3, N);
    return bad != 0 || bad2 != 0 || bad3 != 0;
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
        flight_digest();
        burst_digest();
        dmg_digest();
        printf("digest %016llx\n", (unsigned long long)fnv);
        return 0;
    }
#if defined(__i386__)
    if (argc > 1 && !strcmp(argv[1], "x87")) {
        const int bad = x87_pitch_check();
        const int bad2 = x87_cruise_check();
        return x87_dmg_check() | bad | bad2;
    }
#endif
    pool_rules();
    pitch_rules();
    flight_rules();
    burst_rules();
    dmg_rules();
    printf(fails ? "tasim_combat_test: %d failed\n" : "tasim_combat_test: ok\n", fails);
    return fails != 0;
}
