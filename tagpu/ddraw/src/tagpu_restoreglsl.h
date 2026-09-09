#ifndef TAGPU_RESTOREGLSL_H
#define TAGPU_RESTOREGLSL_H
/* The Classic++ restorer as fragment passes in the game's own GL context
   (research/notes/renderers.md 4c). The shaders are tagpu_restore_glsl.h's;
   this is the driver: it batches frames into slots, runs FILL, the conv layers
   and OUT for each batch, and paints the result straight into the caller's
   RGBA8 atlas in its bordered cell layout. The work is SLICED: one call per
   frame issues draws until a GPU-time budget is spent, so a map's restore is a
   few seconds of ordinary frames, not a stall.

   JOBS. A job is a destination atlas fed from a source atlas: the terrain's
   is a fixed list of every tile, added once; a GAF atlas's is an open QUEUE
   that tagpu_gaf.c feeds on every miss (renderers.md 4b Option 4), so a
   sprite draws indexed for the frame or two before its restore lands. All
   jobs share one set of GL objects and one per-frame budget; the one holding
   a batch in flight keeps it, otherwise the lowest `prio` with work runs, so
   the terrain (0) goes before features (1) before effects (2). A job's
   destination is cleared to alpha 0 when the job starts (and on job_clear),
   and the out pass writes alpha 1 over every texel it paints -- a consumer
   samples the restored colour where the alpha says so and stays indexed
   elsewhere, which is how the progressive reveal and the mixed-mode atlases
   both work with no flag texture.

   gamedir files it reads: <model>.w32.bin beside TotalA.exe (unditherer
   export-weights; full by default), and the options trigger
   tagpu_restoreglsl.on -- tokens, read once per GL context:
     tiny        the 6x24 model (tiny.w32.bin) instead of 12x64
     fp16        RGBA16F activations (fp32 is the default and the one that
                 meets the correctness bar; renderers.md 4c Q4)
     nk=N        output channel-tiles per conv draw (1, 2, 4, 8; default: the
                 most the device's uniform-block size allows)
     budget=MS   GPU milliseconds per frame (default 12)
     log         a line per batch in tagpu.log */

/* The renderer switch the jobs pause under is tagpu_classicpp.h's. */

/* One frame to restore: w x h palette indices at (ax, ay) of the R8 atlas,
   painted at (dx, dy) of the destination with `border` replicated edge texels
   around it, and `padR` / `padB` more past the right and bottom border (an
   aligned atlas's cell slack, tagpu_gaf.h `align`; 0 elsewhere). `wrap` =
   tile it (tagpu_rglsl_tileable) -- the caller decides. `key` = the frame's
   colour key index, or -1 for an opaque frame: keyed texels are inpainted
   before the model and written (0, 0, 0, 0) after (tagpu_restore_glsl.h). */
typedef struct {
    int ax, ay, w, h, wrap, dx, dy, border, key, padR, padB;
} TAGPU_RGLSL_FRAME;

typedef struct TAGPU_RGLSL_JOB TAGPU_RGLSL_JOB;

/* A job: frames read from `atlasTex` (GL_R8, atlasW x atlasH) with the
   palette `pal` (256 x R,G,B,pad; snapshotted now), painted into `destTex`
   (GL_RGBA8, destW x destH; cleared to 0 now). `tag` prefixes its log lines,
   `prio` orders it against the other jobs, `oneshot` = a fixed list whose
   completion is logged as the restore's "done" line (the terrain). NULL,
   with the reason in tagpu.log, when the model or the GL cannot be set up.
   Render thread only, GL context current -- every call here. */
TAGPU_RGLSL_JOB* tagpu_rglsl_job_new(const char* tag, int prio, int oneshot,
                                     unsigned int atlasTex, int atlasW, int atlasH,
                                     const unsigned char* pal,
                                     unsigned int destTex, int destW, int destH);
/* Queue `frames` (copied) behind what is already queued; they restore in this
   order. 0 if out of memory. */
int  tagpu_rglsl_job_add(TAGPU_RGLSL_JOB* j, const TAGPU_RGLSL_FRAME* frames, int count);
/* Drop everything queued or in flight and clear the destination to 0 again
   (the caller's atlas was recycled: every rect will be re-added on its miss). */
void tagpu_rglsl_job_clear(TAGPU_RGLSL_JOB* j);
/* 1 when nothing is queued or in flight: every frame added so far is painted
   (once the GPU drains, i.e. before any later draw samples the destination). */
int  tagpu_rglsl_job_idle(const TAGPU_RGLSL_JOB* j);
/* 1 when the job can do no more (a GL failure): its destination stays as it is */
int  tagpu_rglsl_job_failed(const TAGPU_RGLSL_JOB* j);
/* Frames painted so far -- the OUT draw issued -- over the job's life: a
   counter for a consumer that must do something to the destination after
   each batch (the unit atlas rebuilds its mip levels). Never reset by a
   clear; compare it for change, not for a value. */
int  tagpu_rglsl_job_painted(const TAGPU_RGLSL_JOB* j);
/* Forget the job; the destination is the caller's. */
void tagpu_rglsl_job_free(TAGPU_RGLSL_JOB* j);

/* Once per frame, after the passes have gathered (so this frame's misses are
   queued) and before they render: issue draws for the active job until the
   budget is spent. Leaves the framebuffer, viewport, enables and write masks
   as it found them; program 0, VAO 0, no array buffer, texture unit 0
   active. Left DIRTY, as every tagpu pass leaves them and every pass rebinds
   before drawing: the 2D bindings of units 0-3 and 5, unit 4's 2D-array
   binding, uniform binding point 0, GL_UNPACK_ALIGNMENT = 1. Does nothing
   while the switch is off. */
void tagpu_rglsl_step(void);
/* How many times tagpu_rglsl_step has been CALLED (not how much it painted).
   Its only caller is the native pass, which returns early when there is no
   unit array -- in the shell, and in game with the world passes disarmed. A
   pass whose atlas exists there (the UI's does) compares this across presents
   to find out whether anything stepped the restorer, and steps it itself when
   nothing did; otherwise its queue is never drained. */
unsigned tagpu_rglsl_calls(void);
/* The GL context died with everything in it: forget the ids, no deletes, and
   every job with them -- call it BEFORE the jobs' owners forget theirs. */
void tagpu_rglsl_glreset(void);
/* classical.is_tileable on palette colours: opposite edges agree within 12
   levels on average, over the three channels. A frame with its colour key
   `key` on an edge is never tileable (tagpu_restore_glsl.h says why);
   key = -1 for an opaque frame. */
int  tagpu_rglsl_tileable(const unsigned char* px, int w, int h, const unsigned char* pal, int key);
#endif
