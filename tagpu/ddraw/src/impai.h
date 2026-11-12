#ifndef IMPAI_H
#define IMPAI_H
/* impai.h -- ImpAI's engine-free half: what each seat knows of every unit slot and the metal patches it has
   seen, kept by one tick a seat over a WORLDQ (worldq.h), the engine's (tagpu_impai.c) or the shared core's
   (tests/wikitest.c `impai`); the reach labels of a movement class; and the maps derived from all of it.
   research/notes/modules/impai.md, What a seat knows.

   THE INFORMATION RULE. A seat's row of a slot changes only through what the seat observes: the WORLDQ's
   observe (its own units, its line of sight, an ally's shared radar), its own radar pass, and its own sight of
   the block where it last knew something. A death or a slot reuse the seat cannot see changes nothing it knows,
   so no engine fact reaches a decision through a row.

   No file of this half includes the engine header (thread-split.allow's default-deny), so it cannot read
   anything but its arguments: what it keeps and derives is a function of the WORLDQ's answers, its own state
   and the content tables it is given. Integers only, but for impai_threat_from_roles's one rounding of the
   classifier's figures; no clock, no random generator. */
#include <stddef.h>
#include <stdint.h>
#include "worldq.h"
#include "tasim_snap.h"
#include "tasim_roles.h"

/* IMPAI_SEEN.how: the classes the seat holds the slot by now */
#define IMPAI_OWN     0x01u
#define IMPAI_LOS     0x02u
#define IMPAI_SHARED  0x04u
#define IMPAI_RADAR   0x08u
#define IMPAI_SONAR   0x10u
#define IMPAI_GHOST   0x20u     /* none any longer: the row is the last thing known, at its block        */
#define IMPAI_NOW     (IMPAI_OWN | IMPAI_LOS | IMPAI_SHARED)
#define IMPAI_PASS    (IMPAI_RADAR | IMPAI_SONAR)

/* One seat's knowledge of one slot, 8 bytes. An all-zero row is empty: the seat knows nothing there. */
typedef struct IMPAI_SEEN {
    uint16_t type;              /* the type last identified; WQ_NONE when never identified                */
    uint8_t  how;               /* IMPAI_*                                                                */
    uint8_t  owner;             /* 0..9, 0xFF when never identified                                       */
    uint16_t bx, bz;            /* the 32-px block last known                                             */
} IMPAI_SEEN;
typedef struct IMPAI_ROW { IMPAI_SEEN seat[WQ_SEATS]; } IMPAI_ROW;     /* keyed by unit slot            */

/* A seat's metal patches: the published set in the first half, the scan in progress in the second, so a
   half-done scan is never read. */
#define IMPAI_CLUSTERS_HALF 256
#define IMPAI_CLUSTERS      (2 * IMPAI_CLUSTERS_HALF)

/* One seat's progress through its observation, plain values. */
typedef struct IMPAI_SEAT {
    uint16_t obs_cursor;        /* the next slot the line-of-sight slice looks at                         */
    uint16_t clu_row;           /* the patch scan's next row                                              */
    uint16_t nclusters;         /* the published patches                                                  */
    uint16_t npending;          /* the scan's patches so far                                              */
    WQ_RADAR radar;             /* the radar pass in progress                                             */
    uint32_t known_gen;         /* bumped on every write of this seat's rows or patches                   */
    uint32_t radar_done;        /* the gametime the last pass was published                               */
} IMPAI_SEAT;

/* One seat's arrays, the caller's: its rows (nrows, at least the world's nslots), its radar pass (nbytes, at
   least WQ_RADAR_BYTES(nslots)) and its IMPAI_CLUSTERS patches. */
typedef struct IMPAI_ARRAYS {
    IMPAI_ROW*  rows;
    int         nrows;
    uint8_t*    bits;
    int         nbytes;
    SNAP_PATCH* clusters;
} IMPAI_ARRAYS;

/* The budgets of one seat's tick, each a count the loops are bounded by. */
typedef struct IMPAI_BUDGET {
    uint32_t obs_slice;         /* slots the line-of-sight slice looks at                                 */
    uint32_t radar_visits;      /* the radar pass's target visits, rounded up to one walk                 */
    uint32_t clu_rows;          /* map rows the patch scan reads                                          */
    uint32_t clu_flood;         /* cells its floods may reach                                             */
} IMPAI_BUDGET;

/* What one tick did, against its bounds: `slots` the observe calls (the slice's, and a completed pass's
   publish), `visits` the radar pass's, `cells` the patch scan's cells read, `flood` its floods' cells;
   `scans` 1 when the tick ended a patch scan's pass and published its patches. */
typedef struct IMPAI_WORK {
    uint32_t slots, visits, cells, flood, scans;
} IMPAI_WORK;

/* The tick's phase of a radar pass: a seat starts one when idle and gametime % 30 == impai_radar_phase(seat),
   staggered so two seats never start on one tick. */
int impai_radar_phase(int seat);

/* One tick of seat q->seat: the line-of-sight slice, the radar pass's step and, when it completes, its
   publish, then the patch scan's rows with `scr` (the caller's, for this call; NULL skips the scan). Returns
   0, or -1 when the arrays are short of the world (nothing is written). */
int impai_know_tick(const WORLDQ* q, IMPAI_SEAT* st, const IMPAI_ARRAYS* a, const IMPAI_BUDGET* b,
                    SNAP_SCRATCH* scr, IMPAI_WORK* w);

/* ---- reach: which 32-px blocks a movement class can travel between ---- */

/* the class's code at cell (cx, cz), worldq.h's passable */
typedef int (*IMPAI_PASSFN)(const void* ctx, int mclass, int cx, int cz);
#define IMPAI_REACH_MANY 0xFFFFu    /* a component past the labels a u16 holds                            */

/* the scratch impai_reach_label needs for a w x h map, bytes */
size_t impai_reach_work(int w, int h);
/* THE REACH LABELS of class `mclass` on a w x h map (even, from the WORLDQ's map): the passable cells (code
   not 0) labelled by 4-connectivity, 1 up, and each 32-px block given the label of its first passable cell in
   row-major order, 0 when it has none, into `labels`, (w / 2) x (h / 2). Returns the components found. */
int impai_reach_label(IMPAI_PASSFN fn, const void* ctx, int mclass, int w, int h, uint16_t* labels, void* work);

/* ---- the maps ---- */

/* A unit type's threat and worth, from the role classifier (tasim_roles.h): damage per second against ground,
   air and water x 16 (per tick x 30 x 16), its ground range in px and its value (metal + energy / R), each
   rounded once and held to its field; its TASIM_LAYER and TASIM_RF_* flags' low byte. 16 bytes. */
typedef struct IMPAI_THREAT {
    uint16_t dps_ground, dps_air, dps_water;
    uint16_t range;
    uint32_t value;
    uint8_t  layer, flags;
    uint16_t pad;
} IMPAI_THREAT;
void impai_threat_from_roles(const TASIM_ROLE_OUT* out, int n, IMPAI_THREAT* t);

#define IMPAI_CELL_PX  256      /* a map cell of the maps                                                */
#define IMPAI_GRID_MAX 128      /* 32767 px, the coordinate limit, over 256                              */
enum { IMPAI_THREAT_GROUND, IMPAI_THREAT_AIR, IMPAI_THREAT_WATER, IMPAI_THREATS };

/* What the maps are a function of: one seat's rows and patches, its allies (bit j: record j), the threat
   table (NULL: no threat, no influence) indexed by type and the map's size in 16-px cells. */
typedef struct IMPAI_VIEW {
    int                 seat;
    const IMPAI_ROW*    rows;
    int                 nrows;
    const SNAP_PATCH*   clusters;
    int                 nclusters;
    uint32_t            allies;
    const IMPAI_THREAT* threat;
    int                 ntypes;
    int                 w, h;
} IMPAI_VIEW;

/* The maps, gw x gh cells of IMPAI_CELL_PX, into the caller's arrays of IMPAI_GRID_MAX^2 each:
   threat[layer], each identified enemy row's dps stamped on the cells whose centres lie within its type's
   range of its block's centre; influence, own rows' value minus identified enemies', summed over the 3 x 3
   cells around each cell; presence, the enemy rows known only as radar contacts or ghosts. */
typedef struct IMPAI_MAPS {
    int       gw, gh, threat_built;
    uint32_t* threat[IMPAI_THREATS];
    int32_t*  influence;
    uint16_t* presence;
    int32_t*  diff;             /* scratch, (IMPAI_GRID_MAX + 1) x IMPAI_GRID_MAX                          */
} IMPAI_MAPS;
int impai_maps_build(const IMPAI_VIEW* v, IMPAI_MAPS* m);

/* FNV-1a 64 digests: of one seat's column of the rows and its published patches, and of built maps */
uint64_t impai_known_digest(const IMPAI_VIEW* v);
uint64_t impai_maps_digest(const IMPAI_MAPS* m);

#endif
