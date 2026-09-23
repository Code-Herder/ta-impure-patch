#ifndef TAGPU_VK_FX_H
#define TAGPU_VK_FX_H
/* The effects pass -- weapon fire, explosions, debris and the ten particle
   layers -- drawn by Vulkan (the FOURTH world pass).
   Implementation: tagpu_vk_fx.c. The gather is tagpu_fx.c and is the
   source of the vertices, the uniforms, the texels and the shader.

   THE TWO-PHASE CONTRACT is tagpu_vk_feat.h's, for the same reason: a texture
   upload is a transfer and a transfer may not be recorded inside a render pass.

     prepare()   before vkCmdBeginRenderPass -- build what is missing, upload
                 this frame's geometry and texels, fill this slot's uniforms
     record()    inside the render pass -- bind and draw, the four buckets in
                 TAGPU_FXB_* order (tagpu_fx.h)

   Both on the render thread, on the command buffer the seam is recording, in
   that order, in one frame. `prepare` returning 0 means there is nothing to
   draw and `record` must not be called.

   IT IS THE FIRST PASS THAT NEEDS MORE THAN ONE PIPELINE FOR ONE SHADER. The
   four buckets share one shader but need three different pieces of
   fixed-function state -- a LINE_LIST for the lasers and the lightning, and
   additive blending for the flashes -- and all three of those are pipeline
   state in Vulkan. See the implementation's header for what that costs and for
   the one parity question it opens, which is line rasterisation. */

#include "tagpu_vk_pass.h"

/* Build or refresh what this frame needs and say whether there is anything to
   draw. `slot` is the frame slot (< d->slots) whose resources the GPU has
   finished with; the seam's fence wait is what proves that. */
int  tagpu_vk_fx_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* Draw it, inside the render pass. `w`/`h` are the attachment's extent. */
void tagpu_vk_fx_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                        uint32_t w, uint32_t h);

/* 1 on the ONE frame `tagpu_fx.ab` latched its claim and `tagpu_vk_ab_arm`
   got the `_vk.ppm` target unlinked -- so the seam captures THAT frame rather
   than whichever one its own lever poll landed on. It does NOT mean a file was
   written: the seam records the capture after the draw. Consumed by the call.
   Valid after `prepare`. */
int  tagpu_vk_fx_ab_frame(void);

/* Give everything back. Called by the seam from `vk_down`, after its
   vkDeviceWaitIdle and before the device is destroyed. Safe when nothing was
   ever built. */
void tagpu_vk_fx_down(const TAGPU_VKPASS* d);

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
int  tagpu_vk_fx_down_owed(void);
/* The seam, after its vkDeviceWaitIdle: tear the pass down AND settle the
   debt, so `_down`'s ST_REFUSED latch applies to this teardown only. */
void tagpu_vk_fx_down_paid(const TAGPU_VKPASS* d);

#endif
