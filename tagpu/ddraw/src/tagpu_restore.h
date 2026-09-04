#ifndef TAGPU_RESTORE_H
#define TAGPU_RESTORE_H
/* The Classic++ restorer — the unditherer's full model run inside the DLL
   through ONNX Runtime, once per map, on a worker thread. Design and the
   measurements behind it: research/notes/renderers.md 2.5. */

/* gamedir/tagpu_restorecpu.on — read once, when the runtime is first loaded:
   keeps the model on the CPU provider even where DirectML would load, for an
   A/B of the two. The restored pixels are the same either way. */

/* gamedir/tagpu_classicpp.on — the renderer switch, polled at most twice a
   second. Absent = Classic, exactly today's pixels. */
int tagpu_classicpp_on(void);

/* Start restoring one map's tile set. Copies the tiles (count * 32*32 palette
   indices) and the live palette (256 entries, 4 bytes each) before returning,
   so the worker never reads engine memory. Returns a generation id > 0, or 0
   if the job could not start. A newer call abandons the older job. */
int tagpu_restore_terrain_begin(const unsigned char* tiles, int count,
                                const unsigned char* pal);

/* -1 failed (reason in tagpu.log), 0 still running, 1 finished, for `gen`. A
   stale generation reads as failed. */
int tagpu_restore_state(int gen);

/* When tagpu_restore_state(gen) == 1: the restored tiles as count * 32*32
   RGBA texels (alpha 255), owned by the module until the next begin. */
int tagpu_restore_terrain_result(int gen, const unsigned char** rgba, int* count);
#endif
