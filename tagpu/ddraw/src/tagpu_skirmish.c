/* tagpu_skirmish.c -- the seat buttons with ImpAI as a computer step: the SKIRMISH screen's and the network
   battle room's (research/notes/impai/player.md, The AI button; engine/ui.md, The SKIRMISH screen's
   handler; engine/network.md, A computer seat in a network game).

   A computer seat is one engine value, the settings entry's +0x00 = 2, for both AIs; which one plays it is
   impure.cfg's `seatai` bit for the seat, which the level init reads (tagpu_impai.c, THE SEAT CHOICE). So the
   button keeps the engine's own cycle and adds one step inside its Computer value, and the label says which
   AI. Two call-site redirects, byte-matched at attach, both or neither, and neither while ImpAI is not
   available: then the screen is stock. The battle room's button is THE BATTLE ROOM, below. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_skirmish.h"
#include "tagpu_detour.h"
#include "tagpu_engine.h"
#include "tagpu_impai.h"
#include "impai_seats.h"
#include "tagpu_log.h"
#include "tagpu_settings.h"

#define VA_CYCLE_CALL 0x0047B178u   /* the handler 0x47AE60's `call 0x4797E0`, the cycle's one call     */
#define VA_CYCLE      0x004797E0u   /* the cycle: stdcall(seat), ret 4                                   */
#define VA_LABEL_CALL 0x0047A1FCu   /* the init 0x47A0E0's `call 0x4A0BF0` in its Computer branch        */
#define VA_SET_TEXT   0x004A0BF0u   /* the named text setter: stdcall(gui, name, text, flag), ret 0x10   */
#define OFF_GUI       0x519         /* main + 0x519: the GUI object the cycle and the init pass           */
#define OFF_SETTINGS  0x29A0        /* the settings block; a seat's entry at +0x18 x seat                 */
#define ENTRY_STRIDE  0x18
#define OFF_SEATS     0x38D81       /* the screen's seat count                                            */
#define CTRL_COMPUTER 2
#define SEATS_MAX     10

/* THE FIRST COMPUTER STEP: 0, OTA AI first and ImpAI second; 1, ImpAI first. */
#define IMPAI_FIRST_COMPUTER 0

typedef void (__stdcall* CycleFn)(unsigned seat);
typedef int (__stdcall* SetTextFn)(void* gui, const char* name, const char* text, int flag);

static const char LABEL_OTA[] = "OTA AI", LABEL_IMPAI[] = "ImpAI";

/* the seat's entry +0x00, or NULL past the seat bound or with no settings block */
static unsigned* seat_entry(unsigned seat)
{
    char* ta = *(char**)TA_MAIN_PP;
    char* settings;
    if (!ta || seat >= SEATS_MAX || seat >= *(const unsigned*)(ta + OFF_SEATS)) return NULL;
    settings = *(char**)(ta + OFF_SETTINGS);
    return settings ? (unsigned*)(settings + seat * ENTRY_STRIDE) : NULL;
}

/* the seat's button label, written as the cycle writes it: the name formatted `Player%d` (0x5080E8) */
static void label(unsigned seat, int impai)
{
    char* ta = *(char**)TA_MAIN_PP;
    char name[16];
    snprintf(name, sizeof name, "Player%u", seat);
    ((SetTextFn)VA_SET_TEXT)(ta + OFF_GUI, name, impai ? LABEL_IMPAI : LABEL_OTA, 0);
}

static void choose(unsigned seat, int impai)
{
    const unsigned mask = (unsigned)tagpu_settings_seatai();
    tagpu_settings_set_seatai((int)(impai ? mask | 1u << seat : mask & ~(1u << seat)));
}

/* THE SEAT BUTTON. The handler 0x47AE60 calls the cycle 0x4797E0(seat), stdcall ret 4, at 0x47B178 only,
   after the click's sound. The cycle is Open -> Computer -> Player (when no seat is the player) -> Open
   (engine/ui.md); here its Computer value takes two steps, the first IMPAI_FIRST_COMPUTER's and then the
   other's: a click on a computer seat at its first step moves it to the second without the engine's cycle,
   and every other click runs the cycle, a seat entering Computer at the first step and one leaving it
   losing its bit. Game thread, in the shell. BOUND: the seat below 10 and below the screen's seat count
   main+0x38D81, else the click is stock's. */
static void __stdcall skirmish_cycle(unsigned seat)
{
    unsigned* entry = seat_entry(seat);
    unsigned was;
    int impai;
    if (!entry) {
        ((CycleFn)VA_CYCLE)(seat);
        return;
    }
    was = *entry;
    impai = (tagpu_settings_seatai() >> seat & 1) != 0;
    if (was == CTRL_COMPUTER && impai == IMPAI_FIRST_COMPUTER) {
        choose(seat, !IMPAI_FIRST_COMPUTER);
        label(seat, !IMPAI_FIRST_COMPUTER);
        return;
    }
    ((CycleFn)VA_CYCLE)(seat);
    if (*entry == CTRL_COMPUTER) {
        choose(seat, IMPAI_FIRST_COMPUTER);
        label(seat, IMPAI_FIRST_COMPUTER);
    } else if (was == CTRL_COMPUTER) {
        choose(seat, 0);
    }
}

/* THE INIT LABEL. The SKIRMISH init 0x47A0E0 labels a Computer seat with 0x4A0BF0 at 0x47A1FC, that branch
   only (0x47A1DD..0x47A201). The seat is the name's last character, as the handler itself reads it
   (0x47AE80..0x47AEC0). The text is `ImpAI` or `OTA AI` by the store's bit; a name whose seat cannot be read
   keeps stock's text. Game thread, in the shell. */
static int __stdcall skirmish_label_computer(void* gui, const char* name, const char* text, int flag)
{
    const size_t n = name ? strnlen(name, 32) : 0;
    if (n && n < 32 && name[n - 1] >= '0' && name[n - 1] <= '9')
        text = tagpu_settings_seatai() >> (name[n - 1] - '0') & 1 ? LABEL_IMPAI : LABEL_OTA;
    return ((SetTextFn)VA_SET_TEXT)(gui, name, text, flag);
}

/* ---- THE BATTLE ROOM ----------------------------------------------------------------------------------

   A network game's computer seat is the peer's own record of type 2, named by the add 0x451220 `AI:%s` with
   the local human's long name and cut to 16 characters (0x4512F0..0x45131E), and DirectPlay carries the name
   to every other peer. ImpAI's choice is that name: `ImpAI:%s` for a seat ImpAI plays (impai_seats.h, WHY THE
   NAME). The peer's own AI row cycles, on a click of its PLAYER<n> gadget, the first computer step, then the
   other, then Unused as stock removes it -- Unused -> ImpAI -> OTA AI -> Unused with `netai=impai`, Unused ->
   OTA AI -> ImpAI -> Unused with `netai=ota`, the host's with Blocked before them. Stock's one AI a peer
   (0x457B90) is kept: the step changes which AI plays a row, not how many rows there are.
   Two writes, both or neither, and neither while ImpAI is not available, so the room is then stock:
   - THE FIRST STEP'S NAME: the immediate of the add's `push 0x5064B4` (68 B4 64 50 00 at 0x451310, the
     format's only reference) points at ImpAI's format when the first step is ImpAI's, and stays stock's
     otherwise. The add's cut at 0x45131E stores a NUL at the buffer's sixteenth byte whatever the format,
     and the unbounded sprintf writes at most 6 + 29 + 1 bytes of its 0x100 (engine/network.md);
   - THE STEP: a jmp at 0x447CB3 (8B 53 04 6A 01, `mov edx,[ebx+4]; push 1`), reached only by falling through
     0x447CB1 after the handler's own tests -- the clicked row is active, of type 2 and older than 30 ticks of
     0x4B6340 -- and the target of no branch. room_step renames a row at its first step to the other and the
     stub goes on at the click's tail 0x447E27 (the room's dirty bit, the session descriptor 0x451180 and the
     PlayerInfo broadcast 0x450F90, as stock after a removal); any other row runs the displaced two
     instructions and stock's removal at 0x447CB8. Registers at the site: ebx the record, ebp the row (< 10,
     the loop's), esi the host flag and edi the local row, all kept by the stdcall callee; eax, ecx, edx and
     the flags are dead (eax is reloaded at 0x447E27, ecx set at 0x447CC0, edx by the displaced load).
   THE LABEL follows the name with no write of ours: the tail's dirty bit makes the room's update
   0x44A7CC call 0x448C70, which sets every PLAYER<n> gadget's text from its record's +0x2B.
   THE OTHER PEERS: the rename 0x450090(row, short, long) (stdcall, ret 0xC) copies both names into the
   record, each bounded to 30 bytes, and calls DirectPlay's SetPlayerName on the row's DPID, guaranteed,
   whose system message 0x103 each other peer copies into its own record of the seat (0x45469E..0x454737);
   the engine has the function and no caller of it. [INFERRED: SetPlayerName raises 0x103 on the other
   peers, DirectPlay's documented behaviour; the room check measures it.] Game thread, in the shell. */
#define VA_ROOM_STEP      0x00447CB3u  /* the removal branch's `mov edx,[ebx+4]; push 1`                    */
#define VA_ROOM_REMOVE    0x00447CB8u  /* its `push edx; call 0x453010`: stock's removal                     */
#define VA_ROOM_TAIL      0x00447E27u  /* the click's tail: dirty bit, 0x451180, 0x450F90                    */
#define VA_AI_FORMAT_PUSH 0x00451310u  /* the add's `push 0x5064B4`, "AI:%s"                                 */
#define VA_RENAME         0x00450090u  /* stdcall(row, short, long), ret 0xC                                 */
#define OFF_LOCAL_ROW     0x2A42       /* u8: the local peer's own row, the add's controlling row (0x4512F0) */
#define PL_ACTIVE         0x00
#define PL_CONTROL        0x73
#define PL_NAME           0x2B
#define CTRL_AI           2

typedef void (__stdcall* RenameFn)(unsigned row, const char* short_name, const char* long_name);

static const unsigned char ROOM_STEP_WAS[5] = { 0x8B, 0x53, 0x04, 0x6A, 0x01 };
static const unsigned char AI_FORMAT_WAS[5] = { 0x68, 0xB4, 0x64, 0x50, 0x00 };
static const char IMPAI_FORMAT[] = IMPAI_NAME_PREFIX "%s";
static int s_room_first;               /* the first computer step: 1 ImpAI's, 0 OTA AI's; set at attach  */

/* THE STEP, from the stub at 0x447CB3: 1 when the row was at its first computer step and is now renamed to
   the other, 0 to let stock remove it. BOUND: `rec` is record `row` of main's ten, by the row's own range and
   the address it gives, and still active and of type 2, else stock's; the local row is below 10, else the
   name has no human to take and the click is stock's. */
static int __stdcall room_step(char* rec, unsigned row)
{
    char* ta = *(char**)TA_MAIN_PP;
    char name[IMPAI_NAME_CUT + 1];
    unsigned local;
    if (!ta || row >= PLAYER_COUNT || rec != ta + OFF_PLAYERS + row * PLAYER_STRIDE) return 0;
    if (!*(const unsigned*)(rec + PL_ACTIVE) || (unsigned char)rec[PL_CONTROL] != CTRL_AI) return 0;
    if (impai_seats_named(rec + PL_NAME, IMPAI_NAME_FIELD) != s_room_first) return 0;
    local = *(const unsigned char*)(ta + OFF_LOCAL_ROW);
    if (local >= PLAYER_COUNT) return 0;
    impai_seats_ai_name(name, !s_room_first, ta + OFF_PLAYERS + local * PLAYER_STRIDE + PL_NAME,
                        IMPAI_NAME_FIELD);
    ((RenameFn)VA_RENAME)(row, name, name);
    return 1;
}

static unsigned char* put_rel(unsigned char* p, unsigned char op, unsigned target)
{
    *p++ = op;
    tagpu_detour_rel(p, target);
    return p + 4;
}

/* push ebp; push ebx; call room_step; test eax,eax; jnz 0x447E27; mov edx,[ebx+4]; push 1; jmp 0x447CB8 */
static unsigned char* room_stub(void)
{
    unsigned char* st = tagpu_detour_stub();
    unsigned char* p = st;
    if (!st) return NULL;
    *p++ = 0x55;
    *p++ = 0x53;
    p = put_rel(p, 0xE8, (unsigned)(uintptr_t)room_step);
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x0F;
    p = put_rel(p, 0x85, VA_ROOM_TAIL);
    memcpy(p, ROOM_STEP_WAS, sizeof ROOM_STEP_WAS);
    p += sizeof ROOM_STEP_WAS;
    put_rel(p, 0xE9, VA_ROOM_REMOVE);
    return st;
}

static void room_init(void)
{
    unsigned char push[5] = { 0x68 };
    unsigned char* st;
    const unsigned fmt = (unsigned)(uintptr_t)IMPAI_FORMAT;
    s_room_first = tagpu_settings_netai() != 0;
    if (!tagpu_detour_bytes_ok(VA_ROOM_STEP, ROOM_STEP_WAS, 5) ||
        !tagpu_detour_bytes_ok(VA_AI_FORMAT_PUSH, AI_FORMAT_WAS, 5)) {
        tagpu_log("skirmish: the battle room's AI button stays stock: a site is not stock's");
        return;
    }
    if (!(st = room_stub())) {
        tagpu_log("skirmish: the battle room's AI button stays stock: no memory for its stub");
        return;
    }
    memcpy(push + 1, &fmt, 4);
    if (s_room_first && !tagpu_detour_write(VA_AI_FORMAT_PUSH, push, 5)) {
        tagpu_detour_stub_free(st);
        tagpu_log("skirmish: the battle room's AI button stays stock: 0x451310 could not be written");
        return;
    }
    if (!tagpu_detour_land(VA_ROOM_STEP, st, 5)) {
        if (s_room_first) tagpu_detour_write(VA_AI_FORMAT_PUSH, AI_FORMAT_WAS, 5);
        tagpu_detour_stub_free(st);
        tagpu_log("skirmish: the battle room's AI button stays stock: 0x447CB3 could not be written");
        return;
    }
    tagpu_logf("skirmish: the battle room's AI row steps %s, then %s, then Unused (0x447CB3%s)",
               s_room_first ? "ImpAI" : "OTA AI", s_room_first ? "OTA AI" : "ImpAI",
               s_room_first ? ", 0x451310" : "");
}

void tagpu_skirmish_init(void)
{
    if (!tagpu_impai_available()) return;
    room_init();
    if (!tagpu_detour_redirect_ok(VA_CYCLE_CALL, VA_CYCLE) || !tagpu_detour_redirect_ok(VA_LABEL_CALL, VA_SET_TEXT)) {
        tagpu_log("skirmish: the seat button stays stock: a call site is not stock's");
        return;
    }
    tagpu_detour_redirect(VA_CYCLE_CALL, VA_CYCLE, (void*)skirmish_cycle);
    tagpu_detour_redirect(VA_LABEL_CALL, VA_SET_TEXT, (void*)skirmish_label_computer);
    tagpu_log("skirmish: the seat button steps a computer seat through OTA AI and ImpAI (0x47B178, 0x47A1FC)");
}
