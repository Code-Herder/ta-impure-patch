#ifndef TAGPU_OVERLAY_H
#define TAGPU_OVERLAY_H
#include "tagpu.h"
/* Draw our GPU overlay for this frame. Called from the render thread, GL context
   current, after the game quad and before SwapBuffers. Compiled into the fork —
   no separate DLL, no runtime LoadLibrary. */
void tagpu_overlay_draw(const TAGPU_FRAME* f);

/* Capture the finished frame to tagpu_gl.ppm.  The frame is rendered into an FBO
   we own so the read never touches the window's back buffer: pixels outside the
   visible desktop region fail the pixel-ownership test and read back undefined,
   which is what made every glshot of an off-screen or obscured window garbage.
   capture_begin() runs before anything is drawn (returns 1 when armed and bound);
   capture_end() reads the FBO and blits it to the window so the frame still
   presents.  target_fbo() is "the default draw target for this frame": 0 normally,
   the capture FBO while armed -- every "bind framebuffer 0" on the frame path goes
   through it, so intermediate passes return to the capture target, not the window. */
int          tagpu_overlay_capture_begin(int w, int h);
void         tagpu_overlay_capture_end(const TAGPU_FRAME* f);
unsigned int tagpu_overlay_target_fbo(void);
/* the GL context changed: our capture ids are dead, forget them */
void         tagpu_overlay_glreset(void);
#endif
