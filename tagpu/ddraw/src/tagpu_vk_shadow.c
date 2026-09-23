/* tagpu_vk_shadow.c -- the Classic++ cast-shadow depth map, drawn by Vulkan.
   The FIFTH world pass. The GL twin is tagpu_shadow.c and is
   the oracle; the header tagpu_vk_shadow.h has the contract.

   ---- WHAT THIS PASS DOES THAT THE OTHERS DO NOT, AND WHY ----

   1. IT OWNS A SECOND RENDER TARGET, AND THAT IS NOT A BREACH OF THE SEAM.
      Standing constraint 3 says surface, swapchain, acquire and present live in
      tagpu_vk.c and that nothing else may know a window exists. A render pass
      and a framebuffer over an image this file allocated name no window: the
      seam still owns the one image that reaches a screen. So the second target
      lives HERE rather than in the seam, which is also what keeps the seam's
      file from growing a second personality every time a pass needs somewhere
      to draw. The cost is that this pass records a whole render pass inside
      `prepare` -- legal precisely because `prepare` is the hook the seam calls
      OUTSIDE its own vkCmdBeginRenderPass, and render passes may not nest.

   2. GL'S CLIP-SPACE Z RANGE, AND THIS IS THE PASS THAT NEEDS IT.
      Every other world pass writes a clip z already in [0, 1], so
      `minDepth 0.5 / maxDepth 1.0` reproduces GL's (z+1)/2 exactly and nothing
      is clipped (tagpu_vk_feat.c item 1, which says this extension would be
      the answer only if a shader were ever found writing a z below 0).
      The shadow matrix IS that shader: tagpu_shadow.c's `mrow` builds an
      orthographic projection that fills [-1, 1] by construction, so under
      Vulkan's own convention the near half of every caster is CLIPPED AWAY and
      the map is wrong rather than merely offset. `VK_EXT_depth_clip_control`
      with `negativeOneToOne` adopts GL's rule for this pipeline; the seam
      queries the feature and publishes it as TAGPU_VKPASS::zclipok, and this
      pass stands down without it. And the values matter as much as the
      geometry: the consumers' taShadowAt compares `p.z * 0.5 + 0.5` against
      what is STORED here, so the viewport transform has to be GL's arithmetic
      and the format has to be GL's quantisation (24-bit fixed point, because
      the GL map is GL_DEPTH_COMPONENT24).

   3. THERE IS NO Y FLIP HERE, AND THAT IS NOT AN OMISSION.
      This target is SAMPLED, not presented, and that alone settles it.
      [For the other passes, being presented is not the question either; the
      SHADER's y convention is. A pass whose shader writes GL's window
      convention (`1 - y*2`) flips; one that writes the engine's screen-space
      y, which grows downward, does not, because clip -1 is already the game
      frame's top row. That second group -- terrain, features, effects, units,
      markers -- takes a POSITIVE height too, for a different reason than this
      pass does. tagpu_vk_pass.h's `flipok` carries the whole table.]
      In GL, clip y = -1 is window row 0, which is texel row 0, which is v = 0.
      In Vulkan with a positive viewport height, clip y = -1 is framebuffer row
      0, which is texel row 0, which is v = 0. The two agree already, and
      flipping would put every shadow in the wrong half of the map. The flip is
      a property of presentation, not of Vulkan.
      Cull is off on both sides (the GL twin disables GL_CULL_FACE), so the
      winding a flip would also have inverted is not in play either.

   4. THE MAP IS PER FRAME SLOT. It is written every frame and sampled in the
      same frame by the consumers; with frames in flight, one image would have
      frame N's writes racing frame N-1's reads. One image per slot makes the
      seam's fence the whole argument, exactly as the scaffold's per-slot upload
      does -- and it means a resolution change (the zoom octave moves) is a
      rebuild of the slot we are being handed, under its own fence, rather than
      a retire. 16 MB a slot at the default shadowres 2048, 64 MB at the 4096
      ceiling, and none of it allocated until a map is actually drawn.

   5. THE CASTER MESH IS SHARED AND ITS SIZE IS DATA, so it takes the terrain
      pass's retire: the old buffers are held until every slot has passed
      through its own `prepare` once, and `pending == 0` is then "no submitted
      command buffer names them and no future one will", by construction.

   ---- WHAT IT DOES NOT DO ----

   ONLY THE HEIGHTFIELD CASTS. The GL map is drawn from four kinds of geometry
   -- the native 3DO stream, the posed program's depth twin, the replacement
   meshes and the heightfield -- and only the heightfield has a CPU mirror on
   this side of the seam today. A map missing a caster is a different map, so
   tagpu_shadow.c counts the casters it drew that the hand-over carries no copy
   of and this pass REFUSES the frame outright rather than draw an incomplete
   one. The unit pass is the landing that closes it. In practice that means a
   fixture with no unit on screen, which `static-terrain` is.

   IT KNOWS NOTHING ABOUT A WINDOW. Everything arrives in TAGPU_VKPASS.
   A PASS READS NO ENGINE STATE: every value comes from the GL lane's
   hand-over, so this file is not on thread-split.allow and must never be. */

#include "tagpu_vk_pass.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_shadow.h"
#include "tagpu_vk_unit.h"
#include "tagpu_vk_hires.h"   /* the posed casters, drawn inside our render pass */
#include "spirv/tagpu_shadow.spv.h"   /* generated from src/tagpu_shadow_glsl.h */

#define UBLK 64                            /* std140: one mat4, the generated  */
                                           /* header's own figure              */

/* ---- the entry points ----------------------------------------------------
   Resolved from the seam's `gdpa`/`gipa`, never linked, and this pass's own
   (tagpu_vk_pass.h says why every pass carries its own table). */
#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFormatProperties)

#define DFNS(X) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdBindDescriptorSets) X(vkCmdDrawIndexed) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdCopyBuffer) X(vkCmdPipelineBarrier)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

static int s_state;
static int s_downOwed;                     /* a teardown the seam still owes us */
static int s_downPaying;                   /* ...and the seam is paying it NOW  */
static int s_saidCasters;                  /* the refusals, each said once      */
static int s_saidUnitShort;                /* the unit pass owed casters, was short */
static int s_saidZclip;
static int s_saidFormat;
static int s_saidRes;

/* what stands after `prepare`, for the consumers to ask about */
static unsigned s_liveFrame;               /* the frame the map was drawn for   */
static int      s_liveHave;                /* ...and whether one was            */
static int      s_res;                     /* its edge in texels                */

static VkRenderPass          s_rp;         /* depth only, ours                  */
static VkFormat              s_dfmt = VK_FORMAT_UNDEFINED;
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;
static VkDescriptorPool      s_dpool;

static VkBuffer       s_ubuf;              /* one mat4 per slot                 */
static VkDeviceMemory s_umem;
static unsigned char* s_umap;
static VkDeviceSize   s_ustride;

/* ---- THE CASTER MESH, SHARED, AND THE RETIRE THAT MAKES REPLACING IT SAFE.
   The terrain pass's shape (tagpu_vk_terr.c "A SHARED IMAGE, and the retire"):
   `pending` is a bitmask of slots whose last submitted command buffer may still
   NAME the old buffers, a bit clears only under that slot's own fence, and
   `pending == 0` is what licenses the destroy. Buffers rather than images here,
   and they are named by the command buffer rather than by a descriptor set,
   which changes nothing about the argument. */
typedef struct {
    VkBuffer       vbuf, ibuf;
    VkDeviceMemory vmem, imem;
    VkDeviceSize   vbytes, ibytes;
    unsigned       serial;                 /* the mirror serial they hold       */
    int            have;                   /* a copy has been recorded into them*/

    VkBuffer       oldV, oldI;
    VkDeviceMemory oldVM, oldIM;
    uint32_t       pending;
} MESH;
static MESH s_mesh;

/* ---- one of these per frame slot ---------------------------------------- */
typedef struct {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    VkFramebuffer  fb;
    int            res;                    /* what the image is sized for       */

    VkBuffer       stage;                  /* the mesh upload, when one is due  */
    VkDeviceMemory stageMem;
    unsigned char* stageMap;
    VkDeviceSize   stageCap;

    VkDescriptorSet dset;
} SLOT;
static SLOT s_slot[TAGPU_VK_SLOTS];

/* THE BOUNDS ARE RE-CHECKED HERE, because a bound that lives in the file that
   produced the number is a bound only while both files are read together.
   `res` sizes an image and both draw ranges size a buffer and a memcpy.
   tagpu_shadow.c clamps its own resolution to 256..4096 (`shadowres`, and the
   octave loop bounded by GL_MAX_TEXTURE_SIZE); the vertex count is one per
   16-px grid point of a map tagpu_terr.c already refuses past 4096 a side, so
   4097*4097 is the ceiling and 6 indices per cell the multiplier. Stated in
   the terms THIS file allocates in, so that neither file has to be read to
   trust the other. They must not be TIGHTER than the producer's: a pass that
   refused a mesh the GL twin drew would leave the consumers sampling a map
   that is missing the ground. */
#define RES_MIN     256
#define RES_MAX     4096
#define MESH_MAXV   (4097u * 4097u)
#define MESH_MAXI   (4096u * 4096u * 6u)

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
    if (type < 0) { plog(d, "shadow: no memory type for a buffer"); return 0; }
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

static void kill_buffer(const TAGPU_VKPASS* d, VkBuffer* buf, VkDeviceMemory* mem)
{
    if (*buf) { vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; }
    if (*mem) { vkFreeMemory(d->dev, *mem, NULL);    *mem = VK_NULL_HANDLE; }
}

/* the depth image, its view and the framebuffer over it, all three or none */
static int mk_target(const TAGPU_VKPASS* d, int res, SLOT* s)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkMemoryRequirements req;
    int type;

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = s_dfmt;
    ici.extent.width = (uint32_t)res;
    ici.extent.height = (uint32_t)res;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, &s->img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, s->img, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) { plog(d, "shadow: no device-local memory for the map"); return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s->mem) != VK_SUCCESS) return 0;
    if (vkBindImageMemory(d->dev, s->img, s->mem, 0) != VK_SUCCESS) return 0;

    ivi.image = s->img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = s_dfmt;
    /* THE DEPTH ASPECT ALONE, even when the format carries stencil: a view a
       consumer samples through must name exactly one aspect, and the GL map is
       GL_DEPTH_COMPONENT24 with no stencil at all. */
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, &s->view) != VK_SUCCESS) return 0;

    fci.renderPass = s_rp;
    fci.attachmentCount = 1;
    fci.pAttachments = &s->view;
    fci.width = (uint32_t)res;
    fci.height = (uint32_t)res;
    fci.layers = 1;
    if (vkCreateFramebuffer(d->dev, &fci, NULL, &s->fb) != VK_SUCCESS) return 0;
    s->res = res;
    return 1;
}

static void kill_target(const TAGPU_VKPASS* d, SLOT* s)
{
    if (s->fb)   { vkDestroyFramebuffer(d->dev, s->fb, NULL); s->fb = VK_NULL_HANDLE; }
    if (s->view) { vkDestroyImageView(d->dev, s->view, NULL); s->view = VK_NULL_HANDLE; }
    if (s->img)  { vkDestroyImage(d->dev, s->img, NULL);      s->img = VK_NULL_HANDLE; }
    if (s->mem)  { vkFreeMemory(d->dev, s->mem, NULL);        s->mem = VK_NULL_HANDLE; }
    s->res = 0;
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

/* ---- the format ---------------------------------------------------------
   ASKED FOR BY NAME rather than discovered in a wrong picture, as the feature
   pass's sampler check is. Two things are wanted of it at
   once and neither is guaranteed: it must be a depth-stencil attachment AND a
   sampled image, and the consumers' PCF taps it through a LINEAR compare
   sampler, which is a third feature bit again.
   24-BIT FIXED POINT OR NOTHING. The GL map is GL_DEPTH_COMPONENT24 and the
   consumer compares a float it computed against what is stored here, so a
   32-bit float attachment would disagree with the GL twin in the last bits of
   every tap -- which is exactly the kind of difference a 0-px oracle exists to
   catch and cannot be argued away. */
static VkFormat s_fmtCache = VK_FORMAT_UNDEFINED;
static int      s_fmtLinear;
static int      s_fmtAsked;

VkFormat tagpu_vk_shadow_format(const TAGPU_VKPASS* d, int* linearOk)
{
    static const VkFormat want[2] = {
        VK_FORMAT_X8_D24_UNORM_PACK32,      /* GL_DEPTH_COMPONENT24 exactly */
        VK_FORMAT_D24_UNORM_S8_UINT         /* the same 24 bits, plus stencil */
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    int i;
    if (!s_fmtAsked) {
        /* THE ONE ENTRY POINT THIS NEEDS, RESOLVED HERE. A consumer may ask
           before `prepare` has ever run, so `resolve` may not have. */
        PFN_vkGetPhysicalDeviceFormatProperties gp =
            vkGetPhysicalDeviceFormatProperties
                ? vkGetPhysicalDeviceFormatProperties
                : (PFN_vkGetPhysicalDeviceFormatProperties)
                      d->gipa(d->inst, "vkGetPhysicalDeviceFormatProperties");
        s_fmtAsked = 1;
        if (gp) {
            for (i = 0; i < 2; i++) {
                VkFormatProperties fp;
                memset(&fp, 0, sizeof fp);
                gp(d->pd, want[i], &fp);
                if ((fp.optimalTilingFeatures & need) == need) {
                    s_fmtLinear = (fp.optimalTilingFeatures &
                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
                    s_fmtCache = want[i];
                    break;
                }
            }
        }
    }
    if (linearOk) *linearOk = s_fmtLinear;
    return s_fmtCache;
}

/* ---- the render pass ----------------------------------------------------
   One depth attachment and no colour one at all -- which is what makes it the
   Vulkan spelling of the GL twin's `glDrawBuffers(GL_NONE)` + `glReadBuffer
   (GL_NONE)` FBO. CLEAR at 1.0 in (the twin's glClear) and STORE out, because
   unlike the frame's depth buffer this one IS read afterwards. */
static int build_rp(const TAGPU_VKPASS* d)
{
    VkAttachmentDescription at;
    VkAttachmentReference dref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo rci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    memset(&at, 0, sizeof at);
    at.format = s_dfmt;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    memset(&dref, 0, sizeof dref);
    dref.attachment = 0;
    dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &dref;

    memset(dep, 0, sizeof dep);
    /* IN: this slot's image may still be named by the frame that used this slot
       last, which sampled it in a fragment shader. The seam's fence has been
       waited on by the time `prepare` runs, so that submit is complete and this
       is belt and braces -- but a dependency costs nothing and makes the layout
       transition's scope explicit rather than implied by a fence in another
       file. */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    /* OUT: AND THIS ONE IS LOAD-BEARING. The consumers sample this image later
       in the SAME command buffer, so "the depth writes are visible to a
       fragment shader read" is a within-submit ordering question and nothing
       else in the frame answers it. */
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    rci.attachmentCount = 1; rci.pAttachments = &at;
    rci.subpassCount = 1; rci.pSubpasses = &sub;
    rci.dependencyCount = 2; rci.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &rci, NULL, &s_rp) == VK_SUCCESS;
}

/* ---- the pipeline ------------------------------------------------------- */
static int build_pipeline(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b;
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va;
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineViewportDepthClipControlCreateInfoEXT zc =
        { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    int ok = 0;

    memset(&b, 0, sizeof b);
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    dli.bindingCount = 1; dli.pBindings = &b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS) return 0;
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS) return 0;

    vs = mk_module(d, tagpu_spv_tagpu_shadow_VS_H,
                   sizeof tagpu_spv_tagpu_shadow_VS_H / 4);
    fs = mk_module(d, tagpu_spv_tagpu_shadow_FS_NONE,
                   sizeof tagpu_spv_tagpu_shadow_FS_NONE / 4);
    if (!vs || !fs) goto done;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;

    /* the mesh: one vec3 world point per vertex, tightly packed, exactly the
       GL VAO's `glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, 0)` */
    memset(&vb, 0, sizeof vb);
    vb.binding = 0; vb.stride = 12; vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(&va, 0, sizeof va);
    va.location = 0; va.binding = 0; va.format = VK_FORMAT_R32G32B32_SFLOAT; va.offset = 0;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &va;

    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* GL'S CLIP-SPACE Z, item 2 of the file header. The caller has already
       refused to get here without `zclipok`. */
    zc.negativeOneToOne = VK_TRUE;
    vp.pNext = &zc;
    vp.viewportCount = 1; vp.scissorCount = 1;

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* CULL OFF, as the GL twin's glDisable(GL_CULL_FACE): a caster's back faces
       write depth there too, and a shadow map that culled them would let the
       light through the far side of every closed body. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;     /* the twin's glDepthFunc(GL_LESS) */
    ds.maxDepthBounds = 1.0f;

    /* NO COLOUR ATTACHMENT, so no blend state entries -- the subpass has none
       and an attachment count of 0 is what says so. */
    cb.attachmentCount = 0;

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
    gp.renderPass = s_rp;                   /* OURS, not the seam's */
    gp.subpass = 0;
    ok = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipe) == VK_SUCCESS;

done:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorPoolSize ps;
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSet sets[TAGPU_VK_SLOTS];
    VkPhysicalDeviceProperties pp;
    VkDeviceSize align;
    uint32_t i;

    vkGetPhysicalDeviceProperties(d->pd, &pp);
    align = pp.limits.minUniformBufferOffsetAlignment;
    if (align < 1) align = 1;
    s_ustride = (UBLK + align - 1) / align * align;
    if (!mk_buffer(d, s_ustride * d->slots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_ubuf, &s_umem, &s_umap)) return 0;

    memset(&ps, 0, sizeof ps);
    ps.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps.descriptorCount = d->slots;
    dpi.maxSets = d->slots;
    dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) return 0;
    for (i = 0; i < d->slots; i++) lay[i] = s_dsl;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = d->slots;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, sets) != VK_SUCCESS) return 0;
    for (i = 0; i < d->slots; i++) {
        VkDescriptorBufferInfo bi;
        VkWriteDescriptorSet w;
        s_slot[i].dset = sets[i];
        memset(&bi, 0, sizeof bi); memset(&w, 0, sizeof w);
        bi.buffer = s_ubuf; bi.offset = i * s_ustride; bi.range = UBLK;
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = sets[i]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(d->dev, 1, &w, 0, NULL);
    }
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{
    int linearOk = 0;
    /* THE SLOT COUNT IS THE SEAM'S AND IT IS RE-CHECKED HERE. It sizes two
       stack arrays in build_descriptors and indexes s_slot; the seam refuses a
       swapchain with more images than TAGPU_VK_SLOTS, and this is that bound
       stated where the arrays are. */
    if (d->slots < 1 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "shadow: the seam reports %u frame slots, outside 1..%d",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    if (!resolve(d)) { plog(d, "shadow: an entry point is missing"); return 0; }
    s_dfmt = tagpu_vk_shadow_format(d, &linearOk);
    if (s_dfmt == VK_FORMAT_UNDEFINED) {
        plog(d, "shadow: this device has no 24-bit depth format that is both a "
                "depth attachment and a sampled image - the map stays down (the "
                "GL twin's is GL_DEPTH_COMPONENT24 and a float one would not "
                "quantise the same way)");
        return 0;
    }
    if (!linearOk && !s_saidFormat) {
        s_saidFormat = 1;
        /* NOT A REFUSAL, AND SAID ANYWAY. The consumer's own compare sampler is
           what needs LINEAR (the twin's PCF is bilinear); this pass only writes
           the image. A consumer that finds it missing stands down itself, and
           knowing which of the two it was starts here. */
        plog(d, "shadow: the chosen depth format does not support LINEAR "
                "filtering - a consumer's bilinear PCF tap will have to stand "
                "down, though the map itself is fine");
    }
    if (!build_rp(d))          { plog(d, "shadow: the depth render pass was refused"); return 0; }
    if (!build_pipeline(d))    { plog(d, "shadow: the depth pipeline was refused"); return 0; }
    if (!build_descriptors(d)) { plog(d, "shadow: the uniform buffer or the descriptors were refused"); return 0; }
    plog(d, "shadow: up (depth format %d, linear %d, %u slots)",
         (int)s_dfmt, linearOk, (unsigned)d->slots);
    return 1;
}

/* ---- the caster mesh ----------------------------------------------------- */

/* Retire what is there and put buffers of `vbytes`/`ibytes` in their place.
   1 = there are buffers of that size; -1 = a retire is still outstanding, so
   the caller draws nothing this frame and tries again; 0 = THE DEVICE REFUSED,
   which is a different answer and has to be, or a refusal would read for ever
   as a retire that never clears. */
/* Hand the mesh to the retire and keep nothing. 0 when there was nothing to
   hand over or a retire is already outstanding, in which case the caller simply
   tries again next frame -- this is a give-back, never a correctness step. */
static int mesh_retire(const TAGPU_VKPASS* d)
{
    if (!s_mesh.vbuf && !s_mesh.ibuf) return 0;
    if (s_mesh.oldV || s_mesh.oldI) return 0;    /* one retire at a time */
    s_mesh.oldV = s_mesh.vbuf; s_mesh.oldVM = s_mesh.vmem;
    s_mesh.oldI = s_mesh.ibuf; s_mesh.oldIM = s_mesh.imem;
    s_mesh.vbuf = VK_NULL_HANDLE; s_mesh.vmem = VK_NULL_HANDLE;
    s_mesh.ibuf = VK_NULL_HANDLE; s_mesh.imem = VK_NULL_HANDLE;
    s_mesh.pending = d->slots >= 32 ? 0xFFFFFFFFu : ((1u << d->slots) - 1u);
    s_mesh.vbytes = s_mesh.ibytes = 0; s_mesh.serial = 0; s_mesh.have = 0;
    return 1;
}

static int mesh_resize(const TAGPU_VKPASS* d, VkDeviceSize vbytes, VkDeviceSize ibytes)
{
    if (s_mesh.vbuf && s_mesh.vbytes == vbytes && s_mesh.ibytes == ibytes) return 1;
    if (s_mesh.oldV || s_mesh.oldI) return -1;   /* one retire at a time */
    mesh_retire(d);
    s_mesh.vbytes = s_mesh.ibytes = 0; s_mesh.serial = 0; s_mesh.have = 0;
    if (!mk_buffer(d, vbytes,
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                   &s_mesh.vbuf, &s_mesh.vmem, NULL) ||
        !mk_buffer(d, ibytes,
                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                   &s_mesh.ibuf, &s_mesh.imem, NULL)) {
        /* mk_buffer can fail after vkCreateBuffer succeeded, so the half-made
           pair is given back here: "is there a buffer at all" is what tells a
           device refusal from a retire still clearing, and a partial one would
           read as the second for ever. */
        kill_buffer(d, &s_mesh.vbuf, &s_mesh.vmem);
        kill_buffer(d, &s_mesh.ibuf, &s_mesh.imem);
        return 0;
    }
    s_mesh.vbytes = vbytes; s_mesh.ibytes = ibytes;
    return 1;
}

/* THE RETIRE, and why it is a lifetime rather than a timer -- tagpu_vk_terr.c's
   argument, word for word, with "names it in a command buffer" where that file
   says "names it through a descriptor set". Called for every slot at the top of
   that slot's own `prepare`: the one instant the seam's fence proves the submit
   that last used this slot has COMPLETED. */
static void mesh_slot_done(const TAGPU_VKPASS* d, uint32_t slot)
{
    if (!s_mesh.oldV && !s_mesh.oldI) return;
    s_mesh.pending &= ~(1u << slot);
    if (s_mesh.pending == 0) {
        kill_buffer(d, &s_mesh.oldV, &s_mesh.oldVM);
        kill_buffer(d, &s_mesh.oldI, &s_mesh.oldIM);
    }
}

/* Room for `bytes` of staging in this slot, grown by doubling and never shrunk
   while the pass is drawing. */
static int slot_stage(const TAGPU_VKPASS* d, SLOT* s, VkDeviceSize bytes)
{
    VkDeviceSize want = 65536;
    if (s->stageCap >= bytes && s->stage) return 1;
    while (want < bytes) want *= 2;
    if (s->stageMap) { vkUnmapMemory(d->dev, s->stageMem); s->stageMap = NULL; }
    kill_buffer(d, &s->stage, &s->stageMem);
    s->stageCap = 0;
    if (!mk_buffer(d, want, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->stage, &s->stageMem, &s->stageMap)) return 0;
    s->stageCap = want;
    return 1;
}

/* The staging buffer is per slot and freed the moment the copy it fed has been
   recorded -- the mesh moves once per MAP, so keeping 19 MB of host-visible
   memory per slot for the rest of the session would be the largest thing this
   pass owns and the least used. */
static void slot_drop_stage(const TAGPU_VKPASS* d, SLOT* s)
{
    if (s->stageMap) { vkUnmapMemory(d->dev, s->stageMem); s->stageMap = NULL; }
    kill_buffer(d, &s->stage, &s->stageMem);
    s->stageCap = 0;
}

static void slot_free(const TAGPU_VKPASS* d, SLOT* s)
{
    slot_drop_stage(d, s);
    kill_target(d, s);
}

/* THE HAND-OVER, WHICH NOTHING PUBLISHES: no producer exists in this build,
   so this returns 0 on every frame. See `tagpu_vk_shadow.h`'s block on
   TAGPU_SHADOWHAND.

   IT IS A FUNCTION RATHER THAN A `return 0;` AT THE CALL SITE so that the one
   thing a reviver has to change is one body, and so that the refusal keeps its
   name in the code instead of becoming an unexplained early exit. */
static int shadow_handover(TAGPU_SHADOWHAND* out, unsigned now)
{
    (void)out; (void)now;
    return 0;
}

/* ---- the frame ---------------------------------------------------------- */

int tagpu_vk_shadow_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_SHADOWHAND h;
    SLOT* s;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue cv;
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize vbytes, ibytes, zero = 0;
    int mr, casters, ours, drew;

    /* THE MAP THIS FRAME WOULD SAMPLE IS NOT THIS FRAME'S UNTIL THE DRAW BELOW
       SUCCEEDS. Cleared first so that every exit path leaves the consumers'
       `tagpu_vk_shadow_ready` saying no rather than yes for an older frame. */
    s_liveHave = 0;
    s_res = 0;

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    s = &s_slot[slot];

    /* THE RETIRE'S ACCOUNTING, FIRST AND UNCONDITIONALLY -- the terrain pass's
       rule, and the reason it is that rule: every path below can return early,
       and the path that returns early is exactly the path that must still clear
       this slot's bit, or the retire stalls for ever. A map that goes away for
       a hundred frames must not hold 19 MB of dead buffers for all of them.
       Safe before anything is built: with no old buffers it returns at once,
       which is also the state in which its entry points are unresolved. */
    mesh_slot_done(d, slot);

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW, AND NOTHING IS KEPT
       ONCE THERE IS NOT. Giving this slot's target back here needs no new
       argument and no timer: it is the same instant, and the same ownership,
       that the rest of this function would write it in. */
    if (!shadow_handover(&h, d->frame)) {
        if (s_state == ST_READY) slot_free(d, s);
        return 0;
    }

    /* GL'S CLIP-SPACE Z OR NOTHING (item 2 of the file header). Refused before
       anything is built, because without it every pipeline this pass could make
       would clip half of every caster away. */
    if (!d->zclipok) {
        if (!s_saidZclip) {
            s_saidZclip = 1;
            plog(d, "shadow: this device has no "
                    "VK_EXT_depth_clip_control/depthClipControl, so a clip z "
                    "below 0 would be clipped where GL keeps it - the map is "
                    "not drawn, and the passes that sample it stand down");
        }
        return 0;
    }

    /* THE MAP IS INCOMPLETE ON THIS SIDE OF THE SEAM. `otherCasters` is
       tagpu_native.c's count of everything it drew into the GL map that the
       terrain hand-over carries no copy of: the native 3DO stream, the posed
       bodies, the replacement meshes -- and the heightfield itself when its
       mirror is missing.

       THE POSED BODIES ARE COVERED BY THE UNIT PASS, and this is where that
       is accounted for. `tagpu_vk_unit_casters` is the subset of
       that count this frame's unit pass is ready to draw into the map below,
       and it can only be SMALLER than the posed bodies' share of it -- the
       header says why, and an over-count is the safe direction: it refuses a
       frame the lane could have drawn, where an under-count would draw a
       different map from the oracle's.

       WHAT IS LEFT OVER IS THE REPLACEMENT MESHES, AND THE HEIGHTFIELD WHOSE
       MIRROR WENT MISSING. The native 3DO stream is not a third.
       `tagpu_shadow_unit`'s only call site is behind
       `firstv[i+1] == firstv[i]`, and `nv` is 0 for the whole of that loop
       because the posed program is the path -- so no ordinary unit has native
       vertices and that counter never moves.

       The unit pass's `upload` has already run for this slot -- the seam calls
       it before this function and says so -- which is what makes the number
       available before the render pass begins. */
    /* THE REPLACEMENT MESHES ARE THE SECOND TERM. Both passes
       count the same way -- the subset of the GL map's casters they are ready
       to draw this frame -- so the sum is comparable to `otherCasters` by the
       same construction, and both can only UNDER-count, which refuses a frame
       the lane could have drawn rather than drawing a map the oracle does not
       have. What is left over is the heightfield whose mirror went
       missing, which is an out-of-memory path and not a caster kind. */
    ours = tagpu_vk_unit_casters() + tagpu_vk_hires_casters();
    if (h.otherCasters - ours > 0) {
        if (!s_saidCasters) {
            s_saidCasters = 1;
            plog(d, "shadow: the GL map holds %d caster(s) this lane has no copy "
                    "of (%d of them the unit and hi-res passes carry). Nothing "
                    "drawn while there are, and the passes that sample the map "
                    "stand down with it", h.otherCasters, ours);
        }
        return 0;
    }
    s_saidCasters = 0;

    /* THE BOUNDS, RE-CHECKED IN THIS FILE'S OWN TERMS (see the block above). */
    if (h.res < RES_MIN || h.res > RES_MAX) {
        if (!s_saidRes) {
            s_saidRes = 1;
            plog(d, "shadow: the twin reports a %d-texel map, outside %d..%d - "
                    "nothing drawn", h.res, RES_MIN, RES_MAX);
        }
        return 0;
    }
    s_saidRes = 0;
    /* AN EMPTY MAP IS A MAP, AND IT IS REPRODUCIBLE EXACTLY. `terrainshadow`
       defaults to 0, so on a frame with no unit caster either the GL twin
       draws NOTHING into its depth texture and the clear at 1.0 IS the map --
       every receiver then finds no blocker and is lit. Refusing that frame
       would stand the consumers down over a map this lane can reproduce with a
       render pass and no draw call, which is the Classic++ default
       configuration. So `casters` decides whether anything is bound and drawn
       below, not whether the map is written. */
    casters = h.hv && h.hi && h.indexCount > 0;
    if (casters) {
        if (h.hnv < 1 || h.hnv > MESH_MAXV || h.hni < 1 || h.hni > MESH_MAXI) {
            plog(d, "shadow: the caster mesh reports %u vertices and %u indices, "
                    "outside 1..%u / 1..%u - nothing drawn",
                 (unsigned)h.hnv, (unsigned)h.hni, MESH_MAXV, MESH_MAXI);
            return 0;
        }
        /* AND THE RANGE INSIDE THE MESH, which is the bound the draw itself
           uses. The producer clamps it (tagpu_terr.c) and this re-checks it,
           for the same reason every other bound here is re-checked. */
        if ((VkDeviceSize)h.firstIndex + h.indexCount > h.hni) {
            plog(d, "shadow: the draw range %u+%u is outside a %u-index mesh - "
                    "nothing drawn", h.firstIndex, h.indexCount, (unsigned)h.hni);
            return 0;
        }
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_shadow_down(d); s_state = ST_REFUSED; return 0; }
        s_state = ST_READY;
    }

    /* this slot's target, rebuilt when the octave moved the resolution. Its own
       fence has been waited on, so the image it replaces is not in flight. */
    if (s->res != h.res || !s->fb) {
        kill_target(d, s);
        if (!mk_target(d, h.res, s)) {
            kill_target(d, s);
            plog(d, "shadow: the device refused a %dx%d depth target for slot %u "
                    "- the pass asks the seam to tear it down", h.res, h.res, slot);
            s_downOwed = 1;
            return 0;
        }
    }

    /* NOTHING IS KEPT ONCE THERE IS NOTHING TO DRAW -- the file header's own
       rule, applied to the empty-map path too. The caster mesh is the largest
       thing this pass owns (10 MB on Town & Country, 19 on Two Continents) and
       `terrainshadow` is a LIVE knob, so a map that drew hills and then stopped
       would otherwise hold the whole pair for the rest of the session. Through
       the retire rather than a destroy, because other slots' submitted command
       buffers may still name the buffers. */
    if (!casters) mesh_retire(d);
    vbytes = casters ? (VkDeviceSize)h.hnv * 3u * sizeof(float) : 0;
    ibytes = casters ? (VkDeviceSize)h.hni * sizeof(unsigned) : 0;
    mr = casters ? mesh_resize(d, vbytes, ibytes) : 1;
    if (mr < 0) return 0;                        /* a retire is clearing */
    if (!mr) {
        plog(d, "shadow: the device refused a %u-byte caster mesh - the pass "
                "asks the seam to tear it down", (unsigned)(vbytes + ibytes));
        s_downOwed = 1;
        return 0;
    }

    /* THE UPLOAD, ON THE SERIAL AND NOT PER FRAME. The mesh is built once per
       map; `hillsSerial` is bumped by the build that produced these bytes. */
    if (casters && (!s_mesh.have || s_mesh.serial != h.hillsSerial)) {
        VkBufferMemoryBarrier bb[2];
        VkBufferCopy cp[2];
        if (!slot_stage(d, s, vbytes + ibytes)) {
            plog(d, "shadow: the device refused a %u-byte staging buffer - the "
                    "pass asks the seam to tear it down",
                 (unsigned)(vbytes + ibytes));
            s_downOwed = 1;
            return 0;
        }
        memcpy(s->stageMap, h.hv, (size_t)vbytes);
        memcpy(s->stageMap + vbytes, h.hi, (size_t)ibytes);
        memset(cp, 0, sizeof cp);
        cp[0].srcOffset = 0;      cp[0].dstOffset = 0; cp[0].size = vbytes;
        cp[1].srcOffset = vbytes; cp[1].dstOffset = 0; cp[1].size = ibytes;
        /* THE WRITE-AFTER-READ BARRIER. These buffers are shared by every slot,
           so the frames still in flight may be reading them; a barrier's first
           synchronisation scope includes everything submitted to this queue
           before it, which is what makes the copy wait for those reads. */
        memset(bb, 0, sizeof bb);
        bb[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb[0].srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        bb[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb[0].srcQueueFamilyIndex = bb[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb[0].buffer = s_mesh.vbuf; bb[0].size = VK_WHOLE_SIZE;
        bb[1] = bb[0]; bb[1].buffer = s_mesh.ibuf;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 2, bb, 0, NULL);
        vkCmdCopyBuffer(cb, s->stage, s_mesh.vbuf, 1, &cp[0]);
        vkCmdCopyBuffer(cb, s->stage, s_mesh.ibuf, 1, &cp[1]);
        bb[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        bb[1].srcAccessMask = bb[0].srcAccessMask;
        bb[1].dstAccessMask = bb[0].dstAccessMask;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 2, bb, 0, NULL);
        s_mesh.serial = h.hillsSerial;
        s_mesh.have = 1;
        plog(d, "shadow: caster mesh uploaded, %u vertices / %u indices (%u KB), "
                "serial %u", (unsigned)h.hnv, (unsigned)h.hni,
             (unsigned)((vbytes + ibytes) / 1024), h.hillsSerial);
    } else {
        slot_drop_stage(d, s);
    }

    /* this slot's uniform block: the matrix, exactly as GL got it */
    memcpy(s_umap + slot * s_ustride, h.mat, sizeof h.mat);

    /* ---- the draw, in a render pass of our own ---- */
    cv.depthStencil.depth = 1.0f;           /* the twin's glClear value */
    cv.depthStencil.stencil = 0;
    rbi.renderPass = s_rp;
    rbi.framebuffer = s->fb;
    rbi.renderArea.extent.width = (uint32_t)h.res;
    rbi.renderArea.extent.height = (uint32_t)h.res;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    /* NO Y FLIP: a positive height, item 3 of the file header. minDepth 0 /
       maxDepth 1 with `negativeOneToOne` on is GL's own (z+1)/2. */
    memset(&vp, 0, sizeof vp);
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)h.res; vp.height = (float)h.res;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    memset(&sc, 0, sizeof sc);
    sc.extent.width = (uint32_t)h.res; sc.extent.height = (uint32_t)h.res;
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipe);
    /* the set is this slot's and its buffer range was written at build with
       this slot's offset already in it, so there is no dynamic offset here */
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plo, 0, 1,
                            &s->dset, 0, NULL);
    if (casters) {
        vkCmdBindVertexBuffers(cb, 0, 1, &s_mesh.vbuf, &zero);
        vkCmdBindIndexBuffer(cb, s_mesh.ibuf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, h.indexCount, 1, h.firstIndex, 0, 0);
    }

    /* THE POSED CASTERS, drawn by the unit pass into OUR render pass with our
       viewport and our scissor. It owns the geometry and the pose; this pass
       owns the target, the matrix and the clear.

       AND `ours` IS ALWAYS 0 FROM THE UNIT PASS, which makes the test below
       `0 - 0` rather than a comparison: `TAGPU_PDHAND.depthOn` has no
       producer, so no posed unit is ever a caster here (tagpu_posedraw.h).

       A SHORT COUNT IS A REFUSAL, not a partial map. `ours` was taken before
       the render pass began and is what the census above was reconciled
       against; if fewer arrive, the map is missing a caster the GL map has and
       nothing may sample it. The draws already recorded stay -- they are in a
       submitted command buffer either way -- but `s_liveHave` is not set, so
       every consumer stands down and the picture nobody draws is the one that
       would have been wrong. */
    drew = tagpu_vk_unit_cast(d, cb, slot, s_rp);
    {
        /* AND THE REPLACEMENT MESHES, into the same render pass, before it
           ends. A -1 from either is the same refusal, and it must not be
           allowed to cancel the other's count: summing a -1 into a positive
           would read as a short count and land on the same branch by accident
           rather than on purpose. */
        int hi = tagpu_vk_hires_cast(d, cb, slot, s_rp);
        if (drew < 0 || hi < 0) drew = -1;
        else drew += hi;
    }
    vkCmdEndRenderPass(cb);
    if (drew < 0 || drew != ours) {
        if (!s_saidUnitShort) {
            s_saidUnitShort = 1;
            plog(d, "shadow: the caster passes put %d of the %d caster(s) they "
                    "owed into the map - the map is incomplete and nothing "
                    "samples it this frame", drew, ours);
        }
        return 0;
    }
    s_saidUnitShort = 0;

    s_liveHave = 1;
    s_liveFrame = d->frame;
    s_res = h.res;
    return 1;
}

int tagpu_vk_shadow_ready(unsigned frame)
{
    return s_liveHave && s_liveFrame == frame;
}

/* THE FRAME IS PART OF THE QUESTION, not an argument about call order.
   `s_liveHave` is one flag for the whole pass rather than one per slot, and
   "the seam calls this pass's `prepare` first and every consumer asks with the
   current slot" is an enumeration of call sites, not a bound. Asking for the
   frame makes a stale view impossible to obtain: a caller out of step gets
   VK_NULL_HANDLE, binds its own dummy, and `tagpu_vk_shadow_ready` refuses it
   the draw anyway. */
VkImageView tagpu_vk_shadow_view(unsigned frame, uint32_t slot)
{
    if (!s_liveHave || s_liveFrame != frame || slot >= TAGPU_VK_SLOTS)
        return VK_NULL_HANDLE;
    return s_slot[slot].view;
}

int tagpu_vk_shadow_res(void) { return s_res; }

void tagpu_vk_shadow_down(const TAGPU_VKPASS* d)
{
    VkDevice dev = d->dev;
    uint32_t i;
    /* The paid/unpaid distinction, and why the debt is discharged by every
       teardown while the VERDICT is not: tagpu_vk_fx.c at length. */
    int owed = s_downPaying;
    s_downOwed = 0;
    s_liveHave = 0;
    s_res = 0;
    if (!dev || !vkDestroyBuffer) { s_state = owed ? ST_REFUSED : ST_UNBUILT; return; }

    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        slot_free(d, &s_slot[i]);
        s_slot[i].dset = VK_NULL_HANDLE;   /* goes back with the pool below */
    }
    kill_buffer(d, &s_mesh.vbuf, &s_mesh.vmem);
    kill_buffer(d, &s_mesh.ibuf, &s_mesh.imem);
    kill_buffer(d, &s_mesh.oldV, &s_mesh.oldVM);
    kill_buffer(d, &s_mesh.oldI, &s_mesh.oldIM);
    memset(&s_mesh, 0, sizeof s_mesh);
    if (s_pipe)  { vkDestroyPipeline(dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    if (s_plo)   { vkDestroyPipelineLayout(dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dpool) { vkDestroyDescriptorPool(dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
    if (s_rp)    { vkDestroyRenderPass(dev, s_rp, NULL); s_rp = VK_NULL_HANDLE; }
    if (s_umap)  { vkUnmapMemory(dev, s_umem); s_umap = NULL; }
    if (s_ubuf)  { vkDestroyBuffer(dev, s_ubuf, NULL); s_ubuf = VK_NULL_HANDLE; }
    if (s_umem)  { vkFreeMemory(dev, s_umem, NULL); s_umem = VK_NULL_HANDLE; }
    s_dfmt = VK_FORMAT_UNDEFINED;
    /* AND THE CACHED FORMAT GOES WITH THE DEVICE. `_down` is called when the
       lane comes down, and the lane can come back up on a DIFFERENT physical
       device (the menu's GPU row), whose answer to the same question need not
       be the same. A consumer asking again gets a fresh one. */
    s_fmtAsked = 0; s_fmtCache = VK_FORMAT_UNDEFINED; s_fmtLinear = 0;
    /* ST_UNBUILT and not ST_REFUSED: a pass brought down by a mode change or a
       cleared lever must be able to come back. The one exception is the
       teardown this pass asked for. */
    s_state = owed ? ST_REFUSED : ST_UNBUILT;
}

int tagpu_vk_shadow_down_owed(void)
{
    return s_downOwed;
}

void tagpu_vk_shadow_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_shadow_down(d);            /* clears s_downOwed itself */
    s_downPaying = 0;
}
