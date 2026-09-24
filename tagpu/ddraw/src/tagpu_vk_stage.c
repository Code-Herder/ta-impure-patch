/* The bounded upload. tagpu_vk_stage.h carries the argument; this is the
   mechanism. */

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_stage.h"
#include "tagpu_pal.h"                     /* tagpu_pal_expand */
#include "tagpu_gaf.h"                     /* TAGPU_GAFMOVE */

#define IFNS(X) X(vkGetPhysicalDeviceMemoryProperties)

#define DFNS(X) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkGetFenceStatus) \
    X(vkQueueSubmit) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdClearColorImage)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

#define ALIGN4(v) (((v) + 3) & ~(VkDeviceSize)3)

/* THE CAP HOLDS THE ALLOWANCE: everything an atlas may have due at once
   (tagpu_gaf.h `budget`) goes into one slot's staging, and the dirty map
   never names more rects for it than one copy takes. */
typedef char stage_cap_holds_budget[
    TAGPU_VK_STAGE_CAP >= (VkDeviceSize)TAGPU_GAF_BUDGET * TAGPU_GAF_TILEW * TAGPU_GAF_TILEH * 4
        ? 1 : -1];
typedef char stage_rects_hold_budget[TAGPU_VK_STAGE_MAXRECT >= TAGPU_GAF_BUDGET ? 1 : -1];

/* The device the entry points above were resolved for. A new device -- the
   lane came down and back up -- resolves them again. */
static VkDevice      s_dev;
/* THE BANDED PATH'S OWN COMMAND BUFFER, made on the first band and kept for
   the device's life: one pool, one buffer, one fence, used strictly one submit
   at a time. The fence is waited on before the buffer is recorded again, so the
   buffer is never re-recorded while pending. */
static VkCommandPool   s_pool;
static VkCommandBuffer s_cb;
static VkFence         s_fence;
/* A SUBMIT NOT YET SEEN TO COMPLETE: set by a submit, cleared once its fence
   has been waited on and reset. A wait that gave up leaves it set, and the
   buffer is not recorded again, nor the fence reused, until the device reports
   that submit done (`banded_ready`). */
static int             s_inflight;
static int             s_saidRefused;      /* the halving reached the floor    */

static void slog(const TAGPU_VKPASS* d, const char* fmt, ...)
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
    if (s_dev == d->dev && vkQueueSubmit) return 1;
#define RES_I(n) n = (PFN_##n)d->gipa(d->inst, #n); if (!n) return 0;
#define RES_D(n) n = (PFN_##n)d->gdpa(d->dev, #n); if (!n) return 0;
    IFNS(RES_I)
    DFNS(RES_D)
#undef RES_I
#undef RES_D
    s_dev = d->dev;
    return 1;
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

/* One host-coherent TRANSFER_SRC buffer, mapped once and left mapped: a write
   is visible to the device at the next submit without a flush. On a refusal
   every piece made so far is given back, so the slot holds nothing. */
static int mk_stage(const TAGPU_VKPASS* d, TAGPU_VKSTAGE* st, VkDeviceSize size)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    int type;
    void* p = NULL;

    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, &st->buf) != VK_SUCCESS) {
        st->buf = VK_NULL_HANDLE;
        return 0;
    }
    vkGetBufferMemoryRequirements(d->dev, st->buf, &req);
    type = mem_type(d, req.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || vkAllocateMemory(d->dev, &mai, NULL, &st->mem) != VK_SUCCESS ||
        vkBindBufferMemory(d->dev, st->buf, st->mem, 0) != VK_SUCCESS ||
        vkMapMemory(d->dev, st->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        tagpu_vk_stage_drop(d, st);
        return 0;
    }
    st->map = (unsigned char*)p;
    st->cap = size;
    return 1;
}

void tagpu_vk_stage_drop(const TAGPU_VKPASS* d, TAGPU_VKSTAGE* st)
{
    /* THE UNMAP IS GUARDED BY THE MAP POINTER, NOT BY THE ALLOCATION: a
       vkMapMemory that failed leaves the allocation standing and unmapping it
       would be an error of its own. */
    if (st->map) { vkUnmapMemory(d->dev, st->mem); st->map = NULL; }
    if (st->buf) { vkDestroyBuffer(d->dev, st->buf, NULL); st->buf = VK_NULL_HANDLE; }
    if (st->mem) { vkFreeMemory(d->dev, st->mem, NULL); st->mem = VK_NULL_HANDLE; }
    st->cap = 0;
    st->used = 0;
}

int tagpu_vk_stage_begin(const TAGPU_VKPASS* d, TAGPU_VKSTAGE* st,
                         VkDeviceSize want, VkDeviceSize floor)
{
    VkDeviceSize size;

    if (!resolve(d)) return 0;
    st->used = 0;
    floor = ALIGN4(floor);
    if (floor < 4) floor = 4;
    if (floor > TAGPU_VK_STAGE_CAP) return 0;   /* a row wider than the cap */
    size = ALIGN4(want);
    if (size > TAGPU_VK_STAGE_CAP) size = TAGPU_VK_STAGE_CAP;
    if (size < floor) size = floor;
    if (st->buf && st->cap >= size) return 1;
    tagpu_vk_stage_drop(d, st);
    /* A REFUSAL HALVES THE ASK, down to one row of the widest upload: a
       smaller buffer means more bands, never a frame that cannot be drawn. */
    for (;;) {
        if (mk_stage(d, st, size)) {
            s_saidRefused = 0;
            return 1;
        }
        if (size <= floor) break;
        size = ALIGN4(size / 2);
        if (size < floor) size = floor;
    }
    if (!s_saidRefused) {
        s_saidRefused = 1;
        slog(d, "stage: the device would not map even %u bytes of staging - the "
                "pass skips the frame and asks again on the next",
             (unsigned)floor);
    }
    return 0;
}

/* ---- the copy itself ----------------------------------------------------- */

typedef void (*FILL)(void* ctx, unsigned char* dst, int y, int rows);

static void barrier_in(VkCommandBuffer cb, VkImage img, int had)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    /* THE WRITE-AFTER-READ ORDER. The image is shared by every slot, so the
       frames still in flight may be sampling it; a barrier's first
       synchronisation scope includes everything submitted to this queue
       before it, which is all of them. With no contents to keep there is
       nothing to order against and the image comes in UNDEFINED. */
    b.oldLayout = had ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = had ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, had ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void barrier_out(VkCommandBuffer cb, VkImage img)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void copy_band(VkCommandBuffer cb, VkBuffer src, VkDeviceSize off, VkImage img,
                      int x, int y, int w, int h)
{
    VkBufferImageCopy rg;
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = off;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageOffset.x = x; rg.imageOffset.y = y;
    rg.imageExtent.width = (uint32_t)w; rg.imageExtent.height = (uint32_t)h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, src, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
}

static int banded_ready(const TAGPU_VKPASS* d)
{
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

    if (s_inflight) {
        if (vkGetFenceStatus(d->dev, s_fence) != VK_SUCCESS ||
            vkResetFences(d->dev, 1, &s_fence) != VK_SUCCESS) return 0;
        s_inflight = 0;
    }
    if (s_cb && s_fence) return 1;
    if (!d->queue) return 0;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = d->qfam;
    if (!s_pool && vkCreateCommandPool(d->dev, &pci, NULL, &s_pool) != VK_SUCCESS) {
        s_pool = VK_NULL_HANDLE;
        return 0;
    }
    cai.commandPool = s_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (!s_cb && vkAllocateCommandBuffers(d->dev, &cai, &s_cb) != VK_SUCCESS) {
        s_cb = VK_NULL_HANDLE;
        return 0;
    }
    if (!s_fence && vkCreateFence(d->dev, &fci, NULL, &s_fence) != VK_SUCCESS) {
        s_fence = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

/* One band through the banded path's own command buffer, submitted and waited
   on. 1 done, 0 the command buffer could not be made (nothing submitted),
   -1 the device failed the submit or the fence. */
static int banded_one(const TAGPU_VKPASS* d, const TAGPU_VKSTAGE* st, VkDeviceSize off,
                      VkImage img, int had, int first, int last,
                      int x, int y, int w, int h)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };

    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetCommandBuffer(s_cb, 0) != VK_SUCCESS ||
        vkBeginCommandBuffer(s_cb, &bi) != VK_SUCCESS) return first ? 0 : -1;
    if (first) barrier_in(s_cb, img, had);
    copy_band(s_cb, st->buf, off, img, x, y, w, h);
    if (last) barrier_out(s_cb, img);
    if (vkEndCommandBuffer(s_cb) != VK_SUCCESS) return first ? 0 : -1;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cb;
    if (vkQueueSubmit(d->queue, 1, &si, s_fence) != VK_SUCCESS) return -1;
    s_inflight = 1;
    /* THE ORDERING THE BUFFER'S REUSE RESTS ON: the next band is written into
       the same bytes only after this fence says the copy that read them has
       completed. A second, as the seam's own frame fence allows; a wait that
       gives up is the pass's refusal, and the staging it read stays alive until
       the seam's vkDeviceWaitIdle, because a refusal destroys nothing. */
    if (vkWaitForFences(d->dev, 1, &s_fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) return -1;
    if (vkResetFences(d->dev, 1, &s_fence) != VK_SUCCESS) return -1;
    s_inflight = 0;
    return 1;
}

/* A whole page: in the frame's command buffer when it fits, banded when not.
   Only the whole-page callers reach it (tagpu_vk_stage.h). */
static int upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKSTAGE* st,
                  VkImage img, int had, int x, int y, int w, int h, int bpp,
                  FILL fill, void* ctx)
{
    VkDeviceSize row = (VkDeviceSize)w * bpp;
    VkDeviceSize bytes = row * (VkDeviceSize)h;
    VkDeviceSize off = ALIGN4(st->used);
    int band, y0, rc;

    if (!st->buf || !st->map || w <= 0 || h <= 0 || !img) return 0;

    /* IT FITS: into the frame's own command buffer, as the passes always did.
       The bytes stay held until this frame's submit completes, which is the
       seam's fence on this slot. */
    if (off + bytes <= st->cap) {
        fill(ctx, st->map + off, 0, h);
        barrier_in(cb, img, had);
        copy_band(cb, st->buf, off, img, x, y, w, h);
        barrier_out(cb, img);
        st->used = off + bytes;
        return 1;
    }

    /* IT DOES NOT: bands of whole rows through what is left of the buffer, each
       waited on before the next is written. Nothing of this frame's command
       buffer is touched, so an earlier in-frame copy out of [0, off) is not
       disturbed, and the image leaves in SHADER_READ_ONLY_OPTIMAL. */
    band = off < st->cap ? (int)((st->cap - off) / row) : 0;
    if (band < 1) return 0;
    if (!banded_ready(d)) return 0;
    for (y0 = 0; y0 < h; y0 += band) {
        int n = h - y0 < band ? h - y0 : band;
        fill(ctx, st->map + off, y0, n);
        rc = banded_one(d, st, off, img, had, y0 == 0, y0 + n >= h, x, y + y0, w, n);
        if (rc <= 0) {
            /* 0 happens only before the first submit, so the image is as it
               was. After one, the image is half written and in a transfer
               layout: that is the device failing, and the pass comes down. */
            if (rc < 0 || y0 > 0) {
                slog(d, "stage: a banded upload failed on the device at row %d of %d", y0, h);
                return -1;
            }
            return 0;
        }
    }
    return 1;
}

/* ---- the two fills ------------------------------------------------------- */

typedef struct {
    const unsigned char *idx, *key, *pal, *alpha;
    int pitch, x, y, w;
} EXPAND;

static void fill_expand(void* ctx, unsigned char* dst, int y, int rows)
{
    const EXPAND* e = (const EXPAND*)ctx;
    tagpu_pal_expand(dst, e->idx, e->key, e->pitch, e->x, e->y + y, e->w, rows, e->pal, e->alpha);
}

typedef struct {
    const unsigned char* src;
    int pitch, rowBytes;
} COPY;

static void fill_copy(void* ctx, unsigned char* dst, int y, int rows)
{
    const COPY* c = (const COPY*)ctx;
    int i;
    for (i = 0; i < rows; i++)
        memcpy(dst + (size_t)i * c->rowBytes, c->src + (size_t)(y + i) * c->pitch,
               (size_t)c->rowBytes);
}

int tagpu_vk_stage_expand(const TAGPU_VKPASS* d, VkCommandBuffer cb,
                          TAGPU_VKSTAGE* st, VkImage img, int had,
                          const unsigned char* idx, const unsigned char* key,
                          int pitch, int x, int y, int w, int h,
                          const unsigned char* pal, const unsigned char* alpha)
{
    EXPAND e;
    if (!idx || !pal) return 0;
    e.idx = idx; e.key = key; e.pal = pal; e.alpha = alpha;
    e.pitch = pitch; e.x = x; e.y = y; e.w = w;
    return upload(d, cb, st, img, had, x, y, w, h, 4, fill_expand, &e);
}

/* The regions of one in-frame copy. Render thread only, and filled and used
   inside one call, so one static array serves every pass. */
static VkBufferImageCopy s_reg[TAGPU_VK_STAGE_MAXRECT];

int tagpu_vk_stage_expand_rects(const TAGPU_VKPASS* d, VkCommandBuffer cb,
                                TAGPU_VKSTAGE* st, VkImage img, int had,
                                const unsigned char* idx, const unsigned char* key,
                                int pitch, const int (*r)[4], int n,
                                const unsigned char* pal, const unsigned char* alpha)
{
    VkDeviceSize o, total = 0;
    int i, m = 0;

    (void)d;
    if (!idx || !pal || n < 0 || n > TAGPU_VK_STAGE_MAXRECT) return 0;
    if (!st->buf || !st->map || !img) return 0;
    for (i = 0; i < n; i++) {
        const int w = r[i][2] - r[i][0], h = r[i][3] - r[i][1];
        if (w > 0 && h > 0) total += (VkDeviceSize)w * (VkDeviceSize)h * 4;
    }
    if (total == 0) return 1;
    /* IN THIS FRAME OR NOT AT ALL: there is no banded branch here, so no
       partial upload can wait on the device (tagpu_vk_stage.h) */
    if (!tagpu_vk_stage_fits(st, total)) return 0;

    /* one barrier pair and one copy of `m` regions, in the frame's own
       command buffer */
    o = ALIGN4(st->used);
    for (i = 0; i < n; i++) {
        const int x = r[i][0], y = r[i][1], w = r[i][2] - x, h = r[i][3] - y;
        VkBufferImageCopy* rg = &s_reg[m];
        if (w <= 0 || h <= 0) continue;
        tagpu_pal_expand(st->map + o, idx, key, pitch, x, y, w, h, pal, alpha);
        memset(rg, 0, sizeof *rg);
        rg->bufferOffset = o;
        rg->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg->imageSubresource.layerCount = 1;
        rg->imageOffset.x = x; rg->imageOffset.y = y;
        rg->imageExtent.width = (uint32_t)w; rg->imageExtent.height = (uint32_t)h;
        rg->imageExtent.depth = 1;
        o += (VkDeviceSize)w * (VkDeviceSize)h * 4;
        m++;
    }
    barrier_in(cb, img, had);
    vkCmdCopyBufferToImage(cb, st->buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)m, s_reg);
    barrier_out(cb, img);
    st->used = o;
    return 1;
}

int tagpu_vk_stage_fits(const TAGPU_VKSTAGE* st, VkDeviceSize bytes)
{
    return st->buf && st->map && ALIGN4(st->used) + bytes <= st->cap;
}

/* ---- the move ------------------------------------------------------------ */

int tagpu_vk_stage_move_ready(const TAGPU_VKPASS* d, TAGPU_VKMOVEBUF* mb, int dim,
                              const TAGPU_GAFMOVE* m, int n)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    const VkDeviceSize need = (VkDeviceSize)dim * (VkDeviceSize)dim * 4;
    int i, type;

    if (!resolve(d) || dim <= 0 || n < 0 || (n > 0 && !m)) return 0;
    /* THE MOVES ARE BOUNDED HERE, as data: each cell inside the square at both
       ends, so no region below can name a texel the image or the buffer does
       not have */
    for (i = 0; i < n; i++)
        if (m[i].w == 0 || m[i].h == 0 || m[i].ox + m[i].w > dim || m[i].oy + m[i].h > dim ||
            m[i].nx + m[i].w > dim || m[i].ny + m[i].h > dim) return 0;
    if (mb->buf) return mb->size >= need;
    if (mb->refused) return 0;
    bci.size = need;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, &mb->buf) != VK_SUCCESS) {
        mb->buf = VK_NULL_HANDLE;
        mb->refused = 1;
        return 0;
    }
    vkGetBufferMemoryRequirements(d->dev, mb->buf, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (type < 0 || vkAllocateMemory(d->dev, &mai, NULL, &mb->mem) != VK_SUCCESS ||
        vkBindBufferMemory(d->dev, mb->buf, mb->mem, 0) != VK_SUCCESS) {
        tagpu_vk_stage_move_drop(d, mb);
        mb->refused = 1;
        slog(d, "stage: no %u MB of device memory to move an atlas repack in - "
                "a repack re-sends the page", (unsigned)(need >> 20));
        return 0;
    }
    mb->size = need;
    return 1;
}

/* The move's commands. THE ORDER: the image leaves SHADER_READ_ONLY only after
   every earlier access to it -- a sample, an upload, or a render pass's colour
   write, which is how a restored twin is written -- and the buffer is written
   only after every earlier move's read of it: a barrier's first scope is
   everything submitted to this queue before it. The cells are read back only
   once the copy into the buffer is complete; with `clear` the image is cleared
   between the two, and the cells land only after the clear. The image returns
   to SHADER_READ_ONLY with its writes visible to the fragment stage, the
   layout and the visibility every other upload here leaves it in. */
static void move_record(VkCommandBuffer cb, const TAGPU_VKMOVEBUF* mb, VkImage img, int dim,
                        const TAGPU_GAFMOVE* m, int n, int clear)
{
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    VkBufferImageCopy rg;
    int i, k, rows = 0;

    /* the rows the old cells occupy: what the buffer has to hold */
    for (i = 0; i < n; i++)
        if (m[i].oy + m[i].h > rows) rows = m[i].oy + m[i].h;

    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ib.subresourceRange.levelCount = 1;
    ib.subresourceRange.layerCount = 1;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = mb->buf;
    bb.size = VK_WHOLE_SIZE;

    ib.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &bb, 1, &ib);
    if (rows > 0) {
        memset(&rg, 0, sizeof rg);
        rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg.imageSubresource.layerCount = 1;
        rg.imageExtent.width = (uint32_t)dim;
        rg.imageExtent.height = (uint32_t)rows;
        rg.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mb->buf, 1, &rg);
    }

    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &bb, 1, &ib);
    if (clear) {
        VkClearColorValue cc;
        VkImageSubresourceRange rr;
        memset(&cc, 0, sizeof cc);
        rr = ib.subresourceRange;
        vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rr);
        /* the clear and the cells write the same texels: the cells last */
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &ib);
    }
    /* each cell from its old place in the buffer, whose rows are `dim` texels
       as the image's are, to its new place in the image */
    for (i = 0; i < n; ) {
        for (k = 0; k < TAGPU_VK_STAGE_MAXRECT && i < n; k++, i++) {
            VkBufferImageCopy* r = &s_reg[k];
            memset(r, 0, sizeof *r);
            r->bufferOffset = ((VkDeviceSize)m[i].oy * (VkDeviceSize)dim + m[i].ox) * 4;
            r->bufferRowLength = (uint32_t)dim;
            r->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r->imageSubresource.layerCount = 1;
            r->imageOffset.x = m[i].nx; r->imageOffset.y = m[i].ny;
            r->imageExtent.width = m[i].w; r->imageExtent.height = m[i].h;
            r->imageExtent.depth = 1;
        }
        vkCmdCopyBufferToImage(cb, mb->buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               (uint32_t)k, s_reg);
    }
    barrier_out(cb, img);
}

int tagpu_vk_stage_move(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKMOVEBUF* mb,
                        VkImage img, int dim, const TAGPU_GAFMOVE* m, int n)
{
    (void)d;
    if (!mb->buf || !img || dim <= 0 || n < 0 || (n > 0 && !m) ||
        (VkDeviceSize)dim * (VkDeviceSize)dim * 4 > mb->size)
        return 0;
    move_record(cb, mb, img, dim, m, n, 0);
    return 1;
}

int tagpu_vk_stage_move_twin(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKMOVEBUF* mb,
                             VkImage img, int dim, const TAGPU_GAFMOVE* m, int n)
{
    if (!resolve(d)) return 0;
    if (!mb->buf || !img || dim <= 0 || n < 0 || (n > 0 && !m) ||
        (VkDeviceSize)dim * (VkDeviceSize)dim * 4 > mb->size)
        return 0;
    move_record(cb, mb, img, dim, m, n, 1);
    return 1;
}

void tagpu_vk_stage_move_drop(const TAGPU_VKPASS* d, TAGPU_VKMOVEBUF* mb)
{
    if (d->dev && s_dev == d->dev) {
        if (mb->buf) vkDestroyBuffer(d->dev, mb->buf, NULL);
        if (mb->mem) vkFreeMemory(d->dev, mb->mem, NULL);
    }
    mb->buf = VK_NULL_HANDLE;
    mb->mem = VK_NULL_HANDLE;
    mb->size = 0;
    mb->refused = 0;                       /* a new device may say yes */
}

int tagpu_vk_stage_copy(const TAGPU_VKPASS* d, VkCommandBuffer cb,
                        TAGPU_VKSTAGE* st, VkImage img, int had,
                        const unsigned char* src, int pitch, int w, int h, int bpp)
{
    COPY c;
    if (!src) return 0;
    c.src = src; c.pitch = pitch; c.rowBytes = w * bpp;
    return upload(d, cb, st, img, had, 0, 0, w, h, bpp, fill_copy, &c);
}

void tagpu_vk_stage_down(const TAGPU_VKPASS* d)
{
    if (d->dev && s_dev == d->dev) {
        if (s_fence) vkDestroyFence(d->dev, s_fence, NULL);
        if (s_pool)  vkDestroyCommandPool(d->dev, s_pool, NULL);   /* frees s_cb */
    }
    s_fence = VK_NULL_HANDLE;
    s_pool = VK_NULL_HANDLE;
    s_cb = VK_NULL_HANDLE;
    s_dev = VK_NULL_HANDLE;
    s_inflight = 0;
    s_saidRefused = 0;
}
