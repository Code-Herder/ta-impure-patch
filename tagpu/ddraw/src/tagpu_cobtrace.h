#ifndef TAGPU_COBTRACE_H
#define TAGPU_COBTRACE_H
/* tagpu_cobtrace.h — the COB script-call oracle.

   With `tagpu_cobtrace.on` in the game dir at DLL attach, every COB thread the
   engine's script VM starts, refuses, returns, kills or draws a random number
   for is appended as one tab-separated line to `tagpu_cobtrace.log`, stamped
   with the sim tick, so a headless VM (tacob) can be diffed against
   the real game. The line contract is research/notes/tacob-design.md, "The
   trace contract"; the engine seam is exe-reverse-engineering.md, "The COB
   engine". Reads only — byte-matched, all-or-nothing hooks inside the COB
   engine (0x4B08C0, 0x4B0DA0, 0x4B19D0, 0x4B1A99, the call at 0x4B15E0), no
   engine state written. The destructors at 0x4B06B0 and 0x4B06F0 invalidate
   deferred starts before memory is freed. The file's optional contents are a type filter:
   `ARMPW,CORAK` logs only those unit types; empty or `all` logs every unit. */

void tagpu_cobtrace_init(void);      /* DllMain: arm if the flag file exists */
void tagpu_cobtrace_destroy(char* cob);
void tagpu_cobtrace_vm_context(char* cob, int parent, int argc, int call);
void tagpu_cobtrace_step(char* cob, unsigned slot);
#endif
