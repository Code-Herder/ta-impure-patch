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
   compared -- the seam 0x465031, the AI command's two calls 0x4162F4 and 0x4162FE and the
   sixteen type tests -- and only when every one is stock are the stubs built and every site
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
        stub_emit(p, &HSITES[i], s_diag.hits[HSITES[i].row - 1]);
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
    /* the addresses handicap_check.py peeks: the seat table, and the counts as 18 pairs of u32
       by row id, scaled then exempt */
    tagpu_logf("impai: seam 0x465031, the AI command 0x4162F4 and 0x4162FE and %d handicap sites "
               "in; seats at 0x%08X, handicap counts at 0x%08X", NSITES,
               (unsigned)(uintptr_t)s_seat, (unsigned)(uintptr_t)s_diag.hits);
}
