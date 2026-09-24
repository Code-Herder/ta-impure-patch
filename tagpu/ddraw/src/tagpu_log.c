/* tagpu_log.c -- the log sink. The contract, the caps and the threading rules are in
   tagpu_log.h; this file is the arithmetic that makes the caps hold.

   THE INVARIANT. Every byte reaches a file through raw(), and every call of raw() is
   preceded by the two checks it needs: the file's size plus the write stays within
   TLOG_FILE_CAP, and the total of every counted file plus the write stays within
   TLOG_TOTAL_CAP (after deleting history to make room). A write that cannot pass both is
   not made. So the caps do not depend on a rotation succeeding, on another process
   releasing a file, or on any timing: they are a precondition of the only write path.

   The sizes are this process's own bookkeeping, taken from the directory at attach and
   updated by every write, rotation and delete. It is exact because `log\tagpu.lock` makes
   this process the only writer of these names. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dllmain.h"
#include "tagpu_log.h"

#ifndef TLOG_FILE_CAP
#define TLOG_FILE_CAP   (16u << 20)
#endif
#ifndef TLOG_TOTAL_CAP
#define TLOG_TOTAL_CAP  (128u << 20)
#endif
#ifndef TLOG_KEEP
#define TLOG_KEEP       10
#endif

#define NSTREAM     2
#define LINE_MAX    2048          /* one line's text, before its ending: above every line
                                     buffer that feeds the sink (the packet heartbeat's is
                                     1700 bytes, and its lines run past 1200)             */
#define NOTE_MAX    192           /* one of the sink's own lines, ending included          */
#define RETRY_MS    1000          /* a refused rotation or delete is retried this often    */
#define TRUNC       "...[truncated]"
#define BLOCK_MAX   (TLOG_FILE_CAP - 4 * NOTE_MAX)  /* a block fits a fresh file: header,
                                                       gap line, block, continuation   */

/* The total can always be brought under its cap by deleting history only if the current
   files alone fit inside it; and a fresh file must hold its notes and one longest line. */
typedef char tlog_caps_consistent[
    ((unsigned long long)TLOG_TOTAL_CAP >= (unsigned long long)NSTREAM * TLOG_FILE_CAP &&
     TLOG_FILE_CAP >= 8 * (2 * LINE_MAX + 4 * NOTE_MAX) && TLOG_KEEP >= 1 && TLOG_KEEP <= 99)
    ? 1 : -1];

static const char* const s_base[NSTREAM] = { "tagpu", "tagpu_cobtrace" };
static const char* const s_note[NSTREAM] = { "", "# " };      /* cobtrace's parsers skip '#' */
static const char* const s_eol[NSTREAM]  = { "\r\n", "\n" };  /* what each stream always had */

typedef struct {
    HANDLE             h;                    /* the current file; INVALID_HANDLE_VALUE when closed */
    unsigned long long cur;                  /* bytes in the current file, open or not             */
    int                stale;                /* the current file is closed to writing -- the
                                                previous run's, or one whose rotation was
                                                refused after its `continued in` line: it must
                                                rotate before another line goes anywhere      */
    long long          slot[TLOG_KEEP + 1];  /* bytes in <base>.<n>.log, -1 absent; [0] unused   */
    unsigned long long age[TLOG_KEEP + 1];   /* its last write (FILETIME): the eviction order    */
    unsigned long long extra;                /* <base>.<n>.log past TLOG_KEEP that would not
                                                delete at attach: counted, never written        */
    unsigned           part;                 /* this run's part number of the current file     */
    unsigned           dropped;              /* lines refused since the last one written       */
    DWORD              dropFrom;             /* GetTickCount of the first of them              */
    int                blocked;              /* the last rotation or delete was refused ...    */
    DWORD              retryAt;              /* ... and is not tried again before this         */
} TLOG_STREAM;

struct TLOG_BLOCK { int s; char* p; unsigned n, cap; int cut; };

static CRITICAL_SECTION s_cs;
static volatile LONG    s_live;           /* set once, last thing in init; never cleared     */
static volatile LONG    s_detach;         /* process detach: see tagpu_log_detaching         */
static char             s_dir[MAX_PATH];  /* "<folder of our ddraw.dll>\log\"                */
static char             s_run[48];        /* "YYYYMMDD-HHMMSS-<pid>"                         */
static SYSTEMTIME       s_start;
static TLOG_STREAM      s_st[NSTREAM];

/* ---- files ------------------------------------------------------------------------ */

static void path_of(char* out, int s, unsigned n)
{
    if (n) _snprintf(out, MAX_PATH, "%s%s.%u.log", s_dir, s_base[s], n);
    else   _snprintf(out, MAX_PATH, "%s%s.log", s_dir, s_base[s]);
    out[MAX_PATH - 1] = 0;
}

static unsigned long long now_age(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* 1 and the file's size and last write when it exists */
static int file_info(const char* p, unsigned long long* size, unsigned long long* age)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a)) return 0;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    *size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    *age  = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return 1;
}

static int gone(const char* p)
{
    return DeleteFileA(p) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

static unsigned long long total(void)
{
    unsigned long long t = 0;
    int s;
    unsigned n;
    for (s = 0; s < NSTREAM; s++) {
        t += s_st[s].cur + s_st[s].extra;
        for (n = 1; n <= TLOG_KEEP; n++)
            if (s_st[s].slot[n] >= 0) t += (unsigned long long)s_st[s].slot[n];
    }
    return t;
}

/* Delete the oldest rotated file of either stream that CAN be deleted; one held open by
   another process is passed over for the next oldest. 0 when none can go. */
static int evict_oldest(void)
{
    unsigned char tried[NSTREAM][TLOG_KEEP + 1];
    char p[MAX_PATH];
    memset(tried, 0, sizeof tried);
    for (;;) {
        int s, bs = -1;
        unsigned n, bn = 0;
        for (s = 0; s < NSTREAM; s++)
            for (n = 1; n <= TLOG_KEEP; n++)
                if (s_st[s].slot[n] >= 0 && !tried[s][n] &&
                    (bs < 0 || s_st[s].age[n] < s_st[bs].age[bn])) { bs = s; bn = n; }
        if (bs < 0) return 0;
        path_of(p, bs, bn);
        if (gone(p)) { s_st[bs].slot[bn] = -1; return 1; }
        tried[bs][bn] = 1;
    }
}

/* Make room for `n` more bytes under the total cap. 0 when history cannot be deleted. */
static int room_total(unsigned long long n)
{
    while (total() + n > TLOG_TOTAL_CAP)
        if (!evict_oldest()) return 0;
    return 1;
}

/* THE ONE WRITE. Callers have checked both caps for exactly these bytes. */
static int raw(int s, const char* b, unsigned n)
{
    DWORD w = 0;
    BOOL ok = WriteFile(s_st[s].h, b, n, &w, NULL);
    s_st[s].cur += w;
    return ok && w == n;
}

/* One of the sink's own lines. Written only when it passes both caps; 0 when it did not.
   put() reserves room for the header and the gap line with the line they precede, so those
   two are written whenever the line is. */
static int note(int s, const char* fmt, ...)
{
    TLOG_STREAM* st = &s_st[s];
    char b[NOTE_MAX];
    int n, k;
    va_list ap;
    if (st->h == INVALID_HANDLE_VALUE) return 0;
    n = _snprintf(b, sizeof b, "%s", s_note[s]);
    va_start(ap, fmt);
    k = _vsnprintf(b + n, sizeof b - n - 3, fmt, ap);
    va_end(ap);
    n = (k < 0 || k >= (int)(sizeof b - n - 3)) ? (int)sizeof b - 3 : n + k;
    n += _snprintf(b + n, 3, "%s", s_eol[s]);
    if (st->cur + n > TLOG_FILE_CAP || !room_total(n)) return 0;
    return raw(s, b, n);
}

/* Shift the history up one and the current file into <base>.1.log. The shift starts at
   the lowest FREE slot, so after a refused rename the numbering is still in order with one
   gap, and the next attempt resumes there instead of deleting a second file; with no free
   slot the oldest (<base>.<TLOG_KEEP>.log) is deleted first. The current file is closed. */
static int shift(int s)
{
    TLOG_STREAM* st = &s_st[s];
    char a[MAX_PATH], b[MAX_PATH];
    unsigned m, n;
    for (m = 1; m <= TLOG_KEEP && st->slot[m] >= 0; m++) {}
    if (m > TLOG_KEEP) {
        m = TLOG_KEEP;
        path_of(a, s, m);
        if (!gone(a)) return 0;
        st->slot[m] = -1;
    }
    /* A source that is already gone (deleted from outside: our handle shares delete) moved
       nothing and is not a refusal. REPLACE_EXISTING: the destination is free by our
       bookkeeping, so anything there is a stray under our name that was never counted. */
    for (n = m; n > 1; n--) {
        path_of(a, s, n - 1);
        path_of(b, s, n);
        if (MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING)) {
            st->slot[n] = st->slot[n - 1];
            st->age[n]  = st->age[n - 1];
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND)
            return 0;
        st->slot[n - 1] = -1;
    }
    path_of(a, s, 0);
    path_of(b, s, 1);
    if (MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING)) {
        st->slot[1] = (long long)st->cur;
        st->age[1]  = now_age();
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND)
        return 0;
    st->cur   = 0;
    st->stale = 0;
    return 1;
}

static int rotate(int s)
{
    TLOG_STREAM* st = &s_st[s];
    if (st->h != INVALID_HANDLE_VALUE) {
        note(s, "log: continued in %s.log (part %u)", s_base[s], st->part + 1);
        CloseHandle(st->h);
        st->h = INVALID_HANDLE_VALUE;
        st->stale = 1;               /* until the shift succeeds, nothing more goes in it */
    }
    return shift(s);
}

/* The current file, created on the first line after a rotation so that an empty file
   never takes a history slot. FILE_SHARE_DELETE: our handle never blocks a rename. A new
   file's first line is its header, or the file is removed again: tools/talog.py knows a
   file by that line, and a file without it would drop out of every reader's view. */
static int open_current(int s)
{
    TLOG_STREAM* st = &s_st[s];
    char p[MAX_PATH];
    LARGE_INTEGER sz;
    path_of(p, s, 0);
    st->h = CreateFileA(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (st->h == INVALID_HANDLE_VALUE) return 0;
    st->cur = GetFileSizeEx(st->h, &sz) ? (unsigned long long)sz.QuadPart : 0;
    if (st->cur == 0) {
        char from[64];
        from[0] = 0;
        st->part++;
        if (st->part > 1) _snprintf(from, sizeof from, ", continues from %s.1.log", s_base[s]);
        from[sizeof from - 1] = 0;
        if (!note(s, "log: run %s part %u of %s.log, started %04u-%02u-%02u %02u:%02u:%02u%s",
                  s_run, st->part, s_base[s], s_start.wYear, s_start.wMonth, s_start.wDay,
                  s_start.wHour, s_start.wMinute, s_start.wSecond, from)) {
            CloseHandle(st->h);
            st->h = INVALID_HANDLE_VALUE;
            st->part--;
            /* a torn header that will not delete rotates away before the next line */
            if (gone(p)) st->cur = 0;
            else st->stale = 1;
            return 0;
        }
    }
    return 1;
}

/* One line or one block, ending included; s_cs held. */
static void put(int s, const char* b, unsigned n)
{
    TLOG_STREAM* st = &s_st[s];
    DWORD now = GetTickCount();

    if (st->blocked && (LONG)(now - st->retryAt) < 0) goto drop;
    st->blocked = 0;
    /* NOTE_MAX stays free in every file for the `continued in` line its rotation writes */
    if ((st->stale || st->cur + n + NOTE_MAX > TLOG_FILE_CAP) && !rotate(s)) goto refused;
    /* the header and the gap line go with the line or not at all: room for all three first */
    if (!room_total(n + (st->h == INVALID_HANDLE_VALUE ? NOTE_MAX : 0) + (st->dropped ? NOTE_MAX : 0)))
        goto refused;
    if (st->h == INVALID_HANDLE_VALUE && !open_current(s)) goto refused;
    if (st->dropped) {
        if (!note(s, "log: %u lines dropped over %lu ms -- a rename, delete or create in log\\ was refused",
                  st->dropped, (unsigned long)(now - st->dropFrom)))
            goto refused;
        st->dropped = 0;
    }
    if (st->cur + n > TLOG_FILE_CAP || !room_total(n)) goto refused;
    if (raw(s, b, n)) return;
    goto drop;
refused:
    st->blocked = 1;
    st->retryAt = now + RETRY_MS;
drop:
    if (!st->dropped++) st->dropFrom = now;
}

/* ---- attach ------------------------------------------------------------------------ */

/* Read the history this stream left in log\: its slots, anything numbered past
   TLOG_KEEP (deleted -- the numbering is ours), and the previous run's current file,
   which rotates into <base>.1.log before this run writes a line. */
static void survey(int s)
{
    TLOG_STREAM* st = &s_st[s];
    char p[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    unsigned n;
    size_t bl = strlen(s_base[s]);

    for (n = 1; n <= TLOG_KEEP; n++) {
        unsigned long long size, age;
        path_of(p, s, n);
        if (file_info(p, &size, &age)) { st->slot[n] = (long long)size; st->age[n] = age; }
    }
    _snprintf(pat, sizeof pat, "%s%s.*.log", s_dir, s_base[s]);
    pat[sizeof pat - 1] = 0;
    f = FindFirstFileA(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const char* q = fd.cFileName;
            char* end;
            unsigned long k;
            if (_strnicmp(q, s_base[s], bl) || q[bl] != '.' || q[bl + 1] < '0' || q[bl + 1] > '9')
                continue;
            k = strtoul(q + bl + 1, &end, 10);
            if (_stricmp(end, ".log") || k <= TLOG_KEEP) continue;
            _snprintf(p, sizeof p, "%s%s", s_dir, q);
            p[sizeof p - 1] = 0;
            if (!gone(p))
                st->extra += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }

    path_of(p, s, 0);
    {
        unsigned long long size, age;
        if (!file_info(p, &size, &age)) return;
        if (size == 0) { if (gone(p)) return; }
        st->cur = size;
        st->stale = 1;
        if (!rotate(s)) { st->blocked = 1; st->retryAt = GetTickCount() + RETRY_MS; }
    }
}

void tagpu_log_init(void)
{
    char mod[MAX_PATH], p[MAX_PATH];
    char* cut;
    DWORD len;
    int s;
    unsigned n;

    InitializeCriticalSection(&s_cs);
    for (s = 0; s < NSTREAM; s++) {
        s_st[s].h = INVALID_HANDLE_VALUE;
        for (n = 0; n <= TLOG_KEEP; n++) s_st[s].slot[n] = -1;
    }

    len = GetModuleFileNameA(g_ddraw_module, mod, sizeof mod);
    if (!len || len >= sizeof mod) return;
    cut = strrchr(mod, '\\');
    if (!cut) cut = strrchr(mod, '/');
    if (!cut) return;
    cut[1] = 0;
    if (strlen(mod) + 48 >= sizeof s_dir) return;    /* leaves room for the longest name */
    _snprintf(s_dir, sizeof s_dir, "%slog\\", mod);
    s_dir[sizeof s_dir - 1] = 0;
    if (!CreateDirectoryA(s_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;

    /* THE OWNER. Never closed: the lock is this process's until it ends, however it ends,
       and the OS releases it then -- a crash cannot leave a stale one. */
    _snprintf(p, sizeof p, "%stagpu.lock", s_dir);
    p[sizeof p - 1] = 0;
    if (CreateFileA(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL) == INVALID_HANDLE_VALUE)
        return;

    GetLocalTime(&s_start);
    _snprintf(s_run, sizeof s_run, "%04u%02u%02u-%02u%02u%02u-%lu", s_start.wYear,
              s_start.wMonth, s_start.wDay, s_start.wHour, s_start.wMinute, s_start.wSecond,
              (unsigned long)GetCurrentProcessId());
    s_run[sizeof s_run - 1] = 0;

    for (s = 0; s < NSTREAM; s++) survey(s);
    room_total(0);
    InterlockedExchange(&s_live, 1);
}

/* ---- lines ------------------------------------------------------------------------- */

void tagpu_log_detaching(void) { InterlockedExchange(&s_detach, 1); }

const char* tagpu_log_dir(void) { return s_live ? s_dir : ""; }

/* At detach ExitProcess has already killed every other thread, and one killed inside put()
   left s_cs owned by a dead thread: waiting on it would hang the exit (Windows terminates
   the process there, Wine grants it over half-updated state). So from detach on, a line
   that cannot take the lock at once is dropped. 1 when the lock is held. */
static int lock(void)
{
    if (s_detach) return TryEnterCriticalSection(&s_cs);
    EnterCriticalSection(&s_cs);
    return 1;
}

/* `in` into `out` as one line of stream s: at most LINE_MAX bytes of text, marked when cut,
   then the stream's ending. On the CRLF stream a bare '\n' inside the text becomes CRLF, as
   the text-mode fopen every writer used before this sink made it. */
static unsigned compose(int s, char* out, const char* in)
{
    unsigned n = 0;
    const char* eol = s_eol[s];
    while (*in && n < LINE_MAX) {
        if (*in == '\n' && eol[0] == '\r') {
            if (n + 2 > LINE_MAX) break;
            out[n++] = '\r';
        }
        out[n++] = *in++;
    }
    if (*in) { memcpy(out + n, TRUNC, sizeof TRUNC - 1); n += sizeof TRUNC - 1; }
    while (*eol) out[n++] = *eol++;
    return n;
}

void tagpu_log_stream(int s, const char* line)
{
    char b[LINE_MAX + sizeof TRUNC + 4];
    unsigned n;
    if (!s_live || s < 0 || s >= NSTREAM || !line) return;
    n = compose(s, b, line);
    if (!lock()) return;
    put(s, b, n);
    LeaveCriticalSection(&s_cs);
}

void tagpu_log(const char* line)
{
    tagpu_log_stream(TLOG_MAIN, line);
}

/* formatted one past LINE_MAX, so that compose() sees a cut line as too long and marks it
   (MSVCRT's _vsnprintf returns -1 without a NUL when the text does not fit) */
static void vformat(char* b, unsigned cap, const char* fmt, va_list ap)
{
    int k = _vsnprintf(b, cap - 1, fmt, ap);
    if (k < 0 || k >= (int)cap - 1) k = (int)cap - 1;
    b[k] = 0;
}

void tagpu_logf(const char* fmt, ...)
{
    char b[LINE_MAX + 2];
    va_list ap;
    if (!s_live) return;
    va_start(ap, fmt);
    vformat(b, sizeof b, fmt, ap);
    va_end(ap);
    tagpu_log_stream(TLOG_MAIN, b);
}

/* ---- blocks ------------------------------------------------------------------------ */

TLOG_BLOCK* tagpu_log_block_begin(int s)
{
    TLOG_BLOCK* b;
    if (!s_live || s < 0 || s >= NSTREAM) return NULL;
    b = (TLOG_BLOCK*)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->s = s;
    b->cap = 16384;
    b->p = (char*)malloc(b->cap);
    if (!b->p) { free(b); return NULL; }
    return b;
}

void tagpu_log_blockf(TLOG_BLOCK* b, const char* fmt, ...)
{
    char f[LINE_MAX + 2], line[LINE_MAX + sizeof TRUNC + 4];
    unsigned n;
    va_list ap;
    if (!b || b->cut) return;
    va_start(ap, fmt);
    vformat(f, sizeof f, fmt, ap);
    va_end(ap);
    n = compose(b->s, line, f);
    /* room for the cut marker's own line is kept below BLOCK_MAX */
    if (b->n + n + sizeof line > BLOCK_MAX) { b->cut = 1; return; }
    if (b->n + n > b->cap) {
        unsigned cap = b->cap * 2;
        char* q;
        while (cap < b->n + n) cap *= 2;
        q = (char*)realloc(b->p, cap);
        if (!q) { b->cut = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, line, n);
    b->n += n;
}

void tagpu_log_block_end(TLOG_BLOCK* b)
{
    if (!b) return;
    if (b->cut) {
        char line[LINE_MAX + sizeof TRUNC + 4];
        unsigned n = compose(b->s, line, "...[block truncated]");
        char* q = (char*)realloc(b->p, b->n + n);
        if (q) { b->p = q; memcpy(b->p + b->n, line, n); b->n += n; }
    }
    if (b->n && lock()) {
        put(b->s, b->p, b->n);
        LeaveCriticalSection(&s_cs);
    }
    free(b->p);
    free(b);
}

#ifdef TLOG_SELFTEST
/* tests/logtest.c only: run `f` under the sink's lock, so a directory listing it makes is one
   instant rather than a sum over a rotation in progress. Not in the DLL. */
void tagpu_log_selftest_locked(void (*f)(void))
{
    EnterCriticalSection(&s_cs);
    f();
    LeaveCriticalSection(&s_cs);
}
#endif
