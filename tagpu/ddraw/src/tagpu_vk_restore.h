#ifndef TAGPU_VK_RESTORE_H
#define TAGPU_VK_RESTORE_H
/* The Classic++ RESTORER, drawn by Vulkan.
   Contract only; tagpu_vk_restore.c is the backend.

   IT IS THE SECOND BACKEND OF tagpu_restore_core.c, not a second restorer.
   The scheduler -- the job queues, batch formation, the pass sequencer, the
   cost model and the GPU-time budget -- is shared, and this file implements
   the twelve-entry TAGPU_RBACKEND against it. Everything below is therefore
   about DEVICE RESOURCES and THREE DRAWS, and nothing below decides when to
   draw.

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
   CPU mirror and no read-back. What the consumer owes is the
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
                                       VkImage srcImg, VkImageView srcView,
                                       int srcW, int srcH,
                                       const unsigned char* pal,
                                       VkImage dstImg, VkImageView dstView,
                                       int dstW, int dstH);
/* THE DEEPEST MIP LEVEL A REGISTERED CHAIN MAY HAVE. 12 covers a 4096 twin
   down to 1x1; the unit atlas, the only consumer with a chain, asks for 2. */
#define TAGPU_VK_MAXMIP 12

/* GIVE A JOB A MIP CHAIN, once, straight after `job_new` -- only a consumer
   whose twin is mipped calls this, and a job without it restores level 0
   alone.

   `mips` is the deepest level (1..TAGPU_VK_MAXMIP) and `dim` level 0's square
   size; `attach[i]` and `sample[i]`, for i in 0..mips-1, are views of level
   i+1 and level i of `dstImg`, each naming EXACTLY ONE LEVEL. That is what
   makes reducing level i into level i+1 sound with no copy and no second
   image: the source view cannot reach the level being written, which is the
   guarantee GL buys with GL_TEXTURE_BASE_LEVEL and this buys with
   `levelCount = 1`. The destination image needs COLOR_ATTACHMENT usage, as it
   already does for the OUT pass.

   The levels are then reduced ONCE PER SLICE that painted -- and once over an
   unpainted twin at the start, because a Vulkan image's levels begin UNDEFINED
   and the consumer samples the whole chain. The arithmetic is the GL lane's
   own: the exact integer (sum + 1) / 4 of gpu-status 2.45 and 2.46, from one
   shader string compiled for both APIs, so the chains are identical by
   construction rather than by two drivers agreeing.

   1 when the chain was registered. 0 leaves the job chainless -- it still
   restores level 0, and the consumer must then decide whether a twin with no
   levels is a picture it can draw. The views stay the CONSUMER's to destroy,
   and not before the job is freed. */
int  tagpu_vk_restore_job_chain(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j,
                                int mips, int dim,
                                const VkImageView* attach, const VkImageView* sample);

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

/* `srcImg` IS THE IMAGE `srcView` NAMES, and it is here for the oracle rather
   than for the drawing: under `tagpu_restoredump.on` the source is written out
   beside the destination, so a pair that differs can be read as "the two lanes
   restored different bytes" or "the two lanes restored the same bytes
   differently" without another run: the destination alone cannot tell the
   two apart. */

/* THE BYTE ORACLE IS THIS FILE'S, and it needs no call of its own. Under
   `tagpu_restoredump.on` each job writes its finished destination to
   `tagpu_restore_<tag>_vk.rgba` out of `step`, once per picture: the copy is
   recorded into the frame's command buffer and read at that slot's next step,
   so nothing waits on the device. */

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
