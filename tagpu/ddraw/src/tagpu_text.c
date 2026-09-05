/* tagpu_text.c — TA's own glyphs, rasterised into an atlas of ours (G13p).
   See tagpu_text.h for why the engine's blitter can be called with our
   destination and why the font and colour are snapshotted rather than read
   live.

   THE FONT OBJECT, byte for byte (`DrawTextCustomFont 0x4C14F0`'s measure loop
   at `0x4C1527` and the blitter `0x4CCF60`; both read the same four fields):

     font+0x00  u8    glyph height in ROWS. `0x4C1659` also adds it to y to make
                      the measured box's bottom edge, which is why an earlier
                      note here called it a "baseline offset" — it is the row
                      count, used as both.
     font+0x02  s8    a row offset the blitter SUBTRACTS from the y it is given
                      (`sub eax,ebx` at `0x4CCF87` after `movsx ebx,[esi+2]`)
     font+0x03  u8    first character code
     font+0x04  u16[] per-character offset, indexed by (char - first);
                      0 = the glyph is absent and the cursor does NOT advance
     font+off   u8    that glyph's width, in pixels AND in bits
     font+off+1 ...   the glyph, a packed MSB-first bitstream of rows x width
                      bits that runs ACROSS row boundaries — the bit counter in
                      `dl` is reset per GLYPH (`0x4CCFC8`) and not per row, so
                      only each glyph starts on a byte boundary

   and the blit itself, per pixel: `colour = bit ? fg : bg; if (colour !=
   transparent) *dst = colour`. With (255, 0, 0) that is a coverage mask — 255
   rather than 1 because the texel reaches the shader as `r/255`, and a mask of
   1 samples as 0.004 and fails any sane threshold. (It did: the first live run
   drew every glyph and discarded every fragment of it.)

   IT DOES NOT CLIP — no OFFSCREEN, no clip rect, not even a width — so it
   writes exactly `sum(widths) x rows` pixels and it is on US to have measured
   that first. The measure below is the engine's own loop, character for
   character, so the two cannot disagree about how much lands. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "opengl_utils.h"
#include "tagpu_text.h"

#define GFX_GLOBALS_PP  0x0051FBD0u   /* 0x4B6220 is `mov eax,ds:0x51FBD0; ret` */
#define GFX_FONT        0x204         /* SetFont 0x4C1420 writes it             */
#define GFX_FG          0x208         /* SetTextColors 0x4C13A0 writes it       */
#define BLIT_VA         0x004CCF60u

#define F_ROWS          0
#define F_YOFF          2
#define F_FIRST         3
#define F_TAB           4

/* The value a covered pixel gets. Full white, so the sampler sees 1.0. */
#define INK             255

/* The glyphs we will ever ask for: printable ASCII. The engine bounds neither
   the measure nor the blit against the offset table's length — it trusts the
   caller's string — so we bound the CHARACTER instead, which is the same thing
   from the other end and costs one compare. */
#define CH_LO           0x20
#define CH_HI           0x7E

/* 512 x 256 holds every string this pass has (nine digits, nine range names,
   three weapon-range names, six formatted weapon labels and "attack length")
   several times over at any font size TA ships. */
#define ATLAS_W         512
#define ATLAS_H         256
#define MAXSTR          64
#define STRMAX          40

typedef void (__cdecl *PFN_BLIT)(unsigned char*, int, const void*, const char*,
                                 int, int, int, int, int);

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* the snapshot, written by the game thread at hook 8 and read by the present
   thread; both are aligned dwords, so neither can be read half-written */
static const unsigned char* volatile g_font;
static volatile int                  g_fg = -1;

typedef struct { char s[STRMAX]; short ax, ay, w, h; } TXENT;

static TXENT s_ent[MAXSTR];
static int   s_nent;
static int   s_shelfX, s_shelfY, s_shelfH;
static int   s_dropped;
static const unsigned char* s_builtWith;   /* the font the atlas holds         */
static const unsigned char* s_fontOk;      /* ...and the one already validated */
static unsigned char s_atlas[ATLAS_W * ATLAS_H];
static int   s_dirty;
static GLuint s_tex;

void tagpu_text_snapshot(void)
{
    const char* g = *(const char* const*)GFX_GLOBALS_PP;
    if (!ptr_ok(g)) return;
    /* One SEH-guarded probe per distinct globals block, not one per frame: this
       runs at every hook 8, i.e. ~83 times per presented frame, and
       IsBadReadPtr is not free (markown's alpha table learned the same lesson). */
    if (IsBadReadPtr((void*)(g + GFX_FG), 4)) return;
    g_font = *(const unsigned char* const*)(g + GFX_FONT);
    g_fg   = *(const int*)(g + GFX_FG);
}

int tagpu_text_colour(void) { return g_fg; }

/* The font, validated as far as we will index it. `first` is read before the
   probe because the probe's length depends on it — so that first byte is
   covered by a probe of its own. */
static const unsigned char* font_ok(void)
{
    const unsigned char* f = g_font;
    int first, need;
    if (!ptr_ok(f)) return NULL;
    if (f == s_fontOk) return f;
    if (IsBadReadPtr((void*)f, F_TAB)) return NULL;
    first = f[F_FIRST];
    if (first > CH_HI) return NULL;
    need = F_TAB + (CH_HI + 1 - first) * 2;
    if (IsBadReadPtr((void*)f, (UINT_PTR)need)) return NULL;
    if (f[F_ROWS] == 0 || f[F_ROWS] > ATLAS_H) return NULL;
    s_fontOk = f;
    return f;
}

/* `0x4C1527`, character for character: a code below `first` and a glyph whose
   offset is 0 are both skipped WITHOUT advancing, and a NUL or a newline ends
   the string. */
static int measure(const unsigned char* f, const char* s, int* w, int* h)
{
    const unsigned short* tab = (const unsigned short*)(f + F_TAB);
    int first = f[F_FIRST];
    int total = 0;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        unsigned off;
        if (c == '\n') break;
        if (c < (unsigned)first || c < CH_LO || c > CH_HI) continue;
        off = tab[c - first];
        if (!off) continue;
        total += f[off];
    }
    *w = total;
    *h = f[F_ROWS];
    return total > 0;
}

/* A string the atlas could not take. Logged ONCE — the caller asks again every
   frame, and a per-frame line would bury the log it is meant to explain. */
static void drop(const char* s)
{
    char b[96];
    if (s_dropped++) return;
    _snprintf(b, sizeof b, "text: atlas FULL, dropping \"%.48s\" (%d strings)", s, s_nent);
    flog(b);
}

int tagpu_text_place(const char* s, int* ax, int* ay, int* w, int* h, int* yoff)
{
    const unsigned char* f = font_ok();
    int i, sw, sh, y;

    if (!f || !s || !*s) return 0;
    /* A new font is a new atlas: the glyphs in it are that font's, and the
       shelves are sized by its row count. */
    if (f != s_builtWith) {
        s_nent = 0; s_shelfX = 0; s_shelfY = 0; s_shelfH = 0; s_dropped = 0;
        memset(s_atlas, 0, sizeof s_atlas);
        s_builtWith = f;
        s_dirty = 1;
        flog("text: atlas reset (font changed)");
    }
    *yoff = (int)(signed char)f[F_YOFF];

    for (i = 0; i < s_nent; i++) {
        if (!strcmp(s_ent[i].s, s)) {
            *ax = s_ent[i].ax; *ay = s_ent[i].ay;
            *w  = s_ent[i].w;  *h  = s_ent[i].h;
            return 1;
        }
    }
    if (s_nent >= MAXSTR || strlen(s) >= STRMAX) { drop(s); return 0; }
    if (!measure(f, s, &sw, &sh)) return 0;
    if (sw > ATLAS_W || sh > ATLAS_H) { drop(s); return 0; }

    if (s_shelfX + sw > ATLAS_W) { s_shelfY += s_shelfH; s_shelfX = 0; s_shelfH = 0; }
    if (s_shelfY + sh > ATLAS_H) { drop(s); return 0; }

    /* clear first: the blitter stores nothing for a clear bit (bg == the
       transparent index), so an unclear slot would keep the last string's ink */
    for (y = 0; y < sh; y++)
        memset(s_atlas + (size_t)(s_shelfY + y) * ATLAS_W + s_shelfX, 0, (size_t)sw);
    /* x=0 and y=font+0x02 put the string's first pixel at the sub-rect's own
       origin, because the blitter's destination is base + (y - font[2])*pitch + x */
    ((PFN_BLIT)BLIT_VA)(s_atlas + (size_t)s_shelfY * ATLAS_W + s_shelfX, ATLAS_W,
                        f, s, 0, (int)(signed char)f[F_YOFF], INK, 0, 0);

    strcpy(s_ent[s_nent].s, s);
    s_ent[s_nent].ax = (short)s_shelfX;
    s_ent[s_nent].ay = (short)s_shelfY;
    s_ent[s_nent].w  = (short)sw;
    s_ent[s_nent].h  = (short)sh;
    *ax = s_shelfX; *ay = s_shelfY; *w = sw; *h = sh;
    s_nent++;
    s_shelfX += sw;
    if (sh > s_shelfH) s_shelfH = sh;
    s_dirty = 1;
    return 1;
}

void tagpu_text_dims(int* w, int* h) { *w = ATLAS_W; *h = ATLAS_H; }

unsigned int tagpu_text_tex(void)
{
    if (!s_nent) return 0;
    if (!s_tex) {
        glGenTextures(1, &s_tex);
        if (!s_tex) return 0;
        glBindTexture(GL_TEXTURE_2D, s_tex);
        /* NEAREST for the same reason the captured layer uses it: the texel is
           a coverage bit, and a filtered half of one is neither */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_dirty = 1;
    } else {
        glBindTexture(GL_TEXTURE_2D, s_tex);
    }
    if (s_dirty) {
        /* the whole 128 KB, because it only ever changes when a string appears
           for the first time — around twenty times in a session */
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, ATLAS_H, 0,
                     GL_RED, GL_UNSIGNED_BYTE, s_atlas);
        s_dirty = 0;
    }
    return s_tex;
}

void tagpu_text_glreset(void)
{
    s_tex = 0;            /* the id died with the context */
    s_dirty = 1;
}

int tagpu_text_stats(int* strings, int* dropped)
{
    if (strings) *strings = s_nent;
    if (dropped) *dropped = s_dropped;
    return s_nent;
}
