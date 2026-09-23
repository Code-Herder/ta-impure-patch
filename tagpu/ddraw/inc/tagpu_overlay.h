#ifndef TAGPU_OVERLAY_H
#define TAGPU_OVERLAY_H
#include "tagpu.h"
/* Draw our GPU overlay for this frame. Called from the render thread, once per
   present, from render_vk.c -- render_ogl.c was its other caller until landing
   11-2 deleted that lane, and render_gdi.c makes no tagpu_ call at all, so on
   `renderer=gdi` this function is never entered. Compiled into the fork -- no
   separate DLL, no runtime LoadLibrary. */
void tagpu_overlay_draw(const TAGPU_FRAME* f);

/* THE GL CAPTURE IS GONE (11-5e-1) and nothing replaces it here.
   `tagpu_overlay_capture_begin` / `_capture_end` / `_target_fbo` rendered the
   frame into an FBO we owned and read it back to tagpu_gl.ppm, so that a
   glshot of an off-screen or obscured window was correct: glReadPixels on the
   default framebuffer is defined only for pixels that pass the ownership test,
   which is what made every earlier glshot garbage. `_glreset` forgot those ids
   when the fork's GL context changed.

   All four had NO CALLER in this build [masked scan, 11-5e-1]: their caller was
   render_ogl.c's present loop, deleted in 11-2.

   THE VERB WENT WITH THEM AND IS NOT COMING BACK. `tacli glshot` no longer
   exists, for the same reason: it captured a GL framebuffer this process no longer has.
   An earlier draft of this tombstone said it had "a Vulkan answer" -- it does
   not, and a reader acting on that sentence gets a hard error [the 11-5e-1
   review's MEDIUM-4]. What DOES read the offscreen game-res target is
   `tagpu_vk_shot_record` / `_finish`, driven by the A/B capture
   (`tagpu_vk.c`), and that is an oracle rather than a screenshot verb.

   THE OWNERSHIP-TEST LESSON IS THE PART WORTH KEEPING: a capture must read a
   target we own, never the window, because glReadPixels on the default
   framebuffer is defined only for pixels that pass the ownership test. The
   A/B capture satisfies it for the same reason, by construction.

   `tacli shot` is a different verb and is untouched: it reads the fork's
   DirectDraw primary from the game thread at the flip (`ss_shot_service`, at
   the end of tagpu_overlay.c), never a GL surface. */
#endif
