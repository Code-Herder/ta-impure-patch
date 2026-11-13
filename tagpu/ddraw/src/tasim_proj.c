/* tasim_proj.c -- the engine's projectile record and pool (tasim_proj.h). Each function's engine reading is at its
   declaration. */
#include <math.h>
#include <string.h>
#include "tasim_proj.h"
#include "tasim_fixed.h"

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

static void get3(const TS_PROJ* p, unsigned off, int32_t v[3])
{
    v[0] = ts_pj_i32(p, off);
    v[1] = ts_pj_i32(p, off + 4);
    v[2] = ts_pj_i32(p, off + 8);
}

static void set3(TS_PROJ* p, unsigned off, const int32_t v[3])
{
    ts_pj_set_i32(p, off, v[0]);
    ts_pj_set_i32(p, off + 4, v[1]);
    ts_pj_set_i32(p, off + 8, v[2]);
}

static int32_t wsub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

void ts_proj_guidance(TS_PROJ* p, const TASIM_WEAPONDEF* w, const TS_PROJPOOL* pool, const int32_t* target_pos,
                      int32_t ground, uint8_t sea, int32_t out[3])
{
    const int cruise = (w->flags & TS_WF_CRUISE) != 0;
    int32_t pos[3], tp[3];
    uint32_t q;
    get3(p, TS_PJ_POS, pos);
    get3(p, TS_PJ_TPOS, tp);
    if (cruise) {
        const double dx = (double)wsub(pos[0], tp[0]), dy = (double)wsub(pos[1], tp[1]), dz = (double)wsub(pos[2], tp[2]);
        const int32_t d = ts_ftol(sqrt((dx * dx + dy * dy) + dz * dz));
        if ((int16_t)((uint32_t)d >> 16) > 0x400) {
            set3(p, TS_PJ_TAIL, tp);
            ts_pj_set_u16(p, TS_PJ_TAIL + 6, 0x2BC);
            get3(p, TS_PJ_TAIL, out);
            return;
        }
    }
    q = ts_pj_u32(p, TS_PJ_QUARRY);
    if (q && q - 1u < (uint32_t)pool->cap) {
        get3(&pool->rec[q - 1u], TS_PJ_POS, out);
        return;
    }
    if (target_pos) {
        out[0] = target_pos[0];
        out[1] = target_pos[1];
        out[2] = target_pos[2];
        return;
    }
    if (cruise) {
        tp[1] = (int32_t)((uint32_t)(ground > (int32_t)sea ? ground : (int32_t)sea) << 16);
        set3(p, TS_PJ_TAIL, tp);
        get3(p, TS_PJ_TAIL, out);
        return;
    }
    get3(p, TS_PJ_TPOS, out);
}

/* one of steer's two turns: answers 0 for a burnblow detonation, else stores the new angle */
static int turn(TS_PROJ* p, unsigned off, int32_t want, uint16_t tr, int burnblow)
{
    const uint16_t cur = ts_pj_u16(p, off);
    const int16_t e = (int16_t)(uint16_t)((uint32_t)want - cur);
    const int16_t a = (int16_t)(uint16_t)(e < 0 ? -(int32_t)e : (int32_t)e);
    if (a > 0x6978 && burnblow) return 0;
    if ((int32_t)a < (int32_t)tr) ts_pj_set_u16(p, off, (uint16_t)want);
    else if (e < 0) ts_pj_set_u16(p, off, (uint16_t)(cur - tr));
    else ts_pj_set_u16(p, off, (uint16_t)(cur + tr));
    return 1;
}

int ts_proj_steer(TS_PROJ* p, const TASIM_WEAPONDEF* w, const int32_t t[3])
{
    const int burnblow = (w->flags & TS_WF_BURNBLOW) != 0;
    const int32_t dx = wsub(ts_pj_i32(p, TS_PJ_POS), t[0]), dy = wsub(ts_pj_i32(p, TS_PJ_POS + 4), t[1]);
    const int32_t dz = wsub(ts_pj_i32(p, TS_PJ_POS + 8), t[2]);
    const int32_t hw = ts_angle(dx, dz);
    const int32_t hd = ts_ftol(ts_hypot(dx, dz));
    const int32_t pw = ts_angle(-(int32_t)(int16_t)((uint32_t)dy >> 16), (int32_t)(int16_t)((uint32_t)hd >> 16));
    if (!turn(p, TS_PJ_HEADING, hw, w->turnrate, burnblow)) return 0;
    return turn(p, TS_PJ_PITCH, pw, w->turnrate, burnblow);
}

void ts_proj_drift(TS_PROJ* p, const int32_t wind[3], int32_t gravity)
{
    for (unsigned k = 0; k < 3; ++k)
        ts_pj_set_i32(p, TS_PJ_POS + 4 * k, wadd(ts_pj_i32(p, TS_PJ_POS + 4 * k), ts_pj_i32(p, TS_PJ_VEL + 4 * k)));
    for (unsigned k = 0; k < 3; ++k)
        ts_pj_set_i32(p, TS_PJ_POS + 4 * k, wadd(ts_pj_i32(p, TS_PJ_POS + 4 * k), wind[k]));
    ts_pj_set_i32(p, TS_PJ_VEL + 4, wsub(ts_pj_i32(p, TS_PJ_VEL + 4), gravity));
}
