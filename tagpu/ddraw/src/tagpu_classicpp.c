/* tagpu_classicpp.c -- the Classic++ switch and its knobs (tagpu_classicpp.h).

   The rule the knobs feed is the lab's one lighting rule (tascene-view.html
   LAB_LIGHT, tascene-design.md "Level ground takes exactly 1.0"): an
   ambient-floored lambert divided by what level ground receives. Defaults are
   the lab's: the terrain sun 324.5,53.1 (north-west, the side the tile art
   is painted from -- artlight), the unit sun 215.5,53.1 (= tagpu_render3do.c's
   SH_L taken into map space: south-west, the camera light), amb 0.35. The two
   suns stay constant on every map (renderers.md 2.1, decided 2026-09-04). */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tagpu_classicpp.h"
#include "tagpu_opt.h"

#define ON_FILE   "tagpu_classicpp.on"
#define CFG_FILE  "tagpu_classicpp.cfg"
#define POLL_MS   500
#define DEF_SUN_AZ    324.5f
#define DEF_SUN_EL    53.1f
#define DEF_USUN_AZ   215.5f
#define DEF_USUN_EL   53.1f
#define DEF_AMB       0.35f

static void cplog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int          s_on = 0;
static int          s_assets = 1;         /* assets=: the restored atlases  */
static int          s_lit = 1;            /* light=:  the lambert           */
static DWORD        s_last = 0;
static TAGPU_LIGHT  s_light;
static int          s_cfgSeen = 0;        /* a cfg was read (or its absence logged) */
static FILETIME     s_cfgTime;
static DWORD        s_cfgSize;
static int          s_cfgPresent = -1;    /* -1 never checked, 0 absent, 1 present */

/* the lab's sunVector(az, el): degrees to the unit vector toward the light */
static void sun_vector(float az, float el, float v[3])
{
    double a = az * 3.14159265358979323846 / 180.0, e = el * 3.14159265358979323846 / 180.0;
    v[0] = (float)(sin(a) * cos(e));
    v[1] = (float)sin(e);
    v[2] = (float)(-cos(a) * cos(e));
}

/* amb + (1-amb)*max(n.sun, 0): the un-normalised rule, one copy so that the
   level divisor and a level normal's lambert are the same float */
static float lambert_raw(const float n[3], const float sun[3], float amb)
{
    float d = n[0] * sun[0] + n[1] * sun[1] + n[2] * sun[2];
    if (d < 0.0f) d = 0.0f;
    return amb + (1.0f - amb) * d;
}

static float level_of(const float sun[3], float amb)
{
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    float l = lambert_raw(up, sun, amb);
    /* the lab's floor: amb 0 with the sun on the horizon stays finite */
    return l > 1e-3f ? l : 1e-3f;
}

/* the shadows' defaults: the lab's readLook (tascene-view.html), SHADOWSUN_DEFAULT
   225,40 -- the engine's slant azimuth, lowered so a shadow is 1.2x the height */
#define DEF_SSUN_AZ 225.0f
#define DEF_SSUN_EL 40.0f
static void shadow_defaults(TAGPU_LIGHT* L)
{
    L->shadows = TAGPU_SHADOWS_SOFT;
    L->penumbra = 0.05f;
    L->shadowlenOn = 1; L->shadowlen[0] = 14.0f; L->shadowlen[1] = 0.25f;
    L->shade = 1.0f;
    /* OFF (2026-09-09, renderers.md 2.7b). The hills mesh casting on the ground
       it was built from self-shadows it: on open sea with nothing that can cast,
       the water darkens in the caster's own 16-unit lattice, and it gets WORSE as
       `Shadow quality` goes up because the bias is scaled to the texel while the
       error is scaled to the relief. Seven candidate fixes were swept to
       convergence and costed in the lab and every one removes the artifact and
       the terrain-shadow feature together at about one for one, because a cell's
       own relief IS the terrain shadow -- a hill shadowing the valley beside it
       is one cell's height difference read at range, a cell shadowing itself is
       the same difference read at zero range, so no depth threshold separates
       them. Until the fix that does not compare depths at all is built (a
       precomputed horizon / sun-visibility map, or a receiver-side ray-march),
       this default is the same trade every knob makes, taken for free -- and the
       1997 engine casts no terrain shadows either, so it is also the parity
       answer. `terrainshadow=1` in the cfg still turns it on: it is the fixture
       the fix will be measured against. Nothing in the render-options menu
       writes this key or can reach it (tagpu_menu.c `ours`). */
    L->terrainshadow = 0;
    L->shadowres = 2048;
    L->airshadow = TAGPU_AIRSHADOW_LEN;
}

static void apply(float sunAz, float sunEl, float usunAz, float usunEl, float amb,
                  float ssunAz, float ssunEl, int off, const char* how)
{
    char b[240];
    static const char* const air[3] = { "len", "physical", "drop" };
    static const char* const shad[3] = { "off", "soft", "hard" };
    if (amb < 0.0f) amb = 0.0f;
    if (amb > 1.0f) amb = 1.0f;
    /* G18b: `sun=off` IS `light=0` -- the level normal handed to taLambert, not
       amb forced to 1. The picture is the same to the pixel (measured in G18a:
       `light=0 shadows=0` and the old `sun=off` are byte-identical) and the
       shadow term, which lives inside the lambert and was multiplied by
       (1 - amb) = 0, survives. Nothing here touches `shadows=` any more: it is
       the player's key, and a lever that silently rewrote it made the log lie
       and did not put it back when the sun came on again. */
    if (off) s_lit = 0;
    sun_vector(sunAz, sunEl, s_light.sun);
    sun_vector(usunAz, usunEl, s_light.unitSun);
    sun_vector(ssunAz, ssunEl, s_light.shadowSun);
    s_light.amb = amb;
    s_light.level = level_of(s_light.sun, amb);
    s_light.unitLevel = level_of(s_light.unitSun, amb);
    /* its own line: `classicpp: light ` is a prefix the ta-drive skill greps */
    _snprintf(b, sizeof b, "classicpp: assets=%d light=%d (%s)", s_assets, s_lit, how);
    cplog(b);
    _snprintf(b, sizeof b, "classicpp: light sun=%s%.1f,%.1f unitsun=%.1f,%.1f amb=%.2f level=%.4f/%.4f (%s)",
              off ? "off " : "", sunAz, sunEl, usunAz, usunEl, amb,
              s_light.level, s_light.unitLevel, how);
    cplog(b);
    _snprintf(b, sizeof b, "classicpp: shadows=%d(%s) shadowsun=%.1f,%.1f penumbra=%.3f shadowlen=%s%.1f,%.2f"
              " shade=%.2f terrainshadow=%d shadowres=%d airshadow=%s",
              s_light.shadows, shad[s_light.shadows], ssunAz, ssunEl, s_light.penumbra,
              s_light.shadowlenOn ? "" : "off:", s_light.shadowlen[0], s_light.shadowlen[1],
              s_light.shade, s_light.terrainshadow, s_light.shadowres, air[s_light.airshadow]);
    cplog(b);
}

static void bad(const char* p)
{
    char b[160]; _snprintf(b, sizeof b, "classicpp: cfg: bad token \"%s\" ignored", p); cplog(b);
}

/* the cfg: whitespace-separated key=value tokens, unknown keys reported once */
static void read_cfg(void)
{
    HANDLE h;
    char buf[1024]; DWORD n = 0;
    float sunAz = DEF_SUN_AZ, sunEl = DEF_SUN_EL, usunAz = DEF_USUN_AZ, usunEl = DEF_USUN_EL;
    float amb = DEF_AMB;
    float ssunAz = DEF_SSUN_AZ, ssunEl = DEF_SSUN_EL;
    int off = 0;
    s_assets = 1; s_lit = 1;              /* the switch undivided: G18a's defaults */
    shadow_defaults(&s_light);
    h = CreateFileA(CFG_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { apply(sunAz, sunEl, usunAz, usunEl, amb, ssunAz, ssunEl, 0, "no cfg: defaults"); return; }
    if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
        char* p = buf;
        buf[n] = 0;
        while (*p) {
            char* q;
            int last;
            float a, e;
            while (*p && (unsigned char)*p <= ' ') p++;
            if (!*p) break;
            q = p;
            while (*q && (unsigned char)*q > ' ') q++;
            last = (*q == 0);
            *q = 0;
            if (!_strnicmp(p, "assets=", 7)) {
                s_assets = atoi(p + 7) != 0;
            } else if (!_strnicmp(p, "light=", 6)) {
                s_lit = atoi(p + 6) != 0;
            } else if (!_strnicmp(p, "sun=", 4)) {
                if (!lstrcmpiA(p + 4, "off")) off = 1;
                else if (sscanf(p + 4, "%f,%f", &a, &e) == 2) { sunAz = a; sunEl = e; }
                else { char b[160]; _snprintf(b, sizeof b, "classicpp: cfg: bad token \"%s\" ignored", p); cplog(b); }
            } else if (!_strnicmp(p, "unitsun=", 8)) {
                if (sscanf(p + 8, "%f,%f", &a, &e) == 2) { usunAz = a; usunEl = e; }
                else { char b[160]; _snprintf(b, sizeof b, "classicpp: cfg: bad token \"%s\" ignored", p); cplog(b); }
            } else if (!_strnicmp(p, "amb=", 4)) {
                amb = (float)atof(p + 4);
            } else if (!_strnicmp(p, "shadows=", 8)) {
                int m = atoi(p + 8);          /* 0 none, 1 soft, 2 hard (G18b) */
                if (m >= TAGPU_SHADOWS_OFF && m <= TAGPU_SHADOWS_HARD) s_light.shadows = m;
                else bad(p);                  /* out of range: the default stands */
            } else if (!_strnicmp(p, "shadowsun=", 10)) {
                if (sscanf(p + 10, "%f,%f", &a, &e) == 2) { ssunAz = a; ssunEl = e; }
                else bad(p);
            } else if (!_strnicmp(p, "penumbra=", 9)) {
                s_light.penumbra = (float)atof(p + 9);
                if (s_light.penumbra < 0.0f) s_light.penumbra = 0.0f;
            } else if (!_strnicmp(p, "shadowlen=", 10)) {
                if (!lstrcmpiA(p + 10, "off")) s_light.shadowlenOn = 0;
                else if (sscanf(p + 10, "%f,%f", &a, &e) == 2) {
                    s_light.shadowlenOn = 1; s_light.shadowlen[0] = a; s_light.shadowlen[1] = e;
                } else bad(p);
            } else if (!_strnicmp(p, "shade=", 6)) {
                s_light.shade = (float)atof(p + 6);
                if (s_light.shade < 0.0f) s_light.shade = 0.0f;
                if (s_light.shade > 1.0f) s_light.shade = 1.0f;
            } else if (!_strnicmp(p, "terrainshadow=", 14)) {
                s_light.terrainshadow = atoi(p + 14) != 0;
            } else if (!_strnicmp(p, "shadowres=", 10)) {
                int r = atoi(p + 10);
                if (r < 256) r = 256;
                if (r > 4096) r = 4096;
                s_light.shadowres = r;
            } else if (!_strnicmp(p, "airshadow=", 10)) {
                if (!lstrcmpiA(p + 10, "len")) s_light.airshadow = TAGPU_AIRSHADOW_LEN;
                else if (!lstrcmpiA(p + 10, "physical")) s_light.airshadow = TAGPU_AIRSHADOW_PHYSICAL;
                else if (!lstrcmpiA(p + 10, "drop")) s_light.airshadow = TAGPU_AIRSHADOW_DROP;
                else bad(p);
            } else {
                char b[160]; _snprintf(b, sizeof b, "classicpp: cfg: unknown token \"%s\" ignored", p); cplog(b);
            }
            if (last) break;
            p = q + 1;
        }
    }
    CloseHandle(h);
    apply(sunAz, sunEl, usunAz, usunEl, amb, ssunAz, ssunEl, off, CFG_FILE);
}

static void poll(void)
{
    DWORD t = GetTickCount();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    int present, changed;
    if (s_last != 0 && t - s_last <= POLL_MS) return;
    s_last = t;
    s_on = tagpu_opt_on(ON_FILE);
    present = GetFileAttributesExA(CFG_FILE, GetFileExInfoStandard, &fad) ? 1 : 0;
    changed = present != s_cfgPresent;
    if (present && !changed)
        changed = CompareFileTime(&fad.ftLastWriteTime, &s_cfgTime) != 0 ||
                  fad.nFileSizeLow != s_cfgSize;
    if (!s_cfgSeen || changed) {
        s_cfgSeen = 1;
        s_cfgPresent = present;
        if (present) { s_cfgTime = fad.ftLastWriteTime; s_cfgSize = fad.nFileSizeLow; }
        read_cfg();
    }
}

int tagpu_classicpp_on(void)
{
    poll();
    return s_on;
}

int tagpu_classicpp_assets(void)
{
    poll();
    return s_on && s_assets;
}

int tagpu_classicpp_lit(void)
{
    poll();
    return s_on && s_lit;
}

const TAGPU_LIGHT* tagpu_classicpp_light(void)
{
    poll();
    return &s_light;
}

float tagpu_classicpp_ground(const float n[3])
{
    poll();
    return lambert_raw(n, s_light.sun, s_light.amb) / s_light.level;
}
