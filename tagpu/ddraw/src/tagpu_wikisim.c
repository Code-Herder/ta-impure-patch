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
/* tagpu_place.c's cap on one commit's orders; its cap on a list, PLACE_LIST_CAP, is
   tagpu_placegeo.h's */
#define PLACE_ORDER_BUDGET 4096
/* a cell far enough off any map for every test to refuse it, and small enough that no cell
   arithmetic below can overflow: a wild cursor answers as off the map */
#define CELL_BOUND (1 << 20)

enum { KIND_NONE, KIND_MEX, KIND_GEO };
enum { HOW_BUILD, HOW_RECLAIM };
/* where the cursor is (sim.js over()); NONE: a mouse button that has not gone down */
enum { OVER_NONE, OVER_SIDE, OVER_MAP, OVER_DEAD };

static const WIKITRIPROW TRIP_ROWS[WIKI_TRIP_NROWS] = WIKI_TRIP_ROWS;
/* Every row ends on the button's down, or it would arm at another tick in another mod; and the
   inputs it makes -- its glide, a move, a down and an up per press, the glide on (row 0 has none),
   the button's down and up and the glide back -- fit WIKISIM.trip_in. Checked as this file
   compiles. */
#define TRIP_FITS_(k, g, h, gp, b, hv) \
    ((g) + (k) * ((h) + (gp)) + (b) + (hv) == WIKI_TRIP_DOWN \
     && (g) + 3 * (k) + ((k) ? (b) : 0) + 2 + (WIKI_TRIP - WIKI_TRIP_UP) <= WIKI_TRIP_EVENTS)
#define TRIP_FITS(k, row) TRIP_FITS_(k, row)
typedef char trip_row0_fits[TRIP_FITS(0, WIKI_TRIP_ROW0) ? 1 : -1];
typedef char trip_row1_fits[TRIP_FITS(1, WIKI_TRIP_ROW1) ? 1 : -1];
typedef char trip_row2_fits[TRIP_FITS(2, WIKI_TRIP_ROW2) ? 1 : -1];
typedef char trip_row3_fits[TRIP_FITS(3, WIKI_TRIP_ROW3) ? 1 : -1];
typedef char trip_rows_four[WIKI_TRIP_NROWS == 4 ? 1 : -1];

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

/* ---- tagpu_placegeo.c's shapes and take-off rule, called directly; sim.js ports each one ---- */

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
/* the layout's pages of a type, NULL for none or with the sidebar off */
static const WIKIPAGES* type_pages(const WIKISIM* s, int t)
{
    return s->lay && s->ltype[t] >= 0 ? &s->lay->type[s->ltype[t]] : NULL;
}
static int npages(const WIKISIM* s, int t) { const WIKIPAGES* p = type_pages(s, t); return p ? p->npage : 0; }

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

/* Where the cursor is: the column left of x = 0, the map, or the dead band under a map shorter
   than the column. The map is closed at its far edges because the stage clamps a pointer beyond
   them onto exactly W x 16 or H x 16, which the map rules have always read as the map. With the
   sidebar off everything is the map. */
static int over(const WIKISIM* s)
{
    if (!s->lay) return OVER_MAP;
    if (s->cursor_x < 0) return OVER_SIDE;
    return s->cursor_x <= s->W * CELL && s->cursor_z <= s->H * CELL ? OVER_MAP : OVER_DEAD;
}

/* sim.js's Math.min(Math.max(v, 0), m): a v at or below 0, -0 included, gives +0 */
static double clamp_map(double v, double m) { return v > 0 ? (v < m ? v : m) : 0.0; }

/* the cursor the map rules read: with the sidebar, clamped onto the map, so a drag released over
   the column selects up to its edge */
static XZ map_cursor(const WIKISIM* s)
{
    XZ c;
    c.x = s->lay ? clamp_map(s->cursor_x, s->W * CELL) : s->cursor_x;
    c.z = s->lay ? clamp_map(s->cursor_z, s->H * CELL) : s->cursor_z;
    return c;
}

/* ---- units: each one on the heap, so a pointer to it holds while the list grows ---- */

/* a unit joins the world, appended in the room new_unit made for it. With the sidebar it carries
   its page state: 0x485CD5 creates a unit whose type has numbered pages with bit 22 set and page
   1; any other unit has neither. */
static WIKIUNIT* enter(WIKISIM* s, WIKIUNIT* u)
{
    if (s->lay) {
        const int n = npages(s, u->type);
        u->bp = n >= 1;
        u->page = n >= 1 ? 1 : 0;
    }
    s->unit[s->nunit++] = u;
    return u;
}

/* THE ONE APPEND to a unit's orders, so a list never holds more than PLACE_LIST_CAP: an order
   past it is dropped, as sim.js's append drops it. Every order a player's clicks add comes
   through here, so they cannot grow a list until realloc fails, and take_offs' scratch
   (WIKISIM's q and by) holds any list. 1 when the order went in; 0 when the list was full or
   memory ran out, which sets `oom`. */
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

/* a structure at its site. It faces (0, 1), toward the viewer, whatever its scene gives: TA's
   default heading 0x8000 is the centre of the spawn's 0x8000 - BuildAngle/2 + rand(BuildAngle)
   (0x485A40, tagpu_native.c ghost_one), and the simulation has no randomness. */
static WIKIUNIT* place(WIKISIM* s, WIKIUNIT* u, int ox, int oz)
{
    u->ox = ox;
    u->oz = oz;
    u->x = (ox + fpx(s, u->type) / 2.0) * CELL;
    u->z = (oz + fpz(s, u->type) / 2.0) * CELL;
    u->hx = 0;
    u->hz = 1;
    return enter(s, u);
}

/* (hx, hz): the scene's facing, already a unit vector (tagpu_wikidata.c) */
static WIKIUNIT* add(WIKISIM* s, int type, int owner, double x, double z, double built, const char* id, const char* label,
                     double hx, double hz)
{
    WIKIUNIT* u = new_unit(s, type, owner, built, id, label);
    if (!u) return NULL;
    if (mobile(s, type)) {
        u->x = x;
        u->z = z;
        u->hx = hx;
        u->hz = hz;
        return enter(s, u);
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
    const XZ m = map_cursor(s);
    const double x = m.x, z = m.z;
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

/* ---- the sidebar's rules; none of them runs with the sidebar off (`lay` NULL) ---- */

static void disarm(WIKISIM* s);
static void arm(WIKISIM* s, int t);

static const WIKISCREEN* screen_of(const WIKISIM* s, int i) { return &s->lay->screen[i]; }

/* a point of the stage in the column's own pixels is (x + 128, z); the minimap's 128 x 128 there
   holds no gadget */
static int on_minimap(double x, double z)
{
    return x + WIKI_SIDE_W >= 0 && x + WIKI_SIDE_W < WIKI_SIDE_MINIMAP && z >= 0 && z < WIKI_SIDE_MINIMAP;
}

/* The engine's hit tests (the pump's 0x4AA1F6..0x4AA216, the button handler's 0x4A6B79..0x4A6B95
   on the press and 0x4A6E84..0x4A6EAC on the release) take the whole pixels x .. x + w - 1 and
   y .. y + h - 1, DISASSEMBLED. The cursor here is a double, and the pixel it stands in is its
   floor, so that is half-open on the doubles. */
static int in_rect(const WIKIGADGET* g, double px, double pz)
{
    return px >= g->x && px < g->x + g->w && pz >= g->y && pz < g->y + g->h;
}

static int inside(const WIKISCREEN* sc, const WIKIGADGET* g, double x, double z)
{
    return x < 0 && !on_minimap(x, z) && in_rect(g, x + WIKI_SIDE_W, z - sc->y);
}

/* a build button's unit as a type of the entry, -1 when it names none the entry defines */
static int unit_def(const WIKISIM* s, const WIKIGADGET* g)
{
    return g->unit[0] ? tagpu_wikidata_def(s->entry, g->unit) : -1;
}

/* OWNER QUESTION Q-O1, the recommended answer: a build button is live only when the entry defines
   its unit and the shown builder's list holds it, and greyed otherwise, so a button "comes alive"
   when an entry adds its unit (decision 7's principle) */
static int build_live(const WIKISIM* s, const WIKIGADGET* g, const WIKIUNIT* shown)
{
    const int t = unit_def(s, g);
    return !g->grey && t >= 0 && shown && can_build(s, shown, t);
}

/* A gadget's state as the stage draws it: -1 not drawn, 0 normal, 1 pressed or the current tab, 2
   greyed (frame 2 at rest, GUI_ButtonDraw 0x4A5F40). The tabs follow bit 22 as 0x41A120 sets them;
   every order button but STOP is greyed (decision 7: an entry that needs an order adds it to the
   simulation). STOP is live while some selected unit of the player's has canstop: the router
   0x41B2E0 ORs def+0x245 bit 3 over the controlled player's selected units (0x41B4AC) and 0x41A120
   greys STOP when the OR is clear (0x41A2F3), DISASSEMBLED. That 0x41A120 un-greys it for the next
   selection, because a route to the screen shown reloads it, is [INFERRED]. */
static int gadget_state(const WIKISIM* s, const WIKIGADGET* g, const WIKIUNIT* shown, int pressed)
{
    const int n = shown ? npages(s, shown->type) : 0;
    int i;
    if (g->hidden) return -1;
    switch (g->role) {
    case WIKI_ROLE_ORDERS:   return n >= 1 ? (shown->bp ? 0 : 1) : 2;
    case WIKI_ROLE_BUILDTAB: return n >= 1 ? (shown->bp ? 1 : 0) : 2;
    case WIKI_ROLE_BUILD:    return build_live(s, g, shown) ? pressed : 2;
    case WIKI_ROLE_STOP:
        for (i = 0; i < s->nunit; i++)
            if (s->unit[i]->sel && s->unit[i]->owner == 0 && def(s, s->unit[i]->type)->canstop) return pressed;
        return 2;
    case WIKI_ROLE_ORDER:    return 2;
    default:                 return pressed;
    }
}

static int live(int state) { return state == 0 || state == 1; }

/* a tab (commonattribs 0x10) acts on its press; every other button on its release */
static int press_acts(const WIKIGADGET* g) { return g->role == WIKI_ROLE_ORDERS || g->role == WIKI_ROLE_BUILDTAB; }

/* THE GADGET A PRESS AT (x, z) LANDS ON, as the GUI pump 0x4A9FD0 finds it, DISASSEMBLED: it
   offers the press to the gadgets in file order and stops at the first whose handler acts. An
   inactive gadget is skipped (0x4AA1CE..0x4AA1D3) and a greyed one's handler refuses at once
   (0x4A6B10..0x4A6B19), so neither takes it. A button that acts on the release takes the capture
   and lets the scan go on (0x4A6BAC, 0x49FC50 at 0x4A6BB3), so where buttons overlap the LAST one
   holds it. A tab acts during its own turn: it drops the capture an earlier button took (0x4A6C28)
   and returns 1 (0x4A6CD4), which ends the scan (0x4AA549..0x4AA54E), so the FIRST tab holding the
   point wins over any button before or after it. -1: none. */
static int gadget_at(const WIKISIM* s, int screen, const WIKIUNIT* shown, double x, double z)
{
    const WIKISCREEN* sc = screen_of(s, screen);
    int i, last = -1;
    for (i = 0; i < sc->ngadget; i++) {
        const WIKIGADGET* g = &sc->gadget[i];
        if (!live(gadget_state(s, g, shown, 0)) || !inside(sc, g, x, z)) continue;
        if (press_acts(g)) return i;
        last = i;
    }
    return last;
}

/* THE PAGE ROUTER, 0x41B2E0, run from the frame loop at 0x4969B4: nothing of the player's selected
   shows the side's MAIN2; EXACTLY ONE selected unit whose type has pages shows its numbered page
   while bit 22 is set, else its orders page (<NAME>0.GUI) when the type has one; anything else,
   two builders of one type included, shows the side's GEN, with the unit shown when there is
   exactly one. GEN and MAIN2 are the player's side's. The sim routes after every input and every
   tick, where the engine re-routes a frame or more later: a known difference of at most a frame.
   A screen change drops the capture, as the engine's page swap reloads the screen and its
   gadgets. */
static void route(WIKISIM* s)
{
    const WIKISIDEDEF* side = &s->lay->side[s->lay->player_side];
    const WIKIPAGES* T;
    WIKIUNIT* u = NULL;
    int i, nsel = 0, screen;
    for (i = 0; i < s->nunit; i++)
        if (s->unit[i]->sel && s->unit[i]->owner == 0 && nsel++ == 0) u = s->unit[i];
    if (nsel != 1) u = NULL;
    T = u ? type_pages(s, u->type) : NULL;
    /* THE BOUND on the screen shown: every index taken here is one tagpu_wikilayout_check held
       below lay->nscreen (a side's main2 and gen, a type's pages and orders), and u->page is 1..n:
       enter() starts it at 1 and act() is its only other writer, wrapping within 1..n */
    screen = !nsel ? side->main2
           : T && u->bp && u->page >= 1 && u->page <= T->npage ? T->page[u->page - 1]
           : T && T->orders >= 0 ? T->orders : side->gen;
    if (screen != s->screen) s->cap = -1;
    s->screen = screen;
    s->shown = u;
}

/* A BUTTON ACTS ON THE RELEASE INSIDE IT, as the engine's: 0x4A6AE0 returns 1 for a release inside
   a captured button, 0x4A9E98 stores its index and 0x4AA797 calls the screen's handler 0x41AA00.
   The tabs, which act on the press, come here too. PREV and NEXT step the shown unit's page as
   0x41BF10 and 0x41BDE0 do, wrapping within 1..n, and set bit 22; ORDERS and BUILD clear and set
   bit 22 (0x41C180). A build button arms its building; a mobile unit's button would queue it at a
   factory (0x419B00), which no entry has, so it does nothing.

   STOP is 0x419BE0's STOP branch (0x419CB6..0x419CF2, called from 0x41AA00 at 0x41ABCC),
   DISASSEMBLED: main+0x2CC3 = 1 (0x419CC2) replaces an armed build's mode 0x0E, so it disarms; it
   clears main+0x2CC6 bit 3 and leaves bit 5, the Shift-placement bit (shift_placed); the order
   issuer 0x48CF30 then gives every selected unit of the controlled player the STOP order through
   0x43AFC0 (0x48D1F4), which replaces its orders, with no canstop test. Its effect with Shift held
   is not established; the orders are emptied either way [INFERRED]. The placement gesture ends
   with the mode, as tagpu_place.c ends it on the first record handled in any mode but 14. */
static void act(WIKISIM* s, const WIKIGADGET* g)
{
    WIKIUNIT* u = s->shown;
    const int n = u ? npages(s, u->type) : 0;
    int i, t;
    if ((g->role == WIKI_ROLE_PREV || g->role == WIKI_ROLE_NEXT) && n) {
        u->page = g->role == WIKI_ROLE_NEXT ? (u->page == n ? 1 : u->page + 1) : (u->page < 2 ? n : u->page - 1);
        u->bp = 1;
    } else if (g->role == WIKI_ROLE_ORDERS && n) u->bp = 0;
    else if (g->role == WIKI_ROLE_BUILDTAB && n) u->bp = 1;
    else if (g->role == WIKI_ROLE_STOP) {
        s->mode = -1;
        s->has_gesture = 0;
        for (i = 0; i < s->nunit; i++)
            if (s->unit[i]->sel && s->unit[i]->owner == 0) s->unit[i]->norder = 0;
    } else if (g->role == WIKI_ROLE_BUILD && (t = unit_def(s, g)) >= 0 && !mobile(s, t)) arm(s, t);
    route(s);
}

/* a press over the column: a tab drops the capture (0x4A6C28) and acts now; any other button is
   captured, either mouse button alike (0x41AA00 never tests which one arms a structure) */
static void press_side(WIKISIM* s, int key)
{
    const int i = gadget_at(s, s->screen, s->shown, s->cursor_x, s->cursor_z);
    const WIKIGADGET* g;
    if (i < 0) return;
    g = &screen_of(s, s->screen)->gadget[i];
    if (press_acts(g)) {
        s->cap = -1;
        act(s, g);
    } else {
        s->cap = i;
        s->cap_key = key;
    }
}

/* WHO TAKES A MOUSE PRESS, DISASSEMBLED. IdleTick runs the GUI pump 0x4A9FD0 (0x499992) before the
   in-play handler 0x499200 (0x499A1C), and the pump's poll 0x4AB5D0 pops a record whose point is
   inside the screen's window, its gadget 0 (0x4A1680, the inclusive test 0x4A1920 at 0x4AB63E),
   and leaves a record outside it that carries a button (0x4AB647..0x4AB64D: its wParam's MK_
   bits). A press on the screen, on a gadget or not, is the GUI's alone; the handler sees only the
   releases IdleTick copies after the pump (0x4999EF..0x499A04). Every other press, the minimap's
   and the column's rows outside the screen included, reaches the handler, whose WM_RBUTTONDOWN
   calls 0x499100 at 0x4993AC wherever the point is; with an order armed (main+0x2CC3 != 1) that
   is the cancel, and it reads neither the point nor the Interface Type (0x499107..0x49915F). The
   sim does nothing else there: a press with nothing armed depends on the Interface Type
   (0x499162, 0x499567), and a left press with a building armed goes to the placement click
   0x498F70 (0x4993BF -> 0x4995B3), not established off the viewport. */
static int on_screen(const WIKISIM* s)
{
    const WIKISCREEN* sc = screen_of(s, s->screen);
    const double px = s->cursor_x + WIKI_SIDE_W, pz = s->cursor_z;
    return px >= 0 && px < sc->w && pz >= sc->y && pz < sc->y + sc->h;
}

/* the release of the captured button: inside its gadget, still live, it acts; outside, nothing */
static void release_side(WIKISIM* s)
{
    const WIKISCREEN* sc = screen_of(s, s->screen);
    const int i = s->cap;
    s->cap = -1;
    /* THE BOUND: a capture is a gadget of the screen shown, since route() drops it with any
       change of screen */
    if (i < 0 || i >= sc->ngadget) return;
    if (live(gadget_state(s, &sc->gadget[i], s->shown, 0)) && inside(sc, &sc->gadget[i], s->cursor_x, s->cursor_z))
        act(s, &sc->gadget[i]);
}

/* the sidebar as the view shows it; `under` reads the cursor unclamped, as sim.js's view does */
static void side_view(const WIKISIM* s, WIKIVSIDE* v)
{
    const WIKISCREEN* sc = screen_of(s, s->screen);
    const WIKIUNIT* under = over(s) == OVER_MAP ? unit_at(s, s->cursor_x, s->cursor_z) : NULL;
    int i;
    memset(v, 0, sizeof *v);
    v->h = s->lay->height;
    v->screen = s->screen;
    if (s->shown) snprintf(v->unit, sizeof v->unit, "%s", s->shown->id);
    v->page = s->shown ? s->shown->page : 0;
    /* sc->ngadget <= WIKI_SIDE_GADGETS, the length of state[] (tagpu_wikilayout_check) */
    v->nstate = sc->ngadget;
    for (i = 0; i < sc->ngadget; i++) {
        const WIKIGADGET* g = &sc->gadget[i];
        v->state[i] = gadget_state(s, g, s->shown, s->cap == i && inside(sc, g, s->cursor_x, s->cursor_z));
    }
    if (under) snprintf(v->under, sizeof v->under, "%s", under->id);
}

/* ---- the build trip: a scripted `build` input played out through the sidebar ----
   A data input {build: T} at tick t arms nothing itself: it becomes generated inputs, a glide, the
   pager presses and the button's own press, that go through exactly the path the player's clicks
   take in Try it. They are made when tick t is fed, from the state at t, and the arm is the
   release at t + WIKI_TRIP_UP whatever the route (tagpu_wikisim.h says why it is fixed). */

enum { GEN_MOVE, GEN_DOWN, GEN_UP };

static void gen_push(WIKISIM* s, int at, int kind, double x, double z)
{
    WIKIINPUT* in;
    /* THE BOUND: one trip makes at most WIKI_TRIP_EVENTS inputs (TRIP_FITS), and trip() starts
       the table afresh once the last trip's are all fed, which the data rule makes so before the
       next build: no other input stands in a trip */
    if (s->ntrip_in >= WIKI_TRIP_EVENTS) return;
    in = &s->trip_in[s->ntrip_in++];
    memset(in, 0, sizeof *in);
    in->at = at;
    in->down = in->up = in->build = -1;
    if (kind == GEN_MOVE) {
        in->has = WIKI_IN_MOVE;
        in->move.x = x;
        in->move.z = z;
    } else if (kind == GEN_DOWN) in->down = WIKI_KEY_LMB;
    else in->up = WIKI_KEY_LMB;
}

static XZ centre_of(const WIKISCREEN* sc, const WIKIGADGET* g)
{
    XZ c;
    c.x = -WIKI_SIDE_W + g->x + g->w / 2.0;
    c.z = sc->y + g->y + g->h / 2.0;
    return c;
}

typedef struct { int k; XZ press[WIKI_TRIP_NROWS - 1]; XZ button; } TRIPPLAN;

/* the plan: the shown unit's first page holding a live T button, the pagers the nearer way round
   (a tie goes to NEXT), and each press's point. Each point is the pager on the page it is pressed
   on, and every point is checked to land on its gadget, so the release at t + WIKI_TRIP_UP arms T
   in any layout this accepts. 0: T cannot be reached here. */
static int plan_trip(const WIKISIM* s, int T, TRIPPLAN* plan)
{
    const WIKIUNIT* u = s->shown;
    const WIKIPAGES* TP = u ? type_pages(s, u->type) : NULL;
    const int n = TP ? TP->npage : 0;
    const char* name = def(s, T)->name;
    int p = 0, gi = -1, k, i, j, c, page, knext, kprev, dir;
    const WIKISCREEN* sc;
    /* no data input presses a tab, so bp is set in a scripted demo; a build met on the orders page
       is locked */
    if (!n || !u->bp) return 0;
    for (k = 1; k <= n && !p; k++) {
        sc = screen_of(s, TP->page[k - 1]);
        for (i = 0; i < sc->ngadget; i++) {
            const WIKIGADGET* g = &sc->gadget[i];
            if (g->role == WIKI_ROLE_BUILD && !strcmp(g->unit, name) && live(gadget_state(s, g, u, 0))) {
                p = k;
                gi = i;
                break;
            }
        }
    }
    if (!p) return 0;
    c = u->page;
    knext = ((p - c) % n + n) % n;
    kprev = ((c - p) % n + n) % n;
    dir = knext <= kprev ? WIKI_ROLE_NEXT : WIKI_ROLE_PREV;
    plan->k = knext < kprev ? knext : kprev;
    /* THE BOUND: n <= WIKI_SIDE_PAGES = 7, so the nearer way round is at most 3 presses, the rows
       TRIP_ROWS has; and c is 1..n (route() says why), so every page below is too */
    if (plan->k >= WIKI_TRIP_NROWS || c < 1 || c > n) return 0;
    for (j = 0, page = c; j < plan->k; j++) {
        const int scr = TP->page[page - 1];
        sc = screen_of(s, scr);
        for (i = 0; i < sc->ngadget && !(sc->gadget[i].role == dir && live(gadget_state(s, &sc->gadget[i], u, 0))); i++) {}
        if (i == sc->ngadget) return 0;
        plan->press[j] = centre_of(sc, &sc->gadget[i]);
        if (gadget_at(s, scr, u, plan->press[j].x, plan->press[j].z) != i) return 0;
        page = dir == WIKI_ROLE_NEXT ? (page == n ? 1 : page + 1) : (page < 2 ? n : page - 1);
    }
    sc = screen_of(s, TP->page[p - 1]);
    plan->button = centre_of(sc, &sc->gadget[gi]);
    return gadget_at(s, TP->page[p - 1], u, plan->button.x, plan->button.z) == gi;
}

/* THE GLIDE, exact in both languages: step i of n from a to b is
   a + trunc((b - a) * (i*i*(3n - 2i)) / (n*n*n)), each axis, evaluated in that order. Step n is b
   itself: the formula reaches b only when b - a is a whole number, and the cursor must stand
   exactly on the button and exactly back where it started. */
static double glide_at(double a, double b, int i, int n)
{
    return i == n ? b : a + trunc((b - a) * (double)(i * i * (3 * n - 2 * i)) / (double)(n * n * n));
}

static void glide(WIKISIM* s, int t, XZ from, XZ to, int n, int t0)
{
    int i;
    for (i = 1; i <= n; i++) gen_push(s, t + t0 + i, GEN_MOVE, glide_at(from.x, to.x, i, n), glide_at(from.z, to.z, i, n));
}

static void trip(WIKISIM* s, int T)
{
    const int t = s->feeding;
    const WIKITRIPROW* R;
    TRIPPLAN plan;
    XZ a, cur;
    int j;
    /* OWNER QUESTION Q-O2, the recommended answer: a build this layout cannot reach stops the demo
       here, holding the frame under the entry's "Turned off in this mod" badge (decision 4),
       rather than playing on unarmed */
    if (!plan_trip(s, T, &plan)) {
        s->locked = T;
        return;
    }
    if (s->trip_next == s->ntrip_in) s->ntrip_in = s->trip_next = 0;
    a.x = s->cursor_x;
    a.z = s->cursor_z;
    R = &TRIP_ROWS[plan.k];
    cur = a;
    if (plan.k) {
        glide(s, t, a, plan.press[0], R->glide, 0);
        cur = plan.press[0];
        for (j = 0; j < plan.k; j++) {
            const int d = R->glide + j * (R->hold + R->gap);
            /* a mod whose pages move the pager: the cursor steps onto it, before the press of the
               same tick */
            if (plan.press[j].x != cur.x || plan.press[j].z != cur.z)
                gen_push(s, t + d, GEN_MOVE, plan.press[j].x, plan.press[j].z);
            gen_push(s, t + d, GEN_DOWN, 0, 0);
            gen_push(s, t + d + R->hold, GEN_UP, 0, 0);
            cur = plan.press[j];
        }
    }
    glide(s, t, cur, plan.button, plan.k ? R->back : R->glide, plan.k ? R->glide + plan.k * (R->hold + R->gap) : 0);
    gen_push(s, t + WIKI_TRIP_DOWN, GEN_DOWN, 0, 0);
    gen_push(s, t + WIKI_TRIP_UP, GEN_UP, 0, 0);
    glide(s, t, plan.button, a, WIKI_TRIP - WIKI_TRIP_UP, WIKI_TRIP_UP);
}

static void evaluate(WIKISIM* s, WIKIPLACE* p);

/* the build-menu mode: the ghost is manual_cursor's snapped site (no legality), or where the cursor
   is, as the build cursor 0x4197D0 rounds it */
static WIKIHOVER map_hover(WIKISIM* s)
{
    const XZ m = map_cursor(s);
    const double x = m.x, z = m.z;
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

/* None while the cursor is off the map, and the gesture's evaluation stands as the map last left
   it: the in-play handler calls 0x4197D0 only with main+0x2CC6 bit 1, the cursor in the viewport
   (0x499236..0x499241), and tagpu_place.c evaluates only from that call. */
static WIKIHOVER hover(WIKISIM* s)
{
    WIKIHOVER h;
    if (over(s) == OVER_MAP) return map_hover(s);
    memset(&h, 0, sizeof h);
    h.kind = WIKI_HOVER_NONE;
    return h;
}

/* the evaluation the stage shows, as tagpu_place.c's current() lets the packet have it: only while
   the building it was made for is still the one armed; NULL for none */
static const WIKIPLACE* placed(const WIKISIM* s)
{
    return s->place.kind != PLACE_NONE && s->mode == s->place.type ? &s->place : NULL;
}

/* ---- the placement gesture, tagpu_place.c as sim.js mirrors it ---- */

/* mirrors tagpu_place.c's cursor site: the build cursor's rectangle, snapped as the ghost is.
   PlaceCell holds int16, so the cell is held to its range. */
static PlaceCell cursor_site(WIKISIM* s)
{
    const int t = s->mode;
    const XZ m = map_cursor(s);
    PlaceCell p;
    CHOICE c;
    int cx, cz;
    if (!s->held[WIKI_KEY_ALT] && snapped_site(s, t, m.x, m.z, 0, s->held[WIKI_KEY_SHIFT], &c)) {
        cx = c.ox;
        cz = c.oz;
    } else {
        cx = origin(m.x, fpx(s, t));
        cz = origin(m.z, fpz(s, t));
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

/* A world position as tagpu_place.c's order positions hold it, 16.16 fixed point, for the shared
   take-off rule. Exact for every position that rule compares, so it answers as sim.js's doubles
   do: a footprint's centre is (cx + f/2) * 16, a multiple of 8, exact in 16.16 within the
   clamp. The clamp changes no answer: the rule only ever compares a site that is not refused --
   inside the map, so its centre lies within 0..WIKI_MAP_MAX * 16 = 2048 (the loader refuses a
   larger map) -- with an order, and an order clamped to +-8192 is still further than 16 from
   every such site, as it was before the clamp. Within the clamp no difference reaches 2^31, so
   place_within's wrapped compare never wraps. */
_Static_assert(WIKI_MAP_MAX * WIKI_CELL + 16 < 8192
               && (8192LL + WIKI_MAP_MAX * WIKI_CELL) * 65536 < 0x80000000LL,
               "a clamped order lies beyond the reach of every site on the map, and no difference wraps");
static int32_t fixed16(double v)
{
    return (int32_t)((v < -8192 ? -8192 : v > 8192 ? 8192 : v) * 65536);
}

/* mirrors tagpu_place.c take_offs for one builder: place_take_offs_one (tagpu_placegeo.h), the
   game's own function, over the builder's queue as it stands -- its build orders are the ones
   the rule reads -- and the sites, whose positions and refused flags `pos` holds. A site that
   takes one becomes WIKI_SITE_REMOVE. Leaves in s->by the site that takes each order off
   (PLACE_NO_SITE for none) and in `took` whether each site took one. */
static void take_offs_one(WIKISIM* s, const PlaceSitePos* pos, WIKISITE* sites, int n, const WIKIUNIT* u, uint8_t* took)
{
    int i, j;
    for (j = 0; j < u->norder; j++) {
        const XZ q = order_at(s, &u->order[j]);
        s->q[j].x = fixed16(q.x);
        s->q[j].z = fixed16(q.z);
        s->q[j].match = u->order[j].k == WIKI_ORDER_BUILD;
    }
    place_take_offs_one(s->q, (unsigned)u->norder, pos, (unsigned)n, s->by, took);
    for (i = 0; i < n; i++)
        if (took[i]) sites[i].state = WIKI_SITE_REMOVE;
}

/* the sites as the take-off rule reads them. A site turns only from WIKI_SITE_PLACE to
   WIKI_SITE_REMOVE after this, never to or from refused, so `pos` holds for every builder. */
static void site_positions(const WIKISIM* s, int t, const WIKISITE* sites, int n, PlaceSitePos* pos)
{
    int i;
    for (i = 0; i < n; i++) {
        const XZ at = site_centre(s, t, &sites[i]);
        pos[i].x = fixed16(at.x);
        pos[i].z = fixed16(at.z);
        pos[i].refused = sites[i].state == WIKI_SITE_REFUSED;
    }
}

/* mirrors tagpu_place.c take_offs over every selected builder, for the sites' states. A ring takes
   nothing off: a site a Shift-click would toggle is WIKI_SITE_REMOVE, and left out. */
static void take_offs(WIKISIM* s, int t, WIKISITE* sites, int n, int ring)
{
    PlaceSitePos pos[PLACE_MAX_SITES];
    uint8_t took[PLACE_MAX_SITES];
    int i;
    if (ring) {
        for (i = 0; i < n; i++)
            if (sites[i].state != WIKI_SITE_REFUSED && toggles(s, t, &sites[i])) sites[i].state = WIKI_SITE_REMOVE;
        return;
    }
    site_positions(s, t, sites, n, pos);
    for (i = 0; i < s->nunit; i++)
        if (is_builder(s, s->unit[i])) take_offs_one(s, pos, sites, n, s->unit[i], took);
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
    const XZ m = map_cursor(s);
    const WIKIUNIT* u = unit_at(s, m.x, m.z);
    int i, j, px, pz;
    if (u) {
        *tw = fpx(s, u->type);
        *th = fpz(s, u->type);
        *tx = origin(u->x, *tw);
        *tz = origin(u->z, *th);
        return 1;
    }
    px = cell_of(m.x);
    pz = cell_of(m.z);
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
    PlaceSitePos pos[PLACE_MAX_SITES];
    uint8_t took[PLACE_MAX_SITES];
    const int t = p->type, ring = p->kind == PLACE_RING;
    int i, j, b, changed = 0;
    for (i = 0; i < p->n; i++) {
        cells[i].cx = (int16_t)p->site[i].cx;
        cells[i].cz = (int16_t)p->site[i].cz;
    }
    classify(s, t, cells, p->n, ring, sites);
    site_positions(s, t, sites, p->n, pos);
    for (b = 0; b < s->nunit; b++) {
        WIKIUNIT* u = s->unit[b];
        if (!is_builder(s, u)) continue;
        if (!ring) take_offs_one(s, pos, sites, p->n, u, took);
        for (i = 0; i < p->n; i++)
            if (ring) took[i] = sites[i].state == WIKI_SITE_REMOVE;
        for (j = ring ? 0 : u->norder; j-- > 0;)
            if (s->by[j] != PLACE_NO_SITE) {
                splice_orders(u, j, 1);
                changed++;
            }
        for (i = 0; i < p->n; i++) {
            WIKIORDER o;
            if (sites[i].state == WIKI_SITE_REFUSED || took[i]) continue;
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
   else commits the last evaluation. A release over the sidebar reaches tagpu_place.c too: IdleTick
   hands the in-play handler a button's release the GUI pump took (0x4999CB..0x499A04, then
   0x499A1C), and the route at 0x4993BF passes it to place_button wherever the cursor is. Its
   record is never the evaluated one there (the evaluation stands as the map left it), so it is
   never the stock Shift-click: it commits the evaluation the map last made. */
static void release(WIKISIM* s)
{
    const WIKIPLACE* p;
    PlaceCell cur;
    s->has_gesture = 0;
    if (!s->held[WIKI_KEY_SHIFT]) return;
    p = placed(s);
    cur = cursor_site(s);
    if (p && over(s) == OVER_MAP && p->kind != PLACE_RING && p->n == 1 && p->site[0].cx == cur.cx
        && p->site[0].cz == cur.cz) {
        place_click(s, 1);
        return;
    }
    if (p) commit(s, p);
}

/* ---- orders ---- */

/* leaving the build-menu mode ends the gesture and clears the Shift-placement bit with it */
static void disarm(WIKISIM* s)
{
    s->mode = -1;
    s->has_gesture = 0;
    s->shift_placed = 0;
}

/* ARMING a building, the build button's branch of 0x41AA00, DISASSEMBLED: main+0x2CC3 = 0x0E
   (0x41AB89) and the type at main+0x2CC4 (0x41AB9C), and no write to main+0x2CC6. So a
   Shift-placement made earlier in this Shift hold keeps bit 5 (shift_placed), and releasing
   Shift disarms the building armed since as well: 0x4992D8..0x49930F tests bit 5, not the mode.
   tagpu_place.c leaves the bit too (it sets it only on a commit), and ends its gesture when the
   armed type changes (its evaluate, `s_gesture.type != c.type`), so a gesture outlives a re-arm
   of its own type only. */
static void arm(WIKISIM* s, int t)
{
    if (s->mode != t) s->has_gesture = 0;
    s->mode = t;
    s->has_drag = 0;
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
    const XZ c = map_cursor(s);
    int i, n = 0, m = 0;
    for (i = 0; i < s->nunit; i++)
        if (s->unit[i]->sel && s->unit[i]->owner == 0 && mobile(s, s->unit[i]->type)) n++;
    for (i = 0; i < s->nunit; i++) {
        WIKIUNIT* u = s->unit[i];
        WIKIORDER o;
        if (!(u->sel && u->owner == 0 && mobile(s, u->type))) continue;
        memset(&o, 0, sizeof o);
        o.k = WIKI_ORDER_MOVE;
        o.x = c.x + (m++ - (n - 1) / 2.0) * SPACING;
        o.z = c.z;
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
    XZ m;
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
    m = map_cursor(s);
    u = unit_at(s, m.x, m.z);
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
    const WIKIHOVER h = map_hover(s);
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
    const XZ m = map_cursor(s);
    const double x = m.x, z = m.z;
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

/* A scripted demo with the sidebar on plays a `build` out as its trip; Try it, and a demo without
   the sidebar, arm at once (the wiki never sends `build`, and the rule cases use it as their
   shortcut). */
static void input(WIKISIM* s, const WIKIINPUT* ev)
{
    if (ev->has & WIKI_IN_CAPTION) s->caption = ev->caption;
    if (ev->has & WIKI_IN_BUILD) {
        /* tagpu_wikidata_trips refused a trip's build naming no type before the sim started */
        if (s->trips) {
            if (ev->build >= 0 && ev->build < s->entry->ndef) trip(s, ev->build);
        } else if (ev->build >= 0 && ev->build < s->entry->ndef) arm(s, ev->build);
        else {
            disarm(s);
            s->has_drag = 0;
        }
    }
    if (ev->has & WIKI_IN_MOVE) {
        XZ c;
        s->cursor_x = ev->move.x;
        s->cursor_z = ev->move.z;
        c = map_cursor(s);
        if (s->has_drag && sq(c.x - s->drag_x0) + sq(c.z - s->drag_z0) > DRAG * DRAG) s->drag_active = 1;
    }
    if (ev->down == WIKI_KEY_Z || ev->down == WIKI_KEY_X) {
        s->held[ev->down] = 1;
        step_spacing(s, ev->down);
    } else if (ev->down >= 0 && ev->down < WIKI_NKEYS && !s->held[ev->down]) {
        const int mouse = ev->down == WIKI_KEY_LMB || ev->down == WIKI_KEY_RMB;
        int shift, where;
        s->held[ev->down] = 1;
        shift = s->held[WIKI_KEY_SHIFT];
        where = over(s);
        if (mouse) s->down_on[ev->down] = where;
        if (mouse && where == OVER_SIDE && on_screen(s)) press_side(s, ev->down);
        else if (where == OVER_MAP) {
            /* with a building armed a left press carrying Shift starts a gesture at the cursor
               site instead of placing */
            if (ev->down == WIKI_KEY_LMB) {
                if (s->mode >= 0 && shift) {
                    s->gesture = cursor_site(s);
                    s->has_gesture = 1;
                } else if (s->mode >= 0) place_click(s, 0);
                else {
                    const XZ c = map_cursor(s);
                    s->has_drag = 1;
                    s->drag_active = 0;
                    s->drag_x0 = c.x;
                    s->drag_z0 = c.z;
                }
            }
            if (ev->down == WIKI_KEY_RMB) {
                s->has_drag = 0;
                right_click(s, shift);
            }
        }
        /* off the map and off the screen the right press is the in-play handler's cancel and
           nothing else */
        else if (ev->down == WIKI_KEY_RMB && s->mode >= 0) disarm(s);
    }
    if (ev->up >= 0 && ev->up < WIKI_NKEYS && s->held[ev->up]) {
        s->held[ev->up] = 0;
        /* a release runs the map rules only for a press that went down on the map */
        if (s->cap >= 0 && s->cap_key == ev->up) release_side(s);
        else if (s->down_on[ev->up] == OVER_MAP) {
            if (ev->up == WIKI_KEY_LMB && s->has_gesture) release(s);
            else if (ev->up == WIKI_KEY_LMB && s->has_drag) {
                select_units(s, s->held[WIKI_KEY_SHIFT]);
                s->has_drag = 0;
            }
        }
        /* 0x4992D8: Shift found up after a placement in this hold disarms, as after a stock Shift-click */
        if (ev->up == WIKI_KEY_SHIFT) {
            if (s->shift_placed) disarm(s);
            s->shift_placed = 0;
        }
    }
    if (s->lay) route(s);
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

/* A step toward (tx, tz), ending `stop` short of it; 1 once there. The step turns the unit to face
   along it, at once: the simulation has no turn rate.

   THE HEADING IS A VECTOR, NOT AN ANGLE. An angle would come from atan2, which no standard rounds
   correctly, so its last bit differs between C libraries and JavaScript engines, and this file
   would drift from sim.js. The step's own dx / d and dz / d are + - * / and sqrt, each rounded
   once, so both simulations hold the same doubles. */
static int walk(const WIKISIM* s, WIKIUNIT* u, double tx, double tz, double stop)
{
    const double dx = tx - u->x, dz = tz - u->z, d = sqrt(dx * dx + dz * dz);
    double step;
    if (d <= stop) return 1;
    step = def(s, u->type)->max_velocity;
    if (d - stop < step) step = d - stop;
    u->hx = dx / d;
    u->hz = dz / d;
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

/* a builder in range works on st, and turns at once to face its centre (unchanged when it stands
   on the centre) */
static void credit(const WIKISIM* s, WIKIUNIT* u, WIKIUNIT* st, int how)
{
    const double dx = st->x - u->x, dz = st->z - u->z, d = sqrt(dx * dx + dz * dz);
    const double w = def(s, u->type)->worker_time / def(s, st->type)->build_time / TPS * s->PACE;
    if (d > 0) {
        u->hx = dx / d;
        u->hz = dz / d;
    }
    if (how == HOW_BUILD) st->work_build += w;
    else st->work_reclaim += w;
    st->worked = 1;
    u->job = st;
    u->job_how = how;
}

static void shift_order(WIKIUNIT* u) { splice_orders(u, 0, 1); }

/* the unit leaves the list and its memory goes; the sidebar's shown unit goes with it, which
   route() would make so on this tick's end anyway */
static void remove_unit(WIKISIM* s, WIKIUNIT* u)
{
    int i, k = 0;
    if (s->shown == u) s->shown = NULL;
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

/* THE CURSOR'S SHAPE (WIKI_CURSOR_*, the game's cursor for each): the build-menu mode, else a
   selection holding a mobile unit of the player's, else none. Quick Build's ghost changes nothing
   here: it is drawn from the packet and leaves the engine's cursor as the selection has it. Off
   the map it is the arrow: the GUI shows cursornormal [INFERRED, capture C5]. */
static int cursor_shape(const WIKISIM* s)
{
    int i;
    if (over(s) != OVER_MAP) return WIKI_CURSOR_ARROW;
    if (s->mode >= 0) return WIKI_CURSOR_BUILD;
    for (i = 0; i < s->nunit; i++)
        if (s->unit[i]->sel && s->unit[i]->owner == 0 && mobile(s, s->unit[i]->type)) return WIKI_CURSOR_MOVE;
    return WIKI_CURSOR_ARROW;
}

/* the sequence a shape shows: the build cursor is the arrow's cursornormal (tagpu_wikisim.h) */
static int cursor_seq(int shape) { return shape == WIKI_CURSOR_BUILD ? WIKI_CURSOR_ARROW : shape; }

/* THE SHAPE AFTER EVERY CHANGE, and `now`, the first tick that shows it, as the tick its animation
   began when it shows another sequence. The engine installs a cursor only when its index differs
   from the installed one (0x499254, 0x4992A9 compare main+0x2CBE first), so the arrow and the
   build cursor, both cursornormal, run on through each other. */
static void cursor_tick(WIKISIM* s, int now)
{
    const int k = cursor_shape(s);
    if (cursor_seq(k) != cursor_seq(s->cursor_shape)) s->cursor_since = now;
    s->cursor_shape = k;
}

/* a script's inputs of tick `t` are shown first by tick t, which the step's cursor_tick records; a
   tick's generated inputs come before its data inputs, and within a trip's tick a move comes
   before a press */
static void feed(WIKISIM* s, int t)
{
    s->feeding = t;
    while (s->trip_next < s->ntrip_in && s->trip_in[s->trip_next].at <= t) input(s, &s->trip_in[s->trip_next++]);
    while (s->next < s->nscript && s->script[s->next]->at == t) input(s, s->script[s->next++]);
}

/* the player's input stands between two ticks: the view of the tick the sim stands at shows it */
void tagpu_wikisim_input(WIKISIM* s, const WIKIINPUT* ev)
{
    input(s, ev);
    cursor_tick(s, s->tick);
}

void tagpu_wikisim_step(WIKISIM* s)
{
    const int t = s->tick + 1;
    feed(s, t);
    advance(s);
    s->tick = t;
    /* a locked build ends the demo on its own tick, which then holds under the entry's badge */
    s->done = s->tick >= s->end || s->locked >= 0;
    if (s->lay) route(s);
    s->hover = hover(s);
    cursor_tick(s, t);
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
    const WIKIPLACE* p = placed(s) && s->place.n ? &s->place : NULL;
    const WIKIHOVER* h = &s->hover;
    const int gh = !p && (h->kind == WIKI_HOVER_PLACE || h->kind == WIKI_HOVER_QUICK);
    int i, j, k, n = 0;

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
            w->hx = u->hx;
            w->hz = u->hz;
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

    v->cursor_x = s->cursor_x;
    v->cursor_z = s->cursor_z;
    /* every change of the shape -- an input, a tick, the scene -- records it (cursor_tick) */
    v->cursor_shape = s->cursor_shape;
    v->cursor_since = s->cursor_since;
    memcpy(v->held, s->held, sizeof v->held);
    if ((v->has_drag = s->has_drag && s->drag_active)) {
        const XZ c = map_cursor(s);
        v->drag_x0 = s->drag_x0;
        v->drag_z0 = s->drag_z0;
        v->drag_x1 = c.x;
        v->drag_z1 = c.z;
    }
    v->caption = s->caption;
    v->pace = s->PACE;
    v->mode = s->mode;
    /* the column, only with the sidebar on; then the build a demo could not reach */
    if ((v->has_side = s->lay != NULL)) side_view(s, &v->side);
    v->locked = s->locked;
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
    const WIKIPLACE* p = placed(s) && s->place.n ? &s->place : NULL;
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
        fnv_double(&h, u->hx);
        fnv_double(&h, u->hz);
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

    fnv_int(&h, s->lay != NULL);
    if (s->lay) {
        WIKIVSIDE v;
        side_view(s, &v);
        fnv_int(&h, v.h);
        fnv_str(&h, s->lay->screen[v.screen].name);
        fnv_str(&h, v.unit);
        fnv_int(&h, v.page);
        fnv_int(&h, v.nstate);
        for (i = 0; i < v.nstate; i++) fnv_int(&h, v.state[i]);
        fnv_str(&h, v.under);
    }
    fnv_str(&h, s->locked >= 0 ? def(s, s->locked)->name : "");
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
        WIKIUNIT* u = add(s, d->def, d->owner, d->x, d->z, d->built, d->id, d->label, d->hx, d->hz);
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

/* the scene gives the player a mobile builder: sim.js builders() without the selection test */
static int scene_builder(const WIKIENTRY* e, const WIKISCENE* sc)
{
    int i;
    for (i = 0; sc && i < sc->nunit; i++) {
        const WIKIDEF* t = &e->def[sc->unit[i].def];
        if (sc->unit[i].owner == 0 && t->max_velocity > 0 && t->worker_time > 0) return 1;
    }
    return 0;
}

int tagpu_wikisim_init(WIKISIM* s, const WIKIENTRY* e, const WIKIDEMO* d, int scripted, const WIKILAYOUT* lay,
                       char* err, size_t errn)
{
    const WIKISCENE* a = &e->scene;
    const WIKISCENE* b = d ? &d->scene : NULL;
    const WIKISCENE* sel = b && b->has_selected ? b : a->has_selected ? a : NULL;
    const size_t cells = (size_t)a->w * a->h;
    int i, k;
    memset(s, 0, sizeof *s);
    /* a layout is checked whole before any sim reads it, as sim.js's checked() does; every index
       read out of it below is then in range */
    if (lay && !tagpu_wikilayout_check(lay, err, errn)) return 0;
    /* THE SIDEBAR, on or off for the sim's whole life: a demo shows it when its `sidebar` says so,
       by default when the scene gives the player a mobile builder; Try it shows it whenever the
       scene does, whatever the demo said */
    s->lay = lay && (scripted && d && d->has_sidebar ? d->sidebar : scene_builder(e, a) || scene_builder(e, b))
           ? lay : NULL;
    s->trips = scripted && s->lay;
    if (s->trips && d && !tagpu_wikidata_trips(d, err, errn)) return 0;
    for (i = 0; i < e->ndef; i++) {
        s->ltype[i] = -1;
        for (k = 0; s->lay && k < s->lay->ntype; k++)
            if (!strcmp(s->lay->type[k].name, e->def[i].name)) s->ltype[i] = k;
    }
    s->cap = -1;
    s->locked = -1;
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
    if (s->lay) route(s);
    feed(s, 0);
    s->done = s->locked >= 0;
    s->hover = hover(s);
    s->cursor_shape = cursor_shape(s);
    s->cursor_since = 0;
    if (s->oom) goto oom;
    return 1;
oom:
    snprintf(err, errn, "wiki sim: out of memory");
    tagpu_wikisim_free(s);
    return 0;
}
