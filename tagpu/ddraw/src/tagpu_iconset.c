/* tagpu_iconset.c -- a mod's own strategic-icon set, the pure half
   (inc/tagpu_iconset.h): the PCX decoder, the profile-file reader, the
   iconcfg's reading, the load list and the atlas packer. No file I/O, no
   engine name, no Win32: tests/iconsettest.c runs all of it on the host. */
#include <string.h>
#include "tagpu_iconset.h"

static unsigned rd_u16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

int tagpu_pcx_decode(const unsigned char* b, size_t n, unsigned char* out, int cap, int* w, int* h)
{
    size_t at = 128;
    long need, done = 0;
    int W, H, bpl;
    *w = *h = 0;
    if (!b || !out || n < 128) return 0;
    /* ZSoft's header: the manufacturer byte, RLE, 8 bits a pixel, one plane */
    if (b[0] != 0x0A || b[2] != 1 || b[3] != 8 || b[65] != 1) return 0;
    W = (int)rd_u16(b + 8) - (int)rd_u16(b + 4) + 1;
    H = (int)rd_u16(b + 10) - (int)rd_u16(b + 6) + 1;
    bpl = (int)rd_u16(b + 66);
    if (W < 1 || H < 1 || W > TAGPU_PCX_MAXW || H > TAGPU_PCX_MAXH) return 0;
    if (bpl < W || bpl > TAGPU_PCX_MAXBPL) return 0;
    if (W * H > cap) return 0;
    need = (long)H * bpl;
    /* One stream of h x bpl bytes. A run's length is at most 63 and is spent
       only while `done` is below `need`, and a byte is written only for a
       column below W of a row below H: nothing the stream says can address
       past `out`. A run that overshoots the image's last byte ends with it. */
    while (done < need) {
        unsigned c, run = 1;
        if (at >= n) return 0;
        c = b[at++];
        if ((c & 0xC0u) == 0xC0u) {
            run = c & 0x3Fu;
            if (at >= n) return 0;
            c = b[at++];
        }
        for (; run && done < need; run--, done++) {
            const long col = done % bpl, row = done / bpl;
            if (col < W) out[row * W + col] = (unsigned char)c;
        }
    }
    *w = W; *h = H;
    return 1;
}

/* ---- the profile files ---- */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int blank(int c) { return c == ' ' || c == '\t'; }

/* [p, e) equals the NUL-terminated `s`, ASCII case folded */
static int span_ieq(const char* p, const char* e, const char* s)
{
    for (; p < e; p++, s++)
        if (!*s || lower((unsigned char)*p) != lower((unsigned char)*s)) return 0;
    return *s == 0;
}

static void trim(const char** p, const char** e)
{
    while (*p < *e && blank((unsigned char)**p)) (*p)++;
    while (*e > *p && blank((unsigned char)(*e)[-1])) (*e)--;
}

/* [p, e) into out (cap bytes), or 0 when it does not fit */
static int span_copy(const char* p, const char* e, char* out, int cap)
{
    const size_t len = (size_t)(e - p);
    if (cap < 1 || len >= (size_t)cap) return 0;
    memcpy(out, p, len);
    out[len] = 0;
    return 1;
}

/* One pass over the file. `fn` gets the trimmed key and value spans of every
   line of the first `section`; it returns 0 to stop. */
typedef int (*LINE_FN)(const char* k, const char* ke, const char* v, const char* ve, void* ctx);

static void ini_walk(const char* text, size_t n, const char* section, LINE_FN fn, void* ctx)
{
    const char* p = text;
    const char* const end = text + n;
    int in = 0;
    if (!text) return;
    while (p < end) {
        const char *ls = p, *le, *eq;
        while (p < end && *p != '\n' && *p != '\0') p++;
        le = p;
        if (p < end) p++;
        if (le > ls && le[-1] == '\r') le--;
        trim(&ls, &le);
        if (ls == le || *ls == ';') continue;
        if (*ls == '[') {
            const char *ns = ls + 1, *ne = ns;
            while (ne < le && *ne != ']') ne++;
            if (ne == le) continue;              /* no `]`: not a section line */
            /* only the first section of the name is read: a second ends it */
            if (in) return;
            trim(&ns, &ne);
            in = span_ieq(ns, ne, section);
            continue;
        }
        if (!in) continue;
        for (eq = ls; eq < le && *eq != '='; eq++) {}
        if (eq == le) continue;                  /* no `=`: no value */
        {
            const char *ks = ls, *ke = eq, *vs = eq + 1, *ve = le, *sc;
            trim(&ks, &ke);
            for (sc = vs; sc < ve && *sc != ';'; sc++) {}
            ve = sc;
            trim(&vs, &ve);
            if (!fn(ks, ke, vs, ve, ctx)) return;
        }
    }
}

typedef struct { const char* key; char* out; int cap; int result; } GET;

static int get_line(const char* k, const char* ke, const char* v, const char* ve, void* ctx)
{
    GET* g = (GET*)ctx;
    if (!span_ieq(k, ke, g->key)) return 1;
    g->result = span_copy(v, ve, g->out, g->cap) ? 1 : -1;
    if (g->result < 0 && g->cap > 0) g->out[0] = 0;
    return 0;
}

int tagpu_ini_get(const char* text, size_t n, const char* section, const char* key, char* out, int cap)
{
    GET g;
    if (cap > 0) out[0] = 0;
    g.key = key; g.out = out; g.cap = cap; g.result = 0;
    ini_walk(text, n, section, get_line, &g);
    return g.result;
}

typedef struct { TAGPU_INI_LINE fn; void* ctx; int passed, refused; } SECT;

static int sect_line(const char* k, const char* ke, const char* v, const char* ve, void* ctx)
{
    SECT* s = (SECT*)ctx;
    char key[TAGPU_INI_KEYMAX], val[TAGPU_INI_VALMAX];
    if (!span_copy(k, ke, key, sizeof key) || !span_copy(v, ve, val, sizeof val)) {
        s->refused++;
        return 1;
    }
    s->passed++;
    return s->fn(key, val, s->ctx);
}

int tagpu_ini_section(const char* text, size_t n, const char* section, TAGPU_INI_LINE fn, void* ctx,
                      int* refused)
{
    SECT s;
    s.fn = fn; s.ctx = ctx; s.passed = 0; s.refused = 0;
    ini_walk(text, n, section, sect_line, &s);
    if (refused) *refused = s.refused;
    return s.passed;
}

int tagpu_ini_bool(const char* value)
{
    const char* p;
    for (p = value; *p; p++) {
        int k;
        for (k = 0; k < 4 && p[k] && lower((unsigned char)p[k]) == "true"[k]; k++) {}
        if (k == 4) return 1;
    }
    return 0;
}

int tagpu_ini_byte(const char* value, int dflt)
{
    int v = 0, k;
    for (k = 0; value[k] >= '0' && value[k] <= '9'; k++) {
        v = v * 10 + (value[k] - '0');
        if (v > 255) return dflt;
    }
    return k ? v : dflt;
}

/* ---- names ---- */
int tagpu_iconset_ini_name_ok(const char* s, size_t cap)
{
    size_t k;
    for (k = 0; k < cap && k <= 32; k++) {
        const int c = (unsigned char)s[k];
        if (!c) return k >= 1;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-'))
            return 0;
    }
    return 0;
}

int tagpu_iconset_relpath_ok(const char* s)
{
    const char* seg = s;
    if (!s[0] || s[0] == '\\' || s[0] == '/' || strchr(s, ':')) return 0;
    /* no segment that climbs: `..`, and, to be safe from the path parser's
       trimming of a segment's trailing dots and blanks, every segment of only
       dots and blanks with two dots or more */
    for (;;) {
        const char* e = seg;
        int dots = 0, other = 0;
        for (; *e && *e != '\\' && *e != '/'; e++) {
            if (*e == '.') dots++;
            else if (*e != ' ') other = 1;
        }
        if (!other && dots >= 2) return 0;
        if (!*e) return 1;
        seg = e + 1;
    }
}

int tagpu_iconset_side_ok(const char* s, size_t cap)
{
    size_t k;
    for (k = 0; k < cap; k++) {
        const int c = (unsigned char)s[k];
        if (!c) return k >= 1;
        if (c < 0x20 || c >= 0x7F || strchr("\\/:*?\"<>|", c)) return 0;
    }
    return 0;
}

/* ---- the iconcfg ---- */
static int cfg_line(const char* key, const char* value, void* ctx)
{
    TAGPU_ICONCFG* c = (TAGPU_ICONCFG*)ctx;
    if (!key[0] || !tagpu_iconset_relpath_ok(value) || c->n >= TAGPU_ICONCFG_LINES) {
        c->refused++;
        return 1;
    }
    /* both fit: the walk passes a key under TAGPU_INI_KEYMAX and a value
       under TAGPU_INI_VALMAX, the two fields' sizes */
    strcpy(c->line[c->n].key, key);
    strcpy(c->line[c->n].file, value);
    c->n++;
    return 1;
}

void tagpu_iconcfg_parse(const char* text, size_t n, TAGPU_ICONCFG* c)
{
    char v[TAGPU_INI_VALMAX];
    int refused = 0;
    memset(c, 0, sizeof *c);
    c->fill = 0; c->transparent = 9; c->selected = 89;
    if (tagpu_ini_get(text, n, "Option", "FillColor", v, sizeof v) == 1)
        c->fill = tagpu_ini_byte(v, 0);
    if (tagpu_ini_get(text, n, "Option", "TransparentColor", v, sizeof v) == 1)
        c->transparent = tagpu_ini_byte(v, 9);
    if (tagpu_ini_get(text, n, "Option", "SelectedColor", v, sizeof v) == 1)
        c->selected = tagpu_ini_byte(v, 89);
    /* TADR's default is "true": absent or unreadable is the fixed list */
    c->custom = tagpu_ini_get(text, n, "Option", "UseDefaultIcon", v, sizeof v) == 1 && !tagpu_ini_bool(v);
    tagpu_ini_section(text, n, "Icon", cfg_line, c, &refused);
    c->refused += refused;
}

/* ---- the load list ---- */
static int key_is(const char* key, const char* name)
{
    return span_ieq(key, key + strlen(key), name);
}

static int plan_add(TAGPU_ISENTRY* out, int cap, int* k, int kind, int arg, const char* name,
                    const char* file, const char* ext)
{
    TAGPU_ISENTRY* e;
    const size_t nl = strlen(name), fl = strlen(file), xl = strlen(ext);
    if (*k >= cap || nl >= sizeof e->name || fl + xl >= sizeof e->file) return 0;
    e = &out[(*k)++];
    e->kind = kind; e->arg = arg;
    memcpy(e->name, name, nl + 1);
    memcpy(e->file, file, fl);
    memcpy(e->file + fl, ext, xl + 1);
    return 1;
}

int tagpu_iconset_plan(const TAGPU_ICONCFG* c, const char* const* sides, int nsides, TAGPU_ISENTRY* out,
                       int cap)
{
    static const char* const DEF[TAGPU_ISD_N] = {
        "COMMANDER", "AIRCRAFTCOMBAT", "AIRCONS", "CONS", "MOBILECOMBAT", "FACTORY", "BUILDING" };
    int k = 0, s, i;
    if (nsides > TAGPU_ICONSET_SIDES) nsides = TAGPU_ICONSET_SIDES;
    for (s = 0; s < nsides; s++)
        if (sides[s] && tagpu_iconset_side_ok(sides[s], TAGPU_INI_KEYMAX))
            plan_add(out, cap, &k, TAGPU_ISE_SIDE, s, sides[s], sides[s], ".PCX");
    if (!c->custom) {
        for (i = 0; i < TAGPU_ISD_N; i++) plan_add(out, cap, &k, TAGPU_ISE_DEFAULT, i, "", DEF[i], ".PCX");
        plan_add(out, cap, &k, TAGPU_ISE_NONE, 0, "", "NONE", ".PCX");
        plan_add(out, cap, &k, TAGPU_ISE_UNKNOWN, 0, "", "UNKNOWN", ".PCX");
        plan_add(out, cap, &k, TAGPU_ISE_NUKE, 0, "", "NUKEICON", ".PCX");
        return k;
    }
    for (i = 0; i < c->n; i++) {
        const TAGPU_ICONCFG_LINE* l = &c->line[i];
        const int kind = key_is(l->key, "nothing") ? TAGPU_ISE_NONE
                       : key_is(l->key, "unknow") ? TAGPU_ISE_UNKNOWN
                       : key_is(l->key, "nukeicon") ? TAGPU_ISE_NUKE : TAGPU_ISE_CATEGORY;
        plan_add(out, cap, &k, kind, 0, kind == TAGPU_ISE_CATEGORY ? l->key : "", l->file, "");
    }
    return k;
}

/* ---- the atlas ---- */
void tagpu_iconset_classify(const unsigned char* pix, int n, int transparent, int fill, int selected,
                            unsigned char* cls)
{
    int k;
    for (k = 0; k < n; k++)
        cls[k] = pix[k] == transparent ? TAGPU_ITX_KEY
               : pix[k] == fill        ? TAGPU_ITX_FILL
               : pix[k] == selected    ? TAGPU_ITX_SEL : TAGPU_ITX_DRAWN;
}

int tagpu_iconart_pack(const TAGPU_ICONSRC* src, int n, unsigned char* atlas, int aw, int ah,
                       TAGPU_ICONRECT* rect, int marks[3])
{
    unsigned char used[256];
    int i, k, x = 1, y = 1, rowh = 0;
    if (n < 0 || aw < 3 || ah < 3 || aw > 0xFFFF || ah > 0xFFFF) return 0;
    memset(used, 0, sizeof used);
    for (i = 0; i < n; i++) {
        const TAGPU_ICONSRC* s = &src[i];
        if (!s->pix || !s->cls || s->w < 1 || s->h < 1 || s->w > aw - 2 || s->h > ah - 2) return 0;
        for (k = 0; k < s->w * s->h; k++) if (s->cls[k] == TAGPU_ITX_DRAWN) used[s->pix[k]] = 1;
    }
    for (k = 0, i = 0; i < 256 && k < 3; i++) if (!used[i]) marks[k++] = i;
    if (k < 3) return 0;
    /* shelves: left to right with a gutter, a new row under the tallest of the
       last when the next would pass the right edge */
    for (i = 0; i < n; i++) {
        const TAGPU_ICONSRC* s = &src[i];
        if (x + s->w + 1 > aw) { y += rowh + 1; x = 1; rowh = 0; }
        if (y + s->h + 1 > ah) return 0;
        rect[i].x = (unsigned short)x; rect[i].y = (unsigned short)y;
        rect[i].w = (unsigned short)s->w; rect[i].h = (unsigned short)s->h;
        rect[i].hx = 0; rect[i].hy = 0;
        x += s->w + 1;
        if (s->h > rowh) rowh = s->h;
    }
    memset(atlas, marks[0], (size_t)aw * (size_t)ah);
    for (i = 0; i < n; i++) {
        const TAGPU_ICONSRC* s = &src[i];
        int row, col;
        for (row = 0; row < s->h; row++)
            for (col = 0; col < s->w; col++) {
                const int t = row * s->w + col;
                const int c = s->cls[t];
                atlas[(size_t)(rect[i].y + row) * (size_t)aw + rect[i].x + col] = (unsigned char)
                    (c == TAGPU_ITX_DRAWN ? s->pix[t] : c == TAGPU_ITX_FILL ? marks[1]
                   : c == TAGPU_ITX_SEL ? marks[2] : marks[0]);
            }
    }
    return 1;
}
