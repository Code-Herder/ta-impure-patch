/* Recording metadata and file maintenance. A persistent path lock orders cooperating writers
   from the scan through closing the source and atomically replacing it; see recr_manage. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_recfile.h"
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

HANDLE recfile_lock(const char* path, char* why, unsigned cap)
{
    char name[MAX_PATH];
    HANDLE f;
    if (strlen(path) + 5 >= sizeof name) {
        snprintf(why, cap, "the recording lock path is too long");
        return NULL;
    }
    snprintf(name, sizeof name, "%s.lock", path);
    f = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_HIDDEN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(why, cap, "the recording is busy or its lock cannot be opened (%lu)",
                 GetLastError());
        return NULL;
    }
    return f;
}

void recfile_unlock(HANDLE* lock)
{
    if (*lock) CloseHandle(*lock);
    *lock = NULL;
}

#define FILE_JOBS 8
static HANDLE s_jobs[FILE_JOBS];
static volatile LONG s_lock;
static int s_exiting;
typedef struct FileJob {
    char path[MAX_PATH], output[MAX_PATH], key[MAX_PATH], request[33];
    unsigned op, id;
    BY_HANDLE_FILE_INFORMATION identity;
} FileJob;
static FileJob* s_pending[FILE_JOBS];
static BY_HANDLE_FILE_INFORMATION s_play_identity;
static unsigned s_play_id;
static char s_play_key[MAX_PATH];
static int s_playing;

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

int recfile_play(const char* path, unsigned id)
{
    BY_HANDLE_FILE_INFORMATION identity;
    unsigned i;
    char key[MAX_PATH];
    int ok = path_key(path, key) && file_identity(path, &identity);
    if (!ok) return 0;
    jobs_lock();
    for (i = 0; i < FILE_JOBS; i++) {
        FileJob* j = s_pending[i];
        if (j && j->op == REC_FILE_DELETE && j->id == id &&
            same_reservation(key, &identity, j->key, &j->identity)) ok = 0;
    }
    if (ok) {
        s_play_identity = identity;
        strcpy(s_play_key, key);
        s_play_id = id;
        s_playing = 1;
    }
    jobs_unlock();
    return ok;
}

void recfile_leave(void)
{
    jobs_lock();
    s_playing = 0;
    jobs_unlock();
}

static DWORD WINAPI file_worker(void* arg)
{
    FileJob* j = (FileJob*)arg;
    char why[256];
    int ok = recr_manage(j->path, j->op, j->id, j->output, why, sizeof why);
    tagpu_logf("replay: forks %s: %s: %s [request=%s]", j->path,
               ok ? "done" : "refused", why, j->request[0] ? j->request : "-");
    jobs_lock();
    {
        unsigned i;
        for (i = 0; i < FILE_JOBS; i++) if (s_pending[i] == j) s_pending[i] = NULL;
    }
    jobs_unlock();
    free(j);
    return 0;
}

int recfile_request(const char* path, unsigned op, unsigned id, const char* output,
                    const char* request)
{
    FileJob* j;
    int i, slot = -1;
    HANDLE t;
    if (strlen(path) >= MAX_PATH || (output && strlen(output) >= MAX_PATH) ||
        (request && strlen(request) > 32)) return 0;
    j = (FileJob*)calloc(1, sizeof *j);
    if (!j) return 0;
    strcpy(j->path, path);
    if (output) strcpy(j->output, output);
    j->op = op;
    j->id = id;
    if (request) strcpy(j->request, request);
    if (op == REC_FILE_DELETE &&
        (!path_key(path, j->key) || !file_identity(path, &j->identity))) {
        free(j);
        return 0;
    }
    /* Publication and exit use the same lock: every started worker is tracked before exit
       takes its handles, and exit seals the set before waiting. Nothing holds this lock while
       waiting for a worker, which may log or acquire the recording's file. */
    jobs_lock();
    if (op == REC_FILE_DELETE && s_playing && s_play_id == id &&
        same_reservation(s_play_key, &s_play_identity, j->key, &j->identity)) {
        jobs_unlock();
        tagpu_logf("replay: forks %s: refused: timeline %u is in use [request=%s]", path, id,
                   j->request[0] ? j->request : "-");
        free(j);
        return 1;                   /* the refusal itself is the completed request */
    }
    for (i = 0; i < FILE_JOBS && !s_exiting; i++) {
        if (s_jobs[i] && WaitForSingleObject(s_jobs[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(s_jobs[i]);
            s_jobs[i] = NULL;
        }
        if (!s_jobs[i] && slot < 0) slot = i;
    }
    if (slot >= 0) s_pending[slot] = j;
    t = slot >= 0 ? CreateThread(NULL, 0, file_worker, j, 0, NULL) : NULL;
    if (t) s_jobs[slot] = t;
    else if (slot >= 0) s_pending[slot] = NULL;
    jobs_unlock();
    if (!t) free(j);
    return t != NULL;
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
