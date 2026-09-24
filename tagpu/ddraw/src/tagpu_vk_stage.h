#ifndef TAGPU_VK_STAGE_H
#define TAGPU_VK_STAGE_H
/* THE BOUNDED UPLOAD: how a world pass copies host bytes into one of its
   shared images -- the base atlases of the unit, feature and effects passes,
   the terrain's base atlas and height grid -- through a staging buffer whose
   size a constant caps, whatever the size of the image.

   WHY A CAP. A full page of a base atlas is its used rows x 2048 x 4 bytes, up
   to 16 MB for units, features and effects, and the terrain's is its whole
   atlas, 2176 x 5338 x 4 = 44 MB on Town & Country. A full page is sent on the
   first upload, an atlas recycle, a band-ring overflow, a palette move and
   after every teardown (a swapchain rebuild runs every pass's `_down`). When
   each slot staged the whole page, a teardown that re-sent everything held
   tens of megabytes of host-visible memory mapped at once in a 32-bit process
   (gpu-status §2.88 has the numbers), and a refused allocation took the pass
   down for the session.

   THE SHAPE. Each slot keeps one TAGPU_VKSTAGE, at most TAGPU_VK_STAGE_CAP
   bytes, allocated on a frame that uploads and given back on that slot's next
   frame with nothing to send.

     * AN UPLOAD THAT FITS is recorded into the frame's own command buffer, as
       every pass did before: the seam waits on the slot's fence before handing
       the slot over, and that wait is what frees the buffer for the next
       frame on this slot.
     * AN UPLOAD THAT DOES NOT FIT goes out in BANDS of whole rows through this
       module's own command buffer. Each band is written into the free part of
       the slot's buffer, copied by a submit of its own, and WAITED ON before the
       next band overwrites the same bytes. The fence is the ordering: no band
       is written while the device can still be reading the one before it.

   WHAT A FRAME SAMPLES. Never a texel the device has not received. A banded
   upload is complete -- every band copied, the image back in
   SHADER_READ_ONLY_OPTIMAL -- before the call returns, so the frame that asked
   for it draws the whole page, as the in-frame copy did. Its submits reach the
   queue before the frame's own command buffer, so every command of that frame
   runs after the upload, including any recorded before the call; the image
   leaves in the layout they were recorded against. The price is a wait on the
   CPU: the first band's barrier orders its copy after every earlier frame's
   sampling of the image, so the first wait includes the frames still in
   flight. It is paid only by an upload larger than the cap -- a full page, or
   the union of a burst of paints -- and gpu-status §2.88 has what it costs.

   A REFUSAL NEVER LATCHES. `tagpu_vk_stage_begin` asks for what the frame needs
   up to the cap, and halves on a refusal down to one row of the widest upload.
   A slot that cannot map even that gets 0, and the pass skips the frame without
   advancing its serials: the upload is still due, and the next frame asks
   again. Only a device that fails a submit or a fence is a refusal of the
   pass, and that is the device going away.

   ONE UPLOAD PER IMAGE PER FRAME, and the calls of one frame share the slot's
   buffer: an in-frame upload holds its bytes until the frame's submit, and a
   banded one after it bands through what is left.

   RENDER THREAD ONLY, like every pass. The queue is the seam's, and the seam
   submits from this thread too, so the two never race for it. */

#include "tagpu_vk_pass.h"

/* Bytes one slot may map for its uploads, whatever the image. 1 MB is 128 rows
   of a 2048-wide RGBA8 atlas: the steady state -- the paints of one frame --
   fits under it (gpu-status §2.88), and a pass holds at most `slots` of them. */
#define TAGPU_VK_STAGE_CAP ((VkDeviceSize)1 << 20)

typedef struct {
    VkBuffer       buf;
    VkDeviceMemory mem;
    unsigned char* map;
    VkDeviceSize   cap;                    /* bytes mapped, <= TAGPU_VK_STAGE_CAP */
    VkDeviceSize   used;                   /* held this frame by in-frame copies  */
} TAGPU_VKSTAGE;

/* Make room for this frame's uploads: `want` bytes, capped, and never less
   than `floor` -- one row of the widest upload -- or nothing. 1 when the slot
   has a buffer to upload through, 0 when not even `floor` could be mapped. */
int  tagpu_vk_stage_begin(const TAGPU_VKPASS* d, TAGPU_VKSTAGE* st,
                          VkDeviceSize want, VkDeviceSize floor);

/* Give the slot's buffer back. Legal where the slot is the pass's own -- the
   seam has waited on its fence -- and nowhere else. */
void tagpu_vk_stage_drop(const TAGPU_VKPASS* d, TAGPU_VKSTAGE* st);

/* The rect (x, y, w, h) of `img`, an RGBA8 image, from the palette indices
   `idx` (row pitch `pitch`), through tagpu_pal_expand with `key` and `alpha`
   as it documents. `had` says the image holds contents earlier frames may be
   sampling: 1 orders the copy after them and keeps what lies outside the rect,
   0 takes it from UNDEFINED.
   1 sent, 0 no staging this frame (nothing recorded, the image untouched),
   -1 the device failed a submit or a fence partway (the pass must come down). */
int  tagpu_vk_stage_expand(const TAGPU_VKPASS* d, VkCommandBuffer cb,
                           TAGPU_VKSTAGE* st, VkImage img, int had,
                           const unsigned char* idx, const unsigned char* key,
                           int pitch, int x, int y, int w, int h,
                           const unsigned char* pal, const unsigned char* alpha);

/* The whole of a `w` x `h` image of `bpp` bytes a texel, from `src` (row pitch
   `pitch` bytes). Same arguments and answers as above. */
int  tagpu_vk_stage_copy(const TAGPU_VKPASS* d, VkCommandBuffer cb,
                         TAGPU_VKSTAGE* st, VkImage img, int had,
                         const unsigned char* src, int pitch, int w, int h, int bpp);

/* This module's command pool and fence, with the device. Called by the seam
   after every pass's `_down`, behind its vkDeviceWaitIdle. */
void tagpu_vk_stage_down(const TAGPU_VKPASS* d);

#endif
