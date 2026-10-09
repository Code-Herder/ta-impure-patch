/* tagpu_hudart.c -- the replay HUD's files, loaded. Contract: tagpu_hudart.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dllmain.h"
#include "lodepng.h"
#include "tagpu_log.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "tagpu_hudart.h"

static volatile LONG s_started;
static volatile LONG s_ready;           /* 1: s_art is complete; 2: the load failed */
static HUDART s_art;

/* ---- the archive (tools/hudpak.py, THE FORMAT) --------------------------------------------- */

static const unsigned char* s_pak;
static DWORD s_npak;

static uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* A file of the archive by its path under art/hud/. Every offset is checked against the
   archive's size, so a short or damaged resource names nothing rather than reading past it. */
static const unsigned char* pak_file(const char* path, size_t* n)
{
    uint32_t count, i, plen = (uint32_t)strlen(path);
    if (!s_pak || s_npak < 12 || memcmp(s_pak, "HPAK", 4) || rd32(s_pak + 4) != 1) return NULL;
    count = rd32(s_pak + 8);
    if (count > (s_npak - 12) / 16) return NULL;
    for (i = 0; i < count; i++) {
        const unsigned char* e = s_pak + 12 + 16 * i;
        uint32_t no = rd32(e), nl = rd32(e + 4), off = rd32(e + 8), len = rd32(e + 12);
        if (no > s_npak || nl > s_npak - no || off > s_npak || len > s_npak - off) return NULL;
        if (nl == plen && !memcmp(s_pak + no, path, plen)) { *n = len; return s_pak + off; }
    }
    return NULL;
}

/* a PNG's size from its IHDR, which is always the first chunk */
static int png_size(void* ctx, const char* part, int* w, int* h)
{
    char path[64];
    size_t n;
    const unsigned char* p;
    (void)ctx;
    snprintf(path, sizeof path, "parts/%s.png", part);
    if (!(p = pak_file(path, &n)) || n < 24 || memcmp(p + 12, "IHDR", 4)) return 0;
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

static int parts(char* err, size_t errn, double* mpx)
{
    int i, n = tagpu_hudlay_nparts(s_art.lay);
    if (n > (int)(sizeof s_art.img / sizeof s_art.img[0])) { snprintf(err, errn, "%d parts, more than the HUD holds", n); return 0; }
    *mpx = 0;
    for (i = 0; i < n; i++) {
        const HUDPART* k = tagpu_hudlay_part(s_art.lay, i);
        char path[64];
        size_t len;
        const unsigned char* png;
        unsigned w, h, e;
        unsigned char* rgba = NULL;
        snprintf(path, sizeof path, "parts/%s.png", k->name);
        if (!(png = pak_file(path, &len))) { snprintf(err, errn, "no %s", path); return 0; }
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
        s_art.img[i].w = (int)w;
        s_art.img[i].h = (int)h;
        s_art.img[i].rgba = rgba;
        *mpx += (double)w * h / 1e6;
    }
    return 1;
}

static DWORD WINAPI loader(LPVOID unused)
{
    char err[256];
    size_t nl, nk;
    const char *lt, *kt;
    HRSRC r;
    HGLOBAL g;
    DWORD t0 = GetTickCount();
    double mpx = 0;
    (void)unused;
    err[0] = 0;
    if (!(r = FindResourceA(g_ddraw_module, "HUDPAK", MAKEINTRESOURCEA(10))) || !(g = LoadResource(g_ddraw_module, r))
        || !(s_pak = (const unsigned char*)LockResource(g)) || !(s_npak = SizeofResource(g_ddraw_module, r)))
        snprintf(err, sizeof err, "no HUDPAK resource");
    else if (!(lt = (const char*)pak_file("layout.json", &nl)) || !(kt = (const char*)pak_file("kit.json", &nk)))
        snprintf(err, sizeof err, "the archive lacks layout.json or kit.json");
    else if (!(s_art.lay = tagpu_hudlay_load(lt, nl, kt, nk, png_size, NULL, err, sizeof err))) {}
    else if (!faces(err, sizeof err)) {}
    else if (!parts(err, sizeof err, &mpx)) {}
    if (err[0]) {
        tagpu_logf("hud: NOT loaded - %s", err);
        InterlockedExchange(&s_ready, 2);
        return 0;
    }
    tagpu_logf("hud: loaded %d parts (%.1f Mpx) and %d faces in %lu ms",
               tagpu_hudlay_nparts(s_art.lay), mpx, s_art.nface, (unsigned long)(GetTickCount() - t0));
    InterlockedExchange(&s_ready, 1);
    return 0;
}

void tagpu_hudart_start(void)
{
    HANDLE t;
    if (InterlockedCompareExchange(&s_started, 1, 0) != 0) return;
    if (!(t = CreateThread(NULL, 0, loader, NULL, 0, NULL))) {
        tagpu_logf("hud: NOT loaded - CreateThread failed (%lu)", (unsigned long)GetLastError());
        InterlockedExchange(&s_ready, 2);
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
