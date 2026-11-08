/* modslaycheck.c -- tagpu_modslay's ops for a page the mockup dumped, as JSON on stdout; or the
   layout's load-time check.

     modslaycheck <art/hud dir> <art/wiki dir> <art/mods dir> <state.json>
     modslaycheck --check <art/hud dir> <art/wiki dir> <art/mods dir>

   --check runs tagpu_modslay_check as the game's load does, in the game's faces at their own sizes
   (no browser fallback): exit 0 when the fullest page builds whole, 1 with the failure.

   Everything is read from the sources under art/, not the DLL's packs: the text styles and faces from
   art/hud/ as tagpu_hudart.c opens them, the kit from art/wiki/kit/ through tagpu_hudlay_load_kit, the
   way tagpu_wikilay_load takes it, each part's face lightness from its decoded PNG, and the layout's
   numbers from art/mods/layout.json. The state is the mockup's page (modslaycheck.py COLLECT); its
   `fallback` and `sizes` give the browser's advance of a code point the faces lack and the size it
   sets a face at, as in tools/wiki/wikilaycheck.c. Each text op carries its baseline as the draw
   places it. tools/mods/modslaycheck.py compiles this and compares its output with the mockup's
   boxes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_hudlay.h"
#include "tagpu_hudtext.h"
#include "tagpu_modslay.h"
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
static void faces(UIFACES* f, const char* dir)
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

#define STR(dst, v, name) snprintf((dst), sizeof(dst), "%s", tj_str(tj_get((v), (name)), ""))

/* mods.js's page, field for field, each text cut to its array as the game's record is */
static void page_json(const TJV* S, MODSPAGE* p)
{
    const TJV *v, *e;
    int i;
    memset(p, 0, sizeof *p);
    p->W = (float)tj_num(tj_get(S, "W"), 0);
    p->H = (float)tj_num(tj_get(S, "H"), 0);
    p->tab = (int)tj_num(tj_get(S, "tab"), 0);
    p->updates = (int)tj_num(tj_get(S, "updates"), 0);
    p->selected = (int)tj_num(tj_get(S, "selected"), -1);
    p->update_all = (int)tj_num(tj_get(S, "update_all"), 0);
    STR(p->empty, S, "empty"); STR(p->title, S, "title"); STR(p->meta, S, "meta"); STR(p->summary, S, "summary");
    /* the mockup's showcase is its catalog entry's `show` with the id, name and version the game adds (mods.js showOf) */
    v = tj_get(S, "show");
    if (v && !tj_is(v, TJ_NULL) && !tagpu_modslay_show(v, tj_str(tj_get(v, "id"), ""), tj_str(tj_get(v, "name"), ""), tj_str(tj_get(v, "version"), ""), &p->show))
        fprintf(stderr, "modslaycheck: the state's showcase does not fit MODSSHOW\n");
    STR(p->note, S, "note"); STR(p->held, S, "held"); STR(p->confirm, S, "confirm");
    v = tj_get(S, "scroll");
    for (i = 0; i < MODS_NSCROLL; i++) p->scroll[i] = (float)tj_num(tj_at(v, i), 0);
    v = tj_get(S, "row");
    for (e = v ? v->first : NULL; e && p->nrow < MODS_MAXROW; e = e->next) {
        MODSROW* r = &p->row[p->nrow++];
        STR(r->name, e, "name"); STR(r->detail, e, "detail"); STR(r->badge_text, e, "badge_text");
        r->badge = (uint8_t)tj_num(tj_get(e, "badge"), 0);
        r->act = (uint8_t)tj_num(tj_get(e, "act"), 0);
        r->progress = (int16_t)tj_num(tj_get(e, "progress"), -1);
    }
    v = tj_get(S, "opt");
    for (e = v ? v->first : NULL; e && p->nopt < MODS_MAXOPT; e = e->next) {
        MODSOPT* o = &p->opt[p->nopt++];
        STR(o->label, e, "label"); STR(o->value, e, "value"); STR(o->reason, e, "reason");
        o->locked = (uint8_t)tj_num(tj_get(e, "locked"), 0);
    }
}

int main(int argc, char** argv)
{
    char path[1024], err[256];
    size_t n, nl, nk;
    char *lt, *kt, *stt, *hl, *hk;
    TJDOC* sd;
    const TJV* S;
    HUDLAY *hud, *kit;
    MODSLAY* l;
    UIFACES f;
    MODSPAGE* st = (MODSPAGE*)calloc(1, sizeof(MODSPAGE));
    MODSFRAME fr;
    HUDOPS* o = (HUDOPS*)malloc(sizeof(HUDOPS));
    int i, check = argc == 5 && !strcmp(argv[1], "--check");
    double px;

    if (check) { argv++; argc--; }
    if (!check && argc != 5) {
        fprintf(stderr, "usage: modslaycheck <art/hud dir> <art/wiki dir> <art/mods dir> <state.json>\n"
                        "       modslaycheck --check <art/hud dir> <art/wiki dir> <art/mods dir>\n");
        return 2;
    }
    snprintf(path, sizeof path, "%s/layout.json", argv[1]);
    if (!(hl = slurp(path, &nl))) { fprintf(stderr, "modslaycheck: cannot read %s\n", path); return 2; }
    snprintf(path, sizeof path, "%s/kit.json", argv[1]);
    if (!(hk = slurp(path, &nk))) { fprintf(stderr, "modslaycheck: cannot read %s\n", path); return 2; }
    if (!(hud = tagpu_hudlay_load(hl, nl, hk, nk, png_size, argv[1], err, sizeof err))) { fprintf(stderr, "modslaycheck: hud: %s\n", err); return 1; }
    s_g = tagpu_hudlay_globals(hud);
    snprintf(path, sizeof path, "%s/kit/kit.json", argv[2]);
    if (!(kt = slurp(path, &nk))) { fprintf(stderr, "modslaycheck: cannot read %s\n", path); return 2; }
    snprintf(path, sizeof path, "%s/kit", argv[2]);
    if (!(kit = tagpu_hudlay_load_kit(hud, kt, nk, png_size, path, err, sizeof err))) { fprintf(stderr, "modslaycheck: wiki %s\n", err); return 1; }
    snprintf(path, sizeof path, "%s/layout.json", argv[3]);
    if (!(lt = slurp(path, &nl))) { fprintf(stderr, "modslaycheck: cannot read %s\n", path); return 2; }
    if (!(l = tagpu_modslay_load(lt, nl, kit, err, sizeof err))) { fprintf(stderr, "modslaycheck: %s\n", err); return 1; }
    if (!check) printf("{\"lights\":{");
    for (i = 0; i < tagpu_hudlay_nparts(kit); i++) {
        const HUDPART* k = tagpu_hudlay_part(kit, i);
        unsigned char* img = NULL;
        unsigned w = 0, h = 0;
        int lt_ = 0;
        snprintf(path, sizeof path, "%s/kit/parts/%s.png", argv[2], k->name);
        if (!lodepng_decode32_file(&img, &w, &h, path))
            lt_ = ui_face_light(img, (int)w, (int)h, k->kind == HK_NINE ? k->slice : NULL, 0);
        free(img);
        tagpu_modslay_light(l, i, lt_);
        if (!check) printf("%s\"%s\":%d", i ? "," : "", k->name, lt_);
    }
    memset(&f, 0, sizeof f);
    faces(&f, argv[1]);
    if (check) {
        if (!tagpu_modslay_check(l, &f, o, err, sizeof err)) {
            printf("modslaycheck: the load-time check FAILED: %s\n", err);
            return 1;
        }
        printf("modslaycheck: the load-time check passed: the fullest page builds whole on every screen, at every scroll\n");
        return 0;
    }
    printf("},\n");

    if (!(stt = slurp(argv[4], &n)) || !(sd = tagpu_json_parse(stt, n, err, sizeof err))) { fprintf(stderr, "modslaycheck: state: %s\n", err); return 2; }
    S = tagpu_json_root(sd);
    s_fallback = tj_get(S, "fallback");
    s_sizes = tj_get(S, "sizes");
    f.missing = missing;
    f.size = set_size;
    page_json(S, st);

    if (!tagpu_modslay_build(l, &f, st, o, &fr, err, sizeof err)) { fprintf(stderr, "modslaycheck: %s\n", err); return 1; }
    px = st->H / 720.0;
    printf("\"overflow\":%d,\"frame\":{\"scroll\":[%.4f,%.4f],\"max\":[%.4f,%.4f],\"window\":[%.4f,%.4f,%.4f,%.4f]},\"ops\":[\n",
           o->overflow, fr.scroll[0], fr.scroll[1], fr.scroll_max[0], fr.scroll_max[1], fr.window.x, fr.window.y, fr.window.w, fr.window.h);
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
            const UIFACE* fc = &f.face[p->style];
            printf(",\"style\":\"%s\",\"size\":%.4f,\"ink\":%d,\"rgb\":\"%06x\",\"alpha\":%.0f", s_g->style[p->style].name, p->size,
                   (p->flags & HOPF_INK) != 0, r->rgb, (double)255 * (p->grp ? o->grp[p->grp - 1] : 1));
            printf(",\"base\":%.4f", tagpu_hudlay_baseline((float)(p->r.y * px), (float)(p->r.h * px), (float)(p->size * px),
                                                            fc->ascent, fc->descent) / px);
            printf(",\"text\":"); json_str(o->text + r->off, r->len);
        }
        if (p->kind == HOP_HOT) { printf(",\"key\":"); json_str(o->text + p->run, (int)strlen(o->text + p->run)); }
        putchar('}');
    }
    printf("]}\n");
    return 0;
}
