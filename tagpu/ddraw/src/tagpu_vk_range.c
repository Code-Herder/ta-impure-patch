/* tagpu_vk_range.c -- the weapon range rings, drawn by Vulkan.
   Contract: tagpu_vk_range.h. What is drawn: tagpu_range.h.

   THE REACH FIRST, ONCE A RING. A compute pass (tagpu_range_comp.h) walks the
   terrain for every direction of a ring and writes the radius and the surface
   altitude there into this slot's reach buffer; the disc and the two bands of
   the ring then read those points. A ring's block is kept from one use of the
   slot to the next while the ring's inputs -- centre, altitude, range,
   velocity, minbarrel, segments -- and the grid, the sea and gravity stay the
   same, so a structure's ring is walked once per slot and a moving unit's
   every frame. THE CACHE IS PER SLOT, and that is what makes reusing a block
   safe: a slot's buffer is read and written only by that slot's submissions,
   and the seam waits on the slot's fence before `prepare`.

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
#include "spirv/tagpu_range_comp.spv.h"

#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties)

#define DFNS(X) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkCmdClearAttachments) \
    X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkCmdPushConstants)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

/* the vertex stages' std140 block, as tagpu_range.spv.h prints it */
#define VS_SZ       48
#define VS_GAME      0
#define VS_ZOOM      8
#define VS_ZOOMC    16
#define VS_EYE      24
#define VS_MAPEXT   32
#define VS_PXGAME   40

/* the reach pass's push constants, tagpu_range_comp.h's PC in std430 */
#define PC_SZ       24
#define PC_HDIM      0
#define PC_SEA       8
#define PC_GRAV     12
#define PC_JOBS     16
#define PC_CAP      20

#define INST_STRIDE ((uint32_t)sizeof(TAGPU_RGINST))
#define JOB_STRIDE  48u                 /* three vec4 a job                    */
#define ENTRY_SZ    8u                  /* one direction's (radius, altitude)  */

/* THE RINGS ONE FRAME CAN HOLD, and the two block sizes the reach buffer is cut
   into: a ring is 128 or 1024 segments (tagpu_range.c's styles), and `prepare`
   refuses a draw list naming any other count. */
#define RG_MAXRING  4096
#define SEG_A       128
#define SEG_B       1024
#define HASH_N      8192                /* a power of two, twice RG_MAXRING    */

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

static int s_state;
static int s_downOwed, s_downPaying;
static int s_drawThis;
static uint32_t s_slot;

static VkDescriptorSetLayout s_dsl, s_cdsl;
static VkPipelineLayout      s_plo, s_cplo;
static VkPipeline            s_pipeDisc, s_pipeOuter, s_pipeLine, s_pipeReach;
static VkRenderPass          s_pipeRp;
static VkDescriptorPool      s_pool;
static VkDescriptorSet       s_set[TAGPU_VK_SLOTS], s_cset[TAGPU_VK_SLOTS];
static VkSampler             s_samp;

typedef struct {
    VkBuffer buf; VkDeviceMemory mem; unsigned char* map; VkDeviceSize cap;
} BUF;

/* a ring as the reach knows it: x, z, altitude, range, v, minbarrel,
   ballistic, scale, segments */
typedef struct { float k[9]; } RKEY;
typedef struct { RKEY key; int base; } RENT;

typedef struct {
    BUF ubo, vb, jobs, reach;
    /* the reach buffer's blocks: `capA` of SEG_A entries, then `capB` of SEG_B */
    int  capA, capB;
    int  freeA[RG_MAXRING]; int nfreeA;
    int  freeB[RG_MAXRING]; int nfreeB;
    /* the rings whose points the buffer holds, and what they were walked over */
    RENT ent[RG_MAXRING]; int nent;
    unsigned serial; int hW, hH; float sea, grav; int epoch;
} SLOTDATA;

static SLOTDATA s_sd[TAGPU_VK_SLOTS];

/* this frame's scratch, render thread only */
static RENT          s_uniq[RG_MAXRING];
static int           s_nuniq;
static int           s_instRing[TAGPU_RG_MAXINST];
static int           s_hash[HASH_N], s_hashOld[HASH_N];
static unsigned char s_taken[RG_MAXRING];
static float         s_jobBuf[RG_MAXRING * 12];
static TAGPU_RGINST  s_inst[TAGPU_RG_MAXINST];

static TAGPU_RGHAND s_h;
static unsigned s_cWalked, s_cKept;      /* rings walked / reused, a heartbeat */
/* ONE LATCH PER SITE: a latch shared by two refusals lets the first silence
   the second for the session. */
static int s_saidUp, s_saidStencil, s_saidHeight, s_saidRoom, s_saidBound;
static int s_saidPipe, s_saidSlot, s_saidDrew, s_saidGrow;

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

static void kill_buf(const TAGPU_VKPASS* d, BUF* b)
{
    if (b->buf) vkDestroyBuffer(d->dev, b->buf, NULL);
    if (b->mem) vkFreeMemory(d->dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* `host` 1: host-visible and mapped; 0: device-local, which only the GPU
   writes (the reach), falling back to any type the buffer allows */
static int mk_buf(const TAGPU_VKPASS* d, VkDeviceSize size, VkBufferUsageFlags use, int host,
                  BUF* b)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    int type;
    void* p = NULL;
    memset(b, 0, sizeof *b);
    bci.size = size;
    bci.usage = use;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, &b->buf) != VK_SUCCESS) { b->buf = VK_NULL_HANDLE; return 0; }
    vkGetBufferMemoryRequirements(d->dev, b->buf, &req);
    type = host ? mem_type(d, req.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                : mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!host && type < 0) type = mem_type(d, req.memoryTypeBits, 0);
    if (type < 0) goto bad;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &b->mem) != VK_SUCCESS) { b->mem = VK_NULL_HANDLE; goto bad; }
    if (vkBindBufferMemory(d->dev, b->buf, b->mem, 0) != VK_SUCCESS) goto bad;
    if (host) {
        if (vkMapMemory(d->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) goto bad;
        b->map = (unsigned char*)p;
    }
    b->cap = size;
    return 1;
bad:
    kill_buf(d, b);
    return 0;
}

/* a host buffer at least `size`: this slot's own, behind its fence */
static int need_buf(const TAGPU_VKPASS* d, BUF* b, VkDeviceSize size, VkBufferUsageFlags use)
{
    if (b->buf && b->cap >= size) return 1;
    kill_buf(d, b);
    return mk_buf(d, size, use, 1, b);
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

/* the draws: binding 0 the vertex stages' block, 1 the reach (spirv-gen's
   allocation by stage). The reach pass: 0 the height grid, 1 the jobs, 2 the
   reach, as tagpu_range_comp.h states them. */
static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[3];
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPushConstantRange pr;
    VkDescriptorPoolSize ps[3];
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    uint32_t i;

    memset(b, 0, sizeof b);
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    li.bindingCount = 2; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &s_dsl) != VK_SUCCESS) {
        s_dsl = VK_NULL_HANDLE; return 0;
    }
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) {
        s_plo = VK_NULL_HANDLE; return 0;
    }

    memset(b, 0, sizeof b);
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    li.bindingCount = 3; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &s_cdsl) != VK_SUCCESS) {
        s_cdsl = VK_NULL_HANDLE; return 0;
    }
    memset(&pr, 0, sizeof pr);
    pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pr.offset = 0; pr.size = PC_SZ;
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_cdsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_cplo) != VK_SUCCESS) {
        s_cplo = VK_NULL_HANDLE; return 0;
    }

    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         ps[0].descriptorCount = d->slots;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;         ps[1].descriptorCount = 3 * d->slots;
    ps[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[2].descriptorCount = d->slots;
    pi.maxSets = 2 * d->slots;
    pi.poolSizeCount = 3; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &pi, NULL, &s_pool) != VK_SUCCESS) {
        s_pool = VK_NULL_HANDLE; return 0;
    }
    ai.descriptorPool = s_pool;
    ai.descriptorSetCount = d->slots;
    ai.pSetLayouts = lay;
    for (i = 0; i < d->slots; i++) lay[i] = s_dsl;
    if (vkAllocateDescriptorSets(d->dev, &ai, s_set) != VK_SUCCESS) return 0;
    for (i = 0; i < d->slots; i++) lay[i] = s_cdsl;
    if (vkAllocateDescriptorSets(d->dev, &ai, s_cset) != VK_SUCCESS) return 0;
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

static int build_reach_pipeline(const TAGPU_VKPASS* d)
{
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkShaderModule m = mk_module(d, tagpu_spv_tagpu_range_comp_REACH_CS,
                                 sizeof tagpu_spv_tagpu_range_comp_REACH_CS / 4);
    VkResult r;
    if (!m) return 0;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = s_cplo;
    r = vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_pipeReach);
    vkDestroyShaderModule(d->dev, m, NULL);
    if (r != VK_SUCCESS) { s_pipeReach = VK_NULL_HANDLE; return 0; }
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
static void put_i(unsigned char* p, int off, int v)   { memcpy(p + off, &v, 4); }

static uint32_t key_hash(const RKEY* k)
{
    const unsigned char* p = (const unsigned char*)k;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < sizeof *k; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* index of `k` in `tab` (a HASH_N open-addressed table of indices into
   `ents`), or the free cell to put it in as -(cell + 1) */
static int key_find(const int* tab, const RENT* ents, const RKEY* k)
{
    uint32_t c = key_hash(k) & (HASH_N - 1);
    for (;;) {
        int e = tab[c];
        if (e < 0) return -(int)c - 1;
        if (!memcmp(&ents[e].key, k, sizeof *k)) return e;
        c = (c + 1) & (HASH_N - 1);
    }
}

static int seg_of(const RKEY* k) { return (int)k->k[8]; }

/* every block free again: nothing the buffer holds is kept */
static void reach_forget(SLOTDATA* sd)
{
    int j;
    sd->nent = 0;
    sd->nfreeA = sd->nfreeB = 0;
    for (j = sd->capA - 1; j >= 0; j--) sd->freeA[sd->nfreeA++] = j * SEG_A;
    for (j = sd->capB - 1; j >= 0; j--) sd->freeB[sd->nfreeB++] = sd->capA * SEG_A + j * SEG_B;
}

static int pow2_at_least(int n, int floor)
{
    int c = floor;
    while (c < n && c < RG_MAXRING) c <<= 1;
    return c < RG_MAXRING ? c : RG_MAXRING;
}

/* the slot's reach buffer with room for `needA` blocks of SEG_A and `needB`
   of SEG_B; a new buffer keeps nothing. The slot's last submission has been
   waited on (the seam's fence), so the old buffer is no one's. */
static int reach_room(const TAGPU_VKPASS* d, SLOTDATA* sd, int needA, int needB)
{
    int a, b;
    if (sd->reach.buf && needA <= sd->capA && needB <= sd->capB) return 1;
    a = pow2_at_least(needA > sd->capA ? needA : sd->capA, 256);
    b = pow2_at_least(needB > sd->capB ? needB : sd->capB, 16);
    kill_buf(d, &sd->reach);
    sd->capA = sd->capB = 0;
    reach_forget(sd);
    if (!mk_buf(d, (VkDeviceSize)a * SEG_A * ENTRY_SZ + (VkDeviceSize)b * SEG_B * ENTRY_SZ,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, &sd->reach))
        return 0;
    sd->capA = a; sd->capB = b;
    reach_forget(sd);
    if (!s_saidGrow) { s_saidGrow = 1;
        plog(d, "range: reach buffer for %d + %d rings, %u KB", a, b,
             (unsigned)(sd->reach.cap >> 10)); }
    return 1;
}

/* The draw list is checked before any of it reaches the device: each draw's
   instances inside the list, and every instance a draw reads with exactly
   the draw's segment count, 128 or 1024. That last one is the reach's read
   bound -- the vertex stages read entries base + k for k < the instance's
   count, which is inside the block its count chose. */
static int draws_ok(const TAGPU_VKPASS* d)
{
    int i, j;
    if (s_h.ninst > TAGPU_RG_MAXINST || s_h.ndraw > TAGPU_RG_MAXDRAW) {
        if (!s_saidBound) { s_saidBound = 1;
            plog(d, "range: %d instances / %d draws is past what this pass carries",
                 s_h.ninst, s_h.ndraw); }
        return 0;
    }
    for (i = 0; i < s_h.ninst; i++) {
        float n = s_h.inst[i].w[1];
        if (n != (float)SEG_A && n != (float)SEG_B) {
            if (!s_saidBound) { s_saidBound = 1;
                plog(d, "range: instance %d has %g segments, not %d or %d",
                     i, (double)n, SEG_A, SEG_B); }
            return 0;
        }
    }
    for (i = 0; i < s_h.ndraw; i++) {
        const TAGPU_RGDRAW* g = &s_h.draws[i];
        int bad = 0;
        if (g->kind == TAGPU_RG_CLEAR) continue;
        if (g->first < 0 || g->count <= 0 || g->first + g->count > s_h.ninst ||
            (g->nseg != SEG_A && g->nseg != SEG_B) ||
            (g->kind != TAGPU_RG_LINE && (g->bit < 1 || g->bit > 255)))
            bad = 1;
        for (j = 0; !bad && j < g->count; j++)
            if (s_h.inst[g->first + j].w[1] != (float)g->nseg) bad = 1;
        if (bad) {
            if (!s_saidBound) { s_saidBound = 1;
                plog(d, "range: draw %d is out of bounds - kind=%d bit=%d first=%d "
                        "count=%d nseg=%d of %d", i, g->kind, g->bit, g->first,
                        g->count, g->nseg, s_h.ninst); }
            return 0;
        }
    }
    return 1;
}

/* This frame's rings, each once, and which ring every instance draws. */
static int collect_rings(const TAGPU_VKPASS* d)
{
    int i;
    s_nuniq = 0;
    memset(s_hash, 0xFF, sizeof s_hash);
    for (i = 0; i < s_h.ninst; i++) {
        const TAGPU_RGINST* t = &s_h.inst[i];
        RKEY k;
        int e;
        memcpy(&k.k[0], t->c, 4 * sizeof(float));
        memcpy(&k.k[4], t->b, 4 * sizeof(float));
        k.k[8] = t->w[1];
        e = key_find(s_hash, s_uniq, &k);
        if (e < 0) {
            if (s_nuniq >= RG_MAXRING) {
                if (!s_saidBound) { s_saidBound = 1;
                    plog(d, "range: more than %d distinct rings in one frame", RG_MAXRING); }
                return 0;
            }
            s_hash[-e - 1] = s_nuniq;
            s_uniq[s_nuniq].key = k;
            s_uniq[s_nuniq].base = -1;
            e = s_nuniq++;
        }
        s_instRing[i] = e;
    }
    return 1;
}

/* Give every ring of this frame a block: the one it had last time this slot
   was used when the ring and what it was walked over are the same, else a new
   one, which gets a job. Returns the job count, or -1. */
static int assign_blocks(const TAGPU_VKPASS* d, SLOTDATA* sd, int hW, int hH, unsigned serial)
{
    int j, u, needA = 0, needB = 0, njob = 0;
    if (serial != sd->serial || hW != sd->hW || hH != sd->hH ||
        s_h.sea != sd->sea || s_h.grav != sd->grav) {
        reach_forget(sd);
        sd->serial = serial; sd->hW = hW; sd->hH = hH;
        sd->sea = s_h.sea; sd->grav = s_h.grav;
    }
    for (u = 0; u < s_nuniq; u++) {
        if (seg_of(&s_uniq[u].key) == SEG_A) needA++; else needB++;
    }
    if (needA > sd->capA || needB > sd->capB || !sd->reach.buf) {
        if (!reach_room(d, sd, needA, needB)) return -1;
    }
    /* keep what is still drawn, free the rest, then place the new ones */
    memset(s_hashOld, 0xFF, sizeof s_hashOld);
    for (j = 0; j < sd->nent; j++) {
        int e = key_find(s_hashOld, sd->ent, &sd->ent[j].key);
        if (e < 0) s_hashOld[-e - 1] = j;
        s_taken[j] = 0;
    }
    s_cKept = 0;
    for (u = 0; u < s_nuniq; u++) {
        int e = sd->nent ? key_find(s_hashOld, sd->ent, &s_uniq[u].key) : -1;
        if (e >= 0 && !s_taken[e]) {
            s_uniq[u].base = sd->ent[e].base;
            s_taken[e] = 1;
            s_cKept++;
        }
    }
    for (j = 0; j < sd->nent; j++) {
        if (s_taken[j]) continue;
        if (seg_of(&sd->ent[j].key) == SEG_A) sd->freeA[sd->nfreeA++] = sd->ent[j].base;
        else                                  sd->freeB[sd->nfreeB++] = sd->ent[j].base;
    }
    for (u = 0; u < s_nuniq; u++) {
        RENT* r = &s_uniq[u];
        float* jb;
        if (r->base >= 0) continue;
        if (seg_of(&r->key) == SEG_A) r->base = sd->freeA[--sd->nfreeA];
        else                          r->base = sd->freeB[--sd->nfreeB];
        jb = &s_jobBuf[njob * 12];
        memcpy(jb, r->key.k, 8 * sizeof(float));
        jb[8] = (float)r->base; jb[9] = r->key.k[8]; jb[10] = 0.0f; jb[11] = 0.0f;
        njob++;
    }
    memcpy(sd->ent, s_uniq, (size_t)s_nuniq * sizeof *s_uniq);
    sd->nent = s_nuniq;
    s_cWalked = (unsigned)njob;
    return njob;
}

int tagpu_vk_range_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    VkDescriptorBufferInfo bu, br, bj;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w[5];
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkImageView hv;
    SLOTDATA* sd;
    unsigned serial = 0;
    unsigned char pc[PC_SZ];
    int hW = 0, hH = 0, i, njob;
    unsigned char* u;

    s_drawThis = 0;
    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    s_slot = slot;
    sd = &s_sd[slot];
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
        if (!resolve(d) || !build_descriptors(d) || !build_reach_pipeline(d)) {
            plog(d, "range: an entry point, the descriptors or the reach pipeline would "
                    "not build - the range rings stay down");
            s_state = ST_REFUSED; s_downOwed = 1; return 0;
        }
        s_state = ST_READY;
        if (!s_saidUp) { s_saidUp = 1;
            plog(d, "range: up - %u frame slots", (unsigned)d->slots); }
    }
    /* THE RINGS' SHAPE IS THE TERRAIN'S: no height grid bound for this slot,
       no rings this frame */
    hv = tagpu_vk_terr_height(slot, &hW, &hH, &serial);
    if (!hv) {
        if (!s_saidHeight) { s_saidHeight = 1;
            plog(d, "range: the terrain pass bound no height grid for this frame - "
                    "no range rings while that is true"); }
        return 0;
    }
    s_saidHeight = 0;
    if (!draws_ok(d) || !collect_rings(d)) return 0;
    njob = assign_blocks(d, sd, hW, hH, serial);
    if (njob < 0 ||
        !need_buf(d, &sd->ubo, VS_SZ, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !need_buf(d, &sd->vb, (VkDeviceSize)s_h.ninst * INST_STRIDE,
                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !need_buf(d, &sd->jobs, (VkDeviceSize)(s_nuniq > 0 ? s_nuniq : 1) * JOB_STRIDE,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        if (!s_saidRoom) { s_saidRoom = 1;
            plog(d, "range: the device would not take this frame's buffers"); }
        reach_forget(sd);
        return 0;
    }
    for (i = 0; i < s_h.ninst; i++) {
        s_inst[i] = s_h.inst[i];
        s_inst[i].w[3] = (float)s_uniq[s_instRing[i]].base;
    }
    memcpy(sd->vb.map, s_inst, (size_t)s_h.ninst * INST_STRIDE);
    if (njob > 0) memcpy(sd->jobs.map, s_jobBuf, (size_t)njob * JOB_STRIDE);
    u = sd->ubo.map;
    memset(u, 0, VS_SZ);
    put_f(u, VS_GAME, s_h.gw);        put_f(u, VS_GAME + 4, s_h.gh);
    put_f(u, VS_ZOOM, s_h.zoom);
    put_f(u, VS_ZOOMC, s_h.zoomCx);   put_f(u, VS_ZOOMC + 4, s_h.zoomCy);
    put_f(u, VS_EYE, s_h.eyeX);       put_f(u, VS_EYE + 4, s_h.eyeY);
    put_f(u, VS_MAPEXT, s_h.mapW);    put_f(u, VS_MAPEXT + 4, s_h.mapH);
    put_f(u, VS_PXGAME, s_h.ss > 0.0f ? 1.0f / s_h.ss : 1.0f);

    memset(&bu, 0, sizeof bu); memset(&br, 0, sizeof br); memset(&bj, 0, sizeof bj);
    memset(&ii, 0, sizeof ii); memset(w, 0, sizeof w);
    bu.buffer = sd->ubo.buf;   bu.range = VS_SZ;
    br.buffer = sd->reach.buf; br.range = VK_WHOLE_SIZE;
    bj.buffer = sd->jobs.buf;  bj.range = VK_WHOLE_SIZE;
    ii.sampler = s_samp; ii.imageView = hv;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (i = 0; i < 5; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].descriptorCount = 1; }
    w[0].dstSet = s_set[slot];  w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bu;
    w[1].dstSet = s_set[slot];  w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &br;
    w[2].dstSet = s_cset[slot]; w[2].dstBinding = 0;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &ii;
    w[3].dstSet = s_cset[slot]; w[3].dstBinding = 1;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &bj;
    w[4].dstSet = s_cset[slot]; w[4].dstBinding = 2;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &br;
    vkUpdateDescriptorSets(d->dev, 5, w, 0, NULL);

    if (njob > 0) {
        /* THE HEIGHT GRID'S UPLOAD BEFORE THE WALK. The terrain pass's copy
           ends in a barrier whose second scope is the fragment stage (and its
           layout change happens there); a barrier whose first scope takes the
           fragment stage chains onto it, as the terrain's own vertex-stage
           barrier does. */
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        put_f(pc, PC_HDIM, (float)hW); put_f(pc, PC_HDIM + 4, (float)hH);
        put_f(pc, PC_SEA, s_h.sea);    put_f(pc, PC_GRAV, s_h.grav);
        put_i(pc, PC_JOBS, njob);
        put_i(pc, PC_CAP, sd->capA * SEG_A + sd->capB * SEG_B);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeReach);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_cplo, 0, 1,
                                &s_cset[slot], 0, NULL);
        vkCmdPushConstants(cb, s_cplo, VK_SHADER_STAGE_COMPUTE_BIT, 0, PC_SZ, pc);
        vkCmdDispatch(cb, (uint32_t)njob, 1, 1);
    }
    /* THE WALK BEFORE THE DRAWS, recorded every frame and not only after a
       dispatch: a kept block was written by an earlier submission of this
       slot, and a barrier's first scope covers every command submitted to the
       queue before it. */
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    if (d->frame % 300u == 0u)
        plog(d, "range: %d rings, %u walked, %u kept", s_nuniq, s_cWalked, s_cKept);
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
    vkCmdBindVertexBuffers(cb, 0, 1, &s_sd[slot].vb.buf, &zero);
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
        SLOTDATA* sd = &s_sd[i];
        kill_buf(d, &sd->ubo); kill_buf(d, &sd->vb);
        kill_buf(d, &sd->jobs); kill_buf(d, &sd->reach);
        sd->capA = sd->capB = 0;
        reach_forget(sd);
        sd->serial = 0; sd->hW = sd->hH = 0;
        s_set[i] = s_cset[i] = VK_NULL_HANDLE;
    }
    kill_pipes(d);
    if (s_pipeReach) { vkDestroyPipeline(d->dev, s_pipeReach, NULL); s_pipeReach = VK_NULL_HANDLE; }
    if (s_samp) { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_pool) { vkDestroyDescriptorPool(d->dev, s_pool, NULL); s_pool = VK_NULL_HANDLE; }
    if (s_plo)  { vkDestroyPipelineLayout(d->dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_cplo) { vkDestroyPipelineLayout(d->dev, s_cplo, NULL); s_cplo = VK_NULL_HANDLE; }
    if (s_dsl)  { vkDestroyDescriptorSetLayout(d->dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_cdsl) { vkDestroyDescriptorSetLayout(d->dev, s_cdsl, NULL); s_cdsl = VK_NULL_HANDLE; }
    s_state = s_downPaying ? ST_REFUSED : ST_UNBUILT;
    s_drawThis = 0;
    s_downOwed = 0; s_downPaying = 0;
    s_saidStencil = s_saidHeight = s_saidRoom = s_saidBound = 0;
    s_saidPipe = s_saidSlot = s_saidDrew = s_saidGrow = 0;
}

int  tagpu_vk_range_down_owed(void) { return s_downOwed && !s_downPaying; }
void tagpu_vk_range_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_range_down(d);
}
