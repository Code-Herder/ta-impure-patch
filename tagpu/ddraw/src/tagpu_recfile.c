/* Recording metadata and file maintenance. A persistent path lock orders cooperating writers
   from the scan through closing the source and atomically replacing it; see recr_manage. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_recfile.h"
#include "tagpu_recreader.h"
#include "tagpu_replays.h"
#include "tagpu_log.h"

int recmeta_parse(RecTimelineState* out, unsigned* count, const void* raw, unsigned bytes)
{
    const unsigned char* p = (const unsigned char*)raw;
    unsigned i, j, n = bytes / sizeof *out;
    if (!n || n > REC_TIMELINE_ROWS || bytes % sizeof *out) return 0;
    for (i = 0; i < n; i++) {
        RecTimelineState row;
        memcpy(&row, p + i * sizeof row, sizeof row);
        if (row.id == UINT32_MAX || (row.state != REC_TL_LIVE && row.state != REC_TL_DELETED) ||
            row.reserved[0] || row.reserved[1] || row.reserved[2]) return 0;
        for (j = 0; j < i; j++) if (out[j].id == row.id) return 0;
        out[i] = row;
    }
    *count = n;
    return 1;
}

uint64_t recfile_now(void)
{
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

/* SetFileInformationByHandle, resolved at run time so that the DLL still loads on XP, which has
   none: NULL there */
typedef BOOL (WINAPI* SetInfoFn)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
static SetInfoFn set_info(void)
{
    return (SetInfoFn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                            "SetFileInformationByHandle");
}

/* DELETE access, so that recfile_unlock can mark the lock for deletion on this handle */
HANDLE recfile_lock(const char* path, char* why, unsigned cap)
{
    char name[MAX_PATH];
    HANDLE f;
    if (strlen(path) + 5 >= sizeof name) {
        snprintf(why, cap, "the recording lock path is too long");
        return NULL;
    }
    snprintf(name, sizeof name, "%s.lock", path);
    f = CreateFileA(name, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_HIDDEN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, cap, "the recording is busy or its lock cannot be opened (%lu)",
                 GetLastError());
        return NULL;
    }
    return f;
}

/* THE LOCK'S END (tagpu_recfile.h, THE PATH LOCK): the recording's absence is read while the
   lock is still held, when no Impure caller can change it, and only a definite "not found"
   deletes; any other failure to read it keeps the lock, as a recording that exists does. */
void recfile_unlock(HANDLE* lock, const char* path)
{
    FILE_DISPOSITION_INFO gone = { TRUE };
    SetInfoFn set;
    DWORD error;
    if (!*lock) return;
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        if ((error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) && (set = set_info()))
            set(*lock, FileDispositionInfo, &gone, sizeof gone);
    }
    CloseHandle(*lock);
    *lock = NULL;
}

#define FILE_JOBS 8
static HANDLE s_jobs[FILE_JOBS];
static volatile LONG s_lock;
static int s_exiting;
typedef struct FileJob {
    char path[MAX_PATH], output[MAX_PATH], key[MAX_PATH], request[33];
    unsigned op, id, ticket;
    BY_HANDLE_FILE_INFORMATION identity;
    RecTimelineState rows[REC_TIMELINE_ROWS];   /* a touch's stamps (recfile_touch) */
    unsigned nrows;
} FileJob;
static FileJob* s_pending[FILE_JOBS];
/* THE TICKETS. A request's outcome waits in a ring of REC_FILE_RESULTS slots, filled in the order
   the requests finish: the k-th completion writes slot k % N with its ticket.
   THE INVARIANT, an ordering under jobs_lock: from its issue until N - 1 completions after its
   own, a ticket is in s_pending or in the ring at every instant the lock is free -- the worker
   writes the result before the job leaves s_pending, both under the lock, and the in-use refusal
   writes its result in the same hold of the lock that issues the ticket. So a result outlives
   N - 1 completions after its own, and a ticket the ring no longer holds reads unknown (-2),
   never another request's outcome: each slot names its ticket. */
static unsigned s_ticket;           /* the last issued; 0 is never one */
static struct { unsigned ticket; int ok; char why[160]; } s_done[REC_FILE_RESULTS];
static volatile LONG s_completed;   /* requests finished, written under the lock */
/* THE CLAIMS (tagpu_recfile.h): the timeline that plays, and the one a switch moves to while
   its reader is open beside the playing's (tagpu_replay.c, THE SWITCH) */
#define REC_FILE_CLAIMS 2
static struct {
    int on;
    unsigned id;
    char key[MAX_PATH];
    BY_HANDLE_FILE_INFORMATION identity;
} s_claim[REC_FILE_CLAIMS];

static void jobs_lock(void) { while (InterlockedExchange(&s_lock, 1)) Sleep(0); }
static void jobs_unlock(void) { InterlockedExchange(&s_lock, 0); }

static int file_identity(const char* path, BY_HANDLE_FILE_INFORMATION* identity)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = GetFileInformationByHandle(f, identity);
    CloseHandle(f);
    return ok;
}

static int same_identity(const BY_HANDLE_FILE_INFORMATION* a,
                         const BY_HANDLE_FILE_INFORMATION* b)
{
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
           a->nFileIndexHigh == b->nFileIndexHigh && a->nFileIndexLow == b->nFileIndexLow;
}

static int path_key(const char* path, char* key)
{
    DWORD n = GetFullPathNameA(path, MAX_PATH, key, NULL);
    return n && n < MAX_PATH;
}

/* Replacement changes an inode but not its canonical path. Identity also protects an alias
   while it still names the same file. Both predicates must be kept across publication. */
static int same_reservation(const char* akey, const BY_HANDLE_FILE_INFORMATION* a,
                            const char* bkey, const BY_HANDLE_FILE_INFORMATION* b)
{
    return !_stricmp(akey, bkey) || same_identity(a, b);
}

/* a deletion of timeline `id` of that file queued or running; under the lock */
static int delete_pending(const char* key, const BY_HANDLE_FILE_INFORMATION* identity,
                          unsigned id)
{
    unsigned i;
    for (i = 0; i < FILE_JOBS; i++) {
        const FileJob* j = s_pending[i];
        if (j && j->op == REC_FILE_DELETE && j->id == id &&
            same_reservation(key, identity, j->key, &j->identity)) return 1;
    }
    return 0;
}

/* claim `c` set to timeline `id` of that file; under the lock */
static void claim_put(unsigned c, const char* key, const BY_HANDLE_FILE_INFORMATION* identity,
                      unsigned id)
{
    s_claim[c].identity = *identity;
    strcpy(s_claim[c].key, key);
    s_claim[c].id = id;
    s_claim[c].on = 1;
}

int recfile_play(const char* path, unsigned id)
{
    BY_HANDLE_FILE_INFORMATION identity;
    unsigned i;
    char key[MAX_PATH];
    int ok = path_key(path, key) && file_identity(path, &identity);
    if (!ok) return 0;
    jobs_lock();
    ok = !delete_pending(key, &identity, id);
    if (ok) {
        for (i = 0; i < REC_FILE_CLAIMS; i++) s_claim[i].on = 0;
        claim_put(0, key, &identity, id);
    }
    jobs_unlock();
    return ok;
}

int recfile_claim(const char* path, unsigned id)
{
    BY_HANDLE_FILE_INFORMATION identity;
    unsigned i, c = REC_FILE_CLAIMS;
    char key[MAX_PATH];
    int ok = path_key(path, key) && file_identity(path, &identity);
    if (!ok) return 0;
    jobs_lock();
    for (i = 0; i < REC_FILE_CLAIMS && c == REC_FILE_CLAIMS; i++)
        if (!s_claim[i].on) c = i;
    ok = c < REC_FILE_CLAIMS && !delete_pending(key, &identity, id);
    if (ok) claim_put(c, key, &identity, id);
    jobs_unlock();
    return ok;
}

void recfile_release(unsigned id)
{
    unsigned i;
    jobs_lock();
    for (i = 0; i < REC_FILE_CLAIMS; i++)
        if (s_claim[i].on && s_claim[i].id == id) {
            s_claim[i].on = 0;
            break;
        }
    jobs_unlock();
}

void recfile_leave(void)
{
    unsigned i;
    jobs_lock();
    for (i = 0; i < REC_FILE_CLAIMS; i++) s_claim[i].on = 0;
    jobs_unlock();
}

/* ---- the prune (tagpu_recfile.h, THE PRUNE) ---------------------------------------------------- */

enum { PRUNE_GONE, PRUNE_KEPT, PRUNE_BUSY, PRUNE_ODD };

/* Has the recording a live timeline besides the original? 0 for one of another format, whose
   timelines this build cannot read and cannot play either; -1 when it is not one of ours. */
static int forked(const char* path)
{
    RecTimeline t[RECR_TIMELINES];
    char why[160];
    RecReader* r;
    int n, i, f = 0, k = recr_format_of(path);
    if (k <= 0) return k;
    r = recr_open(path, why, sizeof why);
    if (!r) return -1;
    n = recr_timelines(r, t, RECR_TIMELINES, why, sizeof why);
    recr_close(r);
    if (n < 0) return -1;
    for (i = 0; i < n && i < RECR_TIMELINES; i++)
        if (t[i].id && t[i].state != REC_TL_DELETED) f = 1;
    return f;
}

/* One recording older than the kept ones. WHY NOTHING IN USE OR MARKED IS DELETED (an ordering):
   1. its lock, taken exclusively as every writer takes it (recfile_lock): no append, fork,
      deletion or compaction runs on it from here on, so what step 2 reads stays true;
   2. its timelines read: one with a live fork is kept, and so is a file that is not one of our
      recordings; one in another format has none this build can read, so only step 4 keeps it;
   3. the recording itself opened exclusively, with DELETE access: it fails while anything holds
      it open (a playing, a reader, the writer), and while it is held nothing can open it, so no
      playing starts and no bookmark is added, a bookmark being added only by a playing of it;
   4. under the index's lock, a favourite or a bookmark keeps it; otherwise it is marked for
      deletion on that handle (tagpu_replays_unmarked_delete) and goes when the handle closes.
   Its lock goes with it, deleted through the exclusive handle of step 1, which no other caller
   can share, so no two callers ever hold lock objects of one path. */
static int prune_one(const char* path)
{
    char lock[MAX_PATH];
    const char* name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    HANDLE lk, f;
    int made, fk, r;
    FILE_DISPOSITION_INFO gone = { TRUE };
    SetInfoFn set = set_info();
    if (!set || snprintf(lock, sizeof lock, "%s.lock", path) >= (int)sizeof lock) return PRUNE_BUSY;
    lk = CreateFileA(lock, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, OPEN_ALWAYS,
                     FILE_ATTRIBUTE_HIDDEN, NULL);
    if (lk == INVALID_HANDLE_VALUE) return PRUNE_BUSY;
    made = GetLastError() != ERROR_ALREADY_EXISTS;
    fk = forked(path);
    r = fk < 0 ? PRUNE_ODD : fk ? PRUNE_KEPT : PRUNE_BUSY;
    if (!fk) {
        f = CreateFileA(path, GENERIC_READ | DELETE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            int d = tagpu_replays_unmarked_delete(name, f);
            r = d > 0 ? PRUNE_GONE : d == 0 ? PRUNE_KEPT : PRUNE_BUSY;
            CloseHandle(f);
        }
    }
    /* a lock this prune made for a recording it keeps goes too: nothing else holds it */
    if (r == PRUNE_GONE || made) set(lk, FileDispositionInfo, &gone, sizeof gone);
    CloseHandle(lk);
    return r;
}

/* `t` the recorded start and `dated` what recr_wallclock_of said: 1 dated, 0 read without a
   start, -1 not read */
typedef struct { char name[MAX_PATH]; FILETIME t; int dated; } PruneFile;
int recfile_newer_first(int dx, const FILETIME* tx, const char* x, int dy, const FILETIME* ty,
                        const char* y)
{
    int c = (dy > 0) - (dx > 0);
    if (!c && dx > 0) c = CompareFileTime(ty, tx);
    return c ? c : _stricmp(y, x);
}

static int newer_first(const void* a, const void* b)
{
    const PruneFile* x = (const PruneFile*)a;
    const PruneFile* y = (const PruneFile*)b;
    return recfile_newer_first(x->dated, &x->t, x->name, y->dated, &y->t, y->name);
}

static int exiting(void)
{
    int e;
    jobs_lock();
    e = s_exiting;
    jobs_unlock();
    return e;
}

static void prune(const char* fresh, unsigned keep)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("replays\\*.imprec", &fd);
    PruneFile* all = NULL;
    const char* own = strrchr(fresh, '\\') ? strrchr(fresh, '\\') + 1 : fresh;
    unsigned n = 0, cap = 0, i, c[4] = { 0 };
    /* the new recording is one of the `keep` whether or not its writer has made it yet */
    if (keep) keep--;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !_stricmp(fd.cFileName, own))
                continue;
            if (n == cap) {
                PruneFile* g = (PruneFile*)realloc(all, (cap = cap ? 2 * cap : 256) * sizeof *all);
                if (!g) break;
                all = g;
            }
            /* a name too long for its path and its lock's is left alone, counted unreadable */
            if (snprintf(all[n].name, MAX_PATH, "replays\\%s", fd.cFileName) >= MAX_PATH - 5) {
                c[PRUNE_ODD]++;
                continue;
            }
            n++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* the process's exit waits for this worker (recfile_exit): both passes stop at the next file
       once it has begun, and nothing is deleted after a header pass it cut short */
    if (n > keep) {
        /* one open and one read of the header block a file, on this worker */
        for (i = 0; i < n && !exiting(); i++)
            all[i].dated = recr_wallclock_of(all[i].name, &all[i].t);
        if (i == n) {
            qsort(all, n, sizeof *all, newer_first);
            /* a file the header pass could not read is kept, counted in use: it may read again
               by the time prune_one would reach it, dated as newly as any of the kept */
            for (i = keep; i < n && !exiting(); i++)
                c[all[i].dated < 0 ? PRUNE_BUSY : prune_one(all[i].name)]++;
        }
    }
    free(all);
    tagpu_logf("recorder: prune at %s: %u older recordings, the newest %u kept; of the rest, %u "
               "deleted, %u kept (forked or marked), %u in use, %u unreadable", fresh, n, n < keep ? n : keep,
               c[PRUNE_GONE], c[PRUNE_KEPT], c[PRUNE_BUSY], c[PRUNE_ODD]);
}

/* a ticket's result into the ring's next slot, and the count of finished requests; under the
   lock, which every write of s_completed holds */
static void result_put(unsigned ticket, int ok, const char* why)
{
    unsigned k = (unsigned)s_completed % REC_FILE_RESULTS;
    s_done[k].ticket = ticket;
    s_done[k].ok = ok;
    snprintf(s_done[k].why, sizeof s_done[k].why, "%s", why);
    InterlockedIncrement(&s_completed);
}

/* the next ticket, under the lock: never 0, so a refused request's 0 is no ticket */
static unsigned ticket_next(void)
{
    if (!++s_ticket) ++s_ticket;
    return s_ticket;
}

static DWORD WINAPI file_worker(void* arg)
{
    FileJob* j = (FileJob*)arg;
    char why[256] = "";
    int ok;
    if (j->op == REC_FILE_PRUNE) {
        prune(j->path, j->id);
        snprintf(why, sizeof why, "pruned");
        ok = 1;
    } else ok = recr_manage(j->path, j->op, j->id, j->output, j->nrows ? j->rows : NULL,
                            j->nrows, why, sizeof why);
    if (j->op != REC_FILE_PRUNE)
        tagpu_logf("replay: forks %s: %s: %s [request=%s]", j->path,
                   ok ? "done" : "refused", why, j->request[0] ? j->request : "-");
    jobs_lock();
    {
        unsigned i;
        /* THE TICKETS' order: the result before the job leaves s_pending */
        result_put(j->ticket, ok ? 1 : -1, why);
        for (i = 0; i < FILE_JOBS; i++) if (s_pending[i] == j) s_pending[i] = NULL;
    }
    jobs_unlock();
    free(j);
    return 0;
}

/* a job for the worker, its paths copied, or NULL for a path or a request id too long or no
   memory */
static FileJob* job_new(const char* path, unsigned op, unsigned id, const char* output,
                        const char* request)
{
    FileJob* j;
    if (strlen(path) >= MAX_PATH || (output && strlen(output) >= MAX_PATH) ||
        (request && strlen(request) > 32)) return NULL;
    j = (FileJob*)calloc(1, sizeof *j);
    if (!j) return NULL;
    strcpy(j->path, path);
    if (output) strcpy(j->output, output);
    j->op = op;
    j->id = id;
    if (request) strcpy(j->request, request);
    return j;
}

/* `j` handed to a worker, or refused: its ticket, or 0. The job is the worker's, or freed. */
static unsigned job_queue(FileJob* j)
{
    int i, slot = -1;
    unsigned ticket, c;
    HANDLE t;
    /* Publication and exit use the same lock: every started worker is tracked before exit
       takes its handles, and exit seals the set before waiting. Nothing holds this lock while
       waiting for a worker, which may log or acquire the recording's file. */
    jobs_lock();
    for (c = 0; j->op == REC_FILE_DELETE && c < REC_FILE_CLAIMS; c++)
        if (s_claim[c].on && s_claim[c].id == j->id &&
            same_reservation(s_claim[c].key, &s_claim[c].identity, j->key, &j->identity)) {
            char why[40];
            /* the refusal itself is the completed request, its outcome in the ring at once */
            ticket = ticket_next();
            snprintf(why, sizeof why, "timeline %u is in use", j->id);
            result_put(ticket, -1, why);
            jobs_unlock();
            tagpu_logf("replay: forks %s: refused: timeline %u is in use [request=%s]", j->path,
                       j->id, j->request[0] ? j->request : "-");
            free(j);
            return ticket;
        }
    for (i = 0; i < FILE_JOBS && !s_exiting; i++) {
        if (s_jobs[i] && WaitForSingleObject(s_jobs[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(s_jobs[i]);
            s_jobs[i] = NULL;
        }
        if (!s_jobs[i] && slot < 0) slot = i;
    }
    /* a ticket only for a job that has a slot; one a failed CreateThread took is never handed
       out, so no caller asks for it */
    ticket = 0;
    t = NULL;
    if (slot >= 0) {
        ticket = j->ticket = ticket_next();
        s_pending[slot] = j;
        t = CreateThread(NULL, 0, file_worker, j, 0, NULL);
        if (t) s_jobs[slot] = t;
        else s_pending[slot] = NULL;
    }
    jobs_unlock();
    if (!t) free(j);
    return t ? ticket : 0;
}

unsigned recfile_request(const char* path, unsigned op, unsigned id, const char* output,
                         const char* request)
{
    FileJob* j = job_new(path, op, id, output, request);
    if (!j) return 0;
    if (op == REC_FILE_DELETE &&
        (!path_key(path, j->key) || !file_identity(path, &j->identity))) {
        free(j);
        return 0;
    }
    return job_queue(j);
}

unsigned recfile_touch(const char* path, const RecTimelineState* rows, unsigned n)
{
    FileJob* j;
    if (!n || n > REC_TIMELINE_ROWS) return 0;
    j = job_new(path, REC_FILE_TOUCH, rows[0].id, NULL, NULL);
    if (!j) return 0;
    memcpy(j->rows, rows, n * sizeof *rows);
    j->nrows = n;
    return job_queue(j);
}

int recfile_result(unsigned ticket, char* why, unsigned cap)
{
    unsigned i;
    int r = -2;
    if (cap) why[0] = 0;
    if (!ticket) return r;
    jobs_lock();
    for (i = 0; i < FILE_JOBS; i++)
        if (s_pending[i] && s_pending[i]->ticket == ticket) r = 0;
    for (i = 0; r && i < REC_FILE_RESULTS; i++)
        if (s_done[i].ticket == ticket) {
            r = s_done[i].ok;
            if (cap) snprintf(why, cap, "%s", s_done[i].why);
        }
    jobs_unlock();
    return r;
}

/* A prune is not counted: its `path` names the new recording it keeps, and it never opens that
   file or its lock (prune skips it by name) -- it locks only older recordings. */
int recfile_pending(const char* path)
{
    unsigned i;
    int k = 0;
    jobs_lock();
    for (i = 0; i < FILE_JOBS; i++)
        if (s_pending[i] && s_pending[i]->op != REC_FILE_PRUNE &&
            !_stricmp(s_pending[i]->path, path)) k = 1;
    jobs_unlock();
    return k;
}

/* without the lock: the frame visit asks every frame, and a count read one completion late only
   delays its reader's next look by a frame -- the reader then reads the file, not the count */
unsigned recfile_completed(void)
{
    return (unsigned)InterlockedCompareExchange(&s_completed, 0, 0);
}

void recfile_exit(void)
{
    HANDLE h[FILE_JOBS];
    unsigned i, n = 0;
    jobs_lock();
    s_exiting = 1;
    for (i = 0; i < FILE_JOBS; i++) if (s_jobs[i]) h[n++] = s_jobs[i];
    jobs_unlock();
    if (n) WaitForMultipleObjects(n, h, TRUE, INFINITE);
}
