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
   lines and the effects models. The CPU emitters stay too — they are Gate B's
   and Gate D's oracle, and they survive to the last commit of the gate
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

   THE LEVER IS `tagpu_posedraw.on`, off in play. Nothing here runs without it.

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
    float alpha;                /* 0.5 while cloaked                          */
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
void tagpu_posedraw_frame(void);      /* once per frame, before anything else */

/* 0 when the pass cannot draw (no lever, no GL, a driver whose uniform block
   is too small), so the caller leaves those units to the CPU emitter rather
   than to nobody. Builds the program on the first call — CALL ONLY WITH A
   CURRENT GL CONTEXT. */
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

void tagpu_posedraw_glreset(void);
/* one `posed=` field for the native: line; writes nothing when disarmed */
int  tagpu_posedraw_stats(char* out, int n);
#endif
