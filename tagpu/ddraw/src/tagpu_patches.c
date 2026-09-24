/* tagpu_patches.c — our own runtime engine patches on the loaded TotalA.exe image.
   Applied from DllMain via VirtualProtect; the on-disk exe stays pristine. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_patches.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"

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

/* ---- defects of the stock engine -------------------------------------------

   Two places where TotalA.exe itself writes or reads memory it does not own. Each
   patch below is the identity on every input the stock code handles safely and
   differs only where the stock code would write past an allocation or read through
   NULL. The engine map (exe-reverse-engineering.md, "Engine defects we patch") has
   the disassembly, the callers and the measurements; binary-patches.md lists them.
   `tagpu_enginefix.off` in the gamedir leaves both unpatched, for an A/B against
   stock. */

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
   them. Registers: eax (row), edi and ebp (unit) are stock's inputs; eax, ecx, edx
   and esi are dead at 0x469826, which reloads esi. The only branch into the block
   from outside is the stock NULL-cursor skip to 0x469826 [a rel8/rel32 scan of
   .text]. Identical to stock for every unit whose slot is inside the buffer; a
   unit whose slot would be past it is not drawn by the engine's sweep that frame.
   Bounding by the row instead would drop units stock draws correctly — a row run
   on into an empty neighbour is drawn whole. The list feeds nothing but
   DrawGameScreen's two draw loops, so the simulation reads nothing different. */
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

    if (memcmp((const void*)0x00469807, was, sizeof was) != 0) return 0;
    s = tagpu_detour_stub();
    if (!s) return 0;
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
    return patch_bytes(0x00469807, was, now, sizeof was);
}

/* A NULL PLOT, handed to GetGridPosFeature 0x421E60 (stdcall(plot), ret 4).
   [DISASSEMBLED] Its first two instructions are `mov ecx,[esp+4]` and
   `mov ax,[ecx+8]`: the plot's feature index is read with no test, and "no
   feature" is `or ax,0xFFFF` at 0x421E9C. GetGridPosPLOT 0x481550 and
   0x4815F0 return NULL for a cell outside main+0x14233 x main+0x14237. Three
   callers [E8 scan of .text]: 0x47EAE3 tests the plot first (0x47EADA); 0x498F4F
   does not — the cursor's hover feature in 0x498DA0, whose cell comes from
   GetTPosition's row, and GetTPosition can answer up to 143 px below the point it
   is handed; 0x40514A does not either — an order handler's target lookup through
   0x4815F0 on the order's position. For 0x498F4F stock keeps the point inside
   the scroll extent main+0x1422F, which is the map's height less 128 (the default
   of the debug-level `Edge` console command 0x416730), and that margin is exactly
   what keeps GetTPosition's row on the map. A point past the extent — an eye
   outside the stock range, a pointer on the bottom bar below one — reads
   [NULL+8] at 0x421E64: MEASURED, with our clamp at 0x498EF9 disabled.

   THE FIX, a prologue detour: the stub runs the first stolen instruction, and
   for a NULL plot returns the engine's own "no feature" 0xFFFF with the
   function's `ret 4`; otherwise it runs the second and resumes at 0x421E68.
   THE INVARIANT: 0x421E60 never dereferences NULL, whichever caller hands it
   the plot. Identity for every non-NULL plot; for a NULL one the stock code
   faults, so nothing the simulation reads changes except where it would have
   crashed. No branch lands inside the eight stolen bytes [rel8/rel32 scan]. */
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

    if (memcmp((const void*)0x00421E60, was, sizeof was) != 0) return 0;
    s = p = tagpu_detour_stub();
    if (!s) return 0;
    *p++ = 0x8B; *p++ = 0x4C; *p++ = 0x24; *p++ = 0x04;     /* mov ecx,[esp+4]  */
    *p++ = 0x85; *p++ = 0xC9;                               /* test ecx,ecx     */
    *p++ = 0x75; *p++ = 0x07;                               /* jnz +7           */
    *p++ = 0x66; *p++ = 0x0D; *p++ = 0xFF; *p++ = 0xFF;     /* or ax,0xFFFF     */
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;                  /* ret 4            */
    *p++ = 0x66; *p++ = 0x8B; *p++ = 0x41; *p++ = 0x08;     /* mov ax,[ecx+8]   */
    *p++ = 0xE9; tagpu_detour_rel(p, 0x00421E68); p += 4;   /* jmp 0x421E68     */
    return tagpu_detour_land(0x00421E60, s, 8);
}

static void patch_engine_defects(void)
{
    int sort, plot;
    char b[200];

    if (GetFileAttributesA("tagpu_enginefix.off") != INVALID_FILE_ATTRIBUTES) {
        plog("enginefix: stock engine defects left unpatched (tagpu_enginefix.off)");
        return;
    }
    sort = fix_sort_buffer_end();
    plot = fix_feature_null_plot();
    _snprintf(b, sizeof b,
              "enginefix: sort-buffer end bound 0x469807 %s; NULL-plot guard 0x421E60 %s",
              sort ? "ARMED" : "SKIPPED (byte mismatch)",
              plot ? "ARMED" : "SKIPPED (byte mismatch)");
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
