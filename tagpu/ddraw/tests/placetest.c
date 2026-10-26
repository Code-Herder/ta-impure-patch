/* placetest.c -- the self-test of tagpu_placegeo.c, for tests/placetest.py.

   `placetest` checks hand-worked tables (each worked out from Recoil's GetBuildPositions and
   BAR's api_build_orders.lua in the comment above it) and then properties by enumeration over
   every footprint 1..4, spacings 0/1/3/16 and drags in every direction. It prints one line per
   failure and a summary; exit 0 when nothing failed.

   `placetest dump` reads `shape KIND FX FZ S FROMX FROMZ TOX TOZ`, `ring FX FZ TX TZ TW TH` and
   `split N P0 P1 ...` lines on stdin and prints the result, for a cross-check against another
   implementation of the same rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_placegeo.h"

static unsigned s_checks, s_failed;

static void fail(const char* what, const char* fmt, int a, int b, int c, int d)
{
    s_failed++;
    printf("FAIL %s: ", what);
    printf(fmt, a, b, c, d);
    printf("\n");
}

static void expect_sites(const char* what, const PlaceCell* got, unsigned n,
                         const int* want, unsigned wn)
{
    unsigned i;
    s_checks++;
    if (n != wn) { fail(what, "%d sites, want %d%c%c", (int)n, (int)wn, ' ', ' '); return; }
    for (i = 0; i < n; i++)
        if (got[i].cx != want[2 * i] || got[i].cz != want[2 * i + 1]) {
            fail(what, "site %d is (%d,%d), want x %d", (int)i, got[i].cx, got[i].cz, want[2 * i]);
            return;
        }
}

static unsigned shape(int kind, int fx, int fz, int s, int ax, int az, int bx, int bz,
                      PlaceCell* out)
{
    PlaceCell a, b;
    a.cx = (int16_t)ax; a.cz = (int16_t)az;
    b.cx = (int16_t)bx; b.cz = (int16_t)bz;
    return place_shape(kind, fx, fz, s, a, b, out, PLACE_MAX_SITES);
}

#define WANT(name, n, ...) do { static const int w_[] = { __VA_ARGS__ }; \
    expect_sites(name, out, n, w_, sizeof w_ / sizeof w_[0] / 2); } while (0)

static void tables(void)
{
    PlaceCell out[PLACE_MAX_SITES];
    unsigned n;

    /* 2x2, (10,10) -> (18,10): xnum = (8 + 1.4*2) / 2 = 5, one pitch apart */
    n = shape(PLACE_LINE, 2, 2, 0, 10, 10, 18, 10, out);
    WANT("line east", n, 10,10, 12,10, 14,10, 16,10, 18,10);

    /* no drag: one site */
    n = shape(PLACE_LINE, 2, 2, 0, 5, 7, 5, 7, out);
    WANT("line still", n, 5,7);
    n = shape(PLACE_RECT, 3, 3, 0, 5, 7, 5, 7, out);
    WANT("rect still", n, 5,7);
    n = shape(PLACE_HOLLOW, 3, 3, 0, 5, 7, 5, 7, out);
    WANT("hollow still", n, 5,7);

    /* 2x2, (0,0) -> (8,4): z follows at 2 * 4/8 = 1 cell per site */
    n = shape(PLACE_LINE, 2, 2, 0, 0, 0, 8, 4, out);
    WANT("line diagonal", n, 0,0, 2,1, 4,2, 6,3, 8,4);
    n = shape(PLACE_AXIS, 2, 2, 0, 0, 0, 8, 4, out);
    WANT("axis", n, 0,0, 2,0, 4,0, 6,0, 8,0);

    /* 2x2, (10,10) -> (9,2): z dominant, x steps -2/8 cell per site (centres 176, 172, 168,
       164, 160 wu; 168/16 - 1 = 9.5 rounds up to 10) */
    n = shape(PLACE_LINE, 2, 2, 0, 10, 10, 9, 2, out);
    WANT("line north-west", n, 10,10, 10,8, 10,6, 9,4, 9,2);

    /* westwards, the step negative */
    n = shape(PLACE_LINE, 2, 2, 0, 10, 0, 4, 0, out);
    WANT("line west", n, 10,0, 8,0, 6,0, 4,0);

    /* spacing 1: pitch 3, xnum = (9 + 4.2) / 3 = 4 */
    n = shape(PLACE_LINE, 2, 2, 1, 0, 0, 9, 0, out);
    WANT("line spaced", n, 0,0, 3,0, 6,0, 9,0);

    /* 1.4: a drag short of 0.6 pitch adds no site, from 0.6 it adds one */
    n = shape(PLACE_LINE, 5, 5, 0, 0, 0, 2, 0, out);
    WANT("line 0.4 pitch", n, 0,0);
    n = shape(PLACE_LINE, 5, 5, 0, 0, 0, 3, 0, out);
    WANT("line 0.6 pitch", n, 0,0, 5,0);

    /* 2x2, (0,0) -> (4,2): 3 x 2, serpentine */
    n = shape(PLACE_RECT, 2, 2, 0, 0, 0, 4, 2, out);
    WANT("rect", n, 0,0, 2,0, 4,0, 4,2, 2,2, 0,2);
    /* the same dragged up-left */
    n = shape(PLACE_RECT, 2, 2, 0, 4, 2, 0, 0, out);
    WANT("rect up-left", n, 4,2, 2,2, 0,2, 0,0, 2,0, 4,0);

    /* 2x2, (0,0) -> (6,4): 4 x 3 ring of 10, Recoil's side order */
    n = shape(PLACE_HOLLOW, 2, 2, 0, 0, 0, 6, 4, out);
    WANT("hollow", n, 0,2, 0,4, 2,4, 4,4, 6,4, 6,2, 6,0, 4,0, 2,0, 0,0);
    /* one column, one row */
    n = shape(PLACE_HOLLOW, 2, 2, 0, 0, 0, 0, 4, out);
    WANT("hollow column", n, 0,0, 0,2, 0,4);
    n = shape(PLACE_HOLLOW, 2, 2, 0, 0, 0, 4, 0, out);
    WANT("hollow row", n, 0,0, 2,0, 4,0);

    /* 2x2 around a 2x2 at (10,10): the pinwheel of 8 */
    n = place_ring(2, 2, 10, 10, 2, 2, out, PLACE_MAX_SITES);
    WANT("ring 2 around 2", n, 12,10, 12,12, 10,12, 8,12, 8,10, 8,8, 10,8, 12,8);
    /* 1x1 around a 1x1: the 8 neighbours */
    n = place_ring(1, 1, 0, 0, 1, 1, out, PLACE_MAX_SITES);
    WANT("ring 1 around 1", n, 1,0, 1,1, 0,1, -1,1, -1,0, -1,-1, 0,-1, 1,-1);

    /* the cap */
    n = shape(PLACE_LINE, 1, 1, 0, 0, 0, 30000, 0, out);
    s_checks++;
    if (n != PLACE_MAX_SITES) fail("cap", "%d sites, want %d%c%c", (int)n, PLACE_MAX_SITES, ' ', ' ');
}

static void expect_split(const char* what, const uint32_t* power, unsigned k, unsigned n,
                         const int* want /* per participant: first, count, follow */)
{
    PlaceChunk c[16];
    unsigned i;
    s_checks++;
    place_split(power, k, n, c);
    for (i = 0; i < k; i++)
        if (c[i].first != want[3 * i] || c[i].count != want[3 * i + 1] ||
            c[i].follow != want[3 * i + 2]) {
            fail(what, "participant %d is %d+%d from %d", (int)i, c[i].first, c[i].count,
                 c[i].follow);
            return;
        }
}

static void split_tables(void)
{
    /* {300,100} over 4: the strong one's share is 3 (7 * 400 > 2 * 4 * 300 stops the 4th) */
    { static const uint32_t p[] = { 300, 100 }; static const int w[] = { 0,3,3, 3,1,4 };
      expect_split("split 300/100 x4", p, 2, 4, w); }
    /* equal powers over 3: one each */
    { static const uint32_t p[] = { 100, 100, 100 }; static const int w[] = { 0,1,1, 1,1,2, 2,1,3 };
      expect_split("split equal x3", p, 3, 3, w); }
    /* one site, three builders: the first takes it, the others start their followups at
       i * 1 / 3 = 0 */
    { static const uint32_t p[] = { 100, 100, 100 }; static const int w[] = { 0,1,1, 1,0,0, 1,0,0 };
      expect_split("split one site", p, 3, 1, w); }
    /* {200,100,50} over 7: shares 4, 2, 1 */
    { static const uint32_t p[] = { 200, 100, 50 }; static const int w[] = { 0,4,4, 4,2,6, 6,1,7 };
      expect_split("split 200/100/50 x7", p, 3, 7, w); }

    /* the order: own chunk, then the others from `follow`, wrapping */
    {
        static const uint32_t p[] = { 300, 100 };
        PlaceChunk c[2];
        uint16_t o[4];
        place_split(p, 2, 4, c);
        place_split_order(&c[1], 4, o);
        s_checks++;
        if (o[0] != 3 || o[1] != 0 || o[2] != 1 || o[3] != 2)
            fail("split order", "%d %d %d %d", o[0], o[1], o[2], o[3]);
    }
}

/* Two fx x fz footprints share a cell. */
static int overlap(PlaceCell a, PlaceCell b, int fx, int fz)
{
    return a.cx < b.cx + fx && b.cx < a.cx + fx && a.cz < b.cz + fz && b.cz < a.cz + fz;
}

static void distinct(const char* what, const PlaceCell* s, unsigned n, int fx, int fz,
                     int k1, int k2)
{
    unsigned i, j;
    s_checks++;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (overlap(s[i], s[j], fx, fz)) {
                fail(what, "sites %d and %d share a cell (case %d/%d)", (int)i, (int)j, k1, k2);
                return;
            }
}

static int count_along(int d, int p) { return (10 * abs(d) + 14 * p) / (10 * p); }

static void properties(void)
{
    static const int spacings[] = { 0, 1, 3, 16 };
    PlaceCell out[PLACE_MAX_SITES];
    int fx, fz, si, dx, dz, kind, tw, th;

    for (fx = 1; fx <= 4; fx++)
    for (fz = 1; fz <= 4; fz++)
    for (si = 0; si < 4; si++)
    for (dx = -40; dx <= 40; dx += 3)
    for (dz = -40; dz <= 40; dz += 3)
    for (kind = PLACE_LINE; kind <= PLACE_HOLLOW; kind++) {
        int s = spacings[si];
        int xnum = count_along(dx, fx + s), znum = count_along(dz, fz + s), want;
        unsigned n = shape(kind, fx, fz, s, 100, 100, 100 + dx, 100 + dz, out);
        unsigned i;
        int key = (fx * 10 + fz) * 100 + s;

        if (kind == PLACE_LINE || kind == PLACE_AXIS)
            want = abs(dx) > abs(dz) ? xnum : znum;
        else if (kind == PLACE_RECT)
            want = xnum * znum;
        else
            want = xnum > 1 && znum > 1 ? 2 * (xnum - 1) + 2 * (znum - 1)
                 : xnum == 1 ? znum : xnum;
        if (want > PLACE_MAX_SITES) want = PLACE_MAX_SITES;
        s_checks++;
        if ((int)n != want) fail("count", "%d sites, want %d (case %d, kind %d)", (int)n, want, key, kind);
        distinct("distinct", out, n, fx, fz, key, kind);

        /* the line and the rectangle start where the drag started */
        s_checks++;
        if (kind != PLACE_HOLLOW && n && (out[0].cx != 100 || out[0].cz != 100))
            fail("start", "(%d,%d) case %d kind %d", out[0].cx, out[0].cz, key, kind);

        /* a line's sites advance one pitch at a time along its dominant axis */
        if (kind == PLACE_LINE || kind == PLACE_AXIS) {
            int xdom = abs(dx) > abs(dz), p = xdom ? fx + s : fz + s;
            s_checks++;
            for (i = 1; i < n; i++) {
                int step = xdom ? out[i].cx - out[i - 1].cx : out[i].cz - out[i - 1].cz;
                if (abs(step) != p) { fail("pitch", "step %d, want %d (case %d kind %d)", step, p, key, kind); break; }
            }
        }
    }

    /* rings: every footprint around every target up to 9x9 */
    for (fx = 1; fx <= 4; fx++)
    for (fz = 1; fz <= 4; fz++)
    for (tw = 1; tw <= 9; tw++)
    for (th = 1; th <= 9; th++) {
        unsigned n = place_ring(fx, fz, 50, 50, tw, th, out, PLACE_MAX_SITES), i;
        int key = (fx * 10 + fz) * 100 + tw * 10 + th;
        distinct("ring distinct", out, n, fx, fz, key, 0);
        s_checks++;
        if ((int)n != 2 * (1 + th / fz) + 2 * (1 + tw / fx))
            fail("ring count", "%d sites (case %d)%c%c", (int)n, key, ' ', ' ');
        s_checks++;
        for (i = 0; i < n; i++) {
            /* outside the target and touching it, edge or corner */
            int x0 = out[i].cx, z0 = out[i].cz, x1 = x0 + fx, z1 = z0 + fz;
            int inside = x0 < 50 + tw && 50 < x1 && z0 < 50 + th && 50 < z1;
            int touch = x1 >= 50 && x0 <= 50 + tw && z1 >= 50 && z0 <= 50 + th;
            if (inside || !touch) { fail("ring site", "site %d at (%d,%d) case %d", (int)i, x0, z0, key); break; }
        }
    }

    /* split: each order is a permutation of the sites, its own chunk first; the chunks tile */
    {
        static const uint32_t sets[][4] = { {300,100,0,0}, {100,100,100,0}, {200,100,50,0},
                                            {300,140,140,100}, {1,1000,0,0} };
        static const unsigned ks[] = { 2, 3, 3, 4, 2 };
        unsigned si2, n, i, k;
        for (si2 = 0; si2 < 5; si2++)
        for (n = 1; n <= 40; n++) {
            PlaceChunk c[4];
            unsigned at = 0;
            k = ks[si2];
            place_split(sets[si2], k, n, c);
            s_checks++;
            for (i = 0; i < k; i++) {
                uint16_t o[64];
                unsigned seen[64] = { 0 }, j;
                if (c[i].first != at) { fail("split tiles", "set %d n %d participant %d first %d", (int)si2, (int)n, (int)i, c[i].first); break; }
                at += c[i].count;
                place_split_order(&c[i], n, o);
                for (j = 0; j < n; j++) {
                    if (o[j] >= n || seen[o[j]]++) { fail("split perm", "set %d n %d participant %d index %d", (int)si2, (int)n, (int)i, (int)j); break; }
                    if (j < c[i].count && o[j] != c[i].first + j) { fail("split own", "set %d n %d participant %d index %d", (int)si2, (int)n, (int)i, (int)j); break; }
                }
            }
            if (at != n) fail("split covers", "set %d n %d covered %d%c", (int)si2, (int)n, (int)at, ' ');
        }
    }
}

/* ---- the take-off rule, place_take_offs_one ---- */

/* the order position of a 1 x 1 footprint at cell (cx, 5): (2cx + 1) << 19 */
static int32_t at1(int cx) { return (int32_t)((uint32_t)(2 * cx + 1) << 19); }

static void expect_take(const char* what, const PlaceOrder* q, unsigned nq, const PlaceSitePos* s,
                        unsigned n, const int* want_by, int want_count)
{
    uint16_t by[8];
    uint8_t took[8];
    unsigned j;
    int count = place_take_offs_one(q, nq, s, n, by, took);
    s_checks++;
    if (count != want_count) { fail(what, "%d sites took one, want %d%c%c", count, want_count, ' ', ' '); return; }
    for (j = 0; j < nq; j++) {
        const int got = by[j] == PLACE_NO_SITE ? -1 : by[j];
        if (got != want_by[j]) { fail(what, "order %d taken by %d, want %d%c", (int)j, got, want_by[j], ' '); return; }
    }
    for (j = 0; j < n; j++) {
        unsigned k;
        int any = 0;
        for (k = 0; k < nq; k++) any |= want_by[k] == (int)j;
        if (took[j] != any) { fail(what, "site %d took %d, want %d%c", (int)j, took[j], any, ' '); return; }
    }
}

static void take_tables(void)
{
    PlaceOrder q[3];
    PlaceSitePos s[4];
    unsigned i;
    /* a queued 1 x 1 row at cells 10..12, neighbours 16 world units apart */
    for (i = 0; i < 3; i++) { q[i].x = at1(10 + (int)i); q[i].z = at1(5); q[i].match = 1; }
    {
        /* dragged over from 9: each order goes to the site on it, and 9, beside the row, takes none */
        static const int by[] = { 1, 2, 3 };
        for (i = 0; i < 4; i++) { s[i].x = at1(9 + (int)i); s[i].z = at1(5); s[i].refused = 0; }
        expect_take("take: drag 9..12 over a queued row", q, 3, s, 4, by, 3);
    }
    {
        /* dragged from 12 back to 9: the same orders taken by the same cells */
        static const int by[] = { 2, 1, 0 };
        for (i = 0; i < 4; i++) { s[i].x = at1(12 - (int)i); s[i].z = at1(5); s[i].refused = 0; }
        expect_take("take: drag 12..9 over a queued row", q, 3, s, 4, by, 3);
    }
    {
        /* a refused site takes nothing, and the site beside it takes that order within reach */
        static const int by[] = { 0, -1, -1 };
        s[0].x = at1(9); s[1].x = at1(10);
        s[0].z = s[1].z = at1(5);
        s[0].refused = 0; s[1].refused = 1;
        expect_take("take: a refused site", q, 3, s, 2, by, 1);
    }
    {
        /* an order of another action is never taken, even at a site's own position */
        static const int by[] = { -1, 0, -1 };
        q[0].match = 0;
        s[0].x = at1(10); s[0].z = at1(5); s[0].refused = 0;
        expect_take("take: an order of another action", q, 3, s, 1, by, 1);
        q[0].match = 1;
    }
    {
        /* 0x43AFC0's reach is 16 world units, inclusive: 0x100000 in 16.16 reaches, one more does not */
        static const int reach[] = { 0, -1, -1 }, beyond[] = { -1, -1, -1 };
        s[0].x = q[0].x - 0x100000; s[0].z = q[0].z + 0x100000; s[0].refused = 0;
        expect_take("take: at the reach", q, 1, s, 1, reach, 1);
        s[0].x = q[0].x - 0x100001;
        expect_take("take: past the reach", q, 1, s, 1, beyond, 0);
    }
}

static int dump(void)
{
    char line[256];
    PlaceCell out[PLACE_MAX_SITES];
    while (fgets(line, sizeof line, stdin)) {
        int a[8];
        unsigned n = 0, i;
        if (sscanf(line, "shape %d %d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4],
                   &a[5], &a[6], &a[7]) == 8) {
            n = shape(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], out);
        } else if (sscanf(line, "ring %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4],
                          &a[5]) == 6) {
            n = place_ring(a[0], a[1], a[2], a[3], a[4], a[5], out, PLACE_MAX_SITES);
        } else if (!strncmp(line, "split ", 6)) {
            uint32_t p[16];
            PlaceChunk c[16];
            unsigned k = 0, sites;
            char* s = line + 6;
            sites = (unsigned)strtoul(s, &s, 10);
            while (k < 16) {
                char* e;
                unsigned long v = strtoul(s, &e, 10);
                if (e == s) break;
                p[k++] = (uint32_t)v;
                s = e;
            }
            if (k == 0 || sites == 0 || sites > 64) { puts("?"); continue; }
            place_split(p, k, sites, c);
            for (i = 0; i < k; i++) {
                uint16_t o[64];
                unsigned j;
                place_split_order(&c[i], sites, o);
                for (j = 0; j < sites; j++) printf(j ? " %u" : "%u", o[j]);
                printf(i + 1 < k ? " | " : "\n");
            }
            continue;
        } else {
            puts("?");
            continue;
        }
        for (i = 0; i < n; i++) printf(i ? " %d,%d" : "%d,%d", out[i].cx, out[i].cz);
        printf("\n");
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "dump")) return dump();
    tables();
    split_tables();
    take_tables();
    properties();
    printf("placetest: %u checks, %u failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
