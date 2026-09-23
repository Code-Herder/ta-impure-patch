#ifndef TAGPU_VK_SHADOW_H
#define TAGPU_VK_SHADOW_H
/* The Classic++ cast-shadow depth map, drawn by Vulkan (Phase G, the FIFTH
   world pass). Implementation: tagpu_vk_shadow.c. Read the TAGPU_SHADOWHAND
   block at the bottom of this file before anything else here, because this
   pass has no producer.

   IT IS THE FIRST PASS THAT DRAWS INTO SOMETHING OTHER THAN THE FRAME. Every
   pass before it records into the seam's render pass and produces pixels; this
   one owns an offscreen depth image, draws the casters into it from the
   light's point of view, and leaves it in SHADER_READ_ONLY_OPTIMAL for the
   passes that sample it. So it has a `prepare` and no `record`:

     prepare()   before vkCmdBeginRenderPass -- build what is missing, upload
                 the caster mesh when it moved, then begin ITS OWN render pass,
                 draw, and end it. A render pass may not be nested inside
                 another, and `prepare` is the hook that runs outside the
                 seam's, which is the whole reason the contract has two halves.

   NOTHING HERE NAMES A WINDOW, A SURFACE OR A SWAPCHAIN -- standing constraint
   3. Owning a second render target is not presentation: the seam still owns the
   one image that reaches a screen, and this pass would work unchanged against
   an offscreen frame or another process's.

   IT MUST RUN BEFORE ITS CONSUMERS' `prepare`, because they point their
   descriptor sets at `tagpu_vk_shadow_view(slot)` there -- see the seam's
   ordering comment. */

#include "tagpu_vk_pass.h"

/* Draw this frame's map. Returns 1 when a complete map now stands in
   `tagpu_vk_shadow_view(slot)`, 0 when nothing was drawn -- no map this frame,
   a device refusal, or a map holding casters the hand-over carries no copy of
   (`otherCasters`, in the TAGPU_SHADOWHAND block below, which is every unit until the unit pass
   lands). `slot` is the frame slot the seam's fence has proved free.

   IT DRAWS NO FRAME PIXEL, so the seam must NOT count it among the passes that
   drew into the frame: that count exists to catch two passes contaminating one
   A/B capture, and this one cannot. */
int  tagpu_vk_shadow_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);

/* 1 when the map drawn for `frame` is complete and may be sampled. A consumer
   asks with its OWN frame number so that a map left over from an earlier frame
   can never be sampled as though it were this one's. Valid after `prepare`. */
int  tagpu_vk_shadow_ready(unsigned frame);

/* This slot's depth image, in SHADER_READ_ONLY_OPTIMAL, or VK_NULL_HANDLE.
   A consumer writes it into its own descriptor set during its own `prepare`,
   which is the one instant the seam's fence proves that set is not in flight.

   ASK WITH YOUR OWN FRAME, as for `tagpu_vk_shadow_ready`: the map behind this
   view is one image per slot and one `live` flag for the pass, so a caller out
   of step with `prepare` would otherwise be handed an earlier frame's. Wrong
   frame or wrong slot gives VK_NULL_HANDLE, and the caller names its own dummy
   -- which it must have anyway for a frame with no map. */
VkImageView tagpu_vk_shadow_view(unsigned frame, uint32_t slot);

/* THE FORMAT THE MAP IS IN, asked of the device and cached. A consumer needs
   it for two things and must not guess at either: whether its COMPARE sampler
   may be LINEAR (taShadowAt's PCF is bilinear, and linear filtering of a depth
   format is a feature bit, not a given), and what format to give the dummy
   image its descriptor set names on a frame with no map -- which has to be one
   the same sampler is valid against. Exposed rather than re-derived so that the
   two files agree by construction instead of by both happening to try the same
   candidates in the same order. VK_FORMAT_UNDEFINED when the device has no
   24-bit depth format that is both a depth attachment and a sampled image, and
   `*linearOk` says whether it can be filtered. Safe to call before `prepare`
   and before anything is built; it resolves the one entry point it needs. */
VkFormat tagpu_vk_shadow_format(const TAGPU_VKPASS* d, int* linearOk);

/* The map's edge in texels this frame, or 0. The consumers' uShScale carries
   1/res in their own hand-overs (tagpu_terr.h `shScale`), so this is not for
   arithmetic; nothing in the DLL calls it. */
int  tagpu_vk_shadow_res(void);

/* Give everything back. Called by the seam from `vk_down`, after its
   vkDeviceWaitIdle and before the device is destroyed. Safe when nothing was
   ever built. */
void tagpu_vk_shadow_down(const TAGPU_VKPASS* d);

/* 1 while the pass has stopped drawing mid-frame because the device would not
   give it this frame's resources, and is waiting for the seam to tear it down
   behind a vkDeviceWaitIdle. The contract, and why `_down_paid` is separate
   from `_down`, is tagpu_vk_fx.h's at length. */
int  tagpu_vk_shadow_down_owed(void);
void tagpu_vk_shadow_down_paid(const TAGPU_VKPASS* d);

/* ---- WHAT A SHADOW HAND-OVER IS, AND WHO IS SUPPOSED TO PUBLISH ONE -------

   This type is kept because it is the shape of the thing this pass needs,
   and the next producer should not have to invent it again.

   THERE IS NO PRODUCER. Nothing writes a hand-over, so
   `tagpu_vk_shadow_prepare` returns 0 on every frame -- and nothing says so,
   because `shadowOn` is 0 as well and the consumers' refusals are gated on
   it: both halves are dark together. See the vulkan-only plan's landing 11.

   SO THIS PASS IS A FOUNDATION, NOT A FEATURE. Everything below it still
   compiles, still builds its pipeline lazily, and still draws exactly what it
   is handed. Reviving cast shadows is writing the producer: something on the
   render thread that owns the light basis and the map extent, fills the struct
   below and publishes it for `d->frame`. That is a feature landing with its own
   measurement.
   THE POINTERS ARE THE TERRAIN MODULE'S and name the CPU mirror of the caster
   mesh, which that module frees and rebuilds on a map change. `frame` is the
   fork's monotonic render-thread counter and the hand-over refuses
   any other frame's, exactly as the terrain and feature hand-overs do: it is
   what makes "these pointers are alive" a property of the frame number rather
   than of which functions happened to run. */
typedef struct TAGPU_SHADOWHAND {
    unsigned frame;
    int      res;                /* the map's edge in texels, this frame     */
    float    mat[16];            /* uShadowMat, column-major, as the
                                    consumers' shaders take it             */
    /* the heightfield caster, and the exact index range of it to draw */
    const float*    hv;   size_t hnv;
    const unsigned* hi;   size_t hni;
    unsigned        hillsSerial;
    unsigned        firstIndex, indexCount;   /* 0 = the hills did not draw  */
    int             otherCasters;             /* see above; 0 = complete     */
} TAGPU_SHADOWHAND;

#endif
