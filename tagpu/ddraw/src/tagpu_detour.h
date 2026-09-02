#ifndef TAGPU_DETOUR_H
#define TAGPU_DETOUR_H
/* tagpu_detour.h — the small pieces every own-the-draw module needs to put a
   flag-gated stub in front of an engine function.

   The shape is always the same: allocate an RWX stub, compare a byte flag,
   unwind exactly as the callee would (`ret n`) while the flag is set, else
   run the bytes stolen from the prologue and jump back past them. Callers
   byte-match the site first and install all-or-nothing, so a patched or
   different exe arms nothing.

   Used by tagpu_fxown.c (effects, particles) and tagpu_featown.c (features).
   The older modules — owndraw, tracer, suppress, scenario — carry their own
   copies with different stub shapes and are left alone. */
#include <windows.h>

/* RWX scratch for one stub, or NULL */
unsigned char* tagpu_detour_stub(void);
/* write a rel32 at p so that a preceding E8/E9 opcode reaches `target` */
void tagpu_detour_rel(unsigned char* p, unsigned int target);
/* `cmp byte [flag], 0` (7 bytes); returns the new cursor */
unsigned char* tagpu_detour_cmp_flag(unsigned char* p, volatile unsigned char* flag);
/* `mov byte [flag], v` (7 bytes); returns the new cursor */
unsigned char* tagpu_detour_set_flag(unsigned char* p, volatile unsigned char* flag,
                                     unsigned char v);
/* copy `n` bytes over `va` with the page temporarily writable */
int tagpu_detour_write(unsigned int va, const unsigned char* bytes, int n);

/* Prologue detour: while *flag is set the function returns at once with
   `ret retn`; otherwise the stolen prologue bytes run and control resumes at
   va + nst. `nst` must be 5 or 6 and must end on an instruction boundary.
   1 on success. */
int tagpu_detour_leaf(unsigned int va, const unsigned char* stolen, int nst,
                      volatile unsigned char* flag, unsigned char retn);
#endif
