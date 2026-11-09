/* tagpu_range.c -- the weapon range rings' look and draw list, on the render
   thread. Contract: tagpu_range.h. The rings come from the packet
   (TAGPU_PK_RANGE / TAGPU_PK_RING, filled by tagpu_rangeown.c on the game
   thread); this file reads nothing else.

   THE LOOK IS BAR'S, table for table: gui_attackrange_gl4.lua colorConfig
   (l.84-182) for the attack widget, gui_defenserange_gl4.lua (l.82-146) for
   the defense widget, and the alpha of weapon_range_rings_unified_gl4's vertex
   stage (l.312-437): a fade by camera distance, a highlight near the cursor,
   and the inner rings dimmed by how many rings are shown. tools/tascene-ranges.js
   is the browser reference this file was ported from and draws the same frame.

   THE CAMERA DISTANCE BAR's fades read is the distance at which its 45-degree
   camera shows as much ground as this view does: the view's height in world
   px over 2 tan(22.5 deg). */

#include <math.h>
#include <string.h>
#include "tagpu_range.h"
#include "tagpu_packet.h"
#include "tagpu_native.h"
#include "tagpu_log.h"
#include "tagpu_range_glsl.inc"

#define FOV_SPAN 0.82842712f       /* 2 tan(pi/8) */

typedef struct {
    float c[4];          /* colour; the alpha is BAR's colour alpha           */
    float fade[4];       /* start, end, start alpha, end alpha (elmos)        */
    float w[2];          /* outer, inner width, screen px                     */
    float a[2];          /* outer, inner alpha                                */
    float dim;           /* the inner rings' dimming, 0 = none                */
    int   bit;           /* the union's stencil bit, -1 = never merged        */
    int   segs;
} RSTYLE;

/* the attack widget: ground and cannon share one union, lrpc is never merged */
static const RSTYLE ATK[5] = {
    /* ground */ { { 1.0f, 0.22f, 0.05f, 0.60f }, { 4000, 6000, 1, 0 },   { 3, 1.8f }, { 0.75f, 0.12f }, 0.75f, 0, 128 },
    /* nano   */ { { 0.24f, 1.0f, 0.2f, 0.60f },  { 2000, 4000, 1, 0 },   { 3, 2.0f }, { 0.75f, 0.12f }, 0.05f, 1, 128 },
    /* AA     */ { { 0.8f, 0.44f, 2.0f, 0.40f },  { 2000, 5000, 1, 0 },   { 3, 1.8f }, { 0.75f, 0.12f }, 0.75f, 2, 1024 },
    /* cannon */ { { 1.0f, 0.22f, 0.05f, 0.60f }, { 5000, 8500, 1, 0 },   { 3, 1.8f }, { 0.75f, 0.12f }, 0.75f, 0, 128 },
    /* lrpc   */ { { 1.0f, 0.22f, 0.05f, 0.60f }, { 5000, 1000, 1, 0.5f },{ 2, 0 },    { 0.75f, 0.12f }, 0.75f, -1, 1024 },
};
static const float ATK_WATER[4] = { 0.32f, 0.48f, 1.0f, 0.30f };
static const float ATK_EMP[4]   = { 0.72f, 0.72f, 1.0f, 0.60f };
#define ATK_FILL       0.12f
#define ATK_FADE_OFF   2500.0f   /* the outer ring fades this much later      */

/* the defense widget: no fill, the cursor highlight five times as strong */
static const RSTYLE DEF[5] = {
    /* ground */ { { 1.3f, 0.18f, 0.04f, 0.70f },  { 2200, 5500, 1.0f, 0 }, { 5, 3.0f }, { 0.75f, 0.10f }, 0, 0, 128 },
    /* air    */ { { 0.8f, 0.44f, 1.6f, 0.70f },   { 3200, 8000, 0.4f, 0 }, { 5, 3.0f }, { 0.75f, 0.10f }, 0, 1, 128 },
    /* nuke   */ { { 1.05f, 1.0f, 0.2f, 0.72f },   { 6000, 3000, 0.6f, 0 }, { 5, 2.0f }, { 0.80f, 0.15f }, 0, 2, 1024 },
    /* cannon */ { { 1.3f, 0.18f, 0.04f, 0.70f },  { 2000, 8000, 0.8f, 0 }, { 5, 2.5f }, { 0.75f, 0.10f }, 0, 3, 128 },
    /* lrpc   */ { { 1.3f, 0.18f, 0.04f, 0.68f },  { 9000, 6000, 0.8f, 0 }, { 3, 1.5f }, { 0.25f, 0.0f },  0, -1, 1024 },
};
static const float DEF_WATER[4] = { 0.48f, 0.67f, 1.0f, 0.30f };

/* one ring as this frame draws it */
typedef struct {
    float x, z, alt;
    float range, v, minbarrel, ballistic, scale;
    const RSTYLE* st;
    const float* col;
    int   attack, dgun, nano;
    float aOuter, aInner;
} RITEM;

static RITEM        s_items[TAGPU_PK_MAX_RING];
static TAGPU_RGINST s_inst[TAGPU_RG_MAXINST];
static TAGPU_RGDRAW s_draw[TAGPU_RG_MAXDRAW];
static int          s_ninst, s_ndraw;
static TAGPU_RGHAND s_pub;
static int          s_have;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int style_index(int cls, int attack)
{
    if (attack) {
        switch (cls) {
        case TAGPU_PK_RC_NANO:   return 1;
        case TAGPU_PK_RC_AA:     return 2;
        case TAGPU_PK_RC_CANNON: return 3;
        case TAGPU_PK_RC_LRPC:   return 4;
        default:                 return 0;
        }
    }
    switch (cls) {
    case TAGPU_PK_RC_AIR:    return 1;
    case TAGPU_PK_RC_NUKE:   return 2;
    case TAGPU_PK_RC_CANNON: return 3;
    case TAGPU_PK_RC_LRPC:   return 4;
    default:                 return 0;
    }
}

/* BAR's vertex-stage alpha */
static float ring_alpha(const RITEM* it, float camDist, int outer, const TAGPU_PK_RANGE* h,
                        int selCount, int builderCount)
{
    const RSTYLE* st = it->st;
    float fs = st->fade[0], fe = st->fade[1], sa = st->fade[2], ea = st->fade[3];
    float off = (outer && it->attack) ? ATK_FADE_OFF : 0.0f;
    float lo = ea < sa ? ea : sa, hi = ea < sa ? sa : ea;
    float fade = (fe + off + (it->dgun ? 1000.0f : 0.0f) - camDist) / (fe - fs);
    float hl = 0.0f, a;
    fade = clampf(fade, lo, hi);
    if (h->flags & TAGPU_PK_RG_MOUSE) {
        float dx = it->x - (float)h->mouse[0], dz = it->z - (float)h->mouse[2];
        float t = clampf((sqrtf(dx * dx + dz * dz) - 48.0f) / 16.0f, 0.0f, 1.0f);
        hl = (1.0f - t * t * (3.0f - 2.0f * t)) * (it->attack ? 1.0f : 5.0f);
    }
    a = it->col[3] * (0.6f * hl + fade * st->a[outer ? 0 : 1]);
    if (it->dgun) a = clampf(a * 3.0f, 0.1f, 1.0f);
    if (!outer && st->dim > 0.0f) {
        int n = it->nano ? builderCount : selCount;
        float dim;
        if (n < 1) n = 1;
        if (n > 25) n = 25;
        dim = st->dim * 0.1f * (float)n;
        a *= fminf(1.0f, 1.0f / (dim * dim));
    }
    return a;
}

/* KINDS of instance one ring can give */
enum { IK_DISC = 0, IK_OUTER, IK_INNER };

static int put_inst(const RITEM* it, int kind, float devPx, float ss)
{
    TAGPU_RGINST* o;
    float wDev, wGame, halfT, a;
    if (s_ninst >= TAGPU_RG_MAXINST) return 0;
    o = &s_inst[s_ninst];
    o->c[0] = it->x; o->c[1] = it->z; o->c[2] = it->alt; o->c[3] = it->range;
    o->b[0] = it->v; o->b[1] = it->minbarrel; o->b[2] = it->ballistic; o->b[3] = it->scale;
    o->col[0] = fminf(it->col[0], 1.0f);
    o->col[1] = fminf(it->col[1], 1.0f);
    o->col[2] = fminf(it->col[2], 1.0f);
    if (kind == IK_DISC) {
        /* the fill: the attack widget's colour alpha x fill x inner alpha,
           nothing for the defense widget (its discs only mark the union) */
        o->col[3] = it->attack ? it->col[3] * ATK_FILL * it->st->a[1] : 0.0f;
        o->w[0] = 0.0f; o->w[2] = 0.0f;
    } else {
        a = kind == IK_OUTER ? it->aOuter : it->aInner;
        o->col[3] = a;
        /* BAR's widths are screen px: in game px through the composite's
           scale, plus half a target px of rim on each side */
        wDev = it->st->w[kind == IK_OUTER ? 0 : 1];
        wGame = wDev / devPx;
        halfT = wGame * ss * 0.5f + 0.5f;
        o->w[0] = halfT / ss;
        o->w[2] = wGame * 0.5f;
    }
    o->w[1] = (float)it->st->segs;
    o->w[3] = 0.0f;
    s_ninst++;
    return 1;
}

static void put_draw(int kind, int bit, int first, int nseg)
{
    TAGPU_RGDRAW* g;
    if (kind != TAGPU_RG_CLEAR && s_ninst <= first) return;
    if (s_ndraw >= TAGPU_RG_MAXDRAW) return;
    g = &s_draw[s_ndraw++];
    g->kind = kind; g->bit = bit; g->first = first;
    g->count = kind == TAGPU_RG_CLEAR ? 0 : s_ninst - first;
    g->nseg = nseg;
}

/* one widget's group, BAR's order: the union's discs (and the fill), the
   outer rings outside the union, the inner rings over everything */
static void draw_layer(const RITEM* const* L, int n, float devPx, float ss)
{
    static const int SEGS[2] = { 128, 1024 };
    int b, s, k, first, merged = 0;
    for (k = 0; k < n; k++) if (L[k]->st->bit >= 0) merged = 1;
    if (merged) {
        put_draw(TAGPU_RG_CLEAR, 0, 0, 0);
        for (b = 0; b < 4; b++)
            for (s = 0; s < 2; s++) {
                first = s_ninst;
                for (k = 0; k < n; k++)
                    if (L[k]->st->bit == b && L[k]->st->segs == SEGS[s]) put_inst(L[k], IK_DISC, devPx, ss);
                put_draw(TAGPU_RG_DISC, 1 << b, first, SEGS[s]);
            }
    }
    for (b = -1; b < 4; b++)
        for (s = 0; s < 2; s++) {
            first = s_ninst;
            for (k = 0; k < n; k++)
                if (L[k]->st->bit == b && L[k]->st->segs == SEGS[s] &&
                    L[k]->st->w[0] > 0.0f && L[k]->aOuter > 0.002f)
                    put_inst(L[k], IK_OUTER, devPx, ss);
            put_draw(b < 0 ? TAGPU_RG_LINE : TAGPU_RG_OUTER, b < 0 ? 0 : 1 << b, first, SEGS[s]);
        }
    for (s = 0; s < 2; s++) {
        first = s_ninst;
        for (k = 0; k < n; k++)
            if (L[k]->st->segs == SEGS[s] && L[k]->st->w[1] > 0.0f && L[k]->aInner > 0.002f)
                put_inst(L[k], IK_INNER, devPx, ss);
        put_draw(TAGPU_RG_LINE, 0, first, SEGS[s]);
    }
}

void tagpu_range_render(const TAGPU_FXVIEW* v)
{
    static const RITEM* layer[TAGPU_PK_RL_LAST + 1][TAGPU_PK_MAX_RING];
    int nl[TAGPU_PK_RL_LAST + 1];
    const TAGPU_PACKET* pk = v->packet;
    const TAGPU_PK_RANGE* h = pk ? tagpu_pk_range(pk) : NULL;
    const TAGPU_PK_RING* r = h ? tagpu_pk_rings(pk) : NULL;
    const TAGPU_PK_UNIT* units = pk ? tagpu_pk_units(pk) : NULL;
    float zoom = v->zoom > 0.0f ? v->zoom : 1.0f;
    float ss = v->ss > 0 ? (float)v->ss : 1.0f;
    float devPx = v->devPx >= 0.25f && v->devPx <= 16.0f ? v->devPx : 1.0f;
    float camDist = ((float)v->vh / zoom) / FOV_SPAN;
    unsigned k, n;
    int L;

    s_have = 0;
    s_ninst = 0; s_ndraw = 0;
    if (!h || !r || !pk->in_game) return;
    n = pk->n_ring;
    for (L = 0; L <= TAGPU_PK_RL_LAST; L++) nl[L] = 0;
    for (k = 0; k < n; k++) {
        const TAGPU_PK_RING* e = &r[k];
        RITEM* it = &s_items[k];
        int attack = e->layer >= TAGPU_PK_RL_ATK_ALLY;
        float ux, uy, uz;
        it->x = (float)e->pos[0]; it->alt = (float)e->pos[1]; it->z = (float)e->pos[2];
        /* the interpolated position, when the unit the record names is still
           the one in that packet slot: `uidx` is bounded by the table's own
           count before it indexes it */
        if (units && e->uidx < pk->n_units && units[e->uidx].slot == e->slot &&
            tagpu_native_unit_pos(&units[e->uidx], &ux, &uy, &uz)) {
            it->x = ux; it->alt = uy; it->z = uz;
        }
        it->range = (float)e->range;
        it->ballistic = (e->flags & TAGPU_PK_RF_BALLISTIC) ? 1.0f : 0.0f;
        it->v = it->ballistic > 0.5f ? e->v : 0.0f;
        it->minbarrel = it->ballistic > 0.5f ? e->minbarrel : 0.0f;
        it->attack = attack;
        it->st = attack ? &ATK[style_index(e->cls, 1)] : &DEF[style_index(e->cls, 0)];
        it->col = it->st->c;
        it->dgun = attack && e->style == TAGPU_PK_RS_DGUN;
        it->nano = attack && e->cls == TAGPU_PK_RC_NANO;
        if (attack && e->style == TAGPU_PK_RS_WATER) it->col = ATK_WATER;
        if (attack && e->style == TAGPU_PK_RS_EMP)   it->col = ATK_EMP;
        if (!attack && e->style == TAGPU_PK_RS_WATER) it->col = DEF_WATER;
        it->scale = it->dgun ? 1.05f : 1.0f;
        it->aOuter = ring_alpha(it, camDist, 1, h, h->nsel, h->nbuild);
        it->aInner = ring_alpha(it, camDist, 0, h, h->nsel, h->nbuild);
        layer[e->layer][nl[e->layer]++] = it;
    }
    for (L = 0; L <= TAGPU_PK_RL_LAST; L++)
        if (nl[L]) draw_layer(layer[L], nl[L], devPx, ss);
    /* the heartbeat: what arrived per layer and what it became */
    if ((v->frame_counter % 300u) == 0u)
        tagpu_logf("range: rings=%u def=%d/%d atk=%d/%d sel=%u build=%u inst=%d draws=%d "
                   "camDist=%.0f%s", n, nl[0], nl[1], nl[2], nl[3], (unsigned)h->nsel,
                   (unsigned)h->nbuild, s_ninst, s_ndraw, camDist,
                   (pk->truncated & TAGPU_PK_TRUNC_RING) ? " CUT" : "");
    if (!s_ndraw) return;

    memset(&s_pub, 0, sizeof s_pub);
    s_pub.inst = s_inst; s_pub.ninst = s_ninst;
    s_pub.draws = s_draw; s_pub.ndraw = s_ndraw;
    s_pub.frame = v->frame_counter;
    s_pub.gw = (float)v->gw; s_pub.gh = (float)v->gh; s_pub.ss = ss;
    s_pub.zoom = zoom; s_pub.zoomCx = v->zoomCx; s_pub.zoomCy = v->zoomCy;
    s_pub.eyeX = (float)(v->eyeX - v->vpL); s_pub.eyeY = (float)(v->eyeY - v->vpT);
    s_pub.vpL = v->vpL; s_pub.vpT = v->vpT; s_pub.vw = v->vw; s_pub.vh = v->vh;
    s_pub.mapW = (float)pk->map_pxw; s_pub.mapH = (float)pk->map_pxh;
    s_pub.sea = (float)pk->sea_level;
    s_pub.grav = h->grav;
    s_have = 1;
}

int tagpu_range_handover(TAGPU_RGHAND* out, unsigned frame)
{
    if (!s_have || !out || s_pub.frame != frame) return 0;
    *out = s_pub;
    s_have = 0;
    return 1;
}
