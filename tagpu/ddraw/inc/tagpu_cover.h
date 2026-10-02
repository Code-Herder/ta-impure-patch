#ifndef TAGPU_COVER_H
#define TAGPU_COVER_H
/* tagpu_cover -- the coverage build's record of which of this DLL's functions ran
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S3c-3).

   `make COVER=1` builds ddraw-cover.dll: the simulation files of thread-split.allow and the demo
   recorder's own are compiled with -finstrument-functions-once, so each of their functions calls
   __cyg_profile_func_enter the first time it runs, which marks the function's first byte in a
   bitmap over the DLL's .text. The replay harness asks for the bitmap with the trigger's
   `cover <name>` and maps each address to its function through the build's linker map
   (ddraw-cover.map). In any other build nothing is instrumented and the bitmap does not exist. */

/* The functions that have run, as `replays\cover-<name>.bin`: "ICV1", the count, then each
   function's RVA, all u32 little-endian. The name is 1 to 39 letters, digits, '-' and '_'. 1
   written; 0 with a line when this is not a coverage build, the name is bad or the file could
   not be written. Any thread. The first name given is armed for tagpu_cover_flush. */
int tagpu_cover_write(const char* name);

/* The first name tagpu_cover_write was given, written again with every function that has run
   since, at the level's end (tagpu_rec_level_end): a harness that asks while the level plays also
   gets what runs up to the end, and it collects the files after the end. Nothing in another
   build. */
void tagpu_cover_flush(void);

#endif
