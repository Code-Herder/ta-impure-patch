#ifndef TAGPU_RECFILE_H
#define TAGPU_RECFILE_H
#include <stdint.h>
#include <windows.h>

/* RB_TIMELINES rows are bounded independently of IDs: a compacted file retains the highest
   allocated ID's tombstone even when its segment is gone, so an ID is never reused. */
#define REC_TIMELINE_ROWS 65
#define REC_FORK_LIMIT 3
#define REC_TL_LIVE 1
#define REC_TL_DELETED 2
typedef struct RecTimelineState {
    uint32_t id;
    uint8_t state, reserved[3];
    uint32_t used_low, used_high;
} RecTimelineState;
_Static_assert(sizeof(RecTimelineState) == 16, "a timeline state row");

int recmeta_parse(RecTimelineState* out, unsigned* count, const void* raw, unsigned bytes);
uint64_t recfile_now(void);
/* THE PATH LOCK: `path`.lock, opened with share mode 0, so one caller at a time holds it.
   THE INVARIANT: every caller in this program that creates, replaces or deletes a recording holds
   that recording's lock from before it looks for the file until its last change of it -- the
   writer, which creates its file CREATE_NEW after the lock (tagpu_recwriter.c, file_open); a
   takeover's fork, which appends to the file it opened under the lock, through the ring and the
   writer; maintenance (recr_manage), whose compaction replaces the file; an export, which holds
   its output's lock besides its source's from before its temporary file to after the move that
   makes the output; and the prune (prune_one), which deletes a recording under its lock on its own
   handle. So while the lock is held, no caller here can make its recording appear or go.
   recfile_unlock(lock, path), `path` the path it was taken for: when the recording does not
   exist, the lock is marked for deletion through the held handle before it closes, so no lock
   outlives its recording -- a touch or a takeover asked of a recording the prune has deleted
   leaves nothing behind. Deleting through the exclusive handle never lets two callers hold two
   lock objects of one path: no other open can share the handle, and the name goes only at its
   close; a lock deleted early costs the next caller a recreate, never a second lock object.
   Not covered: a recording made or deleted outside the program (a deletion leaves its lock until
   the next caller of that path releases it), and XP, which has no SetFileInformationByHandle and
   keeps every lock. */
HANDLE recfile_lock(const char* path, char* why, unsigned cap);
void recfile_unlock(HANDLE* lock, const char* path);
/* `f`, opened with DELETE access, marked for deletion at its close (`on`) or its mark lifted: 1
   done, 0 refused, or where there is no SetFileInformationByHandle (XP) */
int recfile_delete_on_close(HANDLE f, int on);

/* THE PRUNE'S ORDER, which the Replays browser lists by too (tagpu_replays.c, scan): < 0 when the
   file named `x`, whose recorded start `tx` recr_wallclock_of read with answer `dx`, comes before
   `y`'s. A strict weak order, newest first: by the recorded start, every file without one (an
   answer of 0 or -1) after every dated file, then by name, latest first (a recording's name begins
   with its local start time). Names equal under the case-insensitive comparison keep an
   unspecified order between them. */
int recfile_newer_first(int dx, const FILETIME* tx, const char* x, int dy, const FILETIME* ty,
                        const char* y);

enum RecFileOp { REC_FILE_TOUCH, REC_FILE_DELETE, REC_FILE_EXPORT, REC_FILE_COMPACT, REC_FILE_PRUNE };
/* THE PRUNE. REC_FILE_PRUNE, requested as a new recording starts (`id` = how many to keep, `path`
   the new recording, one of the `id`): the other replays\*.imprec newest first by the start their
   header records (RT_WALLCLOCK, recr_wallclock_of), the first `id` - 1 kept, and each older one
   deleted unless it is in use, has a live fork, or is a favourite or bookmarked in
   replays\index.dat. A file with no recorded start sorts after every dated one, then by name,
   latest first: one that is not one of our recordings, one whose header lacks the entry at 12
   bytes (every recording of format 24 or earlier), and one that cannot be opened or read at the
   moment, which is kept and counted in use whatever its place. A file that is not one of our
   recordings is kept; one of another format is deleted like any other (this build can neither
   read its forks nor play it).
   "Newest" is the recording's own date: the latest start by the clock of the machine that
   recorded it, whatever the file's dates. So watching a replay does not keep it, nor does
   exporting one (the export carries its root's start); a favourite or a bookmark does. A clock
   set back makes each new recording the oldest at the next game's prune, and a file dated in the
   future holds one of the kept places until the clock passes it.
   REC_KEEP is the count when the recorder's lever names none (`keep N`, tagpu_recorder.c). */
#define REC_KEEP 50
/* The worker owns copies of the paths. A full queue or an exiting process refuses the request;
   it never runs file maintenance on the caller's thread. The request's TICKET, never 0, whose
   outcome recfile_result gives; 0 when it is refused before it is queued (a bad argument, no
   memory, every worker busy, the process exiting). A deletion refused because a playing claims
   the timeline (THE CLAIMS) is a queued request whose outcome is that refusal. */
unsigned recfile_request(const char* path, unsigned op, unsigned id, const char* output,
                         const char* request);
/* A ticket's outcome: 0 still queued or running; 1 done; -1 refused, `why` filled; -2 unknown --
   never issued, or finished before the last REC_FILE_RESULTS completions: a result outlives
   REC_FILE_RESULTS - 1 completions after its own. Any thread. */
#define REC_FILE_RESULTS 16
int recfile_result(unsigned ticket, char* why, unsigned cap);
/* 1 while a queued or running request names `path`, spelled as the request spelled it: for a
   caller that must not take the file's lock before that request is done. Any thread. */
int recfile_pending(const char* path);
/* How many requests have finished: it changes after each, for a reader that wants to know that
   some file may have changed. Any thread, without the queue's lock. */
unsigned recfile_completed(void);
void recfile_exit(void);
/* REC_FILE_TOUCH with every row a playing stamped, the first its last timeline: the ticket, or 0
   as recfile_request refuses. */
unsigned recfile_touch(const char* path, const RecTimelineState* rows, unsigned n);
/* THE CLAIMS. The timeline a playing chooses -- the one it plays, and the one a switch moves to
   -- is claimed before the scan it is chosen from and released after its reader stops playing
   it. A deletion of a claimed timeline is refused, and a claim of a timeline with a deletion
   queued is refused. A reader that only reads, the HUD's walker's, claims nothing: what keeps its
   bytes is the file's lifetime (tagpu_replay.c, THE SWITCH).
   A claim and a queued deletion both name a canonical path, a file identity and a timeline,
   under the queue's lock; replacement of the file cannot erase its path reservation.
   recfile_play: the playing's claim, every other cleared; 0 when that timeline's deletion is
   queued or the file cannot be opened. recfile_claim: one more claim, 0 refused as recfile_play
   refuses or when both are taken. recfile_release: the first claim of timeline `id` cleared;
   recfile_leave: every claim. Game thread. */
int recfile_play(const char* path, unsigned id);
int recfile_claim(const char* path, unsigned id);
void recfile_release(unsigned id);
void recfile_leave(void);

/* File-only work, called by maintenance workers. No engine state is read. `rows`, a touch's
   alone (NULL, 0 for every other op): the timelines a playing stamped, the first `id`, its last
   timeline. A touch sets each row's live timeline's last-used time to the later of the file's and
   the row's, skips a row whose timeline is absent or deleted, and refuses when no row's is
   live. */
int recr_manage(const char* path, unsigned op, unsigned id, const char* output,
                const RecTimelineState* rows, unsigned nrows, char* why, unsigned cap);
#endif
