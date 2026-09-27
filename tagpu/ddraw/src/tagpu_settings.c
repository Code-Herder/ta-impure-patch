/* tagpu_settings.c -- the player's settings store (tagpu_settings.h,
   renderers.md 2.10b). */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_log.h"

#include "tagpu_settings.h"

#define STORE      "impure.cfg"
#define STORE_TMP  "impure.cfg.tmp"
#define MASTER_OFF "tagpu_defaults.off"
#define MIGRATED   ".migrated"
#define RECORD     "impure-migration.txt"
#define EXTRA_LEN  1024
#define RETRY_MS   1000
#define GPU_LEN    128
#define MON_MAX    8
#define MON_LEN    40

static void slog(const char* fmt, ...)
{
    char b[700];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    tagpu_logf("settings: %s", b);
}

static int exists(const char* p)
{
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

/* ---- the keys ------------------------------------------------------------
   Each key is a closed list of (spelling, value) pairs, and the list IS the
   bound: a value is valid exactly when it is one of the pairs' values. The
   first spelling is the one written; the others are read, so a hand-edited
   `shadows=2` or `ss=on` means what it looks like. */
typedef struct { const char* text; int value; } Opt;
typedef struct {
    const char* name;
    const Opt*  opts;
    int         nopts;
    int         def;
} Key;

static const Opt O_STYLE[]   = { {"classic++", TS_STYLE_PP}, {"classic", TS_STYLE_CLASSIC},
                                 {"custom", TS_STYLE_CUSTOM} };
static const Opt O_BOOL[]    = { {"1", 1}, {"0", 0}, {"on", 1}, {"off", 0} };
/* `soft` and 1 are READ as hard: the soft map has no producer on this lane, so
   honouring them would draw nothing (tagpu_classicpp.c's `shadow_defaults`). */
static const Opt O_SHADOWS[] = { {"hard", 2}, {"off", 0}, {"2", 2}, {"0", 0},
                                 {"soft", 2}, {"1", 2} };
static const Opt O_RES[]     = { {"2048", 2048}, {"512", 512}, {"1024", 1024}, {"4096", 4096} };
static const Opt O_SS[]      = { {"2", 2}, {"1", 1}, {"on", 2}, {"off", 1} };
static const Opt O_HUD[]     = { {"off", -1}, {"auto", 0}, {"100", 100}, {"150", 150},
                                 {"200", 200}, {"300", 300}, {"400", 400} };
static const Opt O_DISPLAY[] = { {"fullscreen", 1}, {"window", 0} };
static const Opt O_GAMMA[]   = { {"12", 12}, {"0", 0}, {"1", 1}, {"2", 2}, {"3", 3}, {"4", 4},
                                 {"5", 5}, {"6", 6}, {"7", 7}, {"8", 8}, {"9", 9}, {"10", 10},
                                 {"11", 11}, {"13", 13}, {"14", 14}, {"15", 15}, {"16", 16},
                                 {"17", 17}, {"18", 18}, {"19", 19}, {"20", 20} };
/* THE ENGINE'S TABLE IS THE BOUND. The sound object tracks at most 32 playing
   buffers (+0x38, +0xB8, +0x138, every loop `cmp 0x20`), and MixingBuffers is
   the count at which it evicts one (0x4CF570 -> 0x4CF180). Past 32 the
   eviction never fires and a 33rd sound plays untracked, where the stop-all
   0x4CF150 cannot reach it -- a looping one plays on. Below 2 the eviction
   can find no victim (only one looping sound is tracked at a time) and reads
   past the table. research/notes/tadr-port/limits-evidence.md §11. */
static const Opt O_MIXING[]  = { {"32", 32}, {"8", 8}, {"16", 16}, {"24", 24} };
/* The map edge: the world reflected past the map, or the black the engine
   leaves there (bar-camera-port.md 1.2). Not a render key: it is the
   player's under every style, as the shadows row is. */
static const Opt O_EDGE[]    = { {"mirror", 1}, {"black", 0}, {"1", 1}, {"0", 0},
                                 {"on", 1}, {"off", 0} };

#define N(a) (int)(sizeof a / sizeof a[0])

/* TS_MONITOR and TS_RESOLUTION have no option list: the monitor is stored by
   device NAME and resolved to an index against the registered list
   (`tagpu_settings_monitors`), the resolution as `native` or `WxH`. */
static const Key s_key[TS_NKEYS] = {
    { "style",     O_STYLE,   N(O_STYLE),   TS_STYLE_PP },
    { "assets",    O_BOOL,    N(O_BOOL),    1 },
    { "light",     O_BOOL,    N(O_BOOL),    1 },
    { "shadows",   O_SHADOWS, N(O_SHADOWS), 2 },
    { "shadowres", O_RES,     N(O_RES),     2048 },
    { "ss",        O_SS,      N(O_SS),      2 },
    { "fps",       O_BOOL,    N(O_BOOL),    0 },
    /* OFF, not Auto: HUD scale is off the play defaults and whether it belongs
       on is the owner's call (tagpu_opt.c). Off is also what the pass reads when
       nothing has armed it, so the default changes nothing. */
    { "hudscale",  O_HUD,     N(O_HUD),     -1 },
    { "display",   O_DISPLAY, N(O_DISPLAY), 1 },
    { "monitor",   NULL,      0,            -1 },
    /* 12 is the engine's own default (0x4301C0) and its factor 1.0: the slider
       applies 0.5 + Gamma/24 (exe-reverse-engineering.md, SetGamma 0x4BA590) */
    { "gamma",     O_GAMMA,   N(O_GAMMA),   12 },
    { "resolution", NULL,     0,            0 },
    { "mixingbuffers", O_MIXING, N(O_MIXING), 32 },
    { "edge",      O_EDGE,    N(O_EDGE),    1 },
    /* ON: a player sees no tearing unless they ask for the lower latency
       (renderers.md 2.10b). */
    { "vsync",     O_BOOL,    N(O_BOOL),    1 },
};

static volatile LONG s_val[TS_NKEYS];
static volatile LONG s_gen;
/* A COUNT of changes, not a flag: the flush clears it only by the count it
   saw, and only once the write has landed (flush). */
static volatile LONG s_dirty;
static int  s_ignored;                  /* tagpu_defaults.off, latched at attach */
static int  s_attached;
/* The store exists and could not be read: the session runs at the defaults and
   NEVER writes, because a write would replace the player's file with them. */
static int  s_unreadable;
static DWORD s_retryAt;                 /* flushing thread only: see flush */
static int   s_failing;
static volatile LONG s_detaching;       /* process detach: see tagpu_settings_detaching */

/* The strings. Written at attach, then only under s_io, which every reader
   also takes -- so no half-written name is ever serialised. */
static CRITICAL_SECTION s_io;
static char s_gpu[GPU_LEN];
static char s_restoreOff[GPU_LEN];     /* tagpu_restore_guard.h's record      */
static char s_monName[MON_MAX][MON_LEN];
static int  s_monCount;
static char s_monStored[MON_LEN];      /* the name the file held            */
static int  s_win[4], s_winSet;
/* Keys this build does not know, verbatim, so a file an other build wrote
   keeps them. Filled at attach, read by serialise under s_io. */
static char s_extra[EXTRA_LEN];
static int  s_extraLen;

/* A windowed frame: a position, and a client size that is either 0,0 -- the
   size the game asks for, cnc-ddraw's own meaning of a zero window_rect size --
   or at least TA's 320x240 floor. -32000 is the fork's "centre me"
   sentinel and is not a position anybody left. */
static int frame_ok(const int w[4])
{
    int sized = w[2] >= 320 && w[3] >= 240 && w[2] <= 16384 && w[3] <= 16384;
    int bare  = w[2] == 0 && w[3] == 0;
    return (sized || bare) &&
           w[0] > -32000 && w[1] > -32000 && w[0] < 32000 && w[1] < 32000;
}

/* A size TA can run at: its own 640x480 floor, and a ceiling far past any
   monitor. Whether the selected monitor offers it is tagpu_menu.c's `eng_resolve`. */
static int res_ok(int v)
{
    int w = TS_RES_W(v), h = TS_RES_H(v);
    return v == 0 || (w >= 640 && w <= 16384 && h >= 480 && h <= 16384);
}

static int valid(TagpuSetting k, int v)
{
    int i;
    if (k == TS_MONITOR) return v == -1 || (v >= 0 && v < s_monCount);
    if (k == TS_RESOLUTION) return res_ok(v);
    for (i = 0; i < s_key[k].nopts; i++)
        if (s_key[k].opts[i].value == v) return 1;
    return 0;
}

static const char* spelling(TagpuSetting k, int v)
{
    int i;
    for (i = 0; i < s_key[k].nopts; i++)
        if (s_key[k].opts[i].value == v) return s_key[k].opts[i].text;
    return "?";
}

int tagpu_settings_preset(TagpuSetting key)
{
    switch (key) {
    case TS_ASSETS:    return 1;
    case TS_LIGHT:     return 1;
    case TS_SHADOWRES: return 2048;
    default:           return s_key[key].def;
    }
}

static int is_render_key(TagpuSetting k)
{
    return k == TS_ASSETS || k == TS_LIGHT || k == TS_SHADOWRES;
}

/* BOUNDED ON EVERY READ, not only on the way in: the value crossed a thread,
   and an index that was valid when it was written is only trusted after this. */
static int read_val(TagpuSetting k)
{
    int v = (int)s_val[k];
    return valid(k, v) ? v : s_key[k].def;
}

int tagpu_settings_get(TagpuSetting key, int* out)
{
    int v;
    if (key < 0 || key >= TS_NKEYS || s_ignored || !s_attached) return 0;
    if (is_render_key(key) && read_val(TS_STYLE) != TS_STYLE_CUSTOM)
        v = tagpu_settings_preset(key);
    else
        v = read_val(key);
    if (out) *out = v;
    return 1;
}

int tagpu_settings_ignored(void) { return s_ignored; }

int tagpu_settings_placement(TagpuSetting key, int* out)
{
    if ((key != TS_DISPLAY && key != TS_VSYNC) || !s_attached) return 0;
    if (out) *out = read_val(key);
    return 1;
}

long tagpu_settings_gen(void) { return (long)s_gen; }

void tagpu_settings_set(TagpuSetting key, int value)
{
    /* no say, no write: under tagpu_defaults.off the menu greys every row, and
       a value written here would reach the next launch that does read it */
    if (key < 0 || key >= TS_NKEYS || s_ignored || !s_attached) return;
    if (!valid(key, value)) {
        slog("refused %s=%d: not one of its values", s_key[key].name, value);
        return;
    }
    if ((int)s_val[key] == value) return;
    InterlockedExchange(&s_val[key], value);
    /* the generation SECOND, and with a barrier: a reader that sees it has the
       value behind it */
    InterlockedIncrement(&s_gen);
    InterlockedIncrement(&s_dirty);
}

/* ---- the file ------------------------------------------------------------ */

static int serialise(char* b, int cap)
{
    int at = 0, i, k;
    k = _snprintf(b, cap,
        "# impure.cfg -- Total Annihilation: Impure's settings. The in-game menu\r\n"
        "# writes this file; a key that is missing takes its default.\r\n");
    if (k < 0) return -1;
    at = k;
    for (i = 0; i < TS_NKEYS; i++) {
        if (i == TS_MONITOR) {
            int m = read_val(TS_MONITOR);
            const char* name = m >= 0 ? s_monName[m] : s_monStored;
            if (!name[0]) continue;               /* nobody has chosen one */
            k = _snprintf(b + at, cap - at, "monitor=%s\r\n", name);
        } else if (i == TS_RESOLUTION) {
            int v = read_val(TS_RESOLUTION);
            k = v ? _snprintf(b + at, cap - at, "resolution=%dx%d\r\n", TS_RES_W(v), TS_RES_H(v))
                  : _snprintf(b + at, cap - at, "resolution=native\r\n");
        } else {
            /* the value IN FORCE: under Classic or Classic++ a render key is the
               preset's, whatever a custom session left in the slot */
            int v = is_render_key((TagpuSetting)i) && read_val(TS_STYLE) != TS_STYLE_CUSTOM
                    ? tagpu_settings_preset((TagpuSetting)i) : read_val((TagpuSetting)i);
            k = _snprintf(b + at, cap - at, "%s=%s\r\n", s_key[i].name,
                          spelling((TagpuSetting)i, v));
        }
        if (k < 0 || k >= cap - at) return -1;
        at += k;
    }
    k = _snprintf(b + at, cap - at, "gpu=%s\r\n", s_gpu[0] ? s_gpu : "auto");
    if (k < 0 || k >= cap - at) return -1;
    at += k;
    /* only while there is a record: a player whose restorer works never sees
       the key */
    if (s_restoreOff[0]) {
        k = _snprintf(b + at, cap - at, "restoreoff=%s\r\n", s_restoreOff);
        if (k < 0 || k >= cap - at) return -1;
        at += k;
    }
    if (s_winSet) {
        k = _snprintf(b + at, cap - at, "window=%d,%d,%d,%d\r\n",
                      s_win[0], s_win[1], s_win[2], s_win[3]);
        if (k < 0 || k >= cap - at) return -1;
        at += k;
    }
    if (s_extraLen) {
        if (s_extraLen >= cap - at) return -1;
        memcpy(b + at, s_extra, s_extraLen);
        at += s_extraLen;
    }
    return at;
}

/* Temporary and RENAME: CREATE_ALWAYS truncates first, so a write in place
   leaves the player an empty file if we die between the two. Under s_io.
   Returns 0 when STORE is left as it was. The failures are logged once per
   run of them, since the caller retries. */
static int write_store(void)
{
    char b[3072];
    HANDLE h;
    DWORD wrote = 0;
    const char* why = NULL;
    DWORD err = 0;
    int len = serialise(b, sizeof b);

    if (len < 0) why = "the store does not fit its buffer";
    else {
        h = CreateFileA(STORE_TMP, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, 0);
        if (h == INVALID_HANDLE_VALUE) { why = "cannot create " STORE_TMP; err = GetLastError(); }
        else {
            int ok = WriteFile(h, b, (DWORD)len, &wrote, 0) && wrote == (DWORD)len;
            if (!ok) { why = "short write to " STORE_TMP; err = GetLastError(); }
            CloseHandle(h);
            if (!why && !MoveFileExA(STORE_TMP, STORE, MOVEFILE_REPLACE_EXISTING)) {
                why = "cannot replace " STORE;
                err = GetLastError();
            }
            if (why) DeleteFileA(STORE_TMP);
        }
    }
    if (why) {
        if (!s_failing) slog("%s (error %lu) - " STORE " left as it was, retried", why, err);
        s_failing = 1;
        return 0;
    }
    if (s_failing) slog(STORE " written after a failed attempt");
    s_failing = 0;
    return 1;
}

/* CLEARED ONLY BY WHAT WAS WRITTEN, AND ONLY AFTER IT WAS. The count is read
   before serialise reads the values, so everything it covers is in the file;
   it is then taken back with a compare-exchange, so a set that raced the write
   leaves the store marked. A write that fails -- or a thread killed inside
   one -- leaves the count where it was, for a later frame (at most once a
   second: a file a scanner holds should not be hammered at the frame rate) or
   the final flush. The file itself is only ever replaced whole (write_store). */
static void flush(int final)
{
    LONG seen;
    if (!s_attached || s_unreadable) return;
    if (!final && s_retryAt && (LONG)(GetTickCount() - s_retryAt) < 0) return;
    seen = s_dirty;
    if (!seen) return;
    if (final) {
        /* Only ever called once no other thread can be inside s_io: after the
           render thread is joined, or at process detach, when every other
           thread is gone. So a lock that is taken here is ORPHANED -- a thread
           killed inside a write -- and waiting on it would hang the exit. */
        if (!TryEnterCriticalSection(&s_io)) {
            slog("the store's lock was left held by a dead thread - the last change is NOT written");
            return;
        }
    } else {
        EnterCriticalSection(&s_io);
    }
    if (write_store()) {
        InterlockedCompareExchange(&s_dirty, 0, seen);
        s_retryAt = 0;
    } else {
        s_retryAt = GetTickCount() + RETRY_MS;
        if (!s_retryAt) s_retryAt = 1;
    }
    LeaveCriticalSection(&s_io);
}

void tagpu_settings_flush(void) { flush(0); }
void tagpu_settings_final(void) { flush(1); }
void tagpu_settings_detaching(void) { InterlockedExchange(&s_detaching, 1); }

static char* trim(char* s)
{
    char* e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (unsigned char)e[-1] <= ' ') *--e = 0;
    return s;
}

static void parse_line(char* line)
{
    char* eq = strchr(line, '=');
    char* k;
    char* v;
    int i, j;

    if (!eq) return;
    *eq = 0;
    k = trim(line);
    v = trim(eq + 1);
    if (!*k || *k == '#' || *k == ';') return;

    if (!lstrcmpiA(k, "gpu"))     { lstrcpynA(s_gpu, v, sizeof s_gpu); return; }
    if (!lstrcmpiA(k, "restoreoff")) { lstrcpynA(s_restoreOff, v, sizeof s_restoreOff); return; }
    if (!lstrcmpiA(k, "monitor")) { lstrcpynA(s_monStored, v, sizeof s_monStored); return; }
    if (!lstrcmpiA(k, "window")) {
        int w[4];
        /* refused when it is not a frame, so the fork's default placement
           stands */
        if (sscanf(v, "%d,%d,%d,%d", &w[0], &w[1], &w[2], &w[3]) == 4 && frame_ok(w)) {
            memcpy(s_win, w, sizeof s_win);
            s_winSet = 1;
        } else {
            slog("window=%s ignored: not a frame", v);
        }
        return;
    }
    if (!lstrcmpiA(k, "resolution")) {
        int w, h;
        if (!lstrcmpiA(v, "native")) { s_val[TS_RESOLUTION] = 0; return; }
        if (sscanf(v, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w < 32768 && h < 32768 &&
            res_ok(TS_RES(w, h))) {
            s_val[TS_RESOLUTION] = TS_RES(w, h);
            return;
        }
        slog("resolution=%s is not a screen size - native stands", v);
        return;
    }
    for (i = 0; i < TS_NKEYS; i++) {
        if (!s_key[i].opts || lstrcmpiA(k, s_key[i].name)) continue;
        for (j = 0; j < s_key[i].nopts; j++)
            if (!lstrcmpiA(v, s_key[i].opts[j].text)) {
                s_val[i] = s_key[i].opts[j].value;
                return;
            }
        slog("%s=%s is not one of its values - the default %s stands",
             s_key[i].name, v, spelling((TagpuSetting)i, s_key[i].def));
        return;
    }
    /* kept verbatim: another build wrote it, and this one does not own it */
    {
        int n = _snprintf(s_extra + s_extraLen, sizeof s_extra - s_extraLen, "%s=%s\r\n", k, v);
        if (n > 0 && n < (int)sizeof s_extra - s_extraLen) {
            s_extraLen += n;
            slog("unknown key \"%s\" kept as it is", k);
        } else {
            s_extra[s_extraLen] = 0;
            slog("unknown key \"%s\" DROPPED: no room to keep it", k);
        }
    }
}

static void load(void)
{
    char buf[4096];
    HANDLE h;
    DWORD n = 0;
    char* p;

    h = CreateFileA(STORE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE || !ReadFile(h, buf, sizeof buf - 1, &n, 0)) {
        s_unreadable = 1;
        slog(STORE " exists and could NOT be read (error %lu) - this session runs at the "
             "defaults and will not write it", GetLastError());
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return;
    }
    CloseHandle(h);
    buf[n] = 0;
    if (n >= sizeof buf - 1) slog(STORE " is larger than the read buffer - the tail was IGNORED");

    p = buf;
    while (*p) {
        char* e = p;
        while (*e && *e != '\n') e++;
        if (*e) *e++ = 0;
        parse_line(p);
        p = e;
    }
}

/* ---- the first-run migration ---------------------------------------------
   Everything an earlier menu wrote is a lever under the new precedence, and a
   lever holds its row greyed -- so a file v0.2..v0.2.2 left behind would pin a
   row for good. Renamed, never deleted: a bad migration is undone by hand.
   Nothing is IMPORTED; the store starts at the defaults (renderers.md 2.10b).

   ONCE PER DIRECTORY, BY RECORD. A missing store is not proof of a first run --
   a player resets by deleting it -- and anything they have typed since is
   theirs. So the migration leaves RECORD behind, listing what it did, and a
   directory that has one only gets the defaults written. No backup is ever
   overwritten either: a backup that already exists means the thing it would
   back up is not the original, and the step is refused. */
static char s_rec[1024];
static int  s_recLen;

static void record(const char* fmt, ...)
{
    va_list ap;
    int n, room = (int)sizeof s_rec - s_recLen - 3;   /* CR, LF and the NUL */
    if (room <= 0) return;
    va_start(ap, fmt);
    n = _vsnprintf(s_rec + s_recLen, room, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= room) { s_rec[s_recLen] = 0; return; }
    s_recLen += n;
    s_rec[s_recLen++] = '\r';
    s_rec[s_recLen++] = '\n';
    s_rec[s_recLen] = 0;
}

static void rename_aside(const char* path)
{
    char to[MAX_PATH];
    if (!exists(path)) return;
    _snprintf(to, sizeof to, "%s" MIGRATED, path);
    to[sizeof to - 1] = 0;
    if (MoveFileExA(path, to, 0)) {
        slog("migrated: %s -> %s", path, to);
        record("renamed %s -> %s", path, to);
    }
    else
        slog("migration could not rename %s (error %lu) - it still holds its row",
             path, GetLastError());
}

/* The four keys the menu used to write into tagpu_classicpp.cfg, and `sun=off`,
   the legacy spelling of `light=0` it also owned. Every other token is a
   developer knob with no row and stays. */
static int menu_token(const char* t)
{
    return !_strnicmp(t, "assets=", 7) || !_strnicmp(t, "light=", 6) ||
           !_strnicmp(t, "shadows=", 8) || !_strnicmp(t, "shadowres=", 10) ||
           !lstrcmpiA(t, "sun=off");
}

static void strip_classicpp_cfg(void)
{
    static const char* CFG = "tagpu_classicpp.cfg";
    char in[4096], out[4096];
    HANDLE h;
    DWORD n = 0, wrote = 0;
    int at = 0, dropped = 0;
    char* p;

    h = CreateFileA(CFG, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, in, sizeof in - 1, &n, 0)) n = 0;
    CloseHandle(h);
    in[n] = 0;
    if (n >= sizeof in - 1) {
        slog("migration: %s is larger than its buffer - left untouched", CFG);
        return;
    }
    for (p = in; *p; ) {
        char* q;
        while (*p && (unsigned char)*p <= ' ') p++;
        if (!*p) break;
        q = p;
        while (*q && (unsigned char)*q > ' ') q++;
        if (*q) *q++ = 0;
        if (menu_token(p)) dropped++;
        else {
            int k = _snprintf(out + at, sizeof out - at, "%s\r\n", p);
            if (k < 0 || k >= (int)sizeof out - at) {
                slog("migration: %s does not fit its buffer - left untouched", CFG);
                return;
            }
            at += k;
        }
        p = q;
    }
    if (!dropped) return;

    if (!CopyFileA(CFG, "tagpu_classicpp.cfg" MIGRATED, TRUE)) {
        slog("migration: could not back up %s - left untouched", CFG);
        return;
    }
    if (!at) {
        DeleteFileA(CFG);
        slog("migration: %s held only menu keys - removed (backup %s" MIGRATED ")", CFG, CFG);
        record("removed %s, which held only the menu's keys (backup %s" MIGRATED ")", CFG, CFG);
        return;
    }
    h = CreateFileA("tagpu_classicpp.cfg.tmp", GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!WriteFile(h, out, (DWORD)at, &wrote, 0) || wrote != (DWORD)at) {
        CloseHandle(h);
        DeleteFileA("tagpu_classicpp.cfg.tmp");
        return;
    }
    CloseHandle(h);
    if (MoveFileExA("tagpu_classicpp.cfg.tmp", CFG, MOVEFILE_REPLACE_EXISTING)) {
        slog("migration: %d menu key(s) stripped from %s, its knobs kept (backup %s" MIGRATED ")",
             dropped, CFG, CFG);
        record("stripped the menu's keys from %s (backup %s" MIGRATED ")", CFG, CFG);
    }
    else
        DeleteFileA("tagpu_classicpp.cfg.tmp");
}

static void migrate(void)
{
    static const char* const LEVERS[] = {
        "tagpu_classicpp.on", "tagpu_classicpp.off", "tagpu_ss.off", "tagpu_fps.on",
        "tagpu_hud.on", "tagpu_hud.off", "tagpu_vk.cfg",
    };
    int i;
    HANDLE h;
    DWORD wrote = 0;

    InterlockedIncrement(&s_dirty);     /* written by attach: every key, at its default */
    if (exists(RECORD)) {
        slog("no " STORE ", and " RECORD " says this directory was migrated - writing the defaults");
        return;
    }
    slog("no " STORE " - first run: moving the files an earlier menu wrote aside, "
         "and writing the defaults");
    record("# The first launch with impure.cfg moved these aside. Every setting");
    record("# starts at its default; rename a file back to undo its step.");
    for (i = 0; i < N(LEVERS); i++) rename_aside(LEVERS[i]);
    strip_classicpp_cfg();
    h = CreateFileA(RECORD, GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE || !WriteFile(h, s_rec, (DWORD)s_recLen, &wrote, 0))
        slog("could not write " RECORD " (error %lu) - a later launch with no " STORE
             " refuses any step whose backup exists", GetLastError());
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void tagpu_settings_attach(void)
{
    int i, first;
    char b[512];
    int at;

    if (s_attached) return;
    InitializeCriticalSection(&s_io);
    for (i = 0; i < TS_NKEYS; i++) s_val[i] = s_key[i].def;

    s_ignored = exists(MASTER_OFF);
    first = !exists(STORE);
    /* NEVER under tagpu_defaults.off. That file means a tacli control launch,
       and tacli creates the store before every launch -- so a missing store
       there is a tacli that did not, and renaming a measurement's levers aside
       would silently change what it measures. */
    if (first && !s_ignored) migrate();
    else if (!first) load();
    s_attached = 1;
    /* The defaults are written now; if that fails the flag stays set and the
       render thread retries, as for any click. */
    if (first && !s_ignored) flush(0);

    at = _snprintf(b, sizeof b, "%s%s:", STORE,
                   s_ignored ? " IGNORED but for display, vsync and window (" MASTER_OFF
                               " present: every other setting is the lever's or the compiled "
                               "default)" : "");
    for (i = 0; i < TS_NKEYS && at > 0 && at < (int)sizeof b; i++) {
        int k;
        if (i == TS_MONITOR)
            k = _snprintf(b + at, sizeof b - at, " monitor=%s", s_monStored[0] ? s_monStored : "-");
        else if (i == TS_RESOLUTION) {
            int v = read_val(TS_RESOLUTION);
            k = v ? _snprintf(b + at, sizeof b - at, " resolution=%dx%d", TS_RES_W(v), TS_RES_H(v))
                  : _snprintf(b + at, sizeof b - at, " resolution=native");
        } else
            k = _snprintf(b + at, sizeof b - at, " %s=%s", s_key[i].name,
                          spelling((TagpuSetting)i, read_val((TagpuSetting)i)));
        if (k < 0) break;
        at += k;
    }
    b[sizeof b - 1] = 0;
    slog("%s gpu=%s window=%s", b, s_gpu[0] ? s_gpu : "auto", s_winSet ? "set" : "-");
}

/* ---- the monitors, the GPU and the window -------------------------------- */

void tagpu_settings_monitors(const char* const* names, int n)
{
    int i;
    if (n > MON_MAX) n = MON_MAX;
    for (i = 0; i < n; i++) lstrcpynA(s_monName[i], names[i] ? names[i] : "", MON_LEN);
    s_monCount = n;
    /* the stored name, resolved here and nowhere else: a monitor that is no
       longer attached reads as "none chosen", and util_default_monitor
       answers -- the name is kept, so plugging it back in restores it */
    s_val[TS_MONITOR] = -1;
    if (s_monStored[0])
        for (i = 0; i < n; i++)
            if (!lstrcmpiA(s_monName[i], s_monStored)) { s_val[TS_MONITOR] = i; break; }
    if (s_monStored[0] && s_val[TS_MONITOR] < 0)
        slog("monitor=%s is not attached - none is chosen (util_target_monitor)", s_monStored);
}

const char* tagpu_settings_gpu(void)
{
    return s_ignored ? "" : s_gpu;
}

void tagpu_settings_set_gpu(const char* name)
{
    if (!s_attached || s_ignored || !name) return;
    EnterCriticalSection(&s_io);
    if (lstrcmpiA(s_gpu, name)) {
        lstrcpynA(s_gpu, name, sizeof s_gpu);
        InterlockedIncrement(&s_dirty);
    }
    LeaveCriticalSection(&s_io);
}

void tagpu_settings_restoreoff(char* out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!s_attached || s_ignored) return;
    EnterCriticalSection(&s_io);
    lstrcpynA(out, s_restoreOff, cap);
    LeaveCriticalSection(&s_io);
}

void tagpu_settings_set_restoreoff(const char* key)
{
    if (!s_attached || s_ignored || !key) return;
    EnterCriticalSection(&s_io);
    if (lstrcmpiA(s_restoreOff, key)) {
        lstrcpynA(s_restoreOff, key, sizeof s_restoreOff);
        InterlockedIncrement(&s_dirty);
    }
    LeaveCriticalSection(&s_io);
}

int tagpu_settings_window(int* x, int* y, int* w, int* h)
{
    if (!s_winSet) return 0;
    *x = s_win[0]; *y = s_win[1]; *w = s_win[2]; *h = s_win[3];
    return 1;
}

/* Records only: the write is the final flush's, once the render thread is
   joined (dd_Release) or at detach. */
void tagpu_settings_save_window(int x, int y, int w, int h)
{
    int f[4];
    f[0] = x; f[1] = y; f[2] = w; f[3] = h;
    if (!s_attached || s_ignored || !frame_ok(f)) return;
    /* at detach a held lock is an orphan (tagpu_settings_final) */
    if (s_detaching) {
        if (!TryEnterCriticalSection(&s_io)) return;
    } else {
        EnterCriticalSection(&s_io);
    }
    if (!s_winSet || memcmp(s_win, f, sizeof f)) {
        memcpy(s_win, f, sizeof f);
        s_winSet = 1;
        InterlockedIncrement(&s_dirty);
        slog("the windowed frame %d,%d %dx%d is kept for the next launch", x, y, w, h);
    }
    LeaveCriticalSection(&s_io);
}

/* ---- the two rows whose lever is a bare file ----------------------------- */

int tagpu_settings_ss(void)
{
    int v;
    if (exists("tagpu_ss.off")) return 1;
    return tagpu_settings_get(TS_SS, &v) ? v : 2;
}

int tagpu_settings_fps(void)
{
    int v;
    if (exists("tagpu_fps.on")) return 1;
    return tagpu_settings_get(TS_FPS, &v) ? v : 0;
}

int tagpu_settings_vsync(void)
{
    int v;
    return tagpu_settings_placement(TS_VSYNC, &v) ? v : s_key[TS_VSYNC].def;
}

