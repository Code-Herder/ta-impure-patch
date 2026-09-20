/* tagpu_vk_hires.c -- the replacement meshes' CASTERS, drawn by Vulkan.
   Contract: tagpu_vk_hires.h. Phase G, the Vulkan-only plan's gate 3b.

   THIS PASS DRAWS NOTHING, AND HAS SINCE BEFORE LANDING 11 D3 DELETED ITS
   PRODUCER. `hires_handover` below is a stub returning 0 and the block above it
   says why. Everything in this header comment is the design it was built to,
   in the PAST tense, kept because it is what a revival has to re-establish.

   WHY THIS PASS WAS SMALL, AND WHY IT WAS THE ONE THAT UNBLOCKED THE LANE.
   `tagpu_shadow.c` counted every caster in the GL cast-shadow map that this
   side of the seam had no copy of. A tacli instance ships `hires/armpw.glb`
   ACTIVE, so one Peewee on screen made that count 1 and a 257-unit crowd made
   it 16 -- and a map with an uncounted caster was one `tagpu_vk_shadow.c`
   refused to draw at all, which stood the terrain and unit passes down behind
   it. So gate 3b was not "port the glTF renderer": it was "put those
   silhouettes in the map". The bodies stayed with `tagpu_hires_draw.c`'s GL
   program, which was the whole difference between this file and its 1400-line
   siblings. `tagpu_shadow.c` went to landing 11 D2 and `tagpu_hires_draw.c` to
   D3, so neither the census nor the GL map exists now.

   WHAT THE ORACLE WAS. `tagpu_hires_depth` in that same file, and this pass was
   fed by the record IT wrote as it drew (`tagpu_hires_draw.h`,
   `tagpu_hires_handover`) rather than by a second walk over the units. Three
   things dropped a unit from the GL map -- `castSkip`, a VAO that would not
   build, a group whose count was zero -- and a re-derivation was free to
   disagree with the original about any of them while both looked right. That is
   the reason a revival wants a producer beside the geometry, not a second walk.

   ---- THE ALBEDO IS NOT CARRIED, AND THAT IS CHECKED RATHER THAN HOPED ----

   The fragment shader's depth path, in full:

       vec4 tex = texture(uAlbedo, vUV);
       if (uCutoff >= 0.0 && tex.a * uBase.a < uCutoff) discard;
       if (uDepthPass == 1) { frag = vec4(0.0); return; }

   One sampler is read and one branch can discard. Every other sampler the
   shader declares (uNormal, uLUT, uPal, uScaf, uFogGrid, uFogLUT) is untouched
   on this path and needs a VALID DESCRIPTOR and nothing more -- a set cannot be
   bound with a hole in it. And because the hand-over refuses any frame carrying
   a group with `cutoff >= 0` (`cutoutSeen`), the one branch that reads the
   sample cannot be taken, which makes a 1x1 white stand-in not an approximation
   of the albedo but exactly equivalent TO THIS PATH. Ship the textures when the
   bodies are ported; until then the deferral costs nothing and hides nothing.

   ---- TWO SIMPLIFICATIONS AGAINST THE SIBLING PASSES, BOTH DELIBERATE ----

   THE VERTEX BUFFERS ARE HOST-VISIBLE, not staged through a transfer. The unit
   pass stages because it moves a 21 MB atlas every frame; these are unit-sized
   meshes written ONCE per load and then only read, so a staging buffer, a copy
   and a barrier would be apparatus with nothing to do. What is not simplified
   is their LIFETIME -- see the retire below.

   THE UNIFORM BLOCKS ARE DYNAMIC OFFSETS into one per-slot buffer, one vertex
   block per unit and one fragment block per material group, which is the shape
   tagpu_vk_unit.c uses at bindings 0/1/32. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "tagpu_vk_hires.h"
#include "tagpu_hires.h"   /* TAGPU_HMAXPIECE -- all that is left of that header */
#include "spirv/tagpu_hires_draw.spv.h"

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
    X(vkCmdDraw) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

/* the std140 blocks the generated SPIR-V header prints, and the only place
   these numbers are written down in this file */
#define VS_SZ      2480
#define VS_PIECE      0        /* vec4 uPiece[144] -- 48 pieces x 3          */
#define VS_GAME    2304
#define VS_OFFSET  2312
#define VS_ZOOM    2320
#define VS_ZOOMC   2328
#define VS_DSCALE  2336
#define VS_ANCHOR  2352
#define VS_YAWENC  2368
#define VS_SLANT   2380
#define VS_DEPTH   2384
#define VS_SHMAT   2400
#define VS_CAST    2464

/* THE BOUND AT THE MEMCPY BELOW IS `TAGPU_HMAXPIECE`; THE DESTINATION IS
   `VS_SZ`. Nothing derives one from the other -- the numbers above are copied
   by hand from what the generated SPIR-V header prints -- so raising
   TAGPU_HMAXPIECE and regenerating the shader would widen the bound and leave
   the block its old size, and `np * 12 * sizeof(float)` would run past
   `uPiece` into the projection uniforms and then off the end.

   A comment asking the next maintainer to remember this is what was here
   before, and it was WRONG about which file sizes what. This is the bound
   instead: a negative array size is the C99 way to fail at compile time (the
   build is -std=c99, so `_Static_assert` is not available), and it fails
   LOUDLY -- `size of array is negative` naming this line. If you widen the
   uniform block on purpose, update VS_SZ/VS_GAME from the generated header and
   this assertion goes quiet by itself. [Landing 11 D3's review, finding M1.] */
typedef char tagpu_vk_hires_upiece_fits[
    (TAGPU_HMAXPIECE * 12 * (int)sizeof(float) <= VS_GAME - VS_PIECE) ? 1 : -1];


#define FS_SZ       208
#define FS_BASE      80
#define FS_CUTOFF   104
#define FS_DEPTH    108

#define HI_VSTRIDE   36        /* bytes per vertex: HVSTRIDE floats          */
#define MAXVB         8        /* cached mesh buffers; tagpu_hires.c's MAXMESH */

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

static int s_state;
static int s_downOwed, s_downPaying;

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;
static VkRenderPass          s_pipeRp;
static VkDescriptorPool      s_pool;
static VkDescriptorSet       s_set[TAGPU_VK_SLOTS];

/* the 1x1 stand-in every sampler binding names; see the header block */
static VkImage        s_whiteImg;
static VkDeviceMemory s_whiteMem;
static VkImageView    s_whiteView;
static VkSampler      s_samp;
static int            s_whiteReady;

/* one uniform buffer per slot, mapped for its life */
static VkBuffer       s_ubo[TAGPU_VK_SLOTS];
static VkDeviceMemory s_uboMem[TAGPU_VK_SLOTS];
static unsigned char* s_uboMap[TAGPU_VK_SLOTS];
static VkDeviceSize   s_uboCap[TAGPU_VK_SLOTS];

/* THE MESH BUFFERS, AND THEIR RETIRE. A .glb is hot-reloaded, so the triangles
   under a cached buffer can change mid-session -- and destroying a buffer that
   command buffers already submitted still name is the use-after-free this phase
   has already shipped once (gate 2's shared image) and nearly shipped twice
   (gate 3a's mip chain). The rule is the one tagpu_vk_terr.c established:
   A BUFFER MAY ONLY BE DESTROYED ONCE EVERY SLOT THAT USED IT HAS TURNED OVER
   UNDER ITS OWN FENCE. `usedBy` records which slots named it; a retire snapshots
   that into `pending`, and entering slot i clears bit i -- because the seam's
   fence wait at the top of slot i's frame is the proof, and the only proof,
   that slot i's previous submit has completed. The frame that asks for the
   retire, and every frame until `pending` is empty, DRAWS NOTHING: a caster
   this pass cannot draw is a map the shadow pass must refuse, which it already
   does on the count this file returns. */
typedef struct {
    const float*   key;        /* the CPU array; identity, not ownership      */
    unsigned       gen;
    int            ntri;
    VkBuffer       buf;
    VkDeviceMemory mem;
    unsigned       usedBy;     /* slots that have named it in a submit        */
    unsigned       pending;    /* slots still to turn over before the destroy */
    int            retiring;
} VB;
static VB s_vb[MAXVB];

/* this frame's plan, filled by `upload` and consumed by `cast` */
static TAGPU_HIHAND s_h;
static int  s_have;            /* the hand-over was taken this frame          */
static int  s_ready;           /* ...and every caster in it can be drawn      */
static int  s_vbOf[TAGPU_HI_MAXHAND];   /* hand-over mesh index -> s_vb index */
static VkDeviceSize s_vsOff[TAGPU_HI_MAXHAND];  /* per unit  */
static VkDeviceSize s_fsOff[TAGPU_HI_MAXHAND * 32]; /* per group */
static VkDeviceSize s_ualign = 256;
static uint32_t s_slot;

static int s_saidNoZclip, s_saidCutout, s_saidPipe, s_saidRoom;

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
                     VkMemoryPropertyFlags want, VkBuffer* buf, VkDeviceMemory* mem,
                     unsigned char** map)
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
    type = mem_type(d, req.memoryTypeBits, want);
    if (type < 0) goto bad;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) { *mem = VK_NULL_HANDLE; goto bad; }
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) goto bad;
    if (map) {
        if (vkMapMemory(d->dev, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) goto bad;
        *map = (unsigned char*)p;
    }
    return 1;
bad:
    /* EVERY OUT-PARAM IS NULL ON FAILURE, from every exit -- the guarantee the
       gate-3a verification pass found missing in the unit pass's own helper,
       where it was a property of the call sites instead. */
    if (*mem) vkFreeMemory(d->dev, *mem, NULL);
    if (*buf) vkDestroyBuffer(d->dev, *buf, NULL);
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
    if (map) *map = NULL;
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

/* ---- the 1x1 stand-in ---------------------------------------------------- */

static int build_white(const TAGPU_VKPASS* d, VkCommandBuffer cb)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkClearColorValue white;
    VkImageSubresourceRange rg;
    int mt;

    if (s_whiteReady) return 1;
    if (s_whiteImg) return 0;               /* a previous attempt failed */

    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = 1; ii.extent.height = 1; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ii, NULL, &s_whiteImg) != VK_SUCCESS) {
        s_whiteImg = VK_NULL_HANDLE; return 0;
    }
    vkGetImageMemoryRequirements(d->dev, s_whiteImg, &mr);
    mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) return 0;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(d->dev, &ai, NULL, &s_whiteMem) != VK_SUCCESS) {
        s_whiteMem = VK_NULL_HANDLE; return 0;
    }
    if (vkBindImageMemory(d->dev, s_whiteImg, s_whiteMem, 0) != VK_SUCCESS) return 0;
    vi.image = s_whiteImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &vi, NULL, &s_whiteView) != VK_SUCCESS) {
        s_whiteView = VK_NULL_HANDLE; return 0;
    }
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(d->dev, &si, NULL, &s_samp) != VK_SUCCESS) {
        s_samp = VK_NULL_HANDLE; return 0;
    }

    /* OPAQUE WHITE, and the alpha is the point: the one branch that reads this
       texel tests `tex.a * uBase.a < uCutoff`, and a cleared-to-zero image
       would make it discard on any frame that reached it. The frames that could
       reach it are refused, so this is belt and braces -- but an image whose
       contents are UNDEFINED is what gate 3a's own review caught in the UI
       lane, and it is not a mistake to make twice. */
    rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.baseMipLevel = 0; rg.levelCount = 1;
    rg.baseArrayLayer = 0; rg.layerCount = 1;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s_whiteImg;
    b.subresourceRange = rg;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.0f;
    vkCmdClearColorImage(cb, s_whiteImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &white, 1, &rg);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    s_whiteReady = 1;
    return 1;
}

/* ---- descriptors and the pipeline ---------------------------------------- */

#define NSAMP 7
static const uint32_t SAMP_BIND[NSAMP] = { 40, 41, 42, 43, 44, 45, 46 };

static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[2 + NSAMP];
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
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
    (void)lay;
    return 1;
}

static void vertex_layout(VkVertexInputBindingDescription* vb,
                          VkVertexInputAttributeDescription* va,
                          VkPipelineVertexInputStateCreateInfo* vi)
{
    memset(vb, 0, sizeof *vb);
    vb[0].binding = 0;
    vb[0].stride = HI_VSTRIDE;
    vb[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(va, 0, sizeof *va * 4);
    va[0].location = 0; va[0].binding = 0; va[0].format = VK_FORMAT_R32G32B32_SFLOAT; va[0].offset = 0;
    va[1].location = 1; va[1].binding = 0; va[1].format = VK_FORMAT_R32G32B32_SFLOAT; va[1].offset = 12;
    va[2].location = 2; va[2].binding = 0; va[2].format = VK_FORMAT_R32G32_SFLOAT;    va[2].offset = 24;
    va[3].location = 3; va[3].binding = 0; va[3].format = VK_FORMAT_R32_SFLOAT;       va[3].offset = 32;
    memset(vi, 0, sizeof *vi);
    vi->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi->vertexBindingDescriptionCount = 1;
    vi->pVertexBindingDescriptions = vb;
    vi->vertexAttributeDescriptionCount = 4;
    vi->pVertexAttributeDescriptions = va;
}

static int build_pipeline(const TAGPU_VKPASS* d, VkRenderPass rp)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[1];
    VkVertexInputAttributeDescription va[4];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineViewportDepthClipControlCreateInfoEXT zc =
        { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    int ok = 0;

    vs = mk_module(d, tagpu_spv_tagpu_hires_draw_VS,
                   sizeof tagpu_spv_tagpu_hires_draw_VS / 4);
    fs = mk_module(d, tagpu_spv_tagpu_hires_draw_FS,
                   sizeof tagpu_spv_tagpu_hires_draw_FS / 4);
    if (!vs || !fs) goto done;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;

    vertex_layout(vb, va, &vi);
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* GL'S CLIP-SPACE Z, for the same reason the posed caster pipeline needs
       it: tagpu_shadow.c's matrix fills [-1, 1] by construction, so without
       this the near half of every caster is clipped away. The caller has
       already refused to get here without `zclipok`. */
    zc.negativeOneToOne = VK_TRUE;
    vp.pNext = &zc;
    vp.viewportCount = 1; vp.scissorCount = 1;

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.maxDepthBounds = 1.0f;

    cb.attachmentCount = 0;                 /* the subpass has no colour one   */
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
    gp.layout = s_plo;
    gp.renderPass = rp;
    gp.subpass = 0;
    ok = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL,
                                   &s_pipe) == VK_SUCCESS;
    if (ok) s_pipeRp = rp;
done:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

/* ---- the mesh buffers and their retire ----------------------------------- */

static void vb_kill(const TAGPU_VKPASS* d, VB* e)
{
    if (e->buf) vkDestroyBuffer(d->dev, e->buf, NULL);
    if (e->mem) vkFreeMemory(d->dev, e->mem, NULL);
    memset(e, 0, sizeof *e);
}

/* Entering slot `slot` proves, by the seam's fence wait and nothing else, that
   everything slot `slot` previously submitted has completed. That is the only
   fact this retire rests on. */
static void vb_slot_turned(const TAGPU_VKPASS* d, uint32_t slot)
{
    int i;
    for (i = 0; i < MAXVB; i++) {
        VB* e = &s_vb[i];
        if (!e->buf || !e->retiring) continue;
        e->pending &= ~(1u << slot);
        e->usedBy &= ~(1u << slot);
        if (!e->pending) vb_kill(d, e);
    }
}

/* The device buffer for one hand-over mesh, or -1. Never destroys anything
   here: a changed `gen` asks for a retire and refuses the frame. */
static int vb_for(const TAGPU_VKPASS* d, const TAGPU_HIMESH* m)
{
    int i, free = -1;
    VkDeviceSize sz;
    for (i = 0; i < MAXVB; i++) {
        VB* e = &s_vb[i];
        if (!e->buf) { if (free < 0) free = i; continue; }
        if (e->key != m->v) continue;
        if (e->retiring) return -1;
        if (e->gen == m->gen && e->ntri == m->ntri) return i;
        /* the file was reloaded under us */
        e->retiring = 1;
        e->pending = e->usedBy;
        if (!e->pending) { vb_kill(d, e); if (free < 0) free = i; }
        return -1;
    }
    if (free < 0) return -1;
    /* THE STRIDE IS CARRIED SO THAT IT CAN BE CHECKED, and the first draft
       carried it and ignored it. `HI_VSTRIDE` and `vertex_layout`'s offsets are
       this file's copy of the producer's `HVSTRIDE`; if that ever changes, a
       copy sized on ours reads past the producer's array by
       `ntri * 3 * (36 - 4 * stride)` bytes and draws a wrong picture either
       way. Refusing is the pass's own idiom and costs a branch.
       [The gate-3b landing review's finding 2.] */
    if (m->stride * 4 != HI_VSTRIDE) return -1;
    sz = (VkDeviceSize)m->ntri * 3 * HI_VSTRIDE;
    if (sz == 0) return -1;
    {
        unsigned char* map = NULL;
        VB* e = &s_vb[free];
        if (!mk_buffer(d, sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &e->buf, &e->mem, &map))
            return -1;
        memcpy(map, m->v, (size_t)sz);
        e->key = m->v; e->gen = m->gen; e->ntri = m->ntri;
        e->usedBy = 0; e->pending = 0; e->retiring = 0;
        return free;
    }
}

/* ---- the frame ----------------------------------------------------------- */

static void put_f(unsigned char* p, int off, float v) { memcpy(p + off, &v, 4); }
static void put_i(unsigned char* p, int off, int v)   { memcpy(p + off, &v, 4); }

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a)
{
    return a <= 1 ? v : ((v + a - 1) / a) * a;
}

static int slot_ubo(const TAGPU_VKPASS* d, uint32_t slot, VkDeviceSize need)
{
    if (s_ubo[slot] && s_uboCap[slot] >= need) return 1;
    /* A GROW DESTROYS A BUFFER THIS SLOT MAY STILL HAVE IN FLIGHT -- except
       that it cannot: the seam's fence wait at the top of this slot's frame has
       already proved this slot's previous submit finished, and no other slot
       ever names this slot's buffer. That is why the uniform buffers are PER
       SLOT and the mesh buffers, which every slot names, are not. */
    if (s_ubo[slot]) {
        vkDestroyBuffer(d->dev, s_ubo[slot], NULL);
        vkFreeMemory(d->dev, s_uboMem[slot], NULL);
        s_ubo[slot] = VK_NULL_HANDLE; s_uboMem[slot] = VK_NULL_HANDLE;
        s_uboMap[slot] = NULL; s_uboCap[slot] = 0;
    }
    if (!mk_buffer(d, need, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_ubo[slot], &s_uboMem[slot], &s_uboMap[slot]))
        return 0;
    s_uboCap[slot] = need;
    return 1;
}

static void write_set(const TAGPU_VKPASS* d, uint32_t slot)
{
    VkDescriptorBufferInfo bi[2];
    VkDescriptorImageInfo ii[NSAMP];
    VkWriteDescriptorSet w[2 + NSAMP];
    uint32_t i;
    int n = 0;
    memset(bi, 0, sizeof bi);
    memset(ii, 0, sizeof ii);
    memset(w, 0, sizeof w);
    bi[0].buffer = s_ubo[slot]; bi[0].offset = 0; bi[0].range = VS_SZ;
    bi[1].buffer = s_ubo[slot]; bi[1].offset = 0; bi[1].range = FS_SZ;
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
        ii[i].imageView = s_whiteView;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = s_set[slot]; w[n].dstBinding = SAMP_BIND[i];
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &ii[i]; n++;
    }
    vkUpdateDescriptorSets(d->dev, (uint32_t)n, w, 0, NULL);
}

/* THE HAND-OVER, WHICH NOTHING PUBLISHES. Its producer was
   `tagpu_hires_depth` in `tagpu_hires_draw.c`, and that function had ZERO call
   sites well before landing 11 D3 deleted the file. `s_hiHave` was assigned in
   five places across three functions, but the ONLY assignment of 1 was at
   `tagpu_hires_draw.c:611`, inside that callerless function; the other four
   wrote 0. So the flag was never set and `tagpu_hires_handover` returned 0 on
   every frame of every session. There was a second, independent reason the same pass could not
   fire: `tagpu_hires_draw_ready()` was 0 because `opengl32.dll` is never in the
   process, so `tagpu_native.c` nulled every replacement-mesh pointer and the
   hand-over had nothing to carry even if something had published one.

   So this returns exactly what the deleted function returned, and D3 changes no
   behaviour -- which is the whole claim it has to make.

   REVIVING THIS IS A FEATURE LANDING AND IT NEEDS A LOADER FIRST. glTF parsing,
   the piece table, the material grouping and the COB-driven pose went with
   `tagpu_hires.c`; they are in git at this landing's parent. What this pass
   still owns is everything below: the vertex buffers, the descriptors, the
   pose uniform block and the caster draw. [The vulkan-only plan, landing 11 D3;
   gpu-status 2.80.] */
static int hires_handover(TAGPU_HIHAND* out, unsigned now)
{
    (void)out; (void)now;
    return 0;
}

void tagpu_vk_hires_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    VkPhysicalDeviceProperties props;
    VkDeviceSize vstride, fstride, need, off;
    int i, k;

    s_have = 0; s_ready = 0;
    if (s_state == ST_REFUSED) return;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return;
    s_slot = slot;

    if (s_state == ST_UNBUILT) {
        if (!resolve(d)) { s_state = ST_REFUSED; s_downOwed = 1; return; }
        vkGetPhysicalDeviceProperties(d->pd, &props);
        s_ualign = props.limits.minUniformBufferOffsetAlignment;
        if (s_ualign == 0) s_ualign = 1;
        if (props.limits.maxUniformBufferRange < VS_SZ) {
            plog(d, "hires: maxUniformBufferRange is %u and the pose block needs "
                    "%d - the replacement meshes' casters stay with GL",
                 (unsigned)props.limits.maxUniformBufferRange, VS_SZ);
            s_state = ST_REFUSED; s_downOwed = 1; return;
        }
        if (!build_descriptors(d)) {
            plog(d, "hires: no descriptors for the caster pass");
            s_state = ST_REFUSED; s_downOwed = 1; return;
        }
        s_state = ST_READY;
    }

    /* the fence that got us here is the proof a retire has been waiting for */
    vb_slot_turned(d, slot);

    if (!build_white(d, cb)) {
        if (!s_saidRoom) {
            s_saidRoom = 1;
            plog(d, "hires: no 1x1 stand-in for the sampler bindings - the "
                    "caster pass draws nothing and the map stands down");
        }
        return;
    }

    /* THE PRODUCER IS GONE. This used to ask `tagpu_hires_verts_want()` to keep
       the CPU triangle copy, because this pass was the only consumer that
       needed it. Landing 11 D3 deleted `tagpu_hires.c` and `tagpu_hires_draw.c`
       with the rest of the GL lane, on the owner's ruling that glTF replacement
       models are disabled and their implementation is TODO and out of scope. */

    if (!hires_handover(&s_h, d->frame)) return;
    s_have = 1;
    if (!s_h.depthOn || s_h.nunit <= 0) return;

    /* THE ONE FRAME SHAPE THIS PASS CANNOT REPRODUCE. See the file header: the
       albedo does not cross, and a group that actually cuts out is a hole in
       the GL map this lane would leave filled. */
    if (s_h.cutoutSeen) {
        if (!s_saidCutout) {
            s_saidCutout = 1;
            plog(d, "hires: a replacement mesh draws an alpha-cutout group into "
                    "the shadow map and this lane carries no albedo - nothing "
                    "drawn while that is true");
        }
        return;
    }
    if (s_h.nunit > TAGPU_HI_MAXHAND || s_h.nmesh > TAGPU_HI_MAXHAND ||
        s_h.ngroup > TAGPU_HI_MAXHAND * 32)
        return;

    for (i = 0; i < s_h.nmesh; i++) {
        s_vbOf[i] = vb_for(d, &s_h.meshes[i]);
        if (s_vbOf[i] < 0) return;          /* retiring, or no room: refuse */
    }

    vstride = align_up(VS_SZ, s_ualign);
    fstride = align_up(FS_SZ, s_ualign);
    need = vstride * (VkDeviceSize)s_h.nunit + fstride * (VkDeviceSize)s_h.ngroup;
    if (!slot_ubo(d, slot, need)) return;
    write_set(d, slot);

    off = 0;
    for (i = 0; i < s_h.nunit; i++) {
        const TAGPU_HIUREC* u = &s_h.units[i];
        unsigned char* p = s_uboMap[slot] + off;
        int np = u->npose;
        /* AND THE DESTINATION, not only the source. `np` bounded the read out
           of `rows` and nothing bounded the write into this unit's 2480-byte
           block -- `uPiece` is 144 vec4, so `np > 48` walks off the end of it.
           The producer clamps to TAGPU_HMAXPIECE today; this is a seam struct
           and a value that crosses one is DATA until it has been bounded here.
           [The gate-3b landing review's finding 3.] */
        if (np < 0 || np > TAGPU_HMAXPIECE) return;
        memset(p, 0, VS_SZ);
        if (np > 0 && u->rowOff + (unsigned)np * 12 <= s_h.nrow)
            memcpy(p + VS_PIECE, s_h.rows + u->rowOff, (size_t)np * 12 * sizeof(float));
        /* THE PROJECTION UNIFORMS STILL HAVE TO BE FINITE. The vertex shader
           takes gl_Position from uShadowMat alone on this path, but it still
           EVALUATES the discarded branch, and a zero uGame makes that NaN on a
           strict driver -- tagpu_posedraw.c's depth pass says the same and for
           the same reason. */
        put_f(p, VS_GAME, 1.0f); put_f(p, VS_GAME + 4, 1.0f);
        put_f(p, VS_OFFSET, 0.0f); put_f(p, VS_OFFSET + 4, 0.0f);
        put_f(p, VS_ZOOM, 1.0f);
        put_f(p, VS_ZOOMC, 0.0f); put_f(p, VS_ZOOMC + 4, 0.0f);
        put_f(p, VS_DSCALE, 1.0f);
        memcpy(p + VS_ANCHOR, u->anchor, sizeof u->anchor);
        memcpy(p + VS_YAWENC, u->yawEnc, sizeof u->yawEnc);
        put_i(p, VS_SLANT, 0);
        put_i(p, VS_DEPTH, 1);
        memcpy(p + VS_SHMAT, s_h.shadowMat, sizeof s_h.shadowMat);
        memcpy(p + VS_CAST, u->cast, sizeof u->cast);
        s_vsOff[i] = off;
        off += vstride;
    }
    for (k = 0; k < s_h.ngroup; k++) {
        const TAGPU_HIGREC* g = &s_h.groups[k];
        unsigned char* p = s_uboMap[slot] + off;
        memset(p, 0, FS_SZ);
        memcpy(p + FS_BASE, g->base, sizeof g->base);
        put_f(p, FS_CUTOFF, g->cutoff);
        put_i(p, FS_DEPTH, 1);
        s_fsOff[k] = off;
        off += fstride;
    }
    s_ready = 1;
}

int tagpu_vk_hires_casters(void)
{
    return (s_state == ST_READY && s_ready) ? s_h.nunit : 0;
}

int tagpu_vk_hires_cast(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                        VkRenderPass rp)
{
    VkDeviceSize zero = 0;
    int i, drew = 0;

    if (s_state != ST_READY || !s_ready) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS || slot != s_slot) return 0;
    if (!d->zclipok) {
        if (!s_saidNoZclip) {
            s_saidNoZclip = 1;
            plog(d, "hires: no VK_EXT_depth_clip_control, so a caster's near half "
                    "would be clipped away - the map is incomplete and says so");
        }
        return -1;
    }
    if (!s_pipe || s_pipeRp != rp) {
        if (s_pipe) { vkDestroyPipeline(d->dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
        if (!build_pipeline(d, rp)) {
            if (!s_saidPipe) {
                s_saidPipe = 1;
                plog(d, "hires: the caster pipeline would not build against the "
                        "shadow pass's render pass - the map is incomplete and "
                        "the pass says so rather than leaving a caster out of it");
            }
            return -1;
        }
    }

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    for (i = 0; i < s_h.nunit; i++) {
        const TAGPU_HIUREC* u = &s_h.units[i];
        const TAGPU_HIMESH* m;
        int vbi, k;
        if (u->mesh < 0 || u->mesh >= s_h.nmesh) return -1;
        m = &s_h.meshes[u->mesh];
        vbi = s_vbOf[u->mesh];
        if (vbi < 0 || !s_vb[vbi].buf) return -1;
        s_vb[vbi].usedBy |= (1u << slot);
        vkCmdBindVertexBuffers(cb, 0, 1, &s_vb[vbi].buf, &zero);
        for (k = 0; k < m->ngroup; k++) {
            int gi = m->grpOff + k;
            uint32_t dyno[2];
            const TAGPU_HIGREC* g;
            if (gi < 0 || gi >= s_h.ngroup) return -1;
            g = &s_h.groups[gi];
            if (g->count <= 0) continue;
            if (g->first < 0 || g->first + g->count > m->ntri * 3) return -1;
            dyno[0] = (uint32_t)s_vsOff[i];
            dyno[1] = (uint32_t)s_fsOff[gi];
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo,
                                    0, 1, &s_set[slot], 2, dyno);
            vkCmdDraw(cb, (uint32_t)g->count, 1, (uint32_t)g->first, 0);
        }
        drew++;
    }
    return drew;
}

/* ---- teardown ------------------------------------------------------------ */

void tagpu_vk_hires_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    int k;
    if (s_state == ST_UNBUILT && !s_pool) return;
    for (k = 0; k < MAXVB; k++) vb_kill(d, &s_vb[k]);
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        if (s_ubo[i]) vkDestroyBuffer(d->dev, s_ubo[i], NULL);
        if (s_uboMem[i]) vkFreeMemory(d->dev, s_uboMem[i], NULL);
        s_ubo[i] = VK_NULL_HANDLE; s_uboMem[i] = VK_NULL_HANDLE;
        s_uboMap[i] = NULL; s_uboCap[i] = 0;
        s_set[i] = VK_NULL_HANDLE;
    }
    if (s_pipe) { vkDestroyPipeline(d->dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    s_pipeRp = VK_NULL_HANDLE;
    if (s_pool) { vkDestroyDescriptorPool(d->dev, s_pool, NULL); s_pool = VK_NULL_HANDLE; }
    if (s_plo) { vkDestroyPipelineLayout(d->dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dsl) { vkDestroyDescriptorSetLayout(d->dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_whiteView) { vkDestroyImageView(d->dev, s_whiteView, NULL); s_whiteView = VK_NULL_HANDLE; }
    if (s_whiteImg) { vkDestroyImage(d->dev, s_whiteImg, NULL); s_whiteImg = VK_NULL_HANDLE; }
    if (s_whiteMem) { vkFreeMemory(d->dev, s_whiteMem, NULL); s_whiteMem = VK_NULL_HANDLE; }
    if (s_samp) { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    s_whiteReady = 0;
    s_state = ST_UNBUILT;
    s_have = s_ready = 0;
    s_downOwed = 0; s_downPaying = 0;
    s_saidNoZclip = s_saidCutout = s_saidPipe = s_saidRoom = 0;
}

int  tagpu_vk_hires_down_owed(void) { return s_downOwed && !s_downPaying; }
void tagpu_vk_hires_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_hires_down(d);
}
