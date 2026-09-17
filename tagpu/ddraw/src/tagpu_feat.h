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

/* ---- the Vulkan edition of this pass (Phase G / G19e, the second world pass)
   ----------------------------------------------------------------------------

   Everything the GL lane just drew this pass FROM, so that the Vulkan lane
   draws the same thing rather than a second implementation of it. Nothing here
   is re-derived: the vertices are the two arrays the gather filled and the GL
   upload took, the uniforms are the numbers the GL draw passed, the texels are
   the bytes each texture was uploaded from, and the shader is the same GLSL
   through tools/spirv-gen.py.

   HANDED OVER EXACTLY ONCE, like the scaffold's, so one frame's geometry can
   never be drawn twice; a frame this pass skipped hands over nothing and the
   Vulkan lane draws nothing, which is what the GL lane did.

   THE POINTERS ARE THIS FILE'S AND THE ATLAS MODULE'S, and they are valid until
   the next frame rebuilds them. That is safe for one reason worth naming: both
   lanes run on the RENDER THREAD, and the whole of the native pass -- this one
   included -- happens earlier in the same iteration of render_ogl.c's loop than
   the tagpu_vk_frame that consumes this. The game thread never touches them. */

/* FLOATS PER VERTEX, AND THE ATTRIBUTE TABLE, DEFINED ONCE FOR BOTH LANES.
   A vertex layout copied into a second file is two things that can drift, and
   the whole worth of a 0-px comparison is that only the rasteriser differs.
   Each entry is {location, components, byte offset}; the GL VAO and the Vulkan
   VkVertexInputAttributeDescription array are both built from it. */
#define TAGPU_FEAT_VST    10       /* x,y,enc, u,v, ck,mode, wx,wz, lam */
#define TAGPU_FEAT_NATTR  5
#define TAGPU_FEAT_ATTRS  { {0,3,0}, {1,2,12}, {2,2,20}, {3,2,28}, {4,1,36} }

typedef struct TAGPU_FEATHAND {
    /* THE FRAME THIS WAS PUBLISHED ON. `tagpu_feat_handover` refuses any other
       -- see there, and tagpu_terr.h for the failure it bounds. */
    unsigned frame;

    /* The geometry, in the GL lane's own two buckets and its own draw order:
       shadows first (they test depth and never write it), bodies second. */
    const float* shadow;  int nShadow;     /* vertices, TAGPU_FEAT_VST floats each */
    const float* body;    int nBody;

    /* The vertex stage's uniform block (std140 offsets are printed in
       inc/spirv/tagpu_feat.spv.h and are the contract for the buffer). */
    float gw, gh;
    float zoom, zoomCx, zoomCy;
    float depthScale;

    /* The fragment stage's. `restored` and `lit` are the two Classic++
       branches; `fog` is the engine's overlay bit. */
    int   restored, lit, fog;
    float fogOrgX, fogOrgY, fogCols, fogRows;

    /* The texels, as bytes rather than as GL names -- a second backend cannot
       read a GL texture. Each carries the serial that says when it last
       changed, so the Vulkan lane re-uploads on a change and not per frame. */
    const unsigned char*  atlas;      /* dim x dim R8, tagpu_gaf.c's mirror  */
    int                   atlasDim;
    /* THE ROWS IN USE, which is what a second backend needs to upload and is
       usually a quarter of the page. The shelf packer never places a cell
       below `shelfY + shelfH`, so every texel any of these vertices can name
       is above this line -- and uploading 4 MB when 1 is live is a cost paid
       every time a feature frame is added to the atlas. */
    int                   atlasRows;
    unsigned              atlasSerial;

    /* CLASSIC++'s RESTORED TWIN, MIRRORED (the Vulkan-only plan's gate 2).
       NULL until the restorer has painted something and the read-back has run,
       so `restored` being 1 with this NULL is the case that existed before this
       field and a consumer must stand down there exactly as it did then.

       `atlasRgbRows` is the READ-BACK's own high-water mark and NOT the shelf
       cursor `atlasRows` above: tagpu_gaf.c reads back what the restorer has
       painted, and the restorer is sliced across frames, so it lags the shelf
       by design. A consumer uploads these rows and treats everything below them
       as alpha 0 -- which is what an unpainted cell reads as anyway, so the
       progressive reveal works on this side with no extra flag. */
    const unsigned char*  atlasRgb;      /* atlasDim x atlasRgbRows RGBA8     */
    int                   atlasRgbRows;
    unsigned              atlasRgbSerial;
    const unsigned char*  pal;        /* 256 x RGBA8, tagpu_pal_live()       */
    unsigned              palSerial;
    const unsigned short* fogGrid;    /* cols x rows RG8; NULL when fog is off */
    int                   fogGridCols, fogGridRows;
    const unsigned char*  fogLut;     /* 256 x R8, tagpu_native_foglut()     */

    /* THE SCISSOR THE NATIVE PASS SET AROUND THIS DRAW, in game-frame pixels
       measured from the TOP of the frame -- the engine's own viewport rect.
       The GL lane is clipped to it and so must the Vulkan one be, and the two
       coordinate systems disagree about which way y runs: see
       tagpu_vk_feat.c, where the flip is done and argued. */
    int   vpL, vpT, vw, vh;
    int   scissorOn;                  /* the GL lane actually enabled it     */
    int   ss;                         /* the FBO's supersample factor        */

    /* 1 on the ONE frame this pass captured `tagpu_feat_gl.ppm` under
       `tagpu_feat.ab`, so the Vulkan lane captures the SAME frame rather than
       whichever one its own lever poll landed on. */
    int   ab;
} TAGPU_FEATHAND;

/* 0 when there is nothing to draw, when this frame's has already been taken,
   or when the standing hand-over was published on a DIFFERENT frame than
   `now` -- the fork's monotonic render-thread counter, which a Vulkan pass has
   as TAGPU_VKPASS::frame. That last refusal is the safety one: the pointers
   in here alias buffers this file frees and rebuilds, so a hand-over that
   outlived its frame can name memory that is gone. Render thread only. */
int tagpu_feat_handover(TAGPU_FEATHAND* out, unsigned now);
#endif
