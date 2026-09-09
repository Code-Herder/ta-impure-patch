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

/* THE LEVEL GENERATION. Bumped once per level teardown, on the game thread, in
   the POST hook — after the cascade has freed the templates, and before the
   render thread is released.

   NOT the pre hook, and the difference is a bug rather than a preference: the
   render thread is stopped by tagpu_overlay.c's teardown_active() gate, not by
   pass_begin, so a pass that got past that gate before the flag was set runs on
   while the pre hook waits for it — and would there see a generation bumped in
   the pre hook, drop its template caches and refill them from templates the
   cascade has not freed yet. reclaim_teardown_post carries the full reasoning.

   It exists for the OTHER lifetime this module does not otherwise cover.
   `FreeObjectState` owns the per-unit one: an `Object3do` and its posed vertex
   buffers. But a `Model3DONode` tree — the model TEMPLATE — is shared by every
   unit of a type, is not reached through that destructor, and its lifetime is
   the LEVEL. Anything that caches a template pointer across frames (the whole-
   tree AABB the shadow height reads, the select-box bounds, the glTF piece
   map) therefore holds an address the next level's allocator may hand to a
   different model — a wrong model drawn for the rest of the session rather
   than a fault. Comparing this counter against the one an entry was built
   under is what makes such a cache safe.

   Read it from the render thread; it is a plain aligned 32-bit load. It starts
   at 0, and on an exe where the teardown could not be hooked it never moves —
   which is exactly the behaviour those caches had before it existed. */
unsigned tagpu_reclaim_level_gen(void);
#endif
