#ifndef TAGPU_VK_FEAT_H
#define TAGPU_VK_FEAT_H
/* The feature pass -- trees, rocks, metal patches, splats and GAF wreckage --
   drawn by Vulkan (the second world pass). Implementation:
   tagpu_vk_feat.c. Its producer is tagpu_feat.c, the gather that publishes
   the vertices, the uniforms and the texels, and the file whose GLSL
   tools/spirv-gen.py compiles into this pass's shader.

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

/* 1 on the ONE frame `tagpu_feat.ab` latched its claim and `tagpu_vk_ab_arm`
   got the `_vk.ppm` target unlinked -- so the seam captures THAT frame rather
   than whichever one its own lever poll landed on. It does NOT mean a file was
   written: the capture the seam then records is this lane's own. Consumed by
   the call. Valid after `prepare`. */
int  tagpu_vk_feat_ab_frame(void);

/* Give everything back. Called by the seam from `vk_down`, after its
   vkDeviceWaitIdle and before the device is destroyed. Safe when nothing was
   ever built. */
void tagpu_vk_feat_down(const TAGPU_VKPASS* d);

/* 1 while the pass has stopped drawing mid-frame because the device would not
   give it this frame's resources. The game's picture has a hole without this pass, so
   the seam brings the whole lane down at the top of the next frame and the retry
   rebuilds it (render_vk.c's budget, then GDI); the wiki's copy only loses its stage
   (`_down_refused`). The pass destroys nothing itself:
   only its own slot's fence has been waited on, and the command buffer of the
   frame in hand already names its objects. */
int  tagpu_vk_feat_down_owed(void);
#ifdef TAGPU_SCENE_WIKI
void tagpu_vk_feat_down_refused(const TAGPU_VKPASS* d);
#endif



#endif
