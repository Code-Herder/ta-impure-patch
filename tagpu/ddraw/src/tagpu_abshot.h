#ifndef TAGPU_ABSHOT_H
#define TAGPU_ABSHOT_H
/* The GL half of a Phase G A/B, once, for every ported pass. Implementation:
   tagpu_abshot.c. The Vulkan half is tagpu_vk_shot.c.

   WHAT AN A/B IS HERE. Route D gives the Vulkan lane a window of its own, so
   nothing on the GL side can see what Vulkan drew -- `tacli glshot` reads the
   GL framebuffer and the Vulkan frame is not in it. Each lane therefore
   captures its own half of ONE frame and tools/vk-ab.py diffs the two files.
   This is the GL half: black the frame, let the pass draw, read it back.

   THE BACKGROUND IS THE HARD PART OF A WORLD PASS, and this is the answer
   G19e settled on. `tagpu_fps.c` could clear the whole frame because the
   readout is the last thing drawn; a world pass has the rest of the frame
   under it on the GL side and a bare clear under it on the Vulkan side. So
   the GL lane is made to draw its pass ALONE: `begin` clears the frame to
   black immediately before the pass draws and `end` reads it back immediately
   after, BEFORE anything later in the frame has run. What is compared is then
   one pass over black against one pass over black. The player sees one frame
   with everything drawn before the pass missing from it, which is what a
   measuring lever costs and why it is one frame.

   THE ENTRY POINTS ARE RESOLVED IN ONE PLACE, ONCE, AND A MISSING ONE SAYS SO.
   This DLL is ddraw.dll: it does not link opengl32, it loads it, so even GL 1.1
   is a pointer to resolve. Five of the nine this needs are not among the fork's
   own globals in opengl_utils.h (glGetFloatv, glIsEnabled, glDisable,
   glClearColor, glReadPixels) and are resolved here. What the G19d review found
   in tagpu_fps.c was not the resolving but the SILENCE -- a context missing one
   made the lever do nothing at all, no clear, no capture and no line in the
   log, retried every poll for the session. Here there is one `init`, one
   message naming the entry point that was missing, and one latch. */

#include "glcorearb.h"

/* What `begin` saved, so that `end` can put it back. A lever that exists to
   measure the renderer is the last thing that should change it, and the clear
   colour, the scissor enable and GL_PACK_ALIGNMENT were all left moved for the
   session by the first version of this in tagpu_fps.c. */
typedef struct {
    GLfloat   clear[4];
    GLboolean scissor;
    GLint     pack;
    int       live;            /* 0 = `begin` never ran; `end` is then a no-op */
} TAGPU_ABSHOT;

/* Black the whole frame, scissor off -- a scissor left on by an earlier pass
   would clear a rectangle rather than the frame. Call immediately before the
   pass draws. */
void tagpu_abshot_begin(TAGPU_ABSHOT* s);

/* Read the viewport back and write it as a binary PPM, then put every piece of
   state `begin` moved back. Call immediately after the pass draws, before
   anything else in the frame does. `tag` names the pass in the log.

   THE RECT IS THE VIEWPORT'S, asked for rather than assumed, because that is
   what maps the pass's coordinates onto the target -- and the Vulkan lane's
   swapchain covers the window's CLIENT area, which is the same rect only when
   the fork is not letterboxing. tools/vk-ab.py refuses two captures of
   different sizes rather than scaling one. */
void tagpu_abshot_end(TAGPU_ABSHOT* s, const char* path, const char* tag);

#endif
