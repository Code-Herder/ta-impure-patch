#ifndef TAGPU_VK_UNIT_H
#define TAGPU_VK_UNIT_H
/* The posed unit bodies and their cast-shadow depth twins, drawn by Vulkan
   (Phase G / G19e, the SIXTH world pass and the last of the gate).
   Implementation: tagpu_vk_unit.c. The GL edition is tagpu_posedraw.c and
   stays the source of the vertices, the pose, the uniforms and the shader.

   IT IS THE FIRST PASS WITH FOUR HOOKS, and every one of them is forced by the
   same fact: this pass BOTH FEEDS AND SAMPLES the cast-shadow depth map.

     upload()    before tagpu_vk_shadow_prepare -- the vertex buffers this
                 frame's types need, the pose blocks, the texels. Nothing here
                 depends on the map, and the caster draw below needs all of it.
     cast()      called BY tagpu_vk_shadow.c from INSIDE its render pass, to
                 put this frame's posed casters in the map. Not called by the
                 seam, and never outside that render pass.
     prepare()   after the map is drawn -- point this slot's descriptor set at
                 it (and at the scaffold), decide whether to draw at all.
     record()    inside the SEAM's render pass -- bind and draw the bodies.

   The GL twin has exactly this shape and for exactly this reason: its depth
   twin draws into the shadow FBO at tagpu_native.c:3936, long before its
   bodies sample the finished map at :4238.

   A SET IS WRITTEN ONLY DURING ITS OWN SLOT'S `prepare`, which is the one
   instant the seam's fence proves it is not in flight -- so `cast` binds a
   SECOND, smaller set that names no image at all. That is not an optimisation:
   during the shadow pass's render pass this slot's shadow image IS the render
   target, and a set naming it as SHADER_READ_ONLY would be a descriptor
   describing the wrong layout for as long as the draw lasts.

   NOTHING HERE NAMES A WINDOW, A SURFACE OR A SWAPCHAIN -- standing
   constraint 3. A PASS READS NO ENGINE STATE: every value arrives through
   tagpu_posedraw.h's hand-over, so this file is not on thread-split.allow and
   must never be. */

#include "tagpu_vk_pass.h"

/* Build what is missing and upload this frame's geometry, pose and texels.
   Returns 1 when there is something for `cast` and `record` to draw.
   CALLED BEFORE tagpu_vk_shadow_prepare, and the seam says why at the site. */
int  tagpu_vk_unit_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* Draw this frame's posed casters into the map, inside the caller's render
   pass, with the caller's viewport and scissor already set. `rp` is that
   render pass, needed once to build the pipeline against it; a second call
   with a different one rebuilds.

   RETURNS THE NUMBER OF CASTERS DRAWN, or -1 when the pass has casters it
   could not draw -- a type whose mirror has gone, a buffer the device refused.
   The caller subtracts the count from its own census of casters it has no copy
   of, and treats -1 as "the map is incomplete", which is the same refusal.
   0 is an ordinary answer: a frame with no posed caster in it. */
int  tagpu_vk_unit_cast(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                        VkRenderPass rp);

/* How many posed casters this pass is READY to put in the map this frame: the
   units it has vertices for whose twin drew them into the GL map. Valid after
   `upload` and before `cast`, which is where the shadow pass needs it -- its
   own census of casters it has no copy of is taken before it begins its render
   pass, and this is the part of that census this pass answers for.

   THE TWO COUNTS ARE COMPARABLE BY CONSTRUCTION AND THIS ONE CAN ONLY BE
   SMALLER. tagpu_native.c counts every posed unit with `castSkip` clear; this
   counts the subset of those that reached the hand-over AND still have a bake
   mirror. So `otherCasters - casters()` is 0 exactly when every caster in the
   GL map has a copy here, and positive otherwise -- never negative. */
int  tagpu_vk_unit_casters(void);

/* Point this slot's set at the map and the scaffold, and say whether to draw.
   CALLED AFTER tagpu_vk_shadow_prepare. 0 means `record` must not be called. */
int  tagpu_vk_unit_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* Draw the bodies, inside the seam's render pass. `w`/`h` are its extent. */
void tagpu_vk_unit_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h);

/* 1 on the one frame the GL twin captured `tagpu_posedraw_gl.ppm`, so that the
   seam captures the SAME frame. Consumed by the call. Valid after `prepare`. */
int  tagpu_vk_unit_ab_frame(void);

/* Give everything back. Called by the seam from `vk_down`, after its
   vkDeviceWaitIdle and before the device is destroyed. Safe when nothing was
   ever built. */
void tagpu_vk_unit_down(const TAGPU_VKPASS* d);

/* 1 while the pass has stopped drawing mid-frame because the device would not
   give it this frame's resources, and is waiting for the seam to tear it down
   behind a vkDeviceWaitIdle. The contract, and why `_down_paid` is separate
   from `_down`, is tagpu_vk_scaffold.h's at length. */
int  tagpu_vk_unit_down_owed(void);
void tagpu_vk_unit_down_paid(const TAGPU_VKPASS* d);

#endif
