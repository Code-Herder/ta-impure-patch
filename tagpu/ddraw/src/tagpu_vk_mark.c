/* tagpu_vk_mark.c -- the UI MARKERS, drawn by Vulkan.
   Contract: tagpu_vk_mark.h. Phase G, the Vulkan-only plan's landing 5.

   WHAT THE ORACLE IS. `tagpu_mark_render` in tagpu_mark.c, and this pass is fed
   by the DRAW LIST that function records as it issues its draws
   (`tagpu_mark_handover`). The list matters more here than in any pass before
   it: this is seven draws, not one, and they differ only in `uText`, `uFog` and
   which texture feeds `uLayer` -- the order markers and their labels first, then
   the bars over them, then the group digit, then the captured layer, and the
   build cursor last of all with fog OFF because the engine draws it after the
   fog overlay and never darkens it. Every one of those is a decision the GL
   pass made from state this side cannot see, so it is carried rather than
   re-derived.

   NO DEPTH, NO BLEND, and that is the twin's rule rather than a shortcut:
   tagpu_mark.h says the pass "expects depth test and blending OFF (the markers
   are the frame's top layer and every fragment is opaque)". A pipeline that
   blended here would be a different picture on every anti-aliased glyph edge.

   THE LINES ARE `ss` DEVICE PIXELS WIDE, which is one SCREEN pixel at any
   supersample -- the same rule tagpu_vk_fx.c follows, and the reason a marker
   stays a hairline at 4x instead of a 1997 pixel blown up to sixteen. Wide
   lines are an optional feature in Vulkan, so a frame with order lines on a
   device that will not draw them is REFUSED rather than drawn thin. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "tagpu_vk_mark.h"
#include "tagpu_mark.h"
#include "spirv/tagpu_mark.spv.h"

#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties)

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
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
    X(vkCmdSetLineWidth)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

/* the std140 blocks the generated header prints, and the only place these
   numbers are written down in this file */
#define VS_SZ      32
#define VS_GAME     0
#define VS_ZOOM     8
#define VS_ZOOMC   16

#define FS_SZ      32
#define FS_KEY      0
#define FS_TEXT     4
#define FS_FOGORG   8
#define FS_FOGDIM  16
#define FS_FOG     24

#define MVST       7           /* floats a vertex: x,y u,v wx,wz colour */
#define MK_VSTRIDE (MVST * 4)

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };
enum { IMG_LAYER = 0, IMG_TEXT, IMG_PAL, IMG_FOG, IMG_LUT, IMG_N };

static int s_state;
static int s_downOwed, s_downPaying;
static int s_drawThis;

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipeTri, s_pipeLine;
static VkRenderPass          s_pipeRp;
static VkDescriptorPool      s_pool;
static VkDescriptorSet       s_set[TAGPU_VK_SLOTS];
static VkSampler             s_samp;          /* NEAREST: every texel here is an
                                                 index or a coverage byte */

/* the five sampled images. `w`/`h` are what they were built for, so a change of
   extent rebuilds -- safe because these are per-pass images no other slot
   names, and the seam's fence for this slot has already been waited on. */
typedef struct {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    int            w, h;
    VkFormat       fmt;
    unsigned       gen;        /* the source's own serial, when it has one */
    int            have;
} IMG;
static IMG s_img[IMG_N];

static VkBuffer       s_ubo[TAGPU_VK_SLOTS];
static VkDeviceMemory s_uboMem[TAGPU_VK_SLOTS];
static unsigned char* s_uboMap[TAGPU_VK_SLOTS];
static VkDeviceSize   s_uboCap[TAGPU_VK_SLOTS];
static VkBuffer       s_vb[TAGPU_VK_SLOTS];
static VkDeviceMemory s_vbMem[TAGPU_VK_SLOTS];
static unsigned char* s_vbMap[TAGPU_VK_SLOTS];
static VkDeviceSize   s_vbCap[TAGPU_VK_SLOTS];
static VkBuffer       s_stg[TAGPU_VK_SLOTS];
static VkDeviceMemory s_stgMem[TAGPU_VK_SLOTS];
static unsigned char* s_stgMap[TAGPU_VK_SLOTS];
static VkDeviceSize   s_stgCap[TAGPU_VK_SLOTS];

static TAGPU_MKHAND s_h;
static VkDeviceSize s_fsOff[TAGPU_MK_MAXDRAW];
static VkDeviceSize s_ualign = 256;
static uint32_t s_slot;
static int s_abFrame;
static int s_saidLine, s_saidFog, s_saidRoom;

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
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int mk_buffer(const TAGPU_VKPASS* d, VkDeviceSize size, VkBufferUsageFlags use,
                     VkBuffer* buf, VkDeviceMemory* mem, unsigned char** map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    int type;
    void* p = NULL;
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
    if (map) *map = NULL;
    bci.size = size;
    bci.usage = use;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, buf) != VK_SUCCESS) { *buf = VK_NULL_HANDLE; return 0; }
    vkGetBufferMemoryRequirements(d->dev, *buf, &req);
    type = mem_type(d, req.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) goto bad;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) { *mem = VK_NULL_HANDLE; goto bad; }
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) goto bad;
    if (map && vkMapMemory(d->dev, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) goto bad;
    if (map) *map = (unsigned char*)p;
    return 1;
bad:
    /* every out-param NULL from every exit -- the contract the gate-3a
       verification pass found missing in the unit pass's helper */
    if (*mem) vkFreeMemory(d->dev, *mem, NULL);
    if (*buf) vkDestroyBuffer(d->dev, *buf, NULL);
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
    if (map) *map = NULL;
    return 0;
}

static void kill_img(const TAGPU_VKPASS* d, IMG* im)
{
    if (im->view) vkDestroyImageView(d->dev, im->view, NULL);
    if (im->img)  vkDestroyImage(d->dev, im->img, NULL);
    if (im->mem)  vkFreeMemory(d->dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static int img_size(const TAGPU_VKPASS* d, IMG* im, int w, int h, VkFormat fmt)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    int mt;
    if (w < 1 || h < 1) return 0;
    if (im->img && im->w == w && im->h == h && im->fmt == fmt) return 1;
    kill_img(d, im);
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = (uint32_t)w; ii.extent.height = (uint32_t)h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ii, NULL, &im->img) != VK_SUCCESS) { im->img = VK_NULL_HANDLE; return 0; }
    vkGetImageMemoryRequirements(d->dev, im->img, &mr);
    mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { kill_img(d, im); return 0; }
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(d->dev, &ai, NULL, &im->mem) != VK_SUCCESS) { im->mem = VK_NULL_HANDLE; kill_img(d, im); return 0; }
    if (vkBindImageMemory(d->dev, im->img, im->mem, 0) != VK_SUCCESS) { kill_img(d, im); return 0; }
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &vi, NULL, &im->view) != VK_SUCCESS) { im->view = VK_NULL_HANDLE; kill_img(d, im); return 0; }
    im->w = w; im->h = h; im->fmt = fmt; im->have = 0; im->gen = 0;
    return 1;
}

/* copy `rows` rows of `pitch` bytes out of `src` into the image, through this
   slot's staging buffer at `off`. The source may be a SUB-RECT of a larger
   buffer, which is why the pitch is separate from the width. */
static int img_up(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot, IMG* im,
                  const unsigned char* src, int pitch, int bpp, VkDeviceSize off)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy r;
    VkDeviceSize need = (VkDeviceSize)im->w * im->h * bpp;
    int y;
    if (!im->img || !src) return 0;
    if (off + need > s_stgCap[slot]) return 0;
    for (y = 0; y < im->h; y++)
        memcpy(s_stgMap[slot] + off + (VkDeviceSize)y * im->w * bpp,
               src + (size_t)y * pitch, (size_t)im->w * bpp);
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = im->have ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                           : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = im->have ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, im->have ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                      : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    memset(&r, 0, sizeof r);
    r.bufferOffset = off;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = (uint32_t)im->w;
    r.imageExtent.height = (uint32_t)im->h;
    r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, s_stg[slot], im->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    im->have = 1;
    return 1;
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

#define NSAMP 4
static const uint32_t SAMP_BIND[NSAMP] = { 40, 41, 42, 43 };

static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[2 + NSAMP];
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    uint32_t i;
    int n = 0;

    memset(b, 0, sizeof b);
    b[n].binding = 0;  b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; n++;
    b[n].binding = 32; b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; n++;
    for (i = 0; i < NSAMP; i++) {
        b[n].binding = SAMP_BIND[i];
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; n++;
    }
    li.bindingCount = (uint32_t)n; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &s_dsl) != VK_SUCCESS) {
        s_dsl = VK_NULL_HANDLE; return 0;
    }
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) {
        s_plo = VK_NULL_HANDLE; return 0;
    }
    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ps[0].descriptorCount = 2 * d->slots;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = NSAMP * d->slots;
    pi.maxSets = d->slots;
    pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &pi, NULL, &s_pool) != VK_SUCCESS) {
        s_pool = VK_NULL_HANDLE; return 0;
    }
    for (i = 0; i < d->slots; i++) lay[i] = s_dsl;
    ai.descriptorPool = s_pool;
    ai.descriptorSetCount = d->slots;
    ai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &ai, s_set) != VK_SUCCESS) return 0;
    /* NEAREST on all four, and it is not a style choice: uLayer's texel IS a
       palette index and interpolating two of them gives a colour that is in
       neither, which is the GL twin's own comment on the same sampler. */
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(d->dev, &si, NULL, &s_samp) != VK_SUCCESS) {
        s_samp = VK_NULL_HANDLE; return 0;
    }
    return 1;
}

static int build_pipelines(const TAGPU_VKPASS* d, VkRenderPass rp)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[1];
    VkVertexInputAttributeDescription va[4];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_LINE_WIDTH };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    int ok = 0;

    vs = mk_module(d, tagpu_spv_tagpu_mark_VS, sizeof tagpu_spv_tagpu_mark_VS / 4);
    fs = mk_module(d, tagpu_spv_tagpu_mark_FS, sizeof tagpu_spv_tagpu_mark_FS / 4);
    if (!vs || !fs) goto done;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;

    memset(vb, 0, sizeof vb);
    vb[0].binding = 0; vb[0].stride = MK_VSTRIDE;
    vb[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(va, 0, sizeof va);
    va[0].location = 0; va[0].format = VK_FORMAT_R32G32_SFLOAT; va[0].offset = 0;
    va[1].location = 1; va[1].format = VK_FORMAT_R32G32_SFLOAT; va[1].offset = 8;
    va[2].location = 2; va[2].format = VK_FORMAT_R32G32_SFLOAT; va[2].offset = 16;
    va[3].location = 3; va[3].format = VK_FORMAT_R32_SFLOAT;    va[3].offset = 24;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = va;

    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* NO DEPTH. The render pass has a depth attachment, so a pipeline must
       still declare the state -- with testing and writes OFF, which is what the
       GL twin runs with. */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.maxDepthBounds = 1.0f;

    /* NO BLEND, for the twin's own reason: every fragment here is opaque. */
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = VK_FALSE;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    dy.dynamicStateCount = d->lineok ? 3 : 2; dy.pDynamicStates = dyn;

    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = s_plo;
    gp.renderPass = rp;
    gp.subpass = 0;

    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeTri) != VK_SUCCESS)
        goto done;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    if (vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeLine) != VK_SUCCESS)
        goto done;
    s_pipeRp = rp;
    ok = 1;
done:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

/* ---- the frame -------------------------------------------------------- */

static void put_f(unsigned char* p, int off, float v) { memcpy(p + off, &v, 4); }
static void put_i(unsigned char* p, int off, int v)   { memcpy(p + off, &v, 4); }

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a)
{
    return a <= 1 ? v : ((v + a - 1) / a) * a;
}

static int slot_buf(const TAGPU_VKPASS* d, uint32_t slot, VkDeviceSize ubo,
                    VkDeviceSize vert, VkDeviceSize stg)
{
    /* A GROW DESTROYS THIS SLOT'S OWN BUFFER AND NOBODY ELSE'S, and the seam's
       fence for this slot has already been waited on before `prepare` runs --
       which is the whole reason these are per slot. */
    if (!s_ubo[slot] || s_uboCap[slot] < ubo) {
        if (s_ubo[slot]) { vkDestroyBuffer(d->dev, s_ubo[slot], NULL); vkFreeMemory(d->dev, s_uboMem[slot], NULL); }
        s_ubo[slot] = VK_NULL_HANDLE; s_uboMem[slot] = VK_NULL_HANDLE; s_uboMap[slot] = NULL; s_uboCap[slot] = 0;
        if (!mk_buffer(d, ubo, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       &s_ubo[slot], &s_uboMem[slot], &s_uboMap[slot])) return 0;
        s_uboCap[slot] = ubo;
    }
    if (!s_vb[slot] || s_vbCap[slot] < vert) {
        if (s_vb[slot]) { vkDestroyBuffer(d->dev, s_vb[slot], NULL); vkFreeMemory(d->dev, s_vbMem[slot], NULL); }
        s_vb[slot] = VK_NULL_HANDLE; s_vbMem[slot] = VK_NULL_HANDLE; s_vbMap[slot] = NULL; s_vbCap[slot] = 0;
        if (!mk_buffer(d, vert, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       &s_vb[slot], &s_vbMem[slot], &s_vbMap[slot])) return 0;
        s_vbCap[slot] = vert;
    }
    if (!s_stg[slot] || s_stgCap[slot] < stg) {
        if (s_stg[slot]) { vkDestroyBuffer(d->dev, s_stg[slot], NULL); vkFreeMemory(d->dev, s_stgMem[slot], NULL); }
        s_stg[slot] = VK_NULL_HANDLE; s_stgMem[slot] = VK_NULL_HANDLE; s_stgMap[slot] = NULL; s_stgCap[slot] = 0;
        if (!mk_buffer(d, stg, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       &s_stg[slot], &s_stgMem[slot], &s_stgMap[slot])) return 0;
        s_stgCap[slot] = stg;
    }
    return 1;
}

static void write_set(const TAGPU_VKPASS* d, uint32_t slot)
{
    VkDescriptorBufferInfo bi[2];
    VkDescriptorImageInfo ii[NSAMP];
    VkWriteDescriptorSet w[2 + NSAMP];
    /* the order the shader names them: uLayer, uPal, uFogGrid, uFogLUT. A
       binding with no image this frame falls back to the LAYER's view, which
       always exists once the pass is ready -- a set cannot be bound with a
       hole, and the uniform that would read it is 0 on such a frame. */
    const IMG* src[NSAMP];
    uint32_t i;
    int n = 0;
    src[0] = &s_img[IMG_LAYER]; src[1] = &s_img[IMG_PAL];
    src[2] = &s_img[IMG_FOG];   src[3] = &s_img[IMG_LUT];
    memset(bi, 0, sizeof bi); memset(ii, 0, sizeof ii); memset(w, 0, sizeof w);
    bi[0].buffer = s_ubo[slot]; bi[0].range = VS_SZ;
    bi[1].buffer = s_ubo[slot]; bi[1].range = FS_SZ;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[n].dstSet = s_set[slot]; w[n].dstBinding = 0; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[n].pBufferInfo = &bi[0]; n++;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[n].dstSet = s_set[slot]; w[n].dstBinding = 32; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[n].pBufferInfo = &bi[1]; n++;
    for (i = 0; i < NSAMP; i++) {
        ii[i].sampler = s_samp;
        ii[i].imageView = src[i]->view ? src[i]->view : s_img[IMG_LAYER].view;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = s_set[slot]; w[n].dstBinding = SAMP_BIND[i];
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &ii[i]; n++;
    }
    vkUpdateDescriptorSets(d->dev, (uint32_t)n, w, 0, NULL);
}

int tagpu_vk_mark_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    VkPhysicalDeviceProperties props;
    VkDeviceSize vsz, usz, ssz, off;
    int i, needLines = 0;

    s_drawThis = 0; s_abFrame = 0;
    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    s_slot = slot;

    if (s_state == ST_UNBUILT) {
        if (!resolve(d)) { s_state = ST_REFUSED; s_downOwed = 1; return 0; }
        vkGetPhysicalDeviceProperties(d->pd, &props);
        s_ualign = props.limits.minUniformBufferOffsetAlignment;
        if (s_ualign == 0) s_ualign = 1;
        if (!build_descriptors(d)) {
            plog(d, "mark: no descriptors for the marker pass");
            s_state = ST_REFUSED; s_downOwed = 1; return 0;
        }
        s_state = ST_READY;
    }

    if (!tagpu_mark_handover(&s_h, d->frame)) return 0;
    s_abFrame = s_h.ab;
    if (s_h.ndraw <= 0 || s_h.nvert <= 0 || !s_h.verts || !s_h.draws) return 0;
    if (s_h.ndraw > TAGPU_MK_MAXDRAW) return 0;

    for (i = 0; i < s_h.ndraw; i++) {
        const TAGPU_MKDRAW* g = &s_h.draws[i];
        if (g->first < 0 || g->count <= 0 || g->first + g->count > s_h.nvert) return 0;
        if (g->lines) needLines = 1;
        /* A DRAW THAT WANTED FOG AND HAS NO GRID IS REFUSED, not drawn clear:
           the GL twin sampled a grid this lane would not have, and an unfogged
           marker over fogged ground is a different picture. */
        if (g->fog && !s_h.fogGrid) {
            if (!s_saidFog) {
                s_saidFog = 1;
                plog(d, "mark: a fogged marker draw arrived with no fog grid - "
                        "nothing drawn while that is true");
            }
            return 0;
        }
    }
    if (needLines && !d->lineok) {
        if (!s_saidLine) {
            s_saidLine = 1;
            plog(d, "mark: order lines need wideLines and the device has not got "
                    "it - a thin line is a different picture, so nothing is drawn");
        }
        return 0;
    }

    /* the images this frame needs, and the staging to fill them */
    ssz = 0;
    if (s_h.layer && s_h.layerW > 0 && s_h.layerH > 0)
        ssz += (VkDeviceSize)s_h.layerW * s_h.layerH;
    if (s_h.text && s_h.textW > 0 && s_h.textH > 0)
        ssz += (VkDeviceSize)s_h.textW * s_h.textH;
    ssz += 256 * 4;                                  /* the palette */
    if (s_h.fogGrid) ssz += (VkDeviceSize)s_h.fogGridCols * s_h.fogGridRows * 2;
    if (s_h.fogLut) ssz += 256;
    if (ssz < 4096) ssz = 4096;

    vsz = (VkDeviceSize)s_h.nvert * MK_VSTRIDE;
    usz = align_up(VS_SZ, s_ualign) + align_up(FS_SZ, s_ualign) * (VkDeviceSize)s_h.ndraw;
    if (!slot_buf(d, slot, usz, vsz, ssz)) {
        if (!s_saidRoom) {
            s_saidRoom = 1;
            plog(d, "mark: the device would not take this frame's buffers - "
                    "nothing drawn");
        }
        return 0;
    }

    memcpy(s_vbMap[slot], s_h.verts, (size_t)vsz);

    off = 0;
    if (s_h.layer && s_h.layerW > 0 && s_h.layerH > 0) {
        if (!img_size(d, &s_img[IMG_LAYER], s_h.layerW, s_h.layerH, VK_FORMAT_R8_UNORM) ||
            !img_up(d, cb, slot, &s_img[IMG_LAYER], s_h.layer, s_h.layerPitch, 1, off))
            return 0;
        off += (VkDeviceSize)s_h.layerW * s_h.layerH;
    }
    if (s_h.text && s_h.textW > 0 && s_h.textH > 0) {
        int fresh = !s_img[IMG_TEXT].have || s_img[IMG_TEXT].gen != s_h.textGen;
        if (!img_size(d, &s_img[IMG_TEXT], s_h.textW, s_h.textH, VK_FORMAT_R8_UNORM)) return 0;
        if (fresh) {
            if (!img_up(d, cb, slot, &s_img[IMG_TEXT], s_h.text, s_h.textW, 1, off)) return 0;
            s_img[IMG_TEXT].gen = s_h.textGen;
        }
        off += (VkDeviceSize)s_h.textW * s_h.textH;
    }
    if (s_h.pal) {
        if (!img_size(d, &s_img[IMG_PAL], 256, 1, VK_FORMAT_R8G8B8A8_UNORM) ||
            !img_up(d, cb, slot, &s_img[IMG_PAL], s_h.pal, 256 * 4, 4, off))
            return 0;
        off += 256 * 4;
    }
    if (s_h.fogGrid && s_h.fogGridCols > 0 && s_h.fogGridRows > 0) {
        if (!img_size(d, &s_img[IMG_FOG], s_h.fogGridCols, s_h.fogGridRows, VK_FORMAT_R8G8_UNORM) ||
            !img_up(d, cb, slot, &s_img[IMG_FOG], (const unsigned char*)s_h.fogGrid,
                    s_h.fogGridCols * 2, 2, off))
            return 0;
        off += (VkDeviceSize)s_h.fogGridCols * s_h.fogGridRows * 2;
    }
    if (s_h.fogLut) {
        if (!img_size(d, &s_img[IMG_LUT], 256, 1, VK_FORMAT_R8_UNORM) ||
            !img_up(d, cb, slot, &s_img[IMG_LUT], s_h.fogLut, 256, 1, off))
            return 0;
        off += 256;
    }
    /* the layer view is the fallback every unused binding names, so the pass
       cannot draw before it exists */
    if (!s_img[IMG_LAYER].view) {
        if (!img_size(d, &s_img[IMG_LAYER], 1, 1, VK_FORMAT_R8_UNORM)) return 0;
    }

    /* the vertex block first, then one fragment block per draw */
    memset(s_uboMap[slot], 0, (size_t)usz);
    put_f(s_uboMap[slot], VS_GAME, s_h.gw);
    put_f(s_uboMap[slot], VS_GAME + 4, s_h.gh);
    put_f(s_uboMap[slot], VS_ZOOM, s_h.zoom);
    put_f(s_uboMap[slot], VS_ZOOMC, s_h.zoomCx);
    put_f(s_uboMap[slot], VS_ZOOMC + 4, s_h.zoomCy);
    {
        VkDeviceSize at = align_up(VS_SZ, s_ualign);
        VkDeviceSize fstride = align_up(FS_SZ, s_ualign);
        for (i = 0; i < s_h.ndraw; i++) {
            unsigned char* p = s_uboMap[slot] + at;
            put_i(p, FS_KEY, s_h.key);
            put_i(p, FS_TEXT, s_h.draws[i].text);
            put_f(p, FS_FOGORG, s_h.fogOrgX);
            put_f(p, FS_FOGORG + 4, s_h.fogOrgY);
            put_f(p, FS_FOGDIM, s_h.fogCols);
            put_f(p, FS_FOGDIM + 4, s_h.fogRows);
            put_i(p, FS_FOG, s_h.draws[i].fog);
            s_fsOff[i] = at;
            at += fstride;
        }
    }
    write_set(d, slot);
    s_drawThis = 1;
    return 1;
}

void tagpu_vk_mark_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          VkRenderPass rp)
{
    VkDeviceSize zero = 0;
    int i;
    if (s_state != ST_READY || !s_drawThis) return;
    if (slot != s_slot) return;
    if (!s_pipeTri || s_pipeRp != rp) {
        if (s_pipeTri) { vkDestroyPipeline(d->dev, s_pipeTri, NULL); s_pipeTri = VK_NULL_HANDLE; }
        if (s_pipeLine) { vkDestroyPipeline(d->dev, s_pipeLine, NULL); s_pipeLine = VK_NULL_HANDLE; }
        if (!build_pipelines(d, rp)) {
            plog(d, "mark: the marker pipelines would not build - nothing drawn");
            s_drawThis = 0;
            return;
        }
    }
    vkCmdBindVertexBuffers(cb, 0, 1, &s_vb[slot], &zero);
    for (i = 0; i < s_h.ndraw; i++) {
        const TAGPU_MKDRAW* g = &s_h.draws[i];
        uint32_t dyno[2];
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          g->lines ? s_pipeLine : s_pipeTri);
        if (g->lines && d->lineok) vkCmdSetLineWidth(cb, s_h.ss);
        dyno[0] = 0;
        dyno[1] = (uint32_t)s_fsOff[i];
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo,
                                0, 1, &s_set[slot], 2, dyno);
        vkCmdDraw(cb, (uint32_t)g->count, 1, (uint32_t)g->first, 0);
    }
}

int tagpu_vk_mark_ab_frame(void) { return s_abFrame; }

void tagpu_vk_mark_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    int k;
    if (s_state == ST_UNBUILT && !s_pool) return;
    for (k = 0; k < IMG_N; k++) kill_img(d, &s_img[k]);
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        if (s_ubo[i]) { vkDestroyBuffer(d->dev, s_ubo[i], NULL); vkFreeMemory(d->dev, s_uboMem[i], NULL); }
        if (s_vb[i])  { vkDestroyBuffer(d->dev, s_vb[i], NULL);  vkFreeMemory(d->dev, s_vbMem[i], NULL); }
        if (s_stg[i]) { vkDestroyBuffer(d->dev, s_stg[i], NULL); vkFreeMemory(d->dev, s_stgMem[i], NULL); }
        s_ubo[i] = s_vb[i] = s_stg[i] = VK_NULL_HANDLE;
        s_uboMem[i] = s_vbMem[i] = s_stgMem[i] = VK_NULL_HANDLE;
        s_uboMap[i] = s_vbMap[i] = s_stgMap[i] = NULL;
        s_uboCap[i] = s_vbCap[i] = s_stgCap[i] = 0;
        s_set[i] = VK_NULL_HANDLE;
    }
    if (s_pipeTri) { vkDestroyPipeline(d->dev, s_pipeTri, NULL); s_pipeTri = VK_NULL_HANDLE; }
    if (s_pipeLine) { vkDestroyPipeline(d->dev, s_pipeLine, NULL); s_pipeLine = VK_NULL_HANDLE; }
    s_pipeRp = VK_NULL_HANDLE;
    if (s_samp) { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_pool) { vkDestroyDescriptorPool(d->dev, s_pool, NULL); s_pool = VK_NULL_HANDLE; }
    if (s_plo) { vkDestroyPipelineLayout(d->dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dsl) { vkDestroyDescriptorSetLayout(d->dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    s_state = ST_UNBUILT;
    s_drawThis = 0; s_abFrame = 0;
    s_downOwed = 0; s_downPaying = 0;
    s_saidLine = s_saidFog = s_saidRoom = 0;
}

int  tagpu_vk_mark_down_owed(void) { return s_downOwed && !s_downPaying; }
void tagpu_vk_mark_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_mark_down(d);
}
