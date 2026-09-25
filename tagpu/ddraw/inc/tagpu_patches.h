#ifndef TAGPU_PATCHES_H
#define TAGPU_PATCHES_H
/* Apply our small engine byte-patches to the loaded TotalA.exe image. Called once
   from DllMain (before TA's startup code runs). All patches are guarded: a byte is
   only written if it currently holds the expected value, so a different build is
   left untouched. The exe on disk is never modified. */
void tagpu_apply_patches(void);

/* The wire-robustness landing's three observe-only oracle counters (morph, recreate, ghost;
   sim-fixes.md B3, evidence §10), read by the heartbeat. Zero until the game runs. */
void tagpu_wire_counters(unsigned int* morph, unsigned int* recreate, unsigned int* ghost);
#endif
