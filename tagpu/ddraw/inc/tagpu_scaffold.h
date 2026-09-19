#ifndef TAGPU_SCAFFOLD_H
#define TAGPU_SCAFFOLD_H
#include "tagpu.h"
/* G12a: per-frame scene-depth scaffold (painter's row key as a depth buffer)
   + colour debug overlay + per-unit occlusion prediction log. Armed by the
   tagpu_scaffold.on trigger file; safe no-op otherwise. Leaves program/VAO
   bindings at 0. */
void tagpu_scaffold_frame(const TAGPU_FRAME* f);
/* `tagpu_scaffold_texref` stood here and returned this pass's GL texture id.
   It had NO caller anywhere in the tree even before the GL half went, and the
   texture it named is gone with it [the vulkan-only plan, landing 11-4c]. */
int tagpu_scaffold_frameinfo(unsigned frame_counter, int* r0, int* nrows);

/* ---- the Vulkan edition of this pass (Phase G / G19e) ----
   Everything the overlay this frame was drawn FROM, so that the Vulkan lane
   draws the same thing rather than a second implementation of it: the scaffold
   bytes themselves (`w` x `h`, one byte a pixel, the buffer this file just
   uploaded to GL), the quad's NDC rect and the row count the fragment shader
   ramps its colour over.

   `buf` POINTS INTO THIS FILE'S OWN ALLOCATION and is valid until the next
   frame rebuilds it. That is safe for exactly one reason and it is worth
   naming: both lanes run on the RENDER THREAD, and the whole of
   tagpu_overlay_draw -- this pass included -- happens earlier in the same
   iteration of render_ogl.c's loop than the tagpu_vk_frame that consumes this.
   The game thread never touches it.

   IT HANDS THEM OVER ONCE, so one frame's scaffold can never be drawn twice.
   A frame the overlay was SKIPPED on (not in game, no sane viewport) hands
   over nothing and the Vulkan lane draws nothing, which is what the GL lane
   did too. Returns 0 when there is nothing to draw, and when it has been taken.

   `ab` comes back 1 on the ONE frame this pass captured `tagpu_scaffold_gl.ppm`
   under `tagpu_scaffold.ab`, so the Vulkan lane captures the SAME frame rather
   than whichever one its own lever poll landed on. The two poll on different
   cadences (this pass every 30 frames, the lane every 250 ms) and the scaffold
   changes with the camera, so a flag each lane polled for itself would compare
   two different frames and report every moved pixel as a port failure.

   UNDER `renderer=vulkan` THERE IS NO `_gl.ppm` AND `ab` MEANS THE INTENT
   INSTEAD -- 1 on the one frame the lever was seen with the latch clear. This
   pass gathers and publishes there but draws nothing (tagpu_vk_owns_present),
   so a GL capture is not merely refused, it is not attempted, and a flag that
   waited for one would refuse every capture on the only lane that presents.
   The comparison that flag serves is a different one: not the two lanes of one
   frame, but this build's `_vk.ppm` against the previous build's
   (vulkan-only-plan.md, landing 4b). What the coupling was buying -- that a
   file on the disk belongs to this arming -- `tagpu_vk_ab_arm` now establishes,
   called by this pass in the same statement sequence that latches the claim and
   refusing the claim when the target could not be removed. */
int tagpu_scaffold_overlay(const unsigned char** buf, int* w, int* h,
                           float rect[4], float* rows, int* ab);

/* The largest scaffold tagpu_scaffold_frame will ever hand over. Its own gate
   is `vw < 64 || vh < 64 || vw > 4096 || vh > 4096`, so this is that bound
   squared -- and the Vulkan pass re-checks `w` and `h` against it rather than
   trusting the #define, because a bound in one file guarding an allocation in
   another is a bound only while both are read together. */
#define TAGPU_SCAF_MAXDIM 4096

/* THE QUAD ITSELF IS DEFINED ONCE, HERE, and both lanes initialise their vertex
   buffer from it. It is the pass's fixed geometry rather than a per-frame input
   -- what varies is the scaffold bytes, the rect and the row count above, and
   those are handed over -- but a four-vertex literal copied into a second file
   is still two things that can drift, and the whole worth of a 0-px comparison
   is that only the rasteriser differs. `p` is a unit quad and the strip order
   is p.y=0 first, so uv row 0 is the buffer's top row. */
#define TAGPU_SCAF_QUAD { 0.f,0.f, 1.f,0.f, 0.f,1.f, 1.f,1.f }
#define TAGPU_SCAF_QUADV 4
#endif
