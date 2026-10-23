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
/* Every recording writer holds this persistent file's exclusive handle until its last mutation.
   Never unlink the lock file while another may hold it: unlinking lets two callers lock different
   objects at one path. The prune alone deletes one, through its own exclusive handle: with the
   recording it locks, or one it made itself for a recording it keeps. */
HANDLE recfile_lock(const char* path, char* why, unsigned cap);
void recfile_unlock(HANDLE* lock);

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
   memory, every worker busy, the process exiting). A deletion refused because a playing holds
   the timeline is a queued request whose outcome is that refusal. */
unsigned recfile_request(const char* path, unsigned op, unsigned id, const char* output,
                         const char* request);
/* A ticket's outcome: 0 still queued or running; 1 done; -1 refused, `why` filled; -2 unknown --
   never issued, or finished before the last REC_FILE_RESULTS completions: a result outlives
   REC_FILE_RESULTS - 1 later ones. Any thread. */
#define REC_FILE_RESULTS 16
int recfile_result(unsigned ticket, char* why, unsigned cap);
/* 1 while a queued or running request names `path`, spelled as the request spelled it: for a
   caller that must not take the file's lock before that request is done. Any thread. */
int recfile_pending(const char* path);
/* How many requests have finished: it changes after each, for a reader that wants to know that
   some file may have changed. Any thread, without the queue's lock. */
unsigned recfile_completed(void);
void recfile_exit(void);
/* A playing and a queued deletion reserve canonical path, file identity and timeline under
   the same lock; replacement of the file cannot erase its path reservation.
   A playing cannot start while its deletion is pending, nor can its deletion be queued. */
int recfile_play(const char* path, unsigned id);
void recfile_leave(void);

/* File-only work, called by maintenance workers. No engine state is read. */
int recr_manage(const char* path, unsigned op, unsigned id, const char* output,
                char* why, unsigned cap);
#endif
