/* tagpu_wikistage.c -- the in-game wiki's stage as HUD ops. Contract: tagpu_wikistage.h; the
   specification is tools/wiki/mockup/stage.js, whose names and arithmetic this file follows line
   for line, so that its doubles are stage.js's. The DLL computes it on SSE2 for that reason
   (Makefile, THE WIKI'S STAGE).

   WHAT THE CANVAS DOES, DONE HERE AS OPS (tagpu_huddraw.c draws each):
     * fillRect is HOP_SOLID, its rect normalised as the canvas normalises a negative size;
     * strokeRect undashed is HOP_SOLID with HOPF_RING: its band runs half the width either side
       of the rect, so the ring's outer box is the rect grown by half the width and the ring is
       the width deep;
     * a stroked straight subpath is HOP_LINE, butt-ended as the canvas's default cap. Dashed, it
       is one HOP_LINE a dash, by the canvas dash rule: the pattern starts at the subpath's first
       point and runs on round its corners. A corner inside a dash is a miter join, which for the
       right angles the stage turns is the incoming stroke carried half the width past the
       corner; and a closed subpath still in a dash at its end joins its first dash round its
       first corner (Skia's dash effect, which Chrome draws with);
     * an arc filled is HOP_DOT with HOPF_RING as deep as its radius, a disc; stroked, a ring
       from half the width outside it to half the width inside;
     * fillText is HOP_TEXT in the `toast` style at the canvas's size, its colour a run's and
       its opacity the op's, its line box centred on the point (stage.js sets every line with the
       middle baseline);
     * drawImage of a cursor is HOP_PART of the kit's icon, in the box stage.js snaps it to.
   A canvas call that paints nothing -- an empty rect, a zero-length line, empty text -- is no op. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_wikistage.h"

/* ---- the copy ------------------------------------------------------------------------------- */

/* `s` into `d` (n bytes with its terminator), cut at a UTF-8 character; 1 when it was cut */
static int cut(char* d, size_t n, const char* s)
{
    size_t len = s ? strlen(s) : 0;
    if (len < n) {
        memcpy(d, s ? s : "", len + 1);
        return 0;
    }
    len = n - 1;
    while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
    memcpy(d, s, len);
    d[len] = 0;
    return 1;
}

static uint8_t owner8(int o) { return o >= 0 && o < 8 ? (uint8_t)o : 255; }

static void site8(WIKISTAGESITE* d, const WIKIVSITE* s)
{
    memset(d, 0, sizeof *d);
    d->x = (float)s->x;
    d->z = (float)s->z;
    d->type = (uint8_t)(s->type >= 0 && s->type < WIKI_MAX_DEFS ? s->type : 0);
    d->fx = (uint8_t)(s->fx > 0 && s->fx < 256 ? s->fx : 1);
    d->fz = (uint8_t)(s->fz > 0 && s->fz < 256 ? s->fz : 1);
    d->ok = (uint8_t)(s->ok != 0);
    d->state = (uint8_t)s->state;
    d->owner = owner8(s->owner);
    d->order = (uint16_t)(s->order > 0 && s->order < 65536 ? s->order : 0);
}

uint32_t tagpu_wikistage_copy(WIKISTAGEVIEW* out, const WIKIVIEW* v, const WIKIENTRY* e)
{
    uint32_t t = 0;
    int i, x, z, n;
    out->trunc = 0;
    out->w = out->h = 0;
    out->nvent = out->nunit = out->nsite = out->nqueued = out->nline = 0;
    out->has_ghost = out->has_drag = out->has_caption = 0;
    out->ntype = 0;
    if (!v || v->w <= 0 || v->h <= 0) return 0;

    out->w = (int16_t)(v->w < WIKISTAGE_MAP ? v->w : WIKISTAGE_MAP);
    out->h = (int16_t)(v->h < WIKISTAGE_MAP ? v->h : WIKISTAGE_MAP);
    if (out->w != v->w || out->h != v->h) t |= WIKISTAGE_TRUNC_MAP;
    out->surface_metal = v->surface_metal;
    for (z = 0; z < out->h; z++)
        for (x = 0; x < out->w; x++) {
            const double m = v->metal[z * v->w + x];
            const double r = m > 0 ? floor(m + 0.5) : 0;
            out->metal[z * out->w + x] = (uint8_t)(r < 255 ? r : 255);
            if (r != m || m > 255) t |= WIKISTAGE_TRUNC_METAL;
            out->fog[z * out->w + x] = v->fog[z * v->w + x];
        }

    n = v->nvent < WIKISTAGE_VENTS ? v->nvent : WIKISTAGE_VENTS;
    if (n < v->nvent) t |= WIKISTAGE_TRUNC_VENTS;
    for (i = 0; i < n; i++) { out->vent[i][0] = (float)v->vent[i].x; out->vent[i][1] = (float)v->vent[i].z; }
    out->nvent = (int16_t)n;

    n = v->nunit < WIKISTAGE_UNITS ? v->nunit : WIKISTAGE_UNITS;
    if (n < v->nunit) t |= WIKISTAGE_TRUNC_UNITS;
    for (i = 0; i < n; i++) {
        const WIKIVUNIT* s = &v->unit[i];
        WIKISTAGEUNIT* d = &out->unit[i];
        const char* name = s->label && *s->label ? s->label
                         : s->type >= 0 && s->type < e->ndef ? e->def[s->type].name : "";
        d->x = (float)s->x;
        d->z = (float)s->z;
        d->built = (float)s->built;
        d->reclaimed = (float)s->reclaimed;
        d->fx = (uint8_t)(s->fx > 0 && s->fx < 256 ? s->fx : 1);
        d->fz = (uint8_t)(s->fz > 0 && s->fz < 256 ? s->fz : 1);
        d->owner = owner8(s->owner);
        d->selected = (uint8_t)(s->selected != 0);
        if (cut(d->label, sizeof d->label, name)) t |= WIKISTAGE_TRUNC_LABEL;
    }
    out->nunit = (int16_t)n;

    out->has_ghost = (uint8_t)(v->has_ghost != 0);
    if (v->has_ghost) site8(&out->ghost, &v->ghost);
    n = v->nsite < WIKISTAGE_SITES ? v->nsite : WIKISTAGE_SITES;
    if (n < v->nsite) t |= WIKISTAGE_TRUNC_SITES;
    for (i = 0; i < n; i++) site8(&out->site[i], &v->site[i]);
    out->nsite = (int16_t)n;
    out->shape = (int8_t)v->shape;
    out->spacing = (int16_t)(v->spacing >= 0 && v->spacing < 32768 ? v->spacing : -1);
    n = v->nqueued < WIKISTAGE_QUEUED ? v->nqueued : WIKISTAGE_QUEUED;
    if (n < v->nqueued) t |= WIKISTAGE_TRUNC_QUEUED;
    for (i = 0; i < n; i++) site8(&out->queued[i], &v->queued[i]);
    out->nqueued = (int16_t)n;
    n = v->nline < WIKISTAGE_LINES ? v->nline : WIKISTAGE_LINES;
    if (n < v->nline) t |= WIKISTAGE_TRUNC_LINES;
    for (i = 0; i < n; i++) {
        WIKISTAGELINE* d = &out->line[i];
        d->from_x = (float)v->line[i].from_x;
        d->from_z = (float)v->line[i].from_z;
        d->to_x = (float)v->line[i].to_x;
        d->to_z = (float)v->line[i].to_z;
        d->kind = (uint8_t)v->line[i].kind;
    }
    out->nline = (int16_t)n;

    out->cursor_x = v->cursor_x;
    out->cursor_z = v->cursor_z;
    out->cursor_shape = (uint8_t)v->cursor_shape;
    for (i = 0; i < WIKI_NKEYS; i++) out->held[i] = (uint8_t)(v->held[i] != 0);
    out->has_drag = (uint8_t)(v->has_drag != 0);
    out->drag_x0 = (float)v->drag_x0;
    out->drag_z0 = (float)v->drag_z0;
    out->drag_x1 = (float)v->drag_x1;
    out->drag_z1 = (float)v->drag_z1;
    out->has_caption = (uint8_t)(v->caption != NULL);
    if (cut(out->caption, sizeof out->caption, v->caption)) t |= WIKISTAGE_TRUNC_CAPTION;
    out->pace = v->pace;
    out->mode = (int16_t)(v->mode >= 0 && v->mode < WIKI_MAX_DEFS ? v->mode : -1);
    n = e->ndef < WIKI_MAX_DEFS ? e->ndef : WIKI_MAX_DEFS;
    for (i = 0; i < n; i++)
        if (cut(out->type[i], sizeof out->type[i], e->def[i].name)) t |= WIKISTAGE_TRUNC_LABEL;
    out->ntype = (int16_t)n;
    out->trunc = t;
    return t;
}

/* ---- the canvas and the fit ------------------------------------------------------------------ */

void tagpu_wikistage_canvas(HUDRECT stage, double u, double* x0, double* y0, double* w, double* h)
{
    const float px = (float)u;
    *x0 = tagpu_hud_snap(stage.x * px);
    *y0 = tagpu_hud_snap(stage.y * px);
    *w = tagpu_hud_edge(stage.x, stage.w, px) - *x0;
    *h = tagpu_hud_edge(stage.y, stage.h, px) - *y0;
}

void tagpu_wikistage_fit(int w, int h, double stage_w, double stage_h, double u, WIKISTAGEFIT* out)
{
    const double cell = WIKI_CELL, mw = w > 0 ? w : 1, mh = h > 0 ? h : 1, pad = 6 * u;
    const double sx = (stage_w - 2 * pad) / (mw * cell), sz = (stage_h - 2 * pad) / (mh * cell);
    out->s = sx < sz ? sx : sz;
    out->ox = (stage_w - mw * cell * out->s) / 2;
    out->oz = (stage_h - mh * cell * out->s) / 2;
}

/* ---- the ops ------------------------------------------------------------------------------- */

typedef struct {
    const WIKISTAGEART* art;
    WIKISTAGEOPS*       o;
    double              u, X0, Y0, W, H;    /* the canvas in device pixels             */
    HUDRECT             clip;               /* the canvas in units                     */
    int                 room;               /* ops the frame may use before the edge's  */
} B;

/* a colour as the canvas parses one: each channel rounded, the alpha in 1/255ths, rounded */
static uint32_t rgba(double r, double g, double b, double a)
{
    const double R = floor(r + 0.5), G = floor(g + 0.5), Bl = floor(b + 0.5), A = floor(a * 255 + 0.5);
    return (uint32_t)R << 24 | (uint32_t)G << 16 | (uint32_t)Bl << 8 | (uint32_t)A;
}
#define HEX(c) ((uint32_t)(c) << 8 | 0xFFu)

static HUDOP* op(B* c, int kind, double x, double y, double w, double h)
{
    HUDOP* p;
    if (c->o->n >= c->room) { c->o->dropped++; return NULL; }
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

/* fillRect */
static void rect(B* c, double x, double y, double w, double h, uint32_t col)
{
    HUDOP* p;
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (w == 0 || h == 0) return;
    if ((p = op(c, HOP_SOLID, x, y, w, h))) p->rgba = col;
}

/* strokeRect, undashed: the band lw wide centred on the rect's edges */
static void ring(B* c, double x, double y, double w, double h, double lw, uint32_t col)
{
    HUDOP* p;
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if ((p = op(c, HOP_SOLID, x - lw / 2, y - lw / 2, w + lw, h + lw))) {
        p->flags = HOPF_RING;
        p->size = (float)(lw / c->u);
        p->rgba = col;
    }
}

/* an arc round (cx, cy) of radius r: filled when depth >= r, else its stroke's band */
static void circle(B* c, double cx, double cy, double r, double depth, uint32_t col)
{
    HUDOP* p;
    if (!(r > 0)) return;
    if ((p = op(c, HOP_DOT, cx - r, cy - r, 2 * r, 2 * r))) {
        p->flags = HOPF_RING;
        p->size = (float)(depth / c->u);
        p->rgba = col;
    }
}

static int line(B* c, double x0, double y0, double x1, double y1, double lw, uint32_t col)
{
    HUDOP* p;
    if (x0 == x1 && y0 == y1) return -1;
    if (!(p = op(c, HOP_LINE, x0, y0, x1 - x0, y1 - y0))) return -1;
    p->size = (float)(lw / c->u);
    p->rgba = col;
    return (int)(p - c->o->op);
}

/* a polyline's stroke (the header of this file): `np` points, dashed `on`, `off` from its first
   point, or undashed with `on` HUGE_VAL; `closed` when its last point is its first */
static void poly(B* c, const double* pt, int np, int closed, double lw, uint32_t col, double on, double off)
{
    const double pat[2] = { on, off };
    double rem = on;
    int k = 0, i, last = -1;
    for (i = 0; i + 1 < np; i++) {
        const double ax = pt[2 * i], ay = pt[2 * i + 1], bx = pt[2 * i + 2], by = pt[2 * i + 3];
        const double dx = bx - ax, dy = by - ay, L = sqrt(dx * dx + dy * dy);
        double t = 0;
        if (last >= 0) {
            /* a dash ran on to this corner: the miter fills the corner's outer square */
            HUDOP* p = &c->o->op[last];
            const double px = (double)p->r.w * c->u, py = (double)p->r.h * c->u, pl = sqrt(px * px + py * py);
            p->r.w = (float)((px + px / pl * lw / 2) / c->u);
            p->r.h = (float)((py + py / pl * lw / 2) / c->u);
            last = -1;
        }
        if (!(L > 0)) continue;
        while (t < L) {
            const double step = rem < L - t ? rem : L - t;
            int at = -1;
            if (k == 0 && step > 0)
                at = line(c, ax + dx * (t / L), ay + dy * (t / L), ax + dx * ((t + step) / L), ay + dy * ((t + step) / L), lw, col);
            t += step;
            rem -= step;
            if (rem <= 0) { k ^= 1; rem = pat[k]; }
            else if (t >= L && k == 0) last = at;
        }
        /* an open polyline's last point joins nothing; a closed one's joins its first dash */
        if (i + 2 == np && !closed) last = -1;
    }
    if (last >= 0) {
        HUDOP* p = &c->o->op[last];
        const double px = (double)p->r.w * c->u, py = (double)p->r.h * c->u, pl = sqrt(px * px + py * py);
        p->r.w = (float)((px + px / pl * lw / 2) / c->u);
        p->r.h = (float)((py + py / pl * lw / 2) / c->u);
    }
}

/* strokeRect, dashed: the closed subpath from its top-left corner, clockwise as given */
static void dashed_rect(B* c, double x, double y, double w, double h, double lw, uint32_t col, double on, double off)
{
    const double pt[10] = { x, y, x + w, y, x + w, y + h, x, y + h, x, y };
    poly(c, pt, 5, 1, lw, col, on, off);
}

/* fillText with the middle baseline: align 0 left, 1 centre, 2 right of x */
static void text(B* c, const char* s, double x, double y, double size, int align, uint32_t col)
{
    const size_t len = strlen(s);
    HUDOP* p;
    HUDRUN* r;
    if (!len) return;
    if (c->o->nrun >= WIKI_MAXRUN || (size_t)c->o->ntext + len + 1 > WIKI_TEXTMAX) { c->o->dropped++; return; }
    if (!(p = op(c, HOP_TEXT, x, y, 0, 0))) return;
    p->style = c->art->style;
    p->centre = (uint8_t)align;
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

static double measure(B* c, const char* s, double size) { return c->art->measure(c->art->measure_ctx, size, s); }

/* a number as JavaScript prints it: the shortest decimal that reads back as the same double */
static void jsnum(char* d, size_t n, double v)
{
    int p;
    for (p = 1; p <= 17; p++) {
        snprintf(d, n, "%.*g", p, v);
        if (strtod(d, NULL) == v) return;
    }
}

static const uint32_t TEAM[8] = { 0x2348e0, 0xd32b00, 0xf4f4f4, 0x2f8a1b, 0x7a3fb0, 0xe8d200, 0x2a2a2a, 0xe07018 };
static uint32_t team(int o) { return HEX(o < 8 ? TEAM[o] : 0xcccccc); }

/* stage.js `label`: a name on its box, shrunk to 1.5 times the box when longer */
static void label(B* c, const char* t, double bw, double bh, double cx, double cy, uint32_t col)
{
    const double U = c->u, max = bw * 1.5;
    double sz = fmax(6 * U, fmin(9 * U, bh * .3)), w;
    if (!*t) return;
    w = measure(c, t, sz);
    if (w > max) sz = sz * max / w;
    text(c, t, cx, cy, sz, 1, col);
}

/* stage.js `readout`: a line on a dark plate at the stage's bottom, right or left */
static void readout(B* c, const char* t, int right)
{
    const double U = c->u, sz = floor(8.5 * U + 0.5), h = 12 * U, tw = measure(c, t, sz);
    rect(c, right ? c->W - tw - 8 * U : 2 * U, c->H - h - 2 * U, tw + 6 * U, h, rgba(30, 30, 28, .7));
    text(c, t, right ? c->W - 5 * U : 5 * U, c->H - 2 * U - h / 2, sz, right ? 2 : 0, HEX(0xf0f0ea));
}

static double snapd(double v) { return floor(v + 0.5 + 1.0 / 64); }

int tagpu_wikistage_build(const WIKISTAGEVIEW* v, const char* msg, HUDRECT stage, double u,
                          const WIKISTAGEART* art, WIKISTAGEOPS* out)
{
    static const uint32_t SITE[3] = { 0x2bd248, 0xe62828, 0xf0d020 };
    static const uint32_t LINE[4] = { 0x3ccf3c, 0xf0cc10, 0xe23030, 0x3a86f0 };   /* WIKI_ORDER_* */
    B c;
    const double U = u;
    int i, x, z;
    out->n = out->nrun = out->ntext = 0;
    out->dropped = 0;
    memset(&c, 0, sizeof c);
    c.art = art;
    c.o = out;
    c.u = u;
    c.room = WIKI_MAXOP - 1;              /* the last op is the dropped frame's red edge */
    tagpu_wikistage_canvas(stage, u, &c.X0, &c.Y0, &c.W, &c.H);
    c.clip.x = (float)(c.X0 / u);
    c.clip.y = (float)(c.Y0 / u);
    c.clip.w = (float)(c.W / u);
    c.clip.h = (float)(c.H / u);

    rect(&c, 0, 0, c.W, c.H, HEX(0x6e6e68));
    if (!v || v->w <= 0 || v->h <= 0) {
        if (msg) text(&c, msg, c.W / 2, c.H / 2, floor(11 * U + 0.5), 1, HEX(0xe8e8e2));
    } else {
        const int mw = v->w, mh = v->h;
        const double cell = WIKI_CELL;
        WIKISTAGEFIT f;
        double s, ox, oz, cp, sm = v->surface_metal, top = sm;
#define X(v_) (ox + (v_) * s)
#define Z(v_) (oz + (v_) * s)
        tagpu_wikistage_fit(mw, mh, c.W, c.H, u, &f);
        s = f.s; ox = f.ox; oz = f.oz;
        cp = cell * s;

        /* the ground: plain cells light, metal darker by how far it stands above surfaceMetal */
        for (i = 0; i < mw * mh; i++)
            if (v->metal[i] > top) top = v->metal[i];
        for (z = 0; z < mh; z++)
            for (x = 0; x < mw; x++) {
                const double m = v->metal[z * mw + x];
                double L = m > 0 ? 188 : 204;
                if (m > sm && top > sm) L = 176 - 108 * (m - sm) / (top - sm);
                rect(&c, floor(X(x * cell)), floor(Z(z * cell)), ceil(cp) + 1, ceil(cp) + 1, rgba(L, L, L - 4, 1));
            }
        /* the grid: one pixel wide on pixel centres, each row broken at the columns so that no
           pixel of the translucent stroke is laid twice */
        if (cp >= 5) {
            const uint32_t g = rgba(0, 0, 0, .12);
            const double end = X(mw * cell);
            for (x = 0; x <= mw; x++) {
                const double px = floor(X(x * cell) + 0.5);
                line(&c, px + .5, Z(0), px + .5, Z(mh * cell), 1, g);
            }
            for (z = 0; z <= mh; z++) {
                const double pz = floor(Z(z * cell) + 0.5) + .5;
                double a = X(0);
                for (x = 0; x <= mw; x++) {
                    const double px = floor(X(x * cell) + 0.5);
                    if (px > a) line(&c, a, pz, px, pz, 1, g);
                    a = px + 1;
                }
                if (end > a) line(&c, a, pz, end, pz, 1, g);
            }
        }
        /* a geothermal vent: a ringed circle with a cross */
        for (i = 0; i < v->nvent; i++) {
            const double vx = X(v->vent[i][0]), vz = Z(v->vent[i][1]), r = cp * .8, lw = fmax(1, 1.5 * U);
            circle(&c, vx, vz, r, r, HEX(0x9a9a94));
            circle(&c, vx, vz, r + lw / 2, lw, HEX(0x2a2a28));
            circle(&c, vx, vz, r * .5 + lw / 2, lw, HEX(0x2a2a28));
            line(&c, vx - r, vz, vx + r, vz, lw, HEX(0x2a2a28));
            line(&c, vx, vz - r, vx, vz + r, lw, HEX(0x2a2a28));
        }
        /* fog: grey half over seen-before ground, black over unexplored */
        for (z = 0; z < mh; z++)
            for (x = 0; x < mw; x++) {
                const int fg = v->fog[z * mw + x];
                if (!fg) continue;
                rect(&c, floor(X(x * cell)), floor(Z(z * cell)), ceil(cp) + 1, ceil(cp) + 1,
                     fg == 2 ? rgba(0, 0, 0, .9) : rgba(128, 128, 128, .5));
            }

        /* order lines, dashed, coloured by kind, each ending on a dot */
        for (i = 0; i < v->nline; i++) {
            const WIKISTAGELINE* l = &v->line[i];
            const uint32_t col = HEX(l->kind < 4 ? LINE[l->kind] : 0xd8d8d8);
            const double pt[4] = { X(l->from_x), Z(l->from_z), X(l->to_x), Z(l->to_z) };
            poly(&c, pt, 2, 0, fmax(1, 1.4 * U), col, 5 * U, 3.5 * U);
            circle(&c, pt[2], pt[3], 2 * U, 2 * U, col);
        }
        /* queued build sites: a dashed square in the owner's colour */
        for (i = 0; i < v->nqueued; i++) {
            const WIKISTAGESITE* q = &v->queued[i];
            const double w = q->fx * cp, h = q->fz * cp, bx = X(q->x) - w / 2, by = Z(q->z) - h / 2;
            dashed_rect(&c, bx, by, w, h, fmax(1, 1.5 * U), team(q->owner), 3 * U, 2 * U);
            label(&c, q->type < v->ntype ? v->type[q->type] : "", w, h, X(q->x), Z(q->z), rgba(20, 20, 20, .75));
        }
        /* units: grey boxes at their footprint, a team-coloured edge; construction fills up from
           the bottom, a reclaim empties from the top */
        for (i = 0; i < v->nunit; i++) {
            const WIKISTAGEUNIT* un = &v->unit[i];
            const double w = un->fx * cp, h = un->fz * cp, bx = X(un->x) - w / 2, by = Z(un->z) - h / 2;
            const double built = fmin(fmax(un->built, 0), 1), rec = fmin(fmax(un->reclaimed, 0), 1);
            const double bw = fmax(1.5, 2 * U);
            if (built < 1) {
                rect(&c, bx, by, w, h, rgba(150, 150, 144, .35));
                rect(&c, bx, by + h * (1 - built), w, h * built, HEX(0x8a8a84));
            } else if (rec > 0) {
                rect(&c, bx, by, w, h, rgba(150, 150, 144, .35));
                rect(&c, bx, by + h * rec, w, h * (1 - rec), HEX(0x9a7c78));
            } else
                rect(&c, bx, by, w, h, HEX(0x8a8a84));
            ring(&c, bx + bw / 2, by + bw / 2, w - bw, h - bw, bw, team(un->owner));
            label(&c, un->label, w, h, X(un->x), Z(un->z), HEX(0x141414));
            if (un->selected) {
                /* the selection box: corner brackets just outside the footprint */
                const double m = 3 * U, x0 = bx - m, y0 = by - m, x1 = bx + w + m, y1 = by + h + m, k = fmin(w, h) * .35 + m;
                const double cn[4][4] = { { x0, y0, 1, 1 }, { x1, y0, -1, 1 }, { x0, y1, 1, -1 }, { x1, y1, -1, -1 } };
                int j;
                for (j = 0; j < 4; j++) {
                    const double pt[6] = { cn[j][0] + cn[j][2] * k, cn[j][1], cn[j][0], cn[j][1], cn[j][0], cn[j][1] + cn[j][3] * k };
                    poly(&c, pt, 3, 0, fmax(1, 1.5 * U), HEX(0xf2f6e8), HUGE_VAL, 0);
                }
            }
        }
        /* a placement gesture's sites: a translucent ghost, TA's build square in the site's
           colour, and its place among the orders the release adds */
        for (i = 0; i < v->nsite; i++) {
            const WIKISTAGESITE* st = &v->site[i];
            const double w = st->fx * cp, h = st->fz * cp, bx = X(st->x) - w / 2, by = Z(st->z) - h / 2, lw = fmax(1, 1.5 * U);
            rect(&c, bx + lw, by + lw, w - 2 * lw, h - 2 * lw, rgba(138, 138, 132, .45));
            ring(&c, bx + lw / 2, by + lw / 2, w - lw, h - lw, lw, HEX(st->state < 3 ? SITE[st->state] : SITE[0]));
            if (st->order) {
                const double sz = fmax(5 * U, fmin(7.5 * U, h * .32));
                char n[8];
                snprintf(n, sizeof n, "%u", (unsigned)st->order);
                text(&c, n, bx + 2.5 * lw, by + 2 * lw + sz / 2, sz, 0, HEX(0x101010));
            }
        }
        /* the placement ghost: translucent green where it may go, red where it may not */
        if (v->has_ghost) {
            const WIKISTAGESITE* gh = &v->ghost;
            const double w = gh->fx * cp, h = gh->fz * cp, bx = X(gh->x) - w / 2, by = Z(gh->z) - h / 2;
            rect(&c, bx, by, w, h, gh->ok ? rgba(40, 210, 70, .35) : rgba(230, 40, 40, .35));
            ring(&c, bx, by, w, h, fmax(1, 1.5 * U), HEX(gh->ok ? 0x2bd248 : 0xe62828));
            label(&c, gh->type < v->ntype ? v->type[gh->type] : "", w, h, X(gh->x), Z(gh->z),
                  gh->ok ? rgba(12, 58, 18, 1) : rgba(74, 10, 10, 1));
        }
        if (v->has_drag) {
            const double x0 = X(fmin(v->drag_x0, v->drag_x1)), y0 = Z(fmin(v->drag_z0, v->drag_z1));
            ring(&c, x0, y0, fabs((double)v->drag_x1 - v->drag_x0) * s, fabs((double)v->drag_z1 - v->drag_z0) * s, fmax(1, U), HEX(0xf2f6e8));
        }
        /* the cursor at screen size: its kit icon centred on the point, in the box stage.js snaps */
        if (v->cursor_shape < 3 && art->cursor[v->cursor_shape] >= 0) {
            const int k = v->cursor_shape;
            const double w = art->cursor_w[k] * U, h = art->cursor_h[k] * U;
            const double a = X(v->cursor_x) - w / 2, b = Z(v->cursor_z) - h / 2;
            const double x0 = snapd(a), y0 = snapd(b);
            HUDOP* p = op(&c, HOP_PART, x0, y0, snapd(a + w) - x0, snapd(b + h) - y0);
            if (p) p->part = art->cursor[k];
        }
        if (v->pace > 1) {
            char t[64], n[32];
            jsnum(n, sizeof n, v->pace);
            snprintf(t, sizeof t, "construction \xC3\x97%s", n);
            readout(&c, t, 1);
        }
        if (v->spacing > 0) {
            char t[32];
            snprintf(t, sizeof t, "Spacing: %d", v->spacing);
            readout(&c, t, 0);
        }
#undef X
#undef Z
    }

    if (out->dropped) {
        HUDOP* p;
        c.room = WIKI_MAXOP;
        if ((p = op(&c, HOP_SOLID, 0, 0, c.W, c.H))) {
            p->flags = HOPF_RING;
            p->size = 2;
            p->rgba = HEX(0xff2020);
        }
        if (!out->logged && art->log) {
            char line[96];
            snprintf(line, sizeof line, "wiki stage: %d ops past the budget of %d were dropped", out->dropped, WIKI_MAXOP);
            art->log(art->log_ctx, line);
            out->logged = 1;
        }
    }
    return out->n;
}
