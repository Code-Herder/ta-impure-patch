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

   ONE DEPARTURE: the attack widget takes no camera fade. TA's zoom reaches
   distances BAR's camera does not -- the floor shows the whole map, and a 4K
   view at zoom 1 already shows twice the ground of a 1080-high one -- so on
   a 4K view BAR's fades hid a selected unit's rings below zoom 0.3 and its
   inner rings below 0.42.
   The defense widget keeps them: it shows every defense of a class at once.

   THE CAMERA DISTANCE BAR's fades read is the distance at which its 45-degree
   camera shows as much ground as this view does: the view's height in world
   px over 2 tan(22.5 deg). */

#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_range.h"
#include "tagpu_packet.h"
#include "tagpu_native.h"
#include "tagpu_log.h"
#include "tagpu_terr.h"         /* tagpu_terr_heights: the ground the sensors lie on */
#include "tagpu_range_glsl.inc"

#define FOV_SPAN 0.82842712f       /* 2 tan(pi/8) */

typedef struct {
    float c[4];          /* colour; the alpha is BAR's colour alpha           */
    float fade[4];       /* start, end, start alpha, end alpha (elmos); the
                            defense widget's only                             */
    float w[2];          /* outer, inner width, screen px                     */
    float a[2];          /* outer, inner alpha                                */
    float dim;           /* the inner rings' dimming, 0 = none                */
    int   bit;           /* the union's stencil bit, -1 = never merged        */
    int   segs;
} RSTYLE;

/* the attack widget: ground and cannon share one union, lrpc is never merged;
   no camera fade (the header) */
static const RSTYLE ATK[5] = {
    /* ground */ { { 1.0f, 0.22f, 0.05f, 0.60f }, { 0 }, { 3, 1.8f }, { 0.75f, 0.12f }, 0.75f, 0, 128 },
    /* nano   */ { { 0.24f, 1.0f, 0.2f, 0.60f },  { 0 }, { 3, 2.0f }, { 0.75f, 0.12f }, 0.05f, 1, 128 },
    /* AA     */ { { 0.8f, 0.44f, 2.0f, 0.40f },  { 0 }, { 3, 1.8f }, { 0.75f, 0.12f }, 0.75f, 2, 1024 },
    /* cannon */ { { 1.0f, 0.22f, 0.05f, 0.60f }, { 0 }, { 3, 1.8f }, { 0.75f, 0.12f }, 0.75f, 0, 128 },
    /* lrpc   */ { { 1.0f, 0.22f, 0.05f, 0.60f }, { 0 }, { 2, 0 },    { 0.75f, 0.12f }, 0.75f, -1, 1024 },
};
static const float ATK_WATER[4] = { 0.32f, 0.48f, 1.0f, 0.30f };
static const float ATK_EMP[4]   = { 0.72f, 0.72f, 1.0f, 0.60f };
#define ATK_FILL       0.12f

/* the defense widget: no fill, the cursor highlight five times as strong */
static const RSTYLE DEF[5] = {
    /* ground */ { { 1.3f, 0.18f, 0.04f, 0.70f },  { 2200, 5500, 1.0f, 0 }, { 5, 3.0f }, { 0.75f, 0.10f }, 0, 0, 128 },
    /* air    */ { { 0.8f, 0.44f, 1.6f, 0.70f },   { 3200, 8000, 0.4f, 0 }, { 5, 3.0f }, { 0.75f, 0.10f }, 0, 1, 128 },
    /* nuke   */ { { 1.05f, 1.0f, 0.2f, 0.72f },   { 6000, 3000, 0.6f, 0 }, { 5, 2.0f }, { 0.80f, 0.15f }, 0, 2, 1024 },
    /* cannon */ { { 1.3f, 0.18f, 0.04f, 0.70f },  { 2000, 8000, 0.8f, 0 }, { 5, 2.5f }, { 0.75f, 0.10f }, 0, 3, 128 },
    /* lrpc   */ { { 1.3f, 0.18f, 0.04f, 0.68f },  { 9000, 6000, 0.8f, 0 }, { 3, 1.5f }, { 0.25f, 0.0f },  0, -1, 1024 },
};
static const float DEF_WATER[4] = { 0.48f, 0.67f, 1.0f, 0.30f };

/* THE SENSORS, BAR's sensor widgets on the engine's coverage rule, as
   tools/tascene-ranges.js draws them (its DASH, SENSOR_LINE and JAMMER
   tables are these):
   - THE VIEW, under `sensorview`: radar and sonar as
     gui_sensor_ranges_radar.lua / _sonar.lua's colours on a dashed band just
     outside each kind's union; the jammers as
     gui_sensor_ranges_jammer_preview.lua's always-shown layer, a dashed band
     just inside the union and the union inside the band filled;
   - THE PREVIEW of a building being placed is not BAR's: the building's disc
     joins its kinds' unions in the view's look -- whether the view is on or
     not -- and the arcs it adds to the outline draw brighter (`NEW_GAIN`),
     so the outline shows the coverage the building would give.
   Every width, dash and speed is BAR's px of a screen scaled by its
   lineScale, (screen height + 500) / 1300, so they hold on screen at every
   zoom; the device's px reach the game frame through `devPx`. */
enum { SK_RADAR = 0, SK_SONAR, SK_RADARJAM, SK_SONARJAM, SK_N };
static const int SK_WATER[SK_N] = { 0, 1, 0, 1 };   /* sonar finds targets at or below the sea (0x4678E6) */

/* sensor_ranges_los's colours, the alpha times the dash's gain of 2.5 */
static const float VIEW_COL[2][4] = { { 0.0f, 1.0f, 0.0f, 0.40f }, { 0.5f, 0.7f, 0.9f, 0.425f } };
/* THE DASHES are the route dots' level of detail (tagpu_order.c) on a path:
   every dash is `len` long on screen at every zoom; where they stand is
   pinned to the map, on a lattice of levels -- level j a dash every s0 x 2^j
   along the path, each level every other dash of the one below. The zoom
   picks the level (its octave, held DASH_HYST past a boundary), so the
   spacing on screen stays within about a factor of two; a level change fades
   the dashes it drops or adds over DASH_FADE_MS and moves none. s0 divides
   the path's length evenly, so every level closes round a loop. The dashes
   move along at `speed` px a second on screen, `sign` the way round. */
typedef struct { float w, len, space, speed, sign; } DASHROW;
enum { DR_LINE = 0, DR_JAM };
static const DASHROW DASH[2] = {
    /* line */ { 1.5f, 4.0f, 11.0f, 9.0f, -1.0f },
    /* jam  */ { 0.0f, 5.0f, 11.0f, 6.0f,  1.0f },
};
#define DASH_SPAN     8            /* levels kept either side of the zoom's    */
#define DASH_HYST     0.65
#define DASH_FADE_MS  300.0
/* gui_sensor_ranges_jammer_preview.lua shaderConfig; the band is `JAM_W` px
   of a 1080-high screen plus `JAM_W_ELMO` world px */
static const float JAM_ALLIED[3] = { 0.62f, 0.35f, 0.33f };
static const float JAM_OUTLINE[4] = { 1.0f, 0.32f, 0.22f, 0.8f };
#define JAM_A          0.17f
#define JAM_ALLIED_REL 0.85f
#define JAM_W          1.2f
#define JAM_W_ELMO     0.89f
/* the arcs a building being placed adds to an outline: its band's alpha
   times this, at most 1 (radar 0.40 and sonar 0.425 reach 1.0) */
#define NEW_GAIN       2.5f
/* a jammer disc: the finer of the two counts tagpu_vk_range.c carries, for a
   coverage circle of up to 2880 px (ARMARAD's radar, the largest sensor) */
#define SENSOR_SEGS 1024
typedef char range_lodn_is_the_span[(DASH_SPAN + 1 == TAGPU_RG_LODN) ? 1 : -1];

/* one ring as this frame draws it */
typedef struct {
    float x, z, alt;
    float range, v, minbarrel, ballistic, scale;
    const RSTYLE* st;
    const float* col;
    int   attack, dgun, nano;
    int   sensor, isNew;  /* sensor: 0..3, the kind (radar, sonar, radarjam,
                             sonarjam); -1 for a weapon ring              */
    float aOuter, aInner;
} RITEM;

static RITEM        s_items[TAGPU_PK_MAX_RING];
static TAGPU_RGINST s_inst[TAGPU_RG_MAXINST];
static TAGPU_RGDRAW s_draw[TAGPU_RG_MAXDRAW];
static int          s_ninst, s_ndraw;
static TAGPU_RGHAND s_pub;
static int          s_have;
static int          s_npathDrawn;      /* this frame's paths, for the heartbeat */
static int          s_sensorLast;      /* the last frame had sensor rings      */
static int          s_saidNoGround, s_saidPathCut;

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

/* BAR's camera fade, the defense widget's */
static float camera_fade(const RSTYLE* st, float camDist)
{
    float fs = st->fade[0], fe = st->fade[1], sa = st->fade[2], ea = st->fade[3];
    float lo = ea < sa ? ea : sa, hi = ea < sa ? sa : ea;
    return clampf((fe - camDist) / (fe - fs), lo, hi);
}

/* BAR's vertex-stage alpha */
static float ring_alpha(const RITEM* it, float camDist, int outer, const TAGPU_PK_RANGE* h,
                        int selCount, int builderCount)
{
    const RSTYLE* st = it->st;
    float fade = it->attack ? 1.0f : camera_fade(st, camDist);
    float hl = 0.0f, a;
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

/* ---- THE SENSOR OUTLINES --------------------------------------------------
   One kind's union, as paths: the circles' arcs that no other disc of the
   kind covers, chained at the points where the circles cross into closed
   loops, so a loop's dashes run on through a junction as along any curve.
   Flat circles on x/z -- the coverage rule -- each point then laid on the
   ground. A point of a circle at angle a is (x + r sin a, z + r cos a), and
   every arc runs the same way round, so where one ends the next one starts.
   tools/tascene-ranges.js's `openArcs`, `loops` and `makePath` are the
   reference. */

#define TAU      6.283185307179586
#define SC_MAX   512           /* circles one outline takes                  */
#define ARC_MAX  4096          /* arcs one outline takes                     */
#define SPATH_MAX 256           /* paths a frame                              */

typedef struct { double x, z, r; } SCIRC;
typedef struct { int i; double a0, a1; int whole; } SARC;
/* a path: its points in s_pts, and what draws it */
enum { PS_VIEW = 0, PS_JAM };
typedef struct { int first, nseg; float len; unsigned char set, kind; } SPATH;

/* the ground the points are laid on: the height grid, one byte a 16-px
   corner, bilinear as GetPosHeight 0x485070 blends (the reach pass's
   `taGround`), and the nearest corner past the grid's last row and column */
typedef struct { const unsigned char* g; int w, h; unsigned serial; float sea, mapW, mapH; } GROUND;

static float ground_at(const GROUND* G, double x, double z)
{
    double fx = x / 16.0, fz = z / 16.0, tx, tz;
    int c = (int)floor(fx), r = (int)floor(fz);
    const unsigned char* p;
    if (c < 0 || r < 0 || c + 1 >= G->w || r + 1 >= G->h) {
        if (c < 0) c = 0;
        if (r < 0) r = 0;
        if (c > G->w - 1) c = G->w - 1;
        if (r > G->h - 1) r = G->h - 1;
        return (float)G->g[(size_t)r * G->w + c];
    }
    tx = fx - c; tz = fz - r;
    p = G->g + (size_t)r * G->w + c;
    return (float)((p[0] * (1.0 - tx) + p[1] * tx) * (1.0 - tz) +
                   (p[G->w] * (1.0 - tx) + p[G->w + 1] * tx) * tz);
}

/* where a sensor's point is drawn: the water surface over water */
static float surface_at(const GROUND* G, double x, double z)
{
    float g = ground_at(G, x, z);
    return g > G->sea ? g : G->sea;
}

static int segs_of(double r) { return r > 1500.0 ? 1024 : 512; }

static double s_cov[2 * SC_MAX][2];

static int cov_cmp(const void* a, const void* b)
{
    double x = ((const double*)a)[0], y = ((const double*)b)[0];
    return x < y ? -1 : x > y;
}

/* The arcs of circle i no other disc covers, a1 > a0, into `out` (at most
   `max`): their count, or -1 when they do not fit. */
static int open_arcs(const SCIRC* c, int n, int i, SARC* out, int max)
{
    int j, nc = 0, m, k, no = 0;
    for (j = 0; j < n; j++) {
        double dx, dz, d, th, al, lo, hi, q;
        if (j == i) continue;
        dx = c[j].x - c[i].x; dz = c[j].z - c[i].z;
        d = sqrt(dx * dx + dz * dz);
        if (d >= c[i].r + c[j].r) continue;
        if (d + c[i].r <= c[j].r) {
            /* inside j; of two equal ones, the earlier is the one kept */
            if (d < 1e-6 && c[i].r == c[j].r && j > i) continue;
            return 0;
        }
        if (d + c[j].r <= c[i].r) continue;          /* j inside this one */
        th = atan2(dx, dz);
        q = (c[i].r * c[i].r + d * d - c[j].r * c[j].r) / (2.0 * c[i].r * d);
        al = acos(q < -1.0 ? -1.0 : q > 1.0 ? 1.0 : q);
        lo = fmod(fmod(th - al, TAU) + TAU, TAU);
        hi = lo + 2.0 * al;
        if (hi > TAU) {
            s_cov[nc][0] = lo; s_cov[nc][1] = TAU; nc++;
            s_cov[nc][0] = 0.0; s_cov[nc][1] = hi - TAU; nc++;
        } else {
            s_cov[nc][0] = lo; s_cov[nc][1] = hi; nc++;
        }
    }
    if (!nc) {
        if (max < 1) return -1;
        out[0].i = i; out[0].a0 = 0.0; out[0].a1 = TAU; out[0].whole = 1;
        return 1;
    }
    qsort(s_cov, (size_t)nc, sizeof s_cov[0], cov_cmp);
    m = 0;
    for (k = 1; k < nc; k++) {
        if (s_cov[k][0] <= s_cov[m][1]) {
            if (s_cov[k][1] > s_cov[m][1]) s_cov[m][1] = s_cov[k][1];
        } else {
            m++;
            s_cov[m][0] = s_cov[k][0]; s_cov[m][1] = s_cov[k][1];
        }
    }
    m++;
    if (m == 1 && s_cov[0][0] <= 0.0 && s_cov[0][1] >= TAU) return 0;
    for (k = 0; k < m; k++) {
        double a0 = s_cov[k][1], a1 = k + 1 < m ? s_cov[k + 1][0] : s_cov[0][0] + TAU;
        if (a1 - a0 <= 1e-6) continue;
        if (no >= max) return -1;
        out[no].i = i; out[no].a0 = a0; out[no].a1 = a1; out[no].whole = 0;
        no++;
    }
    return no;
}

static void arc_point(const SCIRC* c, double a, double* x, double* z)
{
    *x = c->x + sin(a) * c->r;
    *z = c->z + cos(a) * c->r;
}

/* THIS FRAME'S PATHS. Rebuilt whole when anything they are made of moves
   (`s_key`), so a frame whose sensors stand still re-uses them and the pass
   uploads nothing (`ptSerial`). */
static TAGPU_RGPT s_pts[TAGPU_RG_MAXPT];
static int        s_npts;
static SPATH      s_paths[SPATH_MAX];
static int        s_npaths;
static unsigned   s_ptSerial;
static int        s_pathCut;           /* a path or point would not fit     */
/* the scratch a path is made in */
static double     s_px[TAGPU_RG_MAXPT], s_pz[TAGPU_RG_MAXPT];
static float      s_sx[TAGPU_RG_MAXPT], s_sy[TAGPU_RG_MAXPT];
static int        s_owner[TAGPU_RG_MAXPT];

/* A chain of arcs as a path into s_pts: each point's place on the ground,
   the distance along the path on the screen at zoom 1 (game px: x, and z
   less half the altitude -- the projection's own lift, so a slope facing the
   camera stretches the dashes exactly as it stretches the ground), the
   band's direction there: the mitre of the two segments, away from the
   point's own circle, scaled to keep the band's width -- and whether the
   segment that starts there lies on circle 0 when `newFirst` says circle 0
   is a building being placed. A segment lies on the arc of the point it
   ends at: an arc after the first starts at the previous one's last point,
   and a closed path's last segment ends at the point dropped as the first
   again, still in s_owner. */
static void make_path(const GROUND* G, const SCIRC* c, const SARC* chain, int nch, int closed,
                      int water, int set, int kind, int newFirst)
{
    int k, t, n = 0, segs, first = s_npts, room = TAGPU_RG_MAXPT - s_npts;
    double along = 0.0;
    SPATH* p;
    if (s_npaths >= SPATH_MAX) { s_pathCut = 1; return; }
    for (k = 0; k < nch; k++) {
        const SCIRC* ci = &c[chain[k].i];
        double span = chain[k].a1 - chain[k].a0;
        int m = (int)ceil(span / TAU * segs_of(ci->r));
        if (m < 2) m = 2;
        for (t = k ? 1 : 0; t <= m; t++) {
            if (n >= room - 1) { s_pathCut = 1; return; }
            arc_point(ci, chain[k].a0 + span * t / m, &s_px[n], &s_pz[n]);
            s_owner[n] = chain[k].i;
            n++;
        }
    }
    if (closed) n--;                   /* the last point is the first again */
    if (n < 2) return;
    segs = closed ? n : n - 1;
    for (k = 0; k < n; k++) {
        TAGPU_RGPT* q = &s_pts[first + k];
        double x = s_px[k], z = s_pz[k];
        float alt = surface_at(G, x, z);
        int inMap = x >= 0.0 && z >= 0.0 && x <= G->mapW && z <= G->mapH;
        s_sx[k] = (float)x; s_sy[k] = (float)(z - alt * 0.5);
        q->p[0] = (float)x; q->p[1] = (float)z; q->p[2] = alt;
        q->n[2] = inMap && !(water && ground_at(G, x, z) > G->sea) ? 1.0f : 0.0f;
        q->n[3] = newFirst && k < segs && s_owner[k + 1] == 0 ? 1.0f : 0.0f;
    }
    s_pts[first].p[3] = 0.0f;
    for (k = 0; k < segs; k++) {
        int j = (k + 1) % n;
        along += hypot((double)s_sx[j] - s_sx[k], (double)s_sy[j] - s_sy[k]);
        if (k + 1 < n) s_pts[first + k + 1].p[3] = (float)along;
    }
    for (k = 0; k < n; k++) {
        int pi = closed ? (k + n - 1) % n : (k > 0 ? k - 1 : 0);
        int qi = closed ? (k + 1) % n : (k + 1 < n ? k + 1 : n - 1);
        int a0 = pi == k ? k : pi, b0 = pi == k ? qi : k;
        int a1 = qi == k ? pi : k, b1 = qi == k ? k : qi;
        double d1x = s_sx[b0] - s_sx[a0], d1y = s_sy[b0] - s_sy[a0];
        double d2x = s_sx[b1] - s_sx[a1], d2y = s_sy[b1] - s_sy[a1];
        double l1 = hypot(d1x, d1y), l2 = hypot(d2x, d2y), tx, ty, tl, nx, ny, cs, sc;
        const SCIRC* o = &c[s_owner[k]];
        if (l1 < 1e-9) l1 = 1.0;
        if (l2 < 1e-9) l2 = 1.0;
        d1x /= l1; d1y /= l1; d2x /= l2; d2y /= l2;
        tx = d1x + d2x; ty = d1y + d2y;
        tl = hypot(tx, ty);
        if (tl < 1e-9) tl = 1.0;
        tx /= tl; ty /= tl;
        nx = -ty; ny = tx;
        cs = o->z - surface_at(G, o->x, o->z) * 0.5;
        if (nx * (s_sx[k] - o->x) + ny * (s_sy[k] - cs) < 0.0) { nx = -nx; ny = -ny; }
        sc = fabs(-d1y * nx + d1x * ny);
        sc = 1.0 / (sc > 0.35 ? sc : 0.35);
        s_pts[first + k].n[0] = (float)(nx * sc);
        s_pts[first + k].n[1] = (float)(ny * sc);
    }
    if (closed) {
        s_pts[first + n] = s_pts[first];
        s_pts[first + n].p[3] = (float)along;
    }
    p = &s_paths[s_npaths++];
    p->first = first;
    p->nseg = segs;
    p->len = (float)along;
    p->set = (unsigned char)set;
    p->kind = (unsigned char)kind;
    s_npts = first + (closed ? n + 1 : n);
}

static SARC          s_arcs[ARC_MAX];
static SARC          s_chain[ARC_MAX];
static unsigned char s_used[ARC_MAX];
static double        s_arcStart[ARC_MAX][2];

/* One kind's union as closed loops of arcs, each a path; circle 0's arcs
   marked new when `newFirst`. */
static void outline(const GROUND* G, const SCIRC* c, int n, int water, int newFirst, int set,
                    int kind)
{
    int i, na = 0, s;
    for (i = 0; i < n; i++) {
        int got = open_arcs(c, n, i, s_arcs + na, ARC_MAX - na);
        if (got < 0) { s_pathCut = 1; return; }
        na += got;
    }
    memset(s_used, 0, (size_t)na);
    /* each arc's start, once: the chaining compares every arc's against
       every end, so computing it there costs na^2 sines */
    for (s = 0; s < na; s++) arc_point(&c[s_arcs[s].i], s_arcs[s].a0, &s_arcStart[s][0], &s_arcStart[s][1]);
    for (s = 0; s < na; s++) {
        int k = s, nch = 0;
        if (s_used[s]) continue;
        while (k >= 0 && !s_used[k]) {
            double ex, ez, bd = 1e30;
            int q, best = -1;
            s_used[k] = 1;
            s_chain[nch++] = s_arcs[k];
            if (s_arcs[k].whole) break;
            /* the next arc: the one of another circle starting where this one ends */
            arc_point(&c[s_arcs[k].i], s_arcs[k].a1, &ex, &ez);
            for (q = 0; q < na; q++) {
                double dd;
                if (s_arcs[q].i == s_arcs[k].i || s_arcs[q].whole) continue;
                dd = hypot(s_arcStart[q][0] - ex, s_arcStart[q][1] - ez);
                if (dd < bd) { bd = dd; best = q; }
            }
            k = bd < 1.0 ? best : -1;
        }
        make_path(G, c, s_chain, nch, s_chain[0].whole || k == s, water, set, kind, newFirst);
    }
}

/* the circles one outline is made of: the kind's records, the placed
   building's first when there is one -- `*newFirst` says so; 0 past SC_MAX */
static int circles(const RITEM* const* L, int n, int kind, int* newFirst, SCIRC* out)
{
    int k, m = 0, pass;
    *newFirst = 0;
    for (pass = 0; pass < 2; pass++)
        for (k = 0; k < n; k++) {
            const RITEM* it = L[k];
            if (it->sensor != kind || it->isNew != (pass == 0)) continue;
            /* a union of only some of the circles would draw edges inside
               the coverage: past the cap the kind is not drawn */
            if (m >= SC_MAX) { s_pathCut = 1; return 0; }
            if (it->isNew && m == 0) *newFirst = 1;
            out[m].x = it->x; out[m].z = it->z; out[m].r = it->range;
            m++;
        }
    return m;
}

/* the inputs every path is made of, for the frame's comparison, bit for bit */
static float  s_key[2][8 + 5 * TAGPU_PK_MAX_RING];
static int    s_keyN[2];
static int    s_keyCur;

/* a kind is drawn under the view, and while a building that adds it is
   placed */
static int kind_shown(int kind, int view, int newMask)
{
    return view || (newMask & (1 << kind));
}

static void build_paths(const GROUND* G, const RITEM* const* L, int n, int view, int newMask)
{
    static SCIRC c[SC_MAX];
    int kind, m, newFirst;
    s_npts = 0; s_npaths = 0; s_pathCut = 0;
    for (kind = 0; kind < SK_N; kind++)
        if (kind_shown(kind, view, newMask) && (m = circles(L, n, kind, &newFirst, c)) > 0)
            outline(G, c, m, SK_WATER[kind], newFirst,
                    kind >= SK_RADARJAM ? PS_JAM : PS_VIEW, kind);
    s_ptSerial++;
}

/* ---- the dashes' level and motion, per frame ---------------------------- */
static int      s_lodHave, s_lodK;
static float    s_lodOp[2 * DASH_SPAN + 1];
static LONGLONG s_lodQpc;
static double   s_shift[2];

static float lod_op(int j)
{
    if (j >= s_lodK - DASH_SPAN && j <= s_lodK + DASH_SPAN) return s_lodOp[j - s_lodK + DASH_SPAN];
    return j >= s_lodK ? 1.0f : 0.0f;
}

/* THIS FRAME'S LEVEL AND MOTION. The level is the zoom's octave, held until
   the zoom is DASH_HYST past it; each level's opacity walks to 1 at the level
   and coarser and 0 finer at 1 / DASH_FADE_MS a millisecond. The dashes move
   their speed on screen, so speed / zoom on the map. THE TWO CLOCKS ARE
   BAR'S: the radar and sonar lines advance with the game (`dTicks`, a tick a
   thirtieth of a second at normal speed), so they stop on pause and run
   faster at a higher speed; the jammers' dashes with the wall clock. A step back in
   time, or of more than a second (a load, a stall, a seek), moves nothing
   and ends a fade at once. */
static void dash_step(double zoom, double ls, double devPx, double dTicks)
{
    float old[2 * DASH_SPAN + 1];
    LARGE_INTEGER now, hz;
    double lv, dt, rate;
    int j, oldK, row;
    if (zoom < 1.0 / 64.0) zoom = 1.0 / 64.0;
    if (zoom > 64.0) zoom = 64.0;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&hz);
    lv = -log2(zoom);
    if (!s_lodHave) {
        s_lodK = (int)floor(lv + 0.5);
        for (j = 0; j <= 2 * DASH_SPAN; j++) s_lodOp[j] = j >= DASH_SPAN ? 1.0f : 0.0f;
        s_lodQpc = now.QuadPart;
        s_lodHave = 1;
        return;
    }
    dt = hz.QuadPart > 0 ? (double)(now.QuadPart - s_lodQpc) / (double)hz.QuadPart : 0.0;
    s_lodQpc = now.QuadPart;
    rate = dt * 1000.0 / DASH_FADE_MS;
    if (dt < 0.0 || dt > 1.0) { dt = 0.0; rate = 1.0; }
    oldK = s_lodK;
    while (lv > s_lodK + DASH_HYST) s_lodK++;
    while (lv < s_lodK - DASH_HYST) s_lodK--;
    memcpy(old, s_lodOp, sizeof old);
    for (j = 0; j <= 2 * DASH_SPAN; j++) {
        int oj = j + s_lodK - oldK;
        float want = j >= DASH_SPAN ? 1.0f : 0.0f;
        float cur = (oj >= 0 && oj <= 2 * DASH_SPAN) ? old[oj] : want;
        s_lodOp[j] = want > cur ? (float)fmin(want, cur + rate) : (float)fmax(want, cur - rate);
    }
    if (!(dTicks >= 0.0 && dTicks <= 30.0)) dTicks = 0.0;
    for (row = 0; row < 2; row++)
        s_shift[row] += (row == DR_LINE ? dTicks / 30.0 : dt) * DASH[row].speed * ls / devPx / zoom;
}

/* ---- a radar or sonar going on and off ----------------------------------
   BAR's widgets grow a radar's circle from nothing over 15 game frames when
   it switches on, and shrink it away over 15 when it switches off; a unit
   that is gone is dropped at once. The scale is kept per unit slot and kind,
   on the game's clock -- the packet's tick and the fraction of the next one
   since the publisher first saw it, a visual weight that never reaches an
   index -- so a paused game holds it. A slot not drawn on the frame before
   starts over: switched on it grows, unless the whole layer is starting --
   the first frame, a new level, another viewed player (a replay's
   perspective), a jump in the game's time (a load, a seek), or the frame
   after one with no sensors -- when it is at its end at once. */
#define ANIM_TICKS 15.0
typedef struct { double t0; float s0; unsigned seen; unsigned char on; } SANIM;
static SANIM    s_anim[TAGPU_PK_DESIGN_SLOTS][2];
static unsigned s_animFrame;            /* counts the frames with ring records, from 1 */
static unsigned s_animLevel;
static unsigned s_animViewer;
static double   s_animT;

static double game_ticks(const TAGPU_PACKET* pk)
{
    LARGE_INTEGER now, hz;
    LONGLONG t0 = ((LONGLONG)(unsigned)pk->tick_start_hi << 32) | (LONGLONG)(unsigned)pk->tick_start_lo;
    double frac = 0.0;
    if (t0 && pk->game_speed > 0) {
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&hz);
        if (hz.QuadPart > 0)
            frac = (double)(now.QuadPart - t0) / (double)hz.QuadPart * 3.0 * pk->game_speed;
        if (!(frac >= 0.0)) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
    }
    return (double)pk->tick + frac;
}

static float anim_at(const SANIM* a, double t)
{
    double s = a->on ? a->s0 + (t - a->t0) / ANIM_TICKS : a->s0 - (t - a->t0) / ANIM_TICKS;
    return (float)(s < 0.0 ? 0.0 : s > 1.0 ? 1.0 : s);
}

/* this frame's scale of slot `slot`'s kind `kind` (0 radar, 1 sonar) */
static float anim_scale(unsigned slot, int kind, int on, double t, int fresh)
{
    SANIM* a;
    if (slot == 0 || slot >= TAGPU_PK_DESIGN_SLOTS || kind < 0 || kind > 1) return on ? 1.0f : 0.0f;
    a = &s_anim[slot][kind];
    if (fresh || (a->seen != s_animFrame && a->seen + 1 != s_animFrame)) {
        a->on = (unsigned char)on; a->t0 = t;
        a->s0 = fresh && on ? 1.0f : 0.0f;
    } else if (a->on != on) {
        a->s0 = anim_at(a, t); a->t0 = t; a->on = (unsigned char)on;
    }
    a->seen = s_animFrame;
    return anim_at(a, t);
}

/* a jammer disc: its fill; the kind tagpu_range_glsl.inc reads is 2, + 1
   water only */
static int put_sdisc(const RITEM* it, float r, float alt, const float rgb[3], float a)
{
    TAGPU_RGINST* o;
    if (s_ninst >= TAGPU_RG_MAXINST) return 0;
    o = &s_inst[s_ninst++];
    o->c[0] = it->x; o->c[1] = it->z; o->c[2] = alt; o->c[3] = r;
    o->b[0] = 0.0f; o->b[1] = 0.0f;
    o->b[2] = 2.0f + (SK_WATER[it->sensor] ? 1.0f : 0.0f);
    o->b[3] = 1.0f;
    o->col[0] = rgb[0]; o->col[1] = rgb[1]; o->col[2] = rgb[2]; o->col[3] = a;
    o->w[0] = 0.0f; o->w[1] = (float)SENSOR_SEGS; o->w[2] = 0.0f; o->w[3] = 0.0f;
    return 1;
}

/* a path's band: `side` 1 outside its circles, -1 inside; dashed on `row`'s
   lattice -- its coarsest level puts 4..8 dashes along it, and s0 divides its
   length evenly, so the pattern closes round a loop. The segments a building
   being placed adds (their start point's n[3]) take `col`'s alpha times
   NEW_GAIN, at most 1. */
static void put_spath(const SPATH* p, float side, float width, int row, const float col[4],
                      float space, float dashLen)
{
    TAGPU_RGINST* o;
    int first = s_ninst;
    double L = p->len, s0, top, shift, nTop;
    if (L < 1.0 || s_ninst >= TAGPU_RG_MAXINST) return;
    top = floor(log2(L / (space * 4.0)));
    if (top < 0.0) top = 0.0;
    nTop = floor(L / (space * pow(2.0, top)) + 0.5);
    if (nTop < 1.0) nTop = 1.0;
    s0 = L / (nTop * pow(2.0, top));
    shift = fmod(DASH[row].sign * s_shift[row], L);
    if (shift < 0.0) shift += L;
    o = &s_inst[s_ninst++];
    o->c[0] = (float)p->first; o->c[1] = (float)p->nseg; o->c[2] = side; o->c[3] = width;
    o->b[0] = (float)s0; o->b[1] = (float)top; o->b[2] = 0.0f; o->b[3] = (float)shift;
    memcpy(o->col, col, sizeof o->col);
    o->w[0] = dashLen; o->w[1] = fminf(col[3] * NEW_GAIN, 1.0f); o->w[2] = 0.0f; o->w[3] = 0.0f;
    put_draw(TAGPU_RG_SLINE, 0, first, p->nseg);
}

static void put_paths(int set, int kind, float side, float width, int row, const float col[4],
                      float space, float dashLen)
{
    int k;
    for (k = 0; k < s_npaths; k++)
        if (s_paths[k].set == set && s_paths[k].kind == kind)
            put_spath(&s_paths[k], side, width, row, col, space, dashLen);
}

/* THE SENSOR LAYER, in its draw order: the jammers' fills and bands, then
   the radar and sonar lines -- one stencil bit a jammer kind, so a kind's
   discs claim each pixel once (the disc pipeline's NOT_EQUAL + REPLACE). A
   kind is drawn under the view, and while a building that adds it is placed
   (`kind_shown`); the building's disc is one of its kind's like any other. */
static void draw_sensors(const RITEM* const* L, int n, const GROUND* G, int view,
                         float zoom, float devPx, float gh)
{
    float hDev = gh * devPx, ls = (hDev + 500.0f) / 1300.0f;
    float wLine = DASH[DR_LINE].w * ls / devPx;
    float wJam = JAM_W * hDev / 1080.0f / devPx + JAM_W_ELMO * zoom, wE = wJam / zoom;
    float space[2], dashLen[2];
    int kind, k, first, newMask = 0, cur;

    for (k = 0; k < n; k++) if (L[k]->isNew) newMask |= 1 << L[k]->sensor;
    /* this frame's inputs; the paths are rebuilt only when they moved */
    cur = s_keyCur ^ 1;
    {
        float* q = s_key[cur];
        int m = 0;
        memcpy(&q[m++], &G->serial, sizeof *q);
        q[m++] = (float)view; q[m++] = (float)G->w; q[m++] = (float)G->h;
        q[m++] = G->sea; q[m++] = G->mapW; q[m++] = G->mapH; q[m++] = (float)newMask;
        for (k = 0; k < n; k++) {
            q[m++] = (float)L[k]->sensor; q[m++] = (float)L[k]->isNew;
            q[m++] = L[k]->x; q[m++] = L[k]->z; q[m++] = L[k]->range;
        }
        s_keyN[cur] = m;
        if (m != s_keyN[s_keyCur] || memcmp(q, s_key[s_keyCur], (size_t)m * sizeof *q))
            build_paths(G, L, n, view, newMask);
        s_keyCur = cur;
    }
    for (k = 0; k < 2; k++) {
        space[k] = DASH[k].space * ls / devPx;
        dashLen[k] = DASH[k].len * ls / devPx;
    }

    put_draw(TAGPU_RG_CLEAR, 0, 0, 0);
    for (kind = SK_RADARJAM; kind <= SK_SONARJAM; kind++) {
        if (!kind_shown(kind, view, newMask)) continue;
        /* the fill stops where the band starts: each disc short of its circle
           by the band's width */
        first = s_ninst;
        for (k = 0; k < n; k++)
            if (L[k]->sensor == kind)
                put_sdisc(L[k], fmaxf(1.0f, L[k]->range - wE), surface_at(G, L[k]->x, L[k]->z),
                          JAM_ALLIED, JAM_A * JAM_ALLIED_REL);
        put_draw(TAGPU_RG_DISC, 1 << kind, first, SENSOR_SEGS);
        put_paths(PS_JAM, kind, -1.0f, wJam, DR_JAM, JAM_OUTLINE, space[DR_JAM], dashLen[DR_JAM]);
    }
    for (kind = SK_RADAR; kind <= SK_SONAR; kind++)
        if (kind_shown(kind, view, newMask))
            put_paths(PS_VIEW, kind, 1.0f, wLine, DR_LINE, VIEW_COL[kind], space[DR_LINE],
                      dashLen[DR_LINE]);
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
    unsigned k, n, nsens = 0;
    int L, fresh;
    double t, dTicks;

    s_have = 0;
    s_ninst = 0; s_ndraw = 0;
    if (!h || !r || !pk->in_game) { s_sensorLast = 0; return; }
    n = pk->n_ring;
    for (L = 0; L <= TAGPU_PK_RL_LAST; L++) nl[L] = 0;
    t = game_ticks(pk);
    dTicks = t - s_animT;
    fresh = s_animFrame == 0 || pk->level_gen != s_animLevel || h->viewer != s_animViewer ||
            dTicks < -0.01 || dTicks > 30.0 || !s_sensorLast;
    s_animFrame++;
    s_animLevel = pk->level_gen;
    s_animViewer = h->viewer;
    s_animT = t;
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
        it->sensor = -1;
        it->isNew = 0;
        if (e->layer == TAGPU_PK_RL_SENSOR) {
            /* a sensor ring in any other class is not one this file made */
            if (e->cls < TAGPU_PK_RC_RADAR) continue;
            it->sensor = e->cls - TAGPU_PK_RC_RADAR;
            it->isNew = (e->flags & TAGPU_PK_RF_NEW) != 0;
            nsens++;
            if (!it->isNew && it->sensor <= SK_SONAR)
                it->range *= anim_scale(e->slot, it->sensor, !(e->flags & TAGPU_PK_RF_OFF), t, fresh);
            if (it->range < 1.0f) continue;
            layer[e->layer][nl[e->layer]++] = it;
            continue;
        }
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
    /* the dashes' level and motion every frame, so a fade or a march
       resumes where it was when the sensors come back */
    dash_step(zoom, ((float)v->gh * devPx + 500.0f) / 1300.0f, devPx, fresh ? 0.0 : dTicks);
    s_npathDrawn = 0;
    s_sensorLast = nsens > 0;   /* a circle shrunk to nothing still counts */
    if (nl[TAGPU_PK_RL_SENSOR]) {
        /* THE SENSORS ARE LAID ON THE GROUND THE TERRAIN PASS MIRRORS on this
           thread, earlier in this frame (tagpu_terr_heights says why the
           buffer holds until the next frame's gather); with none there is
           nothing to lay them on, and the Vulkan pass draws no rings then */
        GROUND G;
        if (tagpu_terr_heights(pk->level_gen, &G.g, &G.w, &G.h, &G.serial)) {
            G.sea = (float)pk->sea_level;
            G.mapW = (float)pk->map_pxw; G.mapH = (float)pk->map_pxh;
            draw_sensors(layer[TAGPU_PK_RL_SENSOR], nl[TAGPU_PK_RL_SENSOR], &G,
                         (h->flags & TAGPU_PK_RG_SVIEW) != 0, zoom, devPx, (float)v->gh);
            s_npathDrawn = s_npaths;
        } else if (!s_saidNoGround) {
            s_saidNoGround = 1;
            tagpu_logf("range: no height grid of this level on this thread - the sensors are not drawn");
        }
        if (s_pathCut && !s_saidPathCut) {
            s_saidPathCut = 1;
            tagpu_logf("range: the sensor outlines are past %d circles, %d arcs, %d paths or "
                       "%d points - some are not drawn", SC_MAX, ARC_MAX, SPATH_MAX, TAGPU_RG_MAXPT);
        }
    }
    for (L = TAGPU_PK_RL_DEF_ALLY; L <= TAGPU_PK_RL_LAST; L++)
        if (nl[L]) draw_layer(layer[L], nl[L], devPx, ss);
    /* the heartbeat: what arrived per layer and what it became */
    if ((v->frame_counter % 300u) == 0u)
        tagpu_logf("range: rings=%u sensor=%d paths=%d pts=%d def=%d/%d atk=%d/%d sel=%u "
                   "build=%u inst=%d draws=%d camDist=%.0f%s", n, nl[TAGPU_PK_RL_SENSOR],
                   s_npathDrawn, s_npathDrawn ? s_npts : 0,
                   nl[TAGPU_PK_RL_DEF_ALLY], nl[TAGPU_PK_RL_DEF_ENEMY],
                   nl[TAGPU_PK_RL_ATK_ALLY], nl[TAGPU_PK_RL_ATK_ENEMY], (unsigned)h->nsel,
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
    s_pub.pts = s_pts; s_pub.npts = s_npts; s_pub.ptSerial = s_ptSerial;
    {
        int fine = s_lodK, i;
        while (fine > s_lodK - DASH_SPAN && lod_op(fine - 1) > 0.004f) fine--;
        s_pub.lodFine = fine; s_pub.lodLevel = s_lodK;
        for (i = 0; i < TAGPU_RG_LODN; i++) s_pub.lodOp[i] = lod_op(fine + i);
    }
    s_have = 1;
}

int tagpu_range_handover(TAGPU_RGHAND* out, unsigned frame)
{
    if (!s_have || !out || s_pub.frame != frame) return 0;
    *out = s_pub;
    s_have = 0;
    return 1;
}
