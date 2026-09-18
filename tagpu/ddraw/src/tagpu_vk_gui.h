#ifndef TAGPU_VK_GUI_H
#define TAGPU_VK_GUI_H
/* tagpu_vk_gui.c -- the GL UI layer's 1x mirror, drawn by Vulkan.
   Phase G / G19f, landing 1. gpu-status.md 2.3e is the pass being ported and
   research/notes/g19f-plan.md is why it is cut this way.

   IT IS NOT A SECOND IMPLEMENTATION OF THE UI. Everything it draws arrives
   through `tagpu_gui_handover` (tagpu_gui.h): the op stream the GL lane's own
   drain APPLIED, the atlas rect it resolved for each sprite, the atlas texels,
   the palette, and the engine's own frame as bytes. The shaders are G19c's
   translation of the same GLSL. The twin store evolves identically on both
   sides because the op stream is identical -- which is the only claim a 0-px
   comparison can make.

   TWO HOOKS, AND THE REPLAY IS IN `prepare`. A twin is drawn into with its own
   render pass and render passes may not nest, so the whole op replay happens in
   `prepare` -- the one hook that runs outside the seam's render pass, exactly
   as tagpu_vk_shadow.c records its map there. `record` draws one quad: the
   presented twin composited over the engine's frame, inside the seam's pass.

   THE FLIP IS A PROPERTY OF PRESENTATION (gpu-status 2.32), and this pass is
   the clearest case of it. The twin draws take NO viewport flip: their target
   is sampled, not presented, and `QVS`'s `y / uSize.y * 2 - 1` puts quad y = 0
   at attachment row 0 under both APIs. `CPY_FS` reads `gl_FragCoord`, and with
   no flip GL measures it from NDC -1 and Vulkan from the upper left -- which
   for an unflipped viewport is the same row index, so it ports unchanged and
   2.33's stand-down does not arise here. The COMPOSITE does flip, because it
   is presented. */
#include "tagpu_vk_pass.h"

/* The op replay and this slot's descriptor set. Outside the seam's render
   pass. 1 when `record` has something to draw. */
int  tagpu_vk_gui_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* The composite, inside the seam's render pass. */
void tagpu_vk_gui_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h);

/* 1 on the ONE frame `tagpu_gui.ab` latched its claim and the `_vk.ppm` target
   was unlinked, so the seam captures THAT frame. It does not mean a file was
   written -- since landing 4d-2 there is no GL half. Consumed. */
int  tagpu_vk_gui_ab_frame(void);

/* The owed-teardown protocol every pass on this lane uses: a refusal mid-frame
   may destroy nothing, because other slots' submits still name it. */
void tagpu_vk_gui_down(const TAGPU_VKPASS* d);
int  tagpu_vk_gui_down_owed(void);
void tagpu_vk_gui_down_paid(const TAGPU_VKPASS* d);
#endif
