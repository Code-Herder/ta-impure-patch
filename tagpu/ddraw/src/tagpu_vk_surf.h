#ifndef TAGPU_VK_SURF_H
#define TAGPU_VK_SURF_H

/* THE REFERENCE TEXTURE: TA's own 8-bit screen, on the device, drawn nowhere.
   Implementation: tagpu_vk_surf.c. The bytes and the palette are
   tagpu_surf.c's, captured from the engine's composed frame.

   IT USED TO BE THE FRAME'S BOTTOM LAYER. This pass resolved TA's indices
   through the presented palette and blitted them under everything of ours,
   through the fork's own GLSL. THE CLEAN CUT DELETED THAT DRAW and the
   pipeline, descriptors, sampler, buffers and quad behind it, so no pixel the
   1997 software rasteriser produced reaches the screen. There is no lever back:
   the code is gone, not gated.

   WHAT IT IS NOW. One R8 index image per frame slot with its 256x1 palette
   beside it, uploaded from the CPU capture and left in
   SHADER_READ_ONLY_OPTIMAL. `tagpu_vk_surf_engine_view` hands that view and its
   size to anything that wants to diff our output against the engine's. That is
   the golden source, and holding it is this module's whole purpose.

   ONE PHASE, NOT TWO. Every other pass here has prepare/record because a
   texture upload is a transfer and a transfer may not be recorded inside a
   render pass. This one has only the transfer, so `prepare` is all there is and
   it is called OUTSIDE vkCmdBeginRenderPass like the rest. Its return value is
   "this slot now holds the current frame", not "call record next".

   THE CLEAR IS THE BOTTOM OF THE FRAME NOW. `vkCmdClearColorImage` runs before
   the render pass and covers the whole image, letterbox included; the world is
   the first thing drawn on it. */

#include "tagpu_vk_pass.h"

/* `slot` is the frame slot (< d->slots) whose resources the GPU has finished
   with; the seam's fence wait is what proves that. */
int  tagpu_vk_surf_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* THE GOLDEN SOURCE, LENT OUT. The engine's frame as an R8 index image --
   palette indices, not colour: index 254 is the terrain key, and a consumer
   resolves through its own palette or compares indices directly.

   Returns VK_NULL_HANDLE when this slot holds no current frame. EVERY path in
   `tagpu_vk_surf_prepare` that does not leave this slot holding the current
   frame clears `haveSerial`, which is what this answers on -- so the answer is
   a property of the slot's CONTENTS and not of when it is asked. [The review
   of landing 10 found the previous wording claiming a frame scope nothing
   enforced, with one early return -- the re-checked dimension bound -- leaving
   a stale slot addressable. That return now invalidates.]

   The caller must treat VK_NULL_HANDLE as "the surface pass has no image to
   lend". That is NOT the same as "there is no engine frame": the pass latches
   ST_REFUSED permanently on an allocation failure, so a caller that stands down
   on this alone stands down for the process.

   NO SAMPLER COMES WITH IT. The view is all this module owns since the cut took
   its draw; a consumer brings its own, and should bring a NEAREST one -- half
   way between index 7 and index 8 is index 7.5, which is in no palette entry
   and belongs to neither neighbour.

   THERE IS NO CONSUMER IN THE TREE TODAY. The one it had was the UI layer's
   composite, deleted by the cut. `w`/`h` may be NULL. */
VkImageView tagpu_vk_surf_engine_view(uint32_t slot, int* w, int* h);

/* The teardown handshake every pass has: the seam asks, waits for the device to
   go idle, then pays. */

void tagpu_vk_surf_down(const TAGPU_VKPASS* d);
int  tagpu_vk_surf_down_owed(void);
void tagpu_vk_surf_down_paid(const TAGPU_VKPASS* d);

#endif
