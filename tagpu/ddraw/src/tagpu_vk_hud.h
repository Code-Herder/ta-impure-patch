#ifndef TAGPU_VK_HUD_H
#define TAGPU_VK_HUD_H
/* The replay HUD, drawn by Vulkan. Implementation: tagpu_vk_hud.c; plan:
   research/notes/hud-in-game.md. The two-phase contract of tagpu_vk_fps.h: `prepare` before the
   render pass, `record` inside it, both on the render thread on the command buffer the seam is
   recording, in one frame; `prepare` returning 0 means nothing to draw. It is recorded after the
   world and before the engine's UI, so the engine's dialogs and the cursor draw over it. */
#include "tagpu_vk_pass.h"

/* Build or refresh what this frame needs and fill slot `slot`'s instances for this frame's HUD
   (tagpu_hudstate_frame), on a `w` x `h` attachment. */
int  tagpu_vk_hud_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h);

/* Draw it, inside the render pass. */
void tagpu_vk_hud_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h);

/* Give everything back: the seam calls it from `vk_down`, after its vkDeviceWaitIdle and
   before the device is destroyed. Safe when nothing was built. */
void tagpu_vk_hud_down(const TAGPU_VKPASS* d);

#endif
