/* wikiscenetest.c -- the C half of the wiki's 3D scene check, for tools/wiki/scenecheck.mjs: the real
   tagpu_wikisim.c, tagpu_wikistage.c (the record's copy) and tagpu_wikiscene.c, compiled natively,
   write the stage's scene as JSON lines, and scenecheck.mjs pairs them with the mockup's.

   `wikiscenetest PAK GROUNDS` reads every entry's ground from GROUNDS, a text file scenecheck.mjs
   writes out of the mockup's pack (tools/wiki/gfx.py) -- per entry a line `ENTRY W H NTILE NFEAT`,
   a line of the W/2 x H/2 tile map, and a line per feature `CX CZ FX FZ HEIGHT FLAGS NBODY
   [W H HX HY CK SUBALP]... NSHADOW [...]` -- and writes, per entry, one line
   {"e", "ground": {"refused", "gw", "gh", "cells": [[col, row, tile]...], "shadow": [[x0, y0, x1,
   y1, enc, mode, sub, feat, wax, waz]...], "body": [...]}}; then plays every demo of every entry in
   PAK (tools/hudpak.py --set wiki) to its end, and at tick 0, every 30th tick and the last copies
   the view as the record carries it (tagpu_wikistage_copy) and writes {"e", "d", "t", "fog": null
   or {"cols", "rows", "org": [x, z], "grid": [...]}} with the grid's entries as their two bytes,
   b0 then b1.
   Exit 0 when everything loaded and ran; the comparison is scenecheck.mjs's. */
#define STB_TRUETYPE_IMPLEMENTATION   /* tagpu_wikistage.c links tagpu_hudtext.c, which needs it once */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hudtext.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikisim.h"
#include "tagpu_wikistage.h"
#include "tagpu_wikiscene.h"

static char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    char* p;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || !(p = (char*)malloc((size_t)len + 1)) || fread(p, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[len] = 0;
    *n = (size_t)len;
    return p;
}

static void quads(const char* name, const WSCENEQUAD* q, int n)
{
    int i;
    printf(",\"%s\":[", name);
    for (i = 0; i < n; i++)
        printf("%s[%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%d,%.9g,%.9g]", i ? "," : "", q[i].x0, q[i].y0, q[i].x1,
               q[i].y1, q[i].enc, q[i].mode, q[i].sub, q[i].feat, q[i].wax, q[i].waz);
    putchar(']');
}

/* one entry's ground from the file, its scene written; 0 at the file's end, -1 when it is malformed */
static int ground(FILE* f, char* entry, size_t n)
{
    static WSCENE sc;
    static WSCENEFEAT feat[WIKISCENE_FEATS];
    static uint16_t tm[WIKISCENE_TILES];
    WSCENEGROUND g;
    char fmt[16];
    int i, k;
    snprintf(fmt, sizeof fmt, "%%%us", (unsigned)(n - 1));
    if (fscanf(f, fmt, entry) != 1) return 0;
    if (fscanf(f, "%d %d %d %d", &g.w, &g.h, &g.ntile, &g.nfeat) != 4 || g.w < 2 || g.h < 2 ||
        g.w > WIKISCENE_MAP || g.h > WIKISCENE_MAP || g.nfeat < 0 || g.nfeat > WIKISCENE_FEATS)
        return -1;
    for (i = 0; i < (g.w / 2) * (g.h / 2); i++) {
        unsigned v;
        if (fscanf(f, "%u", &v) != 1) return -1;
        tm[i] = (uint16_t)v;
    }
    for (i = 0; i < g.nfeat; i++) {
        WSCENEFEAT* t = &feat[i];
        int cx, cz, fx, fz, h, nb, ns;
        unsigned fl;
        memset(t, 0, sizeof *t);
        if (fscanf(f, "%d %d %d %d %d %u %d", &cx, &cz, &fx, &fz, &h, &fl, &nb) != 7 || nb < 0 || nb > WIKISCENE_SUBS)
            return -1;
        t->cx = (int16_t)cx; t->cz = (int16_t)cz; t->fx = (int16_t)fx; t->fz = (int16_t)fz;
        t->height = (int16_t)h; t->flags = fl; t->nbody = (uint8_t)nb;
        for (k = 0; k < nb; k++) {
            int w, hh, hx, hy, ck, sa;
            if (fscanf(f, "%d %d %d %d %d %d", &w, &hh, &hx, &hy, &ck, &sa) != 6) return -1;
            t->body[k].w = (int16_t)w; t->body[k].h = (int16_t)hh; t->body[k].hx = (int16_t)hx;
            t->body[k].hy = (int16_t)hy; t->body[k].ck = (uint8_t)ck; t->body[k].subalp = (uint8_t)sa;
        }
        if (fscanf(f, "%d", &ns) != 1 || ns < 0 || ns > WIKISCENE_SUBS) return -1;
        t->nshadow = (uint8_t)ns;
        for (k = 0; k < ns; k++) {
            int w, hh, hx, hy, ck, sa;
            if (fscanf(f, "%d %d %d %d %d %d", &w, &hh, &hx, &hy, &ck, &sa) != 6) return -1;
            t->shadow[k].w = (int16_t)w; t->shadow[k].h = (int16_t)hh; t->shadow[k].hx = (int16_t)hx;
            t->shadow[k].hy = (int16_t)hy; t->shadow[k].ck = (uint8_t)ck; t->shadow[k].subalp = (uint8_t)sa;
        }
    }
    g.tilemap = tm;
    g.feat = feat;
    tagpu_wikiscene_ground(&g, &sc);
    printf("{\"e\":\"%s\",\"ground\":{\"refused\":%d,\"gw\":%d,\"gh\":%d,\"cells\":[", entry, sc.refused, sc.gw, sc.gh);
    for (i = 0; i < sc.ncell; i++)
        printf("%s[%d,%d,%u]", i ? "," : "", sc.cell[i].col, sc.cell[i].row, (unsigned)sc.cell[i].tile);
    putchar(']');
    quads("shadow", sc.shadow, sc.nshadow);
    quads("body", sc.body, sc.nbody);
    puts("}}");
    return 1;
}

int main(int argc, char** argv)
{
    static WIKISTAGEVIEW v;
    static WSCENEFOG fog;
    char err[256], entry[64];
    size_t n;
    char* pak;
    FILE* gf;
    WIKIDATA* d;
    int i, j, r;
    if (argc != 3) {
        fprintf(stderr, "usage: wikiscenetest PAK GROUNDS\n");
        return 2;
    }
    if (!(gf = fopen(argv[2], "r"))) { fprintf(stderr, "wikiscenetest: cannot read %s\n", argv[2]); return 2; }
    while ((r = ground(gf, entry, sizeof entry)) > 0) {}
    fclose(gf);
    if (r < 0) { fprintf(stderr, "wikiscenetest: %s: a malformed ground after %s\n", argv[2], entry); return 2; }
    if (!(pak = slurp(argv[1], &n))) { fprintf(stderr, "wikiscenetest: cannot read %s\n", argv[1]); return 2; }
    if (!(d = tagpu_wikidata_load((const unsigned char*)pak, n, err, sizeof err))) {
        fprintf(stderr, "wikiscenetest: %s\n", err);
        return 2;
    }
    for (i = 0; i < d->nentry; i++)
        for (j = 0; j < d->entry[i].ndemo; j++) {
            const WIKIENTRY* e = &d->entry[i];
            WIKISIM s;
            if (!tagpu_wikisim_init(&s, e, &e->demo[j], 1, err, sizeof err)) {
                fprintf(stderr, "wikiscenetest: %s/%s: %s\n", e->id, e->demo[j].id, err);
                return 2;
            }
            for (;;) {
                if (s.tick % 30 == 0 || s.done) {
                    int k;
                    tagpu_wikistage_copy(&v, tagpu_wikisim_view(&s), e);
                    tagpu_wikiscene_fog(v.fog, v.w, v.h, &fog);
                    printf("{\"e\":\"%s\",\"d\":\"%s\",\"t\":%d,\"fog\":", e->id, e->demo[j].id, s.tick);
                    if (!fog.on) fputs("null", stdout);
                    else {
                        printf("{\"cols\":%d,\"rows\":%d,\"org\":[%.9g,%.9g],\"grid\":[", fog.cols, fog.rows,
                               fog.orgx, fog.orgz);
                        for (k = 0; k < fog.cols * fog.rows; k++)
                            printf("%s%u,%u", k ? "," : "", fog.grid[k] & 0xFFu, fog.grid[k] >> 8);
                        fputs("]}", stdout);
                    }
                    puts("}");
                }
                if (s.done) break;
                tagpu_wikisim_step(&s);
            }
            tagpu_wikisim_free(&s);
        }
    tagpu_wikidata_free(d);
    free(pak);
    return 0;
}
