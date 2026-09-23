#ifndef TAGPU_FPS_H
#define TAGPU_FPS_H
/* tagpu_fps.c -- the on-screen frame-rate readout, a row on the render-options
   screen (Off|On). Armed by `tagpu_fps.on`, which the menu writes exactly as it
   writes `tagpu_ss.off`, so the file and the row are one setting and either can
   be driven by hand.

   WHY NOT THE ONE THAT ALREADY EXISTS. cnc-ddraw's `dbg_draw_frame_info_start`
   is behind `tagpu_fpsosd.on` already -- but it is compiled only under _DEBUG,
   and it GDI-draws into the engine's 8bpp surface, where it beats against the
   engine's own redraw of that area and flickers (its own comment says so). A
   debug build would also move the very numbers a frame-rate readout exists to
   report. This builds quads that the Vulkan pass (tagpu_vk_fps.c) draws over
   the finished frame instead, so it neither flickers nor perturbs the frame it
   measures. */
#include "tagpu.h"

/* Render thread, once per frame, from `tagpu_overlay_draw`. Polls the trigger
   on its own cadence and builds nothing at all when it is absent. */
void tagpu_fps_present(const TAGPU_FRAME* f);

/* ---- the Vulkan edition of this pass ----
   The quads `tagpu_fps_present` just built -- (x, y, u, v) per vertex, `nv`
   vertices, in a frame `fw` x `fh` game pixels -- for the Vulkan pass to draw;
   the geometry is built here and nowhere else. The pointer is
   into this file's static array and is valid until the next present.

   IT HANDS THEM OVER ONCE, so one frame's vertices can never be drawn twice.
   A frame the overlay was SKIPPED on still gets the newest vertices there are,
   which is a readout one frame stale -- a digit, not a fault. Returns 0 when
   there is nothing to draw, and when they have already been taken.

   `ab` comes back 1 on the ONE frame `tagpu_fps.ab` latched its claim, so the
   Vulkan lane captures THAT frame rather than whichever one its own lever poll
   happened to land on. It does not mean a file was written: the seam writes
   the capture later in the frame, if it gets that far. */
int tagpu_fps_quads(const float** v, int* nv, int* fw, int* fh, int* ab);

/* The most vertices `tagpu_fps_quads` can ever hand over -- MAXCH quads of
   QUADV vertices, checked against those two in tagpu_fps.c so the two cannot
   drift. The Vulkan pass sizes its buffer from this AND re-checks `nv` against
   it, because a #define in one file bounding an array in another is a bound
   only while both are read together. */
#define TAGPU_FPS_MAXV (48 * 6)
#endif
