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
     mult  = S / base                     S = the icon's edge in px
     z_S   = gh / (4970 * dist/100)
     d'    = z_S / (z * mult)             BAR's iconZoomDist / mult / FadeStart
     b     = fade/100                     BAR's (FadeStart - FadeVanish) / FadeStart
     alpha = 0                         if d' < 1 - b
           = 64 + 191 (d' - (1-b)) / b if b > 0 and d' < 1   (the engine's fade ramp)
           = 255                       otherwise
     hide  = d' > 1 and R * z < 0.45 * S                     (BAR's 0.9 * S/2)

   R is the type's model radius in world px. A hidden model's icon is opaque
   by construction (d' > 1 gives 255). With the defaults (scale 100, dist 100,
   fade 33) z = 1 gives alpha 0 for every class while gh is at most about
   0.67 * 4970 * min(mult), ~2600: at 1x the frame is the stock frame on every
   display up to 4K native, which tests/icontest.c checks. */
#include <math.h>
#include "tagpu_icontype.h"   /* TAGPU_ICLS_* */

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

/* A generated shape's edge in px: BAR's size compressed toward 1
   (0.75 * size + 0.25), so the classes stay legible at small bases, and never
   under 4 px. A class out of range is UNKNOWN's. */
static __inline int tagpu_icon_gen_edge(const TAGPU_ICON_VIEW* v, int cls)
{
    const float sz = TAGPU_ICON_BARSIZE[(unsigned)cls < TAGPU_ICLS_N ? cls : TAGPU_ICLS_UNKNOWN];
    const int e = (int)floorf(v->base * (0.75f * sz + 0.25f) + 0.5f);
    return e > 4 ? e : 4;
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

/* The predicate at level z for an icon `edge_px` wide over a model of radius
   `r_px`: the icon's alpha, 0..255, and in *hide whether the model is not
   drawn. 0 and no hide for a level, edge or view that cannot be evaluated. */
static __inline int tagpu_icon_eval(const TAGPU_ICON_VIEW* v, float z, int edge_px, unsigned r_px,
                                    int* hide)
{
    float d;
    *hide = 0;
    if (!(z > 0.0f) || edge_px <= 0 || !(v->zS > 0.0f)) return 0;
    d = v->zS * v->base / (z * (float)edge_px);
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
   INK, the outline, is a texel inside whose neighbour w device px away on
   either axis is outside, w = max(1, round(edge / 12)); the PLUS is ink, two
   arms to 0.45 at least one device px and 1/7 wide; a HOLLOW shape keeps the
   ink and a fill band 3w wide inside it, and its centre is outside.

   THE SHAPE IS DECIDED PER FRAGMENT from the quad's own (+-1, +-1) and its
   uv per device px, so it needs no atlas and no re-raster when the size or the
   supersampling changes. tagpu_icon_shape() below and TAGPU_GLSL_ICON_FN, the
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
   (fwx, fwy) = uv per device px. 0 outside, 1 fill, 2 ink. */
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
        return (!tagpu_icon_sil(sil, px - 4.0f * wx, py) || !tagpu_icon_sil(sil, px + 4.0f * wx, py) ||
                !tagpu_icon_sil(sil, px, py - 4.0f * wy) || !tagpu_icon_sil(sil, px, py + 4.0f * wy))
               ? 1 : 0;
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
    "    return (!taIconSil(s, p - vec2(4.0 * d.x, 0.0)) || !taIconSil(s, p + vec2(4.0 * d.x, 0.0)) ||\n" \
    "            !taIconSil(s, p - vec2(0.0, 4.0 * d.y)) || !taIconSil(s, p + vec2(0.0, 4.0 * d.y))) ? 1 : 0;\n" \
    "  return 1;\n" \
    "}\n"

#endif
