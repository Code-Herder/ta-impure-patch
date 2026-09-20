/* tagpu_featown.c — own the engine's feature draw.

   Every feature pixel in the 8bpp frame comes from ONE leaf:

     0x46A610(OFFSCREEN* ctx, FeatureStruct* tile, int tileX, int tileY)
             stdcall, ret 0x10; exits 0x46A71E / 0x46A76B / 0x46A849

   called from exactly three sites, all inside DrawGameScreen: the flat
   pre-pass (0x469920 gated, 0x46992F plain) and the deferred row sweep
   (0x469ABB). Detouring the leaf rather than the two loops is the narrow cut:
   the loops keep running, so the pre-pass still clears tile->flags bit2 and
   sets it for tall defs, which is the state the row sweep reads — we remove
   the pixels and nothing else.

   The leaf is pure draw. Its decompile (G13a) shows bodies 2 and 3 writing no
   engine state at all, and body 1 (3D wreckage) writing only the DRAW-side
   scratch feature-unit *(main+0x1420F) before calling DrawUnit. Animation is
   advanced by the sim tick, not here: GAFGetCurrentFramePtrAddr only reads
   the anim state's frame index. So skipping it is invisible to the sim.

   Because body 1 goes through DrawUnit, skipping the leaf removes 3D
   wreckage too — tagpu_feat.c therefore only sets the skip while the native
   pass's wreck gather is armed (native.on="… wrecks"), which draws those
   husks natively.

   Prologue: 8B 4C 24 08 53 = `mov ecx,[esp+8]; push ebx` — five
   position-independent bytes ending on an instruction boundary, so the steal
   is clean and the stub resumes at 0x46A615. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_featown.h"
#include "tagpu_opt.h"
#include "tagpu_detour.h"

#define LEAF_FEAT_VA  0x0046A610u   /* ret 0x10 */
static const unsigned char FEAT_STOLEN[5] = { 0x8B, 0x4C, 0x24, 0x08, 0x53 };

/* THE FEATURE SHADOW, AND WHY IT GOES TEAL OVER OUR TERRAIN.

   The leaf draws a feature as shadow-then-body, and `features.md` records that
   each half is "either a plain colour-keyed copy or a 50 % alpha". Which one
   is a per-def bit -- `[esi+0xFE] & 8` -- read twice inside the leaf, and the
   two halves fail differently against a key-filled viewport:

     0x4B7F90  a keyed copy. It writes the sprite's OWN dark indices into the
               engine's surface, the composite reads them as "the engine drew
               here" and lets them through, and the shadow lands correctly.
     0x4B8500  the ALP blend -- the same function tagpu_owndraw.c names for the
               STRUCTURE shadow. It blends the shadow against whatever is
               already in the surface, and once terrown has key-filled the
               viewport that is palette 254. Half of the key's bright cyan is
               an index the composite does not recognise as the key, so our
               fragment is discarded and the blend reaches the screen as an
               opaque teal blob at the foot of every tree.

   MEASURED 2026-09-20 on scenario `e5a`, the engine's own surface: 528431 px
   of raw key (0,255,255) and 19042 px of (0,128,128) -- EXACTLY half of it,
   which is the blend and not a palette coincidence. 15075 of those survived
   into the composite. It is the feature twin of the structure defect landing
   10b measured at 8779 px, and it has the same cause and the same cure.

   THREE SHADOW SITES, AND ONLY TWO ARE STOLEN. The leaf has three draw shapes
   and each tests the shadow separately:

     0x46A6E8  test cl,0x4 / je 0x46A708      shape 1, blit 0x4B7F90 ONLY
     0x46A784  test byte [ebx+0x37F06],0x10   shape 2, blit 0x4B8500 or 0x4B7F90
               / je 0x46A7C0
     0x46A7E0  test byte [ebx+0x37F06],0x10   shape 3, blit 0x4B8500 or 0x4B7F90
               / je 0x46A818

   Shape 1 can only ever reach the keyed copy, so it cannot produce teal and is
   deliberately LEFT ALONE -- taking it would cost a shadow that renders
   correctly today. `ebx` is the dynamic-memory base (`ds:0x511DE8`, loaded at
   0x46A615 and still live on this path, which branches away at 0x46A6B7 before
   shape 1 reloads it), so `+0x37F06` bit 0x10 is the game's own "feature
   shadows" option. NOT clearing that bit is deliberate too: it is a 16-bit
   settings word the options screens read-modify-write at 0x45C76E and
   0x45E37E, so a renderer that cleared it would edit the player's saved
   settings to fix its own compositing -- the mistake a review already caught
   here once, on ScrollSpeed.

   BOTH STOLEN RANGES WERE CHECKED FOR INCOMING BRANCHES, because a detour is
   not immune to one the way a `je` flip is: over the whole image, no branch
   targets any byte of 0x46A784+9 or 0x46A7E0+9, and none of those eighteen
   addresses appears anywhere as a little-endian dword. */
#define FSHAD_A_VA      0x0046A784u
#define FSHAD_A_TARGET  0x0046A7C0u
#define FSHAD_A_RESUME  0x0046A78Du
#define FSHAD_B_VA      0x0046A7E0u
#define FSHAD_B_TARGET  0x0046A818u
#define FSHAD_B_RESUME  0x0046A7E9u

static const unsigned char FSHAD_A_STOLEN[9] =
    { 0xF6, 0x83, 0x06, 0x7F, 0x03, 0x00, 0x10, 0x74, 0x33 };
static const unsigned char FSHAD_B_STOLEN[9] =
    { 0xF6, 0x83, 0x06, 0x7F, 0x03, 0x00, 0x10, 0x74, 0x2F };

volatile unsigned char g_featown_skip = 0;
static int g_installed = 0;
static unsigned g_beat = 0, g_last = 0;

/* THE GATE. Read by the two stubs on the GAME thread, written by terrown when
   it takes the ground or hands it back. One aligned byte, so the store is
   atomic on x86 and no interlock is owed.

   It starts at 0 and only terrown raises it, so every lane that never
   key-fills -- the software renderer, a build with tagpu_terr.on absent, the
   shell before a level loads -- leaves the engine's own shadow exactly as it
   was. Installing the detour suppresses nothing by itself.

   Its stale directions are the same asymmetric pair the structure gate
   documents: stale 1 costs a missing shadow, stale 0 over a key-filled
   viewport costs teal. Terrown raises this BEFORE it publishes its skip and
   lowers it AFTER, which makes the safe direction an ordering rather than a
   window. */
volatile unsigned char g_featshadow_skip = 0;
static int g_fshadow = 0;

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

void tagpu_featown_init(void)
{
    char b[240];
    /* THE SHADOW GATE IS ARMED WHETHER OR NOT WE OWN THE FEATURE DRAW, and so
       it goes in before `tagpu_featown.on` is consulted. What it answers to is
       terrown's key fill, not feature ownership: the teal appears on any lane
       that key-fills the viewport while the ENGINE is still drawing the
       features, which is exactly the lane where `tagpu_featown.on` is off or
       its skip is not taken. Gating the cure on the thing it is not caused by
       would leave the common case broken.

       All-or-nothing, like every other pair in this stack: one shape's shadow
       suppressed and not the other's would make a tree's shadow depend on
       which draw shape its def happens to use. */
    {
        int fa = memcmp((void*)FSHAD_A_VA, FSHAD_A_STOLEN, 9) == 0 &&
                 memcmp((void*)FSHAD_B_VA, FSHAD_B_STOLEN, 9) == 0;
        if (!fa) {
            flog("featown: feature-shadow gate NOT armed - engine bytes differ "
                 "at 0x46A784/0x46A7E0");
        } else {
            int a = tagpu_detour_branch(FSHAD_A_VA, FSHAD_A_STOLEN, 9,
                                        FSHAD_A_TARGET, FSHAD_A_RESUME,
                                        &g_featshadow_skip);
            /* B only if A landed, and A is rolled back if B does not: a lone
               stub would give one shape's shadow a gate the other has not got,
               which is the state the all-or-nothing rule above exists to
               prevent. Restoring A's nine bytes leaks its stub page for the
               process; that is the same trade every rollback here makes, and
               the alternative -- freeing a page a game thread may already be
               inside -- is not one. */
            int bb = a && tagpu_detour_branch(FSHAD_B_VA, FSHAD_B_STOLEN, 9,
                                              FSHAD_B_TARGET, FSHAD_B_RESUME,
                                              &g_featshadow_skip);
            if (a && !bb) tagpu_detour_write(FSHAD_A_VA, FSHAD_A_STOLEN, 9);
            g_fshadow = a && bb;
        }
    }
    if (!tagpu_opt_on("tagpu_featown.on")) {
        _snprintf(b, sizeof b,
            "featown: feature draw left to the engine (tagpu_featown.on absent); "
            "featshadow@0x46A784+0x46A7E0=%s",
            g_fshadow ? "HOOKED" : "SKIP");
        b[sizeof b - 1] = 0;
        flog(b);
        return;
    }
    if (memcmp((void*)LEAF_FEAT_VA, FEAT_STOLEN, 5) != 0) {
        flog("featown: NOT armed — engine bytes differ at 0x46A610");
        return;
    }
    g_installed = tagpu_detour_leaf(LEAF_FEAT_VA, FEAT_STOLEN, 5, &g_featown_skip, 0x10);
    _snprintf(b, sizeof b,
        "featown: %s feature@0x46A610=%d (skip follows tagpu_feat.on) "
        "featshadow@0x46A784+0x46A7E0=%s (follows terrown's key fill; the "
        "hooks are INSTALLED here and suppress nothing until it is up)",
        g_installed ? "ARMED" : "FAILED", g_installed,
        g_fshadow ? "HOOKED" : "SKIP");
    b[sizeof b - 1] = 0;
    flog(b);
}

/* THE KEY-FILL INPUT, raised by `tagpu_terrown_set_skip` and by nothing else --
   the feature twin of `tagpu_owndraw_set_structshadow_terr`, called from the
   same two lines for the same reason. It must be raised BEFORE terrown stores
   its skip and lowered AFTER; see the note at `g_featshadow_skip`. */
void tagpu_featown_set_shadow_terr(int on)
{
    unsigned char v = (unsigned char)(on && g_fshadow);
    if (v != g_featshadow_skip) {
        g_featshadow_skip = v;
        flog(v ? "featown: the engine's feature shadow is SKIPPED (the viewport "
                 "is key-filled, so its ALP blend would land as teal)"
               : "featown: the engine's feature shadow is restored");
    }
}

void tagpu_featown_set_skip(int on)
{
    unsigned char v = (unsigned char)(on && g_installed);
    if (v != g_featown_skip) {
        g_featown_skip = v;
        flog(v ? "featown: engine feature draw SKIPPED (ours live)"
               : "featown: engine feature draw restored");
    }
}

void tagpu_featown_beat(unsigned int frame_counter) { g_beat = frame_counter; }

int tagpu_featown_installed(void) { return g_installed; }

void tagpu_featown_flush(unsigned int frame_counter)
{
    if (!g_installed) return;
    /* if the native pass stops running (overlay off, GL failure, a frame path
       that never reaches it) the engine's features come back rather than the
       map going bare */
    if (g_featown_skip && frame_counter - g_beat > 90) {
        flog("featown: feature pass silent for 90 frames");
        tagpu_featown_set_skip(0);
    }
    if (frame_counter - g_last >= 300) {
        char b[64];
        g_last = frame_counter;
        _snprintf(b, sizeof b, "FEATOWN skip=%u", (unsigned)g_featown_skip);
        flog(b);
    }
}
