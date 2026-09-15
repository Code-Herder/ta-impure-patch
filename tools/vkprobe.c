/* vkprobe — what does a Vulkan device look like from THIS process?
 *
 * Answers one question the renderer's architecture turns on: which Vulkan
 * capabilities a given (bitness x runtime) combination actually reaches on the
 * machine in front of you. It enumerates every physical device and reports, per
 * device, the extensions that decide whether ray tracing and cross-process
 * surface sharing are available at all.
 *
 * Built for BOTH bitnesses and run under whichever wine, it produced the table
 * in research/notes/field-notes.md, "Environment & toolchain" (2026-09-15):
 * the ray-tracing extensions are gated on bitness, not on wine. It prints its
 * own pointer size first so the build under test is never in doubt.
 *
 *   i686-w64-mingw32-gcc   -std=c99 -O1 -Wall -o vkprobe32.exe tools/vkprobe.c
 *   x86_64-w64-mingw32-gcc -std=c99 -O1 -Wall -o vkprobe64.exe tools/vkprobe.c
 *   wine vkprobe32.exe
 *
 * Deliberately self-contained: NO Vulkan SDK headers. The handful of structs
 * and entry points it needs are declared here, so it builds with nothing but a
 * mingw cross compiler. Two things that matter if you extend it:
 *
 *   - Vulkan's calling convention on win32 is __stdcall (VKAPI_CALL). Getting
 *     this wrong corrupts the stack on the 32-bit build ONLY, which reads as
 *     "32-bit Vulkan is broken" and is not.
 *   - VkPhysicalDeviceProperties is read at fixed ABI offsets rather than
 *     declared, because declaring it means declaring VkPhysicalDeviceLimits.
 *     The offsets below are stable across every Vulkan version.
 *
 * Drivers change. Re-run it rather than trusting the recorded table. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef int   VkResult;
typedef void* VkInstance;
typedef void* VkPhysicalDevice;

typedef struct {
    unsigned sType; const void* pNext; const char* pApplicationName;
    unsigned applicationVersion; const char* pEngineName; unsigned engineVersion;
    unsigned apiVersion;
} VkApplicationInfo;                                  /* sType = 0 */

typedef struct {
    unsigned sType; const void* pNext; unsigned flags;
    const VkApplicationInfo* pApplicationInfo;
    unsigned enabledLayerCount;     const char* const* ppEnabledLayerNames;
    unsigned enabledExtensionCount; const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo;                               /* sType = 1 */

typedef struct { char extensionName[256]; unsigned specVersion; } VkExtensionProperties;

/* VkPhysicalDeviceProperties, by byte offset */
#define PDP_API_VERSION   0
#define PDP_VENDOR_ID     8
#define PDP_DEVICE_ID    12
#define PDP_DEVICE_TYPE  16
#define PDP_DEVICE_NAME  20      /* char[256] */
#define PDP_SIZE       2048      /* over-allocated: the real struct is smaller */

typedef void*    (__stdcall *PFN_GetInstanceProcAddr)(VkInstance, const char*);
typedef VkResult (__stdcall *PFN_EnumerateInstanceVersion)(unsigned*);
typedef VkResult (__stdcall *PFN_CreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef VkResult (__stdcall *PFN_EnumeratePhysicalDevices)(VkInstance, unsigned*, VkPhysicalDevice*);
typedef void     (__stdcall *PFN_GetPhysicalDeviceProperties)(VkPhysicalDevice, void*);
typedef VkResult (__stdcall *PFN_EnumerateDeviceExtensionProperties)(VkPhysicalDevice, const char*, unsigned*, VkExtensionProperties*);

/* The four that decide ray tracing, then the ones a cross-process renderer
 * needs. buffer_device_address is listed because it is the 64-bit address
 * plumbing the RT extensions rest on -- it is present at 32-bit, which is how
 * we know the gate is the RT extensions themselves. */
static const char* WANTED[] = {
    "VK_KHR_acceleration_structure",
    "VK_KHR_ray_tracing_pipeline",
    "VK_KHR_ray_query",
    "VK_KHR_deferred_host_operations",
    "VK_KHR_buffer_device_address",
    "VK_KHR_external_memory_win32",
    "VK_KHR_swapchain",
    "VK_EXT_descriptor_indexing",
};
#define NWANTED (sizeof WANTED / sizeof *WANTED)

static const char* DEVTYPE[] = { "other", "integrated", "DISCRETE", "virtual", "cpu" };

int main(void)
{
    printf("host pointer size: %u bytes\n\n", (unsigned)sizeof(void*));

    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) {
        printf("FAIL: vulkan-1.dll did not load (GetLastError %lu)\n", GetLastError());
        return 1;
    }
    PFN_GetInstanceProcAddr gipa =
        (PFN_GetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("FAIL: vulkan-1.dll exports no vkGetInstanceProcAddr\n"); return 1; }

    PFN_EnumerateInstanceVersion enum_ver =
        (PFN_EnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
    unsigned ver = 0;
    if (enum_ver && enum_ver(&ver) == 0)
        printf("loader instance version: %u.%u.%u\n",
               (ver >> 22) & 0x7F, (ver >> 12) & 0x3FF, ver & 0xFFF);

    /* Request 1.0: a higher apiVersion than the loader supports is refused
       outright, and the devices still report their own real apiVersion below. */
    VkApplicationInfo app = { 0 };
    app.sType = 0;
    app.apiVersion = (1u << 22);
    VkInstanceCreateInfo ici = { 0 };
    ici.sType = 1;
    ici.pApplicationInfo = &app;

    PFN_CreateInstance create = (PFN_CreateInstance)gipa(NULL, "vkCreateInstance");
    VkInstance inst = NULL;
    VkResult r = create(&ici, NULL, &inst);
    if (r != 0) { printf("FAIL: vkCreateInstance = %d\n", r); return 1; }

    PFN_EnumeratePhysicalDevices enum_dev =
        (PFN_EnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_GetPhysicalDeviceProperties get_props =
        (PFN_GetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_EnumerateDeviceExtensionProperties enum_ext =
        (PFN_EnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");

    unsigned ndev = 0;
    enum_dev(inst, &ndev, NULL);
    printf("physical devices: %u\n", ndev);
    if (!ndev) return 1;

    VkPhysicalDevice dev[8];
    if (ndev > 8) ndev = 8;
    enum_dev(inst, &ndev, dev);

    for (unsigned i = 0; i < ndev; i++) {
        unsigned char props[PDP_SIZE];
        memset(props, 0, sizeof props);
        get_props(dev[i], props);

        unsigned api, vendor, devid, type;
        memcpy(&api,    props + PDP_API_VERSION,  4);
        memcpy(&vendor, props + PDP_VENDOR_ID,    4);
        memcpy(&devid,  props + PDP_DEVICE_ID,    4);
        memcpy(&type,   props + PDP_DEVICE_TYPE,  4);

        printf("\n[%u] %s\n    type=%s  vendor=0x%04X  device=0x%04X  api=%u.%u.%u\n",
               i, (const char*)(props + PDP_DEVICE_NAME),
               type < 5 ? DEVTYPE[type] : "?", vendor, devid,
               (api >> 22) & 0x7F, (api >> 12) & 0x3FF, api & 0xFFF);

        unsigned next = 0;
        enum_ext(dev[i], NULL, &next, NULL);
        VkExtensionProperties* ext = malloc(next * sizeof *ext);
        if (!ext) { printf("    (out of memory for %u extensions)\n", next); continue; }
        enum_ext(dev[i], NULL, &next, ext);

        printf("    device extensions: %u\n", next);
        for (unsigned w = 0; w < NWANTED; w++) {
            int found = 0;
            for (unsigned j = 0; j < next; j++)
                if (!strcmp(ext[j].extensionName, WANTED[w])) { found = 1; break; }
            printf("      %-34s %s\n", WANTED[w], found ? "YES" : "no");
        }
        free(ext);
    }
    return 0;
}
