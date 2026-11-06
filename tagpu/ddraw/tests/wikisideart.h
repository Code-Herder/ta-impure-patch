/* wikisideart.h -- a side pack's art read into a WIKISIDE as the asset loader fills it, for the
   native checks that compose from it: tests/wikibartest.c (tools/wiki/barcheck.mjs) and
   tests/wikiframetest.c (tools/wiki/framecheck.mjs). DIR holds art.json, sheet.bin, pal.bin and
   fnt.bin, as tools/wiki/barcheck.mjs writeC writes them.

   art.json holds the art as the loader fills WIKISIDE, a frame as [sx, sy, w, h, hx, hy]:
   {"sheet": [w, h], "panelside": [{"f", "key"} | null, one a layout side], "bar": [{"top": {"f",
   "key"}, "bot": {"f", "key"}, "rects": [[x1, y1, x2, y2], ... the fifteen in WB_* order], "ecol",
   "mcol", "font": [offset, length] into fnt.bin} | null, one a layout side], "logos": [{"f", "key"}]
   | null, "gui": [256 indices] | null}.

   Static functions in a header: each check is one translation unit. */
#ifndef WIKISIDEART_H
#define WIKISIDEART_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_wikiasset.h"

static unsigned char* slurp(const char* dir, const char* name, size_t* n)
{
    char path[1024];
    FILE* f;
    unsigned char* p;
    long len;
    *n = 0;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(f = fopen(path, "rb"))) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || !(p = (unsigned char*)malloc((size_t)len + 1)) || fread(p, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[len] = 0;
    *n = (size_t)len;
    return p;
}

static int num(const TJV* v, int dflt) { return tj_is(v, TJ_NUM) ? (int)v->num : dflt; }

static int frame(const TJV* v, WIKISIDEFRAME* f)
{
    if (!tj_is(v, TJ_ARR) || v->n != 6) return 0;
    f->sx = num(tj_at(v, 0), 0);
    f->sy = num(tj_at(v, 1), 0);
    f->w = num(tj_at(v, 2), 0);
    f->h = num(tj_at(v, 3), 0);
    f->hx = num(tj_at(v, 4), 0);
    f->hy = num(tj_at(v, 5), 0);
    return 1;
}

/* a {"f", "key"} into a frame and its key */
static int framed(const TJV* v, WIKISIDEFRAME* f, int* key)
{
    if (!tj_is(v, TJ_OBJ) || !frame(tj_get(v, "f"), f)) return 0;
    *key = num(tj_get(v, "key"), -1);
    return 1;
}

static int bar(const TJV* v, WIKIBARART* b, size_t nfnt)
{
    const TJV *rects = tj_get(v, "rects"), *font = tj_get(v, "font"), *r;
    int k = 0, off, len;
    if (!framed(tj_get(v, "top"), &b->top, &b->top_key) || !framed(tj_get(v, "bot"), &b->bot, &b->bot_key)) return 0;
    if (!tj_is(rects, TJ_ARR) || rects->n != WB_N) return 0;
    for (r = rects->first; r; r = r->next, k++) {
        if (!tj_is(r, TJ_ARR) || r->n != 4) return 0;
        b->rect[k].x1 = (int16_t)num(tj_at(r, 0), 0);
        b->rect[k].y1 = (int16_t)num(tj_at(r, 1), 0);
        b->rect[k].x2 = (int16_t)num(tj_at(r, 2), 0);
        b->rect[k].y2 = (int16_t)num(tj_at(r, 3), 0);
    }
    b->ecol = (uint8_t)num(tj_get(v, "ecol"), 0);
    b->mcol = (uint8_t)num(tj_get(v, "mcol"), 0);
    off = num(tj_at(font, 0), -1);
    len = num(tj_at(font, 1), -1);
    if (off < 0 || len < 0 || (size_t)off + (size_t)len > nfnt) return 0;
    b->font_off = (uint32_t)off;
    b->font_len = (uint32_t)len;
    return 1;
}

static WIKISIDE* load(const char* dir, unsigned char** sheet)
{
    char err[256];
    size_t nart, nsheet, npal, nfnt;
    unsigned char *atext = slurp(dir, "art.json", &nart), *pal = slurp(dir, "pal.bin", &npal), *fnt = slurp(dir, "fnt.bin", &nfnt);
    WIKISIDE* sd = (WIKISIDE*)calloc(1, sizeof *sd);
    TJDOC* doc = NULL;
    const TJV *a, *sh, *ps, *bars, *logos, *gui, *c;
    int ok = 0, k;
    *sheet = slurp(dir, "sheet.bin", &nsheet);
    if (!sd || !atext || !*sheet || !pal || npal != 1024 || !fnt || nfnt > WIKI_FNT_BYTES) {
        fprintf(stderr, "wikibartest: %s: a file is missing\n", dir);
        goto done;
    }
    if (!(doc = tagpu_json_parse((const char*)atext, nart, err, sizeof err))) {
        fprintf(stderr, "wikibartest: %s/art.json: %s\n", dir, err);
        goto done;
    }
    a = tagpu_json_root(doc);
    sh = tj_get(a, "sheet");
    sd->sheet_w = num(tj_at(sh, 0), 0);
    sd->sheet_h = num(tj_at(sh, 1), 0);
    if (sd->sheet_w <= 0 || sd->sheet_h <= 0 || (size_t)sd->sheet_w * (size_t)sd->sheet_h != nsheet) {
        fprintf(stderr, "wikibartest: %s: the sheet is not w x h bytes\n", dir);
        goto done;
    }
    sd->sheet = *sheet;
    memcpy(sd->pal, pal, sizeof sd->pal);
    memcpy(sd->fnt, fnt, nfnt);
    ps = tj_get(a, "panelside");
    bars = tj_get(a, "bar");
    if (!tj_is(ps, TJ_ARR) || !tj_is(bars, TJ_ARR) || ps->n != bars->n || ps->n < 1 || ps->n > WIKI_SIDE_SIDES) goto shape;
    sd->layout.nside = ps->n;
    for (k = 0, c = ps->first; c; c = c->next, k++)
        if ((sd->has_panelside[k] = tj_is(c, TJ_OBJ)) && !framed(c, &sd->panelside[k], &sd->panelside_key[k])) goto shape;
    for (k = 0, c = bars->first; c; c = c->next, k++)
        if ((sd->has_bar[k] = tj_is(c, TJ_OBJ)) && !bar(c, &sd->bar[k], nfnt)) goto shape;
    logos = tj_get(a, "logos");
    if (tj_is(logos, TJ_ARR)) {
        if (logos->n > WIKI_LOGOS) goto shape;
        for (c = logos->first; c; c = c->next, sd->nlogo++)
            if (!framed(c, &sd->logo[sd->nlogo], &sd->logo_key[sd->nlogo])) goto shape;
    }
    gui = tj_get(a, "gui");
    if ((sd->has_gui = tj_is(gui, TJ_ARR))) {
        if (gui->n != 256) goto shape;
        for (k = 0, c = gui->first; c; c = c->next, k++) sd->gui[k] = (uint8_t)num(c, 0);
    }
    ok = 1;
    goto done;
shape:
    fprintf(stderr, "wikibartest: %s/art.json does not fit WIKISIDE\n", dir);
done:
    free(atext);
    free(pal);
    free(fnt);
    if (doc) tagpu_json_free(doc);
    if (!ok) {
        free(sd);
        return NULL;
    }
    return sd;
}

#endif
