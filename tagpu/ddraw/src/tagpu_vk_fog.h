#ifndef TAGPU_VK_FOG_H
#define TAGPU_VK_FOG_H
/* tagpu_vk_fog -- the fog source (inc/tagpu_fogsrc.h) as one R32_UINT image a frame slot, which
   every world pass of its scene binds. Contract only; tagpu_vk_fog.c is the module. RENDER THREAD
   ONLY, like every pass.

   TWO INSTANCES, as the world passes have (tagpu_scene.h): the game's world prepares its own from
   tagpu_native_fogsrc(), the wiki's stage its own (tagpu_vkw_fog_*) from tagpu_wikiworld_fogsrc(),
   and each world pass binds the instance its own scene was compiled with.

   THE ORDERING IS THE CONTRACT. The seam calls `prepare` for slot i once a frame, after its wait on
   fence[i] and before the first pass's `prepare`, outside any render pass; each pass then reads the
   view and the size for slot i in ITS `prepare` for slot i. See tagpu_vk_fog.c for why that makes
   a re-created image unreachable by any submission still in flight. */
#include "tagpu_vk_pass.h"
#include "tagpu_fogsrc.h"

/* Slot `slot`'s image made to hold `f`: re-created only when its extent `(stride, h)` changed,
   the words copied into the slot's staging and the copy recorded when the slot's serial differs
   from `f`'s, then one barrier TRANSFER_WRITE -> SHADER_READ for the vertex and fragment stages.
   A NULL `f`, or one tagpu_fog_src_ok refuses, leaves the slot holding a valid image of no source
   (size 0 x 0), so a pass's descriptor always names a live view. Returns 1 when the slot holds
   `f` this frame, 0 otherwise. */
int  tagpu_vk_fog_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, int slot, const TAGPU_FOGSRC* f);

/* Slot `slot`'s view, or VK_NULL_HANDLE when the device refused the image. `*gen` is the image's
   generation, unique for the life of the process, so a pass rewrites its slot's descriptor exactly
   when the view it names is not this one. */
VkImageView tagpu_vk_fog_view(int slot, unsigned* gen);

/* The source slot `slot` holds this frame, in cells: uFogSize. (0, 0) when it holds none -- then
   the shader fetches nothing, and a draw that wants fog has to stand down. */
void tagpu_vk_fog_size(int slot, int* w, int* h);

/* Every image and buffer back, behind the seam's device wait. */
void tagpu_vk_fog_down(const TAGPU_VKPASS* d);

#endif
