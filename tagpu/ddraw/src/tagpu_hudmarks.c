/* tagpu_hudmarks.c -- the replay HUD's timeline markers and the stats window's samples.
   Contract: tagpu_hudmarks.h.

   GAME THREAD, ALL OF IT BUT THE WALKER, which reads the recording file and nothing of the
   engine's (THE WALKER). The game-thread entry points are the packet's fill and the teardown
   wrap's entry. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_hudmarks.h"
#include "tagpu_replay.h"
#include "tagpu_replays.h"
#include "tagpu_recreader.h"
#include "tagpu_recfmt.h"
#include "tagpu_log.h"
#include "tagpu_packet.h"

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
static unsigned        s_walkTl;        /* the timeline walked: the one the playback plays        */
/* the bookmarks: index.dat keys a recording by its name in replays\ and its size */
static unsigned        s_bm[BM_MAX];
static int             s_nbm;
static char            s_file[MAX_PATH];
static unsigned long long s_size;
static int             s_keyOk;
/* THE STATS: each recorded player's measures every s_period GameTime from s_base, the walk's first
   iteration, read off the same iterations as the markers (THE WALKER) */
enum { ST_METAL, ST_ENERGY, ST_EXCESS, ST_UNITS, ST_ARMY, ST_KILLS, ST_LOSSES, ST_N };
static const char* const ST_KEY[ST_N] = { "metal", "energy", "excess", "units", "army", "kills", "losses" };
static float           s_sv[TAGPU_PK_HUD_POINTS][REC_PLAYERS][ST_N];
static volatile LONG   s_svN;
static unsigned        s_period, s_base;
static float*          s_army;          /* [s_narmy] the army value of a type, by its index      */
static unsigned        s_narmy;

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
   LIFETIME: the worker owns its reader and closes it before it returns; walk_stop stops it and
   waits for it on the game thread -- at the teardown's entry, and when the playback moves to
   another timeline -- before the count goes back to 0. The wait is bounded by the file and the
   format, not by a timeout: before its first look at the stop flag the worker opens the reader,
   which reads every block header (recr_open's segment scan), and between two looks it reads one
   iteration -- at most one block unpacked and decoded (RECR_BLOCK_MAX) -- or crosses one fork,
   a seek that scans the headers again and rebuilds from the last snapshot before the fork
   (SNAPSHOT_TICKS apart when written) plus the child's first iteration.
   THE TIMELINE WALKED IS THE ONE THAT PLAYS (tagpu_replay_timeline): the reader is moved onto its
   chain before the first iteration, as the playback's own is, and crosses each fork on it by a
   seek, one segment's first iteration read whole.
   tagpu_recreader.c has no mutable statics and reads no engine memory, so the worker shares
   nothing with the playback.

   THE STATS are sampled on the same iterations: every s_period GameTime from the first, each
   recorded player's metal and energy income (+0x9C, +0x90, the floats its row shows), the metal
   wasted (the double +0xD4, MetalWasted), the units alive (+0x144), kills and losses (the words
   +0xFC, +0xFE), and the army value: the sum, over the player's live units (the slot live and
   +0x110 bit 28) that are finished (the build left, the float +0x104, 0.0 by the engine's test,
   build_finished in tagpu_replay.c), of their type's value (the word +0xA6, below s_narmy). The
   recorder excludes none of these (replay-layout.tsv). s_period fits the playing's span in
   TAGPU_PK_HUD_POINTS samples. They are published as the markers are: s_base is written before
   the first count, a sample whole before the count that covers it, and none is rewritten.
   THE TYPES' VALUES are the game thread's copy of the engine's (tagpu_replay_army_values), made
   before CreateThread and freed by walk_stop after the join: the worker never reads engine memory
   and never outlives the table. A type index at or past the table's count is data the recording
   holds and is worth nothing. */
_Static_assert(0x144 + 2 <= REC_PLAYER_BYTES, "the walker's player fields are in the player image");
_Static_assert(0x110 + 4 <= REC_UNIT_DWORDS * 4, "the walker's unit fields are in the unit image");

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

static void sample(const RecWorld* W, const RecHeader* h, unsigned n)
{
    float army[REC_PLAYERS];
    unsigned s, slot;
    memset(army, 0, sizeof army);
    for (slot = 1; slot < W->slots; slot++) {
        const unsigned char* u = (const unsigned char*)(W->unit + (size_t)slot * REC_UNIT_DWORDS);
        uint32_t state;
        float left;
        unsigned owner = u[0xFF], type = u16_at((const uint32_t*)u, 0xA6);
        if (!W->live[slot]) continue;
        memcpy(&state, u + 0x110, 4);
        memcpy(&left, u + 0x104, 4);
        if (!(state & 0x10000000u) || left < 0.0f || left > 0.0f || owner >= REC_PLAYERS || type >= s_narmy)
            continue;
        army[owner] += s_army[type];
    }
    for (s = 0; s < h->nseats && s < REC_PLAYERS; s++) {
        unsigned p = h->seats[s].index;
        const unsigned char* pl;
        float* o;
        double wasted;
        if (p >= REC_PLAYERS) continue;
        pl = (const unsigned char*)W->player[p];
        o = s_sv[n][p];
        memcpy(&o[ST_METAL], pl + 0x9C, 4);
        memcpy(&o[ST_ENERGY], pl + 0x90, 4);
        memcpy(&wasted, pl + 0xD4, 8);
        o[ST_EXCESS] = (float)wasted;
        o[ST_UNITS] = (float)u16_at(W->player[p], 0x144);
        o[ST_ARMY] = army[p];
        o[ST_KILLS] = (float)u16_at(W->player[p], 0xFC);
        o[ST_LOSSES] = (float)u16_at(W->player[p], 0xFE);
    }
}

static DWORD WINAPI walker(void* arg)
{
    char why[200] = "";
    RecReader* r;
    const RecHeader* h;
    unsigned cmdr[REC_PLAYERS], alive[REC_PLAYERS], deaths = 0, defeats = 0, lost = 0, iters = 0;
    unsigned s, nsv = 0;
    int seeded = 0, k = 0;
    char dl[200] = "", fl[200] = "", capn[64] = "";
    DWORD t0 = GetTickCount();
    const char* stop = "the recording's end";
    (void)arg;

    r = recr_open(s_walkPath, why, sizeof why);
    if (!r || recr_timeline(r, s_walkTl, why, sizeof why) != 1) {
        tagpu_logf("hud: markers: none - %s", why);
        if (r) recr_close(r);
        return 0;
    }
    h = recr_header(r);
    for (;;) {
        RecWorld* W;
        if (InterlockedCompareExchange(&s_walkStop, 0, 0)) { stop = "stopped: the level ended or the playback moved to another timeline"; break; }
        k = recr_tick(r, why, sizeof why);
        /* a fork's chain goes on in its next timeline, which only a seek reads: to the iteration
           after the world's, the child's first (recr_tick, THE BOUNDARY) */
        if (k == RECR_BOUNDARY && recr_seek(r, recr_world(r)->tick + 1, 0, NULL, NULL, why, sizeof why) != 1)
            k = -1;
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
        /* the samples due by this iteration's GameTime */
        if (!nsv) s_base = W->tick;
        while (nsv < TAGPU_PK_HUD_POINTS &&
               (unsigned long long)W->tick >= (unsigned long long)s_base + (unsigned long long)nsv * s_period) {
            sample(W, h, nsv);
            InterlockedExchange(&s_svN, (LONG)++nsv);
        }
    }
    recr_close(r);
    if (lost) _snprintf(capn, sizeof capn, ", %u past the %d the timeline holds", lost, MK_MAX);
    tagpu_logf("hud: markers: %u commander deaths%s, %u defeats%s, %u iterations walked in %lu ms "
               "(%s)%s; %u stats samples %u GameTime apart, %u unit types valued", deaths, dl, defeats, fl,
               iters, (unsigned long)(GetTickCount() - t0), stop, capn, nsv, s_period, s_narmy);
    return 0;
}

/* The playing's markers and bookmarks, on the first fill that sees its first step played: once per
   level, and again when a rewind moves the playback to another timeline. */
static void walk_start(void)
{
    const char* path = tagpu_replay_path();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    unsigned k;
    s_walkStarted = 1;
    s_walkTl = tagpu_replay_timeline();
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
    InterlockedExchange(&s_svN, 0);
    /* the period: a whole number of seconds that fits the span in the samples */
    {
        unsigned first, last, span = 0, secs;
        if (tagpu_replay_span(&first, &last) && last > first) span = last - first;
        secs = (span + 30u * (TAGPU_PK_HUD_POINTS - 1) - 1) / (30u * (TAGPU_PK_HUD_POINTS - 1));
        s_period = 30u * (secs ? secs : 1);
    }
    s_narmy = 0;
    if ((k = tagpu_replay_army_values(NULL, 0)) && (s_army = (float*)malloc((size_t)k * sizeof(float))))
        s_narmy = tagpu_replay_army_values(s_army, k);
    s_walkH = CreateThread(NULL, 0, walker, NULL, 0, NULL);
    if (!s_walkH) tagpu_logf("hud: markers: none - the walker did not start (%lu)", GetLastError());
}

static void walk_stop(void)
{
    if (s_walkH) {
        InterlockedExchange(&s_walkStop, 1);
        WaitForSingleObject(s_walkH, INFINITE);
        CloseHandle(s_walkH);
        s_walkH = NULL;
    }
    InterlockedExchange(&s_mkN, 0);
    InterlockedExchange(&s_svN, 0);
    /* the worker has returned, or never started: nothing reads the table */
    free(s_army);
    s_army = NULL;
    s_narmy = 0;
    s_walkStarted = 0;
}

int tagpu_hudmarks_get(unsigned* T, unsigned char* kind, unsigned char* seat, int cap)
{
    LONG n;
    int i, k = 0;
    if (s_walkStarted && tagpu_replay_timeline() != s_walkTl) walk_stop();
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

int tagpu_hudmarks_stats(const char* key, const unsigned char* seat, int nseat, float* v, int cap,
                         unsigned* period, unsigned* base)
{
    LONG n;
    int m, i, k;
    for (m = 0; m < ST_N && strcmp(key, ST_KEY[m]); m++) {}
    *period = s_period;
    /* the walker writes the base and then a sample before it publishes the count: the base read
       after a count is that walk's, and the samples below it are whole */
    n = InterlockedCompareExchange(&s_svN, 0, 0);
    *base = n ? s_base : 0;
    if (m == ST_N) return 0;
    if (n > cap) n = cap;
    for (i = 0; i < nseat; i++)
        for (k = 0; k < n; k++) v[(size_t)i * (size_t)cap + (size_t)k] = seat[i] < REC_PLAYERS ? s_sv[k][seat[i]][m] : 0.0f;
    return (int)n;
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
    walk_stop();
    s_nbm = 0;
    s_keyOk = 0;
}
