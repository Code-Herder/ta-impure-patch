#ifndef TASIM_SWEEP_H
#define TASIM_SWEEP_H
/* tasim_sweep.c -- the reclaim sweep's rules over WORLDQ, one copy for the game (tagpu_sweep.c, through the
   engine's instance) and the in-game wiki (tagpu_wikisim.c, through the core's): which features a ring
   holds, the selected reclaimers' split inputs, and the commit's removals and appends. The ring's test and
   the split are tagpu_sweepgeo.h's. research/notes/modules/reclaim-sweep.md.

   Engine-free: everything it knows of a world comes through the WORLDQ it is given, and every list it reads
   is copied into the caller's SWEEP_SCRATCH. */
#include "worldq.h"
#include "tagpu_sweepgeo.h"

/* A list stops growing at SWEEP_LIST_CAP orders, below the 4096 a recorded list holds (REC_ORDERS_MAX,
   tagpu_recfmt.h), so no commit makes a list the recorder would cut. */
#define SWEEP_LIST_CAP     1024u
#define SWEEP_BUILDERS_MAX 1500u     /* the units a seat holds, at most: the raised limit */
#define SWEEP_FOOT_MAX     16        /* the reach of a feature's footprint below its anchor's cell */

enum { SWEEP_KIND_METAL, SWEEP_KIND_ENERGY, SWEEP_KIND_ALL };   /* tagpu_sweep.h's SWEEP_METAL.. */

/* A feature the ring takes: its anchor cell (az * W + ax) and footprint, its centre (world px), its order
   position (16.16, at the ground's height), its definition's amounts and its squared distance to the
   ring's centre. */
typedef struct SWEEP_TARGET {
    int32_t  cell, ax, az, fx, fz, cx, cz;
    int32_t  pos[3];
    uint16_t def;
    float    metal, energy;
    uint64_t d2;
} SWEEP_TARGET;

/* What a sweep reads besides the world: the ring (its centre and radius in world px), the kind, the seat's
   selected live units, slots ascending -- the rule takes the reclaimers out of them -- and Shift. */
typedef struct SWEEP_IN {
    int32_t         c[3];
    int32_t         r;
    unsigned        kind;
    const uint16_t* selected;
    int             nselected;
    int             shift;
} SWEEP_IN;

/* The caller's scratch, for one call at a time: a UI's static, or the wiki's per sim. */
typedef struct SWEEP_SCRATCH {
    SWEEP_TARGET t[SWEEP_MAX_TARGETS];
    SweepTarget  st[SWEEP_MAX_TARGETS];
    uint16_t     who[SWEEP_MAX_TARGETS], seq[SWEEP_MAX_TARGETS], cell_order[SWEEP_MAX_TARGETS];
    uint8_t      drop[SWEEP_MAX_TARGETS];
    WQ_ORDER     list[WQ_LIST_MAX];
    uint16_t     cut[WQ_LIST_MAX];          /* the places of the orders a ring without Shift takes off */
    uint16_t     slot[SWEEP_BUILDERS_MAX];   /* the reclaimers, slots ascending */
    SweepBuilder b[SWEEP_BUILDERS_MAX];
    unsigned     nb;
    uint8_t      act[3];                     /* the reclaim family: RECLAIM, VTOL_RECLAIM, RESURRECT */
} SWEEP_SCRATCH;

/* A commit's counts, its log line's. */
typedef struct SWEEP_RES {
    unsigned found, taken, builders, issued, unassigned, mismatch, capped, cancelled, stale, more;
} SWEEP_RES;

/* a feature's amounts are of `kind` (SWEEP_KIND_*): the kind's amount above 0, any for ALL */
int tasim_sweep_kind_ok(float metal, float energy, unsigned kind);
/* The selected reclaimers out of `in->selected` into `scr`: the count. */
unsigned tasim_sweep_reclaimers(const WORLDQ* q, const SWEEP_IN* in, SWEEP_SCRATCH* scr);
/* The reclaim family's three order bytes into `scr`: 0 when the order table lacks one. */
int tasim_sweep_actions(const WORLDQ* q, SWEEP_SCRATCH* scr);
/* The order the resolver gives the reclaimer at `slot` for a ring at `pos` (no target unit), when it is of
   the reclaim family; 0 otherwise. After tasim_sweep_actions. */
int tasim_sweep_order_at(const WORLDQ* q, const SWEEP_SCRATCH* scr, int slot, const int32_t pos[3]);
/* The feature anchored at or covering cell (x, z) when the ring's rules take it for `kind`: 1 with it in
   `out` (its d2 0). */
int tasim_sweep_feature_takes(const WORLDQ* q, int x, int z, unsigned kind, SWEEP_TARGET* out);
/* WHICH FEATURES A RING HOLDS, nearest the centre first, at most `cap`, into `out`; `*more` the ones past
   it. */
unsigned tasim_sweep_targets(const WORLDQ* q, const SWEEP_IN* in, SWEEP_TARGET* out, unsigned cap, unsigned* more);
/* THE COMMIT, all but the click's tail: 0 and nothing done when there is no reclaimer or the order table
   lacks a reclaim order, else 1 with the counts in `res`. The tail is the caller's, when anything was issued
   or cancelled. */
int tasim_sweep_commit(const WORLDQ* q, const SWEEP_IN* in, SWEEP_RES* res, SWEEP_SCRATCH* scr);

#endif
