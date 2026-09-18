/* vkcoexist -- G19a's REAL gate: can Vulkan present on a window the fork's
 * OpenGL renderer already owns, and what does the hand-over cost?
 *
 * tools/vkpresent.c answered "can a 32-bit process present at all" on a window
 * it created itself. The game's HWND is not free: cnc-ddraw takes
 * `GetDC(hwnd)`, calls `SetPixelFormat` on it (dd.c:1536) and hands that HDC to
 * `wglCreateContext` (render_ogl.c `ogl_create`), and a window generally cannot
 * carry a GL context and a Vulkan swapchain at once. Which of the roadmap's
 * three routes actually works decides how the backend is built, so it is
 * measured here rather than argued -- BEFORE a line of it goes into the DLL.
 *
 * The four things it tries, in the roadmap's order of preference:
 *
 *   A  NAIVE       GL context created AND CURRENT, then Vulkan on the same
 *                  HWND. If this works there is no hand-over to design.
 *   B  ROUTE 1     the GL context released (wglMakeCurrent(NULL,NULL) +
 *                  wglDeleteContext, which is exactly `ogl_release`), then
 *                  Vulkan on the same HWND -- and then GL brought back up on
 *                  it afterwards, because a one-way door is not a lever.
 *   C  ROUTE 2     a CHILD window over the client area: GL stays current on the
 *                  parent, Vulkan presents on the child.
 *   D  ROUTE 3     two top-level windows, one GL, one Vulkan, swapping which is
 *                  visible.
 *
 * AND TWO MORE, ADDED WHEN THE PLAN DECIDED TO DELETE GL [2026-09-17], which
 * ask about a window no GL renderer owns at all:
 *
 *   E  VULKAN-ONLY  no GL context and no pixel format, ever. This is the state
 *                  the game window is in when `renderer=vulkan` selects a
 *                  backend that never calls `ogl_create`, and it is the route
 *                  the vulkan-only plan's landing 4 presents through.
 *   F  THE NET      route E, and then GDI on the same window -- because the
 *                  backend needs to know whether `gdi_render_main` can still
 *                  be SEEN after a surface has existed on the HWND, and that
 *                  decides whether a fallback may be taken late or only ever
 *                  before the surface is created.
 *
 * `--route GD` is F's control (GDI alone, Vulkan never touched), as `--route
 * GL` is the control for A-D.
 *
 * Every route reports GL_VENDOR/GL_RENDERER and the Vulkan deviceName, which is
 * G19b's oracle as well: the two lanes can name different GPUs and the menu has
 * to be able to say so rather than hope.
 *
 * WHAT `WORKS` MEANS HERE, AND WHAT IT DOES NOT. Every verdict below is an API
 * verdict: the calls returned VK_SUCCESS, the frames were accepted, and the GL
 * frame afterwards raised no error and its SwapBuffers returned TRUE. THAT IS
 * NOT THE SAME AS PIXELS REACHING THE SCREEN, and on 2026-09-15 the difference
 * mattered: route A passed every one of those checks in-game and the window
 * stayed on Vulkan's last frame for ever -- GL went on rendering correct frames
 * (`tacli glshot` read 168 distinct colours) that nothing ever saw, through a
 * full video-mode change and a new GL context. A probe that asks the API
 * whether it drew will be told yes by a lane that is painting into a drawable
 * the window no longer shows.
 *
 * So the pixel question is asked from OUTSIDE, by tools/vkcoexist-pixels.sh:
 * `--route X --hold N` runs one route and then spends N seconds painting the
 * window a known colour and swapping, and the script grabs the X window and
 * counts. Which lane paints the hold is what each route is ASKING about: A-D
 * and GL hold with GL (green), F and GD hold with GDI (the same green, so the
 * script needs no table), and E holds with VULKAN itself (flat magenta), since
 * on that route there is no second lane to ask about. That is the number that decides the design; the table below only
 * says which routes are worth grabbing.
 *
 * Build (from the repository root):
 *
 *   i686-w64-mingw32-gcc   -std=c99 -O1 -Wall -Itagpu/ddraw/inc -o vkcoexist32.exe tools/vkcoexist.c -lopengl32 -lgdi32
 *   x86_64-w64-mingw32-gcc -std=c99 -O1 -Wall -Itagpu/ddraw/inc -o vkcoexist64.exe tools/vkcoexist.c -lopengl32 -lgdi32
 *
 * HIDDEN WINDOWS by default, for the reason vkpresent.c states: the gate
 * re-runs without putting anything on the owner's screen. Only a route that
 * cannot present hidden is retried with SW_SHOWNOACTIVATE.
 *
 * The two handle traps vkpresent.c pays for apply here unchanged: a
 * non-dispatchable Vulkan handle is uint64_t at 32-bit and a pointer at 64-bit,
 * so never cast one to a pointer; and do not hand-roll the structs.
 */
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <GL/gl.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NFRAMES 10
#define WAIT_NS 1000000000ull          /* 1 s -- nothing may block for ever */

/* ---- wgl, exactly the way the fork reaches it ---------------------------- */
#define WGL_CONTEXT_MAJOR_VERSION_ARB       0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB       0x2092
#define WGL_CONTEXT_FLAGS_ARB               0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB        0x9126
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x0002
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB    0x0001
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);

static HMODULE g_vk;
static PFN_vkGetInstanceProcAddr GIPA;
static VkInstance g_inst = VK_NULL_HANDLE;

/* the instance-level entry points, resolved once in vk_init */
static PFN_vkEnumeratePhysicalDevices           p_enumPD;
static PFN_vkGetPhysicalDeviceProperties        p_pdProps;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties p_pdQueues;
static PFN_vkCreateWin32SurfaceKHR              p_createSurf;
static PFN_vkDestroySurfaceKHR                  p_destroySurf;
static PFN_vkGetPhysicalDeviceSurfaceSupportKHR p_surfSupport;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR p_surfCaps;
static PFN_vkGetPhysicalDeviceSurfaceFormatsKHR p_surfFmts;
static PFN_vkCreateDevice                       p_createDev;
static PFN_vkDestroyDevice                      p_destroyDev;
static PFN_vkGetDeviceProcAddr                  p_GDPA;

static const char* res_name(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    default: return "(other)";
    }
}

/* ---- address-space cost, the G19a exit's second half --------------------- */
/* Peak committed bytes and the largest free VA block. The second number is the
 * one that matters in a 32-bit process: the driver maps its own heaps into the
 * same 2 GB the engine's allocator lives in, and TA fails by failing to
 * allocate rather than by saying anything. */
static void va_report(const char* when)
{
    PROCESS_MEMORY_COUNTERS pmc;
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
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        memset(&pmc, 0, sizeof pmc);
    printf("  VA  %-18s committed-peak %6.1f MB   free %6.1f MB   largest-free-block %6.1f MB\n",
           when,
           (double)pmc.PeakPagefileUsage / (1024.0 * 1024.0),
           (double)freetotal / (1024.0 * 1024.0),
           (double)largest / (1024.0 * 1024.0));
}

/* ---- the GL side, brought up the way the fork brings it up --------------- */
typedef struct { HWND hwnd; HDC hdc; HGLRC ctx; } Gl;

/* dd.c:1526 verbatim: the fork's pixel format, on a DC it got with GetDC. */
static int gl_pixel_format(HDC hdc)
{
    PIXELFORMATDESCRIPTOR pfd;
    int pf;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf) return 0;
    return SetPixelFormat(hdc, pf, &pfd);
}

/* render_ogl.c `ogl_create` + `ogl_create_core_context`, compressed: a legacy
 * context to reach wglCreateContextAttribsARB, then the 3.3 core context the
 * fork actually renders with, and the legacy one deleted. */
static int gl_up(Gl* g, HWND hwnd)
{
    PFNWGLCREATECONTEXTATTRIBSARBPROC cca;
    static const int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0 };
    HGLRC core;

    memset(g, 0, sizeof *g);
    g->hwnd = hwnd;
    g->hdc = GetDC(hwnd);
    if (!g->hdc) { printf("  FAIL GetDC\n"); return 0; }
    if (!gl_pixel_format(g->hdc)) { printf("  FAIL SetPixelFormat (%lu)\n", GetLastError()); return 0; }

    g->ctx = wglCreateContext(g->hdc);
    if (!g->ctx) { printf("  FAIL wglCreateContext (%lu)\n", GetLastError()); return 0; }
    if (!wglMakeCurrent(g->hdc, g->ctx)) { printf("  FAIL wglMakeCurrent (%lu)\n", GetLastError()); return 0; }

    cca = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (cca && (core = cca(g->hdc, 0, attribs)) != NULL && wglMakeCurrent(g->hdc, core)) {
        wglDeleteContext(g->ctx);
        g->ctx = core;
    }
    printf("  ok   GL  %s | %s | %s\n",
           (const char*)glGetString(GL_VERSION),
           (const char*)glGetString(GL_VENDOR),
           (const char*)glGetString(GL_RENDERER));
    return 1;
}

/* One GL frame. THIS IS AN API CHECK AND NOTHING MORE: it returns 0 only when
 * GL raised an error or SwapBuffers refused, and both can succeed while the
 * window shows something else entirely (see the header). The pixel check is
 * `gl_hold` plus tools/vkcoexist-pixels.sh. */
static int gl_frame(Gl* g, float r)
{
    GLenum e;
    if (!wglMakeCurrent(g->hdc, g->ctx)) return 0;
    glClearColor(r, 0.3f, 1.0f - r, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!SwapBuffers(g->hdc)) return 0;
    while ((e = glGetError()) != GL_NO_ERROR) return 0;
    return 1;
}

/* THE PIXEL PHASE. Paint the GL window a known green and swap, for `secs`
 * seconds, so an external grab can answer the only question that matters: does
 * a GL frame drawn AFTER this route still reach the screen? Green is chosen
 * because it shares no channel with the Vulkan clear's magenta ramp, so a
 * partial or stale frame cannot be mistaken for a fresh one. */
#define HOLD_G 0.85f
static void gl_hold(Gl* g, int secs)
{
    DWORD t0 = GetTickCount();
    printf("  ... holding GL green for %d s (grab the window now)\n", secs);
    fflush(stdout);
    while ((DWORD)(GetTickCount() - t0) < (DWORD)secs * 1000) {
        MSG m;
        if (wglMakeCurrent(g->hdc, g->ctx)) {
            glClearColor(0.0f, HOLD_G, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            SwapBuffers(g->hdc);
        }
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        Sleep(16);
    }
}

/* THE SAME HOLD, PAINTED WITH GDI, for the fallback question route F asks. The
 * green is the same value `gl_hold` paints (0.85 in a UNORM framebuffer is 217,
 * which is what the X server reports), deliberately: one expected colour serves
 * every route whose pixel question is "does the NON-Vulkan lane still reach the
 * screen", and the script needs no per-route table for them.
 *
 * The fork's GDI backend blits with `StretchDIBits` on a DC it holds for the
 * window's life rather than with `FillRect` on a fresh one. That difference is
 * deliberate and it does not weaken the measurement: the question is whether
 * the WINDOW still shows anything a non-Vulkan lane draws, and a window
 * winevulkan has taken over shows neither. */
static void gdi_hold(HWND hw, int secs)
{
    DWORD t0 = GetTickCount();
    HBRUSH br = CreateSolidBrush(RGB(0, 217, 0));
    printf("  ... holding GDI green for %d s (grab the window now)\n", secs);
    fflush(stdout);
    while ((DWORD)(GetTickCount() - t0) < (DWORD)secs * 1000) {
        MSG m;
        HDC dc = GetDC(hw);
        if (dc) {
            RECT rc;
            GetClientRect(hw, &rc);
            FillRect(dc, &rc, br);
            ReleaseDC(hw, dc);
        }
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        Sleep(16);
    }
    DeleteObject(br);
}

/* `ogl_release` (render_ogl.c:1749): unbind, delete. The DC is the fork's and
 * outlives the context, so it is released separately. */
static void gl_down(Gl* g, int release_dc)
{
    if (g->ctx) { wglMakeCurrent(NULL, NULL); wglDeleteContext(g->ctx); g->ctx = NULL; }
    if (release_dc && g->hdc) { ReleaseDC(g->hwnd, g->hdc); g->hdc = NULL; }
}

/* ---- the Vulkan side ----------------------------------------------------- */
static int vk_init(void)
{
    const char* iexts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    PFN_vkCreateInstance ci;
    VkResult r;

    g_vk = LoadLibraryA("vulkan-1.dll");
    if (!g_vk) { printf("FAIL: vulkan-1.dll did not load\n"); return 0; }
    GIPA = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(g_vk, "vkGetInstanceProcAddr");
    if (!GIPA) { printf("FAIL: no vkGetInstanceProcAddr\n"); return 0; }

    app.apiVersion = VK_API_VERSION_1_0;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iexts;
    ci = (PFN_vkCreateInstance)GIPA(NULL, "vkCreateInstance");
    r = ci(&ici, NULL, &g_inst);
    if (r != VK_SUCCESS) { printf("FAIL: vkCreateInstance %s\n", res_name(r)); return 0; }

#define I(n) p_##n = (void*)0;
    p_enumPD     = (PFN_vkEnumeratePhysicalDevices)GIPA(g_inst, "vkEnumeratePhysicalDevices");
    p_pdProps    = (PFN_vkGetPhysicalDeviceProperties)GIPA(g_inst, "vkGetPhysicalDeviceProperties");
    p_pdQueues   = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)GIPA(g_inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    p_createSurf = (PFN_vkCreateWin32SurfaceKHR)GIPA(g_inst, "vkCreateWin32SurfaceKHR");
    p_destroySurf= (PFN_vkDestroySurfaceKHR)GIPA(g_inst, "vkDestroySurfaceKHR");
    p_surfSupport= (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)GIPA(g_inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
    p_surfCaps   = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)GIPA(g_inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    p_surfFmts   = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)GIPA(g_inst, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    p_createDev  = (PFN_vkCreateDevice)GIPA(g_inst, "vkCreateDevice");
    p_destroyDev = (PFN_vkDestroyDevice)GIPA(g_inst, "vkDestroyDevice");
    p_GDPA       = (PFN_vkGetDeviceProcAddr)GIPA(g_inst, "vkGetDeviceProcAddr");
    return p_enumPD && p_createSurf && p_createDev && p_GDPA;
}

/* Seconds of FLAT MAGENTA to keep presenting after the counted frames, for the
 * routes whose pixel question is about Vulkan's own output (E). Zero for every
 * route that holds with GL or GDI instead, which is every other one -- their
 * hold happens after `vk_present_on` has returned and torn its swapchain down.
 * Set around the call and cleared after it, never left armed. */
static int g_vkhold;

/* Present NFRAMES on `hwnd`, tearing everything down again. Returns the number
 * presented, and writes the failing step into `why` so a route that fails says
 * WHICH call refused rather than just "no". */
static unsigned vk_present_on(HWND hwnd, char* why, unsigned whycap, char* devname, unsigned devcap)
{
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkPhysicalDevice pds[8], pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkQueue queue;
    VkImage imgs[8];
    uint32_t n = 0, qfam = 0, ni = 0, nf = 0, want;
    unsigned presented = 0;
    int best = -1;
    VkResult r;
    VkExtent2D ext;
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[16], chosen;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, released = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    int f;
    DWORD hold_t0 = 0;
    int holding = 0;

    PFN_vkCreateSwapchainKHR      d_CreateSwapchainKHR = NULL;
    PFN_vkDestroySwapchainKHR     d_DestroySwapchainKHR = NULL;
    PFN_vkGetSwapchainImagesKHR   d_GetSwapchainImagesKHR = NULL;
    PFN_vkGetDeviceQueue          d_GetDeviceQueue = NULL;
    PFN_vkCreateCommandPool       d_CreateCommandPool = NULL;
    PFN_vkDestroyCommandPool      d_DestroyCommandPool = NULL;
    PFN_vkAllocateCommandBuffers  d_AllocateCommandBuffers = NULL;
    PFN_vkBeginCommandBuffer      d_BeginCommandBuffer = NULL;
    PFN_vkEndCommandBuffer        d_EndCommandBuffer = NULL;
    PFN_vkCmdPipelineBarrier      d_CmdPipelineBarrier = NULL;
    PFN_vkCmdClearColorImage      d_CmdClearColorImage = NULL;
    PFN_vkCreateSemaphore         d_CreateSemaphore = NULL;
    PFN_vkDestroySemaphore        d_DestroySemaphore = NULL;
    PFN_vkCreateFence             d_CreateFence = NULL;
    PFN_vkDestroyFence            d_DestroyFence = NULL;
    PFN_vkAcquireNextImageKHR     d_AcquireNextImageKHR = NULL;
    PFN_vkQueueSubmit             d_QueueSubmit = NULL;
    PFN_vkQueuePresentKHR         d_QueuePresentKHR = NULL;
    PFN_vkWaitForFences           d_WaitForFences = NULL;
    PFN_vkResetFences             d_ResetFences = NULL;
    PFN_vkDeviceWaitIdle          d_DeviceWaitIdle = NULL;

    why[0] = 0;
#define BAIL(msg) do { _snprintf(why, whycap, "%s", msg); goto done; } while (0)
#define BAILR(msg, rr) do { _snprintf(why, whycap, "%s: %s (%d)", msg, res_name(rr), (int)(rr)); goto done; } while (0)

    {
        VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        sci.hinstance = GetModuleHandleA(NULL);
        sci.hwnd = hwnd;
        r = p_createSurf(g_inst, &sci, NULL, &surf);
        if (r != VK_SUCCESS) BAILR("vkCreateWin32SurfaceKHR", r);
    }

    p_enumPD(g_inst, &n, NULL);
    if (n > 8) n = 8;
    if (!n) BAIL("no physical devices");
    p_enumPD(g_inst, &n, pds);

    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkQueueFamilyProperties q[16];
        uint32_t nq = 16;
        p_pdProps(pds[i], &p);
        p_pdQueues(pds[i], &nq, NULL);
        if (nq > 16) nq = 16;
        p_pdQueues(pds[i], &nq, q);
        for (uint32_t k = 0; k < nq; k++) {
            VkBool32 sup = VK_FALSE;
            int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2 : 1;
            p_surfSupport(pds[i], k, surf, &sup);
            if (sup && (q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && score > best) {
                best = score; pd = pds[i]; qfam = k;
            }
        }
    }
    if (!pd) BAIL("no graphics+present device for this surface");
    { VkPhysicalDeviceProperties p; p_pdProps(pd, &p); _snprintf(devname, devcap, "%s", p.deviceName); }

    {
        float prio = 1.0f;
        const char* dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        qci.queueFamilyIndex = qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dexts;
        r = p_createDev(pd, &dci, NULL, &dev);
        if (r != VK_SUCCESS) BAILR("vkCreateDevice", r);
    }

#define D(n) d_##n = (PFN_vk##n)p_GDPA(dev, "vk" #n)
    D(CreateSwapchainKHR); D(DestroySwapchainKHR); D(GetSwapchainImagesKHR);
    D(GetDeviceQueue); D(CreateCommandPool); D(DestroyCommandPool);
    D(AllocateCommandBuffers); D(BeginCommandBuffer); D(EndCommandBuffer);
    D(CmdPipelineBarrier); D(CmdClearColorImage); D(CreateSemaphore);
    D(DestroySemaphore); D(CreateFence); D(DestroyFence); D(AcquireNextImageKHR);
    D(QueueSubmit); D(QueuePresentKHR); D(WaitForFences); D(ResetFences);
    D(DeviceWaitIdle);
#undef D
    if (!d_CreateSwapchainKHR || !d_QueuePresentKHR) BAIL("device entry points missing");

    d_GetDeviceQueue(dev, qfam, 0, &queue);

    r = p_surfCaps(pd, surf, &caps);
    if (r != VK_SUCCESS) BAILR("surface capabilities", r);
    nf = 16;
    p_surfFmts(pd, surf, &nf, NULL);
    if (nf > 16) nf = 16;
    if (!nf) BAIL("no surface formats");
    p_surfFmts(pd, surf, &nf, fmts);
    chosen = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = fmts[i]; break; }

    want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = 320; ext.height = 240; }
    if (!ext.width || !ext.height) BAIL("surface extent is zero");

    {
        VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        swci.surface = surf; swci.minImageCount = want;
        swci.imageFormat = chosen.format; swci.imageColorSpace = chosen.colorSpace;
        swci.imageExtent = ext; swci.imageArrayLayers = 1;
        swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swci.preTransform = caps.currentTransform;
        swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   /* the only guaranteed one */
        swci.clipped = VK_TRUE;
        r = d_CreateSwapchainKHR(dev, &swci, NULL, &sc);
        if (r != VK_SUCCESS) BAILR("vkCreateSwapchainKHR", r);
    }

    ni = 8;
    d_GetSwapchainImagesKHR(dev, sc, &ni, NULL);
    if (ni > 8) ni = 8;
    d_GetSwapchainImagesKHR(dev, sc, &ni, imgs);

    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = qfam;
        if (d_CreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS) BAIL("command pool");
        cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        if (d_AllocateCommandBuffers(dev, &cbai, &cb) != VK_SUCCESS) BAIL("command buffer");
        if (d_CreateSemaphore(dev, &sem, NULL, &acquired) != VK_SUCCESS) BAIL("semaphore");
        if (d_CreateSemaphore(dev, &sem, NULL, &released) != VK_SUCCESS) BAIL("semaphore");
        if (d_CreateFence(dev, &fci, NULL, &fence) != VK_SUCCESS) BAIL("fence");
    }

    for (f = 0; ; f++) {
        int inhold = (f >= NFRAMES);
        uint32_t idx = 0;
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkClearColorValue col;
        VkPipelineStageFlags waitst = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        MSG m;

        /* THE HOLD, for the routes whose pixel question is about VULKAN's own
         * frame rather than about a GL frame drawn afterwards (E below). It
         * presents a FLAT magenta instead of the ramp, because the grab must be
         * able to say "this is our frame" from one pixel: the ramp's value
         * depends on which frame the grab caught, and a stale frame and a fresh
         * one are then the same measurement. Magenta shares no channel with the
         * green `gl_hold` and `gdi_hold` paint, so no colour is ambiguous
         * across routes either. */
        if (inhold) {
            if (!g_vkhold) break;
            if (!holding) {
                holding = 1;
                hold_t0 = GetTickCount();
                printf("  ... holding VULKAN magenta for %d s (grab the window now)\n", g_vkhold);
                fflush(stdout);
            }
            else if ((DWORD)(GetTickCount() - hold_t0) >= (DWORD)g_vkhold * 1000)
                break;
        }

        r = d_AcquireNextImageKHR(dev, sc, WAIT_NS, acquired, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) BAILR("acquire", r);

        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        d_BeginCommandBuffer(cb, &bi);
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = imgs[idx]; b.subresourceRange = rng;
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        d_CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  0, 0, NULL, 0, NULL, 1, &b);
        if (inhold) {
            col.float32[0] = 1.0f; col.float32[1] = 0.0f;
            col.float32[2] = 1.0f; col.float32[3] = 1.0f;
        }
        else {
            col.float32[0] = (float)f / NFRAMES; col.float32[1] = 0.2f;
            col.float32[2] = 1.0f - (float)f / NFRAMES; col.float32[3] = 1.0f;
        }
        d_CmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
        d_CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                  0, 0, NULL, 0, NULL, 1, &b);
        d_EndCommandBuffer(cb);

        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acquired; si.pWaitDstStageMask = &waitst;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &released;
        r = d_QueueSubmit(queue, 1, &si, fence);
        if (r != VK_SUCCESS) BAILR("submit", r);

        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &released;
        pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
        r = d_QueuePresentKHR(queue, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) BAILR("present", r);
        /* THE HOLD'S FRAMES ARE NOT THE MEASUREMENT. `presented` is compared
         * against NFRAMES by every route's `ok` test, so counting the hold
         * would make the verdict depend on how long the grab took. */
        if (!inhold) presented++;
        if (d_WaitForFences(dev, 1, &fence, VK_TRUE, WAIT_NS) != VK_SUCCESS) BAIL("fence timed out");
        d_ResetFences(dev, 1, &fence);
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        if (inhold) Sleep(16);
    }

done:
    if (dev) {
        if (d_DeviceWaitIdle) d_DeviceWaitIdle(dev);
        if (fence)    d_DestroyFence(dev, fence, NULL);
        if (acquired) d_DestroySemaphore(dev, acquired, NULL);
        if (released) d_DestroySemaphore(dev, released, NULL);
        if (pool)     d_DestroyCommandPool(dev, pool, NULL);
        if (sc)       d_DestroySwapchainKHR(dev, sc, NULL);
        p_destroyDev(dev, NULL);
    }
    if (surf) p_destroySurf(g_inst, surf, NULL);
    return presented;
#undef BAIL
#undef BAILR
}

/* ---- windows ------------------------------------------------------------- */
static HWND make_window(const char* cls, int show, HWND parent, int x, int y, int w, int h)
{
    WNDCLASSA wc = { 0 };
    HWND hw;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = cls;
    wc.style = CS_OWNDC;            /* the fork keeps ONE HDC for the window's life */
    RegisterClassA(&wc);
    hw = CreateWindowExA(parent ? 0 : WS_EX_NOACTIVATE, cls, "vkcoexist",
                         parent ? (WS_CHILD | WS_VISIBLE) : (WS_POPUP | WS_BORDER),
                         x, y, w, h, parent, NULL, wc.hInstance, NULL);
    if (hw && show && !parent) ShowWindow(hw, SW_SHOWNOACTIVATE);
    return hw;
}

/* ---- the four routes ----------------------------------------------------- */
typedef struct { const char* tag; const char* what; int ok; unsigned frames; char why[160]; char dev[256]; } Route;

static int g_hold;      /* --hold N: seconds of GL green at the end of a route */

/* A -- the naive one: GL up and CURRENT, Vulkan on the same HWND. */
static void route_A(Route* out, int show)
{
    Gl g;
    HWND hw = make_window("vkco_A", show, NULL, 40, 40, 320, 240);
    out->tag = "A"; out->what = "GL current + Vulkan on the SAME HWND";
    out->ok = 0; out->frames = 0; out->why[0] = 0; out->dev[0] = 0;
    if (!hw) { _snprintf(out->why, sizeof out->why, "CreateWindowEx"); return; }
    if (!gl_up(&g, hw)) { _snprintf(out->why, sizeof out->why, "GL bring-up"); DestroyWindow(hw); return; }
    if (!gl_frame(&g, 0.0f)) { _snprintf(out->why, sizeof out->why, "GL frame"); gl_down(&g, 1); DestroyWindow(hw); return; }
    va_report("GL up");
    out->frames = vk_present_on(hw, out->why, sizeof out->why, out->dev, sizeof out->dev);
    va_report("GL + Vulkan");
    out->ok = (out->frames == NFRAMES);
    /* Did GL survive Vulkan having been on its window? */
    if (out->ok && !gl_frame(&g, 1.0f)) {
        out->ok = 0;
        _snprintf(out->why, sizeof out->why, "Vulkan presented but GL's own calls failed afterwards");
    }
    if (g_hold) gl_hold(&g, g_hold);
    gl_down(&g, 1);
    DestroyWindow(hw);
}

/* B -- route 1: release the GL context, Vulkan on the same HWND, then GL back.
 * The return trip is the half that decides whether this is a LEVER or a
 * one-way door, so it is measured and timed. */
static void route_B(Route* out, int show)
{
    Gl g;
    DWORD t0, t_down, t_vk, t_up;
    HWND hw = make_window("vkco_B", show, NULL, 40, 40, 320, 240);
    out->tag = "B"; out->what = "GL released, Vulkan on the same HWND, GL back (route 1)";
    out->ok = 0; out->frames = 0; out->why[0] = 0; out->dev[0] = 0;
    if (!hw) { _snprintf(out->why, sizeof out->why, "CreateWindowEx"); return; }
    if (!gl_up(&g, hw)) { _snprintf(out->why, sizeof out->why, "GL bring-up"); DestroyWindow(hw); return; }
    if (!gl_frame(&g, 0.0f)) { _snprintf(out->why, sizeof out->why, "GL frame"); gl_down(&g, 1); DestroyWindow(hw); return; }

    t0 = GetTickCount();
    gl_down(&g, 0);                     /* `ogl_release`: the DC stays, as in the fork */
    t_down = GetTickCount();
    out->frames = vk_present_on(hw, out->why, sizeof out->why, out->dev, sizeof out->dev);
    t_vk = GetTickCount();
    va_report("Vulkan only");
    if (out->frames != NFRAMES) { ReleaseDC(hw, g.hdc); DestroyWindow(hw); return; }

    /* GL back on the same HWND and the same DC -- the return trip. */
    g.ctx = wglCreateContext(g.hdc);
    if (!g.ctx || !wglMakeCurrent(g.hdc, g.ctx)) {
        _snprintf(out->why, sizeof out->why, "GL would not come back (%lu)", GetLastError());
        ReleaseDC(hw, g.hdc); DestroyWindow(hw); return;
    }
    if (!gl_frame(&g, 1.0f)) {
        _snprintf(out->why, sizeof out->why, "GL context came back but could not draw");
        gl_down(&g, 1); DestroyWindow(hw); return;
    }
    t_up = GetTickCount();
    out->ok = 1;
    _snprintf(out->why, sizeof out->why, "GL down %lu ms, Vulkan %u frames %lu ms, GL back %lu ms",
              t_down - t0, out->frames, t_vk - t_down, t_up - t_vk);
    if (g_hold) gl_hold(&g, g_hold);
    gl_down(&g, 1);
    DestroyWindow(hw);
}

/* C -- route 2: a CHILD window over the client area. GL never lets go. */
static void route_C(Route* out, int show)
{
    Gl g;
    HWND parent, child;
    out->tag = "C"; out->what = "GL on the parent, Vulkan on a CHILD window (route 2)";
    out->ok = 0; out->frames = 0; out->why[0] = 0; out->dev[0] = 0;
    parent = make_window("vkco_Cp", show, NULL, 40, 40, 320, 240);
    if (!parent) { _snprintf(out->why, sizeof out->why, "CreateWindowEx (parent)"); return; }
    if (!gl_up(&g, parent)) { _snprintf(out->why, sizeof out->why, "GL bring-up"); DestroyWindow(parent); return; }
    if (!gl_frame(&g, 0.0f)) { _snprintf(out->why, sizeof out->why, "GL frame"); gl_down(&g, 1); DestroyWindow(parent); return; }
    child = make_window("vkco_Cc", 0, parent, 0, 0, 320, 240);
    if (!child) { _snprintf(out->why, sizeof out->why, "CreateWindowEx (child)"); gl_down(&g, 1); DestroyWindow(parent); return; }
    out->frames = vk_present_on(child, out->why, sizeof out->why, out->dev, sizeof out->dev);
    va_report("GL + Vulkan (child)");
    out->ok = (out->frames == NFRAMES);
    if (out->ok && !gl_frame(&g, 1.0f)) {
        out->ok = 0;
        _snprintf(out->why, sizeof out->why, "Vulkan presented on the child but GL's own calls failed");
    }
    DestroyWindow(child);
    if (g_hold) gl_hold(&g, g_hold);
    gl_down(&g, 1);
    DestroyWindow(parent);
}

/* D -- route 3: two top-level windows, swapping which is visible. */
static void route_D(Route* out, int show)
{
    Gl g;
    HWND a, b;
    out->tag = "D"; out->what = "two top-level windows, swap which is shown (route 3)";
    out->ok = 0; out->frames = 0; out->why[0] = 0; out->dev[0] = 0;
    a = make_window("vkco_Da", show, NULL, 40, 40, 320, 240);
    b = make_window("vkco_Db", 0, NULL, 40, 40, 320, 240);
    if (!a || !b) { _snprintf(out->why, sizeof out->why, "CreateWindowEx"); return; }
    if (!gl_up(&g, a)) { _snprintf(out->why, sizeof out->why, "GL bring-up"); DestroyWindow(a); DestroyWindow(b); return; }
    if (!gl_frame(&g, 0.0f)) { _snprintf(out->why, sizeof out->why, "GL frame"); gl_down(&g, 1); DestroyWindow(a); DestroyWindow(b); return; }
    if (show) { ShowWindow(b, SW_SHOWNOACTIVATE); ShowWindow(a, SW_HIDE); }
    out->frames = vk_present_on(b, out->why, sizeof out->why, out->dev, sizeof out->dev);
    va_report("GL + Vulkan (two windows)");
    out->ok = (out->frames == NFRAMES);
    if (show) { ShowWindow(a, SW_SHOWNOACTIVATE); ShowWindow(b, SW_HIDE); }
    if (out->ok && !gl_frame(&g, 1.0f)) {
        out->ok = 0;
        _snprintf(out->why, sizeof out->why, "Vulkan presented on the second window but GL's own calls failed");
    }
    if (g_hold) gl_hold(&g, g_hold);
    gl_down(&g, 1);
    DestroyWindow(a);
    DestroyWindow(b);
}

/* ---- the vulkan-only routes (the vulkan-only plan's landing 4) ----------- *
 *
 * A, B, C and D all ask ONE question: can Vulkan present on a window the fork's
 * GL renderer owns, and does GL survive it? E and F ask a different one, and it
 * only became worth asking when the plan decided to delete GL: can Vulkan
 * present on a window NOBODY owns -- and what can still be seen on that window
 * afterwards when the Vulkan lane itself will not come up?
 *
 * They are separate routes rather than a conclusion drawn from A, and that is
 * the point. Route A's verdict is "API ok, pixels dead", but the dead pixels
 * were GL's: its Vulkan half presented 10 of 10 frames on the very HWND the
 * game owns. Landing 4 deletes the half that failed, so on paper A already says
 * yes. "The half that failed is the half we deleted" is an argument, though,
 * and this file exists because arguments about this seam have been wrong twice
 * -- the API said yes when the screen said no, and the roadmap ranked the only
 * working route last. So the remaining half is measured on its own. */

/* E -- THE VULKAN-ONLY PRESENTATION ROUTE. No GL context is ever created and no
 * pixel format is ever set on the window. That is the whole difference from
 * route A, and it is exactly the state the game window is in when
 * `renderer=vulkan` picks a backend that never calls `ogl_create`: `dd.c`'s
 * `SetPixelFormat` is already gated on `g_ddraw.renderer == ogl_render_main`
 * (dd.c:1524), so nothing touches the HDC on the way past.
 *
 * THE LIMIT, STATED: `opengl32` is loaded in this process -- the exe links it
 * -- it is simply never used on this window. A DLL with no GL import at all is
 * a different binary, not a different route, and this probe cannot speak for
 * it. */
static void route_E(Route* out, int show)
{
    HWND hw = make_window("vkco_E", show, NULL, 40, 40, 320, 240);
    out->tag = "E"; out->what = "Vulkan alone: no GL context, no pixel format";
    out->ok = 0; out->frames = 0; out->why[0] = 0; out->dev[0] = 0;
    if (!hw) { _snprintf(out->why, sizeof out->why, "CreateWindowEx"); return; }
    /* The hold belongs to Vulkan here, so it happens INSIDE the present call --
       there is no second lane to paint with once the swapchain is gone. */
    g_vkhold = g_hold;
    out->frames = vk_present_on(hw, out->why, sizeof out->why, out->dev, sizeof out->dev);
    g_vkhold = 0;
    va_report("Vulkan alone, no GL on the window");
    out->ok = (out->frames == NFRAMES);
    DestroyWindow(hw);
}

/* F -- CAN THE FALLBACK STILL BE SEEN? Route E, and then GDI on the same
 * window. This is not curiosity: it decides what `render_vk.c` does when a
 * bring-up fails HALFWAY. `ogl_render_main` hands the session to
 * `gdi_render_main` when GL will not come up, and a vulkan-only backend wants
 * the same net -- but route A measured that once winevulkan has put a surface
 * on an HWND, that HWND is finished for GL for the life of the process. If GDI
 * inherits that verdict, a late fallback paints into a drawable nobody shows:
 * a black window and no diagnostic, which is the failure mode this plan has
 * spent three landings finding in other clothes.
 *
 * AND THE ANSWER DECIDES AN ORDERING RATHER THAN A HEURISTIC, which is what
 * CLAUDE.md asks a fix to be. If GDI does not survive, the fallback has to be
 * taken BEFORE `vkCreateWin32SurfaceKHR` is ever called, and a failure after
 * that point is terminal and must say so instead of drawing where nothing is
 * shown. If GDI does survive, the backend may fall back at any point. Either
 * way the backend is safe by construction; this measurement says which
 * construction it has to be. */
static void route_F(Route* out, int show)
{
    HWND hw = make_window("vkco_F", show, NULL, 40, 40, 320, 240);
    out->tag = "F"; out->what = "Vulkan alone, then GDI on the same HWND";
    out->ok = 0; out->frames = 0; out->why[0] = 0; out->dev[0] = 0;
    if (!hw) { _snprintf(out->why, sizeof out->why, "CreateWindowEx"); return; }
    out->frames = vk_present_on(hw, out->why, sizeof out->why, out->dev, sizeof out->dev);
    va_report("Vulkan, then GDI");
    out->ok = (out->frames == NFRAMES);
    if (g_hold) gdi_hold(hw, g_hold);
    DestroyWindow(hw);
}

/* THE GDI CONTROL, and it is needed for the same reason route GL is: a grab
 * that reads no green after route F has to be distinguishable from a harness
 * that cannot grab a GDI-painted window in the first place.
 *
 * "Vulkan never touched" means no SURFACE, no swapchain and no present on this
 * window -- `main` creates the instance before any route runs, so an instance
 * exists in every route including this one and route GL. That is not a loophole:
 * an instance is not per-window, and what route F puts on its HWND and this one
 * does not is exactly the surface. */
static void route_GD(Route* out, int show)
{
    HWND hw = make_window("vkco_GD", show, NULL, 40, 40, 320, 240);
    out->tag = "GD"; out->what = "control: GDI alone, Vulkan never touched";
    out->ok = 1; out->frames = 0; out->why[0] = 0; out->dev[0] = 0;
    if (!hw) { out->ok = 0; _snprintf(out->why, sizeof out->why, "CreateWindowEx"); return; }
    if (g_hold) gdi_hold(hw, g_hold);
    DestroyWindow(hw);
}

/* THE CONTROL. GL alone, never a Vulkan call, then the hold -- so a grab that
 * reads no green here is measuring the harness and not the routes. Without it
 * "route A kills the window" and "the grab does not work" are one result. */
static void route_GL(Route* out, int show)
{
    Gl g;
    HWND hw = make_window("vkco_G", show, NULL, 40, 40, 320, 240);
    out->tag = "GL"; out->what = "control: GL alone, Vulkan never touched";
    out->ok = 0; out->frames = 0; out->why[0] = 0; out->dev[0] = 0;
    if (!hw) { _snprintf(out->why, sizeof out->why, "CreateWindowEx"); return; }
    if (!gl_up(&g, hw)) { _snprintf(out->why, sizeof out->why, "GL bring-up"); DestroyWindow(hw); return; }
    out->ok = gl_frame(&g, 0.0f);
    /* THE CONTROL REPORTS ITS VA TOO, because "what does the GL lane cost in a
       32-bit address space" has no other answer in this file: every other route
       measures GL and Vulkan together and the two cannot be separated after the
       fact. [ADDED 2026-09-17, for the vulkan-only plan's landing 4.] */
    va_report("GL up, no Vulkan");
    if (g_hold) gl_hold(&g, g_hold);
    gl_down(&g, 1);
    DestroyWindow(hw);
}

int main(int argc, char** argv)
{
    Route r[6];
    int i, show = 0, any = 0;
    const char* only = NULL;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--show")) show = 1;
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) g_hold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--route") && i + 1 < argc) only = argv[++i];
    }
    /* A hold is for a grab, and a hidden window cannot be grabbed. */
    if (g_hold) show = 1;

    printf("vkcoexist -- can Vulkan present on the window the fork's GL renderer owns?\n");
    printf("host pointer size: %u bytes   windows: %s\n\n",
           (unsigned)sizeof(void*), show ? "SHOWN (no focus steal)" : "HIDDEN");

    if (!vk_init()) return 2;
    va_report("baseline");

    /* One route per invocation when `--route` names one: a hold is a single
       question ("does GL reach the screen after THIS route?"), and four of them
       in one process would each inherit the last one's damage. */
    if (only) {
        Route one;
        if      (!strcmp(only, "A"))  route_A(&one, show);
        else if (!strcmp(only, "B"))  route_B(&one, show);
        else if (!strcmp(only, "C"))  route_C(&one, show);
        else if (!strcmp(only, "D"))  route_D(&one, show);
        else if (!strcmp(only, "E"))  route_E(&one, show);
        else if (!strcmp(only, "F"))  route_F(&one, show);
        else if (!strcmp(only, "GL")) route_GL(&one, show);
        else if (!strcmp(only, "GD")) route_GD(&one, show);
        else { printf("unknown route \"%s\" (A, B, C, D, E, F, GL or GD)\n", only); return 2; }
        printf("\n==== RESULT ====\n%s  %-4s %-58s %u/%d frames\n",
               one.ok ? "api-ok" : "api-NO", one.tag, one.what, one.frames, NFRAMES);
        if (one.why[0]) printf("           %s\n", one.why);
        if (one.dev[0]) printf("           vulkan device: %s\n", one.dev);
        return one.ok ? 0 : 1;
    }

    printf("\n=== A: %s ===\n", "GL current + Vulkan, one HWND");
    route_A(&r[0], show);
    printf("\n=== B: %s ===\n", "route 1 -- release GL, Vulkan, GL back");
    route_B(&r[1], show);
    printf("\n=== C: %s ===\n", "route 2 -- Vulkan on a child window");
    route_C(&r[2], show);
    printf("\n=== D: %s ===\n", "route 3 -- two windows");
    route_D(&r[3], show);
    /* E and F are the vulkan-only pair and they answer a different question
       from A-D; the controls (GL, GD) stay `--route`-only, as GL always has. */
    printf("\n=== E: %s ===\n", "Vulkan alone, no GL context, no pixel format");
    route_E(&r[4], show);
    printf("\n=== F: %s ===\n", "Vulkan alone, then GDI on the same HWND");
    route_F(&r[5], show);

    printf("\n==== RESULT ====\n");
    for (i = 0; i < 6; i++) {
        printf("%s  %-4s %-58s %u/%d frames\n",
               r[i].ok ? "api-ok" : "api-NO", r[i].tag, r[i].what, r[i].frames, NFRAMES);
        if (r[i].why[0]) printf("           %s\n", r[i].why);
        if (r[i].dev[0]) printf("           vulkan device: %s\n", r[i].dev);
        any = any || r[i].ok;
    }
    printf("\nAPI-level: %s\n", any ? "at least one route is accepted" :
           "no route is even accepted -- Phase G pivots out of process");
    printf("This says nothing about PIXELS: run tools/vkcoexist-pixels.sh for that.\n");
    return any ? 0 : 1;
}
