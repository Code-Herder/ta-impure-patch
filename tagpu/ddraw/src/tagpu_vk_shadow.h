#ifndef TAGPU_VK_SHADOW_H
#define TAGPU_VK_SHADOW_H
/* The Classic++ cast-shadow depth map, drawn by Vulkan (Phase G / G19e, the
   FIFTH world pass). Implementation: tagpu_vk_shadow.c. The GL edition is
   tagpu_shadow.c and stays the source of the matrix, the caster geometry and
   the shader.

   IT IS THE FIRST PASS THAT DRAWS INTO SOMETHING OTHER THAN THE FRAME. Every
   pass before it records into the seam's render pass and produces pixels; this
   one owns an offscreen depth image, draws the casters into it from the
   light's point of view, and leaves it in SHADER_READ_ONLY_OPTIMAL for the
   passes that sample it. So it has a `prepare` and no `record`:

     prepare()   before vkCmdBeginRenderPass -- build what is missing, upload
                 the caster mesh when it moved, then begin ITS OWN render pass,
                 draw, and end it. A render pass may not be nested inside
                 another, and `prepare` is the hook that runs outside the
                 seam's, which is the whole reason the contract has two halves.

   NOTHING HERE NAMES A WINDOW, A SURFACE OR A SWAPCHAIN -- standing constraint
   3. Owning a second render target is not presentation: the seam still owns the
   one image that reaches a screen, and this pass would work unchanged against
   an offscreen frame or another process's.

   IT MUST RUN BEFORE ITS CONSUMERS' `prepare`, because they point their
   descriptor sets at `tagpu_vk_shadow_view(slot)` there -- see the seam's
   ordering comment. */

#include "tagpu_vk_pass.h"

/* Draw this frame's map. Returns 1 when a complete map now stands in
   `tagpu_vk_shadow_view(slot)`, 0 when nothing was drawn -- no map this frame,
   a device refusal, or a map holding casters the hand-over carries no copy of
   (tagpu_shadow.h's `otherCasters`, which is every unit until the unit pass
   lands). `slot` is the frame slot the seam's fence has proved free.

   IT DRAWS NO FRAME PIXEL, so the seam must NOT count it among the passes that
   drew into the frame: that count exists to catch two passes contaminating one
   A/B capture, and this one cannot. */
int  tagpu_vk_shadow_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* 1 when the map drawn for `frame` is complete and may be sampled. A consumer
   asks with its OWN frame number so that a map left over from an earlier frame
   can never be sampled as though it were this one's. Valid after `prepare`. */
int  tagpu_vk_shadow_ready(unsigned frame);

/* This slot's depth image, in SHADER_READ_ONLY_OPTIMAL, or VK_NULL_HANDLE.
   Valid for the frame `prepare` was called with and that slot only; a consumer
   writes it into its own descriptor set during its own `prepare`, which is the
   one instant the seam's fence proves that set is not in flight. */
VkImageView tagpu_vk_shadow_view(uint32_t slot);

/* THE FORMAT THE MAP IS IN, asked of the device and cached. A consumer needs
   it for two things and must not guess at either: whether its COMPARE sampler
   may be LINEAR (the GL twin's PCF is bilinear, and linear filtering of a depth
   format is a feature bit, not a given), and what format to give the dummy
   image its descriptor set names on a frame with no map -- which has to be one
   the same sampler is valid against. Exposed rather than re-derived so that the
   two files agree by construction instead of by both happening to try the same
   candidates in the same order. VK_FORMAT_UNDEFINED when the device has no
   24-bit depth format that is both a depth attachment and a sampled image, and
   `*linearOk` says whether it can be filtered. Safe to call before `prepare`
   and before anything is built; it resolves the one entry point it needs. */
VkFormat tagpu_vk_shadow_format(const TAGPU_VKPASS* d, int* linearOk);

/* The map's edge in texels this frame, or 0. The consumers' uShScale carries
   1/res and comes from the GL twin, so this is for logging and for the A/B
   dump rather than for arithmetic. */
int  tagpu_vk_shadow_res(void);

/* Give everything back. Called by the seam from `vk_down`, after its
   vkDeviceWaitIdle and before the device is destroyed. Safe when nothing was
   ever built. */
void tagpu_vk_shadow_down(const TAGPU_VKPASS* d);

/* 1 while the pass has stopped drawing mid-frame because the device would not
   give it this frame's resources, and is waiting for the seam to tear it down
   behind a vkDeviceWaitIdle. The contract, and why `_down_paid` is separate
   from `_down`, is tagpu_vk_fx.h's at length. */
int  tagpu_vk_shadow_down_owed(void);
void tagpu_vk_shadow_down_paid(const TAGPU_VKPASS* d);

#endif
