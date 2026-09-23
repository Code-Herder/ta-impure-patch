#ifndef TAGPU_POSEDRAW_H
#define TAGPU_POSEDRAW_H
/* tagpu_posedraw.h — THE POSED PROGRAM (G16 step 5).

   research/notes/gpu-posing.md §4 is the design. Step 4 (`tagpu_posebake.c`)
   turned each `Model3DONode` template into a static geometry buffer and a
   per-owner material stream and drew from neither; this is the pass that
   finally does. A unit is one `glDrawArrays` out of its type's buffers, with
   its whole pose in a uniform block — instead of the ~2.75 MB of CPU-built
   vertices the native stream re-uploads every frame.

   A TWIN OF THE NATIVE PROGRAM, NOT A MODE SWITCH (§4, "[mine]"). The native
   program's attributes arrive already in frame-pixel space and are shared with
   the selection lines and the effects models, which stay CPU-built; a uniform
   switch would leave one path reading attributes the other VAO does not bind.
   `tagpu_hires_draw.c` WAS the precedent for both this and the depth twin;
   landing 11 D3 deleted it, so the precedent is in git at that landing's
   parent rather than in the tree.

   THE FRAGMENT SHADER IS THE NATIVE PASS'S OWN, taken through
   `tagpu_native_unit_fs()` rather than copied, so the two programs cannot
   drift in the half of the pipeline this step does not change. What the vertex
   shader takes over is `emit_node`: the piece transform, the engine's
   projection, the depth key, the world x/z the fog samples, the model height
   the waterline clips on, and the shade quantised off the baked rest normal.

   ALL THREE RANGES SINCE STEP 6. The bake always held them; what step 6 adds
   is the vertex shader's `uRange` and the two draws that use it — the
   structure-shadow SLANT (its own projection, its own integer snap, its own
   per-piece `cached` rule) and the nanoframe WIRE (GL_LINES, the body
   projection, one notch nearer). What is still CPU-built is the selection
   lines and the effects models. The CPU emitters were Gate B's and Gate D's
   oracle and survived to the last commit of the gate; **step 8 DELETED them**,
   so this is now the only unit renderer and there is nothing to fall back to
   (§7 step 8).

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

   THE LEVER IS GONE. `tagpu_posedraw.on` was the step-5 to step-7 gate and
   step 8 removed it with the emitters: this pass runs in play, unconditionally,
   because it is the only thing that draws a unit.

   RENDER THREAD ONLY: it owns GL objects and is called from
   `tagpu_native_frame`. */

/* Everything the pass shares with tagpu_native.c's frame. The textures are
   NOT re-bound here: this pass draws between the native pass's own binds, on
   the same units, and its samplers name the same ones. */
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
                                /* the same entry point but is NOT a unit, so */
                                /* the per-frame unit and triangle counters   */
                                /* skip it (they feed the `posed=N/` stats    */
                                /* and the queued-vs-drawn heartbeat, which a */
                                /* ghost would otherwise fire as a false      */
                                /* positive every frame it draws)             */
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

/* "SOMETHING WILL DRAW THE UNIT, SO THE ENGINE NEED NOT." Published because
   `owndraw` must not skip the engine's unit rasterise unless that is true —
   gpu-posing.md §4, decision B. Safe to call from the GAME thread: the render
   thread is the only writer and the word only ever promises in the safe
   direction.

   IT IS 0 ON THIS LANE and has been since the 4b-2 landing review, because
   this pass hands over rather than draws and cannot promise the consumer ran.
   Two things follow that a reader should have before relying on it, both
   measured by landing 11-5d on 2026-09-19 and written up at the definition in
   tagpu_posedraw.c: the engine therefore rasterises every unit every frame
   (`OWND … skipped=0`), and NONE of that reaches the presented frame — so the
   engine is not a fallback, and a frame our unit pass stands down on is blank
   rather than 8bpp. */
int  tagpu_posedraw_live(void);
/* ...and whether it has TRIED and failed, as opposed to not having run yet.
   Only the first is a reason to say anything: `!live` is also the ordinary
   state of the first frames. */
int  tagpu_posedraw_refused(void);
unsigned tagpu_posedraw_drawn(void);  /* units drawn this frame */
/* once per frame, before anything else. `frame_counter` is the fork's
   monotonic render-thread counter and stamps this frame's hand-over. */
void tagpu_posedraw_frame(unsigned frame_counter);

/* 0 when the pass cannot arm: a device whose `maxStorageBufferRange` will not
   hold one unit's pose at the piece ceiling, which is the whole of bring-up.
   There is no CPU emitter to leave those units to, so the caller instead stops
   skipping the engine's own unit rasterise and they are drawn by the engine at
   8bpp. A 0 ALSO MEANS "NO DEVICE YET" for the first frames —
   the state goes back to untried rather than latching a refusal — so a caller
   that latches on the first answer latches the wrong one. */
int  tagpu_posedraw_ready(void);

/* bodies: begin, then one call per unit, then end. It binds nothing and leaves
   nothing dirty — what the window does is open the RECORDING that becomes this
   frame's hand-over, so the bracket still has to be balanced. */
void tagpu_posedraw_begin(const TAGPU_PDVIEW* v);

/* THE GHOST'S OWN WINDOW, and it is a different entry point for two reasons the
   A/B depends on. The capture must never be opened around it -- the GL half is
   one pass over black and a ghost inside it would be compared against a Vulkan
   frame that draws its ghosts in a different STAGE (after the effects) -- and
   the ghost window is not always the second: with no posed unit on screen
   `tagpu_native.c` skips the unit window entirely and this one is the FIRST.
   Telling the two apart by counting windows was wrong on exactly that frame.
   [Landing 6's review, 2026-09-17.] */
void tagpu_posedraw_begin_ghost(const TAGPU_PDVIEW* v);
void tagpu_posedraw_unit(const TAGPU_PDUNIT* u);
void tagpu_posedraw_end(void);

/* TEN ENTRY POINTS STOOD HERE AND WENT WITH LANDING 11-5d, together with the
   62 GL calls behind them:

     _shadow_begin / _shadow_set / _redraw      the Classic silhouette shadow
     _slant_begin  / _slant_set  / _slant_redraw   the structure-shadow slant
     _wire_begin   / _wire_unit                 the nanoframe wireframe
     _depth_begin  / _depth_unit                the shadow-depth twin

   THEY WERE ALREADY UNCALLED BEFORE THIS LANDING — the caller that used them
   was tagpu_native.c's GL composite, deleted by landing 11-3, and a tree-wide
   search found no other. Deleting them is therefore inert. What replaces them
   is NOT uniform, and the honest split is:

     _depth_begin / _depth_unit   NOT DRAWING EITHER, and this line said
                                  "PORTED" for one commit. The Vulkan code
                                  exists -- `tagpu_vk_unit.c:1043-1046` builds
                                  the `pose_depth` pipeline from the VS + DFS
                                  that stay in tagpu_posedraw.c, and :1769 sets
                                  uDepthPass 1 -- but NOTHING REACHES IT. See
                                  the chain below; found by this landing's
                                  review, which was right that "PORTED" is the
                                  sentence most likely to stop the next session
                                  looking.
     _shadow_* and _slant_*       PORTED 2026-09-22, and not by restoring these
                                  entry points. `TAGPU_PDUREC.shKind` carries
                                  which of the engine's two shadow branches a
                                  unit takes and `shFirst`/`shCount` the range
                                  it draws, and tagpu_vk_unit.c records them as
                                  a stage of its own, stencil-masked, before the
                                  bodies. The stencil dance the six GL entry
                                  points existed to express is two pipelines
                                  there.
     _wire_begin / _wire_unit     PORTED 2026-09-23, the same way.
                                  `TAGPU_PDUREC.wireFirst`/`wireCount` carry the
                                  bake's WIRE range for a unit with `nanoOn`,
                                  `wire` its colour, and tagpu_vk_unit.c records
                                  them after the bodies through a LINE_LIST twin
                                  of the body pipeline.

   SO THIS IS A TOMBSTONE, NOT A MIGRATION, for all ten. The gap is the Vulkan
   unit pass's, not this header's — whoever closes it ports the ranges into
   tagpu_vk_unit.c rather than restoring these entry points, which had no
   caller left to serve.

   THE DEPTH TWIN'S CHAIN, BECAUSE IT IS THE ONE THAT LOOKS DONE. `_depth_begin`
   held the only `s_depthOn = 1` and the only write of `s_depthMat` in the tree.
   With it gone:

     s_depthOn                    only ever assigned 0 (tagpu_posedraw.c:1108)
     -> TAGPU_PDUREC.casts        always 0        (:814)
     -> TAGPU_PDHAND.depthOn      always 0        (:706)
     -> TAGPU_PDHAND.castMat      the memcpy that never runs (:707)
     -> tagpu_vk_unit.c:2358      w->casts always 0, so s_ncast never increments
     -> tagpu_vk_unit_cast        returns at `!s_ncast` (:2579) every frame
     -> build_cast_pipeline       its ONLY caller is :2587, inside that function

   So the pipeline is never built and no posed unit casts into the Vulkan
   cast-shadow map. THIS IS NOT NEW IN 11-5d -- `_depth_begin` was already
   uncalled at 11-3, so `depthOn` has been pinned 0 since then and no behaviour
   changed here. What 11-5d did was delete the last code that could ever set it,
   which turns a dormant path into a dead one, and then briefly call it ported.

   AND THE CENSUS CANNOT SEE IT, though not for the reason a first version of
   this paragraph gave. `tagpu_vk_shadow.c:847` asks `h.otherCasters - ours >
   0`. `otherCasters` had THREE writers, not two: `tagpu_shadow_unit` and
   `tagpu_shadow_note_casters`, both callerless, AND the
   heightfield-mirror-missing path at :473, which both G19e shadow reviewers
   added precisely so the census covers all four kinds of caster. That third
   one is live and reachable, so "0 - 0" is not true in general -- when
   `build_hills` takes its out-of-memory exit the census fires and refuses the
   whole map loudly, which is the design working.

   WHAT IS TRUE IS NARROWER AND STILL THE POINT: a missing UNIT caster can
   never reach `otherCasters`, because the GL lane that counted them is gone
   and its two incrementers went with it. So on an ordinary frame the term is
   0 - 0, the map is published as complete, units cast no shadow and nothing
   logs it. [The third writer was found by 11-5d's landing review; the first
   version of this paragraph overclaimed.] */

/* the highest posed model y of one unit's BODY range, from each piece's rest
   AABB through its pose matrix — what `s_emitTop` was taken from before the
   vertices stopped being built on the CPU (gpu-posing.md §4). Returns 0 when
   the unit has no baked body geometry. */
float tagpu_posedraw_top(const TAGPU_PDUNIT* u);


/* ---- THE VULKAN LANE'S HAND-OVER (Phase G / G19e, the SIXTH world pass) ----

   tagpu_vk_unit.c draws the same bodies and the same depth twins from the same
   bake, so that the frame the Vulkan lane presents has units in it. This is
   everything it is handed; it reads no engine state and re-derives nothing.

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
   hand-over in this lane, and here the reason is its own: `units`, `rows`,
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

   IT DOES NOT COUNT THE REST OF THE FRAME, and that is not an oversight. The
   replacement meshes and the native 3DO stream draw OUTSIDE this window, and
   neither draws at all any more. THE SILHOUETTE AND THE SLANT ARE INSIDE IT
   SINCE 2026-09-22, AND THE NANOFRAME WIRE SINCE 2026-09-23: they are drawn from
   the very records this hand-over carries, out of the same bake, by the same
   consumer -- so a unit the hand-over drops loses its shadow with its body
   rather than leaving a shadow behind, which is what makes the refusal still
   whole.
   And the one place where the rest of the frame really does make a wrong
   picture rather than a partial one is the cast-shadow MAP, whose census in
   tagpu_shadow.h's `otherCasters` CANNOT CURRENTLY FIRE: see the depth twin's
   chain in the tombstone above.

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
    /* the fragment stage's. uNanoT and uNanoC are STICKY on the GL side -- the
       twin sets them only on a unit with `nanoOn`, so a unit without one is
       drawn against whatever the last one that had it left in the program --
       and they are published that way rather than zeroed, because the port
       reproduces the twin and not a tidier version of it. Nothing reads them
       on a `uNanoOn == 0` unit; carrying the real value is what makes that a
       statement about the shader rather than about the upload. */
    float alpha, waterT, digT, nanoT, nanoC[3];
    int   fog, waterMode, nanoOn;
    /* THE NANOFRAME WIRE, resolved here like the shadow's range: the bake's
       WIRE range for a unit with `nanoOn`, and `wireCount == 0` for every
       other unit and for every ghost. `wire` is the colour, idx/255 -- the
       vertex stage's `uWire`. */
    int   wireFirst, wireCount;
    float wire;
    /* 1 = this unit was drawn into the cast-shadow depth map this frame, which
       is `!castSkip` on the frame the twin's depth block ran. Meaningless when
       `depthOn` below is 0. */
    int   casts;
    /* 1 = a BUILD GHOST rather than a unit, carried since landing 6. It is the
       same draw with the same uniforms; the only two differences are `alpha`
       (0.40, above) and that the GL twin brackets its ghosts in
       glDepthMask(GL_FALSE) -- ghosts blend with each other, units still
       occlude them -- so a consumer draws these with depth writes OFF and
       after the units, which is the order they are recorded in. `casts` is
       always 0 for one: the depth loop ran earlier in the frame and over the
       real units only. */
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

    /* THE FRAGMENT STAGE'S, as `_begin` and tagpu_shadow_apply left it.
       uLambert IS PUBLISHED AS THE TWIN HOLDS IT, WHICH IS ZERO, AND THAT IS
       NOT AN OVERSIGHT HERE: tagpu_posedraw.c never looks the uniform up and
       never sets it, so on the posed program it keeps a freshly linked
       program's 0 for the pass's life and the Classic++ lambert lights a unit
       from a flat up normal rather than from vNrm. tagpu_native.c sets it on
       ITS program; this one does not. Publishing a 1 here would be a different
       picture from the oracle's. */
    int   restored, scafOn, lit, lambert, shadowOn;
    float scafP[4], ss;
    float fogOrgX, fogOrgY, fogCols, fogRows;
    float sun[3], amb, norm;
    /* the cast-shadow read-back block, and it is only meaningful while
       `shadowOn` is 1 -- tagpu_shadow_apply writes uShadowOn and RETURNS when
       no map is live, so the rest is published as the zero a freshly linked
       program holds. `shadowMat` goes into BOTH stages' blocks at the Vulkan
       end: in GL the vertex and fragment stages share one uniform of that
       name, and the SPIR-V translation gives each stage its own copy. */
    float shadowMat[16], shadowSun[3], shScale[3], penumbra, shade;

    /* THE DEPTH TWIN. `depthOn` MEANS "the twin drew its posed casters into
       the cast-shadow map this frame", `castMat` is the matrix it used, and
       `ncast` is how many records carry `casts`.

       ALL THREE ARE PERMANENTLY ZERO AND HAVE NO PRODUCER, since landing 11-3
       left `tagpu_posedraw_depth_begin` uncalled and 11-5d deleted it. The
       full chain, and why the census does not catch it, is in the tombstone
       that replaced those entry points. Read this field as "not yet", never as
       "no casters this frame" -- the two are indistinguishable here and only
       the first is true.

       `castMat` is tagpu_shadow_mat() and NOT `shadowMat` above by accident --
       they are the same numbers, published twice because one is the map's
       projection and the other is the read-back's, and a frame could have one
       without the other. */
    int   depthOn, ncast;
    float castMat[16];

    /* THE TEXELS, as bytes rather than as GL names -- a second backend cannot
       read a GL texture. Each carries the serial that says when it last
       changed, so the Vulkan lane re-uploads on a change and not per frame.
       The unit atlas and the shade LUT are tagpu_render3do.h's mirrors; the
       palette is tagpu_pal's snapshot; the fog pair is what the native pass
       uploaded this frame.

       THE CLASSIC++ RESTORED TWIN IS NOT HERE AS TEXELS. `atlasRgb`,
       `atlasRgbRows`, `atlasRgbMips` and `atlasRgbSerial` stood here from gate
       3 until landing 11-5e-2b: the twin READ BACK off the GPU, with its own
       rows and its own serial because the read-back lagged the shelf cursor.
       The read-back was `glReadPixels` and nothing else, opengl32.dll is never
       in the process (`oglu_load_dll` has no caller), so `atlasRgb` was NULL on
       every published frame of every process from the day the GL bring-up
       stopped being called. What replaced it is the frame LIST below, which the
       consuming lane paints into its own twin on the device. */
    const unsigned char* atlas;   int atlasDim, atlasRows; unsigned atlasSerial;
    /* the anisotropy the OTHER lane's twin was filtered at (0 = none). A
       consumer that cannot apply the same ratio draws different art wherever
       the texture is minified at an angle, so this is compared and not assumed.
       IT SURVIVED THE READ-BACK because it is a property of that twin's
       SAMPLER rather than of the mirror, and it is published on the list path
       for exactly that reason -- see `restoreDim` below. */
    float atlasRgbAniso;
    /* THE REQUEST, WHICH IS THE ONLY FORM THE TWIN COMES IN (the Vulkan-only
       plan's landing 7e-2, the shape landing 7d gave the feature and effects
       atlases). The gather half publishes the LIST OF FRAMES to restore; the
       other lane paints them into its own twin.

       `restoreGen` is the only thing a cursor cannot survive: every
       discontinuity in the list -- arm, recycle, repack, GL context loss,
       palette move, a GL job made over a fresh twin, an overflow restart --
       bumps it, and a consumer whose generation moved starts at 0 again.
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
    /* THE TWIN'S SHAPE, because no read-back carries it: `restoreDim` is the
       twin's square size and `restoreMips` its top level, both from the atlas
       itself. `atlasRgbAniso` above comes from the same accessor for the same
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

    /* WHAT THE GL FRAME HAS THAT THIS DOES NOT -- see the header above. Any
       non-zero value refuses the frame. */
    int   otherDraws;

    /* 1 on the ONE frame this pass captured `tagpu_posedraw_gl.ppm` under
       `tagpu_posedraw.ab`, so the Vulkan lane captures the SAME frame. */
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

/* one `posed=` field for the native: line; writes nothing when disarmed */
int  tagpu_posedraw_stats(char* out, int n);
#endif
