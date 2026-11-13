/* tasim_combat_test.c -- the self-test of the core's combat pieces that stand alone: the projectile record and pool
   (src/tasim_proj.h). Driven by tests/tasim_combat_test.py, which builds it natively, under the sanitizers and with
   the DLL's i686 compiler under Wine.

     tasim_combat_test           the rules on hand-checked cases and against literal ports of the engine's
                                 instruction sequences on random input; exit 1 on a failure
     tasim_combat_test digest    one 64-bit FNV-1a digest of every function on arguments from a fixed generator; the
                                 driver requires every build to print the same digest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "digest")) {
        pool_digest();
        printf("digest %016llx\n", (unsigned long long)fnv);
        return 0;
    }
    pool_rules();
    printf(fails ? "tasim_combat_test: %d failed\n" : "tasim_combat_test: ok\n", fails);
    return fails != 0;
}
