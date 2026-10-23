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
   objects at one path. The prune alone deletes one, through its own exclusive handle, with the
   recording it locks. */
HANDLE recfile_lock(const char* path, char* why, unsigned cap);
void recfile_unlock(HANDLE* lock);

enum RecFileOp { REC_FILE_TOUCH, REC_FILE_DELETE, REC_FILE_EXPORT, REC_FILE_COMPACT, REC_FILE_PRUNE };
/* THE PRUNE. REC_FILE_PRUNE, requested as a new recording starts (`id` = how many to keep, `path`
   the new recording, one of the `id`): the other replays\*.imprec newest first by last write, the
   first `id` - 1 kept, and each older one deleted unless it is in use, has a live fork, or is a
   favourite or bookmarked in replays\index.dat. A file that does not read as a recording is kept.
   REC_KEEP is the count when the recorder's lever names none (`keep N`, tagpu_recorder.c). */
#define REC_KEEP 50
/* The worker owns copies of the paths. A full queue or an exiting process refuses the request;
   it never runs file maintenance on the caller's thread. */
int recfile_request(const char* path, unsigned op, unsigned id, const char* output,
                    const char* request);
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
