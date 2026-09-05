#ifndef TAGPU_RESTOREGLSL_H
#define TAGPU_RESTOREGLSL_H
/* The Classic++ restorer as fragment passes in the game's own GL context
   (research/notes/renderers.md 4c). The shaders are tagpu_restore_glsl.h's;
   this is the driver: it batches frames into slots, runs FILL, the conv layers
   and OUT for each batch, and paints the result straight into the caller's
   RGBA8 atlas in its bordered cell layout. The work is SLICED: one call per
   frame issues draws until a GPU-time budget is spent, so a map's restore is a
   few seconds of ordinary frames, not a stall.

   gamedir files it reads: <model>.w32.bin beside TotalA.exe (unditherer
   export-weights; full by default), and the options trigger
   tagpu_restoreglsl.on -- tokens, read once per job:
     tiny        the 6x24 model (tiny.w32.bin) instead of 12x64
     fp16        RGBA16F activations (fp32 is the default and the one that
                 meets the correctness bar; renderers.md 4c Q4)
     nk=N        output channel-tiles per conv draw (1, 2, 4, 8; default: the
                 most the device's uniform-block size allows)
     budget=MS   GPU milliseconds per frame (default 12)
     log         a line per batch in tagpu.log */

/* gamedir/tagpu_classicpp.on -- the renderer switch, polled at most twice a
   second. Absent = Classic, exactly today's pixels. */
int  tagpu_classicpp_on(void);

/* One frame to restore: w x h palette indices at (ax, ay) of the R8 atlas,
   painted at (dx, dy) of the destination with `border` replicated edge texels
   around it. `wrap` = tile it (is_tileable) -- the caller decides. */
typedef struct {
    int ax, ay, w, h, wrap, dx, dy, border;
} TAGPU_RGLSL_FRAME;

/* Start a job: `frames` (copied) in the order they should be restored, read
   from `atlasTex` (GL_R8, atlasW x atlasH) with the palette `pal` (256 x
   R,G,B,pad), painted into `destTex` (GL_RGBA8, destW x destH; cleared to 0
   first). Returns 1, or 0 with the reason in tagpu.log. A running job is
   abandoned. Render thread only, GL context current. */
int  tagpu_rglsl_begin(const TAGPU_RGLSL_FRAME* frames, int count,
                       unsigned int atlasTex, int atlasW, int atlasH,
                       const unsigned char* pal,
                       unsigned int destTex, int destW, int destH);
/* Issue this frame's slice. 0 = still running, 1 = the last draw has been
   issued (the destination is complete once the GPU drains, i.e. before any
   later draw samples it), -1 = failed. Leaves the framebuffer, viewport,
   enables and write masks as it found them; program 0, VAO 0, no array
   buffer, texture unit 0 active. Left DIRTY, as every tagpu pass leaves
   them and every pass rebinds before drawing: the 2D bindings of units 0-3,
   uniform binding point 0, GL_UNPACK_ALIGNMENT = 1. */
int  tagpu_rglsl_step(void);
/* Drop a job and its GL objects (a new map). */
void tagpu_rglsl_abort(void);
/* The GL context died with everything in it: forget the ids, no deletes. */
void tagpu_rglsl_glreset(void);
/* classical.is_tileable on palette colours: opposite edges agree within 12
   levels on average, over the three channels. */
int  tagpu_rglsl_tileable(const unsigned char* px, int w, int h, const unsigned char* pal);
#endif
