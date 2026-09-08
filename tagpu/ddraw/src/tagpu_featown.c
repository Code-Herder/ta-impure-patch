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

volatile unsigned char g_featown_skip = 0;
static int g_installed = 0;
static unsigned g_beat = 0, g_last = 0;

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

void tagpu_featown_init(void)
{
    char b[160];
    if (!tagpu_opt_on("tagpu_featown.on")) return;
    if (memcmp((void*)LEAF_FEAT_VA, FEAT_STOLEN, 5) != 0) {
        flog("featown: NOT armed — engine bytes differ at 0x46A610");
        return;
    }
    g_installed = tagpu_detour_leaf(LEAF_FEAT_VA, FEAT_STOLEN, 5, &g_featown_skip, 0x10);
    _snprintf(b, sizeof b,
        "featown: %s feature@0x46A610=%d (skip follows tagpu_feat.on)",
        g_installed ? "ARMED" : "FAILED", g_installed);
    flog(b);
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
