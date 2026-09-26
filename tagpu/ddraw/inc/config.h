#ifndef CONFIG_H
#define CONFIG_H

#include <windows.h>

#define FILE_EXISTS(a) (GetFileAttributes(a) != INVALID_FILE_ATTRIBUTES)

typedef struct CNCDDRAWCONFIG
{
    RECT window_rect;

    /* Optional settings */

    BOOL fullscreen;
    BOOL windowed;
    BOOL maintas;
    char aspect_ratio[16];
    BOOL boxing;
    BOOL adjmouse;
    BOOL gdi;
    BOOL devmode;
    BOOL border;
    BOOL resizable;
    int anti_aliased_fonts_min_size;
    int min_font_size;
    int center_window;
    char inject_resolution[128];
    char screenshot_dir[MAX_PATH];
    BOOL toggle_borderless;
    BOOL toggle_upscaled;

    /* Compatibility settings */

    BOOL noactivateapp;
    int maxgameticks;
    int limiter_type;
    int minfps;
    BOOL singlecpu;
    int resolutions;
    int fixchilds;
    BOOL hook_peekmessage;

    /* Undocumented settings */

    BOOL fix_alt_key_stuck;
    BOOL fix_not_responding;
    BOOL no_compat_warning;
    int guard_lines;
    int max_resolutions;
    BOOL lock_surfaces;
    BOOL flipclear;
    BOOL rgb555;
    BOOL no_dinput_hook;
    BOOL center_cursor_fix;
    char fake_mode[128];
    BOOL lock_mouse_top_left;
    char win_version[32];
    int hook;
    BOOL limit_gdi_handles;
    BOOL remove_menu;
    int refresh_rate;
    int terminate_process;

    /* Hotkeys */

    struct
    {
        int toggle_fullscreen;
        int toggle_fullscreen2;
        int toggle_maximize;
        int toggle_maximize2;
        int unlock_cursor1;
        int unlock_cursor2;
        int screenshot;
    } hotkeys;

} CNCDDRAWCONFIG;

extern CNCDDRAWCONFIG g_config;

void cfg_load();
void cfg_save();

#endif
