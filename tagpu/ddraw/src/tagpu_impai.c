/* tagpu_impai.c -- ImpAI's seam in the engine and its seat table (tagpu_impai.h). */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "tagpu_impai.h"
#include "tagpu_simreg.h"

/* ImpAI writes nothing on a puppet: the wrapper refuses a puppet seat and the level init writes
   no seat on a puppet level, so the recording is the only writer of the seat table there. */
SIM_FEATURE(impai_observe, SIM_PUPPET_OWNER, "impai-observe impai-observe-perm");

#include "dd.h"                 /* g_ddraw.gui_thread_id: the game thread */
#include "tagpu_engine.h"
#include "tagpu_puppet.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "tagpu_replay.h"
#include "tagpu_settings.h"

#define VA_AI_CALL   0x00465031u   /* the players phase's `call 0x408C40`, E8 0A 3C FA FF */
#define VA_AI_TICK   0x00408C40u   /* the stock AI tick, thiscall, no stack argument      */
#define PL_ACTIVE    0x00          /* u32: nonzero for a seated record                   */
#define PL_CONTROL   0x73          /* u8: 1 human, 2 AI, 3 remote or puppet              */
#define CTRL_AI      2
#define CTRL_REMOTE  3

#define IMPAI_ON     0x01u

/* Which records ImpAI plays, by record index. Every level starts from the table a fresh
   process holds: the level init clears it on every level, live or puppet, on the loader thread
   before the level's first step, so no mark of an earlier level survives into this one. Then
   the level init marks a live level's seats; on a puppet level the recording is the only
   writer, its first restore running on the game thread at the level's first step, after the
   clear (impai_seat_of, THE THREAD, gives the ordering). A recording that carries no row of
   this table leaves it clear. */
static uint8_t s_seat[PLAYER_COUNT];
SIM_ROWS(s_seat, SIM_STATE, SIM_ID_PLAYER, NULL);

/* What went in at attach. `avail`: every site ImpAI needs is in, so a level may mark seats;
   `why` says what kept it out. */
static struct {
    uint8_t in;            /* 1 once 0x465031 reaches impai_ai_tick            */
    uint8_t avail;
    char    why[80];
} s_install;
SIM_VAR(s_install, SIM_INSTALL);

/* the wrapper's turns per seat this level, for the level-end line */
static struct {
    unsigned calls[PLAYER_COUNT];
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

/* One ImpAI seat's turn, at its record's place in the players phase: counted. */
static void impai_observe(int seat)
{
    s_diag.calls[seat]++;
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

/* On the game thread, in the teardown 0x491B60's entry; with tagpu_reclaim.off that wrap is not
   installed and no line is written. */
void tagpu_impai_level_end(void)
{
    const char* ta = *(const char* const*)(uintptr_t)TA_MAIN_PP;
    unsigned i, now;
    if (!s_install.in || !ta) return;
    now = *(const unsigned*)(ta + OFF_GAMETIME);
    for (i = 0; i < PLAYER_COUNT; i++)
        if ((s_seat[i] & IMPAI_ON) || s_diag.calls[i])
            tagpu_logf("impai: calls seat=%u n=%u gametime=%u", i, s_diag.calls[i], now);
}

/* THE LEVEL INIT IS OURS FIRST. Nothing goes in unless tagpu_replay_init has retargeted the
   level init's PLAYERS call 0x4919C8 to tagpu_replay_level_players, the only caller of
   tagpu_impai_level_init: so the seam is never in on a level whose seat table nobody cleared
   and wrote. */
void tagpu_impai_init(void)
{
    const unsigned char* at = (const unsigned char*)(uintptr_t)VA_AI_CALL;
    if (!tagpu_replay_level_init_in()) {
        lstrcpynA(s_install.why, "the level init 0x4919C8 is not retargeted (the log's `replay:` "
                  "lines say why)", sizeof s_install.why);
    } else if (!tagpu_detour_redirect_ok(VA_AI_CALL, VA_AI_TICK)) {
        wsprintfA(s_install.why, "0x465031 not stock (%02X %02X %02X %02X %02X)", at[0], at[1],
                  at[2], at[3], at[4]);
    } else if (!tagpu_detour_redirect(VA_AI_CALL, VA_AI_TICK, (void*)impai_ai_tick)) {
        lstrcpynA(s_install.why, "0x465031 could not be written", sizeof s_install.why);
    } else {
        s_install.in = s_install.avail = 1;
        tagpu_logf("impai: seam 0x465031 in");
        return;
    }
    tagpu_logf("impai: unavailable: %s", s_install.why);
}

/* The level init's seat choice, the AI command and the header's accessor: what decides which
   seats ImpAI plays. On a puppet the level init marks no seat, so the recording is the only
   writer of the table there. */
SIM_FEATURE(impai_seats, SIM_PUPPET_OWNER, "impai-observe impai-observe-perm");

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
