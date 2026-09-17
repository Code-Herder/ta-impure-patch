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
   `tagpu_hires_draw.c` is the precedent for both this and the depth twin.

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
       tagpu_posebake_unit(); the pass reads `vao`, the body range and nparts. */
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
    float cast[3];              /* altitude, ground + throw, the length scale */
    int   castSkip;             /* out of the depth map (nanoframe, air drop) */
} TAGPU_PDUNIT;

/* Whether the pass can actually draw: its programs linked, its buffers exist
   and the driver's uniform block is big enough. Published because `owndraw`
   must not skip the engine's unit rasterise unless something will replace it —
   gpu-posing.md §4, decision B. Safe to call from the GAME thread: the render
   thread is the only writer and the word only ever says "live" after the pass
   is. */
int  tagpu_posedraw_live(void);
/* ...and whether it has TRIED and failed, as opposed to not having run yet.
   Only the first is a reason to say anything: `!live` is also the ordinary
   state of the first frames. */
int  tagpu_posedraw_refused(void);
unsigned tagpu_posedraw_drawn(void);  /* units drawn this frame */
/* once per frame, before anything else. `frame_counter` is the fork's
   monotonic render-thread counter and stamps this frame's hand-over. */
void tagpu_posedraw_frame(unsigned frame_counter);

/* 0 when the pass cannot draw (no GL, a driver whose uniform block is too
   small). Since step 8 there is no CPU emitter to leave those units to: the
   caller instead stops skipping the engine's own unit rasterise, so they are
   drawn by the engine at 8bpp. Builds the program on the first call — CALL
   ONLY WITH A CURRENT GL CONTEXT. */
int  tagpu_posedraw_ready(void);

/* bodies: begin, then one call per unit, then end. Leaves the program and the
   VAO dirty — the caller puts its own back. */
void tagpu_posedraw_begin(const TAGPU_PDVIEW* v);
void tagpu_posedraw_unit(const TAGPU_PDUNIT* u);
void tagpu_posedraw_end(void);

/* THE CLASSIC SILHOUETTE SHADOW, which reuses the body geometry with uShadow
   = 1 and the pass's offset. Without this a posed unit would simply lose its
   shadow whenever Classic++ is off, since the CPU stream it was drawn from no
   longer holds its vertices. The caller owns the stencil dance that keeps one
   blend per pixel, so the pose upload and the uniforms are `_shadow_set` and
   the draw is `_redraw`, called twice against the same state. */
void tagpu_posedraw_shadow_begin(void);
void tagpu_posedraw_shadow_set(const TAGPU_PDUNIT* u, float offX, float offY,
                               float waterT, float digT);
void tagpu_posedraw_redraw(const TAGPU_PDUNIT* u);

/* THE STRUCTURE-SHADOW SLANT (G16 step 6), the range `emit_slant` built. Same
   stencil dance as the silhouette above and the same reason for the split: the
   two draws must see identical geometry with nothing re-uploaded between them.
   `_slant_set` pins the waterline and digger thresholds at -1e9 itself — the
   erases belong to the COMPLETED branch, never the structure branch, which is
   the G14j fix and not a per-caller choice. */
void tagpu_posedraw_slant_begin(void);
void tagpu_posedraw_slant_set(const TAGPU_PDUNIT* u, float offX, float offY);
void tagpu_posedraw_slant_redraw(const TAGPU_PDUNIT* u);

/* THE NANOFRAME WIREFRAME (G16 step 6), the range `emit_wire` built: GL_LINES,
   the body projection, one notch nearer than the surface it traces, and the
   animated blue as a per-unit uniform rather than a per-vertex colour (the
   material stream is per type and owner; this colour is neither). */
void tagpu_posedraw_wire_begin(void);
void tagpu_posedraw_wire_unit(const TAGPU_PDUNIT* u, float wire);

/* the shadow-depth twin: the same posed vertices through tagpu_shadow.c's
   light matrix, with no fragment work at all — the native stream's own depth
   program discards nothing either, so a colour-keyed texel casts on both
   paths. The caller has the shadow FBO bound and takes its program back. */
void tagpu_posedraw_depth_begin(const float* shadowMat);
void tagpu_posedraw_depth_unit(const TAGPU_PDUNIT* u);

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
   streams `glBufferData` was handed, reached through tagpu_posebake.h's
   mirrors; the ranges are the `first`/`count` this pass's own glDrawArrays
   used; the pose block is the bytes `upload_pose` wrote, through the same
   conversion; and the uniforms are the numbers these draws passed. What a 0-px
   comparison then compares is two rasterisers.

   THE POSE ROWS LIVE IN AN ARENA, not in the record. A unit's block is 14 336
   bytes if it is carried whole, and almost all of that is the 256-piece
   ceiling rather than the model: stock's worst is 36 pieces. So each record
   names an offset into two arrays this module owns and grows, and 300 units of
   stock content cost about half a megabyte rather than four. THE VULKAN PASS
   STILL NEEDS THE WHOLE 14 336-byte WINDOW per unit, because that is the size
   of the block the shader declares and a descriptor must cover it -- what the
   arena saves is the hand-over, not the uniform buffer.

   IT IS VALID FOR THE FRAME THAT PUBLISHED IT AND NO OTHER, like every other
   hand-over in this lane, and here the reason is its own: `units`, `rows`,
   `flags` and `vis` are arrays this module REALLOCATES when a frame needs more
   room than the last, and the bake entries the records name are slots
   tagpu_posebake.c evicts and re-bakes. The frame stamp bounds the first; the
   SERIALS bound the second, and the mirror accessor checks them at the instant
   of the read rather than trusting that the eviction has not happened yet.

   `otherDraws` IS THE REFUSAL, AND IT IS NARROW ON PURPOSE. It counts the
   units THIS PASS DREW INSIDE THE PUBLISHED WINDOW that the hand-over does not
   carry -- a build ghost, a unit past TAGPU_PD_MAXHAND, a unit an arena would
   not grow for -- and a non-zero count stands the Vulkan pass down. That is
   the set the A/B brackets: the GL half is blacked immediately before this
   window and read back immediately after it, so a unit drawn there and not
   here is the one thing that makes the two halves differ.

   IT DOES NOT COUNT THE REST OF THE FRAME, and that is not an oversight. The
   nanoframe wire, the Classic silhouette, the slant, the replacement meshes
   and the native 3DO stream all draw OUTSIDE this window -- they are not in
   the GL capture, so they cannot make this comparison disagree -- and the one
   place where the rest of the frame really does make a wrong picture rather
   than a partial one is the cast-shadow MAP, which has its own exact census in
   tagpu_shadow.h's `otherCasters` and needs no second one here.

   RENDER THREAD ONLY, and published later in the same iteration of
   render_ogl.c's loop than the tagpu_vk_frame that consumes it. */

/* Units one frame hands over. A bound on an allocation that scales with what
   is on screen, and the pass re-checks it: at the Vulkan end each unit costs a
   14 336-byte uniform window per FRAME SLOT, so 512 is 7.3 MB a slot and the
   ceiling is a deliberate one rather than MAXU's 2048. A frame with more posed
   units than this hands over nothing and says so once. */
#define TAGPU_PD_MAXHAND 512

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
    int   npose;                      /* pieces the block below carries       */
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
    /* 1 = this unit was drawn into the cast-shadow depth map this frame, which
       is `!castSkip` on the frame the twin's depth block ran. Meaningless when
       `depthOn` below is 0. */
    int   casts;
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

    /* THE DEPTH TWIN. `depthOn` is 1 when the twin drew its posed casters into
       the cast-shadow map this frame; `castMat` is the matrix it used, which
       is tagpu_shadow_mat() and NOT `shadowMat` above by accident -- they are
       the same numbers, published twice because one is the map's projection
       and the other is the read-back's, and a frame could have one without the
       other. `ncast` is how many records carry `casts`. */
    int   depthOn, ncast;
    float castMat[16];

    /* THE TEXELS, as bytes rather than as GL names -- a second backend cannot
       read a GL texture. Each carries the serial that says when it last
       changed, so the Vulkan lane re-uploads on a change and not per frame.
       The unit atlas and the shade LUT are tagpu_render3do.h's mirrors; the
       palette is tagpu_pal's snapshot; the fog pair is what the native pass
       uploaded this frame.

       `atlasRgb` IS HERE SINCE GATE 3 of the Vulkan-only plan, where before it
       was the one texel the pass had no mirror of and a `restored` frame was
       refused outright for the session. It is the Classic++ restored twin, READ
       BACK off the GPU rather than written by the paint -- which is why it
       carries its own rows and its own serial and why neither is the indexed
       mirror's: the indexed rows are the shelf cursor, these are what the last
       read-back covered. NULL until a read-back has covered rows, and NULL
       again after a context loss until it has covered them anew, so a frame the
       twin drew restored is still refused until then -- for a few frames rather
       than for the process. */
    const unsigned char* atlas;   int atlasDim, atlasRows; unsigned atlasSerial;
    const unsigned char* atlasRgb; int atlasRgbRows;       unsigned atlasRgbSerial;
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

void tagpu_posedraw_glreset(void);
/* one `posed=` field for the native: line; writes nothing when disarmed */
int  tagpu_posedraw_stats(char* out, int n);
#endif
