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
       has +Y up and Vulkan's has +Y down, so a shader ported unchanged draws
       its frame upside down -- and the shaders are ported unchanged on purpose,
       because a source edit would make each one disagree with the GL twin that
       is its oracle. The flip is therefore pipeline state, and this is whether
       the device will do it. 0 means a pass that needs it must not arm; it may
       not fall back to flipping geometry, which would mirror every glyph. */
    int                       flipok;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr   gdpa;
    void                    (*log)(const char* s);
} TAGPU_VKPASS;

#endif
