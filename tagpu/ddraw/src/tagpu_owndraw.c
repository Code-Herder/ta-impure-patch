/* tagpu_owndraw.c — Phase B "own the draw": TA keeps its per-unit pipeline
   (pose sync, AABB, composite alloc, hotspots, blit) but the SOFTWARE
   RASTERISATION of the composite planes is skipped for chosen unit types —
   the GPU thread (tagpu_render3do via writeback) is then the only writer of
   those pixels. Two 5-byte detours cover all three rasterise call sites
   (evidence: research/notes/own-the-draw.md):

     0x459830  opaque rasteriser   (called from builder 0x45878B and from the
                                    blit's build-state path 0x459641)
     0x459C70  the SAME rasteriser plus Gouraud lighting, taken for STRUCTURES
               (called from builder 0x458765, whose test at 0x45873C is
               unit+0x110 & 0x20000000 — measured to be the structure bit, not
               "under construction": build-state.md 1. This comment used to
               call it the nanoframe rasteriser; it is not one.)

   Both are thiscall with 4 stack args, callee-clean `ret 0x10`:
     [esp+4]=composite GAFFrame*, [esp+8]=Object3do*, [esp+0xC]=cloak byte,
     [esp+0x10]=mode. Return value is DEAD at every call site (callers do
     `mov eax,1` immediately after), and the builder finishes its hotspot and
     Object3do+0x10 stores BEFORE calling the rasteriser — so a classify-then-
     `ret 0x10` skip is observationally pure apart from the planes staying
     unpainted (ColorKey colour + 0 depth from the allocator).

   Both prologues open with `mov eax,imm32` (5 bytes) before `call __chkstk`,
   giving a clean detour boundary:
     0x459830: B8 04 5F 00 00  -> resume 0x459835
     0x459C70: B8 D4 59 01 00  -> resume 0x459C75

   Stub (same shape as tagpu_suppress.c):
     pushad
     push [esp+0x28]            ; [entry esp+8] = Object3do*
     call classify              ; cdecl; eax=1 => skip rasterise
     add esp,4 ; test eax,eax
     popad
     jnz +10 -> skip
     B8 <imm32>                 ; the 5 stolen bytes (mov eax,imm32)
     E9 <rel32>                 ; resume the real rasteriser
   skip:
     C2 10 00                   ; ret 0x10 — unwind exactly like the callee

   With target "all" two more bytes go in, both inside the blit 0x459200
   (shadows-cloak.md "Shadow decision tree"): the `je` that sends a unit whose
   state carries 0x20000000 (structures) to the CACHED SLANT SHADOW branch
   becomes a `jmp`, so every unit takes the completed-unit silhouette branch
   instead. That branch builds its shadow from the composite -- blank under
   "all" -- and so blits nothing. Why: the cached shadow is drawn by the
   ALP-blend blit 0x4B8500, and inside a key-filled viewport (terrown) the
   blend darkens palette 254's cyan into an opaque teal silhouette that sits
   at the 1x position whatever the zoom. The native pass draws the slant
   shadow in its place (tagpu_native.c, `slant`).
     0x4592C6: 74 5C  je 0x459324   (path A, colour-only composite)  -> EB 5C
     0x45952C: 74 4A  je 0x459578   (path B, colour+depth)           -> EB 4A
   Both leave eax (the graphics-option word the target tests) untouched.

   A THIRD detour, on the BLIT-TIME BUILD-STATE EFFECT 0x458DD0, covers units
   under construction (build-state.md). That function is the whole nanoframe
   look — the height-threshold recolour and the wireframe — and it runs on a
   scratch COPY of the composite every frame, so wiping the composite does not
   stop it: with the rasterise skipped it simply recoloured nothing and stamped
   its wireframe alone, at the 1x projection, where a zoomed view left it
   sitting away from the unit being built. It early-outs on `Nanoframe == 0`
   already, so the detour only ever fires for a nanoframe, and it is skipped on
   exactly the units the native pass has taken over (which stages the same
   effect itself, through the zoom transform, from the same engine formulas).

     0x458DD0: 53 55 8B 6C 24 0C -> resume 0x458DD6 (6 stolen: push ebx,
               push ebp, mov ebp,[esp+0xC] — whole instructions, and the
               esp-relative one is replayed at the entry esp after popad)
     thiscall(this, GAFFrame* frame, Object3do* obj), ret 8; obj is at
     [esp+8] on entry. The skip path returns 0 in eax, which is the engine's
     own "did nothing" return from both of its early-outs.

   Classification: Object3do+0x0C -> UnitStruct -> +0x92 UnitDefStruct, match
   the token against Name@0x00 / UnitName@0x20 / ObjectName@0x80 (all three;
   same rule as suppress/writeback); token "all" skips every unit. Read-only
   over sim. NOTE: a type armed here but NOT armed in tagpu_writeback.on
   renders as an invisible sprite (blank planes are all ColorKey). */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tagpu_owndraw.h"
#include "tagpu_r3dcache.h"

#define RAST_OPAQUE_VA   0x00459830u
#define RAST_OPAQUE_RES  0x00459835u
#define RAST_NANO_VA     0x00459C70u
#define RAST_NANO_RES    0x00459C75u
#define BUILDFX_VA       0x00458DD0u
#define BUILDFX_RES      0x00458DD6u

#define O3_THISUNIT      0x0C
#define U_UNITTYPE       0x92
#define UD_NAME          0x00
#define UD_UNITNAME      0x20
#define UD_OBJNAME       0x80
#define NAME_FIELD_LEN   0x20

static const unsigned char OPQ_STOLEN[5]  = { 0xB8, 0x04, 0x5F, 0x00, 0x00 };
static const unsigned char NANO_STOLEN[5] = { 0xB8, 0xD4, 0x59, 0x01, 0x00 };
static const unsigned char BFX_STOLEN[6]  = { 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C };

/* the two structure-shadow `je`s (see the header comment): site, rel8 */
#define SSHADOW_A_VA     0x004592C6u
#define SSHADOW_A_REL    0x5C
#define SSHADOW_B_VA     0x0045952Cu
#define SSHADOW_B_REL    0x4A

static int               g_armed      = 0;
static char              g_target[32] = "armcom";
static int               g_all        = 0;
static int               g_sshadow    = 0;   /* both je->jmp patches in */
static int               g_buildfx    = 0;   /* 0x458DD0 detour in      */

static volatile unsigned g_skipped    = 0;
static volatile unsigned g_passed     = 0;
static unsigned          g_skip_total = 0, g_pass_total = 0, g_last = 0;

static void olog2(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int ptr_ok(unsigned int p) { return p > 0x00600000u && p < 0x7FFF0000u; }

static int name_matches(const char* field)
{
    int i;
    for (i = 0; i < NAME_FIELD_LEN; i++) {
        unsigned char a = (unsigned char)field[i];
        unsigned char b = (unsigned char)g_target[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (a != b) return 0;
        if (b == 0) return 1;
    }
    return 1;
}

int __cdecl tagpu_owndraw_classify(unsigned int obj3do, unsigned int frame)
{
    unsigned int unit, def;
    int skip = 0;
    if (!ptr_ok(obj3do)) { g_passed++; return 0; }
    /* 3D-wreck draws come through the scratch feature-unit *(main+0x1420F)
       (terrain-depth 3.4): during the draw its +0x9E holds THIS obj3do.
       They are NOT covered by "all" — the engine keeps painting husks unless
       the native wreck pass is armed, which then owns their pixels (skip the
       rasterise + wipe the composite, exactly like native units). Without
       this branch, "all" would skip a fresh husk's FIRST rasterise with
       nothing cached to restore = invisible corpse. */
    {
        unsigned int taMain = *(volatile unsigned int*)0x00511DE8u;
        if (ptr_ok(taMain)) {
            unsigned int scratch = *(volatile unsigned int*)(taMain + 0x1420Fu);
            if (ptr_ok(scratch) &&
                *(volatile unsigned int*)(scratch + 0x9Eu) == obj3do) {
                extern int tagpu_native_wrecks_armed(void);
                if (tagpu_native_wrecks_armed()) {
                    g_skipped++;
                    tagpu_r3dcache_wipe(frame);
                    return 1;
                }
                g_passed++;
                return 0;
            }
        }
    }
    if (g_all) skip = 1;
    else {
        unit = *(volatile unsigned int*)(obj3do + O3_THISUNIT);
        if (!ptr_ok(unit)) { g_passed++; return 0; }
        def = *(volatile unsigned int*)(unit + U_UNITTYPE);
        if (!ptr_ok(def)) { g_passed++; return 0; }
        skip = name_matches((const char*)(def + UD_NAME)) ||
               name_matches((const char*)(def + UD_UNITNAME)) ||
               name_matches((const char*)(def + UD_OBJNAME));
    }
    if (!skip) { g_passed++; return 0; }
    g_skipped++;
    /* engine just (re)built this composite and we are about to skip its
       rasterise — repaint our last render NOW so this frame's blit shows the
       unit (no one-frame empty window = no flicker on movers/builders).
       Natively-owned units get a WIPE instead: their pixels come from the
       G12b pass, the composite must blit nothing. */
    {
        extern int tagpu_native_owns_obj(unsigned int obj3do);
        if (tagpu_native_owns_obj(obj3do)) tagpu_r3dcache_wipe(frame);
        else                               tagpu_r3dcache_restore(obj3do, frame);
    }
    return 1;
}

/* The build-state effect 0x458DD0 is skipped for exactly the units the native
   pass draws: it stages the same scaffold itself, and leaving the engine's copy
   in would stamp a second one at the unzoomed projection. Every other unit —
   the pass unarmed, a type it does not own — keeps the engine's own. */
int __cdecl tagpu_owndraw_buildfx_skip(unsigned int obj3do)
{
    extern int tagpu_native_owns_obj(unsigned int obj3do);
    if (!ptr_ok(obj3do)) return 0;
    return tagpu_native_owns_obj(obj3do) ? 1 : 0;
}

/* Detour 0x458DD0 onto a classify-then-skip stub of the same shape as
   install_one's, with SIX stolen bytes and the callee's own `ret 8`. */
static int install_buildfx(void)
{
    unsigned char* t = (unsigned char*)BUILDFX_VA;
    unsigned char* s;
    unsigned char* p;
    DWORD old;
    int32_t rel;

    if (memcmp(t, BFX_STOLEN, sizeof BFX_STOLEN) != 0) return 0;

    s = (unsigned char*)VirtualAlloc(NULL, 0x80,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s) return 0;
    p = s;

    *p++ = 0x60;                                            /* pushad            */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x28;     /* push [esp+0x28]=obj */
    *p++ = 0xE8;                                            /* call skip?        */
    rel = (int32_t)((unsigned int)&tagpu_owndraw_buildfx_skip - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                  /* add esp,4         */
    *p++ = 0x85; *p++ = 0xC0;                               /* test eax,eax      */
    *p++ = 0x61;                                            /* popad             */
    *p++ = 0x75; *p++ = 0x0B;                               /* jnz +11 -> skip   */
    memcpy(p, BFX_STOLEN, sizeof BFX_STOLEN);               /* the 6 stolen      */
    p += sizeof BFX_STOLEN;
    *p++ = 0xE9;                                            /* jmp resume        */
    rel = (int32_t)(BUILDFX_RES - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x33; *p++ = 0xC0;                               /* skip: xor eax,eax */
    *p++ = 0xC2; *p++ = 0x08; *p++ = 0x00;                  /*       ret 8       */

    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(s, 0, MEM_RELEASE);   /* nothing points at it yet */
        return 0;
    }
    t[0] = 0xE9;
    rel = (int32_t)((unsigned int)s - (BUILDFX_VA + 5));
    memcpy(t + 1, &rel, 4);
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    return 1;
}

/* Whether the 0x458DD0 detour is actually in place. The native pass may only
   claim a unit under construction while it is: without the detour the engine
   still stamps its own recolour and wireframe onto the composite, at the
   unzoomed 1x projection, which is precisely the drift this pass exists to
   remove. install_buildfx() can fail (a build whose bytes do not match, or a
   VirtualAlloc/VirtualProtect refusal) long after the two rasteriser detours
   went in, so "owndraw is armed" is not the same question. */
int tagpu_owndraw_buildfx_armed(void) { return g_buildfx; }

/* Build one classify-then-skip stub and detour `va` onto it. */
static int install_one(unsigned int va, unsigned int resume,
                       const unsigned char* stolen)
{
    unsigned char* t = (unsigned char*)va;
    unsigned char* s;
    unsigned char* p;
    DWORD old;
    int32_t rel;

    if (memcmp(t, stolen, 5) != 0) return 0;      /* wrong build — leave alone */

    s = (unsigned char*)VirtualAlloc(NULL, 0x80,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s) return 0;
    p = s;

    *p++ = 0x60;                                            /* pushad             */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x24;     /* push [esp+0x24] = frame   */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x2C;     /* push [esp+0x2C] = obj3do  */
    *p++ = 0xE8;                                            /* call classify      */
    rel = (int32_t)((unsigned int)&tagpu_owndraw_classify - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                  /* add esp,8          */
    *p++ = 0x85; *p++ = 0xC0;                               /* test eax,eax       */
    *p++ = 0x61;                                            /* popad              */
    *p++ = 0x75; *p++ = 0x0A;                               /* jnz +10 -> skip    */
    memcpy(p, stolen, 5); p += 5;                           /* mov eax,imm32      */
    *p++ = 0xE9;                                            /* jmp resume         */
    rel = (int32_t)(resume - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0xC2; *p++ = 0x10; *p++ = 0x00;                  /* skip: ret 0x10     */

    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t[0] = 0xE9;
    rel = (int32_t)((unsigned int)s - (va + 5));
    memcpy(t + 1, &rel, 4);
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    return 1;
}

/* `74 rel8` (je) -> `EB rel8` (jmp) at one verified site; 0 = wrong build */
static int patch_je_to_jmp(unsigned int va, unsigned char rel)
{
    unsigned char* t = (unsigned char*)va;
    DWORD old;
    if (t[0] != 0x74 || t[1] != rel) return 0;
    if (!VirtualProtect(t, 2, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t[0] = 0xEB;
    VirtualProtect(t, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 2);
    return 1;
}

int tagpu_owndraw_structshadow_ours(void) { return g_sshadow; }

static void read_target(void)
{
    HANDLE h;
    DWORD  n = 0;
    char   buf[64];
    int    i, j;

    h = CreateFileA("tagpu_owndraw.on", GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (ReadFile(h, buf, (DWORD)(sizeof(buf) - 1), &n, NULL) && n > 0) {
        buf[n] = 0;
        i = 0;
        while (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') i++;
        j = 0;
        while (buf[i] && j < 31 &&
               buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' && buf[i] != '\n') {
            char c = buf[i++];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            g_target[j++] = c;
        }
        if (j > 0) g_target[j] = 0;
    }
    CloseHandle(h);
    g_all = (g_target[0] == 'a' && g_target[1] == 'l' &&
             g_target[2] == 'l' && g_target[3] == 0);
}

void tagpu_owndraw_init(void)
{
    char b[192];
    int a, c;

    if (GetFileAttributesA("tagpu_owndraw.on") == INVALID_FILE_ATTRIBUTES) return;

    read_target();
    a = install_one(RAST_OPAQUE_VA, RAST_OPAQUE_RES, OPQ_STOLEN);
    c = install_one(RAST_NANO_VA,   RAST_NANO_RES,   NANO_STOLEN);
    g_armed = a && c;
    if (g_armed) g_buildfx = install_buildfx();
    /* structure shadows: only with "all" (every composite blank), and only
       as a pair -- one path redirected and not the other would leave a
       building's shadow depending on which composite it was given */
    if (g_armed && g_all) {
        int sa = patch_je_to_jmp(SSHADOW_A_VA, SSHADOW_A_REL);
        int sb = sa && patch_je_to_jmp(SSHADOW_B_VA, SSHADOW_B_REL);
        if (sa && !sb) {
            unsigned char* t = (unsigned char*)SSHADOW_A_VA;
            DWORD old;
            if (VirtualProtect(t, 2, PAGE_EXECUTE_READWRITE, &old)) {
                t[0] = 0x74;
                VirtualProtect(t, 2, old, &old);
                FlushInstructionCache(GetCurrentProcess(), t, 2);
            }
        }
        g_sshadow = sa && sb;
    }

    _snprintf(b, sizeof b,
        "owndraw: %s target=\"%s\" opaque@0x459830=%s nano@0x459C70=%s "
        "buildfx@0x458DD0=%s structshadow@0x4592C6+0x45952C=%s "
        "(engine rasterise skipped for target; writeback must paint it)",
        g_armed ? "ARMED" : "not armed", g_target,
        a ? "OK" : "SKIP", c ? "OK" : "SKIP",
        g_buildfx ? "OK" : "SKIP",
        g_sshadow ? "OURS" : (g_all ? "SKIP" : "engine"));
    olog2(b);
}

void tagpu_owndraw_flush(unsigned int frame_counter)
{
    if (!g_armed) return;
    if (frame_counter - g_last >= 60) {
        unsigned s = g_skipped, pa = g_passed;
        char b[160];
        g_skipped = 0; g_passed = 0;
        g_skip_total += s; g_pass_total += pa;
        unsigned rs, ms;
        extern volatile unsigned g_rc_calls, g_rc_off, g_rc_badptr;
        tagpu_r3dcache_stats(&rs, &ms);
        _snprintf(b, sizeof b,
            "OWND target=%s skipped=%u passed=%u repaint=%u miss=%u rcall=%u off=%u bad=%u (total skipped=%u passed=%u)",
            g_target, s, pa, rs, ms, g_rc_calls, g_rc_off, g_rc_badptr,
            g_skip_total, g_pass_total);
        g_rc_calls = 0; g_rc_off = 0; g_rc_badptr = 0;
        olog2(b);
        g_last = frame_counter;
    }
}
