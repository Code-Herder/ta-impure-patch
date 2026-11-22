/* tagpu_patches.c — our own runtime engine patches on the loaded TotalA.exe image.
   Applied from DllMain via VirtualProtect; the on-disk exe stays pristine. */

#include <windows.h>
#include <wincrypt.h>
#include <dplay.h>          /* the engine's interface: IDirectPlay3A (IID at 0x4FCD78), or
                               IDirectPlay2A when lobbied (main+0x4D9) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "tagpu_scatter.h"
#include "tagpu_patches.h"
#include "tagpu_addr.h"   /* tagpu_user_top: the range filters' upper end */
#include "tagpu_limits.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "tagpu_refuse.h"
#include "tagpu_takeover.h"
#include "tagpu_regstore.h"
#include "tagpu_weapons.h"
#include "tagpu_datakeys.h"
#include "tagpu_cob_safe.h"
#include "tagpu_cobvm.h"
#include "tagpu_cobtrace.h"
#include "tagpu_simreg.h"
#include "tagpu_prand.h"
#include "tagpu_pevt.h"
#include "config.h"         /* g_config.gdi: the renderer chosen at attach */
#include "tagpu_recorder.h"
#include "tagpu_puppet.h"
#include "tagpu_replay.h"
#include "tagpu_recfmt.h"   /* REC_PHASE_*: the step's phases */
#include "tagpu_turn3.h"
#include "git.h"

SIM_FEATURE(patch_helpers, SIM_PUPPET_RUNS, "battle");

static void plog(const char* s)
{
    tagpu_log(s);
}

#ifndef TAGPU_LIMITS_STOCK
static int cob_type_rejected(unsigned type);
static void cob_required(unsigned type);
static void cob_level_reset(void);
static void cob_level_enter(void);
#endif
static int fix_effect_pools(void);

/* Write `val` at absolute `addr` iff it currently holds `expect`. Returns 1 on patch,
   0 if the byte didn't match (wrong build) or protection change failed. */
static int patch_byte(unsigned int addr, unsigned char expect, unsigned char val)
{
    if (*(const unsigned char*)(size_t)addr != expect) return 0;
    return tagpu_detour_write(addr, &val, 1);
}

/* Write `n` bytes at absolute `addr` iff they currently hold `expect`. Returns 1 on
   patch, 0 if the bytes didn't match (wrong build) or protection change failed. */
static int patch_bytes(unsigned int addr, const unsigned char* expect,
                       const unsigned char* val, unsigned int n)
{
    if (memcmp((const void*)(size_t)addr, expect, n) != 0) return 0;
    return tagpu_detour_write(addr, val, (int)n);
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

#define LIM_MAXSITE  512
#define LIM_MAXB     80    /* a whole replaced block, so the check covers what the stub stands for */

typedef struct LIMSITE {
    unsigned int  va;
    unsigned char n;
    unsigned char stock[LIM_MAXB];
    unsigned char ours[LIM_MAXB];
    unsigned char have[LIM_MAXB];   /* what the image held when compared            */
    unsigned char file[LIM_MAXB];   /* what the exe FILE says: the reference        */
    unsigned char want[LIM_MAXB];   /* what must be there after the install         */
    unsigned char differs;
    unsigned char unreadable;       /* the page could not be read: `have` means nothing */
    unsigned char fileok;           /* the file may differ from stock here: lim_file_ok */
    const char*   name;
} LIMSITE;

static LIMSITE s_lim[LIM_MAXSITE];
SIM_VAR(s_lim, SIM_INSTALL);
static int     s_nlim;
SIM_VAR(s_nlim, SIM_INSTALL);
static int     s_limState;           /* 0 not tried, 1 installed, -1 failed          */
SIM_VAR(s_limState, SIM_INSTALL);
static int     s_limOverflow;        /* the table itself was too small: our bug      */
SIM_VAR(s_limOverflow, SIM_INSTALL);
static int     s_limNoStub;          /* a code stub could not be made                */
SIM_VAR(s_limNoStub, SIM_INSTALL);
static char    s_limNeeds[192];      /* a table fix's required local fix is not armed */
SIM_VAR(s_limNeeds, SIM_INSTALL);
static unsigned int s_limWriteFail;  /* the site VirtualProtect refused, 0 = none    */
SIM_VAR(s_limWriteFail, SIM_INSTALL);
static unsigned int s_limOverlapA, s_limOverlapB;   /* two sites over one byte: our bug  */
SIM_VAR(s_limOverlapA, SIM_INSTALL);
SIM_VAR(s_limOverlapB, SIM_INSTALL);
static int     s_limRewritten;       /* installed, then rewritten by someone else      */
SIM_VAR(s_limRewritten, SIM_INSTALL);
static unsigned int s_limMarkMissing;/* lim_file_ok named a site the table does not have */
SIM_VAR(s_limMarkMissing, SIM_INSTALL);

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

#ifndef TAGPU_LIMITS_STOCK
/* A SITE WHOSE VALUE STANDS ALONE: the exe file may hold something other than stock 3.1 here
   without refusing the table, and Impure's own value is installed over it. Marking a site says
   three things, each of which has to be true of it: nothing of ours reads the value, no other
   site's argument rests on it, and the site is one Impure raises in the first place -- so what
   the mod's exe holds is the unraised number its own runtime replaces, not a setting of its own.
   Anywhere else a file that differs from stock is the mod's change and refuses. Adding one is a
   claim about that site, checked in a review, never a way past a refusal
   (research/notes/compat/takeover.md, part 2). */
static void lim_file_ok(unsigned int va)
{
    int i;
    for (i = 0; i < s_nlim; i++)
        if (s_lim[i].va == va) { s_lim[i].fileok = 1; return; }
    s_limMarkMissing = va;          /* a marked site that is not in the table is our bug */
}
#endif

/* a compared run longer than a site holds, as consecutive sites */
static void lim_same_run(unsigned int va, int n, const unsigned char* stock, const char* name)
{
    int i;
    for (i = 0; i < n; i += LIM_MAXB)
        lim_same(va + (unsigned)i, n - i < LIM_MAXB ? n - i : LIM_MAXB, stock + i, name);
}

/* a fix of the table whose code stub could not be made: nothing of the table is written */
static void lim_no_stub(void)
{
    s_limNoStub = 1;
}

/* a fix of the table that rests on a local fix, which did not arm: nothing of the table is
   written, and the report says which */
static void lim_needs(const char* fix, const char* needs, const char* state)
{
    _snprintf(s_limNeeds, sizeof s_limNeeds, "%s needs %s armed, and it is %s", fix, needs, state);
    s_limNeeds[sizeof s_limNeeds - 1] = 0;
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

SIM_FEATURE(sort_buffer_end, SIM_PUPPET_RUNS, "battle");

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
        tagpu_detour_stub_free(s);
        return FIX_PROTECT;
    }
    return FIX_ARMED;
}

SIM_FEATURE(feature_null_plot, SIM_PUPPET_RUNS, "battle");

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
        tagpu_detour_stub_free(s);
        return FIX_PROTECT;
    }
    return FIX_ARMED;
}

SIM_FEATURE(terrain_window, SIM_PUPPET_RUNS, "battle");

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
    return (size_t)p > 0x10000u && (size_t)p <= tagpu_user_top();
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
        tagpu_detour_stub_free(s);
        return FIX_PROTECT;
    }
    return FIX_ARMED;
}

SIM_FEATURE(feature_die_pool_full, SIM_PUPPET_RUNS, "battle");

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

SIM_FEATURE(reclaim_mark_anchor, SIM_PUPPET_RUNS, "battle");

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

SIM_FEATURE(saved_features_border, SIM_PUPPET_RUNS, "battle");

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
    bits = why ? NULL : (unsigned char*)tagpu_sim_scratch((n + 7) / 8);
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
    tagpu_sim_scratch_free(bits);
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

SIM_FEATURE(restore_record_owner, SIM_PUPPET_RUNS, "battle");

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

SIM_FEATURE(composite_scratch, SIM_PUPPET_RUNS, "c4-survivor");

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
   the old block's two plane pointers set to the reserved poison region first so that a reader still holding
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
/* the stress lever's freed-frame plane pointers: the MIDDLE of a region RESERVED with no access
   when the lever is read, the largest plane offset (at most 8 MB) on either side, so any plane
   access through a poisoned frame faults, forwards or backwards. Reserved rather than an address above 2 GB, which is only no one's
   while the exe is not large-address-aware (tagpu_addr.h) -- TA:ESC's exe is. */
#define SCR_POISON_SPAN (16u << 20)
static unsigned char* s_scr_poison;
SIM_GUARD(s_scr_poison);

enum { SCR_BUILD, SCR_FRAME, SCR_SHADOW, SCR_BAKE1, SCR_BAKE2 };
static const char* const SCR_WHO[] = {
    "the build-state copy 0x4589C0", "the frame copy 0x45A470", "the shadow build 0x45A790",
    "the 2x bake 0x459830", "the 2x bake 0x459C70",
};

/* the stubs' pushad block, as dword indexes; the site's esp is regs + 8 */
enum { SCR_EDI = 0, SCR_ESI = 1, SCR_EBP = 2, SCR_EBX = 4, SCR_EDX = 5, SCR_ECX = 6,
       SCR_SITE = 8 };

static int s_scr_stress, s_scr_nogrow;
SIM_VAR(s_scr_stress, SIM_INSTALL);
SIM_VAR(s_scr_nogrow, SIM_INSTALL);
static volatile unsigned char s_scr_refused;     /* the build-state copy's answer, GAME THREAD */
SIM_VAR(s_scr_refused, SIM_TRANSIENT);
static struct {
    unsigned grows[5], refusals[5], layout;   /* by writer */
    unsigned merge_refusals;
} s_scrDiag;
SIM_VAR(s_scrDiag, SIM_PRIVATE);

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
                *(unsigned char**)(f + 0x10) = s_scr_poison + SCR_POISON_SPAN / 2;
                *(unsigned char**)(f + 0x14) = s_scr_poison + SCR_POISON_SPAN / 2;
            }
            ((void (__cdecl*)(void*))0x004D85A0)(f);
            scratch_log("grown", who, need, rows, a, px, ++s_scrDiag.grows[who]);
            return 1;
        }
        why = "refused (the allocator has no block)";
    }
    scratch_log(why, who, need, rows, a, a, ++s_scrDiag.refusals[who]);
    return 0;
}

/* the frame at ctx+0x10 and its area; 0 leaves the write to stock (see THE INVARIANT) */
static unsigned char* scratch_frame(unsigned char* ctx, unsigned* a)
{
    unsigned char* f = ptr_sane(ctx) ? *(unsigned char**)(ctx + 0x10) : NULL;
    *a = scratch_area(f);
    if (!*a && s_scrDiag.layout++ == 0)
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
            unsigned n = ++s_scrDiag.merge_refusals;
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
            while (i-- > 0) tagpu_detour_stub_free(site[i].stub);
            return FIX_STUB;
        }
    s_scr_stress = tagpu_sim_lever("tagpu_scratch.stress");
    if (s_scr_stress) {
        /* no poison, no lever: a freed frame is never pointed at memory that might be mapped */
        s_scr_poison = (unsigned char*)SIM_ALLOC(s_scr_poison, SCR_POISON_SPAN);
        if (!s_scr_poison) {
            s_scr_stress = 0;
            plog("enginefix: composite scratch lever: tagpu_scratch.stress refused -- no region "
                 "to poison freed frames with");
        }
    }
    s_scr_nogrow = tagpu_sim_lever("tagpu_scratch.nogrow");

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
        for (i = 0; i < n; i++) tagpu_detour_stub_free(site[i].stub);
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

SIM_FEATURE(build_list, SIM_PUPPET_RUNS, "battle");

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
SIM_CODE(s_fixCode);
static unsigned int   s_fixCodeUsed;          /* its bytes taken                              */
SIM_VAR(s_fixCodeUsed, SIM_INSTALL);
static unsigned int   s_fixPages;             /* pages made                                   */
SIM_VAR(s_fixPages, SIM_INSTALL);
static unsigned int   s_fixBytes;             /* bytes taken over every page                  */
SIM_VAR(s_fixBytes, SIM_INSTALL);

static unsigned char* fix_code(unsigned int n)
{
    unsigned int take = (n + 15u) & ~15u;
    unsigned char* p;
    if (!n || take > 0x1000u) return NULL;
    if (!s_fixCode || s_fixCodeUsed + n > 0x1000u) {
        unsigned char* page = (unsigned char*)SIM_ALLOC(s_fixCode, 0x1000);
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
#ifndef TAGPU_LIMITS_STOCK
    if (cob_type_rejected(regs[PR_EAX] & 0xFFFFu)) return;
#endif
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
#ifndef TAGPU_LIMITS_STOCK
    if (cob_type_rejected(regs[PR_EAX] & 0xFFFFu)) {
        regs[PR_EAX] = regs[PR_ECX] = n;
        regs[PR_EDX] = (unsigned int)(size_t)list;
        return;
    }
#endif
    bl_append(&list, n, (unsigned short)regs[PR_EAX], "CANBUILD");
    hdr[1] = (unsigned int)(size_t)list;
    hdr[0] = n + 1u;
    regs[PR_EAX] = n + 1u;
    regs[PR_ECX] = n;
    regs[PR_EDX] = (unsigned int)(size_t)list;
}

/* CLASS: simulation, fail closed. A builder's list is what the AI's pick 0x40BDB0 chooses
   from, so an AI player without it builds by a different list. The build menu does not read
   it (exe-reverse-engineering.md, *A builder's build list*). */
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

SIM_FEATURE(download_records, SIM_PUPPET_RUNS, "battle");

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
SIM_VAR(s_dlRoom, SIM_TRANSIENT);
static unsigned int s_dlFiles;                /* the files' own records, 0x42DCF0's file count  */
SIM_VAR(s_dlFiles, SIM_TRANSIENT);

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

SIM_FEATURE(debris_emission, SIM_PUPPET_INDEPENDENT, "battle");

/* A FLYING PIECE'S SMOKE AND FIRE, ONCE A TICK. The piece update 0x421550 (stdcall(ctx, sys),
   reached in play only from the explosion DRAW 0x420B00 at 0x420B18; its other caller, 0x4211A0
   at 0x4211B3, has none) emits a grey smoke puff `call 0x472810` at 0x421583 when sys+0x28 bit 1
   is set and a fire `call 0x472AB0` at 0x4215AA when bit 0 is, both at the piece's position
   piece+0x16 and on layer 9 (`push 9` at 0x42156A, 0x421591), on EVERY DRAW [DISASSEMBLED]. The
   piece itself moves only in the tick 0x4213B0. Of the direct calls into the effects.md emitter
   set these two are the only ones DrawGameScreen reaches (a direct-call sweep from 0x468CF0).
   So the engine
   emits one puff per piece per draw, all of a tick's puffs on one point, and the draw rate is not the tick rate: Impure presents
   from its own thread, so the engine's frame loop turns over as fast as the scene allows -- 236
   to 482 draws a second in a battle at 1024x768 under Xvfb, 4 to 29 draws in one tick of
   fx-rockets, 3200 to 4900 a second on a sparse map (MEASURED; engine/sim.md *the draw rate*).
   A burning piece's trail therefore got as dense as that tick's number of draws -- a solid rope
   that pulsed with every frame's cost -- and the flood filled layer 9, where every impact puff
   lives too, towards its cap, past which each emitter destroys the layer's OLDEST object. The
   particle draws themselves are pure and the frame packet gathers once a tick, so what varied
   was the smoke's density from one tick to the next, not one frame against the next.
   THE RULE: a piece emits only in an explosion draw whose GameTime main+0x38A47 differs from the
   one the previous explosion draw saw. The latch is taken once per draw at 0x420B00's own entry,
   which every draw of the effects passes through -- the frame's 0x469B2C, tagpu_fxown.c's stub
   in its place, and the screenshot function 0x495A30's DrawGameScreen(1,1) at 0x495C76 and
   0x495E66; the movie recorder's DrawGameScreen(0,1) at 0x4962C2 skips the effects
   (0x469B09: test ebx,ebx; je 0x469BD0) and neither latches nor emits -- so
   it holds whatever wrote GameTime and whatever skipped the scheduler: a paused or menu frame
   (0x496920 -> 0x496969, 0x496918 -> 0x49697B) still draws but advances nothing, and emits
   nothing; a frame that ran k ticks emits once, as it did in stock; a load or a replay seek that
   moves GameTime emits once. THE STUBS TAIL-JUMP: the emitters see their stock return addresses
   (0x421588, 0x4215AF) and the explosion draw its stock one, so the recorder's sink keys
   (replay-absent.tsv) are unchanged. Visual only: the emitters draw the particles' own keyed rule
   (*The particles' own random rule*; effects.md 7.7), never a CRT stream or the simulation's,
   and the particle pool is a presentation row (replay-globals.tsv, particlepool). Both latch words are SIM_PRIVATE on that ground: written and read by the
   explosion draw only, never by the simulation -- the same class as tagpu_impai.c's GameTime
   last logged. Game thread only: 0x420B00's callers all run there. */
static volatile int           s_debrisTick;   /* the GameTime the last explosion draw saw     */
SIM_VAR(s_debrisTick, SIM_PRIVATE);
static volatile unsigned char s_debrisEmit;   /* 1 iff this explosion draw's GameTime moved   */
SIM_VAR(s_debrisEmit, SIM_PRIVATE);

static unsigned char* fix_put32(unsigned char* p, const volatile void* a)
{
    unsigned int v = (unsigned int)(size_t)a;
    memcpy(p, &v, 4);
    return p + 4;
}

static int fix_debris_emission(void)
{
    static const unsigned char entry[5] = { 0x83, 0xEC, 0x14, 0x53, 0x55 };   /* sub esp,0x14; push ebx; push ebp */
    static const unsigned char smoke[5] = { 0xE8, 0x88, 0x12, 0x05, 0x00 };   /* call 0x472810 */
    static const unsigned char fire[5]  = { 0xE8, 0x01, 0x15, 0x05, 0x00 };   /* call 0x472AB0 */
    FIXSITE site[3];
    unsigned char* c;
    unsigned char* p;
    int k;
    memset(site, 0, sizeof site);
    site[0].va = 0x00420B00u; site[0].n = 5; memcpy(site[0].was, entry, 5);
    site[1].va = 0x00421583u; site[1].n = 5; memcpy(site[1].was, smoke, 5);
    site[2].va = 0x004215AAu; site[2].n = 5; memcpy(site[2].was, fire, 5);
    if (!fix_match(site, 3)) return FIX_BYTES;
    c = fix_code(48 + 2 * 32);
    if (!c) return FIX_STUB;

    /* the latch, at the explosion draw's entry: eax and ecx are free there (the stdcall callee
       reads neither before it writes it, 0x420B00..0x420B0C), and its first three instructions
       follow it unchanged */
    p = c;
    fix_branch(&site[0], 0xE9, p);
    *p++ = 0xA1; *p++ = 0xE8; *p++ = 0x1D; *p++ = 0x51; *p++ = 0x00;     /* mov eax,[0x511DE8]       */
    *p++ = 0x8B; *p++ = 0x80; *p++ = 0x47; *p++ = 0x8A; *p++ = 0x03;     /* mov eax,                 */
    *p++ = 0x00;                                                        /*   [eax+0x38A47] GameTime */
    *p++ = 0x31; *p++ = 0xC9;                                           /* xor ecx,ecx              */
    *p++ = 0x3B; *p++ = 0x05; p = fix_put32(p, &s_debrisTick);          /* cmp eax,[s_debrisTick]   */
    *p++ = 0x0F; *p++ = 0x95; *p++ = 0xC1;                              /* setne cl                 */
    *p++ = 0x88; *p++ = 0x0D; p = fix_put32(p, &s_debrisEmit);          /* mov [s_debrisEmit],cl    */
    *p++ = 0xA3; p = fix_put32(p, &s_debrisTick);                       /* mov [s_debrisTick],eax   */
    memcpy(p, entry, 5); p += 5;                                        /* the stolen prologue      */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00420B05u);                      /* jmp 0x420B05             */

    for (k = 0; k < 2; k++) {
        p = c + 48 + 32 * k;
        fix_branch(&site[1 + k], 0xE8, p);
        *p++ = 0x80; *p++ = 0x3D; p = fix_put32(p, &s_debrisEmit);      /* cmp byte [s_debrisEmit], */
        *p++ = 0x00;                                                    /*   0                      */
        *p++ = 0x75; *p++ = 0x03;                                       /* jne emit                 */
        *p++ = 0xC2; *p++ = 0x08; *p++ = 0x00;                          /* ret 8: GameTime stood    */
        *p++ = 0xE9;                                                    /* emit: jmp emitter        */
        tagpu_detour_rel(p, k == 0 ? 0x00472810u : 0x00472AB0u);
    }
    return fix_write(site, 3);
}

SIM_FEATURE(oom_message, SIM_PUPPET_RUNS, "battle");

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
SIM_VAR(s_oomText, SIM_TRANSIENT);

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

SIM_FEATURE(sync_keys, SIM_PUPPET_RUNS, "battle");

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
SIM_VAR(s_keyVal, SIM_TRANSIENT);
static unsigned char  s_keyUse[KEYTAB];         /* 1 some type's key, 2 claimed by one type  */
SIM_VAR(s_keyUse, SIM_TRANSIENT);
static unsigned short s_keyOrder[TAGPU_LIM_TYPES];
SIM_VAR(s_keyOrder, SIM_TRANSIENT);
static const char*    s_keyDefs;                /* key_order's array, GAME THREAD             */
SIM_VAR(s_keyDefs, SIM_TRANSIENT);

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

SIM_FEATURE(weapon_ids, SIM_PUPPET_RUNS, "battle");

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
   unit array, unbounded in stock), the 0x0F hit flag, the 0x0F receiver's refusal of
   a cell off the map (stock reads through 0x481550's NULL), and the 0x0D sends and 0x0E reads
   where the demo recorder takes shots and interceptions. The stock build installs them as
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
    SIM_ENGINE(s_wpn, SIM_CONTENT);
#define s_weapons s_wpn.rec
#endif
static DWORD         s_wpnGameTid;        /* DllMain's thread, which is the game loop's      */
SIM_VAR(s_wpnGameTid, SIM_INSTALL);
/* The weapon IDs' log budgets, and B3's 0x0D counts: the diverged-0x0D drops and the checks
   run and passed on a live shooter (evidence the bound ran), incremented in wpn_rx_fired below
   and reported by the wire-robustness block further down (its own enginefix line), in both
   builds. */
static struct {
    volatile LONG notes;
    volatile LONG chatNotes;      /* its own budget: out of play a companion drops by design,
                                     and must not use up the malformed messages' notes */
    volatile LONG frameNotes;     /* wpn_tx_fired's frame test failed: logged once */
    unsigned int  drops0D, in0D;
} s_wpnDiag;
SIM_VAR(s_wpnDiag, SIM_PRIVATE);

static void wpn_note_in(volatile LONG* budget, const char* what, unsigned int a, unsigned int b)
{
    if (InterlockedIncrement(budget) <= 32)
        tagpu_logf("enginefix: weapon IDs: %s (%u, %u)", what, a, b);
}
#define wpn_note(what, a, b) wpn_note_in(&s_wpnDiag.notes, (what), (a), (b))

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
   +4 + slot * 0x1C, three of them, and the extra-weapons module's splice there (0x49D364, a
   site of this table) bounds the slot by the unit's own count; for a live shooter the B3 test
   below drops a slot past that count first. */
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
                    s_wpnDiag.drops0D++;
                    wpn_note("a weapon-fired message whose shooter slot holds another weapon; dropped",
                             id, pkt[0x23]);
                    return 0;
                }
                s_wpnDiag.in0D++;
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

#endif

/* In place of the five `call 0x451DF0` that send a 0x0D, the packet its second argument. A fire
   function builds the packet only under the network bit, or while a skirmish is recorded, when
   tagpu_recorder.c releases its test (0x49D7A0, 0x49DA92, 0x49DC64, 0x49DE25, 0x49DFAF); between
   the test and this send it writes nothing but the packet on its stack, and runs the extra
   weapons' slot-index splice, which writes no simulation state (tagpu_weapons.c, cb_slot_index). In the raised build the
   weapon is the slot's (+0xC) in the four unit senders: the slot is esi at 0x49D859, 0x49DB4D
   and 0x49DD27, and ebx at 0x49DEEE. The meteor's (0x49DFF6) is its function's argument, at the
   send's esp+0x40; that sender leaves +0x1A..+0x23 as the stack held them, so they are
   cleared. The recorder then takes the packet, and 0 skips the send when there is no network:
   a skirmish reaches this site only while it is recorded, and stock sends it nothing.

   THE BUILD'S START. Three of the four unit senders copy into the packet's +1 the three dwords
   their build started the projectile from (0x49D7AD, 0x49DC71, 0x49DE32). 0x49D9C0 does not: it
   builds through 0x49C9C0 from the piece point 0x43E240 wrote to its esp+0x10 (0x49D9D9, passed
   at 0x49DA76), and sends the shooter's centre, unit+0x6A (0x49DA9F..0x49DAC7). The receiver
   0x49D270 builds from msg+1 (0x49D547..0x49D558), so a replay of the packet as sent starts the
   projectile 4-22 px off and its collision test 0x49B090 can hit another unit (DISASSEMBLED;
   MEASURED 2026-10-09 on battle-los, each shot's +1 against the spawn point +0x10 of its
   projectile at the next capture: from the packet, 24 of 24 of this sender's shots off it and
   8543 of 8543 of the others' on it; with the start below, 8227 of 8227 on it). The recorder
   therefore takes this site's start from the frame. The packet sent is not touched, so the wire
   stays stock's. At the send (0x49DB4D) the function's esp is the call's + 0xC, its three
   pushes: the start is at +0x1C and the packet, built at esp+0x1C (0x49DAA2; 0x49DB47 passes
   it), at +0x28, which bounds the frame. Nothing writes the start between the build and the
   send: 0x49C9C0, 0x49C740, 0x47F300 (from 0x49C866) and the emitter 0x4729D0 (its method
   0x474D50) only read it, the
   network block writes from esp+0x1C up, and the extra weapons' splice at 0x49DAD5 writes
   registers and g_diag only (DISASSEMBLED). */
static int __cdecl wpn_tx_fired(unsigned int which, unsigned int* regs)
{
    unsigned char* pkt = (unsigned char*)(size_t)regs[PR_RET + 2];
    const char* ta = *(const char* const*)0x00511DE8;
    const unsigned char* start = NULL;
#ifndef TAGPU_LIMITS_STOCK
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
#endif
    if (which == 1) {
        if (pkt == WPN_ESP_CALL(regs) + 0x28) start = WPN_ESP_CALL(regs) + 0x1C;
        else if (InterlockedIncrement(&s_wpnDiag.frameNotes) == 1)
            tagpu_logf("enginefix: weapon IDs: the 0x0D at 0x49DB4D is not in its frame "
                       "(packet %p, esp %p): the shot is recorded with the packet's start",
                       (void*)pkt, (void*)WPN_ESP_CALL(regs));
    }
    tagpu_rec_shot(pkt, which == 4, start);
    return ta && (*(const unsigned char*)(ta + 0x2A44) & 1);
}

/* In place of the 0x0E sender's two ID-byte reads in area damage 0x49A120: 0x49A78C (the weapon
   in edx, the byte into al) and 0x49A7CD (eax, into cl). The packet is at the site's esp+0x58
   with its target point written. The recorder takes each interception here with its weapon's
   index. The stock build reads the byte as stock does. In the raised build, from 256 up the
   companion goes out here, through the send and the net handle the stock message would use
   (esi +0x52 -> +0x96 -> +4, the reads stock makes next), and the type byte is cleared so the
   send that follows is skipped. */
static int __cdecl wpn_tx_intercept(unsigned int which, unsigned int* regs)
{
    unsigned char* pkt = WPN_ESP_CALL(regs) + 0x58;
    const char* w = (const char*)(size_t)regs[which ? PR_EAX : PR_EDX];
    unsigned int* out = &regs[which ? PR_ECX : PR_EAX];
    int i = tagpu_limits_weapon_index(w);
    unsigned int byte;
#ifdef TAGPU_LIMITS_STOCK
    byte = *(const unsigned char*)(w + WPN_BYTE);
#else
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
#endif
    tagpu_rec_intercept(pkt + 1, i >= 0 ? (unsigned int)i : byte);
    *out = (*out & ~0xFFu) | byte;
    return 0;
}

/* 0x49AF90's match: the first projectile of the pool whose target point is `point` (12 bytes) and
   whose weapon's index is `id`, or NULL. The game thread in play, with the pool there: the pool's
   count is bounded by the pool's allocation. */
static char* wpn_intercepted(const char* ta, const unsigned char* point, unsigned int id)
{
    char* pool = *(char* const*)(ta + 0x141F7);
    int count = *(const int*)(ta + 0x141F3), i;
    if (!pool) return NULL;
    if (count > TAGPU_LIM_PROJ) count = TAGPU_LIM_PROJ;
    for (i = 0; i < count; i++) {
        char* p = pool + i * 0x6B;
        if (!memcmp(p + 0x28, point, 12) &&
            tagpu_limits_weapon_index(*(const char* const*)p) == (int)id)
            return p;
    }
    return NULL;
}

#ifndef TAGPU_LIMITS_STOCK
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
#define wpn_chat_note(what, a, b) wpn_note_in(&s_wpnDiag.chatNotes, (what), (a), (b))

static int __cdecl wpn_rx_chat(unsigned int* regs)
{
    const unsigned char* m = *(const unsigned char* const*)(WPN_ESP_JMP(regs) + 0x10);
    const char* ta = *(const char* const*)0x00511DE8;
    char* p;
    unsigned int id;
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
    if (!*(char* const*)(ta + 0x141F7)) {
        wpn_chat_note("a companion arrived with no projectile pool; dropped", id, 0);
        return 0;
    }
    if ((p = wpn_intercepted(ta, m + 3, id)) != NULL) E_DETONATE(p);
    return 0;
}
#endif

int tagpu_wpn_replay_intercept(const unsigned char* point, unsigned int weapon)
{
    const char* ta = *(const char* const*)0x00511DE8;
    char* p;
    if (!ta || *(const unsigned int*)(ta + 0x391F5) != 0x00499200u ||
        GetCurrentThreadId() != s_wpnGameTid || weapon >= TAGPU_LIM_WEAPONS)
        return 0;
    if (!(p = wpn_intercepted(ta, point, weapon))) return 0;
    E_DETONATE(p);
    return 1;
}

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
        static const unsigned char icptSend[2][5] = {
            { 0xE8, 0x43, 0x76, 0xFB, 0xFF }, { 0xE8, 0x02, 0x76, 0xFB, 0xFF } };
        static const unsigned char icptCmp[11] = { 0x8B, 0x1E, 0x8A, 0x9B, 0x0A, 0x01, 0x00, 0x00,
                                                   0x3A, 0x5F, 0x0D };
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
    {
        static const struct { unsigned int va; unsigned char was[5]; } fired[5] = {
            { 0x0049D859, { 0xE8, 0x92, 0x45, 0xFB, 0xFF } },
            { 0x0049DB4D, { 0xE8, 0x9E, 0x42, 0xFB, 0xFF } },
            { 0x0049DD27, { 0xE8, 0xC4, 0x40, 0xFB, 0xFF } },
            { 0x0049DEEE, { 0xE8, 0xFD, 0x3E, 0xFB, 0xFF } },
            { 0x0049DFF6, { 0xE8, 0xF5, 0x3D, 0xFB, 0xFF } },
        };
        static const unsigned char icptRead[2][6] = {
            { 0x8A, 0x82, 0x0A, 0x01, 0x00, 0x00 }, { 0x8A, 0x88, 0x0A, 0x01, 0x00, 0x00 } };
        int k;
        for (k = 0; k < 5; k++) {
            if (!(a = fix_code(32))) return 0;
            wpn_send_or_skip(wpn_call(a, (const void*)wpn_tx_fired, k, 0), 0x00451DF0, 0x0C);
            fix_branch(wpn_site(t, fired[k].va, 5, fired[k].was, "0x0D sender, the ID and the recorder"),
                       0xE8, a);
        }
        for (k = 0; k < 2; k++) {
            if (!(a = fix_code(32))) return 0;
            *wpn_call(a, (const void*)wpn_tx_intercept, k, 0) = 0xC3;
            fix_branch(wpn_site(t, k ? 0x0049A7CD : 0x0049A78C, 6, icptRead[k],
                                "0x0E sender, the ID and the recorder"), 0xE8, a);
        }
    }
    return !t->over;
}

/* CLASS: simulation, fail closed. The 0x0F hit flag is a wire format: a player without it
   reads a flagged hit as stock reads it. In the raised build its sites are rows of the limits
   table; in the stock build they join the table here. */
static int fix_weapon_ids(void)
{
#ifdef TAGPU_LIMITS_STOCK
    static WPNSITES w;
    SIM_VAR(w, SIM_INSTALL);
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

SIM_FEATURE(wire_bounds, SIM_PUPPET_RUNS, "battle");

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

/* 1 for CreateFromNetwork's return into the 0x09 case (0x4553E9) or into the ghost-commander
   replay, which enters it as the case does with a record the case refused in state 5
   (fix_ghost_commander): the receivers that key on the case's return treat both alike */
static int ghost_is_09_return(unsigned int ret);

/* the wire's diagnostics */
static struct {
    /* The B4/B5 oracle counters (§10), observe-only. morph = a create onto a live slot whose
       type changes. dcreate = CreateFromNetwork called by the 0x2C dirty list (returning to
       0x48BA05): a dirty entry whose type is not its slot's, the unit made from the entry
       rather than from its own 0x09 — into an empty slot, the ghost commander's mechanism
       (§2). rcreate = the same from the round robin (returning to 0x48B49C). ghost = the
       engine's own ghost sweep firing (a round-robin type 0 over an occupied slot). */
    unsigned int morph, dcreate, rcreate, ghost;
    /* CreateFromNetwork's own refusal, for B5: when the create's player has no block
       ([rec+0x67] == 0) the jne at 0x486229 falls through to 0x48622B, which returns 0;
       counted per caller. noArr = wire_s09 found no unit array to bound against */
    unsigned int noBlk09, noBlkDirty, noBlkRR, noArr;
    /* drop counters, per message */
    unsigned int drop09, drop09Blk, drop0A, drop0C, drop0Ckill, drop0B, drop2C;
    unsigned int drop2CLen, drop2CStale;
    /* a 0x2C reference past the array, made no unit: the move payload's target, the carrier */
    unsigned int drop2CTarget, drop2CCarrier;
    /* the splitter's stops and the pump's re-pointed message pointer run on whichever thread
       pumps */
    volatile LONG split, pump;
    /* records accepted off the wire, per receiver: evidence each bound ran on real traffic
       (the same role as the line-of-sight own-row counters). 0x0B and 0x0C count only the
       dispatcher's call (return 0x455417 / 0x455428), not the local kill and damage paths
       that share the function; 0x0A has only the dispatcher's stub to count at; 0x2C counts
       messages, dirty entries, dirty creates and round-robin entries. */
    unsigned int in09, in0A, in0B, in0C;
    unsigned int in2C, in2CDirty, in2CCreate, in2CRR;
    /* the sender-block rule: a wire 0x09's slot lies in the block of its TRANSPORT sender,
       the dispatcher's edi (its player record, the one the 0x2C and 0x0D cases pass), which
       CreateFromNetwork saves with its last push (0x4861EC), so it is [esp+0] at 0x4861F7.
       The sender is bounded as one of the ten records at main+0x1B63, stride 0x14B (the loop
       at 0x406E05 counts ten); the pump names record ten, one past them, for a sender it
       cannot find (0x453E14). argDiff counts a create whose player argument is not the
       sender. */
    unsigned int argDiff, sendSeen;
    volatile LONG notes;          /* wire_drop's log budget */
} s_wireDiag;
SIM_VAR(s_wireDiag, SIM_PRIVATE);
/* the type carried from the type check to the after-create check (game thread only) */
static int s_wire_2c_type, s_wire_rr_type;
SIM_VAR(s_wire_2c_type, SIM_TRANSIENT);
SIM_VAR(s_wire_rr_type, SIM_TRANSIENT);
/* the 0x2C being parsed: its reader's address and its end in bits (game thread only). A check
   whose reader is not this one stops, so a nested or stale parse cannot borrow the bound. */
static const unsigned char* s_wire2cRd;
SIM_VAR(s_wire2cRd, SIM_TRANSIENT);
static unsigned long long   s_wire2cEnd;
SIM_VAR(s_wire2cEnd, SIM_TRANSIENT);
static const unsigned char* s_wire2cCopy;
SIM_VAR(s_wire2cCopy, SIM_TRANSIENT);
/* the stop path's zero dword: a misframed 0x2C reader points here so its flag bit reads 0 */
static const unsigned int s_wireZero;
/* the receive's buffer and delivered length, per thread (see wire_rx_note) */
static DWORD s_wireTlsBuf = TLS_OUT_OF_INDEXES, s_wireTlsLen = TLS_OUT_OF_INDEXES;
SIM_TLS(s_wireTlsBuf, SIM_TRANSIENT);
SIM_TLS(s_wireTlsLen, SIM_TRANSIENT);

unsigned int tagpu_wire_received(const void* message)
{
    unsigned int owned = tagpu_rec_wire_received(message);
    if (owned) return owned;
    if (!message || s_wireTlsBuf == TLS_OUT_OF_INDEXES || s_wireTlsLen == TLS_OUT_OF_INDEXES ||
        message != TlsGetValue(s_wireTlsBuf)) return 0;
    return (unsigned int)(size_t)TlsGetValue(s_wireTlsLen);
}

static void wire_drop(unsigned int* c, const char* what, unsigned int a, unsigned int b)
{
    (*c)++;
    if (InterlockedIncrement(&s_wireDiag.notes) <= 64)
        tagpu_logf("enginefix: wire robustness: %s (%u, %u)", what, a, b);
}

/* ---- the 0x2C length ----------------------------------------------------------------------

   tagpu_wire_received reports the current owned receive frame's exact delivered span,
   or the retail receive note's span when the primary installer has not armed. The 0x2C
   copy uses that same API as identified chat; an interior pointer inherits no bound.

   On the retail path, 0x4534E0 takes one message into the buffer at
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
        if (InterlockedIncrement(&s_wireDiag.pump) <= 16)
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

/* the 0x2C being parsed, with WIRE_2C_PAD zero bytes after it: written whole by the copy before
   the parse reads it, on the game thread only (THREADS above) */
static unsigned char s_wire2cBuf[WIRE_2C_COPY];
SIM_VAR(s_wire2cBuf, SIM_TRANSIENT);

/* 0x2C, the receiver's first instruction after its pushes, branch at 0x48B92B (before the
   header's reads). eax = the message. Copies it -- the receive's delivered length, and the
   size field when that is smaller, so at most 0xFFFF bytes -- into s_wire2cBuf,
   zeroes WIRE_2C_PAD bytes after it, and hands the copy back in eax, which stock's
   0x48B933 stores as the reader's buffer. Every read of the parse is then inside memory we
   own, whatever the message says. A message that is not the buffer the receive described,
   or no buffer, drops to 0x48BAC8. Continue -> xor esi,esi; push 8; lea ecx,[esp+0x14];
   0x48B933. Nothing reads the reader after the handler returns: the dispatcher's case
   jumps to the next receive (0x4553F9). */
static int __cdecl wire_s2c_copy(unsigned int* r)
{
    const unsigned char* pkt = (const unsigned char*)(size_t)r[PR_EAX];
    unsigned int len = tagpu_wire_received(pkt);
    unsigned char* copy = s_wire2cBuf;
    s_wire2cRd = 0;
    s_wire2cCopy = 0;
    if (!len) {
        wire_drop(&s_wireDiag.drop2CStale, "a 0x2C is not the message the receive delivered; dropped",
                  0, len);
        return 0;
    }
    if (len >= 3u && *(const unsigned short*)(pkt + 1) < len) len = *(const unsigned short*)(pkt + 1);
    if (len > WIRE_2C_MAX) len = WIRE_2C_MAX;
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
    if (InterlockedIncrement(&s_wireDiag.split) <= 16)
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
        wire_drop(&s_wireDiag.drop2CLen, "a 0x2C check found another reader; stream stopped", 0, 0);
        return 0;
    }
    pos = wire_pos(rd);
    if (!wire_bits_ok(pos, need, s_wire2cEnd)) {
        if (InterlockedIncrement(&s_wireDiag.notes) <= 64)
            tagpu_logf("enginefix: wire robustness: a 0x2C runs past its own length at %s "
                       "(bit %u + %u of %u); stream stopped", where, (unsigned int)pos, need,
                       (unsigned int)s_wire2cEnd);
        s_wireDiag.drop2CLen++;
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
    if (!ta || !rec) { s_wireDiag.noArr++; return 0; }
    first = *(const char* const*)(ta + 0x14357);
    last  = *(const char* const*)(ta + 0x1435B);
    if (!first || last < first) { s_wireDiag.noArr++; return 0; }
    max   = (unsigned int)(last - first) / 0x118;
    count = *(const unsigned int*)(ta + 0x1438F);
    idx   = *(const unsigned short*)(rec + 3);
    type  = *(const unsigned short*)(rec + 1);
    if (!wire_index_ok(idx, max) || !wire_type_ok(type, count)) {
        wire_drop(&s_wireDiag.drop09, "a unit-create names a unit past the array or a bad type; dropped",
                  idx, type);
        return 0;
    }
    {
        char* slot = (char*)first + (size_t)idx * 0x118;
        unsigned int ret = *(const unsigned int*)(sp + 0x1C);   /* CreateFromNetwork's caller */
        int noblk = *(const unsigned int*)(ta + r[PR_EDX] + 0x1BCA) == 0;   /* 0x486220 */
        if (ghost_is_09_return(ret)) {
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
                wire_drop(&s_wireDiag.drop09Blk, "a unit-create from a sender whose block does not hold "
                          "the unit; dropped", si, idx);
                return 0;
            }
            s_wireDiag.in09++;
            if (r[PR_EDX] != si * 0x14B) s_wireDiag.argDiff++;
            if (!(s_wireDiag.sendSeen & (1u << si))) {
                s_wireDiag.sendSeen |= 1u << si;
                tagpu_logf("enginefix: wire robustness: 0x09 from sender %u (type %u, '%.30s'), "
                           "player arg %u, slot %u, sender block %u..%u", si,
                           (unsigned int)*(const unsigned char*)(snd + 0x73), snd + 0x2B,
                           r[PR_EDX] / 0x14B, idx, (unsigned int)(sf - first) / 0x118,
                           (unsigned int)(sl - first) / 0x118);
            }
            if (noblk) s_wireDiag.noBlk09++;
        } else if (ret == 0x0048BA05u) {
            s_wireDiag.dcreate++;
            if (noblk) s_wireDiag.noBlkDirty++;
        } else if (ret == 0x0048B49Cu) {
            s_wireDiag.rcreate++;
            if (noblk) s_wireDiag.noBlkRR++;
        }
        if (noblk && InterlockedIncrement(&s_wireDiag.notes) <= 64)
            tagpu_logf("enginefix: wire robustness: CreateFromNetwork refuses a create for player "
                       "%u, which has no block (from %08X, slot %u, type %u)",
                       r[PR_EDX] / 0x14B, ret, idx, type);
        if ((*(const unsigned int*)(slot + 0x110) & 0x10000000u) &&
            *(const unsigned short*)(slot + 0xA6) &&
            *(const unsigned short*)(slot + 0xA6) != (unsigned short)type)
            s_wireDiag.morph++;
        r[PR_ESI] = (unsigned int)(size_t)slot;
        *(unsigned int*)(sp + 0x20) = r[PR_EDX];   /* stock's mov [esp+0x20],edx at 0x4861FE */
    }
    return 1;
}

/* the destructor's caller, past C4's tx_wrap (TRANSPORTED EXPLOSIONS, below), which makes
   every destructor call */
static unsigned int tx_dtor_caller(const unsigned char* ret);

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
        wire_drop(&s_wireDiag.drop0C, "a unit-destroy names a unit past the array; dropped", idx, max);
        return 0;
    }
    if (tx_dtor_caller(WPN_ESP_JMP(r) + 0x78) == 0x00455428u) s_wireDiag.in0C++;
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
                wire_drop(&s_wireDiag.drop0Ckill, "a unit-destroy names a killer past the array; made NULL",
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
        wire_drop(&s_wireDiag.drop0B, "a damage message names a unit past the array; dropped",
                  vidx > aidx ? vidx : aidx, max);
        return 0;
    }
    if (*(const unsigned int*)(WPN_ESP_JMP(r) + 0x0C) == 0x00455417u) s_wireDiag.in0B++;
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
        wire_drop(&s_wireDiag.drop0A, "an attach names no child or a unit past the array; dropped",
                  child, parent);
        return 0;
    }
    s_wireDiag.in0A++;
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
        wire_drop(&s_wireDiag.drop2C, "a 0x2C names a player whose block is not a valid slot run; dropped",
                  0, n);
        return 0;
    }
    s_wireDiag.in2C++;
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
        wire_drop(&s_wireDiag.drop2C, "a 0x2C dirty entry's slot delta is out of range; stream stopped",
                  (unsigned int)delta, n);
        return 0;
    }
    s_wireDiag.in2CDirty++;
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
        wire_drop(&s_wireDiag.drop2C, "a 0x2C dirty entry's type is out of range; stream stopped",
                  type, count);
        return 0;
    }
    defs = *(const char* const*)(ta + 0x1439B);
    if (!defs || *(const unsigned char*)(defs + (size_t)type * 0x249 + 0x22F) != 1) {
        wire_drop(&s_wireDiag.drop2C, "a 0x2C dirty entry's type has no move class; stream stopped",
                  type, count);
        return 0;
    }
    s_wire_2c_type = (int)type;
    if (*(const unsigned short*)(slot + 0xA6) == (unsigned short)type) return 2;
    s_wireDiag.in2CCreate++;
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
        wire_drop(&s_wireDiag.drop2C, "a 0x2C dirty create did not take; stream stopped",
                  (unsigned int)s_wire_2c_type, 0);
        return 0;
    }
    obj = *(const char* const*)slot;
    if (!obj || !*(const char* const*)obj) {
        wire_drop(&s_wireDiag.drop2C, "a 0x2C dirty unit has no mover object; stream stopped", 0, 0);
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
        if (slot && *(const unsigned short*)(slot + 0xA6)) s_wireDiag.ghost++;
        s_wireDiag.in2CRR++;
        return 1;
    }
    if (!wire_type_ok(type, count)) {
        wire_drop(&s_wireDiag.drop2C, "a 0x2C round-robin type is out of range; entry stopped", type, count);
        return 0;
    }
    s_wire_rr_type = (int)type;
    s_wireDiag.in2CRR++;
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
        wire_drop(&s_wireDiag.drop2C, "a 0x2C round-robin create did not take; entry stopped",
                  (unsigned int)s_wire_rr_type, 0);
        return 0;
    }
    obj = *(const char* const*)(slot + 0x9E);
    if (!obj) {
        wire_drop(&s_wireDiag.drop2C, "a 0x2C round-robin unit has no model object; entry stopped", 0, 0);
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
        wire_drop(&s_wireDiag.drop2CTarget, "a 0x2C move payload names a target past the array; no target",
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
        wire_drop(&s_wireDiag.drop2CCarrier, "a 0x2C round robin names a carrier past the array; no "
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
SIM_VAR(s_wireStubBytes, SIM_INSTALL);

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

    if (!SIM_TLS_ALLOC(s_wireTlsBuf) || !SIM_TLS_ALLOC(s_wireTlsLen))
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
                     " drop 09=%u blk=%u 0a=%u 0b=%u 0c=%u kill=%u 2c=%u len=%u stale=%u"
                     " 0d=%u split=%u target=%u carrier=%u"
                     " morph=%u dcreate=%u rcreate=%u ghost=%u argdiff=%u pump=%u"
                     " noblock 09=%u dirty=%u rr=%u noarr=%u",
                     s_wireDiag.in09, s_wireDiag.in0A, s_wireDiag.in0B, s_wireDiag.in0C, s_wpnDiag.in0D, s_wireDiag.in2C,
                     s_wireDiag.in2CDirty, s_wireDiag.in2CCreate, s_wireDiag.in2CRR,
                     s_wireDiag.drop09, s_wireDiag.drop09Blk, s_wireDiag.drop0A, s_wireDiag.drop0B, s_wireDiag.drop0C, s_wireDiag.drop0Ckill, s_wireDiag.drop2C,
                     s_wireDiag.drop2CLen, s_wireDiag.drop2CStale, s_wpnDiag.drops0D,
                     (unsigned int)s_wireDiag.split, s_wireDiag.drop2CTarget, s_wireDiag.drop2CCarrier,
                     s_wireDiag.morph, s_wireDiag.dcreate, s_wireDiag.rcreate, s_wireDiag.ghost, s_wireDiag.argDiff,
                     (unsigned int)s_wireDiag.pump,
                     s_wireDiag.noBlk09, s_wireDiag.noBlkDirty, s_wireDiag.noBlkRR, s_wireDiag.noArr);
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
                 !wire_block_ok(p0 + 1, p0l, begin, arr_last, 4) &&                /* unaligned */
                 wire_block_ok(p0 + 4 * 0x118, arr_last, begin, arr_last, 4);       /* the last block ends on arr_last */
        t[n].name = "block shape ok/rejects"; t[n].got = ok; t[n].want = 1; n++;
    }
    for (i = 0; i < n; i++) {
        if (t[i].got != t[i].want) bad++;
        tagpu_logf("enginefix: wirecheck %-26s got=%d want=%d %s",
                   t[i].name, t[i].got, t[i].want, t[i].got == t[i].want ? "OK" : "FAIL");
    }
    tagpu_logf("enginefix: wirecheck %d predicate cases, %d failed", n, bad);
}

SIM_FEATURE(victim_caps, SIM_PUPPET_RUNS, "battle-los");

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
SIM_TLS(s_dmgTls, SIM_TRANSIENT);
/* peekable counters (their addresses are in the enginefix line), any thread */
static struct {
    volatile LONG unitRepeats;  /* a victim past stock's 20 found again   */
    volatile LONG featRepeats;  /* an anchor past stock's 64 found again  */
    volatile LONG refused;      /* a pointer outside its array            */
    volatile LONG unframed;     /* a list block run outside the wrapper   */
} s_dmgDiag;
SIM_VAR(s_dmgDiag, SIM_PRIVATE);
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
    if (!f && InterlockedIncrement(&s_dmgDiag.unframed) == 1)
        tagpu_logf("enginefix: area damage's victim list ran outside its wrapper; its victims are "
                   "not deduplicated (counted at 0x%08X)", (unsigned int)(size_t)&s_dmgDiag.unframed);
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
    if (!begin || unit < begin) { InterlockedIncrement(&s_dmgDiag.refused); return 1; }
    off = (size_t)(unit - begin);
    if (off % DMG_UNIT_STRIDE || off / DMG_UNIT_STRIDE >= count) {
        InterlockedIncrement(&s_dmgDiag.refused);
        return 1;
    }
    return dmg_set_seen(&f->unit, (unsigned int)(off / DMG_UNIT_STRIDE), DMG_UNIT_CAP,
                        &s_dmgDiag.unitRepeats);
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
    if (!grid || cell < grid) { InterlockedIncrement(&s_dmgDiag.refused); return 1; }
    off = (size_t)(cell - grid);
    if (off % DMG_CELL || off / DMG_CELL >= cells || off / DMG_CELL >= DMG_KEY_PAST - 1u) {
        InterlockedIncrement(&s_dmgDiag.refused);
        return 1;
    }
    return dmg_set_seen(&f->feat, (unsigned int)(off / DMG_CELL), DMG_FEAT_CAP, &s_dmgDiag.featRepeats);
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
    if (!SIM_TLS_ALLOC(s_dmgTls)) { lim_no_stub(); return FIX_TABLE; }

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

SIM_FEATURE(stacked_air, SIM_PUPPET_RUNS, "battle");

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
       write of it and of s_airIn. It has two holders. This C code holds it across no engine
       code but one: the allocator 0x4D83B0 growing a call's list, which takes its own critical
       section and calls nothing that takes this one; the out-of-memory handler 0x49E700 runs
       inside the allocator's section, or after it from eng_alloc_or_exit, and ends the process.
       The replay's table write (tagpu_replay.c, tables_put: each capture point, frame visit and
       seek) holds it from tagpu_patches_restore_begin to the end of tagpu_patches_restored, on
       the game thread: C code, HeapAlloc/HeapFree and
       the log sink run inside it, and none of them takes s_airLock. The lock order is
       s_airLock first, then the allocator's, the heap's or the log's, never the reverse;
     - a lifetime: a call's list lives in its frame (the wrapper above), which exists exactly
       while that call runs, so nesting (the interceptor tail at 0x49A764 re-enters area damage
       after the serving is done) gets a list of its own.
   CLASS: simulation, fail closed. Air only [DECIDED 2026-09-25]: a ground unit that loses slot
   A stays stock's. */
#define AIR_SLOTS 65536u               /* a cell's unit slot is a u16                      */

static CRITICAL_SECTION s_airLock;
SIM_LOCK(s_airLock);
/* each entry is a unit slot; the order is the order area damage serves aircraft in */
static const SimField air_pool_layout[] = { SIM_FIELD_AT(0, 2, SIMF_ID, SIM_ID_UNIT_SLOT), SIM_FIELD_END };
static unsigned short   s_airPool[AIR_SLOTS];
static unsigned char    s_airIn[AIR_SLOTS];  /* 1 iff the slot is in s_airPool[0, s_airN)   */
SIM_ROWS(s_airIn, SIM_STATE, SIM_ID_UNIT_SLOT, NULL);
static unsigned int     s_airN;
SIM_VAR(s_airN, SIM_STATE);
SIM_ROWS_LIVE(s_airPool, SIM_STATE, SIM_ID_NONE, s_airN, air_pool_layout);
static struct { volatile LONG served; } s_airDiag;  /* aircraft handed to the engine, any thread */
SIM_VAR(s_airDiag, SIM_PRIVATE);

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
            InterlockedIncrement(&s_airDiag.served);
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
    t = fix_code(64); s = fix_code(64); m = fix_code(32); k = fix_code(48);
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

    /* the unit tick's call: pushfd; pushad; call tagpu_replay_capture; call tagpu_rec_capture;
       call air_rebuild; popad; popfd; call 0x48AD30; pushfd; pushad; call tagpu_replay_step;
       popad; popfd; ret -- the unit tick takes no argument and returns with a plain `ret`
       (0x48B080), and the site pushes nothing before its call, so calling it from here is the
       site's own call; every register the step after it uses (ebx, the loop's count) is kept.
       The replay's capture-point write comes first: on a level that plays a recording it writes
       the recording's globals and projectile pool for the iteration the host holds, so that it is
       their last writer before anything in the step reads them, the recorder's dump included
       (tagpu_replay.c, THE GLOBALS AND THE PROJECTILES); elsewhere it returns at once. At the
       level's first iteration it makes the level's own units and writes image T before that
       write, so the host's first unit tick gives each the script slice the recording machine's
       gave (tagpu_replay.c, THE FIRST STEP'S UNITS). Then the demo recorder's capture: it records the world between two
       iterations (tagpu_recorder.c), and the pool it records is the one the last iteration left,
       which air_rebuild then derives again from the units it records. On a level that plays a
       recording the capture records nothing, and the replay's step runs after the unit tick: it
       writes the world the recording machine's unit tick left, so the rest of the iteration --
       the players phase's sight stamps above all -- reads what it read there (tagpu_replay.c),
       and air_rebuild derives the next pool from the units it wrote. */
    p = k;
    *p++ = 0x9C;
    *p++ = 0x60;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)tagpu_replay_capture); p += 4;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)tagpu_rec_capture); p += 4;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)air_rebuild); p += 4;
    *p++ = 0x61;
    *p++ = 0x9D;
    *p++ = 0xE8; tagpu_detour_rel(p, 0x0048AD30); p += 4;
    *p++ = 0x9C;
    *p++ = 0x60;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)tagpu_replay_step); p += 4;
    *p++ = 0x61;
    *p++ = 0x9D;
    *p++ = 0xC3;

    lim_branch(0x0049A664, sizeof tail, tail, 0xE9, (unsigned int)(size_t)t,
               "stacked aircraft: served at the end of area damage's walk");
    lim_branch(0x0049A415, sizeof sel, sel, 0xE9, (unsigned int)(size_t)s,
               "stacked aircraft: area damage's slot selector");
    lim_branch(0x0047CF98, sizeof stamp, stamp, 0xE9, (unsigned int)(size_t)m,
               "stacked aircraft: the stamp's airborne path joins the pool");
    lim_branch(0x004954ED, sizeof tick, tick, 0xE8, (unsigned int)(size_t)k,
               "stacked aircraft: the pool rebuilt before the unit tick, after the replay's write "
               "and the recorder's capture; the replay's step after it");
    return FIX_TABLE;
}

SIM_FEATURE(step_phases, SIM_PUPPET_RUNS, "battle");

/* THE STEP'S PHASES. The step 0x495490 calls the projectile tick 0x49B720 at 0x495513 and the
   explosion tick 0x420F30 at 0x495539, each with no argument, each returning with a plain `ret`
   (0x49BE5C, 0x421147) [DISASSEMBLED]. Each call goes through a stub: pushfd; pushad; push phase;
   call tagpu_rec_phase; add esp,4; popad; popfd; call the function; pushfd; pushad; push phase;
   call tagpu_replay_phase; add esp,4; popad; popfd; ret. The recorder notes the phase a feature
   root is made in, and on a level that plays a recording the replay makes the recording's roots
   of that phase once the host's own call has returned (tagpu_replay.c, THE PHASES); elsewhere
   both return at once. The site pushes nothing before either call and reloads what it uses after
   it (0x495518, 0x49553E), and the stub keeps every register and the flags.
   CLASS: in the fail-closed table. Its absence changes no shared state of a live game, but a
   playing on a host without it would hold its later-phase roots a whole step and a recording
   made without it would carry every root as the unit tick's, so neither may run without it. */
static int fix_step_phases(void)
{
    static const unsigned char proj[5] = { 0xE8, 0x08, 0x62, 0x00, 0x00 };   /* call 0x49B720 */
    static const unsigned char fall[5] = { 0xE8, 0xF2, 0xB9, 0xF8, 0xFF };   /* call 0x420F30 */
    static const struct { unsigned site, callee, phase; } call[2] = {
        { 0x00495513, 0x0049B720, REC_PHASE_PROJ },
        { 0x00495539, 0x00420F30, REC_PHASE_LATE },
    };
    unsigned char *k[2], *p;
    int i;
    k[0] = fix_code(48); k[1] = fix_code(48);
    if (!k[0] || !k[1]) { lim_no_stub(); return FIX_TABLE; }
    for (i = 0; i < 2; i++) {
        p = k[i];
        *p++ = 0x9C;
        *p++ = 0x60;
        *p++ = 0x6A; *p++ = (unsigned char)call[i].phase;
        *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)tagpu_rec_phase); p += 4;
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
        *p++ = 0x61;
        *p++ = 0x9D;
        *p++ = 0xE8; tagpu_detour_rel(p, call[i].callee); p += 4;
        *p++ = 0x9C;
        *p++ = 0x60;
        *p++ = 0x6A; *p++ = (unsigned char)call[i].phase;
        *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)tagpu_replay_phase); p += 4;
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
        *p++ = 0x61;
        *p++ = 0x9D;
        *p++ = 0xC3;
    }
    lim_branch(0x00495513, sizeof proj, proj, 0xE8, (unsigned int)(size_t)k[0],
               "the step's phases: the projectile tick's call");
    lim_branch(0x00495539, sizeof fall, fall, 0xE8, (unsigned int)(size_t)k[1],
               "the step's phases: the explosion tick's call");
    return FIX_TABLE;
}

SIM_FEATURE(script_slice, SIM_PUPPET_RUNS, "slice-walkers battle tenseat contest-waves teleport transport-turn");

/* THE SCRIPT SLICE. The unit tick 0x48AD30 gives each unit it reaches one script slice:

     0x48ADDF  8B 8E 9A 00 00 00   mov ecx,[esi+0x9A]      the unit's COB object
     0x48ADE5  85 C9               test ecx,ecx
     0x48ADE7  74 07               je 0x48ADF0             no script, no slice
     0x48ADE9  6A 01               push 1
     0x48ADEB  E8 70 5F 02 00      call 0x4B0D60           DoScriptsNow(cob, 1), thiscall, `ret 4`
     0x48ADF0  8A 86 FA 00 00 00   mov al,[esi+0xFA]

   The site's facts [DISASSEMBLED, rel8/rel32 and absolute-dword scans of the whole file]: the only
   branches to 0x48ADDF are AutoAim's skips (0x48ADCC, 0x48ADD7); nothing branches into
   0x48ADE0..0x48ADEF and no dword names an address there; at 0x48ADF0 the flags are dead (a mov,
   then 0x48ADF6 test al,al sets them) and so are eax (reloaded at 0x48ADF0 and 0x48AE02), edx
   (0x437910's call clobbered it on every path) and ecx (the COB or DoScriptsNow's leftovers),
   while esi (the unit), edi (its player record), ebx and ebp are live. 0x48ADEB is DoScriptsNow's
   only caller in the file, and dt is the only amount by which a sleep counts down (the runner
   0x4B0DEF..0x4B0E01), so a unit's COB clock is the number of slices it has had.
   The 17 bytes jump to a stub: pushad; push esi; call unit_slice; add esp,4; popad; jmp 0x48ADF0.
   popad gives every register back and the flags it leaves are dead. C makes the call so that one
   function, tagpu_slice_give, gives every slice on both machines, and the count and the call
   cannot disagree.
   THE ORDERING it rests on: the stub runs in the engine's own turn order, and on a recording level
   the marker (tagpu_rec_slice) is written before the slice it names, so the events the slice makes
   follow it in the stream and the marker's place there is the slice's place; and the recorder's
   turn (tagpu_rec_turn) is set here for every unit the loop is on, so a footprint event of this
   turn carries this slot (THE CONTEST ORDER, tagpu_recfmt.h). On a level that plays
   a recording, tagpu_replay_slice_defer skips the slices the iteration's markers name, and the
   replay gives each at its marker (tagpu_replay.c, THE SCRIPT SLICES).
   Deleting the stub deletes the recording's markers, the playing's deferral and the count root
   (tools/replay/tarec.py, SLICE_ROOT) with it.
   CLASS: in the fail-closed table, in both builds: a recording made without it would carry no
   marker, and a playing without it would give every slice in its unit tick. */

/* 0x4B0D60, DoScriptsNow: thiscall(cob, dt), `ret 4` */
typedef void (__thiscall* PFN_DoScripts)(char* cob, int dt);
#define E_DoScriptsNow ((PFN_DoScripts)0x004B0D60u)

void tagpu_slice_give(char* cob, unsigned where)
{
    E_DoScriptsNow(cob, 1);
    tagpu_rec_slice_given(where);
}

static void __cdecl unit_slice(char* unit)
{
    char* cob;
    int placed;
    tagpu_rec_turn(unit);               /* every unit's turn, a script or none */
    memcpy(&cob, unit + 0x9A, 4);       /* 0x48ADDF's own read, of the unit 0x48AD30's loop is on */
    if (!cob) return;                   /* 0x48ADE5..0x48ADE7: no script, no slice */
    placed = tagpu_rec_slice(unit, cob);
    if (tagpu_replay_slice_defer(unit)) return;
    tagpu_slice_give(cob, placed ? 1u : 0u);
}

static int fix_script_slice(void)
{
    static const unsigned char call[17] = {          /* 0x48ADDF..0x48ADEF */
        0x8B, 0x8E, 0x9A, 0x00, 0x00, 0x00,          /* mov ecx,[esi+0x9A]  */
        0x85, 0xC9,                                  /* test ecx,ecx        */
        0x74, 0x07,                                  /* je 0x48ADF0         */
        0x6A, 0x01,                                  /* push 1              */
        0xE8, 0x70, 0x5F, 0x02, 0x00,                /* call 0x4B0D60       */
    };
    unsigned char *k = fix_code(32), *p;
    if (!k) { lim_no_stub(); return FIX_TABLE; }
    p = k;
    *p++ = 0x60;                                                     /* pushad       */
    *p++ = 0x56;                                                     /* push esi     */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)unit_slice); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                           /* add esp,4    */
    *p++ = 0x61;                                                     /* popad        */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0048ADF0); p += 4;            /* jmp 0x48ADF0 */
    lim_branch(0x0048ADDF, sizeof call, call, 0xE9, (unsigned int)(size_t)k,
               "the script slice: the unit tick's DoScriptsNow(1), placed by the recording");
    return FIX_TABLE;
}

SIM_FEATURE(killed_query, SIM_PUPPET_RUNS, "battle slice-walkers codec-decline");

/* THE KILLED SCRIPT OF A REPLAYED DEATH. Where a unit's death is the machine's own,
   Send_UnitDeath 0x4864B0 runs Killed as a query: 0x4B0BC0("Killed" 0x508BE8, &severity, &corpse,
   0, 0) at 0x4865C3, which allocates one thread and runs that thread alone at dt 0 (0x4B0C40: the
   runner 0x4B0DA0 on its one record, 0x4B0CE1), before the destructor 0x4866D0. A death a peer
   receives reaches the destructor with mode 0, and there Killed is a start with run-now set:
   0x4B0A70("Killed", 0, 1, 1, severity, 0, 0, 0) at 0x486877, when the record's severity +9 is
   above 0 (0x48684A..0x48685A), and a run-now start runs every record of the object at dt 0
   (0x4B0940's and 0x4B0B00's run-now branch), so a thread of the dying unit that was only waiting
   -- an aim script whose turn the last slice finished -- runs on, and starts what it starts
   (RestoreAfterDelay), before the destructor deletes the object. A playing applies the recording's
   deaths as a peer's, through C4's wrap with mode 0, and on a skirmish recording every death was
   the recording machine's own. So the destructor's one call of 0x4B0A70 goes through
   dtor_killed: inside a replayed death of a unit whose death the recording machine made
   (tagpu_tx_replay_death's `own`), the query 0x4B0BC0 the owner's Send_UnitDeath ran, with the
   severity the death record carries; everywhere else the start, unchanged. That severity is the
   query's value after the call: Send_UnitDeath reads it back at 0x4865E9, after the query wrote
   its arguments back (0x4B0CE6..0x4B0D11), and writes it as the 0x0C's +9 (0x4865F4). So the
   playing's query starts from the recording machine's answer, which is the value it started from
   unless the script assigns its first parameter, and an answer of 0 or below makes no Killed at
   all on the playing (0x48685A's jle) [INFERRED: no stock Killed seen to assign it]. The corpse
   argument starts at 0: Send_UnitDeath passes a local it writes only in its mode-7 path
   (0x48652C), which the query then skips (0x486528), so a Killed that reads it before writing it
   reads stack contents on the recording machine [DISASSEMBLED 0x4864B0..0x4865C8]. Nothing after
   the call relies on what it leaves in eax, ecx or edx: the paths that skip it (0x486853,
   0x48685A) join it at 0x48687C with unrelated values there, though the stock call returns with
   ecx the COB object (its pop ecx).
   CLASS: in the fail-closed table: a playing without it would run every dying unit's waiting
   threads. */
typedef int (__thiscall* PFN_StartArgs)(char* cob, const char* name, void* cb, int now, int argc,
                                        int a0, int a1, int a2, int a3);
#define E_StartArgs ((PFN_StartArgs)0x004B0A70u)
typedef int (__thiscall* PFN_QueryName)(char* cob, const char* name, int* a0, int* a1, int* a2,
                                        int* a3);
#define E_QueryName ((PFN_QueryName)0x004B0BC0u)

/* The running replayed death's: its Killed is the owner's query. THE INVARIANT, an ordering:
   tagpu_tx_replay_death sets it just before its one destructor call and clears it as that call
   returns, on the game thread, and no capture point and no frame visit runs inside a destructor
   call (B2's stub and the frame visit call the replay, never the reverse), so the flag is 0 at
   every capture point and every dump: TRANSIENT holds nothing a recording, a seek or a takeover
   could need. */
static unsigned char s_txKilledQuery;
SIM_VAR(s_txKilledQuery, SIM_TRANSIENT);

static int __thiscall dtor_killed(char* cob, const char* name, void* cb, int now, int argc, int a0,
                                  int a1, int a2, int a3)
{
    int severity = a0, corpse = 0;
    if (!s_txKilledQuery) return E_StartArgs(cob, name, cb, now, argc, a0, a1, a2, a3);
    return E_QueryName(cob, name, &severity, &corpse, NULL, NULL);
}

static int fix_killed_query(void)
{
    static const unsigned char call[5] = { 0xE8, 0xF4, 0xA1, 0x02, 0x00 };   /* call 0x4B0A70 */
    lim_branch(0x00486877, sizeof call, call, 0xE8, (unsigned int)(size_t)dtor_killed,
               "a replayed death's Killed: the owner's query");
    return FIX_TABLE;
}

SIM_FEATURE(flak_divides, SIM_PUPPET_RUNS, "battle");

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
static struct { volatile unsigned int fallbacks; } s_flakDiag;   /* peekable, GAME THREAD */
SIM_VAR(s_flakDiag, SIM_PRIVATE);

static void __cdecl flak_fallback(const unsigned char* slot)
{
    const char* w = *(const char* const*)(slot + 0x0C);
    unsigned int n = ++s_flakDiag.fallbacks;
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

SIM_FEATURE(repose_trig, SIM_PUPPET_RUNS, "battle");

/* THE REPOSE'S TRIGONOMETRY, ONCE A PIECE. [DISASSEMBLED] The piece transform 0x45B150 (callers
   0x45B12C, and itself at 0x45B219 for a piece's children) turns every vertex of a piece in place
   by the loop 0x45B179..0x45B1AA, one call of the turn 0x4B6CC0(v, v, turn) a vertex, from the
   last vertex down to the first. 0x4B6CC0 turns three pairs through 0x4B7173(angle, pair): (x, y)
   by turn[0], then (y, z) by turn[2], then (x, z) by turn[1]. 0x4B7173 does nothing for a zero
   angle (0x4B717B); otherwise it filds the pair, filds the int16 angle, multiplies it by the
   double at 0x509EF8, takes fsincos, and runs a fixed sequence of multiplies and adds to two
   fistp. So every vertex of a piece takes the same three fsincos of the same three arguments,
   which is four fifths of the repose's time (zoom-out-speed-plan.md, item 3).

   The posed vertices are simulation state: SweetSpot 0x43E0B0 takes the box of a piece's posed
   vertices as AutoAim's target point. So the loop is not skipped; it is made to give the same
   bits faster. THE FIX: the loop's first five bytes jump to a stub that calls repose_rotate(prim,
   turn) and resumes at 0x45B1AC, where stock leaves the loop. repose_rotate takes the triple's
   sines and cosines once and turns every vertex, last to first, through tagpu_turn3.h, which
   writes what 0x4B6CC0 writes, bit for bit (its header states the argument).

   THE INVARIANT: every vertex the loop writes is bit-identical to stock's, and so are the x87
   exception flags the loop itself raises, since the trig is taken only for a piece with a vertex,
   where stock takes it. (The rotation's table, fp_exact below, skips fsincos elsewhere, so the
   sticky flags of the process are not stock's; nothing reads them for a value.)
   Beyond tagpu_turn3.h's argument it rests on:
     - the engine code it stands for being stock's: the loop, 0x4B6CC0 and 0x4B7173 are each
       matched byte for byte before anything is written, and on any difference nothing is;
     - the vertices being the only memory the loop writes: the dead store of the counter into
       the frame's second argument (0x45B188, [esp+0x1C]) is not repeated, and that slot is read
       only at 0x45B155, before the loop, and dropped by 0x45B150's `ret 0xC`;
     - the status word's condition codes not being carried: every fnstsw in the game's code
       follows the fcom that sets them (the 314 in .text; the reads of exception flags,
       `and $0x20`, are the CRT's, at 0x4E5000 and above). The stack ends empty in both;
     - the registers at 0x45B1AC: stock leaves eax, ecx, edx and ebx as the loop's scratch, and
       0x45B1AC..0x45B1C9 reloads all four before reading them. */

/* THE ORACLE, `tagpu_reposecheck.on` (read at install): every vertex is also turned by stock's
   0x4B6CC0 on a copy, and the two compared. 0x4B6CC0's three calls of 0x4B7173 reach the rotation's
   table when fp_exact is installed, which tagpu_fpverify.on compares with the engine's fsincos. A difference is logged with its piece's angles. */
typedef void (__stdcall *TURN3_FN)(const int* in, int* out, const short* turn);
static unsigned char s_rtrigCheck;
SIM_VAR(s_rtrigCheck, SIM_INSTALL);
static struct { volatile unsigned int pieces, verts, differ; } s_rtrigDiag;   /* GAME THREAD */
SIM_VAR(s_rtrigDiag, SIM_PRIVATE);

/* The loop 0x45B179..0x45B1AA: prim's vertex count is (*prim)+4, its vertices at prim+0x22,
   12 bytes each; `turn` is the three int16 angles 0x45B150 was handed. The oracle's compare is
   written inline rather than as a function of its own: the replay gate asks every function of
   this file to be reached, and its games arm no oracle. */
static void __cdecl repose_rotate(const unsigned char* prim, const short* turn)
{
    const unsigned char* node;
    int* v;
    int n, i;
    TAGPU_TURN3 t;

    memcpy(&node, prim, 4);
    memcpy(&n, node + 4, 4);
    memcpy(&v, prim + 0x22, 4);
    if (n <= 0) return;                     /* stock's `dec; js` at 0x45B17E: no turn taken */
    tagpu_turn3_prepare(&t, turn);
    if (s_rtrigCheck) {
        unsigned int np = ++s_rtrigDiag.pieces;
        if (!(np & (np - 1u)))
            tagpu_logf("enginefix: repose check: %u pieces, %u vertices, %u differ", np,
                       s_rtrigDiag.verts, s_rtrigDiag.differ);
    }
    for (i = n - 1; i >= 0; i--) {
        int* p = v + 3 * i;
        int stock[3];
        if (s_rtrigCheck) {
            memcpy(stock, p, 12);
            ((TURN3_FN)0x004B6CC0)(stock, stock, turn);
        }
        tagpu_turn3_apply(&t, p, p);
        if (s_rtrigCheck) {
            s_rtrigDiag.verts++;
            if (memcmp(stock, p, 12) != 0) {
                unsigned int nd = ++s_rtrigDiag.differ;
                if (nd <= 8 || !(nd & (nd - 1u)))
                    tagpu_logf("enginefix: repose check: DIFFERS (%u so far) turn %d %d %d: stock "
                               "%d %d %d, ours %d %d %d", nd, turn[0], turn[1], turn[2], stock[0],
                               stock[1], stock[2], p[0], p[1], p[2]);
            }
        }
    }
}

/* 0x4B6CC0 and 0x4B7173 as stock has them: what repose_rotate reproduces */
static const unsigned char k_turn3[0x86] = {
    0x83,0xEC,0x10,0x56,0x8B,0x74,0x24,0x18,0x57,0x8B,0x7C,0x24,0x24,0x8B,0x06,0x8B,
    0x4E,0x04,0x89,0x44,0x24,0x08,0x66,0x8B,0x07,0x8D,0x54,0x24,0x08,0x89,0x4C,0x24,
    0x0C,0x52,0x50,0xE8,0x8B,0x04,0x00,0x00,0x8B,0x4C,0x24,0x14,0x8B,0x56,0x08,0x83,
    0xC4,0x08,0x89,0x4C,0x24,0x10,0x66,0x8B,0x4F,0x04,0x8D,0x44,0x24,0x10,0x50,0x51,
    0x89,0x54,0x24,0x1C,0xE8,0x6A,0x04,0x00,0x00,0x8B,0x74,0x24,0x28,0x8B,0x54,0x24,
    0x18,0x8B,0x44,0x24,0x1C,0x83,0xC4,0x08,0x89,0x56,0x04,0x66,0x8B,0x57,0x02,0x8D,
    0x4C,0x24,0x08,0x89,0x44,0x24,0x0C,0x51,0x52,0xE8,0x45,0x04,0x00,0x00,0x8B,0x44,
    0x24,0x10,0x8B,0x4C,0x24,0x14,0x83,0xC4,0x08,0x89,0x06,0x89,0x4E,0x08,0x5F,0x5E,
    0x83,0xC4,0x10,0xC2,0x0C,0x00 };
static const unsigned char k_turn2[0x34] = {
    0x55,0x8B,0xEC,0x66,0x83,0x7D,0x08,0x00,0x74,0x28,0x8B,0x45,0x0C,0xDB,0x40,0x04,
    0xDB,0x00,0xD9,0xC1,0xD9,0xC1,0xDF,0x45,0x08,0xDC,0x0D,0xF8,0x9E,0x50,0x00,0xD9,
    0xFB,0xDC,0xCD,0xDE,0xCC,0xDC,0xCA,0xDE,0xC9,0xDE,0xC3,0xDE,0xE9,0xDB,0x18,0xDB,
    0x58,0x04,0xC9,0xC3 };

static int fix_repose_trig(void)
{
    FIXSITE site[3] = {
        { 0x0045B179, 20, { 0x8B,0x0E,0x8B,0x41,0x04,0x48,0x78,0x2B,0x8D,0x1C,0x40,0xC1,0xE3,0x02,
                            0x40,0x89,0x44,0x24,0x1C,0x8B }, { 0 } },
        { 0x0045B18D, 20, { 0x56,0x22,0x8B,0x4C,0x24,0x18,0x51,0x8D,0x04,0x1A,0x50,0x50,0xE8,0x22,
                            0xBB,0x05,0x00,0x83,0xEB,0x0C }, { 0 } },
        { 0x0045B1A1, 11, { 0x8B,0x44,0x24,0x1C,0x48,0x89,0x44,0x24,0x1C,0x75,0xE0 }, { 0 } },
    };
    unsigned char* p;
    if (!fix_match(site, 3) ||
        memcmp((const void*)0x004B6CC0, k_turn3, sizeof k_turn3) != 0 ||
        memcmp((const void*)0x004B7173, k_turn2, sizeof k_turn2) != 0) return FIX_BYTES;
    if (!(p = fix_code(24))) return FIX_STUB;
    s_rtrigCheck = (unsigned char)(tagpu_sim_lever("tagpu_reposecheck.on") != 0);
    fix_branch(&site[0], 0xE9, p);
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x18;          /* mov eax,[esp+0x18]: turn */
    *p++ = 0x50;                                                /* push eax            */
    *p++ = 0x56;                                                /* push esi: the prim  */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)repose_rotate); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                      /* add esp,8           */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0045B1AC);
    return fix_write(site, 1);
}

SIM_FEATURE(shatter_rng, SIM_PUPPET_RUNS, "battle");

/* THE SHATTER'S RANDOM NUMBERS, a SIMULATION fix. COB's EXPLODE with SHATTER (0x481140, reached
   only through its table entry 0x4FD6CC; a death shatters through its Killed script) breaks a piece
   through 0x421620 (0x48123C; the other call, 0x420F14, is in dead code) into 0x421700, which leaves the whole call when the explosion count
   main+0x1491B is at its cap (0x42176F, 0x12C; the raised build's 3000 at the same operand) or no
   debris record is free (0x4217EE..0x421808), and only past both draws the simulation RNG 0x4B6C30
   (state 0x51FC88) eight times for each eligible primitive (0x421851, 0x421868, 0x42187F, 0x4218AA,
   0x4218BE, 0x4218D2, 0x421B39, 0x421B62). How full the effect pools are thus decided how many
   numbers the simulation drew. The pools are not recorded (demo-recorder-completeness.md), and a
   seek empties them, so a takeover, a fork or a rewind went on with a different RNG stream than
   the game it continues. The draws feed only the piece's explosion record (velocity +0x34..+0x3C,
   spin +0x40..+0x48); nothing the simulation reads takes them (DISASSEMBLED 2026-11-15: every one
   of the 129 calls of 0x4B6C30, every reference into the three pools).
   THE FIX: the eight calls draw from shatter_rand instead, the same contract (stdcall, n on the
   stack, `ret 4`, 0 for n < 2, else a value in [0, n)), so the simulation's stream no longer
   depends on the pools by construction. The state is recorded (SIM_STATE, four bytes that change
   only at a shatter) and written back at every step, so a playing's pieces match the recording's
   while its pools are as full as the recording's were (after a seek they are empty, and the
   puppet draws where the live game had no room, until the next write-back). It is opaque: an RNG
   state is no address, and the scan for addresses must not read it as one. THE ORDERING: COB runs
   on the game thread in the step, except a Create's first slice, which the create runs at once
   (0x485DE6) and which the loader's start-unit calls (0x497963, 0x497ACD) make on the LOADER thread;
   those finish before the load-done gate lets the game thread step, so the two never draw at once.
   UNDER THE HARNESS'S SWITCH (A HARNESS'S RANDOM STREAMS) the stream is put back at its start at
   every level load (test_rng_seed) and at a run's clearing apply (tagpu_seed_run), as the other
   streams are re-seeded there, so a harness's level and a calibration run start it alike. */
#define SHATTER_RNG_START 0x2545F491u
static unsigned int s_shatterRng = SHATTER_RNG_START;
static const SimField shatter_rng_layout[] = { SIM_FIELD_AT(0, 4, SIMF_OPAQUE, SIM_ID_NONE),
                                               SIM_FIELD_END };
SIM_VAR_L(s_shatterRng, SIM_STATE, shatter_rng_layout);

static unsigned int __stdcall shatter_rand(int n)
{
    if (n < 2) return 0;
    s_shatterRng = s_shatterRng * 1664525u + 1013904223u;
    return (s_shatterRng >> 8) % (unsigned int)n;
}

static int fix_shatter_rng(void)
{
    static const unsigned int site[8] = { 0x00421851u, 0x00421868u, 0x0042187Fu, 0x004218AAu,
                                          0x004218BEu, 0x004218D2u, 0x00421B39u, 0x00421B62u };
    int k;
    for (k = 0; k < 8; k++) {
        unsigned char stock[5];
        unsigned int rel = 0x004B6C30u - (site[k] + 5u);
        stock[0] = 0xE8; memcpy(stock + 1, &rel, 4);
        lim_branch(site[k], 5, stock, 0xE8, (unsigned int)(size_t)shatter_rand,
                   "the shatter draws from its own generator");
    }
    return FIX_TABLE;
}

SIM_FEATURE(last_cell, SIM_PUPPET_RUNS, "battle");

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

SIM_FEATURE(los_shear, SIM_PUPPET_RUNS, "battle");

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
       compaction (main+0x1427B, at most the cap's records of 36 bytes -- 20, or a test launch's
       tagpu_sightcap.on N -- each holding its own words and byte),
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
   THE ORACLE: every stub counts, per function, the times it takes the own row (s_losDiag.ownRow) --
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
static struct { volatile LONG ownRow[LOS_NFN]; } s_losDiag;
SIM_VAR(s_losDiag, SIM_PRIVATE);

/* pushfd; lock inc dword [&s_losDiag.ownRow[fn]]; popfd -- the count, with the flags kept. The hand
   stubs' literal rel8s (stubAB .. stubO, the projectile pass's stub) jump over it, so they are
   written for this length, and a different one fails the build here. */
#define LOS_COUNT_LEN 9
typedef char los_count_len_is_the_hand_stubs[(LOS_COUNT_LEN == 9) ? 1 : -1];
static int los_count(unsigned char* a, int fn)
{
    unsigned int at = (unsigned int)(size_t)&s_losDiag.ownRow[fn];
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

SIM_FEATURE(projectile_view, SIM_PUPPET_RUNS, "battle");

/* THE PROJECTILE PASS'S VIEW [DISASSEMBLED]. The projectile draw pass 0x49BE60 (called at
   0x469B22 in DrawGameScreen) asks, for each projectile, whether the local player (main+0x2A43)
   sees it, with the same altitude-sheared row as UnitInPlayerLOS: inline under True line of
   sight (0x49BEE8..0x49BF0F, ebp the point, eax the player; read 0x49BF10, not visible
   0x49BF29), through PositionInPlayerMapped 0x408090 otherwise (0x49BF2F, fixed with the
   shear above). A projectile high up near the north edge is not drawn, and a model projectile
   is not posed (0x49C127). The pass runs under the tagpu_gdi.on oracle; a Vulkan launch removes
   its call (tagpu_fxown.c), so there the fix is installed and never reached.
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

SIM_FEATURE(los_local, SIM_PUPPET_RUNS, "battle");

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

SIM_FEATURE(weapon_keys, SIM_PUPPET_RUNS, "battle");

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
     slot past 2) and filters the verdict by the slot's weapon. It is the
     entry's one owner: the extra-weapons module does not hook it, and asks its own
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
SIM_VAR(s_wkCheckStock, SIM_INSTALL);

static int __stdcall wk_check(char* u, char* t, unsigned int idx)
{
    unsigned int i = idx & 0xFF;
    const char* w = NULL;
    int v;
    if (i >= 3) {
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

SIM_FEATURE(veterancy, SIM_PUPPET_RUNS, "battle");

/* ===== VETERANCY (C3) ======================================================================
   Landing C3 of research/notes/tadr-port/data-keys.md; the disassembly is in
   data-keys-evidence.md, Part 3, and exe-reverse-engineering.md. TADR's VeterancyHack is the
   prior art; the design is ours. The keys' records, the levels and every bound are in
   tagpu_datakeys.c; these are the seven sites that ask, all in the fail-closed table, in both
   builds (CLASS: simulation -- a player whose build lacked them would play stock veterancy for
   a keyed type). Each runs the engine's own instructions when the unit's type carries no key,
   so stock content runs stock's bytes. The answers come from the unit-key reader, which is
   installed here, before the table, and without which the table refuses to install.

   Every stub asks with every register kept: pushad; push <the unit>; call <the answer>; the
   answer stored in the pushad frame's slot the continuation reads; cmp eax,-1; popad (which
   leaves the cmp's flags); je <stock: the stolen instructions, then on>.

   - 0x489BFA, damage taken (`mov cx,[esi+0xB8]`, esi the victim): edx = L, on at 0x489C16,
     (25 - L) * 4 % of edi. edx is dead at the site: 0x489C01's imul writes it.
   - 0x499DB5, damage dealt (`mov cx,[ebx+0xB8]`, ebx the shooter): edx = L, on at 0x499DD1,
     (100 + 6L) % of esi. edx is dead: 0x499DBC's imul writes it.
   - 0x49E468, a slot's reload (`xor ecx,ecx; mov eax,0x66666667`, edi the unit): ecx = L, on
     at 0x49E48D. ecx is dead: the stolen `xor` writes it.
   - 0x48A324, target lead (`cmp word [edi+0xB8],5; jbe 0x48A42D`): on at 0x48A332 or
     0x48A42D by the answer; neither reads the flags.
   - 0x49D6EA, the fire method's spread (`mov eax,0x2AAAAAAB`, edi the unit, ecx the spread):
     ebx = the divisor and ecx += 0x800 as stock adds it, on at 0x49D702, its `cmp ebx,1`.
     ebx is dead: stock writes it at 0x49D700.
   - 0x4043D8, the capture's cost (`mov cx,[edx+0xB8]`, edx the target): eax = 10 + L,
     on at 0x4043EC, `imul eax,edi`. The stock path keeps eax, 0x66666667.
   - 0x43869D, a unit reclaim's step (`mov cx,[eax+0xB8]`, eax the reclaimer, dx its workertime
     from 0x438694, esi the target's def from 0x438658, the ticks between steps at [esp+0x1C],
     the argument 0x4386C3 multiplies by): the answer takes all four, to hold the factor to
     what the 32-bit product holds; edi = the workertime, edx = the factor, on at 0x4386B9,
     stock's `imul edi,edx` or B10's jump (fix_reclaim_wrap), which reads the same two;
     nothing from there reads eax or ecx before writing them.
   No branch lands inside any of the seven (rel8/rel32 scan of .text and every dword of the
   image). */
static unsigned char* vet_call(unsigned char* p, unsigned char unit_push, const void* fn)
{
    *p++ = 0x60;                                                    /* pushad             */
    *p++ = unit_push;                                               /* push <the unit>    */
    return wk_jmp(p, 0xE8, (unsigned int)(size_t)fn);               /* call fn            */
}
static unsigned char* vet_stock(unsigned char* p, unsigned char slot)
{
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = slot;             /* mov [esp+slot],eax */
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0xFF;                          /* cmp eax,-1         */
    *p++ = 0x61;                                                    /* popad              */
    return p;
}

static int fix_veterancy(void)
{
    static const unsigned char taken[7]  = { 0x66, 0x8B, 0x8E, 0xB8, 0x00, 0x00, 0x00 };
    static const unsigned char dealt[7]  = { 0x66, 0x8B, 0x8B, 0xB8, 0x00, 0x00, 0x00 };
    static const unsigned char reload[7] = { 0x33, 0xC9, 0xB8, 0x67, 0x66, 0x66, 0x66 };
    static const unsigned char lead[14]  = { 0x66, 0x83, 0xBF, 0xB8, 0x00, 0x00, 0x00, 0x05,
                                             0x0F, 0x86, 0xFB, 0x00, 0x00, 0x00 };
    static const unsigned char aim[5]    = { 0xB8, 0xAB, 0xAA, 0xAA, 0x2A };
    static const unsigned char cost[7]   = { 0x66, 0x8B, 0x8A, 0xB8, 0x00, 0x00, 0x00 };
    static const unsigned char rcl[7]    = { 0x66, 0x8B, 0x88, 0xB8, 0x00, 0x00, 0x00 };
    unsigned char* ct = fix_code(48);
    unsigned char* cd = fix_code(48);
    unsigned char* cr = fix_code(48);
    unsigned char* cl = fix_code(64);
    unsigned char* ca = fix_code(48);
    unsigned char* ck = fix_code(48);
    unsigned char* cm = fix_code(64);
    unsigned char* p;
    unsigned char* j;
    unsigned char now[16];

    if (!tagpu_datakeys_units_install()) {
        lim_needs("veterancy (a simulation fix)", "the unit-key reader", "not installed");
        return FIX_TABLE;
    }
    if (!ct || !cd || !cr || !cl || !ca || !ck || !cm) { lim_no_stub(); return FIX_TABLE; }

    /* 0x489BFA: edx = L -> 0x489C16 */
    p = vet_stock(vet_call(ct, 0x56, (const void*)tagpu_datakeys_vet_taken), 0x14);
    *p++ = 0x74; *p++ = 0x05;                                       /* je stock           */
    p = wk_jmp(p, 0xE9, 0x00489C16u);
    memcpy(p, taken, 7); p += 7;
    wk_jmp(p, 0xE9, 0x00489C01u);

    /* 0x499DB5: edx = L -> 0x499DD1 */
    p = vet_stock(vet_call(cd, 0x53, (const void*)tagpu_datakeys_vet_dealt), 0x14);
    *p++ = 0x74; *p++ = 0x05;
    p = wk_jmp(p, 0xE9, 0x00499DD1u);
    memcpy(p, dealt, 7); p += 7;
    wk_jmp(p, 0xE9, 0x00499DBCu);

    /* 0x49E468: ecx = L -> 0x49E48D */
    p = vet_stock(vet_call(cr, 0x57, (const void*)tagpu_datakeys_vet_reload), 0x18);
    *p++ = 0x74; *p++ = 0x05;
    p = wk_jmp(p, 0xE9, 0x0049E48Du);
    memcpy(p, reload, 7); p += 7;
    wk_jmp(p, 0xE9, 0x0049E46Fu);

    /* 0x48A324: pushad; push edi; call lead; cmp eax,0; popad; jl stock; je off;
       jmp 0x48A332; off: jmp 0x48A42D; stock: cmp word [edi+0xB8],5; jbe 0x48A42D;
       jmp 0x48A332 */
    p = vet_call(cl, 0x57, (const void*)tagpu_datakeys_vet_lead);
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x00;                          /* cmp eax,0          */
    *p++ = 0x61;                                                    /* popad              */
    *p++ = 0x7C; *p++ = 0x0C;                                       /* jl stock           */
    *p++ = 0x74; *p++ = 0x05;                                       /* je off             */
    p = wk_jmp(p, 0xE9, 0x0048A332u);
    p = wk_jmp(p, 0xE9, 0x0048A42Du);
    memcpy(p, lead, 8); p += 8;
    p = wk_jcc(p, 0x86, 0x0048A42Du);
    wk_jmp(p, 0xE9, 0x0048A332u);

    /* 0x49D6EA: ebx = the divisor; add ecx,0x800 -> 0x49D702 */
    p = vet_stock(vet_call(ca, 0x57, (const void*)tagpu_datakeys_vet_accuracy), 0x10);
    *p++ = 0x74; *p++ = 0x0B;
    *p++ = 0x81; *p++ = 0xC1; *p++ = 0x00; *p++ = 0x08; *p++ = 0x00; *p++ = 0x00;
    p = wk_jmp(p, 0xE9, 0x0049D702u);
    memcpy(p, aim, 5); p += 5;
    wk_jmp(p, 0xE9, 0x0049D6EFu);

    /* 0x4043D8: pushad; push edx; call cost; cmp eax,-1; je stock; mov [esp+0x1C],eax;
       popad; jmp 0x4043EC; stock: popad; mov cx,[edx+0xB8]; jmp 0x4043DF */
    p = vet_call(ck, 0x52, (const void*)tagpu_datakeys_vet_capture_cost);
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0xFF;
    *p++ = 0x74; j = p++;
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x1C;
    *p++ = 0x61;
    p = wk_jmp(p, 0xE9, 0x004043ECu);
    *j = (unsigned char)(p - (j + 1));
    *p++ = 0x61;
    memcpy(p, cost, 7); p += 7;
    wk_jmp(p, 0xE9, 0x004043DFu);

    /* The x87 stack is live here: 0x438672/0x43867A load the target's cost, which 0x4386D0
       uses after the stub, so tagpu_datakeys_vet_reclaim_step and all it calls stay free of
       x87 and MMX code.
       0x43869D: pushad; push [esp+0x3C] (the ticks); push [esi+0x1FA] (the target's
       MaxHitPoints, which 0x4386BC reads next); push edx; push eax; call reclaim;
       cmp eax,-1; je stock; mov [esp+0x1C],eax; popad; mov edi,edx; mov edx,eax;
       jmp 0x4386B9; stock: popad; mov cx,[eax+0xB8]; jmp 0x4386A4 */
    p = cm;
    *p++ = 0x60;                                                    /* pushad             */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x3C;             /* push [esp+0x3C]    */
    *p++ = 0xFF; *p++ = 0xB6; *p++ = 0xFA; *p++ = 0x01; *p++ = 0x00; *p++ = 0x00; /* push [esi+0x1FA] */
    *p++ = 0x52;                                                    /* push edx           */
    *p++ = 0x50;                                                    /* push eax           */
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)tagpu_datakeys_vet_reclaim_step);
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0xFF;
    *p++ = 0x74; j = p++;
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x1C;
    *p++ = 0x61;
    *p++ = 0x8B; *p++ = 0xFA;                                       /* mov edi,edx        */
    *p++ = 0x8B; *p++ = 0xD0;                                       /* mov edx,eax        */
    p = wk_jmp(p, 0xE9, 0x004386B9u);
    *j = (unsigned char)(p - (j + 1));
    *p++ = 0x61;
    memcpy(p, rcl, 7); p += 7;
    wk_jmp(p, 0xE9, 0x004386A4u);

#define VET_SITE(va, n, stock, target, name) do { \
        unsigned int rel_ = (unsigned int)(size_t)(target) - ((va) + 5u); \
        memset(now, 0x90, sizeof now); now[0] = 0xE9; memcpy(now + 1, &rel_, 4); \
        lim_add((va), (n), (stock), now, (name)); } while (0)
    VET_SITE(0x00489BFAu, 7,  taken,  ct, "veterancy: the damage taken");
    VET_SITE(0x00499DB5u, 7,  dealt,  cd, "veterancy: the damage dealt");
    VET_SITE(0x0049E468u, 7,  reload, cr, "veterancy: a slot's reload");
    VET_SITE(0x0048A324u, 14, lead,   cl, "veterancy: target lead");
    VET_SITE(0x0049D6EAu, 5,  aim,    ca, "veterancy: the spread");
    VET_SITE(0x004043D8u, 7,  cost,   ck, "veterancy: the capture's cost");
    VET_SITE(0x0043869Du, 7,  rcl,    cm, "veterancy: a unit reclaim's step");
#undef VET_SITE
    return FIX_TABLE;
}

SIM_FEATURE(extra_weapons, SIM_PUPPET_RUNS, "battle");

/* ===== THE EXTRA WEAPONS ==================================================================
   The 1..N weapons module (tagpu_weapons.c; research/notes/extra-weapons.md), always on.
   CLASS: simulation, both builds -- a build without it plays three weapons a unit where this
   one plays the FBI's Weapon4..N, so its 19 entry hooks, 21 splices and 4 byte patches are
   sites of this table, compared with the exe file and written with the rest or not at all.
   The module makes its own trampolines and stubs, and its two record observers, on the
   unit-data load 0x42D2E0 and the FBI loader 0x42BF40, land here first through the shared
   primitive, chained onto the unit-key reader's (fix_veterancy, above): the module's records
   must never outlive their load, so without the observers the table refuses. The weapon keys'
   0x49ABB0 (wk_check) and the 0x0D receiver (wpn_rx_fired) call into the module for a slot
   past 2; they are sites of this table too, so they are installed exactly when it is. */
static int fix_extra_weapons(void)
{
    static TagpuWpnSite s[TAGPU_WPN_SITES];
    SIM_VAR(s, SIM_INSTALL);
    int n = tagpu_weapons_sites(s), k;
    if (n == TAGPU_WPN_OBSERVERS) {
        lim_needs("extra weapons (a simulation change)",
                  "the record observer on the unit-data load and the FBI loader",
                  "not installed: an entry's bytes differ");
        return FIX_TABLE;
    }
    if (n != TAGPU_WPN_SITES) { lim_no_stub(); return FIX_TABLE; }
    for (k = 0; k < n; k++) lim_add(s[k].va, s[k].n, s[k].stock, s[k].ours, s[k].name);
    return FIX_TABLE;
}

SIM_FEATURE(reclaim_wrap, SIM_PUPPET_RUNS, "c3-reclaim-bound");

/* ===== B10: A UNIT RECLAIM'S STEP, ITS PRODUCT IN 64 BITS ====================================
   Section B's tenth item (research/notes/tadr-port/sim-fixes.md, B10).

   THE DEFECT [DISASSEMBLED]. 0x438650(reclaimer, target, ticks), ret 0xC, called by the reclaim
   order 0x40483D and the build order's reclaim 0x414C86 (both push 0xF), sets the HP a unit
   reclaim takes from its target every 15 ticks ([order+0x36], dealt as a kind-5 hit through
   0x489BB0 at 0x404981 and 0x414B8D):
       st0 = max(the target's def +0x18A, 10.0 at 0x4FD2A4)              (0x43865F..0x438680)
       edi = workertime (def +0x1FE, u16) * (kills + 5)/5 * the target's MaxHitPoints
             (def +0x1FA) * ticks, in 32 bits           (0x4386B9 0x4386BC 0x4386C3; C3's
             keyed factor arrives at 0x4386B9 in edx, as stock's does)
       step = _ftol(edi, read as a qword with a zero high dword at 0x4386CC,
                    / (st0 * 300.0 at 0x4FD2A8)), at least 1                   (0x4386D0..0x4386E9)
   so the product wraps past 2^32 - 1: ARMCOM's 300 on a CORKROG wraps at stock's factor 32, 155
   kills, and takes a sliver of its step. And a quotient past 2^31 - 1 would come back from
   _ftol 0x4E43A0 (fistp qword, chop) as its low dword, which `cmp eax,1; jg` turns into 1 when
   negative. The only product that takes a def's +0x1FA (0x46A4CC's mul is the health bar's
   divide by 3); the other ten workertime reads divide it by 30 (0x88888889), and 0x42B673
   copies it.

   THE FIX. The product is computed exactly as an unsigned 64-bit value, saturated at INT64_MAX so
   that `fild qword` reads it positive, into the same qword (0x4386B9..0x4386CF: the three
   imuls, the store and the fild, replaced; the stub runs the fild and goes on at 0x4386D0, so
   the engine's own fxch, fmul and fdivp run unchanged). And the quotient is clamped to
   2147483647.0 before _ftol (0x4386D8..0x4386DE: the fdivp and the call, replaced; the stub runs
   the same fdivp and call and goes on at 0x4386DF).

   THE INVARIANT. For every product below 2^32 the step is bit-identical to stock's: the qword
   holds the same value with a zero high dword, the x87 operations are the engine's in its own
   order under its own control word, and the quotient is below 2^32 / 3000, so the clamp never
   engages. Above, the step is the formula's value, bounded by the saturation and the clamp;
   B7's saturation of a hit's word (0x489C71) then bounds what it does to the target.

   CLASS: simulation, fail closed, both builds (the reclaimer's peer computes the step and the
   target's HP carries it). Silent at run time. C3's keyed stub at 0x43869D is untouched: its
   factor arrives in edx, held to what a 32-bit product holds, which caps a keyed veteran's step
   below its formula (sim-fixes.md, B10, open for the owner). */

/* x87-free, and so is everything it calls: 0x4386B9's caller holds the target's cost in st0. */
static void __stdcall b10_reclaim_product(unsigned int workertime, unsigned int factor,
                                          unsigned int maxhp, unsigned int ticks,
                                          unsigned int* qword)
{
    unsigned long long a = (unsigned long long)workertime * factor;
    unsigned long long b = (unsigned long long)maxhp * ticks;
    unsigned long long p;
    if (__builtin_mul_overflow(a, b, &p) || p > 0x7FFFFFFFFFFFFFFFull)
        p = 0x7FFFFFFFFFFFFFFFull;
    qword[0] = (unsigned int)p;
    qword[1] = (unsigned int)(p >> 32);
}

static const double s_b10_cap = 2147483647.0;

static int fix_reclaim_wrap(void)
{
    static const unsigned char prod[23] = {
        0x0F, 0xAF, 0xFA,                                   /* imul edi,edx               */
        0x0F, 0xAF, 0xBE, 0xFA, 0x01, 0x00, 0x00,           /* imul edi,[esi+0x1FA]       */
        0x0F, 0xAF, 0x7C, 0x24, 0x1C,                       /* imul edi,[esp+0x1C]        */
        0x89, 0x7C, 0x24, 0x08,                             /* mov [esp+8],edi            */
        0xDF, 0x6C, 0x24, 0x08 };                           /* fild qword [esp+8]         */
    static const unsigned char quot[7] = {
        0xDE, 0xF9,                                         /* fdivp st(1),st             */
        0xE8, 0xC1, 0xBC, 0x0A, 0x00 };                     /* call 0x4E43A0              */
    unsigned char* cp = fix_code(48);
    unsigned char* cq = fix_code(48);
    unsigned char *p, *j;
    unsigned int cap = (unsigned int)(size_t)&s_b10_cap, rel;
    unsigned char now[23];

    if (!cp || !cq) { lim_no_stub(); return FIX_TABLE; }

    /* pushad; lea eax,[esp+0x28] (the qword, [esp+8] before the pushad); push eax;
       push [esp+0x40] (the ticks, [esp+0x1C]); push [esi+0x1FA]; push edx (the factor);
       push edi (the workertime); call b10_reclaim_product; popad; fild qword [esp+8];
       jmp 0x4386D0 */
    p = cp;
    *p++ = 0x60;
    *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x28;
    *p++ = 0x50;
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x40;
    *p++ = 0xFF; *p++ = 0xB6; *p++ = 0xFA; *p++ = 0x01; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x52;
    *p++ = 0x57;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)b10_reclaim_product);
    *p++ = 0x61;
    *p++ = 0xDF; *p++ = 0x6C; *p++ = 0x24; *p++ = 0x08;
    wk_jmp(p, 0xE9, 0x004386D0u);

    /* fdivp; fld qword [cap]; fcomp st(1); fnstsw ax; sahf; jae keep (cap >= the quotient);
       fstp st(0); fld qword [cap]; keep: call 0x4E43A0; jmp 0x4386DF. eax and the flags are
       dead here: _ftol writes eax, and 0x4386DF's cmp the flags. */
    p = cq;
    *p++ = 0xDE; *p++ = 0xF9;
    *p++ = 0xDD; *p++ = 0x05; memcpy(p, &cap, 4); p += 4;
    *p++ = 0xD8; *p++ = 0xD9;
    *p++ = 0xDF; *p++ = 0xE0;
    *p++ = 0x9E;
    *p++ = 0x73; j = p++;
    *p++ = 0xDD; *p++ = 0xD8;
    *p++ = 0xDD; *p++ = 0x05; memcpy(p, &cap, 4); p += 4;
    *j = (unsigned char)(p - (j + 1));
    p = wk_jmp(p, 0xE8, 0x004E43A0u);
    wk_jmp(p, 0xE9, 0x004386DFu);

    memset(now, 0x90, sizeof now);
    now[0] = 0xE9;
    rel = (unsigned int)(size_t)cp - (0x004386B9u + 5u); memcpy(now + 1, &rel, 4);
    lim_add(0x004386B9u, 23, prod, now, "B10: a unit reclaim's step, its product in 64 bits");
    memset(now, 0x90, sizeof now);
    now[0] = 0xE9;
    rel = (unsigned int)(size_t)cq - (0x004386D8u + 5u); memcpy(now + 1, &rel, 4);
    lim_add(0x004386D8u, 7, quot, now, "B10: a unit reclaim's step, its quotient clamped");
    return FIX_TABLE;
}

SIM_FEATURE(b7_hits, SIM_PUPPET_RUNS, "b7-radar-hail b7-word-outright");

/* ===== B7: A HIT'S WORD, KILL-OUTRIGHT, THE RADAR'S OWNER TEST, ONE METEOR ONE HIT ==========
   Section B's seventh item (research/notes/tadr-port/sim-fixes.md, B7), four stock defects.

   THE HIT'S WORD [DISASSEMBLED]. 0x489BB0 computes a hit's amount as an int and stores it in the
   9-byte record as a WORD (0x489C71); the damage path subtracts that word from the HP word and
   tests the result as signed (0x489EB5, 0x489EC4), and the paralyser (kind 2, 0x489DFD) and the
   heal (kind 0xA, 0x489D6E) read it unsigned. So an amount past the word's range wraps: 39 000
   on a 3 000-HP unit leaves it 29 536. Retail play reaches it with the disintegrator, 30 000
   raised by its commander's own level (0x499DB5): from level 2, 33 600 [MEASURED 2026-09-26: a
   commander with 25 kills, 39 000 a hit, left a 1 329-HP storage 26 536 HP up a hit, dying only
   when a second hit wrapped the HP word itself]. THE FIX, at the store: the amount is saturated into the
   range its reader reads -- -32768..32767 for the subtraction, 0..65535 for kinds 2 and 0xA --
   so any hit of 32 767 or more kills any unit whose HP the signed word can hold. Identity for
   every amount inside the range. CLASS: simulation (the firer's peer computes it, and the
   record carries it to the victim's owner).

   KILL-OUTRIGHT [DISASSEMBLED]. The engine's "kill outright" is an amount of 30 000 or more
   (kind 3 from self-destruct and player defeat 0x402147 0x486F94, 3 or 6 from a dying
   transport's cargo 0x48680B, kinds 4 and 9 from 0x402701 0x41BC49 0x4886A4 0x4887D0); the armour reduction exempts it (0x489BD1, `cmp edi,0x7530; jge`), and the
   veterancy reduction at 0x489BF3 does not, so a veteran above 24 000 HP survives it (stock's
   level 5 leaves 80 %). THE FIX: a call whose amount is 30 000 or more skips the veterancy
   reduction as it skips the armour one, by the same test of the caller's own amount. That
   amount includes the disintegrators' hits (30 000, or more from a veteran commander), so a
   D-gun kills a veteran Krogoth (29 918 HP), which stock's reduction let live from 5 kills. A
   veteran killed outright then reads the full overkill in its death's severity (0x48655E), as a
   recruit does. CLASS: simulation.

   THE RADAR'S OWNER TEST [DISASSEMBLED]. The radar rebuild's marker branch for a targetable or
   interceptor projectile reads its attacker's owner when the point is out of the local player's
   sight (0x4673B1, `mov ecx,[ebx+0x48]` = proj+0x52; `cmp [ecx+0xFF],al`), and a meteor has no
   attacker. THE FIX: no attacker is not the local player's, the answer the test gives any other
   player's projectile. CLASS: local (a fault on every peer's radar rebuild otherwise).

   ONE METEOR, ONE HIT [DISASSEMBLED + MEASURED]. Every peer runs its own shower (0x437DE0 from
   the sim tick 0x495594, no host test) and broadcasts each stone as a 0x0D (0x49DFF6); the
   receiver builds the stone again through its meteor branch (0x49D307). A projectile's damage
   is computed on a peer only when its owner record, main+0x1B63 + 331 * proj+0x66, is empty or
   not remote (0x49A01B..0x49A047) -- the firer's peer for every unit's shot -- and a stone's
   +0x66 is 10, a record no peer marks remote. So every peer computes every stone, on its own
   copy, and the victim's owner applies it once from its own computation and once more for each
   other peer's 0x0B. THE FIX: a stone is computed on the peer that spawned it, as a shot is on
   the peer that fired it. The mark is the stone's own +0x62, the firing piece (the answer of
   the shooter's QueryPrimary/Secondary/Tertiary script, 0x43E1E0), which 0x49C740 writes
   only for a projectile with a shooter (0x49C833) and whose one reader,
   0x49B7D6, sits behind a burst count that is 0 for a stone. Both places that build a
   shooterless projectile write it: the spawn (0x49DF7D) 0 and the receiver's meteor branch
   (0x49D307) PJ_RECEIVED. It lives in the record, so the pool's compaction (0x49AEE0) carries
   it with the stone, and no save carries projectiles. The damage gate sends a shooterless
   projectile of owner 10 marked received where stock sends a remote one's, 0x49A0AE: its
   explosion plays, its damage is left to its spawner, whose hit on another peer's unit travels
   as every hit does, in B4's tagged 0x05. CLASS: simulation. */
#define PJ_ORIGIN    0x62
#define PJ_RECEIVED  1

static struct { unsigned int word, outright, radar, copies; } s_b7Diag;
SIM_VAR(s_b7Diag, SIM_PRIVATE);

static void b7_event(unsigned int* n, const char* what)
{
    unsigned int c = ++*n;
    if ((c & (c - 1)) == 0) tagpu_logf("enginefix: %s (%u so far)", what, c);
}
static void __stdcall b7_word_event(void) { b7_event(&s_b7Diag.word, "a hit's amount saturated into its word"); }
static void __stdcall b7_radar_event(void) { b7_event(&s_b7Diag.radar, "the radar's owner test met a projectile with no attacker"); }
static void __stdcall b7_copy_event(void) { b7_event(&s_b7Diag.copies, "a received meteor's damage left to its spawner"); }
static void __stdcall b7_outright_event(const char* victim)
{
    if (tagpu_datakeys_vet_taken_level(victim))
        b7_event(&s_b7Diag.outright, "a kill-outright call on a veteran dealt its full amount");
}

typedef int (__stdcall *PFN_ProjInit)(char* proj, const char* weapon, const void* start,
                                      const void* target, int time, const char* attacker);
#define E_ProjInit ((PFN_ProjInit)0x0049C740u)

static int __stdcall meteor_spawned(char* p, const char* w, const void* s, const void* g, int t,
                                    const char* a)
{
    int r = E_ProjInit(p, w, s, g, t, a);
    *(unsigned short*)(p + PJ_ORIGIN) = 0;
    return r;
}
static int __stdcall meteor_received(char* p, const char* w, const void* s, const void* g, int t,
                                     const char* a)
{
    int r = E_ProjInit(p, w, s, g, t, a);
    *(unsigned short*)(p + PJ_ORIGIN) = PJ_RECEIVED;
    return r;
}

static int fix_hit_word(void)
{
    static const unsigned char store[5] = { 0x66, 0x89, 0x54, 0x24, 0x11 };  /* mov [esp+0x11],dx */
    unsigned char* c = fix_code(96);
    unsigned char *p, *jU1, *jU2, *jLo, *jSatA, *jSt1, *jSatB, *jUp, *jSatC, *jSt2;
    unsigned char now[5];
    unsigned int rel;
    if (!c) { lim_no_stub(); return FIX_TABLE; }
    p = c;
    *p++ = 0x83; *p++ = 0xFB; *p++ = 0x02; *p++ = 0x74; jU1 = p++;  /* cmp ebx,2; je unsig */
    *p++ = 0x83; *p++ = 0xFB; *p++ = 0x0A; *p++ = 0x74; jU2 = p++;  /* cmp ebx,0xA; je     */
    *p++ = 0x81; *p++ = 0xFA; *p++ = 0xFF; *p++ = 0x7F; *p++ = 0x00; *p++ = 0x00; /* cmp edx,0x7FFF */
    *p++ = 0x7E; jLo = p++;                                         /* jle low            */
    *p++ = 0xBA; *p++ = 0xFF; *p++ = 0x7F; *p++ = 0x00; *p++ = 0x00; /* mov edx,0x7FFF    */
    *p++ = 0xEB; jSatA = p++;                                       /* jmp sat            */
    *jLo = (unsigned char)(p - (jLo + 1));
    *p++ = 0x81; *p++ = 0xFA; *p++ = 0x00; *p++ = 0x80; *p++ = 0xFF; *p++ = 0xFF; /* cmp edx,-0x8000 */
    *p++ = 0x7D; jSt1 = p++;                                        /* jge store          */
    *p++ = 0xBA; *p++ = 0x00; *p++ = 0x80; *p++ = 0xFF; *p++ = 0xFF; /* mov edx,-0x8000   */
    *p++ = 0xEB; jSatB = p++;                                       /* jmp sat            */
    *jU1 = (unsigned char)(p - (jU1 + 1));
    *jU2 = (unsigned char)(p - (jU2 + 1));
    *p++ = 0x85; *p++ = 0xD2;                                       /* test edx,edx       */
    *p++ = 0x7D; jUp = p++;                                         /* jge up             */
    *p++ = 0x33; *p++ = 0xD2;                                       /* xor edx,edx        */
    *p++ = 0xEB; jSatC = p++;                                       /* jmp sat            */
    *jUp = (unsigned char)(p - (jUp + 1));
    *p++ = 0x81; *p++ = 0xFA; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0x00; *p++ = 0x00; /* cmp edx,0xFFFF */
    *p++ = 0x7E; jSt2 = p++;                                        /* jle store          */
    *p++ = 0xBA; *p++ = 0xFF; *p++ = 0xFF; *p++ = 0x00; *p++ = 0x00; /* mov edx,0xFFFF    */
    *jSatA = (unsigned char)(p - (jSatA + 1));
    *jSatB = (unsigned char)(p - (jSatB + 1));
    *jSatC = (unsigned char)(p - (jSatC + 1));
    *p++ = 0x60;                                                    /* sat: pushad        */
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)b7_word_event);
    *p++ = 0x61;                                                    /* popad              */
    *jSt1 = (unsigned char)(p - (jSt1 + 1));
    *jSt2 = (unsigned char)(p - (jSt2 + 1));
    memcpy(p, store, 5); p += 5;                                    /* store              */
    wk_jmp(p, 0xE9, 0x00489C76u);
    rel = (unsigned int)(size_t)c - (0x00489C71u + 5u);
    now[0] = 0xE9; memcpy(now + 1, &rel, 4);
    lim_add(0x00489C71u, 5, store, now, "B7: a hit's amount saturated into its word");
    return FIX_TABLE;
}

static int fix_kill_outright(void)
{
    static const unsigned char was[7] = { 0x33, 0xC9, 0xB8, 0x67, 0x66, 0x66, 0x66 };
    unsigned char* c = fix_code(48);
    unsigned char *p, *j;
    unsigned char now[7];
    unsigned int rel;
    if (!c) { lim_no_stub(); return FIX_TABLE; }
    p = c;
    *p++ = 0x81; *p++ = 0x7C; *p++ = 0x24; *p++ = 0x24;             /* cmp dword [esp+0x24], */
    *p++ = 0x30; *p++ = 0x75; *p++ = 0x00; *p++ = 0x00;             /*   30000: the call's own amount */
    *p++ = 0x7C; j = p++;                                           /* jl stock           */
    *p++ = 0x60; *p++ = 0x56;                                       /* pushad; push esi   */
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)b7_outright_event);
    *p++ = 0x61;                                                    /* popad              */
    *p++ = 0x8B; *p++ = 0xD7;                                       /* mov edx,edi        */
    p = wk_jmp(p, 0xE9, 0x00489C3Au);
    *j = (unsigned char)(p - (j + 1));
    memcpy(p, was, 7); p += 7;
    wk_jmp(p, 0xE9, 0x00489BFAu);
    rel = (unsigned int)(size_t)c - (0x00489BF3u + 5u);
    memset(now, 0x90, sizeof now); now[0] = 0xE9; memcpy(now + 1, &rel, 4);
    lim_add(0x00489BF3u, 7, was, now, "B7: kill-outright skips the veterancy reduction");
    return FIX_TABLE;
}

static int fix_meteor_once(void)
{
    static const unsigned char spawn[5] = { 0xE8, 0xBE, 0xE7, 0xFF, 0xFF };  /* call 0x49C740 */
    static const unsigned char recv[5]  = { 0xE8, 0x34, 0xF4, 0xFF, 0xFF };  /* call 0x49C740 */
    static const unsigned char gate[5]  = { 0x33, 0xC9, 0x8A, 0x4E, 0x66 };  /* xor ecx,ecx; mov cl,[esi+0x66] */
    unsigned char* c = fix_code(48);
    unsigned char *p, *j1, *j2, *j3;
    unsigned char now[5];
    unsigned int rel;
    if (!c) { lim_no_stub(); return FIX_TABLE; }
    /* 0x49A01B: cmp dword [esi+0x52],0; jne stock; cmp byte [esi+0x66],10; jne stock;
       cmp word [esi+0x62],PJ_RECEIVED; jne stock; pushad; call event; popad; jmp 0x49A0AE;
       stock: xor ecx,ecx; mov cl,[esi+0x66]; jmp 0x49A020 */
    p = c;
    *p++ = 0x83; *p++ = 0x7E; *p++ = 0x52; *p++ = 0x00; *p++ = 0x75; j1 = p++;
    *p++ = 0x80; *p++ = 0x7E; *p++ = 0x66; *p++ = 0x0A; *p++ = 0x75; j2 = p++;
    *p++ = 0x66; *p++ = 0x83; *p++ = 0x7E; *p++ = PJ_ORIGIN; *p++ = PJ_RECEIVED;
    *p++ = 0x75; j3 = p++;
    *p++ = 0x60;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)b7_copy_event);
    *p++ = 0x61;
    p = wk_jmp(p, 0xE9, 0x0049A0AEu);
    *j1 = (unsigned char)(p - (j1 + 1));
    *j2 = (unsigned char)(p - (j2 + 1));
    *j3 = (unsigned char)(p - (j3 + 1));
    memcpy(p, gate, 5); p += 5;
    wk_jmp(p, 0xE9, 0x0049A020u);
    rel = (unsigned int)(size_t)c - (0x0049A01Bu + 5u);
    now[0] = 0xE9; memcpy(now + 1, &rel, 4);
    lim_add(0x0049A01Bu, 5, gate, now, "B7: a received meteor's damage left to its spawner");
    rel = (unsigned int)(size_t)meteor_spawned - (0x0049DF7Du + 5u);
    now[0] = 0xE8; memcpy(now + 1, &rel, 4);
    lim_add(0x0049DF7Du, 5, spawn, now, "B7: a spawned meteor marked its spawner's");
    rel = (unsigned int)(size_t)meteor_received - (0x0049D307u + 5u);
    now[0] = 0xE8; memcpy(now + 1, &rel, 4);
    lim_add(0x0049D307u, 5, recv, now, "B7: a received meteor marked received");
    return FIX_TABLE;
}

/* CLASS: local. Without it the radar rebuild reads through NULL. */
static int fix_radar_owner(void)
{
    static const unsigned char was[11] = { 0x8B, 0x4B, 0x48,                     /* mov ecx,[ebx+0x48]   */
                                           0x38, 0x81, 0xFF, 0x00, 0x00, 0x00,   /* cmp [ecx+0xFF],al    */
                                           0x75, 0x4A };                          /* jne 0x467406         */
    unsigned char *s, *p, *j1, *j2;
    if (memcmp((const void*)0x004673B1, was, sizeof was) != 0) return FIX_BYTES;
    s = p = tagpu_detour_stub();
    if (!s) return FIX_STUB;
    *p++ = 0x8B; *p++ = 0x4B; *p++ = 0x48;                          /* mov ecx,[ebx+0x48] */
    *p++ = 0x85; *p++ = 0xC9;                                       /* test ecx,ecx       */
    *p++ = 0x74; j1 = p++;                                          /* jz none            */
    *p++ = 0x38; *p++ = 0x81; *p++ = 0xFF; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; /* cmp [ecx+0xFF],al */
    *p++ = 0x75; j2 = p++;                                          /* jne skip           */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x004673BC); p += 4;
    *j1 = (unsigned char)(p - (j1 + 1));
    *p++ = 0x60;                                                    /* none: pushad       */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)b7_radar_event); p += 4;
    *p++ = 0x61;                                                    /* popad              */
    *j2 = (unsigned char)(p - (j2 + 1));
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00467406);                   /* skip               */
    if (!tagpu_detour_land(0x004673B1, s, (int)sizeof was)) {
        tagpu_detour_stub_free(s);
        return FIX_PROTECT;
    }
    return FIX_ARMED;
}

SIM_FEATURE(stale_hits, SIM_PUPPET_RUNS, "battle");

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
#define HIT_TAG_DEATH   0x4C                /* the 0x0C with its victim's build fraction (B8)  */

/* B8's half of the envelope, in the KILL COUNTS section below: the 0x4A also carries its
   unit's build fraction and HP, and the 0x0C travels as the 0x4C */
static void kill_state_read(unsigned char* m, unsigned int idx);
static void kill_apply_state(char* slot, const unsigned char* rec);
static int kill_rx_death(unsigned int* regs, const char* ta, const unsigned char* m);
/* counters for the heartbeat's kills: section; the LONGs are written from both pumping threads
   during a load (the refused deaths) or from any thread that holds, the rest by the thread
   that runs the tick */
static struct {
    volatile LONG bare0C;
    unsigned int  out, in, badFrac, deadCopy, noSlot;
    unsigned int  same, turned, turnedBack, swept;
    unsigned int  createDone, createNano, createAbsent, createBad;
    unsigned int  drop12;
    volatile LONG refused, held, holdLate, holdFull, notes;
} s_killDiag;
SIM_VAR(s_killDiag, SIM_PRIVATE);
/* C4's destructor wrap, in the TRANSPORTED EXPLOSIONS section below: the 0x4C's destructor
   call goes through it */
static unsigned char* tx_wrap_code(void);
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
/* three columns of n entries in the table's own block, one entry a unit slot; `room` is the
   block this process allocated, so a restore never carries it into another block */
static const SimField hit_layout[] = {
    SIM_FIELD(struct hit_tab, n, SIMF_COUNT, SIM_ID_NONE),
    SIM_FIELD(struct hit_tab, room, SIMF_LOCAL, SIM_ID_NONE),
    SIM_COLUMN(struct hit_tab, stamp, SIM_ID_UNIT_SLOT),
    SIM_COLUMN(struct hit_tab, freed, SIM_ID_UNIT_SLOT),
    SIM_COLUMN(struct hit_tab, local, SIM_ID_UNIT_SLOT),
    SIM_FIELD_END
};
static struct hit_tab* volatile s_hit;
SIM_BLOCK(s_hit, SIM_STATE, SIM_ID_NONE, 1, hit_layout);
/* The three columns again, each a recorded table of its own, one u32 a unit slot: the recorder
   images hit_layout's column pointers as 0, and a fork reads them across its takeover point
   (tx_mark_held compares the stamps, hit_held the frees). hit_layout keeps its SIMF_COLUMN fields,
   so its n stays a kept count and no byte has two declarations; each column starts on a page of
   its own and shares none (hit_tab_new), as the recorder's write watch needs.
   LIFETIME: each points into the table s_hit was published with, which is never freed.
   BOUND: s_hitRows is that table's n (n <= room), published with the pointers; it is a heap
   table's count, so the seek never writes it (count_kept), and it writes at most that many rows.
   ORDERING: hit_reset publishes them with s_hit during the level load; their only readers, the
   recorder's capture and the seek, run in play, after it. `local` is the delay lever's oracle,
   which the simulation never reads; it is recorded so that a fork keeps it. */
static unsigned int* s_hitStamp;
static unsigned int* s_hitFreed;
static unsigned int* s_hitLocal;
static unsigned s_hitRows;
SIM_HEAP(s_hitStamp, SIM_STATE, SIM_ID_UNIT_SLOT, sizeof(unsigned int), s_hitRows, NULL);
SIM_HEAP(s_hitFreed, SIM_STATE, SIM_ID_UNIT_SLOT, sizeof(unsigned int), s_hitRows, NULL);
SIM_HEAP(s_hitLocal, SIM_STATE, SIM_ID_UNIT_SLOT, sizeof(unsigned int), s_hitRows, NULL);
SIM_VAR(s_hitRows, SIM_STATE);
/* counters for the heartbeat's hits: section; u32, one writer thread */
static struct {
    unsigned int tables;
    unsigned int outCreate, outHit, outBytes, stockBytes, txUnknown;
    unsigned int inCreate, inHit, stateRefused, malformed;
    unsigned int applyOwner, refuseOwner, dead;
    unsigned int applyBy, undecidedBy, refuseBy;
    unsigned int bare09, bare0B;
    unsigned int held, fallback, holdFail, retry;
    unsigned int createExact, createLB, createUnknown;
    unsigned int youngOwner, youngBy, delayed, delayOverflow;
    unsigned int takeNotes;       /* the lever's creation trace, capped */
} s_hitDiag;
SIM_VAR(s_hitDiag, SIM_PRIVATE);

/* TEST LEVER tagpu_dmgdelay.on=K: every outgoing hit waits until GameTime has advanced K
   ticks and leaves at the next send opportunity (an outgoing hit, an incoming companion on the
   game thread). 0 is off. The oracle it feeds, per receiver: a hit applied to a unit whose
   local create is younger than K ticks. */
static unsigned int s_hitDelayK;
SIM_VAR(s_hitDelayK, SIM_INSTALL);
static struct { unsigned int net, due; unsigned char m[HIT_MSG]; } s_hitDelay[HIT_DELAY_MAX];
/* a ring from s_hitDelayHead: the destination's DirectPlay ID, and the stock 0x0B at m+3 whose
   victim and attacker are unit slots (exe-reverse-engineering.md, "The hit") */
static const SimField hit_delay_layout[] = {
    SIM_FIELD(__typeof__(s_hitDelay[0]), net, SIMF_ID, SIM_ID_DPLAY),
    SIM_FIELD_AT(offsetof(__typeof__(s_hitDelay[0]), m) + 4, 2, SIMF_ID, SIM_ID_UNIT_SLOT),
    SIM_FIELD_AT(offsetof(__typeof__(s_hitDelay[0]), m) + 6, 2, SIMF_ID, SIM_ID_UNIT_SLOT),
    SIM_FIELD_END
};
SIM_ROWS(s_hitDelay, SIM_STATE, SIM_ID_NONE, hit_delay_layout);
static unsigned int s_hitDelayHead, s_hitDelayCount;
SIM_VAR(s_hitDelayHead, SIM_STATE);
SIM_VAR(s_hitDelayCount, SIM_STATE);

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
    tagpu_rec_exit();   /* the fork's own ExitProcess does not pass through its hook */
    ExitProcess(1);
}

#define HIT_PAGE 4096u                      /* the x86 page, GetWriteWatch's granularity */

static struct hit_tab* hit_tab_new(unsigned int n)
{
    unsigned int room = HIT_ROOM_MIN;
    struct hit_tab* t;
    while (room < n) room *= 2u;
    /* the header on the block's first page and each column on pages of its own: the recorder
       write-watches each column as a table, and a reset of a page two tables share hides one
       table's writes from the other. room x 4 bytes is a whole number of 4 KB pages (room is a
       power of two from 16384), and the block itself starts on a page (VirtualAlloc) */
    t = (struct hit_tab*)SIM_ALLOC(s_hit, HIT_PAGE + 3u * room * sizeof(unsigned int));
    if (!t) hit_no_memory(room);
    t->room  = room;
    t->stamp = (unsigned int*)((char*)t + HIT_PAGE);
    t->freed = t->stamp + room;
    t->local = t->freed + room;
    s_hitDiag.tables++;
    return t;
}

static void tx_reset(void);

/* In place of 0x4854A0's first two instructions, before the unit array is allocated: every
   stamp starts unknown with the array it describes, never keyed to GameTime; C4's passenger
   marks, keyed by these stamps, are emptied with them. */
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
    s_hitStamp = t->stamp;
    s_hitFreed = t->freed;
    s_hitLocal = t->local;
    s_hitRows = n;
    s_hitDelayHead = s_hitDelayCount = 0;
    tx_reset();
}

/* tagpu_limits.h, tagpu_unit_birth. The stamp column is the level's: hit_reset publishes it during the level
   load, and the level's first creates (0x496EE0, 0x4977BB) take and stamp on the loader thread too (THREADS,
   above). The loader's last store, bit 1 of main+0x38D75 at 0x497C62, comes after them, and the game thread
   reads that bit at 0x498342 before the level's first step, so every loader write is ordered before every
   call, which runs in play on the game thread. In play hit_take and the copies' stamps write it, and on a
   puppet the seek and the frame visit put the recorded column (tagpu_replay.c, tables_put), all on the game
   thread between steps. A slot is an index of the unit array, as hit_slot_index makes one, bounded by the
   column's rows. Without the fail-closed table
   no stamp is written, so there is no answer; nor for a stamp with HIT_LB, unknown or a copy's lower bound,
   which two units of one slot can share. Exact births of one slot rise strictly (hit_next_birth). */
int tagpu_unit_birth(unsigned int slot, unsigned int* out)
{
    unsigned int stamp;
    if (s_limState <= 0 || !s_hitStamp || slot >= s_hitRows) return 0;
    stamp = s_hitStamp[slot];
    if (stamp & HIT_LB) return 0;
    *out = stamp;
    return 1;
}

/* tagpu_limits.h, THE HOLD. The freed column is the level's, as the stamp column is (above); `slot` is
   bounded by its rows. */
int tagpu_unit_hold_on(void)
{
    return s_limState > 0 && s_hitFreed != NULL;
}

int tagpu_unit_slot_takeable(unsigned int slot)
{
    const char* ta = *(const char* const*)0x00511DE8;
    unsigned int f, now;
    if (!tagpu_unit_hold_on() || slot >= s_hitRows || !ta) return 1;
    f = s_hitFreed[slot];
    now = HIT_GAMETIME(ta);
    return !hit_held(f, now) || hit_reusable(f, now);
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
        if (take) s_hitDiag.held++;
        else if (older) { take = older; s_hitDiag.fallback++; }
        else {
            s_hitDiag.holdFail++;
            r[PR_ESI] = r[PR_EBP];
            return 0;
        }
        r[PR_ESI] = (unsigned int)(size_t)take;
        hit_slot_index(t, ta, take, &idx);
    }
    t->stamp[idx] = hit_next_birth(t->stamp[idx], now);
    t->local[idx] = now;
    if (s_hitDelayK && s_hitDiag.takeNotes < 20000u) {
        unsigned int stock = idx;
        s_hitDiag.takeNotes++;
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
   to 0x4553DA in the image), so the record is a carried one, with its birth 23 bytes on; the
   ghost-commander's replay and catch-up creates enter with the same whole carried record, and
   the replay's return counts as 0x4553E9 (ghost_is_09_return); 0x48BA05 and 0x48B49C are the
   0x2C's dirty entry and round robin. */
static void __cdecl hit_created(unsigned int* r)
{
    struct hit_tab* t = s_hit;
    const char* ta = *(const char* const*)0x00511DE8;
    const char* slot = (const char*)(size_t)r[PR_ESI];
    unsigned int ret = *(const unsigned int*)(WPN_ESP_JMP(r) + 0x1C);
    const unsigned char* rec = *(const unsigned char* const*)(WPN_ESP_JMP(r) + 0x24);
    unsigned int idx, stamp = HIT_UNKNOWN;
    if (!hit_slot_index(t, ta, slot, &idx)) return;
    if (ghost_is_09_return(ret) && (rec[3] | (unsigned int)rec[4] << 8) == idx) {
        memcpy(&stamp, rec + 23, 4);
        s_hitDiag.createExact++;
        kill_apply_state((char*)slot, rec);
    } else if ((ret == 0x0048BA05u || ret == 0x0048B49Cu) && hit_2c_bound(ta, slot, &stamp)) {
        s_hitDiag.createLB++;
    } else {
        s_hitDiag.createUnknown++;
    }
    t->stamp[idx] = stamp;
    t->local[idx] = HIT_GAMETIME(ta);
    tagpu_rec_network_created(slot, rec, stamp);
}

/* In place of 0x456050's `call 0x451DF0` (0x4560AE), with its signature: the unit's 0x09,
   carried with its birth and its state (a create its caller holds is read again when the
   caller lets it go, kill_flush). */
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
    kill_state_read(m, idx);
    s_hitDiag.outCreate++;
    s_hitDiag.outBytes += HIT_MSG;
    s_hitDiag.stockBytes += len;
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
    if (stamp == HIT_UNKNOWN) s_hitDiag.txUnknown++;
    s_hitDiag.outHit++;
    s_hitDiag.outBytes += HIT_MSG;
    s_hitDiag.stockBytes += len;
    if (s_hitDelayK && ta) {
        unsigned int now = HIT_GAMETIME(ta);
        hit_delay_flush(now);
        if (s_hitDelayCount < HIT_DELAY_MAX) {
            unsigned int q = (s_hitDelayHead + s_hitDelayCount) % HIT_DELAY_MAX;
            s_hitDelay[q].net = net;
            s_hitDelay[q].due = now + s_hitDelayK;
            memcpy(s_hitDelay[q].m, m, sizeof m);
            s_hitDelayCount++;
            s_hitDiag.delayed++;
            return 1;
        }
        s_hitDiag.delayOverflow++;
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

/* A carried 0x09 past its gate: counted, and its record for CreateFromNetwork -- the whole
   carried message from its 0x09 on, whose birth 0x48634F reads 23 bytes on. The ghost-commander
   replay and catch-up creates enter here too, with the create the gate refused. */
static const unsigned char* hit_rx_create_armed(const unsigned char* m)
{
    s_hitDiag.inCreate++;
    return m + 3;
}

/* 0 stock's chat; 1 done; 2 CreateFromNetwork on the record; 3 0x489CE0 on the record; 4 the
   destructor 0x4866D0 on the record in mode 0 -- the record goes back to the stub in
   regs[PR_EAX], where popad puts it in eax */
static int hit_rx_create(unsigned int* regs, const char* ta, const unsigned char* m)
{
    if (m[3] != 0x09) { s_hitDiag.malformed++; return 1; }
    if (!hit_gate(ta, 0x09)) { s_hitDiag.stateRefused++; return 1; }
    regs[PR_EAX] = (unsigned int)(size_t)hit_rx_create_armed(m);
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
    if (m[3] != 0x0B) { s_hitDiag.malformed++; return 1; }
    if (!hit_gate(ta, 0x0B)) { s_hitDiag.stateRefused++; return 1; }
    s_hitDiag.inHit++;
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
    if (!(flags & 0x10000000u) || (flags & 0x4000u)) { s_hitDiag.dead++; return 3; }   /* 0x489D45 */
    pl = *(const char* const*)(slot + 0x96);
    if (pl && *(const unsigned int*)pl && (pl[0x73] == 1 || pl[0x73] == 2)) {     /* 0x489EC6 */
        if (!hit_owner_accepts(t->stamp[vidx], stamp)) { s_hitDiag.refuseOwner++; return 1; }
        s_hitDiag.applyOwner++;
        if (hit_young(t, ta, vidx)) s_hitDiag.youngOwner++;
    } else {
        switch (hit_bystander_rule(t->stamp[vidx], stamp)) {
        case HIT_BY_STALE: s_hitDiag.refuseBy++; return 1;
        case HIT_BY_SAME:  s_hitDiag.applyBy++; break;
        default:           s_hitDiag.undecidedBy++; break;
        }
        if (hit_young(t, ta, vidx)) s_hitDiag.youngBy++;
    }
    return 3;
}

/* For a carried 0x09 the gate refuses in state 5: 1 when the ghost-commander queue holds it,
   2 when it is created now (the catch-up ticks, its record in regs[PR_EAX] as
   hit_rx_create puts it), 0 when it is not the queue's */
static int ghost_take(unsigned int* regs, const char* ta, const unsigned char* m);

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
    case HIT_TAG_CREATE: {
        int g = ghost_take(regs, ta, m);
        return g ? g : hit_rx_create(regs, ta, m);
    }
    case HIT_TAG_HIT:    return hit_rx_hit(regs, ta, m);
    case HIT_TAG_DEATH:  return kill_rx_death(regs, ta, m);
    default:             return 0;
    }
}

/* ---- stubs, in B4's own page ------------------------------------------------------------- */

static unsigned char* s_hitCode;
SIM_CODE(s_hitCode);
static unsigned int   s_hitCodeUsed;
SIM_VAR(s_hitCodeUsed, SIM_INSTALL);

static unsigned char* hit_code(unsigned int n)
{
    unsigned char* p;
    if (!s_hitCode)
        s_hitCode = (unsigned char*)SIM_ALLOC(s_hitCode, 0x1000);
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
    char b[16];
    unsigned int k;
    if (tagpu_sim_lever_read("tagpu_dmgdelay.on", b, sizeof b) < 0) return;
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
    unsigned char *aTake, *aFree, *aExit, *aAlloc, *aChat, *aBare09, *aBare0B, *aSpawn, *aWrap,
                  *p, *doCreate, *doHit, *doDeath, *jCreate, *jHit, *jDeath, *jNone;

    hit_read_lever();
    if (tagpu_sim_lever("tagpu_wirecheck.on")) hit_selfcheck();

    if (!(aTake = hit_code(64)) || !(aFree = hit_code(32)) || !(aExit = hit_code(32)) ||
        !(aAlloc = hit_code(32)) || !(aChat = hit_code(96)) || !(aBare09 = hit_code(16)) ||
        !(aBare0B = hit_code(16)) || !(aSpawn = hit_code(48)) || !(aWrap = tx_wrap_code())) {
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

    /* the 0x05 slot: 0 stock chat, 1 the loop, 2 the carried 0x09, 3 the carried 0x0B, 4 the
       carried 0x0C, each entered with the return address stock's own case pushes and the
       record in eax */
    p = aChat;
    *p++ = 0x60; *p++ = 0x54;
    p = hit_jmp(p, 0xE8, (unsigned int)(size_t)hit_rx_chat);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x02;                /* cmp eax,2          */
    *p++ = 0x74; jCreate = p++;                          /* je do_create       */
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x03;                /* cmp eax,3          */
    *p++ = 0x74; jHit = p++;                             /* je do_hit          */
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x04;                /* cmp eax,4          */
    *p++ = 0x74; jDeath = p++;                           /* je do_death        */
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
    p = hit_jmp(p, 0xE9, 0x00489CE0);
    doDeath = p;                                         /* the case 0x45541C, */
    *p++ = 0x61;                                         /* through tx_wrap (C4): */
    *p++ = 0x6A; *p++ = 0x00;                            /* push 0 (mode 0)    */
    *p++ = 0x50;                                         /* push eax           */
    *p++ = 0x68; p = hit_abs(p, (const void*)0x00455428); /* push 0x455428     */
    hit_jmp(p, 0xE9, (unsigned int)(size_t)aWrap);
    *jCreate = (unsigned char)(doCreate - (jCreate + 1));
    *jHit = (unsigned char)(doHit - (jHit + 1));
    *jDeath = (unsigned char)(doDeath - (jDeath + 1));

    /* a bare 0x09 / 0x0B: counted, and the loop goes on */
    p = aBare09;
    *p++ = 0xFF; *p++ = 0x05; p = hit_abs(p, &s_hitDiag.bare09);
    hit_jmp(p, 0xE9, 0x00455F50);
    p = aBare0B;
    *p++ = 0xFF; *p++ = 0x05; p = hit_abs(p, &s_hitDiag.bare0B);
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
    *p++ = 0xFF; *p++ = 0x05; p = hit_abs(p, &s_hitDiag.retry);          /* inc [s_hitDiag.retry] */
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
   Deathmatch respawns that found no slot and wait for their countdown's next fire. The alert
   fields come first: the heartbeat is one log line of at most 2040 bytes, cut from its end,
   and only the ghost section follows this one. */
int tagpu_hits_format(char* buf, unsigned int cap)
{
    const struct hit_tab* t = s_hit;
    return _snprintf(buf, cap,
                     " | hits: bad=%u bare 09=%u 0b=%u over=%u young owner=%u by=%u"
                     " slots=%u tables=%u out 09=%u 0b=%u B=%u stockB=%u unk=%u"
                     " in 09=%u 0b=%u owner=%u/%u by=%u/%u/%u dead=%u gate=%u"
                     " copy exact=%u bound=%u unk=%u held=%u fallback=%u"
                     " holdfail=%u retry=%u delay=%u q=%u",
                     s_hitDiag.malformed, s_hitDiag.bare09, s_hitDiag.bare0B, s_hitDiag.delayOverflow,
                     s_hitDiag.youngOwner, s_hitDiag.youngBy,
                     t ? t->n : 0u, s_hitDiag.tables, s_hitDiag.outCreate, s_hitDiag.outHit, s_hitDiag.outBytes,
                     s_hitDiag.stockBytes, s_hitDiag.txUnknown, s_hitDiag.inCreate, s_hitDiag.inHit, s_hitDiag.applyOwner,
                     s_hitDiag.refuseOwner, s_hitDiag.applyBy, s_hitDiag.undecidedBy, s_hitDiag.refuseBy, s_hitDiag.dead,
                     s_hitDiag.stateRefused,
                     s_hitDiag.createExact, s_hitDiag.createLB, s_hitDiag.createUnknown, s_hitDiag.held,
                     s_hitDiag.fallback, s_hitDiag.holdFail, s_hitDiag.retry, s_hitDelayK, s_hitDiag.delayed);
}

SIM_FEATURE(ghost_commander, SIM_PUPPET_RUNS, "battle");

/* ===== GHOST COMMANDER: THE CREATES REFUSED DURING THE LOAD, AND THE DIRTY CREATE'S POSITION ===
   Landing B5 of research/notes/tadr-port/sim-fixes.md ("B5 DESIGN" has the argument in full;
   the addresses are in exe-reverse-engineering.md, "A create refused during the load").

   THE DEFECT [DISASSEMBLED; MEASURED with the fix off]. The dispatcher passes a unit
   create only in net state 6 (0x45473F with the table 0x512BC0: 0x09 has mask 4 alone), and a
   peer stays in state 5 until its game thread's 0x498445, after its load. A peer still loading
   when the others enter play drops their first creates, the commanders among them, until the
   round robin re-creates each unit (50 s at the 1500-unit limit, 16.7 s at 500). A unit that
   moves first comes back sooner through a dirty 0x2C entry, whose create record
   (0x48B9B6..0x48B9FB) takes the SLOT's own stale position, (0,0,0) in a fresh array.

   THE FIX, three parts.
   - The queue: a carried 0x09 the gate refuses while the level loads is kept per sender, in
     arrival order, and replayed before the level's first tick, at the in-play entry's call of
     the frame function (0x49842F). That function's catch-up ticks (up to five, 0x495490) still
     run in state 5, and so does the frame function's own pump after them (0x4968CB); a create
     refused in either is made at once, in the pump it arrives in (a tick's 0x4954C8, or
     0x4968CB), which is where state 6 makes it. Both pass B4's receiver past
     its state test only (hit_rx_create_armed), so the incarnation is set as a live create sets
     it, and enter CreateFromNetwork with the sender in edi as the case has it, so B3's bounds
     and observe run on them too.
   - The kills: a death the gate refuses in state 5 (the carried 0x0C in B4's receiver,
     kill_rx_death) cancels the latest create held for its slot from its sender, so a unit the
     owner destroyed during the load is never made. One refused in the catch-up ticks, for a
     copy the replay or a catch-up tick made, marks the copy dying as the engine's own ghost
     sweep does (0x48B42C), and the unit tick destroys it (0x48AFB9 -> 0x4864B0).
   - The position: the dirty create's record takes the position its entry's own move payload
     carries, read ahead in the engine's bit order, for the payloads disassembled to carry one
     (ghost_payload_pos says which, and where each lands).

   INVARIANTS.
   - Bounded: GHOST_DEPTH records per sender, ten senders. A create past a full queue is
     counted and left to the round robin, which is where stock leaves every one of them.
   - Emptied by the level's own lifetime, never by GameTime: cleared at the load's start
     (0x497F5E, the load state 0x497F40's first call, on the game thread before 0x4982CA
     creates the loader thread), drained before the first tick. Every record leaves exactly
     once: replayed, cancelled by its kill, or counted.
   - An ordering, not a window. During the load two threads pump (the game thread at 0x49852E,
     the loader at 0x49727D), so records go in under a lock. The loader's last pump comes
     before its last store, bit 1 (value 2) of main+0x38D75 at 0x497C62, and the game thread
     reaches 0x49842F only after reading that bit at 0x498342, so the drain sees every create
     the load refused. After it only the game thread pumps (the catch-up ticks' 0x4954C8, the
     frame function's 0x4968CB), and nothing more is held: a create refused there is made in
     that pump.
   - Before the first tick is safe: CreateFromNetwork calls what the local create 0x485F50
     calls (0x485A40, 0x485D40, 0x49E070, 0x437840, 0x43DC00, 0x48A870, 0x47CC30, 0x482AC0,
     0x490580), and over an occupied slot first destroys its unit through 0x4864B0
     (0x486237..0x486244), the unit tick's own destroy; that sends a 0x0C only for a local
     player's unit (0x48664B), and the replay never takes a local player's slot. Stock's loader
     runs 0x485F50 for this peer's commander in state 5, before any tick (0x4977BB); neither
     body reads the net state or GameTime. What the
     replay skips of the dispatcher is the state test alone: the sender test is re-run on the
     record under the DirectPlay id it had. A create made in a catch-up tick has just passed
     the dispatcher's sender test (0x4547AD) and is made where state 6 makes it.
   - A record is replayed only when its sender passes the dispatcher's own sender test now
     (0x4547AD..0x4547E2: present, type 3, +0x146 not 10) under the DirectPlay id it had, and
     its slot is empty or holds an older incarnation by B4's stamps -- never a local player's.
   - Kills pair with creates in ONE sender's arrival order: the owner alone sends a unit's 0x0C
     (0x48664B..0x48666D, the one send of the 0x0C's 11 bytes), and a slot is taken only once
     it is free, so its stream for a slot alternates create, kill. If one sender's messages could arrive out of order (the
     plan's open question), a replayed ghost lasts until the owner's round robin marks it
     (0x48B415), at most N of its ticks -- stock's bound for any ghost.
   - The game thread only: the drain, the catch-up creates and the dying mark check their
     thread and do nothing on any other.
   - On the map by construction: a position is taken only when 0 <= x < W*16 and 0 <= z < H*16
     px (W, H at main+0x14233/+0x14237, each 1..4096), and the record keeps stock's otherwise.

   CLASS: simulation, fail closed, both builds -- which units a peer holds, and where, is shared
   state. Every site is a row of the fail-closed table. */

#define GHOST_DEPTH   64                    /* per sender: a load's window holds a few creates */
#define GHOST_SENDERS 10                    /* the player records main+0x1B63, stride 0x14B    */
#define GHOST_NOTES   64

typedef struct {
    unsigned char m[HIT_MSG];               /* the tagged 0x05 as received                     */
    unsigned int  arg;                      /* the 0x09 case's player argument, its [esp+0x14] */
    unsigned int  dpid;                     /* the sender's DirectPlay id, record + 4           */
    int           dead;                     /* cancelled by a later refused 0x0C for its slot   */
} GHOSTREC;

#define GHOST_DONE    1024                  /* copies made before state 6 a kill can still mark */

typedef struct { unsigned int idx, k, birth; } GHOSTDONE;

typedef void* (__stdcall* GHOSTCALL)(unsigned int arg, const unsigned char* rec, const char* sender);

static CRITICAL_SECTION s_ghostLock;        /* the queue: whichever thread pumps, then the drain */
SIM_LOCK(s_ghostLock);
static GHOSTREC     s_ghostQ[GHOST_SENDERS][GHOST_DEPTH];
SIM_VAR(s_ghostQ, SIM_TRANSIENT);
static unsigned int s_ghostN[GHOST_SENDERS];
SIM_VAR(s_ghostN, SIM_TRANSIENT);
static int          s_ghostPhase;           /* 1 once this level's first drain has run          */
SIM_VAR(s_ghostPhase, SIM_STATE);
static GHOSTREC     s_ghostTake[GHOST_SENDERS][GHOST_DEPTH];   /* the drain's copy, game thread */
SIM_VAR(s_ghostTake, SIM_TRANSIENT);
static const SimField ghost_done_layout[] = {
    SIM_FIELD(GHOSTDONE, idx, SIMF_ID, SIM_ID_UNIT_SLOT),
    SIM_FIELD(GHOSTDONE, k, SIMF_ID, SIM_ID_PLAYER),
    SIM_FIELD_END
};
static GHOSTDONE    s_ghostDone[GHOST_DONE];    /* the drain's and the catch-up ticks' copies */
static unsigned int s_ghostDoneN;
SIM_VAR(s_ghostDoneN, SIM_STATE);
SIM_ROWS_LIVE(s_ghostDone, SIM_STATE, SIM_ID_NONE, s_ghostDoneN, ghost_done_layout);
static DWORD        s_ghostGameTid;         /* DllMain's thread, which runs the main loop       */
SIM_VAR(s_ghostGameTid, SIM_INSTALL);
static int          s_ghostOff;             /* TEST LEVER tagpu_ghostq.off                      */
SIM_VAR(s_ghostOff, SIM_INSTALL);
static unsigned int s_ghostRet;             /* the replay stub's return into CreateFromNetwork  */
SIM_VAR(s_ghostRet, SIM_INSTALL);
static GHOSTCALL    s_ghostCall;
SIM_VAR(s_ghostCall, SIM_INSTALL);

/* counters for the heartbeat's ghost: section; u32. q, over, killed and nokill are written
   under the lock, the rest on the game thread. */
static struct {
    unsigned int  queued, overflow, deep, killed, noKill;
    unsigned int  replayed, now, swept, untracked;
    unsigned int  inactive, stale, bad, cleared;
    unsigned int  offThread, levels;
    unsigned int  posGround, posAir, posNone, posOff;
    unsigned int  posUnbound, posShort;
    volatile LONG notes;
} s_ghostDiag;
SIM_VAR(s_ghostDiag, SIM_PRIVATE);

static int ghost_is_09_return(unsigned int ret)
{
    return ret == 0x004553E9u || (s_ghostRet != 0u && ret == s_ghostRet);
}

/* ---- the pure rules; tagpu_wirecheck.on exercises them on boundary values ------------------ */

/* 1 when a held create, the owner's birth `birth`, is newer than what its slot holds: an empty
   slot, or a copy B4 stamped earlier -- an exact birth, or a lower bound g0 (the unit alive in
   the slot at the owner's g0, so born at or before it). A birth that is itself unknown
   replaces nothing. */
static int ghost_newer(int occupied, unsigned int copy, unsigned int birth)
{
    if (!occupied) return 1;
    if (birth & HIT_LB) return 0;
    return birth > (copy & ~HIT_LB);
}

/* a position in whole world px against the map's size in 16-px cells */
static int ghost_on_map(int x, int z, unsigned int w, unsigned int h)
{
    return w >= 1u && w <= 4096u && h >= 1u && h <= 4096u && x >= 0 && z >= 0 &&
           (unsigned int)x < w * 16u && (unsigned int)z < h * 16u;
}

/* The record a kill for slot `idx` cancels: the latest create held for that slot and not yet
   cancelled, or -1. One sender's stream for a slot alternates create, kill, so the latest is
   the unit the kill names. */
static int ghost_cancel(const GHOSTREC* q, unsigned int n, unsigned int idx)
{
    while (n-- > 0)
        if (!q[n].dead && (q[n].m[6] | (unsigned int)q[n].m[7] << 8) == idx) return (int)n;
    return -1;
}

/* The engine's bit reader 0x415DC0, on a copy: {dwords, word, bit}, least significant bit
   first, n in 1..32. It reads no dword the engine's own read of the same bits does not. */
typedef struct { const unsigned int* data; unsigned int word, bit; } GHOSTBITS;

static unsigned int ghost_bits(GHOSTBITS* b, unsigned int n)
{
    unsigned int lo, used;
    if (b->bit + n < 32u) {
        lo = (b->data[b->word] >> b->bit) & ((1u << n) - 1u);
        b->bit += n;
        return lo;
    }
    if (b->bit == 0u) return b->data[b->word++];         /* n == 32 on a word boundary */
    lo = b->data[b->word++] >> b->bit;                    /* the 32 - bit bits left     */
    used = 32u - b->bit;
    n -= used;
    b->bit = n;
    return n ? lo | (b->data[b->word] & ((1u << n) - 1u)) << used : lo;
}

enum { GHOST_POS_NONE, GHOST_POS_GROUND, GHOST_POS_AIR, GHOST_POS_SHORT };

/* The position a dirty entry's move payload carries, read ahead of the decoder that will parse
   it. A remote unit's move object is made by 0x43DC00: the ground proxy 0x4FD488 unless the
   def's +0x241 bit 11 makes it the air proxy 0x4FD9E0 (0x43DC5F..0x43DC68).
   - Ground, decoder 0x44F5C0 (the owner writes it at 0x44F4A0): one flag bit, a 2-bit count n
     (0..3), then n points of int16 x, int16 z in whole world px -- the owner mover's path
     from its front (+0xC, count +0x5C). Point 0 is the node the unit last REACHED, never the
     next one: the mover's step 0x44F1A0 (vtable 0x4FD458 + 8, called through 0x43DD20 from
     the unit tick at 0x48AFAA) drops the front only once the unit is within 5 px of point 1
     (0x44F1D7..0x44F235, the squared distance against 0x19), and a straight order's path is
     [the unit's position, the goal] (0x44F3F2..0x44F417 stores +0x6C/+0x74, the integer halves
     of +0x6A/+0x72), so a straight move keeps its origin as point 0 until the unit is within
     5 px of its goal. 0x44F100 does the same shift and nothing calls it. A unit created well
     along a long straight move is therefore placed at its origin and trails the owner's until
     the round robin's full state for its slot writes the owner's x, y, z into it
     (0x48B5CA..0x48B6A7): at most N owner ticks, the bound stock's (0,0,0) has too. The
     receiver's proxy hands the points to the unit as its path (0x44F650). Taken: point 0, x
     and z; y stays stock's.
   - Air, decoder 0x490A10: a 2-bit selector. 2 is the 0x2C-byte motion 0x44E9C0 (written by
     0x44E930): one flag bit, then x, y, z in 16.16 and a velocity -- the point the proxy's step
     0x490690 copies into its own +0xC each tick (0x44EA60, called at 0x4906B8), the unit's
     dead-reckoned position. Taken: x, y, z. 1 is 0x44E080's object, whose optional vector
     +0x26 is a GOAL (0x44E3C0 refreshes it from the followed unit) and not the unit's own
     position; 0 and 3 carry no motion. Neither is taken.
   `avail` is the bits left in the message from the reader's position: a read that would pass
   the message's end is GHOST_POS_SHORT, before it is made. Only the bits read here are tested
   (35 for ground, 99 for air), not the rest of the payload: the engine's decoder reads that
   from B3's zero-padded copy, and B3's check before the next entry's type (wire_s2c_delta)
   stops the stream if the payload ran past the end. A well-formed entry's payload is inside
   its message, so the test never refuses one.
   Returns the kind, with x, z in whole px and x16, y16, z16 as the record holds them. */
static int ghost_payload_pos(GHOSTBITS b, unsigned long long avail, int air, int* x, int* z,
                             unsigned int* x16, unsigned int* y16, unsigned int* z16)
{
    if (!air) {
        unsigned int n;
        if (avail < 3u) return GHOST_POS_SHORT;
        (void)ghost_bits(&b, 1);
        n = ghost_bits(&b, 2);
        if (n == 0u) return GHOST_POS_NONE;
        if (avail < 3u + 32u) return GHOST_POS_SHORT;
        *x = (short)ghost_bits(&b, 16);
        *z = (short)ghost_bits(&b, 16);
        if (*x < 0 || *z < 0) return GHOST_POS_NONE;
        *x16 = (unsigned int)*x << 16;
        *z16 = (unsigned int)*z << 16;
        return GHOST_POS_GROUND;
    }
    if (avail < 2u) return GHOST_POS_SHORT;
    if (ghost_bits(&b, 2) != 2u) return GHOST_POS_NONE;
    if (avail < 2u + 1u + 96u) return GHOST_POS_SHORT;
    (void)ghost_bits(&b, 1);
    *x16 = ghost_bits(&b, 32);
    *y16 = ghost_bits(&b, 32);
    *z16 = ghost_bits(&b, 32);
    if ((int)*x16 < 0 || (int)*z16 < 0) return GHOST_POS_NONE;
    *x = (int)(*x16 >> 16);
    *z = (int)(*z16 >> 16);
    return GHOST_POS_AIR;
}

/* ---- the engine side --------------------------------------------------------------------- */

/* the dispatcher's own test of a sender (0x4547AD..0x4547E2), under the id it had */
static int ghost_sender_active(const char* ta, unsigned int k, unsigned int dpid)
{
    const unsigned char* rec;
    if (k >= GHOST_SENDERS) return 0;
    rec = (const unsigned char*)ta + 0x1B63 + k * 0x14B;
    return *(const unsigned int*)rec != 0u && rec[0x73] == 3 && rec[0x146] != 0x0A &&
           *(const unsigned int*)(rec + 4) == dpid;
}

/* 1 when an occupied slot's owner is one of the ten records and a local player (type 1 or 2) */
static int ghost_local_owner(const char* ta, const char* slot)
{
    const char* players = ta + 0x1B63;
    const char* pl = *(const char* const*)(slot + 0x96);
    if (pl < players || pl >= players + GHOST_SENDERS * 0x14B || (pl - players) % 0x14B) return 0;
    return *(const unsigned int*)pl != 0u && (pl[0x73] == 1 || pl[0x73] == 2);
}

/* a copy made before state 6, which a kill refused in the catch-up ticks can still mark;
   game thread. Past GHOST_DONE a copy is not tracked, and its kill waits for the round robin. */
static void ghost_made(unsigned int idx, unsigned int k, unsigned int birth)
{
    GHOSTDONE* d;
    if (s_ghostDoneN >= GHOST_DONE) { s_ghostDiag.untracked++; return; }
    d = &s_ghostDone[s_ghostDoneN++];
    d->idx = idx;
    d->k = k;
    d->birth = birth;
}

/* In B4's 0x05 receiver, before the carried 0x09's own gate: in state 5, where that gate
   refuses it, hold it, or after the drain make it at once. regs is the receiver's frame: edi
   the sender's record, the case's player argument at the site's esp + 0x14, paired by
   0x453E84..0x453EA1 (a row of the table). */
static int ghost_take(unsigned int* regs, const char* ta, const unsigned char* m)
{
    const char* snd = (const char*)(size_t)regs[PR_EDI];
    unsigned int arg, k, birth;
    int now;
    if (s_ghostOff || m[3] != 0x09 || *(const unsigned int*)(ta + 0x391F1) != 5u ||
        hit_gate(ta, 0x09))
        return 0;
    arg = *(const unsigned int*)(WPN_ESP_JMP(regs) + 0x14);
    k = arg & 0xFFu;
    if (k >= GHOST_SENDERS || snd != ta + 0x1B63 + k * 0x14B) return 0;
    EnterCriticalSection(&s_ghostLock);
    now = s_ghostPhase;
    if (!now) {
        if (s_ghostN[k] < GHOST_DEPTH) {
            GHOSTREC* q = &s_ghostQ[k][s_ghostN[k]++];
            memcpy(q->m, m, HIT_MSG);
            q->arg = arg;
            q->dpid = *(const unsigned int*)(snd + 4);
            q->dead = 0;
            s_ghostDiag.queued++;
            if (s_ghostN[k] > s_ghostDiag.deep) s_ghostDiag.deep = s_ghostN[k];
        } else {
            s_ghostDiag.overflow++;
        }
    }
    LeaveCriticalSection(&s_ghostLock);
    if (!now) return 1;
    /* after the drain only the game thread pumps (0x4954C8, 0x4968CB) */
    if (GetCurrentThreadId() != s_ghostGameTid) { s_ghostDiag.offThread++; return 0; }
    memcpy(&birth, m + 26, 4);
    ghost_made(m[6] | (unsigned int)m[7] << 8, k, birth);
    s_ghostDiag.now++;
    if (InterlockedIncrement(&s_ghostDiag.notes) <= GHOST_NOTES)
        tagpu_logf("enginefix: ghost commander: created slot %u from sender %u (birth %u, type %u) "
                   "at GameTime %u, in a catch-up tick", m[6] | (unsigned int)m[7] << 8, k,
                   birth, m[4] | (unsigned int)m[5] << 8, HIT_GAMETIME(ta));
    regs[PR_EAX] = (unsigned int)(size_t)hit_rx_create_armed(m);
    return 2;
}

/* A copy made before state 6 from sender k's create for slot idx, still that copy: marked
   dying as the engine's ghost sweep marks one (0x48B426..0x48B42F, bit 14 of +0x110), which
   the unit tick then destroys without a message (0x48AFB9 -> 0x4864B0; it sends a 0x0C only
   for a local player's unit, 0x48664B). Game thread, from the pump, as that sweep runs.
   frac, when not NULL, is the owner's +0x104 carried by the death (B8), already checked to be
   a number in [0, 1]: written first, so that destroy decides on the owner's value. */
static int ghost_sweep(const char* ta, unsigned int k, unsigned int idx, const unsigned char* frac)
{
    const char* first = *(const char* const*)(ta + 0x14357);
    const char* last  = *(const char* const*)(ta + 0x1435B);
    const struct hit_tab* t = s_hit;
    char* slot;
    unsigned int i = s_ghostDoneN, flags;
    while (i > 0 && !(s_ghostDone[i - 1].idx == idx && s_ghostDone[i - 1].k == k)) i--;
    if (i == 0 || !t || !first || last < first || idx == 0u || idx >= t->n ||
        idx > (unsigned int)(last - first) / 0x118)
        return 0;
    slot = (char*)first + (size_t)idx * 0x118;
    flags = *(const unsigned int*)(slot + 0x110);
    if (!*(const unsigned short*)(slot + 0xA6) ||
        *(const char* const*)(slot + 0x96) != ta + 0x1B63 + k * 0x14B ||
        t->stamp[idx] != s_ghostDone[i - 1].birth ||
        !(flags & 0x10000000u) || (flags & 0x4000u))
        return 0;
    if (frac) memcpy(slot + 0x104, frac, 4);
    *(unsigned int*)(slot + 0x110) = flags | 0x4000u;
    return 1;
}

/* A 0x0C refused in state 5, `m` its record and regs the dispatcher's frame (the sender's
   record index in the low byte of the site's esp + 0x14, its record in edi): a unit its owner
   destroyed while this peer loaded. Its held create is cancelled; failing that, in the
   catch-up ticks, the copy made before state 6 is marked dying, with frac (ghost_sweep)
   written first. Reached from B4's 0x05 receiver for the carried 0x0C (kill_rx_death), whose
   frame is the dispatcher's. 1 when a copy was marked dying, on the game thread. */
static int ghost_refused_kill(unsigned int* regs, const unsigned char* m, const unsigned char* frac)
{
    const char* ta = *(const char* const*)0x00511DE8;
    const unsigned char* sp = WPN_ESP_JMP(regs);
    const char* snd = (const char*)(size_t)regs[PR_EDI];
    unsigned int k, idx;
    int i, late;
    if (s_ghostOff || !ta || !m || m[0] != 0x0C) return 0;
    k = *(const unsigned int*)(sp + 0x14) & 0xFFu;
    if (k >= GHOST_SENDERS || snd != ta + 0x1B63 + k * 0x14B) return 0;
    idx = m[1] | (unsigned int)m[2] << 8;
    EnterCriticalSection(&s_ghostLock);
    i = ghost_cancel(s_ghostQ[k], s_ghostN[k], idx);
    if (i >= 0) {
        s_ghostQ[k][i].dead = 1;
        s_ghostDiag.killed++;
    }
    late = s_ghostPhase;
    LeaveCriticalSection(&s_ghostLock);
    if (i >= 0) return 0;
    if (late && GetCurrentThreadId() == s_ghostGameTid && ghost_sweep(ta, k, idx, frac)) {
        s_ghostDiag.swept++;
        if (InterlockedIncrement(&s_ghostDiag.notes) <= GHOST_NOTES)
            tagpu_logf("enginefix: ghost commander: slot %u from sender %u killed during the "
                       "catch-up ticks; its copy made before state 6 marked dying", idx, k);
        return 1;
    }
    EnterCriticalSection(&s_ghostLock);
    s_ghostDiag.noKill++;
    LeaveCriticalSection(&s_ghostLock);
    return 0;
}

/* The dispatcher's refusal of a message in state 5 (0x45477F), on whichever thread pumps. A
   bare 0x0C cannot come from this build, which sends every death as a 0x4C (B8): counted and
   dropped, as the 0x0C slot drops one in state 6. */
static void __cdecl ghost_refused(unsigned int* regs)
{
    const unsigned char* m = *(const unsigned char* const*)(WPN_ESP_JMP(regs) + 0x10);
    if (m && m[0] == 0x0C) InterlockedIncrement(&s_killDiag.bare0C);
}

/* In place of 0x497F5E, the load state's first call (bit 0 of main+0x38D75 clear), on the game
   thread before the loader thread exists: whatever an abandoned load left is counted and gone. */
static void __cdecl ghost_reset(unsigned int* regs)
{
    unsigned int k, left = 0;
    (void)regs;
#ifndef TAGPU_LIMITS_STOCK
    cob_level_reset();
#endif
    EnterCriticalSection(&s_ghostLock);
    for (k = 0; k < GHOST_SENDERS; k++) {
        left += s_ghostN[k];
        s_ghostN[k] = 0;
    }
    s_ghostPhase = 0;
    s_ghostDoneN = 0;
    LeaveCriticalSection(&s_ghostLock);
    if (left) {
        s_ghostDiag.cleared += left;
        tagpu_logf("enginefix: ghost commander: %u held creates from a load that never reached "
                   "play, cleared", left);
    }
}

/* At 0x49842F, on the game thread, before the frame function runs the level's first tick:
   every held create is replayed through B4's receiver past its state test and
   CreateFromNetwork, or counted. From here on a create refused in state 5 is made at once. */
static void __cdecl ghost_replay(unsigned int* regs)
{
    const char* ta = *(const char* const*)0x00511DE8;
    const struct hit_tab* t = s_hit;
    const char* first;
    const char* last;
    unsigned int n[GHOST_SENDERS], k, i, total = 0, max = 0, done = 0, dead = 0, before;
    (void)regs;
    EnterCriticalSection(&s_ghostLock);
    for (k = 0; k < GHOST_SENDERS; k++) {
        n[k] = s_ghostN[k];
        memcpy(s_ghostTake[k], s_ghostQ[k], n[k] * sizeof(GHOSTREC));
        s_ghostN[k] = 0;
        total += n[k];
    }
    s_ghostPhase = 1;
    LeaveCriticalSection(&s_ghostLock);
    s_ghostDiag.levels++;
    if (!ta || GetCurrentThreadId() != s_ghostGameTid) {
        s_ghostDiag.offThread += total;
        tagpu_logf("enginefix: ghost commander: the in-play entry ran off the game thread; %u "
                   "held creates dropped", total);
        return;
    }
#ifndef TAGPU_LIMITS_STOCK
    cob_level_enter();
#endif
    first = *(const char* const*)(ta + 0x14357);
    last  = *(const char* const*)(ta + 0x1435B);
    if (first && last >= first) max = (unsigned int)(last - first) / 0x118;
    before = s_ghostDiag.inactive + s_ghostDiag.stale + s_ghostDiag.bad;
    for (k = 0; k < GHOST_SENDERS; k++)
        for (i = 0; i < n[k]; i++) {
            const GHOSTREC* q = &s_ghostTake[k][i];
            unsigned int idx = q->m[6] | (unsigned int)q->m[7] << 8, birth;
            const char* slot;
            memcpy(&birth, q->m + 26, 4);
            if (q->dead) { dead++; continue; }                     /* counted as killed */
            if (!ghost_sender_active(ta, k, q->dpid)) { s_ghostDiag.inactive++; continue; }
            if (idx == 0u || idx > max || !t || idx >= t->n) { s_ghostDiag.bad++; continue; }
            slot = first + (size_t)idx * 0x118;
            if (*(const unsigned short*)(slot + 0xA6) &&
                (ghost_local_owner(ta, slot) || !ghost_newer(1, t->stamp[idx], birth))) {
                s_ghostDiag.stale++;
                continue;
            }
            /* the whole carried message: 0x48634F reads the birth 23 bytes into the record */
            s_ghostCall(q->arg, hit_rx_create_armed(q->m), ta + 0x1B63 + k * 0x14B);
            ghost_made(idx, k, birth);
            s_ghostDiag.replayed++;
            done++;
            if (InterlockedIncrement(&s_ghostDiag.notes) <= GHOST_NOTES)
                tagpu_logf("enginefix: ghost commander: replayed slot %u from sender %u (birth %u, "
                           "type %u) at GameTime %u, before the first tick; the slot now holds "
                           "type %u", idx, k, birth, q->m[4] | (unsigned int)q->m[5] << 8,
                           HIT_GAMETIME(ta), *(const unsigned short*)(slot + 0xA6));
        }
    tagpu_logf("enginefix: ghost commander: before the first tick (GameTime %u), %u held creates: "
               "%u replayed, %u killed while held, %u not (inactive %u, stale %u, bad %u in "
               "total)", HIT_GAMETIME(ta), total, done, dead,
               s_ghostDiag.inactive + s_ghostDiag.stale + s_ghostDiag.bad - before, s_ghostDiag.inactive,
               s_ghostDiag.stale, s_ghostDiag.bad);
}

/* In place of the dirty create's `call 0x4861D0` at 0x48BA00, entered by a call so that
   CreateFromNetwork still sees 0x48BA05: [esp] the return, [esp+4] the player (its low byte,
   the slot's +0xFF), [esp+8] the 23-byte record, and the 0x2C's reader at [esp+0x1C] (the
   frame's esp + 0x10, after the two pushes and the call; its slots set at 0x48B933..0x48B93E,
   a row of the table).
   The payload is read only from B3's copy of the message, and only within its length: the
   reader must be the one B3's wire_s2c_copy set up for this 0x2C (s_wire2cRd) and still point
   at its copy, whose WIRE_2C_PAD zero bytes cover the dword a last read touches. B3 is armed
   whenever this runs (fix_ghost_commander refuses the table otherwise), so `unbound` can only
   count a reader that is not the 0x2C's own, which the disassembly says never reaches here. */
static void __cdecl ghost_position(unsigned int* r)
{
    const char* ta = *(const char* const*)0x00511DE8;
    unsigned char* sp = WPN_ESP_JMP(r);
    unsigned char* rec = *(unsigned char* const*)(sp + 8);
    const unsigned int* rd = (const unsigned int*)(sp + 0x1C);
    unsigned int k = *(const unsigned int*)(sp + 4) & 0xFFu;
    unsigned int type, count, x16 = 0, y16 = 0, z16 = 0, sx, sz;
    const unsigned char* pl;
    const char* defs;
    const char* def;
    GHOSTBITS b;
    unsigned long long pos, avail;
    int kind, x = 0, z = 0;
    if (!ta || !rec || k >= GHOST_SENDERS || !rd[0]) { s_ghostDiag.posNone++; return; }
    pl = (const unsigned char*)ta + 0x1B63 + k * 0x14B;
    type  = rec[1] | (unsigned int)rec[2] << 8;
    count = *(const unsigned int*)(ta + 0x1438F);
    defs  = *(const char* const*)(ta + 0x1439B);
    /* 0x43DC48..0x43DC57: only a remote player's unit gets a proxy; 0x4862CF: only a def
       with a move class gets a move object at all */
    if (!*(const unsigned int*)pl || pl[0x73] != 3 || !defs || type == 0u || type >= count) {
        s_ghostDiag.posNone++;
        return;
    }
    def = defs + (size_t)type * 0x249;
    if (*(const unsigned char*)(def + 0x22F) != 1) { s_ghostDiag.posNone++; return; }
    if ((const unsigned char*)rd != s_wire2cRd || !s_wire2cCopy ||
        (const unsigned char*)(size_t)rd[0] != s_wire2cCopy) {
        s_ghostDiag.posUnbound++;
        return;
    }
    b.data = (const unsigned int*)(size_t)rd[0];
    b.word = rd[1];
    b.bit  = rd[2];
    if (b.bit >= 32u) { s_ghostDiag.posNone++; return; }
    pos   = wire_pos((const unsigned char*)rd);
    avail = pos < s_wire2cEnd ? s_wire2cEnd - pos : 0u;
    kind = ghost_payload_pos(b, avail, (*(const unsigned int*)(def + 0x241) >> 11) & 1u, &x, &z,
                             &x16, &y16, &z16);
    if (kind == GHOST_POS_SHORT) {
        s_ghostDiag.posShort++;
        if (InterlockedIncrement(&s_ghostDiag.notes) <= GHOST_NOTES)
            tagpu_logf("enginefix: ghost commander: dirty create slot %u: reading its move "
                       "payload's position would pass the message's end (bit %u of %u); stock's "
                       "record kept",
                       rec[3] | (unsigned int)rec[4] << 8, (unsigned int)pos,
                       (unsigned int)s_wire2cEnd);
        return;
    }
    if (kind == GHOST_POS_NONE) { s_ghostDiag.posNone++; return; }
    if (!ghost_on_map(x, z, *(const unsigned int*)(ta + 0x14233),
                      *(const unsigned int*)(ta + 0x14237))) {
        s_ghostDiag.posOff++;
        return;
    }
    memcpy(&sx, rec + 5, 4);
    memcpy(&sz, rec + 13, 4);
    memcpy(rec + 5, &x16, 4);
    memcpy(rec + 13, &z16, 4);
    if (kind == GHOST_POS_AIR) {
        /* y up to 0x1FF px, the ceiling the goal point 0x44E3C0 clamps its y to (0x44E4F1) */
        if (y16 <= 0x01FF0000u) memcpy(rec + 9, &y16, 4);
        s_ghostDiag.posAir++;
    } else {
        s_ghostDiag.posGround++;
    }
    if (InterlockedIncrement(&s_ghostDiag.notes) <= GHOST_NOTES)
        tagpu_logf("enginefix: ghost commander: dirty create slot %u type %u at (%d, %d) from its "
                   "%s payload; stock's record had (%d, %d)", rec[3] | (unsigned int)rec[4] << 8,
                   type, x, z, kind == GHOST_POS_AIR ? "air" : "ground", (int)sx >> 16,
                   (int)sz >> 16);
}

/* ---- stubs, in B5's own page ------------------------------------------------------------- */

static unsigned char* s_ghostCode;
SIM_CODE(s_ghostCode);
static unsigned int   s_ghostCodeUsed;
SIM_VAR(s_ghostCodeUsed, SIM_INSTALL);

static unsigned char* ghost_code(unsigned int n)
{
    unsigned char* p;
    if (!s_ghostCode)
        s_ghostCode = (unsigned char*)SIM_ALLOC(s_ghostCode, 0x1000);
    if (!s_ghostCode || s_ghostCodeUsed + n > 0x1000) return NULL;
    p = s_ghostCode + s_ghostCodeUsed;
    s_ghostCodeUsed += (n + 15u) & ~15u;
    return p;
}

static void ghost_selfcheck(void)
{
    /* a ground payload: flag 1, n = 2, (1234, 567), (40, 50); an air one: selector 2, flag 0,
       x = 3000.5 px, y = 80 px, z = 12 px; both least significant bit first, as 0x415C10
       writes them */
    static const unsigned int ground[3] = { 0x11B82695u, 0x01900140u, 0x00000000u };
    static const unsigned int air[4]    = { 0x5DC40002u, 0x02800000u, 0x00600000u, 0x00000000u };
    static const unsigned int split[2]  = { 0x80000000u, 0x00000005u };
    static const unsigned int still[1]  = { 0x00000001u };        /* flag 1, n = 0 */
    struct { const char* name; int got; int want; } t[24];
    GHOSTREC q[4];
    int n = 0, bad = 0, i, x, z;
    unsigned int x16, y16, z16;
    GHOSTBITS b;
    memset(q, 0, sizeof q);                  /* held creates for slots 7, 9, 7, 5; the last 7 dead */
    q[0].m[6] = 7; q[1].m[6] = 9; q[2].m[6] = 7; q[3].m[6] = 5;
    t[n].name = "newer: empty slot";         t[n].got = ghost_newer(0, 50, 3);                 t[n].want = 1; n++;
    t[n].name = "newer: exact, later";       t[n].got = ghost_newer(1, 50, 51);                t[n].want = 1; n++;
    t[n].name = "newer: exact, same";        t[n].got = ghost_newer(1, 50, 50);                t[n].want = 0; n++;
    t[n].name = "newer: bound, later";       t[n].got = ghost_newer(1, HIT_LB | 50, 51);       t[n].want = 1; n++;
    t[n].name = "newer: bound, at g0";       t[n].got = ghost_newer(1, HIT_LB | 50, 50);       t[n].want = 0; n++;
    t[n].name = "newer: unknown birth";      t[n].got = ghost_newer(1, 50, HIT_UNKNOWN);       t[n].want = 0; n++;
    t[n].name = "map: corners";              t[n].got = ghost_on_map(0, 0, 2, 3) && ghost_on_map(31, 47, 2, 3); t[n].want = 1; n++;
    t[n].name = "map: past an edge";         t[n].got = ghost_on_map(32, 0, 2, 3) || ghost_on_map(0, 48, 2, 3) ||
                                                        ghost_on_map(-1, 0, 2, 3);             t[n].want = 0; n++;
    t[n].name = "map: no map";               t[n].got = ghost_on_map(0, 0, 0, 3) || ghost_on_map(0, 0, 4097, 3); t[n].want = 0; n++;
    b.data = split; b.word = 0; b.bit = 31;
    t[n].name = "bits: across a word";       t[n].got = ghost_bits(&b, 4) == 0xBu && b.word == 1 && b.bit == 3; t[n].want = 1; n++;
    b.data = ground; b.word = 0; b.bit = 0;
    t[n].name = "ground: point 0";           t[n].got = ghost_payload_pos(b, 96, 0, &x, &z, &x16, &y16, &z16) == GHOST_POS_GROUND &&
                                                        x == 1234 && z == 567 && x16 == 1234u << 16; t[n].want = 1; n++;
    b.data = air; b.word = 0; b.bit = 0;
    t[n].name = "air: the motion's point";   t[n].got = ghost_payload_pos(b, 128, 1, &x, &z, &x16, &y16, &z16) == GHOST_POS_AIR &&
                                                        x == 3000 && z == 12 && y16 == 80u << 16; t[n].want = 1; n++;
    b.data = air; b.word = 0; b.bit = 1;
    t[n].name = "air: a goal is not taken";  t[n].got = ghost_payload_pos(b, 127, 1, &x, &z, &x16, &y16, &z16); t[n].want = GHOST_POS_NONE; n++;
    b.data = ground; b.word = 0; b.bit = 0;
    t[n].name = "ground: one bit short";     t[n].got = ghost_payload_pos(b, 34, 0, &x, &z, &x16, &y16, &z16); t[n].want = GHOST_POS_SHORT; n++;
    t[n].name = "ground: the read ends at end"; t[n].got = ghost_payload_pos(b, 35, 0, &x, &z, &x16, &y16, &z16); t[n].want = GHOST_POS_GROUND; n++;
    b.data = still; b.word = 0; b.bit = 0;
    t[n].name = "ground: no point, at end";  t[n].got = ghost_payload_pos(b, 3, 0, &x, &z, &x16, &y16, &z16); t[n].want = GHOST_POS_NONE; n++;
    b.data = air; b.word = 0; b.bit = 0;
    t[n].name = "air: one bit short";        t[n].got = ghost_payload_pos(b, 98, 1, &x, &z, &x16, &y16, &z16); t[n].want = GHOST_POS_SHORT; n++;
    t[n].name = "kill: the latest create";   t[n].got = ghost_cancel(q, 4, 7);                 t[n].want = 2; n++;
    q[2].dead = 1;
    t[n].name = "kill: past a cancelled one"; t[n].got = ghost_cancel(q, 4, 7);                t[n].want = 0; n++;
    t[n].name = "kill: nothing held";        t[n].got = ghost_cancel(q, 4, 8);                 t[n].want = -1; n++;
    t[n].name = "kill: the high byte";       t[n].got = ghost_cancel(q, 4, 7 + 256);           t[n].want = -1; n++;
    for (i = 0; i < n; i++) {
        if (t[i].got != t[i].want) bad++;
        tagpu_logf("enginefix: ghostcheck %-26s got=%d want=%d %s",
                   t[i].name, t[i].got, t[i].want, t[i].got == t[i].want ? "OK" : "FAIL");
    }
    tagpu_logf("enginefix: ghostcheck %d rule cases, %d failed", n, bad);
}

/* `wire` is fix_wire_bounds' result: the dirty create's position is read through B3's copy
   of the 0x2C, so B5 requires B3 armed. B3 stays a local fix for its own purpose; a peer where
   it did not arm would place dirty creates at the slot's stale position while the others take
   the payload's, so the table is refused and the process ends through the report. */
static int fix_ghost_commander(int wire)
{
    static const unsigned char firstCall[10] = { 0xF6, 0xC1, 0x01, 0x57, 0x0F, 0x85, 0xE2, 0x03,
                                                 0x00, 0x00 };                   /* 0x497F54 */
    static const unsigned char loadStart[6]  = { 0x8B, 0x88, 0x31, 0x05, 0x00, 0x00 };
    static const unsigned char loadNext[2]   = { 0x33, 0xED };                    /* 0x497F64 */
    static const unsigned char loaderDone[3] = { 0x83, 0xC9, 0x02 };              /* 0x497C5F */
    static const unsigned char doneTest[5]   = { 0xD0, 0xEA, 0xF6, 0xC2, 0x01 };  /* 0x498348 */
    static const unsigned char dirtyFrame[11] = { 0x8D, 0x4C, 0x24, 0x1C, 0x51, 0x52, 0x66, 0x89,
                                                  0x44, 0x24, 0x39 };            /* 0x48B9F5 */
    static const unsigned char dirtyCall[5]  = { 0xE8, 0xCB, 0xA7, 0xFF, 0xFF };  /* 0x48BA00 */
    static const unsigned char cfnEntry[3]   = { 0x83, 0xEC, 0x0C };              /* 0x4861D0 */
    static const unsigned char frameCall[5]  = { 0xE8, 0x5C, 0xE3, 0xFF, 0xFF };  /* 0x49842F */
    static const unsigned char frameEntry[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };  /* 0x496790 */
    static const unsigned char refused[9]    = { 0x83, 0xFA, 0x05, 0x0F, 0x84, 0xC8, 0x17, 0x00,
                                                 0x00 };                         /* 0x45477F */
    static const unsigned char notState5[9]  = { 0x83, 0xFA, 0x06, 0x0F, 0x84, 0xBF, 0x17, 0x00,
                                                 0x00 };                         /* 0x454788 */
    static const unsigned char nextSub[7]    = { 0x8B, 0x84, 0x24, 0xF0, 0x00, 0x00, 0x00 };
    /* 0x48B933: the 0x2C reader's buffer, word and bit, [esp+0x14..0x1C] */
    static const unsigned char readerSlots[12] = { 0x89, 0x44, 0x24, 0x14, 0x89, 0x74, 0x24, 0x18,
                                                   0x89, 0x74, 0x24, 0x1C };
    /* 0x453E84: edi = main + 0x1B63 + (byte [esp+0x14]) * 0x14B, the sender's record */
    static const unsigned char senderRec[30] = { 0x8B, 0x74, 0x24, 0x14, 0x81, 0xE6, 0xFF, 0x00,
                                                 0x00, 0x00, 0x8B, 0xC6, 0xC1, 0xE0, 0x05, 0x03,
                                                 0xC6, 0x8D, 0x0C, 0x32, 0x8D, 0x04, 0x80, 0x8D,
                                                 0xBC, 0x41, 0x63, 0x1B, 0x00, 0x00 };
    unsigned char *aCall, *aReset, *aPos, *aEarly, *aRefused, *p;

    InitializeCriticalSection(&s_ghostLock);
    s_ghostGameTid = GetCurrentThreadId();
    s_ghostOff = tagpu_sim_lever("tagpu_ghostq.off");
    if (s_ghostOff)
        tagpu_logf("enginefix: ghost commander: TEST LEVER tagpu_ghostq.off -- nothing is held; "
                   "the dirty create's position still applies");
    if (tagpu_sim_lever("tagpu_wirecheck.on")) ghost_selfcheck();
    if (wire != FIX_ARMED) {
        lim_needs("the ghost commander (a simulation fix)", "wire robustness", fix_state(wire));
        return FIX_TABLE;
    }

    if (!(aCall = ghost_code(32)) || !(aReset = ghost_code(32)) || !(aPos = ghost_code(32)) ||
        !(aEarly = ghost_code(32)) || !(aRefused = ghost_code(32))) {
        lim_no_stub();
        return FIX_TABLE;
    }

    /* the replay: CreateFromNetwork(arg, rec) with the sender in edi, as the 0x09 case has it
       (CreateFromNetwork pushes edi at 0x4861EC, where B3's observe reads the sender) */
    p = aCall;
    *p++ = 0x57;                                         /* push edi           */
    *p++ = 0x8B; *p++ = 0x7C; *p++ = 0x24; *p++ = 0x10;  /* mov edi,[esp+0x10] */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x0C;  /* push [esp+0xC]: rec */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x0C;  /* push [esp+0xC]: arg */
    p = hit_jmp(p, 0xE8, 0x004861D0);
    s_ghostRet = (unsigned int)(size_t)p;
    *p++ = 0x5F;                                         /* pop edi            */
    *p++ = 0xC2; *p++ = 0x0C; *p++ = 0x00;               /* ret 0xC            */
    s_ghostCall = (GHOSTCALL)(void*)aCall;

    /* 0x497F5E: the reset, then stock's mov ecx,[eax+0x531] */
    p = fix_call_regs(aReset, ghost_reset);
    memcpy(p, loadStart, 6); p += 6;
    hit_jmp(p, 0xE9, 0x00497F64);

    /* 0x49842F: the drain, then the frame function under the return 0x498434 */
    p = fix_call_regs(aEarly, ghost_replay);
    hit_jmp(p, 0xE9, 0x00496790);

    /* 0x45477F: stock's `cmp edx,5; je 0x455F50`, the refusal in state 5 noted first */
    p = aRefused;
    *p++ = 0x83; *p++ = 0xFA; *p++ = 0x05;               /* cmp edx,5          */
    p = hit_jcc(p, 0x85, 0x00454788);
    p = fix_call_regs(p, ghost_refused);
    hit_jmp(p, 0xE9, 0x00455F50);

    /* 0x48BA00: the position into the record, then CreateFromNetwork under 0x48BA05 */
    p = fix_call_regs(aPos, ghost_position);
    hit_jmp(p, 0xE9, 0x004861D0);

    hit_site(0x00497F5E, 6, loadStart, 0xE9, aReset, "ghost commander: the queue emptied at the load's start");
    hit_site(0x0049842F, 5, frameCall, 0xE8, aEarly, "ghost commander: the queue replayed before the first tick");
    hit_site(0x0045477F, 9, refused, 0xE9, aRefused, "ghost commander: a kill refused in state 5");
    hit_site(0x0048BA00, 5, dirtyCall, 0xE8, aPos, "ghost commander: the dirty create's position");
    lim_same(0x00497F54, 10, firstCall, "ghost commander: the load state's first-call test");
    lim_same(0x00497F64, 2, loadNext, "ghost commander: the load state's continuation");
    lim_same(0x00497C5F, 3, loaderDone, "ghost commander: the loader's last store, bit 1");
    lim_same(0x00498348, 5, doneTest, "ghost commander: the game thread's test of bit 1");
    lim_same(0x0048B9F5, 11, dirtyFrame, "ghost commander: the dirty create's record and reader");
    lim_same(0x004861D0, 3, cfnEntry, "ghost commander: CreateFromNetwork's entry");
    lim_same(0x00496790, 5, frameEntry, "ghost commander: the frame function's entry");
    lim_same(0x00454788, 9, notState5, "ghost commander: the refusal's other states");
    lim_same(0x00455F50, 7, nextSub, "ghost commander: the dispatcher's next message");
    lim_same(0x0048B933, 12, readerSlots, "ghost commander: the 0x2C reader's slots");
    lim_same(0x00453E84, 30, senderRec, "ghost commander: the sender's record and argument");
    return FIX_TABLE;
}

SIM_FEATURE(transported, SIM_PUPPET_RUNS, "c4-kill-all");

/* ===== C4: TRANSPORTED EXPLOSIONS ===========================================================
   Section C's fourth landing (research/notes/tadr-port/data-keys.md, C4): TransportedExplodeAs=
   and TransportedSelfDestructAs=, TADR's keys, read and resolved by tagpu_datakeys.c.

   THE ENGINE [DISASSEMBLED]. A unit's death explosion is 0x49B000(unit, selfd), which takes
   def+0x224 (SelfDestructAs) when selfd is set and def+0x220 (ExplodeAs) otherwise
   (0x49B017..0x49B021); its damage is computed only on the dying unit's owner's peer (0x49A043),
   a picture on every other. Its two callers: the destructor 0x4866D0 at 0x486D50, selfd = (the
   record's kind == 3), on every peer; and kill-all 0x486F10 at 0x486F9E, selfd = 1, for each
   unit of a player that is not local (a player who left, 0x452E17). The destructor's two
   callers: Send_UnitDeath 0x4864B0 at 0x486679, on the peer that runs the death (the reaper of
   a local unit 0x48AFC9, kill-all's non-local branch 0x486FB4, the teardown), and the 0x0C case
   at 0x455423 on every other peer -- which this build reaches only through B8's 0x4C (B4's
   doDeath), a bare 0x0C being dropped. `unit+0x86` is the transporter while the unit is carried.
   The destructor detaches its unit (0x4867BA, 0x4867CB) before its explosion, and a dying
   transport's cargo loop (0x4867D0..0x48682A) hits each passenger with 30 000 of kind 6 (kind 3
   when the transport self-destructs) and detaches it: when 0x49B017 runs from the destructor,
   +0x86 is already 0, so "carried" is decided before.

   THE DECISION, once a death, where the death starts: 0x48664B, in Send_UnitDeath after its
   0x0C record is complete and before the send (0x48666D) and the destructor (0x486679). A
   keyed unit's death is CARRIED when
     - +0x86 is set: it dies in its transport;
     - the record's kind is 6, which only the cargo loop passes (the one caller of 0x489BB0
       with 6): its transport died and its hit killed it;
     - or the cargo loop marked it: a keyed passenger that was already dying (pending death,
       bit 14 of +0x110: the owner's hit sets it, 0x489EE2, and on a copy kill-all's non-local
       branch, 0x486FAE, and the ghost sweeps, the engine's 0x48B42C and B5's) when its
       transport's destructor detached it, whatever its kind -- 0x489D45 refuses the loop's hit
       on a pending unit, and a
       self-destructing transport's hit is kind 3. The mark is the passenger's birth stamp + 1
       (B4's table: exact and strictly increasing per slot for a local unit), so it holds only
       for the incarnation it was taken for; the decision consumes it, and hit_reset empties
       every mark with the stamps at the unit array's allocation.
   The answer lives in s_txCarried[idx], idx = rec+1, for exactly one destructor call: tx_wrap,
   which makes every destructor call, reads idx before the call and clears it when the call
   returns. The destructor finds its unit by rec+1 (0x4866E0, B3's wire_s0c), and the pick at
   0x49B017 reads the flag by the slot of the unit it is given, which is that slot.

   ON THE WIRE. The owner's answer travels as m[19] of B8's 0x4C (kill_tx_death), since a
   passenger's copy on another peer has been detached by its transport's death, sent first.
   The receiver arms it for the destructor call the 0x4C makes (tx_rx_death, then doDeath ->
   tx_wrap). A 0x4C refused while the receiver loads reaches no destructor then: when B5 marks
   the copy dying instead, the answer waits as a mark on that copy's stamp (tx_rx_swept), for
   tx_arm when the unit tick destroys it. The blast's damage is the owner's alone (0x49A043),
   so what m[19] decides elsewhere is the explosion drawn.

   KILL-ALL [DISASSEMBLED]. Its non-local branch detonates each unit in slot order (0x486F9E),
   then destroys it through Send_UnitDeath, whose destructor runs the cargo loop: a transport
   earlier in the block detaches its passengers before their turn, and they would then detonate
   as not carried. So each outermost kill-all draws a token of its own (tx_kill_all), the cargo
   loop marks with it every keyed passenger it detaches inside, and the pick honours a mark only
   inside the kill-all that wrote it. With +0x86 for a passenger whose turn comes first, every
   carried unit of the player detonates carried. Both detaches the destructor makes, a carried
   unit's own (0x4867CB) and the cargo loop's (0x48681D), broadcast a 0x0A (0x48AAC0), and with
   three peers or more the first to run the player removal (0x452E17) would detach the departed
   player's units on the others before their own kill-all, whichever of a pair comes first in
   the block: inside kill-all, a detach made by the destructor of a unit whose player is not
   local is applied without it (tx_detach), so each peer decides on its own copy alone. A
   departed player's blasts damage nothing (the owner gate 0x49A043), so what that decides is
   the explosion drawn.

   THE FOLD: tagpu_datakeys.c, tagpu_datakeys_tx_fold, at the menu-time loader's last store.

   THREADS: a death, its cargo loop and its pick run on the thread that runs the death -- the
   game thread in play, and during a network load either thread that pumps: the pump 0x453D40
   holds no lock across its dispatch, and both the loader (0x49727D) and the game thread's load loop (0x49852E)
   reach its leave case and the removal's kill-all. So the running kill-all is the thread's own:
   its token lives in a TLS slot, drawn from an interlocked counter, and a mark matches only the
   kill-all that wrote it. Two threads' kill-alls never honour each other's marks, and a mark
   left behind matches no later kill-all until the 32-bit counter has turned, so nothing is
   emptied. The unit array's allocation (hit_reset) runs on the loader thread in the level load,
   before play, the ordering B4's tables rest on. Every index is a u16 or bounded by B4's table,
   and the arrays hold 65 536 entries, so no value read from the engine addresses outside them.

   CLASS: simulation, fail closed -- a peer without it computes a keyed unit's carried blast
   with ExplodeAs's damage. Every site is a row of the fail-closed table. */

static unsigned char  s_txCarried[65536];    /* the running destructor call's decision        */
SIM_VAR(s_txCarried, SIM_TRANSIENT);
/* The two mark tables are heap blocks of TX_ROWS entries, taken by fix_transported and never
   freed: a heap table's pages are write-watched, so the demo recorder images only the ones
   written rather than comparing 512 KB every iteration (tagpu_recorder.c). */
#define TX_ROWS 65536
static unsigned int*  s_txMark;              /* a dying passenger's birth stamp + 1, 0 = none */
SIM_BLOCK(s_txMark, SIM_STATE, SIM_ID_UNIT_SLOT, TX_ROWS, NULL);
static unsigned int*  s_txKillMark;          /* the token of the kill-all that detached it    */
SIM_BLOCK(s_txKillMark, SIM_STATE, SIM_ID_UNIT_SLOT, TX_ROWS, NULL);
static volatile LONG  s_txKillGen;           /* the last token drawn                          */
SIM_VAR(s_txKillGen, SIM_STATE);
static DWORD          s_txKillTls = TLS_OUT_OF_INDEXES;  /* the thread's kill-all token, 0 none */
SIM_TLS(s_txKillTls, SIM_TRANSIENT);
typedef int (__stdcall *PFN_KillAll)(unsigned int player);
static PFN_KillAll    s_txKillTramp;         /* kill-all's first ten bytes, then 0x486F1A     */
SIM_VAR(s_txKillTramp, SIM_INSTALL);
static struct {
    unsigned int decided, marked, killMarked, picked, received, detachedHere;
} s_txDiag;
SIM_VAR(s_txDiag, SIM_PRIVATE);
static unsigned int   s_txWrapRet;           /* tx_wrap's return point inside the destructor  */
SIM_VAR(s_txWrapRet, SIM_INSTALL);

/* B4's hit_reset calls this at every level's unit-array allocation, installed or not */
static void tx_reset(void)
{
    if (s_txMark) memset(s_txMark, 0, TX_ROWS * sizeof *s_txMark);
}

static void tx_event(unsigned int* n, const char* what, const char* unit, const char* weapon)
{
    unsigned int c = ++*n;
    if ((c & (c - 1)) == 0)
        tagpu_logf("enginefix: transported: %s%s%.12s%s%.32s (%u so far)", what,
                   unit ? " " : "", unit ? unit : "", weapon ? " -> " : "",
                   weapon ? weapon : "", c);
}

/* a keyed unit's type name, def+0x20: tagpu_datakeys_tx_keyed has matched its def to a row */
static const char* tx_name(const char* unit)
{
    return *(const char* const*)(unit + 0x92) + 0x20;
}

/* the token of the kill-all running on this thread, 0 outside one */
static unsigned int tx_kill_token(void)
{
    return (unsigned int)(size_t)TlsGetValue(s_txKillTls);
}

static int tx_mark_held(unsigned int idx)
{
    const struct hit_tab* t = s_hit;
    return t && idx < t->n && !(t->stamp[idx] & HIT_LB) && s_txMark[idx] == t->stamp[idx] + 1u;
}

/* 0x48664B: Send_UnitDeath's record complete -- rec+1 the unit's index, copied from its +0xA8
   (0x4865ED, 0x4865FC); rec+0xA the kind in its high nibble (0x48661C..0x486621) */
static void __stdcall tx_arm(const char* unit, const unsigned char* rec)
{
    unsigned int idx = rec[1] | (unsigned int)rec[2] << 8;
    int carried = 0;
    if (tagpu_datakeys_tx_keyed(unit))
        carried = *(const unsigned int*)(unit + 0x86) != 0 || (rec[0xA] >> 4) == 6 ||
                  tx_mark_held(idx);
    s_txMark[idx] = 0;
    s_txCarried[idx] = (unsigned char)carried;
    if (carried) tx_event(&s_txDiag.decided, "a keyed death decided carried:", tx_name(unit), NULL);
}

/* 0x486810: the cargo loop's passenger (the transport's +0x8A), just hit by 0x48680B */
static void __stdcall tx_cargo(const char* passenger)
{
    const struct hit_tab* t = s_hit;
    const char* ta = *(const char* const*)0x00511DE8;
    unsigned int idx, tok;
    if (!hit_slot_index(t, ta, passenger, &idx) || !tagpu_datakeys_tx_keyed(passenger)) return;
    if ((*(const unsigned int*)(passenger + 0x110) & 0x4000u) && !(t->stamp[idx] & HIT_LB)) {
        s_txMark[idx] = t->stamp[idx] + 1u;
        tx_event(&s_txDiag.marked, "a dying keyed passenger marked", NULL, NULL);
    }
    tok = tx_kill_token();
    if (tok && s_txKillMark[idx] != tok) {
        s_txKillMark[idx] = tok;
        tx_event(&s_txDiag.killMarked, "a keyed passenger detached inside kill-all", NULL, NULL);
    }
}

typedef void (__stdcall *PFN_Attach)(const unsigned char* rec);
#define E_Attach ((PFN_Attach)0x0048AB70u)   /* the attach record's apply, ret 4 */

/* The destructor's two detaches, each `call 0x48AAC0(child, 0, -1, 1)` with the unit whose
   destructor runs in esi: a carried unit's own (0x4867CB, the child that unit) and the cargo
   loop's (0x48681D, the child its passenger). 1 when the detach was applied here without its
   broadcast, 0 for the stock wrapper. Inside kill-all, a dying unit whose player is not local
   (kill-all's own test, 0x486F73..0x486F87) belongs to a player every peer is destroying on its
   own copy; the wrapper's 0x0A would reach a peer that has not yet run its own kill-all and
   detach the passenger there first, so that peer's pick would see an uncarried unit. Applied
   here only, each peer's decision rests on its own copy of the departed player's units. The
   record is the wrapper's own (0x48AB0F..0x48AB4E: 0x0A, the child's +0xA8, parent 0, then -1
   and 1), and only when the wrapper would apply it: the child alive, not bit 29, carrying
   nothing (0x48AAC7..0x48AAED). */
static int __stdcall tx_detach(const char* child, const char* dying)
{
    const unsigned char* pl;
    unsigned char rec[7];
    unsigned int f;
    if (!tx_kill_token()) return 0;
    pl = *(const unsigned char* const*)(dying + 0x96);
    if (!pl || (*(const unsigned int*)pl && (pl[0x73] == 1 || pl[0x73] == 2))) return 0;
    f = *(const unsigned int*)(child + 0x110);
    if (!(f & 0x10000000u) || (f & 0x20000000u) || *(const unsigned int*)(child + 0x8A)) return 0;
    rec[0] = 0x0A;
    memcpy(rec + 1, child + 0xA8, 2);
    rec[3] = rec[4] = 0;
    rec[5] = 0xFF;
    rec[6] = 1;
    E_Attach(rec);
    tx_event(&s_txDiag.detachedHere, "a departed player's passenger detached here, not broadcast",
             NULL, NULL);
    return 1;
}

/* 0x49B017: the weapon a carried keyed unit explodes with, or NULL for stock's pick */
static const char* __stdcall tx_pick(const char* unit, int selfd)
{
    const struct hit_tab* t = s_hit;
    const char* ta = *(const char* const*)0x00511DE8;
    const char* w;
    unsigned int idx;
    int carried = *(const unsigned int*)(unit + 0x86) != 0;
    if (!carried && hit_slot_index(t, ta, unit, &idx))
        carried = s_txCarried[idx] || (s_txKillMark[idx] && s_txKillMark[idx] == tx_kill_token());
    if (!carried || !(w = tagpu_datakeys_tx_weapon(unit, selfd))) return NULL;
    tx_event(&s_txDiag.picked, selfd ? "a carried self-destruct:" : "a carried death:",
             tx_name(unit), w);
    return w;
}

int tagpu_tx_carried(const char* unit)
{
    const char* ta = *(const char* const*)0x00511DE8;
    unsigned int idx;
    return hit_slot_index(s_hit, ta, unit, &idx) &&
           (s_txCarried[idx] || (s_txKillMark[idx] && s_txKillMark[idx] == tx_kill_token()));
}

/* 0x486F10, kill-all(player), stdcall ret 4, entered by the site's jump with kill-all's own
   frame: the outermost call on a thread draws the token its detaches are marked with (never 0,
   the "none" of the TLS slot); a nested one runs under its caller's. */
static int __stdcall tx_kill_all(unsigned int player)
{
    unsigned int tok = tx_kill_token();
    int r;
    if (tok) return s_txKillTramp(player);
    do tok = (unsigned int)InterlockedIncrement(&s_txKillGen); while (!tok);
    TlsSetValue(s_txKillTls, (void*)(size_t)tok);
    r = s_txKillTramp(player);
    TlsSetValue(s_txKillTls, NULL);
    return r;
}

/* tx_wrap(rec, mode), the destructor with its own signature: mov eax,[esp+4];
   movzx eax,word [eax+1]; push eax; push [esp+0xC]; push [esp+0xC]; call 0x4866D0;
   xchg eax,[esp]; mov byte [eax+s_txCarried],0; pop eax; ret 8 -- the record's index is read
   before the call, and the flag it names is clear whenever no destructor call made through
   here is running. Every destructor call this build makes goes through it: Send_UnitDeath's
   (0x486679) and the 0x4C's (B4's doDeath, which jumps here with 0x455428 as the return);
   the stock 0x0C case 0x455423 never runs, since a bare 0x0C is dropped (B8). Built on first
   use, so that B4's stub, built before this section's sites, can name it. */
static unsigned char* tx_wrap_code(void)
{
    static unsigned char* wrap;
    SIM_VAR(wrap, SIM_INSTALL);
    unsigned char* p;
    if (wrap || !(p = fix_code(48))) return wrap;
    wrap = p;
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x04;
    *p++ = 0x0F; *p++ = 0xB7; *p++ = 0x40; *p++ = 0x01;
    *p++ = 0x50;
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x0C;
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x0C;
    p = wk_jmp(p, 0xE8, 0x004866D0u);
    s_txWrapRet = (unsigned int)(size_t)p;
    *p++ = 0x87; *p++ = 0x04; *p++ = 0x24;
    *p++ = 0xC6; *p++ = 0x80; p = hit_abs(p, s_txCarried); *p++ = 0x00;
    *p++ = 0x58;
    *p++ = 0xC2; *p++ = 0x08; *p++ = 0x00;
    return wrap;
}

/* The destructor's return address as its caller left it, `ret` pointing at the slot: past
   tx_wrap's frame (idx, mode, rec, then tx_wrap's own return) when tx_wrap made the call */
static unsigned int tx_dtor_caller(const unsigned char* ret)
{
    unsigned int a = *(const unsigned int*)ret;
    return s_txWrapRet && a == s_txWrapRet ? *(const unsigned int*)(ret + 0x10) : a;
}

/* A 0x4C accepted in state 6 (kill_rx_death, on the game thread), m[3..13] its record: the
   owner's decision, m[19], armed for the one destructor call doDeath makes next through
   tx_wrap, which clears it. The receiver's own +0x86 cannot say it: the transport's 0x4C,
   sent first, has already detached the passenger here. */
static void tx_rx_death(const unsigned char* m)
{
    s_txCarried[m[4] | (unsigned int)m[5] << 8] = (unsigned char)(m[19] == 1);
    if (m[19] == 1) tx_event(&s_txDiag.received, "a carried death received", NULL, NULL);
}

int tagpu_tx_replay_death(const unsigned char* rec, int carried, int own)
{
    typedef void (__stdcall *PFN_Wrap)(const unsigned char* rec, unsigned mode);
    unsigned char* wrap = tx_wrap_code();
    if (!wrap) return 0;
    s_txCarried[rec[1] | (unsigned int)rec[2] << 8] = (unsigned char)(carried == 1);
    s_txKilledQuery = (unsigned char)(own != 0);        /* THE KILLED SCRIPT OF A REPLAYED DEATH */
    ((PFN_Wrap)(void*)wrap)(rec, 0);
    s_txKilledQuery = 0;
    return 1;
}

/* A carried 0x4C refused in state 5 whose copy ghost_refused_kill has marked dying, on the
   game thread: the unit tick destroys that copy later through Send_UnitDeath, where tx_arm
   decides, so the owner's decision waits as a mark on the copy's exact stamp, which
   ghost_sweep has just matched to the create it replayed. */
static void tx_rx_swept(const unsigned char* m)
{
    const struct hit_tab* t = s_hit;
    unsigned int idx = m[4] | (unsigned int)m[5] << 8;
    /* B4's receiver runs whether or not fix_transported installed, and only its install
       allocates the marks: a peer without it has no mark to keep */
    if (m[19] != 1 || !t || !s_txMark || idx >= t->n || (t->stamp[idx] & HIT_LB)) return;
    s_txMark[idx] = t->stamp[idx] + 1u;
    tx_event(&s_txDiag.marked, "a carried death refused while loading, its copy marked", NULL, NULL);
}

/* The sites. Each stub's registers are the site's own [DISASSEMBLED]:
   - 0x49B017 (16 bytes, the pick; nothing lands inside them but their own jumps): ecx the
     unit, eax its def, edx 0, and selfd at [esp+0x7C] (0x49B000's frame: sub esp,0x6C, then
     esi and edi pushed). The stub re-tests selfd for the stock pick, since the site's je
     tested the flags of 0x49B00D.
   - 0x48664B (mov eax,[esi+0x96]): esi the unit, the record at [esp+0x14] (0x486672 passes
     it); 0x48663D jumps to the site's first byte.
   - 0x486679 (call 0x4866D0): tx_wrap has the destructor's signature, stdcall (rec, mode),
     ret 8. eax holds the record there and nothing reads it after the call (0x48667E on).
   - 0x4867CB (call 0x48AAC0, after its four pushes): esi the dying unit, which is the child;
     the stub has the wrapper's signature, stdcall (child, parent, a3, a4), ret 0x10 (0x48AB6A).
   - 0x486810 (mov ecx,[esi+0x8A]): esi the transport, after the hit's return.
   - 0x48681D (call 0x48AAC0, after its four pushes): esi still the transport; the same stub.
   - 0x486F10 (kill-all's first ten bytes): stdcall (player), ret 4 (0x486FC6).
   - 0x42B019 (mov [ebp+0x146],edx): edx the finished CRC_weapons, ecx the unit's UNITINFO
     context (0x42B00A), ebp the def, and the call 0x42B01F's three arguments already pushed.
   No branch lands inside any of them past its first byte (rel8/rel32 scan of .text and every
   dword of the image). None of these points holds a live x87 value, and the C answers are
   integer only. */
static int fix_transported(void)
{
    static const unsigned char pick[16]  = { 0x74, 0x08, 0x8B, 0x80, 0x24, 0x02, 0x00, 0x00,
                                             0xEB, 0x06, 0x8B, 0x80, 0x20, 0x02, 0x00, 0x00 };
    static const unsigned char frame[23] = { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04,
                                             0x83, 0xEC, 0x6C, 0x33, 0xD2, 0x3B, 0xC2, 0x8B,
                                             0x81, 0x92, 0x00, 0x00, 0x00, 0x56, 0x57 };  /* 0x49B000 */
    static const unsigned char picked[2] = { 0x3B, 0xC2 };                               /* 0x49B027 */
    static const unsigned char arm[6]    = { 0x8B, 0x86, 0x96, 0x00, 0x00, 0x00 };
    static const unsigned char recIdx[5] = { 0x66, 0x89, 0x54, 0x24, 0x15 };             /* 0x4865FC */
    static const unsigned char recKind[9] = { 0xC0, 0xE3, 0x04, 0x0A, 0xC3, 0x88, 0x44, 0x24,
                                              0x22 };                                    /* 0x48661C */
    static const unsigned char recArg[7] = { 0x8D, 0x44, 0x24, 0x14, 0x6A, 0x01, 0x50 }; /* 0x486672 */
    static const unsigned char dtor[5]   = { 0xE8, 0x52, 0x00, 0x00, 0x00 };             /* 0x486679 */
    static const unsigned char cargo[6]  = { 0x8B, 0x8E, 0x8A, 0x00, 0x00, 0x00 };       /* 0x486810 */
    static const unsigned char detach[5] = { 0xE8, 0x9E, 0x42, 0x00, 0x00 };             /* 0x48681D */
    static const unsigned char detArgs[7] = { 0x6A, 0x01, 0x6A, 0xFF, 0x6A, 0x00, 0x51 }; /* 0x486816 */
    static const unsigned char ownDet[5] = { 0xE8, 0xF0, 0x42, 0x00, 0x00 };             /* 0x4867CB */
    static const unsigned char ownArgs[7] = { 0x6A, 0x01, 0x6A, 0xFF, 0x6A, 0x00, 0x56 }; /* 0x4867C4 */
    static const unsigned char kill[10]  = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x15, 0xE8, 0x1D,
                                             0x51, 0x00 };                               /* 0x486F10 */
    static const unsigned char killRet[5] = { 0x5F, 0x5E, 0xC2, 0x04, 0x00 };             /* 0x486FC4 */
    static const unsigned char fold[6]   = { 0x89, 0x95, 0x46, 0x01, 0x00, 0x00 };       /* 0x42B019 */
    static const unsigned char foldTdf[4] = { 0x8B, 0x4C, 0x24, 0x1C };                  /* 0x42B00A */
    unsigned char* aPick = fix_code(64);
    unsigned char* aArm  = fix_code(32);
    unsigned char* aWrap = tx_wrap_code();
    unsigned char* aCarg = fix_code(32);
    unsigned char* aKill = fix_code(16);
    unsigned char* aFold = fix_code(32);
    unsigned char* aDet  = fix_code(32);
    unsigned char *p, *j;

    if (!tagpu_datakeys_units_install()) {
        lim_needs("transported explosions (a simulation fix)", "the unit-key reader", "not installed");
        return FIX_TABLE;
    }
    if (!aPick || !aArm || !aWrap || !aCarg || !aKill || !aFold || !aDet ||
        !SIM_TLS_ALLOC(s_txKillTls)) {
        lim_no_stub();
        return FIX_TABLE;
    }
    s_txMark = (unsigned int*)SIM_ALLOC(s_txMark, TX_ROWS * sizeof *s_txMark);
    s_txKillMark = (unsigned int*)SIM_ALLOC(s_txKillMark, TX_ROWS * sizeof *s_txKillMark);
    if (!s_txMark || !s_txKillMark) {
        SIM_FREE(s_txMark, s_txMark);
        SIM_FREE(s_txKillMark, s_txKillMark);
        s_txMark = s_txKillMark = NULL;
        lim_no_stub();
        return FIX_TABLE;
    }

    /* 0x49B017: pushad; push [esp+0x9C] (selfd); push ecx; call tx_pick; test eax,eax; je stock;
       mov [esp+0x1C],eax; popad; jmp 0x49B027; stock: popad; cmp dword [esp+0x7C],0;
       je explode; mov eax,[eax+0x224]; jmp 0x49B027; explode: mov eax,[eax+0x220];
       jmp 0x49B027 */
    p = aPick;
    *p++ = 0x60;
    *p++ = 0xFF; *p++ = 0xB4; *p++ = 0x24; *p++ = 0x9C; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x51;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)tx_pick);
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x74; j = p++;
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x1C;
    *p++ = 0x61;
    p = wk_jmp(p, 0xE9, 0x0049B027u);
    *j = (unsigned char)(p - (j + 1));
    *p++ = 0x61;
    *p++ = 0x83; *p++ = 0x7C; *p++ = 0x24; *p++ = 0x7C; *p++ = 0x00;
    *p++ = 0x74; *p++ = 0x0B;
    memcpy(p, pick + 2, 6); p += 6;
    p = wk_jmp(p, 0xE9, 0x0049B027u);
    memcpy(p, pick + 10, 6); p += 6;
    wk_jmp(p, 0xE9, 0x0049B027u);

    /* 0x48664B: pushad; lea eax,[esp+0x34] (the record); push eax; push esi; call tx_arm;
       popad; mov eax,[esi+0x96]; jmp 0x486651 */
    p = aArm;
    *p++ = 0x60;
    *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x34;
    *p++ = 0x50; *p++ = 0x56;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)tx_arm);
    *p++ = 0x61;
    memcpy(p, arm, 6); p += 6;
    wk_jmp(p, 0xE9, 0x00486651u);

    /* 0x486810: pushad; push [esi+0x8A]; call tx_cargo; popad; mov ecx,[esi+0x8A];
       jmp 0x486816 */
    p = aCarg;
    *p++ = 0x60;
    *p++ = 0xFF; *p++ = 0xB6; *p++ = 0x8A; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)tx_cargo);
    *p++ = 0x61;
    memcpy(p, cargo, 6); p += 6;
    wk_jmp(p, 0xE9, 0x00486816u);

    /* 0x4867CB and 0x48681D, in place of `call 0x48AAC0` with its stack: pushad; push esi
       (the unit whose destructor runs); push [esp+0x28] (the child); call tx_detach;
       test eax,eax; popad; jnz done; jmp 0x48AAC0; done: ret 0x10 -- popad leaves the flags,
       and the stock path is a tail jump with the call's own frame */
    p = aDet;
    *p++ = 0x60;
    *p++ = 0x56;
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x28;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)tx_detach);
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x61;
    *p++ = 0x75; *p++ = 0x05;
    p = wk_jmp(p, 0xE9, 0x0048AAC0u);
    *p++ = 0xC2; *p++ = 0x10; *p++ = 0x00;

    /* kill-all's tramp, which tx_kill_all calls: its first ten bytes, position-independent
       (a stack load and an absolute one); jmp 0x486F1A */
    memcpy(aKill, kill, 10);
    wk_jmp(aKill + 10, 0xE9, 0x00486F1Au);
    s_txKillTramp = (PFN_KillAll)(void*)aKill;

    /* 0x42B019: pushad; push ecx; push edx; call tagpu_datakeys_tx_fold;
       mov [esp+0x14],eax (popad's edx); popad; mov [ebp+0x146],edx; jmp 0x42B01F */
    p = aFold;
    *p++ = 0x60;
    *p++ = 0x51; *p++ = 0x52;
    p = wk_jmp(p, 0xE8, (unsigned int)(size_t)tagpu_datakeys_tx_fold);
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x14;
    *p++ = 0x61;
    memcpy(p, fold, 6); p += 6;
    wk_jmp(p, 0xE9, 0x0042B01Fu);

    hit_site(0x0049B017, 16, pick, 0xE9, aPick, "transported: the pick of the death explosion");
    hit_site(0x0048664B, 6, arm, 0xE9, aArm, "transported: the decision, before the send");
    hit_site(0x00486679, 5, dtor, 0xE8, aWrap, "transported: the destructor call, bracketed");
    hit_site(0x00486810, 6, cargo, 0xE9, aCarg, "transported: the cargo loop's passenger");
    hit_site(0x004867CB, 5, ownDet, 0xE8, aDet, "transported: a carried unit's own detach, in kill-all");
    hit_site(0x0048681D, 5, detach, 0xE8, aDet, "transported: the cargo loop's detach, in kill-all");
    hit_site(0x00486F10, 10, kill, 0xE9, (const void*)tx_kill_all, "transported: kill-all, a token a thread");
    hit_site(0x0042B019, 6, fold, 0xE9, aFold, "transported: the keys' weapons in the sync");
    lim_same(0x0049B000, 23, frame, "transported: the explosion's frame and selfd");
    lim_same(0x0049B027, 2, picked, "transported: the pick's continuation");
    lim_same(0x004865FC, 5, recIdx, "transported: the record's unit index");
    lim_same(0x0048661C, 9, recKind, "transported: the record's kind");
    lim_same(0x00486672, 7, recArg, "transported: the record passed to the destructor");
    lim_same(0x004867C4, 7, ownArgs, "transported: the own detach's arguments");
    lim_same(0x00486816, 7, detArgs, "transported: the detach's arguments");
    lim_same(0x00486FC4, 5, killRet, "transported: kill-all's return");
    lim_same(0x0042B00A, 4, foldTdf, "transported: the fold's UNITINFO context");
    return FIX_TABLE;
}

/* the heartbeat's ghost section (tagpu_packet_pub.c): DLL counters only. q= held, over= refused
   by a full queue, deep= the deepest queue, replay= replayed before the first tick, now= made
   in a catch-up tick; killed= held creates their refused 0x0C cancelled,
   swept= copies made before state 6 marked dying, nokill= refused 0x0Cs that matched neither,
   untracked= copies past the list a kill can mark; inactive/stale/bad are the drain's drops,
   cleared= held by a load that never reached play; pos= the dirty creates' kind, unbound= with
   a reader that is not B3's, short= a position read that would pass its message's end.
   The alert fields come first: the heartbeat is one log line of at most 2040 bytes and this
   section is its last, so a long game's line is cut from here. */
int tagpu_ghost_format(char* buf, unsigned int cap)
{
    return _snprintf(buf, cap,
                     " | ghost:%s over=%u bad=%u offthread=%u unbound=%u short=%u q=%u deep=%u"
                     " replay=%u now=%u killed=%u swept=%u nokill=%u untracked=%u inactive=%u"
                     " stale=%u cleared=%u levels=%u pos ground=%u air=%u none=%u off=%u",
                     s_ghostOff ? " LEVER-OFF" : "", s_ghostDiag.overflow, s_ghostDiag.bad,
                     s_ghostDiag.offThread, s_ghostDiag.posUnbound, s_ghostDiag.posShort, s_ghostDiag.queued,
                     s_ghostDiag.deep, s_ghostDiag.replayed, s_ghostDiag.now, s_ghostDiag.killed, s_ghostDiag.swept,
                     s_ghostDiag.noKill, s_ghostDiag.untracked, s_ghostDiag.inactive, s_ghostDiag.stale,
                     s_ghostDiag.cleared, s_ghostDiag.levels, s_ghostDiag.posGround, s_ghostDiag.posAir,
                     s_ghostDiag.posNone, s_ghostDiag.posOff);
}

SIM_FEATURE(kill_counts, SIM_PUPPET_RUNS, "c3-veterancy");

/* ===== KILL COUNTS ACROSS PEERS: THE OWNER'S BUILD STATE AT THE CREATE AND AT THE DEATH ======
   Landing B8 of research/notes/tadr-port/sim-fixes.md ("B8" has the argument and the
   measurements; the addresses are in exe-reverse-engineering.md, "Who counts a kill, and why two
   peers can disagree").

   THE DEFECT [DISASSEMBLED; MEASURED on three peers]. Every peer runs the destructor 0x4866D0
   for every death, the owner from Send_UnitDeath 0x4864B0 and every other peer from the 0x0C,
   and four of its decisions test the victim's +0x104, the build fraction left, on THIS peer's
   copy: the unit's kill count (0x4869A7), the killer player's Kills (0x4868D3), a reclaim's
   credit (0x486CBD) and the death explosion (0x486D2F). A copy starts unfinished at HP 0
   whatever its owner's unit is (CreateFromNetwork 0x4861D0 -> 0x485A40 with finished 0,
   0x485B27..0x485B37) and takes the owner's values only when the owner's round robin reaches
   its slot, up to N owner ticks later (0x48B4B2, 0x48B4E3), or from a 0x12 that only a type
   with a build list takes (0x41B8F1). A kill in that window counts on the owner's peer alone.

   THE FIX: the owner's value travels in the message that needs it.
   - The death: the 0x0C travels as a tagged 0x05 (B4's idiom, 0x4C) with the victim's +0x104
     as its float bits, read at the send (0x48666D) before the owner's own destructor reads it
     (0x486679). Every other peer writes it into its copy and runs the destructor on the record
     as the 0x0C case does (0x45541C), so all four decisions read the owner's value there too.
     A death the gate refuses in state 5 is B5's: it cancels a create still held, or, in the
     catch-up ticks, marks the copy dying with the owner's value written first (ghost_sweep),
     so the unit tick's destroy (0x48AFB9 -> 0x4864B0) decides on it as well.
   - The create: the 0x4A also carries its unit's +0x104 and HP, read as the message leaves,
     and CreateFromNetwork's exit writes them into the copy it made, as the round robin writes
     them (kill_apply_state). Three engine callers and tacli's scenario applier set a new unit's
     HP or fraction after the create returns, and so after its 0x09 has left inside it
     (0x486115): the capture 0x488700, the map's placed units 0x488462 and the resurrection
     0x405104. Each one HOLDS its thread's messages from the call to a flush after its writes;
     the flush reads the unit again and sends what the create sent, in order.
   - A unit still being built is carried at its fraction, and its copy is finished as every
     copy was: by the round robin and by its builder's 0x12, unchanged.

   THE WIRE. Both are B4's 65-byte 0x05 (HIT_MSG): m[0] 0x05, m[1] 0, m[2] the tag. A byte no
   line below names is sent 0 and read by no receiver.
   The death, 0x4C (kill_tx_death builds it, kill_rx_death reads it):
     m[3..13]   the stock 0x0C record, 11 bytes, as Send_UnitDeath built it: +0 0x0C, +1 the
                victim's slot (u16), +3 the killer's player's DirectPlay id, +7 the killer's
                slot (u16, 0 for none), +9 the severity, +0xA the kind << 4 | the corpse type
                (0x4865E9..0x486621). The destructor and B5's refusal read the record at m+3.
     m[14..17]  the victim's +0x104, its float bits, read on the owner as the message leaves
     m[18]      1 when m[14..17] holds it; 0 when the victim's index named no slot, and the
                receiver then writes nothing into its copy
     m[19]      1 when the death is carried (C4: tx_arm's decision at 0x48664B, which the
                receiver arms for its destructor call, tx_rx_death, or keeps as a mark on a
                copy refused while it loads, tx_rx_swept)
     m[20..64]  0, unread
   The create, 0x4A (B4's hit_tx_create and hit_created; B8's bytes are kill_state_put's and
   kill_apply_state's, which reads them through rec = m+3):
     m[3..25]   the stock 0x09, 23 bytes (its slot at m[6..7]); m[26..29] its birth (B4)
     m[30..33]  the unit's +0x104, its float bits
     m[34..35]  its HP, +0x108 (u16)
     m[36]      1 when m[30..35] hold them; 0 when the index named no slot, and the copy
                stays as CreateFromNetwork made it
     m[37..64]  0, unread

   INVARIANTS.
   - A copy made from a held or unheld create starts from its owner's values (a late flush's
     excepted, which starts as stock's), and every peer that holds a copy when the death
     arrives judges it on the owner's value -- in play and in the catch-up ticks alike, by
     construction, not by the round robin's phase. A peer that never had a copy judges
     nothing (GAPS).
   - Bounded: an index names a slot only inside [1, (last - first) / 0x118] (kill_slot); a
     fraction is written only as a number in [0, 1], since the reclaim credit multiplies by
     1 - f; HP is the u16 the round robin itself writes unchecked (0x48B4B2).
   - No hold is left open. The wrapper that holds calls the create itself and lets go at once
     when it returns NULL: every NULL return of 0x485F50 (to 0x4861BD, and 0x48605A) comes
     before its first call, so nothing was sent. A unit returned reaches a flush on every path:
     the capture's two branches (0x488743 with its record, 0x488791 without), the placed
     unit's straight line (0x4884AC), the resurrection's three (0x405119 no unit linked,
     0x405155 no wreck, 0x405164 its success) -- each span a row of the table, no branch
     leaving it -- and tacli's applier on both of its own [DISASSEMBLED].
   - The queue's bound is what one create's DIRECT calls send [DISASSEMBLED, every direct call
     followed from 0x485F50]: the 0x09 as its 0x4A (65 bytes, 0x486115), the 0x12 (5,
     0x48612A, a finished type with def+0x22F 0) and the 0x11 (4, 0x48B1F3, def+0x241 bit 18),
     so 3 messages and 74 bytes. 0x48B090(1, 1)'s reply 0x47F780(unit, 3, 0) (0x48B110) only
     queues a notice, which 0x47F680 plays later by its file name, so none of them sends a 0x13;
     the queue's room for a fourth message of up to 18 bytes keeps one script's send in order
     with them. Nothing between the create and its flush
     sends: the spans are straight code, and the resurrection's calls 0x489690, 0x4815F0 and
     0x421E60 are leaves. A unit's scripts are not covered: the COB interpreter 0x4B0DA0 (19
     callbacks) and 0x4B1C00 (4) are not followed, nor the create's indirect calls 0x49059A and
     0x4905BC, and a Create or Activate script can send a 0x13, 0x11 or 0x0A through the host
     vtable 0x4FD698 [INFERRED]. Such a send takes the late path: past the bound, the whole
     queue goes at once, in order, then the message itself, counted (late=) -- nothing dropped
     or reordered -- and the queued 0x4A goes WITHOUT a state (m+36 0), since its caller's
     writes are still to come: the copy starts exactly as stock's, unfinished at HP 0, and
     takes its values from the round robin, as stock's does.
   - A hold is its thread's own, bounded in time by its caller: an entry claimed with the
     thread id by one InterlockedCompareExchange, touched only by that thread and released at
     its flush in the same call of the caller. The send gate (0x451DF0), which every thread's
     messages pass, queues only the holding thread's; no thread flushes or waits on another's.
     Two threads create, so two entries: the game thread (the order code's capture 0x4046C5
     and resurrection 0x405104 under the unit tick 0x43C334, a capture the dispatcher's case
     0x45577B takes in its pumps 0x4954C8, 0x4968CB and 0x49852E, tacli's applier) and the
     loader (the placed units 0x497B40 -> 0x488310, and that case in its pump 0x49727D). A
     third would find no entry: counted (full=), its messages sent at once, its 0x4A with the
     state at the create.

   GAPS. A unit created and destroyed while this peer is still loading (state 5): its create
   is held by B5 and the death cancels it, so this peer never has a copy, runs no destructor
   for it and counts none of its kill, while its owner and every peer already in play count
   it. The window: a unit whose create and death both reach this peer before its in-play
   entry replays what B5 held -- so within the time this peer's load runs past its owner's.
   The saved-game restore 0x487080 (its create 0x48718E, HP at 0x4871B5, +0x104 at
   0x48727C) is not held. It is reached only from the level load (0x497B29 -> 0x432610 ->
   0x486FD0 -> 0x487080) and from itself; whether a network game can start from a save is not
   established. Between its create and its writes it restores the units the record names
   through itself (0x4871DD, 0x48720D) and attaches (0x48AAC0 sends a 0x0A), so closing it takes
   nested holds with a bound per depth, or the state read from the saved record before the
   create. A copy the 0x2C makes (the dirty create 0x48BA05, the round robin 0x48B497) takes the
   0x2C's state, as stock. A count set on one peer (a scenario's kills, a saved game) stays
   different: B8 makes the increments equal, not the totals.

   CLASS: simulation, fail closed, both builds -- a peer without it counts kills and credits
   reclaims on its own copy's value. Every site is a row of the fail-closed table, beside B4's
   0x05 receiver that it extends. The 0x12's index bound (fix_built_bounds) is local. */

#define KILL_HOLDS    2                     /* the threads that create: the game's and the loader */
#define KILL_Q_N      4                     /* the 0x09, 0x12 and 0x11 one create sends, and one */
#define KILL_Q_BYTES  92                    /* 65 + 5 + 4, and 18 for that one                   */

struct kill_hold {
    volatile LONG tid;                      /* the holding thread, 0 when free                  */
    int flushing;                           /* the flush's own sends pass the gate              */
    unsigned int n, used;
    struct { unsigned int net, len, off; } q[KILL_Q_N];
    unsigned char bytes[KILL_Q_BYTES];
};
static struct kill_hold s_killHold[KILL_HOLDS];
SIM_VAR(s_killHold, SIM_TRANSIENT);

/* slot idx of the unit array, or NULL for 0 or past it (B3's bound, wire_index_ok) */
static char* kill_slot(const char* ta, unsigned int idx)
{
    unsigned int max;
    char* first = (char*)wire_units(ta, &max);
    return first && wire_index_ok(idx, max) ? first + (size_t)idx * 0x118 : NULL;
}

/* the 0x4A's state: +0x104's float bits at m+30, HP at m+34, m+36 = 1 */
static void kill_state_put(unsigned char* m, unsigned int fbits, unsigned int hp)
{
    memcpy(m + 30, &fbits, 4);
    m[34] = (unsigned char)hp;
    m[35] = (unsigned char)(hp >> 8);
    m[36] = 1;
}

/* the state of slot idx as it is now; a slot outside the array leaves m+36 0, and the
   receivers keep the copy CreateFromNetwork made */
static void kill_state_read(unsigned char* m, unsigned int idx)
{
    const char* slot = kill_slot(*(const char* const*)0x00511DE8, idx);
    unsigned int fbits;
    if (!slot) { s_killDiag.noSlot++; return; }
    memcpy(&fbits, slot + 0x104, 4);
    kill_state_put(m, fbits, *(const unsigned short*)(slot + 0x108));
}

static struct kill_hold* kill_hold_mine(void)
{
    LONG me = (LONG)GetCurrentThreadId();
    int i;
    for (i = 0; i < KILL_HOLDS; i++)
        if (s_killHold[i].tid == me) return &s_killHold[i];
    return NULL;
}

/* how a flush fills a queued 0x4A's state */
#define KILL_READ    0      /* its unit's state now: the caller's writes are done              */
#define KILL_GIVEN   1      /* the caller's values, which it writes after this flush           */
#define KILL_ABSENT  2      /* none (m+36 0): the caller's writes are still to come, so the copy
                               starts as stock's, unfinished at HP 0, for the round robin      */

/* Sends what h queued, in order, and frees h. */
static void kill_flush(struct kill_hold* h, int how, unsigned int fbits, unsigned int hp)
{
    unsigned int i;
    h->flushing = 1;
    for (i = 0; i < h->n; i++) {
        unsigned char* m = h->bytes + h->q[i].off;
        if (h->q[i].len == HIT_MSG && m[0] == 0x05 && m[1] == 0 && m[2] == HIT_TAG_CREATE) {
            memset(m + 30, 0, 7);
            if (how == KILL_GIVEN) kill_state_put(m, fbits, hp);
            else if (how == KILL_READ) kill_state_read(m, m[6] | (unsigned int)m[7] << 8);
        }
        HIT_SEND(h->q[i].net, m, h->q[i].len);
    }
    h->n = h->used = 0;
    h->flushing = 0;
    InterlockedExchange(&h->tid, 0);
}

/* before a create whose caller sets the unit's HP or fraction after it returns; a hold this
   thread still has (no caller reaches here holding, by the flush sites) goes first, late */
static void kill_hold_arm(void)
{
    LONG me = (LONG)GetCurrentThreadId();
    struct kill_hold* h = kill_hold_mine();
    int i;
    if (h) {
        InterlockedIncrement(&s_killDiag.holdLate);
        kill_flush(h, KILL_ABSENT, 0, 0);
    }
    for (i = 0; i < KILL_HOLDS; i++) {
        if (InterlockedCompareExchange(&s_killHold[i].tid, me, 0) == 0) {
            s_killHold[i].n = s_killHold[i].used = 0;
            s_killHold[i].flushing = 0;
            InterlockedIncrement(&s_killDiag.held);
            return;
        }
    }
    InterlockedIncrement(&s_killDiag.holdFull);
}

/* In place of the send 0x451DF0's first three instructions, on every thread and for every
   message: 1 when this thread holds a create, the message then queued behind it and the send
   answered as a sent message (eax 1, ret 0xC); 0 for the send to go on. A message past the
   bound (a unit script's, which the bound does not cover) sends the queue first, in order,
   late: its 0x4A without a state, since the caller's writes are still to come. */
static int __cdecl kill_send_gate(unsigned int* regs)
{
    struct kill_hold* h = kill_hold_mine();
    const unsigned char* sp = WPN_ESP_JMP(regs);          /* the return, then net, msg, len */
    const unsigned char* msg;
    unsigned int len;
    if (!h || h->flushing) return 0;
    msg = *(const unsigned char* const*)(sp + 8);
    len = *(const unsigned int*)(sp + 12);
    if (!msg || !len || len > KILL_Q_BYTES - h->used || h->n >= KILL_Q_N) {
        InterlockedIncrement(&s_killDiag.holdLate);
        kill_flush(h, KILL_ABSENT, 0, 0);
        return 0;
    }
    h->q[h->n].net = *(const unsigned int*)(sp + 4);
    h->q[h->n].len = len;
    h->q[h->n].off = h->used;
    memcpy(h->bytes + h->used, msg, len);
    h->used += len;
    h->n++;
    regs[PR_EAX] = 1;
    return 1;
}

/* the flush sites after the capture's and the placed unit's writes, and on the resurrection's
   paths that write nothing (0x405119, 0x405155) */
static void __cdecl kill_flush_site(unsigned int* regs)
{
    struct kill_hold* h = kill_hold_mine();
    (void)regs;
    if (h) kill_flush(h, KILL_READ, 0, 0);
}

/* 0x405164, the resurrection's success: from here it runs straight to its writes of +0x104 =
   0.0 (0x405219) and HP = 1 (0x405226), rows of the table, through a 0x0F send (0x405210) the
   create must go before. Its 0x4A carries those two values. */
static void __cdecl kill_flush_resurrected(unsigned int* regs)
{
    struct kill_hold* h = kill_hold_mine();
    (void)regs;
    if (h) kill_flush(h, KILL_GIVEN, 0x00000000u, 1u);
}

static void __cdecl kill_hold_site(unsigned int* regs)
{
#ifndef TAGPU_LIMITS_STOCK
    /* The held-create wrapper owns a second call frame. Check mission placement
       while its original caller and type argument still name the native site;
       UNITS_Create sees this wrapper's return address, not 0x488467. */
    if (regs[PR_RET] == 0x00488467u)
        cob_required(regs[PR_RET + 2] & 0xFFFFu);
#else
    (void)regs;
#endif
    kill_hold_arm();
}

void tagpu_kill_hold(void)
{
    kill_hold_arm();
}

void tagpu_kill_flush(void)
{
    kill_flush_site(NULL);
}

/* CreateFromNetwork's exit, for a copy made from a carried 0x09 (hit_created, rec = m+3): the
   owner's fraction and HP, written as the round robin writes them -- +0x9E's +0x10 cleared
   (0x48B4A6), the HP (0x48B4B2), the fraction with bit 13 of +0x110 when it changes
   (0x48B4D0..0x48B4EC). +0x9E is the pointer this create stored and dereferenced itself
   (0x485DCC, 0x485DF2), on this thread. */
static void kill_apply_state(char* slot, const unsigned char* rec)
{
    unsigned int fbits;
    float f;
    char* ob;
    if (rec[33] == 0) { s_killDiag.createAbsent++; return; }
    if (rec[33] != 1) { s_killDiag.createBad++; return; }
    memcpy(&fbits, rec + 27, 4);
    memcpy(&f, &fbits, 4);
    if (!(f >= 0.0f && f <= 1.0f)) { s_killDiag.createBad++; return; }
    ob = *(char* const*)(slot + 0x9E);
    if (ob) *(unsigned int*)(ob + 0x10) = 0;
    *(unsigned short*)(slot + 0x108) = (unsigned short)(rec[31] | (unsigned int)rec[32] << 8);
    if (*(const float*)(slot + 0x104) != f) {
        memcpy(slot + 0x104, &fbits, 4);
        *(unsigned int*)(slot + 0x110) |= 0x2000u;
    }
    if (f == 0.0f) s_killDiag.createDone++;
    else s_killDiag.createNano++;
}

/* In place of Send_UnitDeath's `call 0x451DF0` (0x48666D), with its signature: the 0x0C,
   carried with its victim's +0x104 at m+14 (m+18 = 1) */
static int __stdcall kill_tx_death(unsigned int net, const unsigned char* msg, unsigned int len)
{
    const char* slot = kill_slot(*(const char* const*)0x00511DE8,
                                 msg[1] | (unsigned int)msg[2] << 8);
    unsigned char m[HIT_MSG];
    memset(m, 0, sizeof m);
    m[0] = 0x05;
    m[2] = HIT_TAG_DEATH;
    memcpy(m + 3, msg, len < 11u ? len : 11u);
    if (slot) {
        memcpy(m + 14, slot + 0x104, 4);
        m[18] = 1;
    } else {
        s_killDiag.noSlot++;
    }
    m[19] = s_txCarried[msg[1] | (unsigned int)msg[2] << 8];   /* tx_arm's, at 0x48664B */
    s_killDiag.out++;
    s_hitDiag.outBytes += HIT_MSG;
    s_hitDiag.stockBytes += len;
    return HIT_SEND(net, m, sizeof m);
}

/* A carried 0x0C in B4's 0x05 receiver: 1 done; 4 the destructor on the record (regs[PR_EAX]),
   the owner's fraction first written into the copy. Refused in state 5 it is B5's
   (ghost_refused_kill), which writes the same fraction into a copy it marks dying; a victim
   index past the array goes to the destructor unwritten, where B3's bound drops it
   (0x4866E5). The counters a refusal touches are written only after a mark, on the game
   thread (ghost_refused_kill's own test). */
static int kill_rx_death(unsigned int* regs, const char* ta, const unsigned char* m)
{
    char* slot;
    unsigned int fbits;
    float f;
    int ok;
    if (m[3] != 0x0C) { s_hitDiag.malformed++; return 1; }
    memcpy(&fbits, m + 14, 4);
    memcpy(&f, &fbits, 4);
    ok = m[18] == 1 && f >= 0.0f && f <= 1.0f;
    if (!hit_gate(ta, 0x0C)) {
        InterlockedIncrement(&s_killDiag.refused);
        if (*(const unsigned int*)(ta + 0x391F1) == 5u &&
            ghost_refused_kill(regs, m + 3, ok ? m + 14 : NULL)) {
            if (ok) s_killDiag.swept++;
            else s_killDiag.badFrac++;
            tx_rx_swept(m);
        }
        return 1;
    }
    s_killDiag.in++;
    regs[PR_EAX] = (unsigned int)(size_t)(m + 3);
    tx_rx_death(m);
    slot = kill_slot(ta, m[4] | (unsigned int)m[5] << 8);
    if (!slot) return 4;
    if (!ok) { s_killDiag.badFrac++; return 4; }
    if (!(*(const unsigned int*)(slot + 0x110) & 0x10000000u)) { s_killDiag.deadCopy++; return 4; }
    if (*(const float*)(slot + 0x104) == f) s_killDiag.same++;
    else if (f == 0.0f) s_killDiag.turned++;
    else s_killDiag.turnedBack++;
    memcpy(slot + 0x104, &fbits, 4);
    return 4;
}

/* The table's rows. B8 extends B4's 0x05 receiver (the 0x4C branch of its stub), a row of the
   same table: the one cannot be written without the other. */
static int fix_kill_counts(void)
{
    static const unsigned char txDeath[5] = { 0xE8, 0x7E, 0xB7, 0xFC, 0xFF };   /* 0x48666D */
    static const unsigned char len0c[2]   = { 0x6A, 0x0B };                     /* 0x486669 */
    static const unsigned char back0c[5]  = { 0xE9, 0x23, 0x0B, 0x00, 0x00 };   /* 0x455428 */
    /* 0x45541C..0x455427, the 0x0C case the 0x4C's branch of B4's stub reproduces (doDeath):
       mov edx,[esp+0x10]; push 0; push edx; call 0x4866D0 */
    static const unsigned char case0c[12] = {
        0x8B, 0x54, 0x24, 0x10, 0x6A, 0x00, 0x52, 0xE8, 0xA8, 0x12, 0x03, 0x00 };
    static const unsigned char send[6]    = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C }; /* 0x451DF0 */
    static const unsigned char capCall[5] = { 0xE8, 0x4B, 0xD8, 0xFF, 0xFF };   /* 0x488700 */
    static const unsigned char plcCall[5] = { 0xE8, 0xE9, 0xDA, 0xFF, 0xFF };   /* 0x488462 */
    static const unsigned char resCall[5] = { 0xE8, 0x47, 0x0E, 0x08, 0x00 };   /* 0x405104 */
    static const unsigned char capRec[8]  = { 0x8B, 0x29, 0x89, 0x2A, 0x66, 0x8B, 0x49, 0x04 };
    static const unsigned char capOld[5]  = { 0x8B, 0x0A, 0x8D, 0x47, 0x64 };   /* 0x488791 */
    static const unsigned char plcNext[8] = { 0x66, 0x8B, 0x46, 0x18, 0x66, 0x89, 0x41, 0x66 };
    static const unsigned char resNone[5] = { 0x68, 0x10, 0x13, 0x50, 0x00 };   /* 0x405119 */
    static const unsigned char resFail[5] = { 0xB8, 0x08, 0x00, 0x00, 0x00 };   /* 0x405155 */
    static const unsigned char resDone[6] = { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }; /* 0x405164 */
    /* 0x488705..0x488742: the capture's test of its create and the record path's writes */
    static const unsigned char capSpan[62] = {
        0x8B,0xF8,0x85,0xFF,0x0F,0x84,0xE8,0x00,0x00,0x00,0x8B,0x97,0x10,0x01,0x00,0x00,
        0x8B,0x44,0x24,0x34,0x81,0xE2,0xFF,0xFF,0xC3,0xFF,0x85,0xC0,0x89,0x97,0x10,0x01,
        0x00,0x00,0x74,0x4B,0x66,0x8B,0x50,0x0B,0x8D,0x48,0x0F,0x66,0x89,0x97,0x08,0x01,
        0x00,0x00,0x8D,0x57,0x64,0xDB,0x40,0x07,0xD9,0x9F,0x04,0x01,0x00,0x00 };
    /* 0x488774..0x488790: the path without a record, its writes from the old unit */
    static const unsigned char capOldSpan[29] = {
        0x66,0x8B,0x86,0x08,0x01,0x00,0x00,0x8D,0x56,0x64,0x66,0x89,0x87,0x08,0x01,0x00,
        0x00,0x8B,0x8E,0x04,0x01,0x00,0x00,0x89,0x8F,0x04,0x01,0x00,0x00 };
    /* 0x488467..0x4884AB: the placed unit's test and its HP write */
    static const unsigned char plcSpan[69] = {
        0x8B,0xC8,0x85,0xC9,0x74,0x4E,0x8B,0x91,0x10,0x01,0x00,0x00,0x33,0xC0,0x8A,0x46,
        0x23,0x80,0xE6,0x7F,0x24,0x80,0xC1,0xE0,0x08,0x0B,0xD0,0x89,0x91,0x10,0x01,0x00,
        0x00,0x8B,0x91,0x92,0x00,0x00,0x00,0x0F,0xBF,0x46,0x1A,0x8B,0x92,0xFA,0x01,0x00,
        0x00,0x0F,0xAF,0xD0,0xB8,0x1F,0x85,0xEB,0x51,0xF7,0xE2,0xC1,0xEA,0x05,0x66,0x89,
        0x91,0x08,0x01,0x00,0x00 };
    /* the resurrection, 0x405109..0x405118 (the order's link 0x489690 and its test),
       0x405141..0x405154 (the two lookups and the branch) and 0x40515A..0x405163 (the failure's
       return), then its straight line from the success branch to its writes, 0x40516A..0x40522E */
    static const unsigned char resLink[16] = {
        0x50,0x8D,0x4E,0x12,0xE8,0x7E,0x45,0x08,0x00,0x8B,0x46,0x16,0x85,0xC0,0x75,0x28 };
    static const unsigned char resLook[20] = {
        0x53,0xE8,0xA9,0xC4,0x07,0x00,0x8B,0xF8,0x57,0xE8,0x11,0xCD,0x01,0x00,0x66,0x3D,
        0xFB,0xFF,0x72,0x0F };
    static const unsigned char resRet[10] = {
        0x5F,0x5E,0x5D,0x5B,0x83,0xC4,0x68,0xC2,0x0C,0x00 };
    static const unsigned char resLine1[79] = {
        0x33,0xC0,0x66,0x8B,0x47,0x0A,0x8B,0x91,0x0B,0x42,0x01,0x00,0x8D,0x04,0x40,0xC1,
        0xE0,0x04,0x03,0xC2,0x74,0x15,0x8B,0x56,0x16,0x83,0xC0,0x20,0x83,0xC2,0x64,0x8B,
        0x08,0x89,0x0A,0x66,0x8B,0x40,0x04,0x66,0x89,0x42,0x04,0x6A,0x00,0x53,0xE8,0x03,
        0xC4,0x07,0x00,0x50,0xE8,0x0D,0xF5,0x01,0x00,0x8B,0x0D,0xE8,0x1D,0x51,0x00,0x8B,
        0x89,0xE9,0x91,0x03,0x00,0xE8,0x4C,0xFF,0x02,0x00,0x83,0xF8,0x03,0x75,0x5C };
    static const unsigned char resLine2[73] = {
        0x8B,0x1D,0xE8,0x1D,0x51,0x00,0xB8,0x4F,0xEC,0xC4,0x4E,0x6A,0x06,0x2B,0xBB,0x87,
        0x42,0x01,0x00,0xF7,0xEF,0xC1,0xFA,0x02,0x8B,0xC2,0x8B,0xBB,0x33,0x42,0x01,0x00,
        0xC1,0xE8,0x1F,0x03,0xD0,0xC6,0x44,0x24,0x18,0x0F,0x8B,0xCA,0xC6,0x44,0x24,0x19,
        0xFF,0x8B,0xC1,0x99,0xF7,0xFF,0x8B,0xC1,0x8D,0x4C,0x24,0x18,0x51,0x66,0x8B,0xDA,
        0x99,0xF7,0xFF,0x8B,0x95,0x96,0x00,0x00,0x00 };
    static const unsigned char resLine3[45] = {
        0x66,0x89,0x5C,0x24,0x1E,0x66,0x89,0x44,0x24,0x20,0x8B,0x42,0x04,0x50,0xE8,0xDB,
        0xCB,0x04,0x00,0x8B,0x4E,0x16,0x55,0xC7,0x81,0x04,0x01,0x00,0x00,0x00,0x00,0x00,
        0x00,0x8B,0x56,0x16,0x66,0xC7,0x82,0x08,0x01,0x00,0x00,0x01,0x00 };
    unsigned char *aGate, *aBare, *aHold, *aCapRec, *aCapOld, *aPlc, *aResNone, *aResFail,
                  *aResDone, *p, *jPass, *jHeld;
    int k;

    if (!(aGate = hit_code(48)) || !(aBare = hit_code(16)) || !(aHold = hit_code(80)) ||
        !(aCapRec = hit_code(32)) || !(aCapOld = hit_code(32)) || !(aPlc = hit_code(32)) ||
        !(aResNone = hit_code(32)) || !(aResFail = hit_code(32)) || !(aResDone = hit_code(32))) {
        lim_no_stub();
        return FIX_TABLE;
    }

    /* 0x451DF0: queued -> ret 0xC with eax 1; else stock's push ebx; push ebp;
       mov ebp,[esp+0xC] */
    p = aGate;
    *p++ = 0x60; *p++ = 0x54;
    p = hit_jmp(p, 0xE8, (unsigned int)(size_t)kill_send_gate);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x85; *p++ = 0xC0;                            /* test eax,eax       */
    *p++ = 0x61;                                         /* popad              */
    *p++ = 0x74; jPass = p++;                            /* jz pass            */
    *p++ = 0xC2; *p++ = 0x0C; *p++ = 0x00;               /* ret 0xC            */
    *jPass = (unsigned char)(p - (jPass + 1));
    memcpy(p, send, 6); p += 6;
    hit_jmp(p, 0xE9, 0x00451DF6);

    /* a bare 0x0C: counted, and the loop goes on */
    p = aBare;
    *p++ = 0xF0; *p++ = 0xFF; *p++ = 0x05; p = hit_abs(p, (const void*)&s_killDiag.bare0C);
    hit_jmp(p, 0xE9, 0x00455F50);

    /* in place of a caller's `call 0x485F50`: hold, the create on a copy of its eight
       arguments, and a NULL let go at once (it sent nothing) */
    p = fix_call_regs(aHold, kill_hold_site);
    for (k = 0; k < 8; k++) {
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x20;   /* push [esp+0x20]    */
    }
    p = hit_jmp(p, 0xE8, 0x00485F50);
    *p++ = 0x85; *p++ = 0xC0;                            /* test eax,eax       */
    *p++ = 0x75; jHeld = p++;                            /* jnz held           */
    p = fix_call_regs(p, kill_flush_site);
    *jHeld = (unsigned char)(p - (jHeld + 1));
    *p++ = 0xC2; *p++ = 0x20; *p++ = 0x00;               /* ret 0x20           */

    /* each flush, then the instructions it displaced */
    p = fix_call_regs(aCapRec, kill_flush_site);
    memcpy(p, capRec, 8); p += 8;
    hit_jmp(p, 0xE9, 0x0048874B);
    p = fix_call_regs(aCapOld, kill_flush_site);
    memcpy(p, capOld, 5); p += 5;
    hit_jmp(p, 0xE9, 0x00488796);
    p = fix_call_regs(aPlc, kill_flush_site);
    memcpy(p, plcNext, 8); p += 8;
    hit_jmp(p, 0xE9, 0x004884B4);
    p = fix_call_regs(aResNone, kill_flush_site);
    memcpy(p, resNone, 5); p += 5;
    hit_jmp(p, 0xE9, 0x0040511E);
    p = fix_call_regs(aResFail, kill_flush_site);
    memcpy(p, resFail, 5); p += 5;
    hit_jmp(p, 0xE9, 0x0040515A);
    p = fix_call_regs(aResDone, kill_flush_resurrected);
    memcpy(p, resDone, 6); p += 6;
    hit_jmp(p, 0xE9, 0x0040516A);

    hit_site(0x0048666D, 5, txDeath, 0xE8, (const void*)kill_tx_death, "kill counts: the 0x0C sent carried");
    hit_slot(0x00455FAC, 0x0045541Cu, aBare, "kill counts: a bare 0x0C dropped");
    hit_site(0x00451DF0, 6, send, 0xE9, aGate, "kill counts: the send, gated by a held create");
    hit_site(0x00488700, 5, capCall, 0xE8, aHold, "kill counts: the capture's create, held");
    hit_site(0x00488462, 5, plcCall, 0xE8, aHold, "kill counts: a placed unit's create, held");
    hit_site(0x00405104, 5, resCall, 0xE8, aHold, "kill counts: the resurrection's create, held");
    hit_site(0x00488743, 8, capRec, 0xE9, aCapRec, "kill counts: the capture's flush, from its record");
    hit_site(0x00488791, 5, capOld, 0xE9, aCapOld, "kill counts: the capture's flush, from the old unit");
    hit_site(0x004884AC, 8, plcNext, 0xE9, aPlc, "kill counts: a placed unit's flush");
    hit_site(0x00405119, 5, resNone, 0xE9, aResNone, "kill counts: the resurrection's flush, no unit");
    hit_site(0x00405155, 5, resFail, 0xE9, aResFail, "kill counts: the resurrection's flush, no wreck");
    hit_site(0x00405164, 6, resDone, 0xE9, aResDone, "kill counts: the resurrection's flush, its values");
    lim_same(0x00486669, 2, len0c, "kill counts: the 0x0C's length");
    lim_same(0x0045541C, 12, case0c, "kill counts: the 0x0C case the carried death reproduces");
    lim_same(0x00455428, 5, back0c, "kill counts: the 0x0C case's continuation");
    lim_same(0x00488705, 62, capSpan, "kill counts: the capture's writes from its record");
    lim_same(0x00488774, 29, capOldSpan, "kill counts: the capture's writes from the old unit");
    lim_same(0x00488467, 69, plcSpan, "kill counts: a placed unit's HP write");
    lim_same(0x00405109, 16, resLink, "kill counts: the resurrection's link and test");
    lim_same(0x00405141, 20, resLook, "kill counts: the resurrection's lookups");
    lim_same(0x0040515A, 10, resRet, "kill counts: the resurrection's failure return");
    lim_same(0x0040516A, 79, resLine1, "kill counts: the resurrection's straight line, 1");
    lim_same(0x004051B9, 73, resLine2, "kill counts: the resurrection's straight line, 2");
    lim_same(0x00405202, 45, resLine3, "kill counts: the resurrection's writes");
    return FIX_TABLE;
}

SIM_FEATURE(built_bounds, SIM_PUPPET_RUNS, "battle");

/* 0x12, the builder's finish (the case 0x4555BA, entry 0x455FC4 of the jump table 0x455F84):
   stock scales both indices into the unit array with no bound (the built unit at rec+1,
   0x4555C4..0x4555E2; the builder at rec+3, 0x4555FB..0x45560C), and 0x41B8D0 reads +0x110
   through each. 0 stays stock's NULL; an index past the array drops the record to 0x455F50,
   the back edge every case takes. regs: the case's frame, the record at the site's esp + 0x10. */
static int __cdecl kill_s12(unsigned int* r)
{
    const unsigned char* rec = *(const unsigned char* const*)(WPN_ESP_JMP(r) + 0x10);
    unsigned int max, built, builder;
    wire_units(*(const char* const*)0x00511DE8, &max);
    if (!rec) return 0;
    built   = *(const unsigned short*)(rec + 1);
    builder = *(const unsigned short*)(rec + 3);
    if (wire_killer_ok(built, max) && wire_killer_ok(builder, max)) return 1;
    s_killDiag.drop12++;
    if (InterlockedIncrement(&s_killDiag.notes) <= 16)
        tagpu_logf("enginefix: kill counts: a 0x12 names a unit past the array (built %u, "
                   "builder %u, last %u); dropped", built, builder, max);
    return 0;
}

/* LOCAL: its absence changes only a malformed record's fate. The case's first two
   instructions, `mov esi,[esp+0x10]; mov ecx,[0x511DE8]`, go to the stub. */
static int fix_built_bounds(void)
{
    static const unsigned char k12[10] = { 0x8B, 0x74, 0x24, 0x10, 0x8B, 0x0D, 0xE8, 0x1D, 0x51,
                                           0x00 };                             /* 0x4555BA */
    FIXSITE s;
    unsigned char* a;
    s.va = 0x004555BA;
    s.n = 10;
    memcpy(s.was, k12, 10);
    if (!fix_match(&s, 1)) return FIX_BYTES;
    if (!(a = fix_code(40))) return FIX_STUB;
    wire_emit_tail(a, kill_s12, 0x00455F50u, k12, 10, 0x004555C4u);
    fix_branch(&s, 0xE9, a);
    return fix_write(&s, 1);
}

/* the heartbeat's kills line (tagpu_packet_pub.c), a line of its own after the packet: line:
   DLL counters only. The alerts first: bad= deaths whose fraction was not a number in [0, 1],
   bare= bare 0x0Cs dropped, late= holds flushed by anything but their own site (their 0x4A
   without a state), full= holds that found no free entry, noslot= messages sent without a
   state, b12= 0x12s dropped past the array. Then out/in= deaths sent and taken, st5= deaths
   refused by the gate, sw= those whose copy B5 marked dying with the owner's fraction,
   same/fix/rev= a copy's fraction that already matched, was unfinished where the owner's was
   finished, or the reverse; dead= deaths for a copy already gone; cr= copies made
   finished/unfinished/without a state/with an invalid one; held= holds.
   CUT FROM NOTHING: every conversion in KILLS_FMT is a %u of an unsigned int, at most 10
   digits for its 2 characters, so the line is at most 5 times the format's length, which the
   typedef proves shorter than TAGPU_KILLS_LINE, its buffer. */
#define KILLS_FMT "kills: bad=%u bare=%u late=%u full=%u noslot=%u b12=%u out=%u in=%u st5=%u" \
                  " sw=%u same=%u fix=%u rev=%u dead=%u cr=%u/%u/%u/%u held=%u"
typedef char kills_line_fits[(5 * (sizeof KILLS_FMT - 1) < TAGPU_KILLS_LINE) ? 1 : -1];

int tagpu_kills_format(char* buf, unsigned int cap)
{
    return _snprintf(buf, cap, KILLS_FMT,
                     s_killDiag.badFrac, (unsigned int)s_killDiag.bare0C, (unsigned int)s_killDiag.holdLate,
                     (unsigned int)s_killDiag.holdFull, s_killDiag.noSlot, s_killDiag.drop12, s_killDiag.out, s_killDiag.in,
                     (unsigned int)s_killDiag.refused, s_killDiag.swept, s_killDiag.same, s_killDiag.turned,
                     s_killDiag.turnedBack, s_killDiag.deadCopy, s_killDiag.createDone, s_killDiag.createNano,
                     s_killDiag.createAbsent, s_killDiag.createBad, (unsigned int)s_killDiag.held);
}

SIM_FEATURE(map_terrain, SIM_PUPPET_RUNS, "battle");

/* MAPS WITH NO TERRAIN [DISASSEMBLED 2026-09-28]. A map is an .ota (its settings) and a .tnt (its
   terrain), and a mod can ship the one without the other: Total Mayhem's mayhem.gp3 carries 105
   .ota and no .tnt, and 9 of them have terrain in no archive of the Steam install. The engine
   lists a map by its .ota alone, so such a map is listed, can become the saved skirmish map, and
   the SKIRMISH screen then stops on a box naming Maps\<name>.TNT. Both lists, skirmish and
   multiplayer, come from one builder, 0x434BF0 (called at 0x435D73, 0x444F47, 0x444FCC, 0x47AAF7,
   0x47AB7C, and by itself at 0x434F29).

   THE LIST. The builder walks `Maps\*.ota` (0x4BCA30) and keeps a map when its .ota parses
   (0x4C2F60) and 0x436860 accepts it. 0x434D87 follows that test, and there the map's terrain is
   asked for too. A map without it goes where the builder's own refusal goes: 0x434EA9, the loop's
   tail, which frees the parsed .ota and moves to the next name. Every register is as the
   engine's `je` at 0x434D81 leaves it (ebp among them, 0 there, which 0x434EA9's compare needs), because the stub restores them and branches before the site's first
   instruction. The name is the one 0x434D92 reads, [[esp+0x24] + esi], esi = index * 4.

   THE SAVED MAP. 0x430B87 tests whether the options loader read `SkirmishMap`. A value naming a
   map with no terrain is treated as a missing one, so the engine saves the filtered list's
   first entry instead, exactly as it does on a fresh key. The value is the list's DISPLAY
   name, which a `language` other than english translates (Translate.tdf, through 0x4C5740 at
   0x434DE9), so it is resolved as the map loader resolves it: `Maps\<value>.OTA` if that
   exists (the loader's parse, 0x436114), else the untranslated key 0x4C5840 returns (0x43611E),
   the table it reads (0x51FDB8) having been built in WinMain's init (0x49EA33) before any
   state handler runs the options loader.

   THE QUESTION is the engine's own: the path from 0x4290F0(out, "Maps", name, "TNT"), the
   builder this loop uses for the .ota at 0x434D40, then opened and closed through the archive
   layer (0x4BB5B0, 0x4BB5D0), on the thread building the list. CLASS: local. It changes
   which maps are offered, never a rule of play. Whether a network peer has the host's map is
   decided from the terrain file itself (0x448EAE -> 0x435A20 -> 0x435430(1, "Maps", name,
   "TNT") -> 0x4BBC40, read by 0x4358F0 at 0x448EBF), not from this list. */
typedef char* (__stdcall *MAP_PATH_FN)(char* out, const char* dir, const char* name, const char* ext);
typedef void* (__stdcall *VFS_OPEN_FN)(const char* path);
typedef void  (__stdcall *VFS_CLOSE_FN)(void* h);
typedef const char* (__stdcall *UNTRANSLATE_FN)(const char* shown);

static struct {
    unsigned int noTerrain;                   /* maps left out of the list                  */
    unsigned int savedNoTerrain;              /* saved skirmish maps set aside              */
} s_mapsDiag;
SIM_VAR(s_mapsDiag, SIM_PRIVATE);

/* 1 when Maps\<name>.<ext> opens; `name` carries an extension, which the builder replaces.
   0x4290F0 writes with no bound, so the bound is ours: the name and the `language` value
   0x49F580 returns (0x51FB50, "english" when the registry has none) under 256 bytes each keep
   its longest form, "Maps-<language>\<name>.<ext>", inside `path`. A name past the bound
   answers 1, the stock verdict. */
static int map_file_opens(const char* name, const char* ext)
{
    char path[1024];
    void* h;
    if (strnlen(name, 256) >= 256 || strnlen((const char*)0x0051FB50, 256) >= 256) return 1;
    ((MAP_PATH_FN)0x004290F0)(path, "Maps", name, ext);
    h = ((VFS_OPEN_FN)0x004BB5B0)(path);
    if (!h) return 0;
    ((VFS_CLOSE_FN)0x004BB5D0)(h);
    return 1;
}

static int map_has_terrain(const char* name)
{
    return map_file_opens(name, "TNT");
}

/* `shown` + ".ota" into `out` (0x100 + 4 bytes); 0 when `shown` has no NUL in 0x100 bytes */
static int map_file_name(char* out, const char* shown)
{
    size_t n = strnlen(shown, 0x100);
    if (n >= 0x100) return 0;
    memcpy(out, shown, n);
    memcpy(out + n, ".ota", 5);
    return 1;
}

/* at 0x434D87: the answer goes back in eax, which the site's first instruction overwrites */
static void __cdecl map_list_terrain(unsigned int* r)
{
    const char* const* names = *(const char* const* const*)(size_t)(r[PR_ESP] + 0x24);
    const char* name = *(const char* const*)((const char*)names + r[PR_ESI]);
    r[PR_EAX] = (unsigned int)map_has_terrain(name);
    if (!r[PR_EAX]) s_mapsDiag.noTerrain++;
}

/* at 0x430B87, a value read: [main+0x29A0]+0x11C, the 0x100 bytes 0x4B69B0 was given. A value
   with no NUL in them is left to the engine, as stock. */
static void __cdecl saved_map_terrain(unsigned int* r)
{
    const char* ta = *(const char* const*)0x00511DE8;
    const char* saved = *(const char* const*)(ta + 0x29A0) + 0x11C;
    const char* key;
    char name[0x100 + 4];
    r[PR_EAX] = 1;
    if (!map_file_name(name, saved)) return;
    if (map_file_opens(name, "OTA"))
        r[PR_EAX] = (unsigned int)map_has_terrain(name);
    else
        r[PR_EAX] = (unsigned int)((key = ((UNTRANSLATE_FN)0x004C5840)(saved)) != NULL &&
                                   map_file_name(name, key) && map_has_terrain(name));
    if (!r[PR_EAX]) s_mapsDiag.savedNoTerrain++;
}

SIM_FEATURE(map_picture, SIM_PUPPET_RUNS, "battle resource-plain");

/* THE MAP'S MINIMAP PICTURE, SMALLER THAN THE SHRINK READS [DISASSEMBLED, MEASURED 2026-11-07].
   BuildMinimapSurface's call at 0x46685F hands 0x4B95A0 the TNT's picture (main+0x1426B, a GAF frame
   LoadMap sized from the file) and the 126-pixel surface it fits. 0x4B95A0 reads 2w x 2h of the frame
   with the frame's own width as the pitch and no test against its height or width, so a picture
   smaller than that is read past its block: `w*h + 0x18` bytes from 0x4B8DA0. 32x32 into 126x113 read
   7 KB out of 1 KB, which faulted on the loader thread only where the block ended at unmapped memory
   (0x4B9626, a read of 0x0E4B0001, under a map that loaded on every other launch). Nothing else in
   the engine reads the picture.

   THE BOUND: a picture that holds 2w x 2h goes to the engine's shrink, byte for byte the stock
   picture; a smaller one is sampled into the surface here, every index below w and h by the
   arithmetic. The plane is `w*h` bytes at frame+0x18 (tagpu_recorder.c's `minimap_build` holds the
   same); `dest` starts with the surface's i16 pitch and rows, and pixels at +0x10 (0x4B8AE0). */
static void __stdcall map_picture_shrink(const unsigned char* fr, const unsigned char* dest)
{
    unsigned short w, h, dw, dh;
    unsigned char* out;
    const unsigned char* px;
    unsigned x, y;
    char b[128];
    memcpy(&w, fr, 2);
    memcpy(&h, fr + 2, 2);
    memcpy(&dw, dest, 2);
    memcpy(&dh, dest + 2, 2);
    if (w >= 2u * dw && h >= 2u * dh) {
        ((void (__stdcall*)(const void*, const void*))0x004B95A0)(fr, dest);
        return;
    }
    out = *(unsigned char* const*)(dest + 0x10);
    px = fr + 0x18;
    for (y = 0; y < dh; y++, out += dw) {
        if (!w || !h) { memset(out, 0, dw); continue; }
        for (x = 0; x < dw; x++)
            out[x] = px[(size_t)(y * h / dh) * w + x * w / dw];
    }
    _snprintf(b, sizeof b, "enginefix: this map's minimap picture is %ux%u, smaller than the %ux%u "
              "the shrink reads - sampled instead", w, h, 2u * dw, 2u * dh);
    b[sizeof b - 1] = 0;
    plog(b);
}

static int fix_map_picture(void)
{
    FIXSITE site[1] = { { 0x0046685F, 5, { 0xE8, 0x3C, 0x2D, 0x05, 0x00 }, { 0 } } };  /* call 0x4B95A0 */
    if (!fix_match(site, 1)) return FIX_BYTES;
    fix_branch(&site[0], 0xE8, (unsigned char*)(size_t)map_picture_shrink);
    return fix_table(site, 1, "the map's minimap picture");
}

static int fix_map_terrain(void)
{
    FIXSITE site[2] = {
        /* mov eax,[esp+0x24]; or ecx,-1 */
        { 0x00434D87, 7, { 0x8B, 0x44, 0x24, 0x24, 0x83, 0xC9, 0xFF }, { 0 } },
        /* test eax,eax; jne 0x430BFE; push 2 */
        { 0x00430B87, 6, { 0x85, 0xC0, 0x75, 0x73, 0x6A, 0x02 }, { 0 } },
    };
    unsigned char *a, *b, *p, *skip, *missing;
    if (!fix_match(site, 2)) return FIX_BYTES;
    if (!(a = fix_code(48)) || !(b = fix_code(48))) return FIX_STUB;

    p = fix_call_regs(a, map_list_terrain);
    *p++ = 0x85; *p++ = 0xC0;                               /* test eax,eax       */
    *p++ = 0x74; skip = p++;                                /* je skip            */
    memcpy(p, site[0].was, 7); p += 7;                      /* the site's own two */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00434D8E); p += 4;   /* jmp 0x434D8E       */
    *skip = (unsigned char)(p - (skip + 1));
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00434EA9); p += 4;   /* skip: 0x434EA9     */

    p = b;
    *p++ = 0x85; *p++ = 0xC0;                               /* test eax,eax       */
    *p++ = 0x74; missing = p++;                             /* je missing         */
    p = fix_call_regs(p, saved_map_terrain);
    *p++ = 0x85; *p++ = 0xC0;                               /* test eax,eax       */
    *p++ = 0x0F; *p++ = 0x85; tagpu_detour_rel(p, 0x00430BFE); p += 4;  /* jne 0x430BFE */
    *missing = (unsigned char)(p - (missing + 1));
    *p++ = 0x6A; *p++ = 0x02;                               /* missing: push 2    */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00430B8D); p += 4;   /* jmp 0x430B8D       */

    fix_branch(&site[0], 0xE9, a);
    fix_branch(&site[1], 0xE9, b);
    return fix_write(site, 2);
}

SIM_FEATURE(load_unpaused, SIM_PUPPET_RUNS, "battle");

/* A LEVEL STARTS PAUSED after the last one was left through the in-game menu. [DISASSEMBLED]
   The pause is main+0x38A51 bit 0. While it is set the frame function 0x496790 skips the
   scheduler 0x495230 and the tick 0x495490 (0x4968AA, 0x496920), which are called nowhere else;
   the scheduler tests it too (0x4953D9) and the status icons show it (0x46A107, 0x46A2A8). The
   pause key flips it and puts the new bit in a message 0x19 (0x4960AA..0x4960E0) [INFERRED:
   0x451DF0 sends it to the peers]. Opening the in-game menu sets it outside a network game
   (0x460E00) and only the menu's close clears it (0x460A33); leaving the level from the menu
   (EXIT, MAINMENU, yes) closes nothing, so the bit outlives the level. The LOADGAME screens set
   it as well (0x49306D, 0x493330). The loader 0x497180 clears it only in its network branch
   (0x49720A, mode 3). Of the word's other writers, 0x44B6F0 is in the battle room's code
   [INFERRED from its neighbours] and 0x49990E is key 0xE3's in state 7's handler 0x499880;
   0x495B6D clears the bit for a step and 0x495DC2 puts back what 0x495B85 saved;
   0x455967 stores a peer's message 0x19. So the next skirmish, campaign mission or saved game
   starts paused: MEASURED 2026-10-01, a stock SKIRMISH Start after a level left from the menu held
   GameTime 0 with the bit set.

   THE FIX, at 0x49842A, the load state's call of the HUD chrome 0x467D70: outside a network
   game the bit is cleared, then the call runs. THE INVARIANT: every level reaches its first tick
   with the bit clear, except a network game's, whose bit is the loader's and the peers'.
   - Every level is loaded by the load state 0x497F40. Only three front-end states install it:
     0x496BB0 at 0x496C3D for mode 2 (a skirmish, a Watch, a tacli playback), 0x496DB0 at
     0x496DFF (mode 1, a campaign mission; 0x496BB0 sends mode 1 there at 0x496BF6), and the
     battle room's 0x496CE0 at 0x496D75 and 0x496D9D (a network game). A saved game, loaded from
     the shell or from the in-game menu, goes the same way: the LOADGAME screen's load branch sets
     the mode from the save's Gametype (0x49267F, 0x434AB0) and installs 0x496BB0 (edi from
     0x49295B, stored at 0x49297E or 0x492A7F). Those two are the only stores of the state
     handler main+0x391F5 from a register; of the 22 with an immediate, only the five named and
     SetState 0x490B30's case 5 (0x490BB3) store 0x497F40, and SetState's eleven callers hand it
     1, 2 or 7.
   - The load state goes on only once the loader thread 0x497C90 has set its done bit,
     main+0x38D75 bit 1 (tested at 0x498342; set at 0x497C62, 0x497180's last store before it
     returns and the thread ends), so this write races nothing on that thread. Both branches of
     0x498353..0x498455 meet at 0x49842A (0x498386 is the only jump in), before the frame
     function's first run at 0x49842F and before the in-play state 0x499200 is installed at
     0x498455 (SetState's case 6, never handed 6).
   The `and byte` writes bit 0 alone; bits 1 and 2 are the scheduler's (0x49535C, 0x495367,
   0x49527A). The recorder does not carry the word: replay-globals.tsv has no row for it, and a
   recording's iterations are the ticks that ran, so a pause delays a recording without changing
   it. GameingState main+0x391E9 is tested for NULL only as a value filter: the loader has just
   read the mode through it (0x4971C7). */
/* CLASS: local. Without it a level starts paused, which the pause key ends. */
static int fix_load_unpaused(void)
{
    FIXSITE site[1] = {
        { 0x0049842Au, 5, { 0xE8, 0x41, 0xF9, 0xFC, 0xFF }, { 0 } },    /* call 0x467D70 */
    };
    unsigned char *c, *p;
    if (!fix_match(site, 1)) return FIX_BYTES;
    if (!(c = fix_code(40))) return FIX_STUB;
    p = c;
    *p++ = 0x50; *p++ = 0x51;                                           /* push eax; push ecx */
    *p++ = 0xA1; *p++ = 0xE8; *p++ = 0x1D; *p++ = 0x51; *p++ = 0x00;    /* mov eax,[main]     */
    *p++ = 0x8B; *p++ = 0x88; *p++ = 0xE9; *p++ = 0x91; *p++ = 0x03; *p++ = 0x00;
                                                                        /* mov ecx,[eax+0x391E9] */
    *p++ = 0x85; *p++ = 0xC9;                                           /* test ecx,ecx       */
    *p++ = 0x74; *p++ = 0x0C;                                           /* jz done            */
    *p++ = 0x83; *p++ = 0x39; *p++ = 0x03;                              /* cmp dword [ecx],3  */
    *p++ = 0x74; *p++ = 0x07;                                           /* je done            */
    *p++ = 0x80; *p++ = 0xA0; *p++ = 0x51; *p++ = 0x8A; *p++ = 0x03; *p++ = 0x00; *p++ = 0xFE;
                                                                        /* and byte [eax+0x38A51],0xFE */
    *p++ = 0x59; *p++ = 0x58;                                           /* done: pop ecx; pop eax */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00467D70u); p += 4;              /* jmp 0x467D70       */
    fix_branch(&site[0], 0xE8, c);
    return fix_write(site, 1);
}

SIM_FEATURE(particle_walk, SIM_PUPPET_INDEPENDENT, "battle");

/* THE PARTICLE LAYERS' TICK IN ONE PASS [DISASSEMBLED]. The walker 0x471EB0 (cdecl, no argument,
   one caller: 0x4955BF in the step 0x495490) goes over the ten layers of *(main+0x38D77), 16 bytes
   each, a vector of object pointers from +4 to +8. For each object in order it calls slot 3 of
   the object's vtable (thiscall, "have you no particle left?", 0x471EE0); on nonzero it calls
   slot 0 with 1 (the deleting destructor, 0x471EF1), moves every later pointer of the layer down
   one place (0x471EFF..0x471F09) and takes 4 off the end (0x471F14); otherwise it calls slot 1,
   the update (0x471F1C). The end is read again at every turn (0x471F25). Removing k objects from
   a layer of n is k*n moves: MEASURED 2026-11-20, a battle of 7 500 units, the moves were a third
   of the particle tick, 1.9 ms of 5.7 a tick.
   THE SAME CALLS IN THE SAME ORDER, THE SURVIVORS KEPT IN PLACE ORDER, ONE MOVE EACH. This makes
   the engine's three calls on the same objects in the same order and leaves each layer holding
   the same pointers in the same order with the same end; a survivor is written to its final place
   once instead of being moved once for every removal before it.
   THE INVARIANT IT RESTS ON: while a layer is walked, the places between the last survivor kept
   and the object being visited hold stale pointers and the end is not yet lowered, which the
   engine's layer never shows. Nothing the three slots reach can see it. Main+0x38D77 is named at
   nineteen places in the image (a scan of its disassembly for the displacement): the table's
   allocation 0x471DCB and release 0x471DEC/0x471E97, this walker 0x471EBB, the draws 0x471F4B
   and 0x471F9C, and the thirteen emitters 0x471FD9..0x472C58; and none of those functions is
   reached from slots 0, 1, 3, 4 or 5 of the seven vtables 0x4FD588..0x4FD638 by direct calls,
   whose indirect calls inside the classes are the same vtables' slots. The draws run from
   DrawGameScreen, never inside the step.
   THE FIRST READ OF AN OBJECT is most of the walk's own time, a cache miss on a heap block, so the
   walk prefetches the object PW_AHEAD places ahead, its first two lines. A prefetch is a hint: it
   writes nothing and does not fault on any address, so the walk's calls, their order and the
   layer it leaves are as above whatever the address. The pointer it reads, read[PW_AHEAD], lies
   between the visited place and the end, which the walk has not written (it writes only at
   `keep`, at or behind the visited place) and nothing the three slots reach writes (above).
   MEASURED 2026-11-21, the 13 500-unit battle zoomed out at 1080p, the prefetch on in alternate
   64-tick blocks of one game: the particle tick 3.32 -> 1.90 ms, faster in 20 of 20 blocks than
   both neighbours (`units` beside it: 9 of 20, ratio 1.005). `prefetcht0` is SSE: pw_prefetch is
   the only code compiled for it, and it runs only where the processor reported SSE at install
   (s_pwSse); elsewhere the walk is the same without the hint.
   CLASS: local. Without it the engine's own walker runs. */
#define PW_AHEAD 8
typedef struct FxLayer { unsigned int unused; void** begin; void** end; void** cap; } FxLayer;
static unsigned char s_pwSse;                   /* IsProcessorFeaturePresent(SSE), at install */
SIM_VAR(s_pwSse, SIM_INSTALL);
static unsigned char s_pwOn;                    /* this walk replaces 0x4955BF's call         */
SIM_VAR(s_pwOn, SIM_INSTALL);
static void pevt_walked(void);
static void pevt_layer_seen(int L, unsigned n);

__attribute__((target("sse"), noipa))   /* noipa: kept as a call, never judged pure */
static void pw_prefetch(const char* object)
{
    __builtin_prefetch(object);
    __builtin_prefetch(object + 64);
}

static void __cdecl particle_walk(void)
{
    typedef int  (__thiscall* PFN_FxEmpty)(void* object);
    typedef void (__thiscall* PFN_FxUpdate)(void* object);
    typedef void (__thiscall* PFN_FxDestroy)(void* object, int flags);
    FxLayer* layer = *(FxLayer* const*)(*(char* const*)0x00511DE8u + 0x38D77);
    int i;
    for (i = 0; i < 10; i++, layer++) {
        void** read = layer->begin;
        void** keep = read;
        pevt_layer_seen(i, (unsigned)(layer->end - read));
        for (; read != layer->end; read++) {
            void* object = *read;
            if (s_pwSse && layer->end - read > PW_AHEAD) pw_prefetch((const char*)read[PW_AHEAD]);
            if (((PFN_FxEmpty)(*(void***)object)[3])(object)) {
                ((PFN_FxDestroy)(*(void***)object)[0])(object, 1);
                continue;
            }
            *keep++ = object;
            ((PFN_FxUpdate)(*(void***)object)[1])(object);
        }
        layer->end = keep;
    }
    pevt_walked();
}

static int fix_particle_walk(void)
{
    FIXSITE site[1] = {
        { 0x004955BFu, 5, { 0xE8, 0xEC, 0xC8, 0xFD, 0xFF }, { 0 } },    /* call 0x471EB0 */
    };
    if (!fix_match(site, 1)) return FIX_BYTES;
    int r;
    s_pwSse = (unsigned char)(IsProcessorFeaturePresent(PF_XMMI_INSTRUCTIONS_AVAILABLE) != 0);
    fix_branch(&site[0], 0xE8, particle_walk);
    r = fix_write(site, 1);
    s_pwOn = (unsigned char)(r == FIX_ARMED);
    return r;
}

SIM_FEATURE(hover_clock, SIM_PUPPET_RUNS, "waterline transport-turn");

/* THE HOVER WAVE ON THE WALL CLOCK [DISASSEMBLED]. 0x48A490(unit) fits a unit to the heights at
   its footprint's four corners and writes its altitude +0x70 (0x48A797) and its tilts +0x68
   (0x48A7B7) and +0x64 (0x48A7DB). For a def with +0x241 bit 12 (canhover), alive (+0x110 bit 28)
   and not dying (bit 14) (0x48A60C..0x48A630), each corner's height gets a wave
   sin(phase) * amplitude (0x4B7123, the 512-entry table 0x509F00) whose phase is
   ((0x4B6340() & 0x1F) + 8 * corner) << 11 plus the unit's +0xAA (0x48A66A..0x48A684).
   0x4B6340 is GetTickCount (IAT 0x4FC0DC) * [[0x51FBD0]+0xE8] / 1000 (0x4B6340..0x4B6361), and
   +0xE8 is 30: its one writer 0x4B62D0 (0x4B62DA) has one caller, WinMain, which pushes 0x1E
   (0x49E98F..0x49E991). So the phase steps once every thirtieth of a second of wall time, and the
   tilts feed the simulation: the piece positions add +0x64/+0x66/+0x68 before rotating
   (0x43DF86..0x43DF8E), so a hovercraft's shots leave from a point the wall clock chose, and the
   water state 0x43DB50 and the weapons' water path 0x49AC3B read the altitude. Two processes,
   two peers or a recording and its replay, compute different hover poses from the same game.
   THE FIX. The 5-byte `call 0x4B6340` at 0x48A66A calls a stub that returns GameTime
   (main+0x38A47) instead. At speed 10, normal speed, the scheduler 0x495230 runs one tick per
   step of that same clock (due = elapsed clock steps * speed * 0.1, engine/sim.md *The scheduler,
   the clock and the test pacing*), so the wave keeps its amplitude, its 32-step period and its
   speed at normal speed; at another speed it runs with the game, as the rest of the unit does.
   The stub writes eax alone, which is what the call returns; ecx and edx, which the stock call
   clobbers, are written before they are read again (0x48A66F, 0x48A676), and 0x48A643 has just
   read the main pointer the stub reads, inside the same call. 0x48A66F's `mov ecx,[esp+0x10];
   and eax,0x1F` is compared, the low five bits of eax being what the phase takes.
   THE INVARIANT: the wave is a function of GameTime and the unit's record, so every process that
   runs a tick computes the same hover pose for the same unit.
   CLASS: simulation, fail closed. A peer without it fits its hovercraft by its own clock. */
static int fix_hover_clock(void)
{
    static const unsigned char call[5] = { 0xE8, 0xD1, 0xBC, 0x02, 0x00 };   /* call 0x4B6340 */
    static const unsigned char after[7] = { 0x8B, 0x4C, 0x24, 0x10, 0x83, 0xE0, 0x1F };
    unsigned char *c, *p;
    if (!(c = fix_code(12))) {
        lim_no_stub();
        return FIX_TABLE;
    }
    p = c;
    *p++ = 0xA1; *p++ = 0xE8; *p++ = 0x1D; *p++ = 0x51; *p++ = 0x00;    /* mov eax,[0x511DE8]   */
    *p++ = 0x8B; *p++ = 0x80; *p++ = 0x47; *p++ = 0x8A; *p++ = 0x03; *p++ = 0x00;
                                                                        /* mov eax,[eax+0x38A47] */
    *p++ = 0xC3;                                                        /* ret                  */
    lim_branch(0x0048A66A, sizeof call, call, 0xE8, (unsigned int)(size_t)c,
               "hover wave: GameTime for the wall clock");
    lim_same(0x0048A66F, sizeof after, after, "hover wave: the phase's low five bits");
    return FIX_TABLE;
}

SIM_FEATURE(mover_stamp, SIM_PUPPET_RUNS, "transport-pad waterline");

/* THE MOVER'S UNWRITTEN STAMP [DISASSEMBLED]. A unit whose def has +0x22F = 1 gets a 0x2F-byte
   mover from MEM_Alloc 0x4B4F10, which does not zero, and the mover's constructor 0x43DC00 (a
   thiscall, ecx the block). The constructor has four callers, each right after its own
   `push 0x2F; call 0x4B4F10` (0x485E66, 0x485F29, the create 0x485F50's 0x4860D5,
   CreateFromNetwork 0x4861D0's 0x4862E9), and no jump lands inside its first six bytes. It
   writes +0x00..+0x29 and the byte +0x2E (0x43DC04..0x43DD03) and never +0x2A..+0x2D, the dword
   "moved at" stamp. The stamp's one writer is the placement 0x43D6D0 (`mov [ebp+0x2A],GameTime`
   at 0x43D865), which runs only for an uncarried unit (+0x86 = 0, 0x43D6DD..0x43D6E5) whose
   position or layer just changed (0x43D827..0x43D854). Its one reader is the hover wave
   (0x48A6DE): m = min(GameTime - stamp, 60), unsigned (0x48A6E8..0x48A6FA), and the wave's
   amplitude is c - c*m/60 (0x48A6FF..0x48A714), full on the tick a hovercraft moves and gone 60
   ticks after it stops. A hovercraft that has not moved since its create (a factory's unit still
   on its pad, a unit standing where it was made) reads whatever the block's last tenant left,
   and the creates' own fit (0x486109, 0x486306) reads it before any placement.
   THE FIX. A jmp over the constructor's first six bytes (`push esi; mov esi,ecx; push edi;
   xor eax,eax`) goes to a stub that writes MOVER_UNMOVED to [ecx+0x2A], runs the six bytes and
   rejoins at 0x43DC06, compared.
   WHY 0x80000000. Stock's never-moved hovercraft has no wave: the leftovers MEASURED in its
   movers (0x5EE80002, 0x736E0003 at GameTime 12001, and the like) all lie above GameTime, so
   GameTime - stamp wraps past 60 and the amplitude is 0. A leftover inside the 60 ticks below
   GameTime, which gives a fading wave, needs the block's last tenant to have been a mover stamped
   in the last 60 ticks [INFERRED: no case measured]. 0x80000000 is above every GameTime the engine
   reaches (2^31 ticks at 30 a second is over two years), so a mover reads "no wave" from its
   constructor to its first move at every GameTime, the first 60 ticks of a level included, as
   the typical stock one does; 0 would give a never-moved hovercraft a fading wave there. The
   first move stamps GameTime, as in stock, and from there the two agree. A hovercraft still on a
   factory pad (carried: the placement never stamps it) has no wave, as stock's typically has.
   THE INVARIANT: every mover's +0x2A is MOVER_UNMOVED from its constructor until a placement
   stamps it, so the hover wave never reads memory the engine did not write.
   CLASS: simulation, fail closed. A peer without it fits a resting hovercraft by its heap. */
#define MOVER_UNMOVED 0x80000000u

static int fix_mover_stamp(void)
{
    static const unsigned char entry[6] = { 0x56, 0x8B, 0xF1, 0x57, 0x33, 0xC0 };
    static const unsigned char join[3]  = { 0x8D, 0x7E, 0x08 };             /* lea edi,[esi+8] */
    static const unsigned int unmoved = MOVER_UNMOVED;
    unsigned char *c, *p;
    if (!(c = fix_code(24))) {
        lim_no_stub();
        return FIX_TABLE;
    }
    p = c;
    *p++ = 0xC7; *p++ = 0x41; *p++ = 0x2A;                    /* mov dword [ecx+0x2A],imm32 */
    memcpy(p, &unmoved, 4); p += 4;
    memcpy(p, entry, sizeof entry); p += sizeof entry;                  /* the six stolen bytes   */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0043DC06);
    lim_branch(0x0043DC00, sizeof entry, entry, 0xE9, (unsigned int)(size_t)c,
               "mover: the moved-at stamp written in the constructor");
    lim_same(0x0043DC06, sizeof join, join, "mover: the constructor, where the stub rejoins");
    return FIX_TABLE;
}

SIM_FEATURE(planner_site, SIM_PUPPET_RUNS, "battle tenseat");

/* THE AI BUILDER PLANNER'S UNWRITTEN SITE ALTITUDE [DISASSEMBLED]. The stock AI's builder planner
   0x408100 (the AI tick 0x408C40's slot +0x21, every 90 ticks) keeps a site {x, y, z} on its stack
   at [esp+0x30..0x38]. For each idle builder it asks 0x40BDB0 for a type and 0x40BFE0(seat,
   &unit->pos, def, site) for a place (the call at 0x4081E2; stdcall, `ret 0x10`, eax not 0 when
   it found one). 0x40BFE0 writes x (0x40C1D3) and z (0x40C1E0) and never y, and nothing else in
   0x408100's first loop stores [esp+0x34]. The order 0x43ADC0 (0x4082B0) then copies the site
   into the node's position +0x22/+0x26/+0x2A verbatim (the constructor 0x43A0C0, 0x43A16F), and
   the commander branch copies site.y into its base's y (0x408225, 0x408250), which the second
   loop copies into its own sites (0x40831C..0x408330). So a MobileBuild node carries whatever
   the stack held: MEASURED 2026-11-13, the factory pointer 0x40BDB0 saved there during the same
   tick's factory planner 0x4086D0 (0x40BDB9), a unit-array address, which differs whenever the
   heap's layout does. MobileBuild 0x403A20 hands the node's position to CreateUnit 0x485F50 by
   value (0x403D34..0x403D51, the call at 0x403D5B), after 0x47DDC0(def, &pos) at 0x403D2F, which
   writes a structure's ground altitude into y (its store at 0x47DE52) and returns at once for any
   other type (def +0x22F not 0, 0x47DDC8..0x47DDD0). So a structure's stale y reached the
   recording, every comparison of two games and the drawn build-site rectangle (0x438C59); a
   mobile type's reached the create itself (0x485A40), as the unit's first altitude.
   WHAT A PLAYER'S ORDER CARRIES. A click issues its order at the cursor's world point main+0x2CAA
   (0x48CF30 from 0x49906D and 0x4991D9; engine/input.md), whose y the unprojection 0x484B50 sets
   to the larger of the terrain height 0x485070(pos) and the sea level, the byte main+0x1427F,
   shifted to 16.16 (0x484BB2..0x484BDE).
   THE FIX. The call at 0x4081E2 goes to planner_site: it writes site.y = 0, makes the stock call
   with the stock arguments, and when that found a place, writes the y a player's order there
   would carry: for a structure what MobileBuild will write, 0x47DDC0(def, copy) on a copy of the
   site, so the site's x and z, which 0x47DDC0 also snaps, stay 0x40BFE0's; for any other type
   the cursor's rule, max(0x485070(site), sea level) << 16. Both are bounded by the engine itself:
   0x47D820 refuses a cell below 1 or whose footprint reaches the map's last row or column
   (0x47D834..0x47D888), and 0x485070 answers -1 for a cell off the map (0x485098..0x4850C4,
   0x485139), which the max turns into the sea level. Every register the planner reads after
   0x4081E2 is written first (eax is the result; ecx at 0x4081E7, edx at 0x4081EF), and the call
   keeps the stdcall frame.
   THE INVARIANT: every site the planner hands an order, and every base y it copies from one, is
   written by the engine's own arithmetic, so no node carries a stack leftover.
   CLASS: simulation, fail closed. A player without it records heap addresses in its AI's orders. */
typedef int (__stdcall* PFN_PlanSite)(int seat, const int* from, const char* def, int* site);
#define E_PlanSite ((PFN_PlanSite)0x0040BFE0u)
typedef void (__stdcall* PFN_SiteAlt)(const char* def, int* pos);
#define E_SiteAlt  ((PFN_SiteAlt)0x0047DDC0u)
typedef int (__stdcall* PFN_GroundHeight)(const int* pos);
#define E_GroundHeight ((PFN_GroundHeight)0x00485070u)
#define DEF_MOBILE  0x22F                   /* byte: 0 for a type MobileBuild places on cells */
#define MAIN_SEALEVEL 0x1427F               /* byte: the level's sea level */

static int __stdcall planner_site(int seat, const int* from, const char* def, int* site)
{
    int found, copy[3];
    site[1] = 0;
    found = E_PlanSite(seat, from, def, site);
    if (!found) return found;
    if (!def[DEF_MOBILE]) {
        copy[0] = site[0];
        copy[1] = 0;
        copy[2] = site[2];
        E_SiteAlt(def, copy);
        site[1] = copy[1];
    } else {
        const char* ta = *(const char* const*)0x00511DE8;
        int ground = E_GroundHeight(site);
        int sea = (unsigned char)ta[MAIN_SEALEVEL];
        site[1] = (ground > sea ? ground : sea) << 16;
    }
    return found;
}

static int fix_planner_site(void)
{
    static const unsigned char call[5] = { 0xE8, 0xF9, 0x3D, 0x00, 0x00 };   /* call 0x40BFE0 */
    static const unsigned char after[6] = { 0x8B, 0x8D, 0x92, 0x00, 0x00, 0x00 };
    lim_branch(0x004081E2, sizeof call, call, 0xE8, (unsigned int)(size_t)planner_site,
               "AI planner: the build site's altitude written");
    lim_same(0x004081E7, sizeof after, after, "AI planner: ecx written after the site's call");
    return FIX_TABLE;
}

SIM_FEATURE(reclaim_site, SIM_PUPPET_RUNS, "battle");

/* THE PATROL'S RECLAIM SEARCH'S UNWRITTEN ALTITUDE [DISASSEMBLED]. 0x47EA40(pos, radius, &out1,
   &value1, &out2, &value2) (stdcall, `ret 0x18`; its callers RepairPatrol 0x405980 at 0x405B93 and
   VTOL_RepairPatrol 0x4152F0 at 0x41564F) walks the feature grid around pos and keeps two lists of
   16-byte records {x, y, z, value} (0x47EB3B..0x47EB64, 0x47EB86..0x47EBB3), one per kind of
   feature, then copies a record of each into *out1 and *out2 (0x47EC3F..0x47EC56,
   0x47ECCB..0x47ECE2). The point it records is a frame local: x at [esp+0x1C] and z at [esp+0x24]
   are the loop's counters (0x47EAAE, 0x47EA9A, 0x47EBCC, 0x47EBEE), and y at [esp+0x20] is never
   written in the function, so every record, and every RECLAIM or VTOL_RECLAIM order the patrols
   make from one (0x43A0C0 at 0x405BFB, 0x405D11 and 0x415784, its position copied verbatim at
   0x43A16F), carries what the stack held there into the node's +0x26.
   THE FIX. A jmp over `mov esi,[esp+0x30]; mov [esp+0x18],eax` at 0x47EA79, where esp is the
   frame's base less the second allocation's pushed argument, to a stub that writes 0 to the y
   local ([esp+0x24] there), runs the two instructions and rejoins at 0x47EA81, compared. Zero is
   the y the engine's own order parser gives a feature reclaim (0x487BF0's orders are x, 0, z:
   0x487D40, 0x487EE2) [INFERRED: what a reclaim reads of y was not traced].
   THE INVARIANT: every point the search records is written whole before it is copied.
   CLASS: simulation, fail closed. A player without it records stack leftovers in its patrols'
   reclaim orders. */
static int fix_reclaim_site(void)
{
    static const unsigned char body[8] = {
        0x8B, 0x74, 0x24, 0x30,                         /* mov esi,[esp+0x30]  */
        0x89, 0x44, 0x24, 0x18,                         /* mov [esp+0x18],eax  */
    };
    static const unsigned char join[4] = { 0x8B, 0x44, 0x24, 0x34 };   /* mov eax,[esp+0x34] */
    unsigned char *c, *p;
    if (!(c = fix_code(24))) {
        lim_no_stub();
        return FIX_TABLE;
    }
    p = c;
    *p++ = 0xC7; *p++ = 0x44; *p++ = 0x24; *p++ = 0x24;               /* mov dword [esp+0x24],0 */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    memcpy(p, body, sizeof body); p += sizeof body;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0047EA81);
    lim_branch(0x0047EA79, sizeof body, body, 0xE9, (unsigned int)(size_t)c,
               "patrol reclaim: the searched point's altitude written");
    lim_same(0x0047EA81, sizeof join, join, "patrol reclaim: where the stub rejoins");
    return FIX_TABLE;
}

/* AN AI ALLY IS AN ALLY LIKE A HUMAN ONE, in a skirmish and in a network game: the owner's rule
   (research/notes/shared-resources-ai-plan.md). Three fixes, each through the engine's own code:
   a hosted AI's 0x28, the transfers without their tax on an AI, and the auto-share for AI allies,
   in a skirmish too, and for every record a peer controls. The player records are ten, at
   main+0x1B63 with a stride of 0x14B; a record is active when its first dword is nonzero, and its
   controller is +0x73 (1 the local human, 2 a local AI, 3 remote). */
#define AIS_MAIN      0x00511DE8u
#define AIS_ROWS      0x1B63
#define AIS_STRIDE    0x14B
#define AIS_TYPE      0x73
#define AIS_LOCAL_AI  2
#define AIS_SENDER    0x004573D0u   /* stdcall (rec, target, flag), ret 0xC: the 0x28 */
#define AIS_SHARE     0x00457D30u   /* stdcall (giver), ret 4: the auto-share          */

static char* ais_record(char* ta, int i)
{
    return ta + AIS_ROWS + i * AIS_STRIDE;
}

static int ais_local_ai(const char* rec)
{
    return *(const unsigned int*)rec && (unsigned char)rec[AIS_TYPE] == AIS_LOCAL_AI;
}

SIM_FEATURE(ai_economy_send, SIM_PUPPET_OWNER, "ai-share");

/* A HOSTED AI'S NUMBERS [DISASSEMBLED 2026-11-15]. In the players phase, once per 30-tick pass
   and for the viewed record alone (BL = main+0x2A43, 0x465565), a network game (0x435100 = 3,
   0x465588) counts the passes in 0x51E53C and every fourth sends 0x4573D0(viewed, 0, 0)
   (0x4655A1): stored values and storages to every remote human. No path sends one for an AI a
   peer hosts during play, so its record holds its level-start values on every other peer. The
   call now sends the same message for each AI this peer hosts, on the same cadence: 0x4573D0
   takes a type-2 record as it stands (its preconditions 0x4573DA..0x4573F7), sends from the
   record's own +0x04, the AI's DirectPlay ID, and the receiver 0x457540 writes it into that AI's
   record as it does a human's (the source scan 0x453DBD, no controller test). A puppet level is
   never a network game, so this never runs on one. */
static void __stdcall ais_economy_send(char* rec, char* target, int flag)
{
    char* ta = *(char**)(size_t)AIS_MAIN;
    int i;
    ((void (__stdcall*)(char*, char*, int))(size_t)AIS_SENDER)(rec, target, flag);
    if (!ta) return;
    for (i = 0; i < 10; i++) {
        char* ai = ais_record(ta, i);
        if (ai != rec && ais_local_ai(ai))
            ((void (__stdcall*)(char*, char*, int))(size_t)AIS_SENDER)(ai, NULL, 0);
    }
}

/* CLASS: shared. Every peer runs it for its own AIs; the others take the message. */
static int fix_ai_economy_send(void)
{
    FIXSITE site[1] = {
        { 0x004655A1u, 5, { 0xE8, 0x2A, 0x1E, 0xFF, 0xFF }, { 0 } },    /* call 0x4573D0 */
    };
    if (!fix_match(site, 1)) return FIX_BYTES;
    fix_branch(&site[0], 0xE8, (const void*)ais_economy_send);
    return fix_write(site, 1);
}

SIM_FEATURE(ai_untaxed, SIM_PUPPET_OWNER, "ai-share");

/* A GIVE TO AN AI, IN FULL [DISASSEMBLED 2026-11-15]. The transfers credit the recipient's +0xEC
   record; when the recipient's controller is 2 (a local AI: 0x464D36 metal, 0x464C06 energy) and
   the difficulty main+0x37EEE is 0 (Easy) or 1 (Medium), they credit half or seven tenths of what
   the giver paid (the factors -0.5 at 0x4FD548 and -0.7 at 0x4FD540). The same factors scale the
   players phase's credit to a respawned unit's resource object (its +0xBC/+0xD4, 0x46547B,
   0x4654E7) when that unit's owner is type 2, around the Deathmatch respawn (owner test 0x465437,
   0x4654A3; reads at 0x465451, 0x465463, 0x4654BD, 0x4654CF), which is not a give and keeps its
   scale. The owner decided an AI receives the full amount (D1): each `jne` past the scale
   becomes a `jmp` to the full credit (0x464D70 metal, 0x464C3E energy), whatever the controller.
   The giver's debit is untouched. Every giver the transfer serves gets it: the share panel, the
   console's Give, the 0x16 receiver on an AI's host, the auto-share. The transfers do not run on
   a puppet (tagpu_puppet.c gates 0x464C60; nothing reaches 0x464B30 there: the typed command is
   gated, a playback runs no network pump, and ais_share_every refuses a puppet level,
   engine/recorder.md). */
/* CLASS: shared. Every peer applies it to the gives its records receive. */
static int fix_ai_untaxed(void)
{
    FIXSITE site[2] = {
        { 0x00464D3Au, 1, { 0x75 }, { 0xEB } },       /* jne 0x464D70 -> jmp: metal  */
        { 0x00464C0Au, 1, { 0x75 }, { 0xEB } },       /* jne 0x464C3E -> jmp: energy */
    };
    if (!fix_match(site, 2)) return FIX_BYTES;
    return fix_write(site, 2);
}

SIM_FEATURE(ai_autoshare, SIM_PUPPET_OWNER, "ai-share");

/* THE AUTO-SHARE FOR EVERY ALLY AND EVERY PLAYER [DISASSEMBLED 2026-11-15]. 0x457D30(giver) gives,
   every 60 ticks, a third of the metal and half the energy above the giver's levels to one ally:
   the highest-index record the giver allies (giver +0x108[j]) holding less than the giver. Stock
   stops it in three places outside what a human ally gets:
   - the recipient must be remote (+0x73 = 3: 0x457DD6 metal, 0x457EF4 energy) and human
     (PlayerInfo +0x94 = 1: 0x457DDD, 0x457EFB). Both `jne`s become NOPs, so any active, alive
     record the giver allies is a recipient; the giver itself is not, since its stored value is
     not below its own (0x457DF1, 0x457F0F). The map share keeps its own tests (0x458032, 0x458041);
   - it needs a network game (0x457D3D): the `je` becomes NOPs;
   - its caller, the step 0x495490(arg), calls it per tick only when `arg` is nonzero
     (0x49560B), which is so only in a network game (0x49680C passes 1, 0x49693E 0). A stub at
     0x49560B leaves the network path as it is and, with `arg` 0, calls the auto-share and
     resumes past the network block (0x49566C).
   And it ran for the controlled player alone (0x49563A): the call now runs it for that record and
   then for every AI this peer hosts, in record order, so an AI's share levels act as a human's.
   The stock AI sets none; whatever an AI sets is its own decision. Each call tests the same
   GameTime % 60, so every giver shares on the same tick, in record order.
   A puppet level plays a recording and its records are type 3: neither path runs there
   (tagpu_puppet_on), the recording carrying what the owner's auto-share did. */
static void __stdcall ais_share_every(char* giver)
{
    char* ta = *(char**)(size_t)AIS_MAIN;
    int i;
    if (tagpu_puppet_on()) return;
    ((void (__stdcall*)(char*))(size_t)AIS_SHARE)(giver);
    if (!ta) return;
    for (i = 0; i < 10; i++) {
        char* ai = ais_record(ta, i);
        if (ai != giver && ais_local_ai(ai))
            ((void (__stdcall*)(char*))(size_t)AIS_SHARE)(ai);
    }
}

/* the step's per-tick call outside a network game: the controlled player's record, then its AIs */
static void __cdecl ais_share_skirmish(void)
{
    char* ta = *(char**)(size_t)AIS_MAIN;
    unsigned int me;
    if (!ta) return;
    me = *(const unsigned char*)(ta + 0x2A42);
    if (me < 10) ais_share_every(ais_record(ta, (int)me));
}

/* CLASS: shared. */
static int fix_ai_autoshare(void)
{
    FIXSITE site[7] = {
        { 0x00457DD8u, 2, { 0x75, 0x29 }, { 0x90, 0x90 } },                         /* metal: remote */
        { 0x00457DE4u, 2, { 0x75, 0x1D }, { 0x90, 0x90 } },                         /* metal: human  */
        { 0x00457EF6u, 2, { 0x75, 0x29 }, { 0x90, 0x90 } },                         /* energy: remote */
        { 0x00457F02u, 2, { 0x75, 0x1D }, { 0x90, 0x90 } },                         /* energy: human  */
        { 0x00457D44u, 6, { 0x0F, 0x84, 0x01, 0x04, 0x00, 0x00 }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },
                                                                                    /* je: no network game */
        { 0x0049560Bu, 9, { 0x85, 0xFF, 0x74, 0x5D, 0xA1, 0xBC, 0x6D, 0x50, 0x00 }, { 0 } },
                                                                                    /* test edi,edi; je; mov eax,[0x506DBC] */
        { 0x0049563Au, 5, { 0xE8, 0xF1, 0x26, 0xFC, 0xFF }, { 0 } },                /* call 0x457D30 */
    };
    unsigned char *c, *p, *skirmish;
    if (!fix_match(site, 7)) return FIX_BYTES;
    if (!(c = fix_code(48))) return FIX_STUB;
    p = c;
    *p++ = 0x85; *p++ = 0xFF;                                           /* test edi,edi       */
    *p++ = 0x74; skirmish = p++;                                        /* jz skirmish        */
    memcpy(p, site[5].was + 4, 5); p += 5;                              /* mov eax,[0x506DBC] */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00495614u); p += 4;              /* jmp: the stock test of it */
    *skirmish = (unsigned char)(p - (skirmish + 1));
    *p++ = 0x9C; *p++ = 0x60;                                           /* pushfd; pushad     */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)ais_share_skirmish); p += 4;
    *p++ = 0x61; *p++ = 0x9D;                                           /* popad; popfd       */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049566Cu);                      /* past the network block */
    fix_branch(&site[5], 0xE9, c);
    fix_branch(&site[6], 0xE8, (const void*)ais_share_every);
    return fix_write(site, 7);
}

SIM_FEATURE(unit_stream, SIM_PUPPET_OWNER, "multiplayer");

/* THE UNIT STREAM'S DIRTY LIST IN TURN [DISASSEMBLED 2026-11-20]. In a network game the unit tick
   calls 0x48B710(rec) once a tick for each local type-1/2 record (0x48B003): a 0x2C message of
   [8] code, [16] size, [32] GameTime, a DIRTY LIST, a [16] 0xFFFF and one round-robin full-state
   entry for the block's unit GameTime % main+0x37EE6 (0x48B80E..). The list walks the record's
   block [rec+0x67, rec+0x6B] (inclusive, stride 0x118) FROM ITS FIRST UNIT every tick
   (0x48B766); a unit alive (+0x110 bit 28) with a mover whose course says it is dirty ([vt+0x1C]:
   the local ground course 0x44F480, +0x64 bit 3 or a changed bit 2; the local air mover 0x4908B0,
   +0x27 bit 0) gets [16] its index less the block's first index ([[unit+0x96]+0x6F]), [typeBits]
   its type (+0xA6; main+0x14393) and its course's payload ([vt+0x20], which clears the dirty
   state), and the list ends once the message holds 0x200 bytes (0x48B7F6), a test made after each
   entry. An entry is about 15 bytes, so about 33 units go a tick; a dirty unit past them waits for
   the next tick, which starts again at the first unit. So while more than that are dirty a tick,
   the block's high slots are sent only by the round robin, once every main+0x37EE6 ticks.
   MEASURED 2026-11-20 (tools/mp_agree.py, two peers of 1499 kbots on Town & Country, paused at
   GameTime 5400, three runs): a unit's entry waited up to 5266 ticks and its copy on the other peer
   stood off its owner's by up to 1408 world units, the top fifth of each block's slots by 408 at the
   90th percentile against 0 in the bottom fifth.

   The receiver 0x48B920 applies each entry at the block's first index plus its delta
   (0x48B985..0x48B99A), so the entries' order is free on the wire; after the list it runs the
   movement pass 0x43DD20 and the sight pass 0x48A870 for every live unit of the sender's block that
   has a mover (0x48BA28..0x48BA5C) -- a remote unit steps once a message, never by the receiver's
   own clock -- and then reads one flag bit (0x48BA5E..0x48BA97): set, the round-robin entry follows;
   clear, the message ends (0x48BAC8). Stock's sender always sets it (0x48B81B..0x48B84C).

   THE FIX, three parts, all in the fail-closed table:
   - IN TURN. stream_dirty_list replaces the walk (0x48B76C..0x48B80E): the same test and the same
     entries, starting at the unit the previous tick stopped at and wrapping at the block's end.
   - UP TO STREAM_MESSAGES MESSAGES A TICK. When the next entry might not fit, the message in hand
     ends with its flag CLEAR -- a list and nothing else -- is sent, and the list goes on in a new
     message on the same writer; the tick's last message is the one the engine's own tail ends,
     flag set, round robin after it. At the cap the list stops and the cursor stays on the unit
     not written; a walk that reaches every unit leaves the cursor where it was.
   - ONE STEP A TICK. The receiver's sweep is taken only for a message whose flag is set
     (stream_step), so the several messages of a tick step the sender's units once.
   SEVERAL MESSAGES ONLY TO PEERS THAT STEP ON THE LAST. A receiver without the third part steps
   the sender's units once a message, so the second part is a play revision of its own
   (tagpu_recorder.c, MP_PLAY_REVISION) and is used only while this process holds it
   (tagpu_rec_mp_revision_held: every player it makes carries the number, which no host of another
   revision takes, and its own host test takes no joiner without it). Otherwise a tick is one
   message, flag set, as stock sends it and any receiver steps it; the first and third parts change
   nothing a stock peer can tell. A change to what these messages carry raises that number.
   THE SIZE BOUND. The writer's buffer is 0x100 dwords inside the sender's frame (0x415B60), and its
   growth is broken: 0x415C76, 0x415D14 and 0x415BB6 allocate four bytes and copy the whole buffer into them.
   The queue below drops a message over 0x42A bytes (0x4628D0, 0x462911), and one that arrives when a
   forced flush leaves its ring not empty (0x462716..0x462731); tagpu_netlog.on counts both as refused. So an entry is written
   only while the message, with that entry at its largest and the engine's tail at its largest
   after it, stays under STREAM_BITS_MAX: 16 + typeBits + 213 for an entry (the air payload
   0x4908C0: 2 + 209 + 2; the ground's 0x44F4A0 is 99) and 16 + 1 + typeBits + 211 for the tail
   (the round robin 0x48B200), the readers' maxima of engine/network.md, which the writers mirror
   field for field. The growth is never reached and the largest message is 1024 bytes.
   THE INVARIANT: each tick resumes where the last one stopped and writes at least one entry when
   any unit is dirty (an empty message always has room for one), so the walk passes every unit of
   the block within one turn of it, and a turn is the block's dirty units over what STREAM_MESSAGES
   messages hold. The cursor is an index into the block, bounded by the block's count each tick. A
   record whose block is not one of the ten the engine deals (wire_block_ok: inside
   [begin, main+0x1435B], the array's last slot included, at its rank's place) sends an empty list.
   MEASURED 2026-11-21 (tools/mp_agree.py --marches 10: each peer's 1499 kbots ordered to move as
   one, ten times, 150 ticks apart): a unit's entry waited at most 4 ticks; nothing refused by the
   queue; the peers agree. */
#define STREAM_MAIN      0x00511DE8u
#define STREAM_RESUME    0x0048B80Eu        /* the terminator, after the list */
#define STREAM_BITS      0x00415C10u        /* the bit writer: thiscall (value, nbits), ret 8 */
#define STREAM_MESSAGES  4u                 /* messages a tick a record, at most: the bound on a tick's traffic */
#define STREAM_BITS_MAX  8191u              /* the writer's 0x100 dwords less the bit whose write would grow it */
#define STREAM_HEAD_BITS 56u                /* [8] code, [16] size, [32] GameTime */
#define STREAM_ENTRY_BITS(typeBits) (16u + (typeBits) + 213u)
#define STREAM_TAIL_BITS(typeBits)  (16u + 1u + (typeBits) + 211u)
#define STREAM_SEND      0x00451DF0u        /* the queue's send: stdcall (dpid, buf, size), ret 0xC; called 0x48B903 */
#define STREAM_RECV      0x0048B920u        /* the receiver: stdcall (rec, buf), ret 8; called 0x4553F4 */
#define STREAM_SLOTS     16384u             /* the unit array's slots, at most: 10 x 1500 + 1 */

typedef int  (__thiscall *PFN_StreamDirty)(void* course);
typedef void (__thiscall *PFN_StreamPayload)(void* course, void* writer);
typedef void (__thiscall *PFN_StreamBits)(void* writer, unsigned int value, unsigned int nbits);

/* each record's next unit to look at, an index into its block */
static unsigned int s_streamAt[10];
SIM_VAR(s_streamAt, SIM_STATE);

/* THE QUEUE'S MEASUREMENT, under `tagpu_netlog.on` (read at attach): after each list, every unit
   of the block still dirty, and for each unit sent the ticks since its first tick dirty unsent.
   Every 150 ticks one `netstream:` line a record. Nothing here is read but by the log. */
static int s_streamLog;
SIM_VAR(s_streamLog, SIM_INSTALL);
static unsigned int s_streamSince[STREAM_SLOTS];    /* GameTime + 1 of a unit's first tick left dirty */
SIM_VAR(s_streamSince, SIM_PRIVATE);
typedef struct STREAMDIAG {
    unsigned int ticks, sent, sentMost, left, leftMost, units, waited, waitSum, waitMost, waitLong;
    unsigned int msgs, bytes, bytesMost, refused, tickBytes, tickBytesMost, tickMsgsMost;
    int window;
} STREAMDIAG;
static STREAMDIAG s_streamDiag[10];
SIM_VAR(s_streamDiag, SIM_PRIVATE);
/* the record whose message the sender is building, for the lever's counts (stream_sent): set on every entry of
   stream_dirty_list, 10 while the record is not one of the ten */
static unsigned int s_streamCur;
SIM_VAR(s_streamCur, SIM_TRANSIENT);
/* the 0x2C messages this peer received from each record, a window of 150 ticks */
static struct { unsigned int msgs, bytes; } s_streamRecv[10];
SIM_VAR(s_streamRecv, SIM_PRIVATE);
static int s_streamRecvWindow;
SIM_VAR(s_streamRecvWindow, SIM_PRIVATE);

static void stream_log(unsigned int p, int now)
{
    STREAMDIAG* d = &s_streamDiag[p];
    if (d->window != now / 150) {
        if (d->ticks)
            tagpu_logf("netstream: record %u, %u ticks of a block of %u units: sent %.1f a tick (most %u); "
                       "left dirty %.1f a tick (most %u); a unit's wait from its first tick left dirty to "
                       "its entry: %u units, mean %.1f ticks, most %u, 30 ticks or more %u; "
                       "%u messages, %u bytes (most %u), %u refused by the queue; a tick's most: %u bytes in "
                       "%u messages",
                       p, d->ticks, d->units, (double)d->sent / d->ticks, d->sentMost,
                       (double)d->left / d->ticks, d->leftMost, d->waited,
                       d->waited ? (double)d->waitSum / d->waited : 0.0, d->waitMost, d->waitLong,
                       d->msgs, d->bytes, d->bytesMost, d->refused, d->tickBytesMost, d->tickMsgsMost);
        memset(d, 0, sizeof *d);
        d->window = now / 150;
    }
}

/* a message to the queue (0x451DF0), and under the lever its size and whether the queue answered 0 (0x462710
   drops a message it cannot hold). stream_sent is the same for the engine's own send of a tick's last message. */
static int __stdcall stream_sent(int dpid, void* buf, unsigned int size)
{
    const int r = ((int (__stdcall*)(int, void*, unsigned int))(size_t)STREAM_SEND)(dpid, buf, size);
    if (s_streamLog && s_streamCur < 10) {
        STREAMDIAG* d = &s_streamDiag[s_streamCur];
        d->msgs++;
        d->bytes += size;
        if (size > d->bytesMost) d->bytesMost = size;
        if (!r) d->refused++;
        d->tickBytes += size;
    }
    return r;
}

/* THE RECEIVER'S STEP ON A TICK'S LAST MESSAGE [DISASSEMBLED 2026-11-21]. After a 0x2C's list the
   receiver 0x48B920 runs, for every live unit of the sender's block with a mover, the movement pass
   0x43DD20 and the sight pass 0x48A870 (0x48BA28..0x48BA5C), then reads the flag bit after the
   list's end (0x48BA5E..0x48BA97): set, a round-robin entry follows; clear, the message ends
   (0x48BAC8). Stock's sender always sets it (0x48B81B..0x48B84C). So a remote unit moves one step a
   message, and a second message in a tick would move it twice. Branch at 0x48BA28, before the
   sweep's two loads: 0 -> 0x48BA5E, no sweep, for a message whose flag is clear; else the sweep.
   A stopped parse (the reader on s_wireZero: B3's stop, for a malformed entry or a dirty create
   that did not take) keeps stock's sweep, whichever of a tick's messages it stopped in -- the stop
   cannot tell them apart -- so the sender's units step once more in that tick, as they do for
   every stopped message in stock. The flag is the bit stock reads
   next: inside B3's zero-padded copy when that reader is the one in use. */
static int __cdecl stream_step(unsigned int* r)
{
    const unsigned char* rd = WPN_ESP_JMP(r) + 0x10;
    const unsigned int* buf = *(const unsigned int* const*)rd;
    unsigned long long pos;
    if (buf == &s_wireZero) return 1;
    pos = wire_pos(rd);
    return (int)((buf[pos / 32u] >> (pos % 32u)) & 1u);
}

static void __cdecl stream_dirty_list(char* rec, unsigned int* writer)
{
    const PFN_StreamBits bits = (PFN_StreamBits)(size_t)STREAM_BITS;
    char* const ta = *(char* const*)(size_t)STREAM_MAIN;
    char *first, *last, *begin, *end, *players;
    unsigned int n, k, i, p, sent = 0, left = 0, slot0, msgs = 1, typeBits, room;
    const unsigned int most = tagpu_rec_mp_revision_held() ? STREAM_MESSAGES : 1u;
    int now;
    s_streamCur = 10;
    if (!ta) return;
    first = *(char* const*)(rec + 0x67);
    last = *(char* const*)(rec + 0x6B);
    begin = *(char* const*)(ta + 0x14357);
    end = *(char* const*)(ta + 0x1435B);
    if (first > last || (size_t)(last - first) % 0x118) return;
    n = (unsigned int)(last - first) / 0x118 + 1;
    if (!wire_block_ok(first, last, begin, end, n)) return;     /* end is the array's last slot */
    slot0 = (unsigned int)(first - begin) / 0x118;
    players = ta + 0x1B63;
    p = rec >= players && rec < players + 10 * 0x14B && (size_t)(rec - players) % 0x14B == 0
            ? (unsigned int)(rec - players) / 0x14B : 10;
    k = p < 10 && s_streamAt[p] < n ? s_streamAt[p] : 0;
    now = *(const int*)(ta + 0x38A47);
    typeBits = *(const unsigned int*)(ta + 0x14393);
    if (typeBits > 16u) return;                     /* a type is a u16 (+0xA6) */
    room = STREAM_BITS_MAX - STREAM_ENTRY_BITS(typeBits) - STREAM_TAIL_BITS(typeBits);
    s_streamCur = p;
    if (s_streamLog && p < 10) {        /* the last tick's bytes are whole now: its last message left after the list */
        STREAMDIAG* d = &s_streamDiag[p];
        if (d->tickBytes > d->tickBytesMost) d->tickBytesMost = d->tickBytes;
        d->tickBytes = 0;
        stream_log(p, now);
    }
    for (i = 0; i < n; i++) {
        const unsigned int at = (k + i) % n;
        char* const unit = first + (size_t)at * 0x118;
        char* mover;
        void* course;
        if (!(*(const unsigned int*)(unit + 0x110) & 0x10000000u) || !(mover = *(char**)unit)) continue;
        course = *(void**)mover;
        if (!(*(PFN_StreamDirty*)(*(char**)course + 0x1C))(course)) continue;
        if (writer[0] * 32u + writer[1] > room) {       /* THE SIZE BOUND: this entry might not fit */
            unsigned char* const buf = (unsigned char*)(size_t)writer[3];
            unsigned int size;
            if (msgs >= most) {                     /* the tick's traffic is spent: this unit is the next tick's first */
                if (p < 10) s_streamAt[p] = at;
                break;
            }
            msgs++;
            /* the message in hand, a list alone: its end, the flag clear, its size as the engine's
               tail writes it (0x48B8A9..0x48B8E3), sent as that tail sends (0x48B8FD..0x48B903) */
            bits(writer, 0xFFFFu, 16);
            bits(writer, 0, 1);
            size = writer[0] * 4u + ((writer[1] + 7u) >> 3);
            buf[1] = (unsigned char)size;
            buf[2] = (unsigned char)(size >> 8);
            stream_sent(*(const int*)(rec + 4), buf, size);
            /* the next on the same writer, as 0x415B60 leaves one, with the header 0x48B72A..0x48B74E writes */
            writer[0] = 0;
            writer[1] = 0;
            *(unsigned int*)buf = 0;
            bits(writer, 0x2Cu, 8);
            bits(writer, 0, 16);
            bits(writer, (unsigned int)now, 32);
        }
        bits(writer, (unsigned int)(*(const unsigned short*)(unit + 0xA8) -
                                    *(const unsigned short*)(*(char* const*)(unit + 0x96) + 0x6F)), 16);
        bits(writer, *(const unsigned short*)(unit + 0xA6), typeBits);
        (*(PFN_StreamPayload*)(*(char**)course + 0x20))(course, writer);
        sent++;
        if (s_streamLog && slot0 + at < STREAM_SLOTS) {
            unsigned int* since = &s_streamSince[slot0 + at];
            if (*since) {
                STREAMDIAG* d = &s_streamDiag[p < 10 ? p : 0];
                const unsigned int w = (unsigned int)(now + 1) - *since;
                d->waited++;
                d->waitSum += w;
                if (w > d->waitMost) d->waitMost = w;
                if (w >= 30) d->waitLong++;
            }
            *since = 0;
        }
    }
    if (!s_streamLog || p >= 10) return;
    s_streamDiag[p].sent += sent;
    for (i = 0; i < n; i++) {
        char* const unit = first + (size_t)i * 0x118;
        char* mover;
        void* course;
        int dirty = 0;
        if ((*(const unsigned int*)(unit + 0x110) & 0x10000000u) && (mover = *(char**)unit)) {
            course = *(void**)mover;
            dirty = (*(PFN_StreamDirty*)(*(char**)course + 0x1C))(course);
        }
        if (slot0 + i >= STREAM_SLOTS) break;
        if (!dirty) s_streamSince[slot0 + i] = 0;
        else {
            left++;
            if (!s_streamSince[slot0 + i]) s_streamSince[slot0 + i] = (unsigned int)now + 1;
        }
    }
    {
        STREAMDIAG* d = &s_streamDiag[p];
        d->ticks++;
        d->units = n;
        if (sent > d->sentMost) d->sentMost = sent;
        if (msgs > d->tickMsgsMost) d->tickMsgsMost = msgs;
        d->left += left;
        if (left > d->leftMost) d->leftMost = left;
    }
}

/* the measurement's receive: the messages and bytes each sending record's 0x2C reached this peer with */
static void __stdcall stream_recv(char* rec, unsigned char* buf)
{
    char* const ta = *(char* const*)(size_t)STREAM_MAIN;
    char* const players = ta ? ta + 0x1B63 : NULL;
    if (ta) {
        const int now = *(const int*)(ta + 0x38A47);
        if (s_streamRecvWindow != now / 150) {
            unsigned int i;
            for (i = 0; i < 10; i++)
                if (s_streamRecv[i].msgs)
                    tagpu_logf("netstream: received from record %u: %u messages, %u bytes", i,
                               s_streamRecv[i].msgs, s_streamRecv[i].bytes);
            memset(s_streamRecv, 0, sizeof s_streamRecv);
            s_streamRecvWindow = now / 150;
        }
        if (rec >= players && rec < players + 10 * 0x14B && (size_t)(rec - players) % 0x14B == 0) {
            const unsigned int i = (unsigned int)(rec - players) / 0x14B;
            s_streamRecv[i].msgs++;
            s_streamRecv[i].bytes += (unsigned int)buf[1] | (unsigned int)buf[2] << 8;
        }
    }
    ((void (__stdcall*)(char*, unsigned char*))(size_t)STREAM_RECV)(rec, buf);
}

/* CLASS: simulation, fail closed. A peer without the receiver's part steps a sender's units once a message, up
   to STREAM_MESSAGES times a tick, and its copies run ahead of their owners; a peer without the sender's part
   sends its units with no bound on the wait. Both sites are the table's: a launch where either does not hold
   stock's bytes, or is rewritten before the first DirectDraw call, is refused (tagpu_limits_report). */
static int fix_unit_stream(void)
{
    static const unsigned char was[9] = {
        0x3B, 0x77, 0x6B,                       /* cmp esi,[edi+0x6b] */
        0x0F, 0x87, 0x99, 0x00, 0x00, 0x00,     /* ja 0x48B80E        */
    };
    unsigned char *c, *p;
    s_streamLog = tagpu_sim_lever("tagpu_netlog.on");
    c = p = tagpu_detour_stub();
    if (!c) { lim_no_stub(); return FIX_TABLE; }
    *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x0C;                 /* lea eax,[esp+0xc]: the writer */
    *p++ = 0x50;                                                        /* push eax                      */
    *p++ = 0x57;                                                        /* push edi: the record          */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)stream_dirty_list); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                              /* add esp,8                     */
    *p++ = 0xE9; tagpu_detour_rel(p, STREAM_RESUME);                    /* the terminator                */
    lim_branch(0x0048B76Cu, (int)sizeof was, was, 0xE9, (unsigned int)(size_t)c,
               "unit stream: the dirty list in turn");
    {   /* the receiver's sweep, taken only for a message whose flag is set (stream_step) */
        static const unsigned char sweep[8] = {
            0x8B, 0x77, 0x67,               /* mov esi,[edi+0x67] */
            0x8B, 0x47, 0x6B,               /* mov eax,[edi+0x6b] */
            0x3B, 0xF0,                     /* cmp esi,eax        */
        };
        unsigned char* st = tagpu_detour_stub();
        if (!st) { lim_no_stub(); return FIX_TABLE; }
        wire_emit_tail(st, stream_step, 0x0048BA5Eu, sweep, 8, 0x0048BA30u);
        lim_branch(0x0048BA28u, (int)sizeof sweep, sweep, 0xE9, (unsigned int)(size_t)st,
                   "unit stream: the receiver's step on a tick's last message");
    }
    if (s_streamLog && !(tagpu_detour_redirect(0x0048B903u, STREAM_SEND, (void*)stream_sent) &&
                         tagpu_detour_redirect(0x004553F4u, STREAM_RECV, (void*)stream_recv)))
        tagpu_log("netstream: the send and receive counts could not be installed");
    return FIX_TABLE;
}

SIM_FEATURE(engine_defects, SIM_PUPPET_RUNS, "battle");

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
    int phase = fix_step_phases();
    int slice = fix_script_slice();
    int kq   = fix_killed_query();
    int flak = fix_flak_divides();
    int shat = fix_shatter_rng();
    int edge = fix_last_cell();
    int los  = fix_los_shear();
    int losl = fix_los_local();
    int pview = fix_projectile_view();
    int wkey = fix_weapon_keys();
    int vet  = fix_veterancy();
    int xw   = fix_extra_weapons();   /* after veterancy: its observers chain onto the unit keys' */
    int rclm = fix_reclaim_wrap();
    int word = fix_hit_word();
    int kout = fix_kill_outright();
    int once = fix_meteor_once();
    int radar = fix_radar_owner();
    int wire = fix_wire_bounds();
    int hits = fix_stale_hits();
    int ghost = fix_ghost_commander(wire);
    int kills = fix_kill_counts();
    int built = fix_built_bounds();
    int tx   = fix_transported();
    int maps = fix_map_terrain();
    int mmpic = fix_map_picture();
    int unp  = fix_load_unpaused();
    int pwalk = fix_particle_walk();
    int pools = fix_effect_pools();
    int debris = fix_debris_emission();
    int hover = fix_hover_clock();
    int stamp = fix_mover_stamp();
    int site = fix_planner_site();
    int rsite = fix_reclaim_site();
    int aisend = fix_ai_economy_send();
    int aitax  = fix_ai_untaxed();
    int aishr  = fix_ai_autoshare();
    int ustream = fix_unit_stream();
    int rtrig = fix_repose_trig();
    char b[2048], fn[LOS_NFN * 9 + 1];
    int k;

    if (tagpu_sim_lever("tagpu_wirecheck.on"))
        wire_selfcheck();
    tagpu_logf("enginefix: the repose takes its trigonometry once a piece (0x45B179, the vertex "
               "loop of 0x45B150; 0x4B6CC0 and 0x4B7173 matched) %s%s", fix_state(rtrig),
               s_rtrigCheck ? "; the oracle compares every vertex with stock's" : "");

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
              "divides (0x49CF18 0x42F314 0x42F32E) %s; the shatter's own random numbers (0x421851 "
              "0x421868 0x42187F 0x4218AA 0x4218BE 0x4218D2 0x421B39 0x421B62) %s; the map's last row and column "
              "(0x47CC8B 0x47CCA3 0x47CCA9) %s. Counters: unit repeats "
              "refused at 0x%08X, "
              "feature repeats "
              "refused at 0x%08X, victims refused off their arrays at 0x%08X, list blocks run "
              "unwrapped at 0x%08X, flak fallbacks at 0x%08X",
              fix_state(sort), fix_state(plot), fix_state(terr), fix_state(die), fix_state(mark),
              fix_state(sfb), fix_state(rro), fix_state(scr), fix_state(list), fix_state(dl),
              fix_state(oom), fix_state(keys), fix_state(wpn), fix_state(caps), fix_state(flak),
              fix_state(shat), fix_state(edge),
              (unsigned int)(size_t)&s_dmgDiag.unitRepeats, (unsigned int)(size_t)&s_dmgDiag.featRepeats,
              (unsigned int)(size_t)&s_dmgDiag.refused, (unsigned int)(size_t)&s_dmgDiag.unframed,
              (unsigned int)(size_t)&s_flakDiag.fallbacks);
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
              (unsigned int)(size_t)s_losDiag.ownRow, fn);
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: stacked aircraft served to area damage after its walk (0x49A664 "
              "0x49A415; the pool: the stamp 0x47CF98, the unit tick's call 0x4954ED) %s. "
              "Aircraft served, counted at 0x%08X",
              fix_state(air), (unsigned int)(size_t)&s_airDiag.served);
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b, "enginefix: the step's phases for the demo recorder and its replay "
              "(0x495513 0x495539) %s; the script slice 0x48ADDF %s; a replayed death's Killed "
              "0x486877 %s", fix_state(phase), fix_state(slice), fix_state(kq));
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
              "enginefix: veterancy -- VeterancyThresholds, VeterancyAccuracyBuffRate (the unit "
              "keys 0x42D2E0 0x42BF40 0x42BF97; damage taken 0x489BFA; dealt 0x499DB5; reload "
              "0x49E468; lead 0x48A324; spread 0x49D6EA; the capture's cost 0x4043D8; a unit "
              "reclaim's step 0x43869D) %s",
              fix_state(vet));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: extra weapons -- Weapon4..N a unit type (%d sites: 19 entry hooks, 21 "
              "splices, 4 byte patches; the record observers 0x42D2E0 0x42BF40) %s",
              TAGPU_WPN_SITES, fix_state(xw));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: transported explosions -- TransportedExplodeAs, "
              "TransportedSelfDestructAs (the unit keys 0x42BF97; the pick 0x49B017; the "
              "decision 0x48664B; the destructor call 0x486679; the detaches 0x4867CB and 0x48681D; the cargo loop 0x486810; "
              "kill-all 0x486F10; the sync fold 0x42B019) %s",
              fix_state(tx));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: B7 -- a hit's amount saturated into its word (0x489C71) %s; "
              "kill-outright skips the veterancy reduction (0x489BF3) %s; one meteor one hit, "
              "a received stone's damage left to its spawner (0x49A01B 0x49DF7D 0x49D307) %s; "
              "the radar's owner test with no attacker (0x4673B1) %s",
              fix_state(word), fix_state(kout), fix_state(once), fix_state(radar));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: a minimap picture smaller than the shrink reads (0x46685F) %s; maps with "
              "no terrain -- left out of the map list (0x434D87) and "
              "never kept as the saved skirmish map (0x430B87) %s. Maps left out counted at "
              "0x%08X, saved maps set aside at 0x%08X",
              fix_state(mmpic), fix_state(maps), (unsigned int)(size_t)&s_mapsDiag.noTerrain,
              (unsigned int)(size_t)&s_mapsDiag.savedNoTerrain);
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: a level starts unpaused outside a network game, the pause bit "
              "main+0x38A51 cleared before its first tick (0x49842A) %s",
              fix_state(unp));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: the particle layers' tick keeps its survivors in one pass (0x4955BF) %s",
              fix_state(pwalk));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: the effect pools start every level empty -- the explosions' records, "
              "the debris records with their blocks and the flying-piece slots (0x42090A) %s",
              fix_state(pools));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: a flying piece's smoke and fire once per GameTime step, not once a draw "
              "(latch 0x420B00; 0x421583, 0x4215AA) %s", fix_state(debris));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: the hover wave on GameTime, not the wall clock (0x48A66A) %s; a mover's "
              "moved-at stamp written in its constructor (0x43DC00) %s; the AI builder planner's "
              "site altitude written (0x4081E2) %s; the patrols' reclaim search's altitude "
              "written (0x47EA79) %s",
              fix_state(hover), fix_state(stamp), fix_state(site), fix_state(rsite));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: AI allies as human ones -- a hosted AI's 0x28 beside the viewed record's "
              "(0x4655A1) %s; a give to an AI in full (0x464D3A 0x464C0A) %s; the auto-share for AI "
              "allies, in a skirmish too, for every record a peer controls (0x457DD8 0x457DE4 "
              "0x457EF6 0x457F02 0x457D44 0x49560B 0x49563A) %s",
              fix_state(aisend), fix_state(aitax), fix_state(aishr));
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: the unit stream -- its dirty list in turn, in up to %u messages a tick (0x48B76C), "
              "the receiver stepping on the tick's last (0x48BA28) %s%s", STREAM_MESSAGES, fix_state(ustream),
              s_streamLog ? "; its queue logged (tagpu_netlog.on)" : "");
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: B10 -- a unit reclaim's step, its product in 64 bits and its quotient "
              "clamped (0x4386B9 0x4386D8) %s",
              fix_state(rclm));
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
              "enginefix: ghost commander %s: a carried 0x09 the gate refuses in state 5 held per "
              "sender in the 0x05 receiver (0x455F90) and replayed through it before the first "
              "tick (0x49842F), one refused in a catch-up tick made at once, a death refused in "
              "state 5 cancelling its held create (the 0x4C in 0x455F90; a bare 0x0C counted "
              "at 0x45477F), the queue emptied at the load's start "
              "(0x497F5E); the dirty create's position from its move payload (0x48BA00). Counters "
              "on the heartbeat's 'ghost:' section. Stubs: %u of 4096 bytes at 0x%08X",
              fix_state(ghost), s_ghostCodeUsed, (unsigned int)(size_t)s_ghostCode);
    b[sizeof b - 1] = 0;
    plog(b);

    _snprintf(b, sizeof b,
              "enginefix: kill counts %s: the 0x0C carried in a tagged 0x05 with its victim's "
              "build fraction (0x48666D; the receiver 0x455F90; a bare one dropped 0x455FAC), the "
              "0x09's carrying its unit's fraction and HP, held across the post-create writes of "
              "the capture (0x488700; flushed 0x488743 0x488791), the placed units (0x488462; "
              "0x4884AC) and the resurrection (0x405104; 0x405119 0x405155 0x405164) by the send's "
              "gate (0x451DF0). Counters on the heartbeat's 'kills:' section. The 0x12's two "
              "indices bounded (0x4555BA) %s",
              fix_state(kills), fix_state(built));
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

SIM_FEATURE(test_seeds, SIM_PUPPET_RUNS, "battle randprobe");

/* A HARNESS'S RANDOM STREAMS [DISASSEMBLED]. Stock starts every level from three random states,
   each seeded from a clock:
     - the simulation RNG 0x51FC88 (Park-Miller, drawn through 0x4B6C30): the loader 0x497180
       sums QueryPerformanceCounter's two dwords (0x49718C..0x49719A) and seeds it through
       0x4B6CA0 (stdcall, `ret 4`: state = seed ^ 0x66E29572 | 1) at 0x49719D. It reaches the
       burst spray, the accuracy spread, the self-destruct's last countdown step, a structure's
       heading and a unit's hover phase at its create, COB RAND and the AI;
     - the loader thread's CRT stream: `srand(time(0))` at 0x4971A5..0x4971AE. The loader draws
       the skirmish start positions from it (0x4979E7, 0x497A32);
     - the game thread's CRT stream, seeded once by WinMain (`srand(time(0))`, 0x49E8B2..
       0x49E8BB), which the draw path draws every drawn frame, and so, in a player's game, does
       the meteor shower.
   rand 0x4E4870 keeps its state per thread at +0x14 of 0x4EB0F0's block, which srand 0x4E4860
   writes (0x4E4860..0x4E486C). Of rand's 61 call sites the rest are presentation, the front end
   or network-only code (engine/sim.md, *The two random streams*), the wind's 0x490C60, which
   fix_wind replaces, the meteor tick's, which fix_meteor_stream replaces under the switch, and
   the objective's
   0x490385, which nothing reaches.
   THE SWITCH. The game folder's `tagpu_seeds.on`, a lever (tagpu_simreg.h) the automated
   harnesses alone write -- every tareplay Worker, so the replay test groups, the calibration's
   lanes, bar.py, eco_bench and the ImpAI tools that play through one -- and honoured only in a
   test launch (tagpu_regstore_active), read once at install and listed in a recording's header.
   A player's launch, `tacli launch --defaults` and the compatibility suite's games never write it
   and keep stock's seeds: every skirmish there starts from its own random state (owner's
   decision, 2026-11-14).
   UNDER THE SWITCH every level starts from the same states: the loader's two seeds are
   TEST_SEED_RNG for the counter and TEST_SEED_CRT for the time, and the game thread's stream is
   re-seeded with TEST_SEED_CRT on the game thread just before the loader thread starts (fix_wind's
   stub at 0x4982CA, test_seed_game_thread). The wind's and the meteors' seeds are wind_seed's,
   TEST_SEED_WIND outside a network game, and the meteors draw their own stream
   (fix_meteor_stream). The shatter's stream (fix_shatter_rng) is put back at its start with the
   simulation RNG's seed, 0x49719D running on the loader thread before the start units' creates
   (0x497963, 0x497ACD), the only draws a level makes before the load-done gate. And a scenario
   apply that clears the level re-seeds all of them from the wire's seed (tagpu_seed_run), the
   shatter's stream put back at its start, so a calibration run is a function of its
   scenario and its index rather than of the runs its lane played before it. The values are
   arbitrary: a test that wants another game changes its scenario's seed.
   0x49719D and 0x4971AE call test_rng_seed and test_crt_seed directly, with stock's arguments
   and conventions: 0x4B6CA0 is stdcall with one argument, srand is cdecl and its caller pops
   (0x4971B8). After the first, time() at 0x4971A5 clobbers eax, ecx and edx before any is read;
   after the second the loader writes eax (0x4971B3) and ecx (0x4971C1) before reading them, and
   edx on each of the mode dispatch's four ways before it reads it (0x4971F1, 0x4973C6, 0x497468,
   the call at 0x4974FC). Both are rows of the fail-closed table, so a harness's launch runs the
   bytes a player's runs.
   WHAT STAYS PER PROCESS: the heap's layout, `main`'s pad (0x41D924).
   CLASS: simulation, fail closed. */
#define TEST_SEED_RNG  0x5EED0001u
#define TEST_SEED_CRT  0x5EED0002u
#define TEST_SEED_WIND 0x5EED00035EED0003ull

static int s_testSeeds;                 /* tagpu_seeds.on in force: set once at install */
SIM_VAR(s_testSeeds, SIM_INSTALL);

typedef void (__stdcall* PFN_RngSeed)(unsigned int seed);
#define E_RngSeed ((PFN_RngSeed)0x004B6CA0u)
typedef void (__cdecl* PFN_Srand)(unsigned int seed);
#define E_Srand   ((PFN_Srand)0x004E4860u)
/* the wind updater, no argument, plain `ret` (A RUN'S SEEDS, THE WIND AT THE APPLY) */
typedef void (__cdecl* PFN_WindTick)(void);
#define E_WindTick ((PFN_WindTick)0x00490C40u)

/* 0x49719D, the LOADER thread */
static void __stdcall test_rng_seed(unsigned int counter)
{
    if (!s_testSeeds) {
        E_RngSeed(counter);
        return;
    }
    E_RngSeed(TEST_SEED_RNG);
    s_shatterRng = SHATTER_RNG_START;
    tagpu_logf("enginefix: test seeds: the level's simulation RNG seeded with 0x%08X, not the "
               "performance counter; the loader's and the game thread's CRT streams with 0x%08X, "
               "not time(0); the shatter's stream at its start", TEST_SEED_RNG, TEST_SEED_CRT);
}

/* 0x4971AE, the LOADER thread: its own stream */
static void __cdecl test_crt_seed(unsigned int now)
{
    E_Srand(s_testSeeds ? TEST_SEED_CRT : now);
}

/* the GAME thread, at the loader thread's start (fix_wind's stub at 0x4982CA): its own stream */
static void __cdecl test_seed_game_thread(void)
{
    if (s_testSeeds) E_Srand(TEST_SEED_CRT);
}

static int fix_test_seeds(void)
{
    static const unsigned char rng[5]  = { 0xE8, 0xFE, 0xFA, 0x01, 0x00 };   /* call 0x4B6CA0 */
    static const unsigned char crt[5]  = { 0xE8, 0xAD, 0xD6, 0x04, 0x00 };   /* call 0x4E4860 */
    static const unsigned char pops[3] = { 0x83, 0xC4, 0x04 };               /* add esp,4     */
    if (tagpu_sim_lever("tagpu_seeds.on")) {
        if (tagpu_regstore_active())
            s_testSeeds = 1;
        else
            plog("enginefix: tagpu_seeds.on refused: not a test launch (tagpu_regstore.h)");
    }
    lim_branch(0x0049719D, sizeof rng, rng, 0xE8, (unsigned int)(size_t)test_rng_seed,
               "test seeds: the simulation RNG's seed");
    lim_branch(0x004971AE, sizeof crt, crt, 0xE8, (unsigned int)(size_t)test_crt_seed,
               "test seeds: the loader thread's CRT seed");
    lim_same(0x004971B8, sizeof pops, pops, "test seeds: the loader pops srand's argument");
    return FIX_TABLE;
}

SIM_FEATURE(wind, SIM_PUPPET_RUNS, "battle");

/* ===== THE LOADERS AND TWO HUD DIVIDES =====================================================
   Five stock defects outside the battle itself: the wind every peer draws for itself, a
   yardmap parsed past the end of its string, the saved-game loader's order fallback, and two
   divides in the HUD (research/notes/tadr-port/sim-fixes.md, landing B6). The wind and the
   yardmaps change the simulation, so their sites are rows of the fail-closed table; the other
   three are local, each checked, written and skipped on its own. */

#define B6_MAIN        0x00511DE8u
#define B6_TNT         0x391E9u       /* main+ : GameingState*; its +0x204 the TNT path    */
#define B6_NETWORK     3              /* GameingState's +0: 1 campaign, 2 skirmish, 3 network */
#define B6_DPLAY       0x4D9u         /* main+ : IDirectPlay3A* or, lobbied, 2A*; NULL: none */
#define B6_DESC_GUID   0x479u         /* main+ : guidInstance of the engine's session copy */

/* THE WIND [DISASSEMBLED]. The wind updater 0x490C40 has two callers: the level load
   LoadGameData_Main 0x4917D0 at 0x491903, on the loader thread, and the tick 0x495490 at
   0x49558F. While next (main+0x37EC4) >= GameTime (main+0x38A47) it clears the changed flag
   +0x37EE2 and returns (0x490C51, 0x490D87). Otherwise it draws three values:
     - the schedule, next += 30 * (5 + rand() * 10 / 0x8000), from the CRT's rand 0x4E4870
       (0x490C60..0x490C8E);
     - the speed +0x37EDA = min (main+0x1425B) + simrand(max (main+0x1425F) - min), from the
       sim RNG 0x4B6C30, which answers 0 for an argument below 2 (0x4B6C35) (0x490C90..0x490CB7);
     - when the speed is not 0, the heading +0x37ED8 = simrand(0x10000) (0x490CC8..0x490CDC);
   then derives the components +0x37ECC and +0x37ED4 (0x490CE8..0x490D35) and the ratio
   +0x37EDE, capped at 1.0 (0x490D3B..0x490D7B). The wind is simulation state: the projectile
   pass 0x49B720 adds the components to a projectile's position (0x49BC58, 0x49BD04), the fire
   spread 0x4239C0 reads them (0x423AA1, 0x423AC1), and a wind generator's energy follows the
   ratio (0x40156F). The sim RNG is seeded from QueryPerformanceCounter by the loader 0x497180;
   the CRT's rand keeps its state per thread, and the game thread's, which the tick's draw
   uses, is seeded from time(0) by WinMain (0x49E8BB). So each peer draws a wind of its own:
   MEASURED on two peers paused at GameTime 825, speed 2525 heading 0xF5C6 on the host against
   1498 and 0xC827 on the joiner.
   The load's call finds next = 0 (0x4918FD) and GameTime 0 (0x4971BB), so it draws nothing;
   the first draw is the first tick's.
   THE FIX draws the three values from our own generator (splitmix64, its own state; neither
   engine RNG) with stock's rules: next += 30 * (5 + r % 10); the speed min + r % (max - min),
   or min when max - min, taken in 32 bits as 0x490CA1 takes it, is below 2; the heading
   r % 0x10000, drawn only when the speed is not 0. A jmp at 0x490C5A, the draws' first
   instruction (after 0x490C59's push esi), goes to a stub that calls wind_draw and rejoins at
   0x490CE8 with eax the main pointer, as stock's 0x490CE3 leaves it; esi, which stock points
   at `next`, is read after 0x490CE8 only by its pop at 0x490D85. The load's call at 0x491903
   goes through a stub that seeds the generator first: in a network game from DirectPlay's
   session instance GUID and a hash of the map's name, or from the map's hash alone when
   DirectPlay names no session; otherwise, under the harness's switch, from TEST_SEED_WIND and
   the map's hash (*A harness's random streams*), and without it from QueryPerformanceCounter,
   the counter stock seeds its own RNG with.
   THE SESSION, NOT THE HOST [DISASSEMBLED]. The engine's DirectPlay object (its trace strings
   "HAPINET_*" at 0x50B0AC..0x50B4A0) lives in main at +0x14. main+0x4D9 is the interface every
   call goes through: an IDirectPlay3A from QueryInterface (IID 0x4FCD78, at 0x4CA684), or, when
   a lobby launched the game, the IDirectPlay2A its Connect returns (0x4C9B59); GetSessionDesc is
   slot 22 of both (vtable +0x58). Released and zeroed at 0x4C9B92..0x4C9BA1. main+0x471 is the
   engine's DPSESSIONDESC2 copy.
   The host's DirectPlay ID can change during the load: until bit 1 of main+0x38D75 is set the
   game thread pumps the network in its load loop (0x4984DD..0x49852E), and the pump's leave
   case removes a remote human even during the load (0x452CC0: the seat's type cleared at
   0x452E62, its ID set to -1 at 0x452E6D) and, when the leaver held the host bit, elects the
   human seat with the highest ID (0x452EE3..0x452FF8). A seed read from the host seat would
   depend on whether a peer handled a departure before its 0x491903.
   The session's instance GUID is fixed when DirectPlay creates the session, and every peer
   holds it from the moment it enters (a joiner names it to Open(DPOPEN_JOIN), 0x4CA03A,
   0x4CA05A). No departure and no host election writes it. The one engine call that could is
   SetSessionDesc, which passes the engine's whole copy, guidInstance included; an
   implementation that takes guidInstance from it (Wine's copies the whole descriptor) would move
   the session to whatever the copy holds, and after a lobbied launch that is the lobby's
   descriptor (0x4C9B4F), not DirectPlay's. The engine makes it at exactly one site: of the six
   calls through a vtable's +0x7C in the image, 0x4C9903 in HAPINET_updategameinfo 0x4C9890 is
   the only one through main+0x4D9 and the only one with three arguments (0x47C0CB, 0x4B4FB8,
   0x4B5735, 0x4B6069 and 0x4B60E1 push two). 0x4C9890 is called from 0x451180 (0x451208) and
   from the pump 0x453D40 (0x454135): the battle room's updates, the pump's, and every load's
   end on the loader thread (0x497BFF sets bit 4 of the local PlayerInfo's +0x9B, 0x497C0B
   calls 0x451180, which then sets DPSESSION_JOINDISABLED). So a jmp over the call's nine
   bytes, 0x4C98FD..0x4C9905 (`push 0; mov ecx,[esi]; push eax; push esi; call [ecx+0x7C]`, eax
   the descriptor main+0x471, esi the interface main+0x4D9, not null past 0x4C98A9's test; no
   branch or absolute reference lands inside), goes to wind_set_session_desc: it reads the
   GUID DirectPlay holds now, validated as below, and makes the call with a copy of the engine's
   descriptor carrying that GUID, counted when it differed; with no valid answer it makes no
   call and returns the refusal, counted, which 0x4C9908 takes as it takes a failed call (0 out
   of 0x4C9914). No SetSessionDesc the engine makes can then move the session, whatever the
   implementation does with the field, and the engine's copy is not written. It rejoins at
   0x4C9906 (`test eax,eax`) with eax the HRESULT; ecx and edx are clobbered as the COM call
   clobbers them, and nothing reads them before 0x4C9914's pops. On whichever thread reaches
   it, the GetSessionDesc runs right before stock's own SetSessionDesc on that thread, so it adds
   no DirectPlay concurrency stock does not already have.
   The capture runs GetSessionDesc on the GAME THREAD, from a stub on the call that starts the
   loader thread, 0x4982CA (`call 0x4B6B20`, the CRT's _beginthread over CreateThread).
   0x497C70 is started only there and the loader body 0x497180 is called only from it
   (0x497CA1), so every seed at 0x491903 follows the capture made for its own load, and thread
   creation orders the capture's stores before anything the new thread reads. At that call the
   loader thread does not exist yet; stock lets it call DirectPlay concurrently with the game
   thread once it runs (its wait pumps the network at 0x49727D while the game thread pumps at
   0x49852E, and it makes the SetSessionDesc above), so a call on the game thread before it
   starts adds no concurrency that stock does not already have. The same stub then calls
   test_seed_game_thread, which re-seeds the game thread's CRT stream under the harness's
   switch.
   A successful answer is used only when its dwSize is sizeof(DPSESSIONDESC2) and its
   guidInstance is not null (Wine can answer DP_OK with a zeroed descriptor). Otherwise the seed
   takes the engine's copy main+0x479 when it is not null: it held DirectPlay's answer on every
   peer MEASURED, but the pump rewrites it from every DPSYS_SETSESSIONDESC on every peer but the
   host (0x454679, 0x454689), so it is the second choice. Then the map alone. Each is counted.
   GetSessionDesc fails only with no interface or no open session, and a peer with no open
   session exchanges no game traffic, so its wind changes nothing another peer sees.
   THE INVARIANT: in a network game every peer draws the same wind at the same GameTime. It
   rests on
     - the seed, the same on every peer and made again at every level load, so nothing crosses
       from one game to the next. Which path it takes is the engine's own network test,
       GameingState's +0 == 3 (0x435100 is `mov eax,[ecx]`; the load dispatches on it at
       0x4971C7), and inside a network game no path reads anything local to a peer: the
       session's GUID, which no departure changes, from DirectPlay or else from the engine's
       copy, or nothing; and the map, the TNT stem every peer loaded (GameingState +0x204,
       "Maps\Two Continents.TNT" on both peers MEASURED);
     - the draws, made only by the tick's call, once for each GameTime value on every peer, each
       consuming a number of values fixed by the map's range and the values drawn before it;
     - an ordering: the GUID is captured on the game thread before the loader thread is
       created; the seed is written on the loader thread before the loader's last store, bit 1
       of main+0x38D75 (0x497C62), and read only by the tick, which the game thread runs after
       it has seen that bit (0x498342): the handshake every value of the load rests on.
   CLASS: simulation, fail closed. A peer without it draws another wind. */
static unsigned long long s_windState;          /* loader thread writes, then GAME THREAD   */
static const SimField wind_state_layout[] = { SIM_FIELD_AT(0, 8, SIMF_OPAQUE, SIM_ID_NONE),
                                              SIM_FIELD_END };
SIM_VAR_L(s_windState, SIM_STATE, wind_state_layout);
/* the meteor shower's own stream (fix_meteor_stream), seeded with the wind's at the level load */
static unsigned long long s_meteorState;        /* loader thread writes, then GAME THREAD   */
SIM_VAR_L(s_meteorState, SIM_STATE, wind_state_layout);
/* the wind's diagnostics, peekable */
static struct {
    volatile LONG levels;                       /* level loads seeded                        */
    DWORD         descSize;                     /* the refused answer's dwSize, or 0         */
    volatile LONG fromCopy;                     /* network loads seeded from the copy        */
    /* the SetSessionDesc site 0x4C9903, any thread that reaches it */
    volatile LONG setCalls;                     /* calls made, with DirectPlay's GUID        */
    volatile LONG setDiffered;                  /* of those, the engine's GUID was another   */
    volatile LONG setWithheld;                  /* no valid answer: no call made             */
    volatile LONG mapOnly;                      /* network loads with no session             */
} s_windDiag;
SIM_VAR(s_windDiag, SIM_PRIVATE);

enum { WIND_NONE, WIND_DPLAY, WIND_COPY };        /* where this load's GUID came from */

/* the GAME THREAD writes these at 0x4982CA; the loader thread that call creates reads them */
static GUID    s_windSession;                   /* the GUID the network seed takes           */
SIM_VAR(s_windSession, SIM_TRANSIENT);
static int     s_windSource;                    /* WIND_*                                    */
SIM_VAR(s_windSource, SIM_TRANSIENT);
static GUID    s_windEngineCopy;                /* main+0x479 as the capture found it        */
SIM_VAR(s_windEngineCopy, SIM_TRANSIENT);
static HRESULT s_windSessionHr;                 /* GetSessionDesc's answer when refused      */
SIM_VAR(s_windSessionHr, SIM_TRANSIENT);

/* THE PLAYBACK'S SEED. A level that plays a recording draws the recording's wind, as a peer draws
   its session's: the replay's first step sets the state the recording's first image holds, on the
   game thread at the capture point of the level's first iteration, before that tick's unit tick
   and its draw (0x49558F). The image is taken at the start of the level's first tick (0x4954ED),
   after the load's seed and before any draw, the load's call drawing nothing; the host's load
   leaves next = 0 as the recording machine's did. So both draw from the same state at the same
   GameTimes, and THE INVARIANT makes every draw after it the recording's. */
const char* tagpu_wind_state_name(void) { return simdir_s_windState.name; }

void tagpu_wind_state_set(unsigned long long state) { s_windState = state; }

static unsigned long long seed_mix(unsigned long long* state);

static unsigned int wind_next(void)
{
    return (unsigned int)(seed_mix(&s_windState) >> 32);
}

static int wind_guid_null(const GUID* g)
{
    static const GUID zero;
    return memcmp(g, &zero, sizeof zero) == 0;
}

/* DirectPlay's own record of the session's GUID: 1 with *out set when the answer has dwSize
   sizeof(DPSESSIONDESC2) and a non-null GUID (Wine can answer DP_OK with a zeroed descriptor),
   else 0 with *hr and *dwSize saying why */
static int wind_dplay_guid(IDirectPlay2A* dp, GUID* out, HRESULT* hr, DWORD* dwSize)
{
    union { DPSESSIONDESC2 desc; unsigned char b[0x400]; } local;
    unsigned char* buf = local.b;
    DWORD size = sizeof local;
    int ok = 0;
    /* zeroed, as the heap buffer is: an answer that writes less than a descriptor leaves zeros,
       which the dwSize and GUID tests refuse, never a previous frame's bytes */
    memset(&local, 0, sizeof local);
    *dwSize = 0;
    *hr = IDirectPlay2_GetSessionDesc(dp, buf, &size);
    if (*hr == DPERR_BUFFERTOOSMALL && size > sizeof local) {
        buf = (unsigned char*)tagpu_sim_scratch(size);
        *hr = buf ? IDirectPlay2_GetSessionDesc(dp, buf, &size) : E_OUTOFMEMORY;
    }
    if (SUCCEEDED(*hr)) {
        const DPSESSIONDESC2* d = (const DPSESSIONDESC2*)buf;
        *dwSize = d->dwSize;
        if (size >= sizeof *d && d->dwSize == sizeof *d
            && !wind_guid_null(&d->guidInstance)) {
            memcpy(out, &d->guidInstance, sizeof(GUID));
            ok = 1;
        }
    }
    if (buf != local.b) tagpu_sim_scratch_free(buf);
    return ok;
}

/* GAME THREAD, at 0x4982CA, before the loader thread exists: DirectPlay's own record of the
   session this peer is in, else the engine's copy, else nothing (see THE SESSION above) */
static void __cdecl wind_session_capture(void)
{
    const char* ta = *(const char* const*)(size_t)B6_MAIN;
    IDirectPlay2A* dp;
    s_windSource = WIND_NONE;
    s_windDiag.descSize = 0;
    s_windSessionHr = DPERR_UNINITIALIZED;
    memcpy(&s_windEngineCopy, ta + B6_DESC_GUID, sizeof s_windEngineCopy);
    memcpy(&dp, ta + B6_DPLAY, sizeof dp);
    if (dp && wind_dplay_guid(dp, &s_windSession, &s_windSessionHr, &s_windDiag.descSize))
        s_windSource = WIND_DPLAY;
    else if (!wind_guid_null(&s_windEngineCopy)) {
        memcpy(&s_windSession, &s_windEngineCopy, sizeof(GUID));
        s_windSource = WIND_COPY;
    }
}

/* the engine's one SetSessionDesc (0x4C9903), made with the GUID DirectPlay holds now or not at
   all (see THE SESSION above); the engine's descriptor is read, never written */
static HRESULT __stdcall wind_set_session_desc(IDirectPlay2A* dp, const DPSESSIONDESC2* engine)
{
    DPSESSIONDESC2 mine;
    GUID now;
    HRESULT hr;
    DWORD dwSize;
    if (!wind_dplay_guid(dp, &now, &hr, &dwSize)) {
        InterlockedIncrement(&s_windDiag.setWithheld);
        return FAILED(hr) ? hr : DPERR_GENERIC;
    }
    memcpy(&mine, engine, sizeof mine);
    if (memcmp(&mine.guidInstance, &now, sizeof now) != 0) {
        memcpy(&mine.guidInstance, &now, sizeof now);
        InterlockedIncrement(&s_windDiag.setDiffered);
    }
    InterlockedIncrement(&s_windDiag.setCalls);
    return IDirectPlay2_SetSessionDesc(dp, &mine, 0);          /* stock's flags, 0x4C98FD */
}

/* FNV-1a 64 over the session's GUID, then the map's hash */
static unsigned long long wind_session_seed(const GUID* g, unsigned int map)
{
    const unsigned char* p = (const unsigned char*)g;
    unsigned long long h = 0xCBF29CE484222325ull;
    int i;
    for (i = 0; i < 16; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    for (i = 0; i < 4; i++) h = (h ^ ((map >> (8 * i)) & 0xFFu)) * 0x100000001B3ull;
    return h;
}

static void wind_guid_text(const GUID* g, char* out, size_t room)
{
    snprintf(out, room, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
             g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* FNV-1a over the TNT path's stem, case folded: "Maps\Two Continents.TNT" -> "two continents" */
static unsigned int wind_map_hash(const char* gs, char* stem, unsigned int room)
{
    const char* s;
    const char* q;
    unsigned int h = 0x811C9DC5u, n = 0;
    stem[0] = 0;
    s = gs + 0x204;
    for (q = s; q < s + MAX_PATH && *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') s = q + 1;
    for (q = s; q < s + MAX_PATH && *q && *q != '.'; q++) {
        char c = (*q >= 'A' && *q <= 'Z') ? (char)(*q + 32) : *q;
        h = (h ^ (unsigned char)c) * 0x01000193u;
        if (n + 1 < room) stem[n++] = c;
    }
    stem[n] = 0;
    return h;
}

/* splitmix64's step and finaliser: the wind's generator, the meteors' and the run seeds' mixer */
static unsigned long long seed_mix(unsigned long long* state)
{
    unsigned long long z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

#define METEOR_SALT 0x6D6574656F72730Bull       /* the meteors' stream apart from the wind's */

/* THE METEORS' SEED, from the wind's (wind_seed), one seed apart by METEOR_SALT. The stream is
   drawn only under the harness's switch (fix_meteor_stream). */
static void meteor_seed(unsigned long long wind)
{
    s_meteorState = wind ^ METEOR_SALT;
    seed_mix(&s_meteorState);
}

static void __cdecl wind_seed(void)
{
    const char* ta = *(const char* const*)(size_t)B6_MAIN;
    const char* gs;
    char stem[48];
    int mode;
    unsigned int map;
    LONG level = InterlockedIncrement(&s_windDiag.levels);
    /* GameingState is not NULL here: the loader body dereferenced it at 0x4971CD, as `this`
       for 0x435100, on the only path into 0x4917D0 (0x497CA1 -> 0x497581) */
    memcpy(&gs, ta + B6_TNT, sizeof gs);
    memcpy(&mode, gs, sizeof mode);
    map = wind_map_hash(gs, stem, sizeof stem);
    if (mode == B6_NETWORK) {
        char copy[40], guid[40];
        wind_guid_text(&s_windEngineCopy, copy, sizeof copy);
        wind_guid_text(&s_windSession, guid, sizeof guid);
        if (s_windSource == WIND_DPLAY) {
            s_windState = wind_session_seed(&s_windSession, map);
            tagpu_logf("enginefix: wind: level %ld seeded from the session %s and the map \"%s\" "
                       "(0x%08X): seed 0x%08X%08X; the engine's copy main+0x479 %s %s; "
                       "SetSessionDesc so far: %ld made, %ld with another GUID in the engine's "
                       "descriptor, %ld withheld", (long)level, guid, stem, map,
                       (unsigned int)(s_windState >> 32), (unsigned int)s_windState,
                       memcmp(&s_windEngineCopy, &s_windSession, sizeof(GUID)) ? "differs:"
                                                                                : "agrees:",
                       copy, (long)s_windDiag.setCalls, (long)s_windDiag.setDiffered,
                       (long)s_windDiag.setWithheld);
        } else if (s_windSource == WIND_COPY) {
            LONG n = InterlockedIncrement(&s_windDiag.fromCopy);
            s_windState = wind_session_seed(&s_windSession, map);
            tagpu_logf("enginefix: wind: level %ld is a network game and DirectPlay's answer was "
                       "refused (0x%08lX, dwSize %lu): seeded from the engine's copy main+0x479 "
                       "%s and the map \"%s\" (0x%08X): seed 0x%08X%08X (%ld so far)",
                       (long)level, (unsigned long)s_windSessionHr,
                       (unsigned long)s_windDiag.descSize, guid, stem, map,
                       (unsigned int)(s_windState >> 32), (unsigned int)s_windState, (long)n);
        } else {
            LONG n = InterlockedIncrement(&s_windDiag.mapOnly);
            s_windState = map;
            tagpu_logf("enginefix: wind: level %ld is a network game with no session: "
                       "DirectPlay's answer refused (0x%08lX, dwSize %lu) and the engine's copy "
                       "null; seeded from the map \"%s\" (0x%08X) alone (%ld so far)",
                       (long)level, (unsigned long)s_windSessionHr,
                       (unsigned long)s_windDiag.descSize, stem, map, (long)n);
        }
    } else if (s_testSeeds) {
        s_windState = TEST_SEED_WIND ^ ((unsigned long long)map << 32);
        tagpu_logf("enginefix: wind: level %ld seeded from the harness's fixed seed (game mode "
                   "%d, not a network game), map \"%s\"", (long)level, mode, stem);
    } else {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        s_windState = (unsigned long long)c.QuadPart ^ ((unsigned long long)map << 32);
        tagpu_logf("enginefix: wind: level %ld seeded from the performance counter (game mode "
                   "%d, not a network game), map \"%s\"", (long)level, mode, stem);
    }
    meteor_seed(s_windState);
    wind_next();                                /* one step, so a seed of 0 is no special case */
}

static void __cdecl wind_draw(void)
{
    char* ta = *(char* const*)(size_t)B6_MAIN;
    unsigned int next, dir, lo, hi, speed;
    int range;
    memcpy(&next, ta + 0x37EC4, 4);
    next += 30u * (5u + wind_next() % 10u);
    memcpy(ta + 0x37EC4, &next, 4);
    memcpy(&lo, ta + 0x1425B, 4);
    memcpy(&hi, ta + 0x1425F, 4);
    range = (int)(hi - lo);                        /* stock's 32-bit sub at 0x490CA1 */
    speed = range < 2 ? lo : lo + wind_next() % (unsigned int)range;
    memcpy(ta + 0x37EDA, &speed, 4);
    if (speed != 0) {
        unsigned short h;
        dir = wind_next() % 0x10000u;
        h = (unsigned short)dir;
        memcpy(ta + 0x37ED8, &h, 2);
    }
}

static int fix_wind(void)
{
    static const unsigned char test[26] = {
        0xA1, 0xE8, 0x1D, 0x51, 0x00, 0x8B, 0x88, 0xC4, 0x7E, 0x03, 0x00, 0x8B, 0x90,
        0x47, 0x8A, 0x03, 0x00, 0x3B, 0xCA, 0x0F, 0x83, 0x2E, 0x01, 0x00, 0x00, 0x56,
    };
    static const unsigned char join[13] = {
        0x8B, 0x90, 0xDA, 0x7E, 0x03, 0x00, 0x66, 0x8B, 0x80, 0xD8, 0x7E, 0x03, 0x00,
    };
    static const unsigned char draws[6] = { 0x8D, 0xB0, 0xC4, 0x7E, 0x03, 0x00 };
    static const unsigned char load[5]  = { 0xE8, 0x38, 0xF3, 0xFF, 0xFF };  /* call 0x490C40 */
    static const unsigned char start[10] = {       /* push 0x497C70; call 0x4B6B20 */
        0x68, 0x70, 0x7C, 0x49, 0x00, 0xE8, 0x51, 0xE8, 0x01, 0x00,
    };
    static const unsigned char desc[36] = {         /* eax = &desc ... esi = dp; dwSize = 0x50 */
        0x8D, 0x86, 0x5D, 0x04, 0x00, 0x00, 0x89, 0x8E, 0xA1, 0x04, 0x00, 0x00,
        0x89, 0x96, 0x9D, 0x04, 0x00, 0x00, 0x89, 0xBE, 0x8D, 0x04, 0x00, 0x00,
        0x8B, 0xB6, 0xC5, 0x04, 0x00, 0x00, 0xC7, 0x00, 0x50, 0x00, 0x00, 0x00,
    };
    static const unsigned char setdesc[9] = {       /* push 0; mov ecx,[esi]; push eax; push esi;
                                                       call [ecx+0x7C] */
        0x6A, 0x00, 0x8B, 0x0E, 0x50, 0x56, 0xFF, 0x51, 0x7C,
    };
    static const unsigned char after[4] = { 0x85, 0xC0, 0x75, 0x0A };   /* test eax,eax; jne */
    unsigned char *a, *b, *c, *d, *p;
    if (!(a = fix_code(24)) || !(b = fix_code(16)) || !(c = fix_code(24)) ||
        !(d = fix_code(16))) {
        lim_no_stub();
        return FIX_TABLE;
    }
    p = a;
    *p++ = 0x60;                                                /* pushad               */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)wind_draw); p += 4;
    *p++ = 0x61;                                                /* popad                */
    *p++ = 0xA1;                                                /* mov eax,[0x511DE8]   */
    *p++ = 0xE8; *p++ = 0x1D; *p++ = 0x51; *p++ = 0x00;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00490CE8);
    p = b;
    *p++ = 0x60;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)wind_seed); p += 4;
    *p++ = 0x61;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00490C40);
    p = c;
    *p++ = 0x60;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)wind_session_capture); p += 4;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)test_seed_game_thread); p += 4;
    *p++ = 0x61;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x004B6B20);
    p = d;
    *p++ = 0x50;                                                /* push eax: the descriptor */
    *p++ = 0x56;                                                /* push esi: the interface  */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)wind_set_session_desc); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x004C9906);
    lim_same(0x004C98D9, sizeof desc, desc, "wind: SetSessionDesc's descriptor and interface");
    lim_branch(0x004C98FD, sizeof setdesc, setdesc, 0xE9, (unsigned int)(size_t)d,
               "wind: SetSessionDesc, with the GUID DirectPlay holds");
    lim_same(0x004C9906, sizeof after, after, "wind: SetSessionDesc's result test");
    lim_same(0x004982C5, 5, start, "wind: the loader thread's entry 0x497C70");
    lim_branch(0x004982CA, 5, start + 5, 0xE8, (unsigned int)(size_t)c,
               "wind: the loader thread's start, capturing the session first");
    lim_same(0x00490C40, sizeof test, test, "wind: the schedule's test");
    lim_branch(0x00490C5A, sizeof draws, draws, 0xE9, (unsigned int)(size_t)a,
               "wind: the draws, from our generator");
    lim_same(0x00490CE8, sizeof join, join, "wind: the components, where the draws rejoin");
    lim_branch(0x00491903, sizeof load, load, 0xE8, (unsigned int)(size_t)b,
               "wind: the level load's call, seeding first");
    return FIX_TABLE;
}

SIM_FEATURE(meteor_stream, SIM_PUPPET_OWNER, "meteor");

/* THE METEOR SHOWER ON THE DRAWN FRAMES' STREAM, A HARNESS'S FIX [DISASSEMBLED]. The shower tick
   0x437DE0 (the sim step's call at 0x495594, every tick, on every peer, no host test) draws CRT
   rand 0x4E4870 on the game thread: four draws at every shower it schedules, switch on or off --
   the shower's two points (0x437E37, 0x437E57, 0x437E88, 0x437EAA) -- and two at every stone
   (0x437F98, 0x437FB5); the forced shower 0x438070 (the `+meteor` handler 0x417760's call at
   0x41778A) draws the points' four (0x4380AD, 0x4380CE, 0x438100, 0x438124). The game thread's CRT
   stream is also the draw path's, drawn every drawn frame (engine/sim.md, *The two random
   streams*), so where a shower falls follows how many frames the process has drawn: a drawn and an
   undrawn twin of one game parted at meteorp1/meteorp2 (MEASURED 2026-11-13, nodraweq on
   nodraw-shadow).
   THE FIX, under the harness's switch alone (A HARNESS'S RANDOM STREAMS; owner's decision,
   2026-11-14: a player's game keeps stock's shower, these bytes untouched). Each of the ten calls
   goes to meteor_rand, a cdecl function returning 15 bits, rand's range 0..0x7FFF, from the
   meteors' own splitmix64 stream s_meteorState, which only these calls draw. Every caller writes
   ecx and edx before reading them after the call (0x437E3C, 0x437E5C, the cdq at 0x437E8D,
   0x437EAF and 0x437FBA, the imul at 0x437F9D; 0x4380B2, 0x4380D3 and the cdq at 0x438105 and
   0x438129), as after the CRT's. The stream is seeded at the level load with the wind
   (meteor_seed) and at a run's clearing apply (tagpu_seed_run), restored with every declared table
   by a seek and a takeover, and on a puppet nothing draws it: the tick is refused there
   (tagpu_puppet.c, gate_meteor) and the stones come from the recording. Inside a network game every
   peer seeds it alike and rains its own shower, so a harness's two-peer game stacks the peers'
   showers on the same points, where stock's peers rain apart.
   CLASS: simulation, a harness's: the ten sites join the fail-closed table only under the switch. */
static int __cdecl meteor_rand(void)
{
    return (int)(seed_mix(&s_meteorState) >> 49);
}

/* -1 when the switch is off: nothing written */
static int fix_meteor_stream(void)
{
    static const unsigned int sites[10] = {
        0x00437E37, 0x00437E57, 0x00437E88, 0x00437EAA, 0x00437F98, 0x00437FB5,
        0x004380AD, 0x004380CE, 0x00438100, 0x00438124,
    };
    int i;
    if (!s_testSeeds) return -1;
    for (i = 0; i < 10; i++) {
        unsigned char call[5];
        unsigned int rel = 0x004E4870u - (sites[i] + 5u);    /* call 0x4E4870, the CRT's rand */
        call[0] = 0xE8;
        memcpy(call + 1, &rel, 4);
        lim_branch(sites[i], sizeof call, call, 0xE8, (unsigned int)(size_t)meteor_rand,
                   "meteors: a draw from their own stream");
    }
    return FIX_TABLE;
}

/* A RUN'S SEEDS, under the harness's switch (A HARNESS'S RANDOM STREAMS): the scenario applier
   calls it on the game thread, between two steps, for an apply that clears the level, after the
   clear and before the creates (tagpu_scenario.c, apply_now: the invariant and what rests on it),
   with the wire's seed. It seeds every stream a level starts with
   from that seed alone: the simulation RNG through 0x4B6CA0, the game thread's CRT stream
   through srand, the wind's and the meteors' streams; it puts the shatter's stream back at its
   start, a constant, so calibration check R6 finds it at run 0's value at every run's empty point
   (tools/tasim/engtrace.py) and the seed's states stream_states mirrors keep their steps; and it restarts the two schedules that run
   on GameTime from the apply's: the shower's (0x5122E8 the next shower, 0x512318 active), as the
   level's init 0x437CD0 starts the shower, and the wind's, by drawing the seed's wind at once.
   The loader thread's stream is gone with the loader.
   THE WIND AT THE APPLY. A wind left to be drawn on the next step would not be in force for that
   step: the step's projectile tick (0x495513) runs before its wind updater (0x49558F), so a shot
   made in step T + 1 would fly one tick in the wind the level had before the apply -- the previous
   batch's in a packed lane, the clear's alone -- and a cell could differ between packings. So the
   updater 0x490C40 runs here, synchronously: it takes no argument, keeps esi (push 0x490C59, pop
   0x490D85) and the x87 stack balanced (fild/fidiv/fstp, fld/fcomp), and returns with `ret`
   (DISASSEMBLED), so a cdecl call of no argument is its contract. It draws only when next <
   GameTime (`cmp ecx,edx; jae` at 0x490C51), so next is set to T - 1 first; the draw (fix_wind's
   wind_draw, from s_windState) adds 30 * (5 + r % 10) to it, the updater derives the components
   +0x37ECC/+0x37ED4, the ratio +0x37EDE and the changed flag +0x37EE2 from that draw alone, and
   next is then raised by 1, so the schedule after it is the one a draw at step T + 1 from next = T
   would make. From the apply on, every step sees the seed's wind. Without fix_wind's jump the
   updater draws stock's way, from the simulation RNG and the CRT just seeded: still the seed's
   alone, and R6 (tools/tasim/engtrace.py) names the wind's state. An apply at GameTime 0 cannot
   draw (no next is below 0) and leaves the draw to step 1, as the level load does: a level at
   GameTime 0 has drawn no wind yet, so none from an earlier run can be in force.
   ITS LOG LINE names the call's place, "before the creates", and the calibration lane requires
   those words (tools/tasim/engrun.py, seed_report): a DLL that seeds after the creates logs
   otherwise and cannot play a lane whose references claim their applies' streams. The line's
   words and that place change together. tools/tasim/engtrace.py (stream_states) mirrors the
   seed_mix steps below and both constants, and holds every apply's recorded states to them. */
void tagpu_seed_run(unsigned int seed)
{
    char* ta = *(char* const*)(size_t)B6_MAIN;
    unsigned long long st = ((unsigned long long)seed << 32) | seed;
    unsigned int now, period, next, wind, off = 0;
    if (!s_testSeeds || !ta) return;
    E_RngSeed((unsigned int)seed_mix(&st));
    E_Srand((unsigned int)seed_mix(&st));
    s_windState = seed_mix(&st) ^ TEST_SEED_WIND;
    wind_next();
    s_meteorState = seed_mix(&st) ^ METEOR_SALT;
    seed_mix(&s_meteorState);
    s_shatterRng = SHATTER_RNG_START;
    memcpy(&now, ta + 0x38A47, 4);
    if (now) {
        unsigned int before = now - 1;
        memcpy(ta + 0x37EC4, &before, 4);
        E_WindTick();
        memcpy(&wind, ta + 0x37EC4, 4);
        wind += 1;
        memcpy(ta + 0x37EC4, &wind, 4);
    } else {
        memcpy(ta + 0x37EC4, &now, 4);
        wind = now;
    }
    memcpy(&period, (const void*)0x00512314u, 4);
    next = now + period;
    memcpy((void*)0x005122E8u, &next, 4);
    memcpy((void*)0x00512318u, &off, 4);
    tagpu_logf("enginefix: run seeds before the creates: the wire's seed %u at GameTime %u seeds "
               "the simulation RNG, the game thread's CRT stream, the wind and the meteors, the shatter's "
               "stream at its start; the wind "
               "is drawn %s, its next change at %u, and the next shower is at %u", seed, now,
               now ? "now" : "on the next step", wind, next);
}

SIM_FEATURE(particle_rand, SIM_PUPPET_INDEPENDENT, "battle meteor teleport particle-check particle-check-sea");

/* THE PARTICLES' OWN RANDOM RULE [DISASSEMBLED]. The particle code draws the C runtime's rand
   0x4E4870 at twenty sites, and that rand keeps one stream per thread (*A harness's random
   streams*): on the game thread the meteor shower draws after the particles, on the loader thread
   the skirmish's random starts (0x4979E7, 0x497A32) draw after a smoking map feature's endless smoke
   spawned at the map load (engine/sim.md, *The two random streams*). So what was on screen moved
   where meteors fell and where commanders started. Each of the twenty `call 0x4E4870` now calls a
   stub that answers from tagpu_prand.h's rule instead, and no CRT stream sees a particle draw:
     - the fire emitter 0x472AB0: x, alt, y jitter and the life (0x472ACF, 0x472AF4, 0x472B17,
       0x472B3A), drawn before its pool test;
     - the feature tick 0x424050: a burning feature's smoke point (0x424345, 0x424379), drawn
       straight before its `call 0x472810` at 0x4243CF;
     - the flare's spawn 0x4737C0 (0x47395F, the start frame; the object in ebx);
     - the nano spawn 0x473D50 (0x473DAD, 0x473DCA, 0x473DE7: the start x, alt, y; 0x473E04,
       0x473E21, 0x473E3E: the end; the object in ebp);
     - the wake's spawn 0x474880 (0x4749D9, 0x474A00, 0x474A27: x, alt, y; the object in ebx);
     - the smoke spawns 0x474DF0 and 0x4751C0 (0x474F1C, 0x4752D4: a puff's frame count; the
       object in ebx, the puff being built on the caller's stack at +0x18: seq, x +0x1C, alt +0x20,
       y +0x24, appended at the vector's end by 0x476210 / 0x476490);
     - the smoke updates 0x475340 and 0x475600 (0x4753B9, 0x475679: a frame's countdown; the object
       in edi, the puff in ebx, its frame +0x14 already advanced).
   No other code calls these functions with another register layout: the spawns are slot 4 of their
   vtables and the updates slot 1, each reached only through the vtable or the class's own init.
   THE RULE. A value is tagpu_prand(seed, particle, draw), a pure function in rand's range:
     - the SEED is the emission's: the level's particle seed, the GameTime and the emission's
       ordinal among the GameTime's emitter calls, counted at the pool's alloc 0x470EB0, which every
       live emitter calls exactly once a call, after its SFX test (0x51E608) and whatever the pool
       answers. An emission's object keeps its seed in a table indexed by its pool slot. A draw made
       before the object exists (the fire emitter's four, the feature's two) takes the seed the
       next alloc will give and the particle TAGPU_PRAND_EMIT;
     - the PARTICLE is the object's own count of the particles it began: the flare's spawn, the
       nano's and the wake's first draw of a particle advance it, so a nano emission's first burst
       is particles 0..4 and its second 5..9, the wake's two dots 0 and 1, the flare's four 0..3;
     - a SMOKE PUFF's particle is its spawn's ordinal k, the object's spawn count when it spawned.
       A smoke record (0x20 bytes) has no free field, so the puff carries k's low bits in the low
       bits of its position: x bits 0..2, y bits 0..2 and alt bits 0..1 (Smoke2, 8 bits) or 0..3
       (Smoke1, 10 bits), to which the updates only ever add multiples of 8 (x += wind << 3, y +=
       wind << 3) and of 4 (Smoke2's alt += rise << 2) or 16 (Smoke1's << 4), and which every copy
       (the compaction's and the reserve's `rep movs` of 8 dwords) keeps. An update's draw takes k
       back as the newest ordinal the object has spawned, last = next - 1, less (last - low bits)
       modulo the carrier's span: k itself for a puff fewer than 256 (Smoke2) or 1024 (Smoke1)
       spawns old. A Smoke2 object spawns at most life / interval + 1 = 61 puffs (the corpse's 900 /
       15), and a Smoke1 puff that old would have lived 5 120 ticks; whatever the data, the recovered ordinal is that function of the
       puff's spawn ordinal and the object's spawn count at the draw, both known at the emission's
       event. A puff's frame count is draw 0 and each frame advance's countdown draw is the frame it
       advances to, 1 or more.
   THE PICTURE IS STOCK'S: every reader of a position takes its high word (the leaves 0x475700,
   0x475470; the frame packet's gather), and a value under 8 added to a multiple of 8 never crosses
   a multiple of 0x10000, so the high word of each later position is the one the unmarked puff
   would have had. Only the draws' values differ from stock, as any other draw order would.
   THE SEED'S TABLE IS BOUNDED: the slot is (object - the pool's one block) / 0x4C, taken only when
   the object lies in the block on a slot's start and the slot is below the pool's capacity and the
   table's rows; anything else draws from seed 0 and is counted, never indexed.
   THE LEVEL'S PARTICLE SEED is made at the layer table's allocation (0x4918B1, `call 0x471D90`),
   on the loader thread before the map load spawns any feature (0x4918C0), from what the wind's is
   made of: in a network game the session's GUID and the map, so every peer holds the same; under
   the harness's switch the fixed seed and the map; otherwise the performance counter. A viewer sees
   the emissions its own sight allows (COB's EMIT_SFX tests the viewed player's sight, 0x480EC5),
   so the emissions of the simulation step are a function of the game and the viewer, on GDI and
   on Vulkan alike. The flying pieces' smoke and fire are not: the explosion DRAW emits them
   (0x421583, 0x4215AA), once a frame that ran ticks (fix_debris_emission), so whether they exist,
   and the ordinal n of every emission after them in their GameTime, also follow the frames drawn.
   THREADS AND ORDER: the loader thread draws only during the level load (the map's endless smoke),
   the game thread only in play; the loader's writes come before its last store, bit 1 of
   main+0x38D75 (0x497C62), which the game thread waits for before its first tick (0x498342).
   ON A PUPPET the rule runs as on a live host; it changes nothing a recording carries: the meteor
   shower is refused on a puppet and its stones recorded, the starts recorded, and nothing of the
   simulation reads a particle.
   THE STATE is particle state, which the simulation never reads (replay-globals.tsv leaves the
   pool, `particlepool`): one SIM_PRIVATE struct, as the debris latch's.
   CLASS: simulation, fail closed: without it meteors and random starts follow the screen. */
#define PRAND_POOL      0x0051E610u      /* ParticleBase: +8 its block vector, +0x1C capacity */
#define PRAND_OBJ       0x4Cu            /* a pool slot's bytes                               */
#define PRAND_SALT      0x7061727469636C65ull   /* the particles' seed apart from the wind's  */

enum { PRK_EMIT, PRK_FLARE, PRK_NANO, PRK_WAKE, PRK_PUFF, PRK_FRAME };
/* a smoke site's code carries, in place of a draw number, how many alt bits its puff carries */
enum { PRAND_ALT_SMOKE2 = 2, PRAND_ALT_SMOKE1 = 4 };

typedef struct PrandObj { unsigned seed; unsigned next; } PrandObj;   /* next: particles begun */

static struct {
    unsigned level;                     /* the level's particle seed                         */
    unsigned tick;                      /* the GameTime the emission ordinal counts in        */
    unsigned n;                         /* emissions counted in it                            */
    unsigned levels;                    /* level loads seeded                                 */
    unsigned draws;                     /* this level: values answered by the rule            */
    unsigned emissions;                 /*   allocs counted                                   */
    unsigned outside;                   /*   objects outside the pool's block (drawn from 0)  */
    PrandObj obj[TAGPU_LIM_SFXPOOL];    /* by pool slot                                       */
} s_prand;
SIM_VAR(s_prand, SIM_PRIVATE);

static unsigned prand_gametime(void)
{
    const char* ta = *(const char* const*)(size_t)B6_MAIN;
    unsigned t = 0;
    if (ta) memcpy(&t, ta + 0x38A47, 4);
    return t;
}

/* the seed the next emission takes */
static unsigned prand_pending(void)
{
    unsigned t = prand_gametime();
    if (t != s_prand.tick) {
        s_prand.tick = t;
        s_prand.n = 0;
    }
    return tagpu_prand_seed(s_prand.level, t, s_prand.n);
}

/* the object's row, or NULL when it is not a slot of the pool's block (see THE SEED'S TABLE) */
static PrandObj* prand_row(unsigned obj)
{
    unsigned begin, end, base, cap, off;
    memcpy(&begin, (const void*)(size_t)(PRAND_POOL + 0x08), 4);
    memcpy(&end,   (const void*)(size_t)(PRAND_POOL + 0x0C), 4);
    memcpy(&cap,   (const void*)(size_t)(PRAND_POOL + 0x1C), 4);
    if (!begin || end <= begin) return NULL;
    memcpy(&base, (const void*)(size_t)begin, 4);
    if (obj < base) return NULL;
    off = obj - base;
    if (off % PRAND_OBJ || off / PRAND_OBJ >= cap || off / PRAND_OBJ >= TAGPU_LIM_SFXPOOL)
        return NULL;
    return &s_prand.obj[off / PRAND_OBJ];
}

/* THE PARTICLES AS EVENTS (Vulkan) [DISASSEMBLED]. gpu-particles-plan.md, landings 2 and 3; the
   event and its closed forms are inc/tagpu_pevt.h. On a Vulkan launch the two nano emitters, 0x4720D0 (point to
   box) and 0x472200 (box to point), make no object: their call of the pool's alloc 0x470EB0 (from
   0x472126 and 0x472256) answers NULL without asking the pool, so the emitter skips its init, its
   layer's trim and its append exactly as it does for a full pool (0x47213E..0x472163,
   0x47226E..0x472293), and the alloc records an event instead. The emission is still counted where
   every other one is (prand_alloc), so n and every later seed are what they are when the object is
   made.
   WHAT THE EVENT TAKES, AND WHERE. At the alloc the emitter's frame is the alloc's esp + 8 (its four
   pushes done; `push 0x4C` and the call's return address above them): its arguments at +0x2C and
   +0x30, the layer's word at +0x34, and the point copied into a degenerate box {x, alt, y, x, alt, y}
   at +0x10 (0x4720E1..0x472108; 0x472211..0x472238). 0x4720D0 inits box1 = that box and box2 = its
   box argument (0x472169..0x472179); 0x472200 box1 = its box argument and box2 = that box
   (0x472299..0x4722A9). The layer is the word sign-extended and unbounded (0x47217C); one outside
   0..9 indexes past the layer table in stock, and here it makes no event (counted).
   THE LAYER'S CAP, as the emitter applies it after the init (0x47219F, `cmp eax,cap / jbe`, the cap
   TAGPU_LIM_SFX): a layer holding more than the cap loses its front, the oldest object in it. A
   layer's events are its objects in the same order, and an event stays until the walk that would
   destroy its object (tagpu_pevt_nano_finish), so the count is the object count. It counts events
   only, which is exact while no engine object shares an event's layer: every caller files nano in
   layer 6 and no other emitter does (effects.md §7.1); the wake (below) is counted the same way in
   layer 2.
   THE LIST is presentation state the simulation never reads, on the game thread: emitted here, purged
   at the end of each particle walk (pevt_walked), cleared at a seek (tagpu_effects_clear) and at each
   level's start (prand_level_begin, on the loader thread before the game thread's first tick: THREADS
   AND ORDER above), read by the frame packet's fill (tagpu_pevt_copy). It holds five layers' caps
   (PEVT_CAP); only events in a sixth layer could fill it, and then it drops its oldest events whole
   and says so.
   THE ORACLE, tagpu_particlecheck.on (a lever, read at install): the pool is asked as in stock, the
   object is made and walked, and an event is recorded only for an object the pool gave, so both sides
   hold the same emissions; after each walk every object of an event class is held to its event's
   closed form (pevt_check).
   FIRE AND THE FLARE (gpu-particles-plan.md, landing 3) share their layers with classes still drawn
   from the engine's objects: COB fire layer 7 with the submarines' bubbles, the flying pieces' fire
   layer 9 with every weapon's smoke, the teleport flare layer 5 with a burning feature's smoke. The
   layer's cap trims the oldest object of the layer, whatever its class, so a count of events alone
   would be wrong there. These classes therefore keep an engine object, a PROXY: the pool's alloc
   and the emitter run as in stock, and the class's init (0x4742C0 fire, 0x4736E0 the flare; reached
   only through slot 6, by the three emitters, straight after the alloc) records the event and turns
   the object into a proxy instead of building particles (pevt_fire, pevt_flare). The emitter then
   trims and appends it as it would the real object, the walker destroys it at the walk that would
   destroy the real object (its slot 3 answers D, tagpu_pevt_sprite_finish), and every count the
   engine keeps -- the layer's, its front trim order, the pool's, and in the stock build the pool's
   fullness -- is the engine's own, by construction. A proxy has no sub-vector, so nothing draws or
   moves it; a trim (the emitter's slot 0 call) removes its event (pevt_proxy_destroy). The init's
   arguments are its contract (the emitters push them, 0x4723A2..0x4723B0, 0x472BBD..0x472BCC,
   0x472042..0x47204B); the layer is the byte the emitter stores at +0xC before the init; those
   emitters' bytes from the alloc's return to the init's call are held to stock with the table.
   THE WAKE AND THE BUBBLES (landing 4) are one class, the init 0x474760, and take one path each by
   their layer. The wake's emitter 0x472430 files into layer 2, which nothing else files into
   (EMIT_SFX 2..5, all four callers `push 2`: 0x48103E, 0x48105B, 0x481078, 0x481095), so it makes
   no object, as nano: its alloc (from 0x472455) answers NULL, its frame gives the init's points
   and period, and the layer's cap is applied to its events. The bubbles' emitter 0x472530 files
   into layer 7 (`push 7` at 0x4810F4) beside COB fire's proxies, so it keeps a proxy: the init's
   entry, called from that emitter (the return address 0x4725B1), records the event and makes the
   object a proxy. Both emitters' bytes are held to stock with the table. Each dot's death walk
   is found at emission (THE WAKE'S DEATH, below). */
#define PEVT_NANO_FWD   0x0047212Bu      /* 0x4720D0's alloc call returns here                 */
#define PEVT_NANO_REV   0x0047225Bu      /* 0x472200's                                         */
#define PEVT_WAKE_FWD   0x0047245Au      /* 0x472430's                                         */
#define PEVT_BUBBLES    0x004725B1u      /* 0x472530's call of the init 0x474760 returns here  */
#define PEVT_VT_NANO    0x004FD5B8u
#define PEVT_VT_FIRE    0x004FD5D8u
#define PEVT_VT_FLARE   0x004FD588u
#define PEVT_VT_WAKE    0x004FD5F8u
#define PEVT_VT_BASE    0x004FD5A8u      /* the destructors leave this vtable                  */
#define PEVT_FLARESEQ   0x147F3          /* main+: the flare sequence both sprite classes draw */
#define PEVT_SEA        0x1427F          /* main+: the sea level, a byte                       */
/* The rows a level can hold at once: each layer an event can be in (2 the wake, 5 the flare, 6
   nano, 7 COB fire and the bubbles, 9 the pieces' fire; every caller pushes its layer as a
   constant) holds at most the cap plus one objects, and a proxy's row is made before its emitter
   trims the layer, so one more. */
#define PEVT_CAP        (5u * ((unsigned)TAGPU_LIM_SFX + 1u) + 1u)
#define PEVT_SLACK      16               /* world px past the boxes a particle can reach: one step
                                            past its end, under 8 (tagpu_pevt.h)                 */
#define PEVT_SPRITE     128              /* world px a sprite's frame can reach past its point  */
#define PEVT_DETAIL     8                /* differences the oracle details, a level            */

typedef struct PevtRow {
    TAGPU_PEVT e;
    uint32_t   gone;                     /* D, the walk that destroys its object; 0 = removed  */
    uint32_t   serial;                   /* the level's emission count at the row: increasing  */
    int16_t    x0, x1, z0, z1;           /* where its particles can be: world x, y - alt / 2   */
} PevtRow;

static struct {
    PevtRow  row[PEVT_CAP];
    unsigned head, tail;                 /* the rows in use, oldest first                      */
    unsigned layer[10];                  /* rows not removed, a layer                          */
    unsigned front[10];                  /* no counted row of the layer lies before this one   */
    uint32_t walked;                     /* the GameTime of the last particle walk             */
    uint32_t serial;                     /* rows made this process: a proxy names its row by it */
    unsigned capped;                     /* the oracle: layers a walk found at their cap + 1   */
    int      sea;                        /* the sea level the wake's death walks were found at */
    int      pend;                       /* an emitter's alloc is in progress: its class + 1   */
    int      player;                     /*   the layer                                        */
    int32_t  box1[6], box2[6];           /*   nano: the boxes it passes the init, uncut;       */
                                         /*   wake: a and b at box1[0..2], box2[0..2]          */
    int32_t  period;                     /*   wake: the colour's period                        */
    unsigned emitted[TAGPU_PEVT_NCLASS], refused, trimmed, dropped, kept;
    unsigned shore, resea;               /* wake dots dead over ground; sea level changes seen */
    unsigned cTicks, cMissing, cExtra, cTrimmed, cSaid;              /* the oracle, this level */
    unsigned cObjs[TAGPU_PEVT_NCLASS], cSubs[TAGPU_PEVT_NCLASS];     /*   by class             */
    unsigned cDiff[TAGPU_PEVT_NCLASS];
    unsigned cField[8];
} s_pevt;
/* PRIVATE, not STATE, although the screen shows it: the list stands in for the engine's particle
   pool, which the recorder leaves as presentation (replay-globals.tsv, the `particlepool` row
   0x51E610: `leave`), and a replay makes its events anew by re-firing the emitters as its replayed
   simulation reaches them, as it makes the pool's objects. The class is the owner's to settle. */
SIM_VAR(s_pevt, SIM_PRIVATE);
static unsigned char s_pevtOn, s_pevtCheck;       /* a Vulkan launch; the oracle lever        */
static const char* s_pevtOff;                     /* why a Vulkan launch draws no events      */
SIM_VAR(s_pevtOff, SIM_INSTALL);
SIM_VAR(s_pevtOn, SIM_INSTALL);
SIM_VAR(s_pevtCheck, SIM_INSTALL);

/* the events need the alloc's stub (the table, written all or nothing) and the walk's hook */
static int pevt_live(void) { return s_pevtOn && s_pwOn && s_limState > 0; }

/* len = _ftol(fsqrt((dx*dx + dalt*dalt) + dy*dy)): the spawn's own x87 sequence and its own _ftol
   0x4E43A0 (0x473E75..0x473EB6), on the game thread whose control word the spawn computes under. */
static int32_t pevt_len(int32_t dx, int32_t dalt, int32_t dy)
{
    int32_t len;
    __asm__ __volatile__(
        "fildl %1\n\t"
        "fildl %2\n\t"
        "fildl %3\n\t"
        "fld %%st(2)\n\t"
        "fmul %%st(3), %%st\n\t"
        "fld %%st(2)\n\t"
        "fmul %%st(3), %%st\n\t"
        "faddp %%st, %%st(1)\n\t"
        "fld %%st(1)\n\t"
        "fmul %%st(2), %%st\n\t"
        "faddp %%st, %%st(1)\n\t"
        "fsqrt\n\t"
        "call *%4\n\t"
        "fstp %%st(0)\n\t"
        "fstp %%st(0)\n\t"
        "fstp %%st(0)\n\t"
        : "=a"(len)
        : "m"(dx), "m"(dalt), "m"(dy), "c"(0x004E43A0u)
        : "edx", "memory", "cc", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)",
          "st(7)");
    return len;
}

/* the init's cut of one box (0x473B80..0x473D34): per axis, base = min + 4d/11 and extent =
   7d/11 - 4d/11, d = max - min, the products 32-bit and the divisions toward zero */
static void pevt_cut(const int32_t* in, int32_t* out)
{
    int a;
    for (a = 0; a < 3; a++) {
        uint32_t lo = (uint32_t)in[a], d = (uint32_t)in[3 + a] - lo;
        int32_t n4 = (int32_t)(d * 4u) / 11, n7 = (int32_t)(d * 7u) / 11;
        out[a] = (int32_t)(lo + (uint32_t)n4);
        out[3 + a] = (int32_t)((uint32_t)n7 - (uint32_t)n4);
    }
}

static int16_t pevt_clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* the hi-word span of one axis over both boxes */
static void pevt_span(const TAGPU_PEVT* e, int a, int* lo, int* hi)
{
    int v[4], i;
    v[0] = e->box1[a] >> 16;
    v[1] = (int32_t)((uint32_t)e->box1[a] + (uint32_t)e->box1[3 + a]) >> 16;
    v[2] = e->box2[a] >> 16;
    v[3] = (int32_t)((uint32_t)e->box2[a] + (uint32_t)e->box2[3 + a]) >> 16;
    *lo = *hi = v[0];
    for (i = 1; i < 4; i++) { if (v[i] < *lo) *lo = v[i]; if (v[i] > *hi) *hi = v[i]; }
}

static void pevt_clear(void)
{
    s_pevt.head = s_pevt.tail = 0;
    memset(s_pevt.layer, 0, sizeof s_pevt.layer);
    memset(s_pevt.front, 0, sizeof s_pevt.front);
    s_pevt.pend = 0;
}

/* a row's object is gone: removed from its layer's count, once */
static void pevt_drop(PevtRow* r)
{
    if (!r->gone) return;
    r->gone = 0;
    s_pevt.layer[TAGPU_PEVT_LAYER(r->e.flags)]--;
}

/* the layer's front event goes, as the emitter destroys its front object. Only the events with
   no engine object are counted (nano in layer 6, the wake in layer 2, each its layer's only
   class); a proxy's layer is trimmed by the engine, proxies and all. */
static void pevt_trim(int L)
{
    unsigned i = s_pevt.front[L] > s_pevt.head ? s_pevt.front[L] : s_pevt.head;
    for (; i < s_pevt.tail; i++) {
        PevtRow* r = &s_pevt.row[i];
        if (!r->gone || (int)TAGPU_PEVT_LAYER(r->e.flags) != L || (r->e.flags & TAGPU_PEVT_PROXY))
            continue;
        pevt_drop(r);
        s_pevt.trimmed++;
        s_pevt.front[L] = i + 1u;
        return;
    }
}

/* room for one more row at the tail: the removed rows squeezed out, and when the list is still
   full -- only events in a sixth layer could fill it, which no caller files -- its oldest
   sixteenth dropped whole */
static void pevt_room(void)
{
    unsigned i, n = 0;
    for (i = s_pevt.head; i < s_pevt.tail; i++)
        if (s_pevt.row[i].gone) s_pevt.row[n++] = s_pevt.row[i];
    s_pevt.head = 0; s_pevt.tail = n;
    memset(s_pevt.front, 0, sizeof s_pevt.front);
    if (n < PEVT_CAP) return;
    n = PEVT_CAP / 16u;
    for (i = 0; i < n; i++) pevt_drop(&s_pevt.row[i]);
    memmove(s_pevt.row, s_pevt.row + n, (PEVT_CAP - n) * sizeof(PevtRow));
    s_pevt.tail = PEVT_CAP - n;
    if (!s_pevt.dropped)
        tagpu_logf("particles: the event list is full (%u events): its oldest %u dropped "
                   "whole; this level's drops are counted at its end", PEVT_CAP, n);
    s_pevt.dropped += n;
}

/* a new row at the tail, its reach box set from the hi-word spans x (xl..xh), alt (al..ah) and
   y (yl..yh) widened by `slack` */
static PevtRow* pevt_add(const TAGPU_PEVT* e, uint32_t gone, int xl, int xh, int al, int ah,
                         int yl, int yh, int slack)
{
    PevtRow* r;
    if (s_pevt.tail >= PEVT_CAP) pevt_room();
    r = &s_pevt.row[s_pevt.tail++];
    r->e = *e;
    r->gone = gone;
    r->serial = ++s_pevt.serial;
    r->x0 = pevt_clamp16(xl - slack);
    r->x1 = pevt_clamp16(xh + slack);
    r->z0 = pevt_clamp16(yl - (ah >> 1) - slack);
    r->z1 = pevt_clamp16(yh - (al >> 1) + slack);
    s_pevt.layer[TAGPU_PEVT_LAYER(e->flags)]++;
    s_pevt.emitted[TAGPU_PEVT_CLASS(e->flags) < TAGPU_PEVT_NCLASS ? TAGPU_PEVT_CLASS(e->flags) : 0]++;
    return r;
}

/* the row of a serial, or NULL when it is no longer listed: the rows lie in serial order */
static PevtRow* pevt_find(uint32_t serial)
{
    unsigned lo = s_pevt.head, hi = s_pevt.tail;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        uint32_t m = s_pevt.row[mid].serial;
        if (m == serial) return &s_pevt.row[mid];
        if ((int32_t)(m - serial) < 0) lo = mid + 1u; else hi = mid;
    }
    return NULL;
}

/* THE WAKE'S DEATH [DISASSEMBLED]. The wake's update 0x473170 moves each dot (0x474580) and
   then asks its death 0x474720(T): T past the dot's end tick (`cmp; jg`), or else the ground's
   height under the dot, 0x485070(&pos), at or above the sea level, the byte main+0x1427F
   zero-extended (`cmp eax,ecx; jge`); a dead dot is removed. The height (engine/map.md, *The two
   lookups bound themselves*) reads only the position's two high words, the cell grid
   main+0x14287 (each cell's +4) and its size main+0x14233/+0x14237, and writes nothing: off the
   grid it answers -1, so a dot there never dies by ground. A dot moves on a straight line, so its
   death walk is a function of its start, its step, its end tick, the grid and the sea level, and
   is found once, at emission, by asking 0x485070 itself at every walk the engine would, the
   position the move leaves, until one answers death (pevt_wake_gone): the engine's own answers,
   not a copy of its arithmetic. It runs on the game thread, where the walk runs.
   WHAT IT RESTS ON: the heights and the sea level the walks will read are those read here. The
   heights are written only by the map's load (0x483A19, 0x483AC1): no other store to a cell's +4
   is in any function that addresses the grid (each read of main+0x14287 and each caller of the
   cell lookups 0x481550, 0x4815A0, 0x4815F0 and 0x485330). The sea level is written by the load
   (0x483877) and by the console's SeaLevel command (0x416A90, its store 0x416AA3), which runs
   outside the step; so each walk compares the byte with the one the live dots were marched at, and
   on a change finds every live dot's death walk again from that walk on (pevt_wake_resea), before
   the walk's purge reads them. */
typedef int (__stdcall* PFN_Ground)(const int32_t* pos);

static int pevt_sea_now(void)
{
    return *(const unsigned char*)(*(const char* const*)(size_t)B6_MAIN + PEVT_SEA);
}

/* dot k's death walk: the first walk from `from` on (and from its first move on) whose update
   removes it. At most 6 * period + 2 lookups: the walk past its end tick removes it anyway. */
static uint32_t pevt_wake_gone(const TAGPU_PEVT* e, unsigned k, uint32_t from, int sea)
{
    const uint32_t first = tagpu_pevt_wake_first(e, k);
    const uint32_t endT = tagpu_pevt_wake_born(e, k) + 6u * (uint32_t)e->period;
    uint32_t Q = (int32_t)(from - first) > 0 ? from : first;
    int32_t s[3], pos[3];
    unsigned a;
    tagpu_pevt_wake_start(e, k, s);
    for (;; Q++) {
        const uint32_t u = Q - first + 1u;
        if ((int32_t)(Q - endT) > 0) return Q;
        for (a = 0; a < 3; a++) pos[a] = (int32_t)((uint32_t)s[a] + u * (uint32_t)e->step[a]);
        if (((PFN_Ground)0x00485070u)(pos) >= sea) return Q;
    }
}

/* What the init 0x474760(a, b, period, 1, dir) makes, into `e` (its seed, T and flags set): d =
   b - a; the length len by the init's own x87 sequence (pevt_len, 0x4747CC..0x4747E6); the
   divisor the low dword of len * 0x20000 >> 16 (_allmul, _allshr: 0x4747FB, 0x474805), which is
   len * 2; q the low dword of 2^32 / divisor (_alldiv 0x474811), sign-extended; each axis's step
   the low dword of (d * q) >> 16 (0x474822..0x474868). A zero divisor makes no dot (the divide's
   fix: fix_particle_divides). Then each dot's death walk; dot 1 exists when the object is updated
   at T + 1: always when late, otherwise when the walk of T left dot 0. */
static void pevt_wake_fill(TAGPU_PEVT* e, const int32_t* a, const int32_t* b, int32_t period,
                           int32_t dir)
{
    const int sea = pevt_sea_now();
    int32_t d[3], div, q;
    int k;
    for (k = 0; k < 3; k++) d[k] = (int32_t)((uint32_t)b[k] - (uint32_t)a[k]);
    div = (int32_t)((uint32_t)pevt_len(d[0], d[1], d[2]) * 2u);
    q = div ? (int32_t)(0x100000000LL / (int64_t)div) : 0;
    for (k = 0; k < 3; k++) {
        e->from[k] = a[k];
        e->step[k] = (int32_t)(((int64_t)d[k] * q) >> 16);
    }
    e->period = period;
    e->dir = dir != 0;
    e->nsub = 0;
    if (!div) return;
    e->nsub = 1;
    e->gone[0] = pevt_wake_gone(e, 0, e->t0, sea);
    if ((e->flags & TAGPU_PEVT_LATE) || (int32_t)(e->gone[0] - e->t0) > 0) {
        e->nsub = 2;
        e->gone[1] = pevt_wake_gone(e, 1, e->t0, sea);
    }
    for (k = 0; k < e->nsub; k++)
        if ((int32_t)(e->gone[k] - (tagpu_pevt_wake_born(e, (unsigned)k) +
                                   6u * (uint32_t)period)) <= 0)
            s_pevt.shore++;
}

/* the hi-word span of each axis over a particle's path from `from`, `umax` steps on */
static void pevt_path(const TAGPU_PEVT* e, int32_t umax, int* lo, int* hi)
{
    int a;
    for (a = 0; a < 3; a++) {
        int32_t p0 = e->from[a] >> 16;
        int32_t p1 = (int32_t)((uint32_t)e->from[a] + (uint32_t)umax * (uint32_t)e->step[a]) >> 16;
        lo[a] = p0 < p1 ? p0 : p1;
        hi[a] = p0 < p1 ? p1 : p0;
    }
}

/* the sea level changed before the walk of T: every live dot's death walk from T on found again,
   and its event's D with it (a wake proxy asks its row for D: pevt_proxy_wake_empty) */
static void pevt_wake_resea(uint32_t T, int sea)
{
    unsigned i, k, live = 0, moved = 0;
    for (i = s_pevt.head; i < s_pevt.tail; i++) {
        PevtRow* r = &s_pevt.row[i];
        if (!r->gone || TAGPU_PEVT_CLASS(r->e.flags) != TAGPU_PEVT_C_WAKE) continue;
        for (k = 0; k < (unsigned)r->e.nsub && k < 2u; k++) {
            uint32_t g;
            if ((int32_t)(r->e.gone[k] - T) < 0) continue;
            g = pevt_wake_gone(&r->e, k, T, sea);
            live++;
            moved += g != r->e.gone[k];
            r->e.gone[k] = g;
        }
        r->gone = tagpu_pevt_wake_finish(&r->e);
    }
    s_pevt.resea++;
    tagpu_logf("particles: the sea level is %d from the walk of %u (was %d): %u live wake dots' "
               "death walks found again, %u of them moved", sea, T, s_pevt.sea, live, moved);
}

/* the wake call pevt_pre took (no object): the init's event, its D and the layer's cap */
static void pevt_emit_wake(unsigned seed)
{
    TAGPU_PEVT e;
    int L = s_pevt.player, lo[3], hi[3];
    uint32_t gone;
    if ((unsigned)L >= 10u || s_pevt.period < 1 || s_pevt.period > TAGPU_PEVT_WAKE_PERIOD) {
        s_pevt.refused++;
        return;
    }
    memset(&e, 0, sizeof e);
    e.seed = seed;
    e.t0 = s_prand.tick;
    e.flags = (unsigned)L | (s_pevt.walked == e.t0 ? TAGPU_PEVT_LATE : 0u) |
              (unsigned)TAGPU_PEVT_C_WAKE << 8;
    pevt_wake_fill(&e, s_pevt.box1, s_pevt.box2, s_pevt.period, 1);
    gone = tagpu_pevt_wake_finish(&e);
    if (s_pevt.layer[L] > (unsigned)TAGPU_LIM_SFX) pevt_trim(L);
    pevt_path(&e, 6 * e.period + 2, lo, hi);
    pevt_add(&e, gone, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], PEVT_SLACK);
}

/* the call pevt_pre took (`cls` its class), as the init and the spawn would make it */
static void pevt_emit(unsigned seed, int cls)
{
    TAGPU_PEVT e;
    int L = s_pevt.player, k, xl, xh, al, ah, yl, yh;
    uint32_t gone;
    if (cls == TAGPU_PEVT_C_WAKE) { pevt_emit_wake(seed); return; }
    if ((unsigned)L >= 10u) { s_pevt.refused++; return; }
    memset(&e, 0, sizeof e);
    e.seed = seed;
    e.t0 = s_prand.tick;
    e.flags = (unsigned)L | (s_pevt.walked == e.t0 ? TAGPU_PEVT_LATE : 0u);
    pevt_cut(s_pevt.box1, e.box1);
    pevt_cut(s_pevt.box2, e.box2);
    for (k = 0; k < (int)TAGPU_PEVT_NPART; k++) {
        int32_t s[3], t[3], len;
        int64_t q;
        tagpu_pevt_ends(&e, (unsigned)k, s, t);
        len = pevt_len((int32_t)((uint32_t)t[0] - (uint32_t)s[0]),
                       (int32_t)((uint32_t)t[1] - (uint32_t)s[1]),
                       (int32_t)((uint32_t)t[2] - (uint32_t)s[2]));
        q = ((int64_t)len * 65536) / 0x40000;       /* _allshl 16, _alldiv 0x40000 */
        e.steps[k] = (int16_t)(uint16_t)((uint32_t)q >> 16);
    }
    gone = tagpu_pevt_nano_finish(&e);
    if (s_pevt.layer[L] > (unsigned)TAGPU_LIM_SFX) pevt_trim(L);
    pevt_span(&e, 0, &xl, &xh);
    pevt_span(&e, 1, &al, &ah);
    pevt_span(&e, 2, &yl, &yh);
    pevt_add(&e, gone, xl, xh, al, ah, yl, yh, PEVT_SLACK);
}

/* Before the pool's alloc: 1 when the pool is not to be asked (a nano or wake emitter's call
   drawn from events). `regs` is the stub's pushad block, the alloc's return address and argument
   above it; the emitter's esp after its four pushes is past them both. The wake's (0x472430):
   a and b, pointers, at +0x14 and +0x18, the period at +0x1C, the layer's word at +0x20, which it
   passes the init and reads for the cap (0x472498..0x4724B2). */
static int __cdecl pevt_pre(const unsigned* regs)
{
    const unsigned char* frame;
    const int32_t* arg;
    unsigned site = regs[PR_RET];
    s_pevt.pend = 0;
    if ((site != PEVT_NANO_FWD && site != PEVT_NANO_REV && site != PEVT_WAKE_FWD) || !pevt_live())
        return 0;
    frame = (const unsigned char*)(regs + PR_RET + 2);
    if (site == PEVT_WAKE_FWD) {
        s_pevt.player = (int)*(const int16_t*)(frame + 0x20);
        memcpy(&arg, frame + 0x14, sizeof arg);
        memcpy(s_pevt.box1, arg, 3 * sizeof *arg);
        memcpy(&arg, frame + 0x18, sizeof arg);
        memcpy(s_pevt.box2, arg, 3 * sizeof *arg);
        memcpy(&s_pevt.period, frame + 0x1C, sizeof s_pevt.period);
        s_pevt.pend = 1 + TAGPU_PEVT_C_WAKE;
        return !s_pevtCheck;
    }
    s_pevt.player = (int)*(const int16_t*)(frame + 0x34);
    memcpy(&arg, frame + (site == PEVT_NANO_FWD ? 0x30 : 0x2C), sizeof arg);
    if (site == PEVT_NANO_FWD) {
        memcpy(s_pevt.box1, frame + 0x10, sizeof s_pevt.box1);
        memcpy(s_pevt.box2, arg, sizeof s_pevt.box2);
    } else {
        memcpy(s_pevt.box1, arg, sizeof s_pevt.box1);
        memcpy(s_pevt.box2, frame + 0x10, sizeof s_pevt.box2);
    }
    s_pevt.pend = 1;
    return !s_pevtCheck;
}

/* THE PROXY, the engine object a sprite event keeps in its layer (THE PARTICLES AS EVENTS above):
   +4 D, the walk that destroys it, +8 its row's serial; +0xC the layer byte and +0x10..+0x18 the
   empty sub-vector, as the emitter left them. Its seven slots, thiscall as the engine calls them:
   destroy(flags) ret 4, update, draw(ctx) ret 4, empty?, spawn, again?, init. A layer object is
   called by the walker (slot 3, then slot 1 while not empty, slot 0 once empty: at D for a proxy),
   the emitters' trims, the level's teardown 0x471DE0 and tagpu_effects_clear (slot 0), and the
   draw walker 0x471F90 (slot 2, not run on a Vulkan launch). So only slots 0, 1 and 3 are the
   proxy's own; the rest are the class's own entries, copied from its vtable at install: its draw
   (0x4730F0, 0x472E30) walks the sub-vector +0x10..+0x14, empty here, and draws nothing; spawn,
   again and init are called only by the class's update, which the proxy's replaces, and by the
   emitter before the init makes the object a proxy. */
static void* __thiscall pevt_proxy_destroy(void* self, int flags)
{
    typedef void (__thiscall* PFN_PoolPush)(void* pool, void* object);
    unsigned char* o = (unsigned char*)self;
    uint32_t serial, D;
    PevtRow* r;
    memcpy(&D, o + 4, 4);
    memcpy(&serial, o + 8, 4);
    r = pevt_find(serial);
    if (r && r->gone) pevt_drop(r);
    /* what the classes' deleting destructors 0x4716A0 / 0x471430 do to an object whose
       sub-vector is empty: the vector words zeroed, the base vtable, and with flag 1 the
       object back to the pool by 0x470ED0 */
    memset(o + 0x10, 0, 12);
    *(uint32_t*)(void*)o = PEVT_VT_BASE;
    if (flags & 1) ((PFN_PoolPush)0x00470ED0u)((void*)(size_t)PRAND_POOL, self);
    return self;
}
static void __thiscall pevt_proxy_update(void* self) { (void)self; }
static int __thiscall pevt_proxy_empty(void* self)
{
    uint32_t D;
    memcpy(&D, (const unsigned char*)self + 4, 4);
    return (int32_t)(prand_gametime() - D) >= 0;
}
/* A wake proxy's D is its row's: a change of the sea level moves its dots' death walks
   (pevt_wake_resea). Its +4 stands when the row is gone. */
static int __thiscall pevt_proxy_wake_empty(void* self)
{
    uint32_t D, serial;
    const PevtRow* r;
    memcpy(&D, (const unsigned char*)self + 4, 4);
    memcpy(&serial, (const unsigned char*)self + 8, 4);
    if ((r = pevt_find(serial)) != NULL && r->gone) D = r->gone;
    return (int32_t)(prand_gametime() - D) >= 0;
}
static uint32_t s_proxyFire[7];
SIM_VAR(s_proxyFire, SIM_INSTALL);
static uint32_t s_proxyFlare[7];
SIM_VAR(s_proxyFlare, SIM_INSTALL);
static uint32_t s_proxyWake[7];
SIM_VAR(s_proxyWake, SIM_INSTALL);
/* the class's vtable with the proxy's destroy, update and empty in slots 0, 1, 3. The wake's
   destroy 0x4717E0 does what the other two do (MEM_Free +0x10, the vector zeroed, the base
   vtable, the pool push on flag 1: 0x4717E4..0x471813). */
static void pevt_proxy_vtable(uint32_t* vt, uint32_t classVt)
{
    memcpy(vt, (const void*)(size_t)classVt, 7u * sizeof *vt);
    vt[0] = (uint32_t)(size_t)pevt_proxy_destroy;
    vt[1] = (uint32_t)(size_t)pevt_proxy_update;
    vt[3] = (uint32_t)(size_t)(classVt == PEVT_VT_WAKE ? pevt_proxy_wake_empty : pevt_proxy_empty);
}

/* the flare sequence's frame count - 1, the modulus both classes' spawns store (0x4B7F60 - 1:
   the u16 at the sequence's start, which the spawn this replaces reads unconditionally);
   0 when there is no sequence */
static int32_t pevt_nseq1(void)
{
    const char* ta = *(const char* const*)(size_t)B6_MAIN;
    const unsigned char* seq;
    if (!ta) return 0;
    memcpy(&seq, ta + PEVT_FLARESEQ, sizeof seq);
    return seq ? (int32_t)*(const uint16_t*)seq - 1 : 0;
}

/* An event of a proxy class, `e` filled, listed with its D and its reach (`umax` the most moves
   a particle has, `slack` past its path), and the object made the proxy `vt` (or, under the
   oracle, left to its real init) */
static int pevt_hold(unsigned char* obj, TAGPU_PEVT* e, uint32_t D, int32_t umax, int slack,
                     const uint32_t* vt)
{
    int lo[3], hi[3];
    PevtRow* r;
    e->flags |= TAGPU_PEVT_PROXY;
    pevt_path(e, umax, lo, hi);
    r = pevt_add(e, D, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], slack);
    if (s_pevtCheck) return 0;
    *(uint32_t*)(void*)obj = (uint32_t)(size_t)vt;
    memcpy(obj + 4, &D, 4);
    memcpy(obj + 8, &r->serial, 4);
    return 1;
}

static int pevt_sprite(unsigned char* obj, TAGPU_PEVT* e, int32_t umax)
{
    return pevt_hold(obj, e, tagpu_pevt_sprite_finish(e), umax, PEVT_SLACK + PEVT_SPRITE,
                     TAGPU_PEVT_CLASS(e->flags) == TAGPU_PEVT_C_FIRE ? s_proxyFire : s_proxyFlare);
}

/* the event's seed, T, layer and LATE, from the object the pool just gave (prand_alloc set its
   seed) and the layer byte its emitter stored at +0xC; 0 when the object keeps its real init */
static int pevt_head(const unsigned char* obj, unsigned cls, TAGPU_PEVT* e)
{
    const PrandObj* pr = prand_row((unsigned)(size_t)obj);
    unsigned L = obj[0x0C];
    memset(e, 0, sizeof *e);
    if (!pr || L >= 10u) return 0;
    e->seed = pr->seed;
    e->t0 = prand_gametime();
    e->flags = L | (s_pevt.walked == e->t0 ? TAGPU_PEVT_LATE : 0u) | cls << 8;
    return 1;
}

/* the fire init 0x4742C0(from, to, period, life), thiscall `ret 0x10`, at its entry: 1 when the
   object became a proxy (the stub returns past the init), 0 to run the init. What it would make:
   +4 end = T + life (0x4742CB), +0x1C period, step = (d * q) >> 16 per axis with q the low dword,
   sign-extended, of 2^32 / (life << 16) (_alldiv 0x4E4440, _allmul 0x4E4400, _allshr 0x4E43D0:
   0x47432C..0x47438B), then the spawn of particle 0 at `from`, frame 0 and counter 0; a particle
   a tick while the next spawn tick T + k is at most the end (0x4730C0: `+8 <= +4` signed, `+8 <=
   T`), so life + 1 of them. An emission the event cannot hold -- life or period under 1, which
   stock divides by zero on, more than ten particles, no sequence -- keeps its real init. */
static int __cdecl pevt_fire(unsigned* regs)
{
    unsigned char* obj = (unsigned char*)(size_t)regs[PR_ECX];
    const int32_t* from = (const int32_t*)(size_t)regs[PR_RET + 1];
    const int32_t* to = (const int32_t*)(size_t)regs[PR_RET + 2];
    const int32_t period = (int32_t)regs[PR_RET + 3], life = (int32_t)regs[PR_RET + 4];
    TAGPU_PEVT e;
    int32_t q, k;
    int a;
    if (!pevt_live() || !pevt_head(obj, TAGPU_PEVT_C_FIRE, &e)) return 0;
    if (life < 1 || period < 1 || (e.nseq1 = pevt_nseq1()) < 1) { s_pevt.kept++; return 0; }
    for (k = 1; k <= TAGPU_PEVT_NPART && (int32_t)(e.t0 + (uint32_t)k) <= (int32_t)(e.t0 + (uint32_t)life); k++)
        ;
    if (k > TAGPU_PEVT_NPART) { s_pevt.kept++; return 0; }
    e.nsub = k;
    e.period = period;
    e.life = life;
    q = (int32_t)(0x100000000LL / (int64_t)(int32_t)((uint32_t)life << 16));
    for (a = 0; a < 3; a++) {
        int32_t d = (int32_t)((uint32_t)to[a] - (uint32_t)from[a]);
        e.from[a] = from[a];
        e.step[a] = (int32_t)(((int64_t)d * q) >> 16);
    }
    return pevt_sprite(obj, &e, life + 1);
}

/* the flare's init 0x4736E0(from, to, life), thiscall `ret 0xC`, likewise. What it would make:
   +4 end = T + life, steps = the int16 high word of (len << 16) / 0x50000 with len the init's own
   x87 length of d = to - from (0x473744..0x473788; pevt_len is that sequence), step = d / steps
   per axis (idiv, 0x47378E..0x4737A2), and the spawn of particle 0 at `from`; then one particle at
   each update of T + 10k while T + 10k is at most the end (0x472E00), the object alive only while
   its last particle is: the walk of T + 10k finds it empty unless that particle, born at T + 10(k-1)
   and removed by the update past its end, lives to T + 10k - 1, i.e. steps >= 9. Steps 0 makes no
   particle (the divide's fix ends the init there). */
static int __cdecl pevt_flare(unsigned* regs)
{
    unsigned char* obj = (unsigned char*)(size_t)regs[PR_ECX];
    const int32_t* from = (const int32_t*)(size_t)regs[PR_RET + 1];
    const int32_t* to = (const int32_t*)(size_t)regs[PR_RET + 2];
    const int32_t life = (int32_t)regs[PR_RET + 3];
    TAGPU_PEVT e;
    int32_t d[3], len, steps, k;
    int64_t q;
    int a;
    if (!pevt_live() || !pevt_head(obj, TAGPU_PEVT_C_FLARE, &e)) return 0;
    if ((e.nseq1 = pevt_nseq1()) < 1) { s_pevt.kept++; return 0; }
    for (a = 0; a < 3; a++) d[a] = (int32_t)((uint32_t)to[a] - (uint32_t)from[a]);
    len = pevt_len(d[0], d[1], d[2]);
    q = ((int64_t)len * 65536) / 0x50000;                    /* _allshl 16, _alldiv 0x50000 */
    steps = (int16_t)(uint16_t)((uint32_t)q >> 16);
    if (steps) {
        for (k = 1; k <= TAGPU_PEVT_NPART && steps >= 9 &&
                    (int32_t)(e.t0 + 10u * (uint32_t)k) <= (int32_t)(e.t0 + (uint32_t)life); k++)
            ;
        if (k > TAGPU_PEVT_NPART) { s_pevt.kept++; return 0; }
    } else {
        k = 0;
    }
    e.nsub = k;
    e.life = steps;
    for (a = 0; a < 3; a++) {
        e.from[a] = from[a];
        e.step[a] = steps ? d[a] / steps : 0;
    }
    return pevt_sprite(obj, &e, steps + 1);
}

/* the wake's init 0x474760(a, b, period, life, dir), thiscall `ret 0x14`, at its entry: called by
   the bubbles' emitter (its return address PEVT_BUBBLES), 1 when the object became a proxy;
   every other call runs the init (the wake's own emitter makes no object, but under the oracle).
   What it would make: +4 = T + life (0x47476B), the dot of T by its spawn (0x474872) and, while
   +8 = T + 1 is at most +4, the dot of T + 1 (again 0x473220): with the life of 1 both emitters
   pass, the two dots pevt_wake_fill finds. An emission the event cannot hold -- another life, a
   period under 1 (the move divides by it, 0x4745A7) or past TAGPU_PEVT_WAKE_PERIOD -- keeps its
   real init. */
static int __cdecl pevt_wake(unsigned* regs)
{
    unsigned char* obj = (unsigned char*)(size_t)regs[PR_ECX];
    const int32_t* a = (const int32_t*)(size_t)regs[PR_RET + 1];
    const int32_t* b = (const int32_t*)(size_t)regs[PR_RET + 2];
    const int32_t period = (int32_t)regs[PR_RET + 3], life = (int32_t)regs[PR_RET + 4];
    const int32_t dir = (int32_t)regs[PR_RET + 5];
    TAGPU_PEVT e;
    if (regs[PR_RET] != PEVT_BUBBLES || !pevt_live() || !pevt_head(obj, TAGPU_PEVT_C_WAKE, &e))
        return 0;
    if (life != 1 || period < 1 || period > TAGPU_PEVT_WAKE_PERIOD) { s_pevt.kept++; return 0; }
    pevt_wake_fill(&e, a, b, period, dir);
    return pevt_hold(obj, &e, tagpu_pevt_wake_finish(&e), 6 * period + 2, PEVT_SLACK, s_proxyWake);
}

/* the end of each particle walk: the events whose object this walk destroyed go, and the oracle
   runs */
static void pevt_check(uint32_t P);
static void pevt_walked(void)
{
    uint32_t T = prand_gametime();
    unsigned i;
    s_pevt.walked = T;
    if (pevt_live()) {
        const int sea = pevt_sea_now();
        if (s_pevt.sea >= 0 && sea != s_pevt.sea) pevt_wake_resea(T, sea);
        s_pevt.sea = sea;
    }
    for (i = s_pevt.head; i < s_pevt.tail; i++) {
        PevtRow* r = &s_pevt.row[i];
        if (r->gone && (int32_t)(T - r->gone) >= 0) pevt_drop(r);
    }
    while (s_pevt.head < s_pevt.tail && !s_pevt.row[s_pevt.head].gone) s_pevt.head++;
    if (s_pevt.head == s_pevt.tail) pevt_clear();
    if (s_pevtCheck && pevt_live()) pevt_check(T);
}

/* THE ORACLE, after the walk of P: each layer's objects of an event class in order beside the
   layer's events of that class in order, which the emitters and the walk keep the same (above);
   each object paired with its event by its seed (its row of the seed table) and every
   sub-particle's record held to tagpu_pevt_nano_at or tagpu_pevt_sprite_at. Read on the game
   thread, inside the step, from the layers it walked: the objects are live and their sub-vectors
   this thread's own; a vector longer than an object can hold (ten) is a count difference and only
   its first ten records are read.
   A LAYER AT ITS CAP can have lost its front object to any emitter's trim, which no event sees
   (only nano counts its own; fire and the flare share layers with smoke and bubbles). The walk
   marks a layer it finds holding more than the cap (particle_walk: an emission's trim leaves the
   cap plus one until the next walk), and an event of such a layer whose object is gone while a
   later one is there is counted as trimmed, not missing. A layer under its cap all level has every
   event held to an object. */
enum { PF_SEED, PF_COUNT, PF_POS, PF_END, PF_STEP, PF_COLOUR, PF_TICK, PF_FRAME };
static const char* const PF_NAME[8] = { "seed", "count", "pos", "end", "step", "colour", "tick",
                                        "frame" };
static const char* const PC_NAME[TAGPU_PEVT_NCLASS] = { "nano", "fire", "flare", "wake" };
static const unsigned PC_STRIDE[TAGPU_PEVT_NCLASS] = { 0x30u, 0x3Cu, 0x34u, 0x44u };

static int pevt_class_of(uint32_t vt)
{
    return vt == PEVT_VT_NANO ? TAGPU_PEVT_C_NANO : vt == PEVT_VT_FIRE ? TAGPU_PEVT_C_FIRE
         : vt == PEVT_VT_FLARE ? TAGPU_PEVT_C_FLARE : vt == PEVT_VT_WAKE ? TAGPU_PEVT_C_WAKE : -1;
}

/* the first live row of layer L and class c at or after i */
static unsigned pevt_next(unsigned i, int L, int c)
{
    for (; i < s_pevt.tail; i++) {
        const PevtRow* r = &s_pevt.row[i];
        if (r->gone && (int)TAGPU_PEVT_LAYER(r->e.flags) == L &&
            (int)TAGPU_PEVT_CLASS(r->e.flags) == c)
            break;
    }
    return i;
}

/* the walker found layer L holding n objects */
static void pevt_layer_seen(int L, unsigned n)
{
    if (s_pevtCheck && n > (unsigned)TAGPU_LIM_SFX) s_pevt.capped |= 1u << L;
}

static void pevt_said(uint32_t P, int L, int c, unsigned obj, int k, int f, const int32_t* a,
                      const int32_t* b, int n)
{
    if (s_pevt.cSaid >= PEVT_DETAIL) return;
    s_pevt.cSaid++;
    if (n == 3)
        tagpu_logf("pcheck: T=%u L%d %s object %u particle %d %s: engine (%08X %08X %08X) form "
                   "(%08X %08X %08X)", P, L, PC_NAME[c], obj, k, PF_NAME[f], a[0], a[1], a[2],
                   b[0], b[1], b[2]);
    else
        tagpu_logf("pcheck: T=%u L%d %s object %u particle %d %s: engine %08X form %08X", P, L,
                   PC_NAME[c], obj, k, PF_NAME[f], a[0], b[0]);
}

/* one word of a record against the closed form's */
static int pevt_word(uint32_t P, int L, int c, unsigned obj, int k, int f, const unsigned char* g,
                     int32_t want, unsigned* field)
{
    int32_t v;
    memcpy(&v, g, 4);
    if (v == want) return 0;
    field[f]++;
    pevt_said(P, L, c, obj, k, f, &v, &want, 1);
    return 1;
}

static int pevt_triple(uint32_t P, int L, int c, unsigned obj, int k, int f, const unsigned char* g,
                       const int32_t* want, unsigned* field)
{
    int32_t v[3];
    memcpy(v, g, 12);
    if (!memcmp(v, want, 12)) return 0;
    field[f]++;
    pevt_said(P, L, c, obj, k, f, v, want, 3);
    return 1;
}

/* the object's particles against its event's: answers the records compared, adds the differing
   ones to *diff */
static unsigned pevt_subs(uint32_t P, int L, int c, unsigned obj, const unsigned char* ob,
                          const TAGPU_PEVT* e, unsigned* diff, unsigned* field)
{
    const unsigned char *sb, *se;
    unsigned n, j = 0, k, subs = 0;
    memcpy(&sb, ob + 0x10, sizeof sb);
    memcpy(&se, ob + 0x14, sizeof se);
    n = (se > sb) ? (unsigned)(se - sb) / PC_STRIDE[c] : 0u;
    for (k = 0; k < TAGPU_PEVT_NPART; k++) {
        const unsigned char* g;
        int bad = 0;
        if (c == TAGPU_PEVT_C_NANO) {
            TAGPU_PEVT_NANO q;
            if (!tagpu_pevt_nano_at(e, k, P, &q)) continue;
            if (j >= n || j >= TAGPU_PEVT_NPART) { j++; continue; }
            g = sb + (size_t)j++ * PC_STRIDE[c];
            bad |= pevt_triple(P, L, c, obj, (int)k, PF_POS, g, q.pos, field);
            bad |= pevt_triple(P, L, c, obj, (int)k, PF_END, g + 0x0C, q.end, field);
            bad |= pevt_triple(P, L, c, obj, (int)k, PF_STEP, g + 0x18, q.step, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_COLOUR, g + 0x28, (int32_t)q.colour, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_TICK, g + 0x2C, (int32_t)q.endTick, field);
        } else if (c == TAGPU_PEVT_C_WAKE) {
            TAGPU_PEVT_WAKE q;
            if (!tagpu_pevt_wake_at(e, k, P, &q)) continue;
            if (j >= n || j >= TAGPU_PEVT_NPART) { j++; continue; }
            g = sb + (size_t)j++ * PC_STRIDE[c];
            bad |= pevt_triple(P, L, c, obj, (int)k, PF_POS, g + 0x04, q.pos, field);
            bad |= pevt_triple(P, L, c, obj, (int)k, PF_STEP, g + 0x1C, q.step, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_COLOUR, g + 0x28, TAGPU_PEVT_WAKE_LO, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_COLOUR, g + 0x2C, TAGPU_PEVT_WAKE_HI, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_COLOUR, g + 0x30, q.colour, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_COLOUR, g + 0x34, q.delta, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_FRAME, g + 0x38, q.counter, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_FRAME, g + 0x3C, q.period, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_TICK, g + 0x40, (int32_t)q.endTick, field);
        } else {
            TAGPU_PEVT_SPRITE q;
            if (!tagpu_pevt_sprite_at(e, k, P, &q)) continue;
            if (j >= n || j >= TAGPU_PEVT_NPART) { j++; continue; }
            g = sb + (size_t)j++ * PC_STRIDE[c];
            bad |= pevt_triple(P, L, c, obj, (int)k, PF_POS, g + 0x04, q.pos, field);
            bad |= pevt_triple(P, L, c, obj, (int)k, PF_STEP, g + 0x1C, q.step, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_FRAME, g + 0x28, q.nseq1, field);
            bad |= pevt_word(P, L, c, obj, (int)k, PF_FRAME, g + 0x2C, q.frame, field);
            if (c == TAGPU_PEVT_C_FIRE) {
                bad |= pevt_word(P, L, c, obj, (int)k, PF_FRAME, g + 0x30, q.counter, field);
                bad |= pevt_word(P, L, c, obj, (int)k, PF_FRAME, g + 0x34, q.period, field);
                bad |= pevt_word(P, L, c, obj, (int)k, PF_TICK, g + 0x38, (int32_t)q.endTick, field);
            } else {
                bad |= pevt_word(P, L, c, obj, (int)k, PF_TICK, g + 0x30, (int32_t)q.endTick, field);
            }
        }
        subs++;
        *diff += (unsigned)bad;
    }
    if (j != n) {
        int32_t a = (int32_t)n, b = (int32_t)j;
        field[PF_COUNT]++; (*diff)++;
        pevt_said(P, L, c, obj, -1, PF_COUNT, &a, &b, 1);
    }
    return subs;
}

static void pevt_check(uint32_t P)
{
    const FxLayer* layer = *(const FxLayer* const*)(*(char* const*)0x00511DE8u + 0x38D77);
    unsigned objs[TAGPU_PEVT_NCLASS] = { 0 }, subs[TAGPU_PEVT_NCLASS] = { 0 };
    unsigned diff[TAGPU_PEVT_NCLASS] = { 0 }, missing = 0, extra = 0, trimmed = 0;
    unsigned field[8] = { 0 }, sObjs = 0, sSubs = 0, sDiff = 0, lSubs = 0, lDiff = 0;
    int L, c, f;
    if (!layer) return;
    for (L = 0; L < 10; L++) {
        unsigned cur[TAGPU_PEVT_NCLASS];
        void* const* o;
        for (c = 0; c < (int)TAGPU_PEVT_NCLASS; c++) cur[c] = s_pevt.head;
        for (o = layer[L].begin; o != layer[L].end; o++) {
            const unsigned char* ob = (const unsigned char*)*o;
            const PrandObj* pr;
            uint32_t seed;
            unsigned i;
            c = pevt_class_of(*(const uint32_t*)ob);
            if (c < 0) continue;
            pr = prand_row((unsigned)(size_t)ob);
            seed = pr ? pr->seed : 0u;
            i = pevt_next(cur[c], L, c);
            if (i < s_pevt.tail && s_pevt.row[i].e.seed != seed) {
                unsigned j = i;
                do j = pevt_next(j + 1u, L, c);
                while (j < s_pevt.tail && s_pevt.row[j].e.seed != seed);
                if (j < s_pevt.tail) {
                    unsigned skipped = 0, m;
                    for (m = i; m < j; m = pevt_next(m + 1u, L, c)) skipped++;
                    if (s_pevt.capped & (1u << L)) trimmed += skipped; else missing += skipped;
                    i = j;
                }
            }
            if (i >= s_pevt.tail || s_pevt.row[i].e.seed != seed || !pr) {
                int32_t a = (int32_t)seed, b = i < s_pevt.tail ? (int32_t)s_pevt.row[i].e.seed : 0;
                extra++;
                field[PF_SEED]++;
                pevt_said(P, L, c, objs[c] + 1u, -1, PF_SEED, &a, &b, 1);
                continue;
            }
            cur[c] = i + 1u;
            objs[c]++;
            subs[c] += pevt_subs(P, L, c, objs[c], ob, &s_pevt.row[i].e, &diff[c], field);
        }
        for (c = 0; c < (int)TAGPU_PEVT_NCLASS; c++)
            for (cur[c] = pevt_next(cur[c], L, c); cur[c] < s_pevt.tail;
                 cur[c] = pevt_next(cur[c] + 1u, L, c))
                missing++;
    }
    s_pevt.cTicks++;
    for (c = 0; c < (int)TAGPU_PEVT_NCLASS; c++) {
        s_pevt.cObjs[c] += objs[c]; s_pevt.cSubs[c] += subs[c]; s_pevt.cDiff[c] += diff[c];
        sObjs += objs[c]; sSubs += subs[c]; sDiff += diff[c];
        lSubs += s_pevt.cSubs[c]; lDiff += s_pevt.cDiff[c];
    }
    s_pevt.cMissing += missing; s_pevt.cExtra += extra; s_pevt.cTrimmed += trimmed;
    for (f = 0; f < 8; f++) s_pevt.cField[f] += field[f];
    tagpu_logf("pcheck: T=%u objects %u (missing %u extra %u trimmed %u) particles %u differing %u "
               "[seed %u count %u pos %u end %u step %u colour %u tick %u frame %u] nano %u/%u "
               "fire %u/%u flare %u/%u wake %u/%u level: ticks %u particles %u differing %u, "
               "wake dots dead over ground %u", P, sObjs, missing, extra, trimmed, sSubs, sDiff,
               field[0], field[1], field[2], field[3], field[4], field[5], field[6], field[7],
               subs[0], diff[0], subs[1], diff[1], subs[2], diff[2], subs[3], diff[3],
               s_pevt.cTicks, lSubs, lDiff + s_pevt.cMissing + s_pevt.cExtra, s_pevt.shore);
}

/* THE FRAME PACKET'S COPY (tagpu_packet_pub.c, game thread): the live events whose reach box
   meets [l, r] x [t, b], in list order, the newest `max` of them when more meet it. Answers how
   many meet it; `*wrote` how many were copied. */
unsigned tagpu_pevt_copy(TAGPU_PEVT* dst, unsigned max, int l, int t, int r, int b,
                         unsigned* wrote)
{
    unsigned i, n = 0, skip, w = 0;
    *wrote = 0;
    if (!pevt_live()) return 0;
    for (i = s_pevt.head; i < s_pevt.tail; i++) {
        const PevtRow* q = &s_pevt.row[i];
        if (q->gone && q->x1 >= l && q->x0 <= r && q->z1 >= t && q->z0 <= b) n++;
    }
    skip = n > max ? n - max : 0;
    for (i = s_pevt.head; i < s_pevt.tail && w < max; i++) {
        const PevtRow* q = &s_pevt.row[i];
        if (!(q->gone && q->x1 >= l && q->x0 <= r && q->z1 >= t && q->z0 <= b)) continue;
        if (skip) { skip--; continue; }
        dst[w++] = q->e;
    }
    *wrote = w;
    return n;
}

/* the events are drawn (a Vulkan launch with the walk and the alloc's stub in) */
int tagpu_pevt_on(void) { return pevt_live(); }

/* said once the table's answer is known (tagpu_limits_install), since the alloc's stub is its */
static void pevt_report(void)
{
    tagpu_logf("particles: the nano spray, fire, the teleport flare, the wake and the bubbles %s%s",
               pevt_live() ?
               "are drawn from emitter events (nano 0x47212B 0x47225B and the wake 0x47245A: no "
               "object; fire 0x4742C0, the flare 0x4736E0 and the bubbles 0x474760: a proxy "
               "object in the layer; an event the GPU draws)" :
               s_pevtOff ? s_pevtOff :
               g_config.gdi ? "keep the engine's objects (GDI)" :
               s_limState <= 0 ? "keep the engine's objects: the table is not in" :
               "keep the engine's objects: the particle walk is not in",
               s_pevtCheck && pevt_live() ? "; tagpu_particlecheck.on: the engine's objects are "
               "made in full and held to the events every tick" : "");
}

/* the pool's alloc 0x470EB0 returned `obj` (NULL: the pool was full, or a nano call drawn from
   events was not given one) */
static void __cdecl prand_alloc(unsigned obj)
{
    unsigned seed = prand_pending();
    PrandObj* row;
    s_prand.n++;
    s_prand.emissions++;
    if (s_pevt.pend) {
        const int cls = s_pevt.pend - 1;
        s_pevt.pend = 0;
        if (!s_pevtCheck || obj) pevt_emit(seed, cls);
    }
    if (!obj) return;
    row = prand_row(obj);
    if (!row) {
        s_prand.outside++;
        return;
    }
    row->seed = seed;
    row->next = 0;
}

/* one draw at a redirected site: `code` is PRK_* << 8 | the draw's number (a smoke site's: its
   PRAND_ALT_*), regs the pushad block */
static void __cdecl prand_site(unsigned code, unsigned* regs)
{
    unsigned kind = code >> 8, draw = code & 0xFFu, particle, seed;
    PrandObj none = { 0, 0 };
    PrandObj* row = &none;
    if (kind == PRK_EMIT) {
        seed = prand_pending();
        particle = TAGPU_PRAND_EMIT;
    } else {
        unsigned obj = regs[kind == PRK_NANO ? PR_EBP : kind == PRK_FRAME ? PR_EDI : PR_EBX];
        PrandObj* r = prand_row(obj);
        if (r) row = r;
        else s_prand.outside++;
        seed = row->seed;
        if (kind == PRK_FRAME) {
            unsigned char* puff = (unsigned char*)(size_t)regs[PR_EBX];
            unsigned x, alt, y, low, span = 1u << (6 + draw), last = row->next - 1u;
            memcpy(&x, puff + 0x04, 4);
            memcpy(&alt, puff + 0x08, 4);
            memcpy(&y, puff + 0x0C, 4);
            low = (x & 7u) | (y & 7u) << 3 | (alt & (span / 64u - 1u)) << 6;
            particle = last - ((last - low) & (span - 1u));
            memcpy(&draw, puff + 0x14, 4);
        } else if (kind == PRK_PUFF) {
            /* the caller's frame: esp at the call, past the return address the call pushed */
            unsigned char* local = (unsigned char*)(size_t)(regs[PR_ESP] + 4u + 0x18u);
            unsigned x, alt, y, altmask = (1u << draw) - 1u;
            particle = row->next++;
            memcpy(&x, local + 0x04, 4);
            memcpy(&alt, local + 0x08, 4);
            memcpy(&y, local + 0x0C, 4);
            x = (x & ~7u) | (particle & 7u);
            y = (y & ~7u) | (particle >> 3 & 7u);
            alt = (alt & ~altmask) | (particle >> 6 & altmask);
            memcpy(local + 0x04, &x, 4);
            memcpy(local + 0x08, &alt, 4);
            memcpy(local + 0x0C, &y, 4);
            draw = 0;
        } else {
            particle = draw == 0 ? row->next++ : row->next - 1u;
        }
    }
    s_prand.draws++;
    regs[PR_EAX] = (unsigned)tagpu_prand(seed, particle, draw);
}

/* the LOADER thread, at 0x4918B1: this level's particle seed (THE LEVEL'S PARTICLE SEED) */
static void __cdecl prand_level_begin(void)
{
    const char* ta = *(const char* const*)(size_t)B6_MAIN;
    const char* gs;
    const char* from;
    char stem[48];
    int mode;
    unsigned map;
    unsigned long long st;
    /* GameingState is not NULL here: the loader body dereferenced it at 0x4971CD, as `this` for
       0x435100, on the only path into 0x4917D0 (0x497CA1 -> 0x497581) */
    memcpy(&gs, ta + B6_TNT, sizeof gs);
    memcpy(&mode, gs, sizeof mode);
    map = wind_map_hash(gs, stem, sizeof stem);
    if (mode == B6_NETWORK) {
        st = s_windSource != WIND_NONE ? wind_session_seed(&s_windSession, map) : map;
        from = "the session and the map";
    } else if (s_testSeeds) {
        st = TEST_SEED_WIND ^ ((unsigned long long)map << 32);
        from = "the harness's fixed seed and the map";
    } else {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        st = (unsigned long long)c.QuadPart ^ ((unsigned long long)map << 32);
        from = "the performance counter";
    }
    st ^= PRAND_SALT;
    if (s_prand.levels)
        tagpu_logf("enginefix: particles: the last level drew %u values by the rule over %u "
                   "emissions, %u of them outside the pool", s_prand.draws, s_prand.emissions,
                   s_prand.outside);
    if (s_prand.levels && pevt_live())
        tagpu_logf("particles: the last level's events: nano %u, fire %u, flare %u, wake %u "
                   "emitted; %u trimmed by the layer cap (nano, wake: the engine trims the "
                   "proxies), %u refused (a layer outside 0..9, a wake period outside 1..%d), %u "
                   "dropped (the list full), %u emissions kept their engine object; %u wake dots "
                   "die over ground; %u sea level changes", s_pevt.emitted[0], s_pevt.emitted[1],
                   s_pevt.emitted[2], s_pevt.emitted[3], s_pevt.trimmed, s_pevt.refused,
                   TAGPU_PEVT_WAKE_PERIOD, s_pevt.dropped, s_pevt.kept, s_pevt.shore,
                   s_pevt.resea);
    if (s_prand.levels && s_pevtCheck && pevt_live())
        tagpu_logf("pcheck: the last level: %u ticks; nano %u objects %u particles %u differing; "
                   "fire %u objects %u particles %u differing; flare %u objects %u particles %u "
                   "differing; wake %u objects %u particles %u differing; %u events with no "
                   "object, %u objects with no event, %u trimmed in a layer at its cap (mask %03X) "
                   "[seed %u count %u pos %u end %u step %u colour %u tick %u frame %u]",
                   s_pevt.cTicks, s_pevt.cObjs[0], s_pevt.cSubs[0], s_pevt.cDiff[0],
                   s_pevt.cObjs[1], s_pevt.cSubs[1], s_pevt.cDiff[1], s_pevt.cObjs[2],
                   s_pevt.cSubs[2], s_pevt.cDiff[2], s_pevt.cObjs[3], s_pevt.cSubs[3],
                   s_pevt.cDiff[3], s_pevt.cMissing, s_pevt.cExtra, s_pevt.cTrimmed,
                   s_pevt.capped, s_pevt.cField[0], s_pevt.cField[1], s_pevt.cField[2],
                   s_pevt.cField[3], s_pevt.cField[4], s_pevt.cField[5], s_pevt.cField[6],
                   s_pevt.cField[7]);
    pevt_clear();
    s_pevt.walked = 0xFFFFFFFFu;        /* no walk of this level yet */
    s_pevt.sea = -1;                    /* the map's sea level is loaded after this (0x4918C0) */
    s_pevt.refused = s_pevt.trimmed = s_pevt.dropped = s_pevt.kept = s_pevt.capped = 0;
    s_pevt.shore = s_pevt.resea = 0;
    memset(s_pevt.emitted, 0, sizeof s_pevt.emitted);
    memset(s_pevt.cObjs, 0, sizeof s_pevt.cObjs);
    memset(s_pevt.cSubs, 0, sizeof s_pevt.cSubs);
    memset(s_pevt.cDiff, 0, sizeof s_pevt.cDiff);
    s_pevt.cTicks = s_pevt.cMissing = s_pevt.cExtra = s_pevt.cTrimmed = s_pevt.cSaid = 0;
    memset(s_pevt.cField, 0, sizeof s_pevt.cField);
    s_prand.level = (unsigned)(seed_mix(&st) >> 32);
    s_prand.tick = 0xFFFFFFFFu;
    s_prand.n = 0;
    s_prand.draws = s_prand.emissions = s_prand.outside = 0;
    s_prand.levels++;
    tagpu_logf("enginefix: particles: level %u draws from seed 0x%08X (%s, map \"%s\")",
               s_prand.levels, s_prand.level, from, stem);
}

static int fix_particle_rand(void)
{
    static const struct { unsigned int va; unsigned short code; } draws[20] = {
        { 0x00472ACF, PRK_EMIT << 8 | 0 }, { 0x00472AF4, PRK_EMIT << 8 | 1 },
        { 0x00472B17, PRK_EMIT << 8 | 2 }, { 0x00472B3A, PRK_EMIT << 8 | 3 },
        { 0x00424345, PRK_EMIT << 8 | 0 }, { 0x00424379, PRK_EMIT << 8 | 1 },
        { 0x0047395F, PRK_FLARE << 8 | 0 },
        { 0x00473DAD, PRK_NANO << 8 | 0 }, { 0x00473DCA, PRK_NANO << 8 | 1 },
        { 0x00473DE7, PRK_NANO << 8 | 2 }, { 0x00473E04, PRK_NANO << 8 | 3 },
        { 0x00473E21, PRK_NANO << 8 | 4 }, { 0x00473E3E, PRK_NANO << 8 | 5 },
        { 0x004749D9, PRK_WAKE << 8 | 0 }, { 0x00474A00, PRK_WAKE << 8 | 1 },
        { 0x00474A27, PRK_WAKE << 8 | 2 },
        { 0x00474F1C, PRK_PUFF << 8 | PRAND_ALT_SMOKE2 }, { 0x004752D4, PRK_PUFF << 8 | PRAND_ALT_SMOKE1 },
        { 0x004753B9, PRK_FRAME << 8 | PRAND_ALT_SMOKE2 }, { 0x00475679, PRK_FRAME << 8 | PRAND_ALT_SMOKE1 },
    };
    static const unsigned char alloc[7] = { 0x8B, 0x51, 0x20, 0x56, 0x8B, 0x71, 0x1C };
    static const unsigned char layers[5] = { 0xE8, 0xDA, 0x04, 0xFE, 0xFF };   /* call 0x471D90 */
    unsigned char *a, *l, *p;
    int i;
    unsigned char* skip;
    if (!(a = fix_code(96)) || !(l = fix_code(16))) {
        lim_no_stub();
        return FIX_TABLE;
    }
    /* WHAT pevt_pre READS OUT OF A NANO EMITTER'S FRAME is laid out by the emitter's own code from
       its entry to its layer's read (0x4720D0..0x472187, 0x472200..0x4722B7: the frame, the
       degenerate box, the SFX test, the alloc call whose return address names the caller, the
       NULL test that skips the init, the init's arguments and the layer's word), so those bytes are
       held to stock with the table, all or nothing. The first 7 of each are the recorder's
       observer (tagpu_recorder.c OBSERVE_SITES, which byte-matches them itself and runs them as
       stock) and would read as a rewrite at the table's verify, so they are compared here, before
       the recorder installs: other bytes there leave the events off and the engine's objects on. */
    s_pevtOn = (unsigned char)!g_config.gdi;
    {
    static const unsigned char fwd[0xB8] = {   /* 0x4720D0..0x472187 */
        0x83, 0xEC, 0x18, 0x8B, 0x44, 0x24, 0x1C, 0x53, 0x8B, 0xC8, 0x55, 0x33, 0xDB, 0x56, 0x8B, 0x11,
        0x57, 0x89, 0x54, 0x24, 0x10, 0x8B, 0x51, 0x04, 0x89, 0x54, 0x24, 0x14, 0x8B, 0x10, 0x8B, 0x49,
        0x08, 0x89, 0x54, 0x24, 0x1C, 0x8B, 0x50, 0x08, 0x89, 0x4C, 0x24, 0x18, 0x8B, 0x48, 0x04, 0xA1,
        0xE8, 0x1D, 0x51, 0x00, 0x89, 0x4C, 0x24, 0x20, 0x89, 0x54, 0x24, 0x24, 0x8B, 0xA8, 0x77, 0x8D,
        0x03, 0x00, 0xA0, 0x08, 0xE6, 0x51, 0x00, 0x3A, 0xC3, 0x74, 0x04, 0x33, 0xC0, 0xEB, 0x1F, 0x6A,
        0x4C, 0xB9, 0x10, 0xE6, 0x51, 0x00, 0xE8, 0x85, 0xED, 0xFF, 0xFF, 0x8B, 0xD0, 0x3B, 0xD3, 0x74,
        0x0B, 0xB9, 0x13, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x8B, 0xFA, 0xF3, 0xAB, 0x8B, 0xC2, 0x3B, 0xC3,
        0x74, 0x1D, 0x8A, 0x4C, 0x24, 0x34, 0x89, 0x58, 0x04, 0x88, 0x48, 0x0C, 0x89, 0x58, 0x10, 0x89,
        0x58, 0x14, 0x89, 0x58, 0x18, 0xC7, 0x00, 0xB8, 0xD5, 0x4F, 0x00, 0x8B, 0xF0, 0xEB, 0x02, 0x33,
        0xF6, 0x3B, 0xF3, 0x0F, 0x84, 0x86, 0x00, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x30, 0x8B, 0x16, 0x6A,
        0x01, 0x8D, 0x4C, 0x24, 0x14, 0x50, 0x51, 0x8B, 0xCE, 0xFF, 0x52, 0x18, 0x0F, 0xBF, 0x54, 0x24,
        0x34, 0xC1, 0xE2, 0x04, 0x89, 0x74, 0x24, 0x2C,
    };
    static const unsigned char rev[0xB8] = {   /* 0x472200..0x4722B7 */
        0x83, 0xEC, 0x18, 0x8B, 0x44, 0x24, 0x20, 0x53, 0x8B, 0xC8, 0x55, 0x33, 0xDB, 0x56, 0x8B, 0x11,
        0x57, 0x89, 0x54, 0x24, 0x10, 0x8B, 0x51, 0x04, 0x89, 0x54, 0x24, 0x14, 0x8B, 0x10, 0x8B, 0x49,
        0x08, 0x89, 0x54, 0x24, 0x1C, 0x8B, 0x50, 0x08, 0x89, 0x4C, 0x24, 0x18, 0x8B, 0x48, 0x04, 0xA1,
        0xE8, 0x1D, 0x51, 0x00, 0x89, 0x4C, 0x24, 0x20, 0x89, 0x54, 0x24, 0x24, 0x8B, 0xA8, 0x77, 0x8D,
        0x03, 0x00, 0xA0, 0x08, 0xE6, 0x51, 0x00, 0x3A, 0xC3, 0x74, 0x04, 0x33, 0xC0, 0xEB, 0x1F, 0x6A,
        0x4C, 0xB9, 0x10, 0xE6, 0x51, 0x00, 0xE8, 0x55, 0xEC, 0xFF, 0xFF, 0x8B, 0xD0, 0x3B, 0xD3, 0x74,
        0x0B, 0xB9, 0x13, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x8B, 0xFA, 0xF3, 0xAB, 0x8B, 0xC2, 0x3B, 0xC3,
        0x74, 0x1D, 0x8A, 0x4C, 0x24, 0x34, 0x89, 0x58, 0x04, 0x88, 0x48, 0x0C, 0x89, 0x58, 0x10, 0x89,
        0x58, 0x14, 0x89, 0x58, 0x18, 0xC7, 0x00, 0xB8, 0xD5, 0x4F, 0x00, 0x8B, 0xF0, 0xEB, 0x02, 0x33,
        0xF6, 0x3B, 0xF3, 0x0F, 0x84, 0x86, 0x00, 0x00, 0x00, 0x8B, 0x4C, 0x24, 0x2C, 0x8B, 0x16, 0x8D,
        0x44, 0x24, 0x10, 0x6A, 0x01, 0x50, 0x51, 0x8B, 0xCE, 0xFF, 0x52, 0x18, 0x0F, 0xBF, 0x54, 0x24,
        0x34, 0xC1, 0xE2, 0x04, 0x89, 0x74, 0x24, 0x30,
    };
        for (i = 0; i < 0xB8 - 7; i += LIM_MAXB) {
            const int n = 0xB8 - 7 - i < LIM_MAXB ? 0xB8 - 7 - i : LIM_MAXB;
            lim_same(0x004720D7u + (unsigned)i, n, fwd + 7 + i,
                     "particles: the forward nano emitter's frame, read by the events");
            lim_same(0x00472207u + (unsigned)i, n, rev + 7 + i,
                     "particles: the reverse nano emitter's frame, read by the events");
        }
        if (!tagpu_detour_bytes_ok(0x004720D0u, fwd, 7) || !tagpu_detour_bytes_ok(0x00472200u, rev, 7))
            s_pevtOff = "keep the engine's objects: the nano emitters' entries hold other bytes";
    }
    {
    /* THE WAKE EMITTER'S FRAME, read by pevt_pre likewise, from its entry to its layer's read
       (0x472430..0x4724B6: the SFX test, the alloc call, the NULL skip, the init's arguments --
       the life 1 and dir 1 the event assumes -- and the layer's word). Its first 5 bytes are the
       recorder's sink (OBSERVE_SITES), compared here before it installs. */
    static const unsigned char wakeE[0x87] = {   /* 0x472430..0x4724B6 */
            0xA1, 0xE8, 0x1D, 0x51, 0x00, 0x53, 0x55, 0x33, 0xDB, 0x8B, 0xA8, 0x77, 0x8D, 0x03, 0x00, 0xA0,
            0x08, 0xE6, 0x51, 0x00, 0x56, 0x3A, 0xC3, 0x57, 0x74, 0x04, 0x33, 0xC0, 0xEB, 0x1F, 0x6A, 0x48,
            0xB9, 0x10, 0xE6, 0x51, 0x00, 0xE8, 0x56, 0xEA, 0xFF, 0xFF, 0x8B, 0xD0, 0x3B, 0xD3, 0x74, 0x0B,
            0xB9, 0x12, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x8B, 0xFA, 0xF3, 0xAB, 0x8B, 0xC2, 0x3B, 0xC3, 0x74,
            0x1D, 0x8A, 0x4C, 0x24, 0x20, 0x89, 0x58, 0x04, 0x88, 0x48, 0x0C, 0x89, 0x58, 0x10, 0x89, 0x58,
            0x14, 0x89, 0x58, 0x18, 0xC7, 0x00, 0xF8, 0xD5, 0x4F, 0x00, 0x8B, 0xF0, 0xEB, 0x02, 0x33, 0xF6,
            0x3B, 0xF3, 0x0F, 0x84, 0x8D, 0x00, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x1C, 0x8B, 0x4C, 0x24, 0x18,
            0x8B, 0x16, 0x6A, 0x01, 0x6A, 0x01, 0x50, 0x8B, 0x44, 0x24, 0x20, 0x51, 0x50, 0x8B, 0xCE, 0xFF,
            0x52, 0x18, 0x0F, 0xBF, 0x4C, 0x24, 0x20,
    };
        lim_same_run(0x00472435u, (int)sizeof wakeE - 5, wakeE + 5,
                     "particles: the wake emitter's frame, read by the events");
        if (!tagpu_detour_bytes_ok(0x00472430u, wakeE, 5))
            s_pevtOff = "keep the engine's objects: the wake emitter's entry holds other bytes";
    }
    {
    /* THE SPRITE EMITTERS' BYTES the proxies rest on, from the alloc's return to the init's call:
       the NULL test that skips the init, the zeroing, the layer's byte at +0xC, the class's vtable
       and the init's arguments (THE PARTICLES AS EVENTS). None is another hook's: the recorder's
       sinks take the entries (0x471FD0, 0x472330, 0x472AB0, 0x472530), the limits the caps after
       the init. */
    static const unsigned char flareE[0x54] = {   /* 0x471FFA..0x47204D */
            0x8B, 0xD0, 0x3B, 0xD3, 0x74, 0x0B, 0xB9, 0x11, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x8B, 0xFA, 0xF3,
            0xAB, 0x8B, 0xC2, 0x3B, 0xC3, 0x74, 0x1D, 0x8A, 0x4C, 0x24, 0x20, 0x89, 0x58, 0x04, 0x88, 0x48,
            0x0C, 0x89, 0x58, 0x10, 0x89, 0x58, 0x14, 0x89, 0x58, 0x18, 0xC7, 0x00, 0x88, 0xD5, 0x4F, 0x00,
            0x8B, 0xF0, 0xEB, 0x02, 0x33, 0xF6, 0x3B, 0xF3, 0x0F, 0x84, 0x89, 0x00, 0x00, 0x00, 0x8B, 0x44,
            0x24, 0x1C, 0x8B, 0x4C, 0x24, 0x18, 0x8B, 0x16, 0x50, 0x8B, 0x44, 0x24, 0x18, 0x51, 0x50, 0x8B,
            0xCE, 0xFF, 0x52, 0x18
    };
    static const unsigned char cobE[0x59] = {     /* 0x47235A..0x4723B2 */
            0x8B, 0xD0, 0x3B, 0xD3, 0x74, 0x0B, 0xB9, 0x11, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x8B, 0xFA, 0xF3,
            0xAB, 0x8B, 0xC2, 0x3B, 0xC3, 0x74, 0x1D, 0x8A, 0x4C, 0x24, 0x24, 0x89, 0x58, 0x04, 0x88, 0x48,
            0x0C, 0x89, 0x58, 0x10, 0x89, 0x58, 0x14, 0x89, 0x58, 0x18, 0xC7, 0x00, 0xD8, 0xD5, 0x4F, 0x00,
            0x8B, 0xF0, 0xEB, 0x02, 0x33, 0xF6, 0x3B, 0xF3, 0x0F, 0x84, 0x8E, 0x00, 0x00, 0x00, 0x8B, 0x44,
            0x24, 0x20, 0x8B, 0x4C, 0x24, 0x1C, 0x8B, 0x16, 0x50, 0x8B, 0x44, 0x24, 0x1C, 0x51, 0x8B, 0x4C,
            0x24, 0x1C, 0x50, 0x51, 0x8B, 0xCE, 0xFF, 0x52, 0x18
    };
    static const unsigned char pieceE[0x50] = {   /* 0x472B7F..0x472BCE */
            0x8B, 0xF0, 0x3B, 0xF7, 0x74, 0x2C, 0xB9, 0x11, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x8B, 0xFE, 0xF3,
            0xAB, 0x33, 0xFF, 0x3B, 0xF7, 0x74, 0x1B, 0x8A, 0x54, 0x24, 0x24, 0x89, 0x7E, 0x04, 0x88, 0x56,
            0x0C, 0x89, 0x7E, 0x10, 0x89, 0x7E, 0x14, 0x89, 0x7E, 0x18, 0xC7, 0x06, 0xD8, 0xD5, 0x4F, 0x00,
            0xEB, 0x02, 0x33, 0xF6, 0x3B, 0xF7, 0x0F, 0x84, 0x88, 0x00, 0x00, 0x00, 0x8B, 0x06, 0x55, 0x8D,
            0x4C, 0x24, 0x14, 0x6A, 0x01, 0x8D, 0x54, 0x24, 0x18, 0x51, 0x52, 0x8B, 0xCE, 0xFF, 0x50, 0x18
    };
    static const unsigned char bubbleE[0x57] = {  /* 0x47255A..0x4725B0 */
            0x8B, 0xD0, 0x3B, 0xD3, 0x74, 0x0B, 0xB9, 0x12, 0x00, 0x00, 0x00, 0x33, 0xC0, 0x8B, 0xFA, 0xF3,
            0xAB, 0x8B, 0xC2, 0x3B, 0xC3, 0x74, 0x1D, 0x8A, 0x4C, 0x24, 0x20, 0x89, 0x58, 0x04, 0x88, 0x48,
            0x0C, 0x89, 0x58, 0x10, 0x89, 0x58, 0x14, 0x89, 0x58, 0x18, 0xC7, 0x00, 0xF8, 0xD5, 0x4F, 0x00,
            0x8B, 0xF0, 0xEB, 0x02, 0x33, 0xF6, 0x3B, 0xF3, 0x0F, 0x84, 0x8C, 0x00, 0x00, 0x00, 0x8B, 0x44,
            0x24, 0x1C, 0x8B, 0x4C, 0x24, 0x18, 0x8B, 0x16, 0x53, 0x6A, 0x01, 0x50, 0x8B, 0x44, 0x24, 0x20,
            0x51, 0x50, 0x8B, 0xCE, 0xFF, 0x52, 0x18
    };
    static const unsigned char fireInit[6] = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x18 };
    static const unsigned char flareInit[5] = { 0x8B, 0x44, 0x24, 0x0C, 0x53 };
    static const unsigned char wakeInit[5] = { 0x8B, 0x44, 0x24, 0x10, 0x53 };
    static const struct {
        unsigned va; const unsigned char* stolen; int n; void* hook; unsigned char ret;
    } inits[3] = {
        { 0x004742C0u, fireInit, 6, (void*)pevt_fire, 0x10 },    /* push ebx; push ebp;
                                                                      mov ebp,[esp+0x18]      */
        { 0x004736E0u, flareInit, 5, (void*)pevt_flare, 0x0C },  /* mov eax,[esp+0xC]; push ebx */
        { 0x00474760u, wakeInit, 5, (void*)pevt_wake, 0x14 },    /* mov eax,[esp+0x10]; push ebx */
    };
        lim_same_run(0x00471FFAu, sizeof flareE, flareE,
                     "particles: the flare emitter's call of its init, read by the proxy");
        lim_same_run(0x0047235Au, sizeof cobE, cobE,
                     "particles: the COB fire emitter's call of its init, read by the proxy");
        lim_same_run(0x00472B7Fu, sizeof pieceE, pieceE,
                     "particles: the pieces' fire emitter's call of its init, read by the proxy");
        lim_same_run(0x0047255Au, sizeof bubbleE, bubbleE,
                     "particles: the bubbles emitter's call of its init, read by the proxy");
        pevt_proxy_vtable(s_proxyFire, PEVT_VT_FIRE);
        pevt_proxy_vtable(s_proxyFlare, PEVT_VT_FLARE);
        pevt_proxy_vtable(s_proxyWake, PEVT_VT_WAKE);
        /* the init at its entry: the event and the proxy first (pevt_fire, pevt_flare, pevt_wake);
           1 returns past the init with its own `ret`, 0 runs it from its stolen bytes */
        for (i = 0; i < 3; i++) {
            unsigned char* s = fix_code(40);
            if (!s) {
                lim_no_stub();
                return FIX_TABLE;
            }
            p = s;
            *p++ = 0x60;                                        /* pushad                   */
            *p++ = 0x54;                                        /* push esp: the registers  */
            *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)inits[i].hook); p += 4;
            *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;              /* add esp,4                */
            *p++ = 0x85; *p++ = 0xC0;                           /* test eax,eax             */
            *p++ = 0x61;                                        /* popad: the flags kept    */
            *p++ = 0x75; *p++ = (unsigned char)(inits[i].n + 5); /* jnz proxy               */
            memcpy(p, inits[i].stolen, (size_t)inits[i].n); p += inits[i].n;
            *p++ = 0xE9; tagpu_detour_rel(p, inits[i].va + (unsigned)inits[i].n); p += 4;
            *p++ = 0xC2; *p++ = inits[i].ret; *p++ = 0x00;      /* proxy: ret N             */
            lim_branch(inits[i].va, inits[i].n, inits[i].stolen, 0xE9, (unsigned int)(size_t)s,
                       "particles: a proxy class's init records its event");
        }
    }
    if (s_pevtOff) s_pevtOn = 0;
    s_pevtCheck = (unsigned char)(s_pevtOn && tagpu_sim_lever("tagpu_particlecheck.on"));
    /* the alloc: first whether the pool is asked at all (pevt_pre: a nano call drawn from events
       is not, and answers NULL), then the engine's own body on the engine's arguments, then the
       count. Its registers on return are the body's (popad); on the skip path the entry's with
       eax 0, which differs from stock's full pool in edx alone (stock leaves [ecx+0x20] there):
       the skip path serves only the two nano emitters, and both overwrite edx at once with the
       answer (0x47212B / 0x47225B `mov edx,eax`) */
    p = a;
    *p++ = 0x60;                                                /* pushad                   */
    *p++ = 0x54;                                                /* push esp: the registers  */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)pevt_pre); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                      /* add esp,4                */
    *p++ = 0x85; *p++ = 0xC0;                                   /* test eax,eax             */
    *p++ = 0x61;                                                /* popad: the flags kept    */
    *p++ = 0x75; skip = p++;                                    /* jnz skip                 */
    *p++ = 0x51;                                                /* push ecx: the pool       */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x08;         /* push [esp+8]: its size   */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)(a + 72)); p += 4;
    *p++ = 0x59;                                                /* pop ecx                  */
    *p++ = 0x60;                                                /* pushad                   */
    *p++ = 0x50;                                                /* push eax: the object     */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)prand_alloc); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                      /* add esp,4                */
    *p++ = 0x61;                                                /* popad                    */
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;                      /* ret 4                    */
    *skip = (unsigned char)(p - (skip + 1));
    *p++ = 0x60;                                                /* skip: pushad             */
    *p++ = 0x6A; *p++ = 0x00;                                   /* push 0: no object        */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)prand_alloc); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                      /* add esp,4                */
    *p++ = 0x61;                                                /* popad                    */
    *p++ = 0x33; *p++ = 0xC0;                                   /* xor eax,eax              */
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;                      /* ret 4                    */
    p = a + 72;                                                 /* the body: stolen bytes   */
    memcpy(p, alloc, sizeof alloc); p += sizeof alloc;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00470EB7);
    lim_branch(0x00470EB0, sizeof alloc, alloc, 0xE9, (unsigned int)(size_t)a,
               "particles: the pool's alloc counts the emission");
    p = l;
    *p++ = 0x60;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)prand_level_begin); p += 4;
    *p++ = 0x61;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00471D90);
    lim_branch(0x004918B1, sizeof layers, layers, 0xE8, (unsigned int)(size_t)l,
               "particles: the level's seed, before the map's features");
    for (i = 0; i < 20; i++) {
        unsigned char call[5];
        unsigned int rel = 0x004E4870u - (draws[i].va + 5u);    /* call 0x4E4870, the CRT's rand */
        unsigned int code = draws[i].code;
        unsigned char* s = fix_code(24);
        if (!s) {
            lim_no_stub();
            return FIX_TABLE;
        }
        p = s;
        *p++ = 0x60;                                            /* pushad                   */
        *p++ = 0x54;                                            /* push esp: the registers  */
        *p++ = 0x68; memcpy(p, &code, 4); p += 4;               /* push code                */
        *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)prand_site); p += 4;
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                  /* add esp,8                */
        *p++ = 0x61;                                            /* popad: eax the value     */
        *p++ = 0xC3;
        call[0] = 0xE8;
        memcpy(call + 1, &rel, 4);
        lim_branch(draws[i].va, sizeof call, call, 0xE8, (unsigned int)(size_t)s,
                   "particles: a draw by their own rule");
    }
    return FIX_TABLE;
}

/* TWO PARTICLE INITS THAT DIVIDE BY ZERO [DISASSEMBLED].
   - The teleport flare's init 0x4736E0 takes steps = the int16 high word of (len << 16) / 0x50000,
     len the 16.16 distance's _ftol'd length, and divides each axis by it (0x473788: `movsx ecx,
     word [esp+0x1A]; cdq; idiv ecx`, then 0x473799, 0x4737A2): a teleport shorter than 5 world
     units faults.
   - The wake's init 0x474760 divides 2^32 by the sign-extended low dword of len * 2 (0x47480A:
     `cdq; push edx; push eax; push 1; push 0; call _alldiv 0x4E4440`): two equal points fault, as
     bubbles emitted exactly at sea level are (EMIT_SFX 0x103 passes the point and the same point
     at sea level, 0x481102).
   THE FIX tests the divisor first. Zero ends the init where it ends anyway, at its epilogue (the
   flare's pops at 0x4737AE, `ret 0xC`; the wake's at 0x474875, `ret 0x14`), the stack and the x87
   stack there as at the test: every call between is callee-cleaned or takes registers (both: 0x471D70
   `ret 4` and _ftol 0x4E43A0, which pops the x87 top; the flare: _allshl 0x4E44F0 and _alldiv
   0x4E4440 `ret 0x10`; the wake: _allmul 0x4E4400 `ret 0x10` and _allshr 0x4E43D0) and the three
   `fstp` have emptied the x87 stack. The init then
   makes no spawn, so the object holds no particle, and the walk destroys it as empty before it
   updates it (slot 3 first: particle_walk). Nothing else changes: a non-zero divisor runs stock's
   instructions.
   CLASS: local. Without it the engine faults on such an emission. */
static int fix_particle_divides(void)
{
    FIXSITE site[2] = {
        { 0x00473788, 8, { 0x0F, 0xBF, 0x4C, 0x24, 0x1A, 0x99, 0xF7, 0xF9 }, { 0 } },
        { 0x0047480A, 12, { 0x99, 0x52, 0x50, 0x6A, 0x01, 0x6A, 0x00,
                            0xE8, 0x2A, 0xFC, 0x06, 0x00 }, { 0 } },   /* ... call 0x4E4440 */
    };
    unsigned char *f, *w, *p;
    if (!fix_match(site, 2)) return FIX_BYTES;
    if (!(f = fix_code(24)) || !(w = fix_code(32))) return FIX_STUB;
    p = f;
    memcpy(p, site[0].was, 5); p += 5;                          /* movsx ecx,word [esp+0x1A] */
    *p++ = 0x85; *p++ = 0xC9;                                   /* test ecx,ecx             */
    *p++ = 0x74; *p++ = 0x08;                                   /* jz none                  */
    *p++ = 0x99; *p++ = 0xF7; *p++ = 0xF9;                      /* cdq; idiv ecx            */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00473790); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x004737AE);               /* none: the epilogue       */
    p = w;
    *p++ = 0x85; *p++ = 0xC0;                                   /* test eax,eax             */
    *p++ = 0x74; *p++ = 0x11;                                   /* jz none                  */
    memcpy(p, site[1].was, 7); p += 7;                          /* cdq; push edx; push eax;
                                                                   push 1; push 0          */
    *p++ = 0xE8; tagpu_detour_rel(p, 0x004E4440); p += 4;       /* call _alldiv             */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00474816); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00474875);               /* none: the epilogue       */
    fix_branch(&site[0], 0xE9, f);
    fix_branch(&site[1], 0xE9, w);
    return fix_write(site, 2);
}

SIM_FEATURE(yardmap, SIM_PUPPET_RUNS, "battle");

/* A YARDMAP PARSED PAST ITS STRING [DISASSEMBLED]. For a def whose BMcode is 0 ([def+0x22F],
   0x42CF30..0x42CF38) the unit-def loader reads the FBI's YardMap key with
   GetString 0x4C48C0(buf, "YardMap", 0x400, "") into a 0x400-byte stack buffer at the call
   site's [esp+0x128] (0x42CF3E..0x42CF59); GetString NUL-terminates inside that size, or copies
   the "" default when the key is missing, and the return is ignored. The loader then allocates
   footX * footZ bytes (+0x14A, +0x14C, words) at def+0x14E through 0x4D83B0 (0x42CF5E..0x42CF7A)
   and fills them row by row (0x42CF9D..0x42D06B): a char of `.CGOYcfowy` writes one cell
   (0x00 0x35 0x8F 0x2B 0x31 0x2D 0x6F 0x2F 0x37 0x29, the byte map 0x42D198 into the jump
   table 0x42D16C) and moves on unless the NEXT char is the NUL, so a short string repeats its
   last char; any other char, the NUL included, is skipped without filling a cell (0x42CFB5 ->
   0x42D049). So a missing key, or a string that ends on an invalid char, walks past the NUL
   into whatever the stack held and on up it until every cell has a byte: the yardmap decides
   placement (0x47D2E0), occupancy (0x47CC30) and pathing, and each peer's stack is its own.
   No retail unit reaches it (all 126 BMcode-0 units have a YardMap that ends on a valid char);
   a mod does. TADR's fix leaves def+0x14E NULL, which six readers dereference unchecked.
   THE FIX: a jmp at 0x42CF5E into yard_parse, which makes stock's allocation through the same
   allocator and fills it by stock's table, skip rule and repeat rule, rejoining at 0x42D079,
   where nothing the parse held is read again (ecx, eax, esi and edx are written first, ebx and
   edi after 0x42D0E3). Where stock would step past the NUL with cells left, each remaining cell
   takes the last valid char's byte, or 'o' (0x2F) when the string had none [DECIDED
   2026-09-25].
   THE INVARIANT: the parse reads only inside the string GetString wrote — never past its NUL,
   never past the buffer's 0x400 bytes, whether or not a NUL is there — and writes only the
   footX * footZ cells it allocated. Identical to stock for every string stock parses inside
   its NUL, all 126 retail yardmaps among them.
   CLASS: simulation, fail closed. A player without it builds by the bytes of its own stack. */
#define YARD_BUF 0x400
static struct { volatile LONG filled; } s_yardDiag;   /* defs filled past their string, peekable */
SIM_VAR(s_yardDiag, SIM_PRIVATE);

static int yard_code(char c)
{
    switch (c) {
    case '.': return 0x00;
    case 'C': return 0x35;
    case 'G': return 0x8F;
    case 'O': return 0x2B;
    case 'Y': return 0x31;
    case 'c': return 0x2D;
    case 'f': return 0x6F;
    case 'o': return 0x2F;
    case 'w': return 0x37;
    case 'y': return 0x29;
    default:  return -1;
    }
}

static void __cdecl yard_parse(unsigned int* regs)
{
    unsigned char* def = (unsigned char*)(size_t)regs[PR_EBP];
    const char* buf = (const char*)(size_t)(regs[PR_ESP] + 0x128);
    const char* const end = buf + YARD_BUF;
    const char* p = buf;
    short fx, fz;
    int cells, k, last = -1, past = 0;
    unsigned char* map;
    memcpy(&fx, def + 0x14A, 2);
    memcpy(&fz, def + 0x14C, 2);
    map = (unsigned char*)ENG_ALLOC((const char*)(size_t)0x00503978u,
                                    (unsigned int)((int)fz * (int)fx));
    memcpy(def + 0x14E, &map, sizeof map);
    cells = (fx > 0 && fz > 0) ? (int)fx * (int)fz : 0;
    for (k = 0; k < cells; ) {
        int code;
        if (p >= end || *p == 0) {                 /* stock would step past the string here */
            map[k++] = (unsigned char)(last >= 0 ? last : 0x2F);
            past++;
            continue;
        }
        code = yard_code(*p);
        if (code < 0) { p++; continue; }           /* skipped, the cell still to fill        */
        map[k++] = (unsigned char)code;
        last = code;
        if (p + 1 < end && p[1] != 0) p++;         /* the last char before the NUL repeats   */
    }
    if (past) {
        LONG n = InterlockedIncrement(&s_yardDiag.filled);
        if (n <= 32 || !(n & (n - 1)))
            tagpu_logf("enginefix: yardmaps: %.32s: %d of its %d cells lie past its YardMap "
                       "string and take 0x%02X (%ld so far)", (const char*)def, past, cells,
                       last >= 0 ? last : 0x2F, (long)n);
    }
}

static int fix_yardmap(void)
{
    static const unsigned char read[32] = {
        0x8B, 0x4C, 0x24, 0x14, 0x68, 0xB8, 0x19, 0x51, 0x00, 0x68, 0x00, 0x04, 0x00, 0x00, 0x8D,
        0x94, 0x24, 0x30, 0x01, 0x00, 0x00, 0x68, 0x88, 0x39, 0x50, 0x00, 0x52, 0xE8, 0x62, 0x79,
        0x09, 0x00,
    };
    static const unsigned char parse[7] = { 0x0F, 0xBF, 0x85, 0x4C, 0x01, 0x00, 0x00 };
    static const unsigned char join[7]  = { 0x0F, 0xBF, 0x8D, 0x4A, 0x01, 0x00, 0x00 };
    unsigned char *a, *p;
    if (!(a = fix_code(24))) { lim_no_stub(); return FIX_TABLE; }
    p = fix_call_regs(a, yard_parse);
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0042D079);
    lim_same(0x0042CF3E, sizeof read, read, "yardmaps: the key's read into its 0x400 bytes");
    lim_branch(0x0042CF5E, sizeof parse, parse, 0xE9, (unsigned int)(size_t)a,
               "yardmaps: the parse, inside the string");
    lim_same(0x0042D079, sizeof join, join, "yardmaps: the footprint box, where the parse rejoins");
    return FIX_TABLE;
}

SIM_FEATURE(save_order_fallback, SIM_PUPPET_RUNS, "battle");

/* THE SAVED-GAME LOADER'S ORDER FALLBACK [DISASSEMBLED]. The order loader 0x43A420 takes an
   order's type from its `<key>_name` (0x43C6B0, then a strcmp against the table; not found ->
   0x43A552, "Ready", dl = 0). A record without the name falls back to its stored index and
   walks the order table 0x512344 counting records whose +0x14 bit 0 is clear
   (0x43A556..0x43A58F): the walk runs while the record is <= the table's end 0x512348
   (0x43A58B `cmp eax,esi; jbe 0x43A56D`), one record past it, and an index it does not meet
   leaves dl at the count (0x43A58F -> 0x43A598 `mov dl,cl`), 68 or 69 against a table of 68
   records: the order's type byte, and a wild dispatch on its first tick. This exe's writer
   always stores the name (0x43AA90..0x43AAE3), so only a foreign or damaged save reaches it.
   The table holds 68 records for the whole process (UIPipelinesInit 0x491200's one fill), so
   the walk's first record, examined before the bound, is always a real one.
   THE FIX, two bytes: `jbe` -> `jb` at 0x43A58D (the end is exclusive), and the not-found jmp
   at 0x43A58F -> 0x43A552 (EB 07 -> EB C1), the by-name branch's own "Ready". ebx and the rest
   reach 0x43A59A as they did; only dl differs, and only for an index the table does not hold.
   THE INVARIANT: an order's type is an index below the table's count, or 0.
   CLASS: local. A malformed save's fate, on one machine. */
static int fix_save_order_fallback(void)
{
    FIXSITE site = { 0x0043A58D, 4, { 0x76, 0xDE, 0xEB, 0x07 }, { 0x72, 0xDE, 0xEB, 0xC1 } };
    if (!fix_match(&site, 1)) return FIX_BYTES;
    return fix_write(&site, 1);
}

SIM_FEATURE(stockpile_bar, SIM_PUPPET_RUNS, "battle");

/* THE STOCKPILE BAR'S DIVIDE [DISASSEMBLED]. 0x439D20(unit), called once at 0x46B446 for the
   bottom panel, finds the unit's first order with +0x42 bit 19 (0x80000, the weapon build),
   reads its slot index +0x36 and progress +0x3E, the slot's weapon [unit + 0x10 + idx * 0x1C]
   (0x439D51) and the weapon's reload word +0xE4, and divides progress * 100 by it (0x439D65
   `idiv esi`). Nothing bounds the index to the three inline slots, and nothing tests the
   divisor: an unarmed slot holds &Weapons[0], whose +0xE4 is 0, and so does a stockpile weapon
   with no reloadtime. No stock weapon reaches it (every stockpile weapon's reloadtime is
   120-180); a mod, or a remote unit whose type differs on this peer, does. Side slots past the
   third, which the extra-weapons module keeps in its own table, are not inline slots, and the
   engine's bar has no reader for them.
   THE FIX: a jmp at 0x439D41 (`mov ecx,[eax+0x36]; mov eax,[eax+0x3E]`) to a stub that takes
   0x439D6B, the function's own `return 0`, for an index above 2, a NULL weapon or a zero
   +0xE4, and otherwise repeats the two loads and rejoins at 0x439D47 with ecx the index and eax
   the progress. esi, pushed at 0x439D24, is free until 0x439D47 rewrites it, and 0x439D6B
   pops it.
   THE INVARIANT: the divide at 0x439D65 sees a non-zero divisor read from an inline slot.
   CLASS: local. A HUD draw. */
static int fix_stockpile_bar(void)
{
    FIXSITE site = { 0x00439D41, 6, { 0x8B, 0x48, 0x36, 0x8B, 0x40, 0x3E }, { 0 } };
    static const unsigned char stub[38] = {
        0x8B, 0x48, 0x36,                               /* mov ecx,[eax+0x36]       */
        0x83, 0xF9, 0x02,                               /* cmp ecx,2                */
        0x77, 0x23,                                     /* ja zero                  */
        0x8D, 0x34, 0xCD, 0x00, 0x00, 0x00, 0x00,       /* lea esi,[ecx*8]          */
        0x2B, 0xF1,                                     /* sub esi,ecx: idx * 7     */
        0x8B, 0x74, 0xB2, 0x10,                         /* mov esi,[edx+esi*4+0x10] */
        0x85, 0xF6,                                     /* test esi,esi             */
        0x74, 0x12,                                     /* jz zero                  */
        0x66, 0x83, 0xBE, 0xE4, 0x00, 0x00, 0x00, 0x00, /* cmp word [esi+0xE4],0    */
        0x74, 0x08,                                     /* je zero                  */
        0x8B, 0x40, 0x3E,                               /* mov eax,[eax+0x3E]       */
    };                                                  /* jmp 0x439D47; zero: jmp 0x439D6B */
    unsigned char* a;
    if (!fix_match(&site, 1)) return FIX_BYTES;
    if (!(a = fix_code(sizeof stub + 10))) return FIX_STUB;
    memcpy(a, stub, sizeof stub);
    a[sizeof stub] = 0xE9;      tagpu_detour_rel(a + sizeof stub + 1, 0x00439D47);
    a[sizeof stub + 5] = 0xE9;  tagpu_detour_rel(a + sizeof stub + 6, 0x00439D6B);
    fix_branch(&site, 0xE9, a);
    return fix_write(&site, 1);
}

SIM_FEATURE(range_circle, SIM_PUPPET_RUNS, "battle");

/* A RANGE CIRCLE OF RADIUS 1 [DISASSEMBLED]. DrawRangeCircle 0x438EA0 returns at once for
   radius 0 (0x438EAF), takes N = (int)(r * 2pi * 0.125) segments (the doubles 0x4FD2B0 and
   0x4FD2B8, ftol 0x4E43A0), and divides 0x10000 by N at 0x438EEE. Its guard at 0x438EDE, `jl
   0x43904D` after `cmp ecx,ebp` (ebp 0), catches only a negative N, and radius 1 gives N = 0: a
   divide by zero. No stock unit has a range that small; a mod does. TADR's `jle` sends N = 0
   to 0x43904D, which draws the circle's label at the screen's corner (0, 4).
   THE FIX: the six-byte jl becomes a jmp to `jl 0x43904D; je 0x43908F; jmp 0x438EE4`, on the
   flags of 0x438ED8's compare (the mov at 0x438EDA leaves them). N = 0 goes to 0x43908F, the
   epilogue radius 0 takes, with the same stack; a negative N keeps stock's path.
   THE INVARIANT: 0x438EEE divides by N >= 1.
   CLASS: local. A HUD draw. */
static int fix_range_circle(void)
{
    FIXSITE site = { 0x00438EDE, 6, { 0x0F, 0x8C, 0x69, 0x01, 0x00, 0x00 }, { 0 } };
    unsigned char* a;
    if (!fix_match(&site, 1)) return FIX_BYTES;
    if (!(a = fix_code(17))) return FIX_STUB;
    a[0] = 0x0F; a[1] = 0x8C;  tagpu_detour_rel(a + 2, 0x0043904D);    /* jl: stock's path  */
    a[6] = 0x0F; a[7] = 0x84;  tagpu_detour_rel(a + 8, 0x0043908F);    /* je: no circle     */
    a[12] = 0xE9;              tagpu_detour_rel(a + 13, 0x00438EE4);
    fix_branch(&site, 0xE9, a);
    return fix_write(&site, 1);
}

SIM_FEATURE(order_anyone, SIM_PUPPET_RUNS, "battle");

/* AN ORDER MODE WITH NOBODY TO ORDER [DISASSEMBLED; MEASURED]. The order byte main+0x2CC3 is 1
   when no order is armed. A build button arms a placement, 0x0E, with BuildUnitID main+0x2CC4
   (0x41AB89, 0x41AB9C, in the build menus' handler 0x41AA00). The orders menu's handler arms a
   command mode, each from its gadget: MOVE 2 (0x419C6A), ATTACK 3 (0x419D44), BLAST 4
   (0x419DB9), UNLOAD 5 (0x41A066), LOAD 6 (0x41A0D7), DEFEND 7 (0x419E2D), REPAIR 8 (0x419EA1),
   PATROL 9 (0x419F16), RECLAIM 0xC (0x419F8A), CAPTURE 0xD (0x419FFB). Nothing else writes those
   values (0xA and 0xB have no writer; the setter 0x419BC0 has no caller), and every one of them
   is an order for the selection: none stays armed with nothing selected on purpose.
   A left click hands the order to the controlled player's block (record main+0x1B63 + 0x14B *
   main+0x2A42, units +0x67 .. +0x6B inclusive). A placement's click, 0x419670, orders each unit
   selected (+0x110 bit 0x10) whose type (+0x92) has +0x241 bit 0x40 (0x419755..0x41976A). A
   command mode's click, 0x48CF30, counts the selected units other than the one under the pointer
   and returns when there are none (0x48CFDF..0x48D011), then asks the order resolver 0x43F0E0 per
   unit with the click's target and position (0x48D0A0). Stock leaves the byte armed when the set
   empties: the selection dies (the frame check 0x4995C3 drops the tracked unit and pops its menu
   through 0x491D70(0), which writes neither byte), a key recalls a group without such a unit
   (0x495E90's one writer of the byte is its cancel 0x495F36), or a button arms from the menu
   0x491D70 left up because it deferred the drop (0x37EBE & 0x865 or 0x2BEE & 0xE0:
   0x491D86..0x491DA2 only sets 0x37EBE bit 0x10). Every left press then goes to the order
   (0x4993B6 -> 0x498F70) and orders nobody: a placement keeps its square and our build ghost on
   the pointer, and a command mode swallows the click.
   THE FIX: order_check, at two points of the game thread's frame, disarms through the engine's
   own cancel when order_anyone finds nobody for the armed order.
   THE INVARIANT: an order is armed only while at least one unit exists that its click would
   order. For a placement that is 0x419670's own test. For a command mode it is 0x48CF30's first
   test, a selected unit in the block; the resolver's answer depends on the click, so a
   selection whose units cannot carry the mode (ATTACK with only builders left) keeps stock's
   behaviour. Every value read is bounded first.
   WHY TWO POINTS AND NOT ONE: the readers of the byte sit on both sides of the ticks. The
   in-play handler 0x499200 (state 6, entered from IdleTick 0x499890 at 0x499A1C) handles the
   mouse first -- the build cursor 0x4197D0 (0x499241), the cursor choice 0x48D220 (0x499297),
   the click routing 0x4993B6 and the order's click 0x498F70 (0x4995B3) -- then calls the frame
   0x496790 (0x4995B8), whose ticks (0x495490 at 0x49680C, 0x49693E) free units, whose keys
   (0x495E90 at 0x496971) recall groups, and whose draw 0x468CF0 (0x4969CD) reads the byte and
   publishes our packet. IdleTick runs the GUI's dispatch 0x4A9FD0 (0x499992), which reaches the
   menus' buttons, before it enters the handler. So no one point follows every writer and
   precedes every reader:
     0x49697B, the frame's `call 0x48BAE0`: every path of 0x496790 reaches it (0x49691E and
       0x49696F jump to it; nothing leaves the function before 0x496A56), after the ticks, the
       keys and the scroll poll 0x41CE90, and before the cull and the draw.
     0x499226, the handler's first instruction after 0x498DA0 (the mouse's world position):
       after IdleTick's GUI dispatch and before the head's first reader (0x499238); every path
       of the head reaches 0x4995B8 (it holds no ret).
   Both run on the game thread, where the key cancel 0x495F36 and the right-button cancel
   (0x4993AC -> 0x499100) already run, inside the same two functions.
   NO MENU, NO CANCEL: the frame 0x496790 has three callers -- 0x4995B8 (in play), 0x49842F (the
   loading screen, after the load's reset wrote the byte to 1 at 0x4917F9) and 0x4996A5 (a
   network game's end, after 0x491D70(1) at 0x499674 and GUI_Pop 0x4A9660 at 0x499686 have
   popped the menu stack, possibly to NULL). 0x499100 reads [[main+0x531]+4] at 0x49913B with no
   test, so order_check returns while main+0x531 is NULL, the guard the engine puts on the same
   read at 0x491DB3..0x491DBB. The test and the call are on one thread with nothing between.
   UNDER A MODAL SCREEN, LATER: the engine defers its own menu work on one test, 0x37EBE & 0x865
   or 0x2BEE & 0xE0 -- 0x491D70(0) only marks the drop pending (0x491D76..0x491DA2) and the frame
   runs it once the test clears (0x496986..0x4969B4) -- and order_check waits on the same test.
   Its bits that are named: bit 0 is the options stack (ARMOPT, EXITMENU, YESORNO and the
   preferences; set at 0x4961C1, 0x49477E, 0x45D002), bit 2 is the chat, TALK.GUI (pushed by
   0x494050 from the Enter key's case 0x4964FD, the bit at 0x49412C), bit 6 is SHARE.GUI
   (0x49374F); bits 5 and 11 of the word and the byte's three are not identified. Under such a
   screen the cancel's 0x49FE60(top, "STOP") searches the modal screen and misses, which would
   leave the pressed order button drawn with the byte 1; deferred, the disarm lands on the
   first of the two checks after the test clears. While deferred the byte stays armed as in
   stock. A single-player game skips the ticks while bit 0 is set (0x496918 jumps to 0x49697B),
   so no unit dies under the options stack there; a network game (main+0x2A44 bit 0 [INFERRED])
   keeps ticking (0x4967E7), and so does the chat -- MEASURED 2026-09-26 with
   tools/b9-under-chat.sh: a unit died under TALK.GUI, the byte stayed armed until the chat
   closed, then read 1.
   THE CANCEL: 0x499100 with the byte != 1 is the right button's cancel for every order, and
   reads nothing of its message: 0x2CC3 = 1, 0x2CC6 &= ~0x20 (the order kept while a modifier is
   held), then 0x49FE60(menu, "STOP") finds the menu's STOP gadget and 0x4A6A40 resets its radio
   group, which releases the pressed order button, as 0x4990AE, 0x4992FA, 0x495F36 and 0x498FC0
   do. It leaves BuildUnitID, as every cancel does; its readers (0x419686, 0x4197DD, our packet)
   are behind the byte.
   CLASS: local. Every reader of 0x2CC3 is the UI, and no peer reads it. The disarm logs nothing:
   a unit dying with an order armed is ordinary play. */
static const unsigned int s_orderMsg[6];        /* 0x499100's message: its cancel never reads it */

/* The controlled player's units, first..last inclusive, both on the unit array's own slots;
   0 when the record is not a player's or the range is not the array's. */
static int player_units(const unsigned char* m, unsigned int* first, unsigned int* last)
{
    unsigned int player = m[0x2A42];                  /* the controlled player */
    const unsigned char* rec;
    unsigned int begin, end;
    if (player >= 10) return 0;
    rec    = m + 0x1B63 + player * 0x14Bu;
    begin  = *(const unsigned int*)(m + 0x14357);
    end    = *(const unsigned int*)(m + 0x1435B);     /* the array's last slot, inclusive */
    *first = *(const unsigned int*)(rec + 0x67);
    *last  = *(const unsigned int*)(rec + 0x6B);
    return !(*first < begin || *last > end || *first > *last ||
             (*first - begin) % 0x118u || (*last - begin) % 0x118u);
}

/* The UnitDef a unit's +0x92 names, or NULL unless it is a slot of the def array. */
static const unsigned char* unit_def(const unsigned char* m, const unsigned char* unit)
{
    unsigned int defs  = *(const unsigned int*)(m + 0x1439B);
    unsigned int ndefs = *(const unsigned int*)(m + 0x1438F);      /* UNITINFOCount */
    unsigned int def   = *(const unsigned int*)(unit + 0x92);
    if (def < defs || (def - defs) % 0x249u || (def - defs) / 0x249u >= ndefs) return NULL;
    return (const unsigned char*)(size_t)def;
}

/* 1 when a unit of the controlled player's block would take the armed order; builders_only is
   the placement's type test */
static int order_anyone(const unsigned char* m, int builders_only)
{
    unsigned int first, last, u;
    if (!player_units(m, &first, &last)) return 0;
    for (u = first; u <= last; u += 0x118u) {
        const unsigned char* unit = (const unsigned char*)(size_t)u;
        const unsigned char* def;
        if (!(unit[0x110] & 0x10)) continue;
        if (!builders_only) return 1;
        def = unit_def(m, unit);
        if (def && (def[0x241] & 0x40)) return 1;
    }
    return 0;
}

static void __cdecl order_check(void)
{
    const unsigned char* m = *(unsigned char* const*)0x00511DE8;
    unsigned char mode;
    if (!m || !*(void* const*)(m + 0x531)) return;                  /* no menu to cancel in  */
    if ((*(const unsigned short*)(m + 0x37EBE) & 0x865) || (m[0x2BEE] & 0xE0))
        return;                                                     /* a modal screen on top */
    mode = m[0x2CC3];
    if (mode == 0x0E) {
        if (order_anyone(m, 1)) return;
    } else if ((mode >= 2 && mode <= 9) || mode == 0x0C || mode == 0x0D) {
        if (order_anyone(m, 0)) return;
    } else {
        return;
    }
    ((void (__stdcall*)(const void*))0x00499100)(s_orderMsg);
}

static int fix_order_anyone(void)
{
    FIXSITE site[2] = {
        { 0x0049697B, 5, { 0xE8, 0x60, 0x51, 0xFF, 0xFF }, { 0 } },         /* call 0x48BAE0  */
        { 0x00499226, 6, { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 }, { 0 } },   /* mov edx,[main] */
    };
    unsigned char *a, *b, *p;
    if (!fix_match(site, 2)) return FIX_BYTES;
    if (!(a = fix_code(12)) || !(b = fix_code(18))) return FIX_STUB;
    p = a;                                          /* pushad; call; popad; jmp 0x48BAE0 */
    *p++ = 0x60;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)order_check); p += 4;
    *p++ = 0x61;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0048BAE0);   /* whose ret comes back to 0x496980 */
    p = b;                                          /* pushad; call; popad; the stolen mov; jmp */
    *p++ = 0x60;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)order_check); p += 4;
    *p++ = 0x61;
    memcpy(p, site[1].was, 6); p += 6;
    *p++ = 0xE9; tagpu_detour_rel(p, 0x0049922C);
    fix_branch(&site[0], 0xE8, a);
    fix_branch(&site[1], 0xE9, b);
    return fix_write(site, 2);
}

SIM_FEATURE(queued_site, SIM_PUPPET_RUNS, "battle");

/* A QUEUED SITE IS TAKEN GROUND. [DISASSEMBLED] The build cursor 0x4197D0 asks the site test
   0x47D2E0 (stdcall (def, cell, 0, player), ret 0x10; the call is 0x4198C6) whether the
   footprint fits, and shows the answer as main+0x2CC6 bit 6: green, or red -- and a click on red
   plays "notok" and orders nothing (0x498F86, 0x499016). The test refuses a cell whose yardmap
   byte has bit 2 or 4 and whose unit word (+0x00) already names another unit (0x47D54C..0x47D561).
   That word is written by the stamp 0x47CC30 of a unit that EXISTS, into the cells whose yardmap
   byte has bit 4 (0x47CD8E..0x47CD9D; bit 2 instead when +0x10F has bit 2). A site queued and not
   yet built stamps nothing, so a second site over it reads green, is ordered, and its order fails
   when the first is built.

   THE FIX asks the same question of the queue. build_site_test runs stock's test and, when it
   says yes, says no if the footprint shares a cell with a queued site of the controlled player's
   units where one yardmap has bit 4 and the other bit 2 or 4 -- the stamp's and the test's own
   bits, in both directions because either site may be built first. A QUEUED SITE is an order whose
   descriptor (*0x512344, stride 0x19) has capability bit 0, the one 0x438C00 draws a rectangle
   for, with a build type (+0x36) and a ground position (+0x22, +0x2A above 0: a factory's count
   node is made with a NULL position, which 0x43A0C0 zeroes). Its cell is 0x4197D0's own formula
   over the node's position and its type's footprint (def +0x14A, +0x14C).
   What the click does with the selected builders' own orders is left out of the test, as the
   click would leave it: without Shift 0x43ADC0 destroys a unit's orders that lack flag 4
   (+0x42), so those do not block; with Shift 0x43AFC0 cancels a unit's order within 16 world
   units of the new position (0x43B00F..0x43B029), so that one does not block, and Shift-clicking
   a queued site still removes it. A unit is one the click orders when it is selected (+0x110
   bit 4) and its def has +0x241 bit 6, 0x419755..0x41976A's test. Allies' and enemies' plans are
   not looked at, nor are units that are not alive (+0x110 bit 28) or are excluded (bit 14): the
   engine's own marker driver's set.

   THE INVARIANT: the queue's test only ever turns a yes into a no, so it cannot order what stock
   refuses, and it changes what the cursor shows, never what a peer simulates -- CLASS: local. The
   AI's calls of the site test (0x40A4CD) and orders that do not come from the cursor are stock's.

   THE YES IT IS GIVEN is build scatter's, not stock's alone (tagpu_scatter.h, THE CURSOR'S SITE
   TEST): a site stock refuses only for units of the placing player that the builder's arrival
   would move off it is accepted, and the queue's test is then asked of it as of any other. The
   cursor's own call, the one that returns to 0x4198CB, tells the frame packet whether its answer
   was such a site, for the square's colour.
   Every read is bounded first: the units against the array's own slots (player_units), each
   def index against UNITINFOCount (unit_def, qs_def), a node's type byte against the
   descriptor array's length, a yardmap only for a def the parser gave one (BMcode +0x22F = 0,
   0x42CF30) and only within its footprint. A node is read on the game thread, which is the one
   thread that links, unlinks and frees order nodes (the order code runs in unit ticks and click
   handlers), from the build cursor, which runs between those; the list is walked at most
   QS_NODES nodes a unit. Identity on a game with no queued site. */
#define QS_NODES 65536u

typedef unsigned (__stdcall *site_test_fn)(const void* def, unsigned cell, unsigned zero, void* player);
typedef int (__stdcall *key_query_fn)(int key);

/* a def's footprint in cells and its yardmap, or 0 when it has none to read */
static int qs_footprint(const unsigned char* d, const unsigned char** yard, int* fx, int* fz)
{
    if (!ptr_sane(d) || d[0x22F]) return 0;
    *fx   = *(const short*)(d + 0x14A);
    *fz   = *(const short*)(d + 0x14C);
    *yard = *(const unsigned char* const*)(d + 0x14E);
    return *fx > 0 && *fz > 0 && ptr_sane(*yard);
}

/* The footprint's first cell, 0x4197D0's rounding of a 16.16 centre (an arithmetic shift, as its sar) */
static int qs_origin(int centre, int foot)
{
    return (int)((unsigned)centre - ((unsigned)foot << 19) + 0x80000u) >> 20;
}

/* 1 when a cell of both rectangles is one a built unit of one would stamp and the other's test refuses */
static int qs_clash(int ax, int az, int afx, int afz, const unsigned char* ay,
                    int bx, int bz, int bfx, int bfz, const unsigned char* by)
{
    int x0 = ax > bx ? ax : bx, x1 = ax + afx < bx + bfx ? ax + afx : bx + bfx;
    int z0 = az > bz ? az : bz, z1 = az + afz < bz + bfz ? az + afz : bz + bfz;
    int x, z;
    for (z = z0; z < z1; z++)
        for (x = x0; x < x1; x++) {
            unsigned char a = ay[(z - az) * afx + (x - ax)];
            unsigned char b = by[(z - bz) * bfx + (x - bx)];
            if (((a & 4) && (b & 6)) || ((b & 4) && (a & 6))) return 1;
        }
    return 0;
}

static const unsigned char* qs_desc(unsigned type)
{
    const unsigned char* beg = *(const unsigned char* const*)0x00512344;
    const unsigned char* end = *(const unsigned char* const*)0x00512348;
    if (!ptr_sane(beg) || !ptr_sane(end) || end <= beg) return NULL;
    return type < (size_t)(end - beg) / 0x19u ? beg + (size_t)type * 0x19u : NULL;
}

static int qs_taken(const unsigned char* m, const unsigned char* def, unsigned cell)
{
    const unsigned char* yard;
    int fx, fz, cx, cz, shift;
    unsigned int first, last, u, defs, ndefs;
    int nx, nz;
    if (!qs_footprint(def, &yard, &fx, &fz) || !player_units(m, &first, &last)) return 0;
    cx = (short)(cell & 0xFFFFu);
    cz = (short)(cell >> 16);
    nx = (int)((unsigned)(2 * cx + fx) << 19);          /* the order's position, 0x419670 */
    nz = (int)((unsigned)(2 * cz + fz) << 19);
    shift = ((key_query_fn)0x004C1B80)(0xF9) != 0;
    defs  = *(const unsigned int*)(m + 0x1439B);
    ndefs = *(const unsigned int*)(m + 0x1438F);
    for (u = first; u <= last; u += 0x118u) {
        const unsigned char* unit = (const unsigned char*)(size_t)u;
        const unsigned char* udef;
        const unsigned char* node;
        unsigned int state = *(const unsigned int*)(unit + 0x110);
        unsigned int n;
        int ordered;
        if (!(state & 0x10000000u) || (state & 0x4000u)) continue;
        udef    = unit_def(m, unit);
        ordered = (state & 0x10u) && udef && (udef[0x241] & 0x40);
        node    = *(const unsigned char* const*)(unit + 0x5C);
        for (n = 0; n < QS_NODES && ptr_sane(node); n++, node = *(const unsigned char* const*)(node + 0x4A)) {
            const unsigned char* desc = qs_desc(node[4]);
            unsigned type = *(const unsigned short*)(node + 0x36);
            int x = *(const int*)(node + 0x22), z = *(const int*)(node + 0x2A);
            const unsigned char* oyard;
            int ofx, ofz;
            if (!desc || !(*(const unsigned int*)(desc + 0x0C) & 1) || !type || x <= 0 || z <= 0)
                continue;
            if (ordered && !shift && !(*(const unsigned int*)(node + 0x42) & 4)) continue;
            if (ordered && shift && ((unsigned)x - (unsigned)nx + 0x100000u) <= 0x200000u &&
                ((unsigned)z - (unsigned)nz + 0x100000u) <= 0x200000u) continue;
            if (type >= ndefs || !defs) continue;
            if (!qs_footprint((const unsigned char*)(size_t)defs + (size_t)type * 0x249u,
                              &oyard, &ofx, &ofz)) continue;
            if (qs_clash(cx, cz, fx, fz, yard, qs_origin(x, ofx), qs_origin(z, ofz), ofx, ofz, oyard))
                return 1;
        }
    }
    return 0;
}

static unsigned __stdcall build_site_test(const void* def, unsigned cell, unsigned zero, void* player)
{
    int moves = 0;
    unsigned ok = tagpu_scatter_site_test(def, cell, zero, player, &moves);
    const unsigned char* m = *(const unsigned char* const*)0x00511DE8;
    if ((ok & 1) && m && qs_taken(m, (const unsigned char*)def, cell)) ok = 0;
    if (__builtin_return_address(0) == (void*)0x004198CBu) tagpu_scatter_cursor_set(moves && (ok & 1));
    return ok;
}

static int fix_queued_site(void)
{
    FIXSITE site = { 0x004198C6, 5, { 0xE8, 0x15, 0x3A, 0x06, 0x00 }, { 0 } };  /* call 0x47D2E0 */
    if (!fix_match(&site, 1)) return FIX_BYTES;
    fix_branch(&site, 0xE8, (const void*)build_site_test);
    return fix_write(&site, 1);
}

SIM_FEATURE(loader_defects, SIM_PUPPET_RUNS, "battle");

static void patch_loader_defects(void)
{
    int wind = fix_wind();
    int yard = fix_yardmap();
    int save = fix_save_order_fallback();
    int bar  = fix_stockpile_bar();
    int ring = fix_range_circle();
    int plc  = fix_order_anyone();
    int qsit = fix_queued_site();
    int seed = fix_test_seeds();
    int mstr = fix_meteor_stream();
    int prnd = fix_particle_rand();
    int pdiv = fix_particle_divides();
    tagpu_logf("enginefix: the particles draw by their own rule, not the CRT's rand (0x470EB0 "
               "0x4918B1 and twenty draws 0x424345..0x475679) %s; a flare or a wake whose divisor "
               "is zero makes no particle (0x473788 0x47480A) %s. Counters: this level's values "
               "drawn by the rule, emissions and objects outside the pool at 0x%08X 0x%08X 0x%08X",
               fix_state(prnd), fix_state(pdiv), (unsigned int)(size_t)&s_prand.draws,
               (unsigned int)(size_t)&s_prand.emissions, (unsigned int)(size_t)&s_prand.outside);
    tagpu_logf("enginefix: a harness's levels start from fixed random states (0x49719D "
               "0x4971AE 0x4982CA) %s; this launch %s", fix_state(seed),
               s_testSeeds ? "has tagpu_seeds.on: fixed seeds" : "keeps stock's seeds");
    if (mstr >= 0)
        tagpu_logf("enginefix: under tagpu_seeds.on the meteor shower draws its own stream, not the "
                   "CRT's the drawn frames draw (0x437E37 0x437E57 0x437E88 0x437EAA 0x437F98 "
                   "0x437FB5 0x4380AD 0x4380CE 0x438100 0x438124) %s", fix_state(mstr));
    tagpu_logf("enginefix: one wind for every peer (0x490C5A 0x491903 0x4982CA 0x4C98FD) %s; "
               "yardmaps parsed inside their string (0x42CF5E) %s; the saved-game order fallback "
               "(0x43A58D) %s; the stockpile bar's divide (0x439D41) %s; a range circle of "
               "radius 1 (0x438EDE) %s; an order mode disarmed with nobody to order "
               "(0x49697B 0x499226) %s; a build site refused over another queued site (0x4198C6) %s. Counters: levels seeded at 0x%08X, network levels "
               "seeded from the engine's session copy at 0x%08X, SetSessionDesc calls made, "
               "made over another GUID and withheld at 0x%08X 0x%08X 0x%08X, network levels "
               "seeded from the map alone at 0x%08X, yardmaps filled past their string at 0x%08X",
               fix_state(wind), fix_state(yard), fix_state(save), fix_state(bar),
               fix_state(ring), fix_state(plc), fix_state(qsit), (unsigned int)(size_t)&s_windDiag.levels,
               (unsigned int)(size_t)&s_windDiag.fromCopy, (unsigned int)(size_t)&s_windDiag.setCalls,
               (unsigned int)(size_t)&s_windDiag.setDiffered, (unsigned int)(size_t)&s_windDiag.setWithheld,
               (unsigned int)(size_t)&s_windDiag.mapOnly, (unsigned int)(size_t)&s_yardDiag.filled);
}

SIM_FEATURE(sight_cap, SIM_PUPPET_RUNS, "battle");

/* THE TEMPORARY SIGHT RECORDS' CAP, A TEST LEVER: `tagpu_sightcap.on` holding N, 21..127, raises it
   from 20 to N, in a test launch only (tagpu_regstore_active, the turbo's gate); anywhere else, or
   with another text, the lever is refused with a line and nothing changes. The players' game never
   runs it. It is the packed calibration lane's (research/notes/modules/tasim-calibration.md, *The
   packed lane*): the records are one list for the whole game (engine/map.md, *The temporary sight
   records*), so a level holding many fights at once shares one cap that a fight alone never
   reaches, and a death the cap refuses lights nothing for 61 ticks. Raised past every batch's
   viewed-seat units, no add is refused, as in an unpacked run.
   THE SITES (DISASSEMBLED; a .text-wide search finds no other 0x2D0 and no other compare with 20
   on the count main+0x14277 or the block main+0x1427B, through any base):
     0x481505  68 D0 02 00 00   push 0x2D0, the block of 20 x 36 bytes in 0x481500, which nothing
                                calls -> push N x 36
     0x483D88  68 D0 02 00 00   push 0x2D0, LoadMap's block, the one a level allocates
                                (0x483D98 calls the allocator 0x4D83B0) -> push N x 36
     0x48292C  83 F8 14         cmp eax,0x14, the add 0x482910's cap -> cmp eax,N
   N is at most 127 because the compare's imm8 is sign-extended. Every other reader of the count
   and the block works from the count alone: the expiry 0x482130 walks count records and compacts
   them, and the add fills record `count`. The three go into the fail-closed table, so a lever on an
   exe whose bytes differ there ends the process at the report rather than playing at 20.
   Impure's readers of the list take the cap in force from tagpu_sightcap: a seek's and a sight
   change's drop (tagpu_replay.c, sight_records_bounded and sight_records_drop) bound the count by
   it, so every record the engine can hold is dropped and the reads stay inside the block. A
   recording made under the lever carries its N (RT_SIGHTCAP), and a playing refuses a recording
   whose cap is not its own (tagpu_replay.c, validate_header). */
static unsigned int s_sightCap;          /* N, or 0 when the lever is not in force */
SIM_VAR(s_sightCap, SIM_INSTALL);

unsigned int tagpu_sightcap(void)
{
    return s_sightCap ? s_sightCap : TAGPU_SIGHTCAP_STOCK;
}

static int fix_sight_cap(void)
{
    char b[16], *end;
    unsigned long n;
    unsigned int bytes;
    size_t len;
    FIXSITE site[3] = {
        { 0x00481505, 5, { 0x68, 0xD0, 0x02, 0x00, 0x00 }, { 0x68 } },
        { 0x00483D88, 5, { 0x68, 0xD0, 0x02, 0x00, 0x00 }, { 0x68 } },
        { 0x0048292C, 3, { 0x83, 0xF8, 0x14 },             { 0x83, 0xF8 } },
    };

    if (tagpu_sim_lever_read("tagpu_sightcap.on", b, sizeof b) < 0) return -1;
    /* the file's own line end goes, so the refusal below quotes the text on one log line */
    len = strlen(b);
    while (len && (b[len - 1] == ' ' || b[len - 1] == '\t' || b[len - 1] == '\r' || b[len - 1] == '\n'))
        b[--len] = '\0';
    n = strtoul(b, &end, 10);
    if (!tagpu_regstore_active()) {
        plog("sightcap: tagpu_sightcap.on refused: not a test launch (tagpu_regstore.h)");
        return -1;
    }
    if (end == b || *end || n < 21 || n > 127) {
        tagpu_logf("sightcap: tagpu_sightcap.on refused: \"%s\" is not a cap of 21 to 127", b);
        return -1;
    }
    s_sightCap = (unsigned int)n;
    bytes = s_sightCap * 36u;
    memcpy(site[0].now + 1, &bytes, 4);
    memcpy(site[1].now + 1, &bytes, 4);
    site[2].now[2] = (unsigned char)s_sightCap;
    return fix_table(site, 3, "temporary sight record cap, test lever");
}

static void patch_sight_cap(void)
{
    int r = fix_sight_cap();
    if (r >= 0)
        tagpu_logf("sightcap: TEST LEVER tagpu_sightcap.on -- the temporary sight records' cap %u "
                   "(0x481505 0x483D88 0x48292C) %s", s_sightCap, fix_state(r));
}

SIM_FEATURE(duel_lever, SIM_PUPPET_RUNS, "battle");

/* THE DUEL LEVER, A TEST LEVER: `tagpu_impai_duel.on`, in a test launch only (tagpu_regstore_active),
   holds off the network game's end for want of a human, so that two builds of ImpAI play a network
   game with no human on the map and the harness ends it (tools/impai/codeduel.py --no-humans;
   research/notes/modules/duel-lever.md). The players' game never runs it: with no lever, or outside
   a test launch, nothing is written at the site.
   THE SITE (DISASSEMBLED; engine/network.md, *The network game's end test*). After its loop over the
   ten records the players phase 0x464F80 tests, on every call:
       004655C0  call 0x435100                   GameingState's mode
       004655C5  cmp eax,3 / jne 0x46563B        a network game
       004655CA  mov ecx,[0x511DE8]
       004655D0  cmp dword [ecx+0x37EF6],2       ActiveCommanderDeath
       004655D7  je  0x46563B                    not Deathmatch
       004655D9  call 0x457CB0                   the humans still in the game
       004655DE  test eax,eax
       004655E0  jne 0x46563B                    75 59: some, so return
       004655E2  ...                             none: the countdown main+0x39239, then stop and lost
   0x457CB0 never counts a computer seat, so stock ends the level once no human holds a unit. The
   lever writes the jne's opcode, 75 -> EB: the branch to the phase's return is always taken, and
   0x4655E2..0x46563A, which no jump from outside enters, never runs. The count's other call,
   0x4657E8 (whether a player who is out sees the watch notice), keeps its answer.
   WHY IT CANNOT RUN HALF-WRITTEN: one byte is written, the opcode of a two-byte instruction whose
   displacement stays, so at every moment the site holds a whole `jne 0x46563B` or a whole
   `jmp 0x46563B`; and it is written from DllMain, before TotalA.exe's entry point (THE FAIL-CLOSED
   SITE TABLE, above), when no thread runs the players phase.
   The whole test, 0x4655C5..0x4655E1, is byte-matched first: on any other bytes nothing is written,
   the line says so, and the game plays as that exe ships it. */
#define VA_DUEL_TEST 0x004655C5u
#define VA_DUEL_JNE  0x004655E0u

static void patch_duel_lever(void)
{
    static const unsigned char test[29] = {
        0x83, 0xF8, 0x03, 0x75, 0x71, 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00, 0x83, 0xB9, 0xF6, 0x7E,
        0x03, 0x00, 0x02, 0x74, 0x62, 0xE8, 0xD2, 0x26, 0xFF, 0xFF, 0x85, 0xC0, 0x75, 0x59 };
    static const unsigned char jmp = 0xEB;

    if (!tagpu_sim_lever("tagpu_impai_duel.on")) return;
    if (!tagpu_regstore_active()) {
        plog("duel: tagpu_impai_duel.on refused: not a test launch (tagpu_regstore.h)");
        return;
    }
    if (memcmp((const void*)(size_t)VA_DUEL_TEST, test, sizeof test) != 0) {
        plog("duel: tagpu_impai_duel.on NOT installed: 0x4655C5..0x4655E1 does not hold the retail "
             "exe's network end test; the game ends as this exe ends it");
        return;
    }
    if (!tagpu_detour_write(VA_DUEL_JNE, &jmp, 1)) {
        plog("duel: tagpu_impai_duel.on NOT installed: 0x4655E0 could not be written");
        return;
    }
    plog("duel: TEST LEVER tagpu_impai_duel.on -- installed: a network game no longer ends for want "
         "of a human (0x4655E0 jne -> jmp)");
}

SIM_FEATURE(apply_patches, SIM_PUPPET_RUNS, "battle");

static void rot_install(void);
static void hyp_install(void);

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
    if (tagpu_sim_lever("tagpu_curs.off")) {
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
    patch_loader_defects();
    patch_sight_cap();
    patch_duel_lever();
    /* After the engine fixes: repose_trig byte-matches stock's 0x4B7173 before it writes, and the
       rotation's jmp would make it skip itself. */
    rot_install();
    hyp_install();
}

SIM_FEATURE(limits_pools, SIM_PUPPET_RUNS, "battle");

/* ===== THE RAISED LIMITS (tagpu_limits.h) ===================================================
   The engine limits TADR raises -- the effect pools as its EngineLimits.cpp does, the unit
   limit and the pathfinding budget as its LimitCrack.cpp does -- re-derived from the
   pristine 3.1 image (research/notes/tadr-port/limits-evidence.md §1-6 holds every site's
   disassembly). Every site goes into the fail-closed table (top of this file), with the
   simulation fixes' sites, and is compared and written with them. */

/* One debris record initialised free, with the vertex and face blocks it owns, as the level
   load's loop 0x4207FB..0x4208F9 initialises each of the engine's 300 (DISASSEMBLED: +0 = 0xFF,
   +4 = 8, +8 = 6, +0xC = -1, +0x1C = 0x502C28, +0x20 = 0, +0x24 the record's 0x60-byte vertex
   block, +0x28 its 0xC0-byte face block; each of its 6 faces of 0x20 gets +0x1C bit 0, +0 = 0xC8,
   +4 = 4, +0xC = 0x502BF8 + 8 * the face). Both builds, over the records the build uses, at every
   level load and every seek (fx_pools_empty). */
static void debris_init(unsigned char* r, unsigned char* vertices, unsigned char* faces)
{
    unsigned int v, k;
    r[0] = 0xFF;
    v = 8;           memcpy(r + 0x04, &v, 4);
    v = 6;           memcpy(r + 0x08, &v, 4);
    v = 0xFFFFFFFFu; memcpy(r + 0x0C, &v, 4);
    v = 0x00502C28u; memcpy(r + 0x1C, &v, 4);
    v = 0;           memcpy(r + 0x20, &v, 4);
    v = (unsigned int)(size_t)vertices; memcpy(r + 0x24, &v, 4);
    v = (unsigned int)(size_t)faces;    memcpy(r + 0x28, &v, 4);
    for (k = 0; k < 6; k++) {
        unsigned char* f = faces + k * 0x20;
        memcpy(&v, f + 0x1C, 4);
        v |= 1u;                          memcpy(f + 0x1C, &v, 4);
        v = 0xC8;                         memcpy(f + 0x00, &v, 4);
        v = 4;                            memcpy(f + 0x04, &v, 4);
        v = 0x00502BF8u + k * 8u;         memcpy(f + 0x0C, &v, 4);
    }
}


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
SIM_ENGINE(s_expl, SIM_STATE);
static void*         s_psys[TAGPU_LIM_PSYS];
SIM_ENGINE(s_psys, SIM_STATE);
static unsigned char s_aux[TAGPU_LIM_AUX][AUX_REC];
SIM_ENGINE(s_aux, SIM_STATE);
static unsigned char s_auxVert[TAGPU_LIM_AUX][AUX_VERTS];
SIM_ENGINE(s_auxVert, SIM_STATE);
static unsigned char s_auxFace[TAGPU_LIM_AUX][AUX_FACES];
SIM_ENGINE(s_auxFace, SIM_STATE);


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
SIM_CODE(s_limCode);
static unsigned int   s_limCodeUsed;
SIM_VAR(s_limCodeUsed, SIM_INSTALL);

static unsigned char* lim_code(unsigned int n)
{
    unsigned char* p;
    if (!s_limCode)
        s_limCode = (unsigned char*)SIM_ALLOC(s_limCode, 0x1000);
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

SIM_FEATURE(cob_guard, SIM_PUPPET_RUNS, "battle resource-core-upgrade cob-getters");

typedef int (__attribute__((thiscall)) *CobNativeGet)(char*, int, int, int, int, int);
static CobNativeGet s_cobNativeGet;
SIM_VAR(s_cobNativeGet, SIM_INSTALL);
static char* cob_live_unit(char* ta, int id);

typedef struct CobFile {
    struct CobFile* next;
    void* blob;
    unsigned size;
    TagpuCobProgram* program;
    char name[128];
} CobFile;
static CobFile* s_cobFiles[256];
SIM_ROWS(s_cobFiles, SIM_CONTENT, SIM_ID_NONE, tagpu_sim_owned);
static CRITICAL_SECTION s_cobLock;
SIM_LOCK(s_cobLock);

typedef struct CobRejected {
    char unit[33], display[33], script[128], reason[256];
    int announced;
} CobRejected;
static CobRejected* s_cobRejected[TAGPU_LIM_TYPES];
SIM_ROWS(s_cobRejected, SIM_STATE, SIM_ID_UNIT_TYPE, tagpu_sim_owned);
static int s_cobInPlay;
SIM_VAR(s_cobInPlay, SIM_STATE);
static DWORD s_cobNextChat;
SIM_VAR_L(s_cobNextChat, SIM_STATE, tagpu_sim_local);

static void __cdecl cob_load_game_data(void)
{
    typedef int (__attribute__((thiscall)) *TdfSelect)(void*, const char*);
    typedef int (__attribute__((thiscall)) *TdfInt)(void*, const char*, int);
    char* ta = *(char**)0x00511DE8u;
    void* save = *(void**)(ta + 0x38D6B);
    TdfSelect select = (TdfSelect)0x004B4560u;
    TdfSelect has = (TdfSelect)0x004B48F0u;
    TdfInt integer = (TdfInt)0x004B4800u;
    if (save && select(save, "Summary") && !has(save, "BetweenMissions")) {
        int limit = integer(save, "maxunits", 0);
        if (limit < TAGPU_LIM_UNITS_MIN || limit > TAGPU_LIM_UNITS)
            tagpu_refuse("Cannot load this saved game: its per-player unit limit is "
                         "missing or outside the supported range. No units have been restored.");
        /* DISASSEMBLED: 0x497581 precedes every per-level allocation in
           0x4917D0. Stock reads this key only at 0x43263B, after the unit
           pool and player ranges exist, so saved IDs can fall outside their
           owner's range (0x486026..0x486030). Use the saved partition before
           allocating it; never remap IDs that COB stacks may also contain. */
        *(unsigned short*)(ta + 0x37EE6) = (unsigned short)limit;
        *(unsigned short*)(ta + 0x37EEC) = (unsigned short)limit;
    }
    ((void (__cdecl*)(void))0x004917D0u)();
}

static void cob_fault(const char* name, unsigned pc, const char* why)
{
    char report[640];
    snprintf(report, sizeof report,
             "A unit script cannot continue safely. The game has stopped.\n\n"
             "Script: %.127s\nWord: %u\nReason: %.255s", name, pc, why);
    tagpu_refuse(report);
    TerminateProcess(GetCurrentProcess(), 3);
}

static int cob_type_rejected(unsigned type)
{
    int rejected;
    if (type >= TAGPU_LIM_TYPES) return 1;
    EnterCriticalSection(&s_cobLock);
    rejected = s_cobRejected[type] != NULL;
    LeaveCriticalSection(&s_cobLock);
    return rejected;
}

/* Called before the loader thread is created, at the existing load reset.
   Indices belong to one level only; no rejection survives its type reindexing. */
static void cob_level_reset(void)
{
    unsigned i;
    EnterCriticalSection(&s_cobLock);
    for (i = 0; i < TAGPU_LIM_TYPES; ++i) {
        SIM_FREE_OWNED(s_cobRejected, s_cobRejected[i]);
        s_cobRejected[i] = NULL;
    }
    s_cobInPlay = 0;
    s_cobNextChat = 0;
    LeaveCriticalSection(&s_cobLock);
}

static void cob_level_enter(void)
{
    EnterCriticalSection(&s_cobLock);
    s_cobInPlay = 1;
    LeaveCriticalSection(&s_cobLock);
}

static void cob_reject_file(const char* script, const char* reason)
{
    char* ta = *(char**)0x00511DE8u;
    char* defs;
    char unit[33];
    unsigned count, i;
    const char* dot = strrchr(script, '.');
    size_t n = dot ? (size_t)(dot - script) : strlen(script);
    if (!ta || n > 32) cob_fault(script, 0, reason);
    memcpy(unit, script, n); unit[n] = 0;
    count = *(unsigned*)(ta + 0x1438F);
    defs = *(char**)(ta + 0x1439B);
    if (!defs || count > TAGPU_LIM_TYPES) cob_fault(script, 0, "unit definition allocation is invalid");
    for (i = 1; i < count; ++i) {
        char* def = defs + i * 0x249u;
        CobRejected* bad;
        if (_strnicmp(def + 32, unit, 32)) continue;
        bad = (CobRejected*)SIM_ALLOC_OWNED(s_cobRejected, sizeof *bad);
        if (!bad) cob_fault(script, 0, "rejection diagnostic allocation failed");
        snprintf(bad->unit, sizeof bad->unit, "%.32s", def + 32);
        snprintf(bad->display, sizeof bad->display, "%.32s", def);
        snprintf(bad->script, sizeof bad->script, "%s", script);
        snprintf(bad->reason, sizeof bad->reason, "%s", reason);
        EnterCriticalSection(&s_cobLock);
        SIM_FREE_OWNED(s_cobRejected, s_cobRejected[i]);
        s_cobRejected[i] = bad;
        /* This is the same availability bit UNITS_Create tests at 0x485FA0.
           Keep the def itself so required/saved units still resolve by name. */
        *(unsigned*)(def + 0x241) &= ~0x800000u;
        LeaveCriticalSection(&s_cobLock);
        tagpu_logf("cob: disabled unit %s (%s), type %u, script %s: %s",
                   bad->unit, bad->display, i, bad->script, bad->reason);
        return;
    }
    cob_fault(script, 0, "invalid script has no owning unit definition");
}

static void cob_required(unsigned type)
{
    char report[640];
    CobRejected* bad;
    EnterCriticalSection(&s_cobLock);
    bad = type < TAGPU_LIM_TYPES ? s_cobRejected[type] : NULL;
    if (bad) {
        snprintf(report, sizeof report, "Cannot create a required or saved unit.\n\n"
                 "Unit: %s (%s), type %u\nScript: %s\nReason: %s\n\n"
                 "No unit has been substituted. This game cannot continue.",
                 bad->unit, bad->display, type, bad->script, bad->reason);
        LeaveCriticalSection(&s_cobLock);
        tagpu_refuse(report);
        return;
    }
    LeaveCriticalSection(&s_cobLock);
}

static unsigned __cdecl cob_create_check(unsigned* regs)
{
    char* ta = *(char**)0x00511DE8u;
    unsigned type = regs[PR_ESI] & 0xFFFFu;
    unsigned caller = *(unsigned*)(WPN_ESP_JMP(regs) + 20);
    unsigned count = ta ? *(unsigned*)(ta + 0x1438F) : 0;
    const char* def;
    if (!type || type >= count || count > TAGPU_LIM_TYPES) return 0x004861BDu;
    if (cob_type_rejected(type)) {
        /* Saved restore and starting commanders. Mission placement is checked
           in its held-create wrapper, which owns a second return address.
           Ordinary construction returns NULL, just like an unavailable type. */
        if (caller == 0x00487193u || caller == 0x00488467u ||
            caller == 0x00497007u || caller == 0x004977C0u || caller == 0x004653DEu)
            cob_required(type);
        return 0x004861BDu;
    }
    def = *(const char* const*)(ta + 0x1439B) + type * 0x249u;
    return (*(const unsigned*)(def + 0x241) & 0x800000u) ? 0x00485FB0u : 0x004861BDu;
}

static unsigned __cdecl cob_model_check(unsigned* regs)
{
    const char* unit = (const char*)(size_t)regs[PR_ESI];
    const char* def = (const char*)(size_t)regs[PR_EAX];
    /* The model constructor also serves network/recreate paths which bypass
       UNITS_Create. None may leave a live unit with its required COB missing. */
    cob_required(*(const unsigned short*)(unit + 0xA6));
    regs[PR_ECX] = *(const unsigned*)(def + 0x18E);
    return 0x00485D6Au;
}

static unsigned __cdecl cob_chat_frame(void)
{
    unsigned result = ((unsigned (__cdecl*)(void))0x00428C50u)();
    unsigned i;
    CobRejected bad;
    char message[576], line[64];
    size_t offset, length;
    DWORD now = GetTickCount();
    int found = 0;
    EnterCriticalSection(&s_cobLock);
    if (s_cobInPlay && (!s_cobNextChat || (LONG)(now - s_cobNextChat) >= 0)) {
        for (i = 1; i < TAGPU_LIM_TYPES; ++i) {
            if (s_cobRejected[i] && !s_cobRejected[i]->announced) {
                bad = *s_cobRejected[i];
                s_cobRejected[i]->announced = 1;
                s_cobNextChat = now + 5000;
                found = 1;
                break;
            }
        }
    }
    LeaveCriticalSection(&s_cobLock);
    if (!found) return result;
    snprintf(message, sizeof message, "COB: %s (%s), type %u disabled. Script %s: %s",
             bad.unit, bad.display, i, bad.script, bad.reason);
    tagpu_logf("cob: entry message: %s", message);
    /* NewChatText (0x463CA0) owns 64 bytes per line, including NUL. ShowReminder
       copies them locally; no network packet and no pointer outlives this call. */
    length = strlen(message);
    for (offset = 0; offset < length; offset += 63) {
        snprintf(line, sizeof line, "%.63s", message + offset);
        ((void (__stdcall*)(const char*, int))0x0046BC70u)(line, 0);
    }
    return result;
}

/* Metadata follows the engine's script allocation, not a reusable pointer key.
   The destructor hook removes it before the engine frees the blob. Validation
   sees the byte count returned by the SAME file read, before relocation. */
static CobFile* cob_find(const void* blob)
{
    CobFile* f = s_cobFiles[((uintptr_t)blob >> 4) & 255u];
    while (f && f->blob != blob) f = f->next;
    return f;
}

static void* __stdcall cob_read(const char* path, unsigned* ignored)
{
    unsigned size = 0, bucket;
    void* blob = ((void* (__stdcall*)(const char*, unsigned*))0x004BBE50u)(path, &size);
    CobFile* f;
    char reason[256];
    const char* name = strrchr(path, '\\');
    (void)ignored;
    /* The native model constructor explicitly supports scriptless types
       (0x485DFE). Absence is not proof of a malformed script. */
    if (!blob) return NULL;
    f = (CobFile*)SIM_ALLOC_OWNED(s_cobFiles, sizeof *f);
    if (!f) cob_fault(name ? name + 1 : path, 0, "COB metadata allocation failed");
    f->blob = blob; f->size = size;
    snprintf(f->name, sizeof f->name, "%s", name ? name + 1 : path);
    f->program = tagpu_cob_program(blob, size, reason, sizeof reason);
    if (!f->program) {
        cob_reject_file(f->name, reason);
        ((void (__cdecl*)(void*))0x004D85A0u)(blob);
        SIM_FREE_OWNED(s_cobFiles, f);
        return NULL;
    }
    bucket = ((uintptr_t)blob >> 4) & 255u;
    EnterCriticalSection(&s_cobLock);
    f->next = s_cobFiles[bucket]; s_cobFiles[bucket] = f;
    LeaveCriticalSection(&s_cobLock);
    return blob;
}

static unsigned __stdcall cob_read_size(void* blob)
{
    CobFile* f;
    unsigned size;
    EnterCriticalSection(&s_cobLock);
    f = cob_find(blob);
    size = f ? f->size : 0;
    LeaveCriticalSection(&s_cobLock);
    if (!f) cob_fault("unknown", 0, "script allocation has no validated byte count");
    return size;
}

static void __cdecl cob_free_blob(void* blob)
{
    CobFile** link;
    CobFile* f;
    EnterCriticalSection(&s_cobLock);
    link = &s_cobFiles[((uintptr_t)blob >> 4) & 255u];
    while (*link && (*link)->blob != blob) link = &(*link)->next;
    f = *link;
    if (f) *link = f->next;
    tagpu_cob_program_free(f ? f->program : NULL);
    SIM_FREE_OWNED(s_cobFiles, f);
    LeaveCriticalSection(&s_cobLock);
    ((void (__cdecl*)(void*))0x004D85A0u)(blob);
}

const struct TagpuCobProgram* tagpu_cob_def_program(const char* def)
{
    const void* blob;
    const TagpuCobProgram* program = NULL;
    CobFile* f;
    /* the read hook and its lock exist only once lim_sites installed them, which a limits state above 0 proves:
       an early return there sets it to -1 */
    if (!def || !tagpu_limits_in()) return NULL;
    blob = *(void* const*)(def + 0x18E);
    if (!blob) return NULL;
    EnterCriticalSection(&s_cobLock);
    f = cob_find(blob);
    if (f) program = f->program;
    LeaveCriticalSection(&s_cobLock);
    return program;
}

static void* cob_bind_loaded_model(char* def, void* blob)
{
    char* ta = *(char**)0x00511DE8u;
    const char* base = *(const char**)(ta + 0x1439B);
    unsigned count = *(unsigned*)(ta + 0x1438F);
    uintptr_t delta = (uintptr_t)def - (uintptr_t)base;
    CobFile* f;
    if (!base || count > TAGPU_LIM_TYPES || delta % 0x249u || delta / 0x249u >= count)
        cob_fault("unknown", 0, "loaded unit definition exceeds its allocation");
    if (blob) {
        char reason[256];
        const char* root = (*(const char***)(ta + 0x14377))[delta / 0x249u];
        unsigned pieces = root ? ((unsigned (__stdcall*)(const char*))0x0045AE80u)(root) : 0;
        EnterCriticalSection(&s_cobLock);
        f = cob_find(blob);
        if (!f) cob_fault("unknown", 0, "loaded script allocation is not validated");
        /* DISASSEMBLED: 0x42D79C publishes this template before the COB load.
           0x45A950 uses this same node count to allocate 34 + 54*N bytes;
           0x45A9E8 skips excess COB names, it does not enlarge that allocation.
           Unused excess names are legal; reachable accesses must fit. */
        if (!tagpu_cob_bind_model(f->program, pieces, reason, sizeof reason)) {
            cob_reject_file(f->name, reason);
            LeaveCriticalSection(&s_cobLock);
            /* Relocation already registered this blob in the native checksum
               tree (0x4B2499); its destructor removes that entry before the
               hooked blob free. A direct free would leave a stale tree key. */
            ((void (__stdcall*)(void*))0x004B2540u)(blob);
            blob = NULL;
        } else LeaveCriticalSection(&s_cobLock);
    }
    *(void**)(def + 0x18E) = blob;
    return blob;
}

static unsigned __cdecl cob_loaded_model_check(unsigned* regs)
{
    regs[PR_EAX] = (unsigned)(size_t)cob_bind_loaded_model(
        (char*)(size_t)regs[PR_EBP], (void*)(size_t)regs[PR_EAX]);
    return 0x0042D8FAu;
}

static unsigned __cdecl cob_reloaded_model_check(unsigned* regs)
{
    regs[PR_EAX] = (unsigned)(size_t)cob_bind_loaded_model(
        (char*)(size_t)regs[PR_ESI], (void*)(size_t)regs[PR_EAX]);
    return 0x0042D29Fu;
}

/* THE BLOCK (research/notes/cob-speed-plan.md, L4). A unit's COB object is a block sized by its
   program, 64-aligned, from the engine's MEM_Alloc 0x4B4F10 as the engine's object was:
     +0x00 the engine's header line: the vtable, the rate +0x04, the script +0x08, its checksum
           +0x0C, the statics +0x10 and animation +0x14 pointers, "something is animating" +0x18
     +0x1C the running count, +0x20 the model pointer -- the offsets the guard's count and model
           sites take (cob_runtime_sites), at every engine site that reads them
     +0x24 CobTail: `bound`, then the VM, whose schedule, fault, object and latch words come first,
           so a turn with nothing due (tagpu_cobvm_rest) reads this one line of the block
     then the ten record headers packed, then ten stacks of the program's proven depth.
   The engine reads the object through those header words and pointers only, and writes its
   records only in the constructor 0x4B0610 (the engine map, cob), whose record loop the block's
   constructor replaces; the destructor 0x485E30's free at 0x485E40 is the block's, through
   MEM_Free 0x4B4F20. All of it is memory of the engine's allocator, which the object's life
   bounds (from 0x485D40 to the unit destructor's delete at 0x486D8A). Statics and
   animation stay the engine's allocations (0x4B0720, freed by 0x4B06F0). With the oracle armed
   (tagpu_cobverify.on) every object keeps the engine's layout, which its interpreter needs. */
#define COB_BLOCK_COUNT 0x1Cu
#define COB_BLOCK_MODEL 0x20u
#define COB_BLOCK_TAIL  0x24u
static int s_cobBlock;
SIM_VAR(s_cobBlock, SIM_INSTALL);
static unsigned s_cobModelOff = TAGPU_COB_MODEL_OFFSET;
SIM_VAR(s_cobModelOff, SIM_INSTALL);
static unsigned s_cobCountOff = TAGPU_COB_COUNT_OFFSET;
SIM_VAR(s_cobCountOff, SIM_INSTALL);
static unsigned s_cobTailOff = TAGPU_COB_OBJECT_BYTES;
SIM_VAR(s_cobTailOff, SIM_INSTALL);
#define COB_MODEL(cob) (*(unsigned char**)(void*)((cob) + s_cobModelOff))

/* the layout every object of this session has, chosen before the first site that reads it */
static void cob_layout_choose(void)
{
    s_cobBlock = !tagpu_sim_lever("tagpu_cobverify.on");
    if (!s_cobBlock) return;
    s_cobModelOff = COB_BLOCK_MODEL;
    s_cobCountOff = COB_BLOCK_COUNT;
    s_cobTailOff = COB_BLOCK_TAIL;
}

unsigned tagpu_cob_model_offset(void) { return s_cobModelOff; }

static void cob_model_bound(char* cob, const CobFile* f)
{
    const unsigned* model = (const unsigned*)(void*)COB_MODEL(cob);
    /* The unit owns its posed model until after COB destruction (0x486D8A,
       then 0x486D9E). Interpreter and save callbacks run on that owning thread. */
    if (!model || f->program->model_pieces > model[0])
        cob_fault(f->name, 0, "COB piece count exceeds the posed model allocation");
}

static TagpuCobRecord* cob_record(char* cob, unsigned slot)
{
    if (slot >= TAGPU_COB_THREADS) cob_fault("unknown", 0, "thread index exceeds its allocation");
    return (TagpuCobRecord*)(cob + 28 + slot * TAGPU_COB_RECORD_BYTES);
}

static unsigned __cdecl cob_allocated(unsigned* regs)
{
    char* cob = (char*)(size_t)regs[PR_ECX];
    TagpuCobRecord* record = cob_record(cob, regs[PR_EAX]);
    record->sleep = record->piece = record->axis = record->child = 0;
    memset(record->stack, 0, sizeof record->stack);
    ++*(unsigned*)(cob + TAGPU_COB_COUNT_OFFSET);
    return 0x004B0927u;
}

static unsigned __cdecl cob_run_enter(unsigned* regs)
{
    char* cob = (char*)(size_t)regs[PR_EDI];
    TagpuCobRecord* record = cob_record(cob, regs[PR_EBP]);
    CobFile* f;
    char reason[256];
    if (record->status) {
        EnterCriticalSection(&s_cobLock);
        f = cob_find(*(void**)(cob + 8));
        if (!f) cob_fault("unknown", record->pc, "script allocation is not validated");
        cob_model_bound(cob, f);
        if (!tagpu_cob_check_record(f->program, record, reason, sizeof reason))
            cob_fault(f->name, record->pc, reason);
        LeaveCriticalSection(&s_cobLock);
    }
    regs[PR_ECX] = regs[PR_EBP] * (TAGPU_COB_RECORD_BYTES / 4);
    return 0x004B0DB5u;
}

static unsigned __cdecl cob_run_step(unsigned* regs)
{
    char* cob = (char*)(size_t)regs[PR_EDI];
    TagpuCobRecord* record = cob_record(cob, regs[PR_EBP]);
    CobFile* f;
    char reason[256];
    unsigned kind, argc = 0, script = 0;
    int checked;
    if ((void*)record != (void*)(size_t)regs[PR_ESI])
        cob_fault("unknown", 0, "interpreter record does not match its bounded slot");
    EnterCriticalSection(&s_cobLock);
    f = cob_find(*(void**)(cob + 8));
    if (!f) cob_fault("unknown", record->pc, "script allocation is not validated");
    checked = tagpu_cob_check_step(f->program, record, reason, sizeof reason);
    if (!checked) cob_fault(f->name, record->pc, reason);
    if (checked == 2) {
        LeaveCriticalSection(&s_cobLock);
        return 0x004B1B60u;
    }
    kind = f->program->code[record->pc] & 0x100ff000u;
    if (kind == 0x10061000u || kind == 0x10062000u) {
        script = f->program->code[record->pc + 1];
        argc = f->program->code[record->pc + 2];
    }
    LeaveCriticalSection(&s_cobLock);
    tagpu_cobtrace_step(cob, regs[PR_EBP]);
    if (kind == 0x10083000u || kind == 0x10084000u) {
        int words = kind == 0x10083000u ? 3 : 1;
        int id = record->stack[record->sp + 1 - words];
        if (words == 3) {
            int piece = record->stack[record->sp - 1];
            /* The transport packet has one byte for the piece. 0xFF means
               hidden cargo (0x48AC99), not piece 255 of a larger model. */
            if (!tagpu_cob_attach_piece_ok(piece, f->program->model_pieces))
                cob_fault(f->name, record->pc, "attachment piece exceeds its model or wire representation");
        }
        if (!cob_live_unit(*(char**)0x00511DE8u, id)) {
            record->sp -= words;
            ++record->pc;
            return 0x004B0E59u;
        }
    }
    if (kind == 0x10061000u || kind == 0x10062000u) {
        int slot;
        tagpu_cobtrace_vm_context(cob, (int)regs[PR_EBP], (int)argc, kind == 0x10062000u);
        slot = tagpu_rec_cob_child(cob, script, kind == 0x10062000u);
        tagpu_cobtrace_vm_context(NULL, 0, 0, 0);
        if (slot >= 0) {
            TagpuCobRecord* child = cob_record(cob, (unsigned)slot);
            memcpy(child->stack, record->stack + record->sp + 1 - argc, argc * sizeof(int32_t));
            child->mask = record->mask;
        }
        /* DISASSEMBLED 0x4B192F..0x4B1974: a refused CALL retains its arguments
           and waits on child -1. RETURN only compares that sentinel with the
           returning slot (0x4B1A12); it never indexes through it. START drops
           refused arguments so repeated starts cannot fill the parent's stack. */
        record->sp -= (int32_t)tagpu_cob_parent_pops(kind, slot, argc);
        record->pc += 3;
        if (kind == 0x10062000u) {
            record->child = slot;
            record->status = 0x02800000u;
            return 0x004B1BE8u;
        }
        return 0x004B0E59u;
    }
    regs[PR_EDX] = *(unsigned*)(cob + 8);
    regs[PR_ECX] = record->pc;
    return 0x004B0E63u;
}

static void cob_guard_site(unsigned va, unsigned n, const unsigned char* stock,
                           unsigned (__cdecl *check)(unsigned*), const char* name)
{
    unsigned char* code = lim_code(32);
    unsigned char* p = code;
    if (!code) return;
    *p++ = 0x60; *p++ = 0x54;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)check); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 4;
    /* POPAD ignores its saved-ESP cell; use that cell for the continuation,
       then jump indirectly without destroying any of the restored registers. */
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x0C;
    *p++ = 0x61;
    *p++ = 0xFF; *p++ = 0x64; *p++ = 0x24; *p++ = 0xEC;
    lim_branch(va, (int)n, stock, 0xE9, (unsigned)(size_t)code, name);
}

typedef unsigned (__attribute__((thiscall)) *CobStreamIO)(void*, void*, unsigned);
typedef void (__attribute__((thiscall)) *CobSeek)(void*, unsigned);
typedef unsigned (__attribute__((thiscall)) *CobLength)(void*);
typedef int (__attribute__((thiscall)) *CobPieceGet)(char*, unsigned);
typedef int (__attribute__((thiscall)) *CobAxisGet)(char*, unsigned, unsigned);
typedef void (__attribute__((thiscall)) *CobPieceSet)(char*, unsigned, int);
typedef void (__attribute__((thiscall)) *CobAxisSet)(char*, unsigned, unsigned, int);

/* The game host's state after Impure's COB object (THE GAME'S COB INTERPRETER, below). */
typedef struct CobTail {
    int         bound;
    TAGPU_COBVM vm;
    const char* name;
    uint32_t    words;      /* a record's stack in the block: the program's depth */
    int32_t*    side;       /* stacks of TAGPU_COB_WORDS for a loaded record the proof does not cover */
} CobTail;
#define COB_TAIL(cob) ((CobTail*)(void*)((cob) + s_cobTailOff))
#define COB_GAME_BYTES (TAGPU_COB_OBJECT_BYTES + (unsigned)sizeof(CobTail))
#define COB_BLOCK_HEADS ((COB_BLOCK_TAIL + (unsigned)sizeof(CobTail) + 3u) & ~3u)
#define COB_BLOCK_BYTES(words) \
    (COB_BLOCK_HEADS + TAGPU_COB_THREADS * ((unsigned)sizeof(TagpuCobHead) + 4u * (words)))
typedef char cob_tail_aligned[(TAGPU_COB_OBJECT_BYTES % 4u == 0) ? 1 : -1];
typedef char cob_block_line[(COB_BLOCK_TAIL + offsetof(CobTail, vm) + offsetof(TAGPU_COBVM, pend) +
                             sizeof(int) <= 64u) ? 1 : -1];
/* tools/cob_legacy_save_probe.py finds a block's records through its VM at +0x28: the object
   pointer at +0x38, which names the block itself, then the headers, stacks, strides and words */
typedef char cob_block_probe[(COB_BLOCK_TAIL + offsetof(CobTail, vm) == 0x28u &&
                              offsetof(TAGPU_COBVM, object) == 0x10u && offsetof(TAGPU_COBVM, heads) == 0x40u &&
                              offsetof(TAGPU_COBVM, stacks) == 0x44u && offsetof(TAGPU_COBVM, head_stride) == 0x4Cu &&
                              offsetof(TAGPU_COBVM, stack_stride) == 0x50u &&
                              offsetof(TAGPU_COBVM, words) == 0x54u) ? 1 : -1];
static void cob_game_loaded(char* cob);
static void cob_game_settle(char* cob);
static void cob_records_get(char* cob, TagpuCobRecord* out);
static void cob_records_put(char* cob, const CobFile* f, const TagpuCobRecord* in, unsigned busy);
static void cob_game_sites(void);
/* the oracle's (THE ORACLE, below): armed, and cob_get's verified half */
static int s_cvOn;
SIM_VAR(s_cvOn, SIM_INSTALL);
static int cv_get(char* cob, int key, int a, int b, int c, int d, int32_t* value);

static CobFile* cob_owned_file(char* cob)
{
    CobFile* f;
    EnterCriticalSection(&s_cobLock);
    f = cob_find(*(void**)(cob + 8));
    LeaveCriticalSection(&s_cobLock);
    /* The engine owns this script until every unit of its type is destroyed;
       notably +reload destroys those units before 0x42D275 frees the script. */
    if (!f) cob_fault("unknown", 0, "script allocation is not validated");
    cob_model_bound(cob, f);
    return f;
}

typedef void (__stdcall *CobNativePieceCenter)(char*, void*, int);
static CobNativePieceCenter s_cobNativePieceCenter;
SIM_VAR(s_cobNativePieceCenter, SIM_INSTALL);

static void __stdcall cob_piece_center(char* unit, void* output, int piece)
{
    const unsigned* model = *(const unsigned**)(unit + 0x9E);
    if (piece < 0 || !model || (unsigned)piece >= model[0]) {
        char* cob = *(char**)(unit + 0x9A);
        CobFile* f = cob ? cob_owned_file(cob) : NULL;
        cob_fault(f ? f->name : "unknown", 0, "SweetSpot piece exceeds the posed model allocation");
    }
    s_cobNativePieceCenter(unit, output, piece);
}

static int __attribute__((thiscall)) cob_save(char* cob, void* stream)
{
    CobFile* f = cob_owned_file(cob);
    TagpuCobProgram* program = f->program;
    unsigned statics = program->statics * 4u;
    unsigned size = TAGPU_COB_SAVE_BYTES + statics + program->pieces * 108u;
    unsigned char* data = (unsigned char*)tagpu_sim_scratch(size);
    unsigned* pieces;
    unsigned i, axis;
    void** vtable = *(void***)cob;
    char reason[256];
    TagpuCobRecord records[TAGPU_COB_THREADS];
    cob_game_settle(cob);                   /* the sleeps the skipped turns owe, taken off first */
    if (!data) cob_fault(f->name, 0, "saved script allocation failed");
    cob_records_get(cob, records);
    if (!tagpu_cob_save_records(program, *(unsigned*)(cob + 12), records,
                                data, TAGPU_COB_SAVE_BYTES, reason, sizeof reason))
        cob_fault(f->name, 0, reason);
    if (statics) memcpy(data + TAGPU_COB_SAVE_BYTES, *(void**)(cob + 16), statics);
    pieces = (unsigned*)(data + TAGPU_COB_SAVE_BYTES + statics);
    /* Keep the declared-size wire layout, but never ask the model for unused
       excess declarations. Their save rows remain canonical zero padding. */
    for (i = 0; i < program->model_pieces; ++i) {
        unsigned* saved = pieces + i * 27u;
        const unsigned* animation = *(const unsigned**)(cob + 20) + i * 19u;
        memcpy(saved, animation + 1, 18u * 4u);
        for (axis = 0; axis < 3; ++axis) {
            saved[18 + axis] = ((CobAxisGet)vtable[5])(cob, i, axis);
            saved[21 + axis] = ((CobAxisGet)vtable[6])(cob, i, axis);
            saved[24 + axis] = ((CobPieceGet)vtable[7 + axis])(cob, i);
        }
    }
    ((CobSeek)0x004B4C10u)(stream, 0);
    i = ((CobStreamIO)0x004B4CF0u)(stream, data, size);
    tagpu_sim_scratch_free(data);
    if (i != size) cob_fault(f->name, 0, "saved script write was incomplete");
    return 1;
}

static int __attribute__((thiscall)) cob_load(char* cob, void* stream)
{
    CobFile* f = cob_owned_file(cob);
    TagpuCobProgram* program = f->program;
    unsigned statics = program->statics * 4u, tail = statics + program->pieces * 108u;
    unsigned size = ((CobLength)0x004B4BF0u)(stream), records_size, busy, i, axis;
    unsigned char* data;
    unsigned* pieces;
    TagpuCobRecord records[TAGPU_COB_THREADS];
    void** vtable = *(void***)cob;
    char reason[256];
    if (size < tail || (size - tail != 0x528u && size - tail != TAGPU_COB_SAVE_BYTES &&
                        size - tail != TAGPU_COB_LEGACY_SAVE_BYTES))
        cob_fault(f->name, 0, "saved script size does not match its validated allocation");
    records_size = size - tail;
    data = (unsigned char*)tagpu_sim_scratch(size);
    if (!data) cob_fault(f->name, 0, "saved script allocation failed");
    ((CobSeek)0x004B4C10u)(stream, 0);
    if (((CobStreamIO)0x004B4C80u)(stream, data, size) != size)
        cob_fault(f->name, 0, "saved script read was incomplete");
    if (!tagpu_cob_load_records(program, *(unsigned*)(cob + 12), data, records_size,
                                records, &busy, reason, sizeof reason))
        cob_fault(f->name, 0, reason);
    pieces = (unsigned*)(data + records_size + statics);
    for (i = 0; i < program->model_pieces; ++i)
        for (axis = 0; axis < 3; ++axis)
            if (pieces[i * 27u + 24u + axis] > 1u)
                cob_fault(f->name, 0, "saved piece flag is not boolean");
    cob_records_put(cob, f, records, busy);
    if (statics) memcpy(*(void**)(cob + 16), data + records_size, statics);
    /* The constructor zeroes every animation row (0x4B0783..0x4B0791).
       Leave excess declarations inert, including when importing a native save
       whose unchecked model getter serialized bytes beyond the posed model. */
    for (i = program->model_pieces; i < program->pieces; ++i)
        memset(*(unsigned**)(cob + 20) + i * 19u, 0, 19u * sizeof(unsigned));
    for (i = 0; i < program->model_pieces; ++i) {
        unsigned* saved = pieces + i * 27u;
        unsigned* animation = *(unsigned**)(cob + 20) + i * 19u;
        animation[0] = 1;
        memcpy(animation + 1, saved, 18u * 4u);
        for (axis = 0; axis < 3; ++axis) {
            ((CobPieceSet)vtable[2 + axis])(cob, i, (int)saved[24 + axis]);
            ((CobAxisSet)vtable[0])(cob, i, axis, (int)saved[18 + axis]);
            ((CobAxisSet)vtable[1])(cob, i, axis, (int)saved[21 + axis]);
        }
    }
    *(unsigned*)(cob + 24) = 1;
    tagpu_sim_scratch_free(data);
    cob_game_loaded(cob);
    return 1;
}

static void cob_runtime_sites(void)
{
    static const unsigned model[] = {
        0x480B22,0x480C36,0x480C56,0x480C7F,0x480C8C,0x480C99,
        0x480CBA,0x480CF3,0x480D0C,0x480D1E,0x480D2B,0x480D46,
        0x480D5E,0x480D8B,0x480D96,0x480DB6,0x480DDF,0x480DF6,
        0x480E20,0x480E36,0x480E56,0x480E76,0x480EBD,0x480EF4,
        0x480F0C,0x480F5B,0x48115B,0x48124C,0x481388,0x481397,
        0x4813EE,0x481432,0x481472
    };
    static const unsigned count[] = {
        0x4B0638,0x4B09CA,0x4B0A3F,0x4B0B88,0x4B0D22,0x4B0D6A,
        0x4B19EF,0x4B19FB,0x4B1AA1,0x4B1AAD,0x4B1B68,0x4B1B77
    };
    static const unsigned stride[] = {0x4B062F,0x4B08DF,0x4B0D4C,0x4B1A1E,0x4B1AD0,0x4B1AE8};
    /* DISASSEMBLED: constructor, callback clearing, RETURN parent wakeup
       and SIGNAL parent wakeup use dword counts. Allocation, synchronous
       starts, DoScriptsNow and SIGNAL use byte loop limits. Native save/load
       iterators are replaced at their entries below. */
    static const unsigned slots_dword[] = {0x4B0616,0x4B0D33,0x4B19F5,0x4B1AA7};
    static const unsigned slots_byte[] = {0x4B08E5,0x4B09E1,0x4B0A57,0x4B0B9F,0x4B0D81,0x4B1AEE};
    static const unsigned char alloc[] = {0x8D,0x14,0x80,0x8D,0x14,0xD0};
    static const unsigned char callback[] = {0x8D,0x0C,0x80,0x8D,0x14,0xC8};
    static const unsigned char start[] = {0x8D,0x0C,0x80,0x56,0x8D,0x14,0xC8};
    static const unsigned char query[] = {0x8D,0x0C,0x80,0x56,0x55,0x8B,0x6C,0x24,0x14,0x8D,0x14,0xC8};
    static const unsigned char multiply[] = {0x69,0xD0}; /* imul edx,eax,imm32 */
    static const unsigned char entered[] = {0x8D,0x44,0xAD,0,0x8D,0x4C,0xC5,0};
    static const unsigned char step[] = {0x8B,0x57,8,0x8B,0x4E,4};
    static const unsigned char allocated[] = {0xFF,0x81,0x3C,5,0,0};
    static const unsigned char read[] = {0xE8,0xEF,0x99,0,0};
    static const unsigned char size[] = {0xE8,0xCB,0x97,0,0};
    static const unsigned char release[] = {0xE8,0xC6,0x5E,2,0};
    static const unsigned char save_entry[] = {0x81,0xEC,0x9C,5,0,0};
    static const unsigned char load_entry[] = {0x81,0xEC,0x38,5,0,0};
    static const unsigned char create_check[] = {0xF7,0x80,0x41,2,0,0,0,0,0x80,0,
                                                0x0F,0x84,0x0D,2,0,0};
    static const unsigned char model_check[] = {0x8B,0x88,0x8E,1,0,0};
    static const unsigned char loaded_model[] = {0x89,0x85,0x8E,1,0,0};
    static const unsigned char reloaded_model[] = {0x89,0x86,0x8E,1,0,0};
    static const unsigned char piece_center[] = {0x83,0xEC,8,0x8B,0x44,0x24,0x14};
    static const unsigned char chat_frame[] = {0xE8,0xFC,0x21,0xF9,0xFF};
    static const unsigned char path = 0x57, blob = 0x56;
    unsigned i, factor = TAGPU_COB_RECORD_BYTES / 4;
    unsigned char now[12];
    unsigned char* original;
    const unsigned char native_slots = 8, expanded_slots = TAGPU_COB_THREADS;
    InitializeCriticalSection(&s_cobLock);
    lim_dword(0x00485D74u, 0x544, COB_GAME_BYTES, "COB object allocation, the game host's tail included");
    for (i = 0; i < sizeof model / sizeof *model; ++i)
        lim_dword(model[i], 0x540, s_cobModelOff, "COB model field");
    for (i = 0; i < sizeof count / sizeof *count; ++i)
        lim_dword(count[i], 0x53C, s_cobCountOff, "COB active-thread count");
    for (i = 0; i < sizeof stride / sizeof *stride; ++i)
        lim_dword(stride[i], 0xA4, TAGPU_COB_RECORD_BYTES, "COB record iteration stride");
    for (i = 0; i < sizeof slots_dword / sizeof *slots_dword; ++i)
        lim_dword(slots_dword[i], 8, TAGPU_COB_THREADS, "COB record count");
    for (i = 0; i < sizeof slots_byte / sizeof *slots_byte; ++i)
        lim_add(slots_byte[i], 1, &native_slots, &expanded_slots, "COB record loop bound");
    lim_op(0x004B08F0u, sizeof alloc, alloc, multiply, sizeof multiply, factor, "COB allocated record address");
    lim_op(0x004B09B2u, sizeof callback, callback, multiply, sizeof multiply, factor, "COB named callback record");
    lim_op(0x004B0A27u, sizeof callback, callback, multiply, sizeof multiply, factor, "COB indexed callback record");
    now[0] = 0x56;
    memcpy(now + 1, multiply, sizeof multiply); memcpy(now + 3, &factor, 4);
    lim_add(0x004B0B25u, sizeof start, start, now, "COB native call record");
    now[0] = 0x56; now[1] = 0x55;
    memcpy(now + 2, query + 5, 4);
    memcpy(now + 6, multiply, sizeof multiply); memcpy(now + 8, &factor, 4);
    lim_add(0x004B0C57u, sizeof query, query, now, "COB query record");
    cob_guard_site(0x004B0921u, sizeof allocated, allocated, cob_allocated, "COB initialized stack allocation");
    cob_guard_site(0x004B0DADu, sizeof entered, entered, cob_run_enter, "COB bounded interpreter entry");
    cob_guard_site(0x004B0E5Du, sizeof step, step, cob_run_step, "COB bounded opcode execution");
    lim_branch(0x004B245Cu, sizeof read, read, 0xE8, (unsigned)(size_t)cob_read, "COB validated file read");
    lim_add(0x004B246Fu, 1, &path, &blob, "COB checksum uses its own allocation size");
    lim_branch(0x004B2470u, sizeof size, size, 0xE8, (unsigned)(size_t)cob_read_size, "COB allocation size");
    lim_branch(0x004B26D5u, sizeof release, release, 0xE8, (unsigned)(size_t)cob_free_blob, "COB metadata destruction");
    lim_branch(0x004B1EC0u, sizeof save_entry, save_entry, 0xE9, (unsigned)(size_t)cob_save, "COB expanded saved records");
    lim_branch(0x004B2040u, sizeof load_entry, load_entry, 0xE9, (unsigned)(size_t)cob_load, "COB bounded old and new saved records");
    cob_guard_site(0x00485FA0u, sizeof create_check, create_check, cob_create_check, "COB rejected required-unit guard");
    cob_guard_site(0x00485D64u, sizeof model_check, model_check, cob_model_check, "COB rejected model creation guard");
    cob_guard_site(0x0042D8F4u, sizeof loaded_model, loaded_model, cob_loaded_model_check, "COB model allocation bound");
    cob_guard_site(0x0042D299u, sizeof reloaded_model, reloaded_model, cob_reloaded_model_check, "COB reloaded model allocation bound");
    original = lim_code(sizeof piece_center + 5);
    if (original) {
        memcpy(original, piece_center, sizeof piece_center);
        original[sizeof piece_center] = 0xE9;
        tagpu_detour_rel(original + sizeof piece_center + 1, 0x0043E0B7u);
        s_cobNativePieceCenter = (CobNativePieceCenter)(void*)original;
        lim_branch(0x0043E0B0u, sizeof piece_center, piece_center, 0xE9,
                   (unsigned)(size_t)cob_piece_center, "COB bounded SweetSpot piece");
    }
    lim_branch(0x00496A4Fu, sizeof chat_frame, chat_frame, 0xE8, (unsigned)(size_t)cob_chat_frame, "COB entry diagnostics");
    cob_game_sites();
}

/* Interpreter callbacks run on the game thread in play, and on the loader thread
   for Create's first slice while the level load creates units (every create runs
   Create with run-now set, 0x485DE6). The unit array is allocated by the level
   init 0x4854A0 before the first create and freed only by the teardown 0x485980,
   so every slot below the count is engine memory for the whole call on either
   thread. During a network load the other thread's pump can remove a player and
   kill its units, so a slot can die while it is read; the answer is then stale
   for that one call. Nothing read here can index outside what the engine owns:
   the id is bounded by the count, and the owner byte +0xFF, bounded below 10
   here, is stored only by the level init (0x485880, 0x48590E), so a caller may
   read it again. */
static char* cob_live_unit(char* ta, int id)
{
    unsigned slots;
    char* base;
    char* unit;
    if (!ta) return NULL;
    slots = *(const unsigned short*)(ta + 0x14351);
    if (!tagpu_cob_id_valid(id, slots)) return NULL;
    base = *(char**)(ta + 0x14357);
    if (!base) return NULL;
    unit = base + (unsigned)id * 0x118u;
    if (!(*(const unsigned int*)(unit + 0x110) & 0x10000000u) ||
        *(const unsigned short*)(unit + 0xA8) != id ||
        (unsigned char)unit[0xFF] >= 10u) return NULL;
    return unit;
}

static char* cob_caller(char* ta, char* cob)
{
    char* model = (char*)COB_MODEL(cob);
    char* unit;
    char* base;
    uintptr_t delta;
    if (!ta || !model) return NULL;
    unit = *(char**)(model + 0xC);
    base = *(char**)(ta + 0x14357);
    if (!base || (uintptr_t)unit < (uintptr_t)base) return NULL;
    delta = (uintptr_t)unit - (uintptr_t)base;
    if (delta % 0x118u || delta / 0x118u > INT_MAX) return NULL;
    return cob_live_unit(ta, (int)(delta / 0x118u));
}

/* tagpu_cob_getter's world over the engine's units. Every owner it hands out comes from
   cob_live_unit, so is below 10. */
typedef struct { char* ta; char* cob; } CobGetCtx;

static void cob_facts(const char* unit, TagpuCobUnitFacts* out)
{
    out->id = *(const unsigned short*)(unit + 0xA8);
    out->owner = (unsigned char)unit[0xFF];
    out->kills = *(const unsigned short*)(unit + 0xB8);
    out->remaining = *(const float*)(unit + 0x104);
}

static int cob_world_unit(void* ctx, int32_t id, TagpuCobUnitFacts* out)
{
    char* unit = cob_live_unit(((CobGetCtx*)ctx)->ta, id);
    if (unit) cob_facts(unit, out);
    return unit != NULL;
}

static int cob_world_caller(void* ctx, TagpuCobUnitFacts* out)
{
    char* unit = cob_caller(((CobGetCtx*)ctx)->ta, ((CobGetCtx*)ctx)->cob);
    if (unit) cob_facts(unit, out);
    return unit != NULL;
}

static int cob_world_allied(void* ctx, uint32_t own, uint32_t owner)
{
    return ((CobGetCtx*)ctx)->ta[0x1B63u + own * 0x14Bu + 0x108u + owner] != 0;
}

static int cob_world_local(void* ctx, uint32_t owner)
{
    unsigned kind;
    /* a puppet seat's unit answers what the recording machine's did (tagpu_puppet.h) */
    if (tagpu_puppet_seat(owner)) return tagpu_puppet_local(owner);
    kind = (unsigned char)((CobGetCtx*)ctx)->ta[0x1B63u + owner * 0x14Bu + 0x73u];
    return kind == 1 || kind == 2;
}

static void cob_world_fill(char* cob, CobGetCtx* ctx, TagpuCobWorld* w)
{
    const unsigned* file = *(const unsigned**)(cob + 8);
    const unsigned* model = (const unsigned*)(void*)COB_MODEL(cob);
    ctx->ta = *(char**)0x00511DE8u;
    ctx->cob = cob;
    w->ctx = ctx;
    w->slots = ctx->ta ? *(const unsigned short*)(ctx->ta + 0x14351) : 0;
    w->script_pieces = file ? file[2] : 0;
    w->model_pieces = model ? model[0] : 0;
    w->unit = cob_world_unit;
    w->caller = cob_world_caller;
    w->allied = cob_world_allied;
    w->local = cob_world_local;
}

static int __attribute__((thiscall)) cob_get(char* cob, int key, int a, int b, int c, int d)
{
    CobGetCtx ctx;
    TagpuCobWorld w;
    int32_t value;
    if (cv_get(cob, key, a, b, c, d, &value)) return value;
    cob_world_fill(cob, &ctx, &w);
    if (tagpu_cob_getter(&w, key, a, &value)) return value;
    return s_cobNativeGet(cob, key, a, b, c, d);
}

static void cob_get_sites(void)
{
    static const unsigned char entry[] = {0x8B,0x81,0x40,0x05,0,0};
    unsigned char* original = lim_code(sizeof entry + 5);
    if (!original) return;
    memcpy(original, entry, sizeof entry);
    {
        unsigned model_offset = s_cobModelOff;
        memcpy(original + 2, &model_offset, sizeof model_offset);
    }
    original[sizeof entry] = 0xE9;
    tagpu_detour_rel(original + sizeof entry + 1, 0x00480776u);
    s_cobNativeGet = (CobNativeGet)(void*)original;
    lim_branch(0x00480770u, sizeof entry, entry, 0xE9,
               (unsigned int)(size_t)&cob_get, "COB bounded unit getters");
}

SIM_FEATURE(cob_game, SIM_PUPPET_RUNS, "slice-walkers battle tenseat codec-decline cob-getters resource-core-upgrade stock-resource-mex contest-waves teleport transport-turn");

/* THE GAME'S COB INTERPRETER (research/notes/cob-speed-plan.md, L2). The entries 0x4B0940 (start by
   name), 0x4B0B00 (start by index), 0x4B0C40 (the query) and 0x4B0D60 (DoScriptsNow) jump here, and
   tagpu_cobvm.c runs the unit's own COB object under IMPURE's rules: the records and the count where
   the object's layout keeps them (THE BLOCK, above, or the guarded engine's with the oracle armed),
   the flag at +0x18, the statics at *(cob+0x10), the animation at *(cob+0x14), the posed model at
   the layout's model offset. 0x4B0A70 and 0x4B0BC0 stay the engine's:
   they find the name themselves and call 0x4B0B00 and 0x4B0C40 (0x4B0AEE, 0x4B0C2F). The runner
   0x4B0DA0, the stepper 0x4B1C00 and the allocator 0x4B08C0 are no longer reached, nor the guard's
   stubs inside them.
   EVERY EFFECT GOES THROUGH THE ENGINE'S OWN FUNCTION, called through the object's vtable as the
   stepper calls it (0x4B12D8, 0x4B1B3A, 0x4B1B8D, 0x4B1BB4, 0x4B1BD2, with the operands in the same
   order), so its side effects are the engine's by construction: SET's +0xBA bit and its state setter
   0x48B090, which starts scripts on this object run-later -- the core advances a SET's pc after the
   commit, as the engine does at 0x4B1BD5 -- the puppet gates, the COB_WRITES keys, the transport
   messages, the sight-tested emits, EXPLODE's draws. GET's stock half is 0x480770's trampoline after
   the core's own tagpu_cob_getter, which is cob_get's rule; RAND is 0x4B6C30(hi - lo + 1), called for
   every draw as 0x4B15E0 calls it; a completion is its object's slot 0 (0x4B19E5, 0x4B0B1D).
   WHAT ONLY THE ENGINE'S INTERPRETER MADE, the host makes: the allocator's counts (the core reports
   every allocation it is asked for, and tagpu_rec_cob_alloc counts it with the key 0x4B08C0's sink
   would have had: the START and CALL keys, else the return address of the entry's own allocator
   call, 0x4B09A4, 0x4B0B0D, 0x4B0C4D); the slice markers' note of a start (tagpu_rec_cob_started, at
   the entries whose bytes the recorder's observers held); cobtrace's lines, from the core's events.
   THE TAIL. Each object carries the host's state (CobTail): its VM, bound at its first entry, and
   its script's name -- in the block at +0x24, in the engine's layout after Impure's COB object, which
   0x485D74 then sizes. The allocation at 0x485D78 is zeroed and the setup 0x485D40 is the object's
   only maker (0x485D88 is the constructor's only caller, DISASSEMBLED), so a tail starts unbound by
   construction; a block's only other memory is a loaded save's stacks (cob_records_put).
   THREADS. An entry runs on the thread the engine calls it on: the game thread in play, the loader
   for the Create of a level's units (0x485DE6). An object is the one unit's, entered by one thread at
   a time, and the core keeps no state outside it. */

/* MEM_Alloc 0x4B4F10(size), cdecl (`add esp,4` at 0x485D7F), zeroed. While the heap answers NULL,
   0x4D83C0 calls the new handler at [0x5289BC] and asks again (0x4D8409..0x4D841F): with the
   engine's 0x49E700 installed it never returns NULL, and without one a NULL is a fault here, since
   the setup's NULL test at 0x485D82 skips only the constructor and 0x4B0720 then writes through it. */
static void* cob_engine_alloc(unsigned size)
{
    void* p = ((void* (__cdecl*)(unsigned))0x004B4F10u)(size);
    if (!p) cob_fault("unknown", 0, "COB object allocation failed");
    memset(p, 0, size);
    return p;
}

static void cob_engine_free(void* p)
{
    if (p) ((void (__cdecl*)(void*))0x004B4F20u)(p);
}

/* 0x485D78's call with the oracle armed: the engine's object, zeroed for the host's tail */
static void* __cdecl cob_object_alloc(unsigned size)
{
    return cob_engine_alloc(size);
}

/* The block of the unit 0x485D40 makes (esi there, 0x485D48; the stub below passes it), sized by
   its script's program -- the file 0x485D64 found, its program validated at the script's load --
   64-aligned, zeroed, the engine allocation's pointer kept in the four bytes before it. A script
   with no program gets stacks of TAGPU_COB_WORDS; its first entry faults as before
   (cob_owned_file). */
static void* __cdecl cob_block_alloc(unsigned size, const char* unit)
{
    const char* def;
    void* script;
    CobFile* f = NULL;
    unsigned words, bytes;
    unsigned char *raw, *p;
    if (!s_cobBlock) return cob_object_alloc(size);
    def = *(const char* const*)(unit + 0x92);
    script = def ? *(void* const*)(def + 0x18E) : NULL;
    if (script) {
        EnterCriticalSection(&s_cobLock);
        f = cob_find(script);
        LeaveCriticalSection(&s_cobLock);
    }
    words = f ? f->program->depth : TAGPU_COB_WORDS;
    bytes = COB_BLOCK_BYTES(words);
    raw = (unsigned char*)cob_engine_alloc(bytes + 68u);
    p = (unsigned char*)(((uintptr_t)raw + 4u + 63u) & ~(uintptr_t)63u);
    memcpy(p - 4, &raw, sizeof raw);
    COB_TAIL((char*)p)->words = words;
    return p;
}

/* 0x4B0610 for the block: what it writes but the record loop (0x4B061A..0x4B0641), whose status
   words at the guard's 0x224 stride would land in the block's tail and headers and past its end,
   into a block already zero. Returns the object, as it does. */
static char* __attribute__((thiscall)) cob_block_construct(char* cob)
{
    *(uint32_t*)(void*)cob = 0x004FDB00u;          /* the base vtable; 0x485D8D writes 0x4FD698 */
    *(uint32_t*)(void*)(cob + 0x08) = 0;
    *(uint32_t*)(void*)(cob + 0x10) = 0;
    *(uint32_t*)(void*)(cob + 0x14) = 0;
    *(uint32_t*)(void*)(cob + COB_BLOCK_COUNT) = 0;
    *(uint32_t*)(void*)(cob + 0x04) = ((uint32_t (*)(void))0x004B6330u)();   /* the sim rate */
    return cob;
}

/* 0x485E40's call, the free of 0x4B4F20(object): the block's stacks of its own, then the block by
   the engine allocation's pointer */
static void __cdecl cob_block_free(char* cob)
{
    unsigned char* raw;
    if (!cob) return;
    cob_engine_free(COB_TAIL(cob)->side);
    memcpy(&raw, cob - 4, sizeof raw);
    cob_engine_free(raw);
}

/* One entry call: the host, its run, the getter world. */
typedef struct CobCall {
    char*            cob;
    unsigned         key;
    CobGetCtx        gctx;
    TagpuCobWorld    world;
    TAGPU_COBVM_HOST host;
    TAGPU_COBVM_RUN  run;
} CobCall;

typedef int32_t (__attribute__((thiscall)) *CobVtCargo)(char*, int32_t);
typedef int32_t (__attribute__((thiscall)) *CobVtCarrier)(char*);
typedef void (__attribute__((thiscall)) *CobVt1)(char*, int32_t);
typedef void (__attribute__((thiscall)) *CobVt2)(char*, int32_t, int32_t);
typedef void (__attribute__((thiscall)) *CobVt3)(char*, int32_t, int32_t, int32_t);
typedef void (__attribute__((thiscall)) *CobDone)(void*, int32_t);
#define COB_VT(cob, off) ((*(void***)(cob))[(off) / 4u])

static int32_t cg_get(void* c, int32_t id, int32_t a, int32_t b, int32_t cc, int32_t d)
{
    return s_cobNativeGet(((CobCall*)c)->cob, id, a, b, cc, d);
}

static int32_t cg_rand(void* c, int32_t n, int32_t lo)
{
    (void)c; (void)lo;
    return ((int32_t (__stdcall*)(int32_t))0x004B6C30u)(n);
}

static int32_t cg_cargo(void* c, int32_t id)
{
    char* cob = ((CobCall*)c)->cob;
    return ((CobVtCargo)COB_VT(cob, 0x48))(cob, id);
}

static int32_t cg_carrier(void* c)
{
    char* cob = ((CobCall*)c)->cob;
    return ((CobVtCarrier)COB_VT(cob, 0x4C))(cob);
}

static int cg_alive(void* c, int32_t id)
{
    (void)c;
    return cob_live_unit(*(char**)0x00511DE8u, id) != NULL;
}

static void cg_done(void* c, uint32_t cb, int32_t value)
{
    void* object = (void*)(uintptr_t)cb;
    (void)c;
    ((CobDone)(**(void***)object))(object, value);
}

static void cg_commit(void* c, const TAGPU_COBVM_EFFECT* e)
{
    char* cob = ((CobCall*)c)->cob;
    switch (e->op) {
    case 0x10082000u: ((CobVt2)COB_VT(cob, 0x40))(cob, e->v[0], e->v[1]); break;      /* SET id value */
    case 0x1000F000u: ((CobVt2)COB_VT(cob, 0x30))(cob, e->piece, e->v[0]); break;     /* EMIT_SFX */
    case 0x10071000u: ((CobVt2)COB_VT(cob, 0x34))(cob, e->piece, e->v[0]); break;     /* EXPLODE */
    case 0x10083000u: ((CobVt3)COB_VT(cob, 0x38))(cob, e->v[0], e->v[1], e->v[2]); break; /* ATTACH */
    case 0x10084000u: ((CobVt1)COB_VT(cob, 0x3C))(cob, e->v[0]); break;               /* DROP */
    default: cob_fault(COB_TAIL(cob)->name, (unsigned)e->pc, "the COB VM committed an unknown effect");
    }
}

static void cg_alloc(void* c, int script, char from)
{
    CobCall* k = (CobCall*)c;
    tagpu_rec_cob_alloc(k->cob, script, from == TAGPU_COBVM_FROM_START ? 0x004B18C0u :
                                        from == TAGPU_COBVM_FROM_CALL ? 0x004B192Du : k->key);
}

static void cg_trace(void* c, const TAGPU_COBVM_TRACE* t)
{
    tagpu_cobtrace_core(((CobCall*)c)->cob, t);
}

/* The object's VM, bound at its first entry: the program cob_owned_file validated (and bound to the
   model at the script's load), the blocks 0x4B0720 and 0x480D40 made before Create ran (0x485DAC,
   0x485DD2, then 0x485DE6). */
static void cob_bind(char* cob, CobTail* t, const CobFile* f)
{
    char reason[160];
    int ok;
    if (!s_cobBlock)
        ok = tagpu_cobvm_bind(&t->vm, f->program, TAGPU_COBVM_IMPURE, (unsigned char*)cob,
                              *(int32_t**)(cob + 0x10), *(int32_t**)(cob + 0x14), COB_MODEL(cob),
                              reason, sizeof reason);
    else {
        TAGPU_COBVM_LAYOUT l;
        l.heads = (unsigned char*)cob + COB_BLOCK_HEADS;
        l.head_stride = sizeof(TagpuCobHead);
        if (t->side) {
            l.stacks = t->side;
            l.stack_stride = l.words = TAGPU_COB_WORDS;
        } else {
            l.stacks = (int32_t*)(void*)(l.heads + TAGPU_COB_THREADS * sizeof(TagpuCobHead));
            l.stack_stride = l.words = t->words;
        }
        l.count = (uint32_t*)(void*)(cob + COB_BLOCK_COUNT);
        ok = tagpu_cobvm_bind_layout(&t->vm, f->program, TAGPU_COBVM_IMPURE, (unsigned char*)cob, &l,
                                     *(int32_t**)(cob + 0x10), *(int32_t**)(cob + 0x14), COB_MODEL(cob),
                                     reason, sizeof reason);
    }
    if (!ok) cob_fault(f->name, 0, reason);
    t->name = f->name;
    t->bound = 1;
}

static TAGPU_COBVM* cg_vm(char* cob)
{
    CobTail* t = COB_TAIL(cob);
    if (!t->bound) cob_bind(cob, t, cob_owned_file(cob));
    return &t->vm;
}

/* The object's records in the save's form (TAGPU_COB_WORDS a stack): in the block, a stack's words
   past the program's depth, which no record of it touches, are zero. */
static void cob_records_get(char* cob, TagpuCobRecord* out)
{
    TAGPU_COBVM* vm;
    unsigned s;
    if (!s_cobBlock) {
        memcpy(out, cob_record(cob, 0), TAGPU_COB_THREADS * sizeof *out);
        return;
    }
    vm = cg_vm(cob);
    memset(out, 0, TAGPU_COB_THREADS * sizeof *out);
    for (s = 0; s < TAGPU_COB_THREADS; ++s) {
        memcpy(&out[s], tagpu_cobvm_record(vm, s), sizeof(TagpuCobHead));
        memcpy(out[s].stack, tagpu_cobvm_stack(vm, s), vm->words * sizeof(int32_t));
    }
}

/* A saved game's records into the object. In the block a record the stack proof covers fits the
   program's depth; one it does not cover gets stacks of TAGPU_COB_WORDS of the object's own, which
   the block's free releases with it. */
static void cob_records_put(char* cob, const CobFile* f, const TagpuCobRecord* in, unsigned busy)
{
    CobTail* t;
    TAGPU_COBVM* vm;
    unsigned s;
    if (!s_cobBlock) {
        memcpy(cob_record(cob, 0), in, TAGPU_COB_THREADS * sizeof *in);
        *(unsigned*)(cob + TAGPU_COB_COUNT_OFFSET) = busy;
        return;
    }
    t = COB_TAIL(cob);
    for (s = 0; s < TAGPU_COB_THREADS && !t->side && t->words < TAGPU_COB_WORDS; ++s)
        if (!tagpu_cob_record_proven(f->program, (const TagpuCobHead*)(const void*)&in[s])) {
            t->side = (int32_t*)cob_engine_alloc(TAGPU_COB_THREADS * TAGPU_COB_WORDS * sizeof(int32_t));
        }
    cob_bind(cob, t, f);
    vm = &t->vm;
    for (s = 0; s < TAGPU_COB_THREADS; ++s) {
        memcpy(tagpu_cobvm_record(vm, s), &in[s], sizeof(TagpuCobHead));
        memcpy(tagpu_cobvm_stack(vm, s), in[s].stack, vm->words * sizeof(int32_t));
    }
    *vm->count = busy;
}

static void cg_call(CobCall* k, char* cob, unsigned key)
{
    char* ta = *(char**)0x00511DE8u;
    memset(k, 0, sizeof *k);
    k->cob = cob;
    k->key = key;
    cob_world_fill(cob, &k->gctx, &k->world);
    k->host.ctx = k;
    k->host.get = cg_get;
    k->host.world = &k->world;
    k->host.rand = cg_rand;
    k->host.cargo = cg_cargo;
    k->host.carrier = cg_carrier;
    k->host.alive = cg_alive;
    k->host.callback = cg_done;
    k->host.commit = cg_commit;
    k->host.alloc = cg_alloc;
    if (tagpu_cobtrace_armed()) k->host.trace = cg_trace;
    k->run.host = &k->host;
    k->run.tick = ta ? *(const uint32_t*)(ta + 0x38A47) : 0xFFFFFFFFu;   /* cobtrace's tick(), -1 with no game */
}

static int cg_done_call(char* cob, CobCall* k, int r)
{
    if (r < 0) cob_fault(COB_TAIL(cob)->name, 0, k->run.reason);
    return r;
}

SIM_FEATURE(cob_verify, SIM_PUPPET_INDEPENDENT, "cob-verify");

/* THE ORACLE, `tagpu_cobverify.on` (research/notes/cob-speed-plan.md, L2). The engine's interpreter goes
   on running the game: each entry above calls the engine's own through a trampoline of the five bytes
   the swap took. Around each entry call on an object -- a turn, a start, a query -- the oracle copies
   the object, its statics, its animation and its posed model, and logs, in order, what the engine's
   interpreter asked outside the object and what it did there: each getter world's answer and each stock
   getter's (cob_get, cv_get), each draw (the call at 0x4B15E0), each effect and each cargo or carrier
   read (the vtable's slots 0x30..0x4C, sent through logging thunks), each completion (the RETURN handler
   0x4B19D0; a refused 0x4B0B00's when that call returns), and each entry on the same object made inside
   one of those, with its answer (the run-later starts SET's state setter 0x48B090 makes). Then the core
   runs the same call on the copy over a host that answers from that log, each question checked against
   the logged one, and the copy must equal the object: the rate, the flag, the records and the count,
   the statics, the animation and the posed model. What an engine call out of the interpreter writes to
   the posed model -- EXPLODE hides its piece (0x421639, in 0x421620, which 0x481140 calls at 0x48123C),
   EMIT_SFX's repose 0x45AB10, when an axis of the unit's turn is 8 or more from the cached body turn
   (0x45AB30..0x45AB5D), copies it into +0x18..+0x1C (0x45AB65) and sets the dirty flag +8 -- is the
   engine's own, and the swap makes the same call: the oracle takes the model
   bytes each such call changed with its entry, and the replay writes them into the copy where that entry
   is answered. The first difference is logged with the unit, the call, the field and the kinds logged.
   The oracle writes no engine memory but those seven vtable slots and the two sites, and decides
   nothing.
   WHAT IT DOES NOT SEE: ATTACH's and DROP's live-unit test (cob_live_unit) is asked again after the
   engine's call, as the swap asks it; an entry made from inside a call on another object's entry is
   verified on its own, not as part of the outer call; a refused start's completion is logged when the
   start returns, after anything the completion itself did; an entry on a thread other than the one
   whose call is in progress runs unverified, and is counted. */
#define CV_DEPTH 8                  /* entry calls in progress, one inside another's effect */
#define CV_LOG   1024               /* entries one call may log before it is not compared */
#define CV_DELTA 4096               /* model words the engine's calls out of the interpreter may change, a call */
enum { CV_FULL_LOG = 1, CV_FULL_MODEL, CV_FULL_STACK, CV_FULLS };
enum { CV_GET = 'G', CV_UNIT = 'U', CV_CALLER = 'C', CV_ALLIED = 'A', CV_LOCAL = 'O', CV_RAND = 'R',
       CV_EFFECT = 'E', CV_CARGO = 'Y', CV_CARRIER = 'Z', CV_DONE = 'B', CV_NESTED = 'N' };
/* the summary's columns, the effects aside (by vtable slot) */
static const char CV_KINDS[] = "GUCAORYZBN";
typedef struct CvEntry {
    char    kind;                   /* CV_* */
    char    sub;                    /* E: the vtable offset; N: the entry, CvCall's kind */
    int32_t a[5];                   /* the question's words; N: idx, cb, now, argc, the query's mask */
    int32_t ret;                    /* the answer */
    int32_t io[4], out[4];          /* N: the four words in, and the query's four words back */
    TagpuCobUnitFacts facts;        /* U, C: the facts answered */
    unsigned d0, nd;                /* G, E, Y, Z: the posed model's words the call changed, in the pool */
} CvEntry;
typedef struct CvCtx {
    char*    cob;
    int      in_host;               /* inside a call the interpreter made out of the object */
    int      full;                  /* CV_FULL_*: the call is not compared */
    unsigned n, at;                 /* entries logged; consumed by the replay */
    unsigned nd;                    /* model words in the pool */
    unsigned short doff[CV_DELTA];  /* a word's byte offset in the posed model */
    uint32_t dval[CV_DELTA];        /* and what the engine's call left there */
    char     why[240];              /* the replay's first disagreement */
    CvEntry  log[CV_LOG];
} CvCtx;
/* the oracle's records: nothing in the simulation reads them */
static struct {
    CvCtx    ctx[CV_DEPTH];
    int      depth;                 /* written only by the thread `owner` names */
    volatile LONG owner;            /* the thread whose outermost call is in progress, or 0 */
    volatile LONG foreign;          /* calls run unverified: another thread's, or no room */
    unsigned calls, equal, different, full[CV_FULLS], reported, period;
    unsigned seen[10];              /* the entries the equal calls carried, by kind (CV_KINDS) */
    unsigned effects[8];            /* their effects by vtable slot, 0x30 to 0x4C */
} s_cv;
SIM_VAR(s_cv, SIM_PRIVATE);
static void* s_cvVt[0x50 / 4];       /* the vtable's stock slots the thunks call */
SIM_VAR(s_cvVt, SIM_INSTALL);
static void* s_cvEntry[4];           /* the engine's entries: trampolines of the stolen bytes */
SIM_VAR(s_cvEntry, SIM_INSTALL);

/* one entry call: 'n' 0x4B0940, 'i' 0x4B0B00, 'q' 0x4B0C40, 't' 0x4B0D60 */
typedef struct CvCall {
    char     kind;
    int      idx;
    uint32_t cb;
    int      now, argc, mask;       /* mask: a query's non-NULL pointers */
    int32_t  io[4];
    int32_t  dt;
} CvCall;
typedef int (__attribute__((thiscall)) *CvByName)(char*, const char*, uint32_t, int);
typedef int (__attribute__((thiscall)) *CvByIdx)(char*, int, uint32_t, int, int, int32_t, int32_t, int32_t, int32_t);
typedef int (__attribute__((thiscall)) *CvQuery)(char*, int, int32_t*, int32_t*, int32_t*, int32_t*);
typedef void (__attribute__((thiscall)) *CvTick)(char*, int32_t);

static CvCtx* cv_top(void)
{
    return s_cv.owner == (LONG)GetCurrentThreadId() && s_cv.depth > 0 ? &s_cv.ctx[s_cv.depth - 1] : NULL;
}

/* the call in progress on `cob`, when its own interpreter is the code running */
static CvCtx* cv_logging(const char* cob)
{
    CvCtx* c = cv_top();
    return c && c->cob == cob && !c->in_host && !c->full ? c : NULL;
}

static CvEntry* cv_add(CvCtx* c, char kind)
{
    CvEntry* e;
    if (c->n >= CV_LOG) { c->full = CV_FULL_LOG; return NULL; }
    e = &c->log[c->n++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    return e;
}

/* ---- the engine's side: the log ---- */

/* the posed model before an engine call out of the interpreter; NULL when not logging */
static unsigned char* cv_model_before(const CvCtx* c, const char* cob, unsigned* bytes)
{
    const unsigned* model = *(const unsigned* const*)(cob + TAGPU_COB_MODEL_OFFSET);
    unsigned char* copy;
    if (!c || !model) return NULL;
    *bytes = TAGPU_COBVM_MODEL_BYTES(model[0]);
    if ((copy = (unsigned char*)tagpu_sim_scratch(*bytes)) != NULL) memcpy(copy, model, *bytes);
    return copy;
}

/* the words that call changed, onto its entry */
static void cv_model_after(CvCtx* c, CvEntry* e, const char* cob, unsigned char* before, unsigned bytes)
{
    const unsigned char* now = *(const unsigned char* const*)(cob + TAGPU_COB_MODEL_OFFSET);
    /* the replay's copy holds the model of the program's bound pieces, which can be fewer than the
       live model's count: a changed word past it cannot be written there, and is not compared */
    unsigned room = TAGPU_COBVM_MODEL_BYTES(((const CobTail*)COB_TAIL(cob))->vm.program->model_pieces);
    unsigned i;
    if (!before) return;
    if (e) e->d0 = c->nd;
    for (i = 0; e && i < bytes; i++) {
        if (now[i] == before[i]) continue;
        if (c->nd == CV_DELTA || i + 4u > bytes || i + 4u > room || i > 0xFFFFu) {
            c->full = CV_FULL_MODEL;
            break;
        }
        c->doff[c->nd] = (unsigned short)i;
        memcpy(&c->dval[c->nd++], now + i, 4);
        e->nd++;
        i += 3;
    }
    tagpu_sim_scratch_free(before);
}

static void cv_effect(char* cob, unsigned off, int32_t a0, int32_t a1, int32_t a2)
{
    CvCtx* c = cv_logging(cob);
    CvEntry* e = c ? cv_add(c, CV_EFFECT) : NULL;
    unsigned at = e ? (unsigned)(e - c->log) : 0, bytes = 0;
    unsigned char* before = cv_model_before(c, cob, &bytes);
    if (e) { e->sub = (char)off; e->a[0] = a0; e->a[1] = a1; e->a[2] = a2; }
    if (c) c->in_host++;
    if (off == 0x3C) ((CobVt1)s_cvVt[off / 4])(cob, a0);
    else if (off == 0x38) ((CobVt3)s_cvVt[off / 4])(cob, a0, a1, a2);
    else ((CobVt2)s_cvVt[off / 4])(cob, a0, a1);
    if (c) { c->in_host--; cv_model_after(c, e ? &c->log[at] : NULL, cob, before, bytes); }
}

static void __attribute__((thiscall)) cv_vt30(char* cob, int32_t a, int32_t b) { cv_effect(cob, 0x30, a, b, 0); }
static void __attribute__((thiscall)) cv_vt34(char* cob, int32_t a, int32_t b) { cv_effect(cob, 0x34, a, b, 0); }
static void __attribute__((thiscall)) cv_vt38(char* cob, int32_t a, int32_t b, int32_t d) { cv_effect(cob, 0x38, a, b, d); }
static void __attribute__((thiscall)) cv_vt3c(char* cob, int32_t a) { cv_effect(cob, 0x3C, a, 0, 0); }
static void __attribute__((thiscall)) cv_vt40(char* cob, int32_t a, int32_t b) { cv_effect(cob, 0x40, a, b, 0); }

static int32_t cv_read(char* cob, char kind, int32_t id)
{
    CvCtx* c = cv_logging(cob);
    CvEntry* e = c ? cv_add(c, kind) : NULL;
    unsigned at = e ? (unsigned)(e - c->log) : 0, bytes = 0;
    unsigned char* before = cv_model_before(c, cob, &bytes);
    int32_t r;
    if (c) c->in_host++;
    r = kind == CV_CARGO ? ((CobVtCargo)s_cvVt[0x48 / 4])(cob, id) : ((CobVtCarrier)s_cvVt[0x4C / 4])(cob);
    if (c) { c->in_host--; cv_model_after(c, e ? &c->log[at] : NULL, cob, before, bytes); }
    if (e) { e = &c->log[at]; e->a[0] = id; e->ret = r; }
    return r;
}

static int32_t __attribute__((thiscall)) cv_vt48(char* cob, int32_t id) { return cv_read(cob, CV_CARGO, id); }
static int32_t __attribute__((thiscall)) cv_vt4c(char* cob) { return cv_read(cob, CV_CARRIER, 0); }

/* 0x4B15E0's call, stdcall like 0x4B6C30: only the running interpreter's RAND makes it */
static int32_t __stdcall cv_rand(int32_t n)
{
    int32_t r = ((int32_t (__stdcall*)(int32_t))0x004B6C30u)(n);
    CvCtx* c = cv_top();
    CvEntry* e = c && !c->in_host && !c->full ? cv_add(c, CV_RAND) : NULL;
    if (e) { e->a[0] = n; e->ret = r; }
    return r;
}

/* 0x4B19D0, RETURN: ESI the record, EDI the object (the observer's pushad frame: ESI at -0x20, EDI at
   -0x24); with a completion it pops stack[sp] and calls the completion's slot 0 with it (0x4B19D7) */
static int __cdecl cv_return(void* entry)
{
    const char* rec = *(const char* const*)((const char*)entry - 0x20);
    const char* cob = *(const char* const*)((const char*)entry - 0x24);
    CvCtx* c = cv_logging(cob);
    CvEntry* e;
    uint32_t cb;
    int32_t sp;
    if (!c) return 0;
    cb = *(const uint32_t*)(rec + 0x20);
    sp = *(const int32_t*)(rec + 8);
    if (!cb) return 0;
    if (sp < -1 || sp >= (int32_t)TAGPU_COB_WORDS) { c->full = CV_FULL_STACK; return 0; }
    e = cv_add(c, CV_DONE);
    if (e) { e->a[0] = (int32_t)cb; e->a[1] = *(const int32_t*)(rec + 0x24 + 4 * sp); }
    return 0;
}

static int cv_world_unit(void* ctx, int32_t id, TagpuCobUnitFacts* out)
{
    int r = cob_world_unit(ctx, id, out);
    CvCtx* c = cv_top();
    CvEntry* e = c && !c->full ? cv_add(c, CV_UNIT) : NULL;
    if (e) { e->a[0] = id; e->ret = r; if (r) e->facts = *out; }
    return r;
}

static int cv_world_caller(void* ctx, TagpuCobUnitFacts* out)
{
    int r = cob_world_caller(ctx, out);
    CvCtx* c = cv_top();
    CvEntry* e = c && !c->full ? cv_add(c, CV_CALLER) : NULL;
    if (e) { e->ret = r; if (r) e->facts = *out; }
    return r;
}

static int cv_world_allied(void* ctx, uint32_t own, uint32_t owner)
{
    int r = cob_world_allied(ctx, own, owner);
    CvCtx* c = cv_top();
    CvEntry* e = c && !c->full ? cv_add(c, CV_ALLIED) : NULL;
    if (e) { e->a[0] = (int32_t)own; e->a[1] = (int32_t)owner; e->ret = r; }
    return r;
}

static int cv_world_local(void* ctx, uint32_t owner)
{
    int r = cob_world_local(ctx, owner);
    CvCtx* c = cv_top();
    CvEntry* e = c && !c->full ? cv_add(c, CV_LOCAL) : NULL;
    if (e) { e->a[0] = (int32_t)owner; e->ret = r; }
    return r;
}

/* cob_get's verified half: the getter rule over a world that logs its answers, then the stock half */
static int cv_get(char* cob, int key, int a, int b, int c, int d, int32_t* value)
{
    CvCtx* x = s_cvOn ? cv_logging(cob) : NULL;
    CobGetCtx ctx;
    TagpuCobWorld w;
    if (!x) return 0;
    cob_world_fill(cob, &ctx, &w);
    w.unit = cv_world_unit;
    w.caller = cv_world_caller;
    w.allied = cv_world_allied;
    w.local = cv_world_local;
    if (tagpu_cob_getter(&w, key, a, value)) return 1;
    {
        CvEntry* e = cv_add(x, CV_GET);
        unsigned at = e ? (unsigned)(e - x->log) : 0, bytes = 0;
        unsigned char* before = cv_model_before(x, cob, &bytes);
        x->in_host++;
        *value = s_cobNativeGet(cob, key, a, b, c, d);
        x->in_host--;
        cv_model_after(x, e ? &x->log[at] : NULL, cob, before, bytes);
        if (e) {
            e = &x->log[at];
            e->a[0] = key; e->a[1] = a; e->a[2] = b; e->a[3] = c; e->a[4] = d; e->ret = *value;
        }
    }
    return 1;
}

static int cv_engine(char* cob, const CvCall* k, const char* name, int32_t* const* p)
{
    switch (k->kind) {
    case 'n': return ((CvByName)s_cvEntry[0])(cob, name, k->cb, k->now);
    case 'i': return ((CvByIdx)s_cvEntry[1])(cob, k->idx, k->cb, k->now, k->argc,
                                               k->io[0], k->io[1], k->io[2], k->io[3]);
    case 'q': return ((CvQuery)s_cvEntry[2])(cob, k->idx, p[0], p[1], p[2], p[3]);
    default:  ((CvTick)s_cvEntry[3])(cob, k->dt); return 1;
    }
}

/* 0x4B0B11: a refused start by index calls its completion with 0 at once */
static void cv_refused(CvCtx* c, const CvCall* k, int r)
{
    CvEntry* e;
    if (k->kind != 'i' || r || !k->cb || c->full) return;
    if ((e = cv_add(c, CV_DONE)) != NULL) { e->a[0] = (int32_t)k->cb; e->a[1] = 0; }
}

/* an entry on the object whose call is in progress, from inside a call its interpreter made: logged in
   that call, with its own interpreter's entries after it */
static int cv_nested_engine(CvCtx* c, char* cob, const CvCall* k, const char* name, int32_t* const* p)
{
    CvEntry* e = c->full ? NULL : cv_add(c, CV_NESTED);
    unsigned at = e ? (unsigned)(e - c->log) : 0;
    int saved = c->in_host, r, i;
    if (e) {
        e->sub = k->kind;
        e->a[0] = k->idx; e->a[1] = (int32_t)k->cb; e->a[2] = k->now; e->a[3] = k->argc; e->a[4] = k->mask;
        memcpy(e->io, k->io, sizeof e->io);
    }
    c->in_host = 0;
    r = cv_engine(cob, k, name, p);
    cv_refused(c, k, r);
    c->in_host = saved;
    if (e) {
        e = &c->log[at];
        e->ret = r;
        for (i = 0; i < 4; i++) e->out[i] = p && p[i] ? *p[i] : 0;
    }
    return r;
}

/* ---- the core's side: the replay ---- */

typedef struct CvReplay {
    CvCtx*           c;
    TAGPU_COBVM      vm;
    TagpuCobWorld    world;
    TAGPU_COBVM_HOST host;
    int              bad;
} CvReplay;

static void cvr_bad(CvReplay* rp, const char* fmt, ...)
{
    va_list ap;
    if (rp->bad) return;
    rp->bad = 1;
    va_start(ap, fmt);
    vsnprintf(rp->c->why, sizeof rp->c->why, fmt, ap);
    va_end(ap);
}

static CvEntry* cvr_take(CvReplay* rp, char kind)
{
    CvCtx* c = rp->c;
    CvEntry* e;
    if (rp->bad) return NULL;
    if (c->at >= c->n) {
        cvr_bad(rp, "the core asked %c after the engine's last entry (%u)", kind, c->n);
        return NULL;
    }
    e = &c->log[c->at];
    if (e->kind != kind) {
        cvr_bad(rp, "the core asked %c where the engine's entry %u is %c", kind, c->at, e->kind);
        return NULL;
    }
    c->at++;
    return e;
}

static int cv_core(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, const CvCall* k, int32_t io[4])
{
    switch (k->kind) {
    case 'n': return tagpu_cobvm_start(vm, run, k->idx, k->cb, k->now, 0, 0, NULL);
    case 'i': return tagpu_cobvm_start(vm, run, k->idx, k->cb, k->now, 1, k->argc, k->io);
    case 'q': return tagpu_cobvm_query(vm, run, k->idx, io);
    default:  return tagpu_cobvm_tick(vm, run, k->dt);
    }
}

/* the entries the engine made on this object inside the call just answered, run on the copy as the
   swap runs them: inside that call, on the same VM; first the posed model's words that call wrote */
static void cvr_nested(CvReplay* rp)
{
    CvCtx* c = rp->c;
    const CvEntry* done = c->at ? &c->log[c->at - 1] : NULL;
    unsigned i;
    for (i = done ? done->d0 : 0; done && !rp->bad && i < done->d0 + done->nd; i++)
        memcpy(rp->vm.model + c->doff[i], &c->dval[i], 4);
    while (!rp->bad && c->at < c->n && c->log[c->at].kind == CV_NESTED) {
        unsigned at = c->at++;
        CvCall k;
        TAGPU_COBVM_RUN run;
        int32_t io[4];
        int r, i;
        memset(&k, 0, sizeof k);
        k.kind = c->log[at].sub;
        k.idx = c->log[at].a[0]; k.cb = (uint32_t)c->log[at].a[1]; k.now = c->log[at].a[2];
        k.argc = c->log[at].a[3];
        memcpy(k.io, c->log[at].io, sizeof k.io);
        memcpy(io, k.io, sizeof io);
        memset(&run, 0, sizeof run);
        run.host = &rp->host;
        r = cv_core(&rp->vm, &run, &k, io);
        if (r < 0) { cvr_bad(rp, "the core refused the nested %c call (entry %u): %s", k.kind, at, run.reason); return; }
        if (rp->bad) return;
        if (k.kind != 't' && r != c->log[at].ret) {
            cvr_bad(rp, "the nested %c call (entry %u) answered %d, the engine's %d", k.kind, at, r, c->log[at].ret);
            return;
        }
        if (k.kind == 'q' && r == 1)
            for (i = 0; i < 4; i++)
                if ((c->log[at].a[4] >> i & 1) && io[i] != c->log[at].out[i]) {
                    cvr_bad(rp, "the nested query (entry %u) gave word %d %d, the engine's %d", at, i, io[i],
                            c->log[at].out[i]);
                    return;
                }
    }
}

static int32_t cvr_get(void* x, int32_t id, int32_t a, int32_t b, int32_t c, int32_t d)
{
    CvReplay* rp = (CvReplay*)x;
    CvEntry* e = cvr_take(rp, CV_GET);
    int32_t r;
    if (!e) return 0;
    if (e->a[0] != id || e->a[1] != a || e->a[2] != b || e->a[3] != c || e->a[4] != d) {
        cvr_bad(rp, "GET %d(%d,%d,%d,%d) where the engine's was GET %d(%d,%d,%d,%d)", id, a, b, c, d,
                e->a[0], e->a[1], e->a[2], e->a[3], e->a[4]);
        return 0;
    }
    r = e->ret;
    cvr_nested(rp);
    return r;
}

static int32_t cvr_rand(void* x, int32_t n, int32_t lo)
{
    CvReplay* rp = (CvReplay*)x;
    CvEntry* e = cvr_take(rp, CV_RAND);
    (void)lo;
    if (!e) return 0;
    if (e->a[0] != n) { cvr_bad(rp, "RAND of %d where the engine's drew of %d", n, e->a[0]); return 0; }
    return e->ret;
}

static int32_t cvr_read(CvReplay* rp, char kind, int32_t id)
{
    CvEntry* e = cvr_take(rp, kind);
    int32_t r;
    if (!e) return 0;
    if (e->a[0] != id) { cvr_bad(rp, "%c of %d where the engine's was of %d", kind, id, e->a[0]); return 0; }
    r = e->ret;
    cvr_nested(rp);
    return r;
}

static int32_t cvr_cargo(void* x, int32_t id) { return cvr_read((CvReplay*)x, CV_CARGO, id); }
static int32_t cvr_carrier(void* x) { return cvr_read((CvReplay*)x, CV_CARRIER, 0); }
static int cvr_alive(void* x, int32_t id) { (void)x; return cob_live_unit(*(char**)0x00511DE8u, id) != NULL; }

static void cvr_callback(void* x, uint32_t cb, int32_t value)
{
    CvReplay* rp = (CvReplay*)x;
    CvEntry* e = cvr_take(rp, CV_DONE);
    if (!e) return;
    if ((uint32_t)e->a[0] != cb || e->a[1] != value) {
        cvr_bad(rp, "a completion %08X(%d) where the engine's was %08X(%d)", cb, value, (uint32_t)e->a[0], e->a[1]);
        return;
    }
    cvr_nested(rp);
}

static void cvr_commit(void* x, const TAGPU_COBVM_EFFECT* ef)
{
    CvReplay* rp = (CvReplay*)x;
    CvEntry* e = cvr_take(rp, CV_EFFECT);
    int32_t w[3] = {0, 0, 0};
    unsigned off;
    if (!e) return;
    switch (ef->op) {    /* as cg_commit calls them */
    case 0x10082000u: off = 0x40; w[0] = ef->v[0]; w[1] = ef->v[1]; break;
    case 0x1000F000u: off = 0x30; w[0] = ef->piece; w[1] = ef->v[0]; break;
    case 0x10071000u: off = 0x34; w[0] = ef->piece; w[1] = ef->v[0]; break;
    case 0x10083000u: off = 0x38; w[0] = ef->v[0]; w[1] = ef->v[1]; w[2] = ef->v[2]; break;
    case 0x10084000u: off = 0x3C; w[0] = ef->v[0]; break;
    default: cvr_bad(rp, "the core committed op %08X, no effect the engine makes", ef->op); return;
    }
    if ((unsigned)(unsigned char)e->sub != off || e->a[0] != w[0] || e->a[1] != w[1] || e->a[2] != w[2]) {
        cvr_bad(rp, "pc %d: vt+0x%02X(%d,%d,%d) where the engine's was vt+0x%02X(%d,%d,%d)", ef->pc, off,
                w[0], w[1], w[2], (unsigned char)e->sub, e->a[0], e->a[1], e->a[2]);
        return;
    }
    cvr_nested(rp);
}

static int cvr_unit(void* x, int32_t id, TagpuCobUnitFacts* out)
{
    CvReplay* rp = (CvReplay*)x;
    CvEntry* e = cvr_take(rp, CV_UNIT);
    if (!e) return 0;
    if (e->a[0] != id) { cvr_bad(rp, "the world's unit %d where the engine's asked %d", id, e->a[0]); return 0; }
    if (e->ret) *out = e->facts;
    return e->ret;
}

static int cvr_caller(void* x, TagpuCobUnitFacts* out)
{
    CvEntry* e = cvr_take((CvReplay*)x, CV_CALLER);
    if (!e) return 0;
    if (e->ret) *out = e->facts;
    return e->ret;
}

static int cvr_allied(void* x, uint32_t own, uint32_t owner)
{
    CvReplay* rp = (CvReplay*)x;
    CvEntry* e = cvr_take(rp, CV_ALLIED);
    if (!e) return 0;
    if ((uint32_t)e->a[0] != own || (uint32_t)e->a[1] != owner) {
        cvr_bad(rp, "allied(%u,%u) where the engine's asked allied(%d,%d)", own, owner, e->a[0], e->a[1]);
        return 0;
    }
    return e->ret;
}

static int cvr_local(void* x, uint32_t owner)
{
    CvReplay* rp = (CvReplay*)x;
    CvEntry* e = cvr_take(rp, CV_LOCAL);
    if (!e) return 0;
    if ((uint32_t)e->a[0] != owner) { cvr_bad(rp, "local(%u) where the engine's asked local(%d)", owner, e->a[0]); return 0; }
    return e->ret;
}

static int cv_first(const unsigned char* a, const unsigned char* b, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i]) return (int)i;
    return -1;
}

static void cvr_field(CvReplay* rp, const char* what, unsigned off, const unsigned char* core, const unsigned char* engine)
{
    unsigned w = off & ~3u;
    int32_t cw, ew;
    memcpy(&cw, core + w, 4);
    memcpy(&ew, engine + w, 4);
    cvr_bad(rp, "%s +0x%X: the core 0x%08X, the engine 0x%08X", what, w, (uint32_t)cw, (uint32_t)ew);
}

/* the copy the core ran against the object the engine ran */
static void cvr_compare(CvReplay* rp, char* cob, const unsigned char* copy, unsigned stat, unsigned anim, unsigned model)
{
    const unsigned char* live = (const unsigned char*)cob;
    const unsigned char* lm = *(const unsigned char* const*)(cob + TAGPU_COB_MODEL_OFFSET);
    char what[64];
    int d;
    if ((d = cv_first(copy + 4, live + 4, 4)) >= 0) { cvr_field(rp, "the object", 4u + (unsigned)d, copy, live); return; }
    if ((d = cv_first(copy + 0x18, live + 0x18, TAGPU_COB_MODEL_OFFSET - 0x18u)) >= 0) {
        unsigned off = 0x18u + (unsigned)d;
        if (off >= 0x1Cu && off < TAGPU_COB_COUNT_OFFSET)
            snprintf(what, sizeof what, "record %u", (off - 0x1Cu) / TAGPU_COB_RECORD_BYTES);
        else
            snprintf(what, sizeof what, "%s", off < 0x1Cu ? "the animating flag" : "the count");
        if (off >= 0x1Cu && off < TAGPU_COB_COUNT_OFFSET) {
            unsigned base = 0x1Cu + (off - 0x1Cu) / TAGPU_COB_RECORD_BYTES * TAGPU_COB_RECORD_BYTES;
            cvr_field(rp, what, off - base, copy + base, live + base);
        } else {
            cvr_field(rp, what, 0, copy + (off & ~3u), live + (off & ~3u));
        }
        return;
    }
    if (stat && (d = cv_first(copy + TAGPU_COB_OBJECT_BYTES, *(const unsigned char* const*)(cob + 0x10), stat)) >= 0) {
        cvr_field(rp, "the statics", (unsigned)d, copy + TAGPU_COB_OBJECT_BYTES, *(const unsigned char* const*)(cob + 0x10));
        return;
    }
    if (anim && (d = cv_first(copy + TAGPU_COB_OBJECT_BYTES + stat, *(const unsigned char* const*)(cob + 0x14), anim)) >= 0) {
        snprintf(what, sizeof what, "piece %u's animation", (unsigned)d / (TAGPU_COBVM_ANIM * 4u));
        cvr_field(rp, what, (unsigned)d % (TAGPU_COBVM_ANIM * 4u),
                  copy + TAGPU_COB_OBJECT_BYTES + stat + (unsigned)d / (TAGPU_COBVM_ANIM * 4u) * TAGPU_COBVM_ANIM * 4u,
                  *(const unsigned char* const*)(cob + 0x14) + (unsigned)d / (TAGPU_COBVM_ANIM * 4u) * TAGPU_COBVM_ANIM * 4u);
        return;
    }
    {
        const unsigned char* cm = copy + TAGPU_COB_OBJECT_BYTES + stat + anim;
        if ((d = cv_first(cm, lm, model)) >= 0) {
            unsigned off = (unsigned)d;
            if (off < TAGPU_COBVM_PRIM_BASE) {
                cvr_field(rp, "the model's header", off, cm, lm);
            } else {
                unsigned piece = (off - TAGPU_COBVM_PRIM_BASE) / TAGPU_COBVM_PRIM_BYTES;
                unsigned base = TAGPU_COBVM_PRIM_BASE + piece * TAGPU_COBVM_PRIM_BYTES;
                snprintf(what, sizeof what, "piece %u's PrimitiveStruct", piece);
                cvr_field(rp, what, off - base, cm + base, lm + base);
            }
        }
    }
}

static void cvr_host(CvReplay* rp, char* cob)
{
    char* ta = *(char**)0x00511DE8u;
    const unsigned* file = *(const unsigned**)(cob + 8);
    const unsigned* model = *(const unsigned**)(cob + TAGPU_COB_MODEL_OFFSET);
    rp->world.ctx = rp;
    rp->world.slots = ta ? *(const unsigned short*)(ta + 0x14351) : 0;
    rp->world.script_pieces = file ? file[2] : 0;
    rp->world.model_pieces = model ? model[0] : 0;
    rp->world.unit = cvr_unit;
    rp->world.caller = cvr_caller;
    rp->world.allied = cvr_allied;
    rp->world.local = cvr_local;
    rp->host.ctx = rp;
    rp->host.get = cvr_get;
    rp->host.world = &rp->world;
    rp->host.rand = cvr_rand;
    rp->host.cargo = cvr_cargo;
    rp->host.carrier = cvr_carrier;
    rp->host.alive = cvr_alive;
    rp->host.callback = cvr_callback;
    rp->host.commit = cvr_commit;
}

static void cv_report(char* cob, const CvCall* k, const TagpuCobProgram* prog, CvCtx* c, int bad)
{
    char* ta = *(char**)0x00511DE8u;
    char* unit = cob_caller(ta, cob);
    unsigned tick = ta ? *(const unsigned*)(ta + 0x38A47) : 0;
    const char* script = k->kind == 't' ? "a turn" :
                         k->idx >= 0 && (unsigned)k->idx < prog->scripts ? prog->names[k->idx] : "no script";
    s_cv.calls++;
    if (c->full) {
        s_cv.full[c->full]++;
    } else if (bad) {
        s_cv.different++;
        if (s_cv.reported < 40) {
            s_cv.reported++;
            char kinds[33];
            unsigned i;
            for (i = 0; i < c->n && i < sizeof kinds - 1; i++) kinds[i] = c->log[i].kind;
            kinds[i] = 0;
            tagpu_logf("cobverify: DIFFERENT at tick %u, unit %d, %c call (%.40s), %u entries (%s): %s", tick,
                       unit ? (int)*(const unsigned short*)(unit + 0xA8) : -1, k->kind, script, c->n, kinds, c->why);
        }
    } else {
        unsigned i;
        const char* at;
        s_cv.equal++;
        for (i = 0; i < c->n; i++)
            if (c->log[i].kind == CV_EFFECT) s_cv.effects[((unsigned char)c->log[i].sub - 0x30u) / 4u & 7u]++;
            else if ((at = strchr(CV_KINDS, c->log[i].kind)) != NULL) s_cv.seen[at - CV_KINDS]++;
    }
    if (tick / 300u != s_cv.period) {
        s_cv.period = tick / 300u;
        tagpu_logf("cobverify: tick %u: %u calls, %u equal, %u different, %u not compared (%u past %u entries, "
                   "%u past %u model words, %u at a RETURN's stack out of bounds), "
                   "%u unverified (another thread's, or past the depth)", tick,
                   s_cv.calls, s_cv.equal, s_cv.different,
                   s_cv.full[CV_FULL_LOG] + s_cv.full[CV_FULL_MODEL] + s_cv.full[CV_FULL_STACK],
                   s_cv.full[CV_FULL_LOG], CV_LOG, s_cv.full[CV_FULL_MODEL], CV_DELTA, s_cv.full[CV_FULL_STACK],
                   (unsigned)s_cv.foreign);
        tagpu_logf("cobverify: tick %u: the equal calls carried %u stock getters, %u world units, %u callers, "
                   "%u allied and %u local tests, %u draws, %u cargo and %u carrier reads, %u completions, "
                   "%u nested entries; effects EMIT_SFX %u, EXPLODE %u, ATTACH %u, DROP %u, SET %u", tick,
                   s_cv.seen[0], s_cv.seen[1], s_cv.seen[2], s_cv.seen[3], s_cv.seen[4], s_cv.seen[5],
                   s_cv.seen[6], s_cv.seen[7], s_cv.seen[8], s_cv.seen[9], s_cv.effects[0], s_cv.effects[1],
                   s_cv.effects[2], s_cv.effects[3], s_cv.effects[4]);
    }
}

/* One entry call in the oracle: the engine runs it on the object, the core on a copy of it. */
static int cv_verified(char* cob, const CvCall* k, const char* name, int32_t* const* p)
{
    TAGPU_COBVM* tail = cg_vm(cob);
    const TagpuCobProgram* prog = tail->program;
    unsigned stat = prog->statics * 4u, anim = prog->pieces * TAGPU_COBVM_ANIM * 4u;
    unsigned model = TAGPU_COBVM_MODEL_BYTES(prog->model_pieces);
    CvCtx* outer = cv_top();
    CvCtx* c;
    CvReplay rp;
    TAGPU_COBVM_RUN run;
    unsigned char* copy;
    int32_t io[4];
    int r, cr, i, claimed;
    LONG me = (LONG)GetCurrentThreadId();
    char reason[160];
    if (outer && outer->cob == cob) return cv_nested_engine(outer, cob, k, name, p);
    /* The contexts, the counts and the report belong to one thread from its outermost call's claim
       to the end of that call's replay: only the owner writes `owner` back, so a thread that reads
       its own id owns them, and any other runs unverified. */
    claimed = s_cv.owner != me;
    if (claimed && InterlockedCompareExchange(&s_cv.owner, me, 0) != 0) {
        InterlockedIncrement(&s_cv.foreign);
        tail->quiet = 0;
        return cv_engine(cob, k, name, p);
    }
    if (s_cv.depth >= CV_DEPTH ||
        !(copy = (unsigned char*)tagpu_sim_scratch(TAGPU_COB_OBJECT_BYTES + stat + anim + model))) {
        if (claimed) InterlockedExchange(&s_cv.owner, 0);
        InterlockedIncrement(&s_cv.foreign);
        tail->quiet = 0;
        return cv_engine(cob, k, name, p);
    }
    memcpy(copy, cob, TAGPU_COB_OBJECT_BYTES);
    if (stat) memcpy(copy + TAGPU_COB_OBJECT_BYTES, *(const void* const*)(cob + 0x10), stat);
    if (anim) memcpy(copy + TAGPU_COB_OBJECT_BYTES + stat, *(const void* const*)(cob + 0x14), anim);
    memcpy(copy + TAGPU_COB_OBJECT_BYTES + stat + anim, *(const void* const*)(cob + TAGPU_COB_MODEL_OFFSET), model);
    c = &s_cv.ctx[s_cv.depth++];
    c->cob = cob;
    c->in_host = c->full = 0;
    c->n = c->at = c->nd = 0;
    c->why[0] = 0;
    r = cv_engine(cob, k, name, p);
    cv_refused(c, k, r);
    s_cv.depth--;
    memset(&rp, 0, sizeof rp);
    rp.c = c;
    if (!c->full) {
        cvr_host(&rp, cob);
        if (!tagpu_cobvm_bind(&rp.vm, prog, TAGPU_COBVM_IMPURE, copy,
                              stat ? (int32_t*)(void*)(copy + TAGPU_COB_OBJECT_BYTES) : NULL,
                              anim ? (int32_t*)(void*)(copy + TAGPU_COB_OBJECT_BYTES + stat) : NULL,
                              copy + TAGPU_COB_OBJECT_BYTES + stat + anim, reason, sizeof reason)) {
            cvr_bad(&rp, "the copy could not be bound: %s", reason);
            tail->quiet = 0;
        } else {
            rp.vm.checked = tail->checked;
            rp.vm.quiet = tail->quiet;
            rp.vm.due = tail->due;
            memset(&run, 0, sizeof run);
            run.host = &rp.host;
            memcpy(io, k->io, sizeof io);
            cr = cv_core(&rp.vm, &run, k, io);
            if (cr < 0) cvr_bad(&rp, "the core refused: %s", run.reason);
            else if (k->kind != 't' && cr != r) cvr_bad(&rp, "the core answered %d, the engine %d", cr, r);
            if (k->kind == 'q' && r == 1 && cr == 1)
                for (i = 0; i < 4; i++)
                    if (p[i] && io[i] != *p[i]) {
                        cvr_bad(&rp, "the query's word %d: the core %d, the engine %d", i, io[i], *p[i]);
                        break;
                    }
            if (c->at != c->n)
                cvr_bad(&rp, "the engine logged %u entries the core never asked (the next %c)", c->n - c->at,
                        c->log[c->at].kind);
            tagpu_cobvm_settle(&rp.vm);
            if (!rp.bad) cvr_compare(&rp, cob, copy, stat, anim, model);
            tail->quiet = rp.bad ? 0 : rp.vm.quiet;
            tail->due = rp.vm.due;
        }
    }
    if (c->full) tail->quiet = 0;
    cv_report(cob, k, prog, c, rp.bad);
    tagpu_sim_scratch_free(copy);
    if (claimed) InterlockedExchange(&s_cv.owner, 0);
    return r;
}

static void* cv_trampoline(unsigned va, const unsigned char* stolen)
{
    unsigned char* t = tagpu_detour_stub();
    if (!t) return NULL;
    memcpy(t, stolen, 5);
    t[5] = 0xE9;
    tagpu_detour_rel(t + 6, va + 5);
    return t;
}

static void cv_install(const unsigned char* by_name, const unsigned char* by_idx, const unsigned char* tick)
{
    static const unsigned char ret_stolen[5] = {0x8B, 0x4E, 0x20, 0x85, 0xC9};   /* 0x4B19D0 */
    static const unsigned off[7] = {0x30, 0x34, 0x38, 0x3C, 0x40, 0x48, 0x4C};
    static const unsigned stock[7] = {0x480EB0u, 0x481140u, 0x481340u, 0x4813B0u, 0x480B20u, 0x481430u, 0x481470u};
    const void* thunk[7] = {(const void*)cv_vt30, (const void*)cv_vt34, (const void*)cv_vt38, (const void*)cv_vt3c,
                            (const void*)cv_vt40, (const void*)cv_vt48, (const void*)cv_vt4c};
    const unsigned* vt = (const unsigned*)0x004FD698u;
    int i;
    if (tagpu_cobtrace_armed()) {
        tagpu_log("cobverify: NOT armed - tagpu_cobtrace.on holds the sites it needs; the game runs the core");
        return;
    }
    for (i = 0; i < 7; i++)
        if (vt[off[i] / 4] != stock[i]) {
            tagpu_logf("cobverify: NOT armed - the COB vtable's slot +0x%02X is not stock; the game runs the core", off[i]);
            return;
        }
    if (!tagpu_detour_redirect_ok(0x004B15E0u, 0x004B6C30u) || !tagpu_detour_bytes_ok(0x004B19D0u, ret_stolen, 5)) {
        tagpu_log("cobverify: NOT armed - RAND's call 0x4B15E0 or RETURN 0x4B19D0 is not stock; the game runs the core");
        return;
    }
    s_cvEntry[0] = cv_trampoline(0x004B0940u, by_name);
    s_cvEntry[1] = cv_trampoline(0x004B0B00u, by_idx);
    s_cvEntry[2] = cv_trampoline(0x004B0C40u, by_idx);
    s_cvEntry[3] = cv_trampoline(0x004B0D60u, tick);
    for (i = 0; i < 4; i++)
        if (!s_cvEntry[i]) { tagpu_log("cobverify: NOT armed - no stub memory; the game runs the core"); return; }
    for (i = 0; i < 7; i++) s_cvVt[off[i] / 4] = (void*)(uintptr_t)stock[i];
    for (i = 0; i < 7; i++)
        if (!tagpu_detour_write(0x004FD698u + off[i], (const unsigned char*)&thunk[i], 4)) {
            tagpu_log("cobverify: NOT armed - a vtable slot could not be written; the game runs the core");
            return;
        }
    if (!tagpu_detour_redirect(0x004B15E0u, 0x004B6C30u, (void*)cv_rand) ||
        !tagpu_detour_observe(0x004B19D0u, ret_stolen, 5, cv_return, NULL)) {
        tagpu_log("cobverify: NOT armed - RAND's call or RETURN could not be hooked; the game runs the core");
        return;
    }
    s_cvOn = 1;
    tagpu_log("cobverify: ARMED - the engine's interpreter runs the game, and the core replays each of its entry "
              "calls on a copy of the object");
}

SIM_FEATURE(cob_game_entries, SIM_PUPPET_RUNS, "slice-walkers battle tenseat codec-decline cob-getters resource-core-upgrade stock-resource-mex contest-waves teleport transport-turn");

/* 0x4B0940(name, cb, now), ret 0xC: the first script of that name (0x4B0957..0x4B0993; none, -1,
   which the allocator refuses), its completion stored, and with `now` every record at dt 0 when one
   is busy, then the stepper; 1 started, 0 refused */
static int __attribute__((thiscall)) cg_start_name(char* cob, const char* name, uint32_t cb, int now)
{
    CobCall k;
    TAGPU_COBVM* vm;
    tagpu_rec_cob_started(cob);
    vm = cg_vm(cob);
    if (s_cvOn) {
        CvCall v = {.kind = 'n', .idx = tagpu_cobvm_index(vm->program, name), .cb = cb, .now = now};
        return cv_verified(cob, &v, name, NULL);
    }
    cg_call(&k, cob, 0x004B09A4u);
    return cg_done_call(cob, &k, tagpu_cobvm_start(vm, &k.run, tagpu_cobvm_index(vm->program, name), cb,
                                                   now, 0, 0, NULL));
}

/* 0x4B0B00(idx, cb, now, argc, a0..a3), ret 0x20: a refusal calls the completion with 0 (0x4B0B11);
   the four words written, sp = argc - 1 */
static int __attribute__((thiscall)) cg_start_idx(char* cob, int idx, uint32_t cb, int now, int argc,
                                                  int32_t a0, int32_t a1, int32_t a2, int32_t a3)
{
    CobCall k;
    TAGPU_COBVM* vm;
    int32_t args[4];
    tagpu_rec_cob_started(cob);
    vm = cg_vm(cob);
    if (s_cvOn) {
        CvCall v = {.kind = 'i', .idx = idx, .cb = cb, .now = now, .argc = argc, .io = {a0, a1, a2, a3}};
        return cv_verified(cob, &v, NULL, NULL);
    }
    cg_call(&k, cob, 0x004B0B0Du);
    if (argc < 0 || argc > 4) {
        /* Only a malformed script-start message reaches this: the 0x10 receiver passes the
           message's byte as argc (0x455544), every other caller 0 to 2. The core would refuse it
           as a fault; it is refused as a full pool refuses, on every peer alike: counted, traced,
           its completion called with 0. */
        cg_alloc(&k, idx, TAGPU_COBVM_FROM_ENGINE);
        if (k.host.trace) {
            TAGPU_COBVM_TRACE t;
            memset(&t, 0, sizeof t);
            t.kind = 'X'; t.tick = k.run.tick; t.slot = -1; t.script = idx; t.pc = -1;
            t.from = TAGPU_COBVM_FROM_ENGINE; t.parent = -1;
            k.host.trace(&k, &t);
        }
        if (cb) cg_done(&k, cb, 0);
        return 0;
    }
    args[0] = a0; args[1] = a1; args[2] = a2; args[3] = a3;
    return cg_done_call(cob, &k, tagpu_cobvm_start(vm, &k.run, idx, cb, now, 1, argc, args));
}

/* 0x4B0C40(idx, &a0..&a3), ret 0x14: a NULL pointer gives 0 and takes nothing back
   (0x4B0C64..0x4B0D11); a refusal touches none (0x4B0C51) */
static int __attribute__((thiscall)) cg_query(char* cob, int idx, int32_t* p0, int32_t* p1, int32_t* p2,
                                              int32_t* p3)
{
    CobCall k;
    TAGPU_COBVM* vm;
    int32_t io[4];
    int r;
    vm = cg_vm(cob);
    if (s_cvOn) {
        int32_t* p[4] = {p0, p1, p2, p3};
        CvCall v = {.kind = 'q', .idx = idx, .mask = (p0 != NULL) | (p1 != NULL) << 1 | (p2 != NULL) << 2 | (p3 != NULL) << 3,
                    .io = {p0 ? *p0 : 0, p1 ? *p1 : 0, p2 ? *p2 : 0, p3 ? *p3 : 0}};
        return cv_verified(cob, &v, NULL, p);
    }
    cg_call(&k, cob, 0x004B0C4Du);
    io[0] = p0 ? *p0 : 0; io[1] = p1 ? *p1 : 0; io[2] = p2 ? *p2 : 0; io[3] = p3 ? *p3 : 0;
    r = cg_done_call(cob, &k, tagpu_cobvm_query(vm, &k.run, idx, io));
    if (r == 1) {
        if (p0) *p0 = io[0];
        if (p1) *p1 = io[1];
        if (p2) *p2 = io[2];
        if (p3) *p3 = io[3];
    }
    return r;
}

/* 0x4B0D60(dt), ret 4: every record with dt, then the stepper */
static void __attribute__((thiscall)) cg_tick(char* cob, int32_t dt)
{
    CobCall k;
    TAGPU_COBVM* vm = cg_vm(cob);
    if (s_cvOn) {
        CvCall v = {.kind = 't', .dt = dt};
        cv_verified(cob, &v, NULL, NULL);
        return;
    }
    if (tagpu_cobvm_rest(vm, dt)) return;  /* nothing due: no host to build, no getter world to fill */
    cg_call(&k, cob, 0);
    cg_done_call(cob, &k, tagpu_cobvm_tick(vm, &k.run, dt));
}

/* cob_load wrote the records: the proof covers them, or every opcode of this object is checked */
static void cob_game_loaded(char* cob)
{
    tagpu_cobvm_loaded(cg_vm(cob));
}

static void cob_game_settle(char* cob)
{
    tagpu_cobvm_settle(cg_vm(cob));
}

static void cob_game_sites(void)
{
    static const unsigned char alloc[] = {0xE8, 0x93, 0xF1, 0x02, 0x00};     /* call 0x4B4F10 */
    static const unsigned char by_name[] = {0x51, 0x53, 0x55, 0x56, 0x57};
    static const unsigned char by_idx[] = {0x8B, 0x44, 0x24, 0x04, 0x57};
    static const unsigned char tick[] = {0x53, 0x8B, 0x5C, 0x24, 0x08};
    static const unsigned char construct[] = {0xE8, 0x83, 0xA8, 0x02, 0x00};  /* call 0x4B0610 */
    static const unsigned char release[] = {0xE8, 0xDB, 0xF0, 0x02, 0x00};    /* call 0x4B4F20 */
    unsigned char* stub = lim_code(16);
    if (!stub) return;
    stub[0] = 0x56;                                         /* push esi: the unit (0x485D48) */
    stub[1] = 0xFF; stub[2] = 0x74; stub[3] = 0x24; stub[4] = 0x08;   /* push [esp+8]: the size */
    stub[5] = 0xE8; tagpu_detour_rel(stub + 6, (unsigned)(size_t)cob_block_alloc);
    stub[10] = 0x83; stub[11] = 0xC4; stub[12] = 0x08;      /* add esp,8 */
    stub[13] = 0xC3;                                        /* ret */
    lim_branch(0x00485D78u, sizeof alloc, alloc, 0xE8, (unsigned)(size_t)stub,
               "COB object allocation: the block, or the engine's object zeroed for the host's tail");
    if (s_cobBlock) {
        lim_branch(0x00485D88u, sizeof construct, construct, 0xE8, (unsigned)(size_t)cob_block_construct,
                   "COB object constructor: the block's");
        lim_branch(0x00485E40u, sizeof release, release, 0xE8, (unsigned)(size_t)cob_block_free,
                   "COB object free: the block's");
    }
    lim_branch(0x004B0940u, sizeof by_name, by_name, 0xE9, (unsigned)(size_t)cg_start_name,
               "COB start by name: the game's VM");
    lim_branch(0x004B0B00u, sizeof by_idx, by_idx, 0xE9, (unsigned)(size_t)cg_start_idx,
               "COB start by index: the game's VM");
    lim_branch(0x004B0C40u, sizeof by_idx, by_idx, 0xE9, (unsigned)(size_t)cg_query,
               "COB query: the game's VM");
    lim_branch(0x004B0D60u, sizeof tick, tick, 0xE9, (unsigned)(size_t)cg_tick,
               "COB DoScriptsNow: the game's VM");
    if (!s_cobBlock) cv_install(by_name, by_idx, tick);     /* the lever, read once (cob_layout_choose) */
}

SIM_FEATURE(limits_sites, SIM_PUPPET_RUNS, "battle");

/* A MOD WITH MORE UNIT TYPES THAN THE MASKS HOLD. Called from the menu-time count (0x42AA65),
   on the main thread, before the def array is allocated: nothing has been loaded that a
   half-run game could use, and every later count comes from this array (the game load compacts
   it, 0x42D542). Refusing is the owner's decision: dropping types would depend on the order
   the engine walks the archives, and two peers with different files would drop different
   ones. Never returns. */
static void __cdecl lim_types_refused(unsigned int slots)
{
    static char text[2048];
    SIM_VAR(text, SIM_TRANSIENT);
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
    tagpu_rec_exit();   /* the fork's own ExitProcess does not pass through its hook */
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

    cob_layout_choose();
    cob_get_sites();
    cob_runtime_sites();
    {
        static const unsigned char load[5] = { 0xE8, 0x4A, 0xA2, 0xFF, 0xFF };
        lim_branch(0x00497581u, 5, load, 0xE8, (unsigned)(size_t)cob_load_game_data,
                   "saved unit partition before allocation");
    }

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
       slots), not the slots' end, and stay as they are. The level load's clear of the slots
       at 0x42090A is the effect pools' fix in both builds (fix_effect_pools), which clears
       s_psys here. */
    {
        static const unsigned char push[5] = { 0x68, 0xA0, 0x86, 0x01, 0x00 };
        static const unsigned char op = 0x68;
        lim_op(0x004208FB, 5, push, &op, 1, 100000u * TAGPU_LIM_PSYS / 100u,
               "flying-piece backing bytes");
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
    /* The ceiling pair stands alone: Impure's own value goes in over whatever the exe file
       holds, before any code reads it, and every bound above rests on TAGPU_LIM_UNITS, not on
       the file. A mod's number here is the one its own runtime replaces: TA Zero Alpha 5's exe
       holds 5000 at both (DISASSEMBLED), and TADR writes all three sites from its ini's
       UnitLimit at every launch (LimitCrack.cpp), which TA Zero ships as 1500 in TAZero.ini
       and in its 2025 build's tazero.ini.tdraw, the latter's range 20..1500. */
    lim_file_ok(0x00491659);
    lim_file_ok(0x00491666);
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
    /* Nothing is sized or indexed by it and no stub reads it, so a mod's number in its exe
       file is replaced like stock's: the 3.9.02 exe holds 66650, which is what Impure writes
       anyway, and Escalation's holds 1114, which its TADR replaces with TAESC.ini's
       AISearchMapEntries=66650 at every launch (compat/takeover.md, part 2). */
    lim_file_ok(0x0040EAD6);

    /* ---- particles: two ceilings. Every emitter (0x470F00..0x472F00) takes its layer's
       size and `cmp eax,0x190 / jbe append`; past the cap it destroys the layer's oldest
       object and appends anyway, so a layer holds TAGPU_LIM_SFX + 1. Nineteen compares are
       `3D imm32` (operand at +1), one is `81 F9 imm32` against ecx (operand at +2): the
       twenty are every 0x190 compare in the emitters. The objects come from ONE pool,
       0x51E610, built by the C runtime's static initializer at 0x471C80 (`push 0x4C; push
       0x3E8; ... call 0x470A90`), which allocates its whole capacity once; its alloc 0x470EB0
       returns 0 when the pool is empty and every emitter then skips the particle. DllMain
       runs before that initializer, so the pool is built at the raised capacity. Visual
       only: the emitters draw the particles' own keyed rule (*The particles' own random rule*;
       effects.md 7.7), never a CRT stream or the simulation's generator. */
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
        SIM_VAR(w, SIM_INSTALL);
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

/* the stock build installs no read hook, so it holds no validated program */
const struct TagpuCobProgram* tagpu_cob_def_program(const char* def)
{
    (void)def;
    return NULL;
}

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

int tagpu_limits_in(void)
{
    return s_limState > 0;
}

static int lim_install(void);
int tagpu_limits_install(void)
{
    int in;
    if (s_limState) return s_limState > 0;
    in = lim_install();
    pevt_report();
    return in;
}

static int lim_install(void)
{
    int i, bad = 0, written, haveFile, noFile = 0, marked = 0, modBad = 0;
#ifndef TAGPU_LIMITS_STOCK
    lim_sites();
#endif
    tagpu_logf("enginefix: the fixes' and limits' stubs take %u bytes in %u page(s) of 4096",
               s_fixBytes, s_fixPages);
    if (s_limNoStub) { s_limState = -1; plog("limits: FAILED -- a code stub could not be made"); return 0; }
    if (s_limOverflow) { s_limState = -1; plog("limits: FAILED -- the site table is too small"); return 0; }
    if (s_limMarkMissing) {
        s_limState = -1;
        tagpu_logf("limits: FAILED -- 0x%08X is marked as standing alone and is not in the "
                   "table; nothing written", s_limMarkMissing);
        return 0;
    }
    if (s_limNeeds[0]) {
        s_limState = -1;
        tagpu_logf("limits: FAILED -- %s; nothing written", s_limNeeds);
        return 0;
    }
    if (lim_overlap()) {
        s_limState = -1;
        tagpu_logf("limits: FAILED -- the sites at 0x%08X and 0x%08X overlap", s_limOverlapA,
                   s_limOverlapB);
        return 0;
    }

    /* THE EXE FILE ON DISK IS THE REFERENCE (research/notes/compat/takeover.md, part 2). A site
       whose bytes in memory differ from the FILE was rewritten by something that ran before
       Impure, which is what this refuses over. A site whose FILE differs from stock 3.1 is the
       MOD'S OWN CHANGE -- a mod ships its engine changes in its exe -- and is refused just the
       same, because Impure's value there belongs to a fix that argues from the stock bytes,
       EXCEPT where lim_file_ok says the value stands alone: there the difference is accepted and
       Impure's own value is installed, as it is on every other exe. Comparing with baked stock
       bytes alone could not tell a rewrite from a mod's own exe, and refused Escalation's at its
       first changed site. */
    haveFile = tagpu_takeover_file_open();
    for (i = 0; i < s_nlim; i++) {
        LIMSITE* s = &s_lim[i];
        s->unreadable = !lim_read(s->va, s->have, s->n);
        memcpy(s->want, s->ours, s->n);
        if (!haveFile || !tagpu_takeover_file_at(s->va, s->file, s->n)) {
            /* No reference: fall back to the stock bytes, which is the rule as it was. The log
               says so, since a mod's exe then refuses at its first changed site. */
            memcpy(s->file, s->stock, s->n);
            noFile++;
        }
        if (s->unreadable || memcmp(s->have, s->file, s->n)) {
            s->differs = 1;
            bad++;
            continue;
        }
        if (!memcmp(s->file, s->stock, s->n)) continue;     /* stock: ours goes in */
        if (!s->fileok) {
            s->differs = 1;
            bad++;
            modBad++;
            continue;
        }
        marked++;                              /* stands alone: ours goes in over it */
    }
    tagpu_takeover_file_close();
    if (noFile)
        tagpu_logf("limits: the exe file could not be read for %d of %d sites -- those are "
                   "compared with the stock 3.1 bytes, so a mod's own exe refuses there", noFile,
                   s_nlim);
    if (bad) {
        s_limState = -1;
        tagpu_logf("limits: FAILED -- %d of %d sites differ from the exe file%s, nothing "
                   "written; the report shows at the first DirectDraw call", bad, s_nlim,
                   modBad ? " or are the mod's own change at a site Impure's own value needs"
                          : "");
        return 0;
    }
    if (marked)
        tagpu_logf("limits: %d site(s) hold a value of the mod's own in its exe file, at sites "
                   "where the value stands alone -- Impure's own goes in over them", marked);
    for (written = 0; written < s_nlim; written++) {
        LIMSITE* s = &s_lim[written];
        if (!tagpu_detour_write(s->va, s->want, s->n)) break;
    }
    if (written < s_nlim) {
        /* PUT BACK WHAT WAS WRITTEN: the process ends at the report either way, but
           nothing runs meanwhile on a half-changed engine. */
        s_limWriteFail = s_lim[written].va;
        while (written-- > 0)
            tagpu_detour_write(s_lim[written].va, s_lim[written].file, s_lim[written].n);
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

int tagpu_limits_weapon_slots(void) { return wpn_slots(); }

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

/* 1 when a differing site was refused as the mod's own change: memory holds what the file
   holds, and the file is not stock 3.1 */
static int lim_mod_change(const LIMSITE* s)
{
    return !s->unreadable && !memcmp(s->have, s->file, s->n) && memcmp(s->file, s->stock, s->n);
}

/* THE SAFETY NET (research/notes/compat/takeover.md, part 3): every site of the table re-read
   at the first DirectDraw call, which comes after every DLL's DllMain -- the exe's entry point
   runs after all of them, and its WinMain makes the call. A site that no longer holds our
   bytes was rewritten by a patcher that started after Impure (a pre-2026 TADR's limit crack
   rewrote 17 beside Total Mayhem 11.3.0, MEASURED 2026-09-26, and crashes the first skirmish
   load); stopping here, before any game exists,
   turns that into a refusal. A write made after this call is outside it. s_limState stays 1:
   the raised pools stay the ones the engine code was pointed at while the report runs. */
static void lim_verify(void)
{
    int i;
    for (i = 0; i < s_nlim; i++) {
        LIMSITE* s = &s_lim[i];
        s->unreadable = !lim_read(s->va, s->have, s->n);
        if (s->unreadable || memcmp(s->have, s->want, s->n)) {
            s->differs = 1;
            s_limRewritten++;
        }
    }
    if (s_limRewritten)
        tagpu_logf("limits: FAILED -- %d of %d sites were rewritten after Impure installed them",
                   s_limRewritten, s_nlim);
}

void tagpu_limits_report(void)
{
    static char text[8192];
    static LONG once, verified;
    SIM_VAR(text, SIM_TRANSIENT);
    SIM_VAR(once, SIM_INSTALL);
    SIM_VAR(verified, SIM_INSTALL);
    char exe[MAX_PATH], md5[33] = "unknown", line[768], want[3 * LIM_MAXB + 8],
         have[3 * LIM_MAXB + 8];
    const char* base;
    const char* known = "none";
    const char* why;
    unsigned long size = 0;
    DWORD stamp = 0;
    int i, bad = 0, shown = 0;
    HMODULE me = GetModuleHandleA(NULL);

    if (!InterlockedExchange(&verified, 1) && s_limState > 0) lim_verify();
    if (s_limState >= 0 && !s_limRewritten) return;
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
    if (s_limRewritten)
        why = "Impure installed them, but another program that started after it -- usually "
              "TADR's tdraw.dll, or a copy of it under another name -- rewrote some of those "
              "places, and the game would crash as soon as a battle loads.";
    else if (s_limNoStub || s_limOverflow || s_limOverlapA || s_limMarkMissing)
        why = "Impure failed on its own side before it compared anything: this is a bug in "
              "Impure, or the system is out of memory, not a problem with this TotalA.exe.";
    else if (s_limNeeds[0])
        why = "Part of Impure could not be installed on this TotalA.exe (the log says why), and "
              "a change that keeps every player's game the same relies on it.";
    else if (s_limWriteFail)
        why = "Windows refused to let Impure change the game's code in memory.";
    else if (strcmp(known, "none"))
        why = "This TotalA.exe IS the 3.1 that Impure is built for, but something else "
              "-- another patch or loader -- changed those places in memory before "
              "Impure ran.";
    else
        why = "This TotalA.exe is not the Total Annihilation 3.1 that Impure is built for.";

    _snprintf(text, sizeof text,
        "%s\r\n"
        "\r\n"
        "WHY\r\n"
        "Impure raises the game's limits (units, projectiles, explosions...) and fixes "
        "some of its defects by rewriting its code in memory, and it checks every place "
        "first. %s Running "
        "anyway would let this game play by different rules from other players and "
        "break multiplayer without warning.\r\n"
        "\r\n"
        "WHAT TO DO\r\n"
        "%s"
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
        s_limRewritten ? "Impure's engine limits and fixes were changed by another program, so "
                         "Total Annihilation will now close before a battle can crash."
                       : "Impure could not install its engine limits and fixes, so Total "
                         "Annihilation will now close. Nothing was changed.",
        why,
        s_limRewritten ? "- Take the other patcher out of the game folder: TADR's tdraw.dll, or "
                         "a copy of it under another name. If the folder has a file named "
                         "tagpu_takeover.off, delete it: it lets TADR start.\r\n"
                       : "- Use the original 3.1 TotalA.exe (the Steam copy is 3.1). Community "
                         "patches such as 3.9.02 and TA: Escalation ship a modified exe.\r\n",
        GIT_COMMIT, GIT_BRANCH, base, size, md5, (unsigned long)stamp, known);
    text[sizeof text - 1] = 0;

    if (s_limNoStub)
        _snprintf(line, sizeof line, "result: a code stub could not be made, nothing written\r\n");
    else if (s_limOverflow)
        _snprintf(line, sizeof line, "result: the site table overflowed, nothing written\r\n");
    else if (s_limOverlapA)
        _snprintf(line, sizeof line, "result: the sites at 0x%08X and 0x%08X overlap, nothing "
                  "written\r\n", s_limOverlapA, s_limOverlapB);
    else if (s_limNeeds[0])
        _snprintf(line, sizeof line, "result: %s, nothing written\r\n", s_limNeeds);
    else if (s_limWriteFail)
        _snprintf(line, sizeof line, "result: write refused at 0x%08X, all put back\r\n",
                  s_limWriteFail);
    else if (s_limRewritten)
        _snprintf(line, sizeof line, "result: %d of %d sites rewritten after they were installed\r\n",
                  bad, s_nlim);
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
        /* THE REFERENCE THAT WAS COMPARED, not the bytes this build was written against: at
           install that is the exe FILE's bytes, and after install the bytes Impure put there.
           Printing stock 3.1 for a mod's exe names a number nothing ever compared with, and the
           reader's first question is which of the two conditions fired. */
        lim_hex(want, s_limRewritten ? s->want : s->file, s->n, 16);
        if (s->unreadable) strcpy(have, "(unreadable)"); else lim_hex(have, s->have, s->n, 16);
        _snprintf(line, sizeof line, "0x%08X %s\r\n  want %s\r\n  have %s\r\n",
                  s->va, s->name, want, have);
        line[sizeof line - 1] = 0;
        strncat(text, line, sizeof text - strlen(text) - 1);
        /* the mod's own change: memory matches the file, so want and have read the same, and
           the difference that refused is the file's from stock 3.1 */
        if (!s_limRewritten && lim_mod_change(s)) {
            lim_hex(want, s->stock, s->n, 16);
            _snprintf(line, sizeof line, "  stock %s (the exe file differs from 3.1 here)\r\n", want);
            line[sizeof line - 1] = 0;
            strncat(text, line, sizeof text - strlen(text) - 1);
        }
    }

    /* every differing site goes to the log, not only the twelve the box has room for */
    tagpu_log("limits: startup failure report follows");
    for (i = 0; i < s_nlim; i++) {
        const LIMSITE* s = &s_lim[i];
        if (!s->differs) continue;
        lim_hex(want, s_limRewritten ? s->want : s->file, s->n, LIM_MAXB);
        if (s->unreadable) strcpy(have, "(unreadable)"); else lim_hex(have, s->have, s->n, LIM_MAXB);
        tagpu_logf("limits:   0x%08X %s want %s have %s", s->va, s->name, want, have);
        if (!s_limRewritten && lim_mod_change(s)) {
            lim_hex(want, s->stock, s->n, LIM_MAXB);
            tagpu_logf("limits:     stock %s: the exe file differs from 3.1 here", want);
        }
    }
    tagpu_refuse(text);
}

SIM_FEATURE(effect_pools, SIM_PUPPET_RUNS, "battle stock-impai-fight");

/* THE EFFECT POOLS START EVERY LEVEL EMPTY.

   WHAT STOCK LEAVES [DISASSEMBLED]. The explosion pool, the debris records and the flying-piece
   slots live for the process: stock's in the main block, which is allocated once (0x41D924..0x41D9D5),
   and in .data; the raised build's in the statics of the limits block. The level load's effects
   reset 0x420620 zeroes the explosion count alone (0x420630), writes some fields of each debris
   record and of its six faces (0x4207FB..0x4208F9) and zeroes the flying-piece slots (0x42090A). So
   the explosion records past the count and the debris records' unwritten bytes keep an earlier
   level's values -- a record's state B sequence (+0x18) naming the table B the teardown frees
   (0x420960, at 0x4209DF), a debris face's texture pointer into a model template the teardown frees
   (0x42DB90) -- and nothing reads them while the explosion cap equals the debris records' count:
   - THE ENGINE'S INVARIANT: the taken debris records (+0 != 0xFF) are at most the explosion records
     below the count whose body (+0) is not NULL. Every place that changes either side keeps it: the
     load zeroes the count (0x420630) and frees every debris record (0x420804); the add starts a
     record with no body (0x420A30, its +0 at 0x420AE6); the shatter counts its record (0x4217B0)
     and takes a debris record (0x421D82) for its body (0x421806); the tick frees a body and nulls
     +0 together (0x4210A3, 0x4210A6); the compaction drops only a record whose body, +0xC and +0x18
     are all NULL (0x4210E6..0x42113E).
   - SO A SHATTER ALWAYS FINDS A FREE DEBRIS RECORD. It scans for one only after its cap test
     `count < 300` (0x42176F), when at most 299 bodies, and so at most 299 taken records, are live
     among 300 (the scan's bound, `cmp eax,0x12C` at 0x4217FB). Its no-free exit (0x421802, the
     NULL body at 0x421806, the leave at 0x421808) is unreachable while the two caps are equal,
     which the _Static_assert below pins: 300 and 300 in stock, TAGPU_LIM_EXPL and TAGPU_LIM_AUX
     (3000 each) raised.
   - SO NO STALE BYTE IS READ. A record past the count comes below it only through the add, which
     writes its +0xC and +0x18 (0x420A85..0x420AB7), or the shatter, which clears them (0x4218F1,
     0x4218F4); the tick and the draw read records below the count alone. A taken debris record is
     rewritten before any draw: its vertices (0x421915..0x42199B, 0x421BB8..0x421C76) and its faces
     (0x421C95..0x421D2F).
   The explosion cap at 0x42176F still decides how many sim-RNG draws a shatter makes when the
   explosion pool is full, from the records this level wrote.

   THE FIX replaces the slots' clear at 0x42090A (`mov ecx,0x64 / xor eax,eax / mov edi,0x511DF0 /
   rep stosd`, 14 bytes), the last thing 0x420620 does before its epilogue, with a call of
   fx_pools_empty and nine NOPs, in both builds. It zeroes the explosion pool whole, count and
   records, and the debris records with their vertex and face blocks, initialises each debris record
   as the engine's loop does (debris_init), and zeroes the flying-piece slots, in the pools the build
   uses. A fresh process holds exactly these bytes there: its main block is zeroed whole by
   `rep stosd` / `rep stosb` (0x41D94C..0x41D95E) before 0x41D9D5 publishes it, and the statics
   start zeroed [DISASSEMBLED; MEASURED 2026-11-13, a fresh process's dump holds 300 zero explosion
   records]. What it buys:
   - every level's pools hold a fresh process's bytes, whatever the process played before;
   - an in-place seek (tagpu_effects_clear) leaves the same pool bytes as a fresh-process seek;
   - the tail past the count is zero rather than holding freed pointers, should the caps ever
     differ.
   A first level is unchanged.

   EVERY LEVEL LOAD REACHES IT. 0x420620's only caller is 0x4919D2 in LoadGameData_Main 0x4917D0;
   that function's only caller is 0x497581 in the loader body 0x497180; its only caller is 0x497CA1
   in the loader thread's procedure 0x497C70, whose address is pushed only at 0x4982C5, in the load
   state's first call of every level (engine/units.md). The game modes branch at 0x4971D8..0x4971E6
   and meet again at 0x4974E3, before 0x497581; a saved game's restore comes after it (0x497B29).
   Neither 0x4917D0 before 0x4919D2 nor 0x420620 before 0x42090A has a return or a branch past it.
   None of the four addresses occurs in the image as a dword, so no indirect call reaches them
   [DISASSEMBLED 2026-11-13, objdump and a byte search of the pristine exe].

   REGISTERS. The epilogue after the site (0x420918: pop edi, esi, ebp, ebx, ecx; ret) restores edi
   and ecx, the two registers the stock clear leaves changed besides eax, and edx is a scratch
   register of the engine's own code. Stock leaves eax = 0 at the return, so the reset returns 0.

   THREADS (an ordering). At a level load the reset runs on the loader thread, inside 0x420620,
   before the loader's last store, bit 1 of main+0x38D75 (0x497C62). Every engine reader and writer
   of the pools runs only in play, on the game thread, after the load state has seen that bit
   (0x498342..0x49834D): the sim step's shatter and explosion tick, run by the in-play entry that
   follows that test (its catch-up ticks, then the in-play handler 0x499200 it installs at
   0x498455), and the receivers of 0x0A..0x10, whose mask 4 the dispatcher admits in net state 6
   alone (0x454762..0x45477D), which the in-play entry sets at 0x498445. At a seek the reset runs on
   the game thread (tagpu_effects_clear), between two sim steps. The frame packet reads the pools on
   the game thread, in the in-play draw alone.

   CLASS: in the fail-closed table. Its absence would change no shared state of a live game in the
   stock build: nothing reads the bytes it clears (WHAT STOCK LEAVES). It is in the table for two
   reasons. In the raised build this call is what initialises the static debris records and clears
   the static flying-piece slots, a raised limit like the rest of the table: without it no static
   debris record reads free (+0 = 0xFF), lim_aux_alloc hands out none, and every shatter takes the
   no-free exit and draws nothing where stock draws. And in both builds every level then starts
   with the pool bytes a fresh process starts with, which the replay's byte comparisons of a later
   level against a fresh process's playing of it assume. */
#define FXP_EXPL      0x1491Bu   /* main+: the explosion count, its 300 records of 0x54 after it  */
#define FXP_DEBRIS    0x1AB9Fu   /* main+: the 300 debris records of 0x34                          */
#define FXP_VERTS     0x1E88Fu   /* main+: their vertex blocks of 0x60, straight after them        */
#define FXP_FACES     0x2590Fu   /* main+: their face blocks of 0xC0, straight after those         */
#define FXP_STOCK     300        /* explosion and debris records                                   */
#define FXP_PIECES    100        /* flying-piece slots at 0x511DF0                                 */

/* The shatter's no-free exit (0x421802..0x421808) is unreachable, and the pools' stale bytes
   unread, only while the explosion cap equals the debris records' count (WHAT STOCK LEAVES). */
_Static_assert(TAGPU_LIM_EXPL == TAGPU_LIM_AUX, "the explosion cap equals the debris records' count");

/* Both are emptied together, so no record names a freed body. */
static int __cdecl fx_pools_empty(void)
{
    int i;
#ifndef TAGPU_LIMITS_STOCK
    memset(&s_expl, 0, sizeof s_expl);
    memset(s_aux, 0, sizeof s_aux);
    memset(s_auxVert, 0, sizeof s_auxVert);
    memset(s_auxFace, 0, sizeof s_auxFace);
    for (i = 0; i < TAGPU_LIM_AUX; i++) debris_init(s_aux[i], s_auxVert[i], s_auxFace[i]);
    memset(s_psys, 0, sizeof s_psys);
#else
    unsigned char* ta = *(unsigned char* const*)0x00511DE8;
    if (!ta) return 0;
    memset(ta + FXP_EXPL, 0, 4 + FXP_STOCK * 0x54);
    memset(ta + FXP_DEBRIS, 0, FXP_STOCK * (0x34 + 0x60 + 0xC0));
    for (i = 0; i < FXP_STOCK; i++)
        debris_init(ta + FXP_DEBRIS + i * 0x34, ta + FXP_VERTS + i * 0x60, ta + FXP_FACES + i * 0xC0);
    memset((void*)(size_t)0x00511DF0u, 0, FXP_PIECES * 4);
#endif
    return 0;
}

static int fix_effect_pools(void)
{
    static const unsigned char clr[14] = { 0xB9, 0x64, 0x00, 0x00, 0x00, 0x33, 0xC0,
                                           0xBF, 0xF0, 0x1D, 0x51, 0x00, 0xF3, 0xAB };
    lim_branch(0x0042090A, (int)sizeof clr, clr, 0xE8, (unsigned int)(size_t)&fx_pools_empty,
               "the effect pools emptied at every level load");
    return FIX_TABLE;
}

/* THE EFFECTS CLEARED at a seek (tagpu_replay.c, seek_run), on the game thread between two sim
   steps, after the rebuild's kills and creates: what was in flight before the seek and what the
   creates' Create scripts made both go, and nothing that names a unit outlives the kills.
   - The particles: each of the 10 layers of *(main+0x38D77), 16 bytes each, its vector from +4
     to +8: every object destroyed by its own [vtable+0](1) and removed from the front, as the
     teardown 0x471DE0's first loop does (DISASSEMBLED 0x471E0B..0x471E5A); the layer's storage
     is kept (its second loop, which frees it, is not run).
   - The explosions, the debris records and the flying-piece slots, emptied as a level load empties
     them (fx_pools_empty), after the particles: the pools then hold what a fresh process playing
     the same level would hold at its start, whatever this process played before the seek.
   Not the rest of the load 0x420620: it allocates the explosions' sequence tables and the
   pieces' ring, which mid-level would leak. The ring that owns the pieces' blocks (0x511F80)
   stays consistent: an eviction nulls an owner slot only while it still points at that block
   (0x437A96..0x437A9D, 0x437AC8..0x437ACF), so a block orphaned here is reclaimed later without
   touching a reused slot. 1 cleared, 0 when there is no main block or the raise is not in. */
int tagpu_effects_clear(void)
{
    typedef void (__thiscall* PFN_FxDestroy)(void* object, int flags);
    char* ta = *(char* const*)0x00511DE8;
    unsigned char* layers;
    int i;
    /* the world now stands between two steps, as after the walk of its GameTime: an emitter
       re-fired before the next step is late (tagpu_pevt.h) */
    pevt_clear();
    s_pevt.walked = prand_gametime();
    if (!ta) return 0;
    layers = *(unsigned char* const*)(ta + 0x38D77);
    for (i = 0; layers && i < 10; i++) {
        char** first = (char**)(layers + i * 16 + 4);      /* the layer's vector */
        char** last = (char**)(layers + i * 16 + 8);
        while (*last != *first) {
            void* object = *(void**)*first;
            if (object) ((PFN_FxDestroy)**(void***)object)(object, 1);
            memmove(*first, *first + 4, (size_t)(*last - (*first + 4)));
            *last -= 4;
        }
    }
#ifndef TAGPU_LIMITS_STOCK
    if (s_limState <= 0) return 0;
#endif
    fx_pools_empty();
    return 1;
}

SIM_FEATURE(restore_invariants, SIM_PUPPET_RUNS, "battle");

/* THE INVARIANTS THE REPLAY'S TABLE WRITE CANNOT EXPRESS (tagpu_replay.c, tables_put). A puppet
   host writes this file's declared tables back from a recording's image, by their layouts alone,
   at each capture point, frame visit and seek, on the game thread between two sim steps; these re-establish what the layouts cannot say before anything
   reads the tables again. The air pool's lock is held from tagpu_patches_restore_begin, before the
   first table is written, to the end of tagpu_patches_restored: the write runs C code, HeapAlloc,
   HeapFree and the log sink and no engine code, so the lock covers every read and write of the
   pool and of s_airIn, as *The pool* requires, in the lock order it states. Returns what it
   repaired. */
void tagpu_patches_restore_begin(void)
{
    EnterCriticalSection(&s_airLock);
}

unsigned int tagpu_patches_restored(void)
{
    const char* ta = *(const char* const*)0x00511DE8;
    unsigned int slots = ta ? *(const unsigned short*)(ta + 0x14351) : 0, n, i, k = 0;
    unsigned int repaired = 0;
    /* The air triple: s_airIn exactly the indicator of s_airPool[0, s_airN), so a slot enters the
       pool at most once (air_note's test) and the pool cannot outgrow its rows; every entry a
       slot of the array, which air_unit indexes it with */
    n = s_airN < AIR_SLOTS ? s_airN : AIR_SLOTS;
    if (n > slots) n = slots;
    memset(s_airIn, 0, sizeof s_airIn);
    for (i = 0; i < n; i++) {
        unsigned int slot = s_airPool[i];
        if (slot >= slots || s_airIn[slot]) continue;
        s_airIn[slot] = 1;
        s_airPool[k++] = (unsigned short)slot;
    }
    repaired += s_airN - k;
    s_airN = k;
    LeaveCriticalSection(&s_airLock);
    /* the delay queue's head and count index s_hitDelay raw (hit_delay_flush, the enqueue) */
    if (s_hitDelayHead >= HIT_DELAY_MAX || s_hitDelayCount > HIT_DELAY_MAX) {
        s_hitDelayHead = s_hitDelayCount = 0;
        repaired++;
    }
    /* ghost_sweep reads down from s_ghostDone[s_ghostDoneN - 1] unguarded */
    if (s_ghostDoneN > GHOST_DONE) {
        s_ghostDoneN = 0;
        repaired++;
    }
    return repaired;
}


SIM_FEATURE(fp_exact, SIM_PUPPET_RUNS, "fp-verify stock-fp-verify battle slice-walkers stock-resource-mex");

/* THE PIECE ROTATION FROM A TABLE (research/notes/engine/cob.md, the piece matrix). 0x4B7173(angle,
   pair), cdecl, rotates the int pair at `pair` by the signed 16-bit angle word, the low word of its
   first argument: it returns at once on a zero word (0x4B7176..0x4B717B), else loads p1, p0 and a
   copy of each (0x4B717D..0x4B7188), the angle in radians (fild word, fmul qword [0x509EF8]) and its
   cosine and sine (fsincos, 0x4B7189..0x4B7193), and from those the products, sums and two bare fistp
   (0x4B7194..0x4B71A4). It preserves every register but eax. Its fsincos sees only 65 536 inputs.
   THE INVARIANT: a call's result is, bit for bit, the one the engine's code computes on the same
   machine. The table holds, for every angle word, the 80-bit cosine and sine that the running CPU's
   fsincos returns on fild word; fmul qword [0x509EF8] under the control word s_rotCw, computed by
   those instructions at install (rot_emit_build). A call under s_rotCw loads the pair from the table and
   runs the engine's own instructions around it, copied from the exe after byte-matching all of it;
   a call under any other control word runs the engine's code. The fmul's precision and rounding are
   the control word's and fsincos's result the CPU's, both the same for an entry as for the engine's
   call, and fld of a stored 80-bit value is exact. Nothing is shipped precomputed: x87 fsincos
   differs between CPU makes, so a table made elsewhere could disagree with the running CPU's engine.
   One machine can hold processors of more than one kind (performance and efficiency cores), whose
   fsincos nothing documents as equal; the table is therefore built on every processor of the machine,
   each pinned in turn, and installed only when every one fills it identically
   (rot_build_everywhere). Where they differ the engine's code stays, and with it stock's own
   dependence on which core ran the call.
   `tagpu_fpverify.on` at launch runs both on every call and counts the calls whose results differ
   (rot_verify): it must stay 0. */
#define ROT_VA       0x004B7173u
#define ROT_AFTER    0x004B717Bu    /* the je after the zero-angle compare */
#define ROT_LOADS    0x004B717Du    /* mov eax,[ebp+0xC] .. fld st(1): 12 bytes */
#define ROT_SINCOS   0x004B7189u    /* fild word, fmul, fsincos: 11 bytes the table replaces */
#define ROT_TAIL     0x004B7194u    /* the products and the stores: 17 bytes */
#define ROT_END      0x004B71A7u
typedef struct RotEntry { unsigned char cos[10], sin[10]; } RotEntry;
static RotEntry s_rot[65536];
SIM_ROWS(s_rot, SIM_INSTALL, SIM_ID_NONE, NULL);
#define ROT_CHUNK 1024
static RotEntry s_rotCheck[ROT_CHUNK];  /* another processor's fill of one chunk, compared to s_rot */
SIM_ROWS(s_rotCheck, SIM_INSTALL, SIM_ID_NONE, NULL);
static uint16_t s_rotCw;
SIM_VAR(s_rotCw, SIM_INSTALL);
static unsigned char* s_rotFast;       /* the table's entry, which checks the control word */
SIM_CODE(s_rotFast);
static unsigned char* s_rotOrig;       /* the engine's code: its prologue and compare, then 0x4B717B */
SIM_CODE(s_rotOrig);
static unsigned char* s_rotCode;       /* the builder, then the verify entry */
SIM_CODE(s_rotCode);
static struct { volatile LONG calls, zero, table, differ; } s_rotDiag;
SIM_VAR(s_rotDiag, SIM_PRIVATE);
static int s_fpVerify;                  /* tagpu_fpverify.on, read once by rot_install for both */
SIM_VAR(s_fpVerify, SIM_INSTALL);

typedef void (__cdecl *RotFn)(int angle, int32_t* pair);

/* The engine's function from its own bytes: 0x4B7173..0x4B71A6, every byte, so the copies below are
   the engine's instructions and the slow path's jump lands in code that is still the engine's. */
static const unsigned char ROT_BYTES[] = {
    0x55,0x8B,0xEC,0x66,0x83,0x7D,0x08,0x00,0x74,0x28,0x8B,0x45,0x0C,0xDB,0x40,0x04,0xDB,0x00,0xD9,0xC1,
    0xD9,0xC1,0xDF,0x45,0x08,0xDC,0x0D,0xF8,0x9E,0x50,0x00,0xD9,0xFB,0xDC,0xCD,0xDE,0xCC,0xDC,0xCA,0xDE,
    0xC9,0xDE,0xC3,0xDE,0xE9,0xDB,0x18,0xDB,0x58,0x04,0xC9,0xC3 };
typedef char rot_bytes_whole[(sizeof ROT_BYTES == ROT_END - ROT_VA) ? 1 : -1];

/* The table entry, with `count` (verify mode) incremented on the table path:
     push ebp; mov ebp,esp; push eax; fnstcw [ebp-4]; cmp word [ebp-4],s_rotCw; pop eax; jne slow
     cmp word [ebp+8],0; je done; push ecx; movzx ecx,word [ebp+8]; lea ecx,[ecx+ecx*4]
     <0x4B717D..0x4B7188>; fld tbyte [ecx*4+sin]; fld tbyte [ecx*4+cos]; <0x4B7194..0x4B71A4>; pop ecx
   done: leave; ret
   slow: cmp word [ebp+8],0; jmp 0x4B717B
   The control word goes through eax's slot: eax is cdecl's scratch register, which the engine's
   function writes on every non-zero angle and none of its nine call sites reads after the call;
   pop changes no flag. fsincos leaves the cosine on top and the sine below it, as the two loads do. */
static void rot_emit(unsigned char* p, volatile LONG* count)
{
    unsigned char *jslow, *jdone;
    unsigned at;
    *p++ = 0x55; *p++ = 0x8B; *p++ = 0xEC; *p++ = 0x50;
    *p++ = 0xD9; *p++ = 0x7D; *p++ = 0xFC;
    *p++ = 0x66; *p++ = 0x81; *p++ = 0x7D; *p++ = 0xFC;
    *p++ = (unsigned char)s_rotCw; *p++ = (unsigned char)(s_rotCw >> 8);
    *p++ = 0x58;
    *p++ = 0x75; jslow = p++;
    *p++ = 0x66; *p++ = 0x83; *p++ = 0x7D; *p++ = 0x08; *p++ = 0x00;
    *p++ = 0x74; jdone = p++;
    *p++ = 0x51;
    if (count) {
        *p++ = 0xF0; *p++ = 0xFF; *p++ = 0x05;
        memcpy(p, &count, 4); p += 4;
    }
    *p++ = 0x0F; *p++ = 0xB7; *p++ = 0x4D; *p++ = 0x08;
    *p++ = 0x8D; *p++ = 0x0C; *p++ = 0x89;
    memcpy(p, ROT_BYTES + (ROT_LOADS - ROT_VA), ROT_SINCOS - ROT_LOADS); p += ROT_SINCOS - ROT_LOADS;
    *p++ = 0xDB; *p++ = 0x2C; *p++ = 0x8D;
    at = (unsigned)(uintptr_t)s_rot[0].sin; memcpy(p, &at, 4); p += 4;
    *p++ = 0xDB; *p++ = 0x2C; *p++ = 0x8D;
    at = (unsigned)(uintptr_t)s_rot[0].cos; memcpy(p, &at, 4); p += 4;
    memcpy(p, ROT_BYTES + (ROT_TAIL - ROT_VA), ROT_END - 2 - ROT_TAIL); p += ROT_END - 2 - ROT_TAIL;
    *p++ = 0x59;
    *jdone = (unsigned char)(p - (jdone + 1));
    *p++ = 0xC9; *p++ = 0xC3;
    *jslow = (unsigned char)(p - (jslow + 1));
    *p++ = 0x66; *p++ = 0x83; *p++ = 0x7D; *p++ = 0x08; *p++ = 0x00;
    *p++ = 0xE9; tagpu_detour_rel(p, ROT_AFTER);
}

/* The engine's code as an entry of its own: its prologue and zero-angle compare, then 0x4B717B. */
static void rot_emit_orig(unsigned char* p)
{
    memcpy(p, ROT_BYTES, ROT_AFTER - ROT_VA); p += ROT_AFTER - ROT_VA;
    *p++ = 0xE9; tagpu_detour_rel(p, ROT_AFTER);
}

/* The builder, cdecl (RotEntry* out, const uint16_t* cw, unsigned first, unsigned end): under *cw, for
   the angle words first..end-1, the engine's fild word; fmul qword [0x509EF8]; fsincos, the cosine and
   then the sine stored whole from out on:
     push ebp; mov ebp,esp; push ebx; push ecx; push ecx; fnstcw [ebp-8]; mov eax,[ebp+0xC]; fldcw [eax]
     mov ebx,[ebp+8]; mov ecx,[ebp+0x10]
   loop: mov [ebp-0xC],cx; fild word [ebp-0xC]; fmul qword [0x509EF8]; fsincos
     fstp tbyte [ebx]; fstp tbyte [ebx+10]; add ebx,20; inc ecx; cmp ecx,[ebp+0x14]; jb loop
     fldcw [ebp-8]; pop ecx; pop ecx; pop ebx; leave; ret */
static void rot_emit_build(unsigned char* p)
{
    static const unsigned char head[] = { 0x55,0x8B,0xEC,0x53,0x51,0x51,0xD9,0x7D,0xF8,0x8B,0x45,0x0C,
                                          0xD9,0x28,0x8B,0x5D,0x08,0x8B,0x4D,0x10 };
    static const unsigned char body[] = { 0x66,0x89,0x4D,0xF4,0xDF,0x45,0xF4,0xDC,0x0D,0xF8,0x9E,0x50,0x00,
                                          0xD9,0xFB,0xDB,0x3B,0xDB,0x7B,0x0A,0x83,0xC3,0x14,0x41,
                                          0x3B,0x4D,0x14,0x72 };
    static const unsigned char tail[] = { 0xD9,0x6D,0xF8,0x59,0x59,0x5B,0xC9,0xC3 };
    memcpy(p, head, sizeof head); p += sizeof head;
    memcpy(p, body, sizeof body); p += sizeof body;
    *p = (unsigned char)(-(int)(sizeof body + 1)); p++;
    memcpy(p, tail, sizeof tail);
}

static void __cdecl rot_verify(int angle, int32_t* pair)
{
    int32_t mine[2];
    LONG n;
    mine[0] = pair[0];
    mine[1] = pair[1];
    ((RotFn)(void*)s_rotFast)(angle, mine);
    ((RotFn)(void*)s_rotOrig)(angle, pair);
    n = InterlockedIncrement(&s_rotDiag.calls);
    if (!(angle & 0xFFFF))
        InterlockedIncrement(&s_rotDiag.zero);
    if (mine[0] != pair[0] || mine[1] != pair[1]) {
        if (InterlockedIncrement(&s_rotDiag.differ) == 1)
            tagpu_logf("fprot: DIFFER: angle 0x%04X gives (%d,%d) from the table, (%d,%d) from the engine",
                      angle & 0xFFFF, mine[0], mine[1], pair[0], pair[1]);
    }
    if (n >= 0x10000 && !(n & (n - 1)))     /* at every power of two from 65 536 */
        tagpu_logf("fprot: %ld calls, %ld of angle 0, %ld on the table, %ld differ", n, s_rotDiag.zero,
                   s_rotDiag.table, s_rotDiag.differ);
}

/* The verify entry, as the engine's function keeps every register but eax: push ecx; push edx;
   push dword [esp+0x10]; push dword [esp+0x10]; call rot_verify; add esp,8; pop edx; pop ecx; ret */
static void rot_emit_verify(unsigned char* p)
{
    static const unsigned char head[] = { 0x51,0x52,0xFF,0x74,0x24,0x10,0xFF,0x74,0x24,0x10,0xE8 };
    static const unsigned char tail[] = { 0x83,0xC4,0x08,0x5A,0x59,0xC3 };
    memcpy(p, head, sizeof head); p += sizeof head;
    tagpu_detour_rel(p, (unsigned)(uintptr_t)rot_verify); p += 4;
    memcpy(p, tail, sizeof tail);
}

typedef void (__cdecl *RotBuild)(RotEntry* out, const uint16_t* cw, unsigned first, unsigned end);

/* s_rot filled on every processor of the machine, the thread pinned to each in turn and confirmed there
   before and after: 1 when every processor's fill equals the first's, else 0 with the reason logged. A
   thread pinned to one processor cannot run on another, so the comparison is between processors and
   not between scheduling accidents.
   Every processor, not the process's affinity mask: dd_CreateEx widens the mask to the system's at the
   first DirectDraw call (singlecpu is off), and a tool can widen it too, so a process started on one core
   still runs on all of them. A narrower mask is widened for the build and restored after it; every later
   mask is a subset of the system's. The system mask names only one processor group, and at most 32
   processors in a 32-bit process, so a machine with more active processors than it names is refused.
   Processors brought online while the game runs are not compared. */
#ifndef ALL_PROCESSOR_GROUPS
#define ALL_PROCESSOR_GROUPS 0xffff
#endif
WINBASEAPI DWORD WINAPI GetActiveProcessorCount(WORD GroupNumber);

static int rot_build_everywhere(void)
{
    const RotBuild build = (RotBuild)(void*)s_rotCode;
    DWORD (WINAPI *const here)(void) = GetCurrentProcessorNumber;
    const HANDLE th = GetCurrentThread();
    DWORD_PTR proc = 0, sys = 0, saved = 0;
    unsigned cpu, n = 0, f;
    int ok = 1;
    unsigned named = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) || !sys) {
        tagpu_log("fprot: NOT installed: the machine's processors could not be read");
        return 0;
    }
    for (cpu = 0; cpu < 8 * sizeof sys; cpu++)
        named += (unsigned)((sys >> cpu) & 1);
    if (GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) != named) {
        tagpu_logf("fprot: NOT installed: %lu active processors, %u in the system mask",
                   (unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), named);
        return 0;
    }
    if (proc != sys && !SetProcessAffinityMask(GetCurrentProcess(), sys)) {
        tagpu_log("fprot: NOT installed: the process could not be widened to every processor");
        return 0;
    }
    for (cpu = 0; ok && cpu < 8 * sizeof sys; cpu++) {
        const DWORD_PTR bit = (DWORD_PTR)1 << cpu;
        DWORD_PTR was;
        int tries;
        if (!(sys & bit))
            continue;
        if (!(was = SetThreadAffinityMask(th, bit))) {
            tagpu_logf("fprot: NOT installed: the thread could not be pinned to processor %u", cpu);
            ok = 0;
            break;
        }
        if (!saved)
            saved = was;
        for (tries = 0; tries < 4 && here() != cpu; tries++)
            Sleep(tries);
        if (here() != cpu) {
            tagpu_logf("fprot: NOT installed: processor %u could not be confirmed", cpu);
            ok = 0;
            break;
        }
        if (!n) {
            build(s_rot, &s_rotCw, 0, 0x10000);
        } else {
            for (f = 0; ok && f < 0x10000; f += ROT_CHUNK) {
                build(s_rotCheck, &s_rotCw, f, f + ROT_CHUNK);
                if (memcmp(s_rotCheck, s_rot + f, sizeof s_rotCheck)) {
                    tagpu_logf("fprot: NOT installed: processor %u's fsincos differs from the first's "
                               "in angles 0x%04X..0x%04X", cpu, f, f + ROT_CHUNK - 1);
                    ok = 0;
                }
            }
        }
        if (ok && here() != cpu) {
            tagpu_logf("fprot: NOT installed: the thread left processor %u", cpu);
            ok = 0;
        }
        n++;
    }
    if (saved)
        SetThreadAffinityMask(th, saved);
    if (proc != sys)
        SetProcessAffinityMask(GetCurrentProcess(), proc);
    if (ok)
        tagpu_logf("fprot: the table is the same on all %u processors", n);
    return ok;
}

static void rot_install(void)
{
    const int verify = s_fpVerify = tagpu_sim_lever("tagpu_fpverify.on");
    unsigned char jmp[5];
    uint16_t cw;
    if (!tagpu_detour_bytes_ok(ROT_VA, ROT_BYTES, (int)sizeof ROT_BYTES)) {
        tagpu_log("fprot: NOT installed: 0x4B7173..0x4B71A6 is not the retail rotation");
        return;
    }
    if (!(s_rotFast = tagpu_detour_stub()) || !(s_rotOrig = tagpu_detour_stub()) ||
        !(s_rotCode = tagpu_detour_stub())) {
        tagpu_detour_stub_free(s_rotFast); tagpu_detour_stub_free(s_rotOrig); tagpu_detour_stub_free(s_rotCode);
        s_rotFast = s_rotOrig = s_rotCode = NULL;
        tagpu_log("fprot: NOT installed: no stub memory");
        return;
    }
    /* The table is made under this thread's control word at install; a call under another word
       takes the engine's code. The word is read by a three-instruction routine after the builder
       (mov eax,[esp+4]; fnstcw [eax]; ret), the builder being 57 bytes. */
    rot_emit_build(s_rotCode);
    {
        static const unsigned char rd[] = { 0x8B,0x44,0x24,0x04,0xD9,0x38,0xC3 };
        memcpy(s_rotCode + 0x60, rd, sizeof rd);
        cw = 0;
        ((void (__cdecl*)(uint16_t*))(void*)(s_rotCode + 0x60))(&cw);
    }
    s_rotCw = cw;
    if (!rot_build_everywhere()) {
        tagpu_detour_stub_free(s_rotFast); tagpu_detour_stub_free(s_rotOrig); tagpu_detour_stub_free(s_rotCode);
        s_rotFast = s_rotOrig = s_rotCode = NULL;
        return;
    }
    rot_emit(s_rotFast, verify ? &s_rotDiag.table : NULL);
    rot_emit_orig(s_rotOrig);
    if (verify) rot_emit_verify(s_rotCode);
    {
        const unsigned target = (unsigned)(uintptr_t)(verify ? s_rotCode : s_rotFast);
        const unsigned rel = target - (ROT_VA + 5u);     /* against the site, not this buffer */
        jmp[0] = 0xE9;
        memcpy(jmp + 1, &rel, 4);
    }
    if (!tagpu_detour_write(ROT_VA, jmp, 5)) {
        tagpu_log("fprot: NOT installed: 0x4B7173 could not be written");
        return;
    }
    tagpu_logf("fprot: installed: the piece rotation 0x4B7173 from a table under control word 0x%04X%s",
              s_rotCw, verify ? " (tagpu_fpverify.on: both run, differences counted)" : "");
}


/* HYPOT WITH ONE MODE SWITCH EACH WAY (research/notes/engine/cob.md, 0x4FB440). The CRT's _hypot
   0x4FB440 calls _hypothlp 0x4FB480(x, y, 0x18), which sets the x87 control word to 0x133F (64-bit
   significand, round to nearest; _ctrlfp 0x4EC4C0 at 0x4FB491), takes |x| and |y| and M, the larger
   (ties take x; 0x4FB4CB..0x4FB52A), then x/M and y/M each stored to a double (0x4FB55B..0x4FB577),
   their squares' sum stored (0x4FB57B..0x4FB58D), its root through the CRT's sqrt 0x4FB7B4 (fsqrt
   under 0x37F, the same precision and rounding) stored, and the product of that root's and M's
   significands stored (0x4FB5B0, 0x4FB5B7) with the exponents added back exactly (0x4EC290,
   0x4EC250); when the caller's word masks the precision exception (0x4FB68A) it restores that word
   and returns the stored double (0x4FB695..0x4FB6A8). Under the game's 0x27F that is four or five
   fldcw a call: the set, sqrt's switch to 0x37F and back (and _87except's when the sticky inexact
   flag is set, 0x4EC947), and the restore.
   THE INVARIANT: the result is, bit for bit, the engine's. hyp_core runs the same operations in the
   same order on the same values under 0x133F, one fldcw each way: each fdiv, fmul, faddp and fsqrt
   rounds to the 64-bit significand and each store to a double, as the engine's do -- IEEE basic
   operations, the same bits on every x87 at one control word. The last step multiplies the root by
   M where the engine multiplies their significands and sets the exponent: scaling by a power of two
   changes no rounding while the result is a normal double, which M within 2^-1000..2^1000 and a root
   in [1, 2) guarantee. Every other input -- both zero, a NaN or infinity, M outside that range, a caller
   whose word unmasks any exception (the engine's -fpufussy does, 0x4DF1B1) -- goes to the engine's
   0x4FB480. The sticky exception flags may then differ from the engine's, whose sqrt can take the
   CRT's report path: nothing reads them for a value. Of the exe's 314 status-word reads, 282 test
   condition codes and the rest the stack top, C0, fprem's condition codes, or the precision flag and
   _statfp 0x4EC490 inside the CRT's report path, which choose how a result is reported and not the
   result (DISASSEMBLED).
   `tagpu_fpverify.on` at launch runs both on every call and counts the results that differ. */
#define HYP_VA      0x004FB440u
#define HYP_HELP_VA 0x004FB480u
static const unsigned char HYP_BYTES[] = { 0x8B,0x44,0x24,0x10,0x8B,0x4C,0x24,0x0C,0x8B,0x54,0x24,0x08,0x6A,0x18,
                                           0x50,0x8B,0x44,0x24,0x0C,0x51,0x52,0x50,0xE8,0x25,0x00,0x00,0x00,
                                           0x83,0xC4,0x14,0xC3 };
static unsigned char* s_hypCode;        /* hyp_core, then the control-word reader at +0x60 */
SIM_CODE(s_hypCode);
static struct { volatile LONG calls, fast, differ; } s_hypDiag;
SIM_VAR(s_hypDiag, SIM_PRIVATE);

/* The two doubles travel as their bits: the engine's _hypot moves them with integer moves, and a
   copy through the x87 would quiet a signalling NaN on its way to 0x4FB480. The stack layout and
   the result in st(0) are the same as for (double, double). */
typedef double (__cdecl *HypHelp)(uint64_t x, uint64_t y, int op);

/* hyp_core, cdecl (const double in[3] = {|x|, |y|, M}, double out[5], const uint16_t* cw):
     push ebp; mov ebp,esp; push ecx; fnstcw [ebp-4]; mov eax,[ebp+0x10]; fldcw [eax]
     mov eax,[ebp+8]; mov ecx,[ebp+0xC]
     fld [eax]; fdiv [eax+16]; fstp [ecx]           x/M
     fld [eax+8]; fdiv [eax+16]; fstp [ecx+8]       y/M
     fld [ecx]; fmul [ecx]; fld [ecx+8]; fmul [ecx+8]; faddp st(1),st; fstp [ecx+16]
     fld [ecx+16]; fsqrt; fstp [ecx+24]
     fld [ecx+24]; fmul [eax+16]; fstp [ecx+32]
     fldcw [ebp-4]; pop ecx; leave; ret */
static const unsigned char HYP_CORE[] = {
    0x55,0x8B,0xEC,0x51,0xD9,0x7D,0xFC,0x8B,0x45,0x10,0xD9,0x28,0x8B,0x45,0x08,0x8B,0x4D,0x0C,
    0xDD,0x00,0xDC,0x70,0x10,0xDD,0x19,
    0xDD,0x40,0x08,0xDC,0x70,0x10,0xDD,0x59,0x08,
    0xDD,0x01,0xDC,0x09,0xDD,0x41,0x08,0xDC,0x49,0x08,0xDE,0xC1,0xDD,0x59,0x10,
    0xDD,0x41,0x10,0xD9,0xFA,0xDD,0x59,0x18,
    0xDD,0x41,0x18,0xDC,0x48,0x10,0xDD,0x59,0x20,
    0xD9,0x6D,0xFC,0x59,0xC9,0xC3 };
/* push ecx; fnstcw [esp]; pop eax; movzx eax,ax; ret */
static const unsigned char HYP_CW[] = { 0x51,0xD9,0x3C,0x24,0x58,0x0F,0xB7,0xC0,0xC3 };

static uint64_t hyp_bits(double d) { uint64_t b; memcpy(&b, &d, sizeof b); return b; }

/* 1 and *out when the input is one hyp_core computes as the engine does */
static int hyp_fast(uint64_t x, uint64_t y, double* out)
{
    static const uint16_t cw133f = 0x133F;
    const uint64_t bx = x & 0x7FFFFFFFFFFFFFFFull, by = y & 0x7FFFFFFFFFFFFFFFull;
    double in[3], tmp[5];
    uint64_t bm;
    unsigned em;
    if ((((unsigned (__cdecl*)(void))(void*)(s_hypCode + 0x60))() & 0x3F) != 0x3F) return 0;
    if ((bx >> 52) == 0x7FF || (by >> 52) == 0x7FF) return 0;
    memcpy(&in[0], &bx, 8);
    memcpy(&in[1], &by, 8);
    /* the engine's compare: M is |y| when |y| > |x|, else |x| (finite doubles order as their bits) */
    bm = by > bx ? by : bx;
    em = (unsigned)(bm >> 52);
    if (em < 0x3FFu - 1000u || em >= 0x3FFu + 1000u) return 0;    /* 2^-1000 <= M < 2^1000 */
    memcpy(&in[2], &bm, 8);
    ((void (__cdecl*)(const double*, double*, const uint16_t*))(void*)s_hypCode)(in, tmp, &cw133f);
    *out = tmp[4];
    return 1;
}

static double __cdecl hyp_hook(uint64_t x, uint64_t y)
{
    double mine, theirs;
    LONG n;
    if (!s_fpVerify) {
        if (hyp_fast(x, y, &mine)) return mine;
        return ((HypHelp)HYP_HELP_VA)(x, y, 0x18);
    }
    n = InterlockedIncrement(&s_hypDiag.calls);
    theirs = ((HypHelp)HYP_HELP_VA)(x, y, 0x18);
    if (hyp_fast(x, y, &mine)) {
        InterlockedIncrement(&s_hypDiag.fast);
        if (hyp_bits(mine) != hyp_bits(theirs) && InterlockedIncrement(&s_hypDiag.differ) == 1)
        {
            double dx, dy;
            memcpy(&dx, &x, 8);
            memcpy(&dy, &y, 8);
            tagpu_logf("fphyp: DIFFER: hypot(%.17g, %.17g) = %.17g here, %.17g from the engine",
                       dx, dy, mine, theirs);
        }
    }
    if (n >= 0x10000 && !(n & (n - 1)))
        tagpu_logf("fphyp: %ld calls, %ld fast, %ld differ", n, s_hypDiag.fast, s_hypDiag.differ);
    return theirs;
}

static void hyp_install(void)
{
    unsigned char jmp[5];
    unsigned rel;
    if (!tagpu_detour_bytes_ok(HYP_VA, HYP_BYTES, (int)sizeof HYP_BYTES)) {
        tagpu_log("fphyp: NOT installed: 0x4FB440 is not the retail _hypot");
        return;
    }
    if (!(s_hypCode = tagpu_detour_stub())) {
        tagpu_log("fphyp: NOT installed: no stub memory");
        return;
    }
    memcpy(s_hypCode, HYP_CORE, sizeof HYP_CORE);
    memcpy(s_hypCode + 0x60, HYP_CW, sizeof HYP_CW);
    rel = (unsigned)(uintptr_t)hyp_hook - (HYP_VA + 5u);    /* against the site */
    jmp[0] = 0xE9;
    memcpy(jmp + 1, &rel, 4);
    if (!tagpu_detour_write(HYP_VA, jmp, 5)) {
        tagpu_log("fphyp: NOT installed: 0x4FB440 could not be written");
        return;
    }
    tagpu_logf("fphyp: installed: _hypot 0x4FB440 under one control-word switch%s",
              s_fpVerify ? " (tagpu_fpverify.on: both run, differences counted)" : "");
}
