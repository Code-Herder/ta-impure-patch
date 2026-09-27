/* tagpu_vk_fx.c -- the effects pass (weapon fire, explosions, debris, and the
   ten particle layers) drawn by Vulkan. Contract: tagpu_vk_fx.h. The FOURTH
   WORLD PASS.

   THE GATHER IS tagpu_fx.c's; THIS FILE ONLY DRAWS. Everything arrives
   through `tagpu_fx_handover` (tagpu_fx.h): the vertices are the four bucket
   arrays the gather filled, the uniforms are the numbers it derived, the
   texels are CPU bytes -- the atlas through tagpu_gaf.c's CPU mirror, which
   is written by the same atlas_paint that lays the atlas out -- and the
   shader is tagpu_fx.c's GLSL, compiled to SPIR-V by tools/spirv-gen.py.

   ---- WHAT THIS PASS HAS TO ANSWER, AND WHERE EACH ANSWER IS ----

   1. THREE PIPELINES, TWO PROGRAMS. The gather fills four buckets:

        B_UNDER    TRIANGLES, blend ONE / ONE_MINUS_SRC_ALPHA
        B_LINES    line records, the same blend, the LVS/LFS program
        B_FLASH    TRIANGLES, blend ONE / ONE -- pure additive light
        B_SPRITES  TRIANGLES, back to the first blend

      Program, vertex rate and blend are all baked into a VkPipeline, so that
      is three objects off one layout: `s_pipeTri`, `s_pipeLine`,
      `s_pipeFlash`. The feature pass needed two for its depth-write modes and
      this is the same shape, one step further. VK_EXT_extended_dynamic_state3
      would make the blend dynamic and buy exactly one object; it is not worth
      an extension.

   2. THE LINES ARE TRIANGLES. Vulkan does not pin down which pixels a line
      primitive lights -- its default mode is implementation-dependent, and
      even VK_EXT_line_rasterization's BRESENHAM mode allows a deviation of one
      unit -- and a line wider than one target pixel needs `wideLines`. So a
      laser or a lightning segment is one record in the LINES bucket, drawn as
      an instance of tagpu_fx.c's LVS/LFS: a band of two triangles round the
      segment whose fragment stage keeps exactly the line-grid pixels of
      `0x4CC7AB`'s walk, `ss` wide (tagpu_line.h, tagpu_glsl.h). No device
      feature is asked for and none can refuse the lines.

   3. THE FLASH LIGHT TABLE IS A THREE-BYTE FORMAT, AND VULKAN DOES NOT HAVE
      ONE. The producer hands it over as 32 x 3 bytes (tagpu_fx.h `lht`);
      `VK_FORMAT_R8G8B8_UNORM` is optional and is not supported for sampled
      images on the device this lane is measured on. So the 96 bytes are
      EXPANDED to 32 x 1 RGBA8 on the way into the staging buffer. The shader
      reads `.rgb`, so the alpha it gains is never looked at, and the three
      bytes that matter are the producer's own. A format a device may not
      support is exactly the kind of thing that is invisible until a picture
      is wrong; `build_sampler` asks for every format this pass samples by
      name.

   4. DEPTH: TESTED, NEVER WRITTEN, for the whole pass. Effects are transient
      light and must occlude nothing, but aircraft drawn earlier must still
      cover them. All three pipelines therefore test LESS and write nothing.
      The depth RANGE is `minDepth 0.5 / maxDepth 1.0`, the mapping every
      world pass that shares the depth attachment uses (terrain, features,
      units, marks), so their depths compare -- the feature pass's item 1 has
      the argument in full.

   5. THE SCISSOR is the world viewport, not its vertical mirror.
      tagpu_vk_feat.c item 3 is the argument; `fx_scissor` is the arithmetic.

   6. NO Y FLIP. This pass writes `gl_Position.y = p.y/uGame.y*2 - 1` on the
      engine's screen-space y, which grows DOWNWARD, so clip +1 is the BOTTOM of
      the game frame. The world target is copied onto the frame with no flip
      either (tagpu_vk_world.c), so a negative viewport height would turn the
      frame over and present the world upside down. The viewport is positive
      and clip -1 lands on row 0, which is the game's top row.
      An A/B between two builds cannot catch this: it is structurally blind to
      a flip they share. (VK_KHR_maintenance1 is needed
      only for a negative height, so this pass does not require it;
      `tagpu_vk_gui.c`, `_fps.c` and `_scaffold.c` do, because their shaders
      are y-UP and their flip is correct.)

   ---- WHAT IT DOES NOT DO ----

   THE SCAFFOLD TEST. `uScafOn` is 0 on every draw, so the fragment shader
   never samples `uScaf`: the scaffold's texels are in an image
   tagpu_vk_scaffold.c owns privately, and sharing one image between two
   passes is a mechanism with an ordering contract of its own that is not
   built. The scaffold is a debug overlay and is not in the default arm set.

   IT SAMPLES NO OTHER PASS'S RESTORED ATLAS: Classic++'s restored atlas is
   this pass's own. When the producer publishes
   a restore list, `restore_want` has this pass's restore job paint `s_arImg`
   from the base atlas, and binding 42 samples it. Until the job has
   painted a frame of the current generation the pass draws the base atlas
   with `uRestored` 0 (see `prepare`), decided after `restore_want`, which
   makes and feeds the job -- the job itself paints in
   `tagpu_vk_restore_step`, after every pass's prepare -- so a recycle's
   frame draws the base atlas too, and binding 42 names the twin only on the
   frames it is a picture (`twin_bind`).

   THE EFFECTS *MODELS* ARE NOT THIS PASS. RenderType 1/3/6 projectiles are
   emitted as 3DO nodes by tagpu_native.c, not into tagpu_fx.c's buckets.

   IT KNOWS NOTHING ABOUT A WINDOW. Everything arrives in TAGPU_VKPASS.
   A PASS READS NO ENGINE STATE: every value comes from tagpu_fx.c's
   hand-over, so this file is not on thread-split.allow and must never be. */

#include "tagpu_vk_pass.h"
#include "tagpu_vk_restore.h"
#include "tagpu_vk_stage.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_fx.h"
#include "tagpu_vk_unit.h"    /* tagpu_vk_unit_fx_count: the models' pass */
#include "tagpu_posedraw.h"   /* TAGPU_PD_MAXFX, the models' bound */
#include "tagpu_fx.h"
#include "tagpu_line.h"       /* tagpu_line_grid, the line test's uGrid */
#include "tagpu_gaf.h"                     /* tagpu_gaf_rects_due              */
#include "tagpu_pal.h"                     /* tagpu_pal_expand                 */
#include "spirv/tagpu_fx.spv.h"

#define VST    TAGPU_FX_VST                /* floats per vertex, the gather's  */
#define UBLK_VS 32                         /* std140 bytes, the generated header */
#define UBLK_FS 80                         /* LFS's: FS's 64, then uGrid */

/* ---- the entry points ----------------------------------------------------
   Resolved from the seam's `gdpa`/`gipa`, never linked, and this pass's own:
   the presentation table and a pass's barely overlap (tagpu_vk_pass.h). */
#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFormatProperties)

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
    X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

static int s_state;
static int s_downOwed;                     /* a teardown the seam still owes us */
static int s_downPaying;                   /* ...and the seam is paying it NOW  */
static int s_drawThis;                     /* `prepare` left a draw for `record` */
/* the frame whose effects models the posed pass may draw
   (`tagpu_vk_fx_models_ok`): set by `prepare` on the paths that draw, or
   that have nothing of their own to draw, and on no other */
static unsigned s_modelsFrame = 0xFFFFFFFFu;
static int s_saidModels;                   /* the posed pass's refusal, once   */
static int s_abFrame;
static int s_saidRestored;                 /* the Classic++ refusal, said once  */
static int s_saidNoMirror;                 /* ...and the mirror's, likewise     */
static int s_saidLht;                      /* ...the light table's              */

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipeTri, s_pipeLine, s_pipeFlash;
static VkDescriptorPool      s_dpool;
static VkSampler             s_samp;

static VkBuffer       s_ubuf;
static VkDeviceMemory s_umem;
static unsigned char* s_umap;
static VkDeviceSize   s_ustride;           /* two blocks, each device-aligned  */
static VkDeviceSize   s_ublock;

/* ---- THE SHARED IMAGES. One of each for every slot; the feature pass's
   header has the size argument, and these atlases are the same 2048 square. */
static int            s_atDim;             /* the square they were created for */

/* CLASSIC++'s RESTORED ATLAS -- tagpu_vk_feat.c carries the same image and
   states the reasoning. Binding 42 names this image; when it cannot be built
   the binding falls back to the base atlas's view, which keeps the descriptor
   valid, and `uRestored` 0 keeps the branch unreachable. */
static VkImage        s_arImg;
static VkDeviceMemory s_arMem;
static VkImageView    s_arView;
/* the restore job is its only writer */
static int            s_arHave;

/* THE BASE ATLAS, binding 45 -- tagpu_vk_feat.c carries the same image and
   states the reasoning. Its alpha is this pass's own: 0 at the key and
   255 - the flash level elsewhere (tagpu_fx.h TAGPU_FX_FLASH_ALPHA), from
   `s_flashAlpha`. */
static VkImage        s_bImg;
static VkDeviceMemory s_bMem;
static VkImageView    s_bView;
static int            s_bHave;
static unsigned       s_bSerial, s_bPal;
static unsigned char  s_flashAlpha[256];   /* TAGPU_FX_FLASH_ALPHA by index    */
/* ---- THE RESTORE THIS LANE RUNS FOR ITSELF --------------------------------
   `s_rjob` paints `s_arImg` from `s_bImg` when the producer publishes a frame
   list instead of a mirror. Three pieces of state make it a CURSOR rather than
   a serial, which is what a lazy queue needs:

     s_rjGen    the producer's generation this job belongs to. A new one is a
                discontinuity a cursor cannot survive -- a recycle, a repack --
                so the job is rebuilt and the cursor goes back to 0.
     s_rjPal    the engine palette serial the job was built with, a new job
                when it moves -- tagpu_vk_feat.c's `s_rjPal`.
     s_rjTaken  how many of that generation's frames are already queued here.
                The producer retains the whole array, so a frame on which this
                pass took nothing costs nothing: the next one takes more.
     s_rjSrcView  the view the job reads. The atlas image can be re-created
                under a live job (a dimension change), and a job holding the
                old view would paint from freed memory. The terrain consumer has
                the same hazard and the same guard. */
static TAGPU_VKRJOB*  s_rjob;
static unsigned       s_rjGen, s_rjPal;
static int            s_rjTaken;
static int            s_rjPainted;
static int            s_rjTried;           /* the device refused; do not ask again */
static VkImageView    s_rjSrcView;

/* what `record` was left to draw: the four buckets, in bucket order */
static int s_n[TAGPU_FXB_N];
static int s_scX, s_scY, s_scW, s_scH;     /* the scissor, in Vulkan framebuffer px */
/* The scissor's INPUTS, kept as numbers rather than as a copy of the hand-over.
   TAGPU_FXHAND is full of pointers into the gather's own frame memory, and a
   static holding those past the frame they were handed over on is a dangling
   read waiting for someone to add a line that follows one. */
static float s_hGw, s_hGh;
static int   s_hSs;                     /* the ss the line records were built at */
static int   s_hVpL, s_hVpT, s_hVw, s_hVh, s_hScissorOn;

typedef struct {
    VkBuffer        vbuf;                  /* the frame's vertices             */
    VkDeviceMemory  vmem;
    unsigned char*  vmap;
    VkDeviceSize    vcap;

    TAGPU_VKSTAGE   stage;                 /* the base atlas's upload, when due */

    VkImage         pal, lht, fog;         /* the three small per-slot images  */
    VkDeviceMemory  palMem, lhtMem, fogMem;
    VkImageView     palView, lhtView, fogView;
    VkBuffer        smallStage;
    VkDeviceMemory  smallMem;
    unsigned char*  smallMap;
    int             fogW, fogH;            /* what the fog image is sized for  */

    VkDescriptorSet dset;
    int             built;                 /* the fixed-size half is made      */
} SLOT;
static SLOT s_slot[TAGPU_VK_SLOTS];

/* the fog grid is RG8 and the widest the wide-fog builder produces is well
   inside this; a bound here is what keeps a handed-over number from sizing an
   allocation (the scaffold's rule), and tagpu_fx.c's own FOG_COPY_MAXDIM is
   the same number at the other end -- checked in both places on purpose. */
#define FOG_MAXDIM 1024
/* ONE STAGING BUFFER CARRIES ALL THREE SMALL UPLOADS: the palette, the flash
   light table and the fog grid. One allocation, three vkCmdCopyBufferToImage
   regions at three offsets -- and it is rebuilt with the fog image, by the same
   call, so the buffer and the image it feeds can never disagree about the
   size. */
#define SMALL_PALOFF 0                     /* 256 x RGBA8                      */
#define SMALL_LHTOFF (256 * 4)             /* 32 x RGBA8, expanded from RGB    */
#define SMALL_FIXED  (256 * 4 + 32 * 4)
#define SMALL_FOGOFF SMALL_FIXED           /* the grid, cols x rows RG8        */
/* Every one of those three is a multiple of 4 and of its own texel block size,
   which is what vkCmdCopyBufferToImage requires of a bufferOffset. */

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

/* THE MEMORY TYPE IS CHOSEN, NOT ASSUMED -- tagpu_vk_fps.c's reasoning. */
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
                     VkMemoryPropertyFlags want, VkBuffer* buf, VkDeviceMemory* mem,
                     unsigned char** map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    int type;
    void* p = NULL;

    bci.size = size;
    bci.usage = use;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(d->dev, *buf, &req);
    type = mem_type(d, req.memoryTypeBits, want);
    if (type < 0) { plog(d, "fx: no memory type for a buffer"); return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) return 0;
    if (map) {
        /* MAPPED ONCE AND LEFT MAPPED: the memory is HOST_COHERENT, so a write
           is visible to the device without a flush and a map per frame would be
           a driver call for an address that never moves. */
        if (vkMapMemory(d->dev, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
        *map = (unsigned char*)p;
    }
    return 1;
}

/* `usage` IS THE CALLER'S BECAUSE THE RESTORED ATLAS IS WRITTEN BY A SHADER.
   The base atlas is only ever copied into and sampled, but the restored
   one is what this lane's own restorer PAINTS, as a storage image, and a
   Vulkan image may only be one if it was created saying so. Nothing infers it
   from the format -- the destination's usage is a promise made at creation and
   tagpu_vk_restore.h asks for it by name. */
/* WHAT EACH OF THIS PASS'S IMAGES IS FOR. The restored atlas carries STORAGE
   because this lane's restorer paints into it; it costs nothing when nothing
   restores, and an image created without it could not be lent to the
   restorer at all. */
#define IMG_SAMPLED  (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
#define IMG_RESTORED (IMG_SAMPLED | VK_IMAGE_USAGE_STORAGE_BIT)

static int mk_image(const TAGPU_VKPASS* d, int w, int h, VkFormat fmt,
                    VkImageUsageFlags usage,
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
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, *img, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) { plog(d, "fx: no device-local memory for an image"); return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, *img, *mem, 0) != VK_SUCCESS) return 0;

    ivi.image = *img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, view) != VK_SUCCESS) return 0;
    return 1;
}

static void kill_image(const TAGPU_VKPASS* d, VkImage* img, VkDeviceMemory* mem,
                       VkImageView* view)
{
    if (*view) { vkDestroyImageView(d->dev, *view, NULL); *view = VK_NULL_HANDLE; }
    if (*img)  { vkDestroyImage(d->dev, *img, NULL);      *img  = VK_NULL_HANDLE; }
    if (*mem)  { vkFreeMemory(d->dev, *mem, NULL);        *mem  = VK_NULL_HANDLE; }
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

/* ---- barriers ----------------------------------------------------------- */

static void img_barrier(VkCommandBuffer cb, VkImage img,
                        VkImageLayout from, VkImageLayout to,
                        VkPipelineStageFlags srcStage, VkAccessFlags srcAcc,
                        VkPipelineStageFlags dstStage, VkAccessFlags dstAcc)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAcc;
    b.dstAccessMask = dstAcc;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* one buffer->image copy of a w x h rect at the image's origin */
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

/* ---- the slot ----------------------------------------------------------- */

static void slot_free(const TAGPU_VKPASS* d, SLOT* s)
{
    VkDevice dev = d->dev;
    if (s->vmap)  { vkUnmapMemory(dev, s->vmem); s->vmap = NULL; }
    if (s->vbuf)  { vkDestroyBuffer(dev, s->vbuf, NULL); s->vbuf = VK_NULL_HANDLE; }
    if (s->vmem)  { vkFreeMemory(dev, s->vmem, NULL); s->vmem = VK_NULL_HANDLE; }
    s->vcap = 0;
    tagpu_vk_stage_drop(d, &s->stage);
    if (s->smallMap) { vkUnmapMemory(dev, s->smallMem); s->smallMap = NULL; }
    if (s->smallStage) { vkDestroyBuffer(dev, s->smallStage, NULL); s->smallStage = VK_NULL_HANDLE; }
    if (s->smallMem) { vkFreeMemory(dev, s->smallMem, NULL); s->smallMem = VK_NULL_HANDLE; }
    kill_image(d, &s->pal, &s->palMem, &s->palView);
    kill_image(d, &s->lht, &s->lhtMem, &s->lhtView);
    kill_image(d, &s->fog, &s->fogMem, &s->fogView);
    s->fogW = s->fogH = 0;
    s->built = 0;
}

/* The fixed-size half of a slot: the palette and light-table images and the
   descriptor writes that never change again. */
static int slot_build(const TAGPU_VKPASS* d, SLOT* s)
{
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet wr[2];

    if (s->built) return 1;
    if (!mk_image(d, 256, 1, VK_FORMAT_R8G8B8A8_UNORM, IMG_SAMPLED, &s->pal, &s->palMem, &s->palView)) return 0;
    /* RGBA8 AND NOT RGB8 -- see item 3 of the file header. */
    if (!mk_image(d, 32, 1, VK_FORMAT_R8G8B8A8_UNORM, IMG_SAMPLED, &s->lht, &s->lhtMem, &s->lhtView)) return 0;

    memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
    ii[0].sampler = s_samp; ii[0].imageView = s->palView;
    ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].sampler = s_samp; ii[1].imageView = s->lhtView;
    ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = s->dset; wr[0].dstBinding = 40; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &ii[0];
    wr[1] = wr[0]; wr[1].dstBinding = 41; wr[1].pImageInfo = &ii[1];   /* uLht    */
    vkUpdateDescriptorSets(d->dev, 2, wr, 0, NULL);
    s->built = 1;
    return 1;
}

/* This slot's fog-grid image at `w` x `h`, rebuilt when the dimensions move --
   which they do whenever the view walks far enough for the grid to be re-laid.
   Free because the slot is ours: see the file header. */
static int slot_fog(const TAGPU_VKPASS* d, SLOT* s, int w, int h)
{
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    if (s->fogW == w && s->fogH == h && s->fog && s->smallStage) return 1;
    kill_image(d, &s->fog, &s->fogMem, &s->fogView);
    s->fogW = s->fogH = 0;
    if (!mk_image(d, w, h, VK_FORMAT_R8G8_UNORM, IMG_SAMPLED, &s->fog, &s->fogMem, &s->fogView)) return 0;
    /* the staging buffer goes with it: the fog grid's bytes are its tail, so
       its size follows the grid's and the two are made in one place */
    if (s->smallMap) { vkUnmapMemory(d->dev, s->smallMem); s->smallMap = NULL; }
    if (s->smallStage) { vkDestroyBuffer(d->dev, s->smallStage, NULL); s->smallStage = VK_NULL_HANDLE; }
    if (s->smallMem) { vkFreeMemory(d->dev, s->smallMem, NULL); s->smallMem = VK_NULL_HANDLE; }
    if (!mk_buffer(d, (VkDeviceSize)SMALL_FIXED + (VkDeviceSize)w * h * 2,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->smallStage, &s->smallMem, &s->smallMap)) return 0;
    memset(&ii, 0, sizeof ii); memset(&wr, 0, sizeof wr);
    ii.sampler = s_samp; ii.imageView = s->fogView;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = s->dset; wr.dstBinding = 43; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    s->fogW = w; s->fogH = h;
    return 1;
}

/* Room for `bytes` of vertices in this slot, grown by doubling and never
   shrunk while the pass is drawing. */
static int slot_verts(const TAGPU_VKPASS* d, SLOT* s, VkDeviceSize bytes)
{
    VkDeviceSize want = 65536;
    if (s->vcap >= bytes && s->vbuf) return 1;
    while (want < bytes) want *= 2;
    if (s->vmap) { vkUnmapMemory(d->dev, s->vmem); s->vmap = NULL; }
    if (s->vbuf) { vkDestroyBuffer(d->dev, s->vbuf, NULL); s->vbuf = VK_NULL_HANDLE; }
    if (s->vmem) { vkFreeMemory(d->dev, s->vmem, NULL); s->vmem = VK_NULL_HANDLE; }
    s->vcap = 0;
    if (!mk_buffer(d, want, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->vbuf, &s->vmem, &s->vmap)) return 0;
    s->vcap = want;
    return 1;
}

/* ---- building ----------------------------------------------------------- */

static int build_sampler(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties fp;
    int i;
    /* the two formats this pass samples, asked for rather than assumed --
       and R8G8B8_UNORM is deliberately NOT among them: the light table is
       expanded to RGBA8 on the way in (item 3 of the file header). R8 is the
       placeholder scaffold's, which names the base atlas instead. */
    static const VkFormat need[2] = { VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    for (i = 0; i < 2; i++) {
        vkGetPhysicalDeviceFormatProperties(d->pd, need[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            plog(d, "fx: this device cannot sample format %d - the pass stays down",
                 (int)need[i]);
            return 0;
        }
    }
    /* NEAREST AND CLAMP_TO_EDGE. The base atlas is sampled as the engine
       blits a sprite, one texel per pixel -- and its alpha is a hole flag and
       a light level, which interpolating two texels would turn into a number
       that means neither; the palette, the light table and the fog corner
       masks are all fetched by index. */
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d->dev, &sci, NULL, &s_samp) != VK_SUCCESS) return 0;
    return 1;
}

static int build_pipelines(const TAGPU_VKPASS* d)
{
    static const struct { int loc, n, off; } at[TAGPU_FX_NATTR] = TAGPU_FX_ATTRS;
    VkDescriptorSetLayoutBinding b[8];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[TAGPU_FX_NATTR];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
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
    VkShaderModule lvs = VK_NULL_HANDLE, lfs = VK_NULL_HANDLE;
    VkResult r;
    int i, ok = 0;

    /* THE BINDINGS ARE THE GENERATOR'S, NOT THIS FILE'S: tools/spirv-gen.py
       allocates them BY STAGE, so set 0 binding 0 is the vertex stage's uniform
       block, 32 the fragment stage's, and 40.. its samplers in declaration
       order -- uPal, uLht, uAtlasRGB, uFogGrid, uScaf, uBase. The std140
       offsets are printed in the generated header and are the contract for
       what goes into those buffers. */
    memset(b, 0, sizeof b);
    b[0].binding = 0;  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 32; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 2; i < 8; i++) {
        b[i].binding = (uint32_t)(40 + (i - 2));
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    dli.bindingCount = 8; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS) return 0;

    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) return 0;

    vs = mk_module(d, tagpu_spv_tagpu_fx_VS,
                   sizeof tagpu_spv_tagpu_fx_VS / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_tagpu_fx_FS,
                   sizeof tagpu_spv_tagpu_fx_FS / sizeof(uint32_t));
    lvs = mk_module(d, tagpu_spv_tagpu_fx_LVS,
                    sizeof tagpu_spv_tagpu_fx_LVS / sizeof(uint32_t));
    lfs = mk_module(d, tagpu_spv_tagpu_fx_LFS,
                    sizeof tagpu_spv_tagpu_fx_LFS / sizeof(uint32_t));
    if (!vs || !fs || !lvs || !lfs) { plog(d, "fx: a shader module was refused"); goto out; }

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    /* ONE TABLE: tagpu_fx.h's TAGPU_FX_ATTRS sits beside TAGPU_FX_VST, the
       stride the gather writes its vertices at, so a layout change is one
       edit in one header rather than a gather and a pass that disagree. */
    memset(&vb, 0, sizeof vb);
    vb.binding = 0; vb.stride = VST * sizeof(float);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(va, 0, sizeof va);
    for (i = 0; i < TAGPU_FX_NATTR; i++) {
        static const VkFormat f[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                       VK_FORMAT_R32G32B32_SFLOAT,
                                       VK_FORMAT_R32G32B32A32_SFLOAT };
        va[i].location = (uint32_t)at[i].loc;
        va[i].binding = 0;
        va[i].format = f[at[i].n - 1];
        va[i].offset = (uint32_t)at[i].off;
    }
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = TAGPU_FX_NATTR;
    vi.pVertexAttributeDescriptions = va;

    vp.viewportCount = 1; vp.scissorCount = 1;     /* both dynamic, set per frame */

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* NO CULLING. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;          /* no pipeline here rasterises a line */

    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* TESTED, NEVER WRITTEN, for all three pipelines (item 4 of the file
       header). Effects occlude nothing and are occluded by everything drawn
       before them. */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    memset(&cba, 0, sizeof cba);
    /* ONE / ONE_MINUS_SRC_ALPHA, the premultiplied blend, for the alpha
       factors as well as the colour ones. */
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
    gp.layout = s_plo;
    gp.renderPass = d->rp;
    gp.subpass = 0;

    /* A LIST: the gather emits every sprite as six vertices. */
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeTri);
    if (r != VK_SUCCESS) { plog(d, "fx: the triangle pipeline was refused (%d)", (int)r); goto out; }

    /* THE LASERS AND THE LIGHTNING -- item 2 of the file header. The same
       vertex layout read once per INSTANCE, so each line record is one band
       of six vertices (`record` draws 6 x count), with the LVS/LFS program
       and the triangle pipeline's blend. */
    vb.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    st[0].module = lvs; st[1].module = lfs;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeLine);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    st[0].module = vs; st[1].module = fs;
    if (r != VK_SUCCESS) { plog(d, "fx: the line pipeline was refused (%d)", (int)r); goto out; }

    /* THE FLASHES ARE PURE ADDITIVE LIGHT: ONE / ONE, and the shader gives
       them alpha 0 so the target keeps its own. */
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeFlash);
    if (r != VK_SUCCESS) { plog(d, "fx: the flash pipeline was refused (%d)", (int)r); goto out; }
    ok = 1;

out:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    if (lvs) vkDestroyShaderModule(d->dev, lvs, NULL);
    if (lfs) vkDestroyShaderModule(d->dev, lfs, NULL);
    return ok;
}

static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSet sets[TAGPU_VK_SLOTS];
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    uint32_t i;

    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         ps[0].descriptorCount = d->slots * 2;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[1].descriptorCount = d->slots * 6;
    dpi.maxSets = d->slots;
    dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) return 0;

    for (i = 0; i < d->slots; i++) lay[i] = s_dsl;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = d->slots;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, sets) != VK_SUCCESS) return 0;

    /* The two uniform blocks are written now and never again -- they name a
       fixed range of a buffer whose CONTENTS change per frame. The samplers are
       written when there is an image to name: 42, 44 and 45 by
       `atlas_build`, 40 and 41 by `slot_build`, 43 by `slot_fog`. */
    for (i = 0; i < d->slots; i++) {
        VkDescriptorBufferInfo bi[2];
        VkWriteDescriptorSet w[2];
        s_slot[i].dset = sets[i];
        memset(bi, 0, sizeof bi); memset(w, 0, sizeof w);
        bi[0].buffer = s_ubuf; bi[0].offset = i * s_ustride;            bi[0].range = UBLK_VS;
        bi[1].buffer = s_ubuf; bi[1].offset = i * s_ustride + s_ublock; bi[1].range = UBLK_FS;
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = sets[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bi[0];
        w[1] = w[0]; w[1].dstBinding = 32; w[1].pBufferInfo = &bi[1];
        vkUpdateDescriptorSets(d->dev, 2, w, 0, NULL);
    }
    return 1;
}

/* The shared atlas images, and the three descriptor bindings that name them. */
static int atlas_build(const TAGPU_VKPASS* d, int dim)
{
    uint32_t i;
    int k;
    if (s_bImg && s_atDim == dim) return 1;
    kill_image(d, &s_bImg, &s_bMem, &s_bView);
    s_atDim = 0;
    s_bHave = 0; s_bSerial = 0; s_bPal = 0;
    tagpu_fx_atlas_ack(0, 0);              /* this copy holds nothing yet */
    for (k = 0; k < 256; k++) s_flashAlpha[k] = (unsigned char)TAGPU_FX_FLASH_ALPHA(k);
    /* the base atlas is what the effects are drawn from and what the
       restorer reads, so without it the pass does not draw */
    if (!mk_image(d, dim, dim, VK_FORMAT_R8G8B8A8_UNORM, IMG_SAMPLED,
                  &s_bImg, &s_bMem, &s_bView)) {
        kill_image(d, &s_bImg, &s_bMem, &s_bView);
        plog(d, "fx: no %d MB device image for the base atlas", (dim * dim * 4) >> 20);
        return 0;
    }
    s_atDim = dim;
    kill_image(d, &s_arImg, &s_arMem, &s_arView);
    s_arHave = 0;
    if (!mk_image(d, dim, dim, VK_FORMAT_R8G8B8A8_UNORM, IMG_RESTORED,
                  &s_arImg, &s_arMem, &s_arView)) {
        /* `mk_image` CAN FAIL AFTER vkCreateImage AND vkAllocateMemory SUCCEEDED
           -- no device-local memory type, a failed bind, a failed view. Nulling
           the handles here would lose the image while leaving `s_arMem` set, so
           the next kill_image would vkFreeMemory memory that still has an image
           bound to it. kill_image is what tagpu_vk_terr.c's shared_resize does
           for the identical case. */
        kill_image(d, &s_arImg, &s_arMem, &s_arView);
        plog(d, "fx: no %d MB device image for the Classic++ restored twin - the "
                "effects draw the base atlas", (dim * dim * 4) >> 20);
    }
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        VkDescriptorImageInfo ib, irgb;
        VkWriteDescriptorSet wr[3];
        if (!s_slot[i].dset) continue;
        memset(&ib, 0, sizeof ib); memset(wr, 0, sizeof wr);
        /* 45 is uBase */
        ib.sampler = s_samp; ib.imageView = s_bView;
        ib.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = s_slot[i].dset; wr[0].dstBinding = 45; wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[0].pImageInfo = &ib;
        /* BINDING 42 IS uAtlasRGB AND STARTS ON THE BASE ATLAS'S VIEW;
           BINDING 44 IS uScaf AND IS THE BASE ATLAS'S VIEW AS A PLACEHOLDER.
           42 is the restored twin's on the frames the twin is a picture --
           `twin_bind` points it there per slot -- and the base's until then,
           because the twin was just made and is UNDEFINED; `uRestored` 0 keeps
           its branch unreachable on those frames. `uScafOn` is 0 on every draw
           (see the file header), so nothing ever samples 44. A placeholder is
           legitimate only where nothing samples it -- a descriptor must be
           VALID for the set to bind, and naming the image already here costs
           no memory and no second object. The scaffold's image being shared is
           what would make 44 stop being a placeholder. */
        irgb = ib;
        wr[1] = wr[0]; wr[1].dstBinding = 42; wr[1].pImageInfo = &irgb;
        wr[2] = wr[0]; wr[2].dstBinding = 44;
        vkUpdateDescriptorSets(d->dev, 3, wr, 0, NULL);
    }
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties props;
    VkDeviceSize ualign;

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "fx: %u frame slots is outside what this pass carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    if (d->dfmt == VK_FORMAT_UNDEFINED) {
        plog(d, "fx: the seam's render pass carries no depth attachment, and "
                "this pass tests depth - it stays down rather than drawing "
                "effects that nothing occludes");
        return 0;
    }
    if (!resolve(d)) { plog(d, "fx: an entry point is missing"); return 0; }

    vkGetPhysicalDeviceProperties(d->pd, &props);
    /* TWO BLOCKS PER SLOT, EACH AT AN OFFSET THE DEVICE ACCEPTS.
       `minUniformBufferOffsetAlignment` is 16 on some devices and 256 on
       others, and a bound buffer offset that is not a multiple of it is
       undefined behaviour rather than a slow path. The larger block is the
       fragment stage's, so each block takes that size ROUNDED UP to the
       alignment -- 80 bytes is a multiple of 16 but not of 64 -- and the
       stride is two of them. */
    ualign = props.limits.minUniformBufferOffsetAlignment;
    if (ualign < 1) ualign = 1;
    s_ublock = ((UBLK_FS + ualign - 1) / ualign) * ualign;
    s_ustride = s_ublock * 2;

    if (!mk_buffer(d, s_ustride * d->slots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_ubuf, &s_umem, &s_umap)) return 0;
    if (!build_sampler(d)) return 0;
    if (!build_pipelines(d)) return 0;
    if (!build_descriptors(d)) return 0;

    plog(d, "fx: the Vulkan edition is up - %u frame slots, uniform stride %u, "
            "three pipelines (tri, line, flash)",
         (unsigned)d->slots, (unsigned)s_ustride);
    return 1;
}

/* ---- the frame ---------------------------------------------------------- */

/* THE RESTORE, FED FROM THE PUBLISHED LIST.

   Called once a frame after the base atlas's upload, because the FILL pass
   reads the base and a restore issued before this frame's copy would paint
   undefined texels over whatever the base had not received yet.

   IT IS A CURSOR, AND THE GENERATION IS THE ONLY THING THAT RESTARTS IT. A
   effects atlas is a lazy queue: the producer appends one frame per miss
   for the life of the atlas, so the steady state here is "add the few frames
   past my cursor and advance it". Everything that could make the cursor a lie
   -- the rects moving, the destination blanking -- arrives as a new
   generation, and then the job is rebuilt from index 0. */
static void restore_want(const TAGPU_VKPASS* d, const TAGPU_FXHAND* h)
{
    int n;

    if (!h->restoreFrames || h->restoreGen == 0) {
        /* No request: the lever was never on, or the producer's list was
           dropped. Either way this job describes nothing now. */
        if (s_rjob) {
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
            s_rjSrcView = VK_NULL_HANDLE;
            /* AND WHAT IT PAINTED IS NO LONGER A PICTURE. The request going
               away while a job existed means the producer's list DIED -- its
               only such path is the out-of-memory drop -- and nothing will
               feed this image again. Leaving `s_arHave` set would have this
               pass draw a frozen atlas while the base one it was painted
               from moves on, which is a silent divergence rather than a
               stand-down. Only inside the `if`: with the lever off there is
               no job and this branch runs every frame. */
            s_arHave = 0;
        }
        return;
    }
    /* THE SOURCE MOVED UNDER A LIVE JOB. `base_upload` refuses a dimension
       change and `atlas_build` is what re-creates the image, so this is the
       narrow case rather than the common one -- and it is the one that reads
       from a destroyed view if nothing checks. */
    if (s_rjob && s_rjSrcView && s_bView && s_rjSrcView != s_bView) {
        plog(d, "fx: the base atlas moved under a live restore - dropping it "
                "and starting over on the new one");
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
        s_rjSrcView = VK_NULL_HANDLE;
        s_arHave = 0;                      /* what it holds is the old layout's */
    }
    if (s_rjob && s_rjGen == h->restoreGen && s_rjPal == h->palSerial) {
        int painted = tagpu_vk_restore_job_painted(s_rjob);
        /* ONE PAINTED FRAME IS WHAT MAKES THIS A PICTURE, and `s_arHave` is
           what the pass's restored refusal reads. */
        if (painted > 0) s_arHave = 1;
        if (painted != s_rjPainted) {
            s_rjPainted = painted;
            if (tagpu_vk_restore_job_idle(s_rjob))
                plog(d, "fx: restored atlas painted here - %d frames of "
                        "generation %u, no mirror and no read-back",
                     painted, h->restoreGen);
        }
        if (tagpu_vk_restore_job_failed(s_rjob)) {
            /* "WHAT IT PAINTED STANDS" IS ONLY TRUE UNTIL THE RECTS MOVE, and
               they will: the next generation re-lays the atlas and this image
               then holds the OLD layout's texels at every new rect. So the
               image stops being a picture at the moment the restore is
               abandoned, not at the moment it looks wrong. */
            plog(d, "fx: the restore failed on this lane - the twin stops being "
                    "drawn from, because the next atlas layout would sample it "
                    "at rects it was never painted for");
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0;
            s_rjTried = 1;                 /* it will fail the same way again  */
            s_arHave = 0;
            return;
        }
        /* THE STEADY STATE: whatever the producer has appended since. */
        n = h->restoreN - s_rjTaken;
        if (n > 0) {
            int took = tagpu_vk_restore_job_add(s_rjob, h->restoreFrames + s_rjTaken, n);
            /* THE CURSOR ADVANCES BY WHAT WAS OFFERED, NOT BY WHAT WAS TAKEN.
               `tagpu_rcore_job_add` SKIPS a frame it can never queue -- a
               degenerate rect, or one larger than the activation slot even
               after the wrap demote -- and returns only the count it queued,
               so advancing by that re-offers the tail of the list on every
               frame for the life of the atlas: one duplicate restore per
               refused frame, for ever. A refused frame draws the base atlas until the
               next generation, which is what the log line says.
               THE EXCEPTION IS A WHOLE-CALL FAILURE (`took` 0 with frames
               offered): that is the queue's own realloc failing, which is
               transient and already logged by the core, so the cursor stays
               where it is and the next frame offers them again. */
            if (took > 0) {
                s_rjTaken += n;
                if (took < n)
                    plog(d, "fx: %d of %d new restore frames were refused by the "
                            "restorer (degenerate or larger than a slot) - they stay "
                            "on the base atlas until the next generation", n - took, n);
            }
        }
        return;
    }
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; s_rjTaken = 0; }
    /* PAST THIS POINT NO JOB PAINTS THIS GENERATION, so the image is not a
       picture of it: what it holds was painted for the one before, at rects
       that have moved or in the old table's colours. Cleared here, before any
       of the returns below -- a restore given up on, a device that will not
       run one, a job that cannot be made -- so none of them leaves the flag
       naming the old picture. */
    s_arHave = 0;
    if (s_rjTried) return;
    /* BOTH SURFACES HAVE TO BE THERE, and the source has to have contents: a
       FILL over a base no copy has reached yet would paint undefined texels
       over the art. Neither is an error -- the next frame asks again. */
    if (!s_arImg || !s_arView || !s_bView || !s_bHave) return;
    /* THE CONSUMER IS WHAT ASKS THE DEVICE: `up` loads the model off disk, so
       a session that never publishes a request never pays for it. It latches
       its verdict, so this is one integer compare per frame after the first. */
    if (!tagpu_vk_restore_up(d)) { s_rjTried = 1; return; }
    /* EVERY GENERATION BLANKS, for tagpu_vk_feat.c's reason: nothing the
       restore reads moves in play. */
    s_rjob = tagpu_vk_restore_job_new(d, "fx", 2, 0, 0,
                                      s_bImg, s_bView, s_atDim, s_atDim, 1,
                                      h->pal,
                                      s_arImg, s_arView, s_atDim, s_atDim);
    if (!s_rjob) { s_rjTried = 1; return; }   /* the reason is in the log      */
    s_rjTaken = tagpu_vk_restore_job_add(s_rjob, h->restoreFrames, h->restoreN);
    s_rjGen = h->restoreGen;
    s_rjPal = h->palSerial;
    s_rjSrcView = s_bView;
    s_rjPainted = 0;
    plog(d, "fx: restoring the atlas HERE - %d of %d frames over %dx%d, "
            "generation %u", s_rjTaken, h->restoreN, s_atDim, s_atDim,
         h->restoreGen);
}

/* A REFUSED PASS GIVES ITS RESTORE JOB BACK AT ONCE -- tagpu_vk_feat.c's
   `refuse_job`, which has the argument: a banded upload the device failed
   partway leaves the base atlas half written in a transfer layout, and the
   job's FILL would sample it. */
static void refuse_job(const TAGPU_VKPASS* d)
{
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
    s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
    s_rjSrcView = VK_NULL_HANDLE;
    s_arHave = 0;
}

/* THE BASE ATLAS -- tagpu_vk_feat.c's `base_upload`, the same two ways, the
   same answers and the same acknowledgement, with this pass's own alpha
   (`s_flashAlpha`). This atlas never repacks, so there is no move. */
static int s_rect[TAGPU_VK_STAGE_MAXRECT][4];      /* render thread only */
static unsigned s_upFrame;                         /* the frame base_upload last ran in */
static unsigned s_saidSkip;                        /* the skip's line, once a 300 frames */
static int base_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, SLOT* s,
                       const TAGPU_FXHAND* h)
{
    int rows = h->atlasRows, n, rc, keep;
    const char* why = NULL;
    VkDeviceSize bytes;

    s_upFrame = d->frame;
    /* THE DIMENSIONS ARE FIXED AT `atlas_build`, and the bindings naming the
       images touch every slot's set -- tagpu_vk_feat.c's `base_upload` has
       the argument. tagpu_fx.c's ATLAS_DIM is a compile-time constant, so
       this cannot fire; it is the guard that keeps that true. */
    if (s_atDim != h->atlasDim) {
        plog(d, "fx: the atlas changed from %d texels square to %d - the pass "
                "comes down rather than rewrite descriptor sets that frames in "
                "flight are using", s_atDim, h->atlasDim);
        return -1;
    }
    if (s_bHave && s_bSerial == h->atlasSerial && s_bPal == h->palSerial) {
        tagpu_vk_stage_drop(d, &s->stage);
        tagpu_fx_atlas_ack(s_bSerial, 1);
        return 1;
    }
    if (rows < 1) rows = 1;
    if (rows > h->atlasDim) rows = h->atlasDim;
    keep = s_bHave && s_bPal == h->palSerial && (int)(h->atlasWhole - s_bSerial) <= 0;
    if (!keep)
        why = !s_bHave ? "the first upload since the image was made"
            : s_bPal != h->palSerial ? "the engine's table moved"
            : "the atlas is new (made, lost, a new level, its mirror armed)";
    n = tagpu_gaf_rects_due(h->atlasDirty, h->atlasSerial, s_bSerial, keep,
                            h->atlasDim, rows, s_rect, TAGPU_VK_STAGE_MAXRECT);
    bytes = (VkDeviceSize)tagpu_gaf_rects_area((const int (*)[4])s_rect, n) * 4;
    if (keep) {
        if (n > 0 && (!tagpu_vk_stage_begin(d, &s->stage, bytes, (VkDeviceSize)h->atlasDim * 4) ||
                      !tagpu_vk_stage_fits(&s->stage, bytes))) {
            if (!s_saidSkip || d->frame - s_saidSkip >= 300u) {
                s_saidSkip = d->frame ? d->frame : 1u;
                plog(d, "fx: %u KB of tiles due and no staging that holds them - the "
                        "frame is skipped and they go on the next; nothing waited on",
                     (unsigned)(bytes >> 10));
            }
            return 0;
        }
        if (n > 0 &&
            !tagpu_vk_stage_expand_rects(d, cb, &s->stage, s_bImg, s_bHave, h->atlas, h->atlasKey,
                                         h->atlasDim, (const int (*)[4])s_rect, n, h->pal,
                                         s_flashAlpha))
            return 0;
        if (n == 0) tagpu_vk_stage_drop(d, &s->stage);
    } else {
        plog(d, "fx: the whole base atlas, %u KB, through the banded path - %s",
             (unsigned)(bytes >> 10), why);
        if (!tagpu_vk_stage_begin(d, &s->stage, bytes, (VkDeviceSize)h->atlasDim * 4))
            return 0;
        rc = tagpu_vk_stage_expand(d, cb, &s->stage, s_bImg, s_bHave, h->atlas, h->atlasKey,
                                   h->atlasDim, 0, 0, h->atlasDim, rows, h->pal, s_flashAlpha);
        if (rc <= 0) return rc;
    }
    s_bSerial = h->atlasSerial; s_bPal = h->palSerial;
    s_bHave = 1;
    tagpu_fx_atlas_ack(s_bSerial, 1);
    return 1;
}

/* The world viewport, in Vulkan framebuffer pixels, and NOT mirrored.
   tagpu_vk_feat.c item 3 is the argument; this is the same
   arithmetic. */
static void fx_scissor(uint32_t w, uint32_t h)
{
    float sx = s_hGw > 0.0f ? (float)w / s_hGw : 1.0f;
    float sy = s_hGh > 0.0f ? (float)h / s_hGh : 1.0f;
    int x0 = (int)(s_hVpL * sx + 0.5f);
    int ww = (int)(s_hVw * sx + 0.5f);
    int ytop = (int)(s_hVpT * sy + 0.5f);
    int hh = (int)(s_hVh * sy + 0.5f);
    int y0 = ytop;                          /* NOT mirrored */

    /* NO CLIP WHERE THE NATIVE PASS SET NONE. `scissorOn` is what the native pass
       actually did, not what it would have liked to. */
    if (!s_hScissorOn || ww <= 0 || hh <= 0) {
        s_scX = 0; s_scY = 0; s_scW = (int)w; s_scH = (int)h;
        return;
    }
    if (x0 < 0) { ww += x0; x0 = 0; }
    if (y0 < 0) { hh += y0; y0 = 0; }
    if (x0 > (int)w) x0 = (int)w;
    if (y0 > (int)h) y0 = (int)h;
    if (ww < 0) ww = 0;
    if (hh < 0) hh = 0;
    if (x0 + ww > (int)w) ww = (int)w - x0;
    if (y0 + hh > (int)h) hh = (int)h - y0;
    s_scX = x0; s_scY = y0; s_scW = ww; s_scH = hh;
}

/* BINDING 42 NAMES THE TWIN ONLY WHILE IT IS A PICTURE -- tagpu_vk_feat.c
   `twin_bind`, which has the argument: the twin is made UNDEFINED and only
   the job's OUT leaves it SHADER_READ_ONLY, `s_arHave` is exactly "it has",
   and the flag is decided from the same `s_arHave`. Per slot, on every
   prepare that draws, after the flag. */
static void twin_bind(const TAGPU_VKPASS* d, const SLOT* s)
{
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    memset(&ii, 0, sizeof ii); memset(&wr, 0, sizeof wr);
    ii.sampler = s_samp;
    ii.imageView = (s_arView && s_arHave) ? s_arView : s_bView;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = s->dset; wr.dstBinding = 42; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
}

static int prepare_draw(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_FXHAND h;
    SLOT* s;
    VkDeviceSize vbytes;
    /* A UNION, NOT A CAST. Both blocks mix `int` and `float` members and
       writing an int through a float array is the aliasing rule broken at -O2,
       which is not a place to find out that the fog branch took a garbage
       uFog. */
    union { float f[UBLK_FS / 4]; int i[UBLK_FS / 4]; } ub;
    int fogW = 1, fogH = 1;
    int b, total = 0;
    size_t off;

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW, AND NOTHING IS KEPT
       ONCE THERE IS NOT -- §2.28's rule. Giving slot `slot` back at this point
       needs no new argument and no timer, because it is the same instant, and
       the same ownership, that the rest of this function writes it in. */
    if (!tagpu_fx_handover(&h, d->frame)) {
        if (s_state == ST_READY) slot_free(d, &s_slot[slot]);
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_fx_down(d); s_state = ST_REFUSED; return 0; }
        /* the shared atlas images and the descriptor bindings that name
           them, before any set has been bound -- see `base_upload` */
        if (h.atlasDim < 1 || h.atlasDim > 8192 || !atlas_build(d, h.atlasDim)) {
            plog(d, "fx: a %d-texel-square atlas image was refused", h.atlasDim);
            tagpu_vk_fx_down(d); s_state = ST_REFUSED; return 0;
        }
        s_state = ST_READY;
    }

    /* THE BOUNDS, RE-CHECKED. Every one of these sizes an allocation or a
       memcpy, and a bound that lives in the file that produced the number is a
       bound only while both files are read together. */
    if (!h.atlas) {
        /* SAID ONCE. This is the atlas's CPU mirror -- its own indices, the
           source the base atlas is expanded from -- and it cannot be had before the atlas
           has its dimensions, so the first frames of a session legitimately
           arrive without one. It is NOT the restored atlas, so the message does
           not blame it: this is a perfectly healthy run. */
        if (!s_saidNoMirror) {
            s_saidNoMirror = 1;
            plog(d, "fx: the atlas has no CPU mirror yet - nothing "
                    "drawn until the producer arms one");
        }
        return 0;
    }
    s_saidNoMirror = 0;
    if (!h.pal) return 0;
    for (b = 0; b < TAGPU_FXB_N; b++) {
        /* BOUNDED AGAINST THE PRODUCER'S OWN CAP, not merely against negatives.
           `total` sizes the vertex allocation and the memcpy that fills it, and
           this file's rule -- the one every pass here repeats -- is that a bound
           living in the file that produced the number is a bound only while both
           files are read together. TAGPU_FX_MAXV_OF is the gather's own cap,
           shared through tagpu_fx.h so the two cannot drift. */
        if (h.n[b] < 0 || h.n[b] > TAGPU_FX_MAXV_OF(b)) {
            plog(d, "fx: bucket %d reports %d vertices, outside 0..%d - nothing drawn",
                 b, h.n[b], TAGPU_FX_MAXV_OF(b));
            return 0;
        }
        total += h.n[b];
    }

    /* THE FRAME'S MODELS ARE THE POSED PASS'S, AND AN EFFECT IS DRAWN WHOLE OR
       NOT AT ALL (tagpu_fx.h `nmodels`). That pass prepared before this one,
       so what it will draw of them is already known: anything but all of them
       and this pass draws nothing either -- and says nothing to the posed
       pass, which then draws none (`tagpu_vk_fx_models_ok`). The count is
       only compared, but it is bounded like every number that crosses. */
    if (h.nmodels < 0 || h.nmodels > TAGPU_PD_MAXFX) {
        plog(d, "fx: the hand-over reports %d models, outside 0..%d - nothing drawn",
             h.nmodels, TAGPU_PD_MAXFX);
        return 0;
    }
    if (h.nmodels > 0 && tagpu_vk_unit_fx_count(d->frame) != h.nmodels) {
        if (!s_saidModels) {
            s_saidModels = 1;
            plog(d, "fx: the posed pass is not drawing this frame's %d effects "
                    "models (%d) - no effect is drawn on such a frame",
                 h.nmodels, tagpu_vk_unit_fx_count(d->frame));
        }
        return 0;
    }
    s_saidModels = 0;
    /* nothing of this pass's own to draw, and nothing against the models:
       they are drawn */
    if (total < 1) { s_modelsFrame = d->frame; return 0; }

    /* A FLASH WITHOUT ITS LIGHT TABLE has nothing to be lit from -- the
       producer has never built the table -- so the pass stands down rather
       than guess. */
    if (h.n[TAGPU_FXB_FLASH] > 0 && !h.lht) {
        if (!s_saidLht) {
            s_saidLht = 1;
            plog(d, "fx: there are flash vertices and the light table has never "
                    "been built - nothing drawn this frame");
        }
        return 0;
    }

    if (h.fogGrid) {
        if (h.fogGridCols < 1 || h.fogGridRows < 1 ||
            h.fogGridCols > FOG_MAXDIM || h.fogGridRows > FOG_MAXDIM) {
            plog(d, "fx: a %dx%d fog grid is outside what this pass carries - nothing drawn",
                 h.fogGridCols, h.fogGridRows);
            return 0;
        }
        fogW = h.fogGridCols; fogH = h.fogGridRows;
    }

    s = &s_slot[slot];
    /* THIS SLOT IS OURS -- the seam waited on fence[slot] at the top of this
       frame, so the submit that last used these buffers, these images and this
       descriptor set has completed. That is what makes every write below safe
       with no device-wide wait. */
    if (!slot_build(d, s)) goto refuse;
    if (!slot_fog(d, s, fogW, fogH)) goto refuse;

    vbytes = (VkDeviceSize)total * VST * sizeof(float);
    if (!slot_verts(d, s, vbytes)) goto refuse;
    /* the four buckets, contiguous and in bucket order -- which is also the
       order `record` draws them in, walking `first` across them */
    off = 0;
    for (b = 0; b < TAGPU_FXB_N; b++) {
        if (!h.n[b]) continue;
        memcpy(s->vmap + off, h.vert[b], (size_t)h.n[b] * VST * sizeof(float));
        off += (size_t)h.n[b] * VST * sizeof(float);
    }

    /* A FRAME WITH NO STAGING IS SKIPPED, NOT REFUSED: the atlas this frame
       samples has not reached the device, and the next frame sends it. */
    switch (base_upload(d, cb, s, &h)) {
    case -1: goto refuse;
    case 0:  return 0;
    default: break;
    }

    /* THE RESTORE, AFTER THE UPLOAD IT READS AND BEFORE ANY DRAW THAT SAMPLES
       WHAT IT PAINTS. The draws themselves are issued by the seam, from
       `tagpu_vk_restore_step`, after every pass's prepare -- this call only
       creates and feeds the job. */
    restore_want(d, &h);

    /* CAN THIS FRAME DRAW RESTORED? DECIDED HERE, AFTER `restore_want`, for
       tagpu_vk_feat.c's reason: this atlas recycles in play, every recycle is
       a new generation, and `restore_want` replaces the job on it. A flag
       decided before that would have the recycle's frame draw the new
       sprites through the OLD sprites' restored texels at their recycled
       rects. Read here, it is the state this frame's draw samples: a frame
       LIST published, the image there, and `s_arHave` -- cleared by every
       path that disowns the twin -- saying a frame of this generation is
       painted. `twin_bind` below names binding 42 from the same `s_arHave`.

       A FRAME WITHOUT ONE IS DRAWN, from the base atlas, not returned from:
       the restore needs this frame's base atlas uploaded before it can paint
       anything, so returning would be a deadlock -- nothing drawn because
       nothing painted, nothing painted because the atlas never arrived. NOT
       LATCHED: the condition clears by itself a few frames after the
       restorer starts, and gpu-status 2.35 measured what latching such a
       condition costs. */
    if (h.restored && !(s_arImg && h.restoreFrames && s_arHave)) {
        if (!s_saidRestored) {
            s_saidRestored = 1;
            plog(d, "fx: a Classic++ restore is armed and this lane has no "
                    "restored twin of it yet - %s", s_rjob
                        ? "its job is painting one; the base atlas is drawn until it has"
                        : "drawing the base atlas until one is painted");
        }
        h.restored = 0;
    } else s_saidRestored = 0;
    twin_bind(d, s);

    /* THE THREE SMALL IMAGES, per slot, so the one-line invariant covers them:
       UNDEFINED in, because the whole of each is re-sent every frame and there
       are therefore no contents to preserve and no layout to carry. */
    memcpy(s->smallMap + SMALL_PALOFF, h.pal, 256 * 4);
    /* THE LIGHT TABLE, RGB -> RGBA (item 3 of the file header). The alpha is
       never sampled -- the shader reads `.rgb` -- so it is set to 255 rather
       than left as whatever the buffer held, because a staging buffer that is
       deterministic is one less thing to wonder about when a picture is wrong. */
    {
        int L;
        unsigned char* o = s->smallMap + SMALL_LHTOFF;
        if (h.lht) {
            for (L = 0; L < 32; L++) {
                o[L*4+0] = h.lht[L*3+0];
                o[L*4+1] = h.lht[L*3+1];
                o[L*4+2] = h.lht[L*3+2];
                o[L*4+3] = 255;
            }
        } else {
            /* no flash vertices this frame (the refusal above proved it), so
               nothing samples this image; send a defined zero anyway */
            memset(o, 0, 32 * 4);
        }
    }
    /* The grid is one `unsigned short` a cell and the image is RG8: the same
       two bytes in the same order.
       With no grid this frame the image is one zero cell, which the shader
       never reads -- taFog is called only on the `uFog & 1` branch. */
    if (h.fogGrid) memcpy(s->smallMap + SMALL_FOGOFF, h.fogGrid,
                          (size_t)fogW * fogH * 2);
    else           memset(s->smallMap + SMALL_FOGOFF, 0, 2);

    img_barrier(cb, s->pal, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->lht, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->fog, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->smallStage, SMALL_PALOFF, s->pal, 256, 1);
    copy_rect(cb, s->smallStage, SMALL_LHTOFF, s->lht, 32, 1);
    copy_rect(cb, s->smallStage, SMALL_FOGOFF, s->fog, fogW, fogH);
    img_barrier(cb, s->pal, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->lht, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->fog, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    /* The two uniform blocks, at the std140 offsets the generated header
       prints. */
    memset(&ub, 0, sizeof ub);
    ub.f[0] = h.gw; ub.f[1] = h.gh;                    /* uGame       vec2 @0  */
    ub.f[2] = h.zoom;                                  /* uZoom      float @8  */
    ub.f[4] = h.zoomCx; ub.f[5] = h.zoomCy;            /* uZoomC      vec2 @16 */
    ub.f[6] = h.depthScale;                            /* uDepthScale float @24 */
    memcpy(s_umap + (size_t)slot * s_ustride, ub.f, UBLK_VS);
    memset(&ub, 0, sizeof ub);
    ub.i[0] = h.restored;                              /* uRestored    int @0  */
    ub.f[2] = h.fogOrgX; ub.f[3] = h.fogOrgY;          /* uFogOrg     vec2 @8  */
    ub.f[4] = h.fogCols; ub.f[5] = h.fogRows;          /* uFogDim     vec2 @16 */
    ub.i[6] = h.fog;                                   /* uFog         int @24 */
    /* uScafOn @28 IS ALWAYS 0: the scaffold test is not run by this pass (see
       the file header). Running it would need the B_UNDER draw's own block --
       a second offset in this buffer and a second descriptor -- because the
       test applies to that one draw of the frame only. */
    ub.i[7] = 0;                                       /* uScafOn      int @28 */
    ub.f[8]  = h.scafP[0]; ub.f[9]  = h.scafP[1];      /* uScafP      vec4 @32 */
    ub.f[10] = h.scafP[2]; ub.f[11] = h.scafP[3];
    ub.f[12] = h.uss;                                  /* uSS        float @48 */
    ub.f[13] = h.zoomF;                                /* uZoomF     float @52 */
    ub.f[14] = h.zoomCFx; ub.f[15] = h.zoomCFy;        /* uZoomCF     vec2 @56 */
    memcpy(s_umap + (size_t)slot * s_ustride + s_ublock, ub.f, UBLK_FS);

    for (b = 0; b < TAGPU_FXB_N; b++) s_n[b] = h.n[b];
    s_hGw = h.gw; s_hGh = h.gh; s_hSs = h.ss;
    s_hVpL = h.vpL; s_hVpT = h.vpT; s_hVw = h.vw; s_hVh = h.vh;
    s_hScissorOn = h.scissorOn;

    /* THE A/B FRAME IS CLAIMED LAST, AFTER EVERY REASON NOT TO DRAW IS PAST. A
       frame claimed and then not drawn would have the seam capture a bare
       clear, and a diff against another build's capture would report every
       effect pixel as differing: a failure of the instrument, read as one of
       the pass. */
    s_abFrame = h.ab;
    s_drawThis = 1;
    s_modelsFrame = d->frame;
    return 1;

refuse:
    /* NOTHING IS DESTROYED HERE, AND THAT IS THE WHOLE POINT. This is the
       middle of a frame. The seam waited on fence[slot] ALONE (tagpu_vk.c), so
       every OTHER slot's submit is still executing against this pass's
       pipelines, its descriptor pool, its shared image and its per-slot
       buffers -- and `cb`, which this function has already recorded uploads
       into, is submitted whether this pass draws or not. Destroying any of it
       from here is a use-after-free on the FIRST refusal, not a rare one.

       So the pass stops drawing at once and OWES a teardown. The seam pays it
       at the top of a later frame, behind the vkDeviceWaitIdle that makes "no
       submit names these objects" a fact rather than a hope. */
    plog(d, "fx: slot %u would not take this frame's resources - the pass stops "
            "drawing and the seam tears it down", (unsigned)slot);
    refuse_job(d);
    s_state = ST_REFUSED;
    s_downOwed = 1;
    return 0;
}

/* THE UPLOAD THIS PASS OWES ON A FRAME IT DID NOT MAKE ONE --
   tagpu_vk_feat.c's `atlas_owed`, which has the argument: the allowance counts
   from this copy, so a copy brought up to date only on frames that draw could
   leave every effect waiting on itself. After `prepare_draw`, so every
   `slot_free` of this frame is behind it. */
static void atlas_owed(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_FXHAND h;
    if (s_state != ST_READY || s_downOwed || s_upFrame == d->frame) return;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return;
    if (!tagpu_fx_atlas_owed()) return;
    memset(&h, 0, sizeof h);
    if (!tagpu_fx_atlas_hand(&h) || !h.pal) return;
    if (base_upload(d, cb, &s_slot[slot], &h) < 0) {
        plog(d, "fx: the owed atlas upload failed on the device - the pass stops "
                "drawing and the seam tears it down");
        refuse_job(d);
        s_state = ST_REFUSED;
        s_downOwed = 1;
    }
}

int tagpu_vk_fx_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    const int r = prepare_draw(d, cb, slot);
    atlas_owed(d, cb, slot);
    return r;
}

void tagpu_vk_fx_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                        uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = 0;
    uint32_t first = 0;
    int b;

    (void)d;
    if (s_state != ST_READY || !s_drawThis) return;
    s_drawThis = 0;

    /* NO Y FLIP, AND THE DEPTH RANGE, AND THEY ARE TWO SEPARATE QUESTIONS.
       This pass writes `gl_Position.y = p.y/uGame.y*2 - 1` on the engine's
       screen-space y, which grows DOWNWARD, so clip -1 is the game frame's top
       row and a POSITIVE height puts it on row 0 -- where the game's top row
       is. A NEGATIVE height would turn the frame over.
       minDepth 0.5 / maxDepth 1.0 is the depth mapping every world pass that
       shares the attachment uses -- item 4 of the file header. */
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.minDepth = 0.5f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    fx_scissor(w, h);
    sc.offset.x = s_scX; sc.offset.y = s_scY;
    sc.extent.width = (uint32_t)s_scW; sc.extent.height = (uint32_t)s_scH;
    vkCmdSetScissor(cb, 0, 1, &sc);

    /* THE LINES' GRID, uGrid @64 of the fragment block (tagpu_line.h
       `tagpu_line_grid`): the line grid the records were built on -- the game
       frame at the hand-over's ss -- and THIS target's extent, which only
       `record` knows: on a frame the offscreen target refused, the world goes
       into the swapchain image at whatever scale that is, and LFS's line-grid
       pixel has to be that one's (tagpu_vk_mark.c says the same). The block is
       host-coherent and read only when `cb` executes, after this. */
    {
        int g[4];
        tagpu_line_grid(g, (int)(s_hGw + 0.5f), (int)(s_hGh + 0.5f), s_hSs,
                        (int)w, (int)h);
        memcpy(s_umap + (size_t)slot * s_ustride + s_ublock + 64, g, sizeof g);
    }

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1,
                            &s_slot[slot].dset, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_slot[slot].vbuf, &off);

    /* THE FOUR BUCKETS IN BUCKET ORDER, each with the pipeline its
       fixed-function state lives in. Two of them share one: the particle
       layers under everything and the sprites over it differ only in WHERE
       they are in this sequence, which is what makes the engine's own layering
       survive (smoke under weapon sprites, explosions over their flash). */
    for (b = 0; b < TAGPU_FXB_N; b++) {
        VkPipeline pipe;
        if (!s_n[b]) continue;
        pipe = (b == TAGPU_FXB_LINES) ? s_pipeLine
             : (b == TAGPU_FXB_FLASH) ? s_pipeFlash : s_pipeTri;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        /* the lines bucket holds records, one six-vertex band an instance,
           at the same stride as every other bucket's vertices */
        if (b == TAGPU_FXB_LINES) vkCmdDraw(cb, 6, (uint32_t)s_n[b], 0, first);
        else                      vkCmdDraw(cb, (uint32_t)s_n[b], 1, first, 0);
        first += (uint32_t)s_n[b];
    }
}

/* READY as well as stamped: `atlas_owed` runs after `prepare_draw` in the same
   call and can refuse the pass, and then `record` draws nothing. */
int tagpu_vk_fx_models_ok(unsigned frame)
{
    return s_state == ST_READY && !s_downOwed && s_modelsFrame == frame;
}

int tagpu_vk_fx_ab_frame(void)
{
    int a = s_abFrame;
    s_abFrame = 0;
    return a;
}

void tagpu_vk_fx_down(const TAGPU_VKPASS* d)
{
    VkDevice dev = d->dev;
    uint32_t i;
    /* WHETHER THIS TEARDOWN IS THE ONE THE PASS ASKED FOR. Only the seam's
       `_down_paid` sets it, and only after its vkDeviceWaitIdle -- so a
       `vk_down` or a `vk_resize` that happens to run while a debt is
       outstanding tears the pass down WITHOUT consuming it, and leaves it
       ST_UNBUILT so it can come back on the next device.

       THE DEBT ITSELF IS DISCHARGED BY EVERY TEARDOWN, paid or not, and that is
       a separate fact from the verdict: once this function has run there is
       nothing left to free, so an un-cleared flag would have the seam drain the
       device and call `_down_paid` on an already-dead pass the next time it
       looked -- which would latch ST_REFUSED by the back door. */
    int owed = s_downPaying;
    s_downOwed = 0;
    /* NOTHING TO FREE, BUT THE VERDICT STILL STANDS. */
    if (!dev || !vkDestroyBuffer) { s_state = owed ? ST_REFUSED : ST_UNBUILT; return; }

    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        slot_free(d, &s_slot[i]);
        s_slot[i].dset = VK_NULL_HANDLE;   /* goes back with the pool below */
    }
    /* THE RESTORE JOB GOES BACK BEFORE THE IMAGES IT NAMES: it holds a
       framebuffer over the restored atlas's view, and a view still named by a live
       framebuffer may not be destroyed. Every caller of this function is past
       the seam's vkDeviceWaitIdle, so neither is in a queue.
       AND THE VERDICT DOES NOT SURVIVE THE DEVICE -- `s_rjTried` is a fact
       about a device that refused, so a new one is asked again. */
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
    s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0; s_rjTried = 0;
    s_rjSrcView = VK_NULL_HANDLE;
    kill_image(d, &s_arImg, &s_arMem, &s_arView);
    kill_image(d, &s_bImg, &s_bMem, &s_bView);
    s_arHave = 0;
    s_atDim = 0;
    s_bHave = 0; s_bSerial = 0; s_bPal = 0;
    tagpu_fx_atlas_ack(0, 0);              /* the copy went with the device */
    if (s_pipeTri)   { vkDestroyPipeline(dev, s_pipeTri, NULL);   s_pipeTri = VK_NULL_HANDLE; }
    if (s_pipeLine)  { vkDestroyPipeline(dev, s_pipeLine, NULL);  s_pipeLine = VK_NULL_HANDLE; }
    if (s_pipeFlash) { vkDestroyPipeline(dev, s_pipeFlash, NULL); s_pipeFlash = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dpool) { vkDestroyDescriptorPool(dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_samp)  { vkDestroySampler(dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_umap)  { vkUnmapMemory(dev, s_umem); s_umap = NULL; }
    if (s_ubuf)  { vkDestroyBuffer(dev, s_ubuf, NULL); s_ubuf = VK_NULL_HANDLE; }
    if (s_umem)  { vkFreeMemory(dev, s_umem, NULL); s_umem = VK_NULL_HANDLE; }
    s_drawThis = 0;
    s_abFrame = 0;
    memset(s_n, 0, sizeof s_n);
    /* ST_UNBUILT and not ST_REFUSED: a pass brought down by a mode change or a
       cleared lever must be able to come back. THE ONE EXCEPTION IS THE
       TEARDOWN THIS PASS ASKED FOR. */
    s_state = owed ? ST_REFUSED : ST_UNBUILT;
}

/* 1 while this pass has stopped drawing and is waiting for the seam to drain
   the device and tear it down -- see `prepare`'s refusal path. */
int tagpu_vk_fx_down_owed(void)
{
    return s_downOwed;
}

/* THE SEAM'S OWN ENTRY POINT, called only after its vkDeviceWaitIdle. It is
   what makes `_down`'s ST_REFUSED latch apply to the owed teardown and to
   nothing else. */
void tagpu_vk_fx_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_fx_down(d);            /* clears s_downOwed itself */
    s_downPaying = 0;
}
