/* tagpu_vk_gui.c -- the GL UI layer's 1x mirror, drawn by Vulkan.
   Phase G / G19f, landing 1. The header states the contract; this file is the
   machinery. gpu-status.md 2.3e is the pass being ported.

   THE TWIN STORE IS ONE STORE AND THE FRAMES OVERLAP, which is the one
   synchronisation question this pass has that no world pass had. Every world
   pass so far keeps its mutable state PER SLOT, so the seam's fence wait on
   `fence[slot]` is the whole argument. A twin cannot be per slot: it is
   persistent state that an op stream mutates across frames, and 32 of them per
   slot is the memory this phase spends its budget measuring. So the twins are
   shared, and frame N writes them while frame N-1 may still be sampling them in
   its composite -- a write-after-read across submissions.

   IT IS CLOSED BY AN ORDERING, not by a fence count and not by hoping the
   earlier frame has finished. `vkCmdPipelineBarrier`'s first synchronisation
   scope includes every command submitted PREVIOUSLY TO THE SAME QUEUE, so one
   barrier at the top of the replay -- FRAGMENT_SHADER/SHADER_READ before
   COLOR_ATTACHMENT_OUTPUT/COLOR_ATTACHMENT_WRITE -- orders this frame's twin
   writes after every earlier frame's composite reads. One barrier a frame, and
   it is a fact about the queue rather than a claim about timing. */

#include "tagpu_vk_pass.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tagpu_vk_gui.h"
#include "tagpu_gui.h"
#include "spirv/tagpu_gui_surf.spv.h"

/* the std140 blocks, at the sizes the generated SPIR-V header prints */
#define QVS_SZ  16                  /* QVS    binding 0  : vec2 uSize        */
#define TWF_SZ  16                  /* CPY/SPR binding 32: 16 bytes          */
#define LAY_SZ  128                 /* LAY_FS binding 32 : 128 bytes         */

/* MAX_TWINS in tagpu_gui_surf.c. A bound here rather than a number taken from
   the hand-over: the scaffold's rule -- a handed-over count never sizes an
   allocation. */
#define TW_MAX      32
/* sprite + copy draws in one frame. The first draft said 4096 because "a
   steady screen is ~41 ops of which a handful draw" -- which is true of a
   FLIP and not of a PUBLISH: the publisher batches every flip since its last
   one (the shell flips ~12 000 times a second at a 5 ms cadence), so one
   hand-over routinely carries thousands. MEASURED 2026-09-16 at 7414 on a
   1080p level load. This is the bound on a runaway, not on a busy frame. */
#define DRAW_MAX    16384
/* AND THE QUADS, WHICH STOPPED BEING THE DRAWS IN LANDING 2. A string is ONE
   draw and up to 256 quads, so `DRAW_MAX` alone bounds a runaway at 4.2 million
   quads -- a 402 MB host-visible `grow()` per slot, which `grow` never shrinks
   again. Landing 1's worst measured frame was 7414 quads. */
#define QUAD_MAX    65536
/* one descriptor set per distinct IMAGE a frame's twin draws sample. There are
   three kinds and only three: every sprite samples the ONE UI atlas, every
   string the ONE glyph atlas, and a copy samples its SOURCE TWIN -- so the sets
   a frame needs cannot exceed the twins plus two, whatever the op count.
   IT WAS `TW_MAX + 1` UNTIL LANDING 2 AND THAT WAS THE STRING OP'S DOING: the
   glyph atlas is the SECOND non-twin view, and the old bound's own comment
   ("they all sample the one atlas") stopped being true the moment there were
   two. A frame with all TW_MAX twins used as copy sources plus one sprite plus
   one string wanted 34 of 33 and fell into `standdown` -- recoverable, because
   `behind` catches the store up, but a store dropped every frame is not
   parity. Bound it instead of arguing it is unreachable. */
#define SET_MAX     (TW_MAX + 2)
/* objects waiting for every slot to turn over once before they are destroyed.
   THE LANE ALREADY HAD THE ANSWER AND THIS PASS DID NOT USE IT: the seam waits
   `fence[slot]` and nothing more, so `slots - 1` earlier submissions are still
   executing -- and the command buffer being recorded right now already names
   the objects too. Destroying a twin inside `prepare` is a use-after-free on
   the FIRST eviction, not a rare one. tagpu_vk_terr.c's slot bitmask and
   tagpu_vk_unit.c's `ret_push` are the same mechanism.
   [FOUND 2026-09-16 -- BOTH landing reviewers led with it, independently.] */
#define RET_MAX 128
#define ATLAS_MAXDIM 8192
#define SURF_MAXDIM  8192

/* ---- the entry points, this pass's own (tagpu_vk_pass.h) ---------------- */
#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties)

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
    X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) \
    X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };

typedef struct {
    unsigned        surf;           /* the engine surface's pixel base       */
    int             w, h;
    VkImage         img;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkFramebuffer   fb;
    VkImageLayout   layout;         /* what it is in RIGHT NOW               */
    int             needClear;      /* created this frame, not yet cleared   */
} TWIN;

typedef struct {
    VkBuffer        stage;          /* one staging buffer for every upload   */
    VkDeviceMemory  stMem;
    unsigned char*  stMap;
    VkDeviceSize    stCap;

    VkBuffer        vb;             /* the quads, 6 vertices x 4 floats each */
    VkDeviceMemory  vbMem;
    unsigned char*  vbMap;
    VkDeviceSize    vbCap;

    VkBuffer        ub;             /* QVS's block, one window per draw      */
    VkDeviceMemory  ubMem;
    unsigned char*  ubMap;
    VkDeviceSize    ubCap;

    VkBuffer        fb_;            /* CPY/SPR's block, one window per draw  */
    VkDeviceMemory  fbMem;
    unsigned char*  fbMap;
    VkDeviceSize    fbCap;

    VkBuffer        lb;             /* LAY_FS's block, one per frame         */
    VkDeviceMemory  lbMem;
    unsigned char*  lbMap;

    VkDescriptorSet sets[SET_MAX];  /* the twin draws'                       */
    VkDescriptorSet laySet;
    int             built;
} SLOT;

static int              s_state, s_downOwed, s_downPaying;
static int              s_drawThis, s_abFrame;
static int              s_saidColour, s_saidSharp, s_saidRoom, s_saidEng;

static TWIN             s_tw[TW_MAX];
static int              s_ntw;

static VkRenderPass     s_twRp;          /* one R8G8 colour attachment, LOAD  */
static VkDescriptorSetLayout s_dslTwin, s_dslLay;
static VkPipelineLayout s_ploTwin, s_ploLay;
static VkPipeline       s_pipeSpr, s_pipeCpy, s_pipeStr, s_pipeLay;
static VkRenderPass     s_layRp;         /* what s_pipeLay was built against  */
static VkDescriptorPool s_dpool;
static VkSampler        s_samp;
static VkDeviceSize     s_ualign;

/* the shared texels: one of each, not one per slot (2.28's cheaper design --
   they are re-uploaded whole when their serial moves and read by every slot) */
static VkImage          s_atImg, s_palImg, s_engImg, s_dumImg, s_glImg;
static VkDeviceMemory   s_atMem, s_palMem, s_engMem, s_dumMem, s_glMem;
static VkImageView      s_atView, s_palView, s_engView, s_dumView, s_glView;
static int              s_atDim, s_engW, s_engH, s_glW, s_glH;
static unsigned         s_atSerial, s_palSerial, s_glSerial;
static int              s_atHave, s_palHave, s_engHave, s_dumReady, s_glHave;

static SLOT             s_slot[TAGPU_VK_SLOTS];

typedef struct {
    VkImage         img;
    VkDeviceMemory  mem;
    VkImageView     view;
    VkFramebuffer   fb;
    unsigned        pending;        /* bit per slot still to turn over       */
} RET;
static RET              s_ret[RET_MAX];

/* THE STORE MAY BE BEHIND THE GL LANE'S, and that is a state rather than an
   accident. Any path that cannot apply a frame's ops sets it: the twins are
   dropped, the producer is asked for a fresh start, and nothing is composited
   until its RESET arrives. It is `tagpu_gui_surf.c`'s own `s_skipToReset`, for
   the same reason -- applying ops to a store that has missed some is a wrong
   picture that every later frame inherits. */
static int              s_behind, s_cantReplay;
/* RESEEDS RAISED THIS SESSION. `behind` asks once on the transition, but the
   transition REPEATS: a RESET clears `s_behind`, and a condition that is
   structural rather than transient fires again inside the same frame that
   answered it -- so "once on the transition" is once PER FRAME for exactly the
   conditions that never go away. That is the reseed storm the re-review found,
   re-entering through a different door, and it changes the ORACLE. After this
   many fruitless asks the pass stops asking: it keeps its store dropped and
   composites nothing, which is a capability statement and costs the GL lane
   nothing. [FOUND 2026-09-16, the landing-2 review.] */
#define BEHIND_ASKS_MAX 8
/* ...and the cap counts FRUITLESS asks, not asks. A map change is a legitimate
   reason to fall behind once, and a long session can hold several; muting the
   pass for good after eight of those would be the cure killing the patient.
   A run of frames that actually composited is the evidence the last ask WORKED,
   so it gives the budget back. A structural condition never earns that run,
   because it stands the composite down on every frame. */
#define BEHIND_GOOD_RUN 120
static int              s_behindAsks, s_behindMute, s_goodRun;
static VkImageView      s_setView[SET_MAX];

/* this frame's plan, filled by `prepare` and read by `record` */
static unsigned         s_presented;
static int              s_surfW, s_surfH;
static VkDeviceSize     s_uStride, s_fStride;
static VkDeviceSize     s_layQuad;       /* the composite's quad in the vb    */
static int              s_layVp[4];      /* the GL viewport it ran in         */

static void plog(const TAGPU_VKPASS* d, const char* fmt, ...)
{
    char b[320];
    va_list ap;
    if (!d->log) return;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    d->log(b);
}

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a)
{
    return a ? ((v + a - 1) / a) * a : v;
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
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int t;
    void* p = NULL;
    if (!size) return 0;
    bi.size = size; bi.usage = use; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bi, NULL, buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(d->dev, *buf, &mr);
    t = mem_type(d, mr.memoryTypeBits, want);
    if (t < 0) { vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0; }
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    if (vkAllocateMemory(d->dev, &ai, NULL, mem) != VK_SUCCESS) {
        vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0; }
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) goto fail;
    if (map) {
        if (vkMapMemory(d->dev, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) goto fail;
        *map = (unsigned char*)p;
    }
    return 1;
fail:
    vkFreeMemory(d->dev, *mem, NULL); *mem = VK_NULL_HANDLE;
    vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE;
    return 0;
}

static void kill_buffer(const TAGPU_VKPASS* d, VkBuffer* buf, VkDeviceMemory* mem,
                        unsigned char** map)
{
    if (map && *map) { vkUnmapMemory(d->dev, *mem); *map = NULL; }
    if (*buf) { vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; }
    if (*mem) { vkFreeMemory(d->dev, *mem, NULL); *mem = VK_NULL_HANDLE; }
}

static int mk_image(const TAGPU_VKPASS* d, int w, int h, VkFormat fmt,
                    VkImageUsageFlags use, VkImage* img, VkDeviceMemory* mem,
                    VkImageView* view)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    int t;
    if (w < 1 || h < 1) return 0;
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent.width = (uint32_t)w; ii.extent.height = (uint32_t)h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = use; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ii, NULL, img) != VK_SUCCESS) return 0;
    vkGetImageMemoryRequirements(d->dev, *img, &mr);
    t = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) { vkDestroyImage(d->dev, *img, NULL); *img = VK_NULL_HANDLE; return 0; }
    ai.allocationSize = mr.size; ai.memoryTypeIndex = (uint32_t)t;
    if (vkAllocateMemory(d->dev, &ai, NULL, mem) != VK_SUCCESS) {
        vkDestroyImage(d->dev, *img, NULL); *img = VK_NULL_HANDLE; return 0; }
    if (vkBindImageMemory(d->dev, *img, *mem, 0) != VK_SUCCESS) goto fail;
    vi.image = *img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &vi, NULL, view) != VK_SUCCESS) goto fail;
    return 1;
fail:
    vkFreeMemory(d->dev, *mem, NULL); *mem = VK_NULL_HANDLE;
    vkDestroyImage(d->dev, *img, NULL); *img = VK_NULL_HANDLE;
    return 0;
}

static void kill_image(const TAGPU_VKPASS* d, VkImage* img, VkDeviceMemory* mem,
                       VkImageView* view)
{
    if (*view) { vkDestroyImageView(d->dev, *view, NULL); *view = VK_NULL_HANDLE; }
    if (*img)  { vkDestroyImage(d->dev, *img, NULL);  *img = VK_NULL_HANDLE; }
    if (*mem)  { vkFreeMemory(d->dev, *mem, NULL);   *mem = VK_NULL_HANDLE; }
}

static VkShaderModule mk_module(const TAGPU_VKPASS* d, const uint32_t* w, size_t words)
{
    VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    si.codeSize = words * 4; si.pCode = w;
    if (vkCreateShaderModule(d->dev, &si, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

static int resolve(const TAGPU_VKPASS* d)
{
#define GETI(n) n = (PFN_##n)d->gipa(d->inst, #n); if (!n) return 0;
#define GETD(n) n = (PFN_##n)d->gdpa(d->dev, #n);  if (!n) return 0;
    IFNS(GETI)
    DFNS(GETD)
#undef GETI
#undef GETD
    return 1;
}

static void img_barrier(VkCommandBuffer cb, VkImage img,
                        VkImageLayout from, VkImageLayout to,
                        VkPipelineStageFlags srcS, VkAccessFlags srcA,
                        VkPipelineStageFlags dstS, VkAccessFlags dstA)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, NULL, 0, NULL, 1, &b);
}

/* move a twin into `to`, from whatever it is in now */
static void tw_to(VkCommandBuffer cb, TWIN* t, VkImageLayout to)
{
    VkPipelineStageFlags ss, ds;
    VkAccessFlags sa, da;
    if (t->layout == to) return;
    switch (t->layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        ss = VK_PIPELINE_STAGE_TRANSFER_BIT; sa = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        ss = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        sa = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        ss = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; sa = VK_ACCESS_SHADER_READ_BIT; break;
    default:
        ss = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; sa = 0; break;
    }
    switch (to) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        ds = VK_PIPELINE_STAGE_TRANSFER_BIT; da = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        ds = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        da = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT; break;
    default:
        ds = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; da = VK_ACCESS_SHADER_READ_BIT; break;
    }
    img_barrier(cb, t->img, t->layout, to, ss, sa, ds, da);
    t->layout = to;
}

/* ---- the retire --------------------------------------------------------- */

/* this slot has turned over: whatever was waiting on it is one step closer */
static void ret_slot_done(const TAGPU_VKPASS* d, uint32_t slot)
{
    int i;
    for (i = 0; i < RET_MAX; i++) {
        if (!s_ret[i].pending) continue;
        s_ret[i].pending &= ~(1u << slot);
        if (s_ret[i].pending) continue;
        if (s_ret[i].fb)   vkDestroyFramebuffer(d->dev, s_ret[i].fb, NULL);
        if (s_ret[i].view) vkDestroyImageView(d->dev, s_ret[i].view, NULL);
        if (s_ret[i].img)  vkDestroyImage(d->dev, s_ret[i].img, NULL);
        if (s_ret[i].mem)  vkFreeMemory(d->dev, s_ret[i].mem, NULL);
        memset(&s_ret[i], 0, sizeof s_ret[i]);
    }
}

/* Hand an object set to the retire, or say the list is full. EVERY slot's bit
   is set, the one being recorded included: that slot's submit is the one most
   certainly naming these handles, and its bit clears when it comes round
   again -- which is after its fence. A full list is not a reason to destroy
   anything here; it is a reason to stop. */
static int ret_push(const TAGPU_VKPASS* d, VkImage img, VkDeviceMemory mem,
                    VkImageView view, VkFramebuffer fb)
{
    int i;
    if (!img && !mem && !view && !fb) return 1;
    for (i = 0; i < RET_MAX; i++) {
        if (s_ret[i].pending) continue;
        s_ret[i].img = img; s_ret[i].mem = mem;
        s_ret[i].view = view; s_ret[i].fb = fb;
        s_ret[i].pending = (d->slots >= 32) ? 0xFFFFFFFFu
                                            : ((1u << d->slots) - 1u);
        return 1;
    }
    return 0;
}

/* only ever called behind the seam's vkDeviceWaitIdle */
static void ret_drain_idle(const TAGPU_VKPASS* d)
{
    int i;
    for (i = 0; i < RET_MAX; i++) {
        if (s_ret[i].fb)   vkDestroyFramebuffer(d->dev, s_ret[i].fb, NULL);
        if (s_ret[i].view) vkDestroyImageView(d->dev, s_ret[i].view, NULL);
        if (s_ret[i].img)  vkDestroyImage(d->dev, s_ret[i].img, NULL);
        if (s_ret[i].mem)  vkFreeMemory(d->dev, s_ret[i].mem, NULL);
        memset(&s_ret[i], 0, sizeof s_ret[i]);
    }
}

/* ---- the twin store ----------------------------------------------------- */

static TWIN* tw_find(unsigned surf)
{
    int i;
    if (!surf) return NULL;
    for (i = 0; i < s_ntw; i++) if (s_tw[i].surf == surf) return &s_tw[i];
    return NULL;
}

/* 0 when the retire would not take it, which is the one case a caller may not
   shrug off: dropping the entry anyway leaks, destroying it here is the
   use-after-free this list exists to prevent. */
static int tw_drop(const TAGPU_VKPASS* d, TWIN* t)
{
    if (!ret_push(d, t->img, t->mem, t->view, t->fb)) return 0;
    /* THE CLAIM IS DELIBERATELY LEFT STANDING. An earlier version cleared
       `s_setView` here, to stop a recycled view handle matching a stale claim —
       but now that `tw_drop` only RETIRES, the view stays alive for the whole
       frame and no handle can be recycled inside it, while clearing the entry
       frees the set index for `set_claim` to hand out again and REWRITE a set
       a draw recorded earlier this frame already names. The fix for one hazard
       was the cause of a worse one. The per-frame memset is what bounds this.
       [FOUND 2026-09-16, the re-review.] */
    *t = s_tw[--s_ntw];
    memset(&s_tw[s_ntw], 0, sizeof s_tw[s_ntw]);
    return 1;
}

static int tw_reset(const TAGPU_VKPASS* d)
{
    while (s_ntw) if (!tw_drop(d, &s_tw[0])) return 0;
    return 1;
}

static TWIN* tw_make(const TAGPU_VKPASS* d, unsigned surf, int w, int h)
{
    TWIN* t = tw_find(surf);
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    if (t) {
        if (t->w == w && t->h == h) return t;
        if (!tw_drop(d, t)) return NULL;     /* a resized surface is a new one */
    }
    if (w < 1 || h < 1 || w > SURF_MAXDIM || h > SURF_MAXDIM) return NULL;
    /* THE OLDEST GOES, which is what tagpu_gui_surf.c's twin_make does. The
       store is bounded by construction on both sides, so the two cannot
       disagree about which surfaces have a twin. */
    if (s_ntw >= TW_MAX && !tw_drop(d, &s_tw[0])) return NULL;
    t = &s_tw[s_ntw];
    memset(t, 0, sizeof *t);
    if (!mk_image(d, w, h, VK_FORMAT_R8G8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                  &t->img, &t->mem, &t->view))
        return NULL;
    fi.renderPass = s_twRp; fi.attachmentCount = 1; fi.pAttachments = &t->view;
    fi.width = (uint32_t)w; fi.height = (uint32_t)h; fi.layers = 1;
    if (vkCreateFramebuffer(d->dev, &fi, NULL, &t->fb) != VK_SUCCESS) {
        kill_image(d, &t->img, &t->mem, &t->view);
        return NULL;
    }
    t->surf = surf; t->w = w; t->h = h;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    /* A FRESH TWIN IS CLEARED, because GL's `twin_make` clears one
       (`glClearColor(0,0,0,0); glClear`) and a texel neither lane's seed
       covers must read the same in both. Ours would otherwise load whatever
       device memory it was handed, through a LOAD_OP_LOAD render pass, and
       keep it. Cheap, and it removes the question rather than resting on "the
       producer always sends the bytes". [FOUND 2026-09-16, the landing
       review -- the two reviewers disagreed about whether a seed can arrive
       with no bytes, which is itself the reason not to depend on it.] */
    t->needClear = 1;
    s_ntw++;
    return t;
}

/* A TWIN CREATED THIS FRAME IS CLEARED BEFORE ANYTHING READS OR LOADS IT.
   GL's `twin_make` clears; ours must, or the LOAD_OP_LOAD render pass loads
   whatever memory the allocator handed us and keeps it in the twin. Done at
   the first touch rather than at creation, because creation happens outside a
   command buffer. */
static void tw_fresh(VkCommandBuffer cb, TWIN* t)
{
    VkClearColorValue cv;
    VkImageSubresourceRange rg;
    if (!t->needClear) return;
    t->needClear = 0;
    memset(&cv, 0, sizeof cv);
    memset(&rg, 0, sizeof rg);
    rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.levelCount = 1; rg.layerCount = 1;
    tw_to(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &cv, 1, &rg);
}

/* ---- build -------------------------------------------------------------- */

static int build_rp(const TAGPU_VKPASS* d)
{
    VkAttachmentDescription a;
    VkAttachmentReference ar;
    VkSubpassDescription sp;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    memset(&a, 0, sizeof a); memset(&sp, 0, sizeof sp); memset(dep, 0, sizeof dep);
    a.format = VK_FORMAT_R8G8_UNORM;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    /* LOAD, NOT CLEAR: a twin accumulates. Every op writes part of it and the
       rest has to survive -- which is the whole of what a twin IS. */
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ar.attachment = 0; ar.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    /* the copy reads ANOTHER twin in the fragment stage while this one is the
       target, so the external dependencies name both scopes */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_SHADER_READ_BIT;
    dep[1] = dep[0];
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                           VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    ri.attachmentCount = 1; ri.pAttachments = &a;
    ri.subpassCount = 1; ri.pSubpasses = &sp;
    ri.dependencyCount = 2; ri.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &ri, NULL, &s_twRp) == VK_SUCCESS;
}

static int build_layouts(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[6];
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    int i;

    /* the twin draws: QVS binding 0, CPY/SPR binding 32, samplers 40 and 41 */
    memset(b, 0, sizeof b);
    b[0].binding = 0;  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 32; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].binding = 40; b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[3] = b[2]; b[3].binding = 41;
    li.bindingCount = 4; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &s_dslTwin) != VK_SUCCESS) return 0;

    /* the composite: LAY_FS binding 32, samplers 40..44 */
    memset(b, 0, sizeof b);
    b[0].binding = 32; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 0; i < 5; i++) {
        b[1 + i].binding = (uint32_t)(40 + i);
        b[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1 + i].descriptorCount = 1;
        b[1 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.bindingCount = 6; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &s_dslLay) != VK_SUCCESS) return 0;

    pi.setLayoutCount = 1; pi.pSetLayouts = &s_dslTwin;
    if (vkCreatePipelineLayout(d->dev, &pi, NULL, &s_ploTwin) != VK_SUCCESS) return 0;
    pi.pSetLayouts = &s_dslLay;
    if (vkCreatePipelineLayout(d->dev, &pi, NULL, &s_ploLay) != VK_SUCCESS) return 0;
    return 1;
}

/* every quad is one vec4 attribute: x, y, u, v -- the GL lane's own vertex */
static void quad_layout(VkVertexInputBindingDescription* vb,
                        VkVertexInputAttributeDescription* at)
{
    vb->binding = 0; vb->stride = 4 * sizeof(float);
    vb->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    at->location = 0; at->binding = 0;
    at->format = VK_FORMAT_R32G32B32A32_SFLOAT; at->offset = 0;
}

/* the twin pipelines. NO FLIP: the target is SAMPLED, not presented, and
   QVS puts quad y = 0 at attachment row 0 under both APIs (the header). */
static int build_twin_pipe(const TAGPU_VKPASS* d, const uint32_t* fs, size_t fsw,
                           VkPipeline* out)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription at;
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule vm, fm;
    VkResult r;

    vm = mk_module(d, tagpu_spv_tagpu_gui_surf_QVS,
                   sizeof tagpu_spv_tagpu_gui_surf_QVS / 4);
    fm = mk_module(d, fs, fsw);
    if (!vm || !fm) {
        if (vm) vkDestroyShaderModule(d->dev, vm, NULL);
        if (fm) vkDestroyShaderModule(d->dev, fm, NULL);
        return 0;
    }
    memset(st, 0, sizeof st); memset(&cba, 0, sizeof cba);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vm; st[0].pName = "main";
    st[1] = st[0]; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fm;
    quad_layout(&vb, &at);
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &at;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* the GL lane draws these with blending OFF: a sprite discards its keyed
       texels and writes the rest whole, which is what coverage means here */
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds; gp.layout = s_ploTwin;
    gp.renderPass = s_twRp; gp.subpass = 0;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, out);
    vkDestroyShaderModule(d->dev, vm, NULL);
    vkDestroyShaderModule(d->dev, fm, NULL);
    return r == VK_SUCCESS;
}

/* the composite. THIS one flips, because this one is presented. */
static int build_lay_pipe(const TAGPU_VKPASS* d, VkRenderPass rp)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription at;
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule vm, fm;
    VkResult r;

    vm = mk_module(d, tagpu_spv_tagpu_gui_surf_LAY_VS,
                   sizeof tagpu_spv_tagpu_gui_surf_LAY_VS / 4);
    fm = mk_module(d, tagpu_spv_tagpu_gui_surf_LAY_FS,
                   sizeof tagpu_spv_tagpu_gui_surf_LAY_FS / 4);
    if (!vm || !fm) {
        if (vm) vkDestroyShaderModule(d->dev, vm, NULL);
        if (fm) vkDestroyShaderModule(d->dev, fm, NULL);
        return 0;
    }
    memset(st, 0, sizeof st); memset(&cba, 0, sizeof cba);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vm; st[0].pName = "main";
    st[1] = st[0]; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fm;
    quad_layout(&vb, &at);
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &at;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* the GL lane composites with BLEND and DEPTH off; the seam's render pass
       has a depth attachment, so a state must still be declared for it
       (tagpu_vk_pass.h) -- testing and writing both off. */
    dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE;
    dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.pDepthStencilState = (d->dfmt != VK_FORMAT_UNDEFINED) ? &dss : NULL;
    gp.pDynamicState = &ds; gp.layout = s_ploLay;
    gp.renderPass = rp; gp.subpass = 0;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, &s_pipeLay);
    vkDestroyShaderModule(d->dev, vm, NULL);
    vkDestroyShaderModule(d->dev, fm, NULL);
    return r == VK_SUCCESS;
}

static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorPoolSize ps[3];
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    uint32_t nslots = d->slots;
    uint32_t i;
    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ps[0].descriptorCount = nslots * SET_MAX * 2;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = nslots * (SET_MAX * 2 + 5);
    ps[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[2].descriptorCount = nslots;
    pi.maxSets = nslots * (SET_MAX + 1);
    pi.poolSizeCount = 3; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &pi, NULL, &s_dpool) != VK_SUCCESS) return 0;
    for (i = 0; i < nslots && i < TAGPU_VK_SLOTS; i++) {
        VkDescriptorSetLayout ls[SET_MAX];
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        int k;
        for (k = 0; k < SET_MAX; k++) ls[k] = s_dslTwin;
        ai.descriptorPool = s_dpool; ai.descriptorSetCount = SET_MAX;
        ai.pSetLayouts = ls;
        if (vkAllocateDescriptorSets(d->dev, &ai, s_slot[i].sets) != VK_SUCCESS) return 0;
        ai.descriptorSetCount = 1; ai.pSetLayouts = &s_dslLay;
        if (vkAllocateDescriptorSets(d->dev, &ai, &s_slot[i].laySet) != VK_SUCCESS) return 0;
    }
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkPhysicalDeviceProperties pr;
    if (!resolve(d)) return 0;
    if (!d->flipok) return 0;          /* the composite needs it */
    /* THE DEVICE'S OWN NUMBER, not 256 assumed. The unit pass's note records
       the reference device answering 64, and a stride quoted from the source
       rather than the device was how its own first draft got the figure
       wrong (gpu-status 2.33). */
    vkGetPhysicalDeviceProperties(d->pd, &pr);
    s_ualign = pr.limits.minUniformBufferOffsetAlignment;
    if (!s_ualign) s_ualign = 4;
    /* EVERY TEXTURE THIS MODULE SAMPLES IS GL_NEAREST in the twin, and the
       ramp's own taps are texelFetch -- so one nearest sampler serves all of
       them and a linear one would be a different picture. */
    si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f; si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(d->dev, &si, NULL, &s_samp) != VK_SUCCESS) return 0;
    if (!build_rp(d)) return 0;
    if (!build_layouts(d)) return 0;
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_SPR_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_SPR_FS / 4, &s_pipeSpr)) return 0;
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_CPY_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_CPY_FS / 4, &s_pipeCpy)) return 0;
    /* the string shares the twin pipelines' layout exactly: QVS at binding 0,
       its own block at 32, one sampler at 40 */
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_STR_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_STR_FS / 4, &s_pipeStr)) return 0;
    if (!build_descriptors(d)) return 0;
    return 1;
}

/* ---- per-slot buffers --------------------------------------------------- */

static int grow(const TAGPU_VKPASS* d, VkBuffer* b, VkDeviceMemory* m,
                unsigned char** map, VkDeviceSize* cap, VkDeviceSize need,
                VkBufferUsageFlags use)
{
    if (*cap >= need && *b) return 1;
    kill_buffer(d, b, m, map);
    *cap = 0;
    if (!mk_buffer(d, need, use,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   b, m, map)) return 0;
    *cap = need;
    return 1;
}

static void slot_free(const TAGPU_VKPASS* d, SLOT* s)
{
    kill_buffer(d, &s->stage, &s->stMem, &s->stMap);  s->stCap = 0;
    kill_buffer(d, &s->vb,    &s->vbMem, &s->vbMap);  s->vbCap = 0;
    kill_buffer(d, &s->ub,    &s->ubMem, &s->ubMap);  s->ubCap = 0;
    kill_buffer(d, &s->fb_,   &s->fbMem, &s->fbMap);  s->fbCap = 0;
    kill_buffer(d, &s->lb,    &s->lbMem, &s->lbMap);
    s->built = 0;
}

/* ---- the shared texels -------------------------------------------------- */

static void copy_rect(VkCommandBuffer cb, VkBuffer src, VkDeviceSize off,
                      VkImage dst, int x, int y, int w, int h)
{
    VkBufferImageCopy rg;
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = off;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.layerCount = 1;
    rg.imageOffset.x = x; rg.imageOffset.y = y;
    rg.imageExtent.width = (uint32_t)w; rg.imageExtent.height = (uint32_t)h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, src, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
}

/* the 1x1 stand-ins every unused sampler binding names. A descriptor must be
   VALID whether or not the shader reads it -- landing 1 never reads the colour
   twin or the sharp layer, and both still need an image. */
static int dummies(const TAGPU_VKPASS* d, VkCommandBuffer cb)
{
    if (s_dumReady) return 1;
    if (!s_dumImg &&
        !mk_image(d, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  &s_dumImg, &s_dumMem, &s_dumView)) return 0;
    img_barrier(cb, s_dumImg, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    s_dumReady = 1;
    return 1;
}

/* ---- the frame ---------------------------------------------------------- */

static void quadv(float* v, float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1)
{
    float q[24] = { x0,y0,u0,v0,  x1,y0,u1,v0,  x0,y1,u0,v1,
                    x0,y1,u0,v1,  x1,y0,u1,v0,  x1,y1,u1,v1 };
    memcpy(v, q, sizeof q);
}

static void set_scissor(VkCommandBuffer cb, int x, int y, int w, int h,
                        int lim_w, int lim_h)
{
    VkRect2D sc;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (x > lim_w) x = lim_w;
    if (y > lim_h) y = lim_h;
    if (x + w > lim_w) w = lim_w - x;
    if (y + h > lim_h) h = lim_h - y;
    sc.offset.x = x; sc.offset.y = y;
    sc.extent.width = (uint32_t)w; sc.extent.height = (uint32_t)h;
    vkCmdSetScissor(cb, 0, 1, &sc);
}

static void set_viewport(VkCommandBuffer cb, int w, int h)
{
    VkViewport vp;
    /* NO FLIP. The target is sampled, not presented, and QVS puts quad y = 0
       at attachment row 0 under both APIs -- the file header argues it. */
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)w; vp.height = (float)h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
}

/* (s_setView is declared with the pass's state, above: tw_drop clears a claim
   whose view it is retiring.)
   A TWIN-DRAW SET, CLAIMED FOR ONE IMAGE FOR THE LENGTH OF ONE FRAME.
   A set may not be rewritten once a recorded draw names it, so each distinct
   image a frame samples needs its own -- and the twin ARRAY cannot be the
   index, because `tw_drop` moves the last entry into the hole and the indices
   shuffle under it. So the claim is on the VIEW: reuse the set already holding
   it, else take a free one. SET_MAX is the twins plus TWO -- the UI atlas and
   the glyph atlas -- so a frame can never want more than there are. */
static int set_claim(const TAGPU_VKPASS* d, SLOT* s, VkImageView v,
                     VkDeviceSize uRange, VkDeviceSize fRange, int* out)
{
    int j, free_j = -1;
    VkDescriptorBufferInfo bi[2];
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet wr[4];
    for (j = 0; j < SET_MAX; j++) {
        if (s_setView[j] == v) { *out = j; return 1; }
        if (!s_setView[j] && free_j < 0) free_j = j;
    }
    if (free_j < 0) return 0;
    memset(bi, 0, sizeof bi); memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
    bi[0].buffer = s->ub;  bi[0].offset = 0; bi[0].range = uRange;
    bi[1].buffer = s->fb_; bi[1].offset = 0; bi[1].range = fRange;
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = s->sets[free_j]; wr[0].dstBinding = 0; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    wr[0].pBufferInfo = &bi[0];
    wr[1] = wr[0]; wr[1].dstBinding = 32; wr[1].pBufferInfo = &bi[1];
    ii[0].sampler = s_samp; ii[0].imageView = v;
    ii[1].sampler = s_samp; ii[1].imageView = s_dumView;   /* 41, unread here */
    for (j = 0; j < 2; j++) {
        ii[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[2 + j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[2 + j].dstSet = s->sets[free_j];
        wr[2 + j].dstBinding = (uint32_t)(40 + j);
        wr[2 + j].descriptorCount = 1;
        wr[2 + j].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[2 + j].pImageInfo = &ii[j];
    }
    vkUpdateDescriptorSets(d->dev, 4, wr, 0, NULL);
    s_setView[free_j] = v;
    *out = free_j;
    return 1;
}

/* THE STORE IS BEHIND: drop it, ask the producer for the fresh start, and
   composite nothing until its RESET arrives. Every path that cannot apply a
   frame's ops goes through here, and there is exactly one way out -- which is
   what makes "our twins equal the GL lane's" checkable rather than hoped for.
   A drop the retire will not take leaves the pass owing a teardown instead. */
static int behind(const TAGPU_VKPASS* d, const char* why)
{
    /* THE REQUEST IS RAISED ONCE, ON THE TRANSITION, AND THAT IS NOT TIDINESS.
       `tagpu_gui_mirror_reseed` raises the PRODUCER's flag, and the producer
       answers it by dropping every `seeded` mark and re-seeding every surface —
       megabytes into a 16 MB arena. Raising it on every call, from a condition
       that holds every frame, reseeds the GL lane's own twin store at the frame
       rate: `tagpu_gui_hook.c` calls that a reseed storm and measured 2 749
       resets in one walk the last time something caused one. **The GL lane is
       the oracle and this port may not change it** — which the first version of
       this function did, and which `gui.on=nostring` hid from the A/B.
       [FOUND 2026-09-16, the re-review of the review's fixes.] */
    if (!s_behind) {
        s_behind = 1;
        s_goodRun = 0;
        if (s_behindMute) {
            /* asked already, as often as this is worth asking */
        } else if (++s_behindAsks > BEHIND_ASKS_MAX) {
            s_behindMute = 1;
            plog(d, "gui: %d fresh starts have not made the twin store able to "
                    "follow the GL lane (%s) - this is a capability gap and not "
                    "a sync one, so nothing further is asked of the producer "
                    "and nothing is composited", BEHIND_ASKS_MAX, why);
        } else {
            plog(d, "gui: the twin store cannot follow the GL lane (%s) - asking "
                    "the producer for a fresh start, once, and compositing nothing "
                    "until it arrives", why);
            tagpu_gui_mirror_reseed();
        }
    }
    if (!tw_reset(d)) return 0;      /* caller refuses: the retire is full */
    return 1;
}

/* THE REPLAY. Every reason not to draw is taken BEFORE a byte is written, and
   each one stands the whole frame down rather than drawing part of it: a twin
   store is cumulative, so a partially applied frame is not a smaller picture,
   it is a wrong one that the NEXT frame inherits. */
int tagpu_vk_gui_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_GUIHAND h;
    SLOT* s;
    unsigned i;
    int ndraw = 0, nquad = 0, quads = 0;
    int glUp = 0;
    VkDeviceSize glOff = 0;
    VkDeviceSize stNeed = 0, stOff = 0, uStride, fStride;
    VkDeviceSize atOff = 0, palOff = 0, engOff = 0;
    int atUp = 0, palUp = 0, engUp = 0;
    TWIN* cur = NULL;
    int rpOpen = 0;
    int drawn = 0;
    /* WHY THE REPLAY STOOD DOWN. There are four ways to `standdown` now and
       they were all reported as the first one, which is how a log stops being
       evidence. */
    const char* sdWhy = "the presented surface has no twin here";
    int compose = 1;               /* the OPS always run; this gates the quad */
    TWIN* pres;

    s_drawThis = 0; s_abFrame = 0;
    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    s = &s_slot[slot];

    /* THE RETIRE'S ACCOUNTING, FIRST AND UNCONDITIONALLY: every path below can
       return early, and the path that returns early is exactly the one that
       must still clear this slot's bit or the retire stalls for ever. */
    ret_slot_done(d, slot);

    /* ASK FOR THE MIRROR, and keep asking. The GL half copies nothing until
       something wants it, and its drain runs EARLIER in this same iteration of
       render_ogl.c's loop than we do -- so the first frame this pass runs, the
       answer is always "nothing handed over", and the one after it is the first
       with a record. One frame of warm-up, once, and it costs an unarmed
       session nothing at all. */
    tagpu_gui_mirror_want(1);

    if (!tagpu_gui_handover(&h, d->frame)) {
        /* nothing handed over: the GL lane drew no composite either, and
           nothing is kept once there is nothing to draw (2.28) */
        if (s_state == ST_READY) slot_free(d, s);
        return 0;
    }

    /* THE RECORD SAYS IT DOES NOT CARRY THIS FRAME'S OPS. Replaying it would
       not catch the store up -- there is nothing in it to replay -- so this is
       the behind state and not a `compose = 0`. It is published rather than
       withheld precisely so that this branch exists: a withheld record is
       indistinguishable from an unarmed lane. */
    if (h.lost) {
        if (s_state == ST_UNBUILT) return 0;      /* nothing built to drop yet */
        if (!behind(d, "the GL lane's record of this frame was lost")) goto refuse;
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_gui_down(d); s_state = ST_REFUSED; return 0; }
        s_state = ST_READY;
    }

    /* ---- WHAT THIS LANDING DOES NOT CARRY, AND WHY IT DOES NOT RETURN HERE.
       These three gate the COMPOSITE and nothing else. The op stream still has
       to be applied, because our twin store must equal the GL lane's or it is
       worth nothing: the GL lane applied these ops whatever it drew, and a
       frame we skip leaves our twins behind ITS twins for the rest of the
       session, silently. So `compose` is cleared and the replay runs anyway.
       [FOUND 2026-09-16, the first in-game run: the draw-count bound below
       returned early and took the frame's ops with it.] ---- */
    /* A STRING OP IS APPLIED BY THE GL LANE AND CANNOT BE REPLAYED HERE -- the
       hand-over does not carry it, by design, because this landing has no
       glyph path. So it is not a `compose = 0`: those glyphs are in the GL
       twin and will never be in ours, and every later frame would composite a
       twin missing them. It is the behind state.
       [FOUND 2026-09-16, the landing review; §2.34's "the twins are kept
       level" was written of the ops the mirror CARRIES and was false of this
       one, which it does not.] */
    /* `otherOps` is 0 for every op kind that exists today -- the string was the
       last one it counted, and landing 2 carries it. The machinery stays,
       because the NEXT op kind added to the queue will land here rather than
       being drawn wrong, and it is a capability gap rather than a sync one:
       a fresh start would not help, so it says so once, composites nothing,
       and catches the store up when the op stops appearing. */
    if (h.otherOps > 0) {
        if (!s_cantReplay) {
            s_cantReplay = 1;
            plog(d, "gui: the GL twin applied %d op(s) this landing cannot "
                    "replay - nothing composited, and the store is caught up "
                    "with ONE fresh start when they stop", h.otherOps);
        }
        s_drawThis = 0; s_abFrame = 0;
        return 0;
    }
    if (s_cantReplay) {
        s_cantReplay = 0;
        if (!behind(d, "ops this landing cannot replay have stopped")) goto refuse;
        return 0;
    }
    if (h.colourTwins) {
        if (!s_saidColour) { s_saidColour = 1;
            plog(d, "gui: the GL twin is compositing a Classic++ colour twin and "
                    "this landing has none - nothing composited while that is "
                    "true. `gui.on=norestore` is the lever"); }
        compose = 0;
    } else s_saidColour = 0;
    if (h.sharpOn) {
        if (!s_saidSharp) { s_saidSharp = 1;
            plog(d, "gui: the GL twin has coverage in the sharp layer (the "
                    "cursor, a string or the minimap) and this landing draws "
                    "none - nothing composited. `gui.on=nocursor nominimap` are "
                    "the levers"); }
        compose = 0;
    } else s_saidSharp = 0;
    /* THE ENGINE'S OWN FRAME IS THE COMPOSITE'S BOTTOM LAYER. Without it the
       GL lane's `uSurf` reads an image ours would not have, so the two would
       differ everywhere the twin has no coverage -- which is most of a frame. */
    if (!h.eng || h.engW < 1 || h.engH < 1 ||
        h.engW > SURF_MAXDIM || h.engH > SURF_MAXDIM) {
        if (!s_saidEng) { s_saidEng = 1;
            plog(d, "gui: the hand-over carries no copy of the engine's own "
                    "frame - nothing composited while that is true"); }
        compose = 0;
    } else s_saidEng = 0;
    if (!h.pal || !h.presented || h.surfW < 1 || h.surfH < 1 ||
        h.surfW > SURF_MAXDIM || h.surfH > SURF_MAXDIM) compose = 0;
    if (h.atlas && (h.atlasDim < 1 || h.atlasDim > ATLAS_MAXDIM ||
                    h.atlasRows < 1 || h.atlasRows > h.atlasDim)) {
        /* the atlas is what a sprite samples: without a sane one the replay
           itself cannot run */
        if (!behind(d, "an atlas outside what this pass carries")) goto refuse;
        return 0;
    }
    /* THE GLYPH ATLAS'S DIMENSIONS, BOUNDED IN THIS FILE'S OWN TERMS. They are
       compile-time constants in `tagpu_text.c` and cannot move today, which is
       exactly why the bound belongs here rather than being inherited from
       another file: the draw arm below divides by `s_glW`/`s_glH`, and the only
       thing that makes those equal the dimensions the cells were resolved
       against is this refusal. */
    if (h.glyphs && (h.glyphW < 1 || h.glyphH < 1 ||
                     h.glyphW > ATLAS_MAXDIM || h.glyphH > ATLAS_MAXDIM)) {
        if (!behind(d, "a glyph atlas outside what this pass carries")) goto refuse;
        return 0;
    }

    /* ---- pass 1: validate every op and count what the frame needs ----
       IN THIS FILE'S OWN TERMS. A bound that lives in the file that produced
       the number is a bound only while both files are read together. */
    for (i = 0; i < h.nops; i++) {
        const TAGPU_GUIOP* o = &h.ops[i];
        int bw = o->r - o->l + 1, bh = o->b - o->t + 1;
        switch (o->kind) {
        case TAGPU_GUIOP_SEED:
            if (o->w < 1 || o->h < 1 || o->w > SURF_MAXDIM || o->h > SURF_MAXDIM) { if (!behind(d, "a malformed op")) goto refuse; return 0; }
            if (o->alen) {
                if (o->alen != (unsigned)o->w * (unsigned)o->h) { if (!behind(d, "a malformed op")) goto refuse; return 0; }
                if (o->aoff > h.alen || o->aoff + o->alen > h.alen) { if (!behind(d, "a malformed op")) goto refuse; return 0; }
                stNeed += (VkDeviceSize)o->alen * 2;
            }
            break;
        case TAGPU_GUIOP_PIXELS:
            if (bw < 1 || bh < 1) { if (!behind(d, "a malformed op")) goto refuse; return 0; }
            if (o->alen != (unsigned)bw * (unsigned)bh) { if (!behind(d, "a malformed op")) goto refuse; return 0; }
            if (o->aoff > h.alen || o->aoff + o->alen > h.alen) { if (!behind(d, "a malformed op")) goto refuse; return 0; }
            stNeed += (VkDeviceSize)o->alen * 2;
            break;
        case TAGPU_GUIOP_SPRITE:
            if (o->fw < 1 || o->fh < 1) { if (!behind(d, "a malformed op")) goto refuse; return 0; }
            if (!h.atlas) { if (!behind(d, "a sprite with no atlas")) goto refuse; return 0; }
            ndraw++; nquad++;
            break;
        case TAGPU_GUIOP_STRING:
            /* A STRING IS ONE UNIFORM WINDOW AND `nglyph` QUADS. Its cells were
               resolved by the GL lane against an atlas that can repack
               mid-string, so they are carried rather than looked up again --
               and bounded here in this file's own terms all the same. */
            if (o->nglyph < 1 || o->nglyph > 256 ||
                o->alen != (unsigned)o->nglyph * 8 ||
                o->aoff > h.alen || o->aoff + o->alen > h.alen) {
                if (!behind(d, "a malformed string op")) goto refuse;
                return 0;
            }
            if (!h.glyphs) { if (!behind(d, "a string with no glyph atlas")) goto refuse; return 0; }
            ndraw++; nquad += o->nglyph;
            break;
        case TAGPU_GUIOP_COPY:
            /* A SELF-COPY IS REFUSED rather than guessed at: the GL lane reads
               and writes one texture in one draw there, which is undefined in
               both APIs, and reproducing undefined behaviour is not parity. */
            if (o->surf == o->src) { if (!behind(d, "a malformed op")) goto refuse; return 0; }
            ndraw++; nquad++;
            break;
        case TAGPU_GUIOP_CLEAR:
            if (bw < 1 || bh < 1) { if (!behind(d, "a malformed op")) goto refuse; return 0; }
            break;
        case TAGPU_GUIOP_FREE:
        case TAGPU_GUIOP_RESET:
            break;
        default:
            if (!behind(d, "an op kind this pass does not know")) goto refuse;
            return 0;
        }
    }
    if (ndraw > DRAW_MAX || nquad > QUAD_MAX) {
        /* THIS ONE CANNOT APPLY THE OPS, so it is not a `compose = 0`: the
           store would fall behind and stay there. It asks for the fresh start
           instead, which is the only way back to level. */
        if (!s_saidRoom) { s_saidRoom = 1;
            plog(d, "gui: %d draws / %d quads in one frame, past this pass's "
                    "bounds of %d / %d - the store cannot follow, asking for a "
                    "fresh start", ndraw, nquad, DRAW_MAX, QUAD_MAX); }
        if (!behind(d, "more draws or quads in one frame than the bound")) goto refuse;
        return 0;
    }
    s_saidRoom = 0;

    /* the shared texels, and the staging they need */
    if (h.atlas && (!s_atHave || s_atSerial != h.atlasSerial || s_atDim != h.atlasDim)) {
        atUp = 1; stNeed += (VkDeviceSize)h.atlasDim * h.atlasRows;
    }
    if (h.pal && (!s_palHave || s_palSerial != h.palSerial)) { palUp = 1; stNeed += 256 * 4; }
    /* THE CONTENT SERIAL, NOT THE REPACK GENERATION -- see tagpu_gui.h. The
       first draft keyed this on `glyphGen`, which moves only when the atlas is
       thrown away, so the image was uploaded once and every glyph rasterised
       afterwards stayed 0 in it: invisible text where `bg == tr` and a solid
       box where it is not, permanently, and no counter anywhere said so. */
    if (h.glyphs && (!s_glHave || s_glSerial != h.glyphSerial ||
                     s_glW != h.glyphW || s_glH != h.glyphH)) {
        glUp = 1; stNeed += (VkDeviceSize)h.glyphW * h.glyphH;
    }
    /* the engine's frame moves every frame by definition, so it needs no
       serial -- but it is only there on a frame the composite can be drawn on,
       and this function now runs the replay on frames it cannot composite */
    if (h.eng) { engUp = 1; stNeed += (VkDeviceSize)h.engW * h.engH; }

    /* ---- room ---- */
    uStride = align_up(QVS_SZ, s_ualign);
    fStride = align_up(TWF_SZ, s_ualign);
    if (!grow(d, &s->stage, &s->stMem, &s->stMap, &s->stCap,
              stNeed ? stNeed : 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) goto refuse;
    if (!grow(d, &s->vb, &s->vbMem, &s->vbMap, &s->vbCap,
              (VkDeviceSize)(nquad + 1) * 24 * sizeof(float),
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) goto refuse;
    if (!grow(d, &s->ub, &s->ubMem, &s->ubMap, &s->ubCap,
              uStride * (ndraw ? ndraw : 1), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) goto refuse;
    if (!grow(d, &s->fb_, &s->fbMem, &s->fbMap, &s->fbCap,
              fStride * (ndraw ? ndraw : 1), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) goto refuse;
    if (!s->lb && !mk_buffer(d, LAY_SZ, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             &s->lb, &s->lbMem, &s->lbMap)) goto refuse;
    if (!dummies(d, cb)) goto refuse;
    s->built = 1;

    /* ---- the shared texels ---- */
    if (atUp) {
        if (s_atDim != h.atlasDim) {
            /* every in-flight composite samples this one */
            if (!ret_push(d, s_atImg, s_atMem, s_atView, VK_NULL_HANDLE)) goto refuse;
            s_atImg = VK_NULL_HANDLE; s_atMem = VK_NULL_HANDLE; s_atView = VK_NULL_HANDLE;
            s_atDim = 0; s_atHave = 0;
            if (!mk_image(d, h.atlasDim, h.atlasDim, VK_FORMAT_R8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          &s_atImg, &s_atMem, &s_atView)) goto refuse;
            s_atDim = h.atlasDim;
        }
        atOff = stOff;
        memcpy(s->stMap + stOff, h.atlas, (size_t)h.atlasDim * h.atlasRows);
        stOff += (VkDeviceSize)h.atlasDim * h.atlasRows;
    }
    if (glUp) {
        if (s_glW != h.glyphW || s_glH != h.glyphH) {
            if (!ret_push(d, s_glImg, s_glMem, s_glView, VK_NULL_HANDLE)) goto refuse;
            s_glImg = VK_NULL_HANDLE; s_glMem = VK_NULL_HANDLE; s_glView = VK_NULL_HANDLE;
            s_glW = s_glH = 0; s_glHave = 0;
            if (!mk_image(d, h.glyphW, h.glyphH, VK_FORMAT_R8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          &s_glImg, &s_glMem, &s_glView)) goto refuse;
            s_glW = h.glyphW; s_glH = h.glyphH;
        }
        glOff = stOff;
        memcpy(s->stMap + stOff, h.glyphs, (size_t)h.glyphW * h.glyphH);
        stOff += (VkDeviceSize)h.glyphW * h.glyphH;
    }
    if (palUp) {
        if (!s_palImg &&
            !mk_image(d, 256, 1, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &s_palImg, &s_palMem, &s_palView)) goto refuse;
        palOff = stOff;
        memcpy(s->stMap + stOff, h.pal, 256 * 4);
        stOff += 256 * 4;
    }
    if (engUp) {
        if (s_engW != h.engW || s_engH != h.engH) {
            if (!ret_push(d, s_engImg, s_engMem, s_engView, VK_NULL_HANDLE)) goto refuse;
            s_engImg = VK_NULL_HANDLE; s_engMem = VK_NULL_HANDLE; s_engView = VK_NULL_HANDLE;
            s_engW = s_engH = 0; s_engHave = 0;
            if (!mk_image(d, h.engW, h.engH, VK_FORMAT_R8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          &s_engImg, &s_engMem, &s_engView)) goto refuse;
            s_engW = h.engW; s_engH = h.engH;
        }
        engOff = stOff;
        memcpy(s->stMap + stOff, h.eng, (size_t)h.engW * h.engH);
        stOff += (VkDeviceSize)h.engW * h.engH;
    }

    if (atUp) {
        img_barrier(cb, s_atImg,
                    s_atHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                             : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    s_atHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    s_atHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        copy_rect(cb, s->stage, atOff, s_atImg, 0, 0, h.atlasDim, h.atlasRows);
        img_barrier(cb, s_atImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        s_atHave = 1; s_atSerial = h.atlasSerial;
    }
    if (glUp) {
        img_barrier(cb, s_glImg,
                    s_glHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                             : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    s_glHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    s_glHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        copy_rect(cb, s->stage, glOff, s_glImg, 0, 0, h.glyphW, h.glyphH);
        img_barrier(cb, s_glImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        s_glHave = 1; s_glSerial = h.glyphSerial;
    }
    if (palUp) {
        img_barrier(cb, s_palImg,
                    s_palHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    s_palHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                              : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    s_palHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        copy_rect(cb, s->stage, palOff, s_palImg, 0, 0, 256, 1);
        img_barrier(cb, s_palImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        s_palHave = 1; s_palSerial = h.palSerial;
    }
    if (engUp) {
    img_barrier(cb, s_engImg,
                s_engHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                          : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                s_engHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                s_engHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->stage, engOff, s_engImg, 0, 0, h.engW, h.engH);
    img_barrier(cb, s_engImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    s_engHave = 1;
    }

    /* ---- THE ORDERING THAT MAKES A SHARED TWIN STORE SAFE (the file header).
       Every earlier frame's composite read its twin in the fragment stage; this
       frame is about to write them as colour attachments. A barrier's first
       scope covers everything submitted previously to this queue, so this one
       line is the whole argument -- not a fence count, not a pass counter. ---- */
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }

    memset(s_setView, 0, sizeof s_setView);   /* the frame's claims start clean */

    /* WHILE BEHIND, ONLY A RESET IS APPLIED. Everything before the producer's
       fresh start names twins this store never made, and applying it would
       build a picture out of half a history. `tagpu_gui_surf.c`'s drain does
       exactly this with `s_skipToReset` after a GL context change. */

    /* ---- pass 2: the ops, in the order the GL lane applied them ---- */
    for (i = 0; i < h.nops; i++) {
        const TAGPU_GUIOP* o = &h.ops[i];
        TWIN* t;
        int bw = o->r - o->l + 1, bh = o->b - o->t + 1;
        if (s_behind) {
            if (o->kind != TAGPU_GUIOP_RESET) continue;
            s_behind = 0;               /* the fresh start has arrived */
        }

        /* a draw needs a render pass open on ITS destination; everything else
           needs none open at all (a transfer and a layout change may not be
           recorded inside one) */
        if (o->kind == TAGPU_GUIOP_SPRITE || o->kind == TAGPU_GUIOP_COPY ||
            o->kind == TAGPU_GUIOP_STRING || o->kind == TAGPU_GUIOP_CLEAR) {
            TWIN* src = NULL;
            t = tw_find(o->surf);
            if (!t) continue;               /* the GL lane had one; we do not  */
            /* a clear and a layout change are both illegal inside a render
               pass instance, so the open one closes first. Unreachable while
               `tw_make` is the only creator and clears on the next line — but
               "unreachable" is an enumeration of today's call sites, and this
               is the bound. */
            if (t->needClear && rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
            tw_fresh(cb, t);
            if (o->kind == TAGPU_GUIOP_COPY) {
                src = tw_find(o->src);
                if (!src) {
                    /* OUR STORE HAS FALLEN BEHIND. Ask for the fresh start the
                       GL consumer asks for in the same situation and stop
                       drawing until it arrives -- applying the rest would put a
                       wrong picture in a twin that the NEXT frame inherits. */
                    /* THE ASK IS `behind`'s, NOT THIS SITE'S. Raising the
                       producer's flag here and then falling into `standdown`
                       -- which calls `behind`, which raises it again -- is two
                       reseeds per occurrence, and it walks straight past the
                       cap that stops a recurring condition reseeding the GL
                       lane's own store at the frame rate. One door.
                       [FOUND 2026-09-16, the landing-2 review.] */
                    if (rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
                    sdWhy = "a copy names a source twin this store never made";
                    goto standdown;
                }
                if (src->needClear && rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
                tw_fresh(cb, src);
                if (src != cur && rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
                if (rpOpen && cur != t) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
                if (!rpOpen) tw_to(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            if (rpOpen && cur != t) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
            if (!rpOpen) {
                VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                tw_to(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                rb.renderPass = s_twRp; rb.framebuffer = t->fb;
                rb.renderArea.extent.width = (uint32_t)t->w;
                rb.renderArea.extent.height = (uint32_t)t->h;
                vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
                set_viewport(cb, t->w, t->h);
                rpOpen = 1; cur = t;
            }
            /* THE STRING SETS NO SCISSOR, which is the GL lane's own
               behaviour: `twin_string` disables the scissor test outright
               where the sprite and the copy enable it. Clipping here would cut
               glyphs the GL twin has. */
            if (o->kind == TAGPU_GUIOP_STRING) set_scissor(cb, 0, 0, t->w, t->h, t->w, t->h);
            else                               set_scissor(cb, o->l, o->t, bw, bh, t->w, t->h);

            if (o->kind == TAGPU_GUIOP_CLEAR) {
                /* the GL lane's scissored glClear to coverage 0 */
                VkClearAttachment ca;
                VkClearRect cr;
                memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
                ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ca.colorAttachment = 0;
                cr.rect.offset.x = o->l; cr.rect.offset.y = o->t;
                cr.rect.extent.width = (uint32_t)bw;
                cr.rect.extent.height = (uint32_t)bh;
                cr.layerCount = 1;
                if (cr.rect.offset.x < 0) cr.rect.offset.x = 0;
                if (cr.rect.offset.y < 0) cr.rect.offset.y = 0;
                if (cr.rect.offset.x + (int)cr.rect.extent.width > t->w)
                    cr.rect.extent.width = (uint32_t)(t->w - cr.rect.offset.x);
                if (cr.rect.offset.y + (int)cr.rect.extent.height > t->h)
                    cr.rect.extent.height = (uint32_t)(t->h - cr.rect.offset.y);
                if (cr.rect.extent.width && cr.rect.extent.height)
                    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
                continue;
            }

            /* a draw: its two blocks, its quad, its set */
            {
                float qv[24];
                float* uq = (float*)(s->ubMap + (VkDeviceSize)drawn * uStride);
                int*   fq = (int*)(s->fbMap + (VkDeviceSize)drawn * fStride);
                uint32_t dyn[2];
                VkDeviceSize vbOff = (VkDeviceSize)quads * 24 * sizeof(float);
                VkDescriptorSet ds;
                int si_ = 0;
                uq[0] = (float)t->w; uq[1] = (float)t->h;
                if (o->kind == TAGPU_GUIOP_STRING) {
                    /* ONE BLOCK, `nglyph` QUADS. Every glyph of a string shares
                       its three colours and the twin's size, so the set is
                       bound once and the vertex offset walks -- which is what
                       `twin_string` does with one program and n draws. */
                    const short* cell = (const short*)(h.arena + o->aoff);
                    /* pass 1 refuses a string with no glyph atlas in the
                       hand-over; this is the other half -- one whose DIMENSIONS
                       were outside what this pass carries, so the upload never
                       ran and the view is null. A descriptor must be valid. */
                    if (!s_glHave) { sdWhy = "no glyph atlas is uploaded"; goto standdown; }
                    int g, penx = o->sl, top = o->st;
                    int* fq3 = fq;
                    fq3[0] = (int)o->fg; fq3[1] = (int)o->bg; fq3[2] = (int)o->tr;
                    if (!set_claim(d, s, s_glView, QVS_SZ, TWF_SZ, &si_)) {
                        sdWhy = "this frame claimed more distinct images than there are sets";
                        goto standdown; }
                    ds = s->sets[si_];
                    dyn[0] = (uint32_t)((VkDeviceSize)drawn * uStride);
                    dyn[1] = (uint32_t)((VkDeviceSize)drawn * fStride);
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeStr);
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            s_ploTwin, 0, 1, &ds, 2, dyn);
                    for (g = 0; g < (int)o->nglyph; g++) {
                        int ax = cell[g * 4 + 0], ay = cell[g * 4 + 1];
                        int gw = cell[g * 4 + 2], gh = cell[g * 4 + 3];
                        VkDeviceSize off = (VkDeviceSize)quads * 24 * sizeof(float);
                        quadv(qv, (float)penx, (float)top,
                              (float)(penx + gw), (float)(top + gh),
                              (float)ax / (float)s_glW,        (float)ay / (float)s_glH,
                              (float)(ax + gw) / (float)s_glW, (float)(ay + gh) / (float)s_glH);
                        memcpy(s->vbMap + off, qv, sizeof qv);
                        vkCmdBindVertexBuffers(cb, 0, 1, &s->vb, &off);
                        vkCmdDraw(cb, 6, 1, 0, 0);
                        penx += gw;
                        quads++;
                    }
                    drawn++;
                    continue;
                }
                if (o->kind == TAGPU_GUIOP_SPRITE) {
                    fq[0] = (int)o->ck; fq[1] = 0;      /* uCK, uRestored      */
                    quadv(qv, (float)o->sl, (float)o->st,
                          (float)(o->sl + o->fw), (float)(o->st + o->fh),
                          o->u0, o->v0, o->u1, o->v1);
                    if (!set_claim(d, s, s_atView, QVS_SZ, TWF_SZ, &si_)) {
                        sdWhy = "this frame claimed more distinct images than there are sets";
                        goto standdown; }
                    ds = s->sets[si_];
                } else {
                    fq[0] = o->l - o->sl; fq[1] = o->t - o->st; fq[2] = 0;
                    quadv(qv, (float)o->l, (float)o->t,
                          (float)(o->r + 1), (float)(o->b + 1), 0, 0, 0, 0);
                    if (!set_claim(d, s, src->view, QVS_SZ, TWF_SZ, &si_)) {
                    sdWhy = "this frame claimed more distinct images than there are sets";
                    goto standdown; }
                    ds = s->sets[si_];
                }
                memcpy(s->vbMap + vbOff, qv, sizeof qv);
                dyn[0] = (uint32_t)((VkDeviceSize)drawn * uStride);
                dyn[1] = (uint32_t)((VkDeviceSize)drawn * fStride);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  o->kind == TAGPU_GUIOP_SPRITE ? s_pipeSpr : s_pipeCpy);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        s_ploTwin, 0, 1, &ds, 2, dyn);
                vkCmdBindVertexBuffers(cb, 0, 1, &s->vb, &vbOff);
                vkCmdDraw(cb, 6, 1, 0, 0);
                drawn++; quads++;
            }
            continue;
        }

        /* everything else runs outside a render pass */
        if (rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
        switch (o->kind) {
        case TAGPU_GUIOP_RESET:
            /* the retire refusing is the pass stopping, not a drop to shrug
               off: keeping twins the GL lane has destroyed is divergence */
            if (!tw_reset(d)) goto refuse;
            s_behind = 0;
            break;
        case TAGPU_GUIOP_FREE:
            t = tw_find(o->surf);
            if (t && !tw_drop(d, t)) goto refuse;
            break;
        case TAGPU_GUIOP_SEED:
        case TAGPU_GUIOP_PIXELS: {
            int uw = (o->kind == TAGPU_GUIOP_SEED) ? o->w : bw;
            int uh = (o->kind == TAGPU_GUIOP_SEED) ? o->h : bh;
            int ux = (o->kind == TAGPU_GUIOP_SEED) ? 0 : o->l;
            int uy = (o->kind == TAGPU_GUIOP_SEED) ? 0 : o->t;
            unsigned n, j;
            const unsigned char* idx;
            unsigned char* dst;
            if (o->kind == TAGPU_GUIOP_SEED) {
                t = tw_make(d, o->surf, o->w, o->h);
                if (!t) goto refuse;
            } else {
                t = tw_find(o->surf);
                if (!t) continue;
            }
            tw_fresh(cb, t);
            if (!o->alen) break;
            if (ux < 0 || uy < 0 || ux + uw > t->w || uy + uh > t->h) continue;
            /* THE SAME INTERLEAVE THE GL LANE DOES: R = the palette index,
               G = 255 for covered. tagpu_gui_surf.c's twin_upload. */
            n = (unsigned)uw * (unsigned)uh;
            idx = h.arena + o->aoff;
            dst = s->stMap + stOff;
            for (j = 0; j < n; j++) { dst[2 * j] = idx[j]; dst[2 * j + 1] = 255; }
            tw_to(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            copy_rect(cb, s->stage, stOff, t->img, ux, uy, uw, uh);
            stOff += (VkDeviceSize)n * 2;
            break; }
        default: break;
        }
    }
    if (rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }

    /* ---- the composite's own set and block ---- */
    if (s_behind || !compose || !s_engHave || !s_palHave) {
        s_drawThis = 0; s_abFrame = 0; return 0;
    }
    pres = tw_find(h.presented);
    if (!pres || pres->w != h.surfW || pres->h != h.surfH) goto standdown;
    tw_to(cb, pres, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {
        unsigned char* b = s->lbMap;
        int* ip; float* fp;
        memset(b, 0, LAY_SZ);
        ip = (int*)(b + 0);   *ip = 0;                       /* uColOn        */
        ip = (int*)(b + 4);   *ip = 0;                       /* uSharpOn      */
        ip = (int*)(b + 8);   ip[0] = h.surfW; ip[1] = h.surfH;      /* uSize */
        ip = (int*)(b + 16);  ip[0] = 1; ip[1] = 1;          /* uSharpSize    */
        fp = (float*)(b + 24); fp[0] = h.scaleX; fp[1] = h.scaleY;   /* uScale */
        ip = (int*)(b + 32);  *ip = h.strict;
        ip = (int*)(b + 36);  *ip = h.key;
        fp = (float*)(b + 48); fp[0] = h.vpL; fp[1] = h.vpT;
                               fp[2] = h.vpW; fp[3] = h.vpH;
        fp = (float*)(b + 64); fp[0] = h.curEng[0]; fp[1] = h.curEng[1];
                               fp[2] = h.curEng[2]; fp[3] = h.curEng[3];
        ip = (int*)(b + 80);  *ip = h.vpKey;
        ip = (int*)(b + 84);  *ip = h.curOurs;
        fp = (float*)(b + 96); fp[0] = h.hud[0]; fp[1] = h.hud[1];
                               fp[2] = h.hud[2]; fp[3] = h.hud[3];
        ip = (int*)(b + 112); *ip = h.guard;
    }
    {
        VkDescriptorBufferInfo bi;
        VkDescriptorImageInfo ii[5];
        VkWriteDescriptorSet wr[6];
        int j;
        memset(&bi, 0, sizeof bi); memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
        bi.buffer = s->lb; bi.offset = 0; bi.range = LAY_SZ;
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = s->laySet; wr[0].dstBinding = 32; wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr[0].pBufferInfo = &bi;
        ii[0].imageView = pres->view;                /* 40 uTwin              */
        ii[1].imageView = s_palView;                 /* 41 uPal               */
        ii[2].imageView = s_engView;                 /* 42 uSurf              */
        ii[3].imageView = s_dumView;                 /* 43 uTwinCol (unused)  */
        ii[4].imageView = s_dumView;                 /* 44 uSharp  (unused)   */
        for (j = 0; j < 5; j++) {
            ii[j].sampler = s_samp;
            ii[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            wr[1 + j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[1 + j].dstSet = s->laySet;
            wr[1 + j].dstBinding = (uint32_t)(40 + j);
            wr[1 + j].descriptorCount = 1;
            wr[1 + j].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[1 + j].pImageInfo = &ii[j];
        }
        vkUpdateDescriptorSets(d->dev, 6, wr, 0, NULL);
    }

    /* THE COMPOSITE'S OWN QUAD, at the end of this frame's vertex buffer.
       LAY_VS takes `a.xy` as both uv and position, so it is the unit square. */
    {
        float qv[24];
        quadv(qv, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        memcpy(s->vbMap + (VkDeviceSize)nquad * 24 * sizeof(float), qv, sizeof qv);
        s_layQuad = (VkDeviceSize)nquad * 24 * sizeof(float);
    }
    s_layVp[0] = h.vpX; s_layVp[1] = h.vpY;
    s_layVp[2] = h.vpW_gl; s_layVp[3] = h.vpH_gl;
    s_presented = h.presented; s_surfW = h.surfW; s_surfH = h.surfH;
    s_uStride = uStride; s_fStride = fStride;
    s_abFrame = h.ab;
    s_drawThis = 1;
    if (!s_behindMute && s_behindAsks && ++s_goodRun >= BEHIND_GOOD_RUN) {
        s_behindAsks = 0; s_goodRun = 0;    /* the last fresh start worked */
    }
    return 1;

standdown:
    if (rpOpen) vkCmdEndRenderPass(cb);
    /* THE STORE IS BEHIND AND SOMETHING HAS TO ASK. Dropping it and saying
       nothing was the bug: `tw_find(h.presented)` would answer NULL again on
       every later frame, land here again, and the pass would never draw for
       the rest of the session with no line in the log to say why.
       [FOUND 2026-09-16 -- both landing reviewers, separately.] */
    if (!behind(d, sdWhy)) goto refuse;
    s_drawThis = 0; s_abFrame = 0;
    return 0;

refuse:
    /* NOTHING IS DESTROYED HERE. This is the middle of a frame: `cb` already
       names this pass's objects and is submitted whether it draws or not, and
       every OTHER slot's submit is still executing. The pass stops drawing and
       OWES a teardown; the seam pays it behind its vkDeviceWaitIdle. */
    if (rpOpen) vkCmdEndRenderPass(cb);
    plog(d, "gui: slot %u would not take this frame's resources - the pass "
            "stops drawing and the seam tears it down", (unsigned)slot);
    s_state = ST_REFUSED;
    s_downOwed = 1;
    s_drawThis = 0; s_abFrame = 0;
    return 0;
}

void tagpu_vk_gui_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h)
{
    SLOT* s;
    VkViewport vp;
    VkRect2D sc;
    int x, ytop, vw, vh;

    if (s_state != ST_READY || !s_drawThis) return;
    s_drawThis = 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return;
    s = &s_slot[slot];

    /* THE PIPELINE IS BUILT AGAINST THE SEAM'S RENDER PASS, and rebuilt when
       that changes -- a swapchain rebuild can hand us a different one. */
    if (!s_pipeLay || s_layRp != d->rp) {
        if (s_pipeLay) { vkDestroyPipeline(d->dev, s_pipeLay, NULL); s_pipeLay = VK_NULL_HANDLE; }
        if (!build_lay_pipe(d, d->rp)) {
            plog(d, "gui: the composite pipeline would not build against the "
                    "seam's render pass - nothing drawn");
            s_layRp = VK_NULL_HANDLE;
            return;
        }
        s_layRp = d->rp;
    }

    /* THE GL LANE'S OWN VIEWPORT, turned over. GL's y counts from the BOTTOM of
       the window and Vulkan's from the top, and this pass is PRESENTED -- so it
       takes the negative height every presented pass on this lane takes, which
       puts LAY_VS's `1 - a.y*2` back at attachment row 0 (the file header). */
    x = s_layVp[0]; vw = s_layVp[2]; vh = s_layVp[3];
    if (vw <= 0 || vh <= 0) { x = 0; vw = (int)w; vh = (int)h; ytop = 0; }
    else ytop = (int)h - s_layVp[1] - vh;
    if (ytop < 0) ytop = 0;
    vp.x = (float)x;
    vp.y = (float)(ytop + vh);
    vp.width = (float)vw;
    vp.height = -(float)vh;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = w; sc.extent.height = h;
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeLay);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ploLay,
                            0, 1, &s->laySet, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &s->vb, &s_layQuad);
    vkCmdDraw(cb, 6, 1, 0, 0);
}

int tagpu_vk_gui_ab_frame(void)
{
    int a = s_abFrame;
    s_abFrame = 0;
    return a;
}

void tagpu_vk_gui_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    /* whether this teardown is the one the pass asked for -- the lane's
       owed-teardown protocol, unchanged from every other pass */
    int owed = s_downPaying;
    s_downOwed = 0;
    s_drawThis = 0; s_abFrame = 0;
    s_presented = 0;
    /* nothing is kept once nothing asks for it -- the GL half's copy of the op
       stream is the largest thing this lane costs when it is not drawing */
    tagpu_gui_mirror_want(0);

    if (!d->dev || !vkDestroyPipeline) {
        s_state = owed ? ST_REFUSED : ST_UNBUILT;
        s_ntw = 0;
        return;
    }
    /* behind the seam's vkDeviceWaitIdle: nothing of ours is in a queue, so
       the retire is emptied outright rather than one slot at a time */
    while (s_ntw) { TWIN* t = &s_tw[0];
                    if (t->fb) vkDestroyFramebuffer(d->dev, t->fb, NULL);
                    kill_image(d, &t->img, &t->mem, &t->view);
                    *t = s_tw[--s_ntw]; memset(&s_tw[s_ntw], 0, sizeof s_tw[s_ntw]); }
    ret_drain_idle(d);
    s_behind = 0; s_cantReplay = 0;
    s_behindAsks = 0; s_behindMute = 0; s_goodRun = 0;
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        slot_free(d, &s_slot[i]);
        s_slot[i].laySet = VK_NULL_HANDLE;
        memset(s_slot[i].sets, 0, sizeof s_slot[i].sets);  /* back with the pool */
    }
    kill_image(d, &s_atImg,  &s_atMem,  &s_atView);
    kill_image(d, &s_palImg, &s_palMem, &s_palView);
    kill_image(d, &s_engImg, &s_engMem, &s_engView);
    kill_image(d, &s_glImg,  &s_glMem,  &s_glView);
    kill_image(d, &s_dumImg, &s_dumMem, &s_dumView);
    s_atDim = 0; s_atHave = 0; s_atSerial = 0;
    s_palHave = 0; s_palSerial = 0;
    s_engW = s_engH = 0; s_engHave = 0; s_dumReady = 0;
    s_glW = s_glH = 0; s_glHave = 0; s_glSerial = 0;
    if (s_dpool) { vkDestroyDescriptorPool(d->dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_pipeSpr) { vkDestroyPipeline(d->dev, s_pipeSpr, NULL); s_pipeSpr = VK_NULL_HANDLE; }
    if (s_pipeCpy) { vkDestroyPipeline(d->dev, s_pipeCpy, NULL); s_pipeCpy = VK_NULL_HANDLE; }
    if (s_pipeStr) { vkDestroyPipeline(d->dev, s_pipeStr, NULL); s_pipeStr = VK_NULL_HANDLE; }
    if (s_pipeLay) { vkDestroyPipeline(d->dev, s_pipeLay, NULL); s_pipeLay = VK_NULL_HANDLE; }
    s_layRp = VK_NULL_HANDLE;
    if (s_ploTwin) { vkDestroyPipelineLayout(d->dev, s_ploTwin, NULL); s_ploTwin = VK_NULL_HANDLE; }
    if (s_ploLay)  { vkDestroyPipelineLayout(d->dev, s_ploLay, NULL);  s_ploLay = VK_NULL_HANDLE; }
    if (s_dslTwin) { vkDestroyDescriptorSetLayout(d->dev, s_dslTwin, NULL); s_dslTwin = VK_NULL_HANDLE; }
    if (s_dslLay)  { vkDestroyDescriptorSetLayout(d->dev, s_dslLay, NULL);  s_dslLay = VK_NULL_HANDLE; }
    if (s_twRp) { vkDestroyRenderPass(d->dev, s_twRp, NULL); s_twRp = VK_NULL_HANDLE; }
    if (s_samp) { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    memset(s_setView, 0, sizeof s_setView);
    /* ST_UNBUILT and not ST_REFUSED: a pass brought down by a mode change or a
       cleared lever must be able to come back. The owed teardown is the one
       exception. */
    s_state = owed ? ST_REFUSED : ST_UNBUILT;
}

int tagpu_vk_gui_down_owed(void) { return s_downOwed; }

void tagpu_vk_gui_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_gui_down(d);
    s_downPaying = 0;
}
