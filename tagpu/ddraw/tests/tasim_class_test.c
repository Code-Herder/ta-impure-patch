/* tasim_class_test.c -- the self-test of the core's feature grid and pathing maps (src/tasim_map.h,
   src/tasim_class.h), driven by tests/tasim_class_test.py, which builds it natively, under the sanitizers and with the
   DLL's i686 compiler under Wine.

     tasim_class_test            the rules: the ground height 0x485070 on hand-computed cases; the fill against the
                                 ring rule and against the refresh, the edge bands where they disagree, the ring's
                                 off-grid answer 1; the claim, the watermark and the cleanup on hand-placed units;
                                 the re-mark's unsigned clamp; a spawn's destroys and refusals, the pool's order,
                                 the border mask's passes, the metal, LoadMap's order; exit 1 on a failure
     tasim_class_test digest     one 64-bit FNV-1a digest of random maps loaded, filled and re-marked; the driver
                                 requires every build to print the same digest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_class.h"
#include "tasim_map.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                                       printf("\n"); } } while (0)

/* a fixed generator, so every build draws the same maps (xorshift64*) */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
}
static int32_t pick(int32_t n) { return (int32_t)(next() % (uint64_t)n); }

/* ---- fixtures ------------------------------------------------------------------------------------------------ */

typedef struct {
    TS_MAP m;
    void* mem;
    TS_CLASSMAPS cm;
    void* cmem;
    TASIM_MOVECLASS cls[TS_CLASSES];
    TS_CUNIT u[8];
    TS_CUNITS units;
} WORLD;

static void map_make(WORLD* W, int32_t w, int32_t h, uint8_t sea, int32_t pool, const TASIM_FEATUREDEF* fd, int nfd)
{
    size_t n = ts_map_bytes(w, h, pool);
    memset(W, 0, sizeof *W);
    W->mem = malloc(n);
    CHECK(W->mem && ts_map_init(&W->m, W->mem, n, w, h, sea, pool, fd, nfd) == TS_MAP_OK, "map %dx%d", w, h);
}

/* every cell at height `y`, the 2x2 extremes set over the whole map but the strip the engine never writes */
static void map_flat(WORLD* W, uint8_t y)
{
    int32_t i;
    for (i = 0; i < W->m.cells; i++) W->m.height[i] = y;
    ts_map_extremes(&W->m, 0, 0, W->m.w, W->m.h);
}

static void class_one(TASIM_MOVECLASS* c, int16_t fx, int16_t fz)
{
    tasim_moveclass_init(c);
    c->present = 1;
    c->footx = fx;
    c->footz = fz;
    c->maxslope = 20;
    c->badslope = 10;
    c->maxwaterslope = 20;
    c->badwaterslope = 10;
}

static void classes_make(WORLD* W)
{
    int bad;
    size_t n = ts_class_bytes(W->cls, W->m.w, W->m.h);
    W->units.u = W->u;
    W->units.n = 8;
    W->cmem = malloc(n);
    CHECK(W->cmem && ts_class_init(&W->cm, W->cmem, n, W->cls, &W->m, &W->units, &bad) == TS_CLASS_OK, "classes");
}

static void world_free(WORLD* W)
{
    free(W->mem);
    free(W->cmem);
}

/* ---- the ground height ----------------------------------------------------------------------------------------- */

static void test_ground(void)
{
    static const uint8_t rows[2][4] = { { 10, 3, 200, 0 }, { 20, 40, 0, 255 } };
    WORLD W;
    int32_t x, z;
    map_make(&W, 4, 9, 0, 0, NULL, 0);
    for (z = 0; z < 2; z++)
        for (x = 0; x < 4; x++) W.m.height[z * 4 + x] = rows[z][x];
#define PX(v) ((int32_t)((uint32_t)(v) << 16))
    /* (5, 8): top = 10 + (-35 / 16 = -2) = 8, bot = 20 + 100 / 16 = 26, 8 + 144 / 16 = 17 (floor would give 7, 16) */
    CHECK(ts_map_ground(&W.m, PX(5), PX(8)) == 17, "ground (5, 8) = %d", ts_map_ground(&W.m, PX(5), PX(8)));
    /* the fraction words are not read */
    CHECK(ts_map_ground(&W.m, PX(5) | 0xFFFF, PX(8) | 0x8000) == 17, "ground fraction");
    /* (47, 0): 200 + (-3000 / 16 = -187) = 13 (floor: 12) */
    CHECK(ts_map_ground(&W.m, PX(47), 0) == 13, "ground (47, 0) = %d", ts_map_ground(&W.m, PX(47), 0));
    /* (40, 12): top 200 - 100 = 100, bot 2040 / 16 = 127, 100 + 324 / 16 = 120 */
    CHECK(ts_map_ground(&W.m, PX(40), PX(12)) == 120, "ground (40, 12) = %d", ts_map_ground(&W.m, PX(40), PX(12)));
    /* (24, 4): top 3 + 1576 / 16 = 101, bot 40 - 20 = 20, 101 + (-324 / 16 = -20) = 81 (floor: 80) */
    CHECK(ts_map_ground(&W.m, PX(24), PX(4)) == 81, "ground (24, 4) = %d", ts_map_ground(&W.m, PX(24), PX(4)));
    /* off the grid: a negative integer word, the last column or row as the cell's own, a word that wraps */
    CHECK(ts_map_ground(&W.m, -1, PX(8)) == -1, "ground x -1/65536");
    CHECK(ts_map_ground(&W.m, PX(-1), PX(8)) == -1, "ground x -1");
    CHECK(ts_map_ground(&W.m, PX(48), PX(8)) == -1, "ground x 48 (cell 3 of 4)");
    CHECK(ts_map_ground(&W.m, PX(47), PX(128)) == -1, "ground z 128 (cell 8 of 9)");
    CHECK(ts_map_ground(&W.m, PX(47), PX(127)) == 0, "ground z 127");
    CHECK(ts_map_ground(&W.m, PX(0x8000), PX(8)) == -1, "ground x word 0x8000");
#undef PX
    world_free(&W);
}

/* ---- the fill, the refresh and the ring ------------------------------------------------------------------------ */

/* the cell rule's codes by brute force, a cell off the given rectangle counting 0 */
static int code_in(const WORLD* W, int i, int32_t x, int32_t z, int32_t w, int32_t h)
{
    if (x < 0 || z < 0 || x >= w || z >= h) return 0;
    return ts_class_cell(&W->cm, i, z * W->m.w + x);
}

/* entry (x, z) by the ring rule over the cells of [0, w) x [0, h): 0 when a footprint cell is 0, 3 when the
   footprint and its ring are all 3, else 1 -- the fill with w, h = W, H; the refresh with W - 1, H - 1, because
   0x47DFC0 refuses the last column and row */
static int ring_rule(const WORLD* W, int i, int32_t x, int32_t z, int32_t w, int32_t h)
{
    const int32_t fx = W->cm.c[i].fx, fz = W->cm.c[i].fz;
    int32_t a, b, lo = 3;
    for (b = z; b < z + fz; b++)
        for (a = x; a < x + fx; a++) {
            const int c = code_in(W, i, a, b, w, h);
            if (c < lo) lo = c;
        }
    if (lo < 3) return lo;
    for (b = z - 1; b <= z + fz; b++)
        for (a = x - 1; a <= x + fx; a++)
            if (code_in(W, i, a, b, w, h) != 3) return 1;
    return 3;
}

static void test_edges(void)
{
    WORLD W;
    int32_t x, z;
    map_make(&W, 12, 12, 0, 0, NULL, 0);
    map_flat(&W, 100);
    /* the strip LoadMap never computes holds 0, which reads as flat */
    class_one(&W.cls[3], 2, 2);
    classes_make(&W);
    ts_class_fill_all(&W.cm);
    for (z = 0; z < 12; z++)
        for (x = 0; x < 12; x++) {
            const int f = ts_class_code(&W.cm, 3, x, z), r = ts_class_refresh(&W.cm, 3, x, z);
            int wf, wr;
            if (x + 2 > 12 || z + 2 > 12) wf = 0;                               /* the footprint off the map    */
            else if (x == 0 || z == 0 || x + 2 == 12 || z + 2 == 12) wf = 1;    /* only the ring off it         */
            else wf = 3;
            if (x + 2 >= 12 || z + 2 >= 12) wr = 0;                             /* the footprint on the last    */
            else if (x == 0 || z == 0 || x + 2 == 11 || z + 2 == 11) wr = 1;    /* the ring off or on the last  */
            else wr = 3;
            CHECK(f == wf, "fill (%d, %d) = %d, want %d", x, z, f, wf);
            CHECK(r == wr, "refresh (%d, %d) = %d, want %d", x, z, r, wr);
        }
    /* the two bands, named: the footprint on the last column (fill 1, refresh 0), the ring on it (fill 3, refresh 1),
       and the ring off the near edge, where both answer 1 and never 0 */
    CHECK(ts_class_code(&W.cm, 3, 10, 5) == 1 && ts_class_refresh(&W.cm, 3, 10, 5) == 0, "footprint band");
    CHECK(ts_class_code(&W.cm, 3, 9, 5) == 3 && ts_class_refresh(&W.cm, 3, 9, 5) == 1, "ring band");
    CHECK(ts_class_code(&W.cm, 3, 0, 5) == 1 && ts_class_refresh(&W.cm, 3, 0, 5) == 1, "ring off the near edge");
    CHECK(ts_class_code(&W.cm, 3, 12, 5) == 0 && ts_class_code(&W.cm, 3, -1, 5) == 0, "code off the map");
    world_free(&W);
}

static void test_ring(void)
{
    WORLD W;
    int32_t x, z, i;
    for (i = 0; i < 2; i++) {
        const int blocked = i == 1;
        map_make(&W, 14, 14, 0, 0, NULL, 0);
        map_flat(&W, 100);
        class_one(&W.cls[0], 2, 2);
        classes_make(&W);
        /* cell (7, 7): a slope of 15 is bad (above 10, within 20), one of 30 blocks */
        W.m.hmax[7 * 14 + 7] = (uint8_t)(blocked ? 130 : 115);
        ts_class_fill_all(&W.cm);
        CHECK(ts_class_cell(&W.cm, 0, 7 * 14 + 7) == (blocked ? 0 : 1), "the cell's code");
        for (z = 1; z < 11; z++)
            for (x = 1; x < 11; x++) {
                const int in_foot = x <= 7 && 7 < x + 2 && z <= 7 && 7 < z + 2;
                const int in_ring = x - 1 <= 7 && 7 <= x + 2 && z - 1 <= 7 && 7 <= z + 2;
                const int want = in_foot ? (blocked ? 0 : 1) : in_ring ? 1 : 3;
                CHECK(ts_class_code(&W.cm, 0, x, z) == want, "%s: fill (%d, %d) = %d, want %d",
                      blocked ? "blocked" : "bad", x, z, ts_class_code(&W.cm, 0, x, z), want);
                CHECK(ts_class_refresh(&W.cm, 0, x, z) == want, "refresh (%d, %d)", x, z);
            }
        /* the ring's corners: (7, 7) is entry (8, 8)'s top-left ring corner and entry (5, 5)'s bottom-right */
        CHECK(ts_class_code(&W.cm, 0, 8, 8) == 1 && ts_class_code(&W.cm, 0, 5, 5) == 1, "ring corners");
        CHECK(ts_class_code(&W.cm, 0, 9, 9) == 3 && ts_class_code(&W.cm, 0, 4, 4) == 3, "past the ring");
        world_free(&W);
    }
}

/* random cells and classes: the fill against the ring rule over the map, the refresh against it over the map less
   its last column and row, for every entry of every footprint of 1..4 a side */
static void test_random_fill(void)
{
    static TASIM_FEATUREDEF fd[2];
    int round;
    memset(fd, 0, sizeof fd);
    fd[0].footx = fd[0].footz = 1;
    fd[0].flags = 1 | 0x40;                         /* a blocking GAF */
    fd[1].footx = fd[1].footz = 1;
    fd[1].flags = 1;                                /* a GAF that does not block */
    for (round = 0; round < 40; round++) {
        WORLD W;
        const int32_t w = 9 + pick(30), h = 9 + pick(25);
        int32_t i, x, z;
        map_make(&W, w, h, (uint8_t)pick(60), 0, fd, 2);
        for (i = 0; i < W.m.cells; i++) {
            W.m.hmin[i] = (uint8_t)pick(120);
            W.m.hmax[i] = (uint8_t)(W.m.hmin[i] + pick(30));
            if (!pick(9)) W.m.fdef[i] = (uint16_t)pick(2);
            if (!pick(40)) W.m.fdef[i] = (uint16_t)TS_F_BORDER;
        }
        for (i = 0; i < 4; i++) {
            class_one(&W.cls[i * 7], (int16_t)(1 + pick(4)), (int16_t)(1 + pick(4)));
            W.cls[i * 7].maxwaterdepth = (int16_t)(pick(80) - 10);
            W.cls[i * 7].minwaterdepth = (int16_t)(-pick(80));
        }
        classes_make(&W);
        ts_class_fill_all(&W.cm);
        for (i = 0; i < TS_CLASSES; i++) {
            if (!W.cm.c[i].present) continue;
            for (z = 0; z < h; z++)
                for (x = 0; x < w; x++) {
                    CHECK(ts_class_code(&W.cm, i, x, z) == ring_rule(&W, i, x, z, w, h), "round %d class %d fill "
                          "(%d, %d) %dx%d", round, i, x, z, W.cm.c[i].fx, W.cm.c[i].fz);
                    CHECK(ts_class_refresh(&W.cm, i, x, z) == ring_rule(&W, i, x, z, w - 1, h - 1), "round %d "
                          "class %d refresh (%d, %d)", round, i, x, z);
                }
        }
        world_free(&W);
    }
}

/* ---- units: the rule, the claim, the watermark, the cleanup ---------------------------------------------------- */

static void unit_put(WORLD* W, int32_t slot, int16_t x, int16_t z, int mover, uint32_t claim)
{
    TS_CUNIT* u = &W->u[slot];
    u->cx = x;
    u->cz = z;
    u->sx = u->sz = 1;
    u->mover = (uint8_t)mover;
    u->alive = 1;
    u->claim = claim;
    W->m.slot_a[z * W->m.w + x] = (uint16_t)slot;
}

static void test_units(void)
{
    WORLD W;
    map_make(&W, 16, 16, 0, 0, NULL, 0);
    map_flat(&W, 100);
    class_one(&W.cls[1], 1, 1);
    classes_make(&W);
    unit_put(&W, 1, 5, 5, 1, 0);                    /* a mover stamped at GameTime 0                              */
    unit_put(&W, 2, 10, 10, 1, 0);                  /* the searcher                                               */
    unit_put(&W, 3, 3, 12, 0, 0);                   /* a building                                                 */
    ts_class_fill_all(&W.cm);
    CHECK(ts_class_code(&W.cm, 1, 5, 5) == 3, "a mover under watermark 0 is no obstacle");
    CHECK(ts_class_code(&W.cm, 1, 3, 12) == 0 && ts_class_code(&W.cm, 1, 2, 11) == 1, "a building blocks");

    /* the search at GameTime 100: watermark 70; unit 1 (stamp 0) becomes an obstacle; the searcher's own entries are
       not re-marked and its stamp is put back */
    ts_class_watermark(&W.cm, 1, 2, 100);
    CHECK(W.cm.c[1].wm == 70, "watermark %u", W.cm.c[1].wm);
    CHECK(W.u[2].claim == 0, "the searcher's stamp put back");
    CHECK(ts_class_code(&W.cm, 1, 5, 5) == 0, "unit 1 an obstacle");
    CHECK(ts_class_code(&W.cm, 1, 4, 4) == 1 && ts_class_code(&W.cm, 1, 6, 6) == 1, "its ring");
    CHECK(ts_class_code(&W.cm, 1, 10, 10) == 3, "the searcher is not its own obstacle during its search");
    ts_class_cleanup(&W.cm, 1, 2);
    CHECK(ts_class_code(&W.cm, 1, 10, 10) == 0, "the cleanup makes the searcher an obstacle again");

    /* a second search at the same watermark: the searcher (stamp 0 < 70) re-marks its rectangle with its stamp set */
    ts_class_watermark(&W.cm, 1, 2, 100);
    CHECK(ts_class_code(&W.cm, 1, 10, 10) == 3 && W.u[2].claim == 0, "the searcher re-marked free");

    /* the claim at GameTime 120: the old stamp 0 is below the watermark, so the rectangle is re-marked, free */
    ts_class_claim(&W.cm, 1, 120);
    CHECK(W.u[1].claim == 120 && ts_class_code(&W.cm, 1, 5, 5) == 3, "the claim erases the obstacle");
    /* unit 4 re-marked free at stamp 90, then its stamp lowered to 10 with no re-mark: an entry the next search must
       leave stale, a stamp below the old watermark being an obstacle already */
    unit_put(&W, 4, 12, 3, 1, 90);
    ts_class_remark_all(&W.cm, 12, 3, 1, 1);
    W.u[4].claim = 10;
    /* a later search walks stamps in [70, 130): 120 is one, 10 is not */
    ts_class_watermark(&W.cm, 1, 2, 160);
    CHECK(W.cm.c[1].wm == 130 && ts_class_code(&W.cm, 1, 5, 5) == 0, "stamp 120 under watermark 130");
    CHECK(ts_class_code(&W.cm, 1, 12, 3) == 3, "stamp 10 is below the old watermark: not walked");
    /* a claim whose old stamp is at or above the watermark re-marks nothing: the entry keeps its stale 0 */
    W.u[1].claim = 140;
    ts_class_claim(&W.cm, 1, 150);
    CHECK(W.u[1].claim == 150 && ts_class_code(&W.cm, 1, 5, 5) == 0, "a claim above the watermark re-marks nothing");
    /* a dead unit is not walked: free again at stamp 140, a search to watermark 170 leaves it free */
    W.u[1].claim = 140;
    W.u[1].alive = 0;
    ts_class_remark_all(&W.cm, 5, 5, 1, 1);
    CHECK(ts_class_code(&W.cm, 1, 5, 5) == 3, "stamp 140 over watermark 130");
    ts_class_watermark(&W.cm, 1, 2, 200);
    CHECK(W.cm.c[1].wm == 170 && ts_class_code(&W.cm, 1, 5, 5) == 3, "a dead unit's rectangle is not walked");
    /* a building's claim re-marks every present class; a slot past the view blocks */
    W.m.slot_a[3 * 16 + 3] = 99;
    ts_class_claim(&W.cm, 3, 210);
    CHECK(ts_class_code(&W.cm, 1, 3, 12) == 0 && W.u[3].claim == 0, "a building's claim keeps its stamp");
    ts_class_remark_all(&W.cm, 3, 3, 1, 1);
    CHECK(ts_class_code(&W.cm, 1, 3, 3) == 0, "a slot past the units blocks");
    world_free(&W);
}

/* a class index, a cell or a slot from outside the tables answers 0 and writes nothing (ASan sees the reads) */
static void test_bounds(void)
{
    WORLD W;
    uint32_t before;
    map_make(&W, 10, 10, 0, 0, NULL, 0);
    map_flat(&W, 100);
    class_one(&W.cls[31], 1, 1);
    classes_make(&W);
    ts_class_fill_all(&W.cm);
    before = W.cm.c[31].map[0];
    CHECK(ts_class_cell(&W.cm, TS_CLASSES, 0) == 0 && ts_class_cell(&W.cm, -1, 0) == 0, "cell: class index");
    CHECK(ts_class_cell(&W.cm, 31, -1) == 0 && ts_class_cell(&W.cm, 31, 100) == 0, "cell: cell index");
    CHECK(ts_class_rect(&W.cm, 32, 1, 1, 1, 1) == 0 && ts_class_refresh(&W.cm, -5, 1, 1) == 0, "rect, refresh");
    CHECK(ts_class_code(&W.cm, 1000, 1, 1) == 0, "code");
    ts_class_fill(&W.cm, TS_CLASSES);
    ts_class_remark(&W.cm, -1, 0, 0, 3, 3);
    ts_class_watermark(&W.cm, 32, 1, 500);
    ts_class_cleanup(&W.cm, -2, 1);
    ts_class_claim(&W.cm, 9, 500);
    CHECK(W.cm.c[31].map[0] == before && W.cm.c[31].wm == 0, "nothing written");
    world_free(&W);
}

/* the re-mark's far ends clamp by an unsigned compare: a far end below 0 recomputes the whole width */
static void test_remark_clamp(void)
{
    WORLD W;
    int32_t x, z, d;
    map_make(&W, 20, 16, 0, 0, NULL, 0);
    map_flat(&W, 100);
    class_one(&W.cls[2], 1, 1);
    classes_make(&W);
    for (d = 0; d < (int32_t)(W.cm.rows16 * W.cm.w); d++) W.cm.c[2].map[d] = 0xAAAAAAAAu;   /* code 2 everywhere */
    ts_class_remark(&W.cm, 2, -40, 3, 1, 1);        /* x in [0, 20) (x + w + 1 = -38), z in [2, 5)               */
    for (z = 0; z < 16; z++)
        for (x = 0; x < 20; x++) {
            const int c = ts_class_code(&W.cm, 2, x, z);
            CHECK((z >= 2 && z < 5) ? c != 2 : c == 2, "remark clamp (%d, %d) = %d", x, z, c);
        }
    world_free(&W);
}

/* ---- the grid: spawns, destroys, the pool, the mask, the metal, LoadMap's order ------------------------------- */

static TASIM_FEATUREDEF FD[5];

static void features_make(void)
{
    memset(FD, 0, sizeof FD);
    FD[0].footx = FD[0].footz = 2;
    FD[0].flags = 1 | 0x40;                         /* a 2x2 blocking GAF                                         */
    FD[1].footx = FD[1].footz = 1;                  /* a 1x1 3DO                                                  */
    FD[2].footx = FD[2].footz = 1;
    FD[2].flags = 1 | 0x200;                        /* an indestructible GAF with metal                           */
    FD[2].metal = 50.0f;
    FD[3].footx = 3;
    FD[3].footz = 1;
    FD[3].flags = 1;                                /* a 3x1 GAF                                                  */
    FD[4].footx = FD[4].footz = 1;
    FD[4].flags = 1;
    FD[4].metal = 70.0f;                            /* metal, destructible: never on the grid                     */
}

#define AT(W, x, z) ((z) * (W).m.w + (x))

static void test_spawn(void)
{
    WORLD W;
    features_make();
    map_make(&W, 10, 10, 0, 2, FD, 5);
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 2, 2), 0, TS_OWNER_NONE) == 1, "spawn 2x2");
    CHECK(W.m.fdef[AT(W, 2, 2)] == 0 && W.m.back[AT(W, 2, 2)] == 0 && (W.m.flags[AT(W, 2, 2)] & 0x79) == 0x50,
          "the GAF anchor: word, no record, owner 10");
    CHECK(W.m.fdef[AT(W, 3, 3)] == TS_F_FOOT && W.m.back[AT(W, 3, 3)] == 0x0101, "a footprint cell's dz, dx");
    CHECK(W.m.fdef[AT(W, 3, 2)] == TS_F_FOOT && W.m.back[AT(W, 3, 2)] == 0x0100, "dx in +0x0B");
    /* a 3DO onto a footprint cell destroys the whole 2x2 through its anchor and takes record 0 */
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 3, 3), 1, TS_OWNER_NONE) == 1, "spawn 3DO");
    CHECK(W.m.fdef[AT(W, 2, 2)] == TS_F_NONE && W.m.fdef[AT(W, 3, 2)] == TS_F_NONE && W.m.fdef[AT(W, 2, 3)] ==
          TS_F_NONE, "the 2x2 destroyed through its anchor");
    CHECK(W.m.fdef[AT(W, 3, 3)] == 1 && W.m.back[AT(W, 3, 3)] == 0 && (W.m.flags[AT(W, 3, 3)] & 1), "record 0");
    /* a spawn refused on an indestructible keeps what it destroyed before it, and the record comes back first */
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 6, 6), 2, TS_OWNER_NONE) == 1, "indestructible");
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 5, 6), 1, TS_OWNER_NONE) == 1 && W.m.back[AT(W, 5, 6)] == 1, "record 1");
    CHECK(W.m.pool_top == 0, "the pool is empty");
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 8, 8), 1, TS_OWNER_NONE) == 0, "no record left");
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 4, 6), 3, TS_OWNER_NONE) == 0, "3x1 refused on the indestructible");
    CHECK(W.m.fdef[AT(W, 5, 6)] == TS_F_NONE && W.m.fdef[AT(W, 6, 6)] == 2 && W.m.fdef[AT(W, 4, 6)] == TS_F_NONE,
          "the 3DO destroyed before the refusal stays destroyed");
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 8, 8), 1, TS_OWNER_NONE) == 1 && W.m.back[AT(W, 8, 8)] == 1,
          "the freed record is the pool's head again");
    CHECK(ts_map_destroy(&W.m, NULL, AT(W, 6, 6), 0) == 0 && ts_map_destroy(&W.m, NULL, AT(W, 6, 6), 1) == 1,
          "force destroys the indestructible");
    /* the far edge: a footprint ending on the last column is accepted, one past it refused */
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 8, 0), 0, TS_OWNER_NONE) == 1, "2x2 at column 8 of 10");
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 9, 4), 0, TS_OWNER_NONE) == 0, "2x2 at column 9 of 10");
    /* the void marker is written alone and refuses whatever comes after it */
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 0, 9), TS_F_VOID, TS_OWNER_NONE) == 0 && W.m.fdef[AT(W, 0, 9)] == TS_F_VOID,
          "void");
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 0, 9), 3, TS_OWNER_NONE) == 0, "void refuses a spawn");
    /* a word past the definitions is counted and placed nowhere */
    CHECK(ts_map_spawn(&W.m, NULL, AT(W, 1, 1), 5, TS_OWNER_NONE) == 0 && W.m.past_defs == 1, "past the defs");
    world_free(&W);
    /* a negative count is no definitions: every word is past them */
    map_make(&W, 10, 10, 0, 2, FD, -1);
    CHECK(W.m.nfd == 0 && ts_map_spawn(&W.m, NULL, AT(W, 1, 1), 0, TS_OWNER_NONE) == 0 && W.m.past_defs == 1,
          "a negative definition count");
    world_free(&W);
}

static void test_mask(void)
{
    WORLD W;
    int32_t z;
    features_make();
    map_make(&W, 6, 12, 40, 0, FD, 5);
    ts_map_spawn(&W.m, NULL, AT(W, 4, 0), 2, TS_OWNER_NONE);    /* an anchor in the masked columns           */
    ts_map_spawn(&W.m, NULL, AT(W, 3, 1), 0, TS_OWNER_NONE);    /* a 2x2 whose right half is in them         */
    for (z = 0; z < 12; z++) W.m.height[AT(W, 0, z)] = 255;     /* column 0 high: pass 2 masks rows 0..7     */
    ts_map_extremes(&W.m, 0, 0, 6, 12);
    ts_map_mask(&W.m, 0);
    CHECK(W.m.ext_w == 6 * 16 - 0x20 && W.m.ext_h == 12 * 16 - 0x80, "the extent");
    CHECK(W.m.fdef[AT(W, 4, 0)] == 2 && W.m.fdef[AT(W, 5, 0)] == TS_F_BORDER, "pass 1 leaves an anchor");
    CHECK(W.m.fdef[AT(W, 3, 1)] == 0 && W.m.fdef[AT(W, 4, 1)] == TS_F_BORDER && W.m.fdef[AT(W, 3, 2)] == TS_F_FOOT,
          "pass 1 masks a footprint cell");
    for (z = 0; z < 12; z++) {
        /* pass 2, column 0: row * 16 - 127 < 0 for rows 0..7 */
        CHECK((W.m.fdef[AT(W, 0, z)] == TS_F_BORDER) == (z <= 7), "pass 2 row %d", z);
        /* pass 3, column 1 (height 0): rows 11..5 test above ext_h 64, each masking the row above it */
        CHECK((W.m.fdef[AT(W, 1, z)] == TS_F_BORDER) == (z >= 4 && z <= 10), "pass 3 row %d", z);
    }
    CHECK(W.m.fdef[AT(W, 1, 11)] == TS_F_NONE, "pass 3 never masks the last row");
    /* lava: every cell whose 2x2 minimum is at or below the sea */
    W.m.height[AT(W, 2, 1)] = 41;
    W.m.height[AT(W, 3, 1)] = 41;
    W.m.height[AT(W, 2, 2)] = 41;
    W.m.height[AT(W, 3, 2)] = 41;
    ts_map_extremes(&W.m, 0, 0, 6, 12);
    ts_map_mask(&W.m, 1);
    CHECK(W.m.fdef[AT(W, 2, 1)] == TS_F_NONE && W.m.fdef[AT(W, 2, 0)] == TS_F_BORDER, "lava");
    world_free(&W);
}

static void test_load(void)
{
    WORLD W;
    TS_MAPLOAD in;
    TS_MAPFEAT sch[3];
    uint8_t attr[10 * 12 * 4];
    int32_t i;
    features_make();
    map_make(&W, 10, 12, 0, 4, FD, 5);
    for (i = 0; i < 120; i++) {
        attr[i * 4] = (uint8_t)(50 + (i % 10) * 3);   /* a slope along x                                       */
        attr[i * 4 + 1] = 0xFF;
        attr[i * 4 + 2] = 0xFF;
        attr[i * 4 + 3] = 0;
    }
    attr[AT(W, 1, 1) * 4 + 1] = 0xFC;               /* the void marker                                           */
    attr[AT(W, 3, 2) * 4 + 1] = 0;                  /* a 2x2 GAF                                                 */
    attr[AT(W, 3, 2) * 4 + 2] = 0;
    attr[AT(W, 4, 3) * 4 + 1] = 2;                  /* indestructible metal on the 2x2's corner                  */
    attr[AT(W, 4, 3) * 4 + 2] = 0;
    attr[AT(W, 6, 6) * 4 + 1] = 4;                  /* destructible metal: no grid metal                         */
    attr[AT(W, 6, 6) * 4 + 2] = 0;
    attr[AT(W, 5, 2) * 4 + 1] = 9;                  /* a word past the definitions                               */
    attr[AT(W, 5, 2) * 4 + 2] = 0;
    sch[0].def = 1, sch[0].x = 2, sch[0].z = 8;     /* a 3DO: footx / 2 = 0, so at (2, 8)                       */
    sch[1].def = 0, sch[1].x = 1, sch[1].z = 1;     /* onto the void marker: refused                             */
    sch[2].def = -1, sch[2].x = 5, sch[2].z = 5;    /* a name emptied by a negative position                     */
    memset(&in, 0, sizeof in);
    in.version = 0x2000;
    in.attr = attr;
    in.surface_metal = 3;
    in.schema = sch;
    in.nschema = 3;
    CHECK(ts_map_load(&W.m, &in) == 0, "load");
    CHECK(W.m.fdef[AT(W, 1, 1)] == TS_F_VOID, "the void marker");
    CHECK(W.m.fdef[AT(W, 3, 2)] == TS_F_NONE && W.m.fdef[AT(W, 4, 3)] == 2, "the 2x2 destroyed by a later spawn");
    CHECK(W.m.metal[AT(W, 4, 3)] == 50 && W.m.metal[AT(W, 6, 6)] == 3 && W.m.metal[AT(W, 0, 0)] == 3, "metal");
    CHECK(W.m.fdef[AT(W, 2, 8)] == 1 && W.m.back[AT(W, 2, 8)] == 0, "the schema's 3DO takes record 0");
    CHECK(W.m.past_defs == 1 && W.m.fdef[AT(W, 5, 2)] == TS_F_NONE, "the word past the definitions");
    CHECK(W.m.hmin[AT(W, 2, 2)] == 56 && W.m.hmax[AT(W, 2, 2)] == 59, "the 2x2 extremes");
    CHECK(W.m.hmin[AT(W, 9, 2)] == 0 && W.m.hmax[AT(W, 3, 11)] == 0, "the last column and row are not computed");
    CHECK(W.m.fdef[AT(W, 8, 0)] == TS_F_BORDER && W.m.fdef[AT(W, 9, 5)] == TS_F_BORDER, "the mask ran");
    CHECK((W.m.flags[AT(W, 0, 0)] & 0x7B) == 0x50, "the owner bits");
    in.version = 0x1234;
    CHECK(ts_map_load(&W.m, &in) == -1, "an unknown version");
    world_free(&W);
}

/* ---- the digest ---------------------------------------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    while (n--) h = (h ^ *b++) * 0x100000001B3ull;
    return h;
}

static uint64_t digest(void)
{
    static TASIM_FEATUREDEF fd[6];
    uint64_t h = 0xCBF29CE484222325ull;
    int round;
    memset(fd, 0, sizeof fd);
    fd[0].footx = 2, fd[0].footz = 2, fd[0].flags = 1 | 0x40;
    fd[1].footx = 1, fd[1].footz = 1;
    fd[2].footx = 3, fd[2].footz = 3, fd[2].flags = 1 | 0x200, fd[2].metal = 120.0f;
    fd[3].footx = 2, fd[3].footz = 1, fd[3].flags = 0x40;
    fd[4].footx = 1, fd[4].footz = 4, fd[4].flags = 1;
    fd[5].footx = 0, fd[5].footz = 0, fd[5].flags = 1 | 0x40;
    for (round = 0; round < 60; round++) {
        WORLD W;
        TS_MAPLOAD in;
        TS_MAPFEAT sch[4];
        const int32_t w = 9 + pick(50), mh = 9 + pick(50);
        uint8_t* attr = (uint8_t*)malloc((size_t)w * mh * 4);
        uint8_t cell[13];
        int32_t i, k;
        map_make(&W, w, mh, (uint8_t)pick(100), 1 + pick(30), fd, 6);
        for (i = 0; i < w * mh; i++) {
            const uint32_t word = pick(12) == 0 ? (uint32_t)pick(7) : pick(60) == 0 ? 0xFFFCu : 0xFFFFu;
            attr[i * 4] = (uint8_t)(i ? (attr[(i - 1) * 4] + pick(9) - 4) : pick(256));
            attr[i * 4 + 1] = (uint8_t)word;
            attr[i * 4 + 2] = (uint8_t)(word >> 8);
            attr[i * 4 + 3] = 0;
        }
        for (k = 0; k < 4; k++) {
            sch[k].def = pick(7) - 1;
            sch[k].x = pick(w + 4) - 2;
            sch[k].z = pick(mh + 4) - 2;
        }
        memset(&in, 0, sizeof in);
        in.version = 0x2000;
        in.attr = attr;
        in.surface_metal = pick(300) - 20;
        in.lava = pick(4) == 0;
        in.schema = sch;
        in.nschema = 4;
        CHECK(ts_map_load(&W.m, &in) == 0, "digest load");
        for (k = 0; k < 6; k++) {
            class_one(&W.cls[pick(TS_CLASSES)], (int16_t)pick(5), (int16_t)pick(5));
        }
        for (i = 0; i < TS_CLASSES; i++)
            if (W.cls[i].present) {
                W.cls[i].maxwaterdepth = (int16_t)(pick(200) - 50);
                W.cls[i].minwaterdepth = (int16_t)(-pick(100));
                W.cls[i].maxslope = (uint8_t)pick(40);
                W.cls[i].badslope = (uint8_t)pick(W.cls[i].maxslope + 1);
            }
        classes_make(&W);
        for (k = 1; k < 8; k++) {
            TS_CUNIT* u = &W.u[k];
            u->cx = (int16_t)pick(w);
            u->cz = (int16_t)pick(mh);
            u->sx = (int16_t)(1 + pick(3));
            u->sz = (int16_t)(1 + pick(3));
            u->mover = (uint8_t)(pick(4) != 0);
            u->alive = (uint8_t)(pick(6) != 0);
            u->claim = (uint32_t)pick(200);
            W.m.slot_a[u->cz * w + u->cx] = (uint16_t)k;
        }
        ts_class_fill_all(&W.cm);
        for (k = 0; k < 30; k++) {
            int32_t c = pick(TS_CLASSES);
            const uint32_t now = (uint32_t)(k * 13 + pick(20));
            switch (pick(5)) {
            case 0: ts_class_claim(&W.cm, 1 + pick(7), now); break;
            case 1: ts_class_watermark(&W.cm, c, 1 + pick(7), now); break;
            case 2: ts_class_cleanup(&W.cm, c, 1 + pick(7)); break;
            case 3: ts_map_spawn(&W.m, &W.cm, pick(w * mh), (uint32_t)pick(6), TS_OWNER_NONE); break;
            default: ts_map_destroy(&W.m, &W.cm, pick(w * mh), pick(2)); break;
            }
        }
        for (i = 0; i < w * mh; i++) {
            ts_map_cell(&W.m, i, cell);
            h = fnv(h, cell, 13);
        }
        for (i = 0; i < TS_CLASSES; i++)
            if (W.cm.c[i].present) {
                h = fnv(h, W.cm.c[i].map, 4 * (size_t)W.cm.rows16 * (size_t)W.cm.w);
                h = fnv(h, &W.cm.c[i].wm, 4);
            }
        h = fnv(h, &W.m.past_defs, 4);
        free(attr);
        world_free(&W);
    }
    return h;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "digest")) {
        const uint64_t d = digest();
        printf("digest %08x%08x\n", (unsigned)(d >> 32), (unsigned)d);
        return fails ? 1 : 0;
    }
    test_ground();
    test_edges();
    test_ring();
    test_random_fill();
    test_units();
    test_remark_clamp();
    test_bounds();
    test_spawn();
    test_mask();
    test_load();
    printf(fails ? "tasim_class_test: %d failure(s)\n" : "tasim_class_test: ok\n", fails);
    return fails ? 1 : 0;
}
