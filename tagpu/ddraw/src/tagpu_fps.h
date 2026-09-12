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
#endif
