#ifndef TAGPU_OVERLAY_H
#define TAGPU_OVERLAY_H
#include "tagpu.h"
/* Draw our GPU overlay for this frame. Called from the render thread, GL context
   current, after the game quad and before SwapBuffers. Compiled into the fork —
   no separate DLL, no runtime LoadLibrary. */
void tagpu_overlay_draw(const TAGPU_FRAME* f);
void tagpu_overlay_capture(const TAGPU_FRAME* f);
#endif
