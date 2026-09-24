#ifndef TAGPU_VK_STAGE_H
#define TAGPU_VK_STAGE_H
/* THE BOUNDED UPLOAD: how a world pass copies host bytes into one of its
   shared images -- the base atlases of the unit, feature and effects passes,
   the terrain's base atlas and height grid -- through a staging buffer whose
   size a constant caps, whatever the size of the image.

   WHY A CAP. A full page of a base atlas is its used rows x 2048 x 4 bytes, up
   to 16 MiB for units, features and effects, and the terrain's is its whole
   atlas, 2176 x 5338 x 4 = 46 461 952 bytes on Town & Country. A full page is
   sent on a pass's first upload -- the session's first level, and after every
   teardown (a swapchain rebuild runs every pass's `_down`) -- the terrain's
   new atlas at a level's start, a palette move and a mirror's arm; in play a
   pass sends the tiles its atlas's dirty map names, and a feature repack
   moves its cells on the device (tagpu_vk_stage_move). Staged whole, the
   pages of one teardown hold 66.6 MiB of host-visible memory mapped at once
   in a 32-bit process (MEASURED, gpu-status §2.88), and one refused
   allocation is a pass down for the session.

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
   CPU: a fence on this queue completes only after everything submitted
   before it, so the first wait includes the frames still in flight. It is
   paid only by an upload larger than the cap -- a full page, or one frame's
   paints past 1 MiB -- and gpu-status §2.88 has what it costs.

   A REFUSAL NEVER LATCHES. `tagpu_vk_stage_begin` asks for what the frame needs
   up to the cap, and halves on a refusal down to one row of the widest upload.
   A slot that cannot map even that gets 0, and the pass skips the frame without
   advancing its serials: the upload is still due, and the next frame asks
   again. Only a device that fails a submit or a fence is a refusal of the
   pass, and that is the device going away.

   ONE UPLOAD PER IMAGE PER FRAME, and the calls of one frame share the slot's
   buffer: an in-frame upload holds its bytes until the frame's submit, and a
   banded one after it bands through what is left. A repack's move comes
   before the upload, through a device buffer of its own.

   RENDER THREAD ONLY, like every pass. The queue is the seam's, and the seam
   submits from this thread too, so the two never race for it. */

#include "tagpu_vk_pass.h"

/* Bytes one slot may map for its uploads, whatever the image. 1 MiB is 128
   rows of a 2048-wide RGBA8 atlas, and the steady state -- the paints of one
   frame -- fits under it (gpu-status §2.88). THE BOUND: a pass holds at most
   TAGPU_VK_SLOTS of these, 8 MiB, and the four world passes 32 MiB, whatever
   the atlases. */
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

/* The most rects one tagpu_vk_stage_expand_rects call takes. A caller sizes
   its rect array by this and asks tagpu_gaf_dirty_since for no more. */
#define TAGPU_VK_STAGE_MAXRECT 256

/* `n` rects of `img` at once, each x0, y0, x1, y1 (exclusive), from the same
   indices, key, palette and alpha as above -- the tiles an atlas's paints
   touched since the copy the device holds. When they all fit, ONE copy of `n`
   regions in the frame's own command buffer; when not, each rect banded.
   Same answers as above. */
int  tagpu_vk_stage_expand_rects(const TAGPU_VKPASS* d, VkCommandBuffer cb,
                                 TAGPU_VKSTAGE* st, VkImage img, int had,
                                 const unsigned char* idx, const unsigned char* key,
                                 int pitch, const int (*r)[4], int n,
                                 const unsigned char* pal, const unsigned char* alpha);

/* 1 when `bytes` more would go in-frame through the slot's buffer as it
   stands -- the test tagpu_vk_stage_expand_rects makes, so a caller that must
   know which path its rects take asks the same question. */
int  tagpu_vk_stage_fits(const TAGPU_VKSTAGE* st, VkDeviceSize bytes);

/* THE MOVE: an atlas repack's cells carried across `img` ON THE DEVICE, so a
   consumer whose copy was current before the repack takes nothing from the
   host but the paints it lacked (tagpu_gaf.h `moves`). The rows the old cells
   occupy go into a device-local buffer in one copy, and each cell comes back
   from its old place to its new one, so cells whose old and new rects overlap
   cannot read each other's new texels. The buffer is device-local memory the
   process never maps, unlike the staging above.
   THE BUFFER IS MADE ONCE, for the pass's `dim`, and lives until the pass
   comes down (tagpu_vk_stage_move_drop, behind the seam's vkDeviceWaitIdle).
   It is never re-made under a frame in flight, which is what lets every frame
   reuse it with only a barrier. */
struct TAGPU_GAFMOVE;
typedef struct {
    VkBuffer       buf;
    VkDeviceMemory mem;
    VkDeviceSize   size;
    int            refused;                /* refused: not asked again on this device */
} TAGPU_VKMOVEBUF;

/* 1 when `mb` holds a buffer for a `dim` square RGBA8 image (made on the first
   call) and every one of the `n` moves lies inside that square; 0 otherwise,
   and the caller sends the whole page instead. */
int  tagpu_vk_stage_move_ready(const TAGPU_VKPASS* d, TAGPU_VKMOVEBUF* mb, int dim,
                               const struct TAGPU_GAFMOVE* m, int n);

/* Move the `n` cells of `img` (TRANSFER_SRC and TRANSFER_DST usage, in
   SHADER_READ_ONLY_OPTIMAL, and left there). In the frame's command buffer
   when `banded` is 0. `banded` 1 says the uploads that follow it this frame
   will go banded, and those reach the queue before the frame's own command
   buffer: the move then goes through the banded path's command buffer,
   submitted and waited on first, so it lands before them.
   1 moved, 0 nothing recorded (the image untouched: the caller keeps its
   serials and asks again), -1 the device failed a submit or a fence. */
int  tagpu_vk_stage_move(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKMOVEBUF* mb,
                         VkImage img, int dim, const struct TAGPU_GAFMOVE* m, int n,
                         int banded);

/* The buffer, given back. Where the pass's images are: behind the seam's
   vkDeviceWaitIdle. */
void tagpu_vk_stage_move_drop(const TAGPU_VKPASS* d, TAGPU_VKMOVEBUF* mb);

/* The whole of a `w` x `h` image of `bpp` bytes a texel, from `src` (row pitch
   `pitch` bytes). Same arguments and answers as above. */
int  tagpu_vk_stage_copy(const TAGPU_VKPASS* d, VkCommandBuffer cb,
                         TAGPU_VKSTAGE* st, VkImage img, int had,
                         const unsigned char* src, int pitch, int w, int h, int bpp);

/* This module's command pool and fence, with the device. Called by the seam
   after every pass's `_down`, behind its vkDeviceWaitIdle. */
void tagpu_vk_stage_down(const TAGPU_VKPASS* d);

#endif
