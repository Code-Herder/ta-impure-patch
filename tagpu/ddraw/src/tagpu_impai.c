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
#include "impai_seats.h"
#include "impai_mil.h"
#include "tagpu_resource.h"       /* tagpu_buildmenu_types: the build menus */
#include "tagpu_recorder.h"       /* the AI activity bracket around a turn */

#define VA_AI_CALL   0x00465031u   /* the players phase's `call 0x408C40`, E8 0A 3C FA FF */
#define VA_AI_TICK   0x00408C40u   /* the stock AI tick, thiscall, no stack argument      */
#define PL_ACTIVE    0x00          /* u32: nonzero for a seated record                   */
#define PL_CONTROL   0x73          /* u8: 1 human, 2 AI, 3 remote or puppet              */
#define PL_AIOBJ     0x74          /* the AI object 0x408C40 runs on; the players phase
                                      calls it only when set (0x46502A..0x465031)         */
#define PI_TYPE      0x94          /* PlayerInfo's u8 type: 2 for an AI seat, on a remote
                                      AI's type-3 record too (0x45490F..0x45493B)         */
#define PL_NAME      0x2B          /* the long name, 30 bytes: an ImpAI seat's begins ImpAI:
                                      (impai_seats.h, WHY THE NAME)                       */
#define OFF_DIFFICULTY 0x37EEE     /* u32: 0 Easy, 1 Medium, 2 Hard (engine/sim.md)      */
#define OFF_NETFLAGS 0x2A44        /* u8, bit 0: a network game (engine/ui.md, the network
                                      bit: set in the front end only, cleared in play)    */
#define CTRL_AI      2

#define IMPAI_ON     0x01u

/* Which records ImpAI plays, by record index. No level sees another's marks: the level end
   clears the table in the teardown, and the level init clears it again on every level, live or
   puppet, on the loader thread before anything of the level reads it (the stubs' ordering,
   below), which also covers a teardown tagpu_reclaim.off leaves unwrapped. Then the level init
   marks a live level's seats; on a puppet level, after that clear, the recording is the only
   writer, its first restore running on the game thread at the level's first step (the loader's
   done bit orders the two: impai_seat_marked, THE THREAD). A recording that carries no row of this
   table leaves it clear. */
static uint8_t s_seat[PLAYER_COUNT];
SIM_ROWS(s_seat, SIM_STATE, SIM_ID_PLAYER, NULL);

/* The handicap rows keep the ids of engine/sim.md's table, 1 to 18, though rows 15 and 16, the
   Deathmatch respawn's two grants, have no site: no ImpAI seat can reach them (HSITES). */
#define IMPAI_ROWS 18
#define IMPAI_HSITES 16        /* the fifteen Difficulty tests ImpAI exempts and 0x406FC3 */
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
   AI seat) and the unscaled branch for an ImpAI mark, indexed by row id less one, so the pairs
   of rows 15 and 16 stay 0. The stubs increment `hits` with a plain
   `inc` on whichever thread reaches the site; the threads are ordered as the table's writes are
   (the stubs' ordering), but nothing makes the counts exact against a network load's concurrent
   removal: they are diagnostics the simulation never reads. */
static struct {
    unsigned calls[PLAYER_COUNT];
    unsigned hits[IMPAI_ROWS][2];           /* [HIT_SCALED], [HIT_EXEMPT] */
} s_diag;
SIM_VAR(s_diag, SIM_PRIVATE);

typedef void (__thiscall* PFN_AiTick)(void* obj);

static void handicap_counts(char* line);

/* The seat ImpAI plays at this AI object, or -1. Every test must hold:
   - THE BOUND. The AI object's +0 is its record (the constructor 0x408CB0 stores it at
     0x408CBB, DISASSEMBLED); that value is data until it is one of main's ten records, which the
     offset's range and exact division by the stride establish without dereferencing it.
   - THE SEAT. The record is seated with controller 2 -- the test 0x408C40 makes before it plans
     (0x408C45..0x408C50) -- marked in the seat table, and not a puppet seat.
   THE THREAD is the caller's to tell apart (impai_ai_tick): the players phase runs on the game
   thread every step and once on the loader thread, at 0x497C10 at the end of a level load
   (DISASSEMBLED). Both read a whole table. The loader thread's call comes after the level init on
   that same thread; every game-thread call is ordered after it by the engine's own handshake: the
   init runs inside the loader's PLAYERS call 0x4919C8, before the loader's last store sets bit 1
   of main+0x38D75 (0x497C62), and the game thread runs no step until it has read that bit set
   (0x498342; tagpu_replay.c, THE VIEWER'S BUFFERS). The bit itself is no per-call gate: 0x498480
   clears the word after the level's first in-game frame (tagpu_engine.h, OFF_LOADFLAGS). */
static int impai_seat_marked(const void* obj)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const char* rec;
    uintptr_t off;
    unsigned i;
    if (!ta) return -1;
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

/* each seat's radar pass's record of every slot whose bit it set (worldq.h, WQ_RADAR_HIT), keyed by slot as
   s_known is, every seat's record of the slot in its row (impai.h, IMPAI_HITROW): the unit's
   incarnation (tagpu_unit_birth) and block when the pass set it, so the publish takes a contact only for the
   unit the pass found and at the block it found it (impai.h, A RADAR CONTACT IS THE UNIT THE PASS FOUND).
   Without the fail-closed table there is no incarnation, and the seat publishes no radar contact at all.
   A heap block, so the recorder write-watches it and images only the pages a pass wrote: the attach's own
   allocation (tagpu_impai_init), which starts on a page and shares none, and is never freed.
   BOUND: TAGPU_PK_DESIGN_SLOTS rows on every level, a fixed size and no count, so every level records and
   restores the same rows: a replay restores a heap table's rows within the size the host holds and never
   writes a count that sizes one (tagpu_replay.c, count_kept), so a count would stay at whatever the puppet's
   own level init set. The level init clears every row (impai_observe_clear). With no block (ImpAI
   unavailable) the table has no rows and the passes no records. */
static IMPAI_HITROW* s_radar_hit;
SIM_BLOCK(s_radar_hit, SIM_STATE, SIM_ID_UNIT_SLOT, TAGPU_PK_DESIGN_SLOTS, NULL);
#define RADAR_HIT_ROWS (s_radar_hit ? TAGPU_PK_DESIGN_SLOTS : 0u)

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
   the map and its features at PLAYERS, rebuilt on every level a seat can play ImpAI on, live or puppet, and
   none on any other (tagpu_impai_level_init, THE CONTENT). */
static uint16_t* s_reach;
static unsigned  s_reach_n;
SIM_HEAP(s_reach, SIM_CONTENT, SIM_ID_NONE, sizeof(uint16_t), s_reach_n, NULL);
SIM_VAR(s_reach_n, SIM_CONTENT);
/* where each class's labels start in s_reach (present: bit c), the block grid, and the summary the naval rule
   reads (impai.h, IMPAI_REACH; impai_reach_summarise) */
static struct {
    uint32_t at[WQ_MCLASSES];
    uint32_t present;
    uint16_t bw, bh;
    uint32_t sea_blocks, land_total;
    uint16_t land_w[WQ_MCLASSES];
    uint8_t  sea_ref;
} s_reach_ix;
SIM_VAR(s_reach_ix, SIM_CONTENT);
/* THE BLOCK TOPS, bw x bh bytes beside the labels: each block's highest cell byte +0x05 (cell_top), which the sea
   plant's depth filter reads (impai_build.c, THE SEA PLANT). Content, built and freed with s_reach. */
static uint8_t* s_reach_top;
static unsigned s_reach_top_n;
SIM_HEAP(s_reach_top, SIM_CONTENT, SIM_ID_NONE, 1, s_reach_top_n, NULL);
SIM_VAR(s_reach_top_n, SIM_CONTENT);

/* What a turn did, for the cost's tail: the radar pass's publish, the patch scan's last row (its patches
   published), a decision; PLAIN when none of them. */
enum { TAIL_PUBLISH, TAIL_SCAN, TAIL_DECIDE, TAIL_PLAIN, TAIL_CLASSES };
#define TAIL_US 1000u              /* a slow turn: the owner's bound, 1 ms a seat a tick */
static const char* const TAIL_NAME[TAIL_CLASSES] = { "publish", "scan", "decide", "plain" };

/* The level's work and the self-checks' counts, for the level-end lines and tools/impai_check.py. */
static struct {
    unsigned ticks[PLAYER_COUNT], refused[PLAYER_COUNT];
    unsigned slots_max[PLAYER_COUNT], visits_max[PLAYER_COUNT], nslots_max;
    unsigned cells_max[PLAYER_COUNT], flood_max[PLAYER_COUNT], map_w;
    unsigned scans[PLAYER_COUNT];             /* patch scan passes completed and published         */
    /* THE COST, MEASURED: QueryPerformanceCounter around a turn (its observation and its decision, not the
       self-checks nor the weapon scan), each seat's largest, its sum and a log2 histogram of microseconds,
       bucket k holding [2^k, 2^(k+1)) and bucket 0 [0, 2). A measurement for the cost line, never a verdict:
       the gate judges the counts.
       THE TAIL, by what each turn did (TAIL_*): per class its turns, those of at least TAIL_US, its largest
       and its sum, a turn counted in every class it belongs to; and the slowest turn's counts. A slow turn
       that published nothing, scanned no last row and made no decision did only its slice and a bounded
       radar step, so its time is not its work's: the OS's (preemption, a page fault). */
    unsigned cost_max[PLAYER_COUNT], cost_hist[PLAYER_COUNT][24];
    double   cost_sum[PLAYER_COUNT];
    struct { unsigned turns, over, max; double sum; } tail[PLAYER_COUNT][TAIL_CLASSES];
    struct { unsigned us, gametime, flags, slots, visits, cells, flood; } worst[PLAYER_COUNT];
    unsigned los_eval[PLAYER_COUNT], los_miss[PLAYER_COUNT];
    unsigned local_eval, local_miss;
    unsigned radar_passes, radar_eval, radar_miss;
    unsigned radar_stale;               /* IMPAI_WORK.stale over every ImpAI seat's publishes: 0 */
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

#define PL_PASS_DUE 0xF0          /* u32: the GameTime of the record's next 30-tick block (engine/recorder.md) */

/* THE SEAT'S VIEW, once an ImpAI turn through the seat's own site test (WQ_SITE_SEAT):
   - THE ECONOMY LINE, once a level per seat, at its first decision tick from GameTime 590 at which no economy
     pass of the record is due this step: what the economy op reads of the record, for a peek of the same fields
     to be held to. THE ORDERING: the players phase reaches this turn at 0x465031 and the record's own 30-tick
     block, whose economy pass 0x401360 writes these fields (0x465539..0x46555A), later in the same iteration,
     when GameTime has reached +0xF0 (0x46507D..0x465089; +0xF0 += 30 at 0x46508F; DISASSEMBLED,
     engine/recorder.md). So with +0xF0 past GameTime nothing of the pass runs after the line in this step, and
     a peek held at the end of any step from `t` to `pass` - 1 reads what the line read; the line names both.
   - THE SITE CHECK, under tagpu_impai_check.on: four sites of the level's smallest plain building type a turn,
     two near the seat's first unit and two anywhere, each tested through the thunk and held to the seat's own
     mapped bit at the block 0x47D2E0 reads (tagpu_worldq_site_mapped): an acceptance where that bit is clear
     is a mismatch. The same site through the stock test (WQ_SITE_ENGINE, the viewed player's bit) counts where
     the two answers differ. The cells come from GameTime and the unit's position alone. */
static const IMPAI_LEVEL* seat_level(int seat);

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
    if (!s_obs.econ_said[seat] && t >= 590 && impai_brain_due((unsigned)t, (unsigned)seat, seat_level(seat)) &&
        *(const uint32_t*)(c.rec + PL_PASS_DUE) > (uint32_t)t && q.ops->economy(&q, &e) == WQ_OK) {
        s_obs.econ_said[seat] = 1;
        tagpu_logf("impai: econ seat=%d t=%d pass=%u metal=%.9g energy=%.9g metal_max=%.9g energy_max=%.9g "
                   "metal_in=%.9g energy_in=%.9g metal_out=%.9g energy_out=%.9g", seat, t,
                   *(const uint32_t*)(c.rec + PL_PASS_DUE), e.metal, e.energy, e.metal_max, e.energy_max,
                   e.metal_in, e.energy_in, e.metal_out, e.energy_out);
    }
    if (!s_install.check || !tagpu_worldq_open(&ec, &eq, seat, WQ_SITE_ENGINE, NULL)) return;
    if (!s_obs.site_type) {
        s_obs.site_type = WQ_NONE;
        for (k = 1; k < q.ops->ndefs(&q); k++)
            if (q.ops->def(&q, k, &d) && !(d.flags & WQ_DF_MOVER) && d.yard && !d.kind && d.fx >= 1 && d.fz >= 1 &&
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

static LARGE_INTEGER s_qpf;            /* the counter's frequency, taken at attach */
SIM_VAR(s_qpf, SIM_INSTALL);

/* One turn's time: `flags` bit TAIL_* for each class the turn belongs to, `w` its observation's counts. */
static void cost_note(int seat, long long ticks, unsigned flags, const IMPAI_WORK* w, unsigned gametime)
{
    const double us = s_qpf.QuadPart > 0 ? (double)ticks * 1e6 / (double)s_qpf.QuadPart : 0.0;
    const unsigned u = us < 4e9 ? (unsigned)us : 0xFFFFFFFFu;
    unsigned k = 0, v = u;
    while (v > 1u && k < 23u) { v >>= 1; k++; }
    s_obs.cost_hist[seat][k]++;
    s_obs.cost_sum[seat] += us;
    if (u > s_obs.cost_max[seat]) s_obs.cost_max[seat] = u;
    if (!flags) flags = 1u << TAIL_PLAIN;
    for (k = 0; k < TAIL_CLASSES; k++)
        if (flags >> k & 1u) {
            s_obs.tail[seat][k].turns++;
            s_obs.tail[seat][k].over += u >= TAIL_US;
            s_obs.tail[seat][k].sum += us;
            if (u > s_obs.tail[seat][k].max) s_obs.tail[seat][k].max = u;
        }
    if (u >= s_obs.worst[seat].us) {
        s_obs.worst[seat].us = u;
        s_obs.worst[seat].gametime = gametime;
        s_obs.worst[seat].flags = flags;
        s_obs.worst[seat].slots = w->slots;
        s_obs.worst[seat].visits = w->visits;
        s_obs.worst[seat].cells = w->cells;
        s_obs.worst[seat].flood = w->flood;
    }
}

/* One ImpAI seat's observation, at its record's place in the players phase, after the step's unit tick,
   projectiles and explosions and before the seat's own sight emits (0x465053): positions are this tick's, the
   seat's own counters as its last emits left them; nothing after the players phase in the step writes a
   position, the alive bit, an owner, a type or cloak (DISASSEMBLED for the direct calls, [INFERRED for indirect
   calls]: engine/sim.md, the step), so what this turn reads of them holds for the rest of the step. `scr` is
   the turn's patch-flood arena. 0, or -1 when the view could not be opened or the knowledge tick refused it. */
static int impai_observe(int seat, SNAP_SCRATCH* scr, IMPAI_WORK* w)
{
    const IMPAI_ARRAYS arrays = { s_known, (int)(sizeof s_known / sizeof s_known[0]), s_radar_bits[seat],
                                  (int)sizeof s_radar_bits[seat],
                                  { s_radar_hit ? &s_radar_hit[0].seat[seat] : NULL, (int)RADAR_HIT_ROWS,
                                    (unsigned)sizeof(IMPAI_HITROW), s_install.check },
                                  s_cluster[seat] };
    WQ_ENGINE c;
    WORLDQ q;
    memset(w, 0, sizeof *w);
    if (!tagpu_worldq_open(&c, &q, seat, WQ_SITE_ENGINE, NULL) ||
        impai_know_tick(&q, &s_seatst[seat], &arrays, &s_budget, scr, w) < 0) {
        s_obs.refused[seat]++;
        return -1;
    }
    s_obs.ticks[seat]++;
    if (w->slots > s_obs.slots_max[seat]) s_obs.slots_max[seat] = w->slots;
    if (w->visits > s_obs.visits_max[seat]) s_obs.visits_max[seat] = w->visits;
    if (w->cells > s_obs.cells_max[seat]) s_obs.cells_max[seat] = w->cells;
    if (w->flood > s_obs.flood_max[seat]) s_obs.flood_max[seat] = w->flood;
    s_obs.scans[seat] += w->scans;
    if (c.nslots > s_obs.nslots_max) s_obs.nslots_max = c.nslots;
    if ((unsigned)c.w > s_obs.map_w) s_obs.map_w = (unsigned)c.w;
    s_obs.radar_stale += w->stale;
    return 0;
}

/* The turn's self-checks, after its timed work: line of sight under the check lever, and the seat's view. */
static void impai_observe_checks(int seat)
{
    WQ_ENGINE c;
    WORLDQ q;
    if (s_install.check && tagpu_worldq_open(&c, &q, seat, WQ_SITE_ENGINE, NULL)) check_los(&c, &q, seat);
    impai_seat_view(seat);
}

#define VA_RADAR_CALL 0x0046556Du   /* the players phase's `call 0x467440`, E8 CE 1E 00 00 */
#define VA_RADAR_PASS 0x00467440u   /* the radar pass: no argument, plain `ret`            */

/* THE RADAR CHECK, the target of the call at 0x46556D under tagpu_impai_check.on alone: the engine's pass
   runs, then ImpAI's copy (tagpu_worldq.c, wq_radar_step) for the same record, unbudgeted, and every live
   unit's +0x110 bits 8 and 9 are compared with the copy's, and bit 10 where the pass's first loop cleared it
   (WQ_RB_KEPT clear: an own or shared unit's bit 10 is older than the pass), and each bit's record with the
   unit and block of now -- but for the seat's own units, whose record holds the incarnation alone
   (worldq.h, WQ_RADAR_HIT). The site is reached only in the
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
    WQ_HITS hits = { NULL, 0, (unsigned)sizeof(WQ_RADAR_HIT), 0 };
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
    hits.n = (int)c.nslots;
    if (!(hits.at = (WQ_RADAR_HIT*)tagpu_sim_scratch(c.nslots * sizeof *hits.at))) {
        tagpu_sim_scratch_free(bits);
        return;
    }
    memset(&st, 0, sizeof st);
    if (q.ops->radar_step(&q, &st, bits, (int)bytes, &hits, &budget)) {
        s_obs.radar_passes++;
        for (slot = 1; slot < c.nslots; slot++) {
            const unsigned state = *(const unsigned*)(c.units + slot * UNIT_STRIDE + U_STATE);
            const unsigned v = wq_radar_bits(bits, slot);
            const unsigned mask = v & WQ_RB_KEPT ? 3u : 7u;
            const WQ_RADAR_HIT* h = wq_hit(&hits, slot);
            WQ_OBS o;
            /* a pass in one call: each bit's record is the unit and block of now */
            const int seen = (v & 3u) ? q.ops->observe(&q, (int)slot, 1, &o) : 0;
            const int rec_ok = !(v & 3u) || seen == WQ_SEEN_OWN ||
                               (seen && h->known && o.birth_known && o.birth == h->birth && o.bx == h->bx &&
                                o.bz == h->bz);
            if (!(state & ST_ALIVE)) continue;
            s_obs.radar_eval++;
            if (((state >> 8) & mask) != (v & mask) || !rec_ok) {
                s_obs.radar_miss++;
                if (!s_obs.radar_said) {
                    s_obs.radar_said = 1;
                    tagpu_logf("impai: check radar mismatch slot=%u ours=%X engine=%X record=%d gametime=%d", slot,
                               v, (state >> 8) & 7u, rec_ok, *(const int*)(ta + OFF_GAMETIME));
                }
            }
        }
    }
    tagpu_sim_scratch_free(hits.at);
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
            /* each class as <name>=<turns of TAIL_US or more>/<turns>:<largest us>:<mean us> */
            at = 0;
            for (k = 0; k < TAIL_CLASSES; k++)
                at += _snprintf(hist + at, sizeof hist - at, "%s%s=%u/%u:%u:%.1f", k ? " " : "", TAIL_NAME[k],
                                s_obs.tail[i][k].over, s_obs.tail[i][k].turns, s_obs.tail[i][k].max,
                                s_obs.tail[i][k].sum / (s_obs.tail[i][k].turns ? s_obs.tail[i][k].turns : 1));
            hist[sizeof hist - 1] = 0;
            tagpu_logf("impai: tail seat=%u over_us=%u %s worst_us=%u gametime=%u publish=%u scan=%u decide=%u "
                       "slots=%u visits=%u cells=%u flood=%u", i, TAIL_US, hist, s_obs.worst[i].us,
                       s_obs.worst[i].gametime, s_obs.worst[i].flags >> TAIL_PUBLISH & 1u,
                       s_obs.worst[i].flags >> TAIL_SCAN & 1u, s_obs.worst[i].flags >> TAIL_DECIDE & 1u,
                       s_obs.worst[i].slots, s_obs.worst[i].visits, s_obs.worst[i].cells, s_obs.worst[i].flood);
        }
    if (!s_install.check) return;
    for (i = 0; i < PLAYER_COUNT; i++)
        if (s_obs.ticks[i])
            tagpu_logf("impai: check los seat=%u evaluated=%u mismatch=%u", i, s_obs.los_eval[i], s_obs.los_miss[i]);
    impai_patches_dump();
    tagpu_logf("impai: check los-local evaluated=%u mismatch=%u", s_obs.local_eval, s_obs.local_miss);
    tagpu_logf("impai: check radar passes=%u evaluated=%u mismatch=%u stale=%u", s_obs.radar_passes,
               s_obs.radar_eval, s_obs.radar_miss, s_obs.radar_stale);
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
    if (s_radar_hit) memset(s_radar_hit, 0, (size_t)TAGPU_PK_DESIGN_SLOTS * sizeof *s_radar_hit);
    memset(s_cluster, 0, sizeof s_cluster);
    memset(&s_obs, 0, sizeof s_obs);
}

static int reach_pass(const void* ctx, int mclass, int cx, int cz)
{
    (void)ctx;
    return tagpu_worldq_passable_at((unsigned)mclass, cx, cz);
}

/* THE CELL'S TOP, byte +0x05 of the feature grid's cell (z W + x) x FT_STRIDE at [main+0x14287]: the most of the
   heights at the cell and its right, lower and lower-right neighbours, written by 0x483210 (engine/map.md, the
   +0x04..+0x07 row) -- the very bytes the site test compares a water building's depth against (0x47D521,
   0x47D7D7..0x47D7F0; impai_build.c, THE SEA PLANT). BOUND: W and H are the engine's, held as impai_reach_build
   holds them; the last column's and the last row's bytes are never written by 0x483210 (CRT malloc garbage), so
   those cells, and any with no grid, read 255, the top no water building passes. */
typedef struct CELLTOP { const unsigned char* grid; int w, h; } CELLTOP;
static int cell_top(const void* ctx, int cx, int cz)
{
    const CELLTOP* g = (const CELLTOP*)ctx;
    if (!g->grid || cx < 0 || cz < 0 || cx >= g->w - 1 || cz >= g->h - 1) return 255;
    return g->grid[((size_t)cz * (unsigned)g->w + (unsigned)cx) * FT_STRIDE + 5];
}

/* THE CONTENT CHECK, under the check lever, here at the level init where no unit stands yet: every entry of every
   class the level has, the content map ImpAI's way out reads (worldq.h, passable; tagpu_worldq.c, THE CONTENT MAP)
   against the engine's own (tagpu_worldq_passable_at), in windows of PASS_WIN entries a side. Logs `impai: check
   pass classes=<n> evaluated=<entries> mismatch=<n>`, and the first mismatch's class, cell and both codes, which
   impai_check.py (a) judges. Its scratch is freed before it returns; a level without it logs none. */
#define PASS_WIN 64
static void impai_pass_check(int w, int h)
{
    WQ_ENGINE c;
    WORLDQ q;
    uint8_t* win;
    unsigned cls, n = 0, eval = 0, miss = 0;
    int x0, z0, x, z;
    if (!tagpu_worldq_open_content(&c, &q) || !(win = (uint8_t*)tagpu_sim_scratch(WQ_PASS_CAP(PASS_WIN, PASS_WIN))))
        return;
    for (cls = 0; cls < WQ_MCLASSES; cls++) {
        if (!tagpu_worldq_class_exists(cls)) continue;
        n++;
        for (z0 = 0; z0 < h; z0 += PASS_WIN)
            for (x0 = 0; x0 < w; x0 += PASS_WIN) {
                const int ww = w - x0 < PASS_WIN ? w - x0 : PASS_WIN, wh = h - z0 < PASS_WIN ? h - z0 : PASS_WIN;
                if (!q.ops->passable(&q, (int)cls, x0, z0, ww, wh, win, WQ_PASS_CAP(PASS_WIN, PASS_WIN))) continue;
                for (z = 0; z < wh; z++)
                    for (x = 0; x < ww; x++) {
                        const int mine = win[z * ww + x], theirs = tagpu_worldq_passable_at(cls, x0 + x, z0 + z);
                        eval++;
                        if (mine == theirs) continue;
                        if (!miss++)
                            tagpu_logf("impai: check pass mismatch class=%u cell=%d,%d content=%d engine=%d", cls,
                                       x0 + x, z0 + z, mine, theirs);
                    }
            }
    }
    tagpu_sim_scratch_free(win);
    tagpu_logf("impai: check pass classes=%u evaluated=%u mismatch=%u", n, eval, miss);
}

/* THE REACH BUILD, at the level init on the loader thread, after 0x440940 built the class maps at 0x4918E3
   (tagpu_worldq_passable_at): the labels of every class the level has, into one block sized for them. The
   previous level's block goes first, so no level's labels outlive it. BOUND: the map's W and H are the
   engine's, each held to the coordinate limit (2048 cells); the labels take (W/2)(H/2) u16 a class. A level
   whose block or BFS scratch cannot be had has no reach, said once in the log. BOUND: a label names one
   component only below IMPAI_REACH_MANY (impai_reach_label stores every later one as that), so a class with as
   many components is dropped from `present`, logged, and every class a reach test reads is one whose equal
   labels mean connected. The block tops (cell_top) are built beside the labels; without their memory the level
   has none, logged, and no seat places a sea plant. */
static void impai_reach_build(void)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    unsigned c, n = 0, comps = 0;
    int w, h;
    size_t blocks;
    void* work;
    CELLTOP g;
    LARGE_INTEGER t0, t1;
    if (s_reach) SIM_FREE(s_reach, s_reach);
    s_reach = NULL;
    s_reach_n = 0;
    if (s_reach_top) SIM_FREE(s_reach_top, s_reach_top);
    s_reach_top = NULL;
    s_reach_top_n = 0;
    memset(&s_reach_ix, 0, sizeof s_reach_ix);
    s_reach_ix.sea_ref = 0xFF;
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
        int k;
        if (!(s_reach_ix.present >> c & 1u)) continue;
        s_reach_ix.at[c] = (uint32_t)(n * blocks);
        k = impai_reach_label(reach_pass, NULL, (int)c, w, h, s_reach + n * blocks, work);
        if ((unsigned)k >= IMPAI_REACH_MANY) {
            s_reach_ix.present &= ~(1u << c);
            tagpu_logf("impai: reach class %u dropped: %d components, past the labels' %u", c, k, IMPAI_REACH_MANY);
        }
        comps += (unsigned)k;
        n++;
    }
    QueryPerformanceCounter(&t1);
    tagpu_sim_scratch_free(work);
    if (s_install.check) impai_pass_check(w, h);
    s_reach_n = (unsigned)(n * blocks);
    g.grid = *(const unsigned char* const*)(ta + OFF_FEATMAP);
    g.w = w;
    g.h = h;
    if ((s_reach_top = (uint8_t*)SIM_ALLOC(s_reach_top, blocks))) {
        impai_reach_top(cell_top, &g, w, h, s_reach_top);
        s_reach_top_n = (unsigned)blocks;
    } else
        tagpu_logf("impai: reach tops none: %u blocks need memory not had", (unsigned)blocks);
    /* the work in counts -- each class labels every cell once and reads its four neighbours, then every
       block -- and, as a measurement, its time */
    tagpu_logf("impai: reach classes=%u blocks=%ux%u components=%u cells=%u blocks_written=%u us=%u", n,
               s_reach_ix.bw, s_reach_ix.bh, comps, n * (unsigned)w * (unsigned)h, s_reach_n,
               s_qpf.QuadPart > 0 ? (unsigned)((t1.QuadPart - t0.QuadPart) * 1000000 / s_qpf.QuadPart) : 0u);
}

/* THE THREAT TABLE (impai.h, IMPAI_THREAT), one row a unit type of the level: what the role classifier
   (tasim_roles.c) makes of the type's def and weapons as the level load left them, the same rule the
   calibration runner's `tasim_run roles` applies to the install's files. Content: a function of the level's
   defs and weapons, rebuilt at the level init of every level a seat can play ImpAI on, live or puppet. */
static IMPAI_THREAT s_threat_def[TAGPU_LIM_TYPES];
static unsigned     s_threat_n;
SIM_ROWS_LIVE(s_threat_def, SIM_CONTENT, SIM_ID_UNIT_TYPE, s_threat_n, NULL);
SIM_VAR(s_threat_n, SIM_CONTENT);

/* THE TYPE INFORMATION (impai.h, IMPAI_TINFO), one row a unit type of the level: its category, gains and
   value, from the same classification as the threat table, its def and economy keys and the map's wind and
   tide (the content view, tagpu_worldq_open_content), and its army class (impai_table.h, impai_class_of) from
   the same input and output. Content, rebuilt beside the threat table; no row when the classification cannot
   be had. */
static IMPAI_TINFO s_tinfo[TAGPU_LIM_TYPES];
static unsigned    s_tinfo_n;
SIM_ROWS_LIVE(s_tinfo, SIM_CONTENT, SIM_ID_UNIT_TYPE, s_tinfo_n, NULL);
SIM_VAR(s_tinfo_n, SIM_CONTENT);

/* A type's D-gun: its longest CommandFire weapon when its def takes the resolver's mode 4 (+0x245 bit 14,
   `candgun`, 0x43F7E8..0x43F7F4), its range held to 1..65535 px and its energy a shot (+0xC0) to 0..65535; none
   otherwise. BOUND: nweapons is held to the input's slots; a value not finite reads as none. */
#define F245_CANDGUN (1u << 14)
static void dgun_of(const TASIM_ROLE_IN* in, IMPAI_TINFO* t)
{
    const unsigned nw = in->nweapons < TASIM_ROLE_WEAPONS ? in->nweapons : TASIM_ROLE_WEAPONS;
    unsigned k;
    t->dgun_range = t->dgun_e = 0;
    if (!(in->flags245 & F245_CANDGUN)) return;
    for (k = 0; k < nw; k++) {
        const TASIM_ROLE_WEAPON* w = &in->w[k];
        if (!(w->flags & TASIM_WF_COMMANDFIRE) || w->range <= 0 || w->range <= (int32_t)t->dgun_range) continue;
        t->dgun_range = (uint16_t)(w->range < 65535 ? w->range : 65535);
        t->dgun_e = (uint16_t)(w->energy_per_shot >= 0.0f && w->energy_per_shot < 65535.0f
                                   ? (unsigned)w->energy_per_shot : w->energy_per_shot >= 65535.0f ? 65535u : 0u);
    }
}

/* A type's IMPAI_TA_* facts (impai.h) from its def's flags and its weapons 1 and 2, the role input's slots 0
   and 1 (tagpu_worldq.c, the role filler, which leaves an empty slot zeroed and ends at the type's count). The
   resolver reads the unit's own slot weapons, +0x10 and +0x2C, which are its def's: their one writer is the slot
   init 0x49E070 at 0x49E0B0, called only by the creates (0x4860AC, 0x4862C4; tagpu_weapons.c's my_StartWeapons
   in its place writes the same def weapons), and nothing re-types a live unit (DISASSEMBLED, engine/units.md).
   A saved game loaded under changed content breaks it: the slot comes from the new def, its bit 1 from the save
   (0x48766E). */
#define F245_CANATTACK (1u << 4)
static void attack_of(const TASIM_ROLE_IN* in, IMPAI_TINFO* t)
{
    const unsigned nw = in->nweapons < TASIM_ROLE_WEAPONS ? in->nweapons : TASIM_ROLE_WEAPONS;
    const uint32_t w1 = nw > 0 ? in->w[0].flags : 0u, w2 = nw > 1 ? in->w[1].flags : 0u;
    t->attack = (uint8_t)(((in->flags245 & F245_CANATTACK) ? IMPAI_TA_CANATTACK : 0u) |
                          ((in->flags241 & TASIM_F241_KAMIKAZE) ? IMPAI_TA_KAMIKAZE : 0u) |
                          ((w1 & TASIM_WF_TOAIR) ? IMPAI_TA_TOAIR1 : 0u) |
                          ((w1 & TASIM_WF_WATER) ? IMPAI_TA_WATER1 : 0u) |
                          ((w2 & TASIM_WF_WATER) ? IMPAI_TA_WATER2 : 0u) |
                          ((in->flags241 & TASIM_F241_CANHOVER) ? IMPAI_TA_HOVER : 0u) |
                          ((in->flags241 & TASIM_F241_CANFLY) ? IMPAI_TA_FLY : 0u) |
                          ((w1 & TASIM_WF_DROPPED) ? IMPAI_TA_DROP1 : 0u));
}

static void impai_tinfo_fill(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* roles, int n)
{
    WQ_ENGINE c;
    WORLDQ q;
    unsigned k[IMPAI_CATS], army = 0;
    int t;
    const int got = tagpu_worldq_open_content(&c, &q) ? impai_tinfo_build(&q, roles, n, s_tinfo, TAGPU_LIM_TYPES) : -1;
    if (got <= 0) {
        memset(s_tinfo, 0, sizeof s_tinfo);
        tagpu_logf("impai: tinfo none: the content view could not be opened");
        return;
    }
    s_tinfo_n = (unsigned)got;
    for (t = 1; t < got && t < n; t++) {
        s_tinfo[t].klass = (uint8_t)impai_class_of(&in[t], &roles[t]);
        army += s_tinfo[t].klass != IMPAI_K_NONE;
        dgun_of(&in[t], &s_tinfo[t]);
        attack_of(&in[t], &s_tinfo[t]);
    }
    impai_tinfo_offers(in, roles, n, s_tinfo, got);
    impai_tinfo_count(s_tinfo, got, k);
    tagpu_logf("impai: tinfo mex=%u energy=%u maker=%u factory=%u cons=%u army=%u", k[IMPAI_C_MEX],
               k[IMPAI_C_ENERGY], k[IMPAI_C_MAKER], k[IMPAI_C_FACTORY], k[IMPAI_C_CONS], army);
}

/* THE FIGHT CONTENT (impai_fight.h): the fight predictor's tables -- every unit type's figures, the weapon
   records they name and those weapons' [DAMAGE] rows resolved to types -- from the same classification as the
   threat table and the same records (tagpu_worldq.c, THE FIGHT CONTENT'S ENGINE FILLER). Content, rebuilt
   beside the threat table, all or none with the menus: a level whose fight content cannot be had or does not
   fit its tables has no content and marks no seat (impai_content_build). The weapon table keeps its rows by
   weapon index, so s_fweapon_n is one more than the highest index a type names. */
typedef char impai_fweapons_fit[TAGPU_LIM_WEAPONS <= IMPAI_FWEAPONS ? 1 : -1];
static IMPAI_FTYPE   s_ftype[TAGPU_LIM_TYPES];
static unsigned      s_ftype_n;
static IMPAI_FWEAPON s_fweapon[IMPAI_FWEAPONS];
static unsigned      s_fweapon_n;
static IMPAI_FDMG    s_fdmg[IMPAI_FDMG_POOL];
static unsigned      s_fdmg_n;
static const SimField s_ftype_layout[] = {
    SIM_FIELD_ARRAY(IMPAI_FTYPE, w, SIMF_ID, SIM_ID_WEAPON),
    SIM_FIELD_END
};
static const SimField s_fdmg_layout[] = {
    SIM_FIELD(IMPAI_FDMG, type, SIMF_ID, SIM_ID_UNIT_TYPE),
    SIM_FIELD_END
};
SIM_ROWS_LIVE(s_ftype, SIM_CONTENT, SIM_ID_UNIT_TYPE, s_ftype_n, s_ftype_layout);
SIM_VAR(s_ftype_n, SIM_CONTENT);
SIM_ROWS_LIVE(s_fweapon, SIM_CONTENT, SIM_ID_WEAPON, s_fweapon_n, NULL);
SIM_VAR(s_fweapon_n, SIM_CONTENT);
SIM_ROWS_LIVE(s_fdmg, SIM_CONTENT, SIM_ID_NONE, s_fdmg_n, s_fdmg_layout);
SIM_VAR(s_fdmg_n, SIM_CONTENT);

static void impai_fight_clear(void)
{
    memset(s_ftype, 0, sizeof s_ftype);
    memset(s_fweapon, 0, sizeof s_fweapon);
    memset(s_fdmg, 0, sizeof s_fdmg);
    s_ftype_n = s_fweapon_n = s_fdmg_n = 0;
}

static uint64_t fnv64(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/* The fight content from the classifier's input and output for the level's n types: 1, or 0 with none.
   The line names the three tables' addresses and live rows, so tools/impai/predfit.py can read them whole
   with `tacli peek`; under the check lever their FNV-1a 64 over the live rows' bytes, in that order, which
   the tool's copy must equal. */
static int impai_fight_build(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* out, int n)
{
    unsigned nw = 0, nd = 0;
    impai_fight_clear();
    if (tagpu_worldq_fight_in(in, out, n, s_ftype, s_fweapon, IMPAI_FWEAPONS, &nw, s_fdmg, IMPAI_FDMG_POOL, &nd)) {
        impai_fight_clear();
        tagpu_logf("impai: fight none: the engine's types, weapons or [DAMAGE] rows do not fit the tables "
                   "(%d types, %u weapon slots, %u rows)", n, (unsigned)IMPAI_FWEAPONS, (unsigned)IMPAI_FDMG_POOL);
        return 0;
    }
    s_ftype_n = (unsigned)n;
    s_fweapon_n = nw;
    s_fdmg_n = nd;
    tagpu_logf("impai: fight types=%u weapons=%u dmgrows=%u at=0x%08X 0x%08X 0x%08X", s_ftype_n, s_fweapon_n,
               s_fdmg_n, (unsigned)(uintptr_t)s_ftype, (unsigned)(uintptr_t)s_fweapon, (unsigned)(uintptr_t)s_fdmg);
    if (s_install.check) {
        uint64_t h = 0xCBF29CE484222325ull;
        h = fnv64(h, s_ftype, (size_t)s_ftype_n * sizeof *s_ftype);
        h = fnv64(h, s_fweapon, (size_t)s_fweapon_n * sizeof *s_fweapon);
        h = fnv64(h, s_fdmg, (size_t)s_fdmg_n * sizeof *s_fdmg);
        tagpu_logf("impai: fight digest=%08X%08X", (unsigned)(h >> 32), (unsigned)h);
    }
    return 1;
}

/* THE MENUS: for every side the game loaded (main+0x37F39, at most the five side records), the build menu of
   every builder type (def +0x241 bit 6), by Quick Build's page rule with every type a press reaches, the
   structures armed and the units queued (tagpu_buildmenu_types): a factory offers only the second. Sorted by
   (side, builder), each row's types ascending in the pool (impai.h, IMPAI_BMENU). Two readers: the brain, which
   gives orders to a factory, a constructor and a commander through its seat's side's rows (tagpu_impai_menu),
   and the role classifier, whose tech walk follows every builder's options (the threat build below, through
   impai_menu_union). So every builder, not the brain's three categories alone: the categories are the
   classifier's answer, which cannot choose the rows it is fed. Content: a function of the level's defs and
   files alone, so every mark a level can come to hold -- its own, a takeover's, a rewind's restored ones --
   finds its side's menus, whichever seats were marked when it was built. All or none: past either table the
   level has no content, and no seat of it is marked. A side's factories', constructors' and commanders' menus
   took at most 148 builders and at most 1309 types in the compatibility suite's ImpAI stages (MEASURED
   2026-11-13, the `impai: menus` lines of the side each stage built), so five such sides take under half of
   either table [INFERRED for the sides no stage built, and for builders of no such category, which that
   measure left out]. */
#define IMPAI_MENUS     2048u
#define IMPAI_MENU_POOL 65536u
#define SIDE_ROOM       IMPAI_MENU_SIDES
#define M_SIDES         0x37F39     /* u32: the sides this game loaded                              */
static IMPAI_BMENU s_menu[IMPAI_MENUS];
static uint16_t   s_menu_pool[IMPAI_MENU_POOL];
static unsigned   s_nmenu, s_npool;
static const SimField s_menu_layout[] = {
    SIM_FIELD(IMPAI_BMENU, builder, SIMF_ID, SIM_ID_UNIT_TYPE),
    SIM_FIELD_END
};
static const SimField s_menu_pool_layout[] = {
    SIM_FIELD_AT(0, sizeof(uint16_t), SIMF_ID, SIM_ID_UNIT_TYPE),
    SIM_FIELD_END
};
SIM_ROWS_LIVE(s_menu, SIM_CONTENT, SIM_ID_NONE, s_nmenu, s_menu_layout);
SIM_ROWS_LIVE(s_menu_pool, SIM_CONTENT, SIM_ID_NONE, s_npool, s_menu_pool_layout);
SIM_VAR(s_nmenu, SIM_CONTENT);
SIM_VAR(s_npool, SIM_CONTENT);

/* 1 when the level's content is built (impai_content_build), which every mark requires */
static unsigned s_content;
SIM_VAR(s_content, SIM_CONTENT);

/* Record i's side as the page loader reads the controlled player's (0x41AD61..0x41AD66): PlayerInfo at
   +0x27, its byte +0x95. PlayerInfo objects live for the process. BOUND: below the sides loaded and the five
   records; 0xFF otherwise. */
static unsigned char record_side(const char* ta, unsigned i)
{
    const unsigned char* info = *(const unsigned char* const*)(ta + OFF_PLAYERS + i * PLAYER_STRIDE + PL_INFO);
    const unsigned sides = *(const unsigned*)(ta + M_SIDES);
    if (i >= PLAYER_COUNT || !info) return 0xFF;
    return info[0x95] < sides && info[0x95] < SIDE_ROOM ? info[0x95] : 0xFF;
}

/* Every menu of the sides in `sides` (bit s for side s), on the LOADER thread inside 0x4919C8 before the
   threat build, whose classification reads them: 1, or 0 with no menu when a table is past its size or a read
   refuses. BOUND: builder types below the def count the content view answers. */
static int impai_menus_build(unsigned sides)
{
    WQ_ENGINE c;
    WORLDQ q;
    WQ_DEF d;
    unsigned s, t, builders, types;
    LARGE_INTEGER t0, t1;
    s_nmenu = s_npool = 0;
    if (!sides) return 1;
    if (!tagpu_worldq_open_content(&c, &q)) {
        tagpu_logf("impai: menus none: the content view could not be opened");
        return 1;
    }
    for (s = 0; s < SIDE_ROOM; s++) {
        if (!(sides >> s & 1u)) continue;
        QueryPerformanceCounter(&t0);
        builders = types = 0;
        for (t = 1; t < c.ndefs; t++) {
            int n;
            if (!q.ops->def(&q, (int)t, &d) || !(d.flags & WQ_DF_BUILDER)) continue;
            if (s_nmenu == IMPAI_MENUS ||
                (n = tagpu_buildmenu_types(c.ta, c.defs, c.ndefs, t, (unsigned char)s, s_menu_pool + s_npool,
                                           IMPAI_MENU_POOL - s_npool)) < 0) {
                tagpu_logf("impai: menus past their tables (side %u, builder %u: %u menus, %u types)", s, t,
                           s_nmenu, s_npool);
                s_nmenu = s_npool = 0;
                return 0;
            }
            s_menu[s_nmenu].side = (uint8_t)s;
            s_menu[s_nmenu].builder = (uint16_t)t;
            s_menu[s_nmenu].n = (uint16_t)n;
            s_menu[s_nmenu].at = s_npool;
            s_nmenu++;
            s_npool += (unsigned)n;
            builders++;
            types += (unsigned)n;
        }
        QueryPerformanceCounter(&t1);
        tagpu_logf("impai: menus side=%u builders=%u types=%u ms=%.1f", s, builders, types,
                   s_qpf.QuadPart > 0 ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_qpf.QuadPart : 0.0);
    }
    return 1;
}

/* ImpAI's WqMenuFn: builder type `builder`'s menu for seat `seat`'s side, from the menus, copied. GAME
   thread, in an ImpAI seat's turn; the table is whole before the first step (the level init's ordering).
   BOUND: the side as record_side bounds it; a row's types lie inside the pool by construction, and are
   checked against it as values (impai_menu_find). */
int tagpu_impai_menu(int seat, unsigned builder, uint16_t* out, int cap)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    unsigned char side;
    int r;
    if (!ta || seat < 0 || seat >= PLAYER_COUNT || (side = record_side(ta, (unsigned)seat)) == 0xFF) return 0;
    if ((r = impai_menu_find(s_menu, s_nmenu, s_npool, side, builder)) < 0) return 0;
    if (s_menu[r].n > cap) return -1;
    memcpy(out, s_menu_pool + s_menu[r].at, (size_t)s_menu[r].n * sizeof *out);
    return s_menu[r].n;
}

/* The role filler's WqRoleOptFn: a builder's menus of every side, merged (impai_menu_union). LOADER thread, in
   the threat build, after impai_menus_build. */
static int role_options(unsigned type, uint16_t* out, int cap)
{
    return impai_menu_union(s_menu, s_nmenu, s_menu_pool, s_npool, type, out, cap);
}

/* THE THREAT BUILD, at the level init on the loader thread, after the unit-data load (tagpu_worldq.c, THE
   ROLE TABLE'S ENGINE FILLER, has the ordering and the bounds) and after the menus: every type's classifier
   input from the engine, each builder's build options its menus merged over the sides, the classification, and
   its rounding into s_threat_def. The options are packed into scratch of the pool's size, which holds them all,
   since a builder's union is at most the sum of its rows. The classifier's -1 (no type is a commander, so no
   tech level) leaves every figure the table takes defined (tasim_roles_classify writes the tech level last);
   its -2 cannot arise, as the filler refuses an option past the type count. A level whose input cannot be had
   has no threat table, and the maps then draw no threat or influence. Under the check lever every type's role,
   figures and tech level are logged for tools/impai/roles_check.py, which compares them with `tasim_run
   roles`. The fight content is built from the same classification (impai_fight_build): 1 when it was, else
   0. */
static int impai_threat_build(void)
{
    const int n = tagpu_worldq_role_count();
    const unsigned nopt = s_npool ? s_npool : 1u;
    TASIM_ROLE_IN* in = NULL;
    TASIM_ROLE_OUT* out = NULL;
    TASIM_ROLE_Q q;
    uint16_t* opt = NULL;
    void* work = NULL;
    int rc = -3, i, fight = 0;
    memset(s_threat_def, 0, sizeof s_threat_def);
    s_threat_n = 0;
    memset(s_tinfo, 0, sizeof s_tinfo);
    s_tinfo_n = 0;
    if (n > 0 && (in = (TASIM_ROLE_IN*)tagpu_sim_scratch((size_t)n * sizeof *in)) &&
        (out = (TASIM_ROLE_OUT*)tagpu_sim_scratch((size_t)n * sizeof *out)) &&
        (opt = (uint16_t*)tagpu_sim_scratch((size_t)nopt * sizeof *opt)) &&
        (work = tagpu_sim_scratch(tasim_roles_work(n))) &&
        tagpu_worldq_role_in(in, n, role_options, opt, nopt) == 0)
        rc = tasim_roles_classify(in, n, out, &q, work);
    if (rc == 0 || rc == -1) {
        impai_threat_from_roles(out, n, s_threat_def);
        s_threat_n = (unsigned)n;
        impai_tinfo_fill(in, out, n);
        if (s_install.check)
            for (i = 0; i < n; i++)
                tagpu_logf("impai: role %s %s %s value=%.17g dps_ground=%.17g dps_air=%.17g dps_water=%.17g "
                           "range_ground=%d flags=0x%02X tech=%d", in[i].name, tasim_roles_name(out[i].role),
                           tasim_roles_layer_name(out[i].layer), out[i].value, out[i].dps_ground,
                           out[i].dps_air, out[i].dps_water, (int)out[i].range_ground, out[i].flags, out[i].tech);
        tagpu_logf("impai: threat types=%d%s", n, rc == -1 ? " (no commander: no tech level)" : "");
        fight = impai_fight_build(in, out, n);
    } else {
        impai_fight_clear();
        tagpu_logf("impai: threat none: the role classifier's input could not be had (%d types, %d)", n, rc);
        tagpu_logf("impai: fight none: no classification");
    }
    tagpu_sim_scratch_free(work);
    tagpu_sim_scratch_free(opt);
    tagpu_sim_scratch_free(out);
    tagpu_sim_scratch_free(in);
    return fight;
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
   and in the first fill of a level. A fill runs only when the render thread has taken the last packet
   (tagpu_packet_pub.c, the FRESH gate), so the GameTimes of the builds follow the renderer's pace and
   differ between a live game and its puppet; the digests impai_check.py pairs by GameTime come from
   tagpu_impai_frame instead, which builds its own maps. Rebuilt too in a fill whose GameTime or
   generation is below the build's: in play both only grow, so that is a restore (a seek, a rewind, a
   takeover's), and the maps of a restore into the same epoch are the restored state's, not the state the
   cache was built from. `cellh` is the ground's height at each cell's centre, taken at the build. The
   level init forgets the cache (s_ovc.built), on the loader thread before the level's first in-play
   frame, the only frames that fill. */
typedef struct OV_MAPS {
    int      gw, gh;
    uint64_t known, ov;                 /* the digests of the knowledge, and of the maps and labels made of it */
    uint32_t threat[IMPAI_THREATS][IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    int32_t  influence[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    uint16_t presence[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    uint16_t reach[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    int32_t  cellh[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    int32_t  diff[(IMPAI_GRID_MAX + 1) * IMPAI_GRID_MAX];
} OV_MAPS;
static struct {
    uint8_t  built;
    uint32_t epoch, gen, tick;          /* the build's epoch, generation and GameTime */
    OV_MAPS  m;
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

static void ov_push(TAGPU_PK_IMPAI* out, unsigned max, unsigned* n, int* cut, unsigned seat, uint8_t kind,
                    uint8_t level, uint8_t size, int32_t x, int32_t h, int32_t z)
{
    TAGPU_PK_IMPAI* e;
    if (*n >= max) { *cut = 1; return; }
    e = &out[(*n)++];
    e->kind = kind; e->seat = (uint8_t)seat; e->level = level; e->size = size;
    e->x = x; e->h = h; e->z = z;
}

/* THE ITEMS, from seat `seat`'s maps `o` and declared state: layer `layer`'s nonzero cells, then the seat's
   rows in slot order -- its own units, its contacts (any class of now, or radar or sonar), its ghosts --
   then its published patches. A row's point is its block's centre. BOUND: every row index below the
   table's rows, every cell below the maps' gw x gh (each at most IMPAI_GRID_MAX), every patch below the
   published count, at most IMPAI_CLUSTERS_HALF; at most `max` items. */
static void ov_items(const OV_MAPS* o, unsigned seat, unsigned layer, TAGPU_PK_IMPAI* out, unsigned max,
                     unsigned* n, int* cut)
{
    const unsigned npatch = s_seatst[seat].nclusters < IMPAI_CLUSTERS_HALF ? s_seatst[seat].nclusters
                                                                           : IMPAI_CLUSTERS_HALF;
    unsigned i;
    int gx, gz;
    *n = 0;
    for (gz = 0; gz < o->gh; gz++)
        for (gx = 0; gx < o->gw; gx++) {
            const int c = gz * o->gw + gx;
            uint32_t v;
            uint8_t level;
            switch (layer) {
            case TAGPU_PK_IL_INFLUENCE: {
                const int32_t f = o->influence[c];
                v = f < 0 ? (uint32_t)0 - (uint32_t)f : (uint32_t)f;
                level = (uint8_t)(shade(v) | (f < 0 ? TAGPU_PK_IM_NEG : 0u));
                break;
            }
            case TAGPU_PK_IL_PRESENCE: v = o->presence[c]; level = shade(v); break;
            case TAGPU_PK_IL_REACH: v = o->reach[c]; level = (uint8_t)(1u + v % 15u); break;
            default: v = o->threat[layer][c]; level = shade(v); break;
            }
            if (v)
                ov_push(out, max, n, cut, seat, TAGPU_PK_IM_CELL, level, IMPAI_CELL_PX / 32, gx * IMPAI_CELL_PX,
                        o->cellh[c], gz * IMPAI_CELL_PX);
        }
    for (i = 1; i < sizeof s_known / sizeof s_known[0]; i++) {
        const IMPAI_SEEN* r = &s_known[i].seat[seat];
        const int32_t x = r->bx * 32 + 16, z = r->bz * 32 + 16;
        uint8_t kind;
        if (!r->how && !r->bx && !r->bz && !r->type && !r->owner) continue;
        if (r->how & IMPAI_OWN) kind = TAGPU_PK_IM_OWN;
        else if (r->how & (IMPAI_NOW | IMPAI_PASS)) kind = TAGPU_PK_IM_CONTACT;
        else kind = TAGPU_PK_IM_GHOST;
        ov_push(out, max, n, cut, seat, kind, r->how, 0, x, ground_at(x, z), z);
    }
    for (i = 0; i < npatch; i++) {
        const SNAP_PATCH* p = &s_cluster[seat][i];
        const int32_t x = (p->x0 + p->x1 + 1) * 8, z = (p->z0 + p->z1 + 1) * 8;
        const int span = (p->x1 - p->x0 > p->z1 - p->z0 ? p->x1 - p->x0 : p->z1 - p->z0) / 2 + 1;
        ov_push(out, max, n, cut, seat, TAGPU_PK_IM_CLUSTER, 0, (uint8_t)(span > 255 ? 255 : span), x,
                ground_at(x, z), z);
    }
}

/* The seat's view of what it knows: its column of the rows and its published patches */
static void seat_view(unsigned seat, IMPAI_VIEW* v)
{
    memset(v, 0, sizeof *v);
    v->seat = (int)seat;
    v->rows = s_known;
    v->nrows = (int)(sizeof s_known / sizeof s_known[0]);
    v->clusters = s_cluster[seat];
    v->nclusters = s_seatst[seat].nclusters < IMPAI_CLUSTERS_HALF ? s_seatst[seat].nclusters : IMPAI_CLUSTERS_HALF;
    v->hits.at = s_radar_hit ? &s_radar_hit[0].seat[seat] : NULL;
    v->hits.n = (int)RADAR_HIT_ROWS;
    v->hits.stride = (unsigned)sizeof(IMPAI_HITROW);
}

/* What the maps and the brain of seat `seat` (below 10) are a function of: its view (seat_view), its alliance
   row (main's record of the seat, +0x108 + j, one of the ten records by index), the threat table and the map's
   size. */
static void impai_view_of(const char* ta, unsigned seat, IMPAI_VIEW* v)
{
    const char* rec = ta + OFF_PLAYERS + seat * PLAYER_STRIDE;
    unsigned j;
    seat_view(seat, v);
    for (j = 0; j < PLAYER_COUNT; j++)
        if (j != seat && *(const uint8_t*)(rec + 0x108 + j) == 1) v->allies |= 1u << j;
    v->threat = s_threat_n ? s_threat_def : NULL;
    v->ntypes = (int)s_threat_n;
    v->w = *(const int*)(ta + OFF_MAP_W16);
    v->h = *(const int*)(ta + OFF_MAP_H16);
}

/* THE BUILD: seat `seat`'s maps into `o`, over its view (impai_view_of); the reach layer's label at each cell's
   centre block for the level's lowest movement class; each cell's ground height; and the digests of the
   knowledge and of what was built from it. A function of ImpAI's declared state and content and of what the
   recording carries, whatever `o` held before. 0 when the map's size is past the maps' grid. */
static int ov_build(const char* ta, unsigned seat, OV_MAPS* o)
{
    IMPAI_VIEW v;
    IMPAI_MAPS m;
    unsigned c;
    int gx, gz, layer;
    impai_view_of(ta, seat, &v);
    if (v.w < 2 || v.h < 2 || v.w * 16 > IMPAI_GRID_MAX * IMPAI_CELL_PX || v.h * 16 > IMPAI_GRID_MAX * IMPAI_CELL_PX)
        return 0;
    for (layer = 0; layer < IMPAI_THREATS; layer++) m.threat[layer] = o->threat[layer];
    m.influence = o->influence;
    m.presence = o->presence;
    m.diff = o->diff;
    if (!impai_maps_build(&v, &m)) return 0;
    if (!v.threat) {
        for (layer = 0; layer < IMPAI_THREATS; layer++) memset(o->threat[layer], 0, sizeof o->threat[layer]);
        memset(o->influence, 0, sizeof o->influence);
    }
    o->gw = m.gw;
    o->gh = m.gh;
    for (c = 0; c < WQ_MCLASSES && !(s_reach_ix.present >> c & 1u); c++) {}
    for (gz = 0; gz < m.gh; gz++)
        for (gx = 0; gx < m.gw; gx++) {
            const unsigned bx = (unsigned)gx * (IMPAI_CELL_PX / 32) + IMPAI_CELL_PX / 64;
            const unsigned bz = (unsigned)gz * (IMPAI_CELL_PX / 32) + IMPAI_CELL_PX / 64;
            const int k = gz * m.gw + gx;
            o->reach[k] = c < WQ_MCLASSES && bx < s_reach_ix.bw && bz < s_reach_ix.bh
                ? s_reach[s_reach_ix.at[c] + bz * s_reach_ix.bw + bx] : 0;
            o->cellh[k] = ground_at(gx * IMPAI_CELL_PX + IMPAI_CELL_PX / 2, gz * IMPAI_CELL_PX + IMPAI_CELL_PX / 2);
        }
    o->known = impai_known_digest(&v);
    o->ov = fnv64(impai_maps_digest(&m), o->reach, (size_t)m.gw * (size_t)m.gh * sizeof o->reach[0]);
    return 1;
}

/* The overlay's digest: the maps' and labels' (o->ov), then the items made of them */
static uint64_t ov_digest(const OV_MAPS* o, const TAGPU_PK_IMPAI* items, unsigned n)
{
    return fnv64(o->ov, items, (size_t)n * sizeof *items);
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
    int built;
    *n = 0;
    if (!s_ov.on || !ta || seat >= PLAYER_COUNT) return 0;
    gen = s_seatst[seat].known_gen;
    if (!(s_seat[seat] & IMPAI_ON) && !gen) return 0;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    epoch = now / 30u;
    built = !s_ovc.built || (epoch != s_ovc.epoch && gen != s_ovc.gen) || now < s_ovc.tick || gen < s_ovc.gen;
    if (built) {
        if (!ov_build(ta, seat, &s_ovc.m)) return 0;
        s_ovc.built = 1;
        s_ovc.epoch = epoch;
        s_ovc.gen = gen;
        s_ovc.tick = now;
    }
    memset(h, 0, sizeof *h);
    h->seat = (uint8_t)seat;
    h->layer = s_ov.layer;
    h->radar_age = s_seatst[seat].radar_done ? now - s_seatst[seat].radar_done : 0xFFFFFFFFu;
    h->known_gen = gen;
    ov_items(&s_ovc.m, seat, s_ov.layer, out, max, n, cut);
    if (built && s_install.check) {
        /* what the drawn overlay was made of, for impai_check.py (b) to hold to the GameTimes' digests */
        const uint64_t ov = ov_digest(&s_ovc.m, out, *n);
        tagpu_logf("impai: ov seat=%u tick=%u known=%08X%08X ov=%08X%08X items=%u", seat, now,
                   (unsigned)(s_ovc.m.known >> 32), (unsigned)s_ovc.m.known, (unsigned)(ov >> 32), (unsigned)ov,
                   *n);
    }
    return 1;
}

/* THE KNOWLEDGE DIGESTS impai_check.py (b) pairs by GameTime, under the check lever: at every GameTime
   that is a multiple of 30, once, each seat the overlay could show (ImpAI plays it, or its knowledge
   generation is above 0) logs `impai: known seat=<i> tick=<t> known=<FNV> ov=<FNV> items=<n>`: the digest
   of its rows and patches, and the overlay's digest built from them at that GameTime -- the maps, the
   labels and the items of the overlay lever's layer, built by ov_build and ov_items into scratch the call
   frees, so nothing of the drawn overlay's cache or pace reaches it; `ov=none` when the map is past the
   maps' grid or the scratch could not be had. The work runs only under the check lever, outside the turn
   the cost line times. From the end of the in-play draw (tagpu_packet_pub.c,
   after_draw), on the game thread, every draw: the one point both games pass after each step, where a
   live game's knowledge is what its observation made in the step and a puppet's is the recording's,
   which the frame visit put before the draw (tagpu_replay.c, recorded_put). A puppet logs a GameTime only
   when that frame's visit put the recording at it (tagpu_replay_put_at): the visit puts nothing at the
   level's last step, which has no image after it, nor while a seek, a hold or a rewind runs, and the rows
   then hold an earlier step's knowledge. So a puppet's GameTime is exact or missing. Under the harness's
   turbo at k = 1 a frame runs at most one step (tagpu_replay.c, THE TURBO), so every GameTime has its draw
   and both games log the same GameTimes. At stock pacing a frame can run up to five steps, and a GameTime a
   frame steps past is missing from that game's log: fewer ticks for (b) to pair, never another digest. */
static uint32_t s_known_tick;           /* the GameTime last logged */
SIM_VAR(s_known_tick, SIM_PRIVATE);

void tagpu_impai_frame(void)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    IMPAI_VIEW v;
    OV_MAPS* o;
    TAGPU_PK_IMPAI* items;
    unsigned seat, now, n;
    uint64_t known, ov;
    int cut;
    if (!s_install.check || !ta) return;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    if (now % 30u || now == s_known_tick) return;
    if (tagpu_puppet_on() && !tagpu_replay_put_at(now)) return;
    s_known_tick = now;
    o = (OV_MAPS*)tagpu_sim_scratch(sizeof *o);
    items = (TAGPU_PK_IMPAI*)tagpu_sim_scratch(TAGPU_PK_MAX_IMPAI * sizeof *items);
    for (seat = 0; seat < PLAYER_COUNT; seat++) {
        if (!(s_seat[seat] & IMPAI_ON) && !s_seatst[seat].known_gen) continue;
        seat_view(seat, &v);
        known = impai_known_digest(&v);
        n = 0;
        cut = 0;
        if (o && items && ov_build(ta, seat, o)) {
            ov_items(o, seat, s_ov.layer, items, TAGPU_PK_MAX_IMPAI, &n, &cut);
            ov = ov_digest(o, items, n);
            tagpu_logf("impai: known seat=%u tick=%u known=%08X%08X ov=%08X%08X items=%u", seat, now,
                       (unsigned)(known >> 32), (unsigned)known, (unsigned)(ov >> 32), (unsigned)ov, n);
        } else {
            tagpu_logf("impai: known seat=%u tick=%u known=%08X%08X ov=none", seat, now,
                       (unsigned)(known >> 32), (unsigned)known);
        }
    }
    tagpu_sim_scratch_free(items);
    tagpu_sim_scratch_free(o);
}

/* ImpAI plays its seats: the wrapper, the turn, the brain's and the military's state, the economy's glue, the
   pulse and the level end; the turn runs the military's decision (impai_mil, below) too. OWNER: the
   wrapper plays no puppet seat (impai_seat_marked refuses one, and a record that is not controller 2), the
   WORLDQ refuses to open for a puppet seat and order() to command one, and on a puppet level the level init
   only clears the brain's state, which the recording then restores. */
SIM_FEATURE(impai_play, SIM_PUPPET_OWNER, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm impai-naval stock-impai-naval");

/* THE BRAIN'S STATE (impai.h): each seat's standing state, and the job table, SLOT-KEYED -- the task ImpAI gave
   the unit in that slot, for whichever seat owns it: slots are partitioned among the players' unit blocks, so
   one table serves every seat. A job holds only while its slot holds a live unit of the seat's of the job's
   type, which the census tests every decision; a slot reused by a unit of the same type inherits the job, at the
   cost of at most one misjudged failure (impai.h, IMPAI_JOB). Cleared at every level init; a puppet's come from
   its recording. */
static IMPAI_BRAIN s_brain[PLAYER_COUNT];
/* a seat's block holds at most the engine's units a player, every one of which the census lists (impai.h, THE
   CENSUS'S BOUND) */
_Static_assert(TAGPU_LIM_UNITS <= IMPAI_CENSUS_MAX, "a census walks a seat's whole block");
static const SimField s_brain_layout[] = {
    { offsetof(IMPAI_BRAIN, explorer), 2, 0, 0, SIMF_ID, SIM_ID_UNIT_SLOT },
    { offsetof(IMPAI_BRAIN, sea_none_type), 2, 0, 0, SIMF_ID, SIM_ID_UNIT_TYPE },
    { offsetof(IMPAI_BRAIN, way[0].type), 2, IMPAI_WAYS, sizeof(IMPAI_WAYREC), SIMF_ID, SIM_ID_UNIT_TYPE },
    SIM_FIELD_END
};
SIM_ROWS(s_brain, SIM_STATE, SIM_ID_PLAYER, s_brain_layout);
static IMPAI_JOB s_job[TAGPU_PK_DESIGN_SLOTS];
static const SimField s_job_layout[] = {
    { offsetof(IMPAI_JOB, type),   2, 0, 0, SIMF_ID, SIM_ID_UNIT_TYPE },
    { offsetof(IMPAI_JOB, build),  2, 0, 0, SIMF_ID, SIM_ID_UNIT_TYPE },
    { offsetof(IMPAI_JOB, target), 2, 0, 0, SIMF_ID, SIM_ID_UNIT_SLOT },
    { offsetof(IMPAI_JOB, far_type), 2, 0, 0, SIMF_ID, SIM_ID_UNIT_TYPE },
    SIM_FIELD_END
};
SIM_ROWS(s_job, SIM_STATE, SIM_ID_UNIT_SLOT, s_job_layout);

/* THE MILITARY'S STATE (impai.h, IMPAI_MIL), one a seat: its level, its noise seed and its table, set at a live
   level's init or for a takeover's fresh seat (seat_fresh), its squads and its picture, written by its military
   decisions. Cleared at every level init; a puppet's comes from its recording, so a replay plays the recorded
   level, seed and table without either lever, and so does a takeover for every seat ImpAI played there. A squad's target is a unit slot; the picture's enemy types are unit types. The seed is the
   generator's state [0x51FC88] xor the seat: arbitrary bits below 2^31 (0x4B6C30 steps it x 16807 mod 2^31 - 1),
   which the address scan cannot tell from an address, so its layout says so: SIMF_OPAQUE. About one seed in
   fifteen lies inside this DLL's image (SizeOfImage 130..150 MB on the three builds, MEASURED 2026-11-13). */
static IMPAI_MIL s_mil[PLAYER_COUNT];
static const SimField s_mil_layout[] = {
    SIM_FIELD(IMPAI_MIL, seed, SIMF_OPAQUE, SIM_ID_NONE),
    { offsetof(IMPAI_MIL, sq[0].target), 2, IMPAI_SQUADS, sizeof(IMPAI_SQUAD), SIMF_ID, SIM_ID_UNIT_SLOT },
    { offsetof(IMPAI_MIL, pic.e[0].type), 2, IMPAI_PIC_ENEMY, sizeof(IMPAI_ENEMY), SIMF_ID, SIM_ID_UNIT_TYPE },
    SIM_FIELD_END
};
SIM_ROWS(s_mil, SIM_STATE, SIM_ID_PLAYER, s_mil_layout);

/* THE TEST LEVERS of the level and the table, read at attach (tagpu_impai_levers), honoured by impai_mil_init
   only, at a live level's init and for a takeover's fresh seat (seat_fresh), never on a puppet: a seat newly
   marked by a takeover takes the taking process's lever, and its IMPAI_MIL, SIM_STATE, is captured from then
   on. tagpu_impai_level.on, `<seat>=easy|normal|hard` comma-separated, sets a seat's level whatever the
   Difficulty, for the bar's ImpAI-against-ImpAI games; tagpu_impai_table.on, `<seat>=<file>` comma-separated,
   the file a table with its FNV-1a 64 trailer (impai_table.h, THE FILE) in the game's folder, for the tuning's
   tournaments. Never player settings. Each file's bytes are read here, at attach, so a level reads no file. */
#define TABLE_PATH 128
static struct {
    uint8_t  level[PLAYER_COUNT];             /* IMPAI_LV_* + 1, 0 when the lever names none for the seat     */
    uint16_t table_n[PLAYER_COUNT];           /* the file's bytes read, 0 when none                           */
    uint8_t  table[PLAYER_COUNT][IMPAI_TABLE_FILE_BYTES];
    char     table_path[PLAYER_COUNT][TABLE_PATH];
} s_milvr;
SIM_VAR(s_milvr, SIM_INSTALL);

/* The seat's level row. BOUND: IMPAI_MIL.level is recorded state, so a value past the rows reads Normal. */
static const IMPAI_LEVEL* seat_level(int seat)
{
    const unsigned lv = s_mil[seat].level;
    return &IMPAI_LEVELS[lv < IMPAI_LVS ? lv : IMPAI_LV_NORMAL];
}

/* The brain's counts, for the level-end `eco` and `work` lines: decisions, what they gave, each decision's
   largest use of each budget against the level row's bound, and the most units of each category any census
   counted, frames included. The most, not the last: a fixture ends its level with the end scenario, which
   clears every unit before the level-end lines are written (scenarios/replay-end.json). Diagnostics the
   simulation never reads. */
static struct {
    unsigned decisions[PLAYER_COUNT], refused[PLAYER_COUNT];
    unsigned orders[PLAYER_COUNT], queued[PLAYER_COUNT], failed[PLAYER_COUNT], army[PLAYER_COUNT];
    unsigned census_max[PLAYER_COUNT], block_max[PLAYER_COUNT], sites_max[PLAYER_COUNT];
    unsigned snaps_max[PLAYER_COUNT], roots_max[PLAYER_COUNT], queue_max[PLAYER_COUNT];
    unsigned sea_plants[PLAYER_COUNT];
    uint16_t count[PLAYER_COUNT][IMPAI_CATS];
} s_play;
SIM_VAR(s_play, SIM_PRIVATE);

static void play_max(unsigned* m, unsigned v) { if (v > *m) *m = v; }

/* THE TURN'S SCRATCH, static so that no turn allocates: the snap's (the patch flood's arena, and the brain's
   snaps; 639 KB, which the process heap would take from VirtualAlloc and zero on every call) and a decision's,
   the economy's (impai_brain_scratch_bytes, at most IMPAI_BRAIN_SCRATCH_MAX) or the military's
   (impai_mil_scratch_bytes, at most IMPAI_MIL_SCRATCH_MAX), one block for both: one turn at a time uses them,
   on the game thread only (impai_ai_tick runs a turn there alone), a turn makes at most one decision of a
   seat (impai_mil_due: the two never share a tick), and every byte is written before the call that takes it
   reads it -- tasim_snap clears its flood arena per flood and fills its lists from the WORLDQ, and both
   decisions clear their state and fill their arrays (the brain's test runs them over unzeroed memory) -- so
   nothing in them crosses a decision. */
#define TURN_SCRATCH (IMPAI_BRAIN_SCRATCH_MAX > IMPAI_MIL_SCRATCH_MAX ? IMPAI_BRAIN_SCRATCH_MAX : IMPAI_MIL_SCRATCH_MAX)
static SNAP_SCRATCH s_turn_snap;
SIM_VAR(s_turn_snap, SIM_TRANSIENT);
static unsigned char s_turn_brain[TURN_SCRATCH] __attribute__((aligned(16)));
SIM_VAR(s_turn_brain, SIM_TRANSIENT);

/* WHAT A SEAT BUILT AND HOLDS, for the pulse line, the level-end `eco` line and the bar: its units completed
   and their value, those of each army class (IMPAI_TINFO.klass), its factories completed by the tech level they
   offer (IMPAI_TINFO.offer, the level tech-up compares) and its water-only factories, and the value of its finished units alive, in metal (IMPAI_TINFO.value). A unit is
   counted once, at the first walk that finds it finished, keyed by its slot's incarnation (tagpu_unit_birth) so
   that the slot's next unit counts again. Without the fail-closed table there is no incarnation and the key is
   the type: a slot's next unit of the same type is then not counted. The walks run at each economy and military
   decision and at the pulse, so a unit finished and destroyed between two of them -- at most the level's decide
   ticks apart -- is not counted either. Diagnostics the simulation never reads. */
#define VALUE_TECHS 4                       /* fbuilt's buckets: offer 0, 1, 2, and 3 or above */
static struct {
    uint32_t key[TAGPU_PK_DESIGN_SLOTS];      /* birth + 1, or 0x80000000 | type with no birth; 0 none     */
    uint32_t built[PLAYER_COUNT];
    uint32_t kbuilt[PLAYER_COUNT][IMPAI_KS];  /* army units completed, by class                             */
    uint32_t fbuilt[PLAYER_COUNT][VALUE_TECHS];
    uint32_t yards[PLAYER_COUNT];             /* factories completed that place on water (IMPAI_TF_WATER)  */
    float    vbuilt[PLAYER_COUNT], vlive[PLAYER_COUNT];
} s_value;
SIM_VAR(s_value, SIM_PRIVATE);

/* the walk over seat `seat`'s unit block through its WORLDQ: every slot bounded by the block and the table */
static void value_walk(int seat, const WORLDQ* q)
{
    int first = 0, last = -1, slot;
    float live = 0.0f;
    if (!q->ops->seat_units(q, &first, &last)) return;
    if (first < 1) first = 1;
    if (last >= TAGPU_PK_DESIGN_SLOTS) last = TAGPU_PK_DESIGN_SLOTS - 1;
    for (slot = first; slot <= last; slot++) {
        WQ_UNIT u;
        unsigned birth;
        uint32_t key;
        if (!q->ops->unit(q, slot, &u) || !u.finished || !u.type || u.type >= s_tinfo_n) continue;
        live += s_tinfo[u.type].value;
        key = tagpu_unit_birth((unsigned)slot, &birth) && birth < 0x7FFFFFFFu ? birth + 1u : 0x80000000u | u.type;
        if (s_value.key[slot] == key) continue;
        s_value.key[slot] = key;
        s_value.built[seat]++;
        s_value.vbuilt[seat] += s_tinfo[u.type].value;
        if (s_tinfo[u.type].klass < IMPAI_KS) s_value.kbuilt[seat][s_tinfo[u.type].klass]++;
        if (s_tinfo[u.type].cat == IMPAI_C_FACTORY) {
            const unsigned offer = s_tinfo[u.type].offer;
            s_value.fbuilt[seat][offer < VALUE_TECHS ? offer : VALUE_TECHS - 1]++;
            if (s_tinfo[u.type].flags & IMPAI_TF_WATER) s_value.yards[seat]++;
        }
    }
    s_value.vlive[seat] = live;
}

/* THE FIGHT PREDICTOR'S CONTENT for the military and the factories (impai_fight.h, IMPAI_FCONTENT): the fight
   tables (THE FIGHT CONTENT above). Each count is bounded by its table's capacity here, so a count that is not
   one the build wrote -- a restored or damaged one -- yields no content rather than a view past the tables;
   with no content the predictor refuses every prediction, every pair rate is 0, the military sends no attack
   wave and the factories buy the cheapest type of each class. The predictor bounds the rest itself: every type
   index against nt, every weapon index a type names against nw, every weapon's [DAMAGE] range against nd.
   GRAVITY is the level's main+0x14263 (an i32 16.16 px a tick squared), which caps a ballistic weapon's reach:
   read here, on the game thread at each decision, and held to 1..65536 as the range rings hold it
   (tagpu_rangeown.c), else 0, no cap. Not captured at the content build: no store to it is known, so its value
   at the level init is not established. replay-globals.tsv keeps the field. */
#define M_GRAVITY 0x14263
static void impai_fight_content(IMPAI_FCONTENT* c, const char* ta)
{
    int32_t g;
    memset(c, 0, sizeof *c);
    memcpy(&g, ta + M_GRAVITY, sizeof g);
    c->gravity = g > 0 && g <= 65536 ? g : 0;
    if (s_ftype_n == 0 || s_ftype_n > TAGPU_LIM_TYPES || s_fweapon_n > IMPAI_FWEAPONS || s_fdmg_n > IMPAI_FDMG_POOL)
        return;
    c->t = s_ftype;
    c->nt = s_ftype_n;
    c->w = s_fweapon;
    c->nw = s_fweapon_n;
    c->d = s_fdmg;
    c->nd = s_fdmg_n;
}

/* the reach labels' view for a decision (impai.h, IMPAI_REACH): 1, or 0 when the level has none */
static int reach_view(IMPAI_REACH* r)
{
    if (!s_reach) return 0;
    memset(r, 0, sizeof *r);
    r->labels = s_reach;
    memcpy(r->at, s_reach_ix.at, sizeof r->at);
    r->present = s_reach_ix.present;
    r->bw = s_reach_ix.bw;
    r->bh = s_reach_ix.bh;
    r->top = s_reach_top;
    r->sea_blocks = s_reach_ix.sea_blocks;
    r->land_total = s_reach_ix.land_total;
    memcpy(r->land_w, s_reach_ix.land_w, sizeof r->land_w);
    r->sea_ref = s_reach_ix.sea_ref;
    return 1;
}

/* One decision of seat `seat` (impai_brain_decide) over its view through the seat's own site test and ImpAI's
   build menus, in the turn's scratch. */
static void impai_decide(int seat)
{
    const size_t bytes = impai_brain_scratch_bytes();
    WQ_ENGINE c;
    WORLDQ q;
    IMPAI_BRAIN_IN in;
    IMPAI_BRAIN_OUT out;
    IMPAI_VIEW v;
    IMPAI_REACH r;
    IMPAI_FCONTENT fc;
    IMPAI_FCTX fx;
    IMPAI_PREDICTOR pred;
    int rc = -1, first = 0, last = -1, k;
    const IMPAI_LEVEL* lv = seat_level(seat);
    if (s_tinfo_n && bytes <= sizeof s_turn_brain &&
        tagpu_worldq_open(&c, &q, seat, WQ_SITE_SEAT, tagpu_impai_menu)) {
        memset(&in, 0, sizeof in);
        in.q = &q;
        in.st = &s_brain[seat];
        in.job = s_job;
        in.njob = TAGPU_PK_DESIGN_SLOTS;
        in.ti = s_tinfo;
        in.nti = s_tinfo_n;
        in.spots = s_cluster[seat];
        in.nspots = s_seatst[seat].nclusters < IMPAI_CLUSTERS_HALF ? s_seatst[seat].nclusters : IMPAI_CLUSTERS_HALF;
        in.reach = reach_view(&r) ? &r : NULL;
        impai_view_of(c.ta, (unsigned)seat, &v);
        in.view = &v;
        in.lv = lv;
        in.scratch = s_turn_brain;
        in.scratch_bytes = bytes;
        in.snap = &s_turn_snap;
        in.mil = &s_mil[seat];
        impai_fight_content(&fc, c.ta);
        fx.c = &fc;
        fx.p = &impai_fitparams;
        pred.rate = impai_fight_rate_of;
        pred.expf = impai_expf;
        pred.ctx = &fx;
        in.pred = &pred;
        rc = impai_brain_decide(&in, &out);
        q.ops->seat_units(&q, &first, &last);
        value_walk(seat, &q);
    }
    if (rc < 0) {
        s_play.refused[seat]++;
        return;
    }
    s_play.decisions[seat]++;
    s_play.orders[seat] += out.orders;
    s_play.queued[seat] += out.queued;
    s_play.failed[seat] += out.failed;
    s_play.army[seat] += out.army;
    s_play.sea_plants[seat] += out.sea_plants;
    play_max(&s_play.census_max[seat], out.census);
    play_max(&s_play.block_max[seat], last >= first ? (unsigned)(last - first + 1) : 0u);
    play_max(&s_play.sites_max[seat], out.sites);
    play_max(&s_play.snaps_max[seat], out.snaps);
    play_max(&s_play.roots_max[seat], out.roots);
    play_max(&s_play.queue_max[seat], out.queued);
    for (k = 0; k < IMPAI_CATS; k++)
        if (out.count[k] > s_play.count[seat][k]) s_play.count[seat][k] = out.count[k];
}

/* The military's counts, for the level-end `mil` line: decisions, refusals, what they gave and began, and each
   decision's largest use of its bounds. The last decision's army, for the pulse. Diagnostics the simulation
   never reads. */
static struct {
    unsigned decisions[PLAYER_COUNT], refused[PLAYER_COUNT];
    unsigned predictions[PLAYER_COUNT], refused_predictions[PLAYER_COUNT], roots[PLAYER_COUNT];
    unsigned refused_orders[PLAYER_COUNT], left_out[PLAYER_COUNT];
    unsigned attacks[PLAYER_COUNT], raids[PLAYER_COUNT], retreats[PLAYER_COUNT], scouts[PLAYER_COUNT];
    unsigned defends[PLAYER_COUNT], safe[PLAYER_COUNT], dguns[PLAYER_COUNT];
    unsigned army_max[PLAYER_COUNT], enemies_max[PLAYER_COUNT], contacts_max[PLAYER_COUNT];
    unsigned predict_max[PLAYER_COUNT], roots_max[PLAYER_COUNT];
    unsigned army[PLAYER_COUNT];
    unsigned unreached[PLAYER_COUNT], cross[PLAYER_COUNT];
} s_milp;
SIM_VAR(s_milp, SIM_PRIVATE);

static void impai_mil_turn(int seat);   /* the military's region, below */

/* THE PULSE, once a game minute a seat, on its economy phase (GameTime % 1800 == 7 seat % 15): `impai: pulse
   seat= t= built= mroots= eroots= army= squads= metal= energy= vlive= vbuilt=` -- units completed, the
   military's and the economy's orders given, all since the level began; the last military census's army;
   each squad's task letter (impai_mil_letter), `-` for none; the seat's TotalMetalProduced +0xB4 and
   TotalEnergyProduced +0xAC (doubles, engine/recorder.md); the value of its finished units alive and of its
   units completed. Always on: the bar judges stalls from it (tools/impai/bar.py), and a player's log carries
   it for a report. */
static void impai_pulse(int seat, const char* ta, unsigned now)
{
    const char* rec = ta + OFF_PLAYERS + (unsigned)seat * PLAYER_STRIDE;
    char sq[IMPAI_SQUADS + 1];
    WQ_ENGINE c;
    WORLDQ q;
    int k, n = 0;
    if (s_tinfo_n && tagpu_worldq_open(&c, &q, seat, WQ_SITE_SEAT, tagpu_impai_menu)) value_walk(seat, &q);
    for (k = 0; k < IMPAI_SQUADS; k++)
        if (s_mil[seat].sq[k].task != IMPAI_SQ_FREE) sq[n++] = impai_mil_letter(s_mil[seat].sq[k].task);
    if (!n) sq[n++] = '-';
    sq[n] = 0;
    tagpu_logf("impai: pulse seat=%d t=%u built=%u mroots=%u eroots=%u army=%u squads=%s metal=%.0f energy=%.0f "
               "vlive=%.0f vbuilt=%.0f", seat, now, s_value.built[seat], s_mil[seat].pulse_roots,
               s_play.orders[seat], s_milp.army[seat], sq, *(const double*)(rec + 0xB4),
               *(const double*)(rec + 0xAC), s_value.vlive[seat], s_value.vbuilt[seat]);
}

/* One ImpAI seat's turn on the game thread: its observation, then the brain on its decision tick
   (impai_brain_due: GameTime % 15 == 7 x seat % 15, so no two seats decide on one tick), and nothing else
   between decisions. THE COST covers both, the views' opens included, and not the self-checks, which run after
   it. */
static void impai_turn(int seat)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const unsigned now = *(const unsigned*)(ta + OFF_GAMETIME);
    IMPAI_WORK w;
    LARGE_INTEGER t0, t1;
    unsigned flags = 0;
    s_diag.calls[seat]++;
    QueryPerformanceCounter(&t0);
    if (impai_observe(seat, &s_turn_snap, &w) == 0) {
        if (s_seatst[seat].radar.stage == WQ_RS_IDLE && s_seatst[seat].radar_done == now)
            flags |= 1u << TAIL_PUBLISH;
        if (w.scans) flags |= 1u << TAIL_SCAN;
    }
    if (impai_brain_due(now, (unsigned)seat, seat_level(seat))) {
        impai_decide(seat);
        flags |= 1u << TAIL_DECIDE;
    } else if (impai_mil_due(now, (unsigned)seat, seat_level(seat))) {
        impai_mil_turn(seat);
        flags |= 1u << TAIL_DECIDE;
    }
    QueryPerformanceCounter(&t1);
    cost_note(seat, t1.QuadPart - t0.QuadPart, flags, &w, now);
    impai_observe_checks(seat);
    if (now % 1800u == (7u * (unsigned)seat) % 15u) impai_pulse(seat, ta, now);
}

#define VA_AI_SCAN 0x004089A0u     /* the weapon scan, thiscall (obj, mode), `ret 4` */
typedef void (__thiscall* PFN_AiScan)(void* obj, int mode);

/* THE WRAPPER, the target of the call at 0x465031. The site calls 0x408C40 as thiscall with no
   stack argument (ECX the AI object, 0x46502A) and expects a plain `ret` (0x408CA2, 0x408CAF).
   __fastcall takes ECX as its first argument and EDX as its second and pops nothing, so the
   site's stack is unchanged; EBX, ESI, EDI and EBP are callee-saved in every convention, so the
   loop counter BL, the record EDI and the record index ESI that the players phase uses after the
   call (0x465036 `push esi`, 0x46506A `cmp bl,[eax+0x2A43]`) survive.
   A seat ImpAI does not play takes the stock tick 0x408C40, and the recorder's observer on its entry
   (tagpu_recorder.c, ai_enter) sees the call. AN IMPAI SEAT NEVER RUNS 0x408C40's group assignment
   0x408830 or its ten planners, on either thread: they would give its units orders of their own. What it
   runs of the tick is the weapon scan 0x4089A0, called by address so that the extra-weapons module's detour of
   its entry runs, WITH ARGUMENT 0, as every record but an active type-2 one gets it at 0x408C40's end -- a
   human seat's: CommandFire weapons are left to orders (0x408A88..0x408A96; the detour's port tests the same
   argument), and the military orders the commander's D-gun itself (impai_mil.c, the D-gun). The stock AI's
   argument 1 (0x408C96..0x408C9A, DISASSEMBLED) would fire the D-gun at whatever came in range, 400 energy a
   shot, whatever the economy held (research/notes/impai/decisions.md). The players phase also runs once on the LOADER thread (0x497C10), where only the
   scan runs, as there the stock tick would plan and scan. On the GAME thread the turn and the scan run
   inside the recorder's attribution bracket, as the stock tick runs inside ai_enter/ai_leave: orders built in
   the turn are the seat's, and the scan's detour ends the attribution before it scans
   (tagpu_rec_ai_planning_end). */
static void __fastcall impai_ai_tick(void* obj, void* edx_unused)
{
    const int seat = impai_seat_marked(obj);
    (void)edx_unused;
    if (seat < 0) {
        ((PFN_AiTick)(uintptr_t)VA_AI_TICK)(obj);
        return;
    }
    if (GetCurrentThreadId() == g_ddraw.gui_thread_id) {
        const int pushed = tagpu_rec_ai_turn_enter((unsigned)seat);
        impai_turn(seat);
        ((PFN_AiScan)(uintptr_t)VA_AI_SCAN)(obj, 0);
        tagpu_rec_ai_turn_leave(pushed);
    } else {
        ((PFN_AiScan)(uintptr_t)VA_AI_SCAN)(obj, 0);
    }
}

/* `name:count` for each nonzero count of `n`, comma-separated, `-` for none; `names` NULL names each by its
   index. Bounded by `cap`: a pair that does not fit is left out whole, which a caller's n x LIST_ITEM + 1 bytes
   never meets with names of at most 12 characters (impai_class_name's longest, `lartillery`, is 10). */
#define LIST_ITEM 24
static void count_list(char* out, size_t cap, const uint32_t* count, unsigned n, const char* (*names)(unsigned))
{
    size_t at = 0;
    unsigned k;
    out[0] = 0;
    for (k = 0; k < n; k++) {
        char item[48];
        int len;
        if (!count[k]) continue;
        len = names ? snprintf(item, sizeof item, "%s%s:%u", at ? "," : "", names(k), count[k])
                    : snprintf(item, sizeof item, "%s%u:%u", at ? "," : "", k, count[k]);
        if (len <= 0 || (size_t)len >= sizeof item || at + (size_t)len >= cap) continue;
        memcpy(out + at, item, (size_t)len + 1);
        at += (size_t)len;
    }
    if (!at && cap > 1) memcpy(out, "-", 2);
}

/* The brain's level-end lines, for each seat that decided or was refused: what it ordered, the most of each
   category its censuses counted, the record's last pass's rates (the record of the seat, one of the ten by index), and each budget's
   largest use against the level row's bound -- the census against the seat's unit block. The `eco` line ends with
   what the seat completed (s_value): its units, its army by class, its factories by the tech level they offer
   (`ftech=`, the last bucket 3 and above) and its water-only factories; the `mil` line with the metal income a pass at
   which the seat techs up (`tech_m=`, the table's times the level row's tech_scale, impai_brain.c TECH-UP). */
static void impai_play_end(const char* ta, unsigned now)
{
    unsigned i;
    for (i = 0; i < PLAYER_COUNT; i++) {
        const char* rec = ta + OFF_PLAYERS + i * PLAYER_STRIDE;
        const uint16_t* k = s_play.count[i];
        const IMPAI_LEVEL* lv = seat_level((int)i);
        /* LIST_ITEM a pair: the longest, `,lartillery:` and ten digits, is 22 bytes, so every pair fits */
        char classes[IMPAI_KS * LIST_ITEM + 1], ftech[VALUE_TECHS * LIST_ITEM + 1];
        if (!s_play.decisions[i] && !s_play.refused[i]) continue;
        count_list(classes, sizeof classes, s_value.kbuilt[i], IMPAI_KS, impai_class_name);
        count_list(ftech, sizeof ftech, s_value.fbuilt[i], VALUE_TECHS, NULL);
        tagpu_logf("impai: eco seat=%u decisions=%u orders=%u queued=%u failed=%u army=%u mex=%u energy=%u makers=%u "
                   "factories=%u cons=%u metal_in=%.1f energy_in=%.1f gametime=%u sea_plants=%u built=%u classes=%s "
                   "ftech=%s yards=%u", i, s_play.decisions[i],
                   s_play.orders[i], s_play.queued[i], s_play.failed[i], s_play.army[i], k[IMPAI_C_MEX],
                   k[IMPAI_C_ENERGY],
                   k[IMPAI_C_MAKER], k[IMPAI_C_FACTORY], k[IMPAI_C_CONS], *(const float*)(rec + 0x9C),
                   *(const float*)(rec + 0x90), now, s_play.sea_plants[i], s_value.built[i], classes, ftech,
                   s_value.yards[i]);
        tagpu_logf("impai: work seat=%u decisions=%u refused=%u census_max=%u/%u sites_max=%u/%u snaps_max=%u/%u "
                   "roots_max=%u/%u queue_max=%u/%u", i, s_play.decisions[i], s_play.refused[i],
                   s_play.census_max[i], s_play.block_max[i], s_play.sites_max[i], lv->site_tests,
                   s_play.snaps_max[i], lv->snaps, s_play.roots_max[i], lv->roots, s_play.queue_max[i],
                   lv->queue_max);
        tagpu_logf("impai: mil seat=%u level=%u decisions=%u refused=%u predictions=%u refused_predictions=%u "
                   "roots=%u refused_orders=%u left_out=%u attacks=%u raids=%u retreats=%u scouts=%u defends=%u "
                   "safe=%u dguns=%u army_max=%u/%u enemies_max=%u/%u contacts_max=%u/%u predict_max=%u/%u "
                   "roots_max=%u/%u naval=%d unreached=%u cross=%u tech_m=%.1f",
                   i, s_mil[i].level, s_milp.decisions[i], s_milp.refused[i], s_milp.predictions[i],
                   s_milp.refused_predictions[i], s_milp.roots[i], s_milp.refused_orders[i], s_milp.left_out[i],
                   s_milp.attacks[i], s_milp.raids[i], s_milp.retreats[i], s_milp.scouts[i], s_milp.defends[i],
                   s_milp.safe[i], s_milp.dguns[i], s_milp.army_max[i], IMPAI_ARMY_MAX, s_milp.enemies_max[i], IMPAI_ENEMY_MAX,
                   s_milp.contacts_max[i], IMPAI_CONTACTS, s_milp.predict_max[i], lv->predict_max,
                   s_milp.roots_max[i], lv->mil_roots, s_mil[i].naval, s_milp.unreached[i], s_milp.cross[i],
                   (double)(s_mil[i].table.tech_m * lv->tech_scale));
    }
}

/* The level init's half of the brain, on every level, live or puppet: no anchor, no failure, no explorer and
   no job survive a level, and a puppet's come from its recording. */
static void impai_play_clear(void)
{
    unsigned i;
    for (i = 0; i < PLAYER_COUNT; i++) impai_brain_reset(&s_brain[i], s_job, i ? 0u : TAGPU_PK_DESIGN_SLOTS);
    memset(s_mil, 0, sizeof s_mil);
    memset(&s_play, 0, sizeof s_play);
    memset(&s_milp, 0, sizeof s_milp);
    memset(&s_value, 0, sizeof s_value);
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
    if (s_install.in && ta) {
        now = *(const unsigned*)(ta + OFF_GAMETIME);
        for (i = 0; i < PLAYER_COUNT; i++)
            if ((s_seat[i] & IMPAI_ON) || s_diag.calls[i])
                tagpu_logf("impai: calls seat=%u n=%u gametime=%u", i, s_diag.calls[i], now);
        handicap_counts(line);
        tagpu_logf("impai: handicap rows scaled/exempt gametime=%u:%s", now, line);
        impai_observe_end(now);
        impai_play_end(ta, now);
    }
    memset(s_seat, 0, sizeof s_seat);
}

/* No content: every table the level init builds emptied whole, the bytes past each count included, so the
   content of a level that builds none is the same in every process whatever its earlier levels built. */
static void impai_content_clear(void)
{
    if (s_reach) SIM_FREE(s_reach, s_reach);
    s_reach = NULL;
    s_reach_n = 0;
    if (s_reach_top) SIM_FREE(s_reach_top, s_reach_top);
    s_reach_top = NULL;
    s_reach_top_n = 0;
    memset(&s_reach_ix, 0, sizeof s_reach_ix);
    memset(s_threat_def, 0, sizeof s_threat_def);
    s_threat_n = 0;
    memset(s_tinfo, 0, sizeof s_tinfo);
    s_tinfo_n = 0;
    memset(s_menu, 0, sizeof s_menu);
    memset(s_menu_pool, 0, sizeof s_menu_pool);
    s_nmenu = s_npool = 0;
    impai_fight_clear();
    s_content = 0;
}

/* THE REACH SUMMARY (impai_reach_summary), once the type information is whole: the sea blocks, the land army's
   classes and the sea plant's class, which the naval judgement and the sea plant read. Content, as the labels. */
static void impai_reach_summarise(void)
{
    IMPAI_REACH r;
    if (!reach_view(&r)) return;
    impai_reach_summary(&r, s_tinfo, s_tinfo_n);
    s_reach_ix.sea_blocks = r.sea_blocks;
    s_reach_ix.land_total = r.land_total;
    memcpy(s_reach_ix.land_w, r.land_w, sizeof s_reach_ix.land_w);
    s_reach_ix.sea_ref = r.sea_ref;
    tagpu_logf("impai: reach summary sea_blocks=%u of %u land_total=%u sea_ref=%d mostly_water=%d", r.sea_blocks,
               (unsigned)r.bw * r.bh, r.land_total, r.sea_ref == 0xFF ? -1 : (int)r.sea_ref, impai_mostly_water(&r));
}

/* The content of a level a seat plays ImpAI on: the reach labels, the threat table and type information,
   the fight content and the menus of every side loaded. All or none: 1, or 0 with no content when the fight
   content cannot be had or the menus are past their tables. On the LOADER thread inside 0x4919C8 (the level
   init), where every input is the load's. The menus come first: the classification's tech walk reads them. */
static int impai_content_build(void)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    const unsigned loaded = ta ? *(const unsigned*)(ta + M_SIDES) : 0u;
    impai_reach_build();
    if (impai_menus_build((1u << (loaded < SIDE_ROOM ? loaded : SIDE_ROOM)) - 1u) && impai_threat_build()) {
        impai_reach_summarise();
        s_content = 1;
        return 1;
    }
    impai_content_clear();
    return 0;
}


/* The military (impai_mil.c): its turn on its own ticks inside the same turn, its levers and its init.
   OWNER as impai_play: it decides only for a live ImpAI seat, through the same refusing WORLDQ; its init
   runs for a live level's seats and for a takeover's fresh seat (seat_fresh), never on a puppet, whose
   IMPAI_MIL comes from the recording. */
SIM_FEATURE(impai_mil, SIM_PUPPET_OWNER, "impai-fight stock-impai-fight impai-eco impai-observe impai-naval stock-impai-naval");

/* One military decision of seat `seat` (impai_mil_decide) over the same view as the economy's, in the turn's
   scratch. */
static void impai_mil_turn(int seat)
{
    const size_t bytes = impai_mil_scratch_bytes();
    WQ_ENGINE c;
    WORLDQ q;
    IMPAI_MIL_IN in;
    IMPAI_MIL_OUT out;
    IMPAI_VIEW v;
    IMPAI_FCONTENT fc;
    IMPAI_REACH r;
    int rc = -1;
    if (s_tinfo_n && bytes <= sizeof s_turn_brain &&
        tagpu_worldq_open(&c, &q, seat, WQ_SITE_SEAT, tagpu_impai_menu)) {
        memset(&in, 0, sizeof in);
        impai_view_of(c.ta, (unsigned)seat, &v);
        in.reach = reach_view(&r) ? &r : NULL;
        impai_fight_content(&fc, c.ta);
        in.q = &q;
        in.view = &v;
        in.st = &s_mil[seat];
        in.job = s_job;
        in.njob = TAGPU_PK_DESIGN_SLOTS;
        in.ti = s_tinfo;
        in.nti = s_tinfo_n;
        in.fc = &fc;
        in.fit = &impai_fitparams;
        in.brain = &s_brain[seat];
        in.lv = seat_level(seat);
        in.scratch = s_turn_brain;
        in.scratch_bytes = bytes;
        rc = impai_mil_decide(&in, &out);
        value_walk(seat, &q);
    }
    if (rc < 0) {
        s_milp.refused[seat]++;
        return;
    }
    s_milp.decisions[seat]++;
    s_milp.predictions[seat] += out.predictions;
    s_milp.refused_predictions[seat] += out.refused_predictions;
    s_milp.roots[seat] += out.roots;
    s_milp.refused_orders[seat] += out.refused;
    s_milp.left_out[seat] += out.left_out;
    s_milp.attacks[seat] += out.attacks;
    s_milp.raids[seat] += out.raids;
    s_milp.retreats[seat] += out.retreats;
    s_milp.scouts[seat] += out.scouts;
    s_milp.defends[seat] += out.defends;
    s_milp.safe[seat] += out.safe;
    s_milp.dguns[seat] += out.dgun;
    s_milp.unreached[seat] += out.unreached;
    s_milp.cross[seat] += out.cross;
    s_milp.army[seat] = out.army;
    play_max(&s_milp.army_max[seat], out.army);
    play_max(&s_milp.enemies_max[seat], out.enemies);
    play_max(&s_milp.contacts_max[seat], out.contacts);
    play_max(&s_milp.predict_max[seat], out.predictions);
    play_max(&s_milp.roots_max[seat], out.roots);
    if (out.attacks || out.raids || out.scouts || out.retreats || out.safe || out.dgun)
        tagpu_logf("impai: mil seat=%d t=%u attack=%u raid=%u scout=%u retreat=%u safe=%u dgun=%u army=%u",
                   seat, (unsigned)q.ops->gametime(&q), out.attacks, out.raids, out.scouts, out.retreats, out.safe,
                   out.dgun, out.army);
}

static const char* const LEVEL_NAME[IMPAI_LVS] = { "easy", "normal", "hard" };

/* The two military levers' texts (s_milvr): each `<seat>=<value>` item, the seat one digit; an item not
   understood is logged and skipped. A table file is read whole here, up to one byte past its size so that a
   longer file is refused by size at the level init. */
static void impai_mil_levers(void)
{
    char t[PLAYER_COUNT * (TABLE_PATH + 4)];
    char* p;
    int n;
    if ((n = tagpu_sim_lever_read("tagpu_impai_level.on", t, (int)sizeof t)) > 0)
        for (p = strtok(t, ", \r\n\t"); p; p = strtok(NULL, ", \r\n\t")) {
            unsigned k;
            for (k = 0; k < IMPAI_LVS && !(p[0] >= '0' && p[0] <= '9' && p[1] == '=' && !lstrcmpiA(p + 2, LEVEL_NAME[k])); k++) {}
            if (k == IMPAI_LVS) {
                tagpu_logf("impai: tagpu_impai_level.on: `%s` not understood (<seat>=easy|normal|hard)", p);
                continue;
            }
            s_milvr.level[p[0] - '0'] = (uint8_t)(k + 1);
            tagpu_logf("impai: level lever: seat %c plays %s", p[0], LEVEL_NAME[k]);
        }
    if ((n = tagpu_sim_lever_read("tagpu_impai_table.on", t, (int)sizeof t)) > 0)
        for (p = strtok(t, ", \r\n\t"); p; p = strtok(NULL, ", \r\n\t")) {
            uint8_t buf[IMPAI_TABLE_FILE_BYTES + 1];
            DWORD got = 0;
            HANDLE f;
            unsigned seat;
            if (!(p[0] >= '0' && p[0] <= '9' && p[1] == '=' && p[2]) || lstrlenA(p + 2) >= TABLE_PATH) {
                tagpu_logf("impai: tagpu_impai_table.on: `%s` not understood (<seat>=<file>)", p);
                continue;
            }
            seat = (unsigned)(p[0] - '0');
            lstrcpynA(s_milvr.table_path[seat], p + 2, TABLE_PATH);
            f = CreateFileA(p + 2, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (f == INVALID_HANDLE_VALUE || !ReadFile(f, buf, (DWORD)sizeof buf, &got, NULL)) got = 0;
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            s_milvr.table_n[seat] = (uint16_t)got;
            memcpy(s_milvr.table[seat], buf, got < IMPAI_TABLE_FILE_BYTES ? got : IMPAI_TABLE_FILE_BYTES);
            tagpu_logf("impai: table lever: seat %u reads %s, %u bytes", seat, p + 2, (unsigned)got);
        }
}

/* A live seat's military at the level init (tagpu_impai_level_init) or a takeover's fresh seat
   (tagpu_impai_takeover): its level from the level lever, else impai_seats_level -- Hard in a network game,
   else Difficulty main+0x37EEE (any other value plays Normal, and says so); its seed from the simulation RNG's
   state 0x51FC88 xor the seat, READ and never drawn (impai.h, impai_noise), which the loader 0x497180 seeded at
   0x49719D before 0x4917D0 runs PLAYERS, inside which the level init runs (engine/sim.md), and which a
   takeover finds as the recording carries it (replay-globals.tsv, `rngseed`); its table from the table lever's
   file when it loads (impai_table_load bounds it, and refuses it whole), else IMPAI_TABLE_DEFAULT. Each says
   what it chose in one line, the table's as `impai: table seat= hash= from=`, the hash the file's trailer or
   the default's own. */
#define VA_RNG_STATE 0x0051FC88u
typedef char impai_seats_levels_are_the_rows[IMPAI_LEVEL_HARD == IMPAI_LV_HARD && IMPAI_LV_EASY == 0 &&
                                             IMPAI_LV_NORMAL == 1 ? 1 : -1];
static void impai_mil_init(const char* ta, unsigned seat)
{
    const uint32_t difficulty = *(const uint32_t*)(ta + OFF_DIFFICULTY);
    const int network = (*(const unsigned char*)(ta + OFF_NETFLAGS) & 1) != 0;
    const uint64_t seed = (uint64_t)*(const uint32_t*)(uintptr_t)VA_RNG_STATE ^ seat;
    IMPAI_TABLE table = IMPAI_TABLE_DEFAULT;
    uint64_t hash = impai_table_hash(&IMPAI_TABLE_DEFAULT);
    int level = impai_seats_level(network, difficulty), lever = 0;
    if (s_milvr.level[seat]) level = s_milvr.level[seat] - 1;
    else if (level < 0) {
        tagpu_logf("impai: level seat=%u: Difficulty %u is none of 0..2, so Normal", seat, difficulty);
        level = IMPAI_LV_NORMAL;
    }
    if (s_milvr.table_path[seat][0]) {
        IMPAI_TABLE t;
        uint64_t h;
        const int rc = impai_table_load(s_milvr.table[seat], s_milvr.table_n[seat], &t, &h);
        if (rc == IMPAI_TABLE_OK) {
            table = t;
            hash = h;
            lever = 1;
        } else {
            tagpu_logf("impai: table seat=%u: %s refused: %s", seat, s_milvr.table_path[seat],
                       impai_table_error(rc));
        }
    }
    impai_mil_reset(&s_mil[seat], (unsigned)level, seed, &table, lever);
    tagpu_logf("impai: level seat=%u level=%s from=%s", seat, LEVEL_NAME[s_mil[seat].level],
               s_milvr.level[seat] ? "lever" : "difficulty");
    tagpu_logf("impai: table seat=%u hash=%08x%08x from=%s", seat, (unsigned)(hash >> 32), (unsigned)hash,
               lever ? "lever" : "default");
}

/* The level init's seat choice, the takeover's seats, the AI command and the header's accessor: what decides
   which seats ImpAI plays. On a puppet the level init and the level end only clear the table and the AI command
   clears no mark, so between the two clears the recording is the only writer. impai-takeover-fresh and its
   stock twin take over a two-player game with `seatai` naming the seat OTA AI played, so seat_fresh runs on
   both builds. */
SIM_FEATURE(impai_seats, SIM_PUPPET_OWNER, "impai-handicap impai-observe impai-observe-perm stock-impai-observe impai-takeover-fresh stock-impai-takeover-fresh");

typedef char impai_seat_recs_are_the_players[IMPAI_SEAT_RECS == PLAYER_COUNT ? 1 : -1];

/* The ten records as the seat rule reads them (impai_seats.h): seated, controller, AI object, PlayerInfo's
   type and whether the long name begins ImpAI:. PlayerInfo objects live for the process (record_side), so
   +0x94 is read as a value; the name is read within its 30-byte field. */
static void seat_recs(const char* ta, IMPAI_SEAT_REC rec[IMPAI_SEAT_RECS])
{
    unsigned i;
    for (i = 0; i < PLAYER_COUNT; i++) {
        const char* r = ta + OFF_PLAYERS + i * PLAYER_STRIDE;
        const unsigned char* info = *(const unsigned char* const*)(r + PL_INFO);
        rec[i].active = *(const uint32_t*)(r + PL_ACTIVE) != 0;
        rec[i].control = (uint8_t)r[PL_CONTROL];
        rec[i].ai_object = *(void* const*)(r + PL_AIOBJ) != NULL;
        rec[i].info_ai = info && info[PI_TYPE] == CTRL_AI;
        rec[i].named = (uint8_t)impai_seats_named(r + PL_NAME, IMPAI_NAME_FIELD);
    }
}

/* The table holds exactly the marks `marks`, bit i for record i. */
static void seats_set(unsigned marks)
{
    unsigned i;
    for (i = 0; i < PLAYER_COUNT; i++) s_seat[i] = (uint8_t)(marks >> i & 1u ? IMPAI_ON : 0u);
}

/* A takeover's seat ImpAI did not play in the recording, as a level init leaves it: its column of every
   slot-keyed table and its own rows zero, its brain reset, the jobs of its unit block cleared, a new military.
   BOUND: `seat` below PLAYER_COUNT (= WQ_SEATS, the columns), the radar records below their count, the jobs
   within the block seat_units gives and the job table. Game thread (tagpu_impai_takeover). */
static void seat_fresh(const char* ta, unsigned seat)
{
    WQ_ENGINE c;
    WORLDQ q;
    unsigned k;
    int first = 0, last = -1, slot;
    for (k = 0; k < TAGPU_PK_DESIGN_SLOTS; k++) memset(&s_known[k].seat[seat], 0, sizeof s_known[k].seat[seat]);
    for (k = 0; k < RADAR_HIT_ROWS; k++) memset(&s_radar_hit[k].seat[seat], 0, sizeof s_radar_hit[k].seat[seat]);
    memset(&s_seatst[seat], 0, sizeof s_seatst[seat]);
    memset(s_radar_bits[seat], 0, sizeof s_radar_bits[seat]);
    memset(s_cluster[seat], 0, sizeof s_cluster[seat]);
    impai_brain_reset(&s_brain[seat], s_job, 0);
    if (tagpu_worldq_open(&c, &q, (int)seat, WQ_SITE_SEAT, tagpu_impai_menu) && q.ops->seat_units(&q, &first, &last)) {
        if (first < 0) first = 0;
        if (last >= TAGPU_PK_DESIGN_SLOTS) last = TAGPU_PK_DESIGN_SLOTS - 1;
        for (slot = first; slot <= last; slot++) {
            memset(&s_job[slot], 0, sizeof s_job[slot]);
            s_job[slot].type = s_job[slot].build = s_job[slot].target = (uint16_t)WQ_NONE;
        }
    } else {
        tagpu_logf("impai: seat %u's unit block unread at the takeover: its jobs stand to the census", seat);
    }
    impai_mil_init(ta, seat);
}

/* The level's start positions as the `starts` op answers them, once at a live level's init with an ImpAI
   seat: `impai: starts n=<n> <index>:<x>,<z> ...`, world px, which the military's base estimate and scouting
   read, for a check against the commanders' positions under fixed starts. On the LOADER thread inside
   0x4919C8: the array was written by the SKIRMISH Start's .ota read before the loader thread began (N-b1). */
static void impai_starts_line(void)
{
    WQ_ENGINE c;
    WORLDQ q;
    WQ_START s[WQ_STARTS_MAX];
    char line[WQ_STARTS_MAX * 20 + 32];
    int n, k, at;
    if (!tagpu_worldq_open_content(&c, &q)) return;
    n = q.ops->starts(&q, s, WQ_STARTS_MAX);
    at = wsprintfA(line, "impai: starts n=%d", n);
    for (k = 0; k < n; k++) at += wsprintfA(line + at, " %d:%d,%d", s[k].index, s[k].x, s[k].z);
    tagpu_logf("%s", line);
}

/* THE LEVEL INIT, on the loader thread inside the PLAYERS call 0x4919C8, right after the
   engine's players init 0x464990 has seated and activated every record and before the loader's
   last store (0x497C62), which the game thread waits for before the level's first step: so the
   table is whole before any game-thread turn reads it (impai_seat_marked, THE THREAD). It clears
   the table on every level (s_seat); on a puppet level it marks no seat: the recording restores
   the table, and the recorder is the only writer of what a recording carries.
   THE SEAT CHOICE (impai_seats.c, which has the rule) marks only a record the engine runs an AI for
   on this peer: seated, controller 2, its AI object +0x74 set, which 0x464990's player init
   0x464700 builds for every record but an active type 3 (0x46489D..0x4648C1). In a skirmish the
   records are impure.cfg's `seatai`, bit i for settings entry i, which is record i: the seat fill
   0x47A760 activates record i for entry i on every path that seats a skirmish (engine/sim.md,
   Seating a level). In a network game (main+0x2A44 bit 0) an AI seat is type 2 with its AI object
   only on the peer that added it and type 3 without one on every other (engine/network.md, A
   computer seat in a network game), and the seam 0x465031 runs only where +0x74 is set, so ImpAI
   plays a seat on its own peer alone, by the engine's structure: which of this peer's AI seats is
   the record's own name, ImpAI:<human> as the battle room's add or button named it (tagpu_skirmish.c,
   THE BATTLE ROOM), which moves with the record when the room compacts its rows. The mark is this
   peer's record index and ImpAI sends nothing to another peer, so a row index that differs there is
   never used. A type-3 record outside a network game marks none, and the line says which.
   THE CONTENT -- the reach labels, the threat table and type information, the menus of every side loaded --
   is SIM_CONTENT, so it is built here and only here, on this thread, from what the load made: the class maps
   as 0x440940 built them, before any unit or feature of play changes them (engine/map.md), the defs and the
   files. A live level builds it when a seat is marked, and a level whose menus are past their tables marks
   none, so its header names no ImpAI seat; a puppet level builds it whenever ImpAI is available, so a takeover
   or a rewind of it finds the same content the live game had and builds none in play. Every other level has
   none (impai_content_clear). */
void tagpu_impai_level_init(void)
{
    const char* ta;
    char list[2 * IMPAI_SEAT_RECS], remote[2 * IMPAI_SEAT_RECS], rimpai[2 * IMPAI_SEAT_RECS];
    IMPAI_SEAT_REC rec[IMPAI_SEAT_RECS];
    IMPAI_SEAT_PICK pick;
    unsigned i;
    int network;
    memset(&s_diag, 0, sizeof s_diag);
    memset(s_seat, 0, sizeof s_seat);
    s_ovc.built = 0;
    s_known_tick = 0;
    impai_observe_clear();
    impai_play_clear();
    impai_content_clear();
    if (tagpu_puppet_on()) {
        if (s_install.avail) impai_content_build();
        return;
    }
    ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    if (!ta) return;
    seat_recs(ta, rec);
    network = (*(const unsigned char*)(ta + OFF_NETFLAGS) & 1) != 0;
    impai_seats_pick(rec, network, (unsigned)tagpu_settings_seatai(), &pick);
    if (pick.refused >= 0) {
        tagpu_logf("impai: seats none: record %d is remote outside a network game", pick.refused);
        return;
    }
    if (!s_install.avail) {
        tagpu_logf("impai: unavailable: %s", s_install.why);
        return;
    }
    if (pick.marks && !impai_content_build()) pick.marks = 0;   /* the menus' line says why */
    seats_set(pick.marks);
    for (i = 0; i < PLAYER_COUNT; i++)
        if (s_seat[i] & IMPAI_ON) impai_mil_init(ta, i);
    if (pick.marks) impai_starts_line();
    impai_seats_list(pick.marks, list);
    if (network) {
        impai_seats_list(pick.remote_ai, remote);
        impai_seats_list(pick.remote_impai, rimpai);
        tagpu_logf("impai: seats %s (netai=%s, a network game, level %u) of %u records; other peers' AI seats %s, "
                   "ImpAI's %s", list, tagpu_settings_netai() ? "impai" : "ota",
                   (unsigned)impai_seats_level(1, *(const uint32_t*)(ta + OFF_DIFFICULTY)), pick.live, remote, rimpai);
    } else {
        tagpu_logf("impai: seats %s (impure.cfg) of %u records", list, pick.live);
    }
}

/* THE TAKEOVER'S SEATS. A takeover makes every recorded seat but the player's P a fresh stock AI (tagpu_replay.c,
   TK1: new AI objects through 0x408CB0) and P a human's, type 1 through the engine's setter (tk_types). ImpAI's
   seats are then the seats it played in the recording -- the marks R13 (recorded_put) restored into s_seat,
   read here, before anything below writes the table -- and any seat the store's `seatai` adds, bit i for
   record i, both through the level init's rule (impai_seats_pick), which marks only a seated controller-2
   record with its AI object: so P, now type 1, is never ImpAI's, and no other recorded seat loses ImpAI. A
   player who takes over a game against ImpAI plays on against ImpAI, whatever store the playing process has.
   A seat ImpAI played in the recording and still plays KEEPS what R13 restored -- its knowledge, its brain, its jobs, its military with the recorded level, seed and
   table -- so the fork goes on from the recorded state as a rewind's does, and its first capture states the
   parent's tables at the fork for every kept seat (tarec compare-fork compares every declared table). A seat
   ImpAI did not play there starts fresh, as at a level's start (seat_fresh), so its mark, brain, jobs and
   military differ from the parent's by design: compare-fork leaves those four tables' rows of such a seat out
   at the fork's first capture only, the seat named by the fork's header and not by its parent's (tarec.py,
   fork_view's (e)), and compares every later capture in full. A seat ImpAI no longer plays keeps its rows, which no turn
   reads, and loses its knowledge generation, which the overlay and the digests' log test for a seat to show
   (tagpu_impai_overlay, tagpu_impai_frame), so its frozen knowledge is not drawn. The content is the puppet level init's, built from the load
   (THE CONTENT above), so the takeover builds none; a level without it marks no seat.
   Called by tk_end on the GAME thread inside the takeover's frame visit, after tagpu_puppet_end() and after
   R13, and before tagpu_rec_fork_arm, so the fork's first capture, its header's ImpAI byte included, states
   these marks. Not on a rewind's fork: it restores the saved present's own seats and keeps their marks (the
   caller's test).
   THE THREAD AND THE ORDER: the loader thread is done (the level is in play), the render thread reads none
   of these tables (the overlay's fill runs on the game thread), and the players phase runs in a later step,
   so the table is whole before any turn reads it. The playing's level is single player (main+0x2A44 bit 0
   is set only in the front end), so the rule reads `seatai`; after TK1 every recorded seat is type 1 or 2
   with its AI object. */
void tagpu_impai_takeover(void)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    char list[2 * IMPAI_SEAT_RECS], fresh_list[2 * IMPAI_SEAT_RECS];
    IMPAI_SEAT_REC rec[IMPAI_SEAT_RECS];
    IMPAI_SEAT_PICK pick;
    unsigned i, before = 0, fresh;
    s_ovc.built = 0;
    s_known_tick = 0;
    for (i = 0; i < PLAYER_COUNT; i++)
        if (s_seat[i] & IMPAI_ON) before |= 1u << i;
    if (!ta) {
        seats_set(0);
        return;
    }
    seat_recs(ta, rec);
    impai_seats_pick(rec, (*(const unsigned char*)(ta + OFF_NETFLAGS) & 1) != 0,
                     before | (unsigned)tagpu_settings_seatai(), &pick);
    if (pick.refused >= 0) {
        seats_set(0);
        tagpu_logf("impai: seats none: record %d is remote after a takeover", pick.refused);
        return;
    }
    if (!s_install.avail) {
        seats_set(0);
        tagpu_logf("impai: unavailable: %s", s_install.why);
        return;
    }
    if (pick.marks && !s_content) {
        tagpu_logf("impai: seats none: the level has no content (a takeover)");
        pick.marks = 0;
    }
    fresh = pick.marks & ~before;
    seats_set(pick.marks);
    for (i = 0; i < PLAYER_COUNT; i++)
        if ((before & ~pick.marks) >> i & 1u) s_seatst[i].known_gen = 0;
    for (i = 0; i < PLAYER_COUNT; i++)
        if (fresh >> i & 1u) seat_fresh(ta, i);
    impai_seats_list(pick.marks, list);
    impai_seats_list(fresh, fresh_list);
    tagpu_logf("impai: seats %s (the recording's and impure.cfg's, a takeover) of %u records; fresh %s", list,
               pick.live, fresh_list);
}

/* The recording header's byte for record i (RT_PLAYER byte 15): 1 when ImpAI plays it -- the
   record is seated, of controller 2 and marked, the seam's own test -- else 0. Game thread, at
   the header's capture after the level init, or a fork's after the takeover's restore, so it
   states the seats the game ran. */
int tagpu_impai_available(void) { return s_install.avail; }

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
   and marks the record if the seat choice names it. On a puppet nothing is written: the recorder is the
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

/* The Difficulty handicap's exemption: sixteen type tests of the engine skip a record ImpAI
   plays. RUNS on a puppet: the stubs run there and take stock's branch by the site's own
   compare, every recorded record being type 3. */
SIM_FEATURE(impai_handicap, SIM_PUPPET_RUNS, "impai-handicap");

/* The sixteen sites (engine/sim.md, Difficulty and the AI handicap; the hook map, The AI seat):
   rows 1-17 scale a type-2 record's resources by main+0x37EEE, row 18 is the on-hit handler
   0x406F80's commander purge. Rows 15 and 16, the Deathmatch respawn's grants 0x465437 and
   0x4654A3, stay stock: the respawn 0x4653D9 creates its commander for the controlled player only
   (the owner pushed at 0x4653D2 is the byte main+0x2A42, DISASSEMBLED), a record that is type 2
   only after the AI command, which clears its mark (impai_ai_command), so no ImpAI seat ever
   reaches them. Each holds `cmp byte [rec+0x73],2` (or `cmp [eax+0x73],bl` with
   bl = 2, rows 11-12) and a `jne` to the unscaled branch, short or near; `rec_ecx` when the
   record is in ECX, else EAX. Bytes DISASSEMBLED from the retail image, and compared again at
   attach. */
typedef struct HSITE {
    uint32_t va;
    uint8_t  row;          /* engine/sim.md's row id, 1-18                 */
    uint8_t  n;            /* the compare and its jne: 5, 6 or 10 bytes    */
    uint8_t  clen;         /* the compare alone: 3 or 4                    */
    uint8_t  rec_ecx;
    uint8_t  was[10];
} HSITE;

#define CMP2_EAX 0x80, 0x78, 0x73, 0x02
#define CMP2_ECX 0x80, 0x79, 0x73, 0x02
#define CMPBL    0x38, 0x58, 0x73

static const HSITE HSITES[] = {
    { 0x00401435u,  1,  6, 4, 0, { CMP2_EAX, 0x75, 0x37 } },             /*  1 economy: EnergyUse < 0  */
    { 0x004014B2u,  2, 10, 4, 0, { CMP2_EAX, 0x0F, 0x85, 0x83, 0, 0, 0 } }, /* 2 extractor metal    */
    { 0x004014F3u,  3,  6, 4, 0, { CMP2_EAX, 0x75, 0x46 } },             /*  3 makesmetal              */
    { 0x0040157Fu,  4,  6, 4, 0, { CMP2_EAX, 0x75, 0x26 } },             /*  4 wind                    */
    { 0x004015F3u,  5, 10, 4, 0, { CMP2_EAX, 0x0F, 0x85, 0xB8, 0, 0, 0 } }, /* 5 tide                 */
    { 0x0040167Cu,  6,  6, 4, 0, { CMP2_EAX, 0x75, 0x33 } },             /*  6 mobile EnergyUse < 0    */
    { 0x004016F2u,  7,  6, 4, 0, { CMP2_EAX, 0x75, 0x33 } },             /*  7 energymake              */
    { 0x00401753u,  8,  6, 4, 0, { CMP2_EAX, 0x75, 0x36 } },             /*  8 metalmake               */
    { 0x0040269Fu,  9,  6, 4, 0, { CMP2_EAX, 0x75, 0x38 } },             /*  9 build-cancel refund     */
    { 0x0041BBB1u, 10,  6, 4, 0, { CMP2_EAX, 0x75, 0x37 } },             /* 10 unbuild refund          */
    { 0x004238C4u, 11,  5, 3, 0, { CMPBL, 0x75, 0x2E } },                /* 11 reclaim payout energy   */
    { 0x00423918u, 12,  5, 3, 0, { CMPBL, 0x75, 0x38 } },                /* 12 reclaim payout metal    */
    { 0x00464C06u, 13,  6, 4, 1, { CMP2_ECX, 0x75, 0x32 } },             /* 13 energy transfer         */
    { 0x00464D36u, 14,  6, 4, 1, { CMP2_ECX, 0x75, 0x34 } },             /* 14 metal transfer          */
    { 0x00486CDAu, 17,  6, 4, 1, { CMP2_ECX, 0x75, 0x38 } },             /* 17 reclaimed unit's credit */
    { 0x00406FC3u, 18,  6, 4, 0, { CMP2_EAX, 0x75, 0x2D } },             /* 18 on-hit commander purge  */
};
#define NSITES ((int)(sizeof HSITES / sizeof HSITES[0]))
#define STUB_ROOM 96u          /* one stub's bytes, rounded up: the longest is 86 */
_Static_assert(sizeof HSITES / sizeof HSITES[0] == IMPAI_HSITES, "the fifteen handicap sites and 0x406FC3");
_Static_assert(IMPAI_HSITES * STUB_ROOM <= 0x1000, "every stub fits the one page");

/* The level-end line's counts, ` <row>=<scaled>/<exempt>` for each site in row order, into
   `line` (IMPAI_HSITES * 28 bytes: each item is at most 25). */
static void handicap_counts(char* line)
{
    int i, at = 0;
    unsigned r;
    for (i = 0; i < NSITES; i++) {
        r = HSITES[i].row;
        at += wsprintfA(line + at, " %u=%u/%u", r, s_diag.hits[r - 1][HIT_SCALED],
                        s_diag.hits[r - 1][HIT_EXEMPT]);
    }
}

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

   `hits` is the site's row of s_diag.hits (HSITE.row less one): the level's count of type-2
   records that took each exit, for the level-end line and handicap_check.py.

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
     and the flags are dead at all 32 exits: from every U and S the first instruction that
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

/* THE INSTALL and the attach levers: every site ImpAI writes, and the levers every feature reads.
   RUNS: the install writes the same bytes in every process, a puppet's included, and the levers only
   choose what a live level does. */
SIM_FEATURE(impai_install, SIM_PUPPET_RUNS, "impai-eco impai-fight impai-handicap impai-observe impai-observe-perm stock-impai-observe");

/* THE LEVERS, at attach before the registry closes its lever list (dllmain.c), so a recording lists each:
   the self-checks (tagpu_impai_check.on), the overlay (tagpu_impaiov.on) and the cost run's budgets
   (tagpu_impai_budget.on). Read whatever the install then finds; they act only once it is in. */
void tagpu_impai_levers(void)
{
    QueryPerformanceFrequency(&s_qpf);
    s_install.check = (uint8_t)tagpu_sim_lever("tagpu_impai_check.on");
    impai_overlay_lever();
    impai_mil_levers();
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
   compared -- the seam 0x465031, the AI command's two calls 0x4162F4 and 0x4162FE, the
   sixteen type tests and the site test's bit at 0x47D3D1 -- and only when every one is stock
   are the stubs built and every site written; a write that fails puts back every site already
   written. Only then does WQ_SITE_SEAT get its thunk. Otherwise nothing is written,
   s_install.avail stays 0, and each level says why. */
void tagpu_impai_init(void)
{
    enum { NCALLS = 3, NALL = NCALLS + NSITES + 1, SEAT_SITE = NALL - 1 };
    WRITE w[NALL];
    unsigned char *p, *thunk;
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
    s_radar_hit = (IMPAI_HITROW*)SIM_ALLOC(s_radar_hit, (size_t)TAGPU_PK_DESIGN_SLOTS * sizeof *s_radar_hit);
    if (!s_radar_hit) {
        lstrcpynA(s_install.why, "no memory for the radar records", sizeof s_install.why);
        tagpu_logf("impai: unavailable: %s", s_install.why);
        return;
    }
    write_branch(&w[0], 0xE8, (uint32_t)(uintptr_t)impai_ai_tick);
    write_branch(&w[1], 0xE8, (uint32_t)(uintptr_t)impai_ai_command);
    write_branch(&w[2], 0xE8, (uint32_t)(uintptr_t)impai_ai_command);
    for (i = 0; i < NSITES; i++) {
        p = s_code + (unsigned)i * STUB_ROOM;
        stub_emit(p, &HSITES[i], s_diag.hits[HSITES[i].row - 1]);
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
    /* the addresses handicap_check.py peeks: the seat table, and the counts as 18 pairs of u32
       by row id, scaled then exempt */
    tagpu_logf("impai: seam 0x465031, the AI command 0x4162F4 and 0x4162FE, %d handicap sites and the "
               "seat's site test 0x47D3D1 in; seats at 0x%08X, handicap counts at 0x%08X", NSITES,
               (unsigned)(uintptr_t)s_seat, (unsigned)(uintptr_t)s_diag.hits);
}
