/* tagpu_patches.c — our own runtime engine patches on the loaded TotalA.exe image.
   Applied from DllMain via VirtualProtect; the on-disk exe stays pristine. */

#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_patches.h"
#include "tagpu_limits.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
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
}

/* ===== THE RAISED LIMITS (tagpu_limits.h) ===================================================
   The engine limits TADR raises -- the effect pools as its EngineLimits.cpp does, the unit
   limit and the pathfinding budget as its LimitCrack.cpp does -- re-derived from the
   pristine 3.1 image (research/notes/tadr-port/limits-evidence.md §1-6 holds every site's
   disassembly). Every site goes into ONE table; the table is compared with the stock
   bytes as a whole and written as a whole, or not at all.

   WHY BEFORE ANYTHING RUNS: DllMain runs before TotalA.exe's entry point (ddraw.dll is its
   first static import), so no game exists yet and no engine thread executes these bytes
   while they change. Nothing here is ever put back: the patches last for the process. */

#define LIM_MAXSITE  128
#define LIM_MAXB     16

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
static int     s_limNoStub;          /* a code stub could not be allocated           */
static unsigned int s_limWriteFail;  /* the site VirtualProtect refused, 0 = none    */

#ifndef TAGPU_LIMITS_STOCK

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

static void lim_dword(unsigned int va, unsigned int stock, unsigned int ours, const char* name)
{
    lim_add(va, 4, (const unsigned char*)&stock, (const unsigned char*)&ours, name);
}

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

int tagpu_limits_install(void)
{
    int i, bad = 0, written;
    if (s_limState) return s_limState > 0;
    lim_sites();
    if (s_limNoStub) { s_limState = -1; plog("limits: FAILED -- a code stub could not be allocated"); return 0; }
    if (s_limOverflow) { s_limState = -1; plog("limits: FAILED -- the site table is too small"); return 0; }

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
           nothing runs meanwhile on a half-raised engine. */
        s_limWriteFail = s_lim[written].va;
        while (written-- > 0) tagpu_detour_write(s_lim[written].va, s_lim[written].stock, s_lim[written].n);
        s_limState = -1;
        tagpu_logf("limits: FAILED -- the write at 0x%08X was refused; everything written was put back",
                   s_limWriteFail);
        return 0;
    }
    s_limState = 1;
    /* the moved pools' addresses, for `tacli peek`: the explosion count is the first dword */
    tagpu_logf("limits: installed %d sites -- projectiles %d, explosions %d at 0x%08X, "
               "flying pieces %d at 0x%08X, debris records %d at 0x%08X, units %d a player, "
               "pathfinding %d, particles %d a layer from a pool of %d", s_nlim,
               TAGPU_LIM_PROJ, TAGPU_LIM_EXPL, (unsigned int)(size_t)&s_expl,
               TAGPU_LIM_PSYS, (unsigned int)(size_t)s_psys,
               TAGPU_LIM_AUX, (unsigned int)(size_t)s_aux, TAGPU_LIM_UNITS, TAGPU_LIM_PATH,
               TAGPU_LIM_SFX, TAGPU_LIM_SFXPOOL);
    return 1;
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

#else  /* TAGPU_LIMITS_STOCK */

int tagpu_limits_install(void)
{
    plog("limits: stock build -- nothing raised (projectiles 300, explosions 300, "
         "flying pieces 100, debris records 300, units 250 a player up to 500, "
         "pathfinding 1333, particles 400 a layer from a pool of 1000)");
    return 0;
}

const char* tagpu_limits_expl_pool(const char* ta) { return ta + 0x1491B; }
const void* const* tagpu_limits_psys_begin(void) { return (const void* const*)(size_t)0x00511DF0u; }
const void* const* tagpu_limits_psys_end(void) { return (const void* const*)(size_t)0x00511F80u; }

#endif /* TAGPU_LIMITS_STOCK */

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

static void lim_hex(char* out, const unsigned char* b, int n)
{
    int i;
    out[0] = 0;
    for (i = 0; i < n; i++) sprintf(out + strlen(out), i ? " %02X" : "%02X", b[i]);
}

void tagpu_limits_report(void)
{
    static char text[8192];
    static LONG once;
    char exe[MAX_PATH], md5[33] = "unknown", line[256], want[64], have[64];
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
    if (s_limNoStub || s_limOverflow)
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
        "Impure could not install its engine limits, so Total Annihilation will now "
        "close. Nothing was changed.\r\n"
        "\r\n"
        "WHY\r\n"
        "Impure raises the game's limits (units, projectiles, explosions...) by "
        "rewriting its code in memory, and it checks every place first. %s Running "
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
        _snprintf(line, sizeof line, "result: a code stub could not be allocated, nothing written\r\n");
    else if (s_limOverflow)
        _snprintf(line, sizeof line, "result: the site table overflowed, nothing written\r\n");
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
        lim_hex(want, s->stock, s->n);
        lim_hex(have, s->have, s->n);
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
        lim_hex(want, s->stock, s->n);
        lim_hex(have, s->have, s->n);
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
