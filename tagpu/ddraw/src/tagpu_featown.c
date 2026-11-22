/* tagpu_featown.c — the engine draws no feature on a Vulkan launch.

   Every feature pixel in the 8bpp frame comes from ONE leaf:

     0x46A610(OFFSCREEN* ctx, FeatureStruct* tile, int tileX, int tileY)
             stdcall, ret 0x10; exits 0x46A71E / 0x46A76B / 0x46A849

   called from exactly three sites, all inside DrawGameScreen: the flat
   pre-pass (0x469920 gated, 0x46992F plain) and the deferred row sweep
   (0x469ABB). Detouring the leaf rather than the two loops is the narrow cut:
   the loops keep running, so the pre-pass still clears tile->flags bit2 and
   sets it for tall defs, which is the state the row sweep reads — the pixels
   go and nothing else.

   The leaf is pure draw. Its decompile shows bodies 2 and 3 writing no
   engine state at all, and body 1 (3D wreckage) writing only the DRAW-side
   scratch feature-unit *(main+0x1420F) before calling DrawUnit. Animation is
   advanced by the sim tick, not here: GAFGetCurrentFramePtrAddr only reads
   the anim state's frame index. So removing it is invisible to the sim.

   WHY NOTHING IS LOST. The engine's features never reach the presented frame
   on the Vulkan backend: the world phase stamps them and `publish` refuses
   them (tagpu_gui_hook.c), so they land only in the golden source `tacli shot`
   reads, and every feature on screen is the Vulkan feature pass's. Body 1's
   3D wreckage went with DrawUnit's composite already (tagpu_owndraw.c). The
   picture with every engine draw in it is GDI's, the tagpu_gdi.on oracle,
   where nothing here installs; a launch is Vulkan or GDI for its life, so the
   detour installs at attach or never.

   Prologue: 8B 4C 24 08 53 = `mov ecx,[esp+8]; push ebx` — five
   position-independent bytes ending on an instruction boundary, so the steal
   is clean and the stub resumes at 0x46A615. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_featown.h"
#include "tagpu_detour.h"
#include "config.h"         /* g_config.gdi: the renderer chosen at attach */
#include "tagpu_log.h"

#define LEAF_FEAT_VA  0x0046A610u   /* ret 0x10 */
static const unsigned char FEAT_STOLEN[5] = { 0x8B, 0x4C, 0x24, 0x08, 0x53 };

/* THE BYTE THE STUB TESTS, AND IT IS NEVER WRITTEN: tagpu_detour's leaf stub
   is `cmp byte [flag],0; jz resume; ret N`, so the leaf returns at once. */
static volatile unsigned char s_removed = 1;

void tagpu_featown_init(void)
{
    int installed;
    if (g_config.gdi) return;
    if (memcmp((void*)LEAF_FEAT_VA, FEAT_STOLEN, 5) != 0) {
        tagpu_log("featown: the engine's feature draw is KEPT — engine bytes differ at 0x46A610");
        return;
    }
    installed = tagpu_detour_leaf(LEAF_FEAT_VA, FEAT_STOLEN, 5, &s_removed, 0x10);
    tagpu_log(installed ? "featown: the engine's feature draw is removed (0x46A610, Vulkan)"
                        : "featown: the engine's feature draw is KEPT — 0x46A610 could not be written");
}
