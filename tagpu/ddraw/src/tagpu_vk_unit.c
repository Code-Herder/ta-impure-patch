/* tagpu_vk_unit.c -- the posed unit bodies and their cast-shadow depth twins,
   drawn by Vulkan. Contract: tagpu_vk_unit.h. Phase G / G19e, the SIXTH world
   pass and the last of the gate.

   IT IS NOT A SECOND IMPLEMENTATION OF THE PASS. Everything arrives through
   `tagpu_posedraw_handover` (tagpu_posedraw.h): the vertices are the two
   streams the GL bake uploaded, reached through tagpu_posebake.h's mirrors;
   the draw ranges are the `first`/`count` the GL glDrawArrays used; the pose
   block is the bytes `upload_pose` wrote, through the same conversion; the
   uniforms are the numbers the GL draws passed; the texels are the bytes each
   GL texture was uploaded from; and the shaders are the same GLSL through
   tools/spirv-gen.py. What a 0-px comparison then compares is two rasterisers.

   ---- WHAT THIS LANDING HAD TO ANSWER, AND WHERE EACH ANSWER IS ----

   1. A PASS THAT BOTH FEEDS AND SAMPLES ANOTHER PASS'S TARGET. Every pass
      before this one either drew into the frame or drew a map for others; this
      one puts casters INTO the cast-shadow map and then samples the finished
      map in its own fragment shader. So it has four hooks rather than two and
      the seam calls three of them at three different points in the frame --
      tagpu_vk_unit.h lists them and the seam repeats the ordering at the call
      sites. The GL twin has exactly the same shape for exactly the same
      reason: `tagpu_posedraw_depth_unit` runs at tagpu_native.c:3936 and
      `tagpu_posedraw_unit` at :4238.

      AND THE CASTER DRAW BINDS A SET OF ITS OWN. A descriptor set may only be
      written during its own slot's `prepare`, which is after the map is drawn;
      at `cast` time this slot's set still names the map's image from the
      PREVIOUS frame -- the same image, because the views are per slot -- and
      that image is, at that moment, the render target being written. So `cast`
      binds a second, smaller set holding the two vertex-stage buffers and no
      image at all, and the question does not arise. Two layouts, two sets per
      slot, one pipeline each.

   2. PER-UNIT UNIFORMS, WHICH IS WHAT A UNIT PASS IS. The GL twin re-uploads
      one 14 336-byte pose block and a dozen loose uniforms per unit and draws
      between the uploads; a Vulkan command buffer cannot, because every draw
      it records is submitted together. So each unit gets its own window in
      three DYNAMIC uniform buffers and the draw binds the set with three
      offsets. That is the whole of the per-unit cost and it is the pass's
      memory story:

        the POSE buffer   TAGPU_PD_BLOCK (14 336) a unit, per frame slot. The
                          block's SIZE is the 256-piece ceiling, not the model
                          -- stock's worst is 36 pieces -- but a descriptor
                          must cover the block the shader declares, so the
                          window cannot be shortened to the model. 300 units is
                          4.3 MB a slot.
        the SMALL buffer  two vertex-stage blocks (the body's and the caster's)
                          and one fragment-stage block a unit, each at a
                          device-accepted offset: 768 bytes a unit on the
                          reference device.

      Both are grown to the frame's own unit count rather than to
      TAGPU_PD_MAXHAND, and both are given back the moment a frame hands
      nothing over -- §2.28's rule, and at this size it is the rule that makes
      the pass affordable in a 32-bit address space.

   3. PER-TYPE VERTEX BUFFERS, AND A SERIAL RATHER THAN A POINTER. The GL twin
      draws every unit out of its TYPE's two static buffers, so this keeps one
      Vulkan buffer per baked stream and uploads it once. The table is keyed on
      tagpu_posebake.h's SERIAL, which is monotonic and never reused, because
      the bake's cache slots ARE reused -- key on the slot or on the entry
      pointer and a re-baked slot hands the new model the old model's vertices.
      A replaced or evicted buffer goes on the slot-bitmask retire the terrain
      pass established (§2.30): its last bit clears under its own slot's fence,
      so `pending == 0` means "no submitted command buffer names it and no
      future one will" by construction rather than by a timer.

   4. THE Y FLIP IS PIPELINE STATE FOR THE BODY AND ABSENT FOR THE CASTER, and
      that is the shadow pass's rule applied rather than restated: a target that
      is PRESENTED flips, a target that is SAMPLED does not. The body draws into
      the frame, so it takes the negative viewport height every other frame pass
      takes; the caster draws into the depth map, so its viewport is the shadow
      pass's own and this file sets nothing.

   ---- WHAT IT DOES NOT DO ----

   THE BODY RANGE ONLY. The nanoframe WIRE (`uRange` 2, GL_LINES), the Classic
   SILHOUETTE and the structure SLANT are the same program and the same bake
   and are not ported here; the hand-over counts any of them that drew inside
   the published window and the pass stands down. On a Classic++ soft-shadow
   frame -- which is the configuration the shadow work is measured in --
   tagpu_native.c draws none of them.

   CLASSIC++'s RESTORED ATLAS IS NOT MIRRORED, so a frame whose twin reported
   `uRestored` 1 draws nothing and says so once, exactly as the feature and
   terrain passes do. The replacement meshes (tagpu_hires_draw.c) and the
   native 3DO stream's own unit vertices are not this pass's either; they draw
   outside the A/B's window and are counted where they matter, which is the
   shadow map's census.

   IT KNOWS NOTHING ABOUT A WINDOW. Everything arrives in TAGPU_VKPASS.
   A PASS READS NO ENGINE STATE: every value comes from the GL lane's
   hand-over, so this file is not on thread-split.allow and must never be. */

#include "tagpu_vk_pass.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_unit.h"
#include "tagpu_vk_shadow.h"
#include "tagpu_vk_scaffold.h"
#include "tagpu_posedraw.h"
#include "tagpu_posebake.h"
#include "spirv/tagpu_posedraw.spv.h"
#include "spirv/tagpu_native.spv.h"

/* the two std140 blocks, at the sizes the generated SPIR-V headers print for them */
#define VGL_SZ  176                        /* tagpu_posedraw::VS  _Globals    */
#define FGL_SZ  272                        /* tagpu_native::FS    _Globals    */
#define POSE_SZ TAGPU_PD_BLOCK             /* the Pose block, 14336           */

/* the two vertex bindings, which are the GL VAO's two buffers */
#define GEOM_STRIDE (TAGPU_PB_GEOMST * 4)  /* 32 */
#define MAT_STRIDE  (TAGPU_PB_MATST * 4)   /* 20 */

/* the fog grid is RG8 and the widest the wide-fog builder produces is well
   inside this; a bound here is what keeps a handed-over number from sizing an
   allocation (the scaffold's rule) */
#define FOG_MAXDIM 1024

/* per-type vertex buffers cached at once. tagpu_posebake.c holds 128 geometry
   entries and 256 material streams, so 512 can never be the binding limit on a
   healthy frame -- it is the bound on a leak, and eviction is LRU. */
#define VB_MAX  512
/* buffers waiting for every slot to turn over once before they are destroyed */
#define RET_MAX 64

/* ---- the entry points ----------------------------------------------------
   Resolved from the seam's `gdpa`/`gipa`, never linked, and this pass's own:
   the presentation table and a pass's barely overlap (tagpu_vk_pass.h). */
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
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

static int s_state;
static int s_downOwed;                     /* a teardown the seam still owes us */
static int s_downPaying;                   /* ...and the seam is paying it NOW  */
static int s_drawThis;                     /* `prepare` left a draw for `record` */
static int s_abFrame;
static int s_saidRestored, s_saidNoMirror, s_saidShadow, s_saidOther;
static int s_saidFog, s_saidScaf, s_saidVbFull;

static VkDescriptorSetLayout s_dslMain, s_dslCast;
static VkPipelineLayout      s_ploMain, s_ploCast;
static VkPipeline            s_pipeBody, s_pipeCast;
static VkRenderPass          s_castRp;     /* what s_pipeCast was built against */
static VkDescriptorPool      s_dpool;
static VkSampler             s_samp, s_sampCmp;
static int                   s_cmpLinear;  /* the compare sampler is the twin's */
static VkDeviceSize          s_ualign;

/* ---- THE SHARED UNIT ATLAS. One image for every slot, as the feature pass's:
   2048 x 2048 R8 is 4 MB, and per-slot it would be 16 MB of device-local in a
   32-bit address space whose largest free block is the number this phase spends
   its budget measuring. ---- */
static VkImage        s_atImg;
static VkDeviceMemory s_atMem;
static VkImageView    s_atView;
static int            s_atDim;
static unsigned       s_atSerial;
static int            s_atHave;

/* the 1x1 stand-ins a descriptor names when there is nothing real for it: the
   scaffold on a frame with no overlay, and the depth map on a frame with none.
   A descriptor must be VALID whether or not the shader samples it. */
static VkImage        s_dumImg, s_dumDepth;
static VkDeviceMemory s_dumMem, s_dumDepthMem;
static VkImageView    s_dumView, s_dumDepthView;
static int            s_dumReady;

/* ---- the per-type vertex buffers ---------------------------------------- */
typedef struct {
    unsigned       serial;                 /* 0 = free; never reused           */
    VkBuffer       buf;
    VkDeviceMemory mem;
    VkDeviceSize   size;
    unsigned       lastFrame;
} VBENT;
static VBENT s_vb[VB_MAX];

typedef struct {
    VkBuffer       buf;
    VkDeviceMemory mem;
    unsigned       pending;                /* bit per slot still to turn over  */
} VBRET;
static VBRET s_ret[RET_MAX];

/* ---- one of these per frame slot, and every field of it is ours for the
   duration of the hooks we are handed that slot on. ---- */
typedef struct {
    VkBuffer        ubuf;                  /* the three small blocks a unit    */
    VkDeviceMemory  umem;
    unsigned char*  umap;
    VkDeviceSize    ucap;

    VkBuffer        pbuf;                  /* the pose blocks                  */
    VkDeviceMemory  pmem;
    unsigned char*  pmap;
    VkDeviceSize    pcap;

    VkBuffer        vstage;                /* this frame's vertex uploads      */
    VkDeviceMemory  vsmem;
    unsigned char*  vsmap;
    VkDeviceSize    vscap;

    VkImage         pal, lut, fogGrid, fogLut;
    VkDeviceMemory  palMem, lutMem, fogGridMem, fogLutMem;
    VkImageView     palView, lutView, fogGridView, fogLutView;
    VkBuffer        smallStage;
    VkDeviceMemory  smallMem;
    unsigned char*  smallMap;
    int             fogW, fogH;

    VkDescriptorSet dsMain, dsCast;
    int             built;
} SLOT;
static SLOT s_slot[TAGPU_VK_SLOTS];

/* ONE STAGING BUFFER CARRIES THE FOUR SMALL UPLOADS: the palette (256 x 1
   RGBA8), the shade LUT (256 x 32 R8), the fog shade LUT (256 x 1 R8), then the
   fog grid (cols x rows RG8, a different size whenever the view walks far
   enough for the grid to be re-laid). One allocation, four copy regions at four
   offsets -- and the buffer is rebuilt with the fog image, by the same call, so
   the two can never disagree about the size. Every offset is a multiple of 4
   and of its image's texel block size, which is what vkCmdCopyBufferToImage
   requires of a bufferOffset. */
#define SMALL_PALOFF 0                           /* 256 x 1  RGBA8            */
#define SMALL_SHDOFF (256 * 4)                   /* 256 x 32 R8               */
#define SMALL_FLTOFF (256 * 4 + 256 * 32)        /* 256 x 1  R8               */
#define SMALL_FIXED  (256 * 4 + 256 * 32 + 256)
#define SMALL_FOGOFF SMALL_FIXED                 /* cols x rows RG8           */

/* this frame's draw list, filled by `upload` and read by `cast` and `record` */
typedef struct {
    VkBuffer geom, mat;
    uint32_t first, count;
    uint32_t unit;                         /* its window in the slot buffers   */
    int      casts;
} DRAW;
static DRAW*    s_draw;
static unsigned s_drawCap, s_ndraw, s_ncast;
static int      s_scissorOn, s_vpL, s_vpT, s_vw, s_vh;
static int      s_shadowOn;            /* the twin drew these against a map  */
/* the strides and offsets `upload` settled and the three draw hooks bind with.
   They are the device's alignment applied to two block sizes, so they cannot
   change inside a frame -- but they are recomputed every `upload` rather than
   once at build, because `build` is where the alignment is read and a pass that
   cached them would have two places to keep in step. */
static VkDeviceSize s_uStride, s_vglOff2, s_fglOff, s_pStride;

static void plog(const TAGPU_VKPASS* d, const char* fmt, ...)
{
    char b[320];
    va_list ap;
    if (!d->log) return;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    d->log(b);
}

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a)
{
    return a ? ((v + a - 1) / a) * a : v;
}

static int mem_type(const TAGPU_VKPASS* d, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(d->pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static int mk_buffer(const TAGPU_VKPASS* d, VkDeviceSize size, VkBufferUsageFlags use,
                     VkMemoryPropertyFlags props, VkBuffer* buf, VkDeviceMemory* mem,
                     unsigned char** map)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int mt;
    void* p = NULL;
    bi.size = size ? size : 1;
    bi.usage = use;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bi, NULL, buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(d->dev, *buf, &mr);
    mt = mem_type(d, mr.memoryTypeBits, props);
    if (mt < 0) { vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0; }
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(d->dev, &ai, NULL, mem) != VK_SUCCESS) {
        vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0;
    }
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) {
        vkFreeMemory(d->dev, *mem, NULL); vkDestroyBuffer(d->dev, *buf, NULL);
        *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; return 0;
    }
    if (map) {
        if (vkMapMemory(d->dev, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            vkFreeMemory(d->dev, *mem, NULL); vkDestroyBuffer(d->dev, *buf, NULL);
            *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; return 0;
        }
        *map = (unsigned char*)p;
    }
    return 1;
}

static void kill_buffer(const TAGPU_VKPASS* d, VkBuffer* buf, VkDeviceMemory* mem,
                        unsigned char** map)
{
    if (map && *map) { vkUnmapMemory(d->dev, *mem); *map = NULL; }
    if (*buf) { vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; }
    if (*mem) { vkFreeMemory(d->dev, *mem, NULL); *mem = VK_NULL_HANDLE; }
}

static int mk_image(const TAGPU_VKPASS* d, int w, int h, VkFormat fmt,
                    VkImageUsageFlags use, VkImageAspectFlags aspect,
                    VkImage* img, VkDeviceMemory* mem, VkImageView* view)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    int mt;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = (uint32_t)w; ii.extent.height = (uint32_t)h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = use;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ii, NULL, img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, *img, &mr);
    mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { vkDestroyImage(d->dev, *img, NULL); *img = VK_NULL_HANDLE; return 0; }
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(d->dev, &ai, NULL, mem) != VK_SUCCESS) {
        vkDestroyImage(d->dev, *img, NULL); *img = VK_NULL_HANDLE; return 0;
    }
    if (vkBindImageMemory(d->dev, *img, *mem, 0) != VK_SUCCESS) goto bad;
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &vi, NULL, view) != VK_SUCCESS) goto bad;
    return 1;
bad:
    vkFreeMemory(d->dev, *mem, NULL); vkDestroyImage(d->dev, *img, NULL);
    *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
    return 0;
}

static void kill_image(const TAGPU_VKPASS* d, VkImage* img, VkDeviceMemory* mem,
                       VkImageView* view)
{
    if (*view) { vkDestroyImageView(d->dev, *view, NULL); *view = VK_NULL_HANDLE; }
    if (*img)  { vkDestroyImage(d->dev, *img, NULL);  *img = VK_NULL_HANDLE; }
    if (*mem)  { vkFreeMemory(d->dev, *mem, NULL);    *mem = VK_NULL_HANDLE; }
}

static VkShaderModule mk_module(const TAGPU_VKPASS* d, const uint32_t* w, size_t words)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    ci.codeSize = words * 4;
    ci.pCode = w;
    if (vkCreateShaderModule(d->dev, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

static int resolve(const TAGPU_VKPASS* d)
{
#define GETI(n) n = (PFN_##n)d->gipa(d->inst, #n); if (!n) return 0;
#define GETD(n) n = (PFN_##n)d->gdpa(d->dev,  #n); if (!n) return 0;
    IFNS(GETI)
    DFNS(GETD)
#undef GETI
#undef GETD
    return 1;
}

/* ---- barriers ----------------------------------------------------------- */
static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                        VkImageLayout from, VkImageLayout to,
                        VkPipelineStageFlags srcStage, VkAccessFlags srcAcc,
                        VkPipelineStageFlags dstStage, VkAccessFlags dstAcc)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAcc; b.dstAccessMask = dstAcc;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void copy_rect(VkCommandBuffer cb, VkBuffer src, VkDeviceSize srcOff,
                      VkImage dst, int w, int h)
{
    VkBufferImageCopy rg;
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = srcOff;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = (uint32_t)w;
    rg.imageExtent.height = (uint32_t)h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, src, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
}

/* ---- the per-type buffer table ------------------------------------------ */
/* THE RETIRE'S ACCOUNTING, FIRST AND UNCONDITIONALLY in `upload` -- the
   terrain pass's rule and the reason it is that rule: every path below can
   return early, and the path that returns early is exactly the path that must
   still clear this slot's bit, or the retire stalls for ever. */
static void ret_slot_done(const TAGPU_VKPASS* d, uint32_t slot)
{
    int i;
    for (i = 0; i < RET_MAX; i++) {
        if (!s_ret[i].buf) continue;
        s_ret[i].pending &= ~(1u << slot);
        if (s_ret[i].pending == 0) {
            kill_buffer(d, &s_ret[i].buf, &s_ret[i].mem, NULL);
            s_ret[i].pending = 0;
        }
    }
}

/* Hand a buffer to the retire, or -- if the list is full -- say so. A full
   list is not a reason to destroy it here: this is the middle of a frame and
   other slots' submitted command buffers may still name it. */
static int ret_push(const TAGPU_VKPASS* d, VkBuffer buf, VkDeviceMemory mem)
{
    int i;
    for (i = 0; i < RET_MAX; i++)
        if (!s_ret[i].buf) {
            s_ret[i].buf = buf; s_ret[i].mem = mem;
            /* EVERY SLOT, not only the live ones: `d->slots` can only shrink at
               a swapchain rebuild, which goes through vkDeviceWaitIdle anyway,
               and a bit for a slot that never comes round again would hold the
               buffer for ever. So the mask is exactly the slots the seam
               drives. */
            s_ret[i].pending = (d->slots >= 32) ? 0xFFFFFFFFu
                                                : ((1u << d->slots) - 1u);
            return 1;
        }
    return 0;
}

static VBENT* vb_find(unsigned serial)
{
    int i;
    if (!serial) return NULL;
    for (i = 0; i < VB_MAX; i++)
        if (s_vb[i].serial == serial) return &s_vb[i];
    return NULL;
}

static VBENT* vb_slot(const TAGPU_VKPASS* d, unsigned frame)
{
    int i, worst = -1;
    for (i = 0; i < VB_MAX; i++)
        if (!s_vb[i].serial) return &s_vb[i];
    /* the least recently drawn, and never one drawn THIS frame: the frame in
       hand has already recorded draws naming it */
    for (i = 0; i < VB_MAX; i++) {
        if (s_vb[i].lastFrame == frame) continue;
        if (worst < 0 || s_vb[i].lastFrame < s_vb[worst].lastFrame) worst = i;
    }
    if (worst < 0) return NULL;
    if (!ret_push(d, s_vb[worst].buf, s_vb[worst].mem)) return NULL;
    memset(&s_vb[worst], 0, sizeof s_vb[worst]);
    return &s_vb[worst];
}

/* ---- the slot ----------------------------------------------------------- */
static void slot_free_sized(const TAGPU_VKPASS* d, SLOT* s)
{
    kill_buffer(d, &s->ubuf, &s->umem, &s->umap);
    kill_buffer(d, &s->pbuf, &s->pmem, &s->pmap);
    kill_buffer(d, &s->vstage, &s->vsmem, &s->vsmap);
    s->ucap = s->pcap = s->vscap = 0;
}

static void slot_free(const TAGPU_VKPASS* d, SLOT* s)
{
    slot_free_sized(d, s);
    kill_image(d, &s->pal, &s->palMem, &s->palView);
    kill_image(d, &s->lut, &s->lutMem, &s->lutView);
    kill_image(d, &s->fogGrid, &s->fogGridMem, &s->fogGridView);
    kill_image(d, &s->fogLut, &s->fogLutMem, &s->fogLutView);
    kill_buffer(d, &s->smallStage, &s->smallMem, &s->smallMap);
    s->fogW = s->fogH = 0;
    s->built = 0;
}

/* the three fixed-size images and the staging buffer that feeds all four */
static int slot_build(const TAGPU_VKPASS* d, SLOT* s)
{
    if (s->built) return 1;
    if (!mk_image(d, 256, 1, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s->pal, &s->palMem, &s->palView))
        return 0;
    if (!mk_image(d, 256, 32, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s->lut, &s->lutMem, &s->lutView))
        return 0;
    if (!mk_image(d, 256, 1, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s->fogLut, &s->fogLutMem, &s->fogLutView))
        return 0;
    s->built = 1;
    return 1;
}

/* the fog grid image and the one staging buffer, together: a grid whose
   dimensions moved rebuilds both, so the buffer and the image it feeds cannot
   disagree about the size */
static int slot_fog(const TAGPU_VKPASS* d, SLOT* s, int w, int h)
{
    VkDeviceSize need;
    if (s->fogGrid && s->fogW == w && s->fogH == h && s->smallStage) return 1;
    kill_image(d, &s->fogGrid, &s->fogGridMem, &s->fogGridView);
    kill_buffer(d, &s->smallStage, &s->smallMem, &s->smallMap);
    s->fogW = s->fogH = 0;
    if (!mk_image(d, w, h, VK_FORMAT_R8G8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s->fogGrid, &s->fogGridMem, &s->fogGridView))
        return 0;
    need = (VkDeviceSize)SMALL_FIXED + (VkDeviceSize)w * h * 2;
    if (!mk_buffer(d, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->smallStage, &s->smallMem, &s->smallMap))
        return 0;
    s->fogW = w; s->fogH = h;
    return 1;
}

/* Grow one of the slot's three sized buffers to what this frame needs. They are
   grown, never shrunk within a session of drawing, and given back whole the
   first frame the pass is handed nothing -- §2.28's rule. */
static int slot_sized(const TAGPU_VKPASS* d, SLOT* s,
                      VkDeviceSize ubytes, VkDeviceSize pbytes)
{
    if (s->ucap < ubytes) {
        kill_buffer(d, &s->ubuf, &s->umem, &s->umap);
        s->ucap = 0;
        if (!mk_buffer(d, ubytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &s->ubuf, &s->umem, &s->umap)) return 0;
        s->ucap = ubytes;
    }
    if (s->pcap < pbytes) {
        kill_buffer(d, &s->pbuf, &s->pmem, &s->pmap);
        s->pcap = 0;
        if (!mk_buffer(d, pbytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &s->pbuf, &s->pmem, &s->pmap)) return 0;
        s->pcap = pbytes;
    }
    return 1;
}

static int slot_vstage(const TAGPU_VKPASS* d, SLOT* s, VkDeviceSize bytes)
{
    if (s->vscap >= bytes && s->vstage) return 1;
    kill_buffer(d, &s->vstage, &s->vsmem, &s->vsmap);
    s->vscap = 0;
    if (!mk_buffer(d, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->vstage, &s->vsmem, &s->vsmap)) return 0;
    s->vscap = bytes;
    return 1;
}

/* ---- build -------------------------------------------------------------- */
static int build_samplers(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    /* THE TWIN'S: every texture this shader samples is GL_NEAREST with
       GL_CLAMP_TO_EDGE -- the atlas, the shade LUT, the palette, the scaffold
       and the fog pair -- because every one of them is looked up by an exact
       texel and a filtered fetch would blend two palette indices. */
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = si.addressModeU;
    si.addressModeW = si.addressModeU;
    si.maxLod = 0.25f;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(d->dev, &si, NULL, &s_samp) != VK_SUCCESS) return 0;

    /* THE COMPARE SAMPLER IS THE SHADOW MAP'S, and it is LINEAR because the GL
       twin's is (GL_COMPARE_REF_TO_TEXTURE + GL_LEQUAL + MIN/MAG LINEAR): the
       bilinear filtering is half of what makes the penumbra smooth. Linear
       filtering of a DEPTH format is a feature bit, so it is asked of the
       format the shadow pass actually chose rather than assumed -- and a device
       that will not offer it keeps a NEAREST sampler and stands the pass down
       on a frame that would really sample the map. The reasoning, and the VUID
       this is deliberately stricter than, is tagpu_vk_terr.c's. */
    {
        int linear = 0;
        VkFormat mapfmt = tagpu_vk_shadow_format(d, &linear);
        s_cmpLinear = linear && mapfmt != VK_FORMAT_UNDEFINED;
    }
    si.magFilter = s_cmpLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = si.magFilter;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(d->dev, &si, NULL, &s_sampCmp) != VK_SUCCESS) return 0;
    return 1;
}

static int build_layouts(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[12];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    int n = 0, i;

    memset(b, 0, sizeof b);
    /* the body's: the two vertex-stage blocks, the fragment-stage one, and the
       nine samplers inc/spirv/tagpu_native.spv.h names for tagpu_native::FS */
    b[n].binding = 0;  b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; n++;
    b[n].binding = 1;  b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; n++;
    b[n].binding = 32; b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; n++;
    for (i = 40; i <= 48; i++) {
        b[n].binding = (uint32_t)i;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[n].descriptorCount = 1;
        b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        n++;
    }
    dli.bindingCount = (uint32_t)n; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dslMain) != VK_SUCCESS) return 0;
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dslMain;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_ploMain) != VK_SUCCESS) return 0;

    /* THE CASTER'S NAMES NO IMAGE AT ALL -- the file header says why. Its
       fragment stage is tagpu_posedraw::DFS, which is `void main(){}`. */
    dli.bindingCount = 2;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dslCast) != VK_SUCCESS) return 0;
    pli.pSetLayouts = &s_dslCast;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_ploCast) != VK_SUCCESS) return 0;
    return 1;
}

/* The two vertex bindings, which are the GL VAO's two buffers and its seven
   attributes at the very strides and offsets tagpu_posebake.c's
   glVertexAttribPointer calls use. */
static void vertex_layout(VkVertexInputBindingDescription* vb,
                          VkVertexInputAttributeDescription* va,
                          VkPipelineVertexInputStateCreateInfo* vi)
{
    memset(vb, 0, sizeof vb[0] * 2);
    vb[0].binding = 0; vb[0].stride = GEOM_STRIDE; vb[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vb[1].binding = 1; vb[1].stride = MAT_STRIDE;  vb[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(va, 0, sizeof va[0] * 7);
    va[0].location = 0; va[0].binding = 0; va[0].format = VK_FORMAT_R32G32B32_SFLOAT; va[0].offset = 0;
    va[1].location = 1; va[1].binding = 0; va[1].format = VK_FORMAT_R32G32B32_SFLOAT; va[1].offset = 12;
    va[2].location = 2; va[2].binding = 0; va[2].format = VK_FORMAT_R32_SFLOAT;       va[2].offset = 24;
    va[3].location = 3; va[3].binding = 0; va[3].format = VK_FORMAT_R32_SFLOAT;       va[3].offset = 28;
    va[4].location = 4; va[4].binding = 1; va[4].format = VK_FORMAT_R32G32_SFLOAT;    va[4].offset = 0;
    va[5].location = 5; va[5].binding = 1; va[5].format = VK_FORMAT_R32G32_SFLOAT;    va[5].offset = 8;
    va[6].location = 6; va[6].binding = 1; va[6].format = VK_FORMAT_R32_SFLOAT;       va[6].offset = 16;
    vi->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi->pNext = NULL; vi->flags = 0;
    vi->vertexBindingDescriptionCount = 2;   vi->pVertexBindingDescriptions = vb;
    vi->vertexAttributeDescriptionCount = 7; vi->pVertexAttributeDescriptions = va;
}

static int build_body_pipeline(const TAGPU_VKPASS* d)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[2];
    VkVertexInputAttributeDescription va[7];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    int ok = 0;

    vs = mk_module(d, tagpu_spv_tagpu_posedraw_VS,
                   sizeof tagpu_spv_tagpu_posedraw_VS / 4);
    fs = mk_module(d, tagpu_spv_tagpu_native_FS,
                   sizeof tagpu_spv_tagpu_native_FS / 4);
    if (!vs || !fs) goto done;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;

    vertex_layout(vb, va, &vi);
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* CULL OFF: the GL lane never enables GL_CULL_FACE, so a unit's back faces
       are rasterised there too. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    /* glEnable(GL_DEPTH_TEST) + glDepthFunc(GL_LESS), and the mask is back ON
       for the bodies (tagpu_native.c restores it before the body loop). */
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.maxDepthBounds = 1.0f;

    /* glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) -- the GL FBO is
       premultiplied -- and glBlendFunc sets the alpha factors as well as the
       colour ones, so both pairs are set here. */
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
    gp.layout = s_ploMain;
    gp.renderPass = d->rp;
    gp.subpass = 0;
    ok = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL,
                                   &s_pipeBody) == VK_SUCCESS;
done:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

/* The caster pipeline, built against the SHADOW pass's render pass. Every piece
   of state here is tagpu_vk_shadow.c's own, because the two draw into the same
   attachment in the same render pass and a caster that tested depth differently
   from the heightfield would make a different map. */
static int build_cast_pipeline(const TAGPU_VKPASS* d, VkRenderPass rp)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[2];
    VkVertexInputAttributeDescription va[7];
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

    vs = mk_module(d, tagpu_spv_tagpu_posedraw_VS,
                   sizeof tagpu_spv_tagpu_posedraw_VS / 4);
    fs = mk_module(d, tagpu_spv_tagpu_posedraw_DFS,
                   sizeof tagpu_spv_tagpu_posedraw_DFS / 4);
    if (!vs || !fs) goto done;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;

    vertex_layout(vb, va, &vi);
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* GL'S CLIP-SPACE Z. The shadow matrix fills [-1, 1] by construction
       (tagpu_shadow.c `mrow`), so without this the near half of every caster is
       clipped away -- §2.32 at length. The caller has already refused to get
       here without `zclipok`. */
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
    gp.layout = s_ploCast;
    gp.renderPass = rp;
    gp.subpass = 0;
    ok = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL,
                                   &s_pipeCast) == VK_SUCCESS;
done:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSet sets[TAGPU_VK_SLOTS];
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    uint32_t i;

    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ps[0].descriptorCount = d->slots * 5;          /* 3 in main, 2 in cast     */
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = d->slots * 9;
    pi.maxSets = d->slots * 2;
    pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &pi, NULL, &s_dpool) != VK_SUCCESS) return 0;

    for (i = 0; i < d->slots; i++) lay[i] = s_dslMain;
    ai.descriptorPool = s_dpool;
    ai.descriptorSetCount = d->slots;
    ai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &ai, sets) != VK_SUCCESS) return 0;
    for (i = 0; i < d->slots; i++) s_slot[i].dsMain = sets[i];

    for (i = 0; i < d->slots; i++) lay[i] = s_dslCast;
    if (vkAllocateDescriptorSets(d->dev, &ai, sets) != VK_SUCCESS) return 0;
    for (i = 0; i < d->slots; i++) s_slot[i].dsCast = sets[i];
    return 1;
}

static int atlas_build(const TAGPU_VKPASS* d, int dim)
{
    if (s_atImg && s_atDim == dim) return 1;
    kill_image(d, &s_atImg, &s_atMem, &s_atView);
    s_atDim = 0; s_atSerial = 0; s_atHave = 0;
    if (!mk_image(d, dim, dim, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s_atImg, &s_atMem, &s_atView))
        return 0;
    s_atDim = dim;
    return 1;
}

/* The two 1x1 stand-ins, cleared once and left in SHADER_READ_ONLY_OPTIMAL.
   Nothing samples them -- the shader reaches uScaf only on the `uScafOn`
   branch and the map only on `uShadowOn` -- but a descriptor has to be valid
   whether or not it is read, and an image whose contents are undefined is one
   more thing to reason about for the price of one call. */
static void dummies_ready(const TAGPU_VKPASS* d, VkCommandBuffer cb)
{
    VkClearColorValue cv;
    VkImageSubresourceRange rg;
    (void)d;
    if (s_dumReady || !s_dumImg) return;
    memset(&cv, 0, sizeof cv);
    memset(&rg, 0, sizeof rg);
    rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.levelCount = 1; rg.layerCount = 1;
    img_barrier(cb, s_dumImg, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cb, s_dumImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rg);
    img_barrier(cb, s_dumImg, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    /* The depth stand-in is CLEARED rather than merely transitioned, which is
       tagpu_vk_terr.c's answer to the same question: nothing samples it --
       taShadowAt returns 1.0 on `uShadowOn == 0` before it touches either
       sampler -- but an image whose contents are undefined is one more thing to
       reason about for the price of one call. */
    {
        VkClearDepthStencilValue dv;
        VkImageSubresourceRange drg;
        memset(&drg, 0, sizeof drg);
        drg.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        drg.levelCount = 1; drg.layerCount = 1;
        dv.depth = 1.0f; dv.stencil = 0;
        img_barrier(cb, s_dumDepth, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearDepthStencilImage(cb, s_dumDepth,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &drg);
        img_barrier(cb, s_dumDepth, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    s_dumReady = 1;
}

static int build(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties props;
    VkFormat dfmt;

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "unit: %u frame slots is outside what this pass carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    if (!resolve(d)) {
        plog(d, "unit: an entry point this pass needs did not resolve");
        return 0;
    }
    if (!d->flipok) {
        plog(d, "unit: this device does not offer VK_KHR_maintenance1, so the "
                "clip-space flip has no pipeline state to ride - the Vulkan "
                "edition of the unit pass stays down (the GL one is unaffected)");
        return 0;
    }
    /* UNITS DEPTH-TEST AND DEPTH-WRITE. Drawing them untested would put an
       aircraft behind the hill it is over. */
    if (d->dfmt == VK_FORMAT_UNDEFINED) {
        plog(d, "unit: the seam's render pass carries no depth attachment and "
                "these draws both test and write depth - the pass stays down");
        return 0;
    }
    vkGetPhysicalDeviceProperties(d->pd, &props);
    s_ualign = props.limits.minUniformBufferOffsetAlignment;
    if (s_ualign == 0) s_ualign = 1;
    /* THE BOUND THE DRIVER SETS ON A UNIFORM WINDOW, checked rather than
       assumed -- the GL twin checks GL_MAX_UNIFORM_BLOCK_SIZE for the same
       reason and refuses to arm below it. The pose block is the big one. */
    if (props.limits.maxUniformBufferRange < POSE_SZ) {
        plog(d, "unit: maxUniformBufferRange is %u and the pose block needs %d "
                "- the pass stays down",
             (unsigned)props.limits.maxUniformBufferRange, POSE_SZ);
        return 0;
    }
    if (!build_samplers(d) || !build_layouts(d) || !build_body_pipeline(d) ||
        !build_descriptors(d))
        return 0;
    /* ONE LINE PER DEVICE, as every other ported pass writes: the numbers a
       later reader needs to check a memory figure against are the device's
       uniform alignment and what it makes the two per-unit strides, and
       neither is knowable from the source alone. */
    plog(d, "unit: up - %u frame slots, uniform offset alignment %u, %u bytes "
            "of blocks and %d of pose per unit, compare sampler %s",
         (unsigned)d->slots, (unsigned)s_ualign,
         (unsigned)(align_up(VGL_SZ, s_ualign) * 2 + align_up(FGL_SZ, s_ualign)),
         (int)align_up(POSE_SZ, s_ualign),
         s_cmpLinear ? "LINEAR" : "NEAREST (the map cannot be sampled)");
    /* the stand-ins, in the map's own format so that one compare sampler is
       valid against both (tagpu_vk_shadow_format, and tagpu_vk_terr.c's note) */
    dfmt = tagpu_vk_shadow_format(d, NULL);
    if (dfmt == VK_FORMAT_UNDEFINED) dfmt = d->dfmt;
    if (!mk_image(d, 1, 1, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s_dumImg, &s_dumMem, &s_dumView))
        return 0;
    if (!mk_image(d, 1, 1, dfmt,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT, &s_dumDepth, &s_dumDepthMem, &s_dumDepthView))
        return 0;
    return 1;
}

/* ---- the frame ---------------------------------------------------------- */
/* The atlas, on the serial and not per frame, into the one image every slot
   shares. The write-after-read barrier orders the copy after every earlier
   frame's sampling of it: a barrier's first synchronisation scope includes
   everything submitted to this queue before it, and a write-after-read hazard
   needs only an execution dependency. */
static int atlas_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, SLOT* s,
                        const TAGPU_PDHAND* h, VkDeviceSize stageOff)
{
    VkDeviceSize bytes;
    int rows = h->atlasRows;
    if (s_atHave && s_atSerial == h->atlasSerial) return 1;
    if (rows < 1) rows = 1;
    if (rows > h->atlasDim) rows = h->atlasDim;
    bytes = (VkDeviceSize)h->atlasDim * rows;
    memcpy(s->vsmap + stageOff, h->atlas, (size_t)bytes);
    img_barrier(cb, s_atImg, VK_IMAGE_ASPECT_COLOR_BIT,
                s_atHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                s_atHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                         : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                s_atHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->vstage, stageOff, s_atImg, h->atlasDim, rows);
    img_barrier(cb, s_atImg, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    s_atSerial = h->atlasSerial;
    s_atHave = 1;
    return 1;
}

/* the two uniform blocks for one unit: the vertex stage's twice (the body's
   and the caster's) and the fragment stage's once, at the std140 offsets
   the generated SPIR-V headers print. A UNION, NOT A CAST: both blocks mix `int` and
   `float` members, and writing an int through a float array is the aliasing
   rule broken at -O2. */
static void fill_blocks(unsigned char* ub, const TAGPU_PDHAND* h,
                        const TAGPU_PDUREC* r, VkDeviceSize vglOff2,
                        VkDeviceSize fglOff)
{
    union { float f[68]; int i[68]; } b;

    /* ---- the vertex stage, the BODY draw ---- */
    memset(&b, 0, sizeof b);
    b.f[0] = h->gw;  b.f[1] = h->gh;                   /* uGame       vec2 @0  */
    b.f[2] = 0.0f;   b.f[3] = 0.0f;                    /* uOffset     vec2 @8  */
    b.f[4] = h->zoom;                                  /* uZoom      float @16 */
    b.f[6] = h->zoomCx; b.f[7] = h->zoomCy;            /* uZoomC      vec2 @24 */
    b.f[8] = h->depthScale;                            /* uDepthScale float @32*/
    b.f[12] = r->anchor[0]; b.f[13] = r->anchor[1];
    b.f[14] = r->anchor[2]; b.f[15] = r->anchor[3];    /* uAnchor     vec4 @48 */
    b.f[16] = r->enc;                                  /* uEnc       float @64 */
    b.f[18] = h->shd[0]; b.f[19] = h->shd[1];          /* uShd        vec2 @72 */
    b.f[20] = r->cast[0]; b.f[21] = r->cast[1];
    b.f[22] = r->cast[2];                              /* uCast       vec3 @80 */
    b.i[23] = 0;                                       /* uDepthPass   int @92 */
    /* uShadowMat @96 -- in GL the vertex and fragment stages share ONE uniform
       of this name, so the body program's vertex copy holds what
       tagpu_shadow_apply wrote. It is never READ here (the shader reaches it
       only on the uDepthPass branch), and it is written anyway because the two
       lanes are being compared and not merely made to agree. */
    memcpy(&b.f[24], h->shadowMat, 64);
    b.i[40] = 0;                                       /* uRange       int @160*/
    b.f[41] = 0.0f;                                    /* uWire      float @164*/
    memcpy(ub, b.f, VGL_SZ);

    /* ---- the vertex stage, the CASTER draw ----
       tagpu_posedraw_depth_begin's own uniforms: the shadow matrix, uDepthPass
       1, uRange BODY, and the projection pair pinned at something finite
       because the vertex shader still evaluates the discarded branch. */
    memset(&b, 0, sizeof b);
    b.f[0] = 1.0f; b.f[1] = 1.0f;                      /* uGame                */
    b.f[4] = 1.0f;                                     /* uZoom                */
    b.f[8] = 1.0f;                                     /* uDepthScale          */
    b.f[12] = r->anchor[0]; b.f[13] = r->anchor[1];
    b.f[14] = r->anchor[2]; b.f[15] = r->anchor[3];
    b.f[16] = r->enc;
    /* uShd is NOT set on the depth program either: the twin leaves it at a
       freshly linked program's zero, and nothing the caster writes reads it. */
    b.f[20] = r->cast[0]; b.f[21] = r->cast[1]; b.f[22] = r->cast[2];
    b.i[23] = 1;                                       /* uDepthPass           */
    memcpy(&b.f[24], h->castMat, 64);                  /* uShadowMat           */
    b.i[40] = 0;                                       /* uRange = BODY        */
    memcpy(ub + vglOff2, b.f, VGL_SZ);

    /* ---- the fragment stage ---- */
    memset(&b, 0, sizeof b);
    b.i[0] = h->restored;                              /* uRestored   int @0   */
    b.i[1] = h->scafOn;                                /* uScafOn     int @4   */
    b.f[4] = h->scafP[0]; b.f[5] = h->scafP[1];
    b.f[6] = h->scafP[2]; b.f[7] = h->scafP[3];        /* uScafP     vec4 @16  */
    b.f[8]  = h->ss;                                   /* uSS       float @32  */
    b.f[9]  = h->zoom;                                 /* uZoomF    float @36  */
    b.f[10] = h->zoomCx; b.f[11] = h->zoomCy;          /* uZoomCF    vec2 @40  */
    b.f[12] = h->fogOrgX; b.f[13] = h->fogOrgY;        /* uFogOrg    vec2 @48  */
    b.f[14] = h->fogCols; b.f[15] = h->fogRows;        /* uFogDim    vec2 @56  */
    b.i[16] = r->fog;                                  /* uFog        int @64  */
    b.i[17] = 0;                                       /* uShadow     int @68  */
    b.f[18] = r->alpha;                                /* uAlpha    float @72  */
    b.f[19] = r->waterT;                               /* uWaterT   float @76  */
    b.i[20] = r->waterMode;                            /* uWaterMode  int @80  */
    b.f[21] = r->digT;                                 /* uDigT     float @84  */
    b.i[22] = r->nanoOn;                               /* uNanoOn     int @88  */
    b.f[23] = r->nanoT;                                /* uNanoT    float @92  */
    b.f[24] = r->nanoC[0]; b.f[25] = r->nanoC[1];
    b.f[26] = r->nanoC[2];                             /* uNanoC     vec3 @96  */
    b.i[27] = h->lit;                                  /* uLit        int @108 */
    b.i[28] = h->lambert;                              /* uLambert    int @112 */
    b.f[32] = h->sun[0]; b.f[33] = h->sun[1];
    b.f[34] = h->sun[2];                               /* uSun       vec3 @128 */
    b.f[35] = h->amb;                                  /* uAmb      float @140 */
    b.f[36] = h->norm;                                 /* uNorm     float @144 */
    b.i[37] = h->shadowOn;                             /* uShadowOn   int @148 */
    b.f[40] = h->shadowSun[0]; b.f[41] = h->shadowSun[1];
    b.f[42] = h->shadowSun[2];                         /* uShadowSun vec3 @160 */
    memcpy(&b.f[44], h->shadowMat, 64);                /* uShadowMat mat4 @176 */
    b.f[60] = h->shScale[0]; b.f[61] = h->shScale[1];
    b.f[62] = h->shScale[2];                           /* uShScale   vec3 @240 */
    b.f[63] = h->penumbra;                             /* uPenumbra float @252 */
    b.f[64] = h->shade;                                /* uShade    float @256 */
    memcpy(ub + fglOff, b.f, FGL_SZ);
}

/* One unit's pose block, written exactly where and exactly as far as
   `upload_pose` writes it: `np * 3` rows from 0, then `nf * 4` floats at each
   of the two packed offsets. The rest of the 14 336-byte window is never read,
   because no vertex of this model carries a piece index past `np`. */
static void fill_pose(unsigned char* pb, const TAGPU_PDHAND* h,
                      const TAGPU_PDUREC* r)
{
    int np = r->npose, nf;
    if (np < 1) return;
    if (np > TAGPU_PBMAXPIECE) np = TAGPU_PBMAXPIECE;
    nf = (np + 3) / 4;
    memcpy(pb, h->rows + (size_t)r->rowOff * 4, (size_t)np * 12 * sizeof(float));
    memcpy(pb + TAGPU_PD_FLAGOFF, h->flags + r->flagOff,
           (size_t)nf * 4 * sizeof(float));
    memcpy(pb + TAGPU_PD_VISOFF, h->vis + r->flagOff,
           (size_t)nf * 4 * sizeof(float));
}

static int draw_room(unsigned n)
{
    DRAW* q;
    unsigned want;
    if (n <= s_drawCap) return 1;
    want = s_drawCap ? s_drawCap * 2 : 128;
    while (want < n) want *= 2;
    q = (DRAW*)realloc(s_draw, (size_t)want * sizeof(DRAW));
    if (!q) return 0;
    s_draw = q; s_drawCap = want;
    return 1;
}

int tagpu_vk_unit_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_PDHAND h;
    SLOT* s;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkDeviceSize ustride, vglOff2, fglOff, pstride, stageOff, stageNeed;
    int fogW = 1, fogH = 1, i, anyUpload = 0, fogWanted = 0;

    s_ndraw = 0; s_ncast = 0; s_drawThis = 0;

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    s = &s_slot[slot];

    /* THE RETIRE'S ACCOUNTING, FIRST AND UNCONDITIONALLY: every path below can
       return early, and the path that returns early is exactly the path that
       must still clear this slot's bit. */
    ret_slot_done(d, slot);

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW, AND NOTHING IS KEPT
       ONCE THERE IS NOT. The pose buffer alone is 14 336 bytes a unit a slot,
       which is the largest thing this pass owns after the atlas; giving slot
       `slot` back at this point needs no new argument and no timer, because it
       is the same instant, and the same ownership, that the rest of this
       function writes it in. */
    if (!tagpu_posedraw_handover(&h, d->frame)) {
        if (s_state == ST_READY) slot_free(d, s);
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_unit_down(d); s_state = ST_REFUSED; return 0; }
        s_state = ST_READY;
    }

    /* ---- every reason not to draw, before a byte is written ---- */

    /* CLASSIC++'s RESTORED ATLAS IS NOT MIRRORED. Drawing with uRestored 0
       against a twin that drew with 1 would be a different picture, and the A/B
       would call it a rasteriser difference. */
    if (h.restored) {
        if (!s_saidRestored) {
            s_saidRestored = 1;
            plog(d, "unit: the GL twin is drawing through the Classic++ restored "
                    "atlas and that surface has no CPU mirror - the Vulkan "
                    "edition draws nothing this session rather than draw a "
                    "different picture from its own oracle");
        }
        return 0;
    }

    /* THE FRAME HAS DRAWS THIS PASS DOES NOT CARRY -- a build ghost, a unit
       past the hand-over's cap. tagpu_posedraw.h says why the count is narrow. */
    if (h.otherDraws > 0) {
        if (!s_saidOther) {
            s_saidOther = 1;
            plog(d, "unit: the GL twin drew %d posed unit(s) this hand-over does "
                    "not carry (a build ghost, or past its cap) - nothing drawn "
                    "while that is true", h.otherDraws);
        }
        return 0;
    }
    s_saidOther = 0;

    if (h.nunit < 1) return 0;
    if (h.nunit > TAGPU_PD_MAXHAND) return 0;       /* the producer's own cap  */
    if (!h.units || !h.rows || !h.flags || !h.vis) return 0;

    /* THE TEXELS. The mirrors are asked for on the twin's own beat and cannot
       be there before the atlas has its dimensions, so the first frames of a
       session legitimately arrive without one. Said once. */
    if (!h.atlas || h.atlasDim < 1 || h.atlasDim > 8192 || !h.lut || !h.pal ||
        !h.fogLut) {
        if (!s_saidNoMirror) {
            s_saidNoMirror = 1;
            plog(d, "unit: a texel mirror this pass needs is not there yet - "
                    "nothing drawn until it is (the unit atlas is asked for on "
                    "the twin's publish and converges over the next frames)");
        }
        return 0;
    }
    s_saidNoMirror = 0;
    if (h.lutW != 256 || h.lutH != 32) {
        plog(d, "unit: the shade LUT is %dx%d and this pass carries 256x32 - "
                "nothing drawn", h.lutW, h.lutH);
        return 0;
    }

    /* THE FOG GRID, and the bound re-checked in this file's own terms. A unit
       with `uFog & 1` samples it, so a frame that wants one and has none is a
       refusal rather than a frame drawn without fog. */
    for (i = 0; i < h.nunit; i++) if (h.units[i].fog & 1) { fogWanted = 1; break; }
    if (h.fogGrid) {
        if (h.fogGridCols < 1 || h.fogGridRows < 1 ||
            h.fogGridCols > FOG_MAXDIM || h.fogGridRows > FOG_MAXDIM) {
            plog(d, "unit: a %dx%d fog grid is outside what this pass carries - "
                    "nothing drawn", h.fogGridCols, h.fogGridRows);
            return 0;
        }
        fogW = h.fogGridCols; fogH = h.fogGridRows;
    } else if (fogWanted) {
        if (!s_saidFog) {
            s_saidFog = 1;
            plog(d, "unit: a unit this frame samples the fog overlay and the "
                    "hand-over carries no grid - nothing drawn while that is true");
        }
        return 0;
    }
    s_saidFog = 0;

    /* ---- THE SCAFFOLD, AND THE HALF OF IT THIS LANDING DOES NOT CLOSE ----

       Half the question IS answered, and the mechanism is in place: the
       overlay is another pass's image, and tagpu_vk_scaffold.h now exposes a
       FRAME-STAMPED per-slot view in exactly the shape tagpu_vk_shadow.h
       settled on, which `bind_main` points binding 44 at. That is the part
       the effects and hi-res passes will reuse.

       THE OTHER HALF IS `gl_FragCoord`, AND IT CANNOT BE RECONCILED HERE.
       TAGPU_GLSL_SCAF_TEST (tagpu_glsl.h) locates the fragment in the game
       frame with `gl_FragCoord.xy / uSS` -- and the two APIs disagree about
       where that coordinate is measured from. GL's origin is the LOWER left;
       Vulkan's is the UPPER left and `OriginUpperLeft` is the only execution
       mode Vulkan permits, so a fragment this lane draws at stored row r reads
       `r + 0.5` where the GL twin reads `H - (r + 0.5)`. With the negative
       viewport height this pass takes for its geometry, the two are exact
       mirrors -- so the scaffold lookup would land on the wrong end of the
       overlay, and the `discard` it drives would cut the wrong fragments.

       THREE WAYS OUT WERE CONSIDERED AND ALL THREE ARE WORSE THAN STANDING
       DOWN:
         * Editing the macro to take the origin as a uniform changes the GL
           twin, which is both the oracle and the shipped renderer.
         * Re-deriving `uScafP` so the mirrored `gl_FragCoord` comes out at
           GL's value is possible on paper -- w = -vh, y = C - vpT with C
           folding the zoom un-transform -- but it is the port re-deriving the
           pass's inputs, which is the one thing this lane does not do, and it
           silently breaks the day the macro changes.
         * Mirroring the uploaded overlay does NOT cancel: it would need the
           viewport's top and bottom margins to be equal, and they are 32 and
           33.
       So a frame whose twin has the overlay armed is refused, exactly as the
       effects pass refuses one (gpu-status §2.31), and the reason is written
       down rather than worked around. It costs nothing in play: `scaffold.on`
       is not in the default arm set and its own note says to leave it
       disarmed.

       ANY LATER PASS WHOSE FRAGMENT SHADER READS `gl_FragCoord` INHERITS
       THIS. The hi-res path and the effects pass both carry the same macro. */
    if (h.scafOn) {
        if (!s_saidScaf) {
            s_saidScaf = 1;
            plog(d, "unit: the GL twin is sampling the scaffold overlay through "
                    "gl_FragCoord, whose origin is the lower left in GL and the "
                    "upper left in Vulkan - this pass's flipped viewport makes "
                    "the two exact mirrors, so nothing is drawn while the "
                    "overlay is armed rather than cut the wrong fragments");
        }
        return 0;
    }
    s_saidScaf = 0;

    /* ---- the slot is ours: the seam waited on fence[slot] at the top of this
       frame, so the submit that last used these buffers, these images and these
       descriptor sets has completed. ---- */
    if (!slot_build(d, s)) goto refuse;
    if (!slot_fog(d, s, fogW, fogH)) goto refuse;
    if (!atlas_build(d, h.atlasDim)) goto refuse;

    ustride = align_up(VGL_SZ, s_ualign);
    vglOff2 = ustride;
    fglOff  = ustride * 2;
    ustride = ustride * 2 + align_up(FGL_SZ, s_ualign);
    pstride = align_up(POSE_SZ, s_ualign);
    if (!slot_sized(d, s, ustride * (VkDeviceSize)h.nunit,
                    pstride * (VkDeviceSize)h.nunit)) goto refuse;

    /* ---- what has to be uploaded this frame, and the staging to carry it ---- */
    if (!draw_room((unsigned)h.nunit)) goto refuse;
    stageNeed = 0;
    for (i = 0; i < h.nunit; i++) {
        const TAGPU_PDUREC* r = &h.units[i];
        if (!vb_find(r->geomSerial))
            stageNeed += (VkDeviceSize)r->nvert * GEOM_STRIDE;
        if (!vb_find(r->matSerial))
            stageNeed += (VkDeviceSize)r->nvert * MAT_STRIDE;
    }
    if (!s_atHave || s_atSerial != h.atlasSerial)
        stageNeed += (VkDeviceSize)h.atlasDim * h.atlasDim;
    if (stageNeed) {
        if (!slot_vstage(d, s, stageNeed)) goto refuse;
    } else if (s->vstage) {
        /* nothing to upload this frame: give the staging back, as the feature
           pass gives its atlas staging back -- a settled scene holds none */
        kill_buffer(d, &s->vstage, &s->vsmem, &s->vsmap);
        s->vscap = 0;
    }

    stageOff = 0;
    for (i = 0; i < h.nunit; i++) {
        const TAGPU_PDUREC* r = &h.units[i];
        VBENT* g = vb_find(r->geomSerial);
        VBENT* m = vb_find(r->matSerial);
        DRAW* w;
        int j;

        /* THE BOUNDS, RE-CHECKED IN THIS FILE'S OWN TERMS. Each of these sizes
           an allocation or a memcpy, and a bound that lives in the file that
           produced the number is a bound only while both files are read
           together. */
        if (r->nvert < 1 || r->nvert > 262144) continue;
        if (r->count < 1 || r->first < 0 || r->first + r->count > r->nvert) continue;
        if (r->npose < 1 || r->npose > TAGPU_PBMAXPIECE) continue;
        if ((size_t)r->rowOff * 4 + (size_t)r->npose * 12 > h.nrow * 4) continue;
        if (r->flagOff + (unsigned)(((r->npose + 3) / 4) * 4) > h.nflag) continue;

        for (j = 0; j < 2; j++) {
            VBENT** e = j ? &m : &g;
            unsigned serial = j ? r->matSerial : r->geomSerial;
            int st = j ? MAT_STRIDE : GEOM_STRIDE;
            const float* src;
            int nv = 0;
            VkDeviceSize bytes;
            VkBufferCopy cp;
            if (*e) continue;
            /* THE MIRROR IS ASKED FOR WITH THE SERIAL, at the instant of the
               read and by the module that owns it -- tagpu_posebake.h. A type
               whose entry has been evicted and re-baked answers NULL, and this
               unit is then simply not drawn, which the caster census turns into
               a refusal of the whole map. */
            src = j ? tagpu_posebake_mat_mirror((const TAGPU_PBMAT*)r->mat, serial, &nv)
                    : tagpu_posebake_geom_mirror((const TAGPU_PBGEOM*)r->geom, serial, &nv);
            if (!src || nv != r->nvert) break;
            *e = vb_slot(d, d->frame);
            if (!*e) {
                if (!s_saidVbFull) {
                    s_saidVbFull = 1;
                    plog(d, "unit: the per-type vertex buffer table is full and "
                            "every entry is in this frame - nothing drawn this "
                            "frame (VB_MAX %d)", VB_MAX);
                }
                break;
            }
            bytes = (VkDeviceSize)nv * st;
            if (!mk_buffer(d, bytes,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           &(*e)->buf, &(*e)->mem, NULL)) {
                memset(*e, 0, sizeof **e);
                goto refuse;
            }
            (*e)->serial = serial;
            (*e)->size = bytes;
            /* STAMPED AT CREATION, not only when the unit is finally drawn. The
               copy below is already recorded into `cb`, so an entry left at
               frame 0 could be chosen by `vb_slot` for the NEXT unit of this
               same frame -- `vb_slot` skips only entries this frame has
               touched. The retire would still keep the buffer alive (nothing is
               destroyed until every slot has turned over), so this is not a
               use-after-free; it is a buffer uploaded and then immediately
               thrown away, which is worth one store to prevent. */
            (*e)->lastFrame = d->frame;
            memcpy(s->vsmap + stageOff, src, (size_t)bytes);
            memset(&cp, 0, sizeof cp);
            cp.srcOffset = stageOff; cp.dstOffset = 0; cp.size = bytes;
            vkCmdCopyBuffer(cb, s->vstage, (*e)->buf, 1, &cp);
            stageOff += bytes;
            anyUpload = 1;
        }
        if (!g || !m) continue;
        g->lastFrame = d->frame;
        m->lastFrame = d->frame;

        w = &s_draw[s_ndraw];
        w->geom = g->buf; w->mat = m->buf;
        w->first = (uint32_t)r->first; w->count = (uint32_t)r->count;
        w->unit = (uint32_t)i;
        w->casts = (h.depthOn && r->casts) ? 1 : 0;
        if (w->casts) s_ncast++;
        s_ndraw++;

        fill_blocks(s->umap + (VkDeviceSize)i * ustride, &h, r, vglOff2, fglOff);
        fill_pose(s->pmap + (VkDeviceSize)i * pstride, &h, r);
    }

    /* EVERY UNIT OR NONE. A frame drawn with one type missing is a frame the
       A/B reports as a port failure, and the shadow map would be missing a
       caster besides. */
    if (s_ndraw != (unsigned)h.nunit) {
        if (!s_saidNoMirror) {
            s_saidNoMirror = 1;
            plog(d, "unit: %u of %d posed units could be drawn this frame - the "
                    "rest have no bake mirror (a type evicted and re-baked). "
                    "Nothing drawn while that is true",
                 s_ndraw, h.nunit);
        }
        s_ndraw = 0; s_ncast = 0;
        return 0;
    }

    if (!atlas_upload(d, cb, s, &h, stageOff)) goto refuse;

    /* THE FOUR SMALL IMAGES, per slot, so the one-line invariant covers them:
       UNDEFINED in, because the whole of each is re-sent every frame and there
       are therefore no contents to preserve and no layout to carry. */
    memcpy(s->smallMap + SMALL_PALOFF, h.pal, 256 * 4);
    memcpy(s->smallMap + SMALL_SHDOFF, h.lut, 256 * 32);
    memcpy(s->smallMap + SMALL_FLTOFF, h.fogLut, 256);
    /* The grid is one `unsigned short` a cell and the image is RG8: the same
       two bytes in the same order, which is exactly what the GL twin uploads
       (GL_RG / GL_UNSIGNED_BYTE over this very buffer). With no grid this frame
       the image is one zero cell, which the shader never reads -- taFog is
       called only on the `uFog & 1` branch, and a frame that wanted one and had
       none was refused above. */
    if (h.fogGrid) memcpy(s->smallMap + SMALL_FOGOFF, h.fogGrid,
                          (size_t)fogW * fogH * 2);
    else           memset(s->smallMap + SMALL_FOGOFF, 0, 2);

    img_barrier(cb, s->pal, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->lut, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->fogLut, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->fogGrid, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->smallStage, SMALL_PALOFF, s->pal, 256, 1);
    copy_rect(cb, s->smallStage, SMALL_SHDOFF, s->lut, 256, 32);
    copy_rect(cb, s->smallStage, SMALL_FLTOFF, s->fogLut, 256, 1);
    copy_rect(cb, s->smallStage, SMALL_FOGOFF, s->fogGrid, fogW, fogH);
    img_barrier(cb, s->pal, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->lut, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->fogLut, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->fogGrid, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    /* ONE BARRIER FOR EVERY VERTEX BUFFER WRITTEN THIS FRAME. A buffer created
       this frame has no prior access to order against, so the only hazard is
       this transfer against the reads the caster draw and the body draw are
       about to make out of the SAME command buffer -- which is one
       TRANSFER_WRITE -> VERTEX_ATTRIBUTE_READ dependency whatever the count. */
    if (anyUpload) {
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                             1, &mb, 0, NULL, 0, NULL);
    }

    dummies_ready(d, cb);

    s_scissorOn = h.scissorOn;
    s_vpL = h.vpL; s_vpT = h.vpT; s_vw = h.vw; s_vh = h.vh;
    s_shadowOn = h.shadowOn;

    /* THE A/B FRAME IS CLAIMED LAST, AFTER EVERY REASON NOT TO DRAW IS PAST. A
       frame claimed and then not drawn would have the seam capture a bare clear
       against a GL half that has the units in it, and report every unit pixel
       as differing: a port failure that is really an oracle failure. */
    s_abFrame = h.ab;
    s_drawThis = 1;
    /* the offsets `cast`, `prepare` and `record` bind with */
    s_uStride = ustride; s_vglOff2 = vglOff2; s_fglOff = fglOff;
    s_pStride = pstride;
    return 1;

refuse:
    /* NOTHING IS DESTROYED HERE, AND THAT IS THE WHOLE POINT. This is the
       middle of a frame. The seam waited on fence[slot] ALONE, so every OTHER
       slot's submit is still executing against this pass's pipelines, its
       descriptor pool, its shared atlas and its per-slot buffers -- and `cb`,
       which this function has already recorded uploads into, is submitted
       whether this pass draws or not. Destroying any of it from here is a
       use-after-free on the FIRST refusal, not a rare one. So the pass stops
       drawing at once and OWES a teardown; the seam pays it at the top of a
       later frame, behind the vkDeviceWaitIdle that makes "no submit names
       these objects" a fact rather than a hope. */
    plog(d, "unit: slot %u would not take this frame's resources - the pass "
            "stops drawing and the seam tears it down", (unsigned)slot);
    s_state = ST_REFUSED;
    s_downOwed = 1;
    s_ndraw = 0; s_ncast = 0; s_drawThis = 0;
    return 0;
}

int tagpu_vk_unit_casters(void)
{
    return (s_state == ST_READY && s_drawThis) ? (int)s_ncast : 0;
}

int tagpu_vk_unit_cast(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                       VkRenderPass rp)
{
    VkDeviceSize zero = 0;
    unsigned i, drew = 0;

    if (s_state != ST_READY || !s_drawThis || !s_ncast) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    /* The caller has already refused to draw a map at all without it, but this
       pipeline is built here and the bound belongs with the build. */
    if (!d->zclipok) return -1;

    if (!s_pipeCast || s_castRp != rp) {
        if (s_pipeCast) { vkDestroyPipeline(d->dev, s_pipeCast, NULL); s_pipeCast = VK_NULL_HANDLE; }
        if (!build_cast_pipeline(d, rp)) {
            plog(d, "unit: the caster pipeline would not build against the "
                    "shadow pass's render pass - the map is incomplete and the "
                    "pass says so rather than leaving a caster out of it");
            s_castRp = VK_NULL_HANDLE;
            return -1;
        }
        s_castRp = rp;
    }

    /* THE CASTER SET, WRITTEN HERE, and this is the one place in this file that
       writes a descriptor outside `prepare`. It is legal for the same reason
       `prepare`'s is: the seam waited on fence[slot], so nothing of ours is in
       flight for this slot and no submitted command buffer names this set.
       What it may NOT name is an image, and it names none -- see the file
       header on why the map itself is out of reach at this instant. */
    {
        VkDescriptorBufferInfo bi[2];
        VkWriteDescriptorSet wr[2];
        memset(bi, 0, sizeof bi); memset(wr, 0, sizeof wr);
        bi[0].buffer = s_slot[slot].ubuf; bi[0].offset = 0; bi[0].range = VGL_SZ;
        bi[1].buffer = s_slot[slot].pbuf; bi[1].offset = 0; bi[1].range = POSE_SZ;
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = s_slot[slot].dsCast; wr[0].dstBinding = 0;
        wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[0].pBufferInfo = &bi[0];
        wr[1] = wr[0]; wr[1].dstBinding = 1; wr[1].pBufferInfo = &bi[1];
        vkUpdateDescriptorSets(d->dev, 2, wr, 0, NULL);
    }

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeCast);
    for (i = 0; i < s_ndraw; i++) {
        const DRAW* w = &s_draw[i];
        VkBuffer vbs[2];
        VkDeviceSize offs[2];
        uint32_t dyn[2];
        if (!w->casts) continue;
        /* the CASTER's vertex block, which is the second of this unit's two */
        dyn[0] = (uint32_t)((VkDeviceSize)w->unit * s_uStride + s_vglOff2);
        dyn[1] = (uint32_t)((VkDeviceSize)w->unit * s_pStride);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ploCast,
                                0, 1, &s_slot[slot].dsCast, 2, dyn);
        vbs[0] = w->geom; vbs[1] = w->mat;
        offs[0] = zero;   offs[1] = zero;
        vkCmdBindVertexBuffers(cb, 0, 2, vbs, offs);
        vkCmdDraw(cb, w->count, 1, w->first, 0);
        drew++;
    }
    return (int)drew;
}

/* Point this slot's main set at everything it samples, as those things stand
   NOW. On every prepare, and that is the point: it is the one instant the
   seam's fence proves this set is not in use, so another pass's image -- the
   shadow map, the scaffold -- is picked up here rather than by a write that
   would reach a set some other frame is still reading. */
static void bind_main(const TAGPU_VKPASS* d, uint32_t slot)
{
    SLOT* s = &s_slot[slot];
    VkDescriptorBufferInfo bi[3];
    VkDescriptorImageInfo ii[9];
    VkWriteDescriptorSet wr[12];
    VkImageView shadow = tagpu_vk_shadow_view(d->frame, slot);
    VkImageView scaf = tagpu_vk_scaffold_view(d->frame, slot);
    int i, n = 0;

    memset(bi, 0, sizeof bi); memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
    if (!shadow) shadow = s_dumDepthView;
    if (!scaf) scaf = s_dumView;

    bi[0].buffer = s->ubuf; bi[0].offset = 0; bi[0].range = VGL_SZ;
    bi[1].buffer = s->pbuf; bi[1].offset = 0; bi[1].range = POSE_SZ;
    bi[2].buffer = s->ubuf; bi[2].offset = 0; bi[2].range = FGL_SZ;
    for (i = 0; i < 3; i++) {
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[n].dstSet = s->dsMain;
        wr[n].dstBinding = (uint32_t)(i == 2 ? 32 : i);
        wr[n].descriptorCount = 1;
        wr[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[n].pBufferInfo = &bi[i];
        n++;
    }

    /* 40 uAtlas, 41 uLUT, 42 uPal, 43 uAtlasRGB, 44 uScaf, 45 uFogGrid,
       46 uFogLUT, 47 uShadowCmp, 48 uShadowRaw -- the order
       inc/spirv/tagpu_native.spv.h prints for tagpu_native::FS.
       BINDING 43 IS THE ATLAS'S OWN VIEW ON PURPOSE: the fragment stage reads
       uAtlasRGB only on the `uRestored == 1` branch and this pass refuses every
       such frame, but a descriptor must be VALID for the set to be bound, and
       naming the image that is already here costs no memory and no second
       object. If the Classic++ restored atlas is ever mirrored, this is the
       binding that stops being a placeholder. */
    ii[0].sampler = s_samp; ii[0].imageView = s_atView;
    ii[1].sampler = s_samp; ii[1].imageView = s->lutView;
    ii[2].sampler = s_samp; ii[2].imageView = s->palView;
    ii[3].sampler = s_samp; ii[3].imageView = s_atView;
    /* BINDING 44 NAMES THE REAL OVERLAY WHEN THERE IS ONE, even though this
       pass refuses every frame that samples it (see `upload`): the mechanism
       is what the next landing needs, and a descriptor that names the actual
       image is the one that will still be right when the `gl_FragCoord`
       question is answered. Until then it is a valid descriptor nothing
       reads. */
    ii[4].sampler = s_samp; ii[4].imageView = scaf;
    ii[5].sampler = s_samp; ii[5].imageView = s->fogGridView;
    ii[6].sampler = s_samp; ii[6].imageView = s->fogLutView;
    ii[7].sampler = s_sampCmp; ii[7].imageView = shadow;
    ii[8].sampler = s_samp;    ii[8].imageView = shadow;
    for (i = 0; i < 9; i++) {
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[n].dstSet = s->dsMain;
        wr[n].dstBinding = (uint32_t)(40 + i);
        wr[n].descriptorCount = 1;
        wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[n].pImageInfo = &ii[i];
        n++;
    }
    vkUpdateDescriptorSets(d->dev, (uint32_t)n, wr, 0, NULL);
}

int tagpu_vk_unit_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    (void)cb;
    if (s_state != ST_READY || !s_drawThis) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;

    /* THE MAP THE GL TWIN SAMPLED HAS TO BE THE MAP WE SAMPLE. `uShadowOn` came
       from the twin's own tagpu_shadow_apply, so if it is 1 and this frame's
       Vulkan map is not complete, every shadowed fragment would be lit here and
       dark there -- which the A/B would report as a rasteriser difference. The
       terrain pass stands down on exactly this. */
    if (s_shadowOn && !tagpu_vk_shadow_ready(d->frame)) {
        if (!s_saidShadow) {
            s_saidShadow = 1;
            plog(d, "unit: the GL twin drew these units against a cast-shadow map "
                    "and the Vulkan lane has none this frame - nothing drawn "
                    "while that is true");
        }
        s_drawThis = 0;
        s_abFrame = 0;
        return 0;
    }
    s_saidShadow = 0;

    /* A COMPARE SAMPLER THAT IS NOT THE TWIN'S. The GL PCF is bilinear and
       linear filtering of a depth format is a feature bit; a device that will
       not offer it would draw a harder penumbra than the oracle's. */
    if (s_shadowOn && !s_cmpLinear) {
        plog(d, "unit: this device will not filter a depth format linearly and "
                "the GL twin's shadow PCF is bilinear - nothing drawn on a "
                "frame that samples the map");
        s_drawThis = 0;
        s_abFrame = 0;
        return 0;
    }

    bind_main(d, slot);
    return 1;
}

/* the world scissor, flipped onto Vulkan's framebuffer coordinates. The two
   APIs disagree about which row of the target is row 0, and with the negative
   viewport height this pass uses, a RECTANGLE in one is the vertical mirror of
   the same rectangle in the other -- tagpu_vk_feat.c is where this is argued at
   length. Getting it wrong shows up as the world clipped against the wrong
   edge rather than as anything subtle. */
static void unit_scissor(uint32_t w, uint32_t h, VkRect2D* sc)
{
    int x = 0, y = 0, cw = (int)w, ch = (int)h;
    if (s_scissorOn && s_vw > 0 && s_vh > 0) {
        x = s_vpL; cw = s_vw;
        y = (int)h - (s_vpT + s_vh);
        ch = s_vh;
    }
    if (x < 0) { cw += x; x = 0; }
    if (y < 0) { ch += y; y = 0; }
    if (x > (int)w) x = (int)w;
    if (y > (int)h) y = (int)h;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    if (x + cw > (int)w) cw = (int)w - x;
    if (y + ch > (int)h) ch = (int)h - y;
    sc->offset.x = x; sc->offset.y = y;
    sc->extent.width = (uint32_t)cw; sc->extent.height = (uint32_t)ch;
}

void tagpu_vk_unit_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    unsigned i;

    (void)d;
    if (s_state != ST_READY || !s_drawThis) return;
    s_drawThis = 0;

    /* THE FLIP, AND THE DEPTH RANGE. y starts at the bottom and the height is
       negative, so clip space is turned over once and the ported shader keeps
       GL's convention without a character changing. minDepth 0.5 / maxDepth 1.0
       maps clip z in [0, 1] onto GL's own (z+1)/2 -- the vertex shader writes
       `clamp(1.0 - enc/uDepthScale, 0, 1)`, which is already in [0, 1], so this
       needs no extension and must not use one. */
    vp.x = 0.0f;
    vp.y = (float)h;
    vp.width = (float)w;
    vp.height = -(float)h;
    vp.minDepth = 0.5f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    unit_scissor(w, h, &sc);
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeBody);
    for (i = 0; i < s_ndraw; i++) {
        const DRAW* q = &s_draw[i];
        VkBuffer vbs[2];
        VkDeviceSize offs[2];
        uint32_t dyn[3];
        dyn[0] = (uint32_t)((VkDeviceSize)q->unit * s_uStride);
        dyn[1] = (uint32_t)((VkDeviceSize)q->unit * s_pStride);
        dyn[2] = (uint32_t)((VkDeviceSize)q->unit * s_uStride + s_fglOff);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ploMain,
                                0, 1, &s_slot[slot].dsMain, 3, dyn);
        vbs[0] = q->geom; vbs[1] = q->mat;
        offs[0] = 0;      offs[1] = 0;
        vkCmdBindVertexBuffers(cb, 0, 2, vbs, offs);
        vkCmdDraw(cb, q->count, 1, q->first, 0);
    }
}

int tagpu_vk_unit_ab_frame(void)
{
    int a = s_abFrame;
    s_abFrame = 0;
    return a;
}

void tagpu_vk_unit_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    int k;
    /* WHETHER THIS TEARDOWN IS THE ONE THE PASS ASKED FOR. Only the seam's
       `_down_paid` sets it, and only after its vkDeviceWaitIdle -- so a
       `vk_down` or a `vk_resize` that happens to run while a debt is
       outstanding tears the pass down and leaves it coming back ST_UNBUILT
       rather than latched at ST_REFUSED by the back door. The debt is
       discharged by every teardown; the VERDICT is not. */
    int owed = s_downPaying;
    s_downOwed = 0;
    s_drawThis = 0; s_abFrame = 0; s_ndraw = 0; s_ncast = 0;
    s_shadowOn = 0;

    /* NOTHING TO FREE, BUT THE VERDICT STILL STANDS. */
    if (!d->dev || !vkDestroyPipeline) {
        s_state = owed ? ST_REFUSED : ST_UNBUILT;
        return;
    }
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        slot_free(d, &s_slot[i]);
        s_slot[i].dsMain = VK_NULL_HANDLE;   /* both go back with the pool */
        s_slot[i].dsCast = VK_NULL_HANDLE;
    }
    for (k = 0; k < VB_MAX; k++)
        if (s_vb[k].buf) { kill_buffer(d, &s_vb[k].buf, &s_vb[k].mem, NULL);
                           memset(&s_vb[k], 0, sizeof s_vb[k]); }
    for (k = 0; k < RET_MAX; k++)
        if (s_ret[k].buf) { kill_buffer(d, &s_ret[k].buf, &s_ret[k].mem, NULL);
                            s_ret[k].pending = 0; }
    kill_image(d, &s_atImg, &s_atMem, &s_atView);
    kill_image(d, &s_dumImg, &s_dumMem, &s_dumView);
    kill_image(d, &s_dumDepth, &s_dumDepthMem, &s_dumDepthView);
    s_atDim = 0; s_atSerial = 0; s_atHave = 0; s_dumReady = 0;
    if (s_dpool) { vkDestroyDescriptorPool(d->dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_pipeBody) { vkDestroyPipeline(d->dev, s_pipeBody, NULL); s_pipeBody = VK_NULL_HANDLE; }
    if (s_pipeCast) { vkDestroyPipeline(d->dev, s_pipeCast, NULL); s_pipeCast = VK_NULL_HANDLE; }
    s_castRp = VK_NULL_HANDLE;
    if (s_ploMain) { vkDestroyPipelineLayout(d->dev, s_ploMain, NULL); s_ploMain = VK_NULL_HANDLE; }
    if (s_ploCast) { vkDestroyPipelineLayout(d->dev, s_ploCast, NULL); s_ploCast = VK_NULL_HANDLE; }
    if (s_dslMain) { vkDestroyDescriptorSetLayout(d->dev, s_dslMain, NULL); s_dslMain = VK_NULL_HANDLE; }
    if (s_dslCast) { vkDestroyDescriptorSetLayout(d->dev, s_dslCast, NULL); s_dslCast = VK_NULL_HANDLE; }
    if (s_samp) { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_sampCmp) { vkDestroySampler(d->dev, s_sampCmp, NULL); s_sampCmp = VK_NULL_HANDLE; }
    s_cmpLinear = 0;
    /* ST_UNBUILT and not ST_REFUSED: a pass brought down by a mode change or a
       cleared lever must be able to come back. The one exception is the
       teardown this pass asked for. */
    s_state = owed ? ST_REFUSED : ST_UNBUILT;
}

int tagpu_vk_unit_down_owed(void) { return s_downOwed; }

/* THE SEAM'S OWN ENTRY POINT, called only after its vkDeviceWaitIdle. It is
   what makes `_down`'s ST_REFUSED latch apply to the owed teardown and to
   nothing else. */
void tagpu_vk_unit_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_unit_down(d);          /* clears s_downOwed itself */
    s_downPaying = 0;
}
