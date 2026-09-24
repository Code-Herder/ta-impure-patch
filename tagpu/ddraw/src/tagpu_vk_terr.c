/* tagpu_vk_terr.c -- the terrain pass (the 32x32 pre-rendered map tiles)
   drawn by Vulkan. Contract: tagpu_vk_terr.h.

   IT IS NOT A SECOND IMPLEMENTATION OF THE PASS. Everything arrives through
   `tagpu_terr_handover` (tagpu_terr.h): the instances are the array the
   gather filled, the uniforms are the numbers the gather computed, the texels
   are tagpu_terr.c's CPU mirrors of the tile atlas and the height grid -- the
   atlas's palette indices expanded here into the RGBA8 base atlas the shader
   samples -- and the shader is tagpu_terr.c's GLSL through tools/spirv-gen.py.

   ---- THE DESIGN, ITEM BY ITEM ----

   1. INSTANCING. One unit quad six vertices long and one INSTANCE per visible
      cell: a SECOND VkVertexInputBindingDescription at
      VK_VERTEX_INPUT_RATE_INSTANCE, and vkCmdDraw's `instanceCount`.

      The one trap in it is the FORMAT. The cell record is four unnormalised
      shorts read by a `vec4` attribute, so the conversion has to be "the
      integer, as a float". The Vulkan format that does that
      is R16G16B16A16_SSCALED; _SINT would require the shader's attribute to be
      an `ivec4` and would read as garbage against a `vec4`. SSCALED is not a
      format a driver must support as a vertex buffer, so `build_pipelines`
      ASKS for it and the pass stays down if the answer is no, naming the
      fallback (widening the record to floats on the CPU) rather than guessing.

   2. THE TEXELS, AND WHY THIS PASS NEEDS NO MIRROR MECHANISM. Terrain's two
      big textures are built WHOLE, once per map, so the mirror is simply the
      build buffer kept rather than freed (tagpu_terr.c `s_mirrorWant`) --
      unlike §2.29's GAF atlas, which is painted incrementally and needs a
      mirror that is correct from the instant it exists.

   3. SHARED IMAGES, AND SO A LIFETIME QUESTION §2.28 NAMED AND §2.29 DID
      NOT HAVE TO ANSWER. The base atlas is ~24 MB and the height grid up to a few;
      per-slot they would be eight copies of each, in a 32-bit address space
      whose largest free block is the number this phase spends its budget
      measuring. So both are ONE image, uploaded when the mirror's serial says
      the bytes moved, behind the write-after-read barrier §2.29 established.

      Unlike §2.29's atlas, THEIR DIMENSIONS GENUINELY CHANGE -- an in-process
      map change gives the tile set a different count and the height grid a
      different size -- and a shared image that has to be replaced cannot have
      its descriptor rewritten under frames in flight. Two things make that safe
      BY CONSTRUCTION rather than by timing:

        * every slot's samplers are (re)written during THAT SLOT'S OWN
          `prepare`, which is the one instant the seam's fence proves nothing of
          ours is in flight for it. So no write in this file ever touches a set
          another frame may be using.
        * the replaced image is RETIRED behind a slot bitmask, not a timer:
          `pending` starts as every slot and a bit clears when that slot has
          been VISITED -- at the top of its own `prepare`, unconditionally, and
          NOT when its set is rewritten. Saying "when the set has been
          rewritten" would be the tidier invariant and it is not this one: the
          bit has to clear on the paths that return without binding, or the one
          resize that cannot be applied stalls the retire for ever (see the
          accounting at the top of `prepare`). What makes the clear safe is the
          pair of facts in `shared_slot_done`'s own comment -- the slot's last
          submit is complete, and `record` runs only after a `prepare` that
          returned 1, every one of which called `shared_bind`. The second of
          those is also a TEST, in `record`, against the views each slot was
          last bound to. `pending == 0` then means every submit that could name
          the old image has completed, and only then is it destroyed.
          A second change arriving while one is pending draws NOTHING for the
          frames it takes to clear rather than starting a second retire -- at
          most `slots` frames, and only for back-to-back map changes.

   4. DEPTH. Terrain is the frame's implicit far plane: it tests
      VK_COMPARE_OP_LESS and WRITES, and everything above it is tested against
      what it wrote. The range answer is §2.28's: a
      viewport with `minDepth 0.5` / `maxDepth 1.0`, which maps clip z in [0, 1]
      onto exactly GL's `(z+1)/2`, the values the OpenGL renderer this replaced
      wrote, precision included. Never a shader edit -- tools/spirv-gen.py's
      transform is mechanical and touches no line of a shader body.
      The pass refuses to arm when the seam's render pass carries no depth
      attachment.

   5. NO BLENDING. Terrain is opaque with alpha 1 everywhere, so the colour
      blend attachment has blending OFF. The feature pass's premultiplied pair
      is its own and does not belong here.

   6. THE SCISSOR is §2.29's and is NOT mirrored: `offset.y = vpT`, because
      row 0 is the game's top row (item 7). Its ENABLE travels with it. It
      matters more here than it did there: terrain covers the whole gather
      rect, which at zoom < 1 reaches well past the viewport and over the side
      panel.

   7. NO Y FLIP. This pass writes `gl_Position.y = p.y/uGame.y*2 - 1` on the
      engine's screen-space y, which grows DOWNWARD, so clip -1 is the game
      frame's top row, and a POSITIVE viewport height puts it on row 0 -- the
      game's top row. A negative height turns the frame over a second time and
      presents the world upside down, and an A/B that compares two captures is
      blind to a flip they share. VK_KHR_maintenance1 is needed only for a
      negative height, so this pass does not require it.

   ---- WHAT IT REFUSES ----

   A frame whose hand-over reports the cast-shadow map on is refused when THIS
   frame's map was not drawn. tagpu_vk_shadow.c draws the map into an
   offscreen depth image of its own and this pass SAMPLES it, through bindings
   44 and 45, bound to that slot's view during this slot's own `prepare`; the
   shadow pass has no producer today (tagpu_vk_shadow.h) and tagpu_terr.c
   publishes `shadowOn` as 0, so no frame reaches this refusal. The 1x1 dummy
   stays, because
   the descriptors must still be valid on a frame with no map.

   The Classic++ restored tile atlas is NOT a refusal: a frame this lane has
   not restored draws the base atlas (see `prepare`).

   IT KNOWS NOTHING ABOUT A WINDOW. Everything arrives in TAGPU_VKPASS.
   A PASS READS NO ENGINE STATE: every value comes from the hand-over, so this
   file is not on thread-split.allow and must never be. */

#include "tagpu_vk_pass.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_terr.h"
#include "tagpu_terr.h"
#include "tagpu_vk_shadow.h"
#include "tagpu_vk_restore.h"
#include "tagpu_vk_stage.h"
#include "tagpu_pal.h"                  /* tagpu_pal_expand */
#include "spirv/tagpu_terr.spv.h"

#define UBLK_VS  64                        /* std140, the generated header's   */
#define UBLK_FS  192

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
    X(vkCmdCopyBufferToImage) \
    X(vkCmdPipelineBarrier) X(vkCmdClearDepthStencilImage)

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
static int s_saidRestored;                 /* the latched-restorer line, said once */
static int s_saidShadow;
static int s_saidCmp;
static int s_saidNoMirror;

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;
static VkDescriptorPool      s_dpool;
static VkSampler             s_samp, s_sampCmp;

static VkBuffer       s_ubuf;
static VkDeviceMemory s_umem;
static unsigned char* s_umap;
static VkDeviceSize   s_ustride;           /* both blocks, each device-aligned */
static VkDeviceSize   s_ublkF;             /* the fragment block's own offset  */

/* the unit quad, once: six vertices from tagpu_terr.h's literal, uploaded at
   build and never touched again -- it is the pass's fixed geometry */
static VkBuffer       s_qbuf;
static VkDeviceMemory s_qmem;

/* THE 1x1 DEPTH IMAGE THE TWO SHADOW SAMPLERS NAME on a frame with no map.
   D16 because it is the smallest depth format every device carries, and because
   this image is never sampled: taShadowAt returns 1.0 on `uShadowOn == 0`
   before it touches either sampler. It still has to be a VALID descriptor, and
   its format still has to be one the compare sampler is legal against, which is
   why `build_samplers` asks about this one as well as the map's. */
#define SHADOW_DUMMY_FMT VK_FORMAT_D16_UNORM
static int            s_cmpLinear;     /* the compare sampler filters LINEAR  */
static VkImage        s_shImg;
static VkDeviceMemory s_shMem;
static VkImageView    s_shView;
static int            s_shReady;           /* cleared and in SHADER_READ layout */

/* ---- A SHARED IMAGE, and the retire that makes replacing one safe ---------
   See item 3 of the file header. `pending` is a bitmask of slots whose
   descriptor set may still name `old*`; a bit clears only under that slot's own
   fence, and `pending == 0` is what licenses the destroy. */
typedef struct {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    int            w, h;
    unsigned       serial;                 /* the mirror serial it holds       */
    int            have;                   /* a copy has been recorded into it */

    VkImage        oldImg;
    VkDeviceMemory oldMem;
    VkImageView    oldView;
    uint32_t       pending;                /* slots yet to stop naming oldView */
} SHARED;
/* WHAT AN IMAGE HERE IS FOR. Everything this pass makes is uploaded and then
   sampled; the one exception is the restored atlas, which the Vulkan restorer
   RENDERS INTO under Classic++ `assets=1` -- and which therefore carries
   COLOR_ATTACHMENT unconditionally rather than only on the frames the lever is
   on. RGBA8 optimal-tiling colour-attachment support is required of every
   Vulkan device, so the flag cannot be refused, and paying for it always is
   what keeps the image's identity independent of which path filled it: a
   session that flipped the lever would otherwise be asking for a resize of an
   image whose w and h had not changed, which `shared_resize` answers with
   "already right". */
#define IMG_SAMPLED   (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
#define IMG_RESTORED  (IMG_SAMPLED | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
static SHARED s_height;                    /* the height grid, R8             */
/* THE BASE ATLAS, RGBA8 at the tile atlas's size: every texel its index's
   colour through the hand-over's palette -- the engine's table --
   (tagpu_pal_expand, nothing keyed: a tile has no hole). It is what the
   terrain is drawn from, the restored atlas over it where that has been
   painted, and it is the restorer's source. Binding 46. `serial` is the
   tile atlas's serial it was expanded from and `s_basePal` the palette's, and
   a move of either re-sends the whole image -- the tile atlas is rebuilt whole
   or not at all, so there is no smaller rect to send. */
static SHARED   s_base;
static unsigned s_basePal;
/* CLASSIC++'s RESTORED TILE ATLAS, RGBA8. A
   third shared image with the same retire discipline as the other two; binding
   41 names it instead of the placeholder `shared_bind` describes. It is
   only resized when the hand-over carries a restore list, so a session with
   Classic++ off never creates it.
   `SHARED::serial` IS UNUSED FOR THIS ONE. It is the mirror serial an upload
   latches, and `s_base`/`s_height` latch theirs; this image has no upload,
   and what says it holds a picture is `have`, written by the restore job
   alone. */
static SHARED s_rgbAtlas;
/* ---- ...AND THE RESTORE THAT FILLS IT --------------------------------------
   Under Classic++ `assets=1` the producer publishes the frame list
   (tagpu_terr.h, `restoreFrames`), and this pass paints `s_rgbAtlas` itself
   through tagpu_vk_restore.c.
   `s_rjSerial` is the hand-over serial the live job was built from: a change
   means the atlas stopped being the one the job's rectangles describe, so the
   job goes and a new one is built. `s_rjPal` is the engine palette serial it
   was built with, and a move of it is a new job too: the job keeps the
   palette it was made with, and the base atlas it reads is re-sent in the new
   colours. `s_rjTried` stops a device that refused from being asked once a
   frame for the rest of the session. */
static TAGPU_VKRJOB* s_rjob;
static unsigned      s_rjSerial, s_rjPal;
static int           s_rjTried;
static int           s_rjPainted;          /* job_painted at the last report  */
/* THE SOURCE VIEW THE LIVE JOB NAMES, and it is a separate key from the serial
   because the two move for different reasons. A job captures `s_base.view` at
   creation and never re-reads it, so ANY resize of the base atlas leaves its
   descriptors naming `s_base.oldView` -- which `shared_slot_done` destroys
   `slots` frames later while the restorer is still drawing. `restore_want`
   normally frees the job in the same frame, but it sits BELOW the resize and
   every `return 0` and `goto refuse` between the two skips it. So the drop is
   keyed here, immediately after the resize, where nothing can return first. */
static VkImageView   s_rjSrcView;


/* what `record` was left to draw */
static int s_ncell;
static int s_scX, s_scY, s_scW, s_scH;     /* the scissor, in Vulkan framebuffer px */
/* The scissor's INPUTS, kept as numbers rather than as a copy of the hand-over.
   TAGPU_TERRHAND is full of pointers into the producer's frame memory, and a
   static holding those past the frame they were handed over on is a dangling
   read waiting for someone to add a line that follows one. */
static float s_hGw, s_hGh;
static int   s_hVpL, s_hVpT, s_hVw, s_hVh, s_hScissorOn;

/* ---- one of these per frame slot, and every field of it is ours for the
   duration of the `prepare`/`record` pair we are handed that slot on. ---- */
typedef struct {
    VkBuffer        ibuf;                  /* the frame's cell instances       */
    VkDeviceMemory  imem;
    unsigned char*  imap;
    VkDeviceSize    icap;

    TAGPU_VKSTAGE   stage;                 /* the map-scoped uploads, when due  */

    VkImage         pal, fog;              /* the two small per-slot images    */
    VkDeviceMemory  palMem, fogMem;
    VkImageView     palView, fogView;
    VkBuffer        smallStage;
    VkDeviceMemory  smallMem;
    unsigned char*  smallMap;
    int             fogW, fogH;            /* what the fog image is sized for  */

    VkDescriptorSet dset;
    int             built;                 /* the fixed-size half is made      */
    /* WHAT THIS SLOT'S SET WAS LAST BOUND TO, so that "the set names the image
       that still exists" is a fact this file can CHECK rather than an argument
       about two functions a hundred lines apart. `shared_bind` writes them,
       `record` refuses to draw unless they are still the live views, and
       `slot_free` clears them. See the retire's second rule. */
    VkImageView     boundBase, boundHeight, boundShadow;
} SLOT;
static SLOT s_slot[TAGPU_VK_SLOTS];

/* THE BOUNDS ARE RE-CHECKED HERE, because a bound that lives in the file that
   produced the number is a bound only while both files are read together
   (the scaffold's rule). Every one of these sizes an allocation or a memcpy.
   The atlas is 64 cells on a 34-texel pitch, so its width is 2176 and its
   height grows with the tile count; the height grid is the map in 16-px cells,
   which tagpu_terr.c already refuses past 4096; the fog grid is the wide-fog
   builder's and is a few tens of cells. */
#define ATLAS_MAXDIM 16384
#define HEIGHT_MAXDIM 4096
#define FOG_MAXDIM    1024
/* THE CELL COUNT IS ONE OF THEM. It sizes both the
   instance buffer (`ncell * ICOMP * sizeof(short)`) and vkCmdDraw's
   instanceCount, so it belongs under this paragraph's own rule as much as the
   three above do. tagpu_terr.c clamps its gather to INST_MAX_BYTES / a cell,
   which is this number -- stated here in the terms this file allocates in, so
   that neither file has to be read to trust the other. IT MUST NOT BE TIGHTER
   THAN THE PRODUCER'S: a pass that refused a cell count the gather legitimately
   published would drop the terrain from the whole viewport. */
#define CELL_MAXBYTES (24u * 1024u * 1024u)
#define CELL_MAX      ((int)(CELL_MAXBYTES / (TAGPU_TERR_ICOMP * sizeof(short))))
/* ONE STAGING BUFFER CARRIES BOTH SMALL UPLOADS: the palette (256 x 1 RGBA),
   then the fog grid (cols x rows RG8, a few KB and a different size whenever
   the view walks far enough for the grid to be re-laid). One allocation, two
   vkCmdCopyBufferToImage regions at two offsets -- and it is rebuilt with the
   fog image, by the same call, so the buffer and the image it feeds can never
   disagree about the size. */
#define SMALL_PALOFF 0                     /* 256 x RGBA8                      */
#define SMALL_FIXED  (256 * 4)             /* what the fixed upload takes      */
#define SMALL_FOGOFF SMALL_FIXED           /* the grid, cols x rows RG8        */
/* Both are a multiple of 4 and of their own texel block size, which is what
   vkCmdCopyBufferToImage requires of a bufferOffset. The two big uploads share
   one buffer on the same rule -- the height grid at 0 and the base atlas at the
   grid's size rounded up to 4. */
#define ALIGN4(n) (((n) + 3u) & ~(VkDeviceSize)3u)

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
    if (type < 0) { plog(d, "terr: no memory type for a buffer"); return 0; }
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

/* `usage` is the caller's because one of these images is drawn INTO and the
   rest are only sampled: the Classic++ restored atlas is the Vulkan restorer's
   render target, and a colour attachment has to say so at create
   time. It is passed rather than added to every image here because the flag is
   not free -- a driver may pick a different tiling for an image that can be
   rendered to -- and three of these four are never rendered to. */
static int mk_image(const TAGPU_VKPASS* d, int w, int h, VkFormat fmt,
                    VkImageAspectFlags aspect, VkImageUsageFlags usage,
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
    if (type < 0) { plog(d, "terr: no device-local memory for an image"); return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, *img, *mem, 0) != VK_SUCCESS) return 0;

    ivi.image = *img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    ivi.subresourceRange.aspectMask = aspect;
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

static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                        VkImageLayout from, VkImageLayout to,
                        VkPipelineStageFlags srcStage, VkAccessFlags srcAcc,
                        VkPipelineStageFlags dstStage, VkAccessFlags dstAcc)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
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
    if (s->imap) { vkUnmapMemory(dev, s->imem); s->imap = NULL; }
    if (s->ibuf) { vkDestroyBuffer(dev, s->ibuf, NULL); s->ibuf = VK_NULL_HANDLE; }
    if (s->imem) { vkFreeMemory(dev, s->imem, NULL); s->imem = VK_NULL_HANDLE; }
    s->icap = 0;
    tagpu_vk_stage_drop(d, &s->stage);
    /* THE UNMAP IS GUARDED BY THE MAP POINTER, NOT BY THE ALLOCATION: a
       vkMapMemory that failed leaves the allocation standing and unmapping it
       would be an error of its own. */
    if (s->smallMap) { vkUnmapMemory(dev, s->smallMem); s->smallMap = NULL; }
    if (s->smallStage) { vkDestroyBuffer(dev, s->smallStage, NULL); s->smallStage = VK_NULL_HANDLE; }
    if (s->smallMem) { vkFreeMemory(dev, s->smallMem, NULL); s->smallMem = VK_NULL_HANDLE; }
    kill_image(d, &s->pal, &s->palMem, &s->palView);
    kill_image(d, &s->fog, &s->fogMem, &s->fogView);
    s->fogW = s->fogH = 0;
    /* the set this slot holds names nothing live any more, and `record` tests
       exactly that before it draws */
    s->boundBase = s->boundHeight = s->boundShadow = VK_NULL_HANDLE;
    s->built = 0;
}

/* The fixed-size half of a slot: the palette image, and the descriptor write
   that never changes again. The shared images' bindings are NOT here -- they
   are rewritten on every prepare (see `shared_bind`). */
static int slot_build(const TAGPU_VKPASS* d, SLOT* s)
{
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;

    if (s->built) return 1;
    if (!mk_image(d, 256, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, IMG_SAMPLED,
                  &s->pal, &s->palMem, &s->palView)) return 0;

    memset(&ii, 0, sizeof ii); memset(&wr, 0, sizeof wr);
    ii.sampler = s_samp; ii.imageView = s->palView;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = s->dset; wr.dstBinding = 40; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    s->built = 1;
    return 1;
}

/* This slot's fog-grid image at `w` x `h`, rebuilt when the dimensions move --
   which they do whenever the view walks far enough for the grid to be re-laid.
   Free because the slot is ours: see §2.28's one-line invariant. */
static int slot_fog(const TAGPU_VKPASS* d, SLOT* s, int w, int h)
{
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    if (s->fogW == w && s->fogH == h && s->fog && s->smallStage) return 1;
    kill_image(d, &s->fog, &s->fogMem, &s->fogView);
    s->fogW = s->fogH = 0;
    if (!mk_image(d, w, h, VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, IMG_SAMPLED,
                  &s->fog, &s->fogMem, &s->fogView)) return 0;
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

/* Room for `bytes` of cell instances in this slot, grown by doubling and never
   shrunk while the pass is drawing. */
static int slot_inst(const TAGPU_VKPASS* d, SLOT* s, VkDeviceSize bytes)
{
    VkDeviceSize want = 65536;
    if (s->icap >= bytes && s->ibuf) return 1;
    while (want < bytes) want *= 2;
    if (s->imap) { vkUnmapMemory(d->dev, s->imem); s->imap = NULL; }
    if (s->ibuf) { vkDestroyBuffer(d->dev, s->ibuf, NULL); s->ibuf = VK_NULL_HANDLE; }
    if (s->imem) { vkFreeMemory(d->dev, s->imem, NULL); s->imem = VK_NULL_HANDLE; }
    s->icap = 0;
    if (!mk_buffer(d, want, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->ibuf, &s->imem, &s->imap)) return 0;
    s->icap = want;
    return 1;
}

/* ---- the shared images -------------------------------------------------- */

/* Retire what is there and put a new image of `w` x `h` in its place. Returns
   0 when a retire is still outstanding (the caller draws nothing this frame and
   tries again) or when the device refused the image. See item 3 of the header. */
static int shared_resize(const TAGPU_VKPASS* d, SHARED* sh, int w, int h,
                         VkFormat fmt, VkImageUsageFlags usage)
{
    if (sh->img && sh->w == w && sh->h == h) return 1;
    if (sh->oldImg) return 0;              /* one retire at a time, by design  */
    if (sh->img) {
        sh->oldImg = sh->img; sh->oldMem = sh->mem; sh->oldView = sh->view;
        sh->img = VK_NULL_HANDLE; sh->mem = VK_NULL_HANDLE; sh->view = VK_NULL_HANDLE;
        sh->pending = d->slots >= 32 ? 0xFFFFFFFFu : ((1u << d->slots) - 1u);
    }
    sh->w = sh->h = 0; sh->serial = 0; sh->have = 0;
    if (!mk_image(d, w, h, fmt, VK_IMAGE_ASPECT_COLOR_BIT, usage,
                  &sh->img, &sh->mem, &sh->view)) {
        /* mk_image can fail after vkCreateImage succeeded, so the half-made
           object is given back here: the caller's "is there an image at all"
           test is what tells a device refusal from a retire still clearing, and
           a partial one would read as the second for ever. */
        kill_image(d, &sh->img, &sh->mem, &sh->view);
        return 0;
    }
    sh->w = w; sh->h = h;
    return 1;
}

/* THE RETIRE, and why it is a lifetime rather than a timer. Called for every
   slot at the top of that slot's own `prepare` -- the one instant the seam's
   fence proves the submit that last used this slot's descriptor set has
   COMPLETED. Two facts then bound the old image's last reference:

     * no SUBMITTED command buffer can still name it through this slot, because
       that slot's submit is done;
     * no FUTURE one will, because `record` runs only when `prepare` returned 1,
       and every such `prepare` calls `shared_bind` first.

   So when the last bit clears, the old image is unreferenced by construction,
   whatever the frame rate, whatever the driver, and whatever this pass decided
   to do on the frames in between. */
static void shared_slot_done(const TAGPU_VKPASS* d, SHARED* sh, uint32_t slot)
{
    if (!sh->oldImg) return;
    sh->pending &= ~(1u << slot);
    if (sh->pending == 0) {
        kill_image(d, &sh->oldImg, &sh->oldMem, &sh->oldView);
    }
}

/* ---- build -------------------------------------------------------------- */

static int build_samplers(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties fp;
    int i;
    /* the four formats this pass samples, asked for rather than assumed: the
       height grid's, the fog grid's, the palette's and the base atlas's, and
       the shadow dummy's */
    static const VkFormat need[4] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                      VK_FORMAT_R8G8B8A8_UNORM, SHADOW_DUMMY_FMT };
    for (i = 0; i < 4; i++) {
        vkGetPhysicalDeviceFormatProperties(d->pd, need[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            plog(d, "terr: this device cannot sample format %d - the pass stays down",
                 (int)need[i]);
            return 0;
        }
    }
    /* NEAREST AND CLAMP_TO_EDGE. The base atlas is the only one the shader
       reaches through this sampler at all -- the palette, the fog grid and the
       height grid are all texelFetch -- and NEAREST is the engine's own tile
       blit: every screen pixel one tile texel, never a blend of two. */
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d->dev, &sci, NULL, &s_samp) != VK_SUCCESS) return 0;
    /* THE COMPARE SAMPLER: compare LESS_OR_EQUAL, with MIN and MAG **LINEAR**
       -- taShadowAt's 16-tap PCF is bilinear, and that filtering is half of what
       makes the penumbra smooth -- so this one is LINEAR too.
       LINEAR FILTERING OF A DEPTH FORMAT IS A FEATURE BIT, NOT A GIVEN, and it
       is asked of BOTH formats this sampler is ever used against: the map's
       (tagpu_vk_shadow.c chose it, and it is asked rather than assumed for the
       same reason the four above are) and the 1x1 dummy's, which the set names
       on every frame with no map. A device that will not filter either keeps a
       NEAREST compare sampler -- still a valid descriptor, which is all the
       dummy ever needed -- and `s_cmpLinear` 0 stands the pass down on a frame
       that would actually sample it, because a NEAREST PCF is a blockier
       penumbra than the one taShadowAt is written to draw. */
    {
        VkFormat mapfmt = tagpu_vk_shadow_format(d, NULL);
        int ok = 1;
        VkFormat both[2];
        int n = 0, j;
        both[n++] = SHADOW_DUMMY_FMT;
        if (mapfmt != VK_FORMAT_UNDEFINED) both[n++] = mapfmt;
        for (j = 0; j < n; j++) {
            vkGetPhysicalDeviceFormatProperties(d->pd, both[j], &fp);
            if (!(fp.optimalTilingFeatures &
                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) ok = 0;
        }
        /* THIS IS DELIBERATELY STRICTER THAN THE SPECIFICATION AND SAYS SO.
           VUID-vkCmdDraw-magFilter-04553 conditions the FILTER_LINEAR format
           feature on `compareEnable == VK_FALSE`, so a depth-COMPARE sampler is
           arguably entitled to LINEAR without it -- which would make this gate
           refuse Classic++ shadow frames on a device where the PCF is legal.
           The reference device offers the bit, so nothing is lost here today,
           and being wrong the other way is undefined behaviour rather than a
           stand-down. Revisit with a validation layer running, which this lane
           still does not have. */
        s_cmpLinear = ok && mapfmt != VK_FORMAT_UNDEFINED;
        if (ok) { sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR; }
        else
            plog(d, "terr: this device will not filter a depth format LINEARly, "
                    "so the cast-shadow PCF cannot be the twin's - frames with "
                    "the map on will stand down");
    }
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(d->dev, &sci, NULL, &s_sampCmp) != VK_SUCCESS) return 0;
    return 1;
}

static int build_pipeline(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[11];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[2];
    VkVertexInputAttributeDescription va[2];
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
    VkFormatProperties fp;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkResult r;
    int i, ok = 0;

    /* THE INSTANCE FORMAT IS ASKED FOR (item 1 of the file header). */
    vkGetPhysicalDeviceFormatProperties(d->pd, VK_FORMAT_R16G16B16A16_SSCALED, &fp);
    if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
        plog(d, "terr: this device does not take R16G16B16A16_SSCALED as a vertex "
                "format - the Vulkan edition of the terrain pass stays down. "
                "The fix, if a device is ever found here, "
                "is to widen the cell record to four floats on the CPU: every "
                "field is a small integer and is exact in float.");
        return 0;
    }

    /* THE BINDINGS ARE THE GENERATOR'S, NOT THIS FILE'S: tools/spirv-gen.py
       allocates them BY STAGE, so set 0 binding 0 is the vertex stage's uniform
       block, 32 the fragment stage's, and 40.. its samplers in declaration
       order -- uPal, uAtlasRGB, uHeight, uFogGrid, uShadowCmp, uShadowRaw,
       uBase. The std140 offsets are printed in the generated header and
       are the contract for what goes into those buffers. */
    memset(b, 0, sizeof b);
    b[0].binding = 0;  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 32; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 2; i < 9; i++) {
        b[i].binding = (uint32_t)(40 + (i - 2));
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    dli.bindingCount = 9; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS) return 0;

    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) return 0;

    vs = mk_module(d, tagpu_spv_tagpu_terr_VS,
                   sizeof tagpu_spv_tagpu_terr_VS / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_tagpu_terr_FS,
                   sizeof tagpu_spv_tagpu_terr_FS / sizeof(uint32_t));
    if (!vs || !fs) { plog(d, "terr: a shader module was refused"); goto out; }

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    /* TWO BINDINGS (item 1). Binding 0 is the unit quad at VERTEX rate -- six
       corners, uploaded once -- and binding 1 is this frame's cell records at
       INSTANCE rate. */
    memset(vb, 0, sizeof vb);
    vb[0].binding = 0; vb[0].stride = 2 * sizeof(float);
    vb[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vb[1].binding = 1; vb[1].stride = TAGPU_TERR_ICOMP * sizeof(short);
    vb[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    memset(va, 0, sizeof va);
    va[0].location = 0; va[0].binding = 0; va[0].format = VK_FORMAT_R32G32_SFLOAT;
    va[0].offset = 0;
    va[1].location = 1; va[1].binding = 1;
    va[1].format = VK_FORMAT_R16G16B16A16_SSCALED; va[1].offset = 0;
    vi.vertexBindingDescriptionCount = 2; vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = va;

    /* A LIST: TAGPU_TERR_QUAD is six corners
       in the engine's own vertex order, with the shared edge on (1,0)-(0,1). */
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;     /* both dynamic, set per frame */

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* NO CULLING. Nothing here culls and nothing here should. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* LESS AND DEPTH WRITES ON, which is what makes terrain the frame's far
       plane: everything above it is tested against what it wrote. */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    memset(&cba, 0, sizeof cba);
    /* NO BLENDING (item 5): terrain is opaque with alpha 1 everywhere. */
    cba.blendEnable = VK_FALSE;
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
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipe);
    if (r != VK_SUCCESS) { plog(d, "terr: the pipeline was refused (%d)", (int)r); goto out; }
    ok = 1;

out:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
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
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[1].descriptorCount = d->slots * 7;
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
       written when there is an image to name: 40 by `slot_build`, 43 by
       `slot_fog`, 44 and 45 by `shadow_build`, and 41/42/46 (and 44/45 again)
       on every prepare by `shared_bind`. */
    for (i = 0; i < d->slots; i++) {
        VkDescriptorBufferInfo bi[2];
        VkWriteDescriptorSet w[2];
        s_slot[i].dset = sets[i];
        memset(bi, 0, sizeof bi); memset(w, 0, sizeof w);
        bi[0].buffer = s_ubuf; bi[0].offset = i * s_ustride;             bi[0].range = UBLK_VS;
        bi[1].buffer = s_ubuf; bi[1].offset = i * s_ustride + s_ublkF;   bi[1].range = UBLK_FS;
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = sets[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bi[0];
        w[1] = w[0]; w[1].dstBinding = 32; w[1].pBufferInfo = &bi[1];
        vkUpdateDescriptorSets(d->dev, 2, w, 0, NULL);
    }
    return 1;
}

/* The 1x1 depth image bindings 44 and 45 name, and the two writes that name it.
   Done once, at build, before any set has been bound. */
static int shadow_build(const TAGPU_VKPASS* d)
{
    uint32_t i;
    if (!mk_image(d, 1, 1, SHADOW_DUMMY_FMT, VK_IMAGE_ASPECT_DEPTH_BIT, IMG_SAMPLED,
                  &s_shImg, &s_shMem, &s_shView)) return 0;
    for (i = 0; i < d->slots; i++) {
        VkDescriptorImageInfo ii[2];
        VkWriteDescriptorSet wr[2];
        memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
        ii[0].sampler = s_sampCmp; ii[0].imageView = s_shView;
        ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii[1].sampler = s_samp;    ii[1].imageView = s_shView;
        ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = s_slot[i].dset; wr[0].dstBinding = 44; wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[0].pImageInfo = &ii[0];
        wr[1] = wr[0]; wr[1].dstBinding = 45; wr[1].pImageInfo = &ii[1];
        vkUpdateDescriptorSets(d->dev, 2, wr, 0, NULL);
    }
    return 1;
}

/* Clear it once and leave it in SHADER_READ_ONLY_OPTIMAL. Nothing samples it --
   `shared_bind` names it only on a frame with no Vulkan map, and taShadowAt
   returns 1.0 on `uShadowOn == 0` before it touches either sampler -- but an
   image whose contents are undefined is one more thing to reason about for the
   price of one call. */
static void shadow_ready(VkCommandBuffer cb)
{
    VkClearDepthStencilValue cv;
    VkImageSubresourceRange rg;
    if (s_shReady || !s_shImg) return;
    memset(&rg, 0, sizeof rg);
    rg.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    rg.levelCount = 1; rg.layerCount = 1;
    cv.depth = 1.0f; cv.stencil = 0;
    img_barrier(cb, s_shImg, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearDepthStencilImage(cb, s_shImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &cv, 1, &rg);
    img_barrier(cb, s_shImg, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    s_shReady = 1;
}

/* Point this slot's 41, 42 and 46 -- and the shadow pair, 44 and 45 -- at the
   shared images as they stand now.
   ON EVERY PREPARE, and that is the point: it is the one instant the seam's
   fence proves this set is not in use, so a shared image that had to be
   replaced is picked up here rather than by a write that reaches every slot. */
static void shared_bind(const TAGPU_VKPASS* d, uint32_t slot)
{
    VkDescriptorImageInfo ii[5];
    VkWriteDescriptorSet wr[5];
    memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
    /* 46 is uBase, NEAREST: the tile texel as the engine blits it */
    ii[0].sampler = s_samp; ii[0].imageView = s_base.view;
    ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    /* BINDING 41 IS uAtlasRGB, AND IT NAMES THE RESTORED ATLAS -- the
       ordinary case with Classic++ on. The fragment stage reads it on the
       `uRestored == 1` branch, which this pass draws.
       IT FALLS BACK TO THE BASE ATLAS'S VIEW when there is no restored image,
       and that is not a picture: a descriptor has to be VALID for the set to be
       bound, and naming the image already here costs no memory and no second
       object. Nothing samples it through this binding while the fallback is in
       place, because `uRestored` is computed from `s_rgbAtlas.view` and
       `s_rgbAtlas.have` -- the same two facts that decide which view lands
       here (see the test itself, just below), so the flag and the descriptor
       cannot disagree. */
    ii[1] = ii[0];
    /* ...AND ONLY ONCE SOMETHING HAS PAINTED IT, which is a LAYOUT question
       before it is a picture one. `shared_resize` creates this image
       UNDEFINED and nothing transitions it but the restorer's own job, whose
       clear runs UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY. On the
       declined-restorer path the image is created and no job is ever built, so
       it stays UNDEFINED -- and naming it here with
       `imageLayout = SHADER_READ_ONLY_OPTIMAL` is a descriptor-layout mismatch
       on a statically-used binding, which the validation layers report and
       which the spec leaves undefined if the shader's `uRestored == 1 ? … : …`
       is compiled as a select rather than a branch. `have` is set from
       `painted > 0`, which cannot happen before that clear, so testing it is
       exactly "this image has left UNDEFINED".
       It is also the pair that computes `uRestored` below, which is what makes
       the flag and this descriptor agree by construction rather than by a
       refusal placed somewhere else. */
    if (s_rgbAtlas.view && s_rgbAtlas.have) ii[1].imageView = s_rgbAtlas.view;
    /* 42 is uHeight; with no grid it names the base atlas's view, which the
       shader never samples -- its `uHDim.x > 0.5` test is what gates it */
    ii[2].sampler = s_samp;
    ii[2].imageView = s_height.view ? s_height.view : s_base.view;
    ii[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = s_slot[slot].dset; wr[0].dstBinding = 46; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[0].pImageInfo = &ii[0];
    wr[1] = wr[0]; wr[1].dstBinding = 41; wr[1].pImageInfo = &ii[1];
    wr[2] = wr[0]; wr[2].dstBinding = 42; wr[2].pImageInfo = &ii[2];
    /* BINDINGS 44 AND 45 ARE ANOTHER PASS'S IMAGE, and this is
       the one instant it may be written: the seam's fence has proved this set
       is not in flight, and tagpu_vk_shadow.c's `prepare` for this same slot
       has already run this frame -- the seam calls it first, and says why. The
       view it hands back is that slot's own, so it cannot be replaced again
       until this slot comes round, which is after this frame's draw.
       With no map this frame it is the 1x1 dummy: the descriptor must be valid whether or not anything samples it. */
    {
        VkImageView sv = tagpu_vk_shadow_view(d->frame, slot);
        if (!sv) sv = s_shView;
        ii[3].sampler = s_sampCmp; ii[3].imageView = sv;
        ii[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii[4].sampler = s_samp;    ii[4].imageView = sv;
        ii[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[3] = wr[0]; wr[3].dstBinding = 44; wr[3].pImageInfo = &ii[3];
        wr[4] = wr[0]; wr[4].dstBinding = 45; wr[4].pImageInfo = &ii[4];
        s_slot[slot].boundShadow = sv;
    }
    vkUpdateDescriptorSets(d->dev, 5, wr, 0, NULL);
    s_slot[slot].boundBase   = ii[0].imageView;
    s_slot[slot].boundHeight = ii[2].imageView;
}

static int build(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties props;
    VkDeviceSize ualign;
    static const float quad[TAGPU_TERR_QUADV * 2] = TAGPU_TERR_QUAD;
    unsigned char* qmap = NULL;

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "terr: %u frame slots is outside what this pass carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    /* TERRAIN IS THE FRAME'S FAR PLANE: it writes the depth everything above it
       is tested against, so it refuses rather than draws untested. */
    if (d->dfmt == VK_FORMAT_UNDEFINED) {
        plog(d, "terr: the seam's render pass carries no depth attachment, and "
                "this pass writes the depth every later pass is tested against "
                "- it stays down");
        return 0;
    }
    if (!resolve(d)) { plog(d, "terr: an entry point is missing"); return 0; }

    vkGetPhysicalDeviceProperties(d->pd, &props);
    /* TWO BLOCKS PER SLOT, EACH AT AN OFFSET THE DEVICE ACCEPTS.
       `minUniformBufferOffsetAlignment` is 16 on some devices and 256 on
       others, and a bound buffer offset that is not a multiple of it is
       undefined behaviour rather than a slow path. The two blocks are 64 and
       192 bytes (the generated header prints the std140 offsets), so each is
       rounded up to that alignment in turn. */
    ualign = props.limits.minUniformBufferOffsetAlignment;
    if (ualign < 1) ualign = 1;
    s_ublkF = ((VkDeviceSize)UBLK_VS + ualign - 1) / ualign * ualign;
    s_ustride = s_ublkF + ((VkDeviceSize)UBLK_FS + ualign - 1) / ualign * ualign;

    if (!mk_buffer(d, s_ustride * d->slots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_ubuf, &s_umem, &s_umap)) return 0;
    /* the unit quad, once: tagpu_terr.h's literal. */
    if (!mk_buffer(d, sizeof quad, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_qbuf, &s_qmem, &qmap)) return 0;
    memcpy(qmap, quad, sizeof quad);
    if (!build_samplers(d)) return 0;
    if (!build_pipeline(d)) return 0;
    if (!build_descriptors(d)) return 0;
    if (!shadow_build(d)) return 0;

    plog(d, "terr: the Vulkan edition is up - %u frame slots, uniform stride %u, "
            "depth format %d",
         (unsigned)d->slots, (unsigned)s_ustride, (int)d->dfmt);
    return 1;
}

/* ---- the frame ---------------------------------------------------------- */

/* THE SCISSOR, in Vulkan framebuffer pixels. Item 6 of the file header: the
   rect arrives in GAME-FRAME pixels measured from the TOP of the frame (the
   engine's own viewport rect, as the native pass publishes it), and under this
   pass's positive viewport height (item 7) a framebuffer's row 0 is the game
   frame's top row too. So the rect goes in unmirrored.

   It is SCALED, by the attachment's extent over the game frame's. The
   attachment is the world target (`gw*ss`) or, without one, the swapchain
   image at client resolution, and neither need be the game frame's size;
   scaling here keeps the clip on the same part of the world, which is the same
   thing the vertex shader's division by uGame does. */
static void terr_scissor(uint32_t w, uint32_t h)
{
    float sx = s_hGw > 0.0f ? (float)w / s_hGw : 1.0f;
    float sy = s_hGh > 0.0f ? (float)h / s_hGh : 1.0f;
    int x0 = (int)(s_hVpL * sx + 0.5f);
    int ww = (int)(s_hVw * sx + 0.5f);
    int ytop = (int)(s_hVpT * sy + 0.5f);
    int hh = (int)(s_hVh * sy + 0.5f);
    int y0 = ytop;                          /* NOT mirrored: item 6 */

    /* NO CLIP WHERE THE NATIVE PASS HAS NONE. `scissorOn` is what the native pass
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

/* A REFUSED PASS GIVES ITS RESTORE JOB BACK AT ONCE -- tagpu_vk_feat.c's
   `refuse_job`, which has the argument: a banded upload the device failed
   partway leaves `s_base` half written in a transfer layout, and the job's
   FILL would sample it. */
static void refuse_job(const TAGPU_VKPASS* d)
{
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
    s_rjSerial = 0; s_rjPainted = 0; s_rjSrcView = VK_NULL_HANDLE;
    s_rgbAtlas.have = 0;
}

/* ---- THE RESTORE REQUEST, TAKEN --------------------------------------------
   Called once per `prepare`, AFTER the base atlas's upload and before the
   refusal that asks whether the restored atlas holds a picture. The order is
   not cosmetic: the restorer samples `s_base`, the upload that fills it is
   recorded into THIS frame's command buffer a few lines above, and the slice
   that reads it is recorded into the same buffer a few lines later by the
   seam -- so the source is a fact by the time the first FILL runs, and no
   frame is spent waiting for it.

   A CHANGE OF SERIAL IS A NEW JOB, not a re-feed. The serial moves when the
   atlas stopped being the one the frame list describes -- a new map or a
   dropped list -- and in both cases the rectangles or the destination are
   different. */
static void restore_want(const TAGPU_VKPASS* d, const TAGPU_TERRHAND* t)
{
    if (!t->restoreFrames || t->restoreN < 1) {
        /* The request went away: a drop (the atlas is not this map's) or the
           lever was never on. Either way the job describes nothing now. */
        if (s_rjob) {
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL;
            s_rjSrcView = VK_NULL_HANDLE;
            s_rgbAtlas.have = 0;           /* what it holds is the old map's   */
        }
        return;
    }
    if (s_rjob && s_rjSerial == t->restoreSerial && s_rjPal == t->palSerial) {
        /* Live. `painted` is the only thing that changes the pass's own view of
           the atlas: one painted frame is what makes it a picture, and it is
           what `uRestored` and binding 41 are both computed from, below. */
        int painted = tagpu_vk_restore_job_painted(s_rjob);
        if (painted > 0) s_rgbAtlas.have = 1;
        if (painted != s_rjPainted) {
            s_rjPainted = painted;
            if (tagpu_vk_restore_job_idle(s_rjob))
                plog(d, "terr: restored atlas painted here - %d frames, no mirror "
                        "and no read-back", painted);
        }
        if (tagpu_vk_restore_job_failed(s_rjob)) {
            /* WHAT IT PAINTED STANDS UNTIL THE REQUEST CHANGES, and not past
               that: `s_rjSerial` keeps this request's serial, and the line
               below that drops `have` fires only when the serial moves, on a
               map change -- what the image holds was painted for the OLD
               request. */
            plog(d, "terr: the restore of the tile atlas failed on this lane; "
                    "what it painted stands until the request changes, and "
                    "nothing more is queued");
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL;
            s_rjTried = 1;                 /* it will fail the same way again  */
        }
        return;
    }
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
    /* PAST THIS POINT NO JOB IS PAINTING THIS REQUEST. When the serial has
       moved, `have` drops here and every path below starts from it: what the
       image holds was painted for the PREVIOUS request, over the previous map's
       rectangles, and `have` is what tells the shader to sample it. Leaving it
       set is a silently WRONG picture rather than a missing one -- the old
       map's colours over this one -- which is the failure class this stack is
       worst at. Dropping it falls back to the base atlas, which is the
       documented Classic++ fallback. The palette serial moving is the same
       fact about colour: the picture was painted in the old table's. Both
       unmoved is a job that failed on this request, and its picture stands
       (above). */
    if (s_rjSerial != t->restoreSerial || s_rjPal != t->palSerial) s_rgbAtlas.have = 0;
    if (s_rjTried) return;
    /* THE DESTINATION AND THE SOURCE BOTH HAVE TO BE THERE. `img` absent means
       the device refused the image (the resize above reads that way); `have`
       absent on the base atlas means its upload has not been recorded yet,
       and a FILL over an image with no contents would paint undefined texels
       over the world. Neither is an error -- the next frame asks again. */
    if (!s_rgbAtlas.img || !s_rgbAtlas.view || !s_base.view || !s_base.have) return;
    /* THE CONSUMER IS WHAT ASKS THE DEVICE, and that is deliberate: `up`
       loads the model off disk, so a session that never publishes a request
       never pays for it. It latches its verdict, so this is one integer
       compare on every frame after the first. */
    if (!tagpu_vk_restore_up(d)) { s_rjTried = 1; return; }
    s_rjob = tagpu_vk_restore_job_new(d, "terr", 0, 1, 0,
                                      s_base.img, s_base.view, s_base.w, s_base.h, 1,
                                      t->pal,
                                      s_rgbAtlas.img, s_rgbAtlas.view,
                                      s_rgbAtlas.w, s_rgbAtlas.h);
    if (!s_rjob) {
        s_rjTried = 1;                     /* the reason is already in the log */
        return;
    }
    if (!tagpu_vk_restore_job_add(s_rjob, t->restoreFrames, t->restoreN)) {
        plog(d, "terr: %d restore frames would not queue - nothing restored here",
             t->restoreN);
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL;
        s_rjTried = 1;
        return;
    }
    s_rjSerial = t->restoreSerial;
    s_rjPal = t->palSerial;
    s_rjSrcView = s_base.view;
    s_rjPainted = 0;
    plog(d, "terr: restoring the tile atlas HERE - %d frames over %dx%d, "
            "serial %u", t->restoreN, s_rgbAtlas.w, s_rgbAtlas.h,
         t->restoreSerial);
}

int tagpu_vk_terr_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_TERRHAND t;
    SLOT* s;
    int restored;                       /* what the SHADER is told, see below */
    VkDeviceSize ibytes, heightBytes, baseBytes;
    int doBase, rc;
    /* A UNION, NOT A CAST. Both blocks mix `int` and `float` members and
       writing an int through a float array is the aliasing rule broken at -O2,
       which is not a place to find out that the fog branch took a garbage
       uFog. */
    union { float f[UBLK_FS / 4]; int i[UBLK_FS / 4]; } ub;
    int fogW = 1, fogH = 1, hW = 1, hH = 1;
    int doHeight;

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;

    /* THE RETIRE ACCOUNTING, FIRST AND UNCONDITIONALLY, because from here on
       every path either draws or returns and both leave this slot's set unused
       until the next `shared_bind`. Doing it inside `shared_bind` instead would
       stall for ever on the one path that matters: a resize that cannot be
       applied because a retire is outstanding returns before binding, so the
       bit that would end the retire would never clear. See `shared_slot_done`. */
    if (s_state == ST_READY) {
        shared_slot_done(d, &s_height, slot);
        shared_slot_done(d, &s_rgbAtlas, slot);
        shared_slot_done(d, &s_base, slot);
    }

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW, AND NOTHING PER-SLOT
       IS KEPT ONCE THERE IS NOT -- §2.28's rule. The two SHARED images stay:
       they belong to every slot at once, so one slot going idle says nothing
       about them, and they come back only with the map. Giving slot `slot` back
       needs no new argument and no timer, because it is the same instant, and
       the same ownership, that the rest of this function writes it in. */
    if (!tagpu_terr_handover(&t, d->frame)) {
        if (s_state == ST_READY) slot_free(d, &s_slot[slot]);
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_terr_down(d); s_state = ST_REFUSED; return 0; }
        s_state = ST_READY;
    }
    shadow_ready(cb);

    /* THE CLASSIC++ RESTORED TILE ATLAS IS NOT A REASON TO REFUSE A FRAME.
       "Has this lane got the restored atlas YET" decides only whether
       `uRestored` is set and which view binding 41 names -- so a frame this
       lane has not restored draws the base atlas rather than not drawing. */
    /* THE CAST-SHADOW MAP IS DRAWN BY tagpu_vk_shadow.c, so the question is
       "is there a map for THIS frame". It is asked with our own frame number, which is what stops a
       map left standing from an earlier frame being sampled as though it were
       this one's -- and the shadow pass refuses any frame whose map holds a
       caster it has no copy of, so this refusal also carries every frame with a
       unit on screen until the unit pass lands.
       NOT LATCHED: `uShadowOn` follows the
       map, the map follows the casters, and a fixture walks in and out of both
       -- so a refusal here is a per-frame answer and the message is said once
       per run of them rather than once per session. */
    if (t.shadowOn && !tagpu_vk_shadow_ready(d->frame)) {
        if (!s_saidShadow) {
            s_saidShadow = 1;
            plog(d, "terr: the gather asks for the Classic++ cast-shadow map "
                    "and this frame's Vulkan map was not drawn (its casters are "
                    "not all on this side of the seam yet) - nothing drawn "
                    "rather than a different picture from our own oracle");
        }
        return 0;
    }
    if (!t.shadowOn || tagpu_vk_shadow_ready(d->frame)) s_saidShadow = 0;
    /* AND THE PCF HAS TO BE THE TWIN'S. `build_samplers` asked the device
       whether it will filter a depth format LINEARly and kept a NEAREST compare
       sampler when it will not; a 16-tap PCF through a NEAREST sampler is a
       different picture, so the frame stands down rather than draw one. */
    if (t.shadowOn && !s_cmpLinear) {
        if (!s_saidCmp) {
            s_saidCmp = 1;
            plog(d, "terr: the cast-shadow map is on and this device's compare "
                    "sampler could not be made LINEAR - nothing drawn while it "
                    "is, because a NEAREST PCF is not the twin's picture");
        }
        return 0;
    }

    /* THE BOUNDS, RE-CHECKED. Every one of these sizes an allocation or a
       memcpy, and a bound that lives in the file that produced the number is a
       bound only while both files are read together. */
    if (!t.atlas) {
        /* SAID ONCE. The mirrors are asked for on tagpu_terr.c's 30-frame
           poll and cannot be had before the tile set is loaded, so the first frames
           of a session legitimately arrive without one. */
        if (!s_saidNoMirror) {
            s_saidNoMirror = 1;
            plog(d, "terr: the tile atlas has no CPU mirror yet - nothing "
                    "drawn until it does (the twin asks for one on its 30-frame "
                    "poll, and the first one costs a rebuild of the atlas)");
        }
        return 0;
    }
    s_saidNoMirror = 0;
    if (!t.pal) return 0;
    if (t.ncell < 1 || !t.cells) return 0;
    if (t.ncell > CELL_MAX) {
        plog(d, "terr: %d cells is outside what this pass carries - nothing drawn", t.ncell);
        return 0;
    }
    if (t.atlasW < 1 || t.atlasH < 1 ||
        t.atlasW > ATLAS_MAXDIM || t.atlasH > ATLAS_MAXDIM) {
        plog(d, "terr: a %dx%d tile atlas is outside what this pass carries - nothing drawn",
             t.atlasW, t.atlasH);
        return 0;
    }
    if (t.height) {
        if (t.hW < 1 || t.hH < 1 || t.hW > HEIGHT_MAXDIM || t.hH > HEIGHT_MAXDIM) {
            plog(d, "terr: a %dx%d height grid is outside what this pass carries - nothing drawn",
                 t.hW, t.hH);
            return 0;
        }
        hW = t.hW; hH = t.hH;
    }
    if (t.fogGrid) {
        if (t.fogGridCols < 1 || t.fogGridRows < 1 ||
            t.fogGridCols > FOG_MAXDIM || t.fogGridRows > FOG_MAXDIM) {
            plog(d, "terr: a %dx%d fog grid is outside what this pass carries - nothing drawn",
                 t.fogGridCols, t.fogGridRows);
            return 0;
        }
        fogW = t.fogGridCols; fogH = t.fogGridRows;
    }

    s = &s_slot[slot];
    /* THIS SLOT IS OURS -- the seam waited on fence[slot] at the top of this
       frame, so the submit that last used these buffers, these images and this
       descriptor set has completed. That is what makes every write below safe
       with no device-wide wait. */

    /* THE SHARED IMAGES FIRST, because a resize that cannot be applied yet
       (one retire at a time) means this frame draws nothing at all rather than
       sampling the previous map's texels. */
    if (s_rjob && s_rjSrcView && s_base.view && s_rjSrcView != s_base.view) {
        /* see `s_rjSrcView`: the atlas the job reads from has been retired */
        plog(d, "terr: the base atlas moved under a live restore - dropping it "
                "and starting over on the new one");
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL; s_rjSerial = 0; s_rjPainted = 0; s_rjSrcView = VK_NULL_HANDLE;
        s_rgbAtlas.have = 0;
    }
    if (!shared_resize(d, &s_base, t.atlasW, t.atlasH, VK_FORMAT_R8G8B8A8_UNORM, IMG_SAMPLED)) {
        if (!s_base.img) goto refuse;
        return 0;                          /* a retire is still clearing       */
    }
    /* WITH NO HEIGHT GRID THE IMAGE IS ONE TEXEL AND uHDim IS 0: the shader's `uHDim.x > 0.5` test is what keeps it
       unsampled, and a 1x1 image keeps the descriptor valid meanwhile. */
    if (!shared_resize(d, &s_height, hW, hH, VK_FORMAT_R8_UNORM, IMG_SAMPLED)) {
        if (!s_height.img) goto refuse;
        return 0;
    }
    /* THE RESTORED ATLAS IS THE THIRD SHARED IMAGE, and it is sized HERE: after
       the bounds above and before `shared_bind` names its view. The frame LIST
       is the only thing that asks for it. The size is the ATLAS's, because
       terrain restores all of it.

       THE RETURN IS READ LIKE THE OTHER TWO IMAGES': an image still there means
       a retire is clearing and the frame waits; no image means the device
       refused, the view stays NULL, binding 41 keeps the base atlas's view,
       `uRestored` is 0 and `restore_want` below finds no destination. A device
       refusal is NOT fatal to the pass, which is why it does not `goto refuse`:
       the terrain draws the base atlas, which is what tagpu_vk_restore.h calls
       the shipped fallback.

       THE ORDERING IS FORCED: it has to come after the bounds, whose return it
       honours, and before `restore_want`, which needs the destination to
       exist before it can make a job. */
    if (t.restoreFrames &&
        !shared_resize(d, &s_rgbAtlas, t.atlasW, t.atlasH,
                       VK_FORMAT_R8G8B8A8_UNORM, IMG_RESTORED) &&
        s_rgbAtlas.img)
        return 0;
    if (!slot_build(d, s)) goto refuse;
    if (!slot_fog(d, s, fogW, fogH)) goto refuse;
    shared_bind(d, slot);

    ibytes = (VkDeviceSize)t.ncell * TAGPU_TERR_ICOMP * sizeof(short);
    if (!slot_inst(d, s, ibytes)) goto refuse;
    memcpy(s->imap, t.cells, (size_t)ibytes);

    /* THE TWO MAP-SCOPED UPLOADS SHARE THE SLOT'S BOUNDED STAGING
       (tagpu_vk_stage.h), allocated on the frame one of them is due and GIVEN
       BACK at this slot's next prepare on which neither is -- the same fence,
       one turn of the slots later -- so a settled map holds none of it. The
       base atlas is a whole map's tiles, far past the cap on a large map, so it
       goes out in bands, complete before this frame draws.
       A FRAME WITH NO STAGING IS SKIPPED, NOT REFUSED: the serials are not
       advanced, and the next frame sends what this one could not. */
    doHeight = t.height ? (!s_height.have || s_height.serial != t.heightSerial)
                        : !s_height.have;
    doBase = !s_base.have || s_base.serial != t.atlasSerial || s_basePal != t.palSerial;
    if (doHeight || doBase) {
        heightBytes = doHeight ? (VkDeviceSize)hW * hH : 0;
        baseBytes = doBase ? (VkDeviceSize)t.atlasW * t.atlasH * 4 : 0;
        if (!tagpu_vk_stage_begin(d, &s->stage, ALIGN4(heightBytes) + baseBytes,
                                  (VkDeviceSize)t.atlasW * 4))
            return 0;
        /* BOTH ARE WAITS (tagpu_vk_stage.h), so each says why it is here: a
           level's new atlas or grid, the pass's first upload, a palette move --
           none of them a frame of play */
        plog(d, "terr: %s%s%s, %u KB, through the banded path - %s",
             doBase ? "the base atlas" : "", doBase && doHeight ? " and " : "",
             doHeight ? "the height grid" : "",
             (unsigned)((ALIGN4(heightBytes) + baseBytes) >> 10),
             !s_base.have || !s_height.have ? "the first upload since the images were made"
             : doBase && s_basePal != t.palSerial ? "the engine's table moved"
             : "a new level's atlas or height grid");
        if (doHeight) {
            /* no grid: one zero texel, which the shader never reads */
            static const unsigned char zero = 0;
            rc = t.height
                ? tagpu_vk_stage_copy(d, cb, &s->stage, s_height.img, s_height.have,
                                      t.height, hW, hW, hH, 1)
                : tagpu_vk_stage_copy(d, cb, &s->stage, s_height.img, s_height.have,
                                      &zero, 1, 1, 1, 1);
            if (rc < 0) goto refuse;
            if (rc == 0) return 0;
            s_height.have = 1;
            s_height.serial = t.height ? t.heightSerial : 0;
        }
        if (doBase) {
            rc = tagpu_vk_stage_expand(d, cb, &s->stage, s_base.img, s_base.have,
                                       t.atlas, NULL, t.atlasW, 0, 0, t.atlasW, t.atlasH,
                                       t.pal, NULL);
            if (rc < 0) goto refuse;
            if (rc == 0) return 0;
            s_base.have = 1;
            s_base.serial = t.atlasSerial;
            s_basePal = t.palSerial;
        }
    } else {
        tagpu_vk_stage_drop(d, &s->stage);
    }

    /* AND THE REQUEST, if the producer published one --
       after the uploads for the reason stated at `restore_want`, and before
       the `restored` computation below, which is what its `have` feeds. */
    restore_want(d, &t);

    /* WHETHER THE SHADER SAMPLES THE RESTORED ATLAS IS THIS LANE'S OWN FACT,
       NOT THE PRODUCER'S REQUEST. `t.restored` says a restore request STANDS
       for this atlas; `s_rgbAtlas.have` says THIS LANE HAS PAINTED AT LEAST ONE
       CELL of it, and `s_rgbAtlas.view` is what binding 41 names. Until both
       hold, the base atlas is the picture -- which is the documented
       Classic++ fallback and, during a restore, is exactly the centre-out
       reveal: the fragment shader's alpha test picks restored or base PER
       CELL, so the cells show as they land.

       IT MUST NOT STAND THE WHOLE PASS DOWN. Every way the restorer can
       decline -- `tagpu_vk_restore_up` refusing a format or failing to load
       the model, `job_new` returning NULL, `job_add` refusing, a job that
       failed before its first slice -- latches `s_rjTried`, which is one-way
       for the device's life (cleared only in `tagpu_vk_terr_down`). After that
       `have` can never become 1, so a stand-down here would be PERMANENT: no
       terrain at all for the session, said once and silent after that.
       `tagpu_vk_restore.h` promises that a refusal leaves Classic++ on the base
       atlas on this lane, "the shipped fallback, not a fault"; drawing the base
       atlas is what keeps that promise. */
    restored = t.restored && s_rgbAtlas.view && s_rgbAtlas.have;
    /* SAID ONCE, AND ONLY FOR THE CASE WAITING CANNOT FIX: a request stands and
       this lane has latched a refusal, so the terrain draws the base atlas for
       the rest of the session. The ordinary not-yet-painted frames say nothing --
       that is the reveal working, not a fault. */
    if (t.restored && !restored && s_rjTried && !s_saidRestored) {
        s_saidRestored = 1;
        plog(d, "terr: a Classic++ restore request stands but this lane refused "
                "the restorer - the terrain draws the base atlas for this session");
    }

    /* THE TWO SMALL IMAGES, per slot, so the one-line invariant covers them:
       UNDEFINED in, because the whole of each is re-sent every frame and there
       are therefore no contents to preserve and no layout to carry. */
    memcpy(s->smallMap + SMALL_PALOFF, t.pal, 256 * 4);
    /* The grid is one `unsigned short` a cell and the image is RG8: the same
       two bytes in the same order, so the buffer goes up as it is. With no
       grid this frame
       the image is one zero cell, which the shader never reads -- taFog is
       called only on the `uFog & 1` branch. */
    if (t.fogGrid) memcpy(s->smallMap + SMALL_FOGOFF, t.fogGrid,
                          (size_t)fogW * fogH * 2);
    else           memset(s->smallMap + SMALL_FOGOFF, 0, 2);
    img_barrier(cb, s->pal, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->fog, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->smallStage, SMALL_PALOFF, s->pal, 256, 1);
    copy_rect(cb, s->smallStage, SMALL_FOGOFF, s->fog, fogW, fogH);
    img_barrier(cb, s->pal, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->fog, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    /* The two uniform blocks, at the std140 offsets the generated header
       prints. */
    memset(&ub, 0, sizeof ub);
    ub.f[0]  = t.gw;     ub.f[1]  = t.gh;        /* uGame        vec2 @0  */
    ub.f[2]  = t.zoom;                           /* uZoom       float @8  */
    ub.f[4]  = t.zoomCx; ub.f[5]  = t.zoomCy;    /* uZoomC       vec2 @16 */
    ub.f[6]  = t.depthScale;                     /* uDepthScale float @24 */
    ub.f[7]  = t.enc;                            /* uEnc        float @28 */
    ub.f[8]  = t.origX;  ub.f[9]  = t.origY;     /* uOrigin      vec2 @32 */
    ub.f[10] = t.tile0X; ub.f[11] = t.tile0Y;    /* uTile0       vec2 @40 */
    ub.f[12] = t.texelW; ub.f[13] = t.texelH;    /* uTexel       vec2 @48 */
    memcpy(s_umap + (size_t)slot * s_ustride, ub.f, UBLK_VS);

    memset(&ub, 0, sizeof ub);
    ub.i[0]  = restored;                         /* uRestored     int @0   */
    ub.f[2]  = t.hDimW;  ub.f[3]  = t.hDimH;     /* uHDim        vec2 @8   */
    ub.f[4]  = t.fogOrgX; ub.f[5] = t.fogOrgY;   /* uFogOrg      vec2 @16  */
    ub.f[6]  = t.fogCols; ub.f[7] = t.fogRows;   /* uFogDim      vec2 @24  */
    ub.i[8]  = t.fog;                            /* uFog          int @32  */
    ub.i[9]  = t.lambert;                        /* uLambert      int @36  */
    ub.f[12] = t.sun[0]; ub.f[13] = t.sun[1];
    ub.f[14] = t.sun[2];                         /* uSun         vec3 @48  */
    ub.f[15] = t.amb;                            /* uAmb        float @60  */
    ub.f[16] = t.norm;                           /* uNorm       float @64  */
    ub.i[17] = t.shadowOn;                       /* uShadowOn     int @68  */
    /* THE REST OF THE CAST-SHADOW BLOCK, and only when the map is on. With it
       off they stay ZERO, in a block nothing reads. The offsets are the
       generated header's. */
    if (t.shadowOn) {
        ub.f[20] = t.shadowSun[0];               /* uShadowSun   vec3 @80  */
        ub.f[21] = t.shadowSun[1];
        ub.f[22] = t.shadowSun[2];
        memcpy(&ub.f[24], t.shadowMat, 16 * sizeof(float));  /* mat4 @96   */
        ub.f[40] = t.shScale[0];                 /* uShScale     vec3 @160 */
        ub.f[41] = t.shScale[1];
        ub.f[42] = t.shScale[2];
        ub.f[43] = t.penumbra;                   /* uPenumbra   float @172 */
        ub.f[44] = t.shade;                      /* uShade      float @176 */
    }
    memcpy(s_umap + (size_t)slot * s_ustride + s_ublkF, ub.f, UBLK_FS);

    s_ncell = t.ncell;
    s_hGw = t.gw; s_hGh = t.gh;
    s_hVpL = t.vpL; s_hVpT = t.vpT; s_hVw = t.vw; s_hVh = t.vh;
    s_hScissorOn = t.scissorOn;

    /* THE A/B FRAME IS CLAIMED LAST, AFTER EVERY REASON NOT TO DRAW IS PAST. A
       frame claimed and then not drawn would have the seam capture a bare
       clear, and a diff against another build's capture would report every
       terrain pixel as differing: a pass failure that is really a capture
       failure. */
    s_abFrame = t.ab;
    s_drawThis = 1;
    return 1;

refuse:
    /* NOTHING IS DESTROYED HERE, AND THAT IS THE WHOLE POINT. This is the
       middle of a frame. The seam waited on fence[slot] ALONE (tagpu_vk.c), so
       every OTHER slot's submit is still executing against this pass's
       pipeline, its descriptor pool, its shared images and its per-slot
       buffers -- and `cb`, which this function has already recorded uploads
       and a depth clear into, is submitted whether this pass draws or not.
       Destroying any of it from here is a use-after-free on the FIRST
       refusal, not a rare one.

       So the pass stops drawing at once and OWES a teardown. The seam pays it
       at the top of a later frame, behind the vkDeviceWaitIdle that makes "no
       submit names these objects" a fact rather than a hope -- the same proof
       vk_down and vk_resize already use. The memory is still given back, which
       is what this path existed to do; it is given back where that is legal. */
    plog(d, "terr: slot %u would not take this frame's resources - the pass stops "
            "drawing and the seam tears it down", (unsigned)slot);
    refuse_job(d);
    s_state = ST_REFUSED;
    s_downOwed = 1;
    return 0;
}

void tagpu_vk_terr_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkBuffer bufs[2];
    VkDeviceSize offs[2] = { 0, 0 };

    if (s_state != ST_READY || !s_drawThis) return;
    s_drawThis = 0;

    /* THE RETIRE'S SECOND RULE, ASSERTED WHERE IT IS USED -- AND IT CANNOT FIRE
       TODAY, WHICH IS THE POINT AND ALSO THE LIMIT OF IT. The destroy is
       licensed by `pending == 0`, and a slot's bit is cleared at the top of its
       own `prepare` -- BEFORE `shared_bind` runs, and on paths that return
       without ever reaching it. What makes that safe is not the bit: it is that
       `record` only ever runs after a `prepare` that returned 1, and every such
       path passes through `shared_bind`, so the set in hand names the CURRENT
       images.
       THE LICENSING FACT IS THE `pending` BITMASK. This test does not replace
       that argument and cannot: `prepare` has exactly one `return 1` and
       `shared_bind` is on the straight-line path before it, so the comparison
       below is a tautology on every path that exists. It is a GUARD AGAINST A
       FUTURE `prepare` that returns 1 without binding, which is a thing a
       reader of this file might well add. Cheap, and it turns a silent sample
       of freed memory into a dropped frame. */
    if (slot >= TAGPU_VK_SLOTS ||
        s_slot[slot].boundBase != s_base.view ||
        s_slot[slot].boundHeight != (s_height.view ? s_height.view : s_base.view))
        return;
    {
        VkImageView sv = tagpu_vk_shadow_view(d->frame, slot);
        if (s_slot[slot].boundShadow != (sv ? sv : s_shView)) return;
    }

    /* NO Y FLIP, AND THE DEPTH RANGE, AND THEY ARE TWO SEPARATE QUESTIONS.
       This pass writes `gl_Position.y = p.y/uGame.y*2 - 1` on the engine's
       screen-space y, which grows DOWNWARD, so clip -1 is the game frame's top
       row and a POSITIVE height puts it on row 0, the framebuffer's top row.
       A NEGATIVE height would turn the frame over a second time (item 7).
       minDepth 0.5 / maxDepth 1.0 maps clip z in [0, 1]
       onto (z+1)/2 -- see item 4 of the file header. Every world pass maps
       depth the same way; without it every depth VALUE here would be on a
       different scale from theirs and the far plane terrain writes would not
       be the one the passes above it are tested against. */
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.minDepth = 0.5f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    terr_scissor(w, h);
    sc.offset.x = s_scX; sc.offset.y = s_scY;
    sc.extent.width = (uint32_t)s_scW; sc.extent.height = (uint32_t)s_scH;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1,
                            &s_slot[slot].dset, 0, NULL);
    bufs[0] = s_qbuf;                  /* the quad, per vertex     */
    bufs[1] = s_slot[slot].ibuf;       /* the cells, per instance  */
    vkCmdBindVertexBuffers(cb, 0, 2, bufs, offs);
    vkCmdDraw(cb, TAGPU_TERR_QUADV, (uint32_t)s_ncell, 0, 0);
}

int tagpu_vk_terr_ab_frame(void)
{
    int a = s_abFrame;
    s_abFrame = 0;
    return a;
}

void tagpu_vk_terr_down(const TAGPU_VKPASS* d)
{
    VkDevice dev = d->dev;
    uint32_t i;
    /* WHETHER THIS TEARDOWN IS THE ONE THE PASS ASKED FOR. Only the seam's
       `_down_paid` sets it, and only after its vkDeviceWaitIdle -- so a
       `vk_down` or a `vk_resize` that happens to run while a debt is
       outstanding tears the pass down WITHOUT consuming it, and leaves it
       ST_UNBUILT so it can come back on the next device. Reading `s_downOwed`
       here instead would let one transient refusal plus a window drag latch
       the pass at ST_REFUSED for the life of the process.

       THE DEBT ITSELF IS DISCHARGED BY EVERY TEARDOWN, paid or not, and that is
       a separate fact from the verdict: once this function has run there is
       nothing left to free, so an un-cleared flag would have the seam drain the
       device and call `_down_paid` on an already-dead pass the next time it
       looked -- which would latch ST_REFUSED by the back door and lose exactly
       what the two lines above win. */
    int owed = s_downPaying;
    s_downOwed = 0;
    /* NOTHING TO FREE, BUT THE VERDICT STILL STANDS. `owed` says the device
       refused this pass its resources, and that is a fact about the pass and
       not about whether the entry points resolved -- so it is latched here
       too, exactly as below. */
    if (!dev || !vkDestroyBuffer) { s_state = owed ? ST_REFUSED : ST_UNBUILT; return; }

    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        slot_free(d, &s_slot[i]);
        s_slot[i].dset = VK_NULL_HANDLE;   /* goes back with the pool below */
    }
    /* THE RESTORE JOB GOES BACK BEFORE THE IMAGE IT NAMES. The job holds a
       framebuffer over `s_rgbAtlas.view`, and a view still named by a live
       framebuffer may not be destroyed. The seam's vkDeviceWaitIdle is above
       both, so neither is still in a queue. */
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
    s_rjSerial = 0; s_rjPainted = 0; s_rjSrcView = VK_NULL_HANDLE;
    /* ...AND THE VERDICT DOES NOT SURVIVE THE DEVICE. `s_rjTried` is a fact
       about a device that refused, so a new one gets asked again -- the same
       reasoning as ST_UNBUILT below, and the restorer's own `up` latch is
       cleared by its `down` for the same reason. */
    s_rjTried = 0;
    s_saidRestored = 0;                 /* ...and so does the line it printed */
    kill_image(d, &s_rgbAtlas.img, &s_rgbAtlas.mem, &s_rgbAtlas.view);
    kill_image(d, &s_rgbAtlas.oldImg, &s_rgbAtlas.oldMem, &s_rgbAtlas.oldView);
    memset(&s_rgbAtlas, 0, sizeof s_rgbAtlas);
    kill_image(d, &s_base.img, &s_base.mem, &s_base.view);
    kill_image(d, &s_base.oldImg, &s_base.oldMem, &s_base.oldView);
    memset(&s_base, 0, sizeof s_base);
    s_basePal = 0;
    kill_image(d, &s_height.img, &s_height.mem, &s_height.view);
    kill_image(d, &s_height.oldImg, &s_height.oldMem, &s_height.oldView);
    memset(&s_height, 0, sizeof s_height);
    kill_image(d, &s_shImg, &s_shMem, &s_shView);
    s_shReady = 0;
    if (s_pipe)  { vkDestroyPipeline(dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dpool) { vkDestroyDescriptorPool(dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_samp)    { vkDestroySampler(dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_sampCmp) { vkDestroySampler(dev, s_sampCmp, NULL); s_sampCmp = VK_NULL_HANDLE; }
    if (s_qbuf)  { vkDestroyBuffer(dev, s_qbuf, NULL); s_qbuf = VK_NULL_HANDLE; }
    if (s_qmem)  { vkFreeMemory(dev, s_qmem, NULL); s_qmem = VK_NULL_HANDLE; }
    if (s_umap)  { vkUnmapMemory(dev, s_umem); s_umap = NULL; }
    if (s_ubuf)  { vkDestroyBuffer(dev, s_ubuf, NULL); s_ubuf = VK_NULL_HANDLE; }
    if (s_umem)  { vkFreeMemory(dev, s_umem, NULL); s_umem = VK_NULL_HANDLE; }
    s_drawThis = 0;
    s_abFrame = 0;
    s_ncell = 0;
    /* ST_UNBUILT and not ST_REFUSED: a pass brought down by a mode change or a
       cleared lever must be able to come back. THE ONE EXCEPTION IS THE
       TEARDOWN THIS PASS ASKED FOR: there the device refusing resources IS the
       reason, `prepare` has already latched ST_REFUSED, and clearing it here
       would have the pass rebuild and fail again on the very next frame. */
    s_state = owed ? ST_REFUSED : ST_UNBUILT;
}

/* 1 while this pass has stopped drawing and is waiting for the seam to drain
   the device and tear it down -- see `prepare`'s refusal path. */
int tagpu_vk_terr_down_owed(void)
{
    return s_downOwed;
}

/* THE SEAM'S OWN ENTRY POINT, called only after its vkDeviceWaitIdle. It is
   what makes `_down`'s ST_REFUSED latch apply to the owed teardown and to
   nothing else: `vk_down` and `vk_resize` go through plain `_down`, which
   discharges the debt (there is nothing left to free) but returns the pass
   ST_UNBUILT, so a pass refused once can try again on the device that replaces
   this one. Here the verdict stands, because here the device's refusal is
   still the reason. */
void tagpu_vk_terr_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_terr_down(d);          /* clears s_downOwed itself */
    s_downPaying = 0;
}
