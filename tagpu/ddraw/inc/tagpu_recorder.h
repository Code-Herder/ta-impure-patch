#ifndef TAGPU_RECORDER_H
#define TAGPU_RECORDER_H
/* tagpu_recorder -- the demo recorder (research/notes/tadr-port/demo-recorder-skirmish-slice.md).
   tagpu_recorder.c is the capture, on the game thread. tagpu_recwriter.c is the writer thread; it
   reads no engine memory, and since it is not on thread-split.allow the build refuses an engine
   address in it. The records that pass between them are tagpu_recfmt.h's. A skirmish or native
   game is saved when tagpu_recorder.on is in the game's folder at the level's first iteration;
   native owner contributions run independently of that local save request. */

#include <windows.h>
#include "tagpu_recfile.h"
#include "tagpu_recmp_file.h"

/* Game-thread VM allocation with a stable START/CALL counter key. */
int tagpu_rec_cob_child(char* cob, unsigned script, int call);

/* B2's stub at the sim step's call of the unit tick (0x4954ED) calls this on the game thread,
   between two iterations, after the replay's capture-point write (tagpu_replay_capture), so that a
   dump on a level that plays a recording holds what the replay wrote there. */
void __cdecl tagpu_rec_capture(void);

/* The step's phase, on the game thread: the stubs on the step's calls of the projectile tick
   (0x495513) and of the explosion tick (0x495539) call this with 1 and 2 before each call, and
   tagpu_rec_capture sets it back to 0. A feature root records the phase it was made in, so that a
   replay makes it at the same point of its own step (tagpu_replay_phase). */
void __cdecl tagpu_rec_phase(unsigned phase);

/* Arms the harness's dump `name` -- 1 to 39 letters, digits, '-' and '_' -- which tagpu_rec_capture
   takes at the capture point of GameTime `at`, or at its next call when `at` is 0, on any level in
   play, recorded or playing (the replay's frame visit arms it from the trigger's `dump <name>
   [at <T>]`, tagpu_replay.c): the level's images as a stored snapshot and the main block and
   .data whole, in replays\dump-<name>.bin. At most 32 are armed at once, each name once; a timed
   one must be for a GameTime after the level's now, and the level's end drops it. 0 when refused,
   logged. Game thread. */
int tagpu_rec_dump_arm(const char* name, unsigned at);
/* A frame dump: taken at once, between two steps on the game thread (the replay's frame visit),
   with RT_FRAME = GameTime. Two frame dumps of one RT_FRAME saw no step between them unless a seek
   moved GameTime back between them: a step raises it once (0x4954C0). 1 written; 0 with a line
   when no level is in play, the name is bad or the dump could not be written. */
int tagpu_rec_dump_now(const char* name);
/* Harness-only observation over [from, until): new AI-issued orders and their
   normal handler dispatches, reported per seat in the log. Game thread. */
int tagpu_rec_ai_arm(const char* name, unsigned from, unsigned until);
/* Acquisition runs for human seats too; end planner attribution before it. */
void tagpu_rec_ai_planning_end(void);

/* tagpu_reclaim's teardown wrap calls this on entry to 0x491B60, on the game thread, before the
   cascade frees the unit array: the recording in progress ends, complete once its writer has
   closed the file. A fork armed and not yet started is dropped, its file closed. */
void tagpu_rec_level_end(void);


/* DllMain's attach: the event hooks (tagpu_recorder.c, *The events*), all or none. Without them
   no skirmish is recorded, and none plays: a replay takes the feature code's draws, a death's
   explosion and the footprint writers' undo at them. tagpu_rec_hooks_in: all of them are in. */
void tagpu_rec_init(void);
int tagpu_rec_hooks_in(void);
/* DllMain's attach, before tagpu_simreg_attach closes the lever list, so a recording's header
   names it: THE TEST LEVER tagpu_rec.decline, honoured only in a test launch
   (tagpu_regstore_active) and refused with a line otherwise. Present, every recording's codec
   declines stream blocks 2, 7, 12, ... (RecStream.decline): the tareplay fixtures codec-decline
   and stock-codec-decline record under it and check that the recording reads back across each
   declined block. */
void tagpu_rec_lever_init(void);

/* Actual delivered span in this thread's owned primary receive frame. Zero
   means the pointer is not that frame's current packet. No engine buffer is
   borrowed; recursive pump depths have independent storage. The native last
   packet survives return until another receive at that depth overwrites it. */
unsigned tagpu_rec_wire_received(const void* message);

/* The running game's mod fingerprint, as a recording's RT_MODPRINT states it: FNV-1a over each
   archive the engine mounted, in mount order, its lowercased name without its folder, NUL, size
   and mtime, Impure's own impure-patch.ufo left out. Game thread: the engine mounts there. */
unsigned tagpu_rec_mod_print(void);

/* The callouts of the events whose sites belong to other modules, on the game thread. Shots
   and intercepts copy into an active recording; owner script roots also contribute in native
   games whose local recording is off.
   - tagpu_rec_shot: a fire function's 0x0D packet at its send (wpn_tx_fired), `meteor` for the
     meteor spawn's, whose sender leaves +0x1A..+0x23 as the stack held them; `start`, when not
     NULL, the 12 bytes the build started the projectile from, recorded at +1 in place of the
     packet's (0x49D9C0 sends the shooter's centre there).
   - tagpu_rec_intercept: area damage's 0x0E, from its target point (three dwords) and the
     weapon's index (wpn_tx_intercept).
   - tagpu_rec_start_named: a start of the script `name` on `unit` that only the owner's machine
     makes, as the engine's own sites of it are recorded (tagpu_weapons.c's TargetCleared of
     the slots from 3 up): `now` is the start's third argument, `a0` its one argument. */
void tagpu_rec_shot(const unsigned char* pkt, int meteor, const unsigned char* start);
void tagpu_rec_intercept(const unsigned char* point, unsigned weapon);
/* B4's existing CreateFromNetwork success observer, after its incarnation stamp.
   The record's first 17 bytes are the constructor's bounded input; no worker reads it. */
void tagpu_rec_network_created(const char* unit, const unsigned char* record, unsigned birth);
void tagpu_rec_start_named(const char* unit, const char* name, int now, int argc, int a0);

/* What a recording's header says about the game it was made in, computed as the header computes
   it, so that playback (tagpu_replay.c) compares like with like: the map's stem from GameingState's
   TNT path (RT_MAP); the unit-type definitions' count and the FNV-1a of their names (RT_UNITDEFS);
   the weapon array's slots and the FNV-1a of their names (RT_WEAPONS, 0 and 0 before the array
   exists). Game thread, `ta` the main block. */
void tagpu_rec_map_stem(const char* ta, char* out, size_t cap);
void tagpu_rec_defs_print(const char* ta, unsigned* n, unsigned* hash);
void tagpu_rec_weapons_print(unsigned* n, unsigned* hash);
/* The feature grid's columns and rows, the feature defs and FNV-1a of their names, as RT_FEATURES
   carries them: what a playing compares with the level it plays on. */
void tagpu_rec_features_print(const char* ta, unsigned* w, unsigned* h, unsigned* ndefs,
                              unsigned* hash);

/* THE EXIT waits for the files of the recordings that have ended: a recording is complete once
   its writer has closed the file, and ExitProcess would kill a writer still writing its last
   block. Every orderly exit calls this first, on whatever thread exits: the exe's CRT, and each
   DLL of its folder, through the fork's ExitProcess hook (hook.c, winapi_hooks.c); the fork's own
   exits, which the hook does not reach, call it themselves -- SC_CLOSE (wndproc.c), tagpu_refuse,
   and the out-of-memory and unit-type refusals (tagpu_patches.c). A crash exit calls
   TerminateProcess and does not wait: the crashed thread may hold a lock a writer needs. A
   recording still in progress has no end to finish, so nothing waits for it and its file reads to
   its last block, incomplete. The wait ends when the files do: a writer never waits for another
   thread and takes no lock the exiting thread holds -- the exe's CRT holds its exit lock across
   ExitProcess, and a writer never enters the exe's CRT. An exit from inside a DllMain holds the
   loader lock, which a writer thread still starting needs, so no exit that waits runs there: the
   fork's exits in its DllMain (the registry store's refusal at attach, the terminate at detach)
   do not call this, and at attach no recording has ended, so there is nothing to wait for. */
void tagpu_rec_exit(void);

/* THE RING, one per recording, between its capture and its writer. The capture writes records
   past `wpub` and then publishes them by moving `wpub` (InterlockedExchange, so every byte it
   wrote is visible first); the writer reads up to `wpub` and gives the bytes back by moving `rpub`
   the same way. Both count bytes modulo 2^32 and the capacity is a power of two, so a count's
   position is `count & (REC_RING_BYTES - 1)` across the wrap. A record never crosses the ring's
   end: an RK_PAD sends the reader back to the start. The capture never writes past
   `rpub + REC_RING_BYTES` and always keeps REC_RING_RESERVE bytes back, so an RK_END fits
   whatever the ring holds: a recording cannot be left without its end.

   THE HAND-OVER. The capture's last access to the ring is the store that publishes RK_END, and it
   writes `wend` and `ended` before that store. The writer frees the ring once `ended` is set and
   `wpub` equals `wend` -- by then the capture is done with it, whatever the writer could parse. The
   capture signals the writer through a duplicate of `wake` that it owns and closes itself, so its
   signal after that store never touches memory or a handle the writer frees. */
#define REC_RING_BYTES    (16u << 20)
/* an RK_END, after the tail of the ring it skips when it does not fit there: records are 4-byte
   aligned, so that tail is at most the record less 4 bytes */
#define REC_RING_RESERVE  (2u * (4u + REC_END_BYTES) - 4u)

/* WAKING THE WRITER. The capture signals `wake` at the start, once REC_WAKE_BYTES have been
   published since its last signal, and at the end; the writer also wakes every
   REC_WRITER_POLL_MS on its own. A signal under Wine is a round trip to the wineserver, which a
   busy server can hold for milliseconds, and the capture must not pay that every iteration. The
   poll only bounds how late the writer sees bytes: what it may read, and when it may free the
   ring, rest on `wpub` and the hand-over alone. REC_WAKE_BYTES is a small part of the ring, so a
   signal comes long before the ring could fill between two polls. */
#define REC_WAKE_BYTES     (1u << 20)
#define REC_WRITER_POLL_MS 100

/* A FORK, one per takeover: the recording a level played, opened for writing on the game thread at
   the takeover's R0 (tagpu_replay.c, TK0), its append point found with the handle held, and the
   handle moved here by tagpu_rec_fork_arm, into the ring by rec_start and into the writer by
   writer_main: one owner at each step, and each closes it on its own failure (tagpu_recorder.c,
   THE FORK'S HANDLE). */
typedef struct RecFork {
    HANDLE             lock;          /* its persistent file lock, held through the writer */
    HANDLE             file;          /* NULL when none                                          */
    unsigned long long append_off;    /* where the fork's segment goes (recr_append_point)       */
    unsigned           id, parent, fork_at;
    unsigned           nstates;
    RecTimelineState   states[REC_TIMELINE_ROWS];
    int                end_open;      /* the segment before it has no end block ...               */
    unsigned           open_id, open_last;   /* ... is this timeline, and ends at this iteration  */
    char               path[MAX_PATH];
} RecFork;

typedef struct RecRing {
    unsigned char* buf;                /* REC_RING_BYTES, from VirtualAlloc                    */
    volatile LONG  wpub;               /* bytes the capture has published                      */
    volatile LONG  rpub;               /* bytes the writer has given back                      */
    volatile LONG  failed;             /* set by the writer when it cannot write: the capture
                                          then ends the recording                              */
    volatile LONG  flush_req, flush_done; /* completed file writes precede the acknowledgement */
    volatile LONG  wend;               /* the `wpub` that publishes RK_END ...                 */
    volatile LONG  ended;              /* ... valid once this is 1                             */
    HANDLE         wake;               /* auto-reset; the writer's handle                      */
    HANDLE         done;               /* manual-reset; the writer's handle, set once the file
                                          is closed: *The exit* waits on the capture's own    */
    char           path[MAX_PATH];     /* the recording's file, named by the capture           */
    char           opened_path[MAX_PATH]; /* writer's actual name; published by flush_done */
    unsigned char* grid;               /* the feature grid's first image, every cell's, from
                                          VirtualAlloc: written by the capture before it
                                          publishes the RK_GRID that names it, so the writer
                                          reads it after that publish; the writer's from then,
                                          freed at its end whether or not it took it. The grid
                                          is larger than the ring on the largest maps (Seven
                                          Islands, 21 MB)                                      */
    RecFork        fork;               /* a fork's file and append point; fork.file NULL for an
                                          ordinary recording, which creates its own file       */
    RecMpFile*     mp;                 /* copied contribution context, set before writer start;
                                          producer end precedes the final ring publication     */
    int            decline;            /* the test lever tagpu_rec.decline (tagpu_rec_lever_init):
                                          the writer's codec declines blocks (RecStream.decline) */
} RecRing;

/* Starts the writer thread on a ring whose first record is RK_HEADER. From then on the writer
   owns the ring's lifetime and frees the buffer, its handles of the events and the ring by the
   hand-over above, and sets `done` last. 0 when no thread could be started, and the ring is still
   the caller's. */
int tagpu_recwriter_start(RecRing* ring);

/* 1 when a fork armed now would be recorded at the next capture point: the teardown wrap, the
   simreg, the event hooks, a free finishing slot, no recording in progress. Game thread. */
int tagpu_rec_fork_ready(char* why, unsigned cap);
/* Arms timeline f->id: the next capture point starts its recording, whatever REC_ON says (a
   fork is always recorded, decision D3). On 1 the recorder owns f->file (f->file = NULL); on 0
   the caller still does. Game thread. */
int tagpu_rec_fork_arm(RecFork* f);

/* Live rewind, game thread only, between simulation steps. Flush asks asynchronously; poll is
   0 pending, 1 complete, -1 failed. Suspension retains the unpublished step's events and all
   capture baselines. Resume compares every feature row before the next iteration is published. */
int tagpu_rec_rewind_open(unsigned* timeline, char* why, unsigned whycap);
int tagpu_rec_rewind_flush(char* path, unsigned cap);
int tagpu_rec_suspend(int on);

/* The bytes of the ring the game thread's recording holds unwritten, 0 when it records none.
   Game thread only. The turbo's back-pressure (tagpu_replay.c, THE TURBO). */
unsigned tagpu_rec_ring_used(void);
typedef void (*RecPendingVisit)(void* ctx, unsigned kind, const unsigned char* body, unsigned len);
void tagpu_rec_pending_events(RecPendingVisit visit, void* ctx);
/* Ends the original with its pending events. The caller owns the returned done-event duplicate
   and must observe it signalled before opening an append writer. NULL leaves recording intact. */
HANDLE tagpu_rec_rewind_end(void);

#endif
