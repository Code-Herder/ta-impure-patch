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
   none. Without those a request is never read. Then the browser's two sites: the main menu's
   builder call (0x4270A9) and the RESTART gate (0x460880). */
void tagpu_replay_init(void);
/* File-maintenance triggers, on the game-thread flip observer, at every screen. */
void tagpu_replay_files_frame(unsigned frame);

/* The Replays browser's Watch (tagpu_replays.c), the other way a playback starts
   (tagpu_replay.c, THE BROWSER'S START). On the game thread, inside REPLAYS' OnCommand, `gi` the
   GUI info: 1 when the recording `file` of replays\ is seated and its level is on its way --
   every screen is popped then, so nothing of the browser may be touched after it -- or 0 when it
   was refused, with the engine's notice shown, the click answered and the browser still on top.
   `view` is a seat whose view to show, or -1 for full visibility. `back` is called once, with
   `gi`, when the main menu is next built, unless it goes on to the lobby. */
int tagpu_replay_watch(void* gi, const char* file, int view, void (*back)(void* gi));
/* 1 when Watch can start a recording: the replay's sites are in and the engine code its route
   rests on reads as it was made against; else 0, with the reason in `why`. */
int tagpu_replay_watch_ready(char* why, unsigned cap);
/* 1 when a writer holds the recording at `path` open: the one the game is writing, which neither
   Watch nor the browser offers. Any thread; it opens the file for a moment. */
int tagpu_replay_being_written(const char* path);

/* WATCH'S DATA REFUSALS: the refusals whose verdict follows from this game's unit, weapon or
   feature data, or from this build's replay identity, and not from the recording alone. Each is
   handed to the browser as it is made (tagpu_replays_refused), with the running values it
   compared: the level's count and hash for the first three, 0 and the build key for the last
   two. */
enum {
    TAGPU_REPLAY_REFUSED_UNITS = 1,     /* RT_UNITDEFS against the level's unit types         */
    TAGPU_REPLAY_REFUSED_WEAPONS,       /* RT_WEAPONS against the level's weapon table         */
    TAGPU_REPLAY_REFUSED_FEATURES,      /* the feature grid, wreck pool and feature types      */
    TAGPU_REPLAY_REFUSED_LAYOUT,        /* another replay-layout.tsv                           */
    TAGPU_REPLAY_REFUSED_GLOBALS        /* another replay-globals.tsv                          */
};
/* What this build brings to those checks and the install does not: the exclusion file's and the
   positive list's hashes, the wreck pool's size and the weapon table's slots. Constant for a
   process once the limits are installed. */
unsigned tagpu_replay_build_key(void);

/* B2's stub at the sim step's unit-tick call (0x4954ED) calls this first, before the recorder's
   capture and the unit tick, on the game thread: on a level that plays a recording, the
   recording's globals and projectile pool at the capture point (tagpu_replay.c, THE GLOBALS AND
   THE PROJECTILES). */
void __cdecl tagpu_replay_capture(void);

/* B2's stub at the sim step's unit-tick call (0x4954ED), after the unit tick returns and before
   the rest of the iteration, on the game thread. */
void __cdecl tagpu_replay_step(void);

/* The stubs on the step's calls of the projectile tick (0x495513) and of the explosion tick
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

/* The watching dock's requests (tagpu_dock.h), on the game thread.
   live: the level plays a recording and its first step has played, which was not refused.
   span: the recording's first and last GameTime, from its summary at the open; 0 when none.
   path: "replays\\<file>" while a recording is open, else NULL.
   seek_request: a seek to T, clamped to [first + 1, last], for the next frame visit; 0 refused.
   paused / pause: bit 0 of main+0x38A51, the Pause key's bit.
   speed / speed_set: the game speed's ceiling main+0x38A4B, 10 = 1x; speed_set takes 5, 10, 20, 40
   or 80 and writes the live speed too, and the teardown puts the player's back. */
int  tagpu_replay_live(void);
int  tagpu_replay_span(unsigned* first, unsigned* last);
const char* tagpu_replay_path(void);
int  tagpu_replay_seek_request(unsigned T);
int  tagpu_replay_paused(void);
void tagpu_replay_pause(int on);
int  tagpu_replay_speed(void);
int  tagpu_replay_speed_set(unsigned gs);

/* tagpu_reclaim's teardown wrap, once 0x491B60 has returned, on the thread that entered it: V's
   PlayerInfo gets back what the Start's fill left in it. */
void tagpu_replay_level_after(void);

/* 1 while a playing waits at a timeline boundary for its seek (tagpu_replay.c, THE BOUNDARY): the
   recorder's armed dumps wait with it. Game thread. */
int tagpu_replay_held(void);

#endif
