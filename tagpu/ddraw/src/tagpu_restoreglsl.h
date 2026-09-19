#ifndef TAGPU_RESTOREGLSL_H
#define TAGPU_RESTOREGLSL_H
/* THE SHARED CONTRACT OF THE CLASSIC++ RESTORER -- the frame struct both
   backends and tools/tascene are written against, and the tileability test.
   The GL backend this header was named for is gone (11-5e-2); the tombstone
   below says what it was and why it could not run. The restorer that runs is
   `tagpu_vk_restore.c` under `tagpu_restore_core.c`.

   WHAT THE RESTORER IS, for a reader arriving here first: the unditherer's
   residual CNN as fragment passes (research/notes/renderers.md 4c), shaders in
   tagpu_restore_glsl.h. It batches frames into slots, runs FILL, the conv
   layers and OUT for each batch, and paints the result straight into the
   caller's RGBA8 atlas in its bordered cell layout. The work is SLICED: one
   call per frame issues draws until a GPU-time budget is spent, so a map's
   restore is a few seconds of ordinary frames, not a stall.

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
   before the model and written (0, 0, 0, 0) after (tagpu_restore_glsl.h).

   IT CARRIES A STRUCT TAG as well as the typedef, and the tag is load-bearing:
   `inc/tagpu_posedraw.h` publishes a list of these to the unit consumer and
   cannot include this file -- the build's only include directory is `inc`, so a
   header there naming a header here does not resolve. A tag lets that hand-over
   declare the pointer without the layout, and the consumer, which includes both,
   gets the whole type. [The Vulkan-only plan's landing 7e-2.] */
typedef struct TAGPU_RGLSL_FRAME_S {
    int ax, ay, w, h, wrap, dx, dy, border, key, padR, padB;
} TAGPU_RGLSL_FRAME;

/* THE GL JOB API WAS HERE AND IT WENT WITH ITS BACKEND IN 11-5e-2.
   `tagpu_rglsl_job_new/_repaint/_repalette/_add/_clear/_idle/_failed/_painted/
   _free`, `tagpu_rglsl_step`, `_calls`, `_step_forced`, `_mips` and
   `_glreset` were the surface of `tagpu_restoreglsl.c`, which compiled
   tagpu_restore_glsl.h's shaders in the game's own GL context. That file is
   deleted; the Vulkan restorer (`tagpu_vk_restore.c`, the other backend of
   `tagpu_restore_core.c`) is the one this build runs, stepped from
   `tagpu_vk.c` inside the frame's command buffer.

   The API could not have done anything before it went. Its entry into GL was
   `tagpu_rglsl_job_new`, whose ONE call site was `tagpu_gaf.c`'s
   `tagpu_gaf_atlas_restore`, below a `!a->tex` return -- and `a->tex` has
   been 0 for the life of the process since landing 11-4c took the
   `glGenTextures` that filled it out of `tagpu_gaf_atlas_create`. No job was
   ever made, so `gl_ready()` never turned on and `tagpu_rcore_step` returned
   on its second line every frame.

   WHAT THIS HEADER IS NOW. `TAGPU_RGLSL_FRAME` above -- a plain CPU struct
   with no GL in it -- and `tagpu_rglsl_tileable` below. Twelve files include
   this header and almost all of them want only those two; the name is kept
   because the struct's name is the contract both restorer backends and
   tools/tascene are written against.
   [The vulkan-only plan, 11-5e-2.] */
/* classical.is_tileable on palette colours: opposite edges agree within 12
   levels on average, over the three channels. A frame with its colour key
   `key` on an edge is never tileable (tagpu_restore_glsl.h says why);
   key = -1 for an opaque frame. */
/* `pal` is the ART's palette -- the engine's own table (tagpu_pal_engine()),
   never the gamma-scaled one the screen is shown with: the threshold is a raw
   colour distance, so a scaled palette would reclassify tiles at it. */
int  tagpu_rglsl_tileable(const unsigned char* px, int w, int h, const unsigned char* pal, int key);
#endif
