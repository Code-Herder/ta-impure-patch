#ifndef TAGPU_VK_FPS_H
#define TAGPU_VK_FPS_H
/* The frame-rate readout, drawn by Vulkan. Implementation: tagpu_vk_fps.c.
   tagpu_fps.c builds the geometry and places the strings in the atlas -- see
   that file's header.

   THE TWO-PHASE CONTRACT, and it is the shape every ported pass will want. A
   texture upload is a transfer and a transfer may not be recorded inside a
   render pass, so a pass gets two calls on one command buffer:

     prepare()   before vkCmdBeginRenderPass -- build what is missing, upload
                 what has changed, fill this slot's buffers
     record()    inside the render pass -- bind and draw

   Both on the render thread, on the command buffer the seam is recording, in
   that order, in one frame. `prepare` returning 0 means there is nothing to
   draw and `record` must not be called. */

#include "tagpu_vk_pass.h"

/* Build or refresh what this frame needs and say whether there is anything to
   draw. `slot` is the frame slot (< d->slots) whose buffers the GPU has
   finished with; the seam's fence wait is what proves that. */
int  tagpu_vk_fps_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* Draw it, inside the render pass. `w`/`h` are the attachment's extent. */
void tagpu_vk_fps_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h);

/* 1 on the ONE frame `tagpu_fps.ab` latched its claim, so that the seam
   captures THAT frame instead of whichever one its own lever poll landed on --
   the readout changes its number twice a second, so which frame is captured
   matters here more than anywhere. It does not mean a file was written: the
   seam writes the capture after it records. Consumed. Valid after `prepare`. */
int  tagpu_vk_fps_ab_frame(void);

/* Give everything back. Called by the seam from `vk_down`, after its
   vkDeviceWaitIdle and before the device is destroyed. Safe when nothing was
   ever built. */
void tagpu_vk_fps_down(const TAGPU_VKPASS* d);

#endif
