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
   report. This draws in GL after the world composite instead, so it neither
   flickers nor perturbs the frame it measures. */
#include "tagpu.h"

/* Render thread, per present, GL context current. Polls the trigger on its own
   cadence and draws nothing at all when it is absent. */
void tagpu_fps_present(const TAGPU_FRAME* f);
void tagpu_fps_glreset(void);      /* the GL context changed: drop our objects */

/* ---- the Vulkan edition of this pass (Phase G / G19d) ----
   The quads `tagpu_fps_present` just built -- (x, y, u, v) per vertex, `nv`
   vertices, in a frame `fw` x `fh` game pixels -- so that the Vulkan lane draws
   THE SAME GEOMETRY rather than a second implementation of it. The pointer is
   into this file's static array and is valid until the next present.

   IT HANDS THEM OVER ONCE. A second call in the same frame returns 0, which is
   what makes a frame the overlay did not run on draw nothing instead of
   repeating the last one. Returns 0 when there is nothing to draw.

   `ab` comes back 1 on the ONE frame this pass captured `tagpu_fps_gl.ppm`, so
   the Vulkan lane captures the same frame rather than whichever one its own
   lever poll happened to land on. */
int tagpu_fps_quads(const float** v, int* nv, int* fw, int* fh, int* ab);

/* The most vertices `tagpu_fps_quads` can ever hand over -- MAXCH quads of
   QUADV vertices, checked against those two in tagpu_fps.c so the two cannot
   drift. The Vulkan pass sizes its buffer from this AND re-checks `nv` against
   it, because a #define in one file bounding an array in another is a bound
   only while both are read together. */
#define TAGPU_FPS_MAXV (48 * 6)
#endif
