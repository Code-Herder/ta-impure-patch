/* tagpu_cfg.c -- the cnc-ddraw settings TA needs. API: tagpu_cfg.h.

   WHY THESE VALUES. cfg_load leaves every field at cnc-ddraw's own default;
   these are the ones that are wrong for TA, and one of them is a latent crash
   rather than a preference:

   - `max_resolutions` caps the mode list TA's video options is fed. cnc-ddraw's
     default is 0 = NO CAP, and TA's own list is a fixed "DISPLAY MODES"
     allocation of 0x4B0 bytes at 12 bytes an entry -- exactly 100 -- whose
     callback 0x4B5330 appends WITHOUT A BOUNDS CHECK (research/notes/
     resolution.md 6.1, 6.3). Past 100 modes the engine writes off the end of
     its own buffer in the options screen.
   - `toggle_borderless` off sends Alt+Enter down util_toggle_fullscreen's else
     branch, which clears `windowed` and takes the real ChangeDisplaySettings
     path -- the modeset dd.c's own comment says we never do (it blanks every
     physical monitor for about a second under wine).
   - `windowed` + `fullscreen`, together, are borderless fullscreen (dd_SetDisplayMode);
     `windowed` alone is a window. `fullscreen` alone would be the exclusive
     modeset above, so the pair is only ever set together.
   - `lock_surfaces`: only when it is set do dds_Lock/dds_Unlock hold the
     surface's own critical section (ddsurface.c), and does the GDI backend
     hold the primary's while it blits it (render_gdi.c).
   - `singlecpu`: cnc-ddraw's default pins the whole process, the render thread
     included, to one CPU (dllmain.c, dd.c).
   - `maintas`: the aspect-preserving fit (dd.c, utils.c, wndproc.c).
   - `inject_resolution` is the ONE list entry exempt from the CDS_TEST filter
     (dd_EnumDisplayModes), so it is the only way to GUARANTEE the monitor's own mode
     reaches the picker rather than hoping the enumeration includes it. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_log.h"

#include "config.h"
#include "dd.h"
#include "tagpu_cfg.h"
#include "tagpu_settings.h"
#include "fps_limiter.h"

/* TA's "DISPLAY MODES" buffer: 0x4B0 / 12 = 100 entries, and the count itself
   lives in a different allocation, so all 100 are usable. Our counter counts
   descriptors OFFERED, and TA stores only the 8bpp ones, so offered >= stored
   and capping the offer at 100 caps the store at 100. */
#define TA_MODE_MAX 100
#define WANT_MAX_RESOLUTIONS 90
/* THE BOUND. 0 is cnc-ddraw's "no cap", which is precisely the value TA's
   unchecked callback cannot survive on a machine with more than 100 eight-bit
   modes. */
_Static_assert(WANT_MAX_RESOLUTIONS > 0 && WANT_MAX_RESOLUTIONS <= TA_MODE_MAX,
               "TA's mode buffer holds 100 entries");

/* The harness lever that forces the GDI backend (tagpu_cfg.h). */
#define GDI_LEVER "tagpu_gdi.on"

void tagpu_cfg_defaults(void)
{
    int v, x, y, w, h, placed;
    char c[256];

    g_config.max_resolutions   = WANT_MAX_RESOLUTIONS;
    g_config.toggle_borderless = TRUE;
    g_config.lock_surfaces     = TRUE;
    g_config.singlecpu         = FALSE;
    g_config.maintas           = TRUE;
    g_config.windowed          = TRUE;      /* with fullscreen below: borderless */
    g_config.fullscreen        = TRUE;
    g_config.gdi               = GetFileAttributesA(GDI_LEVER) != INVALID_FILE_ATTRIBUTES;

    /* ---- the store's placement (renderers.md 2.10b) ----------------------
       Display mode, frame cap and the windowed frame. Read under
       tagpu_defaults.off too: nothing else can place a control launch's
       window, and tacli writes all three before every launch. */
    if (tagpu_settings_placement(TS_DISPLAY, &v))
        g_config.fullscreen = v ? TRUE : FALSE;
    if (tagpu_settings_placement(TS_MAXFPS, &v)) {
        /* Refresh is resolved by fpsl_init, on the render thread, into a
           positive cap; 60 until then */
        g_config.maxfps = v < 0 ? 60 : v;
        fpsl_request_cap(v);
    }
    placed = tagpu_settings_window(&x, &y, &w, &h);
    if (placed) {
        g_config.window_rect.left   = x;
        g_config.window_rect.top    = y;
        g_config.window_rect.right  = w;
        g_config.window_rect.bottom = h;
        /* A PLACED WINDOW STAYS WHERE IT WAS PUT. At CENTER_WINDOW_AUTO,
           dd_SetDisplayMode re-centres the window whenever TA switches to a mode
           larger than the window's recorded size -- the 640x480 shell giving way
           to a bigger game -- which throws a tiled instance off its tile. */
        g_config.center_window = CENTER_WINDOW_NEVER;
    }

    _snprintf(c, sizeof c,
              "cfg: max_resolutions %d, toggle_borderless, lock_surfaces, singlecpu off, maintas; "
              "display=%s maxfps=%d window=%s; renderer %s",
              g_config.max_resolutions, g_config.fullscreen ? "fullscreen" : "window",
              g_config.maxfps, placed ? "placed (center_window never)" : "default",
              g_config.gdi ? "GDI (" GDI_LEVER ")" : "Vulkan");
    c[sizeof c - 1] = 0;
    tagpu_log(c);
}

void tagpu_cfg_inject_native(unsigned int w, unsigned int h)
{
    static int logged;

    if (!w || !h)
        return;

    /* Re-written on every enumeration rather than once, so a desktop mode that
       changed mid-session cannot leave a stale entry in the picker. */
    _snprintf(g_config.inject_resolution, sizeof g_config.inject_resolution - 1, "%ux%u", w, h);
    g_config.inject_resolution[sizeof g_config.inject_resolution - 1] = 0;

    if (!logged) {
        char c[160];
        logged = 1;
        /* The injected entry is emitted BEFORE the first mode the walk
           enumerates and costs that mode nothing: dd.c steps the index back
           after injecting, so the same mode is re-enumerated and emitted too.
           If the enumeration also reports the injected mode the picker shows it
           twice -- 0x45E4C0 sorts but does not de-dup. Cosmetic, and visible in
           the list, so it is checked rather than assumed. */
        _snprintf(c, sizeof c, "cfg: inject_resolution -> %s (the desktop mode, exempt from CDS_TEST)",
                  g_config.inject_resolution);
        tagpu_log(c);
    }
}
