#ifndef TAGPU_RECLAIM_H
#define TAGPU_RECLAIM_H
/* tagpu_reclaim.h — deferred reclamation of the engine's Object3do.

   The fork's GL render thread gathers a unit's (or wreck's) Object3do and
   dereferences it later in the same frame, while the engine's game thread
   frees it on death — a cross-thread use-after-free that faulted `200v200`
   about 95 s in, two runs in three (research/notes/thread-safe-destruction.md).

   The fix cooperates with the engine's own destructor rather than guarding
   every read: FreeObjectState 0x45AAA0 (the single funnel every Object3do
   free goes through) is detoured to ENQUEUE the object instead of freeing it;
   the engine's own bookkeeping — nulling its pointer, clearing the alive bit —
   runs unchanged, so the sim sees nothing different. The real free runs later,
   on the game thread, once the render thread has PUBLISHED completion of every
   pass that could still hold the pointer (quiescence, never a fixed count: a
   stalled reader freezes reclamation instead of racing it). Level teardown
   (0x491B60) is wrapped so the queue is flushed while the registry those
   frees touch is still alive, and the reader is held off for its duration.

   Installed once at DllMain, byte-matched, all-or-nothing; a different exe
   arms nothing and `tagpu_reclaim.off` disables it. The render thread brackets
   the overlay driver with pass_begin / pass_end (render_ogl.c), and the
   driver skips its engine reads while teardown_active() says a level is
   being freed. */

void tagpu_reclaim_init(void);

/* Render thread, before the overlay driver. Publishes "in a pass". Returns 0
   while a level teardown is in progress (the driver will skip its engine
   reads this frame), 1 otherwise. */
int  tagpu_reclaim_pass_begin(void);

/* Render thread, after the overlay driver returns — UNCONDITIONALLY, on every
   path, or the reader looks busy for ever and reclamation halts. */
void tagpu_reclaim_pass_end(unsigned frame_counter);

/* Render thread, inside the overlay driver, before its first read of an
   engine object: 1 while a level teardown is freeing them — skip the reads. */
int  tagpu_reclaim_teardown_active(void);

int  tagpu_reclaim_armed(void);
#endif
