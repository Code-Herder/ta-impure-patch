/* tagpu_cfg.c -- the fork settings this DLL owns. API: tagpu_cfg.h.

   WHY THIS EXISTS. Four cnc-ddraw settings decide whether TA is usable, and all
   four were reachable only by hand-editing ddraw.ini. One of them is a latent
   crash rather than a preference:

   - `max_resolutions` caps the mode list TA's video options is fed. The
     built-in default when the key is absent is 0 = NO CAP, and TA's own list is
     a fixed "DISPLAY MODES" allocation of 0x4B0 bytes at 12 bytes an entry --
     exactly 100 -- whose callback 0x4B5330 appends WITHOUT A BOUNDS CHECK
     (research/notes/resolution.md 6.1, 6.3). Past 100 modes the engine writes
     off the end of its own buffer in the options screen. So the value is owned
     here and BOUNDED here.
   - `toggle_borderless` off sends Alt+Enter down util_toggle_fullscreen's else
     branch, which clears `windowed` and takes the real ChangeDisplaySettings
     path -- the modeset dd.c's own comment says we never do (it blanks every
     physical monitor for about a second under wine).
   - `windowed` + `fullscreen`, together, are borderless fullscreen (dd.c:750).
   - `inject_resolution` is the ONE list entry exempt from the CDS_TEST filter
     (dd.c:356), so it is the only way to GUARANTEE the monitor's own mode
     reaches the picker rather than hoping the enumeration includes it.

   WINDOWED AND FULLSCREEN ARE ONE DECISION, NOT TWO. cfg_load defaults
   `windowed` to FALSE, so setting `fullscreen` alone for a player with no ini
   would give fullscreen-without-windowed = the exclusive modeset above, i.e.
   exactly the thing the toggle_borderless line is there to prevent. The pair is
   therefore atomic: if the player wrote EITHER key we own NEITHER.

   THE PRESENCE TEST MEANS WHAT IT SAYS ONLY BECAUSE WE DO NOT SHIP THESE
   KEYS. cfg_create_ini() writes an ini for a player who has none, from the
   template in config.c; if that template's [TotalA] section carried, say,
   `max_resolutions=32`, every player's ini would "contain" the key and an
   only-if-absent rule would never fire. The keys are absent from there and
   from tagpu/release/ddraw.ini for that reason -- if one comes back, this
   module silently stops working and nothing warns you. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_log.h"

#include "config.h"
#include "ini.h"
#include "tagpu_cfg.h"
#include "tagpu_settings.h"
#include "fps_limiter.h"

/* TA's "DISPLAY MODES" buffer: 0x4B0 / 12 = 100 entries, and the count itself
   lives in a different allocation, so all 100 are usable. Our counter counts
   descriptors OFFERED, and TA stores only the 8bpp ones, so offered >= stored
   and capping the offer at 100 caps the store at 100. */
#define TA_MODE_MAX 100

/* What we ask for when the player has not spoken. */
#define WANT_MAX_RESOLUTIONS 90

typedef struct {
    const char* key;
    int*        field;
    int         value;
} Def;

static const Def s_def[] = {
    { "max_resolutions",   &g_config.max_resolutions,   WANT_MAX_RESOLUTIONS },
    { "toggle_borderless", &g_config.toggle_borderless, TRUE },
    { "windowed",          &g_config.windowed,          TRUE },
    { "fullscreen",        &g_config.fullscreen,        TRUE },
};
#define NDEF (int)(sizeof s_def / sizeof s_def[0])

/* the windowed/fullscreen pair, by index into s_def */
#define I_WINDOWED   2
#define I_FULLSCREEN 3

static int s_mayInject;                 /* the player left inject_resolution to us */

/* A ddraw.ini key that holds a menu row (tagpu_settings.h, tier 1). Latched
   here, at attach, because the parsed ini is freed straight after. */
static int s_displayHeld, s_maxfpsHeld, s_windowHeld;

static void cfglog(const char* s)
{
    tagpu_log(s);
}

/* Did the player write this key? Mirrors cfg_get_string's own rule exactly --
   the game section first, then `ddraw`, and a key present with an empty value
   counts as absent there too. */
static int typed(const char* key)
{
    char buf[32];

    if (g_config.game_section[0] &&
        ini_get_string(&g_config.ini, g_config.game_section, key, "", buf, sizeof buf) > 0)
        return 1;

    return ini_get_string(&g_config.ini, "ddraw", key, "", buf, sizeof buf) > 0;
}

void tagpu_cfg_defaults(void)
{
    char b[512];
    char kept[256];
    int  i, p = 0, k = 0, pair_is_theirs;

    kept[0] = 0;
    pair_is_theirs = typed(s_def[I_WINDOWED].key) || typed(s_def[I_FULLSCREEN].key);

    p = _snprintf(b, sizeof b, "cfg: tagpu defaults:");

    for (i = 0; i < NDEF; i++) {
        int theirs = (i == I_WINDOWED || i == I_FULLSCREEN) ? pair_is_theirs : typed(s_def[i].key);
        int n;

        if (theirs) {
            n = _snprintf(kept + k, sizeof kept - k, "%s%s", k ? ", " : "", s_def[i].key);
            if (n > 0 && n < (int)(sizeof kept - k)) k += n;
            continue;
        }

        n = _snprintf(b + p, sizeof b - p, " %s %d -> %d,",
                      s_def[i].key, *s_def[i].field, s_def[i].value);
        if (n < 0 || n >= (int)(sizeof b - p)) break;   /* truncated: stop, never walk back */
        p += n;

        *s_def[i].field = s_def[i].value;
    }

    s_mayInject = !typed("inject_resolution");

    /* ---- the store's three (renderers.md 2.10b) --------------------------
       Display mode, frame cap and the windowed frame are menu rows now, kept in
       impure.cfg. A key the ini carries is the lever and wins -- tacli writes
       all of them, which is what keeps an instance on its tile. */
    s_displayHeld = pair_is_theirs;
    s_maxfpsHeld  = typed("maxfps");
    s_windowHeld  = typed("posX") || typed("posY") || typed("width") || typed("height");
    {
        int v, x, y, w, h;
        char c[200];
        if (!s_displayHeld && tagpu_settings_get(TS_DISPLAY, &v)) {
            g_config.windowed   = TRUE;             /* the pair stays atomic: borderless */
            g_config.fullscreen = v ? TRUE : FALSE; /* or a window                        */
        }
        if (!s_maxfpsHeld && tagpu_settings_get(TS_MAXFPS, &v)) {
            /* Refresh is resolved by fpsl_init, on the render thread, into a
               positive cap; 60 until then */
            g_config.maxfps = v < 0 ? 60 : v;
            fpsl_request_cap(v);
        }
        if (!s_windowHeld && tagpu_settings_window(&x, &y, &w, &h)) {
            g_config.window_rect.left   = x;
            g_config.window_rect.top    = y;
            g_config.window_rect.right  = w;
            g_config.window_rect.bottom = h;
        }
        _snprintf(c, sizeof c, "cfg: from %s: display=%s maxfps=%s window=%s",
                  tagpu_settings_ignored() ? "the ini (the store is ignored)" : "the store",
                  s_displayHeld ? "HELD by the ini" : g_config.fullscreen ? "fullscreen" : "window",
                  s_maxfpsHeld ? "HELD by the ini" : "set",
                  s_windowHeld ? "HELD by the ini" : g_config.window_rect.left != -32000 ? "set" : "default");
        c[sizeof c - 1] = 0;
        cfglog(c);
    }
    /* cnc-ddraw's own save wrote the window keys back into ddraw.ini on exit,
       where they would read as a lever on the next launch and pin the rows.
       impure.cfg keeps the frame instead (cfg_save), so the fork's save is off. */
    g_config.save_settings = 0;

    /* THE BOUND, applied whatever the ini said and whoever set it. 0 is not a
       preference here -- it is cnc-ddraw's "no cap", which is precisely the
       value TA's unchecked callback cannot survive on a machine with more than
       100 eight-bit modes. */
    if (g_config.max_resolutions <= 0 || g_config.max_resolutions > TA_MODE_MAX) {
        char c[160];
        _snprintf(c, sizeof c,
                  "cfg: max_resolutions %d -> %d (TA's mode buffer is %d entries and its "
                  "callback 0x4B5330 does not bounds-check)",
                  g_config.max_resolutions, TA_MODE_MAX, TA_MODE_MAX);
        cfglog(c);
        g_config.max_resolutions = TA_MODE_MAX;
    }

    if (p > 0 && b[p - 1] == ',') b[--p] = 0;
    _snprintf(b + p, sizeof b - p, " (the ini wins; %s%s)",
              k ? "the player set " : "the player set nothing",
              k ? kept : "");
    b[sizeof b - 1] = 0;
    cfglog(b);
}

void tagpu_cfg_inject_native(unsigned int w, unsigned int h)
{
    static int logged;

    if (!s_mayInject || !w || !h)
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
        cfglog(c);
    }
}

int tagpu_cfg_display_held(void) { return s_displayHeld; }
int tagpu_cfg_maxfps_held(void)  { return s_maxfpsHeld; }
int tagpu_cfg_window_held(void)  { return s_windowHeld; }
