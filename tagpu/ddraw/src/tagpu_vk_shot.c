/* tagpu_vk_shot.c -- one frame of the Vulkan lane, as a binary PPM. Contract:
   tagpu_vk_shot.h. Phase G / G19d.

   A BINARY PPM AND NOT A PNG, deliberately. This exists to be DIFFED, by
   tools/vk-ab.py and by whatever a session reaches for next, and a P6 file
   is a fifteen-byte header followed by the pixels in order -- no filter, no
   deflate, nothing between the comparison and the bytes the GPU wrote. The fork
   carries lodepng and the screenshot path uses it; this is not a screenshot.
   `ffmpeg -i x.ppm x.png` is one command when a human wants to look.

   THE SWAPCHAIN'S FORMAT IS BGRA ON THE REFERENCE SETUP AND THE FILE IS RGB, so
   the channels are swapped on the way out. Both orders are handled and anything
   else is refused by name rather than written in the wrong colour -- a capture
   that differs from its twin in every pixel because the channels were swapped
   reads as a broken port.

   NOTHING SURVIVES THE CAPTURE. The staging buffer is the size of the frame --
   8.3 MB at 1920x1080, which is real money in a 32-bit address space the lane
   spends its G19a budget measuring -- so it is allocated when the lever fires
   and given back as soon as the file is written. */

#include "tagpu_vk_pass.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tagpu_vk_shot.h"

#define IFNS(X) X(vkGetPhysicalDeviceMemoryProperties)
#define DFNS(X) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

static VkBuffer       s_buf;
static VkDeviceMemory s_mem;
static uint32_t       s_w, s_h;
static int            s_bgr;          /* the source is B,G,R,A rather than R,G,B,A */

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
#define RES_I(n) n = (PFN_##n)d->gipa(d->inst, #n); if (!n) return 0;
#define RES_D(n) n = (PFN_##n)d->gdpa(d->dev, #n); if (!n) return 0;
    IFNS(RES_I)
    DFNS(RES_D)
#undef RES_I
#undef RES_D
    return 1;
}

int tagpu_vk_shot_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, VkImage img,
                         VkImageLayout layout, uint32_t w, uint32_t h, VkFormat fmt)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkPhysicalDeviceMemoryProperties mp;
    VkMemoryRequirements req;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy rg;
    uint32_t i, type = 0xFFFFFFFFu;

    /* EVERY REFUSAL SAYS SO. These two returned 0 in silence, and since an
       arming now unlinks its own target, the operator's only evidence that a
       capture was asked for and not taken is this log -- an absent file with no
       line beside it reads as "the lever never fired", which is a different
       fault with a different fix. [FROM THE 4b-1 LANDING REVIEW, 2026-09-18.] */
    if (s_buf) {
        slog(d, "shot: a capture is already in flight - nothing captured for "
                "this arming");
        return 0;
    }
    if (!w || !h || w > 8192 || h > 8192) {
        slog(d, "shot: the image is %ux%u, outside 1..8192 - nothing captured",
             (unsigned)w, (unsigned)h);
        return 0;
    }
    if (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB) s_bgr = 1;
    else if (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB) s_bgr = 0;
    else {
        slog(d, "shot: swapchain format %d is not one of the four 8-bit RGBA "
                "orders this can read - nothing captured", (int)fmt);
        return 0;
    }
    if (!resolve(d)) { slog(d, "shot: an entry point is missing"); return 0; }

    bci.size = (VkDeviceSize)w * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, &s_buf) != VK_SUCCESS) {
        s_buf = VK_NULL_HANDLE;
        slog(d, "shot: the %u-byte staging buffer was refused - nothing captured",
             (unsigned)bci.size);
        return 0;
    }
    vkGetBufferMemoryRequirements(d->dev, s_buf, &req);
    vkGetPhysicalDeviceMemoryProperties(d->pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i; break;
        }
    if (type == 0xFFFFFFFFu) {
        slog(d, "shot: no host-visible coherent memory for a %ux%u readback", w, h);
        tagpu_vk_shot_down(d);
        return 0;
    }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s_mem) != VK_SUCCESS ||
        vkBindBufferMemory(d->dev, s_buf, s_mem, 0) != VK_SUCCESS) {
        slog(d, "shot: %u bytes of readback would not allocate", (unsigned)req.size);
        tagpu_vk_shot_down(d);
        return 0;
    }

    /* INTO TRANSFER_SRC AND BACK. The image is the swapchain's and the seam
       hands it over in whatever layout its render pass left it in; it must go
       back in that same layout or the present is reading an image in a layout
       it was not promised. */
    b.oldLayout = layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    /* BOTTOM_OF_PIPE, NOT COLOR_ATTACHMENT_OUTPUT. [FROM REVIEW 2026-09-15.] The
       render pass performs its own `finalLayout` transition as part of its
       final subpass dependency, whose destination stage is BOTTOM_OF_PIPE --
       which is LATER than COLOR_ATTACHMENT_OUTPUT, so a barrier sourced there
       is not ordered after that transition and could observe the image in the
       layout it was leaving. The access mask is 0 because BOTTOM_OF_PIPE
       carries none; the colour writes were already made available by that same
       dependency (`srcAccessMask = COLOR_ATTACHMENT_WRITE`), and this barrier's
       `TRANSFER_READ` destination is what makes them visible -- an execution
       dependency chain, which is the pattern the specification names for
       exactly this. */
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = w;
    rg.imageExtent.height = h;
    rg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_buf, 1, &rg);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = layout;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    s_w = w; s_h = h;
    return 1;
}

void tagpu_vk_shot_finish(const TAGPU_VKPASS* d, const char* path)
{
    void* p = NULL;
    const unsigned char* px;
    unsigned char* line;
    FILE* f;
    uint32_t x, y;

    if (!s_buf || !s_mem) return;
    if (vkMapMemory(d->dev, s_mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS || !p) {
        slog(d, "shot: the readback would not map");
        tagpu_vk_shot_down(d);
        return;
    }
    px = (const unsigned char*)p;
    /* THE ROW BUFFER BEFORE THE FILE. [FROM THE REVIEW'S SECOND PASS
       2026-09-15.] Allocating it after the header was written left a
       header-only PPM on disk when it failed, and `tools/vk-ab.py` would then
       report a MALFORMED capture rather than an absent one -- which reads as a
       broken writer instead of a machine that ran out of memory. */
    line = (unsigned char*)malloc((size_t)s_w * 3);
    if (!line) {
        slog(d, "shot: no memory for a row of the capture");
        vkUnmapMemory(d->dev, s_mem);
        tagpu_vk_shot_down(d);
        return;
    }
    f = fopen(path, "wb");
    if (!f) {
        slog(d, "shot: could not open the capture file");
        free(line);
        vkUnmapMemory(d->dev, s_mem);
        tagpu_vk_shot_down(d);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", s_w, s_h);
    /* ROW 0 IS THE TOP ROW HERE, and in a PPM too, so the rows go out in order.
       The GL twin's capture has to be turned over because glReadPixels hands
       back the bottom row first; this one does not, and that asymmetry is
       exactly the kind of thing that produces a capture differing from its twin
       in every text pixel and nothing else. */
    /* ONE fwrite A ROW. This runs on the render thread, and a stdio call per
       pixel is two million of them at 1080p -- most of the one frame a capture
       costs, for nothing. */
    for (y = 0; y < s_h; y++) {
        const unsigned char* row = px + (size_t)y * s_w * 4;
        for (x = 0; x < s_w; x++) {
            const unsigned char* q = row + (size_t)x * 4;
            line[x * 3 + 0] = s_bgr ? q[2] : q[0];
            line[x * 3 + 1] = q[1];
            line[x * 3 + 2] = s_bgr ? q[0] : q[2];
        }
        fwrite(line, 1, (size_t)s_w * 3, f);
    }
    free(line);
    fclose(f);
    vkUnmapMemory(d->dev, s_mem);
    slog(d, "shot: wrote %s, %ux%u", path, s_w, s_h);
    tagpu_vk_shot_down(d);
}

void tagpu_vk_shot_down(const TAGPU_VKPASS* d)
{
    if (s_buf && vkDestroyBuffer) { vkDestroyBuffer(d->dev, s_buf, NULL); }
    if (s_mem && vkFreeMemory)    { vkFreeMemory(d->dev, s_mem, NULL); }
    s_buf = VK_NULL_HANDLE;
    s_mem = VK_NULL_HANDLE;
    s_w = s_h = 0;
}
