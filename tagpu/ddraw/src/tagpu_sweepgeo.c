/* tagpu_sweepgeo.c -- the reclaim sweep's geometry and split (tagpu_sweepgeo.h). */
#include <math.h>
#include <string.h>
#include "tagpu_sweepgeo.h"

/* Squared distances are taken in uint64: callers keep positions within +-2^30 world px (the
   packet bounds every world coordinate to +-2^20), so each axis's square is below 2^62 and
   their sum below 2^63. */
static uint64_t dist2(int32_t ax, int32_t az, int32_t bx, int32_t bz)
{
    int64_t dx = (int64_t)ax - bx, dz = (int64_t)az - bz;
    return (uint64_t)(dx * dx) + (uint64_t)(dz * dz);
}

int sweep_inside(int32_t cx, int32_t cz, int32_t r, int32_t x, int32_t z)
{
    int64_t dx = (int64_t)x - cx, dz = (int64_t)z - cz;
    /* the axis test first keeps every product below 2^62 for any int32 input */
    if (r < 0 || dx > r || dx < -(int64_t)r || dz > r || dz < -(int64_t)r) return 0;
    return dx * dx + dz * dz <= (int64_t)r * r;
}

/* the order's key: nearer first, then z, then x, then the index */
static int before(const SweepTarget* t, int32_t cx, int32_t cz, uint16_t a, uint16_t b)
{
    uint64_t da = dist2(t[a].x, t[a].z, cx, cz), db = dist2(t[b].x, t[b].z, cx, cz);
    if (da != db) return da < db;
    if (t[a].z != t[b].z) return t[a].z < t[b].z;
    if (t[a].x != t[b].x) return t[a].x < t[b].x;
    return a < b;
}

void sweep_order(const SweepTarget* t, unsigned n, int32_t cx, int32_t cz, uint16_t* idx)
{
    uint16_t tmp[SWEEP_MAX_TARGETS];
    unsigned i, width;
    if (n > SWEEP_MAX_TARGETS) n = SWEEP_MAX_TARGETS;
    for (i = 0; i < n; i++) idx[i] = (uint16_t)i;
    /* bottom-up merge sort: the key is a total order, so the result is the one order */
    for (width = 1; width < n; width *= 2) {
        unsigned lo;
        for (lo = 0; lo < n; lo += 2 * width) {
            unsigned mid = lo + width < n ? lo + width : n;
            unsigned hi = lo + 2 * width < n ? lo + 2 * width : n;
            unsigned a = lo, b = mid, k = lo;
            while (a < mid && b < hi)
                tmp[k++] = before(t, cx, cz, idx[b], idx[a]) ? idx[b++] : idx[a++];
            while (a < mid) tmp[k++] = idx[a++];
            while (b < hi) tmp[k++] = idx[b++];
        }
        memcpy(idx, tmp, n * sizeof *idx);
    }
}

/* The Reclaim handler 0x404AD0 sets the order's countdown +0x36 to _ftol(15 + (energy + metal)
   / 2), truncated (0x404BBA..0x404BDC: `fmul` by -0.5 at 0x4FC940, `fsubr` 15.0 at 0x4FC944);
   each working call re-arms 2 ticks (0x439E80(2) at 0x404C77) and takes 2 off it (0x404C7C..
   0x404C84); the call that takes it to 0 or below ends its state (result 1, 0x404C8B) and the
   call after it, 2 ticks on, pays out (state 5, 0x404D66). So C0 = trunc(15 + (e + m) / 2) is
   paid 2 * ceil(C0 / 2) ticks after the first call; no worker-time term enters it (DISASSEMBLED). */
uint32_t sweep_reclaim_ticks(float energy, float metal)
{
    double e = energy > 0.0f ? (double)energy : 0.0;
    double m = metal > 0.0f ? (double)metal : 0.0;
    double c0 = floor(15.0 + (e + m) / 2.0);
    double v = 2.0 * ceil(c0 / 2.0);
    return v >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)v;
}

unsigned sweep_split(SweepBuilder* b, unsigned nb, const SweepTarget* t, unsigned nt,
                     uint16_t* who, uint16_t* seq)
{
    unsigned char taken[SWEEP_MAX_TARGETS];
    /* A builder that cannot reach the nearest target left reaches none: targets only leave and
       its point moves only when it takes one. It is marked once rather than tested again. */
    unsigned char stuck[65536u / 8u];
    unsigned left, m = 0, i, k;
    if (nt > SWEEP_MAX_TARGETS) nt = SWEEP_MAX_TARGETS;
    if (nb > 65535u) nb = 65535u;
    memset(taken, 0, nt);
    memset(stuck, 0, (nb + 7u) / 8u);
    for (left = nt; left; ) {
        SweepBuilder* s;
        int best = -1, near = -1;
        uint64_t bd = 0;
        double d, travel;
        for (i = 0; i < nb; i++)
            if (b[i].room && !(stuck[i >> 3] & (1u << (i & 7))) &&
                (best < 0 || b[i].load < b[best].load)) best = (int)i;
        if (best < 0) break;
        s = &b[best];
        for (k = 0; k < nt; k++) {
            uint64_t dk;
            if (taken[k]) continue;
            dk = dist2(s->x, s->z, t[k].x, t[k].z);
            if (near < 0 || dk < bd) { near = (int)k; bd = dk; }
        }
        d = sqrt((double)bd);
        if (!(s->speed > 0.0)) {
            if (!(d <= s->reach)) { stuck[best >> 3] |= (unsigned char)(1u << (best & 7)); continue; }
            travel = 0.0;
        } else travel = d > s->reach ? (d - s->reach) / s->speed : 0.0;
        s->load += travel + (double)t[near].ticks;
        s->x = t[near].x; s->z = t[near].z;
        s->room--;
        taken[near] = 1; left--;
        who[m] = (uint16_t)best; seq[m] = (uint16_t)near; m++;
    }
    return m;
}
