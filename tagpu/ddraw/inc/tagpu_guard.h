#ifndef TAGPU_GUARD_H
#define TAGPU_GUARD_H
/* tagpu_guard -- builders guarding a factory: seated where they can help without moving, a lane kept free for the
   plant's ground products (research/notes/factory-guard-plan.md; research/notes/modules/factory-guard.md). */

/* At DllMain: the guard handler's site 0x40654B, byte-matched, all-or-nothing. */
void tagpu_guard_init(void);
/* The level init, on the LOADER thread at the PLAYERS call (tagpu_replay.c): every row cleared, then the level's
   plant types read -- what each builds, by side, from its build menu. */
void tagpu_guard_level_init(void);

/* FOR BUILD SCATTER (tagpu_scatter.c), on the GAME thread.
   _seats: 1 when the unit at `slot` is a builder this module seats -- a row of that incarnation whose builder
   still guards its live plant. Such a unit is the guard's to move: a seat or wait spot on a refused build site is
   given up at the builder's next visit (guard_visit).
   _mark: every plant's lanes and every row's seat or wait spot in the w x h box of cells at (x0, z0) set to 1 in
   `t`: ground no other unit is sent to stand on. */
int  tagpu_guard_seats(unsigned slot);
void tagpu_guard_mark(int x0, int z0, int w, int h, unsigned char* t);

#endif
