#ifndef TAGPU_REPLAY_H
#define TAGPU_REPLAY_H
/* tagpu_replay -- playing a demo recording back on puppets
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S2b; demo-recorder-s2-host.md §6).
   `tacli replay play` writes the skirmish settings from the recording's header, copies the
   recording into replays\, writes the request tagpu_replay.play and clicks Start. The SKIRMISH
   Start then loads a level whose recorded players are puppet seats (tagpu_puppet.h), and this
   module applies the recording to them, iteration by iteration. */

/* DllMain's attach: the Start's calls of the seat fill (0x47B092) and of the settings persist
   (0x47B09C), and the loader's two start-unit calls (0x497963, 0x497ACD), byte-matched and
   installed all or none. Without them a request is never read. */
void tagpu_replay_init(void);

/* B2's stub at the sim step's unit-tick call (0x4954ED), after the unit tick returns and before
   the rest of the iteration, on the game thread. */
void __cdecl tagpu_replay_step(void);

/* tagpu_reclaim's teardown wrap, on entry to 0x491B60, on the game thread: the level's replay
   ends, the skirmish settings get back what the player had, and the puppet table is cleared. */
void tagpu_replay_level_end(void);

#endif
