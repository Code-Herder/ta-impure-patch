/* tagpu_fxmodel.c — the effects models' rasteriser. tagpu_fxmodel.h is the
   contract and says why it rasterises rather than draws geometry; this file
   is the engine's arithmetic, transcribed.

   EVERY EXPRESSION BELOW IS THE ENGINE'S, and the ones that look arbitrary
   are the ones to keep: the 0xFFFF an edge starts from, the TRUNCATING
   divisions (`idiv`, which C's `/` on int32 is), the multiply that advances
   a clipped edge in one step (the same integer the engine's `imul` gives, and
   the same one stepping row by row would reach -- 32-bit wrap included, which
   is why every product and sum is taken in uint32), the arithmetic shifts, and
   the SEQUENTIAL row buffer. That last one is not an implementation detail:
   the edge walk appends a row per scanline it covers to one buffer per side,
   and the fill reads the buffer by position, not by y. On a quad whose chain
   doubles back -- a face seen edge-on, projected into a bow-tie -- the two
   disagree, and the engine's answer is the buffer's. DISASSEMBLED
   (0x4C7580..0x4C7A1C, 0x4C7310..0x4C74E5, 0x4C0330..0x4C06D1).

   THE ROWS ALWAYS COVER THE FILL. A chain runs from the first vertex at the
   least y to the first at the greatest, so the edges it walks downward cover
   every scanline in between at least once, clipped or not: each side writes at
   least `ymax - ymin` rows, which is every row the fill reads. The buffer is
   sized per face from the same count and a face that would need more than the
   bound is not drawn and says so (`TAGPU_FXM_ROOM`); on data from the packet
   neither can happen, and the check is the bound on a value, not the argument.

   RENDER THREAD ONLY. */

#include <stdlib.h>
#include <string.h>
#include "tagpu_fxmodel.h"
#include "tagpu_packet.h"
#include "tagpu_render3do.h"

/* ---- the run arena ------------------------------------------------------ */
static TAGPU_FXRUN* s_run;
static unsigned     s_runCap, s_nrun;
static int          s_why;
/* the most runs one frame keeps: a run is at most one pixel of one model on
   one row, so this is sixty 1080p screens of model pixels -- a bound on an
   allocation driven by engine counts, never a budget a real frame meets */
#define RUN_MAX (1u << 23)

void tagpu_fxmodel_frame(void) { s_nrun = 0; }
unsigned tagpu_fxmodel_mark(void) { return s_nrun; }
void tagpu_fxmodel_rewind(unsigned mark) { if (mark < s_nrun) s_nrun = mark; }
const TAGPU_FXRUN* tagpu_fxmodel_runs(unsigned* n) { *n = s_nrun; return s_run; }
int tagpu_fxmodel_why(void) { return s_why; }

static int run_room(unsigned more)
{
    unsigned need = s_nrun + more, n;
    TAGPU_FXRUN* q;
    if (more > RUN_MAX || need > RUN_MAX) return 0;
    if (need <= s_runCap) return 1;
    n = s_runCap ? s_runCap : 4096u;
    while (n < need) n *= 2u;
    q = (TAGPU_FXRUN*)realloc(s_run, (size_t)n * sizeof *q);
    if (!q) return 0;
    s_run = q; s_runCap = n;
    return 1;
}

/* ---- the row buffer ------------------------------------------------------
   One entry per scanline a chain wrote, as the engine's 40-byte stack rows:
   the left chain fills xl/ul/vl, the right xr/ur/vr, each from index 0 in
   the order it walks. `nl`/`nr` are how many each side wrote. */
typedef struct { int32_t xl, xr, ul, vl, ur, vr; } ROW;
static ROW*     s_row;
static unsigned s_rowCap;
/* a face taller than this many scanlines per edge is not drawn: 0x4C7580
   keeps its rows in a 32 140-byte frame, so no face the engine can draw has
   more than ~800 */
#define ROW_MAX 65536u

static int row_room(unsigned need)
{
    unsigned n;
    ROW* q;
    if (need > ROW_MAX) return 0;
    if (need <= s_rowCap) return 1;
    n = s_rowCap ? s_rowCap : 1024u;
    while (n < need) n *= 2u;
    q = (ROW*)realloc(s_row, (size_t)n * sizeof *q);
    if (!q) return 0;
    s_row = q; s_rowCap = n;
    return 1;
}

static __inline int32_t sar16(int32_t v) { return v >> 16; }   /* `sar`, as GCC's >> is */
static __inline int32_t shl16(int32_t v) { return (int32_t)((uint32_t)v << 16); }
static __inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static __inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static __inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

typedef struct { int32_t x, y; } PT;

/* the chains' extremes: FIRST vertex at the strict least y and at the strict
   greatest, and the x range -- one pass, the engine's compares */
static void bounds(const PT* p, int n, int* top, int* bot,
                   int32_t* ymin, int32_t* ymax, int32_t* xmin, int32_t* xmax)
{
    int i;
    *ymin = 999999; *ymax = -999999; *xmin = 999999; *xmax = -999999;
    *top = 0; *bot = 0;
    for (i = 0; i < n; i++) {
        if (p[i].y < *ymin) { *ymin = p[i].y; *top = i; }
        if (p[i].y > *ymax) { *ymax = p[i].y; *bot = i; }
        if (p[i].x > *xmax) *xmax = p[i].x;
        if (p[i].x < *xmin) *xmin = p[i].x;
    }
}

/* ONE EDGE, a to b, appended to side `right`'s rows. The engine skips an edge
   that ends at or above the clip's top and one that does not go down. `uv`
   NULL walks x alone (0x4C0330). Returns 0 only past ROW_MAX. */
static int edge(const PT* a, const PT* b, const int32_t* uva, const int32_t* uvb,
                int right, unsigned* nw, const TAGPU_FXRVIEW* v)
{
    int32_t ya = a->y, yb = b->y, dy, x, dx, u = 0, du = 0, w = 0, dw = 0, k;
    if (yb <= v->clipT || ya >= yb) return 1;
    dy = yb - ya;
    dx = (int32_t)((uint32_t)sub32(b->x, a->x) << 16) / dy;
    x = add32(shl16(a->x), 0xFFFF);
    if (uva) {
        u = shl16(uva[0]); du = sub32(shl16(uvb[0]), u) / dy;
        w = shl16(uva[1]); dw = sub32(shl16(uvb[1]), w) / dy;
    }
    if (ya < v->clipT) {
        const int32_t s = v->clipT - ya;
        x = add32(x, mul32(dx, s));
        u = add32(u, mul32(du, s));
        w = add32(w, mul32(dw, s));
        ya = v->clipT;
    }
    if (yb > v->clipB) yb = v->clipB;
    if (ya >= yb) return 1;
    if (!row_room(*nw + (unsigned)(yb - ya))) return 0;
    for (k = ya; k < yb; k++) {
        ROW* r = &s_row[(*nw)++];
        if (right) { r->xr = sar16(x); r->ur = u; r->vr = w; }
        else       { r->xl = sar16(x); r->ul = u; r->vl = w; }
        x = add32(x, dx); u = add32(u, du); w = add32(w, dw);
    }
    return 1;
}

/* BOTH CHAINS of an n-gon from `top` to `bot`: the left one BACKWARDS
   (cur - 1, wrapping to n - 1), the right one forwards. Returns the rows the
   fill may read, or -1 past ROW_MAX. */
static int chains(const PT* p, const int32_t (*uv)[2], int n, int top, int bot,
                  int32_t ymin, int32_t ymax, const TAGPU_FXRVIEW* v)
{
    unsigned nl = 0, nr = 0;
    int cur, nx;
    const unsigned need = (unsigned)(ymax - ymin);
    /* every row the fill reads is written first: rows past a side's count
       would be the ENGINE's stale stack, which nothing here can reproduce --
       the bound, not the argument (the header above has the argument) */
    for (cur = top; cur != bot; cur = nx) {
        nx = cur - 1 < 0 ? n - 1 : cur - 1;
        if (!edge(&p[cur], &p[nx], uv ? uv[cur] : NULL, uv ? uv[nx] : NULL, 0, &nl, v))
            return -1;
    }
    for (cur = top; cur != bot; cur = nx) {
        nx = cur + 1 >= n ? 0 : cur + 1;
        if (!edge(&p[cur], &p[nx], uv ? uv[cur] : NULL, uv ? uv[nx] : NULL, 1, &nr, v))
            return -1;
    }
    if (nl < need || nr < need) return -1;
    return (int)need;
}

/* ---- run emission ------------------------------------------------------- */
static __inline int emit(int32_t x0, int32_t x1, int32_t y, float u, float w, float fc,
                         const TAGPU_FXRVIEW* v)
{
    TAGPU_FXRUN* r;
    if (!run_room(1)) return 0;
    r = &s_run[s_nrun++];
    r->x0 = (float)(x0 + v->ox); r->x1 = (float)(x1 + v->ox); r->y = (float)(y + v->oy);
    r->pad0 = 0.0f;
    r->u = u; r->v = w; r->fc = fc; r->pad1 = 0.0f;
    return 1;
}

/* ---- 0x4C0330, the flat fill -------------------------------------------- */
static int flat_face(const PT* p, int n, unsigned colour, const TAGPU_FXRVIEW* v)
{
    int top, bot, rows, i;
    int32_t ymin, ymax, xmin, xmax;
    const float fc = (float)(colour & 0xFFu) / 255.0f;
    bounds(p, n, &top, &bot, &ymin, &ymax, &xmin, &xmax);
    if (xmax < v->clipL || xmin > v->clipR || ymax < v->clipT || ymin > v->clipB) return 1;
    if (ymin < v->clipT) ymin = v->clipT;
    if (ymax > v->clipB) ymax = v->clipB;
    if (ymax == ymin) return 1;
    rows = chains(p, NULL, n, top, bot, ymin, ymax, v);
    if (rows < 0) { s_why = TAGPU_FXM_ROOM; return 0; }
    for (i = 0; i < rows; i++) {
        int32_t xl = s_row[i].xl, xr = s_row[i].xr;
        if (xr > v->clipR) xr = v->clipR;
        if (xl < v->clipL) xl = v->clipL;
        if (xr - xl > 0 && !emit(xl, xr, ymin + i, -1.0f, -1.0f, fc, v)) {
            s_why = TAGPU_FXM_ROOM; return 0;
        }
    }
    return 1;
}

/* ---- 0x4C7310, one textured span ------------------------------------------
   The texel a pixel takes, by the frame width's own addressing (the width
   dispatch at 0x4C73BD, table 0x4C7500 into 0x4C74E8): 8 inline, 16/32/64/128
   through 0x4CD962/0x4CD91E/0x4CD8DA/0x4CD896, which mask v shifted by
   log2(w) and add u before the shift, and every other width `(v >> 16) * w +
   (u >> 16)`. On a u and v inside the frame all three are that last one; they
   are kept apart because outside it they are not, and the engine's answer is
   the one to reproduce. */
static __inline int32_t texel_at(int32_t u, int32_t w_, int fw)
{
    switch (fw) {
    case 8:   return ((w_ >> 13) & ~7) + sar16(u);
    case 16:  return (int32_t)(((((uint32_t)w_ << 4) & 0xFFF00000u) + (uint32_t)u) >> 16);
    case 32:  return (int32_t)(((((uint32_t)w_ << 5) & 0xFFE00000u) + (uint32_t)u) >> 16);
    case 64:  return (int32_t)(((((uint32_t)w_ << 6) & 0xFFC00000u) + (uint32_t)u) >> 16);
    case 128: return (int32_t)(((((uint32_t)w_ << 7) & 0xFF800000u) + (uint32_t)u) >> 16);
    default:  return add32(mul32(sar16(w_), fw), sar16(u));
    }
}

static int span(const ROW* r0, int32_t y, const TAGPU_R3DTEX* t, const TAGPU_FXRVIEW* v)
{
    int32_t xl = r0->xl, xr = r0->xr, u = r0->ul, w = r0->vl, du, dw, x, runX = 0;
    int32_t runT = -1;
    const int32_t count0 = xr - xl;
    const int32_t size = t->w * t->h;
    du = sub32(r0->ur, r0->ul) / count0;
    dw = sub32(r0->vr, r0->vl) / count0;
    if (xl < v->clipL) {
        const int32_t s = v->clipL - xl;
        xl = v->clipL;
        u = add32(u, mul32(du, s));
        w = add32(w, mul32(dw, s));
    }
    if (xr > v->clipR) xr = v->clipR;
    if (xr - xl <= 0) return 1;
    for (x = xl; x <= xr; x++) {
        int32_t ti = -2;
        if (x < xr) {
            ti = texel_at(u, w, t->w);
            /* A TEXEL OUTSIDE THE FRAME IS A READ OUTSIDE ITS PIXELS: on the
               engine's arithmetic it cannot happen (u and v stay between the
               values at the span's two ends, and those between the face's
               corners), and if it did the engine would copy whatever byte is
               there -- which nothing here can reproduce, so the model is lost
               rather than drawn with a guess */
            if (ti < 0 || ti >= size) { s_why = TAGPU_FXM_TEXEL; return 0; }
            u = add32(u, du); w = add32(w, dw);
        }
        if (ti != runT) {
            if (runT >= 0) {
                const int tx = runT % t->w, ty = runT / t->w;
                const size_t a = (size_t)(t->y + ty) * (size_t)t->dim + (size_t)(t->x + tx);
                int ok;
                /* THE SPAN COPIES EVERY TEXEL, the frame's key included; the
                   atlas marks a key texel a hole, which the fragment stage
                   discards, so a key texel travels as its palette index */
                if (t->key[a] == 0)
                    ok = emit(runX, x, y, -1.0f, -1.0f, (float)t->idx[a] / 255.0f, v);
                else
                    ok = emit(runX, x, y, ((float)(t->x + tx) + 0.5f) / (float)t->dim,
                              ((float)(t->y + ty) + 0.5f) / (float)t->dim, 0.0f, v);
                if (!ok) { s_why = TAGPU_FXM_ROOM; return 0; }
            }
            runT = ti; runX = x;
        }
    }
    return 1;
}

/* ---- 0x4C7580, the textured quad -----------------------------------------
   Called by the face walk with NO uv array, so it makes its own from the
   frame's size: (0,0) (w-1,0) (w-1,h-1) (0,h-1), one per vertex in the
   face's order. */
static int quad_face(const PT* p, const TAGPU_R3DTEX* t, const TAGPU_FXRVIEW* v)
{
    int32_t uv[4][2];
    int top, bot, rows, i;
    int32_t ymin, ymax, xmin, xmax;
    uv[0][0] = 0;        uv[0][1] = 0;
    uv[1][0] = t->w - 1; uv[1][1] = 0;
    uv[2][0] = t->w - 1; uv[2][1] = t->h - 1;
    uv[3][0] = 0;        uv[3][1] = t->h - 1;
    bounds(p, 4, &top, &bot, &ymin, &ymax, &xmin, &xmax);
    if (xmax < v->clipL || xmin > v->clipR || ymax < v->clipT || ymin > v->clipB) return 1;
    if (ymin < v->clipT) ymin = v->clipT;
    if (ymax > v->clipB) ymax = v->clipB;
    if (ymax == ymin) return 1;
    rows = chains(p, (const int32_t (*)[2])uv, 4, top, bot, ymin, ymax, v);
    if (rows < 0) { s_why = TAGPU_FXM_ROOM; return 0; }
    for (i = 0; i < rows; i++)
        if (s_row[i].xr - s_row[i].xl > 0 && !span(&s_row[i], ymin + i, t, v)) return 0;
    return 1;
}

/* ---- the face walk (0x46BAE0, 0x4211D0) -----------------------------------
   Every vertex projected once, the way both draws project it: the 16.16 sum
   shifted to whole pixels, the eye taken off, cut to 16 bits, the altitude's
   whole part halved (arithmetic, on the 16-bit value) and taken off the row.
   The eye is taken off in 16.16 before the shift, as the engine's callers
   hand it an eye-relative position; `(s - (eye << 16)) >> 16` is exactly what
   they compute. Then the faces in order: the shape holds only those the draw
   paints, with the frame each one takes. */
static PT* s_pt;
static unsigned s_ptCap;

int tagpu_fxmodel_raster(const TAGPU_PACKET* pk, unsigned model, const TAGPU_FXRVIEW* v)
{
    const TAGPU_PK_FXMODEL* m;
    const TAGPU_PK_FXSHAPE* sh;
    const TAGPU_PK_FXFACE* fa;
    const TAGPU_PK_FXVERT* vt;
    const uint16_t* ix;
    const unsigned mark = s_nrun;
    const int32_t ex = shl16(v->eyeX), ey = shl16(v->eyeY);
    unsigned i, j;
    s_why = TAGPU_FXM_OK;
    /* THE TABLES WERE VALIDATED WHEN THE PACKET WAS ACQUIRED (tagpu_packet.c
       `frame_valid`: every model's shape and vertex run, every face's index
       run, every index against its shape's vertex count). The two compares
       here are this file's own terms for the one index it is handed. */
    if (model >= pk->n_fxmodel) { s_why = TAGPU_FXM_NOTCARRIED; return 0; }
    m = &tagpu_pk_fxmodel(pk)[model];
    if (m->shape >= pk->n_fxshape) { s_why = TAGPU_FXM_NOTCARRIED; return 0; }
    sh = &tagpu_pk_fxshape(pk)[m->shape];
    if (sh->nface == 0) return 1;
    fa = tagpu_pk_fxface(pk) + sh->face;
    vt = tagpu_pk_fxvert(pk) + m->vert;
    ix = tagpu_pk_fxidx(pk);
    if (sh->nvert > s_ptCap) {
        PT* q = (PT*)realloc(s_pt, (size_t)sh->nvert * sizeof *q);
        if (!q) { s_why = TAGPU_FXM_ROOM; return 0; }
        s_pt = q; s_ptCap = sh->nvert;
    }
    for (i = 0; i < sh->nvert; i++) {
        const int32_t sx = (int16_t)sar16(sub32(vt[i].x, ex));
        const int32_t sy = (int16_t)sar16(sub32(vt[i].y, ey));
        const int32_t sa = (int16_t)sar16(vt[i].alt);
        s_pt[i].x = sx + 0x80;
        s_pt[i].y = sy - (sa >> 1) + 0x20;
    }
    for (j = 0; j < sh->nface; j++) {
        const TAGPU_PK_FXFACE* f = &fa[j];
        PT q[TAGPU_PK_FXMAXFV];
        unsigned k;
        for (k = 0; k < f->n && k < TAGPU_PK_FXMAXFV; k++) q[k] = s_pt[ix[f->idx + k]];
        if (f->flat) {
            if (!flat_face(q, (int)k, f->colour, v)) goto lost;
        } else {
            TAGPU_R3DTEX t;
            const int r = tagpu_r3d_atlas_texels((const char*)(size_t)f->frame, &t);
            if (r <= 0 || t.w < 1 || t.h < 1) { s_why = TAGPU_FXM_TEXEL; goto lost; }
            if (!quad_face(q, &t, v)) goto lost;
        }
    }
    return 1;
lost:
    s_nrun = mark;
    return 0;
}
