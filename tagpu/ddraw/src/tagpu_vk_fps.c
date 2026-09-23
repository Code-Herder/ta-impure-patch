/* tagpu_vk_fps.c -- the frame-rate readout, drawn by Vulkan. Contract:
   tagpu_vk_fps.h. The smallest pass of the fork's renderer that exercises a
   buffer, a texture, a shader and a draw with nothing depending on it.

   IT IS NOT A SECOND IMPLEMENTATION OF THE PASS, and that is the whole point.
   The geometry comes from tagpu_fps.c through `tagpu_fps_quads` -- the same
   vertices the GL lane just drew, handed over once -- the texels come from
   tagpu_text.c's atlas bytes, the same 128 KB the GL texture is uploaded from,
   and the shader is the same GLSL, translated by tools/spirv-gen.py and
   compiled to the SPIR-V in inc/spirv/tagpu_fps.spv.h. The frame size and the
   ink are the same numbers. So when the two captures are compared, what is
   being compared is two RASTERISERS and nothing else; if this file rebuilt the
   quads, a 0-px result would only mean two pieces of arithmetic agreed.

   THE Y FLIP IS PIPELINE STATE, NEVER A SOURCE EDIT. GL's clip space has +Y up
   and Vulkan's has +Y down, so the vertex shader -- which is byte-identical to
   the GL one below the declarations -- puts the readout at the bottom of the
   frame, mirrored. The fix is a NEGATIVE VIEWPORT HEIGHT (VK_KHR_maintenance1,
   core in Vulkan 1.1), which flips the whole clip space once and leaves every
   shader alone. Flipping the geometry instead would mirror each glyph, because
   the texture coordinates travel with the vertices; flipping the shader would
   make it disagree with the twin that is its oracle. If the device does not
   offer the extension this pass does not arm and says so -- there is no third
   way that is still a fair comparison.

   THE ATLAS UPLOAD WAITS FOR THE DEVICE, and that is a fence rather than a
   hope. The atlas changes when a string is rasterised into it for the first
   time -- about twenty times in a session -- and by then earlier frames may
   still be sampling the image. Recording a barrier would order the upload
   against reads in THIS command buffer and says nothing about the two or three
   submits already in flight. So the upload calls `vkDeviceWaitIdle` first: a
   few milliseconds, twenty times a session, in exchange for an ordering that
   holds by construction. (The image's contents are discarded on the way in --
   `oldLayout` is UNDEFINED -- because the whole 128 KB is re-sent.)

   ONE BUFFER SET PER FRAME SLOT. The vertices and the two uniform blocks are
   written by the CPU every frame while the GPU may still be reading the
   previous ones, so there is one copy per slot and the slot index arrives as an
   argument. What makes slot `i` free is the seam's fence wait at the top of its
   present and nothing else -- not a frame count, not "the GPU will have
   finished by now".

   THE BINDINGS ARE THE GENERATOR'S, NOT THIS FILE'S. set 0 binding 0 is the
   vertex stage's uniform block, binding 32 the fragment stage's and binding 40
   its sampler, because tools/spirv-gen.py allocates bindings BY STAGE (vertex
   from 0, fragment from 32) so that a shader used by several programs keeps one
   allocation. The std140 offsets in the generated header are the contract for
   what goes in those buffers, and the header prints them beside the code.

   IT KNOWS NOTHING ABOUT A WINDOW. Everything it touches arrives in
   TAGPU_VKPASS; it would draw into an offscreen image or another process's
   image unchanged. That is standing constraint 3 holding rather than being
   asserted. */

#include "tagpu_vk_pass.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_fps.h"
#include "tagpu_fps.h"
#include "tagpu_text.h"
#include "spirv/tagpu_fps.spv.h"

/* THE VERTEX FORMAT IS tagpu_fps.c's: (x, y, u, v) floats, six vertices a quad.
   `TAGPU_FPS_MAXV` is that file's own bound, exported so the two cannot drift
   -- and it is checked again at run time against the `nv` handed over, because
   a bound that is only a #define is a bound until someone edits one of them. */
#define VST       4                       /* floats per vertex                 */
#define VBYTES    (TAGPU_FPS_MAXV * VST * sizeof(float))

/* ---- the entry points ----------------------------------------------------
   Resolved from the seam's `gdpa`/`gipa`, never linked: this DLL must not
   import a symbol from vulkan-1.dll or a machine without Vulkan would fail to
   LOAD rather than simply not arming the lane. */
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
    X(vkDeviceWaitIdle) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

/* ---- the objects ---------------------------------------------------------
   Owned by the render thread for their whole life: built in `prepare`, used in
   `record`, destroyed in `down`, all three on that thread and no other. */
enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

static int s_state;
static int s_nvThis;                      /* vertices `prepare` left for `record` */
static int s_abFrame;                     /* this frame is the A/B's (from the GL twin) */

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;
static VkDescriptorPool      s_dpool;
static VkDescriptorSet       s_dset[TAGPU_VK_SLOTS];

static VkBuffer       s_vbuf, s_ubuf, s_stage;
static VkDeviceMemory s_vmem, s_umem, s_smem;
static unsigned char* s_vmap;             /* persistently mapped, coherent     */
static unsigned char* s_umap;
static unsigned char* s_smap;
static VkDeviceSize   s_ustride;          /* bytes per slot in s_ubuf          */

static VkImage        s_img;
static VkDeviceMemory s_imem;
static VkImageView    s_view;
static VkSampler      s_samp;
static int            s_aw, s_ah;         /* the atlas's texel dimensions      */
static unsigned       s_agen;             /* the atlas generation we uploaded  */

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

/* THE MEMORY TYPE IS CHOSEN, NOT ASSUMED. `memoryTypeBits` is the set of types
   the object can live in and the loop takes the first that also has the
   properties we need -- which is how the specification says to do it. -1 means
   the device offers none, which is a refusal and not a fallback. */
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
    if (type < 0) { plog(d, "fps: no memory type for a buffer"); return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) return 0;
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) return 0;
    if (map) {
        /* MAPPED ONCE AND LEFT MAPPED. A map per frame is a driver call per
           frame for an address that never moves, and unmapping between frames
           would gain nothing: the memory is HOST_COHERENT, so a write is
           visible to the device without a flush. */
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

static int build_image(const TAGPU_VKPASS* d)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties fp;
    VkMemoryRequirements req;
    int type;

    /* R8_UNORM SAMPLED, ASKED FOR RATHER THAN ASSUMED. The GL twin uploads the
       atlas as GL_R8 and samples `.r`, so the format has to be the single-
       channel one or the comparison is not of the same texels. It is mandatory
       in the specification's format table, so this has never fired; the day it
       does, the log names it instead of the pass drawing nothing. */
    vkGetPhysicalDeviceFormatProperties(d->pd, VK_FORMAT_R8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        plog(d, "fps: this device cannot sample R8_UNORM - the pass stays down");
        return 0;
    }

    tagpu_text_dims(&s_aw, &s_ah);
    if (s_aw <= 0 || s_ah <= 0) return 0;

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8_UNORM;
    ici.extent.width = (uint32_t)s_aw;
    ici.extent.height = (uint32_t)s_ah;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, &s_img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, s_img, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) { plog(d, "fps: no device-local memory for the atlas"); return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s_imem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, s_img, s_imem, 0) != VK_SUCCESS) return 0;

    ivi.image = s_img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = VK_FORMAT_R8_UNORM;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, &s_view) != VK_SUCCESS) return 0;

    /* NEAREST AND CLAMP_TO_EDGE, WHICH IS WHAT THE GL TWIN SETS. A texel of
       this atlas is a coverage bit and a filtered half of one is neither --
       tagpu_text.c says so where it sets the GL sampler, and a comparison
       between two lanes that filtered differently would differ on every glyph
       edge for a reason that has nothing to do with either rasteriser. */
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
    VkVertexInputAttributeDescription va[2];
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

    vs = mk_module(d, tagpu_spv_tagpu_fps_VS,
                   sizeof tagpu_spv_tagpu_fps_VS / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_tagpu_fps_FS,
                   sizeof tagpu_spv_tagpu_fps_FS / sizeof(uint32_t));
    if (!vs || !fs) { plog(d, "fps: a shader module was refused"); goto out; }

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    memset(&vb, 0, sizeof vb);
    vb.binding = 0; vb.stride = VST * sizeof(float);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(va, 0, sizeof va);
    va[0].location = 0; va[0].binding = 0; va[0].format = VK_FORMAT_R32G32_SFLOAT; va[0].offset = 0;
    va[1].location = 1; va[1].binding = 0; va[1].format = VK_FORMAT_R32G32_SFLOAT;
    va[1].offset = 2 * sizeof(float);
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = va;

    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;     /* both dynamic, set per frame */

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* NO CULLING, which is not laziness: the negative viewport height flips the
       winding of every triangle, so a cull mode that was right under GL would
       throw the whole readout away here. The GL twin does not cull either. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&cba, 0, sizeof cba);
    /* NO BLENDING, because the GL twin does not blend: it `discard`s a texel
       whose coverage is below a half and writes opaque ink everywhere else.
       Leaving blending on with whatever state the frame happened to carry is
       exactly the kind of difference a 0-px comparison exists to catch. */
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
    /* A DEPTH STATE THAT TESTS NOTHING AND WRITES NOTHING, and it is required
       rather than tidy: the seam's render pass carries a depth attachment, and
       a pipeline built against a subpass that has one may not leave
       pDepthStencilState null. This pass's GL twin calls neither
       glEnable(GL_DEPTH_TEST) nor glDepthMask, so all three flags are off. */
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
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipe);
    if (r != VK_SUCCESS) { plog(d, "fps: vkCreateGraphicsPipelines refused it (%d)", (int)r); goto out; }
    ok = 1;

out:
    /* The modules are consumed by pipeline creation and are of no use after it,
       whether it succeeded or not. */
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
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
    if (vkAllocateDescriptorSets(d->dev, &dai, s_dset) != VK_SUCCESS) return 0;

    for (i = 0; i < d->slots; i++) {
        VkDescriptorBufferInfo bi[2];
        VkDescriptorImageInfo ii;
        VkWriteDescriptorSet w[3];
        memset(bi, 0, sizeof bi); memset(&ii, 0, sizeof ii); memset(w, 0, sizeof w);
        bi[0].buffer = s_ubuf; bi[0].offset = i * s_ustride;          bi[0].range = 16;
        bi[1].buffer = s_ubuf; bi[1].offset = i * s_ustride + s_ustride / 2; bi[1].range = 16;
        ii.sampler = s_samp; ii.imageView = s_view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = s_dset[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bi[0];
        w[1] = w[0]; w[1].dstBinding = 32; w[1].pBufferInfo = &bi[1];
        w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[2].dstSet = s_dset[i]; w[2].dstBinding = 40; w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[2].pImageInfo = &ii;
        vkUpdateDescriptorSets(d->dev, 3, w, 0, NULL);
    }
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties props;
    VkDeviceSize ualign;

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "fps: %u frame slots is outside what this pass carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    if (!d->flipok) {
        plog(d, "fps: this device does not offer VK_KHR_maintenance1, so the "
                "clip-space flip has no pipeline state to ride - the Vulkan "
                "edition of the readout stays down (the GL one is unaffected)");
        return 0;
    }
    if (!resolve(d)) { plog(d, "fps: an entry point is missing"); return 0; }

    vkGetPhysicalDeviceProperties(d->pd, &props);
    /* TWO BLOCKS PER SLOT, EACH AT AN OFFSET THE DEVICE ACCEPTS.
       `minUniformBufferOffsetAlignment` is 16 on some devices and 256 on
       others, and a bound buffer offset that is not a multiple of it is
       undefined behaviour rather than a slow path. */
    ualign = props.limits.minUniformBufferOffsetAlignment;
    if (ualign < 16) ualign = 16;
    s_ustride = ualign * 2;

    if (!mk_buffer(d, (VkDeviceSize)VBYTES * d->slots, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_vbuf, &s_vmem, &s_vmap)) return 0;
    if (!mk_buffer(d, s_ustride * d->slots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_ubuf, &s_umem, &s_umap)) return 0;
    if (!build_image(d)) return 0;
    if (!mk_buffer(d, (VkDeviceSize)s_aw * s_ah, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_stage, &s_smem, &s_smap)) return 0;
    if (!build_pipeline(d)) return 0;
    if (!build_descriptors(d)) return 0;

    s_agen = 0;                     /* nothing uploaded yet: the first frame does */
    plog(d, "fps: the Vulkan edition is up - %dx%d atlas, %u frame slots, "
            "uniform stride %u", s_aw, s_ah, (unsigned)d->slots, (unsigned)s_ustride);
    return 1;
}

int tagpu_vk_fps_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    const float* v = NULL;
    const unsigned char* atlas;
    unsigned gen = 0;
    int nv = 0, fw = 0, fh = 0, ab = 0;
    float ub[4];

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW. The readout is off
       unless `tagpu_fps.on` is there, and building a pipeline and a 128 KB
       image for a pass that will never draw is work the lever exists to avoid.
       Asking first also means the atlas has a font in it by the time the image
       is created, which is what `tagpu_text_dims` needs to be right. */
    if (!tagpu_fps_quads(&v, &nv, &fw, &fh, &ab)) return 0;

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_fps_down(d); s_state = ST_REFUSED; return 0; }
        s_state = ST_READY;
    }

    /* THE BOUND, RE-CHECKED. TAGPU_FPS_MAXV is tagpu_fps.c's own and the
       buffer is sized from it, but a #define in one file bounding an array in
       another is a bound only while both are read together. */
    if (nv <= 0 || nv > TAGPU_FPS_MAXV || fw <= 0 || fh <= 0) {
        plog(d, "fps: %d vertices in a %dx%d frame is outside what this pass "
                "carries - nothing drawn", nv, fw, fh);
        return 0;
    }

    memcpy(s_vmap + (size_t)slot * VBYTES, v, (size_t)nv * VST * sizeof(float));

    /* The two uniform blocks, at the std140 offsets the generated header
       prints: `vec2 uFrame` at 0 in the vertex block, `vec3 uInk` at 0 in the
       fragment one. White ink, which is what the GL twin passes. */
    ub[0] = (float)fw; ub[1] = (float)fh; ub[2] = 0.0f; ub[3] = 0.0f;
    memcpy(s_umap + (size_t)slot * s_ustride, ub, 16);
    ub[0] = 1.0f; ub[1] = 1.0f; ub[2] = 1.0f; ub[3] = 0.0f;
    memcpy(s_umap + (size_t)slot * s_ustride + s_ustride / 2, ub, 16);

    atlas = tagpu_text_atlas(&gen);
    if (atlas && gen != s_agen) {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        VkBufferImageCopy rg;
        /* THE ONLY WAIT IN THIS FILE, AND ITS COST IS REAL RATHER THAN NIL.
           Frames already submitted may still be sampling this image, and a
           barrier in THIS command buffer orders nothing about them, so the
           ordering has to come from outside it. That part is sound.

           WHAT IT COSTS, said rather than implied: this is the render thread,
           and the game thread waits INFINITE on the render thread across a mode
           change, so for the length of this wait the lockstep world is behind
           the GPU. It is UNBOUNDED -- `vkDeviceWaitIdle` takes no timeout --
           and it runs when a string is rasterised into the atlas for the first
           time, about twenty times in a session. On a healthy device that is
           the drain of two or three frames in flight; on a wedged one it is as
           long as the wedge, which is the same exposure the lane's own fence
           wait bounds at a second and this one does not.

           THE BY-DESIGN ALTERNATIVE, and why it is not here: a second image
           uploaded into while the first is sampled, swapped when every slot has
           turned over. tagpu_vk_scaffold.c, a pass that uploads EVERY frame,
           uses something simpler than a swap: one image per FRAME SLOT, which
           the seam's fence already proves free, so there is no in-flight problem
           left to move. This file deliberately does not match it -- twenty
           stalls a session buys nothing back, and this atlas is 128 KB against
           that pass's whole viewport.

           A FAILED WAIT IS NOT AN UPLOAD. If the device is lost, writing into
           an image a live frame may be sampling is exactly what the wait was
           for, so the upload is skipped and the generation left unclaimed; the
           lane comes down on the next fatal result and rebuilds. */
        if (vkDeviceWaitIdle(d->dev) != VK_SUCCESS) {
            plog(d, "fps: the device would not go idle - the atlas upload is "
                    "deferred to a later frame");
            return 0;
        }
        memcpy(s_smap, atlas, (size_t)s_aw * s_ah);

        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;   /* the whole image is re-sent */
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s_img;
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
        rg.imageExtent.width = (uint32_t)s_aw;
        rg.imageExtent.height = (uint32_t)s_ah;
        rg.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, s_stage, s_img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        s_agen = gen;
    }

    /* AN IMAGE THAT HAS NEVER BEEN UPLOADED IS UNDEFINED, not empty. The first
       frame after the pass is built always records the copy above (s_agen is 0
       and tagpu_text's counter starts at 1), so this cannot fire -- it is here
       because "cannot fire" is a claim about another file's initial value. */
    if (!s_agen) return 0;

    /* THE A/B FRAME IS CLAIMED LAST, AFTER EVERY REASON NOT TO DRAW IS PAST.
       Claimed when the quads arrive, every `return 0` between there and here --
       a build that failed, the vertex bound, a device that would not go idle for
       the atlas upload, an atlas never uploaded -- would leave it claimed while
       this pass drew NOTHING. The seam would then capture a bare clear and
       report every text pixel as differing: a port failure that is really an
       oracle failure, which is the worst kind of
       answer an oracle can give. Claimed here, the flag means "this pass is
       about to draw this frame" and nothing weaker; a frame that cannot draw
       simply loses its half, and `tools/vk-ab.py` says which one is missing. */
    s_abFrame = ab;
    s_nvThis = nv;
    return 1;
}

void tagpu_vk_fps_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = (VkDeviceSize)slot * VBYTES;

    (void)d;
    if (s_state != ST_READY || !s_nvThis) return;

    /* THE FLIP, AND IT IS THE WHOLE OF IT. y starts at the bottom and the
       height is negative, so clip space is turned over once and every ported
       shader keeps GL's convention without a character changing. */
    vp.x = 0.0f;
    vp.y = (float)h;
    vp.width = (float)w;
    vp.height = -(float)h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    /* The scissor is NOT flipped: it is a framebuffer rectangle and has no
       clip space in it. */
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = w; sc.extent.height = h;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1,
                            &s_dset[slot], 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_vbuf, &off);
    vkCmdDraw(cb, (uint32_t)s_nvThis, 1, 0, 0);
    s_nvThis = 0;
}

/* 1 on the one frame the GL twin captured its half of the A/B, consumed here so
   the seam captures that frame and no other. See tagpu_fps.h. */
int tagpu_vk_fps_ab_frame(void)
{
    int a = s_abFrame;
    s_abFrame = 0;
    return a;
}

void tagpu_vk_fps_down(const TAGPU_VKPASS* d)
{
    VkDevice dev = d->dev;
    if (!dev || !vkDestroyBuffer) { s_state = ST_UNBUILT; return; }

    if (s_pipe)  { vkDestroyPipeline(dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    /* The sets go back with the pool; there is no separate free. */
    if (s_dpool) { vkDestroyDescriptorPool(dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_samp)  { vkDestroySampler(dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_view)  { vkDestroyImageView(dev, s_view, NULL); s_view = VK_NULL_HANDLE; }
    if (s_img)   { vkDestroyImage(dev, s_img, NULL); s_img = VK_NULL_HANDLE; }
    if (s_imem)  { vkFreeMemory(dev, s_imem, NULL); s_imem = VK_NULL_HANDLE; }
    /* THE UNMAP IS GUARDED BY THE MAP POINTER, NOT BY THE ALLOCATION. A
       vkMapMemory that failed leaves the allocation standing and unmapping it
       would be an error of its own -- and `build` unwinds through here on
       exactly that path. */
    if (s_vmap)  { vkUnmapMemory(dev, s_vmem); s_vmap = NULL; }
    if (s_vbuf)  { vkDestroyBuffer(dev, s_vbuf, NULL); s_vbuf = VK_NULL_HANDLE; }
    if (s_vmem)  { vkFreeMemory(dev, s_vmem, NULL); s_vmem = VK_NULL_HANDLE; }
    if (s_umap)  { vkUnmapMemory(dev, s_umem); s_umap = NULL; }
    if (s_ubuf)  { vkDestroyBuffer(dev, s_ubuf, NULL); s_ubuf = VK_NULL_HANDLE; }
    if (s_umem)  { vkFreeMemory(dev, s_umem, NULL); s_umem = VK_NULL_HANDLE; }
    if (s_smap)  { vkUnmapMemory(dev, s_smem); s_smap = NULL; }
    if (s_stage) { vkDestroyBuffer(dev, s_stage, NULL); s_stage = VK_NULL_HANDLE; }
    if (s_smem)  { vkFreeMemory(dev, s_smem, NULL); s_smem = VK_NULL_HANDLE; }
    s_agen = 0;
    s_nvThis = 0;
    s_abFrame = 0;
    /* ST_UNBUILT and not ST_REFUSED: a lane brought down by a mode change or a
       cleared lever must be able to come back. `build` sets ST_REFUSED itself
       when the device is the reason. */
    s_state = ST_UNBUILT;
}
