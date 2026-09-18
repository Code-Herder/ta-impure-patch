#ifndef RENDER_VK_H
#define RENDER_VK_H
/* render_vk -- the Vulkan renderer backend, selected by `renderer=vulkan`.
   Implementation: src/render_vk.c.

   IT IS A BACKEND, NOT A LEVER. Before the vulkan-only plan's landing 4 the
   Vulkan lane rode inside `ogl_render`'s loop behind `tagpu_vk.on` and
   presented into a window of its own, because two backends must not both
   present to one window in one frame. This one owns the render thread, so
   there is no second lane in the frame and no second window: the surface goes
   on `g_ddraw.hwnd`, exactly as the GDI backend's blit does.

   `dd.c` dispatches it on `tolower(g_config.renderer[0]) == 'v'`. Every OTHER
   `g_ddraw.renderer == ogl_render_main` test in `dd.c` stays GL-only, and that
   is a checked result rather than an omission -- see render_vk.c's head. */

#include <windows.h>

DWORD WINAPI vk_render_main(void);

#endif
