#ifndef TAGPU_VK_SHOT_H
#define TAGPU_VK_SHOT_H
/* One frame of the Vulkan lane, as a file (Phase G / G19d). Implementation:
   tagpu_vk_shot.c.

   WHY IT IS NOT `tacli glshot`. That reads the GL framebuffer, and under route
   D the Vulkan lane draws into a window of its own that GL knows nothing about
   -- so the only thing that can say what Vulkan put on the screen is Vulkan.
   This is that, and it is the oracle half of every "0 px against its GL twin"
   claim Phase G will make: G19d compares it against tagpu_fps.c's own capture
   of the same frame, and G19e will compare it against each ported pass's.

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
