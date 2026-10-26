/* wikistagetest.c -- the C half of the wiki stage's check, for tools/wiki/stagecheck.mjs: the real
   tagpu_wikisim.c, tagpu_wikistage.c and tagpu_hudtext.c, compiled natively, write the stage's ops
   as JSON lines, and stagecheck.mjs pairs them with stage.js's canvas calls.

   `wikistagetest measure FONT SPACING UPPER FILE`: FILE's lines, each a string, measured in the
   face FILE with `spacing` em of letter-spacing (upper-cased when UPPER is 1) at a size of one,
   one line out each: the advance a size multiplies, which stage.js's recorded measureText gives.

   `wikistagetest ops PAK FONT SPACING UPPER STYLE ARROW MOVE BUILD AW AH MW MH BW BH X Y W H U`
   plays every demo of every entry in PAK (tools/hudpak.py --set wiki) to its end, and at tick 0,
   every 30th tick and the last copies the view (tagpu_wikistage_copy) and builds the stage on the
   rect X Y W H, in units, at U device pixels a unit: the text in style STYLE, measured in FONT;
   the cursors the kit parts ARROW, MOVE and BUILD, AW x AH, MW x MH and BW x BH units. One line a
   frame: {"e", "d", "t", "x0", "y0", "w", "h", "dropped", "trunc", "ops": [[kind, flags, part,
   x, y, w, h, clip x, y, w, h, rgba, size, style, centre, text], ...]}, every position in units
   as the op holds it.

   Exit 0 when everything loaded and ran; the comparison is stagecheck.mjs's. */
#define STB_TRUETYPE_IMPLEMENTATION
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hudtext.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikisim.h"
#include "tagpu_wikistage.h"

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

typedef struct { stbtt_fontinfo f; unsigned char* ttf; float spacing; int upper; } FACE;   /* the face reads ttf */

static double measure(void* ctx, double size, const char* s)
{
    const FACE* f = (const FACE*)ctx;
    return tagpu_hudtext_width(&f->f, (float)size, f->spacing, f->upper, s, strlen(s));
}

static int face(FACE* f, const char* path, const char* spacing, const char* upper)
{
    size_t n;
    unsigned char* ttf = (unsigned char*)slurp(path, &n);
    if (!ttf || !stbtt_InitFont(&f->f, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        fprintf(stderr, "wikistagetest: cannot read the face %s\n", path);
        return 0;
    }
    f->spacing = (float)atof(spacing);
    f->upper = atoi(upper);
    f->ttf = ttf;
    return 1;
}

static void str(const char* s, size_t n)
{
    size_t i;
    putchar('"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void frame(const WIKISTAGEOPS* o, const WIKISTAGEVIEW* v, const char* e, const char* d, int t,
                  HUDRECT r, double u)
{
    double x0, y0, w, h;
    int i;
    tagpu_wikistage_canvas(r, u, &x0, &y0, &w, &h);
    printf("{\"e\":\"%s\",\"d\":\"%s\",\"t\":%d,\"x0\":%.17g,\"y0\":%.17g,\"w\":%.17g,\"h\":%.17g,"
           "\"dropped\":%d,\"trunc\":%u,\"ops\":[", e, d, t, x0, y0, w, h, o->dropped, (unsigned)v->trunc);
    for (i = 0; i < o->n; i++) {
        const HUDOP* p = &o->op[i];
        printf("%s[%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%.9g,%d,%d,", i ? "," : "", p->kind, p->flags,
               p->part, p->r.x, p->r.y, p->r.w, p->r.h, p->clip.x, p->clip.y, p->clip.w, p->clip.h,
               (unsigned)p->rgba, p->size, p->style, p->centre);
        /* the stage's text ops are one run each: its text and its colour */
        if (p->kind == HOP_TEXT && p->nrun == 1) {
            const HUDRUN* run = &o->run[p->run];
            str(o->text + run->off, run->len);
            printf(",%u]", (unsigned)run->rgb);
        } else
            fputs("null,0]", stdout);
    }
    fputs("]}\n", stdout);
}

static int ops(char** a)
{
    static WIKISTAGEVIEW v;
    static WIKISTAGEOPS o;
    char err[256];
    size_t n;
    char* pak = slurp(a[0], &n);
    WIKIDATA* d;
    WIKISTAGEART art;
    FACE f;
    HUDRECT r;
    double u;
    int i, j, k;
    if (!pak) { fprintf(stderr, "wikistagetest: cannot read %s\n", a[0]); return 2; }
    if (!face(&f, a[1], a[2], a[3])) return 2;
    if (!(d = tagpu_wikidata_load((const unsigned char*)pak, n, err, sizeof err))) {
        fprintf(stderr, "wikistagetest: %s\n", err);
        return 2;
    }
    memset(&art, 0, sizeof art);
    art.style = (int16_t)atoi(a[4]);
    art.measure = measure;
    art.measure_ctx = &f;
    for (k = 0; k < 3; k++) {
        art.cursor[k] = (int16_t)atoi(a[5 + k]);
        art.cursor_w[k] = (float)atof(a[8 + 2 * k]);
        art.cursor_h[k] = (float)atof(a[9 + 2 * k]);
    }
    r.x = (float)atof(a[14]); r.y = (float)atof(a[15]); r.w = (float)atof(a[16]); r.h = (float)atof(a[17]);
    u = atof(a[18]);
    for (i = 0; i < d->nentry; i++)
        for (j = 0; j < d->entry[i].ndemo; j++) {
            const WIKIENTRY* e = &d->entry[i];
            WIKISIM s;
            if (!tagpu_wikisim_init(&s, e, &e->demo[j], 1, err, sizeof err)) {
                fprintf(stderr, "wikistagetest: %s/%s: %s\n", e->id, e->demo[j].id, err);
                return 2;
            }
            for (;;) {
                if (s.tick % 30 == 0 || s.done) {
                    tagpu_wikistage_copy(&v, tagpu_wikisim_view(&s), e);
                    tagpu_wikistage_build(&v, NULL, r, u, &art, &o);
                    frame(&o, &v, e->id, e->demo[j].id, s.tick, r, u);
                }
                if (s.done) break;
                tagpu_wikisim_step(&s);
            }
            tagpu_wikisim_free(&s);
        }
    tagpu_wikidata_free(d);
    free(pak);
    free(f.ttf);
    return 0;
}

static int measure_file(char** a)
{
    FACE f;
    size_t n;
    char* text;
    char* line;
    if (!face(&f, a[0], a[1], a[2])) return 2;
    if (!(text = slurp(a[3], &n))) { fprintf(stderr, "wikistagetest: cannot read %s\n", a[3]); return 2; }
    for (line = strtok(text, "\n"); line; line = strtok(NULL, "\n"))
        printf("%.17g\n", (double)tagpu_hudtext_width(&f.f, 1.0f, f.spacing, f.upper, line, strlen(line)));
    free(text);
    free(f.ttf);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 6 && !strcmp(argv[1], "measure")) return measure_file(argv + 2);
    if (argc == 21 && !strcmp(argv[1], "ops")) return ops(argv + 2);
    fprintf(stderr, "usage: wikistagetest measure FONT SPACING UPPER FILE\n"
                    "       wikistagetest ops PAK FONT SPACING UPPER STYLE ARROW MOVE BUILD AW AH MW MH BW BH X Y W H U\n");
    return 2;
}
