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
/* land the 5-byte jmp to `stub` on `va` and NOP the rest of the `nst` stolen
   bytes. For a module that builds its own stub shape (tagpu_reclaim.c: it
   needs the stolen tail's address as a trampoline) and wants to build every
   stub before landing any. 1 on success. A stub landed this way is NOT
   recorded for chaining (tagpu_detour_landed below): an observer cannot be
   stacked on it, and tagpu_detour_bytes_ok on its site sees the jmp. */
int tagpu_detour_land(unsigned int va, const unsigned char* stub, int nst);

/* Prologue detour: while *flag is set the function returns at once with
   `ret retn`; otherwise the stolen prologue bytes run and control resumes at
   va + nst. `nst` must be 5..16 and must end on an instruction boundary (the
   bytes past the 5-byte jmp are NOPped). 1 on success. */
int tagpu_detour_leaf(unsigned int va, const unsigned char* stolen, int nst,
                      volatile unsigned char* flag, unsigned char retn);

/* Same, but the skipped path is not empty: it calls `fn` with the function's
   FIRST stack argument before unwinding with `ret retn`. Registers and flags
   are preserved across the call (pushad/popad). Used where taking an engine
   call over still leaves us something to do in its place — the terrain fill
   and the fog grid's lazy rebuild (tagpu_terrown.c). */
int tagpu_detour_leaf_call(unsigned int va, const unsigned char* stolen, int nst,
                           volatile unsigned char* flag, unsigned char retn,
                           void (__cdecl *fn)(void*));

/* Observer detour (Phase E, tagpu_gui_hook.c): the function runs UNCHANGED, we
   only watch it. `before(entry_esp)` is called on entry with a pointer to the
   engine's own stack frame — `((void**)entry_esp)[0]` is the return address,
   `[1]` the first stack argument, and so on — with every register preserved
   around the call (pushad/popad). If it returns non-zero the return address is
   replaced by a trampoline that calls `after(regs)` when the function returns:
   `regs` is the pushad frame, so `regs[7]` is the callee's EAX (its return
   value), and `after` must hand back the real return address it was given by
   `before` (the caller keeps that LIFO stack; `before` reads it at
   `((void**)entry_esp)[0]`). Nothing here is skipped and no flag is consulted,
   so the engine's behaviour is byte-identical with the observer installed. */
/* A site another module already landed on is CHAINED, not overwritten: the
   observer hooks that stub's copy of the stolen bytes, so the earlier module's
   skip still wins and the observer sees only the calls that really draw. The
   stolen bytes must agree. `tagpu_detour_bytes_ok` is the byte-match to use
   at install time — it accepts either the pristine bytes at `va` or the same
   bytes inside a stub that already owns `va`. */
unsigned char* tagpu_detour_landed(unsigned int va, int* stolenOff, int* nst);
int tagpu_detour_bytes_ok(unsigned int va, const unsigned char* stolen, int nst);
typedef int   (__cdecl *tagpu_detour_before_fn)(void* entry_esp);
typedef void* (__cdecl *tagpu_detour_after_fn)(unsigned int* regs);
int tagpu_detour_observe(unsigned int va, const unsigned char* stolen, int nst,
                         tagpu_detour_before_fn before, tagpu_detour_after_fn after);
#endif
