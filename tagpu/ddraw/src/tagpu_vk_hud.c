/* tagpu_vk_hud.c -- the replay HUD, drawn by Vulkan. Contract: tagpu_vk_hud.h.

   THIS FILE IS THE RASTERISER AND NOTHING ELSE. Where things go is tagpu_hudlay.c's (the
   mockup's rules, in C); what each op becomes -- quads, glyphs, fills -- is tagpu_huddraw.c's;
   the shaders are that file's GLSL, compiled into inc/spirv/tagpu_huddraw.spv.h. Here: the
   images, the uploads, one pipeline state built twice -- against the seam's render pass and
   against the groups' layer's -- and a draw per run of instances that share a texture.

   ONE INSTANCE PER QUAD, a four-vertex strip each, so a frame's buffer is the instances alone:
   96 bytes a quad, HUD_MAXINST of them per frame slot. Each slot has its own range of one
   persistently mapped, coherent buffer, and the seam's fence wait at the top of its present is
   what proves the slot's range free (tagpu_vk_pass.h).

   THE PARTS ARE UPLOADED ONCE PER BUILD, whole, through tagpu_vk_stage_copy: a pass's first
   upload after it is built is one of the events the banded path is for (tagpu_vk_stage.h). That
   fills mip level 0 and leaves it SHADER_READ_ONLY_OPTIMAL; the rest of each chain is blitted
   down from it, linearly, in this frame's command buffer. The parts are premultiplied, so a
   linear average is the right one.

   THE GLYPH ATLAS changes in play, as text first needs a glyph at a size and a pen fraction. It
   goes whole once after a build, banded. After that only the rows that changed go -- a new
   generation's too, the atlas emptied because a glyph did not fit (tagpu_huddraw.h) -- IN THE
   FRAME'S OWN COMMAND BUFFER through the slot's staging buffer, behind a barrier whose first scope
   includes every frame already submitted (the order tagpu_vk_stage.c's in-frame copies rest on),
   as many as the buffer has room for; the rest go on the next frames, and until the last has
   gone the frame's glyphs are transparent (hide_mode). Text can be late; nothing waits.

   THE CHART'S IMAGE (tagpu_huddraw.h) is 157 KB of RGBA32F, sent whole in the frame's own
   command buffer when its generation moves, behind the same barrier; a frame that has no room for
   it draws no chart lines (hide_chart), and the next sends it. Its first generation goes before
   the HUD draws at all, so every draw that binds it binds a written image.

   THE IN-GAME WIKI IS A THIRD LAYER of the same pass (tagpu_vk_hud_record_wiki): its two op lists,
   the page's (tagpu_wikilay.c) and the stage's (tagpu_wikistage.c), are built by the same
   tagpu_huddraw into the same instance range after the HUD's two layers, on the same glyph atlas,
   with its kit's parts uploaded after the HUD's and numbered after them (a wiki batch's texture is
   its part's index plus s_nparts). Its stage's cursor is the game's own, drawn from one more image,
   the sprite sheet tagpu_wikistate.c decodes (HUD_TEX_SPRITE): 256 KB of RGBA8, sent whole as the
   chart is when its generation moves, cleared after the build so a draw always binds a written
   image, and its sprites hidden in a frame whose ops name a generation the device does not hold.

   IT KNOWS NOTHING ABOUT A WINDOW, and reads no engine state: everything arrives in its
   arguments or in the loaded HUD and wiki (tagpu_hudart.h), which are immutable. */

#include "tagpu_vk_pass.h"
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_hud.h"
#include "tagpu_vk_stage.h"
#include "tagpu_huddraw.h"
#include "tagpu_hudstate.h"
#include "tagpu_wikistate.h"
#include "spirv/tagpu_huddraw.spv.h"

/* a slot's quads: the HUD's two layers (a few thousand), and the wiki's page and stage, whose own
   op budget is WIKI_MAXOP; a quad past the range is dropped by the build, never written past it */
#define HUD_MAXINST   32768
#define HUD_MAXBATCH  1024
#define IBYTES        ((VkDeviceSize)HUD_MAXINST * sizeof(HUDINST))
#define MAXPART       160           /* the HUD's kit, at most 128, then the wiki's */

#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties)

#define DFNS(X) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier) X(vkCmdBlitImage) X(vkCmdClearColorImage) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

typedef struct {
    VkImage         img;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkDescriptorSet set;
    int             w, h, levels;
} TEX;

static int s_state;
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;
static VkDescriptorPool      s_dpool;
static VkSampler             s_clamp, s_repeat, s_glyph;
static TEX                   s_part[MAXPART], s_atlas, s_chart, s_sprite;
static VkSampler             s_near;
static unsigned              s_cgen;            /* the chart generation the device holds; 0 none */
static int                   s_cclear;          /* the chart's and the sprite sheet's images
                                                   cleared since the build                       */
static unsigned              s_sgen;            /* the sprite generation the device holds; 0 none */
static int                   s_nparts;          /* the HUD's parts: s_part[0, s_nparts)       */
static int                   s_nwparts;         /* the wiki's, after them; 0 when it failed    */
static int                   s_nsent;           /* the parts uploaded and their chains blitted */
static int                   s_sent;            /* all of them                                 */
static unsigned              s_agen;            /* the atlas generation the device holds; 0 none */
static VkBuffer              s_ibuf;
static VkDeviceMemory        s_imem;
static unsigned char*        s_imap;
static TAGPU_VKSTAGE         s_stage[TAGPU_VK_SLOTS];
static HUDOPS                s_ops;             /* render thread's; 400 KB, never on a stack  */
static HUDBATCH              s_batch[HUD_MAXBATCH];
static int                   s_nbatch, s_ninst;
static int                   s_saidDrop, s_saidMiss, s_saidChart, s_saidSprite, s_saidUnfit;
static int                   s_nbase;           /* the base layer's batches: s_batch[0, s_nbase) */
static int                   s_tb0, s_tb1;      /* the top layer's: s_batch[s_tb0, s_tb1); the
                                                   groups' batches lie outside both          */
static int                   s_wb0, s_wb1;      /* the wiki layer's: s_batch[s_wb0, s_wb1)     */
static int                   s_wbm;             /* where the stage's ops over its 3D image start:
                                                   [s_wb0, s_wbm) under it, [s_wbm, s_wb1) over */
static VkRect2D              s_top;             /* the top layer's device pixels; empty none   */

/* THE GROUPS' LAYER (tagpu_huddraw.c, THE GROUPS): an image a frame slot, cleared and drawn by
   this pass's own render pass in `prepare` and sampled by the attachment's composites in `record`
   and `record_top` -- both layers' groups are in it -- all in the slot's command buffer. The seam's fence wait on the slot proves the slot's last use
   of it done (tagpu_vk_pass.h), so it is replaced, when a frame's regions outgrow it, with nothing
   in flight naming it. Its descriptor set is the slot's for the pass's life and is re-pointed at
   each new image, so a set in hand always names the current one. */
typedef struct {
    VkImage         img;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkFramebuffer   fb;
    VkDescriptorSet set;
    int             w, h;
} LAYER;
static VkRenderPass          s_lrp;
static VkPipeline            s_lpipe;           /* the pipeline again, against s_lrp          */
static LAYER                 s_layer[TAGPU_VK_SLOTS];
static HUDLAYREG              s_group[HUD_MAXGRP];
static int                   s_ngroup, s_layW, s_layH;

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

static int mem_type(const TAGPU_VKPASS* d, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(d->pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
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

/* ---- the images ------------------------------------------------------------------------------ */

static int levels_for(int w, int h)
{
    int n = 1;
    while ((w > 1 || h > 1) && n < 16) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

static int mk_tex(const TAGPU_VKPASS* d, TEX* t, VkFormat fmt, int w, int h, int levels, VkSampler samp)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    VkMemoryRequirements req;
    int type;

    t->w = w; t->h = h; t->levels = levels;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = (uint32_t)w;
    ici.extent.height = (uint32_t)h;
    ici.extent.depth = 1;
    ici.mipLevels = (uint32_t)levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, &t->img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, t->img, &req);
    if ((type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &t->mem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, t->img, t->mem, 0) != VK_SUCCESS) return 0;
    ivi.image = t->img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = (uint32_t)levels;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, &t->view) != VK_SUCCESS) return 0;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s_dsl;
    if (vkAllocateDescriptorSets(d->dev, &dai, &t->set) != VK_SUCCESS) return 0;
    memset(&ii, 0, sizeof ii);
    ii.sampler = samp;
    ii.imageView = t->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&wr, 0, sizeof wr);
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = t->set;
    wr.dstBinding = 40;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    return 1;
}

static void free_tex(VkDevice dev, TEX* t)
{
    if (t->view) vkDestroyImageView(dev, t->view, NULL);
    if (t->img) vkDestroyImage(dev, t->img, NULL);
    if (t->mem) vkFreeMemory(dev, t->mem, NULL);
    memset(t, 0, sizeof *t);
}

static int mk_sampler(const TAGPU_VKPASS* d, VkSampler* s, VkSamplerAddressMode u, int mips)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = u;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = mips ? 16.0f : 0.0f;
    return vkCreateSampler(d->dev, &sci, NULL, s) == VK_SUCCESS;
}

/* The chain below level 0, blitted down level by level, every level SHADER_READ_ONLY after. */
static void blit_chain(VkCommandBuffer cb, const TEX* t)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    int i, w = t->w, h = t->h;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;

    /* level 0 left the upload SHADER_READ_ONLY: it becomes the first source */
    b.subresourceRange.baseMipLevel = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    for (i = 1; i < t->levels; i++) {
        VkImageBlit r;
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        b.subresourceRange.baseMipLevel = (uint32_t)i;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        memset(&r, 0, sizeof r);
        r.srcSubresource.aspectMask = r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.srcSubresource.mipLevel = (uint32_t)(i - 1);
        r.dstSubresource.mipLevel = (uint32_t)i;
        r.srcSubresource.layerCount = r.dstSubresource.layerCount = 1;
        r.srcOffsets[1].x = w; r.srcOffsets[1].y = h; r.srcOffsets[1].z = 1;
        r.dstOffsets[1].x = nw; r.dstOffsets[1].y = nh; r.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_LINEAR);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        w = nw; h = nh;
    }
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = (uint32_t)t->levels;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

/* one run of instances, its texture bound: the slot's layer for a group's composite */
static void draw_batch(VkCommandBuffer cb, const HUDBATCH* b, uint32_t slot)
{
    VkDescriptorSet set = b->tex == HUD_TEX_LAYER ? s_layer[slot].set
                        : b->tex == HUD_TEX_ATLAS ? s_atlas.set : b->tex == HUD_TEX_CHART ? s_chart.set
                        : b->tex == HUD_TEX_SPRITE ? s_sprite.set
                        : b->tex >= 0 && b->tex < s_nparts + s_nwparts ? s_part[b->tex].set : s_atlas.set;
    if (!b->count) return;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1, &set, 0, NULL);
    vkCmdDraw(cb, 4, (uint32_t)b->count, 0, (uint32_t)b->first);
}

/* ---- the groups' layer ------------------------------------------------------------------------ */

/* THE LAYER'S FORMAT: RGBA8 in the attachment's encoding -- sRGB when the attachment's is, so a
   group blends in the layer in the space it would have blended in on the attachment -- and a full
   8-bit alpha whatever the attachment carries, since a group's coverage lives in the layer's alpha
   and one with little or none would lose it. */
static VkFormat s_lfmt;
static VkFormat layer_format(VkFormat attachment)
{
    switch (attachment) {
    case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8_SRGB: case VK_FORMAT_B8G8R8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return VK_FORMAT_R8G8B8A8_SRGB;
    default:
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

/* One colour attachment in the layer's format (layer_format). LOAD_OP_CLEAR over the render area, which holds every region; the
   contents outside it are never sampled. FIRST SCOPE: the slot's earlier frames, whose composites
   sampled the image -- a write after their reads. SECOND: the composites of this frame's render
   pass, which read what this one wrote. */
static int build_layer_pass(const TAGPU_VKPASS* d)
{
    VkAttachmentDescription at;
    VkAttachmentReference ref;
    VkSubpassDescription sp;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    memset(&at, 0, sizeof at);
    at.format = s_lfmt;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    rpi.attachmentCount = 1; rpi.pAttachments = &at;
    rpi.subpassCount = 1; rpi.pSubpasses = &sp;
    rpi.dependencyCount = 2; rpi.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &rpi, NULL, &s_lrp) == VK_SUCCESS;
}

/* the image, its view and its framebuffer given back; the set stays the slot's */
static void layer_free(VkDevice dev, LAYER* L)
{
    if (L->fb)   vkDestroyFramebuffer(dev, L->fb, NULL);
    if (L->view) vkDestroyImageView(dev, L->view, NULL);
    if (L->img)  vkDestroyImage(dev, L->img, NULL);
    if (L->mem)  vkFreeMemory(dev, L->mem, NULL);
    L->fb = VK_NULL_HANDLE; L->view = VK_NULL_HANDLE; L->img = VK_NULL_HANDLE; L->mem = VK_NULL_HANDLE;
    L->w = L->h = 0;
}

/* The slot's layer at least w x h, grown in steps of 64 pixels so a picker that opens and closes
   does not rebuild it, and never past the attachment's `cw` x `ch`: the regions lie within it
   (tagpu_huddraw.c, THE LAYER IS NO LARGER THAN THE ATTACHMENT), and an image the device made at
   that size is one it can make. Safe to replace here: the caller owns the slot (the seam's fence
   wait). */
static int layer_size(const TAGPU_VKPASS* d, LAYER* L, int w, int h, int cw, int ch)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    VkMemoryRequirements req;
    int type;
    if (w > cw || h > ch) return 0;
    if (L->img && L->w >= w && L->h >= h) return 1;
    w = ((w > L->w ? w : L->w) + 63) / 64 * 64;
    h = ((h > L->h ? h : L->h) + 63) / 64 * 64;
    if (w > cw) w = cw;
    if (h > ch) h = ch;
    layer_free(d->dev, L);
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = s_lfmt;
    ici.extent.width = (uint32_t)w;
    ici.extent.height = (uint32_t)h;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, &L->img) != VK_SUCCESS) { L->img = VK_NULL_HANDLE; return 0; }
    vkGetImageMemoryRequirements(d->dev, L->img, &req);
    if ((type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) < 0) { layer_free(d->dev, L); return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &L->mem) != VK_SUCCESS) { L->mem = VK_NULL_HANDLE; layer_free(d->dev, L); return 0; }
    if (vkBindImageMemory(d->dev, L->img, L->mem, 0) != VK_SUCCESS) { layer_free(d->dev, L); return 0; }
    ivi.image = L->img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = s_lfmt;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, &L->view) != VK_SUCCESS) { L->view = VK_NULL_HANDLE; layer_free(d->dev, L); return 0; }
    fci.renderPass = s_lrp;
    fci.attachmentCount = 1;
    fci.pAttachments = &L->view;
    fci.width = (uint32_t)w;
    fci.height = (uint32_t)h;
    fci.layers = 1;
    if (vkCreateFramebuffer(d->dev, &fci, NULL, &L->fb) != VK_SUCCESS) { L->fb = VK_NULL_HANDLE; layer_free(d->dev, L); return 0; }
    if (!L->set) {
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = s_dpool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &s_dsl;
        if (vkAllocateDescriptorSets(d->dev, &dai, &L->set) != VK_SUCCESS) { L->set = VK_NULL_HANDLE; layer_free(d->dev, L); return 0; }
    }
    /* texel for pixel: the composite samples texel centres, so nearest is exact */
    memset(&ii, 0, sizeof ii);
    ii.sampler = s_near;
    ii.imageView = L->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&wr, 0, sizeof wr);
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = L->set;
    wr.dstBinding = 40;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    L->w = w; L->h = h;
    return 1;
}

/* This frame's groups drawn into the slot's layer, before the render pass: each region cleared and
   its instances drawn with the attachment's viewport, scissored to it; then the composites' uv,
   in layer pixels, divided by the image's size. 0 when the layer could not be made. */
static int layer_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot, uint32_t w, uint32_t h)
{
    LAYER* L = &s_layer[slot];
    HUDINST* in = (HUDINST*)(s_imap + (size_t)slot * IBYTES);
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue clear;
    VkViewport vp;
    VkDeviceSize off = (VkDeviceSize)slot * IBYTES;
    int i, j, k;
    if (!layer_size(d, L, s_layW > 0 ? s_layW : 1, s_layH > 0 ? s_layH : 1, (int)w, (int)h)) return 0;
    /* the composites stand in the three layers' own batches, never in the groups' */
    for (i = 0; i < s_nbatch; i++) {
        const HUDBATCH* b = &s_batch[i];
        if (b->tex != HUD_TEX_LAYER || !(i < s_nbase || (i >= s_tb0 && i < s_tb1) || (i >= s_wb0 && i < s_wb1)))
            continue;
        for (j = b->first; j < b->first + b->count; j++) {
            in[j].uv[0] /= (float)L->w; in[j].uv[2] /= (float)L->w;
            in[j].uv[1] /= (float)L->h; in[j].uv[3] /= (float)L->h;
        }
    }
    memset(&clear, 0, sizeof clear);
    rbi.renderPass = s_lrp;
    rbi.framebuffer = L->fb;
    rbi.renderArea.extent.width = (uint32_t)(s_layW > 0 ? s_layW : 1);
    rbi.renderArea.extent.height = (uint32_t)(s_layH > 0 ? s_layH : 1);
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)w; vp.height = (float)h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_lpipe);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_ibuf, &off);
    for (k = 0; k < s_ngroup; k++) {
        const HUDLAYREG* g = &s_group[k];
        VkRect2D sc;
        if (!g->nb) continue;
        sc.offset.x = g->x; sc.offset.y = g->y;
        sc.extent.width = (uint32_t)g->w; sc.extent.height = (uint32_t)g->h;
        vkCmdSetScissor(cb, 0, 1, &sc);
        for (i = g->b0; i < g->b0 + g->nb; i++) draw_batch(cb, &s_batch[i], slot);
    }
    vkCmdEndRenderPass(cb);
    return 1;
}

/* ---- the build ------------------------------------------------------------------------------- */

static int build_pipeline(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b;
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[6];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule vs, fs;
    VkResult r;
    int i, ok = 0;

    memset(&b, 0, sizeof b);
    b.binding = 40;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dli.bindingCount = 1;
    dli.pBindings = &b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS) return 0;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) return 0;

    vs = mk_module(d, tagpu_spv_tagpu_huddraw_VS, sizeof tagpu_spv_tagpu_huddraw_VS / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_tagpu_huddraw_FS, sizeof tagpu_spv_tagpu_huddraw_FS / sizeof(uint32_t));
    if (!vs || !fs) { plog(d, "hud: a shader module was refused"); goto out; }
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    /* the six vec4s of HUDINST, per instance */
    memset(&vb, 0, sizeof vb);
    vb.binding = 0;
    vb.stride = sizeof(HUDINST);
    vb.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    memset(va, 0, sizeof va);
    for (i = 0; i < 6; i++) {
        va[i].location = (uint32_t)i;
        va[i].binding = 0;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[i].offset = (uint32_t)(i * 4 * sizeof(float));
    }
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 6; vi.pVertexAttributeDescriptions = va;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* PREMULTIPLIED: every colour the fragment stage writes is already weighted by its alpha */
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
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    /* the seam's render pass has a depth attachment, so a depth state is required: none of it */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDepthStencilState = &ds;
    gp.pDynamicState = &dy;
    gp.layout = s_plo;
    gp.renderPass = d->rp;
    gp.subpass = 0;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipe);
    if (r != VK_SUCCESS) { plog(d, "hud: vkCreateGraphicsPipelines refused it (%d)", (int)r); goto out; }
    /* the same state against the layer's pass, which has no depth attachment: the depth state is
       then ignored */
    if (!build_layer_pass(d)) { plog(d, "hud: the layer's render pass was refused"); goto out; }
    gp.renderPass = s_lrp;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_lpipe);
    if (r != VK_SUCCESS) { plog(d, "hud: vkCreateGraphicsPipelines refused the layer's (%d)", (int)r); goto out; }
    ok = 1;
out:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

static int format_ok(const TAGPU_VKPASS* d, VkFormat f, VkFormatFeatureFlags need)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(d->pd, f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

static int build(const TAGPU_VKPASS* d, const HUDART* art, const WIKIART* wart)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkDescriptorPoolSize ps;
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkMemoryRequirements req;
    void* p = NULL;
    int i, type;

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "hud: %u frame slots is outside what this pass carries (%d)", (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    if (!resolve(d)) { plog(d, "hud: an entry point is missing"); return 0; }
    /* SAMPLED, LINEARLY FILTERED AND BLITTED, ASKED FOR RATHER THAN ASSUMED: the parts are
       minified through a mip chain this pass blits, and the glyphs are filtered. All of it is
       mandatory for these two formats, so this has never fired; the day it does the log names
       it instead of the pass drawing nothing. */
    s_lfmt = layer_format(d->fmt);
    if (!format_ok(d, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                                              | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)
        || !format_ok(d, VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
        || !format_ok(d, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        || !format_ok(d, d->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
        || !format_ok(d, s_lfmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        plog(d, "hud: this device cannot filter or blit RGBA8 / R8, sample RGBA32F, blend into its "
                "attachment's format, or blend into and sample the groups' layer's %s - the pass stays down",
             s_lfmt == VK_FORMAT_R8G8B8A8_SRGB ? "RGBA8 sRGB" : "RGBA8");
        return 0;
    }
    s_nparts = tagpu_hudlay_nparts(art->lay);
    if (s_nparts > MAXPART) { plog(d, "hud: %d parts, more than the pass holds", s_nparts); return 0; }
    /* the wiki's kit after the HUD's; one that does not fit leaves the wiki undrawn, not the HUD */
    s_nwparts = wart ? tagpu_hudlay_nparts(wart->draw.lay) : 0;
    if (s_nparts + s_nwparts > MAXPART) {
        plog(d, "wiki: %d kit parts past the HUD's %d, more than the pass holds - the wiki is not drawn",
             s_nwparts, s_nparts);
        s_nwparts = 0;
    }

    if (!build_pipeline(d)) return 0;
    if (!mk_sampler(d, &s_clamp, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1)
        || !mk_sampler(d, &s_repeat, VK_SAMPLER_ADDRESS_MODE_REPEAT, 1)
        || !mk_sampler(d, &s_glyph, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0)) return 0;
    /* the chart's image is data, read with texelFetch; RGBA32F need not filter, so its sampler
       does not ask it to */
    {
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(d->dev, &sci, NULL, &s_near) != VK_SUCCESS) return 0;
    }

    memset(&ps, 0, sizeof ps);
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = (uint32_t)(s_nparts + s_nwparts) + 3 + TAGPU_VK_SLOTS;
    dpi.maxSets = (uint32_t)(s_nparts + s_nwparts) + 3 + TAGPU_VK_SLOTS;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) return 0;

    for (i = 0; i < s_nparts + s_nwparts; i++) {
        const HUDART* a = i < s_nparts ? art : &wart->draw;
        int j = i < s_nparts ? i : i - s_nparts;
        const HUDIMG* im = &a->img[j];
        const HUDPART* k = tagpu_hudlay_part(a->lay, j);
        if (!mk_tex(d, &s_part[i], VK_FORMAT_R8G8B8A8_UNORM, im->w, im->h, levels_for(im->w, im->h),
                    k->kind == HK_TILE ? s_repeat : s_clamp)) {
            plog(d, "hud: the image of part %s was refused", k->name);
            return 0;
        }
    }
    if (!mk_tex(d, &s_atlas, VK_FORMAT_R8_UNORM, HUD_ATLAS, HUD_ATLAS, 1, s_glyph)) return 0;
    if (!mk_tex(d, &s_chart, VK_FORMAT_R32G32B32A32_SFLOAT, HUD_CHART_W, HUD_CHART_H, 1, s_near)) return 0;
    /* the wiki's sprite sheet, sampled nearest as the sharp layer draws the player's cursor */
    if (!mk_tex(d, &s_sprite, VK_FORMAT_R8G8B8A8_UNORM, HUD_SPRITE_CELL * HUD_SPRITE_CELLS, HUD_SPRITE_CELL, 1, s_near))
        return 0;

    bci.size = IBYTES * d->slots;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, &s_ibuf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(d->dev, s_ibuf, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) return 0;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s_imem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(d->dev, s_ibuf, s_imem, 0) != VK_SUCCESS) return 0;
    if (vkMapMemory(d->dev, s_imem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return 0;
    s_imap = (unsigned char*)p;

    s_nsent = s_sent = 0;
    s_agen = 0;
    s_cgen = 0;
    s_sgen = 0;
    s_cclear = 0;
    plog(d, "hud: the pass is up - %d parts and the wiki's %d, a %dx%d glyph atlas, %u frame slots",
         s_nparts, s_nwparts, HUD_ATLAS, HUD_ATLAS, (unsigned)d->slots);
    return 1;
}

/* ---- the uploads ----------------------------------------------------------------------------- */

/* The parts, whole, each followed by its chain, from where the last frame stopped: 23 MB of
   them against a slot's 2 MiB, so they take a dozen frames, and the HUD draws once the last is
   there. A part that would not fit what this frame's buffer has left waits for the next frame,
   where it is the first copy: in the frame's own command buffer when it fits the slot's whole
   buffer, banded when it does not -- the parts larger than the cap, uploaded once after the pass is
   built, which is the banded path's own event (tagpu_vk_stage.h). 1 all sent, 0 more next
   frame, -1 the device. */
static int send_parts(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKSTAGE* st, const HUDART* art,
                      const WIKIART* wart)
{
    for (; s_nsent < s_nparts + s_nwparts; s_nsent++) {
        const HUDIMG* im = s_nsent < s_nparts ? &art->img[s_nsent] : &wart->draw.img[s_nsent - s_nparts];
        int r;
        if (st->used && !tagpu_vk_stage_fits(st, (VkDeviceSize)im->w * im->h * 4)) return 0;
        r = tagpu_vk_stage_copy(d, cb, st, s_part[s_nsent].img, 0, im->rgba, im->w * 4, im->w, im->h, 4);
        if (r <= 0) return r;
        blit_chain(cb, &s_part[s_nsent]);
    }
    return 1;
}

/* The atlas rows this frame added, in this frame's command buffer when they fit what the slot
   mapped; a new generation, whole. 1 the device holds the atlas, 0 not this frame, -1 the device. */
static int send_atlas(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKSTAGE* st)
{
    unsigned gen;
    int y0, y1, n, r;
    const unsigned char* a = tagpu_huddraw_atlas(&gen, &y0, &y1);
    VkDeviceSize off, bytes;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy rg;

    /* THE FIRST UPLOAD AFTER A BUILD goes whole, the frame's first copy as a part's is: the image
       has no contents and no layout yet, and 4 MiB is banded -- the post-build event the banded
       path is for (tagpu_vk_stage.h) */
    if (!s_agen) {
        if (st->used) return 0;
        r = tagpu_vk_stage_copy(d, cb, st, s_atlas.img, 0, a, HUD_ATLAS, HUD_ATLAS, HUD_ATLAS, 1);
        if (r > 0) { s_agen = gen; tagpu_huddraw_atlas_sent(HUD_ATLAS); }
        return r;
    }
    /* AFTER IT, ONLY THE CHANGED ROWS, a new generation's included (tagpu_huddraw.h), in this
       frame's command buffer and as many as its buffer has room for; the rest go next frame, and
       the text waits for them (hide_mode). Nothing here waits on the device. */
    if (y1 <= y0) { s_agen = gen; return 1; }
    off = (st->used + 3) & ~(VkDeviceSize)3;
    if (!st->map || off >= st->cap) return 0;
    n = (int)((st->cap - off) / HUD_ATLAS);
    if (n <= 0) return 0;
    if (n < y1 - y0) y1 = y0 + n;
    bytes = (VkDeviceSize)(y1 - y0) * HUD_ATLAS;
    memcpy(st->map + off, a + (size_t)y0 * HUD_ATLAS, (size_t)bytes);
    st->used = off + bytes;

    /* write after the reads of every frame already submitted: the barrier's first scope */
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s_atlas.img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = off;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageOffset.y = y0;
    rg.imageExtent.width = HUD_ATLAS;
    rg.imageExtent.height = (uint32_t)(y1 - y0);
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, st->buf, s_atlas.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    s_agen = gen;
    tagpu_huddraw_atlas_sent(y1);
    return 1;
}

/* An image whole, `bytes` of `a`, in this frame's command buffer when it fits what the slot mapped.
   Every texel is written, so the image's old contents go: the barrier leaves UNDEFINED, and its
   first scope still holds the reads of every frame already submitted. 1 sent, 0 not this frame;
   `*said` is set when a buffer with nothing else in it cannot hold the image. */
static int send_whole(VkCommandBuffer cb, TAGPU_VKSTAGE* st, const TEX* t, const void* a, VkDeviceSize bytes, int* said)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy rg;
    VkDeviceSize off = (st->used + 15) & ~(VkDeviceSize)15;

    if (!st->map || off + bytes > st->cap) {
        if (st->map && !st->used) *said = 1;
        return 0;
    }
    memcpy(st->map + off, a, (size_t)bytes);
    st->used = off + bytes;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = off;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = (uint32_t)t->w;
    rg.imageExtent.height = (uint32_t)t->h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, st->buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    return 1;
}

/* The chart's image, whole, once its generation has moved. 1 the device holds the generation, 0
   not this frame. */
static int send_chart(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKSTAGE* st)
{
    unsigned gen;
    const float* a = tagpu_huddraw_chart(&gen);
    const VkDeviceSize bytes = (VkDeviceSize)HUD_CHART_W * HUD_CHART_H * 4 * sizeof(float);
    int said = 0;

    if (gen == s_cgen) return 1;
    if (!send_whole(cb, st, &s_chart, a, bytes, &said)) {
        /* a buffer with nothing else in it that cannot hold the image: the lines wait for one */
        if (said && !s_saidChart) {
            s_saidChart = 1;
            plog(d, "hud: the chart's image (%u bytes) does not fit the slot's staging (%u mapped): "
                 "its lines wait for room (said once)", (unsigned)bytes, (unsigned)st->cap);
        }
        return 0;
    }
    s_cgen = gen;
    return 1;
}

/* The wiki's sprite sheet, whole, once its generation has moved; 1 the device holds it. Until it
   does the sprites are not drawn (hide_sprites). */
static int send_sprite(const TAGPU_VKPASS* d, VkCommandBuffer cb, TAGPU_VKSTAGE* st)
{
    unsigned gen;
    const unsigned char* a = tagpu_wikistate_sprites(&gen);
    const VkDeviceSize bytes = (VkDeviceSize)HUD_SPRITE_CELL * HUD_SPRITE_CELLS * HUD_SPRITE_CELL * 4;
    int said = 0;

    if (!gen || gen == s_sgen) return 1;
    if (!send_whole(cb, st, &s_sprite, a, bytes, &said)) {
        if (said && !s_saidSprite) {
            s_saidSprite = 1;
            plog(d, "wiki: the sprite sheet (%u bytes) does not fit the slot's staging (%u mapped): "
                 "the stage's cursor waits for room (said once)", (unsigned)bytes, (unsigned)st->cap);
        }
        return 0;
    }
    s_sgen = gen;
    return 1;
}

/* An image cleared, once after the build: a sampled layout, and for the chart a header that draws
   no line (row 0's count 0), for the sprite sheet transparent texels, so the HUD never waits on an
   image's first upload, which needs room in a frame's staging -- the lines and the sprites wait for
   it (hide_mode, hide_sprites), the rest of the HUD does not. */
static void clear_image(VkCommandBuffer cb, VkImage img)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkClearColorValue zero;
    VkImageSubresourceRange all;

    memset(&zero, 0, sizeof zero);
    memset(&all, 0, sizeof all);
    all.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    all.levelCount = 1;
    all.layerCount = 1;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = all;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- the frame ------------------------------------------------------------------------------- */

/* The instances of one mode made transparent: a frame's glyphs whose atlas the device does not
   hold yet -- changed rows still waiting for room in a frame's buffer (send_atlas), or a frame
   with no staging -- which would otherwise be sampled where the instances say, empty texels or an
   older generation's glyphs; and a frame's chart bands whose image is an older generation's,
   whose points are not this frame's. Fills and parts in the same runs still draw. */
static void hide_mode(HUDINST* in, int n, int mode)
{
    int i;
    for (i = 0; i < n; i++)
        if (in[i].prm[0] == (float)mode) in[i].rgba[0] = in[i].rgba[1] = in[i].rgba[2] = in[i].rgba[3] = 0;
}

/* The sprites of the wiki's stage made transparent while the device does not hold the sheet's
   current generation, whose cells would be another frame's: the part instances of the
   HUD_TEX_SPRITE runs, and not the fills that joined them. */
static void hide_sprites(HUDINST* in, const HUDBATCH* b, int nb)
{
    int i, j;
    for (i = 0; i < nb; i++)
        if (b[i].tex == HUD_TEX_SPRITE)
            for (j = b[i].first; j < b[i].first + b[i].count; j++)
                if (in[j].prm[0] == (float)HUDM_PART) in[j].rgba[0] = in[j].rgba[1] = in[j].rgba[2] = in[j].rgba[3] = 0;
}

/* The device-pixel union of the quads the top layer's `n` instances emit, held to the `w` x `h`
   attachment. The quad is `ndc`, what the vertex shader places; `px` is not always a box (a line's
   two ends, a chart band's segments), so it does not bound what is drawn. */
static void top_rect(const HUDINST* in, int n, uint32_t w, uint32_t h)
{
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    int i;
    for (i = 0; i < n; i++) {
        float a = (in[i].ndc[0] + 1) * 0.5f * (float)w, b = (in[i].ndc[2] + 1) * 0.5f * (float)w;
        float c = (in[i].ndc[1] + 1) * 0.5f * (float)h, e = (in[i].ndc[3] + 1) * 0.5f * (float)h;
        if (a > b) { float t = a; a = b; b = t; }
        if (c > e) { float t = c; c = e; e = t; }
        /* a quad that covers nothing -- a group's composite whose region came out empty stays at
           the corner it was emitted at -- draws nothing to bound */
        if (!(b > a) || !(e > c)) continue;
        if (a < x0) x0 = a;
        if (c < y0) y0 = c;
        if (b > x1) x1 = b;
        if (e > y1) y1 = e;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (float)w) x1 = (float)w;
    if (y1 > (float)h) y1 = (float)h;
    memset(&s_top, 0, sizeof s_top);
    if (n <= 0 || x1 <= x0 || y1 <= y0) return;
    s_top.offset.x = (int32_t)floorf(x0);
    s_top.offset.y = (int32_t)floorf(y0);
    s_top.extent.width = (uint32_t)ceilf(x1) - (uint32_t)s_top.offset.x;
    s_top.extent.height = (uint32_t)ceilf(y1) - (uint32_t)s_top.offset.y;
}

int tagpu_vk_hud_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h)
{
    const HUDART* art;
    const WIKIART* wart;
    const HUDSTATE* st = NULL;
    HUDOPLIST wpage, wstage, wover;
    TAGPU_VKSTAGE* sg;
    char err[160];
    float px = 0, wpx = 0;
    int dropped = 0, unfit = 0, r, due, ox = 0, oy = 0, wox = 0, woy = 0, hud, wiki;

    s_ninst = s_nbatch = s_nbase = s_tb0 = s_tb1 = s_wb0 = s_wbm = s_wb1 = s_ngroup = 0;
    memset(&s_top, 0, sizeof s_top);
    tagpu_hudstate_drawn(NULL);
    tagpu_wikistate_drawn(0);
    if (s_state == ST_REFUSED) return 0;
    hud = tagpu_hudstate_frame(d->frame, &st, &px, &ox, &oy) && px > 0;
    wiki = tagpu_wikistate_frame(d->frame, &wpage, &wstage, &wover, &wpx, &wox, &woy) && wpx > 0;
    if (!hud && !wiki) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS || w == 0 || h == 0) return 0;

    /* NOTHING IS BUILT UNTIL THE HUD HAS LOADED: tagpu_hudart decodes the parts on a thread of
       its own, and the HUD appears on the first frame after it has published them. Every way the
       HUD cannot draw for the session is told to tagpu_hudstate_refuse. */
    if (!(art = tagpu_hudart_get())) {
        if (tagpu_hudart_failed()) tagpu_hudstate_refuse("its art did not load");
        else tagpu_hudart_start();
        return 0;
    }
    /* THE PASS IS BUILT ONCE THE WIKI'S LOAD HAS ENDED TOO, loaded or failed: it follows the HUD's
       on the same thread, and its parts are textures of the same pass */
    if (!tagpu_wikiart_done()) return 0;
    wart = tagpu_wikiart_get();

    if (s_state == ST_UNBUILT) {
        if (!build(d, art, wart)) {
            tagpu_vk_hud_down(d);
            s_state = ST_REFUSED;
            tagpu_hudstate_refuse("its pass could not be built");
            return 0;
        }
        s_state = ST_READY;
    }

    if (!s_nwparts) wiki = 0;
    if (hud && !tagpu_hudlay_build(art->lay, st, &s_ops, err, sizeof err)) {
        if (!s_saidMiss) { s_saidMiss = 1; plog(d, "hud: %s", err); }
        tagpu_hudstate_refuse("its layout did not build");
        return 0;
    }
    if (!hud && !wiki) return 0;
    /* THREE LAYERS IN ONE RANGE: the HUD's base layer's instances and batches first, its top
       layer's after them, the wiki's last, each batch's `first` an index into the slot's whole
       range and each group's b0 an index into s_batch. One begin before every build keeps them on
       one glyph atlas, and one shelf carried through all of them packs every layer's groups on one
       layer image (tagpu_huddraw.h). */
    {
        HUDINST* in = (HUDINST*)(s_imap + (size_t)slot * IBYTES);
        HUDSHELF sh;
        int n0 = 0, n1 = 0, nb0 = 0, nb1 = 0, m1 = 0, mi0 = 0, mi1 = 0, g0, d1 = 0, u1 = 0, i;
        memset(&sh, 0, sizeof sh);
        tagpu_huddraw_begin();
        if (hud) {
            n0 = tagpu_huddraw_build(art, &s_ops, 0, px, ox, oy, (int)w, (int)h, in, HUD_MAXINST,
                                     s_batch, HUD_MAXBATCH, &nb0, &s_nbase, &mi0, s_group, &s_ngroup,
                                     &sh, &dropped, &unfit);
            g0 = s_ngroup;
            n1 = tagpu_huddraw_build(art, &s_ops, 1, px, ox, oy, (int)w, (int)h, in + n0,
                                     HUD_MAXINST - n0, s_batch + nb0, HUD_MAXBATCH - nb0,
                                     &nb1, &m1, &mi1, s_group, &s_ngroup, &sh, &d1, &u1);
            dropped += d1;
            unfit += u1;
            for (i = nb0; i < nb0 + nb1; i++) s_batch[i].first += n0;
            for (i = g0; i < s_ngroup; i++) s_group[i].b0 += nb0;
            /* the top layer's own quads: its groups' are drawn into the layer image, not here */
            top_rect(in + n0, mi1, w, h);
        }
        s_ninst = n0 + n1;
        s_nbatch = nb0 + nb1;
        s_tb0 = nb0;
        s_tb1 = nb0 + m1;
        s_wb0 = s_wbm = s_wb1 = s_nbatch;
        /* THE WIKI'S LAYER, in its own unit and from its own kit: a batch's texture is its kit
           index, which the pass holds after the HUD's parts. The page, then the stage inside its
           bezel -- its ops under the 3D image, then those over it; the stage has no groups, so
           every batch it adds is the attachment's and the layer's batches stay one run
           [s_wb0, s_wb1) when the page's groups' come after it, cut at s_wbm where the image goes */
        if (wiki) {
            const HUDOPLIST* list[3] = { &wpage, &wstage, &wover };
            int k, mw[3] = { 0, 0, 0 }, nbw[3] = { 0, 0, 0 }, nb_at[3] = { 0, 0, 0 };
            s_wb0 = s_nbatch;
            for (k = 0; k < 3; k++) {
                int nw, miw = 0, gw = s_ngroup, dw = 0, uw = 0;
                if (!list[k]->n) continue;
                nb_at[k] = s_nbatch;
                nw = tagpu_huddraw_build_list(&wart->draw, list[k], 0, wpx, wox, woy, (int)w, (int)h, in + s_ninst,
                                              HUD_MAXINST - s_ninst, s_batch + s_nbatch, HUD_MAXBATCH - s_nbatch,
                                              &nbw[k], &mw[k], &miw, s_group, &s_ngroup, &sh, &dw, &uw);
                dropped += dw;
                unfit += uw;
                for (i = s_nbatch; i < s_nbatch + nbw[k]; i++) {
                    s_batch[i].first += s_ninst;
                    if (s_batch[i].tex >= 0) s_batch[i].tex += s_nparts;
                }
                for (i = gw; i < s_ngroup; i++) s_group[i].b0 += s_nbatch;
                s_nbatch += nbw[k];
                s_ninst += nw;
            }
            /* the page's attachment batches, its groups' batches, then the stage's two lists': the
               groups' are moved past the stage's so the attachment's batches are one run. The
               stage's lists follow each other, so theirs are one block of nbw[1] + nbw[2] */
            if (nbw[0] > mw[0] && nbw[1] + nbw[2] > 0) {
                const int g0 = nb_at[0] + mw[0], ng = nbw[0] - mw[0], ns = nbw[1] + nbw[2];
                HUDBATCH tmp[HUD_MAXBATCH];
                memcpy(tmp, s_batch + g0, (size_t)ng * sizeof *tmp);
                memmove(s_batch + g0, s_batch + g0 + ng, (size_t)ns * sizeof *tmp);
                memcpy(s_batch + g0 + ns, tmp, (size_t)ng * sizeof *tmp);
                for (i = 0; i < s_ngroup; i++)
                    if (s_group[i].b0 >= g0 && s_group[i].b0 < g0 + ng) s_group[i].b0 += ns;
            }
            s_wbm = s_wb0 + mw[0] + (wstage.n ? nbw[1] : 0);
            s_wb1 = s_wbm + (wover.n ? nbw[2] : 0);
        }
        s_layW = sh.w_used;
        s_layH = sh.h_used;
    }
    if (unfit && !s_saidUnfit) {
        s_saidUnfit = 1;
        plog(d, "hud: %d disabled controls' regions did not fit the groups' layer (%ux%u) this frame; "
                "they are drawn op by op at their opacity (said once)", unfit, (unsigned)w, (unsigned)h);
    }
    if ((dropped || s_ops.overflow) && !s_saidDrop) {
        s_saidDrop = 1;
        plog(d, "hud: %d glyphs did not fit the atlas this frame%s (said once)", dropped,
             s_ops.overflow ? ", and the ops overflowed" : "");
    }

    /* THE UPLOADS, through this slot's staging, which the seam's fence wait has freed; a slot
       with nothing to send gives its buffer back (tagpu_vk_stage.h, THE SHAPE). */
    sg = &s_stage[slot];
    if (!s_cclear) {
        clear_image(cb, s_chart.img);
        clear_image(cb, s_sprite.img);
        s_cclear = 1;
    }
    {
        unsigned gen, cgen, sgen;
        int y0, y1;
        tagpu_huddraw_atlas(&gen, &y0, &y1);
        tagpu_huddraw_chart(&cgen);
        tagpu_wikistate_sprites(&sgen);
        due = !s_sent || gen != s_agen || y1 > y0 || cgen != s_cgen || (sgen && sgen != s_sgen);
    }
    if (!due) {
        if (sg->buf) tagpu_vk_stage_drop(d, sg);
    } else if (tagpu_vk_stage_begin(d, sg, TAGPU_VK_STAGE_CAP, (VkDeviceSize)4096 * 4)) {
        if (!s_sent) {
            if ((r = send_parts(d, cb, sg, art, wart)) < 0) goto lost;
            if (r > 0) {
                s_sent = 1;
                plog(d, "hud: the parts are on the device");
            }
        }
        if (s_sent && (r = send_atlas(d, cb, sg)) < 0) goto lost;
        if (s_sent && s_agen) send_chart(d, cb, sg);
        if (s_sent) send_sprite(d, cb, sg);
    }
    if (!s_sent || !s_agen) { s_ninst = 0; return 0; }
    /* THE TEXT DRAWS ONLY FROM WHAT THE DEVICE HOLDS: this build's glyphs are in the atlas the
       device has once its generation is s_agen and no row is left dirty */
    {
        unsigned gen;
        int y0, y1;
        tagpu_huddraw_atlas(&gen, &y0, &y1);
        if (gen != s_agen || y1 > y0) hide_mode((HUDINST*)(s_imap + (size_t)slot * IBYTES), s_ninst, HUDM_GLYPH);
        /* and the chart's lines only from the generation this build wrote */
        tagpu_huddraw_chart(&gen);
        if (gen != s_cgen) hide_mode((HUDINST*)(s_imap + (size_t)slot * IBYTES), s_ninst, HUDM_CHART);
        /* and the stage's cursor only from the sheet this frame's ops were built against */
        tagpu_wikistate_sprites(&gen);
        if (gen != s_sgen) hide_sprites((HUDINST*)(s_imap + (size_t)slot * IBYTES), s_batch, s_nbatch);
    }
    /* a frame with a group has its composites, which bind the layer: it is made whenever there is
       one, even if every region is empty */
    if (s_ngroup > 0 && !layer_record(d, cb, slot, w, h)) {
        plog(d, "hud: the groups' layer (%dx%d) could not be made - the pass stops drawing", s_layW, s_layH);
        s_state = ST_REFUSED;
        tagpu_hudstate_refuse("its groups' layer could not be made");
        s_ninst = 0;
        return 0;
    }
    /* what this frame draws is what a click is tested against */
    if (s_ninst > 0) {
        if (hud) tagpu_hudstate_drawn(&s_ops);
        if (wiki) tagpu_wikistate_drawn(1);
    }
    return s_ninst > 0;
lost:
    /* THE DEVICE FAILED A SUBMIT OR A FENCE partway (tagpu_vk_stage.h): an image is half written
       and the banded path's resources may still be in flight, so the pass stops drawing and asks
       for nothing more. Nothing is destroyed here, mid-frame, where other slots' submits still
       name these objects: tagpu_vk_hud_down gives them back behind the seam's vkDeviceWaitIdle
       on the seam's next teardown (vk_down, passes_down). The HUD stays off for the session
       (tagpu_hudstate_refuse). */
    plog(d, "hud: the device failed an upload - the pass stops drawing");
    s_state = ST_REFUSED;
    tagpu_hudstate_refuse("the device failed an upload");
    s_ninst = 0;
    return 0;
}

/* batches [b0, b1) of this frame's, with the pass's pipeline, viewport, scissor and buffer;
   0 when there is nothing to draw */
static int draw_batches(VkCommandBuffer cb, uint32_t slot, uint32_t w, uint32_t h, int b0, int b1)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = (VkDeviceSize)slot * IBYTES;
    int i, drew = 0;
    if (s_state != ST_READY || !s_ninst || b1 <= b0) return 0;
    /* NO FLIP: the instances are in clip space with y down already (tagpu_huddraw.c) */
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)w; vp.height = (float)h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = w; sc.extent.height = h;
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_ibuf, &off);
    for (i = b0; i < b1; i++) {
        if (!s_batch[i].count) continue;
        draw_batch(cb, &s_batch[i], slot);
        drew = 1;
    }
    return drew;
}

void tagpu_vk_hud_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h)
{
    (void)d;
    draw_batches(cb, slot, w, h, 0, s_nbase);
}

int tagpu_vk_hud_record_top(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                            uint32_t w, uint32_t h)
{
    (void)d;
    return draw_batches(cb, slot, w, h, s_tb0, s_tb1);
}

int tagpu_vk_hud_record_wiki(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                             uint32_t w, uint32_t h)
{
    (void)d;
    return draw_batches(cb, slot, w, h, s_wb0, s_wbm);
}

int tagpu_vk_hud_record_wiki_over(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                                  uint32_t w, uint32_t h)
{
    int drew;
    (void)d;
    drew = draw_batches(cb, slot, w, h, s_wbm, s_wb1);
    s_ninst = 0;
    return drew;
}

int tagpu_vk_hud_top_rect(VkRect2D* r)
{
    *r = s_top;
    return s_top.extent.width > 0 && s_top.extent.height > 0;
}

void tagpu_vk_hud_down(const TAGPU_VKPASS* d)
{
    VkDevice dev = d->dev;
    int i;
    if (!dev || !vkDestroyBuffer) { s_state = ST_UNBUILT; return; }
    for (i = 0; i < TAGPU_VK_SLOTS; i++)
        if (s_stage[i].buf) tagpu_vk_stage_drop(d, &s_stage[i]);
    if (s_pipe)  { vkDestroyPipeline(dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    if (s_lpipe) { vkDestroyPipeline(dev, s_lpipe, NULL); s_lpipe = VK_NULL_HANDLE; }
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        layer_free(dev, &s_layer[i]);
        s_layer[i].set = VK_NULL_HANDLE;    /* it goes back with the pool */
    }
    if (s_lrp)   { vkDestroyRenderPass(dev, s_lrp, NULL); s_lrp = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    for (i = 0; i < MAXPART; i++) free_tex(dev, &s_part[i]);
    free_tex(dev, &s_atlas);
    free_tex(dev, &s_chart);
    free_tex(dev, &s_sprite);
    /* the sets go back with the pool */
    if (s_dpool) { vkDestroyDescriptorPool(dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_clamp)  { vkDestroySampler(dev, s_clamp, NULL); s_clamp = VK_NULL_HANDLE; }
    if (s_repeat) { vkDestroySampler(dev, s_repeat, NULL); s_repeat = VK_NULL_HANDLE; }
    if (s_glyph)  { vkDestroySampler(dev, s_glyph, NULL); s_glyph = VK_NULL_HANDLE; }
    if (s_near)   { vkDestroySampler(dev, s_near, NULL); s_near = VK_NULL_HANDLE; }
    if (s_imap)  { vkUnmapMemory(dev, s_imem); s_imap = NULL; }
    if (s_ibuf)  { vkDestroyBuffer(dev, s_ibuf, NULL); s_ibuf = VK_NULL_HANDLE; }
    if (s_imem)  { vkFreeMemory(dev, s_imem, NULL); s_imem = VK_NULL_HANDLE; }
    s_nsent = s_sent = 0;
    s_agen = 0;
    s_cgen = 0;
    s_sgen = 0;
    s_cclear = 0;
    s_ninst = s_nbatch = s_nbase = s_tb0 = s_tb1 = s_wb0 = s_wbm = s_wb1 = s_ngroup = 0;
    s_nwparts = 0;
    memset(&s_top, 0, sizeof s_top);
    /* UNBUILT, not REFUSED: a teardown or a resize must let the pass come back */
    s_state = ST_UNBUILT;
}
