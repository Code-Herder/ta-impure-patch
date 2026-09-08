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
static const Def s_defs[] = {
    { "tagpu_native.on",    "all wrecks", 0, 0 },          /* every unit natively, 3D husks too   */
    { "tagpu_owndraw.on",   "all", "tagpu_native.on", 0 },  /* ...and the engine's composite wiped */
    { "tagpu_terr.on",      "", 0, 0 },                     /* terrain and the fog overlay         */
    { "tagpu_terrown.on",   "", "tagpu_terr.on", 0 },
    { "tagpu_feat.on",      "", 0, 0 },                     /* trees, rocks, wreckage              */
    { "tagpu_featown.on",   "", "tagpu_feat.on", 0 },
    { "tagpu_fx.on",        "", 0, 0 },                     /* weapon fire, explosions, debris     */
    { "tagpu_sfx.on",       "", 0, 0 },                     /* the particle layers                 */
    { "tagpu_fxown.on",     "", "tagpu_fx.on", "tagpu_sfx.on" },
    { "tagpu_mark.on",      "", 0, 0 },                     /* bars, cursor, band box, digits      */
    { "tagpu_markown.on",   "", "tagpu_mark.on", 0 },
    { "tagpu_order.on",     "", 0, 0 },                     /* the shift-held order overlay        */
    { "tagpu_zoom.on",      "", 0, 0 },                     /* the wheel, the camera's range       */
    { "tagpu_vpwide.on",    "", "tagpu_zoom.on", 0 },       /* clicks land at zoom < 1             */
    { "tagpu_gui.on",       "", 0, 0 },                     /* the GL UI layer, Classic 1:1        */
    { "tagpu_classicpp.on", "", 0, 0 },                     /* restored true colour, lit, shadowed */
    { "tagpu_weapons.on",   "", 0, 0 },                     /* 0..N weapons per unit               */
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
