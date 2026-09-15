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

   So THIS FILE TAKES ROUTE D, and the other two are not fallbacks -- they are
   broken on the wine the dev loop builds prefixes with.

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

   WHAT ROUTE D COSTS, and why it is worth it anyway. Vulkan gets a window of
   its own, owned by the game's, sized to its client area and kept there -- so
   there is a second HWND to create, track, show, hide and destroy, and that is
   real machinery where route A was none. In exchange:

     - THE GL LANE IS NEVER TOUCHED, so the lever is two-way and the menu keeps
       its invariant that no row needs a relaunch. On route A, arming Vulkan
       once would have killed the GL renderer for the session.
     - it is the shape the phase is heading for anyway. The out-of-process
       64-bit renderer owns its own window by definition, so the tracking below
       is the work that move needs, written early rather than twice.

   WHAT IS NOT ESTABLISHED, and it is the honest limit: every row of that table
   is the linux NVIDIA ICD under wine. No Windows box has run it, and on Windows
   route A may well be fine -- winevulkan's HWND takeover is a wine-side
   mechanism. Route D is correct on both regardless, which is why it is not
   worth a per-platform branch. The cell that would close it is the `_local`
   test VM.

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

   THE WINDOW BELONGS TO THE LANE while it is up or coming up and to nobody
   otherwise -- `vkw_release_unless_worker` is the whole of that rule, and the
   two states it exempts are the two in which a worker may hold a surface on it.

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
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hook.h"
#include "tagpu_vk.h"
#include "tagpu_vk_fps.h"
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
#define AB_OUT     "tagpu_fps_vk.ppm"
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
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
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
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCreateSemaphore) \
    X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) \
    X(vkAcquireNextImageKHR) X(vkQueueSubmit) X(vkQueuePresentKHR) \
    X(vkWaitForFences) X(vkResetFences) X(vkResetCommandBuffer) \
    X(vkDeviceWaitIdle)

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
    VkSemaphore      semAcquire[MAXIMG];   /* by FRAME index                  */
    VkSemaphore      semRelease[MAXIMG];   /* by IMAGE index -- see vk_present */
    VkFence          fence[MAXIMG];
    uint32_t         qfam;
    uint32_t         nimg;
    VkFormat         fmt;
    VkColorSpaceKHR  cspace;
    VkExtent2D       ext;
    HWND             hwnd;
    int              vsync;
    int              devIndex;             /* into the cached name table, or -1 */
    int              flipok;               /* VK_KHR_maintenance1 was enabled   */
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
    vklog("the A/B capture was lost to %s - only the GL half was written", why);
    tagpu_vk_shot_down(&s_pass);
}


/* The clear colour, `color=r,g,b` in the lever file. Magenta by default: no
   pixel of TA's palette is pure magenta, so "is the Vulkan lane on screen?" is
   answered by looking rather than by measuring. */
static float s_clear[3] = { 1.0f, 0.0f, 1.0f };

/* ---- route D: the Vulkan window ------------------------------------------
   ONE THREAD OWNS IT, AND IT IS THE THREAD THAT ALREADY PUMPS. A window whose
   messages nobody dispatches deadlocks anything that sends it one -- the window
   manager's broadcasts included -- so this window is created, moved, shown and
   destroyed on the GAME's window thread, reached by posting to the game window
   exactly as `tagpu_menu`'s WINDOW rows do. The render thread only presents to
   it, which needs no message pump and no ownership.

   IT IS AN OWNED POPUP, NOT A CHILD. A child was route 2 and system wine
   refuses a surface on one outright. An owned top-level window gets the
   behaviour we would otherwise have to write: it stays above its owner, it
   minimises and restores with it, and WS_EX_TOOLWINDOW keeps it out of the
   taskbar and the alt-tab list. WS_EX_NOACTIVATE and SW_SHOWNOACTIVATE keep it
   from ever taking focus.

   IT IS TRANSPARENT TO THE MOUSE. `WM_NCHITTEST` answers `HTTRANSPARENT`, so a
   click lands on the game window underneath as though this window were not
   there. The fork's injected input posts to `g_ddraw.hwnd` directly and never
   saw it either way; this is for the human's pointer.

   THE GEOMETRY IS EVENT-DRIVEN, NOT POLLED. `WM_WINDOWPOSCHANGED` is the one
   message Windows guarantees for any move, size, z-order or show change of the
   owner, so following it is following the geometry -- there is nothing to miss
   and no per-frame `GetClientRect` on the render thread. */

#define VKW_CLASS   "tagpu_vk_surface"
enum { VKW_CREATE = 1, VKW_DESTROY = 2 };

static HWND volatile s_vkwnd;           /* ours; published by the window thread */
static HWND          s_owner;           /* the game window we are tracking     */
static int           s_askedWin;        /* a create is already posted          */

/* `raise` only on the first placement. An OWNED window already stays above its
   owner, so re-asserting HWND_TOP on every move of the owner would be pushing
   our window up the desktop's z-order for no reason -- and the owner's move is
   exactly when the player may be dragging something else over the game. */
static void vkw_place(HWND win, HWND owner, int raise)
{
    RECT c;
    POINT tl;
    if (!win || !owner) return;
    if (!GetClientRect(owner, &c)) return;
    tl.x = c.left; tl.y = c.top;
    if (!ClientToScreen(owner, &tl)) return;
    real_SetWindowPos(win, raise ? HWND_TOP : NULL, tl.x, tl.y,
                      c.right - c.left, c.bottom - c.top,
                      SWP_NOACTIVATE | (raise ? 0 : SWP_NOZORDER));
}

static LRESULT CALLBACK vkw_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    /* the pointer goes through us to the game window underneath */
    case WM_NCHITTEST:   return HTTRANSPARENT;
    /* we paint every pixel from the present; GDI must not flash over it */
    case WM_ERASEBKGND:  return 1;
    }
    return real_DefWindowProcA(h, m, w, l);
}

static void vkw_create(HWND owner)
{
    static int registered;
    HWND win;
    HINSTANCE inst = GetModuleHandleA(NULL);

    if (s_vkwnd || !owner) return;
    if (!registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = vkw_proc;
        wc.hInstance = inst;
        wc.lpszClassName = VKW_CLASS;
        wc.hCursor = NULL;              /* the game owns the cursor */
        if (!RegisterClassA(&wc)) { vklog("RegisterClass: %lu", GetLastError()); return; }
        registered = 1;
    }
    win = real_CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                               VKW_CLASS, "", WS_POPUP,
                               0, 0, 64, 64, owner, NULL, inst, NULL);
    if (!win) { vklog("CreateWindowEx: %lu", GetLastError()); return; }
    vkw_place(win, owner, 1);
    real_ShowWindow(win, SW_SHOWNOACTIVATE);
    /* PUBLISHED LAST, once the window is placed and shown: the render thread
       starts the bring-up on a handle it reads here, and a half-built window
       would give the swapchain the wrong extent. */
    InterlockedExchangePointer((void* volatile*)&s_vkwnd, win);
    vklog("window: created %p over %p (route D)", (void*)win, (void*)owner);
}

/* THE HANDLE IS TAKEN AWAY WHEN THE DESTROY IS REQUESTED, NOT WHEN IT IS
   SERVICED, AND IT TRAVELS IN THE MESSAGE. [FROM REVIEW 2026-09-15.]

   The previous shape posted `VKW_DESTROY` and left `s_vkwnd` standing until the
   window thread got round to pumping. Between those two moments the lever could
   be re-armed, or the render thread restarted after a mode change: `ST_OFF`
   read `s_vkwnd`, found it non-NULL, skipped the create and started a worker
   that put a surface on that window -- and the window thread then serviced the
   queued destroy and pulled the window out from under a live surface, which is
   the one thing this file forbids everywhere else. Clearing the handle first
   makes the window invisible to the render thread the instant it is doomed,
   which is the ordering the old comment claimed and did not have.

   Callable from the render thread and from the bring-up worker; the window
   thread does the destroying, unless the owner is already gone. */
static void vkw_request_destroy(void)
{
    HWND win = (HWND)InterlockedExchangePointer((void* volatile*)&s_vkwnd, NULL);
    HWND owner = s_owner;
    if (!win) return;
    /* ONLY THE THREAD THAT CREATED A WINDOW MAY DESTROY IT, so there is no
       fallback here -- an earlier revision called `DestroyWindow` directly when
       the owner was gone, from whichever thread happened to be asking, and that
       call simply fails. It is unreachable anyway: `s_owner` is set before the
       create is ever posted, so a window cannot exist without one. Said rather
       than silently dropped, because the day it is reachable the log is the
       only thing that will say so. */
    if (owner) PostMessageA(owner, WM_TAGPU_VK, (WPARAM)VKW_DESTROY, (LPARAM)win);
    else vklog("window %p has no owner to destroy it - it dies with the process",
               (void*)win);
}

/* THE WINDOW BELONGS TO THE LANE WHILE IT IS UP OR COMING UP, AND TO NOBODY
   OTHERWISE -- and this is the single place that says so. [FROM REVIEW
   2026-09-15, second pass.] The rule was spelled out separately at four call
   sites with three different guards, and two of them were wrong: `render_stop`
   destroyed the window while a ZOMBIE worker was still putting a surface on it,
   and an armed lane that FAILED kept a shown, never-painted popup over the
   client area for the session.

   A worker in ST_STARTING or ST_ZOMBIE may hold a surface on the window and
   takes it down on its own way out, so those two states are the only ones that
   leave it standing. Every other state must not. */
static void vkw_release_unless_worker(LONG st)
{
    if (st == ST_STARTING || st == ST_ZOMBIE) return;
    if (s_vkwnd) vkw_request_destroy();
}

/* Called from the fork's wndproc on the game window's thread. An OBSERVER: it
   never swallows a message the fork or the engine needs. */
void tagpu_vk_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_TAGPU_VK:
        if (wparam == VKW_CREATE) vkw_create(hwnd);
        if (wparam == VKW_DESTROY && lparam) {
            real_DestroyWindow((HWND)lparam);
            vklog("window: destroyed %p", (void*)lparam);
        }
        break;
    case WM_WINDOWPOSCHANGED:
        if (s_vkwnd) vkw_place(s_vkwnd, hwnd, 0);
        break;
    case WM_DESTROY:
        /* THE OWNER IS GOING AND NOTHING WE CAN DO ORDERS THIS PROPERLY -- so
           what happens here is the least-bad of the available orders, and the
           residual is named rather than papered over.

           The handle is taken out of `s_vkwnd` first, so the render thread's
           very next frame sees its window gone and takes the rebuild path,
           which tears the surface down. But the window is destroyed in this
           handler, and that next frame has not happened yet: for up to one
           frame a surface may name an HWND that no longer exists. Acquire then
           returns VK_ERROR_SURFACE_LOST_KHR and the frame is skipped, which is
           the safe answer.

           WHY IT IS NOT FIXABLE FROM HERE. The only orders that close it are
           blocking this thread on the render thread -- which in a lockstep game
           is the deadlock this whole file is arranged around -- or letting our
           window outlive its owner, which Windows does not allow for an owned
           window anyway. The fork's own teardown normally stops the render
           thread first (`ogl_release` refuses while `render.thread` is set), so
           in practice this handler is a backstop that finds nothing up; the
           residual is the process-exit case where it does. */
        {
            HWND win = (HWND)InterlockedExchangePointer((void* volatile*)&s_vkwnd, NULL);
            if (win) { real_DestroyWindow(win); vklog("window: destroyed %p with its owner", (void*)win); }
        }
        break;
    }
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
    int on = exists(ON_FILE) && !exists(OFF_FILE);

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

int tagpu_vk_armed(void)
{
    /* ASKED FROM THE GAME THREAD (`vrow_greyed`, when the options screen is
       built) and therefore a PURE READ: it must not call `read_lever`, which
       writes `s_clear` -- the render thread's. A file attribute query costs
       nothing at the rate a screen is built, and sharing no mutable state is
       worth more than the cached answer. */
    return exists(ON_FILE) && !exists(OFF_FILE);
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
    const char* iexts[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r;

    app.pApplicationName = "Total Annihilation (impure)";
    app.apiVersion = VK_API_VERSION_1_0;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
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
    if (exists(OFF_FILE)) { vklog("tagpu_vk.off - nothing in this module runs"); return; }
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
static int vk_renderpass(void)
{
    VkAttachmentDescription at;
    VkAttachmentReference ref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo rci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    memset(&at, 0, sizeof at);
    at.format = s_vk.fmt;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    at.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    memset(&ref, 0, sizeof ref);
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

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

    rci.attachmentCount = 1; rci.pAttachments = &at;
    rci.subpassCount = 1; rci.pSubpasses = &sub;
    rci.dependencyCount = 2; rci.pDependencies = dep;
    if (vkCreateRenderPass(s_vk.dev, &rci, NULL, &s_vk.rp) != VK_SUCCESS) {
        vklog("the render pass would not create"); s_vk.rp = VK_NULL_HANDLE; return 0;
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
        fbi.renderPass = s_vk.rp;
        fbi.attachmentCount = 1; fbi.pAttachments = &s_vk.view[i];
        fbi.width = s_vk.ext.width; fbi.height = s_vk.ext.height; fbi.layers = 1;
        if (vkCreateFramebuffer(s_vk.dev, &fbi, NULL, &s_vk.fb[i]) != VK_SUCCESS) {
            vklog("framebuffer %u", i); return 0;
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
        /* The framebuffer goes before the view it is built on. */
        if (s_vk.fb[i])   { vkDestroyFramebuffer(s_vk.dev, s_vk.fb[i], NULL); s_vk.fb[i] = VK_NULL_HANDLE; }
        if (s_vk.view[i]) { vkDestroyImageView(s_vk.dev, s_vk.view[i], NULL); s_vk.view[i] = VK_NULL_HANDLE; }
        if (s_vk.semAcquire[i]) { vkDestroySemaphore(s_vk.dev, s_vk.semAcquire[i], NULL); s_vk.semAcquire[i] = VK_NULL_HANDLE; }
        if (s_vk.semRelease[i]) { vkDestroySemaphore(s_vk.dev, s_vk.semRelease[i], NULL); s_vk.semRelease[i] = VK_NULL_HANDLE; }
        if (s_vk.fence[i])      { vkDestroyFence(s_vk.dev, s_vk.fence[i], NULL);          s_vk.fence[i] = VK_NULL_HANDLE; }
    }
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
        /* THE PASSES GO FIRST, AFTER THE WAIT AND BEFORE ANYTHING THEY MIGHT
           BE HOLDING. A pass owns pipelines, descriptors, buffers and images of
           its own; they belong to this device and must be back before it is
           destroyed. The wait above is what makes that safe rather than a race
           -- nothing of theirs is still in a queue. */
        tagpu_vk_fps_down(&s_pass);
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
            /* NAMED RATHER THAN SWALLOWED. Route D is the last of the
               roadmap's three still standing, so a driver that refuses a
               surface even on a window of our own is the case the phase's
               out-of-process pivot exists for -- and the difference between
               that and a lane that merely did not arm is this line. */
            vklog("vkCreateWin32SurfaceKHR on our own top-level window: %s (%d) - "
                  "route D is refused by this driver, and it is the only one of the "
                  "roadmap's three that works on system wine; the lane stays down",
                  res_name(r), (int)r);
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
        const char* dexts[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, NULL };
        uint32_t ndext = 1;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };

        /* VK_KHR_maintenance1 IS ASKED FOR AND NOT ASSUMED, AND IT BUYS EXACTLY
           ONE THING: a NEGATIVE VIEWPORT HEIGHT. GL's clip space has +Y up and
           Vulkan's has +Y down, so every shader the fork ports would draw its
           frame upside down -- and the shaders are ported UNCHANGED on purpose,
           because a source edit would make each one disagree with the GL twin
           that is its oracle (tools/spirv-gen.py's header). So the flip is
           pipeline state, and this is the state.

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
                    for (k = 0; k < ne; k++)
                        if (!strcmp(ext[k].extensionName, "VK_KHR_maintenance1")) {
                            dexts[ndext++] = "VK_KHR_maintenance1";
                            s_vk.flipok = 1;
                            break;
                        }
                free(ext);
            }
        }
        if (!s_vk.flipok)
            vklog("VK_KHR_maintenance1 is not offered - the lane will present but "
                  "no ported pass can flip clip space, so none will draw");

        qci.queueFamilyIndex = s_vk.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = ndext; dci.ppEnabledExtensionNames = dexts;
        r = vkCreateDevice(s_vk.pd, &dci, NULL, &s_vk.dev);
        if (r != VK_SUCCESS && s_vk.flipok) {
            /* The only extension that can be refused here is the optional one,
               so one retry without it is a real fallback and not a loop. */
            vklog("vkCreateDevice refused VK_KHR_maintenance1 (%s) - retrying without it",
                  res_name(r));
            s_vk.flipok = 0;
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
            vkw_request_destroy();   /* nothing will present on it; see `fail:` */
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
    s_pass.slots = s_vk.nimg;
    s_pass.flipok = s_vk.flipok;
    s_pass.gipa = s_gipa;
    s_pass.gdpa = vkGetDeviceProcAddr;
    s_pass.log = passlog;

    va_log("with Vulkan up");
    vklog("up in %lu ms on \"%s\" (row %d), our window %p over %p, %ux%u, vsync %d "
          "- route D: the GL lane's window is untouched",
          GetTickCount() - t0, s_vk.devName, s_vk.devIndex, (void*)s_vk.hwnd,
          (void*)s_owner, s_vk.ext.width, s_vk.ext.height, s_vk.vsync);

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
    vkw_request_destroy();
    lane_release();
    return 0;

fail:
    /* AND THE WINDOW GOES WITH IT. [FROM REVIEW 2026-09-15, second pass.] This
       exit and the no-extent one above used to release the lane and leave the
       popup standing -- and with the lane zombied there is no render thread
       left to notice, so it stayed shown and unpainted over the client area for
       the rest of the process. The window is destroyed HERE, on the thread that
       is still the sole owner of the surface that was on it, which is the only
       moment at which the order is not in question. */
    vk_down();
    vkw_request_destroy();
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
        tagpu_vk_shot_finish(&s_pass, AB_OUT);
    }

    vkResetFences(s_vk.dev, 1, &s_vk.fence[fi]);

    cb = s_vk.cmd[fi];
    vkResetCommandBuffer(cb, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

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
        int draw_fps = 0, ab = 0;
        VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        if (s_vk.rp && s_vk.fb[idx]) {
            draw_fps = tagpu_vk_fps_prepare(&s_pass, cb, fi);
            ab = tagpu_vk_fps_ab_frame();
        }

        if (s_vk.rp && s_vk.fb[idx]) {
            rbi.renderPass = s_vk.rp;
            rbi.framebuffer = s_vk.fb[idx];
            rbi.renderArea.extent = s_vk.ext;
            vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
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

        /* THE A/B CAPTURE, ONE FRAME, UNDER THE LEVER tagpu_fps.c SHARES. It
           is recorded last so that what it reads is the finished frame, and it
           leaves the image in PRESENT_SRC so the present below is unaffected. */
        if (ab && !s_vk.cansrc)
            vklog("the A/B asked for a capture and this surface's images do not "
                  "carry TRANSFER_SRC - only the GL half will be written");
        else if (ab && tagpu_vk_shot_record(&s_pass, cb, s_vk.img[idx],
                                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                            s_vk.ext.width, s_vk.ext.height, s_vk.fmt))
            s_abSlot1 = (int)fi + 1;
    }
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
    ab_drop("the swapchain rebuilding", idle);
    vk_perimage_free();
    r = vk_swapchain(w, h);
    if (r <= 0) return r;
    if (!vk_perimage()) return 0;
    s_pass.slots = s_vk.nimg;
    return 1;
}

int tagpu_vk_frame(HWND hwnd, int w, int h, int vsync)
{
    LONG st;
    DWORD now = GetTickCount();

    /* THE LEVER FIRST AND CHEAPLY. With `tagpu_vk.on` absent this returns 0
       here, on a cached answer refreshed every 250 ms, so the GL lane's frame
       is what it was before this file existed. */
    if (s_armed < 0 || (DWORD)(now - s_lastPoll) >= POLL_MS) { s_lastPoll = now; read_lever(); }

    /* THE DISARMED FRAME COSTS A PLAIN LOAD. `lane_state()` is a `lock cmpxchg`
       and the barrier it carries is only owed when something is actually going
       to be read behind it -- with the lever off and the lane at rest there is
       nothing, and this is the render thread's per-frame path. */
    /* `s_askedWin` is cleared on the way past, not left latched: a
       `CreateWindowEx` that failed leaves `s_vkwnd` NULL for ever, and a flag
       that survived the disarm would stop the next arm ever asking again --
       a lane that wedges in silence. [FROM REVIEW 2026-09-15, second pass.] */
    if (!s_armed && s_state == ST_OFF && !s_vkwnd) { s_askedWin = 0; return 0; }

    st = lane_state();

    /* THE WORKER'S HANDLE IS REAPED WHEREVER IT FINISHES, not only on the way
       into ST_READY. A lever cleared while a bring-up was still running used to
       leave the handle open for the session: the disarm branch below returns
       before any close, and the state never passes through ST_READY again. Any
       state but ST_STARTING means the worker has published and exited. */
    if (st != ST_STARTING && s_worker) { CloseHandle(s_worker); s_worker = NULL; }

    if (!s_armed) {
        /* Disarmed: give the objects back, and let a FAILED lane retry the next
           time the player arms it rather than once per launch. THE SURFACE GOES
           BEFORE THE WINDOW, always: a window destroyed under a live surface is
           a handle the driver still holds. `vk_down` is synchronous and the
           handle leaves `s_vkwnd` before the destroy is posted, so the order
           holds across the two threads. */
        if (st == ST_READY || st == ST_FAILED) {
            if (st == ST_READY) { vk_down(); vklog("lever off - down"); }
            InterlockedExchange(&s_state, ST_OFF);
        }
        /* AND THE WINDOW GOES WHATEVER THE STATE WAS. [FROM REVIEW 2026-09-15.]
           This used to be inside the branch above, which left one shape behind:
           arm the lever, have the window thread not pump (a map load, a modal),
           clear the lever at the next 250 ms poll -- the state is still ST_OFF,
           so nothing was posted -- and then the pump catches up and creates and
           SHOWS the popup. Nothing ever took it down: an opaque dead window
           over the client area for the rest of the session. */
        vkw_release_unless_worker(st);
        s_askedWin = 0;
        return 0;
    }

    switch (st) {
    case ST_OFF: {
        HWND win;
        if (!hwnd || w <= 0 || h <= 0) return 0;
        /* THE WINDOW FIRST, AND ON THE OTHER THREAD. Ours is created by the
           window thread from a posted message; this frame asks and the next
           one finds it. That is an ordering, not a wait -- the render thread
           never blocks on the window thread, which in a lockstep game is the
           deadlock this whole file is arranged to avoid. */
        s_owner = hwnd;
        win = s_vkwnd;
        if (!win) {
            /* ASKED ONCE, NOT ONCE A FRAME. The window thread answers when it
               next pumps; posting again every frame would put sixty requests a
               second into a queue that, in the one case where this matters --
               a pump that has stopped -- nobody is draining. The flag is
               cleared when the window arrives or the lane comes down. */
            if (!s_askedWin) {
                s_askedWin = 1;
                PostMessageA(hwnd, WM_TAGPU_VK, (WPARAM)VKW_CREATE, 0);
            }
            return 0;
        }
        s_askedWin = 0;
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
        /* A FAILED LANE PRESENTS NOTHING, so it owns no window either -- the
           previous pass fixed only the UNARMED half of that, and a bring-up
           that failed while the lever was on (a driver that refuses the
           surface, say) left a shown, never-painted popup over the game for
           the session. [FROM REVIEW 2026-09-15, second pass.] */
        vkw_release_unless_worker(ST_FAILED);
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
       the game window changed (a mode switch), OUR window went away (the
       owner's WM_DESTROY takes it), or the player picked another GPU. Each
       invalidates the surface or the device, and neither is patchable in
       place -- and going back through the worker is what keeps every bring-up
       off the render thread, not just the first. */
    if (hwnd != s_owner || s_vkwnd != s_vk.hwnd || lane_gen() != s_choiceSeen) {
        int owner_changed = (hwnd != s_owner);
        vklog(owner_changed       ? "the game window changed - rebuilding" :
              s_vkwnd != s_vk.hwnd ? "our window went away - rebuilding"
                                  : "the GPU row changed - rebuilding");
        vk_down();
        InterlockedExchange(&s_state, ST_OFF);
        /* AND THE WINDOW GOES TOO WHEN THE OWNER CHANGED, because ours is owned
           by the window that just went away -- keeping it would put the next
           bring-up on a window hanging off a dead owner. A GPU change keeps it:
           the window is not what changed. The destroy is posted to the NEW
           owner, whose thread is the one that pumps; the handler needs no hwnd. */
        if (owner_changed) vkw_request_destroy();
        s_askedWin = 0;
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
            s_askedWin = 0;
            /* THE WINDOW IS LEFT TO THE WORKER, which may have a surface on it.
               It destroys it on its way out. */
            return;
        }
        st = lane_state();
    }

    if (s_worker) { CloseHandle(s_worker); s_worker = NULL; }

    if (st == ST_READY || st == ST_FAILED) {
        if (st == ST_READY) { vk_down(); vklog("render thread stopping - down"); }
        InterlockedExchange(&s_state, ST_OFF);
    }
    /* The window goes last, and only once nothing holds a surface on it.
       REACHING HERE IN ST_ZOMBIE IS THE CASE THIS GUARD EXISTS FOR: a previous
       render thread abandoned a worker, this one started and is now stopping
       too, and that worker is still inside `vkCreateWin32SurfaceKHR`. The old
       tail destroyed the window under it. */
    vkw_release_unless_worker(st);
    s_askedWin = 0;
}
