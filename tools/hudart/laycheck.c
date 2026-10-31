/* laycheck.c -- tagpu_hudlay's rectangles for a state the mockup dumped, as JSON on stdout.

     laycheck <art/hud dir> <state.json> [<CSS px a unit> <device px a unit>]

   With the two scales it also gives each text's baseline, in CSS pixels as the browser lays it
   out, each fill's clip as the draw takes it, at least one device pixel wide, the device pixels each
   part, solid, dot and clip is snapped to, and each grid or 9-slice part's pieces in device pixels
   from its snapped corner (tagpu_hudlay_expand). The state is the mockup's own (index.html `state()`), so the two sides lay out the same
   game; tools/hudart/laycheck.py compares this output with the mockup's element boxes.
   Compiled natively by laycheck.py: tagpu_json.c, tagpu_hudlay.c and tagpu_hudtext.c are pure
   C99 for this, and stb_truetype is built here, so text the layout sizes to itself is measured
   in its faces by the game's own rule. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_hudlay.h"
#include "tagpu_hudtext.h"
#include "hudstatejson.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    char* b;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char*)malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[len] = 0; *n = (size_t)len; }
    return b;
}

/* a PNG's size from its IHDR, which is always the first chunk */
static int png_size(void* ctx, const char* part, int* w, int* h)
{
    char path[1024];
    unsigned char b[24];
    FILE* f;
    snprintf(path, sizeof path, "%s/parts/%s.png", (const char*)ctx, part);
    if (!(f = fopen(path, "rb"))) return 0;
    if (fread(b, 1, 24, f) != 24 || memcmp(b + 12, "IHDR", 4)) { fclose(f); return 0; }
    fclose(f);
    *w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    *h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    return 1;
}

/* each style's face, as tagpu_hudart.c faces() opens it: fonts/<family, lower case, no
   spaces>-<weight>.ttf */
static stbtt_fontinfo s_face[24];
static int s_faceOk[24];

static void faces(const HUDLAY* l, const char* dir)
{
    const HUDGLOBALS* g = tagpu_hudlay_globals(l);
    int i;
    for (i = 0; i < g->nstyle && i < 24; i++) {
        char path[1024], fam[32];
        const char* s;
        size_t n;
        int k = 0;
        unsigned char* b;
        for (s = g->style[i].font; *s && k + 1 < (int)sizeof fam; s++)
            if (*s != ' ') fam[k++] = (char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
        fam[k] = 0;
        snprintf(path, sizeof path, "%s/fonts/%s-%d.ttf", dir, fam, g->style[i].weight);
        if ((b = (unsigned char*)slurp(path, &n)))
            s_faceOk[i] = stbtt_InitFont(&s_face[i], b, stbtt_GetFontOffsetForIndex(b, 0));
    }
}

static double measure(void* ctx, int style, double size, const char* s)
{
    const HUDGLOBALS* g = tagpu_hudlay_globals((const HUDLAY*)ctx);
    if (style < 0 || style >= 24 || !s_faceOk[style]) return 0;
    return tagpu_hudtext_width(&s_face[style], (float)size, g->style[style].spacing, g->style[style].upper,
                               s, strlen(s));
}

static void json_str(const char* s, int n)
{
    int i;
    putchar('"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

/* the opacity an op is painted with: its own times its group's, as the mockup's is its own times
   every ancestor's */
static unsigned painted(const HUDOPS* o, const HUDOP* p)
{
    double a = (double)(p->rgba & 0xFF);
    if (p->grp) a *= o->grp[p->grp - 1];
    return (unsigned)(a + 0.5);
}

int main(int argc, char** argv)
{
    char path[1024], err[256];
    size_t nl, nk, ns;
    char *lt, *kt, *stt;
    TJDOC* sd;
    const TJV* S;
    HUDLAY* l;
    HUDSTATE* st = (HUDSTATE*)calloc(1, sizeof(HUDSTATE));
    HUDOPS* o = (HUDOPS*)malloc(sizeof(HUDOPS));
    int i, j;

    if (argc != 3 && argc != 5) { fprintf(stderr, "usage: laycheck <art/hud dir> <state.json> [<CSS px a unit> <device px a unit>]\n"); return 2; }
    snprintf(path, sizeof path, "%s/layout.json", argv[1]);
    if (!(lt = slurp(path, &nl))) { fprintf(stderr, "laycheck: cannot read %s\n", path); return 2; }
    snprintf(path, sizeof path, "%s/kit.json", argv[1]);
    if (!(kt = slurp(path, &nk))) { fprintf(stderr, "laycheck: cannot read %s\n", path); return 2; }
    if (!(l = tagpu_hudlay_load(lt, nl, kt, nk, png_size, argv[1], err, sizeof err))) { fprintf(stderr, "laycheck: %s\n", err); return 1; }
    faces(l, argv[1]);
    tagpu_hudlay_measure(l, measure, l);
    if (!(stt = slurp(argv[2], &ns)) || !(sd = tagpu_json_parse(stt, ns, err, sizeof err))) { fprintf(stderr, "laycheck: state: %s\n", err); return 2; }
    S = tagpu_json_root(sd);
    hudstate_json(S, st);
    /* the fork limit is the mockup's, the layout's proposed.forks.max; the game's is the recorder's */
    {
        TJDOC* ld = tagpu_json_parse(lt, nl, err, sizeof err);
        st->fork_max = ld ? (int)tj_num(tj_get(tj_get(tj_get(tagpu_json_root(ld), "proposed"), "forks"), "max"), 0) : 0;
        tagpu_json_free(ld);
    }
    /* the hint's hit test snaps at the device pixels a unit, as the mockup's pins its boxes */
    st->px = argc == 5 ? (float)atof(argv[4]) : 1.0f;

    if (!tagpu_hudlay_build(l, st, o, err, sizeof err)) { fprintf(stderr, "laycheck: %s\n", err); return 1; }
    printf("{\"overflow\":%d,\"ops\":[\n", o->overflow);
    for (i = 0; i < o->n; i++) {
        const HUDOP* p = &o->op[i];
        static const char* kinds[] = { "part", "solid", "dot", "text", "hot", "line", "chart" };
        const char* kind = p->kind == HOP_SOLID && (p->flags & HOPF_DROP) ? "shadow"
                         : p->kind == HOP_SOLID && (p->flags & HOPF_RING) ? "ring"
                         : p->kind == HOP_TEXT && (p->flags & HOPF_BASE) ? "svgtext" : kinds[p->kind];
        printf("%s{\"kind\":\"%s\",\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f,\"dim\":%d", i ? ",\n" : "",
               kind, p->r.x, p->r.y, p->r.w, p->r.h, (p->flags & HOPF_DIM) != 0);
        if (p->flags & HOPF_DROP) printf(",\"dy\":%.4f,\"blur\":%.4f", p->size, p->shade);
        if (p->kind == HOP_LINE || p->kind == HOP_CHART) printf(",\"rgba\":\"%08x\",\"width\":%.4f", p->rgba, p->size);
        if (p->kind == HOP_CHART) printf(",\"clip\":[%.4f,%.4f,%.4f,%.4f]", p->clip.x, p->clip.y, p->clip.w, p->clip.h);
        if (p->kind == HOP_PART) {
            printf(",\"part\":"); json_str(tagpu_hudlay_part(l, p->part)->name, (int)strlen(tagpu_hudlay_part(l, p->part)->name));
            printf(",\"alpha\":%u", painted(o, p));
            /* a composed part's pieces from its snapped corner, as hud.js keeps them (`quads`) */
            if (argc == 5 && tagpu_hudlay_part(l, p->part)->kind != HK_TILE) {
                static HUDQUAD q[HUD_MAXQUAD];
                const float dpx = (float)atof(argv[4]);
                const double X = tagpu_hud_snap(p->r.x * dpx), Y = tagpu_hud_snap(p->r.y * dpx);   /* the corner */
                int m = tagpu_hudlay_expand(l, p, dpx, q, HUD_MAXQUAD), j;
                printf(",\"quads\":[");
                for (j = 0; j < m; j++)
                    printf("%s[%.4f,%.4f,%.4f,%.4f,%.6f,%.6f,%.6f,%.6f]", j ? "," : "", q[j].x0 - X, q[j].y0 - Y,
                           q[j].x1 - X, q[j].y1 - Y, q[j].u0, q[j].v0, q[j].u1, q[j].v1);
                printf("]");
            }
        }
        if (p->kind == HOP_SOLID || p->kind == HOP_DOT) printf(",\"rgba\":\"%08x\"", p->rgba);
        /* the device pixels a part, a solid or a dot is drawn on: tagpu_hud_snap of the same float
           products the draw rounds (tagpu_hudlay_expand, tagpu_huddraw.c snap) */
        if (argc == 5 && (p->kind == HOP_PART || p->kind == HOP_DOT || (p->kind == HOP_SOLID && !(p->flags & HOPF_DROP)))) {
            const float dpx = (float)atof(argv[4]);
            printf(",\"snap\":[%.0f,%.0f,%.0f,%.0f]", tagpu_hud_snap(p->r.x * dpx), tagpu_hud_snap(p->r.y * dpx),
                   tagpu_hud_edge(p->r.x, p->r.w, dpx), tagpu_hud_edge(p->r.y, p->r.h, dpx));
            /* a ring's inner box: the edges its sides share, as the draw computes them */
            if (p->flags & HOPF_RING) {
                float X[4], Y[4];
                tagpu_hud_ring(p, dpx, X, Y);
                printf(",\"isnap\":[%.0f,%.0f,%.0f,%.0f]", X[1], Y[1], X[2], Y[2]);
            }
        }
        if (p->clip.w >= 0 && p->kind != HOP_TEXT && p->kind != HOP_CHART) {
            HUDRECT k = argc == 5 ? tagpu_hudlay_clip(p, (float)atof(argv[4])) : p->clip;
            printf(",\"clip\":[%.4f,%.4f,%.4f,%.4f]", k.x, k.y, k.w, k.h);
            if (argc == 5) {
                const float dpx = (float)atof(argv[4]);
                printf(",\"csnap\":[%.0f,%.0f,%.0f,%.0f]", tagpu_hud_snap(k.x * dpx), tagpu_hud_snap(k.y * dpx),
                       tagpu_hud_edge(k.x, k.w, dpx), tagpu_hud_edge(k.y, k.h, dpx));
            }
        }
        if (p->kind == HOP_TEXT) {
            const char* sn = tagpu_hudlay_globals(l)->style[p->style].name;
            printf(",\"style\":\"%s\",\"size\":%.4f,\"centre\":%d,\"alpha\":%u", sn, p->size, p->centre,
                   painted(o, p));
            if (p->nrun) printf(",\"rgb\":\"%06x\"", o->run[p->run].rgb);
            /* the baseline at `px` a unit, from the face metrics the state carries per style */
            if (argc == 5) {
                const TJV* fm = tj_get(tj_get(tagpu_json_root(sd), "faces"), sn);
                float px = (float)atof(argv[3]);
                if (fm)
                    printf(",\"base\":%.4f", tagpu_hudlay_baseline(p->r.y * px, p->r.h * px, p->size * px,
                                                                  (float)tj_num(tj_at(fm, 0), 0), (float)tj_num(tj_at(fm, 1), 0)) / px);
            }
            printf(",\"text\":");
            putchar('"');
            for (j = 0; j < p->nrun; j++) {
                const HUDRUN* r = &o->run[p->run + j];
                int k;
                for (k = 0; k < r->len; k++) {
                    unsigned char ch = (unsigned char)o->text[r->off + k];
                    if (ch == '"' || ch == '\\') printf("\\%c", ch); else putchar(ch);
                }
            }
            putchar('"');
        }
        if (p->kind == HOP_HOT) { printf(",\"key\":"); json_str(o->text + p->run, (int)strlen(o->text + p->run)); }
        putchar('}');
    }
    printf("\n],\"chart\":{\"n\":%d,\"npt\":%d,\"w\":%.4f,\"halo_w\":%.4f,\"groups\":[", o->chart.n, o->chart.npt,
           o->chart.w, o->chart.halo_w);
    for (i = 0; i < o->chart.n; i++) {
        printf("%s{\"rgb\":\"%06x\",\"dark\":%d,\"cross\":", i ? "," : "", o->chart.g[i].rgb, o->chart.g[i].dark);
        if (o->chart.g[i].cross) printf("[%.4f,%.4f]", o->chart.g[i].cx, o->chart.g[i].cy);
        else printf("null");
        printf(",\"pts\":[");
        for (j = 0; j < o->chart.npt; j++) printf("%s[%.4f,%.4f]", j ? "," : "", o->chart.pt[i][j][0], o->chart.pt[i][j][1]);
        printf("]}");
    }
    printf("]}}\n");
    return 0;
}
