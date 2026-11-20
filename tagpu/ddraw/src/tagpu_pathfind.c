#include "tagpu_simreg.h"
/* tagpu_pathfind.c -- the ground units' pathfinder: our scheduler and our search in the engine's place, and the
   oracle that holds the search to the engine's (tagpu_pathfind.h; research/notes/pathfinder-parallel.md; the engine
   facts: research/notes/engine/units.md, The pathfinder, and engine/map.md, The pathing maps).

   THE SEARCH is the shared core's (tasim_path.h, ts_pf_search): one unit's search start, A* and extraction, whole,
   on a map it does not write. What the engine's search start 0x40E630 does to the shared map before it reads it is
   here THE PATCH. A unit that has stood still for 30 ticks is an obstacle in its class's map: its claim stamp is
   below the class's watermark. When the searcher is one, the engine's watermark 0x440AF0 recomputes its rectangle
   with the searcher left out (0x440B33..0x440B45), so that it does not stand in its own way, and the cleanup
   0x440BE0 puts it back. The patch is those entries, recomputed with the engine's rule 0x47E1F0 over the engine's
   own cells with that one unit left out, and read by the search in place of the map's; nothing shared is written.
   A searcher that is no obstacle has no patch: the map already holds its rectangle without it, and the engine reads
   it as it stands, an entry no re-mark has refreshed included (a unit made inside a plant's yard leaves by one).

   THE SCHEDULER (sched_tick) stands at the players phase's call of the engine's 0x40EB70 (0x464F92), which then
   never runs. Once a tick, on the game thread:
     1. THE WALK. Every unit of every player the engine's scheduler serves, in the players' and their blocks' order,
        whose ground course wants a search (bit 1 of its +0x64) is READY when
          - its stamp +0x60 is 0: a new goal. Set goal 0x44F2A0 zeroes the stamp when the last search began more
            than 10 ticks ago, and before_set_goal inside those 10 ticks when that search found a path;
          - or it is not blocked and has fewer than two corners left (0x44F24B) of a path its last search found:
            it has followed its path to the end;
          - or its last search began PF_RETRY ticks ago or more: a blocked unit, or one whose search found nothing.
        The first two kinds are served before the third, each kind from its own cursor on, so that the cap never
        starves a unit and a jam never delays a new goal.
     2. THE BATCHES, PF_BATCH searches each, while fewer than PF_CAP_SEARCHES have begun this tick and their pops
        are below PF_CAP_POPS. Both bounds count work, never time, and the batch is a constant: the searches a tick
        begins are the same on every machine, however many threads run a batch. A batch is three steps, each
        over all its searches before the next: THE WRITES (the unit's stamp; its class's watermark and the marking
        of the units that have just become obstacles, once a class a tick, as 0x440AF0's sweep; an obstacle
        searcher's rectangle recomputed in the map through the engine's 0x440830, as the cleanup would); THE
        SEARCHES, which read the map and the cells and write only their own patch, scratch and result; THE
        RESULTS, each applied through the engine's own set path 0x44F080 and its flag call 0x44CED0.
   No search spans ticks, so nothing of a search is state: what a tick carries to the next is the units' own
   stamps and paths, the two cursors and, per unit slot, the stamp of its last search that found a path.
   THE HEURISTIC'S WEIGHT is one of the engine's three: its base [pf + 0x54], 3 times it, 6 times it. The engine
   gives them to a player by how slowly its budget serves the player's units (0x40EBBF..0x40EC06); here they
   follow the tick's load: the base in a quiet tick, one with at most PF_QUIET units ready; 3 times while every
   ready unit can begin this tick; 6 times once more are ready than PF_CAP_SEARCHES. A search at 3 times pops
   under a fifth of what it pops at the base and at 6 times a fifth less again, for a path about as long that
   keeps less clear of what stands in its way (MEASURED, modules/pathfinder.md): a unit ordered in a quiet moment
   gets the best path and a tick that cannot serve everyone the cheapest, as the engine's budget gives them.
   A GOAL OF A KIND THE SEARCH HAS NOT, a search past the scratch's pool, and a unit whose class, grid or patch is
   outside its bound get the result of a search that found nothing, through the same set path, and are counted.
   ON A PUPPET every unit's course is a proxy (replay-layout.tsv, mover +0x000), so the walk finds nothing.
   `tagpu_pathlog.on` logs what the scheduler did, every 150 ticks.

   THE VERIFY LEVER `tagpu_pathverify.on` leaves the engine's scheduler in place and runs our search beside
   its searches: at the engine's grid reset 0x40D900 inside a search start -- after the watermark, so the map holds
   what the engine's search is about to read -- it computes the patch, counts the entries that differ from the
   map's (the engine has just recomputed the same ones: any difference is a fault in our rule), runs the search on
   the scratch and keeps the result; at the search's end, the class's cleanup 0x440BE0, it compares that with what
   the engine wrote: the exit, the path's points and count, the goal cell, the pool's use, the bug walk's answer
   and the flags ORed into the order. It writes no engine memory and decides nothing.

   EVERYTHING HERE RUNS ON THE GAME THREAD inside the players phase, which owns what it reads and writes. A value
   taken from engine memory is data until bounded: a player's block against the unit array, the class record
   against the 32 records, the grid's size against the class's and the pathfinder's, a cell's unit slot against the
   unit array's end, a feature definition against the count, the patch's rectangle against PF_PATCH_MAX, a unit's
   slot against the table of stamps. */
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_engine.h"
#include "tagpu_detour.h"
#include "tagpu_limits.h"
#include "tagpu_log.h"
#include "tagpu_pathfind.h"
#include "tagpu_prof.h"
#include "tasim_path.h"

SIM_FEATURE(path_sched, SIM_PUPPET_RUNS, "battle battle-allied stock-battle-allied formation-basic");

/* the pathfinder object [main + 0x14207] (engine/units.md) */
#define OFF_PATHFINDER   0x14207
#define PF_USED          0x0C       /* nodes taken from the pool's end: written at an A* start only          */
#define PF_W             0x20
#define PF_H             0x24
#define PF_FOUND         0x34       /* the goal cell found, two words                                        */
#define PF_RELAX         0x40       /* the bug walk's answer                                                 */
#define PF_WGT           0x50
#define PF_UNIT          0x58       /* the searching unit, NULL for none                                     */
#define PF_GOAL          0x60
#define PF_CLASS         0x64       /* the search's class record                                             */
#define PF_SEAT          0x78       /* the player being served, a byte                                       */
/* the movement classes, 32 records of 0x20 bytes (engine/map.md) */
#define VA_CLASSES       0x00512358u
#define CLASS_STRIDE     0x20u
#define CLASS_COUNT      32u
/* the explored map: a u16 per 32-px block, (W >> 1) a row, bit p player p's */
#define OFF_EXPLORED     0x14273
/* a unit */
#define UNIT_HEADING     0x66
#define UNIT_CELL        0x76       /* its footprint's top-left cell, two words                              */
#define UNIT_FOOT        0x7E       /* its footprint's size, two words                                       */
#define UNIT_TYPE        0xA6       /* a word, 0 for a free slot                                             */
#define MOVER_CLASS      0x04
#define MOVER_STAMP      0x26       /* the claim stamp, a GameTime                                           */
#define MOVER_FLAGS      0x2E       /* bit 2: blocked (0x44F245)                                             */
/* the local ground mover's class object [mover + 0x00] (vtable 0x4FD458) */
#define VT_GROUND_COURSE 0x004FD458u
#define OBJ_WP           0x0C
#define OBJ_COUNT        0x5C
#define OBJ_LAST         0x60       /* GameTime of its last search's start                                   */
#define OBJ_FLAGS        0x64       /* bit 0 has a path, bit 1 wants a search                                */
/* the ground goals, by vtable; the order node a goal belongs to is [goal + 4], its flags +0x4E */
#define VT_GOAL_POINT    0x004FD328u
#define VT_GOAL_ANNULUS  0x004FD358u
#define VT_GOAL_RING     0x004FD388u
#define NODE_FLAGS       0x4E
/* the sites, each byte-matched at install */
#define VA_SCHEDULER     0x0040EB70u
#define VA_GRID_RESET    0x0040D900u
#define RA_RESET_SEARCH  0x0040E690u    /* the reset's return into the search start 0x40E630             */
#define VA_EXTRACT       0x0040E050u
#define VA_CLEANUP       0x00440BE0u
#define RA_END_OFFGRID   0x0040E7C0u    /* the cleanup's return addresses, one an exit of a search        */
#define RA_END_NOSEARCH  0x0040E998u
#define RA_END_SCHEDULER 0x0040EEDFu

#define PF_PATCH_MAX     4096           /* a patch's entries: (fx + w + 1) (fz + h + 1) of any real unit  */
#define PF_POOL          65536          /* the scratch pool: the engine's node word is 16 bits            */

/* a player's record main + 0x1B63 + 0x14B p, as the engine's scheduler reads it (0x40EC22..0x40EC85) */
#define OFF_PLAYERS      0x1B63
#define PLAYER_STRIDE    0x14B
#define PLAYER_TYPE      0x73
#define PLAYER_FIRST     0x67       /* its block's first and last unit                                      */
#define PLAYER_LAST      0x6B
#define PLAYER_STATE     0x146      /* 10: not served                                                        */
#define OFF_PLAYER_COUNT 0x2A3C     /* a word: the scheduler does nothing while it is 0 (0x40EB8B)           */
#define PF_BASE_WEIGHT   0x54
#define UNIT_STATE       0x110      /* bit 28: alive                                                         */
/* the engine functions the scheduler calls, each thiscall */
#define VA_REMARK        0x00440830u    /* (class; cell x | z << 16, size w | h << 16), ret 8                */
#define VA_SET_PATH      0x0044F080u    /* (course; points, n), ret 8                                      */
#define VA_GOAL_FLAG     0x0044CED0u    /* (goal; flags), ret 4                                            */
#define VA_SET_GOAL      0x0044F2A0u    /* the ground course's set goal                                    */
#define VA_SCHED_CALL    0x00464F92u    /* the players phase's call of the scheduler                       */

/* THE RULES' NUMBERS. A tick begins at most PF_CAP_SEARCHES searches, and no batch once their pops have reached
   PF_CAP_POPS: the first bounds a tick that many units ask at once, the second a tick whose searches are long (one
   that finds no way opens every cell it can reach, up to the pool's 65 536). PF_QUIET is the most units ready
   in a tick that searches at the base weight (MEASURED: 2900 pops a search at the base, 510 at 3 times, 410 at 6
   times). */
#define PF_BATCH         8
#define PF_CAP_SEARCHES  64
#define PF_CAP_POPS      64000
#define PF_QUIET         4
#define PF_RETRY         60u            /* a blocked unit, or one whose search found nothing: the engine's gate
                                           (0x44F271)                                                           */

enum { MODE_ENGINE, MODE_OURS, MODE_VERIFY };
static int s_mode;
SIM_VAR(s_mode, SIM_INSTALL);
static int s_log;
SIM_VAR(s_log, SIM_INSTALL);

/* THE SCRATCH of a search: written by each search before it reads it, and left with every cell new (ts_pf_search).
   The block is ours alone to free; the TS_PF over it is rebuilt whenever it does not describe that block. */
static TS_PF s_pf;
SIM_VAR(s_pf, SIM_TRANSIENT);
static unsigned char* s_mem;
static unsigned s_mem_bytes;
SIM_VAR(s_mem_bytes, SIM_TRANSIENT);
SIM_HEAP(s_mem, SIM_TRANSIENT, SIM_ID_NONE, 1, s_mem_bytes, NULL);

/* WHAT A TICK CARRIES TO THE NEXT, beside the units' own records: per unit slot the stamp of its last search that
   found a path, 0 for none -- it is that search's while it equals the course's stamp +0x60, so a slot's next unit
   never inherits it -- and each kind's cursor, the slot served last, 0 to start from the first. */
#define PF_SLOTS (10 * TAGPU_LIM_UNITS + 1)
static unsigned s_found[PF_SLOTS];
SIM_ROWS(s_found, SIM_STATE, SIM_ID_UNIT_SLOT, NULL);
static unsigned s_cursor[2];
SIM_VAR(s_cursor, SIM_STATE);

/* diagnostics: the oracle's state and both modes' counters, nothing the simulation reads */
static struct {
    /* the tick each unit slot was first seen wanting a search with the gate open, 0 for none: the wait a search's
       start ends */
    unsigned ready_since[0x10000];
    /* the search in hand, between the reset and the cleanup */
    int open, found;
    const char* unit;
    const char* obj;
    const char* node;
    unsigned gametime, node_flags0;
    TS_PFRESULT res;
    unsigned char patch[PF_PATCH_MAX];
    /* since the last summary line */
    unsigned calls, searches, equal, differ, patched, spanned, spanned_differ, overflow, refused, other_goal;
    unsigned patch_entries, patch_differ, patch_refused, logged;
    /* the ticks a unit waited between the gate opening and its search's start: 0, 1-2, 3-9, 10-29, 30-89, 90-299,
       300 and more */
    unsigned waited, wait_sum, wait_max, wait_hist[7];
    /* the gate's census, a tick: units that want a search, and those the 60-tick gate holds, by why */
    unsigned census_ticks, want, held, held_blocked, held_short, held_order, held_max, ready_max;
    /* the scheduler's, since its last line */
    unsigned ticks, begun, nfound, nosearch, nopath, offgrid, failed, pops, pops_max, begun_max;
    unsigned ready_a, ready_b, ready_order, ready_end, left, left_max, held_retry, weight[3];
    unsigned goals, goals_kept, goals_opened;
} s_v;
SIM_VAR(s_v, SIM_PRIVATE);

static const char* ta_main(void) { return *(const char* const*)(size_t)TA_MAIN_PP; }

/* ---- the engine's cells, read as the class rule reads them -------------------------------------------------- */

typedef struct PV {
    const unsigned char* grid;      /* the cells, FT_STRIDE bytes each, w x h                            */
    const unsigned char* fdefs;
    int nfd, w, h, sea;
    const char* units;              /* the unit array, and its last slot                                 */
    unsigned last;
    const char* absent;             /* the unit the rule leaves out                                      */
    unsigned wm;                    /* the class's watermark                                             */
    TS_CLASSMAP c;
} PV;

/* the feature rule the engine's five readers share (engine/map.md, The cell; tagpu_worldq.c's
   content_feature_blocks is the same rule for the seats' view) */
static int pv_feature_blocks(const PV* v, int idx)
{
    const unsigned char* cl = v->grid + (size_t)idx * FT_STRIDE;
    unsigned word = *(const unsigned short*)(cl + FT_DEFIDX);
    if (word == 0xFFFFu) return 0;
    if (word == 0xFFFEu) {
        const int a = idx - (cl[FT_WIDX] * v->w + cl[FT_WIDX + 1]);
        if (a < 0) return 1;
        word = *(const unsigned short*)(v->grid + (size_t)a * FT_STRIDE + FT_DEFIDX);
        if (word >= 0xFFFBu) return 0;
    } else if (word >= 0xFFFBu)
        return 1;
    if ((int)word >= v->nfd || !v->fdefs) return 1;
    return v->fdefs[(size_t)word * FD_STRIDE + FD_MASK] >> 6 & 1;
}

/* the unit rule 0x47E10C..0x47E13F: the unit in slot A blocks with no mover, or with a stamp below the watermark.
   THE BOUND: the slot against the array's last, where the engine indexes unchecked; a slot past it blocks. The
   absent unit is the searcher, whose stamp the engine holds at GameTime while its own rectangle is recomputed
   (0x440B30), so that it never blocks there. */
static int pv_unit_blocks(const PV* v, const unsigned char* cl)
{
    const unsigned slot = *(const unsigned short*)cl;
    const char* unit;
    const char* mover;
    if (!slot) return 0;
    if (slot > v->last) return 1;
    unit = v->units + (size_t)slot * UNIT_STRIDE;
    if (unit == v->absent) return 0;
    mover = *(const char* const*)unit;
    return !mover || *(const unsigned*)(mover + MOVER_STAMP) < v->wm;
}

/* 0x47DFC0(class, x, z, w, h), as tasim_class.c's ts_class_rect over the engine's cells */
static int pv_rect(const PV* v, int x, int z, int w, int h)
{
    const int lo = v->sea - v->c.maxwaterdepth, hi = v->sea - v->c.minwaterdepth;
    int r = 3, row, col;
    if (x < 0 || z < 0 || x + w >= v->w || z + h >= v->h) return 0;
    for (row = 0; row < h; row++)
        for (col = 0; col < w; col++) {
            const int idx = (z + row) * v->w + x + col;
            const unsigned char* cl = v->grid + (size_t)idx * FT_STRIDE;
            const int mn = cl[6], mx = cl[5];
            unsigned char d;
            if (pv_feature_blocks(v, idx) || pv_unit_blocks(v, cl)) return 0;
            if (mn < lo || mx > hi) return 0;
            d = (unsigned char)(mx - mn);
            if (mn < v->sea) {
                if (d <= v->c.badwaterslope) continue;
                if (d > v->c.maxwaterslope) return 0;
            } else {
                if (d <= v->c.badslope) continue;
                if (d > v->c.maxslope) return 0;
            }
            if (r > 1) r = 1;
        }
    return r;
}

/* 0x47E1F0(class, x, z), as ts_class_refresh: the footprint, then its ring in pinwheel order */
static int pv_refresh(const PV* v, int x, int z)
{
    const int fw = v->c.fx, fh = v->c.fz;
    const int r = pv_rect(v, x, z, fw, fh);
    if (r <= 1) return r;
    if (pv_rect(v, x - 1, z - 1, fw + 1, 1) != 3) return 1;
    if (pv_rect(v, x + fw, z - 1, 1, fh + 1) != 3) return 1;
    if (pv_rect(v, x, z + fh, fw + 1, 1) != 3) return 1;
    return pv_rect(v, x - 1, z, 1, fh + 1) == 3 ? 3 : 1;
}

/* The view of class record `cls` with `absent` left out; 0 when a value it would rest on is out of its bound. */
static int pv_open(PV* v, const char* ta, const char* cls, const char* absent)
{
    const char* first;
    const char* last;
    memset(v, 0, sizeof *v);
    v->grid = *(const unsigned char* const*)(ta + OFF_FEATMAP);
    v->w = *(const int*)(ta + OFF_MAP_W16);
    v->h = *(const int*)(ta + OFF_MAP_H16);
    v->fdefs = *(const unsigned char* const*)(ta + OFF_FEATDEF);
    v->nfd = *(const int*)(ta + OFF_FEATCOUNT);
    if (v->nfd < 0 || v->nfd > 0xFFFA) v->nfd = 0;
    v->sea = *(const unsigned char*)(ta + OFF_SEALEVEL);
    first = *(const char* const*)(ta + OFF_UNIT_BEGIN);
    last = *(const char* const*)(ta + OFF_UNIT_END);
    if (!v->grid || !first || last < first || v->w < 1 || v->h < 1 || (long long)v->w * v->h > 0x1000000) return 0;
    v->units = first;
    v->last = (unsigned)((size_t)(last - first) / UNIT_STRIDE);
    v->absent = absent;
    v->wm = *(const unsigned*)(cls + 0x1C);
    v->c.present = 1;
    v->c.fx = *(const short*)(cls + 0x04);
    v->c.fz = *(const short*)(cls + 0x06);
    v->c.maxwaterdepth = *(const short*)(cls + 0x08);
    v->c.minwaterdepth = *(const short*)(cls + 0x0A);
    v->c.maxslope = *(const unsigned char*)(cls + 0x0C);
    v->c.badslope = *(const unsigned char*)(cls + 0x0D);
    v->c.maxwaterslope = *(const unsigned char*)(cls + 0x0E);
    v->c.badwaterslope = *(const unsigned char*)(cls + 0x0F);
    return v->c.fx >= 1 && v->c.fz >= 1 && v->c.fx <= 64 && v->c.fz <= 64;
}

/* THE PATCH of a unit: the entries 0x440830 recomputes for its rectangle -- [x - fx, x + w] x [z - fz, z + h],
   the near ends held at 0 and the far ends at the grid's (0x440830..0x44091F) -- each through the rule with the unit
   left out. 0 when the rectangle is empty or past PF_PATCH_MAX entries. */
static int patch_of(const PV* v, const char* unit, unsigned char* code, TS_PFPATCH* p)
{
    const int ux = *(const short*)(unit + UNIT_CELL), uz = *(const short*)(unit + UNIT_CELL + 2);
    const int uw = *(const short*)(unit + UNIT_FOOT), uh = *(const short*)(unit + UNIT_FOOT + 2);
    int x0 = ux - v->c.fx, z0 = uz - v->c.fz, x1 = uw + ux + 1, z1 = uh + uz + 1, x, z;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if ((unsigned)x1 > (unsigned)v->w) x1 = v->w;
    if ((unsigned)z1 > (unsigned)v->h) z1 = v->h;
    if (x0 >= x1 || z0 >= z1 || (long long)(x1 - x0) * (z1 - z0) > PF_PATCH_MAX) return 0;
    p->x0 = x0;
    p->z0 = z0;
    p->w = x1 - x0;
    p->h = z1 - z0;
    p->code = code;
    for (z = z0; z < z1; z++)
        for (x = x0; x < x1; x++) code[(z - z0) * p->w + (x - x0)] = (unsigned char)pv_refresh(v, x, z);
    return 1;
}

/* The scratch for a w x h grid, made again when the grid's size changes (a new map). */
static int scratch_for(int w, int h)
{
    static const TS_PFSEAT none[TS_PF_SEATS];
    const int nodes = (long long)w * h < PF_POOL ? w * h : PF_POOL;
    size_t need;
    if (s_mem && s_pf.w == w && s_pf.h == h) return 1;
    need = ts_pf_bytes_n(w, h, 2, nodes);
    if (s_mem) SIM_FREE(s_mem, s_mem);
    s_mem = NULL;
    s_mem_bytes = 0;
    memset(&s_pf, 0, sizeof s_pf);
    if (!need || need > 0x7FFFFFFFu) return 0;
    s_mem = (unsigned char*)SIM_ALLOC(s_mem, need);
    if (!s_mem) return 0;
    s_mem_bytes = (unsigned)need;
    if (ts_pf_init_n(&s_pf, s_mem, need, w, h, 2, nodes, 0, none)) {
        SIM_FREE(s_mem, s_mem);
        s_mem = NULL;
        s_mem_bytes = 0;
        return 0;
    }
    return 1;
}

static int goal_of(const char* goal, TS_GOAL* g)
{
    const unsigned vt = *(const unsigned*)goal;
    memset(g, 0, sizeof *g);
    if (vt == VT_GOAL_POINT) {
        g->kind = TS_GOAL_POINT;
        g->gx = *(const short*)(goal + 0x08);
        g->gz = *(const short*)(goal + 0x0A);
        g->r = *(const int*)(goal + 0x0C);
        g->rc2 = *(const int*)(goal + 0x10);
    } else if (vt == VT_GOAL_ANNULUS) {
        g->kind = TS_GOAL_ANNULUS;
        g->gx = *(const short*)(goal + 0x08);
        g->gz = *(const short*)(goal + 0x0A);
        g->rin = *(const int*)(goal + 0x0C);
        g->rout = *(const int*)(goal + 0x10);
        g->rin2 = *(const int*)(goal + 0x14);
        g->rout2 = *(const int*)(goal + 0x18);
    } else if (vt == VT_GOAL_RING) {
        g->kind = TS_GOAL_RING;
        g->x0 = *(const int*)(goal + 0x08);
        g->x1 = *(const int*)(goal + 0x0C);
        g->z0 = *(const int*)(goal + 0x10);
        g->z1 = *(const int*)(goal + 0x14);
    } else
        return 0;
    return 1;
}

/* ---- the scheduler ------------------------------------------------------------------------------------------ */

typedef void (__thiscall *PFN_Remark)(void* cls, unsigned cell, unsigned size);
typedef void (__thiscall *PFN_SetPath)(void* course, const void* points, int n);
typedef void (__thiscall *PFN_GoalFlag)(void* goal, unsigned flags);

typedef struct SCHED {
    char* ta;
    char* pf;
    char* first;                    /* the unit array, and its last slot                                 */
    char* last;
    unsigned now;
    int w, h;                       /* the pathfinder's grid                                             */
    const unsigned short* mapped;
    int wgt;
} SCHED;

/* one search of a batch */
typedef struct JOB {
    char* unit;
    char* mover;
    char* course;
    char* goal;
    char* cls;
    unsigned slot, stamp;
    int whole;                      /* every value the search rests on is inside its bound              */
    unsigned char seat;
    TS_PFPATCH patch;
    TS_PFRESULT r;
    unsigned char code[PF_PATCH_MAX];
} JOB;

static int sched_open(SCHED* e)
{
    memset(e, 0, sizeof *e);
    e->ta = (char*)ta_main();
    if (!e->ta || !*(const unsigned short*)(e->ta + OFF_PLAYER_COUNT)) return 0;
    e->pf = *(char* const*)(e->ta + OFF_PATHFINDER);
    e->first = *(char* const*)(e->ta + OFF_UNIT_BEGIN);
    e->last = *(char* const*)(e->ta + OFF_UNIT_END);
    if (!e->pf || !e->first || e->last < e->first || (size_t)(e->last - e->first) / UNIT_STRIDE >= PF_SLOTS) return 0;
    e->now = *(const unsigned*)(e->ta + OFF_GAMETIME);
    e->w = *(const int*)(e->pf + PF_W);
    e->h = *(const int*)(e->pf + PF_H);
    e->mapped = *(const unsigned short* const*)(e->ta + OFF_EXPLORED);
    e->wgt = *(const int*)(e->pf + PF_BASE_WEIGHT);
    return e->mapped && e->w >= 1 && e->h >= 1;
}

/* Player p's block of the unit array when the engine's scheduler serves the player (0x40EC45..0x40EC62: a record
   in use, of type 1, 2 or 3, not in state 10). THE BOUND: both ends inside the array and on a slot. */
static int served_block(const SCHED* e, unsigned p, char** b0, char** b1)
{
    const char* rec = e->ta + OFF_PLAYERS + (size_t)p * PLAYER_STRIDE;
    const unsigned char type = *(const unsigned char*)(rec + PLAYER_TYPE);
    if (!*(const unsigned*)rec || type < 1 || type > 3 || *(const unsigned char*)(rec + PLAYER_STATE) == 10) return 0;
    *b0 = *(char* const*)(rec + PLAYER_FIRST);
    *b1 = *(char* const*)(rec + PLAYER_LAST);
    return *b0 >= e->first && *b1 >= *b0 && *b1 <= e->last && !((size_t)(*b0 - e->first) % UNIT_STRIDE) &&
           !((size_t)(*b1 - e->first) % UNIT_STRIDE);
}

/* The unit's local ground course when it wants a search, as the engine's examination reads it (0x40ED33..0x40ED4D:
   a type, a mover, the mover's class) and as its wants-search 0x44F260 tests bit 1; no other course class ever
   answers yes (their slot is 0x44EFF0, `xor eax, eax`). */
static char* wanting_course(const char* unit)
{
    const char* mover;
    char* course;
    if (!*(const unsigned short*)(unit + UNIT_TYPE) || !(mover = *(const char* const*)unit) ||
        !*(const char* const*)(mover + MOVER_CLASS) || !(course = *(char* const*)mover) ||
        *(const unsigned*)course != VT_GROUND_COURSE || !(*(const unsigned char*)(course + OBJ_FLAGS) & 2))
        return NULL;
    return course;
}

/* THE WALK. ready[0] takes the units served first -- a new goal (the stamp is 0), and a unit that has followed its
   path to its end -- and ready[1] the others whose last search began PF_RETRY ticks ago. Slots, in the walk's
   order. */
static void sched_walk(const SCHED* e, unsigned short (*ready)[PF_SLOTS], unsigned* n)
{
    unsigned p;
    n[0] = n[1] = 0;
    for (p = 0; p < 10; p++) {
        char* b0;
        char* b1;
        char* u;
        if (!served_block(e, p, &b0, &b1)) continue;
        for (u = b0; u <= b1; u += UNIT_STRIDE) {
            const char* course = wanting_course(u);
            const unsigned slot = (unsigned)((size_t)(u - e->first) / UNIT_STRIDE);
            unsigned stamp, age;
            int kind;
            if (!course) {
                if (s_log) s_v.ready_since[slot] = 0;
                continue;
            }
            stamp = *(const unsigned*)(course + OBJ_LAST);
            age = e->now - stamp;
            if (!stamp) {
                kind = 0;
                s_v.ready_order++;
            } else if (!(*(const unsigned char*)(*(const char* const*)u + MOVER_FLAGS) & 4) &&
                       *(const int*)(course + OBJ_COUNT) < 2 && s_found[slot] == stamp) {
                kind = 0;
                s_v.ready_end++;
            } else if (age >= PF_RETRY)
                kind = 1;
            else {
                if (s_log) s_v.ready_since[slot] = 0;
                s_v.held_retry++;
                continue;
            }
            if (n[kind] >= PF_SLOTS) continue;
            ready[kind][n[kind]++] = (unsigned short)slot;
            if (s_log && !s_v.ready_since[slot]) s_v.ready_since[slot] = e->now ? e->now : 1;
        }
    }
}

/* The class's watermark, and the marking of the units that have just become obstacles: 0x440AF0's own, less its
   searcher (0x440B03..0x440B18, 0x440B4E..0x440BB8). The watermark moves once a tick, so the sweep runs for the
   first search of a class in a tick and for no other. */
static void class_tick(const SCHED* e, char* cls)
{
    const unsigned old = *(const unsigned*)(cls + 0x1C);
    const unsigned wm = (e->now > 30 ? e->now : 30) - 30;
    char* u;
    *(unsigned*)(cls + 0x1C) = wm;
    if (wm == old) return;
    for (u = e->first + UNIT_STRIDE; u <= e->last; u += UNIT_STRIDE) {
        const char* mover;
        unsigned stamp;
        if (!(*(const unsigned*)(u + UNIT_STATE) & 0x10000000u) || !(mover = *(const char* const*)u)) continue;
        stamp = *(const unsigned*)(mover + MOVER_STAMP);
        if (stamp >= old && stamp < wm)
            ((PFN_Remark)(size_t)VA_REMARK)(cls, *(const unsigned*)(u + UNIT_CELL), *(const unsigned*)(u + UNIT_FOOT));
    }
}

/* THE WRITES of one search. The stamp is the engine's wants-search's (0x44F27E), held off 0, which here reads as a
   new goal. THE BOUNDS a search rests on: the class record among the 32, its map of the pathfinder's grid and
   of the cells' grid, a goal. A unit one of them refuses is not searched and gets the result of a search that
   found nothing. */
static void job_writes(const SCHED* e, JOB* j, unsigned slot)
{
    size_t ci;
    memset(j, 0, offsetof(JOB, code));
    j->slot = slot;
    j->unit = e->first + (size_t)slot * UNIT_STRIDE;
    j->mover = *(char* const*)j->unit;
    j->course = *(char* const*)j->mover;
    j->goal = *(char* const*)(j->course + 4);
    j->cls = *(char* const*)(j->mover + MOVER_CLASS);
    j->stamp = e->now ? e->now : 1;
    *(unsigned*)(j->course + OBJ_LAST) = j->stamp;
    for (j->seat = 0; j->seat < 10; j->seat++) {
        char* b0;
        char* b1;
        if (served_block(e, j->seat, &b0, &b1) && j->unit >= b0 && j->unit <= b1) break;
    }
    ci = (size_t)j->cls - VA_CLASSES;
    if (j->seat > 9 || !j->goal || (size_t)j->cls < VA_CLASSES || ci % CLASS_STRIDE || ci / CLASS_STRIDE >= CLASS_COUNT ||
        !*(const unsigned* const*)(j->cls + 0x18) || *(const int*)(j->cls + 0x10) != e->w ||
        *(const int*)(j->cls + 0x14) != e->h || *(const int*)(e->ta + OFF_MAP_W16) != e->w ||
        *(const int*)(e->ta + OFF_MAP_H16) != e->h)
        return;
    j->whole = 1;
    class_tick(e, j->cls);
    /* an obstacle: its rectangle is recomputed in the map with the unit in it, as the cleanup 0x440BE0 leaves it;
       the search reads the patch there */
    if (*(const unsigned*)(j->mover + MOVER_STAMP) < *(const unsigned*)(j->cls + 0x1C))
        ((PFN_Remark)(size_t)VA_REMARK)(j->cls, *(const unsigned*)(j->unit + UNIT_CELL),
                                        *(const unsigned*)(j->unit + UNIT_FOOT));
}

/* THE SEARCH of one job: it reads the class's map, the cells and the units' stamps, and writes the job and the
   scratch. */
static void job_search(const SCHED* e, JOB* j)
{
    TS_PFQUERY q;
    PV v;
    j->r.kind = TS_PF_REFUSED;
    if (!j->whole) return;
    memset(&q, 0, sizeof q);
    if (!goal_of(j->goal, &q.goal)) {
        if (s_v.other_goal++ < 4)
            tagpu_logf("pathsched: a goal of a kind the search has not (vtable %#x): no path",
                       *(const unsigned*)j->goal);
        return;
    }
    if (!pv_open(&v, e->ta, j->cls, j->unit) || !scratch_for(e->w, e->h)) return;
    if (*(const unsigned*)(j->mover + MOVER_STAMP) < v.wm) {
        if (!patch_of(&v, j->unit, j->code, &j->patch)) return;
        q.patch = &j->patch;
        s_v.patched++;
    }
    q.map = *(const unsigned* const*)(j->cls + 0x18);
    q.w = e->w;
    q.h = e->h;
    q.fx = v.c.fx;
    q.fz = v.c.fz;
    q.mapped = e->mapped;
    q.seat = j->seat;
    q.wgt = e->wgt;
    q.sx = *(const short*)(j->unit + UNIT_CELL);
    q.sz = *(const short*)(j->unit + UNIT_CELL + 2);
    q.heading = *(const unsigned short*)(j->unit + UNIT_HEADING);
    ts_pf_search(&s_pf, &q, &j->r);
}

/* THE RESULT of one job, through the engine's own set path 0x44F080: a path's corners, or none -- and with none it
   ORs 0x40 into the order when the unit is not in its goal (0x44F089..0x44F0B9), which is why that bit is left to
   it. The search's other flags go through the goal's 0x44CED0, as the search start ORs them. */
static void job_result(JOB* j)
{
    const unsigned flags = j->r.goalflags & ~0x40u;
    const int found = j->r.kind == TS_PF_FOUND && j->r.n >= 1 && j->r.n <= TS_PF_WP;
    if (flags && j->goal) ((PFN_GoalFlag)(size_t)VA_GOAL_FLAG)(j->goal, flags);
    if (found)
        ((PFN_SetPath)(size_t)VA_SET_PATH)(j->course, j->r.pts, j->r.n);
    else
        ((PFN_SetPath)(size_t)VA_SET_PATH)(j->course, NULL, 0);
    s_found[j->slot] = found ? j->stamp : 0;
    s_v.begun++;
    s_v.pops += (unsigned)j->r.pops;
    if (found) s_v.nfound++;
    else if (j->r.kind == TS_PF_NOSEARCH) s_v.nosearch++;
    else if (j->r.kind == TS_PF_NOPATH) s_v.nopath++;
    else if (j->r.kind == TS_PF_OFFGRID) s_v.offgrid++;
    else if (s_v.failed++ < 4)
        tagpu_logf("pathsched: a search not made (kind %d, slot %u, class %p): no path", j->r.kind, j->slot,
                   (void*)j->cls);
    if (s_log) {
        const unsigned since = s_v.ready_since[j->slot];
        const unsigned waited = since && j->stamp >= since ? j->stamp - since : 0;
        s_v.ready_since[j->slot] = 0;
        s_v.waited++;
        s_v.wait_sum += waited;
        if (waited > s_v.wait_max) s_v.wait_max = waited;
        s_v.wait_hist[waited == 0 ? 0 : waited < 3 ? 1 : waited < 10 ? 2 : waited < 30 ? 3 : waited < 90 ? 4
                      : waited < 300 ? 5 : 6]++;
    }
}

/* every 150 ticks, with `tagpu_pathlog.on`, what the scheduler did */
static void sched_line(void)
{
    const double t = s_v.ticks ? (double)s_v.ticks : 1.0;
    if (s_log) {
    tagpu_logf("pathsched: %u ticks: %.1f searches a tick (most %u; cap %d), %.0f pops (most %u; cap %d): %u found, "
               "%u no search, %u no path, %u off the grid, %u not made, %u read a patch; ready a tick: %.1f on a new "
               "goal, %.1f at a path's end, %.1f for a retry; left for the next tick %.1f (most %u); held by the retry "
               "%.1f a tick; ticks at the base weight %u, at 3 times %u, at 6 times %u",
               s_v.ticks, s_v.begun / t, s_v.begun_max, PF_CAP_SEARCHES, s_v.pops / t, s_v.pops_max, PF_CAP_POPS,
               s_v.nfound, s_v.nosearch, s_v.nopath, s_v.offgrid, s_v.failed, s_v.patched, s_v.ready_order / t,
               s_v.ready_end / t, s_v.ready_b / t, s_v.left / t, s_v.left_max, s_v.held_retry / t, s_v.weight[0],
               s_v.weight[1], s_v.weight[2]);
    tagpu_logf("pathsched: wait from ready to the search, %u searches: mean %.1f ticks, most %u; 0: %u, 1-2: %u, "
               "3-9: %u, 10-29: %u, 30-89: %u, 90-299: %u, 300+: %u; set goal: %u calls, inside the engine's 10 "
               "ticks %u left to its rule and %u opened", s_v.waited, s_v.waited ? (double)s_v.wait_sum / s_v.waited : 0.0,
               s_v.wait_max, s_v.wait_hist[0], s_v.wait_hist[1], s_v.wait_hist[2], s_v.wait_hist[3], s_v.wait_hist[4],
               s_v.wait_hist[5], s_v.wait_hist[6], s_v.goals, s_v.goals_kept, s_v.goals_opened);
    }
    memset(&s_v.calls, 0, (size_t)((char*)(&s_v + 1) - (char*)&s_v.calls));
}

/* In the engine's scheduler's place, once a tick (0x464F92). THE ORDERING the batches rest on: everything from the
   walk to the last result runs inside this one call on the game thread, and nothing it calls makes, moves or frees
   a unit, a mover, a course or a goal -- 0x440830 writes a class's map, 0x44F080 a course's own path and flags,
   0x44CED0 an order's flags -- so a pointer the walk read is the same object in every later step. */
static void __fastcall sched_tick(void* pf_unused)
{
    unsigned short ready[2][PF_SLOTS];
    JOB job[PF_BATCH];
    unsigned n[2], at[2], taken[2] = { 0, 0 }, begun = 0, pops = 0, kind, i;
    SCHED e;
    (void)pf_unused;
    if (!sched_open(&e)) return;
    tagpu_prof_enter(TAGPU_PROF_GAME, TAGPU_PG_PATHFIND);
    sched_walk(&e, ready, n);
    /* THE WEIGHT, by the tick's load: the engine's three (0x40EBBF..0x40EC06) */
    i = n[0] + n[1] > PF_CAP_SEARCHES ? 2 : n[0] + n[1] > PF_QUIET;
    e.wgt *= i == 2 ? 6 : i == 1 ? 3 : 1;
    s_v.weight[i]++;
    for (kind = 0; kind < 2; kind++)
        for (at[kind] = 0; at[kind] < n[kind] && ready[kind][at[kind]] <= s_cursor[kind]; at[kind]++)
            ;
    while (begun < PF_CAP_SEARCHES && pops < PF_CAP_POPS && taken[0] + taken[1] < n[0] + n[1]) {
        unsigned nj = 0;
        while (nj < PF_BATCH && begun + nj < PF_CAP_SEARCHES && taken[0] + taken[1] < n[0] + n[1]) {
            unsigned slot;
            kind = taken[0] < n[0] ? 0 : 1;
            slot = ready[kind][(at[kind] + taken[kind]++) % n[kind]];
            s_cursor[kind] = slot;
            job_writes(&e, &job[nj++], slot);
        }
        for (i = 0; i < nj; i++) job_search(&e, &job[i]);
        for (i = 0; i < nj; i++) {
            job_result(&job[i]);
            pops += (unsigned)job[i].r.pops;
        }
        begun += nj;
    }
    s_v.ticks++;
    s_v.ready_a += n[0];
    s_v.ready_b += n[1];
    i = n[0] + n[1] - taken[0] - taken[1];
    s_v.left += i;
    if (i > s_v.left_max) s_v.left_max = i;
    if (begun > s_v.begun_max) s_v.begun_max = begun;
    if (pops > s_v.pops_max) s_v.pops_max = pops;
    if (s_v.ticks >= 150) sched_line();
    tagpu_prof_leave(TAGPU_PROF_GAME);
}

/* The ground course's set goal 0x44F2A0, at its entry, with a goal. The function's own rule zeroes the stamp when
   the course's last search began more than 10 ticks ago (0x44F420..0x44F436), and the walk then serves the unit at
   once. Here the same is done inside those 10 ticks for a unit whose last search found a path: A UNIT ON ITS WAY
   THAT IS SENT ELSEWHERE. The function only ever zeroes the stamp, so the 0 stands. A unit whose last search
   found nothing keeps the engine's rule: an order handler answers such a search with a new goal within a tick or
   two, for as long as the unit stays where it is (MEASURED, modules/pathfinder.md), and each would be a search.
   THE BOUND: the course's unit against the unit array, as a slot of the table. */
static int __cdecl before_set_goal(void* esp)
{
    char* course = (char*)(size_t)*(const unsigned*)((const unsigned char*)esp - 0x0C);
    const char* ta = ta_main();
    const char* first;
    const char* unit;
    unsigned stamp;
    size_t slot;
    if (s_mode != MODE_OURS || !ta || !((const char* const*)esp)[1] || !course ||
        *(const unsigned*)course != VT_GROUND_COURSE)
        return 0;
    s_v.goals++;
    stamp = *(const unsigned*)(course + OBJ_LAST);
    if (stamp <= *(const unsigned*)(ta + OFF_GAMETIME) - 10u) return 0;
    first = *(const char* const*)(ta + OFF_UNIT_BEGIN);
    unit = *(const char* const*)(course + 8);
    slot = first && unit >= first ? (size_t)(unit - first) / UNIT_STRIDE : PF_SLOTS;
    if (slot >= PF_SLOTS || s_found[slot] != stamp) {
        s_v.goals_kept++;
        return 0;
    }
    s_v.goals_opened++;
    *(unsigned*)(course + OBJ_LAST) = 0;
    return 0;
}

void tagpu_pathfind_level_init(void)
{
    memset(s_found, 0, sizeof s_found);
    memset(s_cursor, 0, sizeof s_cursor);
    memset(&s_v, 0, sizeof s_v);
}
/* the oracle's observers, below */
static int __cdecl before_reset(void* esp);
static int __cdecl before_extract(void* esp);
static int __cdecl before_cleanup(void* esp);
static int __cdecl before_scheduler(void* esp);

void tagpu_pathfind_init(void)
{
    static const unsigned char X_SCHED[5]   = { 0x83, 0xEC, 0x18, 0x53, 0x55 };
    static const unsigned char X_RESET[6]   = { 0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1 };
    static const unsigned char X_EXTRACT[6] = { 0x81, 0xEC, 0x08, 0x02, 0x00, 0x00 };
    static const unsigned char X_CLEANUP[5] = { 0x8B, 0x44, 0x24, 0x04, 0x56 };
    static const unsigned char X_SETGOAL[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };
    static const unsigned char X_REMARK[8]  = { 0x83, 0xEC, 0x14, 0x0F, 0xBF, 0x54, 0x24, 0x1A };
    static const unsigned char X_SETPATH[7] = { 0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x0C };
    static const unsigned char X_GOALFLAG[7] = { 0x8B, 0x41, 0x04, 0x85, 0xC0, 0x74, 0x0C };
    s_log = tagpu_sim_lever("tagpu_pathlog.on");
    if (tagpu_sim_lever("tagpu_pathverify.on")) {
        if (!tagpu_detour_bytes_ok(VA_SCHEDULER, X_SCHED, sizeof X_SCHED) ||
            !tagpu_detour_bytes_ok(VA_GRID_RESET, X_RESET, sizeof X_RESET) ||
            !tagpu_detour_bytes_ok(VA_EXTRACT, X_EXTRACT, sizeof X_EXTRACT) ||
            !tagpu_detour_bytes_ok(VA_CLEANUP, X_CLEANUP, sizeof X_CLEANUP)) {
            tagpu_log("pathverify: NOT armed - a site does not hold the engine's bytes; the engine's scheduler runs");
            return;
        }
        if (tagpu_detour_observe(VA_SCHEDULER, X_SCHED, sizeof X_SCHED, before_scheduler, NULL) &&
            tagpu_detour_observe(VA_GRID_RESET, X_RESET, sizeof X_RESET, before_reset, NULL) &&
            tagpu_detour_observe(VA_EXTRACT, X_EXTRACT, sizeof X_EXTRACT, before_extract, NULL) &&
            tagpu_detour_observe(VA_CLEANUP, X_CLEANUP, sizeof X_CLEANUP, before_cleanup, NULL))
            s_mode = MODE_VERIFY;
        tagpu_log(s_mode == MODE_VERIFY
                      ? "pathverify: ARMED - the engine's scheduler runs, and our search beside it on every search "
                        "(0x40EB70, 0x40D900, 0x40E050, 0x440BE0)"
                      : "pathverify: NOT armed - an observer could not be installed; the engine's scheduler runs");
        return;
    }
    /* the engine functions the scheduler calls hold the engine's bytes, or it is not installed */
    if (!tagpu_detour_redirect_ok(VA_SCHED_CALL, VA_SCHEDULER) ||
        !tagpu_detour_bytes_ok(VA_SET_GOAL, X_SETGOAL, sizeof X_SETGOAL) ||
        !tagpu_detour_bytes_ok(VA_REMARK, X_REMARK, sizeof X_REMARK) ||
        !tagpu_detour_bytes_ok(VA_SET_PATH, X_SETPATH, sizeof X_SETPATH) ||
        !tagpu_detour_bytes_ok(VA_GOAL_FLAG, X_GOALFLAG, sizeof X_GOALFLAG)) {
        tagpu_log("pathsched: NOT installed - a site does not hold the engine's bytes; the engine's scheduler runs");
        return;
    }
    /* the observer does nothing until the mode is ours, so a redirect that fails leaves the engine's rules whole */
    if (!tagpu_detour_observe(VA_SET_GOAL, X_SETGOAL, sizeof X_SETGOAL, before_set_goal, NULL) ||
        !tagpu_detour_redirect(VA_SCHED_CALL, VA_SCHEDULER, (void*)sched_tick)) {
        tagpu_log("pathsched: NOT installed - a site could not be written; the engine's scheduler runs");
        return;
    }
    s_mode = MODE_OURS;
    tagpu_logf("pathsched: installed - our scheduler at the players phase's call 0x464F92, set goal 0x44F2A0 watched; "
               "%d searches and %d pops a tick, in batches of %d", PF_CAP_SEARCHES, PF_CAP_POPS, PF_BATCH);
}

SIM_FEATURE(path_verify, SIM_PUPPET_INDEPENDENT, "battle");

/* Who stands on the cells entry (x, z) reads, for the log: what makes the rule's answer when it is not the map's. */
static void patch_why(const PV* v, int x, int z)
{
    char b[400];
    int n = 0, cx, cz, seen = 0;
    unsigned prev = 0;
    b[0] = 0;
    for (cz = z - 1; cz <= z + v->c.fz && seen < 6; cz++)
        for (cx = x - 1; cx <= x + v->c.fx && seen < 6; cx++) {
            unsigned slot;
            const char* unit;
            const char* mover;
            if (cx < 0 || cz < 0 || cx >= v->w || cz >= v->h) continue;
            slot = *(const unsigned short*)(v->grid + (size_t)(cz * v->w + cx) * FT_STRIDE);
            if (!slot || slot == prev || slot > v->last) continue;
            prev = slot;
            unit = v->units + (size_t)slot * UNIT_STRIDE;
            mover = *(const char* const*)unit;
            n += _snprintf(b + n, sizeof b - (size_t)n - 1, " [%d,%d slot %u%s type %u %s stamp %u]", cx, cz, slot,
                           unit == v->absent ? " THE SEARCHER" : "", *(const unsigned short*)(unit + UNIT_TYPE),
                           mover ? "mover" : "NO MOVER", mover ? *(const unsigned*)(mover + MOVER_STAMP) : 0u);
            b[sizeof b - 1] = 0;
            if (n < 0 || n >= (int)sizeof b - 80) { seen = 6; break; }
            seen++;
        }
    tagpu_logf("pathverify: PATCH entry %d,%d reads (class footprint %d,%d, watermark %u):%s", x, z, v->c.fx, v->c.fz,
               v->wm, seen ? b : " no unit");
}

/* ---- the oracle --------------------------------------------------------------------------------------------- */

static unsigned map_code(const unsigned* map, int w, int x, int z)
{
    return map[(z >> 4) * w + x] >> (2 * (z & 15)) & 3u;
}

/* The engine's grid reset inside a search start: the watermark has run, the search has read nothing yet. */
static int __cdecl before_reset(void* esp)
{
    const char* ta;
    const char* pf;
    const char* unit;
    const char* cls;
    const char* goal;
    const char* mover;
    const unsigned* map;
    TS_PFQUERY q;
    TS_PFPATCH patch;
    PV v;
    int w, h, x, z;
    size_t ci;
    if (s_mode != MODE_VERIFY || ((const unsigned*)esp)[0] != RA_RESET_SEARCH) return 0;
    s_v.open = 0;
    ta = ta_main();
    pf = ta ? *(const char* const*)(ta + OFF_PATHFINDER) : NULL;
    if (!pf) return 0;
    unit = *(const char* const*)(pf + PF_UNIT);
    cls = *(const char* const*)(pf + PF_CLASS);
    goal = *(const char* const*)(pf + PF_GOAL);
    s_v.searches++;
    ci = (size_t)cls - VA_CLASSES;
    if (!unit || !goal || (size_t)cls < VA_CLASSES || ci % CLASS_STRIDE || ci / CLASS_STRIDE >= CLASS_COUNT) {
        s_v.refused++;
        return 0;
    }
    w = *(const int*)(cls + 0x10);
    h = *(const int*)(cls + 0x14);
    map = *(const unsigned* const*)(cls + 0x18);
    mover = *(const char* const*)unit;
    if (!map || !mover || w != *(const int*)(pf + PF_W) || h != *(const int*)(pf + PF_H) ||
        !pv_open(&v, ta, cls, unit) || v.w != w || v.h != h || !*(const char* const*)(ta + OFF_EXPLORED) ||
        !scratch_for(w, h)) {
        s_v.refused++;
        return 0;
    }
    memset(&q, 0, sizeof q);
    if (!goal_of(goal, &q.goal)) {
        s_v.other_goal++;
        return 0;
    }
    q.patch = NULL;
    if (*(const unsigned*)(mover + MOVER_STAMP) >= v.wm)
        ;                                   /* no obstacle in the map: its rectangle is read as it stands */
    else if (patch_of(&v, unit, s_v.patch, &patch)) {
        s_v.patched++;
        q.patch = &patch;
        for (z = 0; z < patch.h; z++)
            for (x = 0; x < patch.w; x++) {
                const unsigned ours = s_v.patch[z * patch.w + x], theirs = map_code(map, w, patch.x0 + x, patch.z0 + z);
                s_v.patch_entries++;
                if (ours == theirs) continue;
                if (s_v.patch_differ++ < 8) {
                    patch_why(&v, patch.x0 + x, patch.z0 + z);
                    tagpu_logf("pathverify: PATCH entry %d,%d: ours %u, the map's %u (unit cell %d,%d foot %d,%d class %u, "
                               "stamp %u watermark %u, GameTime %u)", patch.x0 + x, patch.z0 + z, ours, theirs,
                               *(const short*)(unit + UNIT_CELL), *(const short*)(unit + UNIT_CELL + 2),
                               *(const short*)(unit + UNIT_FOOT), *(const short*)(unit + UNIT_FOOT + 2),
                               (unsigned)(ci / CLASS_STRIDE), *(const unsigned*)(mover + MOVER_STAMP), v.wm,
                               *(const unsigned*)(ta + OFF_GAMETIME));
                }
            }
    } else
        s_v.patch_refused++;
    q.map = map;
    q.w = w;
    q.h = h;
    q.fx = v.c.fx;
    q.fz = v.c.fz;
    q.mapped = *(const unsigned short* const*)(ta + OFF_EXPLORED);
    q.seat = *(const unsigned char*)(pf + PF_SEAT);
    q.wgt = *(const int*)(pf + PF_WGT);
    q.sx = *(const short*)(unit + UNIT_CELL);
    q.sz = *(const short*)(unit + UNIT_CELL + 2);
    q.heading = *(const unsigned short*)(unit + UNIT_HEADING);
    if (q.seat > 9) {
        s_v.refused++;
        return 0;
    }
    ts_pf_search(&s_pf, &q, &s_v.res);
    s_v.unit = unit;
    s_v.obj = *(const char* const*)mover;
    s_v.node = *(const char* const*)(goal + 4);
    s_v.node_flags0 = s_v.node ? *(const unsigned*)(s_v.node + NODE_FLAGS) : 0;
    s_v.gametime = *(const unsigned*)(ta + OFF_GAMETIME);
    s_v.found = 0;
    s_v.open = 1;
    {
        const size_t slot = (size_t)(unit - v.units) / UNIT_STRIDE;
        if (slot < 0x10000u) {
            const unsigned since = s_v.ready_since[slot];
            const unsigned waited = since && s_v.gametime >= since ? s_v.gametime - since : 0;
            s_v.ready_since[slot] = 0;
            s_v.waited++;
            s_v.wait_sum += waited;
            if (waited > s_v.wait_max) s_v.wait_max = waited;
            s_v.wait_hist[waited == 0 ? 0 : waited < 3 ? 1 : waited < 10 ? 2 : waited < 30 ? 3 : waited < 90 ? 4
                          : waited < 300 ? 5 : 6]++;
        }
    }
    return 0;
}

static int __cdecl before_extract(void* esp)
{
    (void)esp;
    s_v.found = 1;
    return 0;
}

/* The class's cleanup at a search's end: the engine has set the path, or none. */
static int __cdecl before_cleanup(void* esp)
{
    const unsigned ra = ((const unsigned*)esp)[0];
    const char* ta = ta_main();
    const char* pf = ta ? *(const char* const*)(ta + OFF_PATHFINDER) : NULL;
    const TS_PFRESULT* r = &s_v.res;
    const char* why = NULL;
    int want, spanned;
    if (s_mode != MODE_VERIFY || !s_v.open || !pf || ((const char* const*)esp)[1] != s_v.unit) return 0;
    s_v.open = 0;
    if (ra != RA_END_OFFGRID && ra != RA_END_NOSEARCH && ra != RA_END_SCHEDULER) return 0;
    if (r->kind == TS_PF_OVERFLOW) { s_v.overflow++; return 0; }
    if (r->kind == TS_PF_REFUSED) { s_v.refused++; return 0; }
    want = ra == RA_END_OFFGRID ? TS_PF_OFFGRID : ra == RA_END_NOSEARCH ? TS_PF_NOSEARCH
           : s_v.found ? TS_PF_FOUND : TS_PF_NOPATH;
    spanned = *(const unsigned*)(ta + OFF_GAMETIME) != s_v.gametime;
    if (want != r->kind)
        why = "the exit";
    else if (s_v.node && *(const unsigned*)(s_v.node + NODE_FLAGS) != (s_v.node_flags0 | r->goalflags))
        why = "the order's flags";
    else if ((want == TS_PF_FOUND || want == TS_PF_NOPATH) &&
             (*(const int*)(pf + PF_USED) != r->used || *(const int*)(pf + PF_RELAX) != r->relax))
        why = "the pool's use or the bug walk's answer";
    else if (want == TS_PF_FOUND && s_v.obj) {
        const int n = *(const int*)(s_v.obj + OBJ_COUNT);
        if (*(const short*)(pf + PF_FOUND) != r->found_x || *(const short*)(pf + PF_FOUND + 2) != r->found_z)
            why = "the goal cell";
        else if (n != r->n || n < 0 || n > TS_PF_WP || memcmp(s_v.obj + OBJ_WP, r->pts, (size_t)n * 4))
            why = "the path";
        else if (!(*(const unsigned char*)(s_v.obj + OBJ_FLAGS) & 1))
            why = "the path's flag";
    } else if (want != TS_PF_FOUND && s_v.obj && (*(const unsigned char*)(s_v.obj + OBJ_FLAGS) & 3))
        why = "the path's flags after no path";
    if (spanned) s_v.spanned++;
    if (!why) {
        if (!spanned) s_v.equal++;
        return 0;
    }
    if (spanned) s_v.spanned_differ++;
    else s_v.differ++;
    if (s_v.logged++ < 8)
        tagpu_logf("pathverify: DIFFERENT in %s%s: engine exit %d used %d relax %d count %d, ours exit %d used %d relax %d "
                   "count %d pops %d (unit cell %d,%d, GameTime %u)", why, spanned ? " (the search spanned ticks)" : "",
                   want, *(const int*)(pf + PF_USED), *(const int*)(pf + PF_RELAX),
                   s_v.obj ? *(const int*)(s_v.obj + OBJ_COUNT) : -1, r->kind, r->used, r->relax, r->n, r->pops,
                   *(const short*)(s_v.unit + UNIT_CELL), *(const short*)(s_v.unit + UNIT_CELL + 2), s_v.gametime);
    return 0;
}

/* The scheduler's entry, once a tick: the gate's census, and every 150 calls the summary line. A unit wants a search
   by bit 1 of its class object; the gate 0x44F260 holds it until 60 ticks after its last search began. Why it wants
   one is read as the mover's step 0x44F1A0 sets the bit: blocked (mover +0x2E bit 2, 0x44F245), else out of
   waypoints (count below 2, 0x44F24B), else by set goal 0x44F2E8, a new order. The walk tests what the scheduler's
   examination tests, in its order (0x40ED33..0x40ED4D): the slot's type, its mover, the mover's class. */
static int __cdecl before_scheduler(void* esp)
{
    const char* ta = ta_main();
    const char* first;
    const char* last;
    const char* u;
    unsigned now, held = 0, ready = 0;
    (void)esp;
    if (s_mode != MODE_VERIFY || !ta) return 0;
    first = *(const char* const*)(ta + OFF_UNIT_BEGIN);
    last = *(const char* const*)(ta + OFF_UNIT_END);
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    if (first && last >= first && (size_t)(last - first) / UNIT_STRIDE < 0x10000u) {
        for (u = first + UNIT_STRIDE; u <= last; u += UNIT_STRIDE) {
            const char* mover;
            const char* obj;
            unsigned char flags;
            size_t slot;
            if (!*(const unsigned short*)(u + UNIT_TYPE) || !(mover = *(const char* const*)u) ||
                !*(const char* const*)(mover + MOVER_CLASS) || !(obj = *(const char* const*)mover) ||
                *(const unsigned*)obj != VT_GROUND_COURSE)
                continue;
            flags = *(const unsigned char*)(obj + OBJ_FLAGS);
            slot = (size_t)(u - first) / UNIT_STRIDE;
            if (!(flags & 2)) {
                s_v.ready_since[slot] = 0;
                continue;
            }
            s_v.want++;
            if (now >= *(const unsigned*)(obj + OBJ_LAST) + 60u) {
                if (!s_v.ready_since[slot]) s_v.ready_since[slot] = now ? now : 1;
                ready++;
                continue;
            }
            s_v.ready_since[slot] = 0;
            held++;
            if (*(const unsigned char*)(mover + MOVER_FLAGS) & 4) s_v.held_blocked++;
            else if (*(const int*)(obj + OBJ_COUNT) < 2) s_v.held_short++;
            else s_v.held_order++;
        }
        s_v.held += held;
        if (held > s_v.held_max) s_v.held_max = held;
        if (ready > s_v.ready_max) s_v.ready_max = ready;
        s_v.census_ticks++;
    }
    if (++s_v.calls < 150) return 0;
    {
        const double t = s_v.census_ticks ? (double)s_v.census_ticks : 1.0;
        tagpu_logf("pathverify: %u searches: %u equal, %u DIFFERENT; %u spanned ticks (%u of them different); "
                   "%u overflowed, %u refused, %u of a goal kind the core has not; patch: %u searches, %u entries, "
                   "%u DIFFERENT, %u refused", s_v.searches, s_v.equal, s_v.differ, s_v.spanned, s_v.spanned_differ,
                   s_v.overflow, s_v.refused, s_v.other_goal, s_v.patched, s_v.patch_entries, s_v.patch_differ,
                   s_v.patch_refused);
        tagpu_logf("pathverify: gate, a tick: %.1f units want a search, %.1f held by the 60 ticks (most %u): "
                   "%.1f blocked, %.1f out of waypoints, %.1f on a new order; %.1f with the gate open (most %u)",
                   s_v.want / t, s_v.held / t, s_v.held_max, s_v.held_blocked / t, s_v.held_short / t,
                   s_v.held_order / t, (s_v.want - s_v.held) / t, s_v.ready_max);
        tagpu_logf("pathverify: wait from the gate opening to the search's start, %u searches: mean %.1f ticks, most %u; "
                   "0: %u, 1-2: %u, 3-9: %u, 10-29: %u, 30-89: %u, 90-299: %u, 300+: %u", s_v.waited,
                   s_v.waited ? (double)s_v.wait_sum / s_v.waited : 0.0, s_v.wait_max, s_v.wait_hist[0], s_v.wait_hist[1],
                   s_v.wait_hist[2], s_v.wait_hist[3], s_v.wait_hist[4], s_v.wait_hist[5], s_v.wait_hist[6]);
    }
    memset(&s_v.calls, 0, (size_t)((char*)(&s_v + 1) - (char*)&s_v.calls));
    return 0;
}
