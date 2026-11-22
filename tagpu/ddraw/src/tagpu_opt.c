/* tagpu_opt.c -- the play defaults (tagpu_opt.h).

   Stateless on purpose: the readers poll from the game thread and the render thread
   on their own cadences (every 30 frames, or 500 ms), so this answers each question
   from the file system every time and caches nothing. Two extra attribute reads per
   poll is noise next to the frame. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_opt.h"
#include "tagpu_log.h"
#include "tagpu_settings.h"

#define MASTER_OFF "tagpu_defaults.off"

/* `needs`: the pass this one is only useful with, so that its default follows that
   pass -- vpwide with zoom (the ta-drive skill, "the wide viewport"). */
/* `store`: the settings store's say on this pass (tagpu_settings.h) -- 1 on,
   0 off, -1 no say -- for a pass that is a menu row. It ranks below the pass's
   own .on/.off files and above the table's default. */
typedef struct {
    const char* on; const char* tokens; const char* needs; const char* needs2;
    int (*store)(void);
} Def;

/* The Renderer row: Classic is the one style that leaves Classic++ off. */
static int store_classicpp(void)
{
    int v;
    if (!tagpu_settings_get(TS_STYLE, &v)) return -1;
    return v != TS_STYLE_CLASSIC;
}

/* The Map edge row: mirror or black. */
static int store_mirror(void)
{
    int v;
    if (!tagpu_settings_get(TS_EDGE, &v)) return -1;
    return v != 0;
}

/* The Formation move row: on or off. */
static int store_formation(void)
{
    int v;
    if (!tagpu_settings_get(TS_FORMATION, &v)) return -1;
    return v != 0;
}

/* The table: the ta-drive skill's default arm set and Classic++. Order is the order
   the log line lists them in. Nothing that changes the simulation is here: the extra
   weapons are always on, in the fail-closed table (tagpu_weapons.h). */
/* THE ENGINE'S UNITS, FEATURES AND EFFECTS ARE NOT LEVERS. A Vulkan launch
   removes the engine's unit draw (tagpu_owndraw.c), its feature leaf
   (tagpu_featown.c) and its effects passes and particle layers
   (tagpu_fxown.c) at attach: the engine's frame reaches no pixel of the
   screen, so they covered nothing and cost the CPU a rasterisation nobody
   sees. The golden source `tagpu_surf_capture` takes, a diagnostic capture
   with no consumer in the tree, holds none of them. The reference picture with
   every engine draw in it is GDI's, the oracle, under `tagpu_gdi.on`.

   `tagpu_terrown` IS STILL A LEVER, A PLAY DEFAULT: it is an
   OPTIMISATION, kept on by default because it is free. The UI layer
   does not need it to find the viewport: `tagpu_gui_hook.c` marks the world
   phase, erases at the top of every in-play publish window and cuts the
   chrome fill round the viewport (gpu-status.md 2.81, the `*own` levers
   subsection, has the account and the numbers). MEASURED 2026-09-22 on
   `build-facing`, Two Continents, 1024x768, exactly-black pixels of the
   896x704 viewport: the play defaults and the play defaults plus
   `tagpu_terrown.off` both read 0.03 %, with the same 8898 distinct colours.

   WHAT IT STILL COSTS, said plainly: the golden source loses its terrain --
   `tacli shot` shows a key-filled viewport, `TERROWN skip=1 filled=1`.
   `tagpu_terrown.off` is the way back for a reference-quality capture, the
   same shape as `mark.on=passive`, and IT COSTS THE PICTURE NOTHING: with
   `terrown` off the fog source is ticked from the packet publisher's `after`
   instead of this module's fog-overlay detour (tagpu_packet_pub.c
   `wide_tick`). */
static const Def s_defs[] = {
    { "tagpu_native.on",    "all wrecks", 0, 0 },          /* every unit natively, 3D husks too   */
    { "tagpu_terr.on",      "", 0, 0 },                     /* terrain and the fog overlay         */
    { "tagpu_feat.on",      "", 0, 0 },                     /* trees, rocks, wreckage              */
    { "tagpu_fx.on",        "", 0, 0 },                     /* weapon fire, explosions, debris     */
    { "tagpu_sfx.on",       "", 0, 0 },                     /* the particle layers                 */
    { "tagpu_mark.on",      "", 0, 0 },                     /* bars, cursor, band box, digits      */
    { "tagpu_order.on",     "", 0, 0 },                     /* the shift-held order overlay        */
    /* MARKOWN IS A PRODUCER, WHICH IS WHY IT IS A LEVER ON THIS TABLE. The
       engine's units, features and effects are removed on a Vulkan launch
       (above) and are levers no more; without `markown` the presented frame has no
       health bars, group digits, build cursor, drag band box, build ghost,
       order markers, ShowRanges labels and NO text at all. The gates are only
       half of it: `mark_orders` is the sole caller of
       `tagpu_order_snapshot` and the hook-8 stub the sole caller of
       `tagpu_packet_pub_font_snapshot`, so without the redirects installed the
       order arena is never filled and the packet carries no font, and no
       marker pass can draw whatever its gates say.

       WHAT IT COSTS THE REFERENCE, said rather than left to be found: while
       our marker pass draws, it asks `markown` to skip the engine's own, so
       the reference frame loses its bars, digits, order markers and build
       cursor -- the ONE hole the play defaults leave in it, and the reason the
       skips stay dynamic rather than becoming unconditional. For a
       reference-quality capture, `tagpu_mark.on=passive` hands every one of
       them back (the engine draws, we do not) with the redirects still
       installed. */
    { "tagpu_markown.on",   "", "tagpu_mark.on", 0 },       /* the snapshots the two passes draw from */
    { "tagpu_terrown.on",   "", "tagpu_terr.on", 0 },       /* the engine's terrain pass skipped, and the key fill `uVpKey` still inverts against */
    { "tagpu_ghost.on",     "", "tagpu_native.on", 0 },     /* the building preview at the cursor  */
    { "tagpu_zoom.on",      "", 0, 0 },                     /* the wheel, the camera's range       */
    { "tagpu_vpwide.on",    "", "tagpu_zoom.on", 0 },       /* clicks land at zoom < 1             */
    { "tagpu_mirror.on",    "", 0, 0, store_mirror },       /* the world reflected past the map edge */
    /* FORMATION MOVE IS INPUT, not simulation: it chooses where each selected
       unit is sent and issues those orders through the engine's own order
       roots, as a click does (tagpu_formation.c). */
    { "tagpu_formation.on", "", 0, 0, store_formation },    /* a drag lays the selection out along the line */
    /* `tagpu_gui.on` IS A DRAW. The layer it arms has no composite -- the
       twins and the device-resolution sharp layer are ours, and `LAY_FS`
       declares no sampler for TA's own frame -- so it is the UI and belongs on
       the play defaults. Without it the game has
       no HUD, no sidebar, no minimap, no cursor and no shell.

       THE OBSERVERS STILL INSTALL WITHOUT IT, which is why the row can be a
       play default rather than a requirement: `tagpu_gui_init` puts the flip
       hook in at DllMain whatever this file says, because that hook hosts
       `tagpu_triggers_frame` and every `tacli` verb is dispatched from inside
       it. What the row turns on is the leaves, the op queue and the layer.
       `gui.off` therefore still gives a fully drivable instance with no UI,
       which is the A/B. */
    { "tagpu_gui.on",       "", 0, 0 },                     /* the UI: panel, bars, minimap, cursor, shell */
    { "tagpu_classicpp.on", "", 0, 0, store_classicpp },    /* restored true colour, lit, shadowed */
    /* THE RECORDER IS A PLAY DEFAULT: every skirmish and multiplayer game is recorded into
       replays\, which is what the Replays browser, a takeover and a live rewind work from, and
       a new recording prunes the old ones (tagpu_recfile.h, THE PRUNE). It records the game; it
       changes nothing the simulation runs. */
    { "tagpu_recorder.on",  "", 0, 0 },                     /* every game recorded, old ones pruned */
    /* HUD SCALE IS NOT A PLAY DEFAULT because nobody has decided it: it
       changes the look of every screen at every resolution, the top bar's
       right-hand half goes off-screen at any scale above 100 %, and whether
       Auto belongs on by default is the owner's call, not a gate's. It never
       writes the rect's L/T (§22.6): those are 0x498DA0's screen->world
       origin, while TA's world->screen projection is a +0x80/+0x20 pair of
       BAKED immediates, so moving them makes picking answer about the unmoved
       origin (measured 2026-09-11 at 1024x768 Auto, 76 px left and 19 px up).

       The magnification is the UI layer's `LAY_FS` reading `uHud`
       (tagpu_gui_surf.c, drawn by tagpu_vk_gui.c). Arm tagpu_hud.on by hand;
       see gui-renderer.md 22.6. */
};
#define NDEFS (int)(sizeof s_defs / sizeof s_defs[0])

static int exists(const char* path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* tagpu_<x>.on -> tagpu_<x>.off; 0 if the name is not of that shape */
static int off_name(const char* on, char* out, size_t cap)
{
    size_t n = strlen(on);
    if (n < 4 || n + 2 > cap || strcmp(on + n - 3, ".on") != 0) return 0;
    memcpy(out, on, n - 3);
    memcpy(out + n - 3, ".off", 5);
    return 1;
}

/* the table entry whose default applies right now, or NULL */
static const Def* applies(const char* on)
{
    char off[64];
    int i;
    for (i = 0; i < NDEFS; i++)
        if (!strcmp(s_defs[i].on, on)) break;
    if (i == NDEFS) return NULL;
    if (off_name(on, off, sizeof off) && exists(off)) return NULL;
    if (s_defs[i].store) {
        int say = s_defs[i].store();
        if (say >= 0) return say ? &s_defs[i] : NULL;
    }
    if (exists(MASTER_OFF)) return NULL;
    /* the pass it serves must be on -- by file or by its own default (one level:
       nothing on the table needs a pass that itself needs another) */
    if (s_defs[i].needs && !tagpu_opt_on(s_defs[i].needs) &&
        !(s_defs[i].needs2 && tagpu_opt_on(s_defs[i].needs2))) return NULL;
    return &s_defs[i];
}

int tagpu_opt_on(const char* on)
{
    if (exists(on)) return 1;
    return applies(on) != NULL;
}

int tagpu_opt_read(const char* on, char* buf, unsigned cap)
{
    HANDLE h;
    DWORD n = 0;
    const Def* d;
    if (cap == 0) return -1;
    h = CreateFileA(on, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (!ReadFile(h, buf, cap - 1, &n, NULL)) n = 0;
        CloseHandle(h);
        buf[n] = 0;
        return (int)n;
    }
    d = applies(on);
    if (!d) { buf[0] = 0; return -1; }
    n = (DWORD)strlen(d->tokens);
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, d->tokens, n);
    buf[n] = 0;
    return (int)n;
}

static void olog(const char* s)
{
    tagpu_log(s);
}

void tagpu_opt_init(void)
{
    char b[512];
    int i, p;
    if (exists(MASTER_OFF)) {
        olog("opt: play defaults OFF (" MASTER_OFF " present): every pass is opt-in, by its .on file");
        return;
    }
    p = _snprintf(b, sizeof b, "opt: play defaults ON (no " MASTER_OFF "):");
    /* per pass, the answer the readers will get: =file (its .on exists), the
       default's tokens, or =OFF -- by its .off file OR because the pass it
       serves is off (the `needs` column), which is why applies() decides and
       not the files alone */
    for (i = 0; i < NDEFS; i++) {
        const Def* d = &s_defs[i];
        const char* name = d->on + 6;                     /* past "tagpu_" */
        int len = (int)strlen(name) - 3;                  /* before ".on"  */
        const char* how = exists(d->on) ? "=file" : applies(d->on) ? (d->tokens[0] ? "=" : "") : "=OFF";
        int k = _snprintf(b + p, sizeof b - p, " %.*s%s%s", len, name, how,
                          (how[0] == '=' && how[1] == 0) ? d->tokens : "");
        if (k < 0 || k >= (int)(sizeof b - p)) break;    /* truncated: stop, do not walk back */
        p += k;
    }
    b[sizeof b - 1] = 0;
    olog(b);
}
