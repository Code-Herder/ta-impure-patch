/* tagpu_vk_restore.c -- the Classic++ restorer's VULKAN BACKEND: the device
   resources and the three draws. tagpu_vk_restore.h is the contract and says
   why this is not one of the frame's passes; tagpu_restore_core.h is the
   scheduler this implements and says why the split is where it is.

   NOTHING HERE DECIDES WHEN TO DRAW. The core hands over one TAGPU_RDRAWREQ at
   a time, already sequenced and already costed, and this file binds and draws
   it. That is deliberate: the sequence, the costing and the budget are the
   core's, and a backend that re-derived the fill -> depth x conv -> out
   sequence would be a second copy of it, free to disagree about which layer
   it was on while looking internally consistent.

   WHAT THE VULKAN API DECIDES HERE, in three places:

     * MRT OVER ARRAY LAYERS IS A RENDER PASS, NOT A CALL. Which layers are
       attached and which are masked is fixed at render-pass and framebuffer
       creation. A layer whose `kout` is not a multiple of NK ends in a TAIL
       group of n < NK, and both shipped models have one (full: 12 layers of
       kout 16 at NK 4, then a last layer of kout 1; tiny: kout 6 at NK 8). So
       there is one render pass per used count 1..NK, each declaring NK colour
       references with the unneeded ones VK_ATTACHMENT_UNUSED, and one pipeline
       per render pass because render-pass compatibility does not treat a used
       slot and an UNUSED one as matching. Writes to an UNUSED location are
       discarded.
     * THE WEIGHT BLOCK IS A DYNAMIC UNIFORM OFFSET. The range each draw reads
       is a dynamic descriptor offset, and it must be a multiple of
       minUniformBufferOffsetAlignment.
       That is why tagpu_restore_core.c's loader bounds `offset & 15` as
       well as `kstride & 15` -- both terms of `(offset + group x kstride) x 16`
       are then multiples of 256 and no device's alignment can reject one.
     * THE SLICE IS TIMED WITH TIMESTAMPS, not a single elapsed-time query: a
       pair of vkCmdWriteTimestamp calls per slot and the difference times the
       period.
       The core's double-buffering is unchanged, so nothing ever waits.

   AND IT NEEDS NO FLIP, WHICH IS A THIRD CASE BESIDE tagpu_vk_pass.h's
   `flipok` table. All three fragment shaders index the slot grid with
   `ivec2(gl_FragCoord.xy)`. Vulkan measures that y from the TOP --
   `OriginLowerLeft` is not permitted -- so gl_FragCoord.y ~ 0 is framebuffer
   row 0, which is image row 0, and NDC y = -1 maps to the viewport's top,
   which is that same row 0.

   So `gl_FragCoord.y` is the target's row index and NDC -1 is row 0. Which
   end of NDC is visually "up" matters only to a pass that speaks the SCREEN's
   y. This one never does:
   FILL and CONV cover the whole target with a full-screen triangle and address
   it in texels, and OUT positions each cell by `aPos / uDst * 2 - 1` from atlas
   coordinates and then recovers the same cell from gl_FragCoord. Nothing here
   has an opinion about up.

   `flipok`'s table is about screen spaces. This is a third case: IMAGE SPACE,
   where row 0 is row 0. It is recorded rather than trusted -- the byte dump
   below is what settles it, and a mirrored restore would show up as every
   tile's rows reversed.

   THE CHECK ON THIS FILE IS BYTES, NOT PIXELS. Under `tagpu_restoredump.on`
   each job writes its finished destination (`dump_step`), and that file can
   be `cmp`-ed against one kept from another build -- a byte comparison of a
   46 MB surface rather than a screenshot diff. It needs no window and no
   settle heuristic.

   EVERYTHING ELSE -- the padding rule, the batch grid, the size-class ladder,
   the budget, every counter and every log line -- is the core's
   (tagpu_restore_core.c). */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_vk_restore.h"
#include "tagpu_restore_core.h"
#include "tagpu_classicpp.h"
#include "tagpu_gaf.h"      /* tagpu_gaf_mip_off/_chain: the chain LAYOUT, so
                              the offsets the dump copies to come from the one
                              function rather than from a second copy of the
                              arithmetic. */
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
    X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkDeviceWaitIdle) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) \
    X(vkCmdBindDescriptorSets) X(vkCmdDraw) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdUpdateBuffer) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdPipelineBarrier) \
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

   It is code rather than a note because the numbers were measured on ONE
   device. They all held there
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

    /* the model and the options, reloaded per DEVICE (tagpu_restore_core.c) */
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

    /* The resources themselves are built lazily, on the first job: a lane that
       never turns Classic++ on should pay nothing for this pass beyond the
       queries above. */
    s_state = ST_READY;
    return 1;
}

/* ============================ RESOURCES ============================
   ALL OF THEM ARE SHARED, NOT PER SLOT, AND THE ARGUMENT IS AN ORDERING.

   A batch's draws can span slices: the budget cuts a batch off mid-way and the
   next slice resumes it, so the activation arrays and the slot tables must hold
   that batch's contents across submissions. Per-slot copies would be the
   reflex, and for the activations they are not affordable -- two RGBA32F arrays
   at 512 x 512 x 16 layers are 100 MB, and eight slots of them is not a
   trade-off, it is a different program.

   So they are shared and ORDERED instead. `vkCmdPipelineBarrier`'s first
   synchronisation scope includes every command submitted EARLIER IN SUBMISSION
   ORDER on the same queue, not merely earlier in the same command buffer, so
   one barrier at the head of each slice makes the previous slice's reads a fact
   before this slice's writes -- which is a fence rather than a hope, and is
   what CLAUDE.md's *Fixes must be safe by construction* calls an ordering. It
   costs one barrier per slice on background work that already has a GPU-time
   budget.

   THE ACTIVATIONS STAY IN VK_IMAGE_LAYOUT_GENERAL for their whole life. They
   alternate between colour attachment and sampled image on every layer, and
   GENERAL is valid for both; the alternative is a transition per ping-pong on
   16 array layers, for a target nothing outside this file ever sees. */

#define RP_MAX   TAGPU_R_MAXNK
#define MAXLAYER 64                 /* ch <= 256, so ch/4 <= 64              */
#define FB_CACHE 32
#define GLOBALS_RING 256            /* 16-byte blocks of scalar uniforms      */

static VkSampler             s_samp;        /* NEAREST, clamp: every sampler here */
static VkDescriptorSetLayout s_dslFill, s_dslConv, s_dslOut;
static VkPipelineLayout      s_ploFill, s_ploConv, s_ploOut;
static VkPipeline            s_pipeFill, s_pipeConv[RP_MAX + 1], s_pipeOut;
static VkRenderPass          s_rpAct[RP_MAX + 1];   /* index = USED count      */
static VkRenderPass          s_rpOut;
/* THE MIP REDUCTION, drawn with our own shader (MIP_FS), so a mipped twin's
   levels are that arithmetic by construction rather than whatever a driver's
   own mip generation does. gpu-status 2.45 is why. A job only has a chain if
   its consumer registered one. */
static VkRenderPass          s_rpMip;
static VkPipeline            s_pipeMip;
static VkDescriptorSetLayout s_dslMip;
static VkPipelineLayout      s_ploMip;
static VkDescriptorPool      s_dpool;
static int                   s_built;

static VkImage        s_actImg[2];
static VkDeviceMemory s_actMem[2];
static VkImageView    s_actArr[2];                  /* whole array, sampled    */
static VkImageView    s_actLay[2][MAXLAYER];        /* per layer, attachments  */
static int            s_actSide, s_actLayers;
static unsigned       s_actGen = 1;                 /* bumped on realloc       */

/* ---- THE RETIRE, and the framebuffers go into it TOO -------------------
   `tagpu_vk_terr.c`'s rule, applied to a bigger object: `pending` starts as
   every slot, a bit clears at the top of that slot's own step UNCONDITIONALLY
   (including on the paths that draw nothing -- clearing it only when the slot
   drew would stall the retire for ever on a lane that pauses), and
   `pending == 0` is what licenses the destroy.

   THE FRAMEBUFFERS ARE PART OF IT because they NAME the retired layer views.
   Destroying the images and keeping a framebuffer over them would be the same
   use-after-free one step removed, and it is the sort of thing a validation
   layer catches on someone else's machine rather than here. */
typedef struct {
    VkImage        img[2];
    VkDeviceMemory mem[2];
    VkImageView    arr[2];
    VkImageView    lay[2][MAXLAYER];
    int            layers;
    VkFramebuffer  fb[FB_CACHE];
    int            nfb;
    uint32_t       pending;                         /* slots yet to turn over  */
} RETIRE;
static RETIRE s_ret;

/* ---- AND A JOB'S OWN RESOURCES NEED THE SAME THING. Three of `job_free`'s
   four callers are in a consumer's `prepare`, where the seam has waited on
   THIS SLOT'S fence and no other. The other `slots - 1` submits are still in
   the queue naming the job's objects: the OUT render pass names `dstFb`, its
   descriptor set is `setOut[i]`, and FILL samples `palView`. Destroying them
   outright on a map change or a repaint within `slots - 1` frames
   of a draw (VUID-vkDestroyFramebuffer-framebuffer-00892,
   VUID-vkFreeDescriptorSets-pDescriptorSets-00309) is a crash on a strict
   driver and corrupt paint on a lax one.

   So they are retired too, with the activations' licence: `pending == 0` and
   not a frame count. There is ONE PATH rather than a fast path for idle
   callers -- `down` flushes these unconditionally because the seam has drained
   the device above it, so nothing here depends on which caller knew what.

   THE RING IS SIZED AT THE WORST CASE ITS CALLERS CAN PRODUCE, which is what
   makes it a bound rather than a guess. A consumer frees at most one job per
   frame -- the serial it keys on moves at most once per frame -- there are at
   most `TAGPU_R_MAXJOBS` consumers, and an entry is given back after `slots`
   frames, so no more than `TAGPU_R_MAXJOBS x TAGPU_VK_SLOTS` can be
   outstanding at once even if every consumer churned its serial on every
   frame. At 80-odd bytes an entry that is under 4 KB, so there is no reason to
   be clever about it.
   The full case is therefore unreachable, and it is still handled rather than
   asserted: the device is DRAINED and every entry freed immediately. A stall
   on a frame is honest; a destroy nobody has licensed is the bug this whole
   structure exists to prevent. */
typedef struct {
    VkFramebuffer   fb[1 + TAGPU_VK_MAXMIP];
    int             nfb;
    VkImage         palImg;
    VkDeviceMemory  palMem;
    VkImageView     palView;
    VkBuffer        palStage;
    VkDeviceMemory  palStageMem;
    /* FIVE SETS AND ONE FRAMEBUFFER, PLUS THE MIP CHAIN. A registered mip
       chain adds one framebuffer and one descriptor set PER
       LEVEL, and they are retired with everything else for the same reason: a
       reduction into them was recorded into some slot's command buffer, and a
       consumer frees its job when its generation moves, which is any frame. */
    VkDescriptorSet set[5 + TAGPU_VK_MAXMIP];
    int             nset;
    /* THE DUMP'S STAGING, when a job is freed with a copy into it still in
       flight. It is here for the same reason everything else is: the copy was
       recorded into the command buffer of the slot that took the dump, and a
       consumer frees its job when its generation moves -- which is any frame,
       not that slot's next one. "Called between frames" is not past the fence
       of the slot that recorded the copy, and that is the whole distinction
       this retire exists for. */
    VkBuffer        buf;      VkDeviceMemory bufMem;
    uint32_t        pending;
} JRETIRE;
#define JRET_MAX (TAGPU_R_MAXJOBS * TAGPU_VK_SLOTS)
static JRETIRE s_jret[JRET_MAX];

/* the framebuffer cache over (image, first layer, count) -- the combinations a
   run actually uses are few (one per conv group shape plus FILL's single
   layer), so a small linear cache beats computing them up front */
typedef struct { int img, first, count, side; VkFramebuffer fb; } FBE;
static FBE s_fb[FB_CACHE];
static int s_nfb;

/* the three 8x8 RGBA32F slot tables, and one host-visible staging buffer */
static VkImage        s_tabImg[3];
static VkDeviceMemory s_tabMem[3];
static VkImageView    s_tabView[3];
static VkBuffer       s_tabStage;
static VkDeviceMemory s_tabStageMem;
static int            s_tabInit;                    /* laid out GENERAL yet    */

static VkBuffer       s_wbuf;                       /* the padded weights      */
static VkDeviceMemory s_wmem;
static VkDeviceSize   s_wrange;                     /* WMAX * 64, the bound range */
static VkBuffer       s_wstage;                     /* ...and its one-time upload */
static VkDeviceMemory s_wstageMem;
static unsigned char* s_wstageMap;
static VkDeviceSize   s_wbytes;
static int            s_wUp;                        /* the copy has been recorded */
static const TAGPU_RBACKEND s_be;                   /* defined with the draws    */

static VkBuffer       s_gbuf;                       /* the scalar-uniform ring */
static VkDeviceMemory s_gmem;
static unsigned char* s_gmap;
static VkDeviceSize   s_gstride;
static uint32_t       s_gnext;
static int            s_gSaid;             /* the ring-full line, once     */

static VkBuffer       s_vbuf;                       /* the OUT vertices        */
static VkDeviceMemory s_vmem;

static VkQueryPool    s_qpool;                      /* 2 pairs, one per parity */

/* the slice's context, stashed by `step` for the vtable to reach */
static const TAGPU_VKPASS* s_d;
static VkCommandBuffer     s_cb;
static uint32_t            s_slot;
static int                 s_sliceOpen;             /* the head barrier is done */

static TAGPU_RSCHED s_sched;                        /* `be` set in build()      */

static uint32_t mem_type(const TAGPU_VKPASS* d, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    memset(&mp, 0, sizeof mp);
    vkGetPhysicalDeviceMemoryProperties(d->pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int mk_buffer(const TAGPU_VKPASS* d, VkDeviceSize bytes, VkBufferUsageFlags use,
                     VkMemoryPropertyFlags want, VkBuffer* buf, VkDeviceMemory* mem,
                     unsigned char** map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    uint32_t type;
    bci.size = bytes; bci.usage = use; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, buf) != VK_SUCCESS) return 0;
    memset(&req, 0, sizeof req);
    vkGetBufferMemoryRequirements(d->dev, *buf, &req);
    type = mem_type(d, req.memoryTypeBits, want);
    if (type == UINT32_MAX) { vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0; }
    mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) {
        vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0;
    }
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) return 0;
    if (map && vkMapMemory(d->dev, *mem, 0, bytes, 0, (void**)map) != VK_SUCCESS) return 0;
    return 1;
}

/* a 2D image, optionally an ARRAY: `layers` > 1 gives a 2D_ARRAY view too */
static int mk_image(const TAGPU_VKPASS* d, int w, int h, int layers, VkFormat fmt,
                    VkImageUsageFlags use, VkImage* img, VkDeviceMemory* mem, VkImageView* view)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements req;
    uint32_t type;

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = (uint32_t)w; ici.extent.height = (uint32_t)h; ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = (uint32_t)(layers > 0 ? layers : 1);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = use;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, img) != VK_SUCCESS) return 0;
    memset(&req, 0, sizeof req);
    vkGetImageMemoryRequirements(d->dev, *img, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { vkDestroyImage(d->dev, *img, NULL); *img = VK_NULL_HANDLE; return 0; }
    mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) {
        vkDestroyImage(d->dev, *img, NULL); *img = VK_NULL_HANDLE; return 0;
    }
    if (vkBindImageMemory(d->dev, *img, *mem, 0) != VK_SUCCESS) return 0;
    if (!view) return 1;
    ivi.image = *img;
    ivi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = (uint32_t)(layers > 0 ? layers : 1);
    return vkCreateImageView(d->dev, &ivi, NULL, view) == VK_SUCCESS;
}

/* one layer of an array image, as a 2D view for use as a colour attachment */
static int layer_view(const TAGPU_VKPASS* d, VkImage img, VkFormat fmt, int layer, VkImageView* v)
{
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivi.image = img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.baseArrayLayer = (uint32_t)layer;
    ivi.subresourceRange.layerCount = 1;
    return vkCreateImageView(d->dev, &ivi, NULL, v) == VK_SUCCESS;
}

/* ---- THE DEPENDENCY EVERY ONE OF THESE RENDER PASSES OWES ---------------
   The three draws of a slice are three render passes -- FILL writes activation
   layers, each CONV samples the layers the one before it wrote and writes its
   own, OUT samples the last of them and writes the consumer's atlas -- and
   NOTHING BETWEEN THEM ORDERS THEM unless it is said here.

   It is worth being exact about why a render-pass boundary is not enough. The
   implicit dependency Vulkan adds when `dependencyCount` is 0 has
   `dstStageMask = BOTTOM_OF_PIPE` and `dstAccessMask = 0`: it orders the
   attachment's LAYOUT TRANSITION and nothing else. A later draw's sample of
   what this pass wrote is not in that destination scope, so the read is
   unordered against the write. Drivers that flush at a render-pass boundary
   hide it -- which is the whole problem, because a hazard hidden by a driver's
   habit is the bug with better odds and not a fix (CLAUDE.md, *Fixes must be
   safe by construction*). The chain is FILL -> CONV -> ... -> OUT -> the
   terrain's own sample of the atlas, and the last link crosses out of this
   file entirely.

   So each pass states both ends, and the two together make the chain
   transitive: a 0 -> EXTERNAL dependency's destination scope is every
   subsequent command, so one pass's outgoing dependency covers the next pass's
   read whatever sits between them -- another slice, another frame's submit, or
   the consumer's own render pass. The incoming one is the WAR/WAW half: the
   previous slice may still be sampling these very layers.

   `BY_REGION` is deliberately NOT set. A conv draw reads a 3x3 neighbourhood
   of its input, and OUT reads the activation at the destination texel's own
   position in a DIFFERENT image; neither is a framebuffer-local read, and a
   by-region dependency would promise exactly the locality these do not have. */
static void rp_deps(VkSubpassDependency dep[2], int out)
{
    VkPipelineStageFlags fs = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkPipelineStageFlags co = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    /* TRANSFER IS IN BOTH SCOPES, and not speculatively: a consumer's
       destination is read back by a copy -- `dump_step`'s byte dump -- and
       `dst_ready`'s own clear writes it with one. A dependency that named
       only the sampling reader would order the picture and not the
       measurement of it. */
    VkPipelineStageFlags tr = VK_PIPELINE_STAGE_TRANSFER_BIT;
    memset(dep, 0, 2 * sizeof *dep);
    /* in: whatever was reading or writing these attachments, before we write */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = fs | co | tr;
    dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dep[0].dstStageMask = co;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    /* ...and the OUT pass LOADS, so it reads the attachment as well as writes it */
    if (out) dep[0].dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    /* out: our writes, before anything samples them, copies them, or writes over them */
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = co;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = fs | co | tr;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
}

/* A RENDER PASS PER USED ATTACHMENT COUNT. The subpass always declares NK
   colour references, so one pipeline shape fits, and the references past
   `used` are VK_ATTACHMENT_UNUSED -- which is what discards the shader's
   writes to those locations. A tail group is not hypothetical: `full` is
   eleven layers of kout 16 at NK 4 and then a last layer of kout 1, and
   `tiny` is kout 6 at NK 8.
   LOAD is DONT_CARE because a conv draw covers every fragment of its target,
   and the layout is GENERAL at both ends because the activations never leave
   it. */
static int build_rp_act(const TAGPU_VKPASS* d, int used, int nk)
{
    VkAttachmentDescription at[RP_MAX];
    VkAttachmentReference   ref[RP_MAX];
    VkSubpassDescription    sp;
    VkSubpassDependency     dep[2];
    VkRenderPassCreateInfo  rci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    int i;
    if (used < 1 || used > nk) return 0;
    memset(at, 0, sizeof at); memset(ref, 0, sizeof ref); memset(&sp, 0, sizeof sp);
    for (i = 0; i < used; i++) {
        at[i].format = s_dev.actFmt;
        at[i].samples = VK_SAMPLE_COUNT_1_BIT;
        at[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[i].initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        at[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        ref[i].attachment = (uint32_t)i;
        ref[i].layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    for (i = used; i < nk; i++) {
        ref[i].attachment = VK_ATTACHMENT_UNUSED;
        ref[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = (uint32_t)nk;
    sp.pColorAttachments = ref;
    rp_deps(dep, 0);
    rci.attachmentCount = (uint32_t)used;
    rci.pAttachments = at;
    rci.subpassCount = 1;
    rci.pSubpasses = &sp;
    rci.dependencyCount = 2;
    rci.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &rci, NULL, &s_rpAct[used]) == VK_SUCCESS;
}

/* The OUT pass renders into the CONSUMER's atlas, so it LOADS (cells are
   painted onto what is already there) and it transitions the image in and out
   of SHADER_READ_ONLY_OPTIMAL itself -- which is what render-pass layout
   transitions are for, and it means the consumer's descriptor keeps naming the
   layout it already names. */
static int build_rp_out(const TAGPU_VKPASS* d)
{
    VkAttachmentDescription at;
    VkAttachmentReference   ref;
    VkSubpassDescription    sp;
    VkSubpassDependency     dep[2];
    VkRenderPassCreateInfo  rci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    memset(&at, 0, sizeof at); memset(&ref, 0, sizeof ref); memset(&sp, 0, sizeof sp);
    at.format = VK_FORMAT_R8G8B8A8_UNORM;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    rp_deps(dep, 1);
    rci.attachmentCount = 1; rci.pAttachments = &at;
    rci.subpassCount = 1; rci.pSubpasses = &sp;
    rci.dependencyCount = 2; rci.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &rci, NULL, &s_rpOut) == VK_SUCCESS;
}

static VkFramebuffer fb_for(const TAGPU_VKPASS* d, int img, int first, int count)
{
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkImageView v[RP_MAX];
    int i;
    for (i = 0; i < s_nfb; i++)
        if (s_fb[i].img == img && s_fb[i].first == first &&
            s_fb[i].count == count && s_fb[i].side == s_actSide) return s_fb[i].fb;
    if (s_nfb >= FB_CACHE || !s_rpAct[count]) return VK_NULL_HANDLE;
    for (i = 0; i < count; i++) {
        if (first + i >= s_actLayers) return VK_NULL_HANDLE;
        v[i] = s_actLay[img][first + i];
    }
    fci.renderPass = s_rpAct[count];
    fci.attachmentCount = (uint32_t)count;
    fci.pAttachments = v;
    fci.width = (uint32_t)s_actSide; fci.height = (uint32_t)s_actSide; fci.layers = 1;
    if (vkCreateFramebuffer(d->dev, &fci, NULL, &s_fb[s_nfb].fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_fb[s_nfb].img = img; s_fb[s_nfb].first = first;
    s_fb[s_nfb].count = count; s_fb[s_nfb].side = s_actSide;
    return s_fb[s_nfb++].fb;
}

static void fb_flush(const TAGPU_VKPASS* d)
{
    int i;
    for (i = 0; i < s_nfb; i++)
        if (s_fb[i].fb) vkDestroyFramebuffer(d->dev, s_fb[i].fb, NULL);
    memset(s_fb, 0, sizeof s_fb);
    s_nfb = 0;
}

/* THE CONV VARIANT, PICKED BY (NK, kmax). `spirv-gen.py` emits the cross
   product `NK in {1,2,4,8}` x `kmax in {56, 148}` -- eight modules -- because
   `#if NK > 1` declares a different number of `out` locations and SPIR-V
   interface variables are static, and because WMAX = NK x kmax is the declared
   length of `WBlock`. THE LANE IS THEREFORE PINNED TO THE TWO SHIPPED MODELS:
   a third model needs
   its four variants generated and committed, and until then this refuses it by
   name rather than restoring with the wrong block length. */
static const uint32_t* conv_spv(int nk, int kmax, size_t* words)
{
#define PICK(NKV, KV, SYM) \
    if (nk == (NKV) && kmax == (KV)) { \
        *words = sizeof tagpu_spv_tagpu_restore_glsl_##SYM / 4; \
        return tagpu_spv_tagpu_restore_glsl_##SYM; }
    PICK(1, 56,  CONV_FS_NK1_K56)   PICK(1, 148, CONV_FS_NK1_K148)
    PICK(2, 56,  CONV_FS_NK2_K56)   PICK(2, 148, CONV_FS_NK2_K148)
    PICK(4, 56,  CONV_FS_NK4_K56)   PICK(4, 148, CONV_FS_NK4_K148)
    PICK(8, 56,  CONV_FS_NK8_K56)   PICK(8, 148, CONV_FS_NK8_K148)
#undef PICK
    *words = 0;
    return NULL;
}

static VkShaderModule mk_mod(const TAGPU_VKPASS* d, const uint32_t* w, size_t words)
{
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    smi.codeSize = words * 4;
    smi.pCode = w;
    if (vkCreateShaderModule(d->dev, &smi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

/* one graphics pipeline. No depth state at all, because none of this pass's
   render passes has a depth attachment -- tagpu_vk_pass.h's rule that every
   pipeline must declare one applies to pipelines built against the SEAM's
   render pass, which has one. `nColour` is the subpass's colour count, which
   the blend state must match exactly. */
static VkPipeline mk_pipe(const TAGPU_VKPASS* d, VkShaderModule vs, VkShaderModule fs,
                          VkRenderPass rp, int nColour, VkPipelineLayout plo,
                          const VkPipelineVertexInputStateCreateInfo* vi)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo viNone = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba[RP_MAX];
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkDynamicState dsv[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipeline out = VK_NULL_HANDLE;
    int i;

    memset(st, 0, sizeof st); memset(cba, 0, sizeof cba);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    for (i = 0; i < nColour && i < RP_MAX; i++) {
        cba[i].blendEnable = VK_FALSE;
        cba[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    cb.attachmentCount = (uint32_t)nColour;
    cb.pAttachments = cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dsv;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = vi ? vi : &viNone;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = plo;
    gp.renderPass = rp;                 /* OURS, never the seam's */
    gp.subpass = 0;
    if (vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &out) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return out;
}

/* THE MIP PASS WRITES A WHOLE LEVEL, so it DISCARDS rather than loads, and it
   takes the level from UNDEFINED -- which is not a shortcut but the thing that
   makes a freshly created twin legal to reduce into: a Vulkan image's levels
   begin UNDEFINED, and a render pass that promised to LOAD one would be reading
   memory with no defined contents. `finalLayout` leaves every level it writes
   SHADER_READ_ONLY_OPTIMAL, which is the layout the consumer's own descriptor
   already names for the whole chain, so nothing else transitions it.
   The attachment is a view of ONE LEVEL, so these transitions apply to that
   level alone and the rest of the chain is untouched. */
static int build_rp_mip(const TAGPU_VKPASS* d)
{
    VkAttachmentDescription at;
    VkAttachmentReference   ref;
    VkSubpassDescription    sp;
    VkSubpassDependency     dep[2];
    VkRenderPassCreateInfo  rci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    memset(&at, 0, sizeof at); memset(&ref, 0, sizeof ref); memset(&sp, 0, sizeof sp);
    at.format = VK_FORMAT_R8G8B8A8_UNORM;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    /* `out` 0: this pass never reads its attachment. The dependency pair still
       names FRAGMENT_SHADER on both sides, which is what orders level L-1's
       write against level L's READ of it -- consecutive render passes over
       levels of one image, and the hazard the whole reduction turns on. */
    rp_deps(dep, 0);
    rci.attachmentCount = 1; rci.pAttachments = &at;
    rci.subpassCount = 1; rci.pSubpasses = &sp;
    rci.dependencyCount = 2; rci.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &rci, NULL, &s_rpMip) == VK_SUCCESS;
}

/* THE THREE LAYOUTS ARE THREE, not one, because the SAMPLER INDICES DIFFER
   BETWEEN THE PROGRAMS. spirv-gen.py allocates bindings by STAGE in
   declaration order, so FILL has uAtlas at 40 while OUT has uAct at 40 and
   uAtlas at 41. One shared layout would bind the atlas where OUT expects the
   activations. [Read out of inc/spirv/tagpu_restore_glsl.spv.h, whose per-
   shader interface comment is the contract.] */
static VkDescriptorSetLayout mk_dsl(const TAGPU_VKPASS* d, const int* bind,
                                    const VkDescriptorType* type,
                                    const VkShaderStageFlags* stage, int n)
{
    VkDescriptorSetLayoutBinding b[8];
    VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout out = VK_NULL_HANDLE;
    int i;
    memset(b, 0, sizeof b);
    for (i = 0; i < n && i < 8; i++) {
        b[i].binding = (uint32_t)bind[i];
        b[i].descriptorType = type[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags = stage[i];
    }
    ci.bindingCount = (uint32_t)n;
    ci.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &ci, NULL, &out) != VK_SUCCESS) return VK_NULL_HANDLE;
    return out;
}

static VkPipelineLayout mk_plo(const TAGPU_VKPASS* d, VkDescriptorSetLayout dsl)
{
    VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout out = VK_NULL_HANDLE;
    ci.setLayoutCount = 1; ci.pSetLayouts = &dsl;
    if (vkCreatePipelineLayout(d->dev, &ci, NULL, &out) != VK_SUCCESS) return VK_NULL_HANDLE;
    return out;
}

/* ---- the retire's two halves ---- */
static void retire_take(void)
{
    int i, l;
    /* the current activations and every framebuffer over them become the
       retire; the caller has already checked `pending == 0` */
    memset(&s_ret, 0, sizeof s_ret);
    for (i = 0; i < 2; i++) {
        s_ret.img[i] = s_actImg[i]; s_ret.mem[i] = s_actMem[i]; s_ret.arr[i] = s_actArr[i];
        for (l = 0; l < MAXLAYER; l++) s_ret.lay[i][l] = s_actLay[i][l];
        s_actImg[i] = VK_NULL_HANDLE; s_actMem[i] = VK_NULL_HANDLE; s_actArr[i] = VK_NULL_HANDLE;
        memset(s_actLay[i], 0, sizeof s_actLay[i]);
    }
    s_ret.layers = s_actLayers;
    for (i = 0; i < s_nfb; i++) s_ret.fb[i] = s_fb[i].fb;
    s_ret.nfb = s_nfb;
    memset(s_fb, 0, sizeof s_fb);
    s_nfb = 0;
    s_ret.pending = 0xFFFFFFFFu;        /* set properly by the caller, which has `d` */
}

/* Destroy one job retire's contents. The caller has established the licence:
   either its `pending` reached 0, or the device has been drained. */
static void jret_kill(const TAGPU_VKPASS* d, JRETIRE* r)
{
    int i;
    for (i = 0; i < r->nfb; i++)
        if (r->fb[i]) vkDestroyFramebuffer(d->dev, r->fb[i], NULL);
    if (r->palView) vkDestroyImageView(d->dev, r->palView, NULL);
    if (r->palImg) vkDestroyImage(d->dev, r->palImg, NULL);
    if (r->palMem) vkFreeMemory(d->dev, r->palMem, NULL);
    if (r->palStage) vkDestroyBuffer(d->dev, r->palStage, NULL);
    if (r->palStageMem) vkFreeMemory(d->dev, r->palStageMem, NULL);
    if (r->buf) vkDestroyBuffer(d->dev, r->buf, NULL);
    if (r->bufMem) vkFreeMemory(d->dev, r->bufMem, NULL);
    if (r->nset && s_dpool) vkFreeDescriptorSets(d->dev, s_dpool, (uint32_t)r->nset, r->set);
    memset(r, 0, sizeof *r);
}

/* Give every job retire back, whatever its mask says. Only legal where the
   device has been drained -- `down`, which the seam's vkDeviceWaitIdle is
   above, and the ring-full path, which drains it first. */
static void jret_flush(const TAGPU_VKPASS* d)
{
    int i;
    if (!d || !d->dev) { memset(s_jret, 0, sizeof s_jret); return; }
    for (i = 0; i < JRET_MAX; i++) if (s_jret[i].pending) jret_kill(d, &s_jret[i]);
}

/* A free slot of the ring, draining the device to make one if it is full. */
static JRETIRE* jret_take(const TAGPU_VKPASS* d)
{
    int i;
    for (i = 0; i < JRET_MAX; i++) if (!s_jret[i].pending) return &s_jret[i];
    rlog(LANE ": the job retire is full, so the device is drained to empty it "
               "-- a stall on this frame and nothing worse");
    if (vkDeviceWaitIdle) vkDeviceWaitIdle(d->dev);
    jret_flush(d);
    return &s_jret[0];
}

/* The activation retire's contents, destroyed. Split out from the slot walk so
   that `down` can use it under the seam's device drain. */
static void retire_kill(const TAGPU_VKPASS* d)
{
    int i, l;
    for (i = 0; i < s_ret.nfb; i++)
        if (s_ret.fb[i]) vkDestroyFramebuffer(d->dev, s_ret.fb[i], NULL);
    for (i = 0; i < 2; i++) {
        for (l = 0; l < MAXLAYER; l++)
            if (s_ret.lay[i][l]) vkDestroyImageView(d->dev, s_ret.lay[i][l], NULL);
        if (s_ret.arr[i]) vkDestroyImageView(d->dev, s_ret.arr[i], NULL);
        if (s_ret.img[i]) vkDestroyImage(d->dev, s_ret.img[i], NULL);
        if (s_ret.mem[i]) vkFreeMemory(d->dev, s_ret.mem[i], NULL);
    }
    memset(&s_ret, 0, sizeof s_ret);
}

/* A SLOT HAS TURNED OVER: its last submit is complete, because the seam waited
   on that slot's fence before handing us this command buffer. Called at the top
   of every step, UNCONDITIONALLY. */
static void retire_slot_done(const TAGPU_VKPASS* d, uint32_t slot)
{
    int i;
    /* THE JOB RETIRES FIRST, and they are independent of the activation one --
       several can be outstanding at once, each with its own mask, because a job
       is freed whenever its consumer's serial moves and not on a schedule. */
    for (i = 0; i < JRET_MAX; i++) {
        if (!s_jret[i].pending) continue;
        s_jret[i].pending &= ~(1u << slot);
        if (!s_jret[i].pending) jret_kill(d, &s_jret[i]);
    }
    if (!s_ret.pending) return;
    s_ret.pending &= ~(1u << slot);
    if (s_ret.pending) return;
    retire_kill(d);
}

/* ---- the shared build, on the first job ---- */
static int build_shared(const TAGPU_VKPASS* d)
{
    const TAGPU_RMODEL* w = tagpu_rcore_model();
    const TAGPU_ROPT*   opt = tagpu_rcore_opt();
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorPoolSize psz[2];
    VkShaderModule vsFS = VK_NULL_HANDLE, vsOut = VK_NULL_HANDLE;
    VkShaderModule fsFill = VK_NULL_HANDLE, fsConv = VK_NULL_HANDLE, fsOut = VK_NULL_HANDLE;
    VkShaderModule fsMip = VK_NULL_HANDLE;
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[3];
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    const uint32_t* convWords;
    size_t convN = 0;
    char b[300];
    int i, nk, ok = 0;

    if (s_built) return 1;
    if (!tagpu_vk_restore_up(d)) return 0;

    /* NK, settled by the core (the power-of-two clamp and the `nk=N` override
       are its rules, not ours), from the limits this device reported */
    {
        uint32_t att = s_dev.maxColour < s_dev.maxFragOut ? s_dev.maxColour : s_dev.maxFragOut;
        s_sched.be = &s_be;
        nk = tagpu_rcore_pick_nk(&s_sched, (int)s_dev.maxUniformRange, (int)att);
    }
    if (!nk) return 0;
    s_wrange = (VkDeviceSize)s_sched.wmax * 64;

    convWords = conv_spv(nk, w->kmax, &convN);
    if (!convWords) {
        _snprintf(b, sizeof b, LANE ": no conv shader for NK=%d kmax=%d -- this lane ships the two "
                               "models only (tiny kmax 56, full kmax 148); a third needs its four "
                               "variants generated and committed", nk, w->kmax);
        rlog(b);
        s_state = ST_REFUSED;
        return 0;
    }

    /* the sampler: NEAREST and clamp */
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d->dev, &sci, NULL, &s_samp) != VK_SUCCESS) goto fail;

    /* the three layouts -- see mk_dsl's comment for why they are three */
    {
        static const int          bF[6] = { 32, 40, 41, 42, 43, 44 };
        static const VkDescriptorType tF[6] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
        static const VkShaderStageFlags sF[6] = {
            VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
            VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT };
        static const int          bC[4] = { 32, 33, 40, 41 };
        static const VkDescriptorType tC[4] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
        static const VkShaderStageFlags sC[4] = {
            VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
            VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT };
        static const int          bO[7] = { 0, 32, 40, 41, 42, 43, 44 };
        static const VkDescriptorType tO[7] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
        static const VkShaderStageFlags sO[7] = {
            VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
            VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
            VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT };
        /* MIP: the globals block at 32 and one sampler at 40, exactly as
           inc/spirv/tagpu_restore_glsl.spv.h records for MIP_FS. */
        static const int          bM[2] = { 32, 40 };
        static const VkDescriptorType tM[2] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
        static const VkShaderStageFlags sM[2] = {
            VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT };
        s_dslFill = mk_dsl(d, bF, tF, sF, 6);
        s_dslConv = mk_dsl(d, bC, tC, sC, 4);
        s_dslOut  = mk_dsl(d, bO, tO, sO, 7);
        s_dslMip  = mk_dsl(d, bM, tM, sM, 2);
    }
    if (!s_dslFill || !s_dslConv || !s_dslOut || !s_dslMip) goto fail;
    s_ploFill = mk_plo(d, s_dslFill);
    s_ploConv = mk_plo(d, s_dslConv);
    s_ploOut  = mk_plo(d, s_dslOut);
    s_ploMip  = mk_plo(d, s_dslMip);
    if (!s_ploFill || !s_ploConv || !s_ploOut || !s_ploMip) goto fail;

    for (i = 1; i <= nk; i++) if (!build_rp_act(d, i, nk)) goto fail;
    if (!build_rp_out(d)) goto fail;
    if (!build_rp_mip(d)) goto fail;

    vsFS   = mk_mod(d, tagpu_spv_tagpu_restore_glsl_FS_VS,
                    sizeof tagpu_spv_tagpu_restore_glsl_FS_VS / 4);
    vsOut  = mk_mod(d, tagpu_spv_tagpu_restore_glsl_OUT_VS,
                    sizeof tagpu_spv_tagpu_restore_glsl_OUT_VS / 4);
    fsFill = mk_mod(d, tagpu_spv_tagpu_restore_glsl_FILL_FS,
                    sizeof tagpu_spv_tagpu_restore_glsl_FILL_FS / 4);
    fsConv = mk_mod(d, convWords, convN);
    fsOut  = mk_mod(d, tagpu_spv_tagpu_restore_glsl_OUT_FS,
                    sizeof tagpu_spv_tagpu_restore_glsl_OUT_FS / 4);
    fsMip  = mk_mod(d, tagpu_spv_tagpu_restore_glsl_MIP_FS,
                    sizeof tagpu_spv_tagpu_restore_glsl_MIP_FS / 4);
    if (!vsFS || !vsOut || !fsFill || !fsConv || !fsOut || !fsMip) goto fail;

    /* FILL goes through rpAct[1]: one layer, and the pipeline's blend state
       still declares NK because that is the subpass's colour count */
    s_pipeFill = mk_pipe(d, vsFS, fsFill, s_rpAct[1], nk, s_ploFill, NULL);
    for (i = 1; i <= nk; i++)
        s_pipeConv[i] = mk_pipe(d, vsFS, fsConv, s_rpAct[i], nk, s_ploConv, NULL);
    /* OUT's vertices are the core's 8 floats: x,y | dx,dy,slotCol,slotRow | w,h,
       which is 2 + 4 + 2 at offsets 0, 8, 24 and stride 32 */
    memset(&vb, 0, sizeof vb); memset(va, 0, sizeof va);
    vb.binding = 0; vb.stride = 32; vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    va[0].location = 0; va[0].binding = 0; va[0].format = VK_FORMAT_R32G32_SFLOAT;       va[0].offset = 0;
    va[1].location = 1; va[1].binding = 0; va[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; va[1].offset = 8;
    va[2].location = 2; va[2].binding = 0; va[2].format = VK_FORMAT_R32G32_SFLOAT;       va[2].offset = 24;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = va;
    s_pipeOut = mk_pipe(d, vsOut, fsOut, s_rpOut, 1, s_ploOut, &vi);
    /* MIP shares the full-viewport triangle FILL and CONV use */
    s_pipeMip = mk_pipe(d, vsFS, fsMip, s_rpMip, 1, s_ploMip, NULL);
    if (!s_pipeFill || !s_pipeOut || !s_pipeMip) goto fail;
    for (i = 1; i <= nk; i++) if (!s_pipeConv[i]) goto fail;

    /* the weights, device-local, padded to WMAX mat4s so the tail binds */
    {
        VkDeviceSize padded = (VkDeviceSize)((size_t)w->ntex + (size_t)s_sched.wmax * 4) * 16;
        if (!mk_buffer(d, padded, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s_wbuf, &s_wmem, NULL)) goto fail;
        if (!mk_buffer(d, padded, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &s_wstage, &s_wstageMem, &s_wstageMap)) goto fail;
        memset(s_wstageMap, 0, (size_t)padded);
        memcpy(s_wstageMap, w->body, (size_t)w->ntex * 16);
        s_wbytes = padded;
        s_wUp = 0;                      /* copied on the first slice */
    }

    /* the scalar-uniform ring and the OUT vertices, PER SLOT: a slot's region
       is reused only when that slot has turned over under its own fence, which
       is what makes reuse safe without a second retire */
    s_gstride = s_dev.uboAlign > 16 ? s_dev.uboAlign : 16;
    if (!mk_buffer(d, s_gstride * GLOBALS_RING * TAGPU_VK_SLOTS,
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_gbuf, &s_gmem, &s_gmap)) goto fail;
    /* ONE BATCH'S WORTH, DEVICE-LOCAL AND NOT PER SLOT: the OUT vertices are
       written with vkCmdUpdateBuffer at the point of the draw, so what orders
       one batch's write against the previous batch's read is a barrier in the
       stream rather than a region nobody else is using -- see the OUT branch of
       `vk_draw`. */
    if (!mk_buffer(d, (VkDeviceSize)sizeof s_sched.verts,
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                   &s_vbuf, &s_vmem, NULL)) goto fail;

    /* the three slot tables and their staging */
    for (i = 0; i < 3; i++)
        if (!mk_image(d, TAGPU_R_SLOTCOLS, TAGPU_R_SLOTROWS, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &s_tabImg[i], &s_tabMem[i], &s_tabView[i])) goto fail;
    /* DEVICE-LOCAL AND UNMAPPED, for the reason the vertex buffer above is:
       the bytes are written by vkCmdUpdateBuffer at the point of the batch's
       own copy, so what orders one batch's write against the previous batch's
       read is a barrier in the stream. The per-SLOT offset is kept -- it costs
       3 KB a slot and it means the barrier only ever names this slot's region
       -- but it is not what makes the write safe. */
    if (!mk_buffer(d, (VkDeviceSize)TAGPU_R_BATCH * 16 * 3 * TAGPU_VK_SLOTS,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                   &s_tabStage, &s_tabStageMem, NULL)) goto fail;

    /* a descriptor pool big enough for every job's five sets */
    memset(psz, 0, sizeof psz);
    psz[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    /* five sets per job, plus one per level of a registered mip chain */
    psz[0].descriptorCount = TAGPU_R_MAXJOBS * (8 + TAGPU_VK_MAXMIP);
    psz[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    psz[1].descriptorCount = TAGPU_R_MAXJOBS * (32 + TAGPU_VK_MAXMIP);
    dpi.maxSets = TAGPU_R_MAXJOBS * (8 + TAGPU_VK_MAXMIP);
    dpi.poolSizeCount = 2; dpi.pPoolSizes = psz;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) goto fail;

    if (s_dev.timestamps) {
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 4;                  /* two pairs, one per parity */
        if (vkCreateQueryPool(d->dev, &qi, NULL, &s_qpool) != VK_SUCCESS) s_qpool = VK_NULL_HANDLE;
    }
    s_sched.timer = s_qpool ? 1 : 0;

    _snprintf(b, sizeof b, LANE ": built: %dx%d %s NK=%d, WMAX %d (%u KB bound per conv draw), "
                           "%d render passes, %s",
              w->depth, w->ch, opt->fp16 ? "fp16" : "fp32", nk, s_sched.wmax,
              (unsigned)(s_wrange >> 10), nk,
              s_qpool ? "timestamp budget" : "fixed slices");
    rlog(b);
    s_built = 1;
    ok = 1;
fail:
    if (vsFS)   vkDestroyShaderModule(d->dev, vsFS, NULL);
    if (vsOut)  vkDestroyShaderModule(d->dev, vsOut, NULL);
    if (fsFill) vkDestroyShaderModule(d->dev, fsFill, NULL);
    if (fsConv) vkDestroyShaderModule(d->dev, fsConv, NULL);
    if (fsOut)  vkDestroyShaderModule(d->dev, fsOut, NULL);
    if (fsMip)  vkDestroyShaderModule(d->dev, fsMip, NULL);
    if (!ok) rlog(LANE ": the shared resources could not be built - the lane stays indexed");
    return ok;
}

/* ============================= THE JOB ============================= */
struct TAGPU_VKRJOB {
    TAGPU_RCORE*   core;
    VkImage        srcImg;                  /* ...for the dump, not the draw   */
    VkImageView    srcView;                 /* the consumer's indexed atlas   */
    int            srcW, srcH;
    VkImage        dstImg;                  /* ...and its restored twin       */
    VkImageView    dstView;
    int            dstW, dstH;
    VkFramebuffer  dstFb;
    /* THE MIP CHAIN, and only for a consumer whose twin has one: `chainN` is 0
       everywhere else and every mip path below is then a single compare. The
       views are the CONSUMER's -- one attachment view and one sampled view per
       level, each naming exactly one level, which is what makes reading L-1
       while writing L sound: the source view CANNOT reach the level being
       written. */
    int            chainN;                  /* levels 1..chainN are reduced   */
    int            chainDim;                /* level 0's square size          */
    VkFramebuffer  chainFb[TAGPU_VK_MAXMIP];
    VkDescriptorSet chainSet[TAGPU_VK_MAXMIP];
    int            chainPainted;            /* `painted` at the last reduction*/
    int            chainDone;               /* it has run at least once       */
    VkImage        palImg;                  /* the palette snapshot, ours     */
    VkDeviceMemory palMem;
    VkImageView    palView;
    VkBuffer       palStage;
    VkDeviceMemory palStageMem;
    unsigned char* palMap;
    int            palDue;                  /* an upload is pending           */
    VkDescriptorSet setFill, setConv[2], setOut[2];
    unsigned       setGen;                  /* the s_actGen the sets name     */
    int            dstReady;                /* brought to SHADER_READ_ONLY    */
    int            clearDue;                /* ...and cleared to alpha 0      */
    /* 1 when the destination HOLDS SOMETHING this pass must not throw away --
       which is what picks `dst_ready`'s source layout. A repaint exists to
       recolour a restored atlas in place; transitioning it from UNDEFINED
       licenses the driver to discard every texel of it, so the repaint would
       blank exactly the world it exists to avoid blanking. Set from `repaint`
       at creation -- the consumer's statement that the atlas already holds a
       restore -- and then by `dst_ready` itself. */
    int            dstHas;
    /* THE BYTE DUMP (`dump_step`). It lives here rather than in each consumer
       because the destination, its size and the moment it is finished are all
       facts this file already holds -- and because the next consumer to be
       wired then gets the dump for free.
       0 nothing, 1 a copy is recorded and owed to `dumpSlot`, 2 written at
       `dumpPainted` frames. */
    char           tag[16];
    VkBuffer       dumpBuf;
    VkDeviceMemory dumpMem;
    unsigned char* dumpMap;
    VkDeviceSize   dumpBytes;
    int            dumpW, dumpH;
    uint32_t       dumpSlot;
    int            dumpState;
    int            dumpPainted;
    VkDeviceSize   dumpSrcOff, dumpSrcBytes;
};
static struct TAGPU_VKRJOB s_vjob[TAGPU_R_MAXJOBS];

/* one 16-byte block of scalar uniforms out of this slot's ring region */
static uint32_t g_alloc(const void* data, size_t bytes)
{
    VkDeviceSize off;
    if (s_gnext >= GLOBALS_RING || !s_gmap) {
        /* IT SAYS WHY. The callers return 0 from `draw`, which the core
           turns into `failed = 1` and a drop -- and the consumer then logs
           that the restore failed, with no reason anywhere else in the log.
           This is the one bound a fast enough device can reach:
           GLOBALS_RING blocks per slot per slice, one for a FILL, one for a
           CONV and two for an OUT, so a `full`-model batch spends 15 and a
           slice of more than about seventeen batches exhausts it.
           Said once per slice, because the slice that hit it will hit it
           again on the next draw and a per-draw line would bury the rest. */
        if (!s_gSaid) {
            char b[200];
            s_gSaid = 1;
            _snprintf(b, sizeof b, "%s: the slice wanted more than %d uniform blocks, "
                      "which is more batches in one slice than this ring carries - "
                      "the job fails here rather than binding the wrong weights",
                      LANE, GLOBALS_RING);
            rlog(b);
        }
        return 0xFFFFFFFFu;
    }
    off = ((VkDeviceSize)s_slot * GLOBALS_RING + s_gnext) * s_gstride;
    memset(s_gmap + off, 0, (size_t)s_gstride);
    memcpy(s_gmap + off, data, bytes);
    s_gnext++;
    return (uint32_t)off;
}

/* A FRESH GROUP OF FIVE, AND THE OLD ONES RETIRED -- never a rewrite in place.
   `vkUpdateDescriptorSets` on a set that a submitted command buffer has bound
   is undefined behaviour, and this one is bound by every frame still in flight:
   the activations are re-created whenever a batch needs a bigger slot geometry
   (five times in one measured run, 94 to 512 square), and each of those bumps
   `s_actGen`; rewriting every live job's five sets through `write_sets` then
   changes them while earlier frames are still executing against them.

   WHAT IT LOOKS LIKE, because this is the shape to recognise rather than the
   API rule to recite: the OUT pass reads `frag = c - net`, the palette colour
   minus the network's output, so a set whose `uAct` still named the array the
   FILL wrote gives net == c and paints the cell BLACK -- MEASURED as every
   texel of the earliest frames black, the source byte-identical, and the
   count varying run to run (165 136, then
   54 912, then 165 136 differing bytes) because it depends on which batches
   happen to straddle a grow.

   0 when no fresh group can be had, and the caller then leaves the job's sets
   naming the old generation -- which is wrong but stable, and the alternative
   is a set naming a destroyed image. The pool is sized for it: five per job
   plus the chain's, and a job holds at most one group at a time. */
static int set_group_new(const TAGPU_VKPASS* d, struct TAGPU_VKRJOB* g)
{
    VkDescriptorSetLayout lay[5];
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSet got[5];
    lay[0] = s_dslFill;
    lay[1] = s_dslConv; lay[2] = s_dslConv;
    lay[3] = s_dslOut;  lay[4] = s_dslOut;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = 5;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, got) != VK_SUCCESS) return 0;
    /* the group it replaces goes into the job retire, on the same mask as
       everything else a submitted command buffer can name */
    if (g->setFill) {
        JRETIRE* r = jret_take(d);
        r->set[0] = g->setFill; r->set[1] = g->setConv[0]; r->set[2] = g->setConv[1];
        r->set[3] = g->setOut[0]; r->set[4] = g->setOut[1];
        r->nset = 5;
        r->pending = d->slots >= 32 ? 0xFFFFFFFFu : ((1u << d->slots) - 1u);
    }
    g->setFill = got[0];
    g->setConv[0] = got[1]; g->setConv[1] = got[2];
    g->setOut[0] = got[3];  g->setOut[1] = got[4];
    return 1;
}

static void write_sets(const TAGPU_VKPASS* d, struct TAGPU_VKRJOB* g)
{
    VkDescriptorBufferInfo bi[2];
    VkDescriptorImageInfo  ii[6];
    VkWriteDescriptorSet   wr[8];
    int i, n;

    memset(bi, 0, sizeof bi); memset(ii, 0, sizeof ii);
    bi[0].buffer = s_gbuf; bi[0].offset = 0; bi[0].range = s_gstride;
    bi[1].buffer = s_wbuf; bi[1].offset = 0; bi[1].range = s_wrange;
    for (i = 0; i < 6; i++) ii[i].sampler = s_samp, ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

#define W_BUF(set, bind, which) do { \
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[n].dstSet = (set); \
        wr[n].dstBinding = (bind); wr[n].descriptorCount = 1; \
        wr[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; \
        wr[n].pBufferInfo = &bi[which]; n++; } while (0)
#define W_IMG(set, bind, slot_, view_, layout_) do { \
        ii[slot_].imageView = (view_); ii[slot_].imageLayout = (layout_); \
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[n].dstSet = (set); \
        wr[n].dstBinding = (bind); wr[n].descriptorCount = 1; \
        wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; \
        wr[n].pImageInfo = &ii[slot_]; n++; } while (0)

    /* FILL: 40 uAtlas, 41 uPal, 42 uRect, 43 uSrc, 44 uKey */
    memset(wr, 0, sizeof wr); n = 0;
    W_BUF(g->setFill, 32, 0);
    W_IMG(g->setFill, 40, 0, g->srcView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    W_IMG(g->setFill, 41, 1, g->palView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    W_IMG(g->setFill, 42, 2, s_tabView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    W_IMG(g->setFill, 43, 3, s_tabView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    W_IMG(g->setFill, 44, 4, s_tabView[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkUpdateDescriptorSets(d->dev, (uint32_t)n, wr, 0, NULL);

    for (i = 0; i < 2; i++) {
        /* CONV: 33 WBlock, 40 uAct (the side being READ), 41 uRect */
        memset(wr, 0, sizeof wr); n = 0;
        W_BUF(g->setConv[i], 32, 0);
        W_BUF(g->setConv[i], 33, 1);
        W_IMG(g->setConv[i], 40, 0, s_actArr[i], VK_IMAGE_LAYOUT_GENERAL);
        W_IMG(g->setConv[i], 41, 1, s_tabView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkUpdateDescriptorSets(d->dev, (uint32_t)n, wr, 0, NULL);
        /* OUT: 0 uDst (vertex), 40 uAct, 41 uAtlas, 42 uPal, 43 uRect, 44 uKey */
        memset(wr, 0, sizeof wr); n = 0;
        W_BUF(g->setOut[i], 0, 0);
        W_BUF(g->setOut[i], 32, 0);
        W_IMG(g->setOut[i], 40, 0, s_actArr[i], VK_IMAGE_LAYOUT_GENERAL);
        W_IMG(g->setOut[i], 41, 1, g->srcView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        W_IMG(g->setOut[i], 42, 2, g->palView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        W_IMG(g->setOut[i], 43, 3, s_tabView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        W_IMG(g->setOut[i], 44, 4, s_tabView[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkUpdateDescriptorSets(d->dev, (uint32_t)n, wr, 0, NULL);
    }
#undef W_BUF
#undef W_IMG
    g->setGen = s_actGen;
}

/* ====================== THE BACKEND'S TWELVE ====================== */

/* THE ACTIVATIONS, grown between batches only, and RETIRED rather than freed.
   The three answers are tagpu_restore_core.h's: 1 ready, 0 this device cannot,
   -1 a retire is still in flight and the core should ask again next slice. */
static int vk_act_ensure(int side)
{
    const TAGPU_RMODEL* w = tagpu_rcore_model();
    const TAGPU_VKPASS* d = s_d;
    VkImageMemoryBarrier mb[2];
    int i, l, layers;

    if (!d || !s_cb || !s_built) return 0;
    if (side > TAGPU_R_ACTMAX) side = TAGPU_R_ACTMAX;
    if (s_actImg[0] && s_actSide >= side) return 1;
    if (s_ret.pending) return -1;               /* the queue is untouched */
    layers = w->ch / 4;
    if (layers < 1 || layers > MAXLAYER) return 0;

    if (s_actImg[0]) {
        retire_take();
        s_ret.pending = d->slots >= 32 ? 0xFFFFFFFFu : ((1u << d->slots) - 1u);
    }
    for (i = 0; i < 2; i++) {
        if (!mk_image(d, side, side, layers, s_dev.actFmt,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &s_actImg[i], &s_actMem[i], &s_actArr[i])) {
            rlog(LANE ": the activation arrays could not be allocated");
            return 0;
        }
        for (l = 0; l < layers; l++)
            if (!layer_view(d, s_actImg[i], s_dev.actFmt, l, &s_actLay[i][l])) {
                rlog(LANE ": an activation layer view could not be created");
                return 0;
            }
    }
    s_actSide = side; s_actLayers = layers; s_actGen++;

    /* UNDEFINED -> GENERAL, once, and they never leave it */
    memset(mb, 0, sizeof mb);
    for (i = 0; i < 2; i++) {
        mb[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        mb[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        mb[i].srcQueueFamilyIndex = mb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb[i].image = s_actImg[i];
        mb[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mb[i].subresourceRange.levelCount = 1;
        mb[i].subresourceRange.layerCount = (uint32_t)layers;
        mb[i].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 2, mb);
    if (tagpu_rcore_opt()->log) {
        char b[160];
        _snprintf(b, sizeof b, LANE ": activations %dx%d x %d layers (%d MB)", side, side, layers,
                  (int)(2u * (unsigned)side * (unsigned)side * (unsigned)layers *
                        (tagpu_rcore_opt()->fp16 ? 8u : 16u) >> 20));
        rlog(b);
    }
    return 1;
}

/* THE IDLE RELEASE GOES THROUGH THE RETIRE, not through a free.
   The core asks after 180 slices without work, and the images may still be
   named by a submitted command buffer -- so freeing them here would be a
   use-after-free decided by a clock. Handing them to the retire instead
   gives the same ~100 MB back, a few frames later, licensed by the slot
   bitmask reaching zero. */
static int vk_act_free(void)
{
    if (!s_d || !s_actImg[0]) return 0;
    if (s_ret.pending) return 0;            /* one retire at a time */
    retire_take();
    s_ret.pending = s_d->slots >= 32 ? 0xFFFFFFFFu : ((1u << s_d->slots) - 1u);
    s_actSide = 0; s_actLayers = 0; s_actGen++;
    return 1;
}

static int vk_ready(void) { return s_built && s_state == ST_READY; }
static int vk_may_draw(void) { return tagpu_classicpp_assets(); }
static void vk_state_push(unsigned slice) { (void)slice; }
static void vk_state_pop(unsigned slice) { (void)slice; }

static void vk_slice_begin(int q)
{
    if (!s_qpool || !s_cb) return;
    vkCmdResetQueryPool(s_cb, s_qpool, (uint32_t)(q * 2), 2);
    vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_qpool, (uint32_t)(q * 2));
}

static void vk_slice_end(int q)
{
    if (!s_qpool || !s_cb) return;
    vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_qpool, (uint32_t)(q * 2 + 1));
}

/* 1 = `*ns` is that slice's GPU time, 0 = not in yet. Asked WITHOUT
   VK_QUERY_RESULT_WAIT_BIT, so this never blocks -- the core's double buffering
   is what makes that safe. */
static int vk_timer_poll(int q, double* ns)
{
    uint64_t t[2] = { 0, 0 };
    if (!s_qpool || !s_d) return 0;
    if (vkGetQueryPoolResults(s_d->dev, s_qpool, (uint32_t)(q * 2), 2, sizeof t, t,
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    if (t[1] <= t[0]) return 0;
    *ns = (double)(t[1] - t[0]) * s_dev.tsPeriod;
    return 1;
}

static void vk_timer_off(void) { /* the pool stays; the core stops asking */ }

/* The three per-frame parameter tables onto the device, at each batch's FILL:
   the destination rect, the source rect, and the KEY AND WRAP of every frame in
   the batch.

   THE BYTES GO INTO THE COMMAND STREAM, NOT INTO A PER-SLOT STAGING REGION.
   "One batch is ever in flight, so nothing else writes them in between" is
   true of the DEVICE and false of the RECORDING: a slice can issue more than
   one batch (tagpu_rcore_step re-picks at every batch boundary and runs until
   the GPU-time budget is spent), and two batches' `memcpy` into one region
   land on the same address before either `vkCmdCopyBufferToImage` has
   executed -- so the first batch's frames would be restored through the
   SECOND batch's tables: wrong source rects, and wrong colour keys.

   WHAT THAT LOOKS LIKE: a frame whose key came from another frame's table has
   no keyed texel where it should have one, so the OUT pass writes the key's
   own palette colour -- opaque (84, 84, 252) -- where a correct restore writes
   (0, 0, 0, 0). MEASURED against the OpenGL renderer this replaced, on 118 of
   1304 feature frames and 3 of 167 effects frames, every other frame
   byte-identical. THE TERRAIN CANNOT REVEAL IT:
   10 036 tiles of one size with no colour key at all, so a swapped table
   costs a source rect and nothing else. It takes a consumer whose frames have
   DIFFERENT SIZES AND A KEY.

   A BIGGER ARENA IS NOT THE FIX: batches per slice is a time budget rather
   than a count, so any arena is a number that can be exceeded and what it
   buys is this failure again. 3 KB at most, well inside the 65 536
   vkCmdUpdateBuffer allows.

   THE IMAGES THEMSELVES ARE ORDERED -- the write-after-read barrier below
   runs before each batch's copy and names the previous batch's shader reads.
   The buffer barrier is the same shape for the buffer: TRANSFER -> TRANSFER,
   because the previous batch's read of these bytes is a copy and not a
   draw. */
static void upload_tables(const TAGPU_VKPASS* d, const TAGPU_RDRAWREQ* r)
{
    const float* src[3];
    VkImageMemoryBarrier mb[3];
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    VkBufferImageCopy bc;
    const size_t bytes = (size_t)TAGPU_R_BATCH * 16;
    VkDeviceSize base = (VkDeviceSize)s_slot * bytes * 3;
    int i;

    /* the previous batch's copy out of this region is a fact before this
       batch's bytes land on it */
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = s_tabStage;
    bb.offset = base;
    bb.size = (VkDeviceSize)bytes * 3;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    src[0] = r->rect; src[1] = r->src; src[2] = r->key;
    for (i = 0; i < 3; i++)
        if (src[i]) vkCmdUpdateBuffer(s_cb, s_tabStage, base + (VkDeviceSize)i * bytes,
                                      (VkDeviceSize)bytes, src[i]);
    /* ...and this batch's write is a fact before its own copy reads it */
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);

    memset(mb, 0, sizeof mb);
    for (i = 0; i < 3; i++) {
        mb[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb[i].oldLayout = s_tabInit ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        mb[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mb[i].srcQueueFamilyIndex = mb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb[i].image = s_tabImg[i];
        mb[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mb[i].subresourceRange.levelCount = 1;
        mb[i].subresourceRange.layerCount = 1;
        mb[i].srcAccessMask = s_tabInit ? VK_ACCESS_SHADER_READ_BIT : 0;
        mb[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 3, mb);
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = TAGPU_R_SLOTCOLS;
    bc.imageExtent.height = TAGPU_R_SLOTROWS;
    bc.imageExtent.depth = 1;
    for (i = 0; i < 3; i++) {
        bc.bufferOffset = base + (VkDeviceSize)i * bytes;
        vkCmdCopyBufferToImage(s_cb, s_tabStage, s_tabImg[i],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    }
    for (i = 0; i < 3; i++) {
        mb[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mb[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 3, mb);
    s_tabInit = 1;
    (void)d;
}

/* A job's destination, brought to the layout the OUT render pass expects and
   cleared to alpha 0 unless this is a repaint in place. `dstHas` is the source
   layout: UNDEFINED for an image whose contents are ours to throw away (which
   is cheaper -- the driver may skip a decompress), SHADER_READ_ONLY_OPTIMAL
   for one a repaint is about to draw over. See the field. */
static void dst_ready(const TAGPU_VKPASS* d, struct TAGPU_VKRJOB* g)
{
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkClearColorValue cc;
    VkImageSubresourceRange rg;
    if (g->dstReady) return;
    memset(&cc, 0, sizeof cc);
    memset(&rg, 0, sizeof rg);
    rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg.levelCount = 1; rg.layerCount = 1;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = g->dstImg;
    mb.subresourceRange = rg;
    mb.oldLayout = g->dstHas ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                             : VK_IMAGE_LAYOUT_UNDEFINED;
    mb.srcAccessMask = g->dstHas ? VK_ACCESS_SHADER_READ_BIT : 0;
    mb.newLayout = g->clearDue ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    mb.dstAccessMask = g->clearDue ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s_cb,
                         g->dstHas ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         g->clearDue ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &mb);
    if (g->clearDue) {
        vkCmdClearColorImage(s_cb, g->dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rg);
        mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &mb);
        g->clearDue = 0;
    }
    g->dstReady = 1;
    g->dstHas = 1;                         /* from here on there is something  */
    (void)d;
}

static void set_vp(int w, int h)
{
    VkViewport vp;
    VkRect2D   sc;
    memset(&vp, 0, sizeof vp); memset(&sc, 0, sizeof sc);
    /* POSITIVE HEIGHT, and the file header says why: these shaders speak image
       space, where NDC -1 is row 0. */
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)w; vp.height = (float)h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    sc.extent.width = (uint32_t)w; sc.extent.height = (uint32_t)h;
    vkCmdSetViewport(s_cb, 0, 1, &vp);
    vkCmdSetScissor(s_cb, 0, 1, &sc);
}

/* THE SLICE'S ORDERING BARRIER, and it is the whole argument for sharing the
   activations and the tables rather than copying them per slot.

   `vkCmdPipelineBarrier`'s first synchronisation scope includes every command
   submitted EARLIER IN SUBMISSION ORDER on this queue -- not merely earlier in
   this command buffer -- so one barrier here makes the previous slice's reads
   of the activations a fact before this slice's writes to them. That is a
   fence, not a hope: nothing about it depends on how long a slice took or on
   how many frames are in flight.

   It is emitted on the slice's FIRST DRAW rather than at the top of `step`,
   because `step` runs every frame including the ones with nothing queued, and
   a barrier per idle frame is a cost for nobody. */
static void slice_head(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    if (!s_wUp && s_wbuf && s_wstage) {
        VkBufferCopy bc;
        memset(&bc, 0, sizeof bc);
        bc.size = s_wbytes;
        vkCmdCopyBuffer(s_cb, s_wstage, s_wbuf, 1, &bc);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
        vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        s_wUp = 1;
    }
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = mb.srcAccessMask;
    vkCmdPipelineBarrier(s_cb,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* the job's palette snapshot onto the device, before the FILL that samples it */
static void upload_pal(struct TAGPU_VKRJOB* g)
{
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy bc;
    if (!g->palDue || !g->palStage) return;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = g->palImg;
    mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.srcAccessMask = 0;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &mb);
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = 256; bc.imageExtent.height = 1; bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, g->palStage, g->palImg,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &mb);
    g->palDue = 0;
}

static int vk_draw(const TAGPU_RDRAWREQ* r)
{
    struct TAGPU_VKRJOB* g = (struct TAGPU_VKRJOB*)r->job->owner;
    const TAGPU_VKPASS* d = s_d;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    uint32_t dyn[2];
    int gi[4];

    if (!d || !s_cb || !g || !s_built) return 0;
    if (!s_sliceOpen) { slice_head(); s_sliceOpen = 1; }
    /* A GENERATION CHANGE MEANS NEW SETS, not a rewrite of the bound ones --
       see `set_group_new`. `setGen` 0 is the first draw, where the sets were
       allocated by `job_new` and have never been bound. */
    if (g->setGen != s_actGen) {
        if (g->setGen != 0 && !set_group_new(d, g)) {
            rlog(LANE ": no descriptor sets for a re-created activation array - "
                       "this batch is dropped rather than bound to the old one");
            return 0;
        }
        write_sets(d, g);
    }

    if (r->kind == TAGPU_RDRAW_FILL) {
        VkFramebuffer fb;
        upload_pal(g);
        upload_tables(d, r);
        fb = fb_for(d, 0, 0, 1);
        if (!fb) { rlog(LANE ": no framebuffer for the fill target"); return 0; }
        gi[0] = r->S; gi[1] = tagpu_rcore_model()->depth;      /* uSlot, uKeyR */
        dyn[0] = g_alloc(gi, 8);
        if (dyn[0] == 0xFFFFFFFFu) return 0;
        rbi.renderPass = s_rpAct[1]; rbi.framebuffer = fb;
        rbi.renderArea.extent.width = (uint32_t)r->TW;
        rbi.renderArea.extent.height = (uint32_t)r->TH;
        vkCmdBeginRenderPass(s_cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        set_vp(r->TW, r->TH);
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeFill);
        vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ploFill, 0, 1,
                                &g->setFill, 1, dyn);
        vkCmdDraw(s_cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(s_cb);
        return 1;
    }

    if (r->kind == TAGPU_RDRAW_CONV) {
        VkFramebuffer fb = fb_for(d, 1 - r->srcAct, r->group, r->n);
        if (!fb || !s_rpAct[r->n] || !s_pipeConv[r->n]) {
            rlog(LANE ": no render pass for this conv group shape");
            return 0;
        }
        gi[0] = (int)r->L->jin; gi[1] = (int)(r->L->kstride / 4);
        gi[2] = r->S;           gi[3] = r->relu;
        dyn[0] = g_alloc(gi, 16);
        if (dyn[0] == 0xFFFFFFFFu) return 0;
        /* THE WEIGHT WINDOW, and its alignment is a loader bound rather than a
           check here: `kstride & 15` makes the group term a multiple of 256 and
           `offset & 15` the base term, so this is always a multiple of the
           device's minUniformBufferOffsetAlignment. */
        dyn[1] = (uint32_t)(((size_t)r->L->offset + (size_t)r->group * r->L->kstride) * 16);
        rbi.renderPass = s_rpAct[r->n]; rbi.framebuffer = fb;
        rbi.renderArea.extent.width = (uint32_t)r->TW;
        rbi.renderArea.extent.height = (uint32_t)r->TH;
        vkCmdBeginRenderPass(s_cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        set_vp(r->TW, r->TH);
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeConv[r->n]);
        vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ploConv, 0, 1,
                                &g->setConv[r->srcAct], 2, dyn);
        vkCmdDraw(s_cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(s_cb);
        return 1;
    }

    /* OUT: straight into the consumer's atlas, in the consumer's cell layout */
    {
        float uDst[4];
        VkDeviceSize voff = 0;
        VkMemoryBarrier vb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        dst_ready(d, g);
        if (!g->dstFb) { rlog(LANE ": the job has no destination framebuffer"); return 0; }
        if (r->nv <= 0) return 1;                     /* an empty batch draws nothing */
        /* THE VERTICES GO INTO THE COMMAND STREAM, not into a host-mapped
           region indexed by the FRAME SLOT: A SLICE CAN ISSUE MORE THAN ONE
           BATCH -- the loop in tagpu_rcore_step re-picks at every batch
           boundary and runs until the GPU-time budget is spent, and the
           terrain's own log shows batches 157 and 158 both issued at slice 917.
           Two OUT draws in one slice would write the same address, so the
           second batch's 36 vertices land on top of the first's before either
           draw executes: the first batch's leading six cells are never painted
           and the second's six are painted twice. MEASURED as six cells of
           10 036, every other one byte-identical to the OpenGL renderer this
           replaced -- silent, and
           exactly the shape of failure this stack produces.
           A BIGGER ARENA WOULD NOT BE THE FIX. Batches per slice is bounded by
           a time budget and not by a count, so any arena is a number that can
           be exceeded, and the failure it buys is this one again. vkCmdUpdateBuffer
           records the data AT THIS POINT IN THE STREAM, so each batch carries its
           own copy and the ordering is the command buffer's own -- an ordering,
           which is what CLAUDE.md asks a fix to rest on. 12 288 bytes at most,
           well inside the 65 536 the command allows.
           The barrier is BOTH WAYS on purpose: TRANSFER -> VERTEX_INPUT makes
           this batch's vertices visible to this batch's draw, and
           VERTEX_INPUT -> TRANSFER makes the PREVIOUS batch's read of the same
           bytes a fact before this write lands on them. */
        vkCmdUpdateBuffer(s_cb, s_vbuf, 0, (VkDeviceSize)r->nv * 32, r->verts);
        vb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vb.dstAccessMask = vb.srcAccessMask;
        vkCmdPipelineBarrier(s_cb,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             0, 1, &vb, 0, NULL, 0, NULL);
        uDst[0] = (float)g->dstW; uDst[1] = (float)g->dstH; uDst[2] = uDst[3] = 0.0f;
        dyn[0] = g_alloc(uDst, 8);                    /* binding 0, the vertex uDst */
        gi[0] = r->S;
        dyn[1] = g_alloc(gi, 4);                      /* binding 32, the frag uSlot */
        if (dyn[0] == 0xFFFFFFFFu || dyn[1] == 0xFFFFFFFFu) return 0;
        rbi.renderPass = s_rpOut; rbi.framebuffer = g->dstFb;
        rbi.renderArea.extent.width = (uint32_t)g->dstW;
        rbi.renderArea.extent.height = (uint32_t)g->dstH;
        vkCmdBeginRenderPass(s_cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        set_vp(g->dstW, g->dstH);
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeOut);
        vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ploOut, 0, 1,
                                &g->setOut[r->srcAct], 2, dyn);
        vkCmdBindVertexBuffers(s_cb, 0, 1, &s_vbuf, &voff);
        vkCmdDraw(s_cb, (uint32_t)r->nv, 1, 0, 0);
        vkCmdEndRenderPass(s_cb);
    }
    return 1;
}

static const TAGPU_RBACKEND s_be = {
    LANE,
    vk_ready,
    vk_act_ensure,
    vk_act_free,
    vk_draw,
    vk_slice_begin,
    vk_slice_end,
    vk_timer_poll,
    vk_timer_off,
    vk_state_push,
    vk_state_pop,
    vk_may_draw
};

/* ======================== THE PUBLIC JOB API ======================== */

static void pal_pack(unsigned char* out, const unsigned char* pal)
{
    int i;
    for (i = 0; i < 256; i++) {
        out[i * 4] = pal[i * 4]; out[i * 4 + 1] = pal[i * 4 + 1];
        out[i * 4 + 2] = pal[i * 4 + 2]; out[i * 4 + 3] = 255;
    }
}

TAGPU_VKRJOB* tagpu_vk_restore_job_new(const TAGPU_VKPASS* d, const char* tag,
                                       int prio, int oneshot, int repaint,
                                       VkImage srcImg, VkImageView srcView,
                                       int srcW, int srcH,
                                       const unsigned char* pal,
                                       VkImage dstImg, VkImageView dstView,
                                       int dstW, int dstH)
{
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkDescriptorSetLayout lay[5];
    VkDescriptorSet got[5];
    TAGPU_RCORE* c;
    struct TAGPU_VKRJOB* g;
    char b[220];

    if (!d || !srcView || !dstImg || !dstView || !pal ||
        srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return NULL;
    if (!build_shared(d)) return NULL;
    c = tagpu_rcore_job_new(&s_sched, tag, prio, oneshot, NULL);
    if (!c) return NULL;
    g = &s_vjob[(int)(c - s_sched.jobs)];
    memset(g, 0, sizeof *g);
    c->owner = g; g->core = c;
    g->srcImg = srcImg; g->srcView = srcView; g->srcW = srcW; g->srcH = srcH;
    g->dstImg = dstImg; g->dstView = dstView; g->dstW = dstW; g->dstH = dstH;
    g->clearDue = repaint ? 0 : 1;
    g->dstHas   = repaint ? 1 : 0;
    if (tag) { strncpy(g->tag, tag, sizeof g->tag - 1); g->tag[sizeof g->tag - 1] = 0; }
    else     { strcpy(g->tag, "restore"); }

    /* the palette snapshot: R,G,B,pad -> RGBA8, uploaded before the first FILL */
    if (!mk_image(d, 256, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  &g->palImg, &g->palMem, &g->palView)) goto fail;
    if (!mk_buffer(d, 256 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &g->palStage, &g->palStageMem, &g->palMap)) goto fail;
    pal_pack(g->palMap, pal);
    g->palDue = 1;

    /* the OUT target's framebuffer, over the consumer's own view */
    fci.renderPass = s_rpOut;
    fci.attachmentCount = 1;
    fci.pAttachments = &g->dstView;
    fci.width = (uint32_t)dstW; fci.height = (uint32_t)dstH; fci.layers = 1;
    if (vkCreateFramebuffer(d->dev, &fci, NULL, &g->dstFb) != VK_SUCCESS) goto fail;

    /* five sets: FILL, and CONV/OUT once per ping-pong side */
    lay[0] = s_dslFill;
    lay[1] = s_dslConv; lay[2] = s_dslConv;
    lay[3] = s_dslOut;  lay[4] = s_dslOut;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = 5;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, got) != VK_SUCCESS) goto fail;
    g->setFill = got[0];
    g->setConv[0] = got[1]; g->setConv[1] = got[2];
    g->setOut[0] = got[3];  g->setOut[1] = got[4];
    /* written on the first draw: the activations may not exist yet, and a set
       naming VK_NULL_HANDLE is not a set that can be bound */
    g->setGen = 0;

    if (!oneshot) {
        _snprintf(b, sizeof b, LANE ": %s: lazy restore armed (%dx%d twin of the %dx%d atlas)",
                  c->tag, dstW, dstH, srcW, srcH);
        rlog(b);
    }
    return g;
fail:
    tagpu_vk_restore_job_free(d, g);
    return NULL;
}

int tagpu_vk_restore_job_chain(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j,
                               int mips, int dim,
                               const VkImageView* attach, const VkImageView* sample)
{
    VkDescriptorSetLayout lay[TAGPU_VK_MAXMIP];
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    int L;

    if (!d || !d->dev || !j || !j->core || !j->core->used) return 0;
    if (mips < 1 || mips > TAGPU_VK_MAXMIP || dim <= 0 || !attach || !sample) return 0;
    if (!s_built || !s_pipeMip || !s_rpMip || !s_dslMip || !s_dpool) return 0;
    if (j->chainN) return 1;                  /* registered once, by contract */
    /* AN ODD LEVEL WOULD NEED A WEIGHTED THREE-TAP, not a 2x2 average, and
       this shader does not pretend to be one. The rule is a property of the
       2x2 reduction and not of the backend. Refused WHOLE rather than
       part-reduced: a chain half ours and half nobody's is the one outcome
       neither lane can describe. */
    for (L = 1; L <= mips; L++)
        if ((dim >> L) < 1 || ((dim >> (L - 1)) & 1)) {
            char b[160];
            _snprintf(b, sizeof b, "%s: %s: a level of the %d-square chain is odd - "
                      "no reduction here", LANE, j->core->tag, dim);
            rlog(b);
            return 0;
        }
    for (L = 0; L < mips; L++) {
        if (!attach[L] || !sample[L]) return 0;
        lay[L] = s_dslMip;
    }
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = (uint32_t)mips;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, j->chainSet) != VK_SUCCESS) {
        char b[160];
        memset(j->chainSet, 0, sizeof j->chainSet);
        _snprintf(b, sizeof b, "%s: %s: no descriptor sets for the mip chain - "
                  "the twin keeps level 0 only", LANE, j->core->tag);
        rlog(b);
        return 0;
    }
    fci.renderPass = s_rpMip;
    fci.attachmentCount = 1;
    fci.layers = 1;
    for (L = 0; L < mips; L++) {
        int dst = dim >> (L + 1);
        fci.pAttachments = &attach[L];
        fci.width = (uint32_t)dst; fci.height = (uint32_t)dst;
        if (vkCreateFramebuffer(d->dev, &fci, NULL, &j->chainFb[L]) != VK_SUCCESS) {
            int k;
            for (k = 0; k < L; k++) vkDestroyFramebuffer(d->dev, j->chainFb[k], NULL);
            memset(j->chainFb, 0, sizeof j->chainFb);
            vkFreeDescriptorSets(d->dev, s_dpool, (uint32_t)mips, j->chainSet);
            memset(j->chainSet, 0, sizeof j->chainSet);
            {   char b[160];
                _snprintf(b, sizeof b, "%s: %s: no framebuffer for mip level %d - "
                          "the twin keeps level 0 only", LANE, j->core->tag, L + 1);
                rlog(b); }
            return 0;
        }
    }
    /* the sets are written ONCE, here: the source view of level L is fixed for
       the life of the job, and the globals block is bound by dynamic offset at
       record time rather than written into the set */
    {
        VkDescriptorBufferInfo bi;
        VkDescriptorImageInfo  ii;
        VkWriteDescriptorSet   wr[2];
        memset(&bi, 0, sizeof bi); memset(&ii, 0, sizeof ii);
        bi.buffer = s_gbuf; bi.offset = 0; bi.range = 16;
        ii.sampler = s_samp;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for (L = 0; L < mips; L++) {
            memset(wr, 0, sizeof wr);
            ii.imageView = sample[L];
            wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[0].dstSet = j->chainSet[L]; wr[0].dstBinding = 32;
            wr[0].descriptorCount = 1;
            wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            wr[0].pBufferInfo = &bi;
            wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[1].dstSet = j->chainSet[L]; wr[1].dstBinding = 40;
            wr[1].descriptorCount = 1;
            wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[1].pImageInfo = &ii;
            vkUpdateDescriptorSets(d->dev, 2, wr, 0, NULL);
        }
    }
    j->chainN = mips;
    j->chainDim = dim;
    j->chainPainted = 0;
    j->chainDone = 0;
    {   char b[180];
        _snprintf(b, sizeof b, "%s: %s: the twin's %d mip level(s) are reduced HERE, "
                  "by the integer (sum+1)/4 box",
                  LANE, j->core->tag, mips);
        rlog(b); }
    return 1;
}

int tagpu_vk_restore_job_add(TAGPU_VKRJOB* j, const TAGPU_RGLSL_FRAME* frames, int count)
{
    if (!j || !j->core) return 0;
    return tagpu_rcore_job_add(&s_sched, j->core, frames, count);
}

int tagpu_vk_restore_job_idle(const TAGPU_VKRJOB* j)
{
    return !j || !j->core || !j->core->used || (j->core->qn == 0 && !j->core->inflight);
}

int tagpu_vk_restore_job_failed(const TAGPU_VKRJOB* j)
{
    return j && j->core && j->core->used && j->core->failed;
}

int tagpu_vk_restore_job_painted(const TAGPU_VKRJOB* j)
{
    return (j && j->core && j->core->used) ? j->core->tframes : 0;
}

/* below, beside the rest of the dump */
static void dump_free(const TAGPU_VKPASS* d, struct TAGPU_VKRJOB* g);

void tagpu_vk_restore_job_free(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j)
{
    if (!j) return;
    if (d && d->dev) {
        /* INTO THE RETIRE, NOT DESTROYED HERE. Every one of these objects can
           still be named by a submitted command buffer -- see JRETIRE. The
           palette's MAPPING goes now, because unmapping is not a device
           operation and the memory it belongs to is kept until the mask
           clears. */
        JRETIRE* r = jret_take(d);
        /* THE DUMP'S STAGING GOES INTO THE RETIRE WITH EVERYTHING ELSE. A copy
           into it may have been recorded into the command buffer of the slot
           that took the dump, and this function runs whenever a consumer's
           generation moves -- which is any frame, not that slot's next one.
           Both MAPPINGS go now, because unmapping is not a device operation
           and the memory they belong to is kept until the mask clears. */
        if (j->dumpMap && j->dumpMem) vkUnmapMemory(d->dev, j->dumpMem);
        r->buf = j->dumpBuf; r->bufMem = j->dumpMem;
        j->dumpBuf = VK_NULL_HANDLE; j->dumpMem = VK_NULL_HANDLE; j->dumpMap = NULL;
        if (j->palMap && j->palStageMem) vkUnmapMemory(d->dev, j->palStageMem);
        r->fb[0] = j->dstFb; r->nfb = 1;
        r->palImg = j->palImg; r->palMem = j->palMem; r->palView = j->palView;
        r->palStage = j->palStage; r->palStageMem = j->palStageMem;
        if (j->setFill && s_dpool) {
            r->set[0] = j->setFill; r->set[1] = j->setConv[0]; r->set[2] = j->setConv[1];
            r->set[3] = j->setOut[0]; r->set[4] = j->setOut[1];
            r->nset = 5;
        }
        /* AND THE CHAIN'S, which is why the retire carries arrays: a reduction
           recorded into some slot's command buffer names both the framebuffer
           and the set, and they die on the same mask as the rest. The VIEWS in
           them are the consumer's and are not retired here -- the contract says
           the consumer keeps them until after this call. */
        {
            int L;
            for (L = 0; L < j->chainN && r->nfb < 1 + TAGPU_VK_MAXMIP; L++)
                if (j->chainFb[L]) r->fb[r->nfb++] = j->chainFb[L];
            if (s_dpool)
                for (L = 0; L < j->chainN && r->nset < 5 + TAGPU_VK_MAXMIP; L++)
                    if (j->chainSet[L]) r->set[r->nset++] = j->chainSet[L];
        }
        r->pending = d->slots >= 32 ? 0xFFFFFFFFu : ((1u << d->slots) - 1u);
    }
    if (j->core) tagpu_rcore_job_free(&s_sched, j->core);
    /* with no device there is nothing to destroy and nothing still executing:
       the memset below forgets the handles, which is what `lost` does too */
    memset(j, 0, sizeof *j);
}

/* ---- the byte dump ---------------------------------------------------- */

/* Give a dump's staging back. Safe whenever the copy into it is not in flight,
   which every caller proves a different way: `dump_step` is past this slot's
   own fence, `job_free` is called between frames on a job whose copy was owed
   to a slot that has since come round, and `down` is behind the seam's
   vkDeviceWaitIdle. */
static void dump_free(const TAGPU_VKPASS* d, struct TAGPU_VKRJOB* g)
{
    if (g->dumpMap && g->dumpMem) vkUnmapMemory(d->dev, g->dumpMem);
    if (g->dumpBuf) vkDestroyBuffer(d->dev, g->dumpBuf, NULL);
    if (g->dumpMem) vkFreeMemory(d->dev, g->dumpMem, NULL);
    g->dumpMap = NULL; g->dumpBuf = VK_NULL_HANDLE; g->dumpMem = VK_NULL_HANDLE;
    g->dumpBytes = 0; g->dumpW = g->dumpH = 0;
}

/* The byte dump, from `step` and outside any render pass: collect a copy
   this slot owes us, or record one.

   THIS DUMP HAS NOTHING TO BE COMPARED AGAINST WITHIN A RUN: nothing else in
   the process writes a second copy. It is useful against a file kept from an
   older build.
   (`tagpu_terr.c`'s dump is under the same `tagpu_restoredump.on`.) A
   mirrored restore shows as every cell's rows reversed and a dropped batch as
   whole cells of alpha 0.

   AND IT DOES NOT BLOCK THE DEVICE. The copy is recorded into this frame's
   command buffer and read at THIS SLOT'S NEXT step, which is the one instant
   the seam's fence has proved the submit carrying it completed -- the same
   argument the retire already makes, so it costs no new reasoning.

   RE-ARMED ON THE PAINT COUNT, not on a serial, because a GAF atlas's job is
   a lazy queue that keeps painting: a dump is owed again whenever the picture
   has moved since the last one, and a settled scene rewrites nothing. */
static int dump_step(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                     struct TAGPU_VKRJOB* g)
{
    char name[64];
    char b[220];
    int painted;

    if (!g->core || !g->core->used) return 0;

    /* THE COLLECTION FIRST, and under this slot's own fence. */
    if (g->dumpState == 1 && slot == g->dumpSlot) {
        FILE* f;
        /* THE DESTINATION'S OWN BYTES, NOT THE WHOLE BUFFER. The source rides
           in the same allocation after it, and writing `dumpBytes` here would
           put it on the end of the destination file, so every pair would
           differ on SIZE. */
        size_t n = (size_t)(g->dumpBytes - g->dumpSrcBytes);
        /* A CHAIN DUMPS AS `.mips`, LEVEL 0 ALONE AS `.rgba`, and the name is
           what tells them apart, so two builds' dumps of the same case compare
           with one `cmp` of two whole files. */
        _snprintf(name, sizeof name, "tagpu_restore_%s_vk.%s", g->tag,
                  g->chainN > 0 ? "mips" : "rgba");
        name[sizeof name - 1] = 0;
        f = g->dumpMap ? fopen(name, "wb") : NULL;
        if (f) {
            size_t w = fwrite(g->dumpMap, 1, n, f);
            fclose(f);
            _snprintf(b, sizeof b, LANE ": %s: restored atlas dumped to %s (%dx%d RGBA"
                      "%s, %u bytes, %d frames)%s", g->tag, name, g->dumpW, g->dumpH,
                      g->chainN > 0 ? " + its mip chain" : "",
                      (unsigned)n, g->dumpPainted, w == n ? "" : " - SHORT WRITE");
        } else {
            _snprintf(b, sizeof b, LANE ": %s: %s would not open - nothing written",
                      g->tag, name);
        }
        b[sizeof b - 1] = 0;
        rlog(b);
        if (g->dumpSrcBytes && g->dumpMap) {
            char sn[64];
            FILE* sf;
            _snprintf(sn, sizeof sn, "tagpu_restore_%s_vk.r8", g->tag);
            sn[sizeof sn - 1] = 0;
            sf = fopen(sn, "wb");
            if (sf) {
                fwrite(g->dumpMap + (size_t)g->dumpSrcOff, 1, (size_t)g->dumpSrcBytes, sf);
                fclose(sf);
            }
        }
        g->dumpState = 2;
        dump_free(d, g);
        return 1;                          /* one action a frame, and it is done */
    }
    painted = g->core->tframes;
    if (g->dumpState == 2 && painted != g->dumpPainted) g->dumpState = 0;
    if (g->dumpState != 0) return 0;
    if (GetFileAttributesA("tagpu_restoredump.on") == INVALID_FILE_ATTRIBUTES) return 0;
    /* THE PICTURE HAS TO BE FINISHED, and it has to exist. `qn`/`inflight` are
       the core's own "every frame added is painted"; taking the dump before
       that would compare a slice count rather than two restorers. A job that
       FAILED is not dumped at all: what it painted is a fragment, and calling
       that the lane's answer is the kind of measurement that reads as a result.
       One painted frame is also what proves the destination is in
       SHADER_READ_ONLY -- an OUT render pass has run and left it there, which
       is the layout the copy borrows and gives back. */
    if (g->core->failed || g->core->qn != 0 || g->core->inflight) return 0;
    if (painted < 1 || !g->dstImg || g->dstW <= 0 || g->dstH <= 0) return 0;

    g->dumpW = g->dstW; g->dumpH = g->dstH;
    g->dumpBytes = (VkDeviceSize)g->dumpW * (VkDeviceSize)g->dumpH * 4;
    /* THE WHOLE CHAIN WHEN THERE IS ONE, in tagpu_gaf.h's layout: level after
       level, end to end, which is what `tagpu_gaf_mip_off` describes. A chain
       is square by construction (job_chain refuses an odd level), so that
       arithmetic applies as it stands. */
    if (g->chainN > 0)
        g->dumpBytes = (VkDeviceSize)tagpu_gaf_mip_chain(g->chainDim, g->chainN);
    /* AND THE SOURCE AFTER IT, in the same buffer and the same submission, so
       the two halves of the pair are read at the same instant rather than a
       frame apart. R8, so one byte a texel. */
    g->dumpSrcOff = g->dumpBytes;
    g->dumpSrcBytes = (g->srcImg && g->srcW > 0 && g->srcH > 0)
                      ? (VkDeviceSize)g->srcW * (VkDeviceSize)g->srcH : 0;
    g->dumpBytes += g->dumpSrcBytes;
    if (!mk_buffer(d, g->dumpBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &g->dumpBuf, &g->dumpMem, &g->dumpMap)) {
        _snprintf(b, sizeof b, LANE ": %s: no host-visible memory for a %u-byte restore"
                  " dump", g->tag, (unsigned)g->dumpBytes);
        b[sizeof b - 1] = 0;
        rlog(b);
        dump_free(d, g);
        g->dumpState = 2;                  /* do not ask again every frame     */
        g->dumpPainted = painted;
        return 1;
    }
    {
        VkBufferImageCopy rg[1 + TAGPU_VK_MAXMIP];
        uint32_t nrg = 1;
        VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        memset(rg, 0, sizeof rg);
        rg[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg[0].imageSubresource.layerCount = 1;
        rg[0].imageExtent.width = (uint32_t)g->dumpW;
        rg[0].imageExtent.height = (uint32_t)g->dumpH;
        rg[0].imageExtent.depth = 1;
        /* ONE REGION PER LEVEL, at the offset tagpu_gaf_mip_off gives it. The
           levels are copied in ONE vkCmdCopyImageToBuffer, so there is no
           ordering question between them and no second barrier. */
        for (; g->chainN > 0 && (int)nrg <= g->chainN; nrg++) {
            int L = (int)nrg, dl = g->chainDim >> L;
            rg[nrg].bufferOffset = (VkDeviceSize)tagpu_gaf_mip_off(g->chainDim, L);
            rg[nrg].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rg[nrg].imageSubresource.mipLevel = (uint32_t)L;
            rg[nrg].imageSubresource.layerCount = 1;
            rg[nrg].imageExtent.width = (uint32_t)dl;
            rg[nrg].imageExtent.height = (uint32_t)dl;
            rg[nrg].imageExtent.depth = 1;
        }
        mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image = g->dstImg;
        mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        /* AND THE BARRIER COVERS EVERY LEVEL IT COPIES, not level 0 alone: a
           transition that names one level leaves the others in whatever layout
           they were, and the copy would then read them from the wrong one. */
        mb.subresourceRange.levelCount = (uint32_t)nrg;
        mb.subresourceRange.layerCount = 1;
        /* SHADER_READ_ONLY in and SHADER_READ_ONLY out: the OUT render pass
           leaves the image there and the consumer's own draw expects it there,
           so the copy borrows the layout and gives it back. The source scope
           names the colour write as well as the sample, because the last thing
           to touch this image was a render pass and not a shader. */
        mb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
        vkCmdCopyImageToBuffer(cb, g->dstImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g->dumpBuf, nrg, rg);
        /* THE SOURCE TOO, and with its own transition: it is the consumer's
           image and the consumer left it SHADER_READ_ONLY, exactly as the
           destination. Same submission, so the pair is one instant. */
        if (g->dumpSrcBytes) {
            VkBufferImageCopy sr;
            VkImageMemoryBarrier sb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            memset(&sr, 0, sizeof sr);
            sr.bufferOffset = g->dumpSrcOff;
            sr.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            sr.imageSubresource.layerCount = 1;
            sr.imageExtent.width = (uint32_t)g->srcW;
            sr.imageExtent.height = (uint32_t)g->srcH;
            sr.imageExtent.depth = 1;
            sb.srcQueueFamilyIndex = sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            sb.image = g->srcImg;
            sb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            sb.subresourceRange.levelCount = 1;
            sb.subresourceRange.layerCount = 1;
            sb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &sb);
            vkCmdCopyImageToBuffer(cb, g->srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   g->dumpBuf, 1, &sr);
            sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            sb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &sb);
        }
        mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &mb);
    }
    g->dumpSlot = slot;
    g->dumpState = 1;
    g->dumpPainted = painted;
    _snprintf(b, sizeof b, LANE ": %s: restore dump of %dx%d recorded on slot %u -"
              " written at this slot's next frame", g->tag, g->dumpW, g->dumpH,
              (unsigned)slot);
    b[sizeof b - 1] = 0;
    rlog(b);
    return 1;
}

/* ONE SLICE. The retire's bit for this slot clears FIRST and UNCONDITIONALLY,
   before any early return -- tagpu_vk_terr.c's rule, and the reason for it is
   that a bit which clears only on the paths that draw would stall the retire
   for ever on a lane that is paused. */
/* ONE REDUCTION OF ONE JOB'S CHAIN, levels 1..chainN, each from the level
   above it. Nothing here is staged per frame slot and written at record time --
   the framebuffers and the descriptor sets were made once when the consumer
   registered the chain, and the only per-draw datum is `uSrcDim`, which goes
   through the globals RING, whose cursor advances per allocation. */
static void chain_step(const TAGPU_VKPASS* d, VkCommandBuffer cb, struct TAGPU_VKRJOB* g)
{
    int L, painted;
    if (!g->core || !g->core->used || g->chainN <= 0 || !s_pipeMip) return;
    painted = g->core->tframes;      /* what `job_painted` publishes */
    /* NOTHING PAINTED MEANS NOTHING TO REDUCE, AND THAT INCLUDES THE FIRST
       PASS. The consumer cannot sample the chain until `job_painted` moves --
       the unit pass sets `s_arHave` only on `painted > 0` -- so there is
       nothing to protect; and level 0 is exactly as UNDEFINED as the rest
       until `dst_ready` transitions it, which happens in the OUT path and
       therefore has NOT happened on a slice that painted nothing. A forced
       first pass would sample level 0 through a descriptor declaring
       SHADER_READ_ONLY_OPTIMAL while the image is still UNDEFINED: a layout
       mismatch, undefined behaviour, and a validation error on the ordinary
       first frame, because the unit job is priority 3 and `covered_prefix`
       deliberately returns 0 while the atlas upload lags. `chainDone` still
       earns its keep -- it is what makes a reduction run again when the paint
       count has NOT moved but the levels are stale. */
    if (painted <= 0) return;
    if (g->chainDone && painted == g->chainPainted) return;
    for (L = 1; L <= g->chainN; L++) {
        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        VkViewport vp; VkRect2D sc;
        int src = g->chainDim >> (L - 1), dst = g->chainDim >> L;
        int32_t dimI = (int32_t)src;
        uint32_t goff = g_alloc(&dimI, sizeof dimI);
        if (goff == 0xFFFFFFFFu) return;       /* the ring said why */
        rbi.renderPass = s_rpMip;
        rbi.framebuffer = g->chainFb[L - 1];
        rbi.renderArea.extent.width = (uint32_t)dst;
        rbi.renderArea.extent.height = (uint32_t)dst;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        memset(&vp, 0, sizeof vp); memset(&sc, 0, sizeof sc);
        vp.width = (float)dst; vp.height = (float)dst; vp.maxDepth = 1.0f;
        sc.extent.width = (uint32_t)dst; sc.extent.height = (uint32_t)dst;
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeMip);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ploMip, 0, 1,
                                &g->chainSet[L - 1], 1, &goff);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    }
    g->chainPainted = painted;
    g->chainDone = 1;
}

void tagpu_vk_restore_step(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    if (!d || !d->dev) return;
    retire_slot_done(d, slot);
    if (!s_built || s_state != ST_READY) return;
    s_d = d; s_cb = cb; s_slot = slot < TAGPU_VK_SLOTS ? slot : 0;
    s_gnext = 0;
    s_gSaid = 0;
    s_sliceOpen = 0;
    /* THE ORACLE BEFORE THE SLICE, and outside every render pass this function
       is about to begin: `cb` is recording and nothing has been drawn into it
       yet, so a copy recorded here sees the destination as the previous frame
       left it -- which is the state the `idle` gate has just called finished.
       One job acts per frame; the dump is 16 to 46 MB and a settled scene does
       none of it. */
    {
        int i;
        for (i = 0; i < TAGPU_R_MAXJOBS; i++)
            if (s_vjob[i].core && dump_step(d, cb, slot, &s_vjob[i])) break;
    }
    tagpu_rcore_step(&s_sched);
    /* AND THE MIP CHAIN AFTER THE SLICE, ONCE -- not once per batch. A slice
       issues as many batches as its budget allows, so a reduction per batch
       would be work per batch for a picture that is only finished at the end
       of the slice; and the levels have to be right before the frame that
       samples them, which is this one. It runs outside every render pass the
       slice began, in the same command buffer and after the last OUT, so the
       render pass dependency chain orders level 0's write against level 1's
       read for us. */
    {
        int i;
        for (i = 0; i < TAGPU_R_MAXJOBS; i++) chain_step(d, cb, &s_vjob[i]);
    }
    s_cb = VK_NULL_HANDLE;
}

void tagpu_vk_restore_lost(void)
{
    /* every id died with the device: forget without destroying, and the jobs
       with them -- the core's own `lost` frees the queues and nothing else */
    memset(s_actImg, 0, sizeof s_actImg); memset(s_actMem, 0, sizeof s_actMem);
    memset(s_actArr, 0, sizeof s_actArr); memset(s_actLay, 0, sizeof s_actLay);
    memset(s_fb, 0, sizeof s_fb); s_nfb = 0;
    /* BOTH RETIRES FORGOTTEN RATHER THAN FREED -- every handle in them died
       with the device, and a `pending` left standing would have a rebuilt
       device's `retire_slot_done` destroy handles that belong to nothing. */
    memset(&s_ret, 0, sizeof s_ret);
    memset(s_jret, 0, sizeof s_jret);
    /* AND EVERY DUMP'S STAGING, forgotten the same way: the buffer and its
       memory died with the device, and a handle left standing here would have
       a later `down` destroy it against the NEW device. The copy it was owed
       never completes, so there is nothing to collect either. */
    {
        int j;
        for (j = 0; j < TAGPU_R_MAXJOBS; j++) {
            s_vjob[j].dumpBuf = VK_NULL_HANDLE; s_vjob[j].dumpMem = VK_NULL_HANDLE;
            s_vjob[j].dumpMap = NULL; s_vjob[j].dumpBytes = 0;
            s_vjob[j].dumpState = 0; s_vjob[j].dumpPainted = 0;
            s_vjob[j].dumpW = s_vjob[j].dumpH = 0;
        }
    }
    memset(s_tabImg, 0, sizeof s_tabImg); memset(s_tabMem, 0, sizeof s_tabMem);
    memset(s_tabView, 0, sizeof s_tabView);
    s_tabStage = VK_NULL_HANDLE; s_tabStageMem = VK_NULL_HANDLE; s_tabInit = 0;
    s_wbuf = VK_NULL_HANDLE; s_wmem = VK_NULL_HANDLE;
    s_gbuf = VK_NULL_HANDLE; s_gmem = VK_NULL_HANDLE; s_gmap = NULL;
    s_vbuf = VK_NULL_HANDLE; s_vmem = VK_NULL_HANDLE;
    s_qpool = VK_NULL_HANDLE;
    s_samp = VK_NULL_HANDLE; s_dpool = VK_NULL_HANDLE;
    s_dslFill = s_dslConv = s_dslOut = s_dslMip = VK_NULL_HANDLE;
    s_ploFill = s_ploConv = s_ploOut = s_ploMip = VK_NULL_HANDLE;
    s_pipeFill = s_pipeOut = s_pipeMip = VK_NULL_HANDLE;
    s_rpMip = VK_NULL_HANDLE;
    /* AND EVERY JOB'S CHAIN, forgotten rather than freed like everything else
       here: the framebuffers and the sets died with the device, and the pool
       they came from with them. A `chainN` left standing would have the next
       reduction bind a set that belongs to nothing. */
    {
        int j;
        for (j = 0; j < TAGPU_R_MAXJOBS; j++) {
            memset(s_vjob[j].chainFb, 0, sizeof s_vjob[j].chainFb);
            memset(s_vjob[j].chainSet, 0, sizeof s_vjob[j].chainSet);
            s_vjob[j].chainN = 0; s_vjob[j].chainDim = 0;
            s_vjob[j].chainPainted = 0; s_vjob[j].chainDone = 0;
        }
    }
    memset(s_pipeConv, 0, sizeof s_pipeConv);
    memset(s_rpAct, 0, sizeof s_rpAct); s_rpOut = VK_NULL_HANDLE;
    s_actSide = 0; s_actLayers = 0; s_actGen++;
    s_built = 0; s_state = ST_UNBUILT;
    memset(&s_dev, 0, sizeof s_dev);
    tagpu_rcore_lost(&s_sched);
}

void tagpu_vk_restore_down(const TAGPU_VKPASS* d)
{
    int i, l;
    if (!d || !d->dev) { tagpu_vk_restore_lost(); return; }
    /* THE TWO RETIRES GO FIRST. A retire outstanding at teardown -- an
       activation grow, or the idle release after 180 quiet slices -- holds two
       array images, their memory, every layer view and up to 32 framebuffers,
       around 100 MB, and `vkDestroyDevice` would run with all of it alive. The
       resize path needs nothing here because the device survives it and a
       later `retire_slot_done` cleans up; after `vk_down` nothing would.
       The licence here is the seam's vkDeviceWaitIdle above this call, which is
       stronger than any mask. */
    jret_flush(d);
    if (s_ret.pending) { s_ret.pending = 0; retire_kill(d); }
    /* AND ANY DUMP STAGING STILL STANDING. A copy recorded into a command
       buffer of the device that is going never completes, so the collection is
       owed to nobody; the seam's vkDeviceWaitIdle is what makes the free safe
       rather than a race. `lost` forgets these the way it forgets every other
       id, through its memset of the jobs. */
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) dump_free(d, &s_vjob[i]);
    fb_flush(d);
    for (i = 0; i < 2; i++) {
        for (l = 0; l < MAXLAYER; l++)
            if (s_actLay[i][l]) vkDestroyImageView(d->dev, s_actLay[i][l], NULL);
        if (s_actArr[i]) vkDestroyImageView(d->dev, s_actArr[i], NULL);
        if (s_actImg[i]) vkDestroyImage(d->dev, s_actImg[i], NULL);
        if (s_actMem[i]) vkFreeMemory(d->dev, s_actMem[i], NULL);
    }
    for (i = 0; i < 3; i++) {
        if (s_tabView[i]) vkDestroyImageView(d->dev, s_tabView[i], NULL);
        if (s_tabImg[i]) vkDestroyImage(d->dev, s_tabImg[i], NULL);
        if (s_tabMem[i]) vkFreeMemory(d->dev, s_tabMem[i], NULL);
    }
    if (s_tabStage) vkDestroyBuffer(d->dev, s_tabStage, NULL);
    if (s_tabStageMem) vkFreeMemory(d->dev, s_tabStageMem, NULL);
    if (s_wbuf) vkDestroyBuffer(d->dev, s_wbuf, NULL);
    if (s_wmem) vkFreeMemory(d->dev, s_wmem, NULL);
    if (s_gbuf) vkDestroyBuffer(d->dev, s_gbuf, NULL);
    if (s_gmem) vkFreeMemory(d->dev, s_gmem, NULL);
    if (s_vbuf) vkDestroyBuffer(d->dev, s_vbuf, NULL);
    if (s_vmem) vkFreeMemory(d->dev, s_vmem, NULL);
    if (s_qpool) vkDestroyQueryPool(d->dev, s_qpool, NULL);
    if (s_pipeFill) vkDestroyPipeline(d->dev, s_pipeFill, NULL);
    if (s_pipeOut) vkDestroyPipeline(d->dev, s_pipeOut, NULL);
    if (s_pipeMip) vkDestroyPipeline(d->dev, s_pipeMip, NULL);
    for (i = 0; i <= RP_MAX; i++) {
        if (s_pipeConv[i]) vkDestroyPipeline(d->dev, s_pipeConv[i], NULL);
        if (s_rpAct[i]) vkDestroyRenderPass(d->dev, s_rpAct[i], NULL);
    }
    if (s_rpOut) vkDestroyRenderPass(d->dev, s_rpOut, NULL);
    if (s_rpMip) vkDestroyRenderPass(d->dev, s_rpMip, NULL);
    if (s_ploFill) vkDestroyPipelineLayout(d->dev, s_ploFill, NULL);
    if (s_ploConv) vkDestroyPipelineLayout(d->dev, s_ploConv, NULL);
    if (s_ploOut) vkDestroyPipelineLayout(d->dev, s_ploOut, NULL);
    if (s_ploMip) vkDestroyPipelineLayout(d->dev, s_ploMip, NULL);
    if (s_dslFill) vkDestroyDescriptorSetLayout(d->dev, s_dslFill, NULL);
    if (s_dslConv) vkDestroyDescriptorSetLayout(d->dev, s_dslConv, NULL);
    if (s_dslOut) vkDestroyDescriptorSetLayout(d->dev, s_dslOut, NULL);
    if (s_dslMip) vkDestroyDescriptorSetLayout(d->dev, s_dslMip, NULL);
    if (s_dpool) vkDestroyDescriptorPool(d->dev, s_dpool, NULL);
    if (s_samp) vkDestroySampler(d->dev, s_samp, NULL);
    tagpu_vk_restore_lost();
}
