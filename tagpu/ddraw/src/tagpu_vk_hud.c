/* tagpu_vk_hud.c -- the replay HUD, drawn by Vulkan. Contract: tagpu_vk_hud.h.

   THIS FILE IS THE RASTERISER AND NOTHING ELSE. Where things go is tagpu_hudlay.c's (the
   mockup's rules, in C); what each op becomes -- quads, glyphs, fills -- is tagpu_huddraw.c's;
   the shaders are that file's GLSL, compiled into inc/spirv/tagpu_huddraw.spv.h. Here: the
   images, the uploads, one pipeline, and a draw per run of instances that share a texture.

   ONE INSTANCE PER QUAD, a four-vertex strip each, so a frame's buffer is the instances alone:
   96 bytes a quad, HUD_MAXINST of them per frame slot. Each slot has its own range of one
   persistently mapped, coherent buffer, and the seam's fence wait at the top of its present is
   what proves the slot's range free (tagpu_vk_pass.h).

   THE PARTS ARE UPLOADED ONCE PER BUILD, whole, through tagpu_vk_stage_copy: a pass's first
   upload after it is built is one of the events the banded path is for (tagpu_vk_stage.h). That
   fills mip level 0 and leaves it SHADER_READ_ONLY_OPTIMAL; the rest of each chain is blitted
   down from it, linearly, in this frame's command buffer. The parts are premultiplied, so a
   linear average is the right one.

   THE GLYPH ATLAS changes in play, as text first needs a glyph at a size and a pen fraction. It
   goes whole once after a build, banded. After that only the rows that changed go -- a new
   generation's too, the atlas emptied because a glyph did not fit (tagpu_huddraw.h) -- IN THE
   FRAME'S OWN COMMAND BUFFER through the slot's staging buffer, behind a barrier whose first scope
   includes every frame already submitted (the order tagpu_vk_stage.c's in-frame copies rest on),
   as many as the buffer has room for; the rest go on the next frames, and until the last has
   gone the frame's glyphs are transparent (hide_glyphs). Text can be late; nothing waits.

   IT KNOWS NOTHING ABOUT A WINDOW, and reads no engine state: everything arrives in its
   arguments or in the loaded HUD (tagpu_hudart.h), which is immutable. */

#include "tagpu_vk_pass.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_hud.h"
#include "tagpu_vk_stage.h"
#include "tagpu_huddraw.h"
#include "tagpu_hudstate.h"
#include "spirv/tagpu_huddraw.spv.h"

#define HUD_MAXINST   8192
#define HUD_MAXBATCH  1024
#define IBYTES        ((VkDeviceSize)HUD_MAXINST * sizeof(HUDINST))
#define MAXPART       128

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
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier) X(vkCmdBlitImage)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

typedef struct {
    VkImage         img;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkDescriptorSet set;
    int             w, h, levels;
} TEX;

static int s_state;
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;
static VkDescriptorPool      s_dpool;
static VkSampler             s_clamp, s_repeat, s_glyph;
static TEX                   s_part[MAXPART], s_atlas;
static int                   s_nparts;
static int                   s_nsent;           /* the parts uploaded and their chains blitted */
static int                   s_sent;            /* all of them                                 */
static unsigned              s_agen;            /* the atlas generation the device holds; 0 none */
static VkBuffer              s_ibuf;
static VkDeviceMemory        s_imem;
static unsigned char*        s_imap;
static TAGPU_VKSTAGE         s_stage[TAGPU_VK_SLOTS];
static HUDOPS                s_ops;             /* render thread's; 400 KB, never on a stack  */
static HUDBATCH              s_batch[HUD_MAXBATCH];
static int                   s_nbatch, s_ninst;
static int                   s_saidDrop, s_saidMiss;

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

/* ---- the images ------------------------------------------------------------------------------ */

static int levels_for(int w, int h)
{
    int n = 1;
    while ((w > 1 || h > 1) && n < 16) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

static int mk_tex(const TAGPU_VKPASS* d, TEX* t, VkFormat fmt, int w, int h, int levels, VkSampler samp)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    VkMemoryRequirements req;
    int type;

    t->w = w; t->h = h; t->levels = levels;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = (uint32_t)w;
    ici.extent.height = (uint32_t)h;
    ici.extent.depth = 1;
    ici.mipLevels = (uint32_t)levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, &t->img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, t->img, &req);
    if ((type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &t->mem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, t->img, t->mem, 0) != VK_SUCCESS) return 0;
    ivi.image = t->img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = (uint32_t)levels;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, &t->view) != VK_SUCCESS) return 0;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s_dsl;
    if (vkAllocateDescriptorSets(d->dev, &dai, &t->set) != VK_SUCCESS) return 0;
    memset(&ii, 0, sizeof ii);
    ii.sampler = samp;
    ii.imageView = t->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&wr, 0, sizeof wr);
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = t->set;
    wr.dstBinding = 40;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    return 1;
}

static void free_tex(VkDevice dev, TEX* t)
{
    if (t->view) vkDestroyImageView(dev, t->view, NULL);
    if (t->img) vkDestroyImage(dev, t->img, NULL);
    if (t->mem) vkFreeMemory(dev, t->mem, NULL);
    memset(t, 0, sizeof *t);
}

static int mk_sampler(const TAGPU_VKPASS* d, VkSampler* s, VkSamplerAddressMode u, int mips)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = u;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = mips ? 16.0f : 0.0f;
    return vkCreateSampler(d->dev, &sci, NULL, s) == VK_SUCCESS;
}

/* The chain below level 0, blitted down level by level, every level SHADER_READ_ONLY after. */
static void blit_chain(VkCommandBuffer cb, const TEX* t)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    int i, w = t->w, h = t->h;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;

    /* level 0 left the upload SHADER_READ_ONLY: it becomes the first source */
    b.subresourceRange.baseMipLevel = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    for (i = 1; i < t->levels; i++) {
        VkImageBlit r;
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        b.subresourceRange.baseMipLevel = (uint32_t)i;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        memset(&r, 0, sizeof r);
        r.srcSubresource.aspectMask = r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.srcSubresource.mipLevel = (uint32_t)(i - 1);
        r.dstSubresource.mipLevel = (uint32_t)i;
        r.srcSubresource.layerCount = r.dstSubresource.layerCount = 1;
        r.srcOffsets[1].x = w; r.srcOffsets[1].y = h; r.srcOffsets[1].z = 1;
        r.dstOffsets[1].x = nw; r.dstOffsets[1].y = nh; r.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_LINEAR);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        w = nw; h = nh;
    }
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = (uint32_t)t->levels;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- the build ------------------------------------------------------------------------------- */

static int build_pipeline(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b;
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[6];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule vs, fs;
    VkResult r;
    int i, ok = 0;

    memset(&b, 0, sizeof b);
    b.binding = 40;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dli.bindingCount = 1;
    dli.pBindings = &b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS) return 0;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) return 0;

    vs = mk_module(d, tagpu_spv_tagpu_huddraw_VS, sizeof tagpu_spv_tagpu_huddraw_VS / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_tagpu_huddraw_FS, sizeof tagpu_spv_tagpu_huddraw_FS / sizeof(uint32_t));
    if (!vs || !fs) { plog(d, "hud: a shader module was refused"); goto out; }
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    /* the six vec4s of HUDINST, per instance */
    memset(&vb, 0, sizeof vb);
    vb.binding = 0;
    vb.stride = sizeof(HUDINST);
    vb.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    memset(va, 0, sizeof va);
    for (i = 0; i < 6; i++) {
        va[i].location = (uint32_t)i;
        va[i].binding = 0;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[i].offset = (uint32_t)(i * 4 * sizeof(float));
    }
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 6; vi.pVertexAttributeDescriptions = va;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* PREMULTIPLIED: every colour the fragment stage writes is already weighted by its alpha */
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
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    /* the seam's render pass has a depth attachment, so a depth state is required: none of it */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDepthStencilState = &ds;
    gp.pDynamicState = &dy;
    gp.layout = s_plo;
    gp.renderPass = d->rp;
    gp.subpass = 0;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipe);
    if (r != VK_SUCCESS) { plog(d, "hud: vkCreateGraphicsPipelines refused it (%d)", (int)r); goto out; }
    ok = 1;
out:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

static int format_ok(const TAGPU_VKPASS* d, VkFormat f, VkFormatFeatureFlags need)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(d->pd, f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

static int build(const TAGPU_VKPASS* d, const HUDART* art)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDescriptorPoolSize ps;
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkMemoryRequirements req;
    void* p = NULL;
    int i, type;

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "hud: %u frame slots is outside what this pass carries (%d)", (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    if (!resolve(d)) { plog(d, "hud: an entry point is missing"); return 0; }
    /* SAMPLED, LINEARLY FILTERED AND BLITTED, ASKED FOR RATHER THAN ASSUMED: the parts are
       minified through a mip chain this pass blits, and the glyphs are filtered. All of it is
       mandatory for these two formats, so this has never fired; the day it does the log names
       it instead of the pass drawing nothing. */
    if (!format_ok(d, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                                              | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)
        || !format_ok(d, VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        plog(d, "hud: this device cannot filter or blit RGBA8 / R8 - the pass stays down");
        return 0;
    }
    s_nparts = tagpu_hudlay_nparts(art->lay);
    if (s_nparts > MAXPART) { plog(d, "hud: %d parts, more than the pass holds", s_nparts); return 0; }

    if (!build_pipeline(d)) return 0;
    if (!mk_sampler(d, &s_clamp, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1)
        || !mk_sampler(d, &s_repeat, VK_SAMPLER_ADDRESS_MODE_REPEAT, 1)
        || !mk_sampler(d, &s_glyph, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0)) return 0;

    memset(&ps, 0, sizeof ps);
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = (uint32_t)s_nparts + 1;
    dpi.maxSets = (uint32_t)s_nparts + 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) return 0;

    for (i = 0; i < s_nparts; i++) {
        const HUDIMG* im = &art->img[i];
        const HUDPART* k = tagpu_hudlay_part(art->lay, i);
        if (!mk_tex(d, &s_part[i], VK_FORMAT_R8G8B8A8_UNORM, im->w, im->h, levels_for(im->w, im->h),
                    k->kind == HK_TILE ? s_repeat : s_clamp)) {
            plog(d, "hud: the image of part %s was refused", k->name);
            return 0;
        }
    }
    if (!mk_tex(d, &s_atlas, VK_FORMAT_R8_UNORM, HUD_ATLAS, HUD_ATLAS, 1, s_glyph)) return 0;

    bci.size = IBYTES * d->slots;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, &s_ibuf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(d->dev, s_ibuf, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s_imem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(d->dev, s_ibuf, s_imem, 0) != VK_SUCCESS) return 0;
    if (vkMapMemory(d->dev, s_imem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
    s_imap = (unsigned char*)p;

    s_nsent = s_sent = 0;
    s_agen = 0;
    plog(d, "hud: the pass is up - %d parts, a %dx%d glyph atlas, %u frame slots",
         s_nparts, HUD_ATLAS, HUD_ATLAS, (unsigned)d->slots);
    return 1;
}

/* ---- the uploads ----------------------------------------------------------------------------- */

/* The parts, whole, each followed by its chain, from where the last frame stopped: 23 MB of
   them against a slot's 2 MiB, so they take a dozen frames, and the HUD draws once the last is
   there. A part that would not fit what this frame's buffer has left waits for the next frame,
   where it is the first copy: in the frame's own command buffer when it fits the slot's whole
   buffer, banded when it does not -- the parts larger than the cap, uploaded once after the pass is
   built, which is the banded path's own event (tagpu_vk_stage.h). 1 all sent, 0 more next
   frame, -1 the device. */
static int send_parts(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKSTAGE* st, const HUDART* art)
{
    for (; s_nsent < s_nparts; s_nsent++) {
        const HUDIMG* im = &art->img[s_nsent];
        int r;
        if (st->used && !tagpu_vk_stage_fits(st, (VkDeviceSize)im->w * im->h * 4)) return 0;
        r = tagpu_vk_stage_copy(d, cb, st, s_part[s_nsent].img, 0, im->rgba, im->w * 4, im->w, im->h, 4);
        if (r <= 0) return r;
        blit_chain(cb, &s_part[s_nsent]);
    }
    return 1;
}

/* The atlas rows this frame added, in this frame's command buffer when they fit what the slot
   mapped; a new generation, whole. 1 the device holds the atlas, 0 not this frame, -1 the device. */
static int send_atlas(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKSTAGE* st)
{
    unsigned gen;
    int y0, y1, n, r;
    const unsigned char* a = tagpu_huddraw_atlas(&gen, &y0, &y1);
    VkDeviceSize off, bytes;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy rg;

    /* THE FIRST UPLOAD AFTER A BUILD goes whole, the frame's first copy as a part's is: the image
       has no contents and no layout yet, and 4 MiB is banded -- the post-build event the banded
       path is for (tagpu_vk_stage.h) */
    if (!s_agen) {
        if (st->used) return 0;
        r = tagpu_vk_stage_copy(d, cb, st, s_atlas.img, 0, a, HUD_ATLAS, HUD_ATLAS, HUD_ATLAS, 1);
        if (r > 0) { s_agen = gen; tagpu_huddraw_atlas_sent(HUD_ATLAS); }
        return r;
    }
    /* AFTER IT, ONLY THE CHANGED ROWS, a new generation's included (tagpu_huddraw.h), in this
       frame's command buffer and as many as its buffer has room for; the rest go next frame, and
       the text waits for them (hide_glyphs). Nothing here waits on the device. */
    if (y1 <= y0) { s_agen = gen; return 1; }
    off = (st->used + 3) & ~(VkDeviceSize)3;
    if (!st->map || off >= st->cap) return 0;
    n = (int)((st->cap - off) / HUD_ATLAS);
    if (n <= 0) return 0;
    if (n < y1 - y0) y1 = y0 + n;
    bytes = (VkDeviceSize)(y1 - y0) * HUD_ATLAS;
    memcpy(st->map + off, a + (size_t)y0 * HUD_ATLAS, (size_t)bytes);
    st->used = off + bytes;

    /* write after the reads of every frame already submitted: the barrier's first scope */
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s_atlas.img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = off;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageOffset.y = y0;
    rg.imageExtent.width = HUD_ATLAS;
    rg.imageExtent.height = (uint32_t)(y1 - y0);
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, st->buf, s_atlas.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    s_agen = gen;
    tagpu_huddraw_atlas_sent(y1);
    return 1;
}

/* ---- the frame ------------------------------------------------------------------------------- */

/* The glyphs of a frame whose atlas the device does not hold yet, made transparent: changed rows
   still waiting for room in a frame's buffer (send_atlas), or a frame with no staging, would
   otherwise be sampled where the instances say -- empty texels, or an older generation's
   glyphs. Fills and parts in the same runs still draw. */
static void hide_glyphs(HUDINST* in, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (in[i].prm[0] == (float)HUDM_GLYPH) in[i].rgba[0] = in[i].rgba[1] = in[i].rgba[2] = in[i].rgba[3] = 0;
}

int tagpu_vk_hud_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h)
{
    const HUDART* art;
    const HUDSTATE* st;
    TAGPU_VKSTAGE* sg;
    char err[160];
    float px;
    int dropped = 0, r, due, ox, oy;

    s_ninst = s_nbatch = 0;
    if (s_state == ST_REFUSED || !tagpu_hudstate_frame(d->frame, &st, &px, &ox, &oy)) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS || w == 0 || h == 0 || !(px > 0)) return 0;

    /* NOTHING IS BUILT UNTIL THE HUD HAS LOADED: tagpu_hudart decodes the parts on a thread of
       its own, and the HUD appears on the first frame after it has published them. Every way the
       HUD cannot draw for the session is told to tagpu_hudstate_refuse, which gives the dock and
       the cards back. */
    if (!(art = tagpu_hudart_get())) {
        if (tagpu_hudart_failed()) tagpu_hudstate_refuse("its art did not load");
        else tagpu_hudart_start();
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d, art)) {
            tagpu_vk_hud_down(d);
            s_state = ST_REFUSED;
            tagpu_hudstate_refuse("its pass could not be built");
            return 0;
        }
        s_state = ST_READY;
    }

    if (!tagpu_hudlay_build(art->lay, st, &s_ops, err, sizeof err)) {
        if (!s_saidMiss) { s_saidMiss = 1; plog(d, "hud: %s", err); }
        tagpu_hudstate_refuse("its layout did not build");
        return 0;
    }
    s_ninst = tagpu_huddraw_build(art, &s_ops, px, ox, oy, (int)w, (int)h, (HUDINST*)(s_imap + (size_t)slot * IBYTES),
                                  HUD_MAXINST, s_batch, HUD_MAXBATCH, &s_nbatch, &dropped);
    if ((dropped || s_ops.overflow) && !s_saidDrop) {
        s_saidDrop = 1;
        plog(d, "hud: %d glyphs did not fit the atlas this frame%s (said once)", dropped,
             s_ops.overflow ? ", and the ops overflowed" : "");
    }

    /* THE UPLOADS, through this slot's staging, which the seam's fence wait has freed; a slot
       with nothing to send gives its buffer back (tagpu_vk_stage.h, THE SHAPE). */
    sg = &s_stage[slot];
    {
        unsigned gen;
        int y0, y1;
        tagpu_huddraw_atlas(&gen, &y0, &y1);
        due = !s_sent || gen != s_agen || y1 > y0;
    }
    if (!due) {
        if (sg->buf) tagpu_vk_stage_drop(d, sg);
    } else if (tagpu_vk_stage_begin(d, sg, TAGPU_VK_STAGE_CAP, (VkDeviceSize)4096 * 4)) {
        if (!s_sent) {
            if ((r = send_parts(d, cb, sg, art)) < 0) goto lost;
            if (r > 0) {
                s_sent = 1;
                plog(d, "hud: the parts are on the device");
            }
        }
        if (s_sent && (r = send_atlas(d, cb, sg)) < 0) goto lost;
    }
    if (!s_sent || !s_agen) { s_ninst = 0; return 0; }
    /* THE TEXT DRAWS ONLY FROM WHAT THE DEVICE HOLDS: this build's glyphs are in the atlas the
       device has once its generation is s_agen and no row is left dirty */
    {
        unsigned gen;
        int y0, y1;
        tagpu_huddraw_atlas(&gen, &y0, &y1);
        if (gen != s_agen || y1 > y0) hide_glyphs((HUDINST*)(s_imap + (size_t)slot * IBYTES), s_ninst);
    }
    return s_ninst > 0;
lost:
    /* THE DEVICE FAILED A SUBMIT OR A FENCE partway (tagpu_vk_stage.h): an image is half written
       and the banded path's resources may still be in flight, so the pass stops drawing and asks
       for nothing more. Nothing is destroyed here, mid-frame, where other slots' submits still
       name these objects: tagpu_vk_hud_down gives them back behind the seam's vkDeviceWaitIdle
       on the seam's next teardown (vk_down, passes_down). The HUD stays off for the session, and
       the dock and the cards come back (tagpu_hudstate_refuse). */
    plog(d, "hud: the device failed an upload - the pass stops drawing");
    s_state = ST_REFUSED;
    tagpu_hudstate_refuse("the device failed an upload");
    s_ninst = 0;
    return 0;
}

void tagpu_vk_hud_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = (VkDeviceSize)slot * IBYTES;
    int i;
    (void)d;
    if (s_state != ST_READY || !s_ninst) return;
    /* NO FLIP: the instances are in clip space with y down already (tagpu_huddraw.c) */
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)w; vp.height = (float)h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = w; sc.extent.height = h;
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_ibuf, &off);
    for (i = 0; i < s_nbatch; i++) {
        const HUDBATCH* b = &s_batch[i];
        const TEX* t = b->tex == HUD_TEX_ATLAS ? &s_atlas : b->tex >= 0 && b->tex < s_nparts ? &s_part[b->tex] : &s_atlas;
        if (!b->count) continue;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1, &t->set, 0, NULL);
        vkCmdDraw(cb, 4, (uint32_t)b->count, 0, (uint32_t)b->first);
    }
    s_ninst = 0;
}

void tagpu_vk_hud_down(const TAGPU_VKPASS* d)
{
    VkDevice dev = d->dev;
    int i;
    if (!dev || !vkDestroyBuffer) { s_state = ST_UNBUILT; return; }
    for (i = 0; i < TAGPU_VK_SLOTS; i++)
        if (s_stage[i].buf) tagpu_vk_stage_drop(d, &s_stage[i]);
    if (s_pipe)  { vkDestroyPipeline(dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    for (i = 0; i < MAXPART; i++) free_tex(dev, &s_part[i]);
    free_tex(dev, &s_atlas);
    /* the sets go back with the pool */
    if (s_dpool) { vkDestroyDescriptorPool(dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_clamp)  { vkDestroySampler(dev, s_clamp, NULL); s_clamp = VK_NULL_HANDLE; }
    if (s_repeat) { vkDestroySampler(dev, s_repeat, NULL); s_repeat = VK_NULL_HANDLE; }
    if (s_glyph)  { vkDestroySampler(dev, s_glyph, NULL); s_glyph = VK_NULL_HANDLE; }
    if (s_imap)  { vkUnmapMemory(dev, s_imem); s_imap = NULL; }
    if (s_ibuf)  { vkDestroyBuffer(dev, s_ibuf, NULL); s_ibuf = VK_NULL_HANDLE; }
    if (s_imem)  { vkFreeMemory(dev, s_imem, NULL); s_imem = VK_NULL_HANDLE; }
    s_nsent = s_sent = 0;
    s_agen = 0;
    s_ninst = s_nbatch = 0;
    /* UNBUILT, not REFUSED: a teardown or a resize must let the pass come back */
    s_state = ST_UNBUILT;
}
