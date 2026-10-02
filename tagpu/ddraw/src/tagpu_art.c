/* tagpu_art.c — Impure's own UI art, inflated once. API and lifetime: tagpu_art.h.

   The blob is tools/artbake.py's: "TART" | u32 version | u32 entries, then per
   entry style[16] name[32] u16 frames u16 0, then per frame
   u16 code u16 w u16 h i16 hotx i16 hoty u8 key u8 0 and w*h indices. It is
   built with the DLL, but every length is still checked against what was
   inflated before it is read: a bake that disagrees with this reader fails here
   as "no art", never as a read past the end. */
#include <windows.h>
#include <string.h>
#include "tagpu_log.h"
#include "lodepng.h"
#include "tagpu_art.h"
#include "tagpu_art_gen.h"

#define ART_VERSION 1u
#define MAX_ENTRIES 512

static TAGPU_ART_ENTRY  s_ent[MAX_ENTRIES];
static TAGPU_ART_FRAME* s_frames;
static int              s_n;
static int              s_state;        /* 0 not tried, 1 ready, -1 failed */

static unsigned rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char* p) { return rd16(p) | (rd16(p + 2) << 16); }

static int parse(const unsigned char* b, size_t n)
{
    size_t at = 12, nframes = 0, pass;
    unsigned ents, i, f;

    if (n < 12 || memcmp(b, "TART", 4) || rd32(b + 4) != ART_VERSION) return 0;
    ents = rd32(b + 8);
    if (ents > MAX_ENTRIES) return 0;

    /* pass 0 counts the frames and checks every length; pass 1 fills */
    for (pass = 0; pass < 2; pass++) {
        size_t k = 0;
        at = 12;
        for (i = 0; i < ents; i++) {
            unsigned nf;
            if (at + 52 > n || b[at + 15] || b[at + 16 + 31]) return 0;   /* names NUL-ended */
            nf = rd16(b + at + 48);
            if (pass) {
                s_ent[i].style   = (const char*)b + at;
                s_ent[i].name    = (const char*)b + at + 16;
                s_ent[i].nframes = (int)nf;
                s_ent[i].frames  = s_frames + k;
            }
            at += 52;
            for (f = 0; f < nf; f++) {
                unsigned w, h;
                if (at + 12 > n) return 0;
                w = rd16(b + at + 2);
                h = rd16(b + at + 4);
                if ((size_t)w * h > n - at - 12) return 0;
                if (pass) {
                    TAGPU_ART_FRAME* fr = &s_frames[k];
                    fr->code = (unsigned short)rd16(b + at);
                    fr->w    = (unsigned short)w;
                    fr->h    = (unsigned short)h;
                    fr->hotx = (short)rd16(b + at + 6);
                    fr->hoty = (short)rd16(b + at + 8);
                    fr->key  = b[at + 10];
                    fr->pix  = b + at + 12;
                }
                k++;
                at += 12 + (size_t)w * h;
            }
        }
        if (at != n) return 0;
        if (!pass) {
            nframes = k;
            s_frames = (TAGPU_ART_FRAME*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                   (nframes ? nframes : 1) * sizeof *s_frames);
            if (!s_frames) return 0;
        }
    }
    s_n = (int)ents;
    return 1;
}

int tagpu_art_init(void)
{
    unsigned char* out = NULL;
    size_t outn = 0;
    char m[160];

    if (s_state) return s_state > 0;
    s_state = -1;
    if (lodepng_zlib_decompress(&out, &outn, tagpu_art_zlib, TAGPU_ART_ZLIB_SIZE,
                                &lodepng_default_decompress_settings) ||
        outn != TAGPU_ART_RAW_SIZE || !parse(out, outn)) {
        /* `out` stays allocated on a parse failure: s_ent may point into it */
        tagpu_log("art: NOT available - the baked art does not inflate or parse");
        return 0;
    }
    s_state = 1;
    wsprintfA(m, "art: %d entries, %u bytes", s_n, (unsigned)outn);
    tagpu_log(m);
    return 1;
}

static const TAGPU_ART_ENTRY* find(const char* style, const char* name)
{
    int i;
    for (i = 0; i < s_n; i++)
        if (!strcmp(s_ent[i].style, style) && !strcmp(s_ent[i].name, name)) return &s_ent[i];
    return NULL;
}

const TAGPU_ART_ENTRY* tagpu_art_entry(const char* style, const char* name)
{
    const TAGPU_ART_ENTRY* e;
    if (s_state <= 0 || !name) return NULL;
    e = style ? find(style, name) : NULL;
    return e ? e : find("retail", name);
}

const TAGPU_ART_FRAME* tagpu_art_char(const TAGPU_ART_ENTRY* e, int code)
{
    int i;
    if (!e) return NULL;
    for (i = 0; i < e->nframes; i++)
        if (e->frames[i].code == code) return &e->frames[i];
    return NULL;
}

const TAGPU_ART_ENTRY* tagpu_art_at(int i)
{
    return (s_state > 0 && i >= 0 && i < s_n) ? &s_ent[i] : NULL;
}
