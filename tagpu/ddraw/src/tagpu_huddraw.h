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

/* A run of instances drawn with one texture bound: a part's index, HUD_TEX_ATLAS or
   HUD_TEX_CHART. A fill samples nothing, so it joins whatever run it falls in rather than
   breaking it. */
#define HUD_TEX_ATLAS (-2)
#define HUD_TEX_CHART (-3)
typedef struct { int tex, first, count; } HUDBATCH;

/* A frame's builds begin: the glyph atlas is emptied here, if a glyph did not fit, and nowhere
   else. THE ORDERING THE TWO LAYERS REST ON: an atlas emptied between a frame's two builds would
   leave the first build's glyphs naming emptied texels; emptied only before the frame's first
   build, both layers' glyphs are of one generation, which the upload rule above covers. Once a
   frame, before its first tagpu_huddraw_build. */
void tagpu_huddraw_begin(void);

/* The instances for layer `layer` of `ops` -- 0 the ops without HOPF_TOP, 1 those with it -- at
   `px` device pixels a unit, the game screen's top-left corner at (`ox`, `oy`) on a `w` x `h`
   attachment. Returns the instance count; `*nb` batches. Glyphs the atlas could not take are left
   out and counted in `*dropped` (the atlas is rebuilt at the next frame's tagpu_huddraw_begin). */
int tagpu_huddraw_build(const HUDART* art, const HUDOPS* ops, int layer, float px, int ox, int oy,
                        int w, int h, HUDINST* inst, int cap, HUDBATCH* b, int bcap, int* nb,
                        int* dropped);

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
