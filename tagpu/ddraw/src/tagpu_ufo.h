#ifndef TAGPU_UFO_H
#define TAGPU_UFO_H
/* tagpu_ufo — write a Total Annihilation HAPI archive (.ufo) at runtime.

   WHY THE DLL WRITES ONE AT ALL. The render-options screen is a real TA `.GUI`
   screen, and `GUI_Load 0x4AA8F0` reaches it as `guis\RENDER.GUI` through the
   engine's own file layer — so the file has to be somewhere the engine looks.
   A NEW name goes in a `.ufo` (file-formats.md §5), the engine globs `*.UFO`
   from the working directory at `InitTAHPIAry 0x41D4C0`, and shipping it from
   the DLL rather than from CI keeps distribution at one `ddraw.dll` and makes
   it impossible for the archive to drift out of step with the code that
   expects it (renderers.md §2.10, "How it is assembled").

   WHY THERE IS NO COMPRESSOR HERE. A literal-only method-1 (LZSS) stream is a
   valid, uncompressed encoding that every method-1 decoder accepts — measured
   while writing `tools/hpipack.py`, which is the reference this is a port of.
   So the writer is a directory walk plus a byte loop, and the archive is a few
   KB bigger than it needs to be, which nothing here cares about.

   WHAT THE ENGINE ACTUALLY CHECKS (0x4BDD70, measured 2026-09-02): the last 36
   bytes must be the literal "Copyright 1997 Cavedog Entertainment", read raw.
   A writer that forgets it produces a file every third-party READER accepts
   and the engine silently ignores. Header key 0 is fine — `rev31.gp3` ships
   with it — so nothing here is obfuscated. */

typedef struct
{
    const char*  path;       /* archive-relative, EXACTLY one directory deep:
                                "guis/render.gui". See the refusal below. */
    const void*  data;
    unsigned     size;
} TAGPU_UFO_FILE;

/* Write `n` files as a HAPI archive at `path`. 1 on success, 0 on failure
   (including a path that is not `dir/file`, which is refused rather than
   guessed at — one level is what the asset set needs, and renderers.md §2.10
   says to hand the job to CI rather than grow this if it ever needs more). */
int tagpu_ufo_write(const char* path, const TAGPU_UFO_FILE* files, int n);

#endif
