/* tagpu_formgeo.c -- the geometry of a formation move (tagpu_formgeo.h). Integer only: the
   Python port in tools/formation_regression.py is this file line for line, and its --selftest
   compares the two through tests/formtest.py dump.

   The scratch below is static (the cost matrix alone is 264 KB), so one caller at a time: the
   game thread, from tagpu_formation.c. */
#include <stdlib.h>
#include "tagpu_formgeo.h"

/* above every reduced cost: |a - u - v| <= (2n + 1) * FORM_COST_MAX < 2^24 */
#define HUNG_INF 0x3FFFFFFF

/* floor(v / 65536) for any sign. C leaves `>>` of a negative value to the implementation, and
   the port's `>>` floors, so the floor is spelled out. |v| < 2^63 - 0xFFFF at every caller. */
static int64_t floor16(int64_t v)
{
    return v >= 0 ? v >> 16 : -((-v + 0xFFFF) >> 16);
}

uint64_t form_isqrt64(uint64_t v)
{
    /* Digit by digit, two bits of `v` per step: r stays below 2^32, so `r + bit` cannot wrap. */
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* floor(|b - a|) in 16.16 over x and z: each delta below 2^31, the sum of squares below 2^63. */
static int64_t span(FormPt a, FormPt b)
{
    const int64_t dx = (int64_t)b.x - a.x, dz = (int64_t)b.z - a.z;
    return (int64_t)form_isqrt64((uint64_t)(dx * dx + dz * dz));
}

unsigned form_path_add(FormPt* node, unsigned m, FormPt p)
{
    unsigned k, w;
    if (m && node[m - 1].x == p.x && node[m - 1].z == p.z) return m;
    if (m >= FORM_MAX_NODES) {
        /* in place: w <= k at every copy */
        w = 0;
        for (k = 0; k + 1 < m; k += 2) node[w++] = node[k];
        node[w++] = node[m - 1];
        m = w;
    }
    node[m] = p;
    return m + 1;
}

int64_t form_path_length(const FormPt* node, unsigned m)
{
    int64_t L = 0;
    unsigned k;
    for (k = 1; k < m; k++) L += span(node[k - 1], node[k]);
    return L;
}

/* a + (b - a) * frac / 65536, floored; frac in [0, 65536], so the product is below 2^48 and
   frac == 65536 gives b exactly. */
static int32_t lerp(int32_t a, int32_t b, int64_t frac)
{
    return (int32_t)(a + floor16(((int64_t)b - a) * frac));
}

void form_slots(const FormPt* node, unsigned m, unsigned n, FormPt* out)
{
    int64_t L, t, d0, d1, seg, frac;
    unsigned k, e;
    if (m == 0) return;
    if (m == 1 || n < 2) {
        for (k = 0; k < n; k++) out[k] = node[0];
        return;
    }
    L = form_path_length(node, m);
    /* d0, d1: the arc length at node[e-1] and node[e]; t only grows, so e only advances */
    e = 1;
    d0 = 0;
    d1 = span(node[0], node[1]);
    for (k = 0; k < n; k++) {
        t = L * (int64_t)k / (int64_t)(n - 1);           /* L < 2^44, k < 2^16: below 2^60 */
        while (e < m - 1 && d1 < t) {
            e++;
            d0 = d1;
            d1 += span(node[e - 1], node[e]);
        }
        seg = d1 - d0;
        frac = seg ? ((t - d0) << 16) / seg : 0;         /* t - d0 <= L < 2^44 */
        if (frac < 0) frac = 0;
        if (frac > 65536) frac = 65536;
        out[k].x = lerp(node[e - 1].x, node[e].x, frac);
        out[k].alt = lerp(node[e - 1].alt, node[e].alt, frac);
        out[k].z = lerp(node[e - 1].z, node[e].z, frac);
    }
}

int32_t form_cost(FormPt a, FormPt b)
{
    /* (floor(s) + 0.5 wu) >> 16 == floor(s / 65536 + 0.5): adding a whole number commutes
       with the floor */
    const int64_t wu = (span(a, b) + 0x8000) >> 16;
    return wu > FORM_COST_MAX ? FORM_COST_MAX : (int32_t)wu;
}

/* ---- the optimal assignment ---------------------------------------------------------------- */

static int32_t s_cost[FORM_HUNGARIAN_MAX + 1][FORM_HUNGARIAN_MAX + 1];

void form_hungarian(const FormPt* start, const FormPt* slot, unsigned n, uint16_t* slot_of)
{
    int32_t u[FORM_HUNGARIAN_MAX + 1], v[FORM_HUNGARIAN_MAX + 1], minv[FORM_HUNGARIAN_MAX + 1];
    unsigned p[FORM_HUNGARIAN_MAX + 1], way[FORM_HUNGARIAN_MAX + 1];
    unsigned char used[FORM_HUNGARIAN_MAX + 1];
    unsigned i, j, i0, j0, j1;
    int32_t delta, cur;

    if (n > FORM_HUNGARIAN_MAX) {
        form_nox(start, slot, n, slot_of);
        return;
    }
    /* row i: unit i-1; column j: slot j-1; row and column 0 are the algorithm's sentinel */
    for (i = 1; i <= n; i++)
        for (j = 1; j <= n; j++) s_cost[i][j] = form_cost(start[i - 1], slot[j - 1]);
    for (j = 0; j <= n; j++) { u[j] = 0; v[j] = 0; p[j] = 0; way[j] = 0; }

    /* The O(n^3) shortest-augmenting-path form with potentials: row i joins through column 0,
       then the cheapest reduced column is taken until a free one is reached. Every potential
       stays within n * FORM_COST_MAX < 2^23, so int32 holds every sum below. The strict `<`
       keeps the first (lowest) column of a tie. */
    for (i = 1; i <= n; i++) {
        p[0] = i;
        j0 = 0;
        for (j = 0; j <= n; j++) { minv[j] = HUNG_INF; used[j] = 0; }
        do {
            used[j0] = 1;
            i0 = p[j0];
            delta = HUNG_INF;
            j1 = 0;
            for (j = 1; j <= n; j++) {
                if (used[j]) continue;
                cur = s_cost[i0][j] - u[i0] - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = j; }
            }
            for (j = 0; j <= n; j++) {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else minv[j] -= delta;
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }
    for (j = 1; j <= n; j++) slot_of[p[j] - 1] = (uint16_t)(j - 1);
}

/* ---- the bounded assignment above FORM_HUNGARIAN_MAX --------------------------------------- */

typedef struct { int32_t x, z; } WuPt;            /* whole world units: x, z in [0, 32767] */
typedef struct { int64_t key; unsigned idx; } SortKey;

static WuPt s_p[FORM_MAX_UNITS], s_q[FORM_MAX_UNITS];   /* the starts and the slots, in wu */
static SortKey s_kp[FORM_MAX_UNITS], s_kq[FORM_MAX_UNITS];

static void to_wu(const FormPt* in, unsigned n, WuPt* out)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        out[i].x = (int32_t)floor16(in[i].x);
        out[i].z = (int32_t)floor16(in[i].z);
    }
}

/* (key, index): a total order, so qsort's result does not depend on its algorithm */
static int by_key(const void* pa, const void* pb)
{
    const SortKey* a = pa;
    const SortKey* b = pb;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->idx < b->idx ? -1 : a->idx > b->idx;
}

/* The furthest pair's squared distance and direction, strict `>`: the first pair wins a tie.
   A wu delta is below 2^16, so each square is below 2^32 and int64 holds their sum. */
static void furthest(const WuPt* pt, unsigned n, int64_t* best, int64_t* dx, int64_t* dz)
{
    unsigned i, j;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++) {
            const int64_t ex = (int64_t)pt[j].x - pt[i].x, ez = (int64_t)pt[j].z - pt[i].z;
            const int64_t d2 = ex * ex + ez * ez;
            if (d2 > *best) { *best = d2; *dx = ex; *dz = ez; }
        }
}

/* Units and slots sorted along the direction of the furthest pair (the starts first, then the
   slots; (1, 0) when every point coincides) and paired in order. */
static void nox_pair(unsigned n, uint16_t* slot_of)
{
    int64_t best = 0, dx = 1, dz = 0;
    unsigned k;
    furthest(s_p, n, &best, &dx, &dz);
    furthest(s_q, n, &best, &dx, &dz);
    /* |x| <= 2^15 and |dx| < 2^16: the key is below 2^32 in magnitude */
    for (k = 0; k < n; k++) {
        s_kp[k].key = s_p[k].x * dx + s_p[k].z * dz;
        s_kp[k].idx = k;
        s_kq[k].key = s_q[k].x * dx + s_q[k].z * dz;
        s_kq[k].idx = k;
    }
    qsort(s_kp, n, sizeof s_kp[0], by_key);
    qsort(s_kq, n, sizeof s_kq[0], by_key);
    for (k = 0; k < n; k++) slot_of[s_kp[k].idx] = (uint16_t)s_kq[k].idx;
}

/* (b - a) x (c - a): wu deltas below 2^16, so each product is below 2^32 */
static int64_t orient(WuPt a, WuPt b, WuPt c)
{
    return ((int64_t)b.x - a.x) * ((int64_t)c.z - a.z) -
           ((int64_t)b.z - a.z) * ((int64_t)c.x - a.x);
}

static int opposite(int64_t a, int64_t b)
{
    return (a > 0 && b < 0) || (a < 0 && b > 0);
}

/* A proper crossing only: each segment's ends strictly on opposite sides of the other's line.
   A touching or collinear pair is not one, so every swap strictly shortens the two paths. */
static int crosses(WuPt p1, WuPt q1, WuPt p2, WuPt q2)
{
    return opposite(orient(p1, q1, p2), orient(p1, q1, q2)) &&
           opposite(orient(p2, q2, p1), orient(p2, q2, q1));
}

/* At most FORM_UNCROSS_PASSES passes over every pair i < j, and FORM_UNCROSS_WORK crossing tests
   in all; a pass that swaps nothing ends it. Returns the tests run. */
static uint32_t nox_uncross(unsigned n, uint16_t* slot_of)
{
    uint32_t tests = 0;
    unsigned pass, i, j, swaps;
    uint16_t s;
    for (pass = 0; pass < FORM_UNCROSS_PASSES; pass++) {
        swaps = 0;
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++) {
                if (tests == FORM_UNCROSS_WORK) return tests;
                tests++;
                if (crosses(s_p[i], s_q[slot_of[i]], s_p[j], s_q[slot_of[j]])) {
                    s = slot_of[i];
                    slot_of[i] = slot_of[j];
                    slot_of[j] = s;
                    swaps++;
                }
            }
        if (!swaps) break;
    }
    return tests;
}

void form_nox(const FormPt* start, const FormPt* slot, unsigned n, uint16_t* slot_of)
{
    unsigned i;
    if (n > FORM_MAX_UNITS) {
        for (i = 0; i < n; i++) slot_of[i] = (uint16_t)i;
        return;
    }
    to_wu(start, n, s_p);
    to_wu(slot, n, s_q);
    nox_pair(n, slot_of);
    nox_uncross(n, slot_of);
}

int form_assign(const FormPt* start, const FormPt* slot, unsigned n, uint16_t* slot_of)
{
    if (n <= FORM_HUNGARIAN_MAX) {
        form_hungarian(start, slot, n, slot_of);
        return 'h';
    }
    form_nox(start, slot, n, slot_of);
    return 'x';
}
