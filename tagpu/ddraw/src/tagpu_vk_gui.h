#ifndef TAGPU_VK_GUI_H
#define TAGPU_VK_GUI_H
/* tagpu_vk_gui.c -- the UI layer's 1x mirror, drawn by Vulkan.
   gpu-status.md 2.3e describes the pass and research/notes/g19f-plan.md
   is why it is cut this way.

   THE UI IS DECIDED IN THE DRAIN AND DRAWN HERE. Everything it draws arrives
   through `tagpu_gui_handover` (tagpu_gui.h): the op stream the drain
   (tagpu_gui_surf.c) APPLIED, the atlas rect it resolved for each sprite, the
   atlas texels, the palette, and the engine's own frame as bytes. The shaders
   are tagpu_gui_surf.c's GLSL through tools/spirv-gen.py. The drain's twin
   table and this pass's twin images evolve together because both follow the
   one op stream.

   TWO HOOKS, AND THE REPLAY IS IN `prepare`. A twin is drawn into with its own
   render pass and render passes may not nest, so the whole op replay happens in
   `prepare` -- the one hook that runs outside the seam's render pass, exactly
   as tagpu_vk_shadow.c records its map there. `record` draws one quad: the
   presented twin composited over the engine's frame, inside the seam's pass.

   THE FLIP IS A PROPERTY OF PRESENTATION (gpu-status 2.32), and this pass is
   the clearest case of it. The twin draws take NO viewport flip: their target
   is sampled, not presented, and `QVS`'s `y / uSize.y * 2 - 1` puts quad y = 0
   at attachment row 0. `CPY_FS` reads `gl_FragCoord`, which Vulkan measures
   from the upper left -- for an unflipped viewport that is attachment row 0,
   the row QVS puts quad y = 0 at, so the two agree and 2.33's stand-down does
   not arise here. The COMPOSITE does flip, because it is presented. */
#include "tagpu_vk_pass.h"

/* The op replay and this slot's descriptor set. Outside the seam's render
   pass. 1 when `record` has something to draw. */
int  tagpu_vk_gui_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* The composite, inside the seam's render pass. RETURNS 1 ONLY WHEN THE DRAW
   REACHED THE COMMAND BUFFER, and every early return answers 0 -- the two the
   caller's `prepare` already excludes, and the one it cannot: a composite
   pipeline that would not build. That one LATCHES (`s_layRp` is cleared, so the
   next frame re-enters and fails identically), so a caller inferring "it was
   called, therefore it drew" would be wrong for the rest of the session.
   `tagpu_vk.c` sets `s_uiDrew` from this return for that reason, and the cursor
   ownership the seam publishes rests on it. */
int  tagpu_vk_gui_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h);

/* The composite's sharp layer -- the cursor -- drawn again inside `r`, in the
   attachment's pixels: the replay HUD's top layer is drawn after the
   composite (tagpu_vk_hud.h), and this puts the cursor back over it. The
   composite's pipeline, set and viewport with its sharp-only quad, whose
   fragments outside the sharp layer are discarded (LAY_FS). Draws only when
   `record` drew this slot's composite this frame; 1 when it drew. Inside the
   seam's render pass, after the top layer. */
int  tagpu_vk_gui_record_sharp(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                               uint32_t w, uint32_t h, VkRect2D r);

/* This frame's cursor -- the quad the sharp layer drew it with, as resolved for this frame, never
   the pointer read live -- drawn again straight onto the attachment, over the in-game wiki's layer
   (tagpu_vk_hud_record_wiki), which covers the composite's copy of it. CURS_FS against the seam's
   render pass. Draws only when `record` drew this slot's composite this frame and its sharp layer
   held a cursor; 1 when it drew. Inside the seam's render pass, after the wiki's layer. */
int  tagpu_vk_gui_record_cursor(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                                uint32_t w, uint32_t h);

/* The whole composite drawn again inside `r`, as `record_sharp` draws its sharp layer: what the
   engine drew there goes back over the HUD's top layer -- the Config panel, inside its rect, over
   the live HUD's strip. The same conditions and the same place in the pass. */
int  tagpu_vk_gui_record_whole(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                               uint32_t w, uint32_t h, VkRect2D r);

/* The Config panel on the `w` x `h` attachment, as the composite `record` drew this frame for
   `slot` places it: 1 with its rect when that composite's own hand-over says the panel was open
   at its flip (TAGPU_GUIHAND::panelL..), 0 otherwise. The box and the composite are one
   snapshot, so the rect names the panel exactly when the composite holds it. */
int  tagpu_vk_gui_panel_rect(uint32_t slot, uint32_t w, uint32_t h, VkRect2D* r);

/* 1 on the ONE frame `tagpu_gui.ab` latched its claim and the `_vk.ppm` target
   was unlinked, so the seam captures THAT frame. It does not mean a file was
   written. Consumed. */
int  tagpu_vk_gui_ab_frame(void);

/* The owed-teardown protocol every pass on this lane uses: a refusal mid-frame
   may destroy nothing, because other slots' submits still name it. */
void tagpu_vk_gui_down(const TAGPU_VKPASS* d);
int  tagpu_vk_gui_down_owed(void);
void tagpu_vk_gui_down_paid(const TAGPU_VKPASS* d);

/* THE RESTORER IS GOING DOWN AND THIS PASS IS NOT -- a swapchain resize
   (`passes_down` in tagpu_vk.c) keeps the twins and drops the restorer. Gives
   back both of this pass's restore jobs, which `tagpu_vk_restore_down`
   requires of every owner: it forgets the job table, and a job pointer kept
   across it names a slot the next `tagpu_vk_restore_job_new` hands to another
   pass. Call it BEFORE `tagpu_vk_restore_down`, behind the device wait. */
void tagpu_vk_gui_restore_drop(const TAGPU_VKPASS* d);
#endif
