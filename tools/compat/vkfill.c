/* vkfill -- holds a GPU's device-local memory until the driver refuses more, so a game started beside it
   meets a really full card. The suite's `tagpu_vkfull.on` rows stand in for the driver's no; this is the
   no itself. Never run it on a GPU something else depends on: it takes memory from every process on it.

     i686-w64-mingw32-gcc -O2 -Itagpu/ddraw/inc -o vkfill.exe tools/compat/vkfill.c -static
     vkfill <status file> <stop file> <seconds>

   The status file gets one line per change: `held <MiB> of <heap MiB>, refused <n>, last <VkResult>`.
   It asks again every 100 ms, so what anything else gives back is taken too, and stops at the stop
   file or at <seconds>, whichever is first. Its process ending is what frees the memory. */
#include <windows.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

#define CHUNK_MIB 16u

int main(int argc, char** argv)
{
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateInstance create_instance;
    PFN_vkEnumeratePhysicalDevices enum_devices;
    PFN_vkGetPhysicalDeviceMemoryProperties mem_props;
    PFN_vkGetPhysicalDeviceProperties props;
    PFN_vkCreateDevice create_device;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkAllocateMemory alloc;
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstance inst;
    VkPhysicalDevice pd[8];
    uint32_t npd = 8, i, type = ~0u, pick = 0;
    VkPhysicalDeviceProperties pp;
    VkPhysicalDeviceMemoryProperties mp;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkDevice dev;
    float prio = 1.0f;
    VkDeviceSize best = 0;
    unsigned held = 0, refused = 0, shown = ~0u;
    VkResult last = VK_SUCCESS;
    DWORD end;

    if (argc != 4 || !lib) { fprintf(stderr, "usage: vkfill <status> <stop> <seconds> (and vulkan-1.dll)\n"); return 2; }
    end = GetTickCount() + 1000u * (DWORD)atoi(argv[3]);
    gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(lib, "vkGetInstanceProcAddr");
    create_instance = (PFN_vkCreateInstance)(void*)gipa(NULL, "vkCreateInstance");
    ai.apiVersion = VK_API_VERSION_1_0;
    ii.pApplicationInfo = &ai;
    if (create_instance(&ii, NULL, &inst)) return 3;
    enum_devices = (PFN_vkEnumeratePhysicalDevices)(void*)gipa(inst, "vkEnumeratePhysicalDevices");
    props = (PFN_vkGetPhysicalDeviceProperties)(void*)gipa(inst, "vkGetPhysicalDeviceProperties");
    mem_props = (PFN_vkGetPhysicalDeviceMemoryProperties)(void*)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    create_device = (PFN_vkCreateDevice)(void*)gipa(inst, "vkCreateDevice");
    gdpa = (PFN_vkGetDeviceProcAddr)(void*)gipa(inst, "vkGetDeviceProcAddr");
    if (enum_devices(inst, &npd, pd) < 0 || !npd) return 4;
    for (i = 0; i < npd; i++) {
        props(pd[i], &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pick = i; break; }
    }
    props(pd[pick], &pp);
    mem_props(pd[pick], &mp);
    for (i = 0; i < mp.memoryTypeCount; i++) {
        VkDeviceSize size = mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size;
        if ((mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && size > best) {
            best = size;
            type = i;
        }
    }
    if (type == ~0u) return 5;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    if (create_device(pd[pick], &di, NULL, &dev)) return 6;
    alloc = (PFN_vkAllocateMemory)(void*)gdpa(dev, "vkAllocateMemory");
    for (;;) {
        VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        VkDeviceMemory m;
        mi.allocationSize = (VkDeviceSize)CHUNK_MIB << 20;
        mi.memoryTypeIndex = type;
        last = alloc(dev, &mi, NULL, &m);
        if (last == VK_SUCCESS) held += CHUNK_MIB;
        else { refused++; Sleep(100); }
        if (held != shown || refused == 1) {
            FILE* f = fopen(argv[1], "wb");
            if (f) {
                fprintf(f, "%s: held %u of %u MiB, refused %u, last %d\n", pp.deviceName, held,
                        (unsigned)(best >> 20), refused, (int)last);
                fclose(f);
            }
            shown = held;
        }
        if ((LONG)(GetTickCount() - end) >= 0 || GetFileAttributesA(argv[2]) != INVALID_FILE_ATTRIBUTES) return 0;
    }
}
