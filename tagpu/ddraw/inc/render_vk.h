#ifndef RENDER_VK_H
#define RENDER_VK_H
/* render_vk -- the Vulkan renderer backend: the default, and the only one
   unless the `tagpu_gdi.on` lever forces GDI. Implementation: src/render_vk.c.

   IT IS A BACKEND, NOT A LEVER. It owns the render thread and is the only
   thing presenting in the frame: the surface goes on `g_ddraw.hwnd`, exactly
   as the GDI backend's blit does.

   `dd.c` dispatches it unless `g_config.gdi` is set (tagpu_cfg.h); GDI is the
   only other backend, and the one this hands the session to if Vulkan fails
   -- see render_vk.c's head. */

#include <windows.h>

DWORD WINAPI vk_render_main(void);

#endif
