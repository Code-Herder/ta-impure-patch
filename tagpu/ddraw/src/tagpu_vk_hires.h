#ifndef TAGPU_VK_HIRES_H
#define TAGPU_VK_HIRES_H
/* The replacement meshes' CASTERS, drawn by Vulkan (the Vulkan-only plan's
   gate 3b). Contract only; tagpu_vk_hires.c is the pass.

   THIS PASS DRAWS INTO THE CAST-SHADOW MAP AND NOWHERE ELSE. The bodies still
   belong to `tagpu_hires_draw.c`'s GL program -- a glTF unit shaded per pixel
   with normal maps and metallic/roughness is not what this gate is about. What
   blocks the Vulkan lane is narrower and entirely mechanical: `tagpu_shadow.c`
   counts every caster the GL map holds that this side has no copy of, and a
   single Peewee with `hires/armpw.glb` active makes that count 1 -- which
   stands the shadow map down, and the terrain and unit passes with it. So the
   whole of gate 3b is: put those casters in the map, and stop the census
   refusing.

   WHAT IT IS FED. `tagpu_hires_handover` (tagpu_hires_draw.h), recorded by the
   GL depth pass AS IT DRAWS: the triangles as CPU bytes, the per-unit anchor,
   yaw/enc and cast triple exactly as the GL uniforms had them, and the pose
   rows. No GL name crosses, which is the whole-phase rule.

   THE ALBEDO DOES NOT CROSS, AND `cutoutSeen` IS WHY THAT IS HONEST. The depth
   path samples the albedo for one purpose -- the alpha cutout -- and every
   material of the shipped replacement is alphaMode OPAQUE, so the sample is
   discarded for every group. A frame that DOES carry a cutout group is one
   this pass cannot reproduce, and it draws nothing rather than a map missing
   its holes. */

#include "tagpu_vk_pass.h"

/* Take this frame's hand-over and put its vertices, poses and uniform blocks
   on the device. Called from the seam BESIDE `tagpu_vk_unit_upload` and before
   `tagpu_vk_shadow_prepare`, for the same reason that one is: the map's
   casters are this pass's geometry, so it all has to exist before the map is
   drawn. Puts no pixel anywhere. */
void tagpu_vk_hires_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* How many replacement-mesh casters this pass is READY to put in the map this
   frame. Valid after `upload` and before `cast`, which is where the shadow
   pass's census needs it.

   COMPARABLE TO `otherCasters` BY CONSTRUCTION, AND ONLY EVER SMALLER.
   tagpu_shadow.c counts what the GL pass drew; this counts the subset that
   reached the hand-over complete and whose device resources exist. The
   hand-over itself refuses to publish a record short of the GL map, so this is
   0 or all of them -- never a part. */
int  tagpu_vk_hires_casters(void);

/* Draw them, inside the caller's render pass, with the caller's viewport and
   scissor already set. `rp` is that render pass, needed once to build the
   pipeline against it; a second call with a different one rebuilds.

   RETURNS THE NUMBER DRAWN, or -1 when this pass has casters it could not draw.
   The caller adds the count to the unit pass's and treats -1 as "the map is
   incomplete", which is the same refusal. 0 is an ordinary answer: a frame
   with no replacement mesh on screen. */
int  tagpu_vk_hires_cast(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         VkRenderPass rp);

/* The seam's teardown, in the shape every other pass uses: `down` releases
   everything this pass owns, and is only safe after the seam's
   vkDeviceWaitIdle. */
void tagpu_vk_hires_down(const TAGPU_VKPASS* d);
int  tagpu_vk_hires_down_owed(void);
void tagpu_vk_hires_down_paid(const TAGPU_VKPASS* d);

#endif
