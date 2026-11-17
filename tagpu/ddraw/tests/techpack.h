/* techpack.h -- a tech tree pack (tools/wiki/mockup/gfx/side/<mod>/techtree/tree.json, which
   tools/wiki/assetcheck.py --side proves equal to the game's own load) made into a WIKITECH as the
   loader makes one, for the native checks: tests/wikitechtest.c (the tree's drawing) and
   tools/wiki/wikilaycheck.c (the unit's panel).

   Every unit's record whole -- its flags, figures, weapons (an id no weapons\*.tdf section has
   reads as the id with no range: `known` 0) -- each title back in code page 1252 and each
   description and weapon name in Latin-1, the code pages the loader reads them in, and each
   picture's size from its PNG under the pack's folder (no pixels: the checks lay out, they do not
   paint). Every side's walk and menus in the file's order, each unit its row index.

   Header-only (static functions), C99 and tagpu_json.c. techpack_free releases what
   techpack_load returns. */
#ifndef TECHPACK_H
#define TECHPACK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_wikiasset.h"

/* UTF-8 `s` into the single bytes of `d` (`n` with the terminator): code page 1252 when `cp1252`,
   else Latin-1; a code point neither holds reads '?' */
static void techpack_bytes(unsigned char* d, size_t n, const char* s, int cp1252)
{
    static const unsigned short HI[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030,
        0x0160, 0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122,
        0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
    size_t o = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p && o + 1 < n) {
        unsigned v = *p++;
        int k;
        if (v >= 0xE0 && p[0] && p[1]) { v = (v & 0x0F) << 12 | (p[0] & 0x3Fu) << 6 | (p[1] & 0x3Fu); p += 2; }
        else if (v >= 0xC0 && p[0]) { v = (v & 0x1F) << 6 | (p[0] & 0x3Fu); p++; }
        if (cp1252 && v > 0xFF)
            for (k = 0; k < 32; k++)
                if (HI[k] == v) { v = 0x80 + (unsigned)k; break; }
        d[o++] = (unsigned char)(v > 0xFF ? '?' : v);
    }
    d[o] = 0;
}

static int techpack_png_size(const char* path, int* w, int* h)
{
    unsigned char b[24];
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(b, 1, 24, f) != 24) { fclose(f); return 0; }
    fclose(f);
    *w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
    *h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
    return 1;
}

/* the row index of unit `key` in the file's units, -1 none */
static int techpack_index(const TJV* U, const char* key)
{
    const TJV* k;
    int i = 0;
    for (k = U->first; k; k = k->next, i++)
        if (!strcmp(k->key, key)) return i;
    return -1;
}

static void techpack_free(WIKITECH* t)
{
    if (!t) return;
    free((void*)t->unit);
    free((void*)t->pic);
    free((void*)t->order);
    free((void*)t->mfirst);
    free((void*)t->mcount);
    free((void*)t->menu);
    free(t);
}

/* The pack at `tree` (its pictures under `dir`) as a WIKITECH, or NULL with the reason in `err` */
static WIKITECH* techpack_load(const char* tree, const char* dir, char* err, size_t errn)
{
    FILE* f;
    long len;
    char* text = NULL;
    TJDOC* doc = NULL;
    const TJV *T, *U, *SD, *v, *s;
    WIKITECH* t = NULL;
    WIKITECHUNIT* units = NULL;
    WIKITECHPIC* pics = NULL;
    uint16_t *order = NULL, *mcount = NULL, *menu = NULL;
    uint32_t* mfirst = NULL;
    uint32_t no = 0, cap = 0, nm = 0, at = 0;
    int nu = 0;
    char path[2048];
    if (!(f = fopen(tree, "rb"))) { snprintf(err, errn, "%.200s: cannot open", tree); return NULL; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || !(text = (char*)malloc((size_t)len + 1)) || fread(text, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(text);
        snprintf(err, errn, "%.200s: cannot read", tree);
        return NULL;
    }
    fclose(f);
    text[len] = 0;
    if (!(doc = tagpu_json_parse(text, (size_t)len, err, errn))) { free(text); return NULL; }
    T = tagpu_json_root(doc);
    U = tj_get(T, "units");
    SD = tj_get(T, "sides");
    if (!tj_is(U, TJ_OBJ) || !tj_is(SD, TJ_ARR) || U->n > WIKITECH_UNITS || SD->n < 1 || SD->n > WIKITECH_SIDES) {
        snprintf(err, errn, "%.200s: not a tech tree pack", tree);
        goto fail;
    }
    for (s = SD->first; s; s = s->next) {
        const TJV* m;
        no += (uint32_t)tj_get(s, "order")->n;
        for (m = tj_get(s, "menus")->first; m; m = m->next) cap += (uint32_t)m->n;
    }
    if (!(t = (WIKITECH*)calloc(1, sizeof *t)) || !(units = (WIKITECHUNIT*)calloc((size_t)U->n + 1, sizeof *units)) ||
        !(pics = (WIKITECHPIC*)calloc((size_t)U->n + 1, sizeof *pics)) || !(order = (uint16_t*)calloc(no + 1, 2)) ||
        !(mfirst = (uint32_t*)calloc(no + 1, 4)) || !(mcount = (uint16_t*)calloc(no + 1, 2)) || !(menu = (uint16_t*)calloc(cap + 1, 2))) {
        snprintf(err, errn, "%.200s: no memory", tree);
        goto fail;
    }
    for (v = U->first; v; v = v->next, nu++) {
        WIKITECHUNIT* x = &units[nu];
        const TJV *fp = tj_get(v, "fp"), *st = tj_get(v, "storage"), *ws = tj_get(v, "weapons"), *w;
        const char* cat = tj_str(tj_get(v, "category"), "");
        size_t k;
        snprintf(x->name, sizeof x->name, "%s", tj_str(tj_get(v, "name"), ""));
        techpack_bytes(x->title, sizeof x->title, tj_str(tj_get(v, "title"), ""), 1);
        if (!strcmp((const char*)x->title, x->name)) x->title[0] = 0;
        techpack_bytes(x->desc, sizeof x->desc, tj_str(tj_get(v, "description"), ""), 0);
        x->structure = tj_is(tj_get(v, "structure"), TJ_TRUE);
        x->mobile = tj_is(tj_get(v, "mobile"), TJ_TRUE);
        x->builder = tj_is(tj_get(v, "builder"), TJ_TRUE);
        x->canfly = tj_is(tj_get(v, "canfly"), TJ_TRUE);
        x->water = tj_is(tj_get(v, "water"), TJ_TRUE);
        x->cloaked = tj_is(tj_get(v, "cloaked"), TJ_TRUE);
        x->stealth = tj_is(tj_get(v, "stealth"), TJ_TRUE);
        x->hidedamage = tj_is(tj_get(v, "hidedamage"), TJ_TRUE);
        x->level = -1;
        for (k = 0; cat[k]; k++) {
            size_t q;
            for (q = 0; q < 5 && cat[k + q] && (cat[k + q] | 0x20) == "level"[q]; q++) {}
            if (q == 5 && cat[k + 5] >= '0' && cat[k + 5] <= '9') { x->level = (int8_t)(cat[k + 5] - '0'); break; }
        }
        x->fx = (int32_t)tj_num(tj_at(fp, 0), 0);
        x->fz = (int32_t)tj_num(tj_at(fp, 1), 0);
        x->health = (int32_t)tj_num(tj_get(v, "health"), 0);
        x->sight = (int32_t)tj_num(tj_get(v, "sight"), 0);
        x->radar = (int32_t)tj_num(tj_get(v, "radar"), 0);
        x->metal = tj_num(tj_get(v, "metal"), 0);
        x->energy = tj_num(tj_get(v, "energy"), 0);
        x->buildtime = tj_num(tj_get(v, "buildtime"), 0);
        x->speed = tj_num(tj_get(v, "speed"), 0);
        x->workertime = tj_num(tj_get(v, "workertime"), 0);
        x->energymake = tj_num(tj_get(v, "energymake"), 0);
        x->metalmake = tj_num(tj_get(v, "metalmake"), 0);
        x->extracts = tj_num(tj_get(v, "extracts"), 0);
        x->mstore = tj_num(tj_at(st, 0), 0);
        x->estore = tj_num(tj_at(st, 1), 0);
        for (w = ws ? ws->first : NULL; w && x->nweapon < WIKITECH_WEAPONS; w = w->next) {
            WIKITECHWEAPON* y = &x->weapon[x->nweapon++];
            snprintf(y->id, sizeof y->id, "%s", tj_str(tj_get(w, "id"), ""));
            techpack_bytes((unsigned char*)y->name, sizeof y->name, tj_str(tj_get(w, "name"), ""), 0);
            y->known = tj_is(tj_get(w, "range"), TJ_NUM);
            y->range = (int32_t)tj_num(tj_get(w, "range"), 0);
            y->damage = (int32_t)tj_num(tj_get(w, "damage"), 0);
        }
        x->pic = -1;
        if (tj_is(tj_get(v, "pic"), TJ_STR)) {
            int pw, ph;
            snprintf(path, sizeof path, "%s/%s", dir, tj_str(tj_get(v, "pic"), ""));
            if (!techpack_png_size(path, &pw, &ph)) { snprintf(err, errn, "cannot read %.200s", path); goto fail; }
            pics[t->npic].w = pw;
            pics[t->npic].h = ph;
            pics[t->npic].tkey = -1;
            x->pic = t->npic++;
        }
    }
    for (s = SD->first; s; s = s->next) {
        WIKITECHSIDE* sd = &t->side[t->nside++];
        const TJV *od = tj_get(s, "order"), *ms = tj_get(s, "menus"), *e;
        snprintf(sd->name, sizeof sd->name, "%s", tj_str(tj_get(s, "name"), ""));
        sd->first = at;
        sd->norder = (uint32_t)od->n;
        for (e = od->first; e; e = e->next, at++) {
            const TJV *k2, *m = tj_get(ms, e->str);
            const int ui = techpack_index(U, e->str);
            if (ui < 0) { snprintf(err, errn, "%.200s: the walk names %s, no unit", tree, e->str); goto fail; }
            order[at] = (uint16_t)ui;
            mfirst[at] = nm;
            mcount[at] = (uint16_t)(m ? m->n : 0);
            for (k2 = m ? m->first : NULL; k2; k2 = k2->next) {
                const int mi = techpack_index(U, k2->str);
                if (mi < 0) { snprintf(err, errn, "%.200s: a menu names %s, no unit", tree, k2->str); goto fail; }
                menu[nm++] = (uint16_t)mi;
            }
        }
        sd->commander = order[sd->first];
        t->of_engine[t->nside - 1] = (int8_t)(t->nside - 1);
    }
    t->norder = no;
    t->nmenu = nm;
    t->order = order;
    t->mfirst = mfirst;
    t->mcount = mcount;
    t->menu = menu;
    t->nunit = nu;
    t->unit = units;
    t->pic = pics;
    tagpu_json_free(doc);
    free(text);
    return t;
fail:
    if (t) {
        t->unit = units; t->pic = pics; t->order = order; t->mfirst = mfirst; t->mcount = mcount; t->menu = menu;
        techpack_free(t);
    } else {
        free(units); free(pics); free(order); free(mfirst); free(mcount); free(menu);
    }
    tagpu_json_free(doc);
    free(text);
    return NULL;
}

#endif
