#ifndef TAGPU_RESTOREGLSL_H
#define TAGPU_RESTOREGLSL_H
/* THE SHARED CONTRACT OF THE CLASSIC++ RESTORER -- the frame struct the
   restorer and tools/tascene are written against, and the tileability test.
   The restorer that runs is `tagpu_vk_restore.c` under `tagpu_restore_core.c`.

   WHAT THE RESTORER IS, for a reader arriving here first: the unditherer's
   residual CNN as Vulkan compute (research/notes/compute-restorer.md), shaders
   in tagpu_restore_comp.h. It batches frames into slots, runs FILL, the conv
   layers and OUT for each batch, and paints the result straight into the
   caller's RGBA8 atlas in its bordered cell layout. The work is SLICED: one
   call per frame issues dispatches until a GPU-time budget is spent, so a map's
   restore is a few seconds of ordinary frames, not a stall.

   JOBS. A job is a destination atlas fed from a source atlas: the terrain's
   is a fixed list of every tile, added once; a GAF atlas's is an open QUEUE
   that tagpu_gaf.c feeds on every miss (renderers.md 4b Option 4), so a
   sprite draws indexed for the frame or two before its restore lands. All
   jobs share the backend's device resources and one per-frame budget; the one holding
   a batch in flight keeps it, otherwise the lowest `prio` with work runs, so
   the terrain (0) goes before features (1) before effects (2). A job's
   destination is cleared to alpha 0 when the job starts (unless the job
   repaints in place), and the out pass writes alpha 1 over every texel it paints -- a consumer
   samples the restored colour where the alpha says so and stays indexed
   elsewhere, which is how the progressive reveal and the mixed-mode atlases
   both work with no flag texture.

   gamedir files it reads: full.w32.bin and tiny.w32.bin beside TotalA.exe
   (unditherer export-weights), and the options trigger tagpu_restoreglsl.on -- tokens,
   read at each restorer bring-up (tagpu_rcore_reload):
     budget=MS   GPU milliseconds per frame (default 12)
     log         a line per batch in tagpu.log */

/* The renderer switch the jobs pause under is tagpu_classicpp.h's. */

/* One frame to restore: w x h palette indices at (ax, ay) of the R8 atlas,
   painted at (dx, dy) of the destination with `border` replicated edge texels
   around it, and `padR` / `padB` more past the right and bottom border (an
   aligned atlas's cell slack, tagpu_gaf.h `align`; 0 elsewhere). `wrap` =
   tile it (tagpu_rglsl_tileable) -- the caller decides. `key` = the frame's
   colour key index, or -1 for an opaque frame: keyed texels are inpainted
   before the model and written (0, 0, 0, 0) after (tagpu_restore_comp.h).

   IT CARRIES A STRUCT TAG as well as the typedef, and the tag is load-bearing:
   `inc/tagpu_posedraw.h` publishes a list of these to the unit consumer and
   cannot include this file -- the build's only include directory is `inc`, so a
   header there naming a header here does not resolve. A tag lets that hand-over
   declare the pointer without the layout, and the consumer, which includes both,
   gets the whole type. */
typedef struct TAGPU_RGLSL_FRAME_S {
    int ax, ay, w, h, wrap, dx, dy, border, key, padR, padB;
} TAGPU_RGLSL_FRAME;

/* A NEIGHBOURHOOD FRAME: one tile of a map restored with its eight neighbours
   around it, so the restored tile and its one-texel ring are the texels a
   restore of the whole map gives there (research/notes/compute-restorer.md
   D5). `f` is the centre -- (ax, ay) its tile in the source, (dx, dy) its cell
   in the destination, `border` the ring painted from the real neighbours;
   `wrap`, `key`, `padR` and `padB` are not used (a neighbourhood is opaque and
   never tiles). `nbo` is each neighbour's tile in the source, x | y << 16, in
   the order NW N NE W E SW S SE. Past a side of the map where `edge` has its
   bit, the neighbour is the mirror image of the cell inside -- `nbo` names
   that cell and the restorer flips its texels across the side -- so the map
   continues as its own reflection, texel -1 being texel 0.
   tagpu_rcore_nb_frame builds one from a map; every neighbour tile has the
   centre's size. */
enum { TAGPU_RNB_L = 1, TAGPU_RNB_R = 2, TAGPU_RNB_T = 4, TAGPU_RNB_B = 8 };
typedef struct {
    TAGPU_RGLSL_FRAME f;
    int      edge;
    unsigned nbo[8];
} TAGPU_RNBFRAME;

/* Map cell (c, r) of a w x h map as a neighbourhood frame into `o`: `org(ctx,
   x, y)` is cell (x, y)'s tile in the source (x | y << 16), each tile `tw` x
   `th`, and the frame paints (dx, dy) of the destination with `border`. A
   neighbour past a side of the map is the cell mirrored back across it, with
   that side's edge bit. 0 for a cell off the map. tagpu_restore_core.c. */
int tagpu_rcore_nb_frame(TAGPU_RNBFRAME* o, int c, int r, int w, int h,
                         unsigned (*org)(void* ctx, int x, int y), void* ctx,
                         int tw, int th, int dx, int dy, int border);

/* THE MODEL A JOB RUNS: full for every job but the terrain's, which is tiny
   (research/notes/compute-restorer.md D3). Each is <name>.w32.bin beside
   TotalA.exe; a job whose model is missing runs full. */
enum { TAGPU_RM_FULL, TAGPU_RM_TINY, TAGPU_RM_N };

/* WHAT THIS HEADER HOLDS. `TAGPU_RGLSL_FRAME` above -- a plain CPU struct
   with no rendering API in it -- and `tagpu_rglsl_tileable` below. Almost
   every file that includes this header wants only those two; the name stays
   because the struct's name is the contract the restorer and tools/tascene
   are written against. */
/* classical.is_tileable on palette colours: opposite edges agree within 12
   levels on average, over the three channels. A frame with its colour key
   `key` on an edge is never tileable;
   key = -1 for an opaque frame. */
/* `pal` is the ART's palette -- the engine's own table (tagpu_pal_engine()),
   never the gamma-scaled one the screen is shown with: the threshold is a raw
   colour distance, so a scaled palette would reclassify tiles at it. */
int  tagpu_rglsl_tileable(const unsigned char* px, int w, int h, const unsigned char* pal, int key);
#endif
