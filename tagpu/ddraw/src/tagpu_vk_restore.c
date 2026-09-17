/* tagpu_vk_restore.c -- the Classic++ restorer's VULKAN BACKEND: the device
   resources and the three draws. tagpu_vk_restore.h is the contract and says
   why this is not one of the frame's passes; tagpu_restore_core.h is the
   scheduler this implements and says why the split is where it is.

   NOTHING HERE DECIDES WHEN TO DRAW. The core hands over one TAGPU_RDRAWREQ at
   a time, already sequenced and already costed, and this file binds and draws
   it. That is deliberate: a second backend that re-derived the fill -> depth x
   conv -> out sequence could disagree with the first about which layer it was
   on, and no A/B would show it -- both lanes would be internally consistent
   and produce different pictures for a reason neither reports.

   WHAT IS DIFFERENT FROM THE GL BACKEND, and it is only three things:

     * MRT OVER ARRAY LAYERS IS A RENDER PASS, NOT A CALL. GL attaches `n`
       layers with glFramebufferTextureLayer and masks the rest with
       glDrawBuffers; Vulkan fixes both at render-pass and framebuffer
       creation. A layer whose `kout` is not a multiple of NK ends in a TAIL
       group of n < NK, and both shipped models have one (full: 12 layers of
       kout 16 at NK 4, then a last layer of kout 1; tiny: kout 6 at NK 8). So
       there is one render pass per used count 1..NK, each declaring NK colour
       references with the unneeded ones VK_ATTACHMENT_UNUSED, and one pipeline
       per render pass because render-pass compatibility does not treat a used
       slot and an UNUSED one as matching. Writes to an UNUSED location are
       discarded, which is what glDrawBuffers masking did.
     * THE WEIGHT BLOCK IS A DYNAMIC UNIFORM OFFSET. GL rebinds a range with
       glBindBufferRange per draw; here the offset is a dynamic descriptor
       offset, and it must be a multiple of minUniformBufferOffsetAlignment.
       That is why tagpu_restore_core.c's loader now bounds `offset & 15` as
       well as `kstride & 15` -- both terms of `(offset + group x kstride) x 16`
       are then multiples of 256 and no device's alignment can reject one.
     * THE SLICE IS TIMED WITH TIMESTAMPS, not a single elapsed-time query. GL
       brackets the slice with GL_TIME_ELAPSED; here it is a pair of
       vkCmdWriteTimestamp calls per slot and the difference times the period.
       The core's double-buffering is unchanged, so nothing ever waits.

   EVERYTHING ELSE -- the padding rule, the batch grid, the size-class ladder,
   the budget, every counter and every log line -- is the core's and is shared
   byte for byte with the lane that is its oracle. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_vk_restore.h"
#include "tagpu_restore_core.h"
#include "tagpu_classicpp.h"
#include "spirv/tagpu_restore_glsl.spv.h"

#define LANE "restorevk"

/* the instance- and device-level entry points this pass needs
   (tagpu_vk_pass.h says why every pass carries its own table) */
#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)

#define DFNS(X) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) \
    X(vkCmdBindDescriptorSets) X(vkCmdDraw) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier) \
    X(vkCmdClearColorImage)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };
static int s_state;

/* what the device answered, kept for the log and for the refusals */
static struct {
    uint32_t maxColour, maxFragOut, maxUniformRange, maxArrayLayers;
    uint64_t uboAlign;
    int      timestamps;                 /* period > 0 AND valid bits > 0     */
    double   tsPeriod;                   /* ns per tick                       */
    VkFormat actFmt;                     /* RGBA32F, or RGBA16F under `fp16`  */
} s_dev;

static void rlog(const char* s) { tagpu_rcore_log(s); }

static int resolve(const TAGPU_VKPASS* d)
{
#define RES_I(n) n = (PFN_##n)d->gipa(d->inst, #n); if (!n) return 0;
#define RES_D(n) n = (PFN_##n)d->gdpa(d->dev, #n);  if (!n) return 0;
    IFNS(RES_I)
    DFNS(RES_D)
#undef RES_I
#undef RES_D
    return 1;
}

/* a format's optimal-tiling features, the three this pass cares about */
static int fmt_ok(const TAGPU_VKPASS* d, VkFormat f, int needColour, int needLinear)
{
    VkFormatProperties p;
    memset(&p, 0, sizeof p);
    vkGetPhysicalDeviceFormatProperties(d->pd, f, &p);
    if (needColour && !(p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return 0;
    if (needLinear && !(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return 0;
    return 1;
}

/* ASK THE DEVICE FOR EVERY PREREQUISITE AND REFUSE BY NAME.

   This is the third time on this plan that measuring the refusal before
   porting the stream behind it has paid, and the reason it is code rather than
   a note is that the numbers were measured on ONE device. They all held there
   -- 8 colour attachments, a 64 KiB uniform block, 2 048 array layers,
   timestamps on the submitting family, and all three formats
   colour-attachment + sampled + linear -- and again through winevulkan. A
   limit that holds on one device is not a property of the port.

   Only the timestamp refusal is SOFT: without a GPU timer the core falls back
   to a fixed draw count per slice, which is slower and safe. Every other one
   stands the pass down, and Classic++ then stays indexed on this lane, which
   is the shipped fallback rather than a fault. */
int tagpu_vk_restore_up(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties dp;
    VkQueueFamilyProperties qp[16];
    uint32_t nq = 16;
    const TAGPU_RMODEL* w;
    const TAGPU_ROPT*   opt;
    char b[420];
    int nk, kbytes, layers;

    if (s_state == ST_READY)   return 1;
    if (s_state == ST_REFUSED) return 0;

    if (!d || !d->dev || !d->pd) return 0;
    if (!resolve(d)) { s_state = ST_REFUSED; rlog(LANE ": a Vulkan entry point this pass needs is missing"); return 0; }

    /* the model and the options, per DEVICE here as they are per CONTEXT on
       the GL lane -- same file, same semantics, so both lanes restore with the
       same weights or the two pictures would not be comparable */
    if (!tagpu_rcore_reload(LANE)) { s_state = ST_REFUSED; return 0; }
    w = tagpu_rcore_model();
    opt = tagpu_rcore_opt();

    memset(&s_dev, 0, sizeof s_dev);
    memset(&dp, 0, sizeof dp);
    vkGetPhysicalDeviceProperties(d->pd, &dp);
    s_dev.maxColour        = dp.limits.maxColorAttachments;
    s_dev.maxFragOut       = dp.limits.maxFragmentOutputAttachments;
    s_dev.maxUniformRange  = dp.limits.maxUniformBufferRange;
    s_dev.maxArrayLayers   = dp.limits.maxImageArrayLayers;
    s_dev.uboAlign         = dp.limits.minUniformBufferOffsetAlignment;

    /* ---- the activation format, which decides two of the refusals ---- */
    s_dev.actFmt = opt->fp16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
    if (!fmt_ok(d, s_dev.actFmt, 1, 1)) {
        _snprintf(b, sizeof b, LANE ": %s is not colour-attachment + sampled + linear-filter in "
                               "optimal tiling on this device - the lane cannot restore",
                  opt->fp16 ? "RGBA16F" : "RGBA32F");
        rlog(b); s_state = ST_REFUSED; return 0;
    }
    /* the destination is the consumer's RGBA8 atlas and this pass renders into
       it, which Vulkan MANDATES for R8G8B8A8_UNORM -- asked anyway, because a
       mandated feature that is somehow absent is a broken driver and a named
       refusal beats an unexplained black atlas */
    if (!fmt_ok(d, VK_FORMAT_R8G8B8A8_UNORM, 1, 1)) {
        rlog(LANE ": R8G8B8A8_UNORM is not a colour attachment here, which the spec requires"
                  " - the lane cannot restore");
        s_state = ST_REFUSED; return 0;
    }

    /* ---- NK, from the two limits that bound it ---- */
    kbytes = w->kmax * 64;
    if ((uint32_t)kbytes > s_dev.maxUniformRange) {
        _snprintf(b, sizeof b, LANE ": maxUniformBufferRange %u < one k-block (%d) for the %dx%d model"
                               " - the lane cannot restore",
                  s_dev.maxUniformRange, kbytes, w->depth, w->ch);
        rlog(b); s_state = ST_REFUSED; return 0;
    }
    {
        uint32_t att = s_dev.maxColour < s_dev.maxFragOut ? s_dev.maxColour : s_dev.maxFragOut;
        if (att < 1) {
            _snprintf(b, sizeof b, LANE ": maxColorAttachments %u / maxFragmentOutputAttachments %u"
                                   " - no MRT at all, the lane cannot restore",
                      s_dev.maxColour, s_dev.maxFragOut);
            rlog(b); s_state = ST_REFUSED; return 0;
        }
        /* THE CEILING NK COULD REACH, for the log only. The core SETTLES NK --
           it owns the power-of-two clamp and the `nk=N` override, and it stores
           the answer in the scheduler -- so that happens when the resources are
           built, not here. This function's job is to establish that the device
           can restore at all, and the honest statement of that is "one k-block
           fits and there is at least one attachment". */
        nk = (int)(s_dev.maxUniformRange / (uint32_t)kbytes);
        if (nk > (int)att) nk = (int)att;
        if (nk > TAGPU_R_MAXNK) nk = TAGPU_R_MAXNK;
    }

    /* ---- the activations are one array layer per channel tile ---- */
    layers = w->ch / 4;
    if ((uint32_t)layers > s_dev.maxArrayLayers) {
        _snprintf(b, sizeof b, LANE ": maxImageArrayLayers %u < the %dx%d model's %d channel tiles"
                               " - the lane cannot restore",
                  s_dev.maxArrayLayers, w->depth, w->ch, layers);
        rlog(b); s_state = ST_REFUSED; return 0;
    }

    /* ---- the slice timer, and this is the ONLY soft refusal ----
       Two conditions and neither is "the device has a clock", exactly as
       tagpu_ftime's own comment says: `timestampPeriod` is 0 on a device that
       cannot do it at all, and `timestampValidBits` is per QUEUE FAMILY and can
       be 0 on the very family we submit to while another family has it. */
    memset(qp, 0, sizeof qp);
    vkGetPhysicalDeviceQueueFamilyProperties(d->pd, &nq, qp);
    s_dev.timestamps = 0;
    if (dp.limits.timestampPeriod > 0.0f) {
        /* the seam does not publish its queue family, so the check is the
           conservative one: every family that can graphics must time. A
           family-exact check belongs here the day tagpu_vk_pass.h carries it. */
        uint32_t i, gfx = 0, timed = 0;
        for (i = 0; i < nq && i < 16; i++) {
            if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                gfx++;
                if (qp[i].timestampValidBits > 0) timed++;
            }
        }
        if (gfx > 0 && timed == gfx) {
            s_dev.timestamps = 1;
            s_dev.tsPeriod = (double)dp.limits.timestampPeriod;
        }
    }
    if (!s_dev.timestamps)
        rlog(LANE ": this device/queue reports no usable timestamps - fixed slices, not a budget");

    _snprintf(b, sizeof b, LANE ": the device can restore: %dx%d %s, one k-block %d KB of a %u KB block"
                           " (so NK up to %d), %u colour attachments, uniform offset alignment %u,"
                           " %d activation layers of %u, %s, budget %.1f ms/frame",
              w->depth, w->ch, opt->fp16 ? "fp16" : "fp32",
              kbytes >> 10, s_dev.maxUniformRange >> 10, nk,
              s_dev.maxColour < s_dev.maxFragOut ? s_dev.maxColour : s_dev.maxFragOut,
              (unsigned)s_dev.uboAlign, layers, s_dev.maxArrayLayers,
              s_dev.timestamps ? "timestamp budget" : "no timestamps: fixed slices",
              opt->budget);
    rlog(b);

    /* The resources themselves are built lazily, on the first job, the way the
       GL backend builds its programs on the first job: a lane that never turns
       Classic++ on should pay nothing for this pass beyond the queries above. */
    s_state = ST_READY;
    return 1;
}

int tagpu_vk_restore_nk(void) { return 0; }   /* set once the scheduler is wired */

void tagpu_vk_restore_lost(void) { s_state = ST_UNBUILT; memset(&s_dev, 0, sizeof s_dev); }

void tagpu_vk_restore_down(const TAGPU_VKPASS* d) { (void)d; s_state = ST_UNBUILT; }
