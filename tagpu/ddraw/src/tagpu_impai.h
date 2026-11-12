#ifndef TAGPU_IMPAI_H
#define TAGPU_IMPAI_H
/* tagpu_impai -- ImpAI's seam in the engine (research/notes/impai/engine-seam.md).

   The players phase's call of the stock AI tick, 0x465031 `call 0x408C40`, is retargeted to a
   wrapper that observes the seats impure.cfg's `seatai` gives ImpAI and then always calls the
   stock tick, so the stock AI still plays every seat. Which seats are ImpAI's is SIM_STATE,
   written at the level init. */

/* DllMain, after tagpu_replay_init: when replay's sites are in -- among them the level init's
   PLAYERS call, the only caller of tagpu_impai_level_init -- byte-matches and redirects the
   call at 0x465031. Otherwise nothing is written, ImpAI is unavailable and every level says
   why. */
void tagpu_impai_init(void);

/* The level init's (tagpu_replay.c, tagpu_replay_level_players), right after the engine's
   players init 0x464990 on the LOADER thread: clears the seat table on every level, then marks
   the computer seats `seatai` names on a live level; a puppet level's seats the recording
   restores. */
void tagpu_impai_level_init(void);

/* The level's teardown (tagpu_reclaim.c, reclaim_teardown_pre), on the game thread: logs each
   marked seat's observed turns. */
void tagpu_impai_level_end(void);

/* 1 when ImpAI plays record i, 0 for OTA AI, a human or an index past the ten: the recording
   header's RT_PLAYER byte 15 (tagpu_recorder.c, header_build). */
unsigned tagpu_impai_seat_ai(unsigned i);

#endif
