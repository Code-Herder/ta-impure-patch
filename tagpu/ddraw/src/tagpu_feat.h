#ifndef TAGPU_FEAT_H
#define TAGPU_FEAT_H
/* Feature pass (G13a) — trees, rocks, metal patches, splats and GAF
   wreckage: the last colour-keyed sprites the engine blits into the 8bpp
   frame besides the terrain itself.

   The engine draws them in two places (research/notes/terrain-depth.md §3,
   research/notes/features.md): a flat pre-pass over the whole sweep rect
   before any unit (def Height < 10), then, per 16-px map row of the
   interleaved sweep, the tall ones after that row's units. Both call the one
   leaf 0x46A610(ctx, tile, tileX, tileY).

   This module reproduces both from the same FeatureMap walk and renders the
   frames natively with DEPTH WRITES ON, so a tall feature occludes units
   through the real depth buffer — which is what retires the G12a scene-depth
   scaffold. Armed by tagpu_feat.on (tokens log, passive, noflat, notall,
   noshadow, nowreck). Rides the native pass's per-frame view. */
#include "tagpu_fx.h"

int  tagpu_feat_armed(unsigned frame_counter);   /* re-reads tagpu_feat.on (30f) */
int  tagpu_feat_on(void);                        /* armed state, no re-read      */
/* walk the sweep rect and build this frame's quads; returns the vertex count
   built (0 = nothing to draw, which is all the caller uses it for) */
int  tagpu_feat_gather(const TAGPU_FXVIEW* v);
/* draw into the currently bound FBO (depth test on; shadows without depth
   writes, bodies with). Uses its own program/VAO and leaves the program,
   VAO and texture bindings dirty. */
void tagpu_feat_render(const TAGPU_FXVIEW* v, unsigned int palTex);
void tagpu_feat_glreset(void);
#endif
