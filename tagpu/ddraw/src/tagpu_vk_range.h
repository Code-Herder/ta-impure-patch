#ifndef TAGPU_VK_RANGE_H
#define TAGPU_VK_RANGE_H
/* The weapon range rings, drawn by Vulkan. Contract only; tagpu_vk_range.c is
   the pass and tagpu_range.h says what it draws.

   IT IS RECORDED BETWEEN THE WORLD AND THE MARKERS, in the world's render
   pass: over the units and the effects, under the selection rects, the order
   markers and the health bars. It tests no depth, and it uses the target's
   STENCIL plane, which it clears to 0 itself before every group and once more
   when it is done, so nothing it leaves there reaches a later pass. A device
   with no stencil-carrying depth format (`stencilok` 0) draws no rings. */

#include "tagpu_vk_pass.h"

/* Take this frame's hand-over and put it on the device. Called from the seam
   after the terrain pass's `prepare` -- the rings sample the height grid that
   pass bound for this slot -- and before the world is recorded. Returns 1 when
   `record` has something to draw. */
int  tagpu_vk_range_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* Draw, inside the caller's render pass; `w`/`h` are the target's extent. */
void tagpu_vk_range_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                           VkRenderPass rp, uint32_t w, uint32_t h);

void tagpu_vk_range_down(const TAGPU_VKPASS* d);
int  tagpu_vk_range_down_owed(void);
void tagpu_vk_range_down_paid(const TAGPU_VKPASS* d);

#endif
