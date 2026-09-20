/* tagpu_vk_unit.c -- the posed unit bodies and their cast-shadow depth twins,
   drawn by Vulkan. Contract: tagpu_vk_unit.h. Phase G / G19e, the SIXTH world
   pass and the last of the gate.

   IT IS NOT A SECOND IMPLEMENTATION OF THE PASS. Everything arrives through
   `tagpu_posedraw_handover` (tagpu_posedraw.h): the vertices are the two
   streams the GL bake uploaded, reached through tagpu_posebake.h's mirrors;
   the draw ranges are the `first`/`count` the GL glDrawArrays used; the pose
   block is the bytes `upload_pose` wrote, through the same conversion; the
   uniforms are the numbers the GL draws passed; the texels are the bytes each
   GL texture was uploaded from; and the shaders are the same GLSL through
   tools/spirv-gen.py. What a 0-px comparison then compares is two rasterisers.

   ---- WHAT THIS LANDING HAD TO ANSWER, AND WHERE EACH ANSWER IS ----

   1. A PASS THAT BOTH FEEDS AND SAMPLES ANOTHER PASS'S TARGET. Every pass
      before this one either drew into the frame or drew a map for others; this
      one puts casters INTO the cast-shadow map and then samples the finished
      map in its own fragment shader. So it has four hooks rather than two and
      the seam calls three of them at three different points in the frame --
      tagpu_vk_unit.h lists them and the seam repeats the ordering at the call
      sites. The GL twin HAD exactly the same shape for exactly the same
      reason, with `tagpu_posedraw_depth_unit` and `tagpu_posedraw_unit` called
      from tagpu_native.c's composite. Both that composite (landing 11-3) and
      `_depth_unit` itself (landing 11-5d) are gone, so the shape is this
      file's own now and there is nothing to read it against.

      AND THE CASTER DRAW BINDS A SET OF ITS OWN. A descriptor set may only be
      written during its own slot's `prepare`, which is after the map is drawn;
      at `cast` time this slot's set still names the map's image from the
      PREVIOUS frame -- the same image, because the views are per slot -- and
      that image is, at that moment, the render target being written. So `cast`
      binds a second, smaller set holding the two vertex-stage buffers and no
      image at all, and the question does not arise. Two layouts, two sets per
      slot, one pipeline each.

   2. PER-UNIT UNIFORMS, WHICH IS WHAT A UNIT PASS IS. The GL twin re-uploads
      one 14 336-byte pose block and a dozen loose uniforms per unit and draws
      between the uploads; a Vulkan command buffer cannot, because every draw
      it records is submitted together. So each unit gets its own window in
      three DYNAMIC uniform buffers and the draw binds the set with three
      offsets. That is the whole of the per-unit cost and it is the pass's
      memory story:

        the POSE buffer   TAGPU_PD_BLOCK (14 336) a unit, per frame slot. The
                          block's SIZE is the 256-piece ceiling, not the model
                          -- stock's worst is 36 pieces -- but a descriptor
                          must cover the block the shader declares, so the
                          window cannot be shortened to the model. 300 units is
                          4.3 MB a slot.
        the SMALL buffer  two vertex-stage blocks (the body's and the caster's)
                          and one fragment-stage block a unit, each at a
                          device-accepted offset: 768 bytes a unit on the
                          reference device.

      Both are grown to the frame's own unit count rather than to
      TAGPU_PD_MAXHAND, and both are given back the moment a frame hands
      nothing over -- §2.28's rule, and at this size it is the rule that makes
      the pass affordable in a 32-bit address space.

   3. PER-TYPE VERTEX BUFFERS, AND A SERIAL RATHER THAN A POINTER. The GL twin
      draws every unit out of its TYPE's two static buffers, so this keeps one
      Vulkan buffer per baked stream and uploads it once. The table is keyed on
      tagpu_posebake.h's SERIAL, which is monotonic and never reused, because
      the bake's cache slots ARE reused -- key on the slot or on the entry
      pointer and a re-baked slot hands the new model the old model's vertices.
      A replaced or evicted buffer goes on the slot-bitmask retire the terrain
      pass established (§2.30): its last bit clears under its own slot's fence,
      so `pending == 0` means "no submitted command buffer names it and no
      future one will" by construction rather than by a timer.

   4. NO Y FLIP ON EITHER, AND SINCE LANDING 5b THAT IS TRUE OF THE BODY TOO.
      The caster never flipped -- it draws into the depth map, which is sampled
      and not presented, and its viewport is the shadow pass's own. The body took
      the negative viewport height every frame pass took, and that was the bug: this pass writes
      `gl_Position.y = p.y/uGame.y*2 - 1` on the engine's screen-space y, which
      grows DOWNWARD, so clip +1 is the BOTTOM of the game frame. GL's composite
      quad turns the world FBO over on the way to the window, which is why the
      game looks right. The Vulkan lane has no composite quad -- the ported
      passes draw STRAIGHT INTO THE SWAPCHAIN IMAGE -- so a negative viewport
      height, which this pass took until 2026-09-17, turned the frame over a
      SECOND time and Route D presented the world upside down. The viewport is
      positive now and clip -1 lands on row 0, the game's top row under both.
      It was invisible for eight landings because `tagpu_abshot.c` turned the GL
      half of every capture over by the same rule, so the two halves lined up and
      the A/B -- which compares the lanes to each other -- is blind to a flip they
      share. The capture takes TAGPU_ABSHOT_TOPDOWN now. VK_KHR_maintenance1 was
      needed only for the negative height, so this pass no longer requires it.

   ---- WHAT IT DOES NOT DO ----

   THE BODY RANGE ONLY. The nanoframe WIRE (`uRange` 2, GL_LINES), the Classic
   SILHOUETTE and the structure SLANT are the same program and the same bake
   and are not ported here; the hand-over counts any of them that drew inside
   the published window and the pass stands down. On a Classic++ soft-shadow
   frame -- which is the configuration the shadow work is measured in --
   tagpu_native.c draws none of them.

   CLASSIC++'s RESTORED ATLAS IS DRAWN SINCE GATE 3 of the Vulkan-only plan, so
   a frame whose twin reports `uRestored` 1 is DRAWN, through the twin's own
   colours at binding 43. It reaches this lane as the producer's frame LIST,
   which the restorer below paints into `s_arImg` here on the device; the
   read-back mirror that carried it until landing 11-5e-2b is gone with the GL
   backend that produced it. What is left of the old refusal is "has a restore
   painted anything YET", and it is no longer latched for the session.
   MEASURED: on the fixture this pass's A/B
   uses, the build before gate 3 drew NO PICTURE AT ALL with Classic++ art on
   and this one is 0 px with the cast-shadow map off, 1 px with it on -- and
   that 1 px is the shadow PCF's, established on the one configuration both
   builds can draw (gpu-status §2.37).

   THE REPLACEMENT MESHES (tagpu_hires_draw.c) ARE STILL NOT THIS PASS'S, and
   they are the last caster the shadow map's census holds that nothing on this
   side draws -- measured at 1 refused caster with one `armpw.glb` on screen and
   16 on a 257-unit crowd, because a tacli instance ships that mesh active.

   THE NATIVE 3DO STREAM'S OWN UNIT VERTICES ARE NOT A THING ANY MORE. That
   clause used to stand here beside the replacement meshes; it is wrong.
   tagpu_native.c builds no vertices for an ordinary unit since G16 step 8 --
   `nv` is 0 for the whole of its unit loop -- so both the body draw and the
   caster draw that read `firstv[i+1] - firstv[i]` are unreachable, and
   `tagpu_shadow_unit`, whose only call site is behind that same test, is never
   called. [FOUND 2026-09-16, reading the census the gate-3 measurement could
   not account for.]

   IT KNOWS NOTHING ABOUT A WINDOW. Everything arrives in TAGPU_VKPASS.
   A PASS READS NO ENGINE STATE: every value comes from the GL lane's
   hand-over, so this file is not on thread-split.allow and must never be. */

#include "tagpu_vk_pass.h"
#include "tagpu_vk_restore.h"   /* this lane restores the twin itself (landing 7e-2) */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tagpu_vk_unit.h"
#include "tagpu_vk_shadow.h"
#include "tagpu_vk_scaffold.h"
#include "tagpu_posedraw.h"
#include "tagpu_posebake.h"
#include "tagpu_gaf.h"
#include "tagpu_classicpp.h" /* aniso=: the one knob both lanes filter by */   /* tagpu_gaf_mip_off/_bytes: the restored twin's chain layout */
#include "spirv/tagpu_posedraw.spv.h"
#include "spirv/tagpu_native.spv.h"

/* the two std140 blocks, at the sizes the generated SPIR-V headers print for them */
#define VGL_SZ  176                        /* tagpu_posedraw::VS  _Globals    */
#define FGL_SZ  272                        /* tagpu_native::FS    _Globals    */
#define POSE_SZ TAGPU_PD_BLOCK             /* the Pose block, 14336           */

/* the two vertex bindings, which are the GL VAO's two buffers */
#define GEOM_STRIDE (TAGPU_PB_GEOMST * 4)  /* 32 */
#define MAT_STRIDE  (TAGPU_PB_MATST * 4)   /* 20 */

/* the fog grid is RG8 and the widest the wide-fog builder produces is well
   inside this; a bound here is what keeps a handed-over number from sizing an
   allocation (the scaffold's rule) */
#define FOG_MAXDIM 1024

/* per-type vertex buffers cached at once. tagpu_posebake.c holds 128 geometry
   entries and 256 material streams, so 512 can never be the binding limit on a
   healthy frame -- it is the bound on a leak, and eviction is LRU. */
#define VB_MAX  512
/* buffers waiting for every slot to turn over once before they are destroyed */
#define RET_MAX 64

/* ---- the entry points ----------------------------------------------------
   Resolved from the seam's `gdpa`/`gipa`, never linked, and this pass's own:
   the presentation table and a pass's barely overlap (tagpu_vk_pass.h). */
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
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage)

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
static int s_saidRestored, s_saidNoMirror, s_saidShadow, s_saidOther;
static int s_saidNoHand;                /* ...and the hand-over's own stand-down */
static int s_saidShort, s_saidCmp;      /* a latch each: one message each */
static int s_saidFog, s_saidScaf, s_saidVbFull;
static int s_saidRgbImg;                /* the restored twin's image was refused */
static int s_saidAniso;                 /* ...and its filter could not be matched */

static VkDescriptorSetLayout s_dslMain, s_dslCast;
static VkPipelineLayout      s_ploMain, s_ploCast;
static VkPipeline            s_pipeBody, s_pipeGhost, s_pipeCast;
static VkRenderPass          s_castRp;     /* what s_pipeCast was built against */
static VkDescriptorPool      s_dpool;
static VkSampler             s_samp, s_sampCmp, s_sampTwin;
/* what `s_sampTwin` asked for and what the device allowed, kept apart on
   purpose (11-5e-2c): the agreement test compares the two CONFIGURATIONS --
   `s_twinAnisoWant` against the producer's published knob -- and deliberately
   does NOT catch a device that cannot offer the feature, because standing
   down for that draws no unit at all on a machine that can do nothing about
   it. The comment here used to say "compared against what the GL twin got". */
static float                 s_twinAniso;      /* what the device applied   */
static float                 s_twinAnisoWant;  /* what the knob asked for   */
static int                   s_cmpLinear;  /* the compare sampler is the twin's */
static VkDeviceSize          s_ualign;

/* ---- THE SHARED UNIT ATLAS. One image for every slot, as the feature pass's:
   2048 x 2048 R8 is 4 MB, and per-slot it would be 16 MB of device-local in a
   32-bit address space whose largest free block is the number this phase spends
   its budget measuring. ---- */
static VkImage        s_atImg;
static VkDeviceMemory s_atMem;
static VkImageView    s_atView;
static int            s_atDim;
static unsigned       s_atSerial;
static int            s_atHave;
/* THE ROWS OF THE INDEXED ATLAS THIS LANE HAS ACTUALLY UPLOADED, which is not
   the same thing as "the atlas has been uploaded at least once" -- and the
   difference is a whole shelf of black art. See `restore_want`. */
static int            s_atRows;
/* CLASSIC++'s RESTORED TWIN, RGBA8 (the Vulkan-only plan's gate 3).

   IT IS THE ATLAS'S FULL SQUARE, `dim x dim`, AND NOT THE ROWS THE READ-BACK
   HAS COVERED. That is not a memory decision, it is the only extent that can
   be right: the UVs the vertex stream carries are normalised against the whole
   atlas (`tagpu_gaf.c`: `u0 = x / a->dim`, `v0 = y / a->dim`), the GL twin they
   were computed for is `glTexImage2D(..., a->dim, a->dim, ...)`, and there is
   no scale uniform between them. An image of `dim x rows` makes `v = 1.0` mean
   row `rows` instead of row `dim`, so every restored texel is sampled from the
   wrong place -- by a factor of `dim / rows`, which on a half-filled shelf is
   two. The first version of this landing built it `dim x rows` and the A/B
   could not see it, because the restorer had painted nothing in that fixture
   and the branch only ever read alpha 0. [FOUND BY THE GATE-3a LANDING REVIEW,
   2026-09-16. tagpu_vk_feat.c and tagpu_vk_fx.c had it right already, which is
   what makes a pass that differs from its neighbours worth explaining.]

   BUILT ONCE AND NEVER RESIZED, which is the second half of the same decision.
   An extent that never moves means `kill_image` is never called on it in the
   middle of a frame -- and this file's own `refuse:` label says why that
   matters: the seam waited on fence[slot] ALONE, so every OTHER slot's submit
   is still executing against this shared image. The earlier version rebuilt
   whenever the read-back's high-water mark advanced, which is routinely, during
   play. [Same review, and it is the same fault twice.]

   It is still built LAZILY -- on the first frame that carries a restore list
   rather than beside the indexed atlas as the feature pass does -- so a session
   with Classic++ off never pays the 16 MB. Lazy is safe here precisely because
   the extent does not depend on the frame that triggers it.

   NOTHING IS UPLOADED INTO IT. Until landing 11-5e-2b this image had a second
   producer: a host-side RGBA8 mirror read back off the GL twin, staged and
   copied in here, with `s_arSerial`/`s_arReq` deciding whether a frame owed it
   bytes and whether the mirror had SHRUNK. That mirror was reachable only
   through `glReadPixels`, and 11-5e-2's premise -- `oglu_load_dll` has no
   caller, so opengl32.dll is never in the process -- made `tagpu_gaf.c`'s
   producer refuse at its entry guard for the life of every process. The
   published `atlasRgb` was NULL and `atlasRgbRows` 0 on every frame, so the
   upload path here was dead code holding a dead field. The restorer below is
   now the only thing that writes this image, and it writes it with
   `vkCmdDraw`, not with a copy. [Landing 11-5e-2b.] */
static VkImage        s_arImg;
static VkDeviceMemory s_arMem;
static VkImageView    s_arView;
static int            s_arDim, s_arMips;
/* THE TWIN'S PER-LEVEL VIEWS (the Vulkan-only plan's landing 7e-2), made only
   when this lane restores for itself. `s_arLvl[L]` names level L alone, and the
   restorer takes them in pairs -- level L as the attachment, level L-1 as the
   source -- which is what makes a reduction with no copy sound: the source view
   CANNOT reach the level being written. `s_arView` above still names the whole
   chain and is what the pass samples. */
static VkImageView    s_arLvl[TAGPU_VK_MAXMIP + 1];
static int            s_arLvlN;
/* THE RESTORE THIS LANE RUNS FOR ITSELF, when the producer publishes the frame
   LIST instead of the read-back. The cursor into that list is `s_rjTaken`; the
   generation is the only thing a cursor cannot survive, so `s_rjGen` is
   compared and a move restarts from 0. `s_rjChain` records that the twin's mip
   levels are reduced here too -- without it the twin has one defined level and
   a trilinear fetch reads the rest as whatever the driver left, which is why a
   chainless job is not a picture this pass may draw. */
static TAGPU_VKRJOB*  s_rjob;
static unsigned       s_rjGen, s_rjBlanks;
static int            s_rjTaken, s_rjPainted, s_rjTried, s_rjChain;
static VkImageView    s_rjSrcView, s_rjDstView;
static int            s_arHave;

/* the 1x1 stand-ins a descriptor names when there is nothing real for it: the
   scaffold on a frame with no overlay, and the depth map on a frame with none.
   A descriptor must be VALID whether or not the shader samples it. */
static VkImage        s_dumImg, s_dumDepth;
static VkDeviceMemory s_dumMem, s_dumDepthMem;
static VkImageView    s_dumView, s_dumDepthView;
static int            s_dumReady;

/* ---- the per-type vertex buffers ---------------------------------------- */
typedef struct {
    unsigned       serial;                 /* 0 = free; never reused           */
    VkBuffer       buf;
    VkDeviceMemory mem;
    VkDeviceSize   size;
    unsigned       lastFrame;
} VBENT;
static VBENT s_vb[VB_MAX];

typedef struct {
    VkBuffer       buf;
    VkDeviceMemory mem;
    unsigned       pending;                /* bit per slot still to turn over  */
} VBRET;
static VBRET s_ret[RET_MAX];

/* ---- one of these per frame slot, and every field of it is ours for the
   duration of the hooks we are handed that slot on. ---- */
typedef struct {
    VkBuffer        ubuf;                  /* the three small blocks a unit    */
    VkDeviceMemory  umem;
    unsigned char*  umap;
    VkDeviceSize    ucap;

    VkBuffer        pbuf;                  /* the pose blocks                  */
    VkDeviceMemory  pmem;
    unsigned char*  pmap;
    VkDeviceSize    pcap;

    VkBuffer        vstage;                /* this frame's vertex uploads      */
    VkDeviceMemory  vsmem;
    unsigned char*  vsmap;
    VkDeviceSize    vscap;

    VkImage         pal, lut, fogGrid, fogLut;
    VkDeviceMemory  palMem, lutMem, fogGridMem, fogLutMem;
    VkImageView     palView, lutView, fogGridView, fogLutView;
    VkBuffer        smallStage;
    VkDeviceMemory  smallMem;
    unsigned char*  smallMap;
    int             fogW, fogH;

    VkDescriptorSet dsMain, dsCast;
    int             built;
} SLOT;
static SLOT s_slot[TAGPU_VK_SLOTS];

/* ONE STAGING BUFFER CARRIES THE FOUR SMALL UPLOADS: the palette (256 x 1
   RGBA8), the shade LUT (256 x 32 R8), the fog shade LUT (256 x 1 R8), then the
   fog grid (cols x rows RG8, a different size whenever the view walks far
   enough for the grid to be re-laid). One allocation, four copy regions at four
   offsets -- and the buffer is rebuilt with the fog image, by the same call, so
   the two can never disagree about the size. Every offset is a multiple of 4
   and of its image's texel block size, which is what vkCmdCopyBufferToImage
   requires of a bufferOffset. */
#define SMALL_PALOFF 0                           /* 256 x 1  RGBA8            */
#define SMALL_SHDOFF (256 * 4)                   /* 256 x 32 R8               */
#define SMALL_FLTOFF (256 * 4 + 256 * 32)        /* 256 x 1  R8               */
#define SMALL_FIXED  (256 * 4 + 256 * 32 + 256)
#define SMALL_FOGOFF SMALL_FIXED                 /* cols x rows RG8           */

/* this frame's draw list, filled by `upload` and read by `cast` and `record` */
typedef struct {
    VkBuffer geom, mat;
    uint32_t first, count;
    uint32_t unit;                         /* its window in the slot buffers   */
    int      casts;
    int      ghost;                        /* depth writes OFF, and drawn last */
} DRAW;
static DRAW*    s_draw;
static unsigned s_drawCap, s_ndraw, s_ncast;
static int      s_scissorOn, s_vpL, s_vpT, s_vw, s_vh;
static float    s_gw, s_gh;            /* the game frame those four are in */
static int      s_shadowOn;            /* the twin drew these against a map  */
/* the strides and offsets `upload` settled and the three draw hooks bind with.
   They are the device's alignment applied to two block sizes, so they cannot
   change inside a frame -- but they are recomputed every `upload` rather than
   once at build, because `build` is where the alignment is read and a pass that
   cached them would have two places to keep in step. */
static VkDeviceSize s_uStride, s_vglOff2, s_fglOff, s_pStride;

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
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static int mk_buffer(const TAGPU_VKPASS* d, VkDeviceSize size, VkBufferUsageFlags use,
                     VkMemoryPropertyFlags props, VkBuffer* buf, VkDeviceMemory* mem,
                     unsigned char** map)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int mt;
    void* p = NULL;
    bi.size = size ? size : 1;
    bi.usage = use;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bi, NULL, buf) != VK_SUCCESS) return 0;
    vkGetBufferMemoryRequirements(d->dev, *buf, &mr);
    mt = mem_type(d, mr.memoryTypeBits, props);
    if (mt < 0) { vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0; }
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(d->dev, &ai, NULL, mem) != VK_SUCCESS) {
        vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0;
    }
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) {
        vkFreeMemory(d->dev, *mem, NULL); vkDestroyBuffer(d->dev, *buf, NULL);
        *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; return 0;
    }
    if (map) {
        if (vkMapMemory(d->dev, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
            vkFreeMemory(d->dev, *mem, NULL); vkDestroyBuffer(d->dev, *buf, NULL);
            *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; return 0;
        }
        *map = (unsigned char*)p;
    }
    return 1;
}

static void kill_buffer(const TAGPU_VKPASS* d, VkBuffer* buf, VkDeviceMemory* mem,
                        unsigned char** map)
{
    if (map && *map) { vkUnmapMemory(d->dev, *mem); *map = NULL; }
    if (*buf) { vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; }
    if (*mem) { vkFreeMemory(d->dev, *mem, NULL); *mem = VK_NULL_HANDLE; }
}

/* `mips` is the number of MIP LEVELS, not the top level index: 1 is an image
   with level 0 alone, which is what every caller but the Classic++ restored
   twin wants. The twin is mipped because ITS GL ORIGINAL IS -- tagpu_gaf.c
   gives it GL_LINEAR_MIPMAP_LINEAR to GL_TEXTURE_MAX_LEVEL -- and a single
   level sampled against that is a different picture wherever a unit is
   minified, which at ordinary zoom is everywhere. */
static int mk_image(const TAGPU_VKPASS* d, int w, int h, int mips, VkFormat fmt,
                    VkImageUsageFlags use, VkImageAspectFlags aspect,
                    VkImage* img, VkDeviceMemory* mem, VkImageView* view)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    int mt;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = (uint32_t)w; ii.extent.height = (uint32_t)h; ii.extent.depth = 1;
    ii.mipLevels = (uint32_t)(mips > 0 ? mips : 1); ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = use;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    /* ON FAILURE, ALL THREE OUT-PARAMS ARE NULL, from every exit. The three
       early returns used to leave the view -- and two of them the memory -- as
       the caller found it, so the guarantee held only at the call sites that
       happen to `kill_image` first. A guarantee that is a property of the
       CALLER is the kind that a new call site does not inherit. */
    if (vkCreateImage(d->dev, &ii, NULL, img) != VK_SUCCESS) {
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *view = VK_NULL_HANDLE;
        return 0;
    }
    vkGetImageMemoryRequirements(d->dev, *img, &mr);
    mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) {
        vkDestroyImage(d->dev, *img, NULL);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *view = VK_NULL_HANDLE;
        return 0;
    }
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(d->dev, &ai, NULL, mem) != VK_SUCCESS) {
        vkDestroyImage(d->dev, *img, NULL);
        *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *view = VK_NULL_HANDLE;
        return 0;
    }
    if (vkBindImageMemory(d->dev, *img, *mem, 0) != VK_SUCCESS) goto bad;
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = (uint32_t)(mips > 0 ? mips : 1);
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &vi, NULL, view) != VK_SUCCESS) goto bad;
    return 1;
bad:
    vkFreeMemory(d->dev, *mem, NULL); vkDestroyImage(d->dev, *img, NULL);
    /* AND THE VIEW, which this label used to leave as the caller found it.
       vkCreateImageView's out-param is undefined on failure, every caller's
       `kill_image` destroys `*view` on the strength of it being non-NULL, and
       three of the EIGHT call sites here hand it an out-param that has held a
       live handle earlier in the session. Nulling it here closes all eight at
       once; `atlas_rgb_build` carried a second `kill_image` to close one.
       [The gate-3a re-review's finding 3.] */
    *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *view = VK_NULL_HANDLE;
    return 0;
}

static void kill_image(const TAGPU_VKPASS* d, VkImage* img, VkDeviceMemory* mem,
                       VkImageView* view)
{
    if (*view) { vkDestroyImageView(d->dev, *view, NULL); *view = VK_NULL_HANDLE; }
    if (*img)  { vkDestroyImage(d->dev, *img, NULL);  *img = VK_NULL_HANDLE; }
    if (*mem)  { vkFreeMemory(d->dev, *mem, NULL);    *mem = VK_NULL_HANDLE; }
}

static VkShaderModule mk_module(const TAGPU_VKPASS* d, const uint32_t* w, size_t words)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    ci.codeSize = words * 4;
    ci.pCode = w;
    if (vkCreateShaderModule(d->dev, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

static int resolve(const TAGPU_VKPASS* d)
{
#define GETI(n) n = (PFN_##n)d->gipa(d->inst, #n); if (!n) return 0;
#define GETD(n) n = (PFN_##n)d->gdpa(d->dev,  #n); if (!n) return 0;
    IFNS(GETI)
    DFNS(GETD)
#undef GETI
#undef GETD
    return 1;
}

/* ---- barriers ----------------------------------------------------------- */
static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                        uint32_t levels, VkImageLayout from, VkImageLayout to,
                        VkPipelineStageFlags srcStage, VkAccessFlags srcAcc,
                        VkPipelineStageFlags dstStage, VkAccessFlags dstAcc)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = levels ? levels : 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAcc; b.dstAccessMask = dstAcc;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void copy_rect(VkCommandBuffer cb, VkBuffer src, VkDeviceSize srcOff,
                      VkImage dst, int level, int w, int h)
{
    VkBufferImageCopy rg;
    memset(&rg, 0, sizeof rg);
    rg.bufferOffset = srcOff;
    rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.imageSubresource.mipLevel = (uint32_t)level;
    rg.imageSubresource.layerCount = 1;
    rg.imageExtent.width = (uint32_t)w;
    rg.imageExtent.height = (uint32_t)h;
    rg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, src, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
}

/* ---- the per-type buffer table ------------------------------------------ */
/* THE RETIRE'S ACCOUNTING, FIRST AND UNCONDITIONALLY in `upload` -- the
   terrain pass's rule and the reason it is that rule: every path below can
   return early, and the path that returns early is exactly the path that must
   still clear this slot's bit, or the retire stalls for ever. */
static void ret_slot_done(const TAGPU_VKPASS* d, uint32_t slot)
{
    int i;
    for (i = 0; i < RET_MAX; i++) {
        if (!s_ret[i].buf) continue;
        s_ret[i].pending &= ~(1u << slot);
        if (s_ret[i].pending == 0) {
            kill_buffer(d, &s_ret[i].buf, &s_ret[i].mem, NULL);
            s_ret[i].pending = 0;
        }
    }
}

/* Hand a buffer to the retire, or -- if the list is full -- say so. A full
   list is not a reason to destroy it here: this is the middle of a frame and
   other slots' submitted command buffers may still name it. */
static int ret_push(const TAGPU_VKPASS* d, VkBuffer buf, VkDeviceMemory mem)
{
    int i;
    for (i = 0; i < RET_MAX; i++)
        if (!s_ret[i].buf) {
            s_ret[i].buf = buf; s_ret[i].mem = mem;
            /* EVERY SLOT, not only the live ones: `d->slots` can only shrink at
               a swapchain rebuild, which goes through vkDeviceWaitIdle anyway,
               and a bit for a slot that never comes round again would hold the
               buffer for ever. So the mask is exactly the slots the seam
               drives. */
            s_ret[i].pending = (d->slots >= 32) ? 0xFFFFFFFFu
                                                : ((1u << d->slots) - 1u);
            return 1;
        }
    return 0;
}

static VBENT* vb_find(unsigned serial)
{
    int i;
    if (!serial) return NULL;
    for (i = 0; i < VB_MAX; i++)
        if (s_vb[i].serial == serial) return &s_vb[i];
    return NULL;
}

static VBENT* vb_slot(const TAGPU_VKPASS* d, unsigned frame)
{
    int i, worst = -1;
    for (i = 0; i < VB_MAX; i++)
        if (!s_vb[i].serial) return &s_vb[i];
    /* the least recently drawn, and never one drawn THIS frame: the frame in
       hand has already recorded draws naming it */
    for (i = 0; i < VB_MAX; i++) {
        if (s_vb[i].lastFrame == frame) continue;
        if (worst < 0 || s_vb[i].lastFrame < s_vb[worst].lastFrame) worst = i;
    }
    if (worst < 0) return NULL;
    if (!ret_push(d, s_vb[worst].buf, s_vb[worst].mem)) return NULL;
    memset(&s_vb[worst], 0, sizeof s_vb[worst]);
    return &s_vb[worst];
}

/* ---- the slot ----------------------------------------------------------- */
static void slot_free_sized(const TAGPU_VKPASS* d, SLOT* s)
{
    kill_buffer(d, &s->ubuf, &s->umem, &s->umap);
    kill_buffer(d, &s->pbuf, &s->pmem, &s->pmap);
    kill_buffer(d, &s->vstage, &s->vsmem, &s->vsmap);
    s->ucap = s->pcap = s->vscap = 0;
}

static void slot_free(const TAGPU_VKPASS* d, SLOT* s)
{
    slot_free_sized(d, s);
    kill_image(d, &s->pal, &s->palMem, &s->palView);
    kill_image(d, &s->lut, &s->lutMem, &s->lutView);
    kill_image(d, &s->fogGrid, &s->fogGridMem, &s->fogGridView);
    kill_image(d, &s->fogLut, &s->fogLutMem, &s->fogLutView);
    kill_buffer(d, &s->smallStage, &s->smallMem, &s->smallMap);
    s->fogW = s->fogH = 0;
    s->built = 0;
}

/* the three fixed-size images and the staging buffer that feeds all four */
static int slot_build(const TAGPU_VKPASS* d, SLOT* s)
{
    if (s->built) return 1;
    if (!mk_image(d, 256, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s->pal, &s->palMem, &s->palView))
        return 0;
    if (!mk_image(d, 256, 32, 1, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s->lut, &s->lutMem, &s->lutView))
        return 0;
    if (!mk_image(d, 256, 1, 1, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s->fogLut, &s->fogLutMem, &s->fogLutView))
        return 0;
    s->built = 1;
    return 1;
}

/* the fog grid image and the one staging buffer, together: a grid whose
   dimensions moved rebuilds both, so the buffer and the image it feeds cannot
   disagree about the size */
static int slot_fog(const TAGPU_VKPASS* d, SLOT* s, int w, int h)
{
    VkDeviceSize need;
    if (s->fogGrid && s->fogW == w && s->fogH == h && s->smallStage) return 1;
    kill_image(d, &s->fogGrid, &s->fogGridMem, &s->fogGridView);
    kill_buffer(d, &s->smallStage, &s->smallMem, &s->smallMap);
    s->fogW = s->fogH = 0;
    if (!mk_image(d, w, h, 1, VK_FORMAT_R8G8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s->fogGrid, &s->fogGridMem, &s->fogGridView))
        return 0;
    need = (VkDeviceSize)SMALL_FIXED + (VkDeviceSize)w * h * 2;
    if (!mk_buffer(d, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->smallStage, &s->smallMem, &s->smallMap))
        return 0;
    s->fogW = w; s->fogH = h;
    return 1;
}

/* Grow one of the slot's three sized buffers to what this frame needs. They are
   grown, never shrunk within a session of drawing, and given back whole the
   first frame the pass is handed nothing -- §2.28's rule. */
static int slot_sized(const TAGPU_VKPASS* d, SLOT* s,
                      VkDeviceSize ubytes, VkDeviceSize pbytes)
{
    if (s->ucap < ubytes) {
        kill_buffer(d, &s->ubuf, &s->umem, &s->umap);
        s->ucap = 0;
        if (!mk_buffer(d, ubytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &s->ubuf, &s->umem, &s->umap)) return 0;
        s->ucap = ubytes;
    }
    if (s->pcap < pbytes) {
        kill_buffer(d, &s->pbuf, &s->pmem, &s->pmap);
        s->pcap = 0;
        if (!mk_buffer(d, pbytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &s->pbuf, &s->pmem, &s->pmap)) return 0;
        s->pcap = pbytes;
    }
    return 1;
}

static int slot_vstage(const TAGPU_VKPASS* d, SLOT* s, VkDeviceSize bytes)
{
    if (s->vscap >= bytes && s->vstage) return 1;
    kill_buffer(d, &s->vstage, &s->vsmem, &s->vsmap);
    s->vscap = 0;
    if (!mk_buffer(d, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s->vstage, &s->vsmem, &s->vsmap)) return 0;
    s->vscap = bytes;
    return 1;
}

/* ---- build -------------------------------------------------------------- */
static int build_samplers(const TAGPU_VKPASS* d)
{
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    /* THE TWIN'S: every INDEXED texture this shader samples is GL_NEAREST with
       GL_CLAMP_TO_EDGE -- the atlas, the shade LUT, the palette, the scaffold
       and the fog pair -- because every one of them is looked up by an exact
       texel and a filtered fetch would blend two palette indices.
       THE RESTORED TWIN IS THE EXCEPTION AND GETS ITS OWN SAMPLER BELOW: it
       holds true colour, so the GL twin filters it, and NEAREST against that is
       a different picture. [Read as "every texture" until the gate-3a review;
       it was true until binding 43 stopped being a placeholder.] */
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = si.addressModeU;
    si.addressModeW = si.addressModeU;
    si.maxLod = 0.25f;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(d->dev, &si, NULL, &s_samp) != VK_SUCCESS) return 0;

    /* THE COMPARE SAMPLER IS THE SHADOW MAP'S, and it is LINEAR because the GL
       twin's is (GL_COMPARE_REF_TO_TEXTURE + GL_LEQUAL + MIN/MAG LINEAR): the
       bilinear filtering is half of what makes the penumbra smooth. Linear
       filtering of a DEPTH format is a feature bit, so it is asked of the
       format the shadow pass actually chose rather than assumed -- and a device
       that will not offer it keeps a NEAREST sampler and stands the pass down
       on a frame that would really sample the map. The reasoning, and the VUID
       this is deliberately stricter than, is tagpu_vk_terr.c's. */
    {
        int linear = 0;
        VkFormat mapfmt = tagpu_vk_shadow_format(d, &linear);
        s_cmpLinear = linear && mapfmt != VK_FORMAT_UNDEFINED;
    }
    si.magFilter = s_cmpLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = si.magFilter;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (vkCreateSampler(d->dev, &si, NULL, &s_sampCmp) != VK_SUCCESS) return 0;

    /* THE CLASSIC++ RESTORED TWIN'S SAMPLER, AND IT IS THE GL TWIN'S SETTINGS
       READ OFF tagpu_gaf.c RATHER THAN CHOSEN. That texture is the only one
       here holding true colour instead of palette indices, and `tagpu_gaf.c`
       gives it GL_LINEAR magnification, GL_LINEAR_MIPMAP_LINEAR minification to
       GL_TEXTURE_MAX_LEVEL = `mip`, 4x anisotropy where the extension answers,
       and GL_CLAMP_TO_EDGE.

       MEASURED WHAT NEAREST COSTS, because the first version of gate 3a used
       the indexed sampler here and the A/B could not see it until the fixture
       was fixed: 2 126 of 2 132 unit pixels differing at 1024x768 and 1 523 of
       1 528 at 640x480 -- the same ~99.7 %, the same worst channel 155, the
       same first pixel -- which is the signature of a MAGNIFICATION filter and
       not of a mip level, because it does not move with the sampling rate.

       THE LOD IS NOT CLAMPED HERE, AND THAT IS DELIBERATE. GL bounds the twin
       with GL_TEXTURE_MAX_LEVEL; the equivalent is the IMAGE's own level count,
       which is exactly the levels the read-back produced, and Vulkan clamps
       sampling to it. Putting the same number in the sampler as well would be
       two places to keep in step for no gain -- and the wrong one of the two
       would be silent.

       ANISOTROPY IS ASKED FOR AT THE TWIN'S OWN RATIO, and what this sampler
       actually got is remembered SEPARATELY from what was asked for. `prepare`
       compares the producer's published knob against `s_twinAnisoWant`, the
       value read here -- not against `s_twinAniso`, the value the device
       allowed. The difference is the whole of 11-5e-2c's reversal: a device
       without `samplerAnisotropy` clamps the applied value to 0.0f, and a test
       against that stands every frame down on such a machine. */
    si.compareEnable = VK_FALSE;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.minLod = 0.0f;
    si.maxLod = VK_LOD_CLAMP_NONE;
    {
        /* THE SAME KNOB THE GL SIDE READS (`aniso=`, tagpu_classicpp.h), so the
           two lanes cannot be configured apart by accident. It is read ONCE,
           here, because a sampler cannot be rebuilt mid-frame for the reason
           the shared images cannot -- every other slot's submit still names it.
           A knob changed mid-session therefore makes the two disagree, and that
           is caught rather than ignored: the hand-over carries the knob the
           producer read, and `prepare` stands the frame down when it is not
           the one read here. */
        float want = tagpu_classicpp_light()->aniso;
        s_twinAniso = 0.0f;
        /* THE KNOB AS READ, KEPT SEPARATELY FROM WHAT THE DEVICE ALLOWED
           (11-5e-2c). `s_twinAniso` below is the applied value and is 0.0f
           wherever the extension is absent or the device's ceiling is lower;
           `s_twinAnisoWant` is what the configuration asked for. The producer
           publishes the same knob, so the agreement test compares the two
           CONFIGURATIONS and stands a frame down only when they have been
           edited apart -- never because a machine lacks a feature, which it
           can do nothing about and for which standing down means drawing no
           unit at all. */
        s_twinAnisoWant = want;
        if (want > 1.0f && d->anisook && d->maxAniso >= want) {
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy = want;
            s_twinAniso = want;
        }
    }
    if (vkCreateSampler(d->dev, &si, NULL, &s_sampTwin) != VK_SUCCESS) return 0;
    return 1;
}

static int build_layouts(const TAGPU_VKPASS* d)
{
    VkDescriptorSetLayoutBinding b[12];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    int n = 0, i;

    memset(b, 0, sizeof b);
    /* the body's: the two vertex-stage blocks, the fragment-stage one, and the
       nine samplers inc/spirv/tagpu_native.spv.h names for tagpu_native::FS */
    b[n].binding = 0;  b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; n++;
    b[n].binding = 1;  b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; n++;
    b[n].binding = 32; b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1; b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; n++;
    for (i = 40; i <= 48; i++) {
        b[n].binding = (uint32_t)i;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[n].descriptorCount = 1;
        b[n].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        n++;
    }
    dli.bindingCount = (uint32_t)n; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dslMain) != VK_SUCCESS) return 0;
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dslMain;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_ploMain) != VK_SUCCESS) return 0;

    /* THE CASTER'S NAMES NO IMAGE AT ALL -- the file header says why. Its
       fragment stage is tagpu_posedraw::DFS, which is `void main(){}`. */
    dli.bindingCount = 2;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dslCast) != VK_SUCCESS) return 0;
    pli.pSetLayouts = &s_dslCast;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_ploCast) != VK_SUCCESS) return 0;
    return 1;
}

/* The two vertex bindings, which are the GL VAO's two buffers and its seven
   attributes at the very strides and offsets tagpu_posebake.c's
   glVertexAttribPointer calls use. */
static void vertex_layout(VkVertexInputBindingDescription* vb,
                          VkVertexInputAttributeDescription* va,
                          VkPipelineVertexInputStateCreateInfo* vi)
{
    memset(vb, 0, sizeof vb[0] * 2);
    vb[0].binding = 0; vb[0].stride = GEOM_STRIDE; vb[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vb[1].binding = 1; vb[1].stride = MAT_STRIDE;  vb[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(va, 0, sizeof va[0] * 7);
    va[0].location = 0; va[0].binding = 0; va[0].format = VK_FORMAT_R32G32B32_SFLOAT; va[0].offset = 0;
    va[1].location = 1; va[1].binding = 0; va[1].format = VK_FORMAT_R32G32B32_SFLOAT; va[1].offset = 12;
    va[2].location = 2; va[2].binding = 0; va[2].format = VK_FORMAT_R32_SFLOAT;       va[2].offset = 24;
    va[3].location = 3; va[3].binding = 0; va[3].format = VK_FORMAT_R32_SFLOAT;       va[3].offset = 28;
    va[4].location = 4; va[4].binding = 1; va[4].format = VK_FORMAT_R32G32_SFLOAT;    va[4].offset = 0;
    va[5].location = 5; va[5].binding = 1; va[5].format = VK_FORMAT_R32G32_SFLOAT;    va[5].offset = 8;
    va[6].location = 6; va[6].binding = 1; va[6].format = VK_FORMAT_R32_SFLOAT;       va[6].offset = 16;
    vi->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi->pNext = NULL; vi->flags = 0;
    vi->vertexBindingDescriptionCount = 2;   vi->pVertexBindingDescriptions = vb;
    vi->vertexAttributeDescriptionCount = 7; vi->pVertexAttributeDescriptions = va;
}

static int build_body_pipeline(const TAGPU_VKPASS* d)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[2];
    VkVertexInputAttributeDescription va[7];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    int ok = 0;

    vs = mk_module(d, tagpu_spv_tagpu_posedraw_VS,
                   sizeof tagpu_spv_tagpu_posedraw_VS / 4);
    fs = mk_module(d, tagpu_spv_tagpu_native_FS,
                   sizeof tagpu_spv_tagpu_native_FS / 4);
    if (!vs || !fs) goto done;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;

    vertex_layout(vb, va, &vi);
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* CULL OFF: the GL lane never enables GL_CULL_FACE, so a unit's back faces
       are rasterised there too. */
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    /* glEnable(GL_DEPTH_TEST) + glDepthFunc(GL_LESS), and the mask is back ON
       for the bodies (tagpu_native.c restores it before the body loop). */
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.maxDepthBounds = 1.0f;

    /* glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) -- the GL FBO is
       premultiplied -- and glBlendFunc sets the alpha factors as well as the
       colour ones, so both pairs are set here. */
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
    gp.layout = s_ploMain;
    gp.renderPass = d->rp;
    gp.subpass = 0;
    ok = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL,
                                   &s_pipeBody) == VK_SUCCESS;
    /* THE GHOST PIPELINE, AND IT DIFFERS IN ONE BIT. The GL twin brackets its
       build ghosts in glDepthMask(GL_FALSE)/glDepthMask(GL_TRUE) and changes
       nothing else -- so ghosts blend with each other (the usual case is the
       cursor ghost standing on a queued ghost's own site) while units drawn
       earlier still occlude them, because the depth TEST stays on. Same
       shaders, same blend, same layout; `depthWriteEnable` alone moves.
       [Landing 6, 2026-09-17.] */
    if (ok) {
        ds.depthWriteEnable = VK_FALSE;
        ok = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL,
                                       &s_pipeGhost) == VK_SUCCESS;
        ds.depthWriteEnable = VK_TRUE;
    }
done:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

/* The caster pipeline, built against the SHADOW pass's render pass. Every piece
   of state here is tagpu_vk_shadow.c's own, because the two draw into the same
   attachment in the same render pass and a caster that tested depth differently
   from the heightfield would make a different map. */
static int build_cast_pipeline(const TAGPU_VKPASS* d, VkRenderPass rp)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[2];
    VkVertexInputAttributeDescription va[7];
    VkPipelineVertexInputStateCreateInfo vi;
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

    vs = mk_module(d, tagpu_spv_tagpu_posedraw_VS,
                   sizeof tagpu_spv_tagpu_posedraw_VS / 4);
    fs = mk_module(d, tagpu_spv_tagpu_posedraw_DFS,
                   sizeof tagpu_spv_tagpu_posedraw_DFS / 4);
    if (!vs || !fs) goto done;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs;

    vertex_layout(vb, va, &vi);
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* GL'S CLIP-SPACE Z. The shadow matrix fills [-1, 1] by construction
       (tagpu_shadow.c `mrow`), so without this the near half of every caster is
       clipped away -- §2.32 at length. The caller has already refused to get
       here without `zclipok`. */
    zc.negativeOneToOne = VK_TRUE;
    vp.pNext = &zc;
    vp.viewportCount = 1; vp.scissorCount = 1;

    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    ds.maxDepthBounds = 1.0f;

    cb.attachmentCount = 0;                 /* the subpass has no colour one   */
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
    gp.layout = s_ploCast;
    gp.renderPass = rp;
    gp.subpass = 0;
    ok = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gp, NULL,
                                   &s_pipeCast) == VK_SUCCESS;
done:
    if (vs) vkDestroyShaderModule(d->dev, vs, NULL);
    if (fs) vkDestroyShaderModule(d->dev, fs, NULL);
    return ok;
}

static int build_descriptors(const TAGPU_VKPASS* d)
{
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout lay[TAGPU_VK_SLOTS];
    VkDescriptorSet sets[TAGPU_VK_SLOTS];
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    uint32_t i;

    memset(ps, 0, sizeof ps);
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ps[0].descriptorCount = d->slots * 5;          /* 3 in main, 2 in cast     */
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = d->slots * 9;
    pi.maxSets = d->slots * 2;
    pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d->dev, &pi, NULL, &s_dpool) != VK_SUCCESS) return 0;

    for (i = 0; i < d->slots; i++) lay[i] = s_dslMain;
    ai.descriptorPool = s_dpool;
    ai.descriptorSetCount = d->slots;
    ai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &ai, sets) != VK_SUCCESS) return 0;
    for (i = 0; i < d->slots; i++) s_slot[i].dsMain = sets[i];

    for (i = 0; i < d->slots; i++) lay[i] = s_dslCast;
    if (vkAllocateDescriptorSets(d->dev, &ai, sets) != VK_SUCCESS) return 0;
    for (i = 0; i < d->slots; i++) s_slot[i].dsCast = sets[i];
    return 1;
}

/* THE RESTORED TWIN'S IMAGE: the atlas's full square, made once. See the
   declarations above for why both halves of that are forced rather than chosen.
   The only rebuild is a new ATLAS DIMENSION, which for this atlas is the
   compile-time ATLAS_DIM and therefore never happens after the first build --
   the test is kept as the assertion that it does not.

   A FAILURE HERE IS NOT FATAL AND MUST NOT BE. The view stays NULL, binding 43
   falls back to the indexed view exactly as it did before gate 3 -- so a device
   that will not give us 16 MB of RGBA8 loses restored frames rather than the
   pass. That is why it does not join the `goto refuse` family, whose label
   stops the pass for the session.

   THIS USED TO REST ON A THIRD CLAUSE -- "and the restored refusal in `prepare`
   keeps the branch unreachable" -- AND 11-5e-2c MADE IT FALSE. That refusal was
   dead code while `restored` was pinned 0; unpinning the flag woke it, and it
   did not lose restored frames, it lost the pass for the session. The refusal
   is gone and what holds the promise now is stated where it is enforced:
   `prepare` clears `h.restored` whenever the twin is not painted, and the
   binding below names the twin only when `s_arHave` says it has left
   UNDEFINED. The two are computed from the same pair, so the flag and the
   descriptor agree by construction. [The 11-5e-2c reviews.] */
/* THE REQUEST, AND WHAT THIS LANE DOES WITH IT (the Vulkan-only plan's landing
   7e-2). Landing 7d's `restore_want` in tagpu_vk_feat.c and tagpu_vk_fx.c is
   the shape and every comment there applies here; what the UNITS add is the
   mip chain, which is registered with the job and without which the twin is
   not a picture this pass may draw.

   Called from `prepare` AFTER the atlas upload, because the job reads the
   indexed atlas's view and that is where it comes to exist. */
/* HOW MANY OF THE LIST'S FRAMES THIS LANE'S SOURCE ACTUALLY HOLDS, counted
   from `first`. A frame names a rect of the INDEXED atlas, and this lane reads
   that atlas out of an image IT uploaded -- `s_atRows` rows of it. A frame
   below that line is read as zeros, and palette index 0 is opaque black, so the
   restore paints a black cell over art that is perfectly fine on the GL lane,
   whose source is the live texture `atlas_paint` writes as entries are added.

   MEASURED, and it is the whole reason this function exists: landing 7e-2's
   first working oracle run had the unit chain differing in rows 0..71 -- the
   first shelf -- with the Vulkan side holding (0,0,0,255) in 55 213 texels and
   the GL side holding art. Those were the 25 frames of the first batch, queued
   in the same frame as an upload that had covered fewer rows than they sit in.

   The count is a PREFIX because the producer appends in shelf order, so the
   first uncovered frame bounds every frame after it. Stopping there rather than
   skipping past it is what keeps the cursor meaning "everything before this is
   queued": the frames left behind are offered again on the next frame, by which
   time the atlas upload has caught up. */
static int covered_prefix(const TAGPU_RGLSL_FRAME* f, int n, int rows)
{
    int i;
    for (i = 0; i < n; i++)
        if (f[i].ay < 0 || f[i].ay + f[i].h > rows) return i;
    return n;
    /* A DEGENERATE FRAME IS NOT AN UNCOVERED ONE, and testing `h <= 0` here
       conflated them: one such entry became a permanent coverage boundary, so
       every frame behind it stayed unpainted for the life of the generation
       while `s_arHave` still said the twin was a picture. The restorer already
       refuses a degenerate frame by name and `restore_want` advances the cursor
       by what was OFFERED, which is what makes "a frame the core can never
       queue is skipped for good" true -- so this function answers the coverage
       question alone and lets `job_add` answer the other one.
       [Landing 7e-2's review, finding 4.] */
}

static void restore_want(const TAGPU_VKPASS* d, const TAGPU_PDHAND* h)
{
    int repaint, n;

    if (!h->restoreFrames || h->restoreGen == 0) {
        /* no request: the lever was never on, or the producer's list died. */
        if (s_rjob) {
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
            s_rjChain = 0; s_rjSrcView = VK_NULL_HANDLE; s_rjDstView = VK_NULL_HANDLE;
            /* AND WHAT IT PAINTED IS NO LONGER A PICTURE -- the list dying is
               the producer's out-of-memory drop, and the read-back cannot take
               over because both arm latches are one-way and the mirror was
               freed when the list armed. [The landing-7d review's finding.] */
            s_arHave = 0;
        }
        return;
    }
    /* the source moved under a live job: `atlas_upload` refuses a dimension
       change and `atlas_build` re-creates the image, so this is the narrow case
       -- and the one that reads from a destroyed view if nothing checks */
    /* THE TWIN MOVING IS THE SAME HAZARD AS THE SOURCE MOVING, one image over:
       `atlas_rgb_build` destroys the per-level views it hands the job, so a
       rebuild under a live job leaves it painting through destroyed handles.
       The rebuild is unreachable today -- the dimensions are compile-time and
       the accessor refuses a depth that moves -- which is exactly why the check
       is here rather than trusted. */
    if (s_rjob && s_rjDstView && (!s_arImg || s_rjDstView != s_arLvl[0])) {
        plog(d, "unit: the restored twin moved under a live restore - dropping it "
                "and starting over on the new one");
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
        s_rjChain = 0; s_rjSrcView = VK_NULL_HANDLE; s_rjDstView = VK_NULL_HANDLE;
        s_arHave = 0;
    }
    if (s_rjob && s_rjSrcView && s_atView && s_rjSrcView != s_atView) {
        plog(d, "unit: the indexed atlas moved under a live restore - dropping it "
                "and starting over on the new one");
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
        s_rjChain = 0; s_rjSrcView = VK_NULL_HANDLE; s_rjDstView = VK_NULL_HANDLE;
        s_arHave = 0;
    }
    if (s_rjob && s_rjGen == h->restoreGen) {
        int painted = tagpu_vk_restore_job_painted(s_rjob);
        /* ONE PAINTED FRAME MAKES THIS A PICTURE -- but only with the chain,
           because the pass samples the twin trilinearly and the levels below 0
           are undefined until something reduces them. */
        if (painted > 0 && s_rjChain) s_arHave = 1;
        if (painted != s_rjPainted) {
            s_rjPainted = painted;
            if (tagpu_vk_restore_job_idle(s_rjob))
                plog(d, "unit: restored twin painted here - %d frames of generation "
                        "%u, %d mip level(s) reduced here too, no mirror and no "
                        "read-back", painted, h->restoreGen, s_arMips);
        }
        if (tagpu_vk_restore_job_failed(s_rjob)) {
            plog(d, "unit: the restore failed on this lane - the twin stops being "
                    "drawn from, because the next atlas layout would sample it at "
                    "rects it was never painted for");
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjChain = 0;
            s_rjTried = 1;
            s_arHave = 0;
            return;
        }
        n = h->restoreN - s_rjTaken;
        /* ...AND ONLY WHAT THIS LANE'S SOURCE HOLDS */
        if (n > 0) n = covered_prefix(h->restoreFrames + s_rjTaken, n, s_atRows);
        if (n > 0) {
            int took = tagpu_vk_restore_job_add(s_rjob, h->restoreFrames + s_rjTaken, n);
            /* the cursor advances by what was OFFERED, not by what was taken:
               a frame the core can never queue is skipped for good, and
               advancing by `took` would re-offer the tail for ever. A whole-call
               failure (`took` 0 with frames offered) is the queue's own realloc
               and is transient, so the cursor stays. [Landing 7d's review.] */
            if (took > 0) {
                s_rjTaken += n;
                if (took < n)
                    plog(d, "unit: %d of %d new restore frames were refused by the "
                            "restorer (degenerate or larger than a slot) - they stay "
                            "indexed until the next generation", n - took, n);
            }
        }
        return;
    }
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; s_rjTaken = 0; s_rjChain = 0; }
    if (s_rjTried) { s_arHave = 0; return; }
    if (!s_arImg || !s_arView || !s_atView || !s_atHave) return;
    /* THE CHAIN IS A PREREQUISITE, NOT AN EXTRA. Without per-level views this
       lane cannot reduce, and a twin whose levels 1.. are undefined is a wrong
       picture wherever a unit is minified -- which is ordinary play. Standing
       the restore down leaves the pass on the indexed atlas, which is the
       shipped fallback and looks like Classic++ off rather than like a bug. */
    /* UNCONDITIONALLY, so a gap stands down LOUDLY instead of falling into
       `job_new`'s silent refusal of a null view. `s_arMips + 1` is the whole
       chain including level 0, which is the one every twin has.
       [Landing 7e-2's review, finding 2.] */
    if (s_arLvlN < s_arMips + 1) {
        plog(d, "unit: the restored twin has no per-level views, so this lane "
                "cannot reduce its own mip chain - staying indexed rather than "
                "sampling levels nothing has written");
        s_rjTried = 1;
        return;
    }
    if (!tagpu_vk_restore_up(d)) { s_rjTried = 1; return; }
    /* a repaint only over something this pass painted, and only if nothing was
       blanked since it last looked -- the blank COUNT is what a single
       `restoreRepaint` flag cannot hide. [Landing 7d's review.] */
    repaint = h->restoreRepaint && s_arHave && h->restoreBlanks == s_rjBlanks;
    s_rjob = tagpu_vk_restore_job_new(d, "unit", 3, 0, repaint,
                                      s_atImg, s_atView, s_atDim, s_atDim,
                                      h->pal,
                                      s_arImg, s_arLvl[0], s_arDim, s_arDim);
    if (!s_rjob) {
        /* IT SAYS SO. `job_new` refuses a bad argument without a word of its
           own, and `s_rjTried` is cleared only by a teardown, so a silent
           refusal here is a pass that draws nothing for the rest of the
           session and never explains why. [Landing 7e-2's review, finding 2.] */
        plog(d, "unit: the restorer would not take a job for the twin "
                "(%dx%d, %d mip level(s), %d per-level view(s)) - staying "
                "indexed for this session",
             s_arDim, s_arDim, s_arMips, s_arLvlN);
        s_rjTried = 1;
        return;
    }
    /* THE OUT PASS PAINTS LEVEL 0 THROUGH `s_arLvl[0]`, not through the
       whole-chain view: a framebuffer attachment must name exactly one level,
       and the whole-chain view named three. */
    s_rjChain = 1;
    if (s_arMips > 0) {
        s_rjChain = tagpu_vk_restore_job_chain(d, s_rjob, s_arMips, s_arDim,
                                               &s_arLvl[1], &s_arLvl[0]);
        if (!s_rjChain) {
            /* the reason is in the log. A level-0-only twin is not a picture
               this pass may sample, so the whole restore stands down rather
               than draw from levels nothing wrote. */
            tagpu_vk_restore_job_free(d, s_rjob);
            s_rjob = NULL; s_rjTried = 1; s_arHave = 0;
            return;
        }
    }
    {
        int want = covered_prefix(h->restoreFrames, h->restoreN, s_atRows);
        s_rjTaken = want > 0 ? tagpu_vk_restore_job_add(s_rjob, h->restoreFrames, want) : 0;
        if (want < h->restoreN)
            plog(d, "unit: %d of %d listed frames sit below the %d atlas rows this "
                    "lane has uploaded - they are queued when the upload reaches them, "
                    "rather than restored from texels it does not have yet",
                 h->restoreN - want, h->restoreN, s_atRows);
    }
    s_rjGen = h->restoreGen;
    s_rjBlanks = h->restoreBlanks;
    s_rjSrcView = s_atView;
    s_rjDstView = s_arLvl[0];
    s_rjPainted = 0;
    if (!repaint) s_arHave = 0;
    plog(d, "unit: restoring the twin HERE - %d of %d frames over %dx%d, "
            "generation %u%s", s_rjTaken, h->restoreN, s_arDim, s_arDim,
         h->restoreGen, repaint ? ", repaint" : "");
}

static int atlas_rgb_build(const TAGPU_VKPASS* d, int dim, int mips)
{
    int i;
    if (s_arImg && s_arDim == dim && s_arMips == mips) return 1;
    /* A LIVE JOB NAMES WHAT THE NEXT FOUR LINES DESTROY, so it goes FIRST and
       it goes from here rather than from `restore_want`. The job holds `dstFb`,
       `dstView` and every `chainFb[]` over this image and these views, and
       `job_free` retires them on the mask a submitted command buffer is bound
       by; `kill_image` below does not defer. `restore_want`'s "the twin moved"
       check is the backstop and cannot be the fix: it runs LATER in the frame,
       so by the time it looked the handles were already dead -- and it could
       not fire at all for a level-0-only twin, because it tested `s_arLvlN > 0`
       and this function leaves that 0 on exactly the paths that destroy the
       most. Reachable because `tagpu_gaf.c` demotes the atlas's `mip` to 0 at
       runtime when `glGenerateMipmap` does not resolve, and the producer
       publishes that unfiltered. [Landing 7e-2's review, finding 1.] */
    if (s_arImg && s_rjob) {
        plog(d, "unit: the restored twin is being rebuilt (%dx%d mip %d -> %dx%d "
                "mip %d) and a restore is live on the old one - dropping the job "
                "before the image it paints into goes away",
             s_arDim, s_arDim, s_arMips, dim, dim, mips);
        tagpu_vk_restore_job_free(d, s_rjob);
        s_rjob = NULL; s_rjGen = 0; s_rjTaken = 0; s_rjPainted = 0;
        s_rjChain = 0; s_rjSrcView = VK_NULL_HANDLE; s_rjDstView = VK_NULL_HANDLE;
        s_arHave = 0;
    }
    for (i = 0; i <= TAGPU_VK_MAXMIP; i++)
        if (s_arLvl[i]) { vkDestroyImageView(d->dev, s_arLvl[i], NULL); s_arLvl[i] = VK_NULL_HANDLE; }
    s_arLvlN = 0;
    kill_image(d, &s_arImg, &s_arMem, &s_arView);
    s_arDim = 0; s_arMips = 0; s_arHave = 0;
    /* COLOR_ATTACHMENT SINCE LANDING 7e-2, and it costs nothing when unused:
       the restorer paints level 0 into this image through a render pass and
       reduces the rest into it the same way, so every level is a colour
       attachment at some point. The mirror path never uses it and the usage
       flag does not change how the image is sampled. */
    if (!mk_image(d, dim, dim, mips + 1, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s_arImg, &s_arMem, &s_arView))
        /* every out-param is NULL on this path, from EVERY exit of `mk_image`
           and not merely because a `kill_image` ran before this call -- see its
           contract. This used to need a second `kill_image` here to null the
           view the failure label left behind. */
        return 0;
    s_arDim = dim; s_arMips = mips;
    /* THE PER-LEVEL VIEWS, ALL OF THEM OR NONE. A chain the restorer can only
       half address is not a chain it may reduce, so a failure here leaves
       `s_arLvlN` 0 and the twin is restored at level 0 only -- which the
       consumer then refuses to draw from, because a trilinear fetch into
       undefined levels is a wrong picture rather than a missing one. The image
       itself is kept: the mirror path uses the same one and does not need
       these. */
    /* `mips >= 0`, NOT `>= 1`. The caller admits `restoreMips == 0` by name
       (`h.restoreMips >= 0` at the build site) and that is a real
       configuration: the atlas demotes `mip` to 0 whenever `glGenerateMipmap`
       did not resolve. A level-0-only twin needs no chain, but the OUT pass
       still paints THROUGH `s_arLvl[0]`, so it needs that one view -- and with
       this loop starting at 1 there was none, `job_new` refused the null
       `dstView` silently, `s_rjTried` latched for the session and the unit pass
       stood down on EVERY frame. No units drawn at all, on any driver missing
       those two GL entry points. [Landing 7e-2's review, finding 2.] */
    if (mips >= 0 && mips <= TAGPU_VK_MAXMIP) {
        for (i = 0; i <= mips; i++) {
            VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = s_arImg;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.baseMipLevel = (uint32_t)i;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.layerCount = 1;
            if (vkCreateImageView(d->dev, &vi, NULL, &s_arLvl[i]) != VK_SUCCESS) {
                int k;
                for (k = 0; k < i; k++) {
                    vkDestroyImageView(d->dev, s_arLvl[k], NULL);
                    s_arLvl[k] = VK_NULL_HANDLE;
                }
                s_arLvl[i] = VK_NULL_HANDLE;
                plog(d, "unit: no per-level view for the restored twin at level %d - "
                        "this lane cannot reduce its own chain", i);
                return 1;
            }
        }
        s_arLvlN = mips + 1;
    }
    return 1;
}

static int atlas_build(const TAGPU_VKPASS* d, int dim)
{
    if (s_atImg && s_atDim == dim) return 1;
    kill_image(d, &s_atImg, &s_atMem, &s_atView);
    s_atDim = 0; s_atSerial = 0; s_atHave = 0; s_atRows = 0;
    if (!mk_image(d, dim, dim, 1, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s_atImg, &s_atMem, &s_atView))
        return 0;
    s_atDim = dim;
    return 1;
}

/* The two 1x1 stand-ins, cleared once and left in SHADER_READ_ONLY_OPTIMAL.
   Nothing samples them -- the shader reaches uScaf only on the `uScafOn`
   branch and the map only on `uShadowOn` -- but a descriptor has to be valid
   whether or not it is read, and an image whose contents are undefined is one
   more thing to reason about for the price of one call. */
static void dummies_ready(const TAGPU_VKPASS* d, VkCommandBuffer cb)
{
    VkClearColorValue cv;
    VkImageSubresourceRange rg;
    (void)d;
    if (s_dumReady || !s_dumImg) return;
    memset(&cv, 0, sizeof cv);
    memset(&rg, 0, sizeof rg);
    rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.levelCount = 1; rg.layerCount = 1;
    img_barrier(cb, s_dumImg, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cb, s_dumImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rg);
    img_barrier(cb, s_dumImg, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    /* The depth stand-in is CLEARED rather than merely transitioned, which is
       tagpu_vk_terr.c's answer to the same question: nothing samples it --
       taShadowAt returns 1.0 on `uShadowOn == 0` before it touches either
       sampler -- but an image whose contents are undefined is one more thing to
       reason about for the price of one call. */
    {
        VkClearDepthStencilValue dv;
        VkImageSubresourceRange drg;
        memset(&drg, 0, sizeof drg);
        drg.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        drg.levelCount = 1; drg.layerCount = 1;
        dv.depth = 1.0f; dv.stencil = 0;
        img_barrier(cb, s_dumDepth, VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearDepthStencilImage(cb, s_dumDepth,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &drg);
        img_barrier(cb, s_dumDepth, VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    s_dumReady = 1;
}

static int build(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties props;
    VkFormat dfmt;

    if (d->slots == 0 || d->slots > TAGPU_VK_SLOTS) {
        plog(d, "unit: %u frame slots is outside what this pass carries (%d)",
             (unsigned)d->slots, TAGPU_VK_SLOTS);
        return 0;
    }
    if (!resolve(d)) {
        plog(d, "unit: an entry point this pass needs did not resolve");
        return 0;
    }
    /* UNITS DEPTH-TEST AND DEPTH-WRITE. Drawing them untested would put an
       aircraft behind the hill it is over. */
    if (d->dfmt == VK_FORMAT_UNDEFINED) {
        plog(d, "unit: the seam's render pass carries no depth attachment and "
                "these draws both test and write depth - the pass stays down");
        return 0;
    }
    vkGetPhysicalDeviceProperties(d->pd, &props);
    s_ualign = props.limits.minUniformBufferOffsetAlignment;
    if (s_ualign == 0) s_ualign = 1;
    /* THE BOUND THE DRIVER SETS ON A UNIFORM WINDOW, checked rather than
       assumed -- the GL twin checks GL_MAX_UNIFORM_BLOCK_SIZE for the same
       reason and refuses to arm below it. The pose block is the big one. */
    if (props.limits.maxUniformBufferRange < POSE_SZ) {
        plog(d, "unit: maxUniformBufferRange is %u and the pose block needs %d "
                "- the pass stays down",
             (unsigned)props.limits.maxUniformBufferRange, POSE_SZ);
        return 0;
    }
    if (!build_samplers(d) || !build_layouts(d) || !build_body_pipeline(d) ||
        !build_descriptors(d))
        return 0;
    /* ONE LINE PER DEVICE, as every other ported pass writes: the numbers a
       later reader needs to check a memory figure against are the device's
       uniform alignment and what it makes the two per-unit strides, and
       neither is knowable from the source alone. */
    plog(d, "unit: up - %u frame slots, uniform offset alignment %u, %u bytes "
            "of blocks and %d of pose per unit, compare sampler %s",
         (unsigned)d->slots, (unsigned)s_ualign,
         (unsigned)(align_up(VGL_SZ, s_ualign) * 2 + align_up(FGL_SZ, s_ualign)),
         (int)align_up(POSE_SZ, s_ualign),
         s_cmpLinear ? "LINEAR" : "NEAREST (the map cannot be sampled)");
    /* the stand-ins, in the map's own format so that one compare sampler is
       valid against both (tagpu_vk_shadow_format, and tagpu_vk_terr.c's note) */
    dfmt = tagpu_vk_shadow_format(d, NULL);
    if (dfmt == VK_FORMAT_UNDEFINED) dfmt = d->dfmt;
    if (!mk_image(d, 1, 1, 1, VK_FORMAT_R8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_COLOR_BIT, &s_dumImg, &s_dumMem, &s_dumView))
        return 0;
    if (!mk_image(d, 1, 1, 1, dfmt,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_IMAGE_ASPECT_DEPTH_BIT, &s_dumDepth, &s_dumDepthMem, &s_dumDepthView))
        return 0;
    return 1;
}

/* ---- the frame ---------------------------------------------------------- */
/* The atlas, on the serial and not per frame, into the one image every slot
   shares. The write-after-read barrier orders the copy after every earlier
   frame's sampling of it: a barrier's first synchronisation scope includes
   everything submitted to this queue before it, and a write-after-read hazard
   needs only an execution dependency. */
static int atlas_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, SLOT* s,
                        const TAGPU_PDHAND* h, VkDeviceSize stageOff)
{
    VkDeviceSize bytes;
    int rows = h->atlasRows;
    if (s_atHave && s_atSerial == h->atlasSerial) return 1;
    if (rows < 1) rows = 1;
    if (rows > h->atlasDim) rows = h->atlasDim;
    bytes = (VkDeviceSize)h->atlasDim * rows;
    /* BOUNDED WHERE IT IS WRITTEN. `upload` reserves this many bytes at the top
       of the allocation's tail and its own loop refuses to spend them, so this
       cannot fire -- which is the reason to keep it: it is the assertion that
       the reservation is still being made, and it costs one compare on a frame
       that re-uploads the atlas. */
    if (stageOff + bytes > s->vscap) return 0;
    memcpy(s->vsmap + stageOff, h->atlas, (size_t)bytes);
    img_barrier(cb, s_atImg, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                s_atHave ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                s_atHave ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                         : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                s_atHave ? VK_ACCESS_SHADER_READ_BIT : 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->vstage, stageOff, s_atImg, 0, h->atlasDim, rows);
    img_barrier(cb, s_atImg, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    s_atSerial = h->atlasSerial;
    s_atRows = rows;
    s_atHave = 1;
    return 1;
}

/* THE RESTORED TWIN IS NOT STAGED AND NOT COPIED. `rgb_stage_bytes`,
   `rgb_rows_due` and `atlas_rgb_upload` lived here and put the GL read-back's
   mirror onto the device: level 0's covered rows plus every other level whole,
   at an offset past the indexed atlas's share, with the whole-square rule for a
   mirror that shrank. All three are gone with the mirror that fed them
   (11-5e-2b). Their careful parts are not lost -- the reservation-and-copy
   agreeing by construction, and the refusal that matched `prepare`'s zeroed
   count -- because `atlas_upload` above still carries both for the INDEXED
   atlas, which is the one mirror this lane still uploads.

   WHAT WRITES `s_arImg` NOW: `restore_want` below, through the restore job, on
   the device, from the indexed atlas and the palette. There is no host-side
   source for it any more, so there is no staging share to reserve and no serial
   to compare -- `s_arHave` alone says whether it is a picture. */

/* the two uniform blocks for one unit: the vertex stage's twice (the body's
   and the caster's) and the fragment stage's once, at the std140 offsets
   the generated SPIR-V headers print. A UNION, NOT A CAST: both blocks mix `int` and
   `float` members, and writing an int through a float array is the aliasing
   rule broken at -O2. */
static void fill_blocks(unsigned char* ub, const TAGPU_PDHAND* h,
                        const TAGPU_PDUREC* r, VkDeviceSize vglOff2,
                        VkDeviceSize fglOff)
{
    union { float f[68]; int i[68]; } b;

    /* ---- the vertex stage, the BODY draw ---- */
    memset(&b, 0, sizeof b);
    b.f[0] = h->gw;  b.f[1] = h->gh;                   /* uGame       vec2 @0  */
    b.f[2] = 0.0f;   b.f[3] = 0.0f;                    /* uOffset     vec2 @8  */
    b.f[4] = h->zoom;                                  /* uZoom      float @16 */
    b.f[6] = h->zoomCx; b.f[7] = h->zoomCy;            /* uZoomC      vec2 @24 */
    b.f[8] = h->depthScale;                            /* uDepthScale float @32*/
    b.f[12] = r->anchor[0]; b.f[13] = r->anchor[1];
    b.f[14] = r->anchor[2]; b.f[15] = r->anchor[3];    /* uAnchor     vec4 @48 */
    b.f[16] = r->enc;                                  /* uEnc       float @64 */
    b.f[18] = h->shd[0]; b.f[19] = h->shd[1];          /* uShd        vec2 @72 */
    b.f[20] = r->cast[0]; b.f[21] = r->cast[1];
    b.f[22] = r->cast[2];                              /* uCast       vec3 @80 */
    b.i[23] = 0;                                       /* uDepthPass   int @92 */
    /* uShadowMat @96 -- in GL the vertex and fragment stages share ONE uniform
       of this name, so the body program's vertex copy holds what
       tagpu_shadow_apply wrote. It is never READ here (the shader reaches it
       only on the uDepthPass branch), and it is written anyway because the two
       lanes are being compared and not merely made to agree. */
    memcpy(&b.f[24], h->shadowMat, 64);
    b.i[40] = 0;                                       /* uRange       int @160*/
    b.f[41] = 0.0f;                                    /* uWire      float @164*/
    memcpy(ub, b.f, VGL_SZ);

    /* ---- the vertex stage, the CASTER draw ----
       What `tagpu_posedraw_depth_begin` set before landing 11-5d deleted it:
       the shadow matrix, uDepthPass 1, uRange BODY, and the projection pair
       pinned at something finite because the vertex shader still evaluates the
       discarded branch.

       NOTHING REACHES THIS TODAY. `tagpu_vk_unit_cast` returns at `!s_ncast`
       every frame, because `h.castMat`'s and `w->casts`'s producer went with
       that same function -- see tagpu_posedraw.h's tombstone for the chain.
       The uniforms below are correct and unexercised. */
    memset(&b, 0, sizeof b);
    b.f[0] = 1.0f; b.f[1] = 1.0f;                      /* uGame                */
    b.f[4] = 1.0f;                                     /* uZoom                */
    b.f[8] = 1.0f;                                     /* uDepthScale          */
    b.f[12] = r->anchor[0]; b.f[13] = r->anchor[1];
    b.f[14] = r->anchor[2]; b.f[15] = r->anchor[3];
    b.f[16] = r->enc;
    /* uShd is NOT set on the depth program either: the twin leaves it at a
       freshly linked program's zero, and nothing the caster writes reads it. */
    b.f[20] = r->cast[0]; b.f[21] = r->cast[1]; b.f[22] = r->cast[2];
    b.i[23] = 1;                                       /* uDepthPass           */
    memcpy(&b.f[24], h->castMat, 64);                  /* uShadowMat           */
    b.i[40] = 0;                                       /* uRange = BODY        */
    memcpy(ub + vglOff2, b.f, VGL_SZ);

    /* ---- the fragment stage ---- */
    memset(&b, 0, sizeof b);
    b.i[0] = h->restored;                              /* uRestored   int @0   */
    b.i[1] = h->scafOn;                                /* uScafOn     int @4   */
    b.f[4] = h->scafP[0]; b.f[5] = h->scafP[1];
    b.f[6] = h->scafP[2]; b.f[7] = h->scafP[3];        /* uScafP     vec4 @16  */
    b.f[8]  = h->ss;                                   /* uSS       float @32  */
    b.f[9]  = h->zoom;                                 /* uZoomF    float @36  */
    b.f[10] = h->zoomCx; b.f[11] = h->zoomCy;          /* uZoomCF    vec2 @40  */
    b.f[12] = h->fogOrgX; b.f[13] = h->fogOrgY;        /* uFogOrg    vec2 @48  */
    b.f[14] = h->fogCols; b.f[15] = h->fogRows;        /* uFogDim    vec2 @56  */
    b.i[16] = r->fog;                                  /* uFog        int @64  */
    b.i[17] = 0;                                       /* uShadow     int @68  */
    b.f[18] = r->alpha;                                /* uAlpha    float @72  */
    b.f[19] = r->waterT;                               /* uWaterT   float @76  */
    b.i[20] = r->waterMode;                            /* uWaterMode  int @80  */
    b.f[21] = r->digT;                                 /* uDigT     float @84  */
    b.i[22] = r->nanoOn;                               /* uNanoOn     int @88  */
    b.f[23] = r->nanoT;                                /* uNanoT    float @92  */
    b.f[24] = r->nanoC[0]; b.f[25] = r->nanoC[1];
    b.f[26] = r->nanoC[2];                             /* uNanoC     vec3 @96  */
    b.i[27] = h->lit;                                  /* uLit        int @108 */
    b.i[28] = h->lambert;                              /* uLambert    int @112 */
    b.f[32] = h->sun[0]; b.f[33] = h->sun[1];
    b.f[34] = h->sun[2];                               /* uSun       vec3 @128 */
    b.f[35] = h->amb;                                  /* uAmb      float @140 */
    b.f[36] = h->norm;                                 /* uNorm     float @144 */
    b.i[37] = h->shadowOn;                             /* uShadowOn   int @148 */
    b.f[40] = h->shadowSun[0]; b.f[41] = h->shadowSun[1];
    b.f[42] = h->shadowSun[2];                         /* uShadowSun vec3 @160 */
    memcpy(&b.f[44], h->shadowMat, 64);                /* uShadowMat mat4 @176 */
    b.f[60] = h->shScale[0]; b.f[61] = h->shScale[1];
    b.f[62] = h->shScale[2];                           /* uShScale   vec3 @240 */
    b.f[63] = h->penumbra;                             /* uPenumbra float @252 */
    b.f[64] = h->shade;                                /* uShade    float @256 */
    memcpy(ub + fglOff, b.f, FGL_SZ);
}

/* One unit's pose block, written exactly where and exactly as far as
   `upload_pose` writes it: `np * 3` rows from 0, then `nf * 4` floats at each
   of the two packed offsets. The rest of the 14 336-byte window is never read,
   because no vertex of this model carries a piece index past `np`. */
static void fill_pose(unsigned char* pb, const TAGPU_PDHAND* h,
                      const TAGPU_PDUREC* r)
{
    int np = r->npose, nf;
    if (np < 1) return;
    if (np > TAGPU_PBMAXPIECE) np = TAGPU_PBMAXPIECE;
    nf = (np + 3) / 4;
    memcpy(pb, h->rows + (size_t)r->rowOff * 4, (size_t)np * 12 * sizeof(float));
    memcpy(pb + TAGPU_PD_FLAGOFF, h->flags + r->flagOff,
           (size_t)nf * 4 * sizeof(float));
    memcpy(pb + TAGPU_PD_VISOFF, h->vis + r->flagOff,
           (size_t)nf * 4 * sizeof(float));
}

static int draw_room(unsigned n)
{
    DRAW* q;
    unsigned want;
    if (n <= s_drawCap) return 1;
    want = s_drawCap ? s_drawCap * 2 : 128;
    while (want < n) want *= 2;
    q = (DRAW*)realloc(s_draw, (size_t)want * sizeof(DRAW));
    if (!q) return 0;
    s_draw = q; s_drawCap = want;
    return 1;
}

int tagpu_vk_unit_upload(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    TAGPU_PDHAND h;
    SLOT* s;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkDeviceSize ustride, vglOff2, fglOff, pstride, stageOff, stageNeed, atlasNeed;
    int feed = 0;
    int fogW = 1, fogH = 1, i, anyUpload = 0, fogWanted = 0;

    s_ndraw = 0; s_ncast = 0; s_drawThis = 0;

    if (s_state == ST_REFUSED) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    s = &s_slot[slot];

    /* THE RETIRE'S ACCOUNTING, FIRST AND UNCONDITIONALLY: every path below can
       return early, and the path that returns early is exactly the path that
       must still clear this slot's bit. */
    ret_slot_done(d, slot);

    /* NOTHING IS BUILT UNTIL THERE IS SOMETHING TO DRAW, AND NOTHING IS KEPT
       ONCE THERE IS NOT. The pose buffer alone is 14 336 bytes a unit a slot,
       which is the largest thing this pass owns after the atlas; giving slot
       `slot` back at this point needs no new argument and no timer, because it
       is the same instant, and the same ownership, that the rest of this
       function writes it in. */
    if (!tagpu_posedraw_handover(&h, d->frame)) {
        /* AND IT SAYS SO, ONCE. This was the pass's only silent stand-down, and
           on the vulkan-only lane it is the one that fires -- every other
           refusal below carries a `plog` with a one-shot latch, so an absent
           capture with an empty log could only be this. Throttled the same way
           they are: the ordinary case is a frame the GL twin drew no posed unit
           on, which is most frames in the shell. [The vulkan-only plan, 4b-2.] */
        if (!s_saidNoHand) {
            s_saidNoHand = 1;
            plog(d, "unit: no hand-over for frame %u - the pass published "
                    "nothing, or published it for another frame",
                 (unsigned)d->frame);
        }
        if (s_state == ST_READY) slot_free(d, s);
        return 0;
    }
    s_saidNoHand = 0;

    if (s_state == ST_UNBUILT) {
        if (!build(d)) { tagpu_vk_unit_down(d); s_state = ST_REFUSED; return 0; }
        s_state = ST_READY;
    }

    /* ---- every reason not to draw, before a byte is written ---- */

    /* EVERY REFUSAL BELOW LEAVES THROUGH `standdown`, WHICH GIVES THE SLOT
       BACK. They are all taken before `slot_build`, so not a byte has been
       recorded into `cb` that names any of this slot's buffers or images, and
       the seam waited on fence[slot] -- the same ownership argument the
       hand-over-failed path above makes, at the same instant. Without it a
       refusal that holds for a session (the scaffold armed, a build ghost on
       screen, a replacement mesh, a device that will not filter the map) keeps
       this slot's pose buffer for the life of the process: 14 336 bytes a unit
       a slot, 7.3 MB a slot at the hand-over's cap, in the 32-bit address
       space whose largest free block this phase spends its budget measuring.
       §2.28's rule is "nothing is kept once there is nothing to draw", and
       before this it held only for the frame that handed nothing over.
       [FOUND 2026-09-16, the landing review.] */

    /* A COMPARE SAMPLER THAT IS NOT THE TWIN'S, AND THE DECISION IS TAKEN
       HERE. The GL PCF is bilinear and linear filtering of a depth format is a
       feature bit; a device that will not offer it would draw a harder penumbra
       than the oracle's, so a frame that samples the map must not be drawn.

       IT IS DECIDED IN `upload` AND NOT IN `prepare` BECAUSE OF WHEN THE MAP IS
       DRAWN. `prepare` runs AFTER `cast` has put this frame's posed casters
       into the map and after the shadow pass has published it -- so a refusal
       taken there stands the BODIES down while the terrain pass goes on
       sampling a map those bodies are in, and the frame shows unit shadows
       lying on terrain with no units above them. Refusing here instead makes
       `tagpu_vk_unit_casters` answer 0, which is what the census subtracts, so
       the map is refused and the terrain stands down with it -- one decision,
       taken before anything downstream can depend on the other answer.
       `s_cmpLinear` is a device property settled in `build`, so it is knowable
       at this instant. [FOUND 2026-09-16, the landing review.] */
    if (h.shadowOn && !s_cmpLinear) {
        if (!s_saidCmp) {
            s_saidCmp = 1;
            plog(d, "unit: this device will not filter a depth format linearly "
                    "and the GL twin's shadow PCF is bilinear - nothing drawn "
                    "on a frame that samples the map");
        }
        goto standdown;
    }
    s_saidCmp = 0;

    /* THE FRAME HAS DRAWS THIS PASS DOES NOT CARRY -- a unit past the
       hand-over's cap, or one an arena would not grow for.
       NOT THE BUILD GHOST since landing 6: it is carried, and drawn by
       `tagpu_vk_unit_record_ghosts` after the effects pass.
       tagpu_posedraw.h says why the count is narrow. */
    if (h.otherDraws > 0) {
        if (!s_saidOther) {
            s_saidOther = 1;
            plog(d, "unit: the GL twin drew %d posed unit(s) this hand-over does "
                    "not carry (past its cap, or an arena that would not grow) - "
                    "nothing drawn while that is true", h.otherDraws);
        }
        goto standdown;
    }
    s_saidOther = 0;

    /* THE THREE THAT USED TO BE SILENT. Every other stand-down in this function
       carries a message; these did not, and a hand-over that succeeds and then
       stands down here is indistinguishable from one that never arrived.
       Periodic rather than latched: this landing has been sent to the wrong
       function twice by a one-shot spent on an ordinary frame. */
    if (h.nunit < 1 || h.nunit > TAGPU_PD_MAXHAND ||
        !h.units || !h.rows || !h.flags || !h.vis) {
        if ((d->frame % 300u) == 0u)
            plog(d, "unit: frame %u: the hand-over carries nunit=%d (cap %d) "
                    "units=%d rows=%d flags=%d vis=%d - nothing drawn",
                 (unsigned)d->frame, h.nunit, TAGPU_PD_MAXHAND,
                 h.units ? 1 : 0, h.rows ? 1 : 0, h.flags ? 1 : 0, h.vis ? 1 : 0);
        goto standdown;
    }

    /* THE TEXELS. The mirrors are asked for on the twin's own beat and cannot
       be there before the atlas has its dimensions, so the first frames of a
       session legitimately arrive without one. Said once. */
    if (!h.atlas || h.atlasDim < 1 || h.atlasDim > 8192 || !h.lut || !h.pal ||
        !h.fogLut) {
        /* WHICH mirror, periodically. Six terms behind one message told me
           only that one of them was missing, and a latch on top of that meant
           it said so once. [The vulkan-only plan, landing 4b-2.] */
        if ((d->frame % 300u) == 0u)
            plog(d, "unit: frame %u: mirrors atlas=%d dim=%d lut=%d pal=%d "
                    "fogLut=%d - nothing drawn until all are there",
                 (unsigned)d->frame, h.atlas ? 1 : 0, h.atlasDim,
                 h.lut ? 1 : 0, h.pal ? 1 : 0, h.fogLut ? 1 : 0);
        if (!s_saidNoMirror) {
            s_saidNoMirror = 1;
            plog(d, "unit: a texel mirror this pass needs is not there yet - "
                    "nothing drawn until it is (the unit atlas is asked for on "
                    "the twin's publish and converges over the next frames)");
        }
        goto standdown;
    }
    s_saidNoMirror = 0;
    if (h.lutW != 256 || h.lutH != 32) {
        plog(d, "unit: the shade LUT is %dx%d and this pass carries 256x32 - "
                "nothing drawn", h.lutW, h.lutH);
        goto standdown;
    }

    /* ---- CLASSIC++'s RESTORED ATLAS, WHICH IS MIRRORED SINCE GATE 3 ----

       IT SITS HERE, BELOW THE TEXEL BOUNDS, and it used to sit three refusals
       higher. The image has to be sized to `atlasDim` and to the rows the
       read-back covered, and neither number is trustworthy until the block
       above has bounded it -- a bound that lives in the file that produced the
       number is a bound only while both files are read together.

       THE ROWS ARE BOUNDED AGAINST THE ATLAS'S OWN SQUARE. The mirror IS the
       atlas's rows, so the atlas's dimension is the ceiling; over it the mirror
       is taken as absent and the frame stands down rather than size an image
       and a memcpy from a number nothing checked.

       AND THE REFUSAL IS NO LONGER A STATEMENT ABOUT THE SESSION. It used to
       say "draws nothing this session", and gpu-status 2.35 measured what that
       costs: a pass that refuses once stays dark for the process after the
       condition has cleared. This one clears by itself within a few frames of
       the restorer starting, so `s_saidRestored` gates the LOG LINE and not the
       refusal. */
    /* BUILT BEFORE THE REFUSAL THAT TESTS IT, and that order is the whole of
       it: `prepare` returning 0 skips everything below, so a build placed after
       the refusal never runs -- the pass cannot make the image because it
       refuses, and refuses because there is no image. The terrain pass was
       measured failing in exactly that shape twice on the way to gate 2, with
       a perfect hand-over against img=0 view=0. A failure is non-fatal: the
       view stays NULL and the refusal below then holds. */
    /* ...AND BECAUSE A LIST WAS PUBLISHED, which is now the only reason there
       is. The twin is the thing this lane PAINTS INTO, so it has to exist
       BEFORE there is anything to put in it. Gating its creation on a
       read-back's rows left `restore_want` with no destination, so it made no
       job, so nothing ever painted, so the pass stood down for the session with
       the list sitting there full: measured on the first run of landing 7e-2's
       oracle, which dumped three sprite atlases and no unit chain at all. The
       shape comes from the producer (`restoreDim`/`restoreMips`) for the same
       reason the aniso does: nothing else carries it. */
    if (h.restoreFrames && h.restoreDim > 0 &&
        h.restoreMips >= 0 && h.restoreMips <= TAGPU_VK_MAXMIP) {
        if (!atlas_rgb_build(d, h.restoreDim, h.restoreMips)) {
            if (!s_saidRgbImg) {
                s_saidRgbImg = 1;
                plog(d, "unit: no %d MB device image to restore the Classic++ twin "
                        "into - restored frames stand down while that is true",
                     (h.restoreDim * h.restoreDim * 4) >> 20);
            }
        }
    }
    /* AND THE FILTER HAS TO BE THE TWIN'S, not merely a filter. The restored
       atlas is the only texture this pass samples that holds true colour rather
       than palette indices, so it is the only one GL filters -- trilinear to
       its MAX_LEVEL with anisotropy where the extension answered. The mip
       levels are carried across as bytes, so those match by construction; the
       ANISOTROPY is carried as the CONFIGURED ratio on both ends, so it
       matches unless the two have been edited apart. A ratio that differs is
       a different picture wherever a unit is minified at an angle, which is
       ordinary play, so the frame stands down rather than draw one. Same rule,
       and the same shape, as `s_cmpLinear` above.

       WHAT THIS TEST DOES NOT CATCH, said plainly (11-5e-2c): a device whose
       ceiling is below the knob. That was the old test's subject and it was
       the wrong one -- the producer cannot know this device's ceiling, and
       standing down for it means drawing no units at all rather than slightly
       differently filtered ones. */
    /* `atlasRgbAniso` IS THE RATIO, AND IT SURVIVED THE MIRROR. It is a fact
       about what the OTHER lane's twin was filtered at, not about the read-back
       that used to carry it, and the producer publishes it on the list path too
       -- so a restore this lane runs for itself is held to the filter test that
       a mirror used to be held to. [Landing 7e-2; 11-5e-2 labelled the field
       `[PINNED 0]`, 11-5e-2b kept it for this test, and 11-5e-2c gave it a
       writer and changed what it is compared against.] */
    if (h.restored && h.restoreFrames && h.atlasRgbAniso != s_twinAnisoWant) {
        if (!s_saidAniso) {
            s_saidAniso = 1;
            plog(d, "unit: the Classic++ restored atlas is published for %.1fx "
                    "anisotropic and this lane's sampler was built for %.1fx - the "
                    "knob changed after the sampler was made, and a sampler cannot "
                    "be rebuilt mid-frame, so nothing is drawn on a frame that "
                    "samples it rather than differently filtered art "
                    "(the device applied %.1fx)",
                 (double)h.atlasRgbAniso, (double)s_twinAnisoWant, (double)s_twinAniso);
        }
        /* AND THE PENALTY IS THE TWIN, NOT THE PASS (11-5e-2c review, H1/M2).
           This stood the whole frame down, which was survivable only while
           `restored` was pinned 0 and the branch was unreachable. Unpinning it
           made a knob edited mid-session -- `tagpu_classicpp.cfg` is re-read
           whenever its mtime moves -- draw NO UNIT AT ALL for the rest of the
           session. Dropping to the indexed atlas is the shipped fallback and
           is what `atlas_rgb_build`'s own header promises: "a device that will
           not give us 16 MB of RGBA8 loses restored frames rather than the
           pass". */
        h.restored = 0;
    } else s_saidAniso = 0;   /* the latch, which the first cut of this fix killed:
                                 `goto standdown` used to make the line below
                                 unreachable while the condition held, so moving to a
                                 fall-through logged it EVERY frame. [The 11-5e-2c
                                 verification pass.] */

    /* A RESTORE THIS LANE RAN, and the refusal must not always return: there is
       no painted twin until a job exists, and the job is made below in
       `restore_want`. So a frame that has the list, the image and no job yet
       FALLS THROUGH to make one and then returns -- which is the deadlock
       landing 7d paid for in the sprite passes and is written the same way
       here. [Landing 7e-2.] */
    feed = 0;
    if (h.restored && !(s_arView && h.restoreFrames && s_arHave)) {
        if (h.restoreFrames && s_arImg && !s_rjTried) feed = 1;
        if (!s_saidRestored) {
            s_saidRestored = 1;
            plog(d, "unit: a Classic++ restore is armed and this lane's twin is not "
                    "painted yet - drawing the indexed atlas until it is%s",
                 feed ? " (this frame also makes the restore job)" : "");
        }
        /* AN UNPAINTED TWIN DRAWS INDEXED. ON BOTH BRANCHES (11-5e-2c review,
           H1, completed by the verification pass). This was `goto standdown`,
           and unpinning `restored` is what made it reachable: with five one-way
           `s_rjTried` latches above it blanked every unit for the rest of the
           SESSION on any restorer refusal, and -- because `rlistRepaint` is a
           constant 0, so `if (!repaint) s_arHave = 0;` fires on every
           generation change -- for the length of a repaint on every level
           boundary, recycle and map change.

           THE FIRST CUT OF THIS FIX ONLY COVERED `!feed`, WHICH IS THE RARE
           HALF. A generation change satisfies all three feed terms, so it took
           `feed = 1` and returned through `feedout` still drawing nothing --
           the routine case, unfixed, while the note claimed otherwise.

           THE STAND-DOWN'S PREMISE IS GONE EITHER WAY. It existed to avoid
           showing "a different picture from its own oracle" -- art filtered
           unlike the other lane's. There is no other lane: `render_vk.c:232`
           is the only caller of `tagpu_overlay_draw`. Indexed art is not a
           disagreement with anybody, it is Classic++ restore off for one
           frame. A bound on what the flag may promise, not a timing fix. */
        h.restored = 0;
    } else s_saidRestored = 0;

    /* THE FOG GRID, and the bound re-checked in this file's own terms. A unit
       with `uFog & 1` samples it, so a frame that wants one and has none is a
       refusal rather than a frame drawn without fog. */
    for (i = 0; i < h.nunit; i++) if (h.units[i].fog & 1) { fogWanted = 1; break; }
    if (h.fogGrid) {
        if (h.fogGridCols < 1 || h.fogGridRows < 1 ||
            h.fogGridCols > FOG_MAXDIM || h.fogGridRows > FOG_MAXDIM) {
            plog(d, "unit: a %dx%d fog grid is outside what this pass carries - "
                    "nothing drawn", h.fogGridCols, h.fogGridRows);
            goto standdown;
        }
        fogW = h.fogGridCols; fogH = h.fogGridRows;
    } else if (fogWanted) {
        if (!s_saidFog) {
            s_saidFog = 1;
            plog(d, "unit: a unit this frame samples the fog overlay and the "
                    "hand-over carries no grid - nothing drawn while that is true");
        }
        goto standdown;
    }
    s_saidFog = 0;

    /* ---- THE SCAFFOLD, AND THE HALF OF IT THIS LANDING DOES NOT CLOSE ----

       Half the question IS answered, and the mechanism is in place: the
       overlay is another pass's image, and tagpu_vk_scaffold.h now exposes a
       FRAME-STAMPED per-slot view in exactly the shape tagpu_vk_shadow.h
       settled on, which `bind_main` points binding 44 at. That is the part
       the effects and hi-res passes will reuse.

       THE `gl_FragCoord` HALF IS CLOSED BY LANDING 5b, and this paragraph used
       to say the opposite -- it is corrected here rather than deleted, because
       it ended by telling every later pass to inherit it.
       [FOUND BY THE 5b REVIEW, 2026-09-17.]

       TAGPU_GLSL_SCAF_TEST (tagpu_glsl.h) locates the fragment in the game
       frame with `gl_FragCoord.xy / uSS`, and the two APIs measure that from
       opposite edges: GL's origin is the LOWER left, Vulkan's is the UPPER left
       and `OriginUpperLeft` is the only execution mode Vulkan permits. That
       used to make them exact mirrors -- but only because this pass took a
       NEGATIVE viewport height. It no longer does (item 4). The GL twin's VS
       maps game row 0 to FBO window y 0, which tagpu_glsl.h states outright, so
       GL reads `g + 0.5` for game row g; with a positive height this lane
       stores game row g at image row g and reads `g + 0.5` as well. THE TWO
       AGREE, and the same is true of any later pass that reads `gl_FragCoord`.

       WHAT STILL STANDS THE PASS DOWN IS THE OTHER HALF, and it is a real
       bound rather than a restatement. `tagpu_vk_scaffold_view` hands back
       VK_NULL_HANDLE for a frame or slot that is not its own, and `bind_main`
       then points binding 44 at the 1x1 stand-in. A frame whose twin sampled a
       real overlay while this lane sampled one texel is a DIFFERENT PICTURE,
       so it is refused. Turning the refusal into `scafOn && !scaffold_view(...)`
       is now a small, bounded change -- but it enables a drawing path this lane
       has never measured, and it needs its own A/B, which is awkward because
       the overlay's own Vulkan pass draws in the same frame. Left for the
       landing that measures it.

       It costs nothing in play either way: `scaffold.on` is not in the default
       arm set and its own note says to leave it disarmed. */
    if (h.scafOn) {
        if (!s_saidScaf) {
            s_saidScaf = 1;
            plog(d, "unit: the GL twin is sampling the scaffold overlay, and "
                    "this lane has never measured that path - binding 44 falls "
                    "back to a 1x1 stand-in on any frame the overlay's own pass "
                    "did not hand over, which would be a different picture, so "
                    "nothing is drawn while the overlay is armed");
        }
        goto standdown;
    }
    s_saidScaf = 0;

    /* ---- the slot is ours: the seam waited on fence[slot] at the top of this
       frame, so the submit that last used these buffers, these images and these
       descriptor sets has completed. ---- */
    if (!slot_build(d, s)) goto refuse;
    if (!slot_fog(d, s, fogW, fogH)) goto refuse;
    if (!atlas_build(d, h.atlasDim)) goto refuse;

    ustride = align_up(VGL_SZ, s_ualign);
    vglOff2 = ustride;
    fglOff  = ustride * 2;
    ustride = ustride * 2 + align_up(FGL_SZ, s_ualign);
    pstride = align_up(POSE_SZ, s_ualign);
    if (!slot_sized(d, s, ustride * (VkDeviceSize)h.nunit,
                    pstride * (VkDeviceSize)h.nunit)) goto refuse;

    /* ---- what has to be uploaded this frame, and the staging to carry it ---- */
    if (!draw_room((unsigned)h.nunit)) goto refuse;
    stageNeed = 0;
    for (i = 0; i < h.nunit; i++) {
        const TAGPU_PDUREC* r = &h.units[i];
        if (!vb_find(r->geomSerial))
            stageNeed += (VkDeviceSize)r->nvert * GEOM_STRIDE;
        if (!vb_find(r->matSerial))
            stageNeed += (VkDeviceSize)r->nvert * MAT_STRIDE;
    }
    /* THE ATLAS SHARES THIS ALLOCATION AND ITS SHARE IS RESERVED, not merely
       counted. `atlas_upload` copies at `stageOff` once the loop below is done,
       so every byte the loop spends past its own budget eats into the atlas's
       room -- and the loop CAN spend more than the pre-pass counted, because an
       eviction inside it turns a later unit's cache hit into a miss. Counted
       but not reserved, an overspend of up to `atlasNeed` bytes leaves every
       unit fitting, the every-unit-or-none gate passing, and the atlas memcpy
       running that far past the end of the mapped allocation. So the loop's
       bound carries `atlasNeed` with it and `atlas_upload` re-checks its own.
       (`atlasRows` can only make the real copy smaller than this square.)
       [FOUND 2026-09-16, the re-review of the fix.] */
    atlasNeed = (!s_atHave || s_atSerial != h.atlasSerial)
                ? (VkDeviceSize)h.atlasDim * h.atlasDim : 0;
    /* THE RESTORED TWIN RESERVES NOTHING, because nothing is copied into it
       from the host any more. It used to reserve its own share here for the
       same reason the indexed atlas does -- an overspend by the loop above
       would have eaten into the room its memcpy needed -- and that share is
       gone with the mirror (11-5e-2b). The restorer paints it on the device. */
    stageNeed += atlasNeed;
    if (stageNeed) {
        if (!slot_vstage(d, s, stageNeed)) goto refuse;
    } else if (s->vstage) {
        /* nothing to upload this frame: give the staging back, as the feature
           pass gives its atlas staging back -- a settled scene holds none */
        kill_buffer(d, &s->vstage, &s->vsmem, &s->vsmap);
        s->vscap = 0;
    }

    stageOff = 0;
    for (i = 0; i < h.nunit; i++) {
        const TAGPU_PDUREC* r = &h.units[i];
        VBENT* g = vb_find(r->geomSerial);
        VBENT* m = vb_find(r->matSerial);
        DRAW* w;
        int j;

        /* THE BOUNDS, RE-CHECKED IN THIS FILE'S OWN TERMS. Each of these sizes
           an allocation or a memcpy, and a bound that lives in the file that
           produced the number is a bound only while both files are read
           together. */
        if (r->nvert < 1 || r->nvert > 262144) continue;
        if (r->count < 1 || r->first < 0 || r->first + r->count > r->nvert) continue;
        if (r->npose < 1 || r->npose > TAGPU_PBMAXPIECE) continue;
        if ((size_t)r->rowOff * 4 + (size_t)r->npose * 12 > h.nrow * 4) continue;
        if (r->flagOff + (unsigned)(((r->npose + 3) / 4) * 4) > h.nflag) continue;

        /* STAMPED THE MOMENT THEY RESOLVE, AND NOT AFTER THE LOOP BELOW.
           `vb_slot` refuses to evict an entry stamped with the frame in hand,
           and that is the ONLY thing keeping this unit's two entries alive
           across its own second allocation. Stamped after the loop instead --
           which is where these two stores used to be -- a unit whose geometry
           is cached and whose material is not hands `vb_slot` its own geometry
           entry as the least recently drawn: `ret_push` retires the buffer,
           the entry is memset and handed back for the MATERIAL, and `g` and
           `m` now name the SAME entry. The `if (*e) continue` above would then
           leave `w->geom` and `w->mat` both pointing at it, and binding 0
           would fetch 32-byte vertices out of a buffer holding 20-byte ones --
           past its end, on a draw that still passes the every-unit-or-none
           gate because the unit WAS drawn. [FOUND 2026-09-16, the landing
           review, by both reviewers independently.] */
        if (g) g->lastFrame = d->frame;
        if (m) m->lastFrame = d->frame;

        for (j = 0; j < 2; j++) {
            VBENT** e = j ? &m : &g;
            unsigned serial = j ? r->matSerial : r->geomSerial;
            int st = j ? MAT_STRIDE : GEOM_STRIDE;
            const float* src;
            int nv = 0;
            VkDeviceSize bytes;
            VkBufferCopy cp;
            if (*e) continue;
            /* THE MIRROR IS ASKED FOR WITH THE SERIAL, at the instant of the
               read and by the module that owns it -- tagpu_posebake.h. A type
               whose entry has been evicted and re-baked answers NULL, and this
               unit is then simply not drawn, which the caster census turns into
               a refusal of the whole map. */
            src = j ? tagpu_posebake_mat_mirror((const TAGPU_PBMAT*)r->mat, serial, &nv)
                    : tagpu_posebake_geom_mirror((const TAGPU_PBGEOM*)r->geom, serial, &nv);
            if (!src || nv != r->nvert) break;
            bytes = (VkDeviceSize)nv * st;
            /* THE COPY IS BOUNDED BY THE ALLOCATION IT GOES INTO, and not by
               the pre-pass that sized it. `stageNeed` above counts the
               `vb_find` misses as the table stands BEFORE a byte is uploaded;
               an eviction inside this loop can turn a hit into a miss for a
               unit further down the list, and that unit's bytes were never
               reserved. The pre-pass cannot be made exact without replaying
               the eviction it is trying to budget for, so the bound is taken
               here against the only number that is a fact -- the mapped
               capacity, less the atlas's reserved share of it.

               IT IS TAKEN BEFORE `vb_slot`, NOT AFTER, and that placement is
               the whole of it: `vb_slot` RETIRES the buffer it evicts and
               memsets the entry before handing it back, so a `break` below it
               would leave `g` or `m` pointing at a zeroed entry -- non-NULL,
               so the `if (!g || !m) continue` below lets the unit through, and
               `w->geom` would be VK_NULL_HANDLE on a recorded draw. Breaking
               here consumes nothing: the pointer is still NULL, the unit is
               not drawn, and the every-unit-or-none gate turns that into a
               refused frame. [FOUND 2026-09-16, the landing review; the
               placement, in the round after it.] */
            if (stageOff + bytes + atlasNeed > s->vscap) break;
            *e = vb_slot(d, d->frame);
            if (!*e) {
                if (!s_saidVbFull) {
                    s_saidVbFull = 1;
                    plog(d, "unit: the per-type vertex buffer table is full and "
                            "every entry is in this frame - nothing drawn this "
                            "frame (VB_MAX %d)", VB_MAX);
                }
                break;
            }
            if (!mk_buffer(d, bytes,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           &(*e)->buf, &(*e)->mem, NULL)) {
                memset(*e, 0, sizeof **e);
                goto refuse;
            }
            (*e)->serial = serial;
            (*e)->size = bytes;
            /* STAMPED AT CREATION, not only when the unit is finally drawn. The
               copy below is already recorded into `cb`, so an entry left at
               frame 0 could be chosen by `vb_slot` for the NEXT unit of this
               same frame -- `vb_slot` skips only entries this frame has
               touched. The retire would still keep the buffer alive (nothing is
               destroyed until every slot has turned over), so this is not a
               use-after-free; it is a buffer uploaded and then immediately
               thrown away, which is worth one store to prevent. */
            (*e)->lastFrame = d->frame;
            memcpy(s->vsmap + stageOff, src, (size_t)bytes);
            memset(&cp, 0, sizeof cp);
            cp.srcOffset = stageOff; cp.dstOffset = 0; cp.size = bytes;
            vkCmdCopyBuffer(cb, s->vstage, (*e)->buf, 1, &cp);
            stageOff += bytes;
            anyUpload = 1;
        }
        if (!g || !m) continue;     /* both are stamped above, either way */

        w = &s_draw[s_ndraw];
        w->geom = g->buf; w->mat = m->buf;
        w->first = (uint32_t)r->first; w->count = (uint32_t)r->count;
        w->unit = (uint32_t)i;
        w->ghost = r->ghost ? 1 : 0;
        w->casts = (h.depthOn && r->casts) ? 1 : 0;
        if (w->casts) s_ncast++;
        s_ndraw++;

        fill_blocks(s->umap + (VkDeviceSize)i * ustride, &h, r, vglOff2, fglOff);
        fill_pose(s->pmap + (VkDeviceSize)i * pstride, &h, r);
    }

    /* ONE BARRIER FOR EVERY VERTEX BUFFER WRITTEN THIS FRAME, AND IT IS
       RECORDED BEFORE THE GATE BELOW CAN RETURN. A buffer created this frame
       has no prior access to order against, so the hazard is this transfer
       against the reads the caster draw and the body draw are about to make
       out of the SAME command buffer -- one TRANSFER_WRITE ->
       VERTEX_ATTRIBUTE_READ dependency whatever the count.

       IT MUST NOT SIT BELOW THE EVERY-UNIT-OR-NONE GATE, which is where it
       used to be. That gate returns without drawing, but the copies are
       already in `cb` and `cb` is submitted either way, and the buffers stay
       in the table with their serials -- so a LATER frame finds them, uploads
       nothing, sets no `anyUpload`, and reads vertices that no barrier ever
       ordered against the write that filled them. Recording it here costs one
       barrier on a frame that draws nothing and closes that for good.
       [FOUND 2026-09-16, the landing review.] */
    if (anyUpload) {
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
                             1, &mb, 0, NULL, 0, NULL);
    }

    /* EVERY UNIT OR NONE. A frame drawn with one type missing is a frame the
       A/B reports as a port failure, and the shadow map would be missing a
       caster besides. */
    if (s_ndraw != (unsigned)h.nunit) {
        if (!s_saidShort) {
            s_saidShort = 1;
            plog(d, "unit: %u of %d posed units could be drawn this frame - the "
                    "rest have no bake mirror (a type evicted and re-baked). "
                    "Nothing drawn while that is true",
                 s_ndraw, h.nunit);
        }
        s_ndraw = 0; s_ncast = 0;
        return 0;
    }

    s_saidShort = 0;

    if (!atlas_upload(d, cb, s, &h, stageOff)) goto refuse;

    /* THE REQUEST, AFTER THE ATLAS EXISTS. `restore_want` reads `s_atView`, and
       this is the call that makes it -- so asking earlier would ask over an
       image with no contents and paint entry 0 over the art. `feed` is the
       frame that came here only to make the job: it has nothing to draw yet and
       says so by returning 0, exactly as the refusal above would have. */
    restore_want(d, &h);
    /* AND THE FEED FRAME NOW DRAWS, so there is no `goto feedout` here any more
       (11-5e-2c). It used to leave through that label having drawn nothing,
       because the only thing it could draw was a twin that did not exist yet;
       with `restored` cleared above it draws the indexed atlas like any other
       frame. `feedout` itself is gone with the jump -- what it protected against
       is recorded at `standdown` instead, because that is where the hazard is. */

    /* THE FOUR SMALL IMAGES, per slot, so the one-line invariant covers them:
       UNDEFINED in, because the whole of each is re-sent every frame and there
       are therefore no contents to preserve and no layout to carry. */
    memcpy(s->smallMap + SMALL_PALOFF, h.pal, 256 * 4);
    memcpy(s->smallMap + SMALL_SHDOFF, h.lut, 256 * 32);
    memcpy(s->smallMap + SMALL_FLTOFF, h.fogLut, 256);
    /* The grid is one `unsigned short` a cell and the image is RG8: the same
       two bytes in the same order, which is exactly what the GL twin uploads
       (GL_RG / GL_UNSIGNED_BYTE over this very buffer). With no grid this frame
       the image is one zero cell, which the shader never reads -- taFog is
       called only on the `uFog & 1` branch, and a frame that wanted one and had
       none was refused above. */
    if (h.fogGrid) memcpy(s->smallMap + SMALL_FOGOFF, h.fogGrid,
                          (size_t)fogW * fogH * 2);
    else           memset(s->smallMap + SMALL_FOGOFF, 0, 2);

    img_barrier(cb, s->pal, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->lut, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->fogLut, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    img_barrier(cb, s->fogGrid, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    copy_rect(cb, s->smallStage, SMALL_PALOFF, s->pal, 0, 256, 1);
    copy_rect(cb, s->smallStage, SMALL_SHDOFF, s->lut, 0, 256, 32);
    copy_rect(cb, s->smallStage, SMALL_FLTOFF, s->fogLut, 0, 256, 1);
    copy_rect(cb, s->smallStage, SMALL_FOGOFF, s->fogGrid, 0, fogW, fogH);
    img_barrier(cb, s->pal, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->lut, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->fogLut, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    img_barrier(cb, s->fogGrid, VK_IMAGE_ASPECT_COLOR_BIT, 1,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

    dummies_ready(d, cb);

    s_scissorOn = h.scissorOn;
    s_vpL = h.vpL; s_vpT = h.vpT; s_vw = h.vw; s_vh = h.vh;
    s_gw = h.gw; s_gh = h.gh;       /* the frame those four are measured in */
    s_shadowOn = h.shadowOn;

    /* THE A/B FRAME IS CLAIMED LAST, AFTER EVERY REASON NOT TO DRAW IS PAST. A
       frame claimed and then not drawn would have the seam capture a bare clear
       against a GL half that has the units in it, and report every unit pixel
       as differing: a port failure that is really an oracle failure. */
    s_abFrame = h.ab;
    s_drawThis = 1;
    /* the offsets `cast`, `prepare` and `record` bind with */
    s_uStride = ustride; s_vglOff2 = vglOff2; s_fglOff = fglOff;
    s_pStride = pstride;
    return 1;


standdown:
    /* NOT A REFUSAL OF THE PASS: a frame this pass will not draw, and the slot
       given back because the next one may not draw either.

       SAFE ONLY ABOVE THE STAGING. Every `goto standdown` is above
       `slot_vstage`, so no copy out of `s->vstage` has been recorded when
       control reaches here. That was the whole of the argument and it was
       stated as "safe here and only here", which read as a property of the
       label rather than of its callers -- and then the feed path was pointed at
       it from BELOW the two atlas uploads.

       `feedout` WAS THE LABEL THAT SOLVED THAT, AND IT IS GONE SINCE 11-5e-2c,
       because the feed frame now draws and nothing jumps there any more. The
       hazard it existed for is unchanged and is recorded here instead, since a
       future stand-down is the only way to meet it again:

         by the time control is below `slot_vstage`, `atlas_upload` has memcpy'd
         the indexed mirror into `s->vstage` and recorded a
         `vkCmdCopyBufferToImage` out of it. `cb` is submitted whether this pass
         draws or not, so that copy WILL run -- and `slot_free` here is
         `kill_buffer(&s->vstage)`, the SOURCE buffer of a copy the GPU has not
         executed yet.

       What that looked like, because the shape is worth recognising rather than
       the rule reciting: `atlas_upload` latches `s_atHave`, `s_atSerial` and
       `s_atRows` at RECORD time, so after the lost copy the pass believed the
       device held rows it never received and did not re-upload until the
       mirror's serial moved again. `restore_want` then handed the restorer
       frames `covered_prefix` called covered, and the lane restored them from an
       EMPTY atlas image: the OUT pass reads `frag = c - net` with both taken
       from palette index 0, so every one of those cells came out BLACK, alpha 1.
       [FOUND 2026-09-17.]

       SO: a new stand-down added BELOW `slot_vstage` must `return 0` without
       freeing the slot -- never `goto standdown`. */
    if (s_state == ST_READY) slot_free(d, s);
    return 0;

refuse:
    /* NOTHING IS DESTROYED HERE, AND THAT IS THE WHOLE POINT. This is the
       middle of a frame. The seam waited on fence[slot] ALONE, so every OTHER
       slot's submit is still executing against this pass's pipelines, its
       descriptor pool, its shared atlas and its per-slot buffers -- and `cb`,
       which this function has already recorded uploads into, is submitted
       whether this pass draws or not. Destroying any of it from here is a
       use-after-free on the FIRST refusal, not a rare one. So the pass stops
       drawing at once and OWES a teardown; the seam pays it at the top of a
       later frame, behind the vkDeviceWaitIdle that makes "no submit names
       these objects" a fact rather than a hope. */
    plog(d, "unit: slot %u would not take this frame's resources - the pass "
            "stops drawing and the seam tears it down", (unsigned)slot);
    s_state = ST_REFUSED;
    s_downOwed = 1;
    s_ndraw = 0; s_ncast = 0; s_drawThis = 0;
    return 0;
}

int tagpu_vk_unit_casters(void)
{
    return (s_state == ST_READY && s_drawThis) ? (int)s_ncast : 0;
}

int tagpu_vk_unit_cast(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                       VkRenderPass rp)
{
    VkDeviceSize zero = 0;
    unsigned i, drew = 0;

    if (s_state != ST_READY || !s_drawThis || !s_ncast) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;
    /* The caller has already refused to draw a map at all without it, but this
       pipeline is built here and the bound belongs with the build. */
    if (!d->zclipok) return -1;

    if (!s_pipeCast || s_castRp != rp) {
        if (s_pipeCast) { vkDestroyPipeline(d->dev, s_pipeCast, NULL); s_pipeCast = VK_NULL_HANDLE; }
        if (!build_cast_pipeline(d, rp)) {
            plog(d, "unit: the caster pipeline would not build against the "
                    "shadow pass's render pass - the map is incomplete and the "
                    "pass says so rather than leaving a caster out of it");
            s_castRp = VK_NULL_HANDLE;
            return -1;
        }
        s_castRp = rp;
    }

    /* THE CASTER SET, WRITTEN HERE, and this is the one place in this file that
       writes a descriptor outside `prepare`. It is legal for the same reason
       `prepare`'s is: the seam waited on fence[slot], so nothing of ours is in
       flight for this slot and no submitted command buffer names this set.
       What it may NOT name is an image, and it names none -- see the file
       header on why the map itself is out of reach at this instant. */
    {
        VkDescriptorBufferInfo bi[2];
        VkWriteDescriptorSet wr[2];
        memset(bi, 0, sizeof bi); memset(wr, 0, sizeof wr);
        bi[0].buffer = s_slot[slot].ubuf; bi[0].offset = 0; bi[0].range = VGL_SZ;
        bi[1].buffer = s_slot[slot].pbuf; bi[1].offset = 0; bi[1].range = POSE_SZ;
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = s_slot[slot].dsCast; wr[0].dstBinding = 0;
        wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[0].pBufferInfo = &bi[0];
        wr[1] = wr[0]; wr[1].dstBinding = 1; wr[1].pBufferInfo = &bi[1];
        vkUpdateDescriptorSets(d->dev, 2, wr, 0, NULL);
    }

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeCast);
    for (i = 0; i < s_ndraw; i++) {
        const DRAW* w = &s_draw[i];
        VkBuffer vbs[2];
        VkDeviceSize offs[2];
        uint32_t dyn[2];
        if (!w->casts) continue;
        /* the CASTER's vertex block, which is the second of this unit's two */
        dyn[0] = (uint32_t)((VkDeviceSize)w->unit * s_uStride + s_vglOff2);
        dyn[1] = (uint32_t)((VkDeviceSize)w->unit * s_pStride);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ploCast,
                                0, 1, &s_slot[slot].dsCast, 2, dyn);
        vbs[0] = w->geom; vbs[1] = w->mat;
        offs[0] = zero;   offs[1] = zero;
        vkCmdBindVertexBuffers(cb, 0, 2, vbs, offs);
        vkCmdDraw(cb, w->count, 1, w->first, 0);
        drew++;
    }
    return (int)drew;
}

/* Point this slot's main set at everything it samples, as those things stand
   NOW. On every prepare, and that is the point: it is the one instant the
   seam's fence proves this set is not in use, so another pass's image -- the
   shadow map, the scaffold -- is picked up here rather than by a write that
   would reach a set some other frame is still reading. */
static void bind_main(const TAGPU_VKPASS* d, uint32_t slot)
{
    SLOT* s = &s_slot[slot];
    VkDescriptorBufferInfo bi[3];
    VkDescriptorImageInfo ii[9];
    VkWriteDescriptorSet wr[12];
    VkImageView shadow = tagpu_vk_shadow_view(d->frame, slot);
    VkImageView scaf = tagpu_vk_scaffold_view(d->frame, slot);
    int i, n = 0;

    memset(bi, 0, sizeof bi); memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
    if (!shadow) shadow = s_dumDepthView;
    if (!scaf) scaf = s_dumView;

    bi[0].buffer = s->ubuf; bi[0].offset = 0; bi[0].range = VGL_SZ;
    bi[1].buffer = s->pbuf; bi[1].offset = 0; bi[1].range = POSE_SZ;
    bi[2].buffer = s->ubuf; bi[2].offset = 0; bi[2].range = FGL_SZ;
    for (i = 0; i < 3; i++) {
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[n].dstSet = s->dsMain;
        wr[n].dstBinding = (uint32_t)(i == 2 ? 32 : i);
        wr[n].descriptorCount = 1;
        wr[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[n].pBufferInfo = &bi[i];
        n++;
    }

    /* 40 uAtlas, 41 uLUT, 42 uPal, 43 uAtlasRGB, 44 uScaf, 45 uFogGrid,
       46 uFogLUT, 47 uShadowCmp, 48 uShadowRaw -- the order
       inc/spirv/tagpu_native.spv.h prints for tagpu_native::FS.
       BINDING 43 IS uAtlasRGB AND SINCE GATE 3 IT NAMES THE RESTORED TWIN'S
       OWN IMAGE. The fragment stage reads it on the `uRestored == 1` branch,
       which this pass now draws. It FALLS BACK to the indexed view when there
       is no restored image, and that is not a picture: a descriptor must be
       VALID for the set to be bound, naming the image already here costs no
       memory and no second object, and `prepare`'s restored refusal keeps the
       branch unreachable on exactly the frames the fallback is in place. That
       fallback is all this binding was before gate 3, on every frame. */
    ii[0].sampler = s_samp; ii[0].imageView = s_atView;
    ii[1].sampler = s_samp; ii[1].imageView = s->lutView;
    ii[2].sampler = s_samp; ii[2].imageView = s->palView;
    /* THE RESTORED TWIN TAKES ITS OWN SAMPLER WITH ITS OWN VIEW. The fallback
       is the indexed image AND the indexed sampler together: filtering palette
       indices would blend two table entries, and the pair only ever stands in
       on frames `prepare` has already refused. */
    /* `s_arHave`, NOT JUST `s_arView` -- A LAYOUT QUESTION BEFORE A PICTURE ONE
       (11-5e-2c verification pass). `mk_image` creates the twin UNDEFINED and
       only the restorer's own job transitions it; on a path where the image is
       made and no job ever runs it stays UNDEFINED, and naming it here with
       `imageLayout = SHADER_READ_ONLY_OPTIMAL` is a descriptor-layout mismatch
       on a statically-used binding. That was survivable only while the removed
       stand-down kept those frames from drawing at all. `s_arHave` comes from
       `painted > 0`, so testing it is exactly "this image has left UNDEFINED",
       and it is the same pair that decides `uRestored` -- which makes the flag
       and this descriptor agree BY CONSTRUCTION rather than by a refusal placed
       somewhere else. `tagpu_vk_terr.c:1080` settled this for binding 42 in the
       11-5c re-review and gives the whole argument. */
    ii[3].sampler   = (s_arView && s_arHave) ? s_sampTwin : s_samp;
    ii[3].imageView = (s_arView && s_arHave) ? s_arView   : s_atView;
    /* BINDING 44 NAMES THE REAL OVERLAY WHEN THERE IS ONE, even though this
       pass refuses every frame that samples it (see `upload`): the mechanism
       is what the next landing needs, and a descriptor that names the actual
       image is the one that will still be right when the `gl_FragCoord`
       question is answered. Until then it is a valid descriptor nothing
       reads. */
    ii[4].sampler = s_samp; ii[4].imageView = scaf;
    ii[5].sampler = s_samp; ii[5].imageView = s->fogGridView;
    ii[6].sampler = s_samp; ii[6].imageView = s->fogLutView;
    ii[7].sampler = s_sampCmp; ii[7].imageView = shadow;
    ii[8].sampler = s_samp;    ii[8].imageView = shadow;
    for (i = 0; i < 9; i++) {
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[n].dstSet = s->dsMain;
        wr[n].dstBinding = (uint32_t)(40 + i);
        wr[n].descriptorCount = 1;
        wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[n].pImageInfo = &ii[i];
        n++;
    }
    vkUpdateDescriptorSets(d->dev, (uint32_t)n, wr, 0, NULL);
}

int tagpu_vk_unit_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    (void)cb;
    if (s_state != ST_READY || !s_drawThis) return 0;
    if (slot >= d->slots || slot >= TAGPU_VK_SLOTS) return 0;

    /* THE MAP THE GL TWIN SAMPLED HAS TO BE THE MAP WE SAMPLE. `uShadowOn` came
       from the twin's own tagpu_shadow_apply, so if it is 1 and this frame's
       Vulkan map is not complete, every shadowed fragment would be lit here and
       dark there -- which the A/B would report as a rasteriser difference. The
       terrain pass stands down on exactly this. */
    if (s_shadowOn && !tagpu_vk_shadow_ready(d->frame)) {
        if (!s_saidShadow) {
            s_saidShadow = 1;
            plog(d, "unit: the GL twin drew these units against a cast-shadow map "
                    "and the Vulkan lane has none this frame - nothing drawn "
                    "while that is true");
        }
        s_drawThis = 0;
        s_abFrame = 0;
        return 0;
    }
    s_saidShadow = 0;

    /* The compare sampler is checked in `upload`, which is the only instant a
       refusal for it can still reach the census -- see the block there. */

    bind_main(d, slot);
    return 1;
}

/* the world scissor, and since landing 5b it is the SAME rectangle the GL twin
   clips to rather than its vertical mirror: this pass's framebuffer row 0 is
   the game frame's top row under both APIs now. It was mirrored for as long as
   the viewport height was negative -- tagpu_vk_feat.c is where that is argued
   at length. Getting it wrong shows up as the world clipped against the wrong
   edge rather than as anything subtle. */
static void unit_scissor(uint32_t w, uint32_t h, VkRect2D* sc)
{
    int x = 0, y = 0, cw = (int)w, ch = (int)h;
    /* AND IT IS SCALED, by the attachment's extent over the game frame's, for
       the same reason tagpu_vk_feat.c's is: the rect arrives in GAME-FRAME
       pixels -- it is the rect the native pass hands glScissor -- while this
       pass's viewport covers the whole attachment and its vertex shader
       divides by `uGame`. At the 1:1 sizes an A/B is run at the two are the
       same number, which is why the measurement could not see this; the
       Vulkan window tracks the client rect and the GL lane's own render target
       need not match it. Unscaled, a 640x480 game frame in a 1920x1080 window
       clips every unit to the left third of the bottom quarter of the world.
       [FOUND 2026-09-16, the landing review.] */
    float sx = s_gw > 0.0f ? (float)w / s_gw : 1.0f;
    float sy = s_gh > 0.0f ? (float)h / s_gh : 1.0f;
    if (s_scissorOn && s_vw > 0 && s_vh > 0) {
        int ytop = (int)((float)s_vpT * sy + 0.5f);
        x  = (int)((float)s_vpL * sx + 0.5f);
        cw = (int)((float)s_vw  * sx + 0.5f);
        ch = (int)((float)s_vh  * sy + 0.5f);
        y  = ytop;                      /* NOT mirrored: landing 5b */
    }
    if (x < 0) { cw += x; x = 0; }
    if (y < 0) { ch += y; y = 0; }
    if (x > (int)w) x = (int)w;
    if (y > (int)h) y = (int)h;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    if (x + cw > (int)w) cw = (int)w - x;
    if (y + ch > (int)h) ch = (int)h - y;
    sc->offset.x = x; sc->offset.y = y;
    sc->extent.width = (uint32_t)cw; sc->extent.height = (uint32_t)ch;
}

/* ONE STAGE OF THIS PASS: the bodies, or the build ghosts. They are separate
   calls because the GL twin draws them at DIFFERENT POINTS OF THE FRAME and
   both blend -- `tagpu_native.c` draws the units, then the effects, then
   `ghost_pass`. Recording the ghosts inside the body stage put them BEFORE the
   effects on this lane and after them on the GL one, and premultiplied `over`
   is not commutative: any translucent effect overlapping a ghost (nano spray on
   a queued site, an explosion under the placement cursor) composites to a
   different colour, by up to the ghost's own alpha share of the effect. The
   seam's own comment states that rule as the reason the body stage sits where
   it does; landing 6 put the ghosts in without re-running it, and the landing's
   review caught it. [2026-09-17.] */
static void record_stage(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                         uint32_t w, uint32_t h, int ghosts)
{
    VkViewport vp;
    VkRect2D sc;
    VkPipeline bound;
    unsigned i;

    (void)d;

    /* NO Y FLIP (landing 5b): this pass writes the engine's screen-space y,
       which grows downward, so clip -1 is the game frame's top row and a
       POSITIVE height puts it on row 0 -- where the game's top row is under
       both APIs. minDepth 0.5 / maxDepth 1.0 maps clip z in [0, 1] onto GL's
       own (z+1)/2 -- the vertex shader writes `clamp(1.0 - enc/uDepthScale, 0,
       1)`, which is already in [0, 1], so this needs no extension and must not
       use one. Both stages set it: the effects pass runs between them and sets
       its own. */
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.minDepth = 0.5f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    unit_scissor(w, h, &sc);
    vkCmdSetScissor(cb, 0, 1, &sc);

    /* THE PIPELINE IS BOUND PER DRAW KIND, NOT ONCE. The records arrive in the
       GL twin's own draw order -- every unit, then every ghost, because
       ghost_pass runs after the unit loop and `pd_record` appends as each draw
       is issued -- so in practice this switches once. It is written as a
       compare rather than as two loops so that it stays correct if that order
       ever stops holding. */
    bound = VK_NULL_HANDLE;
    for (i = 0; i < s_ndraw; i++) {
        const DRAW* q = &s_draw[i];
        VkPipeline want;
        if (!q->ghost != !ghosts) continue;        /* this stage's draws only */
        want = q->ghost ? s_pipeGhost : s_pipeBody;
        VkBuffer vbs[2];
        VkDeviceSize offs[2];
        uint32_t dyn[3];
        if (want != bound) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, want); bound = want; }
        dyn[0] = (uint32_t)((VkDeviceSize)q->unit * s_uStride);
        dyn[1] = (uint32_t)((VkDeviceSize)q->unit * s_pStride);
        dyn[2] = (uint32_t)((VkDeviceSize)q->unit * s_uStride + s_fglOff);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_ploMain,
                                0, 1, &s_slot[slot].dsMain, 3, dyn);
        vbs[0] = q->geom; vbs[1] = q->mat;
        offs[0] = 0;      offs[1] = 0;
        vkCmdBindVertexBuffers(cb, 0, 2, vbs, offs);
        vkCmdDraw(cb, q->count, 1, q->first, 0);
    }
}

/* The bodies. `s_drawThis` is NOT cleared here: the ghost stage below closes the
   frame, and the seam calls both or neither. */
void tagpu_vk_unit_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                          uint32_t w, uint32_t h)
{
    if (s_state != ST_READY || !s_drawThis) return;
    record_stage(d, cb, slot, w, h, 0);
}

/* The build ghosts, AFTER the effects -- the GL twin's own order, and the whole
   reason this is a second entry point. It also ends the pass's frame. */
void tagpu_vk_unit_record_ghosts(const TAGPU_VKPASS* d, VkCommandBuffer cb,
                                 uint32_t slot, uint32_t w, uint32_t h)
{
    if (s_state != ST_READY || !s_drawThis) return;
    record_stage(d, cb, slot, w, h, 1);
    s_drawThis = 0;
}

int tagpu_vk_unit_ab_frame(void)
{
    int a = s_abFrame;
    s_abFrame = 0;
    return a;
}

void tagpu_vk_unit_down(const TAGPU_VKPASS* d)
{
    uint32_t i;
    int k;
    /* WHETHER THIS TEARDOWN IS THE ONE THE PASS ASKED FOR. Only the seam's
       `_down_paid` sets it, and only after its vkDeviceWaitIdle -- so a
       `vk_down` or a `vk_resize` that happens to run while a debt is
       outstanding tears the pass down and leaves it coming back ST_UNBUILT
       rather than latched at ST_REFUSED by the back door. The debt is
       discharged by every teardown; the VERDICT is not. */
    int owed = s_downPaying;
    s_downOwed = 0;
    s_drawThis = 0; s_abFrame = 0; s_ndraw = 0; s_ncast = 0;
    s_shadowOn = 0;

    /* NOTHING TO FREE, BUT THE VERDICT STILL STANDS. */
    if (!d->dev || !vkDestroyPipeline) {
        s_state = owed ? ST_REFUSED : ST_UNBUILT;
        return;
    }
    for (i = 0; i < TAGPU_VK_SLOTS; i++) {
        slot_free(d, &s_slot[i]);
        s_slot[i].dsMain = VK_NULL_HANDLE;   /* both go back with the pool */
        s_slot[i].dsCast = VK_NULL_HANDLE;
    }
    for (k = 0; k < VB_MAX; k++)
        if (s_vb[k].buf) { kill_buffer(d, &s_vb[k].buf, &s_vb[k].mem, NULL);
                           memset(&s_vb[k], 0, sizeof s_vb[k]); }
    for (k = 0; k < RET_MAX; k++)
        if (s_ret[k].buf) { kill_buffer(d, &s_ret[k].buf, &s_ret[k].mem, NULL);
                            s_ret[k].pending = 0; }
    /* THE RESTORE JOB GOES BACK BEFORE THE IMAGES IT NAMES, and before the
       per-level views: it holds a framebuffer over each of them, and a view
       still named by a live framebuffer may not be destroyed. Every caller of
       this function is past the seam's vkDeviceWaitIdle, so none of it is in a
       queue. `s_rjTried` is a fact about a device that refused, so it does not
       survive the device either. [Landing 7e-2; tagpu_vk_feat.c carries the
       same block for the same reason.] */
    if (s_rjob) { tagpu_vk_restore_job_free(d, s_rjob); s_rjob = NULL; }
    s_rjGen = 0; s_rjBlanks = 0; s_rjTaken = 0; s_rjPainted = 0;
    s_rjTried = 0; s_rjChain = 0;
    s_rjSrcView = VK_NULL_HANDLE; s_rjDstView = VK_NULL_HANDLE;
    for (k = 0; k <= TAGPU_VK_MAXMIP; k++)
        if (s_arLvl[k]) { vkDestroyImageView(d->dev, s_arLvl[k], NULL);
                          s_arLvl[k] = VK_NULL_HANDLE; }
    s_arLvlN = 0;
    kill_image(d, &s_atImg, &s_atMem, &s_atView);
    kill_image(d, &s_arImg, &s_arMem, &s_arView);
    kill_image(d, &s_dumImg, &s_dumMem, &s_dumView);
    kill_image(d, &s_dumDepth, &s_dumDepthMem, &s_dumDepthView);
    s_atDim = 0; s_atSerial = 0; s_atHave = 0; s_atRows = 0; s_dumReady = 0;
    s_arDim = 0; s_arMips = 0; s_arHave = 0;
    if (s_dpool) { vkDestroyDescriptorPool(d->dev, s_dpool, NULL); s_dpool = VK_NULL_HANDLE; }
    if (s_pipeBody) { vkDestroyPipeline(d->dev, s_pipeBody, NULL); s_pipeBody = VK_NULL_HANDLE; }
    if (s_pipeGhost) { vkDestroyPipeline(d->dev, s_pipeGhost, NULL); s_pipeGhost = VK_NULL_HANDLE; }
    if (s_pipeCast) { vkDestroyPipeline(d->dev, s_pipeCast, NULL); s_pipeCast = VK_NULL_HANDLE; }
    s_castRp = VK_NULL_HANDLE;
    if (s_ploMain) { vkDestroyPipelineLayout(d->dev, s_ploMain, NULL); s_ploMain = VK_NULL_HANDLE; }
    if (s_ploCast) { vkDestroyPipelineLayout(d->dev, s_ploCast, NULL); s_ploCast = VK_NULL_HANDLE; }
    if (s_dslMain) { vkDestroyDescriptorSetLayout(d->dev, s_dslMain, NULL); s_dslMain = VK_NULL_HANDLE; }
    if (s_dslCast) { vkDestroyDescriptorSetLayout(d->dev, s_dslCast, NULL); s_dslCast = VK_NULL_HANDLE; }
    if (s_samp) { vkDestroySampler(d->dev, s_samp, NULL); s_samp = VK_NULL_HANDLE; }
    if (s_sampCmp) { vkDestroySampler(d->dev, s_sampCmp, NULL); s_sampCmp = VK_NULL_HANDLE; }
    if (s_sampTwin) { vkDestroySampler(d->dev, s_sampTwin, NULL); s_sampTwin = VK_NULL_HANDLE; }
    s_cmpLinear = 0;
    /* ST_UNBUILT and not ST_REFUSED: a pass brought down by a mode change or a
       cleared lever must be able to come back. The one exception is the
       teardown this pass asked for. */
    s_state = owed ? ST_REFUSED : ST_UNBUILT;
}

int tagpu_vk_unit_down_owed(void) { return s_downOwed; }

/* THE SEAM'S OWN ENTRY POINT, called only after its vkDeviceWaitIdle. It is
   what makes `_down`'s ST_REFUSED latch apply to the owed teardown and to
   nothing else. */
void tagpu_vk_unit_down_paid(const TAGPU_VKPASS* d)
{
    s_downPaying = 1;
    tagpu_vk_unit_down(d);          /* clears s_downOwed itself */
    s_downPaying = 0;
}
