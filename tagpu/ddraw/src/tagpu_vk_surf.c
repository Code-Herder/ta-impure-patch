/* THE REFERENCE TEXTURE: TA's own 8-bit screen, on the device, drawn nowhere.
   The header carries the argument.

   THIS PASS USED TO BE THE FRAME'S BOTTOM LAYER -- it resolved TA's indices
   through the presented palette and blitted them under everything we draw.
   THE CLEAN CUT DELETED THAT DRAW and everything that served it: the pipeline,
   the descriptor sets, the sampler, the vertex and uniform buffers, the quad,
   `record`, and the GLSL the fork's GL lane shared with it. Nothing the engine
   rasterises reaches the screen any more, and there is no lever to bring it
   back -- the code is gone rather than gated.

   WHAT IS LEFT IS THE UPLOAD, AND IT IS THE POINT. `tagpu_surf_capture` takes
   TA's composed frame on the GAME thread, at the one point in the process where
   it is finished (tagpu_surf.h); this puts it on the device as a per-slot R8
   index image with its palette beside it, and `tagpu_vk_surf_engine_view`
   hands that view out. It is the golden source: the picture the 1997 software
   rasteriser drew, kept so our passes can be checked against it. It is a
   reference and never a layer.

   IT HAS NO CONSUMER IN THE TREE TODAY, and that is deliberate rather than an
   oversight. The one consumer it had was the UI layer's composite, which went
   with the cut. Keeping the upload alive costs one memcpy and one
   vkCmdCopyBufferToImage per changed frame -- and the serial gate below means
   a still screen pays neither -- which is the price of having the golden
   source addressable from a shader the moment something wants to diff against
   it. Dropping it would make the next comparison a rebuild rather than a call.
   [The vulkan-only plan, THE CLEAN CUT.] */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_vk_surf.h"
#include "tagpu_surf.h"

#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties)

/* NO PIPELINE, NO DESCRIPTORS, NO SAMPLER AND NO DRAW COMMAND. This list is
   the whole of what an upload needs, and the twenty entry points the blit
   needed went with it. A pass that resolves only what it uses cannot quietly
   grow a draw back. */
#define DFNS(X) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

#define PAL_W 256                      /* the palette image, 256 x 1 RGBA */

static int s_state;
static int s_downOwed;
static int s_downPaying;
/* WHAT THIS PASS HAS ACTUALLY DONE, reported periodically rather than latched
   once: the two upload counts are the evidence that the surface and the palette
   are gated SEPARATELY, which a one-shot line could not show and which the
   landing review found this pass had wrong. A fade moves `pal` and not `bytes`;
   a still frame moves neither. */
static unsigned s_nBytes, s_nPal, s_nFrames, s_saidAt;


typedef struct {
    VkImage         img,  pimg;
    VkDeviceMemory  imem, pmem;
    VkImageView     view, pview;
    VkBuffer        stage;
    VkDeviceMemory  smem;
    unsigned char*  smap;
    int             w, h;              /* what this slot is sized for, 0 = nothing */
    unsigned        serial, palSerial; /* the serials this slot's images hold      */
    int             haveSerial;
} SLOT;
static SLOT s_slot[TAGPU_VK_SLOTS];

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
    if (type < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) return 0;
    if (map) {
        if (vkMapMemory(d->dev, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
        *map = (unsigned char*)p;
    }
    return 1;
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

static int mk_image(const TAGPU_VKPASS* d, int w, int h, VkFormat fmt,
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
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, *img, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) return 0;
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

static void slot_free(const TAGPU_VKPASS* d, SLOT* s)
{
    VkDevice dev = d->dev;
    if (s->view)  { vkDestroyImageView(dev, s->view, NULL); s->view = VK_NULL_HANDLE; }
    if (s->img)   { vkDestroyImage(dev, s->img, NULL); s->img = VK_NULL_HANDLE; }
    if (s->imem)  { vkFreeMemory(dev, s->imem, NULL); s->imem = VK_NULL_HANDLE; }
    if (s->pview) { vkDestroyImageView(dev, s->pview, NULL); s->pview = VK_NULL_HANDLE; }
    if (s->pimg)  { vkDestroyImage(dev, s->pimg, NULL); s->pimg = VK_NULL_HANDLE; }
    if (s->pmem)  { vkFreeMemory(dev, s->pmem, NULL); s->pmem = VK_NULL_HANDLE; }
    /* THE UNMAP IS GUARDED BY THE MAP POINTER, NOT BY THE ALLOCATION: a
       vkMapMemory that failed leaves the allocation standing and unmapping it
       would be an error of its own -- and `slot_size` unwinds through here on
       exactly that path. */
    if (s->smap)  { vkUnmapMemory(dev, s->smem); s->smap = NULL; }
    if (s->stage) { vkDestroyBuffer(dev, s->stage, NULL); s->stage = VK_NULL_HANDLE; }
    if (s->smem)  { vkFreeMemory(dev, s->smem, NULL); s->smem = VK_NULL_HANDLE; }
    s->w = s->h = 0;
    s->haveSerial = 0;
}

/* Make slot `s` carry a `w` x `h` surface and its palette. Safe because the
   caller owns this slot: the seam waited on fence[slot] at the top of the
   frame, so the submit that last used these images, this staging buffer and
   this descriptor set has completed. */
static int slot_size(const TAGPU_VKPASS* d, SLOT* s, int w, int h)
{
    if (s->w == w && s->h == h) return 1;
    slot_free(d, s);

    if (!mk_image(d, w, h, VK_FORMAT_R8_UNORM, &s->img, &s->imem, &s->view)) return 0;
    if (!mk_image(d, PAL_W, 1, VK_FORMAT_R8G8B8A8_UNORM, &s->pimg, &s->pmem, &s->pview))
        return 0;

    /* ONE STAGING BUFFER FOR BOTH, the surface first and the palette after it.
       They are uploaded in the same `prepare` and neither outlives it, so a
       second allocation would buy nothing but a second failure path. */
    if (!mk_buffer(d, ((((VkDeviceSize)w * h) + 3u) & ~(VkDeviceSize)3u) + PAL_W * 4,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->stage, &s->smem, &s->smap)) return 0;


    s->w = w; s->h = h;
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "surf: %u frame slots is outside what this pass carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    /* NO `flipok` GATE HERE. This pass takes no clip-space flip, so
       VK_KHR_maintenance1 buys it nothing, and refusing to arm without it would
       leave route E on the seam's flat clear -- the exact blind spot this pass
       exists to close -- while the log sent the next reader to look for a flip
       that is not there. tagpu_vk_pass.h says it in terms: the passes that must
       not flip "no longer ask for this at all".
       [FROM THE 4c-1 LANDING REVIEW.] */
    if (!resolve(d)) { plog(d, "surf: an entry point is missing"); return 0; }



    plog(d, "surf: the reference texture is up - TA's own frame on the device, "
            "%u frame slots, drawn nowhere", (unsigned)d->slots);
    return 1;
}

int tagpu_vk_surf_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_SURFFRAME sf;
    SLOT* s;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy rg;
    int fresh, freshPal;

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW, AND NOTHING IS KEPT
       ONCE THERE IS NOT -- the scaffold pass's reasoning, and the numbers are
       larger here: the per-slot image and staging buffer are the part of this
       pass that scales with the surface. A non-8bpp mode, a palette not
       readable yet or a frame with no viewport all arrive here, and after one
       turn of the slots the pass holds nothing that scales with anything. */
    if (!tagpu_surf_frame(&sf)) {
        if (s_state == ST_READY) slot_free(d, &s_slot[slot]);
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_surf_down(d); s_state = ST_REFUSED; return 0; }
        s_state = ST_READY;
    }

    /* THE BOUND, RE-CHECKED. tagpu_surf.c gates its own copy at
       TAGPU_SURF_MAXDIM, but a #define in one file bounding a vkCreateImage and
       a memcpy in another is a bound only while both are read together. */
    if (sf.w < 1 || sf.h < 1 || sf.w > TAGPU_SURF_MAXDIM || sf.h > TAGPU_SURF_MAXDIM) {
        plog(d, "surf: a %dx%d surface is outside what this pass carries - nothing drawn",
             sf.w, sf.h);
        /* AND THE SLOT STOPS BEING ADDRESSABLE. This return used to leave
           `haveSerial` set, so `tagpu_vk_surf_engine_view` would hand this
           slot's image -- TAGPU_VK_SLOTS frames old -- to the UI layer as the
           current frame. Unreachable today (the capture refuses an
           out-of-range surface before it marks the snapshot usable, so
           `tagpu_surf_frame` cannot return one), which is exactly why it would
           have survived until an edit here made it live. [FOUND by landing
           10's review.] */
        if (s_state == ST_READY) s_slot[slot].haveSerial = 0;
        return 0;
    }

    s = &s_slot[slot];
    if (!slot_size(d, s, sf.w, sf.h)) {
        plog(d, "surf: slot %u would not take a %dx%d surface - the pass stops "
                "drawing and the seam tears it down", (unsigned)slot, sf.w, sf.h);
        s_state = ST_REFUSED;
        s_downOwed = 1;
        return 0;
    }

    /* THE UPLOAD IS SKIPPED WHEN THIS SLOT ALREADY HOLDS THESE BYTES. TA
       redraws its whole screen far less often than we present, and the serial
       moves only when the bytes did (tagpu_surf.c) -- so a paused game uploads
       once and then not again until something changes. The image is left in
       SHADER_READ_ONLY_OPTIMAL by the barrier below and stays there, which is
       what makes the skip legal: the layout a skipped frame needs is the layout
       the last upload into THIS slot left it in.

       THE PALETTE IS ASKED SEPARATELY, AND IT HAS TO BE. It moves independently
       of the indices: a FADE is one picture held still while the table runs
       down to black, and a gamma change rescales every entry under a static
       screen. Gating it on the BYTES' serial froze the bottom layer's colours
       for the whole of a fade -- no fade at all, then a snap when something
       finally redrew. That was not a bound, it was the hope that the two move
       together, and they do not. [FROM THE 4c-1 LANDING REVIEW.] */
    fresh    = !s->haveSerial || s->serial != sf.serial;
    freshPal = !s->haveSerial || s->palSerial != sf.palSerial;
    if (fresh || freshPal) {
        /* THE PALETTE'S OFFSET IS ROUNDED UP TO 4. `vkCmdCopyBufferToImage`
           requires `bufferOffset` to be a multiple of the texel block size, and
           the palette's is RGBA8 = 4 bytes. `w * h` is a multiple of 4 for
           every display mode TA has, which is exactly the kind of fact that
           stops being true one day -- and the bound this file re-checks admits
           any 1..TAGPU_SURF_MAXDIM, so an odd-by-odd primary would produce an
           illegal offset and a palette read out of phase by a channel. The
           staging buffer is sized with the slack.
           [FROM THE 4c-1 LANDING REVIEW.] */
        VkDeviceSize palOff = (((VkDeviceSize)sf.w * sf.h) + 3u) & ~(VkDeviceSize)3u;

        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        memset(&rg, 0, sizeof rg);
        rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg.imageSubresource.layerCount = 1;
        rg.imageExtent.depth = 1;

        /* EACH IMAGE IS ASKED ITS OWN QUESTION. An image whose serial has not
           moved is left alone entirely -- no barrier, no copy -- and stays in
           SHADER_READ_ONLY_OPTIMAL from its last upload into THIS slot, which
           is the layout a skipped frame needs. UNDEFINED going in, because a
           re-sent image preserves nothing. */
        if (fresh) {
            memcpy(s->smap, sf.bytes, (size_t)sf.w * sf.h);
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.image = s->img;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            rg.bufferOffset = 0;
            rg.imageExtent.width = (uint32_t)sf.w;
            rg.imageExtent.height = (uint32_t)sf.h;
            vkCmdCopyBufferToImage(cb, s->stage, s->img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            s->serial = sf.serial;
            s_nBytes++;
        }
        if (freshPal) {
            memcpy(s->smap + palOff, sf.pal, PAL_W * 4);
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.image = s->pimg;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            rg.bufferOffset = palOff;
            rg.imageExtent.width = PAL_W;
            rg.imageExtent.height = 1;
            vkCmdCopyBufferToImage(cb, s->stage, s->pimg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            s->palSerial = sf.palSerial;
            s_nPal++;
        }

        /* BOTH IMAGES HOLD A FRAME NOW, which is what `haveSerial` means: it is
           the two serials' validity, so it is set only once both have been
           through an upload at this size. `slot_free` clears it. */
        s->haveSerial = 1;
    }

    /* READIED, NEVER DRAWN. `sf.dx..sf.dh` -- where TA put this frame inside
       the window -- are deliberately NOT kept any more: they existed for
       `record`'s viewport and there is no record. The reference is the image
       and its extent (`tagpu_vk_surf_engine_view` hands both out); where the
       engine would have placed it on screen is a property of a composite that
       no longer happens, and holding it would invite one back. */
    s_nFrames++;
    if (d->frame - s_saidAt >= 300) {
        s_saidAt = d->frame;
        plog(d, "surf: frame %u: %dx%d readied as the reference, %u frame(s), "
                "%u byte upload(s) and %u palette upload(s) - drawn nowhere",
             (unsigned)d->frame, sf.w, sf.h,
             s_nFrames, s_nBytes, s_nPal);
    }
    return 1;
}

VkImageView tagpu_vk_surf_engine_view(uint32_t slot, int* w, int* h)
{
    const SLOT* s;
    if (w) *w = 0;
    if (h) *h = 0;
    if (s_state != ST_READY || slot >= TAGPU_VK_SLOTS) return VK_NULL_HANDLE;
    s = &s_slot[slot];
    /* `haveSerial` and not just `view`: a slot can own a correctly sized image
       that nothing has been uploaded into yet, and its contents are then
       whatever the allocation came with. `slot_free` clears all four together,
       so this cannot see a stale view with a live size. */
    if (!s->view || !s->haveSerial || s->w < 1 || s->h < 1) return VK_NULL_HANDLE;
    if (w) *w = s->w;
    if (h) *h = s->h;
    return s->view;
}

void tagpu_vk_surf_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    if (!d || !d->dev) return;
    if (!vkDestroyImageView) return;        /* never resolved: nothing was made */
    for (i = 0; i < TAGPU_VK_SLOTS; i++) slot_free(d, &s_slot[i]);
    if (s_state != ST_REFUSED) s_state = ST_UNBUILT;
    s_downOwed = 0;
    s_downPaying = 0;
}

int tagpu_vk_surf_down_owed(void) { return s_downOwed; }

void tagpu_vk_surf_down_paid(const TAGPU_VKPASS* d)
{
    if (!s_downOwed || s_downPaying) return;
    s_downPaying = 1;
    tagpu_vk_surf_down(d);
}
