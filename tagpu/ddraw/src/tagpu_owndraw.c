/* tagpu_owndraw.c -- the engine draws no unit on a Vulkan launch (tagpu_owndraw.h). */
#include <windows.h>
#include "tagpu_owndraw.h"
#include "tagpu_log.h"
#include "tagpu_detour.h"
#include "config.h"         /* g_config.gdi: the renderer chosen at attach */

/* THE ENGINE DRAWS NO UNIT ON THE VULKAN BACKEND.

   DrawUnit 0x45AC20 reposes its unit and then, as its last act, calls the
   composite 0x458810 at 0x45AE73: the unit's 8bpp picture built by the
   rasterisers 0x459830/0x459C70, blitted with its shadow by 0x459200, the build-state
   effect 0x458DD0 stamped on it. That call is the game thread's `sweep`, and
   at the zoom floor of a large battle it is most of the frame. Nothing the
   simulation reads is written under it: the census of DrawGameScreen's stores
   (engine/render.md, *Every store family*) classes the composite, the
   rasterisers and their surfaces draw-only, and the cursor's unit pick reads
   the model's box (0x48C6A0), not these pixels. The pose cache, which the
   simulation does read (the SweetSpot point, EXPLODE), is written before
   0x45AE73 and is kept.

   On the Vulkan backend none of those pixels reach the presented frame: every
   unit and husk there is the unit pass's (modules/units.md, *The engine draws
   no unit on a Vulkan launch*, has the capture).
   A launch is Vulkan or GDI for its life -- `g_config.gdi`, the tagpu_gdi.on
   oracle, is fixed by cfg_load before this runs, and the Vulkan lane never
   hands a session to GDI (render_vk.c) -- so on a Vulkan launch the call is
   removed outright: `add esp,8` pops the two arguments DrawUnit pushed
   (0x45AE6B, 0x45AE72), as the composite's `ret 8` does. EAX, ECX, EDX and the
   flags are dead there: DrawUnit's epilogue 0x45AE78..0x45AE7D pops its own
   registers and returns, reading none of them (tagpu_nodraw.c, THE REPOSE,
   rests on the same reading for its gate on this call, which it installs only
   under tagpu_gdi.on, so each launch has one writer of the site).

   DrawUnit has three callers, the sweeps 0x469A00/0x469BA3 and the 3D-wreck
   draw 0x46A762, all in DrawGameScreen's world, and the composite has this one
   caller; neither address is stored as a datum anywhere in the image
   [DISASSEMBLED 2026-11-21, a scan of every E8 and every dword]. */
#define COMPOSITE_SITE   0x0045AE73u
#define COMPOSITE_VA     0x00458810u

static void olog2(const char* s)
{
    tagpu_log(s);
}

/* The patch at 0x45AE73 on a Vulkan launch, after checking the bytes the
   argument above rests on: the call itself, the two pushes before it and the
   epilogue after it. Logged either way. */
static void remove_composite(void)
{
    static const unsigned char PUSHES[23] = {                /* 0x45AE5C..0x45AE72 */
        0x8B, 0x4C, 0x24, 0x18, 0xA1, 0xE8, 0x1D, 0x51, 0x00, 0x8B, 0x97, 0x9E, 0x00, 0x00, 0x00,
        0x51, 0x8B, 0x88, 0x7B, 0x43, 0x01, 0x00, 0x52
    };
    static const unsigned char EPILOGUE[8] = {               /* 0x45AE78..0x45AE7F */
        0x5D, 0x5E, 0x5F, 0x5B, 0x59, 0xC2, 0x08, 0x00
    };
    static const unsigned char POP_ARGS[5] = { 0x83, 0xC4, 0x08, 0x90, 0x90 };  /* add esp,8 */
    if (g_config.gdi) return;
    if (!tagpu_detour_bytes_ok(0x0045AE5Cu, PUSHES, sizeof PUSHES) ||
        !tagpu_detour_redirect_ok(COMPOSITE_SITE, COMPOSITE_VA) ||
        !tagpu_detour_bytes_ok(0x0045AE78u, EPILOGUE, sizeof EPILOGUE)) {
        olog2("owndraw: the engine's unit draw is KEPT -- 0x45AE5C..0x45AE7F holds other bytes");
        return;
    }
    if (!tagpu_detour_write(COMPOSITE_SITE, POP_ARGS, sizeof POP_ARGS)) {
        olog2("owndraw: the engine's unit draw is KEPT -- 0x45AE73 could not be written");
        return;
    }
    olog2("owndraw: the engine's unit draw is removed (0x45AE73, Vulkan)");
}

void tagpu_owndraw_init(void)
{
    remove_composite();
}
