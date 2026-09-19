#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ddsurface.h"

BOOL ss_take_screenshot(struct IDirectDrawSurfaceImpl* src);

/* THE TRIGGER'S TWO HALVES, AND WHY THEY ARE ONE PASS APART [the vulkan-only
   plan, 11-2]. `tacli shot` used to be polled in render_ogl.c's present loop,
   which ran AFTER the engine had finished writing the primary, so it answered
   on the GL lane only. Its host is now the trigger family on the engine's
   flip -- but `before_flip` runs at the ENTRY of 0x4C63A0, and the engine map
   (the 0x4C63A0 entry, VERIFIED) says that function locks the primary, copies
   back buffer -> primary with 0x4CBBE0, and only then unlocks. So at the poll
   the primary holds the frame the PREVIOUS flip presented.

   That is not a reason to capture somewhere else; it is the clock. The family
   SERVICES a pending arm before it polls for a new one, so a trigger seen on
   pass P is answered on pass P+1 with a frame the engine presented in between.
   Both calls are the same detour on the game thread, so the flag needs no
   interlock and no fence, and the ordering is the engine's own copy.

   THE GAP IS ONE FAMILY PASS, NOT ONE FLIP -- the family is behind a 16 ms QPC
   gate and the shell flips thousands of times a second, so P and P+1 can be
   ~80 flips apart. The property that matters survives (the picture is never
   older than the trigger), but the bound is a pass, and saying "one flip" was
   an overclaim this landing's third review caught.

   AND THE FRAME EXISTS BECAUSE OF THE DIRECTDRAW ARM. The flip has two: the
   arm at 0x4C6475 writes our primary, and the one at 0x4C63C0 does
   GetDC/BitBlt/ReleaseDC and touches no DirectDraw surface. The trigger family
   runs at the flip's entry either way, so the FAMILY is arm-independent -- but the
   picture is only fresh on the DirectDraw arm, and on a build that took the
   other one nothing in this fork would see a frame at all.

   `dds_Unlock`'s primary branch was tried as the capture site and reverted:
   it is gated on `g_ddraw.render.run` (cleared by the window thread on
   deactivate, minimise, fullscreen toggle and mode change) and it is reached
   only when the flip takes its DirectDraw arm at 0x4C6475, never on the GDI
   BitBlt arm at 0x4C63C0. An arm could sit there indefinitely, then answer
   with an unrelated frame stamped with the wrong time. `after_flip` was
   rejected too: the detour only hijacks the return when `before_flip` returns
   NON-ZERO (tagpu_detour.c's `test eax,eax; jz`), and `before_flip` returns 0
   at `if (!s_opsLive)` -- i.e. `after_flip` does not run without
   `tagpu_gui.on`, which is the shape of the 10c-2 HIGH. */
void ss_shot_arm(void);
void ss_shot_service(struct IDirectDrawSurfaceImpl* primary);

#endif
