#ifndef TASIM_H
#define TASIM_H
/* tasim.h -- the shared simulation core's internal interface: what its modules, the tasim_*.c files, and the
   in-game wiki's front end, tagpu_wikisim.c, call across files. research/notes/impai/shared-core.md names the
   modules and what each holds; each function's contract is the comment at its definition.

   THE CORE'S CONTRACT is tagpu_wikisim.h's: engine-free, Win32-free and without globals; every piece of state in
   the WIKISIM its caller owns; no clock and no randomness; doubles with each operation rounded once, so every
   tasim_*.c is compiled on SSE2 as tagpu_wikisim.c is (the Makefile's THE WIKI'S SIMULATION), and
   every build agrees bit for bit. One WIKISIM is the whole state, and the core reads one part of it the front end
   keeps: the sidebar -- a type's pages (tasim_npages, at a unit's create) and the unit it shows, which
   tasim_remove_unit clears. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "tagpu_wikisim.h"
#include "tasim_defs.h"

#define CELL    WIKI_CELL
#define TPS     WIKI_TPS
/* a cell far enough off any map for every test to refuse it, and small enough that no cell
   arithmetic below can overflow: a wild cursor answers as off the map */
#define CELL_BOUND (1 << 20)

enum { HOW_BUILD, HOW_RECLAIM, HOW_FEATURE };

/* Math.round: half rounds up. x - floor(x) is exact wherever it can fall on either side of 0.5,
   so this agrees with it for every double. */
static inline double tasim_js_round(double x)
{
    double r = floor(x);
    return x - r >= 0.5 ? r + 1 : r;
}

static inline int tasim_bound_cell(double c)
{
    return c < -CELL_BOUND ? -CELL_BOUND : c > CELL_BOUND ? CELL_BOUND : (int)c;
}

typedef struct { double x, z; } XZ;

/* ---- the grid ---- */

static inline int tasim_cell_of(double v) { return tasim_bound_cell(floor(v / CELL)); }

typedef struct { int ox, oz, fx, fz; } BOX;

static inline int tasim_overlap(BOX a, BOX b) { return a.ox < b.ox + b.fx && b.ox < a.ox + a.fx && a.oz < b.oz + b.fz && b.oz < a.oz + a.fz; }

static inline BOX tasim_box(const WIKISIM* s, int t, int ox, int oz)
{
    BOX b;
    b.ox = ox;
    b.oz = oz;
    b.fx = tasim_fpx(s, t);
    b.fz = tasim_fpz(s, t);
    return b;
}

/* a world position in 16.16, truncated toward zero and held to the int32 range */
static inline int32_t tasim_fix16(double v)
{
    const double t = trunc(v * 65536.0);
    return t < -2147483648.0 ? INT32_MIN : t > 2147483647.0 ? INT32_MAX : (int32_t)t;
}

/* the cell a footprint `f` cells wide stands on at world coordinate `p`, rounded as the engine
   rounds both the unit's (+0x76/+0x78, 0x48AA00..0x48AA32) and a move's goal (0x44CF60):
   (P - (f << 19) + 0x80000) >> 20 on 16.16 fixed point */
static inline int tasim_foot_cell(double p, int f)
{
    return (int)floor((p - f * (CELL / 2) + CELL / 2) / CELL);
}

/* two bodies overlap: they have area in common (tasim_move.c, MOVEMENT) */
static inline int tasim_hits(WIKIRECT a, WIKIRECT b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1; }

/* ---- tasim_cob.c: the units' scripts, through the in-game wiki's COB host (tagpu_wikicob.h) ---- */

int     tasim_facts(WIKISIM* s);
void    tasim_cob_call(WIKISIM* s, const WIKIUNIT* u, const char* script, int how, int argc, const int32_t* args);

/* ---- tasim_world.c: units and features by id, a unit's entry and leaving, the site test, a kill ---- */

WIKIUNIT* tasim_new_unit(WIKISIM* s, int type, int owner, double built, const char* id, const char* label);
WIKIUNIT* tasim_seat_unit(WIKISIM* s, WIKIUNIT* u, int ox, int oz);
WIKIUNIT* tasim_add(WIKISIM* s, int type, int owner, double x, double z, double built, const char* id, const char* label,
                    double hx, double hz);
WIKIUNIT* tasim_by_id(const WIKISIM* s, const char* id);
WIKIUNIT* tasim_structure_at(const WIKISIM* s, int t, int ox, int oz);
BOX       tasim_feat_box(const WIKIFEAT* f);
WIKIFEAT* tasim_feat_by_id(const WIKISIM* s, const char* id);
WIKIFEAT* tasim_feat_at(const WIKISIM* s, double x, double z);
void      tasim_feat_remove(WIKISIM* s, WIKIFEAT* f);
int       tasim_interior(const WIKISIM* s, int t, int ox, int oz);
int       tasim_vent_under_g(const WIKISIM* s, const PlaceCell* v, int t, int ox, int oz);
int       tasim_site_ok(const WIKISIM* s, int t, int ox, int oz, const WIKIUNIT* ignore);
XZ        tasim_centre(const WIKISIM* s, int t, int ox, int oz);
void      tasim_remove_unit(WIKISIM* s, WIKIUNIT* u);
int       tasim_grow(WIKISIM* s, void** p, int* cap, int need, size_t size);
void      tasim_kill(WIKISIM* s, const char* id, int by);

/* ---- tasim_orders.c: the bounded order list and its kinds ---- */

int       tasim_push_order(WIKISIM* s, WIKIUNIT* u, const WIKIORDER* o);
void      tasim_splice_orders(WIKIUNIT* u, int i, int n);
void      tasim_shift_order(WIKIUNIT* u);
int       tasim_started(const WIKISIM* s, const WIKIORDER* o);
void      tasim_assign(WIKISIM* s, WIKIUNIT* u, const WIKIORDER* os, int n, int shift);
WIKIORDER tasim_order_build(int t, int ox, int oz);
WIKIORDER tasim_order_target(int k, const char* id);

/* ---- tasim_move.c: bodies, the step rule and the planner (MOVEMENT) ---- */

enum { GO_ARRIVED = 1, GO_STEPPED, GO_WAITING, GO_STUCK };
enum { TARGET_POINT, TARGET_SITE, TARGET_UNIT };

/* A TARGET, a point, a site or a unit: the point aimed at; in range within `stop` of r,
   or touching `body`, the target's own body; a straight walk ends `clip` short of the point; the
   planner's goal is within `plan` of r, or touching `body`; `serial` and `kind` name it for the
   leg */
typedef struct {
    double   x, z, stop, clip, plan;
    WIKIRECT r, body;
    int      has_body, serial, kind;
} TARGET;

WIKIRECT  tasim_rect_of(const WIKISIM* s, int t, int ox, int oz);
WIKIRECT  tasim_body_at_type(const WIKISIM* s, int t, double x, double z);
WIKIRECT  tasim_body(const WIKISIM* s, const WIKIUNIT* v);
WIKIRECT  tasim_feat_rect(const WIKIFEAT* f);
TARGET    tasim_to_point(const WIKIORDER* o, double x, double z);
TARGET    tasim_to_site(const WIKISIM* s, const WIKIORDER* o, const WIKIUNIT* u, WIKIRECT r);
TARGET    tasim_to_unit(const WIKISIM* s, const WIKIORDER* o, const WIKIUNIT* g);
TARGET    tasim_to_feat(const WIKIORDER* o, const WIKIFEAT* f);
int       tasim_go(WIKISIM* s, WIKIUNIT* u, const TARGET* t);
int       tasim_evacuate(WIKISIM* s, WIKIUNIT* u);
int       tasim_reach(WIKISIM* s, const WIKIORDER* o, WIKIUNIT* u, int t, int ox, int oz);

/* ---- tasim_econ.c: a game's economy -- the ledgers, the pass, the extractors' rate ---- */

int32_t   tasim_extractor_speed(const WIKISIM* s, const WIKIUNIT* u);
int       tasim_econ_request(WIKIUNIT* u, double e, double m);
void      tasim_econ_counts(WIKISIM* s);
void      tasim_econ_pass(WIKISIM* s);

/* ---- tasim_work.c: a unit appears, its orders' work, the tick's advance ---- */

void      tasim_spawn(WIKISIM* s, WIKIUNIT* u);
void      tasim_advance(WIKISIM* s);
void      tasim_drop_work(WIKISIM* s);

/* ---- tasim_fx.c: the nano spray and the sound, presentation the hash carries ---- */

void      tasim_spray(WIKISIM* s, WIKIUNIT* u, int t);
void      tasim_spray_tick(WIKISIM* s, int t);
void      tasim_sound_named(WIKISIM* s, int name);
void      tasim_notice(WIKISIM* s, const WIKIUNIT* u, int kind);
void      tasim_notice_active(WIKISIM* s, WIKIUNIT* u);
void      tasim_sound_tick(WIKISIM* s);
void      tasim_begin(WIKISIM* s, WIKIUNIT* u, const WIKIORDER* o);
int       tasim_cant_said(const WIKIUNIT* u);
void      tasim_death_sound(WIKISIM* s, const WIKIUNIT* u);

#endif
