/* wikistagetest.c -- the C half of the wiki stage's check, for tools/wiki/stagecheck.mjs: the real
   tagpu_wikisim.c, tagpu_wikistage.c and tagpu_hudtext.c, compiled natively, write the stage's ops
   as JSON lines, and stagecheck.mjs pairs them with stage.js's canvas calls.

   `wikistagetest measure FONT SPACING UPPER FILE`: FILE's lines, each a string, measured in the
   face FILE with `spacing` em of letter-spacing (upper-cased when UPPER is 1) at a size of one,
   one line out each: the advance a size multiplies, which stage.js's recorded measureText gives.

   `wikistagetest ops PAK FONT SPACING UPPER STYLE ARROW MOVE BUILD AW AH MW MH BW BH SPRITES LAYOUT X Y W H U`
   plays every demo of every entry in PAK (tools/hudpak.py --set wiki) to its end, with the
   sidebar of LAYOUT (a file of PAK, side/fixture.json say; - for none), and at tick 0, every 30th
   tick and the last copies the view (tagpu_wikistage_copy) and builds the stage on the rect
   X Y W H, in units, at U device pixels a unit, with the cursor left out of every frame whose
   tick / 30 is odd (as Try it yourself leaves it while the pointer is off the stage): the text in
   style STYLE, measured in FONT;
   the kit's cursors the parts ARROW, MOVE and BUILD, AW x AH, MW x MH and BW x BH units; the game
   cursors' frames those of SPRITES (`sprites` below). With a layout, the sidebar's art is the
   stand-in `side_art` below, in the frames whose tick / 30 is 1 or 2 past a multiple of 3 (with
   its minimap in the first, without in the second), and the whitebox column in the rest.
   One line a frame: {"e", "d", "t", "x0", "y0", "w", "h", "dropped", "trunc", "cur" (the op
   drawing the cursor, -1 none), "grp" (each group's opacity), "ops": [[kind, flags, part, x, y,
   w, h, clip x, y, w, h, rgba, size, style, centre, text, rgb, shade, grp], ...]}, every position
   in units as the op holds it.

   Exit 0 when everything loaded and ran; the comparison is stagecheck.mjs's. */
#define STB_TRUETYPE_IMPLEMENTATION
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hudtext.h"
#include "tagpu_wikiasset.h"
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
           "\"dropped\":%d,\"trunc\":%u,\"cur\":%d,", e, d, t, x0, y0, w, h, o->dropped, (unsigned)v->trunc,
           o->cursor);
    /* each op's range and, for a box, its type's name (WIKISTAGE_IN_*); then where it is drawn
       (tagpu_wikistage_keep) under three images -- every type resolved with the ground in the image,
       none resolved, and the ground fallen back -- one digit an op; and the image's rect */
    {
        static const uint32_t all[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu }, none[2] = { 0, 0 };
        int x0i, y0i, x1i, y1i, k;
        fputs("\"rng\":[", stdout);
        for (i = 0; i < o->n; i++) {
            printf("%s[%d,", i ? "," : "", o->range[i]);
            if (o->range[i] == WIKISTAGE_IN_BOX) {
                const char* tn = v->type[o->rtype[i] % WIKI_MAX_DEFS];
                str(tn, strlen(tn));
            } else
                fputs("null", stdout);
            putchar(']');
        }
        fputs("],\"keep\":[", stdout);
        for (k = 0; k < 3; k++) {
            printf("%s\"", k ? "," : "");
            for (i = 0; i < o->n; i++) putchar('0' + tagpu_wikistage_keep(o, i, 1, k != 2, k == 1 ? none : all));
            putchar('"');
        }
        tagpu_wikistage_image(v->w, v->h, v->has_side ? v->side.h : 0, r, u, &x0i, &y0i, &x1i, &y1i);
        printf("],\"img\":[%d,%d,%d,%d],\"grp\":[", x0i, y0i, x1i, y1i);
        for (i = 0; i < o->ngrp; i++) printf("%s%.9g", i ? "," : "", o->grp[i]);
        fputs("],", stdout);
    }
    fputs("\"ops\":[", stdout);
    for (i = 0; i < o->n; i++) {
        const HUDOP* p = &o->op[i];
        printf("%s[%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%.9g,%d,%d,", i ? "," : "", p->kind, p->flags,
               p->part, p->r.x, p->r.y, p->r.w, p->r.h, p->clip.x, p->clip.y, p->clip.w, p->clip.h,
               (unsigned)p->rgba, p->size, p->style, p->centre);
        /* the stage's text ops are one run each: its text and its colour */
        if (p->kind == HOP_TEXT && p->nrun == 1) {
            const HUDRUN* run = &o->run[p->run];
            str(o->text + run->off, run->len);
            printf(",%u", (unsigned)run->rgb);
        } else
            fputs("null,0", stdout);
        printf(",%.9g,%d]", p->shade, p->grp);
    }
    fputs("]}\n", stdout);
}

/* The game cursors' frames, stagecheck.mjs's stand-ins: SPRITES is "-" for none (every shape draws
   its kit icon), else a file of whitespace-separated numbers: the scale, three shapes' `n first
   loop` by WIKI_CURSOR_*, then every cell's `w h hx hy dur` */
static int sprites(WIKISTAGEART* art, const char* path)
{
    FILE* f;
    int k, ncell = 0;
    if (!strcmp(path, "-")) return 1;
    if (!(f = fopen(path, "r"))) { fprintf(stderr, "wikistagetest: cannot read %s\n", path); return 0; }
    if (fscanf(f, "%f", &art->spr_scale) != 1) goto bad;
    for (k = 0; k < 3; k++) {
        unsigned n, first, loop;
        if (fscanf(f, "%u %u %u", &n, &first, &loop) != 3 || first + n > HUD_SPRITE_CELLS) goto bad;
        art->seq[k].n = (uint8_t)n;
        art->seq[k].first = (uint8_t)first;
        art->seq[k].loop = (uint8_t)loop;
    }
    for (; ncell < HUD_SPRITE_CELLS; ncell++) {
        int w, h, hx, hy, dur;
        if (fscanf(f, "%d %d %d %d %d", &w, &h, &hx, &hy, &dur) != 5) break;
        art->spr[ncell].w = (int16_t)w;
        art->spr[ncell].h = (int16_t)h;
        art->spr[ncell].hx = (int16_t)hx;
        art->spr[ncell].hy = (int16_t)hy;
        art->spr[ncell].dur = (uint16_t)dur;
    }
    fclose(f);
    return 1;
bad:
    fclose(f);
    fprintf(stderr, "wikistagetest: %s is not a sprite table\n", path);
    return 0;
}

/* THE SIDEBAR'S STAND-IN ART, stagecheck.mjs's `sideArt`: the stage reads of a WIKISIDE only
   whether each gadget's picture is a label and that label's text, so gadget i of screen k is a
   label when (7k + i) % 3 is 0, its text "" (the gadget's name) when k + i is even and "L<k>.<i>"
   when it is odd, and every other gadget a one-frame picture. The layout is a copy of `lay`, the
   simulation runs on that copy, and the view names it as the mod's (WIKISTAGE_LAYOUT_MOD), so
   it resolves to the art's own layout. */
static WIKISIDE* side_art(const WIKILAYOUT* lay)
{
    WIKISIDE* sd = (WIKISIDE*)calloc(1, sizeof *sd);
    int k, i;
    if (!sd) return NULL;
    sd->layout = *lay;
    for (k = 0; k < lay->nscreen; k++)
        for (i = 0; i < lay->screen[k].ngadget; i++) {
            WIKISIDEPIC* p = &sd->screen[k].pic[i];
            if ((7 * k + i) % 3 == 0) {
                p->from = WIKISIDE_FROM_LABEL;
                if ((k + i) % 2) snprintf(p->text, sizeof p->text, "L%d.%d", k, i);
            } else {
                p->from = WIKISIDE_FROM_PAGE;
                p->nframe = 1;
            }
        }
    return sd;
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
    WIKISIDE* sd = NULL;
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
    if (!sprites(&art, a[14])) return 2;
    if (strcmp(a[15], "-")) {
        WIKILAYOUT* lay = tagpu_wikidata_layout_pak((const unsigned char*)pak, n, a[15], err, sizeof err);
        if (!lay || !(sd = side_art(lay))) {
            fprintf(stderr, "wikistagetest: %s: %s\n", a[15], lay ? "no memory" : err);
            return 2;
        }
        free(lay);
    }
    r.x = (float)atof(a[16]); r.y = (float)atof(a[17]); r.w = (float)atof(a[18]); r.h = (float)atof(a[19]);
    u = atof(a[20]);
    for (i = 0; i < d->nentry; i++)
        for (j = 0; j < d->entry[i].ndemo; j++) {
            const WIKIENTRY* e = &d->entry[i];
            WIKISIM s;
            if (!tagpu_wikisim_init(&s, e, &e->demo[j], 1, sd ? &sd->layout : NULL, NULL, err, sizeof err)) {
                fprintf(stderr, "wikistagetest: %s/%s: %s\n", e->id, e->demo[j].id, err);
                return 2;
            }
            for (;;) {
                if (s.tick % 30 == 0 || s.done) {
                    const int phase = (s.tick / 30) % 3;
                    tagpu_wikistage_copy(&v, tagpu_wikisim_view(&s), e, WIKISTAGE_LAYOUT_MOD);
                    v.tick = s.tick;            /* the copy leaves it: tagpu_wiki.c sets it so */
                    art.side_layout = sd ? &sd->layout : NULL;
                    art.side = sd && phase ? sd : NULL;
                    art.side_minimap = phase == 1;
                    tagpu_wikistage_build(&v, NULL, r, u, (s.tick / 30) % 2 == 0, &art, &o);
                    frame(&o, &v, e->id, e->demo[j].id, s.tick, r, u);
                }
                if (s.done) break;
                tagpu_wikisim_step(&s);
            }
            tagpu_wikisim_free(&s);
        }
    tagpu_wikidata_free(d);
    free(sd);
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
    if (argc == 23 && !strcmp(argv[1], "ops")) return ops(argv + 2);
    fprintf(stderr, "usage: wikistagetest measure FONT SPACING UPPER FILE\n"
                    "       wikistagetest ops PAK FONT SPACING UPPER STYLE ARROW MOVE BUILD AW AH MW MH BW BH SPRITES LAYOUT X Y W H U\n");
    return 2;
}
