/* tagpu_hudmarks.c -- the replay HUD's timeline markers. Contract: tagpu_hudmarks.h.

   GAME THREAD, ALL OF IT BUT THE WALKER, which reads the recording file and nothing of the
   engine's (THE WALKER). The game-thread entry points are the packet's fill and the teardown
   wrap's entry. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_hudmarks.h"
#include "tagpu_replay.h"
#include "tagpu_replays.h"
#include "tagpu_recreader.h"
#include "tagpu_recfmt.h"
#include "tagpu_log.h"

#define MK_MAX  256
#define BM_MAX  64                      /* index.dat's MARKS_MAX                                  */

enum { MK_DEATH, MK_DEFEAT };
typedef struct { unsigned T; unsigned char kind, seat; } Marker;
static Marker          s_mk[MK_MAX];
static volatile LONG   s_mkN;
static volatile LONG   s_walkStop;
static HANDLE          s_walkH;
static int             s_walkStarted;
static char            s_walkPath[MAX_PATH];
/* the bookmarks: index.dat keys a recording by its name in replays\ and its size */
static unsigned        s_bm[BM_MAX];
static int             s_nbm;
static char            s_file[MAX_PATH];
static unsigned long long s_size;
static int             s_keyOk;

/* ================================================================================================
   The walker
   ================================================================================================
   THE MARKERS, read off the recording by a worker with its own reader: a commander death is a
   player's +0x106 rising (the destructor increments it when the dying unit is its side's commander,
   0x486990..0x486996 and the self-destruct path 0x486A86..0x486A8C), a defeat its +0x144, the units
   alive, reaching 0 from above (decremented at 0x486DFC..0x486E09). Both are in every recorded
   player image: replay-layout.tsv excludes neither.
   ORDERING: an entry is written whole before InterlockedExchange publishes the count that covers
   it, and entries are never rewritten, so the game thread reads entries below the count it read.
   LIFETIME: the worker owns its reader and closes it before it returns; tagpu_hudmarks_level_end
   stops it and waits for it on the game thread, at the teardown's entry, before the count goes
   back to 0. Between two looks at the stop flag the worker reads one iteration -- at most one block
   unpacked and decoded, a bound the format sets (RECR_BLOCK_MAX) -- not a timeout.
   tagpu_recreader.c has no mutable statics and reads no engine memory, so the worker shares
   nothing with the playback. */
_Static_assert(0x144 + 2 <= REC_PLAYER_BYTES, "the walker's two fields are in the player image");

/* the log line's list: the first few markers of a kind, "T (seat p)" each */
static void note(char* b, unsigned cap, unsigned n, unsigned T, unsigned seat)
{
    size_t k = strlen(b);
    if (n > 6 || k + 24 >= cap) return;
    _snprintf(b + k, cap - k, n == 6 ? ", ..." : "%s%u (seat %u)", n ? ", " : " at ", T, seat);
    b[cap - 1] = 0;
}

static void publish(unsigned T, int kind, int seat, unsigned* lost)
{
    LONG n = InterlockedCompareExchange(&s_mkN, 0, 0);
    if (n >= MK_MAX) { (*lost)++; return; }
    s_mk[n].T = T;
    s_mk[n].kind = (unsigned char)kind;
    s_mk[n].seat = (unsigned char)seat;
    InterlockedExchange(&s_mkN, n + 1);
}

static unsigned u16_at(const uint32_t* img, unsigned off)
{
    unsigned short v;
    memcpy(&v, (const unsigned char*)img + off, 2);
    return v;
}

static DWORD WINAPI walker(void* arg)
{
    char why[200] = "";
    RecReader* r;
    const RecHeader* h;
    unsigned cmdr[REC_PLAYERS], alive[REC_PLAYERS], deaths = 0, defeats = 0, lost = 0, iters = 0;
    unsigned s;
    int seeded = 0, k = 0;
    char dl[200] = "", fl[200] = "", capn[64] = "";
    DWORD t0 = GetTickCount();
    const char* stop = "the recording's end";
    (void)arg;

    r = recr_open(s_walkPath, why, sizeof why);
    if (!r) {
        tagpu_logf("hud: markers: none - %s", why);
        return 0;
    }
    h = recr_header(r);
    for (;;) {
        RecWorld* W;
        if (InterlockedCompareExchange(&s_walkStop, 0, 0)) { stop = "stopped at the level's end"; break; }
        k = recr_tick(r, why, sizeof why);
        if (k <= 0) { if (k < 0) stop = why; break; }
        W = recr_world(r);
        iters++;
        for (s = 0; s < h->nseats && s < REC_PLAYERS; s++) {
            unsigned p = h->seats[s].index, c, a;
            if (p >= REC_PLAYERS) continue;
            c = u16_at(W->player[p], 0x106);
            a = u16_at(W->player[p], 0x144);
            if (seeded && c > cmdr[p]) {
                publish(W->tick, MK_DEATH, (int)p, &lost);
                note(dl, sizeof dl, deaths++, W->tick, p);
            }
            if (seeded && alive[p] > 0 && a == 0) {
                publish(W->tick, MK_DEFEAT, (int)p, &lost);
                note(fl, sizeof fl, defeats++, W->tick, p);
            }
            cmdr[p] = c;
            alive[p] = a;
        }
        seeded = 1;
    }
    recr_close(r);
    if (lost) _snprintf(capn, sizeof capn, ", %u past the %d the timeline holds", lost, MK_MAX);
    tagpu_logf("hud: markers: %u commander deaths%s, %u defeats%s, %u iterations walked in %lu ms "
               "(%s)%s", deaths, dl, defeats, fl, iters, (unsigned long)(GetTickCount() - t0), stop,
               capn);
    return 0;
}

/* The level's markers and bookmarks, once per level, on the first fill that sees the playback's
   first step played. */
static void walk_start(void)
{
    const char* path = tagpu_replay_path();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    s_walkStarted = 1;
    if (!path) return;
    lstrcpynA(s_walkPath, path, sizeof s_walkPath);
    s_keyOk = 0;
    s_nbm = 0;
    if (!_strnicmp(path, "replays\\", 8) && GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        lstrcpynA(s_file, path + 8, sizeof s_file);
        s_size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        s_keyOk = 1;
        s_nbm = tagpu_replays_marks_get(s_file, s_size, s_bm, BM_MAX);
    }
    InterlockedExchange(&s_walkStop, 0);
    InterlockedExchange(&s_mkN, 0);
    s_walkH = CreateThread(NULL, 0, walker, NULL, 0, NULL);
    if (!s_walkH) tagpu_logf("hud: markers: none - the walker did not start (%lu)", GetLastError());
}

int tagpu_hudmarks_get(unsigned* T, unsigned char* kind, unsigned char* seat, int cap)
{
    LONG n;
    int i, k = 0;
    if (!s_walkStarted && tagpu_replay_live()) walk_start();
    /* the walker writes an entry, then publishes the count: the entries below it are whole */
    n = InterlockedCompareExchange(&s_mkN, 0, 0);
    for (i = 0; i < n && i < MK_MAX && k < cap; i++, k++) {
        T[k] = s_mk[i].T;
        kind[k] = s_mk[i].kind;
        seat[k] = s_mk[i].seat;
    }
    for (i = 0; i < s_nbm && k < cap; i++, k++) {
        T[k] = s_bm[i];
        kind[k] = 2;
        seat[k] = 0xFF;
    }
    return k;
}

/* ================================================================================================
   The bookmarks
   ================================================================================================ */

int tagpu_hudmarks_bookmark(unsigned now, unsigned within, unsigned* at)
{
    int i, best = -1;
    unsigned d, bd = 0;
    *at = now;
    if (!s_keyOk) return -1;
    for (i = 0; i < s_nbm; i++) {
        d = s_bm[i] > now ? s_bm[i] - now : now - s_bm[i];
        if (d < within && (best < 0 || d < bd)) { best = i; bd = d; }
    }
    if (best >= 0) {
        *at = s_bm[best];
        if (!tagpu_replays_mark_remove(s_file, s_size, s_bm[best])) return -1;
        memmove(&s_bm[best], &s_bm[best + 1], (size_t)(s_nbm - best - 1) * sizeof s_bm[0]);
        s_nbm--;
        return 0;
    }
    if (s_nbm >= BM_MAX || !tagpu_replays_mark_add(s_file, s_size, now)) return -1;
    for (i = s_nbm; i > 0 && s_bm[i - 1] > now; i--) s_bm[i] = s_bm[i - 1];
    s_bm[i] = now;
    s_nbm++;
    return 1;
}

void tagpu_hudmarks_level_end(void)
{
    if (s_walkH) {
        InterlockedExchange(&s_walkStop, 1);
        WaitForSingleObject(s_walkH, INFINITE);
        CloseHandle(s_walkH);
        s_walkH = NULL;
    }
    InterlockedExchange(&s_mkN, 0);
    s_walkStarted = 0;
    s_nbm = 0;
    s_keyOk = 0;
}
