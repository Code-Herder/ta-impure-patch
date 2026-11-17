/* wikitechtest.c -- tagpu_wikitech.c compiled natively for tools/wiki/techcheck.mjs.

   `wikitechtest measure FONT SPACING UPPER FILE`: FILE's lines, each a string, measured in the
   face FONT at a size of one, one line out each: the advance a size multiplies, which the
   recording's measureText gives techtree.js.

   `wikitechtest draw TREE PICS CASES TITLE BODY LABEL STYLES`: the tree of a tech tree pack
   (gfx/side/<mod>/techtree/tree.json, which tools/wiki/assetcheck.py --side proves equal to the
   game's own load) made into a WIKITECH as the loader makes one -- each title and description
   back in the code page it was read from, each picture's size from its PNG under PICS -- and for
   every case of CASES (techcheck.mjs `cases`) the tree laid out and drawn
   (tagpu_wikitech_tree, _fit, _draw), measured in the three faces TITLE, BODY and LABEL
   (`path:spacing:upper`) with the style indices STYLES (`t,b,l`). One JSON line a case:
   {i, x0, y0, w, h, cam, dropped, ops: [[kind, flags, part, x, y, w, h, cx, cy, cw, ch, rgba, size,
   style, centre, text, rgb], ...], hits: [[unit, card, icon], ...]}, the ops in units, each hit
   tagpu_wikitech_hit at the case's tree point of the same index (order indices, -1 nothing).

   Exit 0 when everything loaded and ran; the comparison is techcheck.mjs's. */
#define STB_TRUETYPE_IMPLEMENTATION
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hudtext.h"
#include "tagpu_json.h"
#include "tagpu_wikitech.h"

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

typedef struct { stbtt_fontinfo f; unsigned char* ttf; float spacing; int upper; } FACE;

static int face(FACE* f, const char* spec)
{
    char path[1024];
    const char *a = strchr(spec, ':'), *b = a ? strchr(a + 1, ':') : NULL;
    size_t n;
    unsigned char* ttf;
    if (!a || !b || (size_t)(a - spec) >= sizeof path) { fprintf(stderr, "wikitechtest: a face is path:spacing:upper, not %s\n", spec); return 0; }
    memcpy(path, spec, (size_t)(a - spec));
    path[a - spec] = 0;
    if (!(ttf = (unsigned char*)slurp(path, &n)) || !stbtt_InitFont(&f->f, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        fprintf(stderr, "wikitechtest: cannot read the face %s\n", path);
        return 0;
    }
    f->spacing = (float)atof(a + 1);
    f->upper = atoi(b + 1);
    f->ttf = ttf;
    return 1;
}

static FACE s_face[WT_ROLES];

static double measure(void* ctx, int role, double size, const char* s)
{
    (void)ctx;
    return tagpu_hudtext_width(&s_face[role].f, (float)size, s_face[role].spacing, s_face[role].upper, s, strlen(s));
}

static int measure_file(char** a)
{
    FACE f;
    char spec[1200], *text, *line, *nl;
    size_t n;
    snprintf(spec, sizeof spec, "%s:%s:%s", a[0], a[1], a[2]);
    if (!face(&f, spec) || !(text = slurp(a[3], &n))) return 2;
    for (line = text; *line; line = nl + 1) {
        if (!(nl = strchr(line, '\n'))) break;
        *nl = 0;
        printf("%.17g\n", (double)tagpu_hudtext_width(&f.f, 1.0f, f.spacing, f.upper, line, strlen(line)));
    }
    free(text);
    free(f.ttf);
    return 0;
}

/* a UTF-8 string back in its code page: `cp1252` a title's (gfx.py display_name), else Latin-1 */
static void bytes_of(unsigned char* d, size_t n, const char* s, int cp1252)
{
    static const uint16_t HI[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161,
        0x203A, 0x0153, 0, 0x017E, 0x0178 };
    size_t o = 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p && o + 1 < n) {
        unsigned v = *p++;
        int k;
        if (v >= 0xE0) { v = (v & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F); p += 2; }
        else if (v >= 0xC0) { v = (v & 0x1F) << 6 | (p[0] & 0x3F); p++; }
        if (cp1252 && v > 0xFF) for (k = 0; k < 32; k++) if (HI[k] == v) { v = 0x80 + (unsigned)k; break; }
        d[o++] = (unsigned char)(v > 0xFF ? '?' : v);
    }
    d[o] = 0;
}

static int png_size(const char* path, int* w, int* h)
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

static const TJV* member(const TJV* o, const char* k) { return tj_get(o, k); }

static int draw(char** a)
{
    char err[256], path[2048];
    size_t n;
    char *tt, *ct;
    TJDOC *td, *cd;
    const TJV *T, *U, *SD, *C, *v;
    WIKITECH* t;
    WIKITECHUNIT* units;
    WIKITECHPIC* pics;
    uint16_t *order, *mcount, *menu;
    uint32_t* mfirst;
    int nu = 0, i, ci;
    uint32_t no = 0, nm = 0;
    WTART art;
    static WIKISTAGEOPS o;
    for (i = 0; i < WT_ROLES; i++) if (!face(&s_face[i], a[3 + i])) return 2;
    memset(&art, 0, sizeof art);
    if (sscanf(a[6], "%hd,%hd,%hd", &art.style[0], &art.style[1], &art.style[2]) != 3) return 2;
    art.measure = measure;
    if (!(tt = slurp(a[0], &n)) || !(td = tagpu_json_parse(tt, n, err, sizeof err))) { fprintf(stderr, "wikitechtest: %s: %s\n", a[0], err); return 2; }
    if (!(ct = slurp(a[2], &n)) || !(cd = tagpu_json_parse(ct, n, err, sizeof err))) { fprintf(stderr, "wikitechtest: %s: %s\n", a[2], err); return 2; }
    T = tagpu_json_root(td);
    U = member(T, "units");
    SD = member(T, "sides");
    /* THE UNITS in the file's order, each its row index; then every side's walk and menus */
    if (!(t = calloc(1, sizeof *t)) || !(units = calloc((size_t)U->n + 1, sizeof *units)) || !(pics = calloc((size_t)U->n + 1, sizeof *pics)))
        return 2;
    for (v = U->first; v; v = v->next, nu++) {
        WIKITECHUNIT* x = &units[nu];
        const TJV *fp = member(v, "fp"), *st = member(v, "storage"), *ws = member(v, "weapons"), *w;
        const char* cat = tj_str(member(v, "category"), "");
        size_t k;
        int pw, ph;
        snprintf(x->name, sizeof x->name, "%s", tj_str(member(v, "name"), ""));
        bytes_of(x->title, sizeof x->title, tj_str(member(v, "title"), ""), 1);
        if (!strcmp((const char*)x->title, x->name)) x->title[0] = 0;
        bytes_of(x->desc, sizeof x->desc, tj_str(member(v, "description"), ""), 0);
        x->structure = tj_is(member(v, "structure"), TJ_TRUE);
        x->mobile = tj_is(member(v, "mobile"), TJ_TRUE);
        x->builder = tj_is(member(v, "builder"), TJ_TRUE);
        x->canfly = tj_is(member(v, "canfly"), TJ_TRUE);
        x->water = tj_is(member(v, "water"), TJ_TRUE);
        x->cloaked = tj_is(member(v, "cloaked"), TJ_TRUE);
        x->stealth = tj_is(member(v, "stealth"), TJ_TRUE);
        x->hidedamage = tj_is(member(v, "hidedamage"), TJ_TRUE);
        x->level = -1;
        for (k = 0; cat[k]; k++) {
            size_t q;
            for (q = 0; q < 5 && cat[k + q] && (cat[k + q] | 0x20) == "level"[q]; q++) {}
            if (q == 5 && cat[k + 5] >= '0' && cat[k + 5] <= '9') { x->level = (int8_t)(cat[k + 5] - '0'); break; }
        }
        x->fx = (int32_t)tj_num(tj_at(fp, 0), 0);
        x->fz = (int32_t)tj_num(tj_at(fp, 1), 0);
        x->health = (int32_t)tj_num(member(v, "health"), 0);
        x->metal = tj_num(member(v, "metal"), 0);
        x->energy = tj_num(member(v, "energy"), 0);
        x->buildtime = tj_num(member(v, "buildtime"), 0);
        x->energymake = tj_num(member(v, "energymake"), 0);
        x->metalmake = tj_num(member(v, "metalmake"), 0);
        x->extracts = tj_num(member(v, "extracts"), 0);
        x->mstore = tj_num(tj_at(st, 0), 0);
        x->estore = tj_num(tj_at(st, 1), 0);
        for (w = ws ? ws->first : NULL; w && x->nweapon < WIKITECH_WEAPONS; w = w->next) x->nweapon++;
        x->pic = -1;
        if (tj_is(member(v, "pic"), TJ_STR)) {
            snprintf(path, sizeof path, "%s/%s", a[1], tj_str(member(v, "pic"), ""));
            if (!png_size(path, &pw, &ph)) { fprintf(stderr, "wikitechtest: cannot read %s\n", path); return 2; }
            pics[t->npic].w = pw;
            pics[t->npic].h = ph;
            pics[t->npic].tkey = -1;
            x->pic = t->npic++;
        }
    }
    for (v = SD->first; v; v = v->next) {
        const TJV* od = member(v, "order");
        no += (uint32_t)od->n;
    }
    {
        const TJV* s;
        uint32_t cap = 0;
        for (s = SD->first; s; s = s->next) {
            const TJV* m;
            for (m = member(s, "menus")->first; m; m = m->next) cap += (uint32_t)m->n;
        }
        if (!(order = calloc(no + 1, 2)) || !(mfirst = calloc(no + 1, 4)) || !(mcount = calloc(no + 1, 2)) || !(menu = calloc(cap + 1, 2)))
            return 2;
    }
    {
        const TJV* s;
        uint32_t at = 0;
        for (s = SD->first; s; s = s->next) {
            WIKITECHSIDE* sd = &t->side[t->nside++];
            const TJV *od = member(s, "order"), *ms = member(s, "menus"), *e;
            snprintf(sd->name, sizeof sd->name, "%s", tj_str(member(s, "name"), ""));
            sd->first = at;
            sd->norder = (uint32_t)od->n;
            for (e = od->first; e; e = e->next, at++) {
                const TJV *k2, *m = member(ms, e->str);
                int u = 0;
                for (k2 = U->first; k2 && strcmp(k2->key, e->str); k2 = k2->next) u++;
                order[at] = (uint16_t)u;
                mfirst[at] = nm;
                mcount[at] = (uint16_t)(m ? m->n : 0);
                for (k2 = m ? m->first : NULL; k2; k2 = k2->next) {
                    const TJV* q;
                    int x2 = 0;
                    for (q = U->first; q && strcmp(q->key, k2->str); q = q->next) x2++;
                    menu[nm++] = (uint16_t)x2;
                }
            }
            sd->commander = order[sd->first];
        }
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
    C = tagpu_json_root(cd);
    for (ci = 0, v = C->first; v; v = v->next, ci++) {
        WIKITECHTREE tr;
        WTCAM cam;
        HUDRECT stage;
        const TJV *sr = member(v, "stage"), *cm = member(v, "cam");
        const double u = tj_num(member(v, "u"), 1);
        double x0, y0, w, h;
        int sel = -1, j;
        stage.x = (float)tj_num(tj_at(sr, 0), 0); stage.y = (float)tj_num(tj_at(sr, 1), 0);
        stage.w = (float)tj_num(tj_at(sr, 2), 0); stage.h = (float)tj_num(tj_at(sr, 3), 0);
        if (!tagpu_wikitech_tree(t, (int)tj_num(member(v, "side"), 0), &tr)) { fprintf(stderr, "wikitechtest: case %d made no tree\n", ci); return 1; }
        tagpu_wikistage_canvas(stage, u, &x0, &y0, &w, &h);
        if (tj_is(cm, TJ_OBJ)) { cam.z = tj_num(member(cm, "z"), 1); cam.x = tj_num(member(cm, "x"), 0); cam.y = tj_num(member(cm, "y"), 0); }
        else tagpu_wikitech_fit(&tr, w, h, u, &cam);
        if (tj_is(member(v, "sel"), TJ_STR))
            for (j = 0; j < tr.n; j++) {
                char low[WIKITECH_NAME];
                size_t q;
                for (q = 0; q + 1 < sizeof low && t->unit[tr.unit[j]].name[q]; q++) low[q] = (char)(t->unit[tr.unit[j]].name[q] | (t->unit[tr.unit[j]].name[q] >= 'A' && t->unit[tr.unit[j]].name[q] <= 'Z' ? 0x20 : 0));
                low[q] = 0;
                if (!strcmp(low, tj_str(member(v, "sel"), ""))) sel = j;
            }
        tagpu_wikitech_draw(&tr, &cam, sel, tj_is(member(v, "all"), TJ_TRUE), &art, stage, u, &o);
        printf("{\"i\":%d,\"x0\":%.17g,\"y0\":%.17g,\"w\":%.17g,\"h\":%.17g,\"cam\":[%.17g,%.17g,%.17g],\"dropped\":%d,\"ops\":[",
               ci, x0, y0, w, h, cam.z, cam.x, cam.y, o.dropped);
        for (j = 0; j < o.n; j++) {
            const HUDOP* p = &o.op[j];
            printf("%s[%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%.9g,%d,%d,", j ? "," : "", p->kind, p->flags, p->part,
                   p->r.x, p->r.y, p->r.w, p->r.h, p->clip.x, p->clip.y, p->clip.w, p->clip.h, p->rgba, p->size, p->style, p->centre);
            if (p->kind == HOP_TEXT && p->nrun) {
                const HUDRUN* r = &o.run[p->run];
                const char* s = o.text + r->off;
                putchar('"');
                for (; *s; s++) if (*s == '"' || *s == '\\') printf("\\%c", *s); else putchar(*s);
                printf("\",%u]", r->rgb);
            } else printf("null,0]");
        }
        /* the hit test at each of the case's tree points: [unit, card, icon] in order indices, -1 none */
        printf("],\"hits\":[");
        {
            const TJV* hp = member(v, "hits");
            const TJV* pt;
            int first = 1;
            for (pt = hp ? hp->first : NULL; pt; pt = pt->next, first = 0) {
                int hu = -1, hc = -1, hi = 0;
                if (!tagpu_wikitech_hit(&tr, tj_num(tj_at(pt, 0), 0), tj_num(tj_at(pt, 1), 0), &hu, &hc, &hi)) hu = hc = -1;
                printf("%s[%d,%d,%d]", first ? "" : ",", hu, hc, hi);
            }
        }
        printf("]}\n");
        tagpu_wikitech_free(&tr);
    }
    tagpu_json_free(td);
    tagpu_json_free(cd);
    free(tt); free(ct); free(t); free(units); free(pics); free(order); free(mfirst); free(mcount); free(menu);
    for (i = 0; i < WT_ROLES; i++) free(s_face[i].ttf);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 6 && !strcmp(argv[1], "measure")) return measure_file(argv + 2);
    if (argc == 9 && !strcmp(argv[1], "draw")) return draw(argv + 2);
    fprintf(stderr, "usage: wikitechtest measure FONT SPACING UPPER FILE\n"
                    "       wikitechtest draw TREE PICS CASES TITLE BODY LABEL STYLES\n");
    return 2;
}
