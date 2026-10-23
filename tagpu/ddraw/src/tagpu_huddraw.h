#ifndef TAGPU_HUDDRAW_H
#define TAGPU_HUDDRAW_H
/* tagpu_huddraw -- the replay HUD's ops as the instances tagpu_vk_hud.c draws: the parts as
   textured quads (tagpu_hudlay_expand), the fills, and the text through a glyph atlas this
   module rasterises with stb_truetype. It holds the pass's shaders too (tools/spirv-gen.py reads
   them from here). No Vulkan, and no engine state: RENDER THREAD ONLY, because the glyph atlas is
   this module's and only that thread writes it. */
#include "tagpu_hudart.h"

/* One quad, drawn as one instance of a four-vertex strip. 96 bytes; the vertex input of
   tagpu_vk_hud.c describes exactly these six vec4s, locations 0..5. */
typedef struct {
    float ndc[4];       /* x0, y0, x1, y1 in clip space (y down: no viewport flip)            */
    float px[4];        /* the same, in device pixels: the dot's circle, the shade's box; a
                           line's two ends; a drop shadow's box; a chart band's segments j0, j1
                           and its column                                                     */
    float uv[4];        /* u0, v0, u1, v1; a drop shadow's offset down, in u0 and u1          */
    float rgba[4];      /* premultiplied; a part's opacity in all four                        */
    float clip[4];      /* x0, y0, x1, y1 in device pixels; x1 < x0: none                     */
    float prm[4];       /* mode (HUDM_*), the shade's or the drop's sigma or a line's half
                           width in pixels, dim brightness, saturate                          */
} HUDINST;

enum { HUDM_PART, HUDM_SOLID, HUDM_SHADE, HUDM_DOT, HUDM_GLYPH, HUDM_LINE, HUDM_CHART, HUDM_DROP };

/* A run of instances drawn with one texture bound: a part's index, HUD_TEX_ATLAS,
   HUD_TEX_CHART or HUD_TEX_LAYER. A fill samples nothing, so it joins whatever run it falls in
   rather than breaking it -- except a HUD_TEX_LAYER run, which holds only groups' composites,
   since the pass rescales its uv. */
#define HUD_TEX_ATLAS (-2)
#define HUD_TEX_CHART (-3)
#define HUD_TEX_LAYER (-4)
typedef struct { int tex, first, count; } HUDBATCH;

/* A GROUP'S REGION OF THE LAYER IMAGE (tagpu_huddraw.c, THE GROUPS), in its pixels from its top
   left, and its instances: batches b0 .. b0 + nb - 1, drawn into the layer with the attachment's
   viewport and the region as the scissor. */
typedef struct { int x, y, w, h, b0, nb; } HUDLAYREG;

/* The instances for `ops` at `px` device pixels a unit, the game screen's top-left corner at
   (`ox`, `oy`) on a `w` x `h` attachment. Returns the instance count; `*nb` batches, of which the
   first `*nmain` are the attachment's and the rest the groups' (`g`, `*ng` of them, room for
   HUD_MAXGRP), whose layer is at least `*lw` x `*lh`, never more than `w` x `h`. A group's
   composite is a HUD_TEX_LAYER instance whose uv are the region's corners IN LAYER PIXELS, which
   the pass divides by the size of the image it binds. A group whose region does not fit is drawn
   op by op on the attachment instead, counted in `*unfit`. Glyphs the atlas could not take are
   left out and counted in `*dropped` (the atlas is rebuilt on the next call that needs room). */
int tagpu_huddraw_build(const HUDART* art, const HUDOPS* ops, float px, int ox, int oy, int w, int h,
                        HUDINST* inst, int cap, HUDBATCH* b, int bcap, int* nb, int* nmain,
                        HUDLAYREG* g, int* ng, int* lw, int* lh, int* dropped, int* unfit);

/* THE GLYPH ATLAS: one R8 coverage image. `gen` changes when it is emptied because a glyph did
   not fit; the rows [*y0, *y1) are the ones changed since tagpu_huddraw_atlas_sent last covered
   them. An emptied atlas is all zeros and every glyph it then takes lies, with the empty texels
   round it, inside the changed rows -- so a device copy of an older generation is current once
   those rows are sent, whatever the rest of it holds. */
#define HUD_ATLAS 2048
const unsigned char* tagpu_huddraw_atlas(unsigned* gen, int* y0, int* y1);
/* The changed rows below `y` are on the device. */
void                 tagpu_huddraw_atlas_sent(int y);

/* THE CHART'S LINES AS DATA: one RGBA32F image the chart mode reads with texelFetch, in device
   pixels on the attachment. Row 0 holds (groups, points, the line's and the halo's half width),
   the halo at its opacity, the halo opaque, and (the cross's half size, its half width,
   HUD_CHART_BOX), so the shader names no size of its own. Row 1 + g is group g: texel 0 its
   colour, premultiplied; texel 1 (its halo, its cross, the cross's centre); texels 2 ..
   2 + HUD_MAXPT - 1 its points; texel HUD_CHART_BOX + k the y range in which column k's
   instances can see it. `gen` changes with the contents; the image is sent whole. */
#define HUD_CHART_SEG   8                                       /* segments a column          */
#define HUD_CHART_COLS  ((HUD_MAXPT - 1 + HUD_CHART_SEG - 1) / HUD_CHART_SEG)
#define HUD_CHART_BOX   (2 + HUD_MAXPT)
#define HUD_CHART_W     (HUD_CHART_BOX + HUD_CHART_COLS)
#define HUD_CHART_H     (HUD_MAXSEAT + 1)
const float* tagpu_huddraw_chart(unsigned* gen);

#endif
