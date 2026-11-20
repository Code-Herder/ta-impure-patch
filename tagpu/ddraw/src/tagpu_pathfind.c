#include "tagpu_simreg.h"
/* tagpu_pathfind.c -- the pathfinder's search, ours, held to the engine's on every search (tagpu_pathfind.h;
   research/notes/pathfinder-parallel.md; the engine facts: research/notes/engine/units.md, The pathfinder, and
   engine/map.md, The pathing maps).

   THE SEARCH is the shared core's (tasim_path.h, ts_pf_search): one unit's search start, A* and extraction, whole,
   on a map it does not write. What the engine's search start 0x40E630 does to the shared map before it reads it is
   here THE PATCH. A unit that has stood still for 30 ticks is an obstacle in its class's map: its claim stamp is
   below the class's watermark. When the searcher is one, the engine's watermark 0x440AF0 recomputes its rectangle
   with the searcher left out (0x440B33..0x440B45), so that it does not stand in its own way, and the cleanup
   0x440BE0 puts it back. The patch is those entries, recomputed with the engine's rule 0x47E1F0 over the engine's
   own cells with that one unit left out, and read by the search in place of the map's; nothing shared is written.
   A searcher that is no obstacle has no patch: the map already holds its rectangle without it, and the engine reads
   it as it stands, an entry no re-mark has refreshed included (a unit made inside a plant's yard leaves by one).

   THE VERIFY LEVER `tagpu_pathverify.on` runs both: at the engine's grid reset 0x40D900 inside a search start --
   after the watermark, so the map holds what the engine's search is about to read -- it computes the patch, counts
   the entries that differ from the map's (the engine has just recomputed the same ones: any difference is a fault
   in our rule), runs the search on a scratch of its own and keeps the result; at the
   search's end, the class's cleanup 0x440BE0, it compares that with what the engine wrote: the exit, the path's
   points and count, the goal cell, the pool's use, the bug walk's answer and the flags ORed into the order. It
   writes no engine memory and decides nothing: SIM_PUPPET_INDEPENDENT. With no lever nothing is installed.

   EVERY HOOK RUNS ON THE GAME THREAD inside the scheduler 0x40EB70 (the players phase, 0x464F92), which owns what
   they read. A value taken from engine memory is data until bounded: the class record against the 32 records, the
   grid's size against the class's and the pathfinder's, a cell's unit slot against the unit array's end, a feature
   definition against the count, the patch's rectangle against PF_PATCH_MAX. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_engine.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "tagpu_pathfind.h"
#include "tasim_path.h"

SIM_FEATURE(path_verify, SIM_PUPPET_INDEPENDENT, "battle");

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

static int s_on;
SIM_VAR(s_on, SIM_INSTALL);

static TS_PF s_pf;
SIM_VAR(s_pf, SIM_PRIVATE);
static unsigned char* s_mem;
static unsigned s_mem_bytes;
SIM_VAR(s_mem_bytes, SIM_PRIVATE);
SIM_HEAP(s_mem, SIM_PRIVATE, SIM_ID_NONE, 1, s_mem_bytes, NULL);

/* the oracle's state and counters: nothing the simulation reads */
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
    if (!s_on || ((const unsigned*)esp)[0] != RA_RESET_SEARCH) return 0;
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
    if (!s_on || !s_v.open || !pf || ((const char* const*)esp)[1] != s_v.unit) return 0;
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
    if (!s_on || !ta) return 0;
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

void tagpu_pathfind_init(void)
{
    static const unsigned char X_SCHED[5]   = { 0x83, 0xEC, 0x18, 0x53, 0x55 };
    static const unsigned char X_RESET[6]   = { 0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1 };
    static const unsigned char X_EXTRACT[6] = { 0x81, 0xEC, 0x08, 0x02, 0x00, 0x00 };
    static const unsigned char X_CLEANUP[5] = { 0x8B, 0x44, 0x24, 0x04, 0x56 };
    if (!tagpu_sim_lever("tagpu_pathverify.on")) return;
    if (!tagpu_detour_bytes_ok(VA_SCHEDULER, X_SCHED, sizeof X_SCHED) ||
        !tagpu_detour_bytes_ok(VA_GRID_RESET, X_RESET, sizeof X_RESET) ||
        !tagpu_detour_bytes_ok(VA_EXTRACT, X_EXTRACT, sizeof X_EXTRACT) ||
        !tagpu_detour_bytes_ok(VA_CLEANUP, X_CLEANUP, sizeof X_CLEANUP)) {
        tagpu_log("pathverify: NOT armed - a site does not hold the engine's bytes");
        return;
    }
    s_on = tagpu_detour_observe(VA_SCHEDULER, X_SCHED, sizeof X_SCHED, before_scheduler, NULL) &&
           tagpu_detour_observe(VA_GRID_RESET, X_RESET, sizeof X_RESET, before_reset, NULL) &&
           tagpu_detour_observe(VA_EXTRACT, X_EXTRACT, sizeof X_EXTRACT, before_extract, NULL) &&
           tagpu_detour_observe(VA_CLEANUP, X_CLEANUP, sizeof X_CLEANUP, before_cleanup, NULL);
    tagpu_log(s_on ? "pathverify: ARMED - our search runs beside the engine's on every search (0x40EB70, 0x40D900, "
                     "0x40E050, 0x440BE0)"
                   : "pathverify: NOT armed - an observer could not be installed");
}
