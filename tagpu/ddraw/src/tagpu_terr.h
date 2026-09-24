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
/* build this frame's quads; returns the CELL count (0 = nothing to draw) —
   one instanced quad each, see the vertex shader in tagpu_terr.c */
int  tagpu_terr_gather(const TAGPU_FXVIEW* v);
/* GATHER AND HAND OVER: despite the name it draws nothing; it finishes the
   frame's hand-over and publishes it. */
void tagpu_terr_render(const TAGPU_FXVIEW* v);

/* RESERVE FOR THIS VIEWPORT, then trim a would-be gather rect (game px) to
   what this pass can actually draw in one frame. Called once a frame by
   tagpu_native.c before any pass sizes itself, so every pass gathers over one
   rect they all agree on.

   THE BUDGET IS THE SCREEN, not a constant. `vw`/`vh` are the true 1x world
   viewport, and the widest rect any zoom can ask for is that viewport at
   TAGPU_ZOOM_MIN — so the staging is reserved for exactly that and grows when
   the player changes resolution. Nothing here names a resolution, and no
   screen is a special case: MEASURED, 1024x768 reserves 83 KB, 2560x1440
   423 KB, 3840x2160 972 KB and 5120x2880 1746 KB, and each draws its whole
   view at the zoom floor.

   The trim fires only if the reservation could not be met — an allocation that
   failed, or a viewport so large the module's own memory guard refuses it. A
   gather that exceeds the staging BAILS, which hands the draw back for a frame
   and flashes, and a flash is the one failure that reads as a bug; a black
   margin is the honest degradation instead.

   THE TRIM HAS A FLOOR THE CALLER PUTS BACK, and it is worth knowing which
   failure that leaves. tagpu_native.c raises the rect to the viewport again
   right after this call (`if (evw < vw) evw = vw`) because a rect narrower
   than the screen would cull terrain that is plainly on it -- choosing a
   visible hole over a flash. So the degradation above is real only while the
   trim stays above the viewport, which is every case the memory ceiling can
   produce: at the 16384-px viewport gate the reserve asks for ~65600 px and
   the ceiling cuts it to ~56000, still far wider than the screen. Below the
   viewport is reachable only if `realloc` itself fails, and there the gather
   does bail and flash. Sizing the ceiling so it can never cross the viewport
   is what keeps that path unreachable; do not lower it without re-checking
   this.

   A fixed budget does not work: 32768 cells, sized for 1024x768 and
   1920x1080, is exceeded by an ordinary 3840x2160 desktop at any zoom below
   about 0.5x, and the rect is then cut to roughly a third of the width the
   view shows — terrain, units, features and markers all stopping at a black
   margin that a player reads as the map failing to draw at the edges. */
void tagpu_terr_clamp_span(int vw, int vh, int* w, int* h);

/* the palette index tagpu_terrown.c fills the viewport with in place of the
   engine's terrain blit; the composite treats every OTHER index in the engine's
   frame as "an overlay the engine still draws, and we must not cover it" */
int  tagpu_terr_key(void);

/* ---- the Vulkan edition of this pass
   ----------------------------------------------------------------------------

   Everything the gather built for this pass, so that tagpu_vk_terr.c draws it
   without re-deriving any of it: the instances are the array the gather
   filled, the uniforms are the numbers the gather computed, the texels are
   buffers this file owns (the atlas, the height grid, the fog copy) and the
   engine's palette table, and the shader is tagpu_terr.c's GLSL through
   tools/spirv-gen.py.

   HANDED OVER EXACTLY ONCE, like the scaffold's and the feature pass's, so one
   frame's geometry can never be drawn twice; a frame this pass skipped hands
   over nothing and the Vulkan pass draws no terrain.

   THE POINTERS ARE THIS FILE'S, AND THEY ARE VALID FOR THE FRAME THAT
   PUBLISHED THEM AND NO LONGER. Producer and consumer run on the RENDER THREAD
   and the whole of the native pass -- this one included -- happens earlier in
   the same iteration of render_vk.c's loop than the tagpu_vk_frame that
   consumes this, so the game thread never touches them and there is no lock to
   take.

   THAT IS NOT BY ITSELF ENOUGH. `ensure_atlas` FREES `s_atlasMirror`
   and `build_height` frees `s_hMirror` whenever the map changes, so a
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
   geometry -- what varies is the per-INSTANCE cell record below.
   Its one user is tagpu_vk_terr.c; the macro stays here because the geometry
   is the pass's, stated once where the record it pairs with is stated. */
#define TAGPU_TERR_QUAD  { 0.f,0.f, 1.f,0.f, 0.f,1.f, 1.f,0.f, 1.f,1.f, 0.f,1.f }
#define TAGPU_TERR_QUADV 6
/* SHORTS PER INSTANCE: the cell's column and row in this frame's gather grid,
   then its tile's column and row in the atlas. The vertex shader rebuilds the
   quad's position, world point and UVs from those four and the uniforms; see
   the VS in tagpu_terr.c for why every term is exact in float.
   Unnormalised shorts, so the Vulkan vertex format is SSCALED and not SINT
   -- the shader's attribute is a `vec4`, and SINT would need an `ivec4`.
   tagpu_vk_terr.c asks the device for that format rather than assuming it. */
#define TAGPU_TERR_ICOMP 4

typedef struct TAGPU_TERRHAND {
    /* THE FRAME THIS WAS PUBLISHED ON (the fork's monotonic render-thread
       counter). `tagpu_terr_handover` refuses a hand-over whose stamp is not
       the caller's own frame -- see the paragraph above for what that bounds.
       It is first so that a hand-over read by a debugger leads with it. */
    unsigned frame;

    /* The geometry: one record per visible cell, one INSTANCE each. */
    const short* cells;  int ncell;

    /* The vertex stage's uniform block (std140 offsets are printed in
       inc/spirv/tagpu_terr.spv.h and are the contract for the buffer). */
    float gw, gh;
    float zoom, zoomCx, zoomCy;
    float depthScale, enc;
    float origX, origY;       /* screen px of grid cell (0,0)'s corner  */
    float tile0X, tile0Y;     /* the map cell grid cell (0,0) IS        */
    float texelW, texelH;     /* 1/atlas width, 1/atlas height          */

    /* The fragment stage's. `restored` is the Classic++ restored-colour
       branch, `lit` the full-colour path both presets take (0 only under
       tagpu_classicpp_index), `shadowOn` the cast-shadow one, `fog` the
       engine's overlay bit.

       `restored` SAYS A RESTORE REQUEST IS STANDING FOR THIS ATLAS -- it is
       the producer's `s_rFrames`, the published field itself.

       IT DOES NOT SET `uRestored` BY ITSELF, and a consumer author should read
       that here rather than assume otherwise. `tagpu_vk_terr.c` ANDs it with
       its OWN `s_rgbAtlas.view && .have` -- "this lane has an image, and
       something has painted at least one cell of it" -- and that pair decides
       both the uniform AND which view binding 42 names, so the flag and the
       descriptor cannot disagree.
       WHETHER THE PAINT HAS LANDED IS THE CONSUMER'S OWN FACT, it must keep
       it, and it must GATE ON IT: this flag is the request, not the result. A
       consumer that took it for the result would sample an image nothing has
       written and that has never left VK_IMAGE_LAYOUT_UNDEFINED. */
    int   restored, lit, lambert, fog, shadowOn;
    /* THE REST OF THE CAST-SHADOW BLOCK, and it is only meaningful while
       `shadowOn` is 1. NOTHING PRODUCES IT: tagpu_terr.c publishes `shadowOn`
       0 and never writes these, so they reach the consumer as zero (see
       `terr_publish`). What they would carry: the matrix the shadow map was
       drawn with, the sun the knobs name, uShScale = (texel, depth span,
       1/res), and the two shading scalars. */
    float shadowMat[16], shadowSun[3], shScale[3], penumbra, shade;
    float fogOrgX, fogOrgY, fogCols, fogRows;
    float hDimW, hDimH;       /* uHDim: 0 while there is no usable grid */
    float sun[3], amb, norm;

    /* The texels, as bytes: the Vulkan pass uploads them into images of its
       own. Each carries the serial that says when it last
       changed, so the Vulkan lane re-uploads on a change and not per frame.
       The atlas and the height grid are built ONCE PER MAP and their buffers
       are RETAINED rather than freed (tagpu_terr.c `s_mirrorWant`), which is
       this pass's whole answer to the texel problem: the mirror is the very
       buffer the build loop filled, so it is correct from the instant it
       exists and nothing writes it again. */
    const unsigned char* atlas;       /* atlasW x atlasH R8                 */
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
    const unsigned char* height;      /* hW x hH R8, or NULL                */
    int                  hW, hH;
    unsigned             heightSerial;
    const unsigned char* pal;         /* 256 x RGBA8, tagpu_pal_engine()    */
    unsigned             palSerial;   /* tagpu_pal_engine_serial()          */
    const unsigned short* fogGrid;    /* cols x rows RG8; NULL when fog off */
    int                  fogGridCols, fogGridRows;
    const unsigned char* fogLut;      /* 256 x R8, tagpu_native_foglut()    */

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
