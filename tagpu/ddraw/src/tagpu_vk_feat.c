/* tagpu_vk_feat.c -- the feature pass (trees, rocks, splats, GAF wreckage)
   drawn by Vulkan. Contract: tagpu_vk_feat.h. Phase G / G19e, the SECOND WORLD
   PASS, and the first one that depth-tests.

   IT IS NOT A SECOND IMPLEMENTATION OF THE PASS. Everything arrives through
   `tagpu_feat_handover` (tagpu_feat.h): the vertices are the two arrays the GL
   gather filled and the GL upload took, the uniforms are the numbers the GL
   draw passed, the texels are the bytes each GL texture was uploaded FROM --
   the atlas through tagpu_gaf.c's CPU mirror, which is written by the same
   atlas_paint that writes the texture and from the same buffer -- and the
   shader is the same GLSL through tools/spirv-gen.py. What a 0-px comparison
   then compares is two rasterisers.

   ---- WHAT THIS LANDING HAD TO ANSWER, AND WHERE EACH ANSWER IS ----

   1. DEPTH. gpu-status §2.28 decided the GL/Vulkan depth-range question on
      paper and left it for the first pass that tests. This is it.

        GL maps clip z from [-1, 1] onto the depth range, Vulkan takes [0, 1].
        Every shader here writes a z already in [0, 1], so under GL the near
        half of the range is thrown away and under Vulkan it is not: the
        ORDERING is identical (both mappings are affine and increasing, so every
        depth test comes out the same) but the VALUES are not, and Vulkan would
        get a bit more precision out of a fixed-point buffer than GL does --
        enough to settle a z-fight the other way and put 0 px out of reach.

      The fix is PIPELINE STATE, never a shader edit: a viewport with
      `minDepth = 0.5` and `maxDepth = 1.0` maps clip z in [0, 1] onto exactly
      GL's `(z+1)/2`, values and precision included, and needs no extension.
      `VK_EXT_depth_clip_control` would do it the other way by adopting GL's
      convention outright, and is the answer only if a shader is ever found
      writing a z below 0 -- Vulkan clips those and GL does not. These do not:
      `clamp(1.0 - aPos.z/uDepthScale, 0.0, 1.0)` is the whole of it.
      The attachment's FORMAT matters for the same reason and is the seam's
      (tagpu_vk.c `vk_depth_format`): 24-bit fixed point, because the GL FBO is
      GL_DEPTH24_STENCIL8 and a comparison against it is a comparison of two
      quantisations as much as of two rasterisers.

   2. TWO DEPTH-WRITE MODES, AND SO TWO PIPELINES. The GL twin draws shadows
      with `glDepthMask(GL_FALSE)` and bodies with it true -- shadows are ground
      decals and must occlude nothing -- and that is pipeline state in Vulkan.
      Two pipelines off one layout is the answer that needs no extension;
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE is Vulkan 1.3 or an extension and buys
      one object.

   3. THE SCISSOR is the world viewport, and since landing 5b it is the SAME
      rectangle on both sides. It used to be the vertical mirror
      (`offset.y = H - (vpT + vh)`), which was the right arithmetic for as long
      as the Vulkan image was stored upside down to match the GL world FBO --
      item 4 is why it no longer is. With a positive viewport height row 0 of the
      Vulkan image is the game frame's top row, the same row the GL FBO hands
      back first, so `offset.y` is just the viewport top. Getting it wrong shows
      up as the world clipped against the wrong edge rather than as anything
      subtle.

   4.   NO Y FLIP, AND THAT IS LANDING 5b's CORRECTION. This pass writes
      `gl_Position.y = p.y/uGame.y*2 - 1` on the engine's screen-space y, which
      grows DOWNWARD, so clip +1 is the BOTTOM of the game frame. GL's composite
      quad turns the world FBO over on the way to the window, which is why the
      game looks right. The Vulkan lane has no composite quad -- the ported
      passes draw STRAIGHT INTO THE SWAPCHAIN IMAGE -- so a negative viewport
      height, which this pass took until 2026-09-17, turned the frame over a
      SECOND time and Route D presented the world upside down. The viewport is
      positive now and clip -1 lands on row 0, which is the game's top row under
      both APIs.
      It was invisible for eight landings because `tagpu_abshot.c` turned the GL
      half of every capture over by the same rule, so the two halves lined up and
      the A/B -- which compares the lanes to each other -- is structurally blind
      to a flip they share. The capture takes TAGPU_ABSHOT_TOPDOWN now.
      (VK_KHR_maintenance1 was needed only for the negative height, so this pass
      no longer requires it; `tagpu_vk_gui.c`, `_fps.c` and `_scaffold.c` still
      do, because their shaders are y-UP and their flip is correct.)

   5. BLENDING is glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) -- the GL FBO is
      premultiplied -- and glBlendFunc sets the alpha factors as well as the
      colour ones, so both pairs are set here.

   ---- THE UPLOADS, AND WHY ONE OF THEM IS SHARED ----

   §2.28 established one image and one staging buffer PER FRAME SLOT, on the
   one-line invariant that the seam has just waited on `fence[slot]`, so nothing
   of ours is in flight for that slot: the CPU may write its staging buffer, the
   copy into its image has no prior access to order against, and the descriptor
   set naming it may be rewritten. It also named the cheaper design -- ONE image
   shared by every slot, behind a write-after-read barrier -- and said to reach
   for it when a pass needed the memory back.

   THIS IS THAT PASS, and the split is by size:

     the ATLAS      2048 x 2048 R8 = 4 MB. Per-slot it would be 16 MB of
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
                    transition, which is a write. It is uploaded only when the
                    mirror's serial says the bytes moved, and only the rows the
                    shelf packer has actually used.

     the PALETTE    1 KB, 256 B and a few KB. Per-slot, because at that size
     the FOG LUT    the one-line invariant is worth more than the memory and a
     the FOG GRID   dimension change (the grid's, every time the view moves far
                    enough) is then free: the slot is rebuilt at the moment we
                    are handed it, which is the moment we own it.

   The atlas's STAGING buffer is per-slot either way and cannot be anything
   else: a barrier orders GPU work and the hazard there is a CPU write. It is
   allocated on the frame that uploads and given back at that slot's next
   `prepare` -- the same fence, one turn of the slots later -- so a settled
   scene holds none of it and a map still filling its atlas holds at most one
   per slot while it does.

   AND NOTHING IS KEPT ONCE THERE IS NOTHING TO DRAW, exactly as §2.28's
   scaffold: a frame the GL twin handed nothing over gives that slot back.

   ---- WHAT IT DOES NOT DO ----

   CLASSIC++'s RESTORED ATLAS **IS** MIRRORED SINCE GATE 2 of the Vulkan-only
   plan, so a frame whose twin reports `uRestored` 1 is DRAWN -- through the
   twin's own colours, read back off tagpu_restoreglsl.c's RGBA8 surface by
   tagpu_gaf.c's step and uploaded to binding 42 here. What is left of the old
   refusal is "has the read-back produced rows YET": until it has, the GL twin
   is sampling colours this lane does not have, so the frame is refused rather
   than drawn with `uRestored` 0 -- that would be a different picture from the
   twin's and the A/B would report it as a rasteriser difference, which is the
   one answer an oracle must never give. The refusal is no longer latched: the
   condition clears by itself within a few frames of the restorer starting.

   IT KNOWS NOTHING ABOUT A WINDOW. Everything arrives in TAGPU_VKPASS.
   A PASS READS NO ENGINE STATE: every value comes from the GL lane's
   hand-over, so this file is not on thread-split.allow and must never be. */

#include "tagpu_vk_pass.h"
#include "tagpu_vk_restore.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_feat.h"
#include "tagpu_feat.h"
#include "spirv/tagpu_feat.spv.h"

#define VST TAGPU_FEAT_VST                 /* floats per vertex, the twin's    */
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

/* ---- THE SHARED ATLAS. One image for every slot; see the file header. ---- */
static VkImage        s_atImg;
static VkDeviceMemory s_atMem;
static VkImageView    s_atView;
static int            s_atDim;             /* what the image was created for   */
static unsigned       s_atSerial;          /* the mirror serial it holds       */
static int            s_atHave;            /* a copy has been recorded into it */

/* CLASSIC++'s RESTORED TWIN (the Vulkan-only plan's gate 2). A second image of
   the same square in RGBA8, fed from `tagpu_gaf.c`'s read-back mirror. It is
   built beside the indexed one and binding 42 names it instead of being the
   placeholder the comment in `atlas_build` used to describe. When it cannot be
   built the binding falls back to the indexed view and the pass stands down on
   a restored frame exactly as it did before this existed -- a valid descriptor
   is required for the set to be bound at all, so the fallback is not optional.
   `s_arReq` IS THE REQUESTED ROWS LAST UPLOADED FOR, NOT THE ROWS SENT. It held
   the rows sent, which the whole-square rule had already forced to the atlas's
   full height, so it could never equal the hand-over's shelf-bounded count:
   `doRgb` was then true on EVERY frame and the pass re-sent 16 MB per frame for
   the life of the session while never releasing its staging. The SERIAL alone
   decides whether anything is due, exactly as it does for the indexed atlas
   beside it; these rows are kept only to notice a SHRINK (a re-arm, a context
   loss), which is the one case that needs the whole square re-sent because rows
   above the new mark would otherwise keep the previous twin's colours.
   [FOUND BY THE GATE-2 LANDING REVIEW, 2026-09-16.] */
static VkImage        s_arImg;
static VkDeviceMemory s_arMem;
static VkImageView    s_arView;
static int            s_arReq;
static unsigned       s_arSerial;
static int            s_arHave;
/* ---- THE RESTORE THIS LANE RUNS FOR ITSELF (landing 7d) ------------------
   `s_rjob` paints `s_arImg` from `s_atImg` when the producer publishes a frame
   list instead of a mirror. Three pieces of state make it a CURSOR rather than
   a serial, which is what a lazy queue needs:

     s_rjGen    the producer's generation this job belongs to. A new one is a
                discontinuity a cursor cannot survive -- a recycle, a repack, a
                context loss, a palette move -- so the job is rebuilt and the
                cursor goes back to 0.
     s_rjTaken  how many of that generation's frames are already queued here.
                The producer retains the whole array, so a frame on which this
                pass took nothing costs nothing: the next one takes more.
     s_rjSrcView  the view the job reads. The atlas image can be re-created
                under a live job (a dimension change), and a job holding the
                old view would paint from freed memory. [The landing-7 review
                found this on the terrain consumer; it is the same hazard here
                and the same guard.] */
static TAGPU_VKRJOB*  s_rjob;
static unsigned       s_rjGen;
static unsigned       s_rjBlanks;         /* the producer's blank count, seen */
static int            s_rjTaken;
static int            s_rjPainted;
static int            s_rjTried;           /* the device refused; do not ask again */
static VkImageView    s_rjSrcView;

/* what `record` was left to draw */
static int s_nShadow, s_nBody;
static int s_scX, s_scY, s_scW, s_scH;     /* the scissor, in Vulkan framebuffer px */
/* The scissor's INPUTS, kept as numbers rather than as a copy of the hand-over.
   TAGPU_FEATHAND is full of pointers into the GL lane's own frame memory, and a
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

    VkBuffer        astage;                /* the atlas upload, when one is due */
    VkDeviceMemory  amem;
    unsigned char*  amap;
    VkDeviceSize    acap;

    VkImage         pal, lut, fog;         /* the three small per-slot images  */
    VkDeviceMemory  palMem, lutMem, fogMem;
    VkImageView     palView, lutView, fogView;
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
   allocation (the scaffold's rule, tagpu_vk_scaffold.c) */
#define FOG_MAXDIM 1024
/* ONE STAGING BUFFER CARRIES ALL THREE SMALL UPLOADS: the palette (256 x 1
   RGBA), then the fog shade LUT (256 x 1 R8), then the fog grid (cols x rows
   RG8, a few KB and a different size whenever the view walks far enough for the
   grid to be re-laid). One allocation, three vkCmdCopyBufferToImage regions at
   three offsets -- and it is rebuilt with the fog image, by the same call, so
   the buffer and the image it feeds can never disagree about the size. */
#define SMALL_PALOFF 0                     /* 256 x RGBA8                      */
#define SMALL_LUTOFF (256 * 4)             /* 256 x R8                         */
#define SMALL_FIXED  (256 * 4 + 256)       /* what the two fixed uploads take  */
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

/* `usage` IS THE CALLER'S BECAUSE THE RESTORED TWIN IS A RENDER TARGET.
   It was a constant here until landing 7d: the indexed atlas is only ever
   copied into and sampled, but the twin is what this lane's own restorer
   PAINTS, and a Vulkan image may only be a colour attachment if it was
   created saying so. Nothing infers it from the format -- the destination's
   usage is a promise made at creation and tagpu_vk_restore.h asks for it by
   name. */
/* WHAT EACH OF THIS PASS'S IMAGES IS FOR. The restored twin carries
   COLOR_ATTACHMENT because this lane's restorer paints into it (landing 7d);
   it costs nothing when nothing restores, and an image created without it
   could not be lent to the restorer at all. */
#define IMG_SAMPLED  (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
#define IMG_RESTORED (IMG_SAMPLED | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)

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
    /* THE UNMAP IS GUARDED BY THE MAP POINTER, NOT BY THE ALLOCATION: a
       vkMapMemory that failed leaves the allocation standing and unmapping it
       would be an error of its own. */
    if (s->amap)  { vkUnmapMemory(dev, s->amem); s->amap = NULL; }
    if (s->astage){ vkDestroyBuffer(dev, s->astage, NULL); s->astage = VK_NULL_HANDLE; }
    if (s->amem)  { vkFreeMemory(dev, s->amem, NULL); s->amem = VK_NULL_HANDLE; }
    s->acap = 0;
    if (s->smallMap) { vkUnmapMemory(dev, s->smallMem); s->smallMap = NULL; }
    if (s->smallStage) { vkDestroyBuffer(dev, s->smallStage, NULL); s->smallStage = VK_NULL_HANDLE; }
    if (s->smallMem) { vkFreeMemory(dev, s->smallMem, NULL); s->smallMem = VK_NULL_HANDLE; }
    kill_image(d, &s->pal, &s->palMem, &s->palView);
    kill_image(d, &s->lut, &s->lutMem, &s->lutView);
    kill_image(d, &s->fog, &s->fogMem, &s->fogView);
    s->fogW = s->fogH = 0;
    s->built = 0;
}

/* Give this slot's ATLAS STAGING back -- the one piece of it that is 4 MB and
   is wanted only on the frames the atlas actually moved. Safe here for the
   same one-line reason everything else in `prepare` is: the seam has waited on
   this slot's fence, so the copy that read this buffer has completed. */
static void slot_drop_astage(const TAGPU_VKPASS* d, SLOT* s)
{
    if (s->amap)  { vkUnmapMemory(d->dev, s->amem); s->amap = NULL; }
    if (s->astage){ vkDestroyBuffer(d->dev, s->astage, NULL); s->astage = VK_NULL_HANDLE; }
    if (s->amem)  { vkFreeMemory(d->dev, s->amem, NULL); s->amem = VK_NULL_HANDLE; }
    s->acap = 0;
}

/* The fixed-size half of a slot: the palette and LUT images, their shared
   staging buffer, and the descriptor writes that never change again. */
static int slot_build(const TAGPU_VKPASS* d, SLOT* s)
{
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet wr[2];

    if (s->built) return 1;
    if (!mk_image(d, 256, 1, VK_FORMAT_R8G8B8A8_UNORM, IMG_SAMPLED, &s->pal, &s->palMem, &s->palView)) return 0;
    if (!mk_image(d, 256, 1, VK_FORMAT_R8_UNORM, IMG_SAMPLED, &s->lut, &s->lutMem, &s->lutView)) return 0;

    memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
    ii[0].sampler = s_samp; ii[0].imageView = s->palView;
    ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].sampler = s_samp; ii[1].imageView = s->lutView;
    ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = s->dset; wr[0].dstBinding = 41; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &ii[0];
    wr[1] = wr[0]; wr[1].dstBinding = 44; wr[1].pImageInfo = &ii[1];
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

/* ---- build -------------------------------------------------------------- */

static int build_sampler(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties fp;
    int i;
    /* the three formats this pass samples, asked for rather than assumed */
    static const VkFormat need[3] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                      VK_FORMAT_R8G8B8A8_UNORM };
    for (i = 0; i < 3; i++) {
        vkGetPhysicalDeviceFormatProperties(d->pd, need[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            plog(d, "feat: this device cannot sample format %d - the pass stays down",
                 (int)need[i]);
            return 0;
        }
    }
    /* NEAREST AND CLAMP_TO_EDGE, WHICH IS WHAT EVERY ONE OF THE GL TWIN'S FIVE
       TEXTURES IS SET TO. Every texel here is an INDEX -- a palette entry, a
       fog corner mask, a shade-table slot -- and interpolating two of them
       produces a number that means nothing in any of the three tables. */
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
    VkDescriptorSetLayoutBinding b[7];
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
       order -- uAtlas, uPal, uAtlasRGB, uFogGrid, uFogLUT. The std140 offsets
       are printed in the generated header and are the contract for what goes
       into those buffers. */
    memset(b, 0, sizeof b);
    b[0].binding = 0;  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 32; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 2; i < 7; i++) {
        b[i].binding = (uint32_t)(40 + (i - 2));
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    dli.bindingCount = 7; dli.pBindings = b;
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

    /* ONE TABLE, TWO LANES: tagpu_feat.h TAGPU_FEAT_ATTRS is what the GL VAO is
       built from too, so a layout change cannot reach one lane and miss the
       other -- which would make the A/B compare two different meshes and call
       it a rasteriser difference. */
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

    /* A LIST, which is what the GL twin draws: put_quad emits six vertices. */
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;     /* both dynamic, set per frame */

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* NO CULLING, and the reason is now the simple one: THE GL TWIN DOES NOT
       CULL. It used to be stated the other way round -- a negative viewport
       height flips the winding of every triangle, so a cull mode that was right
       under GL would have thrown the whole frame away -- and that argument went
       with the flip in landing 5b. Nothing here culls and nothing here should,
       so the state is unchanged and only its justification is. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* GL_LESS, which is what the native pass sets around this draw
       (glDepthFunc(GL_LESS), glEnable(GL_DEPTH_TEST)). The WRITE half is what
       the two pipelines differ in. */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    memset(&cba, 0, sizeof cba);
    /* glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) -- the GL FBO is
       premultiplied -- and glBlendFunc sets the alpha factors as well as the
       colour ones, so both pairs are set. */
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
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[1].descriptorCount = d->slots * 5;
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
       written when there is an image to name: 40 and 42 by `atlas_build`, 41
       and 44 by `slot_build`, 43 by `slot_fog`. */
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

/* The shared atlas image, and the two descriptor bindings that name it. */
static int atlas_build(const TAGPU_VKPASS* d, int dim)
{
    uint32_t i;
    VkDescriptorImageInfo irgb;
    if (s_atImg && s_atDim == dim) return 1;
    kill_image(d, &s_atImg, &s_atMem, &s_atView);
    s_atDim = 0; s_atSerial = 0; s_atHave = 0;
    if (!mk_image(d, dim, dim, VK_FORMAT_R8_UNORM, IMG_SAMPLED,
                  &s_atImg, &s_atMem, &s_atView)) return 0;
    s_atDim = dim;
    /* THE RESTORED TWIN'S IMAGE, and a failure here is not fatal: binding 42
       falls back to the indexed view below and the restored stand-down keeps
       its old meaning. 16 MB at the shipped 2048 square. */
    kill_image(d, &s_arImg, &s_arMem, &s_arView);
    s_arReq = 0; s_arSerial = 0; s_arHave = 0;
    if (!mk_image(d, dim, dim, VK_FORMAT_R8G8B8A8_UNORM, IMG_RESTORED,
                  &s_arImg, &s_arMem, &s_arView)) {
        /* `mk_image` CAN FAIL AFTER vkCreateImage AND vkAllocateMemory SUCCEEDED
           -- no device-local memory type, a failed bind, a failed view. Nulling
           the handles here lost the image while leaving `s_arMem` set, so the
           next kill_image would vkFreeMemory memory that still had an image
           bound to it. kill_image is what tagpu_vk_terr.c's shared_resize does
           for the identical case. [FOUND BY THE GATE-2 LANDING REVIEW.] */
        kill_image(d, &s_arImg, &s_arMem, &s_arView);
        plog(d, "feat: no %d MB device image for the Classic++ restored twin - "
                "the pass keeps standing down on a restored frame",
             (dim * dim * 4) >> 20);
    }
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet wr[2];
        if (!s_slot[i].dset) continue;
        memset(&ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
        ii.sampler = s_samp; ii.imageView = s_atView;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = s_slot[i].dset; wr[0].dstBinding = 40; wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[0].pImageInfo = &ii;
        /* BINDING 42 IS uAtlasRGB, AND SINCE GATE 2 IT NAMES THE RESTORED
           TWIN'S OWN IMAGE. It was the indexed view as a placeholder, because
           the pass refused every frame that would have sampled it; now that the
           mirror exists the branch is reachable and the binding has to be the
           real thing. When the image could not be created it goes back to being
           the placeholder -- a descriptor must be VALID for the set to bind,
           and the restored stand-down then keeps the branch unreachable exactly
           as before. */
        irgb = ii;
        if (s_arView) irgb.imageView = s_arView;
        wr[1] = wr[0]; wr[1].dstBinding = 42; wr[1].pImageInfo = &irgb;
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

/* THE RESTORE, FED FROM THE PUBLISHED LIST (landing 7d).

   Called once a frame after the atlas upload, because the FILL pass reads the
   indexed atlas and a restore issued before this frame's copy would paint the
   palette's entry 0 over whatever the atlas had not received yet.

   IT IS A CURSOR, AND THE GENERATION IS THE ONLY THING THAT RESTARTS IT. A
   feature atlas is a lazy queue: the producer appends one frame per miss
   for the life of the atlas, so the steady state here is "add the few frames
   past my cursor and advance it". Everything that could make the cursor a lie
   -- the rects moving, the destination blanking, the palette moving -- arrives
   as a new generation, and then the job is rebuilt from index 0. */
static void restore_want(const TAGPU_VKPASS* d, const TAGPU_FEATHAND* h)
{
    int repaint, n;

    if (!h->restoreFrames || h->restoreGen == 0) {
        /* No request: the lever was never on, or the producer's list was
           dropped. Either way this job describes nothing now -- and what it
           painted is left standing, because the mirror path is what takes over
           and it uploads over the same image. */
        if (s_rjob) {
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
            s_rjSrcView = VK_NULL_HANDLE;
            /* AND WHAT IT PAINTED IS NO LONGER A PICTURE. The request going
               away while a job existed means the producer's list DIED -- its
               only such path is the out-of-memory drop -- and nothing will
               feed this twin again: the read-back cannot take over, because
               both arm latches are one-way and the mirror was freed when the
               list armed. Leaving `s_arHave` set would have this pass draw a
               frozen twin while the GL lane goes on restoring, which is a
               silent divergence rather than a stand-down. Only inside the
               `if` -- with the lever off there is no job and this branch runs
               every frame, where clearing it would break the mirror path.
               [FROM THE LANDING-7d REVIEW.] */
            s_arHave = 0;
        }
        return;
    }
    /* THE SOURCE MOVED UNDER A LIVE JOB. `atlas_upload` refuses a dimension
       change and `atlas_build` is what re-creates the image, so this is the
       narrow case rather than the common one -- and it is the one that reads
       from a destroyed view if nothing checks. */
    if (s_rjob && s_rjSrcView && s_atView && s_rjSrcView != s_atView) {
        plog(d, "feat: the indexed atlas moved under a live restore - dropping it "
                "and starting over on the new one");
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
        s_rjSrcView = VK_NULL_HANDLE;
        s_arHave = 0;                      /* what it holds is the old layout's */
    }
    if (s_rjob && s_rjGen == h->restoreGen) {
        int painted = tagpu_vk_restore_job_painted(s_rjob);
        /* ONE PAINTED FRAME IS WHAT MAKES THIS A PICTURE, and `s_arHave` is
           what the pass's restored refusal reads -- the same role the mirror
           upload gives it. */
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
               abandoned, not at the moment it looks wrong.
               [FROM THE LANDING-7d REVIEW.] */
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
        n = h->restoreN - s_rjTaken;
        if (n > 0) {
            int took = tagpu_vk_restore_job_add(s_rjob, h->restoreFrames + s_rjTaken, n);
            /* THE CURSOR ADVANCES BY WHAT WAS OFFERED, NOT BY WHAT WAS TAKEN.
               `tagpu_rcore_job_add` SKIPS a frame it can never queue -- a
               degenerate rect, or one larger than the activation slot even
               after the wrap demote -- and returns only the count it queued,
               so advancing by that re-offers the tail of the list on every
               frame for the life of the atlas: one duplicate restore per
               refused frame, for ever. A refused frame stays indexed until the
               next generation, which is what the log line says.
               THE EXCEPTION IS A WHOLE-CALL FAILURE (`took` 0 with frames
               offered): that is the queue's own realloc failing, which is
               transient and already logged by the core, so the cursor stays
               where it is and the next frame offers them again.
               [FROM THE LANDING-7d REVIEW.] */
            if (took > 0) {
                s_rjTaken += n;
                if (took < n)
                    plog(d, "feat: %d of %d new restore frames were refused by the "
                            "restorer (degenerate or larger than a slot) - they stay "
                            "indexed until the next generation", n - took, n);
            }
        }
        return;
    }
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; s_rjTaken = 0; }
    /* A NEW GENERATION WITH THE RESTORE GIVEN UP ON: the rects have moved and
       nothing will repaint this twin, so it is not a picture any more -- the
       same fact the failed branch above records, reached by the other route. */
    if (s_rjTried) { s_arHave = 0; return; }
    /* BOTH SURFACES HAVE TO BE THERE, and the source has to have contents: a
       FILL over an atlas no copy has reached yet would paint entry 0 over the
       art. Neither is an error -- the next frame asks again. */
    if (!s_arImg || !s_arView || !s_atView || !s_atHave) return;
    /* THE CONSUMER IS WHAT ASKS THE DEVICE: `up` loads the model off disk, so
       a session that never publishes a request never pays for it. It latches
       its verdict, so this is one integer compare per frame after the first. */
    if (!tagpu_vk_restore_up(d)) { s_rjTried = 1; return; }
    /* A REPAINT ONLY OVER SOMETHING THIS PASS ACTUALLY PAINTED. The producer's
       flag says the GL twin's destination holds a restore; ours is a different
       image and may hold nothing, in which case a repaint would leave every
       cell it has not reached undefined. `s_arHave` is the local fact. */
    /* A REPAINT ONLY IF NOTHING WAS BLANKED SINCE THIS PASS LAST LOOKED.
       `restoreRepaint` describes the LATEST generation and a consumer sees
       only that one, so a recycle followed in the same producer frame by a
       palette move (which `tagpu_feat.c` does: it recycles a full atlas and
       calls `tagpu_gaf_atlas_restore` on the next line) would hand this pass
       "keep what you have" over an atlas the other lane has just cleared.
       The blank COUNT cannot be hidden that way. [FROM THE LANDING-7d
       REVIEW.] */
    repaint = h->restoreRepaint && s_arHave && h->restoreBlanks == s_rjBlanks;
    s_rjob = tagpu_vk_restore_job_new(d, "feat", 1, 0, repaint,
                                      s_atImg, s_atView, s_atDim, s_atDim,
                                      h->pal,
                                      s_arImg, s_arView, s_atDim, s_atDim);
    if (!s_rjob) { s_rjTried = 1; return; }   /* the reason is in the log      */
    s_rjTaken = tagpu_vk_restore_job_add(s_rjob, h->restoreFrames, h->restoreN);
    s_rjGen = h->restoreGen;
    s_rjBlanks = h->restoreBlanks;
    s_rjSrcView = s_atView;
    s_rjPainted = 0;
    if (!repaint) s_arHave = 0;            /* it is being blanked and repainted */
    plog(d, "feat: restoring the atlas HERE - %d of %d frames over %dx%d, "
            "generation %u%s", s_rjTaken, h->restoreN, s_atDim, s_atDim,
         h->restoreGen, repaint ? ", repaint" : "");
}

/* The atlas, when the mirror says its bytes moved. Returns 0 only when
   something was refused; "nothing to do" is a 1. */
static int atlas_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, SLOT* s,
                        const TAGPU_FEATHAND* h)
{
    VkDeviceSize bytes = 0, rbytes = 0;
    int rows = h->atlasRows, rrows = 0;
    int doIdx, doRgb;

    /* THE IMAGE AND ITS TWO DESCRIPTOR BINDINGS ARE MADE ONCE, BY `build`, AND
       THAT IS WHY THEY MAY BE WRITTEN AT ALL. vkUpdateDescriptorSets on a set
       some pending command buffer has bound is undefined behaviour, and the
       atlas binding is the one write in this file that touches EVERY slot's
       set rather than the one slot we own. It is safe because it happens on the
       first prepare after `build`, before any of those sets has ever been
       bound -- so a later change of the atlas's dimensions is refused here
       rather than silently rewriting sets that are in flight. The GL twin's
       ATLAS_DIM is a compile-time constant, so this cannot fire; it is the
       guard that keeps that true if it ever stops being one. */
    if (s_atDim != h->atlasDim) {
        plog(d, "feat: the atlas changed from %d texels square to %d - the pass "
                "comes down rather than rewrite descriptor sets that frames in "
                "flight are using", s_atDim, h->atlasDim);
        return 0;
    }
    doIdx = !(s_atHave && s_atSerial == h->atlasSerial);
    /* THE RESTORED TWIN IS DUE when its serial moved OR its row count did. The
       rows matter on their own because a mirror that SHRANK (a re-arm, a
       context loss: tagpu_gaf.c drops mirrorRgbRows to 0 in exactly those two
       places) would otherwise leave the rows above the new mark holding the
       previous twin's colours, and the serial need not move for that. */
    doRgb = s_arImg && h->atlasRgb && h->atlasRgbRows > 0 &&
            !(s_arHave && s_arSerial == h->atlasRgbSerial);
    if (!doIdx && !doRgb) {
        /* NOTHING TO SEND, SO THE 4 MB GOES BACK. This is the "given back at
           that slot's next prepare" the file header promises, and it is the
           whole of it: the seam has waited on this slot's fence, so the copy
           that read this buffer has completed and it is ours to destroy. A
           settled scene therefore holds no atlas staging at all, and a map
           still filling its atlas holds one buffer per slot that is uploading.
           The cost of being wrong is one vkCreateBuffer on the next frame that
           does upload -- which is a frame on which a GAF frame was decoded
           anyway. */
        slot_drop_astage(d, s);
        return 1;
    }
    if (rows < 1) rows = 1;
    if (rows > h->atlasDim) rows = h->atlasDim;
    if (doIdx) bytes = (VkDeviceSize)h->atlasDim * rows;

    if (doRgb) {
        rrows = h->atlasRgbRows;
        if (rrows > h->atlasDim) rrows = h->atlasDim;
        /* THE WHOLE SQUARE ON THE FIRST UPLOAD AND ON A SHRINK, `rrows` OF IT
           otherwise. The mirror's allocation is the full dim x dim x 4 and it
           was calloc'd, so reading every row of it is in bounds and the rows
           the restorer has not painted read as alpha 0 -- which is what an
           unpainted cell means. Uploading only `rrows` when the image already
           holds more would leave the difference standing. */
        if (!s_arHave || rrows < s_arReq) rrows = h->atlasDim;
        rbytes = (VkDeviceSize)h->atlasDim * rrows * 4;
    }

    if (s->acap < bytes + rbytes || !s->astage) {
        slot_drop_astage(d, s);
        if (!mk_buffer(d, bytes + rbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &s->astage, &s->amem, &s->amap)) return 0;
        s->acap = bytes + rbytes;
    }
    if (doIdx) memcpy(s->amap, h->atlas, (size_t)bytes);
    if (doRgb) memcpy(s->amap + bytes, h->atlasRgb, (size_t)rbytes);
    if (!doIdx) goto rgb_only;

    /* THE WRITE-AFTER-READ BARRIER, and it is the one place in this file that
       needs more than the slot's fence. The image is shared by every slot, so
       the frames still in flight may be sampling it; a barrier's first
       synchronisation scope includes everything submitted to this queue before
       it, which is all of them. A WAR hazard needs only an execution
       dependency -- the access masks here are for the LAYOUT TRANSITION, which
       is a write. On the very first upload there is nothing to order against
       and the image has no contents to preserve, so it goes in as UNDEFINED. */
    img_barrier(cb, s_atImg,
                s_atHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                s_atHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                         : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                s_atHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->astage, 0, s_atImg, h->atlasDim, rows);
    img_barrier(cb, s_atImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    s_atSerial = h->atlasSerial;
    s_atHave = 1;

rgb_only:
    /* THE SAME WRITE-AFTER-READ ARGUMENT AS ABOVE, on the second image: it is
       shared by every slot, frames in flight may be sampling it, and the
       barrier's first synchronisation scope covers everything already
       submitted to this queue. */
    if (doRgb) {
        /* THE SOURCE SCOPE NAMES THE RESTORE'S WRITE AS WELL AS A SAMPLE.
           `s_arHave` has two writers since landing 7d -- the mirror upload
           below, whose last toucher is a fragment READ, and this lane's own
           restore, whose last toucher is a RENDER PASS -- and this barrier
           used to name only the read. It is unreachable today, because the
           producer publishes a mirror or a frame list and never both, so
           `doRgb` and the restore cannot both be live; that is safety by
           exclusion, and the either/or was already got wrong once on this
           plan. Naming both scopes costs nothing and does not depend on it.
           [FROM THE LANDING-7d REVIEW, which flagged it as latent.] */
        img_barrier(cb, s_arImg,
                    s_arHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                             : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    s_arHave ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    s_arHave ? (VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) : 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        copy_rect(cb, s->astage, bytes, s_arImg, h->atlasDim, rrows);
        img_barrier(cb, s_arImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        s_arSerial = h->atlasRgbSerial;
        s_arReq = h->atlasRgbRows;
        s_arHave = 1;
    }
    return 1;
}

/* THE SCISSOR, in Vulkan framebuffer pixels. Item 3 of the file header: the
   rect arrives in GAME-FRAME pixels measured from the TOP of the frame (the
   engine's own viewport rect, what the native pass hands glScissor), and since
   landing 5b this pass's framebuffer row 0 IS that top row -- so the rect goes
   in unchanged. It was mirrored on the way in until 2026-09-17, which was right
   for as long as the viewport height was negative.

   It is also SCALED, by the attachment's extent over the game frame's. At the
   sizes an A/B is run at those are the same number, but the Vulkan window
   tracks the client rect and the GL lane's own render target need not match it;
   scaling here keeps the clip on the same part of the world when they differ,
   which is the same thing the vertex shader's division by uGame does. */
static void feat_scissor(uint32_t w, uint32_t h)
{
    float sx = s_hGw > 0.0f ? (float)w / s_hGw : 1.0f;
    float sy = s_hGh > 0.0f ? (float)h / s_hGh : 1.0f;
    int x0 = (int)(s_hVpL * sx + 0.5f);
    int ww = (int)(s_hVw * sx + 0.5f);
    int ytop = (int)(s_hVpT * sy + 0.5f);
    int hh = (int)(s_hVh * sy + 0.5f);
    int y0 = ytop;                          /* NOT mirrored: landing 5b */

    /* NO CLIP WHERE THE GL LANE HAS NONE. `scissorOn` is what the native pass
       actually did, not what it would have liked to; a Vulkan lane that clipped
       while GL did not would differ in every feature the gather's margin
       reaches past the viewport. */
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

int tagpu_vk_feat_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
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
    int feed;

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW, AND NOTHING IS KEPT
       ONCE THERE IS NOT -- §2.28's rule, and here it is worth more: the atlas
       staging alone is 4 MB a slot. Giving slot `slot` back at this point needs
       no new argument and no timer, because it is the same instant, and the
       same ownership, that the rest of this function writes it in. */
    if (!tagpu_feat_handover(&h, d->frame)) {
        if (s_state == ST_READY) slot_free(d, &s_slot[slot]);
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_feat_down(d); s_state = ST_REFUSED; return 0; }
        /* the shared atlas image and the two descriptor bindings that name it,
           before any set has been bound -- see `atlas_upload` */
        if (h.atlasDim < 1 || h.atlasDim > 8192 || !atlas_build(d, h.atlasDim)) {
            plog(d, "feat: a %d-texel-square atlas image was refused", h.atlasDim);
            tagpu_vk_feat_down(d); s_state = ST_REFUSED; return 0;
        }
        s_state = ST_READY;
    }

    /* WHAT IS LEFT OF THE RESTORED REFUSAL (see the file header): the atlas IS
       mirrored since gate 2, and drawing with uRestored 0 against a twin that
       drew with 1 would still be a different picture that the A/B would call a
       rasteriser difference -- so the frames without a mirror YET are refused
       and the rest are drawn. */
    /* A RESTORED FRAME IS DRAWABLE SINCE GATE 2, but only once the mirror has
       actually been read back: `atlasRgb` is NULL until the restorer has
       painted something and the read-back has run, and until then the GL twin
       is sampling colours this lane does not have. The refusal keeps its old
       shape and its old reason -- it is just no longer permanent.

       IT IS NO LONGER LATCHED, and that is the point of the change. The old
       message said "this session" and meant it; gpu-status 2.35 measured what
       that costs, which is that a pass which refuses once stays dark for the
       process even after the condition clears. The condition here clears on its
       own within a few frames of the restorer starting, so `s_saidRestored`
       gates the LOG LINE only, and it is cleared again below so a later genuine
       loss says so once more. */
    /* CAN THIS LANE DRAW A RESTORED FRAME? Either it holds a mirror of the GL
       twin (gate 2) -- and then the MIRROR is what it has to have this frame,
       because the producer zeroes it and drops its rows to 0 whenever the GL
       twin is re-armed, and a pass drawing its own stale copy of a twin the
       other lane has just blanked is two different pictures -- or the producer
       published a frame LIST and this lane painted the twin itself, where
       `s_arHave` is the local fact and the only one available.
       `s_arHave` ALONE IS NOT THE TEST, and it was in this landing's first
       draft: it is set by both writers, so accepting it on its own let the
       mirror path draw through exactly the window the refusal exists for.

       AND A REFUSAL HERE IS NOT ALWAYS A `return`. When the restore is this
       lane's own it needs THIS frame's indexed atlas uploaded before it can
       paint anything, and the upload is below -- so returning on the frames
       before the first paint would be a deadlock, not a stand-down: nothing
       drawn because nothing painted, nothing painted because the atlas never
       arrived. `feed` is that case, and it runs the uploads and then returns
       without claiming the frame. */
    feed = 0;
    if (h.restored && !(s_arImg && ((h.atlasRgb && h.atlasRgbRows > 0) ||
                                    (h.restoreFrames && s_arHave)))) {
        if (h.restoreFrames && s_arImg && !s_rjTried) feed = 1;
        if (!s_saidRestored) {
            s_saidRestored = 1;
            plog(d, "feat: the GL twin is drawing through the Classic++ restored "
                    "atlas and this lane has no restored twin of it yet - nothing "
                    "drawn until %s", feed ? "this lane's own restore paints one"
                                           : "the read-back produces rows");
        }
        if (!feed) return 0;
    } else s_saidRestored = 0;

    /* THE BOUNDS, RE-CHECKED. Every one of these sizes an allocation or a
       memcpy, and a bound that lives in the file that produced the number is a
       bound only while both files are read together. */
    if (!h.atlas) {
        /* SAID ONCE. The mirror is asked for on the GL twin's 30-frame poll and
           cannot be had before the atlas has its dimensions, so the first
           frames of a session legitimately arrive without one. */
        if (!s_saidNoMirror) {
            s_saidNoMirror = 1;
            plog(d, "feat: the GL atlas has no CPU mirror yet - nothing drawn "
                    "until it does (the twin asks for one on its 30-frame poll)");
        }
        return 0;
    }
    s_saidNoMirror = 0;
    if (!h.pal || !h.fogLut) return 0;
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
    if (!slot_build(d, s)) goto refuse;
    if (!slot_fog(d, s, fogW, fogH)) goto refuse;

    vbytes = (VkDeviceSize)(h.nShadow + h.nBody) * VST * sizeof(float);
    if (!slot_verts(d, s, vbytes)) goto refuse;
    /* the two buckets, contiguous and in the twin's own draw order */
    if (h.nShadow) memcpy(s->vmap, h.shadow, (size_t)h.nShadow * VST * sizeof(float));
    if (h.nBody)   memcpy(s->vmap + (size_t)h.nShadow * VST * sizeof(float),
                          h.body, (size_t)h.nBody * VST * sizeof(float));

    if (!atlas_upload(d, cb, s, &h)) goto refuse;

    /* THE RESTORE, AFTER THE UPLOAD IT READS AND BEFORE ANY DRAW THAT SAMPLES
       WHAT IT PAINTS. The draws themselves are issued by the seam, from
       `tagpu_vk_restore_step`, after every pass's prepare -- this call only
       creates and feeds the job. */
    restore_want(d, &h);
    /* A FEED FRAME ENDS HERE: the atlas is uploaded and the restore has its
       frames, and there is no restored twin to draw against yet. The frame is
       NOT claimed -- see the A/B note at the bottom of this function. */
    if (feed) return 0;

    /* THE THREE SMALL IMAGES, per slot, so the one-line invariant covers them:
       UNDEFINED in, because the whole of each is re-sent every frame and there
       are therefore no contents to preserve and no layout to carry. */
    memcpy(s->smallMap + SMALL_PALOFF, h.pal, 256 * 4);
    memcpy(s->smallMap + SMALL_LUTOFF, h.fogLut, 256);
    /* The grid is one `unsigned short` a cell and the image is RG8: the same
       two bytes in the same order, which is exactly what the GL twin uploads
       (GL_RG / GL_UNSIGNED_BYTE over this very buffer). With no grid this frame
       the image is one zero cell, which the shader never reads -- taFog is
       called only on the `uFog & 1` branch. */
    if (h.fogGrid) memcpy(s->smallMap + SMALL_FOGOFF, h.fogGrid,
                          (size_t)fogW * fogH * 2);
    else           memset(s->smallMap + SMALL_FOGOFF, 0, 2);
    img_barrier(cb, s->pal, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->lut, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->fog, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->smallStage, SMALL_PALOFF, s->pal, 256, 1);
    copy_rect(cb, s->smallStage, SMALL_LUTOFF, s->lut, 256, 1);
    copy_rect(cb, s->smallStage, SMALL_FOGOFF, s->fog, fogW, fogH);
    img_barrier(cb, s->pal, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->lut, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->fog, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    /* The two uniform blocks, at the std140 offsets the generated header
       prints -- the same numbers the GL draw passed to its uniforms. */
    memset(&ub, 0, sizeof ub);
    ub.f[0] = h.gw; ub.f[1] = h.gh;                    /* uGame       vec2 @0  */
    ub.f[2] = h.zoom;                                  /* uZoom      float @8  */
    ub.f[4] = h.zoomCx; ub.f[5] = h.zoomCy;            /* uZoomC      vec2 @16 */
    ub.f[6] = h.depthScale;                            /* uDepthScale float @24 */
    memcpy(s_umap + (size_t)slot * s_ustride, ub.f, UBLK);
    memset(&ub, 0, sizeof ub);
    ub.i[0] = h.restored;                              /* uRestored    int @0  */
    ub.i[1] = h.lit;                                   /* uLit         int @4  */
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
       frame claimed and then not drawn would have the seam capture a bare clear
       against a GL half that has the features in it, and report every feature
       pixel as differing: a port failure that is really an oracle failure. */
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
       refusal, not a rare one. [FOUND BY THE G19e LANDING REVIEW, 2026-09-15,
       by both reviewers independently.]

       So the pass stops drawing at once and OWES a teardown. The seam pays it
       at the top of a later frame, behind the vkDeviceWaitIdle that makes "no
       submit names these objects" a fact rather than a hope -- the same proof
       vk_down and vk_resize already use. The memory is still given back, which
       is what this path existed to do; it is given back where that is legal. */
    plog(d, "feat: slot %u would not take this frame's resources - the pass stops "
            "drawing and the seam tears it down", (unsigned)slot);
    s_state = ST_REFUSED;
    s_downOwed = 1;
    return 0;
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
       row and a POSITIVE height puts it on row 0 -- where the game's top row is
       under both APIs. It took a NEGATIVE height until landing 5b (2026-09-17),
       which turned the frame over a second time; this comment went on saying so
       for another four landings after the code stopped doing it, which is how
       the orientation question had to be re-derived three times in 4c.
       minDepth 0.5 / maxDepth 1.0 maps clip z in [0, 1]
       onto GL's own (z+1)/2 -- see item 1 of the file header; without it every
       depth VALUE here is twice GL's and a z-fight settles the other way. */
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
       here instead is what let one transient refusal plus a window drag latch
       the pass at ST_REFUSED for the life of the process.

       THE DEBT ITSELF IS DISCHARGED BY EVERY TEARDOWN, paid or not, and that is
       a separate fact from the verdict: once this function has run there is
       nothing left to free, so an un-cleared flag would have the seam drain the
       device and call `_down_paid` on an already-dead pass the next time it
       looked -- which would latch ST_REFUSED by the back door and lose exactly
       what the two lines above win. (Found while re-reading this fix, not by a
       reviewer.) [G19e RE-REVIEW, 2026-09-15.] */
    int owed = s_downPaying;
    s_downOwed = 0;
    /* NOTHING TO FREE, BUT THE VERDICT STILL STANDS. `owed` says the device
       refused this pass its resources, and that is a fact about the pass and
       not about whether the entry points resolved -- so it is latched here
       too, exactly as below. [G19e RE-REVIEW, 2026-09-15.] */
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
    s_rjGen = 0; s_rjBlanks = 0; s_rjTaken = 0; s_rjPainted = 0; s_rjTried = 0;
    s_rjSrcView = VK_NULL_HANDLE;
    kill_image(d, &s_atImg, &s_atMem, &s_atView);
    kill_image(d, &s_arImg, &s_arMem, &s_arView);
    s_arReq = 0; s_arSerial = 0; s_arHave = 0;
    s_atDim = 0; s_atSerial = 0; s_atHave = 0;
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
   still the reason. [G19e RE-REVIEW, 2026-09-15.] */
void tagpu_vk_feat_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_feat_down(d);          /* clears s_downOwed itself */
    s_downPaying = 0;
}
