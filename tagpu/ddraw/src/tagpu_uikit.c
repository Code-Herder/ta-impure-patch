/* tagpu_uikit.c -- the widgets of Impure's own windows. Contract: tagpu_uikit.h.

   THE ARITHMETIC IS CHROME'S, because each window is a browser mockup in C and its check pairs the
   two. Lengths are worked in CSS pixels, which in the game are the device's, because that is the
   grid Chrome rounds on: every length of the CSS -- calc(n * --u), a percentage, a line height --
   truncated to a sixty-fourth of a pixel (LayoutUnit's float constructor), the space a flex
   container centres an item in halved by integer division of sixty-fourths, a text's intrinsic
   width rounded up to one, a scrollbar's thickness to a whole pixel, a face's ascent and descent to
   whole pixels and a line's half-leading floored to one. Rounded otherwise, the rows of a list
   drift by a sixty-fourth a row. The ops are converted to units at the end.

   THE LINE RULE is the browser's for these texts: greedy, breaking after a run of spaces and
   after a hyphen between a letter and anything but a digit (UAX #14's HY class, which "right-click"
   and "Shift-drag" in today's entries meet), a line's trailing spaces hanging; a word wider than
   its box overflows it. Each line is placed by tagpu_hudlay_baseline's rule with the line's own
   height: its baseline the line's top, the face's rounded ascent and the half-leading floored. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_uikit.h"
#include "tagpu_hudtext.h"

/* the colours the CSS gives outright */
#define INK         0x1D1D1Bu               /* text on a light face (uikit.js paintParts --ink) */
#define THUMB_RGB   0x808078u               /* ::-webkit-scrollbar-thumb, at THUMB_A              */
#define THUMB_A     0.7
#define OFF_A       0.45                    /* a disabled button                                  */
#define ICON_F      0.6                     /* a kit glyph over its part (uikit.js drawIcon)      */

#if UI_MAXGLYPH > TAGPU_HUDTEXT_MAX
#error a line of one text must fit the glyphs the HUD draw sets of one op
#endif

void ui_begin(UI* u, HUDOPS* o, const UIFACES* f, const struct HUDSTYLE* style, const unsigned char* light,
              double px, int maxhot)
{
    memset(u, 0, offsetof(UI, cp));
    u->o = o; u->f = f; u->style = style; u->light = light;
    u->px = px;
    u->maxhot = maxhot;
}

/* ---- Chrome's rounding ------------------------------------------------------------------------- */

double ui_lu(double v)
{
    double r = (double)(float)v * 64.0;
    return (r < 0 ? ceil(r) : floor(r)) / 64.0;
}
double ui_half(double v)
{
    double r = floor(v * 64.0 + 0.5);
    return (r < 0 ? ceil(r / 2) : floor(r / 2)) / 64.0;
}
double ui_ceil64(double v) { return ceil(v * 64.0 - 1e-6) / 64.0; }
double ui_len(const UI* u, double n) { return ui_lu(n * u->px); }

UIR ui_rect(double x, double y, double w, double h) { UIR r; r.x = x; r.y = y; r.w = w; r.h = h; return r; }
static UIR inter(UIR a, UIR b)
{
    double x0 = fmax(a.x, b.x), y0 = fmax(a.y, b.y), x1 = fmin(a.x + a.w, b.x + b.w), y1 = fmin(a.y + a.h, b.y + b.h);
    return ui_rect(x0, y0, fmax(0, x1 - x0), fmax(0, y1 - y0));
}
static int meets(UIR a, UIR clip) { return a.y < clip.y + clip.h && a.y + a.h > clip.y && a.w > 0 && clip.h > 0; }

/* ---- the ops ----------------------------------------------------------------------------------- */

HUDOP* ui_op(UI* u, int kind, UIR r)
{
    HUDOP* p;
    if (u->o->n >= HUD_MAXOP) { u->o->overflow = 1; return NULL; }
    p = &u->o->op[u->o->n++];
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind;
    p->flags = (uint8_t)(u->top ? HOPF_TOP : 0u);
    p->grp = u->grp;
    p->part = -1;
    p->r.x = (float)(r.x / u->px); p->r.y = (float)(r.y / u->px);
    p->r.w = (float)(r.w / u->px); p->r.h = (float)(r.h / u->px);
    p->clip.w = -1;
    p->rgba = 0xFFFFFFFFu;
    return p;
}
void ui_clip(const UI* u, HUDOP* p, UIR k)
{
    if (!p) return;
    p->clip.x = (float)(k.x / u->px); p->clip.y = (float)(k.y / u->px);
    p->clip.w = (float)(k.w / u->px); p->clip.h = (float)(k.h / u->px);
}
int ui_text_put(UI* u, const char* s, size_t n)
{
    int at = u->o->ntext;
    if ((size_t)at + n + 1 > HUD_TEXTMAX) { u->o->overflow = 1; return -1; }
    memcpy(u->o->text + at, s, n);
    u->o->text[at + n] = 0;
    u->o->ntext += (int)n + 1;
    return at;
}
HUDOP* ui_art(UI* u, int part, UIR r, const UIR* clip)
{
    HUDOP* p;
    if (part < 0 || !(p = ui_op(u, HOP_PART, r))) return NULL;
    p->part = (int16_t)part;
    p->rgba = 0xFFFFFFFFu;
    if (clip) ui_clip(u, p, *clip);
    return p;
}
void ui_glyph(UI* u, int icon, UIR b)
{
    if (icon >= 0) ui_art(u, icon, ui_rect(b.x + b.w * (1 - ICON_F) / 2, b.y + b.h * (1 - ICON_F) / 2, b.w * ICON_F, b.h * ICON_F), NULL);
}
HUDOP* ui_solid(UI* u, int kind, UIR r, uint32_t rgb, double alpha, const UIR* clip)
{
    HUDOP* p = ui_op(u, kind, r);
    if (p) p->rgba = (rgb << 8) | (uint32_t)lround(255.0 * alpha);
    if (clip) ui_clip(u, p, *clip);
    return p;
}
void ui_disc(UI* u, UIR b, const UIR* clip)
{
    ui_solid(u, HOP_DOT, b, UI_LOCK_EDGE, 1, clip);
    ui_solid(u, HOP_DOT, ui_rect(b.x + 1, b.y + 1, b.w - 2, b.h - 2), UI_LOCK_RGB, 1, clip);
}
void ui_hot(UI* u, UIR b, const UIR* clip, const char* key)
{
    HUDOP* p;
    int at;
    if (clip) b = inter(b, *clip);
    if (b.w <= 0 || b.h <= 0) return;
    if (u->nhot >= u->maxhot) { u->o->overflow = 1; return; }
    if (!(p = ui_op(u, HOP_HOT, b))) return;
    if ((at = ui_text_put(u, key, strlen(key))) < 0) { u->o->n--; return; }
    p->run = (uint16_t)at;
    u->nhot++;
}
void ui_group_begin(UI* u, double alpha)
{
    if (u->o->ngrp >= HUD_MAXGRP) { u->o->overflow = 1; return; }
    u->o->grp[u->o->ngrp++] = (float)alpha;
    u->grp = (uint8_t)u->o->ngrp;
}
void ui_group_end(UI* u) { u->grp = 0; }

/* ---- text -------------------------------------------------------------------------------------- */

UITS ui_ts(const UI* u, int style, double line)
{
    const struct HUDSTYLE* s = &u->style[style];
    UITS t;
    t.style = style;
    t.fs = (double)(float)(s->size * u->px);
    t.set = u->f->size ? u->f->size(u->f->ctx, t.style, t.fs) : t.fs;
    /* the line height is the multiple of the font size rounded to a sixty-fourth (MEASURED
       2026-10-25: at 1024 x 768 a 10.133 px name's rows are 876/64 px, which 1.35 x 10.133
       truncated would make 875/64) */
    t.L = ui_lu(line * (floor(t.fs * 64 + 0.5) / 64));
    t.upper = s->upper;
    t.sp = (double)(float)(s->spacing * t.fs);
    return t;
}

int ui_pens(UI* u, const UITS* t, const char* s, size_t len)
{
    const UIFACE* fc = &u->f->face[t->style];
    const unsigned char *p = (const unsigned char*)s, *e = p + len;
    float sc = fc->info ? stbtt_ScaleForMappingEmToPixels(fc->info, (float)t->set) : 0, w = 0;
    int n = 0, prev = 0;
    while (p < e && n < UI_MAXGLYPH) {
        uint32_t ch;
        int gi;
        u->off[n] = (int)(p - (const unsigned char*)s);
        ch = tagpu_hudtext_utf8(&p, e);
        u->cp[n] = ch;
        if (t->upper) ch = tagpu_hudtext_upper(ch);
        gi = fc->info ? stbtt_FindGlyphIndex(fc->info, (int)ch) : 0;
        if (!gi && u->f->missing) {
            u->pen[n] = w;
            w += (float)(u->f->missing(u->f->ctx, t->style, t->set / u->px, ch) * u->px) + (float)t->sp;
            prev = 0;
        } else if (fc->info) {
            int adv, lsb;
            stbtt_GetGlyphHMetrics(fc->info, gi, &adv, &lsb);
            if (prev) w += sc * (float)stbtt_GetGlyphKernAdvance(fc->info, prev, gi);
            u->pen[n] = w;
            w += sc * (float)adv + (float)t->sp;
            prev = gi;
        } else
            u->pen[n] = w;
        n++;
    }
    u->off[n] = (int)(p - (const unsigned char*)s);
    u->cp[n] = 0;
    u->pen[n] = w;
    return n;
}

/* where a line may begin: after a run of spaces, and after a hyphen that follows a letter and
   comes before anything but a digit */
static int may_break(const UI* u, int i)
{
    if (i <= 0) return 0;
    if (u->cp[i] == ' ') return 0;
    if (u->cp[i - 1] == ' ') return 1;
    return u->cp[i - 1] == '-' && i >= 2 && u->cp[i - 2] != ' ' && !(u->cp[i] >= '0' && u->cp[i] <= '9');
}
static int trim(const UI* u, int a, int b)
{
    while (b > a && u->cp[b - 1] == ' ') b--;
    return b;
}
void ui_intrinsic(const UI* u, int n, double* maxc, double* minc)
{
    int a = 0, i;
    *minc = 0;
    for (i = 1; i <= n; i++)
        if (i == n || may_break(u, i)) {
            int b = trim(u, a, i);
            *minc = fmax(*minc, u->pen[b] - u->pen[a]);
            a = i;
        }
    *maxc = ui_ceil64(u->pen[trim(u, 0, n)]);
    *minc = ui_ceil64(*minc);
}
int ui_wrap(UI* u, int n, double w)
{
    int nl = 0, a = 0;
    while (a < n && u->cp[a] == ' ') a++;
    if (a >= n) { u->ls[0] = u->le[0] = a; return 1; }
    while (a < n && nl < UI_MAXLINE) {
        int b = -1, i;
        for (i = a + 1; i <= n; i++) {
            if (i < n && !may_break(u, i)) continue;
            if (b >= 0 && u->pen[trim(u, a, i)] - u->pen[a] > w) break;
            b = i;
            if (u->pen[trim(u, a, i)] - u->pen[a] > w) break;
        }
        u->ls[nl] = a;
        u->le[nl] = trim(u, a, b);
        nl++;
        a = b;
        while (a < n && u->cp[a] == ' ') a++;
    }
    return nl;
}

void ui_line(UI* u, const UITS* t, const char* s, int a, int b, double x, double top, int ink, const UIR* clip)
{
    const UIFACE* fc = &u->f->face[t->style];
    const float fs = (float)t->fs;
    const float A = floorf(fc->ascent * fs + 0.5f), D = floorf(fc->descent * fs + 0.5f);
    double base = top + A + floor((t->L - A - D) / 2);
    double w = u->pen[b] - u->pen[a];
    HUDOP* p;
    HUDRUN* r;
    int off;
    if (b <= a) return;
    if (clip && !meets(ui_rect(x, base - A, w, A + D), *clip)) return;  /* its glyphs' box, as the browser clips */
    if (u->o->nrun >= HUD_MAXRUN) { u->o->overflow = 1; return; }
    if (!(p = ui_op(u, HOP_TEXT, ui_rect(x, base - A - floor((fs - A - D) / 2), w, fs)))) return;
    if ((off = ui_text_put(u, s + u->off[a], (size_t)(u->off[b] - u->off[a]))) < 0) { u->o->n--; return; }
    p->style = (int16_t)t->style;
    p->size = (float)(t->fs / u->px);
    if (ink) p->flags |= HOPF_INK;
    r = &u->o->run[u->o->nrun++];
    r->rgb = ink ? INK : 0;
    r->resource = 0;
    r->off = (uint16_t)off;
    r->len = (uint16_t)(u->off[b] - u->off[a]);
    p->run = (uint16_t)(u->o->nrun - 1);
    p->nrun = 1;
    if (clip) ui_clip(u, p, *clip);
    else p->clip.w = -1;
}

double ui_block(UI* u, int style, double lh, const char* s, double x, double y, double w, int ink, const UIR* clip, int emit)
{
    UITS t = ui_ts(u, style, lh);
    int n = ui_pens(u, &t, s, strlen(s)), nl, i;
    if (!n) return 0;
    nl = ui_wrap(u, n, w);
    for (i = 0; emit && i < nl; i++) ui_line(u, &t, s, u->ls[i], u->le[i], x, y + i * t.L, ink, clip);
    return nl * t.L;
}
int ui_light(const UI* u, int part) { return part >= 0 && part < UI_MAXPART && u->light[part]; }

/* ---- the panels -------------------------------------------------------------------------------- */

static void panel_open(const UI* u, const UIPANELG* g, UIPANEL* p, UIR box)
{
    p->box = box;
    p->scr = ui_rect(box.x + ui_len(u, g->pn[3]), box.y + ui_len(u, g->pn[0]), box.w - ui_len(u, g->pn[3]) - ui_len(u, g->pn[1]),
                     box.h - ui_len(u, g->pn[0]) - ui_len(u, g->pn[2]));
    p->sb = 0;
    p->scroll = 0;
}
/* the content's width with or without the scrollbar; a custom scrollbar is as thick as its CSS
   width truncated to a whole pixel */
static double panel_width(const UI* u, const UIPANELG* g, const UIPANEL* p, int bar)
{
    return p->scr.w - 2 * ui_len(u, g->pad[1]) - (bar ? floor(ui_len(u, g->scrollbar)) : 0);
}
/* The content measured `content` tall at the width panel_width(bar): the scrollbar shows when it
   overflows, and the scroll is held to the panel's limit. Chrome's limit is the scroll height
   less the client height, each snapped to a whole pixel. */
static void panel_place(const UI* u, const UIPANELG* g, UIPANEL* p, double content, int bar, float want, UISCROLL* out)
{
    double pt = ui_len(u, g->pad[0]), mx;
    p->sb = bar ? floor(ui_len(u, g->scrollbar)) : 0;
    p->extent = pt + content + pt;
    mx = fmax(0, floor(p->extent + 0.5) - floor(p->scr.h + 0.5));
    /* the offset to a sixty-fourth of a pixel: a whole-pixel scroll given in float units comes back
       whole, so the thumb's truncated place does not drop a pixel */
    p->scroll = fmin(fmax(0, floor((double)want * u->px * 64 + 0.5) / 64), mx);
    p->view = ui_rect(p->scr.x, p->scr.y, p->scr.w - p->sb, p->scr.h);
    p->cx = p->scr.x + ui_len(u, g->pad[1]);
    p->cy = p->scr.y + pt - p->scroll;
    p->cw = panel_width(u, g, p, bar);
    out->scroll = (float)(p->scroll / u->px);
    out->scroll_max = (float)(mx / u->px);
}
/* The scrollbar's thumb, by Chrome's ScrollbarTheme: its length the visible share of the track,
   rounded, and its place the scroll's share of the track's free length, truncated -- a scroll
   below one pixel shown as one. The track is the scrollbar's whole length (no buttons). */
static void panel_thumb(UI* u, const UIPANEL* p, UISCROLL* out)
{
    double track = floor(p->scr.h + 0.5), total = floor(p->extent + 0.5), vis = track, len, pos;
    if (!p->sb || total <= vis) return;
    len = fmin(track, floor(vis / total * track + 0.5));
    pos = p->scroll * (track - len) / (total - vis);
    pos = pos > 0 && pos < 1 ? 1 : floor(pos);
    ui_solid(u, HOP_SOLID, ui_rect(p->scr.x + p->scr.w - p->sb, floor(p->scr.y + 0.5) + pos, p->sb, len), THUMB_RGB, THUMB_A, NULL);
    out->track.x = (float)((p->scr.x + p->scr.w - p->sb) / u->px);
    out->track.y = (float)(floor(p->scr.y + 0.5) / u->px);
    out->track.w = (float)(p->sb / u->px);
    out->track.h = (float)(track / u->px);
    out->thumb_y = (float)((floor(p->scr.y + 0.5) + pos) / u->px);
    out->thumb_h = (float)(len / u->px);
}
void ui_panel(UI* u, const UIPANELG* g, int part, UIR box, float want, UILIST list, void* user, const char* key, UISCROLL* out)
{
    UIPANEL p;
    double h;
    int bar = 0;
    memset(out, 0, sizeof *out);
    panel_open(u, g, &p, box);
    p.cx = p.cy = 0;
    p.view = p.scr;
    h = list(u, &p, panel_width(u, g, &p, 0), 0, user);
    if (2 * ui_len(u, g->pad[0]) + h > p.scr.h) { bar = 1; h = list(u, &p, panel_width(u, g, &p, 1), 0, user); }
    panel_place(u, g, &p, h, bar, want, out);
    ui_art(u, part, box, NULL);
    ui_hot(u, p.view, NULL, key);
    list(u, &p, p.cw, 1, user);
    panel_thumb(u, &p, out);
}

/* ---- the rows ---------------------------------------------------------------------------------- */

double ui_row(UI* u, const UIROWG* g, const UIROW* rw, double x, double y, double w, const UIR* clip, int emit)
{
    const double py = ui_len(u, g->pad[0]), pxx = ui_len(u, g->pad[1]), gap = ui_len(u, g->gap);
    const double lock = rw->locked ? ui_len(u, g->lock) : 0, inner_w = w - 2 * pxx;
    UITS t = ui_ts(u, rw->style, g->line), tm;
    double fixed = (rw->mark_w > 0 ? rw->mark_w + gap : 0) + (rw->locked ? gap + lock : 0), maxc = 0, minc = 0, sw, h, ih, mh = 0, th;
    int n = ui_pens(u, &t, rw->text, strlen(rw->text)), nl = 0, i, ink = rw->part >= 0 ? ui_light(u, rw->part) : rw->ink;
    UIR b, ty;
    if (rw->mark_icon >= 0) mh = rw->mark_w;
    else if (rw->mark[0] || rw->mark_w > 0) { tm = ui_ts(u, rw->mark_style, g->line); mh = rw->mark[0] ? tm.L : 0; }
    ui_intrinsic(u, n, &maxc, &minc);
    sw = maxc <= inner_w - fixed ? maxc : fmax(inner_w - fixed, minc);
    if (n) nl = ui_wrap(u, n, sw);
    th = nl * t.L;
    h = fmax(ui_len(u, g->h), 2 * py + fmax(fmax(th, mh), lock));
    ih = h - 2 * py;
    b = ui_rect(x, y, w, h);
    if (emit && (!clip || meets(b, *clip))) {
        double mx = x + pxx, tx = mx + (rw->mark_w > 0 ? rw->mark_w + gap : 0);
        ui_art(u, rw->part, b, clip);
        if (rw->key) ui_hot(u, b, clip, rw->key);
        if (rw->mark_icon >= 0) {
            /* the mark's box is as tall as it is wide, centred in the row's height */
            UIR mb = ui_rect(mx, y + py + ui_half(ih - mh), rw->mark_w, mh);
            if (!clip || meets(mb, *clip)) ui_art(u, rw->mark_icon, mb, clip);
        } else if (rw->mark[0]) {
            int m = ui_pens(u, &tm, rw->mark, strlen(rw->mark));
            double mw = u->pen[m];
            double lx = rw->mark_right ? mx + rw->mark_w - ui_ceil64(mw) : mx + ui_half(rw->mark_w - ui_ceil64(mw));
            ui_line(u, &tm, rw->mark, 0, m, lx, y + py + ui_half(ih - tm.L), ink, clip);
        }
        if (n) {
            ui_pens(u, &t, rw->text, strlen(rw->text));
            ty = ui_rect(tx, y + py + ui_half(ih - th), sw, th);
            for (i = 0; i < nl; i++) ui_line(u, &t, rw->text, u->ls[i], u->le[i], ty.x, ty.y + i * t.L, ink, clip);
        }
        if (rw->locked)
            ui_disc(u, ui_rect(fmax(tx + sw + gap, x + w - pxx - lock), y + py + ui_half(ih - lock), lock, lock), clip);
    }
    return h + ui_len(u, g->after);
}

/* ---- buttons and badges ------------------------------------------------------------------------ */

double ui_button_w(UI* u, const UIBUTTONG* g, const char* label)
{
    UITS t = ui_ts(u, g->style, g->line);
    int n = ui_pens(u, &t, label, strlen(label));
    return fmax(ui_len(u, g->min), 2 * ui_len(u, g->pad) + 4 + ui_ceil64(u->pen[n]));
}

/* Chrome's default style gives a button a 2 px border, which the kit skin makes transparent and
   keeps, so its text stands inside the box. */
double ui_button(UI* u, const UIBUTTONG* g, double x, double y, const char* key, const char* label, int icon, int part, int off)
{
    UITS t = ui_ts(u, g->style, g->line);
    const double w = ui_button_w(u, g, label), h = ui_len(u, g->h);
    int n = ui_pens(u, &t, label, strlen(label));
    double tw = ui_ceil64(u->pen[n]);
    UIR b = ui_rect(x, y, w, h);
    if (off) ui_group_begin(u, OFF_A);
    ui_art(u, part, b, NULL);
    if (!off) ui_hot(u, b, NULL, key);
    ui_glyph(u, icon, b);
    ui_line(u, &t, label, 0, n, x + 2 + ui_len(u, g->pad) + ui_half(w - 4 - 2 * ui_len(u, g->pad) - tw), y + 2 + ui_half(h - 4 - t.L),
            ui_light(u, part), NULL);
    if (off) ui_group_end(u);
    return w;
}

double ui_badge_w(UI* u, const UIBADGEG* g, const char* text)
{
    UITS t = ui_ts(u, g->style, g->line);
    int n = ui_pens(u, &t, text, strlen(text));
    return ui_len(u, g->pad) + ui_len(u, g->dot) + ui_len(u, g->gap) + ui_ceil64(u->pen[n]) + ui_len(u, g->pad);
}
double ui_badge(UI* u, const UIBADGEG* g, int part, double x, double y, const char* text)
{
    UITS t = ui_ts(u, g->style, g->line);
    const double dot = ui_len(u, g->dot), pad = ui_len(u, g->pad), bh = ui_len(u, g->h);
    const UIR b = ui_rect(x, y, ui_badge_w(u, g, text), bh);
    int n = ui_pens(u, &t, text, strlen(text));
    ui_art(u, part, b, NULL);
    ui_disc(u, ui_rect(b.x + pad, y + ui_half(bh - dot), dot, dot), NULL);
    ui_line(u, &t, text, 0, n, b.x + pad + dot + ui_len(u, g->gap), y + ui_half(bh - t.L), ui_light(u, part), NULL);
    return b.w;
}

/* ---- parts and layouts ------------------------------------------------------------------------- */

int ui_find_part(const HUDLAY* k, const char* name)
{
    int i;
    for (i = 0; i < tagpu_hudlay_nparts(k); i++)
        if (!strcmp(tagpu_hudlay_part(k, i)->name, name)) return i;
    return -1;
}

int ui_face_light(const unsigned char* rgba, int w, int h, const float slice[4], int premultiplied)
{
    double t = slice ? slice[0] : h / 4.0, r = slice ? slice[1] : w / 4.0, b = slice ? slice[2] : h / 4.0, le = slice ? slice[3] : w / 4.0;
    double mw = fmax(1, w - le - r), mh = fmax(1, h - t - b), s = 0;
    int i, j, n = 0;
    if (!rgba || w < 1 || h < 1) return 0;
    for (j = 0; j < 8; j++)
        for (i = 0; i < 8; i++) {
            int x0 = (int)floor(le + mw * i / 8), x1 = (int)ceil(le + mw * (i + 1) / 8);
            int y0 = (int)floor(t + mh * j / 8), y1 = (int)ceil(t + mh * (j + 1) / 8), x, y, k = 0;
            double cr = 0, cg = 0, cb = 0, ca = 0;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            /* the canvas averages premultiplied colour and reads the cell back straight */
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++) {
                    const unsigned char* p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
                    const double a = p[3], f = premultiplied ? 1 : a / 255;
                    cr += p[0] * f; cg += p[1] * f; cb += p[2] * f; ca += a;
                    k++;
                }
            if (!k || ca / k <= 128) continue;
            s += (0.3 * cr + 0.59 * cg + 0.11 * cb) / ca * 255;
            n++;
        }
    return n > 0 && s / n > 120;
}

double ui_num(UILAYOUT* x, const char* a, const char* b, int i)
{
    const TJV* v = tj_get(x->L, a);
    if (b) v = tj_get(v, b);
    if (i >= 0) v = tj_at(v, i);
    if (!tj_is(v, TJ_NUM)) {
        if (!x->bad++ && x->errn) snprintf(x->err, x->errn, "%s: %s%s%s%s is missing or not a number", x->what, a, b ? "." : "", b ? b : "", i >= 0 ? "[i]" : "");
        return 0;
    }
    return v->num;
}
