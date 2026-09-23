#ifndef TAGPU_SCAFFOLD_H
#define TAGPU_SCAFFOLD_H
#include "tagpu.h"
/* The per-frame scene-depth scaffold (painter's row key as a depth buffer)
   + colour debug overlay + per-unit occlusion prediction log. Armed by the
   tagpu_scaffold.on trigger file; safe no-op otherwise. */
void tagpu_scaffold_frame(const TAGPU_FRAME* f);
int tagpu_scaffold_frameinfo(unsigned frame_counter, int* r0, int* nrows);

/* ---- the hand-over to the Vulkan pass ----
   Everything the overlay this frame was drawn FROM, so that
   tagpu_vk_scaffold.c draws what the gather built rather than a second
   implementation of it: the scaffold bytes themselves (`w` x `h`, one byte a
   pixel), the quad's NDC rect and the row count the fragment shader ramps its
   colour over.

   `buf` POINTS INTO THIS FILE'S OWN ALLOCATION and is valid until the next
   frame rebuilds it. That is safe for exactly one reason and it is worth
   naming: the gather and the Vulkan pass both run on the RENDER THREAD, and
   the whole of tagpu_overlay_draw -- this pass included -- happens earlier in
   the same iteration of render_vk.c's loop than the tagpu_vk_frame that
   consumes this. The game thread never touches it.

   IT HANDS THEM OVER ONCE, so one frame's scaffold can never be drawn twice.
   A frame the overlay was SKIPPED on (not in game, no sane viewport) hands
   over nothing and the Vulkan pass draws nothing. Returns 0 when there is
   nothing to draw, and when it has been taken.

   `ab` is the A/B claim: nonzero on the ONE frame the `tagpu_scaffold.ab`
   lever was seen with the latch clear, and it travels with that frame's
   scaffold, so the Vulkan pass captures the frame the claim latched on rather
   than whichever one a poll of its own landed on. The comparison it serves is
   this build's `_vk.ppm` against another build's. That the file on the disk
   belongs to this arming is `tagpu_vk_ab_arm`'s to establish, called by this
   pass in the same statement sequence that latches the claim and refusing the
   claim when the target could not be removed. */
int tagpu_scaffold_overlay(const unsigned char** buf, int* w, int* h,
                           float rect[4], float* rows, int* ab);

/* The largest scaffold tagpu_scaffold_frame will ever hand over. Its own gate
   is `vw < 64 || vh < 64 || vw > 4096 || vh > 4096`, so this is that bound
   squared -- and the Vulkan pass re-checks `w` and `h` against it rather than
   trusting the #define, because a bound in one file guarding an allocation in
   another is a bound only while both are read together. */
#define TAGPU_SCAF_MAXDIM 4096

/* THE QUAD ITSELF, which tagpu_vk_scaffold.c initialises its vertex buffer
   from. It is the pass's fixed geometry rather than a per-frame input -- what
   varies is the scaffold bytes, the rect and the row count above, and those
   are handed over. `p` is a unit quad and the strip order is p.y=0 first, so
   uv row 0 is the buffer's top row. */
#define TAGPU_SCAF_QUAD { 0.f,0.f, 1.f,0.f, 0.f,1.f, 1.f,1.f }
#define TAGPU_SCAF_QUADV 4
#endif
