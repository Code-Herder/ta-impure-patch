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
