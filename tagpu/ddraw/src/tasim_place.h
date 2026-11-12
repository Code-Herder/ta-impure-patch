#ifndef TASIM_PLACE_H
#define TASIM_PLACE_H
/* tasim_place.c -- the multi-site placement gesture's rules over WORLDQ, one copy for the game (tagpu_place.c,
   through the engine's instance opened with the build cursor's own site test) and the in-game wiki
   (tagpu_wikisim.c, through the core's): which selected units are the gesture's builders, each site judged
   by the site test, the take-off rule that decides which queued orders a site takes off instead of placing,
   and the commit's removals and appends. The shapes, the ring and the take-off rule's arithmetic are
   tagpu_placegeo.h's. research/notes/modules/placement.md.

   Engine-free: everything it knows of a world comes through the WORLDQ it is given, and every list it reads
   is copied into the caller's PLACE_SCRATCH. */
#include "worldq.h"
#include "tagpu_placegeo.h"

/* The caps. A commit makes at most PLACE_ORDER_BUDGET orders (sites x builders): the preview cuts the
   shape to what would be ordered. A builder's list stops growing at PLACE_LIST_CAP nodes
   (tagpu_placegeo.h). PLACE_PLANS bounds the builders' copied orders together; WQ_LIST_MAX every walk of a
   list. */
#define PLACE_ORDER_BUDGET 4096u
#define PLACE_PLANS        16384u
#define PLACE_BUILDERS_MAX 1500u    /* the units a seat holds, at most: the raised limit */

enum { PLACE_SITE_FREE, PLACE_SITE_RED, PLACE_SITE_TAKE };

/* A site: its footprint's first cell (the caller's), and as the rule judged it the altitude the build
   cursor gives it and its state: free, refused by the site test (red), or taking a queued order off. */
typedef struct PLACE_SITE { int32_t cx, cz, alt; uint8_t state; } PLACE_SITE;

/* What a gesture's rule reads besides the world: the armed type, whether the shape is a ring, and the
   seat's selected live units, slots ascending -- the rule takes the builders out of them. */
typedef struct PLACE_IN {
    int             type, ring;
    const uint16_t* selected;
    int             nselected;
} PLACE_IN;

/* One copied order of a builder's that the rule reads: `match` (in `order`) when it is of the builder's
   own build action, `build` when it is a placed build; its builder, its place in the list, its flag word. */
typedef struct PLACE_PLAN { uint16_t type, b, place; uint8_t build; uint32_t flags; } PLACE_PLAN;

/* The caller's scratch, for one gesture's evaluation or commit at a time: a UI's static, or the wiki's per
   sim. The builder set and the plans are the last tasim_place_builders' and tasim_place_classify's. */
typedef struct PLACE_SCRATCH {
    WQ_ORDER   list[WQ_LIST_MAX];
    uint16_t   slot[PLACE_BUILDERS_MAX];     /* the builders, slots ascending, and their build actions */
    uint8_t    action[PLACE_BUILDERS_MAX];
    unsigned   nb;
    PLACE_PLAN plan[PLACE_PLANS];            /* the builders' runs, builder by builder, each in list order */
    PlaceOrder order[PLACE_PLANS];
    uint16_t   by[PLACE_PLANS];              /* the site that takes each plan off, PLACE_NO_SITE none */
    unsigned   nplan;
    int        planned;                      /* 0: more than PLACE_PLANS orders, every site refused */
} PLACE_SCRATCH;

typedef struct PLACE_COMMIT_RES { unsigned red, issued, removed, stale, capped_builders; int planned; } PLACE_COMMIT_RES;

/* The gesture's builders out of `in->selected` into `scr`: the count. */
unsigned tasim_place_builders(const WORLDQ* q, const PLACE_IN* in, PLACE_SCRATCH* scr);
/* the sites a commit may order for `builders` builders, at most */
unsigned tasim_place_budget(unsigned n, unsigned builders);
/* A builder's queued build whose footprint holds cell (px, pz), first in slot order, then list order: 1 with
   its first cell and footprint. Over tasim_place_builders' set. */
int tasim_place_queued_at(const WORLDQ* q, int px, int pz, int* tx, int* tz, int* tw, int* th, PLACE_SCRATCH* scr);
/* The builders' lists, copied as values into `scr`, builder by builder and each in list order: the orders
   of the builder's own build action and the placed builds. 0 when they overflow PLACE_PLANS: classify then
   refuses every site rather than order what it could not check. Over tasim_place_builders' set. */
int tasim_place_snapshot(const WORLDQ* q, PLACE_SCRATCH* scr);
/* The `n` sites judged (their cx, cz the caller's) against the snapshot: the red count. */
unsigned tasim_place_classify(const WORLDQ* q, const PLACE_IN* in, PLACE_SITE* sites, unsigned n, PLACE_SCRATCH* scr);
/* THE COMMIT: the builders, the snapshot and the sites taken afresh, then per builder the orders the take-off
   rule gave a site removed and the other sites that are not red appended in `order`'s order. */
void tasim_place_commit(const WORLDQ* q, const PLACE_IN* in, PLACE_SITE* sites, unsigned n, const uint16_t* order,
                        PLACE_COMMIT_RES* res, PLACE_SCRATCH* scr);

#endif
