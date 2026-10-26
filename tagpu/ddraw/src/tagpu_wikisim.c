/* tagpu_wikisim.c -- the in-game wiki's simplified simulation. Contract: tagpu_wikisim.h.

   The sections and the functions follow tools/wiki/mockup/sim.js in its order and under its names
   in C's spelling (siteOk is site_ok), so that a change to one is found in the other. Where a JS
   value can be undefined, a C value that every comparison treats alike stands in for it: NaN for
   a metal value read off the map, -1 for no vent and for no building armed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_wikisim.h"

#define CELL    WIKI_CELL
#define TPS     WIKI_TPS
#define RADIUS  3
#define ARENA   64
#define DRAG    4
#define FOLLOW  48
#define SPACING 24
/* tagpu_place.c's caps */
#define PLACE_ORDER_BUDGET 4096
#define PLACE_LIST_CAP     1024
/* a cell far enough off any map for every test to refuse it, and small enough that no cell
   arithmetic below can overflow: a wild cursor answers as off the map */
#define CELL_BOUND (1 << 20)

enum { KIND_NONE, KIND_MEX, KIND_GEO };
enum { HOW_BUILD, HOW_RECLAIM };

/* quickChoice's and site's answer, a candidate building: sim.js's `c` */
typedef struct {
    int    type, kind, ox, oz;
    char   target[WIKI_ID_MAX];     /* an upgrade: the building it replaces; "" otherwise */
    int    anchored, has_anchor, ax, az;    /* distinct's answer, once asked */
    double strength;
    int    queued;
} CHOICE;

static double sq(double v) { return v * v; }

/* Math.round: half rounds up. x - floor(x) is exact wherever it can fall on either side of 0.5,
   so this agrees with it for every double. */
static double js_round(double x)
{
    double r = floor(x);
    return x - r >= 0.5 ? r + 1 : r;
}

static int bound_cell(double c)
{
    return c < -CELL_BOUND ? -CELL_BOUND : c > CELL_BOUND ? CELL_BOUND : (int)c;
}

/* ---- the shapes are tagpu_placegeo.c's own, called directly; sim.js ports them line for line ---- */

/* the footprint's first cell for a centre at world v: 0x4197D0's rounding, floor((v - 8f + 8) / 16), half up */
static int origin(double v, int f) { return bound_cell(floor((v - 8 * f + 8) / CELL)); }

typedef struct { double x, z; } XZ;

/* 0x43AFC0's predicate: within 16 world units of the node on both axes, inclusive */
static int within(XZ p, XZ q) { return fabs(p.x - q.x) <= 16 && fabs(p.z - q.z) <= 16; }

/* ---- definitions ---- */

static const WIKIDEF* def(const WIKISIM* s, int t) { return &s->entry->def[t]; }
static int fpx(const WIKISIM* s, int t) { return def(s, t)->fx; }
static int fpz(const WIKISIM* s, int t) { return def(s, t)->fz; }
static int mobile(const WIKISIM* s, int t) { return def(s, t)->max_velocity > 0; }
/* a resource building: a mex extracts metal, a geothermal plant needs a vent under its yard */
static int kind(const WIKISIM* s, int t) { return def(s, t)->extracts > 0 ? KIND_MEX : def(s, t)->vent ? KIND_GEO : KIND_NONE; }
/* mirrors tagpu_resource.c strength: ExtractsMetal for a mex, EnergyMake otherwise */
static double strength(const WIKISIM* s, int t) { return kind(s, t) == KIND_MEX ? def(s, t)->extracts : def(s, t)->energy_make; }
static int can_build(const WIKISIM* s, const WIKIUNIT* u, int t) { return (int)((def(s, u->type)->builds >> t) & 1); }

/* ---- the grid ---- */

static int cell_of(double v) { return bound_cell(floor(v / CELL)); }

/* A cell's flat index as the JS arrays take it, -1 where the array has no element: JS reads
   undefined there, and an index aliasing into the next row reads that row, as it does here. */
static int flat(const WIKISIM* s, int cx, int cz)
{
    long k = (long)cz * s->W + cx;
    return k >= 0 && k < (long)s->W * s->H ? (int)k : -1;
}

static int seen(const WIKISIM* s, int cx, int cz)
{
    int k = flat(s, cx, cz);
    return k < 0 || s->fog[k] != 2;
}

typedef struct { int ox, oz, fx, fz; } BOX;

static int overlap(BOX a, BOX b) { return a.ox < b.ox + b.fx && b.ox < a.ox + a.fx && a.oz < b.oz + b.fz && b.oz < a.oz + a.fz; }

static BOX box(const WIKISIM* s, int t, int ox, int oz)
{
    BOX b;
    b.ox = ox;
    b.oz = oz;
    b.fx = fpx(s, t);
    b.fz = fpz(s, t);
    return b;
}

/* ---- units: each one on the heap, so a pointer to it holds while the list grows ---- */

/* THE ONE APPEND to a unit's orders, so a list never holds more than PLACE_LIST_CAP: an order
   past it is dropped, as sim.js's append drops it. Every order a player's clicks add comes
   through here, so they cannot grow a list until realloc fails. 1 when the order went in; 0
   when the list was full or memory ran out, which sets `oom`. */
static int push_order(WIKISIM* s, WIKIUNIT* u, const WIKIORDER* o)
{
    if (u->norder >= PLACE_LIST_CAP) return 0;
    if (u->norder == u->corder) {
        int c = u->corder ? u->corder * 2 : 8;
        WIKIORDER* p = (WIKIORDER*)realloc(u->order, (size_t)c * sizeof *p);
        if (!p) { s->oom = 1; return 0; }
        u->order = p;
        u->corder = c;
    }
    u->order[u->norder++] = *o;
    return 1;
}

static void splice_orders(WIKIUNIT* u, int i, int n)
{
    memmove(u->order + i, u->order + i + n, (size_t)(u->norder - i - n) * sizeof *u->order);
    u->norder -= n;
}

static WIKIUNIT* new_unit(WIKISIM* s, int type, int owner, double built, const char* id, const char* label)
{
    WIKIUNIT* u;
    if (s->nunit == s->cunit) {
        int c = s->cunit ? s->cunit * 2 : 32;
        WIKIUNIT** p = (WIKIUNIT**)realloc(s->unit, (size_t)c * sizeof *p);
        if (!p) { s->oom = 1; return NULL; }
        s->unit = p;
        s->cunit = c;
    }
    if (!(u = (WIKIUNIT*)calloc(1, sizeof *u))) { s->oom = 1; return NULL; }
    if (id && *id) snprintf(u->id, sizeof u->id, "%s", id);
    else snprintf(u->id, sizeof u->id, "u%d", s->next_id++);
    u->type = type;
    u->owner = owner;
    u->built = built;
    u->label = label ? label : def(s, type)->name;
    return u;
}

/* a structure at its site, appended in the room new_unit made for it */
static WIKIUNIT* place(WIKISIM* s, WIKIUNIT* u, int ox, int oz)
{
    u->ox = ox;
    u->oz = oz;
    u->x = (ox + fpx(s, u->type) / 2.0) * CELL;
    u->z = (oz + fpz(s, u->type) / 2.0) * CELL;
    s->unit[s->nunit++] = u;
    return u;
}

static WIKIUNIT* add(WIKISIM* s, int type, int owner, double x, double z, double built, const char* id, const char* label)
{
    WIKIUNIT* u = new_unit(s, type, owner, built, id, label);
    if (!u) return NULL;
    if (mobile(s, type)) {
        u->x = x;
        u->z = z;
        s->unit[s->nunit++] = u;
        return u;
    }
    return place(s, u, bound_cell(js_round(x / CELL - fpx(s, type) / 2.0)), bound_cell(js_round(z / CELL - fpz(s, type) / 2.0)));
}

static WIKIUNIT* by_id(const WIKISIM* s, const char* id)
{
    int i;
    for (i = 0; i < s->nunit; i++)
        if (!strcmp(s->unit[i]->id, id)) return s->unit[i];
    return NULL;
}

/* the first structure of that type on that site; a mobile unit has no site (sim.js: its ox is undefined) */
static WIKIUNIT* structure_at(const WIKISIM* s, int t, int ox, int oz)
{
    int i;
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        if (u->type == t && !mobile(s, u->type) && u->ox == ox && u->oz == oz) return u;
    }
    return NULL;
}

static int is_builder(const WIKISIM* s, const WIKIUNIT* u)
{
    return u->sel && u->owner == 0 && mobile(s, u->type) && def(s, u->type)->worker_time > 0;
}

static int nbuilders(const WIKISIM* s)
{
    int i, n = 0;
    for (i = 0; i < s->nunit; i++) n += is_builder(s, s->unit[i]);
    return n;
}

/* the types the selected builders' build lists offer, as a mask; read in definition order */
static uint64_t available(const WIKISIM* s)
{
    uint64_t ts = 0;
    int i;
    for (i = 0; i < s->nunit; i++)
        if (is_builder(s, s->unit[i])) ts |= def(s, s->unit[i]->type)->builds;
    return ts;
}

/* ---- plans: the player's build orders; one is started once its building stands at its site ---- */

static int started(const WIKISIM* s, const WIKIORDER* o) { return structure_at(s, o->type, o->ox, o->oz) != NULL; }

/* THE ENGINE'S SITE TEST (0x47D2E0), simplified as sim.js simplifies it: the footprint inside the
   map's placeable interior, cells 1 to w - 2 (tagpu_resource.c site_known's bound), clear of every
   structure but `ignore` (an upgrade's old building), and for a geothermal plant one footprint
   cell on a vent. Mobile units never block. */
static int interior(const WIKISIM* s, int t, int ox, int oz)
{
    return ox >= 1 && oz >= 1 && ox + fpx(s, t) < s->W && oz + fpz(s, t) < s->H;
}

static int site_ok(const WIKISIM* s, int t, int ox, int oz, const WIKIUNIT* ignore)
{
    BOX b = box(s, t, ox, oz);
    int i;
    if (!interior(s, t, ox, oz)) return 0;
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        if (!mobile(s, u->type) && u != ignore && overlap(b, box(s, u->type, u->ox, u->oz))) return 0;
    }
    if (kind(s, t) == KIND_GEO) {
        for (i = 0; i < s->nvent; i++) {
            const PlaceCell* v = &s->vent[i];
            if (v->cx >= ox && v->cx < ox + b.fx && v->cz >= oz && v->cz < oz + b.fz) break;
        }
        if (i == s->nvent) return 0;
    }
    return 1;
}

/* mirrors tagpu_resource.c site_known: the interior, every footprint cell explored */
static int known(const WIKISIM* s, int t, int ox, int oz)
{
    int i, j;
    if (!interior(s, t, ox, oz)) return 0;
    for (j = oz; j < oz + fpz(s, t); j++)
        for (i = ox; i < ox + fpx(s, t); i++)
            if (!seen(s, i, j)) return 0;
    return 1;
}

/* mirrors tagpu_resource.c fits: site_known, then the site test */
static int fits(const WIKISIM* s, int t, int ox, int oz, const WIKIUNIT* ignore)
{
    return known(s, t, ox, oz) && site_ok(s, t, ox, oz, ignore);
}

/* mirrors tagpu_resource.c plans_block: a plan's footprint over the site. Without Shift a click
   replaces the queues of the selected builders it orders, so their plans do not block it. */
static int plans_block(const WIKISIM* s, const CHOICE* c, int shift)
{
    BOX b = box(s, c->type, c->ox, c->oz);
    int i, j, replaced = 0;
    for (i = 0; !shift && i < s->nunit; i++)
        if (is_builder(s, s->unit[i]) && can_build(s, s->unit[i], c->type)) replaced = 1;
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        if (u->owner != 0 || (replaced && is_builder(s, u))) continue;
        for (j = 0; j < u->norder; j++) {
            const WIKIORDER* o = &u->order[j];
            if (o->k == WIKI_ORDER_BUILD && overlap(b, box(s, o->type, o->ox, o->oz))) return 1;
        }
    }
    return 0;
}

/* ---- the rules of tagpu_resource.c, as sim.js mirrors them. The metal a rule reads is -1 on
   unexplored ground. ---- */

static double metal_at(const WIKISIM* s, int i, int j)
{
    int k = flat(s, i, j);
    if (!seen(s, i, j)) return -1;
    return k < 0 ? NAN : s->metal[k];
}

static int coverage(const WIKISIM* s, int ox, int oz, int fx, int fz)
{
    int i, j, n = 0;
    for (j = oz; j < oz + fz; j++)
        for (i = ox; i < ox + fx; i++)
            if (metal_at(s, i, j) > s->SM) n++;
    return n;
}

static int distinct(WIKISIM* s, CHOICE* c);
static int queued(WIKISIM* s, CHOICE* c);

static void choice(CHOICE* c, int t, int k, int ox, int oz)
{
    memset(c, 0, sizeof *c);
    c->type = t;
    c->kind = k;
    c->ox = ox;
    c->oz = oz;
}

/* mirrors tagpu_resource.c site: candidates are centre cells within radius r of the seed's cell; a
   footprint's origin is its centre minus half the footprint, rounded down. A mex scores its
   coverage, a geothermal plant 1. The highest score wins and a tie goes to the centre nearest the
   raw cursor by the (candidate + 0.5) metric, the first in scan order on an exact tie. `legal`
   (Quick Build) skips a candidate the site test or a plan refuses unless it is the site of a
   queued plan; without it a mex ignores occupancy and a geothermal plant takes the nearest site
   that fits. */
static int site(WIKISIM* s, int t, double sx, double sz, double rx, double rz, int r, int legal, int shift, CHOICE* out)
{
    const int fx = fpx(s, t), fz = fpz(s, t), k = kind(s, t), bx = cell_of(sx), bz = cell_of(sz);
    double dist = INFINITY;
    int dx, dz, best = 0, found = 0;
    if (!k) return 0;
    for (dz = -r; dz <= r; dz++)
        for (dx = -r; dx <= r; dx++) {
            const int cx = bx + dx, cz = bz + dz, ox = cx - (fx >> 1), oz = cz - (fz >> 1);
            const double tx = cx - rx / CELL + 0.5, tz = cz - rz / CELL + 0.5, d = tx * tx + tz * tz;
            int n;
            if (ox < 0 || oz < 0 || ox + fx > s->W || oz + fz > s->H) continue;
            n = k == KIND_MEX ? coverage(s, ox, oz, fx, fz) : 1;
            if (!n || n < best || (n == best && d >= dist)) continue;
            if (legal) {
                const int taken = !fits(s, t, ox, oz, NULL);
                CHOICE c;
                choice(&c, t, k, ox, oz);
                if (taken && !known(s, t, ox, oz)) continue;
                if (taken || plans_block(s, &c, shift)) {
                    if (k == KIND_MEX && !distinct(s, &c)) continue;
                    if (!queued(s, &c)) continue;
                }
            } else if (k == KIND_GEO && !fits(s, t, ox, oz, NULL)) continue;
            best = n;
            dist = d;
            found = 1;
            choice(out, t, k, ox, oz);
        }
    return found;
}

/* mirrors tagpu_resource.c snapped_site: a mex winner away from the raw cell must win again in a
   search of radius max(footprint) around itself without legality, the raw cursor still breaking
   ties, or there is no snap */
static int snapped_site(WIKISIM* s, int t, double rx, double rz, int legal, int shift, CHOICE* c)
{
    const int fx = fpx(s, t), fz = fpz(s, t);
    CHOICE v;
    if (!site(s, t, rx, rz, rx, rz, RADIUS, legal, shift, c)) return 0;
    if (c->kind != KIND_MEX) return 1;
    if (c->ox + (fx >> 1) != cell_of(rx) || c->oz + (fz >> 1) != cell_of(rz)) {
        if (!site(s, t, (c->ox + fx / 2.0) * CELL, (c->oz + fz / 2.0) * CELL, rx, rz, fx > fz ? fx : fz, 0, shift, &v)
            || v.ox != c->ox || v.oz != c->oz)
            return 0;
    }
    return 1;
}

/* mirrors tagpu_resource.c distinct: from the first footprint cell above SurfaceMetal in scan
   order, the eight-connected component of cells above it is a deposit when it is closed: it never
   reaches the edge of the 129-cell arena around that cell, meets no unexplored neighbour, has an
   explored background neighbour, and does not touch both opposite map edges on either axis. Its
   anchor (the topmost, then leftmost, cell) identifies the deposit for same_plan. Asked once per
   CHOICE: nothing it reads changes while one lives. */
static int distinct(WIKISIM* s, CHOICE* c)
{
    const int fx = fpx(s, c->type), fz = fpz(s, c->type), W = s->W, H = s->H;
    int i, j, sx = -1, sz = -1, head = 0, tail = 0, boundary = 0, edges = 0, ax, az;
    if (c->anchored) return c->has_anchor;
    c->anchored = 1;
    for (j = c->oz; j < c->oz + fz && sx < 0; j++)
        for (i = c->ox; i < c->ox + fx; i++)
            if (metal_at(s, i, j) > s->SM) { sx = i; sz = j; break; }
    if (sx < 0) return 0;
    if (++s->gen == 0) {    /* the stamps wrapped: none may look current */
        memset(s->mark, 0, (size_t)W * H * sizeof *s->mark);
        s->gen = 1;
    }
    /* metal_at answered a number, so the start's flat index is in the array */
    s->mark[flat(s, sx, sz)] = s->gen;
    s->todo[tail++] = sx;
    s->todo[tail++] = sz;
    ax = sx;
    az = sz;
    while (head < tail) {
        const int ci = s->todo[head], cj = s->todo[head + 1];
        int dx, dz;
        head += 2;
        if (abs(ci - sx) == ARENA || abs(cj - sz) == ARENA) return 0;
        if (ci == 0) edges |= 1;
        if (ci == W - 1) edges |= 2;
        if (cj == 0) edges |= 4;
        if (cj == H - 1) edges |= 8;
        for (dz = -1; dz <= 1; dz++)
            for (dx = -1; dx <= 1; dx++) {
                const int ni = ci + dx, nj = cj + dz;
                int k;
                if (ni < 0 || nj < 0 || ni >= W || nj >= H) continue;
                k = nj * W + ni;
                if (s->mark[k] == s->gen) continue;
                s->mark[k] = s->gen;
                if (!seen(s, ni, nj)) return 0;
                if (s->metal[k] <= s->SM) { boundary = 1; continue; }
                /* every map cell enters once, after the start: the queue holds W * H + 1 cells */
                s->todo[tail++] = ni;
                s->todo[tail++] = nj;
                if (nj < az || (nj == az && ni < ax)) { ax = ni; az = nj; }
            }
    }
    if (!(boundary && (edges & 3) != 3 && (edges & 12) != 12)) return 0;
    c->has_anchor = 1;
    c->ax = ax;
    c->az = az;
    return 1;
}

/* mirrors tagpu_resource.c geo_anchor: the one explored vent under a footprint, by index; -1 when
   it touches no vent or several cells of them */
static int vent_anchor(const WIKISIM* s, int t, int ox, int oz)
{
    const int fx = fpx(s, t), fz = fpz(s, t);
    int i, first = -1;
    for (i = 0; i < s->nvent; i++) {
        const PlaceCell* v = &s->vent[i];
        if (!seen(s, v->cx, v->cz) || v->cx < ox || v->cx >= ox + fx || v->cz < oz || v->cz >= oz + fz) continue;
        if (first < 0) first = i;
        else if (v->cx != s->vent[first].cx || v->cz != s->vent[first].cz) return -1;
    }
    return first;
}

static XZ centre(const WIKISIM* s, int t, int ox, int oz)
{
    XZ p;
    p.x = (ox + fpx(s, t) / 2.0) * CELL;
    p.z = (oz + fpz(s, t) / 2.0) * CELL;
    return p;
}

/* mirrors tagpu_resource.c same_plan: a build order is this choice's plan when it is of the same
   kind and on the same vent, the same deposit (the same anchor), or for an upgrade within 16
   world units of the same place on both axes. Its building type need not be the one offered now. */
static int same_plan(WIKISIM* s, CHOICE* c, const WIKIORDER* o)
{
    CHOICE b;
    if (kind(s, o->type) != c->kind) return 0;
    if (c->kind == KIND_GEO) {
        const int a = vent_anchor(s, c->type, c->ox, c->oz);
        return a >= 0 && a == vent_anchor(s, o->type, o->ox, o->oz);
    }
    if (c->target[0]) {
        const XZ p = centre(s, c->type, c->ox, c->oz), q = centre(s, o->type, o->ox, o->oz);
        return fabs(p.x - q.x) <= 16 && fabs(p.z - q.z) <= 16;
    }
    choice(&b, o->type, kind(s, o->type), o->ox, o->oz);
    return distinct(s, c) && distinct(s, &b) && c->ax == b.ax && c->az == b.az;
}

/* mirrors tagpu_resource.c find_cancel: a builder's first unstarted matching plan; for an upgrade
   only a build that is not the head and follows the unstarted reclaim of the same building, which
   goes with it. Answers the first order to remove and the count in `*n`, or -1. */
static int find_cancel(WIKISIM* s, const WIKIUNIT* u, CHOICE* c, int* n)
{
    int i;
    for (i = 0; i < u->norder; i++) {
        const WIKIORDER* o = &u->order[i];
        const WIKIORDER* p = i > 0 ? &u->order[i - 1] : NULL;
        const WIKIUNIT* g;
        if (o->k != WIKI_ORDER_BUILD || started(s, o) || !same_plan(s, c, o)) continue;
        if (!c->target[0]) { *n = 1; return i; }
        if (p && p->k == WIKI_ORDER_RECLAIM && !strcmp(p->target, c->target) && (g = by_id(s, p->target))
            && !(g->reclaimed > 0)) {
            *n = 2;
            return i - 1;
        }
    }
    return -1;
}

/* mirrors tagpu_resource.c queued: 0 no plan of the player's here, 1 a plan, 2 a plan a selected
   builder can cancel. Each unit's first matching order decides for that unit. */
static int queued(WIKISIM* s, CHOICE* c)
{
    int i, j, n, q = 0;
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        if (u->owner != 0) continue;
        for (j = 0; j < u->norder; j++) {
            if (u->order[j].k != WIKI_ORDER_BUILD || !same_plan(s, c, &u->order[j])) continue;
            q = 1;
            if (u->sel && def(s, u->type)->worker_time > 0 && find_cancel(s, u, c, &n) >= 0) return 2;
            break;
        }
    }
    return q;
}

static WIKIUNIT* unit_at(const WIKISIM* s, double x, double z);

/* mirrors tagpu_resource.c quick_choice, the one answer the ghost, the cursor and a right-click all
   use. Off in the build-menu mode and while Alt is held. Over a unit, it offers only an upgrade:
   the unit must be a finished resource building the player owns, and the replacement, centred on
   it, must have a strictly higher ExtractsMetal (or EnergyMake), fit once the old one is gone or
   be queued there already. Over ground, each offered type gets snapped_site's legal search and a
   mex a distinct deposit. Per kind the highest ExtractsMetal or EnergyMake that fits wins, the
   first in definition order on a tie; between a mex and a geothermal plant, the one whose centre
   is nearer the cursor. */
static int quick_choice(WIKISIM* s, int shift, CHOICE* out)
{
    const double x = s->cursor_x, z = s->cursor_z;
    const WIKIUNIT* target;
    const uint64_t offered = available(s);
    CHOICE best[3];
    int t, desired = KIND_NONE, have[3] = { 0, 0, 0 };
    double old = 0;
    if (s->mode >= 0 || s->held[WIKI_KEY_ALT]) return 0;
    target = unit_at(s, x, z);
    if (target) {
        if (target->owner != 0 || mobile(s, target->type) || target->built < 1 || !(desired = kind(s, target->type)))
            return 0;
        old = strength(s, target->type);
    }
    for (t = 0; t < s->entry->ndef; t++) {
        const int k = kind(s, t), fx = fpx(s, t), fz = fpz(s, t);
        const double value = strength(s, t);
        CHOICE c;
        if (!((offered >> t) & 1)) continue;
        if (!k || (desired && k != desired) || value <= old || (have[k] && value <= best[k].strength)) continue;
        if (target) {
            choice(&c, t, k, bound_cell(trunc((floor(target->x) - fx * 8 + 8) / CELL)),
                   bound_cell(trunc((floor(target->z) - fz * 8 + 8) / CELL)));
            snprintf(c.target, sizeof c.target, "%s", target->id);
            if (!fits(s, t, c.ox, c.oz, target) && (!known(s, t, c.ox, c.oz) || !queued(s, &c))) continue;
        } else if (!snapped_site(s, t, x, z, 1, shift, &c) || (k == KIND_MEX && !distinct(s, &c))) continue;
        c.strength = value;
        c.queued = queued(s, &c);
        if (!c.queued && plans_block(s, &c, shift)) continue;
        best[k] = c;
        have[k] = 1;
    }
    if (have[KIND_MEX]) *out = best[KIND_MEX];
    else if (have[KIND_GEO]) *out = best[KIND_GEO];
    else return 0;
    if (have[KIND_MEX] && have[KIND_GEO]) {
        const XZ g = centre(s, best[KIND_GEO].type, best[KIND_GEO].ox, best[KIND_GEO].oz);
        const XZ m = centre(s, best[KIND_MEX].type, best[KIND_MEX].ox, best[KIND_MEX].oz);
        if (sq(floor(g.x - x)) + sq(floor(g.z - z)) < sq(floor(m.x - x)) + sq(floor(m.z - z))) *out = best[KIND_GEO];
    }
    out->queued = queued(s, out);
    return 1;
}

/* ---- what the cursor is over ---- */

static WIKIUNIT* unit_at(const WIKISIM* s, double x, double z)
{
    WIKIUNIT* hit = NULL;
    int i;
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        if (fabs(x - u->x) <= fpx(s, u->type) * CELL / 2.0 && fabs(z - u->z) <= fpz(s, u->type) * CELL / 2.0
            && (!hit || mobile(s, u->type) || !mobile(s, hit->type)))
            hit = u;
    }
    return hit;
}

static void evaluate(WIKISIM* s, WIKIPLACE* p);

/* the build-menu mode: the ghost is manual_cursor's snapped site (no legality), or where the cursor
   is, as the build cursor 0x4197D0 rounds it */
static WIKIHOVER hover(WIKISIM* s)
{
    const double x = s->cursor_x, z = s->cursor_z;
    const int shift = s->held[WIKI_KEY_SHIFT];
    WIKIHOVER h;
    CHOICE c;
    memset(&h, 0, sizeof h);
    evaluate(s, &s->place);
    if (s->mode >= 0) {
        const int t = s->mode;
        const int snapped = !s->held[WIKI_KEY_ALT] && snapped_site(s, t, x, z, 0, shift, &c);
        h.kind = WIKI_HOVER_PLACE;
        h.type = t;
        h.ox = snapped ? c.ox : origin(x, fpx(s, t));
        h.oz = snapped ? c.oz : origin(z, fpz(s, t));
        h.ok = site_ok(s, t, h.ox, h.oz, NULL);
        return h;
    }
    if (quick_choice(s, shift, &c)) {
        h.kind = WIKI_HOVER_QUICK;
        h.type = c.type;
        h.ox = c.ox;
        h.oz = c.oz;
        h.ok = 1;
        return h;
    }
    h.kind = unit_at(s, x, z) ? WIKI_HOVER_UNIT : WIKI_HOVER_GROUND;
    return h;
}

/* ---- the placement gesture, tagpu_place.c as sim.js mirrors it ---- */

/* mirrors tagpu_place.c's cursor site: the build cursor's rectangle, snapped as the ghost is.
   PlaceCell holds int16, so the cell is held to its range. */
static PlaceCell cursor_site(WIKISIM* s)
{
    const int t = s->mode;
    PlaceCell p;
    CHOICE c;
    int cx, cz;
    if (!s->held[WIKI_KEY_ALT] && snapped_site(s, t, s->cursor_x, s->cursor_z, 0, s->held[WIKI_KEY_SHIFT], &c)) {
        cx = c.ox;
        cz = c.oz;
    } else {
        cx = origin(s->cursor_x, fpx(s, t));
        cz = origin(s->cursor_z, fpz(s, t));
    }
    p.cx = (int16_t)(cx < -32768 ? -32768 : cx > 32767 ? 32767 : cx);
    p.cz = (int16_t)(cz < -32768 ? -32768 : cz > 32767 ? 32767 : cz);
    return p;
}

static XZ site_centre(const WIKISIM* s, int t, const WIKISITE* w) { return centre(s, t, w->cx, w->cz); }
static XZ order_at(const WIKISIM* s, const WIKIORDER* o) { return centre(s, o->type, o->ox, o->oz); }

/* mirrors tagpu_place.c toggles: some selected builder has a build order within 16 world units of
   the site; it compares no building type */
static int toggles(const WIKISIM* s, int t, const WIKISITE* w)
{
    const XZ at = site_centre(s, t, w);
    int i, j;
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        if (!is_builder(s, u)) continue;
        for (j = 0; j < u->norder; j++)
            if (u->order[j].k == WIKI_ORDER_BUILD && within(at, order_at(s, &u->order[j]))) return 1;
    }
    return 0;
}

/* scratch for take_offs_one: one index per order of the builder, never NULL for none; NULL when
   it cannot be had */
static int* by_buffer(WIKISIM* s, int n)
{
    if (n < 1) n = 1;
    if (n > s->cby) {
        int* p = (int*)realloc(s->by, (size_t)n * sizeof *p);
        if (!p) { s->oom = 1; return NULL; }
        s->by = p;
        s->cby = n;
    }
    return s->by;
}

/* mirrors tagpu_place.c take_offs for one builder: the Shift-click per site against its queue as it
   stands, each queued order taken off by at most one site -- every site not refused first takes
   the build order at its own position, then each site that took none takes the first build order
   within 16 world units that no site has taken, both in shape order, each in queue order. A site
   that takes one becomes WIKI_SITE_REMOVE. `by[j]` is the site that takes order j off, -1 for
   none; `took[i]` says site i took one. Own positions go first over the whole shape because
   neighbouring 1 x 1 sites are 16 world units apart: whichever end a drag over a queued row starts
   from, every order of the row is taken off by the site that stands on it. A builder's answer
   reads only its own queue and which sites are refused, so a builder's orders may be changed once
   its own answer is used. */
static void take_offs_one(const WIKISIM* s, int t, WIKISITE* sites, int n, const WIKIUNIT* u, int* by, char* took)
{
    int own, i, j;
    for (j = 0; j < u->norder; j++) by[j] = -1;
    memset(took, 0, (size_t)n);
    for (own = 1; own >= 0; own--)
        for (i = 0; i < n; i++) {
            XZ at;
            if (sites[i].state == WIKI_SITE_REFUSED || (!own && took[i])) continue;
            at = site_centre(s, t, &sites[i]);
            for (j = 0; j < u->norder; j++) {
                const WIKIORDER* o = &u->order[j];
                XZ q;
                if (o->k != WIKI_ORDER_BUILD || by[j] >= 0) continue;
                q = order_at(s, o);
                if (own ? q.x == at.x && q.z == at.z : within(at, q)) break;
            }
            if (j < u->norder) {
                by[j] = i;
                took[i] = 1;
                sites[i].state = WIKI_SITE_REMOVE;
            }
        }
}

/* mirrors tagpu_place.c take_offs over every selected builder, for the sites' states. A ring takes
   nothing off: a site a Shift-click would toggle is WIKI_SITE_REMOVE, and left out. */
static void take_offs(WIKISIM* s, int t, WIKISITE* sites, int n, int ring)
{
    char took[PLACE_MAX_SITES];
    int i;
    if (ring) {
        for (i = 0; i < n; i++)
            if (sites[i].state != WIKI_SITE_REFUSED && toggles(s, t, &sites[i])) sites[i].state = WIKI_SITE_REMOVE;
        return;
    }
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        int* by;
        if (!is_builder(s, u) || !(by = by_buffer(s, u->norder))) continue;
        take_offs_one(s, t, sites, n, u, by, took);
    }
}

/* mirrors tagpu_place.c classify: refused where the site test refuses, else WIKI_SITE_PLACE, and
   then the take-off rule's WIKI_SITE_REMOVE */
static void classify(WIKISIM* s, int t, const PlaceCell* cells, int n, int ring, WIKISITE* sites)
{
    int i;
    for (i = 0; i < n; i++) {
        sites[i].cx = cells[i].cx;
        sites[i].cz = cells[i].cz;
        sites[i].state = site_ok(s, t, cells[i].cx, cells[i].cz, NULL) ? WIKI_SITE_PLACE : WIKI_SITE_REFUSED;
    }
    take_offs(s, t, sites, n, ring);
}

/* mirrors tagpu_place.c ring_target: the unit under the cursor, any unit (0x48CD80), else a selected
   builder's queued build whose footprint holds the cursor's cell, first in selection order then
   queue order. As cells. */
static int ring_target(const WIKISIM* s, int* tx, int* tz, int* tw, int* th)
{
    const WIKIUNIT* u = unit_at(s, s->cursor_x, s->cursor_z);
    int i, j, px, pz;
    if (u) {
        *tw = fpx(s, u->type);
        *th = fpz(s, u->type);
        *tx = origin(u->x, *tw);
        *tz = origin(u->z, *th);
        return 1;
    }
    px = cell_of(s->cursor_x);
    pz = cell_of(s->cursor_z);
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* b = s->unit[i];
        if (!is_builder(s, b)) continue;
        for (j = 0; j < b->norder; j++) {
            const WIKIORDER* o = &b->order[j];
            if (o->k != WIKI_ORDER_BUILD) continue;
            if (px >= o->ox && px < o->ox + fpx(s, o->type) && pz >= o->oz && pz < o->oz + fpz(s, o->type)) {
                *tx = o->ox;
                *tz = o->oz;
                *tw = fpx(s, o->type);
                *th = fpz(s, o->type);
                return 1;
            }
        }
    }
    return 0;
}

/* mirrors tagpu_place.c evaluate: the shape the cursor shows, with the modifiers read live. Nothing
   without Shift (the release would order nothing); Shift + Ctrl over a ring target is a ring,
   before any drag shape; otherwise the gesture's shape from the press site to the cursor site: Alt
   a rectangle, Ctrl held to its axis or hollow. Cut to the order budget; a ring leaves out the
   sites that would take an order off. PLACE_NONE: the engine's single square stands. */
static void evaluate(WIKISIM* s, WIKIPLACE* p)
{
    PlaceCell cells[PLACE_MAX_SITES];
    const int shift = s->held[WIKI_KEY_SHIFT], ctrl = s->held[WIKI_KEY_CTRL], alt = s->held[WIKI_KEY_ALT];
    int t, fx, fz, tx, tz, tw, th, n, budget, builders, i, k;
    p->kind = PLACE_NONE;
    p->n = 0;
    if (s->mode < 0 || !shift || (!ctrl && !s->has_gesture)) return;
    t = s->mode;
    fx = fpx(s, t);
    fz = fpz(s, t);
    if (ctrl && ring_target(s, &tx, &tz, &tw, &th)) {
        p->kind = PLACE_RING;
        n = (int)place_ring(fx, fz, tx, tz, tw, th, cells, PLACE_MAX_SITES);
    } else if (s->has_gesture) {
        p->kind = alt ? (ctrl ? PLACE_HOLLOW : PLACE_RECT) : (ctrl ? PLACE_AXIS : PLACE_LINE);
        n = (int)place_shape(p->kind, fx, fz, s->spacing[t], s->gesture, cursor_site(s), cells, PLACE_MAX_SITES);
    } else return;
    builders = nbuilders(s);
    budget = PLACE_ORDER_BUDGET / (builders ? builders : 1);
    if (n > budget) n = budget;
    p->type = t;
    classify(s, t, cells, n, p->kind == PLACE_RING, p->site);
    p->n = n;
    if (p->kind == PLACE_RING) {
        for (i = k = 0; i < n; i++)
            if (p->site[i].state != WIKI_SITE_REMOVE) p->site[k++] = p->site[i];
        p->n = k;
    }
}

static void place_click(WIKISIM* s, int shift);

/* mirrors tagpu_place.c commit (remove_taken and append_sites): the stock Shift-clicks of the sites,
   judged against each builder's queue as it stood before the gesture, the sites classified afresh
   by the same take-off rule the preview's colours come from. Per builder: the orders the rule gave
   a site are taken off, and each of those sites adds nothing for that builder (a ring takes
   nothing off, its removing sites skipped); the rest are appended in the shape's order while the
   queue is shorter than PLACE_LIST_CAP. Every selected builder gets them, its build list not
   consulted, as 0x419670 orders. A changed queue sets the Shift-placement bit, so releasing Shift
   disarms. */
static void commit(WIKISIM* s, const WIKIPLACE* p)
{
    PlaceCell cells[PLACE_MAX_SITES];
    WIKISITE sites[PLACE_MAX_SITES];
    char took[PLACE_MAX_SITES];
    const int t = p->type, ring = p->kind == PLACE_RING;
    int i, j, b, changed = 0;
    for (i = 0; i < p->n; i++) {
        cells[i].cx = (int16_t)p->site[i].cx;
        cells[i].cz = (int16_t)p->site[i].cz;
    }
    classify(s, t, cells, p->n, ring, sites);
    for (b = 0; b < s->nunit; b++) {
        WIKIUNIT* u = s->unit[b];
        int* by = NULL;
        if (!is_builder(s, u)) continue;
        if (!ring) {
            if (!(by = by_buffer(s, u->norder))) continue;
            take_offs_one(s, t, sites, p->n, u, by, took);
        }
        for (i = 0; i < p->n; i++)
            if (ring) took[i] = sites[i].state == WIKI_SITE_REMOVE;
        for (j = ring ? 0 : u->norder; j-- > 0;)
            if (by[j] >= 0) {
                splice_orders(u, j, 1);
                changed++;
            }
        for (i = 0; i < p->n; i++) {
            WIKIORDER o;
            if (sites[i].state == WIKI_SITE_REFUSED || took[i] || u->norder >= PLACE_LIST_CAP) continue;
            memset(&o, 0, sizeof o);
            o.k = WIKI_ORDER_BUILD;
            o.type = t;
            o.ox = sites[i].cx;
            o.oz = sites[i].cz;
            if (push_order(s, u, &o)) changed++;
        }
    }
    if (changed) s->shift_placed = 1;
}

/* mirrors tagpu_place.c's release (place_button): Shift up first drops the gesture and the building
   stays armed; a one-site gesture ending at the cursor is the stock Shift-click itself; anything
   else commits the last evaluation */
static void release(WIKISIM* s)
{
    WIKIPLACE* p = &s->place;
    PlaceCell cur;
    s->has_gesture = 0;
    if (!s->held[WIKI_KEY_SHIFT]) return;
    cur = cursor_site(s);
    if (p->kind != PLACE_NONE && p->kind != PLACE_RING && p->n == 1 && p->site[0].cx == cur.cx && p->site[0].cz == cur.cz) {
        place_click(s, 1);
        return;
    }
    if (p->kind != PLACE_NONE) commit(s, p);
}

/* ---- orders ---- */

/* leaving the build-menu mode ends the gesture and clears the Shift-placement bit with it */
static void disarm(WIKISIM* s)
{
    s->mode = -1;
    s->has_gesture = 0;
    s->shift_placed = 0;
}

/* a click's orders for one unit: they replace its list, or with Shift are appended to it -- all
   of them or, when they would take the list past PLACE_LIST_CAP, none, so an upgrade's reclaim
   never goes in without its build */
static void assign(WIKISIM* s, WIKIUNIT* u, const WIKIORDER* os, int n, int shift)
{
    int i;
    if (!shift) u->norder = 0;
    else if (u->norder + n > PLACE_LIST_CAP) return;
    for (i = 0; i < n; i++) push_order(s, u, &os[i]);
}

static WIKIORDER order_build(int t, int ox, int oz)
{
    WIKIORDER o;
    memset(&o, 0, sizeof o);
    o.k = WIKI_ORDER_BUILD;
    o.type = t;
    o.ox = ox;
    o.oz = oz;
    return o;
}

static WIKIORDER order_target(int k, const char* id)
{
    WIKIORDER o;
    memset(&o, 0, sizeof o);
    o.k = k;
    snprintf(o.target, sizeof o.target, "%s", id);
    return o;
}

/* mirrors tagpu_resource.c click, a Quick Build click: the builders that can build it get the order (an upgrade's reclaim first),
   the other selected builders guard them, shared out in turn, and units that cannot build keep
   their orders. Shift appends, and never adds a Guard to a helper already guarding one of them. */
static void quick_order(WIKISIM* s, const CHOICE* c, int shift)
{
    int i, j, k, ncap = 0, help = 0;
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        WIKIORDER os[2];
        if (!is_builder(s, u) || !can_build(s, u, c->type)) continue;
        ncap++;
        os[0] = order_target(WIKI_ORDER_RECLAIM, c->target);
        os[1] = order_build(c->type, c->ox, c->oz);
        if (c->target[0]) assign(s, u, os, 2, shift);
        else assign(s, u, os + 1, 1, shift);
    }
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        WIKIORDER g;
        int guarding = 0, nth;
        if (!is_builder(s, u) || can_build(s, u, c->type)) continue;
        nth = help++;
        for (j = 0; shift && j < u->norder && !guarding; j++)
            for (k = 0; u->order[j].k == WIKI_ORDER_GUARD && k < s->nunit && !guarding; k++)
                guarding = is_builder(s, s->unit[k]) && can_build(s, s->unit[k], c->type) && !strcmp(s->unit[k]->id, u->order[j].target);
        if (guarding) continue;
        /* the (nth % ncap)-th capable builder; quick_choice offers only what some builder can build */
        for (j = 0, k = ncap ? nth % ncap : 0; j < s->nunit; j++)
            if (is_builder(s, s->unit[j]) && can_build(s, s->unit[j], c->type) && k-- == 0) break;
        if (j == s->nunit) continue;
        g = order_target(WIKI_ORDER_GUARD, s->unit[j]->id);
        assign(s, u, &g, 1, shift);
    }
}

/* mirrors tagpu_resource.c unqueue: each selected builder loses the plan find_cancel names, and an upgrade's reclaim with it */
static void unqueue(WIKISIM* s, CHOICE* c)
{
    int i, f, n;
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        if (is_builder(s, u) && (f = find_cancel(s, u, c, &n)) >= 0) splice_orders(u, f, n);
    }
}

/* a move: the selected mobile units to points SPACING apart across the cursor, in selection order */
static void move_order(WIKISIM* s, int shift)
{
    int i, n = 0, m = 0;
    for (i = 0; i < s->nunit; i++)
        if (s->unit[i]->sel && s->unit[i]->owner == 0 && mobile(s, s->unit[i]->type)) n++;
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        WIKIORDER o;
        if (!(u->sel && u->owner == 0 && mobile(s, u->type))) continue;
        memset(&o, 0, sizeof o);
        o.k = WIKI_ORDER_MOVE;
        o.x = s->cursor_x + (m++ - (n - 1) / 2.0) * SPACING;
        o.z = s->cursor_z;
        assign(s, u, &o, 1, shift);
    }
}

/* a right-click (tagpu_resource.c right_click): Quick Build's when it has a choice -- on a queued site Shift cancels and a plain
   click is consumed unchanged -- otherwise the game's own: assist an unfinished building of the
   player's, or move */
static void right_click(WIKISIM* s, int shift)
{
    CHOICE c;
    WIKIUNIT* u;
    int i;
    if (s->mode >= 0) { disarm(s); return; }
    if (quick_choice(s, shift, &c)) {
        if (c.queued) {
            if (shift && c.queued == 2) unqueue(s, &c);
            return;
        }
        quick_order(s, &c, shift);
        return;
    }
    u = unit_at(s, s->cursor_x, s->cursor_z);
    if (u && u->owner == 0 && !mobile(s, u->type) && u->built < 1) {
        const WIKIORDER o = order_build(u->type, u->ox, u->oz);
        for (i = 0; i < s->nunit; i++)
            if (is_builder(s, s->unit[i])) assign(s, s->unit[i], &o, 1, shift);
        return;
    }
    move_order(s, shift);
}

/* a build-menu placement, stock's click: the selected builders with that button get it; without
   Shift the menu mode ends. With Shift (0x43AFC0) the first build order of the builder's within 16
   world units of the site is taken off instead of adding one, and the placement sets the
   Shift-placement bit: releasing Shift then disarms. */
static void place_click(WIKISIM* s, int shift)
{
    const WIKIHOVER h = hover(s);
    XZ at;
    int i, j;
    if (h.kind != WIKI_HOVER_PLACE || !h.ok) return;
    at = centre(s, h.type, h.ox, h.oz);
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        int k = -1;
        if (!is_builder(s, u) || !can_build(s, u, h.type)) continue;
        for (j = 0; shift && j < u->norder; j++)
            if (u->order[j].k == WIKI_ORDER_BUILD && within(at, order_at(s, &u->order[j]))) { k = j; break; }
        if (k >= 0) splice_orders(u, k, 1);
        else {
            const WIKIORDER o = order_build(h.type, h.ox, h.oz);
            assign(s, u, &o, 1, shift);
        }
    }
    if (!shift) disarm(s);
    else s->shift_placed = 1;
}

static void select_units(WIKISIM* s, int shift)
{
    const double x = s->cursor_x, z = s->cursor_z;
    WIKIUNIT* u;
    int i;
    if (s->has_drag && s->drag_active) {
        const double x0 = s->drag_x0 < x ? s->drag_x0 : x, x1 = s->drag_x0 > x ? s->drag_x0 : x;
        const double z0 = s->drag_z0 < z ? s->drag_z0 : z, z1 = s->drag_z0 > z ? s->drag_z0 : z;
        int any_mobile = 0;
        for (i = 0; i < s->nunit; i++) {
            u = s->unit[i];
            if (u->owner == 0 && u->x >= x0 && u->x <= x1 && u->z >= z0 && u->z <= z1 && mobile(s, u->type)) any_mobile = 1;
        }
        if (!shift)
            for (i = 0; i < s->nunit; i++) s->unit[i]->sel = 0;
        for (i = 0; i < s->nunit; i++) {
            u = s->unit[i];
            if (u->owner == 0 && u->x >= x0 && u->x <= x1 && u->z >= z0 && u->z <= z1 && (!any_mobile || mobile(s, u->type)))
                u->sel = 1;
        }
        return;
    }
    u = unit_at(s, x, z);
    if (u && u->owner == 0) {
        if (shift) u->sel = !u->sel;
        else {
            for (i = 0; i < s->nunit; i++) s->unit[i]->sel = 0;
            u->sel = 1;
        }
    } else if (!shift)
        for (i = 0; i < s->nunit; i++) s->unit[i]->sel = 0;
}

/* ---- input ---- */

/* mirrors tagpu_place.c tagpu_place_key and spacing_step: with a building armed and Alt down, Z adds
   one empty cell between neighbours and X takes one away, 0..PLACE_SPACING_MAX, kept per building
   type; every down steps, an auto-repeat included, and the next evaluation shows it */
static void step_spacing(WIKISIM* s, int key)
{
    int v;
    if (s->mode < 0 || !s->held[WIKI_KEY_ALT]) return;
    v = s->spacing[s->mode] + (key == WIKI_KEY_Z ? 1 : -1);
    s->spacing[s->mode] = v < 0 ? 0 : v > PLACE_SPACING_MAX ? PLACE_SPACING_MAX : v;
}

void tagpu_wikisim_input(WIKISIM* s, const WIKIINPUT* ev)
{
    if (ev->has & WIKI_IN_CAPTION) s->caption = ev->caption;
    if (ev->has & WIKI_IN_BUILD) {
        disarm(s);
        s->mode = ev->build >= 0 && ev->build < s->entry->ndef ? ev->build : -1;
        s->has_drag = 0;
    }
    if (ev->has & WIKI_IN_MOVE) {
        s->cursor_x = ev->move.x;
        s->cursor_z = ev->move.z;
        if (s->has_drag && sq(s->cursor_x - s->drag_x0) + sq(s->cursor_z - s->drag_z0) > DRAG * DRAG) s->drag_active = 1;
    }
    if (ev->down == WIKI_KEY_Z || ev->down == WIKI_KEY_X) {
        s->held[ev->down] = 1;
        step_spacing(s, ev->down);
    } else if (ev->down >= 0 && ev->down < WIKI_NKEYS && !s->held[ev->down]) {
        int shift;
        s->held[ev->down] = 1;
        shift = s->held[WIKI_KEY_SHIFT];
        /* with a building armed a left press carrying Shift starts a gesture at the cursor site
           instead of placing */
        if (ev->down == WIKI_KEY_LMB) {
            if (s->mode >= 0 && shift) {
                s->gesture = cursor_site(s);
                s->has_gesture = 1;
            } else if (s->mode >= 0) place_click(s, 0);
            else {
                s->has_drag = 1;
                s->drag_active = 0;
                s->drag_x0 = s->cursor_x;
                s->drag_z0 = s->cursor_z;
            }
        }
        if (ev->down == WIKI_KEY_RMB) {
            s->has_drag = 0;
            right_click(s, shift);
        }
    }
    if (ev->up >= 0 && ev->up < WIKI_NKEYS && s->held[ev->up]) {
        s->held[ev->up] = 0;
        if (ev->up == WIKI_KEY_LMB && s->has_gesture) release(s);
        else if (ev->up == WIKI_KEY_LMB && s->has_drag) {
            select_units(s, s->held[WIKI_KEY_SHIFT]);
            s->has_drag = 0;
        }
        /* 0x4992D8: Shift found up after a placement in this hold disarms, as after a stock Shift-click */
        if (ev->up == WIKI_KEY_SHIFT) {
            if (s->shift_placed) disarm(s);
            s->shift_placed = 0;
        }
    }
    s->hover = hover(s);
}

/* ---- one tick. Units walk in a straight line at MaxVelocity (world units a tick) and stop once
   within BuildDistance of the footprint; a building fills at its builders' summed WorkerTime over
   its BuildTime (worker time a second), times the entry's pace, with no cost; a reclaim takes as
   long as building it would. Guards work on whatever their guarded unit works on this tick, so
   they are resolved after everyone else. ---- */

static double rect_dist(const WIKIUNIT* u, BOX b)
{
    double dx = b.ox * CELL - u->x, dz = b.oz * CELL - u->z;
    const double ex = u->x - (b.ox + b.fx) * CELL, ez = u->z - (b.oz + b.fz) * CELL;
    if (dx < 0) dx = 0;
    if (ex > dx) dx = ex;
    if (dz < 0) dz = 0;
    if (ez > dz) dz = ez;
    return sqrt(dx * dx + dz * dz);
}

/* a step toward (tx, tz), ending `stop` short of it; 1 once there */
static int walk(const WIKISIM* s, WIKIUNIT* u, double tx, double tz, double stop)
{
    const double dx = tx - u->x, dz = tz - u->z, d = sqrt(dx * dx + dz * dz);
    double step;
    if (d <= stop) return 1;
    step = def(s, u->type)->max_velocity;
    if (d - stop < step) step = d - stop;
    u->x += dx / d * step;
    u->z += dz / d * step;
    return 0;
}

/* toward a footprint until within build distance of it; 1 once in range */
static int reach(const WIKISIM* s, WIKIUNIT* u, BOX b)
{
    if (rect_dist(u, b) <= def(s, u->type)->build_distance) return 1;
    walk(s, u, (b.ox + b.fx / 2.0) * CELL, (b.oz + b.fz / 2.0) * CELL, 0);
    return 0;
}

static void credit(const WIKISIM* s, WIKIUNIT* u, WIKIUNIT* st, int how)
{
    const double w = def(s, u->type)->worker_time / def(s, st->type)->build_time / TPS * s->PACE;
    if (how == HOW_BUILD) st->work_build += w;
    else st->work_reclaim += w;
    st->worked = 1;
    u->job = st;
    u->job_how = how;
}

static void shift_order(WIKIUNIT* u) { splice_orders(u, 0, 1); }

static void remove_unit(WIKISIM* s, WIKIUNIT* u)
{
    int i, k = 0;
    for (i = 0; i < s->nunit; i++)
        if (s->unit[i] != u) s->unit[k++] = s->unit[i];
    s->nunit = k;
    free(u->order);
    free(u);
}

static void advance(WIKISIM* s)
{
    int i;
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        u->job = NULL;
        u->worked = 0;
        u->work_build = u->work_reclaim = 0;
    }
    /* a structure placed here joins the list and is visited too, with no orders */
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        WIKIUNIT* st;
        WIKIORDER* o = u->norder ? &u->order[0] : NULL;
        if (!o || !mobile(s, u->type) || o->k == WIKI_ORDER_GUARD) continue;
        if (o->k == WIKI_ORDER_MOVE) {
            if (walk(s, u, o->x, o->z, 0)) shift_order(u);
            continue;
        }
        if (o->k == WIKI_ORDER_RECLAIM) {
            if (!(st = by_id(s, o->target))) { shift_order(u); continue; }
            if (reach(s, u, box(s, st->type, st->ox, st->oz))) credit(s, u, st, HOW_RECLAIM);
            continue;
        }
        st = structure_at(s, o->type, o->ox, o->oz);
        if (st && st->built >= 1) { shift_order(u); continue; }
        if (!reach(s, u, box(s, o->type, o->ox, o->oz))) continue;
        if (!st) {
            if (!site_ok(s, o->type, o->ox, o->oz, NULL)) { shift_order(u); continue; }
            if (!(st = new_unit(s, o->type, u->owner, 0, NULL, NULL))) continue;
            place(s, st, o->ox, o->oz);
        }
        credit(s, u, st, HOW_BUILD);
    }
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        WIKIUNIT* g;
        WIKIORDER* o = u->norder ? &u->order[0] : NULL;
        if (!o || o->k != WIKI_ORDER_GUARD) continue;
        if (!(g = by_id(s, o->target))) { shift_order(u); continue; }
        if (g->job) {
            WIKIUNIT* j = g->job;
            if (reach(s, u, box(s, j->type, j->ox, j->oz))) credit(s, u, j, g->job_how);
        } else walk(s, u, g->x, g->z, FOLLOW);
    }
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* st = s->unit[i];
        if (!st->worked) continue;
        if (st->work_build && st->built < 1) {
            st->built += st->work_build;
            if (st->built > 1) st->built = 1;
        }
        if (st->work_reclaim) {
            st->reclaimed += st->work_reclaim;
            if (st->reclaimed > 1) st->reclaimed = 1;
            if (st->reclaimed >= 1) {
                remove_unit(s, st);
                i--;
            }
        }
    }
    /* orders whose work is over end now, so the next one starts on the next tick */
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        const WIKIORDER* o = u->norder ? &u->order[0] : NULL;
        WIKIUNIT* st;
        if (!o) continue;
        if (o->k == WIKI_ORDER_BUILD) {
            if ((st = structure_at(s, o->type, o->ox, o->oz)) && st->built >= 1) shift_order(u);
        } else if ((o->k == WIKI_ORDER_RECLAIM || o->k == WIKI_ORDER_GUARD) && !by_id(s, o->target))
            shift_order(u);
    }
    /* no pointer to a reclaimed unit outlives the tick */
    for (i = 0; i < s->nunit; i++) s->unit[i]->job = NULL;
}

/* ---- the sim ---- */

static void feed(WIKISIM* s, int t)
{
    while (s->next < s->nscript && s->script[s->next]->at == t) tagpu_wikisim_input(s, s->script[s->next++]);
}

void tagpu_wikisim_step(WIKISIM* s)
{
    const int t = s->tick + 1;
    feed(s, t);
    advance(s);
    s->tick = t;
    s->done = s->tick >= s->end;
    s->hover = hover(s);
}

int tagpu_wikisim_orders(const WIKISIM* s, const char* id, const WIKIORDER** o)
{
    const WIKIUNIT* u = by_id(s, id);
    if (!u) return -1;
    *o = u->order;
    return u->norder;
}

static int grow(WIKISIM* s, void** p, int* cap, int need, size_t size)
{
    void* q;
    int c;
    if (need <= *cap) return 1;
    for (c = *cap ? *cap : 16; c < need; c *= 2) {}
    if (!(q = realloc(*p, (size_t)c * size))) { s->oom = 1; return 0; }
    *p = q;
    *cap = c;
    return 1;
}

static WIKIVSITE vsite(const WIKISIM* s, int t, int ox, int oz)
{
    WIKIVSITE v;
    const XZ p = centre(s, t, ox, oz);
    memset(&v, 0, sizeof v);
    v.type = t;
    v.fx = fpx(s, t);
    v.fz = fpz(s, t);
    v.x = p.x;
    v.z = p.z;
    return v;
}

const WIKIVIEW* tagpu_wikisim_view(WIKISIM* s)
{
    WIKIVIEW* v = &s->view;
    /* the gesture's sites replace the cursor's single square; none (an empty ring) leaves the square standing */
    const WIKIPLACE* p = s->place.kind != PLACE_NONE && s->place.n ? &s->place : NULL;
    const WIKIHOVER* h = &s->hover;
    const int gh = !p && (h->kind == WIKI_HOVER_PLACE || h->kind == WIKI_HOVER_QUICK);
    int i, j, k, n = 0, any_mobile = 0;

    v->w = s->W;
    v->h = s->H;
    v->surface_metal = s->SM;
    v->metal = s->metal;
    v->fog = s->fog;
    v->nvent = s->nvent;
    v->vent = s->vent_centre;

    v->nunit = 0;
    if (grow(s, (void**)&s->vunit, &s->cvunit, s->nunit, sizeof *s->vunit))
        for (i = 0; i < s->nunit; i++) {
            const WIKIUNIT* u = s->unit[i];
            WIKIVUNIT* w = &s->vunit[v->nunit++];
            w->id = u->id;
            w->type = u->type;
            w->owner = u->owner;
            w->x = u->x;
            w->z = u->z;
            w->fx = fpx(s, u->type);
            w->fz = fpz(s, u->type);
            w->selected = u->sel;
            w->built = u->built;
            w->reclaimed = u->reclaimed;
            w->label = u->label;
        }
    v->unit = s->vunit;

    if ((v->has_ghost = gh)) {
        v->ghost = vsite(s, h->type, h->ox, h->oz);
        v->ghost.ok = h->ok;
    }

    v->nsite = p ? p->n : 0;
    for (i = 0; i < v->nsite; i++) {
        s->vsite[i] = vsite(s, p->type, p->site[i].cx, p->site[i].cz);
        s->vsite[i].state = p->site[i].state;
        s->vsite[i].order = p->site[i].state == WIKI_SITE_PLACE ? ++n : 0;
    }
    v->site = s->vsite;
    v->shape = p ? p->kind : PLACE_NONE;
    v->spacing = s->mode >= 0 ? s->spacing[s->mode] : -1;

    /* plans(): the player's unstarted build orders, one per site; each owned by the last unit, of
       any owner, that holds an order for it */
    v->nqueued = 0;
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        if (u->owner != 0) continue;
        for (j = 0; j < u->norder; j++) {
            const WIKIORDER* o = &u->order[j];
            const XZ c = centre(s, o->type, o->ox, o->oz);
            int q;
            if (o->k != WIKI_ORDER_BUILD || started(s, o)) continue;
            /* one type at one centre is one site */
            for (q = 0; q < v->nqueued; q++)
                if (s->vqueued[q].type == o->type && s->vqueued[q].x == c.x && s->vqueued[q].z == c.z) break;
            if (q < v->nqueued || !grow(s, (void**)&s->vqueued, &s->cvqueued, v->nqueued + 1, sizeof *s->vqueued)) continue;
            s->vqueued[v->nqueued] = vsite(s, o->type, o->ox, o->oz);
            for (k = 0; k < s->nunit; k++) {
                int m;
                for (m = 0; m < s->unit[k]->norder; m++) {
                    const WIKIORDER* r = &s->unit[k]->order[m];
                    if (r->k == WIKI_ORDER_BUILD && r->type == o->type && r->ox == o->ox && r->oz == o->oz)
                        s->vqueued[v->nqueued].owner = s->unit[k]->owner;
                }
            }
            v->nqueued++;
        }
    }
    v->queued = s->vqueued;

    v->nline = 0;
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        double fx = u->x, fz = u->z;
        if (!u->sel) continue;
        for (j = 0; j < u->norder; j++) {
            const WIKIORDER* o = &u->order[j];
            double tx, tz;
            if (o->k == WIKI_ORDER_MOVE) { tx = o->x; tz = o->z; }
            else if (o->k == WIKI_ORDER_BUILD) {
                const XZ c = centre(s, o->type, o->ox, o->oz);
                tx = c.x;
                tz = c.z;
            } else {
                const WIKIUNIT* t = by_id(s, o->target);
                if (!t) continue;
                tx = t->x;
                tz = t->z;
            }
            if (!grow(s, (void**)&s->vline, &s->cvline, v->nline + 1, sizeof *s->vline)) break;
            s->vline[v->nline].from_x = fx;
            s->vline[v->nline].from_z = fz;
            s->vline[v->nline].to_x = tx;
            s->vline[v->nline].to_z = tz;
            s->vline[v->nline].kind = o->k;
            v->nline++;
            fx = tx;
            fz = tz;
        }
    }
    v->line = s->vline;

    for (i = 0; i < s->nunit; i++)
        if (s->unit[i]->sel && s->unit[i]->owner == 0 && mobile(s, s->unit[i]->type)) any_mobile = 1;
    v->cursor_x = s->cursor_x;
    v->cursor_z = s->cursor_z;
    v->cursor_shape = s->mode >= 0 || gh ? WIKI_CURSOR_BUILD : any_mobile ? WIKI_CURSOR_MOVE : WIKI_CURSOR_ARROW;
    memcpy(v->held, s->held, sizeof v->held);
    if ((v->has_drag = s->has_drag && s->drag_active)) {
        v->drag_x0 = s->drag_x0;
        v->drag_z0 = s->drag_z0;
        v->drag_x1 = s->cursor_x;
        v->drag_z1 = s->cursor_z;
    }
    v->caption = s->caption;
    v->pace = s->PACE;
    v->mode = s->mode;
    return v;
}

/* ---- the state hash, in tagpu_wikisim.h's order ---- */

static void fnv_bytes(uint64_t* h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    while (n--) {
        *h ^= *b++;
        *h *= 0x100000001B3ull;
    }
}

static void fnv_int(uint64_t* h, int v)
{
    const uint32_t u = (uint32_t)v;
    unsigned char b[4];
    int i;
    for (i = 0; i < 4; i++) b[i] = (unsigned char)(u >> (8 * i));
    fnv_bytes(h, b, 4);
}

static void fnv_double(uint64_t* h, double v)
{
    uint64_t u;
    unsigned char b[8];
    int i;
    memcpy(&u, &v, sizeof u);
    for (i = 0; i < 8; i++) b[i] = (unsigned char)(u >> (8 * i));
    fnv_bytes(h, b, 8);
}

static void fnv_str(uint64_t* h, const char* str)
{
    const size_t n = str ? strlen(str) : 0;
    fnv_int(h, (int)n);
    fnv_bytes(h, str, n);
}

uint64_t tagpu_wikisim_hash(const WIKISIM* s)
{
    /* the view's own rule: the gesture's sites stand in for the ghost (tagpu_wikisim_view) */
    const WIKIPLACE* p = s->place.kind != PLACE_NONE && s->place.n ? &s->place : NULL;
    const WIKIHOVER* g = &s->hover;
    const int ghost = !p && (g->kind == WIKI_HOVER_PLACE || g->kind == WIKI_HOVER_QUICK);
    const int cells = s->W * s->H;
    uint64_t h = 0xCBF29CE484222325ull;
    int i, j;

    fnv_int(&h, s->tick);
    fnv_int(&h, s->done ? 1 : 0);

    fnv_int(&h, s->W);
    fnv_int(&h, s->H);
    fnv_double(&h, s->SM);
    for (i = 0; i < cells; i++) fnv_double(&h, s->metal[i]);
    for (i = 0; i < cells; i++) fnv_int(&h, s->fog[i]);
    fnv_int(&h, s->nvent);
    for (i = 0; i < s->nvent; i++) {
        fnv_double(&h, s->vent_centre[i].x);
        fnv_double(&h, s->vent_centre[i].z);
    }

    fnv_int(&h, s->nunit);
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        /* sim.js's orders(id) answers the first unit of that id, so this does too */
        const WIKIUNIT* by = by_id(s, u->id);
        fnv_str(&h, u->id);
        fnv_str(&h, def(s, u->type)->name);
        fnv_int(&h, u->owner);
        fnv_double(&h, u->x);
        fnv_double(&h, u->z);
        fnv_int(&h, u->sel ? 1 : 0);
        fnv_double(&h, u->built);
        fnv_double(&h, u->reclaimed);
        fnv_str(&h, u->label);
        fnv_int(&h, by->norder);
        for (j = 0; j < by->norder; j++) {
            const WIKIORDER* o = &by->order[j];
            fnv_int(&h, o->k);
            if (o->k == WIKI_ORDER_MOVE) {
                fnv_double(&h, o->x);
                fnv_double(&h, o->z);
            } else if (o->k == WIKI_ORDER_BUILD) {
                fnv_str(&h, def(s, o->type)->name);
                fnv_int(&h, o->ox);
                fnv_int(&h, o->oz);
            } else fnv_str(&h, o->target);
        }
    }

    fnv_int(&h, ghost);
    if (ghost) {
        const XZ c = centre(s, g->type, g->ox, g->oz);
        fnv_str(&h, def(s, g->type)->name);
        fnv_double(&h, c.x);
        fnv_double(&h, c.z);
        fnv_int(&h, g->ok ? 1 : 0);
    }

    fnv_int(&h, p ? p->kind : PLACE_NONE);
    fnv_int(&h, p ? p->n : 0);
    for (i = 0; p && i < p->n; i++) {
        const XZ c = site_centre(s, p->type, &p->site[i]);
        fnv_double(&h, c.x);
        fnv_double(&h, c.z);
        fnv_int(&h, p->site[i].state);
    }

    fnv_double(&h, s->cursor_x);
    fnv_double(&h, s->cursor_z);
    for (i = 0; i < WIKI_NKEYS; i++) fnv_int(&h, s->held[i] ? 1 : 0);
    fnv_str(&h, s->mode >= 0 ? def(s, s->mode)->name : "");
    fnv_int(&h, s->mode >= 0 ? s->spacing[s->mode] : -1);
    fnv_int(&h, s->has_drag && s->drag_active);
    if (s->has_drag && s->drag_active) {
        fnv_double(&h, s->drag_x0);
        fnv_double(&h, s->drag_z0);
    }
    fnv_str(&h, s->caption);
    return h;
}

void tagpu_wikisim_free(WIKISIM* s)
{
    int i;
    for (i = 0; s->unit && i < s->nunit; i++) {
        free(s->unit[i]->order);
        free(s->unit[i]);
    }
    free(s->unit);
    free(s->metal);
    free(s->fog);
    free(s->vent);
    free(s->vent_centre);
    free((void*)s->script);
    free(s->mark);
    free(s->todo);
    free(s->by);
    free(s->vunit);
    free(s->vqueued);
    free(s->vline);
    memset(s, 0, sizeof *s);
}

static void fill_metal(WIKISIM* s, const WIKIMETAL* m)
{
    int i, j;
    for (j = m->z > 0 ? m->z : 0; j < (s->H < m->z + m->h ? s->H : m->z + m->h); j++)
        for (i = m->x > 0 ? m->x : 0; i < (s->W < m->x + m->w ? s->W : m->x + m->w); i++) s->metal[j * s->W + i] = m->v;
}

static void fill_fog(WIKISIM* s, const WIKIFOG* f)
{
    int i, j;
    for (j = f->z > 0 ? f->z : 0; j < (s->H < f->z + f->h ? s->H : f->z + f->h); j++)
        for (i = f->x > 0 ? f->x : 0; i < (s->W < f->x + f->w ? s->W : f->x + f->w); i++) s->fog[j * s->W + i] = f->black ? 2 : 1;
}

/* the script's order: by tick, then file order */
static int by_tick(const void* a, const void* b)
{
    const WIKIINPUT* x = *(const WIKIINPUT* const*)a;
    const WIKIINPUT* y = *(const WIKIINPUT* const*)b;
    return x->at < y->at ? -1 : x->at > y->at ? 1 : x < y ? -1 : x > y;
}

static int scene_units(WIKISIM* s, const WIKISCENE* sc)
{
    int i, j;
    for (i = 0; i < sc->nunit; i++) {
        const WIKISCENEUNIT* d = &sc->unit[i];
        WIKIUNIT* u = add(s, d->def, d->owner, d->x, d->z, d->built, d->id, d->label);
        if (!u) return 0;
        for (j = 0; j < d->nmove; j++) {
            WIKIORDER o;
            memset(&o, 0, sizeof o);
            o.k = WIKI_ORDER_MOVE;
            o.x = d->move[j].x;
            o.z = d->move[j].z;
            push_order(s, u, &o);
            if (s->oom) return 0;
        }
    }
    return 1;
}

int tagpu_wikisim_init(WIKISIM* s, const WIKIENTRY* e, const WIKIDEMO* d, int scripted, char* err, size_t errn)
{
    const WIKISCENE* a = &e->scene;
    const WIKISCENE* b = d ? &d->scene : NULL;
    const WIKISCENE* sel = b && b->has_selected ? b : a->has_selected ? a : NULL;
    const size_t cells = (size_t)a->w * a->h;
    int i, k;
    memset(s, 0, sizeof *s);
    s->entry = e;
    s->W = a->w;
    s->H = a->h;
    s->SM = a->surface_metal;
    s->PACE = e->pace;
    s->mode = -1;
    s->end = scripted && d && d->has_end ? d->end : INFINITY;
    s->next_id = 1;
    if (!(s->metal = (double*)malloc(cells * sizeof *s->metal)) || !(s->fog = (unsigned char*)calloc(cells, 1))
        || !(s->mark = (uint32_t*)calloc(cells, sizeof *s->mark)) || !(s->todo = (int*)malloc((cells + 1) * 2 * sizeof *s->todo)))
        goto oom;
    for (i = 0; i < (int)cells; i++) s->metal[i] = s->SM;
    for (i = 0; i < a->nmetal; i++) fill_metal(s, &a->metal[i]);
    for (i = 0; b && i < b->nmetal; i++) fill_metal(s, &b->metal[i]);
    for (i = 0; i < a->nfog; i++) fill_fog(s, &a->fog[i]);
    for (i = 0; b && i < b->nfog; i++) fill_fog(s, &b->fog[i]);
    s->nvent = a->nvent + (b ? b->nvent : 0);
    if (!(s->vent = (PlaceCell*)calloc((size_t)s->nvent + 1, sizeof *s->vent))
        || !(s->vent_centre = (WIKIPOINT*)calloc((size_t)s->nvent + 1, sizeof *s->vent_centre)))
        goto oom;
    for (i = 0; i < s->nvent; i++) {
        const WIKIPOINT* p = i < a->nvent ? &a->vent[i] : &b->vent[i - a->nvent];
        const int cx = cell_of(p->x), cz = cell_of(p->z);
        s->vent[i].cx = (int16_t)(cx < -32768 ? -32768 : cx > 32767 ? 32767 : cx);
        s->vent[i].cz = (int16_t)(cz < -32768 ? -32768 : cz > 32767 ? 32767 : cz);
        s->vent_centre[i].x = (cx + 0.5) * CELL;
        s->vent_centre[i].z = (cz + 0.5) * CELL;
    }
    if (!scene_units(s, a) || (b && !scene_units(s, b))) goto oom;
    for (i = 0; sel && i < sel->nselected; i++) {
        WIKIUNIT* u = by_id(s, sel->selected[i]);
        if (!u) {
            snprintf(err, errn, "wiki sim: the scene selects %s, which it does not have", sel->selected[i]);
            tagpu_wikisim_free(s);
            return 0;
        }
        u->sel = 1;
    }
    if (b && b->has_cursor) { s->cursor_x = b->cursor.x; s->cursor_z = b->cursor.z; }
    else if (a->has_cursor) { s->cursor_x = a->cursor.x; s->cursor_z = a->cursor.z; }
    if (scripted && d) {
        if (!(s->script = (const WIKIINPUT**)malloc(((size_t)d->ninput + 1) * sizeof *s->script))) goto oom;
        for (k = 0; k < d->ninput; k++) s->script[k] = &d->input[k];
        qsort((void*)s->script, (size_t)d->ninput, sizeof *s->script, by_tick);
        s->nscript = d->ninput;
    }
    feed(s, 0);
    s->hover = hover(s);
    if (s->oom) goto oom;
    return 1;
oom:
    snprintf(err, errn, "wiki sim: out of memory");
    tagpu_wikisim_free(s);
    return 0;
}
