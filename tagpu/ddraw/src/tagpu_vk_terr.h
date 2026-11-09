#ifndef TAGPU_VK_TERR_H
#define TAGPU_VK_TERR_H
/* The terrain pass -- the 32x32 pre-rendered map tiles -- drawn by Vulkan
   Implementation: tagpu_vk_terr.c. Its producer is tagpu_terr.c, the gather
   that publishes the instances, the uniforms and the texels, and the file
   whose GLSL tools/spirv-gen.py compiles into this pass's shader.

   THE TWO-PHASE CONTRACT is tagpu_vk_scaffold.h's, for the same reason: a
   texture upload is a transfer and a transfer may not be recorded inside a
   render pass.

     prepare()   before vkCmdBeginRenderPass -- build what is missing, upload
                 this frame's instances and any texels that moved, fill this
                 slot's uniform blocks
     record()    inside the render pass -- bind and draw, one instanced call

   Both on the render thread, on the command buffer the seam is recording, in
   that order, in one frame. `prepare` returning 0 means there is nothing to
   draw and `record` must not be called.

   IT DEPTH-TESTS AND WRITES, so it refuses to arm when `d->dfmt` is
   VK_FORMAT_UNDEFINED rather than drawing untested -- terrain is the frame's
   implicit far plane and everything above it is depth-tested against what it
   wrote. */

#include "tagpu_vk_pass.h"

/* Build or refresh what this frame needs and say whether there is anything to
   draw. `slot` is the frame slot (< d->slots) whose resources the GPU has
   finished with; the seam's fence wait is what proves that. */
int  tagpu_vk_terr_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* Draw it, inside the render pass. `w`/`h` are the attachment's extent. */
void tagpu_vk_terr_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h);

/* 1 on the ONE frame `tagpu_terr.ab` latched its claim and `tagpu_vk_ab_arm`
   got the `_vk.ppm` target unlinked -- so the seam captures THAT frame rather
   than whichever one its own lever poll landed on. It does NOT mean a file was
   written: the capture the seam then records is this lane's own. Consumed by
   the call. Valid after
   `prepare`. */
int  tagpu_vk_terr_ab_frame(void);

/* Give everything back. Called by the seam from `vk_down`, after its
   vkDeviceWaitIdle and before the device is destroyed. Safe when nothing was
   ever built. */
/* THE HEIGHT GRID THIS SLOT'S SET NAMES, for a pass that samples it in the
   same submission (tagpu_vk_range.c): the R8 image of the map's height byte
   per 16-px cell, and its size in cells. VK_NULL_HANDLE unless this slot's set
   names this pass's own height image and a copy has been recorded into it.
   The view lives as long as this slot's set names it -- the retire of item 3
   destroys a replaced image only once no slot's set does -- so a pass that
   asks after this slot's `prepare`, and binds what it is told for this slot
   alone, is covered by the same proof. */
VkImageView tagpu_vk_terr_height(uint32_t slot, int* w, int* h);

void tagpu_vk_terr_down(const TAGPU_VKPASS* d);

/* 1 while the pass has stopped drawing mid-frame because the device would not
   give it this frame's resources. The game's picture has a hole without this pass, so
   the seam brings the whole lane down at the top of the next frame and the retry
   rebuilds it (render_vk.c's budget, then GDI); the wiki's copy only loses its stage
   (`_down_refused`). The pass destroys nothing itself:
   only its own slot's fence has been waited on, and the command buffer of the
   frame in hand already names its objects. */
int  tagpu_vk_terr_down_owed(void);
#ifdef TAGPU_SCENE_WIKI
void tagpu_vk_terr_down_refused(const TAGPU_VKPASS* d);
#endif



#endif
