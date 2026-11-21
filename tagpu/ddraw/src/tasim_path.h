#ifndef TASIM_PATH_H
#define TASIM_PATH_H
/* tasim_path.h -- the engine's pathfinder as the shared simulation core holds it: the per-tick scheduler that shares a
   budget among the players and walks their unit blocks, the search start, the bug walk, the A* with its lazy-top heap,
   the extraction to corners and the class's set path; and the three ground goals' search methods. research/notes/
   engine/units.md (*The pathfinder*) holds the engine facts each function cites; research/notes/impai/shared-core.md
   names the core's modules.

   ENGINE-FREE AND FILE-FREE, ONE CALLER-OWNED BLOCK, NO ALLOCATION, as tasim_map.h. ts_pf_bytes sizes the
   pathfinder's grid, node pool and heap for a map, ts_pf_init lays them out in the caller's memory. The engine grows
   its pool; the core is sized to the bound the engine cannot exceed (ts_pf_bytes).

   THE WORLD it reads and writes is the caller's, handed over per call as a TS_PFWORLD: the class maps (tasim_class.h)
   and through them each slot's cell, footprint, mover and claim stamp; the slots' type, move class, heading and path
   state; the goals; the explored map; the players' blocks. The T1 plan's arena (b1) takes these over; until then the
   caller keeps them, as it keeps TS_CUNITS for the class maps.

   EVERY INDEX TAKEN FROM DATA IS BOUNDED: a slot against the units' view, a move class against the 32 records and
   their presence, a goal against the caller's goals, a node against the pool's live part, a heap position against the
   heap's count. Where the engine has no bound, the function names what the bound replaces. */
#include <stddef.h>
#include <stdint.h>
#include "tasim_class.h"

#define TS_PF_SEATS    10       /* the players the scheduler walks, 0..9 (0x40EC22..0x40EC85)                   */
#define TS_PF_WP       20       /* the class's waypoints: set path keeps at most 20 (0x44F0BC..0x44F0C2)        */
#define TS_PF_RING     64       /* the extraction's ring of corners (0x40E0BC, 0x40E0EB)                        */
#define TS_PF_NOCLASS  (-1)     /* a slot without a mover, or a mover without a move class (+0x04 NULL)         */
#define TS_PF_NOGOAL   (-1)

/* the goal kinds, by the id each vtable's slot +0x08 returns (0x44CFE0 4, 0x44D440 5, 0x44D900 6) */
enum { TS_GOAL_NONE = 0, TS_GOAL_POINT = 4, TS_GOAL_ANNULUS = 5, TS_GOAL_RING = 6 };

/* what 0x44CED0 ORs into the goal's order node +0x4E */
#define TS_GOALF_REACHED  0x020u  /* the class step: the unit reached the goal (0x44F1BE)                        */
#define TS_GOALF_NOPATH   0x040u  /* set path with no points while the goal is not reached (0x44F0A4)            */
#define TS_GOALF_SUPER    0x080u  /* set goal replaced the goal (0x44F2C6)                                       */
#define TS_GOALF_WALKIN   0x100u  /* the start is in the goal, or the bug walk reached it (0x40E758, 0x40E7F7)  */
#define TS_GOALF_WALKOUT  0x200u  /* the start is off the grid, or the bug walk did not reach it (0x40E7A5, 0x40E974) */

/* One ground goal: the point (vtable 0x4FD328), the annulus (0x4FD358) or the ring around a rectangle (0x4FD388).
   Each kind uses its own fields; `flags` is the owning order node's +0x4E. */
typedef struct TS_GOAL {
    int32_t  kind;
    int16_t  gx, gz;            /* point and annulus +0x08, +0x0A: the footprint's top-left goal cell           */
    int32_t  r;                 /* point +0x0C, the radius in px                                                */
    int32_t  rc2;               /* point +0x10, (r / 16)^2                                                      */
    int32_t  rin, rout;         /* annulus +0x0C, +0x10, px                                                     */
    int32_t  rin2, rout2;       /* annulus +0x14, +0x18, (rin / 16)^2 and (rout / 16)^2                        */
    int32_t  x0, x1, z0, z1;    /* ring +0x08, +0x0C, +0x10, +0x14, cells                                      */
    uint32_t flags;             /* the node's +0x4E                                                             */
} TS_GOAL;

/* The ground class object's path state (0x65 bytes, vtable 0x4FD458, constructor 0x44F010). */
typedef struct TS_PATH {
    int32_t  goal;              /* +0x04: the goal, an index into the world's goals, TS_PF_NOGOAL for none      */
    int16_t  wp[TS_PF_WP][2];   /* +0x0C..+0x5B: int16 world px (x, z)                                          */
    int32_t  count;             /* +0x5C                                                                        */
    uint32_t last_search;       /* +0x60: GameTime of the last search started, or 0                             */
    uint8_t  pflags;            /* +0x64: bit 0 has a path, bit 1 wants a search, bit 3 changed                  */
} TS_PATH;

/* What the pathfinder reads and writes of one unit slot beside the class maps' TS_CUNIT. */
typedef struct TS_PFSLOT {
    uint16_t type;              /* +0xA6, 0 for an empty slot                                                   */
    int8_t   cls;               /* the mover's move class (+0x04), TS_PF_NOCLASS without one                   */
    uint16_t heading;           /* +0x66                                                                        */
    TS_PATH  path;              /* the mover's class object [mover + 0x00]                                      */
} TS_PFSLOT;

/* One player as the scheduler reads its record main+0x1B63 + 0x14B * p. */
typedef struct TS_PFSEAT {
    uint8_t  credited;          /* the record is in use (+0x00), its type +0x73 is 1, 2 or 3 and +0x146 is not 10
                                   (0x40EC45..0x40EC62): only such a player's credit is topped up               */
    int32_t  first, last;       /* +0x67, +0x6B: its block's first and last unit, as slots                       */
} TS_PFSEAT;

/* A private view of a rectangle of the class's map: `code[(z - z0) * w + (x - x0)]` is the stored code the reader takes
   for an entry inside it, in place of the map's. A search that must not write the shared map reads its own region
   through one (ts_pf_search). */
typedef struct TS_PFPATCH {
    int32_t        x0, z0, w, h;
    const uint8_t* code;
} TS_PFPATCH;

typedef struct TS_PFWORLD {
    TS_CLASSMAPS*   cm;         /* the class maps; cm->units is the slots' view (cell, footprint, mover, claim)  */
    TS_PFSLOT*      slot;       /* cm->units->n slots, indexed alike                                            */
    TS_GOAL*        goal;       /* the goals a path's index names                                               */
    int32_t         ngoal;
    const uint16_t* mapped;     /* the explored map main+0x14273: a u16 per 32-px block, (W >> 1) a row, bit p
                                   player p's; NULL for everything explored (C1's scenarios set mapping on)     */
    TS_PFSEAT       seat[TS_PF_SEATS];
    uint16_t        players;    /* main+0x2A3C, the budget's divisor                                            */
    uint16_t        per_seat;   /* main+0x1434F = S, copied at 0x4854D5 from main+0x37EE6                       */
    uint32_t        now;        /* GameTime main+0x38A47                                                       */
    const TS_PFPATCH* patch;    /* NULL for none                                                                */
    uint8_t         frozen;     /* the class maps are read as they stand: a search's start runs no watermark
                                   0x440AF0 and its end no cleanup 0x440BE0                                     */
} TS_PFWORLD;

/* the grid's cell [pf+0x1C] + 4 * (z * W + x) */
typedef struct TS_PFCELL {
    uint8_t  b0;                /* bits 0-1: 0 new, 1 open, 2 closed, 3 dead; bit 2 a goal cell; bit 3 walked    */
    uint8_t  b1;                /* the direction the cell was reached by                                        */
    uint16_t node;              /* its open node                                                                */
} TS_PFCELL;

/* the pool's 20-byte node [pf+0x00] + 20 * i (scout-Jb J6) */
typedef struct TS_PFNODE {
    int32_t  link;              /* +0x00: its heap position; on the free list the next free node                 */
    int16_t  x, z;              /* +0x04                                                                        */
    int32_t  g, f;              /* +0x08, +0x0C                                                                 */
    int16_t  pen;               /* +0x10: 30 for a bad cell                                                     */
    int16_t  straight;          /* +0x12: steps since the last turn                                             */
} TS_PFNODE;

/* A heap entry: the node, and a copy of its f. The engine's entry is the node's pointer and every compare reads f
   through it; the copy keeps a sift inside the heap's own memory. It equals the node's f wherever a compare reads
   it: each store of a node's f stores its entry's too -- the start node's, a pushed node's, the lazy top's when a
   new cell overwrites it, a relaxed node's. */
typedef struct TS_PFHEAP {
    int32_t  f;
    uint32_t n;
} TS_PFHEAP;

/* The pathfinder object [main+0x14207] and its three .data globals. */
typedef struct TS_PF {
    TS_PFNODE* pool;            /* +0x00                                                                        */
    TS_PFHEAP* heap;            /* +0x04                                                                        */
    int32_t    freehead;        /* +0x08, -1 for none                                                           */
    int32_t    used;            /* +0x0C: nodes taken from the pool's end                                       */
    int32_t    nodes;           /* the pool's and the heap's length, W * H (the engine's +0x10 grows)           */
    int32_t    count;           /* +0x14: the heap's                                                            */
    int32_t    lazy;            /* +0x18: the popped node still stands at heap[0]                               */
    TS_PFCELL* grid;            /* +0x1C                                                                        */
    int32_t    w, h;            /* +0x20, +0x24: the feature grid's W and H                                      */
    int32_t    cells;           /* +0x28: (W * H + 7) & ~7                                                      */
    uint32_t*  touch;           /* +0x2C: a bit per 8 cells                                                     */
    int16_t    start_x, start_z;/* +0x30: the search's start cell                                               */
    int16_t    found_x, found_z;/* +0x34: the goal cell the search found                                        */
    int32_t    near_x, near_z;  /* +0x38, +0x3C: the goal cell nearest the start                                */
    int32_t    relax;           /* +0x40: the bug walk's answer                                                 */
    int32_t    fan;             /* +0x44: the directions expanded either side, 4 for the start node, then 2      */
    int32_t    budget;          /* +0x48: 1333 stock (0x40EAD3), 66 650 under Impure's raised limits            */
    int32_t    cost;            /* +0x4C                                                                        */
    int32_t    wgt;             /* +0x50: the searching player's weight                                         */
    int32_t    base_w;          /* +0x54: 0x18000                                                               */
    int32_t    searching;       /* +0x58: the searching slot, 0 for none                                        */
    int32_t    cls_obj;         /* +0x5C: the slot whose class object the examination returned, 0 for none      */
    int32_t    goal;            /* +0x60                                                                        */
    int32_t    cls;             /* +0x64: the search's move class, TS_PF_NOCLASS for none                       */
    uint8_t    turnpen[8];      /* +0x68: {0,40,60,80,100,80,60,40}, from 0x4FCA10                             */
    uint8_t    step[8];         /* +0x70: {16,22,16,22,16,22,16,22}                                             */
    uint8_t    rr;              /* +0x78: the player being served                                               */
    int32_t    cursor[TS_PF_SEATS]; /* +0x79 + 4p: the slot each player's turn examined last                     */
    int32_t    credit[TS_PF_SEATS]; /* +0xA1 + 4p                                                               */
    int32_t    weight[TS_PF_SEATS]; /* 0x5119E8: each player's heuristic weight, 16.16                          */
    int32_t    exams[TS_PF_SEATS];  /* 0x511A10: the slots each player's turn examined since the weights' rewrite */
    int32_t    calls;           /* 0x511A38: the scheduler's calls, mod 150                                     */
    /* the core's: three bitsets over the slots that let the scheduler skip examinations (ts_pf_tick) */
    int32_t    nslot;
    uint32_t*  mob;             /* an occupied slot with a mover and a move class: its examination writes cls_obj */
    uint32_t*  cand;            /* mob and pflags bit 1                                                         */
    uint32_t*  starter;         /* cand and past the 60-tick gate, this tick                                    */
    int32_t    overflow;        /* a search asked for a node past a pool smaller than a node a cell (ts_pf_init_n) */
} TS_PF;

/* The bytes ts_pf_init needs for a w x h grid and nslot unit slots; 0 when it would refuse them. */
size_t ts_pf_bytes(int32_t w, int32_t h, int32_t nslot);

/* The per-game init 0x40E9E0 (DISASSEMBLED 0x40E9E0..0x40EB25), in `mem`: every grid cell new, the pool and heap
   empty, the budget, the base weight 0x18000, every weight that base, every credit 0, each cursor its player's first
   slot, rr 0. It leaves `exams` and `calls`, which the engine's init leaves too: a new TS_PF is zeroed by its caller
   (a fresh process's .data) or takes them from a recording. Returns 0, or -1 for a size ts_pf_bytes refuses. */
int ts_pf_init(TS_PF* pf, void* mem, size_t bytes, int32_t w, int32_t h, int32_t nslot, int32_t budget,
               const TS_PFSEAT seat[TS_PF_SEATS]);

/* The same with a pool and a heap of `nodes` nodes, 1 .. w * h: a search that asks for one more sets `overflow` and
   opens nothing further. ts_pf_bytes and ts_pf_init are these at w * h, the bound no search exceeds. */
size_t ts_pf_bytes_n(int32_t w, int32_t h, int32_t nslot, int32_t nodes);
int ts_pf_init_n(TS_PF* pf, void* mem, size_t bytes, int32_t w, int32_t h, int32_t nslot, int32_t nodes, int32_t budget,
                 const TS_PFSEAT seat[TS_PF_SEATS]);

/* The scheduler 0x40EB70, called once a tick by the players phase (0x464F92), with the skip-ahead: exactly the
   engine's effects, in O(players + searches + nodes) a tick rather than O(budget). ts_pf_tick_literal is the engine's
   loop examining one slot at a time, the reference tests/tasim_pf_test.c holds the skip-ahead to. Both refuse a
   world whose class maps are not the pathfinder's size. */
void ts_pf_tick(TS_PF* pf, TS_PFWORLD* W);
void ts_pf_tick_literal(TS_PF* pf, TS_PFWORLD* W);

/* THE CONTRACT the skip-ahead rests on: a caller that changes a slot's type, move class, mover or pflags bit 1 calls
   ts_pf_mark on it before the next tick (the class step, set goal, a create, a death); ts_pf_mark_all after a load or
   a restore. The pathfinder marks the slots it changes itself (set path, a search's start). */
void ts_pf_mark(TS_PF* pf, const TS_PFWORLD* W, int32_t slot);
void ts_pf_mark_all(TS_PF* pf, const TS_PFWORLD* W);

/* 0x40E9C0(obj), thiscall ret 4, DISASSEMBLED 0x40E9C0..0x40E9D9: when the class object of `slot` is the one the last
   examination returned, the search and its object, goal and class are dropped -- with no cleanup 0x440BE0. Set goal
   (0x44F2B5), the class's destructor (0x44F465) and 0x44F055 call it. */
void ts_pf_abandon(TS_PF* pf, int32_t slot);

/* Set path 0x44F080(pts, n) on the class object of `slot`, n points 0..TS_PF_RING. */
void ts_pf_set_path(TS_PF* pf, TS_PFWORLD* W, int32_t slot, const int16_t (*pts)[2], int32_t n);

/* The pathing map's reader 0x40D7B0(x, z) for the search's class and the player being served. */
int ts_pf_reader(const TS_PF* pf, const TS_PFWORLD* W, int32_t x, int32_t z);

/* ONE SEARCH, WHOLE, ON A FROZEN MAP: the search start 0x40E630 after its watermark, the A* passes to their end and
   the extraction, for one unit, with nothing written but `pf`'s scratch and the result. The caller hands the class's
   map as the search must see it -- the searcher's own region through `patch` where the shared map holds the searcher
   as an obstacle -- and applies the result itself: set path's rule (0x44F080) with `n` points, or none, and
   `goalflags` ORed into the goal's order node +0x4E. `pf` is any TS_PF of the map's size; a search leaves nothing in
   it the next one reads. */
enum {
    TS_PF_FOUND = 1,            /* n points, the corners from the start on, at most TS_PF_WP                    */
    TS_PF_NOSEARCH,             /* the start is in the goal, or the bug walk got no nearer than the start      */
    TS_PF_OFFGRID,              /* the start is off the grid                                                    */
    TS_PF_NOPATH,               /* the open set emptied                                                         */
    TS_PF_OVERFLOW,             /* the pool is too small (ts_pf_init_n): no result, run it again on a larger one */
    TS_PF_REFUSED               /* no map, no goal, or a map that is not the pathfinder's size                  */
};

typedef struct TS_PFQUERY {
    const uint32_t*   map;      /* the class's map [class + 0x18], w x h entries                                */
    int32_t           w, h;
    int16_t           fx, fz;   /* the class's footprint                                                        */
    const TS_PFPATCH* patch;    /* NULL for none                                                                */
    const uint16_t*   mapped;   /* the explored map, NULL for everything explored                               */
    uint8_t           seat;     /* the player whose explored bit the reader takes                               */
    int32_t           wgt;      /* the heuristic's weight, 16.16                                                */
    int16_t           sx, sz;   /* the searcher's cell, unit +0x76, +0x78                                       */
    uint16_t          heading;  /* unit +0x66                                                                   */
    TS_GOAL           goal;
} TS_PFQUERY;

typedef struct TS_PFRESULT {
    int32_t  kind;
    uint32_t goalflags;         /* TS_GOALF_WALKIN, _WALKOUT, and _NOPATH where set path with no points sets it  */
    int32_t  n;
    int16_t  pts[TS_PF_WP][2];
    int32_t  pops;              /* the A* pops, the engine's cost of the search past its start                  */
    int32_t  used;              /* the pool's high-water mark, [pf + 0x0C] at the search's end; -1 with no A*   */
    int32_t  relax;             /* [pf + 0x40], the bug walk's answer; 0 when it did not run                    */
    int16_t  found_x, found_z;  /* [pf + 0x34], the goal cell found                                             */
} TS_PFRESULT;

int ts_pf_search(TS_PF* pf, const TS_PFQUERY* q, TS_PFRESULT* r);

/* The goals' constructors, each from its order node's unit footprint (fw, fh) = +0x7E, +0x80: the point 0x44CF60(node,
   x, z, r) and the annulus 0x44D3B0(node, x, z, rout, rin), x and z 16.16; the ring 0x44D8A0(node, cell, size), a
   rectangle's top-left cell and size. */
void ts_goal_point(TS_GOAL* g, int32_t x16, int32_t z16, int32_t r, int16_t fw, int16_t fh);
void ts_goal_annulus(TS_GOAL* g, int32_t x16, int32_t z16, int32_t rout, int32_t rin, int16_t fw, int16_t fh);
void ts_goal_ring(TS_GOAL* g, int16_t cx, int16_t cz, int16_t w, int16_t h, int16_t fw, int16_t fh);

/* The goals' search methods: in_goal (vtable +0x14) of a cell, the heuristic (+0x1C) of a cell, and the goal cells
   (+0x18) one by one, i from 0 while it returns 1. A goal's reached (+0x10) is in_goal of the unit's cell for all
   three kinds. point() (+0x20) is set goal's, not the search's. */
int     ts_goal_in(const TS_GOAL* g, int32_t x, int32_t z);
int32_t ts_goal_h(const TS_GOAL* g, int32_t x, int32_t z);
int     ts_goal_cell(const TS_GOAL* g, int64_t i, int16_t* x, int16_t* z);

#endif
