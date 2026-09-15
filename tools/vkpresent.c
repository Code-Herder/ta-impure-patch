/* vkpresent -- G19a's oracle: can THIS process create a Vulkan swapchain and
 * present frames on a real HWND?
 *
 * Device enumeration says nothing about presentation, and presentation is the
 * step Phase G's kill rule hangs on. This gets all the way to the screen:
 * instance, surface, device, swapchain, and NFRAMES presented frames, clearing
 * through a colour ramp with vkCmdClearColorImage -- no render pass, no
 * pipeline, no shaders, because the question is the present path and not
 * drawing.
 *
 * Loads vulkan-1.dll dynamically, exactly as the DLL must, so a machine with no
 * Vulkan degrades instead of failing to load.
 *
 * Tries a HIDDEN window first, so the gate can be re-run without putting
 * anything on the owner's screen. Only if that cannot present does it retry
 * with a small window shown WITHOUT taking focus (SW_SHOWNOACTIVATE), since the
 * real target -- the game's window -- is visible anyway. Every wait is bounded
 * by WAIT_NS, so it cannot hang a session.
 *
 * Build (from the repository root; headers are vendored, see
 * tagpu/ddraw/inc/vulkan/README.md):
 *
 *   i686-w64-mingw32-gcc   -std=c99 -O1 -Wall -Itagpu/ddraw/inc -o vkpresent32.exe tools/vkpresent.c
 *   x86_64-w64-mingw32-gcc -std=c99 -O1 -Wall -Itagpu/ddraw/inc -o vkpresent64.exe tools/vkpresent.c
 *
 * Result recorded 2026-09-15 (research/notes/roadmap.md, Phase G): 10/10 frames
 * on the RTX 4070 under Proton 11 at BOTH bitnesses and under system wine 9.0
 * at 32-bit, from a hidden window. The 64-bit binary is this same source
 * recompiled and not edited -- which is the phase's premise demonstrated.
 *
 * Two traps this file exists to have already paid for:
 *   - Non-dispatchable Vulkan handles (VkSurfaceKHR, VkSwapchainKHR, VkImage,
 *     ...) are uint64_t at 32-bit and POINTERS at 64-bit. Never cast one to a
 *     pointer, store it in a void*, or key a container on it. Code that is
 *     correct at 32-bit is correct at 64-bit; the reverse breaks silently.
 *   - Do not hand-roll these structs to avoid the headers. At 32-bit under the
 *     MS ABI, VkSurfaceKHR is 8 bytes at offset 16 of VkSwapchainCreateInfoKHR
 *     -- there is alignment padding after three 4-byte fields that is easy to
 *     get wrong, and getting it wrong reads as "32-bit Vulkan is broken".
 *
 * Also see tools/vkprobe.c, which answers the capability question (which
 * extensions a given bitness/runtime reaches) and needs no headers at all.
 */
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NFRAMES 10
#define WAIT_NS 1000000000ull          /* 1 s -- nothing may block for ever */

static HMODULE g_vk;
static PFN_vkGetInstanceProcAddr GIPA;
#define IFN(n) PFN_##n n = (PFN_##n)GIPA(inst, #n)

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
    default: return "(other)";
    }
}
#define CHECK(expr, what) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    printf("  FAIL %-34s %s (%d)\n", what, res_name(_r), _r); return 0; } \
    printf("  ok   %s\n", what); } while (0)

static int run(int show, unsigned* out_presented)
{
    printf("\n=== window %s ===\n", show ? "SHOWN (no focus steal)" : "HIDDEN");

    const char* iexts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iexts;

    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance create_inst = (PFN_vkCreateInstance)GIPA(NULL, "vkCreateInstance");
    CHECK(create_inst(&ici, NULL, &inst), "vkCreateInstance");

    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkCreateWin32SurfaceKHR);
    IFN(vkGetPhysicalDeviceSurfaceSupportKHR); IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    IFN(vkGetPhysicalDeviceSurfaceFormatsKHR); IFN(vkCreateDevice); IFN(vkGetDeviceQueue);
    IFN(vkCreateSwapchainKHR); IFN(vkGetSwapchainImagesKHR); IFN(vkCreateCommandPool);
    IFN(vkAllocateCommandBuffers); IFN(vkBeginCommandBuffer); IFN(vkCmdPipelineBarrier);
    IFN(vkCmdClearColorImage); IFN(vkEndCommandBuffer); IFN(vkCreateSemaphore);
    IFN(vkCreateFence); IFN(vkAcquireNextImageKHR); IFN(vkQueueSubmit);
    IFN(vkQueuePresentKHR); IFN(vkWaitForFences); IFN(vkResetFences);
    IFN(vkDeviceWaitIdle);

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = show ? "vkpresent_s" : "vkpresent_h";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE, wc.lpszClassName, "vkpresent",
                                show ? (WS_POPUP | WS_BORDER) : WS_POPUP,
                                40, 40, 320, 240, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("  FAIL CreateWindowEx (%lu)\n", GetLastError()); return 0; }
    if (show) ShowWindow(hwnd, SW_SHOWNOACTIVATE);   /* never SW_SHOW: no focus steal */
    printf("  ok   HWND %p\n", (void*)hwnd);

    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = wc.hInstance; sci.hwnd = hwnd;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    CHECK(vkCreateWin32SurfaceKHR(inst, &sci, NULL, &surf), "vkCreateWin32SurfaceKHR");

    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice pds[8]; if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(inst, &n, pds);

    VkPhysicalDevice pd = VK_NULL_HANDLE; uint32_t qfam = 0; int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, NULL);
        VkQueueFamilyProperties* q = malloc(nq * sizeof *q);
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, q);
        for (uint32_t k = 0; k < nq; k++) {
            VkBool32 sup = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], k, surf, &sup);
            int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2 : 1;
            if (sup && (q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && score > best) {
                best = score; pd = pds[i]; qfam = k;
            }
        }
        free(q);
    }
    if (!pd) { printf("  FAIL no graphics+present device\n"); return 0; }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("  ok   device: %s (queue family %u)\n", props.deviceName, qfam);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dexts;
    VkDevice dev = VK_NULL_HANDLE;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice (+VK_KHR_swapchain)");

    VkQueue queue; vkGetDeviceQueue(dev, qfam, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps), "surface capabilities");
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL);
    VkSurfaceFormatKHR* fmts = malloc(nf * sizeof *fmts);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts);
    VkSurfaceFormatKHR chosen = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = fmts[i]; break; }
    free(fmts);

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = 320; ext.height = 240; }

    VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swci.surface = surf; swci.minImageCount = want;
    swci.imageFormat = chosen.format; swci.imageColorSpace = chosen.colorSpace;
    swci.imageExtent = ext; swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;      /* the only one guaranteed */
    swci.clipped = VK_TRUE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    CHECK(vkCreateSwapchainKHR(dev, &swci, NULL, &sc), "vkCreateSwapchainKHR (FIFO)");

    uint32_t ni = 0; vkGetSwapchainImagesKHR(dev, sc, &ni, NULL);
    VkImage* imgs = malloc(ni * sizeof *imgs);
    vkGetSwapchainImagesKHR(dev, sc, &ni, imgs);
    printf("  ok   swapchain: %u images, %ux%u, format %d\n", ni, ext.width, ext.height, chosen.format);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = qfam;
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool), "command pool");
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb), "command buffer");

    VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired, released;
    CHECK(vkCreateSemaphore(dev, &sem, NULL, &acquired), "semaphore (acquire)");
    CHECK(vkCreateSemaphore(dev, &sem, NULL, &released), "semaphore (release)");
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CHECK(vkCreateFence(dev, &fci, NULL, &fence), "fence");

    unsigned presented = 0;
    for (int f = 0; f < NFRAMES; f++) {
        uint32_t idx = 0;
        VkResult r = vkAcquireNextImageKHR(dev, sc, WAIT_NS, acquired, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            printf("  FAIL frame %d acquire: %s (%d)\n", f, res_name(r), r); break;
        }
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = imgs[idx]; b.subresourceRange = rng;
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        VkClearColorValue col;
        col.float32[0] = (float)f / NFRAMES; col.float32[1] = 0.2f;
        col.float32[2] = 1.0f - (float)f / NFRAMES; col.float32[3] = 1.0f;
        vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cb);

        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acquired; si.pWaitDstStageMask = &wait;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &released;
        r = vkQueueSubmit(queue, 1, &si, fence);
        if (r != VK_SUCCESS) { printf("  FAIL frame %d submit: %s\n", f, res_name(r)); break; }

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &released;
        pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(queue, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            printf("  FAIL frame %d present: %s (%d)\n", f, res_name(r), r); break;
        }
        presented++;
        if (vkWaitForFences(dev, 1, &fence, VK_TRUE, WAIT_NS) != VK_SUCCESS) {
            printf("  FAIL frame %d fence timed out\n", f); break;
        }
        vkResetFences(dev, 1, &fence);
        MSG m; while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
    printf("  %s presented %u of %d frames\n", presented == NFRAMES ? "ok  " : "FAIL", presented, NFRAMES);
    *out_presented = presented;
    vkDeviceWaitIdle(dev);
    DestroyWindow(hwnd);
    return presented == NFRAMES;
}

int main(void)
{
    printf("host pointer size: %u bytes\n", (unsigned)sizeof(void*));
    g_vk = LoadLibraryA("vulkan-1.dll");
    if (!g_vk) { printf("FAIL: vulkan-1.dll did not load\n"); return 2; }
    GIPA = (PFN_vkGetInstanceProcAddr)GetProcAddress(g_vk, "vkGetInstanceProcAddr");
    if (!GIPA) { printf("FAIL: no vkGetInstanceProcAddr\n"); return 2; }

    unsigned hidden = 0, shown = 0;
    int ok = run(0, &hidden);
    if (!ok) { printf("\nhidden window could not present -- retrying visible\n"); ok = run(1, &shown); }

    printf("\n==== RESULT ====\nhidden: %u/%d frames   shown: %u/%d frames\n",
           hidden, NFRAMES, shown, NFRAMES);
    printf("32-bit swapchain + present on a real HWND: %s\n", ok ? "WORKS" : "FAILED");
    return ok ? 0 : 1;
}
