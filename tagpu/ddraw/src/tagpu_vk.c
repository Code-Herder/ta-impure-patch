/* tagpu_vk.c -- the Vulkan backend (Phase G / G19). API and contract:
   tagpu_vk.h.

   ------------------------------------------------------------------ G19a ---
   COEXISTENCE IS SETTLED, AND ONLY ONE OF THE ROADMAP'S THREE ROUTES SURVIVES
   [MEASURED 2026-09-15, tools/vkcoexist.c + tools/vkcoexist-pixels.sh].
   Phase G's kill rule asked whether Vulkan can present on the window
   cnc-ddraw's GL renderer already owns -- `GetDC(hwnd)`, a `SetPixelFormat` on
   it (dd.c) and a 3.3 core context on that DC (render_ogl.c `ogl_create`). The
   probe brings GL up exactly that way and then tries all three routes, and the
   second script asks the question that decides it: after the route, does a GL
   frame still REACH THE SCREEN?

     route                                    wine 9.0            Proton 11
     A  same HWND, GL context left current    API ok, PIXELS DEAD   ok
     B  route 1: release the GL context       API ok, PIXELS DEAD   ok
        first, Vulkan, then GL back
     C  route 2: a child window               REFUSED               ok
        (VK_ERROR_INCOMPATIBLE_DRIVER -9 -- winevulkan wants a top level)
     D  route 3: Vulkan on its OWN            ok                    ok
        top-level window

   THAT TABLE IS THE RECORD OF A CHOICE THIS FILE NO LONGER MAKES. It took
   route D -- Vulkan on a top-level window of its own -- for as long as a GL
   backend could be in the same process, and the vulkan-only plan's landing 4d-1
   deleted that arrangement. What it takes now is the surface straight on the
   GAME window, which is the table's route A, and the row above marks route A
   PIXELS DEAD on wine. THE REASON THAT IS SAFE HERE IS NOT IN THE TABLE: route
   A's failure is winevulkan taking an HWND over so that GL can never draw to it
   again, and on this path THERE IS NEVER A GL CONTEXT IN THE PROCESS to lose.
   `renderer=vulkan` is dispatched at `dd.c` and cannot change afterwards. The
   table is kept because it is what was measured, and because the out-of-process
   64-bit renderer will own a window again.

   THE API LIED, AND THAT IS THE POINT OF THE SECOND SCRIPT. Routes A and B
   return VK_SUCCESS for every call, present 10 of 10 frames, and then accept
   every GL call afterwards: `SwapBuffers` returns TRUE and `glGetError` is
   clean. The window keeps showing Vulkan's last frame anyway. This was first
   met in the game, not in the probe: with the lever armed and then cleared, the
   window stayed magenta while `tacli glshot` read 168 distinct colours off the
   GL framebuffer -- GL rendering correct frames that nothing would ever see --
   and it SURVIVED A FULL VIDEO-MODE CHANGE and the new GL context that comes
   with it. Once winevulkan has put a surface on an HWND, that HWND is finished
   for GL for the life of the process.

   WHAT ROUTE D COST WHILE IT LASTED, and why it was worth it then: a second
   HWND to create, track, show, hide and destroy -- real machinery where route A
   was none -- in exchange for never touching the GL lane, so the lever was
   two-way and the menu kept its invariant that no row needs a relaunch. That
   trade ended when the GL lane stopped being in the process at all, and landing
   4d-1 removed the machinery (152 lines in, 370 out). The one thing it bought
   that still matters: the out-of-process 64-bit renderer will own its own window
   by definition, so the tracking was written once rather than twice, and it is
   in the history when that move needs it.

   WHAT IS NOT ESTABLISHED, and it is the honest limit: every row of that table
   is the linux NVIDIA ICD under wine. No Windows box has run it. The cell that
   would close it is the `_local` test VM.

   ------------------------------------------------------ WHERE IT RUNS, AND WHY
   THE BRING-UP IS ON A WORKER THREAD OF ITS OWN, AND NOT NEGOTIABLE.
   `vkCreateInstance` loads the ICD, so bringing Vulkan up IS a `LoadLibrary`.
   field-notes' rule, paid for once already by the companion-DLL design: LOAD
   FROM YOUR OWN THREAD, NEVER FROM DllMain OR MID-PRESENT, and go through
   `real_LoadLibraryA` so the fork's `hook=4` LoadLibrary hook does not re-scan
   the module tree underneath us. All three are obeyed here:

     - nothing in this file is reachable from `DLL_PROCESS_ATTACH` except
       `tagpu_vk_names_init`, which is a file read and touches no Vulkan;
     - the instance, the device and the swapchain are built by a worker thread
       started from the render thread, never on the render thread itself;
     - `real_LoadLibraryA` loads `vulkan-1.dll`.

   THE HAND-OVER IS AN ORDERING, AND NOTHING EVER WAITS. `s_state` is the only
   channel between the workers and the render thread:

     ST_OFF -> ST_STARTING   whoever is about to create a worker, by
                             InterlockedCompareExchange and BEFORE the thread
                             exists -- the render thread for a bring-up, and
                             `tagpu_vk_enum_start` for the device list. Both,
                             because they share `s_mod`, `s_gipa` and the whole
                             instance-level dispatch table, and an instance's
                             entry points belong to that instance.
     ST_STARTING -> ST_READY the worker, by compare-exchange (never a bare
                             store: `render_stop` may have moved the lane to
                             ST_ZOMBIE behind its back), AFTER every field of
                             `s_vk` is written. The interlocked operation is a
                             full barrier, so a render thread that reads
                             ST_READY sees all of them.
     ST_STARTING -> ST_FAILED  the worker, the same way.
     ST_READY -> ST_OFF      the render thread, and only it, and only from
                             ST_READY -- the state in which the worker has
                             stopped touching `s_vk` for good.
     ST_STARTING -> ST_ZOMBIE  `tagpu_vk_render_stop`, abandoning a worker.
     ST_ZOMBIE -> ST_OFF     `lane_release`, and nothing else.

   So `s_vk` and the dispatch table have exactly one owner at every instant, by
   the state and not by timing.

   ST_ZOMBIE IS A WORKER WINDING DOWN, NOT A GRAVE, AND NOTHING WAITS FOR IT.
   An earlier revision had `tagpu_vk_render_stop` wait five seconds for a worker
   still in ST_STARTING and then LEAK its objects. Both halves were wrong. The
   game thread waits INFINITE on the render thread across a mode change
   (`dd.c`), so a mode change catching a bring-up stalled the whole LOCKSTEP
   world for the 371-451 ms it had left; and the window thread IS the game
   thread here, so a winevulkan call that reached the window with an
   inter-thread send would have closed the cycle game -> render -> worker ->
   window with only that timeout to break it. Now the lane is simply marked
   ST_ZOMBIE: the worker, still the sole owner of everything it built, puts its
   own objects back, destroys its own window, and hands the lane to ST_OFF so a
   later render thread can bring it up again. Nothing waits and nothing leaks.

   THE WINDOW IS THE GAME'S AND THE LANE NEVER OWNS ONE, since landing 4d-1.
   What used to need a rule here -- who may destroy the lane's own popup, and in
   which states a worker might still hold a surface on it -- has no subject any
   more: the surface goes on the window the caller names and nothing in this file
   creates, moves or destroys a window.

   IT READS NO ENGINE STATE. Every value arrives as an argument. This file is
   not on `thread-split.allow` and must never need to be.

   NON-DISPATCHABLE HANDLES ARE uint64_t HERE AND POINTERS AT 64-BIT. Nothing
   below casts one to a pointer, stores one in a `void*` or keys anything on
   one, and the header exposes no Vulkan type so a caller cannot either.

   ------------------------------------------------------------------ G19b ---
   THE DEVICE LIST IS A CACHE, AND IT IS ONE LAUNCH BEHIND ON PURPOSE. The row's
   captions live inside the generated `.GUI`, which `tagpu_menu_init` writes at
   DLL attach because the engine globs `*.UFO` before it will read any of them
   -- and at DLL attach we may not create a Vulkan instance (see above). So a
   worker enumerates the devices once the render thread is up and writes
   `tagpu_vk.gpus`; the menu reads that file at the NEXT attach. A machine whose
   cards changed lists them at the next launch, which is the same bargain the
   Monitor row already makes for a hot-plugged monitor.

   The enumeration runs WHETHER OR NOT THE VULKAN LANE IS ARMED. The picker is
   the player-facing half of Phase G and the roadmap says a phase that stops at
   G19b has still shipped it -- so it cannot be gated on the lane it outlives.

   THE CHOICE IS STORED BY NAME, NOT BY INDEX (`tagpu_vk.cfg`). An index moves
   when a card is added, removed or re-ordered by the driver, and would then
   point at a different GPU without anything saying so. A name that is no longer
   present falls back to the discrete default and logs that it did.

   WHAT THE ROW CANNOT DO, stated in the UI and not hidden: it binds the VULKAN
   device only. OpenGL cannot be retargeted in-process -- `WGL_NV_gpu_affinity`
   is Quadro-only -- so under the GL lane the GPU is a launcher-level setting
   (`DRI_PRIME` / `__NV_PRIME_RENDER_OFFLOAD` through tacli's `Instance.env()`,
   or the per-application driver profile on Windows). The row is greyed when the
   Vulkan lane is not armed, for exactly that reason. */

/* VK_NO_PROTOTYPES: every entry point here is resolved through
   vkGetInstanceProcAddr / vkGetDeviceProcAddr and held in a variable of the
   same name, so the headers must not also declare them as functions -- and the
   DLL must not import a single symbol from vulkan-1.dll, or a machine without
   Vulkan would fail to LOAD rather than simply not arming the lane. */
/* tagpu_vk_pass.h defines VK_NO_PROTOTYPES and pulls the headers in, and the
   comment above is why both are needed. */
#include "tagpu_vk_pass.h"
#include "tagpu_ftime.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hook.h"
#include "tagpu_vk.h"
#include "tagpu_vk_fps.h"
#include "tagpu_vk_scaffold.h"
#include "tagpu_vk_surf.h"
#include "tagpu_vk_world.h"
#include "tagpu_vk_feat.h"
#include "tagpu_vk_terr.h"
#include "tagpu_vk_fx.h"
#include "tagpu_vk_shadow.h"
#include "tagpu_vk_restore.h"
#include "tagpu_vk_unit.h"
#include "tagpu_vk_hires.h"
#include "tagpu_vk_mark.h"
#include "tagpu_vk_gui.h"
#include "tagpu_vk_shot.h"

#define ON_FILE    "tagpu_vk.on"
/* THE CONTROL, and the module needs one because half of it runs with the lever
   off. The GPU enumeration is deliberately not gated on `tagpu_vk.on` -- the
   picker outlives the lane -- so an instance that wants the GL build's exact
   behaviour, thread for thread and file for file, has to be able to say so.
   `tagpu_vk.off` is that: with it present nothing in this file runs, the
   enumeration worker included, and the row plates whatever the cache last
   said. It is what an A/B against the pre-G19 DLL arms. */
#define OFF_FILE   "tagpu_vk.off"
/* THE A/B (Phase G / G19d). `tagpu_fps.c` owns the lever (`tagpu_fps.ab`): it
   clears the GL frame to black, writes `tagpu_fps_gl.ppm` and hands the flag
   over with the vertices, and this file writes `tagpu_fps_vk.ppm` out of the
   swapchain image it presents THAT frame. Set `color=0,0,0` in the lever file
   so the two backgrounds match, or the comparison is of two different fields
   with the same text on them. */
/* ONE FILE PER PORTED PASS, because each pass has its own GL twin and its own
   lever. `s_abPath` below carries whichever one claimed the pending frame. */
#define AB_FPS     "tagpu_fps_vk.ppm"
#define AB_SCAF    "tagpu_scaffold_vk.ppm"
#define AB_FEAT    "tagpu_feat_vk.ppm"
#define AB_TERR    "tagpu_terr_vk.ppm"
#define AB_FX      "tagpu_fx_vk.ppm"
#define AB_MARK    "tagpu_mark_vk.ppm"
#define AB_UNIT    "tagpu_posedraw_vk.ppm"
#define AB_GUI     "tagpu_gui_vk.ppm"
/* THE ONE LIST OF THEM, in the order `vk_present` selects in (which is the
   order the nested ternary it replaced tested in). The `tag` is the SAME string
   the pass passes to `tagpu_vk_ab_arm`, so a pass names its file once, here,
   and both the write and the arming's unlink read this row.
   `posedraw`'s file is `AB_UNIT`: the pass is the unit pass and the lever is
   `tagpu_posedraw.ab`, and that mismatch is exactly why this is a table and not
   a `_snprintf` of the tag. */
static const struct { const char* tag; const char* path; } s_abFiles[8] = {
    { "terr", AB_TERR }, { "feat", AB_FEAT }, { "posedraw", AB_UNIT }, { "fx", AB_FX },
    { "mark", AB_MARK }, { "scaffold", AB_SCAF }, { "gui", AB_GUI }, { "fps", AB_FPS },
};
#define GPUS_FILE  "tagpu_vk.gpus"      /* the cache the menu reads at attach */
#define CFG_FILE   "tagpu_vk.cfg"       /* the player's choice, by name       */
#define CFG_TMP    "tagpu_vk.cfg.tmp"
#define GPUS_TMP   "tagpu_vk.gpus.tmp"
#define POLL_MS    250                  /* the lever, as elsewhere in the fork */
#define MAXDEV     TAGPU_VK_MAXGPU
/* The frames in flight, and the bound on every per-image array here. Defined
   in tagpu_vk_pass.h because a ported pass keeps one buffer set per slot and
   is handed the slot index -- two files with their own 8 would drift. */
#define MAXIMG     TAGPU_VK_SLOTS
/* THE NAME WE KEEP IS NOT THE NAME THE DRIVER GAVE US. `deviceName` is up to
   VK_MAX_PHYSICAL_DEVICE_NAME_SIZE (256) bytes of whatever the ICD chose, and
   it ends up in three places that are not free-form: the generated `.GUI`,
   where `|` separates a stage button's captions and `;` ends a field; the
   line-per-device cache file; and a 120 px control. So every name entering
   this module goes through `vk_canon` -- bounded, and stripped of every byte
   that could change the meaning of a file we write. The canonical form is what
   is cached, what is stored as the choice AND what the bind compares against,
   so the three can never disagree. */
#define NAMELEN    TAGPU_VK_NAMELEN

enum { ST_OFF = 0, ST_STARTING = 1, ST_READY = 2, ST_FAILED = 3, ST_ZOMBIE = 4 };

static void vklog(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    FILE* f;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "vk: %s\n", b); fclose(f); }
}

static int exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

/* The canonical form of a device name: printable ASCII only, with the bytes
   that are syntax in a `.GUI` or in the cache file mapped to '_', trimmed, and
   truncated to NAMELEN-1. Idempotent, so running it on a name read back out of
   the cache yields the same string. */
static void vk_canon(char* dst, unsigned cap, const char* src)
{
    unsigned n = 0;
    if (!cap) return;
    for (; *src && n + 1 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c < 0x20 || c > 0x7E) c = '_';
        else if (c == '|' || c == ';' || c == '=' || c == '{' || c == '}' ||
                 c == '[' || c == ']') c = '_';
        if (c == ' ' && n == 0) continue;          /* no leading blank */
        dst[n++] = (char)c;
    }
    /* A TRUNCATION CUTS AT A WORD BOUNDARY. `llvmpipe (LLVM 20.1.2, 256 bits`
       is what a straight cut produced, and a name that ends mid-token reads as
       a corrupt string rather than a shortened one. Only when the source was
       actually longer than the bound, and only when a space survives in the
       second half of the result -- otherwise one very long word would trim to
       nothing. */
    if (*src) {
        unsigned k = n;
        while (k && dst[k - 1] != ' ') k--;
        if (k > n / 2) n = k;
    }
    while (n && dst[n - 1] == ' ') n--;            /* nor a trailing one */
    dst[n] = 0;
}

static const char* res_name(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    default: return "(other)";
    }
}

/* ---- the entry points ----------------------------------------------------
   Resolved through vkGetInstanceProcAddr / vkGetDeviceProcAddr rather than
   linked, so a machine with no Vulkan degrades to "the lane does not arm"
   instead of failing to load the DLL. */
static HMODULE  s_mod;
static PFN_vkGetInstanceProcAddr s_gipa;

#define IFNS(X) \
    X(vkCreateInstance) X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceProcAddr)

#define DFNS(X) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) \
    X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCreateSemaphore) \
    X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) \
    X(vkAcquireNextImageKHR) X(vkQueueSubmit) X(vkQueuePresentKHR) \
    X(vkWaitForFences) X(vkResetFences) X(vkResetCommandBuffer) \
    X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory)

#define DECL(n) static PFN_##n n;
IFNS(DECL)
DFNS(DECL)
#undef DECL

/* ---- the live objects ----------------------------------------------------
   ONE OWNER AT A TIME, decided by s_state (see the header comment). */
typedef struct {
    VkInstance       inst;
    VkPhysicalDevice pd;
    VkDevice         dev;
    VkQueue          queue;
    VkSurfaceKHR     surf;
    VkSwapchainKHR   sc;
    VkImage          img[MAXIMG];
    VkCommandPool    pool;
    VkCommandBuffer  cmd[MAXIMG];
    /* G19d: what a pass draws into. THE RENDER PASS IS PER DEVICE AND THE
       FRAMEBUFFERS ARE PER SWAPCHAIN -- a resize rebuilds the views and the
       framebuffers and keeps the render pass, so a pipeline a pass built
       against it stays valid across every resize. The format is checked on
       every rebuild rather than assumed (`vk_swapchain`). */
    VkRenderPass     rp;
    VkImageView      view[MAXIMG];
    VkFramebuffer    fb[MAXIMG];
    /* G19e: THE DEPTH ATTACHMENT, one per swapchain image and not one shared.
       `nimg` frames are in flight at once and each has a framebuffer of its
       own, so a single depth buffer would be written by two frames that
       overlap on the GPU -- and the whole point of the seam's fence is that
       slot i's resources are free when slot i comes round, which is a
       statement about per-slot objects. It is cleared by the render pass's
       loadOp and never stored, so nothing is carried between frames. */
    VkFormat         dfmt;                 /* UNDEFINED = no depth attachment */
    VkImage          dimg[MAXIMG];
    VkDeviceMemory   dmem[MAXIMG];
    VkImageView      dview[MAXIMG];
    VkSemaphore      semAcquire[MAXIMG];   /* by FRAME index                  */
    VkSemaphore      semRelease[MAXIMG];   /* by IMAGE index -- see vk_present */
    VkFence          fence[MAXIMG];
    /* tagpu_ftime (G19f landing 6): two timestamps a frame, slot i at 2i and
       2i+1. READ BEHIND THE FENCE THIS SEAM ALREADY WAITS ON before it
       re-records that slot -- so the results are complete by construction and
       cost no wait of their own. `tsPend` says a slot has a pair worth reading;
       it is cleared when read and when the lane comes down. */
    VkQueryPool      tsPool;
    char             tsPend[MAXIMG];
    double           tsPeriod;       /* ns per tick, 0 = no timestamps here   */
    uint32_t         qfam;
    uint32_t         nimg;
    VkFormat         fmt;
    VkColorSpaceKHR  cspace;
    VkExtent2D       ext;
    HWND             hwnd;
    int              vsync;
    int              devIndex;             /* into the cached name table, or -1 */
    int              flipok;               /* VK_KHR_maintenance1 was enabled   */
    int              lineok;               /* VK_EXT_line_rasterization, bresenham */
    int              zclipok;              /* VK_EXT_depth_clip_control, -1..1 z */
    /* samplerAnisotropy, a CORE feature bit rather than an extension, and the
       largest ratio this device will apply. The Classic++ restored twins are
       the only textures this fork filters at all, and tagpu_gaf.c asks GL for
       4x on them -- so a lane without this draws minified restored art
       differently from its own oracle. */
    int              anisook;
    float            maxAniso;
    int              wideok;
    float            maxLineWidth;
    int              rebuild;              /* the surface said its extent moved */
    int              cansrc;               /* the images carry TRANSFER_SRC     */
    unsigned         frame;
    char             devName[NAMELEN];
} Vk;

static Vk  s_vk;
static volatile LONG s_state;
static HANDLE s_worker;
static struct { HWND hwnd; int w, h, vsync; } s_want;

/* WHAT A PORTED PASS IS HANDED (tagpu_vk_pass.h), filled in by `up_worker` and
   read by the render thread in ST_READY -- the same ownership rule as `s_vk`,
   because it is a view of `s_vk` and nothing more. Nothing in it names a
   window, a surface or a swapchain: that is standing constraint 3 holding
   rather than being asserted. */
static TAGPU_VKPASS s_pass;

/* The passes log through the lane's log, so one file carries the whole lane's
   story in order. */
static void passlog(const char* m) { vklog("%s", m); }

/* THE CAPTURE IS COMPLETED BY THE SEAM'S OWN FENCE, AND ADDS NO WAIT AT ALL.
   [REWRITTEN FROM REVIEW 2026-09-15.]

   The first shape waited on the frame's fence right after the present, for up
   to a second, and on a TIMEOUT it destroyed the staging buffer -- which the
   already-submitted `vkCmdCopyImageToBuffer` writes into. The GPU would then
   have completed the copy into freed memory, and the one-second timeout was
   load-bearing for that rather than being the belt it claimed to be. A wait on
   the render thread was also the one thing this file spent a whole review
   removing: the game thread waits INFINITE on the render thread across a mode
   change.

   So nothing waits. The capture records into slot `fi` and says so, and the
   NEXT frame that reaches that slot finds the fence already waited on at the
   top of `vk_present` -- which is the proof, by construction and with no second
   mechanism, that the copy has completed. `nimg` frames later, which at any
   frame rate the lane runs at is a few milliseconds. */
static int s_abSlot1;       /* 0 = nothing pending, else the slot index + 1 */
static const char* s_abPath;/* the file the pending capture belongs in         */

/* Give the staging buffer back at a point where the device is idle. The FILE is
   not written here -- a lane that is coming down or rebuilding its swapchain
   mid-capture is not evidence of anything, and `tools/vk-ab.py` says loudly that
   one of the pair is missing.

   `idle` IS THE WHOLE SAFETY ARGUMENT, SO IT IS THE CALLER'S RESULT AND NOT AN
   ASSUMPTION. [FROM THE REVIEW'S SECOND PASS 2026-09-15.] `vkDeviceWaitIdle`
   can fail -- `VK_ERROR_OUT_OF_HOST_MEMORY` above all, in a 32-bit address space
   this lane spends its budget measuring -- and it then returns WITHOUT the
   device being idle. Freeing a buffer the submitted `vkCmdCopyImageToBuffer` is
   still writing into would be the same defect this whole mechanism was rewritten
   to remove, one call further along. So a wait that did not succeed LEAKS the
   buffer instead: a leak is recoverable and a free is not, which is the same
   trade `ST_ZOMBIE` makes two hundred lines up. It is once per process at worst,
   it is logged, and `tagpu_vk_shot_record` then refuses further captures for the
   session because the buffer is still there.

   The rest of `vk_down`'s teardown rests on that same wait and always has --
   the swapchain, the per-image objects, the device itself. That is pre-existing
   and is named in [gpu-status] §2.26 rather than changed here. */
static void ab_drop(const char* why, int idle)
{
    if (!s_abSlot1) return;
    s_abSlot1 = 0;
    if (!idle) {
        vklog("the A/B capture was lost to %s AND the device would not go idle - "
              "its readback buffer is leaked rather than freed under a copy that "
              "may still be running", why);
        return;
    }
    vklog("the A/B capture was lost to %s - no tagpu_<pass>_vk.ppm this arming "
          "(the arming unlinked it, so there is no stale one to mistake for it)", why);
    tagpu_vk_shot_down(&s_pass);
}


/* The clear colour, `color=r,g,b` in the lever file. Magenta by default: no
   pixel of TA's palette is pure magenta, so "is the Vulkan lane on screen?" is
   answered by looking rather than by measuring. */
static float s_clear[3] = { 1.0f, 0.0f, 1.0f };

/* ---- the window the surface goes on ---------------------------------------
   THERE IS ONE, AND IT IS THE GAME'S. [The vulkan-only plan, landing 4d-1.]

   ROUTE D IS GONE. Until this landing the lane could also run BESIDE the GL
   backend, presenting into an owned popup of its own placed over the game
   window's client area -- a whole apparatus (a window class, a window proc, a
   create/destroy handshake posted to the thread that pumps, and a
   `WM_WINDOWPOSCHANGED` follow to keep the popup on the client rect) that
   existed for exactly one reason, stated in tagpu_vk.h: *"two backends must not
   both present to one window in one frame"*.

   THAT REASON NO LONGER EXISTS, because there is no longer a second backend to
   present. `renderer=vulkan` reaches `render_vk.c`, which calls
   `tagpu_vk_own_present()` before its loop, and nothing else drives this lane:
   `render_ogl.c`'s own call to `tagpu_vk_frame` went with the popup. So the
   surface goes on the window the caller names and `s_ownWin` is set on every
   frame that gets here.

   WHAT THIS COST, AND IT WAS PAID DELIBERATELY. Route D was also the project's
   ORACLE: both backends rendering the same frame, each capturing its own half,
   `tools/vk-ab.py` diffing the two. No absolute two-lane comparison is
   expressible after this, so every figure that wanted one was taken first --
   all five world passes at the shipped `ss = 2` in landing 4c-3
   ([gpu-status](gpu-status.html) §2.54), the UI layer at 0 px of 307 200, and
   the `ss = 1` set before them. From here a claim is relative: this build
   against the previous one.

   WHAT SURVIVES. `tagpu_vk_wndproc` keeps its name and its call from the fork's
   wndproc, reduced to the one arm that was never about a window of ours -- the
   OWNER dying, which is the only thing the window thread can tell the render
   thread that `hwnd != s_owner` cannot (see `s_ownGone`). */

static HWND          s_owner;           /* the game window we are tracking     */


/* THIS BACKEND OWNS THE PRESENT, and since landing 4d-1 there is no other kind
   of frame: `render_vk.c` sets this before its loop and it is the only driver
   of this lane. The latch is kept rather than collapsed away -- the plan scopes
   4d to the window, and turning every `tagpu_vk_owns_present()` test in the
   tree into a constant is a far larger change than deleting a popup.

   IT IS A ONE-WAY LATCH, set once from `vk_render_main` before its loop and
   never cleared: which backend the process has is decided at `dd.c`'s dispatch
   and cannot change afterwards.

   INTERLOCKED BECAUSE THREE THREADS READ IT: the render thread writes it, the
   bring-up worker reads it for its two log lines, and the game/window thread
   reads it in `tagpu_vk_wndproc` and in `tagpu_vk_armed`. A plain `int` there
   is an unsynchronised cross-thread read, which is not an ordering however
   early the write happens. [FROM THE LANDING REVIEW, 2026-09-17.] */
static volatile LONG s_ownWin;          /* this backend owns the present       */

/* THE OWNER IS GONE, published by the window thread, and it is the ONLY thing
   that detector can be now. `hwnd != s_owner` does not cover it -- `g_ddraw.hwnd`
   is nulled only by the IAT-hooked `DestroyWindow` (winapi_hooks.c), and only
   AFTER the real destroy, so a destroy by any other route leaves that test false
   and the lane presenting on a dead HWND until the driver says
   `VK_ERROR_SURFACE_LOST_KHR`. That would be trading an ordering for "wait for
   an error", which is the shape this file is not allowed to ship, so the latch
   keeps a one-frame bound on it. It is why `tagpu_vk_wndproc` still exists at
   all after 4d-1. [FROM THE LANDING REVIEW, 2026-09-17.] */
static volatile LONG s_ownGone;

/* DID THE UI LAYER ACTUALLY COMPOSITE THIS FRAME? Cleared at the top of
   `tagpu_vk_frame` -- so every one of its early returns (the lever off, a
   bring-up, a rebuild, ST_FAILED, ST_ZOMBIE, an out-of-date acquire) leaves it
   0, which is the truth -- and set only where `tagpu_vk_gui_record` is actually
   called.

   IT EXISTS FOR THE CURSOR, and the gap it closes is `s_curDrew`'s.
   `render_vk.c` has to tell `tagpu_cursown` whether OUR cursor reached the
   screen, because the engine's own cursor blit stands down on that. The answer
   it had was `tagpu_gui_cursor_drew_take()`, which says the mirror RECORD
   survived to `draw_layer`'s tail -- and `tagpu_vk_gui_prepare` can still refuse
   the frame after that (`s_behind`, `!s_engHave`, `!s_palHave`, a presented twin
   that stood down or resized, either shader-refusal path). On those frames
   nothing composited and the engine's cursor had already been suppressed.

   The fix is an ORDERING and not a move: the publish stays on the path every
   iteration reaches, but happens AFTER the frame it reports, with this as the
   second half of its answer. [The vulkan-only plan, gate 4's last item.] */
static int s_uiDrew;

/* Called from the fork's wndproc on the game window's thread. An OBSERVER: it
   never swallows a message the fork or the engine needs, and since landing 4d-1
   it watches exactly one thing -- the game window being destroyed. That is the
   only fact the window thread has which the render thread cannot get for
   itself; see `s_ownGone` for why asking `hwnd != s_owner` instead does not
   work. Everything else this used to do created, placed or destroyed route D's
   popup, and there is no popup. */
void tagpu_vk_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    (void)hwnd; (void)wparam; (void)lparam;
    /* AND ONLY WHEN THERE IS A LANE TO BRING DOWN. [FROM THE 4d-1 LANDING
       REVIEW.] The `s_ownWin` test used to sit in front of the whole switch and
       went with it; without it this appended a line naming a lane and a render
       thread that do not exist to every `tagpu.log` on the DEFAULT renderer,
       with no Vulkan lever present at all. It also restores the symmetry the
       two readers of `s_ownGone` already have. */
    if (msg != WM_DESTROY || !s_ownWin) return;
    InterlockedExchange(&s_ownGone, 1);
    vklog("window: the game window is being destroyed - the lane comes down on "
          "the render thread's next frame");
}

/* ---- the lever ----------------------------------------------------------- */
static volatile LONG s_armed = -1;      /* -1 = never polled */
static DWORD s_lastPoll;
/* THE A/B'S FRAME IS NOT DECIDED HERE (G19d). `tagpu_fps.c` polls the lever,
   captures its own half and hands the flag over with the vertices; this file
   only carries it through. Both lanes polling for themselves was the first
   shape, and it was wrong: they poll on different cadences (that one every 30
   frames, this one every 250 ms), so the two captures could land hundreds of
   frames apart -- and the readout changes its number twice a second, so they
   would have differed in the digits and agreed about nothing else. */
static void read_lever(void)
{
    char b[128];
    HANDLE h;
    DWORD n = 0;
    /* THE LEVER RETIRES INTO `renderer=` when this backend owns the present:
       `renderer=vulkan` that a stray `tagpu_vk.off` could disarm would leave
       the process with NO renderer and a black window, which is a worse
       failure than anything the lever was there to protect against. The ON
       file is still READ below for its `color=`, which every A/B on this plan
       uses; it just no longer decides whether the lane runs. */
    int on = s_ownWin ? 1 : (exists(ON_FILE) && !exists(OFF_FILE));

    InterlockedExchange(&s_armed, on);
    if (!on) return;

    h = CreateFileA(ON_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, b, sizeof b - 1, &n, 0)) n = 0;
    CloseHandle(h);
    b[n] = 0;
    {
        const char* p = strstr(b, "color=");
        int r, g, bl;
        if (p && sscanf(p + 6, "%d,%d,%d", &r, &g, &bl) == 3) {
            s_clear[0] = (float)(r < 0 ? 0 : r > 255 ? 255 : r) / 255.0f;
            s_clear[1] = (float)(g < 0 ? 0 : g > 255 ? 255 : g) / 255.0f;
            s_clear[2] = (float)(bl < 0 ? 0 : bl > 255 ? 255 : bl) / 255.0f;
        }
    }
}

/* UNLINK THIS PASS'S VULKAN CAPTURE, called by the pass at the instant it
   latches a claim -- `if (taking) tagpu_vk_ab_arm("scaffold");` and nothing
   else. That placement IS the guarantee, and it is why this is not done where
   the capture is recorded.

   WHAT IT BUYS, AND IT IS NOW THE WHOLE OF THE GUARANTEE. While route D
   existed the rule was that a capture may be claimed only on a GL half that
   reached the disk, because a refused write leaves the PREVIOUS run's file
   lying there and a half diffed against it reports a capture of a different
   frame as a port failure. Landings 4d-1 and 4d-2 deleted the GL half, so that
   property rests entirely on this call: after it the target does not exist, and
   it comes back only if `tagpu_vk_shot_finish` writes it. Absent means this
   arming produced no capture; present means this arming's -- which is exactly
   what a cross-BUILD diff needs to be trustworthy.

   WHY HERE AND NOT IN `vk_present`. Because the claim is LATCHED on the gather
   side (`s_abDone = 1`) whether or not the lane ever collects it, and
   `vk_present` is not on the path from that latch: `tagpu_vk_frame` returns
   before it all through the ~250 ms bring-up, on a swapchain rebuild, at
   ST_FAILED and at ST_ZOMBIE, and `vk_present` itself returns early on an
   out-of-date acquire and on a swapchain that hands back an impossible image.
   An unlink there is reached on most frames and missed on precisely the frames
   where the capture does not happen -- which is the case it exists for. Here it
   is in the same statement sequence as the latch, on the same thread, with no
   frame boundary between them, so there is no interleaving that separates them.
   [FROM THE 4b-1 LANDING REVIEW, 2026-09-18.]

   THE ONE RESIDUAL, NAMED. A capture recorded by an EARLIER arming is written
   when its frame slot's fence comes round, up to `nimg` frames later, and that
   write would land after this unlink. It cannot overlap a second arming of the
   same pass: a pass re-arms only after its lever file has been taken away and
   put back, and it notices that on a 30-frame poll, which is an order of
   magnitude more frames than the swapchain has images. That is a bound, not a
   race, but it is a bound in two files and so it is written down in both.

   AND THE DELETE'S ANSWER IS THE RETURN VALUE, because "after this line the
   file does not exist" is the whole argument and `DeleteFileA` can fail: a
   reader holding it open, or a read-only file. Discarding that would leave the
   property asserted rather than established, so 1 means the target is gone --
   deleted now, or already absent -- and the caller must not claim the Vulkan
   half on anything else. ERROR_FILE_NOT_FOUND and ERROR_PATH_NOT_FOUND are the
   success cases that look like failures. [FROM THE 4b-1 LANDING REVIEW.]

   An unknown tag is a programming error and says so rather than composing a
   path: nothing should be unlinked on the strength of a string this file does
   not recognise, and no claim is granted for one either. */
int tagpu_vk_ab_arm(const char* tag)
{
    int i;
    DWORD e;
    if (!tag) return 0;
    for (i = 0; i < 8; i++) {
        if (strcmp(tag, s_abFiles[i].tag) != 0) continue;
        if (DeleteFileA(s_abFiles[i].path)) return 1;
        e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return 1;
        vklog("ab: %s could not be removed (error %lu) - this arming is REFUSED "
              "rather than risk the file that is there being diffed as its "
              "capture. Close whatever is holding it and re-arm.",
              s_abFiles[i].path, (unsigned long)e);
        return 0;
    }
    vklog("ab: \"%s\" is not one of the eight ported passes - nothing unlinked "
          "and no claim granted", tag);
    return 0;
}

/* THE RENDERER CHOICE IS THE ARMING when this backend owns the present. Called
   once, from the render thread, before the frame loop. See `s_ownWin`. */
void tagpu_vk_own_present(void)
{
    InterlockedExchange(&s_ownWin, 1);
    /* AND THE "OWNER GONE" LATCH IS CLEARED HERE, once per render thread. It
       must not outlive the thread that saw the destroy: `g_ddraw.hwnd` is
       assigned again on a mode change (dd.c, utils.c), so a latch that stayed
       set would refuse every bring-up for the rest of the process -- the lane
       silently never coming up, which is the failure this file exists to make
       impossible. This function runs once at the top of each render thread,
       which is exactly the scope the latch wants. */
    InterlockedExchange(&s_ownGone, 0);
    vklog("this backend owns the present - the surface goes on the game window");
}

/* 1 when this backend owns the present, i.e. there is no GL context in the
   process. THE PREDICATE LANDING 4b IS BUILT ON: every GL draw in the tree is
   gated on its negation, one pass per commit, while the gather half beside it
   runs unconditionally. Safe from any thread -- the latch is interlocked. */
int tagpu_vk_ui_composited(void) { return s_uiDrew; }

int tagpu_vk_owns_present(void)
{
    return s_ownWin != 0;
}

int tagpu_vk_armed(void)
{
    /* ASKED FROM THE GAME THREAD (`vrow_greyed`, when the options screen is
       built) and therefore a PURE READ: it must not call `read_lever`, which
       writes `s_clear` -- the render thread's. A file attribute query costs
       nothing at the rate a screen is built, and sharing no mutable state is
       worth more than the cached answer.

       AND IT ANSWERS FOR THE OWNING BACKEND TOO, which it did not until the
       landing review found what that cost [2026-09-17]. `renderer=vulkan`
       needs no lever file, so this returned 0 in exactly the configuration
       where the lane is the ONLY renderer -- and `tagpu_menu.c`'s
       `vrow_greyed(VD_GPU)` greys the GPU picker on it. The row whose whole
       purpose is choosing the Vulkan device was dead in the only mode where
       Vulkan draws, which inverts the rule that row is greyed by: it looked
       dead while it was the one thing that could bite.

       It also re-arms the GL halves' mirror latches (`tagpu_fx.c`,
       `tagpu_feat.c`, `tagpu_terr.c`, `tagpu_posebake.c`, `tagpu_posedraw.c`),
       which landing 4b needs and which are inert in 4a only because nothing
       calls the gather halves yet. */
    return s_ownWin || (exists(ON_FILE) && !exists(OFF_FILE));
}

/* ---- G19b: the cached name table -----------------------------------------
   Written once by the enumeration worker, read by the menu on the game thread
   and by the bring-up worker. `s_count` is published LAST with an interlocked
   store, so a reader that sees a non-zero count sees the names behind it -- the
   same ordering the state machine uses. */
static char  s_name[MAXDEV][NAMELEN];
static int   s_disc[MAXDEV];            /* 1 = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU */
static volatile LONG s_count;
/* THE ONLY THING THAT CROSSES A THREAD HERE IS AN ALIGNED LONG, and that is
   deliberate. An earlier shape had the row write a device NAME that the
   bring-up worker and the deferred writer then read; a string written by one
   thread and read by another can be read half-copied, and the half-copy would
   have been PERSISTED to the cfg. There is no such string now:

     s_name[] / s_choice[]  written once, `tagpu_vk_names_init` at DLL attach,
                            before any other thread exists; read-only after.
     s_choiceIdx            the row's request, an INDEX into s_name[]. A
                            naturally aligned LONG cannot tear, and it is
                            BOUNDED against s_count before it indexes anything.
     s_choiceGen            bumped by InterlockedIncrement AFTER s_choiceIdx is
                            written, so a reader that sees the new generation
                            sees the new index. The render thread rebuilds
                            whenever the generation differs from the one the
                            live device was built for, so the LAST click always
                            gets a rebuild after it -- convergence by
                            construction, not by the click being slow. */
static char  s_choice[NAMELEN];         /* the STORED choice, from the cfg; "" = none */
static volatile LONG s_choiceIdx = -1;  /* the row's request, into s_name[]; -1 = none */
static volatile LONG s_choiceGen;       /* bumped by a click: the render thread's signal */
static LONG  s_choiceSeen;              /* the generation the live device was built for */
static volatile LONG s_activeIndex = -1;

/* The name the lane should bind: the row's request when there is one, else
   whatever the cfg stored, else "" for "no preference". Both sources are
   immutable, so this can be called from any thread. */
static const char* want_name(void)
{
    LONG i = s_choiceIdx;
    if (i >= 0 && i < s_count) return s_name[i];
    return s_choice;
}

void tagpu_vk_names_init(void)
{
    char buf[MAXDEV * (NAMELEN + 8) + 64];
    HANDLE h;
    DWORD n = 0;
    char* p;
    int count = 0;

    /* the stored choice first: a plain `gpu=<name>` line, and an absent file
       means "nobody has chosen" rather than an error */
    h = CreateFileA(CFG_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        char c[NAMELEN + 16];
        DWORD m = 0;
        if (!ReadFile(h, c, sizeof c - 1, &m, 0)) m = 0;
        CloseHandle(h);
        c[m] = 0;
        if (!strncmp(c, "gpu=", 4)) {
            char* q = c + 4;
            char* e = q;
            while (*e && *e != '\r' && *e != '\n') e++;
            *e = 0;
            vk_canon(s_choice, sizeof s_choice, q);
        }
    }

    h = CreateFileA(GPUS_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, buf, sizeof buf - 1, &n, 0)) n = 0;
    CloseHandle(h);
    buf[n] = 0;

    /* one device per line: `<0|1> <name>`, the flag being DISCRETE_GPU. The
       name may contain spaces, so the flag leads and the rest of the line is
       the name -- no field the driver controls can shift the parse. */
    p = buf;
    while (*p && count < MAXDEV) {
        char* e = p;
        while (*e && *e != '\n') e++;
        if (*e) *e++ = 0;
        {
            char* q = p;
            while (*q == ' ' || *q == '\t') q++;
            if ((*q == '0' || *q == '1') && (q[1] == ' ' || q[1] == '\t')) {
                s_disc[count] = (*q == '1');
                q += 2;
                while (*q == ' ' || *q == '\t') q++;
                {
                    char* t = q + strlen(q);
                    while (t > q && (unsigned char)t[-1] <= ' ') *--t = 0;
                }
                /* canonicalised on the way IN as well as on the way out: the
                   cache is a file on disk and may have been edited by hand */
                vk_canon(s_name[count], NAMELEN, q);
                if (s_name[count][0]) count++;
            }
        }
        p = e;
    }
    InterlockedExchange(&s_count, count);
}

int tagpu_vk_gpu_count(void) { return (int)s_count; }

const char* tagpu_vk_gpu_name(int i)
{
    return (i >= 0 && i < (int)s_count) ? s_name[i] : "";
}

int tagpu_vk_gpu_default(void)
{
    int i;
    for (i = 0; i < (int)s_count; i++) if (s_disc[i]) return i;
    return 0;
}

int tagpu_vk_gpu_stored(void)
{
    int i;
    LONG k = s_choiceIdx;
    if (k >= 0 && k < s_count) return (int)k;
    if (s_choice[0])
        for (i = 0; i < (int)s_count; i++)
            if (!lstrcmpiA(s_name[i], s_choice)) return i;
    return tagpu_vk_gpu_default();
}

int tagpu_vk_gpu_active(void) { return (int)s_activeIndex; }

void tagpu_vk_gpu_select(int i)
{
    if (i < 0 || i >= (int)s_count) return;
    InterlockedExchange(&s_choiceIdx, i);
    /* THE GENERATION IS BUMPED SECOND AND WITH A BARRIER, so a thread that sees
       it has the index behind it. A counter rather than a flag: the render
       thread compares it against the one the live device was built for, so a
       click that lands while a rebuild is already in flight is not lost. */
    InterlockedIncrement(&s_choiceGen);
}

void tagpu_vk_gpu_store(void)
{
    HANDLE h;
    char b[NAMELEN + 16];
    DWORD wrote = 0, len;

    len = (DWORD)_snprintf(b, sizeof b, "gpu=%s\r\n", want_name());
    if ((int)len <= 0) return;

    /* written to a temporary and renamed over the target, for the reason
       tagpu_menu.c's write_cfg states: CREATE_ALWAYS truncates first, and a
       reader must never see the stub. */
    h = CreateFileA(CFG_TMP, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { vklog("cannot write " CFG_TMP); return; }
    if (!WriteFile(h, b, len, &wrote, 0) || wrote != len) {
        CloseHandle(h); DeleteFileA(CFG_TMP);
        vklog("short write to " CFG_TMP " - " CFG_FILE " left as it was");
        return;
    }
    CloseHandle(h);
    if (!MoveFileExA(CFG_TMP, CFG_FILE, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(CFG_TMP);
        vklog("cannot replace " CFG_FILE " - left as it was");
    }
}

/* ---- address-space cost, the G19a exit's second half ---------------------
   Peak committed bytes and the largest free VA block. The second number is the
   one that decides whether a 32-bit TA survives a driver in its address space:
   the engine's own allocator fails by failing, not by saying anything. */
/* GetProcessMemoryInfo IS RESOLVED AND NEVER IMPORTED. Linking it pulls in
   `K32GetProcessMemoryInfo`, which kernel32 only exports from Windows 7 -- and
   a hard import the loader cannot satisfy does not degrade, it stops the DLL
   loading at all, on exactly the old machines this fork exists to serve. It
   lives in psapi.dll on everything older. Unavailable = the peak reads 0; the
   VA walk below is `VirtualQuery` and is there on every Windows. */
typedef struct { DWORD cb; DWORD PageFaultCount; SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize; SIZE_T QuotaPeakPagedPoolUsage; SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage; SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage; SIZE_T PeakPagefileUsage; } VK_PMC;
typedef BOOL (WINAPI *gpmi_fn)(HANDLE, VK_PMC*, DWORD);

static double peak_committed_mb(void)
{
    static gpmi_fn fn;
    static int tried;
    VK_PMC pmc;

    if (!tried) {
        tried = 1;
        fn = (gpmi_fn)(void*)real_GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                 "K32GetProcessMemoryInfo");
        if (!fn) {
            HMODULE m = real_LoadLibraryA("psapi.dll");
            if (m) fn = (gpmi_fn)(void*)real_GetProcAddress(m, "GetProcessMemoryInfo");
        }
    }
    if (!fn) return 0.0;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (!fn(GetCurrentProcess(), &pmc, sizeof pmc)) return 0.0;
    return (double)pmc.PeakPagefileUsage / 1048576.0;
}

static void va_log(const char* when)
{
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T addr = 0, largest = 0, freetotal = 0;
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    while (addr < (SIZE_T)si.lpMaximumApplicationAddress &&
           VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_FREE) {
            freetotal += mbi.RegionSize;
            if (mbi.RegionSize > largest) largest = mbi.RegionSize;
        }
        if (mbi.RegionSize == 0) break;
        addr += mbi.RegionSize;
    }
    vklog("VA %s: committed-peak %.1f MB, free %.1f MB, largest free block %.1f MB",
          when, peak_committed_mb(),
          (double)freetotal / 1048576.0, (double)largest / 1048576.0);
}

/* ---- loading the runtime -------------------------------------------------
   real_LoadLibraryA, never LoadLibraryA: the fork hooks the latter (hook=4) and
   the hook re-scans the module tree, which is not something to trigger from
   under a driver's own loader work. */
/* THE CACHE TEST IS THE LAST THING RESOLVED, NOT THE FIRST. [FROM REVIEW
   2026-09-15, second pass.] It used to be `if (s_mod) return 1;` -- and `s_mod`
   is set two lines before the two resolutions that can still fail, so a
   `vulkan-1.dll` that loads but does not resolve reported FAILURE to the
   enumeration worker and SUCCESS to the bring-up worker that followed it,
   which then called through a NULL `vkCreateInstance`. Serialising the workers
   removed the race between them; it did not remove the cache's own lie. */
static int vk_load(void)
{
    if (vkCreateInstance) return 1;
    if (!s_mod) {
        s_mod = real_LoadLibraryA("vulkan-1.dll");
        if (!s_mod) { vklog("vulkan-1.dll did not load - no Vulkan runtime is installed"); return 0; }
    }
    if (!s_gipa) {
        s_gipa = (PFN_vkGetInstanceProcAddr)(void*)real_GetProcAddress(s_mod, "vkGetInstanceProcAddr");
        if (!s_gipa) { vklog("vulkan-1.dll has no vkGetInstanceProcAddr"); return 0; }
    }
    vkCreateInstance = (PFN_vkCreateInstance)s_gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) { vklog("the loader has no vkCreateInstance"); return 0; }
    return 1;
}

/* An instance with the two surface extensions. Shared by the enumeration worker
   and the bring-up worker; each destroys its own. */
static VkInstance vk_instance(void)
{
    const char* iexts[3] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                             NULL };
    uint32_t niext = 2;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r;

    /* VK_KHR_get_physical_device_properties2, WHEN THE LOADER HAS IT, AND IT IS
       A DEPENDENCY RATHER THAN A WANT. This instance asks for Vulkan 1.0, where
       VK_EXT_line_rasterization -- which the effects pass needs for GL's own
       line rule (G19e) -- depends on it, and chaining
       VkPhysicalDeviceLineRasterizationFeaturesEXT into VkDeviceCreateInfo is
       only defined with it enabled. The reference setup's loader tolerates the
       omission, which is exactly why it went unnoticed: a stricter one refuses
       vkCreateDevice, the retry there clears `lineok`, and every frame with a
       laser in it is refused for the session ON HARDWARE THAT SUPPORTS THE
       FEATURE. [FROM THE G19e EFFECTS REVIEW, 2026-09-15.]

       ASKED FOR ONLY WHEN IT IS OFFERED: an instance extension the loader does
       not have fails vkCreateInstance outright, which would cost the whole lane
       to buy one pass's lines. Resolved at GLOBAL level (a NULL instance),
       because this runs before there is one. */
    {
        PFN_vkEnumerateInstanceExtensionProperties eiep =
            (PFN_vkEnumerateInstanceExtensionProperties)
                s_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
        if (eiep) {
            uint32_t ne = 0, k;
            VkResult er = eiep(NULL, &ne, NULL);
            VkExtensionProperties* ext = NULL;
            if ((er == VK_SUCCESS || er == VK_INCOMPLETE) && ne)
                ext = (VkExtensionProperties*)malloc((size_t)ne * sizeof *ext);
            if (ext) {
                er = eiep(NULL, &ne, ext);
                if (er == VK_SUCCESS || er == VK_INCOMPLETE)
                    for (k = 0; k < ne; k++)
                        if (!strcmp(ext[k].extensionName,
                                    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
                            iexts[niext++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
                            break;
                        }
                free(ext);
            }
        }
    }

    app.pApplicationName = "Total Annihilation (impure)";
    app.apiVersion = VK_API_VERSION_1_0;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iexts;
    r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { vklog("vkCreateInstance: %s (%d)", res_name(r), (int)r); return VK_NULL_HANDLE; }

#define RES(n) n = (PFN_##n)s_gipa(inst, #n);
    IFNS(RES)
#undef RES
    if (!vkEnumeratePhysicalDevices || !vkCreateWin32SurfaceKHR || !vkCreateDevice ||
        !vkGetDeviceProcAddr || !vkDestroyInstance) {
        vklog("instance entry points missing - the loader is not usable");
        if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
        return VK_NULL_HANDLE;
    }
    return inst;
}

/* ST_ZOMBIE IS A WORKER WINDING DOWN, NOT A GRAVE, and this is what clears it.
   [FROM REVIEW 2026-09-15.]

   A worker that finishes and finds the lane is no longer ST_STARTING has been
   ABANDONED by `tagpu_vk_render_stop` -- but it is still the sole owner of
   everything it built, because ST_ZOMBIE is the state in which nobody else
   touches any of it. So it can put its own objects back, which the previous
   design could not: that one LEAKED them and made ST_ZOMBIE terminal for the
   session. Handing the lane back to ST_OFF here is what lets a later render
   thread bring it up again.

   The transition out of ST_ZOMBIE is this function and nothing else, so the
   compare-exchange is unambiguous: whoever loses is not a second worker, it is
   a state that was never ZOMBIE. */
static void lane_release(void)
{
    if (InterlockedCompareExchange(&s_state, ST_OFF, ST_STARTING) == ST_STARTING) return;
    InterlockedCompareExchange(&s_state, ST_OFF, ST_ZOMBIE);
}

/* The state, read so that the fields behind it cannot be hoisted over it.
   `s_state` is volatile but `s_vk` is not, and a volatile load orders nothing
   about a non-volatile one -- the interlocked read is a full barrier, which is
   what the publish on the other side already is. It is what `s_choiceGen`'s
   read already does, and this one had been left plain. [FROM REVIEW 2026-09-15] */
static LONG lane_state(void)
{
    return InterlockedCompareExchange(&s_state, 0, 0);
}

/* THE DEVICE'S LARGEST 2D IMAGE, or 0 while there is no device yet.
   `GL_MAX_TEXTURE_SIZE`'s counterpart, and a ported pass needs it for the same
   reason the GL one did: an atlas is sized against the limit of the device that
   will sample it, and a pass that guessed would either waste memory or build
   something the driver refuses. Asked of the device we actually BOUND, not of
   row 0 of the enumeration -- the player can pick another.

   0 IS A REFUSAL AND NOT A DEFAULT. A caller must treat it as "not yet" and try
   again on a later frame, never as a limit: the lane takes ~200 ms to come up
   and the gathers run from the first frame, so a pass that read 0 as a bound
   would build a zero-row atlas and cache it for the life of the process. That
   is measured, not hypothetical -- it is what `terr: atlas built 2176x0 ... 0
   kept` was. [The vulkan-only plan, landing 4b-2.] */
int tagpu_vk_max_image_dim(void)
{
    /* CACHED AGAINST THE DEVICE IT CAME FROM, not just cached. The GPU picker
       tears the lane down and re-picks a physical device on a row change, and
       it is live under `renderer=vulkan` -- so a bare `static int cached` would
       keep the old device's limit for the life of the process and a smaller new
       device would have work sized past what it accepts.
       [FROM THE 4b-2 LANDING REVIEW, 2026-09-18.] */
    static int cached;
    static VkPhysicalDevice cachedFor;
    VkPhysicalDeviceProperties p;
    if (!s_vk.pd || lane_state() != ST_READY) return 0;
    if (cached > 0 && cachedFor == s_vk.pd) return cached;
    vkGetPhysicalDeviceProperties(s_vk.pd, &p);
    if (p.limits.maxImageDimension2D > 0x7FFFFFFFu) return 0;
    cached = (int)p.limits.maxImageDimension2D;
    cachedFor = s_vk.pd;
    return cached;
}

/* THE DEVICE'S LARGEST UNIFORM BUFFER RANGE, or 0 while there is no device.
   `GL_MAX_UNIFORM_BLOCK_SIZE`'s counterpart, and a ported pass wants it for the
   same reason: the pose block is a COMPILE-TIME size and both lanes only ask
   whether the device can hold it. Asked of the device we actually bound.
   0 IS A REFUSAL AND NOT A DEFAULT, exactly as above -- a caller treats it as
   "not yet" and asks again on a later frame. [The vulkan-only plan, 4b-2.] */
int tagpu_vk_max_uniform_range(void)
{
    /* CACHED AGAINST THE DEVICE IT CAME FROM, not just cached. The GPU picker
       tears the lane down and re-picks a physical device on a row change, and
       it is live under `renderer=vulkan` -- so a bare `static int cached` would
       keep the old device's limit for the life of the process and a smaller new
       device would have work sized past what it accepts.
       [FROM THE 4b-2 LANDING REVIEW, 2026-09-18.] */
    static int cached;
    static VkPhysicalDevice cachedFor;
    VkPhysicalDeviceProperties p;
    if (!s_vk.pd || lane_state() != ST_READY) return 0;
    if (cached > 0 && cachedFor == s_vk.pd) return cached;
    vkGetPhysicalDeviceProperties(s_vk.pd, &p);
    if (p.limits.maxUniformBufferRange > 0x7FFFFFFFu) return 0;
    cached = (int)p.limits.maxUniformBufferRange;
    cachedFor = s_vk.pd;
    return cached;
}


/* PUT A LANE THAT FAILED BACK TO ST_OFF so the next frame brings it up again.
   The caller owns the policy -- how many times is worth trying -- because what
   to do about a dead lane is a property of the backend and not of the seam.
   Returns 1 when the lane was in ST_FAILED and is now ST_OFF.

   WHY IT EXISTS [FROM THE LANDING REVIEW, 2026-09-17]. ST_FAILED is not only
   reachable from a bring-up that could never work: `tagpu_vk_frame` also
   publishes it when a swapchain REBUILD is refused and when `vk_present`
   returns -2, whose causes include a one-second fence or acquire timeout and
   `VK_ERROR_OUT_OF_HOST_MEMORY` -- "one second of no progress, or one
   allocation refused". While this lane was a lever beside GL, that killed the
   lane and GL kept the picture, and three separate things could bring it back.
   With the present owned, all three are gone at once, so without this a live
   lane that hiccups once after ten minutes would hand the session to software
   rendering for good. */
int tagpu_vk_retry(void)
{
    if (InterlockedCompareExchange(&s_state, ST_OFF, ST_FAILED) != ST_FAILED)
        return 0;
    vklog("the lane failed after it had come up - bringing it up again");
    return 1;
}

/* HAS THE BRING-UP GIVEN UP? A FACT, NOT A TIMEOUT. The backend that owns the
   present needs an answer it can act on, because a lane that will not come up
   leaves the player with no picture at all -- and "wait N frames and assume the
   worst" is the kind of argument this project does not ship. ST_FAILED is
   published by the worker after every path it could have succeeded on, so the
   state IS the answer.

   `tagpu_vk_gpu_active() < 0` is not this question: it reads -1 both for a lane
   that has failed and for one that is still starting, and the two want
   opposite behaviour. */
int tagpu_vk_failed(void)
{
    return lane_state() == ST_FAILED;
}

/* The GPU row's generation, read the same way and for the same reason: the
   index behind it is written first, and only a barrier keeps the two in that
   order on the reading side. */
static LONG lane_gen(void)
{
    return InterlockedCompareExchange(&s_choiceGen, 0, 0);
}

/* ---- G19b: the enumeration worker ---------------------------------------- */
static DWORD WINAPI enum_worker(LPVOID arg)
{
    VkInstance inst;
    VkPhysicalDevice pds[MAXDEV];
    uint32_t n = 0;
    char out[MAXDEV * (NAMELEN + 8) + 64];
    char name[MAXDEV][NAMELEN];
    int  disc[MAXDEV];
    int at = 0, i, changed;
    HANDLE h;
    DWORD wrote = 0;

    (void)arg;
    if (!vk_load())            { lane_release(); return 0; }
    inst = vk_instance();
    if (!inst)                 { lane_release(); return 0; }

    /* A FAILED OR EMPTY ENUMERATION LEAVES THE CACHE ALONE. [FROM REVIEW
       2026-09-15.] The result used to go unchecked, and `n = 0` then differed
       from `s_count` and counted as "the list changed" -- so one transient
       refusal would have rewritten `tagpu_vk.gpus` to nothing and taken the
       GPU row's captions away at the next launch. A list we could not read is
       not a list of no devices. */
    if (vkEnumeratePhysicalDevices(inst, &n, NULL) != VK_SUCCESS || !n) {
        vklog("no devices enumerated - " GPUS_FILE " left as it was");
        vkDestroyInstance(inst, NULL);
        lane_release();
        return 0;
    }
    if (n > MAXDEV) {
        vklog("%u devices and the row carries %d - only the first %d are offered",
              n, MAXDEV, MAXDEV);
        n = MAXDEV;
    }
    if (vkEnumeratePhysicalDevices(inst, &n, pds) != VK_SUCCESS || !n) {
        vklog("the device list would not come back - " GPUS_FILE " left as it was");
        vkDestroyInstance(inst, NULL);
        lane_release();
        return 0;
    }

    /* Build the file text first, and compare it with what the cache already
       says: an unchanged list is not rewritten, so the common launch does no
       write at all and a file watcher sees a change only when there is one. */
    for (i = 0; i < (int)n; i++) {
        VkPhysicalDeviceProperties p;
        int k;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        vk_canon(name[i], NAMELEN, p.deviceName);
        disc[i] = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        /* `_snprintf` RETURNS -1 ON TRUNCATION, not the length it wanted, so
           the return is checked BEFORE it is added: `at += -1` would step the
           cursor backwards and the next device would be written in front of the
           buffer. It cannot truncate at today's bounds (4 devices x 34 bytes
           against 224), which is exactly why it would have sat here unnoticed
           until one of them moved. */
        k = _snprintf(out + at, sizeof out - at - 1, "%d %s\n", disc[i], name[i]);
        if (k < 0 || k >= (int)(sizeof out - at - 1)) break;
        at += k;
    }
    out[sizeof out - 1] = 0;

    changed = (int)n != (int)s_count;
    for (i = 0; !changed && i < (int)n; i++)
        if (lstrcmpA(s_name[i], name[i]) || s_disc[i] != disc[i]) changed = 1;

    if (changed) {
        /* TEMPORARY AND RENAME, for the reason `tagpu_vk_gpu_store` and
           `tagpu_menu.c`'s `write_cfg` both give and this one had not taken:
           CREATE_ALWAYS truncates first, and the file is opened FILE_SHARE_READ,
           so a second instance reading it at DLL attach -- or a crash between
           the truncate and the write -- would see nothing and lose the GPU row's
           captions for that launch. [FROM REVIEW 2026-09-15.] */
        int ok = 0;
        h = CreateFileA(GPUS_TMP, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, 0);
        if (h != INVALID_HANDLE_VALUE) {
            ok = WriteFile(h, out, (DWORD)at, &wrote, 0) && wrote == (DWORD)at;
            CloseHandle(h);
            if (ok) ok = MoveFileExA(GPUS_TMP, GPUS_FILE, MOVEFILE_REPLACE_EXISTING);
            if (!ok) DeleteFileA(GPUS_TMP);
        }
        vklog("enumerated %u device(s) - " GPUS_FILE " %s, so the GPU row lists them "
              "from the NEXT launch (the menu's .GUI is written at attach)",
              n, ok ? "rewritten" : "could NOT be rewritten and is left as it was");
    } else {
        vklog("enumerated %u device(s) - " GPUS_FILE " already agrees", n);
    }
    for (i = 0; i < (int)n; i++)
        vklog("  device %d: %s%s", i, name[i], disc[i] ? "  [discrete]" : "");

    vkDestroyInstance(inst, NULL);
    lane_release();
    return 0;
}

/* THE ENUMERATION TAKES ST_STARTING TOO, and that is not bookkeeping -- it is
   what stops two workers existing at once. [FROM REVIEW 2026-09-15.]

   Before this, `enum_start` created its thread unguarded while `tagpu_vk_frame`
   could create the bring-up thread a few frames later, and the two share every
   global in this file: `s_mod`, `s_gipa` and the whole IFNS/DFNS dispatch
   table. Two faults, both live: `vk_load`'s `if (s_mod) return 1;` is a
   double-checked lock with no barrier, so the second thread could see `s_mod`
   set and `vkCreateInstance` still NULL and call through it; and INSTANCE-LEVEL
   ENTRY POINTS BELONG TO AN INSTANCE, so with two instances alive the table
   ends up holding one instance's thunks while the other instance's handle is
   passed to them.

   The state machine already grants exactly one owner, so the enumeration asks
   for the same grant. A bring-up that arrives while it holds ST_STARTING waits
   for ST_OFF, which is the ordering the rest of the file already relies on. */
void tagpu_vk_enum_start(void)
{
    static LONG once;
    /* THE OFF FILE CANNOT CLAIM TO HAVE STOPPED A LANE IT DID NOT STOP. With
       the present owned, `read_lever` arms the lane whatever this file says --
       the renderer choice decides -- so the old unconditional line ("nothing in
       this module runs") was simply false on that path, and a log that
       misinforms is the same defect as one that says nothing. Its only real
       effect there is the device list not being refreshed, which is worth its
       own sentence rather than a wrong one. [FROM THE LANDING REVIEW,
       2026-09-17.] */
    if (exists(OFF_FILE)) {
        if (s_ownWin) {
            /* AND THE MESSAGE SAYS WHAT ACTUALLY HAPPENS. The first version of
               this fix said "only the device list is left unrefreshed", and the
               very next log line was the enumeration running -- a false line
               written while fixing a false line. Nothing stands down here: the
               GPU row needs the list whatever the OFF file says, because under
               this backend the row is the only way to choose a device. */
            vklog("tagpu_vk.off is present and IGNORED - `renderer=vulkan` "
                  "overrides both levers; nothing in this module stands down");
        }
        else {
            vklog("tagpu_vk.off - nothing in this module runs");
            return;
        }
    }
    if (InterlockedExchange(&once, 1)) return;
    if (InterlockedCompareExchange(&s_state, ST_STARTING, ST_OFF) != ST_OFF) {
        vklog("something already holds the lane - the device list is not refreshed "
              "this launch");
        return;
    }
    s_worker = CreateThread(NULL, 0, enum_worker, NULL, 0, NULL);
    if (!s_worker) {
        vklog("could not start the enumeration thread (%lu)", GetLastError());
        InterlockedExchange(&s_state, ST_OFF);
    }
}

/* ---- G19a: the bring-up -------------------------------------------------- */

/* The swapchain, its images and its per-image objects. Called by the bring-up
   worker (ST_STARTING, the worker owns `s_vk`) and by the render thread on a
   resize (ST_READY, the render thread owns it). Never by both: that is what the
   state machine is for. */
/* 1 built, 0 refused (the lane must come down), -1 not yet (try again). */
static int vk_swapchain(int w, int h)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[16];
    VkPresentModeKHR modes[8];
    VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainKHR old = s_vk.sc;
    VkFormat rpfmt = s_vk.fmt;         /* what the render pass, if any, was built on */
    uint32_t nf = 16, nm = 8, want;
    VkResult r;
    uint32_t i;

    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.pd, s_vk.surf, &caps);
    if (r != VK_SUCCESS) { vklog("surface capabilities: %s", res_name(r)); return 0; }

    vkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.pd, s_vk.surf, &nf, NULL);
    if (nf > 16) nf = 16;
    if (!nf) { vklog("the surface offers no format"); return 0; }
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.pd, s_vk.surf, &nf, fmts);
    s_vk.fmt = fmts[0].format; s_vk.cspace = fmts[0].colorSpace;
    for (i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            s_vk.fmt = fmts[i].format; s_vk.cspace = fmts[i].colorSpace; break;
        }

    /* PARITY WITH THE GL LANE, WHICH SETS wglSwapIntervalEXT(vsync ? 1 : 0).
       FIFO is the only mode the specification guarantees and is what vsync
       means; IMMEDIATE is the unlocked one. MAILBOX IS NOT OFFERED on the
       reference setup's 4070 under wine (roadmap, Phase G), so nothing here may
       be designed around triple buffering -- `fps_limiter.c` and `maxfps`
       remain the pacing mechanism, exactly as under GL. */
    if (!s_vk.vsync) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.pd, s_vk.surf, &nm, NULL);
        if (nm > 8) nm = 8;
        vkGetPhysicalDeviceSurfacePresentModesKHR(s_vk.pd, s_vk.surf, &nm, modes);
        for (i = 0; i < nm; i++)
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) { mode = modes[i]; break; }
    }

    /* A BOUND, NOT A CLAMP. Our per-image arrays are MAXIMG long, so a surface
       whose MINIMUM is larger than that is refused outright: clamping `want`
       below `minImageCount` is invalid, and letting the swapchain come back
       with more images than the arrays hold would mean `vkAcquireNextImageKHR`
       eventually handing back an index the bound in `vk_present` rejects --
       a lane that silently presents nothing rather than one that says why.
       (`minImageCount` is 3 on the reference setup; this has never fired.) */
    if (caps.minImageCount > MAXIMG) {
        vklog("the surface wants at least %u images and this build carries %d - "
              "the lane stays down", caps.minImageCount, MAXIMG);
        return 0;
    }
    want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    if (want > MAXIMG) want = MAXIMG;

    s_vk.ext = caps.currentExtent;
    if (s_vk.ext.width == 0xFFFFFFFFu) {
        s_vk.ext.width  = (uint32_t)(w > 0 ? w : 640);
        s_vk.ext.height = (uint32_t)(h > 0 ? h : 480);
    }
    /* A minimised or hidden window reports a zero extent and a swapchain cannot
       be made on one. THAT IS A WAIT, NOT A FAILURE -- the comment used to say
       "the caller retries" and no caller did: every one of them treated 0 as
       terminal and put the lane into ST_FAILED for the session, so minimising
       the game once with the lever on would have killed it until the lever was
       toggled. -1 says "come back later". [FROM REVIEW 2026-09-15.] */
    if (!s_vk.ext.width || !s_vk.ext.height) return -1;
    if (s_vk.ext.width  < caps.minImageExtent.width)  s_vk.ext.width  = caps.minImageExtent.width;
    if (s_vk.ext.height < caps.minImageExtent.height) s_vk.ext.height = caps.minImageExtent.height;
    if (s_vk.ext.width  > caps.maxImageExtent.width)  s_vk.ext.width  = caps.maxImageExtent.width;
    if (s_vk.ext.height > caps.maxImageExtent.height) s_vk.ext.height = caps.maxImageExtent.height;

    swci.surface = s_vk.surf;
    swci.minImageCount = want;
    swci.imageFormat = s_vk.fmt;
    swci.imageColorSpace = s_vk.cspace;
    swci.imageExtent = s_vk.ext;
    swci.imageArrayLayers = 1;
    /* ASK THE SURFACE, DO NOT ASSUME. COLOR_ATTACHMENT is guaranteed on every
       surface; TRANSFER_DST is not, and G19a's clear needs it -- a driver
       without it would fail swapchain creation with a result that says nothing
       about why. The reference setup reports 0x9F, so this has never fired;
       it is here so that the day it does, the log names it. Same for the
       composite mode: OPAQUE is what we want and INHERIT is the fallback every
       surface that lacks it offers. [FROM REVIEW 2026-09-15.] */
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    /* TRANSFER_SRC IS FOR THE CAPTURE, AND ASKING FOR IT IS NOT OPTIONAL.
       [FROM REVIEW 2026-09-15.] `tagpu_vk_shot.c` copies a presented image into
       a host buffer, and `vkCmdCopyImageToBuffer` requires the source image to
       have been CREATED with this usage — it is not something a layout
       transition confers. It was missing, the reference ICD did it anyway, no
       validation layer was running to say otherwise, and G19d's whole 0-px
       result therefore rested on undefined behaviour. The lane does not need
       it, so a surface that refuses it loses the CAPTURE and keeps the lane. */
    s_vk.cansrc = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? 1 : 0;
    if (s_vk.cansrc) swci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        swci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    else {
        vklog("the surface does not support TRANSFER_DST (usage 0x%X) and the clear "
              "needs it - the lane stays down", (unsigned)caps.supportedUsageFlags);
        return 0;
    }
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha =
        (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    swci.presentMode = mode;
    swci.clipped = VK_TRUE;
    swci.oldSwapchain = old;
    r = vkCreateSwapchainKHR(s_vk.dev, &swci, NULL, &s_vk.sc);
    if (r != VK_SUCCESS) {
        vklog("vkCreateSwapchainKHR: %s (%d)", res_name(r), (int)r);
        /* PUT `old` BACK SO IT CAN BE DESTROYED, not so it can be used: the
           specification retires `oldSwapchain` even when creation FAILS, so
           presenting to it again would be presenting to a retired swapchain.
           Every caller of this function treats 0 as "tear the lane down", and
           `vk_down` is what then destroys this handle. */
        s_vk.sc = old;
        return 0;
    }
    if (old) vkDestroySwapchainKHR(s_vk.dev, old, NULL);

    /* THE FORMAT MAY NOT MOVE UNDER A LIVE RENDER PASS. `vk_renderpass` is
       built once from this format and every ported pass's pipeline is built
       against it; a rebuild that came back with a different format would leave
       both describing an attachment that is no longer there, and Vulkan's
       render-pass compatibility rules make that undefined rather than an error.
       The format is chosen from the same surface by the same preference order
       every time, so this cannot fire -- which is precisely why it is worth a
       line: the day the surface changes its mind, the lane says so and comes
       down instead of drawing into a lie. */
    if (s_vk.rp && s_vk.fmt != rpfmt) {
        vklog("the surface changed format under the render pass (%d -> %d) - down",
              (int)rpfmt, (int)s_vk.fmt);
        return 0;
    }

    /* REFUSED, NOT CLAMPED. `minImageCount` bounds what we ASK for; nothing
       bounds what the implementation returns, and clamping the count would
       leave `vkAcquireNextImageKHR` free to hand back an index past the end of
       our per-image arrays -- a frame dropped every time, with an acquire
       semaphore left signalled behind it. [FROM REVIEW 2026-09-15.] */
    s_vk.nimg = 0;
    if (vkGetSwapchainImagesKHR(s_vk.dev, s_vk.sc, &s_vk.nimg, NULL) != VK_SUCCESS ||
        !s_vk.nimg) {
        vklog("the swapchain reports no images"); s_vk.nimg = 0; return 0;
    }
    if (s_vk.nimg > MAXIMG) {
        vklog("the swapchain came back with %u images and this build carries %d - "
              "the lane stays down", s_vk.nimg, MAXIMG);
        s_vk.nimg = 0;
        return 0;
    }
    if (vkGetSwapchainImagesKHR(s_vk.dev, s_vk.sc, &s_vk.nimg, s_vk.img) != VK_SUCCESS) {
        vklog("the swapchain would not hand over its images"); s_vk.nimg = 0; return 0;
    }

    vklog("swapchain: %u images %ux%u format %d mode %s",
          s_vk.nimg, s_vk.ext.width, s_vk.ext.height, (int)s_vk.fmt,
          mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO");
    return 1;
}

/* THE RENDER PASS, ONE PER DEVICE. One colour attachment in the swapchain's
   format, and it does two things that would otherwise be barriers:

     * it takes the image from TRANSFER_DST_OPTIMAL -- which is what
       `vkCmdClearColorImage` left it in -- to COLOR_ATTACHMENT_OPTIMAL, and
     * it leaves it in PRESENT_SRC_KHR, which is the transition `vk_present`
       used to do by hand.

   `loadOp` is LOAD and not CLEAR because the clear has already happened: the
   colour is the lever's and G19a's `vkCmdClearColorImage` is what applies it.
   The external dependency is what orders that transfer write before the first
   colour write, and it is not optional -- without it the layout transition the
   render pass performs is unordered against the clear.

   It is created once and kept for the life of the device so that a pass can
   build a pipeline against it and keep that pipeline across a resize; only the
   framebuffers follow the swapchain. */
/* THE DEPTH FORMAT IS 24-BIT FIXED POINT OR THERE IS NONE, and that is a
   parity decision rather than a preference. The GL lane's world FBO is
   GL_DEPTH24_STENCIL8, so every depth value a ported pass is tested against
   there is quantised to 24 bits; a D32_SFLOAT attachment here would resolve a
   z-fight the other way in exactly the cases that are too close to call, and
   those are the cases a 0-px comparison is made of. A device that offers
   neither 24-bit format gets no depth attachment at all -- the passes that do
   not test go on working and the ones that do refuse to arm and say so, which
   is a stated gap rather than a silently different picture. */
static VkFormat vk_depth_format(void)
{
    static const VkFormat want[2] = {
        VK_FORMAT_D24_UNORM_S8_UINT,        /* what the GL FBO is */
        VK_FORMAT_X8_D24_UNORM_PACK32       /* the same 24 bits, no stencil */
    };
    int i;
    for (i = 0; i < 2; i++) {
        VkFormatProperties fp;
        memset(&fp, 0, sizeof fp);
        vkGetPhysicalDeviceFormatProperties(s_vk.pd, want[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return want[i];
    }
    return VK_FORMAT_UNDEFINED;
}

static int vk_renderpass(void)
{
    VkAttachmentDescription at[2];
    VkAttachmentReference ref, dref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo rci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    s_vk.dfmt = vk_depth_format();
    if (s_vk.dfmt == VK_FORMAT_UNDEFINED)
        vklog("no 24-bit depth format on this device - the passes that depth-test "
              "will not arm (the GL lane's FBO is DEPTH24_STENCIL8 and a 32-bit "
              "float attachment would not settle a z-fight the same way)");

    memset(at, 0, sizeof at);
    at[0].format = s_vk.fmt;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    at[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    /* CLEARED BY THE RENDER PASS AND NEVER STORED. The GL lane clears depth at
       the top of the world FBO (glClear(GL_DEPTH_BUFFER_BIT), clear value 1.0)
       and this is the same thing in the same place; DONT_CARE on the way out
       says the contents do not outlive the frame, which is what lets the driver
       skip writing them back. UNDEFINED in, because a CLEAR discards whatever
       was there -- so no layout has to be carried between frames and no barrier
       orders one frame's depth writes against the next's clear beyond the
       dependency below. */
    at[1].format = s_vk.dfmt;
    at[1].samples = VK_SAMPLE_COUNT_1_BIT;
    at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    memset(&ref, 0, sizeof ref);
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&dref, 0, sizeof dref);
    dref.attachment = 1;
    dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    if (s_vk.dfmt != VK_FORMAT_UNDEFINED) sub.pDepthStencilAttachment = &dref;

    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep[1].dstAccessMask = 0;
    if (s_vk.dfmt != VK_FORMAT_UNDEFINED) {
        /* THE DEPTH ATTACHMENT NEEDS ITS OWN HALF OF BOTH DEPENDENCIES. The
           clear the render pass performs is an EARLY_FRAGMENT_TESTS write, and
           the previous frame using this same image finished its depth writes at
           LATE_FRAGMENT_TESTS; between two submits on one queue the ordering is
           submission order, but the ACCESS still has to be made visible, which
           is what these masks do. Leaving them off is the class of omission a
           validation layer catches and a correct picture does not. */
        dep[0].srcStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep[0].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep[0].dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep[0].dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep[1].srcStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep[1].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    rci.attachmentCount = s_vk.dfmt != VK_FORMAT_UNDEFINED ? 2 : 1;
    rci.pAttachments = at;
    rci.subpassCount = 1; rci.pSubpasses = &sub;
    rci.dependencyCount = 2; rci.pDependencies = dep;
    if (vkCreateRenderPass(s_vk.dev, &rci, NULL, &s_vk.rp) != VK_SUCCESS) {
        vklog("the render pass would not create"); s_vk.rp = VK_NULL_HANDLE; return 0;
    }
    return 1;
}

/* One frame slot's depth image, at the swapchain's extent. Built with the rest
   of the per-image objects and torn down with them, because it is sized by the
   swapchain exactly as the framebuffers are. */
static int vk_depth_image(uint32_t i)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkPhysicalDeviceMemoryProperties mp;
    VkMemoryRequirements req;
    uint32_t t;
    int type = -1;

    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = s_vk.dfmt;
    ici.extent.width = s_vk.ext.width;
    ici.extent.height = s_vk.ext.height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_vk.dev, &ici, NULL, &s_vk.dimg[i]) != VK_SUCCESS) {
        vklog("depth image %u", i); return 0;
    }
    vkGetImageMemoryRequirements(s_vk.dev, s_vk.dimg[i], &req);
    vkGetPhysicalDeviceMemoryProperties(s_vk.pd, &mp);
    for (t = 0; t < mp.memoryTypeCount; t++)
        if ((req.memoryTypeBits & (1u << t)) &&
            (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = (int)t; break;
        }
    if (type < 0) { vklog("no device-local memory for a depth image"); return 0; }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(s_vk.dev, &mai, NULL, &s_vk.dmem[i]) != VK_SUCCESS) {
        vklog("depth image memory %u", i); return 0;
    }
    if (vkBindImageMemory(s_vk.dev, s_vk.dimg[i], s_vk.dmem[i], 0) != VK_SUCCESS) {
        vklog("binding depth image %u", i); return 0;
    }
    ivi.image = s_vk.dimg[i];
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = s_vk.dfmt;
    /* the STENCIL aspect is deliberately not in the view: nothing here tests or
       writes stencil, and a depth/stencil format whose view carries both cannot
       be used as a plain depth attachment on every driver */
    ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ivi.subresourceRange.levelCount = 1;
    ivi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s_vk.dev, &ivi, NULL, &s_vk.dview[i]) != VK_SUCCESS) {
        vklog("depth image view %u", i); return 0;
    }
    return 1;
}

/* The per-image sync and command objects. Sized by the swapchain, so they are
   built after it and torn down with it -- and since G19d the image views and
   framebuffers a pass draws into are among them. */
static int vk_perimage(void)
{
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    uint32_t i;

    /* SIGNALLED, so the first frame's wait returns at once rather than timing
       out on a fence nothing has ever submitted. */
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    cbai.commandPool = s_vk.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = s_vk.nimg;
    if (vkAllocateCommandBuffers(s_vk.dev, &cbai, s_vk.cmd) != VK_SUCCESS) {
        vklog("command buffers"); return 0;
    }
    /* ---- tagpu_ftime's timestamps (G19f landing 6) ----------------------
       TWO CONDITIONS, AND NEITHER IS "the device has a clock". `timestampPeriod`
       is nanoseconds per tick and is 0 on a device that cannot do it at all;
       `timestampValidBits` is per QUEUE FAMILY and can be 0 on the very family
       we submit to while another family has it. Both are checked, the harness
       simply goes without on a device that refuses, and the report then says
       there is no comparison rather than printing half of one. It is NOT fatal:
       a missing cost figure must never cost a frame. */
    s_vk.tsPeriod = 0.0;
    memset(s_vk.tsPend, 0, sizeof s_vk.tsPend);
    {
        VkPhysicalDeviceProperties dp;
        VkQueueFamilyProperties qp[16];
        uint32_t nq = 16;
        vkGetPhysicalDeviceProperties(s_vk.pd, &dp);
        vkGetPhysicalDeviceQueueFamilyProperties(s_vk.pd, &nq, qp);
        if (dp.limits.timestampPeriod > 0.0f && s_vk.qfam < nq && qp[s_vk.qfam].timestampValidBits > 0) {
            VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = s_vk.nimg * 2u;
            if (vkCreateQueryPool(s_vk.dev, &qi, NULL, &s_vk.tsPool) == VK_SUCCESS)
                s_vk.tsPeriod = (double)dp.limits.timestampPeriod;
            else
                vklog("ftime: no timestamp query pool - the Vulkan lane cannot be timed");
        } else {
            vklog("ftime: this device/queue reports no usable timestamps (period %.3f, valid bits %u)"
                  " - the Vulkan lane cannot be timed",
                  (double)dp.limits.timestampPeriod,
                  s_vk.qfam < nq ? qp[s_vk.qfam].timestampValidBits : 0u);
        }
    }
    for (i = 0; i < s_vk.nimg; i++) {
        VkImageViewCreateInfo ivi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        if (vkCreateSemaphore(s_vk.dev, &sci, NULL, &s_vk.semAcquire[i]) != VK_SUCCESS ||
            vkCreateSemaphore(s_vk.dev, &sci, NULL, &s_vk.semRelease[i]) != VK_SUCCESS ||
            vkCreateFence(s_vk.dev, &fci, NULL, &s_vk.fence[i]) != VK_SUCCESS) {
            vklog("per-image sync objects"); return 0;
        }
        if (!s_vk.rp) continue;         /* no render pass: nothing draws */
        ivi.image = s_vk.img[i];
        ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivi.format = s_vk.fmt;
        ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivi.subresourceRange.levelCount = 1;
        ivi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(s_vk.dev, &ivi, NULL, &s_vk.view[i]) != VK_SUCCESS) {
            vklog("swapchain image view %u", i); return 0;
        }
        {
            VkImageView av[2];
            av[0] = s_vk.view[i];
            if (s_vk.dfmt != VK_FORMAT_UNDEFINED) {
                if (!vk_depth_image(i)) return 0;
                av[1] = s_vk.dview[i];
            }
            fbi.renderPass = s_vk.rp;
            fbi.attachmentCount = s_vk.dfmt != VK_FORMAT_UNDEFINED ? 2 : 1;
            fbi.pAttachments = av;
            fbi.width = s_vk.ext.width; fbi.height = s_vk.ext.height; fbi.layers = 1;
            if (vkCreateFramebuffer(s_vk.dev, &fbi, NULL, &s_vk.fb[i]) != VK_SUCCESS) {
                vklog("framebuffer %u", i); return 0;
            }
        }
    }
    return 1;
}

static void vk_perimage_free(void)
{
    uint32_t i;
    /* THE COMMAND BUFFERS GO BACK TO THE POOL. `vk_perimage` allocates `nimg`
       of them and a resize calls this and then that again -- without the free,
       every swapchain rebuild would leave its buffers in the pool for the life
       of the device, and a window the player drags rebuilds often. `nimg` is
       still the OLD count here, which is what was allocated, because
       `vk_swapchain` has not run yet. */
    if (s_vk.dev && s_vk.pool && s_vk.nimg && s_vk.cmd[0] && vkFreeCommandBuffers) {
        vkFreeCommandBuffers(s_vk.dev, s_vk.pool, s_vk.nimg, s_vk.cmd);
        memset(s_vk.cmd, 0, sizeof s_vk.cmd);
    }
    for (i = 0; i < MAXIMG; i++) {
        /* The framebuffer goes before the views it is built on. */
        if (s_vk.fb[i])   { vkDestroyFramebuffer(s_vk.dev, s_vk.fb[i], NULL); s_vk.fb[i] = VK_NULL_HANDLE; }
        if (s_vk.view[i]) { vkDestroyImageView(s_vk.dev, s_vk.view[i], NULL); s_vk.view[i] = VK_NULL_HANDLE; }
        /* the depth image is OURS, unlike the swapchain's colour image: the
           view, then the image, then its memory */
        if (s_vk.dview[i]) { vkDestroyImageView(s_vk.dev, s_vk.dview[i], NULL); s_vk.dview[i] = VK_NULL_HANDLE; }
        if (s_vk.dimg[i])  { vkDestroyImage(s_vk.dev, s_vk.dimg[i], NULL);      s_vk.dimg[i] = VK_NULL_HANDLE; }
        if (s_vk.dmem[i])  { vkFreeMemory(s_vk.dev, s_vk.dmem[i], NULL);        s_vk.dmem[i] = VK_NULL_HANDLE; }
        if (s_vk.semAcquire[i]) { vkDestroySemaphore(s_vk.dev, s_vk.semAcquire[i], NULL); s_vk.semAcquire[i] = VK_NULL_HANDLE; }
        if (s_vk.semRelease[i]) { vkDestroySemaphore(s_vk.dev, s_vk.semRelease[i], NULL); s_vk.semRelease[i] = VK_NULL_HANDLE; }
        if (s_vk.fence[i])      { vkDestroyFence(s_vk.dev, s_vk.fence[i], NULL);          s_vk.fence[i] = VK_NULL_HANDLE; }
        s_vk.tsPend[i] = 0;
    }
    /* THE TIMESTAMP POOL IS PER-IMAGE TOO, and leaving it out of this function
       leaked one per swapchain rebuild for the life of the device -- a window
       the player drags rebuilds often, and `vk_resize` is exactly
       `vk_perimage_free(); vk_swapchain(); vk_perimage();`, so `vk_perimage`
       would overwrite a live handle every time. Nulled as well as destroyed:
       the write path gates on `tsPool` alone, so a stale handle left behind by
       an early return in `vk_perimage` would be written into.
       [FOUND 2026-09-16, the landing-6 review.] */
    if (s_vk.tsPool) { vkDestroyQueryPool(s_vk.dev, s_vk.tsPool, NULL); s_vk.tsPool = VK_NULL_HANDLE; }
    s_vk.tsPeriod = 0.0;
}

/* Everything, in the reverse order it was built. Called with `s_vk` owned --
   the render thread in ST_READY, or the bring-up worker unwinding its own
   failure while still in ST_STARTING. */
static void vk_down(void)
{
    if (s_vk.dev) {
        /* The result is kept because `ab_drop` below is the one thing here whose
           correctness depends on it rather than on the object's own ownership. */
        int idle = !vkDeviceWaitIdle || vkDeviceWaitIdle(s_vk.dev) == VK_SUCCESS;
        if (!idle) vklog("vkDeviceWaitIdle refused on the way down");
        /* THE FRAME-TIME RING GOES WITH THE LANE. `tagpu_ftime.h` says the seam
           calls this "so a figure from a lane that no longer exists is not
           averaged into one that does", and until the landing-6 review found it
           the function had NO caller at all -- a documented invariant that was
           never implemented. The shell<->game switch brings the lane down and
           back up inside one process (145-173 ms, landing 5), possibly at
           another resolution, so the old lane's samples would have been
           averaged into the new one's percentiles.
           [FOUND 2026-09-16, the landing-6 review.] */
        tagpu_ftime_vk_reset();
        /* THE PASSES GO FIRST, AFTER THE WAIT AND BEFORE ANYTHING THEY MIGHT
           BE HOLDING. A pass owns pipelines, descriptors, buffers and images of
           its own; they belong to this device and must be back before it is
           destroyed. The wait above is what makes that safe rather than a race
           -- nothing of theirs is still in a queue. */
        tagpu_vk_fps_down(&s_pass);
        tagpu_vk_scaffold_down(&s_pass);
        tagpu_vk_feat_down(&s_pass);
        tagpu_vk_terr_down(&s_pass);
        tagpu_vk_fx_down(&s_pass);
        tagpu_vk_shadow_down(&s_pass);
        tagpu_vk_unit_down(&s_pass);
        tagpu_vk_hires_down(&s_pass);
        tagpu_vk_mark_down(&s_pass);
        tagpu_vk_gui_down(&s_pass);
        tagpu_vk_surf_down(&s_pass);
        tagpu_vk_world_down(&s_pass);
        /* AFTER THE PASSES, because a pass owns the JOBS and the restorer owns
           what they are drawn with: each `_down` above gives its job back, and
           this then gives back the pipelines, the activations and the render
           passes those jobs were using. The other way round would destroy a
           framebuffer a live job still names. */
        tagpu_vk_restore_down(&s_pass);
        ab_drop("the lane coming down", idle);
        tagpu_vk_shot_down(&s_pass);
        vk_perimage_free();
        if (s_vk.rp) { vkDestroyRenderPass(s_vk.dev, s_vk.rp, NULL); s_vk.rp = VK_NULL_HANDLE; }
        if (s_vk.pool) { vkDestroyCommandPool(s_vk.dev, s_vk.pool, NULL); s_vk.pool = VK_NULL_HANDLE; }
        if (s_vk.sc)   { vkDestroySwapchainKHR(s_vk.dev, s_vk.sc, NULL);  s_vk.sc = VK_NULL_HANDLE; }
        vkDestroyDevice(s_vk.dev, NULL);
        s_vk.dev = VK_NULL_HANDLE;
    }
    if (s_vk.surf && s_vk.inst) { vkDestroySurfaceKHR(s_vk.inst, s_vk.surf, NULL); s_vk.surf = VK_NULL_HANDLE; }
    if (s_vk.inst) { vkDestroyInstance(s_vk.inst, NULL); s_vk.inst = VK_NULL_HANDLE; }
    memset(&s_vk, 0, sizeof s_vk);
    memset(&s_pass, 0, sizeof s_pass);
    s_vk.devIndex = -1;
    InterlockedExchange(&s_activeIndex, -1);
}

/* Which physical device to bind: the player's stored name when the loader
   still offers it, else the first DISCRETE_GPU, else the first that can
   present.
   Bounded by `n` throughout -- the index never leaves the array the loader
   handed us. */
static int pick_device(VkPhysicalDevice* pds, uint32_t n, uint32_t* qfam_out, char* name_out)
{
    int chosen = -1, fallback = -1;
    uint32_t i, k, qfam = 0, fbqfam = 0;
    char cname[NAMELEN];
    /* Latched ONCE for the whole scan: a name that changed halfway through
       would pick by one string and report the other. */
    const char* want = want_name();

    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkQueueFamilyProperties q[16];
        uint32_t nq = 16;
        int presentable = -1;

        vkGetPhysicalDeviceProperties(pds[i], &p);
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, NULL);
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, q);
        for (k = 0; k < nq; k++) {
            VkBool32 sup = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], k, s_vk.surf, &sup);
            if (sup && (q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { presentable = (int)k; break; }
        }
        if (presentable < 0) continue;

        vk_canon(cname, sizeof cname, p.deviceName);
        if (want[0] && !lstrcmpiA(cname, want)) {
            chosen = (int)i; qfam = (uint32_t)presentable;
        }
        /* the fallback is the first presentable device, upgraded the moment a
           DISCRETE_GPU appears -- which is the G19b exit's "the default lands
           on DISCRETE_GPU when one exists" */
        if (fallback < 0) { fallback = (int)i; fbqfam = (uint32_t)presentable; }
        else {
            VkPhysicalDeviceProperties fp;
            vkGetPhysicalDeviceProperties(pds[fallback], &fp);
            if (fp.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
                p.deviceType  == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                fallback = (int)i; fbqfam = (uint32_t)presentable;
            }
        }
    }

    if (chosen < 0) {
        if (want[0])
            vklog("the requested GPU \"%s\" is not among the devices present - falling back", want);
        chosen = fallback; qfam = fbqfam;
    }
    if (chosen < 0) return -1;

    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[chosen], &p);
        vk_canon(name_out, NAMELEN, p.deviceName);
    }
    *qfam_out = qfam;
    return chosen;
}

/* THE BRING-UP, and it runs here and only here: on its own thread, off the
   render thread, off DllMain. It owns `s_vk` for as long as ST_STARTING holds
   and publishes ST_READY only once every field is written. */
static DWORD WINAPI up_worker(LPVOID arg)
{
    VkPhysicalDevice pds[MAXDEV];
    uint32_t n = 0;
    int pick, i;
    VkResult r;
    DWORD t0 = GetTickCount();

    (void)arg;
    s_vk.devIndex = -1;
    s_vk.hwnd  = s_want.hwnd;
    s_vk.vsync = s_want.vsync;

    if (!vk_load()) goto fail;
    va_log("before bring-up");

    s_vk.inst = vk_instance();
    if (!s_vk.inst) goto fail;

    {
        VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        sci.hinstance = GetModuleHandleA(NULL);
        sci.hwnd = s_vk.hwnd;
        r = vkCreateWin32SurfaceKHR(s_vk.inst, &sci, NULL, &s_vk.surf);
        if (r != VK_SUCCESS) {
            /* NAMED RATHER THAN SWALLOWED. There is one window now and it is
               the game's, so a refusal here is a surface on the game window
               being refused -- measured to work on wine 9.0 and Proton 11
               (roadmap §G19a), which makes a driver that refuses it new
               information. The backend then falls back to GDI, which route F
               measured as still reaching the screen after a surface has been
               attempted here. (Until landing 4d-1 this also named the other
               case, a surface on a window of ours, which was the case the
               phase's out-of-process pivot existed for.) */
            vklog("vkCreateWin32SurfaceKHR on the game window: %s (%d) - the "
                  "lane stays down", res_name(r), (int)r);
            goto fail;
        }
    }

    vkEnumeratePhysicalDevices(s_vk.inst, &n, NULL);
    if (n > MAXDEV) n = MAXDEV;
    if (!n) { vklog("no physical devices"); goto fail; }
    vkEnumeratePhysicalDevices(s_vk.inst, &n, pds);

    pick = pick_device(pds, n, &s_vk.qfam, s_vk.devName);
    if (pick < 0) { vklog("no device can present on our window"); goto fail; }
    s_vk.pd = pds[pick];

    /* The index the MENU means, which is an index into the cached name table
       and not into the loader's list -- the two agree on a launch where the
       cache is current and need not on one where it is not. Matched by name,
       so a disagreement produces -1 ("not one of the listed devices") rather
       than a wrong row. */
    s_vk.devIndex = -1;
    for (i = 0; i < (int)s_count; i++)
        if (!lstrcmpiA(s_name[i], s_vk.devName)) { s_vk.devIndex = i; break; }

    {
        float prio = 1.0f;
        const char* dexts[4] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, NULL, NULL, NULL };
        uint32_t ndext = 1;
        VkPhysicalDeviceLineRasterizationFeaturesEXT lrf =
            { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT };
        VkPhysicalDeviceDepthClipControlFeaturesEXT dcc =
            { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT };
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        VkPhysicalDeviceFeatures feat;      /* must outlive every vkCreateDevice below */

        /* VK_KHR_maintenance1 IS ASKED FOR AND NOT ASSUMED, AND IT BUYS EXACTLY
           ONE THING: a NEGATIVE VIEWPORT HEIGHT. GL's clip space has +Y up and
           Vulkan's has +Y down, so a shader written in GL's WINDOW convention
           would draw its frame upside down -- and the shaders are ported
           UNCHANGED on purpose, because a source edit would make each one
           disagree with the GL twin that is its oracle (tools/spirv-gen.py's
           header). So the flip is pipeline state, and this is the state.

           NOT EVERY PORTED SHADER, which is what this said until landing 5b.
           Only `gui`'s composite, `fps` and `scaffold` write that convention.
           The world passes write the engine's screen-space y, which grows
           DOWNWARD, so clip -1 is already the game frame's top row and a flip
           would turn them over twice -- which is exactly what it did, for eight
           landings. tagpu_vk_pass.h's `flipok` carries the table.

           The instance asks for Vulkan 1.0, where this is an extension rather
           than core; a 1.1+ driver still advertises it to a 1.0 application,
           and the reference setup's 4070 lists 247 device extensions. Asked for
           rather than assumed all the same: a device without it keeps the lane
           and loses the passes, which say so. */
        /* THE COUNT IS ASKED FOR AND THE LIST IS ALLOCATED, and both halves are
           the review's. [FROM REVIEW 2026-09-15.] The first version read into a
           512-entry array on the stack -- 130 KB on a worker thread, and worse,
           it accepted only `VK_SUCCESS`: a driver with more than 512 extensions
           answers `VK_INCOMPLETE` with a perfectly good partial list, and the
           lane would then have reported that maintenance1 "is not offered" on a
           device that offers it, and refused every ported pass for the session.
           `VK_INCOMPLETE` is an answer, not an error. */
        if (vkEnumerateDeviceExtensionProperties) {
            uint32_t ne = 0, k;
            VkResult er = vkEnumerateDeviceExtensionProperties(s_vk.pd, NULL, &ne, NULL);
            VkExtensionProperties* ext = NULL;
            if ((er == VK_SUCCESS || er == VK_INCOMPLETE) && ne)
                ext = (VkExtensionProperties*)malloc((size_t)ne * sizeof *ext);
            if (ext) {
                er = vkEnumerateDeviceExtensionProperties(s_vk.pd, NULL, &ne, ext);
                if (er == VK_SUCCESS || er == VK_INCOMPLETE)
                    for (k = 0; k < ne; k++) {
                        /* BOUNDED, because the array is three long and two
                           optional names can each append. The specification says
                           a name is not enumerated twice; an ICD or layer that
                           does it anyway would write one past this array, and a
                           bound that costs one comparison is cheaper than
                           trusting that. [G19e effects review, 2026-09-15.] */
                        if (ndext >= (uint32_t)(sizeof dexts / sizeof dexts[0])) break;
                        if (!strcmp(ext[k].extensionName, "VK_KHR_maintenance1")) {
                            dexts[ndext++] = "VK_KHR_maintenance1";
                            s_vk.flipok = 1;
                        }
                        /* VK_EXT_line_rasterization, AND IT BUYS EXACTLY ONE
                           THING TOO: `BRESENHAM` line rasterisation, which is
                           the rule GL's non-antialiased lines already follow.
                           MEASURED 2026-09-15, and this is why it is here: with
                           Vulkan's DEFAULT mode the effects pass's lasers came
                           out a strict SUPERSET of the GL twin's -- every one of
                           its 126 pixels plus exactly one extra fragment at the
                           end of each line segment, 4 px on the fixture. That is
                           the diamond-exit rule, which DEFAULT does not
                           implement and BRESENHAM does.
                           A pass that draws lines refuses the frame when this is
                           off rather than drawing those four pixels. */
                        if (!strcmp(ext[k].extensionName,
                                    VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME)) {
                            /* THE DEVICE IS ASKED WHETHER IT HAS THE FEATURE,
                               AND THAT ANSWER IS THE TEST. Offering the
                               extension is not offering `bresenhamLines`, and
                               inferring the feature from a vkCreateDevice that
                               SUCCEEDED does not work: an ICD that does not
                               consider the extension properly enabled is
                               entitled to ignore the unrecognised pNext struct
                               and return VK_SUCCESS, which is indistinguishable
                               from having enabled it. The pass would then build
                               its line pipeline with BRESENHAM chained on a
                               device where the feature is off -- undefined
                               behaviour rather than an error, and in practice
                               the default line mode, which is the four-pixel
                               superset G19e added this for. So: query, and on
                               no query, no lines.
                               [G19e EFFECTS REVIEW, 2026-09-15, both reviewers.] */
                            PFN_vkGetPhysicalDeviceFeatures2KHR gpdf2 =
                                (PFN_vkGetPhysicalDeviceFeatures2KHR)
                                    s_gipa(s_vk.inst, "vkGetPhysicalDeviceFeatures2KHR");
                            if (gpdf2) {
                                VkPhysicalDeviceLineRasterizationFeaturesEXT q;
                                VkPhysicalDeviceFeatures2 f2;
                                memset(&q, 0, sizeof q);
                                memset(&f2, 0, sizeof f2);
                                q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT;
                                f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                                f2.pNext = &q;
                                gpdf2(s_vk.pd, &f2);
                                if (q.bresenhamLines) {
                                    dexts[ndext++] = VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME;
                                    s_vk.lineok = 1;
                                } else {
                                    vklog("the device offers VK_EXT_line_rasterization but not "
                                          "bresenhamLines - a ported pass that draws LINES will "
                                          "stand down");
                                }
                            } else {
                                vklog("VK_EXT_line_rasterization is offered but "
                                      "vkGetPhysicalDeviceFeatures2KHR is not, so bresenhamLines "
                                      "cannot be confirmed - a ported pass that draws LINES will "
                                      "stand down rather than chain a mode the device may ignore");
                            }
                        }
                        /* VK_EXT_depth_clip_control, AND IT BUYS EXACTLY ONE
                           THING: GL'S CLIP-SPACE Z RANGE. GL maps clip z in
                           [-1, 1] onto the depth range and Vulkan takes [0, 1],
                           CLIPPING anything below 0. Every world pass ported so
                           far writes a z already in [0, 1] -- so the feature
                           pass's `minDepth 0.5 / maxDepth 1.0` reproduces GL's
                           (z+1)/2 exactly and nothing is clipped, and
                           tagpu_vk_feat.c's header says this extension is the
                           answer only if a shader is ever found writing a z
                           below 0.
                           G19e's SHADOW pass is that shader. Its orthographic
                           light matrix is built to fill [-1, 1] (tagpu_shadow.c
                           `mrow`), so under Vulkan's own convention the whole
                           near half of every caster would be clipped away and
                           the map would be wrong rather than merely different.
                           With `negativeOneToOne` the viewport transform IS
                           GL's, values and quantisation included, and the
                           depths the consumer's taShadowAt compares against are
                           the same numbers.
                           Queried, never inferred, for the reason the line
                           feature's comment above gives at length. */
                        if (!strcmp(ext[k].extensionName,
                                    VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME)) {
                            PFN_vkGetPhysicalDeviceFeatures2KHR gpdf2 =
                                (PFN_vkGetPhysicalDeviceFeatures2KHR)
                                    s_gipa(s_vk.inst, "vkGetPhysicalDeviceFeatures2KHR");
                            if (gpdf2) {
                                VkPhysicalDeviceDepthClipControlFeaturesEXT q;
                                VkPhysicalDeviceFeatures2 f2;
                                memset(&q, 0, sizeof q);
                                memset(&f2, 0, sizeof f2);
                                q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
                                f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                                f2.pNext = &q;
                                gpdf2(s_vk.pd, &f2);
                                if (q.depthClipControl) {
                                    dexts[ndext++] = VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME;
                                    s_vk.zclipok = 1;
                                } else {
                                    vklog("the device offers VK_EXT_depth_clip_control but not "
                                          "the depthClipControl feature - a ported pass that "
                                          "needs GL's clip-space z range will stand down");
                                }
                            } else {
                                vklog("VK_EXT_depth_clip_control is offered but "
                                      "vkGetPhysicalDeviceFeatures2KHR is not, so "
                                      "depthClipControl cannot be confirmed - a pass that needs "
                                      "GL's z range will stand down rather than chain a struct "
                                      "the device may ignore");
                            }
                        }
                    }
                free(ext);
            }
        }
        if (!s_vk.flipok)
            vklog("VK_KHR_maintenance1 is not offered - the lane will present, and "
                  "the WORLD passes will draw (since landing 5b they need no flip), "
                  "but the GUI layer, the FPS readout and the scaffold overlay stay "
                  "down, so the frame is a world with no UI over it");
        if (!s_vk.lineok)
            vklog("VK_EXT_line_rasterization is not offered - a ported pass that "
                  "draws LINES will stand down (its twin's rule is the diamond-exit "
                  "one, and Vulkan's default mode is not it)");
        if (!s_vk.zclipok)
            vklog("VK_EXT_depth_clip_control is not offered - a ported pass whose "
                  "shader writes a clip z below 0 will stand down (Vulkan clips "
                  "those and GL does not); the world passes are unaffected");

        /* ANISOTROPY IS A CORE FEATURE BIT, so it goes in pEnabledFeatures and
           not in the pNext chain -- which is why it is not on the retry ladder
           below: the ladder drops EXTENSIONS, and this is asked for only when
           the device has just said it has it. `feat` must outlive the
           vkCreateDevice calls, which is why it is declared with them. */
        {
            VkPhysicalDeviceFeatures have;
            memset(&have, 0, sizeof have);
            memset(&feat, 0, sizeof feat);
            vkGetPhysicalDeviceFeatures(s_vk.pd, &have);
            if (have.samplerAnisotropy) {
                VkPhysicalDeviceProperties dp;
                feat.samplerAnisotropy = VK_TRUE;
                s_vk.anisook = 1;
                vkGetPhysicalDeviceProperties(s_vk.pd, &dp);
                s_vk.maxAniso = dp.limits.maxSamplerAnisotropy;
            } else {
                vklog("samplerAnisotropy is not offered - a pass sampling a "
                      "Classic++ restored twin will stand down, because its GL "
                      "original is filtered 4x anisotropically and NOT doing "
                      "that is a different picture from our own oracle");
            }
            /* `wideLines` IS A CORE FEATURE BIT TOO, so it sits beside
               anisotropy for the same reason and is likewise NOT on the retry
               ladder below -- the ladder drops EXTENSIONS, and this is asked
               for only when the device has just said it has it.
               WHY THE LANE WANTS IT: since 4c-2 the world is drawn into a
               target `ss` times the game resolution, and the GL twin calls
               `glLineWidth(ss)` so a line one game pixel wide is `ss` pixels
               there. Without this the ported passes can only draw a 1.0 line,
               which is `ss` times too thin -- so tagpu_vk_fx.c and
               tagpu_vk_mark.c refused the whole pass instead, and on the
               shipped default (`ss` = 2) the effects pass dropped every frame
               that had a laser in it. [The vulkan-only plan, landing 4c-2.] */
            if (have.wideLines) {
                VkPhysicalDeviceProperties dp;
                feat.wideLines = VK_TRUE;
                s_vk.wideok = 1;
                vkGetPhysicalDeviceProperties(s_vk.pd, &dp);
                s_vk.maxLineWidth = dp.limits.lineWidthRange[1];
            } else {
                vklog("wideLines is not offered - a pass drawing lines into a "
                      "supersampled world target will stand down, because its "
                      "GL twin draws them ss pixels wide and a 1.0 line is a "
                      "different picture from our own oracle");
            }
            dci.pEnabledFeatures = &feat;
        }
        qci.queueFamilyIndex = s_vk.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = ndext; dci.ppEnabledExtensionNames = dexts;
        /* THE FEATURE IS ENABLED, NOT MERELY THE EXTENSION. Which feature bits
           the device actually has was settled above, by asking it; this only
           turns the one we want on. The retry below is a fallback, NOT the test
           -- a vkCreateDevice that succeeds proves nothing about a pNext struct
           an ICD may have ignored. [Corrected by the G19e effects review.] */
        if (s_vk.lineok) {
            lrf.bresenhamLines = VK_TRUE;
            lrf.pNext = (void*)dci.pNext;
            dci.pNext = &lrf;
        }
        if (s_vk.zclipok) {
            dcc.depthClipControl = VK_TRUE;
            dcc.pNext = (void*)dci.pNext;
            dci.pNext = &dcc;
        }
        r = vkCreateDevice(s_vk.pd, &dci, NULL, &s_vk.dev);
        /* THE LADDER DROPS THE CHEAPEST THING FIRST, and each rung REBUILDS
           both the extension list and the pNext chain rather than unlinking one
           struct out of the middle of it -- the same reason the list is rebuilt
           rather than shortened. Depth clip control costs one pass (the shadow
           map); line rasterisation costs the passes that draw lines; the flip
           costs every pass. */
        if (r != VK_SUCCESS && s_vk.zclipok) {
            vklog("vkCreateDevice refused VK_EXT_depth_clip_control/depthClipControl "
                  "(%s) - retrying without it", res_name(r));
            s_vk.zclipok = 0;
            dci.pNext = NULL;
            if (s_vk.lineok) { lrf.pNext = NULL; dci.pNext = &lrf; }
            ndext = 1;
            if (s_vk.flipok) dexts[ndext++] = "VK_KHR_maintenance1";
            if (s_vk.lineok) dexts[ndext++] = VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME;
            dci.enabledExtensionCount = ndext;
            r = vkCreateDevice(s_vk.pd, &dci, NULL, &s_vk.dev);
        }
        if (r != VK_SUCCESS && s_vk.lineok) {
            /* Line rasterisation first: it is the one whose FEATURE can be
               refused as well as its extension, and dropping it costs only the
               passes that draw lines. */
            vklog("vkCreateDevice refused VK_EXT_line_rasterization/bresenhamLines "
                  "(%s) - retrying without it", res_name(r));
            s_vk.lineok = 0;
            dci.pNext = NULL;
            /* THE LIST IS REBUILT, NOT SHORTENED. The optional extensions go in
               in the order the DRIVER enumerates them, so `ndext - 1` would
               drop whichever happened to be last -- maintenance1 on a driver
               that lists it second, which costs every ported pass for a reason
               that has nothing to do with lines. */
            ndext = 1;
            if (s_vk.flipok) dexts[ndext++] = "VK_KHR_maintenance1";
            /* `zclipok` is still 0 here: the rung above is the only thing that
               can have left it set, and it clears it before retrying. */
            dci.enabledExtensionCount = ndext;
            r = vkCreateDevice(s_vk.pd, &dci, NULL, &s_vk.dev);
        }
        if (r != VK_SUCCESS && s_vk.flipok) {
            /* Then the flip, which costs every ported pass. One retry each, so
               this is a real fallback and not a loop. */
            vklog("vkCreateDevice refused VK_KHR_maintenance1 (%s) - retrying without it",
                  res_name(r));
            s_vk.flipok = 0;
            dci.pNext = NULL;
            dci.enabledExtensionCount = 1;
            r = vkCreateDevice(s_vk.pd, &dci, NULL, &s_vk.dev);
        }
        if (r != VK_SUCCESS) { vklog("vkCreateDevice: %s (%d)", res_name(r), (int)r); goto fail; }
    }

#define RES(n) n = (PFN_##n)vkGetDeviceProcAddr(s_vk.dev, #n);
    DFNS(RES)
#undef RES
    if (!vkCreateSwapchainKHR || !vkQueuePresentKHR || !vkAcquireNextImageKHR) {
        vklog("device entry points missing"); goto fail;
    }

    vkGetDeviceQueue(s_vk.dev, s_vk.qfam, 0, &s_vk.queue);

    /* -1 here is a window with no extent yet -- a wait, not a failure -- and
       the lane goes back to ST_OFF so the next frame asks again. */
    {
        int sc = vk_swapchain(s_want.w, s_want.h);
        if (sc < 0) {
            vklog("the window has no extent yet - trying again");
            vk_down();
            lane_release();
            return 0;
        }
        if (!sc) goto fail;
    }

    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s_vk.qfam;
        if (vkCreateCommandPool(s_vk.dev, &pci, NULL, &s_vk.pool) != VK_SUCCESS) {
            vklog("command pool"); goto fail;
        }
    }
    /* THE RENDER PASS BEFORE THE PER-IMAGE OBJECTS, because the framebuffers
       are built from it. It is per DEVICE and survives every resize, so a
       ported pass's pipeline survives them too. */
    if (!vk_renderpass()) goto fail;
    if (!vk_perimage()) goto fail;

    /* What a ported pass is handed. Written before the publish below, like
       every other field of the lane, so a render thread that reads ST_READY
       sees all of it. */
    s_pass.inst = s_vk.inst;
    s_pass.pd = s_vk.pd;
    s_pass.dev = s_vk.dev;
    s_pass.rp = s_vk.rp;
    s_pass.fmt = s_vk.fmt;
    s_pass.dfmt = s_vk.dfmt;
    s_pass.slots = s_vk.nimg;
    s_pass.flipok = s_vk.flipok;
    s_pass.anisook = s_vk.anisook;
    s_pass.maxAniso = s_vk.maxAniso;
    s_pass.lineok = s_vk.lineok;
    s_pass.wideok = s_vk.wideok;
    s_pass.maxLineWidth = s_vk.maxLineWidth;
    s_pass.zclipok = s_vk.zclipok;
    s_pass.gipa = s_gipa;
    s_pass.gdpa = vkGetDeviceProcAddr;
    s_pass.log = passlog;

    va_log("with Vulkan up");
    /* ONE HANDLE, NOT TWO. It used to print "our window %p over %p" because
       there were two -- a popup of ours over the game window -- and after
       landing 4d-1 both were the same handle and the line read "X over X" with
       nothing to say why. There is one window and it is the game's. */
    vklog("up in %lu ms on \"%s\" (row %d), the game window %p, %ux%u, vsync %d "
          "- the surface is on the game window and there is no GL lane",
          GetTickCount() - t0, s_vk.devName, s_vk.devIndex, (void*)s_vk.hwnd,
          s_vk.ext.width, s_vk.ext.height, s_vk.vsync);

    InterlockedExchange(&s_activeIndex, s_vk.devIndex);
    /* THE PUBLISH, AND IT IS A COMPARE-EXCHANGE RATHER THAN A STORE.
       [FROM REVIEW 2026-09-15.] Everything above is written before it and the
       interlocked operation is a full barrier, so a render thread that reads
       ST_READY sees all of it. It has to be conditional on ST_STARTING because
       `tagpu_vk_render_stop` may have moved the lane to ST_ZOMBIE while we
       worked -- an unconditional store used to stamp ST_READY over that and
       hand the render thread objects the code had already declared abandoned.
       After a successful publish the worker touches nothing. */
    if (InterlockedCompareExchange(&s_state, ST_READY, ST_STARTING) == ST_STARTING)
        return 0;
    /* Abandoned while we built it. We are still the only owner -- that is what
       ST_ZOMBIE means -- so we put our own objects back rather than leaking
       them, and hand the lane on. */
    vklog("abandoned during bring-up - releasing what was built");
    vk_down();
    lane_release();
    return 0;

fail:
    /* Everything this built goes back, on the thread that is still its sole
       owner. Before landing 4d-1 a window of ours went with it and the ORDER was
       the delicate part; there is no window now, so this is just the release. */
    vk_down();
    if (InterlockedCompareExchange(&s_state, ST_FAILED, ST_STARTING) != ST_STARTING)
        lane_release();          /* zombied: hand the lane back instead */
    return 0;
}

/* ---- G19a: the per-frame present ----------------------------------------- */

/* THE SEMAPHORE INDEXING, which is the one thing in this file that is easy to
   get quietly wrong. `semAcquire` and `fence` and the command buffer are
   indexed by the FRAME (mod image count): the fence wait at the top proves the
   submit that used them has completed, so they are free. `semRelease` is
   indexed by the IMAGE: a present waits on it, and the only thing that proves
   a present is done with an image is `vkAcquireNextImageKHR` handing that image
   back -- so the release semaphore has to follow the image and not the frame.
   `nimg` frames in flight, one per swapchain image: the fence wait at the top
   is what makes frame `n` wait for frame `n - nimg` and nothing sooner. (An
   earlier comment here said "one frame in flight", which `fi = frame % nimg`
   plainly is not.) Deepening this into a pass that paces itself is G19d's. */
/* THE WORLD'S FIVE PASSES, IN THE GL LANE'S OWN ORDER, INTO WHATEVER TARGET IS
   OPEN. One copy called from two places since 4c-2: the offscreen world pass
   when there is a target, and the frame's own render pass when there is not.

   IT IS ONE FUNCTION BECAUSE THE ORDER IS THE FRAGILE PART. tagpu_native.c
   draws terrain, features, units, effects, ghosts, markers, and two passes that
   blend are not commutative -- so an order that drifted between the two arms
   would be a picture that changes when the offscreen target happens to fail.
   Nothing here decides anything; the `draw_*` flags are their own `prepare`s'
   answers and `w`/`h` are the caller's target.

   `s_vk.rp` IS STILL WHAT THE MARKER PASS IS HANDED even when this is recording
   into the offscreen one, and that is correct rather than an oversight: it
   builds its pipelines against the render pass it is given, and render-pass
   COMPATIBILITY -- matching attachment formats and sample counts, never the
   load/store ops or the layouts -- is what lets those pipelines run in either.
   Handing it the offscreen pass instead would build a second set for no gain
   and would make the pass's own lifetime depend on a target it never sees. */
static void world_records(VkCommandBuffer cb, uint32_t fi, uint32_t w, uint32_t h,
                          int draw_terr, int draw_feat, int draw_unit,
                          int draw_fx, int draw_mark)
{
    if (draw_terr) tagpu_vk_terr_record(&s_pass, cb, fi, w, h);
    if (draw_feat) tagpu_vk_feat_record(&s_pass, cb, fi, w, h);
    /* THE UNITS, between the features and the effects -- which is where
       tagpu_native.c draws them, and two passes that blend are not
       commutative. */
    if (draw_unit) tagpu_vk_unit_record(&s_pass, cb, fi, w, h);
    if (draw_fx)   tagpu_vk_fx_record(&s_pass, cb, fi, w, h);
    /* AND THE BUILD GHOSTS AFTER THEM, which is the same rule applied a second
       time: tagpu_native.c draws the units, then the effects, then ghost_pass.
       Landing 6 first recorded the ghosts inside the body stage above, which
       put them on the wrong side of an effect they overlap -- `over` is not
       commutative and the difference is up to the ghost's own alpha share.
       Both calls or neither: the second is what ends the unit pass's frame. */
    if (draw_unit) tagpu_vk_unit_record_ghosts(&s_pass, cb, fi, w, h);
    if (draw_mark) tagpu_vk_mark_record(&s_pass, cb, fi, s_vk.rp, w, h);
}

static int vk_present(void)
{
    uint32_t idx = 0, fi = s_vk.frame % s_vk.nimg;
    VkResult r;
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    /* THE ACQUIRE IS WAITED ON AT THE TRANSFER STAGE TOO, AND THAT IS A FIX.
       [FOUND 2026-09-15 while adding the render pass below.] A semaphore wait
       blocks the stages named here and every LATER one -- and the first thing
       this command buffer does to the image is `vkCmdClearColorImage`, which
       runs at TRANSFER and is not later than COLOR_ATTACHMENT_OUTPUT. So with
       COLOR_ATTACHMENT_OUTPUT alone the clear was free to run before the
       acquire had actually handed the image over: a write to an image the
       presentation engine may still be reading, which is the silent kind of
       fault -- it does not throw, it tears a frame now and then on a driver
       that happens to overlap. */
    VkPipelineStageFlags waitst = VK_PIPELINE_STAGE_TRANSFER_BIT |
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkClearColorValue col;
    VkCommandBuffer cb;

    /* A SECOND IS NOT A FRAME, AND IT IS NOT A HICCUP EITHER. A clear-to-colour
       that has not completed in a second means the device is wedged, so this is
       fatal rather than a skipped frame -- returning 0 here used to leave the
       caller presenting nothing, for ever, at one frame a second, in silence. */
    if (vkWaitForFences(s_vk.dev, 1, &s_vk.fence[fi], VK_TRUE, 1000000000ull) != VK_SUCCESS) {
        vklog("a frame fence did not signal within a second - the device is not answering");
        return -2;
    }

    /* ---- tagpu_ftime: THIS SLOT'S PREVIOUS PAIR, AND IT COSTS NO WAIT.
       The fence above has just signalled, which is precisely the statement that
       everything slot `fi` submitted last time round has completed -- so its two
       timestamps are available and `vkGetQueryPoolResults` is asked WITHOUT the
       WAIT bit. That is the whole reason the read lives here rather than after
       the submit: a harness that blocks the thread it is measuring measures the
       block. */
    if (s_vk.tsPool && s_vk.tsPend[fi]) {
        uint64_t ts[2] = { 0, 0 };
        s_vk.tsPend[fi] = 0;
        if (vkGetQueryPoolResults(s_vk.dev, s_vk.tsPool, fi * 2u, 2,
                                  sizeof ts, ts, sizeof ts[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            ts[1] > ts[0])
            tagpu_ftime_vk_sample((double)(ts[1] - ts[0]) * s_vk.tsPeriod);
    }

    /* THE TEARDOWN A PASS OWES, PAID WHERE IT IS LEGAL TO PAY IT.
       [FROM THE G19e LANDING REVIEW, 2026-09-15 -- both reviewers, separately.]
       A pass that cannot get its per-slot resources mid-frame stops drawing and
       raises this flag INSTEAD of destroying anything, because at the moment it
       finds out, the fence wait above has proved one slot idle and nothing
       more: every other slot's submit is still executing against that pass's
       pipeline, pool and images, and the command buffer of the frame in hand
       already names them too.

       Here is the one place in the frame where the debt can be settled. It is
       before `cb` is reset and recorded, so no command names those objects yet,
       and `vkDeviceWaitIdle` makes "no submit names them either" a fact -- the
       same proof vk_down and vk_resize use. An unresolved entry point is NOT
       idle: with no way to prove the device quiet, the lane comes down rather
       than destroy on a hope. The cost is one drain on the frame after a
       refusal, and nothing at all on every other frame. */
    if (tagpu_vk_terr_down_owed() || tagpu_vk_feat_down_owed() ||
        tagpu_vk_fx_down_owed() || tagpu_vk_scaffold_down_owed() ||
        tagpu_vk_shadow_down_owed() || tagpu_vk_unit_down_owed() ||
        tagpu_vk_hires_down_owed() || tagpu_vk_mark_down_owed() ||
        tagpu_vk_gui_down_owed() || tagpu_vk_surf_down_owed() ||
        tagpu_vk_world_down_owed()) {
        if (!vkDeviceWaitIdle || vkDeviceWaitIdle(s_vk.dev) != VK_SUCCESS) {
            vklog("vkDeviceWaitIdle refused before an owed pass teardown - down");
            return -2;
        }
        vklog("a pass asked to come down: the device is drained, tearing it down");
        /* `_down_paid`, not `_down`: settling the debt is what tells the pass
           this teardown is ITS teardown, so the ST_REFUSED latch applies here
           and not to a `vk_down` or `vk_resize` that merely happened to run
           first. [G19e RE-REVIEW, 2026-09-15.] */
        if (tagpu_vk_terr_down_owed())     tagpu_vk_terr_down_paid(&s_pass);
        if (tagpu_vk_feat_down_owed())     tagpu_vk_feat_down_paid(&s_pass);
        if (tagpu_vk_fx_down_owed())       tagpu_vk_fx_down_paid(&s_pass);
        if (tagpu_vk_scaffold_down_owed()) tagpu_vk_scaffold_down_paid(&s_pass);
        if (tagpu_vk_shadow_down_owed())   tagpu_vk_shadow_down_paid(&s_pass);
        if (tagpu_vk_unit_down_owed())     tagpu_vk_unit_down_paid(&s_pass);
        if (tagpu_vk_hires_down_owed())    tagpu_vk_hires_down_paid(&s_pass);
        if (tagpu_vk_mark_down_owed())     tagpu_vk_mark_down_paid(&s_pass);
        if (tagpu_vk_gui_down_owed())      tagpu_vk_gui_down_paid(&s_pass);
        if (tagpu_vk_surf_down_owed())     tagpu_vk_surf_down_paid(&s_pass);
        if (tagpu_vk_world_down_owed())    tagpu_vk_world_down_paid(&s_pass);
    }

    r = vkAcquireNextImageKHR(s_vk.dev, s_vk.sc, 1000000000ull,
                              s_vk.semAcquire[fi], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return -1;   /* the caller rebuilds */
    if (r == VK_SUBOPTIMAL_KHR) s_vk.rebuild = 1;   /* usable; rebuild after */
    /* EVERYTHING ELSE IS FATAL AND SAYS SO. [FROM REVIEW 2026-09-15, second
       pass.] This used to `return 0` in silence, which is the same defect the
       fence and the submit below were just declared fatal for: VK_TIMEOUT
       burns the full second and comes back, DEVICE_LOST and SURFACE_LOST come
       back for ever, and the lane presents nothing at one frame a second with
       GL still swapping underneath and not a line in the log. */
    else if (r != VK_SUCCESS) {
        vklog("vkAcquireNextImageKHR: %s (%d) - down", res_name(r), (int)r);
        return -2;
    }
    /* BOUNDED, AND A BREACH IS FATAL RATHER THAN A SKIP. `vk_swapchain` refuses
       a swapchain with more images than MAXIMG, so this cannot fire -- and if
       it ever did, the acquire above has already SIGNALLED `semAcquire[fi]`
       with nothing left to wait on it, and the next frame would hand the same
       signalled semaphore back to `vkAcquireNextImageKHR`, which the
       specification forbids. Tearing the lane down is the only exit that does
       not carry that state forward. [FROM REVIEW 2026-09-15.] */
    if (idx >= s_vk.nimg) {
        vklog("the swapchain handed back image %u of %u - down", idx, s_vk.nimg);
        return -2;
    }

    /* THE FENCE ABOVE IS THE CAPTURE'S PROOF TOO. It has just been waited on,
       so the submit that used this slot -- and any copy recorded into it -- has
       completed, and the staging buffer is ours to read. No second wait. */
    if (s_abSlot1 == (int)fi + 1) {
        s_abSlot1 = 0;
        tagpu_vk_shot_finish(&s_pass, s_abPath);
    }

    vkResetFences(s_vk.dev, 1, &s_vk.fence[fi]);

    cb = s_vk.cmd[fi];
    vkResetCommandBuffer(cb, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    /* RESET BEFORE WRITE, AND ON THE DEVICE. A timestamp query must be reset
       between uses, and `vkCmdResetQueryPool` is the only form every Vulkan 1.0
       device has (`vkResetQueryPool` is 1.2). It is recorded here, at the top of
       the buffer, so the reset is ordered before the write that follows it by
       the command buffer itself rather than by anything we argue.
       The lever is read each frame: arming mid-session starts sampling on the
       next slot round rather than needing a relaunch. */
    if (s_vk.tsPool && tagpu_ftime_armed()) {
        vkCmdResetQueryPool(cb, s_vk.tsPool, fi * 2u, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_vk.tsPool, fi * 2u);
        s_vk.tsPend[fi] = 1;
    }

    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s_vk.img[idx];
    b.subresourceRange = rng;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    col.float32[0] = s_clear[0]; col.float32[1] = s_clear[1];
    col.float32[2] = s_clear[2]; col.float32[3] = 1.0f;
    vkCmdClearColorImage(cb, s_vk.img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);

    /* THE PASSES, AND THE RENDER PASS RUNS WHETHER OR NOT ONE DRAWS. It is what
       takes the image from TRANSFER_DST to PRESENT_SRC -- the transition this
       used to do with a second barrier -- so skipping it on a frame with
       nothing to draw would present an image in the wrong layout.

       `prepare` is OUTSIDE it and `record` INSIDE, because a pass's texture
       upload is a transfer and a transfer may not be recorded inside a render
       pass. `fi` is the frame slot: the fence wait at the top of this function
       is what proves the GPU has finished with that slot's buffers, and it is
       the only thing that does. */
    {
        int draw_surf = 0;
        int draw_world = 0;
        uint32_t tw = 0, th = 0;    /* the world target's extent, when there is one */
        int draw_fps = 0, draw_scaf = 0, draw_feat = 0, draw_terr = 0, draw_fx = 0;
        int draw_mark = 0, ab_mark = 0;
        int draw_unit = 0, draw_gui = 0;
        int ab_fps = 0, ab_scaf = 0, ab_feat = 0, ab_terr = 0, ab_fx = 0;
        int ab_unit = 0, ab_gui = 0;
        int ndraw = 0, nclaim = 0;
        const char* abpath = NULL;
        int abIsWorld = 0;              /* the claimed pass draws into the world */
        int abworld = 0;                /* ...and the capture reads that target  */
        VkImage abimg = VK_NULL_HANDLE;
        VkImageLayout ablay = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkFormat abfmt = VK_FORMAT_UNDEFINED;
        uint32_t abw = 0, abh = 0;
        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        if (s_vk.rp && s_vk.fb[idx]) {
            /* THE PORTED PASSES, IN THE GL LANE'S OWN ORDER. tagpu_overlay.c
               draws the scaffold before the readout, and two passes that blend
               are not commutative -- so the order here is that order, not the
               order the files were written in. */
            /* Terrain is the world's bottom layer and the frame's far plane --
               tagpu_native.c draws it first of all and everything after it is
               depth-tested against what it wrote -- so it goes first here too.
               The features are the WORLD as well and follow it, before the
               scaffold overlay and long before the readout. */
            /* THE SHADOW MAP IS FIRST, AND IT IS NOT ONE OF THE FRAME'S
               PASSES. It draws into an offscreen depth image of its OWN -- a
               render pass inside this one's `prepare`, which is legal here and
               nowhere else, because render passes may not nest and `prepare`
               runs before vkCmdBeginRenderPass below. Every pass that samples
               the map points its descriptor set at
               `tagpu_vk_shadow_view(fi)` during its own `prepare`, so this
               call has to come first; that ordering is the whole contract and
               it is stated in tagpu_vk_shadow.h as well.
               ITS RESULT IS DELIBERATELY NOT ADDED TO `ndraw` OR `nclaim`.
               Those two count the passes that put pixels in THIS frame, and
               they exist to catch two of them contaminating one A/B capture.
               This pass puts none there, so counting it would refuse every
               capture taken while Classic++ shadows are on -- which is exactly
               the configuration the shadow work is measured in. */
            /* ---- AND `prepare` ORDER IS NOT `record` ORDER, since G19e's
               sixth pass. Three of the hooks below run in an order the DATA
               forces, and the draws still happen in the GL lane's order inside
               the render pass further down. ---- */

            /* TA'S OWN SCREEN IS UPLOADED BEFORE ANYTHING OF OURS, because it
               is what everything of ours is drawn OVER. On the GL lane the fork
               does this itself before any pass runs (render_ogl.c's
               `g_ogl.main_program`), which is why `tagpu_gui.off` still shows a
               game there and showed a flat clear here.
               [The vulkan-only plan, landing 4c-1.] */
            /* THE WORLD TARGET IS BUILT FIRST OF ALL, and the order is a
                contract rather than a preference. It is not a pass and depends
                on none of them -- it reads the geometry tagpu_native.c
                published from this frame's gather -- but the passes depend on
                IT: any pass whose GL twin scales something by `ss` asks
                `tagpu_vk_world_scale()` during its own `prepare`, and that
                answer does not exist until this has run. It was last of
                `prepare` when 4c-2 was written, which left the effects and
                marker passes setting an `ss`-wide line from their hand-over
                while the target might have refused and the world be going into
                the swapchain image at 1:1. [FROM THE 4c-2 LANDING REVIEW.]
                It puts no pixel anywhere, so it is not counted in
                `ndraw`/`nclaim` -- the shadow map's rule for its reason. 0 means
                there is no target this frame and the world draws into the
                swapchain image exactly as it did before 4c-2: a smaller
                picture, never a wrong one. */
            draw_world = tagpu_vk_world_prepare(&s_pass, cb, fi, &tw, &th);

            draw_surf = tagpu_vk_surf_prepare(&s_pass, cb, fi);

            /* THE SCAFFOLD'S UPLOAD IS FIRST OF OURS, though its DRAW is nearly last.
               The G12a overlay is a texture the unit, hi-res and effects
               fragment shaders sample (`uScafOn`), so the pass that fills it
               has to have filled it before a consumer points a descriptor set
               at it -- and a consumer does that in its own `prepare`.
               tagpu_vk_scaffold.h states the contract; its `record` is still
               where it always was, over the world. */
            draw_scaf = tagpu_vk_scaffold_prepare(&s_pass, cb, fi);
            ab_scaf = tagpu_vk_scaffold_ab_frame();

            /* THEN THE UNIT PASS'S UPLOAD, because the shadow map's casters
               are its geometry: its vertex buffers, pose blocks and texels all
               have to exist before the map is drawn. It puts no pixel anywhere
               yet and its own descriptor set is not written here -- that is
               `tagpu_vk_unit_prepare`, below the map. */
            tagpu_vk_unit_upload(&s_pass, cb, fi);
            /* AND THE REPLACEMENT MESHES' CASTERS, beside it and for the same
               reason: they are geometry the shadow map is about to draw, so
               they have to be on the device before it is. It draws nothing
               here either. [Gate 3b.] */
            tagpu_vk_hires_upload(&s_pass, cb, fi);

            tagpu_vk_shadow_prepare(&s_pass, cb, fi);

            /* ...AND THE UNIT PASS'S `prepare` AFTER IT, which is the one
               instant this slot's set may be pointed at the map the call above
               just drew and at the overlay the call above that just filled. */
            draw_unit = tagpu_vk_unit_prepare(&s_pass, cb, fi);
            ab_unit = tagpu_vk_unit_ab_frame();

            draw_terr = tagpu_vk_terr_prepare(&s_pass, cb, fi);
            ab_terr = tagpu_vk_terr_ab_frame();
            draw_feat = tagpu_vk_feat_prepare(&s_pass, cb, fi);
            ab_feat = tagpu_vk_feat_ab_frame();
            /* The effects are the LAST of the world, after the features and
               the units -- tagpu_native.c draws them there so that lasers and
               explosions sit over everything the world put down and under the
               UI. Two passes that blend are not commutative, so this order is
               the GL lane's and not the order the files were written in. */
            draw_fx = tagpu_vk_fx_prepare(&s_pass, cb, fi);
            ab_fx = tagpu_vk_fx_ab_frame();
            /* THE MARKERS ARE THE FRAME'S TOP LAYER, above the world and below
               the UI -- where tagpu_native.c draws them (landing 5). */
            draw_mark = tagpu_vk_mark_prepare(&s_pass, cb, fi);
            ab_mark = tagpu_vk_mark_ab_frame();
            /* THE UI LAYER, whose whole replay is in `prepare`: a twin is
               drawn into with its own render pass and render passes may not
               nest, so this is the hook it has to be in -- the shadow pass's
               shape, for the same reason. */
            draw_gui = tagpu_vk_gui_prepare(&s_pass, cb, fi);
            ab_gui = tagpu_vk_gui_ab_frame();
            draw_fps = tagpu_vk_fps_prepare(&s_pass, cb, fi);
            ab_fps = tagpu_vk_fps_ab_frame();

            /* THE CLASSIC++ RESTORER'S SLICE IS LAST OF `prepare`, AND IT IS
               NOT ONE OF THE FRAME'S PASSES EITHER -- the shadow map's shape,
               for the shadow map's reasons, so the warning above applies
               word for word and `ndraw`/`nclaim` below do not count it.

               LAST, unlike the shadow map, because it is the only one of the
               two whose CONSUMERS feed it: a pass hands it a job and a frame
               list in its own `prepare`, so every one of those has to have
               run before the slice that drains them -- the reverse of the
               shadow map's ordering and the same argument. The slice then
               paints into images the render pass below samples; what makes
               that write visible to those samples is the OUT render pass's
               own subpass dependency, stated in tagpu_vk_restore.c, and not
               the accident of a render-pass boundary.

               Its own render passes are begun in here, which is legal in
               `prepare` and nowhere else because render passes may not nest
               and this runs before vkCmdBeginRenderPass. It is a no-op until
               a consumer has asked the device for it. */
            tagpu_vk_restore_step(&s_pass, cb, fi);

        }
        ndraw = draw_terr + draw_feat + draw_unit + draw_fx + draw_mark + draw_scaf +
                draw_gui + draw_fps;
        nclaim = ab_terr + ab_feat + ab_unit + ab_fx + ab_mark + ab_scaf + ab_gui + ab_fps;
        /* THE CLAIMED PASS'S FILE, out of the one list of them in this file
           (`s_abFiles`, above). The row order is the order the nested ternary
           this replaced tested in, so which pass wins a (refused) multi-claim
           frame is unchanged -- and `tagpu_vk_ab_arm` reads the same list, so
           the name the capture is WRITTEN to and the name the arming UNLINKS
           cannot drift apart.

           THE UNLINK IS NOT DONE HERE, and that is the point of it. See
           `tagpu_vk_ab_arm`: a claim is latched on the gather side and this
           function is not on the path from that latch -- `tagpu_vk_frame`
           returns before `vk_present` all through the bring-up, on a swapchain
           rebuild, at ST_FAILED and at ST_ZOMBIE, and `vk_present` itself
           returns early on an out-of-date acquire. An unlink here would
           therefore be reached on most frames and missed on exactly the frames
           where a stale capture is likeliest, which is a guarantee about timing
           wearing the words of one about construction.
           [FROM THE 4b-1 LANDING REVIEW, 2026-09-18.] */
        {
            const int abclaim[8] = { ab_terr, ab_feat, ab_unit, ab_fx,
                                     ab_mark, ab_scaf, ab_gui, ab_fps };
            int abi;
            abpath = NULL;
            for (abi = 0; abi < 8 && !abpath; abi++)
                if (abclaim[abi]) {
                    abpath = s_abFiles[abi].path;
                    /* THE FIRST FIVE ROWS ARE THE WORLD, and the row order is
                       the one thing this list is FOR -- it is the same array the
                       path came out of, so "which file" and "which target" can
                       no more drift apart than the path and the unlink can.
                       [The vulkan-only plan, landing 4c-3.] */
                    abIsWorld = abi < 5;
                }
        }

        /* ---- THE WORLD, INTO ITS OWN TARGET AND BEFORE THE FRAME'S PASS ----
           Render passes may not nest, so this has to open and close before
           vkCmdBeginRenderPass below -- the shadow map's constraint, met the
           same way. What it changes for the five passes inside it is the EXTENT
           they are handed and nothing else: they scale the engine's game-space
           rect into whatever target they get (`terr_scissor`'s `sx = w/uGame.x`
           yields GL's own `glScissor(vpL*ss, ...)` at `tw = gw*ss`) and their
           vertex shaders divide by `uGame` rather than by the target. The
           offscreen render pass is format-compatible with `s_vk.rp`, so not one
           of their pipelines moved. [The vulkan-only plan, landing 4c-2.] */
        if (draw_world) {
            tagpu_vk_world_begin(&s_pass, cb, fi);
            world_records(cb, fi, tw, th,
                          draw_terr, draw_feat, draw_unit, draw_fx, draw_mark);
            tagpu_vk_world_end(&s_pass, cb);
        }

        if (s_vk.rp && s_vk.fb[idx]) {
            VkClearValue cv[2];
            memset(cv, 0, sizeof cv);
            /* cv[0] is never used -- the colour attachment's loadOp is LOAD and
               G19a's vkCmdClearColorImage above is what applies the lever's
               colour -- but the array has to reach the index of the attachment
               that DOES clear, which is the depth one. cv[1] is 1.0, the value
               glClear(GL_DEPTH_BUFFER_BIT) uses on the GL side. */
            cv[1].depthStencil.depth = 1.0f;
            rbi.renderPass = s_vk.rp;
            rbi.framebuffer = s_vk.fb[idx];
            rbi.renderArea.extent = s_vk.ext;
            rbi.clearValueCount = s_vk.dfmt != VK_FORMAT_UNDEFINED ? 2 : 0;
            rbi.pClearValues = cv;
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            /* THE BOTTOM LAYER, FIRST AND OVER THE CLEAR. It is opaque and
               depth-testless, so it neither reads nor writes anything the world
               passes below it depend on; the clear that ran before the pass
               still carries the letterbox and any frame this refused.

               ...AND NOT ON A FRAME AN A/B HAS CLAIMED, which is the one thing
               that makes a world pass's A/B mean anything again. The GL half of
               such a capture is the bare world FBO -- black wherever the pass
               did not draw. Since 4c-1 the Vulkan half is the swapchain image
               with TA's own frame UNDERNEATH, so the two stopped being the same
               kind of picture: for an opaque pass every uncovered pixel
               differed, and for a pass that BLENDS every translucent fragment
               differed too, because one lane composites the effect over the
               game and the other over black. Measured on the effects pass
               before this line existed: of 1 622 pixels the GL capture drew,
               226 agreed (the opaque fragments, byte-for-byte) and 1 396 did
               not (median max-channel 19 -- the translucent ones).

               `nclaim` is this frame's armed A/B levers, counted above and
               before the render pass begins, so this costs exactly the frames
               a measurement is being taken on and nothing else. It is the same
               kind of measurement-only divergence as `tagpu_vk.on`'s
               `color=0,0,0`, which exists so the two clears agree.
               [FROM THE 4c-2 LANDING REVIEW.]

               WHAT STILL DEPENDS ON THIS, AFTER 4c-3 AND 4d-2 -- and the
               reason changed under it, so read this rather than the shape.
               A world capture no longer reads this image at all: it reads the
               offscreen world target, which TA's frame never reaches. The
               scaffold, GUI and readout captures DO still read this image, and
               what a capture has to be is THE PASS ALONE OVER THE CLEAR
               COLOUR -- that is what makes it comparable to anything, whether
               the other side of the comparison is another lane or another
               BUILD. TA's own frame underneath turns every uncovered pixel into
               a difference and every blended fragment into a composite over the
               game instead of over the clear, which is exactly the regression
               4c-1 introduced and 4c-2's review caught.

               THE OLD REASON IS GONE AND DID NOT MATTER. This used to argue
               from the GL half -- "their GL halves are still the default
               framebuffer blacked by `tagpu_abshot_begin`" -- and landing 4d-2
               deleted that function with the rest of the GL capture. The line
               stays because the argument above never needed a second lane:
               a capture of one pass over a clear is the thing being measured.
               [THE PREMISE WAS CAUGHT BY THE 4d-2 LANDING REVIEW.] Measured
               with the line in: the GUI A/B is 0 px of 307 200 (2026-09-18). */
            if (draw_surf && nclaim == 0)
                tagpu_vk_surf_record(&s_pass, cb, fi, s_vk.ext.width, s_vk.ext.height);
            /* THE WORLD. With a target it was drawn into it above and this is
               the one draw that puts it on the frame, over TA's own surface and
               under the UI -- which is exactly where tagpu_native.c's composite
               sits. Without one the five passes draw straight into the
               swapchain image at client resolution, which is what this lane did
               before 4c-2. Both arms call `world_records`, so the GL lane's
               ORDER is written down once and cannot drift between them. */
            if (draw_world)
                tagpu_vk_world_record(&s_pass, cb, fi, s_vk.ext.width, s_vk.ext.height);
            else
                world_records(cb, fi, s_vk.ext.width, s_vk.ext.height,
                              draw_terr, draw_feat, draw_unit, draw_fx, draw_mark);
            /* THE UI IS ABOVE THE WORLD and below the readout, which is
               where tagpu_overlay_draw puts it. */
            if (draw_gui) {
                tagpu_vk_gui_record(&s_pass, cb, fi, s_vk.ext.width, s_vk.ext.height);
                /* RECORDED, WHICH IS AS FAR AS THIS FILE CAN HONESTLY GO. The
                   command buffer now carries the composite and the submit below
                   is unconditional, so the only thing between here and the
                   screen is the present -- and a present that fails takes the
                   whole frame with it, cursor included. See `s_uiDrew`. */
                s_uiDrew = 1;
            }
            if (draw_scaf)
                tagpu_vk_scaffold_record(&s_pass, cb, fi, s_vk.ext.width, s_vk.ext.height);
            if (draw_fps)
                tagpu_vk_fps_record(&s_pass, cb, fi, s_vk.ext.width, s_vk.ext.height);
            vkCmdEndRenderPass(cb);
        } else {
            /* No render pass: nothing can draw, so the clear is the frame and
               the layout has to be moved by hand exactly as it was before
               G19d. */
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        }

        /* THE A/B CAPTURE, ONE FRAME, UNDER THE LEVER THE CLAIMING PASS SHARES
           WITH ITS GL TWIN. Recorded last so that what it reads is the finished
           frame, and it leaves the image in PRESENT_SRC so the present below is
           unaffected.

           ONE PASS IN THE FRAME, OR NO CAPTURE. [G19e.] Each GL twin captures
           its own half by blacking the frame, drawing ITSELF and reading back
           immediately -- so a GL half contains exactly one pass. This side is
           one frame with every armed pass drawn into it. With two A/B levers
           armed at once the Vulkan half would therefore hold two passes and
           each GL half one, and the diff would report every pixel of the OTHER
           pass as a difference: an oracle failure that reads exactly like a
           broken port, which is the worst answer an oracle can give. Both
           conditions are checked, not just the claim count -- a second pass
           that drew without claiming contaminates the frame just as much. */
        /* THE LANE'S OWN PER-FRAME CENSUS, throttled. The refusals below say
           why a capture was NOT taken when they fire, but say nothing at all
           when the claim simply never arrived -- and a claim that never arrives
           is indistinguishable, from outside, from a pass that drew nothing.
           This line separates the two, and it is the third time in this landing
           that a periodic report of the CURRENT state settled in one run what a
           one-shot latch had hidden. */
        /* KEYED ON THE DRIVER'S FRAME, not the lane's own present counter, so
           this line can be lined up against the passes' -- two censuses on two
           counters print on different frames and cannot be compared, which cost
           this landing a round. */
        if ((s_pass.frame % 300u) == 0u)
            vklog("census: frame %u: %d pass(es) drew and %d claimed (terr=%d "
                  "feat=%d unit=%d fx=%d mark=%d scaf=%d gui=%d fps=%d)",
                  (unsigned)s_pass.frame, ndraw, nclaim, draw_terr, draw_feat,
                  draw_unit, draw_fx, draw_mark, draw_scaf, draw_gui, draw_fps);
        /* WHICH IMAGE THE CAPTURE READS, AND IT IS NOT ALWAYS THE FRAME.
           [The vulkan-only plan, landing 4c-3.]

           A WORLD PASS'S GL HALF IS THE WORLD FBO, NOT THE WINDOW. tagpu_native.c
           binds it and sets `glViewport(0, 0, gw*ss, gh*ss)`; tagpu_abshot.c
           blacks it, lets the pass draw and reads THAT viewport back. So the GL
           half is `gw*ss` by `gh*ss` -- the game's own resolution times the
           supersample factor -- and it has never been the window's client rect.
           Reading the swapchain image for the Vulkan half therefore produced two
           files of the same size only at `ss = 1` with no letterbox, and `ss` is
           2 unless `tagpu_ss.off` is there. Four passes answered that by
           REFUSING their own A/B whenever `ss != 1`; posedraw did not, and wrote
           a pair tools/vk-ab.py then refused by size.

           Landing 4c-2 gave this lane a `gw*ss, gh*ss` image holding the world
           alone over a transparent clear, which is what GL's FBO is. Reading it
           makes the two halves the same size and the same kind of picture at
           every `ss`, so a world A/B runs on the SHIPPED configuration for the
           first time. It also makes the capture independent of everything the
           swapchain image carries -- TA's own frame included -- which is the
           divergence the `nclaim == 0` line above exists for.

           `tagpu_vk_world_shot` answers about THIS frame's command buffer: it
           names the slot the world render pass was actually opened on, so a
           frame with no target (the world on the swapchain image, the fallback
           path) returns 0 here and is refused BY NAME below rather than captured
           at the wrong size and diagnosed afterwards by the diff tool. */
        if (nclaim == 1) {
            abimg = s_vk.img[idx];
            ablay = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            abw = s_vk.ext.width;
            abh = s_vk.ext.height;
            abfmt = s_vk.fmt;
            /* THE FORMAT COMES BACK WITH THE IMAGE, and that is the point of
               asking for it: the capture picks its channel order from a format,
               and `s_pass.fmt` is copied from `s_vk.fmt` once at bring-up while
               `vk_resize` re-picks `s_vk.fmt` and refreshes only `s_pass.slots`.
               Taking it from the module that built the image makes the pairing
               true by construction instead of by the two staying in step.
               [FROM THE 4c-3 LANDING REVIEW.] */
            if (abIsWorld && tagpu_vk_world_shot(fi, &abimg, &abw, &abh, &abfmt)) {
                /* The offscreen pass's `finalLayout`, and `tagpu_vk_shot_record`
                   puts it back -- which costs nothing, because that pass declares
                   `initialLayout = UNDEFINED` and does not care what it finds. */
                ablay = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                abworld = 1;
            }
        }
        if (nclaim > 1 || (nclaim == 1 && ndraw > 1))
            vklog("%d A/B levers claimed this frame and %d passes drew into it - "
                  "nothing captured. A Vulkan frame carries every armed pass at "
                  "once while each GL capture carries one, so arm one pass's .ab "
                  "at a time (and turn the other pass's .on off).", nclaim, ndraw);
        else if (nclaim == 1 && abIsWorld && !abworld)
            vklog("a world pass claimed the A/B and this frame has no world "
                  "target - its GL half is the gw*ss FBO and the only image here "
                  "is the window's client rect, which tools/vk-ab.py refuses as "
                  "two sizes. Nothing captured; the line above says why the "
                  "target stood down.");
        /* TRANSFER_SRC IS THE SURFACE'S PROBLEM, so it is only asked about the
           surface's image. The world target is ours and is CREATED with the
           usage (tagpu_vk_world.c: `mk_att`), so a device that would not give us
           one has already failed vkCreateImage and stood the target down. */
        else if (nclaim == 1 && !abworld && !s_vk.cansrc)
            vklog("the A/B asked for a capture and this surface's images do not "
                  "carry TRANSFER_SRC - only the GL half will be written");
        else if (nclaim == 1 && tagpu_vk_shot_record(&s_pass, cb, abimg, ablay,
                                                     abw, abh, abfmt)) {
            s_abSlot1 = (int)fi + 1;
            s_abPath = abpath;
        }
    }
    /* BOTTOM_OF_PIPE, paired with the TOP_OF_PIPE above: between them lies
       every stage of everything this buffer recorded, which is this lane's
       whole frame -- the same span the GL bracket takes on its side. */
    if (s_vk.tsPool && s_vk.tsPend[fi])
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_vk.tsPool, fi * 2u + 1u);
    vkEndCommandBuffer(cb);

    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s_vk.semAcquire[fi];
    si.pWaitDstStageMask = &waitst;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s_vk.semRelease[idx];
    /* A FAILED SUBMIT IS FATAL, because the fence was reset a few lines above
       and nothing will ever signal it again: `s_vk.frame` does not advance on
       an early return, so `fi` would stay on that fence and every later frame
       would burn the full one-second wait. Silently, at one frame a second,
       with GL still swapping underneath. [FROM REVIEW 2026-09-15.] */
    if (vkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence[fi]) != VK_SUCCESS) {
        vklog("vkQueueSubmit refused the frame - down");
        return -2;
    }

    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &s_vk.semRelease[idx];
    pi.swapchainCount = 1; pi.pSwapchains = &s_vk.sc; pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(s_vk.queue, &pi);
    s_vk.frame++;
    /* A pending capture is NOT touched on any exit from here: the buffer is
       owned by the GPU until this slot's fence is waited on again, and every
       path that cannot get there -- a rebuild, a teardown -- goes through
       `vkDeviceWaitIdle` and then `ab_drop`. */
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return -1;
    /* SUBOPTIMAL: the frame WAS presented, so it is counted -- but the surface
       has moved on and the next frame should be drawn against a fresh
       swapchain. Rebuilding here instead would throw away a good frame. */
    if (r == VK_SUBOPTIMAL_KHR) { s_vk.rebuild = 1; return 1; }
    if (r != VK_SUCCESS) { vklog("vkQueuePresentKHR: %s - down", res_name(r)); return -2; }
    return 1;
}

/* Rebuild the swapchain in place: the render thread owns `s_vk` in ST_READY, so
   this is single-owner work. Everything sized by the swapchain goes with it. */
/* 1 rebuilt, 0 the lane must come down, -1 not yet (a zero extent). */
static int vk_resize(int w, int h)
{
    int r;
    int idle = vkDeviceWaitIdle(s_vk.dev) == VK_SUCCESS;
    if (!idle) vklog("vkDeviceWaitIdle refused before a swapchain rebuild");
    /* THE PASSES GO BACK ACROSS A RESIZE TOO, and the reason is the slot count:
       a rebuilt swapchain may come back with a different number of images, and
       a pass that kept buffers for the old count would be handed a slot index
       past the end of them. The wait above is what makes dropping them safe.
       (Their PIPELINES could have survived -- the render pass does -- but a
       pass that rebuilds everything has one path instead of two, and a resize
       is a window drag, not a frame.) */
    tagpu_vk_fps_down(&s_pass);
    tagpu_vk_scaffold_down(&s_pass);
    tagpu_vk_feat_down(&s_pass);
    tagpu_vk_terr_down(&s_pass);
    tagpu_vk_fx_down(&s_pass);
    tagpu_vk_shadow_down(&s_pass);
    tagpu_vk_unit_down(&s_pass);
    tagpu_vk_hires_down(&s_pass);
    tagpu_vk_gui_down(&s_pass);
    tagpu_vk_surf_down(&s_pass);
    tagpu_vk_world_down(&s_pass);
    /* AND ON A RESIZE TOO, though the device survives one: the restorer's
       staging and its timestamp pairs are sized by `d->slots`, which is
       `s_vk.nimg` and can change under a swapchain rebuild. The passes above
       have already dropped everything for the same reason, so their jobs are
       gone and the restore would restart from scratch whatever this line did
       -- keeping the shared build would buy a rebuild we have no use for. */
    tagpu_vk_restore_down(&s_pass);
    ab_drop("the swapchain rebuilding", idle);
    vk_perimage_free();
    r = vk_swapchain(w, h);
    if (r <= 0) return r;
    if (!vk_perimage()) return 0;
    s_pass.slots = s_vk.nimg;
    return 1;
}

int tagpu_vk_frame(HWND hwnd, int w, int h, int vsync, unsigned frame_counter)
{
    LONG st;
    DWORD now = GetTickCount();

    /* NOTHING HAS COMPOSITED YET, and every early return below must leave this
       saying so. See `s_uiDrew`. */
    s_uiDrew = 0;

    /* THIS FRAME'S NUMBER, BEFORE ANY PASS CAN ASK FOR IT. Every `prepare`
       below reaches a GL module's hand-over through it, and refuses one that
       was published on a different frame. [G19e RE-REVIEW, 2026-09-15.] */
    s_pass.frame = frame_counter;

    /* THE LEVER FIRST AND CHEAPLY. With `tagpu_vk.on` absent this returns 0
       here, on a cached answer refreshed every 250 ms, so the GL lane's frame
       is what it was before this file existed. */
    if (s_armed < 0 || (DWORD)(now - s_lastPoll) >= POLL_MS) { s_lastPoll = now; read_lever(); }

    /* THE DISARMED FRAME COSTS A PLAIN LOAD. `lane_state()` is a `lock cmpxchg`
       and the barrier it carries is only owed when something is actually going
       to be read behind it -- with the lever off and the lane at rest there is
       nothing, and this is the render thread's per-frame path. */
    if (!s_armed && s_state == ST_OFF) return 0;

    st = lane_state();

    /* THE WORKER'S HANDLE IS REAPED WHEREVER IT FINISHES, not only on the way
       into ST_READY. A lever cleared while a bring-up was still running used to
       leave the handle open for the session: the disarm branch below returns
       before any close, and the state never passes through ST_READY again. Any
       state but ST_STARTING means the worker has published and exited. */
    if (st != ST_STARTING && s_worker) { CloseHandle(s_worker); s_worker = NULL; }

    if (!s_armed) {
        /* Disarmed: give the objects back, and let a FAILED lane retry the next
           time the player arms it rather than once per launch. There is no
           window to take down with them since landing 4d-1 -- the surface was on
           the game's own window, which is not ours to destroy. */
        if (st == ST_READY || st == ST_FAILED) {
            if (st == ST_READY) { vk_down(); vklog("lever off - down"); }
            InterlockedExchange(&s_state, ST_OFF);
        }
        return 0;
    }

    switch (st) {
    case ST_OFF: {
        HWND win;
        if (!hwnd || w <= 0 || h <= 0) return 0;
        /* NOT BACK UP ON A WINDOW WE WATCHED DIE. Without this the rebuild the
           `s_ownGone` test forces would fall straight into another bring-up on
           the same dead HWND, once a frame. The render thread is being stopped
           on every path that destroys the game window, so refusing here is a
           stop rather than a wait. */
        if (s_ownWin && s_ownGone) return 0;
        s_owner = hwnd;
        /* THE CALLER'S WINDOW, AND NOTHING ELSE. Before landing 4d-1 this chose
           between the game window and a popup of ours, and asked another thread
           to make the popup if it did not exist yet -- a create posted to the
           thread that pumps, then a frame's wait for it to arrive. With no
           second backend there is nothing to separate and nothing to wait for:
           `hwnd` is already known non-NULL two lines above. */
        win = hwnd;
        s_want.hwnd = win; s_want.w = w; s_want.h = h; s_want.vsync = vsync;
        s_choiceSeen = lane_gen();
        /* OFF -> STARTING before the thread exists, so two frames cannot start
           two workers even if this were ever called from two threads. */
        if (InterlockedCompareExchange(&s_state, ST_STARTING, ST_OFF) != ST_OFF) return 0;
        s_worker = CreateThread(NULL, 0, up_worker, NULL, 0, NULL);
        if (!s_worker) {
            vklog("could not start the bring-up thread (%lu)", GetLastError());
            InterlockedExchange(&s_state, ST_FAILED);
        }
        return 0;
    }

    case ST_FAILED:
        /* A FAILED LANE RETRIES WHEN THE PLAYER ASKS FOR A DIFFERENT GPU.
           [FROM REVIEW 2026-09-15.] `s_choiceSeen` is latched in the ST_OFF
           branch only, so a player whose chosen device would not come up used
           to click every other row in the list and get nothing at all, for
           ever, until they found the lever file. The click is exactly the
           signal that the thing that failed is not what we would try now. */
        if (lane_gen() != s_choiceSeen) {
            vklog("the GPU row changed after a failed bring-up - trying again");
            InterlockedCompareExchange(&s_state, ST_OFF, ST_FAILED);
        }
        return 0;

    case ST_STARTING:
    case ST_ZOMBIE:
        return 0;

    case ST_READY:
        break;
    default:
        return 0;
    }

    /* Three things force the whole lane down and back up through the worker:
       the game window CHANGED (a mode switch), the game window was DESTROYED
       under us (only the wndproc observer can see that -- `s_ownGone`), or the
       player picked another GPU. Each invalidates the surface or the device and
       neither is patchable in place; going back through the worker is what keeps
       every bring-up off the render thread, not just the first.

       THERE USED TO BE A FOURTH -- "our window went away" -- and it is not a
       case when we have no window. Landing 4d-1 deleted the window. */
    if (hwnd != s_owner || s_ownGone || lane_gen() != s_choiceSeen) {
        vklog(hwnd != s_owner ? "the game window changed - rebuilding" :
              s_ownGone       ? "the game window was destroyed under us - down"
                              : "the GPU row changed - rebuilding");
        vk_down();
        InterlockedExchange(&s_state, ST_OFF);
        return 0;
    }

    /* THE SWAPCHAIN IS REBUILT WHEN VULKAN SAYS SO, NOT WHEN WE GUESS. An
       earlier version compared the caller's `w`/`h` against the swapchain's
       extent and rebuilt when they differed -- and the two are not the same
       number: `g_ddraw.render.width/height` is the RENDER TARGET's size, while
       the extent follows OUR window, which is the game's CLIENT rect. Any
       window the fork letterboxes (`--window WxH`, k != 1) makes them differ
       permanently, and the swapchain would have been torn down and rebuilt on
       every single frame for the rest of the session. `VK_ERROR_OUT_OF_DATE_KHR`
       and `VK_SUBOPTIMAL_KHR` are the surface telling us its extent moved, which
       is the same answer without the guess. `vsync` stays ours, because no
       amount of asking the surface reveals a present mode we chose. */
    if (vsync != s_vk.vsync) { s_vk.vsync = vsync; s_vk.rebuild = 1; }

    if (s_vk.rebuild) {
        int rr = vk_resize(w, h);
        if (rr < 0) return 0;          /* no extent yet: keep the flag, try later */
        s_vk.rebuild = 0;
        if (!rr) {
            vklog("the swapchain would not rebuild - down");
            vk_down();
            InterlockedExchange(&s_state, ST_FAILED);
            return 0;
        }
    }

    {
        int rc = vk_present();
        if (rc == -2) {                /* fatal: the lane cannot carry on */
            vk_down();
            InterlockedExchange(&s_state, ST_FAILED);
            return 0;
        }
        if (rc < 0) {                  /* out of date: rebuild, and skip this frame */
            s_vk.rebuild = 1;
            return 0;
        }
        return rc;
    }
}

/* THERE IS NO WAIT HERE ANY MORE, and removing it removed the last place this
   file could stall the game. [FROM REVIEW 2026-09-15.]

   It used to wait up to five seconds for a worker still in ST_STARTING. The
   game thread waits INFINITE on the render thread across a mode change
   (`dd.c`), and the render thread was waiting on the worker, so a mode change
   that caught a bring-up in flight stalled the whole LOCKSTEP world for as long
   as the bring-up had left -- measured at 371-451 ms routinely. Worse, the
   window thread IS the game thread here (`g_ddraw.gui_thread_id`), so if
   winevulkan ever reaches the window with an inter-thread send during surface
   or swapchain creation the cycle game -> render -> worker -> window closes and
   the timeout is the only thing that breaks it: load-bearing, which the file
   claimed it was not.

   None of it is needed now that a worker cleans up after itself (`up_worker`'s
   publish, and `lane_release`). Marking the lane ST_ZOMBIE tells the worker it
   has been abandoned; it finishes at its own pace, puts back exactly what it
   built and hands the lane to ST_OFF. Nothing waits, nothing leaks, and the
   thread handle can be closed while the thread still runs -- that releases our
   reference, not the thread. */
void tagpu_vk_render_stop(void)
{
    LONG st = lane_state();

    if (st == ST_STARTING) {
        if (InterlockedCompareExchange(&s_state, ST_ZOMBIE, ST_STARTING) == ST_STARTING) {
            vklog("render thread stopping mid bring-up - the worker will release it");
            if (s_worker) { CloseHandle(s_worker); s_worker = NULL; }
            return;
        }
        st = lane_state();
    }

    if (s_worker) { CloseHandle(s_worker); s_worker = NULL; }

    if (st == ST_READY || st == ST_FAILED) {
        if (st == ST_READY) { vk_down(); vklog("render thread stopping - down"); }
        InterlockedExchange(&s_state, ST_OFF);
    }
}
