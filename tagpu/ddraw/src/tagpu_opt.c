/* tagpu_opt.c -- the play defaults (tagpu_opt.h).

   Stateless on purpose: the readers poll from the game thread and the render thread
   on their own cadences (every 30 frames, or 500 ms), so this answers each question
   from the file system every time and caches nothing. Two extra attribute reads per
   poll is noise next to the frame. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_opt.h"

#define MASTER_OFF "tagpu_defaults.off"

/* `needs`: the pass this one is only useful with, so that its default follows that
   pass -- the pairing tacli's launch makes when it auto-arms the `*own` half (a
   stale owndraw with no native pass skips the engine's rasterise and nothing draws
   the unit at all), and vpwide with zoom (the ta-drive skill, "the wide viewport"). */
typedef struct { const char* on; const char* tokens; const char* needs; const char* needs2; } Def;

/* The table: the ta-drive skill's default arm set, Classic++ and the extra weapons.
   Order is the order the log line lists them in. */
/* THE `*own` LEVERS ARE NO LONGER PLAY DEFAULTS, AND THE REASON IS THE
   REFERENCE FRAME. `tagpu_terrown`, `tagpu_featown`, `tagpu_fxown`,
   `tagpu_markown` and `tagpu_owndraw` stop the ENGINE drawing something, so
   that ours can stand in its place. That was right while our world was
   composited OVER the engine's frame: whatever the engine drew and we also
   drew showed through as a double image, and suppressing its half was the
   cure.

   THE CLEAN CUT REMOVED THE COMPOSITE, so the engine's frame reaches no pixel
   of the screen and there is nothing left to show through. What the
   suppressions still do is damage the one thing the cut promised to keep: the
   reference. `tagpu_surf_take` captures the engine's composed frame as the
   golden source, and with these armed that capture holds a flat key-filled
   viewport with no terrain, no trees, no effects and no markers -- it is not
   the picture the 1997 software rasteriser draws, it is the picture it draws
   with five of its passes removed, which is worth nothing to compare against.
   Measured before this change: `TERROWN skip=1 filled=1`, `FEATOWN skip=1`,
   `OWND target=all skipped=0 passed=5311`.

   WHAT IT COSTS is the CPU the engine spends rasterising a frame nobody sees,
   which is real and is the whole of what these levers buy now. They are kept
   and still work -- arm `tagpu_terrown.on` and the engine's terrain stops,
   exactly as before -- so the trade is available to anyone who wants the
   frame time back and does not need the reference. It is not the default,
   because a golden source with holes in it is the more expensive mistake.
   [The vulkan-only plan, THE CLEAN CUT.] */
static const Def s_defs[] = {
    { "tagpu_native.on",    "all wrecks", 0, 0 },          /* every unit natively, 3D husks too   */
    { "tagpu_terr.on",      "", 0, 0 },                     /* terrain and the fog overlay         */
    { "tagpu_feat.on",      "", 0, 0 },                     /* trees, rocks, wreckage              */
    { "tagpu_fx.on",        "", 0, 0 },                     /* weapon fire, explosions, debris     */
    { "tagpu_sfx.on",       "", 0, 0 },                     /* the particle layers                 */
    { "tagpu_mark.on",      "", 0, 0 },                     /* bars, cursor, band box, digits      */
    { "tagpu_order.on",     "", 0, 0 },                     /* the shift-held order overlay        */
    { "tagpu_ghost.on",     "", "tagpu_native.on", 0 },     /* the building preview at the cursor  */
    { "tagpu_zoom.on",      "", 0, 0 },                     /* the wheel, the camera's range       */
    { "tagpu_vpwide.on",    "", "tagpu_zoom.on", 0 },       /* clicks land at zoom < 1             */
    /* `tagpu_gui.on` IS NOT A DRAW ANY MORE and is not armed here. The UI
       layer it turned on was deleted by the clean cut; what the file still
       does is switch on the op CENSUS and the diagnostics, which are a
       harness mode and never a play default. The observers themselves install
       at DllMain with no trigger at all, because they host every `tacli`
       verb. */
    { "tagpu_classicpp.on", "", 0, 0 },                     /* restored true colour, lit, shadowed */
    { "tagpu_weapons.on",   "", 0, 0 },                     /* 0..N weapons per unit               */
    /* HUD SCALE IS NOT A PLAY DEFAULT, and the reason is no longer the origin
       tear — that was the FIRST build (it moved the rect's L/T, which are
       0x498DA0's screen->world origin, while TA's world->screen projection is
       a +0x80/+0x20 pair of BAKED immediates, so picking answered about the
       unmoved origin; measured 2026-09-11 at 1024x768 Auto, 76 px left and
       19 px up). §22.6 ships and never writes L/T. What keeps it hand-armed
       is that nobody has decided it: it changes the look of every screen at
       every resolution, the top bar's right-hand half goes off-screen at any
       scale above 100 %, and whether Auto belongs on by default is the
       owner's call, not a gate's.

       AND SINCE THE CLEAN CUT IT DRAWS NOTHING AT ALL. The magnification was
       the UI layer's `LAY_FS` reading `uHud`, and that layer is deleted, so
       arming `tagpu_hud.on` today shifts the world and the input mapping and
       leaves no HUD behind to magnify. `tagpu_hud.c` is kept whole -- the
       geometry, the ceiling, the input transform and the menu stage all still
       work -- because the pass that consumes it is what has to be rebuilt.
       Arm tagpu_hud.on by hand; see gui-renderer.md 22.6. */
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
    if (exists(MASTER_OFF)) return NULL;
    if (off_name(on, off, sizeof off) && exists(off)) return NULL;
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
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
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
