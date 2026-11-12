/* tasim_move.c -- the shared simulation core's movement: bodies, the step rule, the straight walk and the A*
   planner, and the targets a unit's orders walk to. research/notes/impai/shared-core.md, tasim_move. Contract:
   tasim.h.

   The planner's scratch and the bodies' are the WIKISIM's (tagpu_wikisim_init sizes the planner's for the map), so
   a walk allocates nothing per step but what tasim_grow adds once; no globals. Every distance is a double rounded
   once per operation, the planner's ties broken by push order, so every build walks the same path. */
#include <math.h>
#include <string.h>
#include "tasim.h"

#define FOLLOW  48

/* ---- MOVEMENT (research/notes/ingame-wiki.md, *How units move*, which says why each
   rule is so. A mobile unit's body is its footprint's box about its centre, a structure's its
   footprint, and a builder within build distance of a site it has not started holds the site as a
   claim. Bodies overlap only with area in common. THE STEP RULE: a step may not make its unit
   overlap a body it does not overlap already, nor take its body off the map. A unit walks straight
   at its target while the line is clear of every still body (a structure, a claim, a unit that
   did not step on the last tick), else one leg at a time along the planner's path round every
   body; a refused step turns 45 and then 90 degrees either way. ---- */

enum { REFUSED_NONE = -1, REFUSED_EDGE = -2 };
#define C45  0.70710678118654752440     /* Math.SQRT1_2, the double both round it to */
#define DIAG (CELL * 1.41421356237309504880)   /* CELL * Math.SQRT2 */
/* a step's tries, (cos, sin) of its turn: straight on, 45 degrees either way, then 90 */
static const double TURNS[5][2] = { { 1, 0 }, { C45, C45 }, { C45, -C45 }, { 0, 1 }, { 0, -1 } };
/* the planner's neighbours: the four sides, then the four corners */
static const int DIRS[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

static int meets(WIKIRECT a, WIKIRECT b) { return a.x0 <= b.x1 && b.x0 <= a.x1 && a.z0 <= b.z1 && b.z0 <= a.z1; }
static int same_rect(WIKIRECT a, WIKIRECT b) { return a.x0 == b.x0 && a.z0 == b.z0 && a.x1 == b.x1 && a.z1 == b.z1; }
static int on_map(const WIKISIM* s, WIKIRECT r) { return r.x0 >= 0 && r.z0 >= 0 && r.x1 <= s->W * CELL && r.z1 <= s->H * CELL; }

static WIKIRECT rect(double x0, double z0, double x1, double z1)
{
    WIKIRECT r;
    r.x0 = x0;
    r.z0 = z0;
    r.x1 = x1;
    r.z1 = z1;
    return r;
}

WIKIRECT tasim_rect_of(const WIKISIM* s, int t, int ox, int oz)
{
    return rect(ox * CELL, oz * CELL, (ox + tasim_fpx(s, t)) * CELL, (oz + tasim_fpz(s, t)) * CELL);
}

WIKIRECT tasim_body_at_type(const WIKISIM* s, int t, double x, double z)
{
    const double ax = tasim_fpx(s, t) * CELL / 2.0, az = tasim_fpz(s, t) * CELL / 2.0;
    return rect(x - ax, z - az, x + ax, z + az);
}

static WIKIRECT body_at(const WIKISIM* s, const WIKIUNIT* u, double x, double z) { return tasim_body_at_type(s, u->type, x, z); }

WIKIRECT tasim_body(const WIKISIM* s, const WIKIUNIT* v)
{
    return tasim_mobile(s, v->type) ? body_at(s, v, v->x, v->z) : tasim_rect_of(s, v->type, v->ox, v->oz);
}

/* the distance from (x, z) to r, 0 inside it */
static double rect_dist(double x, double z, WIKIRECT r)
{
    double dx = r.x0 - x, dz = r.z0 - z;
    const double ex = x - r.x1, ez = z - r.z1;
    if (dx < 0) dx = 0;
    if (ex > dx) dx = ex;
    if (dz < 0) dz = 0;
    if (ez > dz) dz = ez;
    return sqrt(dx * dx + dz * dz);
}

/* the gap between rects a and b, 0 when they touch or overlap */
static double rect_gap(WIKIRECT a, WIKIRECT b)
{
    double dx = b.x0 - a.x1, dz = b.z0 - a.z1;
    const double ex = a.x0 - b.x1, ez = a.z0 - b.z1;
    if (dx < 0) dx = 0;
    if (ex > dx) dx = ex;
    if (dz < 0) dz = 0;
    if (ez > dz) dz = ez;
    return sqrt(dx * dx + dz * dz);
}

/* the site u's first order claims: a build not started yet that u has reached --
   its centre within build distance of the site, or its body within a step of it, which is how a
   builder whose BuildDistance is under its half width arrives (touching the site, or stopped short
   by the site refusing its step); unclaimed there, what stands on the site would never leave it.
   0 when none */
static int claim(const WIKISIM* s, const WIKIUNIT* u, WIKIRECT* out)
{
    const WIKIORDER* o = u->norder ? &u->order[0] : NULL;
    if (!o || o->k != WIKI_ORDER_BUILD || tasim_started(s, o)) return 0;
    *out = tasim_rect_of(s, o->type, o->ox, o->oz);
    return rect_dist(u->x, u->z, *out) <= tasim_def(s, u->type)->build_distance
        || rect_gap(tasim_body(s, u), *out) <= tasim_def(s, u->type)->max_velocity;
}

/* a feature's footprint in world units */
WIKIRECT tasim_feat_rect(const WIKIFEAT* f)
{
    return rect(f->ox * CELL, f->oz * CELL, (f->ox + f->fx) * CELL, (f->oz + f->fz) * CELL);
}

/* every body but u's into s->obs: the units' in creation order, then the blocking features', then
   the claims in their claimants' order; their count, or -1 when the scratch cannot grow. It holds
   one more, the site tasim_go adds. */
static int bodies(WIKISIM* s, const WIKIUNIT* u)
{
    int i, n = 0;
    WIKIRECT r;
    if (!tasim_grow(s, (void**)&s->obs, &s->cobs, 2 * s->nunit + s->nfeat + 1, sizeof *s->obs)) return -1;
    for (i = 0; i < s->nunit; i++)
        if (s->unit[i] != u) {
            s->obs[n].r = tasim_body(s, s->unit[i]);
            s->obs[n++].u = s->unit[i];
        }
    for (i = 0; i < s->nfeat; i++)
        if (s->feat[i].blocking) {
            s->obs[n].r = tasim_feat_rect(&s->feat[i]);
            s->obs[n++].u = NULL;
        }
    for (i = 0; i < s->nunit; i++)
        if (tasim_mobile(s, s->unit[i]->type) && claim(s, s->unit[i], &r)) {
            s->obs[n].r = r;
            s->obs[n++].u = NULL;
        }
    return n;
}

static int still(const WIKISIM* s, const WIKIBODY* b) { return !b->u || !tasim_mobile(s, b->u->type) || !b->u->went; }

TARGET tasim_to_point(const WIKIORDER* o, double x, double z)
{
    TARGET t;
    memset(&t, 0, sizeof t);
    t.x = x;
    t.z = z;
    t.r = rect(x, z, x, z);
    t.plan = CELL;
    t.serial = o->serial;
    t.kind = TARGET_POINT;
    return t;
}

TARGET tasim_to_site(const WIKISIM* s, const WIKIORDER* o, const WIKIUNIT* u, WIKIRECT r)
{
    TARGET t;
    memset(&t, 0, sizeof t);
    t.x = (r.x0 + r.x1) / 2;
    t.z = (r.z0 + r.z1) / 2;
    t.r = r;
    t.stop = t.plan = tasim_def(s, u->type)->build_distance;
    t.body = r;
    t.has_body = 1;
    t.serial = o->serial;
    t.kind = TARGET_SITE;
    return t;
}

TARGET tasim_to_unit(const WIKISIM* s, const WIKIORDER* o, const WIKIUNIT* g)
{
    TARGET t;
    memset(&t, 0, sizeof t);
    t.x = g->x;
    t.z = g->z;
    t.r = rect(g->x, g->z, g->x, g->z);
    t.stop = t.clip = t.plan = FOLLOW;
    t.body = tasim_body(s, g);
    t.has_body = 1;
    t.serial = o->serial;
    t.kind = TARGET_UNIT;
    return t;
}

/* a feature's Reclaim walks until the footprints touch: its goal 0x44D8A0 is the ring of cells round
   the feature's footprint widened by the unit's, which 0x44DCB0 passes when the unit's cell lies on
   it; BuildDistance is not read */
TARGET tasim_to_feat(const WIKIORDER* o, const WIKIFEAT* f)
{
    TARGET t;
    memset(&t, 0, sizeof t);
    t.x = f->x;
    t.z = f->z;
    t.r = t.body = tasim_feat_rect(f);
    t.has_body = 1;
    t.plan = CELL;
    t.serial = o->serial;
    t.kind = TARGET_SITE;
    return t;
}

static int is_target(const WIKIBODY* b, const TARGET* t) { return t->has_body && same_rect(b->r, t->body); }

static int in_range(double x, double z, WIKIRECT b, const TARGET* t, double within)
{
    return rect_dist(x, z, t->r) <= within || (t->has_body && meets(b, t->body));
}

/* THE STEP RULE: the body u's step to (x, z) would newly overlap, its index in s->obs, the first,
   or REFUSED_EDGE; REFUSED_NONE when none */
static int refuses(const WIKISIM* s, const WIKIUNIT* u, double x, double z, int nobs)
{
    const WIKIRECT a = body_at(s, u, x, z), now = tasim_body(s, u);
    int k;
    if (!on_map(s, a) && on_map(s, now)) return REFUSED_EDGE;
    for (k = 0; k < nobs; k++)
        if (tasim_hits(a, s->obs[k].r) && !tasim_hits(now, s->obs[k].r)) return k;
    return REFUSED_NONE;
}

/* u steps `len` along the unit vector (hx, hz) -- to (ex, ez) exactly when `exact`, the end of a
   path's leg -- or along the first turn of it the step rule lets go. GO_ARRIVED, with no step,
   when what refuses the straight step is the target itself; GO_WAITING when every try is refused.
   u->turned says whether a turn went. */
static int step(const WIKISIM* s, WIKIUNIT* u, double hx, double hz, double len, int nobs, const TARGET* t,
                int exact, double ex, double ez)
{
    int k;
    for (k = 0; k < 5; k++) {
        const double c = TURNS[k][0], sn = TURNS[k][1];
        const double dx = k ? hx * c - hz * sn : hx, dz = k ? hx * sn + hz * c : hz;
        const double x = !k && exact ? ex : u->x + dx * len, z = !k && exact ? ez : u->z + dz * len;
        const int r = refuses(s, u, x, z, nobs);
        if (r == REFUSED_NONE) {
            u->x = x;
            u->z = z;
            u->hx = dx;
            u->hz = dz;
            u->stepped = 1;
            u->turned = k > 0;
            return GO_STEPPED;
        }
        if (!k && t && r >= 0 && is_target(&s->obs[r], t)) return GO_ARRIVED;
    }
    return GO_WAITING;
}

/* the segment from (x, z) to (tx, tz) passes through the inside of r */
static int crosses(double x, double z, double tx, double tz, WIKIRECT r)
{
    const double p[2] = { x, z }, d[2] = { tx - x, tz - z }, lo[2] = { r.x0, r.z0 }, hi[2] = { r.x1, r.z1 };
    double t0 = 0, t1 = 1;
    int k;
    for (k = 0; k < 2; k++) {
        double a, b;
        if (d[k] == 0) {
            if (!(lo[k] < p[k] && p[k] < hi[k])) return 0;
            continue;
        }
        a = (lo[k] - p[k]) / d[k];
        b = (hi[k] - p[k]) / d[k];
        if (a > b) {
            const double c = a;
            a = b;
            b = c;
        }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
    }
    return t0 < t1;
}

/* u's body can slide straight to the target's point past every still body but the target's own
   and those it overlaps already, and ends on the map */
static int clear(const WIKISIM* s, const WIKIUNIT* u, const TARGET* t, int nobs)
{
    const WIKIRECT now = tasim_body(s, u);
    const double ax = tasim_fpx(s, u->type) * CELL / 2.0, az = tasim_fpz(s, u->type) * CELL / 2.0;
    int k;
    if (on_map(s, now) && !on_map(s, body_at(s, u, t->x, t->z))) return 0;
    for (k = 0; k < nobs; k++) {
        const WIKIBODY* b = &s->obs[k];
        if (!still(s, b) || is_target(b, t) || tasim_hits(now, b->r)) continue;
        if (crosses(u->x, u->z, t->x, t->z, rect(b->r.x0 - ax, b->r.z0 - az, b->r.x1 + ax, b->r.z1 + az))) return 0;
    }
    return 1;
}

/* the planner's heap: by f, then by push, a total order, so every build pops alike */
static int heap_less(const WIKIHEAPNODE* a, const WIKIHEAPNODE* b) { return a->f < b->f || (a->f == b->f && a->seq < b->seq); }

static void heap_push(WIKISIM* s, double f, int n)
{
    WIKIHEAPNODE* h = s->heap;
    int k = s->nheap++;
    h[k].f = f;
    h[k].seq = s->heap_seq++;
    h[k].n = n;
    while (k > 0) {
        const int p = (k - 1) >> 1;
        WIKIHEAPNODE x;
        if (!heap_less(&h[k], &h[p])) break;
        x = h[k];
        h[k] = h[p];
        h[p] = x;
        k = p;
    }
}

static int heap_pop(WIKISIM* s)
{
    WIKIHEAPNODE* h = s->heap;
    const int top = h[0].n;
    int k = 0;
    h[0] = h[--s->nheap];
    for (;;) {
        const int l = 2 * k + 1, r = l + 1;
        int m = k;
        WIKIHEAPNODE x;
        if (l < s->nheap && heap_less(&h[l], &h[m])) m = l;
        if (r < s->nheap && heap_less(&h[r], &h[m])) m = r;
        if (m == k) break;
        x = h[k];
        h[k] = h[m];
        h[m] = x;
        k = m;
    }
    return top;
}

/* a planner node is free: no cell of it held and no mover's body over it */
static int node_free(WIKISIM* s, int n, int NW, int fx, int fz, int nmover)
{
    if (!s->known[n]) {
        const int i = n % NW, j = n / NW;
        const WIKIRECT a = rect(i * CELL, j * CELL, (i + fx) * CELL, (j + fz) * CELL);
        int ok = 1, di, dj, k;
        for (dj = 0; dj < fz && ok; dj++)
            for (di = 0; di < fx && ok; di++)
                if (s->occ[(j + dj) * s->W + i + di]) ok = 0;
        for (k = 0; k < nmover && ok; k++)
            if (tasim_hits(a, s->mover[k])) ok = 0;
        s->known[n] = ok ? 1 : 2;
    }
    return s->known[n] == 1;
}

static double plan_h(double x, double z, const TARGET* t)
{
    const double h = rect_dist(x, z, t->r) - t->plan;
    return h > 0 ? h : 0;
}

/* THE PLANNER: A* over the map's cells, node (i, j) standing u's body on cells
   i..i+fx-1, j..j+fz-1. The node to head for into (*wx, *wz), 1; or 0: no nearer place, or no
   scratch. Its bounds: W x H nodes, each pushed once from a seed and at most once from each of its
   eight neighbours, so the heap holds at most 8 x W x H + 4 (tagpu_wikisim_init sizes it). */
static int plan(WIKISIM* s, const WIKIUNIT* u, const TARGET* t, int nobs, double* wx, double* wz)
{
    const int fx = tasim_fpx(s, u->type), fz = tasim_fpz(s, u->type), NW = s->W - fx + 1, NH = s->H - fz + 1;
    const WIKIRECT now = tasim_body(s, u);
    int k, i, j, n, nmover = 0, end = -1, best = -1, next = -1, i0, j0, i1, j1;
    double bestd = 0, a, b;
    if (NW < 1 || NH < 1 || !s->heap || !tasim_grow(s, (void**)&s->mover, &s->cmover, nobs, sizeof *s->mover)) return 0;
    memset(s->occ, 0, (size_t)s->W * s->H);
    for (k = 0; k < nobs; k++) {
        const WIKIBODY* o = &s->obs[k];
        int x0, z0, x1, z1;
        if (tasim_hits(now, o->r)) continue;
        if (o->u && tasim_mobile(s, o->u->type)) {
            s->mover[nmover++] = o->r;
            continue;
        }
        /* a structure's or a claim's rect is whole cells */
        x0 = (int)(o->r.x0 / CELL);
        z0 = (int)(o->r.z0 / CELL);
        x1 = (int)(o->r.x1 / CELL);
        z1 = (int)(o->r.z1 / CELL);
        for (j = z0 > 0 ? z0 : 0; j < (z1 < s->H ? z1 : s->H); j++)
            for (i = x0 > 0 ? x0 : 0; i < (x1 < s->W ? x1 : s->W); i++) s->occ[j * s->W + i] = 1;
    }
    for (n = 0; n < NW * NH; n++) {
        s->pg[n] = INFINITY;
        s->par[n] = -1;
        s->shut[n] = 0;
        s->known[n] = 0;
    }
    s->nheap = 0;
    s->heap_seq = 0;
    a = u->x / CELL - fx / 2.0;
    b = u->z / CELL - fz / 2.0;
    i0 = (int)floor(a);
    j0 = (int)floor(b);
    i1 = (int)ceil(a);
    j1 = (int)ceil(b);
    for (j = j0; j <= j1; j++)
        for (i = i0; i <= i1; i++) {
            double dx, dz, d;
            if (i < 0 || j < 0 || i >= NW || j >= NH || !node_free(s, j * NW + i, NW, fx, fz, nmover)) continue;
            dx = (i + fx / 2.0) * CELL - u->x;
            dz = (j + fz / 2.0) * CELL - u->z;
            d = sqrt(dx * dx + dz * dz);
            s->pg[j * NW + i] = d;
            heap_push(s, d + plan_h((i + fx / 2.0) * CELL, (j + fz / 2.0) * CELL, t), j * NW + i);
        }
    while (s->nheap) {
        double cx, cz, e;
        n = heap_pop(s);
        if (s->shut[n]) continue;
        s->shut[n] = 1;
        i = n % NW;
        j = n / NW;
        cx = (i + fx / 2.0) * CELL;
        cz = (j + fz / 2.0) * CELL;
        if (in_range(cx, cz, rect(i * CELL, j * CELL, (i + fx) * CELL, (j + fz) * CELL), t, t->plan)) {
            end = n;
            break;
        }
        e = rect_dist(cx, cz, t->r);
        if (best < 0 || e < bestd) {
            best = n;
            bestd = e;
        }
        for (k = 0; k < 8; k++) {
            const int di = DIRS[k][0], dj = DIRS[k][1], ni = i + di, nj = j + dj, m = nj * NW + ni;
            double c;
            if (ni < 0 || nj < 0 || ni >= NW || nj >= NH || s->shut[m] || !node_free(s, m, NW, fx, fz, nmover)) continue;
            if (di && dj && (!node_free(s, j * NW + ni, NW, fx, fz, nmover) || !node_free(s, nj * NW + i, NW, fx, fz, nmover)))
                continue;
            c = s->pg[n] + (di && dj ? DIAG : CELL);
            if (c < s->pg[m]) {
                s->pg[m] = c;
                s->par[m] = n;
                heap_push(s, c + plan_h((ni + fx / 2.0) * CELL, (nj + fz / 2.0) * CELL, t), m);
            }
        }
    }
    if (end < 0) {
        if (best < 0 || !(bestd < rect_dist(u->x, u->z, t->r))) return 0;
        end = best;
    }
    /* back to the seed; past it when u stands on it */
    for (n = end; s->par[n] >= 0; n = s->par[n]) next = n;
    if (s->pg[n] == 0) n = next;
    if (n < 0) return 0;
    *wx = (n % NW + fx / 2.0) * CELL;
    *wz = (n / NW + fz / 2.0) * CELL;
    return 1;
}

/* u's leg is still its to walk: the same order and target, its node covered by no body u is not
   on already */
static int leg_holds(const WIKISIM* s, const WIKIUNIT* u, const TARGET* t, int nobs)
{
    const WIKIRECT now = tasim_body(s, u), a = body_at(s, u, u->leg_x, u->leg_z);
    int k;
    if (!u->leg || u->leg_serial != t->serial || u->leg_kind != t->kind) return 0;
    for (k = 0; k < nobs; k++)
        if (tasim_hits(a, s->obs[k].r) && !tasim_hits(now, s->obs[k].r)) return 0;
    return 1;
}

/* u's turn toward t: GO_ARRIVED in range, else GO_STEPPED, GO_WAITING (its tries refused) or
   GO_STUCK (no nearer place) */
int tasim_go(WIKISIM* s, WIKIUNIT* u, const TARGET* t)
{
    const double v = tasim_def(s, u->type)->max_velocity;
    double dx, dz, d;
    int nobs, r;
    if (in_range(u->x, u->z, tasim_body(s, u), t, t->stop)) {
        u->leg = 0;
        return GO_ARRIVED;
    }
    if ((nobs = bodies(s, u)) < 0) return GO_WAITING;
    /* the site is a body to the unit heading for it, started, claimed or not: its straight step
       stops at the site's edge and is GO_ARRIVED, so a builder whose BuildDistance is shorter
       than its half width does not walk onto it (claim says how it then claims the site) */
    if (t->kind == TARGET_SITE) {
        s->obs[nobs].r = t->body;
        s->obs[nobs++].u = NULL;
    }
    if (clear(s, u, t, nobs)) {
        u->leg = 0;
        dx = t->x - u->x;
        dz = t->z - u->z;
        d = sqrt(dx * dx + dz * dz);
        return step(s, u, dx / d, dz / d, v < d - t->clip ? v : d - t->clip, nobs, t, 0, 0, 0);
    }
    if (!leg_holds(s, u, t, nobs)) {
        double wx, wz;
        if (!plan(s, u, t, nobs, &wx, &wz)) {
            u->leg = 0;
            return GO_STUCK;
        }
        u->leg = 1;
        u->leg_x = wx;
        u->leg_z = wz;
        u->leg_serial = t->serial;
        u->leg_kind = t->kind;
    }
    dx = u->leg_x - u->x;
    dz = u->leg_z - u->z;
    d = sqrt(dx * dx + dz * dz);
    r = d <= v ? step(s, u, dx / d, dz / d, d, nobs, t, 1, u->leg_x, u->leg_z)
               : step(s, u, dx / d, dz / d, v, nobs, t, 0, 0, 0);
    if (r != GO_STEPPED || u->turned || (u->x == u->leg_x && u->z == u->leg_z)) u->leg = 0;
    return r;
}

/* A unit on a structure or a claim steps straight away from its centre (the first such in
   bodies' order), along (0, 1) from the centre itself, and does nothing else on its turn; 1 when
   it was on one */
int tasim_evacuate(WIKISIM* s, WIKIUNIT* u)
{
    const WIKIRECT now = tasim_body(s, u);
    const int nobs = bodies(s, u);
    int k;
    for (k = 0; k < nobs; k++) {
        const WIKIBODY* b = &s->obs[k];
        double dx, dz, d;
        if ((b->u && tasim_mobile(s, b->u->type)) || !tasim_hits(now, b->r)) continue;
        dx = u->x - (b->r.x0 + b->r.x1) / 2;
        dz = u->z - (b->r.z0 + b->r.z1) / 2;
        d = sqrt(dx * dx + dz * dz);
        u->leg = 0;
        if (d > 0) step(s, u, dx / d, dz / d, tasim_def(s, u->type)->max_velocity, nobs, NULL, 0, 0, 0);
        else step(s, u, 0, 1, tasim_def(s, u->type)->max_velocity, nobs, NULL, 0, 0, 0);
        return 1;
    }
    return 0;
}

/* toward a footprint until within build distance of it; 1 once in range */
int tasim_reach(WIKISIM* s, const WIKIORDER* o, WIKIUNIT* u, int t, int ox, int oz)
{
    const TARGET g = tasim_to_site(s, o, u, tasim_rect_of(s, t, ox, oz));
    return tasim_go(s, u, &g) == GO_ARRIVED;
}
