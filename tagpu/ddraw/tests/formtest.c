/* formtest.c -- the self-test of tagpu_formgeo.c, for tests/formtest.py.

   `formtest` checks hand-worked cases (each worked out in the comment above it) and then
   properties over random ones. It prints one line per failure and a summary; exit 0 when
   nothing failed.

   `formtest dump` reads cases on stdin, whitespace-separated, and prints one line per case, for
   tools/formation_regression.py --selftest:
     add K  x alt z ...                  -> "m L" and " x,alt,z" per node, the K points appended
                                            one by one to an empty path
     slots M N  x alt z ...              -> "x,alt,z" per slot, N slots on the M nodes
     hung|nox|assign N  sx sz ... qx qz ... -> "total s0 s1 ...", N starts then N slots (x and z,
                                            16.16); `assign` puts its 'h' or 'x' first

   `formtest bench` times the Hungarian at n = 256 and the bounded assignment at n = 1500.

   The file includes tagpu_formgeo.c rather than linking it, so the checks can reach its static
   uncross pass and count the tests it runs. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L   /* clock_gettime */
#endif
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include "../src/tagpu_formgeo.c"

#define WU(v) ((int32_t)(v) * 65536)

static unsigned s_checks, s_failed;
static FormPt s_node[FORM_MAX_NODES], s_slot[FORM_MAX_UNITS], s_start[FORM_MAX_UNITS];
static uint16_t s_as[FORM_MAX_UNITS];

static void check(int ok, const char* what)
{
    s_checks++;
    if (!ok) {
        s_failed++;
        printf("FAIL %s\n", what);
    }
}

static FormPt pt(int32_t x, int32_t alt, int32_t z)
{
    FormPt p;
    p.x = x; p.alt = alt; p.z = z;
    return p;
}

static int same(FormPt a, FormPt b) { return a.x == b.x && a.alt == b.alt && a.z == b.z; }

/* xorshift64*: a fixed sequence, so every run checks the same cases */
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    s_rng ^= s_rng >> 12;
    s_rng ^= s_rng << 25;
    s_rng ^= s_rng >> 27;
    return s_rng * 0x2545F4914F6CDD1Dull;
}
static int32_t rnd_below(uint32_t n) { return (int32_t)(rnd() % n); }

static int is_perm(const uint16_t* as, unsigned n)
{
    static unsigned char seen[FORM_MAX_UNITS];
    unsigned i;
    if (n > FORM_MAX_UNITS) return 0;
    memset(seen, 0, n);
    for (i = 0; i < n; i++) {
        if (as[i] >= n || seen[as[i]]) return 0;
        seen[as[i]] = 1;
    }
    return 1;
}

static int64_t total(const FormPt* start, const FormPt* slot, unsigned n, const uint16_t* as)
{
    int64_t t = 0;
    unsigned i;
    for (i = 0; i < n; i++) t += form_cost(start[i], slot[as[i]]);
    return t;
}

/* ---- the hand-worked checks ---------------------------------------------------------------- */

static uint64_t isqrt_ref(uint64_t v)
{
    uint64_t r = (uint64_t)sqrtl((long double)v);
    if (r > 0xFFFFFFFFull) r = 0xFFFFFFFFull;
    while (r * r > v) r--;
    while (r < 0xFFFFFFFFull && (r + 1) * (r + 1) <= v) r++;
    return r;
}

static void isqrt_checks(void)
{
    static const uint64_t edge[] = { 0, 1, 2, 3, 4, 5, 8, 9, 15, 16, 17, 0xFFFFFFFFull,
                                     (uint64_t)1 << 62, ((uint64_t)1 << 63) - 1,
                                     (uint64_t)1 << 63, ~(uint64_t)0 };
    static const uint64_t root[] = { 1, 2, 3, 0xFFFF, 0x10000, 0xB504F333ull, 0xFFFFFFFEull,
                                     0xFFFFFFFFull };
    char what[96];
    unsigned i, bad = 0;
    for (i = 0; i < sizeof edge / sizeof edge[0]; i++) {
        snprintf(what, sizeof what, "isqrt(%" PRIu64 ")", edge[i]);
        check(form_isqrt64(edge[i]) == isqrt_ref(edge[i]), what);
    }
    /* a square and the value one below it: k and k - 1 */
    for (i = 0; i < sizeof root / sizeof root[0]; i++) {
        const uint64_t k = root[i], sq = k * k;
        snprintf(what, sizeof what, "isqrt(%" PRIu64 "^2)", k);
        check(form_isqrt64(sq) == k && form_isqrt64(sq - 1) == k - 1, what);
    }
    check(form_isqrt64(~(uint64_t)0) == 0xFFFFFFFFull, "isqrt(2^64 - 1) = 2^32 - 1");
    for (i = 0; i < 100000; i++) {
        const uint64_t v = rnd() >> (rnd() % 64);
        if (form_isqrt64(v) != isqrt_ref(v) && bad++ < 5) {
            snprintf(what, sizeof what, "isqrt(%" PRIu64 ") random", v);
            check(0, what);
        }
    }
    check(bad == 0, "isqrt: 10^5 random values");
}

static void length_cost_checks(void)
{
    FormPt n[2];
    n[0] = pt(0, 0, 0);
    /* (3, 4) wu: 5 wu, in 16.16 */
    n[1] = pt(WU(3), WU(9), WU(4));
    check(form_path_length(n, 2) == WU(5), "length: (3,4) wu is 5 wu, altitude ignored");
    check(form_cost(n[0], n[1]) == 5, "cost: (3,4) wu is 5");
    /* one 16.16 step on both axes: floor(sqrt(2)) = 1 */
    n[1] = pt(1, 0, 1);
    check(form_path_length(n, 2) == 1, "length: (1,1) in 16.16 is 1");
    check(form_path_length(n, 1) == 0 && form_path_length(n, 0) == 0, "length: one node, none");
    /* half a wu rounds up, one 16.16 step below it rounds down */
    check(form_cost(pt(0, 0, 0), pt(0x8000, 0, 0)) == 1, "cost: 0.5 wu rounds to 1");
    check(form_cost(pt(0, 0, 0), pt(0x7FFF, 0, 0)) == 0, "cost: below 0.5 wu rounds to 0");
    /* 32766.5 wu rounds up to the cap, 32767; corner to corner is about 46341 wu */
    check(form_cost(pt(0, 0, 0), pt(WU(32766) + 0x8000, 0, 0)) == 32767, "cost: 32766.5 wu");
    check(form_cost(pt(0, 0, 0), pt(0x7FFFFFFF, 0, 0x7FFFFFFF)) == FORM_COST_MAX,
          "cost: corner to corner is capped");
}

static void path_checks(void)
{
    unsigned m = 0, k, ok;
    /* a point at the last node's x and z is skipped, whatever its altitude */
    m = form_path_add(s_node, m, pt(WU(5), 0, WU(5)));
    m = form_path_add(s_node, m, pt(WU(5), WU(7), WU(5)));
    check(m == 1 && s_node[0].alt == 0, "add: duplicate x and z skipped, the first kept");
    m = form_path_add(s_node, m, pt(WU(6), 0, WU(5)));
    check(m == 2, "add: a new point appended");
    /* returning to an earlier node is not a duplicate: only the last node is compared */
    m = form_path_add(s_node, m, pt(WU(5), 0, WU(5)));
    check(m == 3, "add: back to the first node appended");

    /* 4096 nodes 1 wu apart along x: full; the next point keeps 0, 2, .., 4094, then 4095,
       then appends itself: 2048 + 1 + 1 */
    for (m = 0, k = 0; k < FORM_MAX_NODES; k++) m = form_path_add(s_node, m, pt(WU(k), 0, 0));
    check(m == FORM_MAX_NODES, "add: 4096 distinct points fill the path");
    m = form_path_add(s_node, m, pt(WU(4096), 0, 0));
    for (ok = 1, k = 0; k < 2048; k++) ok &= s_node[k].x == WU(2 * k);
    check(m == 2050 && ok, "decimate: the even nodes kept, in order");
    check(s_node[2048].x == WU(4095) && s_node[2049].x == WU(4096),
          "decimate: the last node kept, then the new one");
    check(form_path_length(s_node, m) == WU(4096), "decimate: a straight path keeps its length");

    /* 10000 points: decimated three times; the first and the last survive, the cap holds */
    for (ok = 1, m = 0, k = 0; k < 10000; k++) {
        m = form_path_add(s_node, m, pt(WU(k % 3000), 0, WU(k / 3000)));
        ok &= m <= FORM_MAX_NODES;
    }
    check(ok && same(s_node[0], pt(0, 0, 0)) && same(s_node[m - 1], pt(WU(999), 0, WU(3))),
          "decimate: 10000 points, the first and the last survive");
}

static void slot_checks(void)
{
    FormPt n[3], out[11];
    unsigned k, ok;
    /* a straight line of 1000 wu rising 10 wu: L = 1000 wu, t_k = 250 k wu, frac = 16384 k,
       so slot k is (250 k, 2.5 k, 0) wu exactly */
    n[0] = pt(0, 0, 0);
    n[1] = pt(WU(1000), WU(10), 0);
    form_slots(n, 2, 5, out);
    for (ok = 1, k = 0; k < 5; k++) ok &= same(out[k], pt(WU(250 * k), (int32_t)(163840 * k), 0));
    check(ok, "slots: a straight line, evenly, both ends exact");
    /* an L: 256 wu east, then 64 wu south, altitude 32 wu at the corner and 0 at the end;
       L = 320 wu, 11 slots 32 wu apart: frac = 8192 k on the first leg, so (32 k, 4 k, 0) wu
       for k = 0..8, the corner at k = 8, then frac 32768 and 65536 on the second leg */
    n[1] = pt(WU(256), WU(32), 0);
    n[2] = pt(WU(256), 0, WU(64));
    form_slots(n, 3, 11, out);
    for (ok = 1, k = 0; k <= 8; k++) ok &= same(out[k], pt(WU(32 * k), WU(4 * k), 0));
    check(ok, "slots: an L, the first leg");
    check(same(out[8], n[1]), "slots: an L, the corner where arc length puts it");
    check(same(out[9], pt(WU(256), WU(16), WU(32))) && same(out[10], n[2]),
          "slots: an L, the second leg and the end");
    /* frac is in 1/65536 of the segment: 50 wu along a 300 wu segment is frac
       floor(50 * 65536 / 300) = 10922, x = 300 * 10922 = 3276600 in 16.16, 200 short of 50 wu */
    n[1] = pt(WU(300), 0, 0);
    form_slots(n, 2, 7, out);
    check(out[1].x == 3276600 && same(out[6], n[1]), "slots: frac in 1/65536 of the segment");
    /* one node: every slot on it */
    form_slots(n, 1, 4, out);
    for (ok = 1, k = 0; k < 4; k++) ok &= same(out[k], n[0]);
    check(ok, "slots: one node");
    /* a path whose length rounds: three nodes (0,0) (1,1) (2,2) in 16.16, each segment
       floor(sqrt(2)) = 1, L = 2; 3 slots at t = 0, 1, 2: the three nodes */
    n[0] = pt(0, 0, 0); n[1] = pt(1, 0, 1); n[2] = pt(2, 0, 2);
    form_slots(n, 3, 3, out);
    check(same(out[0], n[0]) && same(out[1], n[1]) && same(out[2], n[2]),
          "slots: a 16.16-step path lands on its nodes");
}

static void hungarian_checks(void)
{
    FormPt st[4], sl[4];
    uint16_t as[4];
    /* On a line (z = 0): units at 0, 30, 100 wu; slots at 20, 40, -30 wu (shifted by 100 to keep
       x non-negative). Sorted pairing is optimal for |u - s| on a line: unit 0 -> slot 2 (30),
       unit 1 -> slot 0 (10), unit 2 -> slot 1 (60), total 100. The other five permutations
       total 120, 140, 160, 180 and 180, so the optimum is unique; taking each unit's nearest
       free slot in turn gives 0 -> 0, 1 -> 1, 2 -> 2 = 160. */
    st[0] = pt(WU(100), 0, 0); st[1] = pt(WU(130), 0, 0); st[2] = pt(WU(200), 0, 0);
    sl[0] = pt(WU(120), 0, 0); sl[1] = pt(WU(140), 0, 0); sl[2] = pt(WU(70), 0, 0);
    form_hungarian(st, sl, 3, as);
    check(as[0] == 2 && as[1] == 0 && as[2] == 1 && total(st, sl, 3, as) == 100,
          "hungarian 3x3: the unique optimum, not the greedy one");
    /* 4 units on one point, 4 slots 10 wu away around it: every cost is 10, every permutation
       optimal; the strict `<` keeps the lowest column, so unit i takes slot i */
    st[0] = st[1] = st[2] = st[3] = pt(WU(100), 0, WU(100));
    sl[0] = pt(WU(110), 0, WU(100)); sl[1] = pt(WU(90), 0, WU(100));
    sl[2] = pt(WU(100), 0, WU(110)); sl[3] = pt(WU(100), 0, WU(90));
    form_hungarian(st, sl, 4, as);
    check(as[0] == 0 && as[1] == 1 && as[2] == 2 && as[3] == 3,
          "hungarian 4x4: all tied, identity");
    /* two pairs of units, each pair on one point, and two pairs of slots on the other pair's
       point: units 0, 1 at A take slots 2, 3 at A, units 2, 3 at B take slots 0, 1 at B, cost 0.
       Within a pair the tie goes as above: the lower unit takes the lower slot. */
    st[0] = st[1] = sl[2] = sl[3] = pt(0, 0, 0);
    st[2] = st[3] = sl[0] = sl[1] = pt(WU(100), 0, 0);
    form_hungarian(st, sl, 4, as);
    check(as[0] == 2 && as[1] == 3 && as[2] == 0 && as[3] == 1, "hungarian 4x4: tied pairs");
}

static void nox_checks(void)
{
    FormPt st[2], sl[2];
    uint16_t as[2];
    uint32_t tests;
    unsigned i;
    /* units at (0,0) and (0,100), slots at (100,100) and (100,0), paired 0-0 and 1-1: the paths
       cross at (50,50), so the pass swaps them, and the second pass finds nothing */
    st[0] = pt(0, 0, 0); st[1] = pt(0, 0, WU(100));
    sl[0] = pt(WU(100), 0, WU(100)); sl[1] = pt(WU(100), 0, 0);
    to_wu(st, 2, s_p);
    to_wu(sl, 2, s_q);
    as[0] = 0; as[1] = 1;
    tests = nox_uncross(2, as);
    check(as[0] == 1 && as[1] == 0 && tests == 2, "nox: two crossing paths swapped");
    /* the whole of form_nox: the starts' furthest pair is (0, 100), so units sort 0, 1 and slots
       by z, 1 then 0: paired without a crossing */
    form_nox(st, sl, 2, as);
    check(as[0] == 1 && as[1] == 0, "nox: sorted along the furthest pair");
    /* a T: the second path starts on the first, which is not a proper crossing */
    st[0] = pt(0, 0, 0); sl[0] = pt(WU(100), 0, 0);
    st[1] = pt(WU(50), 0, 0); sl[1] = pt(WU(50), 0, WU(100));
    to_wu(st, 2, s_p);
    to_wu(sl, 2, s_q);
    as[0] = 0; as[1] = 1;
    nox_uncross(2, as);
    check(as[0] == 0 && as[1] == 1, "nox: touching paths kept");
    /* collinear and overlapping: kept too */
    st[1] = pt(WU(20), 0, 0); sl[1] = pt(WU(80), 0, 0);
    to_wu(st, 2, s_p);
    to_wu(sl, 2, s_q);
    as[0] = 0; as[1] = 1;
    nox_uncross(2, as);
    check(as[0] == 0 && as[1] == 1, "nox: collinear paths kept");

    /* 1500 random starts paired in index order with a line of slots crosses nearly everywhere:
       the passes stop at the work cap, and the result is still a permutation */
    for (i = 0; i < FORM_MAX_UNITS; i++) {
        s_start[i] = pt(WU(rnd_below(8000)), 0, WU(rnd_below(8000)));
        s_slot[i] = pt(WU(1000 + (int32_t)i), 0, WU(4000));
        s_as[i] = (uint16_t)i;
    }
    to_wu(s_start, FORM_MAX_UNITS, s_p);
    to_wu(s_slot, FORM_MAX_UNITS, s_q);
    tests = nox_uncross(FORM_MAX_UNITS, s_as);
    check(tests <= FORM_UNCROSS_WORK, "nox: the work cap holds");
    check(tests == FORM_UNCROSS_WORK, "nox: index-order pairing of 1500 runs to the cap");
    check(is_perm(s_as, FORM_MAX_UNITS), "nox: a capped pass leaves a permutation");
    /* every point on one spot: the direction is (1, 0), and the order is the index order */
    for (i = 0; i < 300; i++) s_start[i] = s_slot[i] = pt(WU(7), 0, WU(7));
    form_nox(s_start, s_slot, 300, s_as);
    for (tests = 1, i = 0; i < 300; i++) tests &= s_as[i] == i;
    check(tests, "nox: coincident points pair in index order");
}

/* ---- properties over random cases ---------------------------------------------------------- */

static int64_t best_total(const FormPt* st, const FormPt* sl, unsigned n, unsigned k,
                          uint16_t* as, unsigned used)
{
    int64_t best = INT64_MAX, t;
    unsigned j;
    if (k == n) return total(st, sl, n, as);
    for (j = 0; j < n; j++) {
        if (used & (1u << j)) continue;
        as[k] = (uint16_t)j;
        t = best_total(st, sl, n, k + 1, as, used | (1u << j));
        if (t < best) best = t;
    }
    return best;
}

static void random_points(FormPt* p, unsigned n, int32_t span)
{
    unsigned i;
    for (i = 0; i < n; i++)
        p[i] = pt(rnd_below((uint32_t)span) * 65536 + rnd_below(65536), 0,
                  rnd_below((uint32_t)span) * 65536 + rnd_below(65536));
}

static void properties(void)
{
    static const unsigned big[] = { 255, 256, 257, 300, 1500 };
    uint16_t as[8];
    unsigned c, n, bad_opt = 0, bad_perm = 0, bad_algo = 0;
    int algo;
    /* the Hungarian is optimal: equal to the brute-force minimum, n <= 7, small coordinates so
       ties are common */
    for (c = 0; c < 300; c++) {
        n = 1 + (unsigned)rnd_below(7);
        random_points(s_start, n, c & 1 ? 12 : 3000);
        random_points(s_slot, n, c & 1 ? 12 : 3000);
        form_hungarian(s_start, s_slot, n, s_as);
        if (!is_perm(s_as, n) || total(s_start, s_slot, n, s_as) !=
                                     best_total(s_start, s_slot, n, 0, as, 0))
            bad_opt++;
    }
    check(bad_opt == 0, "hungarian: the brute-force minimum, 300 cases n <= 7");
    /* every assignment is a permutation, at every size the DLL uses */
    for (c = 0; c < 60 + sizeof big / sizeof big[0]; c++) {
        n = c < 60 ? 1 + (unsigned)rnd_below(20) : big[c - 60];
        random_points(s_start, n, c & 1 ? 50 : 10000);
        random_points(s_slot, n, 10000);
        if (n <= FORM_HUNGARIAN_MAX) {
            form_hungarian(s_start, s_slot, n, s_as);
            bad_perm += !is_perm(s_as, n);
        }
        form_nox(s_start, s_slot, n, s_as);
        bad_perm += !is_perm(s_as, n);
        algo = form_assign(s_start, s_slot, n, s_as);
        bad_perm += !is_perm(s_as, n);
        bad_algo += algo != (n <= FORM_HUNGARIAN_MAX ? 'h' : 'x');
    }
    check(bad_perm == 0, "every assignment is a permutation");
    check(bad_algo == 0, "assign: hungarian up to 256, nox above");
}

/* ---- dump ---------------------------------------------------------------------------------- */

static int read_i64(int64_t* v) { return scanf("%" SCNd64, v) == 1; }

static int read_pts(FormPt* p, unsigned n, int with_alt)
{
    unsigned i;
    int64_t x, a = 0, z;
    for (i = 0; i < n; i++) {
        if (!read_i64(&x) || (with_alt && !read_i64(&a)) || !read_i64(&z)) return 0;
        p[i] = pt((int32_t)x, (int32_t)a, (int32_t)z);
    }
    return 1;
}

static void put_pts(const FormPt* p, unsigned n, int lead_space)
{
    unsigned i;
    for (i = 0; i < n; i++)
        printf("%s%" PRId32 ",%" PRId32 ",%" PRId32, i || lead_space ? " " : "",
               p[i].x, p[i].alt, p[i].z);
}

static int dump(void)
{
    char cmd[16];
    int64_t a, b;
    unsigned i, m;
    while (scanf("%15s", cmd) == 1) {
        if (!strcmp(cmd, "add")) {
            FormPt p;
            if (!read_i64(&a) || a < 0) return 2;
            for (m = 0, i = 0; i < (unsigned)a; i++) {
                if (!read_pts(&p, 1, 1)) return 2;
                m = form_path_add(s_node, m, p);
            }
            printf("%u %" PRId64, m, form_path_length(s_node, m));
            put_pts(s_node, m, 1);
        } else if (!strcmp(cmd, "slots")) {
            if (!read_i64(&a) || !read_i64(&b) || a < 1 || a > FORM_MAX_NODES || b < 0 ||
                b > FORM_MAX_UNITS || !read_pts(s_node, (unsigned)a, 1))
                return 2;
            form_slots(s_node, (unsigned)a, (unsigned)b, s_slot);
            put_pts(s_slot, (unsigned)b, 0);
        } else if (!strcmp(cmd, "hung") || !strcmp(cmd, "nox") || !strcmp(cmd, "assign")) {
            int algo = 0;
            if (!read_i64(&a) || a < 1 || a > FORM_MAX_UNITS ||
                !read_pts(s_start, (unsigned)a, 0) || !read_pts(s_slot, (unsigned)a, 0))
                return 2;
            m = (unsigned)a;
            if (cmd[0] == 'h') form_hungarian(s_start, s_slot, m, s_as);
            else if (cmd[0] == 'n') form_nox(s_start, s_slot, m, s_as);
            else algo = form_assign(s_start, s_slot, m, s_as);
            if (algo) printf("%c ", algo);
            printf("%" PRId64, total(s_start, s_slot, m, s_as));
            for (i = 0; i < m; i++) printf(" %u", s_as[i]);
        } else {
            printf("?\n");
            return 2;
        }
        printf("\n");
    }
    return 0;
}

/* ---- bench --------------------------------------------------------------------------------- */

static double now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
#endif
}

/* `reps` runs of `what`, the fastest and the mean */
#define TIME(label, reps, what) do { \
    double best_ = 1e30, sum_ = 0; unsigned r_; \
    for (r_ = 0; r_ < (reps); r_++) { \
        const double t0_ = now_ms(); what; { const double d_ = now_ms() - t0_; \
        sum_ += d_; if (d_ < best_) best_ = d_; } } \
    printf("bench: %-44s %9.3f ms min, %9.3f ms mean of %u\n", label, best_, sum_ / (reps), \
           (unsigned)(reps)); } while (0)

static void line_slots(unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        s_slot[i] = pt(WU(1000) + (int32_t)((int64_t)i * WU(3000) / n), 0, WU(4000));
}

static void ring_slots(unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        const double a = 6.283185307179586 * i / n;
        s_slot[i] = pt(WU(4000) + (int32_t)(cos(a) * 600.0 * 65536.0), 0,
                       WU(4000) + (int32_t)(sin(a) * 600.0 * 65536.0));
    }
}

static int bench(void)
{
    uint32_t tests = 0;
    unsigned i;
    random_points(s_start, FORM_HUNGARIAN_MAX, 8000);
    line_slots(FORM_HUNGARIAN_MAX);
    TIME("hungarian n=256, random starts, a line", 20,
         form_hungarian(s_start, s_slot, FORM_HUNGARIAN_MAX, s_as));
    ring_slots(FORM_HUNGARIAN_MAX);
    TIME("hungarian n=256, random starts, a ring", 20,
         form_hungarian(s_start, s_slot, FORM_HUNGARIAN_MAX, s_as));

    random_points(s_start, FORM_MAX_UNITS, 8000);
    line_slots(FORM_MAX_UNITS);
    TIME("nox n=1500, random starts, a line", 10,
         form_nox(s_start, s_slot, FORM_MAX_UNITS, s_as));
    to_wu(s_start, FORM_MAX_UNITS, s_p);
    to_wu(s_slot, FORM_MAX_UNITS, s_q);
    nox_pair(FORM_MAX_UNITS, s_as);
    printf("bench: %-44s %9" PRIu32 " tests\n", "nox n=1500 uncross after the sorted pairing",
           nox_uncross(FORM_MAX_UNITS, s_as));
    ring_slots(FORM_MAX_UNITS);
    TIME("nox n=1500, random starts, a ring", 10,
         form_nox(s_start, s_slot, FORM_MAX_UNITS, s_as));
    /* the uncross alone from index order: the pairing that crosses most, so it runs to the cap */
    line_slots(FORM_MAX_UNITS);
    to_wu(s_slot, FORM_MAX_UNITS, s_q);
    TIME("nox n=1500 uncross alone, index-order pairing", 10,
         for (i = 0; i < FORM_MAX_UNITS; i++) s_as[i] = (uint16_t)i;
         tests = nox_uncross(FORM_MAX_UNITS, s_as));
    printf("bench: %-44s %9" PRIu32 " tests\n", "nox n=1500 uncross, index-order pairing", tests);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "dump")) return dump();
    if (argc > 1 && !strcmp(argv[1], "bench")) return bench();
    isqrt_checks();
    length_cost_checks();
    path_checks();
    slot_checks();
    hungarian_checks();
    nox_checks();
    properties();
    printf("formtest: %u checks, %u failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
