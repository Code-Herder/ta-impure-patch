#ifndef TAGPU_SUPPRESS_H
#define TAGPU_SUPPRESS_H
/* tagpu_suppress.c — G5 render-suppression detour on TA's per-unit draw.

   Installs a single 5-byte E9 JMP detour on DrawUnit 0x45AC20 (pristine TotalA.exe,
   ImageBase 0x400000, md5 8e74a1dffa1f5988624c52048f5b20cd) that makes ONE chosen
   unit TYPE render-invisible while leaving the simulation completely untouched: the
   detour classifies the unit read-only, and for a matching type it returns to the
   caller immediately (emulating DrawUnit's own `ret 8`), skipping the entire draw
   body with no side effects. Non-matching units pass through byte-identically to the
   tracer (execute the 7 stolen prologue bytes, then jmp 0x45AC27).

   OFF BY DEFAULT: tagpu_suppress_init() installs the detour only if the trigger file
   "tagpu_suppress.on" exists in the game directory at startup. That file's first
   whitespace-delimited token names the unit TYPE to suppress (e.g. "armcom",
   "corcom"); an empty file defaults to the ARM Commander ("armcom"). Read once at
   init. The detour is byte-match guarded on the 7-byte stolen prologue exactly like
   tagpu_patches.c / tagpu_tracer.c — a different build is left completely untouched.
   Classification is read-only and pointer-guarded; no UnitStruct field is modified. */

/* Called once from DllMain (same path as tagpu_apply_patches / tagpu_tracer_init),
   before TA runs. No-op unless "tagpu_suppress.on" is present next to the exe. */
void tagpu_suppress_init(void);

/* Called once per rendered frame from the overlay's per-frame callback (next to
   tagpu_tracer_flush). No-op unless suppression was armed. Logs a summary line to
   tagpu.log every ~60 frames: "SUPP type=<name> hits=<n> passed=<n> ...". */
void tagpu_suppress_flush(unsigned int frame_counter);

/* Classifier invoked (via its address) from the runtime-generated detour stub.
   __cdecl(UnitStruct* unit) -> 1 to suppress this unit's draw, 0 to draw it. */
int __cdecl tagpu_suppress_classify(unsigned int unit);

#endif
