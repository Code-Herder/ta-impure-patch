/* scattertest.c -- tagpu_scattergeo.c's self-test (scattertest.py builds it natively with ASan and UBSan). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_scattergeo.h"

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { W = 32, H = 32 };
static uint8_t  s_seen[W * H], s_pass[W * H], s_wall[W * H], s_taken[W * H];
static uint16_t s_occ[W * H];
static int16_t  s_qx[W * H], s_qz[W * H];
static int32_t  s_wsums[(W + 1) * (H + 1)], s_tsums[(W + 1) * (H + 1)];
static const SgWork s_wk = { s_seen, s_qx, s_qz };
static const GgBox  s_box = { 100, 200, W, H };     /* off the origin: every answer is box-absolute */

static void rect(uint8_t* t, int x0, int z0, int fx, int fz, uint8_t v)
{
    int x, z;
    for (z = z0; z < z0 + fz; z++)
        for (x = x0; x < x0 + fx; x++) t[z * W + x] = v;
}

/* open ground, a 4 x 4 site at box (14, 14) */
static void field(void)
{
    memset(s_pass, 1, sizeof s_pass);
    memset(s_wall, 0, sizeof s_wall);
    memset(s_taken, 0, sizeof s_taken);
    memset(s_occ, 0, sizeof s_occ);
    rect(s_taken, 14, 14, 4, 4, 1);
}

static int way(int x, int z, int fx, int fz, unsigned self, int* ox, int* oz)
{
    gg_sums(s_wall, W, H, s_wsums);
    gg_sums(s_taken, W, H, s_tsums);
    return sg_way_off(&s_box, s_box.x0 + x, s_box.z0 + z, fx, fz, self, s_pass, s_wsums, s_tsums, s_occ, &s_wk, ox,
                      oz);
}

static void test_overlap(void)
{
    CHECK(sg_overlap(0, 0, 2, 2, 1, 1, 2, 2), "corner cells share");
    CHECK(!sg_overlap(0, 0, 2, 2, 2, 0, 2, 2), "side by side");
    CHECK(!sg_overlap(0, 0, 2, 2, 0, 2, 2, 2), "one above the other");
    CHECK(sg_overlap(5, 5, 1, 1, 3, 3, 4, 4), "inside");
}

static void test_nearest(void)
{
    int ox = 0, oz = 0;
    field();
    /* a 1 x 1 unit on the site's top-left cell: north first, one step */
    s_occ[14 * W + 14] = 7;
    CHECK(way(14, 14, 1, 1, 7, &ox, &oz) && ox == 100 + 14 && oz == 200 + 13, "top-left %d,%d", ox, oz);
    /* in the site's middle the top edge is two steps off, and the first reached of the places as near */
    memset(s_occ, 0, sizeof s_occ);
    s_occ[15 * W + 15] = 7;
    CHECK(way(15, 15, 1, 1, 7, &ox, &oz) && ox == 100 + 15 && oz == 200 + 13, "middle %d,%d", ox, oz);
    /* a 2 x 2 unit whose footprint must clear the whole site */
    memset(s_occ, 0, sizeof s_occ);
    rect((uint8_t*)s_pass, 0, 0, W, H, 1);
    CHECK(way(15, 15, 2, 2, 7, &ox, &oz), "2x2 found");
    CHECK(!sg_overlap(ox - 100, oz - 200, 2, 2, 14, 14, 4, 4), "2x2 clear of the site %d,%d", ox, oz);
    /* the same answer twice: no state between calls */
    {
        int px = ox, pz = oz;
        CHECK(way(15, 15, 2, 2, 7, &ox, &oz) && ox == px && oz == pz, "repeatable %d,%d", ox, oz);
    }
}

static void test_obstacles(void)
{
    int ox = 0, oz = 0, i;
    field();
    /* another unit stands on the place north of the site: not a place to stand, and the search goes on */
    s_occ[14 * W + 14] = 7;
    s_occ[13 * W + 14] = 9;
    CHECK(way(14, 14, 1, 1, 7, &ox, &oz) && ox == 100 + 13 && oz == 200 + 14, "past a unit %d,%d", ox, oz);
    /* its own number on a cell is no obstacle */
    s_occ[13 * W + 14] = 7;
    CHECK(way(14, 14, 1, 1, 7, &ox, &oz) && ox == 100 + 14 && oz == 200 + 13, "own cell %d,%d", ox, oz);
    /* a place given to another unit is taken */
    s_occ[13 * W + 14] = 0;
    s_taken[13 * W + 14] = 1;
    CHECK(way(14, 14, 1, 1, 7, &ox, &oz) && ox == 100 + 13 && oz == 200 + 14, "past a reservation %d,%d", ox, oz);
    /* a wall round the site with one gap on its east side: the way leads through the gap */
    field();
    for (i = 12; i <= 19; i++) {
        s_wall[12 * W + i] = 1; s_wall[19 * W + i] = 1;
        s_wall[i * W + 12] = 1; s_wall[i * W + 19] = 1;
    }
    s_wall[16 * W + 19] = 0;
    memcpy(s_taken, s_wall, sizeof s_taken);
    rect(s_taken, 14, 14, 4, 4, 1);
    s_occ[14 * W + 14] = 7;
    CHECK(way(14, 14, 1, 1, 7, &ox, &oz), "inside the wall");
    CHECK(ox - 100 >= 13 && ox - 100 <= 18 && oz - 200 >= 13 && oz - 200 <= 18, "the ring inside %d,%d", ox, oz);
    /* the ring inside the wall taken too: the gap's own cell is the nearest place left */
    rect(s_taken, 13, 13, 6, 6, 1);
    CHECK(way(14, 14, 1, 1, 7, &ox, &oz) && ox == 100 + 19 && oz == 200 + 16, "into the gap %d,%d", ox, oz);
    /* the gap's cell taken as well: through it to the first place beyond */
    s_taken[16 * W + 19] = 1;
    CHECK(way(14, 14, 1, 1, 7, &ox, &oz) && ox == 100 + 20 && oz == 200 + 16, "through the gap %d,%d", ox, oz);
    /* the gap shut: no way */
    s_wall[16 * W + 19] = 1;
    CHECK(!way(14, 14, 1, 1, 7, &ox, &oz), "walled in");
    /* a 2 x 2 unit does not fit a gap one cell wide */
    s_wall[16 * W + 19] = 0;
    CHECK(!way(14, 14, 2, 2, 7, &ox, &oz), "2x2 at a 1-cell gap");
    /* ground its class cannot cross is no step: a strip of it round the site */
    field();
    rect(s_pass, 12, 12, 8, 8, 0);
    rect(s_pass, 14, 14, 4, 4, 1);
    s_occ[15 * W + 15] = 7;
    CHECK(!way(15, 15, 1, 1, 7, &ox, &oz), "impassable ring");
}

static void test_edges(void)
{
    int ox = 0, oz = 0;
    /* a site in the box's corner: the answers stay in the box */
    memset(s_pass, 1, sizeof s_pass);
    memset(s_wall, 0, sizeof s_wall);
    memset(s_taken, 0, sizeof s_taken);
    memset(s_occ, 0, sizeof s_occ);
    rect(s_taken, 0, 0, 4, 4, 1);
    CHECK(way(0, 0, 3, 3, 7, &ox, &oz), "corner found");
    CHECK(ox >= 100 && oz >= 200 && ox + 3 <= 100 + W && oz + 3 <= 200 + H, "corner in the box %d,%d", ox, oz);
    CHECK(!sg_overlap(ox - 100, oz - 200, 3, 3, 0, 0, 4, 4), "corner clear of the site");
    /* a start whose footprint leaves the box, and a footprint of no size, are refused */
    CHECK(!way(W - 1, 0, 2, 2, 7, &ox, &oz), "start off the box");
    CHECK(!way(4, 4, 0, 1, 7, &ox, &oz), "no footprint");
    /* the whole box taken: nowhere to stand */
    memset(s_taken, 1, sizeof s_taken);
    CHECK(!way(8, 8, 1, 1, 7, &ox, &oz), "all taken");
}

int main(void)
{
    test_overlap();
    test_nearest();
    test_obstacles();
    test_edges();
    printf("scattertest: %s (%d failed)\n", s_fail ? "FAIL" : "ok", s_fail);
    return s_fail != 0;
}
