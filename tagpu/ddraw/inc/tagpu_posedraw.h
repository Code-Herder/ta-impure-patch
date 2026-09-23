#ifndef TAGPU_POSEDRAW_H
#define TAGPU_POSEDRAW_H
/* tagpu_posedraw.h — THE POSED PROGRAM.

   research/notes/gpu-posing.md §4 is the design. `tagpu_posebake.c` turns
   each `Model3DONode` template into a static geometry buffer and a per-owner
   material stream; this is the pass that draws from them. A unit is one draw
   out of its type's buffers, with its whole pose in a uniform block — instead
   of the ~2.75 MB of CPU-built vertices a per-frame stream re-uploads.

   A TWIN OF THE NATIVE PROGRAM, NOT A MODE SWITCH (§4, "[mine]"). The native
   program's vertex stage (tagpu_native.c `VS`) takes attributes already in
   frame-pixel space; no game pass draws it, and tools/tascene extracts it as
   the browser lab's unit program. A uniform switch would leave one path
   reading attributes the other's vertex input does not supply.

   THE FRAGMENT SHADER IS THE NATIVE PASS'S OWN: the `pose_unit` row of
   tools/spirv-gen.py's manifest pairs this vertex stage with
   `tagpu_native::FS` rather than a copy, so this program and the lab's cannot
   drift in the half of the pipeline this pass does not change. What the vertex
   shader takes over is tagpu_native.c's `emit_node`: the piece transform, the
   engine's projection, the depth key, the world x/z the fog samples, the
   model height the waterline clips on, and the shade quantised off the baked
   rest normal.

   ALL THREE RANGES. The vertex shader's `uRange` selects the body, the
   structure-shadow SLANT (its own projection, its own integer snap, its own
   per-piece `cached` rule) or the nanoframe WIRE (a LINE_LIST, the body
   projection, one notch nearer). What is still CPU-built is the selection
   lines and the effects models. This is the only unit renderer; there is no
   CPU unit emitter to fall back to (§7 step 8).

   THE SLANT IS THE ONE PLACE THE PORT CANNOT BE EXACT BY CONSTRUCTION
   (gpu-posing.md §5). Its snap is an arithmetic FLOOR of the posed 16.16
   value, so the 1-2 LSB our once-rounded float compose sits away from the
   engine's `fistp`-per-level chain becomes a WHOLE screen unit whenever a
   coordinate lands within 2/65536 of an integer — a shadow edge one pixel
   out, where the body would only flip a coverage sample. The shader therefore
   rounds the posed value onto the 16.16 grid before flooring it, which makes
   the port EXACT against the reconstruction (`recon_prim` rounds the same way)
   and leaves only the reconstruction's own residual against the engine. Gate D
   states the tolerance rather than claiming byte-exactness.

   THERE IS NO LEVER: this pass runs in play, unconditionally, because it is
   the only thing that draws a unit.

   RENDER THREAD ONLY: it is called from `tagpu_native_frame`, and
   tagpu_vk_unit.c draws from its hand-over (below). */

/* Everything the pass shares with tagpu_native.c's frame. */
typedef struct {
    float game[2];              /* game_width, game_height                    */
    float zoom, zoomC[2];       /* view zoom and its centre, game px          */
    float depthScale;           /* > every depth key in use this frame        */
    float ss;                   /* supersample factor (1 or 2)                */
    int   scafOn;
    float scafP[4];             /* vpL, vpT, vw, vh                           */
    float fogOrg[2], fogDim[2];
    int   lit;                  /* Classic++ on                               */
    float sun[3], amb, norm;    /* the units' sun, ambient, 1/unitLevel       */
    int   shNeutral, shDir;     /* the engine's SHD rows                      */
} TAGPU_PDVIEW;

typedef struct {
    /* the type's bake. Both must be non-NULL and from the same frame's
       tagpu_posebake_unit(); the pass reads the body range and nparts. */
    const void* geom;           /* const TAGPU_PBGEOM*                        */
    const void* mat;            /* const TAGPU_PBMAT*                         */
    /* one 4x3 row-major matrix (3 vec4) per piece, carrying a REST vertex to
       where this unit is holding that piece — `pose_accum_body`'s output with
       the body turn folded in. An all-zero matrix is a piece this unit is not
       showing, and collapses its triangles onto the model origin. */
    const float* pose;
    const unsigned char* shaded;/* per piece: emit_geom_at's `pieceShaded`    */
    /* per piece, the visibility WORD the slant and the wire read:
         0  the unit is not showing this piece at all (`P_FLAGS` bit 0 clear)
         1  showing it
         3  showing it AND the slant raster casts from it — bit 1 as well,
            `cached`, which a COB's dont-cache clears (a wind generator's mast
            and rotor)
       The slant's rule cannot be folded into the all-zero matrix the way the
       body's visibility is, because a piece with bit 1 clear still draws in
       the body range and needs its matrix there. The wire's is the same rule
       as the body's but cannot use the same MECHANISM: an all-zero matrix
       collapses a triangle to zero area, which provably produces no fragments,
       and a line to zero length, which does not. */
    const unsigned char* pvis;
    int   npose;
    float ax, ay;               /* frame-px anchor                            */
    float wx0, wz0;             /* world x and projected world z at the anchor*/
    float enc;                  /* depth key base                             */
    float alpha;                /* 0.5 while cloaked; the build ghost rides   */
                                /* the same blend at its own alpha            */
    int   ghost;                /* 1 = a build-ghost preview: it draws through*/
                                /* the same entry point but is NOT a unit: no */
                                /* shadow, no nanoframe wire, and the Vulkan  */
                                /* pass draws it in its own stage             */
    int   fog;                  /* uFog bits, as the native shader takes them */
    float waterT, digT;
    int   waterMode;
    int   nanoOn;
    float nanoT, nanoC[3];
    float nanoWire;             /* the wireframe's colour, idx/255 (the second*/
                                /* oscillator); read only while `nanoOn`      */
    float cast[3];              /* altitude, ground + throw, the length scale */
    int   castSkip;             /* out of the depth map (nanoframe, air drop) */
    /* THE CLASSIC HARD SHADOW THIS UNIT CASTS, decided by tagpu_native.c and
       carried rather than re-derived: the engine's option word, the unit-type
       bits and the Classic++ `shadows=` key are all engine or lever state, and
       a pass file may read none of them (tagpu_vk_pass.h). One field says both
       WHETHER and WHICH, because the two kinds are exclusive by the engine's
       own branch (0x459200: a structure takes the cached slant at 0x45A790,
       everything else the blackened composite at 0x45A470):

         TAGPU_PDSH_NONE   no shadow for this unit
         TAGPU_PDSH_SIL    the Classic SILHOUETTE -- the BODY range again,
                           shifted, with the fragment stage's uShadow on
         TAGPU_PDSH_SLANT  the structure SLANT -- the bake's own SLANT range

       `shOffY` is the shift onto the ground line under the unit, `gy - ay` in
       frame pixels, so an aircraft's shadow lands on the ground rather than
       under its hull. The x shift is the engine's constant 5 px (the blit's
       sx+0x85 against the body's sx+0x80) and is the consumer's. */
    int   shKind;
    float shOffY;
} TAGPU_PDUNIT;

#define TAGPU_PDSH_NONE   0
#define TAGPU_PDSH_SIL    1
#define TAGPU_PDSH_SLANT  2

/* Whether the pass has TRIED to arm and refused, as opposed to not having
   run yet. Safe to call from the GAME thread, where `owndraw`'s classifier
   reads it to log the refusal once: the render thread is the only writer. */
int  tagpu_posedraw_refused(void);

/* once per frame, before anything else. `frame_counter` is the fork's
   monotonic render-thread counter and stamps this frame's hand-over. */
void tagpu_posedraw_frame(unsigned frame_counter);

/* 0 when the pass cannot arm: a device whose `maxStorageBufferRange` will not
   hold one unit's pose at the piece ceiling, which is the whole of bring-up.
   There is no CPU emitter to leave those units to. A 0 ALSO MEANS "NO DEVICE
   YET" for the first frames — the state goes back to untried rather than
   latching a refusal — so a caller that latches on the first answer latches
   the wrong one. */
int  tagpu_posedraw_ready(void);

/* bodies: begin, then one call per unit, then end. It binds nothing and leaves
   nothing dirty — what the window does is open the RECORDING that becomes this
   frame's hand-over, so the bracket still has to be balanced. */
void tagpu_posedraw_begin(const TAGPU_PDVIEW* v);

/* THE GHOST'S OWN WINDOW, and it is a different entry point for two reasons the
   A/B depends on. The capture is never armed around it -- the Vulkan pass
   draws its ghosts in a different STAGE (after the effects) -- and the ghost
   window is not always the second: with no posed unit on screen
   `tagpu_native.c` skips the unit window entirely and this one is the FIRST,
   so the two cannot be told apart by counting windows. */
void tagpu_posedraw_begin_ghost(const TAGPU_PDVIEW* v);
void tagpu_posedraw_unit(const TAGPU_PDUNIT* u);
void tagpu_posedraw_end(void);

/* THE SHADOW, SLANT AND WIRE DRAWS HAVE NO ENTRY POINT HERE; they ride the
   records. `TAGPU_PDUREC.shKind` carries which of the engine's two shadow
   branches a unit takes and `shFirst`/`shCount` the range it draws, and
   tagpu_vk_unit.c records them as a stage of its own, stencil-masked, before
   the bodies, with two pipelines. `wireFirst`/`wireCount` carry the bake's
   WIRE range for a unit with `nanoOn`, `wire` its colour, and tagpu_vk_unit.c
   records them after the bodies through a LINE_LIST twin of the body
   pipeline.

   NO POSED UNIT CASTS INTO THE CAST-SHADOW MAP, and this is the one that looks
   done. tagpu_vk_unit.c builds a `pose_depth` pipeline from the VS + DFS in
   tagpu_posedraw.c and sets uDepthPass 1 for it, but NOTHING REACHES IT:

     s_depthOn                    only ever assigned 0 (tagpu_posedraw.c)
     -> TAGPU_PDUREC.casts        always 0
     -> TAGPU_PDHAND.depthOn      always 0
     -> TAGPU_PDHAND.castMat      the memcpy that never runs
     -> tagpu_vk_unit.c           w->casts always 0, so s_ncast never increments
     -> tagpu_vk_unit_cast        returns at `!s_ncast` every frame
     -> build_cast_pipeline       its ONLY caller is inside that function

   So the pipeline is never built. Nor is there a map to be missing from:
   tagpu_vk_shadow.c's `shadow_handover` has no producer and returns 0 on every
   frame, so its `otherCasters` census never runs either. */

/* ---- THE VULKAN PASS'S HAND-OVER ----------------------------------------

   tagpu_vk_unit.c draws the units from this and the bake. This is everything
   it is handed; it reads no engine state and re-derives nothing.

   NOTHING HERE IS A SECOND EVALUATION OF ANYTHING. The vertices are the two
   streams tagpu_posebake.h's mirrors carry; the ranges are the `first`/`count`
   pairs the bake lays down per range; the per-piece words are what
   `pose_words` converts, the single conversion there is.

   THE POSES LIVE IN THREE ARENAS, not in the records. A unit at the 256-piece
   ceiling would be 14 336 bytes, and stock's worst is 36 pieces, so each
   record names OFFSETS into `rows`, `flags` and `vis` -- arrays this module
   owns and grows -- and a unit costs what its model has. The arenas are laid
   out exactly as the shader's storage buffer (tagpu_posebake.h), so the
   Vulkan pass copies them whole, and the record's offsets become the shader's
   base indices unchanged. Offsets rather than pointers are also what lets an
   arena move when it grows without invalidating a record already written.

   IT IS VALID FOR THE FRAME THAT PUBLISHED IT AND NO OTHER, like every other
   hand-over in this tree, and here the reason is its own: `units`, `rows`,
   `flags` and `vis` are arrays this module REALLOCATES when a frame needs more
   room than the last, and the bake entries the records name are slots
   tagpu_posebake.c evicts and re-bakes. The frame stamp bounds the first; the
   SERIALS bound the second, and the mirror accessor checks them at the instant
   of the read rather than trusting that the eviction has not happened yet.

   `otherDraws` IS THE REFUSAL, AND IT IS NARROW ON PURPOSE. It counts the
   units THIS PASS DREW INSIDE THE PUBLISHED WINDOW that the hand-over does not
   carry -- a unit past TAGPU_PD_MAXHAND, a unit an arena would not grow for,
   and every unit of a frame whose packet was truncated -- and a non-zero count
   stands the Vulkan pass down. The build ghost is carried, not counted: it
   rides the same records with its own alpha and depth rule, and `ghost.on` is
   a play default, so counting it would stand the whole pass down for as long
   as a building placement is open. The A/B lever captures the Vulkan frame
   alone, for a comparison across builds.

   IT DOES NOT COUNT THE REST OF THE FRAME, and that is not an oversight:
   nothing else draws a unit (the native 3DO stream draws nothing, and there
   is no other unit renderer). THE SILHOUETTE, THE SLANT AND THE NANOFRAME WIRE ARE INSIDE
   IT: they are drawn from the very records this hand-over carries, out of
   the same bake, by the same consumer -- so a unit the hand-over drops loses its shadow with its body
   rather than leaving a shadow behind, which is what makes the refusal still
   whole.
   The cast-shadow MAP is where the rest of the frame would make a wrong
   picture rather than a partial one; see the depth chain above.

   RENDER THREAD ONLY, and published EARLIER in the same iteration of
   render_vk.c's loop -- by the native pass inside `tagpu_overlay_draw` --
   than the `tagpu_vk_frame` that consumes it. */

/* Records one frame can hand over: every one the producer can make, so no
   frame the packet carries is refused for count. A record is a unit-table
   slot (TAGPU_PK_MAX_UNITS), a wreck (TAGPU_PK_MAX_WRECKS), the placement
   ghost (1) or a queued build ghost (TAGPU_PK_MAX_BUILDS); tagpu_posedraw.c
   asserts the sum against tagpu_packet.h. NOTHING IS SIZED FROM IT -- the
   arenas here and the Vulkan pass's slot buffers grow to the frame's own
   count -- so it is a bound on a number handed between two files, which the
   Vulkan pass re-checks, not a budget. A frame past it hands nothing over and
   says so once. */
#define TAGPU_PD_MAXHAND (16384 + 4096 + 1 + 4096)

typedef struct TAGPU_PDUREC {
    /* the bake entries, and the serial each was baked under. The POINTER alone
       is not an identity: tagpu_posebake.c evicts the least recently used slot
       and bakes another type into it. tagpu_posebake_geom_mirror bounds the
       pointer and checks the serial, in that order. */
    const void* geom;                 /* const TAGPU_PBGEOM*                  */
    const void* mat;                  /* const TAGPU_PBMAT*                   */
    unsigned    geomSerial, matSerial;
    int   nvert;                      /* both streams carry this many         */
    int   first, count;               /* the BODY range this draw used        */
    /* THE CLASSIC HARD SHADOW, as TAGPU_PDUNIT carries it, with the range
       already resolved here so the consumer never has to know which of the
       bake's three ranges a kind means. `shCount == 0` is "no shadow", whatever
       `shKind` says -- a structure whose bake carries no slant triangles is the
       ordinary case of that, and it is expressed once, here, rather than as a
       second test at the draw. */
    int   shKind;                     /* TAGPU_PDSH_*                         */
    int   shFirst, shCount;           /* the range that kind draws            */
    float shOffY;                     /* the ground shift, frame px           */
    int   npose;                      /* pieces the pose arena carries        */
    unsigned rowOff;                  /* first of npose*3 vec4 in `rows`      */
    unsigned flagOff;                 /* first of npose floats in flags/vis   */
    /* the vertex stage's per-unit numbers */
    float anchor[4];                  /* ax, ay, world x, projected world z   */
    float enc, cast[3];
    /* the fragment stage's. uNanoT and uNanoC are STICKY: they are written
       only from a unit with `nanoOn`, so a unit without one carries whatever
       the last one that had it left, rather than zeros. Nothing reads them on
       a `uNanoOn == 0` unit. */
    float alpha, waterT, digT, nanoT, nanoC[3];
    int   fog, waterMode, nanoOn;
    /* THE NANOFRAME WIRE, resolved here like the shadow's range: the bake's
       WIRE range for a unit with `nanoOn`, and `wireCount == 0` for every
       other unit and for every ghost. `wire` is the colour, idx/255 -- the
       vertex stage's `uWire`. */
    int   wireFirst, wireCount;
    float wire;
    /* 1 = this unit casts into the cast-shadow depth map this frame:
       `!castSkip` on a frame the depth record is on. Always 0 -- see the depth
       chain above. Meaningless when `depthOn` below is 0. */
    int   casts;
    /* 1 = a BUILD GHOST rather than a unit. It is the same draw with the same
       uniforms; the only two differences are `alpha` (0.40, above) and depth
       writes -- ghosts blend with each other, units still occlude them -- so a
       consumer draws these with depth writes OFF and after the units, which
       is the order they are recorded in. `casts` is always 0 for one. */
    int   ghost;
} TAGPU_PDUREC;

typedef struct TAGPU_PDHAND {
    unsigned frame;                   /* the fork's render-thread counter     */

    const TAGPU_PDUREC* units; int nunit;
    const float* rows;  unsigned nrow;    /* vec4s, 3 per piece per unit      */
    const float* flags; const float* vis; unsigned nflag;

    /* THE VERTEX STAGE'S BLOCK, as `_begin` left it. Its std140 offsets are
       printed in inc/spirv/tagpu_posedraw.spv.h and are the contract for the
       buffer the Vulkan pass fills. uOffset is (0, 0) for a body draw, which
       is why it is not here. */
    float gw, gh, zoom, zoomCx, zoomCy, depthScale;
    float shd[2];                     /* uShd: shNeutral, shDir              */

    /* THE FRAGMENT STAGE'S, as `_begin` left it. uLambert IS PUBLISHED AS
       ZERO: tagpu_posedraw.c never sets it, so the Classic++ lambert lights a
       posed unit from a flat up normal rather than from vNrm. */
    int   restored, scafOn, lit, lambert, shadowOn;
    float scafP[4], ss;
    float fogOrgX, fogOrgY, fogCols, fogRows;
    float sun[3], amb, norm;
    /* the cast-shadow read-back block, and it is only meaningful while
       `shadowOn` is 1. `shadowOn` is the constant 0 and nothing writes the
       rest, so the whole block is published as zero (tagpu_posedraw.c).
       `shadowMat` goes into BOTH stages' blocks at the Vulkan end: the SPIR-V
       translation gives each stage its own copy of the uniform. */
    float shadowMat[16], shadowSun[3], shScale[3], penumbra, shade;

    /* THE DEPTH RECORD. `depthOn` MEANS "this pass's posed casters go into
       the cast-shadow map this frame", `castMat` is the matrix they use, and
       `ncast` is how many records carry `casts`.

       ALL THREE ARE PERMANENTLY ZERO AND HAVE NO PRODUCER; the full chain is
       in the NO POSED UNIT CASTS block above the hand-over. Read this field as "not yet",
       never as "no casters this frame" -- the two are indistinguishable here and only
       the first is true.

       `castMat` is published apart from `shadowMat` above on purpose: one is
       the map's projection and the other is the read-back's, and a frame
       could have one without the other. */
    int   depthOn, ncast;
    float castMat[16];

    /* THE TEXELS, as bytes. Each carries the serial that says when it last
       changed, so the Vulkan pass re-uploads on a change and not per frame.
       The unit atlas and the shade LUT are tagpu_render3do.h's mirrors; the
       palette is tagpu_pal's snapshot; the fog pair is what the native pass
       built this frame.

       THE CLASSIC++ RESTORED TWIN IS NOT HERE AS TEXELS: it crosses as the
       frame LIST below, which the consuming pass paints into its own twin on
       the device. */
    const unsigned char* atlas;   int atlasDim, atlasRows; unsigned atlasSerial;
    /* the anisotropy the restored twin is filtered at (0 = none). A consumer
       that cannot apply the same ratio draws different art wherever the
       texture is minified at an angle, so this is compared and not assumed.
       It is a property of the twin's SAMPLER rather than of the mirror, which
       is why it is published on the list path -- see `restoreDim` below. */
    float atlasRgbAniso;
    /* THE REQUEST, WHICH IS THE ONLY FORM THE TWIN COMES IN (the shape the
       feature and effects atlases use). The gather half publishes the LIST OF
       FRAMES to restore; the Vulkan pass paints them into its own twin.

       `restoreGen` is the only thing a cursor cannot survive: every
       discontinuity in the list -- arm, recycle, repack, palette move, an
       overflow restart -- bumps it, and a consumer whose generation moved
       starts at 0 again.
       `restoreRepaint` says the destination is to be recoloured in place
       rather than blanked, and `restoreBlanks` counts the resets that DID
       blank, which is how a consumer tells "recolour" from "start again"
       across a frame it did not see. The struct is declared by tag here
       because this header cannot include the one that defines it; the
       consumer includes both. */
    const struct TAGPU_RGLSL_FRAME_S* restoreFrames;
    int                               restoreN;
    unsigned                          restoreGen;
    int                               restoreRepaint;
    unsigned                          restoreBlanks;
    /* THE TWIN'S SHAPE: `restoreDim` is the twin's square size and
       `restoreMips` its top level, both from the atlas itself.
       `atlasRgbAniso` above comes from the same accessor for the same
       reason. */
    int                               restoreDim, restoreMips;
    const unsigned char* lut;     int lutW, lutH;          unsigned lutSerial;
    const unsigned char* pal;     unsigned palSerial;
    /* COPIED, not aliased: the grid points into a frame packet the game thread
       reuses, and tagpu_feat.c's own copy exists for the same reason. NULL when
       this frame had none, which a unit with `uFog & 1` makes a refusal. */
    const unsigned short* fogGrid; int fogGridCols, fogGridRows;
    const unsigned char*  fogLut;  /* 256 x R8 */

    /* the scissor the native pass set around these draws, in game-frame pixels
       from the TOP of the frame -- tagpu_vk_feat.c is where the flip onto
       Vulkan's framebuffer coordinates is done and argued */
    int   vpL, vpT, vw, vh, scissorOn, ss_i;

    /* WHAT THIS PASS DREW THAT THE HAND-OVER DOES NOT CARRY -- see the header
       above. Any non-zero value refuses the frame. */
    int   otherDraws;

    /* the A/B claim: nonzero on the ONE frame `tagpu_posedraw.ab` latched it,
       the frame the Vulkan pass captures. */
    int   ab;
} TAGPU_PDHAND;

/* 0 when nothing was drawn this frame, when this frame's hand-over has already
   been taken, or when the standing one was published on a different frame than
   `now`. Render thread only. */
int  tagpu_posedraw_handover(TAGPU_PDHAND* out, unsigned now);

/* ---- WHO PAINTED A STRUCTURE'S SLANT, REPORTED BY THE PAINTER --------------
   The structure-shadow gate in tagpu_native.c may only be raised on a frame
   something actually drew a slant (its comment says why at length: it is
   "observed, not predicted"), and the only code that knows is the consuming
   pass. So the consumer SAYS SO after recording its shadow stage, and the
   producer's next frame reads it and clears it.

   RENDER THREAD ONLY, and the two calls are one iteration of the render loop
   apart by construction: `tagpu_native_frame` publishes the hand-over and the
   seam's `tagpu_vk_frame` consumes it later in the SAME iteration, so the
   `_take` at the top of frame N+1 reads what the painter reported in frame N.
   That is the one-frame cost the gate's own comment already budgets for, in
   both directions.

   `_drew` is idempotent within a frame (the consumer may record more than one
   stage); `_take` reads and clears, so a frame that never reaches the painter
   lowers the gate on its own. */
void tagpu_posedraw_slant_drew(void);
int  tagpu_posedraw_slant_take(void);

/* THIS FRAME HAS UNITS THE HAND-OVER CANNOT CARRY, because the packet they
   came from was truncated: the frame's `otherDraws` becomes non-zero and the
   Vulkan pass draws no unit rather than some of them. Render thread, after
   `tagpu_posedraw_frame` and before the frame's last window closes. */
void tagpu_posedraw_uncarried(void);
#endif
