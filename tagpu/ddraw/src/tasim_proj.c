/* tasim_proj.c -- the engine's projectile record and pool (tasim_proj.h). Each function's engine reading is at its
   declaration. */
#include <string.h>
#include "tasim_proj.h"

int ts_proj_pool_init(TS_PROJPOOL* pool, TS_PROJ* rec, int32_t cap)
{
    if (cap < 1 || cap > TS_PROJ_CAP_MAX) return -1;
    memset(rec, 0, (size_t)cap * sizeof *rec);
    pool->rec = rec;
    pool->cap = cap;
    pool->count = 0;
    pool->hw = 0;
    return 0;
}

TS_PROJ* ts_proj_append(TS_PROJPOOL* pool)
{
    TS_PROJ* p;
    if (pool->count >= pool->cap) return NULL;
    p = &pool->rec[pool->count++];
    if (pool->count > pool->hw) pool->hw = pool->count;
    p->b[TS_PJ_FLAGS] &= (uint8_t)~TS_PJF_DONE;
    ts_pj_set_u32(p, TS_PJ_TARGET, 0);
    return p;
}

/* the place in [0, n) whose stamp is `old`, or -1: the survivors' stamps ascend after the move */
static int32_t survivor_at(const TS_PROJPOOL* pool, int32_t n, uint32_t old)
{
    int32_t lo = 0, hi = n;
    while (lo < hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        const uint32_t s = ts_pj_u16(&pool->rec[mid], TS_PJ_STAMP);
        if (s == old) return mid;
        if (s < old) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

void ts_proj_compact(TS_PROJPOOL* pool)
{
    const int32_t count = pool->count;
    int32_t n = count, first = -1, dst = -1, i;
    for (i = 0; i < count; ++i) {
        TS_PROJ* r = &pool->rec[i];
        ts_pj_set_u16(r, TS_PJ_STAMP, (uint16_t)i);
        if (r->b[TS_PJ_FLAGS] & TS_PJF_DONE) {
            if (dst < 0) dst = first = i;
            --n;
            continue;
        }
        if (dst < 0) continue;
        pool->rec[dst++] = *r;
    }
    /* the movers are [first, n): every survivor whose old index lies past the first done record. THE BOUND: a
       quarry index is compared with stamps, which are below the old count, and never dereferenced here. */
    if (first >= 0)
        for (i = first; i < n; ++i) {
            const uint32_t q = ts_pj_u32(&pool->rec[i], TS_PJ_QUARRY);
            int32_t at;
            if (!q) continue;
            at = survivor_at(pool, n, q - 1u);
            if (at >= 0) ts_pj_set_u32(&pool->rec[i], TS_PJ_QUARRY, (uint32_t)at + 1u);
        }
    pool->count = n;
}
