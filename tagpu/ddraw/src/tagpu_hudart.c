/* tagpu_hudart.c -- the replay HUD's files, and the in-game wiki's, loaded. Contract:
   tagpu_hudart.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lodepng.h"
#include "tagpu_log.h"
#include "tagpu_pak.h"
#include "tagpu_wikilay.h"

/* the declarations first: the implementation below is included once, and stb_truetype guards
   only its declarations against a second inclusion */
#include "tagpu_hudtext.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "tagpu_hudart.h"

static volatile LONG s_started;
static volatile LONG s_ready;           /* 1: s_art is complete; 2: the load failed */
static HUDART s_art;
static volatile LONG s_wready;          /* the same for s_wiki                      */
static WIKIART s_wiki;

/* ---- the archives (tools/hudpak.py, THE FORMAT; tagpu_pak.h) ----------------------------------- */

static const unsigned char* s_pak;
static size_t s_npak;

/* a file of the HUD's archive by its path under art/hud/ */
static const unsigned char* pak_file(const char* path, size_t* n)
{
    return tagpu_pak_file(s_pak, s_npak, path, n);
}

/* where a kit's parts are: an archive and the folder in it, `parts/` or `kit/parts/` */
typedef struct { const unsigned char* pak; size_t npak; const char* dir; } PARTS;

/* a PNG's size from its IHDR, which is always the first chunk; `ctx` a PARTS */
static int png_size(void* ctx, const char* part, int* w, int* h)
{
    const PARTS* at = (const PARTS*)ctx;
    char path[80];
    size_t n;
    const unsigned char* p;
    snprintf(path, sizeof path, "%s%s.png", at->dir, part);
    if (!(p = tagpu_pak_file(at->pak, at->npak, path, &n)) || n < 24 || memcmp(p + 12, "IHDR", 4)) return 0;
    *w = (int)((p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19]);
    *h = (int)((p[20] << 24) | (p[21] << 16) | (p[22] << 8) | p[23]);
    return 1;
}

/* ---- the faces ---------------------------------------------------------------------------------

   A style names a family and a weight; its face is fonts/<family, lower case, no spaces>-<weight>
   .ttf. THE LINE'S METRICS ARE THE BROWSER'S: the ascent and descent tagpu_hudlay_baseline places
   a line by come from the OS/2 table's typo metrics when the face sets USE_TYPO_METRICS
   (fsSelection bit 7) and from hhea otherwise -- the rule Chrome follows. The faces of 2026-10-08
   set the bit and carry equal values in both tables. */
static int face_open(HUDFACE* f, char* err, size_t errn)
{
    size_t n;
    const unsigned char* p = pak_file(f->file, &n);
    int asc, desc, gap, upem;
    stbtt_uint32 os2;
    if (!p || !stbtt_InitFont(&f->info, p, stbtt_GetFontOffsetForIndex(p, 0))) {
        snprintf(err, errn, "no face %s", f->file);
        return 0;
    }
    upem = ttUSHORT(f->info.data + f->info.head + 18);
    os2 = stbtt__find_table(f->info.data, (stbtt_uint32)f->info.fontstart, "OS/2");
    if (os2 && (ttUSHORT(f->info.data + os2 + 62) & 0x80))
        stbtt_GetFontVMetricsOS2(&f->info, &asc, &desc, &gap);
    else
        stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    if (upem <= 0) { snprintf(err, errn, "%s has no unitsPerEm", f->file); return 0; }
    f->ascent = (float)asc / (float)upem;
    f->descent = (float)-desc / (float)upem;
    return 1;
}

static int faces(char* err, size_t errn)
{
    const HUDGLOBALS* g = tagpu_hudlay_globals(s_art.lay);
    int i, j;
    for (i = 0; i < g->nstyle; i++) {
        char file[48], fam[32];
        const char* s;
        int k = 0;
        for (s = g->style[i].font; *s && k + 1 < (int)sizeof fam; s++)
            if (*s != ' ') fam[k++] = (char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
        fam[k] = 0;
        snprintf(file, sizeof file, "fonts/%s-%d.ttf", fam, g->style[i].weight);
        for (j = 0; j < s_art.nface && strcmp(s_art.face[j].file, file); j++) {}
        if (j == s_art.nface) {
            if (s_art.nface >= HUD_MAXFACE) { snprintf(err, errn, "more than %d faces", HUD_MAXFACE); return 0; }
            snprintf(s_art.face[j].file, sizeof s_art.face[j].file, "%s", file);
            if (!face_open(&s_art.face[j], err, errn)) return 0;
            s_art.nface++;
        }
        s_art.style_face[i] = j;
    }
    return 1;
}

/* the layout's measure: a string's width in a style's face, as tagpu_huddraw.c sets it */
static double measure(void* ctx, int style, double size, const char* s)
{
    const HUDGLOBALS* g = tagpu_hudlay_globals(s_art.lay);
    (void)ctx;
    if (style < 0 || style >= g->nstyle) return 0;
    return tagpu_hudtext_width(&s_art.face[s_art.style_face[style]].info, (float)size, g->style[style].spacing,
                               g->style[style].upper, s, strlen(s));
}

/* ---- the parts ------------------------------------------------------------------------------- */

/* PREMULTIPLIED ONCE, HERE, so that the filtering and the mipmaps the pass makes average colours
   weighted by their coverage: averaged straight, a transparent texel's colour would bleed into
   every edge it borders. */
static void premultiply(unsigned char* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++, p += 4) {
        unsigned a = p[3];
        p[0] = (unsigned char)((p[0] * a + 127) / 255);
        p[1] = (unsigned char)((p[1] * a + 127) / 255);
        p[2] = (unsigned char)((p[2] * a + 127) / 255);
    }
}

/* every part of the kit `a->lay`, decoded from `at` into a->img */
static int parts(HUDART* a, const PARTS* at, char* err, size_t errn, double* mpx)
{
    int i, n = tagpu_hudlay_nparts(a->lay);
    if (n > (int)(sizeof a->img / sizeof a->img[0])) { snprintf(err, errn, "%d parts, more than the HUD holds", n); return 0; }
    *mpx = 0;
    for (i = 0; i < n; i++) {
        const HUDPART* k = tagpu_hudlay_part(a->lay, i);
        char path[80];
        size_t len;
        const unsigned char* png;
        unsigned w, h, e;
        unsigned char* rgba = NULL;
        snprintf(path, sizeof path, "%s%s.png", at->dir, k->name);
        if (!(png = tagpu_pak_file(at->pak, at->npak, path, &len))) { snprintf(err, errn, "no %s", path); return 0; }
        if ((e = lodepng_decode32(&rgba, &w, &h, png, len))) {
            snprintf(err, errn, "%s: %s", path, lodepng_error_text(e));
            return 0;
        }
        if ((int)w != k->pw || (int)h != k->ph) {
            free(rgba);
            snprintf(err, errn, "%s: decoded %ux%u, its header said %dx%d", path, w, h, k->pw, k->ph);
            return 0;
        }
        premultiply(rgba, (size_t)w * h);
        a->img[i].w = (int)w;
        a->img[i].h = (int)h;
        a->img[i].rgba = rgba;
        *mpx += (double)w * h / 1e6;
    }
    return 1;
}

/* THE WIKI'S ART, after the HUD's on the same thread: its tree and entries (tagpu_wikidata.c), its
   page's layout (layout.json, tagpu_wikilay.c) and its window's kit, kit/kit.json and kit/parts/ of
   WIKIPAK (tools/hudart/fit.py --set wiki). The kit's text is set in the HUD's faces and styles,
   so its HUDART shares the HUD's faces, which are immutable once the HUD is published. A failure
   here is the wiki's alone: the HUD stands. */
static void wiki_load(void)
{
    char err[256];
    size_t nk, nl;
    const char *kt, *lt;
    const unsigned char* wpak;
    size_t nwpak;
    DWORD t0 = GetTickCount();
    double mpx = 0;
    WIKILAY* wl = NULL;
    PARTS at;
    int i;
    err[0] = 0;
    if (!(s_wiki.data = tagpu_wikidata_load_resource(err, sizeof err))) {}
    else if (!(wpak = tagpu_pak_resource("WIKIPAK", &nwpak)))
        snprintf(err, sizeof err, "no WIKIPAK resource");
    else if (!(kt = (const char*)tagpu_pak_file(wpak, nwpak, "kit/kit.json", &nk)) ||
             !(lt = (const char*)tagpu_pak_file(wpak, nwpak, "layout.json", &nl)))
        snprintf(err, sizeof err, "the archive lacks kit/kit.json or layout.json");
    else if (at.pak = wpak, at.npak = nwpak, at.dir = "kit/parts/",
             !(wl = tagpu_wikilay_load(lt, nl, kt, nk, png_size, &at, s_art.lay, err, sizeof err))) {}
    else if ((s_wiki.draw.lay = tagpu_wikilay_kit(wl)), !parts(&s_wiki.draw, &at, err, sizeof err, &mpx)) {}
    if (err[0]) {
        tagpu_logf("wiki: NOT loaded - %s", err);
        InterlockedExchange(&s_wready, 2);
        return;
    }
    /* THE FACE LIGHTNESS, once, from the parts as decoded (premultiplied), before the store that
       publishes the layout: the ink of every text on a part follows it (wiki.js faceLight) */
    for (i = 0; i < tagpu_hudlay_nparts(s_wiki.draw.lay); i++) {
        const HUDPART* k = tagpu_hudlay_part(s_wiki.draw.lay, i);
        tagpu_wikilay_light(wl, i, tagpu_wikilay_face_light(s_wiki.draw.img[i].rgba, s_wiki.draw.img[i].w,
                                                            s_wiki.draw.img[i].h, k->kind == HK_NINE ? k->slice : NULL, 1));
    }
    s_wiki.lay = wl;
    s_wiki.draw.nface = s_art.nface;
    memcpy(s_wiki.draw.face, s_art.face, sizeof s_wiki.draw.face);
    memcpy(s_wiki.draw.style_face, s_art.style_face, sizeof s_wiki.draw.style_face);
    tagpu_logf("wiki: loaded %d entries and %d kit parts (%.2f Mpx) in %lu ms", s_wiki.data->nentry,
               tagpu_hudlay_nparts(s_wiki.draw.lay), mpx, (unsigned long)(GetTickCount() - t0));
    InterlockedExchange(&s_wready, 1);
}

static DWORD WINAPI loader(LPVOID unused)
{
    char err[256];
    size_t nl, nk;
    const char *lt, *kt;
    DWORD t0 = GetTickCount();
    double mpx = 0;
    HUDLAY* lay = NULL;
    PARTS at;
    (void)unused;
    err[0] = 0;
    if (!(s_pak = tagpu_pak_resource("HUDPAK", &s_npak)))
        snprintf(err, sizeof err, "no HUDPAK resource");
    else if (!(lt = (const char*)pak_file("layout.json", &nl)) || !(kt = (const char*)pak_file("kit.json", &nk)))
        snprintf(err, sizeof err, "the archive lacks layout.json or kit.json");
    else if (at.pak = s_pak, at.npak = s_npak, at.dir = "parts/",
             !(lay = tagpu_hudlay_load(lt, nl, kt, nk, png_size, &at, err, sizeof err))) {}
    else if ((s_art.lay = lay), !faces(err, sizeof err)) {}
    else if (!parts(&s_art, &at, err, sizeof err, &mpx)) {}
    if (err[0]) {
        tagpu_logf("hud: NOT loaded - %s", err);
        tagpu_logf("wiki: NOT loaded - it draws in the HUD's faces and pass");
        InterlockedExchange(&s_ready, 2);
        InterlockedExchange(&s_wready, 2);
        return 0;
    }
    /* the faces are the measure's, so it is given once they are open, and before s_ready
       publishes the layout to the render thread */
    tagpu_hudlay_measure(lay, measure, NULL);
    tagpu_logf("hud: loaded %d parts (%.1f Mpx) and %d faces in %lu ms",
               tagpu_hudlay_nparts(s_art.lay), mpx, s_art.nface, (unsigned long)(GetTickCount() - t0));
    InterlockedExchange(&s_ready, 1);
    wiki_load();
    return 0;
}

void tagpu_hudart_start(void)
{
    HANDLE t;
    if (InterlockedCompareExchange(&s_started, 1, 0) != 0) return;
    if (!(t = CreateThread(NULL, 0, loader, NULL, 0, NULL))) {
        tagpu_logf("hud: NOT loaded - CreateThread failed (%lu)", (unsigned long)GetLastError());
        InterlockedExchange(&s_ready, 2);
        InterlockedExchange(&s_wready, 2);
        return;
    }
    CloseHandle(t);
}

int tagpu_hudart_failed(void)
{
    return InterlockedCompareExchange(&s_ready, 0, 0) == 2;
}

const HUDART* tagpu_hudart_get(void)
{
    return InterlockedCompareExchange(&s_ready, 0, 0) == 1 ? &s_art : NULL;
}

int tagpu_wikiart_done(void)
{
    return InterlockedCompareExchange(&s_wready, 0, 0) != 0;
}

const WIKIART* tagpu_wikiart_get(void)
{
    return InterlockedCompareExchange(&s_wready, 0, 0) == 1 ? &s_wiki : NULL;
}
