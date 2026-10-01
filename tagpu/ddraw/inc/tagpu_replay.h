#ifndef TAGPU_REPLAY_H
#define TAGPU_REPLAY_H
/* tagpu_replay -- playing a demo recording back on puppets
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S2b; demo-recorder-s2-host.md §6).
   `tacli replay play` writes the skirmish settings from the recording's header, copies the
   recording into replays\, writes the request tagpu_replay.play and clicks Start. The SKIRMISH
   Start then loads a level whose recorded players are puppet seats (tagpu_puppet.h), and this
   module applies the recording to them, iteration by iteration. */

/* DllMain's attach: first the frame visit, the in-play draw call 0x4969CD, on its own -- it reads
   the trigger tagpu_replay.trigger, whose `dump` serves live recordings too, runs a `seek` on a
   level that plays a recording and writes what the draw shows there; then the Start's calls of the seat fill
   (0x47B092) and of the settings persist (0x47B09C), the loader's two start-unit calls (0x497963,
   0x497ACD) and an observer on the settings save (0x430F00), byte-matched and installed all or
   none. Without those a request is never read. */
void tagpu_replay_init(void);

/* B2's stub at the sim step's unit-tick call (0x4954ED) calls this first, before the recorder's
   capture and the unit tick, on the game thread: on a level that plays a recording, the
   recording's globals and projectile pool at the capture point (tagpu_replay.c, THE GLOBALS AND
   THE PROJECTILES). */
void __cdecl tagpu_replay_capture(void);

/* B2's stub at the sim step's unit-tick call (0x4954ED), after the unit tick returns and before
   the rest of the iteration, on the game thread. */
void __cdecl tagpu_replay_step(void);

/* The stubs on the step's calls of the projectile tick (0x495513) and of the falling records
   (0x495539), each once its call has returned, on the game thread, with REC_PHASE_PROJ and
   REC_PHASE_LATE: the feature roots the recording machine made in that phase of its step are
   made here, and after the late phase's the step's feature images go in (tagpu_replay.c, THE
   PHASES). */
void __cdecl tagpu_replay_phase(unsigned phase);

/* The destructor's explosion has returned (tagpu_recorder.c, rec_blast, at 0x486D50), on the game
   thread. While a death is replayed, the feature calls the recording's explosion made are made
   here: the host's explosion makes none, its owner being a puppet seat (the owner gate,
   0x49A01B..0x49A047). */
void tagpu_replay_blast(const char* unit);

/* tagpu_reclaim's teardown wrap, on entry to 0x491B60, on the game thread: the level's replay
   ends, the skirmish settings get back what the player had, and the puppet table is cleared. */
void tagpu_replay_level_end(void);

/* tagpu_reclaim's teardown wrap, once 0x491B60 has returned, on the thread that entered it: V's
   PlayerInfo gets back what the Start's fill left in it. */
void tagpu_replay_level_after(void);

#endif
