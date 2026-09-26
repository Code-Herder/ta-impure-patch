/* tagpu_patches.c — our own runtime engine patches on the loaded TotalA.exe image.
   Applied from DllMain via VirtualProtect; the on-disk exe stays pristine. */

#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_patches.h"
#include "tagpu_limits.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "tagpu_regstore.h"
#include "tagpu_weapons.h"
#include "tagpu_datakeys.h"
#include "git.h"

static void plog(const char* s)
{
    tagpu_log(s);
}

/* Write `val` at absolute `addr` iff it currently holds `expect`. Returns 1 on patch,
   0 if the byte didn't match (wrong build) or protection change failed. */
static int patch_byte(unsigned int addr, unsigned char expect, unsigned char val)
{
    unsigned char* p = (unsigned char*)addr;
    DWORD old;
    int ok;
    if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    ok = (*p == expect);
    if (ok)
        *p = val;
    VirtualProtect(p, 1, old, &old);
    return ok;
}

/* Write `n` bytes at absolute `addr` iff they currently hold `expect`. Returns 1 on
   patch, 0 if the bytes didn't match (wrong build) or protection change failed. */
static int patch_bytes(unsigned int addr, const unsigned char* expect,
                       const unsigned char* val, unsigned int n)
{
    unsigned char* p = (unsigned char*)addr;
    DWORD old;
    unsigned int i;
    int ok = 1;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    for (i = 0; i < n; i++)
        if (p[i] != expect[i]) { ok = 0; break; }
    if (ok)
        for (i = 0; i < n; i++)
            p[i] = val[i];
    VirtualProtect(p, n, old, &old);
    return ok;
}

/* ===== THE FAIL-CLOSED SITE TABLE ======================================================
   Every site that changes the simulation goes into ONE table: the raised limits (tagpu_limits.h,
   the raised build only) and the defect fixes whose absence would let a player silently play
   by stock rules (the fixes below that say so, in both builds). The table is compared with the
   stock 3.1 bytes as a whole and written as a whole, or not at all, by tagpu_limits_install,
   and a mismatch ends the process through tagpu_limits_report. A fix whose absence changes only
   a crash, a draw, a message or a malformed input's fate is local instead: it checks and writes
   its own sites, and is skipped with its reason logged (research/notes/tadr-port/sim-fixes.md,
   "How B fixes are held").

   WHY BEFORE ANYTHING RUNS: DllMain runs before TotalA.exe's entry point (ddraw.dll is its
   first static import), so no game exists yet and no engine thread executes these bytes
   while they change. Nothing here is ever put back: the patches last for the process. */

#define LIM_MAXSITE  256
#define LIM_MAXB     80    /* a whole replaced block, so the check covers what the stub stands for */

typedef struct LIMSITE {
    unsigned int  va;
    unsigned char n;
    unsigned char stock[LIM_MAXB];
    unsigned char ours[LIM_MAXB];
    unsigned char have[LIM_MAXB];   /* what the image held when compared            */
    unsigned char differs;
    const char*   name;
} LIMSITE;

static LIMSITE s_lim[LIM_MAXSITE];
static int     s_nlim;
static int     s_limState;           /* 0 not tried, 1 installed, -1 failed          */
static int     s_limOverflow;        /* the table itself was too small: our bug      */
static int     s_limNoStub;          /* a code stub could not be made                */
static unsigned int s_limWriteFail;  /* the site VirtualProtect refused, 0 = none    */
static unsigned int s_limOverlapA, s_limOverlapB;   /* two sites over one byte: our bug  */

static void lim_add(unsigned int va, int n, const unsigned char* stock,
                    const unsigned char* ours, const char* name)
{
    LIMSITE* s;
    if (s_nlim >= LIM_MAXSITE || n <= 0 || n > LIM_MAXB) { s_limOverflow = 1; return; }
    s = &s_lim[s_nlim++];
    memset(s, 0, sizeof *s);
    s->va = va; s->n = (unsigned char)n; s->name = name;
    memcpy(s->stock, stock, (size_t)n);
    memcpy(s->ours, ours, (size_t)n);
}

#ifndef TAGPU_LIMITS_STOCK
static void lim_dword(unsigned int va, unsigned int stock, unsigned int ours, const char* name)
{
    lim_add(va, 4, (const unsigned char*)&stock, (const unsigned char*)&ours, name);
}
#endif

/* an instruction whose last four bytes are an address or an operand we choose */
static void lim_op(unsigned int va, int n, const unsigned char* stock,
                   const unsigned char* prefix, int np, unsigned int value, const char* name)
{
    unsigned char ours[LIM_MAXB];
    int k;
    if (np + 4 > n || n > LIM_MAXB) { s_limOverflow = 1; return; }
    memcpy(ours, prefix, (size_t)np);
    memcpy(ours + np, &value, 4);
    for (k = np + 4; k < n; k++) ours[k] = 0x90;
    lim_add(va, n, stock, ours, name);
}

/* E8/E9 rel32 at `va` to `target`, NOP-padded to n bytes */
static void lim_branch(unsigned int va, int n, const unsigned char* stock, unsigned char op,
                       unsigned int target, const char* name)
{
    unsigned int rel = target - (va + 5);
    lim_op(va, n, stock, &op, 1, rel, name);
}

/* a site that is compared and never changed: bytes a stub relies on without writing them */
static void lim_same(unsigned int va, int n, const unsigned char* stock, const char* name)
{
    lim_add(va, n, stock, stock, name);
}

/* a fix of the table whose code stub could not be made: nothing of the table is written */
static void lim_no_stub(void)
{
    s_limNoStub = 1;
}

/* one site's bytes, without trusting the page to be readable */
static int lim_read(unsigned int va, unsigned char* out, int n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void*)(size_t)va, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    if ((size_t)va + (size_t)n > (size_t)mbi.BaseAddress + mbi.RegionSize) return 0;
    memcpy(out, (const void*)(size_t)va, (size_t)n);
    return 1;
}


/* ---- the -r switch, in a tacli test launch ----------------------------------

   In a test launch TotalA.exe's registry is a file (tagpu_regstore.h), answered through
   its registry imports. The one registry write of the exe's own code that passes through
   none of them is the `-r` switch's: its handler 0x49F249 loads dsetup.dll and calls
   DirectXRegisterApplicationA, which writes DirectPlay's application key through
   dsetup.dll's own imports, then quits (cmdline-options.md). The parser
   CmdlineArgsNormalize 0x49EE30 (called at 0x49E8D2; token loop head 0x49EED3) dispatches
   on the letter after the dash through the index bytes 0x49F500 ('B'..'w') into the jump
   table 0x49F494: 'R' holds case 9 and 'r' case 22, and both entries
   (0x49F4B8, 0x49F4EC) are 0x49F249; case 26 (0x49F4FC) is the loop tail 0x49F461, where
   every letter the parser does not know goes [DISASSEMBLED 2026-09-25]. Pointing both
   entries at the tail makes -r an unknown switch, ignored. A test folder whose exe
   differs there is not run: the switch would still reach the real registry. */
static void close_register_switch(void)
{
    static const unsigned char handler[4] = { 0x49, 0xF2, 0x49, 0x00 };   /* 0x49F249 */
    static const unsigned char tail[4]    = { 0x61, 0xF4, 0x49, 0x00 };   /* 0x49F461 */

    /* expect == val: a match test, nothing written */
    if (patch_bytes(0x0049F4FC, tail, tail, 4) &&
        patch_bytes(0x0049F4B8, handler, tail, 4) &&
        patch_bytes(0x0049F4EC, handler, tail, 4)) {
        plog("registry: test mode -- the -r switch (DirectPlay registration through dsetup.dll) "
             "is ignored (jump table 0x49F494, cases 9 and 22 -> 0x49F461)");
        return;
    }
    plog("registry: TEST MODE, but the -r switch's jump table at 0x49F494 is not the one this DLL "
         "knows, so the switch could write the real registry: the game is not run");
    TerminateProcess(GetCurrentProcess(), 1);
}

/* ---- defects of the stock engine -------------------------------------------

   Places where TotalA.exe itself goes wrong: it writes or reads memory it does
   not own, takes a player's payment and does not deliver, or loads a saved game
   without all of what it saved. Each patch below is the identity on every input
   the stock code handles correctly and differs only where the stock code would
   write past an allocation, read through NULL, read off the end of the tile
   map, leave a paid-for feature standing, pay for one feature twice, or drop a
   saved feature, or where it silently plays against its own rules: an explosion
   that hits one victim once per cell, an aircraft on the map's last column that
   nothing can hit, a unit near the north edge that nobody can see. The engine
   map (exe-reverse-engineering.md, "Engine defects we patch") has the
   disassembly, the callers and the measurements; binary-patches.md lists them.
   All are installed at every attach: ddraw.dll is a static import of the exe,
   so DllMain runs before the exe's entry point.

   TWO CLASSES [DECIDED 2026-09-25, research/notes/tadr-port/sim-fixes.md]. A fix
   whose absence would let a player silently compute different shared state, on
   an input stock does not fault on, is a SIMULATION fix: its sites go into the
   fail-closed table above, and a mismatch ends the process with the report. A
   fix whose absence changes only a crash, a draw, a message or a malformed
   input's fate is LOCAL: it is skipped, with its reason logged, when its bytes
   differ from the retail exe, its stub cannot be allocated, or its page cannot
   be made writable. Each fix below names its class. */

/* why a defect patch did not go in; the log line names it */
enum { FIX_ARMED, FIX_BYTES, FIX_STUB, FIX_PROTECT, FIX_LIMITS, FIX_TABLE };

static const char* fix_state(int r)
{
    switch (r) {
    case FIX_ARMED: return "ARMED";
    case FIX_BYTES: return "SKIPPED (the bytes differ from the retail exe)";
    case FIX_STUB:  return "SKIPPED (its stub could not be made)";
    case FIX_LIMITS: return "with the raised limits (the limits line)";
    case FIX_TABLE: return "in the fail-closed table (the limits line)";
    default:        return "SKIPPED (VirtualProtect of the site failed)";
    }
}

/* THE SORT BUFFER'S END, in DrawGameScreen's unit binning (0x4697CF..0x469840).
   [DISASSEMBLED] With edi = main+0x141FB, every hot unit (main+0x1435F, NumHotUnits
   main+0x14367) is binned by the 16-px row of its feet, row = (unit+0x74 - eyeY)/16
   + 16, tested 0 <= row < rows at 0x469800/0x469805, and appended to that row:
   count[row]++ (u16, [edi+0x08] = SORT LINE COUNT), *cursor[row]++ = unit
   ([edi+0x04] = SORT INDICES). Nothing compares count[row] with anything.

   LoadMap sizes the buffer once per map (0x483D27..0x483D45): cap = viewW/16 + 12
   into [edi+0x50] (main+0x1424B), rows = viewH/16 + 32 into [edi+0x54]
   (main+0x1424F), and SORT UNIT LIST = rows * cap * 4 bytes into [edi]
   (main+0x141FB); the per-frame reset at 0x46971B points cursor[r] at
   base + r*cap*4 and 0x469731 zeroes count[r]. The two readers (0x4699A8,
   0x469B54) walk count[r] slots from base + r*cap*4 with no bound either. So a row
   holding more than cap hot units runs on into the next row's slots — which the
   readers tolerate: every slot a row counts was written this frame, by that row
   or by the one it ran into — and a row near the end runs past the end of the
   allocation, a heap overwrite with unit pointers. cap is one unit per 16-px
   column swept; units whose feet share a row band beyond that (a dense line or
   stack, aircraft over one spot, or any crowd wider than the 1x view, which the
   rect vpwide widens at zoom < 1 hands the cull) are what reach it.

   THE FIX bounds the append by the ALLOCATION, not by the row: a row may still
   run on into later rows, exactly as stock, but never past the buffer's end. A
   jmp at 0x469807 (the 31-byte append block, NOPped behind it) to:

       mov   edx,[edi+0x54]           ; rows
       sub   edx,eax                  ; rows - row, >= 1 (0x469805)
       imul  edx,[edi+0x50]           ; the slots from this row's start to the end
       mov   ecx,[edi+0x08]
       lea   ecx,[ecx+eax*2]          ; &count[row]
       movzx esi,word [ecx]
       cmp   esi,edx
       jge   join                     ; the next slot is past the end: not binned
       inc   word [ecx]
       mov   edx,[edi+0x04]           ; SORT INDICES
       mov   ecx,[edx+eax*4]          ; the row's cursor
       jecxz join                     ; stock's NULL-cursor skip
       mov   [ecx],ebp                ; append the unit
       add   dword [edx+eax*4],4
     join:
       jmp   0x469826                 ; the stock join

   THE INVARIANT: count[row] <= (rows - row) * cap after every append. The cursor
   and the count start together each frame and move together only here, so the
   slot written is base + (row*cap + count)*4 < base + rows*cap*4, and every slot
   a reader walks is inside the allocation and was written this frame. It rests on
   [edi+0x50]/[edi+0x54] being the values the allocation was made with: LoadMap is
   their only writer, through that one base register, and nothing of ours writes
   them. The count is a u16 and cannot wrap: the cull 0x48BAE0 files each slot of
   the unit array (stride 0x118) at most once, so a frame's appends to any row are
   at most the unit slots, far below 65536. Registers: eax (row), edi and ebp (unit)
   are stock's inputs; eax, ecx, edx and esi are dead at 0x469826, which reloads
   esi. No branch from outside the block lands in 0x469808..0x469825, and stock's
   own 0x46981B -> 0x469826 is the only branch to the join [a rel8/rel32 scan of
   .text]. Identical to stock for every unit whose slot is inside the buffer; a
   unit whose slot would be past it is not drawn by the engine's sweep that frame.
   Bounding by the row instead would drop units stock draws correctly — a row run
   on into an empty neighbour is drawn whole. The list feeds nothing but
   DrawGameScreen's two draw loops, so the simulation reads nothing different. */
/* CLASS: local. The list feeds DrawGameScreen's draw loops alone. */
static int fix_sort_buffer_end(void)
{
    static const unsigned char was[31] = {
        0x8B, 0x57, 0x04,                   /* mov edx,[edi+0x4]        */
        0x8D, 0x0C, 0x82,                   /* lea ecx,[edx+eax*4]      */
        0x8B, 0x57, 0x08,                   /* mov edx,[edi+0x8]        */
        0x66, 0xFF, 0x04, 0x42,             /* inc word [edx+eax*2]     */
        0x8D, 0x04, 0x42,                   /* lea eax,[edx+eax*2]      */
        0x8B, 0x01,                         /* mov eax,[ecx]            */
        0x85, 0xC0,                         /* test eax,eax             */
        0x74, 0x09,                         /* je 0x469826              */
        0x89, 0x28,                         /* mov [eax],ebp            */
        0x8B, 0x01,                         /* mov eax,[ecx]            */
        0x83, 0xC0, 0x04,                   /* add eax,4                */
        0x89, 0x01,                         /* mov [ecx],eax            */
    };
    static const unsigned char body[39] = {
        0x8B, 0x57, 0x54,                   /* mov edx,[edi+0x54]       */
        0x29, 0xC2,                         /* sub edx,eax              */
        0x0F, 0xAF, 0x57, 0x50,             /* imul edx,[edi+0x50]      */
        0x8B, 0x4F, 0x08,                   /* mov ecx,[edi+0x8]        */
        0x8D, 0x0C, 0x41,                   /* lea ecx,[ecx+eax*2]      */
        0x0F, 0xB7, 0x31,                   /* movzx esi,word [ecx]     */
        0x39, 0xD6,                         /* cmp esi,edx              */
        0x7D, 0x11,                         /* jge join                 */
        0x66, 0xFF, 0x01,                   /* inc word [ecx]           */
        0x8B, 0x57, 0x04,                   /* mov edx,[edi+0x4]        */
        0x8B, 0x0C, 0x82,                   /* mov ecx,[edx+eax*4]      */
        0xE3, 0x06,                         /* jecxz join               */
        0x89, 0x29,                         /* mov [ecx],ebp            */
        0x83, 0x04, 0x82, 0x04,             /* add dword [edx+eax*4],4  */
    };
    unsigned char now[31];
    unsigned char* s;

    if (memcmp((const void*)0x00469807, was, sizeof was) != 0) return FIX_BYTES;
    s = tagpu_detour_stub();
    if (!s) return FIX_STUB;
    memcpy(s, body, sizeof body);
    s[sizeof body] = 0xE9;                                  /* join: jmp 0x469826 */
    tagpu_detour_rel(s + sizeof body + 1, 0x00469826);
    /* the jmp is encoded against 0x469807, where it will run — not against
       this buffer (tagpu_detour_rel encodes against its own address) */
    memset(now, 0x90, sizeof now);
    now[0] = 0xE9;                                          /* jmp stub           */
    {
        unsigned int rel = (unsigned int)(size_t)s - (0x00469807u + 5u);
        memcpy(now + 1, &rel, 4);
    }
    if (!tagpu_detour_write(0x00469807, now, sizeof now)) {
        VirtualFree(s, 0, MEM_RELEASE);
        return FIX_PROTECT;
    }
    return FIX_ARMED;
}

/* A NULL PLOT, handed to GetGridPosFeature 0x421E60 (stdcall(plot), ret 4).
   [DISASSEMBLED] Its first two instructions are `mov ecx,[esp+4]` and
   `mov ax,[ecx+8]`: the plot's feature index is read with no test, and "no
   feature" is `or ax,0xFFFF` at 0x421E9C. GetGridPosPLOT 0x481550 returns NULL
   for a cell outside main+0x14233 x main+0x14237. 0x4815F0 does too, and also for
   a cell ON the grid that holds 0xFFFE (a multi-cell feature's non-anchor cell)
   when the anchor offset in its bytes +0xA/+0xB leads off the grid
   (0x48164A..0x481682). Three callers [E8 scan of .text]: 0x47EAE3 tests the plot
   first (0x47EADA). 0x498F4F does not: it is the cursor's hover feature in
   0x498DA0, whose cell comes from GetTPosition's row, and GetTPosition can answer
   up to 143 px below the point it is handed. 0x40514A does not either: it is the
   target lookup of the order handler 0x404DB0 through 0x4815F0 on the order's
   position, and its reachability is not audited. For 0x498F4F stock keeps the
   point inside the scroll extent main+0x1422F, which the level load writes as the
   map's height less 128 (0x4833E0) and only the debug-level `Edge` console
   command 0x416730 rewrites, and that margin is exactly what keeps GetTPosition's
   row on the map, as long as the camera clamp 0x41C3C0 can hold the eye in
   [0, extent - view]. On a map whose extent is shorter than the viewport that
   range is empty and 0x41C40D..0x41C431 alternate the eye between 0 and a
   negative value; at 0 the viewport's bottom row is past the extent
   [INFERRED from the disassembly]. A point past the extent reads [NULL+8] at
   0x421E64: MEASURED, with our clamp at 0x498EF9 disabled.

   THE FIX, a prologue detour: the stub runs the first stolen instruction, and
   for a NULL plot returns the engine's own "no feature" 0xFFFF with the
   function's `ret 4`; otherwise it runs the second and resumes at 0x421E68.
   THE INVARIANT: 0x421E60 never dereferences NULL, whichever caller hands it
   the plot. Identity for every non-NULL plot; for a NULL one the stock code
   faults, so nothing the simulation reads changes except where it would have
   crashed. No branch lands inside the eight stolen bytes [rel8/rel32 scan]. */
/* CLASS: local. Without it the read through NULL faults. */
static int fix_feature_null_plot(void)
{
    static const unsigned char was[16] = {
        0x8B, 0x4C, 0x24, 0x04,             /* mov ecx,[esp+4]          */
        0x66, 0x8B, 0x41, 0x08,             /* mov ax,[ecx+8]           */
        0x66, 0x3D, 0xFB, 0xFF,             /* cmp ax,0xFFFB            */
        0x72, 0x32,                         /* jb 0x421EA0              */
        0x66, 0x3D,                         /* cmp ax,0xFFFE ...        */
    };
    unsigned char* s;
    unsigned char* p;

    if (memcmp((const void*)0x00421E60, was, sizeof was) != 0) return FIX_BYTES;
    s = p = tagpu_detour_stub();
    if (!s) return FIX_STUB;
    *p++ = 0x8B; *p++ = 0x4C; *p++ = 0x24; *p++ = 0x04;     /* mov ecx,[esp+4]  */
    *p++ = 0x85; *p++ = 0xC9;                               /* test ecx,ecx     */
    *p++ = 0x75; *p++ = 0x07;                               /* jnz +7           */
    *p++ = 0x66; *p++ = 0x0D; *p++ = 0xFF; *p++ = 0xFF;     /* or ax,0xFFFF     */
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;                  /* ret 4            */
    *p++ = 0x66; *p++ = 0x8B; *p++ = 0x41; *p++ = 0x08;     /* mov ax,[ecx+8]   */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00421E68); p += 4;   /* jmp 0x421E68     */
    if (!tagpu_detour_land(0x00421E60, s, 8)) {
        VirtualFree(s, 0, MEM_RELEASE);
        return FIX_PROTECT;
    }
    return FIX_ARMED;
}

/* THE TERRAIN PASS'S WINDOW, in 0x483FA0 (stdcall(ctx), ret 4; one caller, 0x468DB0
   in DrawGameScreen). [DISASSEMBLED] From the eye main+0x1431F/0x14323 it takes
   col0 = eyeX/32 and row0 = eyeY/32, truncating toward zero, the offsets
   sx = eyeX - 32*col0 and sy likewise (each in (-32, 32), with the eye's sign),
   and from the view size main+0x37E37/0x37E3B the counts ncols = ceil((W + sx)/32)
   and nrows likewise. It reads the tile map *(main+0x1428B) at
   row*(main+0x14233 / 2) + col for every cell of that window (0x4840A6, 0x4841A9,
   0x4841D6, 0x484345) and the tile graphic at *(*(main+0x14283)+4) + id*0x400,
   with no compare on the index or the id. It paints cell (i, j) of the window at
   (L - sx + 32*j, T - sy + 32*i): the edge columns and rows through the clipped
   blitter 0x4B8150, the rest through the unclipped 0x4C6E70. LoadMap allocates
   the tile map as (pxW/32)*(pxH/32) u16 from the map's pixel size
   main+0x14223/0x14227 (0x48393C..0x483969) and copies the TNT's ids in raw
   (0x48397D), and the tile set as {count, pixels} with count*0x400 bytes of
   graphics (0x483B53..0x483B80).

   WHERE STOCK IS RIGHT. ncols = ceil((W + sx)/32) carries the painted span to
   L + W or past it for any sx, but it starts at L - sx: the viewport's left
   column is painted only when sx >= 0, and its top row only when sy >= 0. So the
   pass reads only map cells AND paints every viewport pixel exactly when
   sx >= 0, sy >= 0 and the window lies inside the tile map; with col0 and row0
   truncated, that is 0 <= eye and eye + view <= 32 * tiles on each axis. Outside
   it, stock does one of two wrong things:
     - an eye in (-32, 0) truncates to col0 = 0: every read is on the map, but the
       leftmost -sx columns (or top -sy rows) keep the last frame's pixels;
     - an eye at or below -32 starts the window before the map, and an eye with
       eye + view past the map's far edge runs it off the end: the id read there
       is whatever the heap holds, and the tile pointer made from it is anywhere.
       MEASURED under the engine's alternating clamp (below): an access
       violation reading that tile at 0x4CBE44 (the row copy 0x4CBDD1 called
       by 0x4B8150) on the first in-play draw of Lava Run at 1920x1440 (row0 = -7) and 3840x2160 (row0 = -29), and
       of Coast To Coast at 3840x2160 (row0 = -6).
   What hands the pass such a window is the camera clamp 0x41C3C0: it holds the
   eye in [0, extent - view] (the extent is the map less 32 px wide and 128
   tall), and where the view is larger than the extent it alternates the eye
   between 0 and the negative extent - view (0x41C40D..0x41C431). With
   tagpu_zoom.on our clamp replaces it and holds the eye at 0 there
   (tagpu_zoom.c, zoom_eye_clamp), so the window leaves the map only where the
   view is larger than the map; the camera's centre range goes below 0 at every
   left and top edge, but only on draws our terrain pass owns, where this pass
   does not run. MEASURED on Lava Run at 1920x1440 with that clamp: the same
   fault from row0 = 0, the window's rows 40..42 past the tile map.

   THE FIX, a jump at 0x484057, the first point at which every value the pass
   places and indexes with is computed and nothing has been read through it. The
   stub hands the pass's frame to terrain_window_on_map. Where stock is right, it
   returns 1 and the stub runs the two stolen instructions and resumes at
   0x484061: the stock pass, unchanged. Otherwise terrain_window_draw draws the
   window itself and the stub leaves through the pass's own epilogue 0x4843AC.

   THE INVARIANT: whatever the eye and the view, the pass reads no tile-map entry
   outside (pxW/32)*(pxH/32) and paints every viewport pixel inside the clip rect
   (our path refuses a context whose clip rect is not inside its surface, and
   then draws nothing rather than write outside it). Only our path bounds the
   tile ids by the tile set's count: the stock path reads them unchecked, so a
   map whose ids reach past its tile set still reads past it there. Identical
   to stock on every draw where stock is right; the draws it changes are the ones where stock leaves a strip unpainted,
   reads before the tile map or past its end, or — on a map narrower than the
   view — reads the next row's cells at the right edge, which stays inside the
   allocation on every row but the last. Registers at 0x484057: ecx (main), esi
   (sx), edi (sy) and ebp (0) are live, eax and ebx are what the stolen
   instructions load, edx is overwritten by the `cdq` at 0x484061, and the flags
   are set again at 0x48406B before anything tests them; pushad/popad keep them
   all. 0x484050's `je` lands on 0x484057 itself, the jump; no branch lands in
   0x484058..0x484060 [rel8/rel32 scan of .text]. */

/* the pass's frame at 0x484057, as dword indexes from its esp: the four registers
   it saved at +0x00, its 0x48 bytes of locals from +0x10, its return address at
   +0x58 and its argument, the OFFSCREEN, at +0x5C */
enum { TPF_NCOLS = 0x10 / 4, TPF_COL0 = 0x14 / 4, TPF_NROWS = 0x18 / 4,
       TPF_T = 0x1C / 4, TPF_L = 0x20 / 4, TPF_ROW0 = 0x24 / 4, TPF_CTX = 0x5C / 4 };
/* the stub's pushad block, as dword indexes */
enum { TPR_EDI = 0, TPR_ESI = 1, TPR_ECX = 6 };

#define TP_MAP_PXW    0x14223   /* the map's own size in px, which the tile map is */
#define TP_MAP_PXH    0x14227   /* allocated from                                   */
#define TP_PLOT_C     0x14233   /* 16-px cells across; the pass's stride is half    */
#define TP_TILE_SET   0x14283   /* {u32 count; u8* pixels}                         */
#define TP_TILE_MAP   0x1428B   /* u16 tile id per 32-px cell                      */
#define TP_VIEW_W     0x37E37
#define TP_VIEW_H     0x37E3B
/* the engine's OFFSCREEN (terrain-depth.md 4): width and height, which
   SurfaceCreateNamed 0x4C69F0 stores at 0x4C6A2C/0x4C6A35 and sizes the pixels
   by, then pitch, pixel base and the inclusive clip rect */
enum { CTX_W = 0, CTX_H = 1, CTX_PITCH = 2, CTX_BASE = 3, CTX_CLIP_L = 7,
       CTX_CLIP_T = 8, CTX_CLIP_R = 9, CTX_CLIP_B = 10 };

static int ptr_sane(const void* p)
{
    return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u;
}

static int floor32(int v)
{
    return v >= 0 ? v / 32 : -((31 - v) / 32);
}

/* The window the pass was about to draw, drawn with every read bounded: the
   viewport filled with palette index 0 (the black the fog paints unexplored
   ground with), then each cell of the same window that is on the map, and whose
   id is below the tile set's count, copied to where stock puts it,
   (L - sx + 32*j, T - sy + 32*i). Every write is inside the context's clip rect,
   and that rect is checked against the surface's own width and height, the size
   SurfaceCreateNamed allocates the pixels with, before anything is written; a
   context whose rect or size does not validate is not drawn at all. GAME THREAD,
   inside DrawGameScreen. */
static void terrain_window_draw(const char* ta, const unsigned int* regs, const int* f,
                                int tilesW, int tilesH)
{
    const int* ctx = (const int*)(size_t)(unsigned)f[TPF_CTX];
    const unsigned short* tmap;
    const unsigned int* tset;
    const unsigned char* pix;
    unsigned char* base;
    unsigned int count;
    int L = f[TPF_L], T = f[TPF_T], W, H, pitch, x0, y0, x1, y1, y, i, j;
    int ox, oy, i0, i1, j0, j1;

    if (!ptr_sane(ctx)) return;
    pitch = ctx[CTX_PITCH];
    base = (unsigned char*)(size_t)(unsigned)ctx[CTX_BASE];
    if (!ptr_sane(base) || pitch <= 0 || pitch > 16384) return;
    W = *(const int*)(ta + TP_VIEW_W);
    H = *(const int*)(ta + TP_VIEW_H);
    /* sizes and an origin no screen has are refused before any sum is formed */
    if (W <= 0 || H <= 0 || W > 32768 || H > 32768 ||
        L < -65536 || L > 65536 || T < -65536 || T > 65536) return;
    {
        int sw = ctx[CTX_W], sh = ctx[CTX_H];
        int cl = ctx[CTX_CLIP_L], ct = ctx[CTX_CLIP_T];
        int cr = ctx[CTX_CLIP_R], cb = ctx[CTX_CLIP_B];
        /* the rect is inclusive, so its last column and row must be inside the
           surface: cr < width <= pitch and cb < height */
        if (!(sw > 0 && sh > 0 && sw <= pitch && sh <= 16384 &&
              cl >= 0 && ct >= 0 && cr >= cl && cb >= ct && cr < sw && cb < sh))
            return;
        x0 = L > cl ? L : cl;
        y0 = T > ct ? T : ct;
        x1 = L + W < cr + 1 ? L + W : cr + 1;
        y1 = T + H < cb + 1 ? T + H : cb + 1;
    }
    if (x1 <= x0 || y1 <= y0) return;
    for (y = y0; y < y1; y++)
        memset(base + (size_t)y * (size_t)pitch + x0, 0, (size_t)(x1 - x0));

    tmap = *(const unsigned short* const*)(ta + TP_TILE_MAP);
    tset = *(const unsigned int* const*)(ta + TP_TILE_SET);
    if (!ptr_sane(tmap) || !ptr_sane(tset)) return;
    count = tset[0];
    pix = (const unsigned char*)(size_t)tset[1];
    if (!ptr_sane(pix)) return;

    /* the cells of the pass's window that meet the drawn rect; (ox, oy) is where
       stock puts its first cell, and sx, sy lie in (-32, 32) */
    ox = L - (int)regs[TPR_ESI];
    oy = T - (int)regs[TPR_EDI];
    i0 = floor32(y0 - oy);      i1 = floor32(y1 - 1 - oy);
    j0 = floor32(x0 - ox);      j1 = floor32(x1 - 1 - ox);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > f[TPF_NROWS] - 1) i1 = f[TPF_NROWS] - 1;
    if (j1 > f[TPF_NCOLS] - 1) j1 = f[TPF_NCOLS] - 1;
    for (i = i0; i <= i1; i++) {
        int r = f[TPF_ROW0] + i, ty = oy + 32 * i;
        int cy0 = ty > y0 ? ty : y0, cy1 = ty + 32 < y1 ? ty + 32 : y1;
        if (r < 0 || r >= tilesH) continue;
        for (j = j0; j <= j1; j++) {
            int c = f[TPF_COL0] + j, tx = ox + 32 * j;
            int cx0 = tx > x0 ? tx : x0, cx1 = tx + 32 < x1 ? tx + 32 : x1;
            unsigned int id;
            const unsigned char* src;
            if (c < 0 || c >= tilesW) continue;
            id = tmap[(size_t)r * (size_t)tilesW + (size_t)c];
            if (id >= count) continue;
            src = pix + (size_t)id * 0x400u;
            for (y = cy0; y < cy1; y++)
                memcpy(base + (size_t)y * (size_t)pitch + cx0,
                       src + (size_t)(y - ty) * 32u + (size_t)(cx0 - tx),
                       (size_t)(cx1 - cx0));
        }
    }
}

/* 1: stock is right for this window (see above) and the stock pass runs. 0: it was
   drawn here and the pass returns. The tile map's own dimensions come from the
   words LoadMap sized it with, and the stock path is taken only where the pass's
   stride agrees with them, so the bound does not rest on main+0x14233 alone. */
static int __cdecl terrain_window_on_map(const unsigned int* regs)
{
    const int* f = (const int*)(regs + 8);
    const char* ta = (const char*)(size_t)regs[TPR_ECX];
    int stride = *(const int*)(ta + TP_PLOT_C) / 2;
    int tilesW = *(const int*)(ta + TP_MAP_PXW) / 32;
    int tilesH = *(const int*)(ta + TP_MAP_PXH) / 32;
    int sx = (int)regs[TPR_ESI], sy = (int)regs[TPR_EDI];
    int col0 = f[TPF_COL0], row0 = f[TPF_ROW0];
    int ncols = f[TPF_NCOLS], nrows = f[TPF_NROWS];

    if (tilesW <= 0 || tilesH <= 0) {
        tilesW = tilesH = 0;             /* no map to read: the fill alone */
    } else if (stride == tilesW && sx >= 0 && sy >= 0 &&
               col0 >= 0 && ncols > 0 && ncols <= tilesW - col0 &&
               row0 >= 0 && nrows > 0 && nrows <= tilesH - row0) {
        return 1;
    }
    terrain_window_draw(ta, regs, f, tilesW, tilesH);
    return 0;
}

/* CLASS: local. A draw, and a fault where stock reads off the tile map. */
static int fix_terrain_window(void)
{
    static const unsigned char was[10] = {
        0x8B, 0x81, 0x33, 0x42, 0x01, 0x00, /* mov eax,[ecx+0x14233]    */
        0x8B, 0x5C, 0x24, 0x5C,             /* mov ebx,[esp+0x5c]       */
    };
    unsigned char* s;
    unsigned char* p;

    if (memcmp((const void*)0x00484057, was, sizeof was) != 0) return FIX_BYTES;
    s = p = tagpu_detour_stub();
    if (!s) return FIX_STUB;
    *p++ = 0x60;                                            /* pushad           */
    *p++ = 0x54;                                            /* push esp         */
    *p++ = 0xE8;                                            /* call             */
    tagpu_detour_rel(p, (unsigned int)(size_t)terrain_window_on_map); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                  /* add esp,4        */
    *p++ = 0x85; *p++ = 0xC0;                               /* test eax,eax     */
    *p++ = 0x61;                                            /* popad            */
    *p++ = 0x74; *p++ = 0x0F;                               /* jz drawn         */
    memcpy(p, was, sizeof was); p += sizeof was;            /* the stolen pair  */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00484061); p += 4;   /* jmp 0x484061     */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x004843AC); p += 4;   /* drawn: epilogue  */
    if (!tagpu_detour_land(0x00484057, s, (int)sizeof was)) {
        VirtualFree(s, 0, MEM_RELEASE);
        return FIX_PROTECT;
    }
    return FIX_ARMED;
}

/* A FEATURE THAT IS PAID FOR AND NOT REMOVED. [DISASSEMBLED] FeatureDie 0x423550,
   stdcall(x, y, reclaimed), ret 0xC, turns a feature into its successor: FeatureDef +0xF8
   (featurereclamate) when reclaimed, +0xF4 (featuredead) when not. It resolves a multi-cell
   feature's anchor and writes the anchor's x and y back over its own arguments (0x423580).
   A 3DO feature, or a GAF one with no sequence for the event, goes to 0x4236EF: `push ebx;
   push ebp; push esi; call 0x423710`, the swap itself -- destroy (0x4246B0), then create the
   successor (0x423C50). A GAF feature with a sequence plays it first: it takes a record from
   the wreck pool, marks the cell (flags bit 0) and returns, and the update loop swaps it when
   the sequence ends (0x424495 -> 0x423710; the record's bit 1 keeps `reclaimed`). With the
   pool empty, 0x42361D sets the "no record" count and 0x423651 `jge 0x4236F7` returns having
   done neither. Its callers have already acted: the reclaim 0x4237D0 pays the feature's
   energy (+0xEC) and metal (+0xF0) at 0x4238EF or 0x4238FF / 0x42395B before calling it at
   0x423965, and in a network game then sends the event (0x0F, 0xFF, x, y) that 0x4554B0
   hands to FeatureDie on every peer. The cell is left unmarked, so the reclaim accepts the same feature again:
   metal for nothing, as often as the builder repeats it. MEASURED 2026-09-24 in a ten-player
   game at 1500 units a player: the free-list head main+0x1421B read -1 on the three peers
   read.

   THE FIX retargets that jge to a stub that reloads x and y from the arguments -- esi and ebp
   hold the pool base by then -- and joins 0x4236EF, the engine's own path for a feature that
   has no sequence. THE INVARIANT: every FeatureDie that reaches the pool either starts the
   sequence that ends in the swap or swaps now, whatever the pool holds. At 0x423651 the stack
   is 0x18 below the return address (sub 8, four pushes), so x is [esp+0x1C] and y [esp+0x20];
   ebx still holds `reclaimed` (0x4235C6), and 0x4236EF's epilogue restores esi and ebp from
   the stack. Identity while the pool has a free record; with none, the successor appears
   without the sequence. The only other branch to 0x4236EF is stock's own at 0x4235FC
   [rel8/rel32 scan]. The cmp's operand at 0x42364D belongs to the limits table (the pool's
   count), so it is not compared here -- only its opcode and the jge's six bytes. */
/* CLASS: simulation, fail closed. Without it a player keeps a feature the reclaim paid for. */
static int fix_feature_die_pool_full(void)
{
    static const unsigned char cmp = 0x3D;                                     /* cmp eax,imm32 */
    static const unsigned char jge[6] = { 0x0F, 0x8D, 0xA0, 0x00, 0x00, 0x00 }; /* jge 0x4236F7 */
    unsigned char now[6];
    unsigned char* s;
    unsigned char* p;

    s = p = tagpu_detour_stub();
    if (!s) { lim_no_stub(); return FIX_TABLE; }
    *p++ = 0x8B; *p++ = 0x74; *p++ = 0x24; *p++ = 0x1C;     /* mov esi,[esp+0x1c]  x  */
    *p++ = 0x8B; *p++ = 0x6C; *p++ = 0x24; *p++ = 0x20;     /* mov ebp,[esp+0x20]  y  */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x004236EF); p += 4;   /* jmp 0x4236EF: the swap */
    now[0] = 0x0F; now[1] = 0x8D;                           /* jge stub               */
    {
        unsigned int rel = (unsigned int)(size_t)s - (0x00423651u + 6u);
        memcpy(now + 2, &rel, 4);
    }
    lim_same(0x0042364C, 1, &cmp, "full wreck pool: FeatureDie's pool test");
    lim_add(0x00423651, sizeof now, jge, now, "full wreck pool: the feature swap");
    return FIX_TABLE;
}

/* A FEATURE THAT IS PAID FOR TWICE. [DISASSEMBLED] The reclaim completion 0x4237D0(who, pos),
   stdcall, ret 8, refuses a GAF feature that is already playing its sequence: 0x423892 tests
   the cell's flags bit 0, 0x423898 the def's GAF bit (+0xFE bit 0), and both set return 0
   before anything is paid. But the flags it tests are the TARGETED cell's (esi, from
   0x481550 at 0x4237FE), and FeatureDie marks only the anchor (0x42368F); every other cell
   of a multi-cell feature keeps bit 0 clear (0x423F4D, 0x4247BB). A reclaim that lands on
   any cell but the anchor while the sequence plays is paid in full, and its FeatureDie then
   returns at 0x423606 on the marked anchor. MEASURED 2026-09-24, Town & Country, free
   records: two commanders ordered together onto Building15's centre cell (2900 metal) were
   paid +5959 in the same moment; onto its anchor cell, +3056 -- paid once.

   THE FIX tests the anchor's flags. The stub resolves a 0xFFFE cell to its anchor exactly
   as 0x423845..0x423862 does for the def -- cell - (row * width + col) * 13, the offsets
   bytes +0x0A/+0x0B of the cell -- so it reads byte +0x0C of the cell stock already reads
   byte +0x08 of at 0x423864: no address stock does not form. THE INVARIANT: the mark the
   reclaim tests is the anchor's, the one every sequence in play sets -- a reclaim or death
   sequence (FeatureDie, 0x42368F) or a burn (0x4233A0, 0x423468) -- so such a feature refuses
   every reclaim, whichever of its cells the builder aimed at, as stock refuses one aimed at
   its anchor. eax and ebx are dead here (eax is reloaded at 0x4238B9 or zeroed at 0x4238A1;
   ebx ends as 3 * (row * width + col), the value stock's own anchor path leaves in it), ecx
   (the FeatureDef) and edx (main) are untouched, and esi is reloaded at 0x4238AD or restored
   by the epilogue. Identity on a one-cell feature, on an anchor, on an unmarked feature, and
   on a 3DO feature, which the def test pays either way. The one reclaim it refuses that stock
   paid only once: through another cell of a multi-cell feature that is burning or dying and
   has no reclaim sequence, which FeatureDie swaps before it reads the mark (0x4235FC) -- the
   reclaim stock already refuses through the anchor. */
/* CLASS: simulation, fail closed. Without it a group reclaim pays once per builder. */
static int fix_reclaim_mark_anchor(void)
{
    static const unsigned char was[6] = {
        0xF6, 0x46, 0x0C, 0x01,             /* test byte [esi+0xc],1    */
        0x74, 0x15,                         /* je 0x4238AD              */
    };
    static const unsigned char anchor[] = {
        0x66, 0x81, 0x7E, 0x08, 0xFE, 0xFF, /* cmp word [esi+0x8],0xfffe */
        0x75, 0x20,                         /* jne test                 */
        0xA1, 0xE8, 0x1D, 0x51, 0x00,       /* mov eax,[0x511de8]       */
        0x8B, 0x80, 0x33, 0x42, 0x01, 0x00, /* mov eax,[eax+0x14233]    */
        0x0F, 0xB6, 0x5E, 0x0A,             /* movzx ebx,byte [esi+0xa] */
        0x0F, 0xAF, 0xC3,                   /* imul eax,ebx             */
        0x0F, 0xB6, 0x5E, 0x0B,             /* movzx ebx,byte [esi+0xb] */
        0x03, 0xC3,                         /* add eax,ebx              */
        0x8D, 0x1C, 0x40,                   /* lea ebx,[eax+eax*2]      */
        0x8D, 0x04, 0x98,                   /* lea eax,[eax+ebx*4]      */
        0x2B, 0xF0,                         /* sub esi,eax              */
        0xF6, 0x46, 0x0C, 0x01,             /* test: test byte [esi+0xc],1 */
    };
    unsigned char* s;
    unsigned char* p;

    s = p = tagpu_detour_stub();
    if (!s) { lim_no_stub(); return FIX_TABLE; }
    memcpy(p, anchor, sizeof anchor); p += sizeof anchor;
    *p++ = 0x0F; *p++ = 0x84; tagpu_detour_rel(p, 0x004238AD); p += 4;  /* je 0x4238AD  */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00423898); p += 4;               /* jmp 0x423898 */
    lim_branch(0x00423892, (int)sizeof was, was, 0xE9, (unsigned int)(size_t)s,
               "reclaim: the anchor's mark");
    return FIX_TABLE;
}

/* A SAVED FEATURE ON THE MAP'S BORDER. [DISASSEMBLED] LoadMap 0x483610 ends by calling
   0x4833B0 (at 0x483CF1), which masks the border of the feature grid: over a fixed set of
   cells -- columns W-2 and W-1, the top rows whose projected y (row*16 - height/2) is
   negative, the row above each bottom cell whose projected y is past the scroll extent, and,
   when [main+0x391E9]+0xD44 is set, every cell at or below the sea level -- it writes 0xFFFD
   over a cell that is EMPTY (0xFFFF) or a footprint cell (0xFFFE) and leaves an anchor alone.
   That function is the only producer of 0xFFFD [every 0xFFFD immediate in .text]. A spawn
   refuses it: SpawnFeatureOnMap 0x423C50 hands every footprint cell that is not EMPTY to
   FEATURES_Destroy 0x4246B0, which refuses a def at 0xFFFB and up, and the spawn is abandoned.
   On a new game the TNT's features are down before the mask, so a feature there keeps its
   anchor and loses only its masked footprint cells. On a saved game LoadMap places none of the
   TNT's features (main+0x38D6B, the save's TDF, is set) -- a v2 map still gets its void markers,
   0xFFFC, at 0x483ACA..0x483ADE -- so the mask runs over a grid with no feature, and the
   features come back later, from the save: the game-load routine 0x432610 calls 0x424C00 at
   0x43265A, which spawns every saved feature -- "Normal Features" (0x424FBF), "Animating
   Features" (0x425050), "3D Features" (0x425180) -- and never tests the result. So a saved
   feature with its anchor or any footprint cell on a masked cell does not come back. MEASURED
   2026-09-25 on Two Continents (672 x 800 cells, 4893 features), a save and its load: 51 do
   not, four in rows 0..2 and 47 in rows 794..797, every one of them an anchor the mask had left
   standing on the new game. The restore then
   goes on as though the spawn had succeeded: an animating or 3D record's state is written
   into the wreck-pool record the cell's +0x0A names (0x4250C8, 0x42518D), a word LoadMap
   never initialises (0x4839D5..0x4839ED writes +0x00, +0x02, +0x07, +0x08 and two bits of
   +0x0C of a fresh cell); fix_restore_record_owner, below, closes that.

   THE FIX retargets the call at 0x43265A to features_restore_under_mask, which opens the mask
   for the restore and shuts it after: every cell holding 0xFFFD is noted in a bitmap and set
   EMPTY, 0x424C00 runs unchanged, and every noted cell that is then EMPTY or 0xFFFE is set back
   to 0xFFFD -- the mask's own rule, over the mask's own cells. The pathing maps follow. They
   are built once a load from the grid as the mask left it (0x440940 at 0x4918E3, before the
   restore), and every grid change the restore makes refreshes them over the cells it changed:
   a spawn (0x440A40 from 0x424031) and a destroy (from 0x424822), both reading the mask open.
   The shut is the one change made without a refresh, so each cell it rewrites is refreshed
   after it with 0x440A40(x | y << 16, 1 | 1 << 16): 0x440830 then recomputes, for every
   movement class, the entries over [x - fw, x + 1] x [y - fh, y + 1], which are the entries
   whose read region (0x47E1F0) holds that cell.
   THE INVARIANT: the grid after the restore is the mask applied to the restored features, as
   a new game's grid is the mask applied to the TNT's -- a restored feature keeps its anchor on
   a masked cell and loses its masked footprint cells, and every other masked cell reads 0xFFFD
   again -- and the pathing maps are that grid's. A map entry depends on the def words of its
   read region and on the units standing there, and no unit is on the grid during the restore
   (the units are restored after it, 0x486FD0 from 0x432672, and LoadMap's init zeroes every
   cell's +0x00). So an entry is stale only if a cell of its region changed after its last
   computation: the open changes cells without a refresh, but every opened cell ends either
   shut, and refreshed after the shut, or under a feature a spawn placed and refreshed over
   after the open. It rests on
     - a bound: the mask is opened only when the grid holds no feature (no def below 0xFFFB and
       no 0xFFFE), the state 0x424C00 is called in, so no cell it opens is covered by a feature,
       every anchor after the restore is a restored feature, and the restore is the only code
       that sees a cell open. The grid is read as LoadMap sized it, W * H cells of 13 bytes
       (0x483986..0x4839A2), W and H from main+0x14233/+0x14237 and refused outside 1..4096;
       the pointer and both counts are compared again after the restore, and the cells are shut
       only when they are unchanged;
     - an ordering: the restore runs on the loader thread, inside the level load, where the
       engine writes the grid and the pathing maps itself, before any unit is restored.
   Anything outside the bound -- no main block, a feature already down, a grid out of range,
   no memory for the bitmap -- leaves the restore to stock, logged. That fallback cannot set
   two peers apart: a saved game never runs in a network session (ARMOPT.GUI's handler 0x460CC0
   greys SAVEGAME and LOADGAME in a game of type 3, at 0x460CF7/0x460D37 through 0x4A1200; the
   console's Save, 0x417430, wants access level 4; a load from the main menu rebuilds the game
   type from the save's Gametype, 0x49267F -> 0x434AB0). 0x424C00 has this one caller and no
   pointer to it in the image; neither has 0x432610 but the one at 0x497B29 [call and literal
   scan of the image]. Identity on a new game, whose load never reaches 0x424C00, and on a saved
   game with no feature on a masked cell. */
#define SF_PLOT_W   0x14233     /* 16-px cells across and down                      */
#define SF_PLOT_H   0x14237
#define SF_GRID     0x14287     /* FeatureStruct[W * H], 13 bytes, the def at +0x08  */
#define SF_CELL     13
#define SF_EMPTY    0xFFFFu
#define SF_MARKER   0xFFFBu     /* a def at or above is not a feature                */
#define SF_MASKED   0xFFFDu
#define SF_FOOT     0xFFFEu
#define SF_NDEFS    0x14253     /* the FeatureDef count                             */
#define SF_DEFS     0x1426F     /* FeatureDef[], 0x100 bytes                         */

typedef void (__stdcall *features_restore_fn)(void* tdf);
/* 0x440A40(xy, wh), stdcall: every movement class's pathing map recomputed over the cells
   whose passability reads the rectangle; xy = x | y << 16, wh = w | h << 16 */
typedef void (__stdcall *path_refresh_fn)(unsigned int xy, unsigned int wh);

static void __stdcall features_restore_under_mask(void* tdf)
{
    const features_restore_fn restore = (features_restore_fn)0x00424C00;
    const path_refresh_fn refresh = (path_refresh_fn)0x00440A40;
    char* ta = *(char* const*)0x00511DE8;
    unsigned char* grid;
    unsigned char* bits;
    unsigned int w, h, n, i, opened = 0, closed = 0, kept = 0;
    const char* why = NULL;
    DWORD t0 = 0, t1 = 0;
    char b[256];

    if (!ptr_sane(ta)) {
        restore(tdf);
        plog("savedfeat: no main block; the restore ran with the border mask shut");
        return;
    }
    grid = *(unsigned char* const*)(ta + SF_GRID);
    w = *(const unsigned int*)(ta + SF_PLOT_W);
    h = *(const unsigned int*)(ta + SF_PLOT_H);
    if (!ptr_sane(grid) || w < 1 || w > 4096 || h < 1 || h > 4096) {
        restore(tdf);
        plog("savedfeat: the grid is out of range; the restore ran with the border mask shut");
        return;
    }
    n = w * h;
    for (i = 0; i < n; i++) {
        unsigned int def = *(const unsigned short*)(grid + (size_t)i * SF_CELL + 8);
        if (def < SF_MARKER || def == SF_FOOT) {
            why = "a feature is already down";
            break;
        }
    }
    bits = why ? NULL : (unsigned char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (n + 7) / 8);
    if (!why && !bits) why = "no memory for the bitmap";
    if (why) {
        restore(tdf);
        _snprintf(b, sizeof b, "savedfeat: %s; the restore ran with the border mask shut", why);
        b[sizeof b - 1] = 0;
        plog(b);
        return;
    }
    for (i = 0; i < n; i++) {
        unsigned short* def = (unsigned short*)(grid + (size_t)i * SF_CELL + 8);
        if (*def == SF_MASKED) {
            bits[i >> 3] |= (unsigned char)(1u << (i & 7));
            *def = (unsigned short)SF_EMPTY;
            opened++;
        }
    }

    restore(tdf);

    if (*(unsigned char* const*)(ta + SF_GRID) == grid &&
        *(const unsigned int*)(ta + SF_PLOT_W) == w &&
        *(const unsigned int*)(ta + SF_PLOT_H) == h) {
        t0 = GetTickCount();
        for (i = 0; i < n; i++) {
            unsigned short* def;
            if (!(bits[i >> 3] & (1u << (i & 7)))) continue;
            def = (unsigned short*)(grid + (size_t)i * SF_CELL + 8);
            if (*def == SF_EMPTY || *def == SF_FOOT) {
                *def = (unsigned short)SF_MASKED;
                refresh((i % w) | ((i / w) << 16), 1u | (1u << 16));
                closed++;
            } else if (*def < SF_MARKER) {
                kept++;
            }
        }
        t1 = GetTickCount();
        _snprintf(b, sizeof b,
                  "savedfeat: the restore ran with the border mask open: %u cells opened, "
                  "%u shut again and the pathing maps refreshed around each in %lu ms, %u hold "
                  "a restored feature's anchor", opened, closed, (unsigned long)(t1 - t0), kept);
    } else {
        _snprintf(b, sizeof b, "savedfeat: the restore replaced the grid; %u opened cells "
                  "left to it", opened);
    }
    b[sizeof b - 1] = 0;
    plog(b);
    HeapFree(GetProcessHeap(), 0, bits);
}

/* CLASS: simulation, fail closed. Without it a loaded game silently lacks the border's
   features. Retail has no saved multiplayer game, so the class follows from what the fix changes
   -- the game's own state -- rather than from a second peer. */
static int fix_saved_features_border(void)
{
    static const unsigned char was[5] = { 0xE8, 0xA1, 0x25, 0xFF, 0xFF };  /* call 0x424C00 */
    unsigned char now[5];

    now[0] = 0xE8;
    /* encoded against 0x43265A, where the call runs -- not against this buffer */
    {
        unsigned int rel = (unsigned int)(size_t)features_restore_under_mask - (0x0043265Au + 5u);
        memcpy(now + 1, &rel, 4);
    }
    lim_add(0x0043265A, sizeof now, was, now, "saved features: the restore under the mask");
    return FIX_TABLE;
}

/* A SAVED FEATURE'S STATE WRITTEN INTO A WRECK RECORD IT DOES NOT OWN. [DISASSEMBLED] The
   restore 0x424C00 spawns each "Animating Features" record (0x425050) and starts its sequence --
   FeatureDie(x, y, 1) at 0x42507E, FeatureDie(x, y, 0) at 0x42509D or the burn 0x4233A0 at
   0x4250BB -- and then, at 0x4250C0..0x4250F4, writes the record's saved state into the wreck
   record the cell's +0x0A names: +0x26 (word), +0x04 (word, the sequence's frame) and +0x2E (the
   high nibble). Each "3D Features" record is spawned (0x425180) and then, at 0x425185..0x4251A2,
   writes its +6 word into +0x26 of the record the cell's +0x0A names. Neither tests anything
   first. +0x0A names the feature's record only when the feature holds one: a 3DO anchor for its
   whole life (SpawnFeatureOnMap takes the record and stores its index at 0x423ED5, or abandons
   the spawn), a GAF anchor while its sequence plays (FeatureDie stores it at 0x42368B and sets
   the mark, +0x0C bit 0, at 0x423695; the burn at 0x42345C and 0x423468). Otherwise it is a word
   no record of this feature stands behind: a GAF anchor's 0 (0x423EE9), a footprint cell's
   offsets, the index an anchor kept when FEATURES_Destroy freed its record (0x42476F; +0x0A is
   never cleared), or a cell nothing has written, which LoadMap leaves as the allocator handed it
   (its init loop 0x4839D5..0x4839ED writes +0x00, +0x02, +0x07, +0x08 and two bits of +0x0C). So
   the state lands in another feature's record -- record 0 on a grid the system handed over
   zeroed, which is what a load gets (MEASURED on Two Continents: no cell of the 537 600 had a
   nonzero +0x0A before the restore, and a 3D record refused for want of a record wrote record
   0's +0x26) -- or, through a stale word, up to 0xFFFF records past the pool's base (3 MB),
   whenever the record's feature is not what it was saved as. Its spawn refused: a void cell, a
   footprint past the map's edge (0x423CF8/0x423D24), an indestructible occupant (FeatureDef
   +0xFF bit 1: FEATURES_Destroy returns 0 at 0x42471E), a 3DO def with no record left. Or its
   sequence not started: with no record left an Animating GAF spawn still succeeds, takes none
   and leaves +0x0A at 0, and then the burn returns at 0x42340D/0x423439 and FeatureDie swaps the
   feature for its successor at once (0x423651, above) -- so the write lands on record 0, live on
   a full pool. And the anchor there may be of the other kind: content changed between the save
   and the load (a GAF name that now loads a 3DO def, 0x424D23..0x424DE1), or a swap's 3DO
   successor that destroyed a neighbour, freeing its record, and took it. An Animating write
   there puts a frame word over the 3DO record's +0x04, the pointer to its object state
   (0x45A8D0, from 0x423ECB; freed by 0x45AAA0).

   THE FIX sends both sites through restore_record_owned, a jump at 0x4250C0 and at 0x425185 (the
   6-byte `mov edx,[0x511de8]` each block begins with, compared first together with the 6 bytes
   after it); the block runs as stock only when the cell at the record's position holds a feature
   of the record's own kind that owns a record -- for an Animating record a GAF anchor with its
   mark set (a sequence in play), for a 3D record a 3DO anchor; either below the def count and
   with FeatureDef +0xFE bit 0 set for GAF, clear for 3DO -- and its +0x0A is below the pool's
   count. That is the rule the save writer applies (0x424890: an Animating record only for a GAF
   anchor with the mark, 0x4249CF -> 0x424A6E; a 3D record only for a 3DO anchor, 0x4249DC).
   Otherwise the stub leaves for the loop's next record (0x4250F7, 0x4251A7).
   THE INVARIANT: the restore writes a saved state only into the wreck record that the feature
   standing on the record's cell owns, of the kind the state was saved from -- the record the
   feature took in this restore -- and never outside the pool. It rests on
     - a lifetime: a record is owned exactly while its index is in +0x0A of an anchor that is 3DO,
       or GAF and marked. FEATURES_Destroy frees the record (0x42476F) and, in the same call,
       clears the mark and sets the anchor EMPTY (0x424774..0x424780), and the swap frees through
       it (0x423792/0x4237B1); the index it leaves in +0x0A is refused by the def test;
     - a bound: the cell must lie in the grid LoadMap sized (W * H cells of 13 bytes), the def
       below main+0x14253, and the index below the pool's count, read from the pool's own
       allocation size, the dword at 0x421F2A that 0x421F20 allocates it with (0x18000 bytes in
       stock, which the limits table rewrites before any level exists).
   A record whose feature did not come back as saved loses its saved state, which has nowhere to
   go. Registers: pushad/popad around the test, whose flags survive the popad; at both skip targets
   the loop reloads everything but edi, ebx and ebp, which the stub preserves. Branches into the
   two sites land on 0x4250C0 itself (0x425065, 0x425083, 0x4250A2); none lands inside either
   stolen instruction [rel8/rel32 scan of .text]. Identity for every record whose feature owns
   its record, which is every record of a save loaded with the same feature content into a pool
   at least as large as the one it was saved from, with no void cell, no footprint past the
   map's edge and no indestructible occupant under it. */
#define RR_POOL_BYTES 0x00421F2Au   /* 0x421F20's `push imm32`: the pool's size in bytes */
#define RR_RECORD     0x30u
#define RR_GAF        0x01          /* FeatureDef +0xFE: a GAF feature, else a 3DO one   */
#define RR_MARK       0x01          /* cell +0x0C: a sequence in play                    */
#define RR_ANIMATING  0             /* the stubs' kind argument                          */
#define RR_3D         1

static int __cdecl restore_record_owned(const unsigned char* cell, int kind)
{
    const char* ta = *(char* const*)0x00511DE8;
    const unsigned char* grid;
    const unsigned char* defs;
    unsigned int w, h, def, ndefs, idx, count;
    size_t off;

    if (!ptr_sane(ta)) return 0;
    grid = *(const unsigned char* const*)(ta + SF_GRID);
    w = *(const unsigned int*)(ta + SF_PLOT_W);
    h = *(const unsigned int*)(ta + SF_PLOT_H);
    if (!ptr_sane(grid) || w < 1 || w > 4096 || h < 1 || h > 4096 || cell < grid) return 0;
    off = (size_t)(cell - grid);
    if (off % SF_CELL || off / SF_CELL >= (size_t)w * h) return 0;
    def = *(const unsigned short*)(cell + 8);
    ndefs = *(const unsigned int*)(ta + SF_NDEFS);
    defs = *(const unsigned char* const*)(ta + SF_DEFS);
    if (def >= SF_MARKER || def >= ndefs || !ptr_sane(defs)) return 0;
    if (kind == RR_ANIMATING) {
        if (!(defs[(size_t)def * 0x100 + 0xFE] & RR_GAF) || !(cell[0x0C] & RR_MARK)) return 0;
    } else if (defs[(size_t)def * 0x100 + 0xFE] & RR_GAF) {
        return 0;
    }
    idx = *(const unsigned short*)(cell + 0x0A);
    count = *(const unsigned int*)RR_POOL_BYTES / RR_RECORD;
    return idx < count;
}

/* CLASS: simulation, fail closed, as the restore above. */
static int fix_restore_record_owner(void)
{
    static const unsigned char was[12] = {
        0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00,     /* mov edx,[0x511de8]  (stolen) */
        0x33, 0xC0,                             /* xor eax,eax                  */
        0x66, 0x8B, 0x46, 0x0A,                 /* mov ax,[esi+0xa]             */
    };
    static const unsigned int site[2] = { 0x004250C0, 0x00425185 };
    static const unsigned int next[2] = { 0x004250F7, 0x004251A7 };
    static const int kind[2] = { RR_ANIMATING, RR_3D };
    unsigned char* s[2] = { NULL, NULL };
    int k;

    for (k = 0; k < 2; k++) {
        unsigned char* p = s[k] = tagpu_detour_stub();
        unsigned char now[sizeof was];
        if (!p) { lim_no_stub(); return FIX_TABLE; }
        *p++ = 0x60;                                            /* pushad           */
        *p++ = 0x6A; *p++ = (unsigned char)kind[k];             /* push kind        */
        *p++ = 0x56;                                            /* push esi: cell   */
        *p++ = 0xE8;
        tagpu_detour_rel(p, (unsigned int)(size_t)restore_record_owned); p += 4;
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                  /* add esp,8        */
        *p++ = 0x85; *p++ = 0xC0;                               /* test eax,eax     */
        *p++ = 0x61;                                            /* popad            */
        *p++ = 0x74; *p++ = 0x0B;                               /* jz next          */
        memcpy(p, was, 6); p += 6;                              /* the stolen mov   */
        *p++ = 0xE9; tagpu_detour_rel(p, site[k] + 6); p += 4;  /* the stock write  */
        *p++ = 0xE9; tagpu_detour_rel(p, next[k]); p += 4;      /* next: skip it    */
        /* the jmp and a NOP over the stolen mov; the six bytes after it are compared too */
        now[0] = 0xE9;
        {
            unsigned int rel = (unsigned int)(size_t)s[k] - (site[k] + 5u);
            memcpy(now + 1, &rel, 4);
        }
        now[5] = 0x90;
        memcpy(now + 6, was + 6, sizeof was - 6);
        lim_add(site[k], sizeof was, was, now, k ? "saved features: a 3D record's owner"
                                                 : "saved features: an Animating record's owner");
    }
    return FIX_TABLE;
}

/* THE COMPOSITE SCRATCH FRAME. [DISASSEMBLED] The composite draw context ctx = *(main+0x1437B)
   keeps one frame at ctx+0x10, made at the level load by 0x458180 through 0x4B8E00(name, w, h)
   and freed by the teardown 0x4581C0 through 0x4D85A0. 0x4B8E00 lays it out as a 0x18-byte
   header -- +0 width u16, +2 height u16, +4/+6 hotspot s16, +8 key, +9..+B flags -- then a
   colour plane at +0x10 = base+0x18 and a depth plane at +0x14 = colour + w*h, w*h bytes each:
   A = [f+0x14] - [f+0x10] is the frame's area. The limits table sizes it 1280 x 1280 (stock
   600 x 600). Five writers size it to one unit and none compares that size with A, all
   thiscall on ctx, all on the game thread:
     - the build-state copy 0x4589C0 sets the header to the box of the unit and its build
       pieces (esi x edx at 0x458B87) and fills or copies w*h of both planes; one caller,
       0x459608 in the unit draw 0x459200;
     - the frame copy 0x45A470(src) copies src's header and w*h of both planes; three callers
       (0x459338, 0x4594DB, 0x45958C);
     - the shadow build 0x45A790 sets w x h from the bounds 0x45A510 leaves at [esp+0x10] and
       [esp+0x20] (at 0x45A7B9), fills w*h of both planes, draws, then run-length encodes the
       colour plane INTO the depth plane (0x4B9E60 from 0x45A85B: two bytes a row and at most
       two a pixel, so 2h(w+1) bytes); callers 0x4592FE, 0x45955B;
     - the 2x bakes 0x459830 and 0x459C70(src, ...) set 2w x 2h from src (a `shl` on the u16)
       and fill, draw and downsample that much, when their fourth argument asks for the 2x
       path (0x459875, 0x459CB5); src is [esp+0x5F18] / [esp+0x159E8] there.
   0x459170 is a sixth, a twin of the frame copy, with no caller and no pointer to it in the
   image [call and literal scan of .text]: it never runs and is not patched.
   A unit whose box passes A -- a quarter of it for the bake, half for the shadow -- writes
   past the allocation, on every lane (only GDI presents the result, but the game thread
   builds it on all of them, MEASURED). And A bounds area, not height: polygons are drawn
   into the frame through rasterisers that keep one 0x28-byte stack entry a row, clipped only
   to the frame's height - 1, in tables of 800 entries (0x4C8BB0, 0x4C8760) or 2048 (0x4C0820,
   0x4C0C70, 0x4C1000); an h-row destination writes at most h - 1, so bounding rows at the
   table's size is one row inside it. A frame taller than the table under it overruns the
   rasteriser's stack upward, over its return address and then its arguments: MEASURED, a 2x
   bake grown to 1548 rows faulted at 0x4C8035 with its texture argument overwritten. The build-state copy's header is drawn under by 0x4C0820 (its own last call,
   0x458D0E) and by 0x4C8760: the unit draw's 1x bake at 0x459641 is 0x459830(ctx+0x10, ...,
   0) with the scratch as its source, and the 1x path rasterises into its source by the
   source's header (0x459B96). The shadow is drawn under by 0x4C1000 (0x45A750), the 2x bakes
   by 0x4C8760 / 0x4C8BB0 as well as 0x4C1000 / 0x4C0C70.

   THE FIX: before each writer forms its size, a check computes what it will write (need) and
   how many rows, and compares them with A and with the smallest span table under that writer
   (800 rows for the build-state copy and for the 2x bakes' 2h, 2048 for the shadow). If it
   fits the writer runs unchanged. If not, the frame grows: a new
   block of 0x18 + 2*px bytes, px >= need rounded up to 256 Ki pixels and at most 2048 x 2048,
   from the engine's own allocator path (below), laid out as 0x4B8E00 lays it out, with the
   header and both planes copied; ctx+0x10 is repointed and the old block freed with 0x4D85A0,
   the free 0x4581C0 uses. If the grow is refused -- the need is over 2048 x 2048, the rows are
   over the span table, the lever `tagpu_scratch.nogrow`, the allocator is out of memory or in
   its debug-heap mode, or the frame is the writer's own source -- the writer takes a fallback
   that writes nothing past A:
     - build-state copy: leaves through its epilogue 0x458D13 and flags the refusal, and the
       wrapper on its one call (0x459608) leaves the unit draw through 0x4597D8, the draw's
       own "no frame" exit (0x459212): the unit is not drawn this frame;
     - frame copy: the frame becomes 1 x 1 holding src's key, with src's hotspot and key, and
       the function returns it as stock does (`mov eax,[ecx+0x10]; ret 4`): nothing drawn;
     - shadow build: the frame becomes 1 x 1 holding its own key, hotspot 0, and the function
       resumes at the encode 0x45A853: the unit's cached shadow is one transparent pixel;
     - 2x bake: the 1x path (0x459913 / 0x459D57), which draws into src by src's own header.
   Never a clip: a header must say what the planes hold.

   THE CARGO MERGE 0x4B90A0(src, dst, sx, sy, dbias), whose one caller is 0x4596D8 in the unit
   draw's cargo loop, paints a carried unit's own frame into the scratch at x0 = dst.hotx -
   src.hotx + sx, y0 = dst.hoty - src.hoty + sy by the scratch's header width, and refuses only a
   negative origin: nothing clips its right or bottom edge [0x4B90A0..0x4B9190]. It fits in stock
   because the build-state copy's box is the union of the carrier's and every cargo's boxes, taken
   with the same projection and margins (0x4581E0, 0x458310). But between the two the cargo's own
   bake runs (0x459670, 0x4586A0(cargo, 1, -1)), and its 2x path would leave the scratch's header
   at the cargo's doubled box -- which the engine's attach guard should make unreachable (a
   structure is never attached, 0x48ABC7..0x48AC1D), and nothing here depends on that. The call
   is sent through scratch_merge, which runs the merge only when x0 + src.w <= dst.w, y0 + src.h
   <= dst.h and dst.w * dst.h <= A, and otherwise leaves the cargo out of this frame's composite.

   THE INVARIANT: every header a writer sets has w*h <= A, and a shadow's 2h(w+1) <= A, and no
   more rows than the span tables under that writer hold, and the merge writes only inside the
   header's box, so no writer, no rasteriser and no reader sized by a header leaves its
   allocation or its stack table. It rests on:
     - a bound: A is read from the frame's own two pointers, which have exactly two producers,
       0x4B8E00 at the level load and scratch_hold here, and both lay the frame out this way.
       The layout test in scratch_area (colour == base+0x18, depth > colour) is a sanity
       filter on those values, not the argument; a frame failing it is left to stock, logged.
       Every fallback's 1 x 1 fits: A >= 360000 from 0x4B8E00 and >= 64 from a grow;
     - an ordering: each writer reads ctx+0x10 only after its check [every read of +0x10 in
       each writer]; its callers re-read ctx+0x10 after every call (0x45933D..0x459342,
       0x4594E3, 0x4595B8, 0x4595D9, 0x459639, 0x4596BA); no function a writer calls reaches
       another writer [call graph of .text: 0x458310 is a leaf, 0x458DD0, 0x4B8A80, 0x4B7F90,
       0x45A510, 0x45A610, 0x4B96A0, 0x4B9D70, 0x4B9E60, 0x437B50 and the bakes' callees
       reach none];
       and a frame is never regrown under a writer that is reading from it (src == frame
       refuses the grow instead);
     - a lifetime: the old block is freed only after ctx+0x10 points at the new one, and it
       has no other holder -- the render thread never reads it, and our tracer only logs the
       pointer's value.
   Identical to stock for every unit whose box fits the frame as allocated. Grows persist for
   the level: the teardown frees whichever block ctx+0x10 holds. Not bounded here: a unit's own
   frame (Object3do+0x10), which the 1x bakes draw into through the same rasterisers, so a model
   more than 800 rows tall still overruns their table (exe-reverse-engineering.md, "The composite
   scratch frame and the rasterisers' span tables").

   THE ALLOCATOR. The engine's malloc 0x4D83B0 -> 0x4D83C0 calls the out-of-memory handler at
   [0x5289BC] when malloc fails, and the installed handler 0x49E700 ends the process. A grow
   must be able to fail, so scratch_alloc runs 0x4D83C0's own success path by hand: inside the
   allocator's critical section (0x4DA780 returns it, 0x528A28), refuse if 0x4D80D0 reports the
   debug heap (its blocks come from 0x4DACF0 and go back through 0x4DB7D0), else CRT malloc
   0x4E8890 and, on success, the byte counters 0x4DA7D0(size). The CRT's new-handler flag
   0x52A430 is never written, so 0x4E8890 answers NULL rather than calling a handler.
   0x4D85A0's non-debug path frees exactly such a block (0x4D8360, 0x4DA840, 0x4E8820). The
   debug fill 0x4D82C0 that 0x4D83C0 applies when its option is set is not applied: every
   plane is written before it is read.

   Levers, read once at attach: `tagpu_scratch.stress` treats every frame as too small, so
   every writer call regrows it to exactly max(need, 64) pixels and frees the old one, with
   the old block's two plane pointers set to SCR_POISON first so that a reader still holding
   the frame faults at any plane access instead of reading bytes the heap has not yet reused
   (a reader that kept a plane pointer itself is not caught); `tagpu_scratch.nogrow` refuses
   every grow, so every oversized writer takes its fallback.
   Registers: every check stub saves them all (pushad/popad) around the check, and the flags
   the stolen instructions set are set after it. The merge's stub is a jmp to scratch_merge, a
   __stdcall that keeps ebx, esi, edi and ebp and clobbers eax, ecx and edx as 0x4B90A0 does;
   nothing from 0x4596DD on reads those three before writing them. No branch lands inside the stolen bytes
   [rel8/rel32 scan of .text]; 0x459608's call is the only reference to 0x4589C0, and
   0x4596D8's the only one to 0x4B90A0. */

#define SCR_CAP_PX    (2048u * 2048u)
#define SCR_ROUND_PX  0x40000u
#define SCR_MIN_PX    64u
#define SCR_ROWS_POLY 2048u     /* 0x4C0820, 0x4C0C70, 0x4C1000: 2048-entry span tables */
#define SCR_ROWS_SPAN 800u      /* 0x4C8760, 0x4C8BB0: 800                              */
/* the stress lever's freed-frame plane pointers: TotalA.exe is not large-address-aware (PE
   characteristics 0x10B), so nothing at or above 2 GB is the process's and any plane offset
   (at most 8 MB) from here faults */
#define SCR_POISON    ((unsigned char*)0x80000000u)

enum { SCR_BUILD, SCR_FRAME, SCR_SHADOW, SCR_BAKE1, SCR_BAKE2 };
static const char* const SCR_WHO[] = {
    "the build-state copy 0x4589C0", "the frame copy 0x45A470", "the shadow build 0x45A790",
    "the 2x bake 0x459830", "the 2x bake 0x459C70",
};

/* the stubs' pushad block, as dword indexes; the site's esp is regs + 8 */
enum { SCR_EDI = 0, SCR_ESI = 1, SCR_EBP = 2, SCR_EBX = 4, SCR_EDX = 5, SCR_ECX = 6,
       SCR_SITE = 8 };

static int s_scr_stress, s_scr_nogrow;
static volatile unsigned char s_scr_refused;     /* the build-state copy's answer, GAME THREAD */
static unsigned s_scr_grows[5], s_scr_refusals[5], s_scr_layout;   /* by writer */
static unsigned s_scr_merge_refusals;

/* A, or 0 when the frame is not in 0x4B8E00's layout */
static unsigned scratch_area(const unsigned char* f)
{
    const unsigned char* colour;
    const unsigned char* depth;
    if (!ptr_sane(f)) return 0;
    colour = *(const unsigned char* const*)(f + 0x10);
    depth  = *(const unsigned char* const*)(f + 0x14);
    if (colour != f + 0x18 || depth <= colour ||
        (size_t)depth - (size_t)colour > 0x10000000u)       /* unsigned: no ptrdiff overflow */
        return 0;
    return (unsigned)((size_t)depth - (size_t)colour);
}

static void scratch_log(const char* what, int who, unsigned long long need, unsigned rows,
                        unsigned a, unsigned px, unsigned n)
{
    char b[288];
    if (n > 16 && (n & 1023)) return;        /* a writer's first 16, then every 1024th */
    _snprintf(b, sizeof b, "enginefix: composite scratch %s for %s: %u px in %u rows asked, "
              "%u held, %u now (%u so far)", what, SCR_WHO[who],
              need > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)need, rows, a, px, n);
    b[sizeof b - 1] = 0;
    plog(b);
}

static unsigned char* scratch_alloc(size_t bytes)
{
    CRITICAL_SECTION* lock = ((CRITICAL_SECTION* (__cdecl*)(void))0x004DA780)();
    unsigned char* p = NULL;
    EnterCriticalSection(lock);
    if (!((char (__cdecl*)(void))0x004D80D0)()) {
        p = ((unsigned char* (__cdecl*)(size_t))0x004E8890)(bytes);
        if (p) ((void (__cdecl*)(size_t))0x004DA7D0)(bytes);
    }
    LeaveCriticalSection(lock);
    return p;
}

/* Make the frame at ctx+0x10 (f, area a) hold `need` pixels. 1: it does, and the writer runs;
   0: it does not, and the writer takes its fallback on f, which is unchanged. */
static int scratch_hold(unsigned char* ctx, unsigned char* f, unsigned a,
                        unsigned long long need, unsigned rows, unsigned rows_max,
                        const void* src, int who)
{
    unsigned char* g;
    unsigned px, keep;
    const char* why;

    if (rows > rows_max)         why = rows_max == SCR_ROWS_SPAN
                                       ? "refused (taller than 800 rows, the span table of 0x4C8760 / 0x4C8BB0)"
                                       : "refused (taller than 2048 rows, the span table of 0x4C0820 / 0x4C1000)";
    else if (need <= a && (!s_scr_stress || src == f)) return 1;
    else if (src == f)           why = "refused (the frame is the writer's own source)";
    else if (need > SCR_CAP_PX)  why = "refused (over 2048 x 2048)";
    else if (s_scr_nogrow)       why = "refused (tagpu_scratch.nogrow)";
    else {
        if (s_scr_stress)
            px = need > SCR_MIN_PX ? (unsigned)need : SCR_MIN_PX;
        else {
            px = ((unsigned)need + SCR_ROUND_PX - 1) & ~(SCR_ROUND_PX - 1);
            if (px > SCR_CAP_PX) px = SCR_CAP_PX;
        }
        g = scratch_alloc(0x18u + 2u * (size_t)px);
        if (g) {
            keep = a < px ? a : px;
            memcpy(g, f, 0x10);
            memcpy(g + 0x18, f + 0x18, keep);
            memcpy(g + 0x18 + px, f + 0x18 + a, keep);
            *(unsigned char**)(g + 0x10) = g + 0x18;
            *(unsigned char**)(g + 0x14) = g + 0x18 + px;
            *(unsigned char**)(ctx + 0x10) = g;
            if (s_scr_stress) {                /* a reader still holding f faults here */
                *(unsigned char**)(f + 0x10) = SCR_POISON;
                *(unsigned char**)(f + 0x14) = SCR_POISON;
            }
            ((void (__cdecl*)(void*))0x004D85A0)(f);
            scratch_log("grown", who, need, rows, a, px, ++s_scr_grows[who]);
            return 1;
        }
        why = "refused (the allocator has no block)";
    }
    scratch_log(why, who, need, rows, a, a, ++s_scr_refusals[who]);
    return 0;
}

/* the frame at ctx+0x10 and its area; 0 leaves the write to stock (see THE INVARIANT) */
static unsigned char* scratch_frame(unsigned char* ctx, unsigned* a)
{
    unsigned char* f = ptr_sane(ctx) ? *(unsigned char**)(ctx + 0x10) : NULL;
    *a = scratch_area(f);
    if (!*a && s_scr_layout++ == 0)
        plog("enginefix: composite scratch at ctx+0x10 is not in 0x4B8E00's layout; "
             "its writers are left to stock");
    return f;
}

/* a 1 x 1 frame of one key pixel: every fallback that must still hand a frame on */
static void scratch_one_pixel(unsigned char* f, short hx, short hy, unsigned char key)
{
    unsigned char* colour = *(unsigned char**)(f + 0x10);
    unsigned char* depth  = *(unsigned char**)(f + 0x14);
    *(unsigned short*)(f + 0) = 1;
    *(unsigned short*)(f + 2) = 1;
    *(short*)(f + 4) = hx;
    *(short*)(f + 6) = hy;
    f[8] = key;
    colour[0] = key;
    depth[0] = 0;
}

/* 0x458B87: esi x edx is the header about to be written; ebx is ctx, ebp the source frame */
static int __cdecl scratch_build_state(unsigned int* regs)
{
    unsigned char* ctx = (unsigned char*)(size_t)regs[SCR_EBX];
    int w = (int)regs[SCR_ESI], h = (int)regs[SCR_EDX];
    unsigned a;
    unsigned char* f = scratch_frame(ctx, &a);
    if (!a) return 1;
    if (w >= 0 && h >= 0 && w <= 0xFFFF && h <= 0xFFFF &&
        scratch_hold(ctx, f, a, (unsigned long long)w * (unsigned)h, (unsigned)h,
                     SCR_ROWS_SPAN,                /* 0x459641's 1x bake: 0x4C8760 */
                     (const void*)(size_t)regs[SCR_EBP], SCR_BUILD))
        return 1;
    s_scr_refused = 1;
    return 0;
}

/* 0x45A470, the entry: ecx is ctx, [esp+4] the source frame */
static int __cdecl scratch_frame_copy(unsigned int* regs)
{
    unsigned char* ctx = (unsigned char*)(size_t)regs[SCR_ECX];
    const unsigned char* src = (const unsigned char*)(size_t)regs[SCR_SITE + 1];
    unsigned a;
    unsigned char* f = scratch_frame(ctx, &a);
    if (!a || !ptr_sane(src)) return 1;
    if (scratch_hold(ctx, f, a, (unsigned long long)*(const unsigned short*)src *
                     *(const unsigned short*)(src + 2), *(const unsigned short*)(src + 2),
                     0xFFFFu, src, SCR_FRAME))              /* nothing rasterizes into it */
        return 1;
    scratch_one_pixel(f, *(const short*)(src + 4), *(const short*)(src + 6), src[8]);
    return 0;
}

/* 0x45A7B9: ebp is ctx; the bounds 0x45A510 left are w at [esp+0x10] and h at [esp+0x20] */
static int __cdecl scratch_shadow(unsigned int* regs)
{
    unsigned char* ctx = (unsigned char*)(size_t)regs[SCR_EBP];
    int w = (int)regs[SCR_SITE + 0x10 / 4], h = (int)regs[SCR_SITE + 0x20 / 4];
    unsigned a;
    unsigned char* f = scratch_frame(ctx, &a);
    if (!a) return 1;
    if (w >= 0 && h >= 0 && w <= 0xFFFF && h <= 0xFFFF &&
        scratch_hold(ctx, f, a, 2ull * (unsigned)h * ((unsigned)w + 1u), (unsigned)h,
                     SCR_ROWS_POLY, NULL, SCR_SHADOW))
        return 1;
    scratch_one_pixel(f, 0, 0, f[8]);
    return 0;
}

/* 0x459875 / 0x459CB5, on the 2x path: ecx is ctx, the source frame at [esp+slot] */
static int scratch_bake(unsigned int* regs, unsigned slot, int who)
{
    unsigned char* ctx = (unsigned char*)(size_t)regs[SCR_ECX];
    const unsigned char* src =
        *(const unsigned char* const*)((const unsigned char*)(regs + SCR_SITE) + slot);
    unsigned a, w, h;
    unsigned char* f = scratch_frame(ctx, &a);
    if (!a || !ptr_sane(src)) return 1;
    w = *(const unsigned short*)src;
    h = *(const unsigned short*)(src + 2);
    return w < 0x8000 && h < 0x8000 &&         /* the doubled header is a u16 as well */
           scratch_hold(ctx, f, a, 4ull * w * h, 2u * h, SCR_ROWS_SPAN, src, who);
}

static int __cdecl scratch_bake1(unsigned int* regs) { return scratch_bake(regs, 0x5F18, SCR_BAKE1); }
static int __cdecl scratch_bake2(unsigned int* regs) { return scratch_bake(regs, 0x159E8, SCR_BAKE2); }

/* the call at 0x4596D8: the cargo merge, run only when it writes inside dst's header box */
static void __stdcall scratch_merge(const unsigned char* src, unsigned char* dst, int sx, int sy,
                                    int dbias)
{
    unsigned a = scratch_area(dst);
    if (a && ptr_sane(src)) {
        const unsigned sw = *(const unsigned short*)src, sh = *(const unsigned short*)(src + 2);
        const unsigned dw = *(const unsigned short*)dst, dh = *(const unsigned short*)(dst + 2);
        const int x0 = *(const short*)(dst + 4) - *(const short*)(src + 4) + sx;
        const int y0 = *(const short*)(dst + 6) - *(const short*)(src + 6) + sy;
        /* a negative origin is 0x4B90A0's own refusal and writes nothing */
        if (x0 >= 0 && y0 >= 0 &&
            ((unsigned long long)dw * dh > a ||
             (unsigned)x0 + sw > dw || (unsigned)y0 + sh > dh)) {
            unsigned n = ++s_scr_merge_refusals;
            if (n <= 16 || !(n & 1023)) {
                char b[224];
                _snprintf(b, sizeof b, "enginefix: composite scratch merge refused: a %ux%u cargo "
                          "at (%d,%d) is past the %ux%u frame (%u held) (%u so far)",
                          sw, sh, x0, y0, dw, dh, a, n);
                b[sizeof b - 1] = 0;
                plog(b);
            }
            return;
        }
    }
    ((void (__stdcall*)(const void*, void*, int, int, int))0x004B90A0)(src, dst, sx, sy, dbias);
}

/* pushad; push esp; call check; add esp,4; test eax,eax; popad -- ZF set = refused */
static unsigned char* scratch_call_check(unsigned char* p, int (__cdecl *check)(unsigned int*))
{
    *p++ = 0x60;                                            /* pushad           */
    *p++ = 0x54;                                            /* push esp         */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)check); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                  /* add esp,4        */
    *p++ = 0x85; *p++ = 0xC0;                               /* test eax,eax     */
    *p++ = 0x61;                                            /* popad            */
    return p;
}

/* the bakes' 2x test, stolen: `test reg,reg; je one_x`; the check runs on the 2x path only */
static unsigned char* scratch_bake_stub(unsigned char* s, unsigned char test_rr,
                                        int (__cdecl *check)(unsigned int*),
                                        unsigned int two_x, unsigned int one_x)
{
    unsigned char* p = s;
    unsigned char* jz0;
    *p++ = 0x85; *p++ = test_rr;                            /* test reg,reg     */
    *p++ = 0x74; jz0 = p++;                                 /* jz one_x         */
    p = scratch_call_check(p, check);
    *p++ = 0x74; *p++ = 0x05;                               /* jz one_x         */
    *p++ = 0xE9; tagpu_detour_rel(p, two_x); p += 4;        /* jmp: the 2x path */
    *jz0 = (unsigned char)(p - (jz0 + 1));
    *p++ = 0xE9; tagpu_detour_rel(p, one_x); p += 4;        /* one_x            */
    return p;
}

typedef struct SCRSITE { unsigned int va; unsigned char was[8]; int n; unsigned char* stub; } SCRSITE;

/* CLASS: local. The frame is drawn state; stock writes past it. */
static int fix_composite_scratch(void)
{
    SCRSITE site[7] = {
        { 0x00459608, { 0xE8, 0xB3, 0xF3, 0xFF, 0xFF }, 5, NULL },          /* call 0x4589C0 */
        { 0x00458B87, { 0x8B, 0x4B, 0x10, 0xF7, 0xD8 }, 5, NULL },          /* mov ecx,[ebx+0x10]; neg eax */
        { 0x0045A470, { 0x8B, 0x44, 0x24, 0x04, 0x53 }, 5, NULL },          /* mov eax,[esp+4]; push ebx */
        { 0x0045A7B9, { 0x8B, 0x4D, 0x10, 0x66, 0x8B, 0x54, 0x24, 0x10 }, 8, NULL },
        { 0x00459875, { 0x85, 0xDB, 0x0F, 0x84, 0x96, 0x00, 0x00, 0x00 }, 8, NULL }, /* test ebx,ebx; je 0x459913 */
        { 0x00459CB5, { 0x85, 0xC0, 0x0F, 0x84, 0x9A, 0x00, 0x00, 0x00 }, 8, NULL }, /* test eax,eax; je 0x459D57 */
        { 0x004596D8, { 0xE8, 0xC3, 0xF9, 0x05, 0x00 }, 5, NULL },          /* call 0x4B90A0 */
    };
    const int n = (int)(sizeof site / sizeof site[0]);
    unsigned char* p;
    int i, done;

    for (i = 0; i < n; i++)
        if (memcmp((const void*)(size_t)site[i].va, site[i].was, (size_t)site[i].n) != 0)
            return FIX_BYTES;
    for (i = 0; i < n; i++)
        if (!(site[i].stub = tagpu_detour_stub())) {
            while (i-- > 0) VirtualFree(site[i].stub, 0, MEM_RELEASE);
            return FIX_STUB;
        }
    s_scr_stress = GetFileAttributesA("tagpu_scratch.stress") != INVALID_FILE_ATTRIBUTES;
    s_scr_nogrow = GetFileAttributesA("tagpu_scratch.nogrow") != INVALID_FILE_ATTRIBUTES;

    /* the build-state copy's caller: run it, and on a refusal leave the unit draw */
    p = site[0].stub;
    p = tagpu_detour_set_flag(p, &s_scr_refused, 0);
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x08;     /* push [esp+8]     */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x08;     /* push [esp+8]     */
    *p++ = 0xE8; tagpu_detour_rel(p, 0x004589C0); p += 4;   /* call 0x4589C0    */
    p = tagpu_detour_cmp_flag(p, &s_scr_refused);
    *p++ = 0x75; *p++ = 0x03;                               /* jnz refused      */
    *p++ = 0xC2; *p++ = 0x08; *p++ = 0x00;                  /* ret 8            */
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x0C;                  /* refused: add esp,0xC (return, 2 args) */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x004597D8); p += 4;   /* jmp 0x4597D8     */

    p = scratch_call_check(site[1].stub, scratch_build_state);
    *p++ = 0x74; *p++ = 0x0A;                               /* jz refused       */
    memcpy(p, site[1].was, 5); p += 5;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00458B8C); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00458D13); p += 4;   /* refused: epilogue */

    p = scratch_call_check(site[2].stub, scratch_frame_copy);
    *p++ = 0x74; *p++ = 0x0A;                               /* jz refused       */
    memcpy(p, site[2].was, 5); p += 5;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0045A475); p += 4;
    *p++ = 0x8B; *p++ = 0x41; *p++ = 0x10;                  /* refused: mov eax,[ecx+0x10] */
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;                  /* ret 4            */

    p = scratch_call_check(site[3].stub, scratch_shadow);
    *p++ = 0x74; *p++ = 0x0D;                               /* jz refused       */
    memcpy(p, site[3].was, 8); p += 8;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0045A7C1); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0045A853); p += 4;   /* refused: the encode */

    scratch_bake_stub(site[4].stub, 0xDB, scratch_bake1, 0x0045987D, 0x00459913);
    scratch_bake_stub(site[5].stub, 0xC0, scratch_bake2, 0x00459CBD, 0x00459D57);

    p = site[6].stub;
    *p++ = 0xE9; tagpu_detour_rel(p, (unsigned int)(size_t)scratch_merge); p += 4;

    /* the caller's wrapper first: alone it only runs the call. Written together or not at all. */
    for (done = 0; done < n; done++) {
        unsigned char now[8];
        unsigned int rel = (unsigned int)(size_t)site[done].stub - (site[done].va + 5u);
        memset(now, 0x90, sizeof now);
        now[0] = site[done].was[0] == 0xE8 ? 0xE8 : 0xE9;   /* a call stays a call */
        memcpy(now + 1, &rel, 4);
        if (!tagpu_detour_write(site[done].va, now, site[done].n)) break;
    }
    if (done < n) {
        while (done-- > 0) tagpu_detour_write(site[done].va, site[done].was, site[done].n);
        for (i = 0; i < n; i++) VirtualFree(site[i].stub, 0, MEM_RELEASE);
        return FIX_PROTECT;
    }
    if (s_scr_stress || s_scr_nogrow)
        plog(s_scr_stress && s_scr_nogrow
             ? "enginefix: composite scratch levers: tagpu_scratch.stress + tagpu_scratch.nogrow "
               "(every writer takes its fallback)"
             : s_scr_stress ? "enginefix: composite scratch lever: tagpu_scratch.stress "
                              "(every writer call regrows the frame)"
                            : "enginefix: composite scratch lever: tagpu_scratch.nogrow "
                              "(an oversized writer takes its fallback)");
    return FIX_ARMED;
}

/* ===== THE BUILD LIST, THE DOWNLOAD-MENU RECORDS, THE OUT-OF-MEMORY TEXT ==================
   Three stock defects a large mod reaches, fixed in both builds (research/notes/tadr-port/
   content-ids.md, "What rides in the same landing"). Their sites are written all together or
   not at all, per fix, and their stubs come from fix_code with every other fix's. */

#define ENG_ALLOC(name, size) (((void* (__cdecl*)(const char*, unsigned int))0x004D83B0)((name), (size)))
#define ENG_FREE(p)           (((void (__cdecl*)(void*))0x004D85A0)(p))

/* The engine's allocator with its out-of-memory exit made unconditional [DISASSEMBLED].
   0x4D83C0 calls the new handler at [0x5289BC] when malloc fails (0x4D8409..0x4D8412), inside
   the allocator's critical section, and returns NULL when the slot is empty. The slot is
   process-global and its setter 0x4D8E50 is called around some of the engine's own
   allocations -- 0x495ABE until 0x49E6F0 (from 0x495AFD) puts 0x49E700 back, 0x4B3B75 until
   0x4B3B8F, 0x4B4146 until 0x4B422B -- so a failure on another thread inside such a window
   returns NULL. On NULL this calls the installed handler 0x49E700 itself (through the
   out-of-memory text's stub when that fix is armed), which ends the process and never returns;
   TerminateProcess stands behind it, so nothing after a failed allocation ever runs. */
static void* eng_alloc_or_exit(const char* name, unsigned int size)
{
    void* p = ENG_ALLOC(name, size);
    if (!p) {
        ((void (__cdecl*)(void))0x0049E700)();
        TerminateProcess(GetCurrentProcess(), 3);
    }
    return p;
}

/* pushad's registers, as a stub hands them to C; PR_RET is the return address when the site
   is a call, so the site's own esp is regs + PR_RET + 1 */
enum { PR_EDI, PR_ESI, PR_EBP, PR_ESP, PR_EBX, PR_EDX, PR_ECX, PR_EAX, PR_RET };

typedef struct FIXSITE { unsigned int va; int n; unsigned char was[20]; unsigned char now[20]; } FIXSITE;

/* The fixes' stubs, each contiguous inside one page. A stub that does not fit the rest of the
   current page opens another, so no fix fails for want of room; every page is kept for the
   process, since the patched sites jump into them. Two installers take stubs: the engine fixes
   (patch_engine_defects) and then, in the raised build, the limits' weapon sites (lim_sites ->
   wpn_build); tagpu_limits_install reports the total once both have. */
static unsigned char* s_fixCode;              /* the page stubs are placed in now             */
static unsigned int   s_fixCodeUsed;          /* its bytes taken                              */
static unsigned int   s_fixPages;             /* pages made                                   */
static unsigned int   s_fixBytes;             /* bytes taken over every page                  */

static unsigned char* fix_code(unsigned int n)
{
    unsigned int take = (n + 15u) & ~15u;
    unsigned char* p;
    if (!n || take > 0x1000u) return NULL;
    if (!s_fixCode || s_fixCodeUsed + n > 0x1000u) {
        unsigned char* page = (unsigned char*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                                           PAGE_EXECUTE_READWRITE);
        if (!page) return NULL;
        s_fixCode = page;
        s_fixCodeUsed = 0;
        s_fixPages++;
    }
    p = s_fixCode + s_fixCodeUsed;
    s_fixCodeUsed += take;
    s_fixBytes += take;
    return p;
}

static int fix_match(const FIXSITE* s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (memcmp((const void*)(size_t)s[i].va, s[i].was, (size_t)s[i].n) != 0) return 0;
    return 1;
}

static int fix_write(const FIXSITE* s, int n)
{
    int done;
    for (done = 0; done < n; done++)
        if (!tagpu_detour_write(s[done].va, s[done].now, s[done].n)) break;
    if (done == n) return FIX_ARMED;
    while (done-- > 0) tagpu_detour_write(s[done].va, s[done].was, s[done].n);
    return FIX_PROTECT;
}

/* E8/E9 to `target` at the site, NOP-padded */
static void fix_branch(FIXSITE* s, unsigned char op, const void* target)
{
    unsigned int rel = (unsigned int)(size_t)target - (s->va + 5u);
    memset(s->now, 0x90, (size_t)s->n);
    s->now[0] = op;
    memcpy(s->now + 1, &rel, 4);
}

/* a simulation fix's sites, into the fail-closed table rather than written here */
static int fix_table(const FIXSITE* s, int n, const char* name)
{
    int i;
    for (i = 0; i < n; i++) lim_add(s[i].va, s[i].n, s[i].was, s[i].now, name);
    return FIX_TABLE;
}

/* pushad; push esp; call fn; add esp,4; popad */
static unsigned char* fix_call_regs(unsigned char* p, void (__cdecl *fn)(unsigned int*))
{
    *p++ = 0x60;
    *p++ = 0x54;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)fn); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x61;
    return p;
}

/* A BUILDER'S LIST OF WHAT IT MAY BUILD: the `canbuild%d` keys of sidedata.tdf's [CANBUILD]
   section [DISASSEMBLED]. The game load reads each builder's keys into one shared heap block,
   `TEMP UTYPE LIST` (0x42D971, 30 u16 type IDs), appending at 0x42DA58 until a key is missing
   (0x42DA46..0x42DA99), and gives the builder its own copy at def+0x156: a fresh 0x3C bytes
   filled by a `rep movs` of 15 dwords (0x42DAC7..0x42DAE1), with the real count at def+0x152.
   Later the download menus' appender 0x42BE30 adds the builder's download entries to its copy
   while the count is at most 30 (0x42BEAF), so it writes entry 30, two bytes past the block.
   The AI's pick 0x40BDB0 and its debug listing loop to the count. So a builder with more than 30
   entries writes past the shared block, and every reader that loops to the count reads past its
   copy.

   THE FIX keeps the whole list [DECIDED 2026-09-24]. A block holding n entries has room for
   bl_room(n): stock's 30, then powers of two from 64. The shared append, the copy and the
   download appender each grow a block to bl_room(n + 1) before writing entry n when that is
   more than bl_room(n), through eng_alloc_or_exit (the engine's allocator 0x4D83B0, whose
   failure is the engine's out-of-memory exit whatever the handler slot holds) and its free (0x4D85A0, which the teardown 0x42DC52 frees the copy with).
   The copy is made at bl_room(count) entries, copied from the shared block.

   THE INVARIANT: every block holds at least bl_room(its count) entries. The shared block starts
   at 30 with a count of 0, and its count goes back to 0 for each builder (0x42D91D, 0x42DAA5),
   where bl_room is 30; each append keeps it; the copy is made at bl_room(count) from a shared
   block holding at least that many; nothing else writes either (the def clone 0x42B370 runs
   before 0x42D9BB zeroes the pair, and no reader writes). So no entry is written past a block
   and the copy reads inside the shared one. A builder with 30 entries or fewer, every stock
   builder, gets stock's exact blocks. The load runs on one thread, before any reader exists. */
static unsigned int bl_room(unsigned int n)
{
    unsigned int r = 64;
    if (n <= 30) return 30;
    if (n > (1u << 28)) return 1u << 30;      /* its allocation fails: the engine's exit */
    while (r < n) r <<= 1;
    return r;
}

/* `*list` holds n entries: make room for entry n, then write it */
static void bl_append(unsigned short** list, unsigned int n, unsigned short type, const char* name)
{
    if (bl_room(n + 1u) > bl_room(n)) {
        unsigned short* bigger = (unsigned short*)eng_alloc_or_exit(name, bl_room(n + 1u) * 2u);
        memcpy(bigger, *list, (size_t)n * 2u);
        ENG_FREE(*list);
        *list = bigger;
    }
    (*list)[n] = type;
}

/* 0x42DA58, the shared append, in place of `mov [ebp],ax; inc ebx; add ebp,2`: ax the type,
   ebx the count, ebp the cursor, and the block at the frame's [esp+0x14], which 0x42DA9B
   reloads ebp from and 0x42DB06 frees */
static void __cdecl bl_shared_append(unsigned int* regs)
{
    unsigned short** list = (unsigned short**)((unsigned char*)(regs + PR_RET + 1) + 0x14);
    unsigned int n = regs[PR_EBX];
    bl_append(list, n, (unsigned short)regs[PR_EAX], (const char*)0x00503EF0);
    regs[PR_EBX] = n + 1u;
    regs[PR_EBP] = (unsigned int)(size_t)(*list + n + 1u);
}

/* 0x42DAC7, the builder's copy, in place of `push 0x3C; push eax; call 0x4D83B0` and the
   `rep movs` after it: esi the def, eax the copy's name, ebp the shared block */
static void __cdecl bl_copy(unsigned int* regs)
{
    unsigned char* def = (unsigned char*)(size_t)regs[PR_ESI];
    const unsigned short* shared = (const unsigned short*)(size_t)regs[PR_EBP];
    unsigned int room = bl_room(*(const unsigned int*)(def + 0x152));
    unsigned short* copy = (unsigned short*)eng_alloc_or_exit((const char*)(size_t)regs[PR_EAX], room * 2u);
    memcpy(copy, shared, (size_t)room * 2u);
    *(unsigned short**)(def + 0x156) = copy;
    regs[PR_EAX] = (unsigned int)(size_t)copy;              /* what the `rep movs` leaves */
    regs[PR_ECX] = 0;
    regs[PR_ESI] = (unsigned int)(size_t)(shared + room);
    regs[PR_EDI] = (unsigned int)(size_t)(copy + room);
}

/* 0x42BEC3, the download entries' append to a builder's copy: ebp is def+0x152 ({count,
   list}), ax the type; eax, ecx and edx as stock's own append leaves them */
static void __cdecl bl_download_append(unsigned int* regs)
{
    unsigned int* hdr = (unsigned int*)(size_t)regs[PR_EBP];
    unsigned int n = hdr[0];
    unsigned short* list = (unsigned short*)(size_t)hdr[1];
    bl_append(&list, n, (unsigned short)regs[PR_EAX], "CANBUILD");
    hdr[1] = (unsigned int)(size_t)list;
    hdr[0] = n + 1u;
    regs[PR_EAX] = n + 1u;
    regs[PR_ECX] = n;
    regs[PR_EDX] = (unsigned int)(size_t)list;
}

/* CLASS: simulation, fail closed. A builder's list is what the AI's pick 0x40BDB0 and the
   build menu offer, so a player without it builds by a different list. */
static int fix_build_list(void)
{
    FIXSITE site[4] = {
        { 0x0042DA58, 8, { 0x66, 0x89, 0x45, 0x00, 0x43, 0x83, 0xC5, 0x02 }, { 0 } },
        { 0x0042DAC7, 8, { 0x6A, 0x3C, 0x50, 0xE8, 0xE1, 0xA8, 0x0A, 0x00 }, { 0 } },
        { 0x0042BEAF, 6, { 0x83, 0x7D, 0x00, 0x1E, 0x7F, 0x25 }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },
        { 0x0042BEC3, 17, { 0x8B, 0x4D, 0x00, 0x8B, 0x55, 0x04, 0x66, 0x89, 0x04, 0x4A,
                            0x8B, 0x45, 0x00, 0x40, 0x89, 0x45, 0x00 }, { 0 } },
    };
    unsigned char *a, *b, *c, *p;
    if (!(a = fix_code(16)) || !(b = fix_code(16)) || !(c = fix_code(16))) {
        lim_no_stub();
        return FIX_TABLE;
    }
    p = fix_call_regs(a, bl_shared_append); *p = 0xC3;
    p = fix_call_regs(b, bl_copy); *p = 0xC3;
    p = fix_call_regs(c, bl_download_append); *p = 0xC3;
    fix_branch(&site[0], 0xE8, a);
    fix_branch(&site[1], 0xE8, b);
    site[1].now[5] = 0xEB; site[1].now[6] = 0x15;          /* jmp 0x42DAE3, past the rep movs */
    fix_branch(&site[3], 0xE8, c);
    return fix_table(site, 4, "whole build lists");
}

/* THE DOWNLOAD MENUS' RECORDS [DISASSEMBLED]. 0x42DCF0 (loader thread) makes one 0xBD-byte
   record per download\*.tdf file: a dword entry count and five 0x25-byte entries ({u16 builder
   type, u8 MENU, u8 BUTTON, char unit[32]}, entry k at +4 + k*0x25). The block, files * 0xBD
   bytes from 0x4D83B0 (0x42DD74), is at main+0x391CB and the record count at main+0x391C7. The
   section loop 0x42DDD5..0x42DF0C takes a file's sections in order, writes the count k + 1 for
   section k (0x42DE12) and fills entry k, with no cap: a file with six or more sections writes
   past its record, and the last file's past the block. The readers walk the records to
   main+0x391C7 -- the build menu 0x41AE0F, the downloadable check 0x42E04B and the build-list
   appender 0x42BE30 -- except the page count 0x42DF60, inside 0x42DCF0, which walks its local
   file count at [esp+0x10].

   THE FIX continues a file into as many records as it needs, at the end of the block
   [DECIDED 2026-09-24]: section k of a file is entry k % 5 of the file's (k / 5)th record. A
   record is a self-contained list of entries, each naming its builder, so every reader reads a
   continued file as it reads five-entry ones.
   - 0x42DD74: the block comes from dl_alloc, which zeroes it and notes its room.
   - 0x42DDF0, each section: dl_section starts a record at section 5, 10, ... -- the block grown
     when full, the new record at index main+0x391C7, which it counts -- with esi at it and edi
     at its first entry, and writes the record's count, k % 5 + 1. Stock's own count write,
     k + 1 at 0x42DE12, is NOPped.
   - 0x42DF23: the next file's record is `imul esi,edi,0xBD` with edi the file index, where
     stock adds 0xBD to an esi that may now be at a continuation.
   - 0x42DF35: after the last file, ebx and the page count's [esp+0x10] are the record count.
   - 0x42E0B9: the downloadable check walks the files' own records only. It flags the unit of a
     record's first entry (+0x241 |= 0x20, "Somebody forgot to set downloadable=1"), which the
     AI's pick skips in one mode (0x40BBA0, when 0x435100 answers 1): a file's first unit, since
     Cavedog's files name one unit for all their builders. Across a continuation it would flag
     the 6th, 11th, ... as well. 0x42BD40 is a copy of the check with no caller.
   THE INVARIANT: records [0, files) are the files' own, at file * 0xBD, and every record past
   them is made by dl_section inside the block's room, so each entry written is inside the
   block; the check's bound, the file count, is at most the record count. Stock's files, four
   entries at most, get stock's exact records. Zeroing the block
   changes one thing stock left to the heap: a section whose unit or builder is not found is
   counted but not filled (0x42DE7F, 0x42DF07), and its entry reads builder 0, None, which no
   reader matches, where stock's held whatever the heap did; so does the count of a file with no
   sections or one that did not open. */
static unsigned int s_dlRoom;                 /* records the block has room for, LOADER THREAD */
static unsigned int s_dlFiles;                /* the files' own records, 0x42DCF0's file count  */

/* 0x42DD74 asks for files * 0xBD bytes */
static void* __cdecl dl_alloc(const char* name, unsigned int size)
{
    void* p = eng_alloc_or_exit(name, size);
    memset(p, 0, size);
    s_dlRoom = s_dlFiles = size / 0xBDu;
    return p;
}

/* 0x42DDF0: ebx is the section index k, esi this file's record offset, edi the entry offset */
static void __cdecl dl_section(unsigned int* regs)
{
    char* ta = *(char**)0x00511DE8;
    unsigned int k = regs[PR_EBX];
    unsigned char* block;
    if (k && k % 5u == 0) {
        unsigned int recs = *(unsigned int*)(ta + 0x391C7);
        if (recs >= s_dlRoom) {
            unsigned int room = s_dlRoom * 2u > recs + 1u ? s_dlRoom * 2u : recs + 1u;
            unsigned int bytes = room > 0x00AAAAAAu ? 0x7FFFFFFFu : room * 0xBDu;  /* fails: the exit */
            unsigned char* bigger = (unsigned char*)eng_alloc_or_exit((const char*)0x00503F7C, bytes);
            unsigned char* old = *(unsigned char**)(ta + 0x391CB);
            memcpy(bigger, old, (size_t)recs * 0xBDu);
            memset(bigger + (size_t)recs * 0xBDu, 0, bytes - recs * 0xBDu);
            ENG_FREE(old);
            *(unsigned char**)(ta + 0x391CB) = bigger;
            s_dlRoom = room;
        }
        *(unsigned int*)(ta + 0x391C7) = recs + 1u;
        regs[PR_ESI] = recs * 0xBDu;
        regs[PR_EDI] = 0;
    }
    block = *(unsigned char**)(ta + 0x391CB);
    *(unsigned int*)(block + regs[PR_ESI]) = k % 5u + 1u;
}

/* CLASS: simulation, fail closed. The records feed the builders' lists (0x42BE30) and the
   build menus, as the build lists above. */
static int fix_download_records(void)
{
    FIXSITE site[6] = {
        { 0x0042DD74, 5, { 0xE8, 0x37, 0xA6, 0x0A, 0x00 }, { 0 } },
        { 0x0042DDF0, 8, { 0xA1, 0xE8, 0x1D, 0x51, 0x00, 0x8D, 0x6B, 0x01 }, { 0 } },
        { 0x0042DE12, 3, { 0x89, 0x2C, 0x31 }, { 0x90, 0x90, 0x90 } },
        { 0x0042DF23, 6, { 0x81, 0xC6, 0xBD, 0x00, 0x00, 0x00 }, { 0x69, 0xF7, 0xBD, 0x00, 0x00, 0x00 } },
        { 0x0042DF35, 6, { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 }, { 0 } },
        { 0x0042E0B9, 6, { 0x3B, 0xA8, 0xC7, 0x91, 0x03, 0x00 }, { 0x3B, 0x2D } },  /* cmp ebp,[files] */
    };
    const unsigned int files = (unsigned int)(size_t)&s_dlFiles;
    static const unsigned char pages[16] = {
        0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00,     /* mov edx,[main]          */
        0x8B, 0x9A, 0xC7, 0x91, 0x03, 0x00,     /* mov ebx,[edx+0x391C7]   */
        0x89, 0x5C, 0x24, 0x14,                 /* mov [esp+0x14],ebx      */
    };
    unsigned char *a, *b, *p;
    if (!(a = fix_code(24)) || !(b = fix_code(24))) {
        lim_no_stub();
        return FIX_TABLE;
    }
    memcpy(site[5].now + 2, &files, 4);
    p = fix_call_regs(a, dl_section);
    memcpy(p, site[1].was, 8); p += 8;                     /* mov eax,[main]; lea ebp,[ebx+1] */
    *p = 0xC3;
    memcpy(b, pages, sizeof pages); b[sizeof pages] = 0xC3;
    fix_branch(&site[0], 0xE8, (const void*)dl_alloc);
    fix_branch(&site[1], 0xE8, a);
    fix_branch(&site[4], 0xE8, b);
    return fix_table(site, 6, "download menus past five entries");
}

/* THE OUT-OF-MEMORY TEXT [DISASSEMBLED]. WinMain installs 0x49E700 as the allocator's new
   handler for the whole process (0x49E849). On any failed allocation it appends its text to
   ErrorLog.txt, dumps the registers and stack through a deliberate fault (0x49E7E4 -> 0x49E680),
   shows the text in a system-modal box (0x49E7FA) and leaves through raise(SIGABRT) (0x49E807):
   it never returns. The text, "Out of memory! Your hard disk may be full" at 0x509764, is read
   at 0x49E7BD (its length), 0x49E7CD (the log) and 0x49E7F4 (the box), and nowhere else. The fix
   points the three at ours, which a call at the handler's entry writes: the game is a 32-bit
   program that has used the memory it can address, and how many unit types are installed,
   which is what a mod's size costs [DECIDED 2026-09-24]. The count is UNITINFOCount - 1: the
   unit files found (0x42AA77) until the menu-time load's end rewrites it with the types it kept
   (0x42B2F6), and a game load's end with the types in play (0x42D542). The log, the dump and the exit stay
   the engine's. Nothing here allocates: the text is a static and wsprintfA writes it. */
static char s_oomText[320];

static void __cdecl oom_text(void)
{
    const char* ta = *(const char* const*)0x00511DE8;
    int types = ta ? *(const int*)(ta + 0x1438F) - 1 : 0;
    if (types > 0)
        wsprintfA(s_oomText, "Out of memory! Total Annihilation is a 32-bit program, and it has "
                  "used all the memory it can address. %d unit types are installed.", types);
    else
        wsprintfA(s_oomText, "Out of memory! Total Annihilation is a 32-bit program, and it has "
                  "used all the memory it can address.");
}

/* CLASS: local. A message. */
static int fix_oom_message(void)
{
    FIXSITE site[4] = {
        { 0x0049E700, 6, { 0x81, 0xEC, 0xEC, 0x03, 0x00, 0x00 }, { 0 } },
        { 0x0049E7BD, 4, { 0x64, 0x97, 0x50, 0x00 }, { 0 } },
        { 0x0049E7CD, 4, { 0x64, 0x97, 0x50, 0x00 }, { 0 } },
        { 0x0049E7F4, 4, { 0x64, 0x97, 0x50, 0x00 }, { 0 } },
    };
    const unsigned int text = (unsigned int)(size_t)s_oomText;
    unsigned char *a, *p;
    int i;
    if (!fix_match(site, 4)) return FIX_BYTES;
    if (!(a = fix_code(24))) return FIX_STUB;
    p = a;
    *p++ = 0x60;                                            /* pushad             */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)oom_text); p += 4;
    *p++ = 0x61;                                            /* popad              */
    memcpy(p, site[0].was, 6); p += 6;                      /* sub esp,0x3EC      */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049E706);
    oom_text();                                             /* the text before any failure */
    fix_branch(&site[0], 0xE9, a);
    for (i = 1; i < 4; i++) memcpy(site[i].now, &text, 4);
    return fix_write(site, 4);
}

/* THE UNIT SYNC'S KEYS [DISASSEMBLED]. A network game matches each unit type between the peers by
   a key, def+0x13E: 0x4B6BA0's checksum of the type's FBI file (0x42ABB3), or a units\*.OVR
   file's `Compatability` value (0x42AC43). The checksum is four 8-bit lanes -- the bytes' sum
   and xor, the sum of i^b and the xor of i+b -- not a CRC. The host keeps, for each joining peer,
   the keys it has received from it, dropping one it already holds (0x46D755), and the room waits
   until that list reaches the count the peer announced (0x46E000; the +syncerr command's reply,
   0x46DF40, reads "expected %d units, got %d"), so a joiner with two types of one key holds the
   battle room at SYNCHING for good;
   and the host's walk 0x46D9E3 answers a key with the first type that has it. FBIs that differ
   in a few digits collide in their thousands: MEASURED 2026-09-24, 16 105 generated types gave
   9 991 keys and a join that never ended.
   THE FIX re-keys, after the menu-time load (its one call, 0x42BD29) returns 1, all but one type
   of every group that shares a key. A load that returns 0 (a unit file with no [UNITINFO],
   0x42AC78) is left as stock leaves it, the array unsealed and part-filled.
   The load is the keys' only writer; the def clone 0x42B370 moves a key with its def.
   THE INVARIANT: after it no two defs share a key, and every key is a function of the loaded
   types' names and natural keys alone, never of the def array's order -- which is the order the
   files were found in (0x4BCA30), loose files and then the archives as the directory listed
   them, and can differ between two peers with the same files. So a group is taken in name order
   (the name at +0x20, the field the engine's own game-load sort compares, 0x42DB60): the first
   keeps its key, and each other type's key is a hash of its name, moved on past every value a
   type already holds. Two types with one name and one key cannot be told apart by the sync
   either. A type re-keyed on one peer and not on another has a different key there and is
   reported as not synced, which is what different content should get. Types that do not collide
   keep stock's keys, so stock content is unchanged: no two of the install's 278 unit names share
   one. GAME THREAD (the state callback 0x496BB0), before any lobby exists. The table holds every
   key a load can have, natural and given, at most half full, so a probe always ends.
   THE DEF ARRAY IS READ-ONLY HERE. When the engine's memory protection is on ([0x5289A0], set
   once through 0x4D9FE0 and the `gonzo` switch; on in a plain launch, MEASURED 2026-09-24: an
   unguarded write faulted), the loader's success exit seals the array (0x42B328 -> 0x4D8710,
   PAGE_READONLY over the block's whole pages), and a writer opens it with 0x4D8780 and seals it
   again (the value checksum 0x42A610: 0x42A63A, 0x42A829). This pass does the same around its
   writes. The sync itself: exe-reverse-engineering.md, "The unit sync's keys". */
#define ENG_WRITABLE(p) (((void (__cdecl*)(void*))0x004D8780)(p))
#define ENG_READONLY(p) (((void (__cdecl*)(void*))0x004D8710)(p))
#define DEF_STRIDE 0x249
#define DEF_KEY    0x13E
#define DEF_NAME   0x20               /* UnitName, 32 bytes */
#define KEYTAB (4 * TAGPU_LIM_TYPES)
typedef char keytab_is_pow2[(KEYTAB & (KEYTAB - 1)) == 0 ? 1 : -1];   /* key_slot's wrap */
static unsigned int   s_keyVal[KEYTAB];
static unsigned char  s_keyUse[KEYTAB];         /* 1 some type's key, 2 claimed by one type  */
static unsigned short s_keyOrder[TAGPU_LIM_TYPES];
static const char*    s_keyDefs;                /* key_order's array, GAME THREAD             */

static unsigned int key_slot(unsigned int key)
{
    unsigned int h = key * 0x9E3779B1u, i = (h ^ (h >> 16)) & (KEYTAB - 1u);
    while ((s_keyUse[i] & 1) && s_keyVal[i] != key) i = (i + 1u) & (KEYTAB - 1u);
    return i;
}

static unsigned int def_key(const char* defs, unsigned int i)
{
    return *(const unsigned int*)(defs + i * DEF_STRIDE + DEF_KEY);
}

/* by key, then by name; the index only orders two types the sync cannot tell apart */
static int __cdecl key_order(const void* a, const void* b)
{
    unsigned int ia = *(const unsigned short*)a, ib = *(const unsigned short*)b;
    unsigned int ka = def_key(s_keyDefs, ia), kb = def_key(s_keyDefs, ib);
    int c;
    if (ka != kb) return ka < kb ? -1 : 1;
    c = strncmp(s_keyDefs + ia * DEF_STRIDE + DEF_NAME, s_keyDefs + ib * DEF_STRIDE + DEF_NAME, 32);
    return c ? c : (ia > ib) - (ia < ib);
}

static unsigned int name_key(const char* name)
{
    unsigned int h = 2166136261u;
    int i;
    for (i = 0; i < 32 && name[i]; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
    return h;
}

static void __cdecl sync_keys_unique(void)
{
    char* ta = *(char**)0x00511DE8;
    char* defs = ta ? *(char**)(ta + 0x1439B) : NULL;
    int n = ta ? *(int*)(ta + 0x1438F) : 0, i, moved = 0;
    unsigned int keeper = 0;
    char b[200];
    if (!defs || n < 2) return;
    if (n - 1 > TAGPU_LIM_TYPES) {
        _snprintf(b, sizeof b, "enginefix: unit sync keys not checked: %d types, the table holds %d",
                  n - 1, TAGPU_LIM_TYPES);
        b[sizeof b - 1] = 0;
        plog(b);
        return;
    }
    memset(s_keyUse, 0, sizeof s_keyUse);
    for (i = 1; i < n; i++) {
        unsigned int k = def_key(defs, (unsigned int)i), s = key_slot(k);
        s_keyVal[s] = k;
        s_keyUse[s] |= 1;
        s_keyOrder[i - 1] = (unsigned short)i;
    }
    s_keyDefs = defs;
    qsort(s_keyOrder, (size_t)(n - 1), sizeof s_keyOrder[0], key_order);
    for (i = 0; i < n - 1; i++) {
        unsigned int d = s_keyOrder[i];
        unsigned int* key = (unsigned int*)(defs + d * DEF_STRIDE + DEF_KEY);
        const char* name = defs + d * DEF_STRIDE + DEF_NAME;
        unsigned int s = key_slot(*key), c = name_key(name);
        if (!(s_keyUse[s] & 2)) {
            s_keyUse[s] |= 2;
            keeper = d;
            continue;
        }
        while (!c || (s_keyUse[s = key_slot(c)] & 1)) c++;
        s_keyVal[s] = c;
        s_keyUse[s] = 3;
        if (moved < 8) {
            _snprintf(b, sizeof b, "enginefix: unit sync key 0x%08X of %.32s is also %.32s's; "
                      "its key is now 0x%08X", *key, name, defs + keeper * DEF_STRIDE + DEF_NAME, c);
            b[sizeof b - 1] = 0;
            plog(b);
        }
        if (!moved) ENG_WRITABLE(defs);
        *key = c;
        moved++;
    }
    if (moved) {
        ENG_READONLY(defs);
        _snprintf(b, sizeof b, "enginefix: unit sync keys: %d of %d types re-keyed", moved, n - 1);
        b[sizeof b - 1] = 0;
        plog(b);
    }
}

/* CLASS: simulation, fail closed. A player without it keeps colliding keys, which the
   host's walk 0x46D9E3 answers with the first type that has them. */
static int fix_sync_keys(void)
{
    FIXSITE site[1] = { { 0x0042BD29, 5, { 0xE8, 0xA2, 0xEB, 0xFF, 0xFF }, { 0 } } };  /* call 0x42A8D0 */
    unsigned char *a, *p;
    if (!(a = fix_code(16))) { lim_no_stub(); return FIX_TABLE; }
    p = a;
    *p++ = 0xE8; tagpu_detour_rel(p, 0x0042A8D0); p += 4;
    *p++ = 0x85; *p++ = 0xC0;                                  /* test eax,eax        */
    *p++ = 0x74; *p++ = 0x05;                                  /* jz ret              */
    *p++ = 0xE9; tagpu_detour_rel(p, (unsigned int)(size_t)sync_keys_unique); p += 4;
    *p = 0xC3;
    fix_branch(&site[0], 0xE8, a);
    return fix_table(site, 1, "unique unit sync keys");
}

/* ===== WEAPON IDS (TAGPU_LIM_WEAPONS) =======================================================
   The plan is research/notes/tadr-port/content-ids.md, "Weapons"; the disassembly is
   exe-reverse-engineering.md, "Weapon IDs", and limits-evidence.md §9.

   THE ARRAY. Weapons[256] is in the main block at main+0x2CF3, 0x115 bytes a record, and ends
   exactly at the projectile pool's header (main+0x141F3). A weapon's TDF `ID` is its slot: the
   loader 0x42E440 takes the address from it with no bound either way. The raised build moves
   the array to a DLL static of 4096 and points each reference at it. Every pointer the engine
   keeps to a weapon (unit defs, unit slots, projectiles, feature defs, the meteor's 0x512328)
   is then into the static, so a weapon's ID is (pointer - base) / 0x115, exact and bounded.

   THE ID BYTE, weapon+0x10A, is the load wipe's (0x42E310): the slot's own index as a byte,
   never the TDF's. Below 256 it stays that. From 256 up it is the index's low byte, or 0xFF
   where that is 0, because the AI's "armed" tests read it as != 0 (0x40954D, 0x409682,
   0x409940, 0x49E0C2). Nothing here takes it as an index: a saved game writes it back on load
   (0x487628), so under a changed mod it can be stale. The model path and the wire use the
   index taken from the pointer instead.

   THE WIRE, twelve bits of ID:
   - 0x0D, weapon fired, 36 bytes: bits 8..11 in the high nibble of the slot byte +0x23. Stock's
     unit senders write `and dl,3` there and the extra-weapons module bits 0..3.
   - 0x0F, feature hit, 6 bytes: bits 8..11 in bits 12..15 of the cell x. Stock's receiver reads
     the bytes 0xFD..0xFF as "feature destroyed / burned / reclaimed", so a weapon's hit whose
     byte is one of them is flagged by bit 11 of x and read as the hit. The three sentinels go
     out as stock sends them: with weapons below 253 every 0x0F is stock's, byte for byte, and a
     peer on another build reads it as stock does. A cell is a 16.16 position >> 20 on the map,
     so x < 0x800.
   - 0x0E, interceptor detonation, 14 bytes, has no room. IDs below 256 send stock's message;
     IDs from 256 up send a companion, a tagged 0x05 message: `05 00 49`, the target point, a
     u16 ID. Stock's receiver matches only projectiles of a weapon below 256 and the companion's
     only from 256 up, so neither can detonate the other's projectile.

   BOTH BUILDS carry the loader's bound (a weapon whose ID is outside the array is skipped and
   logged), the 0x0D receiver's bound on its shooter and target indexes (u16 * 0x118 into the
   unit array, unbounded in stock), the 0x0F hit flag, and the 0x0F receiver's refusal of
   a cell off the map (stock reads through 0x481550's NULL). The stock build installs them as
   one fix; the raised build installs them with the raise, all or nothing. */

#define WPN_REC        0x115
#define WPN_MAIN       0x2CF3             /* Weapons[0] in the main block                    */
#define WPN_BYTE       0x10A              /* the ID byte                                     */
#define WPN_FLAGS      0x111
#define WPN_MODEL      0x74               /* the 3DO, perhaps an earlier weapon's            */
#define WPN_MODELNAME  0x80               /* empty in a weapon that borrowed its model       */
#define WPN_CHAT_TAG   0x49               /* outside TADR's tags, 0x2B..0x31 and 0x60         */
#define WPN_HITFLAG    0x800              /* 0x0F: bit 11 of the cell x, a hit, not a sentinel */
#define WPN_MAXSITE    32

#define E_SEND(net, m, n)         (((void (__stdcall*)(unsigned int, const void*, unsigned int))0x00451DF0)((net), (m), (n)))
#define E_CELL(x, y)              (((char* (__stdcall*)(int, int))0x00481550)((x), (y)))
#define E_FEATURE_HIT(c, x, y, w) (((void (__stdcall*)(char*, int, int, char*))0x004244B0)((c), (x), (y), (w)))
#define E_FEATURE_DIE(x, y, k)    (((void (__stdcall*)(int, int, int))0x00423550)((x), (y), (k)))
#define E_FEATURE_BURN(x, y, k)   (((void (__stdcall*)(int, int, int))0x004233A0)((x), (y), (k)))
#define E_DETONATE(p)             (((void (__stdcall*)(char*, int))0x00499EB0)((p), 0))
#define E_STRCMP(a, b)            (((int (__cdecl*)(const char*, const char*))0x004F8A70)((a), (b)))

/* the site's esp: above the return address after a `call stub`, at it after a `jmp stub` */
#define WPN_ESP_CALL(r) ((unsigned char*)&(r)[PR_RET + 1])
#define WPN_ESP_JMP(r)  ((unsigned char*)&(r)[PR_RET])

#ifndef TAGPU_LIMITS_STOCK
/* The model path copies the TDF's model name, read into a 0x100-byte buffer (0x42EC7B), to a
   record's +0x80 with no bound (0x42ED7B), so a long name runs up to 0x180 - 0x115 bytes past
   the record, into the next one as in stock. The tail keeps the last record's overrun inside
   the static. The section name copied to +0 has no such bound in the engine; wpn_loader_id
   holds it to the record. */
static struct {
    unsigned char rec[TAGPU_LIM_WEAPONS][WPN_REC];
    unsigned char tail[0x80 + 0x100 - WPN_REC];
} s_wpn;
#define s_weapons s_wpn.rec
#endif
static DWORD         s_wpnGameTid;        /* DllMain's thread, which is the game loop's      */
static volatile LONG s_wpnNotes;

static void wpn_note_in(volatile LONG* budget, const char* what, unsigned int a, unsigned int b)
{
    if (InterlockedIncrement(budget) <= 32)
        tagpu_logf("enginefix: weapon IDs: %s (%u, %u)", what, a, b);
}
#define wpn_note(what, a, b) wpn_note_in(&s_wpnNotes, (what), (a), (b))

/* B3's diverged-0x0D drop counter, incremented in wpn_rx_fired below and reported by
   the wire-robustness block further down (its own enginefix line). It is defined here so the
   0x0D fix that owns wpn_rx_fired keeps counting it whichever build is compiled. The
   accepted count is the check run and passed on a live shooter: evidence the bound ran. */
static unsigned int s_wireWpnxDrops, s_wireIn0D;

/* In place of `mov edx,[main]` at 0x42E468, after the loader read `ID` into eax (-1 when the
   section has none) and the section's name into edi. edx is the base the loader's own
   arithmetic adds id * 0x115 + 0x2CF3 to. A weapon outside the array is skipped: the stub leaves
   through the loader's epilogue 0x42F333, past its closing call 0x49E010, and nothing of it is
   written. So is a weapon whose name does not fit its record: 0x42E490 copies the name to +0
   with no bound, and the TDF parser (0x4C4340) cuts a section name from the file's text, so
   nothing else bounds it. A second weapon with an ID keeps stock's rule, the later one wins. */
static int __cdecl wpn_loader_id(unsigned int* regs)
{
    const char* name = (const char*)(size_t)regs[PR_EDI];
    char* base = tagpu_limits_weapon0();
    int id = (int)regs[PR_EAX];
    if (!base || id < 0 || id >= TAGPU_LIM_WEAPONS) {
        tagpu_logf("enginefix: weapon %.32s has ID %d, outside 0..%d, and is skipped", name, id,
                   TAGPU_LIM_WEAPONS - 1);
        return 0;
    }
    if (strnlen(name, WPN_REC) >= WPN_REC) {
        tagpu_logf("enginefix: weapon %.32s... (ID %d) has a name longer than its record, and is "
                   "skipped", name, id);
        return 0;
    }
    if (base[id * WPN_REC])
        tagpu_logf("enginefix: weapon ID %d: %.32s replaces %.32s", id, name, base + id * WPN_REC);
    regs[PR_EDX] = (unsigned int)(size_t)(base - WPN_MAIN);
    /* the weapon keys (C2) of the section being loaded: ebx is its TDF
       context here, from 0x42E447 until 0x42E4AF */
    tagpu_datakeys_weapon_read(id, (void*)(size_t)regs[PR_EBX], name);
    return 1;
}

/* In place of the 0x0D receiver's lookup at 0x49D280 (eax the packet, edx its ID byte):
   ecx = main, ebp = the weapon, edx = its flags >> 5, then on at 0x49D2A6. A weapon without flag
   bit 5 fires from a unit, and the receiver scales the shooter (+0x21) and the target (+0x1F),
   both u16, by 0x118 into the unit array. Each is bounded here by the array's last element
   (main+0x1435B, inclusive: the engine's sweep 0x48BD00 steps at 0x48BD22 and runs `jbe` to
   it); a packet past it is dropped through the receiver's exit 0x49D55D. The slot byte keeps
   its low nibble for the read after this one: stock's 0x49D366 takes the shooter's slot at
   +4 + slot * 0x1C, three of them, so without the extra-weapons module a slot past 2 is dropped
   too; with it, the module's splice there bounds the slot by the unit's own count. */
static int __cdecl wpn_rx_fired(unsigned int* regs)
{
    unsigned char* pkt = (unsigned char*)(size_t)regs[PR_EAX];
    char* ta = *(char* const*)0x00511DE8;
    char* base = tagpu_limits_weapon0();
    unsigned int id = pkt[0x19], flags;
    char* w;
#ifndef TAGPU_LIMITS_STOCK
    id |= (unsigned int)(pkt[0x23] >> 4) << 8;
#endif
    pkt[0x23] &= 0x0F;
    if (!ta || !base || id >= TAGPU_LIM_WEAPONS) return 0;
    w = base + id * WPN_REC;
    memcpy(&flags, w + WPN_FLAGS, 4);
    if (!(flags & 0x20)) {
        const char* first = *(const char* const*)(ta + 0x14357);
        const char* last  = *(const char* const*)(ta + 0x1435B);
        unsigned int shooter = pkt[0x21] | (unsigned int)pkt[0x22] << 8;
        unsigned int target  = pkt[0x1F] | (unsigned int)pkt[0x20] << 8;
        unsigned int max;
        if (!first || last < first) return 0;
        max = (unsigned int)(last - first) / 0x118;
        if (shooter > max || target > max) {
            wpn_note("a weapon-fired message names a unit past the array; dropped",
                     shooter > target ? shooter : target, max);
            return 0;
        }
        if (pkt[0x23] > 2 && !tagpu_weapons_armed()) {
            wpn_note("a weapon-fired message names a slot past the unit's three; dropped",
                     pkt[0x23], shooter);
            return 0;
        }
        /* B3: the diverged-shooter drop (sim-fixes.md B3; evidence §8). Stock branches
           the projectile on the PACKET's weapon flags but divides by the LOCAL slot
           weapon's velocity +0x68 (0x49CE62..0x49CE6A), so on a peer whose copy of the
           shooter is a live unit of another type it faults #DE when that weapon's
           velocity is 0 and otherwise builds a mixed projectile. Resolve the shooter's
           own slot weapon (the extra-weapons accessor past slot 2, NULL when the slot is
           past the unit's count) and drop when it is not &Weapons[id]. A bound on engine
           + wire data, and the identity between consistent peers: the sender fills the
           packet from that same slot (0x49D742), so they never differ. Evidence §8
           classes it simulation, only when diverged; riding this site, it is held
           fail-closed with the weapon IDs' rows. Only for a live shooter, since a dead
           one the receiver drops anyway. */
        {
            char* shooter_u = (char*)first + shooter * 0x118;
            if ((*(const unsigned int*)(shooter_u + 0x110)) & 0x10000000) {
                char* sw = tagpu_weapons_slot_weapon(shooter_u, pkt[0x23]);
                if (sw != w) {
                    s_wireWpnxDrops++;
                    wpn_note("a weapon-fired message whose shooter slot holds another weapon; dropped",
                             id, pkt[0x23]);
                    return 0;
                }
                s_wireIn0D++;
            }
        }
    }
    regs[PR_ECX] = (unsigned int)(size_t)ta;
    regs[PR_EBP] = (unsigned int)(size_t)w;
    regs[PR_EDX] = flags >> 5;
    return 1;
}

/* In place of the 0x0F sender's `call 0x451BC0` at 0x424575: the packet is the send's third
   argument and the weapon is 0x4244B0's fourth, at the send's esp+0x38. The byte is the index
   from the weapon's pointer, not its ID byte, which a saved game can leave stale. The flag and
   the ID's high bits share x with the cell, so an x from 0x800 is not sent; none is reachable,
   since 0x4244B0's caller found the cell on the map. */
static int __cdecl wpn_tx_feature(unsigned int* regs)
{
    unsigned char* pkt = (unsigned char*)(size_t)regs[PR_RET + 3];
    unsigned int x = pkt[2] | (unsigned int)pkt[3] << 8;
    int i = tagpu_limits_weapon_index(*(const char* const*)(WPN_ESP_CALL(regs) + 0x38));
    if (x >= WPN_HITFLAG) { wpn_note("a feature hit at cell x >= 0x800 is not sent", x, pkt[1]); return 0; }
    if (i >= 0) {
        x |= (unsigned int)(i >> 8) << 12;
        if ((i & 0xFF) >= 0xFD) x |= WPN_HITFLAG;
        pkt[1] = (unsigned char)i;
        pkt[2] = (unsigned char)x;
        pkt[3] = (unsigned char)(x >> 8);
    }
    return 1;
}

/* The dispatch table's 0x0F slot (0x455FB8), in place of stock's handler 0x45544D; the stub
   goes on to the dispatcher's continuation 0x455F50. The message is the handler's [esp+0x10].
   The cell must be on the map (0x481550 bounds x and y), then: 0xFD..0xFF unflagged, one of
   stock's three sentinel calls; otherwise the weapon's hit, 0x4244B0, the flag set exactly when
   the byte is 0xFD..0xFF. */
static int __cdecl wpn_rx_feature(unsigned int* regs)
{
    const unsigned char* m = *(const unsigned char* const*)(WPN_ESP_JMP(regs) + 0x10);
    unsigned int b = m[1], x = m[2] | (unsigned int)m[3] << 8, y = m[4] | (unsigned int)m[5] << 8;
    unsigned int cx = x & (WPN_HITFLAG - 1), id = b;
    int hit = (x & WPN_HITFLAG) != 0;
    char* base = tagpu_limits_weapon0();
    char* cell;
    if (!base) return 0;
    cell = E_CELL((int)cx, (int)y);
    if (!cell) { wpn_note("a feature message names a cell off the map; dropped", cx, y); return 0; }
    if (b >= 0xFD && !hit) {
        if (x >> 12) { wpn_note("a feature sentinel's x has bits past the cell; dropped", x, b); return 0; }
        if (b == 0xFD)      E_FEATURE_DIE((int)cx, (int)y, 0);
        else if (b == 0xFE) E_FEATURE_BURN((int)cx, (int)y, 1);
        else                E_FEATURE_DIE((int)cx, (int)y, 1);
        return 0;
    }
    if (b < 0xFD && hit) { wpn_note("a feature hit is flagged but its byte is below 0xFD; dropped", b, x); return 0; }
#ifndef TAGPU_LIMITS_STOCK
    id |= (x >> 12) << 8;
#else
    if (x >> 12) { wpn_note("a feature message's x has bits past the cell; dropped", x, b); return 0; }
#endif
    if (id >= TAGPU_LIM_WEAPONS) return 0;
    E_FEATURE_HIT(cell, (int)cx, (int)y, base + id * WPN_REC);
    return 0;
}

#ifndef TAGPU_LIMITS_STOCK
/* In place of the load wipe's loop at 0x42E31C (every level load, 0x4918BB): each slot's name
   emptied and its ID byte set, then on at 0x42E345. */
static int __cdecl wpn_wipe(unsigned int* regs)
{
    int i;
    (void)regs;
    for (i = 0; i < TAGPU_LIM_WEAPONS; i++) {
        s_weapons[i][0] = 0;
        s_weapons[i][WPN_BYTE] = (unsigned char)(i < 256 ? i : ((i & 0xFF) ? (i & 0xFF) : 0xFF));
    }
    return 0;
}

/* In place of the model path's loop at 0x42EC99 (ebp the weapon, the model's name at the
   site's esp+0x40): a lower slot whose model has this name lends it, as 0x42F340 does. Stock's
   loop runs to the ID byte, which is the slot only below 256. Returns 1 when the model was
   borrowed; the stub then goes on at 0x42EDA1, and otherwise loads it at 0x42ECF9. */
static int __cdecl wpn_model_reuse(unsigned int* regs)
{
    char* w = (char*)(size_t)regs[PR_EBP];
    const char* name = (const char*)WPN_ESP_JMP(regs) + 0x40;
    int n = tagpu_limits_weapon_index(w), j;
    for (j = 0; j < n; j++) {
        const char* o = (const char*)s_weapons[j];
        if (E_STRCMP(name, o + WPN_MODELNAME) == 0) {
            memcpy(w + WPN_MODEL, o + WPN_MODEL, 4);
            w[WPN_MODELNAME] = 0;
            return 1;
        }
    }
    return 0;
}

/* In place of the five `call 0x451DF0` that send a 0x0D, the packet its second argument. The
   weapon is the slot's (+0xC) in the four unit senders: the slot is esi at 0x49D859, 0x49DB4D
   and 0x49DD27, and ebx at 0x49DEEE. The meteor's (0x49DFF6) is its function's argument, at the
   send's esp+0x40; that sender leaves +0x1A..+0x23 as the stack held them, so they are
   cleared. */
static int __cdecl wpn_tx_fired(unsigned int which, unsigned int* regs)
{
    unsigned char* pkt = (unsigned char*)(size_t)regs[PR_RET + 2];
    const char* w;
    int i;
    if (which < 4) {
        w = *(const char* const*)(size_t)(regs[which < 3 ? PR_ESI : PR_EBX] + 0xC);
    } else {
        w = *(const char* const*)(WPN_ESP_CALL(regs) + 0x40);
        memset(pkt + 0x1A, 0, 10);
    }
    i = tagpu_limits_weapon_index(w);
    if (i >= 0) {
        pkt[0x19] = (unsigned char)i;
        pkt[0x23] = (unsigned char)((pkt[0x23] & 0x0F) | ((i >> 8) << 4));
    }
    return 1;
}

/* In place of the 0x0E sender's two ID-byte reads in area damage 0x49A120: 0x49A78C (the weapon
   in edx, the byte into al) and 0x49A7CD (eax, into cl). The packet is at the site's esp+0x58
   with its target point written. From 256 up the companion goes out here, through the send
   and the net handle the stock message would use (esi +0x52 -> +0x96 -> +4, the reads stock
   makes next), and the type byte is cleared so the send that follows is skipped. */
static int __cdecl wpn_tx_intercept(unsigned int which, unsigned int* regs)
{
    unsigned char* pkt = WPN_ESP_CALL(regs) + 0x58;
    const char* w = (const char*)(size_t)regs[which ? PR_EAX : PR_EDX];
    unsigned int* out = &regs[which ? PR_ECX : PR_EAX];
    int i = tagpu_limits_weapon_index(w);
    unsigned int byte;
    if (i < 0) byte = *(const unsigned char*)(w + WPN_BYTE);
    else if (i < 256) byte = (unsigned int)i;
    else {
        unsigned char m[0x41];
        const char* owner = *(const char* const*)(size_t)(regs[PR_ESI] + 0x52);
        unsigned int net = *(const unsigned int*)(*(const char* const*)(owner + 0x96) + 4);
        memset(m, 0, sizeof m);
        m[0] = 0x05;
        m[2] = WPN_CHAT_TAG;
        memcpy(m + 3, pkt + 1, 12);
        m[15] = (unsigned char)i;
        m[16] = (unsigned char)(i >> 8);
        E_SEND(net, m, sizeof m);
        pkt[0] = 0;
        byte = 0;
    }
    *out = (*out & ~0xFFu) | byte;
    return 0;
}

/* In place of the 0x0E sender's two `call 0x451DF0` (0x49A7A8, 0x49A7E9): 0 skips the send. */
static int __cdecl wpn_tx_intercept_send(unsigned int which, unsigned int* regs)
{
    (void)which;
    return ((const unsigned char*)(size_t)regs[PR_RET + 2])[0] == 0x0E;
}

/* In place of the 0x0E receiver's byte compare at 0x49AFC9 (esi the local projectile, edi the
   message; the target point has matched): a weapon below 256 whose index is the byte.
   1 is a match; the stub makes it the zero flag that `je 0x49AFE8` reads. */
static int __cdecl wpn_rx_intercept(unsigned int* regs)
{
    const unsigned char* m = (const unsigned char*)(size_t)regs[PR_EDI];
    int i = tagpu_limits_weapon_index(*(const char* const*)(size_t)regs[PR_ESI]);
    return i >= 0 && i < 256 && (unsigned int)i == m[0xD];
}

/* Tag 0x49 of the 0x05 receiver (hit_rx_chat, which owns the dispatch table's 0x05 slot
   0x455F90 in both builds), ahead of stock's chat handler 0x45522E, which still runs and
   returns at once for text that starts with a zero byte (0x463CA7). A
   companion detonates, as stock's 0x49AF90 does, the first local projectile whose target point
   is the message's, with a weapon whose index is its ID.

   ONLY ON THE GAME THREAD WITH THE IN-PLAY HANDLER 0x499200 AT main+0x391F5, AND THAT IS AN
   ORDERING. During a network load two threads pump messages, the loader (0x49727D) and the
   game thread's loading screen (0x49852E), while the loader allocates the projectile pool
   (0x499A30). 0x499200 is stored in two places: 0x498455, which the game thread reaches after
   reading bit 1 of main+0x38D75, the loader's last store (0x497C62), and 0x490BC5,
   SetInputMode 0x490B30's mode 6, which none of its callers passes (they pass 1, 2 and 7).
   0x49847E clears that flag word right after the install, so each load waits on its own
   loader. With the handler there, the level's pool is complete and no other thread writes it.
   It leaves the slot only when play ends: every exit replaces the handler, after which the
   gate drops, and where the teardown runs before the replacement it nulls the pool at
   0x499A9A on this thread, and the `!pool` test drops. A drop is therefore before play or
   after it. Before play this peer has fired nothing, so
   the companion can name only a remote copy, whose detonation is visual: 0x499EB0 applies
   damage for a projectile of a local owner only. The one residual is the first in-play
   frame, which 0x49842F runs before 0x498455 installs the handler: a projectile this peer
   fires in that frame and another peer catches before the frame ends. Stock's 0x0E has no
   such gate; the dispatcher passes it in net state 6 only (mask 4, 0x45200B and
   0x454762..0x45478B). Drops are logged. */
static volatile LONG s_wpnChatNotes;     /* its own budget: out of play a companion drops by design,
                                            and must not use up the malformed messages' notes */
#define wpn_chat_note(what, a, b) wpn_note_in(&s_wpnChatNotes, (what), (a), (b))

static int __cdecl wpn_rx_chat(unsigned int* regs)
{
    const unsigned char* m = *(const unsigned char* const*)(WPN_ESP_JMP(regs) + 0x10);
    const char* ta = *(const char* const*)0x00511DE8;
    char* pool;
    unsigned int id;
    int count, i;
    if (m[1] != 0 || m[2] != WPN_CHAT_TAG) return 0;
    id = m[15] | (unsigned int)m[16] << 8;
    if (!ta || *(const unsigned int*)(ta + 0x391F5) != 0x00499200u ||
        GetCurrentThreadId() != s_wpnGameTid) {
        wpn_chat_note("a companion arrived off the game thread or out of play; dropped", id,
                 ta ? *(const unsigned int*)(ta + 0x391F5) : 0);
        return 0;
    }
    if (id < 256 || id >= TAGPU_LIM_WEAPONS) {
        wpn_chat_note("a companion names a weapon outside 256..4095; dropped", id, 0);
        return 0;
    }
    count = *(const int*)(ta + 0x141F3);
    pool = *(char* const*)(ta + 0x141F7);
    if (!pool) { wpn_chat_note("a companion arrived with no projectile pool; dropped", id, 0); return 0; }
    if (count > TAGPU_LIM_PROJ) count = TAGPU_LIM_PROJ;
    for (i = 0; i < count; i++) {
        char* p = pool + i * 0x6B;
        if (!memcmp(p + 0x28, m + 3, 12) &&
            tagpu_limits_weapon_index(*(const char* const*)p) == (int)id) {
            E_DETONATE(p);
            break;
        }
    }
    return 0;
}
#endif

/* pushad; push esp; [push which;] call fn; add esp; test eax,eax (cmp eax,1 when `one`); popad.
   The flags survive popad for the branch after it. */
static unsigned char* wpn_call(unsigned char* p, const void* fn, int which, int one)
{
    *p++ = 0x60;
    *p++ = 0x54;
    if (which >= 0) { *p++ = 0x6A; *p++ = (unsigned char)which; }
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)fn); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = (unsigned char)(which >= 0 ? 8 : 4);
    if (one) { *p++ = 0x83; *p++ = 0xF8; *p++ = 0x01; }
    else     { *p++ = 0x85; *p++ = 0xC0; }
    *p++ = 0x61;
    return p;
}

static unsigned char* wpn_rel(unsigned char* p, unsigned int target)
{
    tagpu_detour_rel(p, target);
    return p + 4;
}

/* the function's answer 0: drop the site's return address and leave through `out` */
static unsigned char* wpn_or_leave(unsigned char* p, unsigned int out)
{
    *p++ = 0x74; *p++ = 0x01;                  /* jz leave              */
    *p++ = 0xC3;                               /* ret                   */
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;     /* leave: add esp,4      */
    *p++ = 0xE9;
    return wpn_rel(p, out);
}

/* the function's answer 0: skip the send, cleaning its arguments as it would */
static unsigned char* wpn_send_or_skip(unsigned char* p, unsigned int send, unsigned char args)
{
    *p++ = 0x74; *p++ = 0x05;                  /* jz skip               */
    *p++ = 0xE9; p = wpn_rel(p, send);
    *p++ = 0xC2; *p++ = args; *p++ = 0x00;     /* skip: ret args        */
    return p;
}

static unsigned char* wpn_then(unsigned char* p, unsigned int to)
{
    *p++ = 0xE9;
    return wpn_rel(p, to);
}

typedef struct WPNSITES {
    FIXSITE     s[WPN_MAXSITE];
    const char* name[WPN_MAXSITE];
    int         n;
    int         over;                     /* a site past WPN_MAXSITE: our bug, nothing installs */
    FIXSITE     spill;
} WPNSITES;

static FIXSITE* wpn_site(WPNSITES* t, unsigned int va, int n, const unsigned char* was,
                         const char* name)
{
    FIXSITE* s;
    if (t->n >= WPN_MAXSITE) { t->over = 1; s = &t->spill; }
    else { s = &t->s[t->n]; t->name[t->n++] = name; }
    s->va = va;
    s->n = n;
    memcpy(s->was, was, (size_t)n);
    memcpy(s->now, was, (size_t)n);
    return s;
}

/* A dword operand at `at` within the site: an address or a bound. */
static void wpn_operand(FIXSITE* s, int at, unsigned int v)
{
    memcpy(s->now + at, &v, 4);
}

/* Every weapon site this build writes, with its stubs. 0 when a stub could not be allocated. */
static int wpn_build(WPNSITES* t)
{
    static const unsigned char loader[6]   = { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 };
    static const unsigned char rxFired[16] = { 0x8D, 0x0C, 0x52, 0xC1, 0xE1, 0x03, 0x2B, 0xCA,
                                               0x8D, 0x34, 0x49, 0x8B, 0x0D, 0xE8, 0x1D, 0x51 };
    static const unsigned char rxFeature[4] = { 0x4D, 0x54, 0x45, 0x00 };           /* 0x45544D */
    static const unsigned char txFeature[5] = { 0xE8, 0x46, 0xD6, 0x02, 0x00 };     /* 0x451BC0 */
    FIXSITE* s;
    unsigned char* a;

    t->n = 0;
    t->over = 0;
    s_wpnGameTid = GetCurrentThreadId();

    if (!(a = fix_code(32))) return 0;
    wpn_or_leave(wpn_call(a, (const void*)wpn_loader_id, -1, 0), 0x0042F333);
    fix_branch(wpn_site(t, 0x0042E468, 6, loader, "weapon loader, the ID's bound"), 0xE8, a);

    if (!(a = fix_code(32))) return 0;
    wpn_or_leave(wpn_call(a, (const void*)wpn_rx_fired, -1, 0), 0x0049D55D);
    s = wpn_site(t, 0x0049D280, 16, rxFired, "0x0D receiver, the weapon and its bounds");
    fix_branch(s, 0xE8, a);
    s->now[5] = 0xEB; s->now[6] = 0x1F;                       /* jmp short 0x49D2A6 */
    memcpy(s->now + 7, rxFired + 7, 9);

    if (!(a = fix_code(32))) return 0;
    wpn_then(wpn_call(a, (const void*)wpn_rx_feature, -1, 0), 0x00455F50);
    wpn_operand(wpn_site(t, 0x00455FB8, 4, rxFeature, "0x0F receiver, the dispatch slot"), 0,
                (unsigned int)(size_t)a);

    if (!(a = fix_code(32))) return 0;
    wpn_send_or_skip(wpn_call(a, (const void*)wpn_tx_feature, -1, 0), 0x00451BC0, 0x10);
    fix_branch(wpn_site(t, 0x00424575, 5, txFeature, "0x0F sender, a weapon's hit"), 0xE8, a);

#ifndef TAGPU_LIMITS_STOCK
    {
        static const unsigned char movEax[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };
        static const unsigned char movEcx[6] = { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 };
        static const unsigned char movEdx[6] = { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 };
        static const unsigned char cmpEbx[6] = { 0x81, 0xFB, 0x00, 0x15, 0x01, 0x00 };
        static const unsigned char cmpEsi[6] = { 0x81, 0xFE, 0x00, 0x15, 0x01, 0x00 };
        static const unsigned char idByte[6] = { 0x8A, 0x85, 0x0A, 0x01, 0x00, 0x00 };
        static const unsigned char store[15] = { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00, 0x8D, 0x7C,
                                                 0x24, 0x40, 0x8B, 0x44, 0x24, 0x10, 0x25 };
        /* mov [ebp+0x74],esi; lea edi,[esp+0x40]; lea edx,[ebp+0x80]; jmp 0x42ED7B, the copy */
        static const unsigned char storeOurs[15] = { 0x89, 0x75, 0x74, 0x8D, 0x7C, 0x24, 0x40,
                                                     0x8D, 0x95, 0x80, 0x00, 0x00, 0x00, 0xEB, 0x26 };
        static const unsigned char icptRead[2][6] = {
            { 0x8A, 0x82, 0x0A, 0x01, 0x00, 0x00 }, { 0x8A, 0x88, 0x0A, 0x01, 0x00, 0x00 } };
        static const unsigned char icptSend[2][5] = {
            { 0xE8, 0x43, 0x76, 0xFB, 0xFF }, { 0xE8, 0x02, 0x76, 0xFB, 0xFF } };
        static const unsigned char icptCmp[11] = { 0x8B, 0x1E, 0x8A, 0x9B, 0x0A, 0x01, 0x00, 0x00,
                                                   0x3A, 0x5F, 0x0D };
        static const struct { unsigned int va; unsigned char was[5]; } fired[5] = {
            { 0x0049D859, { 0xE8, 0x92, 0x45, 0xFB, 0xFF } },
            { 0x0049DB4D, { 0xE8, 0x9E, 0x42, 0xFB, 0xFF } },
            { 0x0049DD27, { 0xE8, 0xC4, 0x40, 0xFB, 0xFF } },
            { 0x0049DEEE, { 0xE8, 0xFD, 0x3E, 0xFB, 0xFF } },
            { 0x0049DFF6, { 0xE8, 0xF5, 0x3D, 0xFB, 0xFF } },
        };
        const unsigned int less = (unsigned int)(size_t)s_weapons - WPN_MAIN;
        const unsigned int bound = TAGPU_LIM_WEAPONS * WPN_REC;
        unsigned char* p;
        int k;

        /* `mov reg,[main]` that feeds only a weapon address: the same length as an immediate */
        s = wpn_site(t, 0x0042CDCD, 5, movEax, "unit loader, Weapons[0] for a missing name");
        s->now[0] = 0xB8; wpn_operand(s, 1, less);
        s = wpn_site(t, 0x0042F3AB, 5, movEax, "weapon release, the array");
        s->now[0] = 0xB8; wpn_operand(s, 1, less);
        s = wpn_site(t, 0x0049E5CB, 5, movEax, "weapon name lookup, the array");
        s->now[0] = 0xB8; wpn_operand(s, 1, less);
        s = wpn_site(t, 0x00437CF7, 6, movEcx, "meteor weapon, Weapons[0] for a missing name");
        s->now[0] = 0xB9; wpn_operand(s, 1, less); s->now[5] = 0x90;
        s = wpn_site(t, 0x00437D13, 6, movEdx, "meteor weapon, Weapons[0] for a wrong one");
        s->now[0] = 0xBA; wpn_operand(s, 1, less); s->now[5] = 0x90;
        wpn_operand(wpn_site(t, 0x0042F431, 6, cmpEbx, "weapon release, the bound"), 2, bound);
        wpn_operand(wpn_site(t, 0x0049E5EB, 6, cmpEsi, "weapon name lookup, the bound"), 2, bound);

        if (!(a = fix_code(32))) return 0;
        wpn_then(wpn_call(a, (const void*)wpn_wipe, -1, 0), 0x0042E345);
        fix_branch(wpn_site(t, 0x0042E31C, 6, movEcx, "weapon load wipe"), 0xE9, a);

        if (!(a = fix_code(32))) return 0;
        p = wpn_call(a, (const void*)wpn_model_reuse, -1, 0);
        *p++ = 0x0F; *p++ = 0x85; p = wpn_rel(p, 0x0042EDA1);     /* jnz: borrowed */
        wpn_then(p, 0x0042ECF9);
        fix_branch(wpn_site(t, 0x0042EC99, 6, idByte, "weapon model, an earlier one's"), 0xE9, a);
        memcpy(wpn_site(t, 0x0042ED46, 15, store, "weapon model, the store")->now, storeOurs, 15);

        for (k = 0; k < 5; k++) {
            if (!(a = fix_code(32))) return 0;
            wpn_send_or_skip(wpn_call(a, (const void*)wpn_tx_fired, k, 0), 0x00451DF0, 0x0C);
            fix_branch(wpn_site(t, fired[k].va, 5, fired[k].was, "0x0D sender, the ID's high bits"),
                       0xE8, a);
        }

        for (k = 0; k < 2; k++) {
            if (!(a = fix_code(32))) return 0;
            *wpn_call(a, (const void*)wpn_tx_intercept, k, 0) = 0xC3;
            fix_branch(wpn_site(t, k ? 0x0049A7CD : 0x0049A78C, 6, icptRead[k],
                                "0x0E sender, the ID"), 0xE8, a);
        }
        if (!(a = fix_code(32))) return 0;
        wpn_send_or_skip(wpn_call(a, (const void*)wpn_tx_intercept_send, 0, 0), 0x00451DF0, 0x0C);
        for (k = 0; k < 2; k++)
            fix_branch(wpn_site(t, k ? 0x0049A7E9 : 0x0049A7A8, 5, icptSend[k],
                                "0x0E sender, stock's message below 256"), 0xE8, a);

        if (!(a = fix_code(32))) return 0;
        *wpn_call(a, (const void*)wpn_rx_intercept, -1, 1) = 0xC3;
        fix_branch(wpn_site(t, 0x0049AFC9, 11, icptCmp, "0x0E receiver, the match"), 0xE8, a);

        /* the 0x05 receiver is B4's in both builds: hit_rx_chat hands this tag to wpn_rx_chat */
    }
#endif
    return !t->over;
}

/* CLASS: simulation, fail closed. The 0x0F hit flag is a wire format: a player without it
   reads a flagged hit as stock reads it. In the raised build its sites are rows of the limits
   table; in the stock build they join the table here. */
static int fix_weapon_ids(void)
{
#ifdef TAGPU_LIMITS_STOCK
    static WPNSITES w;
    int k;
    if (!wpn_build(&w)) {
        if (w.over) s_limOverflow = 1;
        else lim_no_stub();
        return FIX_TABLE;
    }
    for (k = 0; k < w.n; k++) lim_add(w.s[k].va, w.s[k].n, w.s[k].was, w.s[k].now, w.name[k]);
    return FIX_TABLE;
#else
    return FIX_LIMITS;
#endif
}

/* ===== WIRE ROBUSTNESS: THE RECEIVERS' UNBOUNDED INDICES ==================================
   Landing B3 of research/notes/tadr-port/sim-fixes.md; the disassembly is in
   sim-fixes-evidence.md Part 1 §3 (the unbounded 0x09/0x0B/0x0C indices), §8 (the diverged
   0x0D, handled in wpn_rx_fired above) and §9 (the 0x2C receiver's bounds), and §10 (the
   oracle counters). CLASS: local — every bound is stock-exact for a well-formed message and
   drops only a malformed or foreign one, so a peer without the fix computes the same shared
   state. The identity rests on the disassembly, not on injected traffic; tagpu_wirecheck.on
   checks the C predicates at attach, and no more than them.

   Five network receivers index the unit array (stride 0x118, first at main+0x14357, last
   inclusive at main+0x1435B) by a u16 taken straight off the wire, with no bound:
     - 0x09 CreateFromNetwork 0x4861D0: index at rec+3, type at rec+1. Index 0 faults at
       0x486237; an index past the array points esi beyond the array. Type indexes the def
       table main+0x1439B + type*0x249 unbounded.
     - 0x0C the destructor 0x4866D0: index at rec+1 (0 faults at 0x486706), killer at rec+7.
     - 0x0B the damage receiver 0x489CE0: victim at rec+1, attacker at rec+3.
     - 0x0A the attach 0x48AB70: child at rec+1 (0 returns having done nothing, 0x48ABC7),
       parent at rec+3 (0 is the detach, 0x48ABA9), both scaled at 0x48AB8A / 0x48ABAF.
     - 0x2C the stat/move receiver 0x48B920: a dirty entry's slot delta (signed, relative to
       the player's block), its type, and a round-robin full-state entry whose type and remainder
       (a SIGNED idiv, 0x48BAAB) are likewise unbounded, and whose player block pointer is
       trusted. Its bit reader 0x415DC0 has no end, and the receiver discards the message's own
       [16] size at 0x48B944, so a message with no 0xFFFF inside it is parsed from whatever
       follows it in the receive buffer. Two more unit indices ride inside it, each an optional
       reference whose 0 the engine takes as no unit: the 0x4FD9E0 move class's payload names
       a target (u16 at 0x44E0D0, handed to the reference set 0x489690 at 0x44E0FF), and the
       round robin's full state names the unit's carrier (15 bits at 0x48B56B, the parent of
       the attach record 0x48AB70 applies at 0x48B58B).

   THE FIX bounds each value before use. Every stub is entered by a jmp at a clean 5-byte
   boundary, verifies the whole stock span first (all-or-nothing: one non-stock span leaves the
   image untouched), sets the same registers stock would, and continues at the same address; a
   failed bound goes to the receiver's own drop/exit. A 0x2C is parsed from a zero-padded copy
   of itself (wire_s2c_copy), and its reader's position is bounded by the message's length
   before each read the stubs precede (wire_2c_len). On a misframed 0x2C the reader is pointed
   at a static zero dword and sent to the engine's own end-of-list 0x48BA28, so no round-robin
   entry is parsed from a stream that cannot be re-framed. The transport's splitter ends a
   packet at a message too short to advance it (wire_split_*).

   THREADS. These receivers run on the game thread only, by the dispatcher's gate: 0x451FD0
   (called at 0x491369) is the only filler of the receive-mask table 0x512BC0 for their codes,
   and gives 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E and 0x2C the mask 4 (0x451FE7..0x45200B); the
   dispatcher passes mask 4 only in net state 6 (0x454762..0x45478B), which 0x498445 sets on
   the game thread after the load (state 5 during it). The 0x2C statics below also rest on the
   receiver being serialised with no re-entry between one check and the next: nothing it calls
   between them (the direct-call closure of 0x4861D0, the move class's parse) reaches the pump
   0x453D40. The one piece that runs on whichever thread pumps, the length capture in the
   receive 0x4534E0, keeps its values in TLS. */

/* pure predicates over field values and engine counts — no I/O, no engine reads; the
   tagpu_wirecheck.on self-check exercises exactly these on boundary values */
static int wire_index_ok(unsigned int idx, unsigned int max)  { return idx >= 1u && idx <= max; }
static int wire_killer_ok(unsigned int idx, unsigned int max) { return idx == 0u || idx <= max; }
static int wire_type_ok(unsigned int type, unsigned int count){ return type >= 1u && type < count; }
static int wire_delta_ok(int delta, int n)                    { return delta >= 0 && delta < n; }
/* an optional unit reference, as the index the engine should see: 0 (no unit) stays 0, a slot
   stays itself, and an index past the array becomes 0, the engine's own "no unit" */
static unsigned int wire_ref_idx(unsigned int idx, unsigned int max)
{
    return wire_index_ok(idx, max) ? idx : 0u;
}
/* an attach record's ids: the child a slot (0 is no record 0x48AB70 acts on), the parent no
   unit (0, the detach) or a slot */
static int wire_attach_ok(unsigned int child, unsigned int parent, unsigned int max)
{
    return wire_index_ok(child, max) && wire_ref_idx(parent, max) == parent;
}
static int wire_bits_ok(unsigned long long pos, unsigned int need, unsigned long long end)
{
    return pos + need <= end;
}
/* a message the splitter may take: a length that advances it, and for a 0x2C at least its
   7-byte header ([8] code, [16] size, [32] GameTime), which every sender writes */
static int wire_split_ok(unsigned int code, unsigned int len)
{
    return len != 0u && (code != 0x2Cu || len >= 7u);
}

/* A player's block is a run of N slots starting at begin + (1 + k*N)*0x118, k < 10 (10*N+1
   slots, slot 0 the sentinel), and ending N-1 slots later, all inside [begin, arr_last].
   k is the player's RANK, not its record's index: 0x4858A6..0x4858E0 assigns blocks in the
   order of an insertion sort (0x485657..0x4856C0) that, in a network game, compares the
   records' DirectPlay ids [rec+4]. A wild [player+0x67] fails this even when a 16-bit delta
   could not. */
static int wire_block_ok(const char* first, const char* last, const char* begin,
                         const char* arr_last, unsigned int n)
{
    long long off;
    unsigned int slot;
    if (!first || !begin || !last || n == 0u) return 0;
    if (first < begin || first > arr_last || last > arr_last || last < first) return 0;
    off = (long long)(first - begin);
    if (off % 0x118 != 0) return 0;
    slot = (unsigned int)(off / 0x118);
    if (slot < 1u || (slot - 1u) % n != 0u || (slot - 1u) / n >= 10u) return 0;
    return last == first + (long long)(n - 1u) * 0x118;
}

/* The B4/B5 oracle counters (§10), observe-only. morph = a create onto a live slot whose type
   changes. dcreate = CreateFromNetwork called by the 0x2C dirty list (returning to 0x48BA05):
   a dirty entry whose type is not its slot's, the unit made from the entry rather than from its
   own 0x09 — into an empty slot, the ghost commander's mechanism (§2). rcreate = the same from
   the round robin (returning to 0x48B49C). ghost = the engine's own ghost sweep firing (a
   round-robin type 0 over an occupied slot). */
static unsigned int s_wireMorph, s_wireDCreate, s_wireRCreate, s_wireGhost;
/* CreateFromNetwork's own refusal, for B5: when the create's player has no block
   ([rec+0x67] == 0) the jne at 0x486229 falls through to 0x48622B, which returns 0; counted per
   caller. noarr = wire_s09 found no unit array to bound against */
static unsigned int s_wireNoBlk09, s_wireNoBlkDirty, s_wireNoBlkRR, s_wireNoArr;
/* drop counters, per message */
static unsigned int s_wire09, s_wire09Blk, s_wire0A, s_wire0C, s_wire0Ckill, s_wire0B, s_wire2C;
static unsigned int s_wire2CLen, s_wire2CStale, s_wire2CNoCopy;
/* a 0x2C reference past the array, made no unit: the move payload's target, the carrier */
static unsigned int s_wire2CTarget, s_wire2CCarrier;
/* the splitter's stops and the pump's re-pointed message pointer run on whichever thread pumps */
static volatile LONG s_wireSplit, s_wirePump;
/* records accepted off the wire, per receiver: evidence each bound ran on real traffic (the
   same role as the line-of-sight own-row counters). 0x0B and 0x0C count only the dispatcher's
   call (return 0x455417 / 0x455428), not the local kill and damage paths that share the
   function; 0x0A has only the dispatcher's stub to count at; 0x2C counts messages, dirty
   entries, dirty creates and round-robin entries. */
static unsigned int s_wireIn09, s_wireIn0A, s_wireIn0B, s_wireIn0C;
static unsigned int s_wireIn2C, s_wireIn2CDirty, s_wireIn2CCreate, s_wireIn2CRR;
/* the sender-block rule: a wire 0x09's slot lies in the block of its TRANSPORT sender, the
   dispatcher's edi (its player record, the one the 0x2C and 0x0D cases pass), which
   CreateFromNetwork saves with its last push (0x4861EC), so it is [esp+0] at 0x4861F7. The
   sender is bounded as one of the ten records at main+0x1B63, stride 0x14B (the loop at
   0x406E05 counts ten); the pump names record ten, one past them, for a sender it cannot
   find (0x453E14). argdiff counts a create whose player argument is not the sender. */
static unsigned int s_wireArgDiff, s_wireSendSeen;
/* the type carried from the type check to the after-create check (game thread only) */
static int s_wire_2c_type, s_wire_rr_type;
/* the 0x2C being parsed: its reader's address and its end in bits (game thread only). A check
   whose reader is not this one stops, so a nested or stale parse cannot borrow the bound. */
static const unsigned char* s_wire2cRd;
static unsigned long long   s_wire2cEnd;
static const unsigned char* s_wire2cCopy;
/* the stop path's zero dword: a misframed 0x2C reader points here so its flag bit reads 0 */
static const unsigned int s_wireZero;
/* the receive's buffer and delivered length, per thread (see wire_rx_note) */
static DWORD s_wireTlsBuf = TLS_OUT_OF_INDEXES, s_wireTlsLen = TLS_OUT_OF_INDEXES;
/* each thread's copy buffer for the 0x2C parse (see wire_s2c_copy) */
static DWORD s_wireTlsCopy = TLS_OUT_OF_INDEXES;

static volatile LONG s_wireNotes;
static void wire_drop(unsigned int* c, const char* what, unsigned int a, unsigned int b)
{
    (*c)++;
    if (InterlockedIncrement(&s_wireNotes) <= 64)
        tagpu_logf("enginefix: wire robustness: %s (%u, %u)", what, a, b);
}

/* ---- the 0x2C length ----------------------------------------------------------------------

   A message's bytes are the receive's own: 0x4534E0 takes one message into the buffer at
   main+0x2A38 and learns its length, which it hands only to the statistics call 0x415EF0 and
   then drops. wire_rx_note keeps both, per thread, at those two calls (0x453595 for the
   transport 0x462F30, 0x45361F for a raw DirectPlay receive, `-p` below 0). On the transport
   the length of a 0x2C IS its [16] size field (the splitter 0x463790 reads it at 0x4639E5 and
   queues a 0x2C only when it fits the packet, 0x46393E); a raw receive has no splitter, so the
   bound is the smaller of the two.

   THE PUMP'S POINTER. The pump 0x453D40 takes its message pointer once, before its loop
   (0x453D90 -> [esp+0x10]), and 0x4534E0 grows the buffer when a message outgrows it
   (0x453565, 0x4535EE: 0x4D84A0, a realloc [INFERRED: (block, size) in, the block out]), so
   after a growth that moves the block, every later message of the same pump call was
   dispatched from the freed one. Nothing else writes the pump's [esp+0x10] or takes its
   address. The note re-points the pump's
   [esp+0x10] at the buffer the message was just received into, so the dispatcher reads the
   message the length describes: the identity when the buffer did not move. The frame is the
   pump's by construction: 0x4534E0's one caller is 0x453D94, its prologue is one push
   (0x4534E0) and the three argument pushes are the span verified at install, and the note
   checks the return address 0x453D99 before it writes. */

/* call-entered at 0x453595 / 0x45361F in place of `call 0x415EF0` (code, len, 0), which the
   stub then jumps to with the stack as stock left it. r[PR_RET+1..3] are the arguments,
   r[PR_RET+4] 0x4534E0's local, r[PR_RET+5] its return into the pump, and the pump's frame
   starts at r[PR_RET+6], so its [esp+0x10] is r[PR_RET+10]. */
static int __cdecl wire_rx_note(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    void* buf = ta ? *(void* const*)(ta + 0x2A38) : NULL;
    TlsSetValue(s_wireTlsBuf, buf);
    TlsSetValue(s_wireTlsLen, (void*)(size_t)r[PR_RET + 2]);
    if (buf && r[PR_RET + 5] == 0x00453D99u && r[PR_RET + 10] != (unsigned int)(size_t)buf) {
        if (InterlockedIncrement(&s_wirePump) <= 16)
            tagpu_logf("enginefix: wire robustness: the receive buffer moved to %p (capacity %u) "
                       "inside a pump call; its message pointer follows (code %u, %u bytes)",
                       buf, *(const unsigned int*)(ta + 0x2A34), r[PR_RET + 1] & 0xFFu,
                       r[PR_RET + 2]);
        r[PR_RET + 10] = (unsigned int)(size_t)buf;
    }
    return 1;
}

/* The copy's padding: the most the engine can read past the last check we control, by
   disassembly of every path.
     - a dirty entry: after the type check, the move class's payload parse [vt+0x24] and then
       the next delta (16) at 0x48BA19, before the delta stub checks. The move classes'
       vtables (0x4FD458, 0x4FD488, 0x4FD980, 0x4FD9B0, 0x4FD9E0; no other table's +0x24
       reaches the reader) give 0x44EFD0 (reads 0), 0x44F5C0 (1 + 2 + 3 * 32 = 99: its loop
       runs a 2-bit count) and 0x490A10 (2 + max(0x44E080's 8+32+16+16+16+96 = 184,
       0x44E9C0's 1+6*32+16 = 209) + 2 = 213). 213 + 16 = 229.
     - the round robin after 0x48B40E: 16+8+8+2+1 at 0x48B4A9..0x48B557, then 15+8 or
       32*3+16*3 at 0x48B56B..0x48B60F and 32 at 0x48B6F2: 35 + 176 = 211. 0x48B090,
       0x48AB70, 0x47D0E0, 0x47CC30 and 0x4827B0 read nothing.
     - the header, before the entry check: 56.
   229 bits is 29 bytes past the last checked bit; the message's end can sit 3 bytes into a
   dword, and 0x415DC0 also loads the next dword when a read reaches the end of one
   (0x415E0C..0x415E3E): 8 more. */
enum {
    WIRE_2C_OVER_BITS = 229,
    WIRE_2C_PAD       = 48,
    WIRE_2C_MAX       = 0xFFFF,                   /* the [16] size bounds the copied length */
    WIRE_2C_COPY      = WIRE_2C_MAX + WIRE_2C_PAD
};
typedef char wire_2c_pad_covers[(WIRE_2C_PAD >= (WIRE_2C_OVER_BITS + 7) / 8 + 3 + 8) ? 1 : -1];

/* 0x2C, the receiver's first instruction after its pushes, branch at 0x48B92B (before the
   header's reads). eax = the message. Copies it -- the receive's delivered length, and the
   size field when that is smaller, so at most 0xFFFF bytes -- into this thread's buffer,
   zeroes WIRE_2C_PAD bytes after it, and hands the copy back in eax, which stock's
   0x48B933 stores as the reader's buffer. Every read of the parse is then inside memory we
   own, whatever the message says. A message that is not the buffer the receive described,
   or no buffer, drops to 0x48BAC8. Continue -> xor esi,esi; push 8; lea ecx,[esp+0x14];
   0x48B933. Nothing reads the reader after the handler returns: the dispatcher's case
   jumps to the next receive (0x4553F9). */
static int __cdecl wire_s2c_copy(unsigned int* r)
{
    const unsigned char* pkt = (const unsigned char*)(size_t)r[PR_EAX];
    const unsigned char* buf = (const unsigned char*)TlsGetValue(s_wireTlsBuf);
    unsigned int len = (unsigned int)(size_t)TlsGetValue(s_wireTlsLen);
    unsigned char* copy = (unsigned char*)TlsGetValue(s_wireTlsCopy);
    s_wire2cRd = 0;
    s_wire2cCopy = 0;
    if (!pkt || pkt != buf) {
        wire_drop(&s_wire2CStale, "a 0x2C is not the message the receive delivered; dropped",
                  0, len);
        return 0;
    }
    if (len >= 3u && *(const unsigned short*)(pkt + 1) < len) len = *(const unsigned short*)(pkt + 1);
    if (len > WIRE_2C_MAX) len = WIRE_2C_MAX;
    if (!copy) {
        copy = (unsigned char*)VirtualAlloc(NULL, WIRE_2C_COPY, MEM_COMMIT | MEM_RESERVE,
                                            PAGE_READWRITE);
        if (!copy) {
            wire_drop(&s_wire2CNoCopy, "no copy buffer for a 0x2C; dropped", len, 0);
            return 0;
        }
        TlsSetValue(s_wireTlsCopy, copy);
    }
    memcpy(copy, pkt, len);
    memset(copy + len, 0, WIRE_2C_PAD);
    s_wire2cRd   = WPN_ESP_JMP(r) + 0x10;
    s_wire2cEnd  = (unsigned long long)len * 8u;
    s_wire2cCopy = copy;
    r[PR_EAX] = (unsigned int)(size_t)copy;
    return 1;
}

/* The transport's splitter 0x463790 walks a packet by each message's length and advances by
   it, so a length of 0 keeps its counting loop (0x4638F0..0x463947) on one message for good,
   and its third walk (0x463AD3..0x463B8B) queues that message until the queue is full. Both
   take the length at one place; a length wire_split_ok refuses ends the split there, through
   the engine's own end for a code it does not know (0x463949; 0x463B91). The counting pass
   decides how many messages the queuing pass 0x4639BC takes, so that pass never reaches it. */
static int wire_split(unsigned int code, unsigned int len)
{
    if (wire_split_ok(code, len)) return 1;
    if (InterlockedIncrement(&s_wireSplit) <= 16)
        tagpu_logf("enginefix: wire robustness: a packet's message of code %u has length %u, too "
                   "short to advance; the packet's split ends there", code, len);
    return 0;
}
/* 0x463939 in the counting loop: ax = the length, the code at [esp+0x28] (0x4638F4) */
static int __cdecl wire_split_count(unsigned int* r)
{
    return wire_split(*(const unsigned char*)(WPN_ESP_JMP(r) + 0x28), r[PR_EAX] & 0xFFFF);
}
/* 0x463B33 in the third walk: ax = the length, the code at [esp+0x2C] (0x463AD9) */
static int __cdecl wire_split_walk(unsigned int* r)
{
    return wire_split(*(const unsigned char*)(WPN_ESP_JMP(r) + 0x2C), r[PR_EAX] & 0xFFFF);
}

static unsigned long long wire_pos(const unsigned char* rd)
{
    return (unsigned long long)*(const unsigned int*)(rd + 4) * 32u + *(const unsigned int*)(rd + 8);
}

/* the reader `rd` is the 0x2C's own, and `need` more bits fit inside it */
static int wire_2c_len(const unsigned char* rd, unsigned int need, const char* where)
{
    unsigned long long pos;
    if (rd != s_wire2cRd) {
        wire_drop(&s_wire2CLen, "a 0x2C check found another reader; stream stopped", 0, 0);
        return 0;
    }
    pos = wire_pos(rd);
    if (!wire_bits_ok(pos, need, s_wire2cEnd)) {
        if (InterlockedIncrement(&s_wireNotes) <= 64)
            tagpu_logf("enginefix: wire robustness: a 0x2C runs past its own length at %s "
                       "(bit %u + %u of %u); stream stopped", where, (unsigned int)pos, need,
                       (unsigned int)s_wire2cEnd);
        s_wire2CLen++;
        return 0;
    }
    return 1;
}

/* ---- the checkers, one per site; regs is the stub's pushad frame (PR_* order) ------------- */

/* the unit array's first slot, and its last valid index in *max (0 when there is no array) */
static const char* wire_units(const char* ta, unsigned int* max)
{
    const char* first = ta ? *(const char* const*)(ta + 0x14357) : 0;
    const char* last  = ta ? *(const char* const*)(ta + 0x1435B) : 0;
    *max = (first && last >= first) ? (unsigned int)(last - first) / 0x118 : 0u;
    return first;
}

/* 0x09, branch at 0x4861F7. edi = rec, edx = player*0x14B, ebx = main. Continue -> 0x486220
   (esi = the unit, [esp+0x20] = edx as stock stored it); drop -> 0x48622B. */
static int __cdecl wire_s09(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    unsigned char* rec = (unsigned char*)(size_t)r[PR_EDI];
    unsigned char* sp = WPN_ESP_JMP(r);
    const char* first;
    const char* last;
    unsigned int max, count, idx, type;
    if (!ta || !rec) { s_wireNoArr++; return 0; }
    first = *(const char* const*)(ta + 0x14357);
    last  = *(const char* const*)(ta + 0x1435B);
    if (!first || last < first) { s_wireNoArr++; return 0; }
    max   = (unsigned int)(last - first) / 0x118;
    count = *(const unsigned int*)(ta + 0x1438F);
    idx   = *(const unsigned short*)(rec + 3);
    type  = *(const unsigned short*)(rec + 1);
    if (!wire_index_ok(idx, max) || !wire_type_ok(type, count)) {
        wire_drop(&s_wire09, "a unit-create names a unit past the array or a bad type; dropped",
                  idx, type);
        return 0;
    }
    {
        char* slot = (char*)first + (size_t)idx * 0x118;
        unsigned int ret = *(const unsigned int*)(sp + 0x1C);   /* CreateFromNetwork's caller */
        int noblk = *(const unsigned int*)(ta + r[PR_EDX] + 0x1BCA) == 0;   /* 0x486220 */
        if (ret == 0x004553E9u) {
            const char* players = ta + 0x1B63;
            const char* snd = *(const char* const*)sp;     /* the dispatcher's edi */
            unsigned int si = 10;
            const char* sf = 0;
            const char* sl = 0;
            if (snd >= players && snd < players + 10 * 0x14B && (snd - players) % 0x14B == 0) {
                si = (unsigned int)(snd - players) / 0x14B;
                sf = *(const char* const*)(snd + 0x67);
                sl = *(const char* const*)(snd + 0x6B);
            }
            if (si >= 10 || !sf || !sl || slot < sf || slot > sl) {
                wire_drop(&s_wire09Blk, "a unit-create from a sender whose block does not hold "
                          "the unit; dropped", si, idx);
                return 0;
            }
            s_wireIn09++;
            if (r[PR_EDX] != si * 0x14B) s_wireArgDiff++;
            if (!(s_wireSendSeen & (1u << si))) {
                s_wireSendSeen |= 1u << si;
                tagpu_logf("enginefix: wire robustness: 0x09 from sender %u (type %u, '%.30s'), "
                           "player arg %u, slot %u, sender block %u..%u", si,
                           (unsigned int)*(const unsigned char*)(snd + 0x73), snd + 0x2B,
                           r[PR_EDX] / 0x14B, idx, (unsigned int)(sf - first) / 0x118,
                           (unsigned int)(sl - first) / 0x118);
            }
            if (noblk) s_wireNoBlk09++;
        } else if (ret == 0x0048BA05u) {
            s_wireDCreate++;
            if (noblk) s_wireNoBlkDirty++;
        } else if (ret == 0x0048B49Cu) {
            s_wireRCreate++;
            if (noblk) s_wireNoBlkRR++;
        }
        if (noblk && InterlockedIncrement(&s_wireNotes) <= 64)
            tagpu_logf("enginefix: wire robustness: CreateFromNetwork refuses a create for player "
                       "%u, which has no block (from %08X, slot %u, type %u)",
                       r[PR_EDX] / 0x14B, ret, idx, type);
        if ((*(const unsigned int*)(slot + 0x110) & 0x10000000u) &&
            *(const unsigned short*)(slot + 0xA6) &&
            *(const unsigned short*)(slot + 0xA6) != (unsigned short)type)
            s_wireMorph++;
        r[PR_ESI] = (unsigned int)(size_t)slot;
        *(unsigned int*)(sp + 0x20) = r[PR_EDX];   /* stock's mov [esp+0x20],edx at 0x4861FE */
    }
    return 1;
}

/* 0x0C destructor, branch at 0x4866E5 (stock's push edi at 0x4866E4 is left in place, so the
   drop's pop edi at 0x486E59 balances). ebx = rec, edx = main. Continue -> 0x486706 (esi = the
   unit); drop -> 0x486E59. */
static int __cdecl wire_s0c(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    unsigned char* rec = (unsigned char*)(size_t)r[PR_EBX];
    const char* first;
    const char* last;
    unsigned int max, idx;
    if (!ta || !rec) return 0;
    first = *(const char* const*)(ta + 0x14357);
    last  = *(const char* const*)(ta + 0x1435B);
    if (!first || last < first) return 0;
    max = (unsigned int)(last - first) / 0x118;
    idx = *(const unsigned short*)(rec + 1);
    if (!wire_index_ok(idx, max)) {
        wire_drop(&s_wire0C, "a unit-destroy names a unit past the array; dropped", idx, max);
        return 0;
    }
    if (*(const unsigned int*)(WPN_ESP_JMP(r) + 0x78) == 0x00455428u) s_wireIn0C++;
    r[PR_ESI] = (unsigned int)(size_t)((char*)first + (size_t)idx * 0x118);
    return 1;
}

/* 0x0C's killer, branch at 0x486753. ebx = rec, esi = the victim, edx = main. Always continues
   to 0x486778 (mov [esi+0xF0],eax) with eax = the killer pointer, 0 for index 0 or a bounded
   miss (counted). */
static int __cdecl wire_s0ckill(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    unsigned char* rec = (unsigned char*)(size_t)r[PR_EBX];
    r[PR_EAX] = 0;
    if (ta && rec) {
        const char* first = *(const char* const*)(ta + 0x14357);
        const char* last  = *(const char* const*)(ta + 0x1435B);
        if (first && last >= first) {
            unsigned int max = (unsigned int)(last - first) / 0x118;
            unsigned int kidx = *(const unsigned short*)(rec + 7);
            if (kidx == 0) {
                r[PR_EAX] = 0;
            } else if (!wire_killer_ok(kidx, max)) {
                wire_drop(&s_wire0Ckill, "a unit-destroy names a killer past the array; made NULL",
                          kidx, max);
                r[PR_EAX] = 0;
            } else {
                r[PR_EAX] = (unsigned int)(size_t)((char*)first + (size_t)kidx * 0x118);
            }
        }
    }
    return 1;
}

/* 0x0B, branch at 0x489CED. edi = rec, edx = main. Continue -> 0x489D37 (esi = victim, 0 for
   index 0 which stock then drops; ebx = attacker, 0 allowed); drop -> 0x489F93. */
static int __cdecl wire_s0b(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    unsigned char* rec = (unsigned char*)(size_t)r[PR_EDI];
    const char* first;
    const char* last;
    unsigned int max, vidx, aidx;
    if (!ta || !rec) return 0;
    first = *(const char* const*)(ta + 0x14357);
    last  = *(const char* const*)(ta + 0x1435B);
    if (!first || last < first) return 0;
    max  = (unsigned int)(last - first) / 0x118;
    vidx = *(const unsigned short*)(rec + 1);
    aidx = *(const unsigned short*)(rec + 3);
    if (vidx > max || aidx > max) {
        wire_drop(&s_wire0B, "a damage message names a unit past the array; dropped",
                  vidx > aidx ? vidx : aidx, max);
        return 0;
    }
    if (*(const unsigned int*)(WPN_ESP_JMP(r) + 0x0C) == 0x00455417u) s_wireIn0B++;
    r[PR_ESI] = vidx ? (unsigned int)(size_t)((char*)first + (size_t)vidx * 0x118) : 0;
    r[PR_EBX] = aidx ? (unsigned int)(size_t)((char*)first + (size_t)aidx * 0x118) : 0;
    return 1;
}

/* 0x0A attach, the dispatcher's case, branch at 0x4553FE (entry 8 of the jump table 0x455F84,
   code 0x0A), in place of stock's mov eax,[esp+0x10]; push eax before `call 0x48AB70`.
   [esp+0x10] = the record, the pump's message pointer. 0x48AB70 scales both ids into the
   array unbounded (child +1 at 0x48AB8A..0x48AB9F, parent +3 at 0x48ABAF..0x48ABC4); a child
   0 returns having done nothing (0x48ABC7) and a parent 0 is the detach (0x48ABA9). The sender
   0x48AAC0 writes its child's own +0xA8 (it has read the child's +0x110 at 0x48AAC7, so the
   child is never NULL) and its parent's, or 0. A child outside [1, max] or a parent past the
   array drops the record to 0x455F50, the back edge every case and every unknown code takes;
   otherwise -> the displaced mov and push, then 0x455403. Only the wire's path is guarded:
   0x48AB70's other callers are the local wrapper 0x48AB62 (the ids of live units) and the 0x2C
   round robin (0x48B58B, its carrier bounded at 0x48B574; 0x48B5C5, the slot's own id and
   parent 0). */
static int __cdecl wire_s0a(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    const unsigned char* rec = *(const unsigned char* const*)(WPN_ESP_JMP(r) + 0x10);
    unsigned int max, child, parent;
    if (!ta || !rec) return 0;
    wire_units(ta, &max);
    child  = *(const unsigned short*)(rec + 1);
    parent = *(const unsigned short*)(rec + 3);
    if (!wire_attach_ok(child, parent, max)) {
        wire_drop(&s_wire0A, "an attach names no child or a unit past the array; dropped",
                  child, parent);
        return 0;
    }
    s_wireIn0A++;
    return 1;
}

/* 0x2C entry, branch at 0x48B960, after the header's three reads ([8] code, [16] size,
   [32] GameTime, 56 bits, from the copy). edi = player record, ebp = GameTime, the reader at
   [esp+0x10]. Continue -> 0x48B96E (stock's mov [edi+0x18],ebp done here); a reader that is
   not the copy's, a header and first delta that do not fit the message, or a block that is
   not a valid slot run, drops to 0x48BAC8, the engine's own clean 'no block' exit. The block check covers the dirty loop, the block sweep 0x48BA28 and the
   round-robin slot 0x48BAAD, all of which read [edi+0x67]/[edi+0x6B]. */
static int __cdecl wire_s2c_entry(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    char* pr = (char*)(size_t)r[PR_EDI];
    unsigned char* sp = WPN_ESP_JMP(r);
    const unsigned char* rd = sp + 0x10;
    const char* arr_first;
    const char* arr_last;
    const char* bfirst;
    const char* blast;
    unsigned int n;
    if (!pr || !ta) return 0;
    if (!s_wire2cCopy || *(const unsigned char* const*)rd != s_wire2cCopy ||
        !wire_2c_len(rd, 16, "the first delta")) {
        s_wire2cRd = 0;
        return 0;
    }
    *(unsigned int*)(pr + 0x18) = r[PR_EBP];
    arr_first = *(const char* const*)(ta + 0x14357);
    arr_last  = *(const char* const*)(ta + 0x1435B);
    n = *(const unsigned short*)(ta + 0x37EE6);
    bfirst = *(const char* const*)(pr + 0x67);
    blast  = *(const char* const*)(pr + 0x6B);
    if (!wire_block_ok(bfirst, blast, arr_first, arr_last, n)) {
        wire_drop(&s_wire2C, "a 0x2C names a player whose block is not a valid slot run; dropped",
                  0, n);
        return 0;
    }
    s_wireIn2C++;
    return 1;
}

/* 0x2C dirty-entry delta, branch at 0x48B985, after the delta's read (the first, or the one at
   0x48BA19 after the previous entry's payload). edi = player record, ax = the signed delta.
   Continue -> 0x48B99D (esi = the slot, eax = main); a delta outside [0, N), or a read so far
   or a type read next that does not fit the message, stops the stream. */
static int __cdecl wire_s2c_delta(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    char* pr = (char*)(size_t)r[PR_EDI];
    const char* bfirst;
    int delta;
    unsigned int n;
    if (!ta || !pr) return 0;
    if (!wire_2c_len(WPN_ESP_JMP(r) + 0x10, *(const unsigned int*)(ta + 0x14393), "a dirty type"))
        return 0;
    bfirst = *(const char* const*)(pr + 0x67);
    n = *(const unsigned short*)(ta + 0x37EE6);
    delta = (int)(short)(unsigned short)(r[PR_EAX] & 0xFFFF);
    if (!bfirst || !wire_delta_ok(delta, (int)n)) {
        wire_drop(&s_wire2C, "a 0x2C dirty entry's slot delta is out of range; stream stopped",
                  (unsigned int)delta, n);
        return 0;
    }
    s_wireIn2CDirty++;
    r[PR_ESI] = (unsigned int)(size_t)(bfirst + (size_t)delta * 0x118);
    r[PR_EAX] = (unsigned int)(size_t)ta;
    return 1;
}

/* 0x2C dirty-entry type, branch at 0x48B9AD. esi = the slot, ax = the type. Returns
   0 stop, 1 create (-> 0x48B9B6), 2 the type matched the slot's own (-> 0x48BA05, parse in
   place). A type outside [1, count) or one whose def has no move class stops the stream:
   0x48BA05 would otherwise dereference a NULL mover (§9's 13 field faults at 0x48BA07). The
   move class's own parse follows, and only it knows its width: a message that ends inside it
   is caught at the next delta, after the parse. */
static int __cdecl wire_s2c_type(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    char* slot = (char*)(size_t)r[PR_ESI];
    const char* defs;
    unsigned int count, type;
    if (!ta || !slot) return 0;
    count = *(const unsigned int*)(ta + 0x1438F);
    type  = r[PR_EAX] & 0xFFFF;
    if (!wire_type_ok(type, count)) {
        wire_drop(&s_wire2C, "a 0x2C dirty entry's type is out of range; stream stopped",
                  type, count);
        return 0;
    }
    defs = *(const char* const*)(ta + 0x1439B);
    if (!defs || *(const unsigned char*)(defs + (size_t)type * 0x249 + 0x22F) != 1) {
        wire_drop(&s_wire2C, "a 0x2C dirty entry's type has no move class; stream stopped",
                  type, count);
        return 0;
    }
    s_wire_2c_type = (int)type;
    if (*(const unsigned short*)(slot + 0xA6) == (unsigned short)type) return 2;
    s_wireIn2CCreate++;
    return 1;
}

/* 0x2C after the dirty create, branch at 0x48BA05. esi = the slot. Requires the slot now holds
   the created type, and its mover [esi] and the mover's object [[esi]] non-NULL, before the
   payload parse dereferences them. Continue -> 0x48BA0D (eax = &reader at [esp+0x10],
   ecx = [[esi]]); a failed create stops. */
static int __cdecl wire_s2c_after(unsigned int* r)
{
    unsigned char* sp = WPN_ESP_JMP(r);
    char* slot = (char*)(size_t)r[PR_ESI];
    const char* obj;
    if (!slot || *(const unsigned short*)(slot + 0xA6) != (unsigned short)s_wire_2c_type) {
        wire_drop(&s_wire2C, "a 0x2C dirty create did not take; stream stopped",
                  (unsigned int)s_wire_2c_type, 0);
        return 0;
    }
    obj = *(const char* const*)slot;
    if (!obj || !*(const char* const*)obj) {
        wire_drop(&s_wire2C, "a 0x2C dirty unit has no mover object; stream stopped", 0, 0);
        return 0;
    }
    r[PR_EAX] = (unsigned int)(size_t)(sp + 0x10);
    r[PR_ECX] = *(const unsigned int*)obj;
    return 1;
}

/* 0x2C round-robin flag, at 0x48BA5E, after the dirty list's end and the block sweep, before
   the flag bit's direct read. The stop path's zero reader passes (its bit reads 0). Otherwise
   the flag bit must fit, and when it is set, so must the round-robin type read 0x48B409 that
   follows. Continue -> the two stock loads, then 0x48BA66; otherwise -> 0x48BAC8, no round
   robin. */
static int __cdecl wire_s2c_flag(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    const unsigned char* rd = WPN_ESP_JMP(r) + 0x10;
    const unsigned int* buf = *(const unsigned int* const*)rd;
    unsigned long long pos;
    if (buf == &s_wireZero) return 1;
    if (!ta || !wire_2c_len(rd, 1, "the round-robin flag")) return 0;
    pos = wire_pos(rd);
    if ((buf[pos / 32u] >> (pos % 32u)) & 1u)
        return wire_2c_len(rd, 1u + *(const unsigned int*)(ta + 0x14393), "the round-robin type");
    return 1;
}

/* 0x2C round-robin type, branch at 0x48B40E. esi = the reader, [esp+0x38] = the slot, ax = the
   type, and ebp is zeroed as stock's xor ebp,ebp. Returns 0 stop (-> 0x48B435), 1 the engine's
   own type-0 ghost path (-> 0x48B415), 2 the typed path (-> 0x48B43F). A type 0 over an
   occupied slot is the engine's ghost sweep and is counted, not dropped. Only the type is
   bounded: the round robin carries every unit, structures included, and tests the mover itself
   before its one use (0x48B6E8: [edi] == 0 skips to 0x48B6FC), so a type with no move class is
   well-formed here, unlike in the dirty list, whose sender lists only units with a mover
   (0x48B782..0x48B786). The full-state reads after the create branch on their own data, so a
   message that ends inside them is read to their end. */
static int __cdecl wire_s2c_rr(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    unsigned char* sp = WPN_ESP_JMP(r);
    const char* slot;
    unsigned int count, type;
    r[PR_EBP] = 0;
    if (!ta || !wire_2c_len((const unsigned char*)(size_t)r[PR_ESI], 0, "the round-robin entry"))
        return 0;
    slot  = *(const char* const*)(sp + 0x38);
    count = *(const unsigned int*)(ta + 0x1438F);
    type  = r[PR_EAX] & 0xFFFF;
    if (type == 0) {
        if (slot && *(const unsigned short*)(slot + 0xA6)) s_wireGhost++;
        s_wireIn2CRR++;
        return 1;
    }
    if (!wire_type_ok(type, count)) {
        wire_drop(&s_wire2C, "a 0x2C round-robin type is out of range; entry stopped", type, count);
        return 0;
    }
    s_wire_rr_type = (int)type;
    s_wireIn2CRR++;
    return 2;
}

/* 0x2C after the round-robin create, branch at 0x48B49C. edi = the slot. Requires the slot
   holds the created type and its model object [edi+0x9E] (the Object3do, set at 0x485DCC;
   +0x9A is the COB script) non-NULL, since 0x48B4A6 writes through it. Continue -> 0x48B4A2
   (eax = [edi+0x9E]); otherwise stop -> 0x48B435. */
static int __cdecl wire_s2c_rr_after(unsigned int* r)
{
    char* slot = (char*)(size_t)r[PR_EDI];
    const char* obj;
    if (!slot || *(const unsigned short*)(slot + 0xA6) != (unsigned short)s_wire_rr_type) {
        wire_drop(&s_wire2C, "a 0x2C round-robin create did not take; entry stopped",
                  (unsigned int)s_wire_rr_type, 0);
        return 0;
    }
    obj = *(const char* const*)(slot + 0x9E);
    if (!obj) {
        wire_drop(&s_wire2C, "a 0x2C round-robin unit has no model object; entry stopped", 0, 0);
        return 0;
    }
    r[PR_EAX] = (unsigned int)(size_t)obj;
    return 1;
}

/* 0x2C move payload's target, branch at 0x44E0DE in 0x44E080, the payload parse of the
   0x4FD9E0 move class (a dirty entry's [vt+0x24] 0x490A10 calls it at 0x490A4F), after stock's
   own test has sent index 0 to 0x44E0DA. ax = the index, nonzero; the sender writes its
   target's own +0xA8, or 0 for none (0x44DDEC..0x44DDF9). Continue -> 0x44E0FC (push eax; the
   reference set 0x489690) with eax = the slot, or for an index past the array NULL, counted:
   0x44E0DA's own value, which 0x489690 takes as no unit (0x4896C3 -> 0x4896E6 clears the
   reference). ecx and edx as stock leaves them (index*35, main). */
static int __cdecl wire_2c_target(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    unsigned int idx = r[PR_EAX] & 0xFFFF, max, keep;
    const char* first = wire_units(ta, &max);
    keep = wire_ref_idx(idx, max);
    if (keep != idx)
        wire_drop(&s_wire2CTarget, "a 0x2C move payload names a target past the array; no target",
                  idx, max);
    r[PR_EAX] = keep ? (unsigned int)(size_t)(first + (size_t)keep * 0x118) : 0u;
    r[PR_ECX] = idx * 35u;
    r[PR_EDX] = (unsigned int)(size_t)ta;
    return 1;
}

/* 0x2C round-robin carrier, branch at 0x48B574 in the round robin's full-state tail, in place
   of stock's mov [esp+0x17],ax. ax = the 15-bit carrier index read at 0x48B56B; the sender
   writes its carrier's own +0xA8 (0x48B300..0x48B321). The tail packs it as the parent id (+3)
   of the attach record that 0x48AB70 applies at 0x48B58B and scales into the array unbounded
   (0x48ABAF..0x48ABC4); its 0 is 0x48AB70's own no parent (0x48ABA9), the detach the tail
   itself builds at 0x48B5B5. Writes the record's word as stock does -- the index, or for one
   past the array 0, counted -- leaves the same in ax, and continues -> 0x48B579. */
static int __cdecl wire_2c_carrier(unsigned int* r)
{
    char* ta = *(char* const*)0x00511DE8;
    unsigned int idx = r[PR_EAX] & 0xFFFF, max, keep;
    unsigned short word;
    wire_units(ta, &max);
    keep = wire_ref_idx(idx, max);
    if (keep != idx)
        wire_drop(&s_wire2CCarrier, "a 0x2C round robin names a carrier past the array; no "
                  "carrier", idx, max);
    word = (unsigned short)keep;
    memcpy(WPN_ESP_JMP(r) + 0x17, &word, sizeof word);
    r[PR_EAX] = (r[PR_EAX] & 0xFFFF0000u) | keep;
    return 1;
}

/* ---- stub emission, into fix_code's pages ------------------------------------------------ */

enum {
    WIRE_PRO_LEN   = 10,                        /* pushad; push esp; call; add esp,4        */
    WIRE_EMIT1_LEN = WIRE_PRO_LEN + 1 + 5,      /* popad; jmp                                */
    WIRE_EMIT2_LEN = WIRE_PRO_LEN + 2 + 1 + 6 + 5,
    WIRE_EMIT3_LEN = WIRE_PRO_LEN + 3 + 1 + 6 + 6 + 5,
    WIRE_TAIL_LEN  = WIRE_PRO_LEN + 2 + 1 + 6 + 5,       /* plus the displaced bytes       */
    WIRE_STOP_LEN  = 8 + 8 + 8 + 5
};

/* pushad; push esp; call fn; add esp,4 — the shared prologue; eax holds fn's return */
static unsigned char* wire_pro(unsigned char* p, int (__cdecl *fn)(unsigned int*))
{
    *p++ = 0x60;
    *p++ = 0x54;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)fn); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    return p;
}

/* two-way: test before popad (flags survive it), jz drop else jmp cont */
static unsigned char* wire_emit2(unsigned char* p, int (__cdecl *fn)(unsigned int*),
                                 unsigned int drop, unsigned int cont)
{
    p = wire_pro(p, fn);
    *p++ = 0x85; *p++ = 0xC0;                                  /* test eax,eax   */
    *p++ = 0x61;                                               /* popad          */
    *p++ = 0x0F; *p++ = 0x84; tagpu_detour_rel(p, drop); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, cont); p += 4;
    return p;
}

/* one-way: always continue (the killer's bound never drops; the receive's note) */
static unsigned char* wire_emit1(unsigned char* p, int (__cdecl *fn)(unsigned int*),
                                 unsigned int cont)
{
    p = wire_pro(p, fn);
    *p++ = 0x61;                                               /* popad          */
    *p++ = 0xE9; tagpu_detour_rel(p, cont); p += 4;
    return p;
}

/* three-way: cmp eax,1 before popad, then jb t0 / je t1 / jmp t2 (0/1/2) */
static unsigned char* wire_emit3(unsigned char* p, int (__cdecl *fn)(unsigned int*),
                                 unsigned int t0, unsigned int t1, unsigned int t2)
{
    p = wire_pro(p, fn);
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x01;                     /* cmp eax,1      */
    *p++ = 0x61;                                               /* popad          */
    *p++ = 0x0F; *p++ = 0x82; tagpu_detour_rel(p, t0); p += 4; /* jb  t0 (==0)   */
    *p++ = 0x0F; *p++ = 0x84; tagpu_detour_rel(p, t1); p += 4; /* je  t1 (==1)   */
    *p++ = 0xE9; tagpu_detour_rel(p, t2); p += 4;              /* jmp t2 (==2)   */
    return p;
}

/* two-way over displaced code: jz drop, else the n stock bytes the branch replaced (no
   relative operand among them) and on at cont */
static unsigned char* wire_emit_tail(unsigned char* p, int (__cdecl *fn)(unsigned int*),
                                     unsigned int drop, const unsigned char* bytes, int n,
                                     unsigned int cont)
{
    p = wire_pro(p, fn);
    *p++ = 0x85; *p++ = 0xC0;                                  /* test eax,eax   */
    *p++ = 0x61;                                               /* popad          */
    *p++ = 0x0F; *p++ = 0x84; tagpu_detour_rel(p, drop); p += 4;
    memcpy(p, bytes, (size_t)n); p += n;
    *p++ = 0xE9; tagpu_detour_rel(p, cont); p += 4;
    return p;
}

/* the 0x2C stop stub: registers already restored, esp is the receiver's own; point the reader
   (buf,dword,bit at [esp+0x10..0x18]) at s_wireZero and jump to the engine's end-of-list. */
static unsigned char* wire_emit_stop(unsigned char* p)
{
    unsigned int z = (unsigned int)(size_t)&s_wireZero;
    *p++ = 0xC7; *p++ = 0x44; *p++ = 0x24; *p++ = 0x10; memcpy(p, &z, 4); p += 4;
    *p++ = 0xC7; *p++ = 0x44; *p++ = 0x24; *p++ = 0x14; memset(p, 0, 4); p += 4;
    *p++ = 0xC7; *p++ = 0x44; *p++ = 0x24; *p++ = 0x18; memset(p, 0, 4); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0048BA28u); p += 4;
    return p;
}

/* ---- install: verify every stock span, build stubs, write the branches all-or-nothing ---- */

typedef struct WIRESPAN { unsigned int va; int n; const unsigned char* stock; } WIRESPAN;

static unsigned int s_wireStubBytes;

static unsigned char* wire_code(unsigned int n)
{
    unsigned char* p = fix_code(n);
    if (p) s_wireStubBytes += (n + 15u) & ~15u;
    return p;
}

static int fix_wire_bounds(void)
{
    /* the whole stock span of each site, verified before any write */
    static const unsigned char k09[41]  = {
        0x66,0x8B,0x47,0x03,0x66,0x85,0xC0,0x89,0x54,0x24,0x20,0x75,0x04,0x33,0xF6,0xEB,
        0x18,0x25,0xFF,0xFF,0x00,0x00,0x8B,0xC8,0xC1,0xE0,0x03,0x2B,0xC1,0x8B,0x8B,0x57,
        0x43,0x01,0x00,0x8D,0x04,0x80,0x8D,0x34,0xC1 };
    static const unsigned char k0c[38]  = {
        0x66,0x8B,0x43,0x01,0x57,0x66,0x85,0xC0,0x75,0x04,0x33,0xF6,0xEB,0x18,0x25,0xFF,
        0xFF,0x00,0x00,0x8B,0xC8,0xC1,0xE0,0x03,0x2B,0xC1,0x8B,0x8A,0x57,0x43,0x01,0x00,
        0x8D,0x04,0x80,0x8D,0x34,0xC1 };
    static const unsigned char k0ck[37] = {
        0x66,0x8B,0x43,0x07,0x66,0x85,0xC0,0x75,0x04,0x33,0xC0,0xEB,0x18,0x25,0xFF,0xFF,
        0x00,0x00,0x8B,0xC8,0xC1,0xE0,0x03,0x2B,0xC1,0x8B,0x8A,0x57,0x43,0x01,0x00,0x8D,
        0x04,0x80,0x8D,0x04,0xC1 };
    static const unsigned char k0b[74]  = {
        0x66,0x8B,0x47,0x01,0x66,0x85,0xC0,0x75,0x04,0x33,0xF6,0xEB,0x18,0x25,0xFF,0xFF,
        0x00,0x00,0x8B,0xC8,0xC1,0xE0,0x03,0x2B,0xC1,0x8B,0x8A,0x57,0x43,0x01,0x00,0x8D,
        0x04,0x80,0x8D,0x34,0xC1,0x66,0x8B,0x47,0x03,0x66,0x85,0xC0,0x75,0x04,0x33,0xDB,
        0xEB,0x18,0x25,0xFF,0xFF,0x00,0x00,0x8B,0xC8,0xC1,0xE0,0x03,0x2B,0xC1,0x8B,0x8A,
        0x57,0x43,0x01,0x00,0x8D,0x04,0x80,0x8D,0x1C,0xC1 };
    static const unsigned char ken[14] = {
        0x8B,0x47,0x67,0x89,0x6F,0x18,0x3B,0xC6,0x0F,0x84,0x5A,0x01,0x00,0x00 };
    static const unsigned char kdl[24] = {
        0x0F,0xBF,0xC8,0x8B,0x57,0x67,0x8B,0xC1,0xC1,0xE0,0x03,0x2B,0xC1,0x8D,0x0C,0x80,
        0xA1,0xE8,0x1D,0x51,0x00,0x8D,0x34,0xCA };
    static const unsigned char kty[9]  = { 0x66,0x39,0x86,0xA6,0x00,0x00,0x00,0x74,0x4F };
    static const unsigned char kaf[8]  = { 0x8B,0x06,0x8B,0x08,0x8D,0x44,0x24,0x10 };
    static const unsigned char krem[14]= {
        0x33,0xC9,0x8B,0xC5,0x66,0x8B,0x8A,0xE6,0x7E,0x03,0x00,0x99,0xF7,0xF9 };
    static const unsigned char krr[7]  = { 0x33,0xED,0x66,0x3B,0xC5,0x75,0x2A };
    static const unsigned char krra[6] = { 0x8B,0x87,0x9E,0x00,0x00,0x00 };
    static const unsigned char kfl[8]  = { 0x8B,0x4C,0x24,0x18,0x8B,0x44,0x24,0x10 };
    /* the receive's two statistics calls with the pushes that make their (code, len, 0) */
    static const unsigned char krx1[21] = {
        0x8B,0x54,0x24,0x00,0x6A,0x00,0x52,0x8B,0x88,0x38,0x2A,0x00,0x00,0x8A,0x11,0x52,
        0xE8,0x56,0x29,0xFC,0xFF };
    static const unsigned char krx2[21] = {
        0x8B,0x44,0x24,0x00,0x6A,0x00,0x50,0x8B,0x91,0x38,0x2A,0x00,0x00,0x8A,0x02,0x50,
        0xE8,0xCC,0x28,0xFC,0xFF };
    /* the frame the note writes: 0x4534E0's one push, and the pump's pointer and call */
    static const unsigned char kpro[1] = { 0x51 };
    static const unsigned char kpump[9] = { 0x89,0x4C,0x24,0x10,0xE8,0x47,0xF7,0xFF,0xFF };
    /* the 0x2C receiver's reader set-up, and the splitter's two length points */
    static const unsigned char kcp[8]  = { 0x33,0xF6,0x6A,0x08,0x8D,0x4C,0x24,0x14 };
    static const unsigned char ksc[5]  = { 0x25,0xFF,0xFF,0x00,0x00 };
    static const unsigned char ksw[8]  = { 0x8B,0xD0,0x81,0xE2,0xFF,0xFF,0x00,0x00 };
    static const unsigned char kscode[8] = { 0x8A,0x06,0x3C,0x01,0x88,0x44,0x24,0x28 };
    static const unsigned char kswcode[8] = { 0x8A,0x06,0x3C,0x01,0x88,0x44,0x24,0x2C };
    /* the move payload's target, from stock's zero test to the reference set's call */
    static const unsigned char ktg[47] = {
        0x66,0x3B,0xC5,0x75,0x04,0x33,0xC0,0xEB,0x1E,0x8B,0x15,0xE8,0x1D,0x51,0x00,0x25,
        0xFF,0xFF,0x00,0x00,0x8B,0xC8,0xC1,0xE0,0x03,0x2B,0xC1,0x8D,0x0C,0x80,0x8B,0x82,
        0x57,0x43,0x01,0x00,0x8D,0x04,0xC8,0x50,0x8B,0xCB,0xE8,0x8C,0xB5,0x03,0x00 };
    /* the round robin's attach record, from the child id to the call of 0x48AB70 */
    static const unsigned char kcr[53] = {
        0x66,0x8B,0x8F,0xA8,0x00,0x00,0x00,0x6A,0x0F,0x66,0x89,0x4C,0x24,0x15,0x8B,0xCE,
        0xE8,0x50,0xA8,0xF8,0xFF,0x6A,0x08,0x8B,0xCE,0x66,0x89,0x44,0x24,0x17,0xE8,0xE2,
        0xA8,0xF8,0xFF,0x8D,0x54,0x24,0x10,0x88,0x5C,0x24,0x16,0x52,0x88,0x44,0x24,0x19,
        0xE8,0xE0,0xF5,0xFF,0xFF };
    /* the 0x0A case (mov eax,[esp+0x10]; push eax; call 0x48AB70; jmp 0x455F50) and the jump
       table's entry that makes it the 0x0A case */
    static const unsigned char k0a[15] = {
        0x8B,0x44,0x24,0x10,0x50,0xE8,0x68,0x57,0x03,0x00,0xE9,0x43,0x0B,0x00,0x00 };
    static const unsigned char k0atab[4] = { 0xFE,0x53,0x45,0x00 };
    static const WIRESPAN span[] = {
        { 0x004861F7, 41, k09 }, { 0x004866E0, 38, k0c }, { 0x00486753, 37, k0ck },
        { 0x00489CED, 74, k0b }, { 0x0048B960, 14, ken }, { 0x0048B985, 24, kdl },
        { 0x0048B9AD,  9, kty }, { 0x0048BA05,  8, kaf }, { 0x0048BA9F, 14, krem },
        { 0x0048B40E,  7, krr }, { 0x0048B49C,  6, krra }, { 0x0048BA5E,  8, kfl },
        { 0x00453585, 21, krx1 }, { 0x0045360F, 21, krx2 }, { 0x004534E0, 1, kpro },
        { 0x00453D90,  9, kpump }, { 0x0048B92B,  8, kcp }, { 0x00463939,  5, ksc },
        { 0x00463B33,  8, ksw }, { 0x004638F0,  8, kscode }, { 0x00463AD5,  8, kswcode },
        { 0x0044E0D5, 47, ktg }, { 0x0048B55B, 53, kcr }, { 0x004553FE, 15, k0a },
        { 0x00455FA4,  4, k0atab },
    };
    static const unsigned char loads[8] = { 0x8B,0x4C,0x24,0x18,0x8B,0x44,0x24,0x10 };
    /* S8, the unsigned remainder, patched in place (movzx ecx,[edx+0x37EE6]; mov eax,ebp;
       xor edx,edx; div ecx; nop) */
    static const unsigned char remNow[14] = {
        0x0F,0xB7,0x8A,0xE6,0x7E,0x03,0x00,0x8B,0xC5,0x33,0xD2,0xF7,0xF1,0x90 };

    unsigned char *stop, *a09, *a0c, *a0ck, *a0b, *aen, *adl, *aty, *aaf, *arr, *arra, *afl, *arx;
    unsigned char *acp, *asc, *asw, *atg, *acr, *a0a;
    FIXSITE s[20];
    int i, n = 0;

    for (i = 0; i < (int)(sizeof span / sizeof span[0]); i++)
        if (memcmp((const void*)(size_t)span[i].va, span[i].stock, (size_t)span[i].n) != 0) {
            tagpu_logf("enginefix: wire robustness SKIPPED (%08X not stock)", span[i].va);
            return FIX_BYTES;
        }

    if (s_wireTlsBuf == TLS_OUT_OF_INDEXES) s_wireTlsBuf = TlsAlloc();
    if (s_wireTlsLen == TLS_OUT_OF_INDEXES) s_wireTlsLen = TlsAlloc();
    if (s_wireTlsCopy == TLS_OUT_OF_INDEXES) s_wireTlsCopy = TlsAlloc();
    if (s_wireTlsBuf == TLS_OUT_OF_INDEXES || s_wireTlsLen == TLS_OUT_OF_INDEXES ||
        s_wireTlsCopy == TLS_OUT_OF_INDEXES)
        return FIX_STUB;

    if (!(stop = wire_code(WIRE_STOP_LEN)) || !(a09 = wire_code(WIRE_EMIT2_LEN)) ||
        !(a0c = wire_code(WIRE_EMIT2_LEN)) || !(a0ck = wire_code(WIRE_EMIT1_LEN)) ||
        !(a0b = wire_code(WIRE_EMIT2_LEN)) || !(aen = wire_code(WIRE_EMIT2_LEN)) ||
        !(adl = wire_code(WIRE_EMIT2_LEN)) || !(aty = wire_code(WIRE_EMIT3_LEN)) ||
        !(aaf = wire_code(WIRE_EMIT2_LEN)) || !(arr = wire_code(WIRE_EMIT3_LEN)) ||
        !(arra = wire_code(WIRE_EMIT2_LEN)) || !(afl = wire_code(WIRE_TAIL_LEN + 8)) ||
        !(arx = wire_code(WIRE_EMIT1_LEN)) || !(acp = wire_code(WIRE_TAIL_LEN + 8)) ||
        !(asc = wire_code(WIRE_TAIL_LEN + 5)) || !(asw = wire_code(WIRE_TAIL_LEN + 8)) ||
        !(atg = wire_code(WIRE_EMIT1_LEN)) || !(acr = wire_code(WIRE_EMIT1_LEN)) ||
        !(a0a = wire_code(WIRE_TAIL_LEN + 5)))
        return FIX_STUB;
    wire_emit_stop(stop);
    wire_emit2(a09, wire_s09, 0x0048622Bu, 0x00486220u);
    wire_emit2(a0c, wire_s0c, 0x00486E59u, 0x00486706u);
    wire_emit1(a0ck, wire_s0ckill, 0x00486778u);
    wire_emit2(a0b, wire_s0b, 0x00489F93u, 0x00489D37u);
    wire_emit2(aen, wire_s2c_entry, 0x0048BAC8u, 0x0048B96Eu);
    wire_emit2(adl, wire_s2c_delta, (unsigned int)(size_t)stop, 0x0048B99Du);
    wire_emit3(aty, wire_s2c_type, (unsigned int)(size_t)stop, 0x0048B9B6u, 0x0048BA05u);
    wire_emit2(aaf, wire_s2c_after, (unsigned int)(size_t)stop, 0x0048BA0Du);
    wire_emit3(arr, wire_s2c_rr, 0x0048B435u, 0x0048B415u, 0x0048B43Fu);
    wire_emit2(arra, wire_s2c_rr_after, 0x0048B435u, 0x0048B4A2u);
    wire_emit_tail(afl, wire_s2c_flag, 0x0048BAC8u, loads, 8, 0x0048BA66u);
    wire_emit1(arx, wire_rx_note, 0x00415EF0u);
    wire_emit_tail(acp, wire_s2c_copy, 0x0048BAC8u, kcp, 8, 0x0048B933u);
    wire_emit_tail(asc, wire_split_count, 0x00463949u, ksc, 5, 0x0046393Eu);
    wire_emit_tail(asw, wire_split_walk, 0x00463B91u, ksw, 8, 0x00463B3Bu);
    wire_emit1(atg, wire_2c_target, 0x0044E0FCu);
    wire_emit1(acr, wire_2c_carrier, 0x0048B579u);
    wire_emit_tail(a0a, wire_s0a, 0x00455F50u, k0a, 5, 0x00455403u);

    /* the branch each site takes, at its clean boundary; the stock bytes were verified above.
       The last entry of s is S8's, so a site past the one before it is refused, not written. */
#define WIRE_SITE(va_, n_, op_, to_) do { \
    if (n >= (int)(sizeof s / sizeof s[0]) - 1) return FIX_STUB; \
    s[n].va = (va_); s[n].n = (n_); memcpy(s[n].was, (const void*)(size_t)(va_), (n_)); \
    fix_branch(&s[n], (op_), (to_)); n++; \
} while (0)
    WIRE_SITE(0x004861F7, 5, 0xE9, a09);
    WIRE_SITE(0x004866E5, 5, 0xE9, a0c);
    WIRE_SITE(0x00486753, 5, 0xE9, a0ck);
    WIRE_SITE(0x00489CED, 5, 0xE9, a0b);
    WIRE_SITE(0x0048B960, 5, 0xE9, aen);
    WIRE_SITE(0x0048B985, 5, 0xE9, adl);
    WIRE_SITE(0x0048B9AD, 5, 0xE9, aty);
    WIRE_SITE(0x0048BA05, 5, 0xE9, aaf);
    WIRE_SITE(0x0048B40E, 5, 0xE9, arr);
    WIRE_SITE(0x0048B49C, 6, 0xE9, arra);
    WIRE_SITE(0x0048BA5E, 8, 0xE9, afl);
    WIRE_SITE(0x00453595, 5, 0xE8, arx);
    WIRE_SITE(0x0045361F, 5, 0xE8, arx);
    WIRE_SITE(0x0048B92B, 8, 0xE9, acp);
    WIRE_SITE(0x00463939, 5, 0xE9, asc);
    WIRE_SITE(0x00463B33, 8, 0xE9, asw);
    WIRE_SITE(0x0044E0DE, 5, 0xE9, atg);
    WIRE_SITE(0x0048B574, 5, 0xE9, acr);
    WIRE_SITE(0x004553FE, 5, 0xE9, a0a);
#undef WIRE_SITE
    s[n].va = 0x0048BA9F; s[n].n = 14;                         /* S8, in place */
    memcpy(s[n].was, krem, 14); memcpy(s[n].now, remNow, 14); n++;
    return fix_write(s, n);
}

/* the heartbeat's wire section, read by tagpu_packet_pub.c's extra() on the render thread:
   DLL counters only, each a u32 the game thread alone writes */
int tagpu_wire_format(char* buf, unsigned int cap)
{
    return _snprintf(buf, cap,
                     " | wire: in 09=%u 0a=%u 0b=%u 0c=%u 0d=%u 2c=%u dirty=%u create=%u rr=%u"
                     " drop 09=%u blk=%u 0a=%u 0b=%u 0c=%u kill=%u 2c=%u len=%u stale=%u nocopy=%u"
                     " 0d=%u split=%u target=%u carrier=%u"
                     " morph=%u dcreate=%u rcreate=%u ghost=%u argdiff=%u pump=%u"
                     " noblock 09=%u dirty=%u rr=%u noarr=%u",
                     s_wireIn09, s_wireIn0A, s_wireIn0B, s_wireIn0C, s_wireIn0D, s_wireIn2C,
                     s_wireIn2CDirty, s_wireIn2CCreate, s_wireIn2CRR,
                     s_wire09, s_wire09Blk, s_wire0A, s_wire0B, s_wire0C, s_wire0Ckill, s_wire2C,
                     s_wire2CLen, s_wire2CStale, s_wire2CNoCopy, s_wireWpnxDrops,
                     (unsigned int)s_wireSplit, s_wire2CTarget, s_wire2CCarrier,
                     s_wireMorph, s_wireDCreate, s_wireRCreate, s_wireGhost, s_wireArgDiff,
                     (unsigned int)s_wirePump,
                     s_wireNoBlk09, s_wireNoBlkDirty, s_wireNoBlkRR, s_wireNoArr);
}

/* tagpu_wirecheck.on: a unit test of the C predicates on boundary values (0, the last valid
   index, one past, the receivers' own sentinels), each verdict logged against the expected.
   The compiler folds every case to a constant, so this checks the predicates' C and nothing
   else: the stubs and the drop paths rest on the disassembly. */
static void wire_selfcheck(void)
{
    struct { const char* name; int got; int want; } t[40];
    int n = 0, bad = 0, i;
    t[n].name = "index 0 rejected";        t[n].got = wire_index_ok(0, 100);      t[n].want = 0; n++;
    t[n].name = "index 1 accepted";        t[n].got = wire_index_ok(1, 100);      t[n].want = 1; n++;
    t[n].name = "index max accepted";      t[n].got = wire_index_ok(100, 100);    t[n].want = 1; n++;
    t[n].name = "index max+1 rejected";    t[n].got = wire_index_ok(101, 100);    t[n].want = 0; n++;
    t[n].name = "killer 0 accepted";       t[n].got = wire_killer_ok(0, 100);     t[n].want = 1; n++;
    t[n].name = "killer max accepted";     t[n].got = wire_killer_ok(100, 100);   t[n].want = 1; n++;
    t[n].name = "killer max+1 rejected";   t[n].got = wire_killer_ok(101, 100);   t[n].want = 0; n++;
    t[n].name = "type 0 rejected";         t[n].got = wire_type_ok(0, 50);        t[n].want = 0; n++;
    t[n].name = "type 1 accepted";         t[n].got = wire_type_ok(1, 50);        t[n].want = 1; n++;
    t[n].name = "type count-1 accepted";   t[n].got = wire_type_ok(49, 50);       t[n].want = 1; n++;
    t[n].name = "type count rejected";     t[n].got = wire_type_ok(50, 50);       t[n].want = 0; n++;
    t[n].name = "delta -1 rejected";       t[n].got = wire_delta_ok(-1, 500);     t[n].want = 0; n++;
    t[n].name = "delta 0 accepted";        t[n].got = wire_delta_ok(0, 500);      t[n].want = 1; n++;
    t[n].name = "delta N-1 accepted";      t[n].got = wire_delta_ok(499, 500);    t[n].want = 1; n++;
    t[n].name = "delta N rejected";        t[n].got = wire_delta_ok(500, 500);    t[n].want = 0; n++;
    t[n].name = "bits to the end accepted"; t[n].got = wire_bits_ok(56, 16, 72);  t[n].want = 1; n++;
    t[n].name = "bits past the end rejected"; t[n].got = wire_bits_ok(57, 16, 72); t[n].want = 0; n++;
    t[n].name = "split length 0 rejected";  t[n].got = wire_split_ok(0x0B, 0);     t[n].want = 0; n++;
    t[n].name = "split length 1 accepted";  t[n].got = wire_split_ok(0x0B, 1);     t[n].want = 1; n++;
    t[n].name = "split 0x2C of 6 rejected"; t[n].got = wire_split_ok(0x2C, 6);     t[n].want = 0; n++;
    t[n].name = "split 0x2C of 7 accepted"; t[n].got = wire_split_ok(0x2C, 7);     t[n].want = 1; n++;
    t[n].name = "ref 0 stays no unit";      t[n].got = (int)wire_ref_idx(0, 100);      t[n].want = 0; n++;
    t[n].name = "ref 1 kept";               t[n].got = (int)wire_ref_idx(1, 100);      t[n].want = 1; n++;
    t[n].name = "ref max kept";             t[n].got = (int)wire_ref_idx(100, 100);    t[n].want = 100; n++;
    t[n].name = "ref max+1 made no unit";   t[n].got = (int)wire_ref_idx(101, 100);    t[n].want = 0; n++;
    t[n].name = "ref 0xFFFF made no unit";  t[n].got = (int)wire_ref_idx(0xFFFF, 100); t[n].want = 0; n++;
    t[n].name = "ref with no array no unit"; t[n].got = (int)wire_ref_idx(1, 0);       t[n].want = 0; n++;
    t[n].name = "attach detach accepted";   t[n].got = wire_attach_ok(1, 0, 100);     t[n].want = 1; n++;
    t[n].name = "attach max to max accepted"; t[n].got = wire_attach_ok(100, 100, 100); t[n].want = 1; n++;
    t[n].name = "attach child 0 rejected";  t[n].got = wire_attach_ok(0, 5, 100);     t[n].want = 0; n++;
    t[n].name = "attach child max+1 rejected"; t[n].got = wire_attach_ok(101, 5, 100); t[n].want = 0; n++;
    t[n].name = "attach parent max+1 rejected"; t[n].got = wire_attach_ok(5, 101, 100); t[n].want = 0; n++;
    {   /* the block-shape predicate on a synthetic array: begin, N=4, two players */
        char base[1];
        const char* begin = base;
        const char* arr_last = begin + (4 * 2 + 1 - 1) * 0x118;   /* 10*N+1 shrunk to 2 players */
        const char* p0 = begin + 1 * 0x118;                       /* player 0: slot 1 */
        const char* p0l = p0 + 3 * 0x118;
        int ok = wire_block_ok(p0, p0l, begin, arr_last, 4) &&
                 !wire_block_ok(begin, begin + 3 * 0x118, begin, arr_last, 4) &&   /* slot 0 */
                 !wire_block_ok(p0 + 1, p0l, begin, arr_last, 4);                  /* unaligned */
        t[n].name = "block shape ok/rejects"; t[n].got = ok; t[n].want = 1; n++;
    }
    for (i = 0; i < n; i++) {
        if (t[i].got != t[i].want) bad++;
        tagpu_logf("enginefix: wirecheck %-26s got=%d want=%d %s",
                   t[i].name, t[i].got, t[i].want, t[i].got == t[i].want ? "OK" : "FAIL");
    }
    tagpu_logf("enginefix: wirecheck %d predicate cases, %d failed", n, bad);
}

/* ===== DAMAGE: THE VICTIM CAPS, FLAK'S DIVIDES, THE MAP'S EDGES ============================
   Landing B1 of research/notes/tadr-port/sim-fixes.md; the disassembly is in
   sim-fixes-evidence.md, Part 2 §1, §4 and §11, and exe-reverse-engineering.md, "Engine
   defects we patch". TADR's AreaDamageOverflow (the unit cap) and OffMapAircraft (the shear)
   are the prior art; the code is ours. */

/* THE VICTIM CAPS [DISASSEMBLED]. Area damage 0x49A120(proj, at), __stdcall, walks the cells
   of the blast's rect once: for each cell its two unit slots (0x49A214..0x49A421), then the
   cell's feature (0x49A427..0x49A62B), then the next cell (0x49A62F..0x49A645). It keeps two
   lists on its stack, so that a victim standing on several cells is damaged once: 20 unit
   pointers ([esp+0xA0], count [esp+0x98]) and 64 anchor cells ([esp+0xF0], count [esp+0x9C]).
   Each list records only while it has room (0x49A28F `cmp ecx,0x14`, 0x49A5FA `cmp esi,0x40`),
   and the damage runs whether or not the victim was recorded: the unit block falls through to
   its damage at 0x49A2AA, the feature block to its call of 0x4244B0 at 0x49A615. So a unit
   found after the twentieth, or an anchor after the sixty-fourth, is damaged (or, on a peer
   that is not the host, reported with 0x0F) once for every cell of it inside the rect -- up to
   nine times for a 3x3 structure under a commander's blast.

   THE FIX replaces the two list blocks with calls that answer "seen: skip" or "record:
   continue" from sets with no capacity, keyed by the unit's slot and the anchor's ordinal. The
   calls sit where stock's lists are consulted -- a unit on its first cell, before its distance
   test; a feature only on a cell that passed its distance test (0x49A5C8) -- so below the caps
   every record and every skip is stock's, and only the repeats past them go. The damage math
   and every value are stock's. The sets live in a frame per call of 0x49A120: the wrapper on
   both of its calls (0x49A0A9 in 0x499EB0, 0x49A109 in the fire spread's 0x49A0C0; no other
   reference to 0x49A120 exists [call and literal scan of the image]) holds the frame on its
   own C stack. After the walk, a weapon with w+0x111 bit 30 (0x49A66F) detonates the
   projectiles inside its blast through 0x499EB0 (0x49A764), which calls 0x49A120 again from
   inside it; that call has a frame of its own.

   THE INVARIANT: one explosion damages a unit at most once and reports a feature at most
   once, and every index is bounded before it is used. It rests on
     - a bound: a unit is recorded by its slot, (unit - begin) / 0x118 with the remainder 0 and
       the slot below the array's count (u16 main+0x14351, 10 * units a player + 1, set with
       the array at 0x4854EF); an anchor by its ordinal, (cell - grid) / 13 with the remainder 0
       and the ordinal below W * H (main+0x14233, +0x14237). A pointer that fails either is not
       damaged: stock would read through it;
     - a lifetime, per thread: a call's frame is a local of its wrapper, so it exists exactly
       while that call runs, and the innermost frame is a thread-local pointer (a TLS slot)
       whose previous value the wrapper keeps and puts back. So a list block reads the frame of
       the innermost call on its own thread, whatever runs on any other. That matters: besides
       the projectile tick and the fire spread, 0x499EB0 is reached from the 0x0E receiver
       (0x49AFEB). That receiver runs on the game thread only: 0x451FD0 gives 0x0E the
       receive mask 4 (0x45200B), which the dispatcher passes only in net state 6
       (0x454762..0x45478B), set at 0x498445 on the game thread after the load. The frames
       are per thread all the same, which costs nothing and asks no argument about who
       calls 0x49A120.
   The sets grow through eng_alloc_or_exit, whose failure is the engine's own out-of-memory
   exit whatever the handler slot holds, so no answer is ever given from a set that could not
   hold it.
   A list block with no frame cannot run: both calls of 0x49A120 and both blocks are rows of the
   one fail-closed table, written together or not at all, and nothing else calls 0x49A120. If
   it ever did, the block counts it, logs it once and damages as though the victim were new.
   CLASS: simulation, fail closed. B2 serves stacked aircraft through the same unit set. */
#define DMG_UNIT_CAP   20u             /* stock's list, 0x49A28F                            */
#define DMG_FEAT_CAP   64u             /* stock's list, 0x49A5FA                            */
#define DMG_KEY_PAST   0x80000000u     /* a key recorded past stock's list                  */
#define DMG_INLINE     64u             /* slots of a set in the frame: 32 keys before it grows */
#define DMG_CELL       13u
#define DMG_UNIT_STRIDE 0x118u

/* open addressing over value + 1 (0 = empty), DMG_KEY_PAST or'd in; never more than half full */
typedef struct DMGSET {
    unsigned int* key;                  /* inl, or a block from 0x4D83B0 once it outgrows it */
    unsigned int  n, cap;               /* cap a power of two                                */
    unsigned int  inl[DMG_INLINE];
} DMGSET;

/* the stacked aircraft one call serves after stock's walk (the stacked-aircraft block below) */
typedef struct DMGAIR {
    int xs, xe, zs, ze;                 /* the blast's cell rect, as 0x49A120 clamps it      */
    unsigned short* idx;                /* inl, or a block from 0x4D83B0 once it outgrows it */
    unsigned int n, next, cap;
    unsigned short inl[32];
} DMGAIR;

typedef struct DMGSEEN {
    struct DMGSEEN* outer;              /* this thread's enclosing call's frame, or NULL     */
    DMGSET unit, feat;
    DMGAIR air;
} DMGSEEN;

static DWORD s_dmgTls = TLS_OUT_OF_INDEXES;
/* peekable counters (their addresses are in the enginefix line), any thread */
static volatile LONG s_dmgUnitRepeats;  /* a victim past stock's 20 found again   */
static volatile LONG s_dmgFeatRepeats;  /* an anchor past stock's 64 found again  */
static volatile LONG s_dmgRefused;      /* a pointer outside its array            */
static volatile LONG s_dmgUnframed;     /* a list block run outside the wrapper   */
static const char s_dmgTag[] = "tagpu damage seen-set";

static void dmg_set_init(DMGSET* t)
{
    t->key = t->inl;
    t->n = 0;
    t->cap = DMG_INLINE;
    memset(t->inl, 0, sizeof t->inl);
}

static void dmg_set_free(DMGSET* t)
{
    if (t->key != t->inl) ENG_FREE(t->key);
}

static unsigned int dmg_hash(unsigned int key, unsigned int mask)
{
    unsigned int h = key * 0x9E3779B1u;
    return (h ^ (h >> 16)) & mask;
}

/* the table doubled and its keys moved */
static void dmg_set_grow(DMGSET* t)
{
    unsigned int cap = t->cap * 2u, i, j;
    unsigned int* key = (unsigned int*)eng_alloc_or_exit(s_dmgTag, cap * 4u);
    memset(key, 0, cap * 4u);
    for (i = 0; i < t->cap; i++) {
        if (!t->key[i]) continue;
        for (j = dmg_hash(t->key[i] & ~DMG_KEY_PAST, cap - 1u); key[j]; j = (j + 1u) & (cap - 1u)) {}
        key[j] = t->key[i];
    }
    dmg_set_free(t);
    t->key = key;
    t->cap = cap;
}

/* 1 = already recorded by this call (a repeat past stock's list is counted); 0 = recorded now */
static int dmg_set_seen(DMGSET* t, unsigned int value, unsigned int stock_cap, volatile LONG* repeats)
{
    unsigned int key = value + 1u, j;
    if (2u * (t->n + 1u) > t->cap) dmg_set_grow(t);
    for (j = dmg_hash(key, t->cap - 1u); t->key[j]; j = (j + 1u) & (t->cap - 1u))
        if ((t->key[j] & ~DMG_KEY_PAST) == key) {
            if (t->key[j] & DMG_KEY_PAST) InterlockedIncrement(repeats);
            return 1;
        }
    t->key[j] = key | (t->n >= stock_cap ? DMG_KEY_PAST : 0u);
    t->n++;
    return 0;
}

static DMGSEEN* dmg_frame(void)
{
    DMGSEEN* f = (DMGSEEN*)TlsGetValue(s_dmgTls);
    if (!f && InterlockedIncrement(&s_dmgUnframed) == 1)
        tagpu_logf("enginefix: area damage's victim list ran outside its wrapper; its victims are "
                   "not deduplicated (counted at 0x%08X)", (unsigned int)(size_t)&s_dmgUnframed);
    return f;
}

/* In place of 0x49A262..0x49A2A9, esi the unit: 1 = seen, skip it (0x49A415); 0 = recorded,
   damage it (0x49A2AA). */
static int __stdcall dmg_unit_seen(const char* unit)
{
    DMGSEEN* f = dmg_frame();
    const char* ta = *(const char* const*)0x00511DE8;
    const char* begin;
    unsigned int count;
    size_t off;

    if (!f) return 0;
    begin = ta ? *(const char* const*)(ta + 0x14357) : NULL;
    count = ta ? *(const unsigned short*)(ta + 0x14351) : 0;
    if (!begin || unit < begin) { InterlockedIncrement(&s_dmgRefused); return 1; }
    off = (size_t)(unit - begin);
    if (off % DMG_UNIT_STRIDE || off / DMG_UNIT_STRIDE >= count) {
        InterlockedIncrement(&s_dmgRefused);
        return 1;
    }
    return dmg_set_seen(&f->unit, (unsigned int)(off / DMG_UNIT_STRIDE), DMG_UNIT_CAP,
                        &s_dmgUnitRepeats);
}

/* In place of 0x49A5CE..0x49A614, the anchor cell at [esp+0x10]: 1 = seen, skip it (0x49A62B);
   0 = recorded, hit it (0x49A615). */
static int __stdcall dmg_feature_seen(const unsigned char* cell)
{
    DMGSEEN* f = dmg_frame();
    const char* ta = *(const char* const*)0x00511DE8;
    const unsigned char* grid;
    unsigned long long cells;
    size_t off;

    if (!f) return 0;
    grid = ta ? *(const unsigned char* const*)(ta + 0x14287) : NULL;
    cells = ta ? (unsigned long long)*(const unsigned int*)(ta + 0x14233) *
                 *(const unsigned int*)(ta + 0x14237) : 0;
    if (!grid || cell < grid) { InterlockedIncrement(&s_dmgRefused); return 1; }
    off = (size_t)(cell - grid);
    if (off % DMG_CELL || off / DMG_CELL >= cells || off / DMG_CELL >= DMG_KEY_PAST - 1u) {
        InterlockedIncrement(&s_dmgRefused);
        return 1;
    }
    return dmg_set_seen(&f->feat, (unsigned int)(off / DMG_CELL), DMG_FEAT_CAP, &s_dmgFeatRepeats);
}

/* In place of both `call 0x49A120`. The engine's eax is handed back: 0x499EB0 and 0x49A0C0
   return it as their own. */
typedef unsigned int (__stdcall *dmg_area_fn)(void* proj, void* at);
static unsigned int __stdcall dmg_area(void* proj, void* at)
{
    DMGSEEN f;
    unsigned int r;
    f.outer = (DMGSEEN*)TlsGetValue(s_dmgTls);
    dmg_set_init(&f.unit);
    dmg_set_init(&f.feat);
    f.air.idx = f.air.inl;
    f.air.n = f.air.next = 0;
    f.air.cap = sizeof f.air.inl / sizeof f.air.inl[0];
    TlsSetValue(s_dmgTls, &f);
    r = ((dmg_area_fn)0x0049A120)(proj, at);
    TlsSetValue(s_dmgTls, f.outer);
    dmg_set_free(&f.unit);
    dmg_set_free(&f.feat);
    if (f.air.idx != f.air.inl) ENG_FREE(f.air.idx);
    return r;
}

static int fix_victim_caps(void)
{
    static const unsigned char callA[5] = { 0xE8, 0x72, 0x00, 0x00, 0x00 };   /* 0x49A0A9 */
    static const unsigned char callB[5] = { 0xE8, 0x12, 0x00, 0x00, 0x00 };   /* 0x49A109 */
    static const unsigned char units[72] = {
        0x8B, 0x8C, 0x24, 0x98, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x85, 0xC9, 0x7E,
        0x20, 0x8D, 0x8C, 0x24, 0xA0, 0x00, 0x00, 0x00, 0x39, 0x31, 0x0F, 0x84,
        0x97, 0x01, 0x00, 0x00, 0x8B, 0x94, 0x24, 0x98, 0x00, 0x00, 0x00, 0x40,
        0x83, 0xC1, 0x04, 0x3B, 0xC2, 0x7C, 0xE9, 0x8B, 0xCA, 0x83, 0xF9, 0x14,
        0x7D, 0x16, 0x89, 0xB4, 0x8C, 0xA0, 0x00, 0x00, 0x00, 0x8B, 0x84, 0x24,
        0x98, 0x00, 0x00, 0x00, 0x40, 0x89, 0x84, 0x24, 0x98, 0x00, 0x00, 0x00,
    };
    static const unsigned char feats[71] = {
        0x8B, 0xB4, 0x24, 0x9C, 0x00, 0x00, 0x00, 0x33, 0xC9, 0x85, 0xF6, 0x7E,
        0x1B, 0x8D, 0x94, 0x24, 0xF0, 0x00, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x10,
        0x8B, 0x3A, 0x3B, 0xF8, 0x74, 0x3F, 0x41, 0x83, 0xC2, 0x04, 0x3B, 0xCE,
        0x7C, 0xEE, 0xEB, 0x04, 0x8B, 0x44, 0x24, 0x10, 0x83, 0xFE, 0x40, 0x7D,
        0x16, 0x89, 0x84, 0xB4, 0xF0, 0x00, 0x00, 0x00, 0x8B, 0x8C, 0x24, 0x9C,
        0x00, 0x00, 0x00, 0x41, 0x89, 0x8C, 0x24, 0x9C, 0x00, 0x00, 0x00,
    };
    unsigned char u[sizeof units], fe[sizeof feats], p[5];
    unsigned int rel;

    /* the frames' thread-local slot; without one the table is not written, as for a stub */
    if (s_dmgTls == TLS_OUT_OF_INDEXES) s_dmgTls = TlsAlloc();
    if (s_dmgTls == TLS_OUT_OF_INDEXES) { lim_no_stub(); return FIX_TABLE; }

    /* 0x49A262: push esi; call dmg_unit_seen; test eax,eax; jnz 0x49A415; jmp 0x49A2AA. The
       registers the loop keeps -- ebx the projectile, esi the unit, edi the cell -- are the
       callee's to preserve; eax, ecx and edx are dead at both exits, as after stock's block. */
    memset(u, 0x90, sizeof u);
    u[0] = 0x56;
    u[1] = 0xE8; rel = (unsigned int)(size_t)dmg_unit_seen - (0x0049A263u + 5u); memcpy(u + 2, &rel, 4);
    u[6] = 0x85; u[7] = 0xC0;
    u[8] = 0x0F; u[9] = 0x85; rel = 0x0049A415u - (0x0049A26Au + 6u); memcpy(u + 10, &rel, 4);
    u[14] = 0xEB; u[15] = (unsigned char)(0x0049A2AAu - (0x0049A270u + 2u));

    /* 0x49A5CE: mov eax,[esp+0x10]; push eax; call dmg_feature_seen; test eax,eax;
       jnz 0x49A62B; mov eax,[esp+0x10]; jmp 0x49A615 -- 0x49A615 passes eax, the anchor, to
       0x4244B0. esi and edi, which stock's block used, are reloaded before their next read. */
    memset(fe, 0x90, sizeof fe);
    fe[0] = 0x8B; fe[1] = 0x44; fe[2] = 0x24; fe[3] = 0x10;
    fe[4] = 0x50;
    fe[5] = 0xE8; rel = (unsigned int)(size_t)dmg_feature_seen - (0x0049A5D3u + 5u); memcpy(fe + 6, &rel, 4);
    fe[10] = 0x85; fe[11] = 0xC0;
    fe[12] = 0x75; fe[13] = (unsigned char)(0x0049A62Bu - (0x0049A5DAu + 2u));
    fe[14] = 0x8B; fe[15] = 0x44; fe[16] = 0x24; fe[17] = 0x10;
    fe[18] = 0xEB; fe[19] = (unsigned char)(0x0049A615u - (0x0049A5E0u + 2u));

    p[0] = 0xE8;
    rel = (unsigned int)(size_t)dmg_area - (0x0049A0A9u + 5u); memcpy(p + 1, &rel, 4);
    lim_add(0x0049A0A9, 5, callA, p, "area damage from a projectile, the call");
    rel = (unsigned int)(size_t)dmg_area - (0x0049A109u + 5u); memcpy(p + 1, &rel, 4);
    lim_add(0x0049A109, 5, callB, p, "area damage from the fire spread, the call");
    lim_add(0x0049A262, sizeof units, units, u, "area damage, the unit victims' list");
    lim_add(0x0049A5CE, sizeof feats, feats, fe, "area damage, the feature victims' list");
    return FIX_TABLE;
}

/* STACKED AIRCRAFT [DISASSEMBLED + MEASURED]. Landing B2 of research/notes/tadr-port/
   sim-fixes.md (evidence Part 2 §2; TADR's AreaDamageOverflow is the prior art, the design is
   ours). A feature-grid cell holds two unit slots: the grid stamp 0x47CC30 files a unit whose
   state +0x110 has (& 3) == 1 in slot A and one with == 2 -- airborne -- in slot B
   (0x47CF98), and a contested slot keeps one of them (the loser gets +0x110 bit 27). Area
   damage reads only the two slots of each cell (above), and so does the direct-hit test
   0x49B090. So of several aircraft over one spot, only the ones an in-rect slot names are ever
   found: the others -- holding no cell, or holding cells only outside the rect while others
   hold the ones inside it -- take no splash at all. MEASURED on the previous build: ten
   ARMATLAS ordered to one point, up to seven held no cell; a CORFLAK's first burst took the
   four holders from 150 HP to 5-11 and left the five holding none, each within a cell of
   them, at 150. On this build the same burst took all ten, six holding none, to 10-16.

   THE FIX serves the missing aircraft to the engine's own per-victim code AFTER stock's walk
   has finished, so stock's victims, their order and every value they take are unchanged, and
   the new victims go through stock's distance test, falloff and damage with no new arithmetic:
     - the walk's end 0x49A664 (the fall-through of the row loop, and 0x49A1DF's jump for an
       empty rect) jumps to a stub that asks air_first for the first candidate; with one, it
       puts the selector [esp+0x10] at 2 -- stock's selector is only ever 0 or 1 at 0x49A415 --
       and enters stock's unit block at 0x49A24E with esi the unit; with none, it runs the
       11 bytes it displaced and joins the tail at 0x49A66F;
     - the selector step 0x49A415 is stock's for 0 and 1 and, at 2, asks air_next for the next
       candidate, entering 0x49A24E again, or joins the tail when there is none. Every path
       from 0x49A24E (the NULL test, the shooter's skip 0x49A259, the seen-set's skip, the
       damage and its reload 0x49A411) comes back to 0x49A415, and none of them writes
       [esp+0x10]. The tail at 0x49A66F reloads every register it reads.
   A candidate goes through the seen-set block at 0x49A262 like any victim, and one the set
   already holds is never offered, so an aircraft stock found (it holds a slot the walk
   visited) is left to stock and a served one is damaged once.

   WHICH AIRCRAFT. air_first builds this call's list from the pool (below). A slot is a
   candidate when the unit in it
     - has the alive bit 0x10000000 and (& 3) == 2, and not bit 29 -- slot B's own rule: the
       stamp sends a bit-29 unit down its yardmap path to slot A (0x47CD5A) whatever its & 3;
     - is in the grid: its sort bucket +0x82 is neither NULL nor the off-map bucket
       *(main+0x142B7), and it is not pending death (bit 14). Stock never offers a dying
       aircraft to its own death explosion: the destructor's grid clear 0x47CBD0 (called at
       0x48682D) empties its cells and sets +0x82 to NULL (0x47CC19) before the explosion
       0x49B000 it calls at 0x486D50, whose projectile has no shooter (proj+0x52 = 0,
       0x49B03E), so the shooter's skip 0x49A259 would not keep it out. The bucket test is what
       excludes it in every case, since the clear always runs first. Bit 14 alone would not:
       the damage receiver skips that write when the owner's player record has a zero first
       dword (0x489ECC), and the owner gate 0x49A03F..0x49A047 then lets the explosion's area
       damage run (its projectile carries the dying unit's player, proj+0x66, 0x49B055); and a
       death that does not come through 0x489CE0 (Send_UnitDeath's direct callers) need not
       set it. On a peer that does not own the unit the gate skips the explosion's area damage
       whatever the bits;
     - is not carried (+0x86 is 0) and has its model (+0x9E not NULL: the engine map's death
       guard, 0x486D9E frees it before the alive bit clears);
     - has a footprint +0x76/+0x78, +0x7E/+0x80 of at least one cell each way that meets the
       blast's rect, and is not in this call's unit set.
   The rect is 0x49A120's own: the radius (u16)w[+0xD6] >> 1, c = radius/16 + 1 cells about the
   blast point's cell (x, z of `at` +0x02/+0x0A, each truncated /16 as cdq/and 15/add/sar 4
   do), the low ends raised to 0 and the high ends lowered to W, H (0x49A149..0x49A1C7).
   air_next re-tests every one of those, on the live unit, at the moment it hands it over:
   a unit the damage before it killed, or whose slot a new unit took, is re-judged, not
   trusted.

   THE POOL. s_airPool holds the slots of every unit the grid stamp has filed as airborne since
   the last rebuild, and the rebuild refills it with every alive unit whose state is airborne
   now: the stub on the step's call of the unit tick (0x4954ED, 0x48AD30's only caller) rebuilds
   it, and the stamp's airborne path adds each unit it files. So at any explosion the pool
   holds every unit that is airborne and was stamped since the step began, and every unit that
   was airborne when it began: the unit tick's death explosions, the projectile tick 0x49B720,
   the fire spread and the interceptor tail all see it. The network pump at 0x4954C8 runs
   before the rebuild, so an explosion from its 0x0E receiver sees the previous step's pool
   plus the units stamped since. What it can miss: a unit whose state turned airborne through
   a writer of +0x110 that is not followed by the stamp, since the last rebuild; such a unit
   holds no slot either, so it is stock's result until the next rebuild. A stale pool can only
   miss a victim, never offer a wrong one: air_unit decides every hand-over on the live unit.

   THE INVARIANT: every unit handed to the engine was validated alive, airborne, uncarried, on
   the map and inside the rect in this call, at the moment it was handed over, and every index
   was bounded first -- a pool slot below the array's count (u16 main+0x14351), the pool's
   length below 65 536 because a slot enters it at most once (s_airIn, kept with it). It rests
   on
     - a bound, above, on every value read from the engine;
     - a lock: the pool is written by the rebuild (the game thread) and by the stamp, which also
       runs from the network pump and from a saved game's restore on the loader thread, and is
       read by air_first on whatever thread runs area damage; s_airLock covers every read and
       write of it and of s_airIn, and is held only inside this C code, never across engine
       code but one: the allocator 0x4D83B0 growing a call's list, which takes its own critical
       section and calls nothing that takes this one. The lock order is s_airLock, then the
       allocator's, and never the reverse; the out-of-memory handler 0x49E700 runs inside the
       allocator's section, or after it from eng_alloc_or_exit, and ends the process;
     - a lifetime: a call's list lives in its frame (the wrapper above), which exists exactly
       while that call runs, so nesting (the interceptor tail at 0x49A764 re-enters area damage
       after the serving is done) gets a list of its own.
   CLASS: simulation, fail closed. Air only [DECIDED 2026-09-25]: a ground unit that loses slot
   A stays stock's. */
#define AIR_SLOTS 65536u               /* a cell's unit slot is a u16                      */

static CRITICAL_SECTION s_airLock;
static unsigned short   s_airPool[AIR_SLOTS];
static unsigned char    s_airIn[AIR_SLOTS];  /* 1 iff the slot is in s_airPool[0, s_airN)   */
static unsigned int     s_airN;
static volatile LONG    s_airServed;         /* aircraft handed to the engine, any thread   */

/* the unit array, or NULL; *count its slots */
static const unsigned char* air_array(const char** main_out, unsigned int* count)
{
    const char* ta = *(const char* const*)0x00511DE8;
    *main_out = ta;
    *count = 0;
    if (!ta) return NULL;
    *count = *(const unsigned short*)(ta + 0x14351);
    return *(const unsigned char* const*)(ta + 0x14357);
}

/* the unit in `slot` if it may be served against `r`: see WHICH AIRCRAFT */
static const unsigned char* air_unit(unsigned int slot, const DMGAIR* r)
{
    const char* ta;
    unsigned int count, st, cargo, bucket;
    const unsigned char* u = air_array(&ta, &count);
    short x, z, fw, fh;
    if (!u || slot >= count) return NULL;
    u += (size_t)slot * DMG_UNIT_STRIDE;
    memcpy(&st, u + 0x110, 4);
    if ((st & 0x30004003u) != 0x10000002u) return NULL;
    memcpy(&cargo, u + 0x86, 4);
    if (cargo || !*(void* const*)(u + 0x9E)) return NULL;
    memcpy(&bucket, u + 0x82, 4);
    if (!bucket || bucket == *(const unsigned int*)(ta + 0x142B7)) return NULL;
    memcpy(&x, u + 0x76, 2); memcpy(&z, u + 0x78, 2);
    memcpy(&fw, u + 0x7E, 2); memcpy(&fh, u + 0x80, 2);
    if (fw <= 0 || fh <= 0) return NULL;
    if (x >= r->xe || x + fw <= r->xs || z >= r->ze || z + fh <= r->zs) return NULL;
    return u;
}

static int dmg_set_has(const DMGSET* t, unsigned int value)
{
    unsigned int key = value + 1u, j;
    for (j = dmg_hash(key, t->cap - 1u); t->key[j]; j = (j + 1u) & (t->cap - 1u))
        if ((t->key[j] & ~DMG_KEY_PAST) == key) return 1;
    return 0;
}

/* this call's next candidate, re-judged now, or NULL: stdcall, from the selector stub */
static const unsigned char* __stdcall air_next(void)
{
    DMGSEEN* f = (DMGSEEN*)TlsGetValue(s_dmgTls);
    if (!f) return NULL;
    while (f->air.next < f->air.n) {
        unsigned int slot = f->air.idx[f->air.next++];
        const unsigned char* u = air_unit(slot, &f->air);
        if (u && !dmg_set_has(&f->unit, slot)) {
            InterlockedIncrement(&s_airServed);
            return u;
        }
    }
    return NULL;
}

/* the walk is over: this call's rect, its list from the pool, its first candidate */
static const unsigned char* __stdcall air_first(const unsigned char* proj, const unsigned char* at)
{
    DMGSEEN* f = dmg_frame();
    const char* ta;
    unsigned int count, i, radius, c;
    short ax, az;
    int xc, zc;
    const unsigned char* w;
    if (!f || !air_array(&ta, &count) || !proj || !at) return NULL;
    if (!(w = *(const unsigned char* const*)proj)) return NULL;
    radius = (unsigned int)*(const unsigned short*)(w + 0xD6) >> 1;
    c = (radius >> 4) + 1u;
    memcpy(&ax, at + 0x02, 2);
    memcpy(&az, at + 0x0A, 2);
    xc = ax / 16;                      /* C truncates toward zero, as cdq/and 15/add/sar 4 */
    zc = az / 16;
    f->air.xs = xc - (int)c; if (f->air.xs < 0) f->air.xs = 0;
    f->air.xe = xc + (int)c; if (f->air.xe > *(const int*)(ta + 0x14233)) f->air.xe = *(const int*)(ta + 0x14233);
    f->air.zs = zc - (int)c; if (f->air.zs < 0) f->air.zs = 0;
    f->air.ze = zc + (int)c; if (f->air.ze > *(const int*)(ta + 0x14237)) f->air.ze = *(const int*)(ta + 0x14237);
    f->air.n = f->air.next = 0;
    if (f->air.xs >= f->air.xe || f->air.zs >= f->air.ze) return NULL;

    EnterCriticalSection(&s_airLock);
    for (i = 0; i < s_airN; i++) {
        unsigned int slot = s_airPool[i];
        if (!air_unit(slot, &f->air) || dmg_set_has(&f->unit, slot)) continue;
        if (f->air.n == f->air.cap) {
            unsigned int cap = f->air.cap * 2u;
            unsigned short* p = (unsigned short*)eng_alloc_or_exit(s_dmgTag, cap * 2u);
            memcpy(p, f->air.idx, f->air.n * 2u);
            if (f->air.idx != f->air.inl) ENG_FREE(f->air.idx);
            f->air.idx = p;
            f->air.cap = cap;
        }
        f->air.idx[f->air.n++] = (unsigned short)slot;
    }
    LeaveCriticalSection(&s_airLock);
    return air_next();
}

/* the stamp filed `unit` as airborne (0x47CF98): it joins the pool */
static void __cdecl air_note(const unsigned char* unit)
{
    const char* ta;
    unsigned int count;
    const unsigned char* begin = air_array(&ta, &count);
    size_t off, slot;
    if (!begin || unit < begin) return;
    off = (size_t)(unit - begin);
    slot = off / DMG_UNIT_STRIDE;
    if (off % DMG_UNIT_STRIDE || slot >= count) return;
    EnterCriticalSection(&s_airLock);
    if (!s_airIn[slot]) {
        s_airIn[slot] = 1;
        s_airPool[s_airN++] = (unsigned short)slot;
    }
    LeaveCriticalSection(&s_airLock);
}

/* the step's unit tick is about to run (0x4954ED): the pool is every airborne unit now */
static void __cdecl air_rebuild(void)
{
    const char* ta;
    unsigned int count, i, st;
    const unsigned char* begin = air_array(&ta, &count);
    EnterCriticalSection(&s_airLock);
    for (i = 0; i < s_airN; i++) s_airIn[s_airPool[i]] = 0;
    s_airN = 0;
    for (i = 0; begin && i < count; i++) {
        memcpy(&st, begin + (size_t)i * DMG_UNIT_STRIDE + 0x110, 4);
        if ((st & 0x30000003u) == 0x10000002u) {
            s_airIn[i] = 1;
            s_airPool[s_airN++] = (unsigned short)i;
        }
    }
    LeaveCriticalSection(&s_airLock);
}

static int fix_stacked_air(void)
{
    static const unsigned char tail[11] = {             /* 0x49A664                          */
        0x8B, 0x45, 0x08,                   /* mov eax,[ebp+8]                   */
        0x8B, 0x08,                         /* mov ecx,[eax]                     */
        0x8B, 0x91, 0x11, 0x01, 0x00, 0x00, /* mov edx,[ecx+0x111]               */
    };
    static const unsigned char sel[18] = {              /* 0x49A415                          */
        0x8B, 0x44, 0x24, 0x10,             /* mov eax,[esp+0x10]                */
        0x40,                               /* inc eax                           */
        0x83, 0xF8, 0x01,                   /* cmp eax,1                         */
        0x89, 0x44, 0x24, 0x10,             /* mov [esp+0x10],eax                */
        0x0F, 0x8E, 0xED, 0xFD, 0xFF, 0xFF, /* jle 0x49A214                      */
    };
    static const unsigned char stamp[9] = {             /* 0x47CF98                          */
        0x83, 0xF8, 0x02,                   /* cmp eax,2                         */
        0x0F, 0x85, 0x34, 0x01, 0x00, 0x00, /* jne 0x47D0D5                      */
    };
    static const unsigned char tick[5] = { 0xE8, 0x3E, 0x58, 0xFF, 0xFF };   /* call 0x48AD30 */
    unsigned char *t, *s, *m, *k, *p;

    InitializeCriticalSection(&s_airLock);
    t = fix_code(64); s = fix_code(64); m = fix_code(32); k = fix_code(16);
    if (!t || !s || !m || !k) { lim_no_stub(); return FIX_TABLE; }

    /* the walk's end: push [ebp+0xC]; push [ebp+8]; call air_first; test eax,eax; jz tail;
       mov esi,eax; mov dword [esp+0x10],2; jmp 0x49A24E; tail: the 11 bytes; jmp 0x49A66F */
    p = t;
    *p++ = 0xFF; *p++ = 0x75; *p++ = 0x0C;
    *p++ = 0xFF; *p++ = 0x75; *p++ = 0x08;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)air_first); p += 4;
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x74; *p++ = 0x0F;
    *p++ = 0x89; *p++ = 0xC6;
    *p++ = 0xC7; *p++ = 0x44; *p++ = 0x24; *p++ = 0x10; *p++ = 2; *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049A24E); p += 4;
    memcpy(p, tail, sizeof tail); p += sizeof tail;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049A66F); p += 4;

    /* the selector: mov eax,[esp+0x10]; cmp eax,2; jae serve; inc eax; cmp eax,1;
       mov [esp+0x10],eax; jle 0x49A214; jmp 0x49A427; serve: call air_next; test eax,eax;
       jz tail; mov esi,eax; jmp 0x49A24E; tail: the 11 bytes; jmp 0x49A66F. For 0 and 1 the
       flags jle reads are stock's: inc, cmp, then a mov that leaves them. */
    p = s;
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x10;
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x02;
    *p++ = 0x73; *p++ = 0x13;
    *p++ = 0x40;
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x01;
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x10;
    *p++ = 0x0F; *p++ = 0x8E; tagpu_detour_rel(p, 0x0049A214); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049A427); p += 4;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)air_next); p += 4;
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x74; *p++ = 0x07;
    *p++ = 0x89; *p++ = 0xC6;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049A24E); p += 4;
    memcpy(p, tail, sizeof tail); p += sizeof tail;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049A66F); p += 4;

    /* the stamp's airborne path: cmp eax,2; jne 0x47D0D5; pushad; push esi; call air_note;
       add esp,4; popad; jmp 0x47CFA1 -- 0x47CFA1 sets its own flags (test edi,edi) */
    p = m;
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x02;
    *p++ = 0x0F; *p++ = 0x85; tagpu_detour_rel(p, 0x0047D0D5); p += 4;
    *p++ = 0x60;
    *p++ = 0x56;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)air_note); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x61;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0047CFA1); p += 4;

    /* the unit tick's call: pushfd; pushad; call air_rebuild; popad; popfd; jmp 0x48AD30 --
       the return address the site's call pushed stays on top, so 0x48AD30 returns to 0x4954F2 */
    p = k;
    *p++ = 0x9C;
    *p++ = 0x60;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)air_rebuild); p += 4;
    *p++ = 0x61;
    *p++ = 0x9D;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0048AD30); p += 4;

    lim_branch(0x0049A664, sizeof tail, tail, 0xE9, (unsigned int)(size_t)t,
               "stacked aircraft: served at the end of area damage's walk");
    lim_branch(0x0049A415, sizeof sel, sel, 0xE9, (unsigned int)(size_t)s,
               "stacked aircraft: area damage's slot selector");
    lim_branch(0x0047CF98, sizeof stamp, stamp, 0xE9, (unsigned int)(size_t)m,
               "stacked aircraft: the stamp's airborne path joins the pool");
    lim_branch(0x004954ED, sizeof tick, tick, 0xE8, (unsigned int)(size_t)k,
               "stacked aircraft: the pool rebuilt before the unit tick");
    return FIX_TABLE;
}

/* FLAK'S DIVIDES [DISASSEMBLED]. The ballistic fire 0x49CDE0 (callers 0x49D0F8 in the fire
   dispatch, 0x49D44E in the 0x0D receiver, 0x49D76E; all three only for a weapon with
   w+0x111 bit 1, ballistic) divides twice.
     - 0x49CE6A `div ecx`, ecx = w+0x68, weaponvelocity x 65536/30 (the loader, 0x42E4C6..
       0x42E4DC), for every shot: a ballistic weapon with velocity 0 faults here.
     - 0x49CF19 `idiv ebp`, for a burnblow weapon (bit 23, 0x49CECC): flight time = the
       horizontal distance / ebp, ebp = 0x4B7123(pitch, v) = v * cos(pitch) from the 512-entry
       table 0x509F00, which is 0 at entries 0 and 256: a pitch within 0.35 degrees of
       straight up or down. Stock's flak guns (ARMFLAK_GUN, CORFLAK_GUN, ARMYORK_GUN,
       CORSENT_GUN) are ballistic and burnblow, and an aircraft overhead asks for that pitch.
       The 0x0D receiver calls the same function, so every peer that gets the shot divides.
   THE FIX. 0x49CF18 (`cdq; idiv ebp; mov edx,[0x511DE8]`, nine bytes; no branch lands inside
   [rel8/rel32 scan of .text]) jumps to a stub that divides as stock for ebp != 0, and for
   ebp == 0 takes the engine's own non-burnblow rule instead, w+0xE6 = weapontimer (0x49CF29,
   with eax the weapon, as 0x49CEC6 left it). And the loader's closing call 0x49E010 (the aim
   routine's choice by flags, w+0x60; its only callers are the loader's two exits 0x42F314 and
   0x42F32E) is preceded by a floor: a ballistic weapon whose w+0x68 is 0 gets 1, logged.
   THE INVARIANT: no divide in 0x49CDE0 sees a zero divisor -- the second's is tested where it
   is used, the first's is at least 1 from the load on, and the loader is its only writer.
   Identity for every shot stock does not fault on, and for every stock weapon (none has
   weaponvelocity 0). The floor also changes the pitch solver's input (0x49A890, handed w+0x68
   at 0x49D608), but only for such a weapon, which stock faults on the moment it fires.
   CLASS: local. A peer without it faults on the shot, which is not a silent divergence. */
static volatile unsigned int s_flakFallbacks;   /* peekable, GAME THREAD */

static void __cdecl flak_fallback(const unsigned char* slot)
{
    const char* w = *(const char* const*)(slot + 0x0C);
    unsigned int n = ++s_flakFallbacks;
    if (n <= 8 || !(n & (n - 1u)))
        tagpu_logf("enginefix: flak: a burnblow shot at pitch 0x%04X has no horizontal speed; "
                   "%.32s flies its weapontimer (%u so far)", *(const unsigned short*)(slot + 0x18),
                   w ? w : "?", n);
}

static void __cdecl wpn_velocity_floor(unsigned int* regs)
{
    unsigned char* w = (unsigned char*)(size_t)regs[PR_EBP];
    unsigned int flags, v;
    memcpy(&flags, w + WPN_FLAGS, 4);
    memcpy(&v, w + 0x68, 4);
    if ((flags & 2u) && v == 0) {
        v = 1;
        memcpy(w + 0x68, &v, 4);
        tagpu_logf("enginefix: flak: weapon %.32s is ballistic with weaponvelocity 0; its "
                   "velocity is 1, so 0x49CE6A does not divide by zero", (const char*)w);
    }
}

static int fix_flak_divides(void)
{
    FIXSITE site[3] = {
        { 0x0049CF18, 9, { 0x99, 0xF7, 0xFD, 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 }, { 0 } },
        { 0x0042F314, 5, { 0xE8, 0xF7, 0xEC, 0x06, 0x00 }, { 0 } },     /* call 0x49E010 */
        { 0x0042F32E, 5, { 0xE8, 0xDD, 0xEC, 0x06, 0x00 }, { 0 } },
    };
    unsigned char *a, *b, *p;
    int k;
    if (!fix_match(site, 3)) return FIX_BYTES;
    if (!(a = fix_code(48)) || !(b = fix_code(32))) return FIX_STUB;
    p = a;
    *p++ = 0x85; *p++ = 0xED;                                   /* test ebp,ebp        */
    *p++ = 0x74; *p++ = 0x0E;                                   /* jz zero             */
    memcpy(p, site[0].was, 9); p += 9;                          /* cdq; idiv; mov edx  */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049CF21); p += 4;
    *p++ = 0x60;                                                /* zero: pushad        */
    *p++ = 0x56;                                                /* push esi, the slot  */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)flak_fallback); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                      /* add esp,4           */
    *p++ = 0x61;                                                /* popad               */
    *p++ = 0x8B; *p++ = 0x46; *p++ = 0x0C;                      /* mov eax,[esi+0xC]   */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049CF29);
    p = fix_call_regs(b, wpn_velocity_floor);
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049E010);
    fix_branch(&site[0], 0xE9, a);
    for (k = 1; k < 3; k++) fix_branch(&site[k], 0xE8, b);
    return fix_write(site, 3);
}

/* THE LAST ROW AND COLUMN [DISASSEMBLED]. The grid stamp 0x47CC30 (the motion relink's call
   0x43DA41 after its clear 0x47D0E0, and 0x48610F, 0x48630C, 0x48AA9A, 0x48B6B9) parks a unit
   in the off-map bucket main+0x142B7 when its footprint leaves the map: X < 0, Z < 0, or
   X + fw >= W (0x47CC85/0x47CC8B), Z + fh >= H (0x47CCA1/0x47CCA3). The last two are one
   past: a footprint that ends on the last column or row, every cell of it on the map, is
   parked, and a parked unit holds no cell, so area damage and the direct-hit test 0x49B090
   never find it. Ground units never stand there (LoadMap masks the border with 0xFFFD); an
   aircraft over the bottom or right edge does.
   THE FIX: `jge` -> `jg` at 0x47CC8B and 0x47CCA3, and a bound on the unit's sort bucket.
   What the rest of the stamp and its readers need, settled from the disassembly:
     - the stamp's cell walk starts at (Z * W + X) * 13 and covers fh rows of fw cells: with
       X + fw <= W and Z + fh <= H every cell is inside the grid; the clear 0x47D0E0 decides by
       the bucket (0x47D0FD) and walks the same cells, and the re-claim 0x47C790 has no bound
       of its own but reaches only a stamped unit (bit 27, which only the stamp sets) and walks
       the same footprint. Every writer of +0x76 on a stamped unit clears first (0x43DA0F,
       0x48AA6B, 0x48B685); the other two, the create (0x485BD3) and the saved game's restore
       (0x487243), write a fresh unit that holds no stamp yet;
     - the sort bucket (0x47CCA9..0x47CCDA) is taken from the unit's position, not its
       footprint: column x >> 23, row z >> 23 (128-px buckets), in a grid of
       ceil(16W / 128) x ceil(16H / 128) (0x482C84..0x482CA6), with no bound. Every writer of
       the footprint computes X = (x - 8 fw + 8) >> 4 in pixels (0x43D877..0x43D895,
       0x485BA3..0x485BC7, 0x48AA24..0x48AA32), so X + fw <= W gives x < 16W - 8 fw + 8, which
       is inside the grid for fw >= 1 -- every stock footprint -- and one bucket past its end
       for fw = 0. Stock itself forms column -1 for fw = 0 and x in [-8, 0) at the west edge,
       whose linear index row * cols - 1 is the previous row's last bucket, inside the grid
       for row >= 1. Buckets are read by simulation code (0x40F2E9, 0x47E5DA, ...), so stock's
       index is kept whenever it lands inside the rows * cols grid, and only a linear index
       outside [0, rows * cols) is replaced: its column and row clamped into the grid.
   THE INVARIANT: a unit is parked only when a cell of its footprint is off the map, every
   bucket index the stamp forms is inside the rows * cols grid -- LoadMap allocates
   (rows * cols + 7) & ~7 buckets (0x482CAE..0x482CBA), so inside the block too -- and it is
   stock's whenever stock's is inside the grid.
   CLASS: simulation, fail closed (who can be hit). */
static int fix_last_cell(void)
{
    static const unsigned char jgeX[6] = { 0x0F, 0x8D, 0xE8, 0x03, 0x00, 0x00 };
    static const unsigned char jgeZ[6] = { 0x0F, 0x8D, 0xD0, 0x03, 0x00, 0x00 };
    static const unsigned char bucket[50] = {
        0x8D, 0x4E, 0x6A, 0x8B, 0xD1, 0x8B, 0x0A, 0xC1, 0xF9, 0x17, 0x8B, 0x42,
        0x04, 0x89, 0x44, 0x24, 0x20, 0x8B, 0x42, 0x08, 0x8B, 0x95, 0x9F, 0x42,
        0x01, 0x00, 0xC1, 0xF8, 0x17, 0x0F, 0xAF, 0x85, 0xA3, 0x42, 0x01, 0x00,
        0x03, 0xC1, 0x8D, 0x0C, 0x80, 0x8B, 0x86, 0x82, 0x00, 0x00, 0x00, 0x8D,
        0x14, 0x4A,
    };
    /* esi the unit, ebp main; out: edx the bucket, eax [esi+0x82], [esp+0x20] = y, as stock.
       ebx (fw) and edi (fh) are live past 0x47CCDB and kept. */
    static const unsigned char stub[] = {
        0x8B, 0x4E, 0x6A,                   /* mov ecx,[esi+0x6a]                */
        0xC1, 0xF9, 0x17,                   /* sar ecx,23: stock's column        */
        0x8B, 0x46, 0x6E,                   /* mov eax,[esi+0x6e]                */
        0x89, 0x44, 0x24, 0x20,             /* mov [esp+0x20],eax                */
        0x8B, 0x56, 0x72,                   /* mov edx,[esi+0x72]                */
        0xC1, 0xFA, 0x17,                   /* sar edx,23: stock's row           */
        0x89, 0xD0,                         /* mov eax,edx                       */
        0x0F, 0xAF, 0x85, 0xA3, 0x42, 0x01, 0x00, /* imul eax,[ebp+0x142a3]      */
        0x01, 0xC8,                         /* add eax,ecx: stock's index        */
        0x53,                               /* push ebx                          */
        0x8B, 0x9D, 0xA3, 0x42, 0x01, 0x00, /* mov ebx,[ebp+0x142a3]: columns    */
        0x0F, 0xAF, 0x9D, 0xA7, 0x42, 0x01, 0x00, /* imul ebx,[ebp+0x142a7]: rows */
        0x39, 0xD8,                         /* cmp eax,ebx                       */
        0x5B,                               /* pop ebx                           */
        0x72, 0x31,                         /* jb keep: inside the array         */
        0x8B, 0x85, 0xA3, 0x42, 0x01, 0x00, /* mov eax,[ebp+0x142a3]             */
        0x48,                               /* dec eax                           */
        0x39, 0xC1, 0x7E, 0x02,             /* cmp ecx,eax; jle +2               */
        0x89, 0xC1,                         /* mov ecx,eax                       */
        0x85, 0xC9, 0x7D, 0x02,             /* test ecx,ecx; jge +2              */
        0x31, 0xC9,                         /* xor ecx,ecx                       */
        0x8B, 0x85, 0xA7, 0x42, 0x01, 0x00, /* mov eax,[ebp+0x142a7]             */
        0x48,                               /* dec eax                           */
        0x39, 0xC2, 0x7E, 0x02,             /* cmp edx,eax; jle +2               */
        0x89, 0xC2,                         /* mov edx,eax                       */
        0x85, 0xD2, 0x7D, 0x02,             /* test edx,edx; jge +2              */
        0x31, 0xD2,                         /* xor edx,edx                       */
        0x89, 0xD0,                         /* mov eax,edx                       */
        0x0F, 0xAF, 0x85, 0xA3, 0x42, 0x01, 0x00, /* imul eax,[ebp+0x142a3]      */
        0x01, 0xC8,                         /* add eax,ecx                       */
        0x8D, 0x0C, 0x80,                   /* keep: lea ecx,[eax+eax*4]         */
        0x8B, 0x95, 0x9F, 0x42, 0x01, 0x00, /* mov edx,[ebp+0x1429f]: buckets    */
        0x8D, 0x14, 0x4A,                   /* lea edx,[edx+ecx*2]               */
        0x8B, 0x86, 0x82, 0x00, 0x00, 0x00, /* mov eax,[esi+0x82]                */
    };
    unsigned char gX[6], gZ[6];
    unsigned char* a;
    if (!(a = fix_code(sizeof stub + 5))) { lim_no_stub(); return FIX_TABLE; }
    memcpy(a, stub, sizeof stub);
    a[sizeof stub] = 0xE9; tagpu_detour_rel(a + sizeof stub + 1, 0x0047CCDB);
    memcpy(gX, jgeX, 6); gX[1] = 0x8F;                          /* jg */
    memcpy(gZ, jgeZ, 6); gZ[1] = 0x8F;
    lim_add(0x0047CC8B, 6, jgeX, gX, "the grid stamp: the last column is on the map");
    lim_add(0x0047CCA3, 6, jgeZ, gZ, "the grid stamp: the last row is on the map");
    lim_branch(0x0047CCA9, sizeof bucket, bucket, 0xE9, (unsigned int)(size_t)a,
               "the grid stamp: the sort bucket, bounded");
    return FIX_TABLE;
}

/* THE LINE-OF-SIGHT SHEAR [DISASSEMBLED]. UnitInPlayerLOS 0x465AC0(player, unit) (callers
   0x40AB11 in the periodic acquisition 0x40AA40, 0x439761, 0x46AF3D, 0x46B5B4, 0x480EE5,
   0x48BC56, 0x494396, and our order markers through it) tests up to four points of the unit's
   box, in turn, until one is visible: (x + def+0x15E, y + def+0x16E, z + def+0x166) (0x465AFD);
   the same with x + def+0x176 (0x465BDB); then (.., y - def+0x17A, z + def+0x17E) (0x465C73);
   and that with x - def+0x176 (0x465D19) -- 16.16 dwords at [esp+0x10] x, [esp+0x14] y,
   [esp+0x18] z, def the unit's type at +0x92. Every point is placed at col = x >> 5,
   row = (z - (y >> 1)) >> 5 (32-px cells), projected by altitude, under unsigned bounds on the
   player's grid size (player +0x80 width, +0x84 height): outside them the point is not
   visible. What it then reads depends on the line-of-sight mode, and the shear is the same in
   every mode:
     - LosType bit 1 (main+0x14281; True, LosType 14) reads the player's LOS grid (+0x7C) inline:
       0x465B6A..0x465B94, 0x465C04..0x465C2E, 0x465CA2..0x465CD2 and 0x465D46..0x465D67;
     - without it (Permanent 12, Circular 8) the first three points go to
       PositionInPlayerMapped 0x408090(player, point), whose place is 0x408095..0x4080C0 and
       which reads the shared mapped grid main+0x14273, and the fourth to an inline copy of it,
       0x465DA9..0x465DCA.
   0x408090 has two more callers, each handing it a world point whose row means the same, and
   each with an inline copy of the True read beside the call:
     - 0x407FBD in 0x407E90, the first method of the vtable 0x4FC9A0 (its destructor 0x407E70
       restores the base's 0x4FC980) [INFERRED: an AI routine], which tests a probe point (esi)
       stepped 320 px from a position along a heading it draws, for the player at ecx; its
       copy is 0x407F74..0x407F9B (read 0x407F9C, not visible 0x407FB7). The probe's answer
       decides what the routine keeps, so it joins this table;
     - 0x49BF2F in the projectile draw pass 0x49BE60, the local player's view of a projectile;
       its copy is 0x49BEE8..0x49BF0F. That one is a draw: fix_projectile_view, below.
   Each sees a changed answer only for a point whose sheared row is off the grid and whose own
   row is on it -- the defect this fixes.
   A unit whose top is more than twice its distance from the north edge -- an aircraft at
   cruise altitude a few tiles inside it, a unit on a hill beside it -- samples rows above the
   grid and is invisible to every other player, so nothing acquires it. Underwater, y < 0, the
   same happens at the south edge.
   THE SAME READ, INLINED ELSEWHERE. The census enumerates by the data, not by one instruction
   shape: every instruction of .text that reads a grid's height in memory -- the player's +0x84
   (the LOS and mapped grids), the sight grid's main+0x14297, and [r+8] through a register that
   a lea of main+0x1428F or of +0x7C (or an add r,0x7C) set -- and every compare with a register
   loaded from one of them (exe-reverse-engineering.md, "Line of sight at the map's edge", lists
   each). 57 compares read such a height. 46 are this read, a point placed at (z - h/2) >> 5
   against one player's grid; three re-test the row the sight emitter stored (0x4819B7,
   0x481DE4, 0x482304), three bound a ray's cell (0x481B0B, 0x481F15, 0x482435), two re-test a
   row 0x47D3B8 already bounded (0x47D41A, 0x47D44E), and three compare another structure's
   +0x84 with zero. The 13 compares with a register loaded from a +0x84 are other structures'
   counts.
   WHAT IT DOES NOT ENUMERATE, found by a second pass over those two forms: a grid pointer
   spilled to the stack and reloaded, and a height recomputed from the plot's rows
   main+0x14237 >> 1. Of four spills, the mapped stamp's, the removal's and the stamp's
   (0x4819AD, 0x481DDA, 0x4822FA) are reloaded on each ray's back edge and reach compares the
   census has (0x481B0B, 0x481F15, 0x482435). The sight grid's builder's (0x482F2A) reaches two
   it has not: the builder projects every terrain tile to its sheared row (0x482FFC..0x483002)
   and writes it only inside the grid (cmp ecx,-1 at 0x483005, then 0x48307C and 0x4830C6
   through [esp+0x14]), so the sight grid is BUILT in sheared space, and that is the space the
   ray fan reads, from the own row as from stock's. A tile sheared off the grid loses only its
   occlusion, never anyone's sight, so it stays stock. The recomputed heights reach four
   compares -- the circle's row clips (0x481C28, 0x481FF9, 0x482519) and 0x40D80A, a
   mapped-grid reader by a scaled coordinate -- and one scale (0x466CB3, a minimap pixel to a
   cell), none of them this read. A scan of the shear's arithmetic (a halving subtracted from a z,
   then >> 5, following one register copy) finds 39 computations: the builder's, and one
   feeding each of 38 of the 46 -- the other eight reuse a y >> 1 computed further up.
   Beyond the eight above:
     - the sight emitter 0x4825B0 (called at 0x481836 in the all-units rebuild, 0x482824 in
       0x4827B0, which builds a unit's record -- itself called at 0x43DA59, 0x464DBF,
       0x465053, 0x48AAA0 and 0x48B6BF --, 0x482B5E in 0x482AC0, which does the same after
       zeroing the unit's height byte (called at 0x486178 and 0x486312), and 0x4829F4 in
       0x482910, which builds a temporary sight record; 0x482868 is in 0x482830, which nothing
       calls and no pointer names): under
       the ray fan (LosType bit 0x4) it places a unit's sight at the sheared row of its
       clamped sight height h (0x482615..0x48261E), stores the column and row in the record's
       words (the unit's +0x7A/+0x7C, 0x48264C/0x482652) and stamps it only inside the grid
       (0x482663); outside, it zeroes the height byte and stamps nothing, so an aircraft near
       the north edge reveals nothing to its owner. The stamp 0x482270, the mapped stamp
       0x481930 and the removal 0x481D50 all read the stored words, and the height byte decides
       whether there is a stamp to remove (0x482634), so the row is fixed once, before it is
       stored: every stamp and its removal use the same row by construction. Every removal --
       the emitter's own (0x482644), the death removal 0x482090 (called at 0x486845), the
       temporary record's expiry 0x482130 (0x482161) -- goes through 0x481D50, which re-tests
       the stored column and row, so a record whose stored words the bound refused removes
       nothing. The stored
       words are otherwise written only by the create (0 at 0x485C0A), a saved game's restore
       (0x48724D, with the height byte at 0x4872BF, as saved), and the temporary records'
       compaction (main+0x1427B, at most 20 of 36 bytes, each holding its own words and byte),
       which moves the words (0x482220) and the byte (0x48222D) together -- the table;
     - the order resolver 0x43F0E0, six: whether the target's cell is mapped for the local
       player decides the order it resolves -- the table;
     - the view player's map build 0x467440, two: a unit it sees gets +0x110 bit 8, which the
       acquisition 0x40AA40 lists for every player -- the table;
     - local, fix_los_local: the cursor picker 0x43E490, six; the site test 0x47D2E0, one; the
       feature helper 0x4658E0, four (two corners); the radar rebuild 0x466DC0's projectile
       dots, four; the particle leaves 0x473590, 0x473A00, 0x474170, 0x4745E0 and 0x475470, two
       each; positional sound 0x47F300, two;
     - 0x474B80, two: left stock, since nothing runs it (no call or jump reaches it in .text
       and no pointer to it is in the image).
   The circle (LosType bit 0x4 clear) places sight at the circle's corner, (z >> 5) - (y >> 6)
   less its offset (0x4826B7), and the stamp clips each row to the grid (0x4824F8..0x482547),
   so every row of the circle that is inside the grid is stamped.
   THE FIX, at every one of them: when the sheared row is outside the grid and the point's own
   row, z >> 5, is inside it, the own row is used; otherwise the stock answer stands, so a unit
   beyond the map's edge stays unseen (the margin TADR adds there is a gameplay change, not a
   defect). Registers: the third read loads dx and di (the point's y and z words) and the fourth
   reads them again (0x465D46, 0x465DA9), so its stub loads them as stock does; ebp after the
   third and edx after the fourth are dead on both exits. The fourth's column comes in ecx from
   0x465D3B, in both modes. In 0x408090, esi (pushed at 0x408094) is free and edx is reloaded
   with the player from [esp+8] before either exit.
   THE INVARIANT: the grid is read only at a column and a row inside it -- the same unsigned
   bounds as stock, now applied to the row actually used. Exact whenever stock's row is inside.
   THE ORACLE: every stub counts, per function, the times it takes the own row (s_losOwnRow) --
   the one path only the fix runs, so a count above zero is that function's stub at work.
   CLASS: simulation, fail closed (what is acquired, the order resolved); the local reads are
   fix_los_local's. */

/* The functions whose reads are fixed, in the order of their counters. */
enum {
    LOS_UNIT, LOS_MAPPED, LOS_PROBE, LOS_ORDER, LOS_VIEWMAP, LOS_EMIT, /* the table */
    LOS_PROJ, LOS_CURSOR, LOS_BUILD, LOS_FEATURE, LOS_SOUND,       /* local     */
    LOS_FX_473590, LOS_FX_474170, LOS_FX_4745E0, LOS_FX_475470, LOS_FX_473A00, LOS_RADAR,
    LOS_NFN
};
static const unsigned int s_losFn[LOS_NFN] = {
    0x465AC0, 0x408090, 0x407E90, 0x43F0E0, 0x467440, 0x4825B0,
    0x49BE60, 0x43E490, 0x47D2E0, 0x4658E0, 0x47F300,
    0x473590, 0x474170, 0x4745E0, 0x475470, 0x473A00, 0x466DC0,
};
/* the own row taken, one count per function; interlocked, so the count assumes nothing about
   which thread calls the engine's draw and sound code. It counts the row taken, not a read or a
   stamp made: where the column is bounded after the row, as at the sight emitter (0x48265B), a
   counted row can still be refused by its column, and nothing is read or stamped. */
static volatile LONG s_losOwnRow[LOS_NFN];

/* pushfd; lock inc dword [&s_losOwnRow[fn]]; popfd -- the count, with the flags kept. The hand
   stubs' literal rel8s (stubAB .. stubO, the projectile pass's stub) jump over it, so they are
   written for this length, and a different one fails the build here. */
#define LOS_COUNT_LEN 9
typedef char los_count_len_is_the_hand_stubs[(LOS_COUNT_LEN == 9) ? 1 : -1];
static int los_count(unsigned char* a, int fn)
{
    unsigned int at = (unsigned int)(size_t)&s_losOwnRow[fn];
    a[0] = 0x9C;
    a[1] = 0xF0; a[2] = 0xFF; a[3] = 0x05;
    memcpy(a + 4, &at, 4);
    a[8] = 0x9D;
    return LOS_COUNT_LEN;
}

typedef struct LOSREAD {
    unsigned int va, in, out;           /* the block, stock's read, stock's "not visible" */
    unsigned char n;
    unsigned char stock[49];
    const unsigned char* stub;
    unsigned char nstub;
    const char* name;
    unsigned char fn;
} LOSREAD;

/* A copy of the read whose row test stands alone: `cmp r,[g+0x84]` and the jae or jb after it,
   n bytes at va (8 with a short branch, 12 with a near one). stock holds the retail bytes from
   the instruction that loaded the point's z word into r (nz bytes, pre bytes before va) through
   the row test, and the whole span is compared before anything is written: the stub reloads z
   with that instruction at va, which is exact only while nothing in between writes what it
   reads -- and the span is everything in between. */
typedef struct LOSROW {
    unsigned int  va;
    unsigned char n, pre, nz, fn;
    unsigned char stock[52];
} LOSROW;

/* 1 when z is `movsx r,word <operand>`, exactly nz bytes, and the operand reads no r: at the row
   test r holds the sheared row, so a z whose base or source is r would reload garbage */
static int los_z_ok(const unsigned char* z, int nz, int r)
{
    int mod, rm, len = 3, base = -1, index = -1;
    if (nz < 3 || z[0] != 0x0F || z[1] != 0xBF || ((z[2] >> 3) & 7) != r) return 0;
    mod = z[2] >> 6;
    rm = z[2] & 7;
    if (mod == 3) {
        base = rm;                                  /* movsx r,<reg16>: its source */
    } else {
        if (rm == 4) {                              /* a SIB byte */
            if (nz < 4) return 0;
            len++;
            base = z[3] & 7;
            index = (z[3] >> 3) & 7;
            if (index == 4) index = -1;
            if (mod == 0 && base == 5) { base = -1; len += 4; }
        } else if (mod == 0 && rm == 5) {
            len += 4;                               /* an absolute address */
        } else {
            base = rm;
        }
        if (mod == 1) len += 1;
        else if (mod == 2) len += 4;
    }
    return len == nz && base != r && index != r;
}

/* One stub at a, for the row test c (n bytes at va) of a point whose z word z reloads into r,
   after lead (nlead bytes, where stock has overwritten z's base by the test):
       cmp r,[g+0x84]; jb in        the sheared row is inside: stock's read
       lead; movsx r,<z>; sar r,5   the point's own row
       cmp r,[g+0x84]; jae out      outside too: stock's "not visible"
       count; jmp in                the own row, read where stock reads a row
   in and out are the site's own exits, decoded from its stock bytes (after a jae, out is the
   target; after a jb, in is), and those bytes are compared with the image before any of this
   is written. Only r, the flags and what lead writes change: r leaves as the row read, and in
   is reached with CF set and out with CF clear, as stock reaches them.
   Returns the length, LOS_STUB_LEN(nz, nlead), or 0 when c is not that shape, z is not a
   movsx into r, or either reads r for its address; nothing is written over the site then. */
#define LOS_STUB_LEN(nz, nlead) (32 + LOS_COUNT_LEN + (nz) + (nlead))
static int los_stub(unsigned char* a, unsigned int va, int n, const unsigned char* c,
                    const unsigned char* z, int nz, const unsigned char* lead, int nlead, int fn)
{
    unsigned int next = va + (unsigned int)n, target, in, out;
    int r = (c[1] >> 3) & 7, jb, rel, k = 0;
    if (c[0] != 0x3B || (c[1] & 0xC0) != 0x80 || (c[1] & 7) == 4 || (c[1] & 7) == r ||
        r == 4 || c[2] != 0x84 || c[3] || c[4] || c[5] || !los_z_ok(z, nz, r))
        return 0;
    if (n == 8 && (c[6] == 0x72 || c[6] == 0x73)) {
        jb = c[6] == 0x72;
        target = next + (unsigned int)(int)(signed char)c[7];
    } else if (n == 12 && c[6] == 0x0F && (c[7] == 0x82 || c[7] == 0x83)) {
        jb = c[7] == 0x82;
        memcpy(&rel, c + 8, 4);
        target = next + (unsigned int)rel;
    } else {
        return 0;
    }
    in  = jb ? target : next;
    out = jb ? next : target;
    memcpy(a + k, c, 6); k += 6;
    a[k++] = 0x0F; a[k++] = 0x82; tagpu_detour_rel(a + k, in); k += 4;
    if (nlead) { memcpy(a + k, lead, (size_t)nlead); k += nlead; }
    memcpy(a + k, z, (size_t)nz); k += nz;
    a[k++] = 0xC1; a[k++] = (unsigned char)(0xF8 + r); a[k++] = 0x05;
    memcpy(a + k, c, 6); k += 6;
    a[k++] = 0x0F; a[k++] = 0x83; tagpu_detour_rel(a + k, out); k += 4;
    k += los_count(a + k, fn);
    a[k++] = 0xE9; tagpu_detour_rel(a + k, in); k += 4;
    return k == LOS_STUB_LEN(nz, nlead) ? k : 0;
}

static int los_row_stub(unsigned char* a, const LOSROW* s)
{
    return los_stub(a, s->va, s->n, s->stock + s->pre, s->stock, s->nz, NULL, 0, s->fn);
}

static int fix_los_shear(void)
{
    /* the first two points: the words of the box point at [esp+0x12] x, [esp+0x16] y,
       [esp+0x1a] z; esi the player */
    static const unsigned char stubAB[] = {
        0x0F, 0xBF, 0x6C, 0x24, 0x16,       /* movsx ebp,word [esp+0x16]      */
        0x0F, 0xBF, 0x44, 0x24, 0x1A,       /* movsx eax,word [esp+0x1a]      */
        0x0F, 0xBF, 0x4C, 0x24, 0x12,       /* movsx ecx,word [esp+0x12]      */
        0xD1, 0xFD,                         /* sar ebp,1                      */
        0x2B, 0xC5,                         /* sub eax,ebp: the shear         */
        0xC1, 0xF9, 0x05,                   /* sar ecx,5                      */
        0xC1, 0xF8, 0x05,                   /* sar eax,5                      */
        0x3B, 0x8E, 0x80, 0x00, 0x00, 0x00, /* cmp ecx,[esi+0x80]             */
        0x73, 0x26,                         /* jae out                        */
        0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[esi+0x84]             */
        0x72, 0x19,                         /* jb in                          */
        0x0F, 0xBF, 0x44, 0x24, 0x1A,       /* movsx eax,word [esp+0x1a]      */
        0xC1, 0xF8, 0x05,                   /* sar eax,5: the point's own row */
        0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[esi+0x84]             */
        0x73, 0x0E,                         /* jae out                        */
    };                                      /* count; in: jmp; out: jmp       */
    /* the third point: dx and di stay loaded for the fourth */
    static const unsigned char stubC[] = {
        0x66, 0x8B, 0x54, 0x24, 0x16,       /* mov dx,[esp+0x16]              */
        0x66, 0x8B, 0x7C, 0x24, 0x1A,       /* mov di,[esp+0x1a]              */
        0x0F, 0xBF, 0x4C, 0x24, 0x12,       /* movsx ecx,word [esp+0x12]      */
        0x0F, 0xBF, 0xEA,                   /* movsx ebp,dx                   */
        0x0F, 0xBF, 0xC7,                   /* movsx eax,di                   */
        0xD1, 0xFD,                         /* sar ebp,1                      */
        0x29, 0xE8,                         /* sub eax,ebp: the shear         */
        0xC1, 0xF9, 0x05,                   /* sar ecx,5                      */
        0xC1, 0xF8, 0x05,                   /* sar eax,5                      */
        0x3B, 0x8E, 0x80, 0x00, 0x00, 0x00, /* cmp ecx,[esi+0x80]             */
        0x73, 0x24,                         /* jae out                        */
        0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[esi+0x84]             */
        0x72, 0x17,                         /* jb in                          */
        0x0F, 0xBF, 0xC7,                   /* movsx eax,di                   */
        0xC1, 0xF8, 0x05,                   /* sar eax,5: the point's own row */
        0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[esi+0x84]             */
        0x73, 0x0E,                         /* jae out                        */
    };
    /* the fourth point: ecx its x word, dx and di as the third left them */
    static const unsigned char stubD[] = {
        0x0F, 0xBF, 0xD2,                   /* movsx edx,dx                   */
        0x0F, 0xBF, 0xC7,                   /* movsx eax,di                   */
        0xD1, 0xFA,                         /* sar edx,1                      */
        0x29, 0xD0,                         /* sub eax,edx: the shear         */
        0xC1, 0xF9, 0x05,                   /* sar ecx,5                      */
        0xC1, 0xF8, 0x05,                   /* sar eax,5                      */
        0x3B, 0x8E, 0x80, 0x00, 0x00, 0x00, /* cmp ecx,[esi+0x80]             */
        0x73, 0x24,                         /* jae out                        */
        0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[esi+0x84]             */
        0x72, 0x17,                         /* jb in                          */
        0x0F, 0xBF, 0xC7,                   /* movsx eax,di                   */
        0xC1, 0xF8, 0x05,                   /* sar eax,5: the point's own row */
        0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[esi+0x84]             */
        0x73, 0x0E,                         /* jae out                        */
    };
    /* PositionInPlayerMapped: eax the point; out: ecx the column, eax the row, edx the player */
    static const unsigned char stubM[] = {
        0x0F, 0xBF, 0x70, 0x0A,             /* movsx esi,word [eax+0xa]       */
        0x0F, 0xBF, 0x50, 0x06,             /* movsx edx,word [eax+6]         */
        0x0F, 0xBF, 0x48, 0x02,             /* movsx ecx,word [eax+2]         */
        0x89, 0xF0,                         /* mov eax,esi                    */
        0xD1, 0xFA,                         /* sar edx,1                      */
        0x29, 0xD0,                         /* sub eax,edx: the shear         */
        0x8B, 0x54, 0x24, 0x08,             /* mov edx,[esp+8]: the player    */
        0xC1, 0xF9, 0x05,                   /* sar ecx,5                      */
        0xC1, 0xF8, 0x05,                   /* sar eax,5                      */
        0xC1, 0xFE, 0x05,                   /* sar esi,5: the point's own row */
        0x3B, 0x8A, 0x80, 0x00, 0x00, 0x00, /* cmp ecx,[edx+0x80]             */
        0x73, 0x20,                         /* jae out                        */
        0x3B, 0x82, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[edx+0x84]             */
        0x72, 0x13,                         /* jb in                          */
        0x89, 0xF0,                         /* mov eax,esi                    */
        0x3B, 0x82, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[edx+0x84]             */
        0x73, 0x0E,                         /* jae out                        */
    };
    /* the AI probe's copy: esi the point, ecx the player. ebx is dead on both exits: the read
       at 0x407F9C writes it, and past 0x408002 it is written at 0x40803E or popped at
       0x408081 */
    static const unsigned char stubP[] = {
        0x0F, 0xBF, 0x5E, 0x06,             /* movsx ebx,word [esi+6]         */
        0x0F, 0xBF, 0x46, 0x0A,             /* movsx eax,word [esi+0xa]       */
        0x0F, 0xBF, 0x56, 0x02,             /* movsx edx,word [esi+2]         */
        0xD1, 0xFB,                         /* sar ebx,1                      */
        0x29, 0xD8,                         /* sub eax,ebx: the shear         */
        0x8B, 0x99, 0x80, 0x00, 0x00, 0x00, /* mov ebx,[ecx+0x80]             */
        0xC1, 0xFA, 0x05,                   /* sar edx,5                      */
        0xC1, 0xF8, 0x05,                   /* sar eax,5                      */
        0x39, 0xDA,                         /* cmp edx,ebx                    */
        0x73, 0x25,                         /* jae out                        */
        0x3B, 0x81, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[ecx+0x84]             */
        0x72, 0x18,                         /* jb in                          */
        0x0F, 0xBF, 0x46, 0x0A,             /* movsx eax,word [esi+0xa]       */
        0xC1, 0xF8, 0x05,                   /* sar eax,5: the point's own row */
        0x3B, 0x81, 0x84, 0x00, 0x00, 0x00, /* cmp eax,[ecx+0x84]             */
        0x73, 0x0E,                         /* jae out                        */
    };
    static const LOSREAD read[7] = {
        { 0x00465B6A, 0x00465B95, 0x00465BB1, 43, {
            0x0F, 0xBF, 0x6C, 0x24, 0x16, 0x0F, 0xBF, 0x44, 0x24, 0x1A, 0x0F, 0xBF,
            0x4C, 0x24, 0x12, 0xD1, 0xFD, 0x2B, 0xC5, 0x8B, 0xAE, 0x80, 0x00, 0x00,
            0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCD, 0x73, 0x24, 0x3B,
            0x86, 0x84, 0x00, 0x00, 0x00, 0x73, 0x1C }, stubAB, sizeof stubAB,
          "line of sight: the first point's row", LOS_UNIT },
        { 0x00465C04, 0x00465C2F, 0x00465C49, 43, {
            0x0F, 0xBF, 0x6C, 0x24, 0x16, 0x0F, 0xBF, 0x44, 0x24, 0x1A, 0x0F, 0xBF,
            0x4C, 0x24, 0x12, 0xD1, 0xFD, 0x2B, 0xC5, 0x8B, 0xAE, 0x80, 0x00, 0x00,
            0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCD, 0x73, 0x22, 0x3B,
            0x86, 0x84, 0x00, 0x00, 0x00, 0x73, 0x1A }, stubAB, sizeof stubAB,
          "line of sight: the second point's row", LOS_UNIT },
        { 0x00465CA2, 0x00465CD3, 0x00465CED, 49, {
            0x66, 0x8B, 0x54, 0x24, 0x16, 0x66, 0x8B, 0x7C, 0x24, 0x1A, 0x0F, 0xBF,
            0x4C, 0x24, 0x12, 0x0F, 0xBF, 0xEA, 0x0F, 0xBF, 0xC7, 0xD1, 0xFD, 0x2B,
            0xC5, 0x8B, 0xAE, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8,
            0x05, 0x3B, 0xCD, 0x73, 0x22, 0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, 0x73,
            0x1A }, stubC, sizeof stubC,
          "line of sight: the third point's row", LOS_UNIT },
        { 0x00465D46, 0x00465D68, 0x00465D94, 34, {
            0x0F, 0xBF, 0xD2, 0x0F, 0xBF, 0xC7, 0xD1, 0xFA, 0x2B, 0xC2, 0x8B, 0x96,
            0x80, 0x00, 0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCA,
            0x73, 0x34, 0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, 0x73, 0x2C }, stubD, sizeof stubD,
          "line of sight: the fourth point's row", LOS_UNIT },
        { 0x00465DA9, 0x00465DE0, 0x00465DCB, 34, {
            0x0F, 0xBF, 0xD2, 0x0F, 0xBF, 0xC7, 0xD1, 0xFA, 0x2B, 0xC2, 0x8B, 0x96,
            0x80, 0x00, 0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCA,
            0x73, 0x08, 0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, 0x72, 0x15 }, stubD, sizeof stubD,
          "mapped: the fourth point's row", LOS_UNIT },
        { 0x00408095, 0x004080C7, 0x004080C1, 44, {
            0x0F, 0xBF, 0x50, 0x06, 0x0F, 0xBF, 0x48, 0x02, 0x0F, 0xBF, 0x40, 0x0A,
            0xD1, 0xFA, 0x2B, 0xC2, 0x8B, 0x54, 0x24, 0x08, 0xC1, 0xF9, 0x05, 0x8B,
            0xB2, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xF8, 0x05, 0x3B, 0xCE, 0x73, 0x08,
            0x3B, 0x82, 0x84, 0x00, 0x00, 0x00, 0x72, 0x06 }, stubM, sizeof stubM,
          "mapped: PositionInPlayerMapped's row", LOS_MAPPED },
        { 0x00407F74, 0x00407F9C, 0x00407FB7, 40, {
            0x0F, 0xBF, 0x5E, 0x06, 0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x56, 0x02,
            0xD1, 0xFB, 0x2B, 0xC3, 0x8B, 0x99, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFA,
            0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xD3, 0x73, 0x23, 0x3B, 0x81, 0x84, 0x00,
            0x00, 0x00, 0x73, 0x1B }, stubP, sizeof stubP,
          "line of sight: the AI probe's row", LOS_PROBE },
    };
    /* the copies whose row test stands alone */
    static const LOSROW row[7] = {
        { 0x0043F5D1, 8, 38, 4, LOS_ORDER, {   /* r = ecx */
            0x0F, 0xBF, 0x4E, 0x0A, 0x8B, 0xA8, 0x96, 0x00, 0x00, 0x00, 0x0F, 0xBF,
            0x46, 0x06, 0x0F, 0xBF, 0x56, 0x02, 0xD1, 0xF8, 0x2B, 0xC8, 0x8B, 0x85,
            0x80, 0x00, 0x00, 0x00, 0xC1, 0xFA, 0x05, 0xC1, 0xF9, 0x05, 0x3B, 0xD0,
            0x73, 0x46, 0x3B, 0x8D, 0x84, 0x00, 0x00, 0x00, 0x73, 0x3E } },
        { 0x0043FC05, 12, 38, 4, LOS_ORDER, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0x8B, 0x97, 0x96, 0x00,
            0x00, 0x00, 0xD1, 0xFB, 0x2B, 0xC3, 0x8B, 0x9A, 0x80, 0x00, 0x00, 0x00,
            0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCB, 0x0F, 0x83, 0xD0, 0x00,
            0x00, 0x00, 0x3B, 0x82, 0x84, 0x00, 0x00, 0x00, 0x0F, 0x83, 0xC4, 0x00,
            0x00, 0x00 } },
        { 0x0043FD1A, 12, 38, 4, LOS_ORDER, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0x8B, 0x97, 0x96, 0x00,
            0x00, 0x00, 0xD1, 0xFB, 0x2B, 0xC3, 0x8B, 0x9A, 0x80, 0x00, 0x00, 0x00,
            0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCB, 0x0F, 0x83, 0xE5, 0x00,
            0x00, 0x00, 0x3B, 0x82, 0x84, 0x00, 0x00, 0x00, 0x0F, 0x83, 0xD9, 0x00,
            0x00, 0x00 } },
        { 0x0043FF85, 12, 32, 4, LOS_ORDER, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0xD1, 0xFB, 0x2B, 0xC3,
            0x8B, 0x9A, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05,
            0x3B, 0xCB, 0x0F, 0x83, 0xE0, 0x00, 0x00, 0x00, 0x3B, 0x82, 0x84, 0x00,
            0x00, 0x00, 0x0F, 0x83, 0xD4, 0x00, 0x00, 0x00 } },
        { 0x004400AA, 12, 38, 4, LOS_ORDER, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0x8B, 0x92, 0x96, 0x00,
            0x00, 0x00, 0xD1, 0xFB, 0x2B, 0xC3, 0x8B, 0x9A, 0x80, 0x00, 0x00, 0x00,
            0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCB, 0x0F, 0x83, 0xF3, 0x00,
            0x00, 0x00, 0x3B, 0x82, 0x84, 0x00, 0x00, 0x00, 0x0F, 0x83, 0xE7, 0x00,
            0x00, 0x00 } },
        { 0x0046778A, 8, 27, 3, LOS_VIEWMAP, {   /* r = ecx */
            0x0F, 0xBF, 0x0E, 0x0F, 0xBF, 0x56, 0xF8, 0xD1, 0xFF, 0x2B, 0xCF, 0x8B,
            0xB8, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFA, 0x05, 0xC1, 0xF9, 0x05, 0x3B,
            0xD7, 0x73, 0x23, 0x3B, 0x88, 0x84, 0x00, 0x00, 0x00, 0x73, 0x1B } },
        { 0x004677D0, 8, 27, 3, LOS_VIEWMAP, {   /* r = edx */
            0x0F, 0xBF, 0x16, 0x0F, 0xBF, 0x7E, 0xF8, 0xD1, 0xFB, 0x2B, 0xD3, 0x8B,
            0x98, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFF, 0x05, 0xC1, 0xFA, 0x05, 0x3B,
            0xFB, 0x73, 0x08, 0x3B, 0x90, 0x84, 0x00, 0x00, 0x00, 0x72, 0x04 } },
    };
    /* the order resolver's second read, 0x43F631..0x43F654: stock overwrites the point's
       register (esi, at 0x43F635) before the row test, so the stub is the whole block, with z
       held on the stack. ebp the player, edx the x word; both exits as stock: ecx the row,
       edx the column, esi the width */
    static const unsigned char blockO[36] = {
        0x0F, 0xBF, 0x4E, 0x0A, 0x0F, 0xBF, 0x76, 0x06, 0xD1, 0xFE, 0x2B, 0xCE,
        0x8B, 0xB5, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFA, 0x05, 0xC1, 0xF9, 0x05,
        0x3B, 0xD6, 0x73, 0x5D, 0x3B, 0x8D, 0x84, 0x00, 0x00, 0x00, 0x73, 0x55,
    };
    static const unsigned char stubO[] = {
        0x0F, 0xBF, 0x4E, 0x0A,             /* movsx ecx,word [esi+0xa]       */
        0x51,                               /* push ecx: z                    */
        0x0F, 0xBF, 0x76, 0x06,             /* movsx esi,word [esi+6]         */
        0xD1, 0xFE,                         /* sar esi,1                      */
        0x2B, 0xCE,                         /* sub ecx,esi: the shear         */
        0x8B, 0xB5, 0x80, 0x00, 0x00, 0x00, /* mov esi,[ebp+0x80]             */
        0xC1, 0xFA, 0x05,                   /* sar edx,5                      */
        0xC1, 0xF9, 0x05,                   /* sar ecx,5                      */
        0x3B, 0xD6,                         /* cmp edx,esi                    */
        0x73, 0x28,                         /* jae out                        */
        0x3B, 0x8D, 0x84, 0x00, 0x00, 0x00, /* cmp ecx,[ebp+0x84]             */
        0x72, 0x17,                         /* jb in                          */
        0x8B, 0x0C, 0x24,                   /* mov ecx,[esp]: z               */
        0xC1, 0xF9, 0x05,                   /* sar ecx,5: the point's own row */
        0x3B, 0x8D, 0x84, 0x00, 0x00, 0x00, /* cmp ecx,[ebp+0x84]             */
        0x73, 0x12,                         /* jae out                        */
    };                                      /* count; in: drop; jmp; out: drop; jmp */
    static const unsigned char drop[4] = { 0x8D, 0x64, 0x24, 0x04 };   /* lea esp,[esp+4] */
    /* the sight emitter 0x4825B0, from its point (ebp the column, ebx the clamped height h,
       edi the z word, eax |old h - h|, edx the record's stored column/row) to its row: compared,
       never written */
    static const unsigned char emitSpan[65] = {
        0x0F, 0xBF, 0x6E, 0x12, 0x0F, 0xBF, 0x46, 0x16, 0x33, 0xDB, 0x8A, 0x5E,
        0x0A, 0xC1, 0xFD, 0x05, 0x03, 0xD8, 0x79, 0x02, 0x33, 0xDB, 0x81, 0xFB,
        0xFF, 0x00, 0x00, 0x00, 0x7E, 0x05, 0xBB, 0xFF, 0x00, 0x00, 0x00, 0x8B,
        0x56, 0x0C, 0x8B, 0xCB, 0x0F, 0xBF, 0x7E, 0x1A, 0x8A, 0x02, 0x88, 0x44,
        0x24, 0x18, 0x25, 0xFF, 0x00, 0x00, 0x00, 0x2B, 0xC3, 0x99, 0x33, 0xC2,
        0x2B, 0xC2, 0x8B, 0x56, 0x04,
    };
    static const unsigned char emitRow[10] = {
        0xD1, 0xF9,                         /* sar ecx,1                      */
        0x2B, 0xF9,                         /* sub edi,ecx: the shear         */
        0x0F, 0xBF, 0x0A,                   /* movsx ecx,word [edx]: stored   */
        0xC1, 0xFF, 0x05,                   /* sar edi,5                      */
    };
    /* from the stub's landing point to the bound it mirrors, which the stub's argument rests on:
       the compare with the stored words and the early-out, the removal, the store, then
       cmp ebp,[eax+0x14293]; jae; cmp edi,[eax+0x14297]; jae -- compared, never written */
    static const unsigned char emitAfter[76] = {
        0x3B, 0xCD, 0x75, 0x11, 0x0F, 0xBF, 0x52, 0x02, 0x3B, 0xD7, 0x75, 0x09,
        0x83, 0xF8, 0x05, 0x0F, 0x8E, 0x6C, 0x01, 0x00, 0x00, 0x8A, 0x44, 0x24,
        0x18, 0x84, 0xC0, 0x74, 0x0D, 0xF6, 0x44, 0x24, 0x10, 0x02, 0x74, 0x06,
        0x56, 0xE8, 0x07, 0xF7, 0xFF, 0xFF, 0x8B, 0x46, 0x04, 0x66, 0x89, 0x28,
        0x8B, 0x4E, 0x04, 0x66, 0x89, 0x79, 0x02, 0xA1, 0xE8, 0x1D, 0x51, 0x00,
        0x3B, 0xA8, 0x93, 0x42, 0x01, 0x00, 0x73, 0x46, 0x3B, 0xB8, 0x97, 0x42,
        0x01, 0x00, 0x73, 0x3E,
    };
    static const unsigned char emitHead[] = {
        0x50,                               /* push eax: |dh|, read at 0x48262B */
        0x89, 0xF8,                         /* mov eax,edi: z                 */
        0xC1, 0xF8, 0x05,                   /* sar eax,5: the point's own row */
        0xD1, 0xF9,                         /* sar ecx,1                      */
        0x2B, 0xF9,                         /* sub edi,ecx: the shear         */
        0xC1, 0xFF, 0x05,                   /* sar edi,5                      */
        0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00, /* mov ecx,[0x511DE8]             */
        0x3B, 0xB9, 0x97, 0x42, 0x01, 0x00, /* cmp edi,[ecx+0x14297]          */
        0x72, 0x00,                         /* jb keep: inside, stock's row   */
        0x3B, 0x81, 0x97, 0x42, 0x01, 0x00, /* cmp eax,[ecx+0x14297]          */
        0x73, 0x00,                         /* jae keep: outside too          */
        0x89, 0xC7,                         /* mov edi,eax: the own row       */
    };                                      /* count                          */
    enum { EMIT_JB = 25, EMIT_JAE = 33 };   /* the two branches in emitHead; keep follows the count */
    static const unsigned char emitTail[] = {
        0x58,                               /* keep: pop eax                  */
        0x0F, 0xBF, 0x0A,                   /* movsx ecx,word [edx]           */
        0xE9, 0x00, 0x00, 0x00, 0x00,       /* jmp 0x48261F                   */
    };
    int k;
    for (k = 0; k < 7; k++) {
        const LOSREAD* r = &read[k];
        unsigned char* a = fix_code(r->nstub + LOS_COUNT_LEN + 10u);
        int m;
        if (!a) { lim_no_stub(); return FIX_TABLE; }
        memcpy(a, r->stub, r->nstub);
        m = r->nstub + los_count(a + r->nstub, r->fn);
        a[m] = 0xE9; tagpu_detour_rel(a + m + 1, r->in);
        a[m + 5] = 0xE9; tagpu_detour_rel(a + m + 6, r->out);
        lim_branch(r->va, r->n, r->stock, 0xE9, (unsigned int)(size_t)a, r->name);
    }
    for (k = 0; k < (int)(sizeof row / sizeof row[0]); k++) {
        const LOSROW* w = &row[k];
        const char* name = w->fn == LOS_ORDER ? "line of sight: the order resolver's row"
                                              : "line of sight: the view player's map's row";
        unsigned char* a = fix_code(LOS_STUB_LEN(w->nz, 0));
        if (!a || !los_row_stub(a, w)) { lim_no_stub(); return FIX_TABLE; }
        lim_same(w->va - w->pre, w->pre, w->stock, name);
        lim_branch(w->va, w->n, w->stock + w->pre, 0xE9, (unsigned int)(size_t)a, name);
    }
    {
        unsigned char* a = fix_code(sizeof stubO + LOS_COUNT_LEN + 18u);
        int m;
        if (!a) { lim_no_stub(); return FIX_TABLE; }
        memcpy(a, stubO, sizeof stubO);
        m = (int)sizeof stubO + los_count(a + sizeof stubO, LOS_ORDER);
        memcpy(a + m, drop, 4);     a[m + 4] = 0xE9;  tagpu_detour_rel(a + m + 5, 0x0043F655);
        memcpy(a + m + 9, drop, 4); a[m + 13] = 0xE9; tagpu_detour_rel(a + m + 14, 0x0043F6AA);
        lim_branch(0x0043F631, sizeof blockO, blockO, 0xE9, (unsigned int)(size_t)a,
                   "line of sight: the order resolver's second row");
    }
    {
        unsigned char* a = fix_code(sizeof emitHead + LOS_COUNT_LEN + sizeof emitTail);
        unsigned int rel;
        int m;
        if (!a) { lim_no_stub(); return FIX_TABLE; }
        memcpy(a, emitHead, sizeof emitHead);
        if (a[EMIT_JB] != 0x72 || a[EMIT_JAE] != 0x73) { lim_no_stub(); return FIX_TABLE; }
        a[EMIT_JB + 1]  = (unsigned char)(sizeof emitHead + LOS_COUNT_LEN - (EMIT_JB + 2));
        a[EMIT_JAE + 1] = (unsigned char)(sizeof emitHead + LOS_COUNT_LEN - (EMIT_JAE + 2));
        m = (int)sizeof emitHead + los_count(a + sizeof emitHead, LOS_EMIT);
        memcpy(a + m, emitTail, sizeof emitTail);
        rel = 0x0048261Fu - ((unsigned int)(size_t)a + (unsigned int)m + sizeof emitTail);
        memcpy(a + m + sizeof emitTail - 4, &rel, 4);
        lim_same(0x004825D4, sizeof emitSpan, emitSpan, "line of sight: the sight emitter's point");
        lim_branch(0x00482615, sizeof emitRow, emitRow, 0xE9, (unsigned int)(size_t)a,
                   "line of sight: the sight emitter's row");
        lim_same(0x0048261F, sizeof emitAfter, emitAfter,
                 "line of sight: the sight emitter's store and bound");
    }
    return FIX_TABLE;
}

/* THE PROJECTILE PASS'S VIEW [DISASSEMBLED]. The projectile draw pass 0x49BE60 (called at
   0x469B22 in DrawGameScreen) asks, for each projectile, whether the local player (main+0x2A43)
   sees it, with the same altitude-sheared row as UnitInPlayerLOS: inline under True line of
   sight (0x49BEE8..0x49BF0F, ebp the point, eax the player; read 0x49BF10, not visible
   0x49BF29), through PositionInPlayerMapped 0x408090 otherwise (0x49BF2F, fixed with the
   shear above). A projectile high up near the north edge is not drawn, and a model projectile
   is not posed (0x49C127). The pass still runs: fxown, which would stop it, is not a play
   default, because the engine's frame is the golden source (tagpu_opt.c).
   THE FIX: the own row when the sheared one is off the grid and the own one is on it, as for
   units, counted under LOS_PROJ. ecx leaves as the row read, or unread past 0x49C058 on the
   not-visible exit. ebx is dead on both exits: the read at 0x49BF10 writes it, and the
   not-visible one reaches either the loop's top 0x49BEA3, which writes it (0x49BEE8, 0x49BF42)
   before any read, or its pop at 0x49C07D.
   THE INVARIANT: the grid is read only at a column and a row inside it.
   CLASS: local. A draw. */
static int fix_projectile_view(void)
{
    static const unsigned char was[40] = {
        0x0F, 0xBF, 0x5D, 0x06, 0x0F, 0xBF, 0x4D, 0x0A, 0x0F, 0xBF, 0x55, 0x02,
        0xD1, 0xFB, 0x2B, 0xCB, 0x8B, 0x98, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFA,
        0x05, 0xC1, 0xF9, 0x05, 0x3B, 0xD3, 0x73, 0x21, 0x3B, 0x88, 0x84, 0x00,
        0x00, 0x00, 0x73, 0x19,
    };
    static const unsigned char stub[] = {
        0x0F, 0xBF, 0x5D, 0x06,             /* movsx ebx,word [ebp+6]         */
        0x0F, 0xBF, 0x4D, 0x0A,             /* movsx ecx,word [ebp+0xa]       */
        0x0F, 0xBF, 0x55, 0x02,             /* movsx edx,word [ebp+2]         */
        0xD1, 0xFB,                         /* sar ebx,1                      */
        0x29, 0xD9,                         /* sub ecx,ebx: the shear         */
        0x8B, 0x98, 0x80, 0x00, 0x00, 0x00, /* mov ebx,[eax+0x80]             */
        0xC1, 0xFA, 0x05,                   /* sar edx,5                      */
        0xC1, 0xF9, 0x05,                   /* sar ecx,5                      */
        0x39, 0xDA,                         /* cmp edx,ebx                    */
        0x73, 0x25,                         /* jae out                        */
        0x3B, 0x88, 0x84, 0x00, 0x00, 0x00, /* cmp ecx,[eax+0x84]             */
        0x72, 0x18,                         /* jb in                          */
        0x0F, 0xBF, 0x4D, 0x0A,             /* movsx ecx,word [ebp+0xa]       */
        0xC1, 0xF9, 0x05,                   /* sar ecx,5: the point's own row */
        0x3B, 0x88, 0x84, 0x00, 0x00, 0x00, /* cmp ecx,[eax+0x84]             */
        0x73, 0x0E,                         /* jae out                        */
    };                                      /* count; in: jmp; out: jmp       */
    unsigned char now[sizeof was];
    unsigned char* a;
    unsigned int rel;
    int m;
    if (memcmp((const void*)0x0049BEE8, was, sizeof was) != 0) return FIX_BYTES;
    if (!(a = fix_code(sizeof stub + LOS_COUNT_LEN + 10))) return FIX_STUB;
    memcpy(a, stub, sizeof stub);
    m = (int)sizeof stub + los_count(a + sizeof stub, LOS_PROJ);
    a[m] = 0xE9; tagpu_detour_rel(a + m + 1, 0x0049BF10);
    a[m + 5] = 0xE9; tagpu_detour_rel(a + m + 6, 0x0049BF29);
    memset(now, 0x90, sizeof now);
    now[0] = 0xE9;
    rel = (unsigned int)(size_t)a - (0x0049BEE8u + 5u);
    memcpy(now + 1, &rel, 4);
    return tagpu_detour_write(0x0049BEE8, now, sizeof now) ? FIX_ARMED : FIX_PROTECT;
}

/* THE SAME READ IN LOCAL CODE [DISASSEMBLED]: the 27 copies that decide only what this player
   is shown or hears -- the cursor picker 0x43E490 (the pointer over a point, and with it what
   this player's own left click orders), the build cursor's site test 0x47D2E0, the feature
   helper 0x4658E0 (both corners), the radar rebuild 0x466DC0's projectile dots, the particle
   leaves 0x473590, 0x473A00, 0x474170, 0x4745E0 and 0x475470, positional sound 0x47F300. The
   census is under THE LINE-OF-SIGHT SHEAR, above.
   The site test's accumulator 0x51E688 is also read by AI code: 0x40A76A, through the getter
   0x47C770, after 0x47DB70, whose [+0x22F] path returns 1 without calling the site test -- so
   the value can be one this player's build cursor left, a per-peer leak stock already has. The
   fix changes only its value at the north edge (the footprint's sum instead of 0), no new kind
   of divergence, so the site test stays local.
   The radar's dots: the first two tests reload z from [ebx+4] (0x4671E5) with ebx intact; by
   the second two stock has overwritten ebx (0x467315, 0x467322, 0x467365), so their stubs take
   it back from [esp+0x18], where the loop keeps the same pointer (0x4671C3, 0x467425) -- ebx is
   dead on both exits of those two (reloaded at 0x4673A9, cleared at 0x467381). The whole loop
   is compared for them, since that pointer is its invariant.
   THE FIX and THE INVARIANT are the shear's, one generic stub per copy.
   CLASS: local. A cursor, a draw, a sound: the 27 are compared together, each from its z load
   through its row test, and written together, or not at all. */
static int fix_los_local(void)
{
    static const LOSROW row[23] = {
        { 0x0043E69D, 12, 32, 4, LOS_CURSOR, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0xD1, 0xFD, 0x2B, 0xC5,
            0x8B, 0xAA, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05,
            0x3B, 0xCD, 0x0F, 0x83, 0xD3, 0x00, 0x00, 0x00, 0x3B, 0x82, 0x84, 0x00,
            0x00, 0x00, 0x0F, 0x83, 0xC7, 0x00, 0x00, 0x00 } },
        { 0x0043E904, 12, 32, 4, LOS_CURSOR, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0xD1, 0xFB, 0x2B, 0xC3,
            0x8B, 0x9A, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05,
            0x3B, 0xCB, 0x0F, 0x83, 0xD8, 0x00, 0x00, 0x00, 0x3B, 0x82, 0x84, 0x00,
            0x00, 0x00, 0x0F, 0x83, 0xCC, 0x00, 0x00, 0x00 } },
        { 0x0043EBC6, 12, 32, 4, LOS_CURSOR, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0xD1, 0xFF, 0x2B, 0xC7,
            0x8B, 0xBA, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05,
            0x3B, 0xCF, 0x0F, 0x83, 0xD4, 0x00, 0x00, 0x00, 0x3B, 0x82, 0x84, 0x00,
            0x00, 0x00, 0x0F, 0x83, 0xC8, 0x00, 0x00, 0x00 } },
        { 0x0043ECDB, 12, 32, 4, LOS_CURSOR, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0xD1, 0xFF, 0x2B, 0xC7,
            0x8B, 0xBA, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05,
            0x3B, 0xCF, 0x0F, 0x83, 0xBD, 0x03, 0x00, 0x00, 0x3B, 0x82, 0x84, 0x00,
            0x00, 0x00, 0x0F, 0x83, 0xB1, 0x03, 0x00, 0x00 } },
        { 0x0043EE94, 12, 32, 4, LOS_CURSOR, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0xD1, 0xFD, 0x2B, 0xC5,
            0x8B, 0xAA, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05,
            0x3B, 0xCD, 0x0F, 0x83, 0xD8, 0x00, 0x00, 0x00, 0x3B, 0x82, 0x84, 0x00,
            0x00, 0x00, 0x0F, 0x83, 0xCC, 0x00, 0x00, 0x00 } },
        { 0x0043EFA9, 12, 38, 4, LOS_CURSOR, {   /* r = eax */
            0x0F, 0xBF, 0x46, 0x0A, 0x0F, 0xBF, 0x4E, 0x02, 0x8B, 0x92, 0x96, 0x00,
            0x00, 0x00, 0xD1, 0xFD, 0x2B, 0xC5, 0x8B, 0xAA, 0x80, 0x00, 0x00, 0x00,
            0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCD, 0x0F, 0x83, 0xD3, 0x00,
            0x00, 0x00, 0x3B, 0x82, 0x84, 0x00, 0x00, 0x00, 0x0F, 0x83, 0xC7, 0x00,
            0x00, 0x00 } },
        { 0x00465942, 8, 23, 3, LOS_FEATURE, {   /* r = ecx */
            0x0F, 0xBF, 0xCB, 0x8B, 0xA8, 0x80, 0x00, 0x00, 0x00, 0xD1, 0xFE, 0x2B,
            0xCE, 0xC1, 0xFA, 0x05, 0xC1, 0xF9, 0x05, 0x3B, 0xD5, 0x73, 0x24, 0x3B,
            0x88, 0x84, 0x00, 0x00, 0x00, 0x73, 0x1C } },
        { 0x0046598A, 8, 23, 3, LOS_FEATURE, {   /* r = ecx */
            0x0F, 0xBF, 0xCB, 0x8B, 0xA8, 0x80, 0x00, 0x00, 0x00, 0xD1, 0xFE, 0x2B,
            0xCE, 0xC1, 0xFA, 0x05, 0xC1, 0xF9, 0x05, 0x3B, 0xD5, 0x73, 0x08, 0x3B,
            0x88, 0x84, 0x00, 0x00, 0x00, 0x72, 0x04 } },
        { 0x00465A17, 8, 21, 3, LOS_FEATURE, {   /* r = ecx */
            0x0F, 0xBF, 0xCB, 0x2B, 0xCE, 0x8B, 0xB0, 0x80, 0x00, 0x00, 0x00, 0xC1,
            0xFA, 0x05, 0xC1, 0xF9, 0x05, 0x3B, 0xD6, 0x73, 0x2B, 0x3B, 0x88, 0x84,
            0x00, 0x00, 0x00, 0x73, 0x23 } },
        { 0x00465A63, 8, 21, 3, LOS_FEATURE, {   /* r = ecx */
            0x0F, 0xBF, 0xCB, 0x2B, 0xCE, 0x8B, 0xB0, 0x80, 0x00, 0x00, 0x00, 0xC1,
            0xFA, 0x05, 0xC1, 0xF9, 0x05, 0x3B, 0xD6, 0x73, 0x08, 0x3B, 0x88, 0x84,
            0x00, 0x00, 0x00, 0x72, 0x0C } },
        { 0x0047360C, 8, 28, 4, LOS_FX_473590, {   /* r = ecx */
            0x0F, 0xBF, 0x48, 0x0E, 0x0F, 0xBF, 0x78, 0x06, 0xD1, 0xFB, 0x2B, 0xCB,
            0x8B, 0x9A, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFF, 0x05, 0xC1, 0xF9, 0x05,
            0x3B, 0xFB, 0x73, 0x21, 0x3B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x73, 0x19 } },
        { 0x00473657, 8, 23, 4, LOS_FX_473590, {   /* r = ecx */
            0x0F, 0xBF, 0x48, 0x0E, 0xD1, 0xFB, 0x2B, 0xCB, 0x8B, 0x5C, 0x24, 0x18,
            0xC1, 0xF9, 0x05, 0x3B, 0x9A, 0x80, 0x00, 0x00, 0x00, 0x73, 0x08, 0x3B,
            0x8A, 0x84, 0x00, 0x00, 0x00, 0x72, 0x04 } },
        { 0x00473A94, 8, 24, 3, LOS_FX_473A00, {   /* r = eax */
            0x0F, 0xBF, 0xC5, 0x0F, 0xBF, 0xCF, 0x2B, 0xC2, 0x8B, 0x96, 0x80, 0x00,
            0x00, 0x00, 0xC1, 0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCA, 0x73, 0x23,
            0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, 0x73, 0x1B } },
        { 0x00473AD3, 8, 24, 3, LOS_FX_473A00, {   /* r = eax */
            0x0F, 0xBF, 0xC5, 0x0F, 0xBF, 0xFF, 0x2B, 0xC2, 0x8B, 0x96, 0x80, 0x00,
            0x00, 0x00, 0xC1, 0xFF, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xFA, 0x73, 0x08,
            0x3B, 0x86, 0x84, 0x00, 0x00, 0x00, 0x72, 0x04 } },
        { 0x004741EC, 8, 28, 4, LOS_FX_474170, {   /* r = ecx */
            0x0F, 0xBF, 0x48, 0x0E, 0x0F, 0xBF, 0x78, 0x06, 0xD1, 0xFB, 0x2B, 0xCB,
            0x8B, 0x9A, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFF, 0x05, 0xC1, 0xF9, 0x05,
            0x3B, 0xFB, 0x73, 0x21, 0x3B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x73, 0x19 } },
        { 0x00474237, 8, 23, 4, LOS_FX_474170, {   /* r = ecx */
            0x0F, 0xBF, 0x48, 0x0E, 0xD1, 0xFB, 0x2B, 0xCB, 0x8B, 0x5C, 0x24, 0x18,
            0xC1, 0xF9, 0x05, 0x3B, 0x9A, 0x80, 0x00, 0x00, 0x00, 0x73, 0x08, 0x3B,
            0x8A, 0x84, 0x00, 0x00, 0x00, 0x72, 0x04 } },
        { 0x00474674, 8, 28, 4, LOS_FX_4745E0, {   /* r = ecx */
            0x0F, 0xBF, 0x48, 0x0E, 0x0F, 0xBF, 0x70, 0x06, 0xD1, 0xFF, 0x2B, 0xCF,
            0x8B, 0xBA, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFE, 0x05, 0xC1, 0xF9, 0x05,
            0x3B, 0xF7, 0x73, 0x23, 0x3B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x73, 0x1B } },
        { 0x004746BB, 8, 28, 4, LOS_FX_4745E0, {   /* r = ecx */
            0x0F, 0xBF, 0x48, 0x0E, 0x0F, 0xBF, 0x58, 0x06, 0xD1, 0xFD, 0x2B, 0xCD,
            0x8B, 0xAA, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFB, 0x05, 0xC1, 0xF9, 0x05,
            0x3B, 0xDD, 0x73, 0x08, 0x3B, 0x8A, 0x84, 0x00, 0x00, 0x00, 0x72, 0x04 } },
        { 0x0047551E, 8, 33, 5, LOS_FX_475470, {   /* r = eax */
            0x0F, 0xBF, 0x44, 0x24, 0x1C, 0x0F, 0xBF, 0xCB, 0x0F, 0xBF, 0x5C, 0x24,
            0x18, 0xD1, 0xFB, 0x2B, 0xC3, 0x8B, 0x9D, 0x80, 0x00, 0x00, 0x00, 0xC1,
            0xF9, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xCB, 0x73, 0x23, 0x3B, 0x85, 0x84,
            0x00, 0x00, 0x00, 0x73, 0x1B } },
        { 0x0047556C, 8, 24, 5, LOS_FX_475470, {   /* r = eax */
            0x0F, 0xBF, 0x44, 0x24, 0x1C, 0xD1, 0xFB, 0x2B, 0xC3, 0x8B, 0x5C, 0x24,
            0x20, 0xC1, 0xF8, 0x05, 0x3B, 0x9D, 0x80, 0x00, 0x00, 0x00, 0x73, 0x08,
            0x3B, 0x85, 0x84, 0x00, 0x00, 0x00, 0x72, 0x04 } },
        { 0x0047D3B8, 12, 29, 5, LOS_BUILD, {   /* r = eax */
            0x0F, 0xBF, 0x44, 0x24, 0x3A, 0xD1, 0xF9, 0x2B, 0xC1, 0x8B, 0x8F, 0x80,
            0x00, 0x00, 0x00, 0xC1, 0xFA, 0x05, 0xC1, 0xF8, 0x05, 0x3B, 0xD1, 0x0F,
            0x83, 0x57, 0x04, 0x00, 0x00, 0x3B, 0x87, 0x84, 0x00, 0x00, 0x00, 0x0F,
            0x83, 0x4B, 0x04, 0x00, 0x00 } },
        { 0x0047F431, 8, 28, 4, LOS_SOUND, {   /* r = ecx */
            0x0F, 0xBF, 0x4E, 0x0A, 0x0F, 0xBF, 0x56, 0x02, 0xD1, 0xFF, 0x2B, 0xCF,
            0x8B, 0xB8, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFA, 0x05, 0xC1, 0xF9, 0x05,
            0x3B, 0xD7, 0x73, 0x21, 0x3B, 0x88, 0x84, 0x00, 0x00, 0x00, 0x73, 0x19 } },
        { 0x0047F476, 8, 28, 4, LOS_SOUND, {   /* r = edx */
            0x0F, 0xBF, 0x56, 0x0A, 0x0F, 0xBF, 0x7E, 0x02, 0xD1, 0xFB, 0x2B, 0xD3,
            0x8B, 0x98, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFF, 0x05, 0xC1, 0xFA, 0x05,
            0x3B, 0xFB, 0x73, 0x08, 0x3B, 0x90, 0x84, 0x00, 0x00, 0x00, 0x72, 0x04 } },
    };
    /* the radar rebuild's projectile loop, 0x4671C0..0x46742E */
    static const unsigned char dots[623] = {
        0x8D, 0x59, 0x0A, 0x89, 0x5C, 0x24, 0x18, 0xEB, 0x04, 0x8B, 0x4C, 0x24,
        0x1C, 0x0F, 0xBF, 0x6B, 0xFC, 0x0F, 0xBF, 0x86, 0xEB, 0x42, 0x01, 0x00,
        0x0F, 0xAF, 0xC5, 0x99, 0xF7, 0xBE, 0x2B, 0x42, 0x01, 0x00, 0x0F, 0xBF,
        0x13, 0x0F, 0xBF, 0x7B, 0x04, 0xD1, 0xFA, 0x2B, 0xFA, 0x89, 0x44, 0x24,
        0x20, 0x0F, 0xBF, 0x86, 0xED, 0x42, 0x01, 0x00, 0x0F, 0xAF, 0xC7, 0x99,
        0xF7, 0xBE, 0x2F, 0x42, 0x01, 0x00, 0x89, 0x44, 0x24, 0x24, 0x8B, 0x01,
        0x8B, 0x88, 0x11, 0x01, 0x00, 0x00, 0xF7, 0xC1, 0x00, 0x00, 0x00, 0x60,
        0x0F, 0x85, 0xE6, 0x00, 0x00, 0x00, 0xF6, 0xC1, 0x40, 0x0F, 0x85, 0xE3,
        0x01, 0x00, 0x00, 0x8A, 0x8E, 0x43, 0x2A, 0x00, 0x00, 0x81, 0xE1, 0xFF,
        0x00, 0x00, 0x00, 0x8B, 0xD1, 0xC1, 0xE2, 0x05, 0x03, 0xD1, 0x8D, 0x04,
        0x0E, 0x8D, 0x14, 0x92, 0x8D, 0x94, 0x50, 0x63, 0x1B, 0x00, 0x00, 0x8A,
        0x86, 0x81, 0x42, 0x01, 0x00, 0x24, 0x02, 0x3C, 0x02, 0x75, 0x35, 0x8B,
        0x82, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFD, 0x05, 0xC1, 0xFF, 0x05, 0x3B,
        0xE8, 0x73, 0x21, 0x3B, 0xBA, 0x84, 0x00, 0x00, 0x00, 0x73, 0x19, 0x8B,
        0x8A, 0x80, 0x00, 0x00, 0x00, 0x0F, 0xAF, 0xCF, 0x03, 0x4A, 0x7C, 0x80,
        0x3C, 0x29, 0x00, 0x74, 0x07, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x48,
        0x33, 0xD2, 0xEB, 0x44, 0x8B, 0x82, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFD,
        0x05, 0xC1, 0xFF, 0x05, 0x3B, 0xE8, 0x73, 0x08, 0x3B, 0xBA, 0x84, 0x00,
        0x00, 0x00, 0x72, 0x04, 0x33, 0xD2, 0xEB, 0x28, 0x8B, 0x92, 0x80, 0x00,
        0x00, 0x00, 0x8B, 0x86, 0x73, 0x42, 0x01, 0x00, 0x0F, 0xAF, 0xD7, 0x03,
        0xD5, 0x33, 0xFF, 0x66, 0x8B, 0x3C, 0x50, 0xB8, 0x01, 0x00, 0x00, 0x00,
        0xD3, 0xE0, 0x8B, 0xD7, 0x23, 0xD0, 0xF7, 0xDA, 0x1B, 0xD2, 0xF7, 0xDA,
        0x85, 0xD2, 0x75, 0x11, 0x8A, 0x86, 0x43, 0x2A, 0x00, 0x00, 0x8A, 0x4B,
        0x5C, 0x3A, 0xC8, 0x0F, 0x85, 0x29, 0x01, 0x00, 0x00, 0x8B, 0x54, 0x24,
        0x14, 0x8B, 0x44, 0x24, 0x24, 0x33, 0xC9, 0x8A, 0x4A, 0x0E, 0x8B, 0x54,
        0x24, 0x10, 0x51, 0x8B, 0x4C, 0x24, 0x24, 0x50, 0x51, 0x52, 0xE8, 0x65,
        0x7B, 0x05, 0x00, 0xE9, 0x00, 0x01, 0x00, 0x00, 0x8A, 0x86, 0x43, 0x2A,
        0x00, 0x00, 0x8B, 0xC8, 0x81, 0xE1, 0xFF, 0x00, 0x00, 0x00, 0x8B, 0xD1,
        0xC1, 0xE2, 0x05, 0x03, 0xD1, 0x8D, 0x1C, 0x0E, 0x8D, 0x14, 0x92, 0x8D,
        0x94, 0x53, 0x63, 0x1B, 0x00, 0x00, 0x8A, 0x9E, 0x81, 0x42, 0x01, 0x00,
        0x80, 0xE3, 0x02, 0x80, 0xFB, 0x02, 0x75, 0x35, 0x8B, 0x8A, 0x80, 0x00,
        0x00, 0x00, 0xC1, 0xFD, 0x05, 0xC1, 0xFF, 0x05, 0x3B, 0xE9, 0x73, 0x21,
        0x3B, 0xBA, 0x84, 0x00, 0x00, 0x00, 0x73, 0x19, 0x8B, 0x8A, 0x80, 0x00,
        0x00, 0x00, 0x0F, 0xAF, 0xCF, 0x03, 0x4A, 0x7C, 0x80, 0x3C, 0x29, 0x00,
        0x74, 0x07, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x48, 0x33, 0xD2, 0xEB,
        0x44, 0x8B, 0x9A, 0x80, 0x00, 0x00, 0x00, 0xC1, 0xFD, 0x05, 0xC1, 0xFF,
        0x05, 0x3B, 0xEB, 0x73, 0x08, 0x3B, 0xBA, 0x84, 0x00, 0x00, 0x00, 0x72,
        0x04, 0x33, 0xD2, 0xEB, 0x28, 0x8B, 0x92, 0x80, 0x00, 0x00, 0x00, 0x33,
        0xDB, 0x0F, 0xAF, 0xD7, 0x8B, 0xBE, 0x73, 0x42, 0x01, 0x00, 0x03, 0xD5,
        0x66, 0x8B, 0x1C, 0x57, 0xBF, 0x01, 0x00, 0x00, 0x00, 0xD3, 0xE7, 0x8B,
        0xD3, 0x23, 0xD7, 0xF7, 0xDA, 0x1B, 0xD2, 0xF7, 0xDA, 0x8B, 0x5C, 0x24,
        0x18, 0x85, 0xD2, 0x75, 0x0B, 0x8B, 0x4B, 0x48, 0x38, 0x81, 0xFF, 0x00,
        0x00, 0x00, 0x75, 0x4A, 0x8B, 0x54, 0x24, 0x24, 0x8B, 0x44, 0x24, 0x20,
        0x33, 0xC9, 0x52, 0x8A, 0x4B, 0x5C, 0x50, 0x8B, 0xC1, 0xC1, 0xE0, 0x05,
        0x03, 0xC1, 0x8D, 0x14, 0x80, 0x8B, 0xC6, 0x03, 0xC1, 0x8B, 0x8C, 0x50,
        0x8A, 0x1B, 0x00, 0x00, 0x8B, 0x86, 0xE7, 0x47, 0x01, 0x00, 0x33, 0xD2,
        0x8A, 0x91, 0x96, 0x00, 0x00, 0x00, 0x52, 0x50, 0xE8, 0x3B, 0x0B, 0x05,
        0x00, 0x50, 0x8B, 0x4C, 0x24, 0x1C, 0x51, 0xE8, 0x90, 0x0B, 0x05, 0x00,
        0x8B, 0x35, 0xE8, 0x1D, 0x51, 0x00, 0x8B, 0x44, 0x24, 0x28, 0x8B, 0x7C,
        0x24, 0x1C, 0x8B, 0x8E, 0xF3, 0x41, 0x01, 0x00, 0x40, 0x83, 0xC7, 0x6B,
        0x83, 0xC3, 0x6B, 0x3B, 0xC1, 0x89, 0x44, 0x24, 0x28, 0x89, 0x7C, 0x24,
        0x1C, 0x89, 0x5C, 0x24, 0x18, 0x0F, 0x8C, 0x9A, 0xFD, 0xFF, 0xFF
    };
    static const unsigned int dotVa[4] = { 0x0046725F, 0x00467294, 0x00467340, 0x00467375 };
    static const unsigned char lead[4] = { 0x8B, 0x5C, 0x24, 0x18 };   /* mov ebx,[esp+0x18] */
    FIXSITE s[27];
    int k;
    for (k = 0; k < 23; k++)
        if (memcmp((const void*)(size_t)(row[k].va - row[k].pre), row[k].stock,
                   (size_t)(row[k].pre + row[k].n)) != 0)
            return FIX_BYTES;
    if (memcmp((const void*)0x004671C0, dots, sizeof dots) != 0) return FIX_BYTES;
    for (k = 0; k < 27; k++) {
        unsigned char* a;
        int ok;
        if (k < 23) {
            if (!(a = fix_code(LOS_STUB_LEN(row[k].nz, 0)))) return FIX_STUB;
            ok = los_row_stub(a, &row[k]);
            s[k].va = row[k].va;
            s[k].n = row[k].n;
            memcpy(s[k].was, row[k].stock + row[k].pre, row[k].n);
        } else {
            const unsigned char* test = dots + (dotVa[k - 23] - 0x004671C0u);
            int late = k >= 25;
            if (!(a = fix_code(LOS_STUB_LEN(4, late ? 4 : 0)))) return FIX_STUB;
            ok = los_stub(a, dotVa[k - 23], 8, test, dots + (0x004671E5u - 0x004671C0u), 4,
                          late ? lead : NULL, late ? 4 : 0, LOS_RADAR);
            s[k].va = dotVa[k - 23];
            s[k].n = 8;
            memcpy(s[k].was, test, 8);
        }
        if (!ok) return FIX_STUB;
        fix_branch(&s[k], 0xE9, a);
    }
    return fix_write(s, 27);
}

/* ===== THE WEAPON KEYS (C2) ===============================================================
   Landing C2 of research/notes/tadr-port/data-keys.md; the disassembly is in
   data-keys-evidence.md, Part 1, and exe-reverse-engineering.md. TADR's WeaponTags,
   NotToAir, SurfaceFire and TerrainFireGate are the prior art; the design is ours. The keys'
   store and every decision are in tagpu_datakeys.c; these are the five sites that ask it,
   all in the fail-closed table, in both builds (CLASS: simulation -- a player whose build
   lacked them would play stock rules for their own units). Each re-executes the engine's own
   instructions when the weapon it is about carries no key, so stock content runs stock's
   bytes.

   - 0x42E310, the weapon load's entry (`sub esp,0x120`; its one caller 0x4918BB): the store
     emptied before the load assigns it (the read is in wpn_loader_id, at 0x42E468).
   - 0x49ABB0, the can-engage test's entry (`mov eax,[esp+0xC]; sub esp,0xC`), stdcall
     (unit, target, idx), `ret 0xC`, 11 callers: wk_check takes its place with the same
     signature, asks stock's body through a trampoline (or the extra-weapons module's for a
     slot past 2 while it is armed) and filters the verdict by the slot's weapon. It is the
     entry's one owner: the extra-weapons module no longer hooks it, and asks its own
     verdicts through it.
   - 0x43F1D4, the order action's unit branch (`mov eax,[edi+0x110]`; reached only from
     0x43F17C, on its first byte): edi the target, ebp the shooter. The branch's decision is
     tagpu_datakeys_order's when weapon 0 or slot 1 carries a key: refuse 0x4401DC, the
     no-action exit 0x43F26C, or on at 0x43F27A with edx the target's def, which stock holds
     there from 0x43F1F1 and reads at 0x43F2AA.
   - 0x49B9EB, the guidance (`test eax,0x10000; je 0x49BA16`): esi the weapon; a
     surfacefire weapon steers, 0x49BA16.
   - 0x49E1FD, AutoAim's fire gate (`mov eax,[ebx+0x111]`, after the target read 0x49E1E1
     and the fire-function test 0x49E1F2; reached from 0x49E1F7's fall-through only): ebx the
     weapon, edi the unit, esi the slot's state byte, [esp+0x10] the slot index (0..2, the
     loop's own counter). Held: on to the next slot 0x49E541; dropped: the aiming bit
     cleared first, as stock clears it at 0x49E1EA for a lost target. */
static int (__stdcall *s_wkCheckStock)(char* unit, char* target, unsigned int idx);

static int __stdcall wk_check(char* u, char* t, unsigned int idx)
{
    unsigned int i = idx & 0xFF;
    const char* w = NULL;
    int v;
    if (i >= 3 && tagpu_weapons_armed()) {
        v = tagpu_weapons_check_slot(u, t, idx);
        w = tagpu_weapons_slot_weapon(u, i);
    } else {
        v = s_wkCheckStock(u, t, idx);
        if (i < 3) w = *(const char* const*)(u + 4 + i * 0x1C + 0x0C);
    }
    return tagpu_datakeys_engage(u, t, w, v);
}

static int __stdcall wk_gate(char* u, int idx, const char* w)
{
    return tagpu_datakeys_fire_gate(u, idx, w, u + 4 + idx * 0x1C);
}

static unsigned char* wk_jmp(unsigned char* p, unsigned char op, unsigned int target)
{
    *p++ = op; tagpu_detour_rel(p, target); return p + 4;
}
static unsigned char* wk_jcc(unsigned char* p, unsigned char cc, unsigned int target)
{
    *p++ = 0x0F; *p++ = cc; tagpu_detour_rel(p, target); return p + 4;
}

static int fix_weapon_keys(void)
{
    static const unsigned char load[6]  = { 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00 };
    static const unsigned char check[7] = { 0x8B, 0x44, 0x24, 0x0C, 0x83, 0xEC, 0x0C };
    static const unsigned char order[6] = { 0x8B, 0x87, 0x10, 0x01, 0x00, 0x00 };
    static const unsigned char steer[7] = { 0xA9, 0x00, 0x00, 0x01, 0x00, 0x74, 0x24 };
    static const unsigned char gate[6]  = { 0x8B, 0x83, 0x11, 0x01, 0x00, 0x00 };
    unsigned char* tr = fix_code(16);
    unsigned char* cl = fix_code(24);
    unsigned char* co = fix_code(80);
    unsigned char* cs = fix_code(48);
    unsigned char* cg = fix_code(64);
    unsigned char* p;
    unsigned char* j;
    unsigned char now[8];
    if (!tr || !cl || !co || !cs || !cg) { lim_no_stub(); return FIX_TABLE; }

    /* 0x42E310: pushad; call clear; popad; sub esp,0x120; jmp 0x42E316 */
    p = cl;
    *p++ = 0x60;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)tagpu_datakeys_weapons_clear);
    *p++ = 0x61;
    memcpy(p, load, 6); p += 6;
    wk_jmp(p, 0xE9, 0x0042E316u);

    /* 0x49ABB0: the stolen prologue, then on at 0x49ABB7 -- stock's body for wk_check */
    memcpy(tr, check, 7);
    wk_jmp(tr + 7, 0xE9, 0x0049ABB7u);
    s_wkCheckStock = (int (__stdcall*)(char*, char*, unsigned int))(void*)tr;

    /* 0x43F1D4: pushad; push edi; push ebp; call order; mov [esp+0x1C],eax; popad;
       cmp eax,1; je 0x4401DC; cmp eax,3; je 0x43F26C; cmp eax,2; jne stock;
       mov edx,[edi+0x92]; jmp 0x43F27A; stock: mov eax,[edi+0x110]; jmp 0x43F1DA */
    p = co;
    *p++ = 0x60; *p++ = 0x57; *p++ = 0x55;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)tagpu_datakeys_order);
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x1C;
    *p++ = 0x61;
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x01; p = wk_jcc(p, 0x84, 0x004401DCu);
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x03; p = wk_jcc(p, 0x84, 0x0043F26Cu);
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x02;
    *p++ = 0x75; j = p++;
    *p++ = 0x8B; *p++ = 0x97; *p++ = 0x92; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    p = wk_jmp(p, 0xE9, 0x0043F27Au);
    *j = (unsigned char)(p - (j + 1));
    memcpy(p, order, 6); p += 6;
    wk_jmp(p, 0xE9, 0x0043F1DAu);

    /* 0x49B9EB: push eax; push ecx; push edx; push esi; call steer; test eax,eax;
       pop edx; pop ecx; pop eax; jnz 0x49BA16; test eax,0x10000; jz 0x49BA16;
       jmp 0x49B9F2 (pop leaves the flags as the test set them) */
    p = cs;
    *p++ = 0x50; *p++ = 0x51; *p++ = 0x52; *p++ = 0x56;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)tagpu_datakeys_steer);
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x5A; *p++ = 0x59; *p++ = 0x58;
    p = wk_jcc(p, 0x85, 0x0049BA16u);
    memcpy(p, steer, 5); p += 5;
    p = wk_jcc(p, 0x84, 0x0049BA16u);
    wk_jmp(p, 0xE9, 0x0049B9F2u);

    /* 0x49E1FD: pushad; push ebx; push [esp+0x34] (the site's [esp+0x10]); push edi;
       call wk_gate; mov [esp+0x1C],eax; popad; test eax,eax; jz go; cmp eax,2; jne hold;
       and byte [esi],0xFE; hold: jmp 0x49E541; go: mov eax,[ebx+0x111]; jmp 0x49E203 */
    p = cg;
    *p++ = 0x60; *p++ = 0x53;
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x34;
    *p++ = 0x57;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)wk_gate);
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x1C;
    *p++ = 0x61;
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x74; j = p++;
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x02;
    *p++ = 0x75; *p++ = 0x03;
    *p++ = 0x80; *p++ = 0x26; *p++ = 0xFE;
    p = wk_jmp(p, 0xE9, 0x0049E541u);
    *j = (unsigned char)(p - (j + 1));
    memcpy(p, gate, 6); p += 6;
    wk_jmp(p, 0xE9, 0x0049E203u);

#define WK_SITE(va, n, stock, target, name) do { \
        unsigned int rel_ = (unsigned int)(size_t)(target) - ((va) + 5u); \
        memset(now, 0x90, sizeof now); now[0] = 0xE9; memcpy(now + 1, &rel_, 4); \
        lim_add((va), (n), (stock), now, (name)); } while (0)
    WK_SITE(0x0042E310u, 6, load,  cl, "weapon keys: the store emptied at the weapon load");
    WK_SITE(0x0049ABB0u, 7, check, wk_check, "weapon keys: the can-engage verdict");
    WK_SITE(0x0043F1D4u, 6, order, co, "weapon keys: the order action's unit branch");
    WK_SITE(0x0049B9EBu, 7, steer, cs, "weapon keys: surfacefire's guidance");
    WK_SITE(0x0049E1FDu, 6, gate,  cg, "weapon keys: the fire gate");
#undef WK_SITE
    return FIX_TABLE;
}

/* ===== STALE HITS: THE INCARNATION ON THE WIRE, AND THE TWO-TICK HOLD ======================
   Landing B4 of research/notes/tadr-port/sim-fixes.md ("B4 DESIGN" has the argument in full;
   the addresses are in exe-reverse-engineering.md, "Unit identity on the wire").

   THE DEFECT [DISASSEMBLED]. A hit is computed on the attacker's peer against its copy of the
   victim and sent as a 0x0B naming the victim by SLOT alone (0x489BB0: the local apply at
   0x489C89, the send at 0x489CB9 / 0x489CCD). The owner's allocator is first-free (0x486036), so
   a slot freed by a death is taken by the owner's next create at once, and a hit still in
   flight lands on the new unit, on the owner and on every bystander.

   THE FIX, three parts.
   - The incarnation: per slot, the owner's GameTime at the create, made strictly increasing
     per slot (hit_next_birth), and a copy's stamp of it: exact for a copy made from the
     owner's create message, a LOWER BOUND ("the unit in this slot at the owner's time g0",
     bit 31) for a copy the 0x2C made, whose own header carries g0.
   - Containment on the wire: the 0x09 and the 0x0B travel INSIDE tagged 0x05 messages that
     carry the stamp in the same bytes (A′3's idiom), so no loss or reordering can separate a
     record from its stamp. A bare 0x09 or 0x0B cannot come from this build and is dropped.
   - The hold: first-free does not take a slot freed in this tick or the last while the block
     has another free slot; with none, it takes the one freed longest ago, if that was in an
     earlier tick, and never one freed in this tick.
   THE RULES: the owner applies a hit iff its unit's birth <= the hit's stamp; a bystander
   refuses only a hit provably aimed at another incarnation (hit_owner_accepts,
   hit_bystander_rule).

   CLASS: simulation, fail closed, both builds -- a peer without it applies hits this build
   refuses. Every site is a row of the fail-closed table. */

#define HIT_TAG_CREATE  0x4A                /* outside TADR's 0x2B..0x31 and 0x60, and A′3's 0x49 */
#define HIT_TAG_HIT     0x4B
#define HIT_LB          0x80000000u         /* a stamp's bit 31: a lower bound, not a birth     */
#define HIT_UNKNOWN     HIT_LB              /* no information: the owner reads it as "alive at
                                               time 0", a bystander as undecidable              */
#define HIT_ROOM_MIN    16384u              /* a table's least room: 10 x 1500 + 1 fits          */
#define HIT_MSG         0x41                /* the 0x05 length, the table 0x512AD8 + 5*4         */
#define HIT_DELAY_MAX   512
#define HIT_GAMETIME(ta) (*(const unsigned int*)((ta) + 0x38A47))
#define HIT_SEND(net, m, n) (((int (__stdcall*)(unsigned int, const void*, unsigned int))0x00451DF0)((net), (m), (n)))

enum { HIT_BY_STALE, HIT_BY_SAME, HIT_BY_UNDECIDED };

/* THE PER-SLOT TABLES, sized from the engine's own slot count. 0x4854A0 sets u16 main+0x14351
   to 10 x main+0x37EE6 + 1 in 16-bit arithmetic (0x4854E3..0x4854EF) and allocates that many
   slots; hit_reset, in place of its first two instructions, computes the same count the same
   way, and every index into a table is checked against that table's n before it is used. The
   engine's count is a u16, so n < 65536 in either build, raised or not.

   LIFETIME: a table is NEVER freed. A table is published whole through the one store to s_hit,
   n and the arrays together, and a level whose count exceeds the room gets a new, larger table
   while the old one stays allocated; a pointer any thread read before the swap still addresses
   memory this DLL owns, bounded by that table's own n (n <= room). The room is a power of two
   from 16384, so at most three tables are ever made.

   THREADS: the loading thread (0x497C70 -> 0x497180) runs the level init that resets the table
   (0x497581 -> 0x4917D0 -> 0x4854A0) and the level's first creates (0x496EE0, 0x4977BB), which
   take and stamp; meanwhile the main thread pumps the network in state 5, where the gate (0x09,
   0x0B, 0x2C pass in state 6 only, applied by hit_gate before any table access) keeps the
   dispatch out of the tables. In play the creates, the frees, the dispatch and the hits run on
   the thread that runs the tick and pumps the network (0x4954C8) [INFERRED from the tick's
   order]; memory safety does not rest on that, only on the lifetime above. */
struct hit_tab {
    unsigned int n;           /* the level's slots                                   */
    unsigned int room;        /* each array's length, n <= room                      */
    unsigned int* stamp;      /* a local unit's birth, or a copy's stamp             */
    unsigned int* freed;      /* GameTime of the slot's last free + 1; 0 never       */
    unsigned int* local;      /* local GameTime at the create: the test oracle       */
};
static struct hit_tab* volatile s_hit;
static unsigned int s_hitTables;

/* counters for the heartbeat's hits: section; u32, one writer thread */
static unsigned int s_hitOutCreate, s_hitOutHit, s_hitOutBytes, s_hitStockBytes, s_hitTxUnknown;
static unsigned int s_hitInCreate, s_hitInHit, s_hitStateRefused, s_hitMalformed;
static unsigned int s_hitApplyOwner, s_hitRefuseOwner, s_hitDead;
static unsigned int s_hitApplyBy, s_hitUndecidedBy, s_hitRefuseBy;
static unsigned int s_hitBare09, s_hitBare0B;
static unsigned int s_hitHeld, s_hitFallback, s_hitHoldFail, s_hitRetry;
static unsigned int s_hitCreateExact, s_hitCreateLB, s_hitCreateUnknown;
static unsigned int s_hitYoungOwner, s_hitYoungBy, s_hitDelayed, s_hitDelayOverflow;

/* TEST LEVER tagpu_dmgdelay.on=K: every outgoing hit waits until GameTime has advanced K
   ticks and leaves at the next send opportunity (an outgoing hit, an incoming companion on the
   game thread). 0 is off. The oracle it feeds, per receiver: a hit applied to a unit whose
   local create is younger than K ticks. */
static unsigned int s_hitDelayK;
static struct { unsigned int net, due; unsigned char m[HIT_MSG]; } s_hitDelay[HIT_DELAY_MAX];
static unsigned int s_hitDelayHead, s_hitDelayCount;
static unsigned int s_hitTakeNotes;          /* the lever's creation trace, capped */

/* ---- the pure rules; tagpu_wirecheck.on exercises them on boundary values ------------------ */

static int hit_owner_accepts(unsigned int birth, unsigned int stamp)
{
    return !(birth & HIT_LB) && birth <= (stamp & ~HIT_LB);
}

/* A bystander refuses only a hit provably aimed at another incarnation; every value is the
   owner's GameTime. Both exact: the same unit iff equal. One exact birth b and one lower bound
   g0 (the unit alive at g0, so born at or before it): a unit born after g0 is another one. Two
   bounds, or no information: undecidable, applied as stock applies it. Applying cannot kill a
   remote copy: 0x489CE0 sets pending death only for a victim whose player is local (0x489EC6),
   and the owner's round robin rewrites a copy's hit points (0x48B4B2). */
static int hit_bystander_rule(unsigned int copy, unsigned int stamp)
{
    unsigned int b, g0;
    if (copy == HIT_UNKNOWN || stamp == HIT_UNKNOWN) return HIT_BY_UNDECIDED;
    if (!(copy & HIT_LB) && !(stamp & HIT_LB)) return copy == stamp ? HIT_BY_SAME : HIT_BY_STALE;
    if ((copy & HIT_LB) && (stamp & HIT_LB)) return HIT_BY_UNDECIDED;
    b  = (copy & HIT_LB) ? stamp : copy;
    g0 = ((copy & HIT_LB) ? copy : stamp) & ~HIT_LB;
    return b > g0 ? HIT_BY_STALE : HIT_BY_UNDECIDED;
}

/* freed1 = the free's GameTime + 1, 0 for never; unsigned, so a stamp from before a GameTime
   reset (now < f) never holds */
static int hit_held(unsigned int freed1, unsigned int now)
{
    return freed1 != 0 && now - (freed1 - 1u) < 2u;
}

/* a held slot the fallback may take: freed in an EARLIER tick. Birth > free time is what the
   owner's rule needs of a reused slot; a slot freed in this tick is never taken. */
static int hit_reusable(unsigned int freed1, unsigned int now)
{
    return freed1 != 0 && freed1 - 1u < now;
}

/* The next birth in a slot: its GameTime, and past the previous one. GameTime stays below
   2^31 (a tick at speed 20 is 1/60 s: 414 days), and each create adds at most one, so a birth
   never reaches bit 31. */
static unsigned int hit_next_birth(unsigned int prev, unsigned int now)
{
    now &= ~HIT_LB;
    if (!(prev & HIT_LB) && now <= prev) return prev + 1u;
    return now;
}

/* ---- the engine side --------------------------------------------------------------------- */

static int hit_slot_index(const struct hit_tab* t, const char* ta, const char* slot,
                          unsigned int* idx)
{
    const char* first = ta ? *(const char* const*)(ta + 0x14357) : NULL;
    size_t off;
    if (!t || !first || !slot || slot < first) return 0;
    off = (size_t)(slot - first);
    if (off % 0x118 || off / 0x118 >= t->n) return 0;
    *idx = (unsigned int)(off / 0x118);
    return 1;
}

static void hit_delay_flush(unsigned int now)
{
    while (s_hitDelayCount && (int)(now - s_hitDelay[s_hitDelayHead].due) >= 0) {
        HIT_SEND(s_hitDelay[s_hitDelayHead].net, s_hitDelay[s_hitDelayHead].m, HIT_MSG);
        s_hitDelayHead = (s_hitDelayHead + 1u) % HIT_DELAY_MAX;
        s_hitDelayCount--;
    }
}

/* Without the tables no create can be stamped and no hit judged, and a peer that stops judging
   applies hits the others refuse: the process ends here rather than play on unfixed. */
static void hit_no_memory(unsigned int room)
{
    tagpu_logf("enginefix: stale hits: no memory for a table of %u slots -- the game closes", room);
    MessageBoxA(NULL, "Total Annihilation: Impure ran out of memory while starting the level, "
                "so the game will now close.", "Total Annihilation: Impure",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    ExitProcess(1);
}

static struct hit_tab* hit_tab_new(unsigned int n)
{
    unsigned int room = HIT_ROOM_MIN;
    struct hit_tab* t;
    while (room < n) room *= 2u;
    t = (struct hit_tab*)VirtualAlloc(NULL, sizeof *t + 3u * room * sizeof(unsigned int),
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!t) hit_no_memory(room);
    t->room  = room;
    t->stamp = (unsigned int*)(t + 1);
    t->freed = t->stamp + room;
    t->local = t->freed + room;
    s_hitTables++;
    return t;
}

/* In place of 0x4854A0's first two instructions, before the unit array is allocated: every
   stamp starts unknown with the array it describes, never keyed to GameTime. */
static void __cdecl hit_reset(unsigned int* regs)
{
    const char* ta = *(const char* const*)0x00511DE8;
    unsigned int n = (unsigned short)(*(const unsigned short*)(ta + 0x37EE6) * 10u + 1u);
    struct hit_tab* t = s_hit;
    unsigned int i;
    (void)regs;
    if (!t || n > t->room) t = hit_tab_new(n);
    for (i = 0; i < t->room; i++) t->stamp[i] = HIT_UNKNOWN;
    memset(t->freed, 0, t->room * sizeof *t->freed);
    memset(t->local, 0, t->room * sizeof *t->local);
    t->n = n;
    s_hit = t;
    s_hitDelayHead = s_hitDelayCount = 0;
}

/* At 0x486036, the first free slot of the player's block that first-free meets (esi; the stub
   has tested type 0; ebp is the block's last slot, [player+0x6B]). edx's low word is 0x485F50's
   arg 8: nonzero asks for that slot, and only first-free holds. Returns 1 to take esi, which it
   may move to a later free slot of the block through regs; 0 with esi at the block's end, where
   0x486040's step ends the loop and the create returns NULL, as at the unit cap. */
static int __cdecl hit_take(unsigned int* r)
{
    struct hit_tab* t = s_hit;
    const char* ta = *(const char* const*)0x00511DE8;
    const char* first = (const char*)(size_t)r[PR_ESI];
    const char* last = (const char*)(size_t)r[PR_EBP];
    const char* take = first;
    unsigned int idx, now;
    if (!hit_slot_index(t, ta, first, &idx)) return 1;   /* esi is a slot of the array: never */
    now = HIT_GAMETIME(ta);
    if ((r[PR_EDX] & 0xFFFF) == 0 && hit_held(t->freed[idx], now)) {
        const char* s;
        const char* older = NULL;
        unsigned int oldest = 0, j;
        take = NULL;
        for (s = first; s <= last; s += 0x118) {
            if (*(const unsigned short*)(s + 0xA6) || !hit_slot_index(t, ta, s, &j)) continue;
            if (!hit_held(t->freed[j], now)) { take = s; break; }
            if (hit_reusable(t->freed[j], now) && (!older || t->freed[j] < oldest)) {
                older = s;
                oldest = t->freed[j];
            }
        }
        if (take) s_hitHeld++;
        else if (older) { take = older; s_hitFallback++; }
        else {
            s_hitHoldFail++;
            r[PR_ESI] = r[PR_EBP];
            return 0;
        }
        r[PR_ESI] = (unsigned int)(size_t)take;
        hit_slot_index(t, ta, take, &idx);
    }
    t->stamp[idx] = hit_next_birth(t->stamp[idx], now);
    t->local[idx] = now;
    if (s_hitDelayK && s_hitTakeNotes < 20000u) {
        unsigned int stock = idx;
        s_hitTakeNotes++;
        hit_slot_index(t, ta, first, &stock);
        tagpu_logf("enginefix: stale hits: take %u (first-free %u) at %u", idx, stock, now);
    }
    return 1;
}

/* At 0x486DC1, the destructor's store of type 0 (esi the slot), local and remote alike. */
static void __cdecl hit_freed(unsigned int* r)
{
    struct hit_tab* t = s_hit;
    const char* ta = *(const char* const*)0x00511DE8;
    unsigned int idx;
    if (hit_slot_index(t, ta, (const char*)(size_t)r[PR_ESI], &idx))
        t->freed[idx] = HIT_GAMETIME(ta) + 1u;
}

/* The owner's time g0 of the 0x2C being parsed, as a lower bound. A 0x2C's creates are in its
   sender's block (a dirty entry at +0x67 + delta, the round robin at +0x67 + GameTime % N), so
   the record whose block holds the slot is the sender, whose +0x18 holds the header's [32]
   field before the first entry: wire_s2c_entry stores it in place of stock's 0x48B963, after
   the copy and length checks, and a 0x2C that fails them creates nothing. The blocks go to the ten records in DPID order
   in a network game (0x485842 sorts the record pointers on +4 before 0x4858BD hands out
   1 + k*N), not in record order, so the record is found by its block, [+0x67, +0x6B]: the
   range first-free walks. */
static int hit_2c_bound(const char* ta, const char* slot, unsigned int* bound)
{
    unsigned int k;
    for (k = 0; k < 10u; k++) {
        const char* rec = ta + 0x1B63 + k * 0x14B;
        const char* lo = *(const char* const*)(rec + 0x67);
        const char* hi = *(const char* const*)(rec + 0x6B);
        if (lo && lo <= slot && slot <= hi) {
            *bound = HIT_LB | (*(const unsigned int*)(rec + 0x18) & ~HIT_LB);
            return 1;
        }
    }
    return 0;
}

/* At CreateFromNetwork's success exit 0x48634F (esi the unit; its caller's return address at
   the site's esp + 0x1C and its record argument at + 0x24, as 0x4861ED reads it). 0x4553E9 is
   the 0x09 case, reached only through our 0x05 slot (its jump-table entry is the one reference
   to 0x4553DA in the image), so the record is a carried one, with its birth 23 bytes on;
   0x48BA05 and 0x48B49C are the 0x2C's dirty entry and round robin. */
static void __cdecl hit_created(unsigned int* r)
{
    struct hit_tab* t = s_hit;
    const char* ta = *(const char* const*)0x00511DE8;
    const char* slot = (const char*)(size_t)r[PR_ESI];
    unsigned int ret = *(const unsigned int*)(WPN_ESP_JMP(r) + 0x1C);
    const unsigned char* rec = *(const unsigned char* const*)(WPN_ESP_JMP(r) + 0x24);
    unsigned int idx, stamp = HIT_UNKNOWN;
    if (!hit_slot_index(t, ta, slot, &idx)) return;
    if (ret == 0x004553E9u && (rec[3] | (unsigned int)rec[4] << 8) == idx) {
        memcpy(&stamp, rec + 23, 4);
        s_hitCreateExact++;
    } else if ((ret == 0x0048BA05u || ret == 0x0048B49Cu) && hit_2c_bound(ta, slot, &stamp)) {
        s_hitCreateLB++;
    } else {
        s_hitCreateUnknown++;
    }
    t->stamp[idx] = stamp;
    t->local[idx] = HIT_GAMETIME(ta);
}

/* In place of 0x456050's `call 0x451DF0` (0x4560AE), with its signature: the unit's 0x09,
   carried with its birth. */
static int __stdcall hit_tx_create(unsigned int net, const unsigned char* msg, unsigned int len)
{
    const struct hit_tab* t = s_hit;
    unsigned char m[HIT_MSG];
    unsigned int idx = msg[3] | (unsigned int)msg[4] << 8;
    unsigned int birth = t && idx < t->n ? t->stamp[idx] : HIT_UNKNOWN;
    memset(m, 0, sizeof m);
    m[0] = 0x05;
    m[2] = HIT_TAG_CREATE;
    memcpy(m + 3, msg, len < 23u ? len : 23u);
    memcpy(m + 26, &birth, 4);
    s_hitOutCreate++;
    s_hitOutBytes += HIT_MSG;
    s_hitStockBytes += len;
    return HIT_SEND(net, m, sizeof m);
}

/* In place of 0x489BB0's two `call 0x451DF0` (0x489CB9, 0x489CCD), with its signature: the
   0x0B, carried with this peer's stamp of its copy of the victim. */
static int __stdcall hit_tx_hit(unsigned int net, const unsigned char* msg, unsigned int len)
{
    const struct hit_tab* t = s_hit;
    const char* ta = *(const char* const*)0x00511DE8;
    unsigned char m[HIT_MSG];
    unsigned int vidx = msg[1] | (unsigned int)msg[2] << 8;
    unsigned int stamp = t && vidx && vidx < t->n ? t->stamp[vidx] : HIT_UNKNOWN;
    memset(m, 0, sizeof m);
    m[0] = 0x05;
    m[2] = HIT_TAG_HIT;
    memcpy(m + 3, msg, len < 9u ? len : 9u);
    memcpy(m + 12, &stamp, 4);
    if (stamp == HIT_UNKNOWN) s_hitTxUnknown++;
    s_hitOutHit++;
    s_hitOutBytes += HIT_MSG;
    s_hitStockBytes += len;
    if (s_hitDelayK && ta) {
        unsigned int now = HIT_GAMETIME(ta);
        hit_delay_flush(now);
        if (s_hitDelayCount < HIT_DELAY_MAX) {
            unsigned int q = (s_hitDelayHead + s_hitDelayCount) % HIT_DELAY_MAX;
            s_hitDelay[q].net = net;
            s_hitDelay[q].due = now + s_hitDelayK;
            memcpy(s_hitDelay[q].m, m, sizeof m);
            s_hitDelayCount++;
            s_hitDelayed++;
            return 1;
        }
        s_hitDelayOverflow++;
    }
    return HIT_SEND(net, m, sizeof m);
}

/* The dispatcher's type gate (0x45473F), for the type a companion carries: the same table,
   the same state bits. */
static int hit_gate(const char* ta, unsigned int type)
{
    unsigned int state = *(const unsigned int*)(ta + 0x391F1);
    unsigned int bits = *(const unsigned int*)(size_t)(0x00512BC0u + 4u * type);
    if (state == 5) return (bits & 2) != 0;
    if (state == 6) return (bits & 4) != 0;
    return (bits & 1) != 0;
}

static int hit_young(const struct hit_tab* t, const char* ta, unsigned int idx)
{
    return s_hitDelayK && HIT_GAMETIME(ta) - t->local[idx] < s_hitDelayK;
}

/* 0 stock's chat; 1 done; 2 CreateFromNetwork on the record; 3 0x489CE0 on the record, which
   goes back to the stub in regs[PR_EAX], where popad puts it in eax */
static int hit_rx_create(unsigned int* regs, const char* ta, const unsigned char* m)
{
    if (m[3] != 0x09) { s_hitMalformed++; return 1; }
    if (!hit_gate(ta, 0x09)) { s_hitStateRefused++; return 1; }
    s_hitInCreate++;
    regs[PR_EAX] = (unsigned int)(size_t)(m + 3);
    return 2;
}

static int hit_rx_hit(unsigned int* regs, const char* ta, const unsigned char* m)
{
    const struct hit_tab* t = s_hit;
    const char* first;
    const char* last;
    const char* slot;
    const char* pl;
    unsigned int vidx, stamp, flags;
    if (m[3] != 0x0B) { s_hitMalformed++; return 1; }
    if (!hit_gate(ta, 0x0B)) { s_hitStateRefused++; return 1; }
    s_hitInHit++;
    if (s_hitDelayK && GetCurrentThreadId() == s_wpnGameTid) hit_delay_flush(HIT_GAMETIME(ta));
    regs[PR_EAX] = (unsigned int)(size_t)(m + 3);
    vidx = m[4] | (unsigned int)m[5] << 8;
    memcpy(&stamp, m + 12, 4);
    first = *(const char* const*)(ta + 0x14357);
    last  = *(const char* const*)(ta + 0x1435B);
    /* an index stock refuses (0), or past the array (B3's bound at 0x489CED): theirs to drop */
    if (!t || !first || last < first || vidx == 0 || vidx >= t->n ||
        vidx > (unsigned int)(last - first) / 0x118)
        return 3;
    slot = first + (size_t)vidx * 0x118;
    flags = *(const unsigned int*)(slot + 0x110);
    if (!(flags & 0x10000000u) || (flags & 0x4000u)) { s_hitDead++; return 3; }   /* 0x489D45 */
    pl = *(const char* const*)(slot + 0x96);
    if (pl && *(const unsigned int*)pl && (pl[0x73] == 1 || pl[0x73] == 2)) {     /* 0x489EC6 */
        if (!hit_owner_accepts(t->stamp[vidx], stamp)) { s_hitRefuseOwner++; return 1; }
        s_hitApplyOwner++;
        if (hit_young(t, ta, vidx)) s_hitYoungOwner++;
    } else {
        switch (hit_bystander_rule(t->stamp[vidx], stamp)) {
        case HIT_BY_STALE: s_hitRefuseBy++; return 1;
        case HIT_BY_SAME:  s_hitApplyBy++; break;
        default:           s_hitUndecidedBy++; break;
        }
        if (hit_young(t, ta, vidx)) s_hitYoungBy++;
    }
    return 3;
}

/* The dispatch table's 0x05 slot 0x455F90, entered by the dispatcher's `jmp [eax*4+0x455F84]`
   with its frame: the message at the site's esp + 0x10. */
static int __cdecl hit_rx_chat(unsigned int* regs)
{
    const unsigned char* m = *(const unsigned char* const*)(WPN_ESP_JMP(regs) + 0x10);
    const char* ta = *(const char* const*)0x00511DE8;
    if (m[1] != 0 || !ta) return 0;
    switch (m[2]) {
#ifndef TAGPU_LIMITS_STOCK
    case WPN_CHAT_TAG:   wpn_rx_chat(regs); return 0;
#endif
    case HIT_TAG_CREATE: return hit_rx_create(regs, ta, m);
    case HIT_TAG_HIT:    return hit_rx_hit(regs, ta, m);
    default:             return 0;
    }
}

/* ---- stubs, in B4's own page ------------------------------------------------------------- */

static unsigned char* s_hitCode;
static unsigned int   s_hitCodeUsed;

static unsigned char* hit_code(unsigned int n)
{
    unsigned char* p;
    if (!s_hitCode)
        s_hitCode = (unsigned char*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_EXECUTE_READWRITE);
    if (!s_hitCode || s_hitCodeUsed + n > 0x1000) return NULL;
    p = s_hitCode + s_hitCodeUsed;
    s_hitCodeUsed += (n + 15u) & ~15u;
    return p;
}

static unsigned char* hit_jmp(unsigned char* p, unsigned char op, unsigned int target)
{
    *p++ = op;
    tagpu_detour_rel(p, target);
    return p + 4;
}

static unsigned char* hit_jcc(unsigned char* p, unsigned char cc, unsigned int target)
{
    *p++ = 0x0F;
    *p++ = cc;
    tagpu_detour_rel(p, target);
    return p + 4;
}

static unsigned char* hit_abs(unsigned char* p, const void* a)
{
    unsigned int v = (unsigned int)(size_t)a;
    memcpy(p, &v, 4);
    return p + 4;
}

/* E8/E9 at `va` to `target`, NOP-padded to n, into the table */
static void hit_site(unsigned int va, int n, const unsigned char* stock, unsigned char op,
                     const void* target, const char* name)
{
    lim_branch(va, n, stock, op, (unsigned int)(size_t)target, name);
}

static void hit_slot(unsigned int va, unsigned int stock, const void* target, const char* name)
{
    unsigned int v = (unsigned int)(size_t)target;
    lim_add(va, 4, (const unsigned char*)&stock, (const unsigned char*)&v, name);
}

static void hit_selfcheck(void)
{
    struct { const char* name; int got; int want; } t[24];
    int n = 0, bad = 0, i;
    t[n].name = "owner: birth = stamp";     t[n].got = hit_owner_accepts(100, 100);           t[n].want = 1; n++;
    t[n].name = "owner: older stamp";       t[n].got = hit_owner_accepts(100, 99);            t[n].want = 0; n++;
    t[n].name = "owner: bound after birth"; t[n].got = hit_owner_accepts(100, HIT_LB | 150);  t[n].want = 1; n++;
    t[n].name = "owner: bound before";      t[n].got = hit_owner_accepts(100, HIT_LB | 99);   t[n].want = 0; n++;
    t[n].name = "owner: unknown, first";    t[n].got = hit_owner_accepts(0, HIT_UNKNOWN);     t[n].want = 1; n++;
    t[n].name = "owner: unknown, later";    t[n].got = hit_owner_accepts(5, HIT_UNKNOWN);     t[n].want = 0; n++;
    t[n].name = "owner: own stamp unknown"; t[n].got = hit_owner_accepts(HIT_UNKNOWN, 7);     t[n].want = 0; n++;
    t[n].name = "by: exact, equal";         t[n].got = hit_bystander_rule(42, 42);            t[n].want = HIT_BY_SAME; n++;
    t[n].name = "by: exact, differ";        t[n].got = hit_bystander_rule(42, 43);            t[n].want = HIT_BY_STALE; n++;
    t[n].name = "by: copy bound, born after"; t[n].got = hit_bystander_rule(HIT_LB | 50, 51); t[n].want = HIT_BY_STALE; n++;
    t[n].name = "by: copy bound, born at";  t[n].got = hit_bystander_rule(HIT_LB | 50, 50);   t[n].want = HIT_BY_UNDECIDED; n++;
    t[n].name = "by: hit bound, born after"; t[n].got = hit_bystander_rule(51, HIT_LB | 50);  t[n].want = HIT_BY_STALE; n++;
    t[n].name = "by: hit bound, born before"; t[n].got = hit_bystander_rule(49, HIT_LB | 50); t[n].want = HIT_BY_UNDECIDED; n++;
    t[n].name = "by: two bounds";           t[n].got = hit_bystander_rule(HIT_LB | 9, HIT_LB | 70); t[n].want = HIT_BY_UNDECIDED; n++;
    t[n].name = "by: copy unknown";         t[n].got = hit_bystander_rule(HIT_UNKNOWN, 70);   t[n].want = HIT_BY_UNDECIDED; n++;
    t[n].name = "by: hit unknown";          t[n].got = hit_bystander_rule(70, HIT_UNKNOWN);   t[n].want = HIT_BY_UNDECIDED; n++;
    t[n].name = "hold: same tick";          t[n].got = hit_held(10 + 1, 10);                  t[n].want = 1; n++;
    t[n].name = "hold: next tick";          t[n].got = hit_held(10 + 1, 11);                  t[n].want = 1; n++;
    t[n].name = "hold: two ticks on";       t[n].got = hit_held(10 + 1, 12);                  t[n].want = 0; n++;
    t[n].name = "hold: never / reset";      t[n].got = hit_held(0, 3) || hit_held(50 + 1, 3); t[n].want = 0; n++;
    t[n].name = "fallback: earlier tick";   t[n].got = hit_reusable(10 + 1, 11);              t[n].want = 1; n++;
    t[n].name = "fallback: this tick";      t[n].got = hit_reusable(10 + 1, 10) || hit_reusable(0, 10); t[n].want = 0; n++;
    t[n].name = "birth: rises per slot";    t[n].got = hit_next_birth(HIT_UNKNOWN, 7) == 7 &&
                                                       hit_next_birth(7, 7) == 8 && hit_next_birth(8, 20) == 20 &&
                                                       hit_next_birth(20, 3) == 21;           t[n].want = 1; n++;
    for (i = 0; i < n; i++) {
        if (t[i].got != t[i].want) bad++;
        tagpu_logf("enginefix: hitcheck %-27s got=%d want=%d %s",
                   t[i].name, t[i].got, t[i].want, t[i].got == t[i].want ? "OK" : "FAIL");
    }
    tagpu_logf("enginefix: hitcheck %d rule cases, %d failed", n, bad);
}

static void hit_read_lever(void)
{
    FILE* f = fopen("tagpu_dmgdelay.on", "rb");
    char b[16] = { 0 };
    unsigned int k;
    if (!f) return;
    fread(b, 1, sizeof b - 1, f);
    fclose(f);
    k = (unsigned int)strtoul(b, NULL, 10);
    s_hitDelayK = k ? (k > 3600u ? 3600u : k) : 30u;
    tagpu_logf("enginefix: stale hits: TEST LEVER tagpu_dmgdelay.on -- every outgoing hit is "
               "held %u ticks", s_hitDelayK);
}

static int fix_stale_hits(void)
{
    static const unsigned char take[10]  = { 0x66, 0x83, 0xBE, 0xA6, 0x00, 0x00, 0x00, 0x00,
                                             0x74, 0x1D };                   /* 0x486036 */
    static const unsigned char freed[6]  = { 0x8B, 0xAE, 0x10, 0x01, 0x00, 0x00 };
    static const unsigned char exitCfn[5] = { 0x8B, 0xC6, 0x5F, 0x5E, 0x5D };
    static const unsigned char alloc[8]  = { 0x83, 0xEC, 0x30, 0xA1, 0xE8, 0x1D, 0x51, 0x00 };
    static const unsigned char tx09[5]   = { 0xE8, 0x3D, 0xBD, 0xFF, 0xFF };
    static const unsigned char tx0bA[5]  = { 0xE8, 0x32, 0x81, 0xFC, 0xFF };
    static const unsigned char tx0bB[5]  = { 0xE8, 0x1E, 0x81, 0xFC, 0xFF };
    static const unsigned char spawn[8]  = { 0x8B, 0xF0, 0x33, 0xC0, 0x8B, 0x7C, 0x24, 0x34 };
    static const unsigned char back09[5] = { 0xE9, 0x62, 0x0B, 0x00, 0x00 };  /* 0x4553E9 */
    static const unsigned char back0b[5] = { 0xE9, 0x34, 0x0B, 0x00, 0x00 };  /* 0x455417 */
    static const unsigned char len09[2]  = { 0x6A, 0x17 };                    /* 0x45605C */
    static const unsigned char len0b[2]  = { 0x6A, 0x09 };                    /* 0x489CA8 */
    unsigned char *aTake, *aFree, *aExit, *aAlloc, *aChat, *aBare09, *aBare0B, *aSpawn, *p,
                  *doCreate, *doHit, *jCreate, *jHit, *jNone;

    hit_read_lever();
    if (GetFileAttributesA("tagpu_wirecheck.on") != INVALID_FILE_ATTRIBUTES) hit_selfcheck();

    if (!(aTake = hit_code(64)) || !(aFree = hit_code(32)) || !(aExit = hit_code(32)) ||
        !(aAlloc = hit_code(32)) || !(aChat = hit_code(96)) || !(aBare09 = hit_code(16)) ||
        !(aBare0B = hit_code(16)) || !(aSpawn = hit_code(48))) {
        lim_no_stub();
        return FIX_TABLE;
    }

    /* 0x486036: cmp word [esi+0xA6],0; jne 0x486040; take esi as hit_take leaves it, or end */
    p = aTake;
    memcpy(p, take, 8); p += 8;
    p = hit_jcc(p, 0x85, 0x00486040);
    *p++ = 0x60; *p++ = 0x54;
    p = hit_jmp(p, 0xE8, (unsigned int)(size_t)hit_take);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x61;
    p = hit_jcc(p, 0x84, 0x00486040);
    hit_jmp(p, 0xE9, 0x0048605D);

    /* 0x486DC1: stamp the free, then stock's mov ebp,[esi+0x110] */
    p = fix_call_regs(aFree, hit_freed);
    memcpy(p, freed, 6); p += 6;
    hit_jmp(p, 0xE9, 0x00486DC7);

    /* 0x48634F: stamp the copy, then stock's mov eax,esi; pop edi; pop esi; pop ebp */
    p = fix_call_regs(aExit, hit_created);
    memcpy(p, exitCfn, 5); p += 5;
    hit_jmp(p, 0xE9, 0x00486354);

    /* 0x4854A0: reset with the array, then stock's sub esp,0x30; mov eax,[0x511DE8] */
    p = fix_call_regs(aAlloc, hit_reset);
    memcpy(p, alloc, 8); p += 8;
    hit_jmp(p, 0xE9, 0x004854A8);

    /* the 0x05 slot: 0 stock chat, 1 the loop, 2 the carried 0x09, 3 the carried 0x0B, each
       entered with the return address stock's own case pushes and the record in eax */
    p = aChat;
    *p++ = 0x60; *p++ = 0x54;
    p = hit_jmp(p, 0xE8, (unsigned int)(size_t)hit_rx_chat);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x02;                /* cmp eax,2          */
    *p++ = 0x74; jCreate = p++;                          /* je do_create       */
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x03;                /* cmp eax,3          */
    *p++ = 0x74; jHit = p++;                             /* je do_hit          */
    *p++ = 0x85; *p++ = 0xC0;                            /* test eax,eax       */
    *p++ = 0x61;                                         /* popad              */
    p = hit_jcc(p, 0x84, 0x0045522E);                    /* jz stock's chat    */
    p = hit_jmp(p, 0xE9, 0x00455F50);                    /* the loop           */
    doCreate = p;
    *p++ = 0x61;                                         /* popad: eax = record */
    *p++ = 0x8B; *p++ = 0x4C; *p++ = 0x24; *p++ = 0x14;  /* mov ecx,[esp+0x14] */
    *p++ = 0x50; *p++ = 0x51;                            /* push eax; push ecx */
    *p++ = 0x68; p = hit_abs(p, (const void*)0x004553E9); /* push 0x4553E9     */
    p = hit_jmp(p, 0xE9, 0x004861D0);
    doHit = p;
    *p++ = 0x61;                                         /* popad: eax = record */
    *p++ = 0x50;                                         /* push eax           */
    *p++ = 0x68; p = hit_abs(p, (const void*)0x00455417); /* push 0x455417     */
    hit_jmp(p, 0xE9, 0x00489CE0);
    *jCreate = (unsigned char)(doCreate - (jCreate + 1));
    *jHit = (unsigned char)(doHit - (jHit + 1));

    /* a bare 0x09 / 0x0B: counted, and the loop goes on */
    p = aBare09;
    *p++ = 0xFF; *p++ = 0x05; p = hit_abs(p, &s_hitBare09);
    hit_jmp(p, 0xE9, 0x00455F50);
    p = aBare0B;
    *p++ = 0xFF; *p++ = 0x05; p = hit_abs(p, &s_hitBare0B);
    hit_jmp(p, 0xE9, 0x00455F50);

    /* 0x4653DE, after the Deathmatch respawn's create 0x4653D9, whose unit it uses at 0x465414
       with no test: stock's mov esi,eax; xor eax,eax; mov edi,[esp+0x34] when there is one.
       With none, the block's own end 0x4654FB is taken, whose stack is this site's (0x496E90
       ret 0xC, 0x4816A0 and 0x48D630 ret 4) and which restores edi, bl and ebp. NULL comes from
       the block with no free slot, from the hold (every free slot freed in this tick), and --
       below the cap too, stock faulting here on each -- from a type 0 (0x485F7C), a type
       without def+0x241 bit 0x800000 (0x485FAA) and the type's own limit def+0x15A (0x485FE4).
       NOTHING IS WRITTEN: the countdown main+0x39239 stays at -1, as stock's fire leaves it, so
       every reader of it (0x46554F, which gates 0x401360, above all) sees stock's own sequence, and
       while the respawn's trigger still holds, its own countdown reloads (0x46518B) and fires
       again six of the controlled player's passes later -- the retry, with its placement
       search. */
    p = aSpawn;
    *p++ = 0x8B; *p++ = 0xF0;                            /* mov esi,eax        */
    *p++ = 0x85; *p++ = 0xF6;                            /* test esi,esi       */
    *p++ = 0x74; jNone = p++;                            /* jz none            */
    *p++ = 0x33; *p++ = 0xC0;                            /* xor eax,eax        */
    *p++ = 0x8B; *p++ = 0x7C; *p++ = 0x24; *p++ = 0x34;  /* mov edi,[esp+0x34] */
    p = hit_jmp(p, 0xE9, 0x004653E6);
    *jNone = (unsigned char)(p - (jNone + 1));
    *p++ = 0xFF; *p++ = 0x05; p = hit_abs(p, &s_hitRetry);          /* inc [s_hitRetry] */
    hit_jmp(p, 0xE9, 0x004654FB);

    hit_site(0x00486036, 10, take, 0xE9, aTake, "stale hits: the hold, first-free's free test");
    hit_site(0x00486DC1, 6, freed, 0xE9, aFree, "stale hits: the free's stamp");
    hit_site(0x0048634F, 5, exitCfn, 0xE9, aExit, "stale hits: a copy's stamp, CreateFromNetwork's exit");
    hit_site(0x004854A0, 8, alloc, 0xE9, aAlloc, "stale hits: the reset, with the unit array");
    hit_site(0x004560AE, 5, tx09, 0xE8, (const void*)hit_tx_create, "stale hits: the 0x09 sent carried");
    hit_site(0x00489CB9, 5, tx0bA, 0xE8, (const void*)hit_tx_hit, "stale hits: the 0x0B sent carried");
    hit_site(0x00489CCD, 5, tx0bB, 0xE8, (const void*)hit_tx_hit, "stale hits: the 0x0B sent carried, no attacker");
    hit_site(0x004653DE, 8, spawn, 0xE9, aSpawn, "stale hits: the Deathmatch respawn's create, tested");
    hit_slot(0x00455F90, 0x0045522Eu, aChat, "stale hits: the 0x05 receiver");
    hit_slot(0x00455FA0, 0x004553DAu, aBare09, "stale hits: a bare 0x09 dropped");
    hit_slot(0x00455FA8, 0x0045540Du, aBare0B, "stale hits: a bare 0x0B dropped");
    lim_same(0x004553E9, 5, back09, "stale hits: the 0x09 case's continuation");
    lim_same(0x00455417, 5, back0b, "stale hits: the 0x0B case's continuation");
    lim_same(0x0045605C, 2, len09, "stale hits: the 0x09's length");
    lim_same(0x00489CA8, 2, len0b, "stale hits: the 0x0B's length");
    return FIX_TABLE;
}

/* the heartbeat's hits section (tagpu_packet_pub.c): DLL counters only. owner= is
   applied/refused, by= applied as the same unit/applied undecidable/refused as stale; B= is
   the companions' bytes and stockB= what the bare messages would have been; copy= counts the
   stamps CreateFromNetwork's exit took, by kind; held= creates the hold moved to an unheld
   slot, fallback= to a slot freed in the last tick, holdfail= creates it failed, retry= the
   Deathmatch respawns that found no slot and wait for their countdown's next fire. */
int tagpu_hits_format(char* buf, unsigned int cap)
{
    const struct hit_tab* t = s_hit;
    return _snprintf(buf, cap,
                     " | hits: slots=%u tables=%u out 09=%u 0b=%u B=%u stockB=%u unk=%u"
                     " in 09=%u 0b=%u owner=%u/%u by=%u/%u/%u dead=%u gate=%u bad=%u"
                     " bare 09=%u 0b=%u copy exact=%u bound=%u unk=%u held=%u fallback=%u"
                     " holdfail=%u retry=%u delay=%u q=%u over=%u young owner=%u by=%u",
                     t ? t->n : 0u, s_hitTables, s_hitOutCreate, s_hitOutHit, s_hitOutBytes,
                     s_hitStockBytes, s_hitTxUnknown, s_hitInCreate, s_hitInHit, s_hitApplyOwner,
                     s_hitRefuseOwner, s_hitApplyBy, s_hitUndecidedBy, s_hitRefuseBy, s_hitDead,
                     s_hitStateRefused, s_hitMalformed, s_hitBare09, s_hitBare0B,
                     s_hitCreateExact, s_hitCreateLB, s_hitCreateUnknown, s_hitHeld,
                     s_hitFallback, s_hitHoldFail, s_hitRetry, s_hitDelayK, s_hitDelayed,
                     s_hitDelayOverflow, s_hitYoungOwner, s_hitYoungBy);
}

static void patch_engine_defects(void)
{
    int sort = fix_sort_buffer_end();
    int plot = fix_feature_null_plot();
    int terr = fix_terrain_window();
    int die  = fix_feature_die_pool_full();
    int mark = fix_reclaim_mark_anchor();
    int sfb  = fix_saved_features_border();
    int rro  = fix_restore_record_owner();
    int scr  = fix_composite_scratch();
    int list = fix_build_list();
    int dl   = fix_download_records();
    int oom  = fix_oom_message();
    int keys = fix_sync_keys();
    int wpn  = fix_weapon_ids();
    int caps = fix_victim_caps();
    int air  = fix_stacked_air();
    int flak = fix_flak_divides();
    int edge = fix_last_cell();
    int los  = fix_los_shear();
    int losl = fix_los_local();
    int pview = fix_projectile_view();
    int wkey = fix_weapon_keys();
    int wire = fix_wire_bounds();
    int hits = fix_stale_hits();
    char b[2048], fn[LOS_NFN * 9 + 1];
    int k;

    if (GetFileAttributesA("tagpu_wirecheck.on") != INVALID_FILE_ATTRIBUTES)
        wire_selfcheck();

    _snprintf(b, sizeof b,
              "enginefix: sort-buffer end bound 0x469807 %s; NULL-plot guard 0x421E60 %s; "
              "terrain window bound 0x484057 %s; feature swap on a full wreck pool 0x423651 %s; "
              "reclaim tests the anchor's mark 0x423892 %s; saved features restored under the "
              "border mask (0x43265A) %s; a saved feature's state written only into the record it "
              "owns (0x4250C0 0x425185) %s; composite scratch bound "
              "(0x4589C0 0x45A470 0x45A790 0x459830 0x459C70 0x4B90A0) %s; whole build lists "
              "(0x42DA58 0x42DAC7 0x42BEC3) %s; download menus past five entries (0x42DCF0) %s; "
              "the out-of-memory text (0x49E700) %s; unique unit sync keys (0x42BD29) %s; weapon IDs "
              "bounded, feature hits told from sentinels (0x42E468 0x49D280 0x455FB8 0x424575) %s; "
              "one hit a victim an explosion (0x49A0A9 0x49A109 0x49A262 0x49A5CE) %s; flak's "
              "divides (0x49CF18 0x42F314 0x42F32E) %s; the map's last row and column "
              "(0x47CC8B 0x47CCA3 0x47CCA9) %s. Counters: unit repeats "
              "refused at 0x%08X, "
              "feature repeats "
              "refused at 0x%08X, victims refused off their arrays at 0x%08X, list blocks run "
              "unwrapped at 0x%08X, flak fallbacks at 0x%08X",
              fix_state(sort), fix_state(plot), fix_state(terr), fix_state(die), fix_state(mark),
              fix_state(sfb), fix_state(rro), fix_state(scr), fix_state(list), fix_state(dl),
              fix_state(oom), fix_state(keys), fix_state(wpn), fix_state(caps), fix_state(flak),
              fix_state(edge),
              (unsigned int)(size_t)&s_dmgUnitRepeats, (unsigned int)(size_t)&s_dmgFeatRepeats,
              (unsigned int)(size_t)&s_dmgRefused, (unsigned int)(size_t)&s_dmgUnframed,
              (unsigned int)(size_t)&s_flakFallbacks);
    b[sizeof b - 1] = 0;
    plog(b);

    for (k = 0; k < LOS_NFN; k++) _snprintf(fn + 9 * k, 10, " 0x%06X", s_losFn[k]);
    fn[sizeof fn - 1] = 0;
    _snprintf(b, sizeof b,
              "enginefix: line of sight at the map's edge, in the table (0x465B6A 0x465C04 "
              "0x465CA2 0x465D46 0x465DA9 0x408095 0x407F74; the sight emitter 0x482615; the order resolver 0x43F5D1 "
              "0x43F631 0x43FC05 0x43FD1A 0x43FF85 0x4400AA; the view player's map 0x46778A "
              "0x4677D0) %s; local (the cursor 0x43E69D 0x43E904 0x43EBC6 0x43ECDB 0x43EE94 "
              "0x43EFA9; the build cursor 0x47D3B8; the feature helper 0x465942 0x46598A "
              "0x465A17 0x465A63; the radar's projectile dots 0x46725F 0x467294 0x467340 "
              "0x467375; the particles 0x47360C 0x473657 0x473A94 0x473AD3 0x4741EC 0x474237 "
              "0x474674 0x4746BB 0x47551E 0x47556C; sound 0x47F431 0x47F476) %s; the projectile pass (0x49BEE8) %s. Own row "
              "taken at 0x%08X, one LONG per function:%s",
              fix_state(los), fix_state(losl), fix_state(pview),
              (unsigned int)(size_t)s_losOwnRow, fn);
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: stacked aircraft served to area damage after its walk (0x49A664 "
              "0x49A415; the pool: the stamp 0x47CF98, the unit tick's call 0x4954ED) %s. "
              "Aircraft served, counted at 0x%08X",
              fix_state(air), (unsigned int)(size_t)&s_airServed);
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: the weapon keys -- nottoair, nottounderwater, surfacefire, notoverwater, "
              "notoverland (the store 0x42E310 0x42E468; the verdict 0x49ABB0; the order action "
              "0x43F1D4; the guidance 0x49B9EB; the fire gate 0x49E1FD) %s",
              fix_state(wkey));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: stale hits %s: the 0x09 and 0x0B carried in tagged 0x05s with the "
              "victim's incarnation (0x4560AE 0x489CB9 0x489CCD; the receiver 0x455F90; bare ones "
              "dropped 0x455FA0 0x455FA8), a copy's stamp at CreateFromNetwork's exit (0x48634F), "
              "the two-tick hold (0x486036 0x486DC1), reset with the unit array (0x4854A0), "
              "the Deathmatch respawn's create tested (0x4653DE). "
              "Counters on the heartbeat's 'hits:' section. Stubs: %u of 4096 bytes at 0x%08X",
              fix_state(hits), s_hitCodeUsed, (unsigned int)(size_t)s_hitCode);
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: wire robustness %s: 0x09 create -- index, type, the sender's block "
              "(0x4861F7), 0x0A attach (0x4553FE), 0x0C destroy + killer (0x4866E5 0x486753), "
              "0x0B damage (0x489CED), "
              "0x2C stat/move -- parsed from a zero-padded copy (0x48B92B), the message's length "
              "(the receive's 0x453595 0x45361F, which also keeps the pump's message pointer on "
              "the buffer), block, delta, type, dirty-list move class, after-create, the "
              "round-robin flag and type, and the unsigned round-robin remainder (0x48B960 "
              "0x48B985 0x48B9AD 0x48BA05 0x48BA5E 0x48B40E 0x48B49C 0x48BA9F), a move "
              "payload's target and the round robin's carrier (0x44E0DE 0x48B574); the splitter's "
              "short messages (0x463939 0x463B33); the diverged 0x0D dropped in wpn_rx_fired "
              "(0x49D280). Records accepted, drops, the morph/dcreate/rcreate/ghost oracles and "
              "CreateFromNetwork's no-block refusals are on the heartbeat's 'wire:' section. "
              "Stubs: %u bytes",
              fix_state(wire), s_wireStubBytes);
    b[sizeof b - 1] = 0;
    plog(b);
}

void tagpu_apply_patches(void)
{
    /* Skip the startup "installed version of Microsoft DirectX may not function
       properly with Total Annihilation" warning dialog. TA's version check at
       0x004B5070 returns 0 under wine/modern DirectX, so the branch at 0x004266A7
       (jne 0x0042670A) falls into the warning block. Force it to jump always:
       0x75 (jne) -> 0xEB (jmp). Stock TA 3.1 only. */
    int ok = patch_byte(0x004266A7, 0x75, 0xEB);
    plog(ok ? "tagpu: patched out DirectX version warning (0x4266A7 jne->jmp)"
            : "tagpu: DirectX-warning patch skipped (byte mismatch — not stock 3.1?)");

    if (tagpu_regstore_active())
        close_register_switch();

    /* Contextual order cursors under Interface Type 1. Opt out with `tagpu_curs.off`,
       which — like every byte patch here — is read once, now.

       TA picks the pointer sprite in 0x43E490, whose ONLY caller is
       CorretCursor_InGame 0x48D220, so this one compare governs the cursor and
       nothing else. Its order-1 case — "no command button pressed", the contextual
       cursor — opens with

           0043E505  cmp dword [ebx+0x37EFA], 1    ; Interface Type
           0043E50C  je   0x43EB02                 ; the right-mouse-orders branch

       and 0x43EB02 can only ever return select(15) / red(17) / grn(18) / normal(19):
       under Interface Type 1 there is no cursormove over ground and no
       cursorreclamate over a wreck. `tacli` writes Interface Type = 1 into every
       instance (the only type that accepts posted clicks), so every instance loses
       them — measured: ground 19 normal / wreck 18 grn at type 1, against 14 move /
       11 reclamate at type 0.

       NOP the je and the contextual case always takes the classic branch, which
       re-dispatches to the move (0x43EDB6), attack (order 3) and reclaim (order 12)
       cases. That is NOT cursor-only by itself — the index also decides what a left
       click does, and the patch below is what keeps the promise. Enumerating the
       readers of 0x37EFA (0x499046 / 0x499162 / 0x499352 / 0x499567) misses it:
       0x499046's arm is guarded by the cursor index, so leaving the
       interface-type compares untouched does not leave the button untouched. */
    if (GetFileAttributesA("tagpu_curs.off") != INVALID_FILE_ATTRIBUTES) {
        plog("curs: contextual cursors left to the engine (tagpu_curs.off)");
    } else {
        static const unsigned char je_expect[6] = { 0x0F, 0x84, 0xF0, 0x05, 0x00, 0x00 };
        static const unsigned char six_nops[6]  = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        ok = patch_bytes(0x0043E50C, je_expect, six_nops, sizeof je_expect);
        plog(ok ? "curs: ARMED — contextual cursors on at any Interface Type "
                  "(0x43E50C je->nop x6)"
                : "curs: patch skipped (byte mismatch at 0x43E50C — not stock 3.1?)");

        /* ...and the companion that keeps it cursor-only, because on its own it is
           NOT. The index 0x43E490 returns is not consumed as a sprite id: 0x499200
           stores it in `main+0x2CBE` (0x4992AD) and the LEFT-CLICK ACTION dispatches
           on that byte —

               00499027  mov dl, [eax+0x2CBE]        ; the installed cursor index
               0049902D  cmp dl, 0x0F  / je          ; cursorselect -> select the unit
               00499041  cmp dl, 0x11
               00499044  jl  0x49906D                ; anything lower -> ISSUE THE ORDER
               00499046  cmp [eax+0x37EFA], 1        ; Interface Type
               00499053  cmp cl, 1                   ; order byte: contextual
               0049905C  ...                         ; -> deselect everything

           — so `main+0x2CBE` is the state the click reads, not a picture. The
           interface type reaches the left button only through the value in it: the
           type-1 arm at 0x43EB02 returns 15/17/18/19 and NOTHING else (verified by
           reachability over 0x43E490's 220 reachable blocks: 0x43EB56, 0x43EB67,
           0x43EB78, 0x43EC8D, 0x43EDA9, 0x43EE42, 0x43F09B are its only returns), so
           stock "left click at type 1" is exactly "15 selects, everything else
           deselects". Feed it the classic 14 `cursormove` instead and `jl` fires:
           the left button starts issuing move orders next to the right button's.
           Measured on `one-unit` / Two Continents, commander selected, type 1:
           left-click on ground walked it to the clicked point and kept it selected;
           the same click with `tagpu_curs.off` deselected (ARMCOM1.GUI ->
           ARMMAIN2.GUI) and moved nothing.

           So decide the contextual left click on the interface type itself, which is
           what the index was standing in for, and leave the classic ordering path to
           the index as before:

               00499041  cmp [eax+0x37EFA], 1        ; Interface Type
               00499048  jne 0x499051                ; type 0: classic, decide on dl
               0049904A  cmp cl, 1                   ; order byte: contextual?
               0049904D  jne 0x499051                ; a pressed command button acts
               0049904F  jmp 0x49905C                ; -> deselect (stock type-1 rule)
               00499051  cmp dl, 0x11
               00499054  jl  0x49906D                ; -> issue the order
               00499056  jmp 0x4990F6                ; -> nothing

           27 bytes for 27, and equivalent to stock on a stock cursor state: at type 0
           it is the original two instructions in the original order, and at type 1 the
           only indexes the engine can put in `main+0x2CBE` are 15 (taken at 0x49902D,
           above this), 17, 18, 19 and the hourglass 20 (0x49258B / 0x497F94) — all
           >= 0x11, all of which stock deselects when the order byte is 1. */
        if (ok) {
            static const unsigned char click_expect[27] = {
                0x80, 0xFA, 0x11,                          /* cmp dl,0x11         */
                0x7C, 0x27,                                /* jl  0x49906D        */
                0x83, 0xB8, 0xFA, 0x7E, 0x03, 0x00, 0x01,  /* cmp [eax+0x37EFA],1 */
                0x0F, 0x85, 0xA3, 0x00, 0x00, 0x00,        /* jne 0x4990F6        */
                0x80, 0xF9, 0x01,                          /* cmp cl,1            */
                0x0F, 0x85, 0x9A, 0x00, 0x00, 0x00,        /* jne 0x4990F6        */
            };
            static const unsigned char click_patch[27] = {
                0x83, 0xB8, 0xFA, 0x7E, 0x03, 0x00, 0x01,  /* cmp [eax+0x37EFA],1 */
                0x75, 0x07,                                /* jne 0x499051        */
                0x80, 0xF9, 0x01,                          /* cmp cl,1            */
                0x75, 0x02,                                /* jne 0x499051        */
                0xEB, 0x0B,                                /* jmp 0x49905C        */
                0x80, 0xFA, 0x11,                          /* cmp dl,0x11         */
                0x7C, 0x17,                                /* jl  0x49906D        */
                0xE9, 0x9B, 0x00, 0x00, 0x00,              /* jmp 0x4990F6        */
                0x90,                                      /* pad to 0x49905C     */
            };
            ok = patch_bytes(0x00499041, click_expect, click_patch,
                             sizeof click_expect);
            if (ok)
            {
                plog("curs: left click decided by Interface Type, not by the "
                     "cursor index (0x499041, 27 bytes)");
            }
            else
            {
                /* THE PAIR IS ARMED TOGETHER OR NOT AT ALL. The cursor patch on
                   its own IS the bug — it feeds 14 to a click handler that reads
                   anything under 0x11 as "issue the order". So if the companion
                   will not take, put the cursor patch back and run stock rather
                   than ship the thing this pair exists to fix. */
                int back = patch_bytes(0x0043E50C, six_nops, je_expect,
                                       sizeof je_expect);
                plog(back ? "curs: DISARMED — no left-click patch at 0x499041 "
                            "(byte mismatch), so 0x43E50C was put back; stock "
                            "cursors, stock buttons"
                          : "curs: STUCK — 0x499041 would not take and 0x43E50C "
                            "would not revert; the left button may issue orders");
            }
        }
    }

    patch_engine_defects();
}

/* ===== THE RAISED LIMITS (tagpu_limits.h) ===================================================
   The engine limits TADR raises -- the effect pools as its EngineLimits.cpp does, the unit
   limit and the pathfinding budget as its LimitCrack.cpp does -- re-derived from the
   pristine 3.1 image (research/notes/tadr-port/limits-evidence.md §1-6 holds every site's
   disassembly). Every site goes into the fail-closed table (top of this file), with the
   simulation fixes' sites, and is compared and written with them. */


#ifndef TAGPU_LIMITS_STOCK

/* ---- the pools that move out of the engine ---------------------------------------------
   Process-lifetime statics: longer-lived than stock's per-level block, which is the better
   lifetime for anything that reads a record after the level that wrote it. */
#define EXPL_REC      0x54
#define AUX_REC       0x34
#define AUX_VERTS     0x60   /* 8 vertices of 12 bytes                                 */
#define AUX_FACES     0xC0   /* 6 face records of 0x20                                 */
#define AUX_NFACE     6
#define PROJ_REC      0x6B

/* THE LAYOUT IS THE ENGINE'S: the count, then the records straight after it. The add site
   0x420A3C takes the COUNT's address as its base and reaches the records at +4. */
static struct {
    int           count;
    unsigned char rec[TAGPU_LIM_EXPL][EXPL_REC];
} s_expl;
static void*         s_psys[TAGPU_LIM_PSYS];
static unsigned char s_aux[TAGPU_LIM_AUX][AUX_REC];
static unsigned char s_auxVert[TAGPU_LIM_AUX][AUX_VERTS];
static unsigned char s_auxFace[TAGPU_LIM_AUX][AUX_FACES];

static void put32(unsigned char* p, unsigned int v) { memcpy(p, &v, 4); }
static unsigned int get32(const unsigned char* p) { unsigned int v; memcpy(&v, p, 4); return v; }

/* The debris-record allocator, in place of 0x420920 and of its inline copy at 0x4217DE.
   FIRST-FREE, AS STOCK'S IS: below stock's 300 the records are handed out exactly as stock
   hands them out, and nothing is carried from one game to the next. The engine frees a
   record by writing 0xFF over its first byte through the pointer it was given. */
static unsigned char* __cdecl lim_aux_alloc(void)
{
    int i;
    for (i = 0; i < TAGPU_LIM_AUX; i++)
        if (s_aux[i][0] == 0xFF) { s_aux[i][0] = 0; return s_aux[i]; }
    return NULL;
}

/* In place of the level load's `mov ecx,0x64 / xor eax,eax / mov edi,0x511DF0 / rep stosd`
   at 0x42090A, which clears the flying-piece slots: clear ours, and initialise the debris
   records exactly as the loop at 0x4207FB..0x4208F9 initialises stock's (that loop still
   runs, over the engine's own array, which nothing uses any more). Returns 0 because stock
   leaves eax = 0 on the way out of the function. */
static int __cdecl lim_level_reset(void)
{
    int i, k;
    memset(s_psys, 0, sizeof s_psys);
    for (i = 0; i < TAGPU_LIM_AUX; i++) {
        unsigned char* r = s_aux[i];
        r[0] = 0xFF;
        put32(r + 0x04, 8);
        put32(r + 0x08, 6);
        put32(r + 0x0C, 0xFFFFFFFFu);
        put32(r + 0x1C, 0x00502C28u);
        put32(r + 0x20, 0);
        put32(r + 0x24, (unsigned int)(size_t)s_auxVert[i]);
        put32(r + 0x28, (unsigned int)(size_t)s_auxFace[i]);
        for (k = 0; k < AUX_NFACE; k++) {
            unsigned char* f = s_auxFace[i] + k * 0x20;
            put32(f + 0x1C, get32(f + 0x1C) | 1u);
            put32(f + 0x00, 0xC8);
            put32(f + 0x04, 4);
            put32(f + 0x0C, 0x00502BF8u + (unsigned int)k * 8u);
        }
    }
    return 0;
}

/* THE PROJECTILE COMPACTION FRAME. 0x49AE20 keeps two i16 index arrays on its stack, one
   entry per slot ([esp+0x20+i*2] and [esp+0x278+i*2] after its four pushes), so its frame
   grows from 0x4C0 to 0x20 - 0x10 + 4 * slots bytes. The frame is larger than a page, so
   it is committed a page at a time, touching each new page at esp before going further —
   the __chkstk rule, correct by construction: no touch is ever more than a page below the
   last one. Both callers (0x49BE53, 0x49C8F4) are on the game thread. */
#define PROJ_FRAME    (0x20 - 0x10 + 4 * TAGPU_LIM_PROJ)
#define PROJ_ARRAY2   (0x20 + 2 * TAGPU_LIM_PROJ)
/* the probe below commits two whole pages and then the rest, which must be one page or less */
typedef char lim_frame_check[(PROJ_FRAME > 0x2000 && PROJ_FRAME <= 0x3000) ? 1 : -1];

static unsigned char* lim_probe_stub(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    static const unsigned char page[9] = { 0x81, 0xEC, 0x00, 0x10, 0x00, 0x00,   /* sub esp,0x1000 */
                                           0x85, 0x04, 0x24 };                   /* test [esp],eax */
    unsigned int last = PROJ_FRAME - 0x2000;
    if (!s) return NULL;
    memcpy(p, page, 9); p += 9;
    memcpy(p, page, 9); p += 9;
    *p++ = 0x81; *p++ = 0xEC; memcpy(p, &last, 4); p += 4;                       /* sub esp,rest   */
    *p++ = 0x85; *p++ = 0x04; *p++ = 0x24;                                       /* test [esp],eax */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049AE26u);                               /* jmp back       */
    return s;
}

/* THE EXPLOSION SEQUENCE TABLE STAYS IN THE ENGINE. 0x420AA2 reads it as
   `[edi + eax*4 + 0x6274]` because stock's pool sits right in front of it; with the pool
   moved, edi no longer reaches it, so the load is a call that finds the table from the main
   pointer: main+0x1AB8F = main+0x1491B+0x6274. Only eax changes. */
static unsigned char* lim_seqtab_stub(void)
{
    static const unsigned char code[15] = {
        0x52,                                     /* push edx                           */
        0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00,       /* mov edx,[0x511DE8]                 */
        0x8B, 0x84, 0x82, 0x8F, 0xAB, 0x01, 0x00, /* mov eax,[edx+eax*4+0x1AB8F]        */
        0x5A,                                     /* pop edx                            */
    };
    unsigned char* s = tagpu_detour_stub();
    if (!s) return NULL;
    memcpy(s, code, sizeof code);
    s[sizeof code] = 0xC3;                        /* ret                                */
    return s;
}

/* A unit limit from outside the ini, clamped to [TAGPU_LIM_UNITS_MIN, TAGPU_LIM_UNITS] before
   it is stored. Called in place of the stock `mov word [ecx+off],ax`, with ecx = main as stock
   set it and eax = the whole value (a `maxunits` key's int, or the host's word zero-extended).
   The clamp runs on the whole int, so a value past 65535 cannot wrap into range the way
   stock's 16-bit store wraps it. Only eax and the flags change; at every site the next
   instruction overwrites or ignores them. */
static unsigned char* lim_maxunits_stub(unsigned int off)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    const unsigned int lo = TAGPU_LIM_UNITS_MIN, hi = TAGPU_LIM_UNITS;
    if (!s) return NULL;
    *p++ = 0x3D; memcpy(p, &lo, 4); p += 4;                      /* cmp eax,lo             */
    *p++ = 0x7D; *p++ = 0x05;                                    /* jge +5                 */
    *p++ = 0xB8; memcpy(p, &lo, 4); p += 4;                      /* mov eax,lo             */
    *p++ = 0x3D; memcpy(p, &hi, 4); p += 4;                      /* cmp eax,hi             */
    *p++ = 0x7E; *p++ = 0x05;                                    /* jle +5                 */
    *p++ = 0xB8; memcpy(p, &hi, 4); p += 4;                      /* mov eax,hi             */
    *p++ = 0x66; *p++ = 0x89; *p++ = 0x81; memcpy(p, &off, 4); p += 4;  /* mov [ecx+off],ax */
    *p++ = 0xC3;                                                 /* ret                    */
    return s;
}

/* ONE PAGE FOR THE STUBS OF THE UNIT-TYPE RAISE, carved in order: seventeen small stubs do
   not need seventeen 64 KB reservations of the address space a 16 383-type mod is short of. */
static unsigned char* s_limCode;
static unsigned int   s_limCodeUsed;

static unsigned char* lim_code(unsigned int n)
{
    unsigned char* p;
    if (!s_limCode)
        s_limCode = (unsigned char*)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_EXECUTE_READWRITE);
    if (!s_limCode || s_limCodeUsed + n > 0x1000) { s_limNoStub = 1; return NULL; }
    p = s_limCode + s_limCodeUsed;
    s_limCodeUsed += (n + 15u) & ~15u;
    return p;
}

/* A site whose widened instruction does not fit: `jmp stub` at va, NOP-padded to n, and the
   stub runs `code` and then jumps back to va + n, unless `code` ends in its own ret. Every
   such site's bytes are position-independent, so they run the same from the stub. */
static void lim_reloc(unsigned int va, int n, const unsigned char* stock,
                      const unsigned char* code, int nc, int back, const char* name)
{
    unsigned char* s = lim_code((unsigned int)nc + 5u);
    if (!s) return;
    memcpy(s, code, (size_t)nc);
    if (back) { s[nc] = 0xE9; tagpu_detour_rel(s + nc + 1, va + (unsigned int)n); }
    lim_branch(va, n, stock, 0xE9, (unsigned int)(size_t)s, name);
}

/* `op` then a 32-bit operand, into a stub's code */
static unsigned char* lim_emit(unsigned char* p, const unsigned char* op, int n, unsigned int v)
{
    memcpy(p, op, (size_t)n);
    memcpy(p + n, &v, 4);
    return p + n + 4;
}

/* A MOD WITH MORE UNIT TYPES THAN THE MASKS HOLD. Called from the menu-time count (0x42AA65),
   on the main thread, before the def array is allocated: nothing has been loaded that a
   half-run game could use, and every later count comes from this array (the game load compacts
   it, 0x42D542). Refusing is the owner's decision: dropping types would depend on the order
   the engine walks the archives, and two peers with different files would drop different
   ones. Never returns. */
static void __cdecl lim_types_refused(unsigned int slots)
{
    static char text[2048];
    _snprintf(text, sizeof text,
        "This game's files hold %u unit types, and Total Annihilation: Impure plays at most "
        "%u, so the game will now close.\r\n"
        "\r\n"
        "WHY\r\n"
        "Impure raises the game's limit from 511 unit types to %u. Past it the engine would "
        "write outside its unit tables, and leaving some types out would make this a different "
        "mod from the one installed, one that plays differently from another player's copy.\r\n"
        "\r\n"
        "WHAT TO DO\r\n"
        "Remove unit archives (.ufo, .hpi, .ccx, .gp3) from the game folder until %u unit types "
        "or fewer remain.\r\n",
        slots - 1u, (unsigned int)TAGPU_LIM_TYPES - 1u, (unsigned int)TAGPU_LIM_TYPES - 1u,
        (unsigned int)TAGPU_LIM_TYPES - 1u);
    text[sizeof text - 1] = 0;
    tagpu_logf("limits: %u unit types found, %u is the most this build plays -- refused", slots - 1u,
               (unsigned int)TAGPU_LIM_TYPES - 1u);
    MessageBoxA(NULL, text, "Total Annihilation: Impure cannot load these units",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    ExitProcess(1);
}

/* In place of `inc ebx; mov edx,[main]` at 0x42AA65: ebx is the unitinfo files found, and the
   def array about to be allocated is ebx + 1 slots, None first. */
static unsigned char* lim_types_stub(void)
{
    static const unsigned char head[3] = { 0x43, 0x81, 0xFB };          /* inc ebx; cmp ebx,imm32 */
    static const unsigned char tail[6] = { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 };  /* mov edx,[main] */
    unsigned char* s = lim_code(32);
    unsigned char* p = s;
    if (!s) return NULL;
    p = lim_emit(p, head, 3, TAGPU_LIM_TYPES);
    *p++ = 0x77; *p++ = 0x07;                                           /* ja refuse          */
    memcpy(p, tail, 6); p += 6;
    *p++ = 0xC3;                                                        /* ret                */
    *p++ = 0x53;                                                        /* refuse: push ebx   */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)&lim_types_refused); p += 4;
    return s;
}

static void lim_sites(void)
{
    const unsigned int expl = (unsigned int)(size_t)&s_expl;
    const unsigned int psys = (unsigned int)(size_t)&s_psys[0];
    const unsigned int psysEnd = (unsigned int)(size_t)&s_psys[TAGPU_LIM_PSYS];
    unsigned char* probe = lim_probe_stub();
    unsigned char* seqtab = lim_seqtab_stub();
    unsigned char* saveMax = lim_maxunits_stub(0x37EEC);
    unsigned char* missionMax = lim_maxunits_stub(0x37EE6);
    int k;

    if (!probe || !seqtab || !saveMax || !missionMax) { s_limNoStub = 1; return; }

    /* ---- projectiles: allocated per game by 0x499A30, refused past the cap at ten sites */
    lim_dword(0x00499A32, 300 * PROJ_REC, TAGPU_LIM_PROJ * PROJ_REC, "projectile pool bytes");
    lim_dword(0x00499A56, 300 * PROJ_REC / 4, TAGPU_LIM_PROJ * PROJ_REC / 4,
              "projectile pool clear dwords");
    {
        static const unsigned int caps[10] = {
            0x0049B6F0, 0x0049B80A, 0x0049C9D2, 0x0049CC34, 0x0049CDF3,
            0x0049D011, 0x0049D2BE, 0x0049D4B5, 0x0049DD96, 0x0049DF24 };
        for (k = 0; k < 10; k++)
            lim_dword(caps[k], 300, TAGPU_LIM_PROJ, "projectile cap");
    }
    {
        static const unsigned char subEsp[6] = { 0x81, 0xEC, 0xC0, 0x04, 0x00, 0x00 };
        lim_branch(0x0049AE20, 6, subEsp, 0xE9, (unsigned int)(size_t)probe,
                   "projectile compaction frame");
    }
    lim_dword(0x0049AEB8, 0x278, PROJ_ARRAY2, "projectile compaction array write");
    lim_dword(0x0049AF39, 0x278, PROJ_ARRAY2, "projectile compaction array read");
    lim_dword(0x0049AF7F, 0x4C0, PROJ_FRAME, "projectile compaction frame release");

    /* ---- explosions: the count and records move from main+0x1491B to s_expl */
    {
        static const unsigned char reset[10] = { 0xC7, 0x80, 0x1B, 0x49, 0x01, 0x00, 0, 0, 0, 0 };
        unsigned char ours[10] = { 0xC7, 0x05, 0, 0, 0, 0, 0, 0, 0, 0 };
        memcpy(ours + 2, &expl, 4);
        lim_add(0x00420630, 10, reset, ours, "explosion reset");            /* mov [pool],0   */
    }
    {
        static const unsigned char addCnt[6] = { 0x8B, 0x88, 0x1B, 0x49, 0x01, 0x00 };
        static const unsigned char addLea[6] = { 0x8D, 0xB8, 0x1B, 0x49, 0x01, 0x00 };
        static const unsigned char drwCnt[6] = { 0x8B, 0x90, 0x1B, 0x49, 0x01, 0x00 };
        static const unsigned char drwLea[6] = { 0x8D, 0x88, 0x1B, 0x49, 0x01, 0x00 };
        static const unsigned char tckLea[6] = { 0x8D, 0x98, 0x1B, 0x49, 0x01, 0x00 };
        static const unsigned char pceLea[6] = { 0x8D, 0xB0, 0x1B, 0x49, 0x01, 0x00 };
        static const unsigned char movEcxM[2] = { 0x8B, 0x0D }, movEdxM[2] = { 0x8B, 0x15 };
        static const unsigned char movEdi = 0xBF, movEcx = 0xB9, movEbx = 0xBB, movEsi = 0xBE;
        lim_op(0x00420A36, 6, addCnt, movEcxM, 2, expl, "explosion add count");  /* mov ecx,[pool] */
        lim_op(0x00420A3C, 6, addLea, &movEdi, 1, expl, "explosion add pool");   /* mov edi,pool   */
        lim_op(0x00420B35, 6, drwCnt, movEdxM, 2, expl, "explosion draw count");  /* 0x420B00 */
        lim_op(0x00420B3B, 6, drwLea, &movEcx, 1, expl, "explosion draw pool");
        lim_op(0x00420F66, 6, tckLea, &movEbx, 1, expl, "explosion tick pool");   /* 0x420F30 */
        lim_op(0x00421738, 6, pceLea, &movEsi, 1, expl, "piece explosion pool");
    }
    {
        static const unsigned char seq[7] = { 0x8B, 0x84, 0x87, 0x74, 0x62, 0x00, 0x00 };
        lim_branch(0x00420AA2, 7, seq, 0xE8, (unsigned int)(size_t)seqtab,
                   "explosion sequence table");
    }
    lim_dword(0x00420A44, 300, TAGPU_LIM_EXPL, "explosion cap");
    lim_dword(0x00421771, 300, TAGPU_LIM_EXPL, "piece explosion cap");

    /* ---- flying pieces: the slots move from 0x511DF0..0x511F80 to s_psys. The five
       `mov ecx,0x511F80` are the backing allocator's `this` (it lives right after the
       slots), not the slots' end, and stay as they are. */
    {
        static const unsigned char push[5] = { 0x68, 0xA0, 0x86, 0x01, 0x00 };
        static const unsigned char op = 0x68;
        lim_op(0x004208FB, 5, push, &op, 1, 100000u * TAGPU_LIM_PSYS / 100u,
               "flying-piece backing bytes");
    }
    {
        static const unsigned char clr[14] = { 0xB9, 0x64, 0x00, 0x00, 0x00, 0x33, 0xC0,
                                               0xBF, 0xF0, 0x1D, 0x51, 0x00, 0xF3, 0xAB };
        lim_branch(0x0042090A, 14, clr, 0xE8, (unsigned int)(size_t)&lim_level_reset,
                   "effect pools level reset");
    }
    {
        static const unsigned int base[7] = { 0x00420B08, 0x00420F38, 0x00421153, 0x00421172,
                                              0x004211A7, 0x0042165F, 0x00421680 };
        static const unsigned int end[6]  = { 0x00420B28, 0x00420F53, 0x00421162, 0x0042118D,
                                              0x004211C3, 0x0042166D };
        for (k = 0; k < 7; k++) lim_dword(base[k], 0x00511DF0u, psys, "flying-piece slots");
        for (k = 0; k < 6; k++) lim_dword(end[k], 0x00511F80u, psysEnd, "flying-piece slots end");
    }

    /* ---- debris records: both first-free scans become lim_aux_alloc */
    {
        static const unsigned char head[6] = { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 };
        lim_branch(0x00420920, 6, head, 0xE9, (unsigned int)(size_t)&lim_aux_alloc,
                   "debris allocator");
    }
    {
        /* 0x4217DE: `mov edx,[main] / xor esi,esi / xor eax,eax / lea ecx,[edx+0x1AB9F]`,
           the head of the inline scan. 0x421804 wants eax = the record or 0 and esi = 0,
           and reloads ecx and edx before it uses them; edi (the explosion record) is
           callee-saved across the call. */
        static const unsigned char head[16] = { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00, 0x33, 0xF6,
                                                0x33, 0xC0, 0x8D, 0x8A, 0x9F, 0xAB, 0x01, 0x00 };
        unsigned char ours[16];
        unsigned int rel;
        ours[0] = 0x33; ours[1] = 0xF6;                                  /* xor esi,esi   */
        ours[2] = 0xE8;                                                  /* call alloc    */
        rel = (unsigned int)(size_t)&lim_aux_alloc - (0x004217E0u + 5u);
        memcpy(ours + 3, &rel, 4);
        ours[7] = 0xE9;                                                  /* jmp 0x421804  */
        rel = 0x00421804u - (0x004217E5u + 5u);
        memcpy(ours + 8, &rel, 4);
        for (k = 12; k < 16; k++) ours[k] = 0x90;
        lim_add(0x004217DE, 16, head, ours, "piece debris allocator");
    }

    /* ---- units a player. The process-start read 0x491653 (totala.ini [Preferences]
       UnitLimit, in 0x491200, which WinMain calls once) defaults to 250 and clamps to
       [20, 500] into main+0x37EEC; the default and the ceiling both become TAGPU_LIM_UNITS,
       and the floor at 0x491678 stays. The unit array is 10 x main+0x37EE6 + 1 slots, which
       the frame packet's design point covers (tagpu_packet_pub.c asserts it), and every
       writer of +0x37EE6 is held to [20, TAGPU_LIM_UNITS]: the game start copies the clamped
       +0x37EEC (0x4971F8, 0x4973CD), and the three below clamp what they store. */
    lim_dword(0x00491640, 250, TAGPU_LIM_UNITS, "unit limit default");
    lim_dword(0x00491659, 500, TAGPU_LIM_UNITS, "unit limit ceiling test");
    lim_dword(0x00491666, 500, TAGPU_LIM_UNITS, "unit limit ceiling");
    {
        /* the two `maxunits` keys stock stores unclamped: a saved game's [Summary]
           (0x432610, which writes the per-player limit) and the map's own .ota
           [GlobalHeader] (0x435DA0, default 200, which writes the array's count directly;
           a skirmish or network game overwrites it at game start, a campaign mission plays
           with it). Every retail map sets 200 to 400, inside the clamp. */
        static const unsigned char sumStore[7] = { 0x66, 0x89, 0x81, 0xEC, 0x7E, 0x03, 0x00 };
        static const unsigned char hdrStore[7] = { 0x66, 0x89, 0x81, 0xE6, 0x7E, 0x03, 0x00 };
        lim_branch(0x00432646, 7, sumStore, 0xE8, (unsigned int)(size_t)saveMax,
                   "saved game maxunits");
        lim_branch(0x00436037, 7, hdrStore, 0xE8, (unsigned int)(size_t)missionMax,
                   "mission maxunits");
    }
    {
        /* THE HOST'S LIMIT. A network game's start overwrites the array's count with the
           word at +0xA5 of the host's player record (0x4973AE, 0x4973B5). The host's own
           slider bounds it (ActualUnitLimit - 20, 0x44A2B2), but a lobby launch sets
           ActualUnitLimit unclamped (0x449D9B) and a peer's record comes off the wire; past
           6553 the slot count 10 x N + 1 wraps its u16 (0x4854EA) and the unit array is
           allocated too small. The read becomes a zero-extending movzx, the same 7 bytes,
           so the whole of eax is the value, and the store goes through the clamp. eax and
           the flags are dead after it: the next instruction is `jmp 0x4974E3`. */
        static const unsigned char hostRead[7]  = { 0x66, 0x8B, 0x80, 0xA5, 0x00, 0x00, 0x00 };
        static const unsigned char hostMovzx[7] = { 0x0F, 0xB7, 0x80, 0xA5, 0x00, 0x00, 0x00 };
        static const unsigned char hostStore[7] = { 0x66, 0x89, 0x81, 0xE6, 0x7E, 0x03, 0x00 };
        lim_add(0x004973AE, 7, hostRead, hostMovzx, "host unit limit read");
        lim_branch(0x004973B5, 7, hostStore, 0xE8, (unsigned int)(size_t)missionMax,
                   "host unit limit");
    }
    /* THE BATTLEROOM'S "NO LIMIT" SENTINEL IS A CAP ON ONE PATH. The unit-restriction menu
       keeps each type's count as 0..100, and 101 (`mov ecx,0x65` at 0x44CAFD) for a type
       with no limit. Cancel (0x44C6FC) writes those saved values back into the restriction
       store as they are (0x44C750), the game start copies them to UnitDef+0x15A (0x46E160),
       and the unit constructor 0x485F50 then refuses a type's 102nd unit. The sentinel
       becomes the unit limit, as TADR writes it, so a cancelled menu caps nothing a player
       could reach; the menu shows any value past 100 as "No Limit" (0x44BEC0). */
    lim_dword(0x0044CAFE, 101, TAGPU_LIM_UNITS, "unrestricted type count");

    /* ---- the pathfinder's search budget: `mov dword [esi+0x48],0x535` at 0x40EAD3, in the
       pathfinder's per-game init (0x40E9E0) and the only 0x535 in .text. Its one reader is
       the per-tick 0x40EB70, which shares it among the players; nothing is sized or indexed
       by it, so the raise costs time, not memory. */
    lim_dword(0x0040EAD6, 1333, TAGPU_LIM_PATH, "pathfinding budget");

    /* ---- particles: two ceilings. Every emitter (0x470F00..0x472F00) takes its layer's
       size and `cmp eax,0x190 / jbe append`; past the cap it destroys the layer's oldest
       object and appends anyway, so a layer holds TAGPU_LIM_SFX + 1. Nineteen compares are
       `3D imm32` (operand at +1), one is `81 F9 imm32` against ecx (operand at +2): the
       twenty are every 0x190 compare in the emitters. The objects come from ONE pool,
       0x51E610, built by the C runtime's static initializer at 0x471C80 (`push 0x4C; push
       0x3E8; ... call 0x470A90`), which allocates its whole capacity once; its alloc 0x470EB0
       returns 0 when the pool is empty and every emitter then skips the particle. DllMain
       runs before that initializer, so the pool is built at the raised capacity. Visual
       only: the emitters draw the C runtime's rand, never the simulation's generator. */
    {
        static const unsigned int cmpEax[19] = {
            0x00471183, 0x004713D8, 0x00471508, 0x0047163D, 0x00471782, 0x004718B1, 0x00471AD7,
            0x00472071, 0x0047219F, 0x004722CF, 0x004723D6, 0x004724D5, 0x004725D4, 0x004726C0,
            0x004727B0, 0x0047289A, 0x0047297A, 0x00472A5A, 0x00472CD9 };
        int k;
        for (k = 0; k < 19; k++)
            lim_dword(cmpEax[k] + 1, 400, TAGPU_LIM_SFX, "particle layer cap");
        lim_dword(0x00472BF2 + 2, 400, TAGPU_LIM_SFX, "particle layer cap");
        lim_dword(0x00471C83, 1000, TAGPU_LIM_SFXPOOL, "particle pool");
    }

    /* ---- the composite scratch frame. The composite draw context *(main+0x1437B), made once a
       level by the model loader (0x42D473 -> 0x458180), keeps one scratch frame at +0x10 from
       0x4B8E00(name, width, height): a colour and a depth plane of width x height bytes, freed
       with the context by the level teardown (0x42DC8F -> 0x4581C0). Four writers in the blit
       size it to a unit and none compares that size with the allocation: the build-state copy
       0x4589C0, the frame copy 0x45A470, the shadow build 0x45A790 and the 2x structure bake in
       0x459830 / 0x459C70. The engine fix fix_composite_scratch bounds them all and grows the
       frame when a unit needs more; this raise is only where it starts, so that no unit of the
       shipped content needs a grow. Every reader takes its size from the header a writer set,
       so no write grows with the raise. */
    lim_dword(0x0045819B, 600, TAGPU_LIM_COMPOSITE, "composite scratch width");
    lim_dword(0x00458196, 600, TAGPU_LIM_COMPOSITE, "composite scratch height");

    /* ---- the wreck pool, main+0x1420B: records of 0x30 bytes that 0x421F20 allocates, zeroes
       and threads onto a free list once a level. A 3DO wreck (a corpse, a heap) holds one for
       its life, and a GAF feature holds one while it plays its death or reclaim sequence. Its
       size is written in four places at the build -- the allocation, the rep-stos count, the
       loop's end and the offset of the last record, whose next link is cut to -1 -- and the
       allocators' "no record" is the count itself, in seven more: 0x4232A0 (no E8 caller),
       the burn start 0x4233A0, FeatureDie 0x423550 and the feature creator 0x423C50 each set
       it when the free list is empty and compare against it before using the index. Nothing
       else sizes the pool [every reference to main+0x1420B/0x1421B read]. What an empty pool
       costs is research/notes/tadr-port/raised-limits.md's to state; FeatureDie's case is the
       engine fix fix_feature_die_pool_full. */
    lim_dword(0x00421F2A, 0x18000, TAGPU_LIM_WRECKS * 0x30u, "wreck pool bytes");
    lim_dword(0x00421F41, 0x6000, TAGPU_LIM_WRECKS * 0x30u / 4u, "wreck pool clear");
    lim_dword(0x00421F7A, 0x18000, TAGPU_LIM_WRECKS * 0x30u, "wreck free list end");
    lim_dword(0x00421F97, 0x17FD0, (TAGPU_LIM_WRECKS - 1u) * 0x30u, "wreck free list last");
    {
        static const unsigned int none[] = { 0x004232B9, 0x0042340E, 0x0042343A, 0x0042361E,
                                             0x0042364D, 0x00423DBA, 0x00423DE1 };
        int k;
        for (k = 0; k < (int)(sizeof none / sizeof none[0]); k++)
            lim_dword(none[k], 0x800, TAGPU_LIM_WRECKS, "wreck pool none");
    }

    /* ---- unit-type slots. A category mask is one bit a type: 0x40-byte heap blocks the name
       map 0x488C50 allocates and clears (a type's own bit set by 0x488E03 and at game start by
       0x488E70, from 0x42D6C2), and 0x40-byte stack masks in the AI plan's `Weight` and `Limit`
       commands (0x406DB0, 0x406E40, filled by 0x488D30) and in Ctrl-Z (0x48BE00). Every reader
       indexes one by a type ID below the def count, and nothing else sizes them: the 17 sites
       below are the allocation, the five 16-dword clear and OR loops and the three stack
       frames, each of which is its mask plus at most one dword under it, so the mask grows
       upward over the frame and only the displacements above it move. A 0x804-byte frame is
       inside one page, so no probe is needed. The count check at 0x42AA65 makes the bound
       hold: the def array is allocated from it, and every later count is that array's. */
    {
        enum { MB = TAGPU_LIM_TYPES / 8, G = MB - 0x40 };
        unsigned char c[24], *p;
        static const unsigned char subEsp[2] = { 0x81, 0xEC }, addEsp[2] = { 0x81, 0xC4 };
        static const unsigned char movEsi[3] = { 0x8B, 0xB4, 0x24 }, movEbp[3] = { 0x8B, 0xAC, 0x24 };
        static const unsigned char movEcx[3] = { 0x8B, 0x8C, 0x24 }, leaEax[3] = { 0x8D, 0x84, 0x24 };
        static const unsigned char fstp[3] = { 0xD9, 0x9C, 0x24 };
        static const unsigned char pushImm = 0x68;
        static const unsigned char wEntry[5] = { 0x83, 0xEC, 0x44, 0x85, 0xC0 };
        static const unsigned char wArg[6]   = { 0x8B, 0x74, 0x24, 0x50, 0xF3, 0xAB };
        static const unsigned char wFstp[8]  = { 0xD9, 0x5C, 0x24, 0x58, 0x8B, 0x6C, 0x24, 0x58 };
        static const unsigned char wExit[6]  = { 0x83, 0xC4, 0x44, 0xC2, 0x04, 0x00 };
        static const unsigned char lEntry[5] = { 0x83, 0xEC, 0x40, 0x85, 0xC0 };
        static const unsigned char lArg[6]   = { 0x8B, 0x74, 0x24, 0x4C, 0xF3, 0xAB };
        static const unsigned char lOut[5]   = { 0x8D, 0x44, 0x24, 0x50, 0x53 };
        static const unsigned char lArg2[8]  = { 0x8B, 0x4C, 0x24, 0x54, 0x8D, 0x54, 0x24, 0x10 };
        static const unsigned char lExit[6]  = { 0x83, 0xC4, 0x40, 0xC2, 0x04, 0x00 };
        static const unsigned char hAlloc[7] = { 0x6A, 0x40, 0xE8, 0x47, 0xC2, 0x02, 0x00 };
        static const unsigned char zEntry[9] = { 0x83, 0xEC, 0x40, 0x8A, 0x8A, 0x42, 0x2A, 0x00, 0x00 };
        static const unsigned char zExit[5]  = { 0x83, 0xC4, 0x40, 0xC3, 0x90 };
        static const unsigned char count[7]  = { 0x43, 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 };
        unsigned char* types = lim_types_stub();
        unsigned char* halloc;

        /* the AI's Weight: a dword local at the frame's foot, the mask above it, one argument */
        p = lim_emit(c, subEsp, 2, 0x44 + G); *p++ = 0x85; *p++ = 0xC0;           /* test eax,eax */
        lim_reloc(0x00406DB5, 5, wEntry, c, (int)(p - c), 1, "AI weight mask frame");
        lim_dword(0x00406DBE, 0x10, MB / 4, "AI weight mask clear");
        p = lim_emit(c, movEsi, 3, 0x50 + G); *p++ = 0xF3; *p++ = 0xAB;           /* rep stosd    */
        lim_reloc(0x00406DC9, 6, wArg, c, (int)(p - c), 1, "AI weight argument");
        p = lim_emit(c, fstp, 3, 0x58 + G); p = lim_emit(p, movEbp, 3, 0x58 + G);
        lim_reloc(0x00406DFD, 8, wFstp, c, (int)(p - c), 1, "AI weight argument slot");
        p = lim_emit(c, addEsp, 2, 0x44 + G); *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;  /* ret 4   */
        lim_reloc(0x00406E3A, 6, wExit, c, (int)(p - c), 0, "AI weight mask frame release");

        /* the AI's Limit: the mask at the frame's foot, one argument */
        p = lim_emit(c, subEsp, 2, 0x40 + G); *p++ = 0x85; *p++ = 0xC0;
        lim_reloc(0x00406E45, 5, lEntry, c, (int)(p - c), 1, "AI limit mask frame");
        lim_dword(0x00406E52, 0x10, MB / 4, "AI limit mask clear");
        p = lim_emit(c, movEsi, 3, 0x4C + G); *p++ = 0xF3; *p++ = 0xAB;
        lim_reloc(0x00406E5D, 6, lArg, c, (int)(p - c), 1, "AI limit argument");
        p = lim_emit(c, leaEax, 3, 0x50 + G); *p++ = 0x53;                         /* push ebx     */
        lim_reloc(0x00406E64, 5, lOut, c, (int)(p - c), 1, "AI limit argument slot");
        p = lim_emit(c, movEcx, 3, 0x54 + G); memcpy(p, lArg2 + 4, 4); p += 4;     /* lea edx,[esp+0x10] */
        lim_reloc(0x00406EB2, 8, lArg2, c, (int)(p - c), 1, "AI limit argument read");
        p = lim_emit(c, addEsp, 2, 0x40 + G); *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;
        lim_reloc(0x00406ED6, 6, lExit, c, (int)(p - c), 0, "AI limit mask frame release");

        /* the heap masks: `push 0x40; call 0x4B4F10` has no room for a 32-bit size */
        halloc = lim_code(16);
        if (halloc) {
            p = lim_emit(halloc, &pushImm, 1, MB);
            *p++ = 0xE8; tagpu_detour_rel(p, 0x004B4F10); p += 4;
            *p++ = 0xE9; tagpu_detour_rel(p, 0x00488CC9);
            lim_branch(0x00488CC2, 7, hAlloc, 0xE9, (unsigned int)(size_t)halloc, "category mask bytes");
        }
        lim_dword(0x00488CD3, 0x10, MB / 4, "category mask clear");
        lim_dword(0x00488E3E, 0x10, MB / 4, "category mask OR");

        /* Ctrl-Z: the mask at the frame's foot, no argument */
        p = lim_emit(c, subEsp, 2, 0x40 + G); memcpy(p, zEntry + 3, 6); p += 6;    /* mov cl,[edx+0x2A42] */
        lim_reloc(0x0048BE08, 9, zEntry, c, (int)(p - c), 1, "Ctrl-Z mask frame");
        lim_dword(0x0048BE22, 0x10, MB / 4, "Ctrl-Z mask clear");
        p = lim_emit(c, addEsp, 2, 0x40 + G); *p++ = 0xC3;                         /* ret          */
        lim_reloc(0x0048BF1E, 5, zExit, c, (int)(p - c), 0, "Ctrl-Z mask frame release");

        if (types)
            lim_branch(0x0042AA65, 7, count, 0xE8, (unsigned int)(size_t)types, "unit type count");
    }

    /* ---- the join's pace. A joining peer sends its unit-sync checksums, one message a type,
       four types a lobby tick (0x46DDFA..0x46DE95, `cmp ebp,4` at 0x46DE8F), and the battle room
       shows SYNCHING until the last is in. MEASURED 2026-09-24, two peers at 16 383 types: 20 s,
       the cursor at a steady ~880 types a second while the host kept up, so the pace and not
       the host's searches is the cost. 64 a tick sends the same messages sooner. */
    {
        static const unsigned char four = 0x04, many = 0x40;
        lim_add(0x0046DE91, 1, &four, &many, "unit sync types a tick");
    }

    /* ---- weapon IDs: the array, its references and the wire ("WEAPON IDS" above), with the
       sites both builds carry, since they rewrite the same lookups */
    {
        static WPNSITES w;
        if (!wpn_build(&w)) { s_limOverflow = w.over; s_limNoStub = !w.over; return; }
        for (k = 0; k < w.n; k++) lim_add(w.s[k].va, w.s[k].n, w.s[k].was, w.s[k].now, w.name[k]);
    }
}

const char* tagpu_limits_expl_pool(const char* ta)
{
    return s_limState > 0 ? (const char*)&s_expl : ta + 0x1491B;
}

const void* const* tagpu_limits_psys_begin(void)
{
    return s_limState > 0 ? (const void* const*)&s_psys[0]
                          : (const void* const*)(size_t)0x00511DF0u;
}

const void* const* tagpu_limits_psys_end(void)
{
    return s_limState > 0 ? (const void* const*)&s_psys[TAGPU_LIM_PSYS]
                          : (const void* const*)(size_t)0x00511F80u;
}

char* tagpu_limits_weapon0(void)
{
    char* ta;
    if (s_limState > 0) return (char*)s_weapons;
    ta = *(char* const*)0x00511DE8;
    return ta ? ta + WPN_MAIN : NULL;
}

static int wpn_slots(void) { return s_limState > 0 ? TAGPU_LIM_WEAPONS : 256; }

#else  /* TAGPU_LIMITS_STOCK */

const char* tagpu_limits_expl_pool(const char* ta) { return ta + 0x1491B; }
const void* const* tagpu_limits_psys_begin(void) { return (const void* const*)(size_t)0x00511DF0u; }
const void* const* tagpu_limits_psys_end(void) { return (const void* const*)(size_t)0x00511F80u; }

char* tagpu_limits_weapon0(void)
{
    char* ta = *(char* const*)0x00511DE8;
    return ta ? ta + WPN_MAIN : NULL;
}

static int wpn_slots(void) { return TAGPU_LIM_WEAPONS; }

#endif /* TAGPU_LIMITS_STOCK */

/* Two sites of the table that cover one byte: two fixes, or a fix and a raised limit, would
   each have been checked against stock bytes the other rewrites. Our bug, found before
   anything is compared. */
static int lim_overlap(void)
{
    int i, j;
    for (i = 0; i < s_nlim; i++)
        for (j = i + 1; j < s_nlim; j++)
            if (s_lim[i].va < s_lim[j].va + s_lim[j].n && s_lim[j].va < s_lim[i].va + s_lim[i].n) {
                s_limOverlapA = s_lim[i].va;
                s_limOverlapB = s_lim[j].va;
                return 1;
            }
    return 0;
}

int tagpu_limits_install(void)
{
    int i, bad = 0, written;
    if (s_limState) return s_limState > 0;
#ifndef TAGPU_LIMITS_STOCK
    lim_sites();
#endif
    tagpu_logf("enginefix: the fixes' and limits' stubs take %u bytes in %u page(s) of 4096",
               s_fixBytes, s_fixPages);
    if (s_limNoStub) { s_limState = -1; plog("limits: FAILED -- a code stub could not be made"); return 0; }
    if (s_limOverflow) { s_limState = -1; plog("limits: FAILED -- the site table is too small"); return 0; }
    if (lim_overlap()) {
        s_limState = -1;
        tagpu_logf("limits: FAILED -- the sites at 0x%08X and 0x%08X overlap", s_limOverlapA,
                   s_limOverlapB);
        return 0;
    }

    for (i = 0; i < s_nlim; i++) {
        LIMSITE* s = &s_lim[i];
        if (!lim_read(s->va, s->have, s->n) || memcmp(s->have, s->stock, s->n)) {
            s->differs = 1;
            bad++;
        }
    }
    if (bad) {
        s_limState = -1;
        tagpu_logf("limits: FAILED -- %d of %d sites differ from stock 3.1, nothing written; "
                   "the report shows at the first DirectDraw call", bad, s_nlim);
        return 0;
    }
    for (written = 0; written < s_nlim; written++) {
        LIMSITE* s = &s_lim[written];
        if (!tagpu_detour_write(s->va, s->ours, s->n)) break;
    }
    if (written < s_nlim) {
        /* PUT BACK WHAT WAS WRITTEN: the process ends at the report either way, but
           nothing runs meanwhile on a half-changed engine. */
        s_limWriteFail = s_lim[written].va;
        while (written-- > 0) tagpu_detour_write(s_lim[written].va, s_lim[written].stock, s_lim[written].n);
        s_limState = -1;
        tagpu_logf("limits: FAILED -- the write at 0x%08X was refused; everything written was put back",
                   s_limWriteFail);
        return 0;
    }
    s_limState = 1;
#ifndef TAGPU_LIMITS_STOCK
    /* the moved pools' addresses, for `tacli peek`: the explosion count is the first dword */
    tagpu_logf("limits: installed %d sites, the simulation fixes' included -- projectiles %d, "
               "explosions %d at 0x%08X, flying pieces %d at 0x%08X, debris records %d at 0x%08X, "
               "units %d a player, pathfinding %d, particles %d a layer from a pool of %d, "
               "composite %d, wreck records %d, unit types %d, weapons %d at 0x%08X", s_nlim,
               TAGPU_LIM_PROJ, TAGPU_LIM_EXPL, (unsigned int)(size_t)&s_expl,
               TAGPU_LIM_PSYS, (unsigned int)(size_t)s_psys,
               TAGPU_LIM_AUX, (unsigned int)(size_t)s_aux, TAGPU_LIM_UNITS, TAGPU_LIM_PATH,
               TAGPU_LIM_SFX, TAGPU_LIM_SFXPOOL, TAGPU_LIM_COMPOSITE, TAGPU_LIM_WRECKS,
               TAGPU_LIM_TYPES - 1, TAGPU_LIM_WEAPONS, (unsigned int)(size_t)s_weapons);
    return 1;
#else
    tagpu_logf("limits: stock build -- nothing raised (projectiles 300, explosions 300, "
               "flying pieces 100, debris records 300, units 250 a player up to 500, "
               "pathfinding 1333, particles 400 a layer from a pool of 1000, composite 600, "
               "wreck records 2048, unit types 511); the simulation fixes' %d sites installed",
               s_nlim);
    return 1;
#endif
}

int tagpu_limits_weapon_index(const void* weapon)
{
    const char* base = tagpu_limits_weapon0();
    size_t d;
    if (!base || (const char*)weapon < base) return -1;
    d = (size_t)((const char*)weapon - base);
    if (d % WPN_REC || d / WPN_REC >= (size_t)wpn_slots()) return -1;
    return (int)(d / WPN_REC);
}

/* ---- the report ------------------------------------------------------------------------
   Shown once, at the first DirectDraw call rather than in DllMain: a MessageBox under the
   loader lock can deadlock. Its text is written for a player first and for whoever
   debugs it second -- a person or an agent -- so the block after `--- report ---` says
   exactly which build met exactly which exe, and every site that differed. It carries no
   path: a report is meant to be pasted in public, and an install path names a user. */

static const struct { const char* md5; const char* name; } LIM_KNOWN[] = {
    { "8e74a1dffa1f5988624c52048f5b20cd", "Total Annihilation 3.1 (retail)" },
};

static int lim_md5(const char* path, char out[33], unsigned long* size)
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH h = 0;
    HANDLE f;
    unsigned char buf[65536], md[16];
    DWORD got, mdlen = sizeof md, total = 0;
    int ok = 0, i;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(prov, CALG_MD5, 0, 0, &h)) {
        ok = 1;
        while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) {
            if (!CryptHashData(h, buf, got, 0)) { ok = 0; break; }
            total += got;
        }
        if (ok && CryptGetHashParam(h, HP_HASHVAL, md, &mdlen, 0) && mdlen == 16) {
            for (i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", md[i]);
            *size = total;
        } else ok = 0;
    }
    if (h) CryptDestroyHash(h);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(f);
    return ok;
}

/* at most `max` bytes, then "..." -- the box shows a long site's head, the log all of it */
static void lim_hex(char* out, const unsigned char* b, int n, int max)
{
    int i;
    out[0] = 0;
    for (i = 0; i < n && i < max; i++) sprintf(out + strlen(out), i ? " %02X" : "%02X", b[i]);
    if (n > max) strcat(out, " ...");
}

void tagpu_limits_report(void)
{
    static char text[8192];
    static LONG once;
    char exe[MAX_PATH], md5[33] = "unknown", line[768], want[3 * LIM_MAXB + 8],
         have[3 * LIM_MAXB + 8];
    const char* base;
    const char* known = "none";
    const char* why;
    unsigned long size = 0;
    DWORD stamp = 0;
    int i, bad = 0, shown = 0;
    HMODULE me = GetModuleHandleA(NULL);

    if (s_limState >= 0) return;
    if (InterlockedExchange(&once, 1)) return;

    GetModuleFileNameA(NULL, exe, sizeof exe);
    base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe;
    lim_md5(exe, md5, &size);
    for (i = 0; i < (int)(sizeof LIM_KNOWN / sizeof LIM_KNOWN[0]); i++)
        if (!strcmp(md5, LIM_KNOWN[i].md5)) known = LIM_KNOWN[i].name;
    if (me) {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)me;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const char*)me + dos->e_lfanew);
        stamp = nt->FileHeader.TimeDateStamp;
    }
    for (i = 0; i < s_nlim; i++) bad += s_lim[i].differs;

    /* ONE LINE PER PARAGRAPH: the box wraps prose to its own width, and a hard break
       inside a paragraph wraps a second time into ragged half-lines. The report lines
       are kept short enough that the box never wraps them. */
    if (s_limNoStub || s_limOverflow || s_limOverlapA)
        why = "Impure failed on its own side before it compared anything: this is a bug in "
              "Impure, or the system is out of memory, not a problem with this TotalA.exe.";
    else if (s_limWriteFail)
        why = "Windows refused to let Impure change the game's code in memory.";
    else if (strcmp(known, "none"))
        why = "This TotalA.exe IS the 3.1 that Impure is built for, but something else "
              "-- another patch or loader -- changed those places in memory before "
              "Impure ran.";
    else
        why = "This TotalA.exe is not the Total Annihilation 3.1 that Impure is built for.";

    _snprintf(text, sizeof text,
        "Impure could not install its engine limits and fixes, so Total Annihilation will "
        "now close. Nothing was changed.\r\n"
        "\r\n"
        "WHY\r\n"
        "Impure raises the game's limits (units, projectiles, explosions...) and fixes "
        "some of its defects by rewriting its code in memory, and it checks every place "
        "first. %s Running "
        "anyway would let this game play by different rules from other players and "
        "break multiplayer without warning.\r\n"
        "\r\n"
        "WHAT TO DO\r\n"
        "- Use the original 3.1 TotalA.exe (the Steam copy is 3.1). Community patches "
        "such as 3.9.02 and TA: Escalation ship a modified exe.\r\n"
        "- Or report it: press Ctrl+C to copy this message and paste it into a new "
        "issue at\r\n"
        "github.com/Code-Herder/ta-impure-patch/issues\r\n"
        "The same report is saved in log\\startup-failure.txt\r\n"
        "\r\n"
        "--- report ---\r\n"
        "impure %s (%s)\r\n"
        "exe %s, %lu bytes\r\n"
        "md5 %s\r\n"
        "PE stamp 0x%08lX, known build: %s\r\n",
        why, GIT_COMMIT, GIT_BRANCH, base, size, md5, (unsigned long)stamp, known);
    text[sizeof text - 1] = 0;

    if (s_limNoStub)
        _snprintf(line, sizeof line, "result: a code stub could not be made, nothing written\r\n");
    else if (s_limOverflow)
        _snprintf(line, sizeof line, "result: the site table overflowed, nothing written\r\n");
    else if (s_limOverlapA)
        _snprintf(line, sizeof line, "result: the sites at 0x%08X and 0x%08X overlap, nothing "
                  "written\r\n", s_limOverlapA, s_limOverlapB);
    else if (s_limWriteFail)
        _snprintf(line, sizeof line, "result: write refused at 0x%08X, all put back\r\n",
                  s_limWriteFail);
    else
        _snprintf(line, sizeof line, "result: %d of %d sites differ, nothing written\r\n", bad, s_nlim);
    line[sizeof line - 1] = 0;
    strncat(text, line, sizeof text - strlen(text) - 1);

    for (i = 0; i < s_nlim; i++) {
        const LIMSITE* s = &s_lim[i];
        if (!s->differs) continue;
        if (++shown > 12) {
            _snprintf(line, sizeof line, "... and %d more, all of them in log\\tagpu.log\r\n", bad - 12);
            line[sizeof line - 1] = 0;
            strncat(text, line, sizeof text - strlen(text) - 1);
            break;
        }
        lim_hex(want, s->stock, s->n, 16);
        lim_hex(have, s->have, s->n, 16);
        _snprintf(line, sizeof line, "0x%08X %s\r\n  want %s\r\n  have %s\r\n",
                  s->va, s->name, want, have);
        line[sizeof line - 1] = 0;
        strncat(text, line, sizeof text - strlen(text) - 1);
    }

    /* every differing site goes to the log, not only the twelve the box has room for */
    tagpu_log("limits: startup failure report follows");
    for (i = 0; i < s_nlim; i++) {
        const LIMSITE* s = &s_lim[i];
        if (!s->differs) continue;
        lim_hex(want, s->stock, s->n, LIM_MAXB);
        lim_hex(have, s->have, s->n, LIM_MAXB);
        tagpu_logf("limits:   0x%08X %s want %s have %s", s->va, s->name, want, have);
    }
    {
        char path[MAX_PATH];
        const char* dir = tagpu_log_dir();
        if (dir && *dir && _snprintf(path, sizeof path, "%sstartup-failure.txt", dir) > 0) {
            HANDLE f;
            path[sizeof path - 1] = 0;
            f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD w;
                WriteFile(f, text, (DWORD)strlen(text), &w, NULL);
                CloseHandle(f);
            }
        }
    }
    tagpu_log(text);

    MessageBoxA(NULL, text, "Total Annihilation: Impure cannot start",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    ExitProcess(ERROR_BAD_EXE_FORMAT);
}
