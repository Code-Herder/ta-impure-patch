#ifndef TAGPU_VK_RESTORE_H
#define TAGPU_VK_RESTORE_H
/* The Classic++ RESTORER, computed by Vulkan.
   Contract only; tagpu_vk_restore.c is the backend.

   IT IS THE BACKEND OF tagpu_restore_core.c, not a restorer of its own.
   The scheduler -- the job queues, batch formation, the pass sequencer, the
   cost model and the GPU-time budget -- is the core's, and this file
   implements the ten-entry TAGPU_RBACKEND against it. Everything below is
   therefore about DEVICE RESOURCES and FOUR COMPUTE SHADERS, and nothing below
   decides when to dispatch.

   IT IS NOT ONE OF THE FRAME'S PASSES, and tagpu_vk_shadow.c is the precedent
   rather than any of the world passes:

     * it writes buffers of its OWN (the activations) and the CONSUMER'S atlas
       image -- never the swapchain;
     * its dispatches are recorded inside the seam's `prepare`, before
       vkCmdBeginRenderPass, because a dispatch may not be recorded inside a
       render pass;
     * and IT MUST NOT BE COUNTED IN `ndraw` OR `nclaim`. Those count the
       passes that put pixels in THIS frame, so that two of them cannot
       contaminate one A/B capture. This pass puts none there, and counting it
       would refuse every capture taken with Classic++ on -- which is exactly
       the configuration restored art is measured in. The shadow pass carries
       the same warning for the same reason.

   WHAT IT IS FED, AND WHY THERE IS NO MIRROR. A job's two surfaces are
   ALREADY on the device for every consumer: the source is the pass's own
   atlas image (the RGBA base for a world pass, the R8 atlas for the UI) and
   the destination is the restored twin it already binds (binding 40 in
   tagpu_vk_feat.c, 42 in tagpu_vk_fx.c). So this pass needs no CPU mirror and
   no read-back. What the consumer owes is the destination's usage widened to
   carry STORAGE and its view lent here; the palette is the one thing that
   still crosses as bytes, because it is 1 KB and the engine's own table is the
   source of truth.

   THE PREREQUISITES ARE ASKED OF THE DEVICE, NOT ASSUMED. The shaders stay
   inside Vulkan 1.0's minimums, and what a device can still answer
   differently is refused BY NAME when it does not hold. */

#include "tagpu_vk_pass.h"
#include "tagpu_restoreglsl.h"      /* TAGPU_RGLSL_FRAME, the shared frame */

typedef struct TAGPU_VKRJOB TAGPU_VKRJOB;

/* Ask the device for everything this pass needs and say so in the log, once.
   1 when it can restore at all. Called from the seam once the device is up;
   every other entry point here is a no-op until it has returned 1.

   The refusals, each named in the log rather than reported as a failure:
     the submitting queue family has no COMPUTE
     R8G8B8A8_UNORM is not a storage image (the specification requires it)
     maxStorageBufferRange < the largest activation buffer
     no usable timestamps                                     fixed slices
   The timestamp one is the only soft refusal: without a GPU timer the core
   falls back to a fixed dispatch count per slice, which is slower and safe.
   Every other one stands the pass down, and Classic++ then draws the
   original dithered art -- which is the shipped fallback, not a fault. */
int  tagpu_vk_restore_up(const TAGPU_VKPASS* d);

/* A job: frames read from `srcView` (srcW x srcH) -- with `srcBase` 1 the
   consumer's RGBA8 BASE atlas, colours already expanded and alpha 0 at a key
   (the world passes); with 0 an R8 atlas of palette indices read through the
   palette `pal` (256 x R,G,B,pad; snapshotted now), which is the UI's -- painted
   into `dstImg`/`dstView` (RGBA8, dstW x dstH). The destination MUST have been
   created with VK_IMAGE_USAGE_STORAGE_BIT, and `dstView` must be an
   R8G8B8A8_UNORM view of level 0 alone; it is cleared to alpha 0
   here unless `repaint`, which is the palette-moved case that recolours in
   place instead of blanking the world for the length of the job.
   `tag` prefixes the log lines, `prio` orders it against the other jobs
   (terrain 0, features 1, effects 2, units 3, the UI 4, the UI's pictures
   5; a negative prio is the backend's own self-test), `oneshot` marks a fixed list whose completion is logged as the
   restore's "done" line, and `model` is the network it runs (TAGPU_RM_*).
   NULL, with the reason in tagpu.log, when the model or the device cannot.
   Render thread only, and only between the seam's frames. */
TAGPU_VKRJOB* tagpu_vk_restore_job_new(const TAGPU_VKPASS* d, const char* tag,
                                       int prio, int oneshot, int model, int repaint,
                                       VkImage srcImg, VkImageView srcView,
                                       int srcW, int srcH, int srcBase,
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
   i+1 (written, as a storage image) and level i (sampled) of `dstImg`, each
   naming EXACTLY ONE LEVEL. That is what makes reducing level i into level
   i+1 sound with no copy and no second image: the source view cannot reach
   the level being written, which is the guarantee `levelCount = 1` buys. The
   destination image needs STORAGE usage, as it already does for OUT.

   The levels are then reduced ONCE PER SLICE that painted, and not before the
   first paint: until `job_painted` moves the consumer does not sample the
   chain. The arithmetic is the exact integer (sum + 1) / 4 of gpu-status 2.45
   and 2.46.

   1 when the chain was registered. 0 leaves the job chainless -- it still
   restores level 0, and the consumer must then decide whether a twin with no
   levels is a picture it can draw. The views stay the CONSUMER's to destroy,
   and not before the job is freed. */
int  tagpu_vk_restore_job_chain(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j,
                                int mips, int dim,
                                const VkImageView* attach, const VkImageView* sample);

/* Queue frames (copied) behind what is already queued; they restore in order.
   The count taken, 0 if none was. */
int  tagpu_vk_restore_job_add(TAGPU_VKRJOB* j, const TAGPU_RGLSL_FRAME* frames, int count);
/* 1 when nothing is queued or in flight -- every frame added is painted, once
   the GPU drains, i.e. before any later draw samples the destination. */
int  tagpu_vk_restore_job_idle(const TAGPU_VKRJOB* j);
/* 1 when the job can do no more; its destination stays as it is. */
int  tagpu_vk_restore_job_failed(const TAGPU_VKRJOB* j);
/* Frames painted over the job's life -- for a consumer that must do something
   after each batch. Never reset by a clear; compare it for change. */
int  tagpu_vk_restore_job_painted(const TAGPU_VKRJOB* j);
/* THE ATLAS WAS RE-LAID AND ITS CELLS MOVED ON THE DEVICE, source and
   destination alike: the job's frames follow them (tagpu_rcore_job_remap --
   `map` rewrites a frame to its new rect or answers 0 for one whose entry is
   gone; the batch in flight goes back to the head of the queue). 1 done, 0
   when the job could not take it and the consumer must drop the job. */
int  tagpu_vk_restore_job_remap(TAGPU_VKRJOB* j, int (*map)(void* ctx, TAGPU_RGLSL_FRAME* f),
                                void* ctx, int* kept, int* requeued, int* dropped);
/* 1 when the destination is a picture a move may carry: made ready by the
   job's first OUT -- cleared, in SHADER_READ_ONLY_OPTIMAL -- and with no mip
   chain, whose levels a move of level 0 would leave behind. 0 before the first
   OUT, when the job's own clear is still to come. */
int  tagpu_vk_restore_job_dst_live(const TAGPU_VKRJOB* j);
void tagpu_vk_restore_job_free(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j);

/* `srcImg` IS THE IMAGE `srcView` NAMES, and it is here for the oracle rather
   than for the drawing: under `tagpu_restoredump.on` the source is written out
   beside the destination, so two dumps that differ -- two builds, say -- can
   be read as "the sources differed" or "the same source was restored
   differently" without another run: the destination alone cannot tell the
   two apart. */

/* THE BYTE ORACLE IS THIS FILE'S, and it needs no call of its own. Under
   `tagpu_restoredump.on` each job writes its finished destination to
   `tagpu_restore_<tag>_vk.rgba` out of `step`, once per picture: the copy is
   recorded into the frame's command buffer and read at that slot's next step,
   so nothing waits on the device. */

/* ONE SLICE, from the seam's `prepare` and NOWHERE ELSE: issue dispatches for
   the active job until the budget is spent. `cb` must be recording and OUTSIDE
   any render pass -- a dispatch may not be recorded inside one. `slot` is the
   frame slot, for the retire's mask and the byte dump.
   Does nothing until `up` has returned 1, and nothing while the renderer
   switch the jobs pause under is off; the queues keep filling either way. */
void tagpu_vk_restore_step(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* THE EPOCH A REFUSAL HOLDS FOR. A consumer that latches "the restorer
   refused" latches it for the epoch of its attempt, and asks again once this
   has moved: the render options' On after the restorer turned itself off
   (tagpu_restore_guard.h) moves it, and `up` answers afresh from then on. */
unsigned tagpu_vk_restore_epoch(void);

/* The device went with everything on it: forget every id without destroying,
   and every job with them. Call it BEFORE the jobs' owners forget theirs. */
void tagpu_vk_restore_lost(void);
/* Ordinary teardown, device still alive. EVERY JOB MUST HAVE BEEN FREED BY
   ITS OWNER FIRST: this forgets the job table, so an owner's pointer kept
   across it names a slot the next `job_new` hands to someone else. A job
   still standing here is freed and logged, which recovers its objects and
   not the owner's pointer. */
void tagpu_vk_restore_down(const TAGPU_VKPASS* d);

#endif
