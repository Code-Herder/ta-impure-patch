#ifndef TAGPU_VK_SURF_H
#define TAGPU_VK_SURF_H

/* THE FRAME'S BOTTOM LAYER: TA's own 8-bit screen, resolved through the
   presented palette, drawn before anything of ours. Implementation:
   tagpu_vk_surf.c. The bytes, the palette and the destination rect are
   tagpu_surf.c's; the shader is the FORK's own -- PASSTHROUGH_VERT_SHADER and
   PALETTE_FRAG_SHADER out of inc/openglshader.h, the pair render_ogl.c:260
   builds as `g_ogl.main_program` and draws for the GL lane before any pass
   runs. This is that draw, on the other backend.

   WHY IT EXISTS. On `renderer=vulkan` the seam cleared the swapchain image to
   the lever's colour and TA's surface reached the frame ONLY through the GUI
   pass's mirror hand-over, so `tagpu_gui.off` left a flat colour where the GL
   lane still shows a game. That is one of the three blind spots the
   vulkan-only plan's landing 1 named.

   THE CLEAR STAYS. `vkCmdClearColorImage` still runs before the render pass and
   is still what covers the letterbox and a frame with no surface to draw. This
   pass paints the viewport rect on top of it, inside the render pass, which is
   the only place a pipeline can run.

   THE TWO-PHASE CONTRACT is tagpu_vk_fps.h's, for the reason every pass here
   shares: a texture upload is a transfer and a transfer may not be recorded
   inside a render pass.
     prepare()   before vkCmdBeginRenderPass -- build what is missing, upload
                 this frame's surface and palette into this slot
     record()    inside the render pass, FIRST of all the passes -- bind and draw
   Both on the render thread, on the command buffer the seam is recording, in
   that order, in one frame. `prepare` returning 0 means there is nothing to
   draw and `record` must not be called. */

#include "tagpu_vk_pass.h"

/* `slot` is the frame slot (< d->slots) whose resources the GPU has finished
   with; the seam's fence wait is what proves that. */
int  tagpu_vk_surf_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);
void tagpu_vk_surf_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h);

/* THE ENGINE'S FRAME, FOR A SECOND READER (the vulkan-only plan's landing 10).
   This pass already holds TA's 8-bit surface as an R8 image, uploaded once per
   frame and left in SHADER_READ_ONLY_OPTIMAL -- and the UI layer's shader wants
   exactly that image for its stale-mirror guard (`uSurf`). It used to upload a
   SECOND copy of the same bytes from the same source; this hands over the one
   that already exists.

   Returns VK_NULL_HANDLE when this slot holds no current frame. EVERY path in
   `tagpu_vk_surf_prepare` that does not leave this slot holding the current
   frame clears `haveSerial`, which is what this answers on -- so the answer is
   a property of the slot's CONTENTS and not of when it is asked. [The review
   of landing 10 found the previous wording claiming a frame scope nothing
   enforced, with one early return -- the re-checked dimension bound -- leaving
   a stale slot addressable. That return now invalidates.]

   The caller must treat VK_NULL_HANDLE as "the surface pass has no image to
   lend". That is NOT the same as "there is no engine frame": the pass latches
   ST_REFUSED permanently on an allocation failure, so a caller that stands
   down on this alone stands down for the process. `tagpu_vk_gui.c` composites
   without its stale-mirror guard instead.

   The seam calls surf's prepare BEFORE the UI layer's (tagpu_vk.c:2771 before
   :2822, same command buffer, same slot), so the image is uploaded and
   barriered into SHADER_READ_ONLY_OPTIMAL ahead of the descriptor that names
   it. `w`/`h` may be NULL. */
VkImageView tagpu_vk_surf_engine_view(uint32_t slot, int* w, int* h);

/* The teardown handshake every pass has: the seam asks, waits for the device to
   go idle, then pays. */

void tagpu_vk_surf_down(const TAGPU_VKPASS* d);
int  tagpu_vk_surf_down_owed(void);
void tagpu_vk_surf_down_paid(const TAGPU_VKPASS* d);

#endif
