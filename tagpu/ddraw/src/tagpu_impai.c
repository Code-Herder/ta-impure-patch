/* tagpu_impai.c -- ImpAI's seam in the engine and its seat table (tagpu_impai.h). */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "tagpu_impai.h"
#include "tagpu_simreg.h"

/* ImpAI marks no seat on a puppet: the wrapper refuses a puppet seat, and on a puppet level the
   level init and the level end only clear the table, so between the two the recording is the
   only writer of the seat table. */
SIM_FEATURE(impai_observe, SIM_PUPPET_OWNER, "impai-observe impai-observe-perm");

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
   bug in the code, and the level-end `work` lines show each tick's largest against its bound. */
#define OBS_SLICE     2048u
#define RADAR_VISITS  32768u
#define CLU_ROWS      16u
#define CLU_FLOOD     (2u * SNAP_PATCH_CAP)

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
    unsigned los_eval[PLAYER_COUNT], los_miss[PLAYER_COUNT];
    unsigned local_eval, local_miss;
    unsigned radar_passes, radar_eval, radar_miss;
    unsigned site_eval, site_miss, site_ok, site_stock_diff, site_type, site_area;
    unsigned char los_said, radar_said, site_said, econ_said[PLAYER_COUNT];
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

/* THE SEAT'S VIEW, once an ImpAI turn through the seat's own site test (WQ_SITE_SEAT):
   - THE ECONOMY LINE, once a level per seat, at its first decision tick from GameTime 590 (GameTime % 15 equal
     to 7 x seat % 15): what the economy op reads of the record, for a peek of the same fields to be held to.
   - THE SITE CHECK, under tagpu_impai_check.on: four sites of the level's smallest plain building type a turn,
     two near the seat's first unit and two anywhere, each tested through the thunk and held to the seat's own
     mapped bit at the block 0x47D2E0 reads (tagpu_worldq_site_mapped): an acceptance where that bit is clear
     is a mismatch. The same site through the stock test (WQ_SITE_ENGINE, the viewed player's bit) counts where
     the two answers differ. The cells come from GameTime and the unit's position alone. */
static void impai_seat_view(int seat)
{
    WQ_ENGINE c, ec;
    WORLDQ q, eq;
    WQ_ECON e;
    WQ_UNIT u;
    WQ_DEF d;
    int t, k, first, last, slot, ux = 0, uz = 0;
    if (!tagpu_worldq_open(&c, &q, seat, WQ_SITE_SEAT, NULL)) return;
    t = q.ops->gametime(&q);
    if (!s_obs.econ_said[seat] && t >= 590 && t % 15 == 7 * seat % 15 && q.ops->economy(&q, &e) == WQ_OK) {
        s_obs.econ_said[seat] = 1;
        tagpu_logf("impai: econ seat=%d t=%d metal=%.9g energy=%.9g metal_max=%.9g energy_max=%.9g metal_in=%.9g "
                   "energy_in=%.9g metal_out=%.9g energy_out=%.9g", seat, t, e.metal, e.energy, e.metal_max,
                   e.energy_max, e.metal_in, e.energy_in, e.metal_out, e.energy_out);
    }
    if (!s_install.check || !tagpu_worldq_open(&ec, &eq, seat, WQ_SITE_ENGINE, NULL)) return;
    if (!s_obs.site_type) {
        s_obs.site_type = WQ_NONE;
        for (k = 1; k < q.ops->ndefs(&q); k++)
            if (q.ops->def(&q, k, &d) && !(d.flags & WQ_DF_MOVER) && !d.kind && d.fx >= 1 && d.fz >= 1 &&
                (s_obs.site_type == WQ_NONE || (unsigned)(d.fx * d.fz) < s_obs.site_area)) {
                s_obs.site_type = (unsigned)k;
                s_obs.site_area = (unsigned)(d.fx * d.fz);
            }
    }
    if (s_obs.site_type == WQ_NONE) return;
    q.ops->seat_units(&q, &first, &last);
    for (slot = first; slot <= last; slot++)
        if (q.ops->unit(&q, slot, &u)) {
            ux = u.pos[0] >> 20;
            uz = u.pos[2] >> 20;
            break;
        }
    for (k = 0; k < 4; k++) {
        const unsigned h = (unsigned)t * 2654435761u + (unsigned)k * 40503u;
        const int cx = k < 2 ? ux + (int)(h % 61u) - 30 : (int)(h % (unsigned)c.w);
        const int cz = k < 2 ? uz + (int)((h >> 8) % 61u) - 30 : (int)((h >> 12) % (unsigned)c.h);
        const int bit = tagpu_worldq_site_mapped(&q, (int)s_obs.site_type, cx, cz);
        int ok, stock;
        if (bit < 0) continue;
        ok = q.ops->site_ok(&q, (int)s_obs.site_type, cx, cz, 0, NULL);
        stock = eq.ops->site_ok(&eq, (int)s_obs.site_type, cx, cz, 0, NULL);
        s_obs.site_eval++;
        s_obs.site_ok += (unsigned)ok;
        s_obs.site_stock_diff += (unsigned)(ok != stock);
        if (ok && !bit) {
            s_obs.site_miss++;
            if (!s_obs.site_said) {
                s_obs.site_said = 1;
                tagpu_logf("impai: check site mismatch seat=%d type=%u cell=%d,%d gametime=%d", seat,
                           s_obs.site_type, cx, cz, t);
            }
        }
    }
}

/* One ImpAI seat's turn, at its record's place in the players phase, after the step's unit tick, projectiles
   and explosions and before the seat's own sight emits (0x465053): positions are this tick's, the seat's own
   counters as its last emits left them; nothing after the players phase in the step writes a position, the
   alive bit, an owner, a type or cloak (engine/sim.md, the step), so what this turn reads of them holds for
   the rest of the step. */
static void impai_observe(int seat)
{
    static const IMPAI_BUDGET budget = { OBS_SLICE, RADAR_VISITS, CLU_ROWS, CLU_FLOOD };
    const IMPAI_ARRAYS arrays = { s_known, (int)(sizeof s_known / sizeof s_known[0]), s_radar_bits[seat],
                                  (int)sizeof s_radar_bits[seat], s_cluster[seat] };
    WQ_ENGINE c;
    WORLDQ q;
    IMPAI_WORK w;
    SNAP_SCRATCH* scr;
    int rc = -1;
    s_diag.calls[seat]++;
    /* the patch flood's arena, freed before return: nothing in it crosses a tick */
    if ((scr = (SNAP_SCRATCH*)tagpu_sim_scratch(sizeof *scr)) && tagpu_worldq_open(&c, &q, seat, WQ_SITE_ENGINE, NULL))
        rc = impai_know_tick(&q, &s_seatst[seat], &arrays, &budget, scr, &w);
    tagpu_sim_scratch_free(scr);
    if (rc < 0) {
        s_obs.refused[seat]++;
        return;
    }
    s_obs.ticks[seat]++;
    if (w.slots > s_obs.slots_max[seat]) s_obs.slots_max[seat] = w.slots;
    if (w.visits > s_obs.visits_max[seat]) s_obs.visits_max[seat] = w.visits;
    if (w.cells > s_obs.cells_max[seat]) s_obs.cells_max[seat] = w.cells;
    if (w.flood > s_obs.flood_max[seat]) s_obs.flood_max[seat] = w.flood;
    if (c.nslots > s_obs.nslots_max) s_obs.nslots_max = c.nslots;
    if ((unsigned)c.w > s_obs.map_w) s_obs.map_w = (unsigned)c.w;
    if (s_install.check) check_los(&c, &q, seat);
    impai_seat_view(seat);
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
                       "flood_max=%u/%u clusters=%u gametime=%u", i, s_obs.ticks[i], s_obs.refused[i],
                       s_obs.slots_max[i], OBS_SLICE + s_obs.nslots_max, s_obs.visits_max[i],
                       RADAR_VISITS + s_obs.nslots_max, s_obs.cells_max[i], CLU_ROWS * s_obs.map_w,
                       s_obs.flood_max[i], CLU_FLOOD, s_seatst[i].nclusters, now);
    if (!s_install.check) return;
    for (i = 0; i < PLAYER_COUNT; i++)
        if (s_obs.ticks[i])
            tagpu_logf("impai: check los seat=%u evaluated=%u mismatch=%u", i, s_obs.los_eval[i], s_obs.los_miss[i]);
    tagpu_logf("impai: check los-local evaluated=%u mismatch=%u", s_obs.local_eval, s_obs.local_miss);
    tagpu_logf("impai: check radar passes=%u evaluated=%u mismatch=%u", s_obs.radar_passes, s_obs.radar_eval,
               s_obs.radar_miss);
    tagpu_logf("impai: check site evaluated=%u mismatch=%u accepted=%u stock_differs=%u", s_obs.site_eval,
               s_obs.site_miss, s_obs.site_ok, s_obs.site_stock_diff);
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
    for (c = 0, n = 0; c < WQ_MCLASSES; c++) {
        if (!(s_reach_ix.present >> c & 1u)) continue;
        s_reach_ix.at[c] = (uint32_t)(n * blocks);
        comps += (unsigned)impai_reach_label(reach_pass, NULL, (int)c, w, h, s_reach + n * blocks, work);
        n++;
    }
    tagpu_sim_scratch_free(work);
    s_reach_n = (unsigned)(n * blocks);
    tagpu_logf("impai: reach classes=%u blocks=%ux%u components=%u", n, s_reach_ix.bw, s_reach_ix.bh, comps);
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
SIM_FEATURE(impai_seats, SIM_PUPPET_OWNER, "impai-handicap impai-observe impai-observe-perm");

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
    impai_observe_clear();
    impai_reach_build();
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

/* THE SEAT'S OWN SITE TEST. 0x47D2E0(def, cell, ignore, player) refuses a site whose centre the VIEWED player
   has not mapped, `mov cl,[ebx+0x2A43]` at 0x47D3D1 loading the bit index whatever record it is passed
   (DISASSEMBLED 0x47D3C4..0x47D3FA; tagpu_worldq.c, wq_site_ok). ImpAI calls it only through a thunk, and the
   instruction at 0x47D3D1 tells that caller apart by its return address:

     thunk:  push dword [esp+0x10]  x4     player, ignore, cell, def, re-pushed in order
             call 0x47D2E0
     ret_va: ret 0x10                      stdcall, as the engine function

     0x47D3D1 (8A 8B 43 2A 00 00 -> jmp stub; nop):
     stub:   cmp dword [esp+0x3C], ret_va
             jne stock
             mov cl,[edi+0x146]            the passed record's own mapped bit
             jmp 0x47D3D7
     stock:  mov cl,[ebx+0x2A43]           the stock instruction
             jmp 0x47D3D7

   THE INVARIANT is an identity, not a state. At 0x47D3D1 [esp+0x3C] is the function's return address: it
   opens with `sub esp,0x2C` and four pushes (0x47D2E0..0x47D306), and its one call before the site, 0x485010
   at 0x47D385, pops its own argument (`ret 4`, 0x48505F), so nothing else is on the stack (DISASSEMBLED). That
   address is ret_va only for a call from the thunk, and only the WQ_SITE_SEAT view calls the thunk; every other
   caller -- the build cursor 0x4198C6, the AI's spot search 0x40A4CD, the placement test 0x47DBE5 (scout I14)
   -- reads main+0x2A43 as stock does. No static is read, so nothing crosses a tick or a thread. EDI holds the
   record the caller passed from 0x47D358 on, the seat's own (ctx->rec), whose +0x146 is its mapped bit
   (the activation 0x464290; the stamp 0x481930 writes 1 << rec+0x146); the LOS-shear fix's row at
   0x47D3B8..0x47D3C3 (tagpu_patches.c, fix_los_local) ends before 0x47D3C4 and leaves EDI alone, so the two do
   not overlap. ECX is the loaded register in both arms; the flags the `cmp` leaves are written again by the
   `add` at 0x47D3EE before the first read, the `je` at 0x47D3FA (the `shl` at 0x47D3D7 writes none when CL
   is 0). The occupancy flag that follows reads the passed record's sight counters under True line of sight
   (0x47D40C..0x47D43A), which is what a human in the seat gets. */
#define VA_SITE_TEST  0x0047D2E0u
#define VA_SITE_BIT   0x0047D3D1u   /* `mov cl,[ebx+0x2A43]`, 8A 8B 43 2A 00 00 */
#define VA_SITE_AFTER 0x0047D3D7u
static const uint8_t SITE_BIT_WAS[6] = { 0x8A, 0x8B, 0x43, 0x2A, 0x00, 0x00 };
#define SEAT_SITE_ROOM 64u          /* the thunk and the stub: 24 and 32 bytes */
_Static_assert(18 * STUB_ROOM + SEAT_SITE_ROOM <= 0x1000, "the thunk and its stub fit after the handicap stubs");

/* the thunk at `p`, the stub after it; returns the stub */
static unsigned char* seat_site_emit(unsigned char* p)
{
    unsigned char *stub, *ret_va;
    int k;
    for (k = 0; k < 4; k++) { *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x10; }
    *p++ = 0xE8; tagpu_detour_rel(p, VA_SITE_TEST); p += 4;
    ret_va = p;
    *p++ = 0xC2; *p++ = 0x10; *p++ = 0x00;
    stub = p;
    *p++ = 0x81; *p++ = 0x7C; *p++ = 0x24; *p++ = 0x3C; p = put32(p, (uint32_t)(uintptr_t)ret_va);
    *p++ = 0x75; *p++ = 0x0B;
    *p++ = 0x8A; *p++ = 0x8F; p = put32(p, 0x146);
    *p++ = 0xE9; tagpu_detour_rel(p, VA_SITE_AFTER); p += 4;
    memcpy(p, SITE_BIT_WAS, sizeof SITE_BIT_WAS); p += sizeof SITE_BIT_WAS;
    *p++ = 0xE9; tagpu_detour_rel(p, VA_SITE_AFTER);
    return stub;
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

/* THE INSTALL, all or none, after tagpu_replay_init and tagpu_reclaim_init. THE LEVEL'S EDGES
   ARE OURS FIRST: nothing goes in unless replay's sites are in, among them the level init's
   PLAYERS call 0x4919C8, the only caller of tagpu_impai_level_init, and the teardown wrap is in,
   the only caller of tagpu_impai_level_end; so no site is in on a level whose seat table nobody
   cleared and wrote, nor between levels with a table nobody cleared. Then every site is
   compared -- the seam 0x465031, the AI command's two calls 0x4162F4 and 0x4162FE, the
   eighteen type tests and the site test's bit at 0x47D3D1 -- and only when every one is stock
   are the stubs built and every site written; a write that fails puts back every site already
   written. Only then does WQ_SITE_SEAT get its thunk. Otherwise nothing is written,
   s_install.avail stays 0, and each level says why. */
void tagpu_impai_init(void)
{
    enum { NCALLS = 3, NALL = NCALLS + NSITES + 1, SEAT_SITE = NALL - 1 };
    WRITE w[NALL];
    unsigned char *p, *thunk;
    int i, done;
    s_install.check = (uint8_t)tagpu_sim_lever("tagpu_impai_check.on");
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
    w[SEAT_SITE].va = VA_SITE_BIT;
    w[SEAT_SITE].n = (int)sizeof SITE_BIT_WAS;
    memcpy(w[SEAT_SITE].was, (const void*)(uintptr_t)VA_SITE_BIT, sizeof SITE_BIT_WAS);
    for (i = 0; i < NALL; i++) {
        int stock = i == 0 ? tagpu_detour_redirect_ok(VA_AI_CALL, VA_AI_TICK)
                  : i < NCALLS ? tagpu_detour_redirect_ok(w[i].va, VA_SET_TYPE)
                  : i == SEAT_SITE ? !memcmp(w[i].was, SITE_BIT_WAS, sizeof SITE_BIT_WAS)
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
    thunk = s_code + (unsigned)NSITES * STUB_ROOM;
    write_branch(&w[SEAT_SITE], 0xE9, (uint32_t)(uintptr_t)seat_site_emit(thunk));
    for (done = 0; done < NALL; done++)
        if (!tagpu_detour_write(w[done].va, w[done].now, w[done].n)) break;
    if (done < NALL) {
        while (done-- > 0) tagpu_detour_write(w[done].va, w[done].was, w[done].n);
        SIM_FREE(s_code, s_code);
        s_code = NULL;
        lstrcpynA(s_install.why, "a site could not be written", sizeof s_install.why);
        tagpu_logf("impai: unavailable: %s", s_install.why);
        return;
    }
    tagpu_worldq_seat_site_set((WqSiteFn)(uintptr_t)thunk);
    s_install.in = s_install.avail = 1;
    if (s_install.check) impai_check_install();
    /* the addresses handicap_check.py peeks: the seat table, and the counts as 18 pairs of u32,
       scaled then exempt */
    tagpu_logf("impai: seam 0x465031, the AI command 0x4162F4 and 0x4162FE, %d handicap sites and the "
               "seat's site test 0x47D3D1 in; seats at 0x%08X, handicap counts at 0x%08X", NSITES,
               (unsigned)(uintptr_t)s_seat, (unsigned)(uintptr_t)s_diag.hits);
}
