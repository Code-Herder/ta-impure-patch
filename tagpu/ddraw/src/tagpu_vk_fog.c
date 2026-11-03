/* tagpu_vk_fog.c -- the fog source as one image a frame slot, shared by every world pass of its
   scene. Contract: tagpu_vk_fog.h.

   TWO INSTANCES: compiled for the game's world and, under TAGPU_SCENE_WIKI, for the wiki's stage,
   each with its own statics; tagpu_scene.h renames this one's entry points, and each world pass
   binds the instance its own scene was compiled with.

   WHAT A SLOT HOLDS. One R32_UINT image `stride` x `h` (device-local, never mapped), its view, one
   host-visible staging buffer of exactly its bytes, mapped once, the serial of the source it holds
   and a generation. The source is at most TAGPU_FOG_MAXDIM / 16 x TAGPU_FOG_MAXDIM words, 1 MiB;
   Seven Islands' is 100 KB. An image is re-created only when its extent changes, which is once
   per level; the words are copied only when the serial moves, at most once per sim tick and once
   per slot.

   THE ORDERING, WHICH IS THE INVARIANT. Slot i's image and staging, and every pass's slot-i
   descriptor that names its view, are written only in slot i's own `prepare` -- this module's,
   called by the seam once a frame after its wait on fence[i] and before the first pass's
   `prepare`, and then each pass's, which rewrites its slot-i descriptor whenever the generation
   it last wrote differs from this one. That fence proves every earlier submission that sampled
   slot i's image, or read its staging, has completed. So an image destroyed here is named by no
   submission in flight, and a pass's descriptor never names a destroyed view when it is next
   used: no pass draws from slot i without a `prepare` for slot i in the same frame, after this
   one. The upload is recorded into the frame's command buffer before every pass's draw, behind
   the barrier that makes it visible to the vertex and fragment stages.

   THE GENERATION IS NEVER REUSED, across slots, sources and devices: a pass whose own state
   outlived a teardown of this module (one that is not brought down with it) sees a generation it
   has not written and rewrites its descriptor.

   IT KNOWS NOTHING ABOUT A WINDOW and READS NO ENGINE STATE: the source arrives as an argument. */

#include "tagpu_scene.h"            /* first: the wiki instance's names */
#include "tagpu_vk_fog.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties)

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

#define FOG_FMT VK_FORMAT_R32_UINT

enum { ST_UNRESOLVED = 0, ST_READY = 1, ST_REFUSED = 2 };
static int s_state;
static int s_saidImage;
static unsigned s_genNext;

typedef struct {
    VkImage         img;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkBuffer        stage;
    VkDeviceMemory  stageMem;
    unsigned char*  stageMap;
    int             iw, ih;                /* the image's extent, texels       */
    unsigned        gen;                   /* 0 = no image                     */
    int             written;               /* the image has been uploaded: its
                                              layout is SHADER_READ_ONLY       */
    int             held;                  /* ...with the source `serial` names */
    unsigned        serial;
    int             w, h;                  /* the source it holds THIS frame, in
                                              cells; 0 x 0 = none              */
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

static void slot_free(const TAGPU_VKPASS* d, SLOT* s)
{
    VkDevice dev = d->dev;
    if (s->view) { vkDestroyImageView(dev, s->view, NULL); s->view = VK_NULL_HANDLE; }
    if (s->img)  { vkDestroyImage(dev, s->img, NULL);      s->img  = VK_NULL_HANDLE; }
    if (s->mem)  { vkFreeMemory(dev, s->mem, NULL);        s->mem  = VK_NULL_HANDLE; }
    /* the unmap is guarded by the map pointer: a vkMapMemory that failed leaves
       the allocation standing, and unmapping it would be an error of its own */
    if (s->stageMap) { vkUnmapMemory(dev, s->stageMem); s->stageMap = NULL; }
    if (s->stage)    { vkDestroyBuffer(dev, s->stage, NULL); s->stage = VK_NULL_HANDLE; }
    if (s->stageMem) { vkFreeMemory(dev, s->stageMem, NULL); s->stageMem = VK_NULL_HANDLE; }
    s->iw = s->ih = 0;
    s->gen = 0;
    s->written = s->held = 0;
    s->w = s->h = 0;
}

/* The image `iw` x `ih` and its staging, or 0 with nothing left half-made (the caller frees). */
static int slot_make(const TAGPU_VKPASS* d, SLOT* s, int iw, int ih)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    void* p = NULL;
    int type;

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = FOG_FMT;
    ici.extent.width = (uint32_t)iw;
    ici.extent.height = (uint32_t)ih;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, &s->img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, s->img, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s->mem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, s->img, s->mem, 0) != VK_SUCCESS) return 0;
    ivi.image = s->img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = FOG_FMT;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, &s->view) != VK_SUCCESS) return 0;

    bci.size = (VkDeviceSize)iw * (VkDeviceSize)ih * 4u;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, &s->stage) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(d->dev, s->stage, &req);
    type = mem_type(d, req.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s->stageMem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(d->dev, s->stage, s->stageMem, 0) != VK_SUCCESS) return 0;
    /* MAPPED ONCE AND LEFT MAPPED: the memory is HOST_COHERENT, so a write is
       visible to the device without a flush */
    if (vkMapMemory(d->dev, s->stageMem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
    s->stageMap = (unsigned char*)p;
    s->iw = iw; s->ih = ih;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
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

/* The staging's bytes into the whole image. UNDEFINED in, because the whole image is re-sent and
   there are no contents to keep; the slot's fence is what orders this after every earlier read. */
static void upload(VkCommandBuffer cb, SLOT* s)
{
    VkBufferImageCopy rg;
    barrier(cb, s->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = (uint32_t)s->iw;
    rg.imageExtent.height = (uint32_t)s->ih;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, s->stage, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    barrier(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    s->written = 1;
}

int tagpu_vk_fog_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, int slot, const TAGPU_FOGSRC* f)
{
    SLOT* s;
    int ok, iw, ih;

    if (slot < 0 || slot >= TAGPU_VK_SLOTS || (uint32_t)slot >= d->slots) return 0;
    if (s_state == ST_REFUSED) return 0;
    if (s_state == ST_UNRESOLVED) {
        VkFormatProperties fp;
        if (!resolve(d)) {
            plog(d, "fog: an entry point did not resolve - no pass draws fog");
            s_state = ST_REFUSED;
            return 0;
        }
        /* R32_UINT sampled is required of every device; asked rather than assumed */
        vkGetPhysicalDeviceFormatProperties(d->pd, FOG_FMT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            plog(d, "fog: this device cannot sample R32_UINT - no pass draws fog");
            s_state = ST_REFUSED;
            return 0;
        }
        s_state = ST_READY;
    }

    s = &s_slot[slot];
    s->w = s->h = 0;                       /* until it holds this frame's source */
    ok = tagpu_fog_src_ok(f);
    /* NO SOURCE KEEPS WHATEVER IMAGE THE SLOT HAS, and a slot with none gets one texel: the passes'
       descriptors must name a live view on every frame, and with a size of 0 x 0 no shader reads
       it */
    iw = ok ? f->stride : (s->img ? s->iw : 1);
    ih = ok ? f->h      : (s->img ? s->ih : 1);
    if (!s->img || s->iw != iw || s->ih != ih) {
        slot_free(d, s);
        if (!slot_make(d, s, iw, ih)) {
            slot_free(d, s);
            if (!s_saidImage) {
                s_saidImage = 1;
                plog(d, "fog: the device refused a %dx%d fog image - the world passes stand down",
                     iw, ih);
            }
            return 0;
        }
        s_saidImage = 0;
        if (++s_genNext == 0) ++s_genNext;
        s->gen = s_genNext;
    }
    if (ok) {
        if (!s->held || s->serial != f->serial) {
            memcpy(s->stageMap, f->words, (size_t)f->stride * (size_t)f->h * 4u);
            upload(cb, s);
            s->held = 1;
            s->serial = f->serial;
        }
        s->w = f->w; s->h = f->h;
        return 1;
    }
    if (!s->written) {
        memset(s->stageMap, 0, (size_t)iw * (size_t)ih * 4u);
        upload(cb, s);
        s->held = 0;
    }
    return 0;
}

VkImageView tagpu_vk_fog_view(int slot, unsigned* gen)
{
    if (slot < 0 || slot >= TAGPU_VK_SLOTS || !s_slot[slot].img) {
        if (gen) *gen = 0;
        return VK_NULL_HANDLE;
    }
    if (gen) *gen = s_slot[slot].gen;
    return s_slot[slot].view;
}

void tagpu_vk_fog_size(int slot, int* w, int* h)
{
    if (slot < 0 || slot >= TAGPU_VK_SLOTS) { *w = *h = 0; return; }
    *w = s_slot[slot].w;
    *h = s_slot[slot].h;
}

void tagpu_vk_fog_down(const TAGPU_VKPASS* d)
{
    int i;
    if (s_state == ST_READY)
        for (i = 0; i < TAGPU_VK_SLOTS; i++) slot_free(d, &s_slot[i]);
    s_state = ST_UNRESOLVED;
    s_saidImage = 0;
}
