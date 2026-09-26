#include <windows.h>
#include <string.h>
#include "config.h"
#include "dd.h"
#include "tagpu_cfg.h"
#include "tagpu_settings.h"

CNCDDRAWCONFIG g_config =
    { .window_rect = {.left = -32000, .top = -32000, .right = 0, .bottom = 0 } };

/* THERE IS NO CONFIG FILE. Upstream cnc-ddraw reads every field below from
   ddraw.ini; this DLL reads no ddraw.ini at all, and the settings a player
   chooses live in impure.cfg (tagpu_settings.h). What is set here is
   cnc-ddraw's own default for each field -- the value it took when the key was
   absent -- and a field not named is cnc-ddraw's zero default. The values TA
   needs that differ from these are tagpu_cfg_defaults', which runs last. */
void cfg_load()
{
    g_config.adjmouse = TRUE;
    g_config.border = TRUE;
    g_config.resizable = TRUE;
    g_config.anti_aliased_fonts_min_size = 13;
    g_config.center_window = CENTER_WINDOW_AUTO;
    strcpy(g_config.screenshot_dir, ".\\Screenshots\\");

    g_config.limiter_type = LIMIT_AUTO;
    g_config.singlecpu = TRUE;
    g_config.resolutions = RESLIST_NORMAL;
    g_config.fixchilds = FIX_CHILDS_DETECT_PAINT;

    g_config.guard_lines = 200;
    g_config.hook = 4;

    g_config.hotkeys.toggle_fullscreen = VK_RETURN;
    g_config.hotkeys.toggle_maximize = VK_NEXT;
    g_config.hotkeys.unlock_cursor1 = VK_TAB;
    g_config.hotkeys.unlock_cursor2 = VK_RCONTROL;
    g_config.hotkeys.screenshot = VK_SNAPSHOT;

    tagpu_settings_attach();
    tagpu_cfg_defaults();
}

/* The windowed frame goes to impure.cfg for the next launch. */
void cfg_save()
{
    if (!g_config.fullscreen)
        tagpu_settings_save_window(g_config.window_rect.left, g_config.window_rect.top,
                                   g_config.window_rect.right, g_config.window_rect.bottom);
}
