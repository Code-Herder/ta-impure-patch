/* tagpu_owndraw.c — Phase B "own the draw": TA keeps its per-unit pipeline
   (pose sync, AABB, composite alloc, hotspots, blit), and the SOFTWARE
   RASTERISATION of the composite planes is detoured so that a draw we own can
   skip it. The one draw skipped is a 3D wreck while the native wreck pass is
   armed, whose composite is also wiped; every unit is rasterised by the
   engine (`tagpu_owndraw_classify` says why). Two 5-byte detours cover all
   three rasterise call sites (evidence: research/notes/own-the-draw.md):

     0x459830  opaque rasteriser   (called from builder 0x45878B and from the
                                    blit's build-state path 0x459641)
     0x459C70  the SAME rasteriser plus Gouraud lighting, taken for STRUCTURES
               (called from builder 0x458765, whose test at 0x45873C is
               unit+0x110 & 0x20000000 — measured to be the structure bit, not
               "under construction": build-state.md 1. It is not the nanoframe
               rasteriser.)

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

   With target "all" the blit's two STRUCTURE-SHADOW branches are taken over
   too -- see the block at "THE STRUCTURE-SHADOW PAIR" below. In short: 21
   bytes are stolen at 0x4592BF and 0x459522 (each site's `test` plus its `je`)
   and replaced by a detour onto a stub that asks `g_ssSkip` first, so the
   suppression is decided PER DRAW rather than once at DllMain.
   Why it matters that the engine's branch is suppressed at all: the cached
   shadow is drawn by the ALP-blend blit 0x4B8500, and inside a key-filled
   viewport (terrown) the blend darkens palette 254's cyan into an opaque teal
   silhouette that sits at the 1x position whatever the zoom. MEASURED
   2026-09-18 on the OpenGL renderer this fork had then (renderer=openglcore):
   with the gate down, four structures put
   8779 px of exactly (0,128,128) on the screen. The native pass draws the
   slant shadow in its place (tagpu_native.c, `slant`).

   A THIRD detour, on the BLIT-TIME BUILD-STATE EFFECT 0x458DD0, covers units
   under construction (build-state.md). That function is the whole nanoframe
   look — the height-threshold recolour and the wireframe — and it runs on a
   scratch COPY of the composite every frame, so wiping the composite does not
   stop it: with the rasterise skipped it would recolour nothing and stamp its
   wireframe alone, at the 1x projection, where a zoomed view leaves it
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
   same rule as suppress/writeback); token "all" covers every unit. Read-only
   over sim. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tagpu_owndraw.h"
#include "tagpu_opt.h"

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

/* THE TWO STRUCTURE-SHADOW BRANCHES, AND WHY THEY ARE DETOURS AND NOT `je`
   FLIPS.

   A `74 rel8` -> `EB rel8` flip would make the engine take its "no cached
   slant shadow" path unconditionally, for the whole process, from `DllMain`
   onwards. That would be the ONE suppression in this file with no runtime
   gate to be inert through, and it would make `renderer=gdi` not stock:
   `tagpu_owndraw_init` runs whatever `renderer=` says, `tagpu_owndraw.on` is a
   play default, and on a lane where `tagpu_overlay_draw` is never called --
   `render_gdi.c` contains no `tagpu_` call at all -- nothing of ours ever
   draws the shadow the flip took away. Every OTHER suppression here already
   asks a flag a live lane sets (`tagpu_native_wrecks_armed`,
   `tagpu_native_owns_obj`), so every other one stands down on that lane by
   itself.

   So the branch is stolen instead of rewritten, and the stub asks `g_ssSkip`
   first: set, it takes the engine's skip path exactly as a flip would; clear,
   it evaluates the engine's ORIGINAL test and does what the engine would have
   done. `g_ssSkip` starts at 0 and is set only by `tagpu_native.c`, once per
   frame, from the render thread -- so a lane that never runs leaves it at 0
   and the engine keeps drawing, which is stock by construction rather than by
   anybody remembering to switch it off.

   THE BYTES, from the pristine build:

     A  0x4592BF  F6 81 13 01 00 00 20   test byte [ecx+0x113],0x20
        0x4592C6  74 5C                  je 0x459324
                  fallthrough 0x4592C8
     B  0x459522  F7 81 10 01 00 00 ...  test dword [ecx+0x110],0x20000000
        0x45952C  74 4A                  je 0x459578
                  fallthrough 0x45952E

   A 5-byte detour needs more room than a 2-byte `je`, so it starts at the
   `test` and swallows the branch: 9 bytes at A, 12 at B. BOTH RANGES ARE
   CHECKED FOR INCOMING BRANCHES, because a flip is immune to one and a detour
   is not: `objdump -d -M intel` over the whole
   image finds exactly one branch into either range -- `0x4594D6 je 0x459522`,
   which lands on the FIRST byte, our jump, and is therefore fine -- and a
   search of the image for each address as a little-endian 32-bit datum finds
   no jump-table entry pointing at any of them. */
#define SSHADOW_A_VA     0x004592BFu
#define SSHADOW_A_TARGET 0x00459324u
#define SSHADOW_A_RESUME 0x004592C8u
#define SSHADOW_B_VA     0x00459522u
#define SSHADOW_B_TARGET 0x00459578u
#define SSHADOW_B_RESUME 0x0045952Eu

/* test + je, exactly as they stand; the `je` is re-emitted by the stub with
   its own rel8, never copied. */
static const unsigned char SSA_STOLEN[9] =
    { 0xF6, 0x81, 0x13, 0x01, 0x00, 0x00, 0x20, 0x74, 0x5C };
static const unsigned char SSB_STOLEN[12] =
    { 0xF7, 0x81, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x74, 0x4A };

/* THE GATE ITSELF. Read by the two stubs on the GAME thread, written by
   `tagpu_native_frame` on the render thread -- one aligned byte, so the store
   is atomic on x86 and no interlock is owed. Its stale directions are not
   symmetric, and WHICH ONE IS WORSE DEPENDS ON THE VIEWPORT:

     stale 1 while nothing paints   -> NO shadow. Silent, and durable if the
                                       cause is durable.
     stale 0 while the viewport is  -> the engine blits its cached slant into a
     KEY-FILLED                        surface the composite then inverts, and
                                       the ALP blend at 0x4B8500 turns palette
                                       254's cyan into an OPAQUE TEAL SILHOUETTE
                                       over the building. MEASURED 2026-09-18:
                                       8779 px on four structures. This is the
                                       worse of the two -- a player reads it as
                                       a broken renderer, not a missing shadow.
     stale 0 while it is NOT        -> a double shadow for a frame. Benign.
     key-filled

   THE TWO INPUTS ARE SHAPED AROUND THAT. `g_ssTerr` is raised by terrown
   BEFORE it publishes its own skip, so no draw can key-fill under a lowered
   gate -- an ordering, not a window. `g_ssPass` carries the slower question of
   whether the unit pass will paint, one frame behind, because there its errors
   are the benign pair above.

   Three things keep a stuck `g_ssSkip` out, and none is "the publisher always
   runs" -- it does not:

     * `tagpu_native_frame` publishes on every frame it REACHES, including 0 on
       each of the seven early returns and 0 whenever no painter reported one the
       frame before (`s_ssSuppress`, tagpu_native.c);
     * terrown lowers `g_ssTerr` when it hands the ground back;
     * and the heartbeat below, for the frames the pass does not reach at all --
       which releases `g_ssPass` only, because releasing `g_ssTerr` there would
       hand the engine a draw the composite is still inverting.
 */
static volatile unsigned char g_ssSkip = 0;
static unsigned               g_ssBeat = 0;   /* frame of the last publish  */

/* THE GATE HAS TWO INPUTS, OR'ED, AND THEY ARE NOT THE SAME KIND OF THING.

     g_ssPass  the unit pass will paint a structure's slant this frame. Published
               once per frame from `tagpu_native_frame`, one frame behind the
               painters by construction, and the 8-frame heartbeat below lowers
               THIS one when the pass stops publishing.
     g_ssTerr  the viewport is key-filled, so whatever the engine draws into its
               own surface arrives on screen as teal rather than as a shadow.

   WHY `g_ssTerr` IS NOT PART OF THE PER-FRAME PUBLISH. As
   `tagpu_terrown_filled()` inside the pass's predicate it would be a frame too
   late. The composite reads the same state and acts on it in the SAME frame,
   while a value the pass publishes is not read by the game thread until the
   next one. On every 0->1 terrown acquisition -- level entry, and recovery from
   any of `terr_bail`'s refusals -- the game thread would key-fill and blit its
   cached slant under a gate that was still down, and the composite would then
   invert over it: up to two frames of exactly the teal this gate exists to
   remove.

   So terrown raises it DIRECTLY, from `tagpu_terrown_set_skip`, before that
   function publishes its own skip byte. Both are render-thread stores, x86 does
   not reorder stores with stores, and the game thread reaches the key fill only
   after latching that byte into `g_terrown_own` (`tagpu_terrown_latch`, at the
   top of the in-play draw); its unit blits come after its own terrain blit. A
   draw that key-fills on a RISE therefore cannot see a lowered gate. That is an
   ordering, not a window.
   THE FALL IS NOT SYMMETRIC. The gate drops right after the skip byte, but a
   draw that latched before the drop keeps key-filling to its end -- and the
   non-in-play callers of DrawGameScreen until the next latch -- so the engine's
   slant can land on a key-filled surface under a lowered gate. It is harmless:
   the composite inverts on `tagpu_terrown_filled()`, which reads the skip byte,
   already 0, so no such frame is inverted and the key reaches no screen as
   teal. */
static volatile unsigned char g_ssTerr = 0;
static unsigned char          g_ssPass = 0;


/* HOW LONG THE GATE MAY STAND WITHOUT A PUBLISH before `tagpu_owndraw_flush`
   lowers it. The publisher is NOT reached on every frame the lane presents:
   `tagpu_overlay_draw` gates it behind `tagpu_overlay.off` and a level
   teardown, and the first of those is a live lever a human can create
   mid-session. Without this the flag would freeze raised
   across any of them and the engine's structure shadow would stay suppressed
   with nothing painting it -- the exact fault the gate exists to remove,
   reached by another door.

   `tagpu_owndraw_flush` runs ABOVE both of those in `tagpu_overlay_draw`,
   which is what makes it the right place. It does NOT cover the frame-ABI
   check at the top of that function, which sits above the flush. Nothing is
   owed for it: the one call site, render_vk.c, fills `f.abi` from the same
   header in the same DLL and passes `&f`, so it cannot fire.

   `tagpu_fxown.c` solves the same problem the same way, at 90 frames. Eight is
   used here because a missing shadow is a picture the player sees, not a
   counter. It is measured against `f->frame_counter`, which is render_vk.c's
   `s_frames`. */
#define SS_BEAT_FRAMES 8

static int               g_armed      = 0;
static char              g_target[32] = "armcom";
static int               g_all        = 0;
static int               g_sshadow    = 0;   /* both branch detours in  */

static void ss_recompute(void);   /* defined below olog2 */
static int               g_buildfx    = 0;   /* 0x458DD0 detour in      */

static volatile unsigned g_skipped    = 0;
static volatile unsigned g_passed     = 0;
static unsigned          g_skip_total = 0, g_pass_total = 0, g_last = 0;

static void olog2(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}
/* The one writer of `g_ssSkip`. `g_sshadow` is the install and cannot change
   after DllMain, so a stranded stub can never be reached with a raised gate.
   Render thread only -- both inputs are written there. */
static void ss_recompute(void)
{
    unsigned char v = (unsigned char)((g_ssTerr || g_ssPass) && g_sshadow);
    if (v != g_ssSkip) {
        g_ssSkip = v;
        olog2(v ? "owndraw: the engine's cached slant shadow is SKIPPED (ours live)"
                : "owndraw: the engine's cached slant shadow is restored");
    }
}

static int ptr_ok(unsigned int p) { return p > 0x00600000u && p < 0x7FFF0000u; }

/* Wipe a composite's planes to the ColorKey (index 1) and far depth (0). The
   engine keeps building and blitting the composite, and an empty plane makes
   that colour-keyed blit a no-op. GAME thread, from the classifier only.

   Safe by construction rather than by the probes: it runs synchronously inside
   the engine's own call to rasterise THIS frame, into the w x h planes the
   engine allocated for it and was about to fill itself. The dimension bound is
   the check on the header's values; `IsBadWritePtr` is a sanity filter, never
   the argument. */
#define GF_WIDTH      0x00
#define GF_HEIGHT     0x02
#define GF_COMPRESSED 0x09
#define GF_PTRCOLOR   0x10
#define GF_PTRDEPTH   0x14

static void wipe_composite(unsigned int frame)
{
    int w, h;
    unsigned char *c, *d;
    if (!ptr_ok(frame)) return;
    if (*(unsigned char*)(frame + GF_COMPRESSED) != 0) return;
    w = *(unsigned short*)(frame + GF_WIDTH);
    h = *(unsigned short*)(frame + GF_HEIGHT);
    if (w <= 0 || h <= 0 || w > 1280 || h > 1280) return;
    c = *(unsigned char**)(frame + GF_PTRCOLOR);
    d = *(unsigned char**)(frame + GF_PTRDEPTH);
    if (ptr_ok((unsigned int)(size_t)c) && !IsBadWritePtr(c, (SIZE_T)w * h))
        memset(c, 1, (size_t)w * h);
    if (ptr_ok((unsigned int)(size_t)d) && !IsBadWritePtr(d, (SIZE_T)w * h))
        memset(d, 0, (size_t)w * h);
}

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

/* DOES owndraw's OWN TARGET COVER THIS OBJECT? `tagpu_owndraw.on`'s token —
   `all`, or a name to match against the def's three name fields. The
   classifier asks it only after the wreck test: a husk's Object3do+0x0C is the
   SHARED SCRATCH feature-unit, whose +0x92 is the UnitInfo array BASE
   (0x422003/0x422009), so a husk would be name-matched against UnitInfo[0],
   an arbitrary loaded def.

   A bad pointer answers NO, which is the same answer as a name that does not
   match, and both mean "the engine keeps this one" — so for the classifier
   those two reads are this function returning 0, and both fall through to the
   same `g_passed++`. */
static int target_covers(unsigned int obj3do)
{
    unsigned int unit, def;
    if (g_all) return 1;
    unit = *(volatile unsigned int*)(obj3do + O3_THISUNIT);
    if (!ptr_ok(unit)) return 0;
    def = *(volatile unsigned int*)(unit + U_UNITTYPE);
    if (!ptr_ok(def)) return 0;
    return name_matches((const char*)(def + UD_NAME)) ||
           name_matches((const char*)(def + UD_UNITNAME)) ||
           name_matches((const char*)(def + UD_OBJNAME));
}

/* IS THIS DRAW A 3D WRECK (a husk)? The classifier's first question.

   How a husk is drawn [BINARY-VERIFIED 2026-09-14]: the feature draw 0x46A610
   splits wreck cells at 0x46A6B5 and GAF wrecks from 3D ones at 0x46A6DC; the
   3D branch at 0x46A721 borrows the UNIT pipeline through a single shared
   scratch "feature unit" allocated once at 0x421F83 and kept at main+0x1420F.
   Two stores set it up, 0x41 bytes before the `call 0x45AC20` that draws:

     0x46A72B  mov [scratch+0x9E], obj3do      ; scratch -> this husk
     0x46A731  mov [obj3do+0x0C], scratch      ; and the husk back at the scratch

   BOTH are tested here, and the second is the point. `scratch+0x9E` alone is
   an ABA: FEATURES_Destroy 0x42474F frees the husk's Object3do and nulls
   [rec+4] at 0x424754 but never clears +0x9E, so the scratch goes on naming a
   freed block. If the allocator hands that block back for a LIVE unit's
   Object3do (0x485DCC / 0x485E0E) the equality holds again and that unit is
   misclassified as a husk — until the next 3D-wreck draw moves the pointer on.
   `obj3do+0x0C == scratch` closes it: a block that has become a live unit's
   carries that unit's own record there (0x485E14), not the scratch. It costs
   no false negative, because a genuine husk has just had +0x0C written by
   0x46A731 in the same straight line as +0x9E.

   Residual, stated rather than assumed: the second constructor path
   (0x485DCC / 0x45A950) was not seen writing +0x0C within the first 0x60 bytes
   of the constructor, so a reused block taken down THAT path could in
   principle still carry a stale scratch there. The test is strictly better
   than `scratch+0x9E` alone, not proven airtight. */
static int is_wreck_draw(unsigned int obj3do)
{
    unsigned int taMain = *(volatile unsigned int*)0x00511DE8u;
    unsigned int scratch;
    if (!ptr_ok(taMain)) return 0;
    scratch = *(volatile unsigned int*)(taMain + 0x1420Fu);
    if (!ptr_ok(scratch)) return 0;
    if (*(volatile unsigned int*)(scratch + 0x9Eu) != obj3do) return 0;
    return *(volatile unsigned int*)(obj3do + O3_THISUNIT) == scratch;
}

int __cdecl tagpu_owndraw_classify(unsigned int obj3do, unsigned int frame)
{
    if (!ptr_ok(obj3do)) { g_passed++; return 0; }
    /* 3D-wreck draws come through the scratch feature-unit *(main+0x1420F)
       (terrain-depth 3.4): during the draw its +0x9E holds THIS obj3do.
       They are NOT covered by the target — the engine keeps painting husks
       unless the native wreck pass is armed, which then owns their pixels:
       skip the rasterise and wipe the composite. */
    if (is_wreck_draw(obj3do)) {
        extern int tagpu_native_wrecks_armed(void);
        if (tagpu_native_wrecks_armed()) {
            g_skipped++;
            wipe_composite(frame);
            return 1;
        }
        g_passed++;
        return 0;
    }
    if (!target_covers(obj3do)) { g_passed++; return 0; }
    /* A UNIT IS NEVER SKIPPED, covered or not: the engine rasterises every
       unit into its composite. Skipping is only safe while something of ours
       is certain to draw the unit instead (gpu-posing.md §4, decision B), and
       nothing the game thread can read promises that: the Vulkan unit pass
       stands down whenever a mirror is missing, the hand-over is short or the
       lane is not ready, and these detours are installed at DLL attach and
       cannot be uninstalled.

       THE ENGINE'S COPY IS NOT A FALLBACK ON EVERY LANE. Measured on
       `one-unit`, 2026-09-19: `OWND target=all skipped=0 passed=55991`, and
       the detours are armed on gdi too. On `renderer=gdi` that engine copy
       REACHES THE PLAYER: the window capture and `tacli shot` are the same
       picture. On `renderer=vulkan` it does not -- the commander is on TA's
       own surface in colour and absent from the presented frame, with our
       unit pass disarmed and with our terrain pass disarmed as well, so
       nothing of ours is covering it. The loss is in the Vulkan composite
       path; its mechanism is not established.

       What a covered unit still gets is the warning below, said ONLY when the
       posed program has tried and failed. Not having run yet is the ordinary
       state of the first frames, before the render thread has built anything,
       and logging that would dress a one-frame state up as a broken driver. */
    {
        extern int tagpu_posedraw_refused(void);
        static int said = 0;
        if (!said && tagpu_posedraw_refused()) {
            said = 1;
            olog2("owndraw: the posed unit program REFUSED to arm. "
                  "On renderer=gdi that means units are drawn by the engine at 8bpp; on renderer=vulkan "
                  "the engine's copy does not currently reach the presented frame, "
                  "so expect NO units rather than 8bpp ones. The posedraw: line "
                  "above says why the program refused.");
        }
    }
    g_passed++;
    return 0;
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

    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(s, 0, MEM_RELEASE);  /* nothing was written; keep nothing */
        return 0;
    }
    t[0] = 0xE9;
    rel = (int32_t)((unsigned int)s - (va + 5));
    memcpy(t + 1, &rel, 4);
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    return 1;
}

/* One structure-shadow branch, stolen whole and replaced by a gated copy.
   `stolen` is the engine's `test` followed by its `je`; `n` is their total
   length; `target` is where the `je` goes and `resume` is its fallthrough.
   0 = wrong build, and then NOTHING is written.

   `stubOut` receives the stub so the ROLLBACK can give it back: installing A
   and then failing on B has to undo A, and an undo that restores the engine's
   bytes but keeps the page alive leaks it for the process. Written only on
   success, so a caller may leave it initialised to NULL and free
   unconditionally. */
static int install_sshadow(unsigned int va, const unsigned char* stolen, int n,
                           unsigned int target, unsigned int resume,
                           unsigned char** stubOut)
{
    unsigned char* t = (unsigned char*)va;
    unsigned char* s;
    unsigned char* p;
    DWORD old;
    int32_t rel;
    const int tlen = n - 2;                  /* the `test`; the `je` is ours */

    if (memcmp(t, stolen, (size_t)n) != 0) return 0;

    s = (unsigned char*)VirtualAlloc(NULL, 0x80,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s) return 0;
    p = s;

    /* cmp byte [g_ssSkip],0 -- before the engine's own test, so the engine's
       flags are the ones its `je` reads */
    *p++ = 0x80; *p++ = 0x3D;
    { unsigned int a = (unsigned int)(size_t)&g_ssSkip; memcpy(p, &a, 4); p += 4; }
    *p++ = 0x00;
    *p++ = 0x75; *p++ = (unsigned char)(tlen + 2 + 5);      /* jne -> TAKE     */
    memcpy(p, stolen, (size_t)tlen); p += tlen;             /* the engine's test */
    *p++ = 0x74; *p++ = 0x05;                               /* je  -> TAKE     */
    *p++ = 0xE9;                                            /* jmp resume      */
    rel = (int32_t)(resume - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0xE9;                                            /* TAKE: jmp target */
    rel = (int32_t)(target - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;

    if (!VirtualProtect(t, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(s, 0, MEM_RELEASE);      /* nothing was written; keep nothing */
        return 0;
    }
    t[0] = 0xE9;
    rel = (int32_t)((unsigned int)s - (va + 5));
    memcpy(t + 1, &rel, 4);
    memset(t + 5, 0x90, (size_t)(n - 5));    /* the tail of the stolen range */
    VirtualProtect(t, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T)n);
    *stubOut = s;
    return 1;
}

/* The engine's bytes back, and the stub's page with them. `stub` may be NULL.
   The order matters: the branch has to stop pointing at the stub BEFORE the
   stub is freed, or a game thread already inside it returns into an unmapped
   page. Only ever called from `tagpu_owndraw_init` at DLL_PROCESS_ATTACH, so
   no thread has reached either yet -- but the order is free and a later caller
   would need it. */
static void restore_sshadow(unsigned int va, const unsigned char* stolen, int n,
                            unsigned char* stub)
{
    unsigned char* t = (unsigned char*)va;
    DWORD old;
    if (!VirtualProtect(t, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(t, stolen, (size_t)n);
    VirtualProtect(t, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T)n);
    if (stub) VirtualFree(stub, 0, MEM_RELEASE);
}

/* THE PUBLISH. Render thread, once per frame, from `tagpu_native_frame` --
   including the frames on which it publishes 0. `g_sshadow` is the install,
   which cannot change after DllMain; `ours` is whether the pass will paint. */
void tagpu_owndraw_set_structshadow(int ours, unsigned frame)
{
    g_ssBeat = frame;
    g_ssPass = (unsigned char)(ours != 0);
    ss_recompute();
}

/* THE KEY-FILL INPUT, raised by `tagpu_terrown_set_skip` and by nothing else.
   It must be raised BEFORE that function stores `g_terrown_skip`, and lowered
   AFTER -- see the long note at `g_ssTerr`. */
void tagpu_owndraw_set_structshadow_terr(int on)
{
    g_ssTerr = (unsigned char)(on != 0);
    ss_recompute();
}

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

void tagpu_owndraw_init(void)
{
    /* worst case is 289 chars: the arm token is capped at 31 and every field
       takes its longest value ("not armed", "SKIP" x3, "HOOKED"). _snprintf
       does NOT terminate a truncation, so the tail is forced rather than
       assumed. */
    char b[384];
    int a, c;

    if (!tagpu_opt_on("tagpu_owndraw.on")) return;

    /* all-or-nothing: both sites checked before either is written, so a
       different build arms nothing rather than half of it. Half would be
       worse than nothing here: a detoured opaque site with g_armed clear
       still skips a husk's rasterise once the wreck pass is armed (classify
       never reads g_armed) while buildfx and the structure shadows stay the
       engine's. */
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
    /* structure shadows: only with "all" -- `g_ssSkip` is one byte for every
       building, so it may drop the engine's shadow only when the native pass
       draws every building's -- and only as a pair: one path redirected and
       not the other would leave a building's shadow depending on which
       composite it was given.
       Installing the detour does NOT suppress anything: `g_ssSkip` decides,
       and it is 0 until the native pass says otherwise. */
    if (g_armed && g_all) {
        unsigned char* stubA = NULL;
        unsigned char* stubB = NULL;
        int sa = install_sshadow(SSHADOW_A_VA, SSA_STOLEN, (int)sizeof SSA_STOLEN,
                                 SSHADOW_A_TARGET, SSHADOW_A_RESUME, &stubA);
        int sb = sa && install_sshadow(SSHADOW_B_VA, SSB_STOLEN, (int)sizeof SSB_STOLEN,
                                       SSHADOW_B_TARGET, SSHADOW_B_RESUME, &stubB);
        if (sa && !sb)
            restore_sshadow(SSHADOW_A_VA, SSA_STOLEN, (int)sizeof SSA_STOLEN, stubA);
        /* B is never rolled back: nothing is installed after it, so nothing
           can fail behind it. `stubB` is written and deliberately not freed --
           the detour stays for the life of the process, like every other one
           this file installs. */
        g_sshadow = sa && sb;
    }

    _snprintf(b, sizeof b,
        "owndraw: %s target=\"%s\" opaque@0x459830=%s nano@0x459C70=%s "
        "buildfx@0x458DD0=%s structshadow@0x4592BF+0x459522=%s "
        "(hooks INSTALLED; every skip is decided per draw against a flag a live "
        "lane sets, so this line does not say anything was skipped)",
        g_armed ? "ARMED" : "not armed", g_target,
        a ? "OK" : "SKIP", c ? "OK" : "SKIP",
        g_buildfx ? "OK" : "SKIP",
        /* the structure pair is installed only under `all`, hence its
           "engine" case */
        g_sshadow ? "HOOKED" : (g_all ? "SKIP" : "engine"));
    b[sizeof b - 1] = 0;
    olog2(b);
}

void tagpu_owndraw_flush(unsigned int frame_counter)
{
    if (!g_armed) return;
    /* THE STRUCTURE-SHADOW WATCHDOG. This function is called from
       `tagpu_overlay.c:365`, ABOVE every gate that can stop the publisher
       running, so it is the one place that still gets a frame when the
       publisher does not. A raised flag with no publish behind it means nobody
       is painting the shadow the engine is no longer drawing, so it comes
       down. Restoring the engine's own draw is the safe direction OUTSIDE a
       key-filled viewport; inside one it is teal, which is why terrown holds
       its own input to the gate (see the note at `g_ssSkip`). */
    if (g_ssPass && frame_counter - g_ssBeat >= SS_BEAT_FRAMES) {
        /* the PASS input only: `g_ssTerr` has terrown's own 90-frame watchdog
           behind it, and lowering it here would hand the engine a draw that the
           composite is still inverting -- teal, which is the worse direction. */
        g_ssPass = 0;
        ss_recompute();
        olog2("owndraw: the unit pass stopped publishing (tagpu_overlay.off, a "
              "failed overlay init, or a level teardown), so its half of the "
              "structure-shadow gate is released -- a raised gate with nobody "
              "painting is the one state this gate exists to prevent");
    }
    if (frame_counter - g_last >= 60) {
        unsigned s = g_skipped, pa = g_passed;
        char b[224];
        g_skipped = 0; g_passed = 0;
        g_skip_total += s; g_pass_total += pa;
        _snprintf(b, sizeof b,
            "OWND target=%s skipped=%u passed=%u (total skipped=%u passed=%u)",
            g_target, s, pa, g_skip_total, g_pass_total);
        olog2(b);
        g_last = frame_counter;
    }
}
