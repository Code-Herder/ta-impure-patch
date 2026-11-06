/* tagpu_modinst.c -- Contract: tagpu_modinst.h. */
#include <windows.h>
#include <winhttp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_modinst.h"
#include "tagpu_modcat.h"
#include "tagpu_modpak.h"
#include "tagpu_modtar.h"
#include "tagpu_hpi.h"
#include "tagpu_ufo.h"
#include "tagpu_log.h"
#include "zstd.h"
#ifndef MODINST_TEST
#include "tagpu_pak.h"
#include "tagpu_modload.h"
#endif

/* The test build's way of dying at a chosen step (tests/modinsttest.c). */
#ifdef MODINST_TEST
void modinst_test_step(const char* step, unsigned n);
#define STEP(step, n) modinst_test_step(step, n)
#else
#define STEP(step, n) ((void)0)
#endif

#define MARKER         "installed.txt"
#define LOCK           "install.lock"
#define IO_CHUNK       (256u * 1024u)
#define WINDOW_LOG_MAX 27               /* 128 MiB: modpack's packages use 4 to 8; a larger frame is refused before it allocates */
#define RECIPE_MAX     (16u << 20)      /* the recipe's text */
#define ORIGINALS_MAX  (128u << 20)     /* the recipe's files together, held in memory for the archive writer */
#define MAX_ARCHIVES   64

static volatile LONG s_busy;
static volatile LONG s_phase = MODINST_IDLE;
static volatile LONG s_done, s_total;
static char s_why[200];

typedef struct {
    MODINSTALL m;
    char       root[MAX_PATH];  /* game_root's, on the thread that started it */
    char       base[512];
    HANDLE     lock;
    int        keep_new;        /* a finished folder this install could not retire: not this install's to delete */
} JOB;

static void set_phase(MODINST_PHASE p, unsigned total)
{
    InterlockedExchange(&s_done, 0);
    InterlockedExchange(&s_total, (LONG)total);
    InterlockedExchange(&s_phase, p);
}

static int path_of(char* out, size_t cap, const char* fmt, ...);

/* The folder of the exe, never the current directory: the engine's own code calls SetCurrentDirectoryA, and a process started
   from elsewhere must not act on a mods\\ that happens to lie there. The test build runs from outside the folder it tests. */
static int game_root(char* out, size_t cap)
{
#ifdef MODINST_TEST
    DWORD n = GetFullPathNameA(".", (DWORD)cap, out, NULL);
    return n && n < cap;
#else
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    char* slash;
    if (!n || n >= cap || !(slash = strrchr(out, '\\'))) return 0;
    *slash = 0;
    return 1;
#endif
}

/* the path built, or 0 when it would not fit: nothing here works on a truncated path */
static int path_of(char* out, size_t cap, const char* fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n > 0 && (size_t)n < cap;
}

static int is_dir(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int is_file(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* A folder and what it holds. Never through a link: its target is not ours to delete. */
static int remove_tree(const char* path)
{
    char spec[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    DWORD a = GetFileAttributesA(path);
    int ok = 1;

    if (a == INVALID_FILE_ATTRIBUTES) return 1;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileA(path) != 0;
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryA(path) != 0;
    if (!path_of(spec, sizeof spec, "%s\\*", path)) return 0;
    f = FindFirstFileA(spec, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            if (!path_of(child, sizeof child, "%s\\%s", path, fd.cFileName) || !remove_tree(child)) ok = 0;
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    return RemoveDirectoryA(path) != 0 && ok;
}

/* The marker first, or nothing at all: a folder with a marker is a whole version to the sweep, so one must never lose a file
   while it still has one. 0 when the marker is still there, with every other file kept: then the folder still is what it said. */
static int retire_marker(const char* dir)
{
    char marker[MAX_PATH];
    if (!path_of(marker, sizeof marker, "%s\\" MARKER, dir)) return 0;
    SetFileAttributesA(marker, FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(marker);
    return GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES;
}

/* Held for as long as this process may write in mods\. The system drops it with the process, however that ends, and it is why
   a start can tell another copy's unfinished install from a dead one's. */
static HANDLE take_lock(const char* mods)
{
    char path[MAX_PATH];
    if (!path_of(path, sizeof path, "%s\\" LOCK, mods)) return INVALID_HANDLE_VALUE;
    return CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
}

/* ---- the sweep at start-up ---------------------------------------------------------------- */

/* One mod's folders. Each state a crash can leave has one answer here, and every rename comes
   before the deletion that depends on it. */
static void settle_one(const char* mods, const char* id)
{
    char live[MAX_PATH], fresh[MAX_PATH], old[MAX_PATH], marker[MAX_PATH], part[MAX_PATH];
    int has_fresh, has_live, has_old;

    if (!path_of(live, sizeof live, "%s\\%s", mods, id) || !path_of(fresh, sizeof fresh, "%s\\%s.new", mods, id) ||
        !path_of(old, sizeof old, "%s\\%s.old", mods, id) || !path_of(marker, sizeof marker, "%s\\" MARKER, fresh) ||
        !path_of(part, sizeof part, "%s\\%s.part", mods, id)) return;

    DeleteFileA(part);
    has_fresh = is_dir(fresh);
    has_live = is_dir(live);
    has_old = is_dir(old);
    if (has_fresh && !is_file(marker)) {
        tagpu_logf("modinst: deleted the unfinished mods\\%s.new", id);
        remove_tree(fresh);
        has_fresh = 0;
    }
    if (has_old && has_live) {
        remove_tree(old);
        has_old = is_dir(old);
    }
    if (has_fresh) {
        if (has_old && has_live) {
            tagpu_logf("modinst: mods\\%s.old could not be deleted, so the update waits for another start", id);
            return;
        }
        if (has_live && !MoveFileA(live, old)) {
            tagpu_logf("modinst: mods\\%s is in use (error %lu), so the update waits for another start", id, GetLastError());
            return;
        }
        if (!MoveFileA(fresh, live)) {
            tagpu_logf("modinst: mods\\%s.new could not take its place (error %lu)", id, GetLastError());
            if (is_dir(old) && !MoveFileA(old, live)) tagpu_logf("modinst: mods\\%s.old could not go back (error %lu); the next start tries again", id, GetLastError());
            return;
        }
        remove_tree(old);
        tagpu_logf("modinst: mods\\%s now holds the installed update", id);
    } else if (has_old) {
        if (MoveFileA(old, live)) tagpu_logf("modinst: mods\\%s is back from an update that did not finish", id);
        else tagpu_logf("modinst: mods\\%s.old could not go back (error %lu); the next start tries again", id, GetLastError());
    }
}

enum { MAX_SETTLE = 64 };
typedef struct {
    const char* mods;
    char        ids[MAX_SETTLE][64];
    int         n;
} TO_SETTLE;

/* Only what this module would have made: a catalog id's .part, .new and .old. Any other name in mods\\ is the player's. */
static void note_id(void* ctx, const char* id)
{
    TO_SETTLE* t = (TO_SETTLE*)ctx;
    char a[MAX_PATH], b[MAX_PATH], c[MAX_PATH];
    if (t->n == MAX_SETTLE || strlen(id) >= sizeof t->ids[0]) return;
    if (!path_of(a, sizeof a, "%s\\%s.new", t->mods, id) || !path_of(b, sizeof b, "%s\\%s.old", t->mods, id) || !path_of(c, sizeof c, "%s\\%s.part", t->mods, id)) return;
    if (is_dir(a) || is_dir(b) || is_file(c)) lstrcpyA(t->ids[t->n++], id);
}

static int may_act_here(void)
{
#ifdef MODINST_TEST
    return 1;
#else
    return tagpu_modload_is_game();
#endif
}

void tagpu_modinst_sweep(const char* catalog, size_t n)
{
    char root[MAX_PATH], mods[MAX_PATH];
    TO_SETTLE t;
    HANDLE lock;
    int i;

    if (!game_root(root, sizeof root) || !path_of(mods, sizeof mods, "%s\\mods", root) || !is_dir(mods)) return;
    t.mods = mods;
    t.n = 0;
    if (!tagpu_modcat_each_id(catalog, n, note_id, &t) || !t.n || !may_act_here()) return;
    lock = take_lock(mods);
    if (lock == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) tagpu_log("modinst: another copy of the game is installing a mod, so this start leaves mods\\ alone");
        else tagpu_logf("modinst: mods\\ cannot be written (error %lu), so this start leaves it alone", GetLastError());
        return;
    }
    for (i = 0; i < t.n; i++) settle_one(mods, t.ids[i]);
    CloseHandle(lock);
}

int tagpu_modinst_locked(void)
{
    char root[MAX_PATH], path[MAX_PATH];
    HANDLE f;
    if (!game_root(root, sizeof root) || !path_of(path, sizeof path, "%s\\mods\\" LOCK, root)) return 0;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); return 0; }
    return GetLastError() == ERROR_SHARING_VIOLATION;
}

/* ---- the download -------------------------------------------------------------------------- */

/* WinHTTP is loaded only when a download runs: nothing else in the game needs it, and a ddraw.dll's imports
   are resolved on every start. */
typedef HINTERNET (WINAPI* PFN_Open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI* PFN_Connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI* PFN_OpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
typedef BOOL (WINAPI* PFN_Send)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI* PFN_Receive)(HINTERNET, LPVOID);
typedef BOOL (WINAPI* PFN_Query)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL (WINAPI* PFN_Read)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI* PFN_Close)(HINTERNET);
typedef BOOL (WINAPI* PFN_Crack)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
typedef BOOL (WINAPI* PFN_Timeouts)(HINTERNET, int, int, int, int);
typedef BOOL (WINAPI* PFN_SetOption)(HINTERNET, DWORD, LPVOID, DWORD);

static int fail(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_why, sizeof s_why, fmt, ap);
    va_end(ap);
    return 0;
}

/* Bytes the server sends past the declared size are never written: the file cannot outgrow what the catalog names. */
static int download(const char* url, const char* part, uint32_t size)
{
    HMODULE lib = LoadLibraryA("winhttp.dll");
    HINTERNET session = NULL, conn = NULL, req = NULL;
    HANDLE out = INVALID_HANDLE_VALUE;
    unsigned char* buf = NULL;
    wchar_t wurl[1024], host[256], rest[1024];
    URL_COMPONENTS uc;
    DWORD status = 0, nstatus = sizeof status, n, wrote;
    uint32_t got = 0;
    int ok = 0;
    PFN_Open w_open;
    PFN_Connect w_connect;
    PFN_OpenRequest w_request;
    PFN_Send w_send;
    PFN_Receive w_receive;
    PFN_Query w_query;
    PFN_Read w_read;
    PFN_Close w_close;
    PFN_Crack w_crack;
    PFN_Timeouts w_timeouts;
    PFN_SetOption w_option;

    if (!lib) return fail("this system has no winhttp.dll");
    w_open = (PFN_Open)GetProcAddress(lib, "WinHttpOpen");
    w_connect = (PFN_Connect)GetProcAddress(lib, "WinHttpConnect");
    w_request = (PFN_OpenRequest)GetProcAddress(lib, "WinHttpOpenRequest");
    w_send = (PFN_Send)GetProcAddress(lib, "WinHttpSendRequest");
    w_receive = (PFN_Receive)GetProcAddress(lib, "WinHttpReceiveResponse");
    w_query = (PFN_Query)GetProcAddress(lib, "WinHttpQueryHeaders");
    w_read = (PFN_Read)GetProcAddress(lib, "WinHttpReadData");
    w_close = (PFN_Close)GetProcAddress(lib, "WinHttpCloseHandle");
    w_crack = (PFN_Crack)GetProcAddress(lib, "WinHttpCrackUrl");
    w_timeouts = (PFN_Timeouts)GetProcAddress(lib, "WinHttpSetTimeouts");
    w_option = (PFN_SetOption)GetProcAddress(lib, "WinHttpSetOption");
    if (!w_open || !w_connect || !w_request || !w_send || !w_receive || !w_query || !w_read || !w_close || !w_crack || !w_timeouts || !w_option) {
        fail("this system's winhttp.dll lacks a function a download needs");
        goto done;
    }

    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, sizeof wurl / sizeof wurl[0])) { fail("the package's address is too long"); goto done; }
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = sizeof host / sizeof host[0];
    uc.lpszUrlPath = rest;
    uc.dwUrlPathLength = sizeof rest / sizeof rest[0];
    if (!w_crack(wurl, 0, 0, &uc)) { fail("the package's address is not one this can fetch: %s", url); goto done; }

    session = w_open(L"Impure", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { fail("the download could not start (error %lu)", GetLastError()); goto done; }
    w_timeouts(session, 30000, 30000, 30000, 60000);
    {
        /* TLS 1.2 is off by default on older systems, and they refuse a list naming 1.3 (0x2000), which mingw's header may lack */
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000u;
        if (!w_option(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            w_option(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
    }
    conn = w_connect(session, host, uc.nPort, 0);
    req = conn ? w_request(conn, L"GET", rest, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (!req || !w_send(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !w_receive(req, NULL)) {
        fail("could not reach the download server (error %lu)", GetLastError());
        goto done;
    }
    if (!w_query(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &nstatus, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        fail("the download server answered %lu", status);
        goto done;
    }

    out = CreateFileA(part, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    buf = (unsigned char*)malloc(IO_CHUNK);
    if (out == INVALID_HANDLE_VALUE || !buf) { fail("could not write in mods\\ (error %lu)", GetLastError()); goto done; }
    for (;;) {
        if (!w_read(req, buf, IO_CHUNK, &n)) { fail("the download stopped at %u of %u bytes (error %lu)", got, size, GetLastError()); goto done; }
        if (!n) break;
        if (n > size - got) { fail("the server sent more than the package's %u bytes", size); goto done; }
        if (!WriteFile(out, buf, n, &wrote, NULL) || wrote != n) { fail("could not write the download (error %lu); is the disk full?", GetLastError()); goto done; }
        got += n;
        InterlockedExchange(&s_done, (LONG)got);
        STEP("download", got);
    }
    if (got != size) { fail("the download stopped at %u of %u bytes", got, size); goto done; }
    ok = 1;
done:
    free(buf);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (lib) {
        if (req) w_close(req);
        if (conn) w_close(conn);
        if (session) w_close(session);
        FreeLibrary(lib);
    }
    return ok;
}

static int hash_file(const char* path, uint32_t size, const uint8_t want[32])
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    unsigned char* buf = (unsigned char*)malloc(IO_CHUNK);
    MODSHA sha;
    uint8_t got[32];
    DWORD n;
    uint32_t total = 0;
    int ok = 0;

    if (f == INVALID_HANDLE_VALUE || !buf) { fail("could not read the download back"); goto done; }
    modsha_init(&sha);
    for (;;) {
        if (!ReadFile(f, buf, IO_CHUNK, &n, NULL)) { fail("could not read the download back (error %lu)", GetLastError()); goto done; }
        if (!n) break;
        modsha_add(&sha, buf, n);
        total += n;
        InterlockedExchange(&s_done, (LONG)total);
    }
    modsha_end(&sha, got);
    if (total != size || memcmp(got, want, 32)) { fail("the download is damaged: its hash is not the catalog's"); goto done; }
    ok = 1;
done:
    free(buf);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return ok;
}

/* ---- the unpack ---------------------------------------------------------------------------- */

typedef struct {
    char   root[MAX_PATH];
    HANDLE out;
    char   err[120];
    unsigned files;
} SINK;

static void make_parents(const char* path, size_t start)
{
    char dir[MAX_PATH];
    size_t i;
    lstrcpynA(dir, path, sizeof dir);
    for (i = start; dir[i]; i++) {
        if (dir[i] == '\\') {
            dir[i] = 0;
            CreateDirectoryA(dir, NULL);
            dir[i] = '\\';
        }
    }
}

static int sink_begin(void* ctx, const char* path, uint32_t size)
{
    SINK* s = (SINK*)ctx;
    char full[MAX_PATH];
    size_t at, i;
    (void)size;
    if (!path_of(full, sizeof full, "%s\\%s", s->root, path)) {
        snprintf(s->err, sizeof s->err, "this folder's path is too long for %s", path);
        return 0;
    }
    at = strlen(s->root) + 1;
    for (i = at; full[i]; i++) if (full[i] == '/') full[i] = '\\';
    make_parents(full, at);
    /* a new file only: a name given twice in another case cannot overwrite the first */
    s->out = CreateFileA(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s->out == INVALID_HANDLE_VALUE) {
        snprintf(s->err, sizeof s->err, "could not create %s (error %lu)", path, GetLastError());
        return 0;
    }
    return 1;
}

static int sink_data(void* ctx, const void* p, size_t n)
{
    SINK* s = (SINK*)ctx;
    DWORD wrote;
    if (!WriteFile(s->out, p, (DWORD)n, &wrote, NULL) || wrote != n) {
        snprintf(s->err, sizeof s->err, "a write failed (error %lu); is the disk full?", GetLastError());
        return 0;
    }
    return 1;
}

static int sink_end(void* ctx)
{
    SINK* s = (SINK*)ctx;
    /* on the disk before the marker can say it is */
    int ok = FlushFileBuffers(s->out) != 0;
    CloseHandle(s->out);
    s->out = INVALID_HANDLE_VALUE;
    s->files++;
    STEP("unpack", s->files);
    return ok;
}

static int unpack(const char* part, const char* dest, const MODPKG* p)
{
    HANDLE f = CreateFileA(part, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    size_t in_cap = ZSTD_DStreamInSize(), out_cap = ZSTD_DStreamOutSize();
    unsigned char* in = (unsigned char*)malloc(in_cap), *dec = (unsigned char*)malloc(out_cap), *seen = (unsigned char*)malloc((size_t)p->nfiles);
    ZSTD_DStream* z = ZSTD_createDStream();
    MODTAR* tar = (MODTAR*)malloc(sizeof(MODTAR));
    SINK sink;
    MODTAR_SINK ts;
    size_t ret = 1;
    uint32_t read_total = 0;
    int ok = 0;

    memset(&sink, 0, sizeof sink);
    sink.out = INVALID_HANDLE_VALUE;
    lstrcpynA(sink.root, dest, sizeof sink.root);
    ts.begin = sink_begin;
    ts.data = sink_data;
    ts.end = sink_end;
    ts.ctx = &sink;
    if (f == INVALID_HANDLE_VALUE || !in || !dec || !seen || !z || !tar) { fail("could not open the download to unpack it"); goto done; }
    if (ZSTD_isError(ZSTD_DCtx_setParameter(z, ZSTD_d_windowLogMax, WINDOW_LOG_MAX))) { fail("the decompressor refused its bound"); goto done; }
    modtar_init(tar, &ts, p->files, p->nfiles, seen);

    for (;;) {
        DWORD n;
        ZSTD_inBuffer ib;
        if (!ReadFile(f, in, (DWORD)in_cap, &n, NULL)) { fail("could not read the download (error %lu)", GetLastError()); goto done; }
        if (!n) break;
        read_total += n;
        InterlockedExchange(&s_done, (LONG)read_total);
        ib.src = in;
        ib.size = n;
        ib.pos = 0;
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob;
            ob.dst = dec;
            ob.size = out_cap;
            ob.pos = 0;
            ret = ZSTD_decompressStream(z, &ob, &ib);
            if (ZSTD_isError(ret)) { fail("the package is damaged: %s", ZSTD_getErrorName(ret)); goto done; }
            if (ob.pos && !modtar_feed(tar, dec, ob.pos)) {
                fail("the package was refused: %s%s%s", tar->why, sink.err[0] ? ": " : "", sink.err);
                goto done;
            }
        }
    }
    if (ret != 0) { fail("the package was refused: it stops inside its compression"); goto done; }
    if (!modtar_finish(tar)) { fail("the package was refused: %s", tar->why); goto done; }
    ok = 1;
done:
    if (sink.out != INVALID_HANDLE_VALUE) CloseHandle(sink.out);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (z) ZSTD_freeDStream(z);
    free(in);
    free(dec);
    free(seen);
    free(tar);
    return ok;
}

/* ---- the recipe ---------------------------------------------------------------------------- */

typedef struct {
    char        name[64];
    HANDLE      file;
    TAGPU_HPI*  hpi;
} ARCHIVE;

static int arc_read(void* ctx, uint32_t off, void* dst, uint32_t n)
{
    HANDLE h = (HANDLE)ctx;
    LARGE_INTEGER at;
    DWORD got;
    at.QuadPart = off;
    return SetFilePointerEx(h, at, NULL, FILE_BEGIN) && ReadFile(h, dst, n, &got, NULL) && got == n;
}

static TAGPU_HPI* open_archive(ARCHIVE* list, int* n, const char* root, const char* name)
{
    char path[MAX_PATH], why[160];
    TAGPU_HPISRC src;
    LARGE_INTEGER size;
    int i;
    for (i = 0; i < *n; i++) if (!lstrcmpiA(list[i].name, name)) return list[i].hpi;
    if (*n == MAX_ARCHIVES || strlen(name) >= sizeof list[0].name) { fail("the recipe names too many archives"); return NULL; }
    if (!path_of(path, sizeof path, "%s\\%s", root, name)) { fail("this folder's path is too long"); return NULL; }
    list[*n].file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (list[*n].file == INVALID_HANDLE_VALUE) { fail("this folder has no %s, which this mod needs from the game", name); return NULL; }
    if (!GetFileSizeEx(list[*n].file, &size) || size.QuadPart > 0x7FFFFFFF) {
        CloseHandle(list[*n].file);
        fail("this folder's %s is not the game's", name);
        return NULL;
    }
    src.ctx = list[*n].file;
    src.size = (uint32_t)size.QuadPart;
    src.read = arc_read;
    list[*n].hpi = tagpu_hpi_open(&src, why, sizeof why);
    if (!list[*n].hpi) {
        CloseHandle(list[*n].file);
        fail("this folder's %s is not the game's: %s", name, why);
        return NULL;
    }
    lstrcpynA(list[*n].name, name, sizeof list[0].name);
    return list[(*n)++].hpi;
}

/* a member's bytes, read and checked; NULL with the reason set */
static unsigned char* read_member(TAGPU_HPI* a, const char* arc, char* member, uint32_t size, const uint8_t want[32])
{
    char why[160];
    unsigned char* data;
    uint8_t got[32];
    MODSHA sha;
    uint32_t off = 0;
    char* c;
    int e;

    for (c = member; *c; c++) if (*c == '/') *c = '\\';     /* the reader splits as 0x4BB4E0 does */
    e = tagpu_hpi_find(a, member);
    if (e < 0 || tagpu_hpi_is_dir(a, e)) { fail("this folder's %s is not the game's: it lacks %s", arc, member); return NULL; }
    if (tagpu_hpi_size(a, e) != size) { fail("this folder's %s is not the game's: %s differs", arc, member); return NULL; }
    if (!(data = (unsigned char*)malloc(size ? size : 1))) { fail("out of memory"); return NULL; }
    while (off < size) {
        uint32_t n = size - off > TAGPU_HPI_READMAX ? TAGPU_HPI_READMAX : size - off;
        if (!tagpu_hpi_read(a, e, off, n, data + off, why, sizeof why)) {
            fail("this folder's %s could not be read: %s", arc, why);
            free(data);
            return NULL;
        }
        off += n;
    }
    modsha_init(&sha);
    modsha_add(&sha, data, size);
    modsha_end(&sha, got);
    if (memcmp(got, want, 32)) {
        fail("this folder's %s is not the game's: %s differs", arc, member);
        free(data);
        return NULL;
    }
    return data;
}

static int read_all(const char* path, char** out, uint32_t* n, uint32_t cap)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    char* b;
    if (f == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > cap || !(b = (char*)malloc((size_t)size + 1))) { CloseHandle(f); return 0; }
    if (!ReadFile(f, b, size, &got, NULL) || got != size) { free(b); CloseHandle(f); return 0; }
    CloseHandle(f);
    b[size] = 0;
    *out = b;
    *n = size;
    return 1;
}

/* <id>.originals.tsv: path, md5, size, archive, member, sha256, one file a line. The hash is why this can take a
   folder's archive by its name: what it yields is the game's own bytes or the install stops. */
static int run_recipe(const char* root, const char* dest, const char* id)
{
    ARCHIVE archives[MAX_ARCHIVES];
    TAGPU_UFO_FILE* files = NULL;
    char path[MAX_PATH], out[MAX_PATH];
    char* text = NULL, *line, *next;
    uint32_t ntext, total = 0;
    int narch = 0, nfiles = 0, cap = 0, i, ok = 0;

    if (!path_of(path, sizeof path, "%s\\%s.originals.tsv", dest, id) || !path_of(out, sizeof out, "%s\\%s.originals.hpi", dest, id)) {
        return fail("this folder's path is too long");
    }
    if (!read_all(path, &text, &ntext, RECIPE_MAX)) return fail("the package's recipe could not be read");
    {
        unsigned lines = 0;
        for (line = text; *line; line++) lines += *line == '\n';
        InterlockedExchange(&s_total, (LONG)lines);
    }

    for (line = text; line && *line; line = next) {
        char* f[6], *nl = strchr(line, '\n');
        int k;
        uint8_t sha[32];
        unsigned long size;
        TAGPU_HPI* a;
        unsigned char* data;

        next = nl ? nl + 1 : NULL;
        if (nl) *nl = 0;
        for (k = 0, f[0] = line; k < 5; k++) {
            char* tab = strchr(f[k], '\t');
            if (!tab) { fail("the package's recipe has a line it cannot read"); goto done; }
            *tab = 0;
            f[k + 1] = tab + 1;
        }
        size = strtoul(f[2], NULL, 10);
        if (!tagpu_modcat_hash(f[5], sha) || !modtar_path_safe(f[0]) || !modtar_path_safe(f[3]) || strchr(f[3], '/') || size > ORIGINALS_MAX || total + size > ORIGINALS_MAX) {
            fail("the package's recipe has a line it cannot use");
            goto done;
        }
        if (!(a = open_archive(archives, &narch, root, f[3]))) goto done;
        if (!(data = read_member(a, f[3], f[4], (uint32_t)size, sha))) goto done;
        if (nfiles == cap) {
            TAGPU_UFO_FILE* grown;
            cap = cap ? cap * 2 : 256;
            grown = (TAGPU_UFO_FILE*)realloc(files, (size_t)cap * sizeof *files);
            if (!grown) { free(data); fail("out of memory"); goto done; }
            files = grown;
        }
        files[nfiles].path = f[0];
        files[nfiles].data = data;
        files[nfiles].size = (unsigned)size;
        nfiles++;
        total += (uint32_t)size;
        InterlockedExchange(&s_done, (LONG)nfiles);
        STEP("recipe", (unsigned)nfiles);
    }
    if (!tagpu_ufo_write(out, files, nfiles)) { fail("could not write %s.originals.hpi; is the disk full?", id); goto done; }
    ok = 1;
done:
    for (i = 0; i < nfiles; i++) free((void*)files[i].data);
    free(files);
    for (i = 0; i < narch; i++) {
        tagpu_hpi_close(archives[i].hpi);
        CloseHandle(archives[i].file);
    }
    free(text);
    return ok;
}

/* a file's contents on the disk, or 0 */
static int flush_file(const char* path)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = FlushFileBuffers(f) != 0;
    CloseHandle(f);
    return ok;
}

/* Under another name until it is on the disk, so a folder either has its whole marker or none. */
static int write_marker(const char* dest, const char* version)
{
    char path[MAX_PATH], tmp[MAX_PATH];
    HANDLE f;
    DWORD wrote;
    int ok;
    if (!path_of(path, sizeof path, "%s\\" MARKER, dest) || !path_of(tmp, sizeof tmp, "%s\\" MARKER ".tmp", dest)) return 0;
    f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(f, version, (DWORD)strlen(version), &wrote, NULL) && wrote == strlen(version);
    ok = ok && FlushFileBuffers(f);
    CloseHandle(f);
    return ok && MoveFileA(tmp, path);
}

/* ---- the job ------------------------------------------------------------------------------- */

static int install(JOB* j)
{
    const char* id = j->m.id;
    char mods[MAX_PATH], part[MAX_PATH], dest[MAX_PATH], url[1024], hpi[MAX_PATH];
    const char* base = j->base[0] ? j->base : j->m.base_url;
    int i;

    if (!path_of(mods, sizeof mods, "%s\\mods", j->root) || !path_of(part, sizeof part, "%s\\%s.part", mods, id) ||
        !path_of(dest, sizeof dest, "%s\\%s.new", mods, id) || !path_of(hpi, sizeof hpi, "%s\\%s.originals.hpi", dest, id)) {
        return fail("this folder's path is too long");
    }
    /* A finished update waiting for its start goes first: this one replaces it. */
    if (!retire_marker(dest)) {
        j->keep_new = 1;
        return fail("mods\\%s.new is in use, so the update waiting there stays as it is", id);
    }
    STEP("retired", 0);
    if (!remove_tree(dest) || !CreateDirectoryA(dest, NULL)) return fail("could not make mods\\%s.new (error %lu)", id, GetLastError());

    for (i = 0; i < j->m.npackages; i++) {
        const MODPKG* p = &j->m.packages[i];
        int fits;
        if (strstr(p->url, "://")) fits = path_of(url, sizeof url, "%s", p->url);
        else if (!base[0]) return fail("the catalog does not yet name where to download from");
        else fits = path_of(url, sizeof url, "%s%s%s", base, base[strlen(base) - 1] == '/' ? "" : "/", p->url);
        if (!fits) return fail("the package's address is too long");

        set_phase(MODINST_DOWNLOADING, p->size);
        if (!download(url, part, p->size)) return 0;
        set_phase(MODINST_CHECKING, p->size);
        if (!hash_file(part, p->size, p->sha256)) return 0;
        set_phase(MODINST_UNPACKING, p->size);
        if (!unpack(part, dest, p)) return 0;
        DeleteFileA(part);
    }
    set_phase(MODINST_RECIPE, 0);
    if (!run_recipe(j->root, dest, id)) return 0;
    if (!flush_file(hpi)) return fail("could not finish %s.originals.hpi (error %lu)", id, GetLastError());
    STEP("marker", 0);
    if (!write_marker(dest, j->m.version)) return fail("could not finish mods\\%s.new (error %lu)", id, GetLastError());
    return 1;
}

static DWORD WINAPI worker(void* arg)
{
    JOB* j = (JOB*)arg;
    char part[MAX_PATH], dest[MAX_PATH];
    int ok = install(j);
    HMODULE self = NULL;

    tagpu_logf("modinst: %s: %s", j->m.id, ok ? "installed; it takes effect at the next start" : s_why);
    if (!ok) {
        if (path_of(part, sizeof part, "%s\\mods\\%s.part", j->root, j->m.id)) DeleteFileA(part);
        if (!j->keep_new && path_of(dest, sizeof dest, "%s\\mods\\%s.new", j->root, j->m.id) && retire_marker(dest)) remove_tree(dest);
    }
    CloseHandle(j->lock);
    tagpu_modcat_install_free(&j->m);
    free(j);
    InterlockedExchange(&s_phase, ok ? MODINST_READY : MODINST_FAILED);
    InterlockedExchange(&s_busy, 0);
    /* The reference taken at the start: this code cannot be unmapped under its own last instructions. */
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)worker, &self);
    FreeLibraryAndExitThread(self, 0);
    return 0;
}

int tagpu_modinst_start(const char* catalog, size_t n, const char* id, const char* base, char* why, size_t nwhy)
{
    JOB* j;
    char err[200], mods[MAX_PATH];
    HANDLE t;
    HMODULE self = NULL;

    if (InterlockedCompareExchange(&s_busy, 1, 0)) {
        snprintf(why, nwhy, "another mod is installing");
        return 0;
    }
    if (!(j = (JOB*)calloc(1, sizeof *j))) { snprintf(why, nwhy, "out of memory"); goto refused; }
    j->lock = INVALID_HANDLE_VALUE;
    if (!tagpu_modcat_install(catalog, n, id, &j->m, err, sizeof err)) { snprintf(why, nwhy, "%s", err); free(j); j = NULL; goto refused; }
    {
        /* The marker is what says a folder is complete, so a package can never carry it: half a folder would pass for whole. */
        int i, k;
        for (i = 0; i < j->m.npackages; i++) {
            for (k = 0; k < j->m.packages[i].nfiles; k++) {
                if (!lstrcmpiA(j->m.packages[i].files[k].path, MARKER)) {
                    snprintf(why, nwhy, "a package of %s declares a file named " MARKER, id);
                    goto refused;
                }
            }
        }
    }
    if (base && strlen(base) >= sizeof j->base) { snprintf(why, nwhy, "the download address is too long"); goto refused; }
    if (base) strcpy(j->base, base);
    if (!game_root(j->root, sizeof j->root) || !path_of(mods, sizeof mods, "%s\\mods", j->root)) { snprintf(why, nwhy, "this folder's path is too long"); goto refused; }
    CreateDirectoryA(mods, NULL);
    j->lock = take_lock(mods);
    if (j->lock == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) snprintf(why, nwhy, "another copy of the game is installing a mod");
        else snprintf(why, nwhy, "mods\\ cannot be written (error %lu)", GetLastError());
        goto refused;
    }
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)worker, &self)) { snprintf(why, nwhy, "could not hold this DLL (error %lu)", GetLastError()); goto refused; }
    s_why[0] = 0;
    set_phase(MODINST_DOWNLOADING, 0);
    t = CreateThread(NULL, 0, worker, j, 0, NULL);
    if (!t) {
        snprintf(why, nwhy, "could not start the install (error %lu)", GetLastError());
        FreeLibrary(self);
        InterlockedExchange(&s_phase, MODINST_IDLE);
        goto refused;
    }
    CloseHandle(t);
    return 1;
refused:
    if (j) {
        if (j->lock != INVALID_HANDLE_VALUE) CloseHandle(j->lock);
        tagpu_modcat_install_free(&j->m);
        free(j);
    }
    InterlockedExchange(&s_busy, 0);
    return 0;
}

void tagpu_modinst_status(MODINST_STATUS* out)
{
    out->phase = (MODINST_PHASE)InterlockedCompareExchange(&s_phase, 0, 0);
    out->done = (unsigned)InterlockedCompareExchange(&s_done, 0, 0);
    out->total = (unsigned)InterlockedCompareExchange(&s_total, 0, 0);
    /* s_why is written before the phase says FAILED, and rewritten only by tagpu_modinst_start on this same thread */
    out->why[0] = 0;
    if (out->phase == MODINST_FAILED) lstrcpynA(out->why, s_why, sizeof out->why);
}

int tagpu_modinst_installed(const char* id, char* out, int cap)
{
    char root[MAX_PATH], path[MAX_PATH];
    HANDLE f;
    DWORD got = 0;
    if (cap < 1 || !game_root(root, sizeof root) || !path_of(path, sizeof path, "%s\\mods\\%s\\" MARKER, root, id)) return 0;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(f, out, (DWORD)cap - 1, &got, NULL)) got = 0;
    CloseHandle(f);
    out[got] = 0;
    return got != 0;
}

#ifndef MODINST_TEST
void tagpu_modinst_sweep_at_start(void)
{
    size_t npak = 0, ncat = 0;
    const unsigned char* pak = tagpu_pak_resource("MODPAK", &npak);
    const unsigned char* cat = pak ? tagpu_pak_file(pak, npak, "catalog.json", &ncat) : NULL;
    if (cat) tagpu_modinst_sweep((const char*)cat, ncat);
}

void tagpu_modinst_lever(void)
{
    char root[MAX_PATH], path[MAX_PATH], text[600], id[64], base[512], why[200];
    HANDLE f;
    DWORD got = 0;
    size_t npak = 0, ncat = 0;
    const unsigned char* pak, *cat;

    if (!game_root(root, sizeof root) || !path_of(path, sizeof path, "%s\\tagpu_modinst.on", root)) return;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(f, text, sizeof text - 1, &got, NULL)) got = 0;
    CloseHandle(f);
    text[got] = 0;
    if (!may_act_here()) return;
    if (sscanf(text, "%63s %511s", id, base) != 2) { tagpu_log("modinst: tagpu_modinst.on does not read <id> <base url>"); return; }
    pak = tagpu_pak_resource("MODPAK", &npak);
    cat = pak ? tagpu_pak_file(pak, npak, "catalog.json", &ncat) : NULL;
    if (!cat) { tagpu_log("modinst: this DLL holds no catalog"); return; }
    if (!tagpu_modinst_start((const char*)cat, ncat, id, base, why, sizeof why)) tagpu_logf("modinst: %s: %s", id, why);
}
#endif
