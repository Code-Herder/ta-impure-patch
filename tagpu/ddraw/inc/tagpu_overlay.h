#ifndef TAGPU_OVERLAY_H
#define TAGPU_OVERLAY_H
#include "tagpu.h"
/* Draw our GPU overlay for this frame. Called from the render thread, once per
   present, from render_vk.c -- its only caller: render_gdi.c makes no tagpu_
   call at all, so on the GDI backend this function is never entered. Compiled
   into the fork -- no separate DLL, no runtime LoadLibrary. */
void tagpu_overlay_draw(const TAGPU_FRAME* f);

/* NOTHING HERE CAPTURES WHAT THE RENDERER DREW. What reads the offscreen
   game-res target is `tagpu_vk_shot_record` / `_finish` (tagpu_vk_shot.c),
   driven by the A/B capture (`tagpu_vk.c`), and that is an oracle rather than
   a screenshot verb. It reads a target we own, never the window.

   `tacli shot` is a different verb: it reads the fork's DirectDraw primary
   from the game thread at the flip (`ss_shot_service`, at the end of
   tagpu_overlay.c), never a renderer surface. */
#endif
