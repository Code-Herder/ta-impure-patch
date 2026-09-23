#ifndef TAGPU_VK_WORLD_H
#define TAGPU_VK_WORLD_H

/* THE OFFSCREEN WORLD TARGET AND THE DRAW THAT PUTS IT ON THE FRAME.
   Implementation: tagpu_vk_world.c.

   WHAT IT IS. The world is rendered into a target at the GAME's resolution
   times `ss`, and that block is then composited into the letterboxed viewport
   rect of the window.

   WHY IT IS NOT MERELY A QUALITY LEVER. Two of the lane's passes stand DOWN on
   the shipped default without it. `ss` is 2 unless `tagpu_ss.off` is there,
   lines are drawn `ss` pixels wide, and with no ss target there is nowhere to
   put a 2-px line -- so tagpu_vk_fx.c and tagpu_vk_mark.c refuse the WHOLE
   pass on any frame carrying line vertices:

       vk: fx: the target is 2x supersampled, which makes a line 2 px wide,
               and this device offers no wideLines at all - nothing drawn
               while there are line vertices

   MEASURED ON THE LANE 2026-09-18, in a live game on `renderer=vulkan` with
   `fx.on` and no ss target. The census read `fx=1` on frames without lasers
   and the pass dropped every frame with one. That is what "ss=2 has no
   target" actually costs, and it is why the device is also asked for
   `wideLines`.

   THE TWO IMAGES ARE PER SLOT, and that is a lifetime argument rather than a
   convenience. The composite SAMPLES the colour image in the same command
   buffer that wrote it, but frame N+1's writes may begin while frame N is still
   in flight; one shared image would be written by the next frame while the
   present of this one still reads it. The seam's fence wait at the top of the
   frame is what proves slot `i` is free, and it is the only thing that does --
   so there is one colour+depth pair per slot, exactly like every other per-slot
   resource in this tree.

   THE RENDER PASS IS COMPATIBLE WITH THE SEAM'S BY CONSTRUCTION, WHICH IS WHY
   NO PASS FILE CHANGED. Vulkan render-pass compatibility ignores load/store ops
   and layouts and compares attachment FORMATS and sample counts, so an
   offscreen pass built with `d->fmt` and `d->dfmt` in the seam's own order
   accepts every pipeline already built against `d->rp`. The world passes are
   handed a different render pass and a different extent and do not know it:
   tagpu_vk_pass.h's promise that a pass "could draw into an offscreen image, a
   swapchain image or another process's image without a line of it changing"
   holds here.

   AND THE EXTENT IS THE ONLY THING THEY NEEDED. Every world pass already scales
   the engine's game-space rect into whatever target it is handed --
   `terr_scissor` computes `sx = w / uGame.x`, so handing it `gw * ss` yields
   exactly the scissor `(vpL * ss, vpT * ss, vw * ss, vh * ss)` -- and the
   vertex shaders divide by `uGame` rather than by the target.

   ORIENTATION: NO FLIP, and the argument needs to know which way up neither
   image is. `tagpu_native::DVS` pairs `uv = 0` with clip `y = -1`, which maps
   source row 0 to destination row 0 when the viewport height is
   positive. The world passes write the engine's screen-space y, which grows
   DOWNWARD, under a positive height -- so the game's top row is row 0 of this
   image for exactly the reason it is row 0 of the swapchain image.

   ONE DRAW. This draws the `ss` image straight into the viewport rect with
   LINEAR, which at k = 1 is the 2:1 box filter -- a destination centre maps to
   the corner of a 2x2 source block, so bilinear IS the 2:1 box filter -- and
   at k != 1 it is the `devres` path. The gap that buys: `selAt1x` is not
   expressible -- there is no 1x buffer to draw a one-pixel line into -- so the
   selection rect, which the marker pass draws into this target, answers it in
   its fragment stage instead: it keeps every sample of a game pixel on the
   engine's Bresenham path and none of any other, so the downsample resolves it
   to the full colour (tagpu_mark.c, SVS/SFS; ui-markers.md §1).

   THE CLEAR IS TRANSPARENT AND THE COMPOSITE IS PREMULTIPLIED, which together
   are what leave TA's own frame showing where the world drew nothing: the
   clear is `{0, 0, 0, 0}` and the blend is (ONE, ONE_MINUS_SRC_ALPHA), and
   dropping either half would either black out the letterbox or double-expose
   the bottom layer.

   THE SEAM DRIVES THE RENDER PASS, because it has to wrap the OTHER passes'
   `record` calls -- so this module is not shaped like a pass and deliberately
   does not pretend to be. `prepare` builds and sizes; `begin`/`end` bracket the
   world; `record` is the composite, drawn inside the SWAPCHAIN render pass
   between TA's own frame and the UI. Nothing here knows a window exists: it is
   handed an extent and a rect, which is standing constraint 3 intact. */

#include "tagpu_vk_pass.h"

/* Build and size this slot's target from the frame's published geometry.
   Returns 1 when `begin`/`record` may be called for this slot this frame, and
   fills `tw`/`th` with the extent the world passes must be handed. 0 means
   there is no target this frame -- the caller draws the world into the
   swapchain image, which is a smaller picture and never a wrong one. Outside any render pass, before `begin`. */
int  tagpu_vk_world_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                            uint32_t* tw, uint32_t* th);

/* HOW MANY SAMPLES PER GAME PIXEL THIS FRAME'S TARGET ACTUALLY HAS, or 1 when
   there is none and the world is going into the swapchain image at client
   resolution. A pass that scales something by `ss` -- the line width
   is the only one today -- must ask THIS rather than read `ss` out of its own
   hand-over, because the hand-over says what the gather was told (tagpu_native.c's
   `ss`) and this says what this frame's target is, and on the fallback path
   those differ. Valid only
   after `tagpu_vk_world_prepare` has run for this frame, which is why the seam
   calls it before any pass's `prepare`. */
int  tagpu_vk_world_scale(void);

/* Open and close the world render pass around the world passes' `record`s. */
void tagpu_vk_world_begin(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);
void tagpu_vk_world_end(const TAGPU_VKPASS* d, VkCommandBuffer cb);

/* The composite, inside the seam's render pass, after TA's own frame and
   before the UI. `w`/`h` are the swapchain extent, for clamping the rect. */
void tagpu_vk_world_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                           uint32_t w, uint32_t h);

/* THE IMAGE THE WORLD WAS DRAWN INTO THIS FRAME, for the A/B capture and for
   nothing else. Returns 1 and fills `img`/`w`/`h` when `slot` is the slot the
   world render pass was opened on this frame; 0 when there is no target, when
   the slot is a different one, or before `begin` has run. The image is in
   SHADER_READ_ONLY_OPTIMAL -- the offscreen pass's `finalLayout` -- and `fmt`
   comes back as the format `slot_size` ACTUALLY built it with rather than as
   `d->fmt`, because a caller that decides a channel order needs the format of
   the image in its hand: `s_pass.fmt` is copied from `s_vk.fmt` once at bring-up
   while `vk_resize` re-picks `s_vk.fmt` and refreshes only `s_pass.slots`.
   Valid from `begin` until the next frame's `prepare`, and a teardown in between
   clears it.

   WHY THE CAPTURE READS THIS IMAGE. It is `gw*ss, gh*ss` and holds the world
   alone over a transparent clear, so a world A/B is the same size and the
   same content at every `ss`. The SWAPCHAIN image at the window's client rect
   matches that only at `ss = 1` with no letterbox, and `ss` is 2 unless
   `tagpu_ss.off` is there. */
int  tagpu_vk_world_shot(uint32_t slot, VkImage* img, uint32_t* w, uint32_t* h,
                         VkFormat* fmt);

/* The teardown handshake every module here has: the seam asks, waits for the
   device to go idle, then pays. */
void tagpu_vk_world_down(const TAGPU_VKPASS* d);
int  tagpu_vk_world_down_owed(void);
void tagpu_vk_world_down_paid(const TAGPU_VKPASS* d);

#endif
