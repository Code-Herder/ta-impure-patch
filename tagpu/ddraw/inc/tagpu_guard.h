#ifndef TAGPU_GUARD_H
#define TAGPU_GUARD_H
/* tagpu_guard -- builders guarding a factory: seated where they can help without moving, a lane kept free for the
   plant's ground products (research/notes/factory-guard-plan.md; research/notes/modules/factory-guard.md). */

/* At DllMain: the guard handler's site 0x40654B, byte-matched, all-or-nothing. */
void tagpu_guard_init(void);
/* The level init, on the LOADER thread at the PLAYERS call (tagpu_replay.c): every row cleared, then the level's
   plant types read -- what each builds, by side, from its build menu. */
void tagpu_guard_level_init(void);

#endif
