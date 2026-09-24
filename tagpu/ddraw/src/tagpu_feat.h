#ifndef TAGPU_FEAT_H
#define TAGPU_FEAT_H
/* Feature pass — trees, rocks, metal patches, splats and GAF
   wreckage: the last colour-keyed sprites the engine blits into the 8bpp
   frame besides the terrain itself.

   The engine draws them in two places (research/notes/terrain-depth.md §3,
   research/notes/features.md): a flat pre-pass over the whole sweep rect
   before any unit (def Height < 10), then, per 16-px map row of the
   interleaved sweep, the tall ones after that row's units. Both call the one
   leaf 0x46A610(ctx, tile, tileX, tileY).

   This module reproduces both from the same FeatureMap walk and renders the
   frames natively with DEPTH WRITES ON, so a tall feature occludes units
   through the real depth buffer. Armed by tagpu_feat.on (tokens log, passive, noflat, notall,
   noshadow, nowreck). Rides the native pass's per-frame view. */
#include "tagpu_fx.h"
#include "tagpu_restoreglsl.h"   /* TAGPU_RGLSL_FRAME, the restore request */

int  tagpu_feat_armed(unsigned frame_counter);   /* re-reads tagpu_feat.on (30f) */
/* walk the sweep rect and build this frame's quads; returns the vertex count
   built (0 = nothing to draw, which is all the caller uses it for) */
int  tagpu_feat_gather(const TAGPU_FXVIEW* v);
/* draws nothing: latches this frame's `tagpu_feat.ab` claim and publishes the
   gather for the Vulkan pass (tagpu_feat_handover below), which draws the
   shadows without depth writes and the bodies with. */
void tagpu_feat_render(const TAGPU_FXVIEW* v);

/* ---- the Vulkan edition of this pass (the second world pass)
   ----------------------------------------------------------------------------

   Everything this pass built this frame, so that tagpu_vk_feat.c draws it
   rather than a second implementation of it. Nothing here is re-derived: the
   vertices are the two arrays the gather filled, the uniforms are the view's
   numbers, the texels are the CPU-side bytes (the atlas's mirror and the
   tables below), and the shader is tagpu_feat.c's GLSL through
   tools/spirv-gen.py.

   HANDED OVER EXACTLY ONCE, like the scaffold's, so one frame's geometry can
   never be drawn twice; a frame this pass skipped hands over nothing and the
   Vulkan pass draws nothing.

   THE POINTERS ARE THIS FILE'S AND THE ATLAS MODULE'S, and they are valid until
   the next frame rebuilds them. That is safe for one reason worth naming: the
   gather and the Vulkan pass both run on the RENDER THREAD, and the whole of
   the native pass -- this one included -- happens earlier in the same
   iteration of render_vk.c's loop than the tagpu_vk_frame that consumes this.
   The game thread never touches them. */

/* FLOATS PER VERTEX, AND THE ATTRIBUTE TABLE, DEFINED ONCE: the gather that
   writes the vertices and the pass that reads them share this header, and a
   vertex layout copied into a second file is two things that can drift. Each
   entry is {location, components, byte offset}; tagpu_vk_feat.c builds its
   VkVertexInputAttributeDescription array from it. */
#define TAGPU_FEAT_VST    10       /* x,y,enc, u,v, ck,mode, wx,wz, lam */
#define TAGPU_FEAT_NATTR  5
#define TAGPU_FEAT_ATTRS  { {0,3,0}, {1,2,12}, {2,2,20}, {3,2,28}, {4,1,36} }

typedef struct TAGPU_FEATHAND {
    /* THE FRAME THIS WAS PUBLISHED ON. `tagpu_feat_handover` refuses any other
       -- see there, and tagpu_terr.h for the failure it bounds. */
    unsigned frame;

    /* The geometry, in the gather's two buckets and in draw order: shadows
       first (they test depth and never write it), bodies second. */
    const float* shadow;  int nShadow;     /* vertices, TAGPU_FEAT_VST floats each */
    const float* body;    int nBody;

    /* The vertex stage's uniform block (std140 offsets are printed in
       inc/spirv/tagpu_feat.spv.h and are the contract for the buffer). */
    float gw, gh;
    float zoom, zoomCx, zoomCy;
    float depthScale;

    /* The fragment stage's. `restored` is the Classic++ restored-colour
       branch; `fog` is the engine's overlay bit. */
    int   restored, fog;
    float fogOrgX, fogOrgY, fogCols, fogRows;

    /* The texels, as CPU-side bytes. Each carries the serial that says when
       it last changed, so the Vulkan pass re-sends on a change and not per
       frame. */
    const unsigned char*  atlas;      /* dim x dim palette indices,
                                         tagpu_gaf.c's mirror: the source the
                                         Vulkan pass expands its base atlas
                                         from (tagpu_pal_expand)             */
    int                   atlasDim;
    /* THE ROWS IN USE, which is what a second backend needs to upload and is
       usually a quarter of the page. The shelf packer never places a cell
       below `shelfY + shelfH`, so every texel any of these vertices can name
       is above this line -- and expanding and sending 16 MB when 4 are live is
       a cost paid every time the whole page has to go. */
    int                   atlasRows;
    unsigned              atlasSerial;
    /* THE BASE ATLAS'S TWO OTHER INPUTS, beside the indices: the key plane
       (dim x dim, 0 at a keyed texel -- tagpu_gaf.h `keym`) and the dirty map,
       the serial of the last write to each tile, which tells a consumer
       holding serial S which tiles to re-send (tagpu_gaf_dirty_since). Both
       are the atlas's own buffers, alive as long as the atlas is. */
    const unsigned char*        atlasKey;
    const unsigned*             atlasDirty;
    /* THE LAST REPACK'S MOVES (tagpu_gaf.h `moves`): the cells a copy as of
       a serial in [atlasMovePrev, atlasMoveSerial) carries to their new place
       on the device instead of taking the page again. The atlas's own array,
       alive as long as the atlas is; NULL until a repack has recorded one. */
    const struct TAGPU_GAFMOVE* atlasMoves;
    int                         atlasMoveN;
    unsigned                    atlasMoveSerial, atlasMovePrev;

    /* ...AND THE WORK ITSELF IS THE ONLY FORM IT COMES IN. There is no
       restored picture on the CPU side: the restored copy of the atlas reaches
       a consumer as the list below and is painted on the device.

       IT IS AN APPEND-ONLY LIST WITH A CURSOR, not terrain's whole list per
       serial, because a feature atlas is a lazy QUEUE: tagpu_gaf.c adds one
       frame on every miss for the life of the atlas. A consumer keeps its own
       index into `restoreFrames` and takes `[cursor, restoreN)`; a frame on
       which it takes nothing costs nothing, because the entries are still
       there on the next one. `restoreGen` is the discontinuity a cursor cannot
       survive -- the arm, an overflow restart, a recycle, a repack, the atlas
       being laid out afresh -- and a consumer that sees a new one drops its
       job and starts at 0. Every generation blanks the destination: the
       restore reads the base atlas, built from the engine's table, and the
       Gamma factor is applied after it, to the finished world image
       (tagpu_pal.h), so no generation is a recolour of the last.

       LIFETIME: the array is the atlas's, retained for the atlas rather than
       for the frame, but a consumer still copies on the frame it takes it (as
       tagpu_vk_restore_job_add does) -- a restart re-uses the same memory.
       The destination's size is the ATLAS's (`atlasDim` square). */
    const TAGPU_RGLSL_FRAME* restoreFrames;
    int                      restoreN;
    unsigned                 restoreGen;
    const unsigned char*  pal;        /* 256 x RGBA8, tagpu_pal_engine()     */
    unsigned              palSerial;  /* tagpu_pal_engine_serial()           */
    const unsigned short* fogGrid;    /* cols x rows RG8; NULL when fog is off */
    int                   fogGridCols, fogGridRows;

    /* THE SCISSOR THE NATIVE PASS SET AROUND THIS DRAW, in game-frame pixels
       measured from the TOP of the frame -- the engine's own viewport rect.
       The Vulkan pass is clipped to it when `scissorOn` says so, and the two
       coordinate systems disagree about which way y runs: see
       tagpu_vk_feat.c, where the flip is done and argued. */
    int   vpL, vpT, vw, vh;
    int   scissorOn;                  /* the native pass enabled the clip    */
    int   ss;                         /* the world target's supersampling   */

    /* 1 on the ONE frame `tagpu_feat.ab` latched its claim and
       `tagpu_vk_ab_arm` got the `_vk.ppm` target unlinked, so the Vulkan lane
       captures THAT frame rather than whichever one its own lever poll landed
       on. It does NOT mean a capture file was written: the Vulkan pass
       writes it, and only when it has a target to read. */
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
