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
    float px[4];        /* the same, in device pixels: the dot's circle, the shade's box       */
    float uv[4];        /* u0, v0, u1, v1                                                     */
    float rgba[4];      /* premultiplied; a part's opacity in all four                        */
    float clip[4];      /* x0, y0, x1, y1 in device pixels; x1 < x0: none                     */
    float prm[4];       /* mode (HUDM_*), the shade's sigma in pixels, dim brightness, saturate */
} HUDINST;

enum { HUDM_PART, HUDM_SOLID, HUDM_SHADE, HUDM_DOT, HUDM_GLYPH };

/* A run of instances drawn with one texture bound: a part's index, or HUD_TEX_ATLAS. A fill
   samples nothing, so it joins whatever run it falls in rather than breaking it. */
#define HUD_TEX_ATLAS (-2)
typedef struct { int tex, first, count; } HUDBATCH;

/* The instances for `ops` at `px` device pixels a unit, the game screen's top-left corner at
   (`ox`, `oy`) on a `w` x `h` attachment. Returns the instance count; `*nb` batches. Glyphs the
   atlas could not take are left out and counted in `*dropped` (the atlas is rebuilt on the next
   call that needs room). */
int tagpu_huddraw_build(const HUDART* art, const HUDOPS* ops, float px, int ox, int oy, int w, int h,
                        HUDINST* inst, int cap, HUDBATCH* b, int bcap, int* nb, int* dropped);

/* THE GLYPH ATLAS: one R8 coverage image. `gen` changes when it is rebuilt from empty, which
   makes every texel stale; otherwise only the rows [*y0, *y1) changed since the last
   tagpu_huddraw_atlas_sent. */
#define HUD_ATLAS 2048
const unsigned char* tagpu_huddraw_atlas(unsigned* gen, int* y0, int* y1);
void                 tagpu_huddraw_atlas_sent(void);

#endif
