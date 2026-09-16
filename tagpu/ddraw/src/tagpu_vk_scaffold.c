/* tagpu_vk_scaffold.c -- the scene-depth scaffold overlay, drawn by Vulkan.
   Contract: tagpu_vk_scaffold.h. Phase G / G19e, the FIRST WORLD PASS: the
   smallest of them, and the one that forces the question tagpu_vk_fps.c was
   allowed to leave open.

   IT IS NOT A SECOND IMPLEMENTATION OF THE PASS. The scaffold bytes come from
   tagpu_scaffold.c through `tagpu_scaffold_overlay` -- the very buffer the GL
   lane just uploaded to its own texture, handed over once -- the quad is the
   one literal both lanes are built from (TAGPU_SCAF_QUAD), the NDC rect and the
   row count are the numbers the GL draw used, and the shader is the same GLSL,
   translated by tools/spirv-gen.py into inc/spirv/tagpu_scaffold.spv.h. What a
   0-px comparison then compares is two RASTERISERS.

   ---- THE PER-FRAME UPLOAD, WHICH IS THIS LANDING'S REAL WORK ----

   tagpu_vk_fps.c uploads its atlas about twenty times in a session and pays for
   the ordering with `vkDeviceWaitIdle`: correct, and cheap at that rate. THIS
   pass uploads a viewport-sized image EVERY FRAME, and a device-wide stall per
   frame on the render thread -- the thread the game thread waits INFINITE on
   across a mode change -- is not a cost, it is a defect. §2.26 named the
   by-design alternative and left it to this gate. This is it, and it turned out
   to need no new mechanism at all:

     ONE IMAGE AND ONE STAGING BUFFER PER FRAME SLOT, and the seam's fence is
     what makes writing them safe.

   The invariant is the one tagpu_vk_pass.h already publishes for buffers, used
   for an image: when `prepare` is called with `slot`, the seam has just waited
   on `fence[slot]`, so the submit that last used slot `slot`'s resources has
   COMPLETED. Nothing of ours is in flight for that slot. So the CPU may write
   slot `slot`'s staging buffer, the copy into slot `slot`'s image has no prior
   access to be ordered against, and the descriptor set naming that image may be
   rewritten -- all three, for the same reason, with no device-wide wait and no
   barrier against anything outside this command buffer. `oldLayout` is
   UNDEFINED because the whole image is re-sent every frame, so there are no
   contents to preserve and no layout to carry between frames.

   WHAT IT COSTS, stated rather than implied: nimg images plus nimg staging
   buffers of the VIEWPORT's size -- which is not the screen's, and measuring it
   rather than assuming it halves the figure. On the reference setup's four-image
   swapchain: a 1024x768 screen has an 896x704 viewport, so 630 784 bytes a slot,
   2.4 MB of device-local and as much again host-visible, 4.8 MB in all; a
   1920x1080 screen has 1792x1016, so 1 820 672 a slot and 13.9 MB in all. (The
   image's own allocation is whatever vkGetImageMemoryRequirements asks for an
   optimal-tiled R8 of that extent, so at or above those numbers.) The pass
   allocates none of it until `tagpu_scaffold.on` is there and gives every byte
   of it back -- a slot at a time, as each comes round -- on the first frame it
   is handed nothing to draw, which is what makes that affordable in a 32-bit
   address space whose largest free block is the number this phase spends its
   budget measuring.

   THE CHEAPER ALTERNATIVE, and why it is not here. One image shared by every
   slot is also correct: a barrier at the top of each upload, FRAGMENT_SHADER /
   SHADER_READ -> TRANSFER / TRANSFER_WRITE, orders the copy after the previous
   frame's sampling, because submission order spans submits to one queue and a
   write-after-read hazard needs only an execution dependency. That saves
   (nimg-1) images -- 5.2 MB at 1080p -- and costs three things this one does
   not have: the image's layout has to be tracked across frames, a viewport
   change has to retire the old image until every slot has turned over, and the
   safety argument moves from an invariant one line long to a paragraph about
   cross-submit ordering. The staging buffers stay per-slot either way, because
   a barrier orders GPU work and the hazard there is a CPU write. If a later
   per-frame pass needs that memory back, this is the design to reach for; for
   the first of them, the one-line invariant is worth 6 MB behind a lever.

   A VIEWPORT CHANGE IS THEREFORE FREE. The game's viewport can change without
   the swapchain changing -- a shell/game transition alone does it -- and with
   per-slot resources the fix is to rebuild slot `slot` at the moment we are
   called with it, which is exactly the moment we own it. No retire list, no
   deferred free, no second wait.

   THE Y FLIP IS PIPELINE STATE, exactly as in tagpu_vk_fps.c: a negative
   viewport height (VK_KHR_maintenance1), never a source edit, because an edited
   shader would disagree with the GL twin that is its oracle.

   BLENDING IS PIPELINE STATE TOO, and it is new here -- the readout had none.
   The GL twin sets glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), which in
   GL sets the RGB *and* the alpha factors, so both are set here. Over the black
   field both lanes clear to, the result is the shader's colour times 0.55 on
   each side; if the two ever differ by a least significant bit this is the
   first place to look, and the A/B will say so rather than hide it.

   IT KNOWS NOTHING ABOUT A WINDOW. Everything arrives in TAGPU_VKPASS.
   A PASS READS NO ENGINE STATE: every value comes from the GL lane's hand-over,
   so this file is not on thread-split.allow and must never need to be. */

#include "tagpu_vk_pass.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_scaffold.h"
#include "tagpu_scaffold.h"
#include "spirv/tagpu_scaffold.spv.h"

#define VST 2                              /* floats per vertex: vec2 p        */

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
/* THE OVERLAY AS A SAMPLED TEXTURE, and the frame it is this frame's for.
   Cleared wherever the pass stops being able to answer for one -- a slot given
   back, a refusal, a teardown -- so that "no overlay" is what a consumer gets
   rather than a stale one. */
static int      s_liveHave;
static unsigned s_liveFrame;

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;
static VkDescriptorPool      s_dpool;
static VkSampler             s_samp;

static VkBuffer       s_vbuf, s_ubuf;
static VkDeviceMemory s_vmem, s_umem;
static unsigned char* s_umap;
static VkDeviceSize   s_ustride;

/* ---- one of these per frame slot, and every field of it is ours for the
   duration of the `prepare`/`record` pair we are handed that slot on. ---- */
typedef struct {
    VkImage         img;
    VkDeviceMemory  imem;
    VkImageView     view;
    VkBuffer        stage;
    VkDeviceMemory  smem;
    unsigned char*  smap;
    VkDescriptorSet dset;
    int             w, h;                  /* what this slot is sized for, 0 = nothing */
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
    if (type < 0) { plog(d, "scaf: no memory type for a buffer"); return 0; }
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

/* Give one slot's sized resources back. Only ever called for a slot we own --
   from `slot_size` on a viewport change, or from `down` after the seam's
   vkDeviceWaitIdle. */
static void slot_free(const TAGPU_VKPASS* d, SLOT* s)
{
    VkDevice dev = d->dev;
    if (s->view)  { vkDestroyImageView(dev, s->view, NULL); s->view = VK_NULL_HANDLE; }
    if (s->img)   { vkDestroyImage(dev, s->img, NULL); s->img = VK_NULL_HANDLE; }
    if (s->imem)  { vkFreeMemory(dev, s->imem, NULL); s->imem = VK_NULL_HANDLE; }
    /* THE UNMAP IS GUARDED BY THE MAP POINTER, NOT BY THE ALLOCATION: a
       vkMapMemory that failed leaves the allocation standing and unmapping it
       would be an error of its own -- and `slot_size` unwinds through here on
       exactly that path. */
    if (s->smap)  { vkUnmapMemory(dev, s->smem); s->smap = NULL; }
    if (s->stage) { vkDestroyBuffer(dev, s->stage, NULL); s->stage = VK_NULL_HANDLE; }
    if (s->smem)  { vkFreeMemory(dev, s->smem, NULL); s->smem = VK_NULL_HANDLE; }
    s->w = s->h = 0;
}

/* Make slot `s` carry a `w` x `h` scaffold, rebuilding it if it does not
   already. Safe because the caller owns this slot: see the file header. */
static int slot_size(const TAGPU_VKPASS* d, SLOT* s, int w, int h)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements req;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    int type;

    if (s->w == w && s->h == h) return 1;
    slot_free(d, s);

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8_UNORM;
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
    if (vkCreateImage(d->dev, &ici, NULL, &s->img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, s->img, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) { plog(d, "scaf: no device-local memory for a scaffold image"); return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s->imem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, s->img, s->imem, 0) != VK_SUCCESS) return 0;

    ivi.image = s->img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = VK_FORMAT_R8_UNORM;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, &s->view) != VK_SUCCESS) return 0;

    if (!mk_buffer(d, (VkDeviceSize)w * h, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->stage, &s->smem, &s->smap)) return 0;

    /* THE DESCRIPTOR SET IS REWRITTEN HERE AND THAT IS LEGAL FOR THE SAME
       REASON THE REST OF IT IS: this set is only ever bound by a submit that
       used this slot, and that submit has completed. */
    memset(&ii, 0, sizeof ii); memset(&wr, 0, sizeof wr);
    ii.sampler = s_samp; ii.imageView = s->view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = s->dset; wr.dstBinding = 40; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);

    s->w = w; s->h = h;
    return 1;
}

static int build_sampler(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties fp;

    /* R8_UNORM SAMPLED, ASKED FOR RATHER THAN ASSUMED. The GL twin uploads the
       scaffold as GL_R8 and the shader reads `.r` and multiplies by 255 to get
       the row key back, so the format has to be the single-channel one or the
       comparison is not of the same texels. */
    vkGetPhysicalDeviceFormatProperties(d->pd, VK_FORMAT_R8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        plog(d, "scaf: this device cannot sample R8_UNORM - the pass stays down");
        return 0;
    }
    /* NEAREST AND CLAMP_TO_EDGE, WHICH IS WHAT THE GL TWIN SETS. A texel here is
       a row KEY, not a colour: a filtered half of one is a depth that belongs to
       no row, and the shader's `(v - 3.0) / 4.0` would decode it as a different
       band. Filtering differently between the lanes would differ on every
       silhouette edge for a reason that is neither rasteriser's. */
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

static int build_pipeline(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[3];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va;
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkResult r;
    int ok = 0;

    /* THE BINDINGS ARE THE GENERATOR'S, NOT THIS FILE'S: set 0 binding 0 is the
       vertex stage's uniform block, 32 the fragment stage's and 40 its sampler,
       because tools/spirv-gen.py allocates bindings BY STAGE. The std140 offsets
       are printed in the generated header and are the contract for what goes in
       those buffers -- here `vec4 uRect` at 0 and `float uRows` at 0. */
    memset(b, 0, sizeof b);
    b[0].binding = 0;  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 32; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].binding = 40; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dli.bindingCount = 3; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS) return 0;

    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) return 0;

    vs = mk_module(d, tagpu_spv_tagpu_scaffold_VS,
                   sizeof tagpu_spv_tagpu_scaffold_VS / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_tagpu_scaffold_FS,
                   sizeof tagpu_spv_tagpu_scaffold_FS / sizeof(uint32_t));
    if (!vs || !fs) { plog(d, "scaf: a shader module was refused"); goto out; }

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    memset(&vb, 0, sizeof vb);
    vb.binding = 0; vb.stride = VST * sizeof(float);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(&va, 0, sizeof va);
    va.location = 0; va.binding = 0; va.format = VK_FORMAT_R32G32_SFLOAT; va.offset = 0;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &va;

    /* A STRIP, WHICH IS WHAT THE GL TWIN DRAWS. Four vertices as a strip and six
       as a list cover the same quad, but they do not necessarily cover it with
       the same two triangles in the same order, and a diagonal that ran the
       other way would put every pixel on it on the other side of a rounding
       decision. The oracle is a 0-px comparison; this is not a place to
       paraphrase. */
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    vp.viewportCount = 1; vp.scissorCount = 1;     /* both dynamic, set per frame */

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* NO CULLING: the negative viewport height flips the winding of every
       triangle, so a cull mode that was right under GL would throw the whole
       quad away. The GL twin does not cull either. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&cba, 0, sizeof cba);
    /* BLENDING, BECAUSE THE GL TWIN BLENDS -- and on both factor pairs, because
       glBlendFunc sets the alpha factors as well as the colour ones. The
       fragment shader writes a 0.55 alpha and `discard`s where the scaffold is
       free, so what reaches the frame is 55% of the ramp colour over whatever
       is behind it; over the A/B's black field that is 0.55 * c on both lanes. */
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
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
    /* A DEPTH STATE THAT TESTS NOTHING AND WRITES NOTHING, and it is required
       rather than tidy: since G19e the seam's render pass carries a depth
       attachment, and a pipeline built against a subpass that has one may not
       leave pDepthStencilState null. This pass's GL twin calls neither
       glEnable(GL_DEPTH_TEST) nor glDepthMask, so all three flags are off and
       the picture is what it was before the attachment existed -- which is what
       its own A/B re-measures. */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    gp.pColorBlendState = &cb;
    gp.pDepthStencilState = &ds;
    gp.pDynamicState = &dy;
    gp.layout = s_plo;
    gp.renderPass = d->rp;
    gp.subpass = 0;
    /* THE DEPTH STATE IS OFF, not absent -- see where `ds` is filled above. The
       seam's render pass carries a depth attachment since G19e's second world
       pass, and the GL/Vulkan depth-range answer that one needed
       (minDepth 0.5 / maxDepth 1.0, gpu-status §2.28) is in tagpu_vk_feat.c;
       this pass does not test, so it does not need it. */
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipe);
    if (r != VK_SUCCESS) { plog(d, "scaf: vkCreateGraphicsPipelines refused it (%d)", (int)r); goto out; }
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
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ps[1].descriptorCount = d->slots;
    dpi.maxSets = d->slots;
    dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) return 0;

    for (i = 0; i < d->slots; i++) lay[i] = s_dsl;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = d->slots;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, sets) != VK_SUCCESS) return 0;

    /* The two uniform blocks are written now and never again -- they name a
       fixed range of a buffer whose CONTENTS change per frame. The sampler at
       binding 40 is written by `slot_size`, when there is an image to name. */
    for (i = 0; i < d->slots; i++) {
        VkDescriptorBufferInfo bi[2];
        VkWriteDescriptorSet w[2];
        s_slot[i].dset = sets[i];
        memset(bi, 0, sizeof bi); memset(w, 0, sizeof w);
        bi[0].buffer = s_ubuf; bi[0].offset = i * s_ustride;                  bi[0].range = 16;
        bi[1].buffer = s_ubuf; bi[1].offset = i * s_ustride + s_ustride / 2;  bi[1].range = 16;
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = sets[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bi[0];
        w[1] = w[0]; w[1].dstBinding = 32; w[1].pBufferInfo = &bi[1];
        vkUpdateDescriptorSets(d->dev, 2, w, 0, NULL);
    }
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties props;
    VkDeviceSize ualign;
    const float quad[] = TAGPU_SCAF_QUAD;
    unsigned char* vmap = NULL;

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "scaf: %u frame slots is outside what this pass carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    if (!d->flipok) {
        plog(d, "scaf: this device does not offer VK_KHR_maintenance1, so the "
                "clip-space flip has no pipeline state to ride - the Vulkan "
                "edition of the scaffold stays down (the GL one is unaffected)");
        return 0;
    }
    if (!resolve(d)) { plog(d, "scaf: an entry point is missing"); return 0; }

    vkGetPhysicalDeviceProperties(d->pd, &props);
    /* TWO BLOCKS PER SLOT, EACH AT AN OFFSET THE DEVICE ACCEPTS.
       `minUniformBufferOffsetAlignment` is 16 on some devices and 256 on others,
       and a bound buffer offset that is not a multiple of it is undefined
       behaviour rather than a slow path. */
    ualign = props.limits.minUniformBufferOffsetAlignment;
    if (ualign < 16) ualign = 16;
    s_ustride = ualign * 2;

    /* THE QUAD IS UPLOADED ONCE AND NEVER AGAIN, so it needs no per-slot copy:
       nothing writes it after this. It is the same literal the GL lane builds
       its own buffer from (TAGPU_SCAF_QUAD in inc/tagpu_scaffold.h). */
    if (!mk_buffer(d, sizeof quad, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_vbuf, &s_vmem, &vmap)) return 0;
    memcpy(vmap, quad, sizeof quad);
    vkUnmapMemory(d->dev, s_vmem);

    if (!mk_buffer(d, s_ustride * d->slots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_ubuf, &s_umem, &s_umap)) return 0;
    if (!build_sampler(d)) return 0;
    if (!build_pipeline(d)) return 0;
    if (!build_descriptors(d)) return 0;

    plog(d, "scaf: the Vulkan edition is up - %u frame slots, uniform stride %u",
         (unsigned)d->slots, (unsigned)s_ustride);
    return 1;
}

int tagpu_vk_scaffold_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    const unsigned char* buf = NULL;
    float rect[4], rows = 0.0f, ub[4];
    int w = 0, h = 0, ab = 0;
    SLOT* s;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy rg;

    /* THE OVERLAY A CONSUMER MAY SAMPLE IS NOT THIS FRAME'S UNTIL THE UPLOAD
       BELOW HAS BEEN RECORDED. Cleared first so that every exit path leaves
       `tagpu_vk_scaffold_ready` saying no rather than yes for an older frame. */
    s_liveHave = 0;

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW: the scaffold is off
       unless `tagpu_scaffold.on` is there, and a pipeline plus nimg images for a
       pass that will never draw is exactly the work the lever exists to avoid.

       AND NOTHING IS KEPT ONCE THERE IS NOT. The per-slot images and staging
       buffers are the part of this pass that scales with the viewport -- 13.9 MB
       of a 32-bit address space at 1080p -- and an early return here held every
       byte of them for the life of the lane after one look at the scaffold.
       Giving slot `slot` back at this point needs no new argument and no timer:
       it is the same instant, and the same ownership, that the rest of this
       function writes that slot in. The lever cleared, the shell, a level
       teardown and a frame the GL twin skipped all arrive here, so after one
       turn of the slots the pass holds nothing but its pipeline, its sampler,
       its descriptor sets and a 512-byte uniform buffer -- none of which scales
       with anything. The cost of being wrong about that is one vkCreateImage a
       slot when the pass comes back, and that happens at a shell/game
       transition, not in a frame. */
    if (!tagpu_scaffold_overlay(&buf, &w, &h, rect, &rows, &ab)) {
        if (s_state == ST_READY) slot_free(d, &s_slot[slot]);
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_scaffold_down(d); s_state = ST_REFUSED; return 0; }
        s_state = ST_READY;
    }

    /* THE BOUND, RE-CHECKED. tagpu_scaffold.c gates its own viewport at 64..4096
       and TAGPU_SCAF_MAXDIM is that number, but a #define in one file bounding
       an allocation in another is a bound only while both are read together --
       and here it decides the size of a vkCreateImage and of a memcpy. */
    if (w < 1 || h < 1 || w > TAGPU_SCAF_MAXDIM || h > TAGPU_SCAF_MAXDIM) {
        plog(d, "scaf: a %dx%d scaffold is outside what this pass carries - nothing drawn", w, h);
        return 0;
    }

    s = &s_slot[slot];
    /* THIS SLOT IS OURS -- the seam waited on fence[slot] at the top of this
       frame, so the submit that last used this image, this staging buffer and
       this descriptor set has completed. That is what makes all three of the
       next steps safe with no device-wide wait: see the file header. */
    if (!slot_size(d, s, w, h)) {
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
        plog(d, "scaf: slot %u would not take a %dx%d image - the pass stops "
                "drawing and the seam tears it down", (unsigned)slot, w, h);
        s_state = ST_REFUSED;
        s_downOwed = 1;
        return 0;
    }

    memcpy(s->smap, buf, (size_t)w * h);

    /* UNDEFINED IN, because the whole image is re-sent: there are no contents to
       preserve and therefore no layout to carry between frames. TOP_OF_PIPE with
       an empty source access mask is right for the same reason the fence makes
       everything else here right -- there is no access left in flight for this
       barrier to order against. */
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    memset(&rg, 0, sizeof rg);
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = (uint32_t)w;
    rg.imageExtent.height = (uint32_t)h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, s->stage, s->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    /* The two uniform blocks, at the std140 offsets the generated header prints:
       `vec4 uRect` at 0 in the vertex block, `float uRows` at 0 in the fragment
       one -- the same four numbers and the same row count the GL draw used. */
    ub[0] = rect[0]; ub[1] = rect[1]; ub[2] = rect[2]; ub[3] = rect[3];
    memcpy(s_umap + (size_t)slot * s_ustride, ub, 16);
    ub[0] = rows; ub[1] = ub[2] = ub[3] = 0.0f;
    memcpy(s_umap + (size_t)slot * s_ustride + s_ustride / 2, ub, 16);

    /* THE A/B FRAME IS CLAIMED LAST, AFTER EVERY REASON NOT TO DRAW IS PAST. A
       frame claimed and then not drawn would have the seam capture a bare clear
       against a GL half that has the overlay in it, and report every overlay
       pixel as differing: a port failure that is really an oracle failure, which
       is the worst answer an oracle can give. (tagpu_vk_fps.c had exactly that
       defect until the G19d review's second pass.) */
    s_abFrame = ab;
    s_drawThis = 1;
    /* THE OVERLAY IS ALSO A TEXTURE OTHER PASSES SAMPLE (Phase G / G19e, the
       unit pass). It is in SHADER_READ_ONLY_OPTIMAL from the barrier above and
       stays that way for the rest of the frame, so a consumer that points its
       own descriptor set at it during its own `prepare` is naming an image
       this frame's upload has already been ordered into.
       STAMPED WITH THE FRAME, for the reason tagpu_vk_shadow.h gives at
       length: the view behind it is one image per slot and one flag for the
       pass, so a caller out of step would otherwise be handed an earlier
       frame's overlay and draw a scaffold that is not this frame's. */
    s_liveHave = 1;
    s_liveFrame = d->frame;
    return 1;
}

int tagpu_vk_scaffold_ready(unsigned frame)
{
    return s_liveHave && s_liveFrame == frame;
}

VkImageView tagpu_vk_scaffold_view(unsigned frame, uint32_t slot)
{
    if (!s_liveHave || s_liveFrame != frame || slot >= TAGPU_VK_SLOTS)
        return VK_NULL_HANDLE;
    return s_slot[slot].view;
}

void tagpu_vk_scaffold_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                              uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = 0;

    (void)d;
    if (s_state != ST_READY || !s_drawThis) return;
    s_drawThis = 0;

    /* THE FLIP, AND IT IS THE WHOLE OF IT: y starts at the bottom and the height
       is negative, so clip space is turned over once and the ported shader keeps
       GL's convention without a character changing. */
    vp.x = 0.0f;
    vp.y = (float)h;
    vp.width = (float)w;
    vp.height = -(float)h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    /* The scissor is NOT flipped: it is a framebuffer rectangle and has no clip
       space in it. */
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = w; sc.extent.height = h;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1,
                            &s_slot[slot].dset, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_vbuf, &off);
    vkCmdDraw(cb, TAGPU_SCAF_QUADV, 1, 0, 0);
}

int tagpu_vk_scaffold_ab_frame(void)
{
    int a = s_abFrame;
    s_abFrame = 0;
    return a;
}

void tagpu_vk_scaffold_down(const TAGPU_VKPASS* d)
{
    VkDevice dev = d->dev;
    uint32_t i;
    /* the views are about to be destroyed, so no consumer may be handed one */
    s_liveHave = 0;
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
    if (s_pipe)  { vkDestroyPipeline(dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dpool) { vkDestroyDescriptorPool(dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_samp)  { vkDestroySampler(dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_vbuf)  { vkDestroyBuffer(dev, s_vbuf, NULL); s_vbuf = VK_NULL_HANDLE; }
    if (s_vmem)  { vkFreeMemory(dev, s_vmem, NULL); s_vmem = VK_NULL_HANDLE; }
    if (s_umap)  { vkUnmapMemory(dev, s_umem); s_umap = NULL; }
    if (s_ubuf)  { vkDestroyBuffer(dev, s_ubuf, NULL); s_ubuf = VK_NULL_HANDLE; }
    if (s_umem)  { vkFreeMemory(dev, s_umem, NULL); s_umem = VK_NULL_HANDLE; }
    s_drawThis = 0;
    s_abFrame = 0;
    /* ST_UNBUILT and not ST_REFUSED: a pass brought down by a mode change or a
       cleared lever must be able to come back. THE ONE EXCEPTION IS THE
       TEARDOWN THIS PASS ASKED FOR: there the device refusing resources IS the
       reason, `prepare` has already latched ST_REFUSED, and clearing it here
       would have the pass rebuild and fail again on the very next frame. */
    s_state = owed ? ST_REFUSED : ST_UNBUILT;
}

/* 1 while this pass has stopped drawing and is waiting for the seam to drain
   the device and tear it down -- see `prepare`'s refusal path. */
int tagpu_vk_scaffold_down_owed(void)
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
void tagpu_vk_scaffold_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_scaffold_down(d);          /* clears s_downOwed itself */
    s_downPaying = 0;
}
