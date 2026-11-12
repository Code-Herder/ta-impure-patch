/* tagpu_impai.c -- ImpAI's seam in the engine and its seat table (tagpu_impai.h). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_impai.h"
#include "tagpu_simreg.h"

/* ImpAI marks no seat on a puppet: the wrapper refuses a puppet seat, and on a puppet level the
   level init and the level end only clear the table, so between the two the recording is the
   only writer of the seat table. */
SIM_FEATURE(impai_observe, SIM_PUPPET_OWNER, "impai-observe impai-observe-perm stock-impai-observe");

#include "dd.h"                 /* g_ddraw.gui_thread_id: the game thread */
#include "tagpu_engine.h"
#include "tagpu_puppet.h"
#include "tagpu_detour.h"
#include "tagpu_reclaim.h"
#include "tagpu_log.h"
#include "tagpu_replay.h"
#include "tagpu_settings.h"
#include "tagpu_packet.h"           /* TAGPU_PK_DESIGN_SLOTS: the most unit slots a world has */
#include "tagpu_worldq.h"
#include "tagpu_limits.h"
#include "impai.h"

#define VA_AI_CALL   0x00465031u   /* the players phase's `call 0x408C40`, E8 0A 3C FA FF */
#define VA_AI_TICK   0x00408C40u   /* the stock AI tick, thiscall, no stack argument      */
#define PL_ACTIVE    0x00          /* u32: nonzero for a seated record                   */
#define PL_CONTROL   0x73          /* u8: 1 human, 2 AI, 3 remote or puppet              */
#define CTRL_AI      2
#define CTRL_REMOTE  3

#define IMPAI_ON     0x01u

/* Which records ImpAI plays, by record index. No level sees another's marks: the level end
   clears the table in the teardown, and the level init clears it again on every level, live or
   puppet, on the loader thread before anything of the level reads it (the stubs' ordering,
   below), which also covers a teardown tagpu_reclaim.off leaves unwrapped. Then the level init
   marks a live level's seats; on a puppet level, after that clear, the recording is the only
   writer, its first restore running on the game thread at the level's first step (the loader's
   done bit orders the two: impai_seat_of, THE THREAD). A recording that carries no row of this
   table leaves it clear. */
static uint8_t s_seat[PLAYER_COUNT];
SIM_ROWS(s_seat, SIM_STATE, SIM_ID_PLAYER, NULL);

#define IMPAI_HSITES 18        /* the handicap sites: the seventeen Difficulty tests and 0x406FC3 */
enum { HIT_SCALED, HIT_EXEMPT };

/* What went in at attach. `avail`: every site ImpAI needs is in, so a level may mark seats;
   `why` says what kept it out. */
static struct {
    uint8_t in;            /* 1 once 0x465031 reaches impai_ai_tick            */
    uint8_t avail;
    uint8_t check;         /* tagpu_impai_check.on: the self-checks run        */
    char    why[80];
} s_install;
SIM_VAR(s_install, SIM_INSTALL);

/* The level's counts, for the level-end lines and handicap_check.py's peek: the wrapper's turns
   per seat, and per handicap site how often a type-2 record took the scaled continuation (an OTA
   AI seat) and the unscaled branch for an ImpAI mark. The stubs increment `hits` with a plain
   `inc` on whichever thread reaches the site; the threads are ordered as the table's writes are
   (the stubs' ordering), but nothing makes the counts exact against a network load's concurrent
   removal: they are diagnostics the simulation never reads. */
static struct {
    unsigned calls[PLAYER_COUNT];
    unsigned hits[IMPAI_HSITES][2];         /* [HIT_SCALED], [HIT_EXEMPT] */
} s_diag;
SIM_VAR(s_diag, SIM_PRIVATE);

typedef void (__thiscall* PFN_AiTick)(void* obj);

/* The seat whose turn this is, or -1. Every test must hold:
   - THE THREAD. The players phase runs on the game thread every step and once on the loader
     thread, at 0x497C10 at the end of a level load (DISASSEMBLED). Only the game thread observes.
     The seat table the level init writes on the loader thread is ordered before every game-thread
     call by the engine's own handshake: the init runs inside the loader's PLAYERS call 0x4919C8,
     before the loader's last store sets bit 1 of main+0x38D75 (0x497C62), and the game thread
     runs no step until it has read that bit set (0x498342; tagpu_replay.c, THE VIEWER'S
     BUFFERS). The bit itself is no per-call gate: 0x498480 clears the word after the level's
     first in-game frame (tagpu_engine.h, OFF_LOADFLAGS).
   - THE BOUND. The AI object's +0 is its record (the constructor 0x408CB0 stores it at
     0x408CBB, DISASSEMBLED); that value is data until it is one of main's ten records, which the
     offset's range and exact division by the stride establish without dereferencing it.
   - THE SEAT. The record is seated with controller 2 -- the test 0x408C40 makes before it plans
     (0x408C45..0x408C50) -- marked in the seat table, and not a puppet seat. */
static int impai_seat_of(const void* obj)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const char* rec;
    uintptr_t off;
    unsigned i;
    if (GetCurrentThreadId() != g_ddraw.gui_thread_id || !ta) return -1;
    rec = *(const char* const*)obj;
    off = (uintptr_t)rec - (uintptr_t)(ta + OFF_PLAYERS);
    if (off >= (uintptr_t)PLAYER_COUNT * PLAYER_STRIDE || off % PLAYER_STRIDE) return -1;
    i = (unsigned)(off / PLAYER_STRIDE);
    if (!*(const uint32_t*)(rec + PL_ACTIVE) || (uint8_t)rec[PL_CONTROL] != CTRL_AI) return -1;
    if (!(s_seat[i] & IMPAI_ON) || tagpu_puppet_seat(i)) return -1;
    return (int)i;
}

/* ---- WHAT A SEAT KNOWS: impai.h's rows, kept by impai_know.c over the engine's WORLDQ ---- */

/* Each seat's knowledge of every unit slot (impai.h, THE INFORMATION RULE): a row changes only through what
   the seat observes, so a death or a slot reuse the seat cannot see changes nothing it knows. A ghost row keeps
   the last block and type the seat knew until the seat sees that block again. Keyed by slot, at most the
   unit array's design size (tagpu_worldq_open bounds nslots by it). An ID field holds WQ_NONE or 0xFF when
   the seat never identified the unit; the recorder images an SIMF_ID field as the value it is. */
static IMPAI_ROW s_known[TAGPU_PK_DESIGN_SLOTS];
static const SimField s_known_layout[] = {
    { offsetof(IMPAI_ROW, seat[0].type),  2, WQ_SEATS, sizeof(IMPAI_SEEN), SIMF_ID, SIM_ID_UNIT_TYPE },
    { offsetof(IMPAI_ROW, seat[0].owner), 1, WQ_SEATS, sizeof(IMPAI_SEEN), SIMF_ID, SIM_ID_PLAYER },
    SIM_FIELD_END
};
SIM_ROWS(s_known, SIM_STATE, SIM_ID_UNIT_SLOT, s_known_layout);

/* each seat's slice cursor and radar pass in progress */
static IMPAI_SEAT s_seatst[PLAYER_COUNT];
SIM_ROWS(s_seatst, SIM_STATE, SIM_ID_PLAYER, NULL);

/* each seat's radar pass, one nibble a slot (worldq.h, WQ_RB_*): the pass's answer between passes, its
   partial state during one that spans ticks */
static uint8_t s_radar_bits[PLAYER_COUNT][WQ_RADAR_BYTES(TAGPU_PK_DESIGN_SLOTS)];
SIM_ROWS(s_radar_bits, SIM_STATE, SIM_ID_PLAYER, NULL);

/* each seat's metal patches (impai.h): the published set, then the scan in progress */
static SNAP_PATCH s_cluster[PLAYER_COUNT][IMPAI_CLUSTERS];
SIM_ROWS(s_cluster, SIM_STATE, SIM_ID_PLAYER, NULL);

/* THE BUDGETS of one seat's tick, by count (impai.h): the loops are the budgets, so a tick past them is a
   bug in the code, and the level-end `work` lines show each tick's largest against its bound. The test
   lever tagpu_impai_budget.on raises the slice to every slot of the design point and the radar pass's
   visits to 10^6, so the cost line measures each loop's cost per unit of work (research/notes/impai/
   player.md, the cost). */
#define OBS_SLICE     2048u
#define RADAR_VISITS  32768u
#define CLU_ROWS      16u
#define CLU_FLOOD     (2u * SNAP_PATCH_CAP)
static IMPAI_BUDGET s_budget = { OBS_SLICE, RADAR_VISITS, CLU_ROWS, CLU_FLOOD };
SIM_VAR(s_budget, SIM_INSTALL);

/* THE REACH LABELS of every movement class the level has (impai.h, impai_reach_label), class after class,
   bw x bh 32-px blocks each, from the engine's own pathing maps at the level init. Content: a function of
   the map and its features at PLAYERS, rebuilt on every level, live or puppet. */
static uint16_t* s_reach;
static unsigned  s_reach_n;
SIM_HEAP(s_reach, SIM_CONTENT, SIM_ID_NONE, sizeof(uint16_t), s_reach_n, NULL);
SIM_VAR(s_reach_n, SIM_CONTENT);
/* where each class's labels start in s_reach (present: bit c), and the block grid */
static struct {
    uint32_t at[WQ_MCLASSES];
    uint32_t present;
    uint16_t bw, bh;
} s_reach_ix;
SIM_VAR(s_reach_ix, SIM_CONTENT);

/* The level's work and the self-checks' counts, for the level-end lines and tools/impai_check.py. */
static struct {
    unsigned ticks[PLAYER_COUNT], refused[PLAYER_COUNT];
    unsigned slots_max[PLAYER_COUNT], visits_max[PLAYER_COUNT], nslots_max;
    unsigned cells_max[PLAYER_COUNT], flood_max[PLAYER_COUNT], map_w;
    unsigned scans[PLAYER_COUNT];             /* patch scan passes completed and published         */
    /* THE COST, MEASURED: QueryPerformanceCounter around a turn's observation (the knowledge tick and its
       view, not the self-checks nor the stock tick), each seat's largest, its sum and a log2 histogram of
       microseconds, bucket k holding [2^k, 2^(k+1)) and bucket 0 [0, 2). A measurement for the cost line,
       never a verdict: the gate judges the counts. */
    unsigned cost_max[PLAYER_COUNT], cost_hist[PLAYER_COUNT][24];
    double   cost_sum[PLAYER_COUNT];
    unsigned los_eval[PLAYER_COUNT], los_miss[PLAYER_COUNT];
    unsigned local_eval, local_miss;
    unsigned radar_passes, radar_eval, radar_miss;
    unsigned char los_said, radar_said;
} s_obs;
SIM_VAR(s_obs, SIM_PRIVATE);

typedef int (__stdcall* PFN_UnitInLos)(const void* player, const void* unit);
#define VA_UNIT_IN_LOS 0x00465AC0u   /* (player, unit), `ret 8`: 1 when the player sees the unit; writes nothing */

/* THE LINE-OF-SIGHT CHECK, under tagpu_impai_check.on: ImpAI's rule (tagpu_worldq_unit_los) against the
   engine's 0x465AC0 for every live unit of another owner that is not under the sea, at one instant on one
   thread, nothing written in between. Under True line of sight 0x465AC0 reads the record it is given, so the
   seat's own record is compared. In the other modes it reads the viewed player's mapped bit whatever record
   it is given (tagpu_worldq.c, sight_lit), so the rule is compared for the local record main[0x2A43], where
   the two read the same bit. A puppet seat is no seat here: the open refuses it. */
static void check_los(const WQ_ENGINE* c, const WORLDQ* q, int seat)
{
    WQ_ENGINE lc;
    WORLDQ lq;
    const WQ_ENGINE* ec = c;
    const WORLDQ* eq = q;
    const int truelos = (*(const unsigned char*)(c->ta + OFF_LOSTYPE) & 2) != 0;
    const unsigned local = *(const unsigned char*)(c->ta + OFF_LOCALPLAYER);
    unsigned slot, *eval, *miss;
    if (truelos) {
        eval = &s_obs.los_eval[seat];
        miss = &s_obs.los_miss[seat];
    } else {
        if (local >= PLAYER_COUNT || !tagpu_worldq_open(&lc, &lq, (int)local, WQ_SITE_ENGINE, NULL)) return;
        ec = &lc;
        eq = &lq;
        eval = &s_obs.local_eval;
        miss = &s_obs.local_miss;
    }
    for (slot = 1; slot < ec->nslots; slot++) {
        const char* u = ec->units + slot * UNIT_STRIDE;
        int mine, theirs, under;
        if (*(const unsigned char*)(u + U_OWNER) == (unsigned)eq->seat) continue;
        if ((mine = tagpu_worldq_unit_los(eq, (int)slot, &under)) < 0 || under) continue;
        theirs = ((PFN_UnitInLos)(uintptr_t)VA_UNIT_IN_LOS)(ec->rec, u) != 0;
        (*eval)++;
        if (mine != theirs) {
            (*miss)++;
            if (!s_obs.los_said) {
                s_obs.los_said = 1;
                tagpu_logf("impai: check los mismatch seat=%d slot=%u ours=%d engine=%d pos=%d,%d,%d gametime=%d",
                           eq->seat, slot, mine, theirs, *(const int*)(u + U_XFIX), *(const int*)(u + U_ZFIX),
                           *(const int*)(u + U_YFIX), *(const int*)(ec->ta + OFF_GAMETIME));
            }
        }
    }
}

static LARGE_INTEGER s_qpf;            /* the counter's frequency, taken at attach */
SIM_VAR(s_qpf, SIM_INSTALL);

static void cost_note(int seat, long long ticks)
{
    const double us = s_qpf.QuadPart > 0 ? (double)ticks * 1e6 / (double)s_qpf.QuadPart : 0.0;
    const unsigned u = us < 4e9 ? (unsigned)us : 0xFFFFFFFFu;
    unsigned k = 0, v = u;
    while (v > 1u && k < 23u) { v >>= 1; k++; }
    s_obs.cost_hist[seat][k]++;
    s_obs.cost_sum[seat] += us;
    if (u > s_obs.cost_max[seat]) s_obs.cost_max[seat] = u;
}

/* One ImpAI seat's turn, at its record's place in the players phase, after the step's unit tick, projectiles
   and explosions and before the seat's own sight emits (0x465053): positions are this tick's, the seat's own
   counters as its last emits left them; nothing after the players phase in the step writes a position, the
   alive bit, an owner, a type or cloak (engine/sim.md, the step), so what this turn reads of them holds for
   the rest of the step. */
static void impai_observe(int seat)
{
    const IMPAI_ARRAYS arrays = { s_known, (int)(sizeof s_known / sizeof s_known[0]), s_radar_bits[seat],
                                  (int)sizeof s_radar_bits[seat], s_cluster[seat] };
    WQ_ENGINE c;
    WORLDQ q;
    IMPAI_WORK w;
    SNAP_SCRATCH* scr;
    LARGE_INTEGER t0, t1;
    int rc = -1;
    s_diag.calls[seat]++;
    QueryPerformanceCounter(&t0);
    /* the patch flood's arena, freed before return: nothing in it crosses a tick */
    if ((scr = (SNAP_SCRATCH*)tagpu_sim_scratch(sizeof *scr)) && tagpu_worldq_open(&c, &q, seat, WQ_SITE_ENGINE, NULL))
        rc = impai_know_tick(&q, &s_seatst[seat], &arrays, &s_budget, scr, &w);
    tagpu_sim_scratch_free(scr);
    QueryPerformanceCounter(&t1);
    cost_note(seat, t1.QuadPart - t0.QuadPart);
    if (rc < 0) {
        s_obs.refused[seat]++;
        return;
    }
    s_obs.ticks[seat]++;
    if (w.slots > s_obs.slots_max[seat]) s_obs.slots_max[seat] = w.slots;
    if (w.visits > s_obs.visits_max[seat]) s_obs.visits_max[seat] = w.visits;
    if (w.cells > s_obs.cells_max[seat]) s_obs.cells_max[seat] = w.cells;
    if (w.flood > s_obs.flood_max[seat]) s_obs.flood_max[seat] = w.flood;
    s_obs.scans[seat] += w.scans;
    if (c.nslots > s_obs.nslots_max) s_obs.nslots_max = c.nslots;
    if ((unsigned)c.w > s_obs.map_w) s_obs.map_w = (unsigned)c.w;
    if (s_install.check) check_los(&c, &q, seat);
}

#define VA_RADAR_CALL 0x0046556Du   /* the players phase's `call 0x467440`, E8 CE 1E 00 00 */
#define VA_RADAR_PASS 0x00467440u   /* the radar pass: no argument, plain `ret`            */

/* THE RADAR CHECK, the target of the call at 0x46556D under tagpu_impai_check.on alone: the engine's pass
   runs, then ImpAI's copy (tagpu_worldq.c, wq_radar_step) for the same record, unbudgeted, and every live
   unit's +0x110 bits 8 and 9 are compared with the copy's, and bit 10 where the pass's first loop cleared it
   (WQ_RB_KEPT clear: an own or shared unit's bit 10 is older than the pass). The site is reached only in the
   viewed record's turn (0x465565..0x46556B), so the record is main[0x2A43]. Same contract as 0x467440: no
   argument, a plain `ret`, the callee-saved registers kept. The players phase's run on the loader thread
   (0x497C10) passes through unchecked. */
static void impai_radar_check(void)
{
    const char* ta;
    WQ_ENGINE c;
    WORLDQ q;
    WQ_RADAR st;
    uint8_t* bits;
    int32_t budget = INT32_MAX;
    unsigned slot, bytes, seat;
    ((void (*)(void))(uintptr_t)VA_RADAR_PASS)();
    ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    if (GetCurrentThreadId() != g_ddraw.gui_thread_id || !ta) return;
    seat = *(const unsigned char*)(ta + OFF_LOCALPLAYER);
    if (seat >= PLAYER_COUNT || *(const unsigned short*)(ta + 0x2A3C) < 2 ||
        !tagpu_worldq_open(&c, &q, (int)seat, WQ_SITE_ENGINE, NULL))
        return;
    bytes = WQ_RADAR_BYTES(c.nslots);
    if (!(bits = (uint8_t*)tagpu_sim_scratch(bytes))) return;
    memset(&st, 0, sizeof st);
    if (q.ops->radar_step(&q, &st, bits, (int)bytes, &budget)) {
        s_obs.radar_passes++;
        for (slot = 1; slot < c.nslots; slot++) {
            const unsigned state = *(const unsigned*)(c.units + slot * UNIT_STRIDE + U_STATE);
            const unsigned v = wq_radar_bits(bits, slot);
            const unsigned mask = v & WQ_RB_KEPT ? 3u : 7u;
            if (!(state & ST_ALIVE)) continue;
            s_obs.radar_eval++;
            if (((state >> 8) & mask) != (v & mask)) {
                s_obs.radar_miss++;
                if (!s_obs.radar_said) {
                    s_obs.radar_said = 1;
                    tagpu_logf("impai: check radar mismatch slot=%u ours=%X engine=%X gametime=%d", slot, v,
                               (state >> 8) & 7u, *(const int*)(ta + OFF_GAMETIME));
                }
            }
        }
    }
    tagpu_sim_scratch_free(bits);
}

/* Under tagpu_impai_check.on, once every ImpAI site is in: the radar check's call site, byte-matched, else no
   radar check and a line says why. */
static void impai_check_install(void)
{
    unsigned char now[5];
    const int32_t rel = (int32_t)((uint32_t)(uintptr_t)impai_radar_check - (VA_RADAR_CALL + 5u));
    if (!tagpu_detour_redirect_ok(VA_RADAR_CALL, VA_RADAR_PASS)) {
        tagpu_logf("impai: check: 0x%06X not stock, no radar check", (unsigned)VA_RADAR_CALL);
        return;
    }
    now[0] = 0xE8;
    memcpy(now + 1, &rel, 4);
    if (!tagpu_detour_write(VA_RADAR_CALL, now, 5)) {
        tagpu_logf("impai: check: 0x%06X could not be written, no radar check", (unsigned)VA_RADAR_CALL);
        return;
    }
    tagpu_logf("impai: check: line of sight in every ImpAI turn, radar at 0x%06X", (unsigned)VA_RADAR_CALL);
}

/* THE PATCHES' EVIDENCE, under the check lever at the level end: each observing seat's published patches,
   and the map's metal as the patch rule reads it (tagpu_worldq.c, metal: the feature grid's byte +7 of each
   16-px cell, against the threshold the open takes from game state +0xD30, held to 255), run-length coded,
   so tools/impai/impai_check.py recomputes every patch the whole map holds and finds each published one
   among them. On the game thread in the teardown's entry, before it frees the grid. BOUND: the open's --
   W and H even, 2..0x7FFF, W x H x 13 below 2^31, the grid read below W x H records. */
static void impai_patches_dump(void)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const unsigned char* grid;
    const char* state;
    char line[1600];
    unsigned i, k, th;
    int w, h, x, z, at;
    if (!ta) return;
    for (i = 0; i < PLAYER_COUNT; i++)
        if (s_obs.ticks[i])
            for (k = 0; k < s_seatst[i].nclusters && k < IMPAI_CLUSTERS_HALF; k++) {
                const SNAP_PATCH* p = &s_cluster[i][k];
                tagpu_logf("impai: patch seat=%u ax=%d az=%d x0=%d z0=%d x1=%d z1=%d cells=%u", i, p->ax, p->az,
                           p->x0, p->z0, p->x1, p->z1, p->cells);
            }
    w = *(const int*)(ta + OFF_MAP_W16);
    h = *(const int*)(ta + OFF_MAP_H16);
    grid = *(const unsigned char* const*)(ta + OFF_FEATMAP);
    state = *(const char* const*)(ta + 0x391E9);
    if (!grid || !state || w < 2 || h < 2 || (w & 1) || (h & 1) || w > 0x7FFF || h > 0x7FFF ||
        (unsigned)w > 0x7FFFFFFFu / FT_STRIDE / (unsigned)h)
        return;
    th = *(const unsigned*)(state + 0xD30);
    tagpu_logf("impai: metal w=%d h=%d threshold=%u", w, h, th > 255u ? 255u : th);
    for (z = 0; z < h; z++) {
        x = 0;
        while (x < w) {
            at = wsprintfA(line, "impai: metal z=%d x=%d", z, x);
            while (x < w && at < (int)sizeof line - 16) {
                const unsigned v = grid[((size_t)z * (unsigned)w + (unsigned)x) * FT_STRIDE + 7];
                int run = 1;
                while (x + run < w && grid[((size_t)z * (unsigned)w + (unsigned)(x + run)) * FT_STRIDE + 7] == v) run++;
                at += wsprintfA(line + at, " %X*%d", v, run);
                x += run;
            }
            tagpu_logf("%s", line);
        }
    }
}

/* The level's observation lines, at the level end: each seat's work against its bounds, and the checks'
   counts. The bound of `slots` is the slice plus a pass's publish, one observe a slot; of `visits` the
   budget plus one walk past it, which reads each slot's unit once at most; of `cells` the scan's rows across
   the map; of `flood` its budget. */
static void impai_observe_end(unsigned now)
{
    unsigned i;
    for (i = 0; i < PLAYER_COUNT; i++)
        if (s_obs.ticks[i] || s_obs.refused[i])
            tagpu_logf("impai: work seat=%u ticks=%u refused=%u slots_max=%u/%u visits_max=%u/%u cells_max=%u/%u "
                       "flood_max=%u/%u scans=%u clusters=%u gametime=%u", i, s_obs.ticks[i], s_obs.refused[i],
                       s_obs.slots_max[i], s_budget.obs_slice + s_obs.nslots_max, s_obs.visits_max[i],
                       s_budget.radar_visits + s_obs.nslots_max, s_obs.cells_max[i],
                       s_budget.clu_rows * s_obs.map_w, s_obs.flood_max[i], s_budget.clu_flood, s_obs.scans[i],
                       s_seatst[i].nclusters, now);
    for (i = 0; i < PLAYER_COUNT; i++)
        if (s_obs.ticks[i] || s_obs.refused[i]) {
            /* p99: the upper edge of the bucket the 99th percentile turn falls in */
            const unsigned turns = s_obs.ticks[i] + s_obs.refused[i];
            unsigned k, below = 0, p99 = 0;
            char hist[24 * 24] = "";   /* 24 buckets of at most 22 characters: never cut */
            int at = 0;
            for (k = 0; k < 24u; k++) {
                below += s_obs.cost_hist[i][k];
                if (!p99 && (double)below >= 0.99 * turns) p99 = 2u << k;
                /* each bucket as <upper edge in us>:<turns> */
                if (s_obs.cost_hist[i][k] && at >= 0 && at < (int)sizeof hist)
                    at += _snprintf(hist + at, sizeof hist - at, "%s%u:%u", at ? "," : "", 2u << k,
                                    s_obs.cost_hist[i][k]);
            }
            hist[sizeof hist - 1] = 0;
            tagpu_logf("impai: cost seat=%u ticks=%u max_us=%u mean_us=%.1f p99_us=%u slice=%u visits=%u nslots=%u "
                       "hist=%s", i, turns, s_obs.cost_max[i], s_obs.cost_sum[i] / (turns ? turns : 1), p99,
                       s_budget.obs_slice, s_budget.radar_visits, s_obs.nslots_max, hist);
        }
    if (!s_install.check) return;
    for (i = 0; i < PLAYER_COUNT; i++)
        if (s_obs.ticks[i])
            tagpu_logf("impai: check los seat=%u evaluated=%u mismatch=%u", i, s_obs.los_eval[i], s_obs.los_miss[i]);
    impai_patches_dump();
    tagpu_logf("impai: check los-local evaluated=%u mismatch=%u", s_obs.local_eval, s_obs.local_miss);
    tagpu_logf("impai: check radar passes=%u evaluated=%u mismatch=%u", s_obs.radar_passes, s_obs.radar_eval,
               s_obs.radar_miss);
}

/* The level init's half of observation, on every level, live or puppet: nothing of the previous level's
   knowledge survives, and a puppet's comes from its recording. */
static void impai_observe_clear(void)
{
    memset(s_known, 0, sizeof s_known);
    memset(s_seatst, 0, sizeof s_seatst);
    memset(s_radar_bits, 0, sizeof s_radar_bits);
    memset(s_cluster, 0, sizeof s_cluster);
    memset(&s_obs, 0, sizeof s_obs);
}

static int reach_pass(const void* ctx, int mclass, int cx, int cz)
{
    (void)ctx;
    return tagpu_worldq_passable_at((unsigned)mclass, cx, cz);
}

/* THE REACH BUILD, at the level init on the loader thread, after 0x440940 built the class maps at 0x4918E3
   (tagpu_worldq_passable_at): the labels of every class the level has, into one block sized for them. The
   previous level's block goes first, so no level's labels outlive it. BOUND: the map's W and H are the
   engine's, each held to the coordinate limit (2048 cells); the labels take (W/2)(H/2) u16 a class. A level
   whose block or BFS scratch cannot be had has no reach, said once in the log. */
static void impai_reach_build(void)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    unsigned c, n = 0, comps = 0;
    int w, h;
    size_t blocks;
    void* work;
    LARGE_INTEGER t0, t1;
    if (s_reach) SIM_FREE(s_reach, s_reach);
    s_reach = NULL;
    s_reach_n = 0;
    memset(&s_reach_ix, 0, sizeof s_reach_ix);
    if (!ta) return;
    w = *(const int*)(ta + OFF_MAP_W16);
    h = *(const int*)(ta + OFF_MAP_H16);
    if (w < 2 || h < 2 || w > 2048 || h > 2048) return;
    blocks = (size_t)(w / 2) * (size_t)(h / 2);
    for (c = 0; c < WQ_MCLASSES; c++)
        if (tagpu_worldq_class_exists(c)) {
            s_reach_ix.present |= 1u << c;
            n++;
        }
    if (!n) return;
    work = tagpu_sim_scratch(impai_reach_work(w, h));
    if (!work || !(s_reach = (uint16_t*)SIM_ALLOC(s_reach, n * blocks * sizeof(uint16_t)))) {
        tagpu_sim_scratch_free(work);
        s_reach_ix.present = 0;
        tagpu_logf("impai: reach none: %u classes of %dx%d cells need memory not had", n, w, h);
        return;
    }
    s_reach_ix.bw = (uint16_t)(w / 2);
    s_reach_ix.bh = (uint16_t)(h / 2);
    QueryPerformanceCounter(&t0);
    for (c = 0, n = 0; c < WQ_MCLASSES; c++) {
        if (!(s_reach_ix.present >> c & 1u)) continue;
        s_reach_ix.at[c] = (uint32_t)(n * blocks);
        comps += (unsigned)impai_reach_label(reach_pass, NULL, (int)c, w, h, s_reach + n * blocks, work);
        n++;
    }
    QueryPerformanceCounter(&t1);
    tagpu_sim_scratch_free(work);
    s_reach_n = (unsigned)(n * blocks);
    /* the work in counts -- each class labels every cell once and reads its four neighbours, then every
       block -- and, as a measurement, its time */
    tagpu_logf("impai: reach classes=%u blocks=%ux%u components=%u cells=%u blocks_written=%u us=%u", n,
               s_reach_ix.bw, s_reach_ix.bh, comps, n * (unsigned)w * (unsigned)h, s_reach_n,
               s_qpf.QuadPart > 0 ? (unsigned)((t1.QuadPart - t0.QuadPart) * 1000000 / s_qpf.QuadPart) : 0u);
}

/* THE THREAT TABLE (impai.h, IMPAI_THREAT), one row a unit type of the level: what the role classifier
   (tasim_roles.c) makes of the type's def and weapons as the level load left them, the same rule the
   calibration runner's `tasim_run roles` applies to the install's files. Content: a function of the level's
   defs and weapons, rebuilt at every level init, live or puppet. */
static IMPAI_THREAT s_threat_def[TAGPU_LIM_TYPES];
static unsigned     s_threat_n;
SIM_ROWS_LIVE(s_threat_def, SIM_CONTENT, SIM_ID_UNIT_TYPE, s_threat_n, NULL);
SIM_VAR(s_threat_n, SIM_CONTENT);

/* THE THREAT BUILD, at the level init on the loader thread, after the unit-data load (tagpu_worldq.c, THE
   ROLE TABLE'S ENGINE FILLER, has the ordering and the bounds): every type's classifier input from the
   engine, the classification, and its rounding into s_threat_def. The classifier's -1 (no type is a
   commander, so no tech level) leaves every figure the table takes defined (tasim_roles_classify writes the
   tech level last); its -2 cannot arise, as the filler gives no build options. A level whose input cannot be
   had has no threat table, and the maps then draw no threat or influence. Under the check lever every type's
   role and figures are logged for tools/impai/roles_check.py, which compares them with `tasim_run roles`. */
static void impai_threat_build(void)
{
    const int n = tagpu_worldq_role_count();
    TASIM_ROLE_IN* in = NULL;
    TASIM_ROLE_OUT* out = NULL;
    TASIM_ROLE_Q q;
    void* work = NULL;
    int rc = -3, i;
    memset(s_threat_def, 0, sizeof s_threat_def);
    s_threat_n = 0;
    if (n > 0 && (in = (TASIM_ROLE_IN*)tagpu_sim_scratch((size_t)n * sizeof *in)) &&
        (out = (TASIM_ROLE_OUT*)tagpu_sim_scratch((size_t)n * sizeof *out)) &&
        (work = tagpu_sim_scratch(tasim_roles_work(n))) && tagpu_worldq_role_in(in, n) == 0)
        rc = tasim_roles_classify(in, n, out, &q, work);
    if (rc == 0 || rc == -1) {
        impai_threat_from_roles(out, n, s_threat_def);
        s_threat_n = (unsigned)n;
        if (s_install.check)
            for (i = 0; i < n; i++)
                tagpu_logf("impai: role %s %s %s value=%.17g dps_ground=%.17g dps_air=%.17g dps_water=%.17g "
                           "range_ground=%d flags=0x%02X", in[i].name, tasim_roles_name(out[i].role),
                           tasim_roles_layer_name(out[i].layer), out[i].value, out[i].dps_ground,
                           out[i].dps_air, out[i].dps_water, (int)out[i].range_ground, out[i].flags);
        tagpu_logf("impai: threat types=%d%s", n, rc == -1 ? " (no commander: no tech level)" : "");
    } else {
        tagpu_logf("impai: threat none: the role classifier's input could not be had (%d types, %d)", n, rc);
    }
    tagpu_sim_scratch_free(work);
    tagpu_sim_scratch_free(out);
    tagpu_sim_scratch_free(in);
}

/* ---- THE OVERLAY (tagpu_impai.h, tagpu_impai_overlay): what one seat knows, for the marker pass ----

   The test lever tagpu_impaiov.on, read at attach: `<seat>[:<layer>]`, the layer one of ground, air,
   water, influence, presence, reach (default ground). `tacli arm <instance> impaiov.on=1:air` writes it.
   Never a player setting. */
static struct {
    uint8_t on, seat, layer;
} s_ov;
SIM_VAR(s_ov, SIM_INSTALL);

static const char* const OV_LAYER[TAGPU_PK_IL_LAST + 1] = {
    "ground", "air", "water", "influence", "presence", "reach" };

static void impai_overlay_lever(void)
{
    char t[40];
    int n = tagpu_sim_lever_read("tagpu_impaiov.on", t, (int)sizeof t), k;
    char* p;
    if (n < 0) return;
    while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\r' || t[n - 1] == '\n' || t[n - 1] == '\t')) t[--n] = 0;
    if (t[0] < '0' || t[0] > '9' || (t[1] && t[1] != ':')) {
        tagpu_logf("impai: tagpu_impaiov.on: `%s` not understood (<seat>[:<layer>]) - no overlay", t);
        return;
    }
    s_ov.seat = (uint8_t)(t[0] - '0');
    s_ov.layer = TAGPU_PK_IL_GROUND;
    if (t[1] == ':') {
        p = t + 2;
        for (k = 0; k <= TAGPU_PK_IL_LAST && lstrcmpiA(p, OV_LAYER[k]); k++) {}
        if (k > TAGPU_PK_IL_LAST) {
            tagpu_logf("impai: tagpu_impaiov.on: layer `%s` not understood - no overlay", p);
            return;
        }
        s_ov.layer = (uint8_t)k;
    }
    s_ov.on = 1;
    tagpu_logf("impai: overlay seat %u, %s", s_ov.seat, OV_LAYER[s_ov.layer]);
}

/* THE MAPS' CACHE, the overlay's alone: SIM_PRIVATE, so nothing the simulation reads. Rebuilt in the
   first fill of each 30-tick epoch (GameTime / 30) in which the seat's knowledge generation has moved,
   and in the first fill of a level; under the turbo the harness plays with, every tick has its fill, so
   the live game and its puppet rebuild at the same GameTimes. `cellh` is the ground's height at each
   cell's centre, taken at the build. The level init forgets the cache (s_ovc.built), on the loader thread
   before the level's first in-play frame, the only frames that fill. */
static struct {
    uint8_t  built;
    uint32_t epoch, gen;
    int      gw, gh;
    uint64_t known, ov;
    uint32_t threat[IMPAI_THREATS][IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    int32_t  influence[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    uint16_t presence[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    uint16_t reach[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    int32_t  cellh[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    int32_t  diff[(IMPAI_GRID_MAX + 1) * IMPAI_GRID_MAX];
} s_ovc;
SIM_VAR(s_ovc, SIM_PRIVATE);

typedef int (__stdcall* PFN_GroundHeight)(const int* pos);
#define VA_GROUND_HEIGHT 0x00485070u   /* (pos 16.16 x, alt, z), `ret 4`: the ground's height, -1 off the map */

/* The ground's height at world px (x, z), 0 off the map: the one engine call of the fill, the call the
   sweep's ring makes (tagpu_sweep.c), on the thread that owns the map */
static int32_t ground_at(int32_t x, int32_t z)
{
    int pos[3];
    int h;
    pos[0] = (int)((uint32_t)x << 16); pos[1] = 0; pos[2] = (int)((uint32_t)z << 16);
    h = ((PFN_GroundHeight)(uintptr_t)VA_GROUND_HEIGHT)(pos);
    return h < 0 ? 0 : h;
}

/* a value's shade: 1 + floor(log2(v)), held to 15 */
static uint8_t shade(uint32_t v)
{
    uint8_t s = 1;
    while (v > 1u && s < 15u) { v >>= 1; s++; }
    return s;
}

static void ov_push(TAGPU_PK_IMPAI* out, unsigned max, unsigned* n, int* cut, uint8_t kind, uint8_t level,
                    uint8_t size, int32_t x, int32_t h, int32_t z)
{
    TAGPU_PK_IMPAI* e;
    if (*n >= max) { *cut = 1; return; }
    e = &out[(*n)++];
    e->kind = kind; e->seat = s_ov.seat; e->level = level; e->size = size;
    e->x = x; e->h = h; e->z = z;
}

/* THE ITEMS, from the cache and the seat's declared state: the layer's nonzero cells, then the seat's rows
   in slot order -- its own units, its contacts (any class of now, or radar or sonar), its ghosts -- then
   its published patches. A row's point is its block's centre. BOUND: every row index below the table's
   rows, every cell below the cache's gw x gh (each at most IMPAI_GRID_MAX), every patch below the
   published count, at most IMPAI_CLUSTERS_HALF; at most `max` items. */
static void ov_items(TAGPU_PK_IMPAI* out, unsigned max, unsigned* n, int* cut)
{
    const unsigned seat = s_ov.seat;
    const unsigned npatch = s_seatst[seat].nclusters < IMPAI_CLUSTERS_HALF ? s_seatst[seat].nclusters
                                                                           : IMPAI_CLUSTERS_HALF;
    unsigned i;
    int gx, gz;
    *n = 0;
    for (gz = 0; gz < s_ovc.gh; gz++)
        for (gx = 0; gx < s_ovc.gw; gx++) {
            const int c = gz * s_ovc.gw + gx;
            uint32_t v;
            uint8_t level;
            switch (s_ov.layer) {
            case TAGPU_PK_IL_INFLUENCE: {
                const int32_t f = s_ovc.influence[c];
                v = f < 0 ? (uint32_t)0 - (uint32_t)f : (uint32_t)f;
                level = (uint8_t)(shade(v) | (f < 0 ? TAGPU_PK_IM_NEG : 0u));
                break;
            }
            case TAGPU_PK_IL_PRESENCE: v = s_ovc.presence[c]; level = shade(v); break;
            case TAGPU_PK_IL_REACH: v = s_ovc.reach[c]; level = (uint8_t)(1u + v % 15u); break;
            default: v = s_ovc.threat[s_ov.layer][c]; level = shade(v); break;
            }
            if (v)
                ov_push(out, max, n, cut, TAGPU_PK_IM_CELL, level, IMPAI_CELL_PX / 32, gx * IMPAI_CELL_PX,
                        s_ovc.cellh[c], gz * IMPAI_CELL_PX);
        }
    for (i = 1; i < sizeof s_known / sizeof s_known[0]; i++) {
        const IMPAI_SEEN* r = &s_known[i].seat[seat];
        const int32_t x = r->bx * 32 + 16, z = r->bz * 32 + 16;
        uint8_t kind;
        if (!r->how && !r->bx && !r->bz && !r->type && !r->owner) continue;
        if (r->how & IMPAI_OWN) kind = TAGPU_PK_IM_OWN;
        else if (r->how & (IMPAI_NOW | IMPAI_PASS)) kind = TAGPU_PK_IM_CONTACT;
        else kind = TAGPU_PK_IM_GHOST;
        ov_push(out, max, n, cut, kind, r->how, 0, x, ground_at(x, z), z);
    }
    for (i = 0; i < npatch; i++) {
        const SNAP_PATCH* p = &s_cluster[seat][i];
        const int32_t x = (p->x0 + p->x1 + 1) * 8, z = (p->z0 + p->z1 + 1) * 8;
        const int span = (p->x1 - p->x0 > p->z1 - p->z0 ? p->x1 - p->x0 : p->z1 - p->z0) / 2 + 1;
        ov_push(out, max, n, cut, TAGPU_PK_IM_CLUSTER, 0, (uint8_t)(span > 255 ? 255 : span), x,
                ground_at(x, z), z);
    }
}

static uint64_t fnv64(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/* THE BUILD: the maps over the seat's rows, its patches, its alliance row (main's record of the seat,
   +0x108 + j, one of the ten records by index), the threat table and the map's size; the reach layer's
   label at each cell's centre block for the level's lowest movement class; each cell's ground height. */
static int ov_build(const char* ta, unsigned gen, uint32_t epoch)
{
    const unsigned seat = s_ov.seat;
    const char* rec = ta + OFF_PLAYERS + seat * PLAYER_STRIDE;
    IMPAI_VIEW v;
    IMPAI_MAPS m;
    unsigned j, c;
    int gx, gz, layer;
    memset(&v, 0, sizeof v);
    v.seat = (int)seat;
    v.rows = s_known;
    v.nrows = (int)(sizeof s_known / sizeof s_known[0]);
    v.clusters = s_cluster[seat];
    v.nclusters = s_seatst[seat].nclusters < IMPAI_CLUSTERS_HALF ? s_seatst[seat].nclusters : IMPAI_CLUSTERS_HALF;
    for (j = 0; j < PLAYER_COUNT; j++)
        if (j != seat && *(const uint8_t*)(rec + 0x108 + j) == 1) v.allies |= 1u << j;
    v.threat = s_threat_n ? s_threat_def : NULL;
    v.ntypes = (int)s_threat_n;
    v.w = *(const int*)(ta + OFF_MAP_W16);
    v.h = *(const int*)(ta + OFF_MAP_H16);
    for (layer = 0; layer < IMPAI_THREATS; layer++) m.threat[layer] = s_ovc.threat[layer];
    m.influence = s_ovc.influence;
    m.presence = s_ovc.presence;
    m.diff = s_ovc.diff;
    if (!impai_maps_build(&v, &m)) return 0;
    if (!v.threat) {
        for (layer = 0; layer < IMPAI_THREATS; layer++) memset(s_ovc.threat[layer], 0, sizeof s_ovc.threat[layer]);
        memset(s_ovc.influence, 0, sizeof s_ovc.influence);
    }
    s_ovc.gw = m.gw;
    s_ovc.gh = m.gh;
    for (c = 0; c < WQ_MCLASSES && !(s_reach_ix.present >> c & 1u); c++) {}
    for (gz = 0; gz < m.gh; gz++)
        for (gx = 0; gx < m.gw; gx++) {
            const unsigned bx = (unsigned)gx * (IMPAI_CELL_PX / 32) + IMPAI_CELL_PX / 64;
            const unsigned bz = (unsigned)gz * (IMPAI_CELL_PX / 32) + IMPAI_CELL_PX / 64;
            const int k = gz * m.gw + gx;
            s_ovc.reach[k] = c < WQ_MCLASSES && bx < s_reach_ix.bw && bz < s_reach_ix.bh
                ? s_reach[s_reach_ix.at[c] + bz * s_reach_ix.bw + bx] : 0;
            s_ovc.cellh[k] = ground_at(gx * IMPAI_CELL_PX + IMPAI_CELL_PX / 2, gz * IMPAI_CELL_PX + IMPAI_CELL_PX / 2);
        }
    s_ovc.built = 1;
    s_ovc.epoch = epoch;
    s_ovc.gen = gen;
    /* the digests' first halves, for the fill's log line: what the seat knew, and the maps made of it */
    s_ovc.known = impai_known_digest(&v);
    s_ovc.ov = fnv64(impai_maps_digest(&m), s_ovc.reach, (size_t)m.gw * (size_t)m.gh * sizeof s_ovc.reach[0]);
    return 1;
}

/* THE FILL. It reads ImpAI's declared state and content (s_seat, s_known, s_seatst, s_cluster,
   s_threat_def, s_reach), all of which a puppet takes from its recording or rebuilds at its level init,
   and of the engine only what the recording carries as well -- GameTime, the map's size, the seat's
   alliance row -- and the ground's height. So a puppet's overlay is the live game's. */
int tagpu_impai_overlay(TAGPU_PK_IMPAIHDR* h, TAGPU_PK_IMPAI* out, unsigned max, unsigned* n, int* cut)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const unsigned seat = s_ov.seat;
    uint32_t now, epoch, gen;
    int w, hh, built;
    *n = 0;
    if (!s_ov.on || !ta || seat >= PLAYER_COUNT) return 0;
    gen = s_seatst[seat].known_gen;
    if (!(s_seat[seat] & IMPAI_ON) && !gen) return 0;
    w = *(const int*)(ta + OFF_MAP_W16);
    hh = *(const int*)(ta + OFF_MAP_H16);
    if (w < 2 || hh < 2 || w * 16 > IMPAI_GRID_MAX * IMPAI_CELL_PX || hh * 16 > IMPAI_GRID_MAX * IMPAI_CELL_PX)
        return 0;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    epoch = now / 30u;
    built = !s_ovc.built || (epoch != s_ovc.epoch && gen != s_ovc.gen);
    if (built && !ov_build(ta, gen, epoch)) return 0;
    memset(h, 0, sizeof *h);
    h->seat = (uint8_t)seat;
    h->layer = s_ov.layer;
    h->radar_age = s_seatst[seat].radar_done ? now - s_seatst[seat].radar_done : 0xFFFFFFFFu;
    h->known_gen = gen;
    ov_items(out, max, n, cut);
    if (built && s_install.check) {
        /* the pair impai_check.py (b) judges: what the seat knew, and the maps and the items made of it */
        const uint64_t ov = fnv64(s_ovc.ov, out, (size_t)*n * sizeof *out);
        tagpu_logf("impai: ov seat=%u tick=%u known=%08X%08X ov=%08X%08X items=%u", seat, now,
                   (unsigned)(s_ovc.known >> 32), (unsigned)s_ovc.known, (unsigned)(ov >> 32), (unsigned)ov, *n);
    }
    return 1;
}

/* THE WRAPPER, the target of the call at 0x465031. The site calls 0x408C40 as thiscall with no
   stack argument (ECX the AI object, 0x46502A) and expects a plain `ret` (0x408CA2, 0x408CAF).
   __fastcall takes ECX as its first argument and EDX as its second and pops nothing, so the
   site's stack is unchanged; EBX, ESI, EDI and EBP are callee-saved in every convention, so the
   loop counter BL, the record EDI and the record index ESI that the players phase uses after the
   call (0x465036 `push esi`, 0x46506A `cmp bl,[eax+0x2A43]`) survive. The stock tick always runs,
   so the stock AI plays every seat and the recorder's observer on 0x408C40's entry
   (tagpu_recorder.c, ai_enter) still sees each call. */
static void __fastcall impai_ai_tick(void* obj, void* edx_unused)
{
    int seat = impai_seat_of(obj);
    (void)edx_unused;
    if (seat >= 0) impai_observe(seat);
    ((PFN_AiTick)(uintptr_t)VA_AI_TICK)(obj);
}

/* THE LEVEL END, on the game thread in the teardown 0x491B60's entry, after the recorder has
   closed the level's recording (tagpu_reclaim.c, reclaim_teardown_pre). It writes the level's
   counts and clears the seat table, so nothing the engine runs between this level and the next
   level init reads a mark of this one: the next load's loader thread, the only thread that can
   reach a site before that init (its early pump 0x49727D, network loads), is created later by
   this thread (0x4982CA), which orders the clear before it. The install requires the teardown
   wrap (tagpu_impai_init), so a level with marks always ends here. */
void tagpu_impai_level_end(void)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    char line[IMPAI_HSITES * 28];
    unsigned i, now;
    int at;
    if (s_install.in && ta) {
        now = *(const unsigned*)(ta + OFF_GAMETIME);
        for (i = 0; i < PLAYER_COUNT; i++)
            if ((s_seat[i] & IMPAI_ON) || s_diag.calls[i])
                tagpu_logf("impai: calls seat=%u n=%u gametime=%u", i, s_diag.calls[i], now);
        at = 0;
        for (i = 0; i < IMPAI_HSITES; i++)
            at += wsprintfA(line + at, " %u=%u/%u", i + 1, s_diag.hits[i][HIT_SCALED],
                            s_diag.hits[i][HIT_EXEMPT]);
        tagpu_logf("impai: handicap rows scaled/exempt gametime=%u:%s", now, line);
        impai_observe_end(now);
    }
    memset(s_seat, 0, sizeof s_seat);
}

/* The level init's seat choice, the AI command and the header's accessor: what decides which
   seats ImpAI plays. On a puppet the level init and the level end only clear the table and the
   AI command clears no mark, so between the two clears the recording is the only writer. */
SIM_FEATURE(impai_seats, SIM_PUPPET_OWNER, "impai-handicap impai-observe impai-observe-perm stock-impai-observe");

/* THE LEVEL INIT, on the loader thread inside the PLAYERS call 0x4919C8, right after the
   engine's players init 0x464990 has seated and activated every record and before the loader's
   last store (0x497C62), which the game thread waits for before the level's first step: so the
   table is whole before any game-thread turn reads it (impai_seat_of, THE THREAD). It clears
   the table on every level (s_seat); on a puppet level it marks no seat: the recording restores
   the table, and the recorder is the only writer of what a recording carries.
   THE SEAT CHOICE is impure.cfg's `seatai`, bit i for settings entry i, which is record i: the
   seat fill 0x47A760 activates record i for entry i on every path that seats a skirmish
   (engine/sim.md, Seating a level). */
void tagpu_impai_level_init(void)
{
    const char* ta;
    char list[2 * PLAYER_COUNT + 1];
    unsigned i, live = 0, remote = PLAYER_COUNT, n = 0, want;
    memset(&s_diag, 0, sizeof s_diag);
    memset(s_seat, 0, sizeof s_seat);
    s_ovc.built = 0;
    impai_observe_clear();
    impai_reach_build();
    impai_threat_build();
    if (tagpu_puppet_on()) return;
    ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    if (!ta) return;
    for (i = 0; i < PLAYER_COUNT; i++) {
        const char* rec = ta + OFF_PLAYERS + i * PLAYER_STRIDE;
        if (!*(const uint32_t*)(rec + PL_ACTIVE)) continue;
        live++;
        if ((uint8_t)rec[PL_CONTROL] == CTRL_REMOTE && remote == PLAYER_COUNT) remote = i;
    }
    /* A remote record means a network game, which ImpAI does not play yet. */
    if (remote < PLAYER_COUNT) {
        tagpu_logf("impai: seats none: record %u is remote, a network game", remote);
        return;
    }
    if (!s_install.avail) {
        tagpu_logf("impai: unavailable: %s", s_install.why);
        return;
    }
    want = (unsigned)tagpu_settings_seatai();
    for (i = 0; i < PLAYER_COUNT; i++) {
        const char* rec = ta + OFF_PLAYERS + i * PLAYER_STRIDE;
        if (!*(const uint32_t*)(rec + PL_ACTIVE) || (uint8_t)rec[PL_CONTROL] != CTRL_AI) continue;
        if (!(want & (1u << i))) continue;
        s_seat[i] = IMPAI_ON;
        if (n) list[n++] = ',';
        list[n++] = (char)('0' + i);
    }
    list[n] = 0;
    tagpu_logf("impai: seats %s (impure.cfg) of %u records", n ? list : "none", live);
}

/* The recording header's byte for record i (RT_PLAYER byte 15): 1 when ImpAI plays it -- the
   record is seated, of controller 2 and marked, the seam's own test -- else 0. Game thread, at
   the header's capture after the level init, or a fork's after the takeover's restore, so it
   states the seats the game ran. */
unsigned tagpu_impai_seat_ai(unsigned i)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const char* rec;
    if (i >= PLAYER_COUNT || !ta || !(s_seat[i] & IMPAI_ON)) return 0;
    rec = ta + OFF_PLAYERS + i * PLAYER_STRIDE;
    return *(const uint32_t*)(rec + PL_ACTIVE) && (uint8_t)rec[PL_CONTROL] == CTRL_AI;
}

#define VA_AI_CMD_AI    0x004162F4u   /* `AI`'s type 1 -> 2: `call 0x463C60`, E8 67 D9 04 00      */
#define VA_AI_CMD_HUMAN 0x004162FEu   /* `AI`'s type 2 or 3 -> 1: `call 0x463C60`, E8 5D D9 04 00  */
#define VA_SET_TYPE     0x00463C60u   /* thiscall (rec, type), ret 4: +0x73 = type             */

typedef void (__thiscall* PFN_SetType)(void* rec, int type);

/* THE AI COMMAND: `AI` never leaves a seat to ImpAI. 0x416280, the console's `AI` (row
   0x501FD0, mask 4: cheats), sets the type of the active record its argument names through
   0x463C60 at one of two calls (DISASSEMBLED): type 1 becomes 2 with `push 2; call 0x463C60` at
   0x4162F2..0x4162F4 -- a human's seat handed to the stock AI, as in stock (impai/decisions.md,
   Settled at L0) -- and types 2 and 3 become 1 with `push 1; call 0x463C60` at
   0x4162FC..0x4162FE, so `AI` on a computer seat makes it a human's. Both calls come
   here, and the record loses its ImpAI mark either way: an ImpAI seat becomes a human's and, at a
   second `AI`, OTA AI's; a type-1 record that carries a mark (a takeover played as an ImpAI seat)
   becomes OTA AI's. The record is ECX (thiscall), the type one stack argument the callee pops
   (0x463C60 ends `ret 4`), which __fastcall's third parameter reproduces; both sites return
   (`ret 4` at 0x4162F9 and 0x416303) reading no register.
   THE BOUND is the seam's: the record is one of main's ten by range and exact division, else
   nothing is cleared. THE THREAD: the game thread between steps (the TALK screen 0x493BF0 and
   the keys 0x495E90), or the loader thread when a profile script names `AI` (the profile load
   0x4648E0, inside the players init 0x464990), before tagpu_impai_level_init clears the table
   and marks the record if `seatai` names it. On a puppet nothing is written: the recorder is the
   only writer of the table there. */
static void __fastcall impai_ai_command(char* rec, void* edx_unused, int type)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    (void)edx_unused;
    if (ta && !tagpu_puppet_on()) {
        uintptr_t off = (uintptr_t)rec - (uintptr_t)(ta + OFF_PLAYERS);
        if (off < (uintptr_t)PLAYER_COUNT * PLAYER_STRIDE && !(off % PLAYER_STRIDE)
            && (s_seat[off / PLAYER_STRIDE] & IMPAI_ON)) {
            s_seat[off / PLAYER_STRIDE] = 0;
            tagpu_logf("impai: the AI command makes record %u type %d: ImpAI no longer plays it",
                       (unsigned)(off / PLAYER_STRIDE), type);
        }
    }
    ((PFN_SetType)(uintptr_t)VA_SET_TYPE)(rec, type);
}

/* The Difficulty handicap's exemption: eighteen type tests of the engine skip a record ImpAI
   plays. RUNS on a puppet: the stubs run there and take stock's branch by the site's own
   compare, every recorded record being type 3. */
SIM_FEATURE(impai_handicap, SIM_PUPPET_RUNS, "impai-handicap");

/* The eighteen sites (engine/sim.md, Difficulty and the AI handicap; the hook map, The AI seat):
   rows 1-17 scale a type-2 record's resources by main+0x37EEE, row 18 is the on-hit handler
   0x406F80's commander purge. Each holds `cmp byte [rec+0x73],2` (or `cmp [eax+0x73],bl` with
   bl = 2, rows 11-12) and a `jne` to the unscaled branch, short or near; `rec_ecx` when the
   record is in ECX, else EAX. Bytes DISASSEMBLED from the retail image, and compared again at
   attach. */
typedef struct HSITE {
    uint32_t va;
    uint8_t  n;            /* the compare and its jne: 5, 6 or 10 bytes    */
    uint8_t  clen;         /* the compare alone: 3 or 4                    */
    uint8_t  rec_ecx;
    uint8_t  was[10];
} HSITE;

#define CMP2_EAX 0x80, 0x78, 0x73, 0x02
#define CMP2_ECX 0x80, 0x79, 0x73, 0x02
#define CMPBL    0x38, 0x58, 0x73

static const HSITE HSITES[] = {
    { 0x00401435u,  6, 4, 0, { CMP2_EAX, 0x75, 0x37 } },             /*  1 economy: EnergyUse < 0  */
    { 0x004014B2u, 10, 4, 0, { CMP2_EAX, 0x0F, 0x85, 0x83, 0, 0, 0 } }, /* 2 extractor metal    */
    { 0x004014F3u,  6, 4, 0, { CMP2_EAX, 0x75, 0x46 } },             /*  3 makesmetal              */
    { 0x0040157Fu,  6, 4, 0, { CMP2_EAX, 0x75, 0x26 } },             /*  4 wind                    */
    { 0x004015F3u, 10, 4, 0, { CMP2_EAX, 0x0F, 0x85, 0xB8, 0, 0, 0 } }, /* 5 tide                 */
    { 0x0040167Cu,  6, 4, 0, { CMP2_EAX, 0x75, 0x33 } },             /*  6 mobile EnergyUse < 0    */
    { 0x004016F2u,  6, 4, 0, { CMP2_EAX, 0x75, 0x33 } },             /*  7 energymake              */
    { 0x00401753u,  6, 4, 0, { CMP2_EAX, 0x75, 0x36 } },             /*  8 metalmake               */
    { 0x0040269Fu,  6, 4, 0, { CMP2_EAX, 0x75, 0x38 } },             /*  9 build-cancel refund     */
    { 0x0041BBB1u,  6, 4, 0, { CMP2_EAX, 0x75, 0x37 } },             /* 10 unbuild refund          */
    { 0x004238C4u,  5, 3, 0, { CMPBL, 0x75, 0x2E } },                /* 11 reclaim payout energy   */
    { 0x00423918u,  5, 3, 0, { CMPBL, 0x75, 0x38 } },                /* 12 reclaim payout metal    */
    { 0x00464C06u,  6, 4, 1, { CMP2_ECX, 0x75, 0x32 } },             /* 13 energy transfer         */
    { 0x00464D36u,  6, 4, 1, { CMP2_ECX, 0x75, 0x34 } },             /* 14 metal transfer          */
    { 0x00465437u,  6, 4, 0, { CMP2_EAX, 0x75, 0x38 } },             /* 15 respawn energy          */
    { 0x004654A3u,  6, 4, 0, { CMP2_EAX, 0x75, 0x38 } },             /* 16 respawn metal           */
    { 0x00486CDAu,  6, 4, 1, { CMP2_ECX, 0x75, 0x38 } },             /* 17 reclaimed unit's credit */
    { 0x00406FC3u,  6, 4, 0, { CMP2_EAX, 0x75, 0x2D } },             /* 18 on-hit commander purge  */
};
#define NSITES ((int)(sizeof HSITES / sizeof HSITES[0]))
#define STUB_ROOM 96u          /* one stub's bytes, rounded up: the longest is 86 */
_Static_assert(sizeof HSITES / sizeof HSITES[0] == IMPAI_HSITES, "the seventeen handicap sites and 0x406FC3");
_Static_assert(18 * STUB_ROOM <= 0x1000, "every stub fits the one page");

/* Every stub, one page made at attach and kept for the process: the sites jump into it. */
static unsigned char* s_code;
SIM_CODE(s_code);

/* the jne's target: the unscaled branch */
static uint32_t site_unscaled(const HSITE* s)
{
    const uint8_t* j = s->was + s->clen;
    int32_t rel;
    if (j[0] == 0x75) return s->va + s->n + (uint32_t)(int32_t)(int8_t)j[1];
    memcpy(&rel, j + 2, 4);
    return s->va + s->n + (uint32_t)rel;
}

static unsigned char* put32(unsigned char* p, uint32_t v)
{
    memcpy(p, &v, 4);
    return p + 4;
}

/* THE STUB, one per site; the site's first five bytes become `jmp stub`, the rest NOPs.

       C                       the site's own compare, copied from the site
       jne U                   not type 2: stock's unscaled branch
       push eax; push ecx; push edx
      [mov eax,ecx]            rows whose record is in ECX
       mov ecx,[0x511DE8]; add ecx,0x1B63      record 0
       sub eax,ecx; cmp eax,10*0x14B; jae not
       xor edx,edx; mov ecx,0x14B; div ecx; test edx,edx; jnz not
       test byte [s_seat+eax],IMPAI_ON; jz not
       pop edx; pop ecx; pop eax
       inc dword [hits+EXEMPT]; jmp U          an ImpAI seat: unscaled
   not: pop edx; pop ecx; pop eax
       inc dword [hits+SCALED]; jmp S          an OTA AI seat: stock's scaled path, S = va + n

   `hits` is the site's row of s_diag.hits: the level's count of type-2 records that took each
   exit, for the level-end line and handicap_check.py.

   THE INVARIANT.
   - A bound. The record pointer is data until it is one of main's ten records: an offset below
     10 x 0x14B, the bound the engine itself loops to (0x46602D), that divides by 0x14B exactly.
     Only then is the quotient an index into s_seat[10]; nothing else is dereferenced (the site
     itself has already read [rec+0x73]).
   - Stock is preserved. The site's own compare and branch run first, so a record stock does not
     scale is not scaled. EAX, ECX and EDX are restored before either exit; no x87 register is
     touched (zero to two values are live on the FPU stack at a site, none at rows 13, 14 and 18:
     0x464B88 and 0x464CB8 empty it with `fstp st(0)` before a call, and 0x406F80 holds no x87
     value, DISASSEMBLED); nothing is called. The `inc` before each exit writes only the flags,
     and the flags are dead at all 36 exits: from every U and S the first instruction that
     touches them writes them (sub, cmp, test, xor) or is a call, directly or through one jmp,
     and 0x4016C5 passes only fcomp/fnstsw before its `test ah` (DISASSEMBLED). No direct branch
     in .text lands inside a site, and every site starts and ends on an instruction (the linear
     sweep, 49 473 direct branches).
   - An ordering. s_seat is written by the level init on the loader thread inside PLAYERS
     0x4919C8, which precedes on that thread the loader's own players phase 0x497C10 and every
     loader-thread order destruction that reaches a site (0x497B29, 0x497B40), and precedes the
     game thread's first step through the loader's done bit (0x497C62, tested 0x498342); later
     only by the recorder's restore and the AI command, both on the game thread between steps,
     and by the level end's clear in the teardown. The sites run inside the step on the game
     thread, between steps on it (console Give, the SHARE dialog, a self-destruct), or on the
     loader thread after 0x4919C8. The one earlier path is the loader's early pump 0x49727D,
     which in a network load can remove a player and so destroy its units (scout I1 of the ImpAI
     plan, function-level reachability; in a skirmish the pump returns at once, 0x453D4B). It
     runs before this level's init, and reads a table the previous level's end cleared
     (tagpu_impai_level_end): every type-2 record takes stock's branch there.
   - A puppet holds type-3 records, which the site's own compare sends to U as stock does; s_seat
     is not read there. */
static unsigned char* inc_hits(unsigned char* p, const unsigned* hit)
{
    *p++ = 0xFF; *p++ = 0x05;
    return put32(p, (uint32_t)(uintptr_t)hit);
}

static unsigned char* stub_emit(unsigned char* p, const HSITE* s, const unsigned* hits)
{
    const uint32_t U = site_unscaled(s), S = s->va + s->n;
    unsigned char *j1, *j2, *j3, *not_;
    memcpy(p, (const void*)(uintptr_t)s->va, s->clen);
    p += s->clen;
    *p++ = 0x0F; *p++ = 0x85; tagpu_detour_rel(p, U); p += 4;
    *p++ = 0x50; *p++ = 0x51; *p++ = 0x52;
    if (s->rec_ecx) { *p++ = 0x8B; *p++ = 0xC1; }
    *p++ = 0x8B; *p++ = 0x0D; p = put32(p, TA_MAIN_PP);
    *p++ = 0x81; *p++ = 0xC1; p = put32(p, OFF_PLAYERS);
    *p++ = 0x2B; *p++ = 0xC1;
    *p++ = 0x3D; p = put32(p, PLAYER_COUNT * PLAYER_STRIDE);
    *p++ = 0x73; j1 = p++;
    *p++ = 0x33; *p++ = 0xD2;
    *p++ = 0xB9; p = put32(p, PLAYER_STRIDE);
    *p++ = 0xF7; *p++ = 0xF1;
    *p++ = 0x85; *p++ = 0xD2;
    *p++ = 0x75; j2 = p++;
    *p++ = 0xF6; *p++ = 0x80; p = put32(p, (uint32_t)(uintptr_t)s_seat); *p++ = IMPAI_ON;
    *p++ = 0x74; j3 = p++;
    *p++ = 0x5A; *p++ = 0x59; *p++ = 0x58;
    p = inc_hits(p, &hits[HIT_EXEMPT]);
    *p++ = 0xE9; tagpu_detour_rel(p, U); p += 4;
    not_ = p;
    *j1 = (unsigned char)(not_ - (j1 + 1));
    *j2 = (unsigned char)(not_ - (j2 + 1));
    *j3 = (unsigned char)(not_ - (j3 + 1));
    *p++ = 0x5A; *p++ = 0x59; *p++ = 0x58;
    p = inc_hits(p, &hits[HIT_SCALED]);
    *p++ = 0xE9; tagpu_detour_rel(p, S); p += 4;
    return p;
}

/* One site's bytes as stock has them and as we write them. */
typedef struct WRITE { uint32_t va; int n; unsigned char was[10], now[10]; } WRITE;

/* `op target` at the site, the rest NOPs; the rel32 is relative to the site, not to w->now */
static void write_branch(WRITE* w, unsigned char op, uint32_t target)
{
    int32_t rel = (int32_t)(target - (w->va + 5u));
    memset(w->now, 0x90, (size_t)w->n);
    w->now[0] = op;
    memcpy(w->now + 1, &rel, 4);
}

/* THE LEVERS, at attach before the registry closes its lever list (dllmain.c), so a recording lists each:
   the self-checks (tagpu_impai_check.on), the overlay (tagpu_impaiov.on) and the cost run's budgets
   (tagpu_impai_budget.on). Read whatever the install then finds; they act only once it is in. */
void tagpu_impai_levers(void)
{
    QueryPerformanceFrequency(&s_qpf);
    s_install.check = (uint8_t)tagpu_sim_lever("tagpu_impai_check.on");
    impai_overlay_lever();
    if (tagpu_sim_lever("tagpu_impai_budget.on")) {
        s_budget.obs_slice = TAGPU_PK_DESIGN_SLOTS;
        s_budget.radar_visits = 1000000u;
        tagpu_logf("impai: budget lever: slice %u slots, radar %u visits a tick", s_budget.obs_slice,
                   s_budget.radar_visits);
    }
}

/* THE INSTALL, all or none, after tagpu_replay_init and tagpu_reclaim_init. THE LEVEL'S EDGES
   ARE OURS FIRST: nothing goes in unless replay's sites are in, among them the level init's
   PLAYERS call 0x4919C8, the only caller of tagpu_impai_level_init, and the teardown wrap is in,
   the only caller of tagpu_impai_level_end; so no site is in on a level whose seat table nobody
   cleared and wrote, nor between levels with a table nobody cleared. Then every site is
   compared -- the seam 0x465031, the AI command's two calls 0x4162F4 and 0x4162FE and the
   eighteen type tests -- and only when every one is stock are the stubs built and every site
   written; a write that fails puts back every site already written. Otherwise nothing is
   written, s_install.avail stays 0, and each level says why. */
void tagpu_impai_init(void)
{
    enum { NCALLS = 3 };
    WRITE w[NSITES + NCALLS];
    unsigned char* p;
    int i, done;
    if (!tagpu_replay_level_init_in()) {
        lstrcpynA(s_install.why, "the level init 0x4919C8 is not retargeted (the log's `replay:` "
                  "lines say why)", sizeof s_install.why);
        tagpu_logf("impai: unavailable: %s", s_install.why);
        return;
    }
    if (!tagpu_reclaim_level_tracked()) {
        lstrcpynA(s_install.why, "the teardown wrap 0x491B60 is not in (tagpu_reclaim.c), so "
                  "nothing would clear a level's seats", sizeof s_install.why);
        tagpu_logf("impai: unavailable: %s", s_install.why);
        return;
    }
    w[0].va = VA_AI_CALL;
    w[1].va = VA_AI_CMD_AI;
    w[2].va = VA_AI_CMD_HUMAN;
    for (i = 0; i < NCALLS; i++) {
        w[i].n = 5;
        memcpy(w[i].was, (const void*)(uintptr_t)w[i].va, 5);
    }
    for (i = 0; i < NSITES; i++) {
        w[NCALLS + i].va = HSITES[i].va;
        w[NCALLS + i].n = HSITES[i].n;
        memcpy(w[NCALLS + i].was, (const void*)(uintptr_t)HSITES[i].va, HSITES[i].n);
    }
    for (i = 0; i < NSITES + NCALLS; i++) {
        int stock = i == 0 ? tagpu_detour_redirect_ok(VA_AI_CALL, VA_AI_TICK)
                  : i < NCALLS ? tagpu_detour_redirect_ok(w[i].va, VA_SET_TYPE)
                  : !memcmp(w[i].was, HSITES[i - NCALLS].was, (size_t)w[i].n);
        if (!stock) {
            int k, at = wsprintfA(s_install.why, "0x%06X not stock (", (unsigned)w[i].va);
            for (k = 0; k < w[i].n; k++)
                at += wsprintfA(s_install.why + at, k ? " %02X" : "%02X", w[i].was[k]);
            lstrcpyA(s_install.why + at, ")");
            tagpu_logf("impai: unavailable: %s", s_install.why);
            return;
        }
    }
    s_code = (unsigned char*)SIM_ALLOC(s_code, 0x1000);
    if (!s_code) {
        lstrcpynA(s_install.why, "no page for the stubs", sizeof s_install.why);
        tagpu_logf("impai: unavailable: %s", s_install.why);
        return;
    }
    write_branch(&w[0], 0xE8, (uint32_t)(uintptr_t)impai_ai_tick);
    write_branch(&w[1], 0xE8, (uint32_t)(uintptr_t)impai_ai_command);
    write_branch(&w[2], 0xE8, (uint32_t)(uintptr_t)impai_ai_command);
    for (i = 0; i < NSITES; i++) {
        p = s_code + (unsigned)i * STUB_ROOM;
        stub_emit(p, &HSITES[i], s_diag.hits[i]);
        write_branch(&w[NCALLS + i], 0xE9, (uint32_t)(uintptr_t)p);
    }
    for (done = 0; done < NSITES + NCALLS; done++)
        if (!tagpu_detour_write(w[done].va, w[done].now, w[done].n)) break;
    if (done < NSITES + NCALLS) {
        while (done-- > 0) tagpu_detour_write(w[done].va, w[done].was, w[done].n);
        SIM_FREE(s_code, s_code);
        s_code = NULL;
        lstrcpynA(s_install.why, "a site could not be written", sizeof s_install.why);
        tagpu_logf("impai: unavailable: %s", s_install.why);
        return;
    }
    s_install.in = s_install.avail = 1;
    if (s_install.check) impai_check_install();
    /* the addresses handicap_check.py peeks: the seat table, and the counts as 18 pairs of u32,
       scaled then exempt */
    tagpu_logf("impai: seam 0x465031, the AI command 0x4162F4 and 0x4162FE and %d handicap sites "
               "in; seats at 0x%08X, handicap counts at 0x%08X", NSITES,
               (unsigned)(uintptr_t)s_seat, (unsigned)(uintptr_t)s_diag.hits);
}
