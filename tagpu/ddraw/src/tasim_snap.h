#ifndef TASIM_SNAP_H
#define TASIM_SNAP_H
/* tasim_snap.c -- the resource buildings' rules over WORLDQ, one copy for the game (tagpu_resource.c, through
   the engine's instance) and the in-game wiki (tagpu_wikisim.c, through the core's): the snap scorer that
   moves a build cursor onto a deposit or a vent, the patch rule that tells a deposit from broad metal ground,
   the queued-plan rules, Quick Build's choice and its commit. research/notes/modules/resource-snapping.md.

   Engine-free: everything it knows of a world comes through the WORLDQ it is given, and every list it reads
   is copied into the caller's SNAP_SCRATCH. Its arithmetic is integer, and the definitions' floats compared
   and never computed with, so the DLL's object and the wiki's native host decide alike by construction (the
   snap distance, tasim_snap.c). */
#include "worldq.h"

#define SNAP_PATCH_R    64                       /* a patch's flood reaches this many cells from its start */
#define SNAP_PATCH_SIDE (2 * SNAP_PATCH_R + 1)
#define SNAP_PATCH_CAP  (SNAP_PATCH_SIDE * SNAP_PATCH_SIDE)
#define SNAP_UNITS_MAX  1500                     /* the units a seat holds, at most: the raised limit */
#define SNAP_RADIUS_MAX WQ_FOOT_MAX              /* a snap search's radius, cells: the profiles' 8 and a
                                                    footprint's side both fit */

#define SNAP_QUEUED 0x1u                         /* a plan of the seat's is queued at the choice */
#define SNAP_CANCEL 0x2u                         /* ... and a selected builder can cancel it      */

/* The caller's scratch, for one call at a time: a UI's static, or the wiki's per sim. */
typedef struct SNAP_SCRATCH {
    uint8_t  seen[SNAP_PATCH_CAP];               /* the patch flood's arena                       */
    uint16_t queue[SNAP_PATCH_CAP];
    WQ_ORDER list[WQ_LIST_MAX];                  /* one unit's orders                             */
    uint16_t opts[WQ_TYPES_MAX];                 /* one builder type's build options              */
    uint8_t  available[WQ_TYPES_MAX];            /* the types the selection's menus offer         */
    uint8_t  yard[2][WQ_FOOT_MAX * WQ_FOOT_MAX]; /* the plan's and a queued build's yards         */
    uint16_t primary[SNAP_UNITS_MAX];            /* the commit's builders, their build actions    */
    uint8_t  action[SNAP_UNITS_MAX];             /*   and the reclaims of an upgrade's target     */
    int16_t  reclaim[SNAP_UNITS_MAX];
} SNAP_SCRATCH;

/* A candidate building: its type and kind, its first cell, the altitude the cursor gives it, the unit it
   replaces (an upgrade; 0 none), whether a plan is queued there (SNAP_*), its deposit's anchor cell once
   the patch rule has found it, and its definition's strength. */
typedef struct SNAP_CHOICE {
    int   type, kind, x, z, height, target, queued, anchor_x, anchor_z;
    float strength;
} SNAP_CHOICE;

/* What a Quick Build decision reads besides the world, the UI's to fill. */
typedef struct SNAP_QUICK_IN {
    int32_t         raw[3];        /* the cursor's 16.16 world point                                     */
    int             radius;        /* the snap search's, in cells                                        */
    int             target;        /* the slot of the live unit under the cursor, of any owner, or -1    */
    int             feature_reclaimable;   /* with no unit there: the feature under it is reclaimable    */
    const uint16_t* builders;      /* the seat's selected live units, slots ascending                    */
    int             nbuilders;
    int             shift;
} SNAP_QUICK_IN;

enum { SNAP_NONE, SNAP_ORDERED, SNAP_ALREADY, SNAP_UNQUEUED };
/* a commit's outcome: SNAP_NONE ordered nothing (the click is the game's own), else what it did, with the
   builders ordered, the helpers set to guard them and the builders whose plan was cancelled */
typedef struct SNAP_COMMIT_RES { int built, helpers, removed, outcome; } SNAP_COMMIT_RES;

/* An explored, closed metal patch: its anchor (its least cell by z, then x), its bounding cells, inclusive,
   and the count of its cells above the threshold. */
typedef struct SNAP_PATCH { int16_t ax, az, x0, z0, x1, z1; uint16_t cells; } SNAP_PATCH;

/* the footprint inside the map's placeable interior with every cell explored */
int  tasim_snap_site_known(const WORLDQ* q, int type, int x, int z);
/* THE SNAP SCORER around `seed`, the raw cursor `raw` breaking ties; `legal` needs `in` (its selection and
   Shift decide which plans block), and `ignore` is the unit an upgrade replaces, 0 none */
int  tasim_snap_site(const WORLDQ* q, const SNAP_QUICK_IN* in, int type, const int32_t seed[3],
                     const int32_t raw[3], int radius, int legal, int ignore, SNAP_CHOICE* out, SNAP_SCRATCH* scr);
/* The build cursor's snap (legal 0, `in` NULL) or Quick Build's: the best site of `type` within `radius`
   cells of `raw`, held for a mex only when it stays best in its own footprint-sized neighbourhood. */
int  tasim_snap_snapped(const WORLDQ* q, const SNAP_QUICK_IN* in, int type, const int32_t raw[3], int radius,
                        int legal, SNAP_CHOICE* out, SNAP_SCRATCH* scr);
/* THE PATCH RULE: 1 when the footprint at the choice's cell starts a closed, explored metal patch, with its
   anchor in `c` */
int  tasim_snap_distinct(const WORLDQ* q, SNAP_CHOICE* c, int fx, int fz, SNAP_SCRATCH* scr);
/* the order position of a choice: its footprint's centre, at its altitude, 16.16 */
void tasim_snap_position(const WORLDQ* q, const SNAP_CHOICE* c, int32_t pos[3]);
/* a placed plan of the seat's whose yard collides with the choice's, the ones `in`'s click replaces apart;
   1 with no `in` */
int  tasim_snap_plans_block(const WORLDQ* q, const SNAP_CHOICE* c, const SNAP_QUICK_IN* in, SNAP_SCRATCH* scr);
/* SNAP_QUEUED when a plan of the seat's is the choice's, with SNAP_CANCEL when a selected builder can
   cancel it; 0 with no `in` */
int  tasim_snap_queued(const WORLDQ* q, const SNAP_CHOICE* c, const SNAP_QUICK_IN* in, SNAP_SCRATCH* scr);
/* Quick Build's one answer, the ghost's, the cursor's and the click's: 1 with the choice in `out`. */
int  tasim_snap_quick(const WORLDQ* q, const SNAP_QUICK_IN* in, SNAP_CHOICE* out, SNAP_SCRATCH* scr);
/* Quick Build's click on `c` (tasim_snap_quick's answer for the same input): a queued plan cancelled with
   Shift or left as it is, else the builders that can build it ordered and the other selected helpers set to
   guard them. Its outcome, as `res->outcome`. */
int  tasim_snap_commit(const WORLDQ* q, const SNAP_QUICK_IN* in, const SNAP_CHOICE* c, SNAP_COMMIT_RES* res,
                       SNAP_SCRATCH* scr);
/* The patches whose anchor lies in rows [z0, z1), in scan order, into `out` (at most `cap`): the count. A
   flood is started only with SNAP_PATCH_CAP cells of `*flood_budget` left, and costs the cells it reaches;
   when that is not left, or `cap` patches are out, the row it stops in is given back whole -- its patches
   are not counted -- and `*next_z` is that row, where the next call resumes; else `*next_z` is z1. So one call reads at most
   (z1 - z0) x W cells besides `*flood_budget`, and a row is never reported twice. A budget below what one
   row's floods take makes no progress: the caller sizes it. */
int  tasim_snap_patches(const WORLDQ* q, int z0, int z1, SNAP_PATCH* out, int cap, uint32_t* flood_budget,
                        int* next_z, SNAP_SCRATCH* scr);

#endif
