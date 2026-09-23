/* logtest.c -- the log sink (src/tagpu_log.c) driven hard, under Wine, with small caps.

   Built by `make -C tagpu/ddraw logtest` against the real tagpu_log.c with
   -DTLOG_FILE_CAP=65536 -DTLOG_TOTAL_CAP=524288, so rotation and eviction happen in
   seconds. tests/logtest.py runs the modes below in a scratch folder and checks the
   files they leave; the caps are also checked HERE, from inside the writing process,
   after every batch of lines -- the promise is that no file is ever over its cap, not
   only that none is at the end.

     logtest stress <lines>   two threads: numbered lines on both streams + blocks
     logtest block            a handle without FILE_SHARE_DELETE on tagpu.log blocks the
                              rotation for 2.5 s; lines are dropped, counted, then resume
     logtest hold <ms>        take the lock, write a line, sleep (the owner for `second`)
     logtest second <lines>   a second process: must write nothing and change nothing
     logtest forever          number lines until killed
     logtest orphan           a thread dies holding the sink's lock; after tagpu_log_detaching
                              a line must return at once (dropped), not wait on the dead owner
     logtest slow             number lines, one a millisecond, until killed (a rotation a
                              second or so: slow enough for a reader to follow)

   Each mode prints `ok ...` and exits 0, or prints what failed and exits 1. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_log.h"

void tagpu_log_selftest_locked(void (*f)(void));   /* src/tagpu_log.c, TLOG_SELFTEST */

HMODULE g_ddraw_module;            /* NULL: the sink finds log\ beside logtest.exe */

static char s_dir[MAX_PATH];
static volatile LONG s_fail;

static void fail(const char* what, const char* name, unsigned long long v)
{
    printf("FAIL %s %s %llu\n", what, name, v);
    fflush(stdout);
    InterlockedExchange(&s_fail, 1);
}

/* 0 when the name is not one the sink writes; else 2|stream, plus 4 when rotated */
static int ours(const char* n)
{
    static const char* const base[2] = { "tagpu", "tagpu_cobtrace" };
    int s;
    for (s = 0; s < 2; s++) {
        size_t bl = strlen(base[s]);
        const char* q = n + bl;
        if (_strnicmp(n, base[s], bl) || *q != '.') continue;
        if (!_stricmp(q, ".log")) return 2 | s;
        q++;
        if (*q < '0' || *q > '9') continue;
        while (*q >= '0' && *q <= '9') q++;
        if (!_stricmp(q, ".log")) return 6 | s;
    }
    return 0;
}

/* Every file of ours under the caps, the total under its cap, at most TLOG_KEEP rotated per
   stream, as the directory shows them -- listed under the sink's lock (check_caps), because
   a listing made while a rotation deletes and renames sums sizes from different instants. */
static void check_caps_locked(void)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    unsigned long long total = 0;
    unsigned rot[2] = { 0, 0 };
    _snprintf(pat, sizeof pat, "%s*.log", s_dir);
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        int s = ours(fd.cFileName);
        if (!s) continue;
        total += sz;
        if (sz > TLOG_FILE_CAP) fail("file over cap", fd.cFileName, sz);
        if (s & 4) rot[s & 1]++;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    if (total > TLOG_TOTAL_CAP) fail("total over cap", "", total);
    if (rot[0] > TLOG_KEEP) fail("rotated files", "tagpu", rot[0]);
    if (rot[1] > TLOG_KEEP) fail("rotated files", "tagpu_cobtrace", rot[1]);
}

static void check_caps(void)
{
    tagpu_log_selftest_locked(check_caps_locked);
}

typedef struct { int id; unsigned lines; } WORK;

static DWORD WINAPI writer(LPVOID p)
{
    WORK* w = (WORK*)p;
    unsigned i;
    for (i = 0; i < w->lines; i++) {
        tagpu_logf("t%d %u pad=%0*d", w->id, i, (int)(i % 200), 0);
        if (i % 7 == 0) {
            char c[96];
            _snprintf(c, sizeof c, "S\t%u\tt%d\tcobtrace line", i, w->id);
            c[sizeof c - 1] = 0;
            tagpu_log_stream(TLOG_COBTRACE, c);
        }
        if (i % 97 == 0) {         /* a block of 1..300 lines, never split */
            TLOG_BLOCK* b = tagpu_log_block_begin(TLOG_MAIN);
            unsigned k, n = 1 + (i * 7919u) % 300;
            tagpu_log_blockf(b, "B%d.%u begin %u", w->id, i, n);
            for (k = 0; k < n; k++) tagpu_log_blockf(b, "B%d.%u line %u", w->id, i, k);
            tagpu_log_blockf(b, "B%d.%u end", w->id, i);
            tagpu_log_block_end(b);
        }
        if (i % 50 == 0) check_caps();
    }
    return 0;
}

static int mode_stress(unsigned lines)
{
    WORK w[2] = { { 1, lines }, { 2, lines } };
    HANDLE t[2];
    int i;
    for (i = 0; i < 2; i++) t[i] = CreateThread(NULL, 0, writer, &w[i], 0, NULL);
    WaitForMultipleObjects(2, t, TRUE, INFINITE);
    check_caps();
    tagpu_log("stress: a line longer than LINE_MAX follows");
    {
        char* big = (char*)malloc(5000);
        memset(big, 'x', 4999);
        big[4999] = 0;
        tagpu_log(big);
        free(big);
    }
    tagpu_log("stress: done");
    if (s_fail) return 1;
    printf("ok stress %u lines x 2 threads\n", lines);
    return 0;
}

static int mode_block(void)
{
    char p[MAX_PATH];
    HANDLE h;
    unsigned i = 0;
    DWORD t0;
    /* enough to rotate twice, so history exists and the current file is open */
    while (i < 3000) { tagpu_logf("L %u pad=%0*d", i, 80, 0); i++; }
    _snprintf(p, sizeof p, "%stagpu.log", s_dir);
    h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("FAIL cannot hold %s\n", p); return 1; }
    t0 = GetTickCount();
    while (GetTickCount() - t0 < 2500) {
        tagpu_logf("L %u pad=%0*d", i, 80, 0);
        i++;
        if (i % 50 == 0) check_caps();
    }
    CloseHandle(h);
    /* slowly: the retry comes within RETRY_MS, and the gap line it writes must still be in
       history when the files are read */
    t0 = GetTickCount();
    while (GetTickCount() - t0 < 1500) {
        tagpu_logf("L %u pad=%0*d", i, 80, 0);
        i++;
        check_caps();
        Sleep(10);
    }
    tagpu_logf("block: last %u", i - 1);
    check_caps();
    if (s_fail) return 1;
    printf("ok block %u lines\n", i);
    return 0;
}

static void die_holding(void) { ExitThread(0); }

static DWORD WINAPI orphan_thread(LPVOID p)
{
    (void)p;
    tagpu_log_selftest_locked(die_holding);    /* never returns: the lock stays owned */
    return 0;
}

static int mode_orphan(void)
{
    HANDLE t;
    DWORD t0;
    tagpu_log("orphan: before");
    t = CreateThread(NULL, 0, orphan_thread, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    tagpu_log_detaching();
    t0 = GetTickCount();
    tagpu_log("orphan: after detach, dropped");
    {
        TLOG_BLOCK* b = tagpu_log_block_begin(TLOG_MAIN);
        tagpu_log_blockf(b, "orphan: block, dropped");
        tagpu_log_block_end(b);
    }
    if (GetTickCount() - t0 > 1000) { printf("FAIL waited %lu ms\n", GetTickCount() - t0); return 1; }
    printf("ok orphan\n");
    return 0;
}

int main(int argc, char** argv)
{
    char exe[MAX_PATH], *cut;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    cut = strrchr(exe, '\\');
    if (cut) cut[1] = 0;
    _snprintf(s_dir, sizeof s_dir, "%slog\\", exe);
    if (argc < 2) { printf("usage: logtest stress|block|hold|second|forever|slow|orphan ...\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!strcmp(argv[1], "stress")) { tagpu_log_init(); return mode_stress(argc > 2 ? (unsigned)atoi(argv[2]) : 20000); }
    if (!strcmp(argv[1], "block"))  { tagpu_log_init(); return mode_block(); }
    if (!strcmp(argv[1], "orphan")) { tagpu_log_init(); return mode_orphan(); }
    if (!strcmp(argv[1], "hold")) {
        tagpu_log_init();
        tagpu_log("hold: owner");
        printf("ok holding\n");
        Sleep(argc > 2 ? atoi(argv[2]) : 3000);
        return 0;
    }
    if (!strcmp(argv[1], "second")) {
        unsigned i, n = argc > 2 ? (unsigned)atoi(argv[2]) : 1000;
        tagpu_log_init();
        for (i = 0; i < n; i++) tagpu_logf("second %u", i);
        tagpu_log_stream(TLOG_COBTRACE, "second");
        printf("ok second wrote %u\n", n);
        return 0;
    }
    if (!strcmp(argv[1], "forever")) {
        unsigned i = 0;
        tagpu_log_init();
        printf("ok forever\n");
        for (;;) tagpu_logf("F %u pad=%0*d", i++, 40, 0);
    }
    if (!strcmp(argv[1], "slow")) {
        unsigned i = 0;
        tagpu_log_init();
        printf("ok slow\n");
        for (;;) { tagpu_logf("W %u pad=%0*d", i++, 40, 0); Sleep(1); }
    }
    return 2;
}
