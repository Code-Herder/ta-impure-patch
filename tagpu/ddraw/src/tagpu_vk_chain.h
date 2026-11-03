#ifndef TAGPU_VK_CHAIN_H
#define TAGPU_VK_CHAIN_H
/* The mip chains' reducer on the device: one compute pipeline that writes a source atlas's
   per-tile chains (inc/tagpu_chain.h) from its level 0, on the seam's one queue, inside a world
   pass's `prepare`. Implementation: tagpu_vk_chain.c; the shader: src/tagpu_chain_comp.h.

   THE CONSUMER OWNS EVERYTHING BUT THE PIPELINE: the chain images (5 levels, RGBA8, SAMPLED |
   STORAGE | TRANSFER_SRC | TRANSFER_DST, GENERAL for life), the descriptor sets it allocates from
   its own pool with `tagpu_vk_chain_dsl()`, and the counter buffer the sets name at binding 6.
   The set's bindings: 0 the source atlas (a COMBINED_IMAGE_SAMPLER in SHADER_READ_ONLY_OPTIMAL),
   1..5 one storage view per chain level (GENERAL), 6 a storage buffer of 16 bytes -- four uint
   counters, the check mode's two pairs (tagpu_chain_comp.h) -- and 7 a storage buffer of the
   table form's source rects, two uints a cell (any valid buffer in the grid form, which does not
   read it).

   RENDER THREAD ONLY, like every pass, and it reads no engine state. Compiled a second time for
   the wiki's stage (tagpu_scene.h), each instance with its own pipeline. */

#include "tagpu_vk_pass.h"

/* 1 when the device reduces: the seam's queue family has COMPUTE and R8G8B8A8_UNORM is a storage
   image (the specification requires it; asked anyway). Builds the pipeline on the first call; a 0
   is said once, and every chain consumer then draws its atlases at level 0. */
int  tagpu_vk_chain_up(const TAGPU_VKPASS* d);
VkDescriptorSetLayout tagpu_vk_chain_dsl(void);

/* One reduction. Range mode (`cells` NULL): cells first .. first+count-1, row-major over `cols`,
   in one dispatch. List mode: `count` cells of `cells`, each x | y << 16, in pushes of 24.
   `pitch`/`border` place a cell's source tile in the atlas; `table` 1 takes each cell's source
   rect from binding 7 instead (range mode only); `check` 0 writes, 1 or 2 counts mismatches into
   that counter pair instead. Between `begin` and `end`. */
typedef struct {
    int first, count;
    const uint32_t* cells;
    int cols, pitch, border, check, table;
} TAGPU_CHAINJOB;
void tagpu_vk_chain_record(VkCommandBuffer cb, VkDescriptorSet set, const TAGPU_CHAINJOB* j);

/* THE ORDERING AROUND A FRAME'S REDUCTIONS (the argument is above their definitions):
   `begin` before the first, with the chain images still UNDEFINED among `fresh`, which it moves
   to GENERAL; `clear` for a chain to be emptied, between the two; `end` after the last. */
void tagpu_vk_chain_begin(VkCommandBuffer cb, const VkImage* fresh, int nFresh);
void tagpu_vk_chain_clear(VkCommandBuffer cb, VkImage img);
void tagpu_vk_chain_end(VkCommandBuffer cb);

/* THE ORACLE'S ORDERING (tagpu_chaincheck.on): `check_begin` after the `vkCmdFillBuffer` that
   zeroes the counters and before the check dispatches and the copies out of a chain;
   `check_end` after them, making both visible to the host at the slot's next `prepare`. */
void tagpu_vk_chain_check_begin(VkCommandBuffer cb);
void tagpu_vk_chain_check_end(VkCommandBuffer cb);

/* Give the pipeline back: the seam calls it after every consumer's `_down`, behind its
   vkDeviceWaitIdle. With no device it forgets the handles instead: they died with it. */
void tagpu_vk_chain_down(const TAGPU_VKPASS* d);

/* 1 while `tagpu_chaincheck.on` exists, re-read every 120 frames of `frame`. Defined by the seam
   (tagpu_vk.c) beside the A/B levers, so the wiki's instances get tagpu_wikiworld.c's inert 0
   (tagpu_scene.h) and the oracle's lines are the game's world only. */
int  tagpu_vk_chaincheck_armed(unsigned frame);

#endif
