#ifndef TAGPU_VK_FEAT_H
#define TAGPU_VK_FEAT_H
/* The feature pass -- trees, rocks, metal patches, splats and GAF wreckage --
   drawn by Vulkan (Phase G / G19e, the SECOND world pass). Implementation:
   tagpu_vk_feat.c. The GL edition is tagpu_feat.c and stays the source of the
   vertices, the uniforms, the texels and the shader.

   THE TWO-PHASE CONTRACT is tagpu_vk_scaffold.h's, for the same reason: a
   texture upload is a transfer and a transfer may not be recorded inside a
   render pass.

     prepare()   before vkCmdBeginRenderPass -- build what is missing, upload
                 this frame's geometry and texels, fill this slot's uniforms
     record()    inside the render pass -- bind and draw, shadows then bodies

   Both on the render thread, on the command buffer the seam is recording, in
   that order, in one frame. `prepare` returning 0 means there is nothing to
   draw and `record` must not be called.

   IT IS THE FIRST PASS THAT DEPTH-TESTS, so it is the first that needs the
   seam's render pass to carry a depth attachment: it refuses to arm when
   `d->dfmt` is VK_FORMAT_UNDEFINED rather than drawing untested. */

#include "tagpu_vk_pass.h"

/* Build or refresh what this frame needs and say whether there is anything to
   draw. `slot` is the frame slot (< d->slots) whose resources the GPU has
   finished with; the seam's fence wait is what proves that. */
int  tagpu_vk_feat_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* Draw it, inside the render pass. `w`/`h` are the attachment's extent. */
void tagpu_vk_feat_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h);

/* 1 on the one frame the GL twin captured `tagpu_feat_gl.ppm`, so that the
   seam captures the SAME frame. Consumed by the call. Valid after `prepare`. */
int  tagpu_vk_feat_ab_frame(void);

/* Give everything back. Called by the seam from `vk_down`, after its
   vkDeviceWaitIdle and before the device is destroyed. Safe when nothing was
   ever built. */
void tagpu_vk_feat_down(const TAGPU_VKPASS* d);

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
   followed by a window drag killed the pass for the life of the PROCESS. The
   seam calls `_down_paid` after its drain; every other caller of `_down`
   leaves the debt standing and the pass comes back ST_UNBUILT, which is what
   it did before the owed-teardown protocol existed.
   [FROM THE G19e RE-REVIEW, 2026-09-15.] */
int  tagpu_vk_feat_down_owed(void);
/* The seam, after its vkDeviceWaitIdle: tear the pass down AND settle the
   debt, so `_down`'s ST_REFUSED latch applies to this teardown only. */
void tagpu_vk_feat_down_paid(const TAGPU_VKPASS* d);



#endif
