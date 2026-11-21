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
     1. THE WALK, one pass over the unit array. Every unit of every player the engine's scheduler serves, in slot
        order, whose ground course wants a search (bit 1 of its +0x64) is READY when
          - its stamp +0x60 is 0: a new goal. Set goal 0x44F2A0 zeroes the stamp when the last search began 10
            ticks ago or more, and before_set_goal inside those 10 ticks when that search found a path;
          - or it is not blocked and has fewer than two corners left (0x44F24B) of a path its last search found:
            it has followed its path to the end;
          - or its last search began PF_RETRY ticks ago or more: a blocked unit, or one whose search found nothing.
        The first two kinds are served before the third, each kind from its own cursor on: within a kind the
        caps starve no unit, and a jam never delays a new goal -- the third kind waits while the first two fill
        the caps.
     2. THE TICK'S SEARCHES. At most PF_CAP_SEARCHES units are CHOSEN, and THE WRITES made for each (the unit's
        stamp; its class's watermark and the marking of the units that have just become obstacles, once a class a
        tick, as 0x440AF0's sweep; an obstacle searcher's rectangle recomputed in the map through the engine's
        0x440830, as the cleanup would). THE SEARCHES follow in the order chosen, in groups of PF_BATCH: a group
        begins only while the pops of the groups before it are below PF_CAP_POPS. Both caps count work, never
        time, and the group is a constant, so the searches a tick makes are the same on every machine, however
        many threads make them. THE RESULTS are applied in that order, through the engine's own set path 0x44F080
        and its flag call 0x44CED0; a unit chosen whose group did not begin gets its stamp back and the cursor
        does not pass it: it is chosen again as though it had not been.
     3. IN PLAY THE RESULTS ARE THE JOIN'S, on every machine: THE JOIN, on the game thread, stands at the step's
        call 0x4955E5, after the particles, and before that at the first set goal of a ground course -- the one
        engine function in between that writes what a result writes -- so a set goal finds every result applied.
        In the loader's call the join is the scheduler's own last step.
     4. THE SEARCHES RUN WHILE THE TICK GOES ON where there is a helper and the memory for it: a search then reads
        no engine memory: its query holds the goal, the unit's cell and heading and the patch, and names THE TICK'S
        COPIES of its class's map and of the explored map, made after the writes. The helpers search while the
        game thread leaves the scheduler and runs the rest of the players phase, the features and the particles,
        and the join waits for them. Elsewhere the searches are made inside the scheduler, from the engine's own
        maps, and their results kept for the join: the same searches, the same results, applied at the same
        place.
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
   ON A LEVEL THAT PLAYS A RECORDING the scheduler and the set-goal observer do nothing: the recording is the only
   writer there, and every seat's course is a proxy besides (replay-layout.tsv, mover +0x000).
   `tagpu_pathlog.on` logs what the scheduler did, every 150 ticks.

   THE VERIFY LEVER `tagpu_pathverify.on` leaves the engine's scheduler in place and runs our search beside
   its searches: at the engine's grid reset 0x40D900 inside a search start -- after the watermark, so the map holds
   what the engine's search is about to read -- it computes the patch, counts the entries that differ from the
   map's (the engine has just recomputed the same ones: any difference is a fault in our rule), runs the search on
   the scratch and keeps the result; at the search's end, the class's cleanup 0x440BE0, it compares that with what
   the engine wrote: the exit, the path's points and count, the goal cell, the pool's use, the bug walk's answer
   and the flags ORed into the order. It writes no engine memory and decides nothing.

   EVERYTHING BUT THE SEARCHES RUNS ON THE THREAD THAT RUNS THE PLAYERS PHASE, inside it, inside a set goal the
   simulation calls, or at the step's join: the game thread in play, and once the loader at 0x497C10 during a load,
   after the level init and before the game thread's first step (tagpu_income.c's line of thread-split.allow has
   the handshake). That thread owns what is read and written here, and every engine write is its own. A helper
   reads engine memory only in the tick's first handing-out (the jobs' queries -- the goal, the unit's cell and
   heading, the class record, the patch -- and the copies), while that thread waits inside the scheduler; in the second (the searches) it reads the jobs and the copies alone (run_begin has the
   handshake). In the loader's call everything runs on the loader's thread. A value
   taken from engine memory is data until bounded: a player's block against the unit array, the class record
   against the 32 records, the grid's size against the class's and the pathfinder's, a cell's unit slot against the
   unit array's end, a feature definition against the count, the patch's rectangle against PF_PATCH_MAX, a unit's
   slot against the table of stamps, and at the join a job's unit, mover, course and goal against what its slot
   holds then. */
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
#include "tagpu_puppet.h"
#include "tasim_path.h"

SIM_FEATURE(path_sched, SIM_PUPPET_OWNER, "battle battle-allied stock-battle-allied formation-basic");

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
#define VA_JOIN_CALL     0x004955E5u    /* the step's call after the particles, of 0x466580                */
#define VA_STEP_COUNT    0x00466580u    /* (void), cdecl: a countdown word of main's, nothing of a unit    */

/* THE RULES' NUMBERS. A tick chooses at most PF_CAP_SEARCHES units, which bounds a tick that many units ask at
   once, and begins no group of PF_BATCH searches once the pops of the groups before it have reached PF_CAP_POPS,
   which bounds a tick whose searches are long (one that finds no way pops every cell it can reach; MEASURED
   2026-11-20 with no such cap: 1 621 833 pops in one tick of the battle, against 218 431 and 498 716 with it,
   a group of long searches past the cap). PF_QUIET is
   the most units ready in a tick that searches at the base weight (MEASURED: 2900 pops a search at the base, 510
   at 3 times, 410 at 6 times). */
#define PF_BATCH         16
#define PF_THREADS       4              /* the most threads that search: the scratches made                */
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

/* THE HELPERS: threads made at install that work beside the scheduler's own thread, and the two handles a
      handing-out is made and waited for through. All three are written at install and never closed. */
static unsigned s_helpers;
SIM_VAR(s_helpers, SIM_INSTALL);
static HANDLE s_go;                     /* a semaphore: one count wakes one helper for the tasks in s_run */
SIM_VAR(s_go, SIM_INSTALL);
static HANDLE s_done;                   /* an event: a helper's answer was the last owed, or a group began */
SIM_VAR(s_done, SIM_INSTALL);

/* THE SCRATCHES, one for each thread that searches: s_pf[0] the scheduler's own thread's and the oracle's, s_pf[k]
   helper k's. Each is written by a search before it reads it, and left with every cell new (ts_pf_search). They
   share one block, ours alone to free; the TS_PFs over it are rebuilt whenever they do not describe that block.
   The ones laid out are 1 + s_helpers, or s_pf[0] alone when that much memory was not to be had; one not laid out
   has no pool. */
static TS_PF s_pf[PF_THREADS];
SIM_VAR(s_pf, SIM_TRANSIENT);
static unsigned char* s_mem;
static unsigned s_mem_bytes;
SIM_VAR(s_mem_bytes, SIM_TRANSIENT);
SIM_HEAP(s_mem, SIM_TRANSIENT, SIM_ID_NONE, 1, s_mem_bytes, NULL);

/* THE TICK'S COPIES, what a search reads of the engine while the tick goes on: the explored map, then the maps of
   the classes the tick searches, one a slot, of the scratches' grid: as many slots as PF_COPY_BYTES holds, 32 at
   most. Written after the tick's writes and before its searches are handed out, read by those searches, and
   nothing after the join. NULL when the memory was not to be had; then, and in a tick that searches more classes
   than there are slots, the searches are made inside the scheduler and read the engine's own. */
#define PF_COPY_BYTES (32u << 20)
#define PF_WALK_PARTS 16                /* the walk's parts: any number makes the same lists */
static unsigned char* s_frz;
static unsigned s_frz_bytes;
static unsigned s_frz_no;               /* the smallest size refused, 0 for none */
SIM_VAR(s_frz_no, SIM_TRANSIENT);
SIM_VAR(s_frz_bytes, SIM_TRANSIENT);
SIM_HEAP(s_frz, SIM_TRANSIENT, SIM_ID_NONE, 1, s_frz_bytes, NULL);

/* a class's map is two bits a cell, a dword a column of 16 rows (0x40D7B0's read, tasim_path.c rd_code); the
   explored map a word a 32-px block; each copy on a 16-byte boundary */
static size_t frz_class_bytes(int w, int h) { return (size_t)w * (size_t)((h + 15) >> 4) * 4u; }
static size_t frz_explored_bytes(int w, int h) { return (size_t)(w >> 1) * (size_t)(h >> 1) * 2u; }
static size_t frz_round(size_t n) { return (n + 15u) & ~(size_t)15u; }
static unsigned char* frz_class(int w, int h, unsigned slot)
{
    return s_frz + frz_round(frz_explored_bytes(w, h)) + slot * frz_round(frz_class_bytes(w, h));
}
static unsigned frz_slots(int w, int h)
{
    const size_t ex = frz_round(frz_explored_bytes(w, h)), cl = frz_round(frz_class_bytes(w, h));
    return s_frz && cl && s_frz_bytes > ex ? (unsigned)((s_frz_bytes - ex) / cl) : 0;
}
/* Room for `ncls` classes' copies: the block grows to the most classes a tick of the level has searched, so a game
   holds what its ticks use. Called with nothing due and no handing-out begun (sched_tick), so no search reads what
   it frees. 0 when the room is past PF_COPY_BYTES or not to be had; a size refused is not asked for again. */
static int frz_room(int w, int h, unsigned ncls)
{
    const size_t ex = frz_round(frz_explored_bytes(w, h)), cl = frz_round(frz_class_bytes(w, h));
    size_t bytes;
    if (ncls <= frz_slots(w, h)) return 1;
    if (!cl || ncls > CLASS_COUNT || ex >= PF_COPY_BYTES || ncls > (PF_COPY_BYTES - ex) / cl) return 0;
    bytes = ex + ncls * cl;
    if (s_frz_no && bytes >= s_frz_no) return 0;
    if (s_frz) SIM_FREE(s_frz, s_frz);
    s_frz_bytes = 0;
    s_frz = (unsigned char*)SIM_ALLOC(s_frz, bytes);
    if (!s_frz) {
        s_frz_no = (unsigned)bytes;
        return 0;
    }
    s_frz_bytes = (unsigned)bytes;
    return 1;
}

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
    int refused_said;               /* sched_open's line, once a level */
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
    /* ticks whose searches ran while the tick went on; of those, the ones a set goal joined; searches of the group
       in hand the join found not ended; results whose unit no longer stood as searched */
        unsigned late, joined_goal, unended, dropped;
    /* the join's wait for searches made beside the tick, in QueryPerformanceCounter's units: the ticks that
       waited a tenth of a millisecond or more, by the tick's weight; the sum and the longest */
    unsigned join_waits[3];
    unsigned long long join_sum, join_max;
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

/* The scratches for a w x h grid, made again when the grid's size changes (a new map): one for each thread that
   searches, or one alone when that much memory is not to be had -- the searches are then the same ones, one after
   another. With more than one, the tick's copies beside them, when that memory is to be had too. Nothing is due
   where this is called (sched_tick joins first), so no search reads what it frees. */
static int scratch_for(int w, int h)
{
    static const TS_PFSEAT none[TS_PF_SEATS];
    const int nodes = (long long)w * h < PF_POOL ? w * h : PF_POOL;
    const size_t need = (ts_pf_bytes_n(w, h, 2, nodes) + 15u) & ~(size_t)15u;
    unsigned n, i;
    if (s_mem && s_pf[0].pool && s_pf[0].w == w && s_pf[0].h == h) return 1;
    if (s_mem) SIM_FREE(s_mem, s_mem);
    if (s_frz) SIM_FREE(s_frz, s_frz);
    s_mem = s_frz = NULL;
    s_mem_bytes = s_frz_bytes = s_frz_no = 0;
    memset(s_pf, 0, sizeof s_pf);
    if (need < 16) return 0;
    for (n = 1 + s_helpers; n && !s_mem; n = n > 1 ? 1 : 0) {
        if (need > 0x7FFFFFFFu / n) continue;
        s_mem = (unsigned char*)SIM_ALLOC(s_mem, need * n);
        if (s_mem) break;
    }
    if (!s_mem) return 0;
    s_mem_bytes = (unsigned)(need * n);
    for (i = 0; i < n; i++)
        if (ts_pf_init_n(&s_pf[i], s_mem + i * need, need, w, h, 2, nodes, 0, none)) {
            SIM_FREE(s_mem, s_mem);
            s_mem = NULL;
            s_mem_bytes = 0;
            memset(s_pf, 0, sizeof s_pf);
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
    unsigned wm;                    /* the tick's watermark, every class's: max(now, 30) - 30 (0x440B03) */
    int play;                       /* no load in hand: the game thread runs this tick (sched_tick)      */
    int late;                       /* the searches read the tick's copies and end at the join           */
    /* THE WALK'S LISTS (walk_part, walk_pack): the caller's, ascending by slot once packed */
    unsigned short (*ready)[PF_SLOTS];
    unsigned nready[2];
    unsigned nstill;                /* THE STILL UNITS                                                   */
    unsigned short* still_slot;
    unsigned* still_stamp;
    struct { unsigned n[2], nstill, order, end, held; } part[PF_WALK_PARTS];
    unsigned nblock;                /* the served players' blocks of the unit array, ascending           */
    struct { char* b0; char* b1; unsigned char seat; } block[10];
} SCHED;

/* one search of a tick: what the walk chose, its query, its result */
typedef struct JOB {
    char* unit;
    char* mover;
    char* course;
    char* goal;
    char* cls;
    unsigned slot, stamp, ci;       /* ci: the class's index among the 32, bounded for a whole job       */
    unsigned stamp0, kind;          /* the course's stamp before the writes; the ready list it came from */
    int whole;                      /* every value the search rests on is inside its bound              */
    int ready;                      /* the query is made: the search runs                                */
    unsigned char seat;
    unsigned char patched, other;   /* the search read a patch; its goal is of a kind the search has not */
    TS_PFPATCH patch;
    TS_PFQUERY q;
    TS_PFRESULT r;
    unsigned char code[PF_PATCH_MAX];
} JOB;

/* THE TICK'S JOBS, and how many of them are DUE: chosen, their writes made, their results not yet applied. All
   written by the scheduler before a search reads them; nothing is due between two ticks (the join). `held`: the
   results wait for the step's join, which only the game thread in play sets, so a set goal on another thread
   during a load finds it 0. `ended`: the searches were made inside the scheduler, `searched` of them. `cls`
   lists the classes the tick's whole jobs search, each once, and `slot` each one's copy. */
static JOB s_job[PF_CAP_SEARCHES];
SIM_VAR(s_job, SIM_TRANSIENT);
static struct {
    unsigned n, ncls, searched, wgt;        /* wgt: the tick's weight, 0 the base, 1 and 2 (the log) */
    unsigned char cls[CLASS_COUNT], slot[CLASS_COUNT];
    int held, ended;
} s_due;
SIM_VAR(s_due, SIM_TRANSIENT);

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

/* The tick's view of the engine, 0 with no game to serve. A game whose unit array or grid is outside what the
   tables here hold would leave every ground unit with no path, so that is said once a level. */
static int sched_open(SCHED* e)
{
    const char* why = NULL;
    unsigned p;
    memset(e, 0, sizeof *e);
    e->ta = (char*)ta_main();
    if (!e->ta || tagpu_puppet_on() || !*(const unsigned short*)(e->ta + OFF_PLAYER_COUNT)) return 0;
    e->pf = *(char* const*)(e->ta + OFF_PATHFINDER);
    e->first = *(char* const*)(e->ta + OFF_UNIT_BEGIN);
    e->last = *(char* const*)(e->ta + OFF_UNIT_END);
    if (!e->pf || !e->first || e->last < e->first) return 0;
    e->now = *(const unsigned*)(e->ta + OFF_GAMETIME);
    e->w = *(const int*)(e->pf + PF_W);
    e->h = *(const int*)(e->pf + PF_H);
    e->mapped = *(const unsigned short* const*)(e->ta + OFF_EXPLORED);
    e->wgt = *(const int*)(e->pf + PF_BASE_WEIGHT);
    e->wm = (e->now > 30 ? e->now : 30) - 30;
    e->play = *(const unsigned short*)(e->ta + OFF_LOADFLAGS) == 0;
    if ((size_t)(e->last - e->first) / UNIT_STRIDE >= PF_SLOTS) why = "the unit array is longer than the table of stamps";
    else if (!e->mapped) why = "there is no explored map";
    else if (e->w < 1 || e->h < 1 || !scratch_for(e->w, e->h)) why = "no scratch could be made for the grid";
    if (why) {
        if (!s_v.refused_said) tagpu_logf("pathsched: NO GROUND UNIT GETS A PATH in this game: %s", why);
        s_v.refused_said = 1;
        return 0;
    }
    /* the blocks in the unit array's order, whatever the players' order is, so that a ready list ascends by slot */
    for (p = 0; p < 10; p++) {
        char* b0;
        char* b1;
        unsigned i;
        if (!served_block(e, p, &b0, &b1)) continue;
        for (i = e->nblock++; i && e->block[i - 1].b0 > b0; i--) e->block[i] = e->block[i - 1];
        e->block[i].b0 = b0;
        e->block[i].b1 = b1;
        e->block[i].seat = (unsigned char)p;
    }
    return 1;
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

/* THE WALK, over the unit array. ready[0] takes the units served first -- a new goal (the stamp is 0), and a unit
   that has followed its path to its end -- and ready[1] the others whose last search began PF_RETRY ticks ago:
   slots of the served players' blocks, ascending, which the cursors' rotation rests on.
   THE STILL UNITS are what a class's sweep marks from (class_tick): every slot from 1 on whose unit is alive
   (+0x110 bit 28), has a mover and a claim stamp below the tick's watermark, with that stamp. Nothing inside the
   tick's call writes a unit's alive bit, its mover or a mover's stamp, so the list holds for every class the tick
   sweeps.
   IN PARTS: part k reads the slots [S k / P, S (k + 1) / P) of the array's S and writes its lists from its first
   slot's place in each of the caller's arrays -- a slot gives each list one entry at most, so no part reaches
   another's -- and its counts into its own record; walk_pack then closes the lists up in the parts' order. What
   is listed does not depend on how many parts there are or which thread walked which. */
static void walk_part(SCHED* e, unsigned k)
{
    const unsigned slots = (unsigned)((size_t)(e->last - e->first) / UNIT_STRIDE) + 1;
    const unsigned a = (unsigned)((unsigned long long)slots * k / PF_WALK_PARTS);
    const unsigned z = (unsigned)((unsigned long long)slots * (k + 1) / PF_WALK_PARTS);
    unsigned n[2] = { 0, 0 }, nstill = 0, order = 0, end = 0, held = 0, b = 0, slot;
    for (slot = a; slot < z; slot++) {
        const char* const u = e->first + (size_t)slot * UNIT_STRIDE;
        const char* const mover = *(const char* const*)u;
        const char* course;
        unsigned stamp, age;
        int kind;
        if (slot && mover && (*(const unsigned*)(u + UNIT_STATE) & 0x10000000u) &&
            *(const unsigned*)(mover + MOVER_STAMP) < e->wm) {
            e->still_slot[a + nstill] = (unsigned short)slot;
            e->still_stamp[a + nstill++] = *(const unsigned*)(mover + MOVER_STAMP);
        }
        while (b < e->nblock && u > e->block[b].b1) b++;
        if (b >= e->nblock || u < e->block[b].b0) continue;
        if (!(course = wanting_course(u))) {
            if (s_log) s_v.ready_since[slot] = 0;
            continue;
        }
        stamp = *(const unsigned*)(course + OBJ_LAST);
        age = e->now - stamp;
        if (!stamp) {
            kind = 0;
            order++;
        } else if (!(*(const unsigned char*)(mover + MOVER_FLAGS) & 4) && *(const int*)(course + OBJ_COUNT) < 2 &&
                   s_found[slot] == stamp) {
            kind = 0;
            end++;
        } else if (age >= PF_RETRY)
            kind = 1;
        else {
            if (s_log) s_v.ready_since[slot] = 0;
            held++;
            continue;
        }
        e->ready[kind][a + n[kind]++] = (unsigned short)slot;
        if (s_log && !s_v.ready_since[slot]) s_v.ready_since[slot] = e->now ? e->now : 1;
    }
    e->part[k].n[0] = n[0];
    e->part[k].n[1] = n[1];
    e->part[k].nstill = nstill;
    e->part[k].order = order;
    e->part[k].end = end;
    e->part[k].held = held;
}

static void walk_pack(SCHED* e)
{
    const unsigned slots = (unsigned)((size_t)(e->last - e->first) / UNIT_STRIDE) + 1;
    unsigned k, kind;
    for (k = 0; k < PF_WALK_PARTS; k++) {
        const unsigned a = (unsigned)((unsigned long long)slots * k / PF_WALK_PARTS);
        for (kind = 0; kind < 2; kind++) {
            memmove(e->ready[kind] + e->nready[kind], e->ready[kind] + a, e->part[k].n[kind] * sizeof **e->ready);
            e->nready[kind] += e->part[k].n[kind];
        }
        memmove(e->still_slot + e->nstill, e->still_slot + a, e->part[k].nstill * sizeof *e->still_slot);
        memmove(e->still_stamp + e->nstill, e->still_stamp + a, e->part[k].nstill * sizeof *e->still_stamp);
        e->nstill += e->part[k].nstill;
        s_v.ready_order += e->part[k].order;
        s_v.ready_end += e->part[k].end;
        s_v.held_retry += e->part[k].held;
    }
}

/* The class's watermark, and the marking of the units that have just become obstacles: 0x440AF0's own, less its
   searcher (0x440B03..0x440B18, 0x440B4E..0x440BB8) -- the alive units with a mover whose stamp is in
   [the class's old watermark, the new one), in slot order, here taken from the walk's still units. The watermark
   moves once a tick, so the sweep runs for the first search of a class in a tick and for no other. */
static void class_tick(const SCHED* e, char* cls)
{
    const unsigned old = *(const unsigned*)(cls + 0x1C);
    unsigned i;
    *(unsigned*)(cls + 0x1C) = e->wm;
    if (e->wm == old) return;
    for (i = 0; i < e->nstill; i++)
        if (e->still_stamp[i] >= old) {
            const char* u = e->first + (size_t)e->still_slot[i] * UNIT_STRIDE;
            ((PFN_Remark)(size_t)VA_REMARK)(cls, *(const unsigned*)(u + UNIT_CELL), *(const unsigned*)(u + UNIT_FOOT));
        }
}

/* A UNIT CHOSEN: its job, and its course's stamp -- the engine's wants-search's (0x44F27E), held off 0, which
   here reads as a new goal. THE BOUNDS a search rests on: the class record among the 32, its map of the
   pathfinder's grid and of the cells' grid, a goal. A unit one of them refuses is not searched and gets the result
   of a search that found nothing. */
static void job_pick(const SCHED* e, JOB* j, unsigned slot)
{
    size_t ci;
    unsigned b;
    memset(j, 0, offsetof(JOB, code));
    j->r.kind = TS_PF_REFUSED;
    j->slot = slot;
    j->unit = e->first + (size_t)slot * UNIT_STRIDE;
    j->mover = *(char* const*)j->unit;
    j->course = *(char* const*)j->mover;
    j->goal = *(char* const*)(j->course + 4);
    j->cls = *(char* const*)(j->mover + MOVER_CLASS);
    j->stamp = e->now ? e->now : 1;
    j->stamp0 = *(const unsigned*)(j->course + OBJ_LAST);
    *(unsigned*)(j->course + OBJ_LAST) = j->stamp;
    j->seat = 10;
    for (b = 0; b < e->nblock; b++)
        if (j->unit >= e->block[b].b0 && j->unit <= e->block[b].b1) j->seat = e->block[b].seat;
    ci = (size_t)j->cls - VA_CLASSES;
    if (j->seat > 9 || !j->goal || (size_t)j->cls < VA_CLASSES || ci % CLASS_STRIDE || ci / CLASS_STRIDE >= CLASS_COUNT ||
        !*(const unsigned* const*)(j->cls + 0x18) || *(const int*)(j->cls + 0x10) != e->w ||
        *(const int*)(j->cls + 0x14) != e->h || *(const int*)(e->ta + OFF_MAP_W16) != e->w ||
        *(const int*)(e->ta + OFF_MAP_H16) != e->h)
        return;
    j->whole = 1;
    j->ci = (unsigned)(ci / CLASS_STRIDE);
}

/* THE QUERY of one job, made from the engine's memory while the scheduler's thread is inside the scheduler: the
   goal, the unit's cell and heading, the patch, and the maps the search reads -- the tick's copies when the
   searches end at the join, else the engine's own. A job with no query is not searched. */
static void job_ready(const SCHED* e, JOB* j)
{
    TS_PFQUERY* q = &j->q;
    PV v;
    if (!goal_of(j->goal, &q->goal)) {
        j->other = 1;
        return;
    }
    if (!pv_open(&v, e->ta, j->cls, j->unit)) return;
    if (*(const unsigned*)(j->mover + MOVER_STAMP) < v.wm) {
        if (!patch_of(&v, j->unit, j->code, &j->patch)) return;
        q->patch = &j->patch;
        j->patched = 1;
    }
    q->map = e->late ? (const unsigned*)frz_class(e->w, e->h, s_due.slot[j->ci])
                     : *(const unsigned* const*)(j->cls + 0x18);
    q->w = e->w;
    q->h = e->h;
    q->fx = v.c.fx;
    q->fz = v.c.fz;
    q->mapped = e->late ? (const unsigned short*)s_frz : e->mapped;
    q->seat = j->seat;
    q->wgt = e->wgt;
    q->sx = *(const short*)(j->unit + UNIT_CELL);
    q->sz = *(const short*)(j->unit + UNIT_CELL + 2);
    q->heading = *(const unsigned short*)(j->unit + UNIT_HEADING);
    j->ready = 1;
}

/* One of the tick's copies: 0 the explored map, k the map of class s_due.cls[k - 1] into slot k - 1. THE BOUND on
   each length: a whole job of the class has held the class's grid, the cells' and the pathfinder's to one size
   (job_pick), and the bytes copied are the ones the engine's reader 0x40D7B0 indexes for a grid of that size
   -- the engine's own allocations, ((h + 15) >> 4) w dwords (0x44097E..0x4409B8) and w h / 2 bytes (0x483CF6). */
static void frz_copy(const SCHED* e, unsigned k)
{
    if (!k)
        memcpy(s_frz, e->mapped, frz_explored_bytes(e->w, e->h));
    else {
        const unsigned ci = s_due.cls[k - 1];
        memcpy(frz_class(e->w, e->h, k - 1), *(const void* const*)(size_t)(VA_CLASSES + ci * CLASS_STRIDE + 0x18),
               frz_class_bytes(e->w, e->h));
    }
}

/* THE TASKS HANDED OUT, on the scheduler's thread and the helpers. s_run is the handing-out in hand, all zero
   outside one. A tick makes two, each begun by run_begin and ended by run_end on the scheduler's thread:
     THE FIRST, begun and ended inside the scheduler: the jobs' queries, and the tick's copies when the searches
       end at the join. Its tasks read engine memory.
     THE SECOND, the searches: begun inside the scheduler and ended at the join. Its tasks read their own job,
       the maps its query names and their thread's scratch, and write that job's result and that scratch.
   Tasks 0 .. limit - 1 may be claimed. THE GATE: the thread whose task brings `done` to `limit` -- every task
   claimed has ended, so no other thread is in one -- asks the handing-out's gate, and either raises the limit
   by a group or leaves it, which ends the handing-out. The gate reads the ended tasks' results and nothing
   else, so where the limit stops does not depend on which threads ran what, or when.
   THE ORDERING the helpers rest on, a handshake:
     - a helper reads s_run, a job, a copy or engine memory only between a count of s_go it has taken and its own
       decrement of `pending`; `pending` counts the counts released and not yet answered, and the scheduler's
       thread while it is inside run_end's taking;
     - the scheduler's thread writes s_run before it releases the first counts, and after that writes neither
       s_run (but as a taker), nor a job, nor a copy, nor frees a scratch or the copies, until run_end has
       returned: run_end takes what tasks it can, and returns only when its own decrement brings `pending` to
       0. A thread that raises the limit adds the counts it is about to release to `pending` while it is itself
       counted, so `pending` is 0 only when no count is unanswered and no thread can raise the limit: nothing
       reads after. s_done is set at each fall of `pending` to 0 by a helper's answer and at each raise of the
       limit; run_end waits on it only with `pending` above 0 and looks again after each wait, so a signal left
       from an earlier handing-out costs one more look and nothing else, and a fall is never missed: some thread
       still owes the decrement that sets it.
   IN THE FIRST the scheduler's thread, the game thread (helpers are woken only in play), does not leave
   run_end, so while a helper reads engine memory the engine's state is what the tick's writes left: no other
   thread of the process writes or frees what a task reads (the mouse thread writes its lock bytes and the
   cursor's positions, engine/process.md; the render thread writes no engine memory, thread-split.allow). s_run
   names the scheduler's frame there, so nothing may unwind past it: sched_tick is left by its return alone.
   IN THE SECOND the game thread goes on with the tick, and a search reads NO engine memory: its maps are the
   tick's copies (job_ready), which with the jobs and the scratches are ours alone, written before the counts
   were released and by nothing until the join's run_end has returned. What the engine does meanwhile cannot
   reach a search, and what a search finds cannot depend on when it ran.
   A task is claimed by one thread, the one whose exchange of `next` took it. The results do not depend on
   which thread ran which task, or on how many threads there are: a task reads nothing another task of its
   handing-out writes. */
static struct {
    void (*task)(unsigned i, TS_PF* pf);
    int (*gate)(unsigned limit);    /* may a further group begin, tasks 0 .. limit - 1 having ended; NULL: no */
    SCHED* e;                       /* the tick's view of the engine; NULL in the searches' handing-out     */
    unsigned n, helpers;            /* the tasks; the helpers that may be woken                             */
    volatile LONG next, limit, done, pending;
} s_run;
SIM_VAR(s_run, SIM_TRANSIENT);

static void task_walk(unsigned i, TS_PF* pf)
{
    (void)pf;
    walk_part(s_run.e, i);
}

/* THE CLASSES' TASKS of a tick: task k is everything of class s_due.cls[k], in the order one thread would make
   it -- THE WRITES (its watermark and sweep; then, for each of its jobs whose searcher is an obstacle, the
   searcher's rectangle recomputed in the map with the unit in it, as the cleanup 0x440BE0 leaves it: the search
   reads the patch there), then its copy, then its jobs' queries. The last task is the explored map's copy.
   THE BOUND that lets two classes' tasks run at once: a task writes its own class's record and map, its jobs
   and its copy, and nothing else. The engine's 0x440830 stores into the map of the class it is given and nowhere
   else (its one store, 0x440901); it calls 0x47E1F0 alone, which calls the rectangle 0x47DFC0 alone, which calls
   nothing, and neither stores outside its frame; of the classes they read the given one's record
   (0x47E041..0x47E18F, the watermark at 0x47E13A) -- the rest is the cells, the units, the features and the
   heights, which nothing writes while the scheduler's thread is inside the handing-out. */
static void task_class(unsigned k, TS_PF* pf)
{
    const SCHED* e = s_run.e;
    unsigned i;
    (void)pf;
    if (k >= s_due.ncls) {
        frz_copy(e, 0);
        return;
    }
    class_tick(e, (char*)(size_t)(VA_CLASSES + s_due.cls[k] * CLASS_STRIDE));
    for (i = 0; i < s_due.n; i++) {
        const JOB* j = &s_job[i];
        if (j->whole && j->ci == s_due.cls[k] &&
            *(const unsigned*)(j->mover + MOVER_STAMP) < *(const unsigned*)(j->cls + 0x1C))
            ((PFN_Remark)(size_t)VA_REMARK)(j->cls, *(const unsigned*)(j->unit + UNIT_CELL),
                                            *(const unsigned*)(j->unit + UNIT_FOOT));
    }
    if (e->late) frz_copy(e, 1 + k);
    for (i = 0; i < s_due.n; i++)
        if (s_job[i].whole && s_job[i].ci == s_due.cls[k]) job_ready(e, &s_job[i]);
}

static void task_search(unsigned i, TS_PF* pf)
{
    JOB* j = &s_job[i];
    if (j->ready) ts_pf_search(pf, &j->q, &j->r);
}

/* the searches' gate: the pops cap, over the groups that have ended */
static int gate_pops(unsigned limit)
{
    unsigned i, pops = 0;
    for (i = 0; i < limit; i++) pops += (unsigned)s_job[i].r.pops;
    return pops < PF_CAP_POPS;
}

/* `woken` more helpers, each owing an answer; none when the counts cannot be released (all or none) */
static void run_wake(unsigned woken)
{
    if (!woken) return;
    InterlockedExchangeAdd(&s_run.pending, (LONG)woken);
    if (!ReleaseSemaphore(s_go, (LONG)woken, NULL)) InterlockedExchangeAdd(&s_run.pending, -(LONG)woken);
}

/* Takes tasks while one may be claimed. `helper`: the taker is one, so one fewer is left to wake. */
static void run_take(TS_PF* pf, int helper)
{
    for (;;) {
        const LONG i = s_run.next, limit = s_run.limit;
        if (i >= limit) return;
        if (InterlockedCompareExchange(&s_run.next, i + 1, i) != i) continue;
        s_run.task((unsigned)i, pf);
        /* the limit this task was claimed under stands until every task under it has ended, this one included */
        if (InterlockedIncrement(&s_run.done) == limit && (unsigned)limit < s_run.n && s_run.gate &&
            s_run.gate((unsigned)limit)) {
            const unsigned more = s_run.n - (unsigned)limit < PF_BATCH ? s_run.n - (unsigned)limit : PF_BATCH;
            const unsigned idle = s_run.helpers - (helper && s_run.helpers ? 1u : 0u);
            InterlockedExchange(&s_run.limit, limit + (LONG)more);
            run_wake(more - 1 < idle ? more - 1 : idle);
            SetEvent(s_done);       /* the scheduler's thread, if it waits in run_end, takes of the group too */
        }
    }
}

static DWORD WINAPI helper_main(LPVOID scratch)
{
    /* a wait that fails has taken no count, so it owes no answer; s_go is never closed, so none does */
    while (WaitForSingleObject(s_go, INFINITE) == WAIT_OBJECT_0) {
        run_take(&s_pf[(size_t)scratch], 1);
        if (!InterlockedDecrement(&s_run.pending)) SetEvent(s_done);
    }
    return 0;
}

/* Tasks 0 .. n - 1, the first `limit` of them claimable now and the rest as the gate allows; `woken` helpers at
   first, of the `helpers` the handing-out may wake. */
static void run_begin(void (*task)(unsigned, TS_PF*), int (*gate)(unsigned), SCHED* e, unsigned n,
                      unsigned limit, unsigned helpers, unsigned woken)
{
    s_run.task = task;
    s_run.gate = gate;
    s_run.e = e;
    s_run.n = n;
    s_run.helpers = helpers;
    s_run.next = s_run.done = s_run.pending = 0;
    s_run.limit = (LONG)limit;
    run_wake(woken);
}

/* Ends the handing-out; returns the tasks that ended, 0 .. that - 1. */
static unsigned run_end(void)
{
    unsigned done;
    for (;;) {
        InterlockedIncrement(&s_run.pending);
        run_take(&s_pf[0], 0);
        if (!InterlockedDecrement(&s_run.pending)) break;
        WaitForSingleObject(s_done, INFINITE);
    }
    done = (unsigned)s_run.done;
    memset(&s_run, 0, sizeof s_run);
    return done;
}

/* The job's unit as the walk found it: its slot still holds that unit, mover, class, ground course and goal, and
   the course's stamp is the search's. Read on the scheduler's thread from the unit array, as the walk reads it.
   Inside the scheduler nothing can have changed them (sched_tick's ordering); at the join this is THE BOUND that
   makes a result's writes land on the object it was found for, or nowhere. A set goal cannot come between: it
   joins first (before_set_goal). */
static int job_stands(const JOB* j)
{
    const char* ta = ta_main();
    const char* first = ta ? *(const char* const*)(ta + OFF_UNIT_BEGIN) : NULL;
    const char* last = ta ? *(const char* const*)(ta + OFF_UNIT_END) : NULL;
    const char* unit;
    const char* mover;
    const char* course;
    if (!first || last < first || (size_t)(last - first) / UNIT_STRIDE < j->slot) return 0;
    unit = first + (size_t)j->slot * UNIT_STRIDE;
    if (unit != j->unit || !*(const unsigned short*)(unit + UNIT_TYPE)) return 0;
    mover = *(const char* const*)unit;
    if (!mover || mover != j->mover || *(const char* const*)(mover + MOVER_CLASS) != j->cls) return 0;
    course = *(const char* const*)mover;
    return course && course == j->course && *(const unsigned*)course == VT_GROUND_COURSE &&
           *(const unsigned*)(course + OBJ_LAST) == j->stamp && *(const char* const*)(course + 4) == j->goal;
}

/* THE RESULT of one job whose unit still stands as searched (job_stands), through the engine's own set path
   0x44F080: a path's corners, or none -- and with none it ORs 0x40 into the order when the unit is not in its goal
   (0x44F089..0x44F0B9), which is why that bit is left to it. The search's other flags go through the goal's
   0x44CED0, as the search start ORs them. */
static void job_result(JOB* j)
{
    const unsigned flags = j->r.goalflags & ~0x40u;
    const int found = j->r.kind == TS_PF_FOUND && j->r.n >= 1 && j->r.n <= TS_PF_WP;
    if (!job_stands(j)) {
        if (s_v.dropped++ < 4)
            tagpu_logf("pathsched: slot %u no longer holds the unit searched for: its result is not applied", j->slot);
        return;
    }
    if (flags && j->goal) ((PFN_GoalFlag)(size_t)VA_GOAL_FLAG)(j->goal, flags);
    if (found)
        ((PFN_SetPath)(size_t)VA_SET_PATH)(j->course, j->r.pts, j->r.n);
    else
        ((PFN_SetPath)(size_t)VA_SET_PATH)(j->course, NULL, 0);
    s_found[j->slot] = found ? j->stamp : 0;
    if (j->patched) s_v.patched++;
    if (j->other && s_v.other_goal++ < 4)
        tagpu_logf("pathsched: a goal of a kind the search has not (vtable %#x): no path", *(const unsigned*)j->goal);
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
    LARGE_INTEGER hz;
    const double ms = QueryPerformanceFrequency(&hz) && hz.QuadPart ? 1000.0 / (double)hz.QuadPart : 0.0;
    if (s_log) {
    tagpu_logf("pathsched: %u ticks: %.1f searches a tick (most %u; cap %d), %.0f pops (most %u; cap %d): %u found, "
               "%u no search, %u no path, %u off the grid, %u not made, %u read a patch; ready a tick: %.1f on a new "
               "goal, %.1f at a path's end, %.1f for a retry; left for the next tick %.1f (most %u); held by the retry "
               "%.1f a tick; ticks at the base weight %u, at 3 times %u, at 6 times %u; searched while the tick went "
               "on: %u ticks, %u of them joined by a set goal, %u searches not ended at the join, %u results not applied; "
               "the join waited %.1f ms in all, %.2f at most, 0.1 ms or more in %u ticks at the base weight, %u at 3 "
               "times, %u at 6 times",
               s_v.ticks, s_v.begun / t, s_v.begun_max, PF_CAP_SEARCHES, s_v.pops / t, s_v.pops_max, PF_CAP_POPS,
               s_v.nfound, s_v.nosearch, s_v.nopath, s_v.offgrid, s_v.failed, s_v.patched, s_v.ready_order / t,
               s_v.ready_end / t, s_v.ready_b / t, s_v.left / t, s_v.left_max, s_v.held_retry / t, s_v.weight[0],
               s_v.weight[1], s_v.weight[2], s_v.late, s_v.joined_goal, s_v.unended, s_v.dropped,
               ms * (double)s_v.join_sum, ms * (double)s_v.join_max, s_v.join_waits[0],
               s_v.join_waits[1], s_v.join_waits[2]);
    tagpu_logf("pathsched: wait from ready to the search, %u searches: mean %.1f ticks, most %u; 0: %u, 1-2: %u, "
               "3-9: %u, 10-29: %u, 30-89: %u, 90-299: %u, 300+: %u; set goal: %u calls, inside the engine's 10 "
               "ticks %u left to its rule and %u opened", s_v.waited, s_v.waited ? (double)s_v.wait_sum / s_v.waited : 0.0,
               s_v.wait_max, s_v.wait_hist[0], s_v.wait_hist[1], s_v.wait_hist[2], s_v.wait_hist[3], s_v.wait_hist[4],
               s_v.wait_hist[5], s_v.wait_hist[6], s_v.goals, s_v.goals_kept, s_v.goals_opened);
    }
    memset(&s_v.calls, 0, (size_t)((char*)(&s_v + 1) - (char*)&s_v.calls));
}

/* THE JOIN: the searches handed out are ended and the due results applied, in the order the walk chose them, on
   the scheduler's thread; a job whose group did not begin is put back as the walk found it -- its course's stamp
   -- and the cursors move here, to the last unit searched of each kind. Nothing is due on return, and a set goal
   reached from inside a result's own writes finds nothing due. `apply` 0 ends the searches and drops everything:
   a level that has gone. */
static void due_join(int apply)
{
    const unsigned n = s_due.n, wgt = s_due.wgt;
    unsigned i, pops = 0, searched = s_due.searched;
    if (!n) return;
    if (!s_due.ended) {
        /* `done` first: it is never above `limit`, which only grows */
        const unsigned done = (unsigned)s_run.done;
        LARGE_INTEGER t0, t1, hz;
        s_v.unended += (unsigned)s_run.limit - done;
        if (s_log) QueryPerformanceCounter(&t0);
        searched = run_end();
        if (s_log && QueryPerformanceCounter(&t1) && QueryPerformanceFrequency(&hz)) {
            const unsigned long long d = (unsigned long long)(t1.QuadPart - t0.QuadPart);
            s_v.join_sum += d;
            if (d > s_v.join_max) s_v.join_max = d;
            if (d * 10000u >= (unsigned long long)hz.QuadPart) s_v.join_waits[wgt]++;
        }
    }
    memset(&s_due, 0, sizeof s_due);
    if (!apply) return;
    for (i = 0; i < searched; i++) {
        s_cursor[s_job[i].kind] = s_job[i].slot;
        job_result(&s_job[i]);
        pops += (unsigned)s_job[i].r.pops;
    }
    for (; i < n; i++)
        if (job_stands(&s_job[i])) *(unsigned*)(s_job[i].course + OBJ_LAST) = s_job[i].stamp0;
    s_v.left += n - searched;
    if (searched > s_v.begun_max) s_v.begun_max = searched;
    if (pops > s_v.pops_max) s_v.pops_max = pops;
}

/* In the engine's scheduler's place, once a tick (0x464F92). THE ORDERING its steps rest on: everything from the
   walk to the searches' handing-out runs inside this one call on the thread that runs the simulation, and nothing
   it calls makes, moves or frees a unit, a mover, a course or a goal -- 0x440830 writes a class's map -- so a
   pointer the walk read is the same object in every later step of the call. The results are the join's: this
   call's own last step, or the step's and a set goal's (due_join, job_stands). */
static void __fastcall sched_tick(void* pf_unused)
{
    unsigned short ready[2][PF_SLOTS], still_slot[PF_SLOTS];
    unsigned still_stamp[PF_SLOTS];
    unsigned n[2], at[2], taken[2] = { 0, 0 }, nj = 0, kind, i, wi, helpers;
    SCHED e;
    (void)pf_unused;
    due_join(1);                    /* a tick the step's join did not end, were there one, ends here */
    if (!sched_open(&e)) return;
    tagpu_prof_enter(TAGPU_PROF_GAME, TAGPU_PG_PATHFIND);
    /* Helpers are woken only IN PLAY, when the scheduler's thread is the game thread: the load's word
       main + 0x38D75 is 0. THE ORDERING: the loader goes on to its call of the players phase only once it has read
       bit 3 of that word (0x4975D6..0x4975F1), which the game thread sets (0x498576) and nothing clears until the
       game thread zeroes the word at its entry into play (0x49847E, 0x498480), after it has read the loader's
       last store (0x497C62, 0x498342). So in the loader's call the word is not 0, and where it is 0 no loader
       lives. In that call the game thread is not stopped -- it runs its loading frame -- and the loader's entry
       0x497C70 catches a fault and ends its thread, frame and all; there every task runs on the loader's own
       thread, as the engine's searches did. And every helper has a scratch, or none is woken. */
    helpers = e.play && s_helpers && s_pf[s_helpers].pool ? s_helpers : 0;
    e.ready = ready;
    e.still_slot = still_slot;
    e.still_stamp = still_stamp;
    /* each handing-out but the searches' is ended where it is begun; this thread takes tasks too, so one helper
       fewer than tasks */
    run_begin(task_walk, NULL, &e, PF_WALK_PARTS, PF_WALK_PARTS, helpers,
              helpers < PF_WALK_PARTS - 1 ? helpers : PF_WALK_PARTS - 1);
    run_end();
    walk_pack(&e);
    n[0] = e.nready[0];
    n[1] = e.nready[1];
    /* THE WEIGHT, by the tick's load: the engine's three (0x40EBBF..0x40EC06) */
    i = n[0] + n[1] > PF_CAP_SEARCHES ? 2 : n[0] + n[1] > PF_QUIET;
    e.wgt *= i == 2 ? 6 : i == 1 ? 3 : 1;
    s_v.weight[i]++;
    wi = i;
    for (kind = 0; kind < 2; kind++)
        for (at[kind] = 0; at[kind] < n[kind] && ready[kind][at[kind]] <= s_cursor[kind]; at[kind]++)
            ;
    while (nj < PF_CAP_SEARCHES && taken[0] + taken[1] < n[0] + n[1]) {
        unsigned slot;
        kind = taken[0] < n[0] ? 0 : 1;
        slot = ready[kind][(at[kind] + taken[kind]++) % n[kind]];
        job_pick(&e, &s_job[nj], slot);
        s_job[nj++].kind = kind;
    }
    if (nj) {
        unsigned tasks, seen = 0;
        s_due.n = nj;
        s_due.wgt = wi;
        for (i = 0; i < nj; i++)
            if (s_job[i].whole && !(seen >> s_job[i].ci & 1)) {
                seen |= 1u << s_job[i].ci;
                s_due.slot[s_job[i].ci] = (unsigned char)s_due.ncls;
                s_due.cls[s_due.ncls++] = (unsigned char)s_job[i].ci;
            }
        /* beside the tick: a helper to search, and a copy for every class searched */
        e.late = helpers && s_due.ncls && frz_room(e.w, e.h, s_due.ncls);
        tasks = s_due.ncls + (e.late ? 1 : 0);
        if (tasks) {
            run_begin(task_class, NULL, &e, tasks, tasks, helpers, helpers < tasks - 1 ? helpers : tasks - 1);
            run_end();
        }
        /* the searches, their first group: ended at the join, which then finds the tasks ended or takes them */
        kind = nj < PF_BATCH ? nj : PF_BATCH;
        i = e.late ? kind : kind - 1;
        run_begin(task_search, gate_pops, NULL, nj, kind, helpers, helpers < i ? helpers : i);
        if (e.late) s_v.late++;
        else {
            s_due.searched = run_end();
            s_due.ended = 1;
        }
        /* IN PLAY THE RESULTS WAIT FOR THE JOIN, wherever the searches were made: where a result lands in the
           tick is the same on every machine. The loader's call ends its own. */
        s_due.held = e.play;
        if (!e.play) due_join(1);
    }
    s_v.ticks++;
    s_v.ready_a += n[0];
    s_v.ready_b += n[1];
    i = n[0] + n[1] - taken[0] - taken[1];
    s_v.left += i;
    if (i > s_v.left_max) s_v.left_max = i;
    if (s_v.ticks >= 150) sched_line();
    tagpu_prof_leave(TAGPU_PROF_GAME);
}

/* THE STEP'S JOIN, in place of the step's call of 0x466580 (0x4955E5), which it then makes: every tick's searches
   are ended and applied by here. The step runs from the players phase's return to this call in a straight line
   (0x495564..0x4955E5), so a tick whose scheduler handed searches out reaches it. */
static void __cdecl tick_join(void)
{
    if (s_due.held) {
        tagpu_prof_enter(TAGPU_PROF_GAME, TAGPU_PG_PATHFIND);
        due_join(1);
        tagpu_prof_leave(TAGPU_PROF_GAME);
    }
    ((void (__cdecl*)(void))(size_t)VA_STEP_COUNT)();
}

/* The ground course's set goal 0x44F2A0, at its entry, with a goal. The function's own rule zeroes the stamp when
   the course's last search began 10 ticks ago or more (0x44F420..0x44F436), and the walk then serves the unit at
   once. Here the same is done inside those 10 ticks for a unit whose last search found a path: A UNIT ON ITS WAY
   THAT IS SENT ELSEWHERE. The function only ever zeroes the stamp, so the 0 stands. A unit whose last search
   found nothing keeps the engine's rule: an order handler answers such a search with a new goal within a tick or
   two, for as long as the unit stays where it is (MEASURED, modules/pathfinder.md), and each would be a search.
   THE BOUND: the course's unit against the unit array, as a slot of the table.
   FIRST IT JOINS: a set goal replaces the goal and reads the stamp a result writes beside, so the tick's results
   are applied before it runs, as they are when the searches end inside the scheduler. */
static int __cdecl before_set_goal(void* esp)
{
    char* course = (char*)(size_t)*(const unsigned*)((const unsigned char*)esp - 0x0C);
    const char* ta = ta_main();
    const char* first;
    const char* unit;
    unsigned stamp;
    size_t slot;
    if (s_mode != MODE_OURS) return 0;
    if (s_due.held) {
        s_v.joined_goal++;
        due_join(1);
    }
    if (!ta || tagpu_puppet_on() || !((const char* const*)esp)[1] || !course ||
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
    due_join(0);
    memset(s_found, 0, sizeof s_found);
    memset(s_cursor, 0, sizeof s_cursor);
    memset(&s_v, 0, sizeof s_v);
}
/* the oracle's observers, below */
static int __cdecl before_reset(void* esp);
static int __cdecl before_extract(void* esp);
static int __cdecl before_cleanup(void* esp);
static int __cdecl before_scheduler(void* esp);

/* The machine's processor cores, counted once each whatever hardware threads they carry; 0 when the system does
   not say. */
static unsigned cores_count(void)
{
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[1024];
    DWORD bytes = sizeof info;
    unsigned cores = 0, i;
    if (!GetLogicalProcessorInformation(info, &bytes)) return 0;
    for (i = 0; i < bytes / sizeof info[0]; i++)
        if (info[i].Relationship == RelationProcessorCore) cores++;
    return cores;
}

/* THE NUMBER OF THREADS that search: the machine's cores less one, left to the render thread, and at most
   PF_THREADS; the scheduler's own thread is one of them, so the helpers are one fewer. Cores, not hardware
   threads: a search that shares a core waits on its sibling. The number decides how long the searches take and
   nothing of what they find. A helper that cannot be made is one thread fewer. Returns the cores counted. */
static unsigned helpers_start(void)
{
    const unsigned cores = cores_count();
    unsigned want = cores > 1 ? cores - 1 : 1, k;
    if (want > PF_THREADS) want = PF_THREADS;
    if (--want == 0) return cores;
    /* a handing-out releases counts group by group, some before the earlier ones are taken */
    s_go = CreateSemaphoreA(NULL, 0, 0x10000, NULL);
    s_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s_go || !s_done) return cores;
    for (k = 0; k < want; k++) {
        /* helper k searches on s_pf[1 + k]; the helpers are numbered as they are made, with no gap */
        const HANDLE t = CreateThread(NULL, 0, helper_main, (LPVOID)(size_t)(1 + s_helpers), 0, NULL);
        if (!t) continue;
        CloseHandle(t);
        s_helpers++;
    }
    return cores;
}

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
    unsigned cores;
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
        !tagpu_detour_redirect_ok(VA_JOIN_CALL, VA_STEP_COUNT) ||
        !tagpu_detour_bytes_ok(VA_SET_GOAL, X_SETGOAL, sizeof X_SETGOAL) ||
        !tagpu_detour_bytes_ok(VA_REMARK, X_REMARK, sizeof X_REMARK) ||
        !tagpu_detour_bytes_ok(VA_SET_PATH, X_SETPATH, sizeof X_SETPATH) ||
        !tagpu_detour_bytes_ok(VA_GOAL_FLAG, X_GOALFLAG, sizeof X_GOALFLAG)) {
        tagpu_log("pathsched: NOT installed - a site does not hold the engine's bytes; the engine's scheduler runs");
        return;
    }
        /* the observer does nothing until the mode is ours, and the join nothing but the step's own call while no
       result is held, so a redirect that fails leaves the engine's rules whole */
    if (!tagpu_detour_observe(VA_SET_GOAL, X_SETGOAL, sizeof X_SETGOAL, before_set_goal, NULL) ||
        !tagpu_detour_redirect(VA_JOIN_CALL, VA_STEP_COUNT, (void*)tick_join) ||
        !tagpu_detour_redirect(VA_SCHED_CALL, VA_SCHEDULER, (void*)sched_tick)) {
        tagpu_log("pathsched: NOT installed - a site could not be written; the engine's scheduler runs");
        return;
    }
    s_mode = MODE_OURS;
    cores = helpers_start();
    
    tagpu_logf("pathsched: installed - our scheduler at the players phase's call 0x464F92, set goal 0x44F2A0 watched; "
               "%d searches and %d pops a tick in groups of %d on %u threads (%u cores), joined at the step's call "
               "0x4955E5 and searched %s",
               PF_CAP_SEARCHES, PF_CAP_POPS, PF_BATCH, 1 + s_helpers, cores,
               s_helpers ? "while the tick goes on" : "inside the scheduler");
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
    ts_pf_search(&s_pf[0], &q, &s_v.res);
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
