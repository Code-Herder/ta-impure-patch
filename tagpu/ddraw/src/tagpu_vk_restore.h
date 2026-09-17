#ifndef TAGPU_VK_RESTORE_H
#define TAGPU_VK_RESTORE_H
/* The Classic++ RESTORER, drawn by Vulkan (the Vulkan-only plan's landing 7).
   Contract only; tagpu_vk_restore.c is the backend.

   IT IS THE SECOND BACKEND OF tagpu_restore_core.c, not a second restorer.
   The scheduler -- the job queues, batch formation, the pass sequencer, the
   cost model and the GPU-time budget -- is shared, and this file implements
   the twelve-entry TAGPU_RBACKEND against it. That split landed first, and
   with the GL restorer still running as its oracle: the terrain and unit
   atlases came back byte-for-byte identical across the two builds. Everything
   below is therefore about DEVICE RESOURCES and THREE DRAWS, and nothing
   below decides when to draw.

   IT IS NOT ONE OF THE FRAME'S PASSES, and tagpu_vk_shadow.c is the precedent
   rather than any of the world passes:

     * it draws into targets of its OWN (the activation arrays) and into the
       CONSUMER'S atlas image -- never into the swapchain;
     * so its render passes are its own, begun inside the seam's `prepare`,
       which is legal there and nowhere else because render passes may not
       nest and `prepare` runs before vkCmdBeginRenderPass;
     * and IT MUST NOT BE COUNTED IN `ndraw` OR `nclaim`. Those count the
       passes that put pixels in THIS frame, so that two of them cannot
       contaminate one A/B capture. This pass puts none there, and counting it
       would refuse every capture taken with Classic++ on -- which is exactly
       the configuration restored art is measured in. The shadow pass carries
       the same warning for the same reason.

   WHAT IT IS FED, AND WHY THERE IS NO MIRROR. A job's two surfaces are
   ALREADY on the device for every consumer: the indexed source is the pass's
   own atlas image and the destination is the restored twin it already binds
   (binding 42 in tagpu_vk_feat.c, 43 in tagpu_vk_fx.c). So this pass needs no
   CPU mirror and no read-back -- it RETIRES the mirror gate 2 built rather
   than adding a second path beside it. What the consumer owes is the
   destination's usage widened to carry COLOR_ATTACHMENT and its view lent
   here; the palette is the one thing that still crosses as bytes, because it
   is 1 KB and the engine's own table is the source of truth.

   THE PREREQUISITES ARE ASKED OF THE DEVICE, NOT ASSUMED. Four limits and
   three formats, each refused BY NAME when it does not hold, the way `lineok`
   and `flipok` already are. They were measured on the reference setup and
   again through winevulkan, and they all held -- which is exactly why the
   check is here rather than skipped: a limit that holds on one device is not a
   property of the port. */

#include "tagpu_vk_pass.h"
#include "tagpu_restoreglsl.h"      /* TAGPU_RGLSL_FRAME, the shared frame */

typedef struct TAGPU_VKRJOB TAGPU_VKRJOB;

/* Ask the device for everything this pass needs and say so in the log, once.
   1 when it can restore at all. Called from the seam once the device is up;
   every other entry point here is a no-op until it has returned 1.

   The refusals, each named in the log rather than reported as a failure:
     maxColorAttachments / maxFragmentOutputAttachments < 1   no MRT at all
     maxUniformBufferRange < one k-block                      no weights fit
     maxImageArrayLayers   < the model's channel tiles         no activations
     no usable timestamps                                      fixed slices
     RGBA32F (or RGBA16F under `fp16`) not colour-attachment+sampled+linear
     RGBA8 not colour-attachment                               no destination
   The timestamp one is the only soft refusal: without a GPU timer the core
   falls back to a fixed draw count per slice, which is slower and safe. Every
   other one stands the pass down, and Classic++ then stays indexed on this
   lane -- which is the shipped fallback, not a fault. */
int  tagpu_vk_restore_up(const TAGPU_VKPASS* d);

/* NK as this lane settled it, and the uniform-block bytes one conv draw binds
   -- for the log and for a consumer that wants to report them. 0 before `up`. */
int  tagpu_vk_restore_nk(void);

/* A job: frames read from `srcView` (the consumer's R8 indexed atlas, srcW x
   srcH) with the palette `pal` (256 x R,G,B,pad; snapshotted now), painted
   into `dstImg`/`dstView` (RGBA8, dstW x dstH). The destination MUST have been
   created with VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; it is cleared to alpha 0
   here unless `repaint`, which is the palette-moved case that recolours in
   place instead of blanking the world for the length of the job.
   `tag` prefixes the log lines, `prio` orders it against the other jobs
   (terrain 0, features 1, effects 2, units 3, the UI 4), `oneshot` marks a
   fixed list whose completion is logged as the restore's "done" line.
   NULL, with the reason in tagpu.log, when the model or the device cannot.
   Render thread only, and only between the seam's frames. */
TAGPU_VKRJOB* tagpu_vk_restore_job_new(const TAGPU_VKPASS* d, const char* tag,
                                       int prio, int oneshot, int repaint,
                                       VkImageView srcView, int srcW, int srcH,
                                       const unsigned char* pal,
                                       VkImage dstImg, VkImageView dstView,
                                       int dstW, int dstH);
/* Re-point a live job at a new palette -- a lazy job outlives its atlas's
   entries, so it is re-palettable rather than replaceable. */
void tagpu_vk_restore_job_repalette(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j,
                                    const unsigned char* pal);
/* Queue frames (copied) behind what is already queued; they restore in order.
   The count taken, 0 if none was. */
int  tagpu_vk_restore_job_add(TAGPU_VKRJOB* j, const TAGPU_RGLSL_FRAME* frames, int count);
/* Drop everything queued or in flight and clear the destination again. */
void tagpu_vk_restore_job_clear(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j);
/* 1 when nothing is queued or in flight -- every frame added is painted, once
   the GPU drains, i.e. before any later draw samples the destination. */
int  tagpu_vk_restore_job_idle(const TAGPU_VKRJOB* j);
/* 1 when the job can do no more; its destination stays as it is. */
int  tagpu_vk_restore_job_failed(const TAGPU_VKRJOB* j);
/* Frames painted over the job's life -- for a consumer that must do something
   after each batch. Never reset by a clear; compare it for change. */
int  tagpu_vk_restore_job_painted(const TAGPU_VKRJOB* j);
void tagpu_vk_restore_job_free(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j);

/* ONE SLICE, from the seam's `prepare` and NOWHERE ELSE: issue draws for the
   active job until the budget is spent. `cb` must be recording and OUTSIDE any
   render pass -- this begins its own. `slot` is the frame slot, for the
   per-slot staging and the timestamp pair.
   Does nothing until `up` has returned 1, and nothing while the renderer
   switch the jobs pause under is off; the queues keep filling either way. */
void tagpu_vk_restore_step(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* The device went with everything on it: forget every id without destroying,
   and every job with them. Call it BEFORE the jobs' owners forget theirs. */
void tagpu_vk_restore_lost(void);
/* Ordinary teardown, device still alive. */
void tagpu_vk_restore_down(const TAGPU_VKPASS* d);

#endif
