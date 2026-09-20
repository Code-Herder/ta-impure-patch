#ifndef TAGPU_VK_PASS_H
#define TAGPU_VK_PASS_H
/* What the presentation seam hands a ported pass (Phase G / G19d onward).

   THE SEAM IS STILL ONE FILE. Standing constraint 3 says surface, swapchain,
   acquire and present live in tagpu_vk.c and that nothing else may know a
   window exists. A pass is not presentation: it is handed a device, a render
   pass and a command buffer that is already being recorded, and it could draw
   into an offscreen image, a swapchain image or another process's image
   without a line of it changing. Nothing in this struct names a window, a
   surface or a swapchain, which is what keeps that true rather than merely
   claimed.

   EVERY PASS RESOLVES ITS OWN ENTRY POINTS, from the `gipa` and `gdpa` here.
   That is deliberate duplication: tagpu_vk.c's table is the presentation one
   (swapchain, acquire, present, fences) and a pass's is its own (pipelines,
   descriptors, buffers, images), and the two barely overlap. One shared table
   would have to be the union of every pass ever written, and would make the
   file that owns it a dependency of all of them.

   NEVER CAST A VULKAN HANDLE TO A POINTER, store one in a `void*` or key a
   container on one -- the whole-phase rule (standing constraint 2). At 32-bit
   these are `uint64_t` and the compiler enforces it; at 64-bit it would not.

   A PASS READS NO ENGINE STATE. Every value it needs arrives as an argument,
   exactly as tagpu_vk.c's does, so no pass file may go on
   tagpu/ddraw/thread-split.allow. */

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

/* Frames in flight, which is the swapchain's image count -- tagpu_vk.c's
   MAXIMG is defined from this so the two cannot drift. A pass that writes a
   buffer the GPU may still be reading keeps one copy per slot and is handed the
   slot index; the seam's fence wait at the top of its present is what proves
   slot `i` is free, and it is the only thing that does. */
#define TAGPU_VK_SLOTS 8

typedef struct {
    VkInstance                inst;
    VkPhysicalDevice          pd;
    VkDevice                  dev;
    /* One colour attachment in the swapchain's format, LOAD/STORE, ending in
       PRESENT_SRC. Created once per device and valid for its life, so a pass
       may build a pipeline against it and keep it across a resize. */
    VkRenderPass              rp;
    VkFormat                  fmt;
    /* THE DEPTH ATTACHMENT'S FORMAT, or VK_FORMAT_UNDEFINED when the render
       pass has none (G19e). Every pipeline built against `rp` must supply a
       VkPipelineDepthStencilStateCreateInfo once this is set -- a null
       pDepthStencilState in a subpass that has a depth attachment is invalid --
       so a pass that does not test depth still declares one with testing and
       writes off. A pass that DOES test must refuse to arm when this is
       UNDEFINED rather than draw untested.
       24-bit fixed point by construction, because the GL lane's world FBO is
       GL_DEPTH24_STENCIL8 and a comparison against it is a comparison of two
       quantisations as much as of two rasterisers. */
    VkFormat                  dfmt;
    uint32_t                  slots;    /* <= TAGPU_VK_SLOTS                   */
    /* VK_KHR_maintenance1, and so a NEGATIVE VIEWPORT HEIGHT. GL's clip space
       has +Y up and Vulkan's has +Y down, so a shader written in GL's window
       convention draws its frame upside down -- and the shaders are ported
       unchanged on purpose, because a source edit would make each one disagree
       with the GL twin that is its oracle. The flip is therefore pipeline state,
       and this is whether the device will do it. 0 means a pass that needs it
       must not arm; it may not fall back to flipping geometry, which would
       mirror every glyph.

       WHICH PASSES NEED IT IS NOT "ALL OF THEM", and assuming it was cost the
       lane an upside-down picture for eight landings [landing 5b, 2026-09-17].
       It depends on the SHADER's y convention, and this tree has two:

         * `1 - y*2`, or an NDC rect built y-up -- tagpu_vk_gui.c's composite,
           tagpu_vk_fps.c, tagpu_vk_scaffold.c. These speak GL's WINDOW
           convention, so they need the flip and still take it.
         * `p.y/uGame.y*2 - 1` on the engine's screen-space y, which grows
           DOWNWARD -- tagpu_vk_terr.c, _feat.c, _fx.c, _unit.c, _mark.c. Clip
           -1 is the game frame's TOP row, which is row 0 under Vulkan already.
           These must NOT flip, and no longer ask for this at all.

       The two are distinguishable in one line of each pass's vertex shader, and
       the A/B cannot tell them apart: it compares the lanes to each other, and a
       flip they share cancels. The screen is the oracle for this one. */
    int                       flipok;
    /* VK_EXT_line_rasterization WITH `bresenhamLines`, ENABLED ON THE DEVICE.
       A pass that draws LINES needs it and may not draw without it.
       MEASURED 2026-09-15 (G19e, the effects pass): under Vulkan's DEFAULT
       lineRasterizationMode the ported lasers came out a strict SUPERSET of
       their GL twin's -- all 126 of the twin's pixels plus exactly one extra
       fragment at the END of each line segment. GL's non-antialiased lines
       follow the diamond-exit rule; Vulkan's default mode does not and
       BRESENHAM does. It is the same shape as `flipok`: a rule the GL twin
       already obeys, adopted as pipeline state rather than worked around, and
       a pass whose device will not offer it stands down instead of drawing a
       picture four pixels away from its own oracle. */
    int                       lineok;
    /* `wideLines` WAS ENABLED ON THE DEVICE, and the widest line it will
       rasterise. A pass that draws lines into a SUPERSAMPLED target needs
       it: the GL twin calls `glLineWidth(ss)` so that a line one game pixel
       wide is `ss` pixels of the target, and a Vulkan pipeline fixed at 1.0
       draws a line `ss` times too thin. Same shape as `flipok` and
       `lineok`: a rule the GL twin already obeys, adopted as pipeline state.
       UNTIL 4c-2 THERE WAS NO SUPERSAMPLED TARGET, so `ss` was a number the
       lane could only refuse -- tagpu_vk_fx.c and tagpu_vk_mark.c stood the
       WHOLE pass down on any frame carrying line vertices, which on the
       shipped default (`ss` is 2 unless `tagpu_ss.off` is there) meant the
       effects pass dropped every frame with a laser in it. Measured on the
       lane 2026-09-18 before this was added.
       `maxLineWidth` is `lineWidthRange[1]`, and a width past it is refused
       rather than clamped: a clamped width is a line a different thickness
       from its own oracle, which is the thing this family of flags exists
       to prevent. 0 means the device does not offer it. */
    int                       wideok;
    float                     maxLineWidth;
    /* VK_EXT_depth_clip_control WITH `depthClipControl`, ENABLED ON THE DEVICE,
       and so a pipeline may ask for GL'S OWN CLIP-SPACE Z RANGE.
       GL maps clip z in [-1, 1] onto the depth range; Vulkan takes [0, 1] and
       CLIPS the rest. Every world pass ported through G19e writes a z already
       in [0, 1], so `minDepth 0.5 / maxDepth 1.0` reproduces GL exactly and
       none of them needs this. THE SHADOW PASS IS THE EXCEPTION: its
       orthographic light matrix is built to fill [-1, 1] (tagpu_shadow.c
       `mrow`), so without this the near half of every caster is clipped away
       and the depth map is WRONG rather than merely different -- and the
       depths it stores are what the consumers' taShadowAt compares against.
       Same shape as `flipok` and `lineok`: a rule the GL twin already obeys,
       adopted as pipeline state rather than worked around, and a pass whose
       device will not offer it stands down instead of drawing a map its own
       oracle would not recognise. */
    int                       zclipok;
    /* `samplerAnisotropy` WAS ENABLED ON THE DEVICE, and the largest ratio it
       will apply. This fork filters exactly one class of texture -- the
       Classic++ restored twins, the only ones holding true colour rather than
       palette indices -- and tagpu_gaf.c gives those GL_LINEAR_MIPMAP_LINEAR
       with GL_TEXTURE_MAX_ANISOTROPY_EXT 4. Same shape as `flipok` and
       `zclipok`: a rule the GL twin already obeys, adopted rather than worked
       around, and a pass whose device will not offer it stands down on the
       frames that would sample one instead of drawing art its own oracle
       filtered differently. */
    int                       anisook;
    float                     maxAniso;
    /* WHICH RENDER-THREAD FRAME THIS IS -- the fork's own monotonic counter,
       the same number the GL lane stamped its hand-over with earlier in this
       iteration of render_ogl.c's loop.
       A pass uses it to REFUSE a hand-over that is not this frame's, and that
       refusal is the whole point: the GL modules' hand-overs alias buffers
       those modules own and rebuild (the tile atlas, the height grid, the
       vertex arrays), so a hand-over left standing from an earlier frame can
       name memory that has since been freed. Before the G19e re-review the
       only thing stopping that was "the publishing function is called every
       frame, and it clears the flag on the way out" -- which is not true of a
       frame whose gather bailed, because then it is not called at all.
       [FROM THE G19e RE-REVIEW, 2026-09-15 -- both reviewers, separately.] */
    unsigned                  frame;
    /* THE CLEAN CUT IS ARMED (`tagpu_purevk.on`), so no pixel on the presented
       frame may originate from an OBSERVATION of the engine -- neither TA's own
       composed 8-bit frame drawn as a layer, nor the UI twin built by replaying
       the engine's draw ops. The seam polls the lever on its own 250 ms cadence
       and publishes the answer here, so a pass never reads the file itself and
       every pass in a frame sees one value.

       IT IS NOT AN ARMING FLAG AND MUST NOT BE USED AS ONE. What the cut stops
       is DRAWING; everything upstream of the draw keeps running, because the
       engine's frame is kept as the golden source to check ourselves against
       (`tagpu_surf_take` on the game side, `tagpu_vk_surf_prepare`'s upload on
       this one) and because the twin store has to stay level with the op stream
       whether or not we composite it. A pass that stood DOWN on this would take
       the reference with it, which is the one thing the cut exists to keep.
       [The vulkan-only plan, "The Clean Cut".] */
    int                       pureVk;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr   gdpa;
    void                    (*log)(const char* s);
} TAGPU_VKPASS;

#endif
