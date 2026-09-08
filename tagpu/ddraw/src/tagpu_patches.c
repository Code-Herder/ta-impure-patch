/* tagpu_patches.c — our own runtime engine patches on the loaded TotalA.exe image.
   Applied from DllMain via VirtualProtect; the on-disk exe stays pristine. */

#include <windows.h>
#include <stdio.h>
#include "tagpu_patches.h"

static void plog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
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
       readers of 0x37EFA (0x499046 / 0x499162 / 0x499352 / 0x499567) is what missed
       it the first time: 0x499046's arm is guarded by the cursor index, so leaving
       the interface-type compares untouched does not leave the button untouched. */
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
                   than ship the thing this landing exists to fix.
                   [FROM REVIEW 2026-09-07] */
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
}
