/* impai_build.c -- where ImpAI builds and how it orders it (impai.h, the brain): a metal patch for an
   extractor, the base ring for everything else, a cell to explore, a building to help. Every site is tested
   through the seat's own site test (WQ_SITE_SEAT) and every unit named by slot, the adapter re-deriving it
   before each command. Engine-free, no statics. */
#include <string.h>
#include "impai_brain.h"
#include "tagpu_simreg.h"

/* The sites and the build orders. OWNER: the brain calls them only inside a live ImpAI seat's turn, which
   plays no puppet seat (tagpu_impai.c, impai_play), so a puppet's orders come from the recording. */
SIM_FEATURE(impai_build, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-naval stock-impai-naval");

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
   empty queue (refused) or at a full one (refused, never accepted). A decision runs at most WAY_SEARCHES class
   searches, a candidate site's classes all at once after the site test has accepted it; a plant with more
   classes than that is refused. A site refused fails for fail_t, so the next decision searches other sites.
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
#define WAY_SEARCHES 8          /* class searches a decision                                              */

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

/* Class c's search (THE WAY OUT) for the plant at first cell (cx, cz), its path added to d->way: 1 proven, 0 refused. */
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

/* THE WAY OUT for a plant of `type` at first cell (cx, cz): 1 proven -- d->way holds it, with no rectangle
   for a plant nothing leaves on the ground -- 0 refused, -1 the decision's searches spent (the caller stops) */
static int way_prove(IMPAI_DEC* d, int type, int cx, int cz)
{
    const IMPAI_TINFO* t = &d->in->ti[type];
    IMPAI_EXITS tmp;
    WAY w;
    unsigned c;
    int grow, classes = 0;
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
    if (d->way_searches + classes > WAY_SEARCHES) return -1;
    d->way_searches += classes;
    grow = WAY_OPEN + w.L->size;
    w.plant = impai_rect_at(d, type, cx, cz);
    w.box.x0 = (int16_t)(cx - grow);
    w.box.z0 = (int16_t)(cz - grow);
    w.box.x1 = (int16_t)(w.plant.x1 + grow);
    w.box.z1 = (int16_t)(w.plant.z1 + grow);
    w.bw = w.box.x1 - w.box.x0 + 1;
    w.bh = w.box.z1 - w.box.z0 + 1;
    if (w.bw > IMPAI_WAY_BOX || w.bh > IMPAI_WAY_BOX) return 0;
    way_raster(d, &w);
    for (c = 0; c < WQ_MCLASSES; c++)
        if ((w.L->classes >> c & 1u) && !way_class(d, &w, t, cx, cz, c)) return 0;
    return d->way_ok = 1;
}

/* the plant's way: the one this decision proved for this site, or proven now. 1 ready, 0 refused, -1 the
   decision's searches spent */
static int way_ready(IMPAI_DEC* d, int type, int cx, int cz)
{
    if (d->way_ok && d->way.type == type && d->way.cx == cx && d->way.cz == cz) return 1;
    return way_prove(d, type, cx, cz);
}

/* THE WAY REFUSALS. A plant site the way out refuses is avoided by plants of that type for fail_t, in a ring of its
   own (IMPAI_BRAIN.wfail), so however many sites the way refuses no failed site of any other kind leaves the shared
   ring (impai_fail_add). A decision refuses at most WAY_SEARCHES ways, and IMPAI_WAY_FAILS holds two plant types'
   WAY_REFUSALS each. */
static int way_failed(const IMPAI_DEC* d, int type, int cx, int cz)
{
    const IMPAI_BRAIN* st = d->in->st;
    int i;
    for (i = 0; i < st->nwfail && i < IMPAI_WAY_FAILS; i++)
        if (st->wfail[i].type == type && st->wfail[i].cx == cx && st->wfail[i].cz == cz && st->wfail[i].until > d->now)
            return 1;
    return 0;
}

/* THE PLANT'S FALLBACK. A plant type the base ring places whose way out is refused WAY_REFUSALS times in a row, or whose base ring
   is exhausted at its widest (ring_grow 8), is passed over by the factory choice for fail_t, which tries the next
   type (impai_econ.c, choose_factory). When every type the choice would take is passed over, the plant goes to the
   first ring site the site test passes, without a way out (impai_build_plant_waived): a base with no factory is worse
   than one with a cramped exit, and the opening factory must not wait the game out. TERMINATION: each refusal is a
   site the way refused, avoided for fail_t, so a type is passed over at most WAY_REFUSALS refusals after its first;
   a decision refuses at most WAY_SEARCHES; and with every type passed over the next factory want ends in the waived
   plant's order, or in no site at all. */
#define WAY_REFUSALS 16

/* the type's entry of the refusals' table, made when absent: a free entry, else the one with the fewest refusals
   and no standing pass */
static IMPAI_PLANTREF* plant_ref(IMPAI_DEC* d, int type)
{
    IMPAI_BRAIN* st = d->in->st;
    IMPAI_PLANTREF* best = NULL;
    int i;
    for (i = 0; i < IMPAI_PLANT_REFS; i++) if (st->pref[i].type == type) return &st->pref[i];
    for (i = 0; i < IMPAI_PLANT_REFS; i++) {
        IMPAI_PLANTREF* r = &st->pref[i];
        const int idle = r->type == WQ_NONE || r->until <= d->now;
        if (idle && (!best || r->refused < best->refused)) best = r;
    }
    if (!best) return NULL;
    memset(best, 0, sizeof *best);
    best->type = (uint16_t)type;
    return best;
}

int impai_plant_off(const IMPAI_DEC* d, int type)
{
    const IMPAI_BRAIN* st = d->in->st;
    int i;
    for (i = 0; i < IMPAI_PLANT_REFS; i++)
        if (st->pref[i].type == type) return st->pref[i].until > d->now;
    return 0;
}

static void plant_pass_over(IMPAI_DEC* d, int type)
{
    IMPAI_PLANTREF* r = plant_ref(d, type);
    if (!r) return;
    r->refused = 0;
    r->until = d->now + d->in->lv->fail_t;
}

static void way_refused(IMPAI_DEC* d, int type, int cx, int cz)
{
    IMPAI_BRAIN* st = d->in->st;
    IMPAI_WAYFAIL* f = &st->wfail[st->wfail_next % IMPAI_WAY_FAILS];
    IMPAI_PLANTREF* r;
    f->cx = (int16_t)cx;
    f->cz = (int16_t)cz;
    f->type = (uint16_t)type;
    f->until = d->now + d->in->lv->fail_t;
    st->wfail_next = (uint8_t)((st->wfail_next + 1u) % IMPAI_WAY_FAILS);
    if (st->nwfail < IMPAI_WAY_FAILS) st->nwfail++;
    d->out->failed++;
    if ((r = plant_ref(d, type)) && ++r->refused >= WAY_REFUSALS) plant_pass_over(d, type);
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

/* A build of `type` at first cell (cx, cz), altitude `alt`: the resolver's build mode 14 for the builder at the
   footprint's centre, (2 origin + size) << 19 (tagpu_placegeo.h), then a NEW command without Shift, which
   replaces the builder's queue and keeps its standing orders (engine/orders.md). 1 when given; 0 when the
   resolver refuses (the site is then skipped as a refused site test) or the decision's commands are spent. */
static int build_at(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int cx, int cz, int alt)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_TINFO* t = &in->ti[type];
    const IMPAI_RECT r = impai_rect_at(d, type, cx, cz);
    IMPAI_JOB* j = &in->job[b->slot];
    WQ_CMD c;
    int a;
    if (d->roots >= in->lv->roots) return 0;
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
    d->roots++;
    if (d->q->ops->order(d->q, &c) != WQ_OK) return 0;
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
        IMPAI_PLANTREF* pr;
        if (!d->way_waive) way_keep(d);
        else {
            d->out->waived = (uint16_t)type;
            d->out->waived_cx = (int16_t)cx;
            d->out->waived_cz = (int16_t)cz;
        }
        if ((pr = plant_ref(d, type))) pr->refused = 0;
    }
    d->pending[t->cat < IMPAI_CATS ? t->cat : IMPAI_C_OTHER]++;
    return 1;
}

/* One order at a target or a point, resolver mode `mode`; the job it becomes. */
static int order_mode(IMPAI_DEC* d, const IMPAI_UNITREF* b, int mode, int target, const int32_t pos[3], int kind,
                      int cx, int cz)
{
    const IMPAI_BRAIN_IN* in = d->in;
    IMPAI_JOB* j = &in->job[b->slot];
    WQ_CMD c;
    int a;
    if (d->roots >= in->lv->roots) return 0;
    if ((a = d->q->ops->can(d->q, b->slot, mode, target, pos)) <= 0) return 0;
    memset(&c, 0, sizeof c);
    c.verb = WQ_CMD_NEW;
    c.action = (uint8_t)a;
    c.unit = b->slot;
    c.target = (uint16_t)target;
    memcpy(c.pos, pos, sizeof c.pos);
    d->roots++;
    if (d->q->ops->order(d->q, &c) != WQ_OK) return 0;
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

/* a patch no claim and no failure touches */
static int patch_free(const IMPAI_DEC* d, const SNAP_PATCH* p)
{
    const IMPAI_RECT r = patch_box(p);
    return !impai_claimed(d, &r) && !impai_failed_in(d, &r);
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

/* ground threat at the 256-px cell of a point, from the seat's maps, built the first time a decision needs them */
static int threatened(IMPAI_DEC* d, int x_px, int z_px)
{
    const IMPAI_VIEW* v = d->in->view;
    int gx, gz;
    if (!d->maps_built) d->maps_built = v && v->threat && impai_maps_build(v, &d->maps) ? 1 : -1;
    if (d->maps_built < 0) return 0;
    gx = x_px / IMPAI_CELL_PX;
    gz = z_px / IMPAI_CELL_PX;
    return gx >= 0 && gz >= 0 && gx < d->maps.gw && gz < d->maps.gh &&
           d->maps.threat[IMPAI_THREAT_GROUND][gz * d->maps.gw + gx] > 0;
}

/* THE SPOT for builder b: of the seat's free patches -- for the commander within comm_r of the anchor, reachable
   on b's class, under no ground threat -- the one with the best cells x 4096 / (4096 + d), d the integer
   distance in px from b to the patch's centre, compared exactly in integers; ties to the lower (az, ax). NULL
   when b has none.
   THE OPENING'S FAR PATCH: before the seat's first factory, while it counts fewer than open_mex extractors, a
   commander with no patch within comm_r takes the best patch at any distance by the same rule. Without it a start
   with no metal within comm_r opens on the commander's own income alone: MEASURED 2026-11-13 on Trout Farm, ARM's
   start at cell (227, 432), whose nearest of the 66 published patches lies 1353 px from the anchor; the commander
   stood idle until open_t, then the factory and its constructors starved at 1.25 metal a pass until about
   GameTime 13 800, and the seat sent no attack before 27 000. Inside the opening the walk is bounded by the
   extractors it orders: open_mex at most, and none once a factory is counted, frames included. */
static const SNAP_PATCH* spot_pick(IMPAI_DEC* d, const IMPAI_UNITREF* b, int range)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int32_t apos[3] = { (int32_t)((uint32_t)(in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(in->st->az * 16 + 8) << 16) };
    const SNAP_PATCH* best = NULL;
    uint32_t best_d = 0;
    unsigned i;
    for (i = 0; i < in->nspots; i++) {
        const SNAP_PATCH* p = &in->spots[i];
        const int px = (p->x0 + p->x1 + 1) * 8, pz = (p->z0 + p->z1 + 1) * 8;
        uint32_t dist;
        if (!p->cells || !patch_free(d, p)) continue;
        {
            const IMPAI_RECT box = patch_box(p);
            if (impai_far_in(d, b, &box)) continue;
        }
        if (range >= 0 && impai_dist_px(apos, px, pz) > (uint32_t)range) continue;
        if (!reachable(d, b, p) || threatened(d, px, pz)) continue;
        dist = impai_dist_px(b->pos, px, pz);
        if (best) {
            const uint64_t mine = (uint64_t)p->cells * (4096u + (uint64_t)best_d);
            const uint64_t theirs = (uint64_t)best->cells * (4096u + (uint64_t)dist);
            if (mine < theirs || (mine == theirs && (p->az > best->az || (p->az == best->az && p->ax >= best->ax))))
                continue;
        }
        best = p;
        best_d = dist;
    }
    return best;
}

const SNAP_PATCH* impai_spot_pick(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const SNAP_PATCH* best;
    if (in->ti[b->type].cat != IMPAI_C_COMMANDER) return spot_pick(d, b, -1);
    if ((best = spot_pick(d, b, in->lv->comm_r))) return best;
    if (d->count[IMPAI_C_FACTORY] + d->pending[IMPAI_C_FACTORY] == 0 &&
        d->count[IMPAI_C_MEX] + d->pending[IMPAI_C_MEX] < in->lv->open_mex)
        return spot_pick(d, b, -1);
    return NULL;
}

/* The extractor on b's spot: the snap places it (tasim_snap_site, the build cursor's scorer: the footprint over
   the most explored metal within the patch's reach) and the site test decides. A patch the snap or the test
   refuses fails for fail_t. */
int impai_build_spot(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const SNAP_PATCH* best = impai_spot_pick(d, b);
    SNAP_CHOICE c;
    int32_t seed[3];
    int radius, alt = 0, w, h;
    uint32_t diag;
    if (!best || d->snaps >= in->lv->snaps) return 0;
    d->snaps++;
    w = best->x1 - best->x0 + 1;
    h = best->z1 - best->z0 + 1;
    diag = impai_isqrt((uint64_t)(w * w + h * h) * 256u);         /* px */
    radius = (int)((32u + diag / 2u + 15u) / 16u);
    if (radius > SNAP_RADIUS_MAX) radius = SNAP_RADIUS_MAX;
    seed[0] = (int32_t)((uint32_t)((best->x0 + best->x1 + 1) * 8) << 16);
    seed[1] = 0;
    seed[2] = (int32_t)((uint32_t)((best->z0 + best->z1 + 1) * 8) << 16);
    memset(&c, 0, sizeof c);
    if (!tasim_snap_site(d->q, NULL, type, seed, seed, radius, 0, 0, &c, in->snap)) {
        impai_fail_add(d, best->ax, best->az);
        return 0;
    }
    if (d->sites >= in->lv->site_tests) return 0;
    d->sites++;
    if (!d->q->ops->site_ok(d->q, type, c.x, c.z, 0, &alt)) {
        impai_fail_add(d, c.x, c.z);
        return 0;
    }
    return build_at(d, b, type, c.x, c.z, alt);
}

/* ---- the base ring ---- */

/* THE RING for energy, factories, makers, and extractors on a metal map (impai_econ.c, metal_map): the base box
   (or the commander's footprint at the anchor) grown on every side by gap + ring_grow x (fx + gap) cells, and
   place_ring's sites of fx + gap cells around it, each origin moved gap / 2 into its cell so that neighbours
   keep gap cells between them. In ring order a site is skipped when it overlaps a claim grown by gap, covers
   explored metal above the threshold (the patches are the extractors'; a metal map has none), or holds a
   failure or the builder's own unreachable site; the site test decides the rest, against the decision's site
   tests, and a plant's site its way out after it (THE WAY OUT), a site refused there avoided by plants of its type
   for fail_t (THE WAY REFUSALS) -- but for a waived plant (THE PLANT'S FALLBACK), which takes the first site the
   site test passes. A ring exhausted with tests left pushes the next decision's ring out (ring_grow, at most 8),
   and a plant type's ring exhausted at 8 passes the type over (THE PLANT'S FALLBACK); a build brings it back to 0. */
int impai_build_ring(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    IMPAI_BRAIN* st = in->st;
    const IMPAI_TINFO* t = &in->ti[type];
    const int gap = in->lv->gap, fx = t->fx > 0 ? t->fx : 1, fz = t->fz > 0 ? t->fz : 1;
    const int grow = gap + st->ring_grow * (fx + gap);
    IMPAI_RECT box;
    unsigned n, k;
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
    for (k = 0; k < n; k++) {
        const int cx = d->ring[k].cx + gap / 2, cz = d->ring[k].cz + gap / 2;
        IMPAI_RECT r = impai_rect_at(d, type, cx, cz), wide = r;
        int alt = 0;
        wide.x0 = (int16_t)(wide.x0 - gap); wide.z0 = (int16_t)(wide.z0 - gap);
        wide.x1 = (int16_t)(wide.x1 + gap); wide.z1 = (int16_t)(wide.z1 + gap);
        if (impai_claimed(d, &wide) || impai_failed_in(d, &r) || impai_far_in(d, b, &r) ||
            (t->cat == IMPAI_C_FACTORY && !d->way_waive && way_failed(d, type, cx, cz)) ||
            d->q->ops->coverage(d->q, cx, cz, fx, fz) > 0) continue;
        if (d->sites >= in->lv->site_tests) return 0;
        d->sites++;
        if (!d->q->ops->site_ok(d->q, type, cx, cz, 0, &alt)) continue;
        if (t->cat == IMPAI_C_FACTORY && !d->way_waive) {
            const int w = way_prove(d, type, cx, cz);
            if (w < 0) return 0;
            if (!w) {
                way_refused(d, type, cx, cz);
                continue;
            }
        }
        if (build_at(d, b, type, cx, cz, alt)) {
            st->ring_grow = 0;
            return 1;
        }
        if (d->roots >= in->lv->roots) return 0;
    }
    if (st->ring_grow < 8) st->ring_grow++;
    else if (t->cat == IMPAI_C_FACTORY && !d->way_waive)
        plant_pass_over(d, type);
    return 0;
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

/* THE SEARCH for a sea plant of `type`, once a decision (the answer is the decision's, impai_build_sea_plant):
   1 with a site the seat's site test accepted in d->sea_cx, sea_cz, sea_alt; 0 none this decision; -1 no
   candidate at all. THE ESTIMATE'S WATER: the distinct sea_ref labels within SEA_EST_BLOCKS of the military's
   estimate (any label with no estimate); none, and a sea army could not come near the enemy: -1. THE
   CANDIDATES: centre blocks in ring order (impai_ring_at) around the anchor's block, each within comm_r of the
   anchor so the commander may build any of them, the footprint's origin at an even cell (fx + 1) / 2 blocks
   across. A CONTENT candidate is one inside the site test's own bounds (cx >= 1, cz >= 1, cx + fx < W, cz + fz
   < H: 0x47D302..0x47D352), whose centre block carries one of the estimate's labels and whose every footprint
   block's top is at most sea - minwaterdepth. Content candidates depend on the level's content and on the type,
   the anchor and the estimate's block only, so a search that finds none is remembered against those
   (IMPAI_BRAIN.sea_none_*) and not walked again while they hold. Each content candidate is then held against
   the claims -- the footprint grown by gap overlapping a building's site or a build order's, which an
   extractor's holds for as long as it stands -- and against the seat's exploration: a footprint centre the
   seat has not explored, which its site test refuses (0x47D3D1..0x47D3FA, the seat's own bit: tagpu_impai.c,
   THE SEAT'S OWN SITE TEST; it tests the centre sheared by half the altitude, this the centre itself, a filter
   the site test still decides), so an unmapped seat looks past the near water it has not seen. Neither spends
   SEA_LOOK, so neither a field of standing extractors nor unseen water can hold the search short of a free
   site. The first SEA_LOOK of the rest are held against the fail ring, and the site test and then the way out
   (THE WAY OUT) decide.
   THE CURSOR. A candidate the search examined and did not take -- the fail ring held it, or the site test, the
   way out or then the resolver refused it -- is passed by the cursor (IMPAI_BRAIN.sea_*), and the next decision's
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
   BOUND, a decision: one walk of at most (2 comm_r / 32 + 1)^2 ring blocks (4225 at comm_r 1024), each one
   label read, at most SEA_LABELS compares and fbx x fbz top reads, and for a content candidate one claims test
   (at most IMPAI_CLAIM_MAX overlaps) and one exploration read; SEA_LOOK failure tests (IMPAI_FAILS each);
   site_tests site tests and WAY_SEARCHES class searches of the way out (THE WAY OUT's bound); impai_sea_labels'
   (2 SEA_EST_BLOCKS + 1)^2 = 9409 label reads. A remembered -1 costs none of it. */
static int sea_search(IMPAI_DEC* d, int type)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const IMPAI_REACH* rc = in->reach;
    IMPAI_BRAIN* st = in->st;
    const IMPAI_TINFO* t = &in->ti[type];
    const int fx = t->fx > 0 ? t->fx : 1, fz = t->fz > 0 ? t->fz : 1, fbx = (fx + 1) / 2, fbz = (fz + 1) / 2;
    const int gap = in->lv->gap, rings = in->lv->comm_r / 32;
    const int ex = in->mil ? in->mil->ebx : IMPAI_NO_ANCHOR, ez = in->mil ? in->mil->ebz : IMPAI_NO_ANCHOR;
    const int32_t apos[3] = { (int32_t)((uint32_t)(st->ax * 16 + 8) << 16), 0, (int32_t)((uint32_t)(st->az * 16 + 8) << 16) };
    uint16_t want[SEA_LABELS];
    WQ_DEF_ECON e;
    int nwant = -1, need, r, i, dx, dz, cands = 0, looked = 0;
    if (!rc || !rc->labels || !rc->top || rc->sea_ref >= WQ_MCLASSES || !(rc->present >> rc->sea_ref & 1u)) return -1;
    if (!d->q->ops->def_econ(d->q, type, &e) || e.minwater <= 0 || (need = (int)d->m.sea - e.minwater) < 0) return -1;
    if (st->sea_none_type == type && st->sea_none_ax == st->ax && st->sea_none_az == st->az &&
        st->sea_none_ex == ex && st->sea_none_ez == ez)
        return -1;
    if (st->sea_cur_type != type || st->sea_cur_ex != ex || st->sea_cur_ez != ez ||
        d->now - st->sea_from >= in->lv->fail_t) {
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
            if (impai_dist_px(apos, bx * 32 + 16, bz * 32 + 16) > in->lv->comm_r) continue;
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
            if (looked >= SEA_LOOK) return 0;
            looked++;
            if (!impai_failed_in(d, &site)) {
                if (d->sites >= in->lv->site_tests) return 0;
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
    return -1;
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
   NO BUILDER REACH TEST: a builder that cannot get to the site fails its job, which counts. Testing it here
   would answer 0 to a builder that cannot reach while a candidate exists, and a seat whose only sea-plant
   builders cannot reach would hold its second factory until the count ran out. */
int impai_build_sea_plant(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type)
{
    if (d->sea_type == WQ_NONE) {
        d->sea_type = (uint16_t)type;
        d->sea_answer = sea_search(d, type);
    } else if (d->sea_type != type)
        return 0;
    if (d->sea_answer < 0) return -1;
    if (d->sea_answer > 0) {
        const IMPAI_RECT site = impai_rect_at(d, d->sea_type, d->sea_cx, d->sea_cz);
        if (impai_far_in(d, b, &site)) {
            /* the site stays the decision's for a builder that can reach it; this one's turn counts a refusal */
            impai_sea_refusal(d);
            return d->in->st->sea_refused >= IMPAI_SEA_REFUSALS ? -1 : 0;
        }
        if (build_at(d, b, d->sea_type, d->sea_cx, d->sea_cz, d->sea_alt)) {
            d->out->sea_plants++;
            return 1;
        }
        /* the resolver refused, a refusal like the site test's, which the cursor passes (the search left it at
           the site); with the roots spent nothing was asked, and the site stays the decision's */
        if (d->roots < d->in->lv->roots) {
            d->in->st->sea_i++;
            d->sea_answer = 0;
        }
    }
    impai_sea_refusal(d);
    return d->in->st->sea_refused >= IMPAI_SEA_REFUSALS ? -1 : 0;
}

/* ---- exploring ---- */

/* THE EXPLORE CELL, once a decision: of the 256-px cells, the one nearest the anchor (integer distance, ties
   in row-major order) whose centre cell the seat has not explored, that holds no failure and that b can travel
   into on its class: its centre block carries b's label, else a block within EXPLORE_NEAR blocks of it does
   (impai_reach_near, ring order), so a cell whose centre is a cliff or a rock is still explored, at that block
   (a sea constructor explores water, a land one its own island, so neither holds the seat's one explorer on a
   cell it never arrives at). b moves there (resolver mode 2), at the ground's height, and becomes the seat's one
   explorer; the job and its failure keep the cell's centre. BOUND: at most IMPAI_GRID_MAX^2 cells, each one
   label read, and for a cell nearer than the best so far whose centre block is not b's, at most
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
            if (l > 0 && impai_reach_at(in->reach, cls, cx >> 1, cz >> 1) != l) {
                if (!impai_reach_near(in->reach, cls, (uint16_t)l, cx >> 1, cz >> 1, EXPLORE_NEAR, &ox, &oz)) continue;
                px = ox * 32 + 16;
                pz = oz * 32 + 16;
            }
            best = dd; bx = cx; bz = cz; tx = px; tz = pz;
        }
    if (bx < 0) return 0;
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

/* A builder b can come near enough to help a building of `type` at (x, z) px: some block within R = min(its
   builddistance + half the building's larger side, NEAR_PX_MAX) px of the building's centre holds b's own
   component (impai_reach_near), so a sea constructor helps a shipyard and not a plant inland. No test as
   builder_label says. BOUND: at most (2 NEAR_PX_MAX / 32 + 1)^2 = 4225 label reads. */
#define NEAR_PX_MAX 1024
static int builder_near(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type, int32_t x, int32_t z)
{
    const int l = builder_label(d, b);
    WQ_DEF def;
    int32_t rpx;
    int f, ox, oz;
    if (l < 0) return 1;
    if (!l) return 0;
    f = type > 0 && (unsigned)type < d->in->nti ? (d->in->ti[type].fx > d->in->ti[type].fz ? d->in->ti[type].fx
                                                                                          : d->in->ti[type].fz) : 0;
    rpx = (d->q->ops->def(d->q, b->type, &def) ? def.reach : 0) + 8 * (f > 0 ? f : 0);
    if (rpx > NEAR_PX_MAX) rpx = NEAR_PX_MAX;
    return impai_reach_near(d->in->reach, d->in->ti[b->type].mclass, (uint16_t)l, x >> 5, z >> 5, rpx / 32, &ox, &oz);
}

/* THE ASSIST: the nearest unfinished building of the seat's, helped with resolver mode 8 -- which tests no
   alliance, so the census's own-units-only list is the test -- else the nearest finished factory of the
   seat's that is busy, guarded (mode 7), each one b can come near enough to (builder_near). The commander helps
   only within comm_r of the anchor. Ties go to the lower slot. BOUND: builder_near's reads for at most
   IMPAI_NANO_MAX + IMPAI_IDLE_CAP targets. */
int impai_build_assist(IMPAI_DEC* d, const IMPAI_UNITREF* b)
{
    const IMPAI_BRAIN_IN* in = d->in;
    const int commander = in->ti[b->type].cat == IMPAI_C_COMMANDER;
    const int32_t apos[3] = { (int32_t)((uint32_t)(in->st->ax * 16 + 8) << 16), 0,
                              (int32_t)((uint32_t)(in->st->az * 16 + 8) << 16) };
    const IMPAI_UNITREF* best = NULL;
    uint32_t best_d = 0;
    int i, pass;
    for (pass = 0; pass < 2 && !best; pass++) {
        const IMPAI_UNITREF* list = pass ? d->fbusy : d->nano;
        const int n = pass ? d->nfbusy : d->nnano;
        for (i = 0; i < n; i++) {
            const uint32_t dist = impai_dist_px(b->pos, list[i].pos[0] >> 16, list[i].pos[2] >> 16);
            if (list[i].slot == b->slot) continue;
            if (commander && impai_dist_px(apos, list[i].pos[0] >> 16, list[i].pos[2] >> 16) > in->lv->comm_r) continue;
            if (best && dist >= best_d) continue;
            if (!builder_near(d, b, list[i].type, list[i].pos[0] >> 16, list[i].pos[2] >> 16)) continue;
            best = &list[i];
            best_d = dist;
        }
        if (best) return order_mode(d, b, pass ? 7 : 8, best->slot, best->pos, IMPAI_JOB_ASSIST, 0, 0);
    }
    return 0;
}
