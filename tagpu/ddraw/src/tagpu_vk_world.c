/* The offscreen world target and the draw that puts it on the frame.
   The header carries the argument; this is the mechanism. */

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_world.h"
#include "tagpu_native.h"                  /* TAGPU_WORLDTGT: gw, gh, ss, the rect */
#include "tagpu_pal.h"                     /* tagpu_pal_gamma: the factor applied here */
#include "spirv/tagpu_native.spv.h"        /* DVS / DFS / GFS -- tagpu_native.c's resolve */

#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties)

#define DFNS(X) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
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
    X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

#define NV 4                            /* the unit square, as a strip */

/* THE BOUND ON THE TARGET, AND IT IS A BOUND ON AN ALLOCATION RATHER THAN A
   TASTE. `gw * ss` is arithmetic on a value that reached us through the frame
   packet and a lever, and this module turns it into two device allocations
   of `w * h` pixels: the colour at 4 bytes a pixel and the depth-stencil at
   the seam's format's (4 for D24_UNORM_S8_UINT; D32_SFLOAT_S8_UINT holds 5
   of data, and the reference setup's driver lays it out in 8 -- gpu-status
   §2.91). TAGPU_SS_MAX is 4 and the largest game
   resolution the fork offers is well inside this, so a target past it is a
   value that is not what it claims to be -- refused, and said once. */
#define WORLD_MAXDIM 8192
/* AND A BOUND ON THE FACTOR TOO, so that `gw * ss` cannot overflow before the
   bound above is applied to it. TAGPU_SS_MAX (tagpu_native.c) is 4; this is
   deliberately its own number rather than that one, because it guards a
   DIFFERENT thing -- what this module will allocate -- and coupling the two
   would make a change to the supersample ceiling silently change an
   allocation bound. */
#define WORLD_SSMAX  8

static int s_state;
static int s_downOwed;
static int s_downPaying;
static int s_openSlot = -1;             /* the slot `begin` opened, -1 = none */
/* THE SLOT THIS FRAME'S WORLD WAS ACTUALLY RENDERED INTO, -1 = none, and the
   only thing `tagpu_vk_world_shot` will read. `s_drawThis` cannot serve: it is
   cleared by `record`, which runs BEFORE the capture is recorded, so a shot
   asking it would always be told there is no target. This is set where the
   render pass is opened and cleared where the frame's decision is taken, so it
   is a statement about what the command buffer contains rather than about what
   the module intended. */
static int s_drewSlot = -1;
static int s_drawThis;                  /* `record` may composite this frame  */
static int s_saidBig;                   /* the per-factor bound said once  */
static int s_saidBigProduct;            /* ...and the product's, its own   */
static int s_vx, s_vy, s_vw, s_vh;      /* where the block lands, this frame  */
static unsigned s_nFrames, s_nBuilds, s_saidAt;
static int s_lastW, s_lastH, s_lastSS, s_lastDevres;

/* THE FORMAT THE COLOUR IMAGES WERE ACTUALLY BUILT WITH, so that anything
   reading one takes its format from the image rather than from a field that was
   copied at bring-up. `d->fmt` is assigned once (`s_pass.fmt = s_vk.fmt`) while
   `vk_resize` re-runs `vk_swapchain`, which re-picks `s_vk.fmt` and refreshes
   only `s_pass.slots` -- so the two can in principle part company, and the A/B
   capture decides its channel order from a format. Recording it here makes that
   decision a question about THIS image. */
static VkFormat              s_colFmt = VK_FORMAT_UNDEFINED;

static VkRenderPass          s_rp;      /* the OFFSCREEN pass, not the seam's */
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;    /* the composite, built against d->rp */
static VkPipeline            s_pipeG;   /* ...and the same through the Gamma curve */
static VkDescriptorPool      s_dpool;
static VkSampler             s_samp;
static VkBuffer              s_vbuf;
static VkDeviceMemory        s_vmem;

typedef struct {
    VkImage         col,  dep;
    VkDeviceMemory  cmem, dmem;
    VkImageView     cview, dview;
    VkFramebuffer   fb;
    VkDescriptorSet dset;
    int             w, h;               /* what this slot is sized for, 0 = nothing */
} SLOT;
static SLOT s_slot[TAGPU_VK_SLOTS];

/* THE GAMMA CURVE, ONE PER SLOT, and that is the same lifetime argument as the
   target's: the composite of frame N samples slot N's curve while frame N+1
   may be uploading a new one, so a shared image would be rewritten under a
   read. Slot `i`'s curve is rewritten only in slot `i`'s `prepare`, behind the
   seam's fence wait on that slot, and only when the factor it holds is not
   this frame's -- so a change of Gamma reaches the screen on the frame that
   carries it, and a steady factor costs nothing. They live as long as the
   module, not as long as a target size: `slot_size` never touches them. */
typedef struct {
    VkImage         img;                /* 256 x 1 R8: texel e = the engine's output for e */
    VkDeviceMemory  mem;
    VkImageView     view;
    VkBuffer        st;                 /* 256 bytes, host-visible, mapped for life */
    VkDeviceMemory  stMem;
    unsigned char*  stMap;
    float           f;                  /* the factor `img` holds...           */
    int             have;               /* ...once something has been uploaded */
} GAM;
static GAM s_gam[TAGPU_VK_SLOTS];
static int s_gamOn;                     /* `record` composites through the curve */

/* THE GAMMA ON A FRAME WITH NO TARGET (the header's paragraph). Its own state,
   its own layout and its own quad, because the target's are freed by the very
   refusal that makes these the only way the factor reaches the world: nothing
   here is shared with `build`, and `_down_paid` leaves all of it standing.
   Two pipelines, one per direction, because a blend factor above 1.0 does not
   exist -- a fixed-point attachment clamps its constants to [0,1]:
     UP   (factor > 1)  1.0 x DST_COLOR + frame x CONSTANT (factor - 1)
     DOWN (factor < 1)  frame x CONSTANT (factor)
   and a factor above 2 is UP run more than once (`record_direct`). Nothing
   per slot: the pipelines and the quad are read-only, and the factor rides in
   the command buffer as the blend constants. */
static int            s_dcState;
static int            s_dcSaid;         /* the refusal said once */
static VkPipelineLayout s_dcPlo;
static VkPipeline     s_dcUp, s_dcDown;
static VkBuffer       s_dcVbuf;
static VkDeviceMemory s_dcVmem;

static void plog(const TAGPU_VKPASS* d, const char* fmt, ...)
{
    char b[256];
    va_list ap;
    if (!d->log) return;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    d->log(b);
}

static int mem_type(const TAGPU_VKPASS* d, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(d->pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static VkShaderModule mk_module(const TAGPU_VKPASS* d, const uint32_t* w, size_t words)
{
    VkShaderModuleCreateInfo sci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    sci.codeSize = words * 4;
    sci.pCode = w;
    if (vkCreateShaderModule(d->dev, &sci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

static int resolve(const TAGPU_VKPASS* d)
{
#define RES_I(n) n = (PFN_##n)d->gipa(d->inst, #n); if (!n) return 0;
#define RES_D(n) n = (PFN_##n)d->gdpa(d->dev, #n); if (!n) return 0;
    IFNS(RES_I)
    DFNS(RES_D)
#undef RES_I
#undef RES_D
    return 1;
}

/* One attachment image: colour (sampled afterwards) or depth (never sampled). */
static int mk_att(const TAGPU_VKPASS* d, int w, int h, VkFormat fmt, int depth,
                  VkImage* img, VkDeviceMemory* mem, VkImageView* view)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements req;
    int type;

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = (uint32_t)w;
    ici.extent.height = (uint32_t)h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    /* THE COLOUR IMAGE IS SAMPLED AND THE DEPTH ONE IS NOT, which is the whole
       difference between them here. Nothing reads the depth image, and asking
       for SAMPLED on a depth image narrows the formats a device will give us
       for no gain.

       AND THE COLOUR IMAGE CARRIES TRANSFER_SRC, WHICH IS THE A/B's. This is
       the image a world capture reads (`tagpu_vk_world_shot`), and
       `vkCmdCopyImageToBuffer` requires the usage to have been asked for AT
       CREATION -- no layout transition confers it, and a driver that copies
       anyway is undefined behaviour a whole 0-px result can rest on unseen.

       UNCONDITIONAL, on purpose, AND IT IS NOT FREE. The alternative is a second
       build key so the flag is only present while a lever is armed -- which
       trades a usage bit for a rebuild on the measured frame and a rebuild path
       that only ever runs while someone is measuring, i.e. the least-exercised
       code in the module guarding the most-trusted number in it.

       THE COST, NAMED RATHER THAN WAVED AT: on several drivers -- AMD's DCC
       above all -- TRANSFER_SRC on a colour attachment disables lossless
       framebuffer compression for the life of the image, and this image is
       written by five passes and sampled by the composite EVERY frame. So a
       steady-state cost is being paid to serve a lever that fires on one frame.
       It has not been measured. The swapchain images are no precedent
       (tagpu_vk.c: `if (s_vk.cansrc) swci.imageUsage |= ...`) -- that flag is
       conditional on what the SURFACE offers, not on a build key, so it is not
       the same trade.

       A device that refuses the combination fails vkCreateImage, which this
       function reports as a refusal and the seam answers by leaving the world
       on the swapchain image -- a smaller picture and never a wrong one. */
    ici.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                      : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, *img, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, *img, *mem, 0) != VK_SUCCESS) return 0;

    ivi.image = *img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    /* THE STENCIL ASPECT COMES WITH THE FORMAT, NOT WITH A WISH. The seam's
       depth format is whatever it found; D24_UNORM_S8_UINT and D32_SFLOAT_S8
       carry a stencil plane and a view of them must name it, or the framebuffer
       is refused. */
    ivi.subresourceRange.aspectMask =
        depth ? (VK_IMAGE_ASPECT_DEPTH_BIT |
                 ((fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
                   fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                   fmt == VK_FORMAT_D16_UNORM_S8_UINT)
                      ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
              : VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, view) != VK_SUCCESS) return 0;
    return 1;
}

static void slot_free(const TAGPU_VKPASS* d, SLOT* s)
{
    VkDevice dev = d->dev;
    if (s->fb)    { vkDestroyFramebuffer(dev, s->fb, NULL); s->fb = VK_NULL_HANDLE; }
    if (s->cview) { vkDestroyImageView(dev, s->cview, NULL); s->cview = VK_NULL_HANDLE; }
    if (s->col)   { vkDestroyImage(dev, s->col, NULL); s->col = VK_NULL_HANDLE; }
    if (s->cmem)  { vkFreeMemory(dev, s->cmem, NULL); s->cmem = VK_NULL_HANDLE; }
    if (s->dview) { vkDestroyImageView(dev, s->dview, NULL); s->dview = VK_NULL_HANDLE; }
    if (s->dep)   { vkDestroyImage(dev, s->dep, NULL); s->dep = VK_NULL_HANDLE; }
    if (s->dmem)  { vkFreeMemory(dev, s->dmem, NULL); s->dmem = VK_NULL_HANDLE; }
    s->w = s->h = 0;
}

/* Make slot `s` carry a `w` x `h` target. Safe because the caller owns this
   slot: the seam waited on fence[slot] at the top of the frame, so the submit
   that last drew into these images and sampled them has completed. */
static int slot_size(const TAGPU_VKPASS* d, SLOT* s, int w, int h)
{
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkImageView att[2];
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;

    if (s->w == w && s->h == h) return 1;
    slot_free(d, s);

    if (!mk_att(d, w, h, d->fmt, 0, &s->col, &s->cmem, &s->cview)) { slot_free(d, s); return 0; }
    s_colFmt = d->fmt;
    if (!mk_att(d, w, h, d->dfmt, 1, &s->dep, &s->dmem, &s->dview)) { slot_free(d, s); return 0; }

    att[0] = s->cview; att[1] = s->dview;
    fci.renderPass = s_rp;
    fci.attachmentCount = 2;
    fci.pAttachments = att;
    fci.width = (uint32_t)w;
    fci.height = (uint32_t)h;
    fci.layers = 1;
    if (vkCreateFramebuffer(d->dev, &fci, NULL, &s->fb) != VK_SUCCESS) { slot_free(d, s); return 0; }

    /* THE COMPOSITE'S DESCRIPTOR FOLLOWS THE IMAGE, written here and nowhere
       else: the set is allocated once and re-pointed whenever the image behind
       it is replaced, so a set in hand always names the CURRENT image. A resize
       that rebuilt the image and left the set is how a pass samples freed
       memory, and it is the one thing this function exists to make impossible. */
    memset(&ii, 0, sizeof ii); memset(&wr, 0, sizeof wr);
    ii.sampler = s_samp;
    ii.imageView = s->cview;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = s->dset; wr.dstBinding = 40; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);

    s->w = w; s->h = h;
    s_nBuilds++;
    return 1;
}

static int build_renderpass(const TAGPU_VKPASS* d)
{
    VkAttachmentDescription at[2];
    VkAttachmentReference cr, dr;
    VkSubpassDescription sp;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    memset(at, 0, sizeof at);
    /* COMPATIBLE WITH THE SEAM'S RENDER PASS, WHICH IS WHY NO PIPELINE MOVED.
       Compatibility compares attachment FORMATS, sample counts and the subpass
       references -- never the load/store ops or the layouts -- so these two
       must be `d->fmt` and `d->dfmt` in the seam's own order and everything
       below is free to differ. That is the whole reason tagpu_vk_terr.c and its
       four siblings draw into this image without a line of them changing. */
    at[0].format = d->fmt;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    /* CLEARED TO TRANSPARENT, not loaded: this image is the world alone and the
       composite blends it over what is beneath it. */
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    at[1].format = d->dfmt;
    at[1].samples = VK_SAMPLE_COUNT_1_BIT;
    at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    /* NOTHING READS THE DEPTH AFTER THE PASS, so a store would be a write
       nobody reads and DONT_CARE is the honest declaration. Anything that
       comes to read it (a 1x depth for `selAt1x`, which the header says this
       target cannot express) makes this STORE. */
    at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    memset(&cr, 0, sizeof cr); memset(&dr, 0, sizeof dr);
    cr.attachment = 0; cr.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dr.attachment = 1; dr.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cr;
    sp.pDepthStencilAttachment = &dr;

    /* TWO DEPENDENCIES, AND THEY ARE THE ORDERING THAT MAKES THE COMPOSITE
       LEGAL RATHER THAN LUCKY. [0] keeps this frame's writes behind whatever
       last sampled this image; [1] makes those writes VISIBLE to the fragment
       stage that samples it later in the same command buffer. Without [1] the
       composite reads an image the driver has not finished writing, which no
       fence and no queue submit orders for us -- a render-pass boundary is not
       a memory barrier. */
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    /* THE SOURCE SCOPE IS EVERY WAY THIS SLOT'S IMAGES WERE LAST USED, not
       just the sampling. The previous frame in this slot ended its colour
       writes at COLOR_ATTACHMENT_OUTPUT and its depth writes at
       LATE_FRAGMENT_TESTS, and this frame's LOAD_OP_CLEAR -- which the
       destination scope below DOES name -- is a write-after-write against both.
       Between two submits on one queue the ordering is submission order, but
       the ACCESS still has to be made visible. tagpu_vk.c's own render pass
       states this argument in terms and carries these masks; leaving them off
       here is the class of omission a validation layer catches and a correct
       picture does not -- and the fence wait making it work in practice is
       safety by timing, which CLAUDE.md refuses as an argument. */
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    /* NEITHER OF THESE IS BY-REGION, AND THAT IS THE WHOLE POINT OF THE
       COMPOSITE. `VK_DEPENDENCY_BY_REGION_BIT` promises that a destination
       fragment at (x, y) depends only on (x, y) of the source -- which is what
       lets a tiled device keep a tile on chip. This pass's consumer is the
       composite, in a DIFFERENT render pass with a DIFFERENT framebuffer, and
       it samples at SCALED coordinates: at ss = 2 one destination pixel reads a
       2x2 source block, and at k != 1 an arbitrary neighbourhood. A by-region
       dependency would therefore under-synchronise exactly the reads that cross
       a tile edge -- a hazard that produces a seam on some devices and nothing
       at all on others, which is the failure mode CLAUDE.md's synchronisation
       rule exists for. Both dependencies are global. */
    dep[0].dependencyFlags = 0;

    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    /* AND THE DEPTH HALF HERE TOO, for the final-layout transition: the pass
       wrote depth as well as colour and both have to be made available before
       the attachments leave it. tagpu_vk.c does the same on its own dep[1]. */
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep[1].dependencyFlags = 0;

    rpi.attachmentCount = 2;
    rpi.pAttachments = at;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sp;
    rpi.dependencyCount = 2;
    rpi.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &rpi, NULL, &s_rp) == VK_SUCCESS;
}

static int build_sampler(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties fp;

    /* AN sRGB SWAPCHAIN FORMAT IS REFUSED RATHER THAN RENDERED INTO. The seam
       PREFERS VK_FORMAT_B8G8R8A8_UNORM but falls back to `fmts[0]`, so `d->fmt`
       is not guaranteed linear. Drawing the world into an sRGB intermediate
       would make the passes blend against a DECODED value and the composite
       decode again on sample -- a different picture from the linear RGBA8 one
       every pass's blend is written for, arrived at silently. Refusing leaves the
       world on the swapchain image, which is the same one round trip the lane
       has without a target. */
    if (d->fmt == VK_FORMAT_B8G8R8A8_SRGB || d->fmt == VK_FORMAT_R8G8B8A8_SRGB ||
        d->fmt == VK_FORMAT_A8B8G8R8_SRGB_PACK32) {
        plog(d, "world: the surface gave us an sRGB format (%d) and the "
                "world target is linear RGBA8 - an sRGB intermediate would blend "
                "against decoded values, so the world stays on the swapchain image",
             (int)d->fmt);
        return 0;
    }
    vkGetPhysicalDeviceFormatProperties(d->pd, d->fmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
        !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        plog(d, "world: this device will not both render to and LINEAR-filter "
                "format %d - the world stays on the swapchain image at client "
                "resolution", (int)d->fmt);
        return 0;
    }
    /* LINEAR, AND IT IS THE DOWNSAMPLE RATHER THAN A SMOOTHING. At ss = 2 and
       k = 1 a destination centre maps to the exact corner of a 2x2 source
       block, so the bilinear tap IS that block's average -- the exact 2:1 box
       filter tagpu_native.c's comment on DVS/DFS names.
       CLAMP_TO_EDGE because the quad addresses [0,1] exactly and a sample at
       the very edge must not wrap to the far side of the world. */
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = 0.0f;
    return vkCreateSampler(d->dev, &sci, NULL, &s_samp) == VK_SUCCESS;
}

/* ONE FULL-FRAME QUAD PIPELINE against the seam's render pass: DVS over the
   unit-square strip, no depth test, no cull, and the blend it is handed. The
   two composites and the two no-target Gamma draws are this pipeline with a
   different fragment stage and blend, and differ in nothing else. `nDyn` is 2
   (viewport, scissor) or 3 (and the blend constants, which carry the factor). */
static VkResult quad_pipeline(const TAGPU_VKPASS* d, VkPipelineLayout lay,
                              VkShaderModule vs, VkShaderModule fs,
                              const VkPipelineColorBlendAttachmentState* cba,
                              uint32_t nDyn, VkPipeline* out)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va;
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    /* ONE vec2 AT LOCATION 0, which is what DVS declares in its own source --
       `layout(location=0) in vec2 p;` -- so unlike the fork's shaders this one
       needs no ATTR_LOCATIONS table to agree with. */
    memset(&vb, 0, sizeof vb);
    vb.binding = 0; vb.stride = 2 * sizeof(float);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(&va, 0, sizeof va);
    va.location = 0; va.binding = 0;
    va.format = VK_FORMAT_R32G32_SFLOAT; va.offset = 0;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &va;

    /* A STRIP OF FOUR, over `{0,0, 1,0, 0,1, 1,1}` -- QUAD below. */
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    vp.viewportCount = 1; vp.scissorCount = 1;

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* DECLARED AND OFF: the seam's subpass has a depth attachment and a null
       pDepthStencilState there is invalid (tagpu_vk_pass.h). The world's depth
       lives in the offscreen pass and died with it; this draw is a flat blit
       over TA's frame and tests nothing. */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    cb.attachmentCount = 1; cb.pAttachments = cba;

    dy.dynamicStateCount = nDyn; dy.pDynamicStates = dyn;

    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = lay;
    /* THE SEAM'S RENDER PASS, not `s_rp`: every draw built here happens on the
       FRAME. `s_rp` is where the world goes and none of these ever runs inside
       it. */
    gp.renderPass = d->rp;
    gp.subpass = 0;
    return vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, out);
}

/* THE QUAD: `{0,0, 1,0, 0,1, 1,1}` as a triangle strip, in UNIT-SQUARE space
   because DVS is what maps it to clip (`p * 2 - 1`) and to texcoords
   (`uv = p`). The quad and the flip are ONE choice (the header's ORIENTATION
   paragraph): changing either alone draws the world upside down. */
static const float QUAD[NV * 2] = { 0.f,0.f,  1.f,0.f,  0.f,1.f,  1.f,1.f };

/* A host-visible vertex buffer holding QUAD. */
static int quad_buffer(const TAGPU_VKPASS* d, VkBuffer* buf, VkDeviceMemory* mem)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    void* p = NULL;
    int type;

    bci.size = sizeof QUAD;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(d->dev, *buf, &req);
    type = mem_type(d, req.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) return 0;
    if (vkMapMemory(d->dev, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
    memcpy(p, QUAD, sizeof QUAD);
    vkUnmapMemory(d->dev, *mem);
    return 1;
}

static int build_pipeline(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[2];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, gfs = VK_NULL_HANDLE;
    VkResult r;
    int ok = 0;

    /* BINDINGS 40 AND 41, WHICH ARE THE GENERATED HEADER'S AND NOT A CHOICE.
       spirv-gen puts the fork's samplers at 40 upward in declaration order;
       `tagpu_native::GFS` declares `uTex` at 40 and `uGam` at 41 in its own
       comment, and DFS declares `uTex` alone. ONE LAYOUT SERVES BOTH
       PIPELINES: a binding a pipeline's shader does not use is legal in its
       layout, so the plain composite leaves 41 unread. */
    memset(b, 0, sizeof b);
    b[0].binding = 40;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1] = b[0];
    b[1].binding = 41;
    dli.bindingCount = 2; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS) return 0;
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) return 0;

    vs = mk_module(d, tagpu_spv_tagpu_native_DVS,
                   sizeof tagpu_spv_tagpu_native_DVS / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_tagpu_native_DFS,
                   sizeof tagpu_spv_tagpu_native_DFS / sizeof(uint32_t));
    gfs = mk_module(d, tagpu_spv_tagpu_native_GFS,
                    sizeof tagpu_spv_tagpu_native_GFS / sizeof(uint32_t));
    if (!vs || !fs || !gfs) { plog(d, "world: a shader module was refused"); goto out; }

    /* PREMULTIPLIED, over a target cleared to {0,0,0,0}, so a pixel the world
       did not cover leaves what is beneath it untouched. Both halves are
       needed, because either one alone is a different picture -- ONE/ZERO
       would overwrite every pixel the world did not cover, and an opaque clear
       would do the same through this blend. */
    memset(&cba, 0, sizeof cba);
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    r = quad_pipeline(d, s_plo, vs, fs, &cba, 2, &s_pipe);
    if (r != VK_SUCCESS) { plog(d, "world: the composite pipeline was refused (%d)", (int)r); goto out; }
    /* THE SAME PIPELINE WITH THE GAMMA STAGE: everything but the fragment
       stage is shared, so the two composites cannot differ in anything else. */
    r = quad_pipeline(d, s_plo, vs, gfs, &cba, 2, &s_pipeG);
    if (r != VK_SUCCESS) { plog(d, "world: the Gamma composite pipeline was refused (%d)", (int)r); goto out; }
    ok = 1;
out:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    if (gfs) vkDestroyShaderModule(d->dev, gfs, NULL);
    return ok;
}

static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorPoolSize ps;
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSet sets[TAGPU_VK_SLOTS];
    uint32_t i;

    memset(&ps, 0, sizeof ps);
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = d->slots * 2;          /* the target and the curve */
    dpi.maxSets = d->slots;
    dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) return 0;

    for (i = 0; i < d->slots; i++) lay[i] = s_dsl;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = d->slots;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, sets) != VK_SUCCESS) return 0;
    /* THE SETS ARE ALLOCATED HERE AND POINTED AT AN IMAGE IN `slot_size`. They
       are deliberately left UNWRITTEN until then: there is no image yet, and a
       set written with a stale view is exactly the fault `slot_size` closes. */
    for (i = 0; i < d->slots; i++) s_slot[i].dset = sets[i];
    return 1;
}

static void gamma_free(const TAGPU_VKPASS* d, GAM* g)
{
    VkDevice dev = d->dev;
    if (g->view)  { vkDestroyImageView(dev, g->view, NULL); g->view = VK_NULL_HANDLE; }
    if (g->img)   { vkDestroyImage(dev, g->img, NULL); g->img = VK_NULL_HANDLE; }
    if (g->mem)   { vkFreeMemory(dev, g->mem, NULL); g->mem = VK_NULL_HANDLE; }
    if (g->stMap) { vkUnmapMemory(dev, g->stMem); g->stMap = NULL; }
    if (g->st)    { vkDestroyBuffer(dev, g->st, NULL); g->st = VK_NULL_HANDLE; }
    if (g->stMem) { vkFreeMemory(dev, g->stMem, NULL); g->stMem = VK_NULL_HANDLE; }
    g->have = 0; g->f = 0.0f;
}

/* Every slot's curve image, its staging, and binding 41 of the slot's set --
   written once, because the view never changes for the life of the module. */
static int build_gamma(const TAGPU_VKPASS* d)
{
    uint32_t i;
    for (i = 0; i < d->slots; i++) {
        GAM* g = &s_gam[i];
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        VkMemoryRequirements req;
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet wr;
        void* p = NULL;
        int type;

        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8_UNORM;
        ici.extent.width = 256; ici.extent.height = 1; ici.extent.depth = 1;
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(d->dev, &ici, NULL, &g->img) != VK_SUCCESS) return 0;
        vkGetImageMemoryRequirements(d->dev, g->img, &req);
        type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) return 0;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = (uint32_t)type;
        if (vkAllocateMemory(d->dev, &mai, NULL, &g->mem) != VK_SUCCESS) return 0;
        if (vkBindImageMemory(d->dev, g->img, g->mem, 0) != VK_SUCCESS) return 0;
        ivi.image = g->img;
        ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivi.format = VK_FORMAT_R8_UNORM;
        ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivi.subresourceRange.levelCount = 1;
        ivi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(d->dev, &ivi, NULL, &g->view) != VK_SUCCESS) return 0;

        bci.size = 256;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(d->dev, &bci, NULL, &g->st) != VK_SUCCESS) return 0;
        vkGetBufferMemoryRequirements(d->dev, g->st, &req);
        type = mem_type(d, req.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type < 0) return 0;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = (uint32_t)type;
        if (vkAllocateMemory(d->dev, &mai, NULL, &g->stMem) != VK_SUCCESS) return 0;
        if (vkBindBufferMemory(d->dev, g->st, g->stMem, 0) != VK_SUCCESS) return 0;
        if (vkMapMemory(d->dev, g->stMem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
        g->stMap = (unsigned char*)p;
        g->have = 0;

        /* THE LAYOUT NAMED HERE IS A PROMISE `gamma_upload` KEEPS: the Gamma
           pipeline, the only one that reads 41, is bound only on a frame whose
           `prepare` has put this image in SHADER_READ_ONLY_OPTIMAL. */
        memset(&ii, 0, sizeof ii); memset(&wr, 0, sizeof wr);
        ii.sampler = s_samp;
        ii.imageView = g->view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = s_slot[i].dset; wr.dstBinding = 41; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &ii;
        vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    }
    return 1;
}

/* THE ENGINE'S CURVE FOR FACTOR `f`, into this slot's image, unless it already
   holds it. 0x4BA200 hands DirectDraw min(255, trunc(entry x f)) per channel,
   the product taken in x87 extended precision from the float at
   `globals+0x614`; e x f with e <= 255 and a 24-bit f is exact in a double,
   so this is that value for every e, bit for bit. Recorded before any render
   pass opens (the seam calls `prepare` first), into THIS slot's image, whose
   last reader was this slot's previous composite -- behind the fence the seam
   waited on. The barrier still orders it, because the fence proves the
   submit finished and not that its reads are visible to a transfer. */
static int gamma_upload(VkCommandBuffer cb, GAM* g, float f)
{
    VkImageMemoryBarrier ib;
    VkBufferImageCopy rg;
    int e;

    if (!g->img || !g->stMap) return 0;
    if (g->have && g->f == f) return 1;
    for (e = 0; e < 256; e++) {
        double v = (double)e * (double)f;       /* f is bounded to 0.05..8 */
        g->stMap[e] = (unsigned char)(v >= 255.0 ? 255 : (int)v);
    }

    memset(&ib, 0, sizeof ib);
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = g->img;
    ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ib.subresourceRange.levelCount = 1;
    ib.subresourceRange.layerCount = 1;
    ib.oldLayout = g->have ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcAccessMask = g->have ? VK_ACCESS_SHADER_READ_BIT : 0;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, g->have ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = 256; rg.imageExtent.height = 1; rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, g->st, g->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    g->f = f;
    g->have = 1;
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{
    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "world: %u frame slots is outside what this module carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    /* NO DEPTH FORMAT, NO WORLD TARGET. The five world passes depth-test
       against each other and a target without depth would render them in
       submission order -- a different picture, not a smaller one. The seam
       already refuses to give depth-testing passes a pass without it
       (tagpu_vk_pass.h); this is the same rule for the target they draw into. */
    if (d->dfmt == VK_FORMAT_UNDEFINED) {
        plog(d, "world: the seam found no depth format, so an offscreen world "
                "target would render the five world passes untested - the world "
                "stays on the swapchain image at client resolution");
        return 0;
    }
    if (!resolve(d)) { plog(d, "world: an entry point is missing"); return 0; }
    if (!build_renderpass(d)) { plog(d, "world: the offscreen render pass was refused"); return 0; }
    if (!build_sampler(d)) return 0;
    if (!build_pipeline(d)) return 0;
    if (!build_descriptors(d)) return 0;
    if (!build_gamma(d)) { plog(d, "world: the Gamma curve images were refused"); return 0; }
    if (!quad_buffer(d, &s_vbuf, &s_vmem)) return 0;

    plog(d, "world: the offscreen world target is up - %u frame slots, colour "
            "format %d, depth format %d", (unsigned)d->slots, (int)d->fmt, (int)d->dfmt);
    return 1;
}

static void direct_free(const TAGPU_VKPASS* d)
{
    if (!vkDestroyPipeline) return;         /* never resolved: nothing was made */
    if (s_dcUp)   { vkDestroyPipeline(d->dev, s_dcUp, NULL); s_dcUp = VK_NULL_HANDLE; }
    if (s_dcDown) { vkDestroyPipeline(d->dev, s_dcDown, NULL); s_dcDown = VK_NULL_HANDLE; }
    if (s_dcPlo)  { vkDestroyPipelineLayout(d->dev, s_dcPlo, NULL); s_dcPlo = VK_NULL_HANDLE; }
    if (s_dcVbuf) { vkDestroyBuffer(d->dev, s_dcVbuf, NULL); s_dcVbuf = VK_NULL_HANDLE; }
    if (s_dcVmem) { vkFreeMemory(d->dev, s_dcVmem, NULL); s_dcVmem = VK_NULL_HANDLE; }
}

/* The no-target Gamma's two pipelines and its quad. It needs nothing the
   target needs -- no depth format, no LINEAR filter, no linear colour format --
   so it builds on every device the target refuses. */
static int build_direct(const TAGPU_VKPASS* d)
{
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    int ok = 0;

    if (!resolve(d)) { plog(d, "world: an entry point is missing"); return 0; }
    /* NO SETS: KFS samples nothing, and the factor is the blend constants */
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_dcPlo) != VK_SUCCESS) return 0;
    if (!quad_buffer(d, &s_dcVbuf, &s_dcVmem)) return 0;
    vs = mk_module(d, tagpu_spv_tagpu_native_DVS,
                   sizeof tagpu_spv_tagpu_native_DVS / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_tagpu_native_KFS,
                   sizeof tagpu_spv_tagpu_native_KFS / sizeof(uint32_t));
    if (!vs || !fs) goto out;

    /* THE ALPHA IS NOT WRITTEN: the factor is a colour transfer, and the
       frame's alpha is the swapchain's, which nothing here reads. */
    memset(&cba, 0, sizeof cba);
    cba.blendEnable = VK_TRUE;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT;
    /* UP: 1.0 x frame + frame x (factor - 1) */
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
    if (quad_pipeline(d, s_dcPlo, vs, fs, &cba, 3, &s_dcUp) != VK_SUCCESS) goto out;
    /* DOWN: frame x factor */
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    if (quad_pipeline(d, s_dcPlo, vs, fs, &cba, 3, &s_dcDown) != VK_SUCCESS) goto out;
    ok = 1;
out:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

int tagpu_vk_world_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                           uint32_t* tw, uint32_t* th)
{
    TAGPU_WORLDTGT t;
    int w, h;

    s_drawThis = 0;
    s_gamOn = 0;
    s_openSlot = -1;
    s_drewSlot = -1;
    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    if (!tw || !th) return 0;

    /* PULLED, NOT HANDED. Every module here reads what it needs from the module
       that owns the answer; the seam passes a device, a command buffer and a
       slot and nothing else. `tagpu_native_worldtgt` publishes the ONE ss
       decision, so this is the same number the gather drew with. */
    if (!tagpu_native_worldtgt(&t)) return 0;
    /* NOT THIS FRAME'S, SO NOT OURS. The rule every hand-over in this tree
       carries: a target left standing from an earlier frame describes a
       geometry that has since moved. */
    if (t.frame != d->frame) return 0;

    /* BOUNDED BEFORE THE MULTIPLY, NOT AFTER IT. `t.gw` is `f->game_width`,
       which tagpu_native.c takes as `> 0 ? it : vpL + vw` and never bounds
       above -- it is the device MODE, so it is data until something validates
       it as data. Checking `gw * ss` against WORLD_MAXDIM afterwards is the
       classic version of this mistake: a large enough `gw` makes the product
       overflow a signed int, and an overflowed product can land back inside the
       bound and be handed to vkCreateImage. So each factor is bounded on its
       own first, and only then multiplied -- after which the product cannot
       exceed WORLD_MAXDIM * WORLD_SSMAX and cannot overflow.
       CLAUDE.md: a value is DATA until it has been validated as data. */
    if (t.gw < 1 || t.gw > WORLD_MAXDIM || t.gh < 1 || t.gh > WORLD_MAXDIM ||
        t.ss < 1 || t.ss > WORLD_SSMAX) {
        if (!s_saidBig) {
            s_saidBig = 1;
            plog(d, "world: %dx%d at ss=%d is outside what this module carries "
                    "(%d per edge, ss %d) - the world stays on the swapchain image",
                 t.gw, t.gh, t.ss, WORLD_MAXDIM, WORLD_SSMAX);
        }
        return 0;
    }
    w = t.gw * t.ss;
    h = t.gh * t.ss;
    if (w > WORLD_MAXDIM || h > WORLD_MAXDIM) {
        /* ITS OWN LATCH. Sharing one with the check above would let whichever
           refusal fired first silence the other for the life of the process,
           and they mean different things. */
        if (!s_saidBigProduct) {
            s_saidBigProduct = 1;
            plog(d, "world: a %dx%d target (%dx%d at ss=%d) is outside what this "
                    "module carries (%d) - the world stays on the swapchain image",
                 w, h, t.gw, t.gh, t.ss, WORLD_MAXDIM);
        }
        return 0;
    }
    if (t.vw < 1 || t.vh < 1) return 0;          /* nowhere to put the block */

    if (s_state == ST_UNBUILT) {
        if (!build(d)) {
            /* ASKED FOR, NOT TAKEN. `s_downOwed` is a REQUEST that the seam
               tear this module down once it has drained the device -- it is not
               "there is something to free", and setting it on the success path
               would make the seam destroy and rebuild the whole target every
               frame behind a vkDeviceWaitIdle. A partial build has
               allocated objects no submit names yet, but the drain is the
               contract and paying it here would fork the teardown into two
               shapes for no gain. */
            s_state = ST_REFUSED;
            s_downOwed = 1;
            return 0;
        }
        s_state = ST_READY;
    }
    if (!slot_size(d, &s_slot[slot], w, h)) {
        plog(d, "world: slot %u would not take a %dx%d target - the world stays "
                "on the swapchain image and the seam tears this down",
             (unsigned)slot, w, h);
        /* LATCHED, for tagpu_vk_surf.c's reason: a slot that will not size is a
           device that will not give us the memory, and retrying it every frame
           is a stall per frame rather than a recovery. */
        s_state = ST_REFUSED;
        s_downOwed = 1;
        return 0;
    }

    /* THE FACTOR, ONCE, ON THE FINISHED WORLD: every world colour source is
       the engine's unscaled table (tagpu_pal.h), so this is the only place the
       Gamma reaches the world. At 1.0 the plain composite draws, which is the
       picture with no curve at all rather than an identity curve's rounding. */
    {
        float f = tagpu_pal_gamma();
        s_gamOn = f != 1.0f && gamma_upload(cb, &s_gam[slot], f);
    }

    s_vx = t.vx; s_vy = t.vy; s_vw = t.vw; s_vh = t.vh;
    *tw = (uint32_t)w; *th = (uint32_t)h;
    s_drawThis = 1;
    s_nFrames++;
    s_lastW = w; s_lastH = h; s_lastSS = t.ss; s_lastDevres = t.devres;

    if (d->frame - s_saidAt >= 300) {
        s_saidAt = d->frame;
        /* `devres` IS REPORTED BECAUSE IT IS WHY `ss` CAN BE ABOVE 2: under it
           tagpu_native.c raises `ss` to ceil(k) so the target reaches the
           device resolution, and this draw composites straight out of it, as it
           does on every path. */
        plog(d, "world: frame %u: %dx%d target (%dx%d at ss=%d%s) -> (%d,%d %dx%d), "
                "%u frame(s), %u build(s)", d->frame, s_lastW, s_lastH,
             t.gw, t.gh, s_lastSS, s_lastDevres ? " devres" : "",
             s_vx, s_vy, s_vw, s_vh, s_nFrames, s_nBuilds);
    }
    return 1;
}

void tagpu_vk_world_begin(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue cv[2];

    if (s_state != ST_READY || !s_drawThis) return;
    if (slot >= TAGPU_VK_SLOTS || !s_slot[slot].fb) return;

    memset(cv, 0, sizeof cv);
    /* TRANSPARENT, and the header says why: the composite blends this over TA's
       own frame, so a pixel the world never touched must carry alpha 0. */
    cv[0].color.float32[0] = 0.0f; cv[0].color.float32[1] = 0.0f;
    cv[0].color.float32[2] = 0.0f; cv[0].color.float32[3] = 0.0f;
    cv[1].depthStencil.depth = 1.0f;
    cv[1].depthStencil.stencil = 0;

    rbi.renderPass = s_rp;
    rbi.framebuffer = s_slot[slot].fb;
    rbi.renderArea.extent.width = (uint32_t)s_slot[slot].w;
    rbi.renderArea.extent.height = (uint32_t)s_slot[slot].h;
    rbi.clearValueCount = 2;
    rbi.pClearValues = cv;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    s_openSlot = (int)slot;
    s_drewSlot = (int)slot;
    (void)d;
}

void tagpu_vk_world_end(const TAGPU_VKPASS* d, VkCommandBuffer cb)
{
    (void)d;
    /* ONLY WHAT `begin` ACTUALLY OPENED. The seam calls this unconditionally
       next to its own `end`, and a vkCmdEndRenderPass with no open pass is a
       command-buffer error rather than a no-op. */
    if (s_openSlot < 0) return;
    s_openSlot = -1;
    vkCmdEndRenderPass(cb);
}

void tagpu_vk_world_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                           uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = 0;
    int rx, ry, rw, rh;

    (void)d;
    if (s_state != ST_READY || !s_drawThis) return;
    s_drawThis = 0;
    if (slot >= TAGPU_VK_SLOTS || !s_slot[slot].w) return;

    /* THE SCISSOR IS CLAMPED TO THE RENDER AREA WE ARE HANDED, for
       tagpu_vk_surf.c's reason: the rect was published by the gather at the top
       of this iteration, and tagpu_vk_frame may have rebuilt the swapchain
       since, so on the frame a window shrinks the rect is the old viewport and
       `w`/`h` are the new extent. An unclamped scissor would lie partly outside
       the render area, which the spec leaves undefined.

       THE VIEWPORT IS NOT CLAMPED. It is where the whole block lands, and under
       HUD scale the block is translated so that its far edges hang past the
       window (tagpu_native.c, where the rect is published): clamping the
       viewport too would squeeze the world into what is left instead of
       clipping it, and every world pixel would drift from the pointer by a
       fraction of its distance from the origin. The block is at most the
       viewport plus the HUD shift, well inside the spec's viewport bounds. */
    if (s_vw < 1 || s_vh < 1) return;
    rx = s_vx < 0 ? 0 : s_vx;
    ry = s_vy < 0 ? 0 : s_vy;
    if (rx >= (int)w || ry >= (int)h) return;
    rw = s_vx + s_vw - rx; rh = s_vy + s_vh - ry;
    if (rx + rw > (int)w) rw = (int)w - rx;
    if (ry + rh > (int)h) rh = (int)h - ry;
    if (rw < 1 || rh < 1) return;

    /* NO FLIP, AND THE ARGUMENT NEEDS TO KNOW WHICH WAY UP NEITHER IMAGE IS.
       DVS pairs `uv = 0` with clip `y = -1`; under a POSITIVE height that sends
       source row 0 to destination row 0. The world passes put the game's top
       row at row 0 of the source for the same reason they put it at row 0 of
       the swapchain image, so the two agree and a negative height here would
       be a third turn. */
    vp.x = (float)s_vx;
    vp.y = (float)s_vy;
    vp.width = (float)s_vw;
    vp.height = (float)s_vh;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    sc.offset.x = rx; sc.offset.y = ry;
    sc.extent.width = (uint32_t)rw; sc.extent.height = (uint32_t)rh;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_gamOn ? s_pipeG : s_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1,
                            &s_slot[slot].dset, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_vbuf, &off);
    vkCmdDraw(cb, NV, 1, 0, 0);
}

static void direct_draw(VkCommandBuffer cb, VkPipeline pipe, float c)
{
    float k[4];
    k[0] = c; k[1] = c; k[2] = c; k[3] = 0.0f;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetBlendConstants(cb, k);
    vkCmdDraw(cb, NV, 1, 0, 0);
}

void tagpu_vk_world_record_direct(const TAGPU_VKPASS* d, VkCommandBuffer cb,
                                  uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = 0;
    float f = tagpu_pal_gamma();

    /* A FRAME WITH A TARGET TOOK THE CURVE IN `record`; applying the factor
       here as well would apply it twice. `prepare` decides both arms, and this
       arm is exactly the frames it returned 0 for. */
    if (s_drawThis) return;
    if (f == 1.0f || w < 1 || h < 1) return;
    if (s_dcState == ST_UNBUILT) {
        if (!build_direct(d)) {
            direct_free(d);
            s_dcState = ST_REFUSED;
            if (!s_dcSaid) {
                s_dcSaid = 1;
                plog(d, "world: the no-target Gamma pipelines were refused - "
                        "a frame without a world target shows the world at "
                        "factor 1.0");
            }
            return;
        }
        s_dcState = ST_READY;
        plog(d, "world: the no-target Gamma is up - the world passes drew into "
                "the frame, and the blend applies factor %.3f to it", (double)f);
    }
    if (s_dcState != ST_READY) return;

    /* THE WHOLE FRAME, because all it holds yet is the clear and the world:
       the UI is recorded after this, and nothing of the engine's is drawn
       under the world. */
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)w; vp.height = (float)h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = w; sc.extent.height = h;
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_dcVbuf, &off);

    /* UP ADDS AT MOST ONE FRAME'S WORTH A DRAW, so a factor above 2 doubles
       first. A doubling is exact until it clamps, and a level that has
       clamped stays clamped under every later multiply, so the result is
       the one clamp the curve applies. tagpu_pal_gamma bounds the factor at
       8, which is two doublings at most. */
    while (f > 2.0f) {
        direct_draw(cb, s_dcUp, 1.0f);
        f *= 0.5f;
    }
    if (f > 1.0f) direct_draw(cb, s_dcUp, f - 1.0f);
    else if (f < 1.0f) direct_draw(cb, s_dcDown, f);
}

/* The target and everything built with it -- not the no-target Gamma. */
static void target_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    if (!d || !d->dev) return;
    if (!vkDestroyImageView) return;        /* never resolved: nothing was made */
    for (i = 0; i < TAGPU_VK_SLOTS; i++) slot_free(d, &s_slot[i]);
    for (i = 0; i < TAGPU_VK_SLOTS; i++) gamma_free(d, &s_gam[i]);
    if (s_dpool) { vkDestroyDescriptorPool(d->dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_pipe)  { vkDestroyPipeline(d->dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    if (s_pipeG) { vkDestroyPipeline(d->dev, s_pipeG, NULL); s_pipeG = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(d->dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(d->dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_samp)  { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_rp)    { vkDestroyRenderPass(d->dev, s_rp, NULL); s_rp = VK_NULL_HANDLE; }
    if (s_vbuf)  { vkDestroyBuffer(d->dev, s_vbuf, NULL); s_vbuf = VK_NULL_HANDLE; }
    if (s_vmem)  { vkFreeMemory(d->dev, s_vmem, NULL); s_vmem = VK_NULL_HANDLE; }
    s_drawThis = 0;
    s_gamOn = 0;
    s_openSlot = -1;
    /* AND THE CAPTURE LATCH, so the invariant holds on its own rather than
       because `tagpu_vk_world_shot`'s image check happens to cover it. The
       header promises `s_drewSlot` is valid from `begin` until the next frame's
       `prepare`; a teardown between the two is exactly the gap that promise has
       to survive. */
    s_drewSlot = -1;
    s_colFmt = VK_FORMAT_UNDEFINED;
    if (s_state != ST_REFUSED) s_state = ST_UNBUILT;
    s_downOwed = 0;
    s_downPaying = 0;
}

void tagpu_vk_world_down(const TAGPU_VKPASS* d)
{
    if (!d || !d->dev) return;
    target_down(d);
    /* THE NO-TARGET GAMMA GOES WITH THE DEVICE (or the resize that rebuilds
       the render pass its pipelines were built against), and is rebuilt on the
       next frame that needs it. */
    direct_free(d);
    if (s_dcState != ST_REFUSED) s_dcState = ST_UNBUILT;
}

int tagpu_vk_world_scale(void) { return s_drawThis ? s_lastSS : 1; }

int tagpu_vk_world_shot(uint32_t slot, VkImage* img, uint32_t* w, uint32_t* h,
                        VkFormat* fmt)
{
    if (!img || !w || !h || !fmt) return 0;
    /* THE SLOT THE WORLD WENT INTO, AND NO OTHER. Every other slot's image is
       an earlier frame's picture, still allocated because the slots are
       long-lived -- so a capture read out of one would be a real image of the
       wrong frame, which is the single worst thing an oracle can hand back.
       `s_drewSlot` is set where the render pass is opened, so this is a
       question about the command buffer. */
    if (s_drewSlot < 0 || (uint32_t)s_drewSlot != slot) return 0;
    if (slot >= TAGPU_VK_SLOTS || !s_slot[slot].col ||
        s_slot[slot].w <= 0 || s_slot[slot].h <= 0) return 0;
    if (s_colFmt == VK_FORMAT_UNDEFINED) return 0;
    *img = s_slot[slot].col;
    *w = (uint32_t)s_slot[slot].w;
    *h = (uint32_t)s_slot[slot].h;
    *fmt = s_colFmt;
    return 1;
}

int tagpu_vk_world_down_owed(void) { return s_downOwed; }

void tagpu_vk_world_down_paid(const TAGPU_VKPASS* d)
{
    if (!s_downOwed || s_downPaying) return;
    s_downPaying = 1;
    /* THE TARGET'S DEBT ONLY: the refusal it settles is the case the
       no-target Gamma exists for, so its pipelines stay up. */
    target_down(d);
}
