/* tagpu_wikitechdraw.c -- the tech tree drawn as HUD ops: techtree.js `draw` and wiki.js drawTech's stage.
   Contract: tagpu_wikitech.h. Apart from tagpu_wikitech.c, whose model and layout the asset test compiles
   alone, because this half draws on the stage's canvas (tagpu_wikistage.h).

   PURE, as tagpu_wikitech.c: no engine address, no global, only its arguments. tools/wiki/techcheck.mjs
   pairs it call for op with techtree.js over every setup's tree. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_wikitech.h"

/* ---- THE DRAWING (techtree.js draw), each canvas call an op as tools/wiki/stagecheck.mjs's
   recorder reads it: fillRect a HOP_SOLID, an undashed strokeRect a HOPF_RING solid round its
   band, a stroked polyline its pieces as HOP_LINEs carried half the width into each corner,
   fillText on the alphabetic baseline a HOPF_BASE text in the role's style and its run's colour,
   drawImage a HOP_PIC. globalAlpha is each op's alpha. ---- */

typedef struct {
    WIKISTAGEOPS* o;
    double        u, X0, Y0, W, H;      /* the canvas in device pixels                         */
    HUDRECT       clip;                 /* the canvas in units                                 */
    double        ga;                   /* the canvas's globalAlpha                            */
} TB;

/* the tree's inks (techtree.js INK), as 0xRRGGBB */
enum { I_BG = 0x2a2c25, I_HEAD = 0x33352d, I_RIM = 0x56594c, I_TEXT = 0xe2e2da, I_MUTED = 0x9a9d8e, I_EDGE = 0x5b6050,
       I_ALSO = 0x6a7058, I_PATH = 0x7fc4e8, I_KID = 0x9fd68a, I_SEL = 0xe8c66a, I_UP = 0xc98ae0, I_PIC = 0x000000,
       I_STAGE = 0x16180f, I_SAY = 0xb7b7ae };   /* the stage's fill and its message's (wiki.js drawTech) */

static uint32_t ink(TB* c, uint32_t rgb) { return rgb << 8 | (uint32_t)floor(c->ga * 255 + 0.5); }

static HUDOP* top(TB* c, int kind, double x, double y, double w, double h)
{
    HUDOP* p;
    if (c->o->n >= WIKI_MAXOP) { c->o->dropped++; return NULL; }
    c->o->range[c->o->n] = WIKISTAGE_IN_R0;
    c->o->rtype[c->o->n] = 0;
    p = &c->o->op[c->o->n++];
    memset(p, 0, sizeof *p);
    p->kind = (uint8_t)kind;
    p->part = -1;
    p->r.x = (float)((c->X0 + x) / c->u);
    p->r.y = (float)((c->Y0 + y) / c->u);
    p->r.w = (float)(w / c->u);
    p->r.h = (float)(h / c->u);
    p->clip = c->clip;
    p->rgba = 0xFFFFFFFFu;
    return p;
}

static void t_rect(TB* c, double x, double y, double w, double h, uint32_t col)
{
    HUDOP* p;
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (w == 0 || h == 0) return;
    if ((p = top(c, HOP_SOLID, x, y, w, h))) p->rgba = col;
}

/* strokeRect, undashed: the band lw wide centred on the rect's edges */
static void t_ring(TB* c, double x, double y, double w, double h, double lw, uint32_t col)
{
    HUDOP* p;
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if ((p = top(c, HOP_SOLID, x - lw / 2, y - lw / 2, w + lw, h + lw))) {
        p->flags = HOPF_RING;
        p->size = (float)(lw / c->u);
        p->rgba = col;
    }
}

static int t_line(TB* c, double x0, double y0, double x1, double y1, double lw, uint32_t col)
{
    HUDOP* p;
    if (x0 == x1 && y0 == y1) return -1;
    if (!(p = top(c, HOP_LINE, x0, y0, x1 - x0, y1 - y0))) return -1;
    p->size = (float)(lw / c->u);
    p->rgba = col;
    return (int)(p - c->o->op);
}

/* a dash run on to a corner: the miter fills the corner's outer square (tagpu_wikistage.c poly) */
static void t_carry(TB* c, int at, double lw)
{
    HUDOP* p = &c->o->op[at];
    const double px = (double)p->r.w * c->u, py = (double)p->r.h * c->u, pl = sqrt(px * px + py * py);
    p->r.w = (float)((px + px / pl * lw / 2) / c->u);
    p->r.h = (float)((py + py / pl * lw / 2) / c->u);
}

/* a polyline's stroke by the canvas dash rule, as tagpu_wikistage.c poly: `on` HUGE_VAL undashed */
#define T_DASH_EPS 1e-9
static void t_poly(TB* c, const double* pt, int np, int closed, double lw, uint32_t col, double on, double off)
{
    const double pat[2] = { on, off };
    double rem = on;
    int k = 0, i, last = -1;
    for (i = 0; i + 1 < np; i++) {
        const double ax = pt[2 * i], ay = pt[2 * i + 1], bx = pt[2 * i + 2], by = pt[2 * i + 3];
        const double dx = bx - ax, dy = by - ay, L = sqrt(dx * dx + dy * dy);
        double t = 0;
        if (last >= 0) { t_carry(c, last, lw); last = -1; }
        if (!(L > 0)) continue;
        while (t < L) {
            const double step = rem < L - t ? rem : L - t;
            int at = -1;
            if (k == 0 && step > 0)
                at = t_line(c, ax + dx * (t / L), ay + dy * (t / L), ax + dx * ((t + step) / L), ay + dy * ((t + step) / L), lw, col);
            t += step;
            rem -= step;
            if (rem <= T_DASH_EPS) {
                k ^= 1;
                rem = pat[k];
                if (L - t <= T_DASH_EPS) t = L;
            } else if (t >= L - T_DASH_EPS && k == 0) {
                last = at;
                t = L;
            }
        }
        if (i + 2 == np && !closed) last = -1;
    }
    if (last >= 0) t_carry(c, last, lw);
}

/* fillText on the alphabetic baseline, from x, in role `role`'s style at `size` device pixels */
static void t_text(TB* c, const WTART* art, int role, const char* s, double x, double y, double size, uint32_t col)
{
    const size_t len = strlen(s);
    HUDOP* p;
    HUDRUN* r;
    if (!len) return;
    if (c->o->nrun >= WIKI_MAXRUN || (size_t)c->o->ntext + len + 1 > WIKI_TEXTMAX) { c->o->dropped++; return; }
    if (!(p = top(c, HOP_TEXT, x, y, 0, 0))) return;
    p->flags = HOPF_BASE | HOPF_INK;
    p->style = art->style[role];
    p->size = (float)(size / c->u);
    p->rgba = 0xFFFFFF00u | (col & 0xFF);
    p->run = (uint16_t)c->o->nrun;
    p->nrun = 1;
    r = &c->o->run[c->o->nrun++];
    r->rgb = col >> 8;
    r->resource = 0;
    r->off = (uint16_t)c->o->ntext;
    r->len = (uint16_t)len;
    memcpy(c->o->text + c->o->ntext, s, len + 1);
    c->o->ntext += (int)len + 1;
}

/* code-page bytes as UTF-8, each byte the code point of its value (the mockup's text, which gfx.py
   reads as Latin-1); `cp1252` takes a title's rule instead: Windows-1252, '?' for a byte with no
   character there (gfx.py display_name) */
static void t_utf8(char* d, size_t n, const unsigned char* s, int cp1252)
{
    static const uint16_t HI[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161,
        0x203A, 0x0153, 0, 0x017E, 0x0178 };
    size_t o = 0;
    for (; *s && o + 4 < n; s++) {
        unsigned v = *s;
        if (cp1252 && v >= 0x80 && v < 0xA0) v = HI[v - 0x80] ? HI[v - 0x80] : '?';
        if (v < 0x80) d[o++] = (char)v;
        else if (v < 0x800) { d[o++] = (char)(0xC0 | v >> 6); d[o++] = (char)(0x80 | (v & 0x3F)); }
        else { d[o++] = (char)(0xE0 | v >> 12); d[o++] = (char)(0x80 | (v >> 6 & 0x3F)); d[o++] = (char)(0x80 | (v & 0x3F)); }
    }
    d[o] = 0;
}

/* the last UTF-8 character of `s` taken off */
static void t_drop(char* s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] & 0xC0) == 0x80) n--;
    if (n) n--;
    s[n] = 0;
}

static const char* const T_KIND[WT_KINDS] = { "COMMANDER", "FACTORY", "BUILDER", "ECONOMY", "DEFENSE", "SUPPORT", "LAND",
                                              "AIR", "SEA", "UPGRADE" };

/* the canvas of the stage `stage` at `u`, appending to `o` */
static void t_begin(TB* c, HUDRECT stage, double u, WIKISTAGEOPS* o)
{
    memset(c, 0, sizeof *c);
    c->o = o;
    c->u = u;
    c->ga = 1;
    tagpu_wikistage_canvas(stage, u, &c->X0, &c->Y0, &c->W, &c->H);
    c->clip.x = (float)(c->X0 / u); c->clip.y = (float)(c->Y0 / u); c->clip.w = (float)(c->W / u); c->clip.h = (float)(c->H / u);
}

static void t_empty(WIKISTAGEOPS* o)
{
    o->n = o->nrun = o->ntext = o->dropped = o->ngrp = 0;
    o->cursor = -1;
}

static void draw_tree(TB* cp, const WIKITECHTREE* T, const WTCAM* cam, int sel, int all, const WTART* art);

void tagpu_wikitech_draw(const WIKITECHTREE* T, const WTCAM* cam, int sel, int all, const WTART* art, HUDRECT stage,
                         double u, WIKISTAGEOPS* o)
{
    TB c;
    t_empty(o);
    t_begin(&c, stage, u, o);
    draw_tree(&c, T, cam, sel, all, art);
}

void tagpu_wikitech_stage(const WIKITECHTREE* T, const char* msg, const WTCAM* cam, int sel, int all, const WTART* art,
                          HUDRECT stage, double u, WIKISTAGEOPS* o)
{
    TB c;
    t_empty(o);
    t_begin(&c, stage, u, o);
    t_rect(&c, 0, 0, c.W, c.H, ink(&c, I_STAGE));
    if (T) {
        draw_tree(&c, T, cam, sel, all, art);
        return;
    }
    if (msg && msg[0]) {
        HUDOP* p;
        t_text(&c, art, WT_ROLE_BODY, msg, c.W / 2, c.H / 2, 9 * u, ink(&c, I_SAY));
        if (c.o->n && (p = &c.o->op[c.o->n - 1])->kind == HOP_TEXT) p->centre = 1;
    }
}

static void draw_tree(TB* cp, const WIKITECHTREE* T, const WTCAM* cam, int sel, int all, const WTART* art)
{
    TB c = *cp;
    const double z = cam->z;
    uint8_t *path = NULL, *kids = NULL, *pars = NULL;
    int32_t (*ln)[3] = NULL;
    uint8_t* rank = NULL;
    double* lw_of = NULL;
    int h = -1, nl = 0, i, k;
    uint32_t q;
#define TX(v) floor(((v) - cam->x) * z + 0.5)
#define TY(v) floor(((v) - cam->y) * z + 0.5)
    if (!(path = calloc((size_t)T->n, 1)) || !(kids = calloc((size_t)T->n, 1)) || !(pars = calloc((size_t)T->n, 1)) ||
        !(ln = malloc((size_t)(T->nspine + T->n * 4 + 1) * sizeof *ln)) || !(rank = malloc((size_t)(T->nspine + T->n * 4 + 1))) ||
        !(lw_of = malloc((size_t)(T->nspine + T->n * 4 + 1) * sizeof *lw_of)))
        goto done;
    /* the selection: its card (techtree.js home), the walk to that card, its card's builds, its own
       parents */
    if (sel >= 0 && sel < T->n) {
        int steps = 0;
        for (h = sel; h >= 0 && T->card[h] < 0 && steps++ < T->n; h = T->first[h]) {}
        for (k = h, steps = 0; k >= 0 && steps++ < T->n; k = T->first[k]) path[k] = 1;
        if (h >= 0) for (q = 0; q < T->builds[h].n; q++) kids[T->pool[T->builds[h].at + q]] = 1;
        for (q = 0; q < T->parents[sel].n; q++) pars[T->pool[T->parents[sel].at + q]] = 1;
    }
    /* THE EDGES first, under the cards: each card to its parent card, then (lit, or with `all`) every
       other builder of a card's unit; drawn by rank, a stable order */
    for (i = 0; i < T->nspine; i++) {
        const int kk = T->spine[i], spk = T->sp[kk];
        if (spk >= 0) {
            const int on = path[kk] && path[spk], kid = spk == h || (kk == h && pars[spk]);
            ln[nl][0] = spk; ln[nl][1] = kk;
            ln[nl][2] = on ? I_PATH : kid ? I_KID : I_EDGE;
            lw_of[nl] = on ? 1.5 : 1;
            rank[nl++] = (uint8_t)(on || kid ? 2 : h >= 0 ? 0 : 1);
        }
        for (q = 0; q < T->parents[kk].n; q++) {
            const int p = T->pool[T->parents[kk].at + q];
            int kid;
            if (p == spk || T->card[p] < 0) continue;
            kid = p == h || (kk == h && pars[p]);
            if (!(kid || all)) continue;
            ln[nl][0] = p; ln[nl][1] = kk;
            ln[nl][2] = kid ? I_KID : I_ALSO;
            lw_of[nl] = 1;
            rank[nl++] = (uint8_t)(kid ? 2 : 0);
        }
    }
    for (k = 0; k <= 2; k++)
        for (i = 0; i < nl; i++) {
            const double* A = T->pos[T->card[ln[i][0]]];
            const double* Bp = T->pos[T->card[ln[i][1]]];
            const double mx = (A[0] + A[2] + Bp[0]) / 2;
            double lw, off, pt[8];
            if (rank[i] != k) continue;
            c.ga = h >= 0 && !rank[i] ? 0.35 : 1;
            lw = floor(lw_of[i] * z + 0.5);
            if (lw < 1) lw = 1;
            off = fmod(lw, 2) ? 0.5 : 0;
            pt[0] = TX(A[0] + A[2]);  pt[1] = TY(A[1] + WT_EDGE_Y) + off;
            pt[2] = TX(mx) + off;     pt[3] = TY(A[1] + WT_EDGE_Y) + off;
            pt[4] = TX(mx) + off;     pt[5] = TY(Bp[1] + WT_EDGE_Y) + off;
            pt[6] = TX(Bp[0]);        pt[7] = TY(Bp[1] + WT_EDGE_Y) + off;
            t_poly(&c, pt, 4, 0, lw, ink(&c, (uint32_t)ln[i][2]), HUGE_VAL, 0);
        }
    c.ga = 1;
    /* THE CARDS */
    for (i = 0; i < T->nspine; i++) {
        const int kk = T->spine[i];
        const double* P = T->pos[i];
        const double x = P[0], y = P[1], w = P[2], hh = P[3];
        const WIKITECHUNIT* un = &T->t->unit[T->unit[kk]];
        const int lit = kk == h || path[kk] || kids[kk] || pars[kk];
        const WTLIST ic = T->icons[kk];
        const double cx = TX(x), cy = TY(y), cw = TX(x + w) - cx, ch = TY(y + hh) - cy;
        double lw, tx, tw;
        uint32_t rim;
        char s[600], t[600];
        if (TX(x + w) < 0 || TY(y + hh) < 0 || TX(x) > c.W || TY(y) > c.H) continue;
        c.ga = h >= 0 && !lit ? 0.3 : 1;
        t_rect(&c, cx, cy, cw, ch, ink(&c, I_BG));
        t_rect(&c, cx, cy, cw, TY(y + WT_HEAD) - cy, ink(&c, I_HEAD));
        rim = kk == h ? I_SEL : path[kk] ? I_PATH : kids[kk] || pars[kk] ? I_KID : I_RIM;
        lw = floor((kk == h ? 1.5 : 0.75) * z + 0.5);
        if (lw < 1) lw = 1;
        t_ring(&c, cx + lw / 2, cy + lw / 2, cw - lw, ch - lw, lw, ink(&c, rim));
        /* the picture: its black square, the picture over it */
#define PIC_AT(unit_idx, px_, py_, side_) do { \
            const double bx_ = TX(px_), by_ = TY(py_), bw_ = TX((px_) + (side_)) - bx_, bh_ = TY((py_) + (side_)) - by_; \
            const WIKITECHUNIT* pu_ = &T->t->unit[T->unit[unit_idx]]; \
            HUDOP* pp_; \
            t_rect(&c, bx_, by_, bw_, bh_, ink(&c, I_PIC)); \
            if (pu_->pic >= 0 && pu_->pic < T->t->npic && (pp_ = top(&c, HOP_PIC, bx_, by_, bw_, bh_))) { \
                pp_->part = (int16_t)pu_->pic; \
                pp_->centre = (uint8_t)(bw_ < T->t->pic[pu_->pic].w); \
                pp_->rgba = 0xFFFFFF00u | (uint32_t)floor(c.ga * 255 + 0.5); \
            } \
        } while (0)
        PIC_AT(kk, x + WT_PAD, y + WT_PAD, WT_PIC);
        tx = x + WT_PAD * 2 + WT_PIC;
        tw = w - (tx - x) - WT_PAD;
        /* each text one line, clipped with an ellipsis, left out under READ device pixels */
#define CLIP(str_, ux_, uy_, uw_, role_, px_, col_) do { \
            const double sz_ = (px_) * z; \
            if (sz_ >= 6) { \
                snprintf(t, sizeof t, "%s", (str_)); \
                if (art->measure(art->measure_ctx, (role_), sz_, t) > (uw_) * z) { \
                    char e_[sizeof t + 4]; \
                    for (;;) { \
                        snprintf(e_, sizeof e_, "%s\xE2\x80\xA6", t); \
                        if (!t[0] || !(art->measure(art->measure_ctx, (role_), sz_, e_) > (uw_) * z)) break; \
                        t_drop(t); \
                    } \
                    if (strlen(t) + 4 <= sizeof t) memcpy(t + strlen(t), "\xE2\x80\xA6", 4); \
                } \
                t_text(&c, art, (role_), t, TX(ux_), TY(uy_), sz_, ink(&c, (col_))); \
            } \
        } while (0)
        if (un->title[0]) t_utf8(s, sizeof s, un->title, 1); else snprintf(s, sizeof s, "%s", un->name);
        CLIP(s, tx, y + 10, tw, WT_ROLE_TITLE, 8, I_TEXT);
        t_utf8(s, sizeof s, un->desc, 0);
        CLIP(s, tx, y + 18, tw, WT_ROLE_BODY, 5.5, I_MUTED);
        if (T->upgrades[kk].n) snprintf(s, sizeof s, "%s \xC2\xB7 T%d \xC2\xB7 %u UP", T_KIND[T->kind[kk]], T->tier[kk], T->upgrades[kk].n);
        else snprintf(s, sizeof s, "%s \xC2\xB7 T%d", T_KIND[T->kind[kk]], T->tier[kk]);
        CLIP(s, tx, y + 26, tw, WT_ROLE_LABEL, 5, T->upgradable[kk] ? I_UP : I_MUTED);
        for (q = 0; q < ic.n; q++) {
            const int x2 = T->pool[ic.at + q];
            const double ix = x + WT_PAD + (q % WT_PER) * (WT_ICON + WT_IGAP), iy = y + WT_HEAD + WT_IGAP + (q / WT_PER) * (WT_ICON + WT_IGAP);
            const int mark = x2 == sel ? I_SEL : T->upgrade[x2] || T->upgrades[x2].n ? I_UP : -1;
            PIC_AT(x2, ix, iy, WT_ICON);
            if (mark >= 0) {
                const double mx = TX(ix), my = TY(iy), mw = TX(ix + WT_ICON) - mx, mh = TY(iy + WT_ICON) - my;
                double ml = floor((x2 == sel ? 1.2 : 0.7) * z + 0.5);
                if (ml < 1) ml = 1;
                if (T->upgrade[x2] && x2 != sel) {
                    double on = floor(2 * z + 0.5), off = floor(1.5 * z + 0.5);
                    const double rx = mx + ml / 2, ry = my + ml / 2, rw = mw - ml, rh = mh - ml;
                    const double pt[10] = { rx, ry, rx + rw, ry, rx + rw, ry + rh, rx, ry + rh, rx, ry };
                    if (on < 2) on = 2;
                    if (off < 1) off = 1;
                    t_poly(&c, pt, 5, 1, ml, ink(&c, (uint32_t)mark), on, off);
                } else t_ring(&c, mx + ml / 2, my + ml / 2, mw - ml, mh - ml, ml, ink(&c, (uint32_t)mark));
            }
        }
        {
            const uint32_t nu = T->unlocks[kk].n, more = T->builds[kk].n - nu, nup = T->upgrades[kk].n;
            int n = snprintf(s, sizeof s, "%u new", nu);
            if (more) n += snprintf(s + n, sizeof s - (size_t)n, " \xC2\xB7 %u also built elsewhere", more);
            if (nup) snprintf(s + n, sizeof s - (size_t)n, " \xC2\xB7 %u upgrade%s", nup, nup > 1 ? "s" : "");
            CLIP(s, x + WT_PAD, y + hh - 2.5, w - 2 * WT_PAD, WT_ROLE_LABEL, 5, I_MUTED);
        }
#undef PIC_AT
#undef CLIP
    }
done:
#undef TX
#undef TY
    free(path); free(kids); free(pars); free(ln); free(rank); free(lw_of);
}
