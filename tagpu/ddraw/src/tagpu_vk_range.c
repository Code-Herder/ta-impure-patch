/* tagpu_vk_range.c -- the weapon range rings, drawn by Vulkan.
   Contract: tagpu_vk_range.h. What is drawn: tagpu_range.h.

   THREE PIPELINES, ONE PROGRAM PAIR EACH:
     disc   DVS/FS, stencil NOT_EQUAL against the draw's bit and REPLACE: a
            class's discs set its bit once per pixel, and the fill colour they
            carry lands once per pixel (BAR's stencil path writes its fill the
            same way, gui_attackrange_gl4.lua l.1596-1661);
     outer  LVS/FS, stencil NOT_EQUAL and KEEP: a ring's outline survives only
            outside every disc of its class, which is the union's boundary;
     line   LVS/FS, no stencil: the inner rings and the never-merged lrpc.
   The bit, the compare mask and the write mask are dynamic state, so one
   pipeline serves every class.

   PREMULTIPLIED OVER, AND THE TARGET'S ALPHA KEPT, as the marker pass's order
   art keeps it: the world target's alpha is not a picture.

   NO DEPTH. The rings are drawn over the finished world, as the stock range
   circles are (tagpu_mark.h: the markers are the top layer), and the mockup
   draws them the same way.

   THE STENCIL IS THIS PASS'S BETWEEN ITS FIRST CLEAR AND ITS LAST. The unit
   pass's hard shadows use the plane too and return every mark they make to 0
   (tagpu_vk_unit.c, build_shadow_pipelines); this pass does not rely on it --
   the draw list clears before every group -- and clears once more after its
   last draw, so what it leaves is what it found to be the rule everywhere. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "tagpu_vk_range.h"
#include "tagpu_vk_terr.h"     /* the slot's height grid */
#include "tagpu_range.h"
#include "spirv/tagpu_range.spv.h"

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
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkCmdClearAttachments)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

/* the vertex stages' std140 block, as tagpu_range.spv.h prints it */
#define VS_SZ       64
#define VS_GAME      0
#define VS_ZOOM      8
#define VS_ZOOMC    16
#define VS_EYE      24
#define VS_HDIM     32
#define VS_MAPEXT   40
#define VS_SEA      48
#define VS_GRAV     52
#define VS_PXGAME   56

#define INST_STRIDE ((uint32_t)sizeof(TAGPU_RGINST))

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

static int s_state;
static int s_downOwed, s_downPaying;
static int s_drawThis;
static uint32_t s_slot;

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipeDisc, s_pipeOuter, s_pipeLine;
static VkRenderPass          s_pipeRp;
static VkDescriptorPool      s_pool;
static VkDescriptorSet       s_set[TAGPU_VK_SLOTS];
static VkSampler             s_samp;

static VkBuffer       s_ubo[TAGPU_VK_SLOTS];
static VkDeviceMemory s_uboMem[TAGPU_VK_SLOTS];
static unsigned char* s_uboMap[TAGPU_VK_SLOTS];
static VkDeviceSize   s_uboCap[TAGPU_VK_SLOTS];
static VkBuffer       s_vb[TAGPU_VK_SLOTS];
static VkDeviceMemory s_vbMem[TAGPU_VK_SLOTS];
static unsigned char* s_vbMap[TAGPU_VK_SLOTS];
static VkDeviceSize   s_vbCap[TAGPU_VK_SLOTS];

static TAGPU_RGHAND s_h;
/* ONE LATCH PER SITE: a latch shared by two refusals lets the first silence
   the second for the session. */
static int s_saidUp, s_saidStencil, s_saidHeight, s_saidRoom, s_saidBound;
static int s_saidPipe, s_saidSlot, s_saidDrew;

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

static int mk_buffer(const TAGPU_VKPASS* d, VkDeviceSize size, VkBufferUsageFlags use,
                     VkBuffer* buf, VkDeviceMemory* mem, unsigned char** map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    int type;
    void* p = NULL;
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *map = NULL;
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
    if (vkMapMemory(d->dev, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) goto bad;
    *map = (unsigned char*)p;
    return 1;
bad:
    if (*mem) vkFreeMemory(d->dev, *mem, NULL);
    if (*buf) vkDestroyBuffer(d->dev, *buf, NULL);
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *map = NULL;
    return 0;
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

/* binding 0 the vertex stages' block, 8 their height grid (spirv-gen's
   allocation by stage) */
static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[2];
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    uint32_t i;

    memset(b, 0, sizeof b);
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 8; b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    li.bindingCount = 2; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &s_dsl) != VK_SUCCESS) {
        s_dsl = VK_NULL_HANDLE; return 0;
    }
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) {
        s_plo = VK_NULL_HANDLE; return 0;
    }
    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[0].descriptorCount = d->slots;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = d->slots;
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
    /* NEAREST and texelFetch: the shader does its own bilinear, the engine's */
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(d->dev, &si, NULL, &s_samp) != VK_SUCCESS) {
        s_samp = VK_NULL_HANDLE; return 0;
    }
    return 1;
}

static void kill_pipes(const TAGPU_VKPASS* d)
{
    if (s_pipeDisc)  { vkDestroyPipeline(d->dev, s_pipeDisc, NULL);  s_pipeDisc = VK_NULL_HANDLE; }
    if (s_pipeOuter) { vkDestroyPipeline(d->dev, s_pipeOuter, NULL); s_pipeOuter = VK_NULL_HANDLE; }
    if (s_pipeLine)  { vkDestroyPipeline(d->dev, s_pipeLine, NULL);  s_pipeLine = VK_NULL_HANDLE; }
    s_pipeRp = VK_NULL_HANDLE;
}

static int build_pipelines(const TAGPU_VKPASS* d, VkRenderPass rp)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[4];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[5] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                              VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule dvs, lvs, fs;
    VkStencilOpState so;
    int i, ok = 0;

    dvs = mk_module(d, tagpu_spv_tagpu_range_DVS, sizeof tagpu_spv_tagpu_range_DVS / 4);
    lvs = mk_module(d, tagpu_spv_tagpu_range_LVS, sizeof tagpu_spv_tagpu_range_LVS / 4);
    fs  = mk_module(d, tagpu_spv_tagpu_range_FS,  sizeof tagpu_spv_tagpu_range_FS / 4);
    if (!dvs || !lvs || !fs) goto done;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = dvs; st[0].pName = "main";
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;

    /* one record an instance: the vertex index walks the ring */
    memset(&vb, 0, sizeof vb);
    vb.binding = 0; vb.stride = INST_STRIDE; vb.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    memset(va, 0, sizeof va);
    for (i = 0; i < 4; i++) {
        va[i].location = (uint32_t)i;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[i].offset = (uint32_t)(16 * i);
    }
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = va;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&so, 0, sizeof so);
    so.failOp = VK_STENCIL_OP_KEEP;
    so.passOp = VK_STENCIL_OP_REPLACE;
    so.depthFailOp = VK_STENCIL_OP_KEEP;
    so.compareOp = VK_COMPARE_OP_NOT_EQUAL;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = VK_TRUE;
    ds.front = so; ds.back = so;
    ds.maxDepthBounds = 1.0f;

    memset(&ba, 0, sizeof ba);
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    dy.dynamicStateCount = 5; dy.pDynamicStates = dyn;

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
    if (vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeDisc) != VK_SUCCESS)
        goto done;

    st[0].module = lvs;
    so.passOp = VK_STENCIL_OP_KEEP;
    ds.front = so; ds.back = so;
    if (vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeOuter) != VK_SUCCESS)
        goto done;
    ds.stencilTestEnable = VK_FALSE;
    if (vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeLine) != VK_SUCCESS)
        goto done;
    s_pipeRp = rp;
    ok = 1;
done:
    if (dvs) vkDestroyShaderModule(d->dev, dvs, NULL);
    if (lvs) vkDestroyShaderModule(d->dev, lvs, NULL);
    if (fs)  vkDestroyShaderModule(d->dev, fs, NULL);
    if (!ok) kill_pipes(d);
    return ok;
}

static void put_f(unsigned char* p, int off, float v) { memcpy(p + off, &v, 4); }

/* A grow destroys this slot's own buffer and nobody else's, behind the seam's
   wait on this slot's fence. */
static int slot_buf(const TAGPU_VKPASS* d, uint32_t slot, VkDeviceSize vert)
{
    if (!s_ubo[slot]) {
        if (!mk_buffer(d, VS_SZ, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       &s_ubo[slot], &s_uboMem[slot], &s_uboMap[slot])) return 0;
        s_uboCap[slot] = VS_SZ;
    }
    if (!s_vb[slot] || s_vbCap[slot] < vert) {
        if (s_vb[slot]) { vkDestroyBuffer(d->dev, s_vb[slot], NULL); vkFreeMemory(d->dev, s_vbMem[slot], NULL); }
        s_vb[slot] = VK_NULL_HANDLE; s_vbMem[slot] = VK_NULL_HANDLE; s_vbMap[slot] = NULL; s_vbCap[slot] = 0;
        if (!mk_buffer(d, vert, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       &s_vb[slot], &s_vbMem[slot], &s_vbMap[slot])) return 0;
        s_vbCap[slot] = vert;
    }
    return 1;
}

int tagpu_vk_range_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    VkDescriptorBufferInfo bi;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w[2];
    VkImageView hv;
    VkDeviceSize vsz;
    int hW = 0, hH = 0, i;
    unsigned char* u;
    (void)cb;

    s_drawThis = 0;
    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    s_slot = slot;
    if (!tagpu_range_handover(&s_h, d->frame)) return 0;
    if (s_h.ndraw <= 0 || s_h.ninst <= 0) return 0;

    /* THE UNION IS THE STENCIL'S. A device with no stencil-carrying depth
       format has no plane to draw it in, and rings without their union are a
       different picture: none are drawn. */
    if (!d->stencilok) {
        if (!s_saidStencil) { s_saidStencil = 1;
            plog(d, "range: the target has no stencil plane - no range rings are drawn"); }
        return 0;
    }
    if (s_state == ST_UNBUILT) {
        VkPhysicalDeviceProperties props;
        if (!resolve(d) || !build_descriptors(d)) {
            plog(d, "range: an entry point or the descriptors would not build - "
                    "the range rings stay down");
            s_state = ST_REFUSED; s_downOwed = 1; return 0;
        }
        vkGetPhysicalDeviceProperties(d->pd, &props);
        s_state = ST_READY;
        if (!s_saidUp) { s_saidUp = 1;
            plog(d, "range: up - %u frame slots, %u vertex-stage samplers",
                 (unsigned)d->slots, (unsigned)props.limits.maxPerStageDescriptorSamplers); }
    }
    /* THE RINGS' SHAPE IS THE TERRAIN'S: no height grid bound for this slot,
       no rings this frame */
    hv = tagpu_vk_terr_height(slot, &hW, &hH);
    if (!hv) {
        if (!s_saidHeight) { s_saidHeight = 1;
            plog(d, "range: the terrain pass bound no height grid for this frame - "
                    "no range rings while that is true"); }
        return 0;
    }
    s_saidHeight = 0;
    if (s_h.ninst > TAGPU_RG_MAXINST || s_h.ndraw > TAGPU_RG_MAXDRAW) {
        if (!s_saidBound) { s_saidBound = 1;
            plog(d, "range: %d instances / %d draws is past what this pass carries",
                 s_h.ninst, s_h.ndraw); }
        return 0;
    }
    for (i = 0; i < s_h.ndraw; i++) {
        const TAGPU_RGDRAW* g = &s_h.draws[i];
        if (g->kind == TAGPU_RG_CLEAR) continue;
        if (g->first < 0 || g->count <= 0 || g->first + g->count > s_h.ninst ||
            g->nseg < 3 || g->nseg > 4096 ||
            (g->kind != TAGPU_RG_LINE && (g->bit < 1 || g->bit > 255))) {
            if (!s_saidBound) { s_saidBound = 1;
                plog(d, "range: draw %d is out of bounds - kind=%d bit=%d first=%d "
                        "count=%d nseg=%d of %d", i, g->kind, g->bit, g->first,
                        g->count, g->nseg, s_h.ninst); }
            return 0;
        }
    }
    vsz = (VkDeviceSize)s_h.ninst * INST_STRIDE;
    if (!slot_buf(d, slot, vsz)) {
        if (!s_saidRoom) { s_saidRoom = 1;
            plog(d, "range: the device would not take this frame's buffers"); }
        return 0;
    }
    memcpy(s_vbMap[slot], s_h.inst, (size_t)vsz);
    u = s_uboMap[slot];
    memset(u, 0, VS_SZ);
    put_f(u, VS_GAME, s_h.gw);        put_f(u, VS_GAME + 4, s_h.gh);
    put_f(u, VS_ZOOM, s_h.zoom);
    put_f(u, VS_ZOOMC, s_h.zoomCx);   put_f(u, VS_ZOOMC + 4, s_h.zoomCy);
    put_f(u, VS_EYE, s_h.eyeX);       put_f(u, VS_EYE + 4, s_h.eyeY);
    put_f(u, VS_HDIM, (float)hW);     put_f(u, VS_HDIM + 4, (float)hH);
    put_f(u, VS_MAPEXT, s_h.mapW);    put_f(u, VS_MAPEXT + 4, s_h.mapH);
    put_f(u, VS_SEA, s_h.sea);
    put_f(u, VS_GRAV, s_h.grav);
    put_f(u, VS_PXGAME, s_h.ss > 0.0f ? 1.0f / s_h.ss : 1.0f);

    memset(&bi, 0, sizeof bi); memset(&ii, 0, sizeof ii); memset(w, 0, sizeof w);
    bi.buffer = s_ubo[slot]; bi.range = VS_SZ;
    ii.sampler = s_samp; ii.imageView = hv;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = s_set[slot]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bi;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = s_set[slot]; w[1].dstBinding = 8; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 2, w, 0, NULL);
    s_drawThis = 1;
    return 1;
}

/* the viewport's rect in the target's pixels, as the marker pass clips */
static void rg_scissor(uint32_t w, uint32_t h, VkRect2D* out)
{
    float sx = s_h.gw > 0.0f ? (float)w / s_h.gw : 1.0f;
    float sy = s_h.gh > 0.0f ? (float)h / s_h.gh : 1.0f;
    int x0 = (int)(s_h.vpL * sx + 0.5f), y0 = (int)(s_h.vpT * sy + 0.5f);
    int ww = (int)(s_h.vw * sx + 0.5f), hh = (int)(s_h.vh * sy + 0.5f);
    if (x0 < 0) { ww += x0; x0 = 0; }
    if (y0 < 0) { hh += y0; y0 = 0; }
    if (x0 > (int)w) x0 = (int)w;
    if (y0 > (int)h) y0 = (int)h;
    if (ww < 0) ww = 0;
    if (hh < 0) hh = 0;
    if (x0 + ww > (int)w) ww = (int)w - x0;
    if (y0 + hh > (int)h) hh = (int)h - y0;
    out->offset.x = x0; out->offset.y = y0;
    out->extent.width = (uint32_t)ww; out->extent.height = (uint32_t)hh;
}

static void clear_stencil(VkCommandBuffer cb, const VkRect2D* sc)
{
    VkClearAttachment ca;
    VkClearRect cr;
    memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
    ca.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    ca.clearValue.depthStencil.stencil = 0;
    cr.rect = *sc;
    cr.baseArrayLayer = 0;
    cr.layerCount = 1;
    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
}

void tagpu_vk_range_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                           VkRenderPass rp, uint32_t w, uint32_t h)
{
    VkDeviceSize zero = 0;
    VkViewport vp;
    VkRect2D sc;
    int i, drew = 0;
    if (s_state != ST_READY || !s_drawThis) return;
    if (slot != s_slot) {
        if (!s_saidSlot) { s_saidSlot = 1;
            plog(d, "range: record on slot %u but prepare ran for %u",
                 (unsigned)slot, (unsigned)s_slot); }
        return;
    }
    if (!s_pipeDisc || s_pipeRp != rp) {
        kill_pipes(d);
        if (!build_pipelines(d, rp)) {
            if (!s_saidPipe) { s_saidPipe = 1;
                plog(d, "range: the pipelines would not build - no range rings"); }
            s_drawThis = 0;
            return;
        }
    }
    rg_scissor(w, h, &sc);
    if (sc.extent.width == 0 || sc.extent.height == 0) return;
    /* NO Y FLIP, the marker pass's argument: these are the engine's screen y,
       which grows downward, and a positive height puts clip -1 on row 0 */
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)w; vp.height = (float)h;
    vp.minDepth = 0.5f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_vb[slot], &zero);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1,
                            &s_set[slot], 0, NULL);
    for (i = 0; i < s_h.ndraw; i++) {
        const TAGPU_RGDRAW* g = &s_h.draws[i];
        uint32_t bit = (uint32_t)g->bit;
        if (g->kind == TAGPU_RG_CLEAR) { clear_stencil(cb, &sc); continue; }
        if (g->kind == TAGPU_RG_DISC) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeDisc);
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, bit);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, bit);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, bit);
            vkCmdDraw(cb, 3u * (uint32_t)g->nseg, (uint32_t)g->count, 0, (uint32_t)g->first);
        } else if (g->kind == TAGPU_RG_OUTER) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeOuter);
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, bit);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, bit);
            vkCmdDraw(cb, 6u * (uint32_t)g->nseg, (uint32_t)g->count, 0, (uint32_t)g->first);
        } else {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeLine);
            vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdDraw(cb, 6u * (uint32_t)g->nseg, (uint32_t)g->count, 0, (uint32_t)g->first);
        }
        drew++;
    }
    /* what this pass found is what it leaves: the plane at 0 */
    clear_stencil(cb, &sc);
    if (!s_saidDrew && drew) {
        s_saidDrew = 1;
        plog(d, "range: drew %d draw(s), %d ring instance(s)", drew, s_h.ninst);
    }
}

void tagpu_vk_range_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    if (s_state == ST_UNBUILT && !s_pool) return;
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        if (s_ubo[i]) { vkDestroyBuffer(d->dev, s_ubo[i], NULL); vkFreeMemory(d->dev, s_uboMem[i], NULL); }
        if (s_vb[i])  { vkDestroyBuffer(d->dev, s_vb[i], NULL);  vkFreeMemory(d->dev, s_vbMem[i], NULL); }
        s_ubo[i] = s_vb[i] = VK_NULL_HANDLE;
        s_uboMem[i] = s_vbMem[i] = VK_NULL_HANDLE;
        s_uboMap[i] = s_vbMap[i] = NULL;
        s_uboCap[i] = s_vbCap[i] = 0;
        s_set[i] = VK_NULL_HANDLE;
    }
    kill_pipes(d);
    if (s_samp) { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_pool) { vkDestroyDescriptorPool(d->dev, s_pool, NULL); s_pool = VK_NULL_HANDLE; }
    if (s_plo)  { vkDestroyPipelineLayout(d->dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dsl)  { vkDestroyDescriptorSetLayout(d->dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    s_state = s_downPaying ? ST_REFUSED : ST_UNBUILT;
    s_drawThis = 0;
    s_downOwed = 0; s_downPaying = 0;
    s_saidStencil = s_saidHeight = s_saidRoom = s_saidBound = 0;
    s_saidPipe = s_saidSlot = s_saidDrew = 0;
}

int  tagpu_vk_range_down_owed(void) { return s_downOwed && !s_downPaying; }
void tagpu_vk_range_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_range_down(d);
}
