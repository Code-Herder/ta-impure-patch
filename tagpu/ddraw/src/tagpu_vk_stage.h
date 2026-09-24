#ifndef TAGPU_VK_STAGE_H
#define TAGPU_VK_STAGE_H
/* THE BOUNDED UPLOAD: how a world pass copies host bytes into one of its
   shared images -- the base atlases of the unit, feature and effects passes,
   the terrain's base atlas and height grid -- through a staging buffer whose
   size a constant caps, whatever the size of the image.

   WHY A CAP. A full page of a base atlas is its used rows x 2048 x 4 bytes, up
   to 16 MiB for units, features and effects, and the terrain's is its whole
   atlas, 2176 x 5338 x 4 = 46 461 952 bytes on Town & Country. Staged whole,
   the pages of one teardown hold 66.6 MiB of host-visible memory mapped at
   once in a 32-bit process (MEASURED, gpu-status §2.88), and one refused
   allocation is a pass down for the session.

   THE SHAPE. Each slot keeps one TAGPU_VKSTAGE, at most TAGPU_VK_STAGE_CAP
   bytes, allocated on a frame that uploads and given back on that slot's next
   frame with nothing to send. There are two ways through it, and which one a
   caller takes is a question of WHEN, not of size:

     * IN PLAY, AN UPLOAD IS IN THE FRAME'S OWN COMMAND BUFFER OR IT IS NOT
       MADE. `tagpu_vk_stage_expand_rects` -- the tiles an atlas painted since
       the copy the device holds -- records one copy into `cb` when its bytes
       fit what the slot mapped, and otherwise records nothing and answers 0:
       the caller skips the frame and asks again, and nothing waits. The atlas
       side is what makes it fit: an atlas with an allowance (tagpu_gaf.h
       `budget`) never has more tiles due than TAGPU_VK_STAGE_CAP holds, and
       defers a paint past it to a later frame. A repack's move is recorded
       into `cb` too (tagpu_vk_stage_move).
     * OUTSIDE PLAY, A WHOLE PAGE GOES IN BANDS (`tagpu_vk_stage_expand`,
       `tagpu_vk_stage_copy`) of whole rows through this module's own command
       buffer. Each band is written into the free part of the slot's buffer,
       copied by a submit of its own, and WAITED ON before the next band
       overwrites the same bytes. The fence is the ordering: no band is written
       while the device can still be reading the one before it. The events that
       send a whole page are a pass's first upload after it is built (the
       session's first level, and after every teardown or resize, which runs
       every pass's `_down`), the terrain's new atlas at a level's start, a
       palette move, and an atlas or mirror made, lost or begun for a new
       level -- none of them a frame of play, and each caller logs the cause
       when it sends one. The one way play can reach it is a device that
       refused the repack's move buffer (tagpu_vk_stage_move_ready); the
       caller then takes that repack as a whole page and logs it as a wait in
       play.

   WHAT A FRAME SAMPLES. Never a texel the device has not received. An
   in-frame copy is ordered before the frame's draws by its barriers. A banded
   upload is complete -- every band copied, the image back in
   SHADER_READ_ONLY_OPTIMAL -- before the call returns, so the frame that asked
   for it draws the whole page. Its submits reach the queue before the frame's
   own command buffer, so every command of that frame runs after the upload,
   including any recorded before the call; the image leaves in the layout they
   were recorded against. The price is a wait on the CPU: a fence on this queue
   completes only after everything submitted before it, so the first wait
   includes the frames still in flight -- which is why only the events above
   may take it.

   A REFUSAL NEVER LATCHES. `tagpu_vk_stage_begin` asks for what the frame needs
   up to the cap, and halves on a refusal down to one row of the widest upload.
   A slot that cannot map what an in-frame upload needs skips the frame without
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

/* Bytes one slot may map for its uploads, whatever the image: 2 MiB, which
   is TAGPU_GAF_BUDGET tiles of RGBA8 (asserted in tagpu_vk_stage.c) and so
   everything an atlas may have due at once. The largest single-frame paint
   measured in play is 1.20 MB, a camera jump at 4K (gpu-status §2.88).
   THE BOUND, IN ADDRESS SPACE: a pass holds at most one of these per frame
   slot, and only on frames that send -- the four world passes (units,
   features, effects, terrain) map at most 24 MiB at the three slots the
   swapchain gives and 64 MiB at the TAGPU_VK_SLOTS ceiling of eight, whatever
   the atlases. A frame with nothing to send maps nothing. */
#define TAGPU_VK_STAGE_CAP ((VkDeviceSize)2 << 20)

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
   its rect array by this and asks tagpu_gaf_dirty_since for no more. It is
   the allowance's tile count: every rect the dirty map names covers at least
   one due tile, so an atlas within its allowance never names more rects than
   this and never has one grown over tiles that were not due. */
#define TAGPU_VK_STAGE_MAXRECT 1024

/* `n` rects of `img` at once, each x0, y0, x1, y1 (exclusive), from the same
   indices, key, palette and alpha as above -- the tiles an atlas's paints
   touched since the copy the device holds. ONE copy of `n` regions, in the
   frame's own command buffer, and NEVER BANDED: 1 recorded (or nothing to
   send), 0 when the bytes do not fit what the slot mapped -- nothing is
   recorded and the image is untouched, and the caller skips the frame. */
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
   SHADER_READ_ONLY_OPTIMAL, and left there), in the frame's command buffer.
   1 moved, 0 nothing recorded (the image untouched: the caller keeps its
   serials and asks again). */
int  tagpu_vk_stage_move(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKMOVEBUF* mb,
                         VkImage img, int dim, const struct TAGPU_GAFMOVE* m, int n);

/* THE SAME MOVE FOR A RESTORED TWIN (tagpu_vk_feat.c): an image a render pass
   writes -- the restorer's OUT pass -- and whose texels the host cannot send
   again, since they exist only on the device. The cells move exactly as the
   base atlas's did, by the same list and through the same buffer, and
   EVERYTHING NO CELL LANDS ON IS CLEARED TO 0 in between: alpha 0 is what the
   draw reads as "not restored, take the base atlas" (tagpu_feat.c), and the
   rects a repack frees are handed to other entries, which must not sample
   what the old ones left there. `img` needs TRANSFER_SRC and TRANSFER_DST
   usage and is in SHADER_READ_ONLY_OPTIMAL, where the OUT pass leaves it, and
   is left there. Called after tagpu_vk_stage_move in the same command buffer,
   it reuses the buffer behind the barrier that orders every move's writes of
   it after the last one's reads. 1 moved, 0 nothing recorded. */
int  tagpu_vk_stage_move_twin(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKMOVEBUF* mb,
                              VkImage img, int dim, const struct TAGPU_GAFMOVE* m, int n);

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
