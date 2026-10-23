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
   level that plays a recording and writes what the draw shows there. Independent observers
   reassert pause while rewind owns the level or a test's pause holds it (0x496920) and set the
   scheduler's tick batch before its count is cached (0x49692D): a test launch's turbo, then the
   bounds that give armed round trips their T-1 frame, a pause at T its T and the launch's hold
   its one tick; then the Start's calls of the seat fill
   (0x47B092) and of the settings persist (0x47B09C), the loader's two start-unit calls (0x497963,
   0x497ACD) and an observer on the settings save (0x430F00), byte-matched and installed all or
   none. Without those a request is never read. Then the browser's two sites: the main menu's
   builder call (0x4270A9) and the RESTART gate (0x460880). */
void tagpu_replay_init(void);
/* The test pacing's levers (tagpu_replay.c), each honoured only in a test launch
   (tagpu_regstore_active) -- configurations no player runs -- and refused with a line otherwise.
   DllMain's attach, before tagpu_simreg_attach closes the lever list, so every recording's header
   names them.
   - THE TURBO's k: tagpu_turbo.on, its text a k of 1 to 5 (empty is 1). Nothing changes until the
     trigger's `turbo on`.
   - THE LAUNCH'S HOLD: tagpu_hold.on present. The level's first batch is one tick and the level
     is held after it, until the trigger's `resume`. */
void tagpu_replay_pacing_init(void);
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
   THE PROJECTILES). At the level's first iteration it first makes the level's own units and
   writes image T (THE FIRST STEP'S UNITS), and then makes that write. */
void __cdecl tagpu_replay_capture(void);

/* B2's stub at the sim step's unit-tick call (0x4954ED), after the unit tick returns and before
   the rest of the iteration, on the game thread. */
void __cdecl tagpu_replay_step(void);

/* The unit tick's stub (tagpu_patches.c, THE SCRIPT SLICE), on the game thread, for each unit whose
   script call is reached: 1 when a level that plays a recording gives this unit's slice where the
   recording places it instead (tagpu_replay.c, THE SCRIPT SLICES), and the unit tick skips it. The
   unit is named by arithmetic against the level's unit array, never read. */
int __cdecl tagpu_replay_slice_defer(const char* unit);

/* The stubs on the step's calls of the projectile tick (0x495513) and of the explosion tick
   (0x495539), each once its call has returned, on the game thread, with REC_PHASE_PROJ and
   REC_PHASE_LATE: the feature roots the recording machine made in that phase of its step are
   made here, and after the late phase's the step's feature images go in (tagpu_replay.c, THE
   PHASES). */
void __cdecl tagpu_replay_phase(unsigned phase);

/* The destructor's explosion has returned (tagpu_recorder.c, rec_blast, at 0x486D50), on the game
   thread. While a skirmish recording's death is replayed, the feature calls the recording's
   explosion made are made here: the host's explosion makes none, its owner being a puppet seat
   (the owner gate, 0x49A01B..0x49A047). */
void tagpu_replay_blast(const char* unit);

/* Reproduction's source test (tagpu_recorder.c, feat_source) on a multiplayer recording's level:
   1 with the slot word the recording's tick read for cell n -- the next armed draw, when it is
   that cell's FS_SOURCE; 0 on any other level, with the armed draws untouched, and the engine
   tests its own slot. Game thread, inside the replay's tick root. */
int tagpu_replay_source(unsigned n, unsigned* v);

/* tagpu_reclaim's teardown wrap, on entry to 0x491B60, on the game thread: the level's replay
   ends, the skirmish settings get back what the player had, and the puppet table is cleared. */
void tagpu_replay_level_end(void);

/* The replay HUD's requests (tagpu_hudinput.c), on the game thread.
   live: the level plays a recording and its first step has played, which was not refused.
   span: the chain that plays, its root's first GameTime and its last timeline's last; 0 when
   none.
   path: "replays\\<file>" while a recording is open, else NULL; timeline: the timeline of it
   that plays, 0 the original -- a rewind into a fork, or a switch, changes it within the level.
   seek_request: a seek to T, clamped to [first + 1, last], for the next frame visit; 0 refused.
   paused / pause: bit 0 of main+0x38A51, the Pause key's bit.
   speed / speed_set: the game speed's ceiling main+0x38A4B, 10 = 1x; speed_set takes 5, 10, 20, 40
   or 80 and writes the live speed too, and the teardown puts the player's back. */
int  tagpu_replay_live(void);
int  tagpu_replay_span(unsigned* first, unsigned* last);
const char* tagpu_replay_path(void);
unsigned    tagpu_replay_timeline(void);
int  tagpu_replay_seek_request(unsigned T);
int  tagpu_replay_paused(void);
void tagpu_replay_pause(int on);
int  tagpu_replay_speed(void);
int  tagpu_replay_speed_set(unsigned gs);

/* THE HUD'S RECORDER CALLS (tagpu_replay.c, THE HUD'S RECORDER CALLS), game thread only, between
   two sim steps, never inside one.

   THE TAKEOVER'S PLAN: where a takeover asked for at T starts and what its fork would delete,
   found as the takeover finds them (takeover_point) and committing nothing. It reads the file's
   block headers three times, so it answers a click and is never called per frame. R is the refresh at
   or before T (tk_bound's bound); `next` is set only when T rounds to a timeline's first
   iteration, which no takeover starts at, and names the first GameTime one can (R + 1). 1 with
   R, or 0 with `why` and R = 0. A replay playing, or a live rewind gone into the past
   (TAGPU_REPLAY_RWS_PAST), whose fork the plan then predicts. */
typedef struct TAGPU_REPLAY_TKPLAN {
    unsigned R;
    unsigned next;
    unsigned evict;          /* the timeline the fork would delete, least recently used;
                                UINT32_MAX none */
    char     why[160];
} TAGPU_REPLAY_TKPLAN;
int tagpu_replay_takeover_plan(unsigned T, TAGPU_REPLAY_TKPLAN* out);
/* The takeover as player P at T, for the next frame visit (THE TAKEOVER), D -1 for the
   timeline's own difficulty: rounded to the refresh at or before T and refused at once when no
   fork can be appended there (takeover_point). 1 queued; a later seek or takeover replaces it. */
int tagpu_replay_takeover_request(unsigned P, unsigned T, int D);

/* THE TIMELINES of the playing's file, as the last scan found them (THE TIMELINES CACHE): every
   segment, root first, at most `cap`; their count, 0 when no recording is open. `on_chain`: a
   segment of the chain that plays. `controlled`: the seat its header's RT_OPTIONS names, the
   one a fork was played as. The axis: the root's first GameTime and the greatest last
   GameTime of a live timeline. */
typedef struct TAGPU_REPLAY_TIMELINE {
    unsigned id, parent, fork_at, first, last, controlled;
    int      live, on_chain;
} TAGPU_REPLAY_TIMELINE;
int tagpu_replay_timelines(TAGPU_REPLAY_TIMELINE* out, int cap);
int tagpu_replay_axis(unsigned* first, unsigned* last);
/* One more at every change of what those two give, the playing's end included: a caller that
   remembers it knows when to read them, and to re-plan a takeover, again. */
unsigned tagpu_replay_timelines_serial(void);

/* A fork of the playing's file deleted or exported (replays\<stem>.imprec), by the file worker:
   accepted, its ticket, never 0, which its outcome carries when tagpu_replay_result reports it
   (kind FORK) once the frame visit has taken it; 0 refused at once, with a line and that result. The deletion of the
   timeline that plays is a switch to the nearest live timeline up its chain first (THE SWITCH),
   the deletion queued once that switch is done, and refused when it is refused. */
enum { TAGPU_REPLAY_FORK_DELETE = 1, TAGPU_REPLAY_FORK_EXPORT };
unsigned tagpu_replay_fork_op(int op, unsigned id, const char* stem);

/* LIVE REWIND (tagpu_replay.c, THE LIVE REWIND), the trigger's verbs: queued for the next frame
   visit, the latest replacing an earlier one not yet taken. T is SEEK's; `dump` a frame dump
   the operation's acknowledgement takes (a test's), NULL for none. 1 queued; 0 refused: no
   level in play, an unknown verb, or a dump name that is not letters, digits, - and _. */
enum { TAGPU_REPLAY_RW_OPEN, TAGPU_REPLAY_RW_SEEK, TAGPU_REPLAY_RW_NOW, TAGPU_REPLAY_RW_FORK,
       TAGPU_REPLAY_RW_CLOSE };
int tagpu_replay_rewind(int op, unsigned T, const char* dump);
enum { TAGPU_REPLAY_RWS_IDLE, TAGPU_REPLAY_RWS_FLUSH, TAGPU_REPLAY_RWS_OPEN,
       TAGPU_REPLAY_RWS_PAST, TAGPU_REPLAY_RWS_ENDING };
typedef struct TAGPU_REPLAY_RWSTATE {
    int      state;          /* TAGPU_REPLAY_RWS_*                                          */
    int      available;      /* idle, and OPEN would be taken: a live, recorded, non-network
                                skirmish whose recorder is healthy                           */
    unsigned first, last;    /* the live recording's span while one is open                 */
    unsigned G;              /* the GameTime the rewind opened at: now                       */
    unsigned T;              /* the last seek's target; 0 none yet                           */
} TAGPU_REPLAY_RWSTATE;
void tagpu_replay_rewind_state(TAGPU_REPLAY_RWSTATE* out);

/* THE LAST REQUEST'S OUTCOME: of the seeks, the takeovers, the switches, the rewind verbs and
   the fork operations, the one consumed last. `serial` grows by one at each, so a caller that
   remembers it sees an outcome once; two consumed between its reads show it the later only, so a
   caller waiting for one outcome matches it by its ticket or id and never takes another's. */
enum { TAGPU_REPLAY_RQ_NONE, TAGPU_REPLAY_RQ_TAKEOVER, TAGPU_REPLAY_RQ_REWIND,
       TAGPU_REPLAY_RQ_FORK, TAGPU_REPLAY_RQ_SEEK, TAGPU_REPLAY_RQ_SWITCH };
typedef struct TAGPU_REPLAY_RESULT {
    unsigned serial;
    int      kind, op, ok;   /* op: the rewind verb or the fork op                          */
    unsigned ticket;         /* a fork op's: what tagpu_replay_fork_op returned              */
    unsigned id;             /* the timeline a switch or a fork op named; 0 for the others   */
    char     why[96];        /* the refusal's reason, or what was done                       */
} TAGPU_REPLAY_RESULT;
void tagpu_replay_result(TAGPU_REPLAY_RESULT* out);

/* THE SWITCH (tagpu_replay.c, THE SWITCH): the playing goes on in timeline `id` of the same file
   from the GameTime it is at, for the next frame visit -- in place when the history it has
   played is id's too, else by a seek onto id's chain. 1 queued (the latest replaces an earlier
   one not yet taken), 0 refused at once; either way its outcome is a TAGPU_REPLAY_RQ_SWITCH
   result. */
int tagpu_replay_watch_timeline(unsigned id);

/* The replay HUD's rows, game thread only. Rows stay in the recording header's order, including defeated
   seats. Values come from the player's current recorded image, held after EOF -- in a multiplayer
   recording the composed one, its owners' (THE MULTIPLAYER MATERIALIZER). Sight is independent of the
   camera: -1 selects the full-map viewer, otherwise a recorded seat index. The request runs at
   the frame boundary and rebuilds death vision, minimap fog and contacts before publishing the
   selected seat. */
typedef struct TAGPU_REPLAY_PLAYER {
    unsigned seat;
    unsigned colour;     /* a palette index: the middle of the seat's frame in the engine's
                            colour sequence, as the overlays draw the seat */
    unsigned units, commander_losses;
    int commander_alive; /* 1 present, 0 absent, -1 unavailable */
    int pov;             /* the seat's camera was recorded and a sample is held: Jump and Follow */
    char name[32];
    float metal, energy, metal_in, metal_out, energy_in, energy_out;
    float metal_max, energy_max;    /* the storage maxima the economy pass sums */
    unsigned allies;                /* bit j: the alliance row marks recorded seat j */
} TAGPU_REPLAY_PLAYER;
int tagpu_replay_players(TAGPU_REPLAY_PLAYER* out, int cap);
/* The army value of one finished unit of each type, by the type's index (a unit's +0xA6), for the
   stats window's walker (tagpu_hudmarks.c): its metal cost when the type moves and is not a
   commander, 0 otherwise. Up to `cap` of them into `out`; the count, 0 when no recording plays.
   With `out` NULL, the level's count of types. Game thread only. */
unsigned tagpu_replay_army_values(float* out, unsigned cap);
int tagpu_replay_sight(void);
int tagpu_replay_sight_request(int seat);
/* Follow, Jump and Free cam, game thread only (tagpu_replay.c, FOLLOW, JUMP AND FREE CAM): a HUD
   row's Follow and Jump, and Follow pressed again. `seat` a recorded seat whose camera was recorded, its Follow or,
   with `once`, its Jump; -1 Free cam. Applied at the next frame visit; 0 refused, with a log line
   naming why. followed: the seat a Follow shows, -1 under Free cam (a Jump included). */
int tagpu_replay_follow_request(int seat, int once);
int tagpu_replay_followed(void);

/* THE OVERLAYS, game thread only (tagpu_replay.c, THE OVERLAYS): every recorded player's cursor,
   clicks and selection, built at the frame visit from the recording, and the four switches of the
   HUD's foot plate -- the three overlays and the chat -- each on at every playing's start and
   never saved. `which` is a TAGPU_OV_*; a switched-off overlay produces nothing. */
enum { TAGPU_OV_CURSORS, TAGPU_OV_CLICKS, TAGPU_OV_SELECTIONS, TAGPU_OV_CHAT, TAGPU_OV_N };
int  tagpu_replay_overlay(int which);
void tagpu_replay_overlay_set(int which, int on);
/* The publisher's two reads, after the draw the frame visit precedes: this frame's cursor and
   click items, at most `cap`, with each recorded player's palette index in `col` (10 rows), and
   a unit slot's recorded-selection bits, bit p for player p. Nothing outside a playback. */
struct TAGPU_PK_OVERLAY;
unsigned tagpu_replay_overlays(struct TAGPU_PK_OVERLAY* out, unsigned cap, unsigned char col[10]);
unsigned tagpu_replay_rsel(unsigned slot);

/* tagpu_reclaim's teardown wrap, once 0x491B60 has returned, on the thread that entered it: V's
   PlayerInfo gets back what the Start's fill left in it. */
void tagpu_replay_level_after(void);

/* 1 while a playing waits at a timeline boundary for its seek (tagpu_replay.c, THE BOUNDARY): the
   recorder's armed dumps wait with it. Game thread. */
int tagpu_replay_held(void);

#endif
