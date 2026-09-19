#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ddsurface.h"

BOOL ss_take_screenshot(struct IDirectDrawSurfaceImpl* src);

/* THE TRIGGER'S TWO HALVES, AND WHY THEY ARE TWO [the vulkan-only plan, 11-2].
   `tacli shot` used to be polled in render_ogl.c's present loop, which ran
   AFTER the engine had finished writing the primary. Its new host is the
   trigger family on the engine's flip -- but `before_flip` runs at the ENTRY of
   0x4C63A0, and the engine map (exe-reverse-engineering.md, the 0x4C63A0 entry,
   VERIFIED) says that function locks the primary, copies back buffer -> primary
   with 0x4CBBE0, and only then unlocks. So at the trigger's poll the primary
   still holds the PREVIOUS frame, and capturing there is one frame stale --
   invisible on a screen that presents continuously, wrong on one presented
   exactly once, which is the loading screen `tools/uiwalk.py` is built on.

   So the poll ARMS and the unlock CAPTURES. `ss_shot_service` is called from
   dds_Unlock's `DDSCAPS_PRIMARYSURFACE` branch -- the point at which the fork
   itself declares the frame finished -- so the picture is the frame that flip
   presented, by ordering rather than by timing. Both halves run on the game
   thread (the flip detour and the game's own DirectDraw call), so the flag
   needs no interlock. */
void ss_shot_arm(void);
void ss_shot_service(struct IDirectDrawSurfaceImpl* primary);

#endif
