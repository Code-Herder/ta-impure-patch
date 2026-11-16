/* impai_build.c -- where ImpAI builds and how it orders it (impai.h, the brain): a metal patch for an
   extractor, the base ring for everything else, a cell to explore, a building to help. Every site is tested
   through the seat's own site test (WQ_SITE_SEAT) and every unit named by slot, the adapter re-deriving it
   before each command. Engine-free, no statics. */
#include <string.h>
#include "impai_brain.h"
#include "impai_mil.h"
#include "tagpu_simreg.h"

/* The sites and the build orders. OWNER: the brain calls them only inside a live ImpAI seat's turn, which
   plays no puppet seat (tagpu_impai.c, impai_play), so a puppet's orders come from the recording. */
SIM_FEATURE(impai_build, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval impai-raid impai-persona");

/* ---- a plant's way out ---- */

/* THE WAY OUT. A plant's finished unit leaves over the plant's yard cells. A yard byte (def +0x14E, fx x fz,
   row-major) is stamped into its cell's unit word only for bit 0x04 while the yard is closed and only for bit
   0x02 while it is open (the stamp 0x47CC30 picks the mask by YARD_OPEN, +0x10F bit 2, at 0x47CD88..0x47CD9D), so
   a byte with 0x04 and without 0x02 -- `c`, `C` -- is a cell the unit is built on and walks off (the yardmap
   characters: engine/render.md). An edge of the footprint holding such cells is an EXIT. A plant whose every
   exit leads into a pocket traps what it builds: MEASURED 2026-11-13, a CORVP on Pincushion with LavaSpire006C
   (6 x 5 cells, blocking) below its exit, a CORVP on Trout Farm with Building14 (5 x 5) across half of it and an
   ARMVP on Pincushion with a lava feature below it -- each plant's constructors circled on or beside the yard
   for the rest of the game while MobileBuild ended every order they were given (impai_brain.c, THE REACH
   ABANDON). THE RULE: a plant's site is taken only when a search proves, for each movement class of its ground
   products, that the class's largest product can leave it.
   - THE PRODUCTS. The mobile types of the plant's build menu that do not fly and have a movement class. For
     each class c, s_c is the largest side of its products' def footprints (+0x14A, +0x14C) and (fx_c, fz_c) the
     least of them on each axis, which is the class's own footprint: the loader copies the class record's +4, +6
     to def +0x14A, +0x14C (0x42CD5D..0x42CD97, engine/keys.md).
   - ROOM AT THE EXIT. Class c's search starts from every place just beyond an exit where the s_c x s_c square
     lies wholly outside the plant and beside the exit's run of yard cells.
   - THE PRODUCT FITS ALL THE WAY. A place is open for class c when
       * the class's content map (worldq.h, passable: its pathing map as the level load's fill makes it from the
         map's heights and features, through the per-cell rule 0x47DE60 less its unit rule, with the class's
         footprint folded in at the cell it starts on -- engine/map.md, The pathing maps) answers every entry
         [x, x + s_c - fx_c] x [z, z + s_c - fz_c] passable: those footprints cover the whole square, so the
         product stands anywhere on it whatever its shape;
       * the square overlaps no BLOCKER: a claim but another plant's way (the seat's planned and standing sites,
         which no map holds until a building is begun), a building of the seat's census, or a building of another
         player that the seat's rows hold, over every footprint its last-known block allows (way_blocker);
       * and the square does not overlap the plant.
     No unit is read, ever: the engine's own map holds every player's structures and movers in slot A, seen or
     not, and a seat may know only what it sees (research/notes/impai/player.md, No cheating). The pathfinder
     plans a unit by its class's map (engine/units.md, the pathfinder), so the content map with the buildings
     the seat knows of is the map less what it cannot know.
   - A PATH LEADS OUT. A breadth-first search over the open places, four-connected, from every start: the way
     is proven at the first place whose square stands WAY_OPEN cells clear of the plant. WAY_OPEN is 16 cells,
     256 px: twice the side of the largest retail plant (ARMSY and CORSY, 8 x 8), so a proven way leaves the
     plant's own neighbourhood -- where the base ring sets its next sites -- and a pocket that does not open
     within that distance is refused.
   EVERY CLASS ON ITS OWN PATH. Classes differ in what they cross -- a hovercraft's map holds water a K-bot's
   does not -- so the largest product's path proves nothing of another class's, and one path open to every class
   at once is a stricter rule than the units obey, which a plant whose classes leave by different sides would
   fail. So each class is searched alone and the site is taken when every class's path is proven.
   TERMINATION AND BOUND: the box is the plant grown by WAY_OPEN + the largest s_c on every side, at most
   IMPAI_WAY_BOX cells a side (a larger box is refused). Each class's search reads the content map over the box
   once (one passable call) and the blockers once a proof (way_raster: each claim and building one rectangle,
   each of the view's rows one test), so a place costs a few byte reads; a place enters the queue at most once
   (the visited bits) and the queue holds at most IMPAI_WAY_NODES places, so a search ends at the proof, at an
   empty queue (refused) or at a full one (refused, never accepted). A candidate site's classes are searched all
   at once after the site test has accepted it; a plant with more than WAY_SEARCHES classes is refused. A site
   refused is avoided by plants of its type for fail_t (THE WAY REFUSALS), so the next decision searches other
   sites.
   THE WORK BUDGET. A site's proof costs, in cells, the raster over its box once and, for each class, the
   content map's window (the box grown by the class footprint + 2, which tagpu_worldq.c reads and folds in
   O(window)) and the box again (the square fit, the visited bits and the search, at most one place each):
   way_cost, computed from the exits and the box before any of it is done. A decision spends at most
   WAY_WORK cells. A proof that would pass what is left is DEFERRED, never refused: way_prove answers -1, the
   caller stops proving for the decision, and the next decision, its budget whole, comes back to the site. A
   proof that costs more than a whole WAY_WORK can never run and is refused. TERMINATION: the first proof of a
   decision always fits, so every decision proves or refuses at least one site and a deferral is never
   answered twice for want of budget alone.
   THE COST, MEASURED 2026-11-13 on the reference setup, the DLL's compiler and flags under Wine, each part timed
   alone on synthetic worst cases. At the largest box, 160 a side, with a 61-cell product -- reachable only by a
   mod's footprints -- one class costs 100 929 cells, its content map 0.35 ms and the brain's part 0.07 to 0.11
   ms with the search at its node cap, 4.6 ns a cell. At a retail plant's box, 54 a side with a class footprint
   of 8, one class costs 9 928 cells, 0.03 ms of content map and at most 0.03 ms of the brain's, 6.0 ns a cell.
   WAY_WORK is 98 304 cells: at the worse 6.0 ns a cell 0.59 ms, inside the brain's 1 ms a seat a tick with 40 %
   to spare, and it holds a retail plant's eight classes in one decision (2 916 + 8 x 7 012 = 58 012 cells). A
   class whose proof alone passes it -- a mod's product of about 50 cells or more at the largest box -- is
   refused, and its plant falls back (THE PLANT'S FALLBACK).
   THE CLAIM. The proven paths' squares, from each start to where it ends, are kept as at most IMPAI_WAY_RECTS
   rectangles over all classes -- a rectangle one already kept holds adds none, and a way that needs more is
   refused -- in the seat's state (IMPAI_BRAIN.way) when the plant is ordered, and claimed at every census while
   a plant of that type stands at the site or a build order for it is held (impai_ways_keep): no later building,
   extractor or plant of the seat's is placed on it, for as long as the plant stands.
   A PLANT WHOSE YARD OR MENU CANNOT BE READ WHOLE IS REFUSED, never taken as one nothing leaves; so is one with a
   product whose footprint lies outside 1..WQ_FOOT_MAX.
   GAPS: a product with no movement class is not tested (every retail ground unit names one); a plant ImpAI did
   not place keeps no way; a building of another player the seat has never seen is not a blocker, and its unit
   meets it as a human's would; the content map holds the level load's fill, which at the map's last column and
   row a re-mark lowers (engine/map.md, the fill and the refresh disagree). */
#define WAY_OPEN     16         /* cells: where a way out has left the plant                              */
#define WAY_SEARCHES 8          /* a plant's classes at most                                              */
#define WAY_WORK     98304      /* cells a decision: 0.59 ms at the measured 6.0 ns a cell (THE COST)     */

static void exits_make(IMPAI_DEC* d, int type, IMPAI_EXITS* L)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_TINFO* t = &in->ti[type];
    const int fx = t->fx, fz = t->fz;
    int n, k, side;
    memset(L, 0, sizeof *L);
    L->type = (uint16_t)type;
    if (fx < 1 || fz < 1 || fx > WQ_FOOT_MAX || fz > WQ_FOOT_MAX ||
        d->q->ops->yard(d->q, type, d->yard, (int)sizeof d->yard) != fx * fz)
        return;
    /* the whole menu, into the decision's WQ_TYPES_MAX scratch: a menu read short is refused, never tested in part */
    if ((n = d->q->ops->build_options(d->q, type, d->menu2, WQ_TYPES_MAX)) < 0) return;
    for (k = 0; k < n; k++) {
        const IMPAI_TINFO* p;
        unsigned c;
        if (d->menu2[k] >= in->nti) continue;
        p = &in->ti[d->menu2[k]];
        if (!(p->flags & IMPAI_TF_MOBILE) || (p->flags & IMPAI_TF_VTOL) || p->mclass >= WQ_MCLASSES) continue;
        if (p->fx < 1 || p->fz < 1 || p->fx > WQ_FOOT_MAX || p->fz > WQ_FOOT_MAX) return;
        c = p->mclass;
        side = p->fx > p->fz ? p->fx : p->fz;
        if (!(L->classes >> c & 1u)) {
            L->csize[c] = (uint8_t)side;
            L->cfx[c] = (uint8_t)p->fx;
            L->cfz[c] = (uint8_t)p->fz;
        } else {
            if (side > L->csize[c]) L->csize[c] = (uint8_t)side;
            if (p->fx < L->cfx[c]) L->cfx[c] = (uint8_t)p->fx;
            if (p->fz < L->cfz[c]) L->cfz[c] = (uint8_t)p->fz;
        }
        if (side > L->size) L->size = (uint8_t)side;
        L->classes |= 1u << c;
    }
    L->read = 1;
    if (!L->classes) return;
    for (side = 0; side < 4; side++) {         /* north, south, west, east */
        const int along_x = side < 2, len = along_x ? fx : fz;
        int a = -1, e = -1, i;
        for (i = 0; i < len; i++) {
            const int x = along_x ? i : (side == 2 ? 0 : fx - 1), z = along_x ? (side == 0 ? 0 : fz - 1) : i;
            const uint8_t y = d->yard[z * fx + x];
            if ((y & 0x04) && !(y & 0x02)) {
                if (a < 0) a = i;
                e = i;
            }
        }
        if (a < 0) continue;
        L->edge[L->n] = (uint8_t)side;
        L->a[L->n] = (int16_t)a;
        L->e[L->n] = (int16_t)e;
        L->n++;
    }
}

/* the plant type's exits: the decision's copy, made at first use while there is room, else made into *tmp */
static const IMPAI_EXITS* exits_of(IMPAI_DEC* d, int type, IMPAI_EXITS* tmp)
{
    int k;
    for (k = 0; k < d->nexits; k++) if (d->exits[k].type == type) return &d->exits[k];
    if (d->nexits < IMPAI_EXIT_TYPES) tmp = &d->exits[d->nexits++];
    exits_make(d, type, tmp);
    return tmp;
}

static IMPAI_RECT square_at(int x, int z, int size)
{
    IMPAI_RECT r;
    r.x0 = (int16_t)x; r.z0 = (int16_t)z;
    r.x1 = (int16_t)(x + size - 1); r.z1 = (int16_t)(z + size - 1);
    return r;
}

static int rects_overlap(const IMPAI_RECT* a, const IMPAI_RECT* b)
{
    return a->x0 <= b->x1 && b->x0 <= a->x1 && a->z0 <= b->z1 && b->z0 <= a->z1;
}

/* the cells between rectangle r and the plant p along the axis that separates them most; below 0 they overlap */
static int clear_of(const IMPAI_RECT* r, const IMPAI_RECT* p)
{
    int g = r->x0 - p->x1 - 1, h;
    if ((h = p->x0 - r->x1 - 1) > g) g = h;
    if ((h = r->z0 - p->z1 - 1) > g) g = h;
    if ((h = p->z0 - r->z1 - 1) > g) g = h;
    return g;
}

/* one proof's box, its plant and blockers, and the class searched: `size` its square, s_c */
typedef struct WAY {
    const IMPAI_EXITS* L;
    IMPAI_RECT plant, box;
    int bw, bh, n, size;
} WAY;

/* the blocker raster's side: the box and a cell of room on each side for its sums */
#define BLOCK_SIDE(n) ((n) + 2)

/* r, clipped to the box, into the blocker raster's differences (way_raster) */
static void block_add(int32_t* P, const WAY* w, const IMPAI_RECT* r)
{
    const int W = BLOCK_SIDE(w->bw);
    int x0 = r->x0 - w->box.x0, z0 = r->z0 - w->box.z0, x1 = r->x1 - w->box.x0, z1 = r->z1 - w->box.z0;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 > w->bw - 1) x1 = w->bw - 1;
    if (z1 > w->bh - 1) z1 = w->bh - 1;
    if (x0 > x1 || z0 > z1) return;
    P[(z0 + 1) * W + x0 + 1]++;
    P[(z0 + 1) * W + x1 + 2]--;
    P[(z1 + 2) * W + x0 + 1]--;
    P[(z1 + 2) * W + x1 + 2]++;
}

/* Another player's building the seat's row holds, as every footprint whose centre lies in the row's 32-px block:
   a building stands at its footprint's centre, (2 origin + f) << 19 px (tagpu_placegeo.h), and the block is that
   centre's (impai.h, IMPAI_SEEN), so 2 origin + f is one of 4 bx .. 4 bx + 3 and the origins lie between
   floor((4 bx - f) / 2) and floor((4 bx + 3 - f) / 2). 0 when the row holds no such building. */
static int half_floor(int v) { return v >= 0 ? v / 2 : -((1 - v) / 2); }

static int way_blocker(const IMPAI_DEC* d, int seat, const IMPAI_SEEN* s, IMPAI_RECT* r)
{
    const IMPAI_TINFO* t;
    if (!s->how || s->owner >= WQ_SEATS || s->owner == seat || s->type == WQ_NONE || s->type >= d->in->nti) return 0;
    t = &d->in->ti[s->type];
    if ((t->flags & IMPAI_TF_MOBILE) || t->fx < 1 || t->fz < 1) return 0;
    r->x0 = (int16_t)half_floor(4 * s->bx - t->fx);
    r->z0 = (int16_t)half_floor(4 * s->bz - t->fz);
    r->x1 = (int16_t)(half_floor(4 * s->bx + 3 - t->fx) + t->fx - 1);
    r->z1 = (int16_t)(half_floor(4 * s->bz + 3 - t->fz) + t->fz - 1);
    return 1;
}

/* THE BLOCKER RASTER, once a proof: the cells of the box under a blocker (THE WAY OUT), as sums over the box --
   P at row z + 1, column x + 1 counts the blocked cells of [0, x] x [0, z], box-relative -- so a square's test is
   four reads. Two passes over the box: the rectangles' differences summed into coverage, then the covered cells
   summed into the counts, in place in row-major order, each entry read before it is written and its neighbours
   above and to the left already summed. BOUND: the claims (IMPAI_CLAIM_MAX), the census's buildings
   (IMPAI_CENSUS_MAX) and the view's rows, one rectangle each; a count at most the box's cells. */
static void way_raster(IMPAI_DEC* d, const WAY* w)
{
    const IMPAI_VIEW* v = d->in->view;
    const int W = BLOCK_SIDE(w->bw), H = BLOCK_SIDE(w->bh);
    int32_t* P = d->way_block;
    int i, j;
    memset(P, 0, (size_t)W * (size_t)H * sizeof *P);
    for (i = 0; i < d->nclaims; i++)
        if (!d->claims[i].way) block_add(P, w, &d->claims[i].r);
    for (i = 0; i < d->nstructs; i++) block_add(P, w, &d->structs[i].r);
    if (v && v->rows && v->seat >= 0 && v->seat < WQ_SEATS)
        for (i = 1; i < v->nrows; i++) {
            IMPAI_RECT r;
            if (way_blocker(d, v->seat, &v->rows[i].seat[v->seat], &r) && rects_overlap(&r, &w->box))
                block_add(P, w, &r);
        }
    for (j = 1; j < H; j++)
        for (i = 1; i < W; i++) P[j * W + i] += P[(j - 1) * W + i] + P[j * W + i - 1] - P[(j - 1) * W + i - 1];
    for (j = 1; j < H; j++)
        for (i = 1; i < W; i++)
            P[j * W + i] = (P[j * W + i] > 0) + P[(j - 1) * W + i] + P[j * W + i - 1] - P[(j - 1) * W + i - 1];
}

/* the s x s square at box-relative (x, z), inside the box, holds a blocked cell */
static int way_blocked(const IMPAI_DEC* d, const WAY* w, int x, int z, int s)
{
    const int W = BLOCK_SIDE(w->bw);
    const int32_t* P = d->way_block;
    return P[(z + s) * W + x + s] - P[z * W + x + s] - P[(z + s) * W + x] + P[z * W + x] > 0;
}

/* THE SQUARE'S FIT, in place over the content map of the box's places (bw x bh, row-major): a place becomes 1 when
   every entry of [x, x + ex] x [z, z + ez] is passable, 0 otherwise, an entry past the box counting blocked. A row
   pass then a column pass, each from the far end, keeping the nearest blocked entry at or past the place: each
   reads an entry before it writes it and looks only past it. */
static void square_fit(uint8_t* m, int bw, int bh, int ex, int ez)
{
    int i, j;
    for (j = 0; j < bh; j++) {
        uint8_t* row = m + (size_t)j * (size_t)bw;
        int shut = bw;
        for (i = bw - 1; i >= 0; i--) {
            if (!row[i]) shut = i;
            row[i] = (uint8_t)(shut > i + ex);
        }
    }
    for (i = 0; i < bw; i++) {
        int shut = bh;
        for (j = bh - 1; j >= 0; j--) {
            uint8_t* p = m + (size_t)j * (size_t)bw + (size_t)i;
            if (!*p) shut = j;
            *p = (uint8_t)(shut > j + ez);
        }
    }
}

/* the place (x, z) is open for the class searched (THE WAY OUT): its square in the box, not seen, clear of the
   plant, fitting the class's content map and holding no blocker */
static int way_open(IMPAI_DEC* d, const WAY* w, int x, int z)
{
    const IMPAI_RECT r = square_at(x, z, w->size);
    const int lx = x - w->box.x0, lz = z - w->box.z0, i = lz * w->bw + lx;
    if (r.x0 < w->box.x0 || r.z0 < w->box.z0 || r.x1 > w->box.x1 || r.z1 > w->box.z1) return 0;
    if (d->way_seen[i >> 3] >> (i & 7) & 1) return 0;
    if (clear_of(&r, &w->plant) < 0 || !d->way_pass[i]) return 0;
    return !way_blocked(d, w, lx, lz, w->size);
}

/* queue place (x, z), reached from queue index `from` (-1 a start): 1 queued, 0 not open, -1 the queue full */
static int way_push(IMPAI_DEC* d, WAY* w, int x, int z, int from)
{
    int i;
    if (!way_open(d, w, x, z)) return 0;
    if (w->n >= IMPAI_WAY_NODES) return -1;
    i = (z - w->box.z0) * w->bw + (x - w->box.x0);
    d->way_seen[i >> 3] = (uint8_t)(d->way_seen[i >> 3] | 1u << (i & 7));
    d->way_x[w->n] = (int16_t)x;
    d->way_z[w->n] = (int16_t)z;
    d->way_from[w->n] = (int16_t)from;
    w->n++;
    return 1;
}

/* rectangle (x0, z0, x1, z1) into the plant's way: none when a kept one holds it; 0 when the way is full */
static int way_add(IMPAI_WAYREC* out, const IMPAI_RECT* r)
{
    int k;
    for (k = 0; k < out->n; k++)
        if (out->r[k][0] <= r->x0 && out->r[k][1] <= r->z0 && out->r[k][2] >= r->x1 && out->r[k][3] >= r->z1) return 1;
    if (out->n >= IMPAI_WAY_RECTS) return 0;
    out->r[out->n][0] = r->x0; out->r[out->n][1] = r->z0;
    out->r[out->n][2] = r->x1; out->r[out->n][3] = r->z1;
    out->n++;
    return 1;
}

/* The squares of the path ending at queue index `end`, walked back to its start, as rectangles holding every
   one of them, added to the plant's way: a square joins the rectangle before it while their union has at most
   twice the cells its squares can cover -- s^2 for the first and s for each step after it, a step moving one
   cell -- so a straight run is one rectangle and a bend or a stair costs at most twice its own cells. 0 when the
   way would need more than IMPAI_WAY_RECTS. The walk ends: each step goes to a lower queue index, and a start's
   is -1. */
static int way_rects(IMPAI_DEC* d, const WAY* w, int end, IMPAI_WAYREC* out)
{
    const int s = w->size;
    IMPAI_RECT cur = square_at(d->way_x[end], d->way_z[end], s);
    int k, steps = 0;
    for (k = d->way_from[end]; ; k = d->way_from[k]) {
        if (k >= 0) {
            const IMPAI_RECT r = square_at(d->way_x[k], d->way_z[k], s);
            IMPAI_RECT u = cur;
            if (r.x0 < u.x0) u.x0 = r.x0;
            if (r.z0 < u.z0) u.z0 = r.z0;
            if (r.x1 > u.x1) u.x1 = r.x1;
            if (r.z1 > u.z1) u.z1 = r.z1;
            if ((u.x1 - u.x0 + 1) * (u.z1 - u.z0 + 1) <= 2 * (s * s + s * (steps + 1))) {
                cur = u;
                steps++;
                continue;
            }
        }
        if (!way_add(out, &cur)) return 0;
        if (k < 0) return 1;
        cur = square_at(d->way_x[k], d->way_z[k], s);
        steps = 0;
    }
}

/* Class c's search (THE WAY OUT) for the plant at first cell (cx, cz), its path added to d->way: 1 proven, 0
   refused. */
static int way_class(IMPAI_DEC* d, WAY* w, const IMPAI_TINFO* t, int cx, int cz, unsigned c)
{
    const IMPAI_EXITS* L = w->L;
    int k, i, head;
    w->size = L->csize[c];
    w->n = 0;
    if (!d->q->ops->passable(d->q, (int)c, w->box.x0, w->box.z0, w->bw, w->bh, d->way_pass, IMPAI_WAY_PASS_BYTES))
        return 0;
    square_fit(d->way_pass, w->bw, w->bh, w->size - L->cfx[c], w->size - L->cfz[c]);
    memset(d->way_seen, 0, sizeof d->way_seen);
    for (k = 0; k < L->n; k++) {
        const int side = L->edge[k], s = w->size;
        const int last = L->e[k] - s + 1 > L->a[k] ? L->e[k] - s + 1 : L->a[k];
        for (i = L->a[k]; i <= last; i++) {
            const int x = side < 2 ? cx + i : (side == 2 ? cx - s : cx + t->fx);
            const int z = side < 2 ? (side == 0 ? cz - s : cz + t->fz) : cz + i;
            if (way_push(d, w, x, z, -1) < 0) return 0;
        }
    }
    for (head = 0; head < w->n; head++) {
        static const int DX[4] = { 1, -1, 0, 0 }, DZ[4] = { 0, 0, 1, -1 };
        const IMPAI_RECT r = square_at(d->way_x[head], d->way_z[head], w->size);
        if (clear_of(&r, &w->plant) >= WAY_OPEN) return way_rects(d, w, head, &d->way);
        for (k = 0; k < 4; k++)
            if (way_push(d, w, d->way_x[head] + DX[k], d->way_z[head] + DZ[k], head) < 0) return 0;
    }
    return 0;
}

/* the proof's work in cells (THE WORK BUDGET): the raster's box, then each class's window and box */
static int32_t way_cost(const WAY* w)
{
    const int32_t box = (int32_t)w->bw * w->bh;
    int32_t cost = box;
    unsigned c;
    for (c = 0; c < WQ_MCLASSES; c++)
        if (w->L->classes >> c & 1u)
            cost += (int32_t)(w->bw + w->L->cfx[c] + 2) * (w->bh + w->L->cfz[c] + 2) + box;
    return cost;
}

/* THE WAY OUT for a plant of `type` at first cell (cx, cz): 1 proven -- d->way holds it, with no rectangle
   for a plant nothing leaves on the ground -- 0 refused, -1 deferred, the decision's work spent (the caller
   stops) */
static int way_prove(IMPAI_DEC* d, int type, int cx, int cz)
{
    const IMPAI_TINFO* t = &d->in->ti[type];
    IMPAI_EXITS tmp;
    WAY w;
    unsigned c;
    int grow, classes = 0;
    int32_t cost;
    memset(&d->way, 0, sizeof d->way);
    d->way_ok = 0;
    d->way.type = (uint16_t)type;
    d->way.cx = (int16_t)cx;
    d->way.cz = (int16_t)cz;
    w.L = exits_of(d, type, &tmp);
    if (!w.L->read) return 0;
    if (!w.L->n) return d->way_ok = 1;
    if (d->in->st->nway >= IMPAI_WAYS || d->claims_full) return 0;
    for (c = 0; c < WQ_MCLASSES; c++) classes += (int)(w.L->classes >> c & 1u);
    if (classes > WAY_SEARCHES) return 0;
    grow = WAY_OPEN + w.L->size;
    w.plant = impai_rect_at(d, type, cx, cz);
    w.box.x0 = (int16_t)(cx - grow);
    w.box.z0 = (int16_t)(cz - grow);
    w.box.x1 = (int16_t)(w.plant.x1 + grow);
    w.box.z1 = (int16_t)(w.plant.z1 + grow);
    w.bw = w.box.x1 - w.box.x0 + 1;
    w.bh = w.box.z1 - w.box.z0 + 1;
    if (w.bw > IMPAI_WAY_BOX || w.bh > IMPAI_WAY_BOX) return 0;
    if ((cost = way_cost(&w)) > WAY_WORK) return 0;
    if (d->way_work + cost > WAY_WORK) return -1;
    d->way_work += cost;
    way_raster(d, &w);
    for (c = 0; c < WQ_MCLASSES; c++)
        if ((w.L->classes >> c & 1u) && !way_class(d, &w, t, cx, cz, c)) return 0;
    return d->way_ok = 1;
}

/* the plant's way: the one this decision proved for this site, or proven now. 1 ready, 0 refused, -1 deferred */
static int way_ready(IMPAI_DEC* d, int type, int cx, int cz)
{
    if (d->way_ok && d->way.type == type && d->way.cx == cx && d->way.cz == cz) return 1;
    return way_prove(d, type, cx, cz);
}

/* THE WAY REFUSALS. A plant site the way out refuses is avoided by plants of that type for fail_t, in a ring of its
   own (IMPAI_BRAIN.wfail), so however many sites the way refuses no failed site of any other kind leaves the shared
   ring (impai_fail_add). A decision refuses at most site_tests ways, each after a site test, and IMPAI_WAY_FAILS
   holds a whole decision's at every level's site_tests of 32. */
static int way_failed(const IMPAI_DEC* d, int type, int cx, int cz)
{
    const IMPAI_BRAIN* st = d->in->st;
    int i;
    for (i = 0; i < st->nwfail && i < IMPAI_WAY_FAILS; i++)
        if (st->wfail[i].type == type && st->wfail[i].cx == cx && st->wfail[i].cz == cz && st->wfail[i].until > d->now)
            return 1;
    return 0;
}

/* THE PLANT'S FALLBACK. A plant type whose way out is refused WAY_REFUSALS times in a row, or whose base ring is
   exhausted at its widest (ring_grow 8), is passed over by the factory choice for fail_t, which tries the next
   type (impai_econ.c, THE CHOICE). When every type the choice would take is passed over, the plant goes to the
   first ring site the site test passes, without a way out (impai_build_plant_waived): a base with no factory is worse
   than one with a cramped exit, and the opening factory must not wait the game out. TERMINATION: each refusal is a
   site the way refused, avoided for fail_t, so a type is passed over at most WAY_REFUSALS refusals after its first;
   a decision refuses at most site_tests; and with every type passed over the next factory want ends in the waived
   plant's order, or in no site at all. */
#define WAY_REFUSALS 16

/* the type's plant refusals (IMPAI_BRAIN_IN.pref, an entry for each of the world's nti types); NULL outside them */
static IMPAI_PLANTREF* plant_ref(const IMPAI_DEC* d, int type)
{
    return type > 0 && type < (int)d->in->nti ? &d->in->pref[(size_t)type * d->in->pref_stride] : NULL;
}

int impai_plant_off(const IMPAI_DEC* d, int type)
{
    const IMPAI_PLANTREF* r = plant_ref(d, type);
    return r && r->until > d->now;
}

static void plant_pass_over(IMPAI_DEC* d, int type)
{
    IMPAI_PLANTREF* r = plant_ref(d, type);
    if (!r) return;
    r->n = 0;
    r->until = d->now + d->in->plan->fail_t;
}

static void way_refused(IMPAI_DEC* d, int type, int cx, int cz)
{
    IMPAI_BRAIN* st = d->in->st;
    IMPAI_WAYFAIL* f = &st->wfail[st->wfail_next % IMPAI_WAY_FAILS];
    IMPAI_PLANTREF* r;
    f->cx = (int16_t)cx;
    f->cz = (int16_t)cz;
    f->type = (uint16_t)type;
    f->until = d->now + d->in->plan->fail_t;
    st->wfail_next = (uint8_t)((st->wfail_next + 1u) % IMPAI_WAY_FAILS);
    if (st->nwfail < IMPAI_WAY_FAILS) st->nwfail++;
    d->out->failed++;
    if ((r = plant_ref(d, type)) && ++r->n >= WAY_REFUSALS) plant_pass_over(d, type);
}

static void way_claim(IMPAI_DEC* d, const IMPAI_WAYREC* w)
{
    int k;
    for (k = 0; k < w->n && k < IMPAI_WAY_RECTS; k++) {
        IMPAI_RECT r;
        r.x0 = w->r[k][0]; r.z0 = w->r[k][1]; r.x1 = w->r[k][2]; r.z1 = w->r[k][3];
        impai_claim_add(d, w->type, &r);
        if (!d->claims_full) d->claims[d->nclaims - 1].way = 1;
    }
}

/* the ordered plant's way joins the seat's kept ways and is claimed (build_at) */
static void way_keep(IMPAI_DEC* d)
{
    IMPAI_BRAIN* st = d->in->st;
    if (!d->way.n || st->nway >= IMPAI_WAYS) return;
    st->way[st->nway++] = d->way;
    way_claim(d, &d->way);
}

/* After the census: a kept way whose plant stands at its site (a census building of the type with that first
   cell) or is held by a build order there (a census claim of the type with that first cell) is claimed; any
   other is dropped. A census that could not list every claim drops none. */
void impai_ways_keep(IMPAI_DEC* d)
{
    IMPAI_BRAIN* st = d->in->st;
    const int ncensus = d->nclaims, whole = !d->claims_full;
    int k, i, kept = 0;
    for (k = 0; k < st->nway && k < IMPAI_WAYS; k++) {
        const IMPAI_WAYREC w = st->way[k];
        int live = !whole;
        for (i = 0; i < d->nstructs && !live; i++)
            live = d->structs[i].type == w.type && d->structs[i].r.x0 == w.cx && d->structs[i].r.z0 == w.cz;
        for (i = 0; i < ncensus && !live; i++)
            live = d->claims[i].type == w.type && d->claims[i].r.x0 == w.cx && d->claims[i].r.z0 == w.cz;
        if (!live) continue;
        way_claim(d, &w);
        st->way[kept++] = w;
    }
    for (k = kept; k < IMPAI_WAYS; k++) {
        memset(&st->way[k], 0, sizeof st->way[k]);
        st->way[k].type = (uint16_t)WQ_NONE;
    }
    st->nway = (uint8_t)kept;
}

/* ---- the order ---- */

/* THE COMMANDER'S SAFE DISTANCE. The military flees the commander from a contact with a member within
   IMPAI_CMD_SAFE_PX of it that is armed or never identified (impai_mil.c, the commander's safety, step 2), and
   once its window lapses the economy takes it back (impai_brain.c, THE HAND-BACK); a task that close to such an
   enemy would send it back into the flight, a cycle of SAFE_TICKS for as long as the enemy stands. So no economy
   task sends the commander to a point within IMPAI_CMD_SAFE_PX of such an enemy (cmd_foe): a row of the seat's
   column whose owner is neither the seat nor an ally, held now (IMPAI_NOW or IMPAI_PASS) or, for a structure,
   remembered as a ghost, and armed by its threat row (any dps); or a contact never identified held now
   (impai_row_unidentified_now) -- the military picture's enemies (impai_mil.c, picture).
   INVARIANT, a bound beside THE COMMANDER'S REACH: every task order to the commander passes cmd_refused at its
   point -- a build's footprint centre in build_at, the target of an assist or a walk in order_mode, a reclaim's
   feature centre in reclaim_order -- so no task path, present or added, sends it there. order_mode's moves of
   no job, the turn back and the yard's step, leave danger or a yard and are never refused. THE BLAST'S APPROACH
   is refused with it (impai_mil.c, impai_blast_nearer): a walk to the point -- to the commander's builddistance
   of it -- that ends nearer than the commander now to where a foe's commander was seen within IMPAI_BLAST_SEEN_T, and
   within its blast keep; a point within the builddistance walks nowhere. The searches skip such a point themselves through
   the same test -- the ring, the spot (spot_pick), the assist, the reclaim, the sea plant's site --
   so another candidate is taken; and a walk to a remembered feature the draw shows there ends with no
   failure (impai_brain.c, THE RECLAIM'S CHECK).
   COMPLETE BY CONSTRUCTION: for a point within comm_r of the anchor the answer is the decision's list, made at
   its first such test, of the enemies within comm_r + IMPAI_CMD_SAFE_PX + 1 of the anchor -- every enemy within
   IMPAI_CMD_SAFE_PX of such a point lies there by the triangle inequality, the 1 for the two distances' rounding
   down -- at most IMPAI_CMD_FOES of them; with more, every such point is refused, so the list never misses one.
   A point past comm_r -- the snap's slack, the opening's far patch, THE FINISH AT SEA's sea plant -- is answered
   by its own walk of the view's rows. BOUND: one walk of the view's rows for the list a decision, and one for each
   point past comm_r, which only the commander's spots and its sea plant ask: spot_pick's far patches, tested
   after every cheaper filter, at most nspots a call, build_at's at most snaps a decision, and the sea plant's one
   site a commander's call. RESIDUAL: the military weighs the contact by a prediction and
   may stand (THE STAND), which this refusal does not repeat: near a contact the commander would beat, its tasks
   wait for the contact to leave. */

/* 1 when row r is an enemy THE COMMANDER'S SAFE DISTANCE keeps the commander from, the centre of its block into
   (*x, *z), px */
static int cmd_foe(const IMPAI_DEC* d, const IMPAI_SEEN* r, int32_t* x, int32_t* z)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_VIEW* v = in->view;
    if (!r->how || (r->how & IMPAI_OWN)) return 0;
    if (r->type != WQ_NONE && r->type < in->nti && r->owner < WQ_SEATS) {
        const IMPAI_THREAT* th = v->threat && r->type < (unsigned)v->ntypes ? &v->threat[r->type] : NULL;
        if (r->owner == (unsigned)v->seat || ((v->allies >> r->owner) & 1u)) return 0;
        if (!(r->how & (IMPAI_NOW | IMPAI_PASS)) && (in->ti[r->type].flags & IMPAI_TF_MOBILE)) return 0;
        if (!th || !(th->dps_ground || th->dps_air || th->dps_water)) return 0;
    } else if (!impai_row_unidentified_now(r, in->nti))
        return 0;
    *x = r->bx * 32 + 16;
    *z = r->bz * 32 + 16;
    return 1;
}

static void cmd_foes_list(IMPAI_DEC* d, const int32_t apos[3])
{
    const IMPAI_VIEW* v = d->in->view;
    const uint32_t reach = (uint32_t)d->in->plan->comm_r + IMPAI_CMD_SAFE_PX + 1u;
    int32_t x, z;
    int i;
    d->cfoe_listed = 1;
    for (i = 1; v && i < v->nrows; i++) {
        if (!cmd_foe(d, &v->rows[i].seat[v->seat], &x, &z) || impai_dist_px(apos, x, z) > reach) continue;
        if (d->ncfoe >= IMPAI_CMD_FOES) {
            d->cfoe_listed = -1;
            return;
        }
        d->cfoe_x[d->ncfoe] = x;
        d->cfoe_z[d->ncfoe] = z;
        d->ncfoe++;
    }
}

/* 1 when the point (x, z), px, lies within IMPAI_CMD_SAFE_PX of an enemy THE COMMANDER'S SAFE DISTANCE names */
static int cmd_unsafe(IMPAI_DEC* d, int32_t x, int32_t z)
{
    const IMPAI_VIEW* v = d->in->view;
    const int32_t pos[3] = { (int32_t)((uint32_t)x << 16), 0, (int32_t)((uint32_t)z << 16) };
    const int32_t apos[3] = { (int32_t)((uint32_t)(d->in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(d->in->st->az * 16 + 8) << 16) };
    int32_t fx, fz;
    int k;
    if (impai_dist_px(apos, x, z) <= (uint32_t)d->in->plan->comm_r) {
        if (!d->cfoe_listed) cmd_foes_list(d, apos);
        if (d->cfoe_listed < 0) return 1;
        for (k = 0; k < d->ncfoe; k++)
            if (impai_dist_px(pos, d->cfoe_x[k], d->cfoe_z[k]) <= IMPAI_CMD_SAFE_PX) return 1;
        return 0;
    }
    for (k = 1; v && k < v->nrows; k++)
        if (cmd_foe(d, &v->rows[k].seat[v->seat], &fx, &fz) && impai_dist_px(pos, fx, fz) <= IMPAI_CMD_SAFE_PX)
            return 1;
    return 0;
}

/* 1 when b is the commander and THE COMMANDER'S SAFE DISTANCE refuses it the point (x, z), px, or THE BLAST'S
   APPROACH does (impai_mil.c): a walk -- to its builddistance of the point, or with `to_point` (a move) to the point
   itself -- that would take it nearer a foe's commander it could meet within the blast; the decision's blast_hit
   marks the second, for the log's blast_refused= */
static int cmd_refused_to(IMPAI_DEC* d, const IMPAI_UNITREF* b, int32_t x, int32_t z, int to_point)
{
    const IMPAI_BRAIN_IN* in = d->in;
    if (in->ti[b->type].cat != IMPAI_C_COMMANDER) return 0;
    if (cmd_unsafe(d, x, z)) return 1;
    if (!impai_blast_nearer(d->q, in->mil, in->view, in->ti, in->nti, in->persona, d->now, b->type, to_point,
                            b->pos[0] >> 16, b->pos[2] >> 16, x, z))
        return 0;
    d->blast_hit = 1;
    return 1;
}

/* cmd_refused_to for a task worked within the builddistance of (x, z): a site, a frame, a feature */
static int cmd_refused(IMPAI_DEC* d, const IMPAI_UNITREF* b, int32_t x, int32_t z)
{
    return cmd_refused_to(d, b, x, z, 0);
}

/* the footprint centre of `type` at first cell (cx, cz) lies within `reach` px of the anchor */
static int site_within(const IMPAI_DEC* d, int type, int cx, int cz, uint32_t reach)
{
    const IMPAI_BRAIN* st = d->in->st;
    const IMPAI_TINFO* t = &d->in->ti[type];
    const int32_t apos[3] = { (int32_t)((uint32_t)(st->ax * 16 + 8) << 16), 0, (int32_t)((uint32_t)(st->az * 16 + 8) << 16) };
    return impai_dist_px(apos, (2 * cx + t->fx) * 8, (2 * cz + t->fz) * 8) <= reach;
}

/* A build of `type` at first cell (cx, cz), altitude `alt`: the resolver's build mode 14 for the builder at the
   footprint's centre, (2 origin + size) << 19 (tagpu_placegeo.h), then a NEW command without Shift, which
   replaces the builder's queue and keeps its standing orders (engine/orders.md). 1 when given; 0 when the
   resolver refuses (the site is then skipped as a refused site test) or the decision's commands are spent.
   THE COMMANDER'S REACH: a site for the commander whose footprint centre lies farther than `com_reach` from the
   anchor is refused here, so every economy build order to the commander has its site within comm_r of the
   anchor -- the opening's far patch, both plus the snap's slack (impai_build_spot), and THE FINISH AT SEA's sea
   plant (sea_radius) within FAR_PATCH_K x comm_r, the two radii THE LEASH allows it (impai_mil.c). Every build
   order passes here, so no build path, present or added, can send the commander past it. The ring and the sea
   plant's search hold their candidates to the radius they pass here, so it refuses nothing they offer.
   MEASURED in the bar's round 1 (153 recordings, the ImpAI commander's head order every 300 ticks): at the
   second start 2 167 of its 3 723 build orders lay past comm_r, 98 % of them toward the enemy, and 46 of the 51
   commander deaths came past comm_r. THE COMMANDER'S SAFE DISTANCE is refused here as well (cmd_refused). */
static int build_at(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int cx, int cz, int alt, uint32_t com_reach)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_TINFO* t = &in->ti[type];
    const IMPAI_RECT r = impai_rect_at(d, type, cx, cz);
    IMPAI_JOB* j = &in->job[b->slot];
    WQ_CMD c;
    int a;
    if (d->roots >= d->root_cap || impai_far_at(d, b, type, cx, cz)) return 0;
    if (in->ti[b->type].cat == IMPAI_C_COMMANDER && !site_within(d, type, cx, cz, com_reach)) return 0;
    if (cmd_refused(d, b, (2 * cx + t->fx) * 8, (2 * cz + t->fz) * 8)) return 0;
    memset(&c, 0, sizeof c);
    c.pos[0] = (int32_t)((uint32_t)(2 * cx + t->fx) << 19);
    c.pos[1] = (int32_t)((uint32_t)(alt & 255) << 16);
    c.pos[2] = (int32_t)((uint32_t)(2 * cz + t->fz) << 19);
    if ((a = d->q->ops->can(d->q, b->slot, 14, (int)WQ_NONE, c.pos)) <= 0) return 0;
    if (t->cat == IMPAI_C_FACTORY && !d->way_waive && way_ready(d, type, cx, cz) <= 0) return 0;
    c.verb = WQ_CMD_NEW;
    c.action = (uint8_t)a;
    c.unit = b->slot;
    c.target = (uint16_t)WQ_NONE;
    c.type = (uint16_t)type;
    if (impai_give(d, &c) != WQ_OK) return 0;
    d->out->orders++;
    impai_job_end(j);
    j->type = b->type;
    j->build = (uint16_t)type;
    j->target = (uint16_t)WQ_NONE;
    j->cx = (int16_t)cx;
    j->cz = (int16_t)cz;
    j->kind = IMPAI_JOB_BUILD;
    j->since = d->now;
    impai_claim_add(d, type, &r);
    if (t->cat == IMPAI_C_FACTORY) {
        if (!d->way_waive) way_keep(d);
        else {
            d->out->waived = (uint16_t)type;
            d->out->waived_cx = (int16_t)cx;
            d->out->waived_cz = (int16_t)cz;
        }
        IMPAI_PLANTREF* pr = plant_ref(d, type);
        if (pr) pr->n = 0;
    }
    d->pending[t->cat < IMPAI_CATS ? t->cat : IMPAI_C_OTHER]++;
    return 1;
}

/* One order at a target or a point, resolver mode `mode`; the job it becomes. A task (any job) is refused to the
   commander at a point THE COMMANDER'S SAFE DISTANCE or THE BLAST'S APPROACH refuses (cmd_refused_to), a move --
   mode 2, an explore or a walk to a feature believed there -- tested as a walk to the point itself, any other as
   one to its builddistance of the point; a move of no job is not, and on a contested decision
   it may take the root the factories' turn reserves (impai_brain.c, THE FACTORIES' TURN). */
static int order_mode(IMPAI_DEC* d, const IMPAI_UNITREF* b, int mode, int target, const int32_t pos[3], int kind,
                      int cx, int cz)
{
    const IMPAI_BRAIN_IN* in = d->in;
    IMPAI_JOB* j = &in->job[b->slot];
    WQ_CMD c;
    int a;
    if (d->roots >= (kind == IMPAI_JOB_NONE && d->contested ? d->roots_max : d->root_cap)) return 0;
    if (kind != IMPAI_JOB_NONE && cmd_refused_to(d, b, pos[0] >> 16, pos[2] >> 16, mode == 2)) return 0;
    if ((a = d->q->ops->can(d->q, b->slot, mode, target, pos)) <= 0) return 0;
    memset(&c, 0, sizeof c);
    c.verb = WQ_CMD_NEW;
    c.action = (uint8_t)a;
    c.unit = b->slot;
    c.target = (uint16_t)target;
    memcpy(c.pos, pos, sizeof c.pos);
    if (impai_give(d, &c) != WQ_OK) return 0;
    d->out->orders++;
    impai_job_end(j);
    j->type = b->type;
    j->build = (uint16_t)WQ_NONE;
    j->target = (uint16_t)target;
    j->cx = (int16_t)cx;
    j->cz = (int16_t)cz;
    j->kind = (uint8_t)kind;
    j->since = d->now;
    return 1;
}

/* ---- a metal patch ---- */

/* the patch's box grown by a cell on every side */
static IMPAI_RECT patch_box(const SNAP_PATCH* p)
{
    IMPAI_RECT r;
    r.x0 = (int16_t)(p->x0 - 1); r.z0 = (int16_t)(p->z0 - 1);
    r.x1 = (int16_t)(p->x1 + 1); r.z1 = (int16_t)(p->z1 + 1);
    return r;
}

/* THE PATCH ANOTHER PLAYER BUILT ON: a patch whose box a building of another player's touches, as the seat's rows
   hold it -- seen now or remembered as a ghost, at every footprint its block allows (way_blocker) -- is not free,
   as one an own claim touches is not. WHY: the engine places no footprint over another building, so a builder sent
   there stands beside the other's extractor until its job fails, and is sent back once the failure lapses.
   MEASURED 2026-11-14 on retail Lava Mania under the loader and TADR (bar-nota-lavamania-ca-1, at 6b6e332d): 10
   of the 13 sites ImpAI's seat failed were a CORMEX ordered on a patch where OTA AI's ARMMEX stood, the CORCV
   stopping 46-91 px from the site, 3 of them a patch it had failed on 5 400 ticks or more before. A patch the
   seat has not seen built on is still sent to, and fails once. A ghost keeps its patch until the seat sees the
   block again (impai_know.c clears a lit ghost). Judged once a decision, the first time a patch is tested. BOUND:
   the view's rows, each building's rectangle against at most IMPAI_CLUSTERS_HALF patches, the bound of the
   seat's published patches (tagpu_impai.c); a patch past it is not tested. */
static void foreign_judge(IMPAI_DEC* d)
{
    const IMPAI_VIEW* v = d->in->view;
    const unsigned np = d->in->nspots < IMPAI_CLUSTERS_HALF ? d->in->nspots : IMPAI_CLUSTERS_HALF;
    unsigned k;
    int i;
    d->foreign_built = 1;
    if (!v || !v->rows || v->seat < 0 || v->seat >= WQ_SEATS) return;
    for (i = 1; i < v->nrows; i++) {
        IMPAI_RECT r;
        if (!way_blocker(d, v->seat, &v->rows[i].seat[v->seat], &r)) continue;
        for (k = 0; k < np; k++) {
            const IMPAI_RECT b = patch_box(&d->in->spots[k]);
            if (rects_overlap(&r, &b)) d->foreign[k >> 3] |= (uint8_t)(1u << (k & 7));
        }
    }
}

/* a patch no claim, no failure and no building of another player's touches */
static int patch_free(IMPAI_DEC* d, const SNAP_PATCH* p)
{
    const IMPAI_RECT r = patch_box(p);
    const size_t k = (size_t)(p - d->in->spots);
    if (impai_claimed(d, &r) || impai_failed_in(d, &r)) return 0;
    if (!d->foreign_built) foreign_judge(d);
    return !(k < IMPAI_CLUSTERS_HALF && (d->foreign[k >> 3] >> (k & 7) & 1u));
}

/* The builder's class's label at its own block, which a reach test of it compares: -1 for no test -- a VTOL
   builder, one with no class of the world's, a level with no labels for it -- 0 off the grid or on a block of
   no label, which reaches nothing. */
static int builder_label(const IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_TINFO* t = &d->in->ti[b->type];
    if (!d->in->reach || (t->flags & IMPAI_TF_VTOL)) return -1;
    return impai_reach_at(d->in->reach, t->mclass, b->pos[0] >> 21, b->pos[2] >> 21);
}

/* The reach labels of the builder's class at its block and at block (px, pz) agree, both nonzero; no test as
   builder_label says. */
static int reachable_block(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int px, int pz)
{
    const int l = builder_label(d, b);
    if (l < 0) return 1;
    return l && impai_reach_at(d->in->reach, d->in->ti[b->type].mclass, px, pz) == l;
}

/* the patch's anchor block is reachable on the builder's class */
static int reachable(const IMPAI_DEC* d, const IMPAI_UNITREF* b, const SNAP_PATCH* p)
{
    return reachable_block(d, b, p->ax >> 1, p->az >> 1);
}

/* the seat's maps, built the first time a decision needs them: 1 built, 0 none to build */
static int maps_ready(IMPAI_DEC* d)
{
    const IMPAI_VIEW* v = d->in->view;
    if (!d->maps_built)
        d->maps_built = v && v->threat && impai_maps_build(v, &d->maps, d->in->mil ? d->in->mil->radar_as : WQ_NONE)
                            ? 1 : -1;
    return d->maps_built > 0;
}

/* ground threat on 256-px cell (gx, gz); none off the grid */
static int cell_threatened(IMPAI_DEC* d, int64_t gx, int64_t gz)
{
    if (!maps_ready(d)) return 0;
    return gx >= 0 && gz >= 0 && gx < d->maps.gw && gz < d->maps.gh &&
           d->maps.threat[IMPAI_THREAT_GROUND][gz * d->maps.gw + gx] > 0;
}

/* ground threat at the 256-px cell of a point, from the seat's maps */
static int threatened(IMPAI_DEC* d, int x_px, int z_px)
{
    return cell_threatened(d, impai_cell_of_px(x_px), impai_cell_of_px(z_px));
}

/* THE ROUTE for a builder: 1 when the straight segment from builder b (its pos >> 16) to the point crosses known
   ground threat, by the sampler (impai_maps.c, impai_route_threat, where its samples, its bound and its
   residuals are). */
static int route_threatened(IMPAI_DEC* d, const IMPAI_UNITREF* b, int x_px, int z_px)
{
    return maps_ready(d) &&
           impai_route_threat(&d->maps, b->pos[0] >> 16, b->pos[2] >> 16, x_px, z_px);
}

int impai_threatened(IMPAI_DEC* d, int x_px, int z_px) { return threatened(d, x_px, z_px); }

int impai_walk_threatened(IMPAI_DEC* d, const IMPAI_UNITREF* b, int x_px, int z_px)
{
    return threatened(d, x_px, z_px) || route_threatened(d, b, x_px, z_px);
}

/* THE TURN BACK of builder b: a move (resolver mode 2) to the anchor's cell centre at the ground's height, as the
   explorer's order takes its point; while the anchor's own 256-px cell is threatened, or its block is not
   reachable on b's class (reachable_block), to the centre of the unthreatened cell nearest b whose centre block
   is -- in ring order around b's cell (impai_ring_at), the first on the grid -- so a builder in a raid on the base
   is not sent into it, nor a constructor to a point in the sea or on a cliff it cannot stand on. The order ends
   b's job (order_mode), and the builder is the ladder's again at the next decision whether or not the move ever
   ends (impai_brain.c, THE HAND-BACK). 0 when no order was given: the decision's commands spent, the resolver
   refusing, or no cell both clear and reachable. BOUND: one ring walk of at most (2 IMPAI_GRID_MAX - 1)^2 cells,
   of which only those on the grid are read, IMPAI_GRID_MAX^2 at most, each one map read and one label read.
   RESIDUAL: a threat between b and the point is walked through, as the commander's flight's is (impai_mil.c,
   flee); and a cell is held reachable by its centre block alone. */
int impai_turn_back(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_BRAIN* st = d->in->st;
    int64_t tx = st->ax * 16 + 8, tz = st->az * 16 + 8;
    int32_t pos[3];
    if (threatened(d, (int)tx, (int)tz) || !reachable_block(d, b, (int)(tx >> 5), (int)(tz >> 5))) {
        const int64_t bx = impai_cell_of_px(b->pos[0] >> 16), bz = impai_cell_of_px(b->pos[2] >> 16);
        int r, i, dx, dz, found = 0;
        if (!maps_ready(d)) return 0;
        for (r = 1; r < IMPAI_GRID_MAX && !found; r++)
            for (i = 0; !found && impai_ring_at(r, i, &dx, &dz); i++) {
                const int64_t gx = bx + dx, gz = bz + dz;
                const int64_t cx = gx * IMPAI_CELL_PX + IMPAI_CELL_PX / 2, cz = gz * IMPAI_CELL_PX + IMPAI_CELL_PX / 2;
                if (gx < 0 || gz < 0 || gx >= d->maps.gw || gz >= d->maps.gh || cell_threatened(d, gx, gz) ||
                    !reachable_block(d, b, (int)(cx >> 5), (int)(cz >> 5)))
                    continue;
                tx = cx;
                tz = cz;
                found = 1;
            }
        if (!found) return 0;
    }
    pos[0] = (int32_t)((uint32_t)tx << 16);
    pos[1] = 0;
    pos[2] = (int32_t)((uint32_t)tz << 16);
    pos[1] = (int32_t)((uint32_t)(d->q->ops->ground(d->q, pos) & 0x7FFF) << 16);
    return order_mode(d, b, 2, (int)WQ_NONE, pos, IMPAI_JOB_NONE, (int)(tx >> 4), (int)(tz >> 4));
}

/* a unit of `type`'s half-size, px: its larger side, cells, x 8 */
static int64_t half_px(const IMPAI_DEC* d, int type)
{
    const IMPAI_TINFO* t = &d->in->ti[type];
    return (int64_t)(t->fx > t->fz ? t->fx : t->fz) * 8;
}

/* px (x, z) inside cell rectangle r grown by hb px */
static int rect_holds(const IMPAI_RECT* r, int64_t hb, int64_t x, int64_t z)
{
    return x >= (int64_t)r->x0 * 16 - hb && x < ((int64_t)r->x1 + 1) * 16 + hb &&
           z >= (int64_t)r->z0 * 16 - hb && z < ((int64_t)r->z1 + 1) * 16 + hb;
}

int impai_yard_holds(const IMPAI_DEC* d, const IMPAI_YARDREG* g, int type, int64_t x, int64_t z)
{
    const int64_t hb = half_px(d, type);
    int k;
    for (k = 0; k < g->n && k < 1 + IMPAI_WAY_RECTS; k++)
        if (rect_holds(&g->r[k], hb, x, z)) return 1;
    return 0;
}

/* 1 when a unit of `type` at px (x, z) touches any factory of the census's buildings or any of the seat's kept ways
   out, the recorded rectangles held to IMPAI_WAYS x IMPAI_WAY_RECTS */
static int yards_hold(const IMPAI_DEC* d, int type, int64_t x, int64_t z)
{
    const IMPAI_BRAIN* st = d->in->st;
    const int64_t hb = half_px(d, type);
    int k, i;
    for (k = 0; k < d->nstructs; k++)
        if (d->structs[k].cat == IMPAI_C_FACTORY && rect_holds(&d->structs[k].r, hb, x, z)) return 1;
    for (k = 0; k < st->nway && k < IMPAI_WAYS; k++)
        for (i = 0; i < st->way[k].n && i < IMPAI_WAY_RECTS; i++) {
            IMPAI_RECT r;
            r.x0 = st->way[k].r[i][0]; r.z0 = st->way[k].r[i][1];
            r.x1 = st->way[k].r[i][2]; r.z1 = st->way[k].r[i][3];
            if (rect_holds(&r, hb, x, z)) return 1;
        }
    return 0;
}

/* THE YARD's move (impai_brain.c, THE YARD): builder b, when it touches region g -- a waiting factory's footprint,
   g->r[0], and its kept way out -- is moved off it. The candidates, about the footprint: the point on the line from
   its centre through b at its half-diagonal plus b's half-size plus 32 px, then that distance along +z, -z, +x, -x,
   each held to the map. The first candidate b at it would not touch g is taken unless a later one touches no
   factory and no kept way at all (yards_hold), which is taken instead, so a builder is not moved onto another
   plant's yard or way when the footprint's sides offer a clear one. Its job ends with the move (order_mode, job
   NONE). 1 when moved; 0 with every candidate on g. Positions are recorded data: every product is of int64 terms
   of 16-bit cells and pixels. BOUND: five candidates, each against g's at most 1 + IMPAI_WAY_RECTS rectangles and
   the census's buildings and IMPAI_WAYS x IMPAI_WAY_RECTS kept rectangles. */
int impai_yard_leave(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_YARDREG* g)
{
    static const int8_t AXES[4][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 } };
    const IMPAI_RECT* r = &g->r[0];
    const int64_t hb = half_px(d, b->type);
    const int64_t cx = ((int64_t)r->x0 + r->x1 + 1) * 8, cz = ((int64_t)r->z0 + r->z1 + 1) * 8;
    const int64_t wx = ((int64_t)r->x1 - r->x0 + 1) * 16, wz = ((int64_t)r->z1 - r->z0 + 1) * 16;
    const int64_t reach = (int64_t)impai_isqrt((uint64_t)(wx * wx + wz * wz)) / 2 + hb + 32;
    const int64_t bx = b->pos[0] >> 16, bz = b->pos[2] >> 16;
    const int64_t mw = (int64_t)d->m.w * 16 - 1, mh = (int64_t)d->m.h * 16 - 1;
    int64_t tx = -1, tz = -1;
    int32_t pos[3];
    int k;
    if (g->n < 1 || mw < 0 || mh < 0 || !impai_yard_holds(d, g, b->type, bx, bz)) return 0;
    for (k = -1; k < 4; k++) {
        const int64_t ux = k < 0 ? bx - cx : AXES[k][0], uz = k < 0 ? bz - cz : AXES[k][1];
        const int64_t n = (int64_t)impai_isqrt((uint64_t)(ux * ux + uz * uz));
        int64_t px, pz;
        if (n <= 0) continue;
        px = cx + ux * reach / n;
        pz = cz + uz * reach / n;
        px = px < 0 ? 0 : px > mw ? mw : px;
        pz = pz < 0 ? 0 : pz > mh ? mh : pz;
        if (impai_yard_holds(d, g, b->type, px, pz)) continue;
        if (tx < 0) {
            tx = px;
            tz = pz;
        }
        if (!yards_hold(d, b->type, px, pz)) {
            tx = px;
            tz = pz;
            break;
        }
    }
    if (tx < 0) return 0;
    pos[0] = (int32_t)((uint32_t)tx << 16);
    pos[1] = 0;
    pos[2] = (int32_t)((uint32_t)tz << 16);
    pos[1] = (int32_t)((uint32_t)(d->q->ops->ground(d->q, pos) & 0x7FFF) << 16);
    return order_mode(d, b, 2, (int)WQ_NONE, pos, IMPAI_JOB_NONE, (int)(tx >> 4), (int)(tz >> 4));
}

/* THE EXTRACTOR FOR THE PATCH. The type is chosen for the ground at the patch, never before it.
   - The water-only type goes where the patch centre lies at least its minwaterdepth under the sea; the land type
     goes everywhere else, a wet patch included when the menu has no water type, where the site test's
     maxwaterdepth decides.
   - Why: the site test 0x47D2E0 refuses a footprint whose highest yard cell lies above sea - minwaterdepth
     (0x47D7D7..0x47D7F0), and one whose lowest lies below sea - maxwaterdepth (0x47D7BA..0x47D7D5), the sea the
     byte main+0x1427F (DISASSEMBLED, engine/orders.md). A type chosen before its patch was refused at every patch
     of the other kind: MEASURED 2026-11-14 on Total Mayhem and TA Twilight, whose water extractors extract more
     than the land ones, at 118-136 refusals a level.
   - The centre is the snap's seed, so a footprint holding it can take the water type only where this compare
     passes. This is a choice, not the safety argument: the site test decides, and a refused patch goes to the fail
     ring for fail_t as any other.
   - BOUND: one ground read a call, and none with no water type.
   - RESIDUAL: a shore patch whose centre is on one side of the line and whose best footprint is on the other is
     refused once and waits fail_t. */
int impai_mex_at(const IMPAI_DEC* d, const IMPAI_MEX* mex, int px, int pz)
{
    const int32_t pos[3] = { (int32_t)((uint32_t)px << 16), 0, (int32_t)((uint32_t)pz << 16) };
    if (mex->wet && (int)d->m.sea - d->q->ops->ground(d->q, pos) >= mex->wet_min) return mex->wet;
    return mex->dry;
}

/* THE SPOT for builder b: of the seat's free patches with an extractor of `mex` for their ground (THE EXTRACTOR
   FOR THE PATCH; any free patch with mex NULL) -- for the commander within comm_r of the anchor, for a constructor
   on THE HELD GROUND (below), reachable on b's
   class, under no ground threat and with none known on the straight way there (THE ROUTE), and while b's far
   record is live (impai_brain.c, THE FAR RECORD) not holding its recorded spot and within its reach of the
   patch's extractor at the patch's centre -- the one with the best cells x 4096 / (4096 + d), d the integer
   distance in px from b to the patch's centre, compared exactly in integers; ties to the lower (az, ax). NULL when
   b has none; for the commander, none THE COMMANDER'S SAFE DISTANCE refuses at the patch's centre. The snap's
   site is then held to the same reach (impai_build_spot). The route and that distance are tested last, only
   for a patch that would become the best; the test does not depend on the best so far, so the answer is still
   the best route-clear patch; every filter before it is a `continue` of its own patch alone, so their order
   changes no answer either. BOUND: nspots routes and nspots ground reads (impai_mex_at) a call, THE ROUTE's
   samples each.
   THE OPENING'S FAR PATCH: before the seat has ever had a factory (IMPAI_BRAIN.opened: none counted, frames
   included, this level) and while none is ordered and it counts fewer than open_mex extractors, a commander with no
   patch within comm_r takes the NEAREST free patch past it -- by the integer distance from the commander, ties to
   the lower (az, ax) -- within FAR_PATCH_K x comm_r of the anchor and within job_timeout x its def's maxvelocity
   of where it stands, the most straight line its build job can walk before job_checks gives it up. Without it a
   start with no metal within comm_r opens on the commander's own income alone: MEASURED 2026-11-13 on Trout
   Farm, ARM's start at cell (227, 432), whose nearest of the 66 published patches lies 1353 px from the anchor;
   the commander stood idle until open_t, then the factory and its constructors starved at 1.25 metal a pass
   until about GameTime 13 800, and the seat sent no attack before 27 000. BOUND: the walk is at most
   FAR_PATCH_K x comm_r from the anchor (2048 px at every level's comm_r of 1024, past Trout Farm's 1353) and
   inside the build job's timeout; it orders open_mex extractors at most, and none once a factory has been
   counted, so a seat that lost its factories keeps its commander home. The nearest, not the richest: past comm_r
   every patch is a walk away from the base, and the shortest walk is the soonest income. A commander with no
   maxvelocity takes no far patch. FAR_PATCH_K: impai.h. */
/* THE HELD GROUND: a constructor claims a patch, and a feature to reclaim (THE RECLAIM), only on a 256-px cell the
   seat holds now -- one its strength reaches before any known enemy strength does. A judging decision (the first
   that asks, patch_held) walks the maps' grid
   twice in 8-neighbour steps, each from one side's sources:
   - THE SEAT'S STRENGTH: the cell of each of its rows, and each ally's, of an army type (held_kind: armed
     against the ground and moving on it; an aircraft holds no ground) -- held now or remembered --, and the base
     anchor's cell. A tower of its own is 0 steps at its own cell and is not walked from: it holds what it stands
     on, not the ground beyond;
   - THE ENEMY'S: the cell of every identified enemy row of an army type, seen now or a ghost, whoever's -- so in
     a free-for-all every enemy counts --, each radar contact never identified while the military counts such a
     contact as an army type (IMPAI_MIL.radar_as, as the builders' threat map counts it), and THE ENEMIES' BASES:
     the cell of every identified enemy structure that is not a tower, seen now or a ghost, and while an enemy
     owner the rows identify has none of them -- or the rows identify no enemy at all -- every start position but
     the seat's own (the one nearest its anchor), the map's starts being public. An enemy tower contests only its
     reach, where the spot's threat test already refuses every patch (THE ROUTE), so it is no source here.
     WHY EACH BASE AND NOT THE MILITARY'S ESTIMATE: the estimate is one value-weighted centre of every enemy's
     structures, and in a free-for-all the centre of two enemies on opposite sides falls by the seat's own base,
     which then held its anchor's cell and its army's and claimed no patch.
   THE HYSTERESIS, by construction: a cell not held becomes held when 4 own <= 3 foe, its steps from the seat's
   strength at most three quarters of those from the enemy's; a held cell is let go only when own > foe. Between
   the two it keeps what the last judging decision left it (IMPAI_BRAIN.held), so a front that wavers by a cell
   flips no patch. The held ground so grows behind a won push -- the army standing where no known enemy is nearer
   -- and shrinks after a lost one. Before any enemy is identified the other starts stand for the enemy, so a cell
   is held up to three sevenths of the way to the nearest; with no other start and no enemy known every cell is
   held; with no maps every patch is. The commander keeps comm_r and its opening's far patch.
   WHY, MEASURED 2026-11-14 on the owner's Comet Catcher game (the three ImpAI seats' extractors, by distance from
   each seat's first three): the median life of those 2 048-2 559 px out was 4 470 ticks, against 11 010-17 250 in
   every other 512-px band out to 4 095; seats 1 and 3 started 4 340 px apart, so that band is the ground half way
   between them, contested while the armies met there. WHY NOT A LINE BETWEEN THE BASES: the anchor and the
   estimate stay put, so such a line refuses for the rest of the game the ground a push wins; on the bar's Trout
   Farm games the seats ended with 7-9 extractors under it against 16-24 with no limit (MEASURED 2026-11-14).
   BOUND, a judging decision: three passes over the view's rows, at most HELD_STARTS start positions, and two
   walks of at most IMPAI_GRID_MAX^2 cells, each queued once (held_source queues a cell only while it is not yet a
   source) and read with its 8 neighbours; steps held to HELD_FAR. RESIDUAL: a step is a straight cell,
   blind to water and cliffs; a ghost holds its ground for the enemy until the seat sees it gone. */
#define HELD_FAR 255
static void held_walk(const IMPAI_DEC* d, uint8_t* dist, int n0)
{
    const int gw = d->maps.gw, gh = d->maps.gh;
    uint16_t* q = d->held_q;
    int head = 0, tail = n0;
    while (head < tail) {
        const int c = q[head++], gx = c % gw, gz = c / gw;
        int dx, dz;
        if (dist[c] >= HELD_FAR - 1) continue;
        for (dz = -1; dz <= 1; dz++)
            for (dx = -1; dx <= 1; dx++) {
                const int x = gx + dx, z = gz + dz;
                if (x < 0 || z < 0 || x >= gw || z >= gh || dist[z * gw + x] != HELD_FAR) continue;
                dist[z * gw + x] = (uint8_t)(dist[c] + 1);
                q[tail++] = (uint16_t)(z * gw + x);
            }
    }
}

/* cell (gx, gz) as a source of dist, queued at q[*n]; off the grid, or a source already, nothing */
static void held_source(const IMPAI_DEC* d, uint8_t* dist, int64_t gx, int64_t gz, int* n)
{
    if (gx < 0 || gz < 0 || gx >= d->maps.gw || gz >= d->maps.gh || !dist[gz * d->maps.gw + gx]) return;
    dist[gz * d->maps.gw + gx] = 0;
    d->held_q[(*n)++] = (uint16_t)(gz * d->maps.gw + gx);
}

/* a type's part in the held ground: HELD_ARMY armed against the ground (IMPAI_THREAT.hit_ground, the threat map's
   own reach) and moving on it, HELD_TOWER armed and not moving, 0 neither -- unarmed, or an aircraft, which holds
   no ground -- and every type past the view's or the type table's */
enum { HELD_TOWER = 1, HELD_ARMY };
static int held_kind(const IMPAI_DEC* d, unsigned type)
{
    unsigned f;
    if (type >= (unsigned)d->in->view->ntypes || type >= d->in->nti || !d->in->view->threat[type].hit_ground) return 0;
    f = d->in->ti[type].flags;
    return f & IMPAI_TF_VTOL ? 0 : f & IMPAI_TF_MOBILE ? HELD_ARMY : HELD_TOWER;
}

/* the cells of one side's rows of `kind`, own (the seat and its allies) or the enemy's, into dist: an army's
   queued to walk from, a tower's set to 0 unqueued; the enemy's army with each contact never identified when the
   military counts contacts as an army type. The sources queued. */
static int held_sources(const IMPAI_DEC* d, uint8_t* dist, int own, int kind)
{
    const IMPAI_VIEW* v = d->in->view;
    const IMPAI_MIL* mil = d->in->mil;
    const int radar = !own && kind == HELD_ARMY && mil && held_kind(d, mil->radar_as) == HELD_ARMY;
    int i, n = 0;
    for (i = 1; i < v->nrows; i++) {
        const IMPAI_SEEN* r = &v->rows[i].seat[v->seat];
        const int64_t gx = (int64_t)r->bx >> 3, gz = (int64_t)r->bz >> 3;
        int mine;
        if (!r->how) continue;
        if (impai_row_unidentified_now(r, d->in->nti)) {
            if (radar) held_source(d, dist, gx, gz, &n);
            continue;
        }
        if (r->owner >= WQ_SEATS || held_kind(d, r->type) != kind) continue;
        mine = r->owner == v->seat || (v->allies >> r->owner & 1u);
        if (mine != own) continue;
        if (kind == HELD_ARMY) held_source(d, dist, gx, gz, &n);
        else if (gx < d->maps.gw && gz < d->maps.gh) dist[gz * d->maps.gw + gx] = 0;
    }
    return n;
}

#define HELD_STARTS 16

/* THE ENEMIES' BASES into the enemy's dist, queued after its n sources; the sources queued */
static int held_bases(const IMPAI_DEC* d, uint8_t* dist, int n)
{
    const IMPAI_VIEW* v = d->in->view;
    const IMPAI_BRAIN* st = d->in->st;
    WQ_START s[HELD_STARTS];
    uint32_t seen = 0, based = 0;
    int i, k, ns, own = -1;
    for (i = 1; i < v->nrows; i++) {
        const IMPAI_SEEN* r = &v->rows[i].seat[v->seat];
        if (!r->how || impai_row_unidentified_now(r, d->in->nti) || r->owner >= WQ_SEATS || r->owner == v->seat ||
            (v->allies >> r->owner & 1u))
            continue;
        seen |= 1u << r->owner;
        if (r->type >= d->in->nti || (d->in->ti[r->type].flags & IMPAI_TF_MOBILE) || held_kind(d, r->type)) continue;
        based |= 1u << r->owner;
        held_source(d, dist, (int64_t)r->bx >> 3, (int64_t)r->bz >> 3, &n);
    }
    if ((seen && !(seen & ~based)) || st->ax == IMPAI_NO_ANCHOR ||
        (ns = d->q->ops->starts(d->q, s, HELD_STARTS)) <= 1)
        return n;
    for (k = 0; k < ns; k++) {
        const int64_t dx = (int64_t)s[k].x - (st->ax * 16 + 8), dz = (int64_t)s[k].z - (st->az * 16 + 8);
        const int64_t ox = own >= 0 ? (int64_t)s[own].x - (st->ax * 16 + 8) : 0;
        const int64_t oz = own >= 0 ? (int64_t)s[own].z - (st->az * 16 + 8) : 0;
        if (own < 0 || dx * dx + dz * dz < ox * ox + oz * oz) own = k;
    }
    for (k = 0; k < ns; k++)
        if (k != own) held_source(d, dist, impai_cell_of_px(s[k].x), impai_cell_of_px(s[k].z), &n);
    return n;
}

static void held_judge(IMPAI_DEC* d)
{
    IMPAI_BRAIN* st = d->in->st;
    const int gw = d->maps.gw, gh = d->maps.gh;
    int n, gx, gz;
    memset(d->held_own, HELD_FAR, (size_t)gw * gh);
    n = held_sources(d, d->held_own, 1, HELD_ARMY);
    if (st->ax != IMPAI_NO_ANCHOR)
        held_source(d, d->held_own, impai_cell_of_px((int64_t)st->ax * 16 + 8),
                    impai_cell_of_px((int64_t)st->az * 16 + 8), &n);
    held_walk(d, d->held_own, n);
    held_sources(d, d->held_own, 1, HELD_TOWER);
    memset(d->held_foe, HELD_FAR, (size_t)gw * gh);
    n = held_sources(d, d->held_foe, 0, HELD_ARMY);
    n = held_bases(d, d->held_foe, n);
    held_walk(d, d->held_foe, n);
    for (gz = 0; gz < gh; gz++)
        for (gx = 0; gx < gw; gx++) {
            const int own = d->held_own[gz * gw + gx], foe = d->held_foe[gz * gw + gx];
            const int k = gz * IMPAI_GRID_MAX + gx, was = st->held[k >> 3] >> (k & 7) & 1;
            const int now = was ? !(own > foe) : 4 * own <= 3 * foe;
            st->held[k >> 3] = (uint8_t)((st->held[k >> 3] & ~(1u << (k & 7))) | (unsigned)now << (k & 7));
        }
}

static int patch_held(IMPAI_DEC* d, int px, int pz)
{
    const int64_t gx = impai_cell_of_px(px), gz = impai_cell_of_px(pz);
    int k;
    if (!d->held_built) {
        d->held_built = maps_ready(d) && d->in->view->nrows > 0 ? 1 : -1;
        if (d->held_built > 0) held_judge(d);
    }
    if (d->held_built < 0 || gx < 0 || gz < 0 || gx >= d->maps.gw || gz >= d->maps.gh) return 1;
    k = (int)(gz * IMPAI_GRID_MAX + gx);
    return d->in->st->held[k >> 3] >> (k & 7) & 1;
}

/* range: px from the anchor, -1 none; walk: the far patch's px from b, -1 for the best patch within range; held:
   THE HELD GROUND only; the best patch's extractor into *type when type is not NULL (-1 with mex NULL, 0 with no
   patch) */
static const SNAP_PATCH* spot_pick(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_MEX* mex, int range, int walk,
                                   int held, int* type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int32_t apos[3] = { (int32_t)((uint32_t)(in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(in->st->az * 16 + 8) << 16) };
    const int commander = in->ti[b->type].cat == IMPAI_C_COMMANDER;
    const SNAP_PATCH* best = NULL;
    uint32_t best_d = 0;
    int best_t = 0;
    unsigned i;
    for (i = 0; i < in->nspots; i++) {
        const SNAP_PATCH* p = &in->spots[i];
        const int px = (p->x0 + p->x1 + 1) * 8, pz = (p->z0 + p->z1 + 1) * 8;
        uint32_t dist;
        int t;
        if (!p->cells || !patch_free(d, p)) continue;
        if (range >= 0 && impai_dist_px(apos, px, pz) > (uint32_t)range) continue;
        if (held && !patch_held(d, px, pz)) continue;
        if (!(t = mex ? impai_mex_at(d, mex, px, pz) : -1)) continue;
        {
            const IMPAI_RECT box = patch_box(p);
            if (impai_far_in(d, b, &box) || impai_far_px(d, b, t, px, pz)) continue;
        }
        if (!reachable(d, b, p) || threatened(d, px, pz)) continue;
        dist = impai_dist_px(b->pos, px, pz);
        if (walk >= 0) {
            if (dist > (uint32_t)walk) continue;
            if (best && (dist > best_d || (dist == best_d && (p->az > best->az ||
                                                              (p->az == best->az && p->ax >= best->ax)))))
                continue;
        } else if (best) {
            const uint64_t mine = (uint64_t)p->cells * (4096u + (uint64_t)best_d);
            const uint64_t theirs = (uint64_t)best->cells * (4096u + (uint64_t)dist);
            if (mine < theirs || (mine == theirs && (p->az > best->az || (p->az == best->az && p->ax >= best->ax))))
                continue;
        }
        if (route_threatened(d, b, px, pz) || (commander && cmd_refused(d, b, px, pz))) continue;
        best = p;
        best_d = dist;
        best_t = t;
    }
    if (type) *type = best_t;
    return best;
}

const SNAP_PATCH* impai_spot_pick(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_MEX* mex, int* type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const SNAP_PATCH* best;
    WQ_DEF def;
    int64_t walk;
    if (type) *type = 0;
    if (in->ti[b->type].cat != IMPAI_C_COMMANDER) return spot_pick(d, b, mex, -1, -1, 1, type);
    if ((best = spot_pick(d, b, mex, in->plan->comm_r, -1, 0, type))) return best;
    if (in->st->opened || d->count[IMPAI_C_FACTORY] + d->pending[IMPAI_C_FACTORY] ||
        d->count[IMPAI_C_MEX] + d->pending[IMPAI_C_MEX] >= in->plan->open_mex || !d->q->ops->def(d->q, b->type, &def) ||
        def.speed <= 0)
        return NULL;
    walk = ((int64_t)def.speed * in->plan->job_timeout) >> 16;
    return spot_pick(d, b, mex, FAR_PATCH_K * in->plan->comm_r, walk < INT32_MAX ? (int)walk : INT32_MAX, 0, type);
}

/* The extractor on b's spot, the patch's own (THE EXTRACTOR FOR THE PATCH): the snap places it (tasim_snap_site, the build cursor's scorer: the footprint over
   the most explored metal within the patch's reach) and the site test decides. A patch the snap or the test
   refuses fails for fail_t. The commander's reach (build_at) is its patch's -- comm_r, or FAR_PATCH_K x comm_r
   for the opening's far patch, the only patch past comm_r spot_pick gives it -- plus the snap's slack: the
   snap's candidates are centre cells within `radius` cells of the seed's on each axis (tasim_snap.c, THE SNAP
   SCORER) and a footprint's centre lies within a cell of its centre cell's, so the site's centre is at most
   (16 radius + 16) x sqrt 2 px off the patch's centre. */
int impai_build_spot(IMPAI_DEC* d, const IMPAI_UNITREF* b, const IMPAI_MEX* mex)
{
    const IMPAI_BRAIN_IN* in = d->in;
    int type;
    const SNAP_PATCH* best = impai_spot_pick(d, b, mex, &type);
    const int32_t apos[3] = { (int32_t)((uint32_t)(in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(in->st->az * 16 + 8) << 16) };
    SNAP_CHOICE c;
    int32_t seed[3];
    int radius, alt = 0, w, h;
    uint32_t diag, com_reach;
    uint64_t slack;
    if (!best || type <= 0 || d->snaps >= in->plan->snaps) return 0;
    d->snaps++;
    w = best->x1 - best->x0 + 1;
    h = best->z1 - best->z0 + 1;
    diag = impai_isqrt((uint64_t)(w * w + h * h) * 256u);         /* px */
    radius = (int)((32u + diag / 2u + 15u) / 16u);
    if (radius > SNAP_RADIUS_MAX) radius = SNAP_RADIUS_MAX;
    seed[0] = (int32_t)((uint32_t)((best->x0 + best->x1 + 1) * 8) << 16);
    seed[1] = 0;
    seed[2] = (int32_t)((uint32_t)((best->z0 + best->z1 + 1) * 8) << 16);
    slack = (uint64_t)radius * 16u + 16u;
    com_reach = (impai_dist_px(apos, seed[0] >> 16, seed[2] >> 16) > in->plan->comm_r ? FAR_PATCH_K : 1u) *
                    in->plan->comm_r + impai_isqrt(2u * slack * slack) + 1u;
    memset(&c, 0, sizeof c);
    if (!tasim_snap_site(d->q, NULL, type, seed, seed, radius, 0, 0, &c, in->snap)) {
        impai_fail_add(d, best->ax, best->az);
        return 0;
    }
    if (impai_far_at(d, b, type, c.x, c.z)) return 0;
    if (d->sites >= in->plan->site_tests) return 0;
    d->sites++;
    if (!d->q->ops->site_ok(d->q, type, c.x, c.z, 0, &alt)) {
        impai_fail_add(d, c.x, c.z);
        return 0;
    }
    return build_at(d, b, type, c.x, c.z, alt, com_reach);
}

/* ---- the base ring ---- */

/* THE RING'S ORDER: the sites nearest the order's ORIGIN first, by the squared distance in px from a site's
   footprint centre to it; ties to the site farther from the military's estimate of the enemy base (none without
   one), then to the lower (cz, cx). The origin is the caller's (ring_build): the anchor for every builder but the
   commander, whose origin is where it stands, and for a ground defence its point whoever builds it (THE GROUND
   DEFENCES, impai_build_def), so the tower stands on the ring's side toward the pressing enemy. place_ring lists its sites right side first, top to bottom, then the bottom, the left and the
   top; walked in that order, every builder takes the east side first, toward the enemy at the second start of
   every map of the bar. MEASURED in the bar's cmdr run (65 games, place_ring's order): at GameTime 15 000 the
   structures' mean projection toward the enemy was +577 px at the second start and -146 px at the first (medians
   over 60 and 46 ImpAI seat-games), and 1 961 constructors died at the second start against 551 at the first.
   THE ANCHOR keeps the base compact for the builders nothing else holds near it: the box grows on its side nearest
   the anchor, whichever way the enemy lies. THE COMMANDER is held near it already, every site it takes within
   comm_r of the anchor (THE COMMANDER'S REACH, build_at), and it is the opening's only builder, so within that
   reach its walk decides: ordered from the anchor, the commander back from its far extractors walked to the
   anchor's side of the box for the first plant and the solar after it, and the economy game's bar
   (impai_brain_test.c, impai-eco's numbers) came at GameTime 4 218 against 4 055 in place_ring's order, past the
   4 100 the Fleas' order leaves it; ordered from where it stands, 4 055. INVARIANT: the key is a function of
   distances alone, so a start and its mirror build mirrored bases; place_ring itself is shared with the placement
   UI and keeps its order. BOUND: an in-place heapsort over at most PLACE_MAX_SITES sites. RESIDUAL: the order
   covers the first PLACE_MAX_SITES sites place_ring lists, so a longer ring loses its top side's tail before the
   order, as it does without one; and a commander standing on the enemy's side of the base takes that side's sites
   first, so its buildings can lean toward the enemy, by at most comm_r from the anchor, the ties going to the site
   farther from the estimate. */
typedef struct RINGKEY { int32_t ox, oz, ex, ez; int has_e, fx, fz, half; } RINGKEY;

static int64_t ring_sq(int32_t x, int32_t z, int32_t px, int32_t pz)
{
    const int64_t dx = (int64_t)x - px, dz = (int64_t)z - pz;
    return dx * dx + dz * dz;
}

/* 1 when ring site a comes after ring site b */
static int ring_after(const RINGKEY* k, PlaceCell a, PlaceCell b)
{
    const int32_t xa = (2 * (a.cx + k->half) + k->fx) * 8, za = (2 * (a.cz + k->half) + k->fz) * 8;
    const int32_t xb = (2 * (b.cx + k->half) + k->fx) * 8, zb = (2 * (b.cz + k->half) + k->fz) * 8;
    const int64_t da = ring_sq(xa, za, k->ox, k->oz), db = ring_sq(xb, zb, k->ox, k->oz);
    if (da != db) return da > db;
    if (k->has_e) {
        const int64_t ea = ring_sq(xa, za, k->ex, k->ez), eb = ring_sq(xb, zb, k->ex, k->ez);
        if (ea != eb) return ea < eb;
    }
    return a.cz != b.cz ? a.cz > b.cz : a.cx > b.cx;
}

static void ring_sift(PlaceCell* r, unsigned root, unsigned n, const RINGKEY* k)
{
    for (;;) {
        const unsigned child = 2 * root + 1;
        unsigned last = root;
        PlaceCell x;
        if (child < n && ring_after(k, r[child], r[last])) last = child;
        if (child + 1 < n && ring_after(k, r[child + 1], r[last])) last = child + 1;
        if (last == root) return;
        x = r[root];
        r[root] = r[last];
        r[last] = x;
        root = last;
    }
}

/* the n ring sites r, of footprint fx x fz with their origins gap / 2 short, put in THE RING'S ORDER from the
   origin (ox, oz), px */
static void ring_order(const IMPAI_DEC* d, PlaceCell* r, unsigned n, int fx, int fz, int gap, int32_t ox, int32_t oz)
{
    const IMPAI_BRAIN_IN* in = d->in;
    RINGKEY k;
    unsigned i;
    k.ox = ox;
    k.oz = oz;
    k.has_e = in->mil && in->mil->ebx != IMPAI_NO_ANCHOR;
    k.ex = k.has_e ? in->mil->ebx * 32 + 16 : 0;
    k.ez = k.has_e ? in->mil->ebz * 32 + 16 : 0;
    k.fx = fx;
    k.fz = fz;
    k.half = gap / 2;
    if (n < 2) return;
    for (i = n / 2; i-- > 0;) ring_sift(r, i, n, &k);
    for (i = n - 1; i > 0; i--) {
        const PlaceCell x = r[0];
        r[0] = r[i];
        r[i] = x;
        ring_sift(r, 0, i, &k);
    }
}

/* THE RING for energy, factories, makers, and extractors on a metal map (impai_econ.c, metal_map): the base box
   (or the commander's footprint at the anchor) grown on every side by gap + ring_grow x (fx + gap) cells, and
   place_ring's sites of fx + gap cells around it, each origin moved gap / 2 into its cell so that neighbours
   keep gap cells between them, walked in THE RING'S ORDER. A site is skipped when it overlaps a claim grown by
   gap, covers explored metal above the threshold (the patches are the extractors'; a metal map has none), or
   holds a failure, or while the builder's far record is live holds its recorded spot or lies beyond its reach;
   when its footprint centre lies on a cell of the seat's ground threat map; for the commander, when that centre
   lies past comm_r of the anchor (THE COMMANDER'S REACH, build_at) or within IMPAI_CMD_SAFE_PX of a seen armed
   enemy (THE COMMANDER'S SAFE DISTANCE). The site test decides the rest, against the
   decision's site tests; then for a constructor THE ROUTE, the straight way there, which passes no known ground
   threat (the commander's way is the military's: its safety and its leash, impai_mil.c), tested after the site
   test so that a decision tests at most site_tests routes; and a plant's site its way out after it (THE WAY
   OUT), a site refused there avoided by plants of its type for fail_t (THE WAY REFUSALS) -- but for a waived
   plant (THE PLANT'S FALLBACK), which takes the first site the site test passes; a way out deferred (THE WORK
   BUDGET) ends the ring for the decision. A constructor keeps out of known ground threat at a base site as on
   a walk: MEASURED 2026-11-13, Comet Catcher, 4 of the 27 constructors a human's line killed died at work in
   the base during its raid, ring orders among them. A ring exhausted
   with tests left pushes the next decision's ring out (ring_grow, at most 8), and a plant type's ring exhausted at
   8 passes the type over (THE PLANT'S FALLBACK); a build brings it back to 0. GROWTH ANSWERS ONLY A LACK OF
   ROOM: a ring that left a site for the commander's reach, for ground threat at the site or on the route, or
   for THE COMMANDER'S SAFE DISTANCE is not exhausted -- another builder may take the site, or the threat leave
   it -- and growing the ring or passing a plant type over for those would push every builder's base outward,
   and switch the seat's plant type, for a raid that lasts a few decisions. */
static int ring_build(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, const int32_t* key)
{
    const IMPAI_BRAIN_IN* in = d->in;
    IMPAI_BRAIN* st = in->st;
    const IMPAI_TINFO* t = &in->ti[type];
    const int gap = in->plan->gap, fx = t->fx > 0 ? t->fx : 1, fz = t->fz > 0 ? t->fz : 1;
    const int grow = gap + st->ring_grow * (fx + gap);
    const int commander = in->ti[b->type].cat == IMPAI_C_COMMANDER;
    IMPAI_RECT box;
    unsigned n, k, beyond = 0, threat_skipped = 0;
    if (d->has_bbox) box = d->bbox;
    else {
        const IMPAI_TINFO* ct = d->commander.slot ? &in->ti[d->commander.type] : NULL;
        box.x0 = st->ax;
        box.z0 = st->az;
        box.x1 = (int16_t)(st->ax + (ct && ct->fx > 0 ? ct->fx : 1) - 1);
        box.z1 = (int16_t)(st->az + (ct && ct->fz > 0 ? ct->fz : 1) - 1);
    }
    n = place_ring(fx + gap, fz + gap, box.x0 - grow, box.z0 - grow, box.x1 - box.x0 + 1 + 2 * grow,
                   box.z1 - box.z0 + 1 + 2 * grow, d->ring, PLACE_MAX_SITES);
    if (key) ring_order(d, d->ring, n, fx, fz, gap, key[0], key[1]);
    else if (commander) ring_order(d, d->ring, n, fx, fz, gap, b->pos[0] >> 16, b->pos[2] >> 16);
    else ring_order(d, d->ring, n, fx, fz, gap, st->ax * 16 + 8, st->az * 16 + 8);
    for (k = 0; k < n; k++) {
        const int cx = d->ring[k].cx + gap / 2, cz = d->ring[k].cz + gap / 2;
        IMPAI_RECT r = impai_rect_at(d, type, cx, cz), wide = r;
        int alt = 0;
        wide.x0 = (int16_t)(wide.x0 - gap); wide.z0 = (int16_t)(wide.z0 - gap);
        wide.x1 = (int16_t)(wide.x1 + gap); wide.z1 = (int16_t)(wide.z1 + gap);
        if (impai_claimed(d, &wide) || impai_failed_in(d, &r) || impai_far_at(d, b, type, cx, cz) ||
            (t->cat == IMPAI_C_FACTORY && !d->way_waive && way_failed(d, type, cx, cz)) ||
            d->q->ops->coverage(d->q, cx, cz, fx, fz) > 0) continue;
        if (commander && !site_within(d, type, cx, cz, in->plan->comm_r)) {
            beyond++;
            continue;
        }
        if (threatened(d, (2 * cx + fx) * 8, (2 * cz + fz) * 8) ||
            (commander && cmd_refused(d, b, (2 * cx + fx) * 8, (2 * cz + fz) * 8))) {
            threat_skipped++;
            continue;
        }
        if (d->sites >= in->plan->site_tests) return 0;
        d->sites++;
        if (!d->q->ops->site_ok(d->q, type, cx, cz, 0, &alt)) continue;
        if (!commander && route_threatened(d, b, (2 * cx + fx) * 8, (2 * cz + fz) * 8)) {
            threat_skipped++;
            continue;
        }
        if (t->cat == IMPAI_C_FACTORY && !d->way_waive) {
            const int w = way_prove(d, type, cx, cz);
            if (w < 0) return 0;
            if (!w) {
                way_refused(d, type, cx, cz);
                continue;
            }
        }
        if (build_at(d, b, type, cx, cz, alt, in->plan->comm_r)) {
            st->ring_grow = 0;
            return 1;
        }
        if (d->roots >= d->root_cap) return 0;
    }
    if (beyond || threat_skipped) return 0;
    if (st->ring_grow < 8) st->ring_grow++;
    else if (t->cat == IMPAI_C_FACTORY && !d->way_waive)
        plant_pass_over(d, type);
    return 0;
}

int impai_build_ring(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    return ring_build(d, b, type, NULL);
}

int impai_build_def(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    const int32_t key[2] = { d->def_x, d->def_z };
    return ring_build(d, b, type, key);
}

int impai_build_plant_waived(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    int ok;
    d->way_waive = 1;
    ok = impai_build_ring(d, b, type);
    d->way_waive = 0;
    return ok;
}

/* ---- the sea plant ---- */

#define SEA_EST_BLOCKS 48       /* the estimate's water: sea_ref labels within this many blocks of it (1536 px) */
#define SEA_LABELS     8        /* of them, the most kept                                                       */
#define SEA_LOOK       64       /* the candidates a decision tests against the fail ring                        */
#define SEA_REACH_BLOCKS 16     /* THE BUILDER'S REACH reads the blocks this many rings round a site at most    */

/* the sea plant's radius from the anchor: comm_r, and FAR_PATCH_K x comm_r in THE FINISH AT SEA (IMPAI_DEC.sea_far,
   impai_brain.c, economy) -- the search's (sea_search) and THE COMMANDER'S REACH's for its order (build_at) */
static uint32_t sea_radius(const IMPAI_DEC* d)
{
    return (d->sea_far ? FAR_PATCH_K : 1u) * d->in->plan->comm_r;
}

/* THE BUILDER'S REACH: 1 when builder b can stand within MobileBuild's reach of the footprint of `type` at first
   cell (cx, cz), so that the engine does not abandon the build for reach (THE REACH ABANDON, impai_brain.c): it
   stands within impai_reach_limit of the footprint's centre already, or a block carrying its class's label at its
   own block (builder_label) has its centre within that limit; 1 too where builder_label has no test or the type
   no limit. MEASURED on Trout Farm's west start (the map's class maps, the core's rules, tools/tasim/classcheck.py's
   inputs): the shipyard site nearest the anchor lies 225 px from the nearest cell a vehicle stands on, past
   ARMCV's limit of 163 (builddistance 40, 33 and 90 for the footprints), and 11 px from one the commander's class
   stands on; three games' land constructors walked to it and were abandoned 150 to 210 px short, four to six
   times a game. RESIDUAL: a labelled block holds a cell the class stands on, not necessarily its centre
   (impai.h, THE REACH LABELS), so a block within the limit by less than half its diagonal (23 px) may leave the
   builder just past it -- that build is abandoned as before, its failure counted (THE SEA PLANT's termination)
   and the builder held back by THE FAR RECORD; a limit past SEA_REACH_BLOCKS blocks reads those only, a refusal
   on the safe side. BOUND: at most (2 SEA_REACH_BLOCKS + 1)^2 = 1089 label reads. */
static int builder_reaches(const IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int cx, int cz)
{
    const IMPAI_REACH* rc = d->in->reach;
    const IMPAI_TINFO* t = &d->in->ti[type];
    const int fx = t->fx > 0 ? t->fx : 1, fz = t->fz > 0 ? t->fz : 1;
    const int pcx = (2 * cx + fx) * 8, pcz = (2 * cz + fz) * 8;
    const int l = builder_label(d, b);
    const int64_t lim = impai_reach_limit(d, b->type, fx, fz);
    int r, bx, bz;
    if (l < 0 || lim < 0 || (int64_t)impai_dist_px(b->pos, pcx, pcz) <= lim) return 1;
    if (!l) return 0;
    r = lim / 32 + 1 < SEA_REACH_BLOCKS ? (int)(lim / 32) + 1 : SEA_REACH_BLOCKS;
    for (bz = (pcz >> 5) - r; bz <= (pcz >> 5) + r; bz++)
        for (bx = (pcx >> 5) - r; bx <= (pcx >> 5) + r; bx++) {
            int32_t c[3];
            if (bx < 0 || bz < 0 || bx >= rc->bw || bz >= rc->bh) continue;
            c[0] = (int32_t)((uint32_t)(bx * 32 + 16) << 16);
            c[1] = 0;
            c[2] = (int32_t)((uint32_t)(bz * 32 + 16) << 16);
            if ((int64_t)impai_dist_px(c, pcx, pcz) <= lim &&
                impai_reach_at(rc, d->in->ti[b->type].mclass, bx, bz) == l)
                return 1;
        }
    return 0;
}

/* THE SEARCH for a sea plant of `type`, once a decision (the answer is the decision's, impai_build_sea_plant):
   1 with a site the seat's site test accepted in d->sea_cx, sea_cz, sea_alt; 0 none this decision; -1 no
   candidate at all. THE ESTIMATE'S WATER: the distinct sea_ref labels within SEA_EST_BLOCKS of the military's
   estimate (any label with no estimate); none, and a sea army could not come near the enemy: -1. THE
   CANDIDATES: centre blocks in ring order (impai_ring_at) around the anchor's block, each whose footprint centre
   lies within the sea plant's radius of the anchor (sea_radius), so the commander may build any of them
   (build_at) -- comm_r, and in THE FINISH AT SEA FAR_PATCH_K x comm_r, a candidate past comm_r only on held
   ground (THE HELD GROUND) --, the footprint's origin at an even cell (fx + 1) / 2 blocks across. A CONTENT candidate
   is one inside the site test's own bounds (cx >= 1, cz >= 1, cx + fx < W, cz + fz < H: 0x47D302..0x47D352), whose
   centre block carries one of the estimate's labels and whose every footprint block's top is at most sea -
   minwaterdepth. Content candidates depend on the level's content and on the type, the anchor, the estimate's block and
   the radius only, so a search that finds none is remembered against those (IMPAI_BRAIN.sea_none_*) and not walked
   again while they hold. Each content candidate is then held against the claims -- the footprint grown by gap
   overlapping a building's site or a build order's, which an extractor's holds for as long as it stands -- and against
   the seat's exploration: a footprint centre the seat has not explored, which its site test refuses
   (0x47D3D1..0x47D3FA, the seat's own bit: tagpu_impai.c, THE SEAT'S OWN SITE TEST; it tests the centre sheared by half
   the altitude, this the centre itself, a filter the site test still decides), so an unmapped seat looks past the near
   water it has not seen. Neither spends SEA_LOOK, so neither a field of standing extractors nor unseen water can hold
   the search short of a free site. The first SEA_LOOK of the rest are held against the fail ring, and the site test and
   then the way out (THE WAY OUT) decide.
   THE CURSOR. A candidate the search examined and did not take -- the fail ring held it, or the site test, the
   way out or then the resolver refused it, or no builder took it (THE SITE NO BUILDER TAKES) -- is passed by the
   cursor (IMPAI_BRAIN.sea_*), and the next decision's
   walk examines only the candidates past it: so the refusals are avoided for fail_t without a word of the shared
   fail ring, which they would flush (a decision tests up to site_tests sites, and IMPAI_FAILS is 32). The cursor
   holds for the type and the estimate's block it was walked for, and for fail_t from the walk's start; then the
   walk starts again at the first candidate. A candidate before the cursor that the claims or the exploration
   passed over waits for that restart too. The walk still counts every content candidate, the cursor's skipped
   ones included, so a search with candidates never reads as one with none.
   THE DEPTH PREFILTER is exact enough to trust and still not the safety argument. The engine's site test
   0x47D2E0 keeps, over the footprint's yard cells, the land extremes from the cells' bytes +0x06 and +0x05
   (0x47D4F9..0x47D518: yard bit 0x08) and the water maximum from +0x05 (0x47D51C..0x47D52C: bit 0x10), and
   refuses when max(land maximum, water maximum) is above sea - minwaterdepth (0x47D7D7..0x47D7F0); a shipyard's
   yard is all `C` and `w`, bit 0x10 (0x42CFB2..0x42D03C), so that compare is the one that binds (DISASSEMBLED).
   The block tops are those same +0x05 bytes, the most over a block's four cells (tagpu_impai.c, cell_top), and
   the footprint's blocks cover its cells -- a superset when fx is odd -- so a candidate the filter passes also
   passes that compare. The site test, the seat's own (WQ_SITE_SEAT), decides everything.
   BOUND, a decision: one walk of at most (2 R / 32 + 1)^2 ring blocks, R the radius -- 4225 at comm_r 1024, 16 641 in
   THE FINISH AT SEA's 2 048 --, each one label read, at most SEA_LABELS compares and fbx x fbz top reads, and for a
   content candidate one claims test (at most IMPAI_CLAIM_MAX overlaps) and one exploration read; SEA_LOOK failure tests
   (IMPAI_FAILS each); site_tests site tests and WAY_WORK cells of the way out (THE WORK BUDGET); impai_sea_labels'
   (2 SEA_EST_BLOCKS + 1)^2 = 9409 label reads. A remembered -1 costs none of it. */
static int sea_search(IMPAI_DEC* d, int type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_REACH* rc = in->reach;
    IMPAI_BRAIN* st = in->st;
    const IMPAI_TINFO* t = &in->ti[type];
    const int fx = t->fx > 0 ? t->fx : 1, fz = t->fz > 0 ? t->fz : 1, fbx = (fx + 1) / 2, fbz = (fz + 1) / 2;
    const uint32_t reach = sea_radius(d);
    const int gap = in->plan->gap, rings = (int)(reach / 32);
    const int ex = in->mil ? in->mil->ebx : IMPAI_NO_ANCHOR, ez = in->mil ? in->mil->ebz : IMPAI_NO_ANCHOR;
    uint16_t want[SEA_LABELS];
    WQ_DEF_ECON e;
    int nwant = -1, need, r, i, dx, dz, cands = 0, looked = 0;
    if (!rc || !rc->labels || !rc->top || rc->sea_ref >= WQ_MCLASSES || !(rc->present >> rc->sea_ref & 1u)) return -1;
    if (!d->q->ops->def_econ(d->q, type, &e) || e.minwater <= 0 || (need = (int)d->m.sea - e.minwater) < 0) return -1;
    if (st->sea_none_type == type && st->sea_none_ax == st->ax && st->sea_none_az == st->az &&
        st->sea_none_ex == ex && st->sea_none_ez == ez && st->sea_none_far == d->sea_far)
        return -1;
    if (st->sea_cur_type != type || st->sea_cur_ex != ex || st->sea_cur_ez != ez ||
        d->now - st->sea_from >= in->plan->fail_t) {
        st->sea_cur_type = (uint16_t)type;
        st->sea_cur_ex = (int16_t)ex;
        st->sea_cur_ez = (int16_t)ez;
        st->sea_r = st->sea_i = 0;
        st->sea_from = d->now;
    }
    if (ex != IMPAI_NO_ANCHOR && !(nwant = impai_sea_labels(rc, ex, ez, SEA_EST_BLOCKS, want, SEA_LABELS)))
        r = rings + 1;          /* no water of the estimate's: no candidate */
    else
        r = 0;
    for (; r <= rings; r++)
        for (i = 0; impai_ring_at(r, i, &dx, &dz); i++) {
            const int bx = (st->ax >> 1) + dx, bz = (st->az >> 1) + dz, ox = bx - fbx / 2, oz = bz - fbz / 2;
            const int cx = 2 * ox, cz = 2 * oz;
            IMPAI_RECT site, wide;
            int label, k, j, deep = 1, alt = 0;
            if (!site_within(d, type, cx, cz, reach)) continue;
            if (cx < 1 || cz < 1 || cx + fx >= d->m.w || cz + fz >= d->m.h || ox + fbx > rc->bw || oz + fbz > rc->bh)
                continue;
            if ((label = impai_reach_at(rc, rc->sea_ref, bx, bz)) <= 0) continue;
            for (k = 0; k < nwant && want[k] != label; k++) {}
            if (nwant >= 0 && k == nwant) continue;
            for (k = 0; k < fbz && deep; k++)
                for (j = 0; j < fbx && deep; j++) deep = rc->top[(size_t)(oz + k) * rc->bw + ox + j] <= need;
            if (!deep) continue;
            cands++;
            if (r < st->sea_r || (r == st->sea_r && i < st->sea_i)) continue;
            site = wide = impai_rect_at(d, type, cx, cz);
            wide.x0 = (int16_t)(wide.x0 - gap); wide.z0 = (int16_t)(wide.z0 - gap);
            wide.x1 = (int16_t)(wide.x1 + gap); wide.z1 = (int16_t)(wide.z1 + gap);
            if (impai_claimed(d, &wide) || !d->q->ops->explored(d->q, cx + fx / 2, cz + fz / 2)) continue;
            if (!site_within(d, type, cx, cz, in->plan->comm_r) && !patch_held(d, (2 * cx + fx) * 8, (2 * cz + fz) * 8))
                continue;
            if (looked >= SEA_LOOK) return 0;
            looked++;
            if (!impai_failed_in(d, &site)) {
                if (d->sites >= in->plan->site_tests) return 0;
                d->sites++;
                if (d->q->ops->site_ok(d->q, type, cx, cz, 0, &alt)) {
                    const int w = way_prove(d, type, cx, cz);
                    if (w < 0) return 0;
                    if (w) {
                        st->sea_r = (uint16_t)r;
                        st->sea_i = (uint16_t)i;
                        d->sea_cx = cx;
                        d->sea_cz = cz;
                        d->sea_alt = alt;
                        return 1;
                    }
                }
            }
            st->sea_r = (uint16_t)r;
            st->sea_i = (uint16_t)(i + 1);
        }
    if (cands) return 0;
    st->sea_none_type = (uint16_t)type;
    st->sea_none_ax = st->ax;
    st->sea_none_az = st->az;
    st->sea_none_ex = (int16_t)ex;
    st->sea_none_ez = (int16_t)ez;
    st->sea_none_far = d->sea_far;
    return -1;
}

int impai_sea_none_now(const IMPAI_DEC* d)
{
    const IMPAI_BRAIN* st = d->in->st;
    const int ex = d->in->mil ? d->in->mil->ebx : IMPAI_NO_ANCHOR, ez = d->in->mil ? d->in->mil->ebz : IMPAI_NO_ANCHOR;
    return st->sea_none_type != WQ_NONE && st->sea_none_ax == st->ax && st->sea_none_az == st->az &&
           st->sea_none_ex == ex && st->sea_none_ez == ez && st->sea_none_far == d->sea_far;
}

/* THE SEA PLANT of `type` for builder b: 1 ordered, 0 not in this decision, -1 no site for it at all. One search a
   decision (sea_search), its answer shared by the decision's builders: a site it found goes to the first builder
   the resolver answers for it; a refusal there passes the site (THE CURSOR) and leaves the decision without one. A
   builder offering another type than the searched one waits for a later decision.
   TERMINATION, by construction: a decision whose search places nothing counts one refusal (impai_sea_refusal) --
   whatever stopped it: the site test or the resolver refusing, the claims, the exploration, the failures, its
   own budgets -- and so does a failed water-factory job (impai_brain.c, job_checks); the count advances at most
   once a decision and once a fail_t, so a cause that passes within fail_t (a unit standing on the footprint, a
   site the seat has not yet explored, a refusal the cursor passed) costs one count, not
   sixteen. At IMPAI_SEA_REFUSALS the sea plant is given up (impai_brain.c, economy), so the want ends in a sea
   plant, in -1 (no candidate, in the same decision), or in a land factory at most 16 x fail_t ticks after its
   first count.
   THE BUILDER'S REACH: a builder that cannot reach the site leaves it to the decision's other builders -- the
   commander, whose class walks the sea floor where a land constructor stops at the shore -- and counts the
   decision's refusal, as the commander's safe distance does; so a seat whose only sea-plant builders cannot
   reach gives the plant up by the same count its builders' abandoned jobs would have spent, without the walks.
   A builder whose far record is live (impai_brain.c, THE FAR RECORD) is not offered the plant at all: it neither
   searches nor counts. */
int impai_build_sea_plant(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    if (impai_far(d, b)) return 0;
    if (d->sea_type == WQ_NONE) {
        d->sea_type = (uint16_t)type;
        d->sea_answer = sea_search(d, type);
    } else if (d->sea_type != type)
        return 0;
    if (d->sea_answer < 0) return -1;
    if (d->sea_answer > 0) {
        const IMPAI_TINFO* t = &d->in->ti[d->sea_type];
        /* b cannot reach the site (THE BUILDER'S REACH), or the commander is refused it by THE COMMANDER'S SAFE
           DISTANCE: the site stays the search's, for the decision's other builders, and the decision counts its
           refusal as any other */
        if (!builder_reaches(d, b, d->sea_type, d->sea_cx, d->sea_cz) ||
            cmd_refused(d, b, (2 * d->sea_cx + t->fx) * 8, (2 * d->sea_cz + t->fz) * 8)) {
            d->sea_untaken = 1;
            impai_sea_refusal(d);
            return d->in->st->sea_refused >= IMPAI_SEA_REFUSALS ? -1 : 0;
        }
        if (build_at(d, b, d->sea_type, d->sea_cx, d->sea_cz, d->sea_alt, sea_radius(d))) {
            d->out->sea_plants++;
            return 1;
        }
        /* the resolver refused, a refusal like the site test's, which the cursor passes (the search left it at
           the site); with the roots spent nothing was asked, and the site stays the decision's */
        if (d->roots < d->root_cap) {
            d->in->st->sea_i++;
            d->sea_answer = 0;
        }
    }
    impai_sea_refusal(d);
    return d->in->st->sea_refused >= IMPAI_SEA_REFUSALS ? -1 : 0;
}

/* THE SITE NO BUILDER TAKES: a site the search found that every builder offered it in the decision was refused
   (THE BUILDER'S REACH, THE COMMANDER'S SAFE DISTANCE), with no sea plant ordered, is passed by the cursor, so the
   next decision's search takes the next candidate. WHY: the cursor passed only a site the resolver refused, so a
   site no builder could take was found again at every decision; on Trout Farm the Hard seat of
   bar-hota-troutfarm-aa-1 at bcd7fa05 held its ARMSY site at ring 32, index 29 from GameTime 66 700 to the cap,
   three refusals counted and no frame ordered, while the stock AI's last plants floated in the lake (MEASURED
   2026-11-15, the recording's s_brain and s_job). TERMINATION: each decision passes one site; the walk restarts at
   the first ring every fail_t (sea_search), so a site refused for a cause that passed is taken again, and the
   refusals' count gives the plant up as before. Only an assignment that ended with a command to spare passes it:
   then every builder the decision lists was offered its rules, and one the budget never reached may still take
   the site at the next decision. */
void impai_sea_untaken(IMPAI_DEC* d)
{
    if (d->sea_untaken && d->sea_answer > 0 && !d->out->sea_plants && d->roots < d->root_cap &&
        d->in->st->sea_i < 0xFFFFu)
        d->in->st->sea_i++;
}

/* ---- exploring ---- */

/* THE EXPLORE CELL, once a decision: of the 256-px cells, the one nearest the anchor (integer distance, ties
   in row-major order) whose centre cell the seat has not explored, that holds no failure, that the seat holds
   (THE HELD GROUND, the extractors' rule) and that b can travel into on its class: its centre block carries b's label, else a block within EXPLORE_NEAR blocks of it does
   (impai_reach_near, ring order), so a cell whose centre is a cliff or a rock is still explored, at that block
   (a sea constructor explores water, a land one its own island, so neither holds the seat's one explorer on a
   cell it never arrives at). A cell whose straight way from b crosses known ground threat (THE ROUTE) fails for
   fail_t and b is not sent: the next decision takes the next cell, one route test a decision. Else b moves there
   (resolver mode 2), at the ground's height, and becomes the seat's one explorer; the job and its failure keep
   the cell's centre. WHY THE HELD GROUND: the explorer is asked for once no held patch is left (impai_econ.c,
   assign), and mid-game, the seat's own half explored, the nearest unexplored cell lies in the enemy's half --
   the walk the held ground keeps the constructors from. BOUND: at most IMPAI_GRID_MAX^2 cells, each one held bit
   and one label read, and for a cell nearer than the best so far whose centre block is not b's, at most
   (2 EXPLORE_NEAR + 1)^2 = 81 more. */
#define EXPLORE_NEAR 3          /* blocks: from the centre block, gx x 8 + 4, the ring stays in the cell */
int impai_build_explore(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int gw = (d->m.w * 16 + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX, gh = (d->m.h * 16 + IMPAI_CELL_PX - 1) / IMPAI_CELL_PX;
    const int per = IMPAI_CELL_PX / 16;
    const int l = builder_label(d, b);
    const unsigned cls = in->ti[b->type].mclass;
    int gx, gz, bx = -1, bz = -1, tx = 0, tz = 0;
    uint64_t best = 0;
    int32_t pos[3];
    if (d->explored_once || gw < 1 || gh < 1 || gw > IMPAI_GRID_MAX || gh > IMPAI_GRID_MAX) return 0;
    d->explored_once = 1;
    if (!l) return 0;
    for (gz = 0; gz < gh; gz++)
        for (gx = 0; gx < gw; gx++) {
            int cx = gx * per + per / 2, cz = gz * per + per / 2, px, pz, ox, oz;
            IMPAI_RECT r;
            int64_t dx, dz;
            uint64_t dd;
            if (cx >= d->m.w) cx = d->m.w - 1;
            if (cz >= d->m.h) cz = d->m.h - 1;
            dx = (int64_t)cx - in->st->ax;
            dz = (int64_t)cz - in->st->az;
            dd = (uint64_t)(dx * dx + dz * dz);
            if (bx >= 0 && dd >= best) continue;
            r.x0 = r.x1 = (int16_t)cx;
            r.z0 = r.z1 = (int16_t)cz;
            if (d->q->ops->explored(d->q, cx, cz) || impai_failed_in(d, &r)) continue;
            px = cx * 16 + 8;
            pz = cz * 16 + 8;
            if (!patch_held(d, px, pz)) continue;
            if (l > 0 && impai_reach_at(in->reach, cls, cx >> 1, cz >> 1) != l) {
                if (!impai_reach_near(in->reach, cls, (uint16_t)l, cx >> 1, cz >> 1, EXPLORE_NEAR, &ox, &oz)) continue;
                px = ox * 32 + 16;
                pz = oz * 32 + 16;
            }
            best = dd; bx = cx; bz = cz; tx = px; tz = pz;
        }
    if (bx < 0) return 0;
    if (route_threatened(d, b, tx, tz)) {
        impai_fail_add(d, bx, bz);
        return 0;
    }
    pos[0] = (int32_t)((uint32_t)tx << 16);
    pos[1] = 0;
    pos[2] = (int32_t)((uint32_t)tz << 16);
    pos[1] = (int32_t)((uint32_t)(d->q->ops->ground(d->q, pos) & 0x7FFF) << 16);
    if (!order_mode(d, b, 2, (int)WQ_NONE, pos, IMPAI_JOB_EXPLORE, bx, bz)) return 0;
    in->st->explorer = b->slot;
    d->explorer_live = 1;
    return 1;
}

/* ---- assisting ---- */

/* A builder b can come near enough to work on something whose larger side is f cells at (x, z) px: some block
   within R = min(its builddistance + 8 f, NEAR_PX_MAX) px of its centre holds b's own component
   (impai_reach_near), so a sea constructor helps a shipyard and not a plant inland. No test as builder_label
   says. BOUND: at most (2 NEAR_PX_MAX / 32 + 1)^2 = 4225 label reads. */
#define NEAR_PX_MAX 1024
static int builder_near_side(IMPAI_DEC* d, const IMPAI_UNITREF* b, int f, int32_t x, int32_t z)
{
    const int l = builder_label(d, b);
    WQ_DEF def;
    int32_t rpx;
    int ox, oz;
    if (l < 0) return 1;
    if (!l) return 0;
    rpx = (d->q->ops->def(d->q, b->type, &def) ? def.reach : 0) + 8 * (f > 0 ? f : 0);
    if (rpx > NEAR_PX_MAX) rpx = NEAR_PX_MAX;
    return impai_reach_near(d->in->reach, d->in->ti[b->type].mclass, (uint16_t)l, x >> 5, z >> 5, rpx / 32, &ox, &oz);
}

/* ...a building of `type`, its larger side the type's */
static int builder_near(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int32_t x, int32_t z)
{
    const int f = type > 0 && (unsigned)type < d->in->nti
                      ? (d->in->ti[type].fx > d->in->ti[type].fz ? d->in->ti[type].fx : d->in->ti[type].fz) : 0;
    return builder_near_side(d, b, f, x, z);
}

/* THE ASSIST: the nearest unfinished building of the seat's, else the nearest of its unfinished mobile units
   (IMPAI_DEC.frames: a factory's products on its yard), each helped with resolver mode 8 -- which tests no
   alliance, so the census's own-units-only lists are the test --, each one b can come near enough to
   (builder_near) and, for a constructor, with no known ground threat at it or on the straight way there (THE
   ROUTE), and none at the cell of a help the engine refused this builder (THE HELP REFUSED, impai_brain.c). Ties
   go to the lower slot. THE COMMANDER helps only within comm_r of the anchor. Resolver mode 8 answers
   HELPBUILD for an unfinished target whatever its type (0x43F46C, the target's +0x104 non-zero; engine/orders.md,
   *What each resolver mode answers*, DISASSEMBLED).
   NO BUILDER GUARDS, a LIFETIME: HelpBuild 0x403F70 adds the helper's work through the build step 0x41BA60, called
   at 0x404139 every tick, and ends at its frame's completion -- the "Building complete" notice at 0x40422F, then
   result 5, done (0x40423C..0x404248; DISASSEMBLED 2026-11-14) -- so a help never outlives its frame, and its
   builder is idle at the next census. A guard (mode 7) never ends by itself, and the engine stands the guarder on
   the factory's yard, where the factory's create placement test refuses the next product for as long as it
   stands (THE YARD, impai_brain.c): on Trout Farm a constructor guarding the seat's one factory held it for
   27 450 ticks (MEASURED 2026-11-14, the closing run). A guard's own assist of a factory is the help on the
   factory's frame, put first by its state 1 (0x40658A..0x40667A), together with reactions (0x43B1F0 at 0x4063F5)
   that push attacks in front of the guarder; mode 8 takes the help without them. MEASURED in the bar's round 1
   (153 recordings): every one of the 318 samples of an engine-made attack at an ImpAI commander's head had a
   guard behind it, 221 of them past comm_r, and one chase reached 4 488 px from the anchor. MEASURED in the bar's
   cmdr run (65 games, the commander's head every 300 ticks after GameTime 9 000): Standby or empty in 21-22 % of
   the samples before the commander helped the products (the finish run 3-6 %), help or reclaim 3-5 % (the
   finish run 33 %). INVARIANT, a bound: every order of THE ASSIST to the commander targets a unit within comm_r of
   the anchor -- the finish's sea plant frame, within the sea plant's radius, is THE FRAME RESUMED's --, and no
   builder takes mode 7. The commander helps no frame THE COMMANDER'S SAFE DISTANCE refuses.
   In THE HOLD (impai_brain.c, economy) only an economy building's frame is helped (IMPAI_ECO_CAT), and nothing is
   guarded and no product helped: the second pass does not run.
   BOUND: builder_near's reads for at most 2 x IMPAI_NANO_MAX targets for the commander, IMPAI_NANO_MAX +
   IMPAI_IDLE_CAP for another builder, and for a constructor a route each (THE ROUTE). */
int impai_build_assist(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int commander = in->ti[b->type].cat == IMPAI_C_COMMANDER;
    const int32_t apos[3] = { (int32_t)((uint32_t)(in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(in->st->az * 16 + 8) << 16) };
    const IMPAI_UNITREF* best = NULL;
    uint32_t best_d = 0;
    int i, pass;
    for (pass = 0; pass < (d->hold ? 1 : 2) && !best; pass++) {
        const IMPAI_UNITREF* list = !pass ? d->nano : d->frames;
        const int n = !pass ? d->nnano : d->nframes;
        for (i = 0; i < n; i++) {
            const uint32_t dist = impai_dist_px(b->pos, list[i].pos[0] >> 16, list[i].pos[2] >> 16);
            if (list[i].slot == b->slot || impai_help_refused(d, b, list[i].pos)) continue;
            if (d->hold && !IMPAI_ECO_CAT(in->ti[list[i].type].cat)) continue;
            if (commander && impai_dist_px(apos, list[i].pos[0] >> 16, list[i].pos[2] >> 16) > in->plan->comm_r) continue;
            if (best && dist >= best_d) continue;
            if (commander ? cmd_refused(d, b, list[i].pos[0] >> 16, list[i].pos[2] >> 16)
                          : impai_walk_threatened(d, b, list[i].pos[0] >> 16, list[i].pos[2] >> 16))
                continue;
            if (!builder_near(d, b, list[i].type, list[i].pos[0] >> 16, list[i].pos[2] >> 16)) continue;
            best = &list[i];
            best_d = dist;
        }
        if (best) return order_mode(d, b, 8, best->slot, best->pos, IMPAI_JOB_ASSIST, 0, 0);
    }
    return 0;
}

/* THE FRAME RESUMED, THE FINISH AT SEA's (impai_brain.c, economy): while it holds, the unfinished frame of a water
   plant of the seat's (IMPAI_DEC.nano) is the sea plant's want -- sea_plants counts it, so the want itself is off --
   and the ladder answers it at the want's rung: builder b helps the nearest such frame with resolver mode 8 under
   THE ASSIST's tests -- the help refused, the commander's safe distance or a constructor's route (THE ROUTE),
   builder_near, so THE BUILDER'S REACH --, the commander within the sea plant's radius (sea_radius), as for the
   order that began the frame. A builder that already helps such a frame keeps its help: 1, no order. WHY: the
   commander begins the finish's plant past comm_r, where THE ASSIST does not send it and no land constructor
   reaches the water off a shallow shore (Trout Farm's west start); a contact within THE COMMANDER'S SAFE DISTANCE
   ends its build (impai_mil.c, the flight; impai_brain.c, the release of the commander), and the frame would
   stand for good, no second sea plant wanted while it counts. Its build resumes here at the want's priority, so
   no other rung of the ladder comes first. Each help given is counted (IMPAI_BRAIN_OUT.sea_frame, the
   commander's in sea_frame_cmd) for the glue's `impai: frame` line. BOUND: one walk of at most IMPAI_NANO_MAX frames, builder_near's reads and a
   route or a safe-distance test each. */
int impai_build_sea_frame(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_JOB* j = &in->job[b->slot];
    const int commander = in->ti[b->type].cat == IMPAI_C_COMMANDER;
    const int32_t apos[3] = { (int32_t)((uint32_t)(in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(in->st->az * 16 + 8) << 16) };
    const IMPAI_UNITREF* best = NULL;
    uint32_t best_d = 0;
    int i;
    for (i = 0; i < d->nnano; i++) {
        const IMPAI_UNITREF* f = &d->nano[i];
        const int32_t fx = f->pos[0] >> 16, fz = f->pos[2] >> 16;
        const uint32_t dist = impai_dist_px(b->pos, fx, fz);
        if (!impai_water_plant(in, f->type)) continue;
        if (j->kind == IMPAI_JOB_ASSIST && j->target == f->slot) return 1;
        if (impai_help_refused(d, b, f->pos) || (best && dist >= best_d)) continue;
        if (commander && impai_dist_px(apos, fx, fz) > sea_radius(d)) continue;
        if (commander ? cmd_refused(d, b, fx, fz) : impai_walk_threatened(d, b, fx, fz)) continue;
        if (!builder_near(d, b, f->type, fx, fz)) continue;
        best = f;
        best_d = dist;
    }
    if (!best || !order_mode(d, b, 8, best->slot, best->pos, IMPAI_JOB_ASSIST, 0, 0)) return 0;
    d->out->sea_frame++;
    if (commander) d->out->sea_frame_cmd = 1;
    return 1;
}

/* ---- reclaiming ---- */

/* THE FEATURE MEMORY (IMPAI_BRAIN.fmem): the features the game has shown the seat. THE NO-CHEAT RULE, BY
   CONSTRUCTION: the seat knows a feature exactly when DrawGameScreen would draw it for the seat's player
   (tasim_feature_drawn, tasim_sweep.c: on explored ground every feature, live, greyed outside sight, except a
   nodrawundergray def, drawn only to its owner or in sight), so it knows what a human in its seat is shown and no
   more. A feature on unexplored ground is never taken; a look (tasim_sweep_targets with seat_sight), the refresh and
   the jobs' checks (impai_brain.c, reclaim_seen) all ask THE FEATURE SHOWN. What the memory holds where the draw
   does not show it -- unexplored ground, a gated def out of sight -- is a belief, as old as its `seen`. On the
   bar's setting, True line of sight with the map explored, the draw shows every feature on the map, so every
   remembered one is a fact at every decision and the decay of a belief (THE CONFIDENCE) is left to an
   unexplored map. The human's own reclaim click tests only the mapped bit (engine/orders.md); the danger tests --
   the threat at the feature and on THE ROUTE, THE HELD GROUND, the commander's comm_r and safe distance, and the
   walk checks -- are the same whatever told the seat of the feature.
   THE REFRESH, every decision: each remembered feature is asked THE FEATURE SHOWN -- there and shown, its amounts
   and `seen` are brought up to now; shown gone, it is dropped, for the whole seat; not shown, it stays a belief.
   A BURNING TREE is left to that rule, not skipped: a WQ_FEAT carries no burn state (the burn is the feature
   record's sequence, 0x4233A0, engine/map.md, which no WORLDQ op reads), and a fire's tree is next shown gone and
   dropped like any other.
   THE LOOK, every decision: tasim_sweep_targets round one builder -- the census's first reclaimer past the last
   decision's, round the seat's builders in slot order -- and THE ARMY'S LOOK round one army unit -- the census's
   first finished mobile unit of no economy category past the last decision's, round the army in slot order --,
   each within IMPAI_RECLAIM_R, the IMPAI_RECLAIM_LOOK nearest features shown, reclaimable, with metal; each is
   read again at its anchor and remembered with `seen` now unless it plays its sequence (WQ_FEAT.playing), which
   no reclaim can take. THE PLACE IN A FULL MEMORY: the belief shown longest ago, ties to the earlier entry; with
   every entry shown this decision -- the bar's setting --, the entry of least metal, when the new feature has
   more, ties to the earlier entry, so the memory keeps the richest features its looks have found; never an entry
   a reclaim job claims (fmem_put). WHY THE ARMY
   LOOKS: the wrecks lie where the army fought, which the builders, who keep out of known threat, rarely reach; in
   the owner's four-seat game on Comet Catcher the seats reclaimed 1 548, 1 731 and 4 149 metal in 47 minutes
   (MEASURED 2026-11-14).
   BOUND, a decision: the refresh's IMPAI_FEATS explored tests, cell reads and gates (at most four block tests
   each); the held entries' walk of the census's claims, an IMPAI_FEATS find for each reclaim job's, and one walk a
   feature newly remembered; the two looks' ((2 IMPAI_RECLAIM_R + 8 x 16) / 16 + 1)^2 = 105^2 explored tests each, as many reads of
   the explored cells and a gate for each anchor; and IMPAI_RECLAIM_LOOK reads again and finds of IMPAI_FEATS
   entries each. The grid is never walked whole. */

int impai_feat_seen(const IMPAI_DEC* d, const IMPAI_FMEM* e, WQ_FEAT* f)
{
    WQ_FEAT was;
    if (!d->q->ops->explored(d->q, e->ax, e->az)) return -1;
    if (d->q->ops->feature(d->q, e->ax, e->az, f) && f->ax == e->ax && f->az == e->az && f->def == e->def) {
        if (!tasim_feature_drawn(d->q, f)) return -1;
        return impai_feat_takeable(f);
    }
    /* not there as remembered: the seat knows it only where the draw would have shown the remembered one */
    memset(&was, 0, sizeof was);
    was.ax = e->ax; was.az = e->az;
    was.fx = e->fx; was.fz = e->fz;
    was.gated = e->gated; was.owner = e->owner; was.alt = e->alt;
    return tasim_feature_drawn(d->q, &was) ? 0 : -1;
}

int impai_fmem_find(const IMPAI_BRAIN* st, int ax, int az)
{
    int k;
    for (k = 0; k < st->nfmem && k < IMPAI_FEATS; k++)
        if (st->fmem[k].ax == ax && st->fmem[k].az == az) return k;
    return -1;
}

/* the entry k dropped; the decision's held marks (IMPAI_DEC.fheld) move with the entries, so a mark always names
   the entry it was made for (THE PLACE IN A FULL MEMORY) */
void impai_fmem_drop(IMPAI_DEC* d, int k)
{
    IMPAI_BRAIN* st = d->in->st;
    if (k < 0 || k >= st->nfmem || st->nfmem > IMPAI_FEATS) return;
    memmove(&st->fmem[k], &st->fmem[k + 1], (size_t)(st->nfmem - k - 1) * sizeof st->fmem[0]);
    memmove(&d->fheld[k], &d->fheld[k + 1], (size_t)(st->nfmem - k - 1) * sizeof d->fheld[0]);
    st->nfmem--;
    memset(&st->fmem[st->nfmem], 0, sizeof st->fmem[0]);
    d->fheld[st->nfmem] = 0;
}

int impai_feat_takeable(const WQ_FEAT* f) { return (f->flags & 0x80u) && f->metal > 0.0f && !f->playing; }

/* the entry e made or brought up to f as shown now; `lit` the last GameTime the seat's own sight covered the anchor's
   block, kept for the same feature and 0 for a new one when the block is not lit now */
static void fmem_set(const IMPAI_DEC* d, IMPAI_FMEM* e, const WQ_FEAT* f)
{
    const uint32_t now = d->now;
    const int same = e->ax == f->ax && e->az == f->az && e->def == f->def;
    if (d->q->ops->block_lit(d->q, f->ax >> 1, f->az >> 1)) e->lit = now;
    else if (!same) e->lit = 0;
    e->ax = (int16_t)f->ax;
    e->az = (int16_t)f->az;
    e->def = f->def;
    e->fx = (uint8_t)f->fx;
    e->fz = (uint8_t)f->fz;
    e->gated = f->gated;
    e->owner = f->owner;
    e->alt = f->alt;
    e->metal = f->metal;
    e->energy = f->energy;
    e->seen = now;
}

/* 1 when a reclaim job of the census claims the feature anchored at (ax, az) (IMPAI_CLAIM.feat) */
static int fmem_claimed_at(const IMPAI_DEC* d, int ax, int az)
{
    int i;
    for (i = 0; i < d->nclaims; i++)
        if (d->claims[i].feat && d->claims[i].r.x0 == ax && d->claims[i].r.z0 == az) return 1;
    return 0;
}

/* a feature shown now, remembered: its entry brought up to now, else a new one, in THE PLACE IN A FULL MEMORY. A
   HELD ENTRY KEEPS ITS PLACE: an entry a reclaim job claims (IMPAI_DEC.fheld, marked once a decision from the
   census's claims, marked again when the decision orders a reclaim of it (impai_build_reclaim), and moved with
   every drop, impai_fmem_drop) is never given to another feature, so the job's
   claim, its footprint (reclaim_replaced) and its metal in flight (THE ROOM) stay the memory's; with
   the census's claims incomplete (claims_full) no entry is given at all. */
static void fmem_put(IMPAI_DEC* d, const WQ_FEAT* f)
{
    IMPAI_BRAIN* st = d->in->st;
    int k = impai_fmem_find(st, f->ax, f->az), i;
    if (k >= 0) {
        fmem_set(d, &st->fmem[k], f);
        return;
    }
    if (st->nfmem < IMPAI_FEATS) k = st->nfmem++;
    for (i = 0; k < 0 && !d->claims_full && i < st->nfmem; i++)
        if (!d->fheld[i] && st->fmem[i].seen < d->now && (k < 0 || st->fmem[i].seen < st->fmem[k].seen)) k = i;
    for (i = 0; k < 0 && !d->claims_full && i < st->nfmem; i++)
        if (!d->fheld[i] && st->fmem[i].metal < f->metal && (k < 0 || st->fmem[i].metal < st->fmem[k].metal)) k = i;
    if (k < 0) return;
    fmem_set(d, &st->fmem[k], f);
    d->fheld[k] = (uint8_t)fmem_claimed_at(d, f->ax, f->az);
}

void impai_fmem_refresh(IMPAI_DEC* d)
{
    IMPAI_BRAIN* st = d->in->st;
    int k;
    for (k = st->nfmem - 1; k >= 0; k--) {
        IMPAI_FMEM* e = &st->fmem[k];
        WQ_FEAT f;
        const int seen = impai_feat_seen(d, e, &f);
        if (seen > 0) fmem_set(d, e, &f);
        else if (seen == 0) impai_fmem_drop(d, k);
    }
}

/* one look round the point (x, z) px: what the seat sees there, remembered */
static void fmem_look(IMPAI_DEC* d, int32_t x, int32_t z)
{
    SWEEP_IN sin;
    unsigned n, more, k;
    memset(&sin, 0, sizeof sin);
    sin.c[0] = x;
    sin.c[2] = z;
    sin.r = IMPAI_RECLAIM_R;
    sin.kind = SWEEP_KIND_METAL;
    sin.seat_sight = 1;
    n = tasim_sweep_targets(d->q, &sin, d->rt, IMPAI_RECLAIM_LOOK, &more);
    for (k = 0; k < n; k++) {
        WQ_FEAT f;
        if (d->q->ops->feature(d->q, d->rt[k].ax, d->rt[k].az, &f) && f.ax == d->rt[k].ax && f.az == d->rt[k].az &&
            impai_feat_takeable(&f))
            fmem_put(d, &f);
    }
}

/* the builder's order byte for a reclaim: RECLAIM, VTOL_RECLAIM for one that flies; -1 for a builder whose def
   lacks canreclamate (+0x245 bit 0x400, WQ_DF_RECLAIMER), the test the resolver would have made */
static int reclaim_act(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    WQ_DEF def;
    if (!d->q->ops->def(d->q, b->type, &def) || !(def.flags & WQ_DF_RECLAIMER)) return -1;
    return d->q->ops->action(d->q, (def.flags & WQ_DF_VTOL) ? WQ_ACT_VTOL_RECLAIM : WQ_ACT_RECLAIM);
}

void impai_fmem_observe(IMPAI_DEC* d)
{
    const IMPAI_UNITREF* b = d->fobs.slot ? &d->fobs : d->ffirst.slot ? &d->ffirst : NULL;
    const IMPAI_UNITREF* a = d->aobs.slot ? &d->aobs : d->afirst.slot ? &d->afirst : NULL;
    int i, k;
    memset(d->fheld, 0, sizeof d->fheld);
    for (i = 0; i < d->nclaims; i++)
        if (d->claims[i].feat && (k = impai_fmem_find(d->in->st, d->claims[i].r.x0, d->claims[i].r.z0)) >= 0)
            d->fheld[k] = 1;
    if (b) {
        d->in->st->fscan = b->slot;
        if (reclaim_act(d, b) > 0) fmem_look(d, b->pos[0] >> 16, b->pos[2] >> 16);
    }
    if (a) {
        d->in->st->ascan = a->slot;
        fmem_look(d, a->pos[0] >> 16, a->pos[2] >> 16);
    }
}

/* The remembered feature e as the target it is, its order position at the ground's height (tasim_sweep_target_of) */
static void fmem_target(IMPAI_DEC* d, const IMPAI_FMEM* e, SWEEP_TARGET* t)
{
    WQ_FEAT f;
    memset(&f, 0, sizeof f);
    f.ax = e->ax; f.az = e->az;
    f.def = e->def;
    f.fx = e->fx; f.fz = e->fz;
    f.flags = 0x80u;
    f.metal = e->metal; f.energy = e->energy;
    tasim_sweep_target_of(d->q, &d->m, &f, t);
}

/* THE ORDER, by name and never through the resolver: the reclaim at the feature's order position (its centre at
   the ground's height), through the order root without Shift, which replaces the builder's list. Resolver mode 12
   tests the viewed player's mapped bit whatever unit asks (engine/orders.md), so it would answer by another
   player's map; the handler 0x404AD0 tests the feature and its reclaimable bit, and what the game shows the seat,
   above, decided that the feature is there. The job becomes the feature's RECLAIM, and its metal is on its way to the
   store (THE ROOM) unless the census already counted it, its walk's (RECLAIM_GO) arriving. 1 ordered, 0 the
   decision's commands spent, -1 refused. */
static void reclaim_commit(IMPAI_DEC* d, float metal)
{
    d->m_inflight += metal;
    d->m_room -= metal;
}

static int reclaim_order(IMPAI_DEC* d, const IMPAI_UNITREF* b, const SWEEP_TARGET* t)
{
    IMPAI_JOB* j = &d->in->job[b->slot];
    const int act = reclaim_act(d, b);
    WQ_CMD c;
    if (d->roots >= d->root_cap) return 0;
    if (act <= 0) return -1;
    if (cmd_refused(d, b, t->cx, t->cz)) return IMPAI_RECLAIM_UNSAFE;
    memset(&c, 0, sizeof c);
    c.verb = WQ_CMD_NEW;
    c.action = (uint8_t)act;
    c.unit = b->slot;
    c.target = (uint16_t)WQ_NONE;
    memcpy(c.pos, t->pos, sizeof c.pos);
    if (impai_give(d, &c) != WQ_OK) return -1;
    d->out->orders++;
    d->out->reclaims++;
    d->out->reclaim_metal += t->metal;
    if (!(j->kind == IMPAI_JOB_RECLAIM_GO && j->cx == t->ax && j->cz == t->az)) reclaim_commit(d, t->metal);
    impai_job_end(j);
    j->type = b->type;
    j->build = (uint16_t)WQ_NONE;
    j->target = (uint16_t)WQ_NONE;
    j->cx = (int16_t)t->ax;
    j->cz = (int16_t)t->az;
    j->feat = t->def;
    j->kind = IMPAI_JOB_RECLAIM;
    j->since = d->now;
    return 1;
}

int impai_reclaim_arrived(IMPAI_DEC* d, const IMPAI_UNITREF* b, const WQ_FEAT* f)
{
    SWEEP_TARGET t;
    tasim_sweep_target_of(d->q, &d->m, f, &t);
    return reclaim_order(d, b, &t);
}

/* THE RECLAIM for builder b (impai_econ.c, THE LADDER, says when it is asked): first a look round b (THE FEATURE
   MEMORY), then of the remembered features whose centre lies within IMPAI_HARVEST_R of b, the one that pays b the
   most metal a tick, if that is more than `min_rate`. THE REACH is the score's: a far field pays by its metal over
   the walk, and a walk past job_timeout takes none (below); IMPAI_HARVEST_R holds the candidates to the ground a
   walk inside job_timeout could cover at the speeds of TA's builders, 2700 ticks at under a px a tick. A wreck is a feature like any other: a unit's corpse is
   spawned on the feature grid (0x423C50, engine/map.md) and the handler reclaims it as any reclaimable feature.
   Each is passed over when the claims cover its footprint (another builder's reclaim, a site), when it holds the
   builder's own unreachable record or that record is live and it lies beyond the builder's reach (impai_brain.c,
   THE FAR RECORD), when the seat sees ground threat at its centre or on the straight way there (THE ROUTE), and
   for the commander when its centre lies beyond comm_r of the anchor, where its assist ends too, or where THE
   COMMANDER'S SAFE DISTANCE refuses it, and for every other builder when its centre lies on ground the seat does
   not hold (patch_held, THE HELD GROUND: the rule the extractors' patches keep, since the wrecks the army's look
   finds lie where the armies fought, the contested band that rule exists for; the commander keeps comm_r, as its
   extractors do). The rest are scored by the reclaim rules' own model:
   worth x confidence / (travel + countdown), tasim_sweep_ticks over b's split inputs (tasim_sweep_builder), a
   feature whose ticks pass job_timeout -- the job's lifetime, which would stop it unpaid -- taking none.
   THE WORTH is the feature's metal held to THE ROOM, the store's room less what the seat's reclaims under way
   will pay (impai_brain.c, economy): the payout comes whole at the countdown's end (0x4237D0 pays FeatureDef
   +0xF0 into the reclaimer's ledger, 0x42395B) and the economy pass then holds the store to its maximum, the
   rest lost (0x401AFE..0x401B27). MEASURED 2026-11-13 in the bar's recordings: a 5 000-metal feature's payout on
   Trout Farm made the seat's income for its pass 5 014 and its store, 644 of 1 300, full: 656 of it kept. A
   feature larger than the room is still taken at the room's worth and its overflow lost, rather than left to wait
   for room: waiting kept less (research/notes/impai/decisions.md, the reclaim's worth). With no room no feature
   is worth anything, so a full store reclaims nothing, and each order takes its feature's metal off the room for the builders
   after it. THE CONFIDENCE, knowledge from the draw and risk from the seat's own sight: 1 for a feature whose
   anchor's block the seat's own sight covers now (IMPAI_FMEM.lit this decision), else IMPAI_RECLAIM_DECAY /
   (IMPAI_RECLAIM_DECAY + the ticks since it last did; the whole GameTime for one it never did) -- one half a game
   minute after, a quarter after three. WHY SIGHT AND NOT THE DRAW: the draw shows every wreck on explored ground
   as now, but what the seat knows of the threat round it stops at its sight, so a wreck on a battlefield the army
   left scored as safe as one beside the base; with every feature shown at 1a9350ea the broad check's reclaims
   ordered rose from 533 to 1 146 and the builders lost on them from 7 to 20, 52 % of the reclaims paid against 71 %
   (MEASURED 2026-11-15). The decay prices the unknown: a rich
   feature long out of the seat's sight loses to a poorer one it sees.
   In score order, ties to the lower anchor cell, each is tested for THE ROUTE and the first IMPAI_RECLAIM_TRIES
   clear of it for b's reach (builder_near_side, the feature's larger side), and the first that passes is taken: a feature shown now is
   ordered to be reclaimed (reclaim_order); a belief is walked to (resolver mode 2 at its centre, the job
   RECLAIM_GO), since only the draw may say whether it is still there, and once the draw shows it the job's
   check orders the reclaim or drops it (impai_brain.c, reclaim_check). Either job names the feature by its
   anchor cell and definition, never by an address (IMPAI_JOB.feat), and its footprint is a claim while the job
   holds (impai_brain.c, the census).
   BOUND, a decision: IMPAI_RECLAIM_SCANS builders searched, each with one look (above) however often the ladder
   asks for it, each asking the claims test of at most IMPAI_FEATS features (IMPAI_CLAIM_MAX overlaps each), then
   in score order a route for each candidate taken off the list (impai_route_threat: within IMPAI_HARVEST_R of the
   builder, at most 66 samples), IMPAI_FEATS at most, and a reach test of builder_near's bound for each of the first
   IMPAI_RECLAIM_TRIES whose route is clear -- a threatened route uses no try, so a far field behind known threat
   never hides a clear feature nearer; the ladder asks at most twice a builder. Never per
   tick: only inside a decision. */
int impai_build_reclaim(IMPAI_DEC* d, const IMPAI_UNITREF* b, double min_rate)
{
    const IMPAI_BRAIN_IN* in = d->in;
    IMPAI_BRAIN* st = in->st;
    const int commander = in->ti[b->type].cat == IMPAI_C_COMMANDER;
    const int32_t apos[3] = { (int32_t)((uint32_t)(st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(st->az * 16 + 8) << 16) };
    const int32_t bx = b->pos[0] >> 16, bz = b->pos[2] >> 16;
    SweepBuilder sb;
    int k, tries;
    if (d->roots >= d->root_cap || !(d->m_room > 0.0f)) return 0;
    if (b->slot != d->rslot && d->reclaim_scans >= IMPAI_RECLAIM_SCANS) return 0;
    if (reclaim_act(d, b) <= 0 || !tasim_sweep_builder(d->q, b->slot, &sb)) return 0;
    if (b->slot != d->rslot) {
        d->reclaim_scans++;
        d->rslot = b->slot;
        fmem_look(d, bx, bz);
    }
    for (k = 0; k < st->nfmem; k++) {
        const IMPAI_FMEM* e = &st->fmem[k];
        const IMPAI_RECT r = impai_feat_rect(e->ax, e->az, e->fx, e->fz);
        SWEEP_TARGET t;
        double ticks, conf;
        d->rscore[k] = -1.0;
        fmem_target(d, e, &t);
        if (!sweep_inside(bx, bz, IMPAI_HARVEST_R, t.cx, t.cz)) continue;
        if (impai_claimed(d, &r) || impai_far_feat(d, b, &r, t.cx, t.cz) || threatened(d, t.cx, t.cz)) continue;
        if (commander && (impai_dist_px(apos, t.cx, t.cz) > in->plan->comm_r || cmd_refused(d, b, t.cx, t.cz))) continue;
        if (!commander && !patch_held(d, t.cx, t.cz)) continue;
        ticks = tasim_sweep_ticks(sb.speed, sb.reach, sb.x, sb.z, t.cx, t.cz, t.energy, t.metal);
        if (!(ticks > 0.0) || ticks > (double)in->plan->job_timeout) continue;
        conf = e->lit == d->now ? 1.0
             : (double)IMPAI_RECLAIM_DECAY / ((double)IMPAI_RECLAIM_DECAY + (double)(d->now - e->lit));
        d->rscore[k] = (double)(t.metal < d->m_room ? t.metal : d->m_room) * conf / ticks;
    }
    for (tries = 0; tries < IMPAI_RECLAIM_TRIES;) {
        const IMPAI_FMEM* e;
        SWEEP_TARGET t;
        IMPAI_RECT r;
        int best = -1;
        for (k = 0; k < st->nfmem; k++)
            if (d->rscore[k] > 0.0 && (best < 0 || d->rscore[k] > d->rscore[best] ||
                                       (d->rscore[k] == d->rscore[best] &&
                                        st->fmem[k].az * d->m.w + st->fmem[k].ax <
                                        st->fmem[best].az * d->m.w + st->fmem[best].ax)))
                best = k;
        if (best < 0 || !(d->rscore[best] > min_rate)) return 0;
        e = &st->fmem[best];
        d->rscore[best] = -1.0;
        fmem_target(d, e, &t);
        if (route_threatened(d, b, t.cx, t.cz)) continue;
        tries++;
        if (!builder_near_side(d, b, e->fx > e->fz ? e->fx : e->fz, t.cx, t.cz)) continue;
        r = impai_feat_rect(e->ax, e->az, e->fx, e->fz);
        if (e->seen == d->now) {
            if (reclaim_order(d, b, &t) <= 0) return 0;
        } else {
            IMPAI_JOB* j = &in->job[b->slot];
            if (!order_mode(d, b, 2, (int)WQ_NONE, t.pos, IMPAI_JOB_RECLAIM_GO, e->ax, e->az)) return 0;
            j->feat = e->def;
            reclaim_commit(d, e->metal);
        }
        impai_claim_add(d, WQ_NONE, &r);
        d->fheld[best] = 1;
        return 1;
    }
    return 0;
}
