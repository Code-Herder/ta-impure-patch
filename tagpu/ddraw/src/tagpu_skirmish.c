/* tagpu_skirmish.c -- the SKIRMISH screen's seat button with ImpAI as a computer step
   (research/notes/impai/player.md, The AI button; engine/ui.md, The SKIRMISH screen's handler).

   A computer seat is one engine value, the settings entry's +0x00 = 2, for both AIs; which one plays it is
   impure.cfg's `seatai` bit for the seat, which the level init reads (tagpu_impai.c, THE SEAT CHOICE). So the
   button keeps the engine's own cycle and adds one step inside its Computer value, and the label says which
   AI. Two call-site redirects, byte-matched at attach, both or neither, and neither while ImpAI is not
   available: then the screen is stock. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_skirmish.h"
#include "tagpu_detour.h"
#include "tagpu_engine.h"
#include "tagpu_impai.h"
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

void tagpu_skirmish_init(void)
{
    if (!tagpu_impai_available()) return;
    if (!tagpu_detour_redirect_ok(VA_CYCLE_CALL, VA_CYCLE) || !tagpu_detour_redirect_ok(VA_LABEL_CALL, VA_SET_TEXT)) {
        tagpu_log("skirmish: the seat button stays stock: a call site is not stock's");
        return;
    }
    tagpu_detour_redirect(VA_CYCLE_CALL, VA_CYCLE, (void*)skirmish_cycle);
    tagpu_detour_redirect(VA_LABEL_CALL, VA_SET_TEXT, (void*)skirmish_label_computer);
    tagpu_log("skirmish: the seat button steps a computer seat through OTA AI and ImpAI (0x47B178, 0x47A1FC)");
}
