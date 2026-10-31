/* wikibartest.c -- the C half of the bars' check, for tools/wiki/barcheck.mjs: tagpu_wikibar.c
   compiled natively composes every case barcheck.mjs lists, and barcheck.mjs compares each picture
   byte for byte with bar.js's.

   `wikibartest compose DIR CASES` reads a side pack's bar art from DIR into a WIKISIDE as the
   asset loader fills it (tagpu_wikiasset.h) -- art.json (below), sheet.bin, pal.bin and fnt.bin --
   and composes each line of CASES:
       side colour Wg Hg rgba e m emax mmax ei eo mi mo raw_e raw_m lev_e lev_m
   the numbers as JavaScript prints them (strtod reads its NaN and Infinity too). Per case, to
   stdout: tagpu_wikibar_compose's answer as a little-endian i32 (the bars' width, or -1), then,
   when it is not -1, the WIKIBAR_W x WIKIBAR_H indices, and when `rgba` is 1 the RGBA
   (tagpu_wikibar_rgba), 4 bytes a pixel. The files' shapes: wikisideart.h. Exit 0 when everything
   loaded and ran, 2 when an input is unreadable or does not fit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_wikiasset.h"
#include "tagpu_wikibar.h"
#include "wikisideart.h"

static void put32(int v)
{
    const unsigned u = (unsigned)v;
    const unsigned char b[4] = { (unsigned char)u, (unsigned char)(u >> 8), (unsigned char)(u >> 16), (unsigned char)(u >> 24) };
    fwrite(b, 1, 4, stdout);
}

int main(int argc, char** argv)
{
    static uint8_t idx[WIKIBAR_W * WIKIBAR_H], rgba[WIKIBAR_W * WIKIBAR_H * 4];
    unsigned char* sheet = NULL;
    WIKISIDE* sd;
    FILE* cases;
    char line[2048];
    int ncase = 0;
    if (argc != 4 || strcmp(argv[1], "compose")) {
        fprintf(stderr, "usage: wikibartest compose DIR CASES\n");
        return 2;
    }
    if (!(sd = load(argv[2], &sheet))) return 2;
    if (!(cases = fopen(argv[3], "r"))) { fprintf(stderr, "wikibartest: cannot read %s\n", argv[3]); return 2; }
    while (fgets(line, sizeof line, cases)) {
        WIKIBARNUM n;
        int side, colour, Wg, Hg, want_rgba, w;
        char t[12][64];
        double* const field[12] = { &n.e, &n.m, &n.emax, &n.mmax, &n.ei, &n.eo, &n.mi, &n.mo, &n.raw_e, &n.raw_m,
                                    &n.lev_e, &n.lev_m };
        int k;
        if (sscanf(line, "%d %d %d %d %d %63s %63s %63s %63s %63s %63s %63s %63s %63s %63s %63s %63s", &side, &colour, &Wg,
                   &Hg, &want_rgba, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11]) != 17) {
            fprintf(stderr, "wikibartest: case %d is not 17 fields\n", ncase);
            return 2;
        }
        for (k = 0; k < 12; k++) *field[k] = strtod(t[k], NULL);
        w = tagpu_wikibar_compose(sd, side, colour, Wg, Hg, &n, idx);
        put32(w);
        if (w >= 0) {
            fwrite(idx, 1, sizeof idx, stdout);
            if (want_rgba) {
                tagpu_wikibar_rgba(sd, idx, w, rgba);
                fwrite(rgba, 1, sizeof rgba, stdout);
            }
        }
        ncase++;
    }
    fclose(cases);
    fprintf(stderr, "wikibartest: %d cases\n", ncase);
    free(sd);
    free(sheet);
    return 0;
}
