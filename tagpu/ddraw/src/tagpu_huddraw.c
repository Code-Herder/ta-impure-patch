/* tagpu_huddraw.c -- the replay HUD's ops as instances. Contract: tagpu_huddraw.h.

   WHAT THE BROWSER DOES, DONE HERE, because the mockup is the reference (hud-layout.md):

     * a part is premultiplied (tagpu_hudart.c) and blended ONE, ONE_MINUS_SRC_ALPHA;
     * a box's edges fall where its fractional edges round, as Chrome snaps a background to
       device pixels: the rasteriser's pixel-centre rule does that for a quad;
     * text sits on the baseline `display: flex; align-items: center; line-height: 1` gives it,
       with Blink's roundings (tagpu_hudlay_baseline); the baseline is rounded to a whole pixel
       and the pen keeps its fraction, as Skia draws horizontal text;
     * `letter-spacing` follows every character, the last included, so a centred string is
       centred with its trailing space, as Chrome centres it;
     * a drop shadow (`text-shadow: 0 s 0 rgba(0,0,0,.8)`) and the four-copy outline are the
       string again, offset, under it;
     * an overflowing text box clips its text and its shadows: `overflow: hidden`.

   THE GLYPH ATLAS is a shelf packer over one 2048 x 2048 coverage image. A glyph is keyed by
   its face, its pixel size in 1/64ths, its glyph index and its pen's fraction in quarters; the
   HUD draws a few hundred such at any one scale. When one does not fit, the atlas is emptied and
   refilled on the next build, which happens when the screen or the replay HUD's scale changes
   the sizes. */
#include <math.h>
#include <string.h>
#include "tagpu_huddraw.h"

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
     GLYPH  the colour times the atlas's coverage.
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
    "  } else c = vCol * t.r;\n"
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
} B;

static HUDINST* emit(B* c, int tex, int mode, float x0, float y0, float x1, float y1, const HUDOP* op)
{
    HUDINST* q;
    if (c->n >= c->cap) return NULL;
    if (tex != -1 && (c->nb == 0 || c->b[c->nb - 1].tex != tex)) {
        if (c->nb >= c->bcap) return NULL;
        c->b[c->nb].tex = tex;
        c->b[c->nb].first = c->n;
        c->b[c->nb].count = 0;
        c->nb++;
    } else if (c->nb == 0) {
        if (c->bcap < 1) return NULL;
        c->b[0].tex = HUD_TEX_ATLAS;
        c->b[0].first = c->n;
        c->b[0].count = 0;
        c->nb = 1;
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
        q->clip[0] = op->clip.x * c->px + c->ox; q->clip[1] = op->clip.y * c->px + c->oy;
        q->clip[2] = (op->clip.x + op->clip.w) * c->px + c->ox; q->clip[3] = (op->clip.y + op->clip.h) * c->px + c->oy;
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

/* the next code point of UTF-8, or U+FFFD for a bad sequence */
static uint32_t utf8(const unsigned char** s, const unsigned char* e)
{
    const unsigned char* p = *s;
    uint32_t c = *p++;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (c >= 0x80 && n == 0) { *s = p; return 0xFFFD; }
    c &= n == 3 ? 0x07 : n == 2 ? 0x0F : n == 1 ? 0x1F : 0x7F;
    while (n-- > 0) {
        if (p >= e || (*p & 0xC0) != 0x80) { *s = p; return 0xFFFD; }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *s = p;
    return c;
}

/* CSS `text-transform: uppercase` for what a HUD string can hold: ASCII and Latin-1. ß has no
   one-letter capital and stays; ÿ becomes Ÿ, outside Latin-1. */
static uint32_t upper(uint32_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    return c;
}

#define MAXCH 160

static void text(B* c, const HUDOP* op)
{
    const HUDGLOBALS* g = tagpu_hudlay_globals(c->art->lay);
    const struct HUDSTYLE* S = &g->style[op->style];
    int fi = c->art->style_face[op->style];
    const HUDFACE* f = &c->art->face[fi];
    float size = op->size * c->px, sc = stbtt_ScaleForMappingEmToPixels(&f->info, size);
    float sp = S->spacing * size, width = 0, x, base;
    uint32_t cp[MAXCH], rgb[MAXCH];
    int gi[MAXCH], n = 0, i, k, r, copies;
    float pen[MAXCH];
    float off[5][2];
    float alpha[5];

    for (r = 0; r < op->nrun; r++) {
        const HUDRUN* run = &c->ops->run[op->run + r];
        const unsigned char *s = (const unsigned char*)c->ops->text + run->off, *e = s + run->len;
        while (s < e && n < MAXCH) {
            uint32_t ch = utf8(&s, e);
            cp[n] = S->upper ? upper(ch) : ch;
            rgb[n] = run->rgb ? run->rgb : S->rgb;
            gi[n] = stbtt_FindGlyphIndex(&f->info, (int)cp[n]);
            n++;
        }
    }
    for (i = 0; i < n; i++) {
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f->info, gi[i], &adv, &lsb);
        if (i) width += sc * (float)stbtt_GetGlyphKernAdvance(&f->info, gi[i - 1], gi[i]);
        pen[i] = width;
        width += sc * (float)adv + sp;
    }
    x = op->r.x * c->px + (op->centre ? (op->r.w * c->px - width) / 2 : 0);
    /* Chrome's baseline (tagpu_hudlay_baseline), painted on a whole pixel */
    base = floorf(tagpu_hudlay_baseline(op->r.y * c->px, op->r.h * c->px, size, f->ascent, f->descent) + 0.5f);

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

int tagpu_huddraw_build(const HUDART* art, const HUDOPS* ops, float px, int ox, int oy, int w, int h,
                        HUDINST* inst, int cap, HUDBATCH* b, int bcap, int* nb, int* dropped)
{
    B c;
    int i, j, n;
    HUDQUAD q[HUD_MAXQUAD];
    if (s_full) atlas_empty();
    memset(&c, 0, sizeof c);
    c.art = art; c.ops = ops; c.px = px; c.ox = (float)ox; c.oy = (float)oy; c.W = (float)w; c.H = (float)h;
    c.inst = inst; c.cap = cap; c.b = b; c.bcap = bcap;
    for (i = 0; i < ops->n; i++) {
        const HUDOP* op = &ops->op[i];
        HUDINST* in;
        switch (op->kind) {
        case HOP_PART:
            n = tagpu_hudlay_expand(art->lay, op, px, q, HUD_MAXQUAD);
            for (j = 0; j < n; j++) {
                if (!(in = emit(&c, op->part, HUDM_PART, q[j].x0, q[j].y0, q[j].x1, q[j].y1, op))) break;
                in->uv[0] = q[j].u0; in->uv[1] = q[j].v0; in->uv[2] = q[j].u1; in->uv[3] = q[j].v1;
                colour(in, 0xFFFFFF, (float)(op->rgba & 0xFF) / 255.0f);
            }
            break;
        case HOP_SOLID:
        case HOP_DOT:
            if (!(in = emit(&c, -1, op->kind == HOP_DOT ? HUDM_DOT : op->flags & HOPF_SHADE ? HUDM_SHADE : HUDM_SOLID,
                            op->r.x * px, op->r.y * px, (op->r.x + op->r.w) * px, (op->r.y + op->r.h) * px, op))) break;
            colour(in, op->rgba >> 8, (float)(op->rgba & 0xFF) / 255.0f);
            in->prm[1] = op->shade * px / 2;
            break;
        case HOP_TEXT:
            text(&c, op);
            break;
        default:
            break;
        }
    }
    *nb = c.nb;
    *dropped = c.dropped;
    return c.n;
}
