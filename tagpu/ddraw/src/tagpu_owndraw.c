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
#include "tagpu_opt.h"
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
    /* G16 step 8, decision B (gpu-posing.md §4). Skipping the engine's own
       rasterise is only safe while something replaces it, and since the CPU
       emitters were deleted the only thing that draws a unit is the posed
       program. If it cannot run — a missing GL entry point, a shader that will
       not link, a uniform block under PD_BLOCK — then skipping here would mean
       NO UNITS AT ALL, because these detours are installed at DLL attach and
       cannot be uninstalled.

       So the classifier asks first. This runs on the GAME thread and reads a
       word only the render thread writes; it is safe by DIRECTION, not by
       timing. The word says "live" only after the programs have linked, and is
       cleared before a context change invalidates them, so a stale read can
       only be stale in the direction of NOT skipping — the engine draws a unit
       we also draw, for the frames before the pass first runs, which is the
       near-invisible 8bpp-under-RGB double draw. The reverse, skipping when
       nothing will draw, has no write order that produces it. */
    {
        extern int tagpu_posedraw_live(void);
        extern int tagpu_posedraw_refused(void);
        if (!tagpu_posedraw_live()) {
            /* Say something ONLY when the pass has tried and failed. `!live`
               is also the ordinary state of the first frames, before the
               render thread has built anything — logging that would dress a
               one-frame double draw up as a broken driver. */
            static int said = 0;
            if (!said && tagpu_posedraw_refused()) {
                said = 1;
                olog2("owndraw: the posed unit program REFUSED to arm — the engine's "
                      "own unit rasterise is NOT being skipped, so units are drawn by "
                      "the engine at 8bpp. The posedraw: line above says why.");
            }
            g_passed++;
            return 0;
        }
    }
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
    int    n;
    char   buf[64];
    int    i, j;

    n = tagpu_opt_read("tagpu_owndraw.on", buf, sizeof buf);
    if (n < 0) return;
    if (n > 0) {
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
    g_all = (g_target[0] == 'a' && g_target[1] == 'l' &&
             g_target[2] == 'l' && g_target[3] == 0);
}

/* install_one's write undone: the five stolen bytes back, for the one case
   where the second site refuses after the first was written */
static void restore_one(unsigned int va, const unsigned char* stolen)
{
    unsigned char* t = (unsigned char*)va;
    DWORD old;
    if (t[0] != 0xE9) return;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(t, stolen, 5);
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
}

/* ---- the fourth detour: the engine's completed-unit silhouette shadow -----

   The third thing the engine still draws for a unit we own is its SHADOW, and
   it draws it from the unit's own composite. Inside the per-unit blit 0x459200
   there are three sites that emit the completed-unit silhouette, one per path:

     0x459335  push esi          ; esi = Object3do+0x10, the unit's composite
     0x459336  mov  ecx,edi      ; this
     0x459338  call 0x45A470     ; scratch := the composite, every non-ColorKey
                                 ;   texel -> palette index 0 (0x4B96A0)
     ...                         ; path B then clips the scratch (waterline,
                                 ;   digger) and every path blits it through
                                 ;   0x4B8500, the 50 % ALP blend, at sx+0x85.

     0x45958C  path B (colour+depth composite) — same three instructions
     0x4594DB  path B, the inline digger branch — same three instructions

   0x45A470 writes the SCRATCH (this+0x10) and never the composite, so the
   shadow is nothing more than the composite's own silhouette — which is why
   the design calls it ours: for a unit the native pass draws, "the engine's
   completed-unit shadow is built from the composite, so it comes out empty"
   (shadows-cloak.md §4, measured 2026-09-02).

   THAT ONLY HOLDS WHILE THE COMPOSITE IS EMPTY, and the classifier is not the
   thing that keeps it empty on every frame. Measured 2026-09-13: at map entry
   the commander keeps ONE teal (0,128,128) silhouette — cyan halved, i.e. the
   ALP blend over terrown's palette-254 key fill — sitting on its own body and
   surviving until the unit's pose changes, and 557 px of the composite's plane
   are back at every classify (an empty plane reads 0 NON-KEY bytes — the wipe
   fills it with the ColorKey, index 1 — and the wipe's own read-back after the
   memset read 0). Whatever refills it, the engine's shadow cannot be allowed to
   depend on it. So the shadow's source is emptied
   HERE, at the shadow, on every frame we draw the unit — a wipe the engine
   cannot undo between the call and the read, because the two are adjacent
   instructions of the same call.

   THE GATE IS NOT THE CLASSIFIER'S WHOLE ANSWER, though this comment claimed it
   was until the 2026-09-14 review. Two gaps, both still open — read this as the
   statement of a known defect, not as an argument that the wipe is correct:

     - IT OMITS THE TARGET. The classifier gates on `g_all || name_matches(
       g_target)` (the `tagpu_owndraw.on` token); preshadow never asks. With
       `owndraw=armcom` and `native=all` — both valid — the classifier leaves a
       non-armcom unit to the engine and this wipes its composite anyway, taking
       the body with the shadow. Masked under the play default only because that
       default target IS `all`. The three-site install is likewise ungated on
       `g_all` while the structure-shadow pair below is correctly `g_armed &&
       g_all`.

     - IT READS AT THE WRONG TIME, AND "a second reader, not a new window" (the
       residual 1b599e9 shipped) IS FALSE. The classifier runs only when the
       engine REBUILDS a composite — `Object3do+0x04` (TimeVisible) tested at
       0x458870, branch 0x4588F2, `inc [edi+4]` after each blit — while
       preshadow runs on EVERY blit of every frame. On a non-rebuild frame
       preshadow runs and the classifier does not run at all; for an idle unit
       the gap is seconds, and nothing on the composite records which answer
       built it. Worse, what actually gates OUR draw per unit is the packet flag
       TAGPU_PK_U_NATIVE, stamped at publish, and the publish is skipped while
       the cell still holds a fresh packet — so this wipe can act on an
       ownership answer up to one present NEWER than the one the pass is drawing
       from. Adjudicated 2026-09-14 by two reviewers arguing opposite sides: the
       `s_state` 0->1 chain does NOT produce it (that store happens inside the
       render frame that then draws, so the pass covers the wipe), but an
       `s_armed` 0->1 does — a mid-session re-arm flips it on the render thread
       while the in-flight packet still has every PK_U_NATIVE clear, and for
       that packet's life every owned unit on screen loses body and shadow.
       Reachable from the lever-editing loop and at the session's first arm, NOT
       from play input, which is why play has never shown it. THE FIX IS AN
       ORDERING — gate the wipe on the same published flag the draw used, not on
       a live re-read — and it is not in this landing.

   `tagpu_posedraw_live()` is exactly the question the
   classifier asks before it skips; while it is false the engine is the only
   renderer, and emptying the composite would take the unit's BODY with the
   shadow (the body blits from the composite too, 0x459373), which is the
   invisible-unit failure the classifier's fallback exists to avoid. And a 3D
   WRECK is not covered by the target: the classifier recognises one by the
   scratch feature-unit `*(main+0x1420F)`'s `+0x9E` and hands the husk back to
   the engine unless `tagpu_native_wrecks_armed()`, while the unit predicate
   `tagpu_native_owns_obj` answers YES to a husk under `target="all"` exactly as
   it does to a unit. Wiping here without that branch would blank a husk's body
   for the whole of a `native=all` arm that does not carry the `wrecks` token
   (the play defaults do carry it, which is why this was not seen in play). So
   this asks the wrecks question too, in the classifier's own order. RESIDUAL, stated rather than hidden: while the posed program is down
   the engine's own shadow is left alone, and with `target="all"` that is the
   only window in which the engine draws a unit at all. Measured over a 129-frame
   burst spanning one map entry — the load, the context change and the first
   seconds of play — no teal reached the screen; those frames are composited
   before our surface is up, so the engine's frame is not the one on screen.

   The register. `ebp` is the blit's Object3do — the blit's SECOND stack
   argument (`mov ebp,[esp+0x30]` at 0x459205, before the remaining two pushes:
   0x20 + push ebx + push ebp puts it at entry esp + 8) — and it is never
   reloaded on any path that reaches the three sites; every use of it before
   them is a read or a push. Path A's cargo loop is the ONLY reload of ebp in
   the function (0x459415 `mov ebp,[edx+0x8A]`, 0x459489 `mov ebp,[ebp+0x8E]`),
   and its exit is the function's OWN epilogue — `pop ebp` at 0x459495 with
   `ret 0x18` at 0x45949A; path B is entered at 0x45949D by the `jne` at
   0x459282 and so never runs it. Path B's own cargo loop, at 0x459649, walks
   the same list in ESI (`mov esi,[edx+0x8A]`) and does not touch ebp at all.
   It was called a second ebp reload here until the 2026-09-14 review
   disassembled it; the corrected fact is the stronger one. A register that "happens to survive" is not an argument,
   so the stub passes it and tagpu_owndraw_preshadow CHECKS it:
   `*(Object3do + 0x10)` must be the very composite the site is about to read, or
   it does nothing. A wrong ebp is then a no-op, never a wipe of somebody else's
   plane.

   Stub (entered by jmp from the site, so the composite is at [esp] and the
   blit's frame is untouched):
     pushad
     push [esp+0x20]            ; the composite (entry esp + 0 after pushad)
     push ebp                   ; the Object3do
     call tagpu_owndraw_preshadow   ; cdecl(obj3do, composite)
     add esp,8 ; popad
     E8 <rel32>                 ; the stolen call 0x45A470, replayed
     E9 <rel32>                 ; resume at site+5
   The callee is `ret 4`, so the composite the site pushed is consumed by the
   replayed call exactly as the original did and esp leaves the stub unchanged. */
#define SHADOW_A_VA   0x00459338u   /* path A: push esi; mov ecx,edi; call  */
#define SHADOW_A_RES  0x0045933Du
#define SHADOW_B_VA   0x0045958Cu   /* path B                                 */
#define SHADOW_B_RES  0x00459591u
#define SHADOW_C_VA   0x004594DBu   /* path B, the inline digger branch        */
#define SHADOW_C_RES  0x004594E0u
#define SHADOW_VA     0x0045A470u   /* scratch := blackened composite, ret 4   */
#define O3_COMPOSITE  0x10

static const unsigned char SHAD_A_STOLEN[5] = { 0xE8, 0x33, 0x11, 0x00, 0x00 };
static const unsigned char SHAD_B_STOLEN[5] = { 0xE8, 0xDF, 0x0E, 0x00, 0x00 };
static const unsigned char SHAD_C_STOLEN[5] = { 0xE8, 0x90, 0x0F, 0x00, 0x00 };

static int g_shadow = 0;   /* all three sites patched */

/* cdecl, called from the stub: empty the composite the engine is about to
   build an owned unit's shadow from. See the block above for the gate. */
void __cdecl tagpu_owndraw_preshadow(unsigned int obj3do, unsigned int frame)
{
    extern int tagpu_posedraw_live(void);
    extern int tagpu_native_owns_obj(unsigned int obj3do);
    if (!ptr_ok(obj3do) || !ptr_ok(frame)) return;
    if (*(volatile unsigned int*)(obj3do + O3_COMPOSITE) != frame) return;
    if (!tagpu_posedraw_live()) return;
    /* A HUSK IS NOT OURS UNLESS THE WRECK PASS IS ARMED — the classifier's own
       first branch, and the only place the two answers can differ. It
       recognises a wreck draw by the scratch feature-unit *(main+0x1420F)
       holding THIS Object3do at +0x9E. MEASURED 2026-09-13 on `one-wreck`
       (armalab_dead, Two Continents) with `tagpu_native.on` = "all" and no
       `wrecks` token: with this clause disabled in a test build the husk still
       renders, so the unit predicate does not in fact answer yes to a husk
       today, and the clause changes nothing on that fixture. It stays because
       the wipe's predicate should BE the classifier's answer, not a fact about
       what the wreck builder happens to leave at Object3do+0x0C — a fact this
       landing did not establish. */
    {
        unsigned int taMain = *(volatile unsigned int*)0x00511DE8u;
        if (ptr_ok(taMain)) {
            unsigned int scratch = *(volatile unsigned int*)(taMain + 0x1420Fu);
            if (ptr_ok(scratch) &&
                *(volatile unsigned int*)(scratch + 0x9Eu) == obj3do) {
                extern int tagpu_native_wrecks_armed(void);
                if (!tagpu_native_wrecks_armed()) return;
            }
        }
    }
    if (!tagpu_native_owns_obj(obj3do)) return;
    tagpu_r3dcache_wipe(frame);
}

static int install_shadow(unsigned int va, unsigned int resume,
                          const unsigned char* stolen)
{
    unsigned char* t = (unsigned char*)va;
    unsigned char* s;
    unsigned char* p;
    DWORD old;
    int32_t rel;

    if (memcmp(t, stolen, 5) != 0) return 0;
    s = (unsigned char*)VirtualAlloc(NULL, 0x80,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s) return 0;
    p = s;

    *p++ = 0x60;                                            /* pushad             */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x20;     /* push [esp+0x20]    */
    *p++ = 0x55;                                            /* push ebp           */
    *p++ = 0xE8;                                            /* call preshadow     */
    rel = (int32_t)((unsigned int)&tagpu_owndraw_preshadow - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                  /* add esp,8          */
    *p++ = 0x61;                                            /* popad              */
    *p++ = 0xE8;                                            /* call 0x45A470      */
    rel = (int32_t)(SHADOW_VA - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0xE9;                                            /* jmp resume         */
    rel = (int32_t)(resume - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;

    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t[0] = 0xE9;
    rel = (int32_t)((unsigned int)s - (va + 5));
    memcpy(t + 1, &rel, 4);
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    return 1;
}

void tagpu_owndraw_init(void)
{
    /* worst case is 259 chars: the arm token is capped at 31 and every field
       takes its longest value. _snprintf does NOT terminate a truncation, so
       the tail is forced rather than assumed. */
    char b[320];
    int a, c;

    if (!tagpu_opt_on("tagpu_owndraw.on")) return;

    /* all-or-nothing: both sites checked before either is written, so a
       different build arms nothing rather than half of it. Half would be
       worse than nothing here: a detoured opaque site with g_armed clear
       still skips the engine's rasterise under "all" (classify never reads
       g_armed) while buildfx and the structure shadows stay the engine's.
       (The landing review of 2026-09-08, once owndraw was on by default.) */
    if (memcmp((void*)RAST_OPAQUE_VA, OPQ_STOLEN, 5) != 0 ||
        memcmp((void*)RAST_NANO_VA,   NANO_STOLEN, 5) != 0) {
        olog2("owndraw: NOT armed -- engine bytes differ at 0x459830 / 0x459C70 (nothing written)");
        return;
    }
    read_target();
    a = install_one(RAST_OPAQUE_VA, RAST_OPAQUE_RES, OPQ_STOLEN);
    c = install_one(RAST_NANO_VA,   RAST_NANO_RES,   NANO_STOLEN);
    if (a != c) {                       /* an allocation refused between the two */
        if (a) restore_one(RAST_OPAQUE_VA, OPQ_STOLEN);
        if (c) restore_one(RAST_NANO_VA,   NANO_STOLEN);
        a = c = 0;
    }
    g_armed = a && c;
    if (g_armed) g_buildfx = install_buildfx();
    /* the completed-unit shadow's three emit sites: all three or none, for the
       same reason the structure-shadow pair is all-or-nothing — one path
       emptied and not the others would make a unit's shadow depend on whether
       it was moving when the frame was drawn */
    if (g_armed) {
        int sa = install_shadow(SHADOW_A_VA, SHADOW_A_RES, SHAD_A_STOLEN);
        int sb = sa && install_shadow(SHADOW_B_VA, SHADOW_B_RES, SHAD_B_STOLEN);
        int sc = sb && install_shadow(SHADOW_C_VA, SHADOW_C_RES, SHAD_C_STOLEN);
        if (sa && !sc) {
            if (sb) restore_one(SHADOW_B_VA, SHAD_B_STOLEN);
            restore_one(SHADOW_A_VA, SHAD_A_STOLEN);
        }
        g_shadow = sa && sb && sc;
    }
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
        "buildfx@0x458DD0=%s structshadow@0x4592C6+0x45952C=%s shadow@0x459338+0x45958C+0x4594DB=%s "
        "(engine rasterise skipped for target; writeback must paint it)",
        g_armed ? "ARMED" : "not armed", g_target,
        a ? "OK" : "SKIP", c ? "OK" : "SKIP",
        g_buildfx ? "OK" : "SKIP",
        /* each flag under ITS OWN label: g_sshadow is the structure pair (the
           two je flips, installed only under `all`, hence its "engine" case),
           g_shadow is the three-site detour. They were passed the other way
           round from the landing until the 2026-09-14 review, so the one line
           that says whether the detour went in reported the other flag. */
        g_sshadow ? "OURS" : (g_all ? "SKIP" : "engine"),
        g_shadow ? "OURS" : "SKIP");
    b[sizeof b - 1] = 0;
    olog2(b);
}

void tagpu_owndraw_flush(unsigned int frame_counter)
{
    if (!g_armed) return;
    if (frame_counter - g_last >= 60) {
        unsigned s = g_skipped, pa = g_passed;
        char b[224];
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
