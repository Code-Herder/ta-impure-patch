/* tagpu_vk_chain.c -- the mip chains' reducer on the device. Contract: tagpu_vk_chain.h; the
   format and its C reference: inc/tagpu_chain.h; the shader: src/tagpu_chain_comp.h.

   TWO INSTANCES: compiled for the game's world and, under TAGPU_SCENE_WIKI, for the wiki's stage,
   each with its own pipeline (tagpu_scene.h renames this file's entry points), so the two
   teardowns are independent.

   IT KNOWS NOTHING ABOUT A WINDOW and READS NO ENGINE STATE: a consumer hands it a command buffer,
   a descriptor set and the cells to reduce. */

#include "tagpu_scene.h"            /* first: the wiki instance's names */
#include "tagpu_vk_chain.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "spirv/tagpu_chain_comp.spv.h"

#define IFNS(X) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties)

#define DFNS(X) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

/* the push constants, field for field TAGPU_CHAIN_COMMON_CS's: 128 bytes */
typedef struct {
    int32_t  first, count, list, cols, pitch, border, check, spare;
    uint32_t cell[24];
} PUSH;
typedef char chain_push_is_128[sizeof(PUSH) == 128 ? 1 : -1];

#define LIST_MAX 24                         /* PUSH.cell                          */
#define ROW_WGS  1024                       /* workgroups along x: the shader's wg */
#define MAX_WGS_Y 65535u                    /* maxComputeWorkGroupCount's floor   */

enum { ST_UNBUILT = 0, ST_READY = 1, ST_REFUSED = 2 };
static int s_state;
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout      s_plo;
static VkPipeline            s_pipe;

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

static void kill_all(const TAGPU_VKPASS* d)
{
    if (s_pipe) { vkDestroyPipeline(d->dev, s_pipe, NULL); s_pipe = VK_NULL_HANDLE; }
    if (s_plo)  { vkDestroyPipelineLayout(d->dev, s_plo, NULL); s_plo = VK_NULL_HANDLE; }
    if (s_dsl)  { vkDestroyDescriptorSetLayout(d->dev, s_dsl, NULL); s_dsl = VK_NULL_HANDLE; }
}

/* the reason the device cannot reduce, or NULL when it can and the pipeline is built */
static const char* build(const TAGPU_VKPASS* d)
{
    VkQueueFamilyProperties qp[16];
    uint32_t nq = 16, i;
    VkFormatProperties fp;
    VkDescriptorSetLayoutBinding b[7];
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange pr;
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r;

    if (!resolve(d)) return "an entry point is missing";
    memset(qp, 0, sizeof qp);
    vkGetPhysicalDeviceQueueFamilyProperties(d->pd, &nq, qp);
    if (nq > 16) nq = 16;
    if (d->qfam >= nq || !(qp[d->qfam].queueFlags & VK_QUEUE_COMPUTE_BIT))
        return "the seam's queue family has no COMPUTE";
    memset(&fp, 0, sizeof fp);
    vkGetPhysicalDeviceFormatProperties(d->pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return "R8G8B8A8_UNORM is not a storage image here, which the specification requires";

    memset(b, 0, sizeof b);
    for (i = 0; i < 7; i++) {
        b[i].binding = i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                            : i == 6 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                     : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    dli.bindingCount = 7; dli.pBindings = b;
    if (vkCreateDescriptorSetLayout(d->dev, &dli, NULL, &s_dsl) != VK_SUCCESS)
        return "the descriptor set layout was refused";
    memset(&pr, 0, sizeof pr);
    pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pr.size = sizeof(PUSH);
    pli.setLayoutCount = 1; pli.pSetLayouts = &s_dsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(d->dev, &pli, NULL, &s_plo) != VK_SUCCESS)
        return "the pipeline layout was refused";
    smi.codeSize = sizeof tagpu_spv_tagpu_chain_comp_REDUCE_CS;
    smi.pCode = tagpu_spv_tagpu_chain_comp_REDUCE_CS;
    if (vkCreateShaderModule(d->dev, &smi, NULL, &m) != VK_SUCCESS)
        return "the shader module was refused";
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = s_plo;
    r = vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, NULL, &s_pipe);
    vkDestroyShaderModule(d->dev, m, NULL);
    if (r != VK_SUCCESS) { s_pipe = VK_NULL_HANDLE; return "the compute pipeline was refused"; }
    return NULL;
}

int tagpu_vk_chain_up(const TAGPU_VKPASS* d)
{
    const char* why;
    if (s_state == ST_READY) return 1;
    if (s_state == ST_REFUSED) return 0;
    why = build(d);
    if (why) {
        plog(d, "chains off: %s - the far zoom draws the atlases at level 0", why);
        if (vkDestroyPipeline) kill_all(d);
        s_state = ST_REFUSED;
        return 0;
    }
    s_state = ST_READY;
    return 1;
}

VkDescriptorSetLayout tagpu_vk_chain_dsl(void)
{
    return s_dsl;
}

void tagpu_vk_chain_record(VkCommandBuffer cb, VkDescriptorSet set, const TAGPU_CHAINJOB* j)
{
    PUSH pc;
    int done = 0;
    if (s_state != ST_READY || j->count < 1) return;
    memset(&pc, 0, sizeof pc);
    pc.cols = j->cols; pc.pitch = j->pitch; pc.border = j->border; pc.check = j->check;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_plo, 0, 1, &set, 0, NULL);
    if (!j->cells) {
        /* ONE WORKGROUP A CELL, ROW_WGS along x: the shader's `wg` is y * 1024 + x and returns
           whole past `count`. The y count is within the floor every device guarantees. */
        uint32_t gy = ((uint32_t)j->count + ROW_WGS - 1) / ROW_WGS;
        if (gy > MAX_WGS_Y) return;
        pc.first = j->first; pc.count = j->count;
        vkCmdPushConstants(cb, s_plo, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, j->count < ROW_WGS ? (uint32_t)j->count : ROW_WGS, gy, 1);
        return;
    }
    pc.list = 1;
    while (done < j->count) {
        int n = j->count - done < LIST_MAX ? j->count - done : LIST_MAX;
        memcpy(pc.cell, j->cells + done, (size_t)n * sizeof pc.cell[0]);
        pc.count = n;
        vkCmdPushConstants(cb, s_plo, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, (uint32_t)n, 1, 1);
        done += n;
    }
}

/* ---- THE GPU ORDERING -----------------------------------------------------------------------
   Everything here is on the seam's one queue, in the frame's command buffer, before
   vkCmdBeginRenderPass, inside a world pass's `prepare`: after the fog upload, and before the
   restorer's slice, which the seam records last of `prepare` (tagpu_vk.c).

   `begin`: ONE GLOBAL MEMORY BARRIER, src TRANSFER | COMPUTE | FRAGMENT (TRANSFER_WRITE |
   SHADER_WRITE) -> dst TRANSFER | COMPUTE (TRANSFER_WRITE | SHADER_READ | SHADER_WRITE), plus an
   image barrier UNDEFINED -> GENERAL for each chain image that has never been laid out. A
   barrier's first scope is every command submitted earlier on the queue -- not only earlier in
   this command buffer (the restorer's own argument, tagpu_vk_restore.c above its slice barrier)
   -- so this one orders, before any reduction or clear of this frame:
     * the base atlas's banded upload, whose submits precede this command buffer (RAW on the
       source);
     * every earlier OUT of the restorer into the restored atlas (RAW on the source);
     * every earlier frame's draw that sampled a chain (WAR before the reduction rewrites it);
     * every earlier reduction, clear and oracle copy of a chain (WAW, and WAR for the copy).

   A CLEAR sits between barriers of its own: COMPUTE | FRAGMENT | TRANSFER -> TRANSFER before it,
   and TRANSFER_WRITE -> COMPUTE (SHADER_READ | SHADER_WRITE) and FRAGMENT (SHADER_READ) after it.
   It runs in GENERAL, which vkCmdClearColorImage allows.

   NO BARRIER BETWEEN ONE FRAME'S DISPATCHES: they write disjoint cells (a workgroup writes only
   its own cell's block, tagpu_chain_comp.h) and read only the atlases' level 0, which nothing
   writes between them.

   `end`: COMPUTE (SHADER_WRITE) -> FRAGMENT | COMPUTE (SHADER_READ | SHADER_WRITE).
     * It makes the chains visible to this frame's draw.
     * Its COMPUTE destination is also the EXECUTION DEPENDENCY THAT ORDERS OUR READS OF THE
       RESTORED ATLAS BEFORE THE RESTORER'S OUT WRITES LATER IN THIS FRAME (WAR). Stated here
       rather than left to the restorer's own transition into GENERAL (`dst_layout`), whose first
       scope does include COMPUTE.
     * Its FRAGMENT destination chains into a later re-upload of a base atlas
       (tagpu_vk_stage.c `barrier_in`, whose source stage is FRAGMENT): that is what orders our
       COMPUTE reads of the base atlas before the upload's writes (WAR). A compute stage is not
       "logically earlier" than FRAGMENT, so without this chain that barrier would not cover them.

   THE CHAIN IMAGES STAY GENERAL FOR LIFE, written as storage, sampled as GENERAL, copied out as
   GENERAL: one layout and nothing to track. The atlases' level 0 keeps its layouts, which are the
   upload's and the restorer's. */

static void mem_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags srcAcc,
                        VkPipelineStageFlags dst, VkAccessFlags dstAcc)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = srcAcc;
    mb.dstAccessMask = dstAcc;
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

void tagpu_vk_chain_begin(VkCommandBuffer cb, const VkImage* fresh, int nFresh)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkImageMemoryBarrier ib[4];
    int i, n = 0;
    if (s_state != ST_READY) return;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                       VK_ACCESS_SHADER_WRITE_BIT;
    memset(ib, 0, sizeof ib);
    for (i = 0; i < nFresh && n < 4; i++) {
        if (!fresh[i]) continue;
        ib[n].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib[n].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib[n].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[n].srcQueueFamilyIndex = ib[n].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib[n].image = fresh[i];
        ib[n].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ib[n].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        ib[n].subresourceRange.layerCount = 1;
        ib[n].srcAccessMask = 0;
        ib[n].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                              VK_ACCESS_SHADER_WRITE_BIT;
        n++;
    }
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, NULL, (uint32_t)n, ib);
}

void tagpu_vk_chain_clear(VkCommandBuffer cb, VkImage img)
{
    VkClearColorValue cc;
    VkImageSubresourceRange rg;
    if (s_state != ST_READY || !img) return;
    memset(&cc, 0, sizeof cc);
    memset(&rg, 0, sizeof rg);
    rg.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rg.levelCount = VK_REMAINING_MIP_LEVELS;
    rg.layerCount = 1;
    mem_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rg);
    mem_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void tagpu_vk_chain_end(VkCommandBuffer cb)
{
    if (s_state != ST_READY) return;
    mem_barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

/* THE ORACLE'S ORDERING. `check_begin` follows the counters' vkCmdFillBuffer and this frame's
   reductions (after `end`): the fill and every chain write become visible to the check dispatches
   (COMPUTE) and the copies out of the chain (TRANSFER). `check_end` makes the counters and the
   copies visible to the host, read at the slot's next `prepare` after its fence; its COMPUTE and
   FRAGMENT destinations are `end`'s two execution dependencies again, for the check's own reads of
   the atlases -- before the restorer's OUT later in this frame, and before a later re-upload. */
void tagpu_vk_chain_check_begin(VkCommandBuffer cb)
{
    if (s_state != ST_READY) return;
    mem_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void tagpu_vk_chain_check_end(VkCommandBuffer cb)
{
    if (s_state != ST_READY) return;
    mem_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
}

void tagpu_vk_chain_lost(void)
{
    s_pipe = VK_NULL_HANDLE; s_plo = VK_NULL_HANDLE; s_dsl = VK_NULL_HANDLE;
    s_state = ST_UNBUILT;
}

void tagpu_vk_chain_down(const TAGPU_VKPASS* d)
{
    /* ST_UNBUILT, not ST_REFUSED: the verdict is a fact about the device going away with it, and
       the next one is asked again */
    if (!d || !d->dev) { tagpu_vk_chain_lost(); return; }
    if (vkDestroyPipeline) kill_all(d);
    s_state = ST_UNBUILT;
}
