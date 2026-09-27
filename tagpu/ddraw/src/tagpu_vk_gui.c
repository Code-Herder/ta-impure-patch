/* tagpu_vk_gui.c -- the UI layer's 1x mirror, drawn by Vulkan.
   The header states the contract; this file is the machinery. gpu-status.md
   2.3e describes the pass.

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
#include "tagpu_vk_restore.h"   /* the UI atlas is restored HERE, from a published list */
#include "spirv/tagpu_gui_surf.spv.h"

/* the std140 blocks, at the sizes the generated SPIR-V header prints */
#define QVS_SZ  16                  /* QVS    binding 0  : vec2 uSize        */
#define TWF_SZ  16                  /* CPY/SPR binding 32: 16 bytes          */
#define LAY_SZ  80                  /* LAY_FS binding 32 : 80 bytes          */

/* MAX_TWINS in tagpu_gui_surf.c. A bound here rather than a number taken from
   the hand-over: the scaffold's rule -- a handed-over count never sizes an
   allocation. */
#define TW_MAX      32
/* sprite + copy draws in one frame. "A steady screen is ~41 ops of which a
   handful draw" is true of a FLIP and not of a PUBLISH: the publisher batches
   every flip since its last one (the shell flips ~12 000 times a second at a
   5 ms cadence), so one hand-over routinely carries thousands. MEASURED
   2026-09-16 at 7414 on a 1080p level load. This is the bound on a runaway,
   not on a busy frame. */
#define DRAW_MAX    16384
/* AND THE QUADS, WHICH ARE NOT THE DRAWS. A string is ONE draw and up to 256
   quads, so `DRAW_MAX` alone bounds a runaway at 4.2 million quads -- a 402 MB
   host-visible `grow()` per slot, which `grow` never shrinks again. The worst
   frame measured above was 7414 quads. */
#define QUAD_MAX    65536
/* HOW MANY DESCRIPTOR SETS A FRAME IS GIVEN. Three kinds of image a twin draw
   samples: every sprite the ONE UI atlas, every string the ONE glyph atlas, and
   a copy its SOURCE TWIN. Each claim is a PAIR, so a copy SOURCE can be
   claimed twice in one frame -- once with its colour view for a coloured
   destination, once with the dummy for an uncoloured one, and `uSrcHasCol` is
   `src->rgb && t->rgb`, which is a property of the DESTINATION. The sprite
   dimension doubles the same way (restored and unrestored sprites in one frame
   are two pairs of the same atlas). So the worst case for a frame that does
   not CHURN its store is `2 * TW_MAX` copy pairs + 2 sprite pairs + 1 glyph.

   THIS IS A SIZE AND NOT A BOUND. `tw_drop` deliberately leaves a claim
   standing (see it), so what a frame spends is DISTINCT VIEWS CLAIMED, and a
   present that batches FREE + SEED + COPY claims one per surface GENERATION --
   which nothing bounds by TW_MAX. What IS by construction is the consequence:
   `set_claim` answers 0 deterministically, the replay goes to `standdown`,
   `behind` drops OUR store and asks once, and nothing of the producer's is
   touched beyond that ask. Sized for the case that has to work, degrading predictably past
   it. */
#define SET_MAX     (2 * TW_MAX + 3)
/* objects waiting for every slot to turn over once before they are destroyed.
   The seam waits `fence[slot]` and nothing more, so `slots - 1` earlier
   submissions are still executing -- and the command buffer being recorded
   right now already names the objects too. Destroying a twin inside `prepare`
   is a use-after-free on the FIRST eviction, not a rare one. tagpu_vk_terr.c's
   slot bitmask and tagpu_vk_unit.c's `ret_push` are the same mechanism. */
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
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdPipelineBarrier)

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
    /* ---- CLASSIC++: THE COLOUR TWIN, made by the first op that
       carries `TAGPU_GUICOL_DST` (the drain's `twin_sprite` or `twin_copy`
       decides it).
       It is a SECOND ATTACHMENT of the same draws and not a second pass: one
       MRT draw writes the index and the colour together, so the two can never
       disagree about what a texel holds -- which is the whole reason it is an
       attachment rather than a second program.
       A framebuffer is immutable, so a twin that gains colour gains a SECOND
       framebuffer over both views and every later draw on it uses that one
       and `s_twRp2`.
       `colLayout` is tracked apart from `layout`: the two images are barriered
       together at every point they are used together, but a colour twin made
       mid-frame starts UNDEFINED while its index twin is already somewhere. */
    VkImage         colImg;
    VkDeviceMemory  colMem;
    VkImageView     colView;
    VkFramebuffer   fb2;            /* [index, colour], against s_twRp2      */
    VkImageLayout   colLayout;
    int             colNeedClear;   /* colour made this frame, not cleared   */
    /* ---- CLASSIC++: THE PICTURE THIS TWIN WAS SEEDED FROM (the picture
       store's entry + 1), pinned in the store from the SEED until the twin's
       next SEED or its drop. While `picDrawn` is 0 the twin IS the picture,
       and the store fills its colour plane whole when the entry's restore
       lands; the first op that names the twin as a destination sets it,
       because from then on the twin holds pixels the picture does not
       describe. THE PIN OUTLIVES THE DRAW on purpose: a snapshot's re-seed
       (tagpu_gui_hook.c `repaint_arm`) comes back for the same picture, and
       finds it stored and restored only because nothing could evict it in
       between. `picGen` is the store generation the fill was made in;
       0 = owed. */
    int             pic;
    int             picDrawn;
    unsigned        picGen;
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

    /* THE SHARP LAYER IS PER SLOT, and that is the whole of its
       synchronisation argument. A twin had to be shared because it ACCUMULATES
       across frames; this one is cleared to (0,0,0,0) at every present, so it
       holds nothing that outlives its frame and the seam's own `fence[slot]`
       wait already covers it. None of 2.34's shared-state reasoning applies. */
    VkImage         shImg;
    VkDeviceMemory  shMem;
    VkImageView     shView;
    VkFramebuffer   shFb;
    int             shW, shH;

    VkBuffer        sb;             /* the sharp draws' own 16-byte blocks   */
    VkDeviceMemory  sbMem;
    unsigned char*  sbMap;
    VkDeviceSize    sbCap;
    VkDescriptorSet shSet[3];       /* one per KIND: flat, cursor, minimap   */
    int             built;
} SLOT;

/* the three kinds, as set indices */
#define SDSET_FLAT   0
#define SDSET_CURS   1
#define SDSET_MM     2

static int              s_state, s_downOwed, s_downPaying;
static int              s_drawThis, s_abFrame;
static int              s_saidColour, s_saidSharp, s_saidRoom;

static TWIN             s_tw[TW_MAX];
static int              s_ntw;

static VkRenderPass     s_twRp;          /* one R8G8 colour attachment, LOAD  */
/* ...and the Classic++ edition: R8G8 plus the RGBA8 colour twin, both LOAD.
   A framebuffer is immutable, so a twin with one attachment and a twin with
   two need two framebuffers and two render passes. */
static VkRenderPass     s_twRp2;
static VkDescriptorSetLayout s_dslTwin, s_dslLay;
static VkPipelineLayout s_ploTwin, s_ploLay;
static VkPipeline       s_pipeSpr, s_pipeCpy, s_pipeStr, s_pipeLay;
static VkPipeline       s_pipeSpr2, s_pipeCpy2, s_pipeStr2;
/* THE FOCUS TINT. One more pair against the same two passes and
   the same layout -- `TINT_FS` samples at bindings 40 and 41 and reads one int
   at 32, which is `CPY_FS`'s shape exactly, so nothing about the descriptor
   set layout or the pipeline layout changes for it.
   `s_pipeTintFit` is the colour edition that shades the colour plane
   (`tint_table`); `s_pipeTint2` drops it, for a table made with no palette. */
static VkPipeline       s_pipeTint, s_pipeTint2, s_pipeTintFit;
/* THE SHARP LAYER. Its three programs share one layout -- all three have a
   16-byte block at binding 32 and CURS/MM three samplers at 40..42 -- so one
   descriptor set layout serves them and `SDSET_*` indexes one pre-written set
   per KIND rather than one per draw: a frame's up-to-16 quads sample at most
   three distinct combinations of images, and the per-draw uniform window is
   reached with a dynamic offset. */
static VkRenderPass     s_sharpRp;       /* one RGBA8 attachment, CLEAR       */
static VkDescriptorSetLayout s_dslSharp;
static VkPipelineLayout s_ploSharp;
static VkPipeline       s_pipeCurs, s_pipeMM, s_pipeFlat;
static VkRenderPass     s_layRp;         /* what s_pipeLay was built against  */
static VkDescriptorPool s_dpool;
static VkSampler        s_samp;
/* the one exception: the minimap picture, minified LINEAR (MM_FS says why) */
static VkSampler        s_sampMin;
static VkDeviceSize     s_ualign;

/* the shared texels: one of each, not one per slot (2.28's cheaper design --
   they are re-uploaded whole when their serial moves and read by every slot) */
static VkImage          s_atImg, s_palImg, s_dumImg, s_glImg;
static VkDeviceMemory   s_atMem, s_palMem, s_dumMem, s_glMem;
static VkImageView      s_atView, s_palView, s_dumView, s_glView;
static int              s_atDim, s_glW, s_glH;
static unsigned         s_atSerial, s_palSerial, s_glSerial;
static int              s_atHave, s_palHave, s_dumReady, s_glHave;
/* THE REMAP TABLE: 256 wide by `TAGPU_GUI_SHADE_ROWS` tall in its upper half,
   the index in `.r`, so `TINT_FS`'s `texelFetch(uShade, ivec2(index, row))`
   is the engine's own `LUT[row * 256 + index]` with no arithmetic in between;
   the lower half is each entry's map of colour (`tint_table`). Uploaded on a
   serial change, like the atlas and the palette. */
static VkImage          s_lutImg;
static VkDeviceMemory   s_lutMem;
static VkImageView      s_lutView;
static unsigned         s_lutSerial;
static int              s_lutHave;
/* the palette serial the colour half was made against, and whether there was
   one: without it the colour half is zeros and a colour twin's tint takes the
   pipeline that does not blend (`s_pipeTint2`), dropping the colour as an
   indexed tint must */
static unsigned         s_lutPal;
static int              s_fitHave;
/* ...AND THE SNAPSHOT A TINT READS. A tint is a read-modify-write of the twin
   it draws into, and sampling an attachment a draw is writing is undefined, so
   the box is copied out to this image first and the draw samples the copy.

   AT THE SAME COORDINATES, which is why it is sized to the largest twin any
   tint has landed on rather than to the largest BOX: `TINT_FS` indexes it with
   `gl_FragCoord` and there is no offset uniform to get wrong. One image for
   the module, grown and never shrunk -- 640x480 in the shell, 614 KB, and the
   shell is the only place tints occur at all.

   IT IS NOT DOUBLE-BUFFERED AND DOES NOT NEED TO BE, and not because of "one
   command buffer", which is false: the image is module-wide,
   so every one of `d->slots` in-flight frames records against it and frame
   N+1's copy is a DIFFERENT command buffer from frame N's draw. What actually
   orders them is that `vkCmdPipelineBarrier`'s first scope covers everything
   submitted earlier on the queue, not merely everything earlier in the same
   buffer -- so the TRANSFER_DST transition that opens tint n+1's copy waits on
   frame N's fragment reads wherever they were recorded. The write-after-read
   between consecutive tints is covered by that and by nothing else, and it
   holds only because the layout genuinely alternates (TRANSFER_DST <-> SHADER
   READ_ONLY), which keeps `lay_to`'s `*cur == to` early-out from eliding the
   barrier. It is still an ordering inside a queue rather than a timing
   argument -- but it is a submission-order one, and a future edit that made
   the layout idempotent would silently remove it. */
static VkImage          s_tintImg;
static VkDeviceMemory   s_tintMem;
static VkImageView      s_tintView;
static int              s_tintW, s_tintH;
static VkImageLayout    s_tintLay;
static unsigned         s_tintOps;
/* ---- THE RESTORED UI ATLAS, PAINTED RATHER THAN READ BACK. The UI atlas
   publishes a frame LIST, exactly as the feature and effects atlases do
   (tagpu_gui.h `restoreFrames`), and this pass paints `s_arImg` from `s_atImg`
   with `tagpu_vk_restore.c`. Nothing is read back: what crosses the
   hand-over is the work.

   `s_arHave` IS THE ONLY THING THAT SAYS THIS IS A PICTURE, and it is what
   `tagpu_gui_col_ready` reports back to the producer -- so an op cannot claim
   its texels are restored before there is something restored to sample. One
   painted frame is enough to raise it: the rest of the atlas is alpha 0, which
   `SPR_FS` reads as "no colour here" and falls back to the palette, per texel.
   16 MB at the shipped 2048 square, and only in a session that restores. */
static VkImage          s_arImg;
static VkDeviceMemory   s_arMem;
static VkImageView      s_arView;
static int              s_arDim;
static int              s_arHave;
/* HOW OFTEN THE RESTORE HAS GONE QUIET HAVING PAINTED SOMETHING NEW, reported
   to the producer so it can ask the engine for one repaint each time -- see
   `tagpu_gui_col_ready` in tagpu_gui.h for why a twin needs it. */
static unsigned         s_arSettled;
/* the job that paints it, and the cursor into the producer's list. The shape
   is `tagpu_vk_feat.c`'s, field for field, and so are the rules -- the
   generation is the only thing that restarts the cursor, and `s_rjTried`
   latches a device that refused so the ask is not repeated every frame. */
static TAGPU_VKRJOB*    s_rjob;
static unsigned         s_rjGen;
static int              s_rjTaken, s_rjPainted;
static unsigned         s_rjBlanks;
static VkImageView      s_rjSrcView;
static int              s_rjTried;
/* the generation `s_arImg` holds a restore of, left by a job the restorer's
   going down took away (`tagpu_vk_gui_restore_drop`); 0 = none */
static unsigned         s_rjKeepGen;
/* the last `colRearm` seen: when it moves, every colour twin was invalidated */
static unsigned         s_colRearm;
static int              s_colRearmSeen;
/* the minimap's two: the TNT picture (shared, keyed on its content serial --
   it moves on a map load and a palette change) and the ENGINE's own pair, which
   moves every frame by definition and so needs no serial, exactly as the
   engine's frame does. Both arrive RGB8 and are widened to RGBA8 on the way in:
   `VK_FORMAT_R8G8B8_UNORM` is optional and rarely supported, the four-channel
   one is universal, and MM_FS reads `.rgb` either way. */
static VkImage          s_mmPicImg, s_mmEngImg;
static VkDeviceMemory   s_mmPicMem, s_mmEngMem;
static VkImageView      s_mmPicView, s_mmEngView;
static int              s_mmPicW, s_mmPicH, s_mmEngW, s_mmEngH;
static unsigned         s_mmPicSerial;
static int              s_mmPicHave, s_mmEngHave;

static SLOT             s_slot[TAGPU_VK_SLOTS];

/* TWO OBJECT SETS PER ENTRY, AND THE PAIRING IS THE POINT rather than a way
   to save entries. A colour twin's framebuffer names BOTH views, so the two
   images have to be retired as one thing: split across two entries the sweep
   below would run in index order and could destroy an attachment's view while
   the framebuffer naming it was still alive. Within an entry the order is
   fixed -- both framebuffers, then the views, then the images, then the
   memory -- so that can never arise. */
typedef struct {
    VkImage         img[2];
    VkDeviceMemory  mem[2];
    VkImageView     view[2];
    VkFramebuffer   fb[2];
    unsigned        pending;        /* bit per slot still to turn over       */
} RET;
static RET              s_ret[RET_MAX];

/* THE STORE MAY BE BEHIND THE PRODUCER'S, and that is a state rather than an
   accident. Any path that cannot apply a frame's ops sets it: the twins are
   dropped, the producer is asked for a fresh start, and nothing is composited
   until its RESET arrives -- applying ops to a store that has missed some is a
   wrong picture that every later frame inherits. */
static int              s_behind, s_cantReplay;
/* RESEEDS RAISED THIS SESSION. `behind` asks once on the transition, but the
   transition REPEATS: a RESET clears `s_behind`, and a condition that is
   structural rather than transient fires again inside the same frame that
   answered it -- so "once on the transition" is once PER FRAME for exactly the
   conditions that never go away. That is a reseed storm (see `behind_ex`).
   After this many fruitless asks the pass stops asking: it keeps its store
   dropped and composites nothing, which is a capability statement, and the
   log says so. */
#define BEHIND_ASKS_MAX 8
/* ...and the cap counts FRUITLESS asks, not asks. A map change is a legitimate
   reason to fall behind once, and a long session can hold several; muting the
   pass for good after eight of those would be the cure killing the patient.
   A run of frames that actually composited is the evidence the last ask WORKED,
   so it gives the budget back. A structural condition never earns that run,
   because it stands the composite down on every frame. */
#define BEHIND_GOOD_RUN 120
static int              s_behindAsks, s_behindMute, s_goodRun;
static VkImageView      s_setView[SET_MAX], s_setView2[SET_MAX];

/* this frame's plan, filled by `prepare` and read by `record` */
static unsigned         s_presented;
static int              s_surfW, s_surfH;
static VkDeviceSize     s_uStride, s_fStride;
static VkDeviceSize     s_layQuad;       /* the composite's quad in the vb    */
static int              s_layVp[4];      /* the frame's viewport, y from below */

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

/* move one image into `to`, from whatever it is in now */
static void lay_to(VkCommandBuffer cb, VkImage img, VkImageLayout* cur, VkImageLayout to)
{
    VkPipelineStageFlags ss, ds;
    VkAccessFlags sa, da;
    if (*cur == to) return;
    switch (*cur) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        ss = VK_PIPELINE_STAGE_TRANSFER_BIT; sa = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        ss = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        sa = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        ss = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; sa = VK_ACCESS_SHADER_READ_BIT; break;
    /* TRANSFER_SRC (the focus tint's) needs a case on BOTH sides rather than
       the `default`. Leaving from it through the default would give
       `TOP_OF_PIPE`, which waits for nothing -- so the write
       that follows a tint's snapshot copy could be ordered before the copy's
       own READ of the same image (a write-after-read hazard, the one kind an
       execution dependency alone fixes and a missing one silently corrupts).
       Arriving at it through the default would name the FRAGMENT stage for
       what is a transfer. */
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        ss = VK_PIPELINE_STAGE_TRANSFER_BIT; sa = VK_ACCESS_TRANSFER_READ_BIT; break;
    default:
        ss = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; sa = 0; break;
    }
    switch (to) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        ds = VK_PIPELINE_STAGE_TRANSFER_BIT; da = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        ds = VK_PIPELINE_STAGE_TRANSFER_BIT; da = VK_ACCESS_TRANSFER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        ds = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        da = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT; break;
    default:
        ds = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; da = VK_ACCESS_SHADER_READ_BIT; break;
    }
    img_barrier(cb, img, *cur, to, ss, sa, ds, da);
    *cur = to;
}

/* THE TWO IMAGES OF A TWIN MOVE TOGETHER, and there is no case in this module
   where they do not want to: they are attachments of the same draw, samplers of
   the same copy and layers of the same composite. Keeping one layout variable
   each rather than one shared is only because a colour twin made mid-frame
   starts UNDEFINED while the index twin it joins is already somewhere. */
static void tw_to(VkCommandBuffer cb, TWIN* t, VkImageLayout to)
{
    lay_to(cb, t->img, &t->layout, to);
    if (t->colImg) lay_to(cb, t->colImg, &t->colLayout, to);
}

/* ---- the retire --------------------------------------------------------- */

/* this slot has turned over: whatever was waiting on it is one step closer */
static void ret_kill(const TAGPU_VKPASS* d, RET* r)
{
    int k;
    for (k = 0; k < 2; k++) if (r->fb[k])   vkDestroyFramebuffer(d->dev, r->fb[k], NULL);
    for (k = 0; k < 2; k++) if (r->view[k]) vkDestroyImageView(d->dev, r->view[k], NULL);
    for (k = 0; k < 2; k++) if (r->img[k])  vkDestroyImage(d->dev, r->img[k], NULL);
    for (k = 0; k < 2; k++) if (r->mem[k])  vkFreeMemory(d->dev, r->mem[k], NULL);
    memset(r, 0, sizeof *r);
}

static void ret_slot_done(const TAGPU_VKPASS* d, uint32_t slot)
{
    int i;
    for (i = 0; i < RET_MAX; i++) {
        if (!s_ret[i].pending) continue;
        s_ret[i].pending &= ~(1u << slot);
        if (s_ret[i].pending) continue;
        ret_kill(d, &s_ret[i]);
    }
}

/* Hand an object set to the retire, or say the list is full. EVERY slot's bit
   is set, the one being recorded included: that slot's submit is the one most
   certainly naming these handles, and its bit clears when it comes round
   again -- which is after its fence. A full list is not a reason to destroy
   anything here; it is a reason to stop. */
static int ret_push2(const TAGPU_VKPASS* d,
                     VkImage img0, VkDeviceMemory mem0, VkImageView view0, VkFramebuffer fb0,
                     VkImage img1, VkDeviceMemory mem1, VkImageView view1, VkFramebuffer fb1)
{
    int i;
    if (!img0 && !mem0 && !view0 && !fb0 && !img1 && !mem1 && !view1 && !fb1) return 1;
    for (i = 0; i < RET_MAX; i++) {
        if (s_ret[i].pending) continue;
        s_ret[i].img[0] = img0; s_ret[i].mem[0] = mem0;
        s_ret[i].view[0] = view0; s_ret[i].fb[0] = fb0;
        s_ret[i].img[1] = img1; s_ret[i].mem[1] = mem1;
        s_ret[i].view[1] = view1; s_ret[i].fb[1] = fb1;
        s_ret[i].pending = (d->slots >= 32) ? 0xFFFFFFFFu
                                            : ((1u << d->slots) - 1u);
        return 1;
    }
    return 0;
}

static int ret_push(const TAGPU_VKPASS* d, VkImage img, VkDeviceMemory mem,
                    VkImageView view, VkFramebuffer fb)
{
    return ret_push2(d, img, mem, view, fb,
                     VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

/* only ever called behind the seam's vkDeviceWaitIdle */
static void ret_drain_idle(const TAGPU_VKPASS* d)
{
    int i;
    for (i = 0; i < RET_MAX; i++) ret_kill(d, &s_ret[i]);
}

/* ---- CLASSIC++: THE PICTURE STORE ----------------------------------------
   A box of indices -- a decoded asset's SEED (a backdrop PCX, the sidebar
   plate) or a transformed stamp's PIXELS -- reaches its twin as bytes, not as
   a draw out of the UI atlas, so the atlas's restore never sees it and the
   box stayed at palette colour. This store restores those bytes: each distinct
   picture is written once into `s_psSrc` (R8 indices), restored by ONE job of
   its own into `s_psDst` (RGBA8), and copied into the colour plane of the box
   it was drawn to. The job is the restorer's sixth and last (tagpu_restore_core.h
   `TAGPU_R_MAXJOBS`), which is why the pictures share one pair of images
   rather than taking a job each.

   KEYED BY CONTENT AND CHECKED BYTE FOR BYTE. A hit compares the whole
   picture against the bytes the entry was made from, so a hash collision
   costs a second entry and can never lend a picture another's colour.

   PLACED ON A GRID OF `PS_CELL` CELLS and never moved: an entry keeps its
   rectangle until it is evicted, so a fill already recorded this frame reads
   the texels it was recorded against. Eviction takes the least recently used
   entry that no twin pins, and `tagpu_vk_restore_job_remap` removes its
   frames from the job BEFORE its cells are handed out -- including a batch in
   flight, which goes back to the queue -- so the job can never paint an
   evicted picture over the one that replaced it.

   LARGER THAN THE RESTORER'S SLOT, CUT INTO OVERLAPPING TILES. The core
   refuses a frame over `TAGPU_R_ACTMAX` (512) on either edge, and a shell
   backdrop is 640 wide. Each tile is restored with `PS_MARGIN` texels of the
   picture's own context past its core, and only the core is copied out: the
   model's receptive radius is its depth (tagpu_restore_glsl.h, 12 layers of
   3x3), so a core texel never sees the zero padding at a tile's cut edge and
   the tiled result is the whole-picture result exactly. The tiles lie side
   by side, which is why an entry's rectangle is wider than its picture --
   and they lie side by side in BOTH images, the overlap written twice into
   `s_psSrc`, because the restorer's OUT pass reads a texel's own colour from
   the source at the DESTINATION's coordinates (tagpu_restore_glsl.h
   `TAGPU_RESTORE_OUT_FS`): the two images must share one layout. */
#define PS_W       2048
#define PS_H       1536
#define PS_CELL    64
#define PS_CW      (PS_W / PS_CELL)
#define PS_CH      (PS_H / PS_CELL)
#define PS_MAX     256              /* entries                              */
#define PS_TMAX    16               /* tiles per picture                    */
#define PS_TILE    512              /* TAGPU_R_ACTMAX                       */
#define PS_MARGIN  16               /* > the model's receptive radius of 12 */
#define PS_CORE    (PS_TILE - 2 * PS_MARGIN)
/* tagpu_gui_surf.c's UI_RESTORE_MIN: below the model's receptive field there
   is nothing to restore, and the atlas keeps such frames indexed too */
#define PS_MIN     12
/* recently evicted content, so that restoring it again does not count as a
   settle -- see `ps_recent` */
#define PS_EVRING  64

enum { PSE_FREE = 0, PSE_NEW, PSE_QUEUED, PSE_DONE, PSE_BAD };

typedef struct {
    int              state;          /* PSE_*                               */
    unsigned         hash;           /* pic_hash: a filter, not the key     */
    int              w, h;           /* the picture                         */
    int              cx, cy, cw, ch; /* its cells                           */
    int              nt;             /* tiles                               */
    TAGPU_RGLSL_FRAME f[PS_TMAX];    /* the job's frames, one per tile      */
    /* per tile: where its core sits in s_psDst, where in the picture, and
       its size -- the copy regions of a fill */
    short            core[PS_TMAX][6];
    unsigned char*   bytes;          /* w*h, what a hit is compared against */
    unsigned         used;           /* s_psFrame at the last lookup        */
    int              pins;           /* twins seeded from it (TWIN `pic`)   */
    int              fresh;          /* its settle may ask for a repaint    */
} PSENT;

static PSENT            s_ps[PS_MAX];
static unsigned short   s_psCell[PS_CH][PS_CW];  /* entry index + 1, or 0  */
static int              s_psCellsFree = PS_CW * PS_CH;
static VkImage          s_psSrc, s_psDst;
static VkDeviceMemory   s_psSrcMem, s_psDstMem;
static VkImageView      s_psSrcView, s_psDstView;
static VkImageLayout    s_psSrcLay;
/* THE DESTINATION HAS BEEN MADE READY BY A JOB, so a later job may repaint
   over it rather than clear it: `tagpu_vk_restore_job_new`'s `repaint` names
   SHADER_READ_ONLY as the image's layout, which is only true after the first
   job's own clear. */
static int              s_psDstLive;
static TAGPU_VKRJOB*    s_psJob;
static unsigned char    s_psPal[256 * 4];         /* the job's palette       */
static int              s_psPalHave;
static int              s_psTried;                /* the device refused      */
/* moves when every fill made so far stops being current: a palette rebuild */
static unsigned         s_psGen = 1;
static unsigned         s_psFrame, s_psSettled;
static int              s_psBound;                /* twins a fill can reach:
                                                     `pic` set, `picDrawn` 0 */
static unsigned         s_psEv[PS_EVRING];
static unsigned         s_psEvN;
static unsigned         s_psStored, s_psEvicted, s_psFills, s_psRefused;
static int              s_psSaid;

static void pic_unbind(TWIN* t)
{
    if (!t->pic) return;
    if (t->pic >= 1 && t->pic <= PS_MAX && s_ps[t->pic - 1].pins > 0) s_ps[t->pic - 1].pins--;
    if (!t->picDrawn && s_psBound > 0) s_psBound--;
    t->pic = 0; t->picDrawn = 0; t->picGen = 0;
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
    /* BOTH FRAMEBUFFERS AND BOTH IMAGES IN ONE ENTRY -- `fb2` names `view` as
       well as `colView`, so retiring them apart would let the sweep destroy one
       of its attachments first (see RET). */
    if (!ret_push2(d, t->img, t->mem, t->view, t->fb,
                   t->colImg, t->colMem, t->colView, t->fb2)) return 0;
    pic_unbind(t);
    /* THE CLAIM IS DELIBERATELY LEFT STANDING. `tw_drop` only RETIRES, so the
       view stays alive for the whole frame and no handle can be recycled
       inside it -- while clearing `s_setView` here would free the set index
       for `set_claim` to hand out again and REWRITE a set a draw recorded
       earlier this frame already names. The per-frame memset is what bounds
       this. */
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
    /* TRANSFER_SRC IS THE FOCUS TINT'S, and the only usage the tint needs: a
       tint copies the box it is about to rewrite out to `s_tintImg` first,
       because a draw may not sample the attachment it writes. Nothing else in
       this file reads a twin by transfer. */
    if (!mk_image(d, w, h, VK_FORMAT_R8G8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT |
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
    /* A FRESH TWIN IS CLEARED, so a texel no seed covers is EMPTY, coverage 0
       -- what tagpu_gui_surf.c says a never-seeded twin is. Ours would
       otherwise load whatever device memory it was handed, through a
       LOAD_OP_LOAD render pass, and keep it. Cheap, and it removes the question
       rather than resting on "the producer always sends the bytes". */
    t->needClear = 1;
    s_ntw++;
    return t;
}

/* A TWIN CREATED THIS FRAME IS CLEARED BEFORE ANYTHING READS OR LOADS IT.
   It must be, or the LOAD_OP_LOAD render pass loads
   whatever memory the allocator handed us and keeps it in the twin. Done at
   the first touch rather than at creation, because creation happens outside a
   command buffer. */
static void tw_fresh(VkCommandBuffer cb, TWIN* t)
{
    VkClearColorValue cv;
    VkImageSubresourceRange rg;
    if (!t->needClear && !t->colNeedClear) return;
    memset(&cv, 0, sizeof cv);
    memset(&rg, 0, sizeof rg);
    rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.levelCount = 1; rg.layerCount = 1;
    tw_to(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (t->needClear) {
        t->needClear = 0;
        vkCmdClearColorImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &cv, 1, &rg);
    }
    /* AND THE COLOUR TWIN IS CLEARED TO ALPHA 0 THE SAME WAY, because
       alpha 0 is the restorer's own "nothing restored here" -- nothing is
       restored until an op says so (tagpu_gui_surf.c). A colour twin
       that loaded whatever memory it was handed would composite that memory as
       restored art wherever a byte of it read alpha > 0.5. */
    if (t->colNeedClear && t->colImg) {
        t->colNeedClear = 0;
        vkCmdClearColorImage(cb, t->colImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &cv, 1, &rg);
    }
}

/* THE COLOUR TWIN, MADE BY THE OP THAT ASKS FOR ONE. Never on this pass's
   own judgement: `TAGPU_GUICOL_DST` is what the drain decided (`twin_sprite`,
   `twin_copy`, tagpu_gui_surf.c), so the drain's table and this store hold
   colour for the same surfaces. 0 is a stand-down and not a shrug -- a twin
   the drain gave colour and this one did not composites indexed art under a
   `uColOn` that says otherwise. */
static int tw_colour(const TAGPU_VKPASS* d, TWIN* t)
{
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkImageView att[2];
    if (t->colImg) return 1;
    /* TRANSFER_SRC IS HERE BECAUSE `tw_to` MOVES BOTH PLANES. Nothing
       transfer-reads the colour image -- the tint snapshots only the index
       plane -- but `tw_to`'s contract is that the two images of a twin share a
       layout, so a tint on a twin that HAS colour transitions this one to
       TRANSFER_SRC_OPTIMAL as well, and a barrier to a layout the image was
       not created for is invalid (VUID-VkImageMemoryBarrier-newLayout-01208).
       Nothing on this lane makes a colour twin today
       (`twin_sprite`/`twin_copy` pass col == 0), so without the flag this
       would be a latent fault that fires the day Classic++ colour comes back,
       silently and far from its cause. */
    if (!mk_image(d, t->w, t->h, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                  &t->colImg, &t->colMem, &t->colView))
        return 0;
    att[0] = t->view; att[1] = t->colView;
    fi.renderPass = s_twRp2; fi.attachmentCount = 2; fi.pAttachments = att;
    fi.width = (uint32_t)t->w; fi.height = (uint32_t)t->h; fi.layers = 1;
    if (vkCreateFramebuffer(d->dev, &fi, NULL, &t->fb2) != VK_SUCCESS) {
        kill_image(d, &t->colImg, &t->colMem, &t->colView);
        return 0;
    }
    t->colLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->colNeedClear = 1;
    return 1;
}

/* INDICES ARRIVED FOR THE BOX AND THEY SAY NOTHING ABOUT
   COLOUR, so the colour there goes and the layer falls back to the palette.
   It is a one-attachment clear inside a render pass instance of its own, because
   `vkCmdClearAttachments` is the only rect clear Vulkan has and it needs one.
   Called from outside any open pass -- the seed and pixel ops are transfers. */
static void tw_col_drop(VkCommandBuffer cb, TWIN* t, int x, int y, int w, int h)
{
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearAttachment ca;
    VkClearRect cr;
    if (!t->colImg || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > t->w) w = t->w - x;
    if (y + h > t->h) h = t->h - y;
    if (w <= 0 || h <= 0) return;
    tw_to(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = 1;
    cr.rect.offset.x = x; cr.rect.offset.y = y;
    cr.rect.extent.width = (uint32_t)w; cr.rect.extent.height = (uint32_t)h;
    cr.layerCount = 1;
    rb.renderPass = s_twRp2; rb.framebuffer = t->fb2;
    rb.renderArea.extent.width = (uint32_t)t->w;
    rb.renderArea.extent.height = (uint32_t)t->h;
    vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    vkCmdEndRenderPass(cb);
}

/* ---- build -------------------------------------------------------------- */

/* `n` = 1 for an indexed twin, 2 for one that has gained its Classic++ colour
   attachment. The second is the SAME pass in every other respect -- same LOAD,
   same dependencies -- because it is the same draws writing one more output. */
static int build_rp_n(const TAGPU_VKPASS* d, int n, VkRenderPass* out)
{
    VkAttachmentDescription a[2];
    VkAttachmentReference ar[2];
    VkSubpassDescription sp;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    memset(a, 0, sizeof a); memset(ar, 0, sizeof ar);
    memset(&sp, 0, sizeof sp); memset(dep, 0, sizeof dep);
    a[0].format = VK_FORMAT_R8G8_UNORM;
    a[0].samples = VK_SAMPLE_COUNT_1_BIT;
    /* LOAD, NOT CLEAR: a twin accumulates. Every op writes part of it and the
       rest has to survive -- which is the whole of what a twin IS. */
    a[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[1] = a[0]; a[1].format = VK_FORMAT_R8G8B8A8_UNORM;
    ar[0].attachment = 0; ar[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ar[1].attachment = 1; ar[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = (uint32_t)n; sp.pColorAttachments = ar;
    /* the copy reads ANOTHER twin in the fragment stage while this one is the
       target, so the external dependencies name both scopes */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    /* TRANSFER_READ ON BOTH SIDES: the tint copies the attachment OUT
       (`vkCmdCopyImage` with the twin as source) before it draws into it, so
       a transfer read both precedes a pass and follows one. The explicit
       `tw_to` barriers are what actually order those; the declared dependency
       names the access as well, so it is not silent about a transfer that
       exists. */
    dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                           VK_ACCESS_TRANSFER_READ_BIT |
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
                           VK_ACCESS_TRANSFER_READ_BIT |
                           VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    ri.attachmentCount = (uint32_t)n; ri.pAttachments = a;
    ri.subpassCount = 1; ri.pSubpasses = &sp;
    ri.dependencyCount = 2; ri.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &ri, NULL, out) == VK_SUCCESS;
}

static int build_rp(const TAGPU_VKPASS* d)
{
    return build_rp_n(d, 1, &s_twRp) && build_rp_n(d, 2, &s_twRp2);
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

    /* the composite: LAY_FS binding 32, samplers 40..43. FOUR: the shader
       does not declare `uSurf`, TA's own composed frame, so binding it would
       be a set the pipeline has no use for. */
    memset(b, 0, sizeof b);
    b[0].binding = 32; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 0; i < 4; i++) {
        b[1 + i].binding = (uint32_t)(40 + i);
        b[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1 + i].descriptorCount = 1;
        b[1 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.bindingCount = 5; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &s_dslLay) != VK_SUCCESS) return 0;

    /* the sharp layer's three: QVS binding 0, their own 16-byte block at 32,
       and samplers 40..42. CURS wants (uAtlas, uAtlasRGB, uPal) and MM wants
       (uPic, uEng, uPal); FLAT reads none of them but a bound set must still be
       complete, so its three are the dummy. */
    memset(b, 0, sizeof b);
    b[0].binding = 0;  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 32; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 0; i < 3; i++) {
        b[2 + i].binding = (uint32_t)(40 + i);
        b[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2 + i].descriptorCount = 1;
        b[2 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    li.bindingCount = 5; li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &s_dslSharp) != VK_SUCCESS) return 0;

    pi.setLayoutCount = 1; pi.pSetLayouts = &s_dslTwin;
    if (vkCreatePipelineLayout(d->dev, &pi, NULL, &s_ploTwin) != VK_SUCCESS) return 0;
    pi.pSetLayouts = &s_dslSharp;
    if (vkCreatePipelineLayout(d->dev, &pi, NULL, &s_ploSharp) != VK_SUCCESS) return 0;
    pi.pSetLayouts = &s_dslLay;
    if (vkCreatePipelineLayout(d->dev, &pi, NULL, &s_ploLay) != VK_SUCCESS) return 0;
    return 1;
}

/* every quad is one vec4 attribute: x, y, u, v -- QVS's `a` */
static void quad_layout(VkVertexInputBindingDescription* vb,
                        VkVertexInputAttributeDescription* at)
{
    vb->binding = 0; vb->stride = 4 * sizeof(float);
    vb->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    at->location = 0; at->binding = 0;
    at->format = VK_FORMAT_R32G32B32A32_SFLOAT; at->offset = 0;
}

/* the twin pipelines. NO FLIP: the target is SAMPLED, not presented, and
   QVS puts quad y = 0 at attachment row 0 (the header). */
/* THE SHARP LAYER'S RENDER PASS: one RGBA8, CLEARED. The opposite of the
   twins' LOAD, and for the opposite reason -- `sharp_begin` clears this layer
   to (0,0,0,0) at every present, so nothing in it survives a frame and there is
   nothing to preserve. Its alpha is what the composite gates on, which is why
   the clear has to be transparent rather than merely black. */
static int build_sharp_rp(const TAGPU_VKPASS* d)
{
    VkAttachmentDescription a;
    VkAttachmentReference ar;
    VkSubpassDescription sp;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo ri = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    memset(&a, 0, sizeof a); memset(&sp, 0, sizeof sp); memset(dep, 0, sizeof dep);
    a.format = VK_FORMAT_R8G8B8A8_UNORM;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;   /* cleared, so never read  */
    a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ar.attachment = 0; ar.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    /* it SAMPLES the UI atlas, the palette and the minimap's two while it is
       the target, and the composite samples IT afterwards */
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_SHADER_READ_BIT;
    dep[1] = dep[0];
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ri.attachmentCount = 1; ri.pAttachments = &a;
    ri.subpassCount = 1; ri.pSubpasses = &sp;
    ri.dependencyCount = 2; ri.pDependencies = dep;
    return vkCreateRenderPass(d->dev, &ri, NULL, &s_sharpRp) == VK_SUCCESS;
}

/* `blend`: the tint's colour rule (TINT_FS), `src + dst * srcAlpha` with the
   destination's alpha kept. Every other twin draw writes whole. */
static int build_twin_pipe(const TAGPU_VKPASS* d, const uint32_t* fs, size_t fsw,
                           VkRenderPass rp, int natt, int blend, VkPipeline* out)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription at;
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba[2];
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
    memset(st, 0, sizeof st); memset(cba, 0, sizeof cba);
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
    /* blending OFF: a sprite discards its keyed texels and writes the rest
       whole, which is what coverage means here.
       THE COLOUR ATTACHMENT TAKES ALL FOUR CHANNELS, ALPHA ABOVE ALL: alpha is
       "this texel has restored colour" to every reader of a colour twin, so a
       write mask that dropped it would leave the flag standing wherever an op
       cleared the colour under it.
       AND THE TWO ATTACHMENTS' STATES ARE IDENTICAL, which the device requires
       without `independentBlend` (not enabled, tagpu_vk.c): the index
       attachment takes the same four-channel mask, and R8G8 has no B or A to
       write. The same holds for the tint's blend, which is a plain write of the
       index because `TINT_FS`'s `oIdx.a` is 0. */
    cba[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend) {
        cba[0].blendEnable = VK_TRUE;
        cba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba[0].colorBlendOp = VK_BLEND_OP_ADD;
        cba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    }
    cba[1] = cba[0];
    cb.attachmentCount = (uint32_t)natt; cb.pAttachments = cba;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds; gp.layout = s_ploTwin;
    gp.renderPass = rp; gp.subpass = 0;
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
    /* BLEND and DEPTH off: LAY_FS writes opaque colour where the mirror has
       coverage and discards everywhere else, so there is nothing to blend. The
       seam's render pass has a depth attachment, so a state must still be
       declared for it (tagpu_vk_pass.h) -- testing and writing both off. */
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

/* the same pipeline as the twins', with three differences and no more: it
   writes all four channels (the twins are RG8 and the composite gates on this
   one's ALPHA), it is built against the sharp render pass, and it takes the
   three-sampler layout. Blending stays OFF, as for the twins: a draw writes
   its texels whole, and the composite gates on the layer's alpha. */
static int build_sharp_pipe(const TAGPU_VKPASS* d, const uint32_t* fs, size_t fsw,
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
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds; gp.layout = s_ploSharp;
    gp.renderPass = s_sharpRp; gp.subpass = 0;
    r = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL, out);
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
    ps[0].descriptorCount = nslots * (SET_MAX * 2 + 3 * 2);   /* +3 sharp sets */
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = nslots * (SET_MAX * 2 + 5 + 3 * 3);
    ps[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ps[2].descriptorCount = nslots;
    pi.maxSets = nslots * (SET_MAX + 1 + 3);
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
        {
            VkDescriptorSetLayout sl[3];
            sl[0] = sl[1] = sl[2] = s_dslSharp;
            ai.descriptorSetCount = 3; ai.pSetLayouts = sl;
            if (vkAllocateDescriptorSets(d->dev, &ai, s_slot[i].shSet) != VK_SUCCESS) return 0;
        }
    }
    return 1;
}

static int build(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkPhysicalDeviceProperties pr;
    if (!resolve(d)) return 0;
    if (!d->flipok) return 0;          /* the composite needs it */
    /* THE DEVICE'S OWN NUMBER, not 256 assumed: the reference device answers
       64 (gpu-status 2.33). */
    vkGetPhysicalDeviceProperties(d->pd, &pr);
    s_ualign = pr.limits.minUniformBufferOffsetAlignment;
    if (!s_ualign) s_ualign = 4;
    /* EVERY TEXTURE THIS MODULE SAMPLES IS NEAREST -- WITH EXACTLY ONE
       EXCEPTION. The twin, the atlas, the palette, the engine's frame and the
       ramp's own texelFetch taps are all nearest. The MINIMAP PICTURE is not:
       it is sampled MIN linear, MAG nearest, and `MM_FS`'s own comment says
       why -- at the scales the minimap is drawn at the destination box is
       usually SMALLER than the 252-px picture, so the fragments take the
       MINIFICATION filter and a downsample wants one. (Not at every k: past k
       of about 2.4 the box is the bigger of the two and the magnification
       filter, nearest, is what runs -- "the blow-up at k > 2 stays crisp", as
       that comment puts it.) A linear minification blends four texels where a
       nearest sampler takes one, so this is a second sampler. MM_FS reaches
       the picture only where a 3x3 neighbourhood is unfogged. */
    si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f; si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(d->dev, &si, NULL, &s_samp) != VK_SUCCESS) return 0;
    si.minFilter = VK_FILTER_LINEAR;          /* MAG stays nearest (MM_FS)   */
    /* AND maxLod MUST LEAVE ROOM, OR minFilter IS DEAD CODE. Vulkan clamps the
       level-of-detail to [minLod, maxLod] and only THEN asks whether this is a
       magnification (lambda <= 0) or a minification -- so with both 0 the
       answer is always "magnification" and `magFilter` is always the one used.
       Left at the nearest sampler's 0.0, the whole second sampler would be a
       no-op. `tagpu_vk_unit.c` uses 0.25f here for the same reason. */
    si.maxLod = 0.25f;
    if (vkCreateSampler(d->dev, &si, NULL, &s_sampMin) != VK_SUCCESS) return 0;
    si.maxLod = 0.0f;
    if (!build_rp(d)) return 0;
    if (!build_layouts(d)) return 0;
    /* TWO OF EACH, one per render pass, and the SPIR-V is the same module for
       both: `SPR_FS`, `CPY_FS` and `STR_FS` all declare `layout(location=1) out`
       already -- against a one-attachment pass, a write to a location the
       subpass has no attachment for is discarded. So the colour edition is the
       same shader against a pass that HAS the second attachment. */
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_SPR_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_SPR_FS / 4,
                         s_twRp, 1, 0, &s_pipeSpr)) return 0;
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_CPY_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_CPY_FS / 4,
                         s_twRp, 1, 0, &s_pipeCpy)) return 0;
    /* the string shares the twin pipelines' layout exactly: QVS at binding 0,
       its own block at 32, one sampler at 40 */
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_STR_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_STR_FS / 4,
                         s_twRp, 1, 0, &s_pipeStr)) return 0;
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_SPR_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_SPR_FS / 4,
                         s_twRp2, 2, 0, &s_pipeSpr2)) return 0;
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_CPY_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_CPY_FS / 4,
                         s_twRp2, 2, 0, &s_pipeCpy2)) return 0;
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_STR_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_STR_FS / 4,
                         s_twRp2, 2, 0, &s_pipeStr2)) return 0;
    /* the tint, on the same two passes: it writes both locations exactly as
       the other three do, so the one-attachment edition simply discards the
       colour write. Only the colour edition blends -- it is the one with a
       colour plane to shade -- so an indexed twin's tint is the plain write
       it always was. */
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_TINT_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_TINT_FS / 4,
                         s_twRp, 1, 0, &s_pipeTint)) return 0;
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_TINT_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_TINT_FS / 4,
                         s_twRp2, 2, 0, &s_pipeTint2)) return 0;
    if (!build_twin_pipe(d, tagpu_spv_tagpu_gui_surf_TINT_FS,
                         sizeof tagpu_spv_tagpu_gui_surf_TINT_FS / 4,
                         s_twRp2, 2, 1, &s_pipeTintFit)) return 0;
    if (!build_sharp_rp(d)) return 0;
    if (!build_sharp_pipe(d, tagpu_spv_tagpu_gui_surf_SHARP_FS,
                          sizeof tagpu_spv_tagpu_gui_surf_SHARP_FS / 4, &s_pipeFlat)) return 0;
    if (!build_sharp_pipe(d, tagpu_spv_tagpu_gui_surf_CURS_FS,
                          sizeof tagpu_spv_tagpu_gui_surf_CURS_FS / 4, &s_pipeCurs)) return 0;
    if (!build_sharp_pipe(d, tagpu_spv_tagpu_gui_surf_MM_FS,
                          sizeof tagpu_spv_tagpu_gui_surf_MM_FS / 4, &s_pipeMM)) return 0;
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
    kill_buffer(d, &s->sb,    &s->sbMem, &s->sbMap);  s->sbCap = 0;
    /* the sharp layer is this slot's, so it goes back with the rest of it */
    if (s->shFb) { vkDestroyFramebuffer(d->dev, s->shFb, NULL); s->shFb = VK_NULL_HANDLE; }
    kill_image(d, &s->shImg, &s->shMem, &s->shView);
    s->shW = s->shH = 0;
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
   VALID whether or not the shader reads it -- a draw that never reads the
   colour twin or the sharp layer still needs an image for both. */
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
       at attachment row 0 -- the file header argues it. */
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)w; vp.height = (float)h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
}

/* (s_setView is declared with the pass's state, above. NOTE THAT `tw_drop`
   DELIBERATELY DOES NOT CLEAR A CLAIM whose view it is retiring; `tw_drop`
   carries the argument.)
   A TWIN-DRAW SET, CLAIMED FOR ONE IMAGE FOR THE LENGTH OF ONE FRAME.
   A set may not be rewritten once a recorded draw names it, so each distinct
   image a frame samples needs its own -- and the twin ARRAY cannot be the
   index, because `tw_drop` moves the last entry into the hole and the indices
   shuffle under it. So the claim is on the VIEW: reuse the set already holding
   it, else take a free one. SET_MAX is the twins plus TWO -- the UI atlas and
   the glyph atlas -- which covers any frame that does not churn its store; a
   frame that does can run out, and answering 0 here is how it says so. */
/* THE CLAIM IS ON THE PAIR, not on binding 40 alone. Binding 41 is not always
   the dummy: under Classic++ a sprite samples the restored atlas there and a
   copy its source's COLOUR twin, so two draws that agree about 40 and differ
   about 41 are two different sets. Keying on 40 alone would hand the second
   draw the first one's set and sample the wrong colour image -- with the right
   indices, so the picture would be right everywhere the colour twin happened
   to be empty. */
static int set_claim(const TAGPU_VKPASS* d, SLOT* s, VkImageView v, VkImageView v2,
                     VkDeviceSize uRange, VkDeviceSize fRange, int* out)
{
    int j, free_j = -1;
    VkDescriptorBufferInfo bi[2];
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet wr[4];
    if (!v2) v2 = s_dumView;
    for (j = 0; j < SET_MAX; j++) {
        if (s_setView[j] == v && s_setView2[j] == v2) { *out = j; return 1; }
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
    ii[1].sampler = s_samp; ii[1].imageView = v2;
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
    s_setView2[free_j] = v2;
    *out = free_j;
    return 1;
}

/* THE STORE IS BEHIND: drop it, ask the producer for the fresh start, and
   composite nothing until its RESET arrives. Every path that cannot apply a
   frame's ops goes through here, and there is exactly one way out -- which is
   what makes "our store is in step with the producer's" checkable rather than
   hoped for.
   A drop the retire will not take leaves the pass owing a teardown instead. */
/* `reask` -- THE PRESENT THAT WOULD HAVE CARRIED THE ANSWER WAS LOST, SO ASK
   AGAIN. `g_guiq.reseed` is ONE-SHOT: the producer clears it the instant it
   publishes the RESET (`tagpu_gui_hook.c`). If the present carrying that RESET
   is itself abandoned -- and it is the LIKELIEST one to be, a reseed present
   being every surface seeded at once and so the largest arena there is -- the
   answer never arrives, `s_behind` is already 1 so nothing asks again, and pass
   2 skips every op for the rest of the session waiting for a RESET that will
   never be sent. A lost frame therefore INVALIDATES an outstanding request.
   This is not "the window is small": it is a message provably not delivered,
   re-sent. */
static int behind_ex(const TAGPU_VKPASS* d, const char* why, int reask)
{
    /* THE REQUEST IS RAISED ONCE, ON THE TRANSITION, AND THAT IS NOT TIDINESS.
       `tagpu_gui_mirror_reseed` raises the PRODUCER's flag, and the producer
       answers it by dropping every `seeded` mark and re-seeding every surface —
       megabytes into a 16 MB arena. Raising it on every call, from a condition
       that holds every frame, reseeds the producer at the frame rate:
       `tagpu_gui_hook.c` calls that a reseed storm and measured 2 749 resets in
       one walk the last time something caused one. */
    if (!s_behind || reask) {
        s_behind = 1;
        s_goodRun = 0;
        if (s_behindMute) {
            /* asked already, as often as this is worth asking */
        } else if (++s_behindAsks > BEHIND_ASKS_MAX) {
            s_behindMute = 1;
            plog(d, "gui: %d fresh starts have not made the twin store able to "
                    "follow the producer (%s) - this is a capability gap and not "
                    "a sync one, so nothing further is asked of the producer "
                    "and nothing is composited", BEHIND_ASKS_MAX, why);
        } else {
            plog(d, "gui: the twin store cannot follow the producer (%s) - asking "
                    "the producer for a fresh start, once, and compositing nothing "
                    "until it arrives", why);
            tagpu_gui_mirror_reseed();
        }
    }
    if (!tw_reset(d)) return 0;      /* caller refuses: the retire is full */
    return 1;
}

/* the ordinary entry: a condition we noticed. The request stands until a RESET
   answers it, so asking again would be the storm. */
static int behind(const TAGPU_VKPASS* d, const char* why)
{
    return behind_ex(d, why, 0);
}

/* ---- CLASSIC++: THE RESTORED UI ATLAS ------------------------------------
   Give the restorer the rectangles the producer published and keep the cursor.
   `tagpu_vk_feat.c`'s `restore_want`, with the UI's own two differences: the
   atlas has no mip chain to register (`s_atlas.mip = 0` over there, the layer
   samples NEAREST), and the verdict is reported BACK to the producer, because
   this side is the only one that knows whether there is anything to sample.

   A FAILURE HERE IS NEVER FATAL TO THE UI. Every path that gives up clears
   `s_arHave`, the producer then stops setting `TAGPU_GUICOL_ON`, and the layer
   goes back to the indexed art -- which is a picture, not a stand-down. That
   is the whole reason colour is asked for through a back-channel rather than
   derived on each side. */
static void restore_want(const TAGPU_VKPASS* d, const TAGPU_GUIHAND* h)
{
    int repaint, n;

    /* THE IMAGE FOLLOWS THE INDEXED ATLAS'S SIZE, and a size change retires it
       rather than destroying it: an in-flight composite may still be sampling
       the old one. The job goes with it -- it names both views. */
    if (s_arImg && s_arDim != s_atDim) {
        if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
        s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0; s_rjSrcView = VK_NULL_HANDLE;
        if (!ret_push(d, s_arImg, s_arMem, s_arView, VK_NULL_HANDLE)) {
            tagpu_gui_col_ready(0, s_arSettled);
            return;                       /* the retire is full: ask next frame */
        }
        s_arImg = VK_NULL_HANDLE; s_arMem = VK_NULL_HANDLE; s_arView = VK_NULL_HANDLE;
        s_arDim = 0; s_arHave = 0;
    }

    if (!h->restoreFrames || h->restoreGen == 0) {
        /* No request: Classic++'s `assets=` is off, or the producer's list was
           dropped. Either way this job describes nothing now, and what it
           painted stops being a picture -- nothing else fills this image, so
           leaving `s_arHave` set would have the layer sample a frozen twin for
           the rest of the session. */
        if (s_rjob || s_rjKeepGen) {
            if (s_rjob) tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
            s_rjSrcView = VK_NULL_HANDLE; s_rjKeepGen = 0;
            s_arHave = 0;
        }
        tagpu_gui_col_ready(0, s_arSettled);
        return;
    }
    /* THE SOURCE MOVED UNDER A LIVE JOB: the indexed atlas was re-created at a
       new dimension, so the job reads a destroyed view. */
    if (s_rjob && s_rjSrcView && s_atView && s_rjSrcView != s_atView) {
        plog(d, "gui: the indexed UI atlas moved under a live restore - dropping "
                "it and starting over on the new one");
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
        s_rjSrcView = VK_NULL_HANDLE;
        s_arHave = 0;
    }
    if (s_rjob && s_rjGen == h->restoreGen) {
        int painted = tagpu_vk_restore_job_painted(s_rjob);
        if (painted > 0) s_arHave = 1;
        if (painted != s_rjPainted) {
            s_rjPainted = painted;
            if (tagpu_vk_restore_job_idle(s_rjob)) {
                s_arSettled++;
                plog(d, "gui: the UI atlas is restored HERE - %d frame(s) of "
                        "generation %u", painted, h->restoreGen);
            }
        }
        if (tagpu_vk_restore_job_failed(s_rjob)) {
            /* The rects move whenever the atlas is re-laid, so an abandoned
               restore stops being a picture now rather than when it starts
               looking wrong. */
            plog(d, "gui: the UI restore failed on this lane - the layer goes back "
                    "to indexed art");
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0;
            s_rjTried = 1;
            s_arHave = 0;
            tagpu_gui_col_ready(0, s_arSettled);
            return;
        }
        /* THE STEADY STATE: whatever the producer has appended since. The
           cursor advances by what was OFFERED and not by what was taken, or a
           frame the restorer can never queue is re-offered for ever; a
           whole-call failure (nothing taken at all) is the queue's own realloc
           and leaves the cursor where it is. */
        n = h->restoreN - s_rjTaken;
        if (n > 0) {
            int took = tagpu_vk_restore_job_add(s_rjob, h->restoreFrames + s_rjTaken, n);
            if (took > 0) {
                s_rjTaken += n;
                if (took < n)
                    plog(d, "gui: %d of %d new UI restore frames were refused by the "
                            "restorer - they stay indexed until the next generation",
                         n - took, n);
            }
        }
        tagpu_gui_col_ready(s_arHave, s_arSettled);
        return;
    }
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; s_rjTaken = 0; }
    if (s_rjTried) { s_arHave = 0; tagpu_gui_col_ready(0, s_arSettled); return; }
    /* THE SOURCE HAS TO EXIST AND HAVE CONTENTS: a restore over an atlas no
       copy has reached yet paints the palette's entry 0 over the art. Not an
       error -- the next frame asks again. */
    if (!s_atView || !s_atHave || s_atDim < 1) { tagpu_gui_col_ready(s_arHave, s_arSettled); return; }
    if (!s_arImg) {
        if (!mk_image(d, s_atDim, s_atDim, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                      &s_arImg, &s_arMem, &s_arView)) {
            /* `mk_image` can fail after the image and the memory were made, so
               hand both back rather than nulling the handles. */
            kill_image(d, &s_arImg, &s_arMem, &s_arView);
            s_rjTried = 1;
            plog(d, "gui: no %d MB device image for the restored UI atlas - the "
                    "layer stays indexed", (s_atDim * s_atDim * 4) >> 20);
            tagpu_gui_col_ready(0, s_arSettled);
            return;
        }
        s_arDim = s_atDim; s_arHave = 0;
    }
    /* THE CONSUMER IS WHAT ASKS THE DEVICE: `up` loads the model off disk, so a
       session that never restores never pays for it. It latches its verdict. */
    if (!tagpu_vk_restore_up(d)) {
        s_rjTried = 1; tagpu_gui_col_ready(0, s_arSettled); return;
    }
    /* A REPAINT ONLY OVER SOMETHING THIS PASS ACTUALLY PAINTED, and only when
       nothing was BLANKED since it last looked: `restoreRepaint` describes the
       latest generation alone, so a blanking generation followed in the same
       producer frame by a repainting one would otherwise hand this pass "keep
       what you have" over an image the producer had cleared. */
    repaint = h->restoreRepaint && s_arHave && h->restoreBlanks == s_rjBlanks;
    /* ...AND OVER THE SAME GENERATION'S OWN RESTORE when a resize took its job
       away: the image holds exactly what that job painted, so repainting
       keeps it on screen instead of blanking it, and colour validity does not
       drop -- which would spend one of the producer's repaints for each
       rebuild of a window being dragged. */
    if (s_rjKeepGen && s_rjKeepGen == h->restoreGen && s_arHave &&
        h->restoreBlanks == s_rjBlanks) repaint = 1;
    s_rjKeepGen = 0;
    s_rjob = tagpu_vk_restore_job_new(d, "gui", 4, 0, repaint,
                                      s_atImg, s_atView, s_atDim, s_atDim, 0,
                                      h->pal,
                                      s_arImg, s_arView, s_atDim, s_atDim);
    if (!s_rjob) { s_rjTried = 1; tagpu_gui_col_ready(0, s_arSettled); return; }
    s_rjTaken = tagpu_vk_restore_job_add(s_rjob, h->restoreFrames, h->restoreN);
    s_rjGen = h->restoreGen;
    s_rjBlanks = h->restoreBlanks;
    s_rjSrcView = s_atView;
    s_rjPainted = 0;
    if (!repaint) s_arHave = 0;           /* it is being blanked and repainted */
    plog(d, "gui: restoring the UI atlas HERE - %d of %d frames over %dx%d, "
            "generation %u%s", s_rjTaken, h->restoreN, s_atDim, s_atDim,
         h->restoreGen, repaint ? ", repaint" : "");
    tagpu_gui_col_ready(s_arHave, s_arSettled);
}

/* ---- CLASSIC++: THE PICTURE STORE'S MACHINERY (the store is described
   above `pic_unbind`) ---------------------------------------------------- */

/* A FILTER, NOT THE KEY: 32-bit FNV-1a over words, and every hit is then
   compared byte for byte (`pic_get`). */
static unsigned pic_hash(const unsigned char* p, int w, int h)
{
    unsigned x = 2166136261u ^ ((unsigned)w * 65599u + (unsigned)h);
    size_t i, n = (size_t)w * (size_t)h;
    for (i = 0; i + 4 <= n; i += 4) {
        unsigned v;
        memcpy(&v, p + i, 4);
        x = (x ^ v) * 16777619u;
    }
    for (; i < n; i++) x = (x ^ p[i]) * 16777619u;
    return x;
}

/* ONE AXIS CUT INTO TILES: `[t0, t1)` is what a tile restores, `[c0, c1)` the
   core copied out of it. The cores partition `[0, L)`, each at most
   `PS_CORE`, so a tile is at most `PS_TILE`. The count. */
static int ps_axis(int L, int* t0, int* t1, int* c0, int* c1)
{
    int n, i;
    if (L <= PS_TILE) { t0[0] = c0[0] = 0; t1[0] = c1[0] = L; return 1; }
    n = (L + PS_CORE - 1) / PS_CORE;
    if (n > PS_TMAX) return 0;
    for (i = 0; i < n; i++) {
        c0[i] = (int)((long long)L * i / n);
        c1[i] = (int)((long long)L * (i + 1) / n);
        t0[i] = c0[i] - PS_MARGIN; if (t0[i] < 0) t0[i] = 0;
        t1[i] = c1[i] + PS_MARGIN; if (t1[i] > L) t1[i] = L;
    }
    return n;
}

/* THE STAGING A NEW PICTURE'S TILES TAKE, each tile's rows packed and its
   start rounded to 4; 0 for a picture the store will not take. Pass 1
   reserves this for every op that may be a new picture. */
static VkDeviceSize ps_stage_need(int w, int h)
{
    int tx0[PS_TMAX], tx1[PS_TMAX], cx0[PS_TMAX], cx1[PS_TMAX];
    int ty0[PS_TMAX], ty1[PS_TMAX], cy0[PS_TMAX], cy1[PS_TMAX];
    int nx, ny, i, j;
    VkDeviceSize n = 0;
    if (w < PS_MIN || h < PS_MIN) return 0;
    nx = ps_axis(w, tx0, tx1, cx0, cx1);
    ny = ps_axis(h, ty0, ty1, cy0, cy1);
    if (!nx || !ny || nx * ny > PS_TMAX) return 0;
    for (j = 0; j < ny; j++)
        for (i = 0; i < nx; i++)
            n += align_up((VkDeviceSize)(tx1[i] - tx0[i]) * (VkDeviceSize)(ty1[j] - ty0[j]), 4);
    return n + 4;
}

/* the first free `cw` x `ch` rectangle of cells, row by row */
static int ps_fit(int cw, int ch, int* ox, int* oy)
{
    int x, y, i, j, ok;
    if (cw > PS_CW || ch > PS_CH) return 0;
    for (y = 0; y + ch <= PS_CH; y++)
        for (x = 0; x + cw <= PS_CW; x++) {
            ok = 1;
            for (j = 0; j < ch && ok; j++)
                for (i = 0; i < cw; i++)
                    if (s_psCell[y + j][x + i]) { ok = 0; break; }
            if (ok) { *ox = x; *oy = y; return 1; }
        }
    return 0;
}

/* the job's frames that lie in an evicted entry's cells go; the rest stay */
static int ps_keep_frame(void* ctx, TAGPU_RGLSL_FRAME* f)
{
    const PSENT* e = (const PSENT*)ctx;
    int x0 = e->cx * PS_CELL, y0 = e->cy * PS_CELL;
    return !(f->ax >= x0 && f->ax < x0 + e->cw * PS_CELL &&
             f->ay >= y0 && f->ay < y0 + e->ch * PS_CELL);
}

/* The job goes; what it had queued is queued again on the next one. */
static void ps_job_free(const TAGPU_VKPASS* d)
{
    int i;
    if (!s_psJob) return;
    if (tagpu_vk_restore_job_dst_live(s_psJob)) s_psDstLive = 1;
    tagpu_vk_restore_job_free(d, s_psJob);
    s_psJob = NULL;
    for (i = 0; i < PS_MAX; i++) if (s_ps[i].state == PSE_QUEUED) s_ps[i].state = PSE_NEW;
}

/* `remember` puts the content in the ring of recently evicted pictures, so
   that storing it again asks for no repaint -- see `tagpu_gui_pic_settled`.
   0 when the eviction is not about room. */
static void ps_evict(const TAGPU_VKPASS* d, PSENT* e, int remember)
{
    int x, y;
    /* THE JOB LETS GO OF IT FIRST, and that is what makes the cells safe to
       hand out: a frame of this entry still queued, or in the batch in flight,
       would otherwise paint it over whatever is written there next. A job that
       cannot take the remap is dropped whole, which is the same guarantee. */
    if (s_psJob) {
        int kept, requeued, dropped;
        if (!tagpu_vk_restore_job_remap(s_psJob, ps_keep_frame, e, &kept, &requeued, &dropped))
            ps_job_free(d);
    }
    for (y = 0; y < e->ch; y++)
        for (x = 0; x < e->cw; x++) s_psCell[e->cy + y][e->cx + x] = 0;
    s_psCellsFree += e->cw * e->ch;
    if (remember) s_psEv[s_psEvN++ % PS_EVRING] = e->hash;
    free(e->bytes);
    memset(e, 0, sizeof *e);
    s_psEvicted++;
}

static void ps_queue(PSENT* e)
{
    int took;
    if (!s_psJob) { e->state = PSE_NEW; return; }
    took = tagpu_vk_restore_job_add(s_psJob, e->f, e->nt);
    /* A PARTLY QUEUED PICTURE IS NEVER DONE: a tile left out would read as
       restored from whatever its cells held before. */
    e->state = (took == e->nt) ? PSE_QUEUED : PSE_BAD;
}

/* the pair of images, made on the first picture and kept for the session */
static int ps_images(const TAGPU_VKPASS* d)
{
    if (s_psSrc && s_psDst) return 1;
    if (!mk_image(d, PS_W, PS_H, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  &s_psSrc, &s_psSrcMem, &s_psSrcView) ||
        !mk_image(d, PS_W, PS_H, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                  &s_psDst, &s_psDstMem, &s_psDstView)) {
        kill_image(d, &s_psSrc, &s_psSrcMem, &s_psSrcView);
        kill_image(d, &s_psDst, &s_psDstMem, &s_psDstView);
        s_psTried = 1;
        plog(d, "gui: no device images for the picture store - backdrops and "
                "stamps stay indexed");
        return 0;
    }
    s_psSrcLay = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

/* The pictures in the store are restored against the palette the frame is
   presented with. */
static int pic_current(const TAGPU_GUIHAND* h)
{
    return h->pal && s_psPalHave && !memcmp(h->pal, s_psPal, sizeof s_psPal);
}

/* CONTENT AMONG THE LAST `PS_EVRING` EVICTED, whose store raises no settle
   when it lands: a repaint that evicts a picture of its own screen and draws
   it again must not ask for another repaint. It is what makes the loop
   converge on a screen larger than the store, and a filter rather than the
   bound -- a screen that evicts more than the ring holds between two draws
   of one picture gets past it. The bound is the drain's budget
   (tagpu_gui_surf.c `pic_ask_repaint`). Content evicted longer ago, a map
   preview picked again after a walk through the list, is fresh and asks for
   its repaint. */
static int ps_recent(unsigned hv)
{
    int i;
    for (i = 0; i < PS_EVRING && i < (int)s_psEvN; i++)
        if (s_psEv[i] == hv) return 1;
    return 0;
}

/* THE ENTRY FOR THIS PICTURE, stored now if it is new: its bytes go through
   the slot's staging into `s_psSrc` in this command buffer, ahead of the
   restore step that reads them (tagpu_vk.c runs the step last of every
   `prepare`). NULL when the picture is below the model's reach or the store
   cannot hold it -- it then stays indexed, which is the fallback and not a
   fault. */
static PSENT* pic_get(const TAGPU_VKPASS* d, VkCommandBuffer cb, SLOT* s,
                      VkDeviceSize* stOff, const unsigned char* idx, int w, int h)
{
    int tx0[PS_TMAX], tx1[PS_TMAX], cx0[PS_TMAX], cx1[PS_TMAX];
    int ty0[PS_TMAX], ty1[PS_TMAX], cy0[PS_TMAX], cy1[PS_TMAX];
    int nx, ny, dw = 0, dh = 0, cw, ch, ox = 0, oy = 0, i, j, k, slot = -1;
    int ex, ey, offy;
    unsigned hv;
    size_t n;
    PSENT* e;
    unsigned char* bytes;

    if (s_psTried || w < PS_MIN || h < PS_MIN) return NULL;
    n = (size_t)w * (size_t)h;
    hv = pic_hash(idx, w, h);
    for (i = 0; i < PS_MAX; i++) {
        e = &s_ps[i];
        if (e->state == PSE_FREE) { if (slot < 0) slot = i; continue; }
        if (e->hash == hv && e->w == w && e->h == h && !memcmp(e->bytes, idx, n)) {
            e->used = s_psFrame;
            return e;
        }
    }

    nx = ps_axis(w, tx0, tx1, cx0, cx1);
    ny = ps_axis(h, ty0, ty1, cy0, cy1);
    if (!nx || !ny || nx * ny > PS_TMAX) {
        s_psRefused++;
        if (!(s_psSaid & 1)) { s_psSaid |= 1;
            plog(d, "gui: a %dx%d picture needs more than %d restore tiles - it "
                    "stays indexed", w, h, PS_TMAX); }
        return NULL;
    }
    for (i = 0; i < nx; i++) dw += tx1[i] - tx0[i];
    for (j = 0; j < ny; j++) dh += ty1[j] - ty0[j];
    cw = (dw + PS_CELL - 1) / PS_CELL;
    ch = (dh + PS_CELL - 1) / PS_CELL;
    if (cw > PS_CW || ch > PS_CH) {
        s_psRefused++;
        if (!(s_psSaid & 2)) { s_psSaid |= 2;
            plog(d, "gui: a %dx%d picture is larger than the %dx%d picture store - "
                    "it stays indexed", w, h, PS_W, PS_H); }
        return NULL;
    }
    if (!ps_images(d)) return NULL;
    bytes = (unsigned char*)malloc(n);
    if (!bytes) return NULL;

    /* ROOM: the least recently used entry no twin pins goes, until a slot
       and a rectangle of cells are both free. Pinned entries are what a
       backdrop's fill, a snapshot's re-seed and a palette rebuild read, so
       they stay. */
    while (slot < 0 || s_psCellsFree < cw * ch || !ps_fit(cw, ch, &ox, &oy)) {
        int v = -1;
        for (i = 0; i < PS_MAX; i++) {
            if (s_ps[i].state == PSE_FREE || s_ps[i].pins) continue;
            if (v < 0 || s_ps[i].used < s_ps[v].used) v = i;
        }
        if (v < 0) {
            free(bytes);
            s_psRefused++;
            if (!(s_psSaid & 4)) { s_psSaid |= 4;
                plog(d, "gui: the picture store is full of pictures twins pin "
                        "- a %dx%d picture stays indexed", w, h); }
            return NULL;
        }
        ps_evict(d, &s_ps[v], 1);
        if (slot < 0) slot = v;
    }

    e = &s_ps[slot];
    memset(e, 0, sizeof *e);
    memcpy(bytes, idx, n);
    e->bytes = bytes;
    e->hash = hv; e->w = w; e->h = h;
    e->cx = ox; e->cy = oy; e->cw = cw; e->ch = ch;
    e->used = s_psFrame;
    for (j = 0; j < ch; j++)
        for (i = 0; i < cw; i++) s_psCell[oy + j][ox + i] = (unsigned short)(slot + 1);
    s_psCellsFree -= cw * ch;
    e->fresh = !ps_recent(hv);

    /* the tiles, side by side in the destination from the entry's origin */
    ex = ox * PS_CELL; ey = oy * PS_CELL;
    k = 0; offy = 0;
    for (j = 0; j < ny; j++) {
        int offx = 0;
        for (i = 0; i < nx; i++) {
            TAGPU_RGLSL_FRAME* f = &e->f[k];
            memset(f, 0, sizeof *f);
            f->w = tx1[i] - tx0[i]; f->h = ty1[j] - ty0[j];
            f->ax = f->dx = ex + offx;
            f->ay = f->dy = ey + offy;
            f->key = -1;                        /* a picture has no key */
            e->core[k][0] = (short)(f->dx + (cx0[i] - tx0[i]));
            e->core[k][1] = (short)(f->dy + (cy0[j] - ty0[j]));
            e->core[k][2] = (short)cx0[i];
            e->core[k][3] = (short)cy0[j];
            e->core[k][4] = (short)(cx1[i] - cx0[i]);
            e->core[k][5] = (short)(cy1[j] - cy0[j]);
            offx += f->w;
            k++;
        }
        offy += ty1[j] - ty0[j];
    }
    e->nt = k;

    /* EACH TILE'S BYTES WHERE ITS OUTPUT GOES, through this slot's staging
       (pass 1 reserved `ps_stage_need`) */
    lay_to(cb, s_psSrc, &s_psSrcLay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    k = 0;
    for (j = 0; j < ny; j++)
        for (i = 0; i < nx; i++) {
            const TAGPU_RGLSL_FRAME* f = &e->f[k++];
            unsigned char* dst = s->stMap + *stOff;
            int r;
            for (r = 0; r < f->h; r++)
                memcpy(dst + (size_t)r * f->w, idx + (size_t)(ty0[j] + r) * w + tx0[i], (size_t)f->w);
            copy_rect(cb, s->stage, *stOff, s_psSrc, f->ax, f->ay, f->w, f->h);
            *stOff = align_up(*stOff + (VkDeviceSize)f->w * (VkDeviceSize)f->h, 4);
        }
    lay_to(cb, s_psSrc, &s_psSrcLay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    ps_queue(e);
    s_psStored++;
    return e;
}

/* THE RESTORED PICTURE INTO A TWIN'S COLOUR PLANE, its top-left at (bx, by):
   one copy region per tile core. Only for a DONE entry, which a job has
   painted -- so `s_psDst` is in the SHADER_READ_ONLY layout the restorer
   leaves it in, and it is handed back in that layout for the restorer's next
   draw. The barrier into the copy names the OUT pass's writes itself rather
   than leaning on its render pass's outgoing dependency (the half of a
   dependency a driver can drop, tagpu_vk_restore.c `end_pass`). */
static void pic_fill(VkCommandBuffer cb, TWIN* t, const PSENT* e, int bx, int by)
{
    VkImageCopy rg[PS_TMAX];
    int k, n = 0;
    if (!t->colImg || e->state != PSE_DONE) return;
    for (k = 0; k < e->nt; k++) {
        int sx = e->core[k][0], sy = e->core[k][1];
        int px = bx + e->core[k][2], py = by + e->core[k][3];
        int w = e->core[k][4], h = e->core[k][5];
        if (px < 0) { sx -= px; w += px; px = 0; }
        if (py < 0) { sy -= py; h += py; py = 0; }
        if (px + w > t->w) w = t->w - px;
        if (py + h > t->h) h = t->h - py;
        if (w <= 0 || h <= 0) continue;
        memset(&rg[n], 0, sizeof rg[n]);
        rg[n].srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg[n].srcSubresource.layerCount = 1;
        rg[n].dstSubresource = rg[n].srcSubresource;
        rg[n].srcOffset.x = sx; rg[n].srcOffset.y = sy;
        rg[n].dstOffset.x = px; rg[n].dstOffset.y = py;
        rg[n].extent.width = (uint32_t)w; rg[n].extent.height = (uint32_t)h;
        rg[n].extent.depth = 1;
        n++;
    }
    if (!n) return;
    tw_to(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    img_barrier(cb, s_psDst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyImage(cb, s_psDst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   t->colImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)n, rg);
    img_barrier(cb, s_psDst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    s_psFills++;
}

/* ONCE PER PREPARE, before the replay: what the job finished, a palette that
   replaced the one the pictures were restored against, and the job itself. */
static void pic_step(const TAGPU_VKPASS* d, const TAGPU_GUIHAND* h)
{
    int i, k, any = 0, fresh = 0;
    s_psFrame++;
    if (s_psJob) {
        if (tagpu_vk_restore_job_dst_live(s_psJob)) s_psDstLive = 1;
        if (tagpu_vk_restore_job_failed(s_psJob)) {
            plog(d, "gui: the picture restore failed on this lane - backdrops and "
                    "stamps stay indexed");
            ps_job_free(d);
            s_psTried = 1;
        } else if (tagpu_vk_restore_job_idle(s_psJob)) {
            /* IDLE IS "EVERY FRAME ADDED IS PAINTED" (tagpu_vk_restore.h), and
               this runs before anything is added this frame, so every entry
               queued so far is in the destination -- in the command buffers
               already submitted ahead of this one. */
            for (i = 0; i < PS_MAX; i++) {
                if (s_ps[i].state != PSE_QUEUED) continue;
                s_ps[i].state = PSE_DONE; any++;
                if (s_ps[i].fresh) { fresh++; s_ps[i].fresh = 0; }
            }
            if (fresh) s_psSettled++;
            if (any) {
                static unsigned said;
                if (said < 32 || (said % 64) == 0)
                    plog(d, "gui: %d picture(s) restored HERE - %u stored, %d bound, "
                            "%u fills, %u evicted, %u refused", any, s_psStored,
                         s_psBound, s_psFills, s_psEvicted, s_psRefused);
                said++;
            }
        }
    }
    /* THE PALETTE MOVED, AND THE PRODUCER SAYS THE NEW ONE HAS SETTLED
       (`colourTwins` is its `s_colValid`). A picture a twin pins is
       restored again against it, repainting in place; every other one goes,
       and is stored again against the new palette when it is next drawn --
       so a level load does not restore the shell's whole map list for
       nothing. Nothing is filled from the old colours meanwhile:
       `pic_current` refuses until the new job exists. A pinning twin's colour
       plane is cleared now, since the composite would otherwise show the old
       palette's colours for as long as the restore takes -- the drain's own
       re-arm does the same to every colour twin. The evictions are not about
       room, so they do not count as recent: the pictures drawn again after the
       re-arm are restored and asked a repaint for. */
    if (s_psPalHave && h->pal && h->colourTwins && memcmp(h->pal, s_psPal, sizeof s_psPal)) {
        int kept = 0, dropped = 0;
        ps_job_free(d);
        for (i = 0; i < PS_MAX; i++) {
            if (s_ps[i].state == PSE_FREE) continue;
            if (!s_ps[i].pins) { ps_evict(d, &s_ps[i], 0); dropped++; continue; }
            s_ps[i].state = PSE_NEW; s_ps[i].fresh = 1; kept++;
        }
        plog(d, "gui: the presented palette moved under the picture store - %d "
                "pinned picture(s) restored again, %d dropped", kept, dropped);
        s_psEvN = 0;
        for (k = 0; k < s_ntw; k++)
            if (s_tw[k].pic && s_tw[k].colImg) { s_tw[k].colNeedClear = 1; s_tw[k].picGen = 0; }
        s_psGen++;
        s_psPalHave = 0;
    }
    /* Classic++'s assets are off: nothing is restored, and what the entries
       hold stays for the next arm. */
    if (!h->restoreFrames || h->restoreGen == 0) { ps_job_free(d); return; }
    if (s_psJob || s_psTried || !h->pal) return;
    /* ONLY AGAINST THE PALETTE THE DONE ENTRIES WERE RESTORED AGAINST.
       `s_psPal` is written below and nowhere else, and a new palette reaches
       it only through the rebuild above, which requeues or evicts every entry
       first. A job made while a new palette has not yet settled would name
       it, and every entry restored against the old one would then pass
       `pic_current`. */
    if (s_psPalHave && memcmp(h->pal, s_psPal, sizeof s_psPal)) return;
    for (i = 0; i < PS_MAX; i++) if (s_ps[i].state == PSE_NEW) break;
    if (i == PS_MAX) return;
    if (!tagpu_vk_restore_up(d)) { s_psTried = 1; return; }
    /* REPAINT ONCE A JOB HAS MADE THE DESTINATION READY: the DONE entries in
       it stay DONE, and only the new ones are painted. Before that there is
       nothing to keep, and the job's own clear is what makes the image a
       picture. */
    s_psJob = tagpu_vk_restore_job_new(d, "pic", 5, 0, s_psDstLive,
                                       s_psSrc, s_psSrcView, PS_W, PS_H, 0, h->pal,
                                       s_psDst, s_psDstView, PS_W, PS_H);
    if (!s_psJob) {
        s_psTried = 1;
        plog(d, "gui: no restore job for the picture store - backdrops and stamps "
                "stay indexed");
        return;
    }
    memcpy(s_psPal, h->pal, sizeof s_psPal);
    s_psPalHave = 1;
    for (i = 0; i < PS_MAX; i++) if (s_ps[i].state == PSE_NEW) ps_queue(&s_ps[i]);
}

/* THE HANDLES ARE FORGOTTEN, NOT DESTROYED: the device they belong to is
   already gone. The entries' bytes are ours and go. */
static void pic_forget(void)
{
    int i;
    for (i = 0; i < PS_MAX; i++) free(s_ps[i].bytes);
    memset(s_ps, 0, sizeof s_ps);
    memset(s_psCell, 0, sizeof s_psCell);
    s_psCellsFree = PS_CW * PS_CH;
    s_psJob = NULL;
    s_psSrc = s_psDst = VK_NULL_HANDLE;
    s_psSrcMem = s_psDstMem = VK_NULL_HANDLE;
    s_psSrcView = s_psDstView = VK_NULL_HANDLE;
    s_psSrcLay = VK_IMAGE_LAYOUT_UNDEFINED;
    s_psDstLive = 0; s_psPalHave = 0; s_psTried = 0; s_psBound = 0;
    s_psGen++;
    s_psEvN = 0;
}

/* the store goes with the device: behind the seam's vkDeviceWaitIdle */
static void pic_down(const TAGPU_VKPASS* d)
{
    /* the job first: it names both views */
    if (s_psJob) { tagpu_vk_restore_job_free(d, s_psJob); s_psJob = NULL; }
    kill_image(d, &s_psSrc, &s_psSrcMem, &s_psSrcView);
    kill_image(d, &s_psDst, &s_psDstMem, &s_psDstView);
    pic_forget();
    s_psSettled = 0;
    tagpu_gui_pic_settled(s_psSettled);
}

/* tagpu_vk_gui.h. Both jobs go back while the restorer can still take them;
   the images they paint into are this pass's and keep what they hold. The UI
   atlas's next job is made for the same generation and repaints over it
   (`s_rjKeepGen`, `restore_want`); the store's QUEUED pictures are owed again
   (`ps_job_free`) and its DONE ones stay DONE. */
void tagpu_vk_gui_restore_drop(const TAGPU_VKPASS* d)
{
    if (s_rjob) {
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL;
        s_rjKeepGen = s_rjGen;
    }
    s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0; s_rjSrcView = VK_NULL_HANDLE;
    ps_job_free(d);
}

/* THE REMAP TABLE AS THE IMAGE `TINT_FS` READS, into `dst`: RGBA8, 256 wide
   and twice `TAGPU_GUI_SHADE_ROWS` tall. The upper half is the remap, the
   index in `.r`. The lower half is, per entry, the map of COLOUR that goes
   with it -- `out = b + k * restored`, the blend's own form (TINT_FS):

       i the entry, j = row[i] where the row sends it, s = min(1, min over
       the channels of pal[j] / pal[i]);   k = s,   b = pal[j] - s * pal[i]

   and for black, which no channel constrains, s is the row's own ratio
   (sum of pal[j] over sum of pal[i] across the entries, at most 1): a black
   texel's restored colour is the detail the restorer put into dark art, and
   a dim has to scale it with its neighbours rather than leave it whole.
   So `out = pal[j] + s * (restored - pal[i])`: a texel whose restored colour
   IS its palette colour is shaded to the engine's own tinted colour exactly
   (to a level, the table being bytes), and the restorer's detail is kept on
   top, scaled down with a darkening and whole with a lightening. k in [0, 1]
   and b >= 0 are the blend's limits and not a choice -- a UNORM attachment
   clamps the source and its factors to [0, 1] before blending -- and `s` is
   the largest scale that keeps every channel's offset non-negative.
   With no palette the colour half stays zeros and this returns 0. */
static int tint_table(unsigned char* dst, const unsigned char* shade, const unsigned char* pal)
{
    unsigned r;
    int i, c;
    unsigned char* fit = dst + (size_t)TAGPU_GUI_SHADE_ROWS * 256u * 4u;
    memset(dst, 0, (size_t)TAGPU_GUI_SHADE_ROWS * 256u * 8u);
    for (r = 0; r < TAGPU_GUI_SHADE_ROWS; r++)
        for (i = 0; i < 256; i++) dst[((size_t)r * 256u + (unsigned)i) * 4u] = shade[(size_t)r * 256u + (unsigned)i];
    if (!pal) return 0;
    for (r = 0; r < TAGPU_GUI_SHADE_ROWS; r++) {
        const unsigned char* row = shade + (size_t)r * 256u;
        double sx = 0.0, sy = 0.0, rowk;
        for (i = 0; i < 256; i++)
            for (c = 0; c < 3; c++) {
                sx += pal[(size_t)i * 4u + (unsigned)c];
                sy += pal[(size_t)row[i] * 4u + (unsigned)c];
            }
        rowk = (sx > 0.0 && sy < sx) ? sy / sx : 1.0;
        for (i = 0; i < 256; i++) {
            const unsigned char* x = pal + (size_t)i * 4u;
            const unsigned char* y = pal + (size_t)row[i] * 4u;
            unsigned char* f = fit + ((size_t)r * 256u + (unsigned)i) * 4u;
            double k = (x[0] | x[1] | x[2]) ? 1.0 : rowk;
            for (c = 0; c < 3; c++)
                if (x[c] && (double)y[c] / x[c] < k) k = (double)y[c] / x[c];
            for (c = 0; c < 3; c++) {
                double o = y[c] - k * x[c];
                if (o < 0.0) o = 0.0;
                f[c] = (unsigned char)(o + 0.5);
            }
            f[3] = (unsigned char)(k * 255.0 + 0.5);
        }
    }
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
    unsigned i, first;
    int carries;
    int ndraw = 0, nquad = 0, quads = 0;
    int glUp = 0;
    VkDeviceSize glOff = 0;
    int shOn = 0, mmPicUp = 0, mmEngUp = 0, needMM = 0, needCurs = 0;
    /* THE VALIDATED COUNT, AND NOTHING BELOW SIZES ANYTHING FROM `h.nsdraw`.
       The file's own rule at the top: "a handed-over count never sizes an
       allocation". A raw negative one, cast to VkDeviceSize by the vertex and
       uniform `grow`s, asks for a buffer the size of the address space and
       refuses the pass for the session. */
    int nsd = 0;
    VkDeviceSize mmPicOff = 0, mmEngOff = 0, sStride = 0;
    VkDeviceSize stNeed = 0, stOff = 0, uStride, fStride;
    VkDeviceSize atOff = 0, palOff = 0, lutOff = 0;
    int atUp = 0, palUp = 0, lutUp = 0, lutFit = 0;
    TWIN* cur = NULL;
    int rpOpen = 0;
    int drawn = 0;
    /* WHY THE REPLAY STOOD DOWN. There are four ways to `standdown`, and a log
       that reports them all as the first one stops being evidence. */
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

    /* ASK FOR THE MIRROR, and keep asking. The drain copies nothing until
       something wants it, and it runs EARLIER in this same iteration of
       render_vk.c's loop than we do -- so the first frame this pass runs, the
       answer is always "nothing handed over", and the one after it is the first
       with a record. One frame of warm-up, once, and it costs an unarmed
       session nothing at all. */
    tagpu_gui_mirror_want(1);

    if (!tagpu_gui_handover(&h, d->frame)) {
        /* nothing handed over: nothing to composite, and nothing is kept
           once there is nothing to draw (2.28) */
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
        if (!behind_ex(d, "the producer's record of this frame was lost", 1)) goto refuse;
        return 0;
    }

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_gui_down(d); s_state = ST_REFUSED; return 0; }
        s_state = ST_READY;
    }

    /* ---- WHAT THIS PASS DOES NOT CARRY, AND WHY IT DOES NOT RETURN HERE.
       These three gate the COMPOSITE and nothing else. The op stream still has
       to be applied, because our twin store must follow the producer's or it is
       worth nothing: the drain applied these ops to its table whatever we draw,
       and a frame we skip leaves our twins behind ITS twins for the rest of the
       session, silently. So `compose` is cleared and the replay runs anyway:
       an early return here would take the frame's ops with it. ---- */
    /* `otherOps` is 0 for every op kind that exists today. The machinery stays,
       because the NEXT op kind added to the queue will land here rather than
       being drawn wrong, and it is a capability gap rather than a sync one:
       a fresh start would not help, so it says so once, composites nothing,
       and catches the store up when the op stops appearing. */
    if (h.otherOps > 0) {
        if (!s_cantReplay) {
            s_cantReplay = 1;
            plog(d, "gui: the producer applied %d op(s) this pass cannot "
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
    /* CLASSIC++ IS CARRIED, so `colourTwins` is not a stand-down: it IS
       `uColOn`, exactly as `draw_layer` set it, and it is read where the
       composite's block is filled. The one thing still refused here is a frame
       whose twins the drain has given colour while this pass has no restored
       atlas -- the replay would then write alpha 0 where the ops ask for
       restored colour, silently and cumulatively. */
    if (h.colourTwins && !s_arHave) {
        if (!s_saidColour) { s_saidColour = 1;
            plog(d, "gui: the other lane is compositing Classic++ colour and "
                    "this one has no restored atlas to composite from - nothing "
                    "composited while that is true"); }
        compose = 0;
    } else s_saidColour = 0;
    /* EVERY COLOUR TWIN WAS INVALIDATED, and it happened BEFORE this frame's
       ops (`restore_step` runs ahead of the drain), so it is applied before
       them: each colour twin is kept and cleared whole, by asking for its clear
       again. */
    if (h.colRearm != s_colRearm || !s_colRearmSeen) {
        if (s_colRearmSeen && h.colRearm != s_colRearm) {
            int k;
            for (k = 0; k < s_ntw; k++) {
                if (s_tw[k].colImg) s_tw[k].colNeedClear = 1;
                s_tw[k].picGen = 0;          /* a bound picture is filled again */
            }
            plog(d, "gui: the presented palette moved - %d colour twin(s) "
                    "invalidated, as the producer invalidated its own", s_ntw);
        }
        s_colRearm = h.colRearm;
        s_colRearmSeen = 1;
    }
    /* ---- THE SHARP LAYER. `h.sharpOn` is COVERAGE, and the quads that
       produced that coverage cross with the record and are drawn here.

       AN OVERFLOWING LIST IS A `compose = 0` AND NOT THE BEHIND STATE, which is
       the distinction the twins draw: a sharp-layer quad mutates
       nothing that outlives its frame -- the layer is cleared at every present
       -- so a frame we cannot draw is ONE frame, never a store out of step.
       Nothing is asked of the producer for it. */
    if (h.nsdraw < 0 || h.nsdraw > TAGPU_GUI_SDRAW_MAX ||
        (h.nsdraw > 0 && (h.sharpW < 1 || h.sharpH < 1 ||
                          h.sharpW > SURF_MAXDIM || h.sharpH > SURF_MAXDIM))) {
        if (!s_saidSharp) { s_saidSharp = 1;
            plog(d, "gui: %d sharp-layer quad(s) at %dx%d is past what this pass "
                    "carries - nothing composited this frame, and nothing is "
                    "asked of the producer: the layer keeps no state",
                 h.nsdraw, h.sharpW, h.sharpH); }
        compose = 0;
    } else {
        int shRefused = 0;
        nsd = h.nsdraw;
        shOn = (nsd > 0) ? 1 : 0;
        for (i = 0; i < (unsigned)nsd; i++) {
            if (h.sdraw[i].kind == TAGPU_GUISK_MM)     needMM = 1;
            if (h.sdraw[i].kind == TAGPU_GUISK_CURSOR) needCurs = 1;
        }
        /* the inputs each kind needs, refused in this file's own terms. A
           cursor samples the UI atlas; the minimap its picture and the engine's
           pair. Missing any of them is this frame not being composited. */
        if (needCurs && !h.atlas) {
            shOn = 0; compose = 0; shRefused = 1;
            if (!s_saidSharp) { s_saidSharp = 1;
                plog(d, "gui: the sharp layer wants the cursor and the hand-over "
                        "carries no UI atlas - nothing composited"); }
        }
        if (needMM && (!h.mmPic || h.mmPicW < 1 || h.mmPicH < 1 ||
                       h.mmPicW > ATLAS_MAXDIM || h.mmPicH > ATLAS_MAXDIM ||
                       !h.mmEng || h.mmEngW < 1 || h.mmEngH < 1 ||
                       h.mmEngW > ATLAS_MAXDIM || h.mmEngH > ATLAS_MAXDIM)) {
            /* `needMM` IS CLEARED, and that is the point. Left set, it would
               feed the two cases this guard names straight into the staging
               path below, which reads `mmEngW * mmEngH * 3` bytes out of the very
               pointer the guard refused -- a NULL read of 15 876 texels, or a
               `grow` sized from a rejected dimension that fails and refuses the
               pass for the session. A refusal that still consumes its input is
               not a refusal. */
            shOn = 0; compose = 0; needMM = 0; shRefused = 1;
            if (!s_saidSharp) { s_saidSharp = 1;
                plog(d, "gui: the sharp layer wants the minimap and this frame's "
                        "copy of it is %s - nothing composited",
                     h.mmPic ? "outside what this pass carries" : "absent"); }
        }
        /* the producer says the layer HAS coverage and we produced no quad
           for it: that is a client this pass does not carry, and it composites
           nothing rather than a layer missing a piece. */
        if (h.sharpOn && !shOn) compose = 0;
        /* THE LATCH IS CLEARED ONLY BY A GOOD FRAME. Cleared at the top of this
           branch, `if (!s_saidSharp)` would be true on every present, so a
           condition that holds every frame (the atlas mirror not yet
           allocated, say) would print a line per present for the session. */
        if (!shRefused) s_saidSharp = 0;
    }
    /* ---- TA'S OWN FRAME IS NOT ACQUIRED AT ALL -----------------------------
       This pass cannot name TA's image: `LAY_FS` declares no `uSurf` and the
       descriptor set has four images. THE GOLDEN SOURCE IS UNAFFECTED --
       `tagpu_vk_surf.c` uploads it every changed frame and
       `tagpu_vk_surf_engine_view` hands it out; it has no consumer here. */
    if (!h.pal || !h.presented || h.surfW < 1 || h.surfH < 1 ||
        h.surfW > SURF_MAXDIM || h.surfH > SURF_MAXDIM) compose = 0;
    /* WHILE BEHIND, THE FRAME STARTS AT ITS RESET, for pass 1 as for pass 2.
       Pass 2 skips every op before the RESET, and pass 1 validates only what
       pass 2 will apply: a refused op before the RESET would throw away the
       frame that carries the fresh start -- and `behind` does not ask again
       while already behind, so the pass would wait for a RESET the producer
       had already sent, composite nothing, and never recover.

       `carries` IS THE LOST-FRAME RULE APPLIED TO A REFUSAL. A refusal in
       this frame is the answer provably not applied, exactly as `h.lost` is,
       so every refusal below re-asks through `behind_ex`. */
    first = 0; carries = 0;
    if (s_behind) {
        while (first < h.nops && h.ops[first].kind != TAGPU_GUIOP_RESET) first++;
        carries = first < h.nops;
    }
    if (h.atlas && (h.atlasDim < 1 || h.atlasDim > ATLAS_MAXDIM ||
                    h.atlasRows < 1 || h.atlasRows > h.atlasDim)) {
        /* the atlas is what a sprite samples: without a sane one the replay
           itself cannot run */
        if (!behind_ex(d, "an atlas outside what this pass carries", carries)) goto refuse;
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
        if (!behind_ex(d, "a glyph atlas outside what this pass carries", carries)) goto refuse;
        return 0;
    }

    /* ---- pass 1: validate every op and count what the frame needs ----
       IN THIS FILE'S OWN TERMS. A bound that lives in the file that produced
       the number is a bound only while both files are read together. */
    for (i = first; i < h.nops; i++) {
        const TAGPU_GUIOP* o = &h.ops[i];
        int bw = o->r - o->l + 1, bh = o->b - o->t + 1;
        switch (o->kind) {
        case TAGPU_GUIOP_SEED:
            if (o->w < 1 || o->h < 1 || o->w > SURF_MAXDIM || o->h > SURF_MAXDIM) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
            if (o->alen) {
                if (o->alen != (unsigned)o->w * (unsigned)o->h) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
                if (o->aoff > h.alen || o->aoff + o->alen > h.alen) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
                stNeed += (VkDeviceSize)o->alen * 2;
                /* A PICTURE MAY BE NEW TO THE STORE, and its tiles' R8 bytes
                   then go through this staging too */
                if (o->col & TAGPU_GUICOL_ON) stNeed += ps_stage_need(o->w, o->h);
            }
            break;
        case TAGPU_GUIOP_PIXELS:
            if (bw < 1 || bh < 1) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
            if (o->alen != (unsigned)bw * (unsigned)bh) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
            if (o->aoff > h.alen || o->aoff + o->alen > h.alen) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
            stNeed += (VkDeviceSize)o->alen * 2;
            if (o->col & TAGPU_GUICOL_ON) stNeed += ps_stage_need(bw, bh);
            break;
        case TAGPU_GUIOP_SPRITE:
            if (o->fw < 1 || o->fh < 1) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
            if (!h.atlas) { if (!behind_ex(d, "a sprite with no atlas", carries)) goto refuse; return 0; }
            /* THE PRODUCER SAMPLED THE RESTORED ATLAS AND WE HAVE NONE. Drawing
               anyway writes alpha 0 where it wrote restored colour, into a twin
               that keeps it -- so it is a `behind` and not a `compose = 0`.
               IT SHOULD BE UNREACHABLE AND IS KEPT AS THE BRACE. The producer
               may only set `TAGPU_GUICOL_ON` while this pass has said
               `tagpu_gui_col_ready(1)`, which is `s_arHave`, so the two cannot
               disagree except across the one frame of lag that back-channel
               has -- and that frame moves the flag the safe way, from off to
               on. What this catches is the day something else sets the bit. */
            if ((o->col & TAGPU_GUICOL_ON) && !s_arHave) {
                if (!behind_ex(d, "a restored sprite and no restored atlas on this lane", carries)) goto refuse;
                return 0;
            }
            ndraw++; nquad++;
            break;
        case TAGPU_GUIOP_STRING:
            /* A STRING IS ONE UNIFORM WINDOW AND `nglyph` QUADS. Its cells were
               resolved by the drain (`twin_string`) against an atlas that can repack
               mid-string, so they are carried rather than looked up again --
               and bounded here in this file's own terms all the same. */
            if (o->nglyph < 1 || o->nglyph > 256 ||
                o->alen != (unsigned)o->nglyph * 8 ||
                o->aoff > h.alen || o->aoff + o->alen > h.alen) {
                if (!behind_ex(d, "a malformed string op", carries)) goto refuse;
                return 0;
            }
            if (!h.glyphs) { if (!behind_ex(d, "a string with no glyph atlas", carries)) goto refuse; return 0; }
            ndraw++; nquad += o->nglyph;
            break;
        case TAGPU_GUIOP_COPY:
            /* A SELF-COPY IS REFUSED rather than guessed at: it would read and
               write one image in one draw, which is undefined behaviour. */
            if (o->surf == o->src) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
            ndraw++; nquad++;
            break;
        case TAGPU_GUIOP_CLEAR:
        case TAGPU_GUIOP_BAR:
        case TAGPU_GUIOP_RECT:
            /* NONE NEEDS A DRAW SLOT: all three are `vkCmdClearAttachments`
               over one or more rects, so they cost no quad and no descriptor
               set and are not counted against DRAW_MAX/QUAD_MAX. RECT clears
               four rects in ONE call rather than four, which is why it is here
               and not with the quads. */
            if (bw < 1 || bh < 1) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
            break;
        case TAGPU_GUIOP_TINT:
            /* THE TINT IS A DRAW, unlike the other three geometric ops: there
               is no clear value for "whatever was here, remapped". One quad,
               one uniform window, one descriptor set -- `CPY_FS`'s cost.
               THE ROW IS BOUNDED IN THIS FILE'S OWN TERMS, which is this
               pass's rule and not a duplicate of the producer's: `fg` indexes
               a `TAGPU_GUI_SHADE_ROWS`-row image and a row past the end is a sampler read this
               file cannot describe, whatever the other side believes it sent.
               THE TABLE IS A PRECONDITION and its absence is a `behind` rather
               than a skip: the drain does not mirror a tint without one, so a
               tint arriving here with `h.shade == NULL` means the two halves
               disagree, and a store that quietly skips ops the other half
               applied is the silent divergence this pass exists to avoid. */
            if (bw < 1 || bh < 1) { if (!behind_ex(d, "a malformed op", carries)) goto refuse; return 0; }
            if (o->fg >= TAGPU_GUI_SHADE_ROWS) { if (!behind_ex(d, "a tint row past the table", carries)) goto refuse; return 0; }
            if (!h.shade) { if (!behind_ex(d, "a tint with no shade table", carries)) goto refuse; return 0; }
            ndraw++; nquad++;
            break;
        case TAGPU_GUIOP_FREE:
        case TAGPU_GUIOP_RESET:
            break;
        default:
            if (!behind_ex(d, "an op kind this pass does not know", carries)) goto refuse;
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
        if (!behind_ex(d, "more draws or quads in one frame than the bound", carries)) goto refuse;
        return 0;
    }
    s_saidRoom = 0;

    /* the shared texels, and the staging they need */
    if (h.atlas && (!s_atHave || s_atSerial != h.atlasSerial || s_atDim != h.atlasDim)) {
        atUp = 1; stNeed += (VkDeviceSize)h.atlasDim * h.atlasRows;
    }
    if (h.pal && (!s_palHave || s_palSerial != h.palSerial)) { palUp = 1; stNeed += 256 * 4; }
    /* THE REMAP TABLE, on the palette's rule: uploaded when its serial moves,
       which for this table is once a session in practice -- and when the
       palette's does, because its colour half is made against the palette
       (`tint_table`). */
    if (h.shade && (!s_lutHave || s_lutSerial != h.shadeSerial ||
                    (h.pal ? 1u : 0u) != (unsigned)s_fitHave ||
                    (h.pal && s_lutPal != h.palSerial))) {
        lutUp = 1; stNeed += (VkDeviceSize)TAGPU_GUI_SHADE_BYTES * 8u;
    }
    /* THE MINIMAP'S TWO, WIDENED RGB8 -> RGBA8 ON THE WAY IN, so what they
       reserve is FOUR bytes a texel and not three. The picture moves on a map
       load and a palette change and says so with a serial; the engine's pair
       moves every frame by definition, exactly as its own frame does. */
    if (needMM) {
        if (!s_mmPicHave || s_mmPicSerial != h.mmPicGen ||
            s_mmPicW != h.mmPicW || s_mmPicH != h.mmPicH) {
            mmPicUp = 1; stNeed += (VkDeviceSize)h.mmPicW * h.mmPicH * 4;
        }
        mmEngUp = 1; stNeed += (VkDeviceSize)h.mmEngW * h.mmEngH * 4;
    }
    /* THE CONTENT SERIAL, NOT THE REPACK GENERATION -- see tagpu_gui.h.
       `glyphGen` moves only when the atlas is thrown away, so keyed on it the
       image would be uploaded once and every glyph rasterised afterwards would
       stay 0 in it: invisible text where `bg == tr` and a solid box where it
       is not, permanently, and no counter anywhere would say so. */
    if (h.glyphs && (!s_glHave || s_glSerial != h.glyphSerial ||
                     s_glW != h.glyphW || s_glH != h.glyphH)) {
        glUp = 1; stNeed += (VkDeviceSize)h.glyphW * h.glyphH;
    }

    /* ---- room ---- */
    uStride = align_up(QVS_SZ, s_ualign);
    fStride = align_up(TWF_SZ, s_ualign);
    if (!grow(d, &s->stage, &s->stMem, &s->stMap, &s->stCap,
              stNeed ? stNeed : 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) goto refuse;
    /* +nsdraw: the sharp layer's quads share this slot's vertex buffer, and
       +1 is still the composite's own quad at the very end of it */
    if (!grow(d, &s->vb, &s->vbMem, &s->vbMap, &s->vbCap,
              (VkDeviceSize)(nquad + nsd + 1) * 24 * sizeof(float),
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) goto refuse;
    /* ndraw twin windows THEN nsdraw sharp ones: the sharp layer's quads take
       a QVS window each out of the same buffer, at `ndraw + q` */
    if (!grow(d, &s->ub, &s->ubMem, &s->ubMap, &s->ubCap,
              uStride * (VkDeviceSize)((ndraw + nsd) ? (ndraw + nsd) : 1),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) goto refuse;
    if (!grow(d, &s->fb_, &s->fbMem, &s->fbMap, &s->fbCap,
              fStride * (ndraw ? ndraw : 1), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) goto refuse;
    sStride = align_up(TWF_SZ, s_ualign);
    if (shOn && !grow(d, &s->sb, &s->sbMem, &s->sbMap, &s->sbCap,
                      sStride * (VkDeviceSize)nsd,
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) goto refuse;
    /* THE LAYER ITSELF, this slot's own. Destroying it here is safe for the
       same reason the slot's buffers are: the seam waited `fence[slot]` before
       this hook ran, so nothing in flight names it. It is NOT the twins' case
       and needs no retire. */
    if (shOn && (s->shW != h.sharpW || s->shH != h.sharpH)) {
        if (s->shFb) { vkDestroyFramebuffer(d->dev, s->shFb, NULL); s->shFb = VK_NULL_HANDLE; }
        kill_image(d, &s->shImg, &s->shMem, &s->shView);
        s->shW = s->shH = 0;
    }
    if (shOn && !s->shImg) {
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        if (!mk_image(d, h.sharpW, h.sharpH, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &s->shImg, &s->shMem, &s->shView)) goto refuse;
        fi.renderPass = s_sharpRp; fi.attachmentCount = 1; fi.pAttachments = &s->shView;
        fi.width = (uint32_t)h.sharpW; fi.height = (uint32_t)h.sharpH; fi.layers = 1;
        if (vkCreateFramebuffer(d->dev, &fi, NULL, &s->shFb) != VK_SUCCESS) goto refuse;
        s->shW = h.sharpW; s->shH = h.sharpH;
    }
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
    if (mmPicUp || mmEngUp) {
        /* RGB8 -> RGBA8, alpha 255. Three-channel formats are optional in
           Vulkan and absent on plenty of real devices; MM_FS reads `.rgb`, so
           the widening costs one byte a texel and no shader change. */
        int pass_;
        for (pass_ = 0; pass_ < 2; pass_++) {
            const unsigned char* src = pass_ ? h.mmEng : h.mmPic;
            int pw_ = pass_ ? h.mmEngW : h.mmPicW;
            int ph_ = pass_ ? h.mmEngH : h.mmPicH;
            unsigned char* dst_;
            long n_, k_;
            if (pass_ ? !mmEngUp : !mmPicUp) continue;
            if (pass_) mmEngOff = stOff; else mmPicOff = stOff;
            dst_ = s->stMap + stOff;
            n_ = (long)pw_ * ph_;
            for (k_ = 0; k_ < n_; k_++) {
                dst_[4 * k_ + 0] = src[3 * k_ + 0];
                dst_[4 * k_ + 1] = src[3 * k_ + 1];
                dst_[4 * k_ + 2] = src[3 * k_ + 2];
                dst_[4 * k_ + 3] = 255;
            }
            stOff += (VkDeviceSize)n_ * 4;
        }
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
    if (lutUp) {
        /* 256 WIDE, so row-major `[row][index]` is `(x, y)` with no arithmetic
           in the shader, and twice `TAGPU_GUI_SHADE_ROWS` tall for the colour
           half (`tint_table`). The image is never recreated -- its size is a
           constant of the format -- so there is no retire here and no
           `ret_push`. */
        if (!s_lutImg &&
            !mk_image(d, 256, 2 * (int)TAGPU_GUI_SHADE_ROWS, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      &s_lutImg, &s_lutMem, &s_lutView)) goto refuse;
        lutOff = stOff;
        lutFit = tint_table(s->stMap + stOff, h.shade, h.pal);
        stOff += (VkDeviceSize)TAGPU_GUI_SHADE_BYTES * 8u;
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
    if (mmPicUp) {
        if (s_mmPicW != h.mmPicW || s_mmPicH != h.mmPicH) {
            if (!ret_push(d, s_mmPicImg, s_mmPicMem, s_mmPicView, VK_NULL_HANDLE)) goto refuse;
            s_mmPicImg = VK_NULL_HANDLE; s_mmPicMem = VK_NULL_HANDLE;
            s_mmPicView = VK_NULL_HANDLE; s_mmPicW = s_mmPicH = 0; s_mmPicHave = 0;
            if (!mk_image(d, h.mmPicW, h.mmPicH, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          &s_mmPicImg, &s_mmPicMem, &s_mmPicView)) goto refuse;
            s_mmPicW = h.mmPicW; s_mmPicH = h.mmPicH;
        }
        img_barrier(cb, s_mmPicImg,
                    s_mmPicHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    s_mmPicHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    s_mmPicHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        copy_rect(cb, s->stage, mmPicOff, s_mmPicImg, 0, 0, h.mmPicW, h.mmPicH);
        img_barrier(cb, s_mmPicImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        s_mmPicHave = 1; s_mmPicSerial = h.mmPicGen;
    }
    if (mmEngUp) {
        if (s_mmEngW != h.mmEngW || s_mmEngH != h.mmEngH) {
            if (!ret_push(d, s_mmEngImg, s_mmEngMem, s_mmEngView, VK_NULL_HANDLE)) goto refuse;
            s_mmEngImg = VK_NULL_HANDLE; s_mmEngMem = VK_NULL_HANDLE;
            s_mmEngView = VK_NULL_HANDLE; s_mmEngW = s_mmEngH = 0; s_mmEngHave = 0;
            if (!mk_image(d, h.mmEngW, h.mmEngH, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          &s_mmEngImg, &s_mmEngMem, &s_mmEngView)) goto refuse;
            s_mmEngW = h.mmEngW; s_mmEngH = h.mmEngH;
        }
        img_barrier(cb, s_mmEngImg,
                    s_mmEngHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    s_mmEngHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    s_mmEngHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        copy_rect(cb, s->stage, mmEngOff, s_mmEngImg, 0, 0, h.mmEngW, h.mmEngH);
        img_barrier(cb, s_mmEngImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        s_mmEngHave = 1;
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
    if (lutUp) {
        img_barrier(cb, s_lutImg,
                    s_lutHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    s_lutHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                              : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    s_lutHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        copy_rect(cb, s->stage, lutOff, s_lutImg, 0, 0, 256, 2 * (int)TAGPU_GUI_SHADE_ROWS);
        img_barrier(cb, s_lutImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        s_lutHave = 1; s_lutSerial = h.shadeSerial;
        s_fitHave = lutFit; s_lutPal = lutFit ? h.palSerial : 0;
    }
    /* ---- CLASSIC++: THE UI ATLAS IS RESTORED HERE. After the copy above, so
       a restore issued this frame reads the texels this frame delivered and
       not the palette's entry 0 where the atlas had not arrived yet -- the
       same ordering `tagpu_vk_feat.c` states for the same reason. It records
       nothing into `cb`; `tagpu_vk_restore_step` does that, last of every
       pass's prepare. */
    restore_want(d, &h);
    pic_step(d, &h);
    tagpu_gui_pic_settled(s_psSettled);

    /* no barrier and no copy for the engine's frame: `tagpu_vk_surf_prepare`
       recorded both into this same command buffer, for this same slot, before
       this function was called (tagpu_vk.c). */

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

    /* ANY COLOUR TWIN THE RE-ARM INVALIDATED IS CLEARED NOW, AND NOT LAZILY AT
       ITS NEXT OP. `tw_fresh` is the per-twin path and every draw arm calls it,
       which is enough for a twin an op touches -- and a twin nothing touches
       this frame is still one the composite may PRESENT, and `uColOn` would
       then read colour the producer invalidated three frames ago. The sweep costs
       one early-return per twin on every other frame. */
    {
        int k;
        for (k = 0; k < s_ntw; k++) tw_fresh(cb, &s_tw[k]);
    }

    /* A BACKDROP WHOSE RESTORE HAS LANDED SINCE IT WAS SEEDED is coloured now,
       before this frame's ops: every copy out of it below then carries the
       restored picture. Only a twin still bound -- nothing has drawn into it
       since its seed -- so the picture is exactly what the twin holds. */
    if (s_psBound && pic_current(&h)) {
        int k;
        for (k = 0; k < s_ntw; k++) {
            TWIN* bt = &s_tw[k];
            if (!bt->pic || bt->picDrawn || bt->picGen == s_psGen || bt->pic > PS_MAX) continue;
            if (s_ps[bt->pic - 1].state != PSE_DONE) continue;
            pic_fill(cb, bt, &s_ps[bt->pic - 1], 0, 0);
            bt->picGen = s_psGen;
        }
    }

    memset(s_setView, 0, sizeof s_setView);   /* the frame's claims start clean */
    memset(s_setView2, 0, sizeof s_setView2);

    /* WHILE BEHIND, ONLY A RESET IS APPLIED. Everything before the producer's
       fresh start names twins this store never made, and applying it would
       build a picture out of half a history. */

    /* ---- pass 2: the ops, in the order the drain applied them ---- */
    for (i = 0; i < h.nops; i++) {
        const TAGPU_GUIOP* o = &h.ops[i];
        TWIN* t;
        int bw = o->r - o->l + 1, bh = o->b - o->t + 1;
        if (s_behind) {
            if (o->kind != TAGPU_GUIOP_RESET) continue;
            s_behind = 0;               /* the fresh start has arrived */
        }
        /* A PICTURE TWIN STOPS BEING ONE AT THE FIRST OP THAT DRAWS INTO IT:
           a later fill would paint the picture over what that op drew. It
           keeps the pin (TWIN `pic`). A re-seed is handled in its own arm. */
        if (s_psBound && o->kind != TAGPU_GUIOP_SEED &&
            o->kind != TAGPU_GUIOP_FREE && o->kind != TAGPU_GUIOP_RESET) {
            TWIN* bt = tw_find(o->surf);
            if (bt && bt->pic && !bt->picDrawn) { bt->picDrawn = 1; s_psBound--; }
        }

        /* a draw needs a render pass open on ITS destination; everything else
           needs none open at all (a transfer and a layout change may not be
           recorded inside one) */
        if (o->kind == TAGPU_GUIOP_SPRITE || o->kind == TAGPU_GUIOP_COPY ||
            o->kind == TAGPU_GUIOP_STRING || o->kind == TAGPU_GUIOP_CLEAR ||
            o->kind == TAGPU_GUIOP_BAR    || o->kind == TAGPU_GUIOP_RECT) {
            TWIN* src = NULL;
            t = tw_find(o->surf);
            /* THE DRAIN HAD A TWIN AND WE DO NOT, WHICH IS THE DEFINITION OF
               BEHIND. Skipping would be silent divergence: the op applied to
               the drain's table and never here, for the session. The composite's own
               `tw_find(h.presented)` catches it only for the PRESENTED surface
               and only on a frame the composite is drawn on -- and `compose` is
               0 on most real frames. */
            if (!t) { sdWhy = "an op names a surface this store never seeded"; goto standdown; }
            /* CLASSIC++: THE OP SAYS THE DRAIN GAVE THIS TWIN COLOUR, so this
               one gets it too, and the open render pass closes because the
               framebuffer this twin draws into is about to change. A twin that
               HAS colour never loses it short of being dropped, which is the
               drain's lifetime exactly (`twin_sprite` and `twin_copy` only ever
               set `col`). */
            if ((o->col & TAGPU_GUICOL_DST) && !t->colImg) {
                if (rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
                if (!tw_colour(d, t)) {
                    sdWhy = "the producer gave a twin colour and this store could not";
                    goto standdown;
                }
            }
            /* a clear and a layout change are both illegal inside a render
               pass instance, so the open one closes first. Unreachable while
               `tw_make` is the only creator and clears on the next line — but
               "unreachable" is an enumeration of today's call sites, and this
               is the bound. */
            if ((t->needClear || t->colNeedClear) && rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
            tw_fresh(cb, t);
            if (o->kind == TAGPU_GUIOP_COPY) {
                src = tw_find(o->src);
                if (!src) {
                    /* OUR STORE HAS FALLEN BEHIND. Ask the producer for a
                       fresh start and stop drawing until it arrives --
                       applying the rest would put a
                       wrong picture in a twin that the NEXT frame inherits. */
                    /* THE ASK IS `behind`'s, NOT THIS SITE'S. Raising the
                       producer's flag here and then falling into `standdown`
                       -- which calls `behind`, which raises it again -- is two
                       reseeds per occurrence, and it walks straight past the
                       cap that stops a recurring condition reseeding the
                       producer at the frame rate. One door. */
                    if (rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
                    sdWhy = "a copy names a source twin this store never made";
                    goto standdown;
                }
                if ((src->needClear || src->colNeedClear) && rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
                tw_fresh(cb, src);
                if (src != cur && rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
                if (rpOpen && cur != t) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
                if (!rpOpen) tw_to(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            if (rpOpen && cur != t) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }
            if (!rpOpen) {
                VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                tw_to(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                /* one attachment or two: a twin with colour draws through
                   its second framebuffer, against `s_twRp2` */
                rb.renderPass = t->colImg ? s_twRp2 : s_twRp;
                rb.framebuffer = t->colImg ? t->fb2 : t->fb;
                rb.renderArea.extent.width = (uint32_t)t->w;
                rb.renderArea.extent.height = (uint32_t)t->h;
                vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
                set_viewport(cb, t->w, t->h);
                rpOpen = 1; cur = t;
            }
            /* THE STRING SETS NO SCISSOR: its glyph quads are bounded by the
               twin alone, where the sprite and the copy are scissored to
               their op box. */
            if (o->kind == TAGPU_GUIOP_STRING) set_scissor(cb, 0, 0, t->w, t->h, t->w, t->h);
            else                               set_scissor(cb, o->l, o->t, bw, bh, t->w, t->h);
            /* THE SCISSOR ABOVE DOES NOT BOUND RECT'S EDGES.
               `vkCmdClearAttachments` is NOT affected by the dynamic scissor
               state -- only its own `pRects` bounds it. The explicit per-rect
               clamp below is the real bound and the only one, which is why it
               must not be removed as redundant. */

            if (o->kind == TAGPU_GUIOP_CLEAR || o->kind == TAGPU_GUIOP_BAR ||
                o->kind == TAGPU_GUIOP_RECT) {
                /* CLEAR: THE BOX TO COVERAGE 0 -- AND BOTH ATTACHMENTS ON A
                   COLOUR TWIN. Clearing only the index here would leave
                   restored colour standing under a box the engine erased. */
                /* BAR: the same rect, but the INDEX attachment
                   takes the engine's palette index with coverage 1 instead of
                   zeros. The colour attachment still goes to ZERO on both, and for the
                   same reason: a solid engine fill has no restored art, so
                   leaving the old colour standing would show it through a box
                   the engine just painted over. The index is `o->fg`, one byte,
                   because that is all EITHER writer reads of the engine's own
                   colour argument: `0x4CCDEA` for `DrawBar 0x4BF6F0`, and
                   `0x4CC7AB` for the axis-aligned `DrawLine 0x4BE950`, which
                   is routed through this same packet -- and which is most of
                   the traffic (exe-reverse-engineering.md). */
                /* RECT IS FOUR RECTS IN ONE CALL; CLEAR and BAR are one.
                   `vkCmdClearAttachments` takes a rect ARRAY, so the four edges
                   cost one command rather than four, and each is clamped to the
                   twin separately below. */
                VkClearAttachment ca[2];
                VkClearRect cr[4];
                int nca = t->colImg ? 2 : 1;
                int ncr = 0, i;
                memset(ca, 0, sizeof ca); memset(cr, 0, sizeof cr);
                ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ca[0].colorAttachment = 0;
                if (o->kind == TAGPU_GUIOP_BAR || o->kind == TAGPU_GUIOP_RECT) {
                    ca[0].clearValue.color.float32[0] = (float)o->fg / 255.0f;
                    ca[0].clearValue.color.float32[1] = 1.0f;
                }
                ca[1].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ca[1].colorAttachment = 1;
                if (o->kind == TAGPU_GUIOP_RECT) {
                    /* top, bottom, left, right -- the OUTER box's four edges,
                       one pixel each, corners written twice with the same
                       constant. `bw`/`bh` are the inclusive box's extents, so a
                       1 px box gives four degenerate-but-valid edges rather
                       than none. The interior is NOT cleared: that is the whole
                       difference from BAR. */
                    int ex[4], ey[4], ew[4], eh[4], k;
                    ex[0] = o->l; ey[0] = o->t;      ew[0] = bw; eh[0] = 1;
                    ex[1] = o->l; ey[1] = o->b;      ew[1] = bw; eh[1] = 1;
                    ex[2] = o->l; ey[2] = o->t;      ew[2] = 1;  eh[2] = bh;
                    ex[3] = o->r; ey[3] = o->t;      ew[3] = 1;  eh[3] = bh;
                    for (k = 0; k < 4; k++) {
                        cr[ncr].rect.offset.x = ex[k]; cr[ncr].rect.offset.y = ey[k];
                        cr[ncr].rect.extent.width  = (uint32_t)ew[k];
                        cr[ncr].rect.extent.height = (uint32_t)eh[k];
                        cr[ncr].layerCount = 1;
                        ncr++;
                    }
                } else {
                    cr[0].rect.offset.x = o->l; cr[0].rect.offset.y = o->t;
                    cr[0].rect.extent.width = (uint32_t)bw;
                    cr[0].rect.extent.height = (uint32_t)bh;
                    cr[0].layerCount = 1;
                    ncr = 1;
                }
                /* CLAMP EVERY RECT TO THE TWIN, and drop the ones that clamp
                   away -- `vkCmdClearAttachments` has undefined behaviour for a
                   rect outside the render area, so this is a bound and not a
                   tidy-up. Compacting in place keeps the survivors contiguous,
                   which is what `rectCount` means. */
                {
                    int keep = 0;
                    for (i = 0; i < ncr; i++) {
                        VkClearRect r = cr[i];
                        if (r.rect.offset.x < 0) {
                            r.rect.extent.width = (r.rect.extent.width > (uint32_t)(-r.rect.offset.x))
                                ? r.rect.extent.width - (uint32_t)(-r.rect.offset.x) : 0;
                            r.rect.offset.x = 0;
                        }
                        if (r.rect.offset.y < 0) {
                            r.rect.extent.height = (r.rect.extent.height > (uint32_t)(-r.rect.offset.y))
                                ? r.rect.extent.height - (uint32_t)(-r.rect.offset.y) : 0;
                            r.rect.offset.y = 0;
                        }
                        if (r.rect.offset.x >= t->w || r.rect.offset.y >= t->h) continue;
                        if (r.rect.offset.x + (int)r.rect.extent.width > t->w)
                            r.rect.extent.width = (uint32_t)(t->w - r.rect.offset.x);
                        if (r.rect.offset.y + (int)r.rect.extent.height > t->h)
                            r.rect.extent.height = (uint32_t)(t->h - r.rect.offset.y);
                        if (!r.rect.extent.width || !r.rect.extent.height) continue;
                        cr[keep++] = r;
                    }
                    ncr = keep;
                }
                if (ncr)
                    vkCmdClearAttachments(cb, (uint32_t)nca, ca, (uint32_t)ncr, cr);
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
                    if (!set_claim(d, s, s_glView, VK_NULL_HANDLE, QVS_SZ, TWF_SZ, &si_)) {
                        sdWhy = "this frame claimed more distinct images than there are sets";
                        goto standdown; }
                    ds = s->sets[si_];
                    dyn[0] = (uint32_t)((VkDeviceSize)drawn * uStride);
                    dyn[1] = (uint32_t)((VkDeviceSize)drawn * fStride);
                    /* STR_FS WRITES `oCol = vec4(0.0)`, so on a colour twin a
                       string ERASES the restored colour under every glyph it
                       stamps and leaves it standing between them -- one of
                       the gains the drain's string-op comment (13.4) claims
                       over publishing the box's bytes. The two-attachment pipeline
                       is how that write lands. */
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      t->colImg ? s_pipeStr2 : s_pipeStr);
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
                /* `uRestored` / `uSrcHasCol` ARE THE DRAIN'S DECISION, carried
                   as `TAGPU_GUICOL_ON`. Deriving them here would be asking
                   `s_colValid` (tagpu_gui_surf.c) a second time, of a module
                   that settled it before the drain -- and the whole hand-over exists
                   because a second derivation is a second thing that can drift.
                   A frame whose ops say ON and whose restored atlas never
                   arrived was refused above; this is belt to that brace, and it
                   is the difference between drawing indexed and sampling the
                   dummy image as though it were art. */
                if (o->kind == TAGPU_GUIOP_SPRITE) {
                    int on = (o->col & TAGPU_GUICOL_ON) != 0;
                    /* An op that asks to sample a restored atlas we do not
                       hold cannot be drawn at all -- `prepare` refuses such a
                       frame above and this is belt to that brace. */
                    if (on && !s_arHave) {
                        sdWhy = "a restored sprite and no restored atlas on this lane";
                        goto standdown; }
                    fq[0] = (int)o->ck; fq[1] = on;     /* uCK, uRestored      */
                    quadv(qv, (float)o->sl, (float)o->st,
                          (float)(o->sl + o->fw), (float)(o->st + o->fh),
                          o->u0, o->v0, o->u1, o->v1);
                    /* BINDING 41 IS `uAtlasRGB`, and it takes the restored
                       image only on an op that says it samples one: the
                       descriptor's declared layout is SHADER_READ_ONLY, and
                       until the restorer has painted this image once it is in
                       no layout at all. `on` is exactly "there is a painted
                       picture here", so naming it is safe precisely when it
                       is read. */
                    if (!set_claim(d, s, s_atView, on ? s_arView : VK_NULL_HANDLE,
                                   QVS_SZ, TWF_SZ, &si_)) {
                        sdWhy = "this frame claimed more distinct images than there are sets";
                        goto standdown; }
                    ds = s->sets[si_];
                } else {
                    int on = (o->col & TAGPU_GUICOL_ON) != 0;
                    /* THE SOURCE HAD COLOUR OVER THERE AND NOT HERE, which is
                       our store behind theirs by at least the op that gave it
                       one. Silently drawing `uSrcHasCol = 0` would propagate
                       that gap into the destination and every copy after it. */
                    if (on && !src->colImg) {
                        sdWhy = "a copy whose source twin has colour in the producer's record and not here";
                        goto standdown; }
                    fq[0] = o->l - o->sl; fq[1] = o->t - o->st;
                    fq[2] = on;                         /* uSrcHasCol          */
                    quadv(qv, (float)o->l, (float)o->t,
                          (float)(o->r + 1), (float)(o->b + 1), 0, 0, 0, 0);
                    if (!set_claim(d, s, src->view, on ? src->colView : VK_NULL_HANDLE,
                                   QVS_SZ, TWF_SZ, &si_)) {
                    sdWhy = "this frame claimed more distinct images than there are sets";
                    goto standdown; }
                    ds = s->sets[si_];
                }
                memcpy(s->vbMap + vbOff, qv, sizeof qv);
                dyn[0] = (uint32_t)((VkDeviceSize)drawn * uStride);
                dyn[1] = (uint32_t)((VkDeviceSize)drawn * fStride);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  o->kind == TAGPU_GUIOP_SPRITE
                                    ? (t->colImg ? s_pipeSpr2 : s_pipeSpr)
                                    : (t->colImg ? s_pipeCpy2 : s_pipeCpy));
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
               off: keeping twins the producer has reset is divergence */
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
                pic_unbind(t);               /* new contents, whatever they are */
            } else {
                t = tw_find(o->surf);
                if (!t) { sdWhy = "a pixel op names a surface this store never seeded";
                          goto standdown; }
            }
            /* A RESTORABLE PICTURE GIVES ITS TWIN A COLOUR PLANE, as a
               coloured draw does -- the drain decided it (`TAGPU_GUICOL_DST`). */
            if ((o->col & TAGPU_GUICOL_DST) && !t->colImg && !tw_colour(d, t)) {
                sdWhy = "the producer gave a twin colour and this store could not";
                goto standdown;
            }
            tw_fresh(cb, t);
            if (!o->alen) break;
            if (ux < 0 || uy < 0 || ux + uw > t->w || uy + uh > t->h) {
                sdWhy = "a pixel op outside its own twin"; goto standdown; }
            /* THE TWIN'S INTERLEAVE: R = the palette index, G = 255 for
               covered -- what LAY_FS's `tap` reads (tagpu_gui_surf.c). */
            n = (unsigned)uw * (unsigned)uh;
            idx = h.arena + o->aoff;
            dst = s->stMap + stOff;
            for (j = 0; j < n; j++) { dst[2 * j] = idx[j]; dst[2 * j + 1] = 255; }
            tw_to(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            copy_rect(cb, s->stage, stOff, t->img, ux, uy, uw, uh);
            stOff += (VkDeviceSize)n * 2;
            /* AND THE COLOUR UNDER THE BOX GOES WITH IT: bytes the
               engine published are indexed art by definition, so restored
               colour left standing under them would show through a panel the
               engine has just repainted. */
            tw_col_drop(cb, t, ux, uy, uw, uh);
            /* ...UNLESS THE BYTES ARE A PICTURE THE STORE HAS RESTORED, in
               which case the colour is the restored picture's. A backdrop's
               seed binds its twin, so a restore that lands later still
               reaches it (the fill before pass 2). */
            if (o->col & TAGPU_GUICOL_ON) {
                PSENT* e = pic_get(d, cb, s, &stOff, idx, uw, uh);
                if (e && o->kind == TAGPU_GUIOP_SEED) {
                    t->pic = (int)(e - s_ps) + 1; e->pins++; s_psBound++;
                }
                if (e && e->state == PSE_DONE && pic_current(&h)) {
                    pic_fill(cb, t, e, ux, uy);
                    if (o->kind == TAGPU_GUIOP_SEED) t->picGen = s_psGen;
                }
            }
            break; }
        /* ---- THE FOCUS TINT ------------------------------------------------
           The only op in the stream that READS ITS DESTINATION: `0x4CC8DF`
           does `dst = LUT[row*256 + dst]` per pixel, and a draw may not sample
           the attachment it is writing. So this is a snapshot and then a draw,
           and it lives down here with the transfers rather than up with the
           other draws because the snapshot is a `vkCmdCopyImage`, which is
           illegal inside a render pass instance.

           WHY A COPY AND NOT A SUBPASS SELF-DEPENDENCY. Core Vulkan can do
           this in one pass -- declare the attachment as an input attachment
           too, add a `VkSubpassDependency` with `srcSubpass == dstSubpass` and
           `BY_REGION`, and `subpassLoad()` gives the current value at this
           fragment. That is the textbook answer and it is not used here, on
           blast radius, not on merit: it puts the twin attachment
           in `VK_IMAGE_LAYOUT_GENERAL` for EVERY op of EVERY twin, adds a
           binding to the descriptor set layout that every other draw's set
           must then fill with its own destination, and rewrites the
           synchronisation of a shared, load-bearing path -- the one class this
           project's own rules single out as failing silently. A copy plus a
           draw costs one render-pass instance per tinted edge and touches
           nothing but this case. If the cost ever shows up in a frame time,
           the self-dependency is the measured next step.

           SIX RECTANGLES, TWENTY-FOUR EDGES, PER FOCUSED GADGET.
           `0x4A16F0` walks levels 31, 28, 24, 19, 13, 6 over rectangles
           expanding by one pixel a step, so a menu with a focused button
           spends 24 of these a draw. Each is one pixel thick. */
        case TAGPU_GUIOP_TINT: {
            VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            VkImageCopy rgn;
            float qv[24];
            float* uq;
            int* fq;
            uint32_t dynb[2];
            VkDeviceSize vbOff;
            VkDescriptorSet dst_;
            int si_ = 0;
            int x0 = o->l, y0 = o->t, x1 = o->r, y1 = o->b;
            t = tw_find(o->surf);
            /* THE SAME REFUSAL EVERY OTHER DRAW MAKES: the drain had a twin
               and this store does not, which is the definition of behind. */
            if (!t) { sdWhy = "an op names a surface this store never seeded"; goto standdown; }
            if (!s_lutHave) { sdWhy = "a tint before the shade table was uploaded"; goto standdown; }
            if ((o->col & TAGPU_GUICOL_DST) && !t->colImg) {
                if (!tw_colour(d, t)) {
                    sdWhy = "the producer gave a twin colour and this store could not";
                    goto standdown;
                }
            }
            tw_fresh(cb, t);
            /* CLAMPED TO THE TWIN BEFORE ANYTHING ELSE, because both the copy
               region and the render area are undefined outside it. The
               producer already clips to the surface; this is this file's own
               bound on a value it did not compute. */
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > t->w - 1) x1 = t->w - 1;
            if (y1 > t->h - 1) y1 = t->h - 1;
            if (x0 > x1 || y0 > y1) break;
            /* THE SNAPSHOT IMAGE COVERS THE WHOLE TWIN, not just the box, so
               that the copy lands at the SAME COORDINATES and `TINT_FS` can
               index it with `gl_FragCoord` -- no offset uniform, nothing to
               get the sign of wrong. Grown and never shrunk. */
            if (s_tintW < t->w || s_tintH < t->h) {
                int nw = s_tintW > t->w ? s_tintW : t->w;
                int nh = s_tintH > t->h ? s_tintH : t->h;
                /* RETIRED, NOT DESTROYED: an earlier frame's tint draw may
                   still be in flight naming this view. */
                if (!ret_push(d, s_tintImg, s_tintMem, s_tintView, VK_NULL_HANDLE)) goto refuse;
                s_tintImg = VK_NULL_HANDLE; s_tintMem = VK_NULL_HANDLE;
                s_tintView = VK_NULL_HANDLE; s_tintW = s_tintH = 0;
                if (!mk_image(d, nw, nh, VK_FORMAT_R8G8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              &s_tintImg, &s_tintMem, &s_tintView)) goto refuse;
                s_tintW = nw; s_tintH = nh;
                s_tintLay = VK_IMAGE_LAYOUT_UNDEFINED;
            }
            tw_to(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            lay_to(cb, s_tintImg, &s_tintLay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            memset(&rgn, 0, sizeof rgn);
            rgn.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rgn.srcSubresource.layerCount = 1;
            rgn.dstSubresource = rgn.srcSubresource;
            rgn.srcOffset.x = x0; rgn.srcOffset.y = y0;
            rgn.dstOffset = rgn.srcOffset;
            rgn.extent.width = (uint32_t)(x1 - x0 + 1);
            rgn.extent.height = (uint32_t)(y1 - y0 + 1);
            rgn.extent.depth = 1;
            /* ONLY THE INDEX PLANE IS SNAPSHOTTED. The colour plane is shaded
               by the blend instead (`tint_table`), which needs no read of it. */
            vkCmdCopyImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s_tintImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
            lay_to(cb, s_tintImg, &s_tintLay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            tw_to(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            if (!set_claim(d, s, s_tintView, s_lutView, QVS_SZ, TWF_SZ, &si_)) {
                sdWhy = "this frame claimed more distinct images than there are sets";
                goto standdown; }
            dst_ = s->sets[si_];
            uq = (float*)(s->ubMap + (VkDeviceSize)drawn * uStride);
            fq = (int*)(s->fbMap + (VkDeviceSize)drawn * fStride);
            vbOff = (VkDeviceSize)quads * 24 * sizeof(float);
            uq[0] = (float)t->w; uq[1] = (float)t->h;
            fq[0] = (int)o->fg;                        /* uRow */
            quadv(qv, (float)x0, (float)y0, (float)(x1 + 1), (float)(y1 + 1), 0, 0, 0, 0);
            memcpy(s->vbMap + vbOff, qv, sizeof qv);
            dynb[0] = (uint32_t)((VkDeviceSize)drawn * uStride);
            dynb[1] = (uint32_t)((VkDeviceSize)drawn * fStride);
            rb.renderPass = t->colImg ? s_twRp2 : s_twRp;
            rb.framebuffer = t->colImg ? t->fb2 : t->fb;
            /* THE RENDER AREA IS THE BOX, NOT THE TWIN. The other passes in
               this file are opened ONCE and shared by every draw that follows,
               so their area has to be the whole attachment; a tint opens one
               of its own per edge, and both attachments LOAD and STORE, so a
               full-twin area declares that 614 KB is loaded and stored to
               write about sixty-five pixels -- twenty-four times per focused
               gadget. The scissor is already this rectangle and the viewport
               still spans the twin, so this narrows what the pass TOUCHES and
               changes nothing about what it draws.

               AND IT BOUGHT NOTHING MEASURABLE HERE, which is the honest
               half: measured with it narrowed and at the FULL twin, the two
               tint arms sat inside each other's spread, so llvmpipe evidently
               does not skip work for a partial area. It is kept because it
               states what the pass touches and because the load/store it
               declines is real on an implementation that honours it; THAT
               part is NOT verified here and must not be quoted as though it
               were. The real lever, if the cost ever matters on hardware, is
               the subpass self-dependency described above -- one render pass
               instead of twenty-four.

               THE COST ITSELF, MEASURED on this code:
               uncapped on MAINMENU at 640x480, reference setup, Xvfb +
               llvmpipe, two DLLs run in turn on one instance, three
               alternating runs an arm and eight samples a run --
               **178.7 fps without the tint against 163.1 with it, 24 samples
               each, about 8.7 %**. Run means were 177.2 / 179.7 / 179.3
               without and 162.3 / 167.5 / 159.6 with, so the no-tint arm is
               tight and the tint arm is not: treat 8.7 % as a figure with a
               few points of slop in it, not a constant. */
            rb.renderArea.offset.x = x0; rb.renderArea.offset.y = y0;
            rb.renderArea.extent.width  = (uint32_t)(x1 - x0 + 1);
            rb.renderArea.extent.height = (uint32_t)(y1 - y0 + 1);
            vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
            set_viewport(cb, t->w, t->h);
            set_scissor(cb, x0, y0, x1 - x0 + 1, y1 - y0 + 1, t->w, t->h);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              !t->colImg ? s_pipeTint : s_fitHave ? s_pipeTintFit : s_pipeTint2);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    s_ploTwin, 0, 1, &dst_, 2, dynb);
            vkCmdBindVertexBuffers(cb, 0, 1, &s->vb, &vbOff);
            vkCmdDraw(cb, 6, 1, 0, 0);
            /* CLOSED HERE, so the loop's invariant -- no pass open at the top
               of this switch -- holds for the op after this one, tint or not.
               `cur` is already NULL and stays so. */
            vkCmdEndRenderPass(cb);
            drawn++; quads++;
            /* SAID ONCE, on the first edge this lane ever draws. The counter
               behind it would otherwise be a write-only static, which `-Wall`
               cannot see; one line that proves the path is live is worth more than a
               number nothing prints. */
            if (!s_tintOps++)
                plog(d, "gui: the focus tint is drawing - surface %08X, box "
                        "(%d,%d)-(%d,%d), row %u of the lighten table",
                     o->surf, x0, y0, x1, y1, (unsigned)o->fg);
            break; }
        default: break;
        }
    }
    if (rpOpen) { vkCmdEndRenderPass(cb); rpOpen = 0; cur = NULL; }

    /* THE REPLAY GOT THROUGH, AND THAT -- NOT THE COMPOSITE -- IS THE EVIDENCE
       THE LAST FRESH START WORKED. `compose` is 0 whenever the sharp layer has
       coverage, which is every frame with a cursor on screen, so a refund
       below the composite gate (past `s_drawThis = 1`) would never happen in
       an ordinary session and every legitimate transition would count against
       the cap: four level loads and the pass muted for good. Being LEVEL is a
       property of the replay. */
    /* `!s_behind` IS PART OF THE CONDITION. While `s_behind` stands, pass 2
       applies NOTHING -- it skips every op waiting for a RESET -- so those
       frames are exactly the ones on which the store is provably NOT level.
       Counting them would let a pass waiting for a fresh start that never
       comes climb to the threshold, refund the budget, and go on asking the
       producer for a full reseed for the rest of the session with the mute
       unable to latch. */
    if (!s_behind && !s_behindMute && s_behindAsks && ++s_goodRun >= BEHIND_GOOD_RUN) {
        s_behindAsks = 0; s_goodRun = 0;
    }

    /* ---- the composite's own set and block ---- */
    if (s_behind || !compose || !s_palHave) {
        s_drawThis = 0; s_abFrame = 0; return 0;
    }
    pres = tw_find(h.presented);
    if (!pres || pres->w != h.surfW || pres->h != h.surfH) goto standdown;
    /* `uColOn` IS THE PRODUCER'S (`colourTwins`) AND THE IMAGE UNDER IT HAS TO
       BE OURS. The two agree by construction -- `TAGPU_GUICOL_DST` is what made
       both the drain's `col` mark and our colour image -- so this
       is the belt to that brace, and the alternative to it is compositing
       indexed art through a branch that says it is restored. */
    if (h.colourTwins && !pres->colImg) {
        sdWhy = "the presented twin has colour in the producer's record and none here";
        goto standdown;
    }

    /* ---- THE SHARP LAYER'S OWN PASS, BELOW THE COMPOSITE GATE, because the
       only thing that ever samples it is the composite: recorded above the
       gate, it would draw and throw away a device-resolution layer on every
       frame the composite is stood down on -- which is EVERY frame of a
       Classic++ session, where `colourTwins` clears `compose`.

       Drawn here because `prepare` is the hook
       outside the seam's render pass and a render pass may not nest -- the same
       reason the twin replay is here. It must be complete before `record`
       samples it, and it is: this is recorded earlier in the same buffer.

       NO FLIP (2.32). The layer is SAMPLED by the composite, not presented, and
       `QVS` puts quad y = 0 at attachment row 0 -- which is what "row 0 is the
       viewport's TOP" means. Only the composite flips, because only the
       composite is presented. */
    if (shOn) {
        VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        VkClearValue cv;
        int q;
        memset(&cv, 0, sizeof cv);          /* (0,0,0,0): alpha is coverage   */
        /* the three sets, one per kind, written once for the whole list */
        {
            VkDescriptorBufferInfo bi[2];
            VkDescriptorImageInfo ii[9];
            VkWriteDescriptorSet wr[15];
            int k, n = 0;
            VkImageView v40[3], v41[3], v42[3];
            v40[SDSET_FLAT] = s_dumView;  v41[SDSET_FLAT] = s_dumView;  v42[SDSET_FLAT] = s_dumView;
            VkSampler sm[9];
            /* THE PALETTE IS GUARDED LIKE EVERY SIBLING HERE: `s_palView` is
               null until the first `tagpu_pal_live()` resolves -- which is a
               frame the composite refuses but the layer is still recorded on,
               so a raw write would hand `vkUpdateDescriptorSets` a null handle
               with no validation layer to say so. */
            v40[SDSET_CURS] = s_atHave ? s_atView : s_dumView;          /* uAtlas    */
            v41[SDSET_CURS] = s_dumView;                                /* uAtlasRGB */
            v42[SDSET_CURS] = s_palHave ? s_palView : s_dumView;        /* uPal      */
            v40[SDSET_MM]   = s_mmPicHave ? s_mmPicView : s_dumView;    /* uPic      */
            v41[SDSET_MM]   = s_mmEngHave ? s_mmEngView : s_dumView;    /* uEng      */
            v42[SDSET_MM]   = s_palHave ? s_palView : s_dumView;        /* uPal      */
            memset(bi, 0, sizeof bi); memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
            bi[0].buffer = s->ub; bi[0].offset = 0; bi[0].range = QVS_SZ;
            bi[1].buffer = s->sb; bi[1].offset = 0; bi[1].range = TWF_SZ;
            for (k = 0; k < 9; k++) sm[k] = s_samp;
            /* the ONE linear tap in the module: the minimap picture, minified */
            sm[3 * SDSET_MM + 0] = s_mmPicHave ? s_sampMin : s_samp;
            for (k = 0; k < 3; k++) {
                int j;
                ii[3 * k + 0].imageView = v40[k];
                ii[3 * k + 1].imageView = v41[k];
                ii[3 * k + 2].imageView = v42[k];
                wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                wr[n].dstSet = s->shSet[k]; wr[n].dstBinding = 0;
                wr[n].descriptorCount = 1;
                wr[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                wr[n].pBufferInfo = &bi[0]; n++;
                wr[n] = wr[n - 1]; wr[n].dstBinding = 32;
                wr[n].pBufferInfo = &bi[1]; n++;
                for (j = 0; j < 3; j++) {
                    ii[3 * k + j].sampler = sm[3 * k + j];
                    ii[3 * k + j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    wr[n].dstSet = s->shSet[k];
                    wr[n].dstBinding = (uint32_t)(40 + j);
                    wr[n].descriptorCount = 1;
                    wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    wr[n].pImageInfo = &ii[3 * k + j]; n++;
                }
            }
            vkUpdateDescriptorSets(d->dev, (uint32_t)n, wr, 0, NULL);
        }
        rb.renderPass = s_sharpRp; rb.framebuffer = s->shFb;
        rb.renderArea.extent.width = (uint32_t)h.sharpW;
        rb.renderArea.extent.height = (uint32_t)h.sharpH;
        rb.clearValueCount = 1; rb.pClearValues = &cv;
        vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
        set_viewport(cb, h.sharpW, h.sharpH);
        set_scissor(cb, 0, 0, h.sharpW, h.sharpH, h.sharpW, h.sharpH);
        /* IN THE ORDER THE PRODUCER RECORDED THEM, which is load-bearing: the
           cursor is recorded last and covers the minimap where they overlap
           (the engine's own is drawn inside the flip, 0x4C67C0), and the view
           box after its own base for the same reason the engine draws them
           that way (0x466B44 then 0x466B5E). */
        for (q = 0; q < nsd; q++) {
            const TAGPU_GUISDRAW* sd = &h.sdraw[q];
            float qv[24];
            float* uq = (float*)(s->ubMap + (VkDeviceSize)(ndraw + q) * uStride);
            unsigned char* sq = s->sbMap + (VkDeviceSize)q * sStride;
            VkDeviceSize vbo = (VkDeviceSize)(nquad + q) * 24 * sizeof(float);
            uint32_t dyn[2];
            VkPipeline pipe;
            int si;
            uq[0] = (float)h.sharpW; uq[1] = (float)h.sharpH;
            memset(sq, 0, TWF_SZ);
            if (sd->kind == TAGPU_GUISK_CURSOR) {
                int* ip = (int*)sq; ip[0] = sd->ck; ip[1] = 0;   /* uCK, uRestored */
                pipe = s_pipeCurs; si = SDSET_CURS;
            } else if (sd->kind == TAGPU_GUISK_MM) {
                int* ip = (int*)sq; ip[0] = h.mmEngW; ip[1] = h.mmEngH;  /* uEngSize */
                pipe = s_pipeMM; si = SDSET_MM;
            } else {
                float* fp = (float*)sq;
                fp[0] = sd->col[0]; fp[1] = sd->col[1];
                fp[2] = sd->col[2]; fp[3] = sd->col[3];          /* uCol */
                pipe = s_pipeFlat; si = SDSET_FLAT;
            }
            quadv(qv, sd->dst[0], sd->dst[1], sd->dst[2], sd->dst[3],
                  sd->uv[0], sd->uv[1], sd->uv[2], sd->uv[3]);
            memcpy(s->vbMap + vbo, qv, sizeof qv);
            dyn[0] = (uint32_t)((VkDeviceSize)(ndraw + q) * uStride);
            dyn[1] = (uint32_t)((VkDeviceSize)q * sStride);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    s_ploSharp, 0, 1, &s->shSet[si], 2, dyn);
            vkCmdBindVertexBuffers(cb, 0, 1, &s->vb, &vbo);
            vkCmdDraw(cb, 6, 1, 0, 0);
        }
        vkCmdEndRenderPass(cb);
    }

    tw_to(cb, pres, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    {
        unsigned char* b = s->lbMap;
        int* ip; float* fp;
        memset(b, 0, LAY_SZ);
        /* `uColOn` AS `draw_layer` SET IT: the presented twin has a
           colour attachment AND the palette-validity rule says it may be read
           this frame. `&& pres->colImg` is not redundant -- `colourTwins` is
           the producer's flag, about ITS table, and a frame where our twin has
           no colour image would sample the dummy image through a live branch. The two agree by construction and
           this is what says so out loud. */
        /* THE OFFSETS ARE THE GENERATED HEADER'S, and the block is 80 bytes.
           `inc/spirv/tagpu_gui_surf.spv.h` prints the table this must match --
           read it there, never from here. */
        ip = (int*)(b + 0);   *ip = (h.colourTwins && pres->colImg) ? 1 : 0;
        ip = (int*)(b + 4);   *ip = shOn;                    /* uSharpOn      */
        ip = (int*)(b + 8);   ip[0] = h.surfW; ip[1] = h.surfH;      /* uSize */
        ip = (int*)(b + 16);  ip[0] = shOn ? h.sharpW : 1;
                              ip[1] = shOn ? h.sharpH : 1;   /* uSharpSize    */
        fp = (float*)(b + 24); fp[0] = h.scaleX; fp[1] = h.scaleY;   /* uScale */
        fp = (float*)(b + 32); fp[0] = h.vpL; fp[1] = h.vpT;
                               fp[2] = h.vpW; fp[3] = h.vpH;         /* uVp    */
        ip = (int*)(b + 48);  *ip = h.vpKey;                 /* uVpKey        */
        fp = (float*)(b + 64); fp[0] = h.hud[0]; fp[1] = h.hud[1];
                               fp[2] = h.hud[2]; fp[3] = h.hud[3];   /* uHud   */
    }
    {
        VkDescriptorBufferInfo bi;
        VkDescriptorImageInfo ii[4];
        VkWriteDescriptorSet wr[5];
        int j;
        memset(&bi, 0, sizeof bi); memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
        bi.buffer = s->lb; bi.offset = 0; bi.range = LAY_SZ;
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = s->laySet; wr[0].dstBinding = 32; wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wr[0].pBufferInfo = &bi;
        /* EVERY VIEW IS OURS: the twin this pass replayed, the palette, the
           twin's colour attachment and the device-resolution sharp layer.
           Nothing here can name TA's image. */
        ii[0].imageView = pres->view;                /* 40 uTwin              */
        ii[1].imageView = s_palView;                 /* 41 uPal               */
        ii[2].imageView = pres->colImg ? pres->colView : s_dumView;   /* 42 uTwinCol */
        ii[3].imageView = shOn ? s->shView : s_dumView;   /* 43 uSharp        */
        for (j = 0; j < 4; j++) {
            ii[j].sampler = s_samp;
            ii[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            wr[1 + j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[1 + j].dstSet = s->laySet;
            wr[1 + j].dstBinding = (uint32_t)(40 + j);
            wr[1 + j].descriptorCount = 1;
            wr[1 + j].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[1 + j].pImageInfo = &ii[j];
        }
        vkUpdateDescriptorSets(d->dev, 5, wr, 0, NULL);
    }

    /* THE COMPOSITE'S OWN QUAD, at the end of this frame's vertex buffer.
       LAY_VS takes `a.xy` as both uv and position, so it is the unit square. */
    {
        float qv[24];
        quadv(qv, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        /* PAST THE SHARP LAYER'S QUADS TOO. They occupy `nquad .. nquad +
           nsdraw - 1`, so the composite's own quad is at `nquad + nsdraw` --
           which is what the `+ nsdraw + 1` in the grow above reserves. At
           `nquad` it would sit on top of the layer's first quad. */
        memcpy(s->vbMap + (VkDeviceSize)(nquad + nsd) * 24 * sizeof(float),
               qv, sizeof qv);
        s_layQuad = (VkDeviceSize)(nquad + nsd) * 24 * sizeof(float);
    }
    s_layVp[0] = h.vpX; s_layVp[1] = h.vpY;
    s_layVp[2] = h.vpW_gl; s_layVp[3] = h.vpH_gl;
    s_presented = h.presented; s_surfW = h.surfW; s_surfH = h.surfH;
    s_uStride = uStride; s_fStride = fStride;
    s_abFrame = h.ab;
    s_drawThis = 1;
    return 1;

standdown:
    if (rpOpen) vkCmdEndRenderPass(cb);
    /* THE STORE IS BEHIND AND SOMETHING HAS TO ASK. Dropped silently,
       `tw_find(h.presented)` would answer NULL again on every later frame,
       land here again, and the pass would never draw for the rest of the
       session with no line in the log to say why. */
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

int tagpu_vk_gui_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h)
{
    SLOT* s;
    VkViewport vp;
    VkRect2D sc;
    int x, ytop, vw, vh;

    if (s_state != ST_READY || !s_drawThis) return 0;
    s_drawThis = 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    s = &s_slot[slot];

    /* THE PIPELINE IS BUILT AGAINST THE SEAM'S RENDER PASS, and rebuilt when
       that changes -- a swapchain rebuild can hand us a different one. */
    if (!s_pipeLay || s_layRp != d->rp) {
        if (s_pipeLay) { vkDestroyPipeline(d->dev, s_pipeLay, NULL); s_pipeLay = VK_NULL_HANDLE; }
        if (!build_lay_pipe(d, d->rp)) {
            plog(d, "gui: the composite pipeline would not build against the "
                    "seam's render pass - nothing drawn");
            s_layRp = VK_NULL_HANDLE;
            return 0;
        }
        s_layRp = d->rp;
    }

    /* THE FRAME'S VIEWPORT, turned over -- and this pass is one of the
       THREE that turn it over. The viewport's y counts from the BOTTOM of the
       window and Vulkan's from the top, and LAY_VS writes `1 - a.y*2`, a y-up
       clip convention: so the negative height is what puts its row 0 back at
       attachment row 0 (the file header). It is NOT a rule for every presented
       pass on this lane. The world passes are presented too and write the
       engine's screen-space y, which grows downward, so they take a positive
       height. `gui`, `fps` and `scaffold` are the ones whose shaders need the
       flip; tagpu_vk_pass.h's `flipok` has the table. */
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
    /* AND ONLY HERE. The draw is in the buffer; see the header for why the
       caller must not infer this from having called us. */
    return 1;
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
    /* nothing is kept once nothing asks for it -- the drain's copy of the op
       stream is the largest thing this lane costs when it is not drawing */
    tagpu_gui_mirror_want(0);

    if (!d->dev || !vkDestroyPipeline) {
        s_state = owed ? ST_REFUSED : ST_UNBUILT;
        s_ntw = 0;
        /* forgotten like the store's job in `pic_forget`: with no device the
           restorer forgets its job table, and a pointer kept would alias */
        s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
        s_rjSrcView = VK_NULL_HANDLE; s_rjKeepGen = 0;
        pic_forget();
        return;
    }
    /* behind the seam's vkDeviceWaitIdle: nothing of ours is in a queue, so
       the retire is emptied outright rather than one slot at a time */
    while (s_ntw) { TWIN* t = &s_tw[0];
                    if (t->fb2) vkDestroyFramebuffer(d->dev, t->fb2, NULL);
                    if (t->fb) vkDestroyFramebuffer(d->dev, t->fb, NULL);
                    kill_image(d, &t->colImg, &t->colMem, &t->colView);
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
    kill_image(d, &s_glImg,  &s_glMem,  &s_glView);
    kill_image(d, &s_mmPicImg, &s_mmPicMem, &s_mmPicView);
    kill_image(d, &s_mmEngImg, &s_mmEngMem, &s_mmEngView);
    kill_image(d, &s_dumImg, &s_dumMem, &s_dumView);
    /* the tint's two: the lighten table and the snapshot a tint samples */
    kill_image(d, &s_lutImg,  &s_lutMem,  &s_lutView);
    kill_image(d, &s_tintImg, &s_tintMem, &s_tintView);
    s_lutHave = 0; s_lutSerial = 0;
    s_fitHave = 0; s_lutPal = 0;
    s_tintW = s_tintH = 0; s_tintLay = VK_IMAGE_LAYOUT_UNDEFINED;
    s_atDim = 0; s_atHave = 0; s_atSerial = 0;
    /* THE RESTORE GOES WITH THE DEVICE. The job is freed BEFORE the image it
       paints into, and `s_rjTried` is reset with them: it records that THIS
       device refused, so a rebuilt one is entitled to be asked again. The
       producer is told there is nothing to sample, or it would go on setting
       `TAGPU_GUICOL_ON` into a lane with no atlas until its next present. */
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
    s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0; s_rjBlanks = 0;
    s_rjSrcView = VK_NULL_HANDLE; s_rjTried = 0; s_rjKeepGen = 0;
    kill_image(d, &s_arImg, &s_arMem, &s_arView);
    s_arDim = 0; s_arHave = 0; s_arSettled = 0;
    tagpu_gui_col_ready(0, s_arSettled);
    pic_down(d);
    s_colRearm = 0; s_colRearmSeen = 0;
    s_palHave = 0; s_palSerial = 0;
    s_dumReady = 0;
    s_glW = s_glH = 0; s_glHave = 0; s_glSerial = 0;
    s_mmPicW = s_mmPicH = 0; s_mmPicHave = 0; s_mmPicSerial = 0;
    s_mmEngW = s_mmEngH = 0; s_mmEngHave = 0;
    if (s_dpool) { vkDestroyDescriptorPool(d->dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_pipeSpr) { vkDestroyPipeline(d->dev, s_pipeSpr, NULL); s_pipeSpr = VK_NULL_HANDLE; }
    if (s_pipeCpy) { vkDestroyPipeline(d->dev, s_pipeCpy, NULL); s_pipeCpy = VK_NULL_HANDLE; }
    if (s_pipeStr) { vkDestroyPipeline(d->dev, s_pipeStr, NULL); s_pipeStr = VK_NULL_HANDLE; }
    if (s_pipeSpr2) { vkDestroyPipeline(d->dev, s_pipeSpr2, NULL); s_pipeSpr2 = VK_NULL_HANDLE; }
    if (s_pipeCpy2) { vkDestroyPipeline(d->dev, s_pipeCpy2, NULL); s_pipeCpy2 = VK_NULL_HANDLE; }
    if (s_pipeStr2) { vkDestroyPipeline(d->dev, s_pipeStr2, NULL); s_pipeStr2 = VK_NULL_HANDLE; }
    if (s_pipeTint) { vkDestroyPipeline(d->dev, s_pipeTint, NULL); s_pipeTint = VK_NULL_HANDLE; }
    if (s_pipeTint2) { vkDestroyPipeline(d->dev, s_pipeTint2, NULL); s_pipeTint2 = VK_NULL_HANDLE; }
    if (s_pipeTintFit) { vkDestroyPipeline(d->dev, s_pipeTintFit, NULL); s_pipeTintFit = VK_NULL_HANDLE; }
    if (s_pipeCurs) { vkDestroyPipeline(d->dev, s_pipeCurs, NULL); s_pipeCurs = VK_NULL_HANDLE; }
    if (s_pipeMM)   { vkDestroyPipeline(d->dev, s_pipeMM,   NULL); s_pipeMM   = VK_NULL_HANDLE; }
    if (s_pipeFlat) { vkDestroyPipeline(d->dev, s_pipeFlat, NULL); s_pipeFlat = VK_NULL_HANDLE; }
    if (s_pipeLay) { vkDestroyPipeline(d->dev, s_pipeLay, NULL); s_pipeLay = VK_NULL_HANDLE; }
    s_layRp = VK_NULL_HANDLE;
    if (s_ploSharp) { vkDestroyPipelineLayout(d->dev, s_ploSharp, NULL); s_ploSharp = VK_NULL_HANDLE; }
    if (s_dslSharp) { vkDestroyDescriptorSetLayout(d->dev, s_dslSharp, NULL); s_dslSharp = VK_NULL_HANDLE; }
    if (s_sharpRp) { vkDestroyRenderPass(d->dev, s_sharpRp, NULL); s_sharpRp = VK_NULL_HANDLE; }
    if (s_ploTwin) { vkDestroyPipelineLayout(d->dev, s_ploTwin, NULL); s_ploTwin = VK_NULL_HANDLE; }
    if (s_ploLay)  { vkDestroyPipelineLayout(d->dev, s_ploLay, NULL);  s_ploLay = VK_NULL_HANDLE; }
    if (s_dslTwin) { vkDestroyDescriptorSetLayout(d->dev, s_dslTwin, NULL); s_dslTwin = VK_NULL_HANDLE; }
    if (s_dslLay)  { vkDestroyDescriptorSetLayout(d->dev, s_dslLay, NULL);  s_dslLay = VK_NULL_HANDLE; }
    if (s_twRp) { vkDestroyRenderPass(d->dev, s_twRp, NULL); s_twRp = VK_NULL_HANDLE; }
    if (s_twRp2) { vkDestroyRenderPass(d->dev, s_twRp2, NULL); s_twRp2 = VK_NULL_HANDLE; }
    if (s_samp) { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_sampMin) { vkDestroySampler(d->dev, s_sampMin, NULL); s_sampMin = VK_NULL_HANDLE; }
    memset(s_setView, 0, sizeof s_setView);
    memset(s_setView2, 0, sizeof s_setView2);
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
