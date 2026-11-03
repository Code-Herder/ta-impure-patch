#ifndef TAGPU_TERR_H
#define TAGPU_TERR_H
/* Terrain pass — the 32x32 pre-rendered map tiles, the last layer the
   engine still paints inside the viewport.

   The engine's pass is 0x483FA0(OFFSCREEN* ctx): a flat grid blit of 8bpp
   tiles indexed by a u16 tile map, with no height, no LOS and no depth
   (research/notes/terrain-depth.md 2). This module reproduces it from the same
   two stores, into the world target, at a depth key BELOW every other
   band — terrain is the frame's implicit far plane.

   Because it covers the whole viewport it also inherits the fog overlay's
   solid black: where the engine would paint an unexplored cell black, terrain
   PAINTS black instead of discarding, since nothing is behind it.

   Armed by tagpu_terr.on (tokens: log, passive, over, key=N). `passive` emits
   nothing; `over` draws ours on top of the engine's own terrain without owning
   the draw (the pixel-parity A/B); the default owns it via tagpu_terrown.c. */
#include <stddef.h>
#include "tagpu_fx.h"
#include "tagpu_restoreglsl.h"  /* TAGPU_RGLSL_FRAME -- a CPU struct, no GL in it */

int  tagpu_terr_armed(unsigned frame_counter);   /* re-reads tagpu_terr.on (30f) */
/* size this frame's grid; returns its CELL count, columns times rows (0 =
   nothing to draw) -- one instanced quad each, which the vertex shader in
   tagpu_terr.c derives from the level's tile map */
int  tagpu_terr_gather(const TAGPU_FXVIEW* v);
/* GATHER AND HAND OVER: despite the name it draws nothing; it finishes the
   frame's hand-over and publishes it. */
void tagpu_terr_render(const TAGPU_FXVIEW* v);

/* THE WIDEST GATHER RECT ANY PASS IS ASKED FOR, in world px a side: the
   native pass clamps its zoomed rect to it before any pass sizes itself
   (tagpu_native.c), so every gather agrees on one rect and no rect is past
   what the terrain draw refuses below. */
#define TAGPU_GATHER_SPAN_MAX 65536
/* THE TERRAIN GRID'S SIDE AT THAT RECT, in 32-px cells: the gather's
   columns are ceil32(span + fx) with the eye's fraction fx under 32, at most
   span / 32 + 2. tagpu_vk_terr.c refuses a hand-over past it, which is the bound that
   keeps every column and row the vertex stage derives an exact integer in
   float (tagpu_terr.c, above the VS). */
#define TAGPU_TERR_MAX_SIDE (TAGPU_GATHER_SPAN_MAX / 32 + 2)

/* the palette index tagpu_terrown.c fills the viewport with in place of the
   engine's terrain blit; the composite treats every OTHER index in the engine's
   frame as "an overlay the engine still draws, and we must not cover it" */
int  tagpu_terr_key(void);

/* ---- the Vulkan edition of this pass
   ----------------------------------------------------------------------------

   Everything the gather decided for this pass, so that tagpu_vk_terr.c draws
   it without re-deriving any of it: the grid's rect and the uniforms are the
   numbers the gather computed, the texels are buffers this file owns (the
   atlas, the height grid, the level's tile map and the neighbourhood keys)
   and the engine's palette table, and the shader is tagpu_terr.c's GLSL
   through tools/spirv-gen.py. No per-cell record crosses: the vertex stage
   derives each cell's from the tile map and its instance index.

   HANDED OVER EXACTLY ONCE, like the scaffold's and the feature pass's, so one
   frame's geometry can never be drawn twice; a frame this pass skipped hands
   over nothing and the Vulkan pass draws no terrain.

   THE POINTERS ARE THIS FILE'S, AND THEY ARE VALID FOR THE FRAME THAT
   PUBLISHED THEM AND NO LONGER. Producer and consumer run on the RENDER THREAD
   and the whole of the native pass -- this one included -- happens earlier in
   the same iteration of render_vk.c's loop than the tagpu_vk_frame that
   consumes this, so the game thread never touches them and there is no lock to
   take.

   THAT IS NOT BY ITSELF ENOUGH. `ensure_atlas` FREES `s_atlasMirror`,
   `build_height` frees `s_hMirror`, `ensure_tilemap` replaces `s_tileMap`
   and the restore drop frees the keys whenever the map changes, so a
   hand-over left standing from an earlier frame names memory this file has
   given back -- and one can be left standing, because the flag is cleared
   inside `tagpu_terr_render`, which tagpu_native.c calls only when the gather
   returned cells. A frame that bails (a level load makes `ptr_ok(tmap)` fail,
   which is exactly when the atlas is rebuilt) never reaches it.

   So the hand-over carries the frame it was published on and
   `tagpu_terr_handover` REFUSES any other, which makes "these pointers are
   alive" a property of the frame number rather than of which functions
   happened to run. */

/* THE UNIT QUAD IS DEFINED ONCE, HERE: the two triangles whose shared edge
   runs (1,0)-(0,1), in the engine's own vertex order. It is the pass's fixed
   geometry, drawn once per cell of the grid as an instance. Its one user is
   tagpu_vk_terr.c; the macro stays here because the geometry is the pass's. */
#define TAGPU_TERR_QUAD  { 0.f,0.f, 1.f,0.f, 0.f,1.f, 1.f,0.f, 1.f,1.f, 0.f,1.f }
#define TAGPU_TERR_QUADV 6

typedef struct TAGPU_TERRHAND {
    /* THE FRAME THIS WAS PUBLISHED ON (the fork's monotonic render-thread
       counter). `tagpu_terr_handover` refuses a hand-over whose stamp is not
       the caller's own frame -- see the paragraph above for what that bounds.
       It is first so that a hand-over read by a debugger leads with it. */
    unsigned frame;

    /* THE GRID: `cols` x `rows` cells, one INSTANCE each, instance i being
       column i % cols and row i / cols. Grid cell (0,0) is map cell
       (tile0X, tile0Y); a cell past the engine's area (`areaW` x `areaH`
       32-px cells, tagpu_edge_area) is drawn as the cell it reflects to while
       `mirror` is 1 and not at all while it is 0, and a cell whose tile index
       is `atlasN` or past it is not drawn. tagpu_vk_terr.c refuses a grid
       side outside [1, TAGPU_TERR_MAX_SIDE] and an area outside the tile
       map. */
    int cols, rows;
    int tile0X, tile0Y;
    int areaW, areaH;
    int mirror;
    int atlasN;

    /* The vertex stage's uniform block (std140 offsets are printed in
       inc/spirv/tagpu_terr.spv.h and are the contract for the buffer). */
    float gw, gh;
    float zoom, zoomCx, zoomCy;
    float depthScale, enc;
    float origX, origY;       /* screen px of grid cell (0,0)'s corner  */
    float texelW, texelH;     /* 1/atlas width, 1/atlas height          */
    /* 1 when `keys` below holds this map's neighbourhood keys, and then the
       neighbourhood atlas's texel size: `nbFrames` below is the list those
       keys' cells are painted from. Whether binding 41 IS that atlas is the
       consumer's (uNbhd). */
    int   nbOn;
    float nbTexelW, nbTexelH;

    /* The fragment stage's. `restored` is the Classic++ restored-colour
       branch, `lambert` the `light=` half of the preset, `shadowOn` the
       cast-shadow one, `fog` the engine's overlay bit: the source it samples
       is the slot's fog image (tagpu_vk_fog.h), not a field of this record.

       `restored` SAYS A RESTORE REQUEST IS STANDING FOR THIS ATLAS -- it is
       the producer's `s_rFrames`, the published field itself.

       IT DOES NOT SET `uRestored` BY ITSELF, and a consumer author should read
       that here rather than assume otherwise. `tagpu_vk_terr.c` ANDs it with
       its OWN "this lane has the restored image, and something has painted at
       least one cell of it" -- and that pair decides both the uniform AND
       which view binding 41 names, so the flag and the descriptor cannot
       disagree.
       WHETHER THE PAINT HAS LANDED IS THE CONSUMER'S OWN FACT, it must keep
       it, and it must GATE ON IT: this flag is the request, not the result. A
       consumer that took it for the result would sample an image nothing has
       written and that has never left VK_IMAGE_LAYOUT_UNDEFINED. */
    int   restored, lambert, fog, shadowOn;
    /* THE REST OF THE CAST-SHADOW BLOCK, and it is only meaningful while
       `shadowOn` is 1. NOTHING PRODUCES IT: tagpu_terr.c publishes `shadowOn`
       0 and never writes these, so they reach the consumer as zero (see
       `terr_publish`). What they would carry: the matrix the shadow map was
       drawn with, the sun the knobs name, uShScale = (texel, depth span,
       1/res), and the two shading scalars. */
    float shadowMat[16], shadowSun[3], shScale[3], penumbra, shade;
    float hDimW, hDimH;       /* uHDim: 0 while there is no usable grid */
    float sun[3], amb, norm;
    /* uMapPx: the engine's area on the tile grid (tagpu_edge_area), its 32-px
       cell count times 32 -- where the mirrored cells reflect and the edge tone
       fades from (TAGPU_GLSL_EDGE_FN). Published whether or not a cell is
       mirrored. */
    float mapPxW, mapPxH;

    /* The texels, as bytes: the Vulkan pass uploads them into images of its
       own. Each carries the serial that says when it last
       changed, so the Vulkan lane re-uploads on a change and not per frame.
       The atlas and the height grid are built ONCE PER MAP and their buffers
       are RETAINED rather than freed (tagpu_terr.c `s_mirrorWant`), which is
       this pass's whole answer to the texel problem: the mirror is the very
       buffer the build loop filled, so it is correct from the instant it
       exists and nothing writes it again. */
    const unsigned char* atlas;       /* atlasW x atlasH palette indices, the
                                         source the Vulkan lane expands its
                                         base atlas from (tagpu_pal_expand) */
    int                  atlasW, atlasH;
    unsigned             atlasSerial;
    /* THE WORK ITSELF, for the lane that restores on its own -- the only route
       there is.

       WHY THE LIST AND NOT JUST "RESTORE IT": three of a frame's eleven
       numbers are content-dependent -- `wrap` is tagpu_rglsl_tileable() over
       the tile's own texels against the ART palette, and the ORDER is the
       centre-out rank over the live tile map. Both are engine-memory reads, so
       both belong on this side of the hand-over; what crosses is their result.
       `restoreFrames` therefore points at the list `restore_publish` built,
       in the order it built it.

       LIFETIME: the frame list is retained for the map, not for the frame, but
       a consumer must still copy on the frame it takes it (as
       tagpu_vk_restore_job_add does) -- a new map frees it.
       AND BOTH ENDS ARE THE SAME THREAD, which is what makes that safe rather
       than merely likely. The producer runs inside `tagpu_overlay_draw`
       (render_vk.c) and the consumer inside `tagpu_vk_frame`, in that order, in
       ONE iteration of the render loop -- so the pointer cannot be freed
       between publication and the copy. Do not reason about this field as a
       cross-thread hand-over; it is not one, and neither are `atlas` and
       `height` above. `restoreSerial`
       changes whenever the list or its destination does. There is no repaint:
       the restore reads the base atlas, built from the engine's table, and the
       Gamma factor is applied after it, to the finished world image
       (tagpu_pal.h). The destination's size is the ATLAS's (`atlasW` x `atlasH`):
       terrain restores the whole atlas, which is why there is no row count
       here as there is for the mirror. */
    const TAGPU_RGLSL_FRAME* restoreFrames;
    int                      restoreN;
    unsigned                 restoreSerial;
    /* THE NEIGHBOURHOODS, the same request in the seam-correct form
       (research/notes/compute-restorer.md D5): one frame per key, IN KEY
       ORDER -- frame k paints key k's 34-texel cell, column k % nbCols and row
       k / nbCols of an `nbW` x `nbH` atlas, from the base atlas -- and
       `nbOrder`, the keys in the centre-out order. A map cell's key is `keys`
       at that cell -- a cell past the area takes the key of the cell it
       reflects to -- and is therefore its frame's index.
       Published with `restoreFrames` and under the same serial and lifetime;
       NULL when the map's keys could not be built or do not fit one image of
       the device's size, and then `restoreFrames` is the whole request.
       Whether the device has the memory for it is the consumer's to decide
       (D10), and the per-tile list is its fallback. */
    const TAGPU_RNBFRAME*    nbFrames;
    const int*               nbOrder;
    int                      nbN, nbCols, nbW, nbH;
    /* THE LEVEL'S TILE MAP, `mapW` x `mapH` u16 tile indices (the engine's
       own, copied once a level: tagpu_terr.c `ensure_tilemap`), and the
       neighbourhood key of each of its cells, u32 on the same grid, NULL
       while `nbOn` is 0. Each serial moves when its buffer is replaced, so
       the Vulkan lane uploads each once a level and not per frame. */
    const unsigned short* tileMap;
    int                   mapW, mapH;
    unsigned              mapSerial;
    const unsigned*       keys;
    unsigned              keySerial;
    const unsigned char* height;      /* hW x hH R8, or NULL                */
    int                  hW, hH;
    unsigned             heightSerial;
    const unsigned char* pal;         /* 256 x RGBA8, tagpu_pal_engine()    */
    unsigned             palSerial;   /* tagpu_pal_engine_serial()          */

    /* THE SCISSOR THE NATIVE PASS SET AROUND THIS DRAW, in game-frame pixels
       measured from the TOP of the frame -- the engine's own viewport rect.
       The Vulkan draw must be clipped to it, and the two coordinate systems
       disagree about which way y runs: see tagpu_vk_feat.c, where the flip is
       done and argued. */
    int   vpL, vpT, vw, vh;
    int   scissorOn;                  /* the native pass has the clip on     */
    int   ss;                         /* the world's supersample factor      */

    /* 1 on the ONE frame `tagpu_terr.ab` latched its claim and
       `tagpu_vk_ab_arm` got the `_vk.ppm` target unlinked, so the Vulkan lane
       captures THAT frame rather than whichever one its own lever poll landed
       on. It does NOT mean a capture file was written -- the seam writes it
       after the draw. */
    int   ab;
} TAGPU_TERRHAND;

/* 0 when there is nothing to draw, when this frame's has already been taken,
   or when the standing hand-over was published on a DIFFERENT frame than
   `now` -- the fork's monotonic render-thread counter, which a Vulkan pass has
   as TAGPU_VKPASS::frame. That last refusal is the safety one: the pointers
   in here alias buffers this file frees and rebuilds, so a hand-over that
   outlived its frame can name memory that is gone. Render thread only. */
int tagpu_terr_handover(TAGPU_TERRHAND* out, unsigned now);
#endif
