/* The fog expansion (tagpu/ddraw/inc/tagpu_fogsrc.h), compiled as the DLL compiles it and run
   natively by fog_expand_test.py.

   WHAT IT HOLDS TO WHAT. `tagpu_fog_entry` -- the C mirror of the shaders' taFogEntry, and the
   expansion tagpu_fog_at answers from -- against the window build `tagpu_fog_build_window`, the
   replication of the engine's builder 0x4843C0 that the oracle in tagpu_fogwide.c holds to the
   engine's own grid, plus the edge fill below (every entry past the one that straddles an edge
   repeats it). On every entry the window build defines, the two must be equal: so the expansion
   is the builder, wherever the window lies.

   WHICH ENTRIES THE WINDOW BUILD DEFINES. An entry on the map side of every edge is defined when
   it is not in the window's last column or row (those are short their right and bottom corners:
   the cells that would set them are outside the window). An entry past an edge is the edge fill's
   copy of the entry that straddles it, and is defined when that entry is. A window wholly off the
   map holds no straddling entry, so it defines nothing and is compared nowhere -- its cases still
   run, for the bounds.

   AND THAT THE COMPARISON HAS TEETH. A mirror with any one completion left out, or with one turned
   round, must disagree with the window build somewhere in the same cases. The completions' ORDER
   is not such a mutation: two of them meet only at a corner entry, which holds a single map cell,
   and there every order spreads it to all four corners -- so every order must AGREE, which is
   checked as the fact the header states.

     fog_expand_test [seed] [cases] */
#include "tagpu_fogsrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %u: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint32_t s_rng;
static uint32_t rnd(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }
static int rndi(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

#define MAXW 80
static uint32_t s_words[MAXW * ((MAXW + 15) / 16)];
static uint16_t s_win[(MAXW + 64) * (MAXW + 64)];

static void set_cell(TAGPU_FOGSRC* f, int cx, int cy, unsigned v)
{
    uint32_t* w = &s_words[(size_t)cy * f->stride + (cx >> 4)];
    *w = (*w & ~(3u << (2 * (cx & 15)))) | (v & 3u) << (2 * (cx & 15));
}

/* a source of `kind`: 0 random, 1 all dark, 2 all clear, 3 one cell at a corner or on an edge,
   4 random runs (the shapes a real explored map has) */
static void make_source(TAGPU_FOGSRC* f, int w, int h, int kind)
{
    int x, y;
    f->w = w; f->h = h; f->stride = tagpu_fog_stride(w); f->words = s_words; f->serial = 0;
    memset(s_words, 0, sizeof s_words);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned v = 0;
            if (kind == 0) v = rnd() & 3u;
            else if (kind == 1) v = 3;
            else if (kind == 4) v = ((x / rndi(1, 4) + y / 3) & 1) ? 3u : (rnd() & 1u) << 1;
            set_cell(f, x, y, v);
        }
    if (kind == 3) {
        static const int pick[8][2] = { {0, 0}, {1, 0}, {2, 0}, {0, 1}, {2, 1}, {0, 2}, {1, 2}, {2, 2} };
        int p = rndi(0, 7);
        x = pick[p][0] == 0 ? 0 : pick[p][0] == 1 ? rndi(0, w - 1) : w - 1;
        y = pick[p][1] == 0 ? 0 : pick[p][1] == 1 ? rndi(0, h - 1) : h - 1;
        set_cell(f, x, y, (unsigned)rndi(1, 3));
    }
}

/* the window build's edge fill: every entry past the one straddling an edge repeats it, rows
   first, so the columns then carry the corners outward with them */
static void edge_fill(const TAGPU_FOGSRC* f, uint16_t* out, int cols, int rows, int col0, int row0)
{
    int gTop = -row0 - 1, gBot = f->h - 1 - row0;
    int gLef = -col0 - 1, gRig = f->w - 1 - col0;
    int i, j;
    if (gTop > 0 && gTop < rows)
        for (i = 0; i < gTop; i++)
            memcpy(out + (size_t)i * cols, out + (size_t)gTop * cols, (size_t)cols * 2);
    if (gBot >= 0 && gBot < rows - 1)
        for (i = gBot + 1; i < rows; i++)
            memcpy(out + (size_t)i * cols, out + (size_t)gBot * cols, (size_t)cols * 2);
    if (gLef > 0 && gLef < cols)
        for (j = 0; j < rows; j++)
            for (i = 0; i < gLef; i++) out[(size_t)j * cols + i] = out[(size_t)j * cols + gLef];
    if (gRig >= 0 && gRig < cols - 1)
        for (j = 0; j < rows; j++)
            for (i = gRig + 1; i < cols; i++) out[(size_t)j * cols + i] = out[(size_t)j * cols + gRig];
}

/* the window build's u16 (low byte unexplored, high out of LOS) as the mirror's two nibbles */
static unsigned nibbles(uint16_t e) { return (e & 0xFu) | ((e >> 8) & 0xFu) << 4; }

/* THE MUTANTS: the expansion with the completions in `order` (a permutation of 0 top, 1 bottom,
   2 left, 3 right), with completion `drop` left out (-1 none), and with completion `flip` turned
   round (-1 none) */
static unsigned mutant(const TAGPU_FOGSRC* f, int cx, int cy, const int order[4], int drop, int flip)
{
    static const unsigned down[4] = { 0x33u, 0xCCu, 0x55u, 0xAAu };
    unsigned a, b, c, d, e;
    int k;
    if (cx < -1) cx = -1;
    if (cx > f->w - 1) cx = f->w - 1;
    if (cy < -1) cy = -1;
    if (cy > f->h - 1) cy = f->h - 1;
    a = tagpu_fog_cell(f, cx, cy);     b = tagpu_fog_cell(f, cx + 1, cy);
    c = tagpu_fog_cell(f, cx, cy + 1); d = tagpu_fog_cell(f, cx + 1, cy + 1);
    e = (a & 1u) | (b & 1u) << 1 | (c & 1u) << 2 | (d & 1u) << 3 |
        (a >> 1) << 4 | (b >> 1) << 5 | (c >> 1) << 6 | (d >> 1) << 7;
    for (k = 0; k < 4; k++) {
        int which = order[k], on;
        unsigned m = down[which];
        if (which == drop) continue;
        on = which == 0 ? cy == -1 : which == 1 ? cy == f->h - 1 : which == 2 ? cx == -1 : cx == f->w - 1;
        if (!on) continue;
        if (which == flip) {
            /* the copy the other way: top copies its top corners down, and so on */
            if (which == 0) e |= (e << 2) & 0xCCu;
            else if (which == 1) e |= (e >> 2) & 0x33u;
            else if (which == 2) e |= (e << 1) & 0xAAu;
            else e |= (e >> 1) & 0x55u;
        } else if (which == 0) e |= (e >> 2) & m;
        else if (which == 1) e |= (e << 2) & m;
        else if (which == 2) e |= (e >> 1) & m;
        else e |= (e << 1) & m;
    }
    return e;
}

/* the 24 orders of four completions, the engine's first */
static int s_orders[24][4];
static void make_orders(void)
{
    int n = 0, a, b, c, d;
    for (a = 0; a < 4; a++) for (b = 0; b < 4; b++) for (c = 0; c < 4; c++) for (d = 0; d < 4; d++)
        if (a != b && a != c && a != d && b != c && b != d && c != d) {
            s_orders[n][0] = a; s_orders[n][1] = b; s_orders[n][2] = c; s_orders[n][3] = d; n++;
        }
}

static long s_compared, s_skipped;
static long s_mutKilled[2][4];                 /* [drop, flip][completion]: cases it failed */
static long s_orderDiffer;

static void one_window(const TAGPU_FOGSRC* f, int cols, int rows, int col0, int row0)
{
    int gx, gy, k;
    tagpu_fog_build_window(f, s_win, cols, rows, col0, row0);
    edge_fill(f, s_win, cols, rows, col0, row0);
    for (gy = 0; gy < rows; gy++)
        for (gx = 0; gx < cols; gx++) {
            int cx = col0 + gx, cy = row0 + gy;
            int sx = cx < -1 ? -1 : cx > f->w - 1 ? f->w - 1 : cx;   /* the entry it repeats */
            int sy = cy < -1 ? -1 : cy > f->h - 1 ? f->h - 1 : cy;
            unsigned want, got;
            if (sx - col0 < 0 || sx - col0 > cols - 2 || sy - row0 < 0 || sy - row0 > rows - 2) {
                s_skipped++;
                continue;
            }
            want = nibbles(s_win[(size_t)gy * cols + gx]);
            got = tagpu_fog_entry(f, cx, cy);
            if (got != want) {
                printf("FAIL: w=%d h=%d window %dx%d at (%d,%d): entry (%d,%d) mirror %02x build %02x\n",
                       f->w, f->h, cols, rows, col0, row0, cx, cy, got, want);
                exit(1);
            }
            s_compared++;
            for (k = 1; k < 24; k++)
                if (mutant(f, cx, cy, s_orders[k], -1, -1) != want) s_orderDiffer++;
            for (k = 0; k < 4; k++) {
                if (mutant(f, cx, cy, s_orders[0], k, -1) != want) s_mutKilled[0][k]++;
                if (mutant(f, cx, cy, s_orders[0], -1, k) != want) s_mutKilled[1][k]++;
            }
        }
}

static void checks(void)
{
    TAGPU_FOGSRC f;
    int x, y;
    make_source(&f, 20, 17, 2);
    /* all clear: every entry 0, on and off the map */
    for (y = -5; y < 25; y++) for (x = -5; x < 30; x++) CHECK(tagpu_fog_entry(&f, x, y) == 0);
    /* all dark and all out of LOS: every entry, on and off the map, both nibbles full */
    make_source(&f, 20, 17, 1);
    for (y = -5; y < 25; y++) for (x = -5; x < 30; x++) CHECK(tagpu_fog_entry(&f, x, y) == 0xFFu);
    /* one dark cell at the top-left corner: entries (-1,-1) and (0,0) see it whole or in part */
    make_source(&f, 20, 17, 2);
    set_cell(&f, 0, 0, TAGPU_FOG_UNEXPLORED);
    CHECK(tagpu_fog_entry(&f, -1, -1) == 0x0Fu);       /* the corner spreads to all four      */
    CHECK(tagpu_fog_entry(&f, -7, -3) == 0x0Fu);       /* and past it, the clamp repeats it    */
    CHECK(tagpu_fog_entry(&f, 0, -1) == 0x05u);        /* top: tl and bl from bl              */
    CHECK(tagpu_fog_entry(&f, -1, 0) == 0x03u);        /* left: tl and tr from tr             */
    CHECK(tagpu_fog_entry(&f, 0, 0) == 0x01u);         /* on the map: the tl corner alone      */
    CHECK(tagpu_fog_entry(&f, 1, 1) == 0x00u);
    /* the cell's bits: bit 1 is the next nibble */
    set_cell(&f, 19, 16, TAGPU_FOG_OUTOFLOS);
    CHECK(tagpu_fog_entry(&f, 19, 16) == 0xF0u);       /* bottom-right corner, all four grey  */
    CHECK(tagpu_fog_entry(&f, 18, 15) == 0x80u);       /* its br corner only                  */
    /* the bound: a cell off the map is 0 and reads no word */
    CHECK(tagpu_fog_cell(&f, -1, 0) == 0 && tagpu_fog_cell(&f, 20, 0) == 0 &&
          tagpu_fog_cell(&f, 0, 17) == 0 && tagpu_fog_cell(&f, 0, -1) == 0);
    /* the guard */
    CHECK(tagpu_fog_src_ok(&f));
    f.stride++; CHECK(!tagpu_fog_src_ok(&f)); f.stride--;
    f.w = TAGPU_FOG_MAXDIM + 1; f.stride = tagpu_fog_stride(f.w); CHECK(!tagpu_fog_src_ok(&f));
    f.w = 0; f.stride = 0; CHECK(!tagpu_fog_src_ok(&f));
    CHECK(!tagpu_fog_src_ok(NULL));
    CHECK(tagpu_fog_stride(1) == 1 && tagpu_fog_stride(16) == 1 && tagpu_fog_stride(17) == 2 &&
          tagpu_fog_stride(TAGPU_FOG_MAXDIM) == TAGPU_FOG_MAXDIM / 16);
    printf("checks: every check holds\n");
}

int main(int argc, char** argv)
{
    unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1u;
    long cases = argc > 2 ? strtol(argv[2], NULL, 10) : 20000;
    long c;
    int k;
    s_rng = seed;
    make_orders();
    checks();
    for (c = 0; c < cases; c++) {
        TAGPU_FOGSRC f;
        int w = rndi(1, MAXW), h = rndi(1, MAXW);
        int kind = (int)(c % 5);
        int cols = rndi(2, 48), rows = rndi(2, 48), col0, row0, where = rndi(0, 6);
        make_source(&f, w, h, kind);
        switch (where) {
        case 0:  col0 = rndi(0, w); row0 = rndi(0, h); break;                        /* inside      */
        case 1:  col0 = rndi(-cols, 0); row0 = rndi(-2, h); break;                   /* left edge   */
        case 2:  col0 = rndi(w - cols, w + 1); row0 = rndi(-2, h); break;            /* right edge  */
        case 3:  col0 = rndi(-2, w); row0 = rndi(-rows, 0); break;                   /* top edge    */
        case 4:  col0 = rndi(-2, w); row0 = rndi(h - rows, h + 1); break;            /* bottom edge */
        case 5:  col0 = rndi(-cols - 3, w + 3); row0 = rndi(-rows - 3, h + 3); break;/* a corner    */
        default: col0 = rndi(0, 1) ? -cols - rndi(2, 30) : w + rndi(1, 30);          /* wholly off  */
                 row0 = rndi(-rows - 30, h + 30); break;
        }
        one_window(&f, cols, rows, col0, row0);
    }
    printf("%ld windows (seed %u): %ld entries compared, %ld undefined by the window build\n",
           cases, seed, s_compared, s_skipped);
    CHECK(s_compared > 0);
    /* every other order of the completions agrees with the window build everywhere */
    printf("completion order: %ld disagreements over 23 other orders\n", s_orderDiffer);
    CHECK(s_orderDiffer == 0);
    /* and every completion is needed, and needed the right way round */
    for (k = 0; k < 4; k++) {
        static const char* name[4] = { "top", "bottom", "left", "right" };
        printf("mutant %-6s: dropped differs on %ld entries, turned round on %ld\n",
               name[k], s_mutKilled[0][k], s_mutKilled[1][k]);
        CHECK(s_mutKilled[0][k] > 0 && s_mutKilled[1][k] > 0);
    }
    printf("every check holds\n");
    return 0;
}
