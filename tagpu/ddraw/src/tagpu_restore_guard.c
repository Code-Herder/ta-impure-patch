/* tagpu_restore_guard.c -- the restorer's record, its crash filter and its
   notice (tagpu_restore_guard.h). */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "git.h"
#include "hook.h"
#include "tagpu_log.h"
#include "tagpu_settings.h"
#include "tagpu_restore_guard.h"

#define MARKER      "tagpu_restore_crashed.txt"
#define RECORD_FILE "tagpu_restore_off.txt"
#define FAULT_FILE  "tagpu_restorefault.on"
#define RELAUNCHED  "TAGPU_RESTORE_RELAUNCHED"
#define KEY_LEN     40

/* THE RECORD IS THE RENDER THREAD'S: `_device`, `_turn_off` and the tick
   that carries out a retry are its only writers, and so is the epoch's. The
   game thread's one write is the request, `s_clearReq`. */
static char          s_key[KEY_LEN];
static volatile LONG s_off;
static volatile LONG s_epoch;
static volatile LONG s_clearReq;

/* the marker the last run left, while the record it stands for is not yet on
   disk */
static int  s_markPending;
static char s_markKey[KEY_LEN];

/* ---- the notice (D12) ----------------------------------------------------- */

static const char* const NOTICE[] = {
    "",
    "Undithering crashed on this graphics driver, so it is now off for this driver and "
    "the game shows the original dithered art.\n\n"
    "It is tried again by itself after a driver or game update. To try it now, set "
    "Undithered assets to On in the render options.",
    "The graphics device stopped responding while undithering was running, so undithering "
    "is now off for this driver and the game shows the original dithered art.\n\n"
    "It is tried again by itself after a driver or game update. To try it now, set "
    "Undithered assets to On in the render options.",
    "Undithering failed its startup check on this graphics driver: its results were "
    "wrong. It is now off for this driver and the game shows the original dithered art.\n\n"
    "It is tried again by itself after a driver or game update. To try it now, set "
    "Undithered assets to On in the render options.",
};

/* ON A THREAD OF ITS OWN: a message box is modal and pumps its own messages,
   and the render thread that decided to show it must not stop drawing, nor
   the game thread stop running the simulation, while the player reads it. */
static DWORD WINAPI notice_thread(LPVOID arg)
{
    MessageBoxA(NULL, NOTICE[(int)(INT_PTR)arg], "Total Annihilation: Impure",
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

static void notice(int why)
{
    HANDLE h;
    if (why < TAGPU_RG_CRASH || why > TAGPU_RG_SELFTEST) return;
    h = CreateThread(NULL, 0, notice_thread, (LPVOID)(INT_PTR)why, 0, NULL);
    if (h) CloseHandle(h);
}

/* ---- the fault lever ------------------------------------------------------ */

static int  s_faultRead;
static char s_fault[64];

int tagpu_rguard_fault(const char* token)
{
    if (!s_faultRead) {
        FILE* f = fopen(FAULT_FILE, "rb");
        size_t n = 0;
        if (f) { n = fread(s_fault, 1, sizeof s_fault - 1, f); fclose(f); }
        s_fault[n] = 0;
        /* one log line whatever the file's line ends; the tokens are words */
        for (size_t i = 0; i < n; ++i)
            if (s_fault[i] == '\r' || s_fault[i] == '\n') s_fault[i] = ' ';
        s_faultRead = 1;
        if (f) tagpu_logf("restoreguard: %s holds \"%s\" - a restorer fault is armed", FAULT_FILE, s_fault);
    }
    return token && s_fault[0] && strstr(s_fault, token) != NULL;
}

/* ---- the record ----------------------------------------------------------- */

/* the key recorded off, "" for none: the store's, or the file's under
   tagpu_defaults.off, which keeps the store out of it */
static void record_read(char* out, int cap)
{
    out[0] = 0;
    if (tagpu_settings_ignored()) {
        FILE* f = fopen(RECORD_FILE, "rb");
        if (f) {
            size_t n = fread(out, 1, (size_t)cap - 1, f);
            fclose(f);
            out[n] = 0;
            out[strcspn(out, "\r\n")] = 0;
        }
    } else {
        tagpu_settings_restoreoff(out, cap);
    }
}

static void record_write(const char* key)
{
    if (tagpu_settings_ignored()) {
        if (key[0]) {
            FILE* f = fopen(RECORD_FILE, "wb");
            if (f) { fprintf(f, "%s\r\n", key); fclose(f); }
        } else {
            DeleteFileA(RECORD_FILE);
        }
    } else {
        tagpu_settings_set_restoreoff(key);
    }
}

/* 1 when `key` is the record ON DISK: the file read back, or the store as its
   last successful write left it -- the store writes later, on its own
   thread's schedule (tagpu_settings.h) */
static int record_saved(const char* key)
{
    char rec[KEY_LEN];
    if (!tagpu_settings_ignored()) return tagpu_settings_restoreoff_saved(key);
    record_read(rec, sizeof rec);
    return !strcmp(rec, key);
}

/* the marker goes once its record is on disk; until then it is the record,
   and a launch that dies first reads it again */
static void marker_settle(void)
{
    if (!s_markPending || !record_saved(s_markKey)) return;
    DeleteFileA(MARKER);
    s_markPending = 0;
    tagpu_logf("restoreguard: the record for %s is on disk - the crash marker is deleted", s_markKey);
}

/* ---- the crash path: everything it touches is built here, at install ------ */

static LPTOP_LEVEL_EXCEPTION_FILTER s_prev;
static int   s_installed, s_relaunched;
static char  s_markPath[MAX_PATH];
static char  s_markHead[KEY_LEN + 16];   /* "<key>\r\n", written first          */
static int   s_markHeadLen;
static char  s_exe[MAX_PATH], s_cwd[MAX_PATH];
static char* s_cmd;                      /* CreateProcessA may write into it    */
static char* s_env;                      /* ours plus RELAUNCHED=<this process> */

static char  s_waited[96];               /* what tagpu_rguard_attach found      */

static volatile LONG s_depth;
static volatile LONG s_tid;
static volatile LONG s_fired;
static volatile LONG s_work;             /* a bit per frame slot                */

static int hex8(char* o, unsigned v)
{
    static const char dig[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; i--) { o[i] = dig[v & 15]; v >>= 4; }
    return 8;
}

/* THE MARKER, THEN THE RELAUNCH, AND NOTHING OF OURS THAT ALLOCATES: the
   crash may have happened inside the heap or the log with its lock held, so
   what the calls need from us is in static buffers. CreateFileA and
   CreateProcessA are the system's and may take the heap's lock themselves;
   a crash holding it stops there, with the marker already written, and a
   second fault inside them goes to TA's filter (`s_fired`). Returns only when
   the relaunch did not happen. */
static void blame(int why, unsigned code, unsigned addr)
{
    static char text[KEY_LEN + 64];
    static const char* const word[] = { "", "crash", "lost", "selftest" };
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE h;
    DWORD wrote = 0;
    int n = 0, ok;
    const char* w = word[why];

    memcpy(text, s_markHead, (size_t)s_markHeadLen); n = s_markHeadLen;
    while (*w) text[n++] = *w++;
    if (why == TAGPU_RG_CRASH) {
        text[n++] = ' '; n += hex8(text + n, code);
        text[n++] = ' '; text[n++] = '@'; n += hex8(text + n, addr);
    }
    text[n++] = '\r'; text[n++] = '\n';
    h = CreateFileA(s_markPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    ok = WriteFile(h, text, (DWORD)n, &wrote, NULL) && wrote == (DWORD)n;
    CloseHandle(h);
    if (!ok || s_relaunched || !s_cmd) return;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(s_exe, s_cmd, NULL, NULL, FALSE, 0, s_env, s_cwd[0] ? s_cwd : NULL, &si, &pi))
        return;
    TerminateProcess(GetCurrentProcess(), 3);
}

/* UNHANDLED ONLY, never vectored: a vectored handler sees first-chance
   exceptions, which drivers raise and catch for themselves, and blaming one
   would turn the restorer off on a driver that is working. TA's own filter
   (the one that writes ErrorLog.txt) is installed by WinMain before any
   window exists; this one is installed at the restorer's bring-up, later, and
   hands everything that is not ours to it. */
static LONG WINAPI filter(EXCEPTION_POINTERS* e)
{
    if (s_depth > 0 && (DWORD)s_tid == GetCurrentThreadId() &&
        InterlockedExchange(&s_fired, 1) == 0) {
        unsigned code = e && e->ExceptionRecord ? (unsigned)e->ExceptionRecord->ExceptionCode : 0;
        unsigned addr = e && e->ExceptionRecord ? (unsigned)(size_t)e->ExceptionRecord->ExceptionAddress : 0;
        blame(TAGPU_RG_CRASH, code, addr);
    }
    return s_prev ? s_prev(e) : EXCEPTION_CONTINUE_SEARCH;
}

static void install(void)
{
    char* env;
    size_t n = 0, cl;
    const char* cmd = GetCommandLineA();
    char buf[8], mark[80];
    FILETIME c, e, k, u;
    int ml;

    if (s_installed) return;
    s_installed = 1;
    s_relaunched = GetEnvironmentVariableA(RELAUNCHED, buf, sizeof buf) > 0;
    /* THIS PROCESS, BY PID AND CREATION TIME, for the relaunch to wait on
       (tagpu_rguard_attach): a pid alone can be reused once we are gone */
    memset(&c, 0, sizeof c);
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ml = _snprintf(mark, sizeof mark, "%s=%lu:%08lx:%08lx", RELAUNCHED, (unsigned long)GetCurrentProcessId(),
                   (unsigned long)c.dwHighDateTime, (unsigned long)c.dwLowDateTime);
    if (ml < 0 || ml >= (int)sizeof mark) ml = 0;
    if (!GetFullPathNameA(MARKER, sizeof s_markPath, s_markPath, NULL)) lstrcpynA(s_markPath, MARKER, sizeof s_markPath);
    if (!GetModuleFileNameA(NULL, s_exe, sizeof s_exe)) s_exe[0] = 0;
    if (!GetCurrentDirectoryA(sizeof s_cwd, s_cwd)) s_cwd[0] = 0;
    cl = cmd ? strlen(cmd) : 0;
    s_cmd = s_exe[0] && cmd ? (char*)malloc(cl + 1) : NULL;
    if (s_cmd) memcpy(s_cmd, cmd, cl + 1);
    /* the environment block: ours, with the relaunch mark in front */
    env = GetEnvironmentStringsA();
    if (env) {
        while (env[n] || env[n + 1]) n++;
        n += 2;
        s_env = ml ? (char*)malloc((size_t)ml + 1 + n) : NULL;
        if (s_env) {
            memcpy(s_env, mark, (size_t)ml + 1);
            memcpy(s_env + ml + 1, env, n);
        }
        FreeEnvironmentStringsA(env);
    }
    s_prev = real_SetUnhandledExceptionFilter(filter);
    tagpu_logf("restoreguard: crash filter installed ahead of %p%s%s", (void*)s_prev,
               s_relaunched ? "; this process is a relaunch and will not relaunch again - " : "",
               s_relaunched ? s_waited : "");
}

/* THE RELAUNCH WAITS FOR THE PROCESS THAT CRASHED, before TotalA.exe's
   WinMain can run: WinMain refuses to start while the semaphore "Total
   Annihilation" exists (OpenSemaphoreA at 0x49E885, DISASSEMBLED), and the
   crashed process holds it until it is gone. A terminating process's handles
   are closed before its process object is signalled, so the wait orders this
   process after the semaphore is released. The crashed process is found by
   pid AND creation time: a pid can be reused once it has exited, and then
   there is nothing left to wait for. */
void tagpu_rguard_attach(void)
{
    char v[64];
    unsigned long pid = 0, hi = 0, lo = 0;
    FILETIME c, e, k, u;
    HANDLE h;
    if (!GetEnvironmentVariableA(RELAUNCHED, v, sizeof v)) return;
    if (sscanf(v, "%lu:%lx:%lx", &pid, &hi, &lo) != 3) {
        lstrcpynA(s_waited, "no process to wait for is named", sizeof s_waited);
        return;
    }
    h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        lstrcpynA(s_waited, "the crashed process was already gone", sizeof s_waited);
        return;
    }
    memset(&c, 0, sizeof c);
    if (GetProcessTimes(h, &c, &e, &k, &u) && c.dwHighDateTime == hi && c.dwLowDateTime == lo) {
        WaitForSingleObject(h, INFINITE);
        lstrcpynA(s_waited, "it waited for the crashed process to end", sizeof s_waited);
    } else {
        lstrcpynA(s_waited, "the crashed process was already gone (its pid is reused)", sizeof s_waited);
    }
    CloseHandle(h);
}

/* ---- the entries ---------------------------------------------------------- */

int tagpu_rguard_device(unsigned vendor, unsigned device, unsigned driver)
{
    char rec[KEY_LEN];
    FILE* f;

    _snprintf(s_key, sizeof s_key, "%04x:%04x:%08x:%.16s", vendor & 0xFFFF, device & 0xFFFF, driver,
              GIT_COMMIT);
    s_key[sizeof s_key - 1] = 0;
    install();
    s_markHeadLen = _snprintf(s_markHead, sizeof s_markHead, "%s\r\n", s_key);

    /* THE LAST RUN'S MARKER, turned into the record for the key it names --
       which is the device that crashed, whichever this one is */
    f = fopen(MARKER, "rb");
    if (f) {
        char body[160];
        size_t n = fread(body, 1, sizeof body - 1, f);
        char* reason;
        int why = TAGPU_RG_CRASH;
        fclose(f);
        body[n] = 0;
        reason = body + strcspn(body, "\r\n");
        while (*reason == '\r' || *reason == '\n') *reason++ = 0;
        reason[strcspn(reason, "\r\n")] = 0;
        if (!strncmp(reason, "lost", 4)) why = TAGPU_RG_LOST;
        if (!body[0]) {
            DeleteFileA(MARKER);                /* it names nothing */
        } else {
            record_read(rec, sizeof rec);
            if (strcmp(rec, body)) {
                tagpu_logf("restoreguard: the last run turned the restorer off (%s, device %s) - "
                           "recorded, and the player is told", reason[0] ? reason : "?", body);
                record_write(body);
                notice(why);
            } else {
                tagpu_logf("restoreguard: a crash marker for %s is still on disk, and so is its record", body);
            }
            s_markPending = 1;
            lstrcpynA(s_markKey, body, sizeof s_markKey);
            marker_settle();
        }
    }

    record_read(rec, sizeof rec);
    if (rec[0] && strcmp(rec, s_key)) {
        tagpu_logf("restoreguard: the restorer was off for %s; this is %s, a new driver, "
                   "another card or a new build - it is tried again", rec, s_key);
        record_write("");
        rec[0] = 0;
    }
    InterlockedExchange(&s_off, rec[0] ? 1 : 0);
    if (rec[0])
        tagpu_logf("restoreguard: the restorer is recorded off for %s - the original art is drawn "
                   "(the render options' Undithered assets row tries again)", s_key);
    return (int)s_off;
}

/* a retry the render thread has not carried out yet already reads as on */
int tagpu_rguard_off(void) { return s_off && !s_clearReq; }

const char* tagpu_rguard_key(void) { return s_key; }

void tagpu_rguard_turn_off(int why)
{
    if (!s_key[0]) return;
    tagpu_logf("restoreguard: the restorer is turned off for %s (%s) and the player is told", s_key,
               why == TAGPU_RG_SELFTEST ? "the self-test failed" : why == TAGPU_RG_LOST ? "device lost" : "crash");
    record_write(s_key);
    InterlockedExchange(&s_off, 1);
    notice(why);
}

/* THE GAME THREAD ONLY ASKS: TA is lockstep, and a write to the store's
   file -- or a wait on the store's lock while the render thread writes it --
   must not stall OnCommand (tagpu_menu.c). The render thread's tick carries it
   out. */
void tagpu_rguard_clear(void)
{
    InterlockedExchange(&s_clearReq, 1);
}

void tagpu_rguard_tick(void)
{
    if (InterlockedExchange(&s_clearReq, 0)) {
        record_write("");
        InterlockedExchange(&s_off, 0);
        /* AFTER the record: an epoch seen moved is a record already cleared */
        InterlockedIncrement(&s_epoch);
        tagpu_logf("restoreguard: the player turned undithering back on - the record is cleared "
                   "and the restorer tries again");
    }
    marker_settle();
}

unsigned tagpu_rguard_epoch(void) { return (unsigned)s_epoch; }

void tagpu_rguard_enter(void)
{
    if (s_depth++ == 0) InterlockedExchange(&s_tid, (LONG)GetCurrentThreadId());
}

void tagpu_rguard_leave(void)
{
    if (s_depth > 0) s_depth--;
}

void tagpu_rguard_work(unsigned slot)
{
    if (slot < 32) InterlockedOr(&s_work, (LONG)(1u << slot));
}

void tagpu_rguard_fenced(unsigned slot)
{
    if (slot < 32) InterlockedAnd(&s_work, ~(LONG)(1u << slot));
}

void tagpu_rguard_idle(void)
{
    InterlockedExchange(&s_work, 0);
}

int tagpu_rguard_blame_lost(void)
{
    if (!s_work || !s_installed) return 0;
    tagpu_logf("restoreguard: the device was lost with restorer work unfinished (slots 0x%lx) - "
               "the restorer is blamed, recorded off for %s, and the game relaunches",
               (unsigned long)s_work, s_key);
    blame(TAGPU_RG_LOST, 0, 0);
    tagpu_logf("restoreguard: %s", s_relaunched ? "this process is already a relaunch - no second one"
                                                : "the relaunch could not be made - the marker stands for the next launch");
    return 0;
}

int tagpu_rguard_fault_lost(void)
{
    static int shot;
    if (shot || !s_work || !tagpu_rguard_fault("lost")) return 0;
    shot = 1;
    return 1;
}
