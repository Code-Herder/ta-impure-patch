/* tagpu_vk_restore.c -- the Classic++ restorer's VULKAN BACKEND: the device
   resources and the four compute shaders. tagpu_vk_restore.h is the contract
   and says why this is not one of the frame's passes; tagpu_restore_core.h is
   the scheduler this implements and says why the split is where it is.

   NOTHING HERE DECIDES WHEN TO DISPATCH. The core hands over one
   TAGPU_RDRAWREQ at a time, already sequenced and already costed, and this
   file binds and dispatches it. That is deliberate: the sequence, the costing
   and the budget are the core's, and a backend that re-derived the fill ->
   depth x conv -> out sequence would be a second copy of it, free to disagree
   about which layer it was on while looking internally consistent.

   THE SHADERS are tagpu_restore_comp.h, compiled by tools/spirv-gen.py: FILL,
   one CONV variant per layer shape of the loaded model, OUT, and MIP. Every
   one of them stays inside Vulkan 1.0's guaranteed minimums (128 invocations,
   16 KB of shared memory) and needs no extension; what the device must still
   answer is asked in `up` and refused by name.

   ONE DESCRIPTOR SET LAYOUT FOR FILL, CONV AND OUT, with the bindings the
   shaders declare -- 0 the activations read, 1 the activations written, 2 the
   weights, 3 the slot table, 4 the source atlas, 5 the palette, 6 the
   destination -- so a job's two sets (one per ping-pong side) serve every
   dispatch of a batch. MIP has its own two (7 and 8). Every scalar goes in
   push constants, recorded into the stream with the dispatch it belongs to.

   THE CHECK ON THIS FILE IS BYTES, NOT PIXELS. Under `tagpu_restoredump.on`
   each job writes its finished destination (`dump_step`) with its source and
   frame list beside it, and tools/restore-dumpcheck.py restores the same frames
   with the torch model and holds the two to D11's bar. It needs no window and
   no settle heuristic.

   THE LAUNCH SELF-TEST IS HERE TOO (research/notes/compute-restorer.md D2,
   D13): the first bring-up on a device closes the core's gate, runs a
   synthetic probe through these same pipelines, reads it back and holds it to
   the CPU reference (tagpu_restore_ref.c) before any consumer's job may run.
   The entries that build and dispatch -- `up`, `job_new`, `job_chain`,
   `step` -- run inside tagpu_restore_guard.h's enter/leave, which is what lets
   a crash in one be blamed on the restorer and nothing else; the teardown
   (`job_free`, `down`) does not.

   EVERYTHING ELSE -- the padding rule, the batch grid, the size-class ladder,
   the budget, every counter and every log line -- is the core's
   (tagpu_restore_core.c). */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_vk_restore.h"
#include "tagpu_restore_core.h"
#include "tagpu_restore_guard.h"
#include "tagpu_restore_ref.h"
#include "tagpu_vk.h"           /* tagpu_vk_mem_free */
#include "tagpu_classicpp.h"
#include "tagpu_gaf.h"      /* tagpu_gaf_mip_off/_chain: the chain LAYOUT, so
                              the offsets the dump copies to come from the one
                              function rather than from a second copy of the
                              arithmetic. */
#include "spirv/tagpu_restore_comp.spv.h"

#define LANE "restorevk"

/* the instance- and device-level entry points this pass needs
   (tagpu_vk_pass.h says why every pass carries its own table) */
#define IFNS(X) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)

#define DFNS(X) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkDeviceWaitIdle) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdDispatch) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdUpdateBuffer) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdClearColorImage)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };
static int s_state;
/* A REFUSAL HOLDS FOR ONE EPOCH (tagpu_rguard_epoch): the render options' On
   after the restorer turned itself off moves it, and `up` then asks the
   device again instead of answering from the latch. */
static unsigned s_refEpoch;

/* `ep` is the epoch the refused attempt BEGAN in, read before anything it
   refused on. Only the guard's tick moves the epoch, on this thread and never
   during an attempt, so the stamp is the epoch the attempt saw whole. */
static int refuse_at(unsigned ep)
{
    s_state = ST_REFUSED;
    s_refEpoch = ep;
    return 0;
}

static int refuse(void) { return refuse_at(tagpu_rguard_epoch()); }

/* what the device answered, kept for the log and for the refusals */
static struct {
    uint32_t maxSsbo;                    /* maxStorageBufferRange             */
    int      timestamps;                 /* period > 0 AND valid bits > 0     */
    double   tsPeriod;                   /* ns per tick                       */
} s_dev;

static void rlog(const char* s) { tagpu_rcore_log(s); }

static int resolve(const TAGPU_VKPASS* d)
{
#define RES_I(n) n = (PFN_##n)d->gipa(d->inst, #n); if (!n) return 0;
#define RES_D(n) n = (PFN_##n)d->gdpa(d->dev, #n);  if (!n) return 0;
    IFNS(RES_I)
    DFNS(RES_D)
#undef RES_I
#undef RES_D
    return 1;
}

/* ---- the conv variants, one per layer shape ----
   `spirv-gen.py` reads the shapes out of the shipped weight files and emits
   one module per (CIN, COUT, LAST), with TH, the rows one workgroup covers,
   in the name. This table is how a loaded model's layer finds its module; a
   layer whose shape is not here is refused by name at build, which is the
   day a retrained model is shipped without regenerating the headers. */
typedef struct { int cin, cout, last, th; const uint32_t* spv; size_t words; } CONVV;
#define CV(CI, CO, LA, TH, SYM) \
    { CI, CO, LA, TH, tagpu_spv_tagpu_restore_comp_##SYM, \
      sizeof tagpu_spv_tagpu_restore_comp_##SYM / 4 },
static const CONVV s_convv[] = {
    CV(4,  64, 0, 8,  CONV_CS_I4_O64_T8)
    CV(64, 64, 0, 8,  CONV_CS_I64_O64_T8)
    CV(64, 8,  1, 32, CONV_CS_I64_O8_T32_LAST)
    CV(4,  24, 0, 16, CONV_CS_I4_O24_T16)
    CV(24, 24, 0, 16, CONV_CS_I24_O24_T16)
    CV(24, 8,  1, 32, CONV_CS_I24_O8_T32_LAST)
};
#undef CV
#define NCONVV (int)(sizeof s_convv / sizeof s_convv[0])

/* a layer's shape as the kernel sees it: CIN = 4 x jin, COUT = 4 x kout, and
   the last layer's one output tile padded to eight channels */
static const CONVV* conv_for(const TAGPU_RLAYER* L, int last)
{
    int cin = 4 * (int)L->jin, cout = last ? 8 : 4 * (int)L->kout, i;
    for (i = 0; i < NCONVV; i++)
        if (s_convv[i].cin == cin && s_convv[i].cout == cout && s_convv[i].last == last)
            return &s_convv[i];
    return NULL;
}

/* ASK THE DEVICE FOR EVERY PREREQUISITE AND REFUSE BY NAME.

   Most of what the shaders need is a Vulkan 1.0 minimum and cannot be
   refused -- 128 invocations, 16 KB of shared memory, 128 bytes of push
   constants, 65 535 workgroups per dimension. What a device CAN answer
   differently is asked here:
     * the submitting queue family must have COMPUTE -- guaranteed of SOME
       family on a device with graphics, not of the one the seam picked;
     * R8G8B8A8_UNORM must be a storage image, which OUT and MIP write --
       mandated by the specification, asked anyway, because a mandated
       feature that is absent is a broken driver and a named refusal beats an
       unexplained black atlas;
     * maxStorageBufferRange must hold the largest activation buffer.
   Only the timestamp refusal is SOFT: without a GPU timer the core falls back
   to a fixed dispatch count per slice, which is slower and safe. Every other
   one stands the pass down, and Classic++ then draws the original dithered
   art, which is the shipped fallback rather than a fault. */
static uint64_t act_bytes_max(void);
static int      act_ch(void);
static void probe_arm(void);

static int up_impl(const TAGPU_VKPASS* d)
{
    VkPhysicalDeviceProperties dp;
    VkQueueFamilyProperties qp[16];
    VkFormatProperties fp;
    uint32_t nq = 16;
    const TAGPU_RMODEL* w;
    const TAGPU_ROPT*   opt;
    char b[420];
    uint64_t need;
    unsigned ep;

    if (s_state == ST_READY)   return 1;
    ep = tagpu_rguard_epoch();
    if (s_state == ST_REFUSED) {
        if (s_refEpoch == ep) return 0;
        s_state = ST_UNBUILT;
    }

    if (!d || !d->dev || !d->pd) return 0;
    if (!resolve(d)) { refuse_at(ep); rlog(LANE ": a Vulkan entry point this pass needs is missing"); return 0; }

    /* the model and the options, reloaded per DEVICE (tagpu_restore_core.c) */
    if (!tagpu_rcore_reload(LANE)) return refuse_at(ep);
    w = tagpu_rcore_model(TAGPU_RM_FULL);
    opt = tagpu_rcore_opt();

    memset(&s_dev, 0, sizeof s_dev);
    memset(&dp, 0, sizeof dp);
    vkGetPhysicalDeviceProperties(d->pd, &dp);
    s_dev.maxSsbo = dp.limits.maxStorageBufferRange;

    /* THE RECORD FIRST (tagpu_restore_guard.h): a device and driver the
       restorer crashed on, lost, or failed its self-test on stays on the
       original art until the driver changes or the player asks again */
    if (tagpu_rguard_device(dp.vendorID, dp.deviceID, dp.driverVersion)) return refuse_at(ep);

    memset(qp, 0, sizeof qp);
    vkGetPhysicalDeviceQueueFamilyProperties(d->pd, &nq, qp);
    if (nq > 16) nq = 16;
    if (d->qfam >= nq || !(qp[d->qfam].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        _snprintf(b, sizeof b, LANE ": queue family %u has no COMPUTE - the lane cannot restore",
                  (unsigned)d->qfam);
        rlog(b); return refuse_at(ep);
    }

    memset(&fp, 0, sizeof fp);
    vkGetPhysicalDeviceFormatProperties(d->pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        rlog(LANE ": R8G8B8A8_UNORM is not a storage image here, which the spec requires"
                  " - the lane cannot restore");
        return refuse_at(ep);
    }

    /* THE LIMIT STAYS UNSIGNED, as Vulkan reports it: a driver may report
       UINT32_MAX for a range it does not bound (AMD's Windows driver does for
       the uniform range, MEASURED 2026-09-25 on an R9 200 series card), and
       read as an int that is -1. */
    need = act_bytes_max();
    if ((uint64_t)s_dev.maxSsbo < need) {
        _snprintf(b, sizeof b, LANE ": maxStorageBufferRange %u < the %u-byte activation buffer"
                               " the %d-channel model needs - the lane cannot restore",
                  s_dev.maxSsbo, (unsigned)need, act_ch());
        rlog(b); return refuse_at(ep);
    }

    /* ---- the slice timer, and this is the ONLY soft refusal ----
       Two conditions and neither is "the device has a clock": `timestampPeriod`
       is 0 on a device that cannot do it at all, and `timestampValidBits` is
       per QUEUE FAMILY -- the one the seam submits to is the one asked. */
    s_dev.timestamps = dp.limits.timestampPeriod > 0.0f && qp[d->qfam].timestampValidBits > 0;
    s_dev.tsPeriod = (double)dp.limits.timestampPeriod;
    if (!s_dev.timestamps)
        rlog(LANE ": this device/queue reports no usable timestamps - fixed slices, not a budget");

    _snprintf(b, sizeof b, LANE ": the device can restore: %dx%d fp32 compute, activations up to"
                           " %u MB of a %u MB storage range, %s, budget %.1f ms/frame",
              w->depth, w->ch, (unsigned)(need >> 20), s_dev.maxSsbo >> 20,
              s_dev.timestamps ? "timestamp budget" : "no timestamps: fixed slices",
              opt->budget);
    rlog(b);

    /* The resources themselves are built lazily, on the first job: a lane that
       never turns Classic++ on should pay nothing for this pass beyond the
       queries above. */
    s_state = ST_READY;
    probe_arm();
    return 1;
}

int tagpu_vk_restore_up(const TAGPU_VKPASS* d)
{
    int r;
    tagpu_rguard_enter();
    r = up_impl(d);
    tagpu_rguard_leave();
    return r;
}

unsigned tagpu_vk_restore_epoch(void) { return tagpu_rguard_epoch(); }

/* ============================ RESOURCES ============================
   ALL OF THEM ARE SHARED, NOT PER SLOT, AND THE ARGUMENT IS AN ORDERING.

   A batch's dispatches can span slices: the budget cuts a batch off mid-way
   and the next slice resumes it, so the activations and the slot table must
   hold that batch's contents across submissions. Per-slot copies would be the
   reflex, and for the activations they are not affordable -- two 64-channel
   fp32 grids of 544 x 528 are 147 MB, and eight slots of them is not a
   trade-off, it is a different program.

   So they are shared and ORDERED instead. `vkCmdPipelineBarrier`'s first
   synchronisation scope includes every command submitted EARLIER IN SUBMISSION
   ORDER on the same queue, not merely earlier in the same command buffer, so
   one barrier at the head of each slice makes the previous slice's reads a fact
   before this slice's writes -- which is a fence rather than a hope, and is
   what CLAUDE.md's *Fixes must be safe by construction* calls an ordering. It
   costs one barrier per slice on background work that already has a GPU-time
   budget. */

/* the push constants every shader declares (tagpu_restore_comp.h COMMON) */
typedef struct {
    int32_t gw, gh, pitch, cols;
    int32_t y0;
    int32_t wbase, bbase;
    int32_t base;
    int32_t keyR;
    int32_t dstW, dstH;
    int32_t spare;
} PUSH;

static VkSampler             s_samp;        /* NEAREST, clamp: every sampler here */
static VkDescriptorSetLayout s_dsl, s_dslMip;
static VkPipelineLayout      s_plo, s_ploMip;
static VkPipeline            s_pipeFill, s_pipeOut, s_pipeMip;
static VkPipeline            s_pipeConv[TAGPU_RM_N][TAGPU_R_MAXLAYERS];  /* per LAYER */
static int                   s_convTh[TAGPU_RM_N][TAGPU_R_MAXLAYERS];    /* its TH    */
static VkDescriptorPool      s_dpool;
static int                   s_built;

/* THE WEIGHTS IN THE KERNEL'S LAYOUT, repacked once from the weight files:
   per model and layer, w[wbase + ((tap x CIN + ci) x COUT) / 4 + co4] and the
   biases at bbase, all in vec4 units of ONE storage buffer, the models end to
   end -- so a job's model is a table lookup at dispatch, not another binding. */
static int                   s_wbase[TAGPU_RM_N][TAGPU_R_MAXLAYERS];
static int                   s_bbase[TAGPU_RM_N][TAGPU_R_MAXLAYERS];

/* THE ACTIVATIONS: two storage buffers, the ping-pong, each `s_actCap` texels
   of `ch` fp32 channels. A layer of CIN channels lays its grid out as
   [gh][gw][CIN], so one buffer serves every layer of the batch. */
static VkBuffer       s_act[2];
static VkDeviceMemory s_actMem[2];
static uint64_t       s_actCap;                     /* texels                  */
static unsigned       s_actGen = 1;                 /* bumped on realloc       */

/* ---- THE RETIRE --------------------------------------------------------
   `tagpu_vk_terr.c`'s rule, applied to a bigger object: `pending` starts as
   every slot, a bit clears at the top of that slot's own step UNCONDITIONALLY
   (including on the paths that dispatch nothing -- clearing it only when the
   slot dispatched would stall the retire for ever on a lane that pauses), and
   `pending == 0` is what licenses the destroy. */
typedef struct {
    VkBuffer       buf[2];
    VkDeviceMemory mem[2];
    uint32_t       pending;                         /* slots yet to turn over  */
} RETIRE;
static RETIRE s_ret;

/* ---- AND A JOB'S OWN RESOURCES NEED THE SAME THING. Every consumer's call
   of `job_free` but the one in its `down` is reached from its per-frame call
   (`prepare`, the units' `upload`) -- `restore_want`, `refuse_job`, and the
   drops of a twin or a source that moved (tagpu_vk_feat.c `twin_drop`,
   tagpu_vk_unit.c `atlas_rgb_build`, tagpu_vk_terr.c's base atlas) -- and so
   is `job_new`'s own failure path. There the seam has waited on THIS SLOT'S
   fence and no other. The other `slots - 1` submits are still in the queue
   naming the job's objects: its descriptor sets name the destination and the
   palette. Destroying them outright on a map change or a repaint within
   `slots - 1` frames of a dispatch (VUID-vkFreeDescriptorSets-pDescriptorSets-00309)
   is a crash on a strict driver and corrupt paint on a lax one.

   So they are retired too, with the activations' licence: `pending == 0` and
   not a frame count. There is ONE PATH rather than a fast path for idle
   callers -- `down` flushes these unconditionally because the seam has drained
   the device above it, so nothing here depends on which caller knew what.

   THE RING HOLDS ONE FREE PER JOB PER FRAME, AND A FULL RING DRAINS. A
   consumer holds one job and makes at most one a frame, so it frees one in a
   frame -- when the serial it keys on moves, or what the job reads or paints
   goes -- and two only when the job made to replace it is refused on the way
   (`job_new`'s own failure path, the unit twin's chain). There are at most
   `TAGPU_R_MAXJOBS` jobs and an entry is given back after `slots` frames, so
   `TAGPU_R_MAXJOBS x TAGPU_VK_SLOTS` entries hold every job churning on every
   frame. Only those refusals, repeated frame after frame, can fill it, and
   the full case is handled rather than asserted: the device is DRAINED and
   every entry freed immediately. A stall on a frame is honest; a destroy
   nobody has licensed is the bug this whole structure exists to prevent. */
typedef struct {
    VkImage         palImg;
    VkDeviceMemory  palMem;
    VkImageView     palView;
    VkBuffer        palStage;
    VkDeviceMemory  palStageMem;
    /* TWO SETS, PLUS ONE PER LEVEL OF A REGISTERED MIP CHAIN: a reduction into
       a level was recorded into some slot's command buffer, and a consumer
       frees its job when its generation moves, which is any frame. */
    VkDescriptorSet set[2 + TAGPU_VK_MAXMIP];
    int             nset;
    /* THE DUMP'S STAGING, when a job is freed with a copy into it still in
       flight: the copy was recorded into the command buffer of the slot that
       took the dump, and "called between frames" is not past that slot's
       fence. */
    VkBuffer        buf;      VkDeviceMemory bufMem;
    uint32_t        pending;
} JRETIRE;
#define JRET_MAX (TAGPU_R_MAXJOBS * TAGPU_VK_SLOTS)
static JRETIRE s_jret[JRET_MAX];

/* the slot table: TAGPU_R_BATCH records, device-local, written in the stream */
static VkBuffer       s_tab;
static VkDeviceMemory s_tabMem;

static VkBuffer       s_wbuf;                       /* the repacked weights    */
static VkDeviceMemory s_wmem;
static VkBuffer       s_wstage;                     /* ...and its one-time upload */
static VkDeviceMemory s_wstageMem;
static unsigned char* s_wstageMap;
static VkDeviceSize   s_wbytes;
static int            s_wUp;                        /* the copy has been recorded */
static const TAGPU_RBACKEND s_be;                   /* defined with the dispatches */

static VkQueryPool    s_qpool;                      /* 2 pairs, one per parity */

/* the slice's context, stashed by `step` for the vtable to reach */
static const TAGPU_VKPASS* s_d;
static VkCommandBuffer     s_cb;
static uint32_t            s_slot;
static int                 s_sliceOpen;             /* the head barrier is done */
/* anything recorded into this frame's command buffer: the slot then carries
   restorer work until its fence signals (tagpu_rguard_work) */
static int                 s_rec;

static TAGPU_RSCHED s_sched;                        /* `be` set in build()      */

/* THE LARGEST ACTIVATION BUFFER, from the core's ladder: a batch of `cols`
   slots of class C has cols = min(8, ACT_MAX / C) and a pitch of at most
   C + 1, so its grid is at most cols x (C + 1) before the kernel's padding. The
   maximum over the ladder is the 512 class's single slot and the smaller
   classes' eight, 544 x 528 either way; computed rather than written down, so
   a change to the ladder or the tiling moves it. */
/* the channels an activation texel holds: the widest model loaded, since
   the ping-pong is shared by every job whatever it runs */
static int act_ch(void)
{
    int m, ch = 0;
    for (m = 0; m < TAGPU_RM_N; m++) {
        const TAGPU_RMODEL* w = tagpu_rcore_model(m);
        if (w && w->ch > ch) ch = w->ch;
    }
    return ch;
}

static uint64_t act_bytes_max(void)
{
    static const int cls[] = { 32, 48, 64, 96, 128, 192, 256, 384, 512 };
    uint64_t best = 0;
    int i;
    for (i = 0; i < (int)(sizeof cls / sizeof cls[0]); i++) {
        int cols = TAGPU_R_ACTMAX / cls[i];
        uint64_t g, gw, gh;
        if (cols > TAGPU_R_SLOTCOLS) cols = TAGPU_R_SLOTCOLS;
        g = (uint64_t)cols * (uint64_t)(cls[i] + 1);
        gw = (g + TAGPU_R_GRIDX - 1) / TAGPU_R_GRIDX * TAGPU_R_GRIDX;
        gh = (g + TAGPU_R_GRIDY - 1) / TAGPU_R_GRIDY * TAGPU_R_GRIDY;
        if (gw * gh > best) best = gw * gh;
    }
    return best * (uint64_t)act_ch() * 4u;
}

static uint32_t mem_type(const TAGPU_VKPASS* d, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    memset(&mp, 0, sizeof mp);
    vkGetPhysicalDeviceMemoryProperties(d->pd, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int mk_buffer(const TAGPU_VKPASS* d, VkDeviceSize bytes, VkBufferUsageFlags use,
                     VkMemoryPropertyFlags want, VkBuffer* buf, VkDeviceMemory* mem,
                     unsigned char** map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    uint32_t type;
    bci.size = bytes; bci.usage = use; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d->dev, &bci, NULL, buf) != VK_SUCCESS) return 0;
    memset(&req, 0, sizeof req);
    vkGetBufferMemoryRequirements(d->dev, *buf, &req);
    type = mem_type(d, req.memoryTypeBits, want);
    if (type == UINT32_MAX) { vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0; }
    mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) {
        vkDestroyBuffer(d->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return 0;
    }
    if (vkBindBufferMemory(d->dev, *buf, *mem, 0) != VK_SUCCESS) return 0;
    if (map && vkMapMemory(d->dev, *mem, 0, bytes, 0, (void**)map) != VK_SUCCESS) return 0;
    return 1;
}

/* a 2D image with a view, device-local: the palette snapshot */
static int mk_image(const TAGPU_VKPASS* d, int w, int h, VkFormat fmt,
                    VkImageUsageFlags use, VkImage* img, VkDeviceMemory* mem, VkImageView* view)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements req;
    uint32_t type;

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = (uint32_t)w; ici.extent.height = (uint32_t)h; ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = use;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, img) != VK_SUCCESS) return 0;
    memset(&req, 0, sizeof req);
    vkGetImageMemoryRequirements(d->dev, *img, &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { vkDestroyImage(d->dev, *img, NULL); *img = VK_NULL_HANDLE; return 0; }
    mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    if (vkAllocateMemory(d->dev, &mai, NULL, mem) != VK_SUCCESS) {
        vkDestroyImage(d->dev, *img, NULL); *img = VK_NULL_HANDLE; return 0;
    }
    if (vkBindImageMemory(d->dev, *img, *mem, 0) != VK_SUCCESS) return 0;
    ivi.image = *img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    return vkCreateImageView(d->dev, &ivi, NULL, view) == VK_SUCCESS;
}

/* ---- THE ORDERING BETWEEN DISPATCHES -----------------------------------
   Every dispatch of a batch reads what the one before it wrote: FILL writes
   the first activations, each layer reads its predecessor's and writes the
   other buffer, OUT reads the last and writes the consumer's atlas. Nothing
   orders two dispatches unless a barrier says so, and a driver that happens
   to serialise them hides the hazard -- the bug with better odds, not a fix.

   ONE BARRIER AFTER EVERY DISPATCH THAT FINISHES A STAGE -- FILL, a layer's
   last band, OUT -- from the compute shader's writes to every later reader:
   the next compute dispatch (RAW for the next layer, WAW and WAR for the next
   batch's FILL over the buffer this OUT read), a transfer (the next batch's
   table update, the byte dump), and the consumer's own fragment-shader sample
   of the atlas. The bands of one layer need none between them: they write
   disjoint rows of one buffer and read another that none of them writes. */
static void stage_done(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* A COPY THE HOST WILL READ: the fence the seam waits on makes device writes
   available to the device only, so without this the host may read the buffer
   as it was -- the specification's rule, whatever a given driver does. */
static void host_ready(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

static VkShaderModule mk_mod(const TAGPU_VKPASS* d, const uint32_t* w, size_t words)
{
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    smi.codeSize = words * 4;
    smi.pCode = w;
    if (vkCreateShaderModule(d->dev, &smi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

static VkPipeline mk_pipe(const TAGPU_VKPASS* d, const uint32_t* spv, size_t words,
                          VkPipelineLayout plo)
{
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkPipeline out = VK_NULL_HANDLE;
    VkShaderModule m = mk_mod(d, spv, words);
    if (!m) return VK_NULL_HANDLE;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = plo;
    if (vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, NULL, &out) != VK_SUCCESS)
        out = VK_NULL_HANDLE;
    vkDestroyShaderModule(d->dev, m, NULL);
    return out;
}

static VkDescriptorSetLayout mk_dsl(const TAGPU_VKPASS* d, const int* bind,
                                    const VkDescriptorType* type, int n)
{
    VkDescriptorSetLayoutBinding b[8];
    VkDescriptorSetLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout out = VK_NULL_HANDLE;
    int i;
    memset(b, 0, sizeof b);
    for (i = 0; i < n && i < 8; i++) {
        b[i].binding = (uint32_t)bind[i];
        b[i].descriptorType = type[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    ci.bindingCount = (uint32_t)n;
    ci.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &ci, NULL, &out) != VK_SUCCESS) return VK_NULL_HANDLE;
    return out;
}

static VkPipelineLayout mk_plo(const TAGPU_VKPASS* d, VkDescriptorSetLayout dsl)
{
    VkPipelineLayoutCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPushConstantRange pr;
    VkPipelineLayout out = VK_NULL_HANDLE;
    memset(&pr, 0, sizeof pr);
    pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pr.size = sizeof(PUSH);
    ci.setLayoutCount = 1; ci.pSetLayouts = &dsl;
    ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(d->dev, &ci, NULL, &out) != VK_SUCCESS) return VK_NULL_HANDLE;
    return out;
}

/* ---- the retire's two halves ---- */

/* Destroy one job retire's contents. The caller has established the licence:
   either its `pending` reached 0, or the device has been drained. */
static void jret_kill(const TAGPU_VKPASS* d, JRETIRE* r)
{
    if (r->palView) vkDestroyImageView(d->dev, r->palView, NULL);
    if (r->palImg) vkDestroyImage(d->dev, r->palImg, NULL);
    if (r->palMem) vkFreeMemory(d->dev, r->palMem, NULL);
    if (r->palStage) vkDestroyBuffer(d->dev, r->palStage, NULL);
    if (r->palStageMem) vkFreeMemory(d->dev, r->palStageMem, NULL);
    if (r->buf) vkDestroyBuffer(d->dev, r->buf, NULL);
    if (r->bufMem) vkFreeMemory(d->dev, r->bufMem, NULL);
    if (r->nset && s_dpool) vkFreeDescriptorSets(d->dev, s_dpool, (uint32_t)r->nset, r->set);
    memset(r, 0, sizeof *r);
}

/* Give every job retire back, whatever its mask says. Only legal where the
   device has been drained -- `down`, which the seam's vkDeviceWaitIdle is
   above, and the ring-full path, which drains it first. */
static void jret_flush(const TAGPU_VKPASS* d)
{
    int i;
    if (!d || !d->dev) { memset(s_jret, 0, sizeof s_jret); return; }
    for (i = 0; i < JRET_MAX; i++) if (s_jret[i].pending) jret_kill(d, &s_jret[i]);
}

/* A free slot of the ring, draining the device to make one if it is full.

   THE DRAIN LICENSES ONLY WHAT HAS BEEN SUBMITTED. Inside `step` the frame's
   command buffer is still being recorded, and a retire made earlier in this
   same step names sets that buffer binds: its bit for the recording slot must
   survive the drain, or the buffer is submitted with freed sets. Every older
   retire has that bit clear already -- `retire_slot_done` ran at the top of
   this step -- so the retires kept are this step's alone: a set pair per job
   and the self-test's two freed jobs, far below the ring's size. NULL if none
   is free all the same, and the caller then keeps what it would have
   retired: a leak, where freeing it would be a fault. */
static JRETIRE* jret_take(const TAGPU_VKPASS* d)
{
    uint32_t keep = s_cb ? 1u << s_slot : 0u;
    int i;
    for (i = 0; i < JRET_MAX; i++) if (!s_jret[i].pending) return &s_jret[i];
    rlog(LANE ": the job retire is full, so the device is drained to empty it "
               "-- a stall on this frame and nothing worse");
    if (vkDeviceWaitIdle) vkDeviceWaitIdle(d->dev);
    for (i = 0; i < JRET_MAX; i++) {
        s_jret[i].pending &= keep;
        if (!s_jret[i].pending) jret_kill(d, &s_jret[i]);
    }
    for (i = 0; i < JRET_MAX; i++) if (!s_jret[i].pending) return &s_jret[i];
    return NULL;
}

static uint32_t all_slots(const TAGPU_VKPASS* d)
{
    return d->slots >= 32 ? 0xFFFFFFFFu : ((1u << d->slots) - 1u);
}

/* The activation retire's contents, destroyed. Split out from the slot walk so
   that `down` can use it under the seam's device drain. */
static void retire_kill(const TAGPU_VKPASS* d)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (s_ret.buf[i]) vkDestroyBuffer(d->dev, s_ret.buf[i], NULL);
        if (s_ret.mem[i]) vkFreeMemory(d->dev, s_ret.mem[i], NULL);
    }
    memset(&s_ret, 0, sizeof s_ret);
}

/* the current activations become the retire; the caller has checked
   `pending == 0` */
static void retire_take(const TAGPU_VKPASS* d)
{
    int i;
    memset(&s_ret, 0, sizeof s_ret);
    for (i = 0; i < 2; i++) {
        s_ret.buf[i] = s_act[i]; s_ret.mem[i] = s_actMem[i];
        s_act[i] = VK_NULL_HANDLE; s_actMem[i] = VK_NULL_HANDLE;
    }
    s_ret.pending = all_slots(d);
    s_actCap = 0; s_actGen++;
}

/* A SLOT HAS TURNED OVER: its last submit is complete, because the seam waited
   on that slot's fence before handing us this command buffer. Called at the top
   of every step, UNCONDITIONALLY. */
static void prret_slot_done(const TAGPU_VKPASS* d, uint32_t slot);

static void retire_slot_done(const TAGPU_VKPASS* d, uint32_t slot)
{
    int i;
    prret_slot_done(d, slot);
    /* THE JOB RETIRES FIRST, and they are independent of the activation one --
       several can be outstanding at once, each with its own mask, because a job
       is freed whenever its consumer's serial moves and not on a schedule. */
    for (i = 0; i < JRET_MAX; i++) {
        if (!s_jret[i].pending) continue;
        s_jret[i].pending &= ~(1u << slot);
        if (!s_jret[i].pending) jret_kill(d, &s_jret[i]);
    }
    if (!s_ret.pending) return;
    s_ret.pending &= ~(1u << slot);
    if (s_ret.pending) return;
    retire_kill(d);
}

/* THE REPACK, from the weight file's mat4 blocks (tagpu_restore_core.c) into
   the kernel's [tap][ci][co] vec4 rows. Every index below is bounded by the
   loader: `kstride >= 4 + 36 x jin`, and the block's end within `ntex`. The
   last layer's second output vec4 and every bias past `kout` stay zero, which
   is the padding of its one tile to eight channels. Model `m` goes at vec4
   `at`; returns the vec4 offset past it. */
static size_t repack(int m, const TAGPU_RMODEL* w, float* out, size_t at)
{
    int l;
    for (l = 0; l < w->depth; l++) {
        const TAGPU_RLAYER* L = &w->layer[l];
        int last = l + 1 == w->depth;
        int cin = 4 * (int)L->jin, cout = last ? 8 : 4 * (int)L->kout, co4n = cout / 4;
        int tap, ci, co4, c;
        s_wbase[m][l] = (int)at;
        for (tap = 0; tap < 9; tap++)
            for (ci = 0; ci < cin; ci++)
                for (co4 = 0; co4 < co4n; co4++) {
                    float* o = out ? out + (at + (size_t)((tap * cin + ci) * co4n + co4)) * 4 : NULL;
                    if (!o) continue;
                    if (co4 < (int)L->kout) {
                        const float* src = w->body + ((size_t)L->offset + (size_t)co4 * L->kstride +
                                                      4u * (1u + (unsigned)tap * L->jin + (unsigned)(ci / 4)) +
                                                      (unsigned)(ci % 4)) * 4;
                        for (c = 0; c < 4; c++) o[c] = src[c];
                    } else {
                        for (c = 0; c < 4; c++) o[c] = 0.0f;
                    }
                }
        at += (size_t)9 * cin * co4n;
        s_bbase[m][l] = (int)at;
        for (co4 = 0; co4 < co4n; co4++) {
            float* o = out ? out + (at + (size_t)co4) * 4 : NULL;
            if (!o) continue;
            if (co4 < (int)L->kout) {
                const float* src = w->body + ((size_t)L->offset + (size_t)co4 * L->kstride) * 4;
                for (c = 0; c < 4; c++) o[c] = src[c];
            } else {
                for (c = 0; c < 4; c++) o[c] = 0.0f;
            }
        }
        at += (size_t)co4n;
    }
    return at;
}

/* ---- the shared build, on the first job ---- */
static int build_shared(const TAGPU_VKPASS* d)
{
    const TAGPU_RMODEL* w;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorPoolSize psz[3];
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    char b[300];
    int l, m, ok = 0;

    if (s_built) return 1;
    if (!tagpu_vk_restore_up(d)) return 0;
    s_sched.be = &s_be;

    /* every loaded model's every layer's variant, found before anything is built */
    for (m = 0; m < TAGPU_RM_N; m++) {
        if (!(w = tagpu_rcore_model(m))) continue;
        for (l = 0; l < w->depth; l++)
            if (!conv_for(&w->layer[l], l + 1 == w->depth)) {
                _snprintf(b, sizeof b, LANE ": no conv shader for layer %d (%u -> %u tiles) of the %s %dx%d"
                                       " model - the headers were not regenerated for it",
                          l, w->layer[l].jin, w->layer[l].kout, w->name, w->depth, w->ch);
                rlog(b);
                return refuse();
            }
    }

    /* the sampler: NEAREST and clamp; every read here is a texelFetch */
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (vkCreateSampler(d->dev, &sci, NULL, &s_samp) != VK_SUCCESS) goto fail;

    {
        static const int bind[7] = { 0, 1, 2, 3, 4, 5, 6 };
        static const VkDescriptorType type[7] = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE };
        static const int bindM[2] = { 7, 8 };
        static const VkDescriptorType typeM[2] = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE };
        s_dsl = mk_dsl(d, bind, type, 7);
        s_dslMip = mk_dsl(d, bindM, typeM, 2);
    }
    if (!s_dsl || !s_dslMip) goto fail;
    s_plo = mk_plo(d, s_dsl);
    s_ploMip = mk_plo(d, s_dslMip);
    if (!s_plo || !s_ploMip) goto fail;

    s_pipeFill = mk_pipe(d, tagpu_spv_tagpu_restore_comp_FILL_CS,
                         sizeof tagpu_spv_tagpu_restore_comp_FILL_CS / 4, s_plo);
    s_pipeOut  = mk_pipe(d, tagpu_spv_tagpu_restore_comp_OUT_CS,
                         sizeof tagpu_spv_tagpu_restore_comp_OUT_CS / 4, s_plo);
    s_pipeMip  = mk_pipe(d, tagpu_spv_tagpu_restore_comp_MIP_CS,
                         sizeof tagpu_spv_tagpu_restore_comp_MIP_CS / 4, s_ploMip);
    if (!s_pipeFill || !s_pipeOut || !s_pipeMip) goto fail;
    /* ONE PIPELINE PER LAYER, not per shape: the full model's ten middle layers
       then hold ten pipelines of one module. It keeps the dispatch a table
       lookup by layer index, and a pipeline costs a few KB. */
    for (m = 0; m < TAGPU_RM_N; m++) {
        if (!(w = tagpu_rcore_model(m))) continue;
        for (l = 0; l < w->depth; l++) {
            const CONVV* v = conv_for(&w->layer[l], l + 1 == w->depth);
            s_pipeConv[m][l] = mk_pipe(d, v->spv, v->words, s_plo);
            s_convTh[m][l] = v->th;
            if (!s_pipeConv[m][l]) goto fail;
        }
    }

    /* the weights, repacked into a host-visible staging and copied to a
       device-local buffer on the first slice */
    {
        size_t n = 0;
        for (m = 0; m < TAGPU_RM_N; m++)
            if ((w = tagpu_rcore_model(m))) n = repack(m, w, NULL, n);
        s_wbytes = (VkDeviceSize)n * 16;
        if (!mk_buffer(d, s_wbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s_wbuf, &s_wmem, NULL)) goto fail;
        if (!mk_buffer(d, s_wbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       &s_wstage, &s_wstageMem, &s_wstageMap)) goto fail;
        n = 0;
        for (m = 0; m < TAGPU_RM_N; m++)
            if ((w = tagpu_rcore_model(m))) n = repack(m, w, (float*)s_wstageMap, n);
        s_wUp = 0;
    }

    /* DEVICE-LOCAL AND UNMAPPED: the bytes are written by vkCmdUpdateBuffer
       at each batch's FILL, so what orders one batch's write against the
       previous batch's read is a barrier in the stream (`upload_table`). */
    if (!mk_buffer(d, (VkDeviceSize)sizeof s_sched.slot,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s_tab, &s_tabMem, NULL)) goto fail;

    /* a descriptor pool for every job's two sets and a whole mip chain each */
    memset(psz, 0, sizeof psz);
    psz[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    psz[0].descriptorCount = TAGPU_R_MAXJOBS * 2 * 4;
    psz[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    psz[1].descriptorCount = TAGPU_R_MAXJOBS * (2 * 2 + TAGPU_VK_MAXMIP);
    psz[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    psz[2].descriptorCount = TAGPU_R_MAXJOBS * (2 + TAGPU_VK_MAXMIP);
    /* TWICE the live need: a job's sets are retired, not freed, when the
       activations are re-created, so a job can hold its new pair while the
       old one waits out the slots in flight */
    for (l = 0; l < 3; l++) psz[l].descriptorCount *= 2;
    dpi.maxSets = TAGPU_R_MAXJOBS * (2 + TAGPU_VK_MAXMIP) * 2;
    dpi.poolSizeCount = 3; dpi.pPoolSizes = psz;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (vkCreateDescriptorPool(d->dev, &dpi, NULL, &s_dpool) != VK_SUCCESS) goto fail;

    if (s_dev.timestamps) {
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 4;                  /* two pairs, one per parity */
        if (vkCreateQueryPool(d->dev, &qi, NULL, &s_qpool) != VK_SUCCESS) s_qpool = VK_NULL_HANDLE;
    }
    s_sched.timer = s_qpool ? 1 : 0;

    w = tagpu_rcore_model(TAGPU_RM_TINY);
    _snprintf(b, sizeof b, LANE ": built: fp32 compute, full %dx%d%s%s, %u KB of weights, %s",
              tagpu_rcore_model(TAGPU_RM_FULL)->depth, tagpu_rcore_model(TAGPU_RM_FULL)->ch,
              w ? " and tiny" : ", no tiny", w ? "" : " (the terrain runs full)",
              (unsigned)(s_wbytes >> 10), s_qpool ? "timestamp budget" : "fixed slices");
    rlog(b);
    s_built = 1;
    ok = 1;
fail:
    if (!ok) rlog(LANE ": the shared resources could not be built - the lane stays indexed");
    return ok;
}

/* ============================= THE JOB ============================= */
struct TAGPU_VKRJOB {
    TAGPU_RCORE*   core;
    VkImage        srcImg;                  /* ...for the dump, not the dispatch */
    VkImageView    srcView;                 /* the consumer's source atlas    */
    int            srcW, srcH;
    int            srcBase;                 /* 1 RGBA base, 0 R8 indices: pc.base */
    VkImage        dstImg;                  /* ...and its restored twin       */
    VkImageView    dstView;
    int            dstW, dstH;
    /* THE MIP CHAIN, and only for a consumer whose twin has one: `chainN` is 0
       everywhere else and every mip path below is then a single compare. The
       views are the CONSUMER's -- a view of level i+1 written and a view of
       level i read, each naming exactly one level, which is what makes reading
       L-1 while writing L sound: the source view CANNOT reach the level being
       written. */
    int            chainN;                  /* levels 1..chainN are reduced   */
    int            chainDim;                /* level 0's square size          */
    VkDescriptorSet chainSet[TAGPU_VK_MAXMIP];
    int            chainPainted;            /* `painted` at the last reduction*/
    int            chainDone;               /* it has run at least once       */
    VkImage        palImg;                  /* the palette snapshot, ours     */
    VkDeviceMemory palMem;
    VkImageView    palView;
    VkBuffer       palStage;
    VkDeviceMemory palStageMem;
    unsigned char* palMap;
    int            palDue;                  /* an upload is pending           */
    /* set[p] reads activation buffer p and writes 1 - p: CONV and OUT bind
       the side they read, FILL binds set[1], whose written side is buffer 0 */
    VkDescriptorSet set[2];
    unsigned       setGen;                  /* the s_actGen the sets name     */
    int            dstReady;                /* brought to SHADER_READ_ONLY    */
    int            clearDue;                /* ...and cleared to alpha 0      */
    /* 1 when the destination HOLDS SOMETHING this pass must not throw away --
       which is what picks `dst_ready`'s source layout. A repaint exists to
       recolour a restored atlas in place; transitioning it from UNDEFINED
       licenses the driver to discard every texel of it, so the repaint would
       blank exactly the world it exists to avoid blanking. Set from `repaint`
       at creation -- the consumer's statement that the atlas already holds a
       restore -- and then by `dst_ready` itself. */
    int            dstHas;
    /* THE BYTE DUMP (`dump_step`). It lives here rather than in each consumer
       because the destination, its size and the moment it is finished are all
       facts this file already holds.
       0 nothing, 1 a copy is recorded and owed to `dumpSlot`, 2 written at
       `dumpPainted` frames. */
    char           tag[16];
    VkBuffer       dumpBuf;
    VkDeviceMemory dumpMem;
    unsigned char* dumpMap;
    VkDeviceSize   dumpBytes;
    int            dumpW, dumpH;
    uint32_t       dumpSlot;
    int            dumpState;
    int            dumpPainted;
    VkDeviceSize   dumpSrcOff, dumpSrcBytes;
    /* EVERY FRAME OUT HAS PAINTED, for the dump's `.idx` -- kept only when
       `tagpu_restoredump.on` was there at `job_new`, so a player's session
       keeps nothing. The queued records themselves, a neighbourhood's
       included, so a remap moves them with the queue (`job_remap`) and the
       list always names the cells as they are in the destination now. */
    TAGPU_RQF*     rec;
    int            nrec, caprec, recOn;
    int            feeding;                 /* the consumer has more to add    */
};
static struct TAGPU_VKRJOB s_vjob[TAGPU_R_MAXJOBS];

/* A FRESH PAIR, AND THE OLD ONE RETIRED -- never a rewrite in place.
   `vkUpdateDescriptorSets` on a set that a submitted command buffer has bound
   is undefined behaviour, and this one is bound by every frame still in flight:
   the activations are re-created whenever a batch needs a bigger grid, and
   each of those bumps `s_actGen`; rewriting every live job's sets then changes
   them while earlier frames are still executing against them.

   WHAT IT LOOKS LIKE, because this is the shape to recognise rather than the
   API rule to recite: OUT computes `c - net`, the palette colour minus the
   network's output, so a set that still named the buffer FILL wrote gives
   net == c and paints the cell BLACK -- MEASURED on the fragment backend as
   every texel of the earliest frames black, the source byte-identical, and
   the count varying run to run because it depends on which batches happen to
   straddle a grow.

   0 when no fresh pair can be had, and the caller then fails the batch. The
   pool is sized for a job's live pair and its retired one. */
static int set_pair_new(const TAGPU_VKPASS* d, struct TAGPU_VKRJOB* g)
{
    VkDescriptorSetLayout lay[2];
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSet got[2];
    lay[0] = s_dsl; lay[1] = s_dsl;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, got) != VK_SUCCESS) return 0;
    if (g->set[0]) {
        JRETIRE* r = jret_take(d);
        if (!r) {
            vkFreeDescriptorSets(d->dev, s_dpool, 2, got);
            return 0;
        }
        r->set[0] = g->set[0]; r->set[1] = g->set[1];
        r->nset = 2;
        r->pending = all_slots(d);
    }
    g->set[0] = got[0]; g->set[1] = got[1];
    return 1;
}

static void write_sets(const TAGPU_VKPASS* d, struct TAGPU_VKRJOB* g)
{
    VkDescriptorBufferInfo bi[4];
    VkDescriptorImageInfo  ii[3];
    VkWriteDescriptorSet   wr[7];
    int p, i;

    for (p = 0; p < 2; p++) {
        memset(bi, 0, sizeof bi); memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
        bi[0].buffer = s_act[p];     bi[0].range = VK_WHOLE_SIZE;
        bi[1].buffer = s_act[1 - p]; bi[1].range = VK_WHOLE_SIZE;
        bi[2].buffer = s_wbuf;       bi[2].range = VK_WHOLE_SIZE;
        bi[3].buffer = s_tab;        bi[3].range = VK_WHOLE_SIZE;
        ii[0].sampler = s_samp; ii[0].imageView = g->srcView;
        ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii[1].sampler = s_samp; ii[1].imageView = g->palView;
        ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        /* GENERAL: OUT writes the destination in the layout `out_begin`
           brings it to, and a storage image is used in GENERAL */
        ii[2].imageView = g->dstView;
        ii[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        for (i = 0; i < 7; i++) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = g->set[p];
            wr[i].dstBinding = (uint32_t)i;
            wr[i].descriptorCount = 1;
            if (i < 4) {
                wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                wr[i].pBufferInfo = &bi[i];
            } else {
                wr[i].descriptorType = i == 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                wr[i].pImageInfo = &ii[i - 4];
            }
        }
        vkUpdateDescriptorSets(d->dev, 7, wr, 0, NULL);
    }
    g->setGen = s_actGen;
}

/* ====================== THE BACKEND'S ENTRIES ====================== */

/* THE ACTIVATIONS, grown between batches only, and RETIRED rather than freed.
   The three answers are tagpu_restore_core.h's: 1 ready, 0 this device cannot,
   -1 a retire is still in flight and the core should ask again next slice. */
static int vk_act_ensure(int gw, int gh)
{
    const TAGPU_VKPASS* d = s_d;
    uint64_t texels = (uint64_t)gw * (uint64_t)gh;
    VkDeviceSize bytes;
    int i;

    if (!d || !s_cb || !s_built || gw <= 0 || gh <= 0) return 0;
    if (s_act[0] && s_actCap >= texels) return 1;
    if (s_ret.pending) return -1;               /* the queue is untouched */
    bytes = (VkDeviceSize)(texels * (uint64_t)act_ch() * 4u);
    /* bounded by `up`'s check of the largest grid the ladder makes */
    if ((uint64_t)bytes > (uint64_t)s_dev.maxSsbo) return 0;

    if (s_act[0]) retire_take(d);
    for (i = 0; i < 2; i++)
        if (!mk_buffer(d, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s_act[i], &s_actMem[i], NULL)) {
            rlog(LANE ": the activation buffers could not be allocated");
            return 0;
        }
    s_actCap = texels; s_actGen++;
    if (tagpu_rcore_opt()->log) {
        char b[160];
        _snprintf(b, sizeof b, LANE ": activations %dx%d x %d channels (%d MB)", gw, gh, act_ch(),
                  (int)(2u * (uint64_t)bytes >> 20));
        rlog(b);
    }
    return 1;
}

/* THE IDLE RELEASE GOES THROUGH THE RETIRE, not through a free.
   The core asks after 180 slices without work, and the buffers may still be
   named by a submitted command buffer -- so freeing them here would be a
   use-after-free decided by a clock. Handing them to the retire instead
   gives the same memory back a few frames later, licensed by the slot
   bitmask reaching zero. */
static int vk_act_free(void)
{
    if (!s_d || !s_act[0]) return 0;
    if (s_ret.pending) return 0;            /* one retire at a time */
    retire_take(s_d);
    return 1;
}

static int vk_ready(void) { return s_built && s_state == ST_READY; }
static int vk_may_draw(void) { return tagpu_classicpp_assets(); }

static void vk_slice_begin(int q)
{
    if (!s_qpool || !s_cb) return;
    vkCmdResetQueryPool(s_cb, s_qpool, (uint32_t)(q * 2), 2);
    vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_qpool, (uint32_t)(q * 2));
}

static void vk_slice_end(int q)
{
    if (!s_qpool || !s_cb) return;
    vkCmdWriteTimestamp(s_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_qpool, (uint32_t)(q * 2 + 1));
}

/* 1 = `*ns` is that slice's GPU time, 0 = not in yet. Asked WITHOUT
   VK_QUERY_RESULT_WAIT_BIT, so this never blocks -- the core's double buffering
   is what makes that safe. */
static int vk_timer_poll(int q, double* ns)
{
    uint64_t t[2] = { 0, 0 };
    if (!s_qpool || !s_d) return 0;
    if (vkGetQueryPoolResults(s_d->dev, s_qpool, (uint32_t)(q * 2), 2, sizeof t, t,
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    if (t[1] <= t[0]) return 0;
    *ns = (double)(t[1] - t[0]) * s_dev.tsPeriod;
    return 1;
}

static void vk_timer_off(void) { /* the pool stays; the core stops asking */ }

/* The slot table onto the device, at each batch's FILL.

   THE BYTES GO INTO THE COMMAND STREAM, NOT INTO A MAPPED REGION. "One batch
   is ever in flight, so nothing else writes them in between" is true of the
   DEVICE and false of the RECORDING: a slice can issue more than one batch
   (tagpu_rcore_step re-picks at every batch boundary and runs until the
   GPU-time budget is spent), and two batches' `memcpy` into one region land on
   the same address before either batch has executed -- so the first batch's
   frames would be restored through the SECOND batch's table: wrong source
   rects, and wrong colour keys. MEASURED on the fragment backend against the
   OpenGL renderer it replaced, on 118 of 1304 feature frames. A bigger arena
   is not the fix: batches per slice is a time budget rather than a count.
   vkCmdUpdateBuffer records the data AT THIS POINT IN THE STREAM, 4 KB, well
   inside the 65 536 it allows.

   The barrier before is the write-after-read half: the previous batch's
   dispatches read this buffer, and `stage_done` after its OUT names TRANSFER
   as a destination, which orders them before this write. The barrier after
   makes the write visible to this batch's dispatches. */
static void upload_table(const TAGPU_RDRAWREQ* r)
{
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    vkCmdUpdateBuffer(s_cb, s_tab, 0, (VkDeviceSize)sizeof s_sched.slot, r->slot);
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = s_tab;
    bb.size = VK_WHOLE_SIZE;
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
}

/* A job's destination, brought to SHADER_READ_ONLY_OPTIMAL -- the layout the
   consumer samples it in and the one every OUT starts from -- and cleared to
   alpha 0 unless this is a repaint in place. `dstHas` is the source layout:
   UNDEFINED for an image whose contents are ours to throw away (which is
   cheaper -- the driver may skip a decompress), SHADER_READ_ONLY_OPTIMAL for
   one a repaint is about to paint over. See the field. */
static void dst_ready(struct TAGPU_VKRJOB* g)
{
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkClearColorValue cc;
    VkImageSubresourceRange rg;
    const VkPipelineStageFlags readyStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (g->dstReady) return;
    memset(&cc, 0, sizeof cc);
    memset(&rg, 0, sizeof rg);
    rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rg.levelCount = 1; rg.layerCount = 1;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = g->dstImg;
    mb.subresourceRange = rg;
    mb.oldLayout = g->dstHas ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                             : VK_IMAGE_LAYOUT_UNDEFINED;
    mb.srcAccessMask = g->dstHas ? VK_ACCESS_SHADER_READ_BIT : 0;
    mb.newLayout = g->clearDue ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    mb.dstAccessMask = g->clearDue ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s_cb,
                         g->dstHas ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         g->clearDue ? VK_PIPELINE_STAGE_TRANSFER_BIT : readyStage,
                         0, 0, NULL, 0, NULL, 1, &mb);
    if (g->clearDue) {
        vkCmdClearColorImage(s_cb, g->dstImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rg);
        mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, readyStage,
                             0, 0, NULL, 0, NULL, 1, &mb);
        g->clearDue = 0;
    }
    g->dstReady = 1;
    g->dstHas = 1;                         /* from here on there is something  */
}

/* OUT WRITES THE DESTINATION AS A STORAGE IMAGE, so it is GENERAL for the
   dispatch and SHADER_READ_ONLY again after it. The transition in PRESERVES
   the contents (the old layout is not UNDEFINED): OUT paints this batch's
   cells onto what earlier batches painted. Its source scope is every reader
   the image has had -- the consumer's sample, a previous OUT, a dump's copy --
   and the transition out makes this batch's writes visible to all of them. */
static void dst_layout(struct TAGPU_VKRJOB* g, int toGeneral)
{
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = g->dstImg;
    mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    if (toGeneral) {
        mb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(s_cb,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    } else {
        mb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    }
}

/* THE SLICE'S ORDERING BARRIER, and it is the whole argument for sharing the
   activations and the table rather than copying them per slot.

   `vkCmdPipelineBarrier`'s first synchronisation scope includes every command
   submitted EARLIER IN SUBMISSION ORDER on this queue -- not merely earlier in
   this command buffer -- so one barrier here makes the previous slice's reads
   and writes a fact before this slice's. That is a fence, not a hope: nothing
   about it depends on how long a slice took or on how many frames are in
   flight.

   It is emitted on the slice's FIRST DISPATCH rather than at the top of
   `step`, because `step` runs every frame including the ones with nothing
   queued, and a barrier per idle frame is a cost for nobody. */
static void slice_head(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    if (!s_wUp && s_wbuf && s_wstage) {
        VkBufferCopy bc;
        memset(&bc, 0, sizeof bc);
        bc.size = s_wbytes;
        vkCmdCopyBuffer(s_cb, s_wstage, s_wbuf, 1, &bc);
        s_wUp = 1;
    }
    mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = mb.srcAccessMask;
    vkCmdPipelineBarrier(s_cb,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* the job's palette snapshot onto the device, before the FILL that samples it */
static void upload_pal(struct TAGPU_VKRJOB* g)
{
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkBufferImageCopy bc;
    if (!g->palDue || !g->palStage) return;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = g->palImg;
    mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.srcAccessMask = 0;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &mb);
    memset(&bc, 0, sizeof bc);
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = 256; bc.imageExtent.height = 1; bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cb, g->palStage, g->palImg,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &mb);
    g->palDue = 0;
}

static void push(VkPipelineLayout plo, const PUSH* pc)
{
    vkCmdPushConstants(s_cb, plo, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *pc, pc);
}

static int vk_draw(const TAGPU_RDRAWREQ* r)
{
    struct TAGPU_VKRJOB* g = (struct TAGPU_VKRJOB*)r->job->owner;
    const TAGPU_VKPASS* d = s_d;
    const TAGPU_RMODEL* w = r->job->m;
    const int m = r->job->model;
    PUSH pc;

    if (!d || !s_cb || !g || !s_built) return 0;
    if (!s_sliceOpen) { slice_head(); s_sliceOpen = 1; }
    s_rec = 1;
    /* the fault lever's crash, inside a restorer call on the render thread */
    if (r->kind == TAGPU_RDRAW_FILL && tagpu_rguard_fault("crash"))
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, NULL);
    /* A GENERATION CHANGE MEANS NEW SETS, not a rewrite of the bound ones --
       see `set_pair_new`. `setGen` 0 is the first dispatch, where the sets
       were allocated by `job_new` and have never been bound. */
    if (g->setGen != s_actGen) {
        if (g->setGen != 0 && !set_pair_new(d, g)) {
            rlog(LANE ": no descriptor sets for re-created activations - "
                       "this batch is dropped rather than bound to the old ones");
            return 0;
        }
        write_sets(d, g);
    }

    memset(&pc, 0, sizeof pc);
    pc.gw = r->gw; pc.gh = r->gh; pc.pitch = r->pitch; pc.cols = r->cols;
    pc.base = g->srcBase; pc.keyR = w->depth;
    pc.dstW = g->dstW; pc.dstH = g->dstH;

    if (r->kind == TAGPU_RDRAW_FILL) {
        upload_pal(g);
        upload_table(r);
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeFill);
        vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_plo, 0, 1,
                                &g->set[1], 0, NULL);
        push(s_plo, &pc);
        vkCmdDispatch(s_cb, (uint32_t)(r->gw / 8), (uint32_t)(r->gh / 8), 1);
        stage_done(s_cb);
        return 1;
    }

    if (r->kind == TAGPU_RDRAW_CONV) {
        int th;
        if (m < 0 || m >= TAGPU_RM_N || r->layer < 0 || r->layer >= w->depth || !s_pipeConv[m][r->layer]) {
            rlog(LANE ": a conv dispatch for a layer the model does not have");
            return 0;
        }
        th = s_convTh[m][r->layer];
        if (r->rows <= 0 || r->rows % th || r->gw % TAGPU_R_GRIDX) {
            rlog(LANE ": a conv band the kernel's tiling does not cover");
            return 0;
        }
        pc.y0 = r->y0;
        pc.wbase = s_wbase[m][r->layer]; pc.bbase = s_bbase[m][r->layer];
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeConv[m][r->layer]);
        vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_plo, 0, 1,
                                &g->set[r->srcAct], 0, NULL);
        push(s_plo, &pc);
        vkCmdDispatch(s_cb, (uint32_t)(r->gw / TAGPU_R_GRIDX), (uint32_t)(r->rows / th), 1);
        if (r->y0 + r->rows >= r->gh) stage_done(s_cb);   /* the layer is complete */
        return 1;
    }

    /* OUT: straight into the consumer's atlas, in the consumer's cell layout,
       one workgroup layer per frame over the largest cell of the batch */
    {
        int t, qw = 0, qh = 0;
        for (t = 0; t < r->nframes; t++) {
            const TAGPU_RSLOT* s = &r->slot[(t / r->cols) * TAGPU_R_SLOTCOLS + (t % r->cols)];
            int ww = s->sw + 2 * s->border + s->padR, hh = s->sh + 2 * s->border + s->padB;
            if (ww > qw) qw = ww;
            if (hh > qh) qh = hh;
        }
        if (r->nframes <= 0 || qw <= 0 || qh <= 0) return 1;   /* nothing to paint */
        dst_ready(g);
        dst_layout(g, 1);
        vkCmdBindPipeline(s_cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeOut);
        vkCmdBindDescriptorSets(s_cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_plo, 0, 1,
                                &g->set[r->srcAct], 0, NULL);
        push(s_plo, &pc);
        vkCmdDispatch(s_cb, (uint32_t)((qw + 7) / 8), (uint32_t)((qh + 7) / 8), (uint32_t)r->nframes);
        dst_layout(g, 0);
        stage_done(s_cb);
        if (g->recOn) {
            if (g->nrec + r->nframes > g->caprec) {
                int cap = g->caprec ? g->caprec : 256;
                TAGPU_RQF* n;
                while (cap < g->nrec + r->nframes) cap *= 2;
                n = (TAGPU_RQF*)realloc(g->rec, (size_t)cap * sizeof *n);
                if (n) { g->rec = n; g->caprec = cap; }
            }
            for (t = 0; t < r->nframes && g->nrec < g->caprec; t++) g->rec[g->nrec++] = r->job->bf[t];
        }
    }
    return 1;
}

static const TAGPU_RBACKEND s_be = {
    LANE,
    vk_ready,
    vk_act_ensure,
    vk_act_free,
    vk_draw,
    vk_slice_begin,
    vk_slice_end,
    vk_timer_poll,
    vk_timer_off,
    vk_may_draw
};

/* ======================== THE PUBLIC JOB API ======================== */

static void pal_pack(unsigned char* out, const unsigned char* pal)
{
    int i;
    for (i = 0; i < 256; i++) {
        out[i * 4] = pal[i * 4]; out[i * 4 + 1] = pal[i * 4 + 1];
        out[i * 4 + 2] = pal[i * 4 + 2]; out[i * 4 + 3] = 255;
    }
}

static TAGPU_VKRJOB* job_new_impl(const TAGPU_VKPASS* d, const char* tag,
                                  int prio, int oneshot, int model, int repaint,
                                  VkImage srcImg, VkImageView srcView,
                                  int srcW, int srcH, int srcBase,
                                  const unsigned char* pal,
                                  VkImage dstImg, VkImageView dstView,
                                  int dstW, int dstH)
{
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSetLayout lay[2];
    TAGPU_RCORE* c;
    struct TAGPU_VKRJOB* g;
    char b[220];

    if (!d || !srcView || !dstImg || !dstView || !pal ||
        srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return NULL;
    if (!build_shared(d)) return NULL;
    c = tagpu_rcore_job_new(&s_sched, tag, prio, oneshot, model, NULL);
    if (!c) return NULL;
    g = &s_vjob[(int)(c - s_sched.jobs)];
    memset(g, 0, sizeof *g);
    c->owner = g; g->core = c;
    g->srcImg = srcImg; g->srcView = srcView; g->srcW = srcW; g->srcH = srcH;
    g->srcBase = srcBase ? 1 : 0;
    g->dstImg = dstImg; g->dstView = dstView; g->dstW = dstW; g->dstH = dstH;
    g->clearDue = repaint ? 0 : 1;
    g->dstHas   = repaint ? 1 : 0;
    g->recOn    = GetFileAttributesA("tagpu_restoredump.on") != INVALID_FILE_ATTRIBUTES;
    if (tag) { strncpy(g->tag, tag, sizeof g->tag - 1); g->tag[sizeof g->tag - 1] = 0; }
    else     { strcpy(g->tag, "restore"); }

    /* the palette snapshot: R,G,B,pad -> RGBA8, uploaded before the first FILL */
    if (!mk_image(d, 256, 1, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  &g->palImg, &g->palMem, &g->palView)) goto fail;
    if (!mk_buffer(d, 256 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &g->palStage, &g->palStageMem, &g->palMap)) goto fail;
    pal_pack(g->palMap, pal);
    g->palDue = 1;

    lay[0] = s_dsl; lay[1] = s_dsl;
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, g->set) != VK_SUCCESS) {
        memset(g->set, 0, sizeof g->set);
        goto fail;
    }
    /* written on the first dispatch: the activations may not exist yet, and a
       set naming VK_NULL_HANDLE is not a set that can be bound */
    g->setGen = 0;

    if (!oneshot) {
        _snprintf(b, sizeof b, LANE ": %s: lazy restore armed (%dx%d twin of the %dx%d atlas)",
                  c->tag, dstW, dstH, srcW, srcH);
        rlog(b);
    }
    return g;
fail:
    tagpu_vk_restore_job_free(d, g);
    return NULL;
}

TAGPU_VKRJOB* tagpu_vk_restore_job_new(const TAGPU_VKPASS* d, const char* tag,
                                       int prio, int oneshot, int model, int repaint,
                                       VkImage srcImg, VkImageView srcView,
                                       int srcW, int srcH, int srcBase,
                                       const unsigned char* pal,
                                       VkImage dstImg, VkImageView dstView,
                                       int dstW, int dstH)
{
    TAGPU_VKRJOB* j;
    tagpu_rguard_enter();
    j = job_new_impl(d, tag, prio, oneshot, model, repaint, srcImg, srcView, srcW, srcH, srcBase,
                     pal, dstImg, dstView, dstW, dstH);
    tagpu_rguard_leave();
    return j;
}

static int job_chain_impl(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j,
                          int mips, int dim,
                          const VkImageView* attach, const VkImageView* sample)
{
    VkDescriptorSetLayout lay[TAGPU_VK_MAXMIP];
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    int L;

    if (!d || !d->dev || !j || !j->core || !j->core->used) return 0;
    if (mips < 1 || mips > TAGPU_VK_MAXMIP || dim <= 0 || !attach || !sample) return 0;
    if (!s_built || !s_pipeMip || !s_dslMip || !s_dpool) return 0;
    if (j->chainN) return 1;                  /* registered once, by contract */
    /* AN ODD LEVEL WOULD NEED A WEIGHTED THREE-TAP, not a 2x2 average, and
       this shader does not pretend to be one. Refused WHOLE rather than
       part-reduced: a chain half ours and half nobody's is the one outcome
       neither lane can describe. */
    for (L = 1; L <= mips; L++)
        if ((dim >> L) < 1 || ((dim >> (L - 1)) & 1)) {
            char b[160];
            _snprintf(b, sizeof b, "%s: %s: a level of the %d-square chain is odd - "
                      "no reduction here", LANE, j->core->tag, dim);
            rlog(b);
            return 0;
        }
    for (L = 0; L < mips; L++) {
        if (!attach[L] || !sample[L]) return 0;
        lay[L] = s_dslMip;
    }
    dai.descriptorPool = s_dpool;
    dai.descriptorSetCount = (uint32_t)mips;
    dai.pSetLayouts = lay;
    if (vkAllocateDescriptorSets(d->dev, &dai, j->chainSet) != VK_SUCCESS) {
        char b[160];
        memset(j->chainSet, 0, sizeof j->chainSet);
        _snprintf(b, sizeof b, "%s: %s: no descriptor sets for the mip chain - "
                  "the twin keeps level 0 only", LANE, j->core->tag);
        rlog(b);
        return 0;
    }
    /* written ONCE, here: the views of level L are fixed for the life of the
       job. 7 reads level L (SHADER_READ_ONLY, where every level rests), 8
       writes level L + 1 (GENERAL for the dispatch, `chain_step`). */
    for (L = 0; L < mips; L++) {
        VkDescriptorImageInfo  ii[2];
        VkWriteDescriptorSet   wr[2];
        memset(ii, 0, sizeof ii); memset(wr, 0, sizeof wr);
        ii[0].sampler = s_samp; ii[0].imageView = sample[L];
        ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii[1].imageView = attach[L];
        ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[0].dstSet = j->chainSet[L]; wr[0].dstBinding = 7;
        wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[0].pImageInfo = &ii[0];
        wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[1].dstSet = j->chainSet[L]; wr[1].dstBinding = 8;
        wr[1].descriptorCount = 1;
        wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[1].pImageInfo = &ii[1];
        vkUpdateDescriptorSets(d->dev, 2, wr, 0, NULL);
    }
    j->chainN = mips;
    j->chainDim = dim;
    j->chainPainted = 0;
    j->chainDone = 0;
    {   char b[180];
        _snprintf(b, sizeof b, "%s: %s: the twin's %d mip level(s) are reduced HERE, "
                  "by the integer (sum+1)/4 box",
                  LANE, j->core->tag, mips);
        rlog(b); }
    return 1;
}

int tagpu_vk_restore_job_chain(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j,
                               int mips, int dim,
                               const VkImageView* attach, const VkImageView* sample)
{
    int r;
    tagpu_rguard_enter();
    r = job_chain_impl(d, j, mips, dim, attach, sample);
    tagpu_rguard_leave();
    return r;
}

int tagpu_vk_restore_job_add(TAGPU_VKRJOB* j, const TAGPU_RGLSL_FRAME* frames, int count)
{
    if (!j || !j->core) return 0;
    return tagpu_rcore_job_add(&s_sched, j->core, frames, count);
}

int tagpu_vk_restore_fits(const TAGPU_VKPASS* d, unsigned long long bytes, char* why, int whyLen)
{
    VkPhysicalDeviceMemoryProperties mp;
    unsigned long long heap = 0, freeB;
    uint32_t i;
    if (why && whyLen > 0) why[0] = 0;
    if (!d || !d->pd) return 0;
    if (tagpu_vk_mem_free(&freeB, &heap)) {
        if (why) {
            _snprintf(why, whyLen, "%llu MB against half of the %llu MB the driver says is free (of %llu MB)",
                      bytes >> 20, freeB >> 20, heap >> 20);
            why[whyLen - 1] = 0;
        }
        return bytes <= freeB / 2;
    }
    vkGetPhysicalDeviceMemoryProperties(d->pd, &mp);
    for (i = 0; i < mp.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            mp.memoryHeaps[i].size > heap)
            heap = mp.memoryHeaps[i].size;
    freeB = heap / 4;
    if (why) {
        _snprintf(why, whyLen, "%llu MB against half of %llu MB free (a quarter of the %llu MB heap: "
                               "no memory-budget query here)",
                  bytes >> 20, freeB >> 20, heap >> 20);
        why[whyLen - 1] = 0;
    }
    return bytes <= freeB / 2;
}

int tagpu_vk_restore_job_add_nbhd(TAGPU_VKRJOB* j, const TAGPU_RNBFRAME* frames, int count)
{
    if (!j || !j->core) return 0;
    return tagpu_rcore_job_add_nbhd(&s_sched, j->core, frames, count);
}

void tagpu_vk_restore_job_budget(TAGPU_VKRJOB* j, int prio, double capMs)
{
    if (j && j->core) tagpu_rcore_job_budget(j->core, prio, capMs);
}

int tagpu_vk_restore_job_queued(const TAGPU_VKRJOB* j)
{
    return (j && j->core && j->core->used) ? j->core->qn : 0;
}

void tagpu_vk_restore_job_feeding(TAGPU_VKRJOB* j, int more)
{
    if (j) j->feeding = more != 0;
}

int tagpu_vk_restore_job_idle(const TAGPU_VKRJOB* j)
{
    return !j || !j->core || !j->core->used || (j->core->qn == 0 && !j->core->inflight);
}

int tagpu_vk_restore_job_failed(const TAGPU_VKRJOB* j)
{
    return j && j->core && j->core->used && j->core->failed;
}

int tagpu_vk_restore_job_painted(const TAGPU_VKRJOB* j)
{
    return (j && j->core && j->core->used) ? j->core->tframes : 0;
}

int tagpu_vk_restore_job_remap(TAGPU_VKRJOB* j, int (*map)(void* ctx, TAGPU_RGLSL_FRAME* f),
                               void* ctx, int* kept, int* requeued, int* dropped)
{
    int i, k = 0, ok;
    if (!j || !j->core || !j->core->used) {
        *kept = 0; *requeued = 0; *dropped = 0;
        return 1;
    }
    ok = tagpu_rcore_job_remap(j->core, map, ctx, kept, requeued, dropped);
    if (ok) {
        /* a neighbourhood is dropped, as the core drops it */
        for (i = 0; i < j->nrec; i++)
            if (!j->rec[i].nb && map(ctx, &j->rec[i].f)) j->rec[k++] = j->rec[i];
        j->nrec = k;
    }
    return ok;
}

int tagpu_vk_restore_job_dst_live(const TAGPU_VKRJOB* j)
{
    return j && j->core && j->core->used && j->dstReady && j->chainN == 0;
}

/* below, beside the rest of the dump */
static void dump_free(const TAGPU_VKPASS* d, struct TAGPU_VKRJOB* g);

/* THE TEARDOWN IS NOT GUARDED (tagpu_restore_guard.h), this and `down`: a
   fault in it is the driver freeing objects, not the restorer's work, and at
   exit it would relaunch a game the player has just quit. */
void tagpu_vk_restore_job_free(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j)
{
    if (!j) return;
    if (d && d->dev) {
        /* INTO THE RETIRE, NOT DESTROYED HERE. Every one of these objects can
           still be named by a submitted command buffer -- see JRETIRE. The
           MAPPINGS go now, because unmapping is not a device operation and the
           memory they belong to is kept until the mask clears. */
        JRETIRE* r = jret_take(d);
        JRETIRE kept;
        int L;
        if (!r) {
            rlog(LANE ": no retire could be had, so a freed job's objects are kept, not destroyed");
            memset(&kept, 0, sizeof kept);
            r = &kept;
        }
        if (j->dumpMap && j->dumpMem) vkUnmapMemory(d->dev, j->dumpMem);
        r->buf = j->dumpBuf; r->bufMem = j->dumpMem;
        j->dumpBuf = VK_NULL_HANDLE; j->dumpMem = VK_NULL_HANDLE; j->dumpMap = NULL;
        if (j->palMap && j->palStageMem) vkUnmapMemory(d->dev, j->palStageMem);
        r->palImg = j->palImg; r->palMem = j->palMem; r->palView = j->palView;
        r->palStage = j->palStage; r->palStageMem = j->palStageMem;
        if (j->set[0] && s_dpool) {
            r->set[0] = j->set[0]; r->set[1] = j->set[1];
            r->nset = 2;
        }
        /* AND THE CHAIN'S: a reduction recorded into some slot's command
           buffer names the set. The VIEWS in it are the consumer's and are not
           retired here -- the contract says the consumer keeps them until
           after this call. */
        if (s_dpool)
            for (L = 0; L < j->chainN && r->nset < 2 + TAGPU_VK_MAXMIP; L++)
                if (j->chainSet[L]) r->set[r->nset++] = j->chainSet[L];
        r->pending = all_slots(d);
    }
    if (j->core) tagpu_rcore_job_free(&s_sched, j->core);
    free(j->rec);
    /* with no device there is nothing to destroy and nothing still executing:
       the memset below forgets the handles, which is what `lost` does too */
    memset(j, 0, sizeof *j);
}


/* ---- the byte dump ---------------------------------------------------- */

/* Give a dump's staging back. Safe whenever the copy into it is not in flight,
   which every caller proves a different way: `dump_step` is past this slot's
   own fence, and `down` is behind the seam's vkDeviceWaitIdle. */
static void dump_free(const TAGPU_VKPASS* d, struct TAGPU_VKRJOB* g)
{
    if (g->dumpMap && g->dumpMem) vkUnmapMemory(d->dev, g->dumpMem);
    if (g->dumpBuf) vkDestroyBuffer(d->dev, g->dumpBuf, NULL);
    if (g->dumpMem) vkFreeMemory(d->dev, g->dumpMem, NULL);
    g->dumpMap = NULL; g->dumpBuf = VK_NULL_HANDLE; g->dumpMem = VK_NULL_HANDLE;
    g->dumpBytes = 0; g->dumpW = g->dumpH = 0;
}

/* The byte dump, from `step` and before the slice: collect a copy this slot
   owes us, or record one.

   THIS DUMP HAS NOTHING TO BE COMPARED AGAINST WITHIN A RUN: nothing else in
   the process writes a second copy. It is compared against the torch
   reference of the source it carries (tools/tascene restorediff and its
   siblings) or against a file kept from another build. A mirrored restore
   shows as every cell's rows reversed and a dropped batch as whole cells of
   alpha 0.

   AND IT DOES NOT BLOCK THE DEVICE. The copy is recorded into this frame's
   command buffer and read at THIS SLOT'S NEXT step, which is the one instant
   the seam's fence has proved the submit carrying it completed -- the same
   argument the retire already makes, so it costs no new reasoning.

   RE-ARMED ON THE PAINT COUNT, not on a serial, because a GAF atlas's job is
   a lazy queue that keeps painting: a dump is owed again whenever the picture
   has moved since the last one, and a settled scene rewrites nothing. */
static int dump_step(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                     struct TAGPU_VKRJOB* g)
{
    char name[64];
    char b[220];
    int painted;

    if (!g->core || !g->core->used) return 0;

    /* THE COLLECTION FIRST, and under this slot's own fence. */
    if (g->dumpState == 1 && slot == g->dumpSlot) {
        FILE* f;
        /* THE DESTINATION'S OWN BYTES, NOT THE WHOLE BUFFER. The source rides
           in the same allocation after it, and writing `dumpBytes` here would
           put it on the end of the destination file, so every pair would
           differ on SIZE. */
        size_t n = (size_t)(g->dumpBytes - g->dumpSrcBytes);
        /* A CHAIN DUMPS AS `.mips`, LEVEL 0 ALONE AS `.rgba`, and the name is
           what tells them apart, so two builds' dumps of the same case compare
           with one `cmp` of two whole files. */
        _snprintf(name, sizeof name, "tagpu_restore_%s_vk.%s", g->tag,
                  g->chainN > 0 ? "mips" : "rgba");
        name[sizeof name - 1] = 0;
        f = g->dumpMap ? fopen(name, "wb") : NULL;
        if (f) {
            size_t w = fwrite(g->dumpMap, 1, n, f);
            fclose(f);
            _snprintf(b, sizeof b, LANE ": %s: restored atlas dumped to %s (%dx%d RGBA"
                      "%s, %u bytes, %d frames)%s", g->tag, name, g->dumpW, g->dumpH,
                      g->chainN > 0 ? " + its mip chain" : "",
                      (unsigned)n, g->dumpPainted, w == n ? "" : " - SHORT WRITE");
        } else {
            _snprintf(b, sizeof b, LANE ": %s: %s would not open - nothing written",
                      g->tag, name);
        }
        b[sizeof b - 1] = 0;
        rlog(b);
        /* THE ENTRY LIST AND THE PALETTE, which make the pair self-describing:
           a `# atlas W H` line and a `# source W H` one, then a line per painted frame, `x y w h key
           wrap border padR padB` at its destination cell -- a neighbourhood's
           followed by `ax ay edge` and its eight neighbours' origins, `x y`
           each -- and the job's palette snapshot as 256 x RGBA: everything an
           offline run of the model needs to restore the same cells from the
           source beside them and to check the ring OUT paints
           (tools/restore-dumpcheck.py). */
        {
            char sn[64];
            FILE* sf;
            int i;
            _snprintf(sn, sizeof sn, "tagpu_restore_%s_vk.idx", g->tag);
            sn[sizeof sn - 1] = 0;
            if (g->recOn && (sf = fopen(sn, "wb")) != NULL) {
                fprintf(sf, "# atlas %d %d\n", g->dumpW, g->dumpH);
                fprintf(sf, "# source %d %d\n", g->srcW, g->srcH);
                for (i = 0; i < g->nrec; i++) {
                    const TAGPU_RGLSL_FRAME* r = &g->rec[i].f;
                    int k;
                    fprintf(sf, "%d %d %d %d %d %d %d %d %d", r->dx, r->dy, r->w, r->h,
                            r->key, r->wrap ? 1 : 0, r->border, r->padR, r->padB);
                    if (g->rec[i].nb) {
                        fprintf(sf, " %d %d %d", r->ax, r->ay, g->rec[i].edge);
                        for (k = 0; k < 8; k++)
                            fprintf(sf, " %u %u", g->rec[i].nbo[k] & 0xFFFFu, g->rec[i].nbo[k] >> 16);
                    }
                    fputc('\n', sf);
                }
                fclose(sf);
            }
            _snprintf(sn, sizeof sn, "tagpu_restore_%s_vk.pal", g->tag);
            sn[sizeof sn - 1] = 0;
            if (g->palMap && (sf = fopen(sn, "wb")) != NULL) {
                fwrite(g->palMap, 1, 256 * 4, sf);
                fclose(sf);
            }
        }
        if (g->dumpSrcBytes && g->dumpMap) {
            char sn[64];
            FILE* sf;
            _snprintf(sn, sizeof sn, "tagpu_restore_%s_vk.%s", g->tag,
                      g->srcBase ? "base" : "r8");
            sn[sizeof sn - 1] = 0;
            sf = fopen(sn, "wb");
            if (sf) {
                fwrite(g->dumpMap + (size_t)g->dumpSrcOff, 1, (size_t)g->dumpSrcBytes, sf);
                fclose(sf);
            }
        }
        g->dumpState = 2;
        dump_free(d, g);
        return 1;                          /* one action a frame, and it is done */
    }
    painted = g->core->tframes;
    if (g->dumpState == 2 && painted != g->dumpPainted) g->dumpState = 0;
    if (g->dumpState != 0) return 0;
    if (GetFileAttributesA("tagpu_restoredump.on") == INVALID_FILE_ATTRIBUTES) return 0;
    /* THE PICTURE HAS TO BE FINISHED, and it has to exist. `qn`/`inflight` are
       the core's own "every frame added is painted"; taking the dump before
       that would compare a slice count rather than two restorers. A job that
       FAILED is not dumped at all: what it painted is a fragment, and calling
       that the lane's answer is the kind of measurement that reads as a result.
       One painted frame is also what proves the destination is in
       SHADER_READ_ONLY -- an OUT has run and left it there, which is the
       layout the copy borrows and gives back. */
    if (g->core->failed || g->core->qn != 0 || g->core->inflight || g->feeding) return 0;
    if (painted < 1 || !g->dstImg || g->dstW <= 0 || g->dstH <= 0) return 0;

    g->dumpW = g->dstW; g->dumpH = g->dstH;
    g->dumpBytes = (VkDeviceSize)g->dumpW * (VkDeviceSize)g->dumpH * 4;
    /* THE WHOLE CHAIN WHEN THERE IS ONE, in tagpu_gaf.h's layout: level after
       level, end to end, which is what `tagpu_gaf_mip_off` describes. A chain
       is square by construction (job_chain refuses an odd level), so that
       arithmetic applies as it stands. */
    if (g->chainN > 0)
        g->dumpBytes = (VkDeviceSize)tagpu_gaf_mip_chain(g->chainDim, g->chainN);
    /* AND THE SOURCE AFTER IT, in the same buffer and the same submission, so
       the two halves of the pair are read at the same instant rather than a
       frame apart. One byte a texel from an R8 source, four from a base. */
    g->dumpSrcOff = g->dumpBytes;
    g->dumpSrcBytes = (g->srcImg && g->srcW > 0 && g->srcH > 0)
                      ? (VkDeviceSize)g->srcW * (VkDeviceSize)g->srcH * (g->srcBase ? 4u : 1u)
                      : 0;
    g->dumpBytes += g->dumpSrcBytes;
    if (!mk_buffer(d, g->dumpBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &g->dumpBuf, &g->dumpMem, &g->dumpMap)) {
        _snprintf(b, sizeof b, LANE ": %s: no host-visible memory for a %u-byte restore"
                  " dump", g->tag, (unsigned)g->dumpBytes);
        b[sizeof b - 1] = 0;
        rlog(b);
        dump_free(d, g);
        g->dumpState = 2;                  /* do not ask again every frame     */
        g->dumpPainted = painted;
        return 1;
    }
    {
        VkBufferImageCopy rg[1 + TAGPU_VK_MAXMIP];
        uint32_t nrg = 1;
        VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        memset(rg, 0, sizeof rg);
        rg[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg[0].imageSubresource.layerCount = 1;
        rg[0].imageExtent.width = (uint32_t)g->dumpW;
        rg[0].imageExtent.height = (uint32_t)g->dumpH;
        rg[0].imageExtent.depth = 1;
        /* ONE REGION PER LEVEL, at the offset tagpu_gaf_mip_off gives it. The
           levels are copied in ONE vkCmdCopyImageToBuffer, so there is no
           ordering question between them and no second barrier. */
        for (; g->chainN > 0 && (int)nrg <= g->chainN; nrg++) {
            int L = (int)nrg, dl = g->chainDim >> L;
            rg[nrg].bufferOffset = (VkDeviceSize)tagpu_gaf_mip_off(g->chainDim, L);
            rg[nrg].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rg[nrg].imageSubresource.mipLevel = (uint32_t)L;
            rg[nrg].imageSubresource.layerCount = 1;
            rg[nrg].imageExtent.width = (uint32_t)dl;
            rg[nrg].imageExtent.height = (uint32_t)dl;
            rg[nrg].imageExtent.depth = 1;
        }
        mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image = g->dstImg;
        mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        /* AND THE BARRIER COVERS EVERY LEVEL IT COPIES, not level 0 alone: a
           transition that names one level leaves the others in whatever layout
           they were, and the copy would then read them from the wrong one. */
        mb.subresourceRange.levelCount = (uint32_t)nrg;
        mb.subresourceRange.layerCount = 1;
        /* SHADER_READ_ONLY in and SHADER_READ_ONLY out: OUT and the mip
           reduction leave the image there and the consumer's own draw expects
           it there, so the copy borrows the layout and gives it back. */
        mb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
        vkCmdCopyImageToBuffer(cb, g->dstImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g->dumpBuf, nrg, rg);
        /* THE SOURCE TOO, and with its own transition: it is the consumer's
           image and the consumer left it SHADER_READ_ONLY, exactly as the
           destination. Same submission, so the pair is one instant. */
        if (g->dumpSrcBytes) {
            VkBufferImageCopy sr;
            VkImageMemoryBarrier sb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            memset(&sr, 0, sizeof sr);
            sr.bufferOffset = g->dumpSrcOff;
            sr.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            sr.imageSubresource.layerCount = 1;
            sr.imageExtent.width = (uint32_t)g->srcW;
            sr.imageExtent.height = (uint32_t)g->srcH;
            sr.imageExtent.depth = 1;
            sb.srcQueueFamilyIndex = sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            sb.image = g->srcImg;
            sb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            sb.subresourceRange.levelCount = 1;
            sb.subresourceRange.layerCount = 1;
            sb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &sb);
            vkCmdCopyImageToBuffer(cb, g->srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   g->dumpBuf, 1, &sr);
            sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            sb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            sb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &sb);
        }
        mb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &mb);
        host_ready(cb);
    }
    s_rec = 1;
    g->dumpSlot = slot;
    g->dumpState = 1;
    g->dumpPainted = painted;
    _snprintf(b, sizeof b, LANE ": %s: restore dump of %dx%d recorded on slot %u -"
              " written at this slot's next frame", g->tag, g->dumpW, g->dumpH,
              (unsigned)slot);
    b[sizeof b - 1] = 0;
    rlog(b);
    return 1;
}

/* ONE REDUCTION OF ONE JOB'S CHAIN, levels 1..chainN, each from the level
   above it. The descriptor sets were written once when the consumer
   registered the chain; the level's size goes in push constants.

   EACH LEVEL IS TAKEN FROM UNDEFINED, which is not a shortcut but what makes a
   freshly created twin legal to reduce into: a Vulkan image's levels begin
   UNDEFINED, and the dispatch writes every texel of the level, so nothing of
   what was there is wanted. It is left SHADER_READ_ONLY, the layout the
   consumer's descriptor names for the whole chain -- and the transition out
   is also what orders level L's write before level L + 1's read. */
static void chain_step(VkCommandBuffer cb, struct TAGPU_VKRJOB* g)
{
    int L, painted;
    if (!g->core || !g->core->used || g->chainN <= 0 || !s_pipeMip) return;
    painted = g->core->tframes;      /* what `job_painted` publishes */
    /* NOTHING PAINTED MEANS NOTHING TO REDUCE, AND THAT INCLUDES THE FIRST
       PASS. The consumer cannot sample the chain until `job_painted` moves --
       the unit pass sets `s_arHave` only on `painted > 0` -- and level 0 is
       as UNDEFINED as the rest until `dst_ready` transitions it, which happens
       on the OUT path; reading it through a SHADER_READ_ONLY descriptor before
       then is a layout mismatch. `chainDone` is what makes a reduction run
       when the paint count has NOT moved but the levels are stale. */
    if (painted <= 0) return;
    if (g->chainDone && painted == g->chainPainted) return;
    s_rec = 1;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipeMip);
    for (L = 1; L <= g->chainN; L++) {
        VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        PUSH pc;
        int dst = g->chainDim >> L;
        mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image = g->dstImg;
        mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mb.subresourceRange.baseMipLevel = (uint32_t)L;
        mb.subresourceRange.levelCount = 1;
        mb.subresourceRange.layerCount = 1;
        /* in: whatever read or copied this level before (the consumer's
           sample, a dump) is done before it is overwritten */
        mb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        mb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        mb.srcAccessMask = 0;
        mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
        memset(&pc, 0, sizeof pc);
        pc.dstW = dst; pc.dstH = dst;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_ploMip, 0, 1,
                                &g->chainSet[L - 1], 0, NULL);
        vkCmdPushConstants(cb, s_ploMip, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, (uint32_t)((dst + 7) / 8), (uint32_t)((dst + 7) / 8), 1);
        mb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        mb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &mb);
    }
    g->chainPainted = painted;
    g->chainDone = 1;
}

/* ONE SLICE. The retire's bit for this slot clears FIRST and UNCONDITIONALLY,
   before any early return -- tagpu_vk_terr.c's rule, and the reason for it is
   that a bit which clears only on the paths that dispatch would stall the
   retire for ever on a lane that is paused. */
static void probe_step(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

static void step_impl(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    int i;
    if (!d || !d->dev) return;
    retire_slot_done(d, slot);
    if (s_state != ST_READY) return;
    s_d = d; s_cb = cb; s_slot = slot < TAGPU_VK_SLOTS ? slot : 0;
    s_sliceOpen = 0;
    /* THE SELF-TEST BEFORE THE SLICE, for the dump's reason below: a readback
       recorded here sees the probe as the previous frame left it. It builds
       the shared resources itself, so it runs before the `s_built` test. */
    probe_step(d, cb, slot);
    if (!s_built || s_state != ST_READY) { s_cb = VK_NULL_HANDLE; return; }
    /* THE ORACLE BEFORE THE SLICE: nothing has been recorded into `cb` yet, so
       a copy recorded here sees the destination as the previous frame left it
       -- which is the state the `idle` gate has just called finished. One job
       acts per frame; the dump is 16 to 46 MB and a settled scene does none of
       it. */
    for (i = 0; i < TAGPU_R_MAXJOBS; i++)
        if (s_vjob[i].core && dump_step(d, cb, slot, &s_vjob[i])) break;
    tagpu_rcore_step(&s_sched);
    /* AND THE MIP CHAIN AFTER THE SLICE, ONCE -- not once per batch: the
       levels have to be right before the frame that samples them, which is
       this one, and a reduction per batch would be work per batch for a
       picture that is only finished at the end of the slice. */
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) chain_step(cb, &s_vjob[i]);
    s_cb = VK_NULL_HANDLE;
}

void tagpu_vk_restore_step(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    tagpu_rguard_enter();
    s_rec = 0;
    step_impl(d, cb, slot);
    if (s_rec) tagpu_rguard_work(slot);
    tagpu_rguard_leave();
}

static void probe_forget(void);

void tagpu_vk_restore_lost(void)
{
    probe_forget();
    /* every id died with the device: forget without destroying, and the jobs
       with them -- the core's own `lost` frees the queues and nothing else */
    memset(s_act, 0, sizeof s_act); memset(s_actMem, 0, sizeof s_actMem);
    s_actCap = 0;
    /* BOTH RETIRES FORGOTTEN RATHER THAN FREED -- every handle in them died
       with the device, and a `pending` left standing would have a rebuilt
       device's `retire_slot_done` destroy handles that belong to nothing. */
    memset(&s_ret, 0, sizeof s_ret);
    memset(s_jret, 0, sizeof s_jret);
    /* AND EVERY JOB, forgotten whole: its sets, palette images and dump
       staging died with the device, and the pool its sets came from with
       them. A handle left standing would have a later `job_free` or `down`
       destroy it against the NEW device, a `chainN` would have the next
       reduction bind a set that belongs to nothing, and a `core` would name a
       scheduler slot `tagpu_rcore_lost` clears below -- so `core` is also what
       `down` reads as "an owner still holds this job". The frame records are
       host memory, and go. */
    {
        int i;
        for (i = 0; i < TAGPU_R_MAXJOBS; i++) free(s_vjob[i].rec);
    }
    memset(s_vjob, 0, sizeof s_vjob);
    s_tab = VK_NULL_HANDLE; s_tabMem = VK_NULL_HANDLE;
    s_wbuf = VK_NULL_HANDLE; s_wmem = VK_NULL_HANDLE;
    s_wstage = VK_NULL_HANDLE; s_wstageMem = VK_NULL_HANDLE; s_wstageMap = NULL;
    s_qpool = VK_NULL_HANDLE;
    s_samp = VK_NULL_HANDLE; s_dpool = VK_NULL_HANDLE;
    s_dsl = s_dslMip = VK_NULL_HANDLE;
    s_plo = s_ploMip = VK_NULL_HANDLE;
    s_pipeFill = s_pipeOut = s_pipeMip = VK_NULL_HANDLE;
    memset(s_pipeConv, 0, sizeof s_pipeConv);
    s_actGen++;
    s_built = 0; s_state = ST_UNBUILT;
    memset(&s_dev, 0, sizeof s_dev);
    tagpu_rcore_lost(&s_sched);
}

static void probe_free(const TAGPU_VKPASS* d);
static void prret_kill(const TAGPU_VKPASS* d);

void tagpu_vk_restore_down(const TAGPU_VKPASS* d)
{
    int i;
    if (!d || !d->dev) { tagpu_vk_restore_lost(); return; }
    /* THE PROBE FIRST, and its retire with the others below: the seam's
       vkDeviceWaitIdle is above this call */
    probe_free(d);
    prret_kill(d);
    /* A JOB ITS OWNER DID NOT GIVE BACK (the contract in tagpu_vk_restore.h).
       Its palette images and staging would otherwise outlive the device;
       freed here they go into the retire the flush below empties. */
    for (i = 0; i < TAGPU_R_MAXJOBS; i++)
        if (s_vjob[i].core) {
            char b[160];
            _snprintf(b, sizeof b, LANE ": job '%s' was still held by its owner when "
                      "the restorer went down - freed here", s_vjob[i].tag);
            b[sizeof b - 1] = 0;
            rlog(b);
            tagpu_vk_restore_job_free(d, &s_vjob[i]);
        }
    /* THE TWO RETIRES GO FIRST. A retire outstanding at teardown -- an
       activation grow, or the idle release after 180 quiet slices -- holds two
       activation buffers, up to 150 MB, and `vkDestroyDevice` would run with
       all of it alive. The licence here is the seam's vkDeviceWaitIdle above
       this call, which is stronger than any mask. */
    jret_flush(d);
    if (s_ret.pending) { s_ret.pending = 0; retire_kill(d); }
    /* AND ANY DUMP STAGING STILL STANDING. A copy recorded into a command
       buffer of the device that is going never completes, so the collection is
       owed to nobody; the seam's vkDeviceWaitIdle is what makes the free safe
       rather than a race. */
    for (i = 0; i < TAGPU_R_MAXJOBS; i++) dump_free(d, &s_vjob[i]);
    for (i = 0; i < 2; i++) {
        if (s_act[i]) vkDestroyBuffer(d->dev, s_act[i], NULL);
        if (s_actMem[i]) vkFreeMemory(d->dev, s_actMem[i], NULL);
    }
    if (s_tab) vkDestroyBuffer(d->dev, s_tab, NULL);
    if (s_tabMem) vkFreeMemory(d->dev, s_tabMem, NULL);
    if (s_wbuf) vkDestroyBuffer(d->dev, s_wbuf, NULL);
    if (s_wmem) vkFreeMemory(d->dev, s_wmem, NULL);
    if (s_wstageMap && s_wstageMem) vkUnmapMemory(d->dev, s_wstageMem);
    if (s_wstage) vkDestroyBuffer(d->dev, s_wstage, NULL);
    if (s_wstageMem) vkFreeMemory(d->dev, s_wstageMem, NULL);
    if (s_qpool) vkDestroyQueryPool(d->dev, s_qpool, NULL);
    if (s_pipeFill) vkDestroyPipeline(d->dev, s_pipeFill, NULL);
    if (s_pipeOut) vkDestroyPipeline(d->dev, s_pipeOut, NULL);
    if (s_pipeMip) vkDestroyPipeline(d->dev, s_pipeMip, NULL);
    for (i = 0; i < TAGPU_RM_N * TAGPU_R_MAXLAYERS; i++)
        if (s_pipeConv[i / TAGPU_R_MAXLAYERS][i % TAGPU_R_MAXLAYERS])
            vkDestroyPipeline(d->dev, s_pipeConv[i / TAGPU_R_MAXLAYERS][i % TAGPU_R_MAXLAYERS], NULL);
    if (s_plo) vkDestroyPipelineLayout(d->dev, s_plo, NULL);
    if (s_ploMip) vkDestroyPipelineLayout(d->dev, s_ploMip, NULL);
    if (s_dsl) vkDestroyDescriptorSetLayout(d->dev, s_dsl, NULL);
    if (s_dslMip) vkDestroyDescriptorSetLayout(d->dev, s_dslMip, NULL);
    if (s_dpool) vkDestroyDescriptorPool(d->dev, s_dpool, NULL);
    if (s_samp) vkDestroySampler(d->dev, s_samp, NULL);
    tagpu_vk_restore_lost();
}


/* ============================ THE SELF-TEST ============================
   research/notes/compute-restorer.md D2 and D13; tagpu_restore_ref.h has the
   probe and the reference.

   ARMED BY `up`, WHICH CLOSES THE CORE'S GATE IN THE SAME CALL, so no
   consumer's job can be picked before the verdict: one created in between
   queues and waits, and its destination is never painted by a device that
   has not passed. Then, one step at a time:
     ARMED     build the probe's images and its three jobs (prio -1, which the
               gate lets through), upload the sources, start the CPU worker
     RUNNING   every job has painted and the chain is reduced: record the
               readback into this frame
     READBACK  the slot that took the readback comes round again -- its fence
               has signalled, so the bytes are in
     WAITCPU   the CPU worker has finished: the verdict
   A pass opens the gate. Wrong bytes record the device off (and the notice
   says so) and fail every job. A probe that could not be run at all -- no
   memory, a job refused, a reference that could not be computed -- turns the
   restorer off with no record, since that says nothing about the driver: until
   the lane next comes down and up (a shell/game switch, a swapchain rebuild),
   which runs the self-test again.
   A device that has passed is not tested again in this process, so a
   swapchain rebuild, which takes the restorer down and up, costs nothing. */
enum { PR_NONE, PR_ARMED, PR_RUNNING, PR_READBACK, PR_WAITCPU };
#define PR_DIM      TAGPU_RPROBE_DIM
#define PR_OFF_SRCB 0
#define PR_OFF_SRCR (PR_OFF_SRCB + PR_DIM * PR_DIM * 4)
#define PR_OFF_GOTB (PR_OFF_SRCR + PR_DIM * PR_DIM)
#define PR_NIMG     5
#define PR_NVIEW    (PR_NIMG + TAGPU_RPROBE_MIPS)

static struct {
    int                  state;
    TAGPU_RPROBE*        probe;
    TAGPU_RTEST*         ref;
    /* the base source, the index source, the base job's destination (with
       its chain), the palette job's destination and the neighbourhood job's,
       which reads the base source too */
    VkImage              img[PR_NIMG];
    VkDeviceMemory       mem[PR_NIMG];
    /* each image's level 0, then the base destination's levels 1.. */
    VkImageView          view[PR_NVIEW];
    VkBuffer             buf;                 /* the sources up, the answers back */
    VkDeviceMemory       bufMem;
    unsigned char*       map;
    int                  offR, offN, bytes;
    struct TAGPU_VKRJOB* job[3];
    uint32_t             rbSlot;
} s_pr;
static char s_passedKey[48];

/* THE PROBE'S OBJECTS AFTER THE VERDICT, retired with a job's licence: the
   readback, the uploads and the jobs' sets that name them were recorded into
   frames still in flight. */
static struct {
    VkImage        img[PR_NIMG];
    VkDeviceMemory mem[PR_NIMG];
    VkImageView    view[PR_NVIEW];
    VkBuffer       buf;
    VkDeviceMemory bufMem;
    uint32_t       pending;
} s_prRet;

static void prret_kill(const TAGPU_VKPASS* d)
{
    int i;
    if (d && d->dev) {
        for (i = 0; i < PR_NVIEW; i++) if (s_prRet.view[i]) vkDestroyImageView(d->dev, s_prRet.view[i], NULL);
        for (i = 0; i < PR_NIMG; i++) {
            if (s_prRet.img[i]) vkDestroyImage(d->dev, s_prRet.img[i], NULL);
            if (s_prRet.mem[i]) vkFreeMemory(d->dev, s_prRet.mem[i], NULL);
        }
        if (s_prRet.buf) vkDestroyBuffer(d->dev, s_prRet.buf, NULL);
        if (s_prRet.bufMem) vkFreeMemory(d->dev, s_prRet.bufMem, NULL);
    }
    memset(&s_prRet, 0, sizeof s_prRet);
}

static void prret_slot_done(const TAGPU_VKPASS* d, uint32_t slot)
{
    if (!s_prRet.pending) return;
    s_prRet.pending &= ~(1u << slot);
    if (!s_prRet.pending) prret_kill(d);
}

static void probe_arm(void)
{
    if (s_pr.state != PR_NONE) return;
    if (s_passedKey[0] && !strcmp(s_passedKey, tagpu_rguard_key())) return;
    s_pr.state = PR_ARMED;
    s_sched.gate = 1;
}

/* Everything the probe holds, into the retire (or forgotten, with no
   device). The jobs go through `job_free`, which retires their own objects. */
static void probe_free(const TAGPU_VKPASS* d)
{
    int i, any = 0;
    for (i = 0; i < 3; i++) if (s_pr.job[i]) tagpu_vk_restore_job_free(d, s_pr.job[i]);
    tagpu_rref_test_free(s_pr.ref);
    free(s_pr.probe);
    for (i = 0; i < PR_NIMG; i++) any |= s_pr.img[i] != VK_NULL_HANDLE || s_pr.mem[i] != VK_NULL_HANDLE;
    any |= s_pr.buf != VK_NULL_HANDLE || s_pr.bufMem != VK_NULL_HANDLE;
    if (any && d && d->dev) {
        /* one probe retire at a time; a second within `slots` frames of the
           first is a down and up in between, which drains the device anyway */
        if (s_prRet.pending) { if (vkDeviceWaitIdle) vkDeviceWaitIdle(d->dev); prret_kill(d); }
        if (s_pr.map && s_pr.bufMem) vkUnmapMemory(d->dev, s_pr.bufMem);
        memcpy(s_prRet.img, s_pr.img, sizeof s_prRet.img);
        memcpy(s_prRet.mem, s_pr.mem, sizeof s_prRet.mem);
        memcpy(s_prRet.view, s_pr.view, sizeof s_prRet.view);
        s_prRet.buf = s_pr.buf; s_prRet.bufMem = s_pr.bufMem;
        s_prRet.pending = all_slots(d);
    }
    memset(&s_pr, 0, sizeof s_pr);
}

/* the device died: every handle with it; the worker is host memory and is
   joined and freed */
static void probe_forget(void)
{
    tagpu_rref_test_free(s_pr.ref);
    free(s_pr.probe);
    memset(&s_pr, 0, sizeof s_pr);
    memset(&s_prRet, 0, sizeof s_prRet);
}

static int pr_image(const TAGPU_VKPASS* d, int i, VkFormat fmt, int levels, VkImageUsageFlags use)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    uint32_t type;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = PR_DIM; ici.extent.height = PR_DIM; ici.extent.depth = 1;
    ici.mipLevels = (uint32_t)levels; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = use;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ici, NULL, &s_pr.img[i]) != VK_SUCCESS) { s_pr.img[i] = VK_NULL_HANDLE; return 0; }
    memset(&req, 0, sizeof req);
    vkGetImageMemoryRequirements(d->dev, s_pr.img[i], &req);
    type = mem_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return 0;
    mai.allocationSize = req.size; mai.memoryTypeIndex = type;
    if (vkAllocateMemory(d->dev, &mai, NULL, &s_pr.mem[i]) != VK_SUCCESS) { s_pr.mem[i] = VK_NULL_HANDLE; return 0; }
    return vkBindImageMemory(d->dev, s_pr.img[i], s_pr.mem[i], 0) == VK_SUCCESS;
}

static VkImageView pr_view(const TAGPU_VKPASS* d, VkImage img, VkFormat fmt, int level)
{
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkImageView v = VK_NULL_HANDLE;
    ivi.image = img;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = fmt;
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivi.subresourceRange.baseMipLevel = (uint32_t)level;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d->dev, &ivi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

/* the two sources, from the staging onto the device, before any FILL reads
   them -- the barrier out names the compute stage that does */
static void pr_upload(VkCommandBuffer cb)
{
    VkImageMemoryBarrier mb[2];
    VkBufferImageCopy bc;
    int i;
    memset(mb, 0, sizeof mb);
    for (i = 0; i < 2; i++) {
        mb[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb[i].srcQueueFamilyIndex = mb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb[i].image = s_pr.img[i];
        mb[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mb[i].subresourceRange.levelCount = 1;
        mb[i].subresourceRange.layerCount = 1;
        mb[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        mb[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mb[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, mb);
    for (i = 0; i < 2; i++) {
        memset(&bc, 0, sizeof bc);
        bc.bufferOffset = i == 0 ? PR_OFF_SRCB : PR_OFF_SRCR;
        bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bc.imageSubresource.layerCount = 1;
        bc.imageExtent.width = PR_DIM; bc.imageExtent.height = PR_DIM; bc.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(cb, s_pr.buf, s_pr.img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        mb[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mb[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 2, mb);
    s_rec = 1;
}

static int probe_start(const TAGPU_VKPASS* d, VkCommandBuffer cb)
{
    const VkImageUsageFlags srcUse = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageUsageFlags dstUse = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    static const VkFormat fmt[PR_NIMG] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM,
                                           VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_FORMAT_R8G8B8A8_UNORM };
    /* the neighbourhoods run the terrain's model, whichever that is */
    const int mn = tagpu_rcore_model(TAGPU_RM_TINY) ? TAGPU_RM_TINY : TAGPU_RM_FULL;
    VkImageView attach[TAGPU_RPROBE_MIPS], sample[TAGPU_RPROBE_MIPS];
    int i, L;

    s_pr.probe = (TAGPU_RPROBE*)malloc(sizeof *s_pr.probe);
    if (!s_pr.probe) return 0;
    tagpu_rref_probe(s_pr.probe);
    for (i = 0; i < PR_NIMG; i++) {
        if (!pr_image(d, i, fmt[i], i == 2 ? 1 + TAGPU_RPROBE_MIPS : 1, i < 2 ? srcUse : dstUse)) return 0;
        if (!(s_pr.view[i] = pr_view(d, s_pr.img[i], fmt[i], 0))) return 0;
    }
    for (L = 1; L <= TAGPU_RPROBE_MIPS; L++)
        if (!(s_pr.view[PR_NIMG - 1 + L] = pr_view(d, s_pr.img[2], fmt[2], L))) return 0;
    /* level L - 1 is read and level L written, each through a view of that
       one level (tagpu_vk_restore_job_chain) */
    for (L = 1; L <= TAGPU_RPROBE_MIPS; L++) {
        attach[L - 1] = s_pr.view[PR_NIMG - 1 + L];
        sample[L - 1] = L == 1 ? s_pr.view[2] : s_pr.view[PR_NIMG - 1 + L - 1];
    }

    s_pr.offR = PR_OFF_GOTB + tagpu_rref_chain_bytes();
    s_pr.offN = s_pr.offR + PR_DIM * PR_DIM * 4;
    s_pr.bytes = s_pr.offN + PR_DIM * PR_DIM * 4;
    if (!mk_buffer(d, (VkDeviceSize)s_pr.bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &s_pr.buf, &s_pr.bufMem, &s_pr.map)) return 0;
    memcpy(s_pr.map + PR_OFF_SRCB, s_pr.probe->base, sizeof s_pr.probe->base);
    memcpy(s_pr.map + PR_OFF_SRCR, s_pr.probe->r8, sizeof s_pr.probe->r8);
    pr_upload(cb);

    s_pr.job[0] = tagpu_vk_restore_job_new(d, "probe-base", -1, 1, TAGPU_RM_FULL, 0, s_pr.img[0], s_pr.view[0],
                                           PR_DIM, PR_DIM, 1, s_pr.probe->pal,
                                           s_pr.img[2], s_pr.view[2], PR_DIM, PR_DIM);
    if (!s_pr.job[0]) return 0;
    if (!tagpu_vk_restore_job_chain(d, s_pr.job[0], TAGPU_RPROBE_MIPS, PR_DIM, attach, sample)) return 0;
    s_pr.job[1] = tagpu_vk_restore_job_new(d, "probe-pal", -1, 1, TAGPU_RM_FULL, 0, s_pr.img[1], s_pr.view[1],
                                           PR_DIM, PR_DIM, 0, s_pr.probe->pal,
                                           s_pr.img[3], s_pr.view[3], PR_DIM, PR_DIM);
    if (!s_pr.job[1]) return 0;
    s_pr.job[2] = tagpu_vk_restore_job_new(d, "probe-nbhd", -1, 1, mn, 0, s_pr.img[0], s_pr.view[0],
                                           PR_DIM, PR_DIM, 1, s_pr.probe->pal,
                                           s_pr.img[4], s_pr.view[4], PR_DIM, PR_DIM);
    if (!s_pr.job[2]) return 0;
    if (tagpu_vk_restore_job_add(s_pr.job[0], s_pr.probe->fb, TAGPU_RPROBE_NF) != TAGPU_RPROBE_NF ||
        tagpu_vk_restore_job_add(s_pr.job[1], s_pr.probe->fr, TAGPU_RPROBE_NF) != TAGPU_RPROBE_NF ||
        tagpu_vk_restore_job_add_nbhd(s_pr.job[2], s_pr.probe->fn, TAGPU_RPROBE_NBN) != TAGPU_RPROBE_NBN)
        return 0;
    s_pr.ref = tagpu_rref_test_start(tagpu_rcore_model(TAGPU_RM_FULL), tagpu_rcore_model(mn), s_pr.probe);
    return s_pr.ref != NULL;
}

/* the three destinations, every level of the chain, into the staging after
   the sources; borrowed from SHADER_READ_ONLY, where OUT and MIP leave them,
   and given back */
static void pr_readback(VkCommandBuffer cb)
{
    VkImageMemoryBarrier mb[3];
    VkBufferImageCopy rg[1 + TAGPU_RPROBE_MIPS];
    int i, L, off = PR_OFF_GOTB;
    memset(mb, 0, sizeof mb);
    for (i = 0; i < 3; i++) {
        mb[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb[i].srcQueueFamilyIndex = mb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb[i].image = s_pr.img[2 + i];
        mb[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mb[i].subresourceRange.levelCount = i == 0 ? 1 + TAGPU_RPROBE_MIPS : 1;
        mb[i].subresourceRange.layerCount = 1;
        mb[i].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        mb[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 3, mb);
    memset(rg, 0, sizeof rg);
    for (L = 0; L <= TAGPU_RPROBE_MIPS; L++) {
        rg[L].bufferOffset = (VkDeviceSize)off;
        rg[L].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rg[L].imageSubresource.mipLevel = (uint32_t)L;
        rg[L].imageSubresource.layerCount = 1;
        rg[L].imageExtent.width = (uint32_t)(PR_DIM >> L);
        rg[L].imageExtent.height = (uint32_t)(PR_DIM >> L);
        rg[L].imageExtent.depth = 1;
        off += (PR_DIM >> L) * (PR_DIM >> L) * 4;
    }
    vkCmdCopyImageToBuffer(cb, s_pr.img[2], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_pr.buf,
                           1 + TAGPU_RPROBE_MIPS, rg);
    rg[0].bufferOffset = (VkDeviceSize)s_pr.offR;
    vkCmdCopyImageToBuffer(cb, s_pr.img[3], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_pr.buf, 1, rg);
    rg[0].bufferOffset = (VkDeviceSize)s_pr.offN;
    vkCmdCopyImageToBuffer(cb, s_pr.img[4], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_pr.buf, 1, rg);
    host_ready(cb);
    for (i = 0; i < 3; i++) {
        mb[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        mb[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mb[i].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mb[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 3, mb);
    s_rec = 1;
}

/* `v`: 1 pass, 0 wrong bytes, -1 could not be run */
static void probe_verdict(const TAGPU_VKPASS* d, int v, const char* why)
{
    char b[400];
    char key[48];
    lstrcpynA(key, tagpu_rguard_key(), sizeof key);
    probe_free(d);
    if (v == 1) {
        _snprintf(b, sizeof b, LANE ": self-test passed on %s: %s", key, why);
        b[sizeof b - 1] = 0;
        rlog(b);
        lstrcpynA(s_passedKey, key, sizeof s_passedKey);
        s_sched.gate = 0;
        return;
    }
    _snprintf(b, sizeof b, LANE ": self-test %s on %s: %s - the restorer is off%s", v == 0 ? "FAILED" : "could not be run",
              key, why, v == 0 ? " for this driver" : " until the lane next comes up");
    b[sizeof b - 1] = 0;
    rlog(b);
    if (v == 0) tagpu_rguard_turn_off(TAGPU_RG_SELFTEST);
    tagpu_rcore_fail_all(&s_sched);
    s_sched.gate = 0;
    refuse();
}

static void probe_step(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot)
{
    char why[240];
    switch (s_pr.state) {
    case PR_ARMED:
        if (!probe_start(d, cb)) { probe_verdict(d, -1, "the probe could not be built"); return; }
        s_pr.state = PR_RUNNING;
        rlog(LANE ": self-test started: 4 synthetic frames and 9 neighbourhoods on the device,"
                  " the reference on a CPU worker");
        return;
    case PR_RUNNING: {
        struct TAGPU_VKRJOB* a = s_pr.job[0];
        struct TAGPU_VKRJOB* r = s_pr.job[1];
        struct TAGPU_VKRJOB* n = s_pr.job[2];
        if (tagpu_vk_restore_job_failed(a) || tagpu_vk_restore_job_failed(r) || tagpu_vk_restore_job_failed(n)) {
            probe_verdict(d, -1, "a probe job failed");
            return;
        }
        if (!tagpu_vk_restore_job_idle(a) || !tagpu_vk_restore_job_idle(r) || !tagpu_vk_restore_job_idle(n)) return;
        if (!a->chainDone || a->chainPainted != a->core->tframes) return;
        pr_readback(cb);
        s_pr.rbSlot = slot;
        s_pr.state = PR_READBACK;
        return;
    }
    case PR_READBACK:
        /* this slot's fence has signalled since the readback was recorded
           into it: that is what `step` being called for it means */
        if (slot != s_pr.rbSlot) return;
        s_pr.state = PR_WAITCPU;
        /* fall through: the worker has usually finished already */
    case PR_WAITCPU: {
        int v;
        if (!tagpu_rref_test_done(s_pr.ref)) return;
        /* the fault lever spoils one texel of the base job's keyed frame,
           away from its keys */
        if (tagpu_rguard_fault("probe")) s_pr.map[PR_OFF_GOTB + (5 * PR_DIM + 5) * 4] ^= 0x40;
        v = tagpu_rref_test_check(s_pr.ref, s_pr.map + PR_OFF_GOTB, s_pr.map + s_pr.offR,
                                  s_pr.map + s_pr.offN, why, sizeof why);
        probe_verdict(d, v, why);
        return;
    }
    default:
        return;
    }
}
