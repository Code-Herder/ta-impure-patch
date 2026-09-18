#ifndef TAGPU_SURF_H
#define TAGPU_SURF_H

#include "tagpu_overlay.h"      /* TAGPU_FRAME: the frame this snapshot belongs to */

/* TA'S OWN FRAME, AS THE BOTTOM LAYER OF THE COMPOSITE — one snapshot a frame,
   owned here rather than by whichever pass happened to want it first.

   WHAT THIS IS FOR. The engine draws its whole screen into an 8-bit surface and
   the fork puts that on the display; everything of ours is drawn OVER it, and
   what we do not draw is what the player still sees. On the GL lane the fork
   does that itself — `render_ogl.c` uploads the surface into
   `g_ogl.surface_tex_ids` and `g_ogl.main_program` (PASSTHROUGH_VERT_SHADER +
   PALETTE_FRAG_SHADER) draws it before any pass of ours runs, which is why
   `tagpu_gui.off` still shows a game there. On `renderer=vulkan` there was no
   such step: the seam cleared to the lever's colour and TA's surface reached
   the frame ONLY through the GUI pass's mirror hand-over, so turning the UI
   layer off left a flat colour. That is the blind spot the vulkan-only plan's
   landing 1 named and 4c closes.

   WHY IT IS ITS OWN MODULE. `tagpu_gui_surf.c` already took this snapshot, for
   `s_mHand.eng`, and a second copy beside it would be two readers of the same
   engine-adjacent memory with nothing forcing them to agree about the bound.
   But leaving it there would mean the Vulkan lane's BOTTOM layer existed only
   while the UI pass ran, which is the coupling 4c-1 exists to undo. So the
   snapshot moves here and both read it: the GUI hand-over publishes what this
   took, and the Vulkan base blit draws it.

   IT IS NOT AN ENGINE READ, and so this file is not on
   `tagpu/ddraw/thread-split.allow`. `g_ddraw.primary` is the FORK's own
   DirectDraw surface object, not the game's memory at an absolute address —
   the same standing `tagpu_gui_surf.c` read it under.

   THE LIFETIME ARGUMENT IS THE CRITICAL SECTION'S, and it is a real one rather
   than a hope. `IDirectDrawSurface.c` NULLs `g_ddraw.primary` INSIDE
   `g_ddraw.cs` and frees the object only after leaving it, so a pointer both
   read and dereferenced inside the section is live-or-NULL and never freed
   underneath. `dds_Flip` swaps `This->surface` with the backbuffer's inside the
   same section, so the row loop sees one coherent buffer rather than half of
   each. And the bound comes from the object being read — the PRIMARY's own
   width, height and pitch — never from `g_ddraw.width/height`, which is the
   device MODE and disagrees with the primary between a mode change and the
   primary being recreated.

   ONE SNAPSHOT A FRAME, taken from `tagpu_overlay_draw` before any pass
   gathers, so every consumer in that frame sees the same bytes. Two passes
   reading the engine's surface at two instants is exactly how the lanes end up
   compositing different moments of the same frame. */

/* The widest surface this module will copy. A bound on an allocation and on a
   memcpy, so it is stated here and re-checked where it is used rather than
   trusted across a file boundary. */
#define TAGPU_SURF_MAXDIM 4096

/* WHAT WAS TAKEN, and where it goes. One struct rather than seven out-params,
   which is the shape `tagpu_feat_handover` settled on for the same reason. */
typedef struct {
    /* `w * h` 8-bit indices, TIGHTLY PACKED: the copy removes the primary's
       pitch, so a consumer uploads `w * h` and has no row stride of its own to
       get wrong. */
    const unsigned char* bytes;
    int                  w, h;
    /* 256 four-byte entries, R,G,B at [0],[1],[2] — the palette the screen is
       being shown with, snapshotted in the SAME call as the bytes so the two
       cannot be a frame apart. */
    const unsigned char* pal;
    /* Bumped only when the bytes CHANGED, for a consumer that uploads to a
       device and wants to skip an upload it already holds. TA redraws its whole
       screen far less often than we present. */
    unsigned             serial;
    /* WHERE IT GOES, in window pixels: the frame's letterboxed viewport, the
       same rect the fork gives its own upload on the GL lane. Carried here
       rather than re-derived by the consumer because a pass's `record` is handed
       the swapchain extent and nothing else, and a bottom layer drawn to the
       whole window instead of the viewport would paint over the letterbox. */
    int                  dx, dy, dw, dh;
} TAGPU_SURFFRAME;

/* Take this frame's snapshot. Render thread, once per frame, from
   `tagpu_overlay_draw` AFTER `tagpu_pal_frame` (the palette is what the indices
   are resolved through, and a surface with no palette is not drawable) and
   before any pass gathers. Cheap and silent on a frame with no 8-bit primary. */
void tagpu_surf_take(const TAGPU_FRAME* f);

/* This frame's surface, or 0 when there is none — a non-8bpp mode, no primary,
   a palette not readable yet, or a geometry outside the bound above. */
int tagpu_surf_frame(TAGPU_SURFFRAME* out);

#endif
