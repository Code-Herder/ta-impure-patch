/* tagpu_route.c -- the in-play mouse handler's shared hooks (tagpu_route.h).

   The handler 0x499200 copies the record main+0x2C76 to its stack (0x499210..0x49921A), lets
   0x498DA0 make the world point main+0x2CAA and the region bits main+0x2CC6 (0x499221), runs the
   build cursor 0x4197D0 in mode 14 or else the hover query 0x48CD80 and the cursor chooser
   0x48D220 (0x499297), whose answer it stores in main+0x2CBE with its sprite (0x4992A2..0x4992C8),
   and then routes the click: mode 1 skips the route (0x4993BD), every other mode reaches
   0x4993BF. Three hooks are shared here, because more than one client needs each:

   - the stub at 0x4993BF, which asks the armed mode's client what to do with the record;
   - a before-observer on the handler's entry, where each client ends its own gesture when a
     record is handled in another mode;
   - the cursor chooser's call 0x499297, which tagpu_sweep.c filters. Its one other call,
     0x491D36, lies in 0x491CC0, which nothing calls: no call or jump in .text and no four-byte
     value in the file inside it (DISASSEMBLED), so it is left alone.

   Cursor modes are exclusive, so each record has at most one client, and in every other mode the
   record is stock's. Everything here is game-thread state: the simulation never reads it. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "tagpu_route.h"
#include "tagpu_place.h"
#include "tagpu_sweep.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_packet.h"
#include "tagpu_detour.h"
#include "tagpu_puppet.h"
#include "tagpu_log.h"

#define D_STRIDE 0x249u
#define U_FLAGS  0x110
#define M_MODE   0x2CC3

#define VA_HANDLER     0x00499200u
#define VA_ROUTE       0x004993BFu
#define VA_ROUTE_CLICK 0x004993CBu
#define VA_ROUTE_FRAME 0x004995B8u
#define VA_CHOOSER     0x0048D220u
#define VA_CURSOR      0x00499296u   /* push eax; call 0x48D220 (0x499297) */

static unsigned s_ok;

int tagpu_route_ok(unsigned what)
{
    return (s_ok & what) == what;
}

int tagpu_route_world(TagpuWorld* w)
{
    char *end, *player;
    uintptr_t first, last, distance;
    memset(w, 0, sizeof *w);
    w->ta = *(char**)TA_MAIN_PP;
    if (!w->ta) return 0;
    w->player = *(unsigned char*)(w->ta + OFF_WATCHED);
    if (w->player >= 10 || tagpu_puppet_seat(w->player)) return 0;
    w->ndefs = *(unsigned*)(w->ta + OFF_UDEFCOUNT);
    if (!w->ndefs || w->ndefs > TAGPU_LIM_TYPES) return 0;
    w->defs = *(char**)(w->ta + OFF_UNITDEFS);
    w->units = *(char**)(w->ta + OFF_UNIT_BEGIN);
    end = *(char**)(w->ta + OFF_UNIT_END);
    if (!w->defs || !w->units || (uintptr_t)end < (uintptr_t)w->units) return 0;
    distance = (uintptr_t)end - (uintptr_t)w->units;
    if (distance % UNIT_STRIDE || distance / UNIT_STRIDE >= TAGPU_PK_DESIGN_SLOTS) return 0;
    w->nslots = *(unsigned short*)(w->ta + OFF_UNITSLOTS);
    if (w->nslots != (unsigned)(distance / UNIT_STRIDE) + 1) return 0;
    /* the player's block, validated against the array's own first and last slots */
    player = w->ta + 0x1B63 + w->player * 0x14B;
    first = (uintptr_t)*(char**)(player + 0x67); last = (uintptr_t)*(char**)(player + 0x6B);
    if (first < (uintptr_t)w->units || last < first || last > (uintptr_t)end ||
        (first - (uintptr_t)w->units) % UNIT_STRIDE ||
        (last - (uintptr_t)w->units) % UNIT_STRIDE) return 0;
    w->first = (unsigned)((first - (uintptr_t)w->units) / UNIT_STRIDE);
    w->last = (unsigned)((last - (uintptr_t)w->units) / UNIT_STRIDE);
    return 1;
}

char* tagpu_route_unit(const TagpuWorld* w, unsigned slot)
{
    char* u;
    unsigned flags;
    if (!slot || slot >= w->nslots) return NULL;
    u = w->units + slot * UNIT_STRIDE;
    flags = *(unsigned*)(u + U_FLAGS);
    return (flags & 0x10000000u) && !(flags & 0x4000u) ? u : NULL;
}

unsigned tagpu_route_def_id(const TagpuWorld* w, const char* u)
{
    uintptr_t d = (uintptr_t)*(char* const*)(u + U_TYPE), base = (uintptr_t)w->defs, offset;
    if (d < base) return 0;
    offset = d - base;
    if (offset % D_STRIDE || offset / D_STRIDE >= w->ndefs) return 0;
    return (unsigned)(offset / D_STRIDE);
}

/* The mouse object's current move record, six dwords at +0x196: the one an empty ring's peek and
   pop hand IdleTick. The object is the static 0x51F320 WinMain gives 0x4B5990, so it lives as long
   as the process; the window procedure writes the record on this thread (0x4C2360). */
void tagpu_route_stale_move(int out[6])
{
    const char* mouse = *(const char* const*)0x0051FBD0u;
    if (mouse) memcpy(out, mouse + 0x196, 6 * sizeof(int));
    else memset(out, 0, 6 * sizeof(int));
}

/* THE SAME MOVE, WHEREVER IT NOW SAYS THE POINTER IS. Only the move arm's copy 0x4C2360 writes
   dwords 2..5 of +0x196 (wParam, the clock, the message, the double-click flag). Its x and y
   are rewritten without them, on this thread: by the three cursor polls from GetCursorPos
   (0x4C24F2, 0x4C2624, 0x4C28C0), and by vpwide's repair of main+0x2C76 under zoom
   (tagpu_vpwide.c). Zoomed, those disagree -- the move message carries the unzoomed position,
   the poll the screen one -- so a copy taken at the press and IdleTick's copy a frame later
   differ in x and y alone (MEASURED 2026-10-25 on the reference setup at about zoom 0.35:
   1573,1382 at the press, 1841,1185 after, the same clock 0x85E53). A new move message
   rewrites all six, the input reset 0x4B5A88 zeroes wParam; either differs here. */
int tagpu_route_same_move(const int a[6], const int b[6])
{
    return !memcmp(a + 2, b + 2, 4 * sizeof(int));
}

/* The click routing's verdict for the record `rec` (the handler's stack copy) with message
   `msg`: 0 stock, 1 consumed, 2 run stock's click on this record. Modes are exclusive, so each
   record has at most one client, and every other mode is stock's. */
static int __cdecl route_button(const int* rec, unsigned msg)
{
    const unsigned char* ta = *(const unsigned char* const*)TA_MAIN_PP;
    if (!ta) return 0;
    switch (ta[M_MODE]) {
    case 14: return tagpu_place_button(rec, msg);
    case 12: return tagpu_sweep_button(rec, msg);
    default: return 0;
    }
}

/* THE IN-PLAY HANDLER'S ENTRY, 0x499200 (IdleTick's per-mode call at 0x499A1C, through
   main+0x391F5): each client ends its own gesture when the record is handled in another mode
   (tagpu_place_entry, tagpu_sweep_entry, which say why that is enough for each). A
   before-observer: the handler runs unchanged. */
static int __cdecl handler_entry(void* entry_esp)
{
    (void)entry_esp;
    tagpu_place_entry();
    tagpu_sweep_entry();
    return 0;
}

/* THE ROUTE AT 0x4993BF. The click routing's mode != 1 branch, DISASSEMBLED:
       0x4993BF  81 FA 01 02 00 00   cmp edx,0x201        ; edx: the record's message
       0x4993C5  0F 85 ED 01 00 00   jne 0x4995B8         ; the frame: anything but a press
       0x4993CB  8D 4C 24 0C         lea ecx,[esp+0xc]    ; the handler's record copy
       0x4993CF  51                  push ecx
       0x4993D0  E9 DE 01 00 00      jmp 0x4995B3         ; call 0x498F70, the click
   All 22 bytes are matched; the first 12 become `jmp stub` and seven NOPs. No branch in the
   image lands on 0x4993C0..0x4993CA and no absolute reference names them (a scan of the
   disassembly and of the file), so nothing enters the NOPs. The stub asks route_button and
   takes one of three ways: 1, consumed, to the frame 0x4995B8; 2, stock's click on this record,
   to 0x4993CB; 0, the two original instructions. pushad/popad keep every register; EFLAGS
   differ on ways 1 and 2 only, and neither target reads a flag before writing one (0x4995B8 is
   `call 0x496790`; 0x4993CB is `lea`, `push`, `jmp` to a call). Written at attach, before the
   game thread can run the handler. */
static int install_route(void)
{
    static const unsigned char want[22] = {
        0x81,0xFA,0x01,0x02,0x00,0x00, 0x0F,0x85,0xED,0x01,0x00,0x00,
        0x8D,0x4C,0x24,0x0C, 0x51, 0xE9,0xDE,0x01,0x00,0x00 };
    unsigned char patch[12];
    unsigned char *s, *p;
    int32_t rel;
    if (memcmp((const void*)(uintptr_t)VA_ROUTE, want, sizeof want)) return 0;
    if (!(s = p = tagpu_detour_stub())) return 0;
    *p++ = 0x60;                                            /* pushad                    */
    *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x2C;     /* lea eax,[esp+0x2C]: the record, [esp+0xc] before pushad */
    *p++ = 0x52;                                            /* push edx: the message     */
    *p++ = 0x50;                                            /* push eax: the record      */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(uintptr_t)route_button); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                  /* add esp,8                 */
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x01;                  /* cmp eax,1                 */
    *p++ = 0x61;                                            /* popad: the flags stand    */
    *p++ = 0x0F; *p++ = 0x84; tagpu_detour_rel(p, VA_ROUTE_FRAME); p += 4;  /* je: consumed   */
    *p++ = 0x0F; *p++ = 0x87; tagpu_detour_rel(p, VA_ROUTE_CLICK); p += 4;  /* ja: the click  */
    memcpy(p, want, 6); p += 6;                             /* cmp edx,0x201             */
    *p++ = 0x0F; *p++ = 0x85; tagpu_detour_rel(p, VA_ROUTE_FRAME); p += 4;  /* jne           */
    *p++ = 0xE9; tagpu_detour_rel(p, VA_ROUTE_CLICK);                       /* jmp           */
    rel = (int32_t)((unsigned)(uintptr_t)s - (VA_ROUTE + 5));
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);
    memset(patch + 5, 0x90, sizeof patch - 5);
    if (!tagpu_detour_write(VA_ROUTE, patch, sizeof patch)) { tagpu_detour_stub_free(s); return 0; }
    return 1;
}

/* THE CURSOR CHOOSER'S CALL. 0x48D220(order) is stdcall, `ret 4`, and reads only the low byte
   of its argument (DISASSEMBLED); the handler pushes the cursor mode main+0x2CC3 as that
   argument, reads nothing after the call but EAX, and reloads main from 0x511DE8 at once
   (0x49929C) -- so a stdcall function of one dword returning in EAX, which preserves what the C
   ABI preserves, stands in for it exactly. The wrapper calls the chooser itself and hands its
   answer to tagpu_sweep_cursor, which returns it unchanged outside the sweep. */
typedef int (__stdcall* Chooser)(int);

static int __stdcall cursor_handler(int mode)
{
    return tagpu_sweep_cursor(mode & 0xFF, ((Chooser)VA_CHOOSER)(mode));
}

/* The site: the push of the mode and the call, `50 E8 84 3F FF FF` at 0x499296, decoded to
   0x48D220 before anything is written; then the call's rel32 (the four bytes after E8) is
   rewritten to reach `fn`, relative to the call's own end. Nothing else in the tree writes the
   call (a grep of src/). */
static int cursor_matches(unsigned va)
{
    const unsigned char* p = (const unsigned char*)(uintptr_t)va;
    int32_t rel;
    if (p[0] != 0x50 || p[1] != 0xE8) return 0;
    memcpy(&rel, p + 2, 4);
    return va + 6 + (unsigned)rel == VA_CHOOSER;
}

static int cursor_write(unsigned va, void* fn)
{
    int32_t rel = (int32_t)((unsigned)(uintptr_t)fn - (va + 6));
    return tagpu_detour_write(va + 2, (const unsigned char*)&rel, 4);
}

void tagpu_route_init(void)
{
    /* mov eax,[0x511DE8]; sub esp,0x18 */
    static const unsigned char handler[] = {0xA1,0xE8,0x1D,0x51,0x00,0x83,0xEC,0x18};
    /* the stub first: route_button answers stock until each client arms */
    if (install_route()) s_ok |= TAGPU_ROUTE_STUB;
    else tagpu_logf("route: the click routing (0x4993BF) holds other bytes; not installed");
    if (tagpu_detour_bytes_ok(VA_HANDLER, handler, sizeof handler) &&
        tagpu_detour_observe(VA_HANDLER, handler, sizeof handler, handler_entry, NULL))
        s_ok |= TAGPU_ROUTE_ENTRY;
    else tagpu_logf("route: the in-play handler's entry (0x499200) holds other bytes or refused "
                    "its observer; not installed");
    if (!cursor_matches(VA_CURSOR))
        tagpu_logf("route: the cursor chooser's call (0x499297) holds other bytes; not installed");
    else if (!cursor_write(VA_CURSOR, (void*)cursor_handler))
        tagpu_logf("route: the cursor chooser's call could not be written; not installed");
    else s_ok |= TAGPU_ROUTE_CURSOR;
    if (s_ok == (TAGPU_ROUTE_STUB | TAGPU_ROUTE_ENTRY | TAGPU_ROUTE_CURSOR))
        tagpu_logf("route: armed (stub 0x4993BF, entry 0x499200, cursor 0x499297)");
}
