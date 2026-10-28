#ifndef TAGPU_VK_HUD_H
#define TAGPU_VK_HUD_H
/* The replay HUD, drawn by Vulkan. Implementation: tagpu_vk_hud.c; plan:
   research/notes/hud-in-game.md. The two-phase contract of tagpu_vk_fps.h: `prepare` before the
   render pass, `record` inside it, both on the render thread on the command buffer the seam is
   recording, in one frame; `prepare` returning 0 means nothing to draw. `prepare` records a
   render pass of its own, the groups' layer (tagpu_vk_hud.c), so it must run outside the seam's.

   TWO LAYERS. The base layer is recorded after the world and before the engine's UI, so the
   engine's dialogs and the cursor draw over it. The top layer (HOPF_TOP, tagpu_hudlay.h: the live
   rewind's button, which sits in the top bar the UI paints whole, and its strip and toast, which
   the engine's message lines would cross; tagpu_hudlay.c, live) is recorded after the UI; then
   the whole UI again inside the Config panel's rect while the panel is open
   (tagpu_vk_gui_record_whole), and the UI's sharp layer -- the cursor -- inside the top layer's
   rect (tagpu_vk_gui_record_sharp), so the panel stays over the strip and the cursor stays the
   topmost thing on the screen. Both layers' groups are drawn into the one groups' layer.

   THE WIKI'S LAYER is a third (tagpu_vk_hud_record_wiki), after the top layer and the UI drawn
   again inside it: the in-game wiki's button and window over everything, the cursor alone put
   back over it (tagpu_vk_gui_record_cursor, tagpu_vk.c). `prepare` answers 1 when the HUD or the
   wiki draws. */
#include "tagpu_vk_pass.h"

/* Build or refresh what this frame needs and fill slot `slot`'s instances for this frame's HUD
   (tagpu_hudstate_frame), on a `w` x `h` attachment. */
int  tagpu_vk_hud_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h);

/* Draw the base layer, inside the render pass. */
void tagpu_vk_hud_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h);

/* Draw the top layer, inside the render pass, after the engine's UI: the same pipeline,
   viewport, scissor and buffer as `record`. 1 when it drew. */
int  tagpu_vk_hud_record_top(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                             uint32_t w, uint32_t h);

/* Draw the wiki's layer, the same way, after the top layer and what is drawn again over it, in
   two calls with the stage's 3D image between them (tagpu_wikiworld.h): `_wiki` the page and the
   stage's ops under the image -- every op of the stage on a whitebox frame -- and `_wiki_over` the
   stage's ops over it. 1 when the call drew. `_wiki_over` is the frame's last call of this pass,
   made whether or not the first drew. */
int  tagpu_vk_hud_record_wiki(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                              uint32_t w, uint32_t h);
int  tagpu_vk_hud_record_wiki_over(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                                   uint32_t w, uint32_t h);

/* The device pixels the top layer drew this frame, on the attachment: 1 with the rect, 0 when it
   is empty. */
int  tagpu_vk_hud_top_rect(VkRect2D* r);

/* Give everything back: the seam calls it from `vk_down`, after its vkDeviceWaitIdle and
   before the device is destroyed. Safe when nothing was built. */
void tagpu_vk_hud_down(const TAGPU_VKPASS* d);

#endif
