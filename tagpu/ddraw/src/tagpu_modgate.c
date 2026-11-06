/* tagpu_modgate -- see tagpu_modgate.h */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "tagpu_modgate.h"
#include "tagpu_modprint.h"

/* DISASSEMBLED 2026-11-06. 0x456760 answers whether every seat is ready; its only callers are the battle room's two settings of START's grayed
   state, 0x44A167 and 0x44AC46, both for the host. The click itself is 0x49FD60(message, "START") at 0x44871D, stdcall, whose action asks about game
   CDs and teams (0x448781, 0x448885) but not readiness, so it answers too: no path starts the game on a stale gadget. */
#define VA_READY        0x00456760u
#define VA_READY_CALL_1 0x0044A167u
#define VA_READY_CALL_2 0x0044AC46u
#define VA_NAMED        0x0049FD60u
#define VA_START_CLICK  0x0044871Du
#define VA_IS_ON_TOP    0x004AB060u     /* stdcall (GUIInfo*, const char* name) */
#define VA_CHAT_LINE    0x00463CA0u     /* stdcall (text, flags, u16, seat): a local line, 0x40 bytes kept; seat 10 plays no sound */
#define VA_MAIN         0x00511DE8u
#define MAIN_GUI        0x519
#define MAIN_ROWS       0x1B63
#define ROW_STRIDE      0x14B
#define ROWS            10
#define ROW_ACTIVE      0x00            /* the engine's own test of a seat: 0x456778, 0x4567C0 */
#define ROW_NATIVE      0x04
#define ROW_NAME        0x2B
#define ROW_NAME_MAX    0x1E
#define ROW_TYPE        0x73
#define TYPE_REMOTE     3
#define CHAT_FLAGS      8
#define CHAT_NO_SEAT    10
#define CHAT_MAX        0x40

int tagpu_rec_player_print(unsigned native, MODPRINT* out);     /* tagpu_recorder.c: the record is its */

typedef struct { unsigned native; uint8_t exe[MODPRINT_HASH], content[MODPRINT_HASH]; } SAID;

static int  s_armed;
static SAID s_said[ROWS];

static char* main_block(void)
{
    return *(char**)(uintptr_t)VA_MAIN;
}

/* Each answer is read off DirectPlay's own record of the seat, so a seat that left or changed hands answers as it is now. */
static int seat_differs(const char* row, const MODPRINT* mine, MODPRINT* theirs)
{
    unsigned native;
    memcpy(&native, row + ROW_NATIVE, sizeof native);
    if (!*(const unsigned*)(row + ROW_ACTIVE) || !native || (unsigned char)row[ROW_TYPE] != TYPE_REMOTE || !tagpu_rec_player_print(native, theirs)) return 0;
    return memcmp(theirs->exe, mine->exe, MODPRINT_HASH) || memcmp(theirs->content, mine->content, MODPRINT_HASH);
}

static int everyone_agrees(void)
{
    const MODPRINT* mine = tagpu_modprint_wait();
    const char* ta = main_block();
    int i;
    if (!mine || !ta) return 1;
    for (i = 0; i < ROWS; i++) {
        MODPRINT theirs;
        if (seat_differs(ta + MAIN_ROWS + i * ROW_STRIDE, mine, &theirs)) return 0;
    }
    return 1;
}

static int ready_and_agreed(void)
{
    return ((int (*)(void))(uintptr_t)VA_READY)() && everyone_agrees();
}

static int __stdcall start_clicked(void* message, const char* name)
{
    return ((int (__stdcall*)(void*, const char*))(uintptr_t)VA_NAMED)(message, name) && everyone_agrees();
}

static void chat_line(const char* text)
{
    ((void (__stdcall*)(const char*, int, int, int))(uintptr_t)VA_CHAT_LINE)(text, CHAT_FLAGS, 0, CHAT_NO_SEAT);
}

static void say_seat(const char* row, const MODPRINT* mine, const MODPRINT* theirs)
{
    char line[CHAT_MAX], name[ROW_NAME_MAX + 1];
    memcpy(name, row + ROW_NAME, ROW_NAME_MAX);
    name[ROW_NAME_MAX] = 0;
    snprintf(line, sizeof line, "Cannot start: %s has %s.", name, memcmp(theirs->exe, mine->exe, MODPRINT_HASH) ? "another exe" : "other game files");
    chat_line(line);
    if (strcmp(theirs->label, mine->label)) {
        snprintf(line, sizeof line, "There: %s", theirs->label);
        chat_line(line);
        snprintf(line, sizeof line, "Here: %s", mine->label);
        chat_line(line);
    }
    tagpu_logf("modgate: %s differs: there %s, here %s", name, theirs->label, mine->label);
}

void tagpu_modgate_tick(void)
{
    const MODPRINT* mine;
    char* ta;
    int i;
    if (!s_armed || !tagpu_modprint_started() || !(ta = main_block())) return;
    if (!((int (__stdcall*)(void*, const char*))(uintptr_t)VA_IS_ON_TOP)(ta + MAIN_GUI, "LOUNGE2.GUI")) return;
    if (!(mine = tagpu_modprint_wait())) return;
    for (i = 0; i < ROWS; i++) {
        const char* row = ta + MAIN_ROWS + i * ROW_STRIDE;
        MODPRINT theirs;
        SAID* said = &s_said[i];
        if (!seat_differs(row, mine, &theirs)) { said->native = 0; continue; }
        /* once per seat and print: the ring holds thirty lines and this runs every flip */
        if (said->native == *(const unsigned*)(row + ROW_NATIVE) && !memcmp(said->exe, theirs.exe, MODPRINT_HASH) &&
            !memcmp(said->content, theirs.content, MODPRINT_HASH)) continue;
        said->native = *(const unsigned*)(row + ROW_NATIVE);
        memcpy(said->exe, theirs.exe, MODPRINT_HASH);
        memcpy(said->content, theirs.content, MODPRINT_HASH);
        say_seat(row, mine, &theirs);
    }
}

void tagpu_modgate_install(void)
{
    if (!tagpu_detour_redirect_ok(VA_READY_CALL_1, VA_READY) || !tagpu_detour_redirect_ok(VA_READY_CALL_2, VA_READY) ||
        !tagpu_detour_redirect_ok(VA_START_CLICK, VA_NAMED)) {
        tagpu_log("modgate: the battle room's calls are not stock's: players are not compared");
        return;
    }
    s_armed = tagpu_detour_redirect(VA_READY_CALL_1, VA_READY, ready_and_agreed) &&
              tagpu_detour_redirect(VA_READY_CALL_2, VA_READY, ready_and_agreed) &&
              tagpu_detour_redirect(VA_START_CLICK, VA_NAMED, start_clicked);
    if (!s_armed) tagpu_log("modgate: a battle room call could not be written: players are not compared");
}
