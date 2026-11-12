#ifndef TAGPU_IMPAI_H
#define TAGPU_IMPAI_H
/* tagpu_impai -- ImpAI's seam in the engine (research/notes/impai/engine-seam.md).

   The players phase's call of the stock AI tick, 0x465031 `call 0x408C40`, is retargeted to a
   wrapper that observes the seats the test lever tagpu_impai.on marks and then always calls the
   stock tick, so the stock AI still plays every seat. Which seats are ImpAI's is SIM_STATE,
   written at the level init. */

/* DllMain, before the registry closes its lever list: reads tagpu_impai.on and, when it names
   seats, byte-matches and redirects the call at 0x465031. With no lever the site stays stock. */
void tagpu_impai_init(void);

/* The level init's (tagpu_replay.c, tagpu_replay_level_players), right after the engine's
   players init 0x464990 on the LOADER thread: marks the lever's seats on a live level, and
   writes nothing on a puppet level, whose seats the recording restores. */
void tagpu_impai_level_init(void);

/* The level's teardown (tagpu_reclaim.c, reclaim_teardown_pre), on the game thread: logs each
   marked seat's observed turns. */
void tagpu_impai_level_end(void);

#endif
