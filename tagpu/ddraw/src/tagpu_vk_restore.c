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

   AND IT NEEDS NO FLIP, WHICH IS A THIRD CASE tagpu_vk_pass.h's `flipok` table
   did not have. All three fragment shaders index the slot grid with
   `ivec2(gl_FragCoord.xy)`, and GL measures that y from the framebuffer's
   BOTTOM while Vulkan measures it from the TOP -- `OriginLowerLeft` is not even
   permitted in Vulkan. That looks exactly like landing 5b waiting to happen,
   and it is not, because the two differences cancel:

     * GL:     gl_FragCoord.y ~ 0 is framebuffer row 0, and for an FBO colour
               attachment framebuffer row 0 IS texel row 0. NDC y = -1 maps to
               the viewport's bottom, which is that same row 0.
     * Vulkan: gl_FragCoord.y ~ 0 is framebuffer row 0, which is image row 0.
               NDC y = -1 maps to the viewport's top, which is that same row 0.

   So in BOTH APIs `gl_FragCoord.y` is the target's row index and NDC -1 is row
   0. The conventions differ only about which end of NDC is visually "up", and
   that matters only to a pass that speaks the SCREEN's y. This one never does:
   FILL and CONV cover the whole target with a full-screen triangle and address
   it in texels, and OUT positions each cell by `aPos / uDst * 2 - 1` from atlas
   coordinates and then recovers the same cell from gl_FragCoord. Nothing here
   has an opinion about up.

   The two rows of `flipok`'s table are "GL's window convention, must flip" and
   "the engine's y-DOWN screen space, must not". This is a third: IMAGE SPACE,
   where the two APIs already agree. It is recorded rather than trusted -- the
   byte oracle below is what settles it, and a mirrored restore would show up
   as every tile's rows reversed.

   THE ORACLE FOR THIS PORT IS BYTES, NOT PIXELS, and it is the strongest one
   this plan has had. `tagpu_restoredump.on` writes each restored atlas with
   glGetTexImage; the same dump taken from this lane can be `cmp`-ed against the
   GL lane's, so "the Vulkan restore is the GL restore" is a byte comparison of
   a 46 MB surface rather than a screenshot diff. It needs no window, no Route D
   and no settle heuristic.

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
static unsigned char* s_tabMap;
static int            s_tabInit;                    /* laid out GENERAL yet    */

static VkBuffer       s_wbuf;                       /* the padded weights      */
static VkDeviceMemory s_wmem;
static VkDeviceSize   s_wrange;                     /* WMAX * 64, the bound range */
static VkBuffer       s_wstage;                     /* ...and its one-time upload */
static VkDeviceMemory s_wstageMem;
static unsigned char* s_wstageMap;
static VkDeviceSize   s_wbytes;
static int            s_wUp;                        /* the copy has been recorded */
static const TAGPU_RBACKEND s_be_fwd;               /* defined with the vtable   */

static VkBuffer       s_gbuf;                       /* the scalar-uniform ring */
static VkDeviceMemory s_gmem;
static unsigned char* s_gmap;
static VkDeviceSize   s_gstride;
static uint32_t       s_gnext;

static VkBuffer       s_vbuf;                       /* the OUT vertices        */
static VkDeviceMemory s_vmem;
static unsigned char* s_vmap;

static VkQueryPool    s_qpool;                      /* 2 pairs, one per parity */

/* the slice's context, stashed by `step` for the vtable to reach */
static const TAGPU_VKPASS* s_d;
static VkCommandBuffer     s_cb;
static uint32_t            s_slot;
static int                 s_sliceOpen;             /* the head barrier is done */
static int                 s_sliceTables;           /* tables uploaded this slice */

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

/* A RENDER PASS PER USED ATTACHMENT COUNT. The subpass always declares NK
   colour references, so one pipeline shape fits, and the references past
   `used` are VK_ATTACHMENT_UNUSED -- which is what discards the shader's
   writes to those locations, exactly as glDrawBuffers masking did on the GL
   lane. A tail group is not hypothetical: `full` is eleven layers of kout 16
   at NK 4 and then a last layer of kout 1, and `tiny` is kout 6 at NK 8.
   LOAD is DONT_CARE because a conv draw covers every fragment of its target,
   and the layout is GENERAL at both ends because the activations never leave
   it. */
static int build_rp_act(const TAGPU_VKPASS* d, int used, int nk)
{
    VkAttachmentDescription at[RP_MAX];
    VkAttachmentReference   ref[RP_MAX];
    VkSubpassDescription    sp;
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
    rci.attachmentCount = (uint32_t)used;
    rci.pAttachments = at;
    rci.subpassCount = 1;
    rci.pSubpasses = &sp;
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
    rci.attachmentCount = 1; rci.pAttachments = &at;
    rci.subpassCount = 1; rci.pSubpasses = &sp;
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
   length of `WBlock`. THE LANE IS THEREFORE PINNED TO THE TWO SHIPPED MODELS,
   which is recorded in the plan as the owner's to widen: a third model needs
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

/* THE THREE LAYOUTS ARE THREE, not one, because the SAMPLER INDICES DIFFER
   BETWEEN THE PROGRAMS. spirv-gen.py allocates bindings by STAGE in
   declaration order, so FILL has uAtlas at 40 while OUT has uAct at 40 and
   uAtlas at 41. One shared layout would bind the atlas where OUT expects the
   activations. [Read out of inc/spirv/tagpu_restore_glsl.spv.h, whose per-
   shader interface comment is the contract -- and which only began reporting
   the named WBlock block when this port needed it.] */
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

/* A SLOT HAS TURNED OVER: its last submit is complete, because the seam waited
   on that slot's fence before handing us this command buffer. Called at the top
   of every step, UNCONDITIONALLY. */
static void retire_slot_done(const TAGPU_VKPASS* d, uint32_t slot)
{
    int i, l;
    if (!s_ret.pending) return;
    s_ret.pending &= ~(1u << slot);
    if (s_ret.pending) return;
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
        s_sched.be = &s_be_fwd;
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

    /* the sampler: NEAREST and clamp, which is every sampler the GL lane uses */
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
        s_dslFill = mk_dsl(d, bF, tF, sF, 6);
        s_dslConv = mk_dsl(d, bC, tC, sC, 4);
        s_dslOut  = mk_dsl(d, bO, tO, sO, 7);
    }
    if (!s_dslFill || !s_dslConv || !s_dslOut) goto fail;
    s_ploFill = mk_plo(d, s_dslFill);
    s_ploConv = mk_plo(d, s_dslConv);
    s_ploOut  = mk_plo(d, s_dslOut);
    if (!s_ploFill || !s_ploConv || !s_ploOut) goto fail;

    for (i = 1; i <= nk; i++) if (!build_rp_act(d, i, nk)) goto fail;
    if (!build_rp_out(d)) goto fail;

    vsFS   = mk_mod(d, tagpu_spv_tagpu_restore_glsl_FS_VS,
                    sizeof tagpu_spv_tagpu_restore_glsl_FS_VS / 4);
    vsOut  = mk_mod(d, tagpu_spv_tagpu_restore_glsl_OUT_VS,
                    sizeof tagpu_spv_tagpu_restore_glsl_OUT_VS / 4);
    fsFill = mk_mod(d, tagpu_spv_tagpu_restore_glsl_FILL_FS,
                    sizeof tagpu_spv_tagpu_restore_glsl_FILL_FS / 4);
    fsConv = mk_mod(d, convWords, convN);
    fsOut  = mk_mod(d, tagpu_spv_tagpu_restore_glsl_OUT_FS,
                    sizeof tagpu_spv_tagpu_restore_glsl_OUT_FS / 4);
    if (!vsFS || !vsOut || !fsFill || !fsConv || !fsOut) goto fail;

    /* FILL goes through rpAct[1]: one layer, and the pipeline's blend state
       still declares NK because that is the subpass's colour count */
    s_pipeFill = mk_pipe(d, vsFS, fsFill, s_rpAct[1], nk, s_ploFill, NULL);
    for (i = 1; i <= nk; i++)
        s_pipeConv[i] = mk_pipe(d, vsFS, fsConv, s_rpAct[i], nk, s_ploConv, NULL);
    /* OUT's vertices are the core's 8 floats: x,y | dx,dy,slotCol,slotRow | w,h,
       which is the GL VAO's 2 + 4 + 2 at offsets 0, 8, 24 and stride 32 */
    memset(&vb, 0, sizeof vb); memset(va, 0, sizeof va);
    vb.binding = 0; vb.stride = 32; vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    va[0].location = 0; va[0].binding = 0; va[0].format = VK_FORMAT_R32G32_SFLOAT;       va[0].offset = 0;
    va[1].location = 1; va[1].binding = 0; va[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; va[1].offset = 8;
    va[2].location = 2; va[2].binding = 0; va[2].format = VK_FORMAT_R32G32_SFLOAT;       va[2].offset = 24;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = va;
    s_pipeOut = mk_pipe(d, vsOut, fsOut, s_rpOut, 1, s_ploOut, &vi);
    if (!s_pipeFill || !s_pipeOut) goto fail;
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
    if (!mk_buffer(d, (VkDeviceSize)sizeof s_sched.verts * TAGPU_VK_SLOTS,
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_vbuf, &s_vmem, &s_vmap)) goto fail;

    /* the three slot tables and their staging */
    for (i = 0; i < 3; i++)
        if (!mk_image(d, TAGPU_R_SLOTCOLS, TAGPU_R_SLOTROWS, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &s_tabImg[i], &s_tabMem[i], &s_tabView[i])) goto fail;
    if (!mk_buffer(d, (VkDeviceSize)TAGPU_R_BATCH * 16 * 3 * TAGPU_VK_SLOTS,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_tabStage, &s_tabStageMem, &s_tabMap)) goto fail;

    /* a descriptor pool big enough for every job's five sets */
    memset(psz, 0, sizeof psz);
    psz[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    psz[0].descriptorCount = TAGPU_R_MAXJOBS * 8;
    psz[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    psz[1].descriptorCount = TAGPU_R_MAXJOBS * 32;
    dpi.maxSets = TAGPU_R_MAXJOBS * 8;
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
    if (!ok) rlog(LANE ": the shared resources could not be built - the lane stays indexed");
    return ok;
}

int tagpu_vk_restore_nk(void) { return s_sched.nk; }

void tagpu_vk_restore_lost(void)
{
    /* every id died with the device: forget without destroying, and the jobs
       with them -- the core's own `lost` frees the queues and nothing else */
    memset(s_actImg, 0, sizeof s_actImg); memset(s_actMem, 0, sizeof s_actMem);
    memset(s_actArr, 0, sizeof s_actArr); memset(s_actLay, 0, sizeof s_actLay);
    memset(s_fb, 0, sizeof s_fb); s_nfb = 0;
    memset(s_tabImg, 0, sizeof s_tabImg); memset(s_tabMem, 0, sizeof s_tabMem);
    memset(s_tabView, 0, sizeof s_tabView);
    s_tabStage = VK_NULL_HANDLE; s_tabStageMem = VK_NULL_HANDLE; s_tabMap = NULL; s_tabInit = 0;
    s_wbuf = VK_NULL_HANDLE; s_wmem = VK_NULL_HANDLE;
    s_gbuf = VK_NULL_HANDLE; s_gmem = VK_NULL_HANDLE; s_gmap = NULL;
    s_vbuf = VK_NULL_HANDLE; s_vmem = VK_NULL_HANDLE; s_vmap = NULL;
    s_qpool = VK_NULL_HANDLE;
    s_samp = VK_NULL_HANDLE; s_dpool = VK_NULL_HANDLE;
    s_dslFill = s_dslConv = s_dslOut = VK_NULL_HANDLE;
    s_ploFill = s_ploConv = s_ploOut = VK_NULL_HANDLE;
    s_pipeFill = s_pipeOut = VK_NULL_HANDLE;
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
    for (i = 0; i <= RP_MAX; i++) {
        if (s_pipeConv[i]) vkDestroyPipeline(d->dev, s_pipeConv[i], NULL);
        if (s_rpAct[i]) vkDestroyRenderPass(d->dev, s_rpAct[i], NULL);
    }
    if (s_rpOut) vkDestroyRenderPass(d->dev, s_rpOut, NULL);
    if (s_ploFill) vkDestroyPipelineLayout(d->dev, s_ploFill, NULL);
    if (s_ploConv) vkDestroyPipelineLayout(d->dev, s_ploConv, NULL);
    if (s_ploOut) vkDestroyPipelineLayout(d->dev, s_ploOut, NULL);
    if (s_dslFill) vkDestroyDescriptorSetLayout(d->dev, s_dslFill, NULL);
    if (s_dslConv) vkDestroyDescriptorSetLayout(d->dev, s_dslConv, NULL);
    if (s_dslOut) vkDestroyDescriptorSetLayout(d->dev, s_dslOut, NULL);
    if (s_dpool) vkDestroyDescriptorPool(d->dev, s_dpool, NULL);
    if (s_samp) vkDestroySampler(d->dev, s_samp, NULL);
    tagpu_vk_restore_lost();
}
