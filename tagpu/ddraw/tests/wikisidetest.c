/* wikisidetest.c -- the C half of the sidebar composer's check, for tools/wiki/sidecheck.mjs: the
   real tagpu_wikisim.c, tagpu_wikistage.c's copy and tagpu_wikiside.c, compiled natively, write
   every sampled frame's composed column, and sidecheck.mjs compares it byte for byte with
   side.js's.

   `wikisidetest compose PAK DIR [CTX]` reads a sidebar's art from DIR -- layout.json (sim.js's
   schema), art.json (below), sheet.bin, pal.bin, and seen.bin and greyed.bin when present -- into
   a WIKISIDE, plays every demo of every entry in PAK (tools/hudpak.py --set wiki) on its layout in
   the context CTX names, and at every sampled tick (`sampled` below) copies the view of the entry
   the sim plays (tagpu_wikistage_copy over WIKISIM.entry, the bound entry with a casting, as
   tagpu_wiki.c copies it) and composes its column for the view's side over DIR/bases/<entry>.bin,
   the stage's minimap picture, when that file exists. CTX is a JSON file, sim.js's ctx:
   {"side": k, "iface": 0 | 1, "casting": a pack's casting record | null}; without it the sims are
   made with none (side 0, no casting, Interface Type 1). Per
   frame, to stdout: the key "<entry>/<demo>/<tick>" as a little-endian u16 length and its bytes,
   then two little-endian i32 -- the rows composed (0: none, the view has no sidebar; -1: the demo
   did not start, its tick 0 alone) and whether the minimap was composed -- then, with rows, the
   indices, the coverage and the RGBA (tagpu_wikiside_rgba), 128 x rows each, 1, 1 and 4 bytes a
   pixel.

   art.json holds the art as the asset loader fills WIKISIDE (tagpu_wikiasset.h), a frame as
   [sx, sy, w, h, hx, hy]: {"sheet": [w, h], "panelside": [{"f", "key"} | null, one a layout side],
   "dots": {"key", "radlogo": [f...], "high": f | null}, "unexplored", "box", "screens": [{"panel":
   {"f", "key"} | null, "pics": [{"from" (WIKISIDE_FROM_*), "frames": [f...], "key", "attribs",
   "text"}]}] in the layout's screen order}.

   Exit 0 when everything loaded and ran, 2 when an input is unreadable or refused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_wikiasset.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikiside.h"
#include "tagpu_wikisim.h"
#include "tagpu_wikistage.h"

static unsigned char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    unsigned char* p;
    long len;
    *n = 0;
    if (!f) return NULL;
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

static unsigned char* file_in(const char* dir, const char* name, size_t* n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return slurp(path, n);
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

/* art.json into `sd`, whose layout is already filled; 0 with a message when it does not fit */
static int art(WIKISIDE* sd, const TJV* a)
{
    const TJV *ps = tj_get(a, "panelside"), *dots = tj_get(a, "dots"), *scr = tj_get(a, "screens"), *c;
    int k = 0, i;
    if (!tj_is(ps, TJ_ARR) || ps->n != sd->layout.nside) return 0;
    for (c = ps->first; c; c = c->next, k++)
        if ((sd->has_panelside[k] = tj_is(c, TJ_OBJ))) {
            if (!frame(tj_get(c, "f"), &sd->panelside[k])) return 0;
            sd->panelside_key[k] = num(tj_get(c, "key"), -1);
        }
    k = 0;
    sd->dot_key = num(tj_get(dots, "key"), -1);
    c = tj_get(dots, "radlogo");
    if (!tj_is(c, TJ_ARR) || c->n > WIKISIDE_DOTS) return 0;
    for (c = c->first; c; c = c->next)
        if (!frame(c, &sd->radlogo[sd->nradlogo++])) return 0;
    if ((sd->has_high = tj_is(tj_get(dots, "high"), TJ_ARR)) && !frame(tj_get(dots, "high"), &sd->high)) return 0;
    sd->unexplored = num(tj_get(a, "unexplored"), -1);
    sd->box = num(tj_get(a, "box"), -1);
    if (!tj_is(scr, TJ_ARR) || scr->n != sd->layout.nscreen) return 0;
    for (c = scr->first; c; c = c->next, k++) {
        WIKISIDESCREEN* s = &sd->screen[k];
        const TJV *p = tj_get(c, "panel"), *pics = tj_get(c, "pics"), *q;
        if ((s->has_panel = tj_is(p, TJ_OBJ))) {
            if (!frame(tj_get(p, "f"), &s->panel)) return 0;
            s->panel_key = num(tj_get(p, "key"), -1);
        }
        if (!tj_is(pics, TJ_ARR) || pics->n != sd->layout.screen[k].ngadget) return 0;
        for (i = 0, q = pics->first; q; q = q->next, i++) {
            WIKISIDEPIC* pic = &sd->screen[k].pic[i];
            const TJV *fr = tj_get(q, "frames"), *f;
            const char* text = tj_str(tj_get(q, "text"), "");
            pic->from = num(tj_get(q, "from"), WIKISIDE_FROM_LABEL);
            pic->key = num(tj_get(q, "key"), -1);
            pic->attribs = num(tj_get(q, "attribs"), 0);
            if (strlen(text) >= sizeof pic->text) return 0;
            strcpy(pic->text, text);
            if (!tj_is(fr, TJ_ARR) || fr->n > 3) return 0;
            for (f = fr->first; f; f = f->next)
                if (!frame(f, &pic->frame[pic->nframe++])) return 0;
        }
    }
    return 1;
}

static WIKISIDE* load(const char* dir, unsigned char** sheet)
{
    char err[256];
    size_t n, nart, nsheet, npal, nseen, ngrey;
    unsigned char *text = file_in(dir, "layout.json", &n), *atext = file_in(dir, "art.json", &nart);
    unsigned char *pal = file_in(dir, "pal.bin", &npal), *seen = file_in(dir, "seen.bin", &nseen);
    unsigned char* grey = file_in(dir, "greyed.bin", &ngrey);
    WIKISIDE* sd = (WIKISIDE*)calloc(1, sizeof *sd);
    WIKILAYOUT* lay = NULL;
    TJDOC* doc = NULL;
    const TJV* sh;
    int ok = 0, i;
    *sheet = file_in(dir, "sheet.bin", &nsheet);
    if (!sd || !text || !atext || !*sheet || !pal || npal != 1024) { fprintf(stderr, "wikisidetest: %s: a file is missing\n", dir); goto done; }
    if (!(lay = tagpu_wikidata_layout((const char*)text, n, err, sizeof err))) {
        fprintf(stderr, "wikisidetest: %s/layout.json: %s\n", dir, err);
        goto done;
    }
    sd->layout = *lay;
    if (!(doc = tagpu_json_parse((const char*)atext, nart, err, sizeof err))) {
        fprintf(stderr, "wikisidetest: %s/art.json: %s\n", dir, err);
        goto done;
    }
    sh = tj_get(tagpu_json_root(doc), "sheet");
    sd->sheet_w = num(tj_at(sh, 0), 0);
    sd->sheet_h = num(tj_at(sh, 1), 0);
    if (sd->sheet_w <= 0 || sd->sheet_h <= 0 || (size_t)sd->sheet_w * (size_t)sd->sheet_h != nsheet) {
        fprintf(stderr, "wikisidetest: %s: the sheet is not w x h bytes\n", dir);
        goto done;
    }
    sd->sheet = *sheet;
    memcpy(sd->pal, pal, sizeof sd->pal);
    for (i = 0; i < 256; i++) sd->seen[i] = (unsigned char)i;
    if (seen && nseen == 256) memcpy(sd->seen, seen, 256);
    if ((sd->has_greyed = grey && ngrey == 256)) memcpy(sd->greyed, grey, 256);
    if (!art(sd, tagpu_json_root(doc))) {
        fprintf(stderr, "wikisidetest: %s/art.json does not fit the layout or WIKISIDE\n", dir);
        goto done;
    }
    ok = 1;
done:
    free(text);
    free(atext);
    free(pal);
    free(seen);
    free(grey);
    free(lay);
    if (doc) tagpu_json_free(doc);
    if (!ok) {
        free(sd);
        return NULL;
    }
    return sd;
}

static void put32(int v)
{
    const unsigned u = (unsigned)v;
    const unsigned char b[4] = { (unsigned char)u, (unsigned char)(u >> 8), (unsigned char)(u >> 16), (unsigned char)(u >> 24) };
    fwrite(b, 1, 4, stdout);
}

static void key(const char* e, const char* d, int t)
{
    char k[256];
    const int n = snprintf(k, sizeof k, "%s/%s/%d", e, d, t);
    const unsigned char b[2] = { (unsigned char)n, (unsigned char)(n >> 8) };
    fwrite(b, 1, 2, stdout);
    fwrite(k, 1, (size_t)n, stdout);
}

/* sidecheck.mjs `sampled`: every 30th tick and the last, every tick the routed screen or a
   gadget's state differs from the tick before's (`prev`, its `n` -1 when the tick before had no
   sidebar), and every 10th tick a unit is under the cursor (the minimap's high dot) */
typedef struct { int n, screen; int state[WIKI_SIDE_GADGETS]; } STATES;

static int sampled(const WIKIVIEW* v, const STATES* prev, int tick, int done)
{
    if (tick % 30 == 0 || done) return 1;
    if (!v->has_side) return 0;
    if (prev->n < 0 || prev->screen != v->side.screen || prev->n != v->side.nstate ||
        memcmp(prev->state, v->side.state, sizeof(int) * (size_t)prev->n))
        return 1;
    return v->side.under[0] && tick % 10 == 0;
}

/* CTX's context into `ctx`, its casting into `c`; 1, else 0 with a message */
static int context(const char* path, WIKISIMCTX* ctx, WIKICASTING* c)
{
    char err[256];
    size_t n;
    unsigned char* text = slurp(path, &n);
    TJDOC* doc = text ? tagpu_json_parse((const char*)text, n, err, sizeof err) : NULL;
    const TJV *r, *cast;
    int ok = 0;
    if (!doc) { fprintf(stderr, "wikisidetest: %s: %s\n", path, text ? err : "unreadable"); goto done; }
    r = tagpu_json_root(doc);
    ctx->side = num(tj_get(r, "side"), 0);
    ctx->iface = num(tj_get(r, "iface"), 1);
    ctx->cast = NULL;
    cast = tj_get(r, "casting");
    if (cast && !tj_is(cast, TJ_NULL)) {
        if (!tagpu_wikidata_casting(cast, c, err, sizeof err)) { fprintf(stderr, "wikisidetest: %s: the casting: %s\n", path, err); goto done; }
        ctx->cast = c;
    }
    ok = 1;
done:
    if (doc) tagpu_json_free(doc);
    free(text);
    return ok;
}

static int compose(char** a, const char* ctxfile)
{
    static WIKICASTING casting;
    WIKISIMCTX ctx = { 0, NULL, 1, NULL };
    static WIKISTAGEVIEW v;
    static WIKISIDECOL col;
    static unsigned char rgba[WIKI_SIDE_W * WIKI_SIDE_ROWS * 4];
    char err[256];
    size_t npak;
    unsigned char *pak = slurp(a[0], &npak), *sheet = NULL;
    WIKIDATA* d;
    WIKISIDE* sd;
    int i, j;
    if (!pak) { fprintf(stderr, "wikisidetest: cannot read %s\n", a[0]); return 2; }
    if (!(d = tagpu_wikidata_load(pak, npak, err, sizeof err))) {
        fprintf(stderr, "wikisidetest: %s\n", err);
        return 2;
    }
    if (!(sd = load(a[1], &sheet))) return 2;
    if (ctxfile && !context(ctxfile, &ctx, &casting)) return 2;
    for (i = 0; i < d->nentry; i++) {
        const WIKIENTRY* e = &d->entry[i];
        char name[256];
        size_t nbase;
        unsigned char* base;
        snprintf(name, sizeof name, "bases/%s.bin", e->id);
        base = file_in(a[1], name, &nbase);
        for (j = 0; j < e->ndemo; j++) {
            WIKISIM s;
            if (!tagpu_wikisim_init(&s, e, &e->demo[j], 1, &sd->layout, ctxfile ? &ctx : NULL, err, sizeof err)) {
                key(e->id, e->demo[j].id, 0);
                put32(-1);
                put32(0);
                continue;
            }
            STATES prev, was;
            prev.n = -1;
            for (;;) {
                const WIKIVIEW* w = tagpu_wikisim_view(&s);
                was = prev;
                prev.n = -1;
                if (w->has_side && w->side.nstate >= 0 && w->side.nstate <= WIKI_SIDE_GADGETS) {
                    prev.n = w->side.nstate;
                    prev.screen = w->side.screen;
                    memcpy(prev.state, w->side.state, sizeof(int) * (size_t)prev.n);
                }
                if (sampled(w, &was, s.tick, s.done)) {
                    int h;
                    tagpu_wikistage_copy(&v, w, s.entry, WIKISTAGE_LAYOUT_MOD);
                    h = tagpu_wikiside_compose(sd, &v, v.side.index, base, base ? (int)nbase : 0, &col);
                    key(e->id, e->demo[j].id, s.tick);
                    put32(h);
                    put32(col.minimap);
                    if (h > 0) {
                        tagpu_wikiside_rgba(sd, &col, rgba);
                        fwrite(col.idx, 1, (size_t)WIKI_SIDE_W * (size_t)h, stdout);
                        fwrite(col.a, 1, (size_t)WIKI_SIDE_W * (size_t)h, stdout);
                        fwrite(rgba, 1, (size_t)WIKI_SIDE_W * (size_t)h * 4, stdout);
                    }
                }
                if (s.done) break;
                tagpu_wikisim_step(&s);
            }
            tagpu_wikisim_free(&s);
        }
        free(base);
    }
    tagpu_wikidata_free(d);
    free(sd);
    free(sheet);
    free(pak);
    return fflush(stdout) ? 2 : 0;
}

int main(int argc, char** argv)
{
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "compose")) return compose(argv + 2, argc == 5 ? argv[4] : NULL);
    fprintf(stderr, "usage: wikisidetest compose PAK DIR [CTX]\n");
    return 2;
}
