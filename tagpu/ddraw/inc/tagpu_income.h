#ifndef TAGPU_INCOME_H
#define TAGPU_INCOME_H
/* tagpu_income -- the harness's income lever: one seat's production times a multiplier, for the handicap
   ladder (tools/impai/ladder.py; research/notes/modules/income-lever.md). Off unless the test lever
   tagpu_impai_income.on names a seat in a test launch; never a player setting. */

/* At DllMain, before the registry closes its lever list: reads the lever and redirects the players phase's
   call of the economy pass, 0x46555A, byte-matched, whether or not the lever is present. */
void tagpu_income_init(void);
/* The level init, on the LOADER thread at the PLAYERS call (tagpu_replay.c), after ImpAI's: every seat's row
   cleared, then a new skirmish's rows taken from the lever. */
void tagpu_income_level_init(void);
/* The teardown (tagpu_reclaim.c), on the game thread: each armed seat's totals logged. */
void tagpu_income_level_end(void);

#endif
