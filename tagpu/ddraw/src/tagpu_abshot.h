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
   is a pointer to resolve. Seven of the eleven this needs are not among the
   fork's own globals in opengl_utils.h (glGetFloatv, glIsEnabled, glDisable,
   glClearColor, glReadPixels, and glDepthMask and glClearDepth since G19e's
   second world pass added the depth clear) and are resolved here; the other
   four -- glClear, glEnable, glGetIntegerv, glPixelStorei -- come from the
   fork. What the G19d review found
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
    GLfloat   cleardepth;
    GLboolean scissor;
    GLint     depthmask;
    GLint     pack;
    unsigned  flags;
    int       live;            /* 0 = `begin` never ran; `end` is then a no-op */
} TAGPU_ABSHOT;

/* CLEAR THE DEPTH BUFFER TOO. For a pass that depth-tests, the colour clear
   alone is half an oracle: the GL twin would test against whatever the passes
   BEFORE it left in the depth buffer (the terrain, for the feature pass) while
   the Vulkan lane's render pass starts from a cleared one, and every fragment
   the two disagree about would read as a port failure. With this, both halves
   are one pass over black with nothing in the depth buffer under it. The depth
   WRITE MASK is forced on for the clear and put straight back, because
   glClear(GL_DEPTH_BUFFER_BIT) is masked by it -- a pass that had just turned
   writes off would otherwise clear nothing and say nothing.
   Passes that do not test depth leave this off: clearing a buffer they never
   read is a change to the frame for no gain. */
#define TAGPU_ABSHOT_DEPTH    1u

/* PUT THE SCISSOR BACK BEFORE THE PASS DRAWS. The clear is always unscissored
   -- a scissor left on by an earlier pass would black a rectangle instead of
   the frame -- but a world pass is CLIPPED to the world viewport by a scissor
   its caller set, and drawing it unclipped is measuring a pass the player never
   sees. With this the scissor is restored the instant the clear is done, and
   the Vulkan edition sets the same rectangle. Without it the draw is
   unscissored, which is what tagpu_fps.c and tagpu_scaffold.c have always
   measured and what their 0-px results are results about. */
#define TAGPU_ABSHOT_SCISSOR  2u

/* THE FRAMEBUFFER BEING READ ALREADY HOLDS THE GAME FRAME TOP ROW FIRST, so do
   not turn the rows over on the way out.

   `glReadPixels` hands back the bottom row of the READ framebuffer first, and a
   PPM's first row is the top one, so the default here is to reverse them. That
   is right for a pass drawn into the DEFAULT framebuffer with GL's own
   convention -- tagpu_gui_surf.c's LAY_VS (`1 - a.y*2`, which is what the GUI
   capture wraps), tagpu_fps.c (`1 - aPos.y/uFrame.y*2`) and tagpu_scaffold.c
   (an NDC rect built y-up), so their content sits in the frame the way GL
   expects. Note that gui_surf.c ALSO carries QVS, which is y-DOWN -- it draws
   the offscreen 1x mirror, not the composite, and is not what is captured
   here.

   It is wrong for the WORLD passes. They write `gl_Position.y = p.y/uGame.y*2 -
   1` on the engine's screen-space y, which grows DOWNWARD, so clip +1 is the
   BOTTOM of the game frame and the world FBO's first row IS the game's top row.
   Reversing it produces an upside-down PPM -- which is what gpu-status §2.28
   describes and accepted, because the Vulkan half was mirrored to match by a
   negative-height viewport and the two lined up. They no longer are: the lane
   presents its own picture now, so the world passes are drawn the right way up
   and their captures have to be as well. [Landing 5b, 2026-09-17.] */
#define TAGPU_ABSHOT_TOPDOWN  4u

/* Black the whole frame, scissor off for the clear itself. Call immediately
   before the pass draws. `flags` is 0 or any of the three above. */
void tagpu_abshot_begin(TAGPU_ABSHOT* s, unsigned flags);

/* Read the viewport back and write it as a binary PPM, then put every piece of
   state `begin` moved back. Call immediately after the pass draws, before
   anything else in the frame does. `tag` names the pass in the log.

   THE RECT IS THE VIEWPORT'S, asked for rather than assumed, because that is
   what maps the pass's coordinates onto the target -- and the Vulkan lane's
   swapchain covers the window's CLIENT area, which is the same rect only when
   the fork is not letterboxing. tools/vk-ab.py refuses two captures of
   different sizes rather than scaling one.

   RETURNS 1 ONLY WHEN THE CAPTURE REACHED THE DISK, and the caller must not
   claim the Vulkan half of the A/B on anything else. `begin` refusing an entry
   point, a viewport that is not sane, a failed malloc, a file that would not
   open and a short write all return 0 -- and on every one of them the _gl.ppm
   of the PREVIOUS run is still lying on the disk, so a Vulkan half claimed
   anyway would be diffed against a capture of a different frame and reported
   as a port failure. That guard is what tagpu_fps.c spelled `can` before G19e
   moved this code here.

   THE GUARD IS ABOUT A STALE FILE, NOT ABOUT THE GL LANE, and on the
   vulkan-only lane it is established elsewhere. `renderer=vulkan` has no GL
   context, so a ported pass gathers, publishes and draws nothing there: this
   function is never called, `_gl.ppm` is never written, and a Vulkan half that
   waited on it would never be claimed -- on the only lane that presents. The
   property the rule was protecting is instead made true by construction over
   there: tagpu_vk.c unlinks the target `_vk.ppm` the instant a claim is seen,
   so a file that exists was written by this arming and a refused capture leaves
   nothing to diff. Where BOTH lanes run -- `renderer=opengl` with
   `tagpu_vk.on` -- the rule above still holds and is still what a pass applies;
   the unlink is belt and braces there, and covers the two refusals that used to
   leave a stale `_vk.ppm` behind (a surface whose images do not carry
   TRANSFER_SRC, and two levers armed in one frame). [The vulkan-only plan,
   landing 4b.] */
int tagpu_abshot_end(TAGPU_ABSHOT* s, const char* path, const char* tag);

#endif
