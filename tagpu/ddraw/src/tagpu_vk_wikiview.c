/* The tech tree's unit view's picture. The header carries the argument; this is the mechanism.
   The target, its render pass and the composite are tagpu_vk_world.c's, at the size of the view;
   the atlas upload is tagpu_vk_unit.c base_upload's. */

#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_wikiview.h"
#include "tagpu_vk_stage.h"
#include "tagpu_wikiview.h"
#include "tagpu_wikistate.h"
#include "spirv/tagpu_native.spv.h"        /* DVS / DFS: the world target's composite */
#include "spirv/tagpu_wikiview.spv.h"      /* VS / FS: the view's program */

#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties)

#define DFNS(X) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

#define NV        4            /* the composite's unit square, as a strip                         */
/* THE TARGET IS THE VIEW TWICE OVER EACH WAY, filtered down by the composite's LINEAR tap -- at
   exactly 2:1 a destination pixel's tap is its 2 x 2 block's average (tagpu_vk_world.c
   build_sampler) -- which smooths the model's edges as the mockup's antialiased canvas does */
#define SS        2
/* the view's edge, device pixels, bounded before the multiply: a rect that reached us from the
   layout's arithmetic is DATA until bounded, and this module allocates from it */
#define VIEW_MAX  4096
#define ATLAS_MAX 4096         /* the atlas's edge the copy is made for (tagpu_render3do.c: 2048) */
#define VOFF      256          /* the slot buffer: the camera's block, then the vertices         */
#define VSTRIDE   (TAGPU_WV_STRIDE * sizeof(float))

static int s_state, s_downOwed, s_downPaying;
static int s_drawThis, s_drawSlot = -1;
static int s_vr[4], s_vc[4];                  /* this frame's view rect and its clip, x0 y0 x1 y1 */
static unsigned s_saidAt;

static VkRenderPass          s_rp;            /* the OFFSCREEN pass                              */
static VkDescriptorSetLayout s_mdsl, s_cdsl;  /* the model's (camera, atlas), the composite's     */
static VkPipelineLayout      s_mplo, s_cplo;
static VkPipeline            s_mpipe;         /* the model, against s_rp                         */
static VkPipeline            s_cpipe;         /* the composite, against the seam's d->rp         */
static VkDescriptorPool      s_dpool;
static VkSampler             s_nearest, s_linear;
static VkBuffer              s_qbuf;
static VkDeviceMemory        s_qmem;

/* THE ATLAS'S COPY, one image every slot samples: each upload is recorded in the frame's own
   command buffer behind a barrier on what earlier frames sampled (tagpu_vk_stage.h `had`) */
static VkImage               s_aImg;
static VkDeviceMemory        s_aMem;
static VkImageView           s_aView;
static int                   s_aDim, s_aHave;
static unsigned              s_aSerial, s_aPal;
static int                   s_rect[TAGPU_VK_STAGE_MAXRECT][4];

typedef struct {
    VkImage         col, dep;
    VkDeviceMemory  cmem, dmem;
    VkImageView     cview, dview;
    VkFramebuffer   fb;
    int             w, h;                     /* the target, 0 = none                            */
    VkBuffer        buf;                      /* host-visible: the camera at 0, vertices at VOFF */
    VkDeviceMemory  bmem;
    unsigned char*  bmap;
    VkDeviceSize    bcap;
    VkDescriptorSet mset, cset;
    TAGPU_VKSTAGE   stage;
} SLOT;
static SLOT s_slot[TAGPU_VK_SLOTS];

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
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
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

/* One image and its view: a colour target (sampled by the composite), a depth target, or the
   atlas's copy (written by copies, sampled by the model) */
enum { IMG_COL, IMG_DEP, IMG_ATLAS };
static int mk_image(const TAGPU_VKPASS* d, int w, int h, VkFormat fmt, int kind,
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
    ici.usage = kind == IMG_DEP ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
              : kind == IMG_COL ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
              : (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, *img, &req);
    if ((type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, *img, *mem, 0) != VK_SUCCESS) return 0;
    ivi.image = *img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    /* a depth format's stencil plane is named with it, or the framebuffer is refused */
    ivi.subresourceRange.aspectMask =
        kind == IMG_DEP ? (VK_IMAGE_ASPECT_DEPTH_BIT |
                           ((fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                             fmt == VK_FORMAT_D16_UNORM_S8_UINT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                        : VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    return vkCreateImageView(d->dev, &ivi, NULL, view) == VK_SUCCESS;
}

static void target_free(const TAGPU_VKPASS* d, SLOT* s)
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

static void buf_free(const TAGPU_VKPASS* d, SLOT* s)
{
    if (s->bmap)  { vkUnmapMemory(d->dev, s->bmem); s->bmap = NULL; }
    if (s->buf)   { vkDestroyBuffer(d->dev, s->buf, NULL); s->buf = VK_NULL_HANDLE; }
    if (s->bmem)  { vkFreeMemory(d->dev, s->bmem, NULL); s->bmem = VK_NULL_HANDLE; }
    s->bcap = 0;
}

/* Slot `s`'s target at `w` x `h`, its composite's set pointed at the new image. Safe because the
   seam waited on this slot's fence: the submit that last drew and sampled these has completed. */
static int target_size(const TAGPU_VKPASS* d, SLOT* s, int w, int h)
{
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkImageView att[2];
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    if (s->w == w && s->h == h) return 1;
    target_free(d, s);
    if (!mk_image(d, w, h, d->fmt, IMG_COL, &s->col, &s->cmem, &s->cview) ||
        !mk_image(d, w, h, d->dfmt, IMG_DEP, &s->dep, &s->dmem, &s->dview)) { target_free(d, s); return 0; }
    att[0] = s->cview; att[1] = s->dview;
    fci.renderPass = s_rp;
    fci.attachmentCount = 2;
    fci.pAttachments = att;
    fci.width = (uint32_t)w;
    fci.height = (uint32_t)h;
    fci.layers = 1;
    if (vkCreateFramebuffer(d->dev, &fci, NULL, &s->fb) != VK_SUCCESS) { target_free(d, s); return 0; }
    /* the set follows the image, written here and nowhere else (tagpu_vk_world.c slot_size) */
    memset(&ii, 0, sizeof ii); memset(&wr, 0, sizeof wr);
    ii.sampler = s_linear;
    ii.imageView = s->cview;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = s->cset; wr.dstBinding = 40; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    s->w = w; s->h = h;
    return 1;
}

/* Slot `s`'s buffer holding at least `bytes`, mapped for its life, its model set's camera binding
   pointed at it. The same fence argument as target_size. */
static int buf_size(const TAGPU_VKPASS* d, SLOT* s, VkDeviceSize bytes)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    VkDescriptorBufferInfo bi;
    VkWriteDescriptorSet wr;
    void* p = NULL;
    int type;
    if (s->buf && s->bcap >= bytes) return 1;
    buf_free(d, s);
    bytes = (bytes + 0xFFFF) & ~(VkDeviceSize)0xFFFF;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, &s->buf) != VK_SUCCESS) { buf_free(d, s); return 0; }
    vkGetBufferMemoryRequirements(d->dev, s->buf, &req);
    type = mem_type(d, req.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || vkAllocateMemory(d->dev, &mai, NULL, &s->bmem) != VK_SUCCESS ||
        vkBindBufferMemory(d->dev, s->buf, s->bmem, 0) != VK_SUCCESS ||
        vkMapMemory(d->dev, s->bmem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) { buf_free(d, s); return 0; }
    s->bmap = (unsigned char*)p;
    s->bcap = bytes;
    memset(&bi, 0, sizeof bi); memset(&wr, 0, sizeof wr);
    bi.buffer = s->buf; bi.offset = 0; bi.range = 64;
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = s->mset; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr.pBufferInfo = &bi;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    return 1;
}

/* the offscreen pass: tagpu_vk_world.c build_renderpass's, the same attachments, ops, layouts and
   the two global dependencies its comments argue -- the composite samples this image later in the
   same command buffer, at scaled coordinates, in another render pass */
static int build_renderpass(const TAGPU_VKPASS* d)
{
    VkAttachmentDescription at[2];
    VkAttachmentReference cr, dr;
    VkSubpassDescription sp;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    memset(at, 0, sizeof at);
    at[0].format = d->fmt;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1].format = d->dfmt;
    at[1].samples = VK_SAMPLE_COUNT_1_BIT;
    at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
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
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    rpi.attachmentCount = 2;
    rpi.pAttachments = at;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sp;
    rpi.dependencyCount = 2;
    rpi.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &rpi, NULL, &s_rp) == VK_SUCCESS;
}

static int build_samplers(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties fp;
    /* the world target's two refusals, for its reasons (tagpu_vk_world.c build_sampler) */
    if (d->fmt == VK_FORMAT_B8G8R8A8_SRGB || d->fmt == VK_FORMAT_R8G8B8A8_SRGB ||
        d->fmt == VK_FORMAT_A8B8G8R8_SRGB_PACK32) {
        plog(d, "wikiview: the surface gave an sRGB format (%d) - the unit view is not drawn", (int)d->fmt);
        return 0;
    }
    vkGetPhysicalDeviceFormatProperties(d->pd, d->fmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
        !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        plog(d, "wikiview: the device will not both render to and filter format %d - the unit view is not "
                "drawn", (int)d->fmt);
        return 0;
    }
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d->dev, &sci, NULL, &s_linear) != VK_SUCCESS) return 0;
    /* THE ATLAS NEAREST, as the unit pass samples it (tagpu_vk_unit.c build_samplers): a texel is a
       palette entry's colour and a key texel's alpha is 0, and a filtered tap would blend both */
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    return vkCreateSampler(d->dev, &sci, NULL, &s_nearest) == VK_SUCCESS;
}

static int build_layouts(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[2];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    /* the generated header's bindings: the vertex stage's block at 0, the fragment's sampler at 40 */
    memset(b, 0, sizeof b);
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 40;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dli.bindingCount = 2; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_mdsl) != VK_SUCCESS) return 0;
    dli.bindingCount = 1; dli.pBindings = &b[1];
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_cdsl) != VK_SUCCESS) return 0;
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_mdsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_mplo) != VK_SUCCESS) return 0;
    pli.pSetLayouts = &s_cdsl;
    return vkCreatePipelineLayout(d->dev, &pli, NULL, &s_cplo) == VK_SUCCESS;
}

/* One pipeline of this pass: the model (`model` 1: the view's stream, depth tested, back faces
   culled, opaque, against the offscreen pass) or the composite (DVS's unit square, no depth, no
   cull, premultiplied over the frame, against the seam's pass). */
static VkResult pipeline(const TAGPU_VKPASS* d, int model, VkShaderModule vs, VkShaderModule fs, VkPipeline* out)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[6];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;
    /* the stream's floats (tagpu_wikiview.h): position, uv, colour, quad point, shape, rect */
    static const struct { uint32_t loc; VkFormat f; uint32_t off; } A[6] = {
        { 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },     { 1, VK_FORMAT_R32G32_SFLOAT, 12 },
        { 2, VK_FORMAT_R32G32B32_SFLOAT, 20 },    { 3, VK_FORMAT_R32G32_SFLOAT, 32 },
        { 4, VK_FORMAT_R32G32_SFLOAT, 40 },       { 5, VK_FORMAT_R32G32B32A32_SFLOAT, 48 } };
    int i;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    memset(&vb, 0, sizeof vb);
    memset(va, 0, sizeof va);
    vb.binding = 0;
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    if (model) {
        vb.stride = (uint32_t)VSTRIDE;
        for (i = 0; i < 6; i++) { va[i].location = A[i].loc; va[i].format = A[i].f; va[i].offset = A[i].off; }
        vi.vertexAttributeDescriptionCount = 6;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    } else {
        vb.stride = 2 * sizeof(float);
        va[0].format = VK_FORMAT_R32G32_SFLOAT;
        vi.vertexAttributeDescriptionCount = 1;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    }
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.pVertexAttributeDescriptions = va;
    vp.viewportCount = 1; vp.scissorCount = 1;

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* BACK FACES CULLED, as unitview.js culls them and the game's body pipeline does: front is
       counter-clockwise as the picture shows it, which the negative-height viewport keeps */
    rs.cullMode = model ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = model ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = model ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = model ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_ALWAYS;

    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (!model) {
        /* PREMULTIPLIED over the frame, from a target cleared to {0,0,0,0}: where the model is not,
           the view's ground shows (tagpu_vk_world.c build_pipeline) */
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = model ? s_mplo : s_cplo;
    gp.renderPass = model ? s_rp : d->rp;
    gp.subpass = 0;
    return vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, out);
}

static int build_pipelines(const TAGPU_VKPASS* d)
{
    VkShaderModule mvs, mfs, cvs, cfs;
    int ok;
    mvs = mk_module(d, tagpu_spv_tagpu_wikiview_VS, sizeof tagpu_spv_tagpu_wikiview_VS / sizeof(uint32_t));
    mfs = mk_module(d, tagpu_spv_tagpu_wikiview_FS, sizeof tagpu_spv_tagpu_wikiview_FS / sizeof(uint32_t));
    cvs = mk_module(d, tagpu_spv_tagpu_native_DVS, sizeof tagpu_spv_tagpu_native_DVS / sizeof(uint32_t));
    cfs = mk_module(d, tagpu_spv_tagpu_native_DFS, sizeof tagpu_spv_tagpu_native_DFS / sizeof(uint32_t));
    ok = mvs && mfs && cvs && cfs &&
         pipeline(d, 1, mvs, mfs, &s_mpipe) == VK_SUCCESS && pipeline(d, 0, cvs, cfs, &s_cpipe) == VK_SUCCESS;
    if (mvs) vkDestroyShaderModule(d->dev, mvs, NULL);
    if (mfs) vkDestroyShaderModule(d->dev, mfs, NULL);
    if (cvs) vkDestroyShaderModule(d->dev, cvs, NULL);
    if (cfs) vkDestroyShaderModule(d->dev, cfs, NULL);
    return ok;
}

static const float QUAD[NV * 2] = { 0.f,0.f,  1.f,0.f,  0.f,1.f,  1.f,1.f };

static int build_quad(const TAGPU_VKPASS* d)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    void* p = NULL;
    int type;
    bci.size = sizeof QUAD;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, &s_qbuf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(d->dev, s_qbuf, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s_qmem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(d->dev, s_qbuf, s_qmem, 0) != VK_SUCCESS) return 0;
    if (vkMapMemory(d->dev, s_qmem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
    memcpy(p, QUAD, sizeof QUAD);
    vkUnmapMemory(d->dev, s_qmem);
    return 1;
}

/* the atlas's copy at `dim` and every slot's sets: the model's atlas binding names the one image,
   the camera and the target are written as their buffer and image are made */
static int build_descriptors(const TAGPU_VKPASS* d, int dim)
{
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    uint32_t i;
    if (!mk_image(d, dim, dim, VK_FORMAT_R8G8B8A8_UNORM, IMG_ATLAS, &s_aImg, &s_aMem, &s_aView)) return 0;
    s_aDim = dim;
    s_aHave = 0;
    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[0].descriptorCount = d->slots;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = d->slots * 2;
    dpi.maxSets = d->slots * 2;
    dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) return 0;
    for (i = 0; i < d->slots; i++) {
        dai.descriptorPool = s_dpool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &s_mdsl;
        if (vkAllocateDescriptorSets(d->dev, &dai, &s_slot[i].mset) != VK_SUCCESS) return 0;
        dai.pSetLayouts = &s_cdsl;
        if (vkAllocateDescriptorSets(d->dev, &dai, &s_slot[i].cset) != VK_SUCCESS) return 0;
        memset(&ii, 0, sizeof ii); memset(&wr, 0, sizeof wr);
        ii.sampler = s_nearest;
        ii.imageView = s_aView;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = s_slot[i].mset; wr.dstBinding = 40; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &ii;
        vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    }
    return 1;
}

static int build(const TAGPU_VKPASS* d, int dim)
{
    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) return 0;
    if (d->dfmt == VK_FORMAT_UNDEFINED) {
        plog(d, "wikiview: the seam found no depth format - the unit view is not drawn");
        return 0;
    }
    if (!resolve(d)) { plog(d, "wikiview: an entry point is missing"); return 0; }
    if (!build_renderpass(d) || !build_samplers(d) || !build_layouts(d) || !build_pipelines(d) ||
        !build_quad(d) || !build_descriptors(d, dim)) {
        plog(d, "wikiview: the unit view's pass was refused - the unit view is not drawn");
        return 0;
    }
    plog(d, "wikiview: the unit view's pass is up - %u frame slots, a %dx%d atlas copy", (unsigned)d->slots, dim, dim);
    return 1;
}

/* THE ATLAS'S UPLOAD, base_upload's two ways: the tiles written since this copy's serial in the
   frame's command buffer while the copy is kept, else the whole page banded -- and the whole page
   too when the tiles due pass what one frame sends, which a copy left behind while the stage's
   unit pass was the atlas's consumer can be (tagpu_wikiview.h). 1 the copy is this frame's, 0 not
   this frame (no staging: it goes on the next), -1 the device failed. */
static int atlas_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, SLOT* s, const TAGPU_WIKIVIEWHAND* h)
{
    const TAGPU_GAFVIEW* a = &h->atlas;
    int rows = a->rows, n, keep, rc;
    VkDeviceSize bytes;
    if (s_aHave && s_aSerial == a->serial && s_aPal == h->palSerial) {
        tagpu_vk_stage_drop(d, &s->stage);
        tagpu_wikiview_atlas_ack(s_aSerial, 1);
        return 1;
    }
    if (rows < 1) rows = 1;
    if (rows > a->dim) rows = a->dim;
    keep = s_aHave && s_aPal == h->palSerial && (int)(a->whole - s_aSerial) <= 0;
    n = tagpu_gaf_rects_due(a->dirty, a->serial, s_aSerial, keep, a->dim, rows, s_rect, TAGPU_VK_STAGE_MAXRECT);
    bytes = (VkDeviceSize)tagpu_gaf_rects_area((const int (*)[4])s_rect, n) * 4;
    if (keep && bytes > TAGPU_VK_STAGE_CAP) {
        keep = 0;
        n = tagpu_gaf_rects_due(a->dirty, a->serial, s_aSerial, 0, a->dim, rows, s_rect, TAGPU_VK_STAGE_MAXRECT);
        bytes = (VkDeviceSize)tagpu_gaf_rects_area((const int (*)[4])s_rect, n) * 4;
    }
    if (keep) {
        if (n > 0 && (!tagpu_vk_stage_begin(d, &s->stage, bytes, (VkDeviceSize)a->dim * 4) ||
                      !tagpu_vk_stage_fits(&s->stage, bytes)))
            return 0;
        if (n > 0 && !tagpu_vk_stage_expand_rects(d, cb, &s->stage, s_aImg, s_aHave, a->idx, a->key, a->dim,
                                                  (const int (*)[4])s_rect, n, h->pal, NULL))
            return 0;
        if (n == 0) tagpu_vk_stage_drop(d, &s->stage);
    } else {
        if (!tagpu_vk_stage_begin(d, &s->stage, bytes, (VkDeviceSize)a->dim * 4)) return 0;
        rc = tagpu_vk_stage_expand(d, cb, &s->stage, s_aImg, s_aHave, a->idx, a->key, a->dim, 0, 0, a->dim, rows,
                                   h->pal, NULL);
        if (rc <= 0) return rc;
    }
    s_aSerial = a->serial;
    s_aPal = h->palSerial;
    s_aHave = 1;
    tagpu_wikiview_atlas_ack(s_aSerial, 1);
    return 1;
}

/* unitview.js `mat`, column-major for the shader's mat4: the orbit about the fit's centre at the
   record's yaw, pitch and distance, and its perspective -- with Vulkan's depth range, 0 at the near
   plane and 1 at the far one, where WebGL's is -1 to 1 (the same depth order) */
static void camera(const TAGPU_WIKIVIEWHAND* h, float aspect, float* out)
{
    const float dist = h->dist * h->zoom, n = dist * 0.05f, fr = dist * 10.0f, f = 1.0f / tanf(0.45f);
    const float cy = cosf(h->yaw), sy = sinf(h->yaw), cp = cosf(h->pitch), sp = sinf(h->pitch);
    float z[3], x[3], y[3], e[3], V[4][4], P[4][4], xl;
    int i, j, k;
    z[0] = cp * sy; z[1] = sp; z[2] = cp * cy;
    for (k = 0; k < 3; k++) e[k] = h->c[k] + dist * z[k];
    x[0] = z[2]; x[1] = 0.0f; x[2] = -z[0];
    xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
    if (xl > 0.0f) { x[0] /= xl; x[2] /= xl; } else { x[0] = 1.0f; x[2] = 0.0f; }
    y[0] = z[1] * x[2] - z[2] * x[1];
    y[1] = z[2] * x[0] - z[0] * x[2];
    y[2] = z[0] * x[1] - z[1] * x[0];
    memset(V, 0, sizeof V);
    memset(P, 0, sizeof P);
    for (k = 0; k < 3; k++) { V[0][k] = x[k]; V[1][k] = y[k]; V[2][k] = z[k]; }
    V[0][3] = -(x[0] * e[0] + x[1] * e[1] + x[2] * e[2]);
    V[1][3] = -(y[0] * e[0] + y[1] * e[1] + y[2] * e[2]);
    V[2][3] = -(z[0] * e[0] + z[1] * e[1] + z[2] * e[2]);
    V[3][3] = 1.0f;
    P[0][0] = f / aspect;
    P[1][1] = f;
    P[2][2] = fr / (n - fr);
    P[2][3] = fr * n / (n - fr);
    P[3][2] = -1.0f;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += P[i][k] * V[k][j];
            out[j * 4 + i] = s;
        }
}

int tagpu_vk_wikiview_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_WIKIVIEWHAND h;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue cv[2];
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = VOFF;
    SLOT* s;
    int vw, vh, rc;

    s_drawThis = 0;
    s_drawSlot = -1;
    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    if (!tagpu_wikiview_hand(d->frame, &h) || !tagpu_wikistate_uview(d->frame, s_vr, s_vc)) return 0;
    if (h.nvert <= 0 || h.nvert > TAGPU_WV_MAXVERT || !h.pal) return 0;
    vw = s_vr[2] - s_vr[0];
    vh = s_vr[3] - s_vr[1];
    if (vw < 1 || vh < 1 || vw > VIEW_MAX || vh > VIEW_MAX) return 0;
    if (h.atlas.dim < 1 || h.atlas.dim > ATLAS_MAX) return 0;

    if (s_state == ST_UNBUILT) {
        if (!build(d, h.atlas.dim)) {
            /* a REQUEST that the seam tear this down behind its drain (tagpu_vk_world.c `prepare`) */
            s_state = ST_REFUSED;
            s_downOwed = 1;
            return 0;
        }
        s_state = ST_READY;
    }
    if (h.atlas.dim != s_aDim) return 0;
    s = &s_slot[slot];

    rc = atlas_upload(d, cb, s, &h);
    if (rc < 0) {
        plog(d, "wikiview: the atlas's upload failed on the device - the pass comes down");
        s_state = ST_REFUSED;
        s_downOwed = 1;
        return 0;
    }
    if (!s_aHave) return 0;
    if (!buf_size(d, s, VOFF + (VkDeviceSize)h.nvert * VSTRIDE) || !target_size(d, s, vw * SS, vh * SS)) {
        plog(d, "wikiview: slot %u would not take a %dx%d view - the pass comes down", (unsigned)slot, vw, vh);
        s_state = ST_REFUSED;
        s_downOwed = 1;
        return 0;
    }
    camera(&h, (float)vw / (float)vh, (float*)s->bmap);
    memcpy(s->bmap + VOFF, h.vert, (size_t)h.nvert * VSTRIDE);

    /* THE VIEW'S OWN PASS, whole, before the frame's: transparent where the model is not */
    memset(cv, 0, sizeof cv);
    cv[1].depthStencil.depth = 1.0f;
    rbi.renderPass = s_rp;
    rbi.framebuffer = s->fb;
    rbi.renderArea.extent.width = (uint32_t)s->w;
    rbi.renderArea.extent.height = (uint32_t)s->h;
    rbi.clearValueCount = 2;
    rbi.pClearValues = cv;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    /* Y UP, by a negative-height viewport (tools/spirv-gen.py, THE Y FLIP): the shader keeps GL's
       convention, and the picture stands as WebGL's does */
    vp.x = 0.0f; vp.y = (float)s->h;
    vp.width = (float)s->w; vp.height = -(float)s->h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = (uint32_t)s->w; sc.extent.height = (uint32_t)s->h;
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mpipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mplo, 0, 1, &s->mset, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s->buf, &off);
    vkCmdDraw(cb, (uint32_t)h.nvert, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    s_drawThis = 1;
    s_drawSlot = (int)slot;
    if (d->frame - s_saidAt >= 300u) {
        s_saidAt = d->frame;
        plog(d, "wikiview: frame %u: %d vertices into a %dx%d target -> (%d,%d %dx%d)", d->frame, h.nvert, s->w,
             s->h, s_vr[0], s_vr[1], vw, vh);
    }
    return 1;
}

int tagpu_vk_wikiview_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot, uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = 0;
    int x0, y0, x1, y1;
    (void)d;
    if (!s_drawThis || s_state != ST_READY || s_drawSlot != (int)slot) return 0;
    s_drawThis = 0;
    /* the scissor: the view, inside the panel's clip, inside the attachment */
    x0 = s_vr[0] > s_vc[0] ? s_vr[0] : s_vc[0];
    y0 = s_vr[1] > s_vc[1] ? s_vr[1] : s_vc[1];
    x1 = s_vr[2] < s_vc[2] ? s_vr[2] : s_vc[2];
    y1 = s_vr[3] < s_vc[3] ? s_vr[3] : s_vc[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)w) x1 = (int)w;
    if (y1 > (int)h) y1 = (int)h;
    if (x1 <= x0 || y1 <= y0) return 0;
    vp.x = (float)s_vr[0]; vp.y = (float)s_vr[1];
    vp.width = (float)(s_vr[2] - s_vr[0]); vp.height = (float)(s_vr[3] - s_vr[1]);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    sc.offset.x = x0; sc.offset.y = y0;
    sc.extent.width = (uint32_t)(x1 - x0); sc.extent.height = (uint32_t)(y1 - y0);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_cpipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_cplo, 0, 1, &s_slot[slot].cset, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_qbuf, &off);
    vkCmdDraw(cb, NV, 1, 0, 0);
    return 1;
}

static void teardown(const TAGPU_VKPASS* d)
{
    uint32_t i;
    if (!d || !d->dev || !vkDestroyImageView) return;   /* never resolved: nothing was made */
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        target_free(d, &s_slot[i]);
        buf_free(d, &s_slot[i]);
        tagpu_vk_stage_drop(d, &s_slot[i].stage);
        s_slot[i].mset = s_slot[i].cset = VK_NULL_HANDLE;
    }
    if (s_dpool)   { vkDestroyDescriptorPool(d->dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_aView)   { vkDestroyImageView(d->dev, s_aView, NULL); s_aView = VK_NULL_HANDLE; }
    if (s_aImg)    { vkDestroyImage(d->dev, s_aImg, NULL); s_aImg = VK_NULL_HANDLE; }
    if (s_aMem)    { vkFreeMemory(d->dev, s_aMem, NULL); s_aMem = VK_NULL_HANDLE; }
    if (s_mpipe)   { vkDestroyPipeline(d->dev, s_mpipe, NULL); s_mpipe = VK_NULL_HANDLE; }
    if (s_cpipe)   { vkDestroyPipeline(d->dev, s_cpipe, NULL); s_cpipe = VK_NULL_HANDLE; }
    if (s_mplo)    { vkDestroyPipelineLayout(d->dev, s_mplo, NULL); s_mplo = VK_NULL_HANDLE; }
    if (s_cplo)    { vkDestroyPipelineLayout(d->dev, s_cplo, NULL); s_cplo = VK_NULL_HANDLE; }
    if (s_mdsl)    { vkDestroyDescriptorSetLayout(d->dev, s_mdsl, NULL); s_mdsl = VK_NULL_HANDLE; }
    if (s_cdsl)    { vkDestroyDescriptorSetLayout(d->dev, s_cdsl, NULL); s_cdsl = VK_NULL_HANDLE; }
    if (s_nearest) { vkDestroySampler(d->dev, s_nearest, NULL); s_nearest = VK_NULL_HANDLE; }
    if (s_linear)  { vkDestroySampler(d->dev, s_linear, NULL); s_linear = VK_NULL_HANDLE; }
    if (s_rp)      { vkDestroyRenderPass(d->dev, s_rp, NULL); s_rp = VK_NULL_HANDLE; }
    if (s_qbuf)    { vkDestroyBuffer(d->dev, s_qbuf, NULL); s_qbuf = VK_NULL_HANDLE; }
    if (s_qmem)    { vkFreeMemory(d->dev, s_qmem, NULL); s_qmem = VK_NULL_HANDLE; }
    /* the copy is gone: the atlas is told, so its allowance stops counting against a copy no one
       holds and the next upload is the whole page */
    if (s_aHave) tagpu_wikiview_atlas_ack(0, 0);
    s_aHave = 0;
    s_aDim = 0;
    s_drawThis = 0;
    s_drawSlot = -1;
    s_downOwed = 0;
    s_downPaying = 0;
}

void tagpu_vk_wikiview_down(const TAGPU_VKPASS* d)
{
    teardown(d);
    /* a refusal's latch holds for one device and one swapchain; what replaces them may well have
       the memory */
    s_state = ST_UNBUILT;
}

int tagpu_vk_wikiview_down_owed(void) { return s_downOwed; }

void tagpu_vk_wikiview_down_paid(const TAGPU_VKPASS* d)
{
    if (!s_downOwed || s_downPaying) return;
    s_downPaying = 1;
    teardown(d);
}
