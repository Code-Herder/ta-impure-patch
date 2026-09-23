/* tagpu_vk_mark.c -- the UI MARKERS, drawn by Vulkan.
   Contract: tagpu_vk_mark.h.

   WHAT FEEDS IT. `tagpu_mark_render` in tagpu_mark.c records a DRAW LIST as it
   gathers, and this pass draws exactly that list (`tagpu_mark_handover`). The
   list matters more here than in any pass before it: this is seven draws, not
   one, and they differ only in `uText`, `uFog` and which texture feeds
   `uLayer` -- the order markers and their labels first, then the bars over
   them, then the group digit, then the captured layer, and the build cursor
   last of all with fog OFF because the engine draws it after the fog overlay
   and never darkens it. Every one of those is a decision the
   gather made from state this side cannot see, so it is carried rather than
   re-derived.

   NO BLEND, AND NO DEPTH EXCEPT UNDER THE SELECTION RECTS, and that is the
   layer's rule rather than a shortcut: the markers are the frame's top layer
   and every fragment is opaque (tagpu_mark.h). A pipeline that blended here
   would be a different picture on every anti-aliased glyph edge. The
   selection rects alone test depth, because the engine draws each one inside
   the unit sweep, under its own unit (tagpu_mark.c).

   THE LINES ARE `ss` DEVICE PIXELS WIDE, which is one SCREEN pixel at any
   supersample -- the same rule tagpu_vk_fx.c follows, and the reason a marker
   stays a hairline at 4x instead of a 1997 pixel blown up to sixteen. Wide
   lines are an optional feature in Vulkan, so a frame with order lines on a
   device that will not draw them is REFUSED rather than drawn thin. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "tagpu_vk_mark.h"
#include "tagpu_vk_world.h"   /* the target's ss: the line width follows it */
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
    X(vkCmdSetLineWidth) X(vkCmdSetViewport) X(vkCmdSetScissor)

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

#define FS_SZ      48          /* SFS's block: FS's five, then uPx at 32 */
#define FS_KEY      0
#define FS_TEXT     4
#define FS_FOGORG   8
#define FS_FOGDIM  16
#define FS_FOG     24
#define FS_PX      32          /* SFS only; padding past FS's own block */

#define MVST       8           /* floats a vertex: x,y u,v wx,wz colour z */
#define MK_VSTRIDE (MVST * 4)

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };
/* IMG_NONE is the 1x1 0xFF stand-in that fills a sampler binding with no
   picture this frame. */
enum { IMG_NONE = 0, IMG_TEXT, IMG_PAL, IMG_FOG, IMG_LUT, IMG_N };

static int s_state;
static int s_downOwed, s_downPaying;
static int s_drawThis;

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipeTri, s_pipeLine;
/* THE SELECTION RECTS' PIPELINE: the line pipeline with the depth TEST on and
   the write off, so a rect lands under its own unit and every later row --
   where the engine's sweep draws it -- rather than over the finished world
   like every other marker. It is the only draw in this pass that reads depth,
   and the target's depth plane is still the world's when this pass records
   (tagpu_vk.c `world_records`: the markers are the last pass in the same
   render pass). Built with the line pipeline, and only when that is. */
static VkPipeline            s_pipeLineZ;
static VkRenderPass          s_pipeRp;
static VkDescriptorPool      s_pool;
/* TWO SETS PER SLOT, AND ONE OF THEM IS THE EMPTY ONE. `uLayer` is ONE
   sampler at binding 40, and Vulkan has no per-draw texture bind, so which
   image a draw samples is a property of the set: `s_setT` carries
   tagpu_text.c's coverage atlas for the label and digit draws, `s_setN` the
   1x1 stand-in for every draw that samples nothing (the bars, the rects, the
   order lines and dots), and `record` picks one per draw. The second is a set
   rather than a branch because a set cannot be bound with a hole and something
   must be at binding 40 either way.

   WITH ONE SET THE ATLAS IS NEVER BOUND AT ALL: every text draw reads the 1x1
   0xFF stand-in, so `texture(uLayer, vUV).r` is 1.0 for every fragment, the
   `< 0.5` discard never fires and each label and digit comes out a SOLID
   filled quad in its vertex colour. A bars-only A/B cannot see it -- there is
   no text draw in the frame -- and the first A/B that had labels in it
   measured 3 891 pixels. */
static VkDescriptorSet       s_setN[TAGPU_VK_SLOTS];
static VkDescriptorSet       s_setT[TAGPU_VK_SLOTS];
static VkSampler             s_samp;          /* NEAREST: every texel here is an
                                                 index or a coverage byte */

/* the five sampled images, PER FRAME SLOT. `w`/`h` are what they were built
   for, so a change of extent rebuilds.

   THEY CANNOT BE ONE SET SHARED BY EVERY SLOT. Every slot's `s_setN`/`s_setT`
   names these views, and the seam's fence makes frame `n` wait for frame
   `n - nimg` and nothing sooner (`tagpu_vk.c`, THE SEMAPHORE INDEXING) -- so
   with two swapchain images the previous frame's submission can still be
   sampling them. Shared, they would fault two ways, neither of which any A/B
   can show, because both need a second frame in flight:

     - the palette and the fog grid are re-uploaded EVERY frame, and `img_up`
       barriers SHADER_READ_ONLY -> TRANSFER_DST and copies over an image the
       previous submission may still be reading. Nothing orders the two;
     - `img_size` destroys and rebuilds on any change of extent, which would
       free an image a live command buffer names.

   Per slot is what `tagpu_vk_fx.c` ("the four small per-slot images") and
   `tagpu_vk_terr.c` ("the three small per-slot images") already do, and the one
   pass that genuinely shares a device resource carries a per-slot retirement
   mask for it instead (`tagpu_vk_unit.c`'s `VBRET.pending`). The cost is the
   text atlas uploading once per slot after a change rather than once; it is
   128 KB and it changes when a string is first rasterised. */
typedef struct {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    int            w, h;
    VkFormat       fmt;
    unsigned       gen;        /* the source's own serial, when it has one */
    int            have;
} IMG;
static IMG s_img[TAGPU_VK_SLOTS][IMG_N];

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
static int s_saidLine, s_saidWide, s_saidFog, s_saidLut, s_saidRoom, s_saidHand, s_saidDrew, s_saidIn;
static float s_lineW = 1.0f;   /* line width: THIS frame's target's `ss`  */
/* whether THIS frame's selection rects can be drawn -- settled in `prepare`,
   read in `record`. A frame whose rects cannot be is not refused whole the way
   an order-line frame is: the rects are dropped and every other marker still
   draws (see `prepare`). */
static int s_selOk;
static float s_selW;          /* the rects' band, target px -- set in record */
static int s_selDraw;         /* and whether this frame's can be drawn        */
static int s_saidSel, s_saidSelW;
static int s_saidPal;
/* ONE LATCH PER SITE HERE TOO: one latch over several distinct refusals lets
   the first to fire silence a DIFFERENT one for the rest of the session. */
static int s_saidEmpty, s_saidMany, s_saidBound, s_saidTexA;
/* ONE LATCH PER SITE. A single latch shared by six upload sites hides every
   failure after the first -- including a failure at a DIFFERENT site, which is
   the case that matters. */
static int s_saidImgT, s_saidImgP, s_saidImgF, s_saidImgU, s_saidImgS;
static int s_saidNoDraw, s_saidSlot;

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
    /* every out-param NULL from every exit */
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
    VkDescriptorSetLayout lay[2 * TAGPU_VK_SLOTS];
    VkDescriptorSet       all[2 * TAGPU_VK_SLOTS];
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
    ps[0].descriptorCount = 2 * 2 * d->slots;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = NSAMP * 2 * d->slots;
    pi.maxSets = 2 * d->slots;
    pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &pi, NULL, &s_pool) != VK_SUCCESS) {
        s_pool = VK_NULL_HANDLE; return 0;
    }
    for (i = 0; i < 2 * d->slots; i++) lay[i] = s_dsl;
    ai.descriptorPool = s_pool;
    ai.descriptorSetCount = 2 * d->slots;
    ai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &ai, all) != VK_SUCCESS) return 0;
    for (i = 0; i < d->slots; i++) {
        s_setN[i] = all[i];
        s_setT[i] = all[d->slots + i];
    }
    /* NEAREST on all four, and it is not a style choice: uLayer's texel IS a
       palette index and interpolating two of them gives a colour that is in
       neither. */
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
    VkVertexInputAttributeDescription va[5];
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
    VkPipelineRasterizationLineStateCreateInfoEXT lr =
        { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModule svs = VK_NULL_HANDLE, sfs = VK_NULL_HANDLE;
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
    va[4].location = 4; va[4].format = VK_FORMAT_R32_SFLOAT;    va[4].offset = 28;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = 5; vi.pVertexAttributeDescriptions = va;

    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* NO DEPTH. The render pass has a depth attachment, so a pipeline must
       still declare the state -- with testing and writes OFF: the markers are
       the frame's top layer. */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.maxDepthBounds = 1.0f;

    /* NO BLEND: every fragment here is opaque. */
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = VK_FALSE;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    /* TWO for the triangle pipeline: it rasterises no line, so declaring
       LINE_WIDTH there would be a dynamic state nothing sets and nothing
       reads. The line pipeline below raises it to three. */
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

    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeTri) != VK_SUCCESS)
        goto done;

    /* THE ORDER LINES, AND THE ONE PIECE OF STATE THAT FIXES THEIR FRAGMENTS.
       BRESENHAM is the diamond-exit rule these lines are specified by; Vulkan's
       DEFAULT mode is not it, and the difference is whole fragments along
       every diagonal. MEASURED without the chain: 4 900 pixels the default
       mode drew and the OpenGL renderer this replaced (diamond-exit lines) did
       not, on 436 segments of route line and range circle, with the OpenGL
       picture a near-perfect SUBSET of the default one.
       `tagpu_vk_fx.c` carries the same state for the same reason.

       Built ONLY when the device gave us the mode, which is what makes the
       refusal in `prepare` a refusal rather than a fallback: with `lineok`
       clear there is no line pipeline to bind, so a frame with lines cannot
       be drawn a different way by accident. */
    if (d->lineok) {
        ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        dy.dynamicStateCount = 3;
        lr.lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT;
        lr.stippledLineEnable = VK_FALSE;
        lr.pNext = rs.pNext;
        rs.pNext = &lr;
        if (vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL,
                                      &s_pipeLine) != VK_SUCCESS) {
            rs.pNext = lr.pNext;
            goto done;
        }
        /* the selection rects' pipeline: the SVS/SFS program (tagpu_mark.c
           says why it is its own), the depth TEST on with LESS -- the unit
           pass's own compare, against the keys that pass wrote -- and never
           written, so a rect cannot hide anything drawn after it.

           NOT A FAILURE OF THE PASS IF IT WILL NOT BUILD: the rects are
           dropped and the rest of the markers still draw, the same rule as a
           device without the lines (`prepare`). `s_pipeLineZ` stays NULL and
           `record`'s `s_selDraw` reads that. */
        svs = mk_module(d, tagpu_spv_tagpu_mark_SVS, sizeof tagpu_spv_tagpu_mark_SVS / 4);
        sfs = mk_module(d, tagpu_spv_tagpu_mark_SFS, sizeof tagpu_spv_tagpu_mark_SFS / 4);
        if (svs && sfs) {
            st[0].module = svs; st[1].module = sfs;
            ds.depthTestEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_LESS;
            if (vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL,
                                          &s_pipeLineZ) != VK_SUCCESS)
                s_pipeLineZ = VK_NULL_HANDLE;
        }
        if (!s_pipeLineZ)
            plog(d, "mark: the selection rects' pipeline would not build - "
                    "the rects are not drawn, every other marker is");
        ds.depthTestEnable = VK_FALSE;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        st[0].module = vs; st[1].module = fs;
        rs.pNext = lr.pNext;
    }
    s_pipeRp = rp;
    ok = 1;
done:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    if (svs) vkDestroyShaderModule(d->dev, svs, NULL);
    if (sfs) vkDestroyShaderModule(d->dev, sfs, NULL);
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

/* One of the two sets: `unit0` is what binding 40 -- the shader's `uLayer` --
   samples in it. Everything else in the two sets is identical, which is the
   point: the only thing that differs between a text draw and a textureless one
   is the image on unit 0. */
static void write_one(const TAGPU_VKPASS* d, uint32_t slot, VkDescriptorSet set,
                      const IMG* unit0)
{
    VkDescriptorBufferInfo bi[2];
    VkDescriptorImageInfo ii[NSAMP];
    VkWriteDescriptorSet w[2 + NSAMP];
    /* the order the shader names them: uLayer, uPal, uFogGrid, uFogLUT. A
       binding with no image this frame falls back to the STAND-IN's view, which
       always exists once the pass is ready -- a set cannot be bound with a
       hole, and the uniform that would read it is 0 on such a frame. */
    const IMG* src[NSAMP];
    uint32_t i;
    int n = 0;
    src[0] = unit0;                  src[1] = &s_img[slot][IMG_PAL];
    src[2] = &s_img[slot][IMG_FOG];  src[3] = &s_img[slot][IMG_LUT];
    memset(bi, 0, sizeof bi); memset(ii, 0, sizeof ii); memset(w, 0, sizeof w);
    bi[0].buffer = s_ubo[slot]; bi[0].range = VS_SZ;
    bi[1].buffer = s_ubo[slot]; bi[1].range = FS_SZ;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[n].dstSet = set; w[n].dstBinding = 0; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[n].pBufferInfo = &bi[0]; n++;
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[n].dstSet = set; w[n].dstBinding = 32; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[n].pBufferInfo = &bi[1]; n++;
    for (i = 0; i < NSAMP; i++) {
        ii[i].sampler = s_samp;
        ii[i].imageView = src[i]->view ? src[i]->view : s_img[slot][IMG_NONE].view;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = set; w[n].dstBinding = SAMP_BIND[i];
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &ii[i]; n++;
    }
    vkUpdateDescriptorSets(d->dev, (uint32_t)n, w, 0, NULL);
}

static void write_set(const TAGPU_VKPASS* d, uint32_t slot)
{
    write_one(d, slot, s_setN[slot], &s_img[slot][IMG_NONE]);
    write_one(d, slot, s_setT[slot], &s_img[slot][IMG_TEXT]);
}

int tagpu_vk_mark_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    VkPhysicalDeviceProperties props;
    VkDeviceSize vsz, usz, ssz, off;
    int i, needLines = 0, needSel = 0;

    s_drawThis = 0; s_abFrame = 0;
    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    s_slot = slot;

    if (s_state == ST_UNBUILT) {
        if (!resolve(d)) {
            /* WHICH ENTRY POINT is the question a bare refusal cannot answer,
               and this pass asks for one the others do not -- vkCmdSetLineWidth,
               for the order lines. Named so the next reader is not left
               guessing at a pass that produced no log line at all. */
            plog(d, "mark: an entry point would not resolve (this pass asks for "
                    "vkCmdSetLineWidth, which its siblings do not) - the marker "
                    "pass stays down");
            s_state = ST_REFUSED; s_downOwed = 1; return 0;
        }
        vkGetPhysicalDeviceProperties(d->pd, &props);
        s_ualign = props.limits.minUniformBufferOffsetAlignment;
        if (s_ualign == 0) s_ualign = 1;
        if (!build_descriptors(d)) {
            plog(d, "mark: no descriptors for the marker pass");
            s_state = ST_REFUSED; s_downOwed = 1; return 0;
        }
        s_state = ST_READY;
        /* THE `up` LINE EVERY SIBLING PRINTS. Without it, a pass with no
           refusal, no draw and no voice at all says "never reached ready" and
           nothing else. */
        plog(d, "mark: up - %u frame slots, uniform offset alignment %u, "
                "bresenham lines %s", (unsigned)d->slots, (unsigned)s_ualign,
             d->lineok ? "yes" : "NO (a frame with order lines will refuse)");
    }

    if (!tagpu_mark_handover(&s_h, d->frame)) {
        if (!s_saidHand) {
            s_saidHand = 1;
            plog(d, "mark: no hand-over for frame %u - the mark pass published "
                    "nothing, or published it for another frame", d->frame);
        }
        return 0;
    }
    s_saidHand = 0;
    s_abFrame = s_h.ab;
    /* WHAT ACTUALLY CROSSED, once. A pass that draws the right vertices into
       the right place and still shows black is failing on its INPUTS, and one
       line naming all of them settles in a single run what narrowing one
       suspect at a time costs a relaunch each. */
    if (!s_saidIn) {
        s_saidIn = 1;
        plog(d, "mark: in: %d draw(s) %d verts | pal=%s fog=%s %dx%d lut=%s "
                "text=%s %dx%d | key=%d game=%.0fx%.0f zoom=%.2f ss=%.1f",
             s_h.ndraw, s_h.nvert,
             s_h.pal ? "yes" : "NULL",
             s_h.fogGrid ? "yes" : "NULL", s_h.fogGridCols, s_h.fogGridRows,
             s_h.fogLut ? "yes" : "NULL",
             s_h.text ? "yes" : "NULL", s_h.textW, s_h.textH,
             s_h.key, s_h.gw, s_h.gh, s_h.zoom, s_h.ss);
    }
    /* EVERY BAIL-OUT FROM HERE DOWN SAYS WHY. A silent one leaves a frame with
       no markers on it and the log holding not one word about the cause --
       which is the exact shape of failure this project keeps paying for.
       Each latches SEPARATELY so a refusal is said once and not at the frame
       rate -- and so that one refusal cannot silence a different one. */
    /* NO PALETTE, NO FRAME. Binding 41 falls back to the STAND-IN's view rather
       than leaving a hole in the set, and the fragment shader's LAST line is
       `texelFetch(uPal, ivec2(pi, 0), 0)` on EVERY path -- the flat one and the
       text one alike. So a frame with no palette would draw
       every marker out of a 1x1 R8 image instead of refusing: the whole layer
       in garbage colours, which is a different picture and not an absent one.
       The fallback's comment ("the uniform that would read it is 0 on such a
       frame") is true of uLayer and false of uPal.
       `tagpu_pal_live()` returns NULL until it has resolved one
       (`s_have ? s_pal : NULL`), and `tagpu_vk_fx.c` refuses on it for the same
       reason. */
    if (!s_h.pal) {
        if (!s_saidPal) { s_saidPal = 1;
            plog(d, "mark: the hand-over carries no palette - every marker "
                    "would be drawn out of the fallback image, so nothing is "
                    "drawn while that is true"); }
        return 0;
    }
    if (s_h.ndraw <= 0 || s_h.nvert <= 0 || !s_h.verts || !s_h.draws) {
        if (!s_saidEmpty) { s_saidEmpty = 1;
            plog(d, "mark: the hand-over is empty - ndraw=%d nvert=%d verts=%s "
                    "draws=%s", s_h.ndraw, s_h.nvert,
                    s_h.verts ? "yes" : "NULL", s_h.draws ? "yes" : "NULL"); }
        return 0;
    }
    if (s_h.ndraw > TAGPU_MK_MAXDRAW) {
        if (!s_saidMany) { s_saidMany = 1;
            plog(d, "mark: %d draws is past the %d this pass carries",
                 s_h.ndraw, TAGPU_MK_MAXDRAW); }
        return 0;
    }

    for (i = 0; i < s_h.ndraw; i++) {
        const TAGPU_MKDRAW* g = &s_h.draws[i];
        if (g->first < 0 || g->count <= 0 || g->first + g->count > s_h.nvert) {
            if (!s_saidBound) { s_saidBound = 1;
                plog(d, "mark: draw %d is out of the vertex block - first=%d "
                        "count=%d nvert=%d", i, g->first, g->count, s_h.nvert); }
            return 0;
        }
        /* A DEPTH DRAW IS A SELECTION-RECT LINE LIST AND NOTHING ELSE: it is
           the only shape the depth pipeline exists for. It does not count as
           a line frame for the refusals below. */
        if (g->depth) {
            if (!g->lines) {
                if (!s_saidBound) { s_saidBound = 1;
                    plog(d, "mark: draw %d is depth-tested but not a line list", i); }
                return 0;
            }
            needSel = 1;
        } else if (g->lines) needLines = 1;
        /* A DRAW THAT WANTED FOG AND HAS NO GRID IS REFUSED, not drawn clear:
           an unfogged marker over fogged ground is a different picture. */
        if (g->fog && !s_h.fogGrid) {
            if (!s_saidFog) {
                s_saidFog = 1;
                plog(d, "mark: a fogged marker draw arrived with no fog grid - "
                        "nothing drawn while that is true");
            }
            return 0;
        }
        /* AND THE LUT, WHICH IS THE SAME RULE. The fog shade re-indexes through
           `uFogLUT` inside the grey band (`TAGPU_GLSL_FOG_SHADE`), so a fogged
           draw with no LUT samples binding 43's fallback and every marker in
           that band takes a wrong palette index. `tagpu_native_foglut()`
           returns NULL until a fog frame has built the table, and
           `tagpu_vk_terr.c` refuses on exactly this. */
        if (g->fog && !s_h.fogLut) {
            if (!s_saidLut) {
                s_saidLut = 1;
                plog(d, "mark: a fogged marker draw arrived with no fog LUT - "
                        "nothing drawn while that is true");
            }
            return 0;
        }
        /* AND A DRAW THAT NAMES A TEXTURE THE HAND-OVER DID NOT BRING.
           Binding 40 falls back to whatever image is there rather than leaving
           a hole, so without this a text draw with no atlas would sample the
           1x1 0xFF stand-in and come out a solid quad -- which is precisely
           the shape of the bug the two sets above exist to fix, reachable by a
           second road. Refuse the frame instead. */
        if (g->tex == TAGPU_MK_TEX_TEXT &&
            !(s_h.text && s_h.textW > 0 && s_h.textH > 0)) {
            if (!s_saidTexA) { s_saidTexA = 1;
                plog(d, "mark: a text draw arrived with no coverage atlas - "
                        "nothing drawn while that is true"); }
            return 0;
        }
    }
    /* THE TWO BOUNDS ON A LINE FRAME, and they are different features.

       `lineok` is VK_EXT_line_rasterization with `bresenhamLines` -- the
       diamond-exit rule these lines are specified by. Without it the line
       pipeline is not built at all (build_pipelines), so this is a refusal and
       not a fallback.

       The WIDTH is the other one. A line is one SCREEN pixel wide, `ss` pixels
       of the target, and a Vulkan `lineWidth` other than 1.0 needs the
       `wideLines` device feature
       enabled at device creation -- which is the seam's business, not a
       pass's. THE SEAM ASKS FOR IT, because the world is drawn into a target
       `ss` times the game resolution and a 1.0 line there is `ss` times too
       thin. The width comes from `tagpu_vk_world_scale()` -- the supersample
       factor of THIS frame's target, which is 1 when the target refused and the
       world is going into the swapchain image at 1:1 -- and a width the device
       will not rasterise is still refused rather than clamped. This is
       `tagpu_vk_fx.c`'s rule, item 2 of its header, and it applies here word
       for word. */
    if (needLines && !d->lineok) {
        if (!s_saidLine) {
            s_saidLine = 1;
            plog(d, "mark: order lines need VK_EXT_line_rasterization with "
                    "bresenhamLines and the device has not got it - the "
                    "diamond-exit line is a different picture from Vulkan's "
                    "default, so nothing is drawn");
        }
        return 0;
    }
    /* THE LINE WIDTH. The world is drawn into a target `ss` times the game
       resolution, so `ss` is the width of one screen pixel and `record` below
       sets exactly that. Refused rather than clamped: a clamped width is a
       line thinner than the one screen pixel it is specified at. */
    /* THE WIDTH FOLLOWS THE TARGET, NOT THE HAND-OVER -- tagpu_vk_fx.c carries
       the argument. `s_h.ss` says what the gather was handed; this says what
       this frame's target is, and on the fallback path (no offscreen target,
       the world going into the swapchain image at 1:1) they differ. */
    s_lineW = (float)tagpu_vk_world_scale();
    /* THE SELECTION RECTS NEED THE SAME TWO FEATURES AND ARE NOT REFUSED WITH
       THE FRAME, as the order lines are: a selection is on screen far more
       often than an order line, and refusing the whole marker layer whenever a
       unit is selected would take the health bars with it.
       So on a device without Bresenham lines the rects alone are dropped, and
       said so once; the WIDTH they need is `record`'s to check, because it
       follows the target's real extent. */
    s_selOk = d->lineok;
    if (needSel && !s_selOk && !s_saidSel) {
        s_saidSel = 1;
        plog(d, "mark: selection rects need VK_EXT_line_rasterization "
                "bresenhamLines and this device has not got it - the rects "
                "are not drawn, every other marker is");
    }
    if (needLines && s_lineW != 1.0f &&
        (!d->wideok || s_lineW > d->maxLineWidth)) {
        if (!s_saidWide) {
            s_saidWide = 1;
            plog(d, "mark: the target is %.0fx supersampled, so the order lines "
                    "are that many px wide, and this device "
                    "offers %s - nothing drawn", s_lineW,
                 d->wideok ? "a narrower maximum" : "no wideLines at all");
        }
        return 0;
    }

    /* the images this frame needs, and the staging to fill them */
    ssz = 0;
    if (s_h.text && s_h.textW > 0 && s_h.textH > 0)
        ssz += (VkDeviceSize)s_h.textW * s_h.textH;
    ssz += 256 * 4;                                  /* the palette */
    if (s_h.fogGrid) ssz += (VkDeviceSize)s_h.fogGridCols * s_h.fogGridRows * 2;
    if (s_h.fogLut) ssz += 256;
    /* AND THE 1x1 STAND-IN'S OWN BYTE. Reserving for the images the hand-over
       carries and forgetting the one this pass makes for itself draws nothing:
       the four real uploads land at exactly `ssz`, the stand-in asks for one
       byte past it, `img_up`'s bound refuses, and the whole frame bails. 16
       rather than 1 so the next thing added here is not a second off-by-one. */
    ssz += 16;
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
    if (s_h.text && s_h.textW > 0 && s_h.textH > 0) {
        int fresh = !s_img[slot][IMG_TEXT].have || s_img[slot][IMG_TEXT].gen != s_h.textGen;
        if (!img_size(d, &s_img[slot][IMG_TEXT], s_h.textW, s_h.textH, VK_FORMAT_R8_UNORM)) {
            if (!s_saidImgT) { s_saidImgT = 1; plog(d, "mark: no image for the %dx%d text atlas", s_h.textW, s_h.textH); }
            return 0;
        }
        if (fresh) {
            if (!img_up(d, cb, slot, &s_img[slot][IMG_TEXT], s_h.text, s_h.textW, 1, off)) {
                if (!s_saidImgT) { s_saidImgT = 1; plog(d, "mark: the text atlas would not upload (%dx%d)", s_h.textW, s_h.textH); }
                return 0;
            }
            s_img[slot][IMG_TEXT].gen = s_h.textGen;
        }
        off += (VkDeviceSize)s_h.textW * s_h.textH;
    }
    if (s_h.pal) {
        if (!img_size(d, &s_img[slot][IMG_PAL], 256, 1, VK_FORMAT_R8G8B8A8_UNORM) ||
            !img_up(d, cb, slot, &s_img[slot][IMG_PAL], s_h.pal, 256 * 4, 4, off)) {
            if (!s_saidImgP) { s_saidImgP = 1; plog(d, "mark: the palette would not upload"); }
            return 0;
        }
        off += 256 * 4;
    }
    if (s_h.fogGrid && s_h.fogGridCols > 0 && s_h.fogGridRows > 0) {
        if (!img_size(d, &s_img[slot][IMG_FOG], s_h.fogGridCols, s_h.fogGridRows, VK_FORMAT_R8G8_UNORM) ||
            !img_up(d, cb, slot, &s_img[slot][IMG_FOG], (const unsigned char*)s_h.fogGrid,
                    s_h.fogGridCols * 2, 2, off)) {
            if (!s_saidImgF) { s_saidImgF = 1; plog(d, "mark: the %dx%d fog grid would not upload (R8G8)", s_h.fogGridCols, s_h.fogGridRows); }
            return 0;
        }
        off += (VkDeviceSize)s_h.fogGridCols * s_h.fogGridRows * 2;
    }
    if (s_h.fogLut) {
        if (!img_size(d, &s_img[slot][IMG_LUT], 256, 1, VK_FORMAT_R8_UNORM) ||
            !img_up(d, cb, slot, &s_img[slot][IMG_LUT], s_h.fogLut, 256, 1, off)) {
            if (!s_saidImgU) { s_saidImgU = 1; plog(d, "mark: the fog LUT would not upload"); }
            return 0;
        }
        off += 256;
    }
    /* THE FALLBACK EVERY UNUSED BINDING NAMES, AND IT HAS TO BE INITIALISED.
       Created and left there -- no clear, no transition -- its descriptor would
       claim SHADER_READ_ONLY_OPTIMAL while the image sat in UNDEFINED with
       undefined contents, and sampling that is undefined behaviour: the pass
       draws its vertices and the frame comes back black. The upload is what
       puts it in the layout the descriptor promises. */
    if (!s_img[slot][IMG_NONE].have) {
        static const unsigned char ONE = 0xFF;
        if (!img_size(d, &s_img[slot][IMG_NONE], 1, 1, VK_FORMAT_R8_UNORM) ||
            !img_up(d, cb, slot, &s_img[slot][IMG_NONE], &ONE, 1, 1, off)) {
            if (!s_saidImgS) { s_saidImgS = 1; plog(d, "mark: no 1x1 stand-in for the unused sampler bindings"); }
            return 0;
        }
        off += 1;
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

/* the scissor in the target's pixels, and NOT mirrored. This
   is `tagpu_vk_fx.c`'s `fx_scissor` line for line: the two passes clip to the
   same rect and deriving it twice differently is how they would drift. */
static void mk_scissor(const TAGPU_MKHAND* h, uint32_t w, uint32_t hh,
                       VkRect2D* out)
{
    float sx = h->gw > 0.0f ? (float)w / h->gw : 1.0f;
    float sy = h->gh > 0.0f ? (float)hh / h->gh : 1.0f;
    int x0 = (int)(h->vpL * sx + 0.5f);
    int ww = (int)(h->vw * sx + 0.5f);
    int ytop = (int)(h->vpT * sy + 0.5f);
    int hgt = (int)(h->vh * sy + 0.5f);
    int y0 = ytop;                            /* NOT mirrored */
    if (!h->scissorOn || ww <= 0 || hgt <= 0) {
        out->offset.x = 0; out->offset.y = 0;
        out->extent.width = w; out->extent.height = hh;
        return;
    }
    if (x0 < 0) { ww += x0; x0 = 0; }
    if (y0 < 0) { hgt += y0; y0 = 0; }
    if (x0 > (int)w) x0 = (int)w;
    if (y0 > (int)hh) y0 = (int)hh;
    if (ww < 0) ww = 0;
    if (hgt < 0) hgt = 0;
    if (x0 + ww > (int)w) ww = (int)w - x0;
    if (y0 + hgt > (int)hh) hgt = (int)hh - y0;
    out->offset.x = x0; out->offset.y = y0;
    out->extent.width = (uint32_t)ww; out->extent.height = (uint32_t)hgt;
}

void tagpu_vk_mark_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          VkRenderPass rp, uint32_t w, uint32_t h)
{
    VkDeviceSize zero = 0;
    VkViewport vp;
    VkRect2D sc;
    int i;
    if (s_state != ST_READY || !s_drawThis) {
        if (!s_saidNoDraw) { s_saidNoDraw = 1;
            plog(d, "mark: record with nothing prepared (state=%d drawThis=%d)",
                 s_state, s_drawThis); }
        return;
    }
    if (slot != s_slot) {
        if (!s_saidSlot) { s_saidSlot = 1;
            plog(d, "mark: record on slot %u but prepare ran for %u",
                 (unsigned)slot, (unsigned)s_slot); }
        return;
    }
    if (!s_pipeTri || s_pipeRp != rp) {
        if (s_pipeTri) { vkDestroyPipeline(d->dev, s_pipeTri, NULL); s_pipeTri = VK_NULL_HANDLE; }
        if (s_pipeLine) { vkDestroyPipeline(d->dev, s_pipeLine, NULL); s_pipeLine = VK_NULL_HANDLE; }
        if (s_pipeLineZ) { vkDestroyPipeline(d->dev, s_pipeLineZ, NULL); s_pipeLineZ = VK_NULL_HANDLE; }
        if (!build_pipelines(d, rp)) {
            plog(d, "mark: the marker pipelines would not build - nothing drawn");
            s_drawThis = 0;
            return;
        }
    }
    /* THE VIEWPORT AND THE SCISSOR, SET HERE BECAUSE THIS PASS'S PIPELINES
       DECLARE THEM DYNAMIC -- and dynamic state that is never set is undefined.
       Declared and not set, the draws go correctly into nowhere: the log says
       "drew 1 list entr(ies), 90 vertices" and the frame comes back black, on
       the lane's own window as well as in the capture.
       NO Y FLIP: this pass's vertices are the engine's screen-space y, which
       grows DOWNWARD, so clip -1 is the game frame's TOP row and a positive
       viewport height puts it on row 0 of the swapchain image -- which is where
       the game's top row is. A negative height turns it over a second time and
       presents the markers upside down; `tagpu_vk_fx.c` item 6 has the whole
       argument. minDepth 0.5 / maxDepth 1.0 maps clip z in [0, 1] onto
       [0.5, 1], the range every depth-testing world pass shares
       (tagpu_vk_feat.c item 1) -- the depth range is a separate question from
       the flip. */
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.minDepth = 0.5f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    mk_scissor(&s_h, w, h, &sc);
    vkCmdSetScissor(cb, 0, 1, &sc);

    /* THE SELECTION RECTS' PIXEL SCALE AND BAND. SFS asks which GAME pixel a
       sample lies in, so it needs target pixels per game pixel -- from THIS
       target's extent, not from the supersample factor, because on the frame
       the offscreen target refused the world goes into the swapchain image at
       whatever scale that is, and a wrong scale does not blur the rect, it
       moves every pixel of it.

       THE BAND IS ceil(3 * scale) + 2 WIDE, and the 3 is a bound, not a
       margin. Across the minor axis, a sample of a pixel the test keeps can
       sit 1.5 game px from the ideal line: half a pixel of Bresenham rounding,
       half of the pixel's own extent, and up to half again because a sample's
       column is up to half a pixel from the pixel's centre on a slope of up to
       1. The +2 is the column's own rounding in the target. 2 * scale + 2
       leaves samples uncovered; a brute-force of every edge within 24 px at
       every scale from 1 to 6 in 1/16 steps finds none uncovered at this
       width. It costs nothing to be wide: the fragment test decides the pixels.
       A device that will not draw that wide loses the rects, not the frame. */
    {
        float pxX = s_h.gw > 0.0f ? (float)w / s_h.gw : 1.0f;
        float pxY = s_h.gh > 0.0f ? (float)h / s_h.gh : 1.0f;
        s_selW = (float)ceil(3.0 * (pxX > pxY ? pxX : pxY)) + 2.0f;
        s_selDraw = s_selOk && s_pipeLineZ && d->wideok &&
                    s_selW <= d->maxLineWidth;
        for (i = 0; i < s_h.ndraw; i++) {
            unsigned char* p = s_uboMap[slot] + s_fsOff[i];
            put_f(p, FS_PX, pxX);
            put_f(p, FS_PX + 4, pxY);
        }
        if (!s_selDraw && s_selOk && !s_saidSelW) {
            for (i = 0; i < s_h.ndraw; i++) if (s_h.draws[i].depth) break;
            if (i < s_h.ndraw) {
                s_saidSelW = 1;
                plog(d, "mark: selection rects need a line %.0f px wide and "
                        "this device offers %s - the rects are not drawn",
                     s_selW, d->wideok ? "a narrower maximum" : "no wideLines");
            }
        }
    }

    vkCmdBindVertexBuffers(cb, 0, 1, &s_vb[slot], &zero);
    for (i = 0; i < s_h.ndraw; i++) {
        const TAGPU_MKDRAW* g = &s_h.draws[i];
        uint32_t dyno[2];
        VkDescriptorSet set;
        /* `prepare` refused every frame that has a line draw without the
           line pipeline, so this cannot be NULL -- and it is checked anyway,
           because a bind of VK_NULL_HANDLE is undefined rather than loud and
           the cost of the branch is nothing. */
        if (g->lines && !s_pipeLine) continue;
        if (g->depth && !s_selDraw) continue;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          g->depth ? s_pipeLineZ :
                          g->lines ? s_pipeLine : s_pipeTri);
        /* A line width of `ss`: the seam enables `wideLines` and the world is
           drawn into a target that many times the game resolution. `s_lineW`
           is the TARGET's scale, settled in `prepare`, not the hand-over's --
           the two differ on the frame the target refused. */
        if (g->lines) vkCmdSetLineWidth(cb, g->depth ? s_selW : s_lineW);
        dyno[0] = 0;
        dyno[1] = (uint32_t)s_fsOff[i];
        /* UNIT 0 IS PER DRAW: the text draws sample the coverage atlas and
           everything else samples the 1x1 stand-in. A TEX_NONE draw does not
           read it at all (its vertices carry u < 0, the flat path), so the
           stand-in is there to fill the binding rather than to be sampled. */
        set = (g->tex == TAGPU_MK_TEX_TEXT) ? s_setT[slot] : s_setN[slot];
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo,
                                0, 1, &set, 2, dyno);
        vkCmdDraw(cb, (uint32_t)g->count, 1, (uint32_t)g->first, 0);
    }
    if (!s_saidDrew) {
        s_saidDrew = 1;
        plog(d, "mark: drew %d list entr(ies), %d vertices; first entry "
                "first=%d count=%d lines=%d text=%d fog=%d tex=%d",
             s_h.ndraw, s_h.nvert, s_h.draws[0].first, s_h.draws[0].count,
             s_h.draws[0].lines, s_h.draws[0].text, s_h.draws[0].fog,
             s_h.draws[0].tex);
    }
}

int tagpu_vk_mark_ab_frame(void) { return s_abFrame; }

void tagpu_vk_mark_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    int k;
    if (s_state == ST_UNBUILT && !s_pool) return;
    for (i = 0; i < TAGPU_VK_SLOTS; i++)
        for (k = 0; k < IMG_N; k++) kill_img(d, &s_img[i][k]);
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        if (s_ubo[i]) { vkDestroyBuffer(d->dev, s_ubo[i], NULL); vkFreeMemory(d->dev, s_uboMem[i], NULL); }
        if (s_vb[i])  { vkDestroyBuffer(d->dev, s_vb[i], NULL);  vkFreeMemory(d->dev, s_vbMem[i], NULL); }
        if (s_stg[i]) { vkDestroyBuffer(d->dev, s_stg[i], NULL); vkFreeMemory(d->dev, s_stgMem[i], NULL); }
        s_ubo[i] = s_vb[i] = s_stg[i] = VK_NULL_HANDLE;
        s_uboMem[i] = s_vbMem[i] = s_stgMem[i] = VK_NULL_HANDLE;
        s_uboMap[i] = s_vbMap[i] = s_stgMap[i] = NULL;
        s_uboCap[i] = s_vbCap[i] = s_stgCap[i] = 0;
        s_setN[i] = s_setT[i] = VK_NULL_HANDLE;
    }
    if (s_pipeTri) { vkDestroyPipeline(d->dev, s_pipeTri, NULL); s_pipeTri = VK_NULL_HANDLE; }
    if (s_pipeLine) { vkDestroyPipeline(d->dev, s_pipeLine, NULL); s_pipeLine = VK_NULL_HANDLE; }
    if (s_pipeLineZ) { vkDestroyPipeline(d->dev, s_pipeLineZ, NULL); s_pipeLineZ = VK_NULL_HANDLE; }
    s_pipeRp = VK_NULL_HANDLE;
    if (s_samp) { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_pool) { vkDestroyDescriptorPool(d->dev, s_pool, NULL); s_pool = VK_NULL_HANDLE; }
    if (s_plo) { vkDestroyPipelineLayout(d->dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dsl) { vkDestroyDescriptorSetLayout(d->dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    s_state = ST_UNBUILT;
    s_drawThis = 0; s_abFrame = 0;
    s_downOwed = 0; s_downPaying = 0;
    s_saidLine = s_saidWide = s_saidFog = s_saidLut = s_saidRoom = s_saidPal = 0;
    s_saidSel = s_saidSelW = 0;
    s_saidEmpty = s_saidMany = s_saidBound = s_saidTexA = 0;
    s_saidImgT = s_saidImgP = s_saidImgF = s_saidImgU = s_saidImgS = 0;
    s_saidNoDraw = s_saidSlot = 0;
    s_saidHand = s_saidDrew = s_saidIn = 0;
}

int  tagpu_vk_mark_down_owed(void) { return s_downOwed && !s_downPaying; }
void tagpu_vk_mark_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_mark_down(d);
}
