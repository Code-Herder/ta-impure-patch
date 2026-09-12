/* tagpu_text.c — TA's own glyphs, rasterised into an atlas of ours (G13p).
   See tagpu_text.h for why the engine's blitter can be called with our
   destination and why the font travels as BYTES in the frame packet rather
   than as a pointer.

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
   character, so the two cannot disagree about how much lands.

   THE COPY (frame packet exchange, landing 1). tagpu_packet_pub.c builds, on
   the game thread at hook 8, one such object PER GLYPH — `[rows][0][yoff]
   [code][u16 6][w][bits]`, a font whose only character is the one being drawn
   — and every packet carries the 95 of them. tagpu_text_frame() copies the
   area out of the packet once per font generation; the measure walks the
   glyph table's width bytes; the raster hands each one-glyph object to the
   blitter at the x its own string loop would have reached (it advances by
   the width byte and nothing else, `0x4CCFF7..0x4CCFFD`), so the pixels are
   the engine's. No IsBadReadPtr anywhere on this path: the bytes are ours,
   bounded when they were copied and re-bounded by tagpu_packet.c at acquire.
   `0x4CCF60..0x4CD00E` is pure — reads its arguments, writes its destination,
   no global, no allocation — which is the whole argument for running engine
   code on the present thread, and why this file is the allow-list's one
   "pure engine code" entry. The GLYPH cache below (the GL UI's string op)
   still dereferences an engine font behind probes: that is landing 4c. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "opengl_utils.h"
#include "tagpu_text.h"

#define BLIT_VA         0x004CCF60u   /* pure engine code, 0x4CCF60..0x4CD00E */

#define F_ROWS          0
#define F_YOFF          2
#define F_FIRST         3
#define F_TAB           4

/* The value a covered pixel gets. Full white, so the sampler sees 1.0. */
#define INK             255

/* The glyphs we will ever ask for: printable ASCII — the packet's range. */
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


static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* THE FRAME'S FONT: our copy of the packet's font area, keyed on the packet's
   font generation, and the colour the block draws in. Present thread only;
   nothing here points into a packet once tagpu_text_frame has returned. */
static unsigned       s_copyGen;              /* the generation the copy holds; 0 = none */
static unsigned char  s_fontRows, s_fontFirst;
static signed char    s_fontYoff;
static TAGPU_PK_GLYPH s_glyph[TAGPU_PK_NGLYPH];
static unsigned char  s_fontArea[TAGPU_PK_FONT_MAX];
static unsigned       s_fontLen;              /* 0 = no font: nothing draws */
static int            s_fg = -1;

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
static unsigned s_builtGen;                /* the font generation the atlas holds */
static unsigned s_fontGen;                 /* bumped whenever the copy changes */
static unsigned char s_atlas[ATLAS_W * ATLAS_H];
static int   s_dirty;
static GLuint s_tex;

void tagpu_text_frame(const TAGPU_PACKET* pk)
{
    if (!pk) return;                          /* no packet this frame: keep what we have */
    if (pk->in_game) s_fg = pk->text_fg;
    /* font_gen 0 carries no font (the out-of-game packet, or none copied yet):
       the copy stays, so a readout in the shell keeps the last game's font */
    if (!pk->font_gen || pk->font_gen == s_copyGen) return;
    s_copyGen = pk->font_gen;
    s_fontLen = 0;
    memset(s_glyph, 0, sizeof s_glyph);
    /* the packet's bounds held at acquire (tagpu_packet.c pk_valid): the area
       is inside used_bytes and every glyph inside the area. Ours once more,
       because a copy is cheap and a wrong length here is a buffer overrun. */
    if (pk->font_len && pk->font_len <= TAGPU_PK_FONT_MAX && pk->font_off >= sizeof(TAGPU_PACKET) &&
        pk->font_off + pk->font_len <= pk->used_bytes) {
        memcpy(s_fontArea, (const unsigned char*)pk + pk->font_off, pk->font_len);
        memcpy(s_glyph, pk->font_glyph, sizeof s_glyph);
        s_fontLen   = pk->font_len;
        s_fontRows  = pk->font_rows;
        s_fontYoff  = pk->font_yoff;
        s_fontFirst = pk->font_first;
    }
    s_fontGen++;                              /* a different font: the atlas resets at the next place */
}

int tagpu_text_colour(void) { return s_fg; }

/* The one-glyph font object for `c`, or NULL when the font has no such glyph
   (the blitter would skip it: `0x4CCFAA` below `first`, `0x4CCFB9` a zero
   table entry) or it did not fit the copy. `w` is its width byte. */
static const unsigned char* glyph_obj(unsigned c, unsigned* w)
{
    const TAGPU_PK_GLYPH* g;
    unsigned len;
    if (c < CH_LO || c > CH_HI || !s_fontLen) return NULL;
    g = &s_glyph[c - CH_LO];
    if (!g->w) return NULL;
    len = TAGPU_PK_GLYPH_HDR + (((unsigned)s_fontRows * g->w + 7u) >> 3);
    if ((unsigned)g->off > s_fontLen || len > s_fontLen - g->off) return NULL;
    *w = g->w;
    return s_fontArea + g->off;
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

   So the filter is applied to the STRING: `out` is the subsequence the raster
   will draw, and `total` is its width. Rasterise `out`, not `s`, and the two
   cannot disagree. `0x4C1527`, character for character otherwise: a NUL or a
   newline ends it, and a skipped code does not advance the cursor. A zero
   width never reaches here — the copy refused it, because the blit's per-row
   counter is a do-while (`mov ch,cl` at `0x4CCFCA`, `dec ch; je` at `0x4CCFE9`)
   and `cl == 0` would write 256 columns. [BINARY-VERIFIED] */
static int measure(const char* s, char* out, size_t outsz, int* w, int* h)
{
    int total = 0;
    size_t n = 0;
    if (!s_fontLen) return 0;
    for (; *s && n + 1 < outsz; s++) {
        unsigned c = (unsigned char)*s, gw;
        if (c == '\n') break;
        if (!glyph_obj(c, &gw)) continue;
        total += (int)gw;
        out[n++] = (char)c;
    }
    out[n] = 0;
    *w = total;
    *h = s_fontRows;
    return total > 0;
}

/* The raster: the engine's own string loop, one glyph per call. Its loop puts
   glyph i at rowstart + sum(widths before i) and restarts the bit counter per
   glyph, so a call per glyph at that x, with y = yoff so that `y - font[2]`
   lands on our row 0, writes byte for byte what one call over the whole
   string would. `draw` is measure()'s subsequence: every code in it has an
   object. */
static void raster(unsigned char* dst, int pitch, const char* draw)
{
    int x = 0;
    for (; *draw; draw++) {
        unsigned gw;
        const unsigned char* obj = glyph_obj((unsigned char)*draw, &gw);
        char one[2];
        if (!obj) continue;
        one[0] = *draw; one[1] = 0;
        ((PFN_BLIT)BLIT_VA)(dst + x, pitch, obj, one, 0, (int)s_fontYoff, INK, 0, 0);
        x += (int)gw;
    }
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
    char draw[STRMAX];
    int i, sw, sh, y;

    if (!s_fontLen || !s || !*s) return 0;
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
    *yoff = (int)s_fontYoff;

    for (i = 0; i < s_nent; i++) {
        if (!strcmp(s_ent[i].s, s)) {
            *ax = s_ent[i].ax; *ay = s_ent[i].ay;
            *w  = s_ent[i].w;  *h  = s_ent[i].h;
            return 1;
        }
    }
    if (s_nent >= MAXSTR || strlen(s) >= STRMAX) { drop(s); return 0; }
    if (!measure(s, draw, sizeof draw, &sw, &sh)) return 0;
    if (sw > ATLAS_W || sh > ATLAS_H) { drop(s); return 0; }

    if (s_shelfX + sw > ATLAS_W) { s_shelfY += s_shelfH; s_shelfX = 0; s_shelfH = 0; }
    if (s_shelfY + sh > ATLAS_H) { drop(s); return 0; }

    /* clear first: the blitter stores nothing for a clear bit (bg == the
       transparent index), so an unclear slot would keep the last string's ink */
    for (y = 0; y < sh; y++)
        memset(s_atlas + (size_t)(s_shelfY + y) * ATLAS_W + s_shelfX, 0, (size_t)sw);
    raster(s_atlas + (size_t)s_shelfY * ATLAS_W + s_shelfX, ATLAS_W, draw);

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

/* KEYED ON AN ID, NEVER ON A FONT ADDRESS (landing 4c). The string op used to
   carry the engine's font object and this cache keyed on the pointer and its
   signature — which meant dereferencing that pointer here, on the present
   thread, up to a queue backlog after the observer saw it, behind probes, with
   no note establishing a UI font's lifetime. The producer assigns a number per
   (font, signature) now and sends the GLYPH BITS with the first string that
   needs each code; an id is never reused, so a recycled font address cannot
   serve the old font's cells. Nothing in this file dereferences a font. */
typedef struct {
    unsigned id;                           /* 0 = the slot is free             */
    unsigned char rows;
    signed char   yoff;
    short cell[GA_NCH][4];                 /* ax, ay, w, h; w == 0 = not cached */
    unsigned char known[GA_NCH];           /* 1 = cached; 2 = refused           */
} GFONT;

static GFONT s_gf[GA_FONTS];
static int   s_ngf;
static int   s_gshelfX, s_gshelfY, s_gshelfH;
static unsigned char s_gatlas[GA_W * GA_H];
static int   s_gdirty;
static GLuint s_gtex;
static unsigned s_gglyphs, s_gdrops;
/* Bumped whenever the shelves restart, i.e. whenever every cell handed out so
   far stops being valid. A caller that gathers a run of cells before it draws
   them has to check this across the gather. */
static unsigned s_ggen;

/* The slot for this font id, or NULL when the table is full and had to start
   over (a full table drops every cell rather than evict one font into a shelf
   another font's cells still occupy). */
static GFONT* gfont_slot(unsigned id)
{
    int i;
    GFONT* g;
    for (i = 0; i < s_ngf; i++) if (s_gf[i].id == id) return &s_gf[i];
    if (s_ngf < GA_FONTS) g = &s_gf[s_ngf++];
    else {
        memset(s_gf, 0, sizeof s_gf);
        memset(s_gatlas, 0, sizeof s_gatlas);
        s_gshelfX = s_gshelfY = s_gshelfH = 0;
        s_gdirty = 1;
        s_ngf = 1; g = &s_gf[0];
        s_ggen++;
        flog("text: glyph atlas reset (more than 8 fonts seen)");
    }
    memset(g, 0, sizeof *g);
    g->id = id;
    return g;
}

/* One glyph into the atlas, from the BITS the producer sent. The blitter is
   handed a one-glyph font object of OURS — `[rows][0][yoff][code][u16 6][w]
   [bits]`, the same four header fields it reads and the same packed rows,
   exactly as the marker path's copy is built above — so TA's own glyphs are
   still stamped by TA's own blitter and nothing engine-side is dereferenced. */
static void glyph_raster(GFONT* g, int ch, int gw, const unsigned char* bits, unsigned nb)
{
    int idx = ch - CH_LO, y;
    /* STATIC, not a local: the widest cell this atlas admits makes the object
       16 KB, and this runs on the render thread. One writer, one call at a
       time, no state kept between calls. */
    static unsigned char obj[7 + (GA_W * GA_H + 7) / 8];
    unsigned rows = g->rows;
    if (idx < 0 || idx >= GA_NCH || g->known[idx]) return;
    if (gw <= 0 || gw > GA_W || rows == 0 || rows > GA_H) { g->known[idx] = 2; s_gdrops++; return; }
    if (nb != ((rows * (unsigned)gw) + 7u) / 8u) { g->known[idx] = 2; s_gdrops++; return; }
    if (s_gshelfX + gw > GA_W) { s_gshelfY += s_gshelfH; s_gshelfX = 0; s_gshelfH = 0; }
    if (s_gshelfY + (int)rows > GA_H) {
        /* out of shelf: start over. Every cached cell goes with it, which costs
           one re-rasterise of what is on screen and never a wrong glyph. EVERY
           CELL HANDED OUT BEFORE THIS POINT NOW NAMES CLEARED TEXELS, and a
           caller gathering a whole string is holding a fistful of them — the
           generation below is how it finds out. */
        unsigned keep = g->id; unsigned char kr = g->rows; signed char ky = g->yoff;
        memset(s_gf, 0, sizeof s_gf);
        memset(s_gatlas, 0, sizeof s_gatlas);
        s_gshelfX = s_gshelfY = s_gshelfH = 0;
        s_ngf = 1; s_gdirty = 1; s_gdrops++; s_ggen++;
        g = &s_gf[0]; g->id = keep; g->rows = kr; g->yoff = ky;
        flog("text: glyph atlas full — reset");
        if (s_gshelfY + (int)rows > GA_H) { g->known[idx] = 2; return; }
    }
    for (y = 0; y < (int)rows; y++)
        memset(s_gatlas + (size_t)(s_gshelfY + y) * GA_W + s_gshelfX, 0, (size_t)gw);
    obj[0] = (unsigned char)rows;
    obj[1] = 0;
    obj[2] = (unsigned char)g->yoff;
    obj[3] = (unsigned char)ch;
    obj[4] = 6; obj[5] = 0;
    obj[6] = (unsigned char)gw;
    memcpy(obj + 7, bits, nb);
    {
        char one[2];
        one[0] = (char)ch; one[1] = 0;
        /* fg = INK, bg = 0, transparent = 0: the store keeps only the set bits,
           so what lands is a COVERAGE MASK and one raster serves every colour */
        ((PFN_BLIT)BLIT_VA)(s_gatlas + (size_t)s_gshelfY * GA_W + s_gshelfX, GA_W,
                            obj, one, 0, (int)g->yoff, INK, 0, 0);
    }
    g->cell[idx][0] = (short)s_gshelfX; g->cell[idx][1] = (short)s_gshelfY;
    g->cell[idx][2] = (short)gw;        g->cell[idx][3] = (short)rows;
    g->known[idx] = 1;
    s_gshelfX += gw;
    if ((int)rows > s_gshelfH) s_gshelfH = (int)rows;
    s_gdirty = 1;
    s_gglyphs++;
}

/* Where the STRING starts inside a string op's arena block: past `n` glyph
   records, each 4 + its padded bit count. Bounded by `len`, so a block the
   producer truncated (the arena filled) leaves the string at the end and the
   caller's own NUL test does the rest. */
unsigned tagpu_text_glyph_block_bytes(const unsigned char* block, unsigned n, unsigned len)
{
    unsigned at = 0, k;
    for (k = 0; k < n; k++) {
        unsigned nb, pad;
        if (at + 4u > len) return len;
        nb = (unsigned)block[at + 2] | ((unsigned)block[at + 3] << 8);
        pad = (nb + 3u) / 4u * 4u;
        if (at + 4u + pad > len) return len;
        at += 4u + pad;
    }
    return at;
}

void tagpu_text_glyph_feed(unsigned font_id, int rows, int yoff,
                           const unsigned char* block, unsigned n, unsigned len)
{
    GFONT* g;
    unsigned at = 0, k;
    if (!font_id || rows <= 0 || rows > GA_H) return;
    g = gfont_slot(font_id);
    g->rows = (unsigned char)rows;
    g->yoff = (signed char)yoff;
    for (k = 0; k < n; k++) {
        int ch, gw;
        unsigned nb, pad;
        if (at + 4u > len) return;                 /* the block ended early     */
        ch = block[at]; gw = block[at + 1];
        nb = (unsigned)block[at + 2] | ((unsigned)block[at + 3] << 8);
        pad = (nb + 3u) / 4u * 4u;
        if (at + 4u + pad > len) return;
        glyph_raster(g, ch, gw, block + at + 4, nb);
        /* the slot may have moved if the raster reset the table */
        g = gfont_slot(font_id);
        g->rows = (unsigned char)rows; g->yoff = (signed char)yoff;
        at += 4u + pad;
    }
}

int tagpu_text_glyph_id(unsigned font_id, int ch, int* ax, int* ay,
                        int* w, int* h, int* yoff)
{
    GFONT* g;
    int idx, i;
    if (!font_id || ch < CH_LO || ch > GCH_HI) return 0;
    for (i = 0, g = NULL; i < s_ngf; i++) if (s_gf[i].id == font_id) { g = &s_gf[i]; break; }
    if (!g) return 0;
    if (yoff) *yoff = (int)g->yoff;
    idx = ch - CH_LO;
    if (g->known[idx] != 1) return 0;
    *ax = g->cell[idx][0]; *ay = g->cell[idx][1];
    *w  = g->cell[idx][2]; *h  = g->cell[idx][3];
    return 1;
}

void tagpu_text_glyph_dims(int* w, int* h) { *w = GA_W; *h = GA_H; }
unsigned tagpu_text_glyph_gen(void) { return s_ggen; }

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
