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
   `[1]` the first stack argument, and so on — with every register and the
   flags preserved around the call (pushfd/pushad ... popad/popfd). If it returns non-zero the return address is
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
   bytes inside a stub that already owns `va`. THE CHAIN RULE: an observer
   WITH an `after` replaces the return address the callee will use, so an
   observer chained after it would read the trampoline instead of the engine's
   return address; `tagpu_detour_observe` therefore refuses to chain onto a
   stub that has an `after` (returns 0), i.e. the hijacker must be the last
   observer installed on its site (tagpu_packet_pub.c on DrawGameScreen, after
   tagpu_menu.c's). */
unsigned char* tagpu_detour_landed(unsigned int va, int* stolenOff, int* nst);
int tagpu_detour_bytes_ok(unsigned int va, const unsigned char* stolen, int nst);
typedef int   (__cdecl *tagpu_detour_before_fn)(void* entry_esp);
typedef void* (__cdecl *tagpu_detour_after_fn)(unsigned int* regs);
int tagpu_detour_observe(unsigned int va, const unsigned char* stolen, int nst,
                         tagpu_detour_before_fn before, tagpu_detour_after_fn after);

/* BRANCH STEAL. `va` holds a `test`-then-`je` pair -- `nst` bytes, of which the
   `je` is the last two -- and BOTH of its arms are engine code we must still be
   able to reach. While *flag is set the stub takes the `je`'s arm
   unconditionally (`target`); while it is clear the engine's own `test` runs
   and chooses between `target` and `resume` exactly as it did before.

   Neither existing shape expresses this. A leaf detour has a function to return
   from and this has none -- both arms are inside one -- and a call-site skip
   needs a `call` to repoint. Hence a third stub shape.

   THE FLAG IS COMPARED FIRST, before the stolen `test`, so the flags the
   engine's own `je` reads are the ones the engine's own `test` set.

   TWO THINGS ARE THE CALLER'S: byte-matching `va` before asking for this, and
   having checked that nothing branches INTO the stolen range past its first
   byte. A `je` flip is immune to an incoming branch and a 5-byte detour is not,
   so that check is not optional -- `objdump -d -M intel` over the whole image
   for branch targets in the range, and a search of the image for each address
   in it as a little-endian dword (a jump-table entry). 1 on success. */
int tagpu_detour_branch(unsigned int va, const unsigned char* stolen, int nst,
                        unsigned int target, unsigned int resume,
                        volatile unsigned char* flag);

/* Call-site skip: the 5-byte `call rel32` at `va` (which must resolve to
   `callee`, checked) is repointed at a stub that unwinds as the callee would —
   `ret argBytes`, the callee's own stdcall pop — while *flag is set, and
   tail-jumps to it otherwise. Same length as the instruction it replaces: no
   stolen bytes, no boundary, no trampoline. Use this instead of a leaf detour
   whenever the callee's CALLER depends on side effects the callee's function
   performs — see the comment in tagpu_detour.c. 1 on success. */
int tagpu_detour_call_site(unsigned int va, unsigned int callee,
                           volatile unsigned char* flag, unsigned char argBytes);
#endif
