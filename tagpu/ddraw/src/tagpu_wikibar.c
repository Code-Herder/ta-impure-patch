/* tagpu_wikibar.c -- the game frame's two bars in the mod's own art (tagpu_wikibar.h). A port of
   tools/wiki/mockup/bar.js, step for step and in its order; tools/wiki/barcheck.mjs holds the two
   equal byte for byte. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_wikibar.h"
#include "tagpu_wikiasset.h"
#include "tagpu_wikistage.h"

typedef char wikibar_w_is_the_widest_frame[WIKIBAR_W == 16 * WIKISTAGE_MAP ? 1 : -1];

#define ROWS        32          /* each bar's rows                                               */
#define COL         128         /* the frame column the picture's column 0 is                    */
#define TILE_X      0x81        /* 129: the first tile's frame column, 0x469050 and 0x46A860     */
#define GUI_INCOME  0xA         /* the GUI colour table entries the bar inks with                */
#define GUI_EXPENSE 0xC
#define GUI_TEXT    0xF

/* 0x4E43A0, fistp under RC=11: truncation toward zero; a value outside int32 (or NaN) gives the
   integer indefinite, -2^31, as the x87 does */
static int32_t ftol(double v)
{
    const double t = trunc(v);
    return t >= -2147483648.0 && t <= 2147483647.0 ? (int32_t)t : INT32_MIN;
}

/* `cdq; xor; sub`: the int32 absolute value, whose -2^31 stays -2^31 */
static int32_t iabs(int32_t v) { return v == INT32_MIN ? v : v < 0 ? -v : v; }

/* "%.1f", as the explicit rule both ports share: r = floor(|v| x 10 + 0.5) in doubles, then
   r / 10 "." r % 10, with a minus sign when v < 0 and r > 0. The engine CRT's rounding of an exact
   tie (2.25) is [INFERRED] to be the same; a live capture with non-zero metal rates is the check.
   The two ports print the same digits while r is below 2^53 (v finite and |v| below about 9e14),
   past which JavaScript prints a double's shortest digits and C its exact ones: the bound the
   header states for the metal rates. */
static void fixed1(double v, char* out, size_t n)
{
    const double r = floor(fabs(v) * 10 + 0.5);
    snprintf(out, n, "%s%.0f.%.0f", v < 0 && r > 0 ? "-" : "", floor(r / 10), fmod(r, 10));
}

typedef struct {
    const struct WIKISIDE* sd;
    const WIKIBARART*      B;
    uint8_t*               idx;
    int                    w, Wg, Hg;
    const unsigned char*   font;
    uint32_t               flen;
    int                    rows, yoff;
} BAR;

/* a frame of the sheet inside it: the bound every blit below rests on (the loader checks the same
   before it publishes the block, tagpu_wikiasset.c side_frames_inside) */
static int frame_ok(const WIKISIDE* sd, const WIKISIDEFRAME* f)
{
    return f->sx >= 0 && f->sy >= 0 && f->w >= 1 && f->h >= 1 && f->sx <= sd->sheet_w - f->w && f->sy <= sd->sheet_h - f->h;
}

/* code c's glyph offset in the font, 0 for none: the table starts at byte 4, the first code is 0.
   A glyph whose bits would run past the font is none; the loader refuses such a font whole, so
   this changes nothing for a published block (fnt_ok). */
static uint32_t glyph_at(const BAR* b, unsigned char c)
{
    const uint32_t o = (uint32_t)(b->font[4 + 2 * c] | b->font[5 + 2 * c] << 8);
    if (!o || o >= b->flen || o + 1 + ((uint32_t)b->font[o] * (uint32_t)b->rows + 7) / 8 > b->flen) return 0;
    return o;
}

/* 0x4C1480: the string's width, its glyphs' widths summed up to a newline */
static long text_width(const BAR* b, const char* s)
{
    long w = 0;
    for (; *s && *s != '\n'; s++) {
        const uint32_t o = glyph_at(b, (unsigned char)*s);
        if (o) w += b->font[o];
    }
    return w;
}

/* a frame {f, key} with its top left at picture (dx, dy) whatever its hotspot, its key index
   skipped, clipped to the columns and to rows [y0, y1) */
static void blit(const BAR* b, const WIKISIDEFRAME* f, int key, int dx, int dy, int y0, int y1)
{
    const unsigned char* S = b->sd->sheet;
    int r, c;
    for (r = y0 - dy > 0 ? y0 - dy : 0; r < f->h && dy + r < y1; r++) {
        const unsigned char* src = S + (size_t)(f->sy + r) * (size_t)b->sd->sheet_w + (size_t)f->sx;
        uint8_t* dst = b->idx + (size_t)(dy + r) * WIKIBAR_W;
        for (c = dx < 0 ? -dx : 0; c < f->w && dx + c < b->w; c++)
            if (src[c] != key) dst[dx + c] = src[c];
    }
}

/* a frame pixel of the top bar, clipped to the picture (frame column fx, row fy) */
static void put(const BAR* b, long long fx, long long fy, uint8_t p)
{
    const long long x = fx - COL;
    if (x >= 0 && x < b->w && fy >= 0 && fy < ROWS) b->idx[fy * WIKIBAR_W + x] = p;
}

/* DrawBar 0x4BF6F0 -> 0x4CCDEA: inclusive of both ends, after the clip to the surface 0x4BF620 */
static void fill(const BAR* b, long long l, long long t, long long r, long long bt, uint8_t col)
{
    long long x, y;
    const long long y0 = t > 0 ? t : 0, y1 = bt < ROWS - 1 ? bt : ROWS - 1;
    const long long x0 = l > COL ? l : COL, x1 = r < b->Wg - 1 ? r : b->Wg - 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) b->idx[y * WIKIBAR_W + x - COL] = col;
}

/* A FILL AND ITS TICK, both skipped at storage 0 (0x4690BD, 0x4690ED): x1..ftol(x1 + (x2 - x1) v /
   max); the tick three columns T..T+2 in GUI[0xC], only when the level is not 0 and the raw stored
   value is above it */
static void bar(const BAR* b, int rect, double v, double max, uint8_t col, double raw, double level)
{
    const WIKIBARRECT* R = &b->B->rect[rect];
    const int l = R->x1, t = R->y1, r = R->x2, bt = R->y2;
    if (max == 0) return;
    fill(b, l, t, ftol(l + (double)(r - l) * v / max), bt, col);
    if (level != 0 && raw > level) {
        const int32_t T = ftol(l + (double)(r - l) * level / max);
        fill(b, T, t, (long long)T + 2, bt, b->sd->gui[GUI_EXPENSE]);
    }
}

/* DrawTextCustomFont 0x4C14F0, one ink and no background (0x4CCF60 skips the clear bits). The
   string is REFUSED WHOLE unless its box {x, y, x + width, y + rows} lies inside the surface's clip
   {0, 0, Wg - 1, Hg - 1} (0x4C1629..0x4C169E, the test 0x4B6750, the clip 0x4C69C0 set at
   0x468E3A; DISASSEMBLED). Drawn, each glyph's top row is y - (int8)font[2], its bits MSB-first
   across rows, x advanced by its width; a code with no glyph draws nothing. What lands outside the
   top bar is clipped. */
static void text(const BAR* b, const char* s, long long x, long long y, uint8_t col)
{
    const long long tw = text_width(b, s);
    if (x < 0 || x > b->Wg - 1 || x + tw < 0 || x + tw > b->Wg - 1 || y < 0 || y > b->Hg - 1 || y + b->rows < 0 ||
        y + b->rows > b->Hg - 1)
        return;
    for (; *s; s++) {
        const uint32_t o = glyph_at(b, (unsigned char)*s);
        int r, c, k, gw;
        if (!o) continue;
        gw = b->font[o];
        for (r = 0, k = 0; r < b->rows; r++)
            for (c = 0; c < gw; c++, k++)
                if (b->font[o + 1 + (k >> 3)] & (0x80 >> (k & 7))) put(b, x + c, y - b->yoff + r, col);
        x += gw;
    }
}

static void at(const BAR* b, const char* s, int rect, uint8_t col)
{
    text(b, s, b->B->rect[rect].x1, b->B->rect[rect].y1, col);
}

static void right(const BAR* b, const char* s, int rect, uint8_t col)
{
    text(b, s, b->B->rect[rect].x1 - text_width(b, s), b->B->rect[rect].y1, col);
}

/* income past 99999 and expense below -99999 take "%dK" of trunc(ftol(v) / 1000), C's division
   truncating as Math.trunc does */
static void kilo(double v, char* out, size_t n) { snprintf(out, n, "%ldK", (long)(ftol(v) / 1000)); }

int tagpu_wikibar_compose(const WIKISIDE* sd, int side, int colour, int Wg, int Hg, const WIKIBARNUM* n, uint8_t* idx)
{
    BAR b;
    const WIKIBARART* B;
    const uint8_t* gui;
    char s[64];
    int x, first, r;
    memset(idx, 0, (size_t)WIKIBAR_W * WIKIBAR_H);
    if (!sd || !n || side < 0 || side >= sd->layout.nside || side >= WIKI_SIDE_SIDES || !sd->has_bar[side] ||
        !sd->has_gui || Wg - COL < 1 || Wg - COL > WIKIBAR_W || Hg < 2 * ROWS)
        return -1;
    B = &sd->bar[side];
    gui = sd->gui;
    /* the bounds the reads below rest on, which the loader holds for a published block */
    if (!frame_ok(sd, &B->top) || !frame_ok(sd, &B->bot) || B->font_off > WIKI_FNT_BYTES ||
        B->font_len > WIKI_FNT_BYTES - B->font_off || B->font_len < 4 + 2 * 256)
        return -1;
    b.sd = sd;
    b.B = B;
    b.idx = idx;
    b.w = Wg - COL;
    b.Wg = Wg;
    b.Hg = Hg;
    b.font = sd->fnt + B->font_off;
    b.flen = B->font_len;
    b.rows = b.font[0];
    b.yoff = (int8_t)b.font[2];

    /* COLUMN 0 is the frame's column 128: PANELSIDE's column 128 where PANELSIDE has it (it lands
       with its top left at the frame's corner, tagpu_wikiside.c), else the engine's clear, index 0.
       On the bottom bar's rows it is PANELSIDE's row Hg - 32 + r, below the panel's picture the
       clear. */
    if (sd->has_panelside[side] && frame_ok(sd, &sd->panelside[side]) && sd->panelside[side].w > COL) {
        const WIKISIDEFRAME* ps = &sd->panelside[side];
        for (r = 0; r < ROWS; r++) {
            const int rows[2] = { r, Hg - ROWS + r };
            int k;
            for (k = 0; k < 2; k++)
                if (rows[k] < ps->h) {
                    const uint8_t p = sd->sheet[(size_t)(ps->sy + rows[k]) * (size_t)sd->sheet_w + (size_t)(ps->sx + COL)];
                    idx[(size_t)(k * ROWS + r) * WIKIBAR_W] = p == sd->panelside_key[side] ? 0 : p;
                }
        }
    }

    /* THE TILES, 0x469050..0x46909D: from frame column 129 while x < Wg, PANELTOP[side] for the
       first tile and PANELBOT[side] for every later one (the index s + 5 x (x > 0x81)), each frame 0
       at its top left, x advanced by its width (at least 1: frame_ok). PANELBOT is 33 rows; its
       last lands on the viewport's first row, outside this picture. */
    for (x = TILE_X, first = 1; x < Wg; first = 0) {
        const WIKISIDEFRAME* f = first ? &B->top : &B->bot;
        blit(&b, f, first ? B->top_key : B->bot_key, x - COL, 0, 0, ROWS);
        x += f->w;
    }

    /* THE BADGE, 0x467C00 through GAF_DrawTransformed 0x4C7580: frame `colour` of the logos
       sequence into the LOGO rect, half-open, each step the 16.16 accumulator (fw << 16) / dw.
       THE ENGINE HOLDS NO BOUND on the colour (record 10's colour 10 crashed at 0x4C74C7,
       research/notes/engine/recorder.md); this one is held below the sequence's frame count, and
       past it no badge is drawn. The source row r x sv >> 16 stays below fh for every r < dh, as
       sv <= fh x 65536 / dh, and the column likewise. Only the rect's part inside the picture is
       walked, which writes what bar.js's walk of the whole rect writes. */
    if (colour >= 0 && colour < sd->nlogo && colour < WIKI_LOGOS && frame_ok(sd, &sd->logo[colour])) {
        const WIKISIDEFRAME* f = &sd->logo[colour];
        const WIKIBARRECT* R = &B->rect[WB_LOGO];
        const long long dw = R->x2 - R->x1, dh = R->y2 - R->y1;
        if (dw > 0 && dh > 0) {
            const long long su = (long long)f->w * 65536 / dw, sv = (long long)f->h * 65536 / dh;
            const long long r0 = R->y1 < 0 ? -R->y1 : 0, r1 = ROWS - R->y1 < dh ? ROWS - R->y1 : dh;
            const long long c0 = COL - R->x1 > 0 ? COL - R->x1 : 0, c1 = Wg - R->x1 < dw ? Wg - R->x1 : dw;
            long long rr, cc;
            for (rr = r0; rr < r1; rr++)
                for (cc = c0; cc < c1; cc++) {
                    const uint8_t p = sd->sheet[(size_t)(f->sy + rr * sv / 65536) * (size_t)sd->sheet_w +
                                                (size_t)(f->sx + cc * su / 65536)];
                    if (p != sd->logo_key[colour]) put(&b, R->x1 + cc, R->y1 + rr, p);
                }
        }
    }

    /* the energy fill and tick, then its five strings (0x4690B0..0x46938C) */
    bar(&b, WB_ENERGYBAR, n->e, n->emax, B->ecol, n->raw_e, n->lev_e);
    snprintf(s, sizeof s, "%ld", (long)ftol(n->e));
    at(&b, s, WB_ENERGYNUM, gui[GUI_TEXT]);
    at(&b, "0", WB_ENERGY0, gui[GUI_TEXT]);
    snprintf(s, sizeof s, "%ld", (long)ftol(n->emax));
    right(&b, s, WB_ENERGYMAX, gui[GUI_TEXT]);
    /* the expense's test is the income's mirrored, so A POSITIVE EXPENSE NEVER TAKES THE K: the
       engine's quirk, copied. The expense's compare is written so a NaN takes the K, as fcomp's
       unordered C0 does (0x469311). */
    if (n->ei > 99999) kilo(n->ei, s, sizeof s); else snprintf(s, sizeof s, "%ld", (long)ftol(n->ei));
    at(&b, s, WB_ENERGYPRODUCED, gui[GUI_INCOME]);
    if (!(n->eo >= -99999)) kilo(n->eo, s, sizeof s); else snprintf(s, sizeof s, "%ld", (long)iabs(ftol(n->eo)));
    at(&b, s, WB_ENERGYCONSUMED, gui[GUI_EXPENSE]);
    /* the metal fill and tick, then its strings, the rates "%.1f" (0x469391..0x46960B) */
    bar(&b, WB_METALBAR, n->m, n->mmax, B->mcol, n->raw_m, n->lev_m);
    snprintf(s, sizeof s, "%ld", (long)ftol(n->m));
    at(&b, s, WB_METALNUM, gui[GUI_TEXT]);
    at(&b, "0", WB_METAL0, gui[GUI_TEXT]);
    snprintf(s, sizeof s, "%ld", (long)ftol(n->mmax));
    right(&b, s, WB_METALMAX, gui[GUI_TEXT]);
    fixed1(n->mi, s, sizeof s);
    at(&b, s, WB_METALPRODUCED, gui[GUI_INCOME]);
    fixed1(fabs(n->mo), s, sizeof s);
    at(&b, s, WB_METALCONSUMED, gui[GUI_EXPENSE]);

    /* THE BOTTOM BAR, 0x46A860 (0x46A8A6..0x46A8D5): PANELBOT[side] tiled from frame column 129
       along the frame's last 32 rows, and nothing else on it while nothing is selected. */
    for (x = TILE_X; x < Wg; x += B->bot.w) blit(&b, &B->bot, B->bot_key, x - COL, ROWS, ROWS, WIKIBAR_H);

    return b.w;
}

void tagpu_wikibar_rgba(const WIKISIDE* sd, const uint8_t* idx, int w, uint8_t* rgba)
{
    int x, y;
    memset(rgba, 0, (size_t)WIKIBAR_W * WIKIBAR_H * 4);
    if (w > WIKIBAR_W) w = WIKIBAR_W;
    for (y = 0; y < WIKIBAR_H; y++)
        for (x = 0; x < w; x++) {
            const size_t i = (size_t)y * WIKIBAR_W + (size_t)x;
            const unsigned char* p = sd->pal + 4 * idx[i];
            rgba[4 * i] = p[0];
            rgba[4 * i + 1] = p[1];
            rgba[4 * i + 2] = p[2];
            rgba[4 * i + 3] = 255;
        }
}
