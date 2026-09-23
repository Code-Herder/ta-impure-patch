#ifndef TAGPU_VK_SCAFFOLD_H
#define TAGPU_VK_SCAFFOLD_H
/* The scene-depth scaffold overlay, drawn by Vulkan (Phase G, the first
   world pass). Implementation: tagpu_vk_scaffold.c. Its producer is
   tagpu_scaffold.c, the gather that publishes the bytes, the rect and the row
   count, and the file whose GLSL tools/spirv-gen.py compiles into this pass's
   shader.

   THE TWO-PHASE CONTRACT is tagpu_vk_fps.h's and for the same reason: a texture
   upload is a transfer and a transfer may not be recorded inside a render pass.

     prepare()   before vkCmdBeginRenderPass -- build what is missing, upload
                 this frame's scaffold into this slot, fill this slot's uniforms
     record()    inside the render pass -- bind and draw

   Both on the render thread, on the command buffer the seam is recording, in
   that order, in one frame. `prepare` returning 0 means there is nothing to
   draw and `record` must not be called. */

#include "tagpu_vk_pass.h"

/* Build or refresh what this frame needs and say whether there is anything to
   draw. `slot` is the frame slot (< d->slots) whose resources the GPU has
   finished with; the seam's fence wait is what proves that, and here it proves
   it for an IMAGE as well as for buffers -- see the implementation's header. */
int  tagpu_vk_scaffold_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* Draw it, inside the render pass. `w`/`h` are the attachment's extent. */
void tagpu_vk_scaffold_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                              uint32_t w, uint32_t h);

/* 1 on the ONE frame `tagpu_scaffold.ab` latched its claim and `tagpu_vk_ab_arm`
   got the `_vk.ppm` target unlinked -- so the seam captures THAT frame rather
   than whichever one its own lever poll landed on. It does NOT mean a file was
   written: the capture the seam then records is this lane's own. Consumed by
   the call. Valid after `prepare`. */
int  tagpu_vk_scaffold_ab_frame(void);

/* ---- THE OVERLAY AS A TEXTURE OTHER PASSES SAMPLE (the unit pass) ----

   The overlay is not only drawn: the unit fragment shader samples it through
   `uScaf` whenever `uScafOn` is 1 (the hi-res and effects shaders declare
   `uScaf` too, and neither samples it). So this pass exposes what it uploaded,
   in exactly the shape tagpu_vk_shadow.h settled on for the depth map, and for
   the same reasons.

   ASK WITH YOUR OWN FRAME. The image behind the view is one per slot and the
   "is it this frame's" flag is one for the pass, so a caller out of step with
   `prepare` would be handed an earlier frame's overlay. Wrong frame or wrong
   slot gives VK_NULL_HANDLE and the caller names its own dummy -- which it has
   to have anyway, for a frame with no overlay at all.

   AND IT ORDERS THE SEAM. A consumer points its descriptor set at this view
   during its own `prepare`, so this pass's `prepare` has to have run first --
   even though its `record` comes late, because the overlay is drawn OVER the
   world. Prepare order and record order are not the same order, and the seam
   says so at both call sites. */
int  tagpu_vk_scaffold_ready(unsigned frame);
VkImageView tagpu_vk_scaffold_view(unsigned frame, uint32_t slot);

/* Give everything back. Called by the seam from `vk_down`, after its
   vkDeviceWaitIdle and before the device is destroyed. Safe when nothing was
   ever built. */
void tagpu_vk_scaffold_down(const TAGPU_VKPASS* d);

/* 1 while the pass has stopped drawing mid-frame because the device would not
   give it this frame's resources, and is waiting for the seam to tear it down.
   THE SEAM MUST vkDeviceWaitIdle BEFORE CALLING `_down` FOR THIS: the pass
   cannot destroy anything itself at the moment it finds out, because only its
   own slot's fence has been waited on and the command buffer of the frame in
   hand already names its objects. Checked at the top of a frame, before
   anything is recorded.

   PAIR IT WITH `_down_paid`, AND DO NOT LET AN ORDINARY TEARDOWN SETTLE IT.
   `vk_down` and `vk_resize` call `_down` for their own reasons and would
   otherwise consume an outstanding debt -- leaving the pass latched at
   ST_REFUSED, which `prepare` treats as terminal, so a transient refusal
   followed by a window drag would kill the pass for the life of the PROCESS.
   The seam calls `_down_paid` after its drain; every other caller of `_down`
   leaves the debt standing and the pass comes back ST_UNBUILT. */
int  tagpu_vk_scaffold_down_owed(void);
/* The seam, after its vkDeviceWaitIdle: tear the pass down AND settle the
   debt, so `_down`'s ST_REFUSED latch applies to this teardown only. */
void tagpu_vk_scaffold_down_paid(const TAGPU_VKPASS* d);



#endif
