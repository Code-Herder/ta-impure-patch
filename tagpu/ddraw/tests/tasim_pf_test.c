/* tasim_pf_test.c -- the self-test of the core's pathfinder (src/tasim_path.h), driven by tests/tasim_pf_test.py,
   which builds it natively, under the sanitizers and with the DLL's i686 compiler under Wine. It includes
   src/tasim_path.c itself, so the hand-checked cases can call the expansion, the heap and the bug walk directly.

     tasim_pf_test            the rules, each against a hand-checked case on a 12 x 12 map: the turn penalty and the
                              75 within 5 straight steps, the bad cell's 30 and the unexplored cell's 0, the fan of 2
                              that cannot turn back, the lazy top, the heap's tie rules, the goals' methods, set path,
                              the bug walk on the U, L and box maps; then the engine's loop examining one slot at a
                              time against the skip-ahead on 10^4 random worlds; exit 1 on a failure
     tasim_pf_test digest     one 64-bit FNV-1a digest of the random worlds' final states; the driver requires every
                              build to print the same */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tasim_path.c"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                                       printf("\n"); } } while (0)

/* a fixed generator, so every build draws the same worlds (xorshift64*) */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
}
static int32_t pick(int32_t n) { return (int32_t)(next() % (uint64_t)n); }

/* ---- the world ------------------------------------------------------------------------------------------------- */

#define NGOAL 8

typedef struct {
    TS_MAP m;
    void* mem;
    TASIM_MOVECLASS cls[TS_CLASSES];
    TS_CLASSMAPS cm;
    void* cmem;
    TS_CUNIT* cu;
    TS_CUNITS units;
    TS_PFSLOT* slot;
    TS_GOAL goal[NGOAL];
    uint16_t* mapped;
    TS_PF pf;
    void* pmem;
    TS_PFWORLD W;
} WORLD;

/* a w x h map of heights `ht` (NULL: flat), the classes `cls`, nslot slots, the pathfinder sized for them; the
   seats are the caller's to set before ts_pf_init (world_pf) */
static void world_make(WORLD* w, int32_t W, int32_t H, const uint8_t* ht, int32_t nslot)
{
    size_t n = ts_map_bytes(W, H, 0);
    int32_t i;
    int bad;
    w->mem = malloc(n);
    CHECK(w->mem && ts_map_init(&w->m, w->mem, n, W, H, 0, 0, NULL, 0) == TS_MAP_OK, "map %dx%d", W, H);
    for (i = 0; i < w->m.cells; i++) w->m.height[i] = ht ? ht[i] : 10;
    ts_map_extremes(&w->m, 0, 0, W, H);
    w->cu = calloc((size_t)nslot, sizeof *w->cu);
    w->slot = calloc((size_t)nslot, sizeof *w->slot);
    w->units.u = w->cu;
    w->units.n = nslot;
    n = ts_class_bytes(w->cls, W, H);
    w->cmem = malloc(n);
    CHECK(w->cmem && ts_class_init(&w->cm, w->cmem, n, w->cls, &w->m, &w->units, &bad) == TS_CLASS_OK, "classes");
    ts_class_fill_all(&w->cm);
    w->mapped = NULL;
    memset(&w->W, 0, sizeof w->W);
    w->W.cm = &w->cm;
    w->W.slot = w->slot;
    w->W.goal = w->goal;
    w->W.ngoal = NGOAL;
    memset(w->goal, 0, sizeof w->goal);
    for (i = 0; i < nslot; i++) {
        w->slot[i].cls = TS_PF_NOCLASS;
        w->slot[i].path.goal = TS_PF_NOGOAL;
    }
}

static void world_pf(WORLD* w, int32_t budget)
{
    size_t n = ts_pf_bytes(w->m.w, w->m.h, w->units.n);
    memset(&w->pf, 0, sizeof w->pf);
    w->pmem = malloc(n);
    CHECK(w->pmem && ts_pf_init(&w->pf, w->pmem, n, w->m.w, w->m.h, w->units.n, budget, w->W.seat) == 0, "pf");
}

static void world_free(WORLD* w)
{
    free(w->mem);
    free(w->cmem);
    free(w->cu);
    free(w->slot);
    free(w->pmem);
    free(w->mapped);
}

static void class_one(TASIM_MOVECLASS* c, int16_t fx, int16_t fz, uint8_t maxslope, uint8_t badslope)
{
    tasim_moveclass_init(c);
    c->present = 1;
    c->footx = fx;
    c->footz = fz;
    c->maxslope = maxslope;
    c->badslope = badslope;
    c->maxwaterslope = maxslope;
    c->badwaterslope = badslope;
}

static void code_put(WORLD* w, int i, int32_t x, int32_t z, int v)
{
    uint32_t* p = &w->cm.c[i].map[(z >> 4) * w->cm.w + x];
    const int sh = 2 * (z & 15);
    *p = (*p & ~(3u << sh)) | ((uint32_t)v << sh);
}

/* ---- the hand-checked 12 x 12 maps ------------------------------------------------------------------------------ */

/* rows of '.' good, 'b' bad, '#' blocked, 'S' the start, 'G' the goal (both good); class 0 with a footprint of 1.
   The searcher is slot 1, player 0's only slot, with a mover whose claim stamp is GameTime, so neither the watermark
   nor the cleanup re-marks a hand-set code. */
static void hand(WORLD* w, const char* const rows[12], uint16_t heading, int32_t* gx, int32_t* gz)
{
    int32_t x, z;
    memset(w, 0, sizeof *w);
    class_one(&w->cls[0], 1, 1, 20, 10);
    world_make(w, 12, 12, NULL, 2);
    for (z = 0; z < 12; z++)
        for (x = 0; x < 12; x++) {
            const char ch = rows[z][x];
            code_put(w, 0, x, z, ch == '#' ? 0 : ch == 'b' ? 1 : 3);
            if (ch == 'S') {
                w->cu[1].cx = (int16_t)x;
                w->cu[1].cz = (int16_t)z;
            }
            if (ch == 'G') {
                *gx = x;
                *gz = z;
            }
        }
    w->cu[1].sx = w->cu[1].sz = 1;
    w->cu[1].mover = w->cu[1].alive = 1;
    w->cu[1].claim = 60;
    w->slot[1].type = 1;
    w->slot[1].cls = 0;
    w->slot[1].heading = heading;
    w->slot[1].path.pflags = 2;
    w->slot[1].path.goal = 0;
    ts_goal_point(&w->goal[0], (*gx * 16) << 16, (*gz * 16) << 16, 4, 1, 1);
    w->W.seat[0].credited = 1;
    w->W.seat[0].first = w->W.seat[0].last = 1;
    w->W.players = 1;
    w->W.per_seat = 1;
    w->W.now = 60;
    world_pf(w, 100000);
    ts_pf_mark_all(&w->pf, &w->W);
}

/* the footprint centre the extraction gives cell c of a footprint 1: (2c + 1) * 8 */
#define PX(c) ((int16_t)((2 * (c) + 1) * 8))

static int path_is(const WORLD* w, const int32_t (*cells)[2], int n)
{
    const TS_PATH* p = &w->slot[1].path;
    int i;
    if (p->count != n || !(p->pflags & 1) || (p->pflags & 2)) return 0;
    for (i = 0; i < n; i++)
        if (p->wp[i][0] != PX(cells[i][0]) || p->wp[i][1] != PX(cells[i][1])) return 0;
    return 1;
}

static void path_print(const WORLD* w)
{
    const TS_PATH* p = &w->slot[1].path;
    int i;
    printf("  path count %d pflags %#x:", p->count, p->pflags);
    for (i = 0; i < p->count; i++) printf(" (%d,%d)", (p->wp[i][0] / 8 - 1) / 2, (p->wp[i][1] / 8 - 1) / 2);
    printf("\n");
}

/* the found node's g: the lazy top the last pop left at heap[0] */
static int32_t found_g(const WORLD* w) { return w->pf.pool[w->pf.heap[0].n].g; }

/* An L corridor, entered heading east. The diagonal (4,5) -> (5,6) is opened before (5,5) is popped and is cheaper
   into (5,6): 62 (turn 1: 40 + diagonal 22, straight 102 >= 5) against 16 + 76 (turn 2: 60 + 16). A cell holds one g,
   so (5,6) keeps 94 with b1 = 5, and the turn south from it costs 40 + 16 + 75 (straight 1 < 5): (5,7) is 225 where
   the corner through (5,5) would have given 140. The goal (5,8) is 241: S(2,5) 0, (3,5) 16, (4,5) 32, (5,6) 94,
   (5,7) 225, (5,8) 241. Corners back from the goal: (5,6) where b1 turns 4 -> 5, (4,5) where it turns 5 -> 6. */
static void test_turn(void)
{
    static const char* const rows[12] = {
        "############", "############", "############", "############", "############",
        "##S...######", "#####.######", "#####.######", "#####G######", "############",
        "############", "############" };
    static const int32_t want[4][2] = { { 2, 5 }, { 4, 5 }, { 5, 6 }, { 5, 8 } };
    WORLD w;
    int32_t gx, gz;
    hand(&w, rows, 0xC000, &gx, &gz);
    ts_pf_tick(&w.pf, &w.W);
    CHECK(path_is(&w, want, 4), "turn: the path");
    if (!path_is(&w, want, 4)) path_print(&w);
    CHECK(found_g(&w) == 241, "turn: the goal's g %d, want 241", found_g(&w));
    CHECK(w.pf.searching == 0 && w.goal[0].flags == TS_GOALF_WALKIN, "turn: flags %#x", w.goal[0].flags);
    world_free(&w);
}

/* A straight run east through two bad cells: 7 steps of 16 and two pens of 30, g = 172; no detour can be cheaper,
   since leaving the line costs a turn of 40 or more twice. The path has no corner. */
static void test_bad(void)
{
    static const char* const rows[12] = {
        "............", "............", "............", "............", "............",
        ".S..b.b.G...", "............", "............", "............", "............",
        "............", "............" };
    static const int32_t want[2][2] = { { 1, 5 }, { 8, 5 } };
    WORLD w;
    int32_t gx, gz;
    hand(&w, rows, 0xC000, &gx, &gz);
    ts_pf_tick(&w.pf, &w.W);
    CHECK(path_is(&w, want, 2), "bad: the path");
    if (!path_is(&w, want, 2)) path_print(&w);
    CHECK(found_g(&w) == 16 * 7 + 30 + 30, "bad: the goal's g %d, want 172", found_g(&w));
    world_free(&w);
}

/* An unexplored cell answers 2 before its code is read (0x40D80E..0x40D83B): passable, pen 0. The straight corridor
   east has a blocked cell at (5,5) whose 32-px block (2,2) is unexplored for player 0; every other block is explored.
   g = 16 * 7 = 112. */
static void test_unexplored(void)
{
    static const char* const rows[12] = {
        "############", "############", "############", "############", "############",
        "#S...#..G###", "############", "############", "############", "############",
        "############", "############" };
    static const int32_t want[2][2] = { { 1, 5 }, { 8, 5 } };
    WORLD w;
    int32_t gx, gz, i;
    hand(&w, rows, 0xC000, &gx, &gz);
    w.mapped = malloc(6 * 6 * sizeof *w.mapped);
    for (i = 0; i < 36; i++) w.mapped[i] = 1;
    w.mapped[2 * 6 + 2] = 0;
    w.W.mapped = w.mapped;
    CHECK(ts_pf_reader(&w.pf, &w.W, 5, 5) == 0, "unexplored: the reader with no class");
    w.pf.cls = 0;
    CHECK(ts_pf_reader(&w.pf, &w.W, 5, 5) == 2, "unexplored: (5,5) reads %d", ts_pf_reader(&w.pf, &w.W, 5, 5));
    CHECK(ts_pf_reader(&w.pf, &w.W, 5, 4) == 2, "unexplored: (5,4), the same block");
    CHECK(ts_pf_reader(&w.pf, &w.W, 5, 6) == 0 && ts_pf_reader(&w.pf, &w.W, 6, 4) == 0,
          "unexplored: (5,6) and (6,4) explored and blocked");
    CHECK(ts_pf_reader(&w.pf, &w.W, 6, 5) == 3, "unexplored: (6,5) explored and good");
    CHECK(ts_pf_reader(&w.pf, &w.W, 12, 5) == 0 && ts_pf_reader(&w.pf, &w.W, -1, 5) == 0, "unexplored: off the grid");
    w.pf.cls = TS_PF_NOCLASS;
    ts_pf_tick(&w.pf, &w.W);
    CHECK(path_is(&w, want, 2), "unexplored: the path");
    if (!path_is(&w, want, 2)) path_print(&w);
    CHECK(found_g(&w) == 112, "unexplored: the goal's g %d, want 112", found_g(&w));
    world_free(&w);
}

/* a node with f at the heap's end, pushed as the expansion pushes */
static uint32_t push(TS_PF* pf, int32_t f)
{
    const int32_t n = node_take(pf);
    pf->pool[n].f = f;
    pf->pool[n].link = pf->count;
    pf->heap[pf->count].f = f;
    pf->heap[pf->count++].n = (uint32_t)n;
    sift_up(pf, pf->count - 1);
    return (uint32_t)n;
}

/* node n's f, and its heap entry's copy at the node's link */
static void set_f(TS_PF* pf, uint32_t n, int32_t f)
{
    pf->pool[n].f = f;
    pf->heap[pf->pool[n].link].f = f;
}

/* the expansion with the reader's view a pass of the A* takes */
static void expand_in(TS_PF* pf, TS_PFWORLD* W, const TS_GOAL* g, const TS_PFNODE* L, int32_t pc, int k)
{
    RD rd;
    rd_open(&rd, pf, W);
    expand(pf, &rd, g, L, pc, k);
}

/* The fan: a pop expands the directions -fan .. +fan around the one its cell was reached by. A node at (5,5) reached
   heading east (b1 6) with fan 2 opens S, SE, E, NE and N -- five cells -- and leaves SW, W and NW new. With fan 4,
   the start's, all eight open, and THE ENGINE'S, KEPT: the direction straight back (k = -4 and k = 4, here west) is
   expanded twice, the second finding the cell open with the same g and relaxing nothing. The first new cell
   overwrites the popped node standing as the lazy top, so the heap holds 5, or 8, and the lazy state is over. One
   pop costs 1: from 99 the pass stops at 100. */
static void test_fan(void)
{
    static const char* const rows[12] = {
        "............", "............", "............", "............", "............",
        ".....S.....G", "............", "............", "............", "............",
        "............", "............" };
    static const int8_t open2[8] = { 1, 0, 0, 0, 1, 1, 1, 1 };   /* by direction: N NW W SW S SE E NE */
    WORLD w;
    int32_t gx, gz, d, fan;
    for (fan = 2; fan <= 4; fan += 2) {
        uint32_t n;
        hand(&w, rows, 0xC000, &gx, &gz);
        w.pf.searching = w.pf.cls_obj = 1;
        w.pf.cls = 0;
        w.pf.goal = 0;
        w.pf.wgt = 0x10000;
        w.pf.relax = -1;
        n = push(&w.pf, 0);
        w.pf.pool[n].x = 5;
        w.pf.pool[n].z = 5;
        w.pf.pool[n].straight = 100;
        w.pf.grid[cidx(&w.pf, 5, 5)].b0 = 1;
        w.pf.grid[cidx(&w.pf, 5, 5)].b1 = 6;
        w.pf.fan = fan;
        w.pf.cost = 99;
        astar(&w.pf, &w.W);
        CHECK(w.pf.cost == 100 && w.pf.fan == 2 && w.pf.lazy == 0 && w.pf.used == (fan == 4 ? 8 : 5) &&
              w.pf.grid[cidx(&w.pf, 5, 5)].b0 == 2, "fan %d: one pop", fan);
        for (d = 0; d < 8; d++) {
            const int b0 = w.pf.grid[cidx(&w.pf, 5 + DX[d], 5 + DZ[d])].b0;
            CHECK(b0 == (fan == 4 || open2[d] ? 1 : 0), "fan %d: direction %d b0 %d", fan, d, b0);
        }
        CHECK(w.pf.count == (fan == 4 ? 8 : 5), "fan %d: heap %d", fan, w.pf.count);
        world_free(&w);
    }
}

/* the expansion's arithmetic on one parent, by hand: L at (5,5) with g 100, reached heading east (b1 6); all good */
static void test_expand(void)
{
    static const char* const rows[12] = {
        "............", "............", "............", "............", "............",
        ".....S.....G", "............", "............", "............", "............",
        "......b.....", "............" };
    WORLD w;
    TS_PFNODE L;
    int32_t gx, gz, c;
    TS_PFNODE* N;
    hand(&w, rows, 0xC000, &gx, &gz);
    w.pf.cls = 0;
    w.pf.wgt = 0x10000;
    w.pf.relax = -1;
    w.pf.goal = 0;
    memset(&L, 0, sizeof L);
    L.x = 5;
    L.z = 5;
    L.g = 100;
    L.straight = 4;
    w.pf.grid[cidx(&w.pf, 5, 5)].b1 = 6;
    w.pf.grid[cidx(&w.pf, 5, 5)].b0 = 2;
    /* k = 1: direction 7, (6,4): 40 + 22 + 100 + 75 (straight 4 < 5) = 237; h = 18*5 + 7*1 - 4 = 93 */
    expand_in(&w.pf, &w.W, &w.goal[0], &L, cidx(&w.pf, 5, 5), 1);
    c = cidx(&w.pf, 6, 4);
    N = &w.pf.pool[w.pf.grid[c].node];
    CHECK(w.pf.count == 1 && N->x == 6 && N->z == 4 && N->g == 237 && N->f == 237 + 93 && N->straight == 1 &&
          N->pen == 0 && w.pf.grid[c].b0 == 1 && w.pf.grid[c].b1 == 7, "expand k=1: g %d f %d", N->g, N->f);
    /* the same at straight 5: no 75, g 162; the cell is open now, so this relaxes it: f moves by -75 */
    L.straight = 5;
    expand_in(&w.pf, &w.W, &w.goal[0], &L, cidx(&w.pf, 5, 5), 1);
    CHECK(N->g == 162 && N->f == 162 + 93 && w.pf.count == 1, "expand relax: g %d f %d", N->g, N->f);
    /* not lower: nothing changes */
    expand_in(&w.pf, &w.W, &w.goal[0], &L, cidx(&w.pf, 5, 5), 1);
    CHECK(N->g == 162, "expand no relax");
    /* k = 0 ahead: (6,5), 16 + 100 = 116, straight 6 */
    expand_in(&w.pf, &w.W, &w.goal[0], &L, cidx(&w.pf, 5, 5), 0);
    N = &w.pf.pool[w.pf.grid[cidx(&w.pf, 6, 5)].node];
    CHECK(N->g == 116 && N->straight == 6, "expand k=0: g %d straight %d", N->g, N->straight);
    /* k = 6 (-2): direction 4, (5,6): 60 + 16 + 100 = 176 */
    expand_in(&w.pf, &w.W, &w.goal[0], &L, cidx(&w.pf, 5, 5), 6);
    N = &w.pf.pool[w.pf.grid[cidx(&w.pf, 5, 6)].node];
    CHECK(N->g == 176, "expand k=-2: g %d", N->g);
    /* a bad cell: L at (5,9) heading south (b1 4), k = 7 (-1): direction 3, (4,10)? no -- direction 5, k = 1: (6,10),
       40 + 22 + 100 + 30 = 192, pen 30 */
    L.x = 5;
    L.z = 9;
    w.pf.grid[cidx(&w.pf, 5, 9)].b1 = 4;
    expand_in(&w.pf, &w.W, &w.goal[0], &L, cidx(&w.pf, 5, 9), 1);
    N = &w.pf.pool[w.pf.grid[cidx(&w.pf, 6, 10)].node];
    CHECK(N->g == 192 && N->pen == 30, "expand bad: g %d pen %d", N->g, N->pen);
    /* a goal cell by the relaxation: relax 1000 makes (4,10), k = -1 (direction 3), open as a goal (b0 5) */
    w.pf.relax = 1000;
    expand_in(&w.pf, &w.W, &w.goal[0], &L, cidx(&w.pf, 5, 9), 7);
    CHECK(w.pf.grid[cidx(&w.pf, 4, 10)].b0 == 5, "expand relax goal: b0 %d", w.pf.grid[cidx(&w.pf, 4, 10)].b0);
    /* a blocked cell dies (b0 3), unless the walk stood on it (b0 bit 3), which leaves it new */
    code_put(&w, 0, 5, 10, 0);
    w.pf.grid[cidx(&w.pf, 5, 10)].b0 = 8;
    expand_in(&w.pf, &w.W, &w.goal[0], &L, cidx(&w.pf, 5, 9), 0);
    CHECK(w.pf.grid[cidx(&w.pf, 5, 10)].b0 == 8, "expand walked blocked: b0 %d", w.pf.grid[cidx(&w.pf, 5, 10)].b0);
    w.pf.grid[cidx(&w.pf, 5, 10)].b0 = 4;
    expand_in(&w.pf, &w.W, &w.goal[0], &L, cidx(&w.pf, 5, 9), 0);
    CHECK(w.pf.grid[cidx(&w.pf, 5, 10)].b0 == 7, "expand dead goal cell: b0 %d", w.pf.grid[cidx(&w.pf, 5, 10)].b0);
    world_free(&w);
}

/* The tie rules: sift-up moves only on a strictly lower f, sift-down takes the right child only when its f is below
   the left's, and moves a child only on a strictly lower f. */
static void test_ties(void)
{
    static const char* const rows[12] = {
        "S...........", "............", "............", "............", "............", "............",
        "............", "............", "............", "............", "............", "...........G" };
    WORLD w;
    int32_t gx, gz;
    TS_PF* pf;
    hand(&w, rows, 0, &gx, &gz);
    pf = &w.pf;
    /* three equal f: nothing moves up */
    push(pf, 5);
    push(pf, 5);
    push(pf, 5);
    CHECK(pf->heap[0].n == 0 && pf->heap[1].n == 1 && pf->heap[2].n == 2, "ties: equal pushes stay in order");
    /* a lower one goes to the top; an equal to the top's stays under it */
    push(pf, 4);                                   /* node 3 at 3, parent 1 (5): up; parent 0 (5): up */
    CHECK(pf->heap[0].n == 3 && pf->heap[1].n == 0 && pf->heap[3].n == 1, "ties: the lower to the top %u %u %u",
          pf->heap[0].n, pf->heap[1].n, pf->heap[3].n);
    push(pf, 4);                                   /* node 4 at 4, parent 1 (node 0, 5): up; parent 0 (4): stays */
    CHECK(pf->heap[0].n == 3 && pf->heap[1].n == 4 && pf->heap[4].n == 0, "ties: equal to the top stays under it");
    /* sift-down: the root raised to 9 over children 4 (node 4) and 5 (node 2): the left, node 4, comes up; under it
       children 5 (node 1) and 5 (node 0): equal, so the left, node 1 */
    set_f(pf, 3, 9);
    sift_down(pf, 0);
    CHECK(pf->heap[0].n == 4 && pf->heap[1].n == 1 && pf->heap[3].n == 3, "ties: sift-down goes left on a tie: %u %u %u",
          pf->heap[0].n, pf->heap[1].n, pf->heap[3].n);
    /* a right child strictly lower is taken */
    set_f(pf, 4, 9);
    set_f(pf, 2, 3);
    sift_down(pf, 0);
    CHECK(pf->heap[0].n == 2 && pf->heap[2].n == 4, "ties: sift-down takes a lower right child");
    /* a child equal to the sinking node does not move up */
    set_f(pf, 2, 5);
    sift_down(pf, 0);
    CHECK(pf->heap[0].n == 2, "ties: an equal child stays under");
    world_free(&w);
}

/* The lazy top: a pop leaves its node at heap[0]; the next new cell overwrites it in place and sifts down, so the
   pool does not grow; a relax that lifts a node over it frees it and fills its place from the heap's last. */
static void test_lazy(void)
{
    static const char* const rows[12] = {
        "............", "............", "............", "............", "............",
        ".....S.....G", "............", "............", "............", "............",
        "............", "............" };
    WORLD w;
    TS_PFNODE L;
    TS_PF* pf;
    int32_t gx, gz;
    uint32_t a, b, c;
    hand(&w, rows, 0xC000, &gx, &gz);
    pf = &w.pf;
    pf->cls = 0;
    pf->wgt = 0x10000;
    pf->relax = -1;
    a = push(pf, 10);                               /* node 0, the popped one */
    b = push(pf, 300);
    c = push(pf, 400);
    pf->lazy = 1;
    memset(&L, 0, sizeof L);
    L.x = 5;
    L.z = 5;
    L.straight = 100;
    pf->grid[cidx(pf, 5, 5)].b1 = 6;
    pf->grid[cidx(pf, 5, 5)].b0 = 2;
    /* (6,5) ahead: g 16, h 18*5 - 4 = 86, f 102: overwrites node 0 at heap[0], stays the root (102 < 300) */
    expand_in(pf, &w.W, &w.goal[0], &L, cidx(pf, 5, 5), 0);
    CHECK(pf->lazy == 0 && pf->count == 3 && pf->used == 3 && pf->heap[0].n == a && pf->pool[a].f == 102 &&
          pf->pool[a].x == 6 && pf->grid[cidx(pf, 6, 5)].node == a, "lazy: overwrite in place");
    /* the same with an f above both children, 50 and 60: it sinks to the left, the lower */
    pf->lazy = 1;
    pf->grid[cidx(pf, 5, 5)].b1 = 2;                 /* reached heading west: ahead is (4,5), h 18*7-4 = 122 */
    set_f(pf, b, 50);
    set_f(pf, c, 60);
    expand_in(pf, &w.W, &w.goal[0], &L, cidx(pf, 5, 5), 0);
    CHECK(pf->heap[0].n == b && pf->heap[1].n == a && pf->pool[a].f == 16 + 122 && pf->pool[a].link == 1,
          "lazy: overwrite sinks left: %u %u", pf->heap[0].n, pf->heap[1].n);
    /* a relax under the lazy top: heap [b 50, a, c 60], lazy on b, a the open node of (4,5) with g 1000 and f 1100.
       From L (5,5) reached heading west, k 0: g 16 < 1000, so f moves by -984 to 116, still above b's 50: a sifts up
       nowhere and b stays the lazy top */
    pf->lazy = 1;
    pf->pool[a].g = 1000;
    set_f(pf, a, 1100);
    sift_down(pf, 1);
    L.x = 5;
    L.z = 5;
    L.g = 0;
    expand_in(pf, &w.W, &w.goal[0], &L, cidx(pf, 5, 5), 0);
    CHECK(pf->lazy == 1 && pf->heap[0].n == b && pf->pool[a].g == 16 && pf->pool[a].f == 116, "lazy: relax under");
    /* a relax over it: g 1000 and f 1014 again relaxed by -984, to 30 below b's 50: a rises to the root, b is pushed
       down off it, so b is freed, the lazy state ends and the heap's last fills b's place */
    pf->pool[a].g = 1000;
    set_f(pf, a, 1014);
    expand_in(pf, &w.W, &w.goal[0], &L, cidx(pf, 5, 5), 0);
    CHECK(pf->lazy == 0 && pf->count == 2 && pf->freehead == (int32_t)b && pf->heap[0].n == a && pf->pool[a].f == 30,
          "lazy: the displaced top is freed: count %d freehead %d top %u", pf->count, pf->freehead, pf->heap[0].n);
    /* the next node taken is the freed top (LIFO) */
    CHECK(node_take(pf) == (int32_t)b && pf->freehead == -1, "lazy: the free list is LIFO");
    world_free(&w);
}

/* ---- the goals and set path ------------------------------------------------------------------------------------ */

static void test_goals(void)
{
    TS_GOAL g;
    int16_t x, z;
    int64_t i;
    int n;
    /* the point: footprint 2 x 3 centred at (100.5, 200) px: gx = (100.5 - 16 + 8) * 2^16 >> 20 = 5, gz = (200 - 24 +
       8) / 16 = 11; r 40: rc2 = 2^2 */
    ts_goal_point(&g, (int32_t)(100.5 * 65536), 200 << 16, 40, 2, 3);
    CHECK(g.gx == 5 && g.gz == 11 && g.rc2 == 4, "point: (%d,%d) rc2 %d", g.gx, g.gz, g.rc2);
    CHECK(ts_goal_in(&g, 7, 11) && !ts_goal_in(&g, 7, 12) && ts_goal_in(&g, 4, 12), "point: in");
    CHECK(ts_goal_h(&g, 9, 12) == 18 * 4 + 7 - 40 && ts_goal_h(&g, 6, 11) == 0, "point: h %d", ts_goal_h(&g, 9, 12));
    n = 0;
    for (i = 0; ts_goal_cell(&g, i, &x, &z); i++) n++;
    CHECK(n == 1 && ts_goal_cell(&g, 0, &x, &z) && x == 5 && z == 11, "point: one cell");
    /* r = -17 truncates to -1: rc2 = 1 */
    ts_goal_point(&g, 0, 0, -17, 1, 1);
    CHECK(g.rc2 == 1, "point: rc2 of -17 is %d", g.rc2);
    /* the annulus: rout 100 (6), rin 40 (2): cell (gx, gz + 140 / 32 = 4) */
    ts_goal_annulus(&g, 80 << 16, 80 << 16, 100, 40, 1, 1);
    CHECK(g.gx == 5 && g.gz == 5 && g.rout2 == 36 && g.rin2 == 4, "annulus: fields");
    CHECK(ts_goal_cell(&g, 0, &x, &z) && x == 5 && z == 9 && !ts_goal_cell(&g, 1, &x, &z), "annulus: cell (%d,%d)", x,
          z);
    CHECK(!ts_goal_in(&g, 5, 6) && ts_goal_in(&g, 5, 7) && ts_goal_in(&g, 11, 5) && !ts_goal_in(&g, 12, 5),
          "annulus: in");
    /* h at (5,5): 0, below rin 40: 40; at (15,5): 180 - 100 = 80; at (7,5): 36, below 40: 4 */
    CHECK(ts_goal_h(&g, 5, 5) == 40 && ts_goal_h(&g, 15, 5) == 80 && ts_goal_h(&g, 7, 5) == 4 &&
          ts_goal_h(&g, 8, 5) == 0, "annulus: h");
    /* the ring around cell (10,10) size (2,3), mover footprint (1,2): x 9..12, z 8..13 */
    ts_goal_ring(&g, 10, 10, 2, 3, 1, 2);
    CHECK(g.x0 == 9 && g.x1 == 12 && g.z0 == 8 && g.z1 == 13, "ring: fields");
    n = 0;
    for (i = 0; ts_goal_cell(&g, i, &x, &z); i++) {
        CHECK(ts_goal_in(&g, x, z), "ring: cell %d (%d,%d) on the border", n, x, z);
        n++;
    }
    CHECK(n == 2 * 4 + 2 * 4, "ring: %d cells", n);
    CHECK(ts_goal_cell(&g, 0, &x, &z) && x == 9 && z == 8 && ts_goal_cell(&g, 1, &x, &z) && x == 9 && z == 13 &&
          ts_goal_cell(&g, 8, &x, &z) && x == 9 && z == 9 && ts_goal_cell(&g, 9, &x, &z) && x == 12 && z == 9,
          "ring: the order");
    CHECK(!ts_goal_in(&g, 10, 10) && !ts_goal_in(&g, 9, 7) && !ts_goal_in(&g, 13, 8) && ts_goal_in(&g, 12, 13),
          "ring: in");
    /* h: inside, the nearest side (10,10): min(1, 2, 2, 3) * 16; outside on x only 16 * 3; outside both
       (5,4): a 4, b 4: 16*4 + 6*4 */
    CHECK(ts_goal_h(&g, 10, 10) == 16 && ts_goal_h(&g, 15, 10) == 48 && ts_goal_h(&g, 5, 4) == 88 &&
          ts_goal_h(&g, 10, 20) == 112 && ts_goal_h(&g, 6, 20) == 16 * 7 + 6 * 3, "ring: h");
}

static void test_set_path(void)
{
    static const char* const rows[12] = {
        "S...........", "............", "............", "............", "............", "............",
        "............", "............", "............", "............", "............", "...........G" };
    static const int16_t pts[25][2] = { { 0 } };
    WORLD w;
    int32_t gx, gz;
    hand(&w, rows, 0, &gx, &gz);
    w.slot[1].path.count = 7;
    w.slot[1].path.pflags = 3;
    ts_pf_set_path(&w.pf, &w.W, 1, NULL, 0);
    CHECK(w.slot[1].path.count == 7 && w.slot[1].path.pflags == 8 && w.goal[0].flags == TS_GOALF_NOPATH,
          "set path 0: the count kept, the goal flagged");
    w.goal[0].flags = 0;
    w.cu[1].cx = (int16_t)gx;
    w.cu[1].cz = (int16_t)gz;
    ts_pf_set_path(&w.pf, &w.W, 1, NULL, 0);
    CHECK(w.goal[0].flags == 0, "set path 0: a reached goal is not flagged");
    w.slot[1].path.pflags = 2;
    ts_pf_set_path(&w.pf, &w.W, 1, pts, 25);
    CHECK(w.slot[1].path.count == 20 && w.slot[1].path.pflags == 9, "set path 25: 20 kept");
    ts_pf_set_path(&w.pf, &w.W, 0, pts, 2);
    ts_pf_set_path(&w.pf, &w.W, 2, pts, 2);
    world_free(&w);
}

/* ---- the bug walk ---------------------------------------------------------------------------------------------- */

/* the walk alone, from the hand map's start to its goal, at weight 1.5: the search start's preamble, then 0x40E160 */
static int32_t walk(WORLD* w)
{
    TS_PF* pf = &w->pf;
    int16_t x, z;
    pf->searching = pf->cls_obj = 1;
    pf->cls = 0;
    pf->goal = 0;
    pf->wgt = 0x18000;
    pf->start_x = w->cu[1].cx;
    pf->start_z = w->cu[1].cz;
    reset_grid(pf);
    ts_goal_cell(&w->goal[0], 0, &x, &z);
    pf->grid[cidx(pf, x, z)].b0 = 4;
    touch(pf, cidx(pf, x, z));
    pf->near_x = x;
    pf->near_z = z;
    pf->cost = 0;
    return bug_walk(pf, &w->W, &w->goal[0]);
}

static int walked(const WORLD* w, int32_t x, int32_t z) { return (w->pf.grid[cidx(&w->pf, x, z)].b0 & 8) != 0; }

static void walk_print(const WORLD* w)
{
    int32_t x, z;
    for (z = 0; z < 12; z++) {
        printf("  ");
        for (x = 0; x < 12; x++)
            putchar(ts_class_code(&w->cm, 0, x, z) == 0 ? '#' : walked(w, x, z) ? '0' + w->pf.grid[cidx(&w->pf, x, z)].b1
                                                                                  : '.');
        printf("\n");
    }
}

/* The U: the start inside a U open to the west, the goal east of its closed side. Straight east from (5,5) stops at
   the wall (7,5); the heads follow the inside of the arms -- A north (it tries the turn toward da - 2 first), B south
   -- out round the arms' ends and back east along their outsides, and one of them reaches the m-line, z = 5 east of
   the start, at (8,5), the first cell past the wall; from there straight east (9,5), then the goal (10,5): 0. */
static void test_walk_u(void)
{
    static const char* const rows[12] = {
        "............", "............", "............", "...#####....", "...#...#....",
        "...#.S.#..G.", "...#...#....", "...#...#....", "...#####....", "............",
        "............", "............" };
    WORLD w;
    int32_t gx, gz, r;
    hand(&w, rows, 0xC000, &gx, &gz);
    /* open the U's west side */
    code_put(&w, 0, 3, 4, 3);
    code_put(&w, 0, 3, 5, 3);
    code_put(&w, 0, 3, 6, 3);
    code_put(&w, 0, 3, 7, 3);
    r = walk(&w);
    CHECK(r == 0, "walk U: %d, want 0", r);
    CHECK(walked(&w, 6, 5) && w.pf.grid[cidx(&w.pf, 6, 5)].b1 == 6, "walk U: the straight run to the wall");
    CHECK(walked(&w, 8, 5) && walked(&w, 9, 5) && w.pf.grid[cidx(&w.pf, 9, 5)].b1 == 6 &&
          w.pf.grid[cidx(&w.pf, 10, 5)].b0 == (4 | 8) && w.pf.grid[cidx(&w.pf, 10, 5)].b1 == 6,
          "walk U: the m-line at (8,5), straight to the goal");
    CHECK(walked(&w, 6, 4) && walked(&w, 6, 6) && walked(&w, 2, 3) && walked(&w, 2, 8), "walk U: both heads out");
    if (fails) walk_print(&w);
    world_free(&w);
}

/* The L: a wall x = 6 from z = 2 down to its foot z = 7, which runs east. The start (1,2), the goal (9,5). Straight,
   x first: (2,2) .. (5,2), five costs with the blocked (6,2). The follow from P (5,2), d 6, both heads at 0: A tries
   6 (6,2) blocked, then 7, (6,1): it steps; B tries 2 ((5,2) - D[2] = (6,2)) and 1 ((6,3)) blocked, then 0: (5,3).
   Second pair (cost 7): A from 7 tries 5 first, (7,2): on the m-line's x leg (ax 2, az 0), so P = (7,2). Straight:
   (8,2), (9,2) east, then gx = 0 and gz = 3: (9,3), (9,4) south, (9,5) the goal, five costs: 12, answer 0. */
static void test_walk_l(void)
{
    static const char* const rows[12] = {
        "............", "............", ".S....#.....", "......#.....", "......#.....",
        "......#..G..", "......#.....", "......######", "............", "............",
        "............", "............" };
    static const int8_t trail[8][3] = { { 5, 2, 6 }, { 6, 1, 7 }, { 5, 3, 0 }, { 7, 2, 5 }, { 8, 2, 6 }, { 9, 2, 6 },
                                        { 9, 3, 4 }, { 9, 4, 4 } };
    WORLD w;
    int32_t gx, gz, r, i, n = 0, x, z;
    hand(&w, rows, 0xC000, &gx, &gz);
    r = walk(&w);
    CHECK(r == 0 && w.pf.cost == 12, "walk L: %d cost %d, want 0 cost 12", r, w.pf.cost);
    for (i = 0; i < 8; i++)
        CHECK(walked(&w, trail[i][0], trail[i][1]) && w.pf.grid[cidx(&w.pf, trail[i][0], trail[i][1])].b1 == trail[i][2],
              "walk L: (%d,%d) b1 %d", trail[i][0], trail[i][1], trail[i][2]);
    CHECK(w.pf.grid[cidx(&w.pf, 9, 5)].b0 == (4 | 8) && w.pf.grid[cidx(&w.pf, 9, 5)].b1 == 4, "walk L: on the goal");
    for (z = 0; z < 12; z++)
        for (x = 0; x < 12; x++) n += walked(&w, x, z);
    CHECK(n == 4 + 8, "walk L: %d cells walked, want 12", n);
    if (fails) walk_print(&w);
    world_free(&w);
}

/* The box: the goal walled in. The heads circle the box's outside and meet; nothing nearer than its outside: the
   least weighed heuristic is at the middle of a side, a = 3, b = 0: 54 - 4 = 50, * 1.5 = 75. The search then runs
   the A* with that relaxation, so every cell whose weighed heuristic is at most 75 is a goal cell: the middles of
   the four sides. Straight east, the first popped is (5,5): the path is the start and (5,5), g 48, and the walk's
   flag 0x200 alone. A start walled in alone gets no nearer than itself, fails before the A* and flags no-path. */
static void test_walk_box(void)
{
    static const char* const rows[12] = {
        "............", "............", "............", "......#####.", "......#...#.",
        "..S...#.G.#.", "......#...#.", "......#####.", "............", "............",
        "............", "............" };
    static const char* const shut[12] = {
        "............", "............", "............", "............", ".###........",
        ".#S#........", ".###........", "............", "............", "............",
        "...........G", "............" };
    static const int32_t want[2][2] = { { 2, 5 }, { 5, 5 } };
    WORLD w;
    int32_t gx, gz, r;
    hand(&w, rows, 0xC000, &gx, &gz);
    r = walk(&w);
    CHECK(r == 75, "walk box: %d, want 75", r);
    CHECK(walked(&w, 5, 5) && walked(&w, 8, 2) && walked(&w, 8, 8) && walked(&w, 11, 5), "walk box: round the box");
    CHECK(!walked(&w, 8, 5) && !walked(&w, 4, 2), "walk box: neither inside nor away from the wall");
    if (fails) walk_print(&w);
    world_free(&w);
    hand(&w, rows, 0xC000, &gx, &gz);
    ts_pf_tick(&w.pf, &w.W);
    CHECK(w.goal[0].flags == TS_GOALF_WALKOUT && path_is(&w, want, 2) && found_g(&w) == 48,
          "box search: flags %#x g %d", w.goal[0].flags, found_g(&w));
    world_free(&w);
    hand(&w, shut, 0xC000, &gx, &gz);
    ts_pf_tick(&w.pf, &w.W);
    CHECK(w.goal[0].flags == (TS_GOALF_WALKOUT | TS_GOALF_NOPATH) && w.pf.used == 0 && w.pf.count == 0 &&
          w.slot[1].path.pflags == 8, "shut search: flags %#x, no A*", w.goal[0].flags);
    world_free(&w);
}

/* ---- the literal loop against the skip-ahead ------------------------------------------------------------------- */

static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    while (n--) h = (h ^ *b++) * 0x100000001B3ull;
    return h;
}

/* one random world from `seed`: a map of random heights and up to three classes, up to ten seats of up to 24 slots,
   movers with random cells, classes, bits, stamps and goals, random credits, cursors, counts and budget */
static void random_world(WORLD* w, uint64_t seed)
{
    int32_t W, H, i, seats, S, nslot, c, p;
    uint8_t* ht;
    rng = seed;
    memset(w, 0, sizeof *w);
    W = 12 + pick(29);
    H = 12 + pick(29);
    for (c = 0; c < 3; c++)
        if (c == 0 || pick(2)) class_one(&w->cls[c * 7], (int16_t)(1 + pick(3)), (int16_t)(1 + pick(3)),
                                         (uint8_t)(10 + pick(40)), (uint8_t)(5 + pick(10)));
    ht = malloc((size_t)W * H);
    for (i = 0; i < W * H; i++) ht[i] = (uint8_t)(pick(4) ? 20 + pick(8) : pick(200));
    seats = 1 + pick(10);
    S = 1 + pick(24);
    nslot = 1 + seats * S + pick(3);
    world_make(w, W, H, ht, nslot);
    free(ht);
    if (pick(3) == 0) {
        w->mapped = malloc((size_t)(W >> 1) * (H >> 1) * sizeof *w->mapped);
        for (i = 0; i < (W >> 1) * (H >> 1); i++) w->mapped[i] = (uint16_t)(pick(5) ? 0xFFFF : next());
        w->W.mapped = w->mapped;
    }
    for (p = 0; p < TS_PF_SEATS; p++) {
        TS_PFSEAT* s = &w->W.seat[p];
        if (p < seats) {
            s->credited = (uint8_t)(pick(6) != 0);
            s->first = 1 + p * S;
            s->last = s->first + S - 1;
        } else {
            s->credited = 0;
            s->first = s->last = pick(2) ? 0 : pick(nslot + 2);
        }
    }
    w->W.players = (uint16_t)(1 + pick(10));
    w->W.per_seat = (uint16_t)pick(4) ? (uint16_t)S : 0;
    w->W.now = (uint32_t)(30 + pick(400));
    if (pick(50) == 0) w->W.now = 0xFFFFFFFFu - (uint32_t)pick(80);
    for (i = 0; i < NGOAL; i++) {
        const int k = pick(4);
        if (k == 0) ts_goal_point(&w->goal[i], (pick(W) * 16) << 16, (pick(H) * 16) << 16, pick(3) ? 4 : pick(200),
                                  (int16_t)(1 + pick(3)), (int16_t)(1 + pick(3)));
        else if (k == 1) ts_goal_annulus(&w->goal[i], (pick(W) * 16) << 16, (pick(H) * 16) << 16, 20 + pick(200),
                                         pick(60), 1, 1);
        else if (k == 2) ts_goal_ring(&w->goal[i], (int16_t)(pick(W + 4) - 2), (int16_t)(pick(H + 4) - 2),
                                      (int16_t)(1 + pick(4)), (int16_t)(1 + pick(4)), (int16_t)(1 + pick(3)),
                                      (int16_t)(1 + pick(3)));
    }
    for (i = 1; i < nslot; i++) {
        TS_CUNIT* u = &w->cu[i];
        TS_PFSLOT* s = &w->slot[i];
        s->type = (uint16_t)(pick(4) ? 1 + pick(5) : 0);
        u->mover = (uint8_t)(pick(8) != 0);
        u->alive = (uint8_t)(s->type != 0);
        c = pick(4);
        s->cls = (int8_t)(c == 3 ? TS_PF_NOCLASS : c * 7);
        if (pick(20) == 0) s->cls = (int8_t)pick(40);
        u->cx = (int16_t)(pick(W + 2) - 1);
        u->cz = (int16_t)(pick(H + 2) - 1);
        u->sx = (int16_t)(1 + pick(3));
        u->sz = (int16_t)(1 + pick(3));
        u->claim = (uint32_t)pick(500);
        s->heading = (uint16_t)next();
        s->path.pflags = (uint8_t)(pick(3) ? 2 | pick(2) : pick(16));
        s->path.last_search = pick(3) ? 0 : w->W.now - (uint32_t)pick(120);
        s->path.goal = pick(12) ? pick(NGOAL) : pick(NGOAL + 4) - 2;
        s->path.count = pick(5);
    }
    world_pf(w, 1 + pick(pick(4) ? 2000 : 70000));
    for (p = 0; p < TS_PF_SEATS; p++) {
        w->pf.credit[p] = pick(400) - 200;
        if (w->W.seat[p].last >= w->W.seat[p].first && pick(4))
            w->pf.cursor[p] = w->W.seat[p].first + pick(w->W.seat[p].last - w->W.seat[p].first + 1);
        w->pf.exams[p] = pick(3 * S);
        w->pf.weight[p] = 0x18000 * (1 + pick(6));
    }
    w->pf.calls = pick(150);
    w->pf.rr = (uint8_t)pick(10);
    w->pf.cls_obj = pick(nslot);
    ts_pf_mark_all(&w->pf, &w->W);
}

/* between ticks: GameTime moves on, and a few slots ask for a search as the class step would, marked as its caller
   must */
static void random_between(WORLD* w, uint64_t seed)
{
    int32_t i, n;
    rng = seed;
    w->W.now += (uint32_t)(1 + pick(40));
    n = pick(4);
    for (i = 0; i < n; i++) {
        const int32_t s = 1 + pick(w->units.n - 1);
        w->slot[s].path.pflags |= 2;
        if (w->slot[s].path.goal == TS_PF_NOGOAL) w->slot[s].path.goal = pick(NGOAL);
        ts_pf_mark(&w->pf, &w->W, s);
    }
}

/* every byte the two worlds' pathfinding state holds that the engine's would: the object without its pointers and
   the core's bitsets, its globals, the grid, the touched blocks, the pool, the live heap, the slots, the goals, the
   class maps and their watermarks, the units' claim stamps */
static uint64_t state_hash(const WORLD* w)
{
    const TS_PF* pf = &w->pf;
    uint64_t h = 0xCBF29CE484222325ull;
    int32_t i;
    h = fnv(h, &pf->freehead, sizeof pf->freehead);
    h = fnv(h, &pf->used, sizeof pf->used);
    h = fnv(h, &pf->count, sizeof pf->count);
    h = fnv(h, &pf->lazy, sizeof pf->lazy);
    h = fnv(h, &pf->start_x, offsetof(TS_PF, nslot) - offsetof(TS_PF, start_x));
    h = fnv(h, pf->grid, (size_t)pf->cells * sizeof *pf->grid);
    h = fnv(h, pf->touch, 4 * (size_t)touch_words(pf->cells));
    h = fnv(h, pf->pool, (size_t)pf->nodes * sizeof *pf->pool);
    for (i = 0; i < pf->count; i++) h = fnv(h, &pf->heap[i].n, sizeof pf->heap[i].n);
    for (i = 0; i < w->units.n; i++) {
        h = fnv(h, &w->slot[i].path, sizeof w->slot[i].path);
        h = fnv(h, &w->cu[i].claim, sizeof w->cu[i].claim);
    }
    for (i = 0; i < NGOAL; i++) h = fnv(h, &w->goal[i].flags, sizeof w->goal[i].flags);
    for (i = 0; i < TS_CLASSES; i++) {
        h = fnv(h, &w->cm.c[i].wm, sizeof w->cm.c[i].wm);
        if (w->cm.c[i].map) h = fnv(h, w->cm.c[i].map, 4 * (size_t)w->cm.rows16 * w->cm.w);
    }
    return h;
}

/* the first field in which the two differ, for the report */
static const char* first_diff(const WORLD* a, const WORLD* b)
{
    int p;
    for (p = 0; p < TS_PF_SEATS; p++) {
        if (a->pf.exams[p] != b->pf.exams[p]) return "exams";
        if (a->pf.cursor[p] != b->pf.cursor[p]) return "cursor";
        if (a->pf.credit[p] != b->pf.credit[p]) return "credit";
    }
    if (a->pf.rr != b->pf.rr) return "rr";
    if (a->pf.cls_obj != b->pf.cls_obj) return "cls_obj";
    if (a->pf.searching != b->pf.searching) return "searching";
    if (a->pf.cost != b->pf.cost) return "cost";
    return "elsewhere";
}

static uint64_t test_skip(int worlds, int report)
{
    uint64_t digest = 0xCBF29CE484222325ull;
    int n, t, searches = 0, skipped = 0;
    for (n = 0; n < worlds; n++) {
        const uint64_t seed = 0x5DEECE66Dull * (uint64_t)(n + 1) ^ 0xA5A5A5A5ull;
        WORLD a, b;
        const int ticks = 1 + (int)(seed % 4);
        random_world(&a, seed);
        random_world(&b, seed);
        for (t = 0; t < ticks; t++) {
            int32_t before = 0, p;
            for (p = 0; p < TS_PF_SEATS; p++) before += a.pf.exams[p];
            ts_pf_tick_literal(&a.pf, &a.W);
            ts_pf_tick(&b.pf, &b.W);
            if (a.pf.searching || a.pf.used) searches++;
            for (p = 0; p < TS_PF_SEATS; p++) skipped += a.pf.exams[p] - before > 100;
            if (state_hash(&a) != state_hash(&b)) {
                CHECK(0, "skip-ahead: world %d tick %d differs from the literal loop (%s)", n, t, first_diff(&a, &b));
                break;
            }
            random_between(&a, seed + (uint64_t)t);
            random_between(&b, seed + (uint64_t)t);
        }
        digest = fnv(digest, &(uint64_t){ state_hash(&a) }, 8);
        world_free(&a);
        world_free(&b);
    }
    if (report) printf("skip-ahead: %d worlds, %d ticks with a search, %d with long runs skipped\n", worlds, searches,
                       skipped);
    return digest;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "digest")) {
        printf("%016llx\n", (unsigned long long)test_skip(10000, 0));
        return fails != 0;
    }
    test_goals();
    test_set_path();
    test_expand();
    test_ties();
    test_lazy();
    test_turn();
    test_bad();
    test_unexplored();
    test_fan();
    test_walk_u();
    test_walk_l();
    test_walk_box();
    test_skip(10000, 1);
    printf(fails ? "tasim_pf_test: %d FAILED\n" : "tasim_pf_test: ok\n", fails);
    return fails != 0;
}
