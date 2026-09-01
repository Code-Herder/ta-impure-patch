#ifndef TAGPU_TRACER_H
#define TAGPU_TRACER_H
/* tagpu_tracer.c — G4 in-process tracer.
   Installs 5-byte E9 JMP detours on TA's unit-draw path (DrawUnit 0x45AC20 and the
   composite blit 0x459200) that log — with zero file I/O in the hot path — enough to
   prove "function X draws unit N at (sx,sy)", to measure per-call-site cadence, and to
   settle the composite-buffer-persistence question.

   OFF BY DEFAULT: tagpu_tracer_init() installs the detours only if the trigger file
   "tagpu_tracer.on" exists in the game directory at startup. All detours are guarded on
   a byte-match of the stolen prologue, exactly like tagpu_patches.c — a different build
   is left completely untouched. All engine reads are read-only and pointer-guarded. */

/* Called once from DllMain (same path as tagpu_apply_patches), before TA runs.
   No-op unless "tagpu_tracer.on" is present next to the exe. */
void tagpu_tracer_init(void);

/* Called once per rendered frame from the overlay's per-frame callback. No-op when the
   tracer was not armed. Drains the event ring to tagpu.log: the first ~300 raw events
   once, then a summary line every ~60 frames (per-site cadence, distinct unit count,
   unit-ptr range, and the two composite-buffer pointer samples + change flags). */
void tagpu_tracer_flush(unsigned int frame_counter);

#endif
