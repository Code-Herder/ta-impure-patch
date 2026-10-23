/* tagpu_hudlay.c -- the replay HUD's layout rules. Contract: tagpu_hudlay.h.

   EACH FUNCTION BELOW IS ONE OF hud.js's, under the same name and in the same order, and emits
   its elements in hud.js's document order, which is the order the browser paints them. A rule
   changed in one file is changed in the other; tools/hudart/laycheck.py is what notices when
   they disagree.

   The arithmetic is in double, as JavaScript's is, so the two files' rectangles agree to far
   below a device pixel; ops are stored in float. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hudlay.h"

#define HUD_MAXPART 128

struct HUDLAY {
    TJDOC*      ldoc;
    TJDOC*      kdoc;
    const TJV*  L;
    HUDGLOBALS  g;
    int         max_hot;        /* tagpu_hudlay_max_hot */
    int         nparts;
    HUDPART     part[HUD_MAXPART];
};

typedef struct { double x, y, w, h; } R;

typedef struct {
    const HUDLAY*   l;
    const TJV*      L;
    const HUDSTATE* st;
    HUDOPS*         o;
    char*           err;
    size_t          errn;
    int             bad;
    unsigned        flags;      /* given to every op: HOPF_DIM inside an out player's row */
} C;

/* ---- reading the layout ---------------------------------------------------------------------- */

static void miss(C* c, const TJV* o, const char* k, const char* what)
{
    if (c->bad++) return;
    if (c->err && c->errn)
        snprintf(c->err, c->errn, "layout: %s%s%s is missing or not %s",
                 o && o->key ? o->key : "", o && o->key ? "." : "", k, what);
}
static const TJV* obj(C* c, const TJV* o, const char* k)
{
    const TJV* v = tj_get(o, k);
    if (!tj_is(v, TJ_OBJ)) miss(c, o, k, "an object");
    return v;
}
static const TJV* arr(C* c, const TJV* o, const char* k)
{
    const TJV* v = tj_get(o, k);
    if (!tj_is(v, TJ_ARR)) miss(c, o, k, "a list");
    return v;
}
static double num(C* c, const TJV* o, const char* k)
{
    const TJV* v = tj_get(o, k);
    if (!tj_is(v, TJ_NUM)) { miss(c, o, k, "a number"); return 0; }
    return v->num;
}
static const char* str(C* c, const TJV* o, const char* k)
{
    const TJV* v = tj_get(o, k);
    if (!tj_is(v, TJ_STR)) { miss(c, o, k, "a string"); return ""; }
    return v->str;
}
static uint32_t hexrgb(const char* s)
{
    return s && s[0] == '#' && strlen(s) == 7 ? (uint32_t)strtoul(s + 1, NULL, 16) : 0;
}

/* ---- the ops ----------------------------------------------------------------------------------- */

static HUDOP* op(C* c, int kind, R r)
{
    HUDOP* p;
    if (c->o->n >= HUD_MAXOP) { c->o->overflow = 1; return NULL; }
    p = &c->o->op[c->o->n++];
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind;
    p->flags = (uint8_t)c->flags;
    p->part = -1;
    p->r.x = (float)r.x; p->r.y = (float)r.y; p->r.w = (float)r.w; p->r.h = (float)r.h;
    p->clip.w = -1;
    p->rgba = 0xFFFFFFFFu;
    return p;
}

static int text_put(C* c, const char* s)
{
    size_t n = strlen(s);
    int at = c->o->ntext;
    if ((size_t)at + n + 1 > HUD_TEXTMAX) { c->o->overflow = 1; return -1; }
    memcpy(c->o->text + at, s, n + 1);
    c->o->ntext += (int)n + 1;
    return at;
}

static int part_index(const HUDLAY* l, const char* name)
{
    int i;
    for (i = 0; i < l->nparts; i++)
        if (!strcmp(l->part[i].name, name)) return i;
    return -1;
}

/* a part at box b; `opacity` < 1 for a disabled control */
static HUDOP* art(C* c, const char* part, R b, double opacity)
{
    int k = part_index(c->l, part);
    HUDOP* p;
    if (k < 0) {
        if (!c->bad++ && c->err && c->errn) snprintf(c->err, c->errn, "layout: no part '%s' in the kit", part);
        return NULL;
    }
    if (!(p = op(c, HOP_PART, b))) return NULL;
    p->part = (int16_t)k;
    p->rgba = 0xFFFFFF00u | (uint32_t)lround(255.0 * (opacity < 0 ? 0 : opacity > 1 ? 1 : opacity));
    return p;
}
static void icon_a(C* c, const char* name, R b, double opacity)
{
    char n[40];
    snprintf(n, sizeof n, "i_%s", name);
    art(c, n, b, opacity);
}
static void icon(C* c, const char* name, R b) { icon_a(c, name, b, 1); }
/* a control: a new call site extends tagpu_hudlay_max_hot */
static void hot(C* c, R b, const char* key)
{
    HUDOP* p = op(c, HOP_HOT, b);
    int at;
    if (!p) return;
    if ((at = text_put(c, key)) < 0) { c->o->n--; return; }
    p->run = (uint16_t)at;
}
static HUDOP* solid(C* c, R b, uint32_t rgb, double alpha)
{
    HUDOP* p = op(c, HOP_SOLID, b);
    if (p) p->rgba = (rgb << 8) | (uint32_t)lround(255.0 * alpha);
    return p;
}

/* ---- the band map and the placement rule (hud.js bandMap, frame, extent, place) ------------- */

typedef struct { double w, h; const HUDPART* k; } F;

static double bandmap(const float (*b)[2], int nb, double D, double T, double x)
{
    double S = 0, k, y = 0, last = 0;
    int i;
    for (i = 0; i < nb; i++) S += b[i][1] - b[i][0];
    if (!(S > 0 && T >= D - S)) return x * T / D;
    k = (T - (D - S)) / S;
    for (i = 0; i < nb; i++) {
        double p = b[i][0], q = b[i][1];
        if (x <= p) return y + x - last;
        y += p - last;
        if (x <= q) return y + (x - p) * k;
        y += (q - p) * k;
        last = q;
    }
    return y + x - last;
}
static F frame(const HUDLAY* l, double w, double h, const char* part)
{
    F f;
    int k = part ? part_index(l, part) : -1;
    f.w = w; f.h = h;
    f.k = k >= 0 && l->part[k].kind == HK_GRID ? &l->part[k] : NULL;
    return f;
}
static double fx(const F* f, double x) { return f->k ? bandmap((const float (*)[2])f->k->xs, f->k->nxs, f->k->design[0], f->w, x) : x; }
static double fy(const F* f, double y) { return f->k ? bandmap((const float (*)[2])f->k->ys, f->k->nys, f->k->design[1], f->h, y) : y; }

/* the design extent along axis a of a well, of the union of a list of wells, or of the frame */
static void extent(C* c, const F* f, const TJV* ws, int a, double* lo, double* hi)
{
    const TJV* w;
    int i, first = 1;
    if (!ws) { *lo = 0; *hi = f->k ? f->k->design[a] : (a ? f->h : f->w); return; }
    *lo = *hi = 0;
    for (i = 0; ws->type == TJ_ARR ? i < ws->n : i < 1; i++) {
        const char* name = ws->type == TJ_ARR ? tj_str(tj_at(ws, i), "") : tj_str(ws, "");
        double p, q;
        w = f->k ? tj_get(f->k->wells, name) : NULL;
        if (!tj_is(w, TJ_ARR) || w->n != 4) {
            if (!c->bad++ && c->err && c->errn)
                snprintf(c->err, c->errn, "layout: no well '%s' in part '%s'", name, f->k ? f->k->name : "(none)");
            return;
        }
        p = tj_num(tj_at(w, a), 0);
        q = p + tj_num(tj_at(w, a + 2), 0);
        if (first || p < *lo) *lo = p;
        if (first || q > *hi) *hi = q;
        first = 0;
    }
}

typedef struct {
    const TJV* in_x;
    const TJV* in_y;
    double pad[4];
    double padf;
    int    hasw, hash;
    double w, h;
    char   ax, ay;
    int    centre;          /* a text slot's `align: "c"` */
    double fit;             /* a text slot's `fit`, 0 = none */
    const TJV* src;         /* the JSON object it was read from, for messages */
} PL;

static void pl_in(PL* s, const TJV* in)
{
    if (tj_is(in, TJ_OBJ)) { s->in_x = tj_get(in, "x"); s->in_y = tj_get(in, "y"); }
    else s->in_x = s->in_y = in;
}
static void pl_pad(PL* s, const TJV* pad)
{
    int i;
    for (i = 0; i < 4; i++) s->pad[i] = tj_num(tj_at(pad, i), 0);
}
static PL spec(const TJV* o)
{
    PL s;
    const TJV* v;
    memset(&s, 0, sizeof s);
    s.src = o;
    pl_in(&s, tj_get(o, "in"));
    pl_pad(&s, tj_get(o, "pad"));
    s.padf = tj_num(tj_get(o, "padf"), 0);
    if ((v = tj_get(o, "w"))) { s.hasw = 1; s.w = tj_num(v, 0); }
    if ((v = tj_get(o, "h"))) { s.hash = 1; s.h = tj_num(v, 0); }
    s.ax = tj_str(tj_get(o, "ax"), "c")[0];
    s.ay = tj_str(tj_get(o, "ay"), "c")[0];
    s.centre = tj_str(tj_get(o, "align"), "")[0] == 'c';
    s.fit = tj_num(tj_get(o, "fit"), 0);
    return s;
}

static R place(C* c, const F* f, const PL* s)
{
    double l, r, t, b, x0, x1, y0, y1;
    R out;
    extent(c, f, s->in_x, 0, &l, &r);
    extent(c, f, s->in_y, 1, &t, &b);
    x0 = fx(f, l + s->pad[0]); x1 = fx(f, r - s->pad[2]);
    y0 = fy(f, t + s->pad[1]); y1 = fy(f, b - s->pad[3]);
    if (s->padf) {
        double dx = (x1 - x0) * s->padf, dy = (y1 - y0) * s->padf;
        x0 += dx; x1 -= dx; y0 += dy; y1 -= dy;
    }
    if (s->hasw) { x0 = s->ax == 'l' ? x0 : s->ax == 'r' ? x1 - s->w : (x0 + x1 - s->w) / 2; x1 = x0 + s->w; }
    if (s->hash) { y0 = s->ay == 't' ? y0 : s->ay == 'b' ? y1 - s->h : (y0 + y1 - s->h) / 2; y1 = y0 + s->h; }
    out.x = x0; out.y = y0; out.w = x1 - x0; out.h = y1 - y0;
    return out;
}
static R at(R b, double ox, double oy) { b.x += ox; b.y += oy; return b; }
static R box(double x, double y, double w, double h) { R b; b.x = x; b.y = y; b.w = w; b.h = h; return b; }

/* an inset edge `e` units deep round box b, as one ring: top and bottom whole, the sides between them */
static void ring(C* c, R b, double e, uint32_t rgb, double alpha)
{
    R side[4];
    int k;
    side[0] = box(b.x, b.y, b.w, e);
    side[1] = box(b.x, b.y + b.h - e, b.w, e);
    side[2] = box(b.x, b.y + e, e, b.h - 2 * e);
    side[3] = box(b.x + b.w - e, b.y + e, e, b.h - 2 * e);
    for (k = 0; k < 4; k++) {
        HUDOP* p = solid(c, side[k], rgb, alpha);
        if (p) p->flags |= HOPF_EDGE;
    }
}

/* ---- text -------------------------------------------------------------------------------------- */

static int style_index(const HUDLAY* l, const char* name)
{
    int i;
    for (i = 0; i < l->g.nstyle; i++)
        if (!strcmp(l->g.style[i].name, name)) return i;
    return -1;
}

/* text in a style: its size shrinks to the box's height / s.fit when the box is lower than the
   style's line. A run's rgb 0 is the style's colour; `resource` 1 or 2 the metal or energy one. */
typedef struct { const char* s; uint32_t rgb; } RUN;

static void text_runs(C* c, const RUN* runs, int n, R b, const char* style, const PL* s, double dy)
{
    int st = style_index(c->l, style), i;
    HUDOP* p;
    if (st < 0) {
        if (!c->bad++ && c->err && c->errn) snprintf(c->err, c->errn, "layout: no style '%s'", style);
        return;
    }
    if (c->o->nrun + n > HUD_MAXRUN) { c->o->overflow = 1; return; }
    if (!(p = op(c, HOP_TEXT, box(b.x, b.y + dy, b.w, b.h)))) return;
    p->style = (int16_t)st;
    p->centre = (uint8_t)s->centre;
    p->size = (float)(s->fit ? fmin(c->l->g.style[st].size, b.h / s->fit) : c->l->g.style[st].size);
    p->clip = p->r;
    p->run = (uint16_t)c->o->nrun;
    for (i = 0; i < n; i++) {
        HUDRUN* r = &c->o->run[c->o->nrun];
        int off = text_put(c, runs[i].s);
        if (off < 0) break;
        r->rgb = runs[i].rgb;
        r->resource = 0;
        r->off = (uint16_t)off;
        r->len = (uint16_t)strlen(runs[i].s);
        c->o->nrun++;
        p->nrun++;
    }
}
static void text(C* c, const char* s, R b, const char* style, const PL* sl, uint32_t rgb, double dy)
{
    RUN r;
    r.s = s; r.rgb = rgb;
    text_runs(c, &r, 1, b, style, sl, dy);
}

/* hud.js k1: rounded, with a thousands separator */
static void k1(char* o, size_t n, double v)
{
    char d[32];
    long long x = (long long)floor(v + 0.5);
    int len, i, j = 0, neg = x < 0;
    snprintf(d, sizeof d, "%lld", neg ? -x : x);
    len = (int)strlen(d);
    if (neg && j + 1 < (int)n) o[j++] = '-';
    for (i = 0; i < len && j + 2 < (int)n; i++) {
        if (i && (len - i) % 3 == 0) o[j++] = ',';
        o[j++] = d[i];
    }
    o[j] = 0;
}
/* hud.js kK: thousands in K from 10 000 on, so a ten-player team's income still fits its recess */
static void kK(char* o, size_t n, double v)
{
    if (v >= 10000) snprintf(o, n, "%.0fK", floor(v / 1000 + 0.5));
    else k1(o, n, v);
}
static void clock_s(char* o, size_t n, double t)
{
    long s = (long)floor((t < 0 ? 0 : t) + 0.5);
    snprintf(o, n, "%ld:%02ld", s / 60, s % 60);
}
/* a JavaScript number as String() writes one: integers without a decimal point */
static void jsnum(char* o, size_t n, double v)
{
    if (v == floor(v) && fabs(v) < 1e15) snprintf(o, n, "%.0f", v);
    else snprintf(o, n, "%.15g", v);
}
/* JavaScript's Math.round: the nearer integer, the larger on a tie (x - floor(x) is exact) */
static double js_round(double x)
{
    double f = floor(x);
    return x - f >= 0.5 ? f + 1 : f;
}
/* hud.js rate: a row's rate in tenths below 100, as the game's incomes are fractional, whole
   numbers from there */
static void rate(char* o, size_t n, double v)
{
    jsnum(o, n, fabs(v) < 100 ? js_round(v * 10) / 10 : js_round(v));
}
/* JavaScript's x.toFixed(1): the n / 10 nearest x's exact value, the larger on a tie. Each fma
   rounds once, which cannot change the sign of an exact difference, so the comparisons are
   exact where x * 10 or x * 20 computed directly would not be. */
static double to_fixed1(double x)
{
    double n = floor(x * 10);
    while (fma(10, x, -n) < 0) n--;
    while (fma(10, x, -(n + 1)) >= 0) n++;
    return (fma(20, x, -(2 * n + 1)) >= 0 ? n + 1 : n) / 10;
}

/* "+a / −b", the two numbers in the signs' colours */
static void signed_text(C* c, const char* a, const char* b, R box_, const char* style, const PL* s)
{
    char pa[40], pb[40];
    RUN r[3];
    snprintf(pa, sizeof pa, "+%s", a);
    snprintf(pb, sizeof pb, "\xE2\x88\x92%s", b);          /* U+2212 MINUS SIGN */
    r[0].s = pa; r[0].rgb = c->l->g.pos_rgb;
    r[1].s = "\xC2\xA0/\xC2\xA0"; r[1].rgb = 0;             /* NO-BREAK SPACEs round the slash */
    r[2].s = pb; r[2].rgb = c->l->g.neg_rgb;
    text_runs(c, r, 3, box_, style, s, 0);
}

/* ---- the controls -------------------------------------------------------------------------- */

static const char* const TOGGLES[] = { "stats", "cursors", "clicks", "selections", "chat" };

unsigned tagpu_hudlay_toggle(const char* name)
{
    unsigned i;
    for (i = 0; name && i < sizeof TOGGLES / sizeof *TOGGLES; i++)
        if (!strcmp(name, TOGGLES[i])) return 1u << i;
    return 0;
}
const char* tagpu_hudlay_toggle_name(unsigned bit)
{
    unsigned i;
    for (i = 0; i < sizeof TOGGLES / sizeof *TOGGLES; i++)
        if (bit == 1u << i) return TOGGLES[i];
    return NULL;
}

/* a button: its part, pressed while held (and while on, for a toggle), its icon and text placed
   on its face, which sinks by press_shift while pressed */
typedef struct {
    const char* key;
    const TJV*  icon;       /* {name | playing, pad, w, h, ax, padf} or NULL */
    const char* icon_name;  /* overrides icon.name */
    const TJV*  textspec;   /* {style, pad, align} or NULL */
    const char* text;
    double      off;        /* 0: enabled; else the opacity of a disabled control */
} BTN;

static void button(C* c, const BTN* s, const TJV* kind, R b, int on)
{
    F f = frame(c->l, b.w, b.h, NULL);
    int pressed = on || (c->st->held[0] && !strcmp(c->st->held, s->key));
    double dy = pressed ? c->l->g.press_shift : 0, a = s->off ? s->off : 1;
    art(c, str(c, kind, pressed ? "pressed" : "part"), b, a);
    if (!s->off) hot(c, b, s->key);
    if (s->icon) {
        PL p = spec(s->icon);
        const char* n = s->icon_name ? s->icon_name
                      : tj_get(s->icon, "playing") && c->st->playing ? str(c, s->icon, "playing") : str(c, s->icon, "name");
        R r = at(place(c, &f, &p), b.x, b.y + dy);
        icon_a(c, n, r, a);
    }
    if (s->textspec) {
        PL p = spec(s->textspec);
        text(c, s->text, at(place(c, &f, &p), b.x, b.y), str(c, s->textspec, "style"), &p, 0, dy);
    }
}

/* ---- the dock: the transport plate from the side panel's edge, the timeline housing to the
   screen's right ------------------------------------------------------------------------------- */

static double along(double w, double e, double f) { return e + (w - 2 * e) * f; }

static void lanes(C* c, R g, R tr, double ends);

static void dock(C* c)
{
    const HUDSTATE* st = c->st;
    const TJV *D = obj(c, c->L, "dock"), *T = obj(c, D, "transport"), *Rl = obj(c, D, "rail");
    const TJV *tbtn = obj(c, T, "button"), *btns = arr(c, T, "buttons"), *M = obj(c, Rl, "markers");
    const TJV *Bd = obj(c, M, "badge"), *tk = obj(c, Rl, "track"), *fill = obj(c, tk, "fill"), *ph = obj(c, tk, "playhead");
    const TJV *tm = obj(c, Rl, "time");
    double Dh = num(c, D, "h"), y = st->H - st->bottom - Dh, Tw = num(c, T, "w");
    R tb = box(st->panel + num(c, T, "l"), y, Tw, Dh);
    F f = frame(c->l, Tw, Dh, str(c, T, "part")), fr;
    double rx, rw, p = st->T > 0 ? st->t / st->T : 0, ends;
    R lane, tr, fb, phb;
    PL pl;
    int i;
    char sp[24], hd[24], tx[48], t0[16], t1[16];

    jsnum(sp, sizeof sp - 3, st->speed);
    strcat(sp, "\xC3\x97");                                  /* U+00D7 MULTIPLICATION SIGN */
    snprintf(hd, sizeof hd, "%ld%%", lround(st->hud * 100));
    /* the transport plate and the timeline housing are the HUD's whole: a click between their
       buttons is taken too. Placed before the buttons, so the hit test, last first, finds a
       button over its plate. */
    hot(c, tb, str(c, D, "act"));
    art(c, str(c, T, "part"), tb, 1);
    for (i = 0; btns && i < btns->n; i++) {
        const TJV* s = tj_at(btns, i);
        const TJV* ts = tj_get(s, "text");
        BTN b;
        memset(&b, 0, sizeof b);
        b.key = str(c, s, "act");
        b.icon = tj_get(s, "icon");
        if (ts) {
            const char* bind = str(c, ts, "bind");
            b.textspec = ts;
            b.text = !strcmp(bind, "speed") ? sp : !strcmp(bind, "hud") ? hd : "";
        }
        memset(&pl, 0, sizeof pl);
        pl_in(&pl, tj_get(s, "in"));
        pl_pad(&pl, tj_get(tbtn, "pad"));
        button(c, &b, tbtn, at(place(c, &f, &pl), tb.x, tb.y), 0);
    }

    rx = tb.x + Tw + num(c, Rl, "gap");
    rw = st->W - num(c, Rl, "r") - rx;
    fr = frame(c->l, rw, Dh, str(c, Rl, "part"));
    pl = spec(M);
    lane = place(c, &fr, &pl);
    pl = spec(tk);
    tr = place(c, &fr, &pl);
    hot(c, box(rx, y, rw, Dh), str(c, D, "act"));
    art(c, str(c, Rl, "part"), box(rx, y, rw, Dh), 1);
    {
        double me = num(c, M, "ends"), dot = num(c, Bd, "dot"), ic = num(c, Bd, "icon");
        const TJV* icons = obj(c, M, "icons");
        uint32_t bm = hexrgb(str(c, M, "bookmark_dot"));
        for (i = 0; i < st->nmark; i++) {
            const HUDMARK* m = &st->mark[i];
            double cx = along(lane.w, me, st->T > 0 ? m->t / st->T : 0), s = lane.h;
            R bb = box(rx + lane.x + cx - s / 2, y + lane.y, s, s);
            HUDOP* d = op(c, HOP_DOT, box(bb.x + dot, bb.y + dot, s - 2 * dot, s - 2 * dot));
            static const char* kinds[] = { "death", "defeat", "bookmark" };
            if (d) d->rgba = ((m->seat < 0 || m->seat >= st->nseat ? bm : st->seat[m->seat].colour) << 8) | 0xFF;
            art(c, str(c, Bd, "part"), bb, 1);
            icon(c, str(c, icons, kinds[m->kind < 0 || m->kind > 2 ? 0 : m->kind]), box(bb.x + ic, bb.y + ic, s - 2 * ic, s - 2 * ic));
        }
    }
    ends = num(c, tk, "ends");
    {
        const TJV* fp = arr(c, fill, "pad");
        double p0 = tj_num(tj_at(fp, 0), 0), p1 = tj_num(tj_at(fp, 1), 0), p2 = tj_num(tj_at(fp, 2), 0), p3 = tj_num(tj_at(fp, 3), 0);
        HUDOP* a;
        R trb = at(tr, rx, y);
        F tf = frame(c->l, tr.w, tr.h, NULL);
        hot(c, trb, "track");
        c->o->track_x0 = (float)(trb.x + along(tr.w, ends, 0));
        c->o->track_x1 = (float)(trb.x + along(tr.w, ends, 1));
        fb = box(trb.x + p0, trb.y + p1, tr.w - p0 - p2, tr.h - p1 - p3);
        if ((a = art(c, str(c, fill, "part"), fb, 1))) {
            a->clip.x = (float)fb.x; a->clip.y = (float)fb.y; a->clip.w = (float)(fb.w * p); a->clip.h = (float)fb.h;
        }
        memset(&pl, 0, sizeof pl);
        pl_pad(&pl, tj_get(ph, "pad"));
        phb = place(c, &tf, &pl);
        {
            double pw = num(c, ph, "w"), phx = along(tr.w, ends, p);
            art(c, str(c, ph, "part"), box(trb.x + phx - pw / 2, trb.y + phb.y, pw, phb.h), 1);
        }
    }
    clock_s(t0, sizeof t0, st->t);
    clock_s(t1, sizeof t1, st->T);
    snprintf(tx, sizeof tx, "%s / %s", t0, t1);
    pl = spec(tm);
    text(c, tx, at(place(c, &fr, &pl), rx, y), str(c, tm, "style"), &pl, 0, 0);
    if (st->nfork > 0 && st->lanes_open) lanes(c, box(rx, y, rw, Dh), at(tr, rx, y), ends);
}

/* ---- proposed, variation B: the forks as lanes over the timeline housing, oldest branch on top
   (hud.js `lanesH`, `lanes`). A lane runs from its branch to its end on the track's own time axis,
   widened to its minimum to the right first; the playhead's line runs up through every lane.
   THE CHAIN THAT PLAYS IS FILLED ALONG IT, as the track is filled to where the chain leaves the
   original: the watched lane to the playhead, and each lane before it on the chain to where the
   next one on the chain branches off it, so the path the playing took has no gap.
   THE BRANCH LINE says where a fork's history before its branch is: a fork whose parent is a lane
   drawn above it rises to that lane; every other fork -- of the original, or of a deleted fork,
   whose prefix still plays but has no lane -- drops to the track. The parent is always above:
   the lanes are in (branch, id) order, and a fork branches after its parent does, since a
   takeover starts after the first iteration of the segment it forks (takeover_point). The line's
   x is the fork's branch, inside its parent's span; it is held to the parent's lane, which a lane
   clamped at the track's right end may not reach. g: the housing's box; tr: its track's. ------- */

#define LANE_CHAIN 2u           /* HUDFORK.flags: the packet's TAGPU_PK_FORK_CHAIN, a segment of
                                   the chain that plays */

/* the lane plate's height with its gap above the dock: the column stands on it, since the lanes
   run under the column */
static double lanes_h(C* c)
{
    const HUDSTATE* st = c->st;
    const TJV *P, *Pl, *Ln, *pad;
    int n = st->nfork < HUD_MAXFORK ? st->nfork : HUD_MAXFORK;
    if (n <= 0 || !st->lanes_open) return 0;
    P = obj(c, obj(c, c->L, "proposed"), "forks");
    Pl = obj(c, P, "plate");
    Ln = obj(c, P, "lane");
    pad = arr(c, Pl, "pad");
    return num(c, Pl, "gap") + tj_num(tj_at(pad, 1), 0) + n * num(c, Ln, "h") + (n - 1) * num(c, Ln, "gap")
         + tj_num(tj_at(pad, 3), 0);
}

static void lanes(C* c, R g, R tr, double ends)
{
    const HUDSTATE* st = c->st;
    const TJV *P = obj(c, obj(c, c->L, "proposed"), "forks"), *Pl = obj(c, P, "plate"), *Ln = obj(c, P, "lane");
    const TJV *Bt = obj(c, Ln, "buttons"), *items = arr(c, Bt, "items"), *Lb = obj(c, Ln, "label"), *Fl = obj(c, Ln, "fill");
    const TJV *Br = obj(c, P, "branch"), *Hd = obj(c, P, "playhead"), *Rp = obj(c, P, "replace");
    const TJV *pad = arr(c, Pl, "pad"), *fp = arr(c, Fl, "pad"), *lp = arr(c, Lb, "pad");
    int n = st->nfork < HUD_MAXFORK ? st->nfork : HUD_MAXFORK, nb = items ? items->n : 0, k, j;
    double H = lanes_h(c), py = g.y - H, lh = num(c, Ln, "h"), lg = num(c, Ln, "gap"), min_w = num(c, Ln, "min_w");
    double lo = tr.x + ends, hi = tr.x + tr.w - ends, bw = num(c, Bt, "w"), bg = num(c, Bt, "gap"), bp = num(c, Bt, "pad");
    double f0 = tj_num(tj_at(fp, 0), 0), f1 = tj_num(tj_at(fp, 1), 0), f2 = tj_num(tj_at(fp, 2), 0), f3 = tj_num(tj_at(fp, 3), 0);
    double l0 = tj_num(tj_at(lp, 0), 0), brw = num(c, Br, "w"), hw = num(c, Hd, "w"), rpw = num(c, Rp, "width");
    double x0s[HUD_MAXFORK], x1s[HUD_MAXFORK], lys[HUD_MAXFORK], bxs[HUD_MAXFORK];
    uint32_t brgb = hexrgb(str(c, Br, "colour"));
    PL ls = spec(Lb);
    char s[96], t0[16], key[HUD_KEYLEN];
#define X(t) (st->T > 0 ? tr.x + ends + (tr.w - 2 * ends) * (t) / st->T : lo)

    hot(c, box(g.x, py, g.w, H - num(c, Pl, "gap")), "dock");
    art(c, str(c, Pl, "part"), box(g.x, py, g.w, H - num(c, Pl, "gap")), 1);
    for (k = 0; k < n; k++) {
        const HUDFORK* f = &st->fork[k];
        double ly = py + tj_num(tj_at(pad, 1), 0) + k * (lh + lg), bx = X(f->from), x0 = bx, x1 = fmax(X(f->end), bx + min_w);
        double w, btx, fx;
        int on = st->timeline == k, pressed;
        R lb;
        PL p;
        if (x1 > hi) { x0 = fmax(lo, x0 - (x1 - hi)); x1 = hi; }
        w = x1 - x0;
        btx = w - bp - nb * bw - (nb - 1) * bg;
        fx = bx - x0;
        x0s[k] = x0; x1s[k] = x1; lys[k] = ly; bxs[k] = bx;
        lb = box(x0, ly, w, lh);
        snprintf(key, sizeof key, "lane:%d", k);
        pressed = on || (st->held[0] && !strcmp(st->held, key));
        art(c, str(c, Ln, pressed ? "pressed" : "part"), lb, 1);
        hot(c, lb, key);
        if (on || (f->flags & LANE_CHAIN)) {
            /* the watched lane fills to the playhead, a lane before it on the chain to where the
               next one on the chain branches off it */
            double stop = X(f->end), fh = lh - f1 - f3;
            R fb = box(bx + f0, ly + f1, w - f0 - f2, fh);
            HUDOP* a;
            for (j = 0; !on && j < n; j++)
                if ((st->fork[j].flags & LANE_CHAIN) && st->fork[j].parent == f->id) stop = X(st->fork[j].from);
            if ((a = art(c, str(c, Fl, "part"), fb, 1))) {
                a->clip = a->r;
                a->clip.w = (float)fmax(0, fmin(X(st->t), stop) - bx);
            }
        }
        clock_s(t0, sizeof t0, f->from);
        snprintf(s, sizeof s, "F%d \xC2\xB7 %s from %s", f->id, f->name, t0);     /* U+00B7 MIDDLE DOT */
        memset(&p, 0, sizeof p);
        p.pad[0] = fx + l0;
        p.pad[2] = w - btx + l0;
        {
            F lf = frame(c->l, w, lh, NULL);
            text(c, s, at(place(c, &lf, &p), x0, ly), str(c, Lb, "style"), &ls, 0, 0);
        }
        for (j = 0; j < nb; j++) {
            const TJV* it = tj_at(items, j);
            const char* act = str(c, it, "act");
            char bk[HUD_KEYLEN];
            BTN b;
            memset(&b, 0, sizeof b);
            snprintf(bk, sizeof bk, "%s:%d", act, k);
            b.key = bk;
            b.icon = obj(c, Bt, "icon");
            b.icon_name = str(c, it, "icon");
            button(c, &b, Bt, box(x0 + btx + j * (bw + bg), ly + bp, bw, lh - 2 * bp), st->armed == k && !strcmp(act, "delete"));
        }
        /* the lane a takeover would replace: outlined round its box (hud.js, an inset box-shadow) */
        if (st->replace == k) ring(c, box(x0 - rpw, ly - rpw, w + 2 * rpw, lh + 2 * rpw), rpw, hexrgb(str(c, Rp, "colour")), 1);
    }
    for (k = 0; k < n; k++) {
        for (j = 0; j < k && st->fork[j].id != st->fork[k].parent; j++) {}
        if (j < k) {
            double x = fmin(fmax(bxs[k], x0s[j]), x1s[j]);
            solid(c, box(x - brw / 2, lys[j] + lh, brw, lys[k] - lys[j] - lh), brgb, 1);
        } else {
            solid(c, box(bxs[k] - brw / 2, lys[k] + lh, brw, tr.y - lys[k] - lh), brgb, 1);
        }
    }
    solid(c, box(X(st->t) - hw / 2, py, hw, tr.y - py), hexrgb(str(c, Hd, "colour")), 1);
#undef X
}

/* ---- the column: team headers and player rows from the top bar down, as many as fit above the
   dock and its foot plate, from the first item the scroll shows -------------------------------- */

static int folded(const HUDSTATE* st, int t) { return t < 32 && (st->folded >> t) & 1u; }

/* Teams are told apart only when one of them has two players: a free-for-all, every player alone,
   has no headers, while a 2v1v1 gives each lone player a header of its own (hud.js, `headed`). */
int tagpu_hudlay_headed(const HUDSTATE* st)
{
    int t;
    if (st->nteam < 2) return 0;
    for (t = 0; t < st->nteam && t < HUD_MAXSEAT; t++)
        if (st->team_n[t] > 1) return 1;
    return 0;
}

static void header(C* c, int t, double ox, double oy);
static void row(C* c, int seat, double ox, double oy);

static void column(C* c)
{
    const HUDSTATE* st = c->st;
    const TJV *Cl = obj(c, c->L, "column"), *Fo = obj(c, Cl, "foot"), *Hd = obj(c, c->L, "header"), *Rw = obj(c, c->L, "row");
    double Cw = num(c, Cl, "w"), x = st->W - num(c, Cl, "r") - Cw, top = st->top + num(c, Cl, "t");
    double bottom = st->H - st->bottom - num(c, obj(c, c->L, "dock"), "h") - lanes_h(c) - num(c, Cl, "dock_gap");
    double Foh = num(c, Fo, "h"), Fog = num(c, Fo, "gap"), area = bottom - top - Fog - Foh;
    double gap = num(c, Cl, "gap"), hgap = num(c, Cl, "header_gap"), hh = num(c, Hd, "h"), rh = num(c, Rw, "h");
    double used = 0, yy = 0, fyy, u;
    int item[HUD_MAXSEAT * 2], nitem = 0, n = 0, t, i, k, last = 0, first;
    const int* vis;

    /* a team without a header cannot be folded, so its rows always show */
    int hd = tagpu_hudlay_headed(st);
    for (t = 0; t < st->nteam && t < HUD_MAXSEAT; t++) {
        if (hd) item[nitem++] = -1 - t;
        if (!hd || !folded(st, t))
            for (i = 0; i < st->team_n[t] && i < HUD_MAXSEAT; i++) item[nitem++] = st->team_seat[t][i];
    }
#define HOF(it, k) ((it) < 0 ? hh + ((k) ? hgap : 0) : rh)
    /* the scroll is held to the last first item from which every item to the end still fits */
    for (; last < nitem - 1; last++) {
        for (u = 0, k = last; k < nitem && u <= area; k++) u += (k > last ? gap : 0) + HOF(item[k], k - last);
        if (u <= area) break;
    }
    first = st->scroll < 0 ? 0 : st->scroll > last ? last : st->scroll;
    c->o->first = first;
    c->o->last = last;
    vis = item + first;
    /* the whole column is the HUD's: a click on a row's body is taken and does nothing, and the
       wheel scrolls it. Its box is known only below, so the hot is placed now and sized there. */
    {
        HUDOP* ch = NULL;
        int at0 = c->o->n;
        hot(c, box(x, top, Cw, 0), str(c, Cl, "act"));
        if (c->o->n > at0) ch = &c->o->op[at0];
        while (n < nitem - first && used + (n ? gap : 0) + HOF(vis[n], n) <= area) { used += (n ? gap : 0) + HOF(vis[n], n); n++; }
        if (nitem) {
            if (n < 1) n = 1;
            if (used < HOF(vis[0], 0)) used = HOF(vis[0], 0);
        }
        if (ch) ch->r.h = (float)(used + Fog + Foh);
    }
    for (k = 0; k < n; k++) {
        if (vis[k] < 0) header(c, -1 - vis[k], x, top + yy + (k ? hgap : 0));
        else row(c, vis[k], x, top + yy);
        yy += HOF(vis[k], k) + gap;
    }
    if (n < nitem) {
        const TJV *S = obj(c, Cl, "scroll"), *Bt = obj(c, S, "button"), *Tk = obj(c, S, "track"), *Th = obj(c, S, "thumb");
        const TJV* acts = arr(c, Bt, "acts");
        double tke = num(c, Tk, "ends"), ins = num(c, Th, "inset");
        double len = used - 2 * tke - 2 * ins, th = fmax(num(c, Th, "min"), len * n / nitem);
        double ty = tke + ins + (last ? (len - th) * first / last : 0);
        double bh = num(c, Bt, "h"), bw = num(c, Bt, "w"), bx = num(c, Bt, "x");
        int j;
        R tkb = box(x + num(c, Tk, "x"), top + tke, num(c, Tk, "w"), used - 2 * tke);
        art(c, str(c, Tk, "part"), tkb, 1);
        hot(c, tkb, str(c, Tk, "act"));
        art(c, str(c, Th, "part"), box(x + num(c, Th, "x"), top + ty, num(c, Th, "w"), th), 1);
        for (j = 0; j < 2; j++) {
            R b = box(x + bx, top + (j ? used - bh : 0), bw, bh);
            F bf = frame(c->l, bw, bh, NULL);
            PL p;
            memset(&p, 0, sizeof p);
            p.padf = num(c, Bt, "icon_padf");
            art(c, str(c, Bt, "part"), b, 1);
            hot(c, b, tj_str(tj_at(acts, j), ""));
            icon(c, str(c, Bt, j ? "down" : "up"), at(place(c, &bf, &p), b.x, b.y));
        }
    }
#undef HOF
    fyy = used + Fog;
    {
        const TJV *fbtn = obj(c, Fo, "button"), *btns = arr(c, Fo, "buttons");
        double Fow = num(c, Fo, "w");
        F ff = frame(c->l, Fow, Foh, str(c, Fo, "part"));
        R fb = box(x + Cw - Fow, top + fyy, Fow, Foh);
        art(c, str(c, Fo, "part"), fb, 1);
        for (i = 0; btns && i < btns->n; i++) {
            const TJV* s = tj_at(btns, i);
            const char* tg = tj_str(tj_get(s, "toggle"), NULL);
            unsigned bit = tagpu_hudlay_toggle(tg);
            BTN b;
            PL p;
            if (tg && !bit && !c->bad++ && c->err && c->errn)
                snprintf(c->err, c->errn, "layout: no toggle '%s' (tagpu_hudlay.c, TOGGLES)", tg);
            memset(&b, 0, sizeof b);
            b.key = str(c, s, "act");
            b.icon = tj_get(s, "icon");
            b.off = tj_num(tj_get(s, "off"), 0);
            memset(&p, 0, sizeof p);
            pl_in(&p, tj_get(s, "in"));
            pl_pad(&p, tj_get(fbtn, "pad"));
            button(c, &b, fbtn, at(place(c, &ff, &p), fb.x, fb.y), (st->toggles & bit) != 0);
        }
    }
}

static void header(C* c, int t, double ox, double oy)
{
    const HUDSTATE* st = c->st;
    const TJV *Hd = obj(c, c->L, "header"), *lines = arr(c, Hd, "lines"), *let = obj(c, Hd, "letter"), *val = obj(c, Hd, "value");
    const TJV *ti = obj(c, Hd, "title"), *sm = obj(c, Hd, "sum"), *fo = obj(c, Hd, "fold");
    double W = num(c, obj(c, c->L, "column"), "w"), h = num(c, Hd, "h");
    F f = frame(c->l, W, h, str(c, Hd, "part"));
    double units = 0, inc[4] = { 0, 0, 0, 0 };
    int alive = 0, i, nm = t < st->nteam ? st->team_n[t] : 0;
    char s[96], a[32], b[32], key[HUD_KEYLEN];
    PL p;

    for (i = 0; i < nm; i++) {
        const HUDSEAT* q = &st->seat[st->team_seat[t][i] % HUD_MAXSEAT];
        units += q->units;
        alive += !q->lost;
        inc[0] += q->mi; inc[1] += q->mo; inc[2] += q->ei; inc[3] += q->eo;
    }
    snprintf(key, sizeof key, "%s:%d", str(c, Hd, "act"), t);
    hot(c, box(ox, oy, W, h), key);
    art(c, str(c, Hd, "part"), box(ox, oy, W, h), 1);
    p = spec(fo);
    icon(c, str(c, fo, folded(st, t) ? "closed" : "open"), at(place(c, &f, &p), ox, oy));
    snprintf(s, sizeof s, "Team %d \xC2\xB7 %d/%d cmdrs", t + 1, alive, nm);   /* U+00B7 MIDDLE DOT */
    p = spec(ti);
    text(c, s, at(place(c, &f, &p), ox, oy), str(c, ti, "style"), &p, 0, 0);
    /* the team's units, a bare number as a row's are: the faces carry no Greek, so no sigma */
    k1(a, sizeof a, units);
    p = spec(sm);
    text(c, a, at(place(c, &f, &p), ox, oy), str(c, sm, "style"), &p, 0, 0);
    for (i = 0; lines && i < lines->n; i++) {
        const TJV* l = tj_at(lines, i);
        const char* res = str(c, l, "res");
        int e = res[0] == 'e';
        char up[2];
        up[0] = res[0] >= 'a' && res[0] <= 'z' ? (char)(res[0] - 32) : res[0];
        up[1] = 0;
        p = spec(let);
        pl_in(&p, tj_get(l, "in"));
        text(c, up, at(place(c, &f, &p), ox, oy), str(c, let, "style"), &p, c->l->g.res_rgb[e], 0);
        kK(a, sizeof a, inc[2 * e]);
        kK(b, sizeof b, inc[2 * e + 1]);
        p = spec(val);
        pl_in(&p, tj_get(l, "in"));
        signed_text(c, a, b, at(place(c, &f, &p), ox, oy), str(c, val, "style"), &p);
    }
}

static void hover(C* c, int seat, R b);

static void row(C* c, int seat, double ox, double oy)
{
    const HUDSTATE* st = c->st;
    const HUDSEAT* q = &st->seat[seat % HUD_MAXSEAT];
    const TJV *Rw = obj(c, c->L, "row"), *lines = arr(c, Rw, "lines"), *nm = obj(c, Rw, "name"), *sk = obj(c, Rw, "skull");
    const TJV *un = obj(c, Rw, "units"), *val = obj(c, Rw, "value"), *bar = obj(c, Rw, "bar"), *let = obj(c, Rw, "letter");
    const TJV *inc = obj(c, Rw, "income"), *stp = obj(c, Rw, "stripe");
    double W = num(c, obj(c, c->L, "column"), "w"), h = num(c, Rw, "h");
    F f = frame(c->l, W, h, str(c, Rw, "part"));
    unsigned saved = c->flags;
    char s[96], a[32], b[32], key[HUD_KEYLEN];
    PL p;
    R sb;
    HUDOP* o;
    int i;

    /* the row is a control of its own, under its hover's buttons: hovering it is what shows them */
    snprintf(key, sizeof key, "%s:%d", str(c, Rw, "act"), seat);
    hot(c, box(ox, oy, W, h), key);

    if (q->out) c->flags |= HOPF_DIM;
    art(c, str(c, Rw, "part"), box(ox, oy, W, h), 1);
    p = spec(stp);
    sb = at(place(c, &f, &p), ox, oy);
    if ((o = op(c, HOP_SOLID, sb))) {
        o->rgba = (q->colour << 8) | 0xFF;
        o->flags |= HOPF_SHADE;
        o->shade = (float)num(c, stp, "shade");
    }
    p = spec(nm);
    pl_pad(&p, tj_get(nm, q->lost ? "lost_pad" : "pad"));
    text(c, q->name, at(place(c, &f, &p), ox, oy), str(c, nm, "style"), &p, 0, 0);
    if (q->lost) {
        p = spec(sk);
        icon(c, str(c, sk, "icon"), at(place(c, &f, &p), ox, oy));
    }
    snprintf(s, sizeof s, "%.0f", floor(q->units + 0.5));
    p = spec(un);
    text(c, s, at(place(c, &f, &p), ox, oy), str(c, un, "style"), &p, 0, 0);
    for (i = 0; lines && i < lines->n; i++) {
        const TJV* l = tj_at(lines, i);
        const char* res = str(c, l, "res");
        int e = res[0] == 'e';
        double v = e ? q->e : q->m, mx = e ? q->emax : q->mmax, fr;
        const TJV* store = tj_get(l, "store");
        R vb, bb, lb;
        PL fit;
        char up[2];
        up[0] = res[0] >= 'a' && res[0] <= 'z' ? (char)(res[0] - 32) : res[0];
        up[1] = 0;
        p = spec(val); pl_in(&p, store); vb = at(place(c, &f, &p), ox, oy);
        memset(&fit, 0, sizeof fit);
        fit.fit = num(c, val, "fit");
        p = spec(bar); pl_in(&p, store); bb = at(place(c, &f, &p), ox, oy);
        p = spec(let); pl_in(&p, store); lb = at(place(c, &f, &p), ox, oy);
        text(c, up, lb, str(c, let, "style"), &p, c->l->g.res_rgb[e], 0);
        k1(a, sizeof a, v);
        k1(b, sizeof b, mx);
        snprintf(s, sizeof s, "%s / %s", a, b);
        text(c, s, vb, str(c, val, "style"), &fit, 0, 0);
        {
            double ed = num(c, bar, "edge");
            solid(c, bb, hexrgb(str(c, bar, "bg")), 1);
            ring(c, bb, ed, 0, 0.8);
            /* the fill's width as the mockup writes it, (100 * min(1, v / max)).toFixed(1) % */
            fr = mx > 0 ? to_fixed1(100 * fmin(1, v / mx)) / 100 : 0;
            solid(c, box(bb.x, bb.y, bb.w * fr, bb.h), c->l->g.res_rgb[e], 1);
        }
        rate(a, sizeof a, e ? q->ei : q->mi);
        rate(b, sizeof b, e ? q->eo : q->mo);
        p = spec(inc); pl_in(&p, tj_get(l, "inc"));
        signed_text(c, a, b, at(place(c, &f, &p), ox, oy), str(c, inc, "style"), &p);
    }
    if (st->mx >= ox && st->mx < ox + W && st->my >= oy && st->my < oy + h) {
        p = spec(obj(c, Rw, "hover"));
        hover(c, seat, at(place(c, &f, &p), ox, oy));
    }
    c->flags = saved;
}

/* the actions over a hovered row: one margin round the strip's edge and between its buttons. Sight
   and Follow are pressed in on the player they are set to, and a click on the pressed one undoes it */
static void hover(C* c, int seat, R b)
{
    const TJV *Hv = obj(c, c->L, "hover"), *btns = arr(c, Hv, "buttons"), *kind = obj(c, Hv, "button");
    double m = num(c, Hv, "margin");
    int n = btns ? btns->n : 0, k;
    double bw = n ? (b.w - (n + 1) * m) / n : 0;
    art(c, str(c, Hv, "part"), b, 1);
    for (k = 0; k < n; k++) {
        const TJV* s = tj_at(btns, k);
        char key[HUD_KEYLEN];
        BTN bt;
        memset(&bt, 0, sizeof bt);
        snprintf(key, sizeof key, "%s:%d", str(c, s, "act"), seat);
        bt.key = key;
        bt.icon = obj(c, kind, "icon");
        bt.icon_name = str(c, s, "icon");
        bt.textspec = obj(c, kind, "text");
        bt.text = str(c, s, "text");
        {
            const char* a = str(c, s, "act");
            int on = (!strcmp(a, "sight") && c->st->sight == seat) || (!strcmp(a, "follow") && c->st->follow == seat);
            button(c, &bt, kind, box(b.x + m + k * (bw + m), b.y + m, bw, b.h - 2 * m), on);
        }
    }
}

/* ---- the stats window: chips in two trays, the chart over the chart and legend recesses. The
   window is the HUD's whole, as the column is: a click on its body is taken and does nothing ---- */

static const TJV* measure(C* c, const char* k)
{
    const TJV* ms = arr(c, obj(c, c->L, "window"), "measures");
    int i;
    for (i = 0; ms && i < ms->n; i++)
        if (!strcmp(tj_str(tj_get(tj_at(ms, i), "k"), ""), k)) return tj_at(ms, i);
    return ms && ms->n ? tj_at(ms, 0) : NULL;
}

static void chip(C* c, const TJV* ch, R b, const char* label, int on, const char* val, double off)
{
    char key[HUD_KEYLEN];
    BTN bt;
    memset(&bt, 0, sizeof bt);
    snprintf(key, sizeof key, "chip:%s", val);
    bt.key = key;
    bt.textspec = obj(c, ch, "text");
    bt.text = label;
    bt.off = off;
    button(c, &bt, ch, b, on);
}

static void chart(C* c, R cb, double Rl);

static void window_(C* c)
{
    const HUDSTATE* st = c->st;
    const HUDSTATS* S = &st->stats;
    const TJV *Wn = obj(c, c->L, "window"), *m = measure(c, S->measure), *ch = obj(c, Wn, "chip");
    const TJV *trays = arr(c, Wn, "trays"), *ms = arr(c, Wn, "measures"), *grip = obj(c, Wn, "grip");
    const TJV *ti = obj(c, Wn, "title"), *cl = obj(c, Wn, "close"), *lab = obj(c, Wn, "label");
    const TJV *chips = obj(c, Wn, "chips"), *sp = obj(c, Wn, "split"), *chs = obj(c, Wn, "chart");
    const TJV *fo = obj(c, Wn, "foot"), *shd = arr(c, Wn, "shadow");
    double w = num(c, Wn, "w"), h = num(c, Wn, "h"), x, y, chh = num(c, ch, "h"), gap = num(c, ch, "gap");
    const char *mk = str(c, m, "k"), *mn = str(c, m, "n"), *mu = str(c, m, "u");
    char s[128], t0[16], t1[16];
    F f;
    PL p;
    R cb, lg, cw;
    HUDOP* o;
    BTN bt;
    int i, j;

    x = fmax(0, fmin(st->W - w, S->x));
    y = fmax(0, fmin(st->H - h, S->y));
    f = frame(c->l, w, h, str(c, Wn, "part"));
    if ((o = solid(c, box(x, y, w, h), 0, tj_num(tj_at(shd, 2), 0)))) {
        o->flags |= HOPF_DROP;
        o->size = (float)tj_num(tj_at(shd, 0), 0);
        o->shade = (float)tj_num(tj_at(shd, 1), 0);
    }
    /* placed before everything in it, so the hit test, last first, finds its controls over it */
    hot(c, box(x, y, w, h), str(c, Wn, "act"));
    art(c, str(c, Wn, "part"), box(x, y, w, h), 1);
    p = spec(grip);
    icon(c, str(c, grip, "icon"), at(place(c, &f, &p), x, y));
    snprintf(s, sizeof s, "Game stats \xC2\xB7 %s %s", mn, mu);          /* U+00B7 MIDDLE DOT */
    p = spec(ti);
    text(c, s, at(place(c, &f, &p), x, y), str(c, ti, "style"), &p, 0, 0);
    /* the title well is the drag handle */
    memset(&p, 0, sizeof p);
    pl_in(&p, tj_get(ti, "in"));
    hot(c, at(place(c, &f, &p), x, y), "drag");
    memset(&bt, 0, sizeof bt);
    bt.key = str(c, cl, "act");
    bt.icon = obj(c, cl, "icon");
    p = spec(cl);
    button(c, &bt, cl, at(place(c, &f, &p), x, y), 0);

    for (i = 0; trays && i < trays->n; i++) {
        const TJV* tr = tj_at(trays, i);
        const char* g = str(c, tr, "group");
        double cx;
        R tb;
        memset(&p, 0, sizeof p);
        pl_in(&p, tj_get(tr, "in"));
        tb = place(c, &f, &p);
        p = spec(lab);
        pl_in(&p, tj_get(tr, "in"));
        text(c, str(c, tr, "label"), at(place(c, &f, &p), x, y), str(c, lab, "style"), &p, 0, 0);
        cx = tb.x + num(c, ch, "start");
        for (j = 0; ms && j < ms->n; j++) {
            const TJV* q = tj_at(ms, j);
            const char* k = str(c, q, "k");
            double qw;
            if (strcmp(str(c, q, "g"), g)) continue;
            qw = num(c, chips, k);
            chip(c, ch, box(x + cx, y + tb.y + (tb.h - chh) / 2, qw, chh), str(c, q, "n"), !strcmp(k, mk), k, 0);
            cx += qw + gap;
        }
        if (tj_is(tj_get(tr, "split"), TJ_TRUE)) {
            const TJV* sc = arr(c, sp, "chips");
            double r = tb.x + tb.w - num(c, sp, "end");
            for (j = sc ? sc->n - 1 : -1; j >= 0; j--) {
                const TJV* q = tj_at(sc, j);
                const char* v = str(c, q, "value");
                double qw = num(c, q, "w");
                int team = !strcmp(v, "team");
                chip(c, ch, box(x + r - qw, y + tb.y + (tb.h - chh) / 2, qw, chh), str(c, q, "text"),
                     team == (S->team != 0), v, team && !tagpu_hudlay_headed(st) ? num(c, sp, "off") : 0);
                r -= qw + gap;
            }
        }
    }
    p = spec(chs);
    cb = place(c, &f, &p);
    memset(&p, 0, sizeof p);
    pl_in(&p, tj_get(chs, "legend"));
    lg = place(c, &f, &p);
    memset(&p, 0, sizeof p);
    pl_in(&p, tj_at(tj_get(chs, "in"), 0));
    cw = place(c, &f, &p);
    chart(c, at(cb, x, y), lg.x + lg.w - (cw.x + cw.w) + 1);
    clock_s(t0, sizeof t0, st->t);
    clock_s(t1, sizeof t1, st->T);
    /* "<Name> <unit> · <t> / <T>", NO-BREAK SPACEs round the dot */
    snprintf(s, sizeof s, "%c%s %s\xC2\xA0\xC2\xB7\xC2\xA0%s / %s",
             mn[0] >= 'a' && mn[0] <= 'z' ? mn[0] - 32 : mn[0], mn[0] ? mn + 1 : "", mu, t0, t1);
    p = spec(fo);
    text(c, s, at(place(c, &f, &p), x, y), str(c, fo, "style"), &p, 0, 0);
}

/* hud.js statFormat: metal income in tenths below 100, thousands in k from 10 000 on, whole
   numbers otherwise */
static void stat_format(char* o, size_t n, const char* k, double v)
{
    if (!strcmp(k, "metal") && v < 100) snprintf(o, n, "%.1f", to_fixed1(v));
    else if (v >= 10000) {
        if (v >= 100000) snprintf(o, n, "%.0fk", js_round(v / 1000));
        else snprintf(o, n, "%.1fk", to_fixed1(v / 1000));
    } else jsnum(o, n, js_round(v));
}

/* hud.js nice: the axis's top, 1, 2, 2.5 or 5 times a power of ten at or above v */
static double nice(double v)
{
    double q, f;
    if (!(v > 0)) return 1;
    q = pow(10, floor(log10(v)));
    f = v / q;
    return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * q;
}

static int dark(C* c, uint32_t rgb, const TJV* Ln)
{
    return .3 * (rgb >> 16) + .59 * ((rgb >> 8) & 255) + .11 * (rgb & 255) < num(c, Ln, "dark");
}

/* an SVG <text> of the chart: anchored at (x, y) as hud.js's `tx` writes them, to a tenth */
static void text_at(C* c, const char* s, R org, double x, double y, int anchor, double size, uint32_t rgb, R clip)
{
    int st = style_index(c->l, str(c, obj(c, c->L, "chart"), "style"));
    HUDOP* p;
    HUDRUN* r;
    int off;
    if (st < 0) {
        if (!c->bad++ && c->err && c->errn) snprintf(c->err, c->errn, "layout: no style for the chart");
        return;
    }
    if (c->o->nrun + 1 > HUD_MAXRUN) { c->o->overflow = 1; return; }
    if (!(p = op(c, HOP_TEXT, box(org.x + to_fixed1(x), org.y + to_fixed1(y), 0, 0)))) return;
    p->flags |= HOPF_BASE;
    p->style = (int16_t)st;
    p->centre = (uint8_t)anchor;
    p->size = (float)size;
    p->clip.x = (float)clip.x; p->clip.y = (float)clip.y; p->clip.w = (float)clip.w; p->clip.h = (float)clip.h;
    if ((off = text_put(c, s)) < 0) return;
    r = &c->o->run[c->o->nrun++];
    r->rgb = rgb;
    r->resource = 0;
    r->off = (uint16_t)off;
    r->len = (uint16_t)strlen(s);
    p->run = (uint16_t)(r - c->o->run);
    p->nrun = 1;
}

static void line(C* c, R org, double x0, double y0, double x1, double y1, double w, uint32_t rgb, double alpha)
{
    HUDOP* p = op(c, HOP_LINE, box(org.x + x0, org.y + y0, x1 - x0, y1 - y0));
    if (!p) return;
    p->size = (float)w;
    p->rgba = (rgb << 8) | (uint32_t)lround(255.0 * alpha);
}

/* ---- the chart: one line per player or team; the part after the playhead faint; the legend
   ranked at the playhead. Sample s is at t0 + s * dt seconds; the minute labels step by the first
   of axis.every that puts them axis.space apart, and one nearer the playhead's label than that is
   left out. In the chart's own units from cb's top left, as
   the SVG's viewBox has them. -------------------------------------------------------------------- */

static void chart(C* c, R cb, double Rl)
{
    const HUDSTATE* st = c->st;
    const HUDSTATS* S = &st->stats;
    const TJV *Ch = obj(c, c->L, "chart"), *Pl = obj(c, Ch, "plot"), *A = obj(c, Ch, "axis"), *Gr = obj(c, Ch, "grid");
    const TJV *Nw = obj(c, Ch, "now"), *Ln = obj(c, Ch, "line"), *Lg = obj(c, Ch, "legend"), *ev = arr(c, A, "every");
    const char* mk = str(c, measure(c, S->measure), "k");
    double W = cb.w, H = cb.h, Lp = num(c, Pl, "l"), Tp = num(c, Pl, "t"), pw = W - Lp - Rl, ph = H - Tp - num(c, Pl, "b");
    double T = st->T > 0 ? st->T : 1, DT = S->dt, mx = -HUGE_VAL, xph, step, t, gap = num(c, A, "gap"), asz = num(c, A, "size");
    double below = num(c, A, "below"), lx, ly, lw, lh, rh, fs, sw, sy, sh, tx, ty, th;
    uint32_t acol = hexrgb(str(c, A, "colour")), ncol = hexrgb(str(c, Nw, "colour"));
    uint32_t halo = hexrgb(str(c, Ln, "halo")), edge = hexrgb(str(c, Lg, "swatch_edge"));
    int ng = S->ngroup < HUD_MAXSEAT ? S->ngroup : HUD_MAXSEAT, NS = ng && DT > 0 ? (S->npt < HUD_MAXPT ? S->npt : HUD_MAXPT) : 0;
    int now = 0, g, s, k, rank[HUD_MAXSEAT];
    HUDCHART* K = &c->o->chart;
    HUDOP* o;
    char b[32];
    double v;
    R past;

    if (NS) {
        now = (int)js_round((st->t - S->t0) / DT);
        now = now > NS - 1 ? NS - 1 : now < 0 ? 0 : now;
    }
    for (g = 0; g < ng; g++)
        for (s = 0; s < NS; s++) mx = fmax(mx, S->group[g].v[s]);
    mx = nice(mx);
#define X(i) (Lp + pw * (S->t0 + (i) * DT) / T)
#define Y(val) (Tp + ph * (1 - (val) / mx))
    xph = Lp + pw * st->t / T;
    for (k = 0; k < 3; k++) {
        double y;
        v = k * mx / 2;
        y = Y(v);
        line(c, cb, Lp, y, Lp + pw, y, num(c, Gr, "width"), hexrgb(str(c, Gr, "colour")), num(c, Gr, "opacity"));
        stat_format(b, sizeof b, mk, v);
        text_at(c, b, cb, Lp - gap, y + num(c, A, "baseline"), 2, asz, acol, cb);
    }
    step = tj_num(tj_at(ev, ev ? ev->n - 1 : 0), 0);
    for (k = 0; ev && k < ev->n; k++)
        if (pw * tj_num(tj_at(ev, k), 0) / T >= num(c, A, "space")) { step = tj_num(tj_at(ev, k), 0); break; }
    /* a minute label nearer the playhead's than the labels' own spacing would lie under it */
    for (t = 0; step > 0 && t <= T; t += step) {
        if (fabs(Lp + pw * t / T - xph) < num(c, A, "space")) continue;
        clock_s(b, sizeof b, t);
        text_at(c, b, cb, Lp + pw * t / T, Tp + ph + below, 1, asz, acol, cb);
    }

    /* the lines: the points to a tenth, as the polylines' `points` are written; the cross at the
       sample of the death, unrounded, as its path is */
    memset(K, 0, sizeof *K - sizeof K->pt);
    K->n = ng;
    K->npt = NS;
    K->w = (float)num(c, Ln, "width");
    K->halo_w = K->w + 1;
    K->cross = (float)num(c, Ln, "cross");
    K->cross_w = (float)num(c, Ln, "cross_w");
    K->halo_rgb = halo;
    K->halo_a = (float)num(c, Ln, "halo_opacity");
    for (g = 0; g < ng; g++) {
        const HUDGROUP* G = &S->group[g];
        K->g[g].rgb = G->colour;
        K->g[g].dark = dark(c, G->colour, Ln);
        for (s = 0; s < NS; s++) {
            K->pt[g][s][0] = (float)(cb.x + to_fixed1(X(s)));
            K->pt[g][s][1] = (float)(cb.y + to_fixed1(Y(G->v[s])));
        }
        s = G->death >= 0 ? (int)js_round((G->death - S->t0) / DT) : -1;
        if (s >= 0 && s < NS) {
            K->g[g].cross = 1;
            K->g[g].cx = (float)(cb.x + X(s));
            K->g[g].cy = (float)(cb.y + Y(G->v[s]));
        }
    }
    /* the whole faint, then the part before the playhead over it: clipPath `past`, its width to
       a tenth */
    if ((o = op(c, HOP_CHART, cb))) {
        o->clip = o->r;
        o->rgba = 0xFFFFFF00u | (uint32_t)lround(255.0 * num(c, Ln, "faint"));
    }
    past = box(cb.x, cb.y, fmax(0, fmin(W, to_fixed1(xph))), H);
    if ((o = op(c, HOP_CHART, cb))) {
        o->clip.x = (float)past.x; o->clip.y = (float)past.y; o->clip.w = (float)past.w; o->clip.h = (float)past.h;
    }
    line(c, cb, xph, Tp - num(c, Nw, "over"), xph, Tp + ph + num(c, Nw, "over"), num(c, Nw, "width"), ncol, 1);
    clock_s(b, sizeof b, st->t);
    text_at(c, b, cb, xph, Tp + ph + below, 1, asz, ncol, cb);

    /* the legend, ranked by the value at the playhead; equal values keep the groups' order, as
       JavaScript's sort does */
    lx = W - Rl + num(c, Lg, "x") + num(c, Lg, "pad");
    ly = Tp + num(c, Lg, "pad");
    lw = Rl - num(c, Lg, "r") - 2 * num(c, Lg, "pad");
    lh = ph - 2 * num(c, Lg, "pad");
    for (g = 0; g < ng; g++) {
        double vg = NS ? S->group[g].v[now] : 0;
        for (k = g; k > 0 && (NS ? S->group[rank[k - 1]].v[now] : 0) < vg; k--) rank[k] = rank[k - 1];
        rank[k] = g;
    }
    rh = fmin(num(c, Lg, "row"), ng ? lh / ng : HUGE_VAL);
    fs = fmin(num(c, Lg, "size"), rh * num(c, Lg, "text_h"));
    sw = num(c, Lg, "swatch");
    sy = num(c, Lg, "swatch_y");
    sh = num(c, Lg, "swatch_h");
    tx = num(c, Lg, "text_x");
    ty = num(c, Lg, "text_y");
    th = num(c, Lg, "swatch_edge_w");
    for (k = 0; k < ng; k++) {
        const HUDGROUP* G = &S->group[rank[k]];
        double yy = ly + k * rh;
        R sb = box(cb.x + lx, cb.y + yy + rh * sy, sw, rh * sh);
        solid(c, sb, G->colour, 1);
        /* the stroke on the swatch's edge, half in and half out */
        if (dark(c, G->colour, Ln)) ring(c, box(sb.x - th / 2, sb.y - th / 2, sb.w + th, sb.h + th), th, edge, 1);
        text_at(c, G->name, cb, lx + tx, yy + rh * ty, 0, fs, hexrgb(str(c, Lg, "name")), cb);
        stat_format(b, sizeof b, mk, NS ? G->v[now] : 0);
        text_at(c, b, cb, lx + lw, yy + rh * ty, 2, fs, hexrgb(str(c, Lg, "value")), cb);
    }
#undef X
#undef Y
}

/* ---- the toast: a short message in a dark box centred across the screen, above everything; the box
   is the layout's size, not the text's, since this file has no faces to measure text with ------- */

static void toast(C* c)
{
    const HUDSTATE* st = c->st;
    const TJV* Ts = obj(c, c->L, "toast");
    double w = num(c, Ts, "w"), h = num(c, Ts, "h"), e = num(c, Ts, "edge");
    R b = box((st->W - w) / 2, st->H * num(c, Ts, "top"), w, h);
    uint32_t ec = hexrgb(str(c, Ts, "edge_colour"));
    PL p;
    solid(c, b, hexrgb(str(c, Ts, "bg")), num(c, Ts, "alpha"));
    ring(c, b, e, ec, 1);
    memset(&p, 0, sizeof p);
    p.centre = 1;
    text(c, st->toast, b, str(c, Ts, "style"), &p, 0, 0);
}

/* ---- the entry points ------------------------------------------------------------------------- */

int tagpu_hudlay_build(const HUDLAY* l, const HUDSTATE* st, HUDOPS* out, char* err, size_t errn)
{
    C c;
    memset(&c, 0, sizeof c);
    c.l = l; c.L = l->L; c.st = st; c.o = out; c.err = err; c.errn = errn;
    out->n = out->nrun = out->ntext = out->overflow = 0;
    out->first = out->last = 0;
    out->track_x0 = out->track_x1 = 0;
    if (err && errn) err[0] = 0;
    dock(&c);
    column(&c);
    if (st->stats.open) window_(&c);
    if (st->toast[0]) toast(&c);
    return !c.bad;
}

/* The kit's drawing, as quads. A grid-sliced part is drawn cell by cell, the cells sharing
   their edges; a 9-slice part as CSS border-image draws it, its slice's edges kept at their size
   in units unless the box is smaller than both together, when all four shrink by one factor;
   a tiled fill scaled to its height and repeated along x at a whole-pixel period; an icon kept
   to its aspect and centred. An `xtile` part's x bands are each one period of a seamless tile
   (seamband.py): drawn as a whole number of copies at about their painted length, each copy an
   equal share of the drawn band, as hud.js composes a grid part and as CSS border-image-repeat
   `round` draws a 9-slice's middle. */
float tagpu_hudlay_baseline(float y, float h, float size, float asc, float desc)
{
    float A = floorf(asc * size + 0.5f), D = floorf(desc * size + 0.5f);
    return y + (h - size) / 2 + A + floorf((size - A - D) / 2);
}

/* How many copies of a band `period` px long fill `len` px: the nearest whole number, at least one,
   and at most 1 + `extra`, the copies the caller's quads have room for. */
static int copies(double len, double period, int extra)
{
    double n = period > 0 ? floor(len / period + 0.5) : 1;
    if (n < 1) n = 1;
    if (extra < 0) extra = 0;   /* a cap below one quad a cell still draws every cell once */
    return n > 1 + extra ? 1 + extra : (int)n;
}

int tagpu_hudlay_expand(const HUDLAY* l, const HUDOP* op, float px, HUDQUAD* q, int cap)
{
    const HUDPART* k;
    double x = op->r.x * px, y = op->r.y * px, w = op->r.w * px, h = op->r.h * px;
    int n = 0, i, j;
    if (op->kind != HOP_PART || op->part < 0 || op->part >= l->nparts || cap < 1) return 0;
    k = &l->part[op->part];
    switch (k->kind) {
    case HK_GRID: {
        double bx[2 * HUD_MAXBAND + 2], by[2 * HUD_MAXBAND + 2];
        int nx = 0, ny = 0, extra;
        F f;
        f.w = op->r.w; f.h = op->r.h; f.k = k;
        bx[nx++] = 0; for (i = 0; i < k->nxs; i++) { bx[nx++] = k->xs[i][0]; bx[nx++] = k->xs[i][1]; } bx[nx++] = k->design[0];
        by[ny++] = 0; for (i = 0; i < k->nys; i++) { by[ny++] = k->ys[i][0]; by[ny++] = k->ys[i][1]; } by[ny++] = k->design[1];
        /* THE COPIES ARE BOUNDED BY `cap`: every row gets cap / rows quads, its nx - 1 cells take one
           each, and each band may add an equal share of what is left. */
        extra = k->nxs ? (cap / (ny - 1) - (nx - 1)) / k->nxs : 0;
        for (j = 0; j + 1 < ny; j++)
            for (i = 0; i + 1 < nx; i++) {
                double x0 = x + fx(&f, bx[i]) * px, x1 = x + fx(&f, bx[i + 1]) * px;
                double y0 = y + fy(&f, by[j]) * px, y1 = y + fy(&f, by[j + 1]) * px;
                int c, m = k->xtile && i % 2 ? copies(x1 - x0, (bx[i + 1] - bx[i]) * px, extra) : 1;
                if (x1 <= x0 || y1 <= y0) continue;
                for (c = 0; c < m && n < cap; c++, n++) {
                    q[n].x0 = (float)(x0 + (x1 - x0) * c / m); q[n].x1 = (float)(x0 + (x1 - x0) * (c + 1) / m);
                    q[n].y0 = (float)y0; q[n].y1 = (float)y1;
                    q[n].u0 = (float)(bx[i] / k->design[0]); q[n].u1 = (float)(bx[i + 1] / k->design[0]);
                    q[n].v0 = (float)(by[j] / k->design[1]); q[n].v1 = (float)(by[j + 1] / k->design[1]);
                }
            }
        return n;
    }
    case HK_NINE: {
        double t = k->slice[0] / k->scale * px, r = k->slice[1] / k->scale * px;
        double b = k->slice[2] / k->scale * px, le = k->slice[3] / k->scale * px, f = 1;
        double dx[4], dy[4], su[4], sv[4];
        int m;
        if (le + r > 0) f = fmin(f, w / (le + r));
        if (t + b > 0) f = fmin(f, h / (t + b));
        t *= f; r *= f; b *= f; le *= f;
        dx[0] = x; dx[1] = x + le; dx[2] = x + w - r; dx[3] = x + w;
        dy[0] = y; dy[1] = y + t; dy[2] = y + h - b; dy[3] = y + h;
        su[0] = 0; su[1] = k->slice[3] / k->pw; su[2] = 1 - k->slice[1] / k->pw; su[3] = 1;
        sv[0] = 0; sv[1] = k->slice[0] / k->ph; sv[2] = 1 - k->slice[2] / k->ph; sv[3] = 1;
        /* the middle's period is its painted width at the edges' scale (px * f / scale), as CSS sizes a
           `round` tile from the top edge's; each of the three rows has cap / 3 quads, two of them ends */
        m = k->xtile ? copies(dx[2] - dx[1], (k->pw - k->slice[1] - k->slice[3]) * px * f / k->scale, cap / 3 - 3) : 1;
        for (j = 0; j < 3; j++)
            for (i = 0; i < 3; i++) {
                int c, mi = i == 1 ? m : 1;
                if (dx[i + 1] <= dx[i] || dy[j + 1] <= dy[j]) continue;
                for (c = 0; c < mi && n < cap; c++, n++) {
                    q[n].x0 = (float)(dx[i] + (dx[i + 1] - dx[i]) * c / mi); q[n].x1 = (float)(dx[i] + (dx[i + 1] - dx[i]) * (c + 1) / mi);
                    q[n].y0 = (float)dy[j]; q[n].y1 = (float)dy[j + 1];
                    q[n].u0 = (float)su[i]; q[n].u1 = (float)su[i + 1]; q[n].v0 = (float)sv[j]; q[n].v1 = (float)sv[j + 1];
                }
            }
        return n;
    }
    case HK_TILE: {
        double ch = fmax(1, floor(h + 0.5)), tw = fmax(1, floor(k->pw * ch / k->ph + 0.5));
        q[0].x0 = (float)x; q[0].x1 = (float)(x + w); q[0].y0 = (float)y; q[0].y1 = (float)(y + h);
        q[0].u0 = 0; q[0].u1 = (float)(w / tw); q[0].v0 = 0; q[0].v1 = 1;
        return 1;
    }
    case HK_ICON: {
        double s = fmin(w / k->pw, h / k->ph), iw = k->pw * s, ih = k->ph * s;
        q[0].x0 = (float)(x + (w - iw) / 2); q[0].x1 = q[0].x0 + (float)iw;
        q[0].y0 = (float)(y + (h - ih) / 2); q[0].y1 = q[0].y0 + (float)ih;
        q[0].u0 = 0; q[0].u1 = 1; q[0].v0 = 0; q[0].v1 = 1;
        return 1;
    }
    default:
        q[0].x0 = (float)x; q[0].x1 = (float)(x + w); q[0].y0 = (float)y; q[0].y1 = (float)(y + h);
        q[0].u0 = 0; q[0].u1 = 1; q[0].v0 = 0; q[0].v1 = 1;
        return 1;
    }
}

/* ---- loading ----------------------------------------------------------------------------------- */

static int read_bands(const TJV* a, float (*out)[2], int* n)
{
    int i;
    *n = 0;
    if (!a) return 1;
    if (a->type != TJ_ARR || a->n > HUD_MAXBAND) return 0;
    for (i = 0; i < a->n; i++) {
        const TJV* b = tj_at(a, i);
        if (!tj_is(b, TJ_ARR) || b->n != 2) return 0;
        out[i][0] = (float)tj_num(tj_at(b, 0), 0);
        out[i][1] = (float)tj_num(tj_at(b, 1), 0);
    }
    *n = a->n;
    return 1;
}

static int load_kit(HUDLAY* l, const TJV* parts, HUDPNGSIZE png_size, void* ctx, char* err, size_t errn)
{
    const TJV* e;
    if (!tj_is(parts, TJ_OBJ)) { snprintf(err, errn, "kit: no `parts` object"); return 0; }
    for (e = parts->first; e; e = e->next) {
        HUDPART* k;
        const char* kind = tj_str(tj_get(e, "kind"), "");
        if (l->nparts >= HUD_MAXPART) { snprintf(err, errn, "kit: more than %d parts", HUD_MAXPART); return 0; }
        k = &l->part[l->nparts];
        memset(k, 0, sizeof *k);
        if (strlen(e->key) >= sizeof k->name) { snprintf(err, errn, "kit: part name '%s' too long", e->key); return 0; }
        strcpy(k->name, e->key);
        k->kind = !strcmp(kind, "grid") ? HK_GRID : !strcmp(kind, "nine") ? HK_NINE : !strcmp(kind, "tile") ? HK_TILE
                : !strcmp(kind, "whole") ? HK_WHOLE : !strcmp(kind, "icon") ? HK_ICON : -1;
        k->scale = (float)tj_num(tj_get(e, "scale"), 0);
        if (k->kind < 0 || !(k->scale > 0)) { snprintf(err, errn, "kit: part '%s' has no kind or scale", k->name); return 0; }
        if (!png_size(ctx, k->name, &k->pw, &k->ph) || k->pw < 1 || k->ph < 1) {
            snprintf(err, errn, "kit: part '%s' has no PNG", k->name);
            return 0;
        }
        k->xtile = tj_is(tj_get(e, "xtile"), TJ_TRUE);
        if (k->kind == HK_GRID) {
            const TJV* d = tj_get(e, "design");
            k->design[0] = (float)tj_num(tj_at(d, 0), 0);
            k->design[1] = (float)tj_num(tj_at(d, 1), 0);
            k->wells = tj_get(e, "wells");
            if (!(k->design[0] > 0 && k->design[1] > 0) || !read_bands(tj_get(e, "xs"), k->xs, &k->nxs)
                || !read_bands(tj_get(e, "ys"), k->ys, &k->nys) || !tj_is(k->wells, TJ_OBJ)) {
                snprintf(err, errn, "kit: grid part '%s' lacks its design, bands or wells", k->name);
                return 0;
            }
        }
        if (k->kind == HK_NINE) {
            const TJV* s = tj_get(e, "slice");
            int i;
            if (!tj_is(s, TJ_ARR) || s->n != 4) { snprintf(err, errn, "kit: 9-slice part '%s' has no slice", k->name); return 0; }
            for (i = 0; i < 4; i++) k->slice[i] = (float)tj_num(tj_at(s, i), 0);
        }
        l->nparts++;
    }
    return 1;
}

static int load_globals(HUDLAY* l, char* err, size_t errn)
{
    C c;
    const TJV *styles, *s, *v;
    memset(&c, 0, sizeof c);
    c.l = l; c.L = l->L; c.err = err; c.errn = errn;
    l->g.press_shift = (float)num(&c, l->L, "press_shift");
    v = obj(&c, l->L, "dim");
    l->g.dim_brightness = (float)num(&c, v, "brightness");
    l->g.dim_saturate = (float)num(&c, v, "saturate");
    v = obj(&c, l->L, "resource");
    l->g.res_rgb[0] = hexrgb(str(&c, v, "m"));
    l->g.res_rgb[1] = hexrgb(str(&c, v, "e"));
    v = obj(&c, l->L, "signs");
    l->g.pos_rgb = hexrgb(str(&c, v, "pos"));
    l->g.neg_rgb = hexrgb(str(&c, v, "neg"));
    l->g.track_ends = (float)num(&c, obj(&c, obj(&c, obj(&c, l->L, "dock"), "rail"), "track"), "ends");
    l->g.toast_ms = (unsigned)num(&c, obj(&c, l->L, "toast"), "ms");
    v = arr(&c, obj(&c, l->L, "chart"), "teams");
    for (l->g.nteam_rgb = 0; v && l->g.nteam_rgb < v->n && l->g.nteam_rgb < 8; l->g.nteam_rgb++)
        l->g.team_rgb[l->g.nteam_rgb] = hexrgb(tj_str(tj_at(v, l->g.nteam_rgb), ""));
    if (!c.bad && !l->g.nteam_rgb) { snprintf(err, errn, "layout: chart.teams is empty"); return 0; }
    v = obj(&c, l->L, "window");
    l->g.win_w = (float)num(&c, v, "w");
    l->g.win_h = (float)num(&c, v, "h");
    l->g.win_x = (float)num(&c, v, "x");
    l->g.win_y = (float)num(&c, v, "y");
    styles = obj(&c, l->L, "styles");
    for (s = styles ? styles->first : NULL; s && !c.bad; s = s->next) {
        struct HUDSTYLE* y;
        const char* col;
        if (l->g.nstyle >= (int)(sizeof l->g.style / sizeof l->g.style[0])) { snprintf(err, errn, "layout: too many styles"); return 0; }
        y = &l->g.style[l->g.nstyle++];
        snprintf(y->name, sizeof y->name, "%s", s->key);
        snprintf(y->font, sizeof y->font, "%s", str(&c, s, "font"));
        y->weight = (int)num(&c, s, "weight");
        y->size = (float)num(&c, s, "size");
        col = str(&c, s, "colour");
        y->resource = !strcmp(col, "resource");
        y->rgb = y->resource ? 0 : hexrgb(col);
        y->upper = tj_is(tj_get(s, "upper"), TJ_TRUE);
        y->spacing = (float)num(&c, s, "spacing");
        if ((v = tj_get(s, "outline"))) {
            int i;
            y->outline = 1;
            for (i = 0; i < 4; i++) y->ol[i] = (float)tj_num(tj_at(v, i), 0);
        } else
            y->shadow = (float)num(&c, s, "shadow");
    }
    return !c.bad;
}

/* THE HIT MAP'S BOUND: one term for each place the rules emit a HOP_HOT op, the most it can emit.
   A list's term is the layout's own length; a count the state sets is the code's maximum. */
static int max_hot(HUDLAY* l, char* err, size_t errn)
{
    C c;
    const TJV *D, *Cl, *Wn, *trays, *ms, *sp, *v;
    int n = 0, i, j;
    memset(&c, 0, sizeof c);
    c.l = l; c.L = l->L; c.err = err; c.errn = errn;
    D = obj(&c, l->L, "dock");
    Cl = obj(&c, l->L, "column");
    Wn = obj(&c, l->L, "window");
    n += 2;                                                     /* dock: the plate, the housing */
    if ((v = arr(&c, obj(&c, D, "transport"), "buttons"))) n += v->n;
    n += 1;                                                     /* dock: the track */
    n += 2;                                                     /* takeover(), forksBay() */
    n += 1;                                                     /* column: its plate */
    /* the column's headers and rows: a headed column of HUD_MAXSEAT seats has one team of two at
       least, so at most HUD_MAXSEAT - 1 teams */
    n += 2 * HUD_MAXSEAT - 1;
    n += 3;                                                     /* column: the scroll track and buttons */
    if ((v = arr(&c, obj(&c, l->L, "hover"), "buttons"))) n += v->n;   /* the one hovered row */
    if ((v = arr(&c, obj(&c, Cl, "foot"), "buttons"))) n += v->n;
    n += 3;                                                     /* window: the plate, the drag, close */
    trays = arr(&c, Wn, "trays");
    ms = arr(&c, Wn, "measures");
    sp = arr(&c, obj(&c, Wn, "split"), "chips");
    for (i = 0; trays && i < trays->n; i++) {
        const TJV* tr = tj_at(trays, i);
        const char* g = tj_str(tj_get(tr, "group"), "");
        for (j = 0; ms && j < ms->n; j++)
            if (!strcmp(tj_str(tj_get(tj_at(ms, j), "g"), ""), g)) n++;
        if (tj_is(tj_get(tr, "split"), TJ_TRUE) && sp) n += sp->n;
    }
    n += 1 + HUD_MAXSEAT;                                       /* picker(): the plate, a row a seat */
    /* lanes(): the plate, and for each fork a lane and its buttons */
    if ((v = arr(&c, obj(&c, obj(&c, obj(&c, obj(&c, l->L, "proposed"), "forks"), "lane"), "buttons"), "items")))
        n += 1 + HUD_MAXFORK * (1 + v->n);
    return c.bad ? -1 : n;
}

/* a build's controls against the bound: a hot() site the bound has no term for */
static int hot_fits(const HUDLAY* l, const HUDOPS* o, const char* what, char* err, size_t errn)
{
    int i, n = 0;
    for (i = 0; i < o->n; i++) n += o->op[i].kind == HOP_HOT;
    if (n <= l->max_hot) return 1;
    snprintf(err, errn, "hudlay: the self-test's %s makes %d controls, above the bound of %d "
             "(tagpu_hudlay_max_hot has no term for one)", what, n, l->max_hot);
    return 0;
}

HUDLAY* tagpu_hudlay_load(const char* layout, size_t nlayout, const char* kit, size_t nkit,
                          HUDPNGSIZE png_size, void* ctx, char* err, size_t errn)
{
    HUDLAY* l = (HUDLAY*)calloc(1, sizeof(HUDLAY));
    HUDSTATE* st = (HUDSTATE*)calloc(1, sizeof(HUDSTATE));
    HUDOPS* o = (HUDOPS*)malloc(sizeof(HUDOPS));
    int i, ok = 0;
    char e2[200];
    if (err && errn) err[0] = 0;
    if (!l || !st || !o) { snprintf(err, errn, "hudlay: out of memory"); goto done; }
    if (!(l->ldoc = tagpu_json_parse(layout, nlayout, e2, sizeof e2))) { snprintf(err, errn, "layout.json: %s", e2); goto done; }
    if (!(l->kdoc = tagpu_json_parse(kit, nkit, e2, sizeof e2))) { snprintf(err, errn, "kit.json: %s", e2); goto done; }
    l->L = tagpu_json_root(l->ldoc);
    if (!load_kit(l, tj_get(tagpu_json_root(l->kdoc), "parts"), png_size, ctx, err, errn)) goto done;
    if (!load_globals(l, err, errn)) goto done;
    if ((l->max_hot = max_hot(l, err, errn)) < 0) goto done;
    if (l->max_hot > HUD_MAXHOT) {
        snprintf(err, errn, "layout: the layout makes up to %d controls; the hit map holds %d", l->max_hot, HUD_MAXHOT);
        goto done;
    }

    /* Every branch once: teams with headers, a folded team, a lost commander, a player out of
       the game, each marker kind, a hovered row with Sight on, a held button, every toggle, a
       toast, the stats window with a dark line and a commander's cross, and a column too short
       for its items and scrolled, so the scroll bar is drawn; then the forks' lanes open. */
    st->W = 1280; st->H = 720; st->panel = 128; st->top = 32; st->bottom = 32;
    st->t = 30; st->T = 60; st->playing = 1; st->speed = 1; st->hud = 1;
    st->nseat = HUD_MAXSEAT;
    for (i = 0; i < HUD_MAXSEAT; i++) {
        HUDSEAT* q = &st->seat[i];
        snprintf(q->name, sizeof q->name, "P%d", i);
        q->colour = 0x808080; q->units = 10; q->lost = i == 1; q->out = i == 1;
        q->m = 50; q->mmax = 100; q->mi = 1; q->mo = 1; q->e = 50; q->emax = 100; q->ei = 1; q->eo = 1;
        st->team_seat[i % 3][st->team_n[i % 3]++] = (uint8_t)i;
    }
    st->nteam = 3;
    st->folded = 4;
    st->toggles = HUD_TOG_STATS | HUD_TOG_CURSORS | HUD_TOG_CLICKS | HUD_TOG_SELECTIONS | HUD_TOG_CHAT;
    st->sight = 0; st->follow = -1;
    strcpy(st->toast, "toast");
    st->nmark = 3;
    for (i = 0; i < 3; i++) { st->mark[i].t = 10.0f * (float)i; st->mark[i].kind = i; st->mark[i].seat = i == 2 ? -1 : 1; }
    strcpy(st->held, "play");
    st->mx = 1280 - 8 - 250 + 125; st->my = 32 + 8 + 25 + 2 + 10;
    st->stats.open = 1;
    st->stats.x = 150; st->stats.y = 56;
    strcpy(st->stats.measure, "metal");
    st->stats.dt = 1;
    st->stats.npt = 3;
    st->stats.ngroup = 2;
    for (i = 0; i < 2; i++) {
        HUDGROUP* g = &st->stats.group[i];
        snprintf(g->name, sizeof g->name, "P%d", i);
        g->colour = i ? 0xFFFFFF : 0x202020;
        g->death = i ? -1.0f : 1.0f;
        g->v[0] = 1; g->v[1] = 2; g->v[2] = 3;
    }
    if (!tagpu_hudlay_build(l, st, o, err, errn) || !hot_fits(l, o, "first build", err, errn)) goto done;
    st->H = 300; st->scroll = 1;    /* the scroll bar */
    if (!tagpu_hudlay_build(l, st, o, err, errn) || !hot_fits(l, o, "scrolled build", err, errn)) goto done;
    /* the lanes, every fork the packet carries: a fork of the original on the chain, a fork of it
       watched, whose line rises to its parent, and a short one at the track's right end, clamped
       there, its Delete armed and outlined as the one a takeover replaces; the first lane held */
    st->H = 720; st->scroll = 0;
    st->T = 90; st->ends = 60; st->len = 40; st->fill_to = 10;
    st->nfork = HUD_MAXFORK;
    for (i = 0; i < HUD_MAXFORK; i++) {
        HUDFORK* f = &st->fork[i];
        static const float from[HUD_MAXFORK] = { 10, 20, 88 }, end[HUD_MAXFORK] = { 85, 40, 89 };
        f->id = i + 1;
        snprintf(f->name, sizeof f->name, "P%d", i);
        f->from = from[i];
        f->end = end[i];
        f->parent = i == 1 ? 1 : 0;
        f->flags = i == 0 ? LANE_CHAIN : i == 1 ? 1 | LANE_CHAIN : 0;   /* watched (TAGPU_PK_FORK_WATCHED) */
    }
    st->timeline = 1;
    st->lanes_open = 1;
    st->armed = st->replace = 2;
    strcpy(st->held, "lane:0");
    if (!tagpu_hudlay_build(l, st, o, err, errn) || !hot_fits(l, o, "lanes build", err, errn)) goto done;
    ok = 1;
done:
    free(st);
    free(o);
    if (!ok) { tagpu_hudlay_free(l); return NULL; }
    return l;
}

int tagpu_hudlay_max_hot(const HUDLAY* l) { return l->max_hot; }

void tagpu_hudlay_free(HUDLAY* l)
{
    if (!l) return;
    tagpu_json_free(l->ldoc);
    tagpu_json_free(l->kdoc);
    free(l);
}

const HUDGLOBALS* tagpu_hudlay_globals(const HUDLAY* l) { return &l->g; }
int tagpu_hudlay_nparts(const HUDLAY* l) { return l->nparts; }
const HUDPART* tagpu_hudlay_part(const HUDLAY* l, int i) { return i >= 0 && i < l->nparts ? &l->part[i] : NULL; }
