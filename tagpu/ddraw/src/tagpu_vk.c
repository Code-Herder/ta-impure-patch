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

   THE HAND-OVER IS AN ORDERING, NOT A WAIT. `s_state` is the only channel
   between the worker and the render thread:

     ST_OFF -> ST_STARTING   render thread, by InterlockedCompareExchange, and
                             only then is the worker created -- so two frames
                             cannot start two workers.
     ST_STARTING -> ST_READY / ST_FAILED    the worker, by InterlockedExchange,
                             AFTER every field of `s_vk` is written. The
                             interlocked store is a full barrier, so a render
                             thread that reads ST_READY sees all of them.
     ST_READY -> ST_OFF      the render thread, and only it, and only from
                             ST_READY -- which is the state in which the worker
                             has stopped touching `s_vk` for good.

   So `s_vk` has exactly one owner at every instant, by the state and not by
   timing. The render thread never touches it in ST_STARTING; the worker never
   touches it after publishing. There is no window in which both do.

   THE ONE BOUNDED WAIT, and it is not the safety argument. If the render thread
   stops (a mode change, a shutdown) while a worker is still in ST_STARTING, it
   waits for that worker -- and if the wait expires it goes to ST_ZOMBIE and
   LEAKS the objects rather than freeing them under a live thread. Leaking is
   safe; freeing is not. The timeout only decides which of two safe outcomes
   happens, so it is ergonomics and not correctness. ST_ZOMBIE is terminal for
   the session: the lane stays down until the next launch, and the log says so.

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
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hook.h"
#include "tagpu_vk.h"

#define ON_FILE    "tagpu_vk.on"
/* THE CONTROL, and the module needs one because half of it runs with the lever
   off. The GPU enumeration is deliberately not gated on `tagpu_vk.on` -- the
   picker outlives the lane -- so an instance that wants the GL build's exact
   behaviour, thread for thread and file for file, has to be able to say so.
   `tagpu_vk.off` is that: with it present nothing in this file runs, the
   enumeration worker included, and the row plates whatever the cache last
   said. It is what an A/B against the pre-G19 DLL arms. */
#define OFF_FILE   "tagpu_vk.off"
#define GPUS_FILE  "tagpu_vk.gpus"      /* the cache the menu reads at attach */
#define CFG_FILE   "tagpu_vk.cfg"       /* the player's choice, by name       */
#define CFG_TMP    "tagpu_vk.cfg.tmp"
#define POLL_MS    250                  /* the lever, as elsewhere in the fork */
#define MAXDEV     TAGPU_VK_MAXGPU
#define MAXIMG     8
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

/* How long `tagpu_vk_render_stop` waits for a worker still in ST_STARTING
   before abandoning its objects. See the header comment: this decides between
   two SAFE outcomes and is not the correctness argument. */
#define WORKER_WAIT_MS 5000

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
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceProcAddr)

#define DFNS(X) \
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
    int              rebuild;              /* the surface said its extent moved */
    unsigned         frame;
    char             devName[NAMELEN];
} Vk;

static Vk  s_vk;
static volatile LONG s_state;
static HANDLE s_worker;
static struct { HWND hwnd; int w, h, vsync; } s_want;

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

static void vkw_destroy(void)
{
    HWND win = (HWND)InterlockedExchangePointer((void* volatile*)&s_vkwnd, NULL);
    if (!win) return;
    real_DestroyWindow(win);
    vklog("window: destroyed %p", (void*)win);
}

/* Called from the fork's wndproc on the game window's thread. An OBSERVER: it
   never swallows a message the fork or the engine needs. */
void tagpu_vk_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;
    switch (msg) {
    case WM_TAGPU_VK:
        if (wparam == VKW_CREATE)  vkw_create(hwnd);
        if (wparam == VKW_DESTROY) vkw_destroy();
        break;
    case WM_WINDOWPOSCHANGED:
        if (s_vkwnd) vkw_place(s_vkwnd, hwnd, 0);
        break;
    case WM_DESTROY:
        /* THE OWNER IS GOING AND NOTHING WE CAN DO ORDERS THIS PROPERLY -- so
           what happens here is the least-bad of the available orders, and the
           residual is named rather than papered over.

           `vkw_destroy` publishes `s_vkwnd = NULL` before it calls
           `DestroyWindow`, so the render thread's very next frame sees its
           window gone and takes the rebuild path, which tears the surface down.
           But the window is destroyed in this handler, and that next frame has
           not happened yet: for up to one frame a surface may name an HWND that
           no longer exists. Acquire then returns VK_ERROR_SURFACE_LOST_KHR and
           the frame is skipped, which is the safe answer.

           WHY IT IS NOT FIXABLE FROM HERE. The only orders that close it are
           blocking this thread on the render thread -- which in a lockstep game
           is the deadlock this whole file is arranged around -- or letting our
           window outlive its owner, which Windows does not allow for an owned
           window anyway. The fork's own teardown normally stops the render
           thread first (`ogl_release` refuses while `render.thread` is set), so
           in practice this handler is a backstop that finds nothing up; the
           residual is the process-exit case where it does. */
        vkw_destroy();
        break;
    }
}

/* ---- the lever ----------------------------------------------------------- */
static volatile LONG s_armed = -1;      /* -1 = never polled */
static DWORD s_lastPoll;

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
static int vk_load(void)
{
    if (s_mod) return 1;
    s_mod = real_LoadLibraryA("vulkan-1.dll");
    if (!s_mod) { vklog("vulkan-1.dll did not load - no Vulkan runtime is installed"); return 0; }
    s_gipa = (PFN_vkGetInstanceProcAddr)(void*)real_GetProcAddress(s_mod, "vkGetInstanceProcAddr");
    if (!s_gipa) { vklog("vulkan-1.dll has no vkGetInstanceProcAddr"); return 0; }
    vkCreateInstance = (PFN_vkCreateInstance)s_gipa(NULL, "vkCreateInstance");
    return vkCreateInstance != NULL;
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
    if (!vk_load()) return 0;
    inst = vk_instance();
    if (!inst) return 0;

    vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (n > MAXDEV) {
        vklog("%u devices, and the row can carry %d (there is no stagebuttn5) - "
              "only the first %d are offered", n, MAXDEV, MAXDEV);
        n = MAXDEV;
    }
    if (n) vkEnumeratePhysicalDevices(inst, &n, pds);

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
        h = CreateFileA(GPUS_FILE, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, 0);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, out, (DWORD)at, &wrote, 0);
            CloseHandle(h);
        }
        vklog("enumerated %u device(s) - " GPUS_FILE " rewritten, so the GPU row lists "
              "them from the NEXT launch (the menu's .GUI is written at attach)", n);
    } else {
        vklog("enumerated %u device(s) - " GPUS_FILE " already agrees", n);
    }
    for (i = 0; i < (int)n; i++)
        vklog("  device %d: %s%s", i, name[i], disc[i] ? "  [discrete]" : "");

    vkDestroyInstance(inst, NULL);
    return 0;
}

void tagpu_vk_enum_start(void)
{
    static LONG once;
    HANDLE t;
    if (exists(OFF_FILE)) { vklog("tagpu_vk.off - nothing in this module runs"); return; }
    if (InterlockedExchange(&once, 1)) return;
    t = CreateThread(NULL, 0, enum_worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ---- G19a: the bring-up -------------------------------------------------- */

/* The swapchain, its images and its per-image objects. Called by the bring-up
   worker (ST_STARTING, the worker owns `s_vk`) and by the render thread on a
   resize (ST_READY, the render thread owns it). Never by both: that is what the
   state machine is for. */
static int vk_swapchain(int w, int h)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[16];
    VkPresentModeKHR modes[8];
    VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainKHR old = s_vk.sc;
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
    /* A minimised window reports a zero extent and a swapchain cannot be made
       on one. Not an error: the caller keeps the lane down and retries. */
    if (!s_vk.ext.width || !s_vk.ext.height) return 0;
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
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
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

    s_vk.nimg = MAXIMG;
    vkGetSwapchainImagesKHR(s_vk.dev, s_vk.sc, &s_vk.nimg, NULL);
    if (s_vk.nimg > MAXIMG) s_vk.nimg = MAXIMG;
    vkGetSwapchainImagesKHR(s_vk.dev, s_vk.sc, &s_vk.nimg, s_vk.img);
    if (!s_vk.nimg) { vklog("the swapchain reports no images"); return 0; }

    vklog("swapchain: %u images %ux%u format %d mode %s",
          s_vk.nimg, s_vk.ext.width, s_vk.ext.height, (int)s_vk.fmt,
          mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO");
    return 1;
}

/* The per-image sync and command objects. Sized by the swapchain, so they are
   built after it and torn down with it. */
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
        if (vkCreateSemaphore(s_vk.dev, &sci, NULL, &s_vk.semAcquire[i]) != VK_SUCCESS ||
            vkCreateSemaphore(s_vk.dev, &sci, NULL, &s_vk.semRelease[i]) != VK_SUCCESS ||
            vkCreateFence(s_vk.dev, &fci, NULL, &s_vk.fence[i]) != VK_SUCCESS) {
            vklog("per-image sync objects"); return 0;
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
        if (vkDeviceWaitIdle) vkDeviceWaitIdle(s_vk.dev);
        vk_perimage_free();
        if (s_vk.pool) { vkDestroyCommandPool(s_vk.dev, s_vk.pool, NULL); s_vk.pool = VK_NULL_HANDLE; }
        if (s_vk.sc)   { vkDestroySwapchainKHR(s_vk.dev, s_vk.sc, NULL);  s_vk.sc = VK_NULL_HANDLE; }
        vkDestroyDevice(s_vk.dev, NULL);
        s_vk.dev = VK_NULL_HANDLE;
    }
    if (s_vk.surf && s_vk.inst) { vkDestroySurfaceKHR(s_vk.inst, s_vk.surf, NULL); s_vk.surf = VK_NULL_HANDLE; }
    if (s_vk.inst) { vkDestroyInstance(s_vk.inst, NULL); s_vk.inst = VK_NULL_HANDLE; }
    memset(&s_vk, 0, sizeof s_vk);
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
            /* THE ROUTE-A ANSWER, if a driver ever refuses it. The probe says
               every runtime tested allows a surface on a window that already
               carries a GL context; a refusal here is the case the roadmap's
               route 1 exists for, and it is named rather than swallowed. */
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
    if (pick < 0) { vklog("no device can present on the game window"); goto fail; }
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
        const char* dexts[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        qci.queueFamilyIndex = s_vk.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dexts;
        r = vkCreateDevice(s_vk.pd, &dci, NULL, &s_vk.dev);
        if (r != VK_SUCCESS) { vklog("vkCreateDevice: %s (%d)", res_name(r), (int)r); goto fail; }
    }

#define RES(n) n = (PFN_##n)vkGetDeviceProcAddr(s_vk.dev, #n);
    DFNS(RES)
#undef RES
    if (!vkCreateSwapchainKHR || !vkQueuePresentKHR || !vkAcquireNextImageKHR) {
        vklog("device entry points missing"); goto fail;
    }

    vkGetDeviceQueue(s_vk.dev, s_vk.qfam, 0, &s_vk.queue);

    if (!vk_swapchain(s_want.w, s_want.h)) goto fail;

    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s_vk.qfam;
        if (vkCreateCommandPool(s_vk.dev, &pci, NULL, &s_vk.pool) != VK_SUCCESS) {
            vklog("command pool"); goto fail;
        }
    }
    if (!vk_perimage()) goto fail;

    va_log("with Vulkan up");
    vklog("up in %lu ms on \"%s\" (row %d), our window %p over %p, %ux%u, vsync %d "
          "- route D: the GL lane's window is untouched",
          GetTickCount() - t0, s_vk.devName, s_vk.devIndex, (void*)s_vk.hwnd,
          (void*)s_owner, s_vk.ext.width, s_vk.ext.height, s_vk.vsync);

    InterlockedExchange(&s_activeIndex, s_vk.devIndex);
    /* THE PUBLISH. Everything above is written before it; the interlocked store
       is a full barrier, so a render thread that reads ST_READY sees all of it.
       After this line the worker touches nothing. */
    InterlockedExchange(&s_state, ST_READY);
    return 0;

fail:
    vk_down();
    InterlockedExchange(&s_state, ST_FAILED);
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
   One frame in flight, deliberately: G19a draws a clear and nothing else, and a
   wrong fence here is precisely the silent class this project fails at.
   Deepening it to a real frame ring is G19d's, when there is a pass to pace. */
static int vk_present(void)
{
    uint32_t idx = 0, fi = s_vk.frame % s_vk.nimg;
    VkResult r;
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkPipelineStageFlags waitst = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkClearColorValue col;
    VkCommandBuffer cb;

    if (vkWaitForFences(s_vk.dev, 1, &s_vk.fence[fi], VK_TRUE, 1000000000ull) != VK_SUCCESS)
        return 0;                                   /* a second is not a frame */

    r = vkAcquireNextImageKHR(s_vk.dev, s_vk.sc, 1000000000ull,
                              s_vk.semAcquire[fi], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return -1;   /* the caller rebuilds */
    if (r == VK_SUBOPTIMAL_KHR) s_vk.rebuild = 1;   /* usable; rebuild after */
    else if (r != VK_SUCCESS) return 0;
    if (idx >= s_vk.nimg) return 0;                 /* bounded before it indexes */

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

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(cb);

    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s_vk.semAcquire[fi];
    si.pWaitDstStageMask = &waitst;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s_vk.semRelease[idx];
    if (vkQueueSubmit(s_vk.queue, 1, &si, s_vk.fence[fi]) != VK_SUCCESS) return 0;

    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &s_vk.semRelease[idx];
    pi.swapchainCount = 1; pi.pSwapchains = &s_vk.sc; pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(s_vk.queue, &pi);
    s_vk.frame++;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return -1;
    /* SUBOPTIMAL: the frame WAS presented, so it is counted -- but the surface
       has moved on and the next frame should be drawn against a fresh
       swapchain. Rebuilding here instead would throw away a good frame. */
    if (r == VK_SUBOPTIMAL_KHR) { s_vk.rebuild = 1; return 1; }
    if (r != VK_SUCCESS) return 0;
    return 1;
}

/* Rebuild the swapchain in place: the render thread owns `s_vk` in ST_READY, so
   this is single-owner work. Everything sized by the swapchain goes with it. */
static int vk_resize(int w, int h)
{
    vkDeviceWaitIdle(s_vk.dev);
    vk_perimage_free();
    if (!vk_swapchain(w, h)) return 0;
    return vk_perimage();
}

int tagpu_vk_frame(HWND hwnd, int w, int h, int vsync)
{
    LONG st;
    DWORD now = GetTickCount();

    /* THE LEVER FIRST AND CHEAPLY. With `tagpu_vk.on` absent this returns 0
       here, on a cached answer refreshed every 250 ms, so the GL lane's frame
       is what it was before this file existed. */
    if (s_armed < 0 || (DWORD)(now - s_lastPoll) >= POLL_MS) { s_lastPoll = now; read_lever(); }

    st = s_state;

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
           destroy is posted behind it, so the order holds across the two
           threads by construction. */
        if (st == ST_READY || st == ST_FAILED) {
            if (st == ST_READY) { vk_down(); vklog("lever off - down"); }
            InterlockedExchange(&s_state, ST_OFF);
            if (s_vkwnd && s_owner) PostMessageA(s_owner, WM_TAGPU_VK, (WPARAM)VKW_DESTROY, 0);
        }
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
        s_choiceSeen = InterlockedCompareExchange(&s_choiceGen, 0, 0);
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

    case ST_STARTING:
    case ST_FAILED:
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
    if (hwnd != s_owner || s_vkwnd != s_vk.hwnd || s_choiceGen != s_choiceSeen) {
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
        if (owner_changed && s_vkwnd)
            PostMessageA(hwnd, WM_TAGPU_VK, (WPARAM)VKW_DESTROY, 0);
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
        s_vk.rebuild = 0;
        if (!vk_resize(w, h)) {
            vklog("the swapchain would not rebuild - down");
            vk_down();
            InterlockedExchange(&s_state, ST_FAILED);
            return 0;
        }
    }

    {
        int rc = vk_present();
        if (rc < 0) {                  /* out of date: rebuild, and skip this frame */
            s_vk.rebuild = 1;
            return 0;
        }
        return rc;
    }
}

void tagpu_vk_render_stop(void)
{
    LONG st = s_state;

    if (st == ST_STARTING && s_worker) {
        /* THE ONE BOUNDED WAIT, AND IT IS NOT THE SAFETY ARGUMENT. Whichever
           way it goes the outcome is safe: the worker finishes and we tear
           down, or it does not and we ABANDON the objects rather than free them
           under a live thread. A leak is recoverable; a free is not. */
        DWORD wr = WaitForSingleObject(s_worker, WORKER_WAIT_MS);
        if (wr != WAIT_OBJECT_0) {
            vklog("the bring-up thread did not finish in %d ms - the Vulkan objects are "
                  "ABANDONED, not freed, and the lane stays down until the next launch",
                  WORKER_WAIT_MS);
            InterlockedExchange(&s_state, ST_ZOMBIE);
            s_worker = NULL;               /* deliberately not closed: still running */
            /* AND THE WINDOW IS LEFT STANDING, deliberately. The abandoned
               objects include a surface on it, and destroying a window under a
               live surface is the one thing the order below exists to prevent.
               It costs an invisible 0-byte window for the rest of the process,
               which is the same bargain as the leak itself. */
            return;
        }
        st = s_state;
    }

    if (s_worker) { CloseHandle(s_worker); s_worker = NULL; }

    if (st == ST_READY || st == ST_FAILED) {
        if (st == ST_READY) { vk_down(); vklog("render thread stopping - down"); }
        InterlockedExchange(&s_state, ST_OFF);
    }
    /* The window goes last, and only once nothing holds a surface on it. */
    if (s_vkwnd && s_owner) PostMessageA(s_owner, WM_TAGPU_VK, (WPARAM)VKW_DESTROY, 0);
    s_askedWin = 0;
}
