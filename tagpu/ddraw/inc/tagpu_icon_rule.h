#ifndef TAGPU_ICON_RULE_H
#define TAGPU_ICON_RULE_H
/* tagpu_icon_rule.h -- BAR's strategic-icon rule in TA's terms, and the
   generated icon shapes. Pure: no engine name and no Win32, so
   tests/icontest.c runs every line of it on the host. Included by the render
   side (tagpu_icon.c), the pick (tagpu_zoom.c) and that test.

   THE CALIBRATION. BAR's camera is a perspective camera with a 45-degree
   vertical field of view, and its icon test is on the camera distance, 3000
   elmos by default (Recoil d93c4da, UnitDrawerData.cpp). At that distance the
   ground span is 2 * 3000 * tan(22.5) = 2485 elmos, which is 4970 TA px
   [INFERRED: 1 elmo is about 2 TA px -- Spring's square is 8 elmos against
   TA's 16-px cell, and the Peewee is 2x2 squares in both]. TA's level z is
   screen px per world px, so a frame `gh` engine px tall spans 4970 world px
   at z_S = gh / 4970: 0.217 at 1080, 0.435 at 2160. BAR's camera tilt is
   ignored [INFERRED: a top-down approximation, which the distance setting
   absorbs].

   THE PREDICATE, per unit type, from the frame's level z:

     base  = max(1, max(gw, gh) * 0.005 * 1.05 * scale/100)   (BAR's UnitIconScaleUI)
     mult  = S / base                     S = the icon's BAR size in px
                                          (tagpu_icon_bar_edge)
     z_S   = gh / (4970 * dist/100)
     d'    = z_S / (z * mult)             BAR's iconZoomDist / mult / FadeStart
     b     = fade/100                     BAR's (FadeStart - FadeVanish) / FadeStart
     alpha = 0                         if d' < 1 - b
           = 64 + 191 (d' - (1-b)) / b if b > 0 and d' < 1   (the engine's fade ramp)
           = 255                       otherwise
     hide  = d' > 1 and R * z < 0.45 * E                     (BAR's 0.9 * S/2)

   R is the type's model radius in world px and E the edge the icon is drawn
   at. THE SWITCH IS BAR'S: d' takes the size BAR's rule gives, before the
   drawn edge's 7-px floor (tagpu_icon_gen_edge), so a frame or a scale small
   enough for the floor to bind does not move the level the icons take over
   at; only the cover test, which asks whether the icon as drawn hides the
   model, takes the drawn edge E. The two are equal wherever the floor does
   not bind -- on a frame whose longer side is over 1545 px at scale 100,
   2060 at scale 75 (tests/icontest.c) -- and at scale 75 on a 1920x1080
   frame COMBAT is 6 px by BAR's rule and drawn at 7. A hidden model's icon is opaque
   by construction (d' > 1 gives 255). With the defaults (scale 100, dist 100,
   fade 33) z = 1 gives alpha 0 for every class while gh is at most about
   0.67 * 4970 * min(mult): with S rounded to whole px the first alpha comes
   at 2569 to 2666 px tall by the frame's shape, so at 1x the frame is the
   stock frame on every display up to 2560 px tall, a mod set's pictures
   included, which tests/icontest.c checks. */
#include <math.h>
#include "tagpu_icontype.h"   /* TAGPU_ICLS_* */

/* THE CARGO RULE, one for the picture and the pick: a unit carried -- through
   any chain of carriers the unit pass draws -- by a unit whose model is hidden
   under its icon is part of that carrier's picture, neither a model nor an
   icon, and never hit. The chain is followed at most this many hops, the
   engine's own guard on a cargo chain: the render thread propagates it down
   the packet's cargo lists that many times (tagpu_icon.c), the pick walks up
   the carrier pointers that many hops (tagpu_zoom.c's icon_hit). */
#define TAGPU_ICON_CARGO_HOPS 64

typedef struct TAGPU_ICON_VIEW {
    float base;              /* BAR's icon size base, px                       */
    float zS;                /* the level at which the frame spans BAR's distance */
    float band;              /* the fade band b, 0 = BAR's hard switch          */
} TAGPU_ICON_VIEW;

/* BAR's icon size for the nearest unit of each class (icontypes.lua): armpw,
   armcom, armlab, armsolar, armca, armfig (AIRCOMBAT and AIR), armck, armpw;
   UNKNOWN is BAR's `default`. Indexed by TAGPU_ICLS_*. */
static const float TAGPU_ICON_BARSIZE[TAGPU_ICLS_N] = {
    /* UNKNOWN */ 1.0f,   /* COMMANDER */ 1.8375f, /* FACTORY */ 1.5225f,
    /* BUILDING */ 1.05f, /* AIRCONS */ 1.26f,     /* AIRCOMBAT */ 0.861f,
    /* AIR */ 0.861f,     /* CONS */ 0.9975f,      /* COMBAT */ 0.735f };

/* The view for a frame `gw` x `gh` engine px and the three settings, in
   percent (tagpu_settings.h: TS_ICONSCALE, TS_ICONDIST, TS_ICONFADE). */
static __inline void tagpu_icon_view(TAGPU_ICON_VIEW* v, int gw, int gh, int scale, int dist, int fade)
{
    const int m = gw > gh ? gw : gh;
    const float b = (float)m * 0.005f * 1.05f * (float)scale / 100.0f;
    v->base = b > 1.0f ? b : 1.0f;
    v->zS   = (gh > 0 && dist > 0) ? (float)gh / (4970.0f * (float)dist / 100.0f) : 0.0f;
    v->band = fade <= 0 ? 0.0f : fade >= 99 ? 0.99f : (float)fade / 100.0f;
}

/* A class's icon size by BAR's rule, in px: BAR's size compressed toward 1
   (0.75 * size + 0.25), so the classes stay legible at small bases. It is the
   predicate's S. A class out of range is UNKNOWN's. */
static __inline int tagpu_icon_bar_edge(const TAGPU_ICON_VIEW* v, int cls)
{
    const float sz = TAGPU_ICON_BARSIZE[(unsigned)cls < TAGPU_ICLS_N ? cls : TAGPU_ICLS_UNKNOWN];
    const int e = (int)floorf(v->base * (0.75f * sz + 0.25f) + 0.5f);
    return e > 1 ? e : 1;
}

/* The edge a generated shape is drawn at: BAR's size, never under
   TAGPU_ICON_EDGE_MIN. 7 px is the smallest edge at which all nine classes'
   shapes are pairwise different (tests/icontest.c asserts it, and that 6 is
   not): below it the 1-px ink alone decides the smaller shapes, and the three
   triangles or the diamond and the circle come out as the same picture -- a
   class that reads as another is a wrong answer, not a small one. */
#define TAGPU_ICON_EDGE_MIN 7
static __inline int tagpu_icon_gen_edge(const TAGPU_ICON_VIEW* v, int cls)
{
    const int e = tagpu_icon_bar_edge(v, cls);
    return e > TAGPU_ICON_EDGE_MIN ? e : TAGPU_ICON_EDGE_MIN;
}

/* Art is drawn at an integer scale, never resampled: a mod set's 10-px unit
   icon at k = round(base / 10), the 4-px radar dot at round(0.8 * base / 4)
   (BAR's default icon is size 1). */
static __inline int tagpu_icon_art_k(const TAGPU_ICON_VIEW* v)
{
    const int k = (int)floorf(v->base / 10.0f + 0.5f);
    return k > 1 ? k : 1;
}
static __inline int tagpu_icon_dot_k(const TAGPU_ICON_VIEW* v)
{
    const int k = (int)floorf(0.8f * v->base / 4.0f + 0.5f);
    return k > 1 ? k : 1;
}

/* What a type is drawn as: its mod set's picture when `icon`
   (TAGPU_PK_UTYPE.icon, DATA to this function) names one of `art`'s, at the
   art scale, or else its class's generated shape. *size is the predicate's
   S, *edge the edge the cover test and the pick take. THE SWITCH IS THE
   CLASS'S EITHER WAY: S is BAR's size for the class, so where a type turns
   into an icon does not depend on which picture a mod drew for it, and "1x
   is the stock frame" holds for a set exactly as for the shapes (the host
   test runs every picture size a set can hold). Only the cover test and the
   pick take the picture as drawn: a picture's edge is k x its longer side
   (art has no floor), a shape's the floored edge. Returns the picture's
   rect, or NULL for a shape. The render thread's picture (tagpu_icon.c) and
   the game thread's pick (tagpu_zoom.c) both ask this, so the two agree on
   every size. */
static __inline const TAGPU_ICONRECT* tagpu_icon_type(const TAGPU_ICON_VIEW* v, const TAGPU_ICONART* art,
                                                      int cls, unsigned icon, int* size, int* edge)
{
    *size = tagpu_icon_bar_edge(v, cls);
    if (art && (int)icon < art->nset && icon < TAGPU_ICONART_SETS) {
        const TAGPU_ICONRECT* r = &art->set[icon];
        *edge = tagpu_icon_art_k(v) * (r->w > r->h ? r->w : r->h);
        return r;
    }
    *edge = tagpu_icon_gen_edge(v, cls);
    return NULL;
}

/* A radar contact's BAR size, S for the switch whatever it is drawn as (the
   logo frame or a set's NONE picture): 0.8 x base, the size the dot's scale
   k_r is rounded from, NOT 4 x k_r. The two are equal at 1080p (8 px) and at
   4K native (16), but k_r rounds down by up to half a step, and a dot whose S
   followed it would switch sooner: at 3186 x 2390, 4 x k_r is 12 px against
   13, enough to fade contacts in at 1x. Taken unrounded the switch is the
   size's, and 1x stays stock to 2560 px (tests/icontest.c). */
static __inline int tagpu_icon_dot_size(const TAGPU_ICON_VIEW* v)
{
    const int e = (int)floorf(0.8f * v->base + 0.5f);
    return e > 1 ? e : 1;
}

/* The predicate at level z for an icon of BAR size `size_px` drawn `edge_px`
   wide over a model of radius `r_px`: the icon's alpha, 0..255, and in *hide
   whether the model is not drawn. Art has no floor: its edge is the picture
   as drawn, its size the class's or the contact's BAR size
   (tagpu_icon_type, tagpu_icon_dot_size). 0 and no hide for a level, size or
   view that cannot be evaluated. */
static __inline int tagpu_icon_eval(const TAGPU_ICON_VIEW* v, float z, int size_px, int edge_px,
                                    unsigned r_px, int* hide)
{
    float d;
    *hide = 0;
    if (!(z > 0.0f) || size_px <= 0 || edge_px <= 0 || !(v->zS > 0.0f)) return 0;
    d = v->zS * v->base / (z * (float)size_px);
    if (d < 1.0f - v->band) return 0;
    *hide = d > 1.0f && (float)r_px * z < 0.45f * (float)edge_px;
    if (v->band > 0.0f && d < 1.0f)
        return (int)floorf(64.0f + 191.0f * (d - (1.0f - v->band)) / v->band + 0.5f);
    return 255;
}

/* ---- the generated shapes ----------------------------------------------
   A grammar, not art: the domain by silhouette, a builder by a mark.
     COMMANDER  diamond  |x| + |y| <= 1
     FACTORY    square   max(|x|, |y|) <= 0.8, with a plus
     BUILDING   square
     AIRCONS    triangle, apex up -- apex (0, -0.95), base y = 0.80, half-width
                0.95 there -- with a plus
     AIRCOMBAT  triangle
     AIR        triangle, hollow
     CONS       circle   |p| <= 0.92, with a plus
     COMBAT     circle
     UNKNOWN    circle, hollow
   INK, the outline, is a texel inside whose neighbour w game px away on
   either axis is outside, w = max(1, round(edge / 12)); the PLUS is ink, two
   arms to 0.45 at least one game px and 1/7 wide; a HOLLOW shape keeps a
   RING, the texels whose distance to the silhouette's edge is at most
   max(0.45 * its inradius, 1 game px), and its centre is outside. The ring
   follows the shape's own inradius, not the ink's width: a band of a fixed
   number of w filled an 8-px circle and was wider than the triangle's
   incircle at every size, so the hollow classes looked like their solid twins
   exactly where the icons are small. Tied to the inradius, the hole is 55 % of
   the incircle at every size the 1-px floor does not bind, and the host test
   asserts each hollow class differs from its solid twin at every size from
   the smallest icon up.

   THE SHAPE IS DECIDED PER GAME PIXEL from the quad's own (+-1, +-1) and its
   edge, so it needs no atlas and no re-raster when the size or the supersampling
   changes, and is the same at every supersampling. tagpu_icon_shape() below and TAGPU_GLSL_ICON_FN, the
   marker pass's fragment stage (tagpu_mark.c), are the same function, line
   for line; the host test checks this one. */
enum { TAGPU_ICON_SIL_CIRCLE, TAGPU_ICON_SIL_SQUARE, TAGPU_ICON_SIL_DIAMOND, TAGPU_ICON_SIL_TRIANGLE };

static __inline int tagpu_icon_sil(int sil, float x, float y)
{
    const float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y;
    if (sil == TAGPU_ICON_SIL_CIRCLE)  return x * x + y * y <= 0.92f * 0.92f;
    if (sil == TAGPU_ICON_SIL_SQUARE)  return ax <= 0.8f && ay <= 0.8f;
    if (sil == TAGPU_ICON_SIL_DIAMOND) return ax + ay <= 1.0f;
    return y <= 0.8f && y >= -0.95f && ax <= 0.95f * (y + 0.95f) / 1.75f;
}

/* How deep inside the silhouette p lies, in uv: its distance to the nearest
   edge. Needed for the two hollow silhouettes only, the circle and the
   triangle; the triangle's sides are 1.75 x - 0.95 y - 0.9025 = 0 mirrored,
   normalised by their length 1.99123. */
static __inline float tagpu_icon_depth(int sil, float x, float y)
{
    const float ax = x < 0.0f ? -x : x;
    if (sil == TAGPU_ICON_SIL_CIRCLE) return 0.92f - sqrtf(x * x + y * y);
    return fminf(0.8f - y, (0.9025f + 0.95f * y - 1.75f * ax) / 1.99123f);
}
/* ...and its inradius, the depth of its deepest point: the circle's radius,
   the triangle's incircle (centre (0, 0.2348)) */
static __inline float tagpu_icon_inradius(int sil)
{
    return sil == TAGPU_ICON_SIL_CIRCLE ? 0.92f : 0.5652f;
}

/* The silhouette and the two marks of each class, by TAGPU_ICLS_*. */
static __inline int tagpu_icon_cls_sil(int cls)
{
    return cls == TAGPU_ICLS_COMMANDER ? TAGPU_ICON_SIL_DIAMOND
         : cls == TAGPU_ICLS_FACTORY || cls == TAGPU_ICLS_BUILDING ? TAGPU_ICON_SIL_SQUARE
         : cls == TAGPU_ICLS_AIRCONS || cls == TAGPU_ICLS_AIRCOMBAT || cls == TAGPU_ICLS_AIR
           ? TAGPU_ICON_SIL_TRIANGLE
         : TAGPU_ICON_SIL_CIRCLE;
}
static __inline int tagpu_icon_cls_plus(int cls)
{
    return cls == TAGPU_ICLS_FACTORY || cls == TAGPU_ICLS_AIRCONS || cls == TAGPU_ICLS_CONS;
}
static __inline int tagpu_icon_cls_hollow(int cls)
{
    return cls == TAGPU_ICLS_AIR || cls == TAGPU_ICLS_UNKNOWN;
}

/* The generated shape of class `cls` at p = (px, py) in [-1, 1]^2, y down;
   (fwx, fwy) = uv per game px. 0 outside, 1 fill, 2 ink. */
static __inline int tagpu_icon_shape(int cls, float px, float py, float fwx, float fwy)
{
    const int sil = tagpu_icon_cls_sil(cls);
    const float w = fmaxf(1.0f, floorf(1.0f / (6.0f * fwx) + 0.5f));
    const float wx = w * fwx, wy = w * fwy;
    if (!tagpu_icon_sil(sil, px, py)) return 0;
    if (!tagpu_icon_sil(sil, px - wx, py) || !tagpu_icon_sil(sil, px + wx, py) ||
        !tagpu_icon_sil(sil, px, py - wy) || !tagpu_icon_sil(sil, px, py + wy)) return 2;
    if (tagpu_icon_cls_plus(cls)) {
        const float ax = px < 0.0f ? -px : px, ay = py < 0.0f ? -py : py;
        if (fmaxf(ax, ay) <= 0.45f &&
            (ax <= fmaxf(0.5f * fwx, 1.0f / 7.0f) || ay <= fmaxf(0.5f * fwy, 1.0f / 7.0f))) return 2;
    }
    if (tagpu_icon_cls_hollow(cls))
        return tagpu_icon_depth(sil, px, py) <= fmaxf(0.45f * tagpu_icon_inradius(sil), fwx) ? 1 : 0;
    return 1;
}

/* THE SAME FUNCTION IN GLSL, for the marker pass's fragment stage
   (tagpu_mark.c, its icon branch): tagpu_icon_shape() above, line for line,
   with the class tables spelled as comparisons. `taIconShape(cls, p, fw)`. */
#define TAGPU_GLSL_ICON_FN \
    "bool taIconSil(int s, vec2 p){\n" \
    "  vec2 a = abs(p);\n" \
    "  if (s == 0) return dot(p, p) <= 0.92 * 0.92;\n" \
    "  if (s == 1) return a.x <= 0.8 && a.y <= 0.8;\n" \
    "  if (s == 2) return a.x + a.y <= 1.0;\n" \
    "  return p.y <= 0.8 && p.y >= -0.95 && a.x <= 0.95 * (p.y + 0.95) / 1.75;\n" \
    "}\n" \
    "float taIconDepth(int s, vec2 p){\n" \
    "  if (s == 0) return 0.92 - length(p);\n" \
    "  return min(0.8 - p.y, (0.9025 + 0.95 * p.y - 1.75 * abs(p.x)) / 1.99123);\n" \
    "}\n" \
    "int taIconShape(int c, vec2 p, vec2 fw){\n" \
    "  int s = c == 1 ? 2 : (c == 2 || c == 3) ? 1 : (c == 4 || c == 5 || c == 6) ? 3 : 0;\n" \
    "  float w = max(1.0, floor(1.0 / (6.0 * fw.x) + 0.5));\n" \
    "  vec2 d = w * fw;\n" \
    "  if (!taIconSil(s, p)) return 0;\n" \
    "  if (!taIconSil(s, p - vec2(d.x, 0.0)) || !taIconSil(s, p + vec2(d.x, 0.0)) ||\n" \
    "      !taIconSil(s, p - vec2(0.0, d.y)) || !taIconSil(s, p + vec2(0.0, d.y))) return 2;\n" \
    "  if (c == 2 || c == 4 || c == 7) {\n" \
    "    vec2 a = abs(p);\n" \
    "    if (max(a.x, a.y) <= 0.45 &&\n" \
    "        (a.x <= max(0.5 * fw.x, 1.0 / 7.0) || a.y <= max(0.5 * fw.y, 1.0 / 7.0))) return 2;\n" \
    "  }\n" \
    "  if (c == 6 || c == 0)\n" \
    "    return taIconDepth(s, p) <= max(0.45 * (s == 0 ? 0.92 : 0.5652), fw.x) ? 1 : 0;\n" \
    "  return 1;\n" \
    "}\n"

#endif
