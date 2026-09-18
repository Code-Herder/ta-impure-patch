/* The frame's bottom layer on the Vulkan lane: TA's own 8-bit screen resolved
   through the presented palette. The header carries the argument.

   THE SHADER IS THE FORK'S, NOT THIS FILE'S. `tagpu_spv_openglshader_*` is
   generated from inc/openglshader.h, which is what the GL lane draws through
   `g_ogl.main_program`. The two lanes therefore run the same GLSL, which is the
   whole rule tools/spirv-gen.py exists to keep.

   ORIENTATION: THIS PASS DOES NOT TAKE THE CLIP-SPACE FLIP, and it is the only
   ported one that does not. The first build here did take it -- on the argument
   that a ported shader fed the GL lane's own vertices reproduces the GL lane's
   image -- and it drew TA's shell UPSIDE DOWN. The argument is sound for the
   other passes and wrong for this one: theirs COMPUTE a clip position from
   uniforms written in GL's convention, so turning clip space over is what makes
   the arithmetic land the same way. This one transforms nothing -- MVPMatrix is
   the identity and the quad is a literal already in clip space with its texture
   coordinates attached to its corners -- so flipping moves the quad AND its
   texcoords together and simply mirrors the picture.
   `record` states the mapping directly instead, and says why.

   [The vulkan-only plan, landing 4c-1.] */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_vk_surf.h"
#include "tagpu_surf.h"
#include "spirv/openglshader.spv.h"

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

#define PAL_W 256                      /* the palette image, 256 x 1 RGBA */
#define VF    12                       /* floats per vertex: three vec4s  */
#define NV    6                        /* two triangles, listed           */

static int s_state;
static int s_downOwed;
static int s_downPaying;
static int s_drawThis;
static int s_dx, s_dy, s_dw, s_dh;     /* where `record` puts it, this frame */

static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;
static VkDescriptorPool      s_dpool;
static VkSampler             s_samp;

static VkBuffer       s_vbuf, s_ubuf;
static VkDeviceMemory s_vmem, s_umem;
static unsigned char* s_umap;
static VkDeviceSize   s_ustride;

typedef struct {
    VkImage         img,  pimg;
    VkDeviceMemory  imem, pmem;
    VkImageView     view, pview;
    VkBuffer        stage;
    VkDeviceMemory  smem;
    unsigned char*  smap;
    VkDescriptorSet dset;
    int             w, h;              /* what this slot is sized for, 0 = nothing */
    unsigned        serial;            /* the snapshot serial this slot holds      */
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
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet wr[2];

    if (s->w == w && s->h == h) return 1;
    slot_free(d, s);

    if (!mk_image(d, w, h, VK_FORMAT_R8_UNORM, &s->img, &s->imem, &s->view)) return 0;
    if (!mk_image(d, PAL_W, 1, VK_FORMAT_R8G8B8A8_UNORM, &s->pimg, &s->pmem, &s->pview))
        return 0;

    /* ONE STAGING BUFFER FOR BOTH, the surface first and the palette after it.
       They are uploaded in the same `prepare` and neither outlives it, so a
       second allocation would buy nothing but a second failure path. */
    if (!mk_buffer(d, (VkDeviceSize)w * h + PAL_W * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->stage, &s->smem, &s->smap)) return 0;

    /* THE DESCRIPTOR SET IS REWRITTEN HERE AND THAT IS LEGAL FOR THE SAME
       REASON THE REST OF IT IS: this set is only ever bound by a submit that
       used this slot, and that submit has completed. */
    memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
    ii[0].sampler = s_samp; ii[0].imageView = s->view;
    ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].sampler = s_samp; ii[1].imageView = s->pview;
    ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = s->dset; wr[0].dstBinding = 40; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[0].pImageInfo = &ii[0];
    wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[1].dstSet = s->dset; wr[1].dstBinding = 41; wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[1].pImageInfo = &ii[1];
    vkUpdateDescriptorSets(d->dev, 2, wr, 0, NULL);

    s->w = w; s->h = h;
    return 1;
}

static int build_sampler(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties fp;

    vkGetPhysicalDeviceFormatProperties(d->pd, VK_FORMAT_R8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        plog(d, "surf: this device cannot sample R8_UNORM - the bottom layer stays down");
        return 0;
    }
    /* NEAREST, AND IT IS A CORRECTNESS SETTING RATHER THAN A LOOK. The texel is
       a palette INDEX: half way between index 7 and index 8 is index 7.5, which
       resolves to a colour that is in no palette entry and belongs to neither
       neighbour. render_ogl.c:391 sets GL_NEAREST on the same texture for the
       same reason, and :526 sets NEAREST + CLAMP_TO_EDGE on the palette. */
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
    VkVertexInputAttributeDescription va[3];
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

    /* THE BINDINGS ARE THE GENERATOR'S, printed in the header it emitted: the
       vertex stage's uniform block at 0, and the fragment stage's two samplers
       at 40 and 41, because tools/spirv-gen.py allocates bindings BY STAGE. */
    memset(b, 0, sizeof b);
    b[0].binding = 0;  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 40; b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].binding = 41; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dli.bindingCount = 3; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS) return 0;

    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) return 0;

    vs = mk_module(d, tagpu_spv_openglshader_PASSTHROUGH_VERT_SHADER,
                   sizeof tagpu_spv_openglshader_PASSTHROUGH_VERT_SHADER / sizeof(uint32_t));
    fs = mk_module(d, tagpu_spv_openglshader_PALETTE_FRAG_SHADER,
                   sizeof tagpu_spv_openglshader_PALETTE_FRAG_SHADER / sizeof(uint32_t));
    if (!vs || !fs) { plog(d, "surf: a shader module was refused"); goto out; }

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";

    /* THE LOCATIONS ARE ATTR_LOCATIONS' AND THIS IS THE OBLIGATION THAT TABLE
       NAMES. The fork asks the linker where its attributes landed
       (glGetAttribLocation), so there is no layout(location=) in the GLSL and
       spirv-gen assigns them from a table in its manifest -- VertexCoord 0,
       COLOR 1, TexCoord 2. A vertex buffer laid out any other way would feed
       the shader the wrong vec4s and there is no link step left to catch it. */
    memset(&vb, 0, sizeof vb);
    vb.binding = 0; vb.stride = VF * sizeof(float);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(va, 0, sizeof va);
    va[0].location = 0; va[0].binding = 0;
    va[0].format = VK_FORMAT_R32G32B32A32_SFLOAT; va[0].offset = 0;
    va[1].location = 1; va[1].binding = 0;
    va[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; va[1].offset = 4 * sizeof(float);
    va[2].location = 2; va[2].binding = 0;
    va[2].format = VK_FORMAT_R32G32B32A32_SFLOAT; va[2].offset = 8 * sizeof(float);
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = va;

    /* A LIST OF TWO TRIANGLES, WHICH IS WHAT THE GL LANE DRAWS -- its four
       vertices through a six-index element buffer (render_ogl.c:1427). */
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    vp.viewportCount = 1; vp.scissorCount = 1;   /* both dynamic, set in record */

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* NO CULLING. The negative-height viewport turns clip space over, which
       turns the winding over with it; a cull mode here would have to be stated
       against the flipped sense, and this is one quad. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* THE DEPTH STATE IS DECLARED AND OFF. The seam's subpass has a depth
       attachment, and a null pDepthStencilState there is invalid -- so a pass
       that does not test depth still says so rather than omitting it
       (tagpu_vk_pass.h). This is the frame's BOTTOM layer: it neither tests
       against the world nor writes anything the world would test against. */
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    /* OPAQUE, AND IT MUST BE. This replaces the clear over the viewport rect;
       blending it would mix TA's frame with the lever's clear colour. */
    memset(&cba, 0, sizeof cba);
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
    if (r != VK_SUCCESS) { plog(d, "surf: the pipeline was refused (%d)", (int)r); goto out; }
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
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSet sets[TAGPU_VK_SLOTS];
    VkDescriptorBufferInfo bi;
    VkWriteDescriptorSet wr;
    uint32_t i;

    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; ps[0].descriptorCount = d->slots;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = d->slots * 2;           /* the surface and its palette */
    dpi.maxSets = d->slots;
    dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) return 0;

    for (i = 0; i < d->slots; i++) lay[i] = s_dsl;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = d->slots;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, sets) != VK_SUCCESS) return 0;

    for (i = 0; i < d->slots; i++) {
        s_slot[i].dset = sets[i];
        /* THE UNIFORM BLOCK IS WRITTEN ONCE, HERE: it is one slot's window into
           a buffer that never moves, and the only thing in it is a matrix this
           pass never changes. The images are written per slot in `slot_size`. */
        memset(&bi, 0, sizeof bi); memset(&wr, 0, sizeof wr);
        bi.buffer = s_ubuf; bi.offset = (VkDeviceSize)i * s_ustride; bi.range = 64;
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = sets[i]; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr.pBufferInfo = &bi;
        vkUpdateDescriptorSets(d->dev, 1, &wr, 0, NULL);
    }
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties props;
    VkDeviceSize ualign;
    unsigned char* vmap = NULL;
    uint32_t i;
    /* THE GL LANE'S OWN QUAD (render_ogl.c:557 and :567), its four vertices
       expanded through the 0,1,2 0,2,3 element order into a list, and its
       tex coords at 1.0 rather than scale_w/scale_h because this image is
       sized exactly w x h and needs no padding to address around.
       COLOR is unused by PALETTE_FRAG_SHADER -- the vertex stage forwards it to
       a varying the fragment stage does not read -- but the attribute is
       DECLARED, so it is bound rather than left undefined. */
    static const float quad[NV * VF] = {
        /* VertexCoord         COLOR                 TexCoord            */
        -1.f,-1.f, 0.f, 1.f,   1.f, 1.f, 1.f, 1.f,   0.f, 0.f, 0.f, 0.f,
        -1.f, 1.f, 0.f, 1.f,   1.f, 1.f, 1.f, 1.f,   0.f, 1.f, 0.f, 0.f,
         1.f, 1.f, 0.f, 1.f,   1.f, 1.f, 1.f, 1.f,   1.f, 1.f, 0.f, 0.f,
        -1.f,-1.f, 0.f, 1.f,   1.f, 1.f, 1.f, 1.f,   0.f, 0.f, 0.f, 0.f,
         1.f, 1.f, 0.f, 1.f,   1.f, 1.f, 1.f, 1.f,   1.f, 1.f, 0.f, 0.f,
         1.f,-1.f, 0.f, 1.f,   1.f, 1.f, 1.f, 1.f,   1.f, 0.f, 0.f, 0.f,
    };
    /* MVPMatrix IS THE IDENTITY, which is what render_ogl.c:622 sets: the quad
       above is already in clip space and the vertex stage transforms nothing. */
    static const float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "surf: %u frame slots is outside what this pass carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    if (!d->flipok) {
        plog(d, "surf: this device does not offer VK_KHR_maintenance1, so the "
                "clip-space flip has no pipeline state to ride - the bottom "
                "layer stays down and the seam's clear carries the frame");
        return 0;
    }
    if (!resolve(d)) { plog(d, "surf: an entry point is missing"); return 0; }

    vkGetPhysicalDeviceProperties(d->pd, &props);
    ualign = props.limits.minUniformBufferOffsetAlignment;
    if (ualign < 64) ualign = 64;
    s_ustride = ualign;

    if (!mk_buffer(d, sizeof quad, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_vbuf, &s_vmem, &vmap)) return 0;
    memcpy(vmap, quad, sizeof quad);
    vkUnmapMemory(d->dev, s_vmem);

    if (!mk_buffer(d, s_ustride * d->slots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_ubuf, &s_umem, &s_umap)) return 0;
    for (i = 0; i < d->slots; i++)
        memcpy(s_umap + (size_t)i * s_ustride, ident, sizeof ident);

    if (!build_sampler(d)) return 0;
    if (!build_pipeline(d)) return 0;
    if (!build_descriptors(d)) return 0;

    plog(d, "surf: the bottom layer is up - the fork's own blit, %u frame slots",
         (unsigned)d->slots);
    return 1;
}

int tagpu_vk_surf_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_SURFFRAME sf;
    SLOT* s;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy rg;
    int fresh;

    s_drawThis = 0;
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
       the last upload into THIS slot left it in. */
    fresh = !s->haveSerial || s->serial != sf.serial;
    if (fresh) {
        memcpy(s->smap, sf.bytes, (size_t)sf.w * sf.h);
        memcpy(s->smap + (size_t)sf.w * sf.h, sf.pal, PAL_W * 4);

        /* UNDEFINED IN, because the whole image is re-sent: there are no
           contents to preserve and therefore no layout to carry between
           frames. */
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.image = s->img;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        b.image = s->pimg;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

        memset(&rg, 0, sizeof rg);
        rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg.imageSubresource.layerCount = 1;
        rg.imageExtent.width = (uint32_t)sf.w;
        rg.imageExtent.height = (uint32_t)sf.h;
        rg.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, s->stage, s->img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        rg.bufferOffset = (VkDeviceSize)sf.w * sf.h;
        rg.imageExtent.width = PAL_W;
        rg.imageExtent.height = 1;
        vkCmdCopyBufferToImage(cb, s->stage, s->pimg,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.image = s->img;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        b.image = s->pimg;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

        s->serial = sf.serial;
        s->haveSerial = 1;
    }

    /* WHERE IT GOES, THIS FRAME'S. Kept for `record`, which is handed the
       swapchain extent and nothing else. */
    s_dx = sf.dx; s_dy = sf.dy; s_dw = sf.dw; s_dh = sf.dh;
    s_drawThis = 1;
    return 1;
}

void tagpu_vk_surf_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h)
{
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize off = 0;

    (void)d; (void)w; (void)h;
    if (s_state != ST_READY || !s_drawThis) return;
    s_drawThis = 0;

    /* NO FLIP HERE, AND THIS PASS IS THE EXCEPTION AMONG THE PORTED ONES.
       MEASURED: the negative-height viewport every other pass uses drew TA's
       shell upside down. The difference is in what the vertex stage does. Those
       passes COMPUTE a clip position from uniforms written in GL's convention,
       so turning clip space over is what makes the arithmetic land the same
       way. This one transforms nothing -- MVPMatrix is the identity and the
       quad below is a literal already in clip space, with its texture
       coordinates attached to its corners. Flipping clip space moves the quad
       AND its texcoords together: it still covers the viewport, but v = 0
       arrives at the other end of it, which is a vertical mirror of the frame.

       So the mapping is stated directly instead. GL puts tex (0,0) -- TA's row
       0, the TOP of its screen -- at clip y = -1, which is the bottom in GL and
       reaches the window the right way up because GL's framebuffer is bottom-up.
       Vulkan's clip y = -1 is already the TOP of the viewport, so the same
       vertices with no flip put TA's row 0 at the top of the window, which is
       where it belongs.

       THE RECT IS THE FRAME'S VIEWPORT, not the window: the letterbox is the
       seam's clear and this must not paint over it. */
    vp.x = (float)s_dx;
    vp.y = (float)s_dy;
    vp.width = (float)s_dw;
    vp.height = (float)s_dh;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);

    /* The scissor is NOT flipped: it is a framebuffer rectangle and has no clip
       space in it. It is the same rect, so a viewport whose flip put a fragment
       outside it is clipped rather than drawn somewhere unintended. */
    sc.offset.x = s_dx; sc.offset.y = s_dy;
    sc.extent.width = (uint32_t)s_dw; sc.extent.height = (uint32_t)s_dh;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1,
                            &s_slot[slot].dset, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s_vbuf, &off);
    vkCmdDraw(cb, NV, 1, 0, 0);
}

void tagpu_vk_surf_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    if (!d || !d->dev) return;
    if (!vkDestroyImageView) return;        /* never resolved: nothing was made */
    for (i = 0; i < TAGPU_VK_SLOTS; i++) slot_free(d, &s_slot[i]);
    if (s_dpool) { vkDestroyDescriptorPool(d->dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_pipe)  { vkDestroyPipeline(d->dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(d->dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(d->dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_samp)  { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_umap)  { vkUnmapMemory(d->dev, s_umem); s_umap = NULL; }
    if (s_ubuf)  { vkDestroyBuffer(d->dev, s_ubuf, NULL); s_ubuf = VK_NULL_HANDLE; }
    if (s_umem)  { vkFreeMemory(d->dev, s_umem, NULL); s_umem = VK_NULL_HANDLE; }
    if (s_vbuf)  { vkDestroyBuffer(d->dev, s_vbuf, NULL); s_vbuf = VK_NULL_HANDLE; }
    if (s_vmem)  { vkFreeMemory(d->dev, s_vmem, NULL); s_vmem = VK_NULL_HANDLE; }
    s_drawThis = 0;
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
