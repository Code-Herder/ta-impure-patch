/* tagpu_icon.c -- the strategic icons on the render thread (tagpu_icon.h).

   Past BAR's distance a unit is drawn as a flat icon of constant screen size
   (inc/tagpu_icon_rule.h); past the point its model is smaller than the icon,
   the model is not drawn at all. This file decides both per unit from the
   frame's packet -- the unit table, the type table the publisher built
   (TAGPU_PK_UTYPE) and the screen size -- and the three settings, queues the
   icons the unit gather places, and hands them to the marker pass in BAR's
   draw order. It reads only the packet and the published art. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_icon.h"
#include "tagpu_icontype.h"
#include "tagpu_icon_rule.h"
#include "tagpu_mark.h"
#include "tagpu_settings.h"
#include "tagpu_log.h"

#define IC_HIDE   0x01u          /* the model is not drawn                   */
#define IC_SUPP   0x02u          /* cargo of a hidden unit: no model, no icon */
#define IC_LOG_FRAMES 300

/* one queued icon */
typedef struct {
    unsigned long long key;      /* layer << 48 | sortable(ay) << 16 | index  */
    float    ax, ay;
    const TAGPU_ICONRECT* art;   /* a picture in the art, NULL for a shape   */
    unsigned short ew, eh;       /* edge, screen px                          */
    unsigned char team, sel, alpha, shape;
} ICREC;

/* Per packet unit, this frame. Sized by the design point, which bounds every
   packet's unit table: a frame with more units than that has no icons (and
   says so) rather than writing past the arrays. */
static unsigned char s_alpha[TAGPU_PK_DESIGN_SLOTS];
static unsigned char s_flag[TAGPU_PK_DESIGN_SLOTS];
static ICREC         s_rec[TAGPU_PK_DESIGN_SLOTS];
static unsigned      s_n, s_nrec, s_drop;
static int           s_on;
static int           s_cmdOn;                   /* s_on, until the record takes it */
static TAGPU_ICON_VIEW s_view;
static float         s_zoom;
static int           s_size[TAGPU_ICLS_N], s_edge[TAGPU_ICLS_N], s_dotK;
static unsigned char s_ink;                     /* gui_col[0], a logo-less fill */
static const TAGPU_ICONART*  s_art;
static const TAGPU_PK_UTYPE* s_types;
static unsigned      s_ntypes;
/* the log's numbers: this frame's counts by class, and the cost */
static unsigned      s_cls[TAGPU_ICLS_N], s_hidden, s_supp, s_dots, s_setpics;
static LARGE_INTEGER s_freq;
static double        s_usFrame, s_usEmit;
static unsigned      s_frames;
static int           s_saidCap;

static double us_since(const LARGE_INTEGER* t0)
{
    LARGE_INTEGER t1;
    if (!s_freq.QuadPart) return 0.0;
    QueryPerformanceCounter(&t1);
    return (double)(t1.QuadPart - t0->QuadPart) * 1e6 / (double)s_freq.QuadPart;
}

/* The decisions of one frame. RENDER THREAD; called once a frame before the
   unit gather and the marker gather, which both read these arrays in the same
   frame on the same thread -- program order is the ordering. It reads only the
   packet and the published art. */
void tagpu_icon_frame(const TAGPU_PACKET* pk, float zoom, int on)
{
    const TAGPU_PK_UNIT* units;
    LARGE_INTEGER t0;
    unsigned i;
    int scale, dist, fade, c, pass;

    s_on = 0; s_n = 0; s_nrec = 0; s_drop = 0;
    s_hidden = s_supp = s_dots = s_setpics = 0;
    memset(s_cls, 0, sizeof s_cls);
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&t0);
    s_frames++;
    if (!on || !pk || !pk->in_game || !(zoom > 0.0f)) return;
    s_art = tagpu_icontype_art();
    s_types = tagpu_pk_utypes(pk);
    s_ntypes = pk->utype_n;
    if (!s_art || !s_types) return;
    if (pk->n_units > TAGPU_PK_DESIGN_SLOTS) {
        if (!s_saidCap) {
            char b[128];
            s_saidCap = 1;
            _snprintf(b, sizeof b, "icons: %u units in the packet, past the %u the icons hold - "
                      "no icons while that is true", pk->n_units, (unsigned)TAGPU_PK_DESIGN_SLOTS);
            b[sizeof b - 1] = 0; tagpu_log(b);
        }
        return;
    }
    tagpu_settings_icons(&scale, &dist, &fade);
    tagpu_icon_view(&s_view, pk->screen[0], pk->screen[1], scale, dist, fade);
    s_zoom = zoom;
    for (c = 0; c < TAGPU_ICLS_N; c++) {
        s_size[c] = tagpu_icon_bar_edge(&s_view, c);
        s_edge[c] = tagpu_icon_gen_edge(&s_view, c);
    }
    s_dotK = tagpu_icon_dot_k(&s_view);
    s_ink = pk->gui_col[0];
    s_n = pk->n_units;
    units = tagpu_pk_units(pk);
    /* NEVER AN ICON: a unit the unit pass does not draw -- it keeps the
       engine's sprite, its bar and its digit --, a nanoframe -- it is the
       build's own picture -- or a unit with no health left. The type row is
       bounded by the table's count, the class by the class count and a set
       picture by the art's (tagpu_icon_type); anything else is drawn as its
       model. */
    for (i = 0; i < s_n; i++) {
        const TAGPU_PK_UNIT* u = &units[i];
        int a = 0, h = 0;
        if ((u->flags & TAGPU_PK_U_NATIVE) && u->type_row < s_ntypes && !(u->nano > 0.0f) && u->health > 0) {
            const TAGPU_PK_UTYPE* t = &s_types[u->type_row];
            int size, edge;
            if (t->cls < TAGPU_ICLS_N) {
                tagpu_icon_type(&s_view, s_art, t->cls, t->icon, &size, &edge);
                a = tagpu_icon_eval(&s_view, zoom, size, edge, t->r_px, &h);
            }
        }
        s_alpha[i] = (unsigned char)a;
        s_flag[i] = h ? IC_HIDE : 0u;
    }
    /* THE CARGO OF A HIDDEN UNIT is part of its picture -- a unit on a factory
       pad, one in a transport -- so it is neither a model nor an icon of its
       own, and nor is the cargo of that cargo: TAGPU_ICON_CARGO_HOPS rounds
       each mark the cargo lists of every hidden or suppressed unit, so a chain
       of carriers that many deep is followed down, and the pick's walk up the
       carrier pointers (tagpu_zoom.c's icon_hit) follows the same chains the
       same distance. Only a unit the unit pass draws is suppressed, and only
       its cargo is marked in turn: a carrier the engine draws hides nothing,
       as the pick's walk stops at one. Every hop is a packet index the
       publisher bounded (slot_of), checked against this frame's count again,
       and a list is followed at most TAGPU_ICON_CARGO_HOPS members, the
       engine's own guard on a list that loops. */
    for (pass = 0; pass < TAGPU_ICON_CARGO_HOPS; pass++) {
        int changed = 0;
        for (i = 0; i < s_n; i++) {
            int ci, guard = 0;
            if (!(s_flag[i] & (IC_HIDE | IC_SUPP))) continue;
            for (ci = units[i].cargo_first; ci >= 0 && (unsigned)ci < s_n && guard < TAGPU_ICON_CARGO_HOPS;
                 guard++) {
                if ((units[ci].flags & TAGPU_PK_U_NATIVE) && !(s_flag[ci] & IC_SUPP)) {
                    s_flag[ci] |= IC_SUPP;
                    changed = 1;
                }
                ci = units[ci].cargo_next;
            }
        }
        if (!changed) break;
    }
    for (i = 0; i < s_n; i++) {
        if (s_flag[i] & IC_SUPP) s_supp++;
        else if (s_flag[i] & IC_HIDE) s_hidden++;
    }
    s_on = 1;
    s_cmdOn = 1;
    s_usFrame = us_since(&t0);
}

int tagpu_icon_on(void)               { return s_on; }
int tagpu_icon_cmd_take(void)         { const int on = s_cmdOn; s_cmdOn = 0; return on; }
int tagpu_icon_alpha(unsigned i)      { return s_on && i < s_n && !(s_flag[i] & IC_SUPP) ? s_alpha[i] : 0; }
int tagpu_icon_hidden(unsigned i)     { return s_on && i < s_n && (s_flag[i] & IC_HIDE) != 0; }
int tagpu_icon_suppressed(unsigned i) { return s_on && i < s_n && (s_flag[i] & IC_SUPP) != 0; }
int tagpu_icon_hides(unsigned i)      { return s_on && i < s_n && s_flag[i] != 0; }

/* a float's bits as an unsigned that sorts in the float's order */
static unsigned sortable(float f)
{
    unsigned b;
    memcpy(&b, &f, sizeof b);
    return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

int tagpu_icon_push(unsigned i, const TAGPU_PK_UNIT* u, float ax, float ay, int sel, int dot)
{
    ICREC* r;
    unsigned layer;
    int hide;
    if (!s_on || i >= s_n) return 0;
    if (s_nrec >= TAGPU_PK_DESIGN_SLOTS) { s_drop++; return 0; }
    r = &s_rec[s_nrec];
    if (dot) {
        /* a radar contact: the mod set's NONE picture -- its art for an
           unidentified contact -- at the set's scale when it has one, else
           the minimap's dot, the owner's logo frame at its own scale */
        const TAGPU_ICONRECT* f;
        int k, e;
        if (s_art->none >= 0 && s_art->none < s_art->nset && s_art->none < TAGPU_ICONART_SETS) {
            f = &s_art->set[s_art->none];
            k = tagpu_icon_art_k(&s_view);
            r->team = u->logo < s_art->nlogo ? s_art->team[u->logo] : s_ink;
        } else {
            if (u->logo >= s_art->nlogo) return 0;
            f = &s_art->logo[u->logo];
            k = s_dotK;
            r->team = s_art->team[u->logo];
        }
        e = k * (f->w > f->h ? f->w : f->h);
        r->alpha = (unsigned char)tagpu_icon_eval(&s_view, s_zoom, e, e, 0u, &hide);
        if (!r->alpha) return 0;
        r->art = f;
        r->ew = (unsigned short)(k * f->w); r->eh = (unsigned short)(k * f->h);
        r->shape = 0;
        layer = 1;                       /* BAR's unidentified contact: drawOrder 1 */
        s_dots++;
    } else {
        const TAGPU_PK_UTYPE* t;
        const TAGPU_ICONRECT* pic;
        int size, edge;
        if (!s_alpha[i] || u->type_row >= s_ntypes) return 0;
        t = &s_types[u->type_row];
        if (t->cls >= TAGPU_ICLS_N) return 0;
        pic = tagpu_icon_type(&s_view, s_art, t->cls, t->icon, &size, &edge);
        r->alpha = s_alpha[i];
        r->art = pic;
        if (pic) {
            /* the set's picture at the art scale, its own aspect kept */
            const int k = tagpu_icon_art_k(&s_view);
            r->ew = (unsigned short)(k * pic->w); r->eh = (unsigned short)(k * pic->h);
            r->shape = 0;
            s_setpics++;
        } else {
            r->ew = r->eh = (unsigned short)edge;
            r->shape = (unsigned char)(t->cls + 1);
        }
        r->team = u->logo < s_art->nlogo ? s_art->team[u->logo] : s_ink;
        layer = t->layer & 3u;
        s_cls[t->cls]++;
    }
    r->ax = ax; r->ay = ay;
    r->sel = (unsigned char)(sel != 0);
    r->key = ((unsigned long long)layer << 48) | ((unsigned long long)sortable(ay) << 16) | (i & 0xFFFFu);
    s_nrec++;
    return 1;
}

static int rec_cmp(const void* a, const void* b)
{
    const unsigned long long x = ((const ICREC*)a)->key, y = ((const ICREC*)b)->key;
    return x < y ? -1 : x > y;
}

void tagpu_icon_emit(void)
{
    LARGE_INTEGER t0;
    unsigned k;
    QueryPerformanceCounter(&t0);
    if (s_on && s_nrec) {
        /* BAR's order (Recoil UnitDrawer.cpp, UnitsIconsSortedByDepth): the
           layer, then far to near -- a smaller anchor y is further up the
           screen, further away -- then the gather's index */
        qsort(s_rec, s_nrec, sizeof s_rec[0], rec_cmp);
        for (k = 0; k < s_nrec; k++) {
            const ICREC* r = &s_rec[k];
            if (!tagpu_mark_emit_icon(r->ax, r->ay, r->art, r->shape, r->ew, r->eh, r->team, r->sel,
                                      r->alpha))
                s_drop++;
        }
    }
    s_usEmit = us_since(&t0);
    if (s_frames % IC_LOG_FRAMES == 0) {
        char b[400];
        int dh = 0;
        const int dotA = s_on ? tagpu_icon_eval(&s_view, s_zoom, 4 * s_dotK, 4 * s_dotK, 0u, &dh) : 0;
        int combatA = 0;
        if (s_on) combatA = tagpu_icon_eval(&s_view, s_zoom, s_size[TAGPU_ICLS_COMBAT], s_edge[TAGPU_ICLS_COMBAT],
                                          0u, &dh);
        _snprintf(b, sizeof b,
                  "icons: on=%d units=%u icons=%u set=%u hidden=%u cargo=%u dots=%u drop=%u "
                  "base=%.2f zS=%.4f z=%.3f alpha(combat)=%d alpha(dot)=%d "
                  "edges=%d/%d/%d/%d/%d/%d/%d/%d/%d dotk=%d "
                  "by-class=%u/%u/%u/%u/%u/%u/%u/%u/%u us=%.1f+%.1f",
                  s_on, s_n, s_nrec, s_setpics, s_hidden, s_supp, s_dots, s_drop,
                  s_view.base, s_view.zS, s_zoom, combatA, dotA,
                  s_edge[0], s_edge[1], s_edge[2], s_edge[3], s_edge[4], s_edge[5], s_edge[6],
                  s_edge[7], s_edge[8], s_dotK,
                  s_cls[0], s_cls[1], s_cls[2], s_cls[3], s_cls[4], s_cls[5], s_cls[6], s_cls[7],
                  s_cls[8], s_usFrame, s_usEmit);
        b[sizeof b - 1] = 0;
        tagpu_log(b);
    }
}
