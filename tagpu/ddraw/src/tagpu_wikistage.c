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
     * drawImage of a game cursor's frame is HOP_PART with HOPF_SPRITE, its cell of the sprite
       sheet in the frame's own box; of a kit icon, HOP_PART of the icon in the box stage.js snaps
       it to; of the sidebar's composed column, HOP_SIDE in the box stage.js snaps it to;
     * stage.js's `group` -- calls drawn at full opacity, then faded once as a whole -- is the
       HUD's group: the ops it makes carry one `grp`, whose opacity is out->grp.
   A canvas call that paints nothing -- an empty rect, a zero-length line, empty text -- is no op. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_cobvm.h"
#include "tagpu_wikistage.h"
#include "tagpu_wikiside.h"

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

/* Math.trunc held to int16_t: a world point on the stage's map lies in 0..2048 */
static int16_t whole16(double v)
{
    const double t = v < 0 ? ceil(v) : floor(v);
    return (int16_t)(t > 32767 ? 32767 : t < -32768 ? -32768 : t);
}

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

uint32_t tagpu_wikistage_copy(WIKISTAGEVIEW* out, const WIKIVIEW* v, const WIKIENTRY* e, int layout_src)
{
    uint32_t t = 0;
    int i, x, z, n;
    out->trunc = 0;
    out->w = out->h = 0;
    out->nvent = out->nunit = out->nsite = out->nqueued = out->nline = 0;
    out->has_ghost = out->has_drag = out->has_caption = 0;
    out->ntype = 0;
    out->npose = 0;
    out->has_side = 0;
    out->layout_src = WIKISTAGE_LAYOUT_NONE;
    memset(&out->side, 0, sizeof out->side);
    out->side.index = -1;
    out->locked = -1;
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
        d->hx = (float)s->hx;
        d->hz = (float)s->hz;
        d->type = (uint8_t)(s->type >= 0 && s->type < WIKI_MAX_DEFS ? s->type : 0);
        d->built = (float)s->built;
        d->reclaimed = (float)s->reclaimed;
        d->fx = (uint8_t)(s->fx > 0 && s->fx < 256 ? s->fx : 1);
        d->fz = (uint8_t)(s->fz > 0 && s->fz < 256 ? s->fz : 1);
        d->owner = owner8(s->owner);
        d->selected = (uint8_t)(s->selected != 0);
        d->tx = whole16(s->x);
        d->tz = whole16(s->z);
        d->pose_at = d->pose_n = 0;
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
    out->cursor_since = v->cursor_since;
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
    /* the sidebar: its states to their array, the unit under the cursor found by id among the
       units copied (sim.js's ids are unique; the first wins, as side.js's find), -1 when it is
       none of them */
    if (v->has_side) {
        const WIKIVSIDE* sd = &v->side;
        out->has_side = 1;
        out->layout_src = (uint8_t)(layout_src > 0 && layout_src < WIKISTAGE_LAYOUT_N ? layout_src : WIKISTAGE_LAYOUT_NONE);
        out->side.h = (int16_t)(sd->h > 0 && sd->h <= WIKI_SIDE_ROWS ? sd->h : 0);
        out->side.index = (int8_t)(sd->index >= 0 && sd->index < WIKI_SIDE_SIDES ? sd->index : -1);
        out->side.screen = (int16_t)(sd->screen >= 0 && sd->screen < WIKI_SIDE_SCREENS ? sd->screen : -1);
        n = sd->nstate > 0 ? sd->nstate < WIKI_SIDE_GADGETS ? sd->nstate : WIKI_SIDE_GADGETS : 0;
        for (i = 0; i < n; i++) out->side.state[i] = (int8_t)(sd->state[i] >= -1 && sd->state[i] <= 2 ? sd->state[i] : -1);
        out->side.nstate = (int16_t)n;
        out->side.under = -1;
        for (i = 0; sd->under[0] && i < out->nunit; i++)
            if (v->unit[i].id && !strcmp(v->unit[i].id, sd->under)) {
                out->side.under = (int16_t)i;
                break;
            }
    }
    /* A LOCK REACHES THE PAGE WITH THE SIDEBAR OFF TOO: a scene unit or a build of a cast the
       casting leaves unbound stops a demo that has no sidebar, and the page's badge says so */
    out->locked = (int16_t)(v->locked >= 0 && v->locked < WIKI_MAX_DEFS ? v->locked : -1);
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

void tagpu_wikistage_fit(int w, int h, int side_h, double stage_w, double stage_h, double u, WIKISTAGEFIT* out)
{
    const double cell = WIKI_CELL, mw = (w > 0 ? w : 1) * cell, mh = (h > 0 ? h : 1) * cell, pad = 6 * u;
    double sx, sz;
    /* stage.js `content`: the column left of the map, as tall as the taller of the two */
    if (side_h > 0) {
        out->x0 = -WIKI_SIDE_W;
        out->cw = WIKI_SIDE_W + mw;
        out->ch = mh > side_h ? mh : side_h;
    } else {
        out->x0 = 0;
        out->cw = mw;
        out->ch = mh;
    }
    sx = (stage_w - 2 * pad) / out->cw;
    sz = (stage_h - 2 * pad) / out->ch;
    out->s = sx < sz ? sx : sz;
    out->ox = (stage_w - out->cw * out->s) / 2 - out->x0 * out->s;
    out->oz = (stage_h - out->ch * out->s) / 2;
}

static double snapd(double v) { return floor(v + 0.5 + 1.0 / 64); }

/* The engine's countdown run over `elapsed` at once: taking the ticks off in several steps, as the
   UI's updates do, lands on the same frame, since each step runs its loop until the countdown is
   above zero again. A looping sequence is back where it began after its whole length, so only the
   remainder is run; a length of 0 would never end the engine's own loop and is drawn as frame 0. */
int tagpu_wikistage_cursor_frame(const WIKISTAGESPR* frames, int n, int loop, long elapsed)
{
    long total = 0, cd;
    int f = 0, i;
    if (n <= 0) return -1;
    if (n == 1 || elapsed <= 0) return 0;
    for (i = 0; i < n; i++) total += frames[i].dur;
    if (total <= 0) return 0;
    if (loop) elapsed %= total;
    cd = (long)frames[0].dur - elapsed;
    while (cd <= 0) {
        if (++f >= n) {
            if (!loop) return -1;
            f = 0;
        }
        cd += frames[f].dur;
    }
    return f;
}

void tagpu_wikistage_image(int w, int h, int side_h, HUDRECT stage, double u, int* x0, int* y0, int* x1, int* y1)
{
    double cx, cy, cw, ch;
    WIKISTAGEFIT f;
    tagpu_wikistage_canvas(stage, u, &cx, &cy, &cw, &ch);
    tagpu_wikistage_fit(w, h, side_h, cw, ch, u, &f);
    /* the canvas's corner is a whole device pixel (tagpu_wikistage_canvas), so snapping each edge
       from the canvas, as stage.js does, snaps it from the screen too */
    *x0 = (int)(cx + snapd(f.ox));
    *y0 = (int)(cy + snapd(f.oz));
    *x1 = (int)(cx + snapd(f.ox + (w > 0 ? w : 1) * WIKI_CELL * f.s));
    *y1 = (int)(cy + snapd(f.oz + (h > 0 ? h : 1) * WIKI_CELL * f.s));
}

int tagpu_wikistage_keep(const WIKISTAGEOPS* o, int i, int image, int ground, const uint32_t* resolved)
{
    int t;
    if (i < 0 || i >= o->n || i >= WIKI_MAXOP) return 0;
    if (!image) return 1;
    switch (o->range[i]) {
    case WIKISTAGE_IN_R0:     return 1;
    case WIKISTAGE_IN_SIDE:   return 1;
    case WIKISTAGE_IN_GROUND: return ground ? 0 : 1;
    case WIKISTAGE_IN_BOX:
        t = o->rtype[i];
        return t < WIKI_MAX_DEFS && resolved && (resolved[t >> 5] >> (t & 31) & 1u) ? 0 : 2;
    case WIKISTAGE_IN_LABEL:  return 0;
    default:               return 2;
    }
}

/* ---- the ops ------------------------------------------------------------------------------- */

typedef struct {
    const WIKISTAGEART* art;
    WIKISTAGEOPS*       o;
    double              u, X0, Y0, W, H;    /* the canvas in device pixels             */
    HUDRECT             clip;               /* the canvas in units                     */
    int                 room;               /* ops the frame may use before the edge's  */
    uint8_t             range, rtype;       /* the range every op made now belongs to: stage.js's
                                               `mark` before the calls that follow it      */
    uint8_t             grp;                /* the group every op made now is drawn in, 0 none */
} B;

/* every group the build makes is one the HUD pass can draw, which holds HUD_MAXGRP of them */
typedef char wikistage_groups_fit[WIKI_MAXGRP <= HUD_MAXGRP ? 1 : -1];

static void mark(B* c, int range, int type)
{
    c->range = (uint8_t)range;
    c->rtype = (uint8_t)(type >= 0 && type < WIKI_MAX_DEFS ? type : 0);
}

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
    c->o->range[c->o->n] = c->range;
    c->o->rtype[c->o->n] = c->rtype;
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
    p->grp = c->grp;
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

/* stage.js `label`: a name on its box, shrunk to `max` when longer */
static void label_to(B* c, const char* t, double bh, double cx, double cy, double max, uint32_t col)
{
    const double U = c->u;
    double sz = fmax(6 * U, fmin(9 * U, bh * .3)), w;
    if (!*t) return;
    w = measure(c, t, sz);
    if (w > max) sz = sz * max / w;
    text(c, t, cx, cy, sz, 1, col);
}

/* ... and by default to 1.5 times its box */
static void label(B* c, const char* t, double bw, double bh, double cx, double cy, uint32_t col)
{
    label_to(c, t, bh, cx, cy, bw * 1.5, col);
}

/* stage.js `grounds`: the shade of map cell i -- plain cells light, metal darker by how far it
   stands above surfaceMetal -- for the map's ground and the whitebox minimap's alike */
static uint32_t ground(const WIKISTAGEVIEW* v, int i, double top)
{
    const double m = v->metal[i], sm = v->surface_metal;
    double L = m > 0 ? 188 : 204;
    if (m > sm && top > sm) L = 176 - 108 * (m - sm) / (top - sm);
    return rgba(L, L, L - 4, 1);
}

/* ---- the sidebar ------------------------------------------------------------------------------
   stage.js `drawSide`: the column x in [-128, 0) of the stage at one game pixel a world unit,
   whatever the HUD scale, side.h rows tall. In the game look one image, the column the frame
   composed in the mod's pictures (tagpu_wikiside.h); otherwise the whitebox: the column, the
   minimap -- its well, the ground's shades and the fog one solid a map cell at the minimap's
   scale, a dot a unit in its owner's colour, the camera box -- then the routed screen's panel and
   each drawn gadget as a button: a solid, an edge one game pixel deep and its name. A pressed
   button (or the current tab) takes the pressed colour and its name moves one game pixel right
   and down, as the engine moves a pressed button's text (0x4A5F40); a greyed one is drawn whole
   at opacity 0.45, as one group. stage.js's SIDE_WB, whose colours these are. */
#define SIDE_COLUMN  0x2e2e2a
#define SIDE_PANEL   0x3a3a36
#define SIDE_WELL    0x1e1e1c
#define SIDE_BUTTON  0x55554f
#define SIDE_EDGE    0x8a8a80
#define SIDE_PRESSED 0x6e6e66
#define SIDE_GREY    0.45f
#define SIDE_LABEL   0xe8e8e2
#define SIDE_BOX     0xf0cc10

/* stage.js drawSide's `box`: a rect of the column in game pixels, x from its left edge */
typedef struct { double x, y, w, h, cx, cy; } SBOX;
static SBOX sbox(double ox, double oz, double s, double x, double y, double w, double h)
{
    SBOX b;
    b.x = ox + (x - WIKI_SIDE_W) * s;
    b.y = oz + y * s;
    b.w = w * s;
    b.h = h * s;
    b.cx = ox + ((x + w / 2) - WIKI_SIDE_W) * s;
    b.cy = oz + (y + h / 2) * s;
    return b;
}

/* drawSide's `ring`: an edge one game pixel deep inside the box */
static void sring(B* c, const SBOX* b, double s, uint32_t col)
{
    ring(c, b->x + s / 2, b->y + s / 2, b->w - s, b->h - s, s, col);
}

/* a whitebox button's name: its unit, or its gadget name without the prefix of the side the view
   shows (stage.js: L.sides[sv.index]), none for a side the layout does not have (ARMORDERS ->
   ORDERS). The layout is checked (tagpu_wikilayout_check), so every name in it is terminated
   inside its array; `side` is the record's, data, bounded here against the layout's sides. */
static const char* named(const WIKILAYOUT* L, const WIKIGADGET* g, int side)
{
    const char* prefix = side >= 0 && side < L->nside && side < WIKI_SIDE_SIDES ? L->side[side].prefix : "";
    const size_t n = strlen(prefix);
    if (g->role == WIKI_ROLE_BUILD && g->unit[0]) return g->unit;
    if (n && strlen(g->name) > n && !strncmp(g->name, prefix, n)) return g->name + n;
    return g->name;
}

static void side(B* c, const WIKISTAGEVIEW* v, const WIKISTAGEART* art, double ox, double oz, double s, double top)
{
    const double H = v->side.h;
    const WIKILAYOUT* L = art->side_layout;
    /* the mod's pictures belong to their own layout's gadgets (wiki.js sideArt) */
    const struct WIKISIDE* im = art->side && v->layout_src == WIKISTAGE_LAYOUT_MOD && L && L == &art->side->layout
                              ? art->side : NULL;
    const WIKISCREEN* scr;
    int i, x, z;
    if (im) {
        const double x0 = snapd(ox + -WIKI_SIDE_W * s), y0 = snapd(oz + 0 * s);
        HUDOP* p = op(c, HOP_SIDE, x0, y0, snapd(ox + 0 * s) - x0, snapd(oz + H * s) - y0);
        if (p) {
            p->size = WIKI_SIDE_W;
            p->shade = (float)H;
        }
    } else {
        const SBOX b = sbox(ox, oz, s, 0, 0, WIKI_SIDE_W, H);
        rect(c, b.x, b.y, b.w, b.h, HEX(SIDE_COLUMN));
    }

    if (!im || !art->side_minimap) {
        /* the minimap's picture where the engine puts it (0x466780) over the stage map's extent, as
           the game look has it */
        const int mw = v->w, mh = v->h, W = mw * WIKI_CELL, Hm = mh * WIKI_CELL;
        WIKISIDEMAP m;
        SBOX b = sbox(ox, oz, s, 0, 0, WIKI_SIDE_MINIMAP, WIKI_SIDE_MINIMAP);
        tagpu_wikiside_geometry(W, Hm, &m);
        rect(c, b.x, b.y, b.w, b.h, HEX(SIDE_WELL));
        /* one solid a map cell, its edges on the device pixels the picture's own fractions round to */
#define CELLAT(cx_, cz_, col_)                                                                     \
        do {                                                                                      \
            const double a_ = snapd(ox + ((m.offx + (double)(cx_) * m.w / mw) - WIKI_SIDE_W) * s);  \
            const double b_ = snapd(ox + ((m.offx + (double)((cx_) + 1) * m.w / mw) - WIKI_SIDE_W) * s); \
            const double c_ = snapd(oz + (m.offy + (double)(cz_) * m.h / mh) * s);                  \
            const double d_ = snapd(oz + (m.offy + (double)((cz_) + 1) * m.h / mh) * s);            \
            if (b_ > a_ && d_ > c_) rect(c, a_, c_, b_ - a_, d_ - c_, (col_));                      \
        } while (0)
        for (z = 0; z < mh; z++)
            for (x = 0; x < mw; x++) CELLAT(x, z, ground(v, z * mw + x, top));
        for (z = 0; z < mh; z++)
            for (x = 0; x < mw; x++) {
                const int fg = v->fog[z * mw + x];
                if (fg) CELLAT(x, z, fg == 2 ? rgba(0, 0, 0, .9) : rgba(128, 128, 128, .5));
            }
#undef CELLAT
        /* a dot 3 game pixels across on the unit's minimap pixel (0x466DC0's point), in its
           owner's colour */
        for (i = 0; i < v->nunit; i++) {
            const WIKISTAGEUNIT* u = &v->unit[i];
            int px, py;
            tagpu_wikiside_dot(u->tx, u->tz, &m, W, Hm, &px, &py);
            circle(c, ox + ((m.offx + px + .5) - WIKI_SIDE_W) * s, oz + (m.offy + py + .5) * s, 1.5 * s, 1.5 * s,
                   team(u->owner));
        }
        /* the camera box: the stage shows the whole map, so it is the picture's border */
        b = sbox(ox, oz, s, m.offx, m.offy, m.w, m.h);
        sring(c, &b, s, HEX(SIDE_BOX));
    }

    /* THE SCREEN IS DATA UNTIL BOUNDED: an index of the layout, which tagpu_wikilayout_check
       has bounded whole, and the states read only as far as both they and its gadgets go */
    if (!L || v->side.screen < 0 || v->side.screen >= L->nscreen || v->side.screen >= WIKI_SIDE_SCREENS) return;
    scr = &L->screen[v->side.screen];
    if (!im) {
        const SBOX b = sbox(ox, oz, s, 0, scr->y, scr->w, scr->h);
        rect(c, b.x, b.y, b.w, b.h, HEX(SIDE_PANEL));
    }
    for (i = 0; i < scr->ngadget && i < v->side.nstate && i < WIKI_SIDE_GADGETS; i++) {
        const WIKIGADGET* g = &scr->gadget[i];
        const int st = v->side.state[i];
        const char* lt = "";
        SBOX b;
        /* wiki.js sideArt `label`: over the image, only a gadget whose picture is a label, by its
           label's text ("" names it) */
        if (im) {
            const WIKISIDEPIC* p = &im->screen[v->side.screen].pic[i];
            if (p->from != WIKISIDE_FROM_LABEL && p->nframe > 0) continue;
            /* a text not terminated inside its array is no text */
            lt = memchr(p->text, 0, sizeof p->text) ? p->text : "";
        }
        if (!(st >= 0)) continue;
        b = sbox(ox, oz, s, g->x, scr->y + g->y, g->w, g->h);
        if (st == 2) {
            if (c->o->ngrp >= WIKI_MAXGRP) {
                c->o->dropped++;
                continue;
            }
            c->o->grp[c->o->ngrp++] = SIDE_GREY;
            c->grp = (uint8_t)c->o->ngrp;
        }
        rect(c, b.x, b.y, b.w, b.h, HEX(st == 1 ? SIDE_PRESSED : SIDE_BUTTON));
        sring(c, &b, s, HEX(SIDE_EDGE));
        label_to(c, *lt ? lt : named(L, g, v->side.index), b.h, st == 1 ? b.cx + s : b.cx, st == 1 ? b.cy + s : b.cy,
                 fmax(s, b.w - 4 * s), HEX(SIDE_LABEL));
        c->grp = 0;
    }
}

/* stage.js `readout`: a line on a dark plate at the stage's bottom, right or left */
static void readout(B* c, const char* t, int right)
{
    const double U = c->u, sz = floor(8.5 * U + 0.5), h = 12 * U, tw = measure(c, t, sz);
    rect(c, right ? c->W - tw - 8 * U : 2 * U, c->H - h - 2 * U, tw + 6 * U, h, rgba(30, 30, 28, .7));
    text(c, t, right ? c->W - 5 * U : 5 * U, c->H - 2 * U - h / 2, sz, right ? 2 : 0, HEX(0xf0f0ea));
}

int tagpu_wikistage_build(const WIKISTAGEVIEW* v, const char* msg, HUDRECT stage, double u, int cursor,
                          const WIKISTAGEART* art, WIKISTAGEOPS* out)
{
    static const uint32_t SITE[3] = { 0x2bd248, 0xe62828, 0xf0d020 };
    static const uint32_t LINE[4] = { 0x3ccf3c, 0xf0cc10, 0xe23030, 0x3a86f0 };   /* WIKI_ORDER_* */
    B c;
    const double U = u;
    int i, x, z;
    out->n = out->nrun = out->ntext = 0;
    out->dropped = 0;
    out->cursor = -1;
    out->ngrp = 0;
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

    mark(&c, WIKISTAGE_IN_R0, 0);
    rect(&c, 0, 0, c.W, c.H, HEX(0x6e6e68));
    if (!v || v->w <= 0 || v->h <= 0) {
        if (msg) text(&c, msg, c.W / 2, c.H / 2, floor(11 * U + 0.5), 1, HEX(0xe8e8e2));
    } else {
        const int mw = v->w, mh = v->h;
        const double cell = WIKI_CELL;
        WIKISTAGEFIT f;
        double s, ox, oz, cp, top = v->surface_metal;
#define X(v_) (ox + (v_) * s)
#define Z(v_) (oz + (v_) * s)
        tagpu_wikistage_fit(mw, mh, v->has_side ? v->side.h : 0, c.W, c.H, u, &f);
        s = f.s; ox = f.ox; oz = f.oz;
        cp = cell * s;
        for (i = 0; i < mw * mh; i++)
            if (v->metal[i] > top) top = v->metal[i];

        /* the game's left sidebar, before the map: the cursor and the overlays are drawn over it */
        if (v->has_side) {
            mark(&c, WIKISTAGE_IN_SIDE, 0);
            side(&c, v, art, ox, oz, s, top);
        }

        /* the ground: plain cells light, metal darker by how far it stands above surfaceMetal */
        mark(&c, WIKISTAGE_IN_GROUND, 0);
        for (z = 0; z < mh; z++)
            for (x = 0; x < mw; x++)
                rect(&c, floor(X(x * cell)), floor(Z(z * cell)), ceil(cp) + 1, ceil(cp) + 1, ground(v, z * mw + x, top));
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
        mark(&c, WIKISTAGE_IN_OVER, 0);
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
            mark(&c, WIKISTAGE_IN_OVER, 0);
            dashed_rect(&c, bx, by, w, h, fmax(1, 1.5 * U), team(q->owner), 3 * U, 2 * U);
            mark(&c, WIKISTAGE_IN_LABEL, 0);
            label(&c, q->type < v->ntype ? v->type[q->type] : "", w, h, X(q->x), Z(q->z), rgba(20, 20, 20, .75));
        }
        /* units: grey boxes at their footprint, a team-coloured edge; construction fills up from
           the bottom, a reclaim empties from the top */
        for (i = 0; i < v->nunit; i++) {
            const WIKISTAGEUNIT* un = &v->unit[i];
            const double w = un->fx * cp, h = un->fz * cp, bx = X(un->x) - w / 2, by = Z(un->z) - h / 2;
            const double built = fmin(fmax(un->built, 0), 1), rec = fmin(fmax(un->reclaimed, 0), 1);
            const double bw = fmax(1.5, 2 * U);
            mark(&c, WIKISTAGE_IN_BOX, un->type);
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
                mark(&c, WIKISTAGE_IN_OVER, 0);
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
            mark(&c, WIKISTAGE_IN_BOX, st->type);
            rect(&c, bx + lw, by + lw, w - 2 * lw, h - 2 * lw, rgba(138, 138, 132, .45));
            mark(&c, WIKISTAGE_IN_OVER, 0);
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
            mark(&c, WIKISTAGE_IN_BOX, gh->type);
            rect(&c, bx, by, w, h, gh->ok ? rgba(40, 210, 70, .35) : rgba(230, 40, 40, .35));
            mark(&c, WIKISTAGE_IN_OVER, 0);
            ring(&c, bx, by, w, h, fmax(1, 1.5 * U), HEX(gh->ok ? 0x2bd248 : 0xe62828));
            mark(&c, WIKISTAGE_IN_BOX, gh->type);
            label(&c, gh->type < v->ntype ? v->type[gh->type] : "", w, h, X(gh->x), Z(gh->z),
                  gh->ok ? rgba(12, 58, 18, 1) : rgba(74, 10, 10, 1));
        }
        mark(&c, WIKISTAGE_IN_OVER, 0);
        if (v->has_drag) {
            const double x0 = X(fmin(v->drag_x0, v->drag_x1)), y0 = Z(fmin(v->drag_z0, v->drag_z1));
            ring(&c, x0, y0, fabs((double)v->drag_x1 - v->drag_x0) * s, fabs((double)v->drag_z1 - v->drag_z0) * s, fmax(1, U), HEX(0xf2f6e8));
        }
        /* THE CURSOR: the game's own, as sharp_cursor (tagpu_gui_surf.c) draws the player's -- the
           frame its animation shows, at `spr_scale` device pixels an art pixel whatever the map's
           scale or the HUD's, the hotspot on the pointer's device pixel. Else, for a shape whose
           frames did not load, its kit icon centred on the point, in the box stage.js snaps. Neither
           when the caller leaves the cursor out. */
        const WIKISTAGESEQ* sq = cursor && v->cursor_shape < 3 ? &art->seq[v->cursor_shape] : NULL;
        if (sq && sq->n > 0) {
            const int f = tagpu_wikistage_cursor_frame(art->spr + sq->first, sq->n, sq->loop,
                                                       (long)v->tick - v->cursor_since);
            if (f >= 0) {
                const WIKISTAGESPR* q = &art->spr[sq->first + f];
                const float k = art->spr_scale;
                const double dx = snapd(X(v->cursor_x)), dy = snapd(Z(v->cursor_z));
                const int w = (int)((float)q->w * k + 0.5f), h = (int)((float)q->h * k + 0.5f);
                HUDOP* p = w > 0 && h > 0 ? op(&c, HOP_PART, dx - (int)((float)q->hx * k), dy - (int)((float)q->hy * k), w, h) : NULL;
                if (p) {
                    p->flags = HOPF_SPRITE;
                    p->part = (int16_t)(sq->first + f);
                    p->size = q->w;
                    p->shade = q->h;
                    out->cursor = (int)(p - out->op);
                }
            }
        } else if (cursor && v->cursor_shape < 3 && art->cursor[v->cursor_shape] >= 0) {
            const int k = v->cursor_shape;
            const double w = art->cursor_w[k] * U, h = art->cursor_h[k] * U;
            const double a = X(v->cursor_x) - w / 2, b = Z(v->cursor_z) - h / 2;
            const double x0 = snapd(a), y0 = snapd(b);
            HUDOP* p = op(&c, HOP_PART, x0, y0, snapd(a + w) - x0, snapd(b + h) - y0);
            if (p) {
                p->part = art->cursor[k];
                out->cursor = (int)(p - out->op);
            }
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
        mark(&c, WIKISTAGE_IN_OVER, 0);
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

uint32_t tagpu_wikistage_pose(WIKISTAGEVIEW* out, const WIKISIM* s)
{
    uint32_t t = 0;
    int i, k;
    for (i = 0; i < out->nunit; i++) {
        WIKISTAGEUNIT* d = &out->unit[i];
        int n;
        const unsigned char* m = tagpu_wikisim_model(s, i, &n);
        if (!m || n <= 0) continue;
        if (n > WIKISTAGE_PIECES || n > WIKISTAGE_POSE - out->npose) { t |= WIKISTAGE_TRUNC_POSE; continue; }
        for (k = 0; k < n; k++) {
            const unsigned char* p = m + TAGPU_COBVM_PRIM_BASE + (size_t)k * TAGPU_COBVM_PRIM_BYTES;
            WIKISTAGEPIECE* q = &out->pose[out->npose + k];
            memcpy(q->pos, p + TAGPU_COBVM_PRIM_POS, sizeof q->pos);
            memcpy(q->turn, p + TAGPU_COBVM_PRIM_TURN, sizeof q->turn);
            memcpy(&q->flags, p + TAGPU_COBVM_PRIM_FLAGS, sizeof q->flags);
        }
        d->pose_at = out->npose;
        d->pose_n = (uint16_t)n;
        out->npose = (uint16_t)(out->npose + n);
    }
    out->trunc |= t;
    return t;
}
