#ifndef TAGPU_VK_MARK_H
#define TAGPU_VK_MARK_H
/* The UI MARKERS, drawn by Vulkan: health
   bars, the build and band-box cursors, order markers and their labels, group
   digits, and the captured post-fog layer. Contract only; tagpu_vk_mark.c is
   the pass.

   IT IS THE FRAME'S TOP LAYER. tagpu_mark.h states the rule: every fragment
   is opaque and nothing behind it matters -- so this pass blends nothing and
   tests no depth, except for the one marker that header declares
   depth-tested, the selection rect (`s_pipeLineZ`).

   WHAT IT IS FED: `tagpu_mark_handover` (tagpu_mark.h), which is a DRAW LIST
   rather than a set of counts, because this pass is seven draws with different
   `uText`/`uFog` and the last two with fog forced off. */

#include "tagpu_vk_pass.h"

/* Take this frame's hand-over and put its vertices, draw list and textures on
   the device. Called from the seam before the world is recorded; draws nothing.
   Returns 1 when `record` has something to draw. */
int  tagpu_vk_mark_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* Draw, inside the caller's render pass. `w`/`h` are the target's extent: this
   pass SETS ITS OWN viewport and scissor, as every world pass does, because its
   pipelines declare both dynamic and dynamic state that is never set is
   undefined -- the vertices land nowhere. `rp` is the render pass, needed once
   to build the pipelines against it; a second call with a different one
   rebuilds. */
void tagpu_vk_mark_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          VkRenderPass rp, uint32_t w, uint32_t h);

/* The A/B: 1 when `tagpu_mark.ab` claimed this frame. */
int  tagpu_vk_mark_ab_frame(void);

void tagpu_vk_mark_down(const TAGPU_VKPASS* d);
int  tagpu_vk_mark_down_owed(void);
void tagpu_vk_mark_down_paid(const TAGPU_VKPASS* d);

#endif
