#ifndef TAGPU_VK_SHOT_H
#define TAGPU_VK_SHOT_H
/* One frame of the Vulkan lane, as a file. Implementation: tagpu_vk_shot.c.

   WHY THE LANE WRITES IT ITSELF. The only thing that can say what Vulkan put
   on the screen is Vulkan: this copies out of the image the lane drew, inside
   the command buffer the seam is recording. It is the file every ported
   pass's A/B lever claims (`tagpu_vk_ab_arm`), and two of them -- the same
   pass from two builds -- are what `tools/vk-ab.py a.ppm b.ppm` diffs.

   ONE FRAME, ONCE, UNDER A LEVER. It allocates a host-visible buffer the size
   of the frame, records the copy into the command buffer the seam is already
   recording, and gives the memory straight back once the file is written.
   Nothing of it survives the capture, so the steady-state lane is untouched. */

#include "tagpu_vk_pass.h"

/* Record the copy of `img` into a staging buffer of our own. `layout` is what
   the image is in when the call is made and what it is left in. Returns 0 when
   nothing was recorded, and then `finish` must not be called. */
int  tagpu_vk_shot_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, VkImage img,
                          VkImageLayout layout, uint32_t w, uint32_t h, VkFormat fmt);

/* Write the file and give the buffer back. THE CALLER MUST HAVE WAITED for the
   submit that carried the copy -- the seam's frame fence is what does it -- and
   this is the one place in the lane that is allowed to block, because a capture
   is one frame under a lever and not the steady state. */
void tagpu_vk_shot_finish(const TAGPU_VKPASS* d, const char* path);

/* Give anything still held back. Safe when nothing was captured. */
void tagpu_vk_shot_down(const TAGPU_VKPASS* d);

#endif
