/* tagpu_huddraw.c -- the replay HUD's ops as instances. Contract: tagpu_huddraw.h.

   WHAT THE BROWSER DOES, DONE HERE, because the mockup is the reference (hud-layout.md):

     * a part is premultiplied (tagpu_hudart.c) and blended ONE, ONE_MINUS_SRC_ALPHA;
     * a box's edges fall where its fractional edges round half up (tagpu_hud_snap), as Chrome
       snaps an element to device pixels and hud.js pins its boxes: a part's in
       tagpu_hudlay_expand, a solid's, a dot's and a clip's here (snap). The rasteriser's
       pixel-centre rule alone would round an edge at exactly half a pixel down;
     * text sits on the baseline `display: flex; align-items: center; line-height: 1` gives it,
       with Blink's roundings (tagpu_hudlay_baseline); the baseline is rounded to a whole pixel
       and the pen keeps its fraction, as Skia draws horizontal text;
     * `letter-spacing` follows every character, the last included, so a centred string is
       centred with its trailing space, as Chrome centres it;
     * a drop shadow (`text-shadow: 0 s 0 rgba(0,0,0,.8)`) and the four-copy outline are the
       string again, offset, under it;
     * an overflowing text box clips its text and its shadows: `overflow: hidden`;
     * an SVG <text> stands on its y with its pen from its x, its middle or its end there by its
       text-anchor, and the chart's SVG clips what it holds to its box;
     * a stroke covers a pixel by the area of it the stroke's band takes, a pixel-wide box filter
       across it and along it: an SVG line's ends are butt, a polyline's joins round, and one
       polyline is one shape, its overlaps covered once.

   THE GLYPH ATLAS is a shelf packer over one 2048 x 2048 coverage image. A glyph is keyed by
   its face, its pixel size in 1/64ths, its glyph index and its pen's fraction in quarters; the
   HUD draws a few hundred such at any one scale. When one does not fit, the atlas is emptied and
   refilled on the next build, which happens when the screen or the replay HUD's scale changes
   the sizes. */
#include <math.h>
#include <string.h>
#include "tagpu_huddraw.h"
#include "tagpu_hudtext.h"

/* ---- the shaders ---------------------------------------------------------------------------

   THE SHADER PAIR IS A BUILD INPUT, NOT CODE THIS FILE RUNS: tools/spirv-gen.py reads it out of
   the preprocessed translation unit into inc/spirv/tagpu_huddraw.spv.h, which tagpu_vk_hud.c
   draws with.

   The vertex stage puts a four-vertex strip on the instance's rect, in clip space as the CPU
   computed it with y DOWN, so the pass needs no viewport flip and gl_FragCoord is in device
   pixels from the top left. The fragment stage clips to the instance's clip rect by pixel
   centre, then draws by mode:
     PART   the part's premultiplied texel times its opacity;
     SOLID  the colour;
     SHADE  the colour under an inset shadow: CSS `box-shadow: inset 0 0 B rgba(0,0,0,.55)`, a
            Gaussian of sigma B / 2 blurring the outside of the box in, so the shadow at a point
            is .55 x (1 - the blurred box's coverage there), the coverage being separable;
     DOT    the colour inside the rect's inscribed circle, its edge antialiased over a pixel;
     GLYPH  the colour times the atlas's coverage;
     LINE   the colour over a stroke from vPx.xy to vPx.zw, vPrm.y pixels each side, butt ends;
     CHART  the chart's lines in one band of one column (the chart's image, tagpu_huddraw.h):
            for each group in order its halo, its line and its cross composited over what is
            under it in the group, the line's coverage the largest any of its segments within
            reach gives (the union of round-joined segments), and the whole at the instance's
            opacity -- an SVG <g opacity> composites its children first, so lines that cross in
            the faint part show the top one, not the two summed;
     DROP   CSS box-shadow: the box vPx moved down by vUV.x and blurred as SHADE blurs, drawn
            outside the box only, as an outer shadow is.
   The dim filter of a player who is out, `brightness(b) saturate(s)`, is linear in a
   premultiplied colour, so it applies per instance and composes as the browser's does on the
   whole row. Its matrix is the Filter Effects spec's saturate. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 aNdc;\n"
    "layout(location=1) in vec4 aPx;\n"
    "layout(location=2) in vec4 aUv;\n"
    "layout(location=3) in vec4 aCol;\n"
    "layout(location=4) in vec4 aClip;\n"
    "layout(location=5) in vec4 aPrm;\n"
    "out vec2 vUV; flat out vec4 vPx; flat out vec4 vCol; flat out vec4 vClip; flat out vec4 vPrm;\n"
    "void main(){\n"
    "  vec2 k = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  gl_Position = vec4(mix(aNdc.xy, aNdc.zw, k), 0.0, 1.0);\n"
    "  vUV = mix(aUv.xy, aUv.zw, k);\n"
    "  vPx = aPx; vCol = aCol; vClip = aClip; vPrm = aPrm;\n"
    "}\n";

static const char* FS =
    "#version 330 core\n"
    "in vec2 vUV; flat in vec4 vPx; flat in vec4 vCol; flat in vec4 vClip; flat in vec4 vPrm;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 oCol;\n"
    "float phi(float x){\n"
    "  float t = 1.0 / (1.0 + 0.2316419 * abs(x));\n"
    "  float d = 0.3989423 * exp(-0.5 * x * x);\n"
    "  float p = d * t * (0.3193815 + t * (-0.3565638 + t * (1.781478 + t * (-1.821256 + t * 1.330274))));\n"
    "  return x >= 0.0 ? 1.0 - p : p;\n"
    "}\n"
    /* a pixel's share of a band hw either side of a line d away: a pixel-wide box filter */
    "float cover(float d, float hw){ return clamp(min(d + 0.5, hw) - max(d - 0.5, -hw), 0.0, 1.0); }\n"
    /* a segment's stroke: round at an end that joins another, butt at an end b0 / b1 */
    "float seg(vec2 p, vec2 a, vec2 b, float hw, bool b0, bool b1){\n"
    "  vec2 d = b - a;\n"
    "  float L = length(d);\n"
    "  vec2 u = L > 1e-4 ? d / L : vec2(1.0, 0.0);\n"
    "  vec2 q = p - a;\n"
    "  float al = dot(q, u), ac = abs(dot(q, vec2(-u.y, u.x)));\n"
    "  if (al < 0.0) return b0 ? cover(ac, hw) * clamp(0.5 + al, 0.0, 1.0) : cover(length(q), hw);\n"
    "  if (al > L) return b1 ? cover(ac, hw) * clamp(0.5 + L - al, 0.0, 1.0) : cover(length(p - b), hw);\n"
    "  return cover(ac, hw) * (b0 ? clamp(0.5 + al, 0.0, 1.0) : 1.0) * (b1 ? clamp(0.5 + L - al, 0.0, 1.0) : 1.0);\n"
    "}\n"
    "void main(){\n"
    "  vec2 p = gl_FragCoord.xy;\n"
    "  if (vClip.z >= vClip.x && (p.x < vClip.x || p.x >= vClip.z || p.y < vClip.y || p.y >= vClip.w)) discard;\n"
    "  vec4 t = texture(uTex, vUV);\n"
    "  int m = int(vPrm.x + 0.5);\n"
    "  vec4 c;\n"
    "  if (m == 0) c = t * vCol;\n"
    "  else if (m == 1) c = vCol;\n"
    "  else if (m == 2) {\n"
    "    float s = max(vPrm.y, 1e-3);\n"
    "    float ix = phi((p.x - vPx.x) / s) - phi((p.x - vPx.z) / s);\n"
    "    float iy = phi((p.y - vPx.y) / s) - phi((p.y - vPx.w) / s);\n"
    "    float sh = 0.55 * (1.0 - ix * iy);\n"
    "    c = vec4(vCol.rgb * (1.0 - sh), vCol.a);\n"
    "  } else if (m == 3) {\n"
    "    vec2 ce = 0.5 * (vPx.xy + vPx.zw);\n"
    "    float r = 0.5 * (vPx.z - vPx.x);\n"
    "    c = vCol * clamp(r - length(p - ce) + 0.5, 0.0, 1.0);\n"
    "  } else if (m == 4) c = vCol * t.r;\n"
    "  else if (m == 5) c = vCol * seg(p, vPx.xy, vPx.zw, vPrm.y, true, true);\n"
    "  else if (m == 6) {\n"
    "    vec4 g0 = texelFetch(uTex, ivec2(0, 0), 0), hp = texelFetch(uTex, ivec2(1, 0), 0);\n"
    "    vec4 ho = texelFetch(uTex, ivec2(2, 0), 0), cx = texelFetch(uTex, ivec2(3, 0), 0);\n"
    "    int n = int(g0.x + 0.5), np = int(g0.y + 0.5), j0 = int(vPx.x + 0.5), j1 = int(vPx.y + 0.5);\n"
    "    int bx = int(cx.z + 0.5) + int(vPx.z + 0.5);\n"
    "    vec4 acc = vec4(0.0);\n"
    "    for (int g = 1; g <= n; g++) {\n"
    "      vec4 yr = texelFetch(uTex, ivec2(bx, g), 0);\n"
    "      if (p.y < yr.x || p.y > yr.y) continue;\n"
    "      vec4 lc = texelFetch(uTex, ivec2(0, g), 0), fl = texelFetch(uTex, ivec2(1, g), 0);\n"
    "      float kl = 0.0, kh = 0.0;\n"
    "      vec2 a = texelFetch(uTex, ivec2(2 + j0, g), 0).xy;\n"
    "      for (int j = j0; j <= j1; j++) {\n"
    "        vec2 b = texelFetch(uTex, ivec2(3 + j, g), 0).xy;\n"
    "        kl = max(kl, seg(p, a, b, g0.z, j == 0, j == np - 2));\n"
    "        if (fl.x > 0.5) kh = max(kh, seg(p, a, b, g0.w, j == 0, j == np - 2));\n"
    "        a = b;\n"
    "      }\n"
    "      acc = hp * kh + acc * (1.0 - hp.a * kh);\n"
    "      acc = lc * kl + acc * (1.0 - kl);\n"
    "      if (fl.y > 0.5) {\n"
    "        vec2 q = fl.zw;\n"
    "        float r = cx.x;\n"
    "        float k = max(seg(p, q - vec2(r), q + vec2(r), cx.y, true, true),\n"
    "                      seg(p, q + vec2(r, -r), q + vec2(-r, r), cx.y, true, true));\n"
    "        acc = (fl.x > 0.5 ? ho : lc) * k + acc * (1.0 - k);\n"
    "      }\n"
    "    }\n"
    "    c = acc * vCol.a;\n"
    "  } else {\n"
    "    if (p.x >= vPx.x && p.x < vPx.z && p.y >= vPx.y && p.y < vPx.w) discard;\n"
    "    float s = max(vPrm.y, 1e-3);\n"
    "    float ix = phi((p.x - vPx.x) / s) - phi((p.x - vPx.z) / s);\n"
    "    float iy = phi((p.y - vPx.y - vUV.x) / s) - phi((p.y - vPx.w - vUV.x) / s);\n"
    "    c = vCol * (ix * iy);\n"
    "  }\n"
    "  if (vPrm.z != 1.0 || vPrm.w != 1.0) {\n"
    "    float s = vPrm.w;\n"
    "    vec3 q = c.rgb * vPrm.z;\n"
    "    c.rgb = vec3(dot(q, vec3(0.213 + 0.787 * s, 0.715 - 0.715 * s, 0.072 - 0.072 * s)),\n"
    "                 dot(q, vec3(0.213 - 0.213 * s, 0.715 + 0.285 * s, 0.072 - 0.072 * s)),\n"
    "                 dot(q, vec3(0.213 - 0.213 * s, 0.715 - 0.715 * s, 0.072 + 0.928 * s)));\n"
    "  }\n"
    "  oCol = c;\n"
    "}\n";
#pragma GCC diagnostic pop

/* ---- the glyph atlas ------------------------------------------------------------------------ */

#define NSLOT  8192             /* the cache's open-addressed table: well under half full   */
#define PADPX  1                /* empty texels round each glyph, so filtering never reaches a neighbour */

typedef struct {
    uint32_t key[3];            /* face | size64, glyph | quarter; 0 0 0 = empty           */
    int16_t  x, y, w, h;        /* in the atlas                                            */
    int16_t  ox, oy;            /* the bitmap's offset from the pen and the baseline        */
} GLYPH;

static unsigned char s_atlas[HUD_ATLAS * HUD_ATLAS];
static GLYPH    s_slot[NSLOT];
static int      s_shelf_y, s_shelf_h, s_shelf_x;    /* the open shelf                     */
static unsigned s_gen = 1;
static int      s_dirty0 = HUD_ATLAS, s_dirty1;
static int      s_full;                             /* a glyph did not fit: empty it next  */

static void atlas_empty(void)
{
    memset(s_atlas, 0, sizeof s_atlas);
    memset(s_slot, 0, sizeof s_slot);
    s_shelf_x = s_shelf_y = s_shelf_h = 0;
    s_gen++;
    s_dirty0 = HUD_ATLAS;
    s_dirty1 = 0;
    s_full = 0;
}

const unsigned char* tagpu_huddraw_atlas(unsigned* gen, int* y0, int* y1)
{
    *gen = s_gen;
    *y0 = s_dirty0;
    *y1 = s_dirty1;
    return s_atlas;
}
void tagpu_huddraw_atlas_sent(int y)
{
    if (y >= s_dirty1) { s_dirty0 = HUD_ATLAS; s_dirty1 = 0; }
    else if (y > s_dirty0) s_dirty0 = y;
}

/* The glyph's place in the atlas, rasterised on first use; NULL when it does not fit. */
static const GLYPH* glyph(const HUDFACE* f, int fi, float size, int g, int quarter)
{
    uint32_t k0 = ((uint32_t)fi << 24) | ((uint32_t)lroundf(size * 64) & 0xFFFFFFu), k1 = (uint32_t)g, k2 = (uint32_t)quarter + 1;
    uint32_t h = (k0 * 2654435761u) ^ (k1 * 40503u) ^ (k2 * 97u);
    int n, x0, y0, x1, y1, w, hh;
    float sc = stbtt_ScaleForMappingEmToPixels(&f->info, size), sx = (float)quarter / 4.0f;
    GLYPH* s;
    for (n = 0; n < NSLOT; n++) {
        s = &s_slot[(h + (uint32_t)n) & (NSLOT - 1)];
        if (s->key[2] == 0) break;
        if (s->key[0] == k0 && s->key[1] == k1 && s->key[2] == k2) return s;
    }
    if (n == NSLOT || n > NSLOT / 2) { s_full = 1; return NULL; }
    stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g, sc, sc, sx, 0, &x0, &y0, &x1, &y1);
    w = x1 - x0;
    hh = y1 - y0;
    if (s_shelf_x + w + 2 * PADPX > HUD_ATLAS) {
        s_shelf_y += s_shelf_h;
        s_shelf_x = s_shelf_h = 0;
    }
    if (w + 2 * PADPX > HUD_ATLAS || s_shelf_y + hh + 2 * PADPX > HUD_ATLAS) { s_full = 1; return NULL; }
    s->key[0] = k0; s->key[1] = k1; s->key[2] = k2;
    s->x = (int16_t)(s_shelf_x + PADPX);
    s->y = (int16_t)(s_shelf_y + PADPX);
    s->w = (int16_t)w;
    s->h = (int16_t)hh;
    s->ox = (int16_t)x0;
    s->oy = (int16_t)y0;
    if (w > 0 && hh > 0)
        stbtt_MakeGlyphBitmapSubpixel(&f->info, s_atlas + (size_t)s->y * HUD_ATLAS + s->x, w, hh, HUD_ATLAS, sc, sc, sx, 0, g);
    s_shelf_x += w + 2 * PADPX;
    if (hh + 2 * PADPX > s_shelf_h) s_shelf_h = hh + 2 * PADPX;
    if (s->y - PADPX < s_dirty0) s_dirty0 = s->y - PADPX;
    if (s->y + hh + PADPX > s_dirty1) s_dirty1 = s->y + hh + PADPX;
    return s;
}

/* ---- the instances ------------------------------------------------------------------------- */

typedef struct {
    const HUDART* art;
    const HUDOPS* ops;
    float         px;
    float         ox, oy;       /* the game screen's top-left on the attachment      */
    float         W, H;         /* the attachment                                    */
    HUDINST*      inst;
    int           n, cap;
    HUDBATCH*     b;
    int           nb, bcap;
    int           dropped;
    int           fresh;        /* the next instance opens a batch: a group's first          */
} B;

/* a device coordinate snapped as a part's edges are (tagpu_hud_snap) */
static float snap(float v) { return (float)tagpu_hud_snap(v); }

static HUDINST* emit(B* c, int tex, int mode, float x0, float y0, float x1, float y1, const HUDOP* op)
{
    HUDINST* q;
    /* a fill (tex -1) joins the run it falls in, but never a layer's: the pass rescales a layer
       run's uv (layer_record), and a drop shadow carries its offset there */
    const int join = !c->fresh && c->nb > 0 &&
                     (tex == -1 ? c->b[c->nb - 1].tex != HUD_TEX_LAYER : c->b[c->nb - 1].tex == tex);
    if (c->n >= c->cap) return NULL;
    if (!join) {
        if (c->nb >= c->bcap) return NULL;
        c->fresh = 0;
        c->b[c->nb].tex = tex == -1 ? HUD_TEX_ATLAS : tex;
        c->b[c->nb].first = c->n;
        c->b[c->nb].count = 0;
        c->nb++;
    }
    q = &c->inst[c->n++];
    c->b[c->nb - 1].count++;
    x0 += c->ox; x1 += c->ox; y0 += c->oy; y1 += c->oy;
    q->ndc[0] = x0 / c->W * 2 - 1; q->ndc[1] = y0 / c->H * 2 - 1;
    q->ndc[2] = x1 / c->W * 2 - 1; q->ndc[3] = y1 / c->H * 2 - 1;
    q->px[0] = x0; q->px[1] = y0; q->px[2] = x1; q->px[3] = y1;
    q->uv[0] = q->uv[1] = 0; q->uv[2] = q->uv[3] = 1;
    q->rgba[0] = q->rgba[1] = q->rgba[2] = q->rgba[3] = 1;
    if (op->clip.w >= 0) {
        /* a box's clip snapped as the box; the chart's SVG clipPath is not a box Chrome snaps */
        HUDRECT k = tagpu_hudlay_clip(op, c->px);
        if (op->kind != HOP_CHART) {
            q->clip[0] = snap(k.x * c->px); q->clip[1] = snap(k.y * c->px);
            q->clip[2] = (float)tagpu_hud_edge(k.x, k.w, c->px); q->clip[3] = (float)tagpu_hud_edge(k.y, k.h, c->px);
        } else {
            q->clip[0] = k.x * c->px; q->clip[1] = k.y * c->px;
            q->clip[2] = (k.x + k.w) * c->px; q->clip[3] = (k.y + k.h) * c->px;
        }
        q->clip[0] += c->ox; q->clip[2] += c->ox; q->clip[1] += c->oy; q->clip[3] += c->oy;
    } else {
        q->clip[0] = 1; q->clip[2] = 0; q->clip[1] = q->clip[3] = 0;
    }
    q->prm[0] = (float)mode;
    q->prm[1] = 0;
    if (op->flags & HOPF_DIM) {
        const HUDGLOBALS* g = tagpu_hudlay_globals(c->art->lay);
        q->prm[2] = g->dim_brightness;
        q->prm[3] = g->dim_saturate;
    } else
        q->prm[2] = q->prm[3] = 1;
    return q;
}

static void colour(HUDINST* q, uint32_t rgb, float a)
{
    q->rgba[0] = (float)((rgb >> 16) & 0xFF) / 255.0f * a;
    q->rgba[1] = (float)((rgb >> 8) & 0xFF) / 255.0f * a;
    q->rgba[2] = (float)(rgb & 0xFF) / 255.0f * a;
    q->rgba[3] = a;
}

#define MAXCH TAGPU_HUDTEXT_MAX

static void text(B* c, const HUDOP* op)
{
    const HUDGLOBALS* g = tagpu_hudlay_globals(c->art->lay);
    const struct HUDSTYLE* S = &g->style[op->style];
    int fi = c->art->style_face[op->style];
    const HUDFACE* f = &c->art->face[fi];
    float size = op->size * c->px, sc = stbtt_ScaleForMappingEmToPixels(&f->info, size);
    float sp = S->spacing * size, width, x, base;
    uint32_t cp[MAXCH], rgb[MAXCH];
    int gi[MAXCH], n = 0, i, k, r, copies;
    float pen[MAXCH];
    float off[5][2];
    float alpha[5];

    for (r = 0; r < op->nrun; r++) {
        const HUDRUN* run = &c->ops->run[op->run + r];
        const unsigned char *s = (const unsigned char*)c->ops->text + run->off, *e = s + run->len;
        while (s < e && n < MAXCH) {
            uint32_t ch = tagpu_hudtext_utf8(&s, e);
            cp[n] = S->upper ? tagpu_hudtext_upper(ch) : ch;
            rgb[n] = run->rgb ? run->rgb : S->rgb;
            gi[n] = stbtt_FindGlyphIndex(&f->info, (int)cp[n]);
            n++;
        }
    }
    width = tagpu_hudtext_pens(&f->info, sc, sp, gi, n, pen);
    if (op->flags & HOPF_BASE) {
        /* an SVG <text>: its anchor at x, its baseline at y, painted on a whole pixel */
        x = op->r.x * c->px - (op->centre == 1 ? width / 2 : op->centre == 2 ? width : 0);
        base = floorf(op->r.y * c->px + 0.5f);
    } else {
        x = op->r.x * c->px + (op->centre == 1 ? (op->r.w * c->px - width) / 2 : op->centre == 2 ? op->r.w * c->px - width : 0);
        /* Chrome's baseline (tagpu_hudlay_baseline), painted on a whole pixel */
        base = floorf(tagpu_hudlay_baseline(op->r.y * c->px, op->r.h * c->px, size, f->ascent, f->descent) + 0.5f);
    }

    /* the copies under the text, then the text */
    copies = 0;
    if (S->outline) {
        static const int pick[4][2] = { { 0, 1 }, { 2, 3 }, { 0, 3 }, { 2, 1 } };
        for (k = 0; k < 4; k++) {
            off[copies][0] = S->ol[pick[k][0]] * c->px;
            off[copies][1] = S->ol[pick[k][1]] * c->px;
            alpha[copies++] = 1;
        }
    } else if (S->shadow > 0) {
        off[copies][0] = 0;
        off[copies][1] = S->shadow * c->px;
        alpha[copies++] = 0.8f;
    }
    off[copies][0] = off[copies][1] = 0;
    alpha[copies] = 1;
    for (k = 0; k <= copies; k++) {
        for (i = 0; i < n; i++) {
            float gx = x + pen[i] + off[k][0], whole = floorf(gx);
            int q4 = (int)((gx - whole) * 4);
            const GLYPH* gl;
            HUDINST* q;
            float x0, y0;
            if (q4 > 3) q4 = 3;
            if (!(gl = glyph(f, fi, size, gi[i], q4))) { c->dropped++; continue; }
            if (gl->w <= 0 || gl->h <= 0) continue;
            x0 = whole + gl->ox;
            /* a copy keeps its vertical offset's fraction, which the atlas's filter carries */
            y0 = base + off[k][1] + gl->oy;
            if (!(q = emit(c, HUD_TEX_ATLAS, HUDM_GLYPH, x0, y0, x0 + gl->w, y0 + gl->h, op))) return;
            q->uv[0] = (float)gl->x / HUD_ATLAS; q->uv[1] = (float)gl->y / HUD_ATLAS;
            q->uv[2] = (float)(gl->x + gl->w) / HUD_ATLAS; q->uv[3] = (float)(gl->y + gl->h) / HUD_ATLAS;
            colour(q, k < copies ? 0 : rgb[i], alpha[k]);
        }
    }
}

/* ---- the chart -------------------------------------------------------------------------------

   THE IMAGE (tagpu_huddraw.h) holds the lines in device pixels; THE INSTANCES are bands of
   columns. A column is HUD_CHART_SEG segments wide, the first and the last stretched by what a
   stroke or a cross reaches past the ends; it sees the segments within that reach of it, and each
   group's y range there, which its band test reads. The groups' ranges in a column, merged where
   they overlap, are its bands: every pixel a line can cover is in exactly one band, so the
   fragment stage composites every group a pixel can see in one invocation, and a pixel no line
   reaches is never shaded. */
static float    s_chart[HUD_CHART_H][HUD_CHART_W][4];
static float    s_cnext[HUD_CHART_H][HUD_CHART_W][4];
static unsigned s_cgen = 1;
typedef struct { float x0, y0, x1, y1; int j0, j1, col; } BAND;
static BAND     s_band[HUD_CHART_COLS * HUD_MAXSEAT];
static int      s_nband;

const float* tagpu_huddraw_chart(unsigned* gen)
{
    *gen = s_cgen;
    return &s_chart[0][0][0];
}

static void rgba4(float* t, uint32_t rgb, float a)
{
    t[0] = (float)((rgb >> 16) & 0xFF) / 255.0f * a;
    t[1] = (float)((rgb >> 8) & 0xFF) / 255.0f * a;
    t[2] = (float)(rgb & 0xFF) / 255.0f * a;
    t[3] = a;
}

static void chart_prepare(const B* c)
{
    const HUDCHART* K = &c->ops->chart;
    float px = c->px, hw = K->w * px / 2, hh = K->halo_w * px / 2, cr = K->cross * px, cw = K->cross_w * px / 2;
    float reach = (hw > hh ? hw : hh) + 1, ce = cr + cw + 1, ends = reach > ce ? reach : ce;
    int n = K->n < HUD_MAXSEAT ? K->n : HUD_MAXSEAT, np = K->npt < HUD_MAXPT ? K->npt : HUD_MAXPT;
    int ncol = !n || np < 1 ? 0 : np < 2 ? 1 : (np - 2) / HUD_CHART_SEG + 1, col, g, j, k;
    float lo[HUD_MAXSEAT], hi[HUD_MAXSEAT];
    int ord[HUD_MAXSEAT];

    memset(s_cnext, 0, sizeof s_cnext);
    s_cnext[0][0][0] = (float)n;
    s_cnext[0][0][1] = (float)np;
    s_cnext[0][0][2] = hw;
    s_cnext[0][0][3] = hh;
    rgba4(s_cnext[0][1], K->halo_rgb, K->halo_a);
    rgba4(s_cnext[0][2], K->halo_rgb, 1);
    s_cnext[0][3][0] = cr;
    s_cnext[0][3][1] = cw;
    s_cnext[0][3][2] = (float)HUD_CHART_BOX;
    for (g = 0; g < n; g++) {
        float* f = s_cnext[1 + g][1];
        rgba4(s_cnext[1 + g][0], K->g[g].rgb, 1);
        f[0] = (float)(K->g[g].dark != 0);
        f[1] = (float)(K->g[g].cross != 0);
        f[2] = K->g[g].cx * px + c->ox;
        f[3] = K->g[g].cy * px + c->oy;
        for (j = 0; j < np; j++) {
            s_cnext[1 + g][2 + j][0] = K->pt[g][j][0] * px + c->ox;
            s_cnext[1 + g][2 + j][1] = K->pt[g][j][1] * px + c->oy;
        }
    }

    /* the bands, in the screen's device pixels as emit takes them; the image is the attachment's */
    s_nband = 0;
#define PX(i) (K->pt[0][i][0] * px)
    for (col = 0; col < ncol; col++) {
        int js = col * HUD_CHART_SEG, je = np - 2 < js + HUD_CHART_SEG - 1 ? np - 2 : js + HUD_CHART_SEG - 1;
        float x0 = col == 0 ? PX(0) - ends : PX(js), x1 = col == ncol - 1 ? PX(np - 1) + ends : PX(je + 1);
        int j0 = js, j1 = je, nb = 0;
        while (j0 > 0 && PX(j0) >= x0 - reach) j0--;
        while (j1 + 1 <= np - 2 && PX(j1 + 1) <= x1 + reach) j1++;
        for (g = 0; g < n; g++) {
            float y0 = 1e30f, y1 = -1e30f;
            for (j = j0; j <= j1 + 1 && j < np; j++) {
                float y = K->pt[g][j][1] * px;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
            y0 -= reach;
            y1 += reach;
            if (K->g[g].cross) {
                float cx = K->g[g].cx * px, cy = K->g[g].cy * px;
                if (cx + ce >= x0 && cx - ce <= x1) {
                    if (cy - ce < y0) y0 = cy - ce;
                    if (cy + ce > y1) y1 = cy + ce;
                }
            }
            s_cnext[1 + g][HUD_CHART_BOX + col][0] = y0 + c->oy;
            s_cnext[1 + g][HUD_CHART_BOX + col][1] = y1 + c->oy;
            lo[g] = y0;
            hi[g] = y1;
            for (k = nb++; k > 0 && lo[ord[k - 1]] > y0; k--) ord[k] = ord[k - 1];
            ord[k] = g;
        }
        for (k = 0; k < nb; k++) {
            BAND* b = &s_band[s_nband++];
            float top = lo[ord[k]], bot = hi[ord[k]];
            while (k + 1 < nb && lo[ord[k + 1]] <= bot) {
                k++;
                if (hi[ord[k]] > bot) bot = hi[ord[k]];
            }
            b->x0 = x0; b->x1 = x1; b->y0 = top; b->y1 = bot;
            b->j0 = j0; b->j1 = j1; b->col = col;
        }
    }
#undef PX
    if (memcmp(s_cnext, s_chart, sizeof s_chart)) {
        memcpy(s_chart, s_cnext, sizeof s_chart);
        s_cgen++;
    }
}

/* One op's instances, at the B's origin. */
static void draw_op(B* c, const HUDOP* op, int* charted)
{
    const HUDART* art = c->art;
    const float px = c->px;
    HUDQUAD q[HUD_MAXQUAD];
    HUDINST* in;
    int j, n;
    switch (op->kind) {
    case HOP_PART:
        n = tagpu_hudlay_expand(art->lay, op, px, q, HUD_MAXQUAD);
        for (j = 0; j < n; j++) {
            if (!(in = emit(c, op->part, HUDM_PART, q[j].x0, q[j].y0, q[j].x1, q[j].y1, op))) break;
            in->uv[0] = q[j].u0; in->uv[1] = q[j].v0; in->uv[2] = q[j].u1; in->uv[3] = q[j].v1;
            colour(in, 0xFFFFFF, (float)(op->rgba & 0xFF) / 255.0f);
        }
        break;
    case HOP_SOLID:
        if (op->flags & HOPF_DROP) {
            /* the blurred box reaches 3 sigma past its edges, where a Gaussian has all but
               0.14 % of itself */
            float x0 = op->r.x * px, y0 = op->r.y * px, x1 = (op->r.x + op->r.w) * px, y1 = (op->r.y + op->r.h) * px;
            float sg = op->shade * px / 2, dy = op->size * px, e = 3 * sg;
            if (!(in = emit(c, -1, HUDM_DROP, x0 - e, y0 - e + (dy < 0 ? dy : 0), x1 + e, y1 + e + (dy > 0 ? dy : 0), op))) break;
            in->px[0] = x0 + c->ox; in->px[1] = y0 + c->oy; in->px[2] = x1 + c->ox; in->px[3] = y1 + c->oy;
            in->uv[0] = in->uv[2] = dy;
            in->uv[1] = in->uv[3] = 0;
            colour(in, op->rgba >> 8, (float)(op->rgba & 0xFF) / 255.0f);
            in->prm[1] = sg;
            break;
        }
        if (op->flags & HOPF_RING) {
            float X[4], Y[4];
            int s;
            tagpu_hud_ring(op, px, X, Y);
            for (s = 0; s < 4; s++) {
                static const int side[4][4] = { { 0, 0, 3, 1 }, { 0, 2, 3, 3 }, { 0, 1, 1, 2 }, { 2, 1, 3, 2 } };
                const int* d = side[s];
                if (X[d[2]] <= X[d[0]] || Y[d[3]] <= Y[d[1]]) continue;
                if (!(in = emit(c, -1, HUDM_SOLID, X[d[0]], Y[d[1]], X[d[2]], Y[d[3]], op))) break;
                colour(in, op->rgba >> 8, (float)(op->rgba & 0xFF) / 255.0f);
            }
            break;
        }
        /* fall through */
    case HOP_DOT:
        if (!(in = emit(c, -1, op->kind == HOP_DOT ? HUDM_DOT : op->flags & HOPF_SHADE ? HUDM_SHADE : HUDM_SOLID,
                        snap(op->r.x * px), snap(op->r.y * px), (float)tagpu_hud_edge(op->r.x, op->r.w, px),
                        (float)tagpu_hud_edge(op->r.y, op->r.h, px), op))) break;
        colour(in, op->rgba >> 8, (float)(op->rgba & 0xFF) / 255.0f);
        in->prm[1] = op->shade * px / 2;
        break;
    case HOP_LINE: {
        float x0 = op->r.x * px, y0 = op->r.y * px, x1 = (op->r.x + op->r.w) * px, y1 = (op->r.y + op->r.h) * px;
        float hw = op->size * px / 2, e = hw + 1;
        if (!(in = emit(c, -1, HUDM_LINE, (x0 < x1 ? x0 : x1) - e, (y0 < y1 ? y0 : y1) - e,
                        (x0 > x1 ? x0 : x1) + e, (y0 > y1 ? y0 : y1) + e, op))) break;
        in->px[0] = x0 + c->ox; in->px[1] = y0 + c->oy; in->px[2] = x1 + c->ox; in->px[3] = y1 + c->oy;
        colour(in, op->rgba >> 8, (float)(op->rgba & 0xFF) / 255.0f);
        in->prm[1] = hw;
        break;
    }
    case HOP_CHART:
        if (!*charted) { chart_prepare(c); *charted = 1; }
        for (j = 0; j < s_nband; j++) {
            const BAND* bd = &s_band[j];
            if (!(in = emit(c, HUD_TEX_CHART, HUDM_CHART, bd->x0, bd->y0, bd->x1, bd->y1, op))) break;
            in->px[0] = (float)bd->j0; in->px[1] = (float)bd->j1; in->px[2] = (float)bd->col; in->px[3] = 0;
            colour(in, 0xFFFFFF, (float)(op->rgba & 0xFF) / 255.0f);
        }
        break;
    case HOP_TEXT:
        text(c, op);
        break;
    default:
        break;
    }
}

/* ---- the groups ------------------------------------------------------------------------------

   A GROUP (tagpu_hudlay.h, THE GROUPS) IS DRAWN AS CSS OPACITY DRAWS IT: its ops at full opacity
   into a region of its own on the layer image, cleared to transparent, and that region then one
   quad on the attachment at the group's opacity a, where the group stands in paint order. Both are
   premultiplied OVER, so the quad's pixel is a L + (1 - a L.a) B, the mockup's, for the layer L
   its ops composite to over nothing. Each group has its own region, so two that overlap on screen
   still fade apart. The region is the group's box on the attachment in whole pixels, and its
   instances are moved there by a whole-pixel offset, so they rasterise there as they would have in
   place, and the quad covers that box and samples the region texel for pixel.

   THE LAYER IS NO LARGER THAN THE ATTACHMENT: the regions are packed in shelves across the
   attachment's width and drawn with its viewport, so a region below its height would draw
   nothing. A group whose region does not fit is refused, and its ops are drawn on the attachment
   where they stand, each at the group's opacity -- overlapping ops of it then show through each
   other, the one difference -- and the build starts again without it, at most once per group. */


static void group_box(const B* c, int i0, int* x0, int* y0, int* x1, int* y1)
{
    float bx0 = c->W, by0 = c->H, bx1 = 0, by1 = 0;
    int i;
    for (i = i0; i < c->n; i++) {
        const HUDINST* q = &c->inst[i];
        float a = (q->ndc[0] + 1) * 0.5f * c->W, b = (q->ndc[1] + 1) * 0.5f * c->H;
        float e = (q->ndc[2] + 1) * 0.5f * c->W, f = (q->ndc[3] + 1) * 0.5f * c->H;
        if (q->clip[2] >= q->clip[0]) {
            a = a > q->clip[0] ? a : q->clip[0]; b = b > q->clip[1] ? b : q->clip[1];
            e = e < q->clip[2] ? e : q->clip[2]; f = f < q->clip[3] ? f : q->clip[3];
        }
        if (e <= a || f <= b) continue;
        bx0 = a < bx0 ? a : bx0; by0 = b < by0 ? b : by0;
        bx1 = e > bx1 ? e : bx1; by1 = f > by1 ? f : by1;
    }
    *x0 = (int)floorf(bx0 > 0 ? bx0 : 0); *y0 = (int)floorf(by0 > 0 ? by0 : 0);
    *x1 = (int)ceilf(bx1 < c->W ? bx1 : c->W); *y1 = (int)ceilf(by1 < c->H ? by1 : c->H);
}

/* group k's ops [i0, i1) drawn into its region; its composite at index `at` (-1 none) set. 0 when
   the region does not fit the attachment. */
static int draw_group(B* c, const HUDOPS* ops, int k, int i0, int i1, int at, int* charted,
                      HUDSHELF* sh, HUDLAYREG* g)
{
    int j0 = c->n, i, x0, y0, x1, y1, w, h;
    float dx, dy;
    g->b0 = c->nb;
    g->x = g->y = g->w = g->h = 0;
    c->fresh = 1;
    for (i = i0; i < i1; i++) draw_op(c, &ops->op[i], charted);
    c->fresh = 0;
    g->nb = c->nb - g->b0;
    group_box(c, j0, &x0, &y0, &x1, &y1);
    w = x1 - x0; h = y1 - y0;
    if (w <= 0 || h <= 0) {
        g->nb = 0;
        if (at >= 0) colour(&c->inst[at], 0, 0);
        return 1;
    }
    /* the box lies within the attachment, so its width fits an empty shelf */
    if (sh->x + w > (int)c->W) { sh->y += sh->h; sh->x = 0; sh->h = 0; }
    if (sh->y + h > (int)c->H) return 0;
    g->x = sh->x; g->y = sh->y; g->w = w; g->h = h;
    sh->x += w;
    sh->h = h > sh->h ? h : sh->h;
    sh->w_used = sh->x > sh->w_used ? sh->x : sh->w_used;
    sh->h_used = sh->y + sh->h;
    dx = (float)(g->x - x0); dy = (float)(g->y - y0);
    for (i = j0; i < c->n; i++) {
        HUDINST* q = &c->inst[i];
        q->ndc[0] += 2 * dx / c->W; q->ndc[2] += 2 * dx / c->W;
        q->ndc[1] += 2 * dy / c->H; q->ndc[3] += 2 * dy / c->H;
        q->px[0] += dx; q->px[2] += dx; q->px[1] += dy; q->px[3] += dy;
        if (q->clip[2] >= q->clip[0]) { q->clip[0] += dx; q->clip[2] += dx; q->clip[1] += dy; q->clip[3] += dy; }
    }
    if (at >= 0) {
        HUDINST* q = &c->inst[at];
        q->ndc[0] = x0 / c->W * 2 - 1; q->ndc[1] = y0 / c->H * 2 - 1;
        q->ndc[2] = x1 / c->W * 2 - 1; q->ndc[3] = y1 / c->H * 2 - 1;
        q->px[0] = (float)x0; q->px[1] = (float)y0; q->px[2] = (float)x1; q->px[3] = (float)y1;
        q->uv[0] = (float)g->x; q->uv[1] = (float)g->y;
        q->uv[2] = (float)(g->x + w); q->uv[3] = (float)(g->y + h);
        colour(q, 0xFFFFFF, ops->grp[k]);
    }
    return 1;
}

void tagpu_huddraw_begin(void)
{
    if (s_full) atlas_empty();
}

int tagpu_huddraw_build(const HUDART* art, const HUDOPS* ops, int layer, float px, int ox, int oy,
                        int w, int h, HUDINST* inst, int cap, HUDBATCH* b, int bcap, int* nb,
                        int* nmain, int* nminst, HUDLAYREG* g, int* ng, HUDSHELF* shelf,
                        int* dropped, int* unfit)
{
    B c;
    HUDOP none;
    HUDSHELF sh;
    int at[HUD_MAXGRP];
    unsigned char out[HUD_MAXGRP];      /* the groups refused a region: drawn op by op */
    const int ng0 = *ng;
    int i, i0, k, charted, again;
    memset(out, 0, sizeof out);
    memset(&none, 0, sizeof none);
    none.clip.w = -1;
    *unfit = 0;
    do {
        again = 0;
        charted = 0;
        memset(&c, 0, sizeof c);
        c.art = art; c.ops = ops; c.px = px; c.ox = (float)ox; c.oy = (float)oy; c.W = (float)w; c.H = (float)h;
        c.inst = inst; c.cap = cap; c.b = b; c.bcap = bcap;
        for (k = 0; k < HUD_MAXGRP; k++) at[k] = -1;

        /* the attachment's instances, a group's composite where its last op stands; a refused
           group's ops in place, each faded by the group's opacity (premultiplied: all four). A
           group's ops are one control's, all of one layer. */
        for (i = 0; i < ops->n; i++) {
            const HUDOP* op = &ops->op[i];
            HUDINST* q;
            if (((op->flags & HOPF_TOP) != 0) != (layer != 0)) continue;
            if (!op->grp || op->grp > ops->ngrp || out[op->grp - 1]) {
                int j, n0 = c.n;
                float a = op->grp && op->grp <= ops->ngrp ? ops->grp[op->grp - 1] : 1.0f;
                if (op->grp > ops->ngrp) continue;
                draw_op(&c, op, &charted);
                for (j = n0; j < c.n && a != 1.0f; j++) {
                    c.inst[j].rgba[0] *= a; c.inst[j].rgba[1] *= a; c.inst[j].rgba[2] *= a; c.inst[j].rgba[3] *= a;
                }
                continue;
            }
            if (i + 1 < ops->n && ops->op[i + 1].grp == op->grp) continue;
            if ((q = emit(&c, HUD_TEX_LAYER, HUDM_PART, 0, 0, 0, 0, &none)))
                at[op->grp - 1] = (int)(q - c.inst);
        }
        *nmain = c.nb;
        *nminst = c.n;

        /* the groups' own, each in its region of the layer, packed after the regions an earlier
           build of the frame placed; a retry starts again from where this build began */
        sh = *shelf;
        *ng = ng0;
        for (i = 0; i < ops->n && !again; i = i0) {
            int gk = ops->op[i].grp;
            i0 = i + 1;
            if (!gk || ((ops->op[i].flags & HOPF_TOP) != 0) != (layer != 0)) continue;
            while (i0 < ops->n && ops->op[i0].grp == gk) i0++;
            if (gk > ops->ngrp || out[gk - 1]) continue;
            if (draw_group(&c, ops, gk - 1, i, i0, at[gk - 1], &charted, &sh, &g[*ng])) ++*ng;
            else { out[gk - 1] = 1; ++*unfit; again = 1; }
        }
    } while (again);
    *shelf = sh;
    *nb = c.nb;
    *dropped = c.dropped;
    return c.n;
}
