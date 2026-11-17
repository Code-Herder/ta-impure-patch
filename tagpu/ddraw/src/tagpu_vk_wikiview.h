#ifndef TAGPU_VK_WIKIVIEW_H
#define TAGPU_VK_WIKIVIEW_H
/* tagpu_vk_wikiview -- the tech tree's unit view's picture: the stream tagpu_wikiview.c lays out,
   drawn under unitview.js's orbit camera into a target of its own the size of the view (twice
   over each way, and filtered down as it is composited), then composited into the view's rect on
   the frame, scissored to the unit's panel. The mockup's program: perspective with a vertical
   field of 0.9 radians, near and far at 0.05 and 10 times the distance, depth tested, back faces
   culled, a textured quad mapped per pixel, the colour lit by the face's own normal.

   ITS OWN COPY OF THE WIKI'S UNIT ATLAS. The stage's unit pass uploads the atlas only on frames
   that draw a stage, and the tech tree entry has none; so this pass keeps its own RGBA copy,
   brought up to date tile by tile in the frame's command buffer, as that pass does
   (tagpu_vk_unit.c base_upload), and says what it holds to the atlas (tagpu_wikiview.h).

   THE SEAM's calls, render thread only: `prepare` outside any render pass, after the wiki's
   stage's prepares and before the main pass begins -- it uploads and records the view's own
   offscreen pass whole; `record` inside the main pass, between tagpu_vk_hud_record_wiki_over and
   tagpu_vk_hud_record_wiki_top; `_down` behind the seam's device wait; a refusal asks for its
   teardown through `_down_owed`, settled by `_down_paid` behind the drain. */
#include "tagpu_vk_pass.h"

/* 1 when this frame draws the view: its picture is recorded, and `record` composites it. */
int  tagpu_vk_wikiview_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);
/* Composite the picture into the frame's `w` x `h` attachment; 1 when it drew. */
int  tagpu_vk_wikiview_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                              uint32_t w, uint32_t h);
void tagpu_vk_wikiview_down(const TAGPU_VKPASS* d);
int  tagpu_vk_wikiview_down_owed(void);
void tagpu_vk_wikiview_down_paid(const TAGPU_VKPASS* d);

#endif
