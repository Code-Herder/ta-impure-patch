#ifndef TAGPU_CFG_H
#define TAGPU_CFG_H
/* tagpu_cfg -- the cnc-ddraw settings TA needs, set in code: there is no
   ddraw.ini (config.c). The sibling of tagpu_opt.h: that one arms our render
   passes, this one configures cnc-ddraw itself.

   Display mode, the frame cap and the windowed frame come from the settings
   store (tagpu_settings.h, `tagpu_settings_placement`), under every launch.
   The renderer is Vulkan unless the harness lever `tagpu_gdi.on` sits beside
   TotalA.exe, which forces the GDI backend; GDI is otherwise only the lane the
   Vulkan backend hands a session to when it cannot come up (render_vk.c). */

/* Called from the END of cfg_load(), once cnc-ddraw's defaults are in place
   and the store is attached. At DLL attach, under the loader lock. */
void tagpu_cfg_defaults(void);

/* Called from EnumDisplayModes once the desktop mode is known, before the
   `inject_resolution` string is parsed. Deliberately NOT done at
   DLL_PROCESS_ATTACH: cfg_load runs under the loader lock and this needs the
   display. */
void tagpu_cfg_inject_native(unsigned int w, unsigned int h);

#endif
