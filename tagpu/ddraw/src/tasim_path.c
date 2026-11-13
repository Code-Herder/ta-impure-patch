/* tasim_path.c -- the engine's pathfinder: the scheduler with the core's skip-ahead, the search start, the bug walk, the
   A* steps with the expansion and the lazy-top heap, the extraction, set path, and the ground goals' search methods
   (tasim_path.h). Every function is a port of the engine function it names, in its order of operations, DISASSEMBLED
   from pristine/TotalA.exe.pristine (objdump -d -M intel). Arithmetic the engine does in 32 bits wraps here too:
   it goes through uint32_t, so a wrap is the engine's and never undefined. */
#include <string.h>
#include "tasim_fixed.h"
#include "tasim_path.h"

#define AL4(n) (((n) + 3u) & ~(size_t)3u)
#define ADD(a, b) ((int32_t)((uint32_t)(a) + (uint32_t)(b)))
#define SUB(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)))
#define MUL(a, b) ((int32_t)((uint32_t)(a) * (uint32_t)(b)))

/* the directions, the bytes of 0x4FD670 and 0x4FD678: 0 is -z (heading 0), d is heading d * 0x2000 */
static const int8_t DX[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };
static const int8_t DZ[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
/* the turn penalty by the turn k (the bytes of 0x4FCA10) and the step by the direction (0x40E66B..0x40E688), both
   written into the object at every search start */
static const uint8_t TURNPEN[8] = { 0, 40, 60, 80, 100, 80, 60, 40 };
static const uint8_t STEP[8] = { 16, 22, 16, 22, 16, 22, 16, 22 };

/* ---- the goals --------------------------------------------------------------------------------------------------- */

/* the footprint rule of the point and the annulus (0x44CF7B..0x44CFA7, 0x44D3D3..0x44D3FF): the top-left cell of a unit
   of footprint f centred on the 16.16 coordinate v, (v - (f << 19) + 0x80000) >> 20, stored as a word */
static int16_t goal_cell_of(int32_t v, int16_t f)
{
    return (int16_t)(ADD(SUB(v, (uint32_t)(int32_t)f << 19), 0x80000) >> 20);
}

/* (r / 16)^2, the division truncating toward zero (cdq; and edx, 15; add; sar 4) and the square an imul */
static int32_t sq16(int32_t r)
{
    const int32_t q = r / 16;
    return MUL(q, q);
}

/* 0x44CF60(node, x, z, r), thiscall ret 0x10, DISASSEMBLED 0x44CF60..0x44CFCE */
void ts_goal_point(TS_GOAL* g, int32_t x16, int32_t z16, int32_t r, int16_t fw, int16_t fh)
{
    memset(g, 0, sizeof *g);
    g->kind = TS_GOAL_POINT;
    g->gx = goal_cell_of(x16, fw);
    g->gz = goal_cell_of(z16, fh);
    g->r = r;
    g->rc2 = sq16(r);
}

/* 0x44D3B0(node, x, z, rout, rin), thiscall ret 0x14, DISASSEMBLED 0x44D3B0..0x44D43A: the fourth argument goes to
   +0x10 and the fifth to +0x0C, and in_goal holds d^2 to [+0x14, +0x18], so +0x10 is the outer radius */
void ts_goal_annulus(TS_GOAL* g, int32_t x16, int32_t z16, int32_t rout, int32_t rin, int16_t fw, int16_t fh)
{
    memset(g, 0, sizeof *g);
    g->kind = TS_GOAL_ANNULUS;
    g->gx = goal_cell_of(x16, fw);
    g->gz = goal_cell_of(z16, fh);
    g->rin = rin;
    g->rout = rout;
    g->rout2 = sq16(rout);
    g->rin2 = sq16(rin);
}

/* 0x44D8A0(node, cell, size), thiscall ret 0xC, DISASSEMBLED 0x44D8A0..0x44D8F0: each word sign-extended, x0 = cx - fw,
   x1 = cx + w, z0 = cz - fh, z1 = cz + h, with (fw, fh) the order's unit's footprint */
void ts_goal_ring(TS_GOAL* g, int16_t cx, int16_t cz, int16_t w, int16_t h, int16_t fw, int16_t fh)
{
    memset(g, 0, sizeof *g);
    g->kind = TS_GOAL_RING;
    g->x0 = (int32_t)cx - fw;
    g->x1 = (int32_t)cx + w;
    g->z0 = (int32_t)cz - fh;
    g->z1 = (int32_t)cz + h;
}

/* in_goal (vtable +0x14): the point 0x44D290 (DISASSEMBLED 0x44D290..0x44D2BC), dx^2 + dz^2 <= rc2 from the goal cell,
   imul and add wrapping, the compare signed; the annulus 0x44D7C0 (0x44D7C0..0x44D7F7), rin2 <= d^2 <= rout2; the ring
   0x44DCB0 (0x44DCB0..0x44DCF7), only the rectangle's border: x on x0 or x1 with z0 <= z <= z1, or z on z0 or z1 with
   x0 <= x <= x1, in the branch order of the engine's code. */
int ts_goal_in(const TS_GOAL* g, int32_t x, int32_t z)
{
    int32_t dx, dz, d2;
    switch (g->kind) {
    case TS_GOAL_POINT:
    case TS_GOAL_ANNULUS:
        dx = SUB(x, g->gx);
        dz = SUB(z, g->gz);
        d2 = ADD(MUL(dx, dx), MUL(dz, dz));
        if (g->kind == TS_GOAL_POINT) return d2 <= g->rc2;
        return d2 <= g->rout2 && d2 >= g->rin2;
    case TS_GOAL_RING:
        if (x == g->x0 || x == g->x1) {
            if (z < g->z0) goto bottom;
            if (z <= g->z1) return 1;
        }
        if (z == g->z0) goto across;
    bottom:
        if (z != g->z1) return 0;
    across:
        return x >= g->x0 && x <= g->x1;
    default:
        return 0;
    }
}

/* |v| as cdq; xor; sub computes it: INT_MIN stays INT_MIN */
static int32_t iabs(int32_t v) { return v < 0 ? SUB(0, v) : v; }

/* the heuristic (vtable +0x1C), in mixed units the engine keeps: 18 a straight cell and 7 more a diagonal one against
   the steps' 16 and 22, less a radius in px. The point 0x44D350 (DISASSEMBLED 0x44D350..0x44D3A1): 18 max + 7 min of
   |x - gx| and |z - gz|, less r, 0 below it. The annulus 0x44D840 (0x44D840..0x44D89D): the same sum h, then h - rout
   above rout, rin - h below rin, else 0. The ring 0x44DD00 (0x44DD00..0x44DDB3): from the rectangle [x0, x1] x
   [z0, z1], outside on both axes 16 max + 6 min of the axis distances, outside on one 16 times its distance, inside
   16 times the distance to the nearest side. */
int32_t ts_goal_h(const TS_GOAL* g, int32_t x, int32_t z)
{
    int32_t a, b, h;
    switch (g->kind) {
    case TS_GOAL_POINT:
    case TS_GOAL_ANNULUS:
        a = iabs(SUB(x, g->gx));
        b = iabs(SUB(z, g->gz));
        h = a > b ? ADD(MUL(a, 18), MUL(b, 7)) : ADD(MUL(a, 7), MUL(b, 18));
        if (g->kind == TS_GOAL_POINT) return h < g->r ? 0 : SUB(h, g->r);
        if (h > g->rout) return SUB(h, g->rout);
        return h < g->rin ? SUB(g->rin, h) : 0;
    case TS_GOAL_RING:
        if (x < g->x0 || x > g->x1) {
            a = x < g->x0 ? SUB(g->x0, x) : SUB(x, g->x1);
            if (z >= g->z0 && z <= g->z1) return MUL(a, 16);
            b = z < g->z0 ? SUB(g->z0, z) : SUB(z, g->z1);
            return a > b ? ADD(MUL(a, 16), MUL(b, 6)) : ADD(MUL(b, 16), MUL(a, 6));
        }
        if (z < g->z0) return MUL(SUB(g->z0, z), 16);
        if (z > g->z1) return MUL(SUB(z, g->z1), 16);
        a = SUB(x, g->x0);
        if (SUB(g->x1, x) <= a) a = SUB(g->x1, x);
        b = SUB(g->z1, z);
        if (SUB(z, g->z0) < b) b = SUB(z, g->z0);
        if (a < b) b = a;
        return MUL(b, 16);
    default:
        return 0;
    }
}

/* the goal cells (vtable +0x18) as the vector each one fills, word pairs: the point's 0x44D0E0 (DISASSEMBLED
   0x44D0E0..0x44D27E) one, (gx, gz); the annulus's 0x44D560 (0x44D586..0x44D5A6) one, (gx, gz + (rin + rout) / 32),
   the division truncating and the add a word's; the ring's 0x44DA00 (0x44DA30..0x44DC54), for x from x0 to x1 (x, z0)
   then (x, z1), then for z from z0 + 1 to z1 - 1 (x0, z) then (x1, z). */
int ts_goal_cell(const TS_GOAL* g, int64_t i, int16_t* x, int16_t* z)
{
    int64_t nx, nz;
    if (i < 0) return 0;
    switch (g->kind) {
    case TS_GOAL_POINT:
        if (i) return 0;
        *x = g->gx;
        *z = g->gz;
        return 1;
    case TS_GOAL_ANNULUS:
        if (i) return 0;
        *x = g->gx;
        *z = (int16_t)(uint16_t)((uint32_t)(uint16_t)g->gz + (uint32_t)(ADD(g->rin, g->rout) / 32));
        return 1;
    case TS_GOAL_RING:
        nx = g->x1 >= g->x0 ? (int64_t)g->x1 - g->x0 + 1 : 0;
        nz = (int64_t)g->z1 - g->z0 - 1 > 0 ? (int64_t)g->z1 - g->z0 - 1 : 0;
        if (i < 2 * nx) {
            *x = (int16_t)(g->x0 + (int32_t)(i >> 1));
            *z = (int16_t)(i & 1 ? g->z1 : g->z0);
            return 1;
        }
        i -= 2 * nx;
        if (i >= 2 * nz) return 0;
        *x = (int16_t)(i & 1 ? g->x1 : g->x0);
        *z = (int16_t)(g->z0 + 1 + (int32_t)(i >> 1));
        return 1;
    default:
        return 0;
    }
}

/* ---- the layout -------------------------------------------------------------------------------------------------- */

static int32_t grid_cells(int32_t w, int32_t h) { return (int32_t)(((int64_t)w * h + 7) & ~(int64_t)7); }
static int32_t touch_words(int32_t cells) { return (cells + 0xFF) >> 8; }
static int32_t slot_words(int32_t n) { return (n + 31) >> 5; }

/* THE POOL'S BOUND: one node per cell. The expansion 0x40DA70 takes a new node only for a cell in state 0, which it
   leaves open or dead (0x40DCBC, 0x40DCC3, 0x40DC7D); an open cell is relaxed in place (0x40DB2D..0x40DB4B) and a
   closed or dead one returns (0x40DACE..0x40DADB), and only the next search's reset makes a cell new again. The start
   node is the one other, on a cell the expansion never opens (it is open from the start). So a search allocates at
   most W * H nodes, the heap holds at most as many, and the free list reuses popped ones. The engine grows both with
   cap + cap / 2 + 16 (0x40DDB6..0x40DDCE, 0x40F110); the core is sized to the bound the engine cannot exceed. */
size_t ts_pf_bytes(int32_t w, int32_t h, int32_t nslot)
{
    int32_t cells;
    if (w <= 0 || h <= 0 || (int64_t)w * h > TS_MAP_CELLS_MAX || nslot < 1) return 0;
    cells = grid_cells(w, h);
    return AL4((size_t)cells * sizeof(TS_PFCELL)) + 4 * (size_t)touch_words(cells) +
           (size_t)w * h * sizeof(TS_PFNODE) + 4 * (size_t)w * h + 3 * 4 * (size_t)slot_words(nslot);
}

static void reset_grid(TS_PF* pf);

int ts_pf_init(TS_PF* pf, void* mem, size_t bytes, int32_t w, int32_t h, int32_t nslot, int32_t budget,
               const TS_PFSEAT seat[TS_PF_SEATS])
{
    const size_t need = ts_pf_bytes(w, h, nslot);
    unsigned char* p = (unsigned char*)mem;
    int32_t exams[TS_PF_SEATS], calls = pf->calls;
    int i;
    if (!need || !mem || bytes < need) return -1;
    memcpy(exams, pf->exams, sizeof exams);
    memset(pf, 0, sizeof *pf);
    memset(mem, 0, need);
    memcpy(pf->exams, exams, sizeof exams);
    pf->calls = calls;
    pf->w = w;
    pf->h = h;
    pf->cells = grid_cells(w, h);
    pf->nodes = w * h;
    pf->grid = (TS_PFCELL*)p;
    p += AL4((size_t)pf->cells * sizeof(TS_PFCELL));
    pf->touch = (uint32_t*)p;
    p += 4 * (size_t)touch_words(pf->cells);
    pf->pool = (TS_PFNODE*)p;
    p += (size_t)pf->nodes * sizeof(TS_PFNODE);
    pf->heap = (uint32_t*)p;
    p += 4 * (size_t)pf->nodes;
    pf->nslot = nslot;
    pf->mob = (uint32_t*)p;
    pf->cand = pf->mob + slot_words(nslot);
    pf->starter = pf->cand + slot_words(nslot);
    /* the engine marks every block touched and resets the grid, so every b0 is 0 (0x40EA66..0x40EACB); the memset is
       the same. THE ENGINE'S, which the core does not keep: under 256 cells its loop sets no bit (cells - 0x100 wraps,
       0x40EA9D) and b0 keeps the allocator's bytes; no map is that small (TS_MAP_W_MIN x TS_MAP_H_MIN is 18 cells,
       a 1 x 1 TA map 1024). */
    pf->freehead = -1;
    pf->budget = budget;
    pf->base_w = 0x18000;
    pf->goal = TS_PF_NOGOAL;
    pf->cls = TS_PF_NOCLASS;
    memcpy(pf->turnpen, TURNPEN, 8);
    memcpy(pf->step, STEP, 8);
    for (i = 0; i < TS_PF_SEATS; i++) {
        pf->weight[i] = pf->base_w;
        pf->cursor[i] = seat[i].first;
    }
    reset_grid(pf);
    return 0;
}

/* ---- the grid ---------------------------------------------------------------------------------------------------- */

static int inside(const TS_PF* pf, int32_t x, int32_t z)
{
    return (uint32_t)x < (uint32_t)pf->w && (uint32_t)z < (uint32_t)pf->h;
}

static int32_t cidx(const TS_PF* pf, int32_t x, int32_t z) { return z * pf->w + x; }

/* the touched-block bit of cell c, as 0x40D8B0 and every inline copy set it: dword c >> 8, bit (c >> 3) & 31 */
static void touch(TS_PF* pf, int32_t c) { pf->touch[(uint32_t)c >> 8] |= 1u << (((uint32_t)c >> 3) & 31); }

/* 0x40D900, DISASSEMBLED 0x40D900..0x40D9BE: b0 = 0 in all 8 cells of every touched block, and the bits cleared; b1
   and the node word keep the last search's values, dead while b0 is 0. The engine bounds the last dword's cells by
   (dword << 8) + k < cells, which always holds there; the core bounds every cell by cells. */
static void reset_grid(TS_PF* pf)
{
    const int32_t n = touch_words(pf->cells);
    int32_t i, j, k;
    for (i = 0; i < n; i++) {
        uint32_t bits = pf->touch[i];
        if (!bits) continue;
        pf->touch[i] = 0;
        for (j = 0; bits; j++, bits >>= 1)
            if (bits & 1)
                for (k = 0; k < 8; k++) {
                    const int32_t c = (i << 8) + (j << 3) + k;
                    if (c < pf->cells) pf->grid[c].b0 = 0;
                }
    }
}

/* THE BOUNDS on a move class taken from data: a class index outside the 32 records, or a class with no map, is no
   class. The engine's is a pointer the FBI loader took from 0x440420, which answers only a present class. */
static int class_ok(const TS_CLASSMAPS* cm, int32_t i)
{
    return i >= 0 && i < TS_CLASSES && cm->c[i].present && cm->c[i].map;
}

/* 0x40D7B0(x, z), thiscall on the pathfinder ret 8, DISASSEMBLED 0x40D7B0..0x40D870: 0 off the class's grid
   (unsigned, the class's +0x10/+0x14); 0 when the 32-px block at the footprint's centre, ((x >> 1) + (fx >> 2),
   (z >> 1) + (fz >> 2)), is outside (W >> 1, H >> 1) of the feature grid; 2 when that block's explored bit of the
   player being served, 1 << rr, is clear; else the class map's code, 0 blocked, 1 bad or 3 good. */
int ts_pf_reader(const TS_PF* pf, const TS_PFWORLD* W, int32_t x, int32_t z)
{
    const TS_CLASSMAPS* cm = W->cm;
    const TS_CLASSMAP* c;
    int32_t bx, bz, mw, mh;
    if (!class_ok(cm, pf->cls)) return 0;
    c = &cm->c[pf->cls];
    if ((uint32_t)x >= (uint32_t)cm->w || (uint32_t)z >= (uint32_t)cm->h) return 0;
    mw = cm->map->w >> 1;
    mh = cm->map->h >> 1;
    bx = (x >> 1) + (c->fx >> 2);
    bz = (z >> 1) + (c->fz >> 2);
    if (bx >= mw || bz >= mh) return 0;
    if (W->mapped && !(W->mapped[bz * mw + bx] & (1u << (pf->rr & 31)))) return 2;
    return ts_class_code(cm, pf->cls, x, z);
}

/* ---- the slots and the goals ------------------------------------------------------------------------------------- */

/* THE BOUND on a slot: 1 .. n - 1 of the units' view; slot 0 is never a unit (the players' blocks start at 1) */
static TS_PFSLOT* slot_at(const TS_PFWORLD* W, int32_t s)
{
    return W->cm->units && s > 0 && s < W->cm->units->n ? &W->slot[s] : NULL;
}

/* THE BOUND on a goal index against the caller's goals */
static TS_GOAL* goal_at(const TS_PFWORLD* W, int32_t i)
{
    return i >= 0 && i < W->ngoal && W->goal[i].kind != TS_GOAL_NONE ? &W->goal[i] : NULL;
}

/* the examination's test (0x40ED33..0x40ED4D): an occupied slot (+0xA6), with a mover ([unit]) whose class (+0x04)
   is set */
static int is_mob(const TS_PFWORLD* W, int32_t s)
{
    const TS_PFSLOT* u = slot_at(W, s);
    return u && u->type && W->cm->units->u[s].mover && class_ok(W->cm, u->cls);
}

/* the 60-tick gate of wants_search 0x44F260 (DISASSEMBLED 0x44F260..0x44F284): bit 1, and GameTime not below
   last_search + 60, the sum wrapping (unsigned `jb`) */
static int gate(const TS_PATH* p, uint32_t now) { return (p->pflags & 2) && now >= p->last_search + 60u; }

static void bit_put(uint32_t* b, int32_t i, int v)
{
    if (v) b[i >> 5] |= 1u << (i & 31);
    else b[i >> 5] &= ~(1u << (i & 31));
}

void ts_pf_mark(TS_PF* pf, const TS_PFWORLD* W, int32_t slot)
{
    int m, c;
    if (slot < 0 || slot >= pf->nslot) return;
    m = is_mob(W, slot);
    c = m && (W->slot[slot].path.pflags & 2);
    bit_put(pf->mob, slot, m);
    bit_put(pf->cand, slot, c);
    bit_put(pf->starter, slot, c && gate(&W->slot[slot].path, W->now));
}

void ts_pf_mark_all(TS_PF* pf, const TS_PFWORLD* W)
{
    int32_t s;
    for (s = 0; s < pf->nslot; s++) ts_pf_mark(pf, W, s);
}

/* the first set bit of b in [from, to), or -1 */
static int32_t bit_next(const uint32_t* b, int32_t from, int32_t to)
{
    int32_t i = from;
    while (i < to) {
        const uint32_t w = b[i >> 5] >> (i & 31);
        if (w) {
            int32_t j = i;
            uint32_t v = w;
            while (!(v & 1)) {
                v >>= 1;
                j++;
            }
            return j < to ? j : -1;
        }
        i = (i | 31) + 1;
    }
    return -1;
}

/* ---- set path and the search's end ------------------------------------------------------------------------------- */

/* 0x44F080(pts, n), thiscall ret 8, DISASSEMBLED 0x44F080..0x44F0F7. n = 0: a goal not reached by the unit (vtable
   +0x10, in_goal of its cell) is flagged no-path; bits 0 and 1 clear, bit 3 set. THE ENGINE'S, KEPT: the waypoint count
   is left as it was, so a failed search leaves the old count with bit 0 off, and the class step does not ask again
   unless the unit is blocked or the count is below 2. n > 0: the first min(n, 20) points, bit 0 set, bit 1 clear,
   bit 3 set -- the class keeps the 20 corners nearest the start, and a long path is followed 20 at a time. */
void ts_pf_set_path(TS_PF* pf, TS_PFWORLD* W, int32_t slot, const int16_t (*pts)[2], int32_t n)
{
    TS_PFSLOT* s = slot_at(W, slot);
    TS_PATH* p;
    if (!s) return;
    p = &s->path;
    if (n <= 0) {
        TS_GOAL* g = goal_at(W, p->goal);
        const TS_CUNIT* cu = &W->cm->units->u[slot];
        if (g && !ts_goal_in(g, cu->cx, cu->cz)) g->flags |= TS_GOALF_NOPATH;
        p->pflags = (uint8_t)((p->pflags & ~3u) | 8u);
    } else {
        const int32_t c = n < TS_PF_WP ? n : TS_PF_WP;
        memcpy(p->wp, pts, (size_t)c * sizeof p->wp[0]);
        p->count = c;
        p->pflags = (uint8_t)(((p->pflags | 1u) & ~2u) | 8u);
    }
    ts_pf_mark(pf, W, slot);
}

/* the failure every path out of a search shares (0x40E982..0x40E99B, 0x40E7AA..0x40E7C8, 0x40EEC9..0x40EEE2): set path
   with no points on the class object, the class's cleanup 0x440BE0 on the searcher, and the search and its class
   dropped -- the object and the goal stay */
static void fail(TS_PF* pf, TS_PFWORLD* W)
{
    ts_pf_set_path(pf, W, pf->cls_obj, NULL, 0);
    ts_class_cleanup(W->cm, pf->cls, pf->searching);
    pf->searching = 0;
    pf->cls = TS_PF_NOCLASS;
}

void ts_pf_abandon(TS_PF* pf, int32_t slot)
{
    if (slot != pf->cls_obj) return;
    pf->searching = 0;
    pf->cls_obj = 0;
    pf->goal = TS_PF_NOGOAL;
    pf->cls = TS_PF_NOCLASS;
}

/* ---- the bug walk ------------------------------------------------------------------------------------------------ */

static int32_t weighed(const TS_PF* pf, const TS_GOAL* g, int32_t x, int32_t z)
{
    return ts_mul_shr(ts_goal_h(g, x, z), pf->wgt, 16);
}

/* a step of the walk onto (x, z) by direction d: touched, b1 = d, b0 bit 3; 1 when the cell is a goal cell. The reader
   answered the cell passable, so it is on the class's grid, which the tick holds equal to the pathfinder's. */
static int walk_onto(TS_PF* pf, int32_t x, int32_t z, int d)
{
    TS_PFCELL* c;
    if (!inside(pf, x, z)) return 0;
    touch(pf, cidx(pf, x, z));
    c = &pf->grid[cidx(pf, x, z)];
    c->b1 = (uint8_t)d;
    c->b0 |= 8;
    return c->b0 & 4;
}

/* 0x40E3D5..0x40E43F: Q is on the m-line from P when, the axes turned so that the nearest goal cell lies at
   gx, gz >= 0 from P, Q is on its x leg (0 < ax <= gx, az = 0) or on its z leg (ax = gx, 0 < az <= gz): the L the
   straight walk follows, x first */
static int on_mline(const TS_PF* pf, int32_t qx, int32_t qz, int32_t px, int32_t pz)
{
    int32_t gx = SUB(pf->near_x, px), gz = SUB(pf->near_z, pz), ax = SUB(qx, px), az = SUB(qz, pz);
    if (gx < 0) {
        gx = SUB(0, gx);
        ax = SUB(0, ax);
    }
    if (gz < 0) {
        gz = SUB(0, gz);
        az = SUB(0, az);
    }
    return (az == 0 && ax > 0 && ax <= gx) || (ax == gx && az > 0 && az <= gz);
}

/* 0x40E160, thiscall, DISASSEMBLED 0x40E160..0x40E627: a bug walk from the start toward the goal cell nearest it,
   whose answer is the A*'s relaxation (a cell whose weighed heuristic is at most it counts as a goal cell). It
   answers 0 when it steps onto a goal cell or the weighed heuristic falls to 0, else the least weighed heuristic of
   every cell it stood on. A blocked start answers the start's.
   STRAIGHT: one step a cost, toward the nearest goal cell along x first, then z; a blocked step starts the follow.
   FOLLOW: two heads leave the blocked cell, A trying the eight directions from da - 2 upward, B mirrored on the other
   side, stepping by -D[db] and trying from db + 2 downward; one cost a pair of steps. A head with no free direction
   ends the walk; a head that reaches the m-line beyond P makes it P and the walk goes straight again; the heads'
   meeting ends the walk. THE ENGINE'S, KEPT: each meeting test compares a head's position before its step with the
   other's, and the second has no `started` term (0x40E352..0x40E376, 0x40E4F6..0x40E512).
   THE BOUND, the engine has none: a follow in which the heads neither meet, stick nor reach the m-line hangs the game.
   The core ends such a follow after 8 * W * H pairs of steps and answers its least heuristic. A head's own state, its
   cell and its direction, takes at most 8 * W * H values, so a head that has stepped that often in one follow repeats
   itself and can no longer stick or reach the m-line for the first time; only a meeting of two heads cycling with
   different periods could come later [INFERRED: none does on a map the game finishes]. MEASURED 2026-11-12 with a
   scratch driver over this function: on 2 000 000 random 10 x 10 class maps and 300 000 24 x 24 ones, 20 to 60 %
   blocked, random start and goal, no follow reached the bound, and the longest that ended took 41 and 162 pairs
   against bounds of 800 and 4608. Each straight run is finite
   (every step brings P nearer the goal cell along the L), and each new P lies further along the first L, so the
   follows are at most W + H. */
static int32_t bug_walk(TS_PF* pf, const TS_PFWORLD* W, const TS_GOAL* g)
{
    const int64_t bound = 8 * (int64_t)pf->w * pf->h;
    int32_t px = pf->start_x, pz = pf->start_z, best = weighed(pf, g, px, pz);
    int32_t ax, az, bx, bz, h;
    int64_t steps;
    int d = 0, da, db, lim, started;
    if (ts_pf_reader(pf, W, px, pz) < 1) return best;
    for (;;) {
        for (;;) {
            int32_t nx, nz, gx;
            pf->cost = ADD(pf->cost, 1);
            if (best == 0) return 0;
            gx = SUB(pf->near_x, px);
            d = gx < 0 ? 2 : gx > 0 ? 6 : (SUB(pf->near_z, pz) <= 0 ? 0 : 4);
            nx = px + DX[d];
            nz = pz + DZ[d];
            if (ts_pf_reader(pf, W, nx, nz) < 1) break;
            px = nx;
            pz = nz;
            if (walk_onto(pf, nx, nz, d)) return 0;
            h = weighed(pf, g, nx, nz);
            if (h < best) best = h;
        }
        ax = bx = px;
        az = bz = pz;
        da = db = (d + 2) & 7;
        started = 0;
        for (steps = 0;; steps++) {
            if (steps >= bound) return best;
            pf->cost = ADD(pf->cost, 1);
            lim = (da - 3) & 7;
            da = (da - 2) & 7;
            while (ts_pf_reader(pf, W, ax + DX[da], az + DZ[da]) < 1) {
                if (da == lim) return best;
                da = (da + 1) & 7;
            }
            if (ax == bx && az == bz && da == db && started) return best;
            ax += DX[da];
            az += DZ[da];
            started = 1;
            if (walk_onto(pf, ax, az, da)) return 0;
            if (on_mline(pf, ax, az, px, pz)) {
                px = ax;
                pz = az;
                break;
            }
            h = weighed(pf, g, ax, az);
            if (h < best) best = h;
            lim = (db + 3) & 7;
            db = (db + 2) & 7;
            while (ts_pf_reader(pf, W, bx - DX[db], bz - DZ[db]) < 1) {
                if (db == lim) return best;
                db = (db - 1) & 7;
            }
            if (bx == ax && bz == az && db == da) return best;
            bx -= DX[db];
            bz -= DZ[db];
            if (walk_onto(pf, bx, bz, db)) return 0;
            if (on_mline(pf, bx, bz, px, pz)) {
                px = bx;
                pz = bz;
                break;
            }
            h = weighed(pf, g, bx, bz);
            if (h < best) best = h;
        }
    }
}

/* ---- the heap ---------------------------------------------------------------------------------------------------- */

/* 0x40F000(i), thiscall ret 4, DISASSEMBLED 0x40F000..0x40F055: heap[i] moves up while its f is below its parent's,
   strictly; nothing is written when it does not move. */
static void sift_up(TS_PF* pf, int32_t i)
{
    uint32_t x;
    int32_t p;
    if (i == 0) return;
    x = pf->heap[i];
    p = (i - 1) >> 1;
    if (pf->pool[x].f >= pf->pool[pf->heap[p]].f) return;
    do {
        pf->heap[i] = pf->heap[p];
        pf->pool[pf->heap[i]].link = i;
        i = p;
        if (!i) break;
        p = (i - 1) >> 1;
    } while (pf->pool[x].f < pf->pool[pf->heap[p]].f);
    pf->heap[i] = x;
    pf->pool[x].link = i;
}

/* 0x40F060(i), thiscall ret 4, DISASSEMBLED 0x40F060..0x40F109: with both children present the right one is taken
   only when its f is below the left's, so ties go left; a child moves up only when its f is below heap[i]'s. */
static void sift_down(TS_PF* pf, int32_t i)
{
    const uint32_t x = pf->heap[i];
    const int32_t fx = pf->pool[x].f;
    for (;;) {
        const int32_t l = 2 * i + 1, r = 2 * i + 2;
        uint32_t c;
        int32_t to;
        if (r < pf->count) {
            const uint32_t L = pf->heap[l], R = pf->heap[r];
            if (pf->pool[R].f < pf->pool[L].f) {
                c = R;
                to = r;
            } else {
                c = L;
                to = l;
            }
        } else if (l < pf->count) {
            c = pf->heap[l];
            to = l;
        } else
            break;
        if (pf->pool[c].f >= fx) break;
        pf->heap[i] = c;
        pf->pool[c].link = i;
        i = to;
    }
    pf->heap[i] = x;
    pf->pool[x].link = i;
}

/* the free 0x40F1E0(i), DISASSEMBLED 0x40F1E0..0x40F1F6, and its inline copies: LIFO */
static void node_free(TS_PF* pf, uint32_t n)
{
    pf->pool[n].link = pf->freehead;
    pf->freehead = (int32_t)n;
}

/* a node from the free list, else the pool's next (0x40DE86..0x40DEAB, 0x40E8DE..0x40E902); -1 past the pool, which
   the bound of one node per cell keeps every search from reaching (ts_pf_bytes) */
static int32_t node_take(TS_PF* pf)
{
    int32_t n = pf->freehead;
    if (n != -1) {
        if ((uint32_t)n >= (uint32_t)pf->used) return -1;
        pf->freehead = pf->pool[n].link;
        return n;
    }
    if (pf->used >= pf->nodes) return -1;
    return pf->used++;
}

/* the node at heap position `at` taken out and the heap's last moved into its place (0x40EDE2..0x40EE03,
   0x40DBEF..0x40DC17): nothing moves when `at` was the last. THE BOUND: `at` is read from the node, a value. */
static void heap_fill(TS_PF* pf, int32_t at)
{
    pf->count -= 1;
    if (at < 0 || at >= pf->count) return;
    pf->heap[at] = pf->heap[pf->count];
    pf->pool[pf->heap[at]].link = at;
    sift_down(pf, at);
}

/* ---- A* ---------------------------------------------------------------------------------------------------------- */

/* 0x40DA70(L, pcell, k), thiscall ret 0xC, DISASSEMBLED 0x40DA70..0x40DEFC: the neighbour of the popped node L in
   direction d = (pcell.b1 + k) & 7, k the turn from the direction L was reached by.
   A NEW CELL (b0 & 3 = 0) is touched and read: blocked makes it dead (b0 |= 3) unless the bug walk stood on it
   (b0 bit 3), which leaves it new; else it opens, as a goal cell when its weighed heuristic is at most the walk's
   relaxation (b0 |= 5) and plainly otherwise (b0 |= 1), b1 = d, and its node has
       g = turnpen[k] + step[d] + L.g + pen, plus 75 when k != 0 and L.straight < 5,
   pen 30 for a bad cell (code 1) and 0 for a good or unexplored one, straight 1 after a turn and L.straight + 1
   ahead (a word), f = g + h. With the lazy top standing the node overwrites it in place and sifts down (a pop and a
   push in one); else it is pushed.
   AN OPEN CELL (b0 & 3 = 1) is relaxed when the new g, with the cell's own pen, is below its node's: b1 = d, f moved
   by the same amount, straight as above, and the node sifted up. With the lazy top standing, a relaxed node that
   displaces it from the root frees it, ends the lazy state and fills its place from the heap's last.
   A CLOSED OR DEAD CELL (b0 & 3 = 2, 3) is left.
   THE ENGINE'S, KEPT: the node word is 16 bits (0x40DD9A, 0x40DEF1, read 0x40DAE7), so past 65 536 live nodes a cell
   keeps a wrapped index and a relax reads another node. THE BOUNDS: that index against the pool's live part, and the
   relaxed node's heap position against the heap's count -- the engine sifts whatever its +0x00 holds, a free-list
   link included; the core sifts only a position in the heap. */
static void expand(TS_PF* pf, TS_PFWORLD* W, const TS_GOAL* g, const TS_PFNODE* L, int32_t pc, int k)
{
    const int d = (pf->grid[pc].b1 + k) & 7;
    const int32_t nx = L->x + DX[d], nz = L->z + DZ[d];
    const int32_t add = (k && L->straight < 5) ? 75 : 0;
    const int16_t straight = k ? 1 : (int16_t)(uint16_t)((uint16_t)L->straight + 1u);
    TS_PFCELL* c;
    int32_t ci;
    if (!inside(pf, nx, nz)) return;
    ci = cidx(pf, nx, nz);
    c = &pf->grid[ci];
    if ((c->b0 & 3) == 0) {
        int code, n;
        int32_t h, gg;
        int16_t pen;
        touch(pf, ci);
        code = ts_pf_reader(pf, W, nx, nz);
        if (code < 1) {
            if (!(c->b0 & 8)) c->b0 |= 3;
            return;
        }
        h = weighed(pf, g, nx, nz);
        c->b0 |= h <= pf->relax ? 5 : 1;
        c->b1 = (uint8_t)d;
        pen = code > 1 ? 0 : 30;
        gg = ADD(ADD(ADD(pf->turnpen[k], pf->step[d]), L->g), pen);
        gg = ADD(gg, add);
        if (pf->lazy) {
            n = (int32_t)pf->heap[0];
        } else {
            n = node_take(pf);
            if (n < 0) return;
        }
        pf->pool[n].x = (int16_t)nx;
        pf->pool[n].z = (int16_t)nz;
        pf->pool[n].g = gg;
        pf->pool[n].f = ADD(gg, h);
        pf->pool[n].pen = pen;
        pf->pool[n].straight = straight;
        if (pf->lazy) {
            sift_down(pf, 0);
            pf->lazy = 0;
        } else {
            pf->pool[n].link = pf->count;
            pf->heap[pf->count] = (uint32_t)n;
            pf->count += 1;
            sift_up(pf, pf->count - 1);
        }
        c->node = (uint16_t)n;
    } else if ((c->b0 & 3) == 1) {
        const int32_t n = c->node;
        TS_PFNODE* N;
        int32_t gg, hi;
        if (n >= pf->used) return;
        N = &pf->pool[n];
        gg = ADD(ADD(ADD(ADD(pf->turnpen[k], pf->step[d]), L->g), N->pen), add);
        if (gg >= N->g) return;
        c->b1 = (uint8_t)d;
        N->f = ADD(N->f, SUB(gg, N->g));
        N->g = gg;
        N->straight = straight;
        hi = N->link;
        if (!pf->lazy) {
            if (hi > 0 && hi < pf->count) sift_up(pf, hi);
        } else {
            const uint32_t top = pf->heap[0];
            int32_t at;
            if (hi > 0 && hi < pf->count) sift_up(pf, hi);
            at = pf->pool[top].link;
            if (at == 0) return;
            pf->lazy = 0;
            node_free(pf, top);
            heap_fill(pf, at);
        }
    }
}

/* the found search's path (0x40E050, thiscall, DISASSEMBLED 0x40E050..0x40E15D): back from the found cell by each
   cell's b1 to the start, a corner kept wherever b1 changes, in a ring of 64 -- so the 64 corners nearest the start
   survive -- and the start last; then the corners from the start on, each the cell's footprint centre in px,
   ((2x + fx) << 3, (2z + fz) << 3) as words, handed to set path.
   THE BOUND: the walk back reads b1 bytes off the grid. Each cell on it was opened or relaxed from a cell already
   closed, which never changes again, so the chain ends at the start within the closed cells; the core stops it after
   W * H steps or off the grid, and a b1 is taken & 7 (the engine's tables are 8 bytes; the core writes only 0..7). */
static void extract(TS_PF* pf, TS_PFWORLD* W)
{
    int16_t ring[TS_PF_RING][2], pts[TS_PF_RING][2];
    int16_t cx = pf->found_x, cz = pf->found_z;
    const int32_t fx = class_ok(W->cm, pf->cls) ? W->cm->c[pf->cls].fx : 0;
    const int32_t fz = class_ok(W->cm, pf->cls) ? W->cm->c[pf->cls].fz : 0;
    int32_t n = 1, m, i, steps = 0;
    int d = inside(pf, cx, cz) ? (int8_t)pf->grid[cidx(pf, cx, cz)].b1 : 0;
    ring[0][0] = cx;
    ring[0][1] = cz;
    while (!(cx == pf->start_x && cz == pf->start_z)) {
        int e;
        if (!inside(pf, cx, cz) || ++steps > pf->nodes) break;
        e = (int8_t)pf->grid[cidx(pf, cx, cz)].b1;
        if (e != d) {
            ring[n & (TS_PF_RING - 1)][0] = cx;
            ring[n & (TS_PF_RING - 1)][1] = cz;
            n++;
            d = e;
        }
        cx = (int16_t)(uint16_t)((uint16_t)cx - (uint16_t)DX[d & 7]);
        cz = (int16_t)(uint16_t)((uint16_t)cz - (uint16_t)DZ[d & 7]);
    }
    ring[n & (TS_PF_RING - 1)][0] = pf->start_x;
    ring[n & (TS_PF_RING - 1)][1] = pf->start_z;
    n++;
    m = n < TS_PF_RING ? n : TS_PF_RING;
    for (i = 0; i < m; i++) {
        const int16_t* q = ring[(n - 1 - i) & (TS_PF_RING - 1)];
        pts[i][0] = (int16_t)(uint16_t)(((uint32_t)(2 * q[0] + fx)) << 3);
        pts[i][1] = (int16_t)(uint16_t)(((uint32_t)(2 * q[1] + fz)) << 3);
    }
    ts_pf_set_path(pf, W, pf->cls_obj, (const int16_t (*)[2])pts, m);
}

/* The scheduler's in-search branch, DISASSEMBLED 0x40ED90..0x40EEE2: an empty open set fails the search; else pops,
   one cost each, until the cost reaches 100 or the open set empties (the next pass fails it). A pop frees the lazy
   top left by the previous one and copies the new top into L, which stays at heap[0] as the lazy top. A goal cell
   ends the search: the path, the class's cleanup, the search dropped. Otherwise the cell closes (a byte store of 2)
   and the directions -fan .. +fan around L's are expanded, then fan is 2. THE ENGINE'S, KEPT: the start node's fan of
   4 expands direction 4 twice, as k = -4 and k = 4; the second finds the cell the first opened and relaxes nothing. */
static void astar(TS_PF* pf, TS_PFWORLD* W)
{
    const TS_GOAL* g = goal_at(W, pf->goal);
    if (pf->count - pf->lazy == 0 || !g) {
        fail(pf, W);
        return;
    }
    for (;;) {
        TS_PFNODE L;
        int32_t pc, k;
        if (pf->count - pf->lazy == 0) return;
        pf->cost = ADD(pf->cost, 1);
        if (pf->lazy) {
            const uint32_t top = pf->heap[0];
            const int32_t at = pf->pool[top].link;
            pf->lazy = 0;
            node_free(pf, top);
            heap_fill(pf, at);
        }
        L = pf->pool[pf->heap[0]];
        pf->lazy = 1;
        if (!inside(pf, L.x, L.z)) {
            fail(pf, W);
            return;
        }
        pc = cidx(pf, L.x, L.z);
        if (pf->grid[pc].b0 & 4) {
            pf->found_x = L.x;
            pf->found_z = L.z;
            extract(pf, W);
            ts_class_cleanup(W->cm, pf->cls, pf->searching);
            pf->searching = 0;
            pf->cls = TS_PF_NOCLASS;
            return;
        }
        pf->grid[pc].b0 = 2;
        for (k = -pf->fan; k <= pf->fan; k++) expand(pf, W, g, &L, pc, k & 7);
        pf->fan = 2;
        if (pf->cost >= 100) return;
    }
}

/* 0x40E630(goal), thiscall ret 4, DISASSEMBLED 0x40E630..0x40E99E. The searcher's class and cell, the class's
   watermark 0x440AF0 (an obstacle is a unit that has stood still for 30 ticks), the grid reset; each goal cell on the
   grid marked a goal (a byte store of 4) and the one nearest the start kept -- d^2 an imul sum that wraps, THE
   ENGINE'S, KEPT, the first of equals winning. A start in the goal flags 0x100 and fails; a start off the grid flags
   0x200 and fails. Then the bug walk: 0 flags 0x100, anything else 0x200, and a walk that got no nearer than the
   start's own weighed heuristic fails. Otherwise the A* starts empty with the start node: g 0, f the start's weighed
   heuristic, straight 100, b1 the searcher's heading rounded to a direction, fan 4.
   THE ENGINE'S, KEPT: the start node's pen word is uninitialised stack ([esp+0x2C]) and nothing reads it (scout-Jb
   J6: the expansion reads an open neighbour's own pen, and the start cell is closed before any expansion); the core
   stores 0, and the word is never compared with an engine node. The push's lazy branch 0x40E882..0x40E8C9 is dead.
   THE BOUND: a path naming no goal fails the search; the engine calls through a NULL goal and faults, and bit 1 is
   set only with a goal. */
static void search_start(TS_PF* pf, TS_PFWORLD* W, int32_t goal)
{
    const int32_t slot = pf->searching;
    const TS_CUNIT* cu = &W->cm->units->u[slot];
    TS_GOAL* g;
    TS_PFCELL* c;
    int32_t best = INT32_MAX, h0, r, ci, n;
    int16_t x, z;
    int64_t i;
    pf->cls = W->slot[slot].cls;
    pf->goal = goal;
    pf->start_x = cu->cx;
    pf->start_z = cu->cz;
    ts_class_watermark(W->cm, pf->cls, slot, W->now);
    memcpy(pf->turnpen, TURNPEN, 8);
    memcpy(pf->step, STEP, 8);
    reset_grid(pf);
    g = goal_at(W, goal);
    if (!g) {
        fail(pf, W);
        return;
    }
    for (i = 0; ts_goal_cell(g, i, &x, &z); i++) {
        int32_t ddx, ddz, d2;
        if (inside(pf, x, z)) {
            pf->grid[cidx(pf, x, z)].b0 = 4;
            touch(pf, cidx(pf, x, z));
        }
        ddz = SUB(pf->start_z, z);
        ddx = SUB(pf->start_x, x);
        d2 = ADD(MUL(ddx, ddx), MUL(ddz, ddz));
        if (d2 < best) {
            pf->near_x = x;
            pf->near_z = z;
            best = d2;
        }
    }
    if (ts_goal_in(g, pf->start_x, pf->start_z)) {
        g->flags |= TS_GOALF_WALKIN;
        fail(pf, W);
        return;
    }
    h0 = weighed(pf, g, pf->start_x, pf->start_z);
    if (!inside(pf, pf->start_x, pf->start_z)) {
        g->flags |= TS_GOALF_WALKOUT;
        fail(pf, W);
        return;
    }
    r = bug_walk(pf, W, g);
    pf->relax = r;
    if (r == 0)
        g->flags |= TS_GOALF_WALKIN;
    else {
        g->flags |= TS_GOALF_WALKOUT;
        if (r >= h0) {
            fail(pf, W);
            return;
        }
    }
    pf->count = 0;
    pf->freehead = -1;
    pf->used = 0;
    pf->lazy = 0;
    ci = cidx(pf, pf->start_x, pf->start_z);
    touch(pf, ci);
    c = &pf->grid[ci];
    c->b0 |= 1;
    c->b1 = (uint8_t)((((uint32_t)W->slot[slot].heading + 0x1000u) >> 13) & 7);
    n = node_take(pf);
    pf->pool[n].link = 0;
    pf->pool[n].x = pf->start_x;
    pf->pool[n].z = pf->start_z;
    pf->pool[n].g = 0;
    pf->pool[n].f = h0;
    pf->pool[n].pen = 0;
    pf->pool[n].straight = 100;
    pf->heap[0] = (uint32_t)n;
    pf->count = 1;
    c->node = (uint16_t)n;
    pf->fan = 4;
}

/* ---- the scheduler ----------------------------------------------------------------------------------------------- */

/* one examination, DISASSEMBLED 0x40ECDA..0x40ED8B: the served player's count of slots examined (0x40ECF9), its cursor
   one slot on, back to its block's first after the last; then an occupied slot with a mover and a class asks its
   class object whether it wants a search (vtable +0x18, 0x44F260), and the answer -- the object, or 0 -- is stored in
   cls_obj whatever it is (0x40ED5C, before the branch). A yes stamps last_search, costs 100 more and starts the
   search with the served player's weight. */
static void examine(TS_PF* pf, TS_PFWORLD* W)
{
    const TS_PFSEAT* s = &W->seat[pf->rr];
    TS_PFSLOT* u;
    int32_t slot;
    pf->exams[pf->rr] = ADD(pf->exams[pf->rr], 1);
    pf->cursor[pf->rr] = pf->cursor[pf->rr] == s->last ? s->first : ADD(pf->cursor[pf->rr], 1);
    slot = pf->cursor[pf->rr];
    if (!is_mob(W, slot)) return;
    u = &W->slot[slot];
    if (!gate(&u->path, W->now)) {
        pf->cls_obj = 0;
        return;
    }
    u->path.last_search = W->now;
    pf->cls_obj = slot;
    pf->cost = ADD(pf->cost, 100);
    pf->searching = slot;
    pf->wgt = pf->weight[pf->rr];
    ts_pf_mark(pf, W, slot);
    search_start(pf, W, u->path.goal);
}

/* THE SKIP-AHEAD, the core's: the m examinations ahead that the engine's loop would make one at a time and that start
   nothing, applied at once. It is exact:
   - the served player p stays served while its credit c is positive, and the tick goes on while the total T is;
     every examination costs 1, so the next m = min(k - 1, c - 1, T - 1) leave both at 1 or more, where k is the
     distance in p's circular block from its cursor to the next starter;
   - an examination that starts nothing writes the four counters (exams[p] + 1, cursor + 1 in the block, credit - 1,
     the total - 1), the cost (1), and cls_obj = 0 when the slot is a mover with a class, because wants_search writes
     nothing when it answers 0 (0x44F260..0x44F284) but the scheduler stores the answer; so m of them are those
     counters moved by m and cls_obj = 0 when any of the m slots is a mover with a class;
   - a starter is a mover with a class, bit 1 and the 60-tick gate. GameTime does not change inside the tick, and the
     only writes the tick makes to what the gate reads are the stamp of a search's own start and set path's clear of
     bit 1 on its searcher, both re-marked where they happen; so the starters taken at the tick's start, kept by those
     re-marks, are the slots the engine's examinations would find wanting a search, for the whole tick.
   The step after the m is the engine's literal one. tests/tasim_pf_test.c runs both side by side on 10^4 random
   worlds and requires every field of the object, its globals, the paths and the goals equal. A cursor outside its
   block, or a block outside the slots, takes the literal steps alone. */
static void skip_run(TS_PF* pf, const TS_PFWORLD* W, int32_t* total)
{
    const int p = pf->rr;
    const TS_PFSEAT* s = &W->seat[p];
    int32_t len, pos, q, k = INT32_MAX, m, a, b;
    if (s->first < 1 || s->last < s->first || s->last >= pf->nslot || pf->cursor[p] < s->first ||
        pf->cursor[p] > s->last)
        return;
    len = s->last - s->first + 1;
    pos = pf->cursor[p] - s->first;
    q = bit_next(pf->starter, s->first + pos + 1, s->last + 1);
    if (q >= 0) k = q - s->first - pos;
    else {
        q = bit_next(pf->starter, s->first, s->first + pos + 1);
        if (q >= 0) k = len - pos + (q - s->first);
    }
    m = k - 1;
    if (pf->credit[p] - 1 < m) m = pf->credit[p] - 1;
    if (*total - 1 < m) m = *total - 1;
    if (m <= 0) return;
    /* the m slots after the cursor, around the block as often as m asks */
    if (m >= len) a = bit_next(pf->mob, s->first, s->last + 1);
    else if (pos + m < len) a = bit_next(pf->mob, s->first + pos + 1, s->first + pos + m + 1);
    else {
        a = bit_next(pf->mob, s->first + pos + 1, s->last + 1);
        b = bit_next(pf->mob, s->first, s->first + (pos + m - len) + 1);
        if (a < 0) a = b;
    }
    if (a >= 0) pf->cls_obj = 0;
    pf->exams[p] = ADD(pf->exams[p], m);
    pf->cursor[p] = s->first + (int32_t)(((int64_t)pos + m) % len);
    pf->credit[p] -= m;
    *total -= m;
}

/* the starters of this tick, from the candidates by the gate */
static void take_starters(TS_PF* pf, const TS_PFWORLD* W)
{
    const int32_t nw = slot_words(pf->nslot);
    int32_t i;
    for (i = 0; i < nw; i++) {
        uint32_t bits = pf->cand[i], out = 0;
        int j;
        for (j = 0; bits; j++, bits >>= 1)
            if ((bits & 1) && gate(&W->slot[(i << 5) + j].path, W->now)) out |= 1u << j;
        pf->starter[i] = out;
    }
}

/* 0x40EB70, DISASSEMBLED 0x40EB70..0x40EF18. With no players it does nothing. The budget is shared by the player
   count (idiv). Every 150th call rewrites all ten weights from the slots each player's turn examined over S: below 1
   six times the base (9 in 16.16), below 2 three times (4.5), else the base (1.5); and zeroes the counts. Each
   credited player's credit grows by the share, and the total is the sum of theirs. Then, while the total is positive:
   with no search under way, one examination (cost 1) of the first player from rr on with a positive credit; with
   one, the A* steps; the cost comes off the total and off rr's credit -- a search, its bug walk and its pops are
   charged to the player it started for, across ticks. A player that overspends stops every player until the sum is
   positive again. THE BOUNDS: a world whose class maps are not this grid's size is refused (the reader's answers and
   the grid's cells must name the same cells); S = 0, on which the engine's idiv faults, rewrites as a count of 0;
   and the search for a positive credit stops after the ten players -- the engine's loop does not, but cannot need
   to: the credits of the credited players exceed the total by the costs charged to the others, so one is positive
   whenever the total is. */
static void tick(TS_PF* pf, TS_PFWORLD* W, int skip)
{
    int32_t share, total = 0;
    int p;
    if (!W->players || !W->cm->units || W->cm->w != pf->w || W->cm->h != pf->h || W->cm->units->n != pf->nslot)
        return;
    share = pf->budget / (int32_t)W->players;
    pf->calls = ADD(pf->calls, 1);
    if (pf->calls >= 150) {
        pf->calls = 0;
        for (p = 0; p < TS_PF_SEATS; p++) {
            const int32_t r = W->per_seat ? pf->exams[p] / (int32_t)W->per_seat : 0;
            pf->weight[p] = r < 1 ? MUL(pf->base_w, 6) : r < 2 ? MUL(pf->base_w, 3) : pf->base_w;
            pf->exams[p] = 0;
        }
    }
    for (p = 0; p < TS_PF_SEATS; p++)
        if (W->seat[p].credited) {
            pf->credit[p] = ADD(pf->credit[p], share);
            total = ADD(total, pf->credit[p]);
        }
    if (total <= 0) return;
    if (skip) take_starters(pf, W);
    do {
        pf->cost = 0;
        if (!pf->searching) {
            int tries = 0;
            pf->cost = 1;
            while (pf->credit[pf->rr] <= 0) {
                if (++tries > TS_PF_SEATS) return;
                pf->rr = (uint8_t)(pf->rr + 1 < TS_PF_SEATS ? pf->rr + 1 : 0);
            }
            if (skip) skip_run(pf, W, &total);
            examine(pf, W);
        } else
            astar(pf, W);
        total = SUB(total, pf->cost);
        pf->credit[pf->rr] = SUB(pf->credit[pf->rr], pf->cost);
    } while (total > 0);
}

void ts_pf_tick(TS_PF* pf, TS_PFWORLD* W) { tick(pf, W, 1); }
void ts_pf_tick_literal(TS_PF* pf, TS_PFWORLD* W) { tick(pf, W, 0); }
