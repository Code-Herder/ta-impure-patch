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
/* the distinct strings the atlas refused; the first MAXDROP are remembered by
   name so a repeat is not counted twice */
#define MAXDROP  16
static char  s_drop[MAXDROP][STRMAX];
static int   s_ndrop;                      /* distinct strings refused         */
static int   s_nremem;                     /* ...of which we remember the text */
static unsigned s_builtGen;                /* the font generation it holds     */
static const unsigned char* s_fontOk;      /* ...and the one already validated */
static unsigned char s_fontOkSig[3];       /* its rows/yoff/first, as validated */
static unsigned s_fontGen;                 /* bumped whenever those change      */
static const char*          s_gfxOk;       /* globals block already probed     */
static unsigned char s_atlas[ATLAS_W * ATLAS_H];
static int   s_dirty;
static GLuint s_tex;

void tagpu_text_snapshot(void)
{
    const char* g = *(const char* const*)GFX_GLOBALS_PP;
    if (!ptr_ok(g)) return;
    /* ONE SEH-guarded probe per distinct globals block, and the cache is what
       makes that true: this runs at every hook 8, i.e. ~83 times per presented
       frame, and an SEH-guarded probe is not free. markown's alpha table
       learned exactly this and grew a `g_gfxOk` for it — there the probe was
       IsBad*Write*Ptr, which really does read-modify-write the page it tests;
       this one only reads, and the cost is the guard, not a write into engine
       memory; the first revision here wrote the comment and left the
       probe unconditional, which is worse than the "one per frame" it disclaims.
       The probe spans BOTH fields we read (`+0x204` and `+0x208`), so a block
       whose 0x204 sits on the previous page is refused rather than faulted. */
    if (g != s_gfxOk) {
        if (IsBadReadPtr((void*)(g + GFX_FONT), GFX_FG + 4 - GFX_FONT)) return;
        s_gfxOk = g;
    }
    g_font = *(const unsigned char* const*)(g + GFX_FONT);
    g_fg   = *(const int*)(g + GFX_FG);
}

int tagpu_text_colour(void) { return g_fg; }

/* The font this FRAME rasterises with. The game thread republishes `g_font` at
   every hook 8, ~83 times per present, so reading it per string would let a font
   change land between the ShowRanges labels and the group digit — and a change
   re-packs the atlas from scratch, which would leave the quads already emitted
   into tagpu_mark.c's bucket naming texels that have just been cleared. Every
   one of those labels would sample 0 and discard every fragment. Latched once
   per gather instead, so within a frame the atlas cannot move under anyone. */
static const unsigned char* s_frameFont;

void tagpu_text_frame(void) { s_frameFont = g_font; }

/* The font's HEADER and offset table, validated. Not the glyphs: an entry in
   that table is an unbounded `u16`, so `f + off` reaches up to 64 KB past `f`
   and no probe here could cover it. `measure()` probes each glyph it accepts
   instead, which is the only place that knows how long one is.

   `first` is read before the table probe because the probe's length depends on
   it, so that byte gets a probe of its own.

   THE CACHE IS BY POINTER PLUS A HEADER FINGERPRINT. Pointer identity alone is
   what an allocator recycles: a different font object at the same address would
   otherwise skip both this validation and the atlas reset, and be drawn with the
   previous font's metrics out of the previous font's texels. The three header
   bytes are a weak check and are honestly weak — they catch a font of a
   different size or range, not a different font of the same shape — but they
   cost nothing and the alternative is trusting an address. */
static const unsigned char* font_ok(void)
{
    const unsigned char* f = s_frameFont;
    unsigned char sig[3];
    int first, need;

    if (!ptr_ok(f)) return NULL;
    if (IsBadReadPtr((void*)f, F_TAB)) return NULL;
    sig[0] = f[F_ROWS]; sig[1] = f[F_YOFF]; sig[2] = f[F_FIRST];
    if (f == s_fontOk && !memcmp(sig, s_fontOkSig, sizeof sig)) return f;

    first = sig[2];
    if (first > CH_HI) return NULL;
    need = F_TAB + (CH_HI + 1 - first) * 2;
    if (IsBadReadPtr((void*)f, (UINT_PTR)need)) return NULL;
    if (sig[0] == 0 || sig[0] > ATLAS_H) return NULL;
    s_fontOk = f;
    memcpy(s_fontOkSig, sig, sizeof sig);
    s_fontGen++;                  /* a different font, whatever its address */
    return f;
}

/* THE STRING WE WILL ACTUALLY RASTERISE, and the measure of it, from one pass.

   These two must agree about every character or the blit writes past the width
   we reserved — and they cannot be made to agree by bounding the CHARACTER on
   our side alone, which is what the first revision did: `0x4CCF60` skips a code
   below `first` and a zero table entry and NOTHING ELSE (`0x4CCFAA`,
   `0x4CCFB9`), so a byte outside our `[CH_LO, CH_HI]` window with a non-zero
   offset entry is measured as nothing here and blitted as a glyph there. Every
   string this pass has is an ASCII literal, so it was not reachable — but the
   invariant was held by the call sites rather than by the code, in a function
   whose contract is "hand me any string".

   So the filter is applied to the STRING: `out` is the subsequence the blitter
   will draw, and `total` is its width. Rasterise `out`, not `s`, and the two
   cannot disagree. `0x4C1527`, character for character otherwise: a NUL or a
   newline ends it, and a skipped code does not advance the cursor. */
static int measure(const unsigned char* f, const char* s, char* out, size_t outsz,
                   int* w, int* h)
{
    const unsigned short* tab = (const unsigned short*)(f + F_TAB);
    int first = f[F_FIRST];
    int rows = f[F_ROWS];
    int total = 0;
    size_t n = 0;
    for (; *s && n + 1 < outsz; s++) {
        unsigned c = (unsigned char)*s;
        unsigned off;
        int gw;
        if (c == '\n') break;
        if (c < (unsigned)first || c < CH_LO || c > CH_HI) continue;
        off = tab[c - first];
        if (!off) continue;
        /* The glyph is at an unbounded u16 offset, so this is the first read of
           it and the only place its length is known: probe the width byte, then
           the bitstream the blit will walk (rows x width bits, one byte per
           eight, restarted per glyph). A glyph that will not read is dropped
           from `out` and therefore never reaches the blitter. */
        if (IsBadReadPtr((void*)(f + off), 1)) continue;
        gw = f[off];
        /* A ZERO WIDTH IS NOT A ZERO-WIDTH GLYPH — it writes 256 columns. The
           blit's per-row counter is a do-while: `mov ch,cl` at `0x4CCFCA` and
           `dec ch; je` at `0x4CCFE9`, so `cl == 0` wraps to 255 and runs 256
           times, on every row, while this measure would have reserved nothing.
           The subsequence filter below is what keeps measure and blit agreeing
           about the CHARACTER SET; this is the same class one level down, and
           the guard is one compare. [BINARY-VERIFIED] */
        if (gw <= 0) continue;
        if (IsBadReadPtr((void*)(f + off + 1), (UINT_PTR)((rows * gw + 7) / 8)))
            continue;
        total += gw;
        out[n++] = (char)c;
    }
    out[n] = 0;
    *w = total;
    *h = rows;
    return total > 0;
}

/* A string the atlas could not take. Counted as DISTINCT strings, not as calls:
   the caller asks again every frame, so a per-call tally would report the frame
   rate rather than what was lost — in the one log line somebody reads when text
   goes missing. Logged once for the same reason. */
static void drop(const char* s)
{
    char b[96];
    int i;
    for (i = 0; i < s_nremem; i++)
        if (!strcmp(s_drop[i], s)) return;                 /* already counted */
    /* A string we cannot remember is not counted either. Remembering is what
       makes the count DISTINCT strings, so incrementing past the memory would
       put the per-frame call tally back — which is the defect this counter was
       rewritten to remove, reappearing only in the degraded state where nobody
       would look for it. Two strings that do not fit in the memory are reported
       as one; that is a known undercount and it is the honest half. */
    if (s_nremem >= MAXDROP || strlen(s) >= STRMAX) return;
    strcpy(s_drop[s_nremem++], s);
    if (!s_ndrop++) {
        _snprintf(b, sizeof b, "text: atlas FULL, dropping \"%.48s\" (%d strings)",
                  s, s_nent);
        flog(b);
    }
}

int tagpu_text_place(const char* s, int* ax, int* ay, int* w, int* h, int* yoff)
{
    const unsigned char* f = font_ok();
    char draw[STRMAX];
    int i, sw, sh, y;

    if (!f || !s || !*s) return 0;
    /* A new font is a new atlas: the glyphs in it are that font's, and the
       shelves are sized by its row count. Keyed on the GENERATION, not on the
       pointer — an allocator that hands the same address to a different font
       would otherwise leave the old glyphs in place and the old rects cached. */
    if (s_builtGen != s_fontGen) {
        s_nent = 0; s_shelfX = 0; s_shelfY = 0; s_shelfH = 0;
        s_ndrop = 0; s_nremem = 0;
        memset(s_atlas, 0, sizeof s_atlas);
        s_builtGen = s_fontGen;
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
    if (!measure(f, s, draw, sizeof draw, &sw, &sh)) return 0;
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
                        f, draw, 0, (int)(signed char)f[F_YOFF], INK, 0, 0);

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

/* ================================================================= glyphs */
/* THE GLYPH CACHE (G17d), and why the string atlas above cannot serve the UI.

   That atlas is keyed on the whole STRING, which is right for the world's
   markers: a range label and a group digit are a fixed set of a dozen texts
   that never change. The engine's UI is the opposite — the metal and energy
   readouts, the clock, unit counts and build percentages are a new string every
   tick, so a 64-entry string cache would evict itself several times a second
   and rasterise for ever. It also resets on any font change, and the UI switches
   font many times a frame.

   So the UI stamps GLYPHS. The engine's own blitter draws a string one glyph
   after another, advancing x by the glyph's own width byte and nothing else
   (`0x4C1527` and `0x4CCF60` walk the same table; there is no kerning), so a run
   of per-glyph quads at those same offsets is not an approximation of the
   engine's blit — it is the same arithmetic. Each font gets 95 cells, rasterised
   once, and any string in that font is then free.

   Its atlas is separate from the string one on purpose: they have different
   lifetimes (a font change repacks the string atlas and must not throw the UI's
   glyphs away) and different key spaces, and the marker path is a landed gate
   that should not move to make room for this one. */

#define GA_W       512
#define GA_H       256
#define GA_FONTS   8                       /* distinct fonts held at once      */
/* THE ENGINE'S RANGE, NOT THE STRING ATLAS'S. `0x4CCF60` bounds a character
   below (`sub ebx,first; jb` at 0x4CCFAA) and NOT above: it indexes the offset
   table with whatever byte the string carries. The string atlas can afford
   [0x20, 0x7E] because every marker string is an ASCII literal of ours; a UI
   string is the engine's and must be reproduced, so the cache runs to 0xFF and
   probes each table entry as it reaches it — the same unbounded index the
   engine makes, with a fault check the engine does not. */
#define GCH_HI     0xFF
#define GA_NCH     (GCH_HI - CH_LO + 1)

typedef struct {
    const unsigned char* font;             /* the object, and its signature:   */
    unsigned char sig[3];                  /* rows / yoff / first              */
    short cell[GA_NCH][4];                 /* ax, ay, w, h; w == 0 = not cached */
    unsigned char known[GA_NCH];           /* 1 = tried; 2 = tried and refused  */
} GFONT;

static GFONT s_gf[GA_FONTS];
static int   s_ngf;
static int   s_gshelfX, s_gshelfY, s_gshelfH;
static unsigned char s_gatlas[GA_W * GA_H];
static int   s_gdirty;
static GLuint s_gtex;
static unsigned s_gglyphs, s_gdrops;

/* Validate an arbitrary font object — the UI's, which arrives in a published op
   rather than from the snapshot font_ok() reads. Same probes, and it must stay
   that way: the table is indexed to CH_HI and a short block would fault. */
static const unsigned char* gfont_ok(const unsigned char* f)
{
    if (!ptr_ok(f)) return NULL;
    if (IsBadReadPtr((void*)f, F_TAB)) return NULL;
    if (f[F_ROWS] == 0 || f[F_ROWS] > GA_H) return NULL;
    /* the TABLE is probed per character below, not here: its length is not
       stated anywhere in the object, and demanding the full 0x100 - first
       entries would reject a short font outright rather than serve the codes
       it does have */
    return f;
}

/* The slot for this font, matched on the POINTER AND THE SIGNATURE — an
   allocator that hands the same address to a different font would otherwise
   serve the old font's glyphs. A full table drops the oldest slot's cells
   rather than refusing, because refusing would leave the UI text-less. */
static GFONT* gfont_slot(const unsigned char* f)
{
    int i;
    GFONT* g;
    for (i = 0; i < s_ngf; i++)
        if (s_gf[i].font == f && s_gf[i].sig[0] == f[F_ROWS] &&
            s_gf[i].sig[1] == f[F_YOFF] && s_gf[i].sig[2] == f[F_FIRST])
            return &s_gf[i];
    if (s_ngf < GA_FONTS) g = &s_gf[s_ngf++];
    else {
        /* every slot is taken: start the whole cache over rather than evict one
           font into a shelf another font's cells still occupy */
        memset(s_gf, 0, sizeof s_gf);
        memset(s_gatlas, 0, sizeof s_gatlas);
        s_gshelfX = s_gshelfY = s_gshelfH = 0;
        s_gdirty = 1;
        s_ngf = 1; g = &s_gf[0];
        flog("text: glyph atlas reset (more than 8 fonts seen)");
    }
    memset(g, 0, sizeof *g);
    g->font = f;
    g->sig[0] = f[F_ROWS]; g->sig[1] = f[F_YOFF]; g->sig[2] = f[F_FIRST];
    return g;
}

int tagpu_text_glyph(const void* font, int ch, int* ax, int* ay, int* w, int* h, int* yoff)
{
    const unsigned char* f = gfont_ok((const unsigned char*)font);
    const unsigned short* tab;
    GFONT* g;
    int idx, first, rows, off, gw, y;
    char one[2];

    if (!f || ch < CH_LO || ch > GCH_HI) return 0;
    first = f[F_FIRST];
    rows  = f[F_ROWS];
    if (ch < first) return 0;                        /* 0x4CCFAA skips it      */
    tab = (const unsigned short*)(f + F_TAB);
    idx = ch - CH_LO;
    g = gfont_slot(f);
    if (yoff) *yoff = (int)(signed char)f[F_YOFF];
    if (g->known[idx] == 2) return 0;
    if (g->known[idx] == 1) {
        *ax = g->cell[idx][0]; *ay = g->cell[idx][1];
        *w  = g->cell[idx][2]; *h  = g->cell[idx][3];
        return 1;
    }
    /* the table entry itself, probed at the index the engine would use */
    if (IsBadReadPtr((void*)(tab + (ch - first)), 2)) { g->known[idx] = 2; return 0; }
    off = tab[ch - first];
    if (!off) { g->known[idx] = 2; return 0; }       /* 0x4CCFB9 skips it      */
    if (IsBadReadPtr((void*)(f + off), 1)) { g->known[idx] = 2; return 0; }
    gw = f[off];
    /* A ZERO WIDTH WRITES 256 COLUMNS in the engine (the do-while at 0x4CCFCA /
       0x4CCFE9 wraps cl to 255), so it is refused here exactly as `measure`
       refuses it. That is a deliberate DIVERGENCE from the engine on a corrupt
       font: the engine would smear 256 columns of ink and we draw nothing. No
       stock font has one. */
    if (gw <= 0) { g->known[idx] = 2; return 0; }
    if (IsBadReadPtr((void*)(f + off + 1), (UINT_PTR)((rows * gw + 7) / 8))) {
        g->known[idx] = 2; return 0;
    }
    if (gw > GA_W || rows > GA_H) { g->known[idx] = 2; s_gdrops++; return 0; }
    if (s_gshelfX + gw > GA_W) { s_gshelfY += s_gshelfH; s_gshelfX = 0; s_gshelfH = 0; }
    if (s_gshelfY + rows > GA_H) {
        /* out of shelf: start over. Every cached cell goes with it, which costs
           one re-rasterise of what is on screen and never a wrong glyph */
        memset(s_gf, 0, sizeof s_gf);
        memset(s_gatlas, 0, sizeof s_gatlas);
        s_gshelfX = s_gshelfY = s_gshelfH = 0;
        s_ngf = 0; s_gdirty = 1; s_gdrops++;
        flog("text: glyph atlas full — reset");
        g = gfont_slot(f);
    }
    /* clear first: the blitter stores nothing for a clear bit, so a dirty slot
       would keep the previous glyph's ink */
    for (y = 0; y < rows; y++)
        memset(s_gatlas + (size_t)(s_gshelfY + y) * GA_W + s_gshelfX, 0, (size_t)gw);
    one[0] = (char)ch; one[1] = 0;
    /* fg = INK, bg = 0, transparent = 0: the store keeps only the set bits, so
       what lands is a COVERAGE MASK and one raster serves every colour */
    ((PFN_BLIT)BLIT_VA)(s_gatlas + (size_t)s_gshelfY * GA_W + s_gshelfX, GA_W,
                        f, one, 0, (int)(signed char)f[F_YOFF], INK, 0, 0);
    g->cell[idx][0] = (short)s_gshelfX; g->cell[idx][1] = (short)s_gshelfY;
    g->cell[idx][2] = (short)gw;        g->cell[idx][3] = (short)rows;
    g->known[idx] = 1;
    *ax = s_gshelfX; *ay = s_gshelfY; *w = gw; *h = rows;
    s_gshelfX += gw;
    if (rows > s_gshelfH) s_gshelfH = rows;
    s_gdirty = 1;
    s_gglyphs++;
    return 1;
}

void tagpu_text_glyph_dims(int* w, int* h) { *w = GA_W; *h = GA_H; }

unsigned int tagpu_text_glyph_tex(void)
{
    if (!s_gglyphs) return 0;
    if (!s_gtex) {
        glGenTextures(1, &s_gtex);
        if (!s_gtex) return 0;
        glBindTexture(GL_TEXTURE_2D, s_gtex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_gdirty = 1;
    } else {
        glBindTexture(GL_TEXTURE_2D, s_gtex);
    }
    if (s_gdirty) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, GA_W, GA_H, 0,
                     GL_RED, GL_UNSIGNED_BYTE, s_gatlas);
        s_gdirty = 0;
    }
    return s_gtex;
}

int tagpu_text_glyph_stats(unsigned* glyphs, unsigned* drops, int* fonts)
{
    if (glyphs) *glyphs = s_gglyphs;
    if (drops)  *drops  = s_gdrops;
    if (fonts)  *fonts  = s_ngf;
    return (int)s_gglyphs;
}


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
    s_gtex = 0;           /* ...and the glyph atlas's; the CELLS survive, they are CPU-side */
    s_gdirty = 1;
}

int tagpu_text_stats(int* strings, int* dropped)
{
    if (strings) *strings = s_nent;
    if (dropped) *dropped = s_ndrop;
    return s_nent;
}
