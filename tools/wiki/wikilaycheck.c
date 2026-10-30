/* wikilaycheck.c -- tagpu_wikilay's ops for a page the mockup dumped, as JSON on stdout; or the
   layout's load-time check over every pack's castings.

     wikilaycheck <art/hud dir> <art/wiki dir> <wikipak.bin> <state.json>
     wikilaycheck --check <art/hud dir> <art/wiki dir> <wikipak.bin> [side.json ...]

   --check runs tagpu_wikilay_check as the game's load does, in the game's faces at their own sizes
   (no browser fallback), with every casting of each side.json given besides the entries' own names
   and the worst castings: exit 0 when the page builds whole and every caption shows whole in its
   strip, 1 with the first failure.

   The data and the layout's numbers are read out of the archive the DLL embeds (tools/hudpak.py
   --set wiki), the kit from art/wiki/kit/ (not packed yet), the text styles and faces from art/hud/
   as tagpu_hudart.c opens them, and each kit part's face lightness from its decoded PNG. The state
   is the mockup's page (wikilaycheck.py COLLECT); its `fallback` gives the browser's advance, in em,
   of each code point the HUD faces lack, by style. Each text op carries its baseline as the draw
   places it (tagpu_hudlay_baseline at the screen's pixels a unit). tools/wiki/wikilaycheck.py
   compiles this with tagpu_wikilay.c, tagpu_hudlay.c, tagpu_hudtext.c, tagpu_wikidata.c,
   tagpu_json.c, tagpu_pak.c and lodepng.c, and compares its output with the mockup's boxes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_hudlay.h"
#include "tagpu_hudtext.h"
#include "tagpu_mod.h"
#include "tagpu_pak.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikilay.h"
#include "lodepng.h"
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

static stbtt_fontinfo s_info[24];
static const TJV* s_fallback;
static const TJV* s_sizes;
static const HUDGLOBALS* s_g;

/* each style's face and its metrics, as tagpu_hudart.c face_open reads them: OS/2's typo
   metrics when the face sets USE_TYPO_METRICS, hhea's otherwise */
static void faces(WIKIFACES* f, const char* dir)
{
    int i;
    for (i = 0; i < s_g->nstyle && i < 24; i++) {
        char path[1024], fam[32];
        const char* s;
        size_t n;
        int k = 0, asc, desc, gap, upem;
        unsigned char* b;
        stbtt_uint32 os2;
        for (s = s_g->style[i].font; *s && k + 1 < (int)sizeof fam; s++)
            if (*s != ' ') fam[k++] = (char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s);
        fam[k] = 0;
        snprintf(path, sizeof path, "%s/fonts/%s-%d.ttf", dir, fam, s_g->style[i].weight);
        if (!(b = (unsigned char*)slurp(path, &n)) || !stbtt_InitFont(&s_info[i], b, stbtt_GetFontOffsetForIndex(b, 0))) continue;
        upem = ttUSHORT(s_info[i].data + s_info[i].head + 18);
        os2 = stbtt__find_table(s_info[i].data, (stbtt_uint32)s_info[i].fontstart, "OS/2");
        if (os2 && (ttUSHORT(s_info[i].data + os2 + 62) & 0x80)) stbtt_GetFontVMetricsOS2(&s_info[i], &asc, &desc, &gap);
        else stbtt_GetFontVMetrics(&s_info[i], &asc, &desc, &gap);
        f->face[i].info = &s_info[i];
        f->face[i].ascent = (float)asc / (float)upem;
        f->face[i].descent = (float)-desc / (float)upem;
    }
}

/* the browser's advance of a code point the faces lack, from the state's `fallback` */
static double missing(void* ctx, int style, double size, uint32_t cp)
{
    char k[16];
    const TJV* v;
    (void)ctx;
    snprintf(k, sizeof k, "%u", (unsigned)cp);
    v = tj_get(tj_get(s_fallback, s_g->style[style].name), k);
    return tj_is(v, TJ_NUM) ? v->num * size : 0;
}

/* the size Chrome sets a face at: the sixty-fourth of a pixel near the CSS size at which the face
   gives the reference string the width the browser's canvas measured, from the state's `sizes` */
#define REF "Hold Shift to queue one deposit after another"
static double set_size(void* ctx, int style, double px)
{
    char k[32];
    const TJV* v;
    double best = px, bd = 1e9;
    int i;
    (void)ctx;
    snprintf(k, sizeof k, "%.4f", px);
    v = tj_get(tj_get(s_sizes, s_g->style[style].name), k);
    if (!tj_is(v, TJ_NUM) || !s_info[style].data) return px;
    for (i = -3; i <= 3; i++) {
        double z = (floor(px * 64) + i) / 64;
        double w = tagpu_hudtext_width(&s_info[style], (float)z, 0, 0, REF, strlen(REF));
        if (fabs(w - v->num) < bd) { bd = fabs(w - v->num); best = z; }
    }
    return best;
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

static int mod_of(const char* s)
{
    int m;
    for (m = 0; m < MOD_N; m++)
        if (!strcmp(tagpu_wikilay_mod_name(m), s)) return m;
    return MOD_UNKNOWN;
}

/* --check: tagpu_wikilay_check with every casting of each pack's side.json, in the game's faces */
static int check_packs(const WIKILAY* l, const WIKIDATA* d, const char* huddir, int npack, char** pack, HUDOPS* o)
{
    WIKIFACES f;
    WIKICASTING* all = NULL;
    char err[256];
    int n = 0, k;
    memset(&f, 0, sizeof f);
    faces(&f, huddir);
    for (k = 0; k < npack; k++) {
        size_t len;
        char* text = slurp(pack[k], &len);
        TJDOC* doc = text ? tagpu_json_parse(text, len, err, sizeof err) : NULL;
        const TJV *list = doc ? tj_get(tagpu_json_root(doc), "casting") : NULL, *v;
        if (!tj_is(list, TJ_ARR)) { fprintf(stderr, "wikilaycheck: %s holds no casting list\n", pack[k]); return 2; }
        for (v = list->first; v; v = v->next) {
            if (!(all = (WIKICASTING*)realloc(all, (size_t)(n + 1) * sizeof *all))) { fprintf(stderr, "wikilaycheck: out of memory\n"); return 2; }
            if (!tagpu_wikidata_casting(v, &all[n], err, sizeof err)) { fprintf(stderr, "wikilaycheck: %s: %s\n", pack[k], err); return 1; }
            n++;
        }
        tagpu_json_free(doc);
        free(text);
    }
    if (!tagpu_wikilay_check(l, d, &f, all, n, o, err, sizeof err)) {
        printf("wikilaycheck: the load-time check FAILED: %s\n", err);
        return 1;
    }
    printf("wikilaycheck: the load-time check passed: the page builds whole with the entries' own names, %d castings of "
           "%d packs and the worst castings, and every caption shows whole in its strip but the worst castings'\n", n, npack);
    free(all);
    return 0;
}

int main(int argc, char** argv)
{
    char path[1024], err[256];
    size_t n, nl, nk, npak;
    char *lt, *kt, *stt, *hl, *hk;
    const unsigned char *pak, *wl;
    TJDOC* sd;
    const TJV *S, *v, *e;
    HUDLAY* hud;
    WIKILAY* l;
    WIKIDATA* d;
    WIKIFACES f;
    WIKIPAGE* st = (WIKIPAGE*)calloc(1, sizeof(WIKIPAGE));
    WIKIFRAME fr;
    HUDOPS* o = (HUDOPS*)malloc(sizeof(HUDOPS));
    const HUDLAY* kit;
    int i, j, check = argc >= 5 && !strcmp(argv[1], "--check");
    double px;

    if (check) { argv++; argc--; }
    if (!check && argc != 5) {
        fprintf(stderr, "usage: wikilaycheck <art/hud dir> <art/wiki dir> <wikipak.bin> <state.json>\n"
                        "       wikilaycheck --check <art/hud dir> <art/wiki dir> <wikipak.bin> [side.json ...]\n");
        return 2;
    }
    snprintf(path, sizeof path, "%s/layout.json", argv[1]);
    if (!(hl = slurp(path, &nl))) { fprintf(stderr, "wikilaycheck: cannot read %s\n", path); return 2; }
    snprintf(path, sizeof path, "%s/kit.json", argv[1]);
    if (!(hk = slurp(path, &nk))) { fprintf(stderr, "wikilaycheck: cannot read %s\n", path); return 2; }
    if (!(hud = tagpu_hudlay_load(hl, nl, hk, nk, png_size, argv[1], err, sizeof err))) { fprintf(stderr, "wikilaycheck: hud: %s\n", err); return 1; }
    s_g = tagpu_hudlay_globals(hud);
    if (!(pak = (const unsigned char*)slurp(argv[3], &npak))) { fprintf(stderr, "wikilaycheck: cannot read %s\n", argv[3]); return 2; }
    if (!(d = tagpu_wikidata_load(pak, npak, err, sizeof err))) { fprintf(stderr, "wikilaycheck: data: %s\n", err); return 1; }
    if (!(wl = tagpu_pak_file(pak, npak, "layout.json", &n))) { fprintf(stderr, "wikilaycheck: the archive has no layout.json\n"); return 1; }
    lt = (char*)wl;
    nl = n;
    snprintf(path, sizeof path, "%s/kit/kit.json", argv[2]);
    if (!(kt = slurp(path, &nk))) { fprintf(stderr, "wikilaycheck: cannot read %s\n", path); return 2; }
    snprintf(path, sizeof path, "%s/kit", argv[2]);
    if (!(l = tagpu_wikilay_load(lt, nl, kt, nk, png_size, path, hud, err, sizeof err))) { fprintf(stderr, "wikilaycheck: %s\n", err); return 1; }
    kit = tagpu_wikilay_kit(l);
    if (!check) printf("{\"lights\":{");
    for (i = 0; i < tagpu_hudlay_nparts(kit); i++) {
        const HUDPART* k = tagpu_hudlay_part(kit, i);
        unsigned char* img = NULL;
        unsigned w = 0, h = 0;
        int lt_ = 0;
        snprintf(path, sizeof path, "%s/kit/parts/%s.png", argv[2], k->name);
        if (!lodepng_decode32_file(&img, &w, &h, path))
            lt_ = tagpu_wikilay_face_light(img, (int)w, (int)h, k->kind == HK_NINE ? k->slice : NULL, 0);
        free(img);
        tagpu_wikilay_light(l, i, lt_);
        if (!check) printf("%s\"%s\":%d", i ? "," : "", k->name, lt_);
    }
    if (check) return check_packs(l, d, argv[1], argc - 4, argv + 4, o);
    printf("},\n");

    if (!(stt = slurp(argv[4], &n)) || !(sd = tagpu_json_parse(stt, n, err, sizeof err))) { fprintf(stderr, "wikilaycheck: state: %s\n", err); return 2; }
    S = tagpu_json_root(sd);
    memset(&f, 0, sizeof f);
    faces(&f, argv[1]);
    s_fallback = tj_get(S, "fallback");
    s_sizes = tj_get(S, "sizes");
    f.missing = missing;
    f.size = set_size;
    st->W = (float)tj_num(tj_get(S, "W"), 0);
    st->H = (float)tj_num(tj_get(S, "H"), 0);
    st->bar = (float)tj_num(tj_get(S, "bar"), 32);
    st->rewind = tj_is(tj_get(S, "rewind"), TJ_TRUE);
    st->open = tj_is(tj_get(S, "open"), TJ_TRUE);
    st->entry = (int)tj_num(tj_get(S, "entry"), -1);
    st->demo = (int)tj_num(tj_get(S, "demo"), 0);
    v = tj_get(S, "fold");
    for (e = v ? v->first : NULL; e; e = e->next) {
        int k = (int)tj_num(e, -1);
        if (k >= 0 && k < WIKI_MAXNODE) st->fold[k >> 5] |= 1u << (k & 31);
    }
    v = tj_get(S, "scroll");
    for (i = 0; i < WIKI_NSCROLL; i++) st->scroll[i] = (float)tj_num(tj_at(v, i), 0);
    st->playing = tj_is(tj_get(S, "playing"), TJ_TRUE);
    st->done = tj_is(tj_get(S, "done"), TJ_TRUE);
    st->speed = (int)tj_num(tj_get(S, "speed"), 1);
    st->try_it = tj_is(tj_get(S, "try"), TJ_TRUE);
    st->sim_locked = tj_is(tj_get(S, "sim_locked"), TJ_TRUE);
    st->mod = mod_of(tj_str(tj_get(S, "mod"), "stock"));
    snprintf(st->held, sizeof st->held, "%s", tj_str(tj_get(S, "held"), ""));
    snprintf(st->caption, sizeof st->caption, "%s", tj_str(tj_get(S, "caption"), ""));
    /* the context the mockup fills its text with (wiki.js `fill`): the interface type and the
       side's casting, null for the entry's own names */
    st->iface = (int)tj_num(tj_get(S, "iface"), 1);
    v = tj_get(S, "casting");
    if (v && !tj_is(v, TJ_NULL)) {
        static WIKICASTING casting;
        if (!tagpu_wikidata_casting(v, &casting, err, sizeof err)) { fprintf(stderr, "wikilaycheck: casting: %s\n", err); return 1; }
        st->casting = &casting;
    }
    v = tj_get(S, "keys");
    for (e = v ? v->first : NULL; e; e = e->next)
        for (j = 0; j < WIKI_NKEYS; j++)
            if (!strcmp(tagpu_wikidata_key_name(j), tj_str(e, ""))) st->keys |= 1u << j;

    if (!tagpu_wikilay_build(l, d, &f, st, o, &fr, err, sizeof err)) { fprintf(stderr, "wikilaycheck: %s\n", err); return 1; }
    px = st->H / 720.0;
    printf("\"overflow\":%d,\"frame\":{\"scroll\":[%.4f,%.4f,%.4f],\"max\":[%.4f,%.4f,%.4f],\"stage\":[%.4f,%.4f,%.4f,%.4f]},\"ops\":[\n",
           o->overflow, fr.scroll[0], fr.scroll[1], fr.scroll[2], fr.scroll_max[0], fr.scroll_max[1], fr.scroll_max[2],
           fr.stage.x, fr.stage.y, fr.stage.w, fr.stage.h);
    for (i = 0; i < o->n; i++) {
        const HUDOP* p = &o->op[i];
        static const char* kinds[] = { "part", "solid", "dot", "text", "hot", "line", "chart" };
        double a = (double)(p->rgba & 0xFF) * (p->grp ? o->grp[p->grp - 1] : 1);
        printf("%s{\"kind\":\"%s\",\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f", i ? ",\n" : "", kinds[p->kind],
               p->r.x, p->r.y, p->r.w, p->r.h);
        if (p->clip.w >= 0) printf(",\"clip\":[%.4f,%.4f,%.4f,%.4f]", p->clip.x, p->clip.y, p->clip.w, p->clip.h);
        if (p->kind == HOP_PART) {
            printf(",\"part\":"); json_str(tagpu_hudlay_part(kit, p->part)->name, (int)strlen(tagpu_hudlay_part(kit, p->part)->name));
            printf(",\"alpha\":%.0f", a);
        }
        if (p->kind == HOP_SOLID || p->kind == HOP_DOT) printf(",\"rgba\":\"%08x\"", p->rgba);
        if (p->kind == HOP_TEXT) {
            const HUDRUN* r = &o->run[p->run];
            const WIKIFACE* fc = &f.face[p->style];
            printf(",\"style\":\"%s\",\"size\":%.4f,\"ink\":%d,\"rgb\":\"%06x\",\"alpha\":%.0f", s_g->style[p->style].name, p->size,
                   (p->flags & HOPF_INK) != 0, r->rgb, (double)255 * (p->grp ? o->grp[p->grp - 1] : 1));
            printf(",\"base\":%.4f", tagpu_hudlay_baseline((float)(p->r.y * px), (float)(p->r.h * px), (float)(p->size * px),
                                                            fc->ascent, fc->descent) / px);
            printf(",\"text\":"); json_str(o->text + r->off, r->len);
        }
        if (p->kind == HOP_HOT) { printf(",\"key\":"); json_str(o->text + p->run, (int)strlen(o->text + p->run)); }
        putchar('}');
    }
    printf("\n]}\n");
    return 0;
}
