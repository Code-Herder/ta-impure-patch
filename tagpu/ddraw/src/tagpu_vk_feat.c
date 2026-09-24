/* tagpu_vk_feat.c -- the feature pass (trees, rocks, splats, GAF wreckage)
   drawn by Vulkan. Contract: tagpu_vk_feat.h. A world pass that depth-tests.

   IT IS NOT A SECOND IMPLEMENTATION OF THE PASS. Everything arrives through
   `tagpu_feat_handover` (tagpu_feat.h): the vertices are the two arrays the
   gather filled, the uniforms are the view's numbers, the texels are CPU-side
   bytes -- the atlas's indices through tagpu_gaf.c's CPU mirror, which
   `atlas_paint` writes as the art's only destination, expanded here into the
   RGBA8 base atlas the shader samples -- and the shader is tagpu_feat.c's
   GLSL through tools/spirv-gen.py.

   ---- WHAT A DEPTH-TESTING PASS HAS TO ANSWER, AND WHERE EACH ANSWER IS ----

   1. DEPTH (the depth-range question, gpu-status §2.28).

      Every shader here writes a z already in [0, 1], and the viewport maps it
      onto [0.5, 1] with `minDepth = 0.5` and `maxDepth = 1.0` -- PIPELINE
      STATE, never a shader edit. The range is SHARED: the terrain, unit,
      effects and marker passes set the same one, and each depth test compares
      values the other passes wrote, so a pass that changed its range alone
      would settle z-fights differently from the rest. It needs no extension.
      `VK_EXT_depth_clip_control` is the answer only if a shader is ever found
      writing a z below 0 -- Vulkan clips those. These do not:
      `clamp(1.0 - aPos.z/uDepthScale, 0.0, 1.0)` is the whole of it.
      The attachment's FORMAT is the seam's (tagpu_vk.c `vk_depth_format`):
      24-bit fixed point, D24_UNORM_S8_UINT or X8_D24_UNORM_PACK32.

   2. TWO DEPTH-WRITE MODES, AND SO TWO PIPELINES. Shadows are drawn with
      depth writes off and bodies with them on -- shadows are ground decals and
      must occlude nothing -- and that is pipeline state in Vulkan.
      Two pipelines off one layout is the answer that needs no extension;
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE is Vulkan 1.3 or an extension and buys
      one object.

   3. THE SCISSOR is the world viewport, the engine's own rectangle and not
      its vertical mirror (`offset.y = H - (vpT + vh)`), because of item 4:
      with a positive viewport height row 0 of the Vulkan image is the game
      frame's top row, so `offset.y` is just the viewport top. Getting it
      wrong shows up as the world clipped against the wrong edge rather than
      as anything subtle.

   4. NO Y FLIP. This pass writes `gl_Position.y = p.y/uGame.y*2 - 1` on the
      engine's screen-space y, which grows DOWNWARD, so clip +1 is the BOTTOM of
      the game frame. Nothing downstream turns the frame over, so a negative
      viewport height would present the world upside down. The viewport is
      positive and clip -1 lands on row 0, which is the game's top row.
      (VK_KHR_maintenance1 is needed only for a negative height, so this pass
      does not require it; `tagpu_vk_gui.c`, `_fps.c` and `_scaffold.c` do,
      because their shaders are y-UP and their flip is correct.)

   5. BLENDING is ONE, ONE_MINUS_SRC_ALPHA -- the fragment shader writes
      premultiplied colour (`frag = vec4(rgb * a, a)`) -- on the alpha factors
      as well as the colour ones, so both pairs are set here.

   ---- THE UPLOADS, AND WHY ONE OF THEM IS SHARED ----

   §2.28 established one image and one staging buffer PER FRAME SLOT, on the
   one-line invariant that the seam has just waited on `fence[slot]`, so nothing
   of ours is in flight for that slot: the CPU may write its staging buffer, the
   copy into its image has no prior access to order against, and the descriptor
   set naming it may be rewritten. It also named the cheaper design -- ONE image
   shared by every slot, behind a write-after-read barrier -- and said to reach
   for it when a pass needed the memory back.

   THIS IS THAT PASS, and the split is by size:

     the BASE ATLAS 2048 x 2048 RGBA8 = 16 MB. Per-slot it would be 64 MB of
                    device-local on a four-image swapchain, in a 32-bit address
                    space whose largest free block is the number this phase
                    spends its budget measuring. So it is ONE image, and the
                    barrier at the top of an upload -- FRAGMENT_SHADER /
                    SHADER_READ -> TRANSFER / TRANSFER_WRITE -- orders the copy
                    after every earlier frame's sampling of it. That is legal
                    across submits because a barrier's first synchronisation
                    scope includes everything submitted to the queue before it,
                    and a write-after-read hazard needs only an execution
                    dependency; the access masks are there for the layout
                    transition, which is a write. It is sent only when the
                    mirror's serial says the bytes moved, and only the tiles
                    the atlas's dirty map says were written.

     the FOG GRID   a few KB. Per-slot, because at that size the one-line
                    invariant is worth more than the memory and a dimension
                    change (every time the view moves far enough) is then free:
                    the slot is rebuilt at the moment we are handed it, which is
                    the moment we own it.

   The base atlas's STAGING buffer is per-slot either way and cannot be
   anything else: a barrier orders GPU work and the hazard there is a CPU
   write. It is tagpu_vk_stage.h's bounded upload: at most
   TAGPU_VK_STAGE_CAP a slot, allocated on the frame that uploads and given
   back at that slot's next `prepare` with nothing to send -- the same fence,
   one turn of the slots later -- so a settled scene holds none of it. A page
   larger than the cap goes out in bands, complete before the frame draws.

   AND NOTHING IS KEPT ONCE THERE IS NOTHING TO DRAW, exactly as §2.28's
   scaffold: a frame the gather handed nothing over gives that slot back.

   ---- WHAT IT DOES NOT DO ----

   IT DOES NOT SAMPLE AN UNPAINTED RESTORE. Classic++'s restored atlas is
   painted HERE: when the producer publishes a restore list (tagpu_feat.h
   `restoreFrames`), `restore_want` feeds this lane's own restore job
   (tagpu_vk_restore.h), which paints the RGBA8 twin at binding 40 from the
   base atlas. Until the job has painted a frame, a frame with `uRestored`
   1 is drawn from the base atlas with `uRestored` 0 (`prepare`), and the
   flag is decided after the upload and the job have run, so a frame whose
   upload or job took the twin away draws the base atlas too. That is not
   latched: it clears by itself within a few frames of the restorer
   starting.

   IT KNOWS NOTHING ABOUT A WINDOW. Everything arrives in TAGPU_VKPASS.
   A PASS READS NO ENGINE STATE: every value comes from the gather's
   hand-over, so this file is not on thread-split.allow and must never be. */

#include "tagpu_vk_pass.h"
#include "tagpu_vk_restore.h"
#include "tagpu_vk_stage.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_feat.h"
#include "tagpu_feat.h"
#include "tagpu_gaf.h"                     /* tagpu_gaf_rects_due              */
#include "tagpu_pal.h"                     /* tagpu_pal_expand                 */
#include "spirv/tagpu_feat.spv.h"

#define VST TAGPU_FEAT_VST                 /* floats per vertex, the gather's  */
#define UBLK 32                            /* bytes in each std140 block below */

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
static int s_abFrame;
static int s_saidRestored;                 /* the Classic++ refusal, said once  */
static int s_saidNoMirror;                 /* ...and the mirror's, likewise     */

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipeShadow, s_pipeBody;
static VkDescriptorPool      s_dpool;
static VkSampler             s_samp;

static VkBuffer       s_ubuf;
static VkDeviceMemory s_umem;
static unsigned char* s_umap;
static VkDeviceSize   s_ustride;           /* two blocks, each device-aligned  */
static VkDeviceSize   s_ublock;

/* ---- THE SHARED IMAGES. One of each for every slot; see the file header. -- */
static int            s_atDim;             /* the square they were created for */

/* CLASSIC++'s RESTORED TWIN. A second image of the same square in RGBA8,
   built beside the base atlas; binding 40 names it. When it cannot be built
   the binding falls back to the base atlas's view and `uRestored` stays 0 --
   a valid descriptor is required for the set to be bound at all, so the
   fallback is not optional.

   THE RESTORE JOB IS ITS ONLY WRITER, and `s_arHave` alone says whether this
   is a picture. */
static VkImage        s_arImg;
static VkDeviceMemory s_arMem;
static VkImageView    s_arView;
static int            s_arHave;

/* THE BASE ATLAS: the same square in RGBA8, every texel its index's colour
   through the hand-over's palette -- the engine's table -- and alpha 0 at its
   frame's key (tagpu_pal_expand). It is what the features are drawn from, the
   twin over it where that has been painted, and it is the restorer's source.
   Binding 42. `s_bSerial` and `s_bPal` are the mirror and palette serials it
   holds; a palette move re-sends the whole page, a paint only the tiles the
   atlas's dirty map says it wrote. */
static VkImage        s_bImg;
static VkDeviceMemory s_bMem;
static VkImageView    s_bView;
static int            s_bHave;
static unsigned       s_bSerial, s_bPal;
/* THE REPACK'S MOVE THE BASE ATLAS HAS TAKEN (its `atlasMoveSerial`), which
   is what stops a move from being applied twice: a frame that moved the cells
   and then could not send the paints keeps `s_bSerial`, so the next frame
   sends them without moving again. The buffer the move goes through is
   `s_move`. */
static unsigned        s_bMove;
static TAGPU_VKMOVEBUF s_move;
/* ---- THE RESTORE THIS LANE RUNS FOR ITSELF --------------------------------
   `s_rjob` paints `s_arImg` from `s_bImg` when the producer publishes a frame
   list instead of a mirror. Three pieces of state make it a CURSOR rather than
   a serial, which is what a lazy queue needs:

     s_rjGen    the producer's generation this job belongs to. A new one is a
                discontinuity a cursor cannot survive -- a recycle, a repack
                whose moves are not published, the atlas laid out afresh -- so
                the job is rebuilt and the cursor goes back to 0. A repack
                whose moves are published is carried instead (`twin_move`).
     s_rjPal    the engine palette serial the job was built with. The
                generation does NOT move with the palette, and the job keeps
                the palette it was made with, so a move of the serial is a new
                job too: the base atlas is re-sent in the new colours
                (`base_upload`) and the twin is blanked and repainted from it,
                never left in the old ones beside a base in the new.
     s_rjTaken  how many of that generation's frames are already queued here.
                The producer retains the whole array, so a frame on which this
                pass took nothing costs nothing: the next one takes more.
     s_rjSrcView  the view the job reads. The atlas image can be re-created
                under a live job (a dimension change), and a job holding the
                old view would paint from freed memory -- the same hazard, and
                the same guard, as the terrain consumer's. */
static TAGPU_VKRJOB*  s_rjob;
static unsigned       s_rjGen, s_rjPal;
static int            s_rjTaken;
static int            s_rjPainted;
static int            s_rjTried;           /* the device refused; do not ask again */
static VkImageView    s_rjSrcView;

/* what `record` was left to draw */
static int s_nShadow, s_nBody;
static int s_scX, s_scY, s_scW, s_scH;     /* the scissor, in Vulkan framebuffer px */
/* The scissor's INPUTS, kept as numbers rather than as a copy of the hand-over.
   TAGPU_FEATHAND is full of pointers into the gather's own frame memory, and a
   static holding those past the frame they were handed over on is a dangling
   read waiting for someone to add a line that follows one. */
static float s_hGw, s_hGh;
static int   s_hVpL, s_hVpT, s_hVw, s_hVh, s_hScissorOn;

/* ---- one of these per frame slot, and every field of it is ours for the
   duration of the `prepare`/`record` pair we are handed that slot on. ---- */
typedef struct {
    VkBuffer        vbuf;                  /* the frame's vertices             */
    VkDeviceMemory  vmem;
    unsigned char*  vmap;
    VkDeviceSize    vcap;

    TAGPU_VKSTAGE   stage;                 /* the base atlas's upload, when due */

    VkImage         fog;                   /* the fog grid, per slot           */
    VkDeviceMemory  fogMem;
    VkImageView     fogView;
    VkBuffer        smallStage;
    VkDeviceMemory  smallMem;
    unsigned char*  smallMap;
    int             fogW, fogH;            /* what the fog image is sized for  */

    VkDescriptorSet dset;
} SLOT;
static SLOT s_slot[TAGPU_VK_SLOTS];

/* the fog grid is RG8 and the widest the wide-fog builder produces is well
   inside this; a bound here is what keeps a handed-over number from sizing an
   allocation (the scaffold's rule, tagpu_vk_scaffold.c) */
#define FOG_MAXDIM 1024
/* THE FOG GRID'S STAGING BUFFER (cols x rows RG8, a few KB and a different
   size whenever the view walks far enough for the grid to be re-laid) is
   rebuilt with the fog image, by the same call, so the buffer and the image it
   feeds can never disagree about the size. */

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
    if (type < 0) { plog(d, "feat: no memory type for a buffer"); return 0; }
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

/* `usage` IS THE CALLER'S BECAUSE THE RESTORED TWIN IS A RENDER TARGET. The
   base atlas is only ever copied into and sampled, but the twin is what this
   lane's own restorer PAINTS, and a Vulkan image may only be a colour
   attachment if it was created saying so. Nothing infers it from the format --
   the destination's usage is a promise made at creation and tagpu_vk_restore.h
   asks for it by name. */
/* WHAT EACH OF THIS PASS'S IMAGES IS FOR. The restored twin carries
   COLOR_ATTACHMENT because this lane's restorer paints into it;
   it costs nothing when nothing restores, and an image created without it
   could not be lent to the restorer at all. It is a transfer source as well,
   because a repack's move carries its cells (`twin_move`). */
#define IMG_SAMPLED  (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
#define IMG_RESTORED (IMG_SAMPLED | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | \
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
/* the base atlas is also a transfer source, for a repack's move */
#define IMG_BASE     (IMG_SAMPLED | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)

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
    if (type < 0) { plog(d, "feat: no device-local memory for an image"); return 0; }
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
    kill_image(d, &s->fog, &s->fogMem, &s->fogView);
    s->fogW = s->fogH = 0;
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
    if (!mk_buffer(d, (VkDeviceSize)w * h * 2,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->smallStage, &s->smallMem, &s->smallMap)) return 0;
    memset(&ii, 0, sizeof ii); memset(&wr, 0, sizeof wr);
    ii.sampler = s_samp; ii.imageView = s->fogView;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = s->dset; wr.dstBinding = 41; wr.descriptorCount = 1;
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

/* ---- build -------------------------------------------------------------- */

static int build_sampler(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties fp;
    int i;
    /* the two formats this pass samples, asked for rather than assumed: the
       fog grid's and the atlases' */
    static const VkFormat need[2] = { VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
    for (i = 0; i < 2; i++) {
        vkGetPhysicalDeviceFormatProperties(d->pd, need[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            plog(d, "feat: this device cannot sample format %d - the pass stays down",
                 (int)need[i]);
            return 0;
        }
    }
    /* NEAREST AND CLAMP_TO_EDGE, one sampler for every binding. The base
       atlas is sampled as the engine blits a sprite, one texel per pixel and
       never a blend of two -- which is also what makes its alpha an exact
       hole test -- and the fog grid's corner masks are fetched, not filtered. */
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
    VkDescriptorSetLayoutBinding b[5];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[TAGPU_FEAT_NATTR];
    static const struct { int loc, n, off; } at[TAGPU_FEAT_NATTR] = TAGPU_FEAT_ATTRS;
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
    VkResult r;
    int i, ok = 0;

    /* THE BINDINGS ARE THE GENERATOR'S, NOT THIS FILE'S: tools/spirv-gen.py
       allocates them BY STAGE, so set 0 binding 0 is the vertex stage's uniform
       block, 32 the fragment stage's, and 40.. its samplers in declaration
       order -- uAtlasRGB, uFogGrid, uBase. The std140 offsets are printed in
       the generated header and are the contract for what goes into those
       buffers. */
    memset(b, 0, sizeof b);
    b[0].binding = 0;  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 32; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 2; i < 5; i++) {
        b[i].binding = (uint32_t)(40 + (i - 2));
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    dli.bindingCount = 5; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS) return 0;

    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) return 0;

    vs = mk_module(d, tagpu_spv_tagpu_feat_VS,
                   sizeof tagpu_spv_tagpu_feat_VS / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_tagpu_feat_FS,
                   sizeof tagpu_spv_tagpu_feat_FS / sizeof(uint32_t));
    if (!vs || !fs) { plog(d, "feat: a shader module was refused"); goto out; }

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    /* ONE TABLE: tagpu_feat.h TAGPU_FEAT_ATTRS is the layout the gather writes
       the vertices in, so a layout change cannot reach the writer and miss
       this reader. */
    memset(&vb, 0, sizeof vb);
    vb.binding = 0; vb.stride = VST * sizeof(float);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(va, 0, sizeof va);
    for (i = 0; i < TAGPU_FEAT_NATTR; i++) {
        static const VkFormat f[4] = { VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                       VK_FORMAT_R32G32B32_SFLOAT,
                                       VK_FORMAT_R32G32B32A32_SFLOAT };
        va[i].location = (uint32_t)at[i].loc;
        va[i].binding = 0;
        va[i].format = f[at[i].n - 1];
        va[i].offset = (uint32_t)at[i].off;
    }
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = TAGPU_FEAT_NATTR;
    vi.pVertexAttributeDescriptions = va;

    /* A LIST: tagpu_feat.c's emit_frame writes six vertices a quad. */
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;     /* both dynamic, set per frame */

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* NO CULLING. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* LESS, the compare every depth-testing world pass uses (tagpu_vk_terr.c,
       tagpu_vk_unit.c, tagpu_vk_fx.c). The WRITE half is what the two
       pipelines differ in. */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    memset(&cba, 0, sizeof cba);
    /* ONE, ONE_MINUS_SRC_ALPHA on colour and alpha alike -- the fragment
       shader writes premultiplied colour; item 5 of the file header. */
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

    /* SHADOWS TEST AND DO NOT WRITE: they are ground decals, so a feature's
       body is not fighting its own shadow and nothing is occluded by one. */
    ds.depthWriteEnable = VK_FALSE;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeShadow);
    if (r != VK_SUCCESS) { plog(d, "feat: the shadow pipeline was refused (%d)", (int)r); goto out; }
    /* BODIES WRITE: this is what occludes units behind trees. */
    ds.depthWriteEnable = VK_TRUE;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeBody);
    if (r != VK_SUCCESS) { plog(d, "feat: the body pipeline was refused (%d)", (int)r); goto out; }
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
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[1].descriptorCount = d->slots * 3;
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
       written when there is an image to name: 40 and 42 by `atlas_build`,
       41 by `slot_fog`. */
    for (i = 0; i < d->slots; i++) {
        VkDescriptorBufferInfo bi[2];
        VkWriteDescriptorSet w[2];
        s_slot[i].dset = sets[i];
        memset(bi, 0, sizeof bi); memset(w, 0, sizeof w);
        bi[0].buffer = s_ubuf; bi[0].offset = i * s_ustride;              bi[0].range = UBLK;
        bi[1].buffer = s_ubuf; bi[1].offset = i * s_ustride + s_ublock;   bi[1].range = UBLK;
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = sets[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bi[0];
        w[1] = w[0]; w[1].dstBinding = 32; w[1].pBufferInfo = &bi[1];
        vkUpdateDescriptorSets(d->dev, 2, w, 0, NULL);
    }
    return 1;
}

/* The shared images, and the two descriptor bindings that name them. */
static int atlas_build(const TAGPU_VKPASS* d, int dim)
{
    uint32_t i;
    VkDescriptorImageInfo irgb;
    if (s_bImg && s_atDim == dim) return 1;
    kill_image(d, &s_bImg, &s_bMem, &s_bView);
    s_atDim = 0;
    s_bHave = 0; s_bSerial = 0; s_bPal = 0; s_bMove = 0;
    tagpu_feat_atlas_ack(0, 0);            /* this copy holds nothing yet */
    /* the base atlas is what the features are drawn from and what the
       restorer reads, so without it the pass does not draw */
    if (!mk_image(d, dim, dim, VK_FORMAT_R8G8B8A8_UNORM, IMG_BASE,
                  &s_bImg, &s_bMem, &s_bView)) {
        kill_image(d, &s_bImg, &s_bMem, &s_bView);
        plog(d, "feat: no %d MB device image for the base atlas", (dim * dim * 4) >> 20);
        return 0;
    }
    s_atDim = dim;
    /* THE RESTORED TWIN'S IMAGE, and a failure here is not fatal: binding 40
       falls back to the base atlas's view below and `uRestored` stays 0.
       16 MB at the shipped 2048 square. */
    kill_image(d, &s_arImg, &s_arMem, &s_arView);
    s_arHave = 0;
    if (!mk_image(d, dim, dim, VK_FORMAT_R8G8B8A8_UNORM, IMG_RESTORED,
                  &s_arImg, &s_arMem, &s_arView)) {
        /* `mk_image` CAN FAIL AFTER vkCreateImage AND vkAllocateMemory SUCCEEDED
           -- no device-local memory type, a failed bind, a failed view.
           Nulling the handles here would lose the image while leaving
           `s_arMem` set, so the next kill_image would vkFreeMemory memory that
           still had an image bound to it. kill_image is what tagpu_vk_terr.c's
           shared_resize does for the identical case. */
        kill_image(d, &s_arImg, &s_arMem, &s_arView);
        plog(d, "feat: no %d MB device image for the Classic++ restored twin - "
                "the features draw the base atlas",
             (dim * dim * 4) >> 20);
    }
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        VkDescriptorImageInfo ib;
        VkWriteDescriptorSet wr[2];
        if (!s_slot[i].dset) continue;
        memset(&ib, 0, sizeof ib); memset(wr, 0, sizeof wr);
        /* 42 is uBase */
        ib.sampler = s_samp; ib.imageView = s_bView;
        ib.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = s_slot[i].dset; wr[0].dstBinding = 42; wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[0].pImageInfo = &ib;
        /* BINDING 40 IS uAtlasRGB, AND IT NAMES THE RESTORED TWIN'S OWN
           IMAGE: the branch that samples it is reachable, so the binding has
           to be the real thing. When the image could not be created it is the
           base atlas's view as a placeholder -- a descriptor must be VALID for
           the set to bind, and `uRestored` 0 then keeps the branch
           unreachable. */
        irgb = ib;
        if (s_arView) irgb.imageView = s_arView;
        wr[1] = wr[0]; wr[1].dstBinding = 40; wr[1].pImageInfo = &irgb;
        vkUpdateDescriptorSets(d->dev, 2, wr, 0, NULL);
    }
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties props;
    VkDeviceSize ualign;

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "feat: %u frame slots is outside what this pass carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    /* THE FIRST PASS THAT DEPTH-TESTS REFUSES RATHER THAN DRAWS UNTESTED. */
    if (d->dfmt == VK_FORMAT_UNDEFINED) {
        plog(d, "feat: the seam's render pass carries no depth attachment, and "
                "this pass tests depth - it stays down rather than drawing "
                "features that occlude nothing");
        return 0;
    }
    if (!resolve(d)) { plog(d, "feat: an entry point is missing"); return 0; }

    vkGetPhysicalDeviceProperties(d->pd, &props);
    /* TWO BLOCKS PER SLOT, EACH AT AN OFFSET THE DEVICE ACCEPTS.
       `minUniformBufferOffsetAlignment` is 16 on some devices and 256 on
       others, and a bound buffer offset that is not a multiple of it is
       undefined behaviour rather than a slow path. Each block is 32 bytes
       (the generated header prints the std140 offsets), so the stride is the
       larger of the two, twice. */
    ualign = props.limits.minUniformBufferOffsetAlignment;
    if (ualign < UBLK) ualign = UBLK;
    s_ublock = ualign;
    s_ustride = ualign * 2;

    if (!mk_buffer(d, s_ustride * d->slots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_ubuf, &s_umem, &s_umap)) return 0;
    if (!build_sampler(d)) return 0;
    if (!build_pipelines(d)) return 0;
    if (!build_descriptors(d)) return 0;

    plog(d, "feat: the Vulkan edition is up - %u frame slots, uniform stride %u, "
            "depth format %d",
         (unsigned)d->slots, (unsigned)s_ustride, (int)d->dfmt);
    return 1;
}

/* ---- the frame ---------------------------------------------------------- */

/* THE FRAMES PAST THE CURSOR onto the job. THE CURSOR ADVANCES BY WHAT WAS
   OFFERED, NOT BY WHAT WAS TAKEN. `tagpu_rcore_job_add` SKIPS a frame it can
   never queue -- a degenerate rect, or one larger than the activation slot
   even after the wrap demote -- and returns only the count it queued, so
   advancing by that would re-offer the tail of the list on every frame for
   the life of the atlas: one duplicate restore per refused frame, for ever.
   A BLANK FRAME (w and h 0) is one a repack dropped (tagpu_feat.h), skipped
   the same way and not a refusal. A refused frame draws the base atlas until
   the next generation, which is what the log line says.
   THE EXCEPTION IS A WHOLE-CALL FAILURE (nothing taken of frames that were
   not all blank): that is the queue's own realloc failing, which is transient
   and already logged by the core, so the cursor stays where it is and the
   next frame offers them again. */
static void restore_take(const TAGPU_VKPASS* d, const TAGPU_FEATHAND* h)
{
    const int n = h->restoreN - s_rjTaken;
    const TAGPU_RGLSL_FRAME* f;
    int i, blank = 0, took;
    if (n <= 0 || s_rjTaken < 0) return;
    f = h->restoreFrames + s_rjTaken;
    for (i = 0; i < n; i++) if (f[i].w <= 0 || f[i].h <= 0) blank++;
    took = blank < n ? tagpu_vk_restore_job_add(s_rjob, f, n) : 0;
    if (took == 0 && blank < n) return;
    s_rjTaken += n;
    if (took < n - blank)
        plog(d, "feat: %d of %d new restore frames were refused by the "
                "restorer (degenerate or larger than a slot) - they draw "
                "the base atlas until the next generation", n - blank - took, n - blank);
}

/* THE RESTORE, FED FROM THE PUBLISHED LIST.

   Called once a frame after the base atlas's upload, because the FILL pass
   reads the base and a restore issued before this frame's copy would paint
   undefined texels over whatever the base had not received yet.

   IT IS A CURSOR, AND THE GENERATION IS THE ONLY THING THAT RESTARTS IT. A
   feature atlas is a lazy queue: the producer appends one frame per miss
   for the life of the atlas, so the steady state here is "add the few frames
   past my cursor and advance it". Everything that could make the cursor a lie
   -- the destination blanking, the rects moving with no moves to follow --
   arrives as a new generation, and then the job is rebuilt from index 0. A
   repack that publishes its moves has already been carried into the job and
   the twin by `base_upload` (`twin_move`), before this runs. */
static void restore_want(const TAGPU_VKPASS* d, const TAGPU_FEATHAND* h)
{
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
               feed this twin again. Leaving `s_arHave` set would have this
               pass draw a frozen twin of an atlas that goes on changing, which
               is a silent divergence rather than a stand-down. */
            s_arHave = 0;
        }
        return;
    }
    /* THE SOURCE MOVED UNDER A LIVE JOB. `base_upload` refuses a dimension
       change and `atlas_build` is what re-creates the image, so this is the
       narrow case rather than the common one -- and it is the one that reads
       from a destroyed view if nothing checks. */
    if (s_rjob && s_rjSrcView && s_bView && s_rjSrcView != s_bView) {
        plog(d, "feat: the base atlas moved under a live restore - dropping it "
                "and starting over on the new one");
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
        s_rjSrcView = VK_NULL_HANDLE;
        s_arHave = 0;                      /* what it holds is the old layout's */
    }
    if (s_rjob && s_rjGen == h->restoreGen && s_rjPal == h->palSerial) {
        int painted = tagpu_vk_restore_job_painted(s_rjob);
        /* ONE PAINTED FRAME IS WHAT MAKES THIS A PICTURE, and `s_arHave` is
           what `prepare`'s restored check reads. */
        if (painted > 0) s_arHave = 1;
        if (painted != s_rjPainted) {
            s_rjPainted = painted;
            if (tagpu_vk_restore_job_idle(s_rjob))
                plog(d, "feat: restored atlas painted here - %d frames of "
                        "generation %u, no mirror and no read-back",
                     painted, h->restoreGen);
        }
        if (tagpu_vk_restore_job_failed(s_rjob)) {
            /* "WHAT IT PAINTED STANDS" IS ONLY TRUE UNTIL THE RECTS MOVE, and
               they will: the next generation re-lays the atlas and this twin
               then holds the OLD layout's texels at every new rect. So the
               twin stops being a picture at the moment the restore is
               abandoned, not at the moment it looks wrong. */
            plog(d, "feat: the restore failed on this lane - the twin stops being "
                    "drawn from, because the next atlas layout would sample it "
                    "at rects it was never painted for");
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0;
            s_rjTried = 1;                 /* it will fail the same way again  */
            s_arHave = 0;
            return;
        }
        /* THE STEADY STATE: whatever the producer has appended since. */
        restore_take(d, h);
        return;
    }
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; s_rjTaken = 0; }
    /* PAST THIS POINT NO JOB PAINTS THIS GENERATION, so the twin is not a
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
    /* EVERY GENERATION BLANKS. Nothing the restore reads moves in play -- its
       source is the base atlas, built from the engine's table, and the Gamma
       factor is applied after it, to the finished world image (tagpu_pal.h) --
       and a move of the table itself is a new job here (`s_rjPal`), so no job
       is a recolour of the last one. */
    s_rjob = tagpu_vk_restore_job_new(d, "feat", 1, 0, 0,
                                      s_bImg, s_bView, s_atDim, s_atDim, 1,
                                      h->pal,
                                      s_arImg, s_arView, s_atDim, s_atDim);
    if (!s_rjob) { s_rjTried = 1; return; }   /* the reason is in the log      */
    s_rjTaken = 0;
    restore_take(d, h);
    s_rjGen = h->restoreGen;
    s_rjPal = h->palSerial;
    s_rjSrcView = s_bView;
    s_rjPainted = 0;
    plog(d, "feat: restoring the atlas HERE - %d of %d frames over %dx%d, "
            "generation %u", s_rjTaken, h->restoreN, s_atDim, s_atDim,
         h->restoreGen);
}

/* A REFUSED PASS GIVES ITS RESTORE JOB BACK AT ONCE, not at the teardown the
   seam pays frames later. A refusal can be a banded upload the device failed
   partway (tagpu_vk_stage.c `upload`), which leaves the base atlas half
   written and in a transfer layout, and the job's FILL samples that image
   through a descriptor that says SHADER_READ_ONLY. The seam records the
   restorer's slice after every pass's `prepare`, so a job given back here is
   never recorded again: nothing samples an image this pass has disowned. The
   free is legal mid-frame -- `tagpu_vk_restore_job_free` retires what a
   submitted buffer still names -- and the twin it painted stops being a
   picture with it. */
static void refuse_job(const TAGPU_VKPASS* d)
{
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
    s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
    s_rjSrcView = VK_NULL_HANDLE;
    s_arHave = 0;
}

/* ---- THE RESTORED TWIN FOLLOWS A REPACK ----------------------------------
   The moves by the old cell origin, which is how a frame names its cell as
   well: (dx - border, dy - border), the move's (ox, oy) (tagpu_gaf.h). Open
   addressing over twice the atlas's entries, so a probe always ends. */
#define MOVEHASH 8192                      /* a power of two                  */
static unsigned short s_mvHash[MOVEHASH];  /* move index + 1, 0 empty; render thread */

static unsigned mv_slot(unsigned x, unsigned y)
{
    return (x * 73856093u ^ y * 19349663u) & (MOVEHASH - 1);
}

/* a queued frame to its entry's new rect: 1, or 0 when no move names its cell
   -- the repack dropped the entry. The size is compared as well as the
   origin, so a frame can only follow the cell it was built for. */
static int twin_map(void* ctx, TAGPU_RGLSL_FRAME* f)
{
    const TAGPU_GAFMOVE* m = (const TAGPU_GAFMOVE*)ctx;
    const int ox = f->dx - f->border, oy = f->dy - f->border;
    const int cw = f->w + 2 * f->border + f->padR, ch = f->h + 2 * f->border + f->padB;
    unsigned s;
    if (ox < 0 || oy < 0) return 0;
    for (s = mv_slot((unsigned)ox, (unsigned)oy); s_mvHash[s]; s = (s + 1) & (MOVEHASH - 1)) {
        const TAGPU_GAFMOVE* c = &m[s_mvHash[s] - 1];
        if (c->ox == ox && c->oy == oy && c->w == cw && c->h == ch) {
            f->ax = f->dx = c->nx + f->border;
            f->ay = f->dy = c->ny + f->border;
            return 1;
        }
    }
    return 0;
}

/* The job goes, and with it every claim the twin makes: `s_arHave` drops, so
   this frame -- whose `uRestored` is decided after the upload -- draws the
   base atlas, and the next `restore_want` blanks the twin and restores from
   the list's first frame. */
static void twin_drop(const TAGPU_VKPASS* d, const char* why)
{
    if (!s_rjob) return;
    plog(d, "feat: %s - the restored twin is blanked and restored again from "
            "the list's first frame", why);
    tagpu_vk_restore_job_free(d, s_rjob);
    s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
    s_rjSrcView = VK_NULL_HANDLE;
    s_arHave = 0;
}

/* THE REPACK'S MOVE, CARRIED INTO THE RESTORED TWIN, so that what was
   restored stays restored, at its new rect, and only what never was is
   restored after. Called once per move, straight after the base atlas's move
   is recorded into `cb`: the job's frames are moved by the same list -- the
   producer has rewritten its published list in place (tagpu_gaf.h
   `rlistGen`) -- and the twin's cells are moved on the device, with every
   texel no cell lands on cleared (tagpu_vk_stage_move_twin).
   WHY THE TWIN CANNOT DISAGREE WITH THE BASE ATLAS: both images move by one
   list in one command buffer; the job's frames are rewritten by that list
   before the restorer records its next slice, which it does after every
   prepare; and the batch in flight is taken back out -- so after the move no
   FILL reads the base atlas, and no OUT writes the twin, at a rect of the old
   layout. A twin the job has not drawn into yet is not moved: the job's first
   draw clears it.
   Anything that stops the twin following -- more moves than the table holds,
   a job that cannot take its batch back, a twin the device cannot move --
   drops the job, the rule for any discontinuity. */
static void twin_move(const TAGPU_VKPASS* d, VkCommandBuffer cb, const TAGPU_FEATHAND* h)
{
    int kept = 0, requeued = 0, dropped = 0, carried = 0, i;
    if (!s_rjob) return;
    if (h->atlasMoveN < 0 || h->atlasMoveN > MOVEHASH / 2) {
        twin_drop(d, "a repack moved more cells than the twin's table holds");
        return;
    }
    memset(s_mvHash, 0, sizeof s_mvHash);
    for (i = 0; i < h->atlasMoveN; i++) {
        unsigned s = mv_slot(h->atlasMoves[i].ox, h->atlasMoves[i].oy);
        while (s_mvHash[s]) s = (s + 1) & (MOVEHASH - 1);
        s_mvHash[s] = (unsigned short)(i + 1);
    }
    if (!tagpu_vk_restore_job_remap(s_rjob, twin_map, (void*)h->atlasMoves,
                                    &kept, &requeued, &dropped)) {
        twin_drop(d, "the restore job could not take its batch back after a repack");
        return;
    }
    if (tagpu_vk_restore_job_dst_live(s_rjob)) {
        if (!tagpu_vk_stage_move_twin(d, cb, &s_move, s_arImg, h->atlasDim,
                                      h->atlasMoves, h->atlasMoveN)) {
            twin_drop(d, "the restored twin could not be moved with the repack");
            return;
        }
        carried = h->atlasMoveN;
    }
    plog(d, "feat: the restored twin followed the repack - %d cells carried on the "
            "device, %d queued frames moved with them, %d in flight taken back and "
            "re-queued, %d dropped with their entries", carried, kept, requeued, dropped);
}

/* THE BASE ATLAS, when the mirror or the engine's table moved. Which of two
   ways is decided by what this copy holds, never by how much is due:

     * THE COPY IS KEPT -- current up to `s_bSerial`, in this palette, and not
       behind a whole-page write: the tiles the atlas's dirty map says were
       written since (tagpu_gaf_rects_due), after a repack's move on the device
       when one is due (tagpu_vk_stage_move). IN THIS FRAME'S COMMAND BUFFER OR
       NOT AT ALL. The atlas's allowance (tagpu_gaf.h `budget`) keeps what is
       due within one slot's staging, and a slot that could not map it skips
       the frame and sends it on the next: nothing waits.
     * THE COPY IS NOT KEPT -- the first upload after `atlas_build` (a new
       pass: the session's first level, a teardown, a resize), a move of the
       engine's table, the atlas's whole page written (made, lost, a new
       level, its mirror armed): the used page, banded through the slot's
       staging, which waits on the device. None of those is a frame of play,
       and the line names the cause. Play reaches it one way, a repack this
       copy cannot move because the device refused the move buffer, and the
       line says so.

   Either way the atlas is then told what the copy holds
   (`tagpu_feat_atlas_ack`), which is what its allowance counts from.
   1 sent or nothing due; 0 no staging this frame -- nothing drawn, the serials
   not advanced, so the next frame sends it; -1 the pass must come down. */
static int s_rect[TAGPU_VK_STAGE_MAXRECT][4];      /* render thread only */
static unsigned s_upFrame;                         /* the frame base_upload last ran in */
static unsigned s_saidSkip;                        /* the skip's line, once a 300 frames */
static int base_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, SLOT* s,
                       const TAGPU_FEATHAND* h)
{
    int rows = h->atlasRows, n, rc, keep, move = 0, moveDue;
    const char* why = NULL;
    VkDeviceSize bytes;

    s_upFrame = d->frame;
    /* THE IMAGES AND THEIR TWO DESCRIPTOR BINDINGS ARE MADE ONCE, BY
       `atlas_build`, AND THAT IS WHY THEY MAY BE WRITTEN AT ALL.
       vkUpdateDescriptorSets on a set some pending command buffer has bound is
       undefined behaviour, and those bindings are the writes in this file that
       touch EVERY slot's set rather than the one slot we own. It is safe
       because it happens on the first prepare after `build`, before any of
       those sets has ever been bound -- so a later change of the atlas's
       dimensions is refused here rather than silently rewriting sets that are
       in flight. tagpu_feat.c's ATLAS_DIM is a compile-time constant, so this
       cannot fire; it is the guard that keeps that true if it ever stops being
       one. */
    if (s_atDim != h->atlasDim) {
        plog(d, "feat: the atlas changed from %d texels square to %d - the pass "
                "comes down rather than rewrite descriptor sets that frames in "
                "flight are using", s_atDim, h->atlasDim);
        return -1;
    }
    if (s_bHave && s_bSerial == h->atlasSerial && s_bPal == h->palSerial) {
        tagpu_vk_stage_drop(d, &s->stage);
        tagpu_feat_atlas_ack(s_bSerial, 1);
        return 1;
    }
    if (rows < 1) rows = 1;
    if (rows > h->atlasDim) rows = h->atlasDim;
    keep = s_bHave && s_bPal == h->palSerial && (int)(h->atlasWhole - s_bSerial) <= 0;
    if (!keep)
        why = !s_bHave ? "the first upload since the image was made"
            : s_bPal != h->palSerial ? "the engine's table moved"
            : "the atlas is new (made, lost, a new level, its mirror armed)";
    /* A REPACK SINCE THIS COPY: its cells are moved here, on the device, and
       only the paints the copy lacked are sent (tagpu_gaf.h `moves`). The atlas
       holds a repack until this copy is current (tagpu_gaf_atlas_reset), so
       the move always applies; a device that refused the buffer takes the
       page instead, which is right and is a wait. The restored twin follows
       whichever it is: moved with the base (`twin_move`), or restored again. */
    moveDue = h->atlasMoves && (int)(h->atlasMoveSerial - s_bSerial) > 0 &&
              h->atlasMoveSerial != s_bMove;
    if (keep && moveDue) {
        if ((int)(s_bSerial - h->atlasMovePrev) >= 0 &&
            tagpu_vk_stage_move_ready(d, &s_move, h->atlasDim, h->atlasMoves, h->atlasMoveN))
            move = 1;
        else {
            keep = 0;
            why = "a repack this copy cannot move - A WAIT IN PLAY";
        }
    }
    n = tagpu_gaf_rects_due(h->atlasDirty, h->atlasSerial, s_bSerial, keep,
                            h->atlasDim, rows, s_rect, TAGPU_VK_STAGE_MAXRECT);
    bytes = (VkDeviceSize)tagpu_gaf_rects_area((const int (*)[4])s_rect, n) * 4;
    if (keep) {
        /* checked before anything is recorded, so a skipped frame leaves the
           image and every serial as they were */
        if (n > 0 && (!tagpu_vk_stage_begin(d, &s->stage, bytes, (VkDeviceSize)h->atlasDim * 4) ||
                      !tagpu_vk_stage_fits(&s->stage, bytes))) {
            if (!s_saidSkip || d->frame - s_saidSkip >= 300u) {
                s_saidSkip = d->frame ? d->frame : 1u;
                plog(d, "feat: %u KB of tiles due and no staging that holds them - the "
                        "frame is skipped and they go on the next; nothing waited on",
                     (unsigned)(bytes >> 10));
            }
            return 0;
        }
        if (move) {
            if (!tagpu_vk_stage_move(d, cb, &s_move, s_bImg, h->atlasDim, h->atlasMoves,
                                     h->atlasMoveN))
                return 0;
            s_bMove = h->atlasMoveSerial;
            twin_move(d, cb, h);
        }
        if (n > 0 &&
            !tagpu_vk_stage_expand_rects(d, cb, &s->stage, s_bImg, s_bHave, h->atlas, h->atlasKey,
                                         h->atlasDim, (const int (*)[4])s_rect, n, h->pal, NULL))
            return 0;
        /* nothing else due: every paint since landed below the used rows, or
           the move was all there was */
        if (n == 0) tagpu_vk_stage_drop(d, &s->stage);
    } else {
        plog(d, "feat: the whole base atlas, %u KB, through the banded path - %s",
             (unsigned)(bytes >> 10), why);
        if (!tagpu_vk_stage_begin(d, &s->stage, bytes, (VkDeviceSize)h->atlasDim * 4))
            return 0;
        rc = tagpu_vk_stage_expand(d, cb, &s->stage, s_bImg, s_bHave, h->atlas, h->atlasKey,
                                   h->atlasDim, 0, 0, h->atlasDim, rows, h->pal, NULL);
        if (rc <= 0) return rc;
        /* the base atlas took the repack's layout whole, so the twin, which
           cannot be sent from here, starts again in it */
        if (moveDue) twin_drop(d, "a repack the base atlas took whole");
    }
    s_bSerial = h->atlasSerial; s_bPal = h->palSerial;
    s_bHave = 1;
    tagpu_feat_atlas_ack(s_bSerial, 1);
    return 1;
}

/* THE SCISSOR, in Vulkan framebuffer pixels. Item 3 of the file header: the
   rect arrives in GAME-FRAME pixels measured from the TOP of the frame (the
   engine's own viewport rect), and this pass's framebuffer row 0 IS that top
   row -- so the rect goes in unchanged.

   It is also SCALED, by the attachment's extent over the game frame's. The
   attachment need not be the game frame's size -- the world target is `ss`
   times it -- and scaling here keeps the clip on the same part of the world
   when they differ, which is the same thing the vertex shader's division by
   uGame does. */
static void feat_scissor(uint32_t w, uint32_t h)
{
    float sx = s_hGw > 0.0f ? (float)w / s_hGw : 1.0f;
    float sy = s_hGh > 0.0f ? (float)h / s_hGh : 1.0f;
    int x0 = (int)(s_hVpL * sx + 0.5f);
    int ww = (int)(s_hVw * sx + 0.5f);
    int ytop = (int)(s_hVpT * sy + 0.5f);
    int hh = (int)(s_hVh * sy + 0.5f);
    int y0 = ytop;                          /* NOT mirrored: see above   */

    /* NO CLIP WHERE THE NATIVE PASS SAYS NONE. `scissorOn` is its decision
       (tagpu_native.c `s_scissorOn`); clipping when it says not to would cut
       every feature the gather's margin reaches past the viewport. */
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

static int prepare_draw(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_FEATHAND h;
    SLOT* s;
    VkDeviceSize vbytes;
    /* A UNION, NOT A CAST. Both blocks mix `int` and `float` members and
       writing an int through a float array is the aliasing rule broken at -O2,
       which is not a place to find out that the fog branch took a garbage
       uFog. */
    union { float f[8]; int i[8]; } ub;
    int fogW = 1, fogH = 1;

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW, AND NOTHING IS KEPT
       ONCE THERE IS NOT -- §2.28's rule. Giving slot `slot` back at this point
       needs no new argument and no timer, because it is the same instant, and
       the same ownership, that the rest of this function writes it in. */
    if (!tagpu_feat_handover(&h, d->frame)) {
        if (s_state == ST_READY) slot_free(d, &s_slot[slot]);
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_feat_down(d); s_state = ST_REFUSED; return 0; }
        /* the shared atlas image and the two descriptor bindings that name it,
           before any set has been bound -- see `base_upload` */
        if (h.atlasDim < 1 || h.atlasDim > 8192 || !atlas_build(d, h.atlasDim)) {
            plog(d, "feat: a %d-texel-square atlas image was refused", h.atlasDim);
            tagpu_vk_feat_down(d); s_state = ST_REFUSED; return 0;
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
           arrive without one -- a healthy run, not a fault. */
        if (!s_saidNoMirror) {
            s_saidNoMirror = 1;
            plog(d, "feat: the atlas has no CPU mirror yet - nothing "
                    "drawn until the producer arms one");
        }
        return 0;
    }
    s_saidNoMirror = 0;
    if (!h.pal) return 0;
    if (h.nShadow < 0 || h.nBody < 0 || h.nShadow + h.nBody < 1) return 0;
    if (h.fogGrid) {
        if (h.fogGridCols < 1 || h.fogGridRows < 1 ||
            h.fogGridCols > FOG_MAXDIM || h.fogGridRows > FOG_MAXDIM) {
            plog(d, "feat: a %dx%d fog grid is outside what this pass carries - nothing drawn",
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
    if (!slot_fog(d, s, fogW, fogH)) goto refuse;

    vbytes = (VkDeviceSize)(h.nShadow + h.nBody) * VST * sizeof(float);
    if (!slot_verts(d, s, vbytes)) goto refuse;
    /* the two buckets, contiguous and in draw order: shadows, then bodies */
    if (h.nShadow) memcpy(s->vmap, h.shadow, (size_t)h.nShadow * VST * sizeof(float));
    if (h.nBody)   memcpy(s->vmap + (size_t)h.nShadow * VST * sizeof(float),
                          h.body, (size_t)h.nBody * VST * sizeof(float));

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

    /* CAN THIS FRAME DRAW RESTORED? DECIDED HERE, AFTER THE UPLOAD AND AFTER
       `restore_want`, because both can take the twin away in this very frame:
       a repack the base atlas took whole, or one the twin could not follow,
       drops the job (`twin_drop`); a new generation or a move of the engine's
       table replaces it. Binding 40 names the twin whatever it holds, and a
       new job clears it only at its first draw, so a flag decided before them
       would have this frame sample a twin nobody owns any more. Read here, it
       is the state this frame's draw samples: the producer published a frame
       LIST, this lane's job has painted a frame of it, and `s_arHave` -- which
       every path above that disowns the twin clears -- says so.

       A FRAME WITHOUT A PAINTED TWIN IS DRAWN, from the base atlas, not
       returned from: the restore needs this frame's base atlas uploaded before
       it can paint anything, so returning on the frames before the first
       paint would be a deadlock -- nothing drawn because nothing painted,
       nothing painted because the atlas never arrived. This atlas changes
       generation on every map or level change, so this is the routine path
       and not only the failure one: base art is Classic++ restore off for a
       few frames, not a wrong picture.

       IT IS NOT LATCHED. gpu-status 2.35 measured what a latch costs -- a pass
       that refuses once stays dark for the process even after the condition
       clears. This one clears within a few frames of the restorer starting, so
       `s_saidRestored` gates the LOG LINE only and is cleared again below. */
    if (h.restored && !(s_arImg && h.restoreFrames && s_arHave)) {
        if (!s_saidRestored) {
            s_saidRestored = 1;
            plog(d, "feat: a Classic++ restore is armed and this lane has no "
                    "restored twin of it yet - %s", s_rjob
                        ? "its job is painting one; the base atlas is drawn until it has"
                        : "drawing the base atlas until one is painted");
        }
        h.restored = 0;
    } else s_saidRestored = 0;

    /* THE FOG GRID, per slot, so the one-line invariant covers it: UNDEFINED
       in, because the whole of it is re-sent every frame and there are
       therefore no contents to preserve and no layout to carry. */
    /* The grid is one `unsigned short` a cell and the image is RG8: the same
       two bytes in the same order, so the buffer is copied as it stands into
       the VK_FORMAT_R8G8_UNORM image. With no grid this frame
       the image is one zero cell, which the shader never reads -- taFog is
       called only on the `uFog & 1` branch. */
    if (h.fogGrid) memcpy(s->smallMap, h.fogGrid, (size_t)fogW * fogH * 2);
    else           memset(s->smallMap, 0, 2);
    img_barrier(cb, s->fog, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->smallStage, 0, s->fog, fogW, fogH);
    img_barrier(cb, s->fog, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    /* The two uniform blocks, at the std140 offsets the generated header
       prints -- the numbers the hand-over carries. */
    memset(&ub, 0, sizeof ub);
    ub.f[0] = h.gw; ub.f[1] = h.gh;                    /* uGame       vec2 @0  */
    ub.f[2] = h.zoom;                                  /* uZoom      float @8  */
    ub.f[4] = h.zoomCx; ub.f[5] = h.zoomCy;            /* uZoomC      vec2 @16 */
    ub.f[6] = h.depthScale;                            /* uDepthScale float @24 */
    memcpy(s_umap + (size_t)slot * s_ustride, ub.f, UBLK);
    memset(&ub, 0, sizeof ub);
    ub.i[0] = h.restored;                              /* uRestored    int @0  */
    ub.f[2] = h.fogOrgX; ub.f[3] = h.fogOrgY;          /* uFogOrg     vec2 @8  */
    ub.f[4] = h.fogCols; ub.f[5] = h.fogRows;          /* uFogDim     vec2 @16 */
    ub.i[6] = h.fog;                                   /* uFog         int @24 */
    memcpy(s_umap + (size_t)slot * s_ustride + s_ublock, ub.f, UBLK);

    s_nShadow = h.nShadow;
    s_nBody = h.nBody;
    s_hGw = h.gw; s_hGh = h.gh;
    s_hVpL = h.vpL; s_hVpT = h.vpT; s_hVw = h.vw; s_hVh = h.vh;
    s_hScissorOn = h.scissorOn;

    /* THE A/B FRAME IS CLAIMED LAST, AFTER EVERY REASON NOT TO DRAW IS PAST. A
       frame claimed and then not drawn would have the seam capture a bare
       clear, and a diff against another build's capture would report every
       feature pixel as differing: a capture failure read as a pass failure. */
    s_abFrame = h.ab;
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
    plog(d, "feat: slot %u would not take this frame's resources - the pass stops "
            "drawing and the seam tears it down", (unsigned)slot);
    refuse_job(d);
    s_state = ST_REFUSED;
    s_downOwed = 1;
    return 0;
}

/* THE UPLOAD THIS PASS OWES ON A FRAME IT DID NOT MAKE ONE. The atlas's
   allowance counts from this copy (tagpu_gaf.h `budget`) and defers paints
   past it; were the copy brought up to date only on frames that draw, a frame
   whose every feature waits on the allowance would draw nothing, upload
   nothing and leave them waiting for good. So whenever the atlas says the copy
   is behind (`tagpu_feat_atlas_owed`) and `prepare_draw` did not upload --
   nothing to draw, no hand-over, or any refusal before its upload -- the tiles
   go here, drawn from or not.
   AFTER `prepare_draw`, WHICH IS WHAT MAKES IT SAFE: every `slot_free` of this
   frame is behind us, so the staging this records a copy out of lives until
   this slot's next frame -- the rule every upload here keeps. Outside any
   render pass, and before the restorer's slice, like the upload it stands in
   for. */
static void atlas_owed(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_FEATHAND h;
    if (s_state != ST_READY || s_downOwed || s_upFrame == d->frame) return;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return;
    if (!tagpu_feat_atlas_owed()) return;
    memset(&h, 0, sizeof h);
    if (!tagpu_feat_atlas_hand(&h) || !h.pal) return;
    if (base_upload(d, cb, &s_slot[slot], &h) < 0) {
        plog(d, "feat: the owed atlas upload failed on the device - the pass stops "
                "drawing and the seam tears it down");
        refuse_job(d);
        s_state = ST_REFUSED;
        s_downOwed = 1;
    }
}

int tagpu_vk_feat_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    const int r = prepare_draw(d, cb, slot);
    atlas_owed(d, cb, slot);
    return r;
}

void tagpu_vk_feat_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = 0;

    (void)d;
    if (s_state != ST_READY || !s_drawThis) return;
    s_drawThis = 0;

    /* NO Y FLIP, AND THE DEPTH RANGE, AND THEY ARE TWO SEPARATE QUESTIONS.
       This pass writes `gl_Position.y = p.y/uGame.y*2 - 1` on the engine's
       screen-space y, which grows DOWNWARD, so clip -1 is the game frame's top
       row and a POSITIVE height puts it on row 0 -- where the game's top row
       is; a NEGATIVE height would turn the frame upside down. minDepth 0.5 /
       maxDepth 1.0 maps clip z in [0, 1] onto [0.5, 1], the range every
       depth-testing world pass shares -- see item 1 of the file header;
       without it every depth VALUE here differs from theirs and a z-fight
       settles the other way. */
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.minDepth = 0.5f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    feat_scissor(w, h);
    sc.offset.x = s_scX; sc.offset.y = s_scY;
    sc.extent.width = (uint32_t)s_scW; sc.extent.height = (uint32_t)s_scH;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1,
                            &s_slot[slot].dset, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_slot[slot].vbuf, &off);
    if (s_nShadow) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeShadow);
        vkCmdDraw(cb, (uint32_t)s_nShadow, 1, 0, 0);
    }
    if (s_nBody) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeBody);
        vkCmdDraw(cb, (uint32_t)s_nBody, 1, (uint32_t)s_nShadow, 0);
    }
}

int tagpu_vk_feat_ab_frame(void)
{
    int a = s_abFrame;
    s_abFrame = 0;
    return a;
}

void tagpu_vk_feat_down(const TAGPU_VKPASS* d)
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
    /* THE RESTORE JOB GOES BACK BEFORE THE IMAGES IT NAMES: it holds a
       framebuffer over the twin's view, and a view still named by a live
       framebuffer may not be destroyed. Every caller of this function is past
       the seam's vkDeviceWaitIdle, so neither is in a queue.
       AND THE VERDICT DOES NOT SURVIVE THE DEVICE -- `s_rjTried` is a fact
       about a device that refused, so a new one is asked again. */
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
    s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0; s_rjTried = 0;
    s_rjSrcView = VK_NULL_HANDLE;
    kill_image(d, &s_arImg, &s_arMem, &s_arView);
    kill_image(d, &s_bImg, &s_bMem, &s_bView);
    tagpu_vk_stage_move_drop(d, &s_move);
    s_arHave = 0;
    s_atDim = 0;
    s_bHave = 0; s_bSerial = 0; s_bPal = 0; s_bMove = 0;
    tagpu_feat_atlas_ack(0, 0);            /* the copy went with the device */
    if (s_pipeShadow) { vkDestroyPipeline(dev, s_pipeShadow, NULL); s_pipeShadow = VK_NULL_HANDLE; }
    if (s_pipeBody)   { vkDestroyPipeline(dev, s_pipeBody, NULL);   s_pipeBody = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dpool) { vkDestroyDescriptorPool(dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_samp)  { vkDestroySampler(dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_umap)  { vkUnmapMemory(dev, s_umem); s_umap = NULL; }
    if (s_ubuf)  { vkDestroyBuffer(dev, s_ubuf, NULL); s_ubuf = VK_NULL_HANDLE; }
    if (s_umem)  { vkFreeMemory(dev, s_umem, NULL); s_umem = VK_NULL_HANDLE; }
    s_drawThis = 0;
    s_abFrame = 0;
    s_nShadow = s_nBody = 0;
    /* ST_UNBUILT and not ST_REFUSED: a pass brought down by a mode change or a
       cleared lever must be able to come back. THE ONE EXCEPTION IS THE
       TEARDOWN THIS PASS ASKED FOR: there the device refusing resources IS the
       reason, `prepare` has already latched ST_REFUSED, and clearing it here
       would have the pass rebuild and fail again on the very next frame. */
    s_state = owed ? ST_REFUSED : ST_UNBUILT;
}

/* 1 while this pass has stopped drawing and is waiting for the seam to drain
   the device and tear it down -- see `prepare`'s refusal path. */
int tagpu_vk_feat_down_owed(void)
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
void tagpu_vk_feat_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_feat_down(d);          /* clears s_downOwed itself */
    s_downPaying = 0;
}
