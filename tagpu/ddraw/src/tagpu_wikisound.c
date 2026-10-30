/* tagpu_wikisound.c -- see tagpu_wikisound.h */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_log.h"
#include "tagpu_menu.h"
#include "tagpu_wikisound.h"

#define VOLUME (-585)           /* the engine's own per play, 0x47FED0 and 0x47F17A */

typedef HRESULT (WINAPI* create_fn)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);

/* THE OBJECT, made whole by tagpu_wikisound_make and then read-only: the clips it was made for and
   each clip's voices, voice 0 its buffer and the rest duplicates sharing its samples; voice 0 NULL
   for a clip DirectSound refused, a later voice NULL for a duplicate it refused. Never freed. */
typedef struct {
    IDirectSound*       ds;
    const WIKI3DSOUND*  snd;
    int                 nclip;          /* min(snd->nclip, WIKI3D_CLIPS) */
    IDirectSoundBuffer* voice[WIKI3D_CLIPS][WIKISOUND_VOICES];
} DEVICE;

/* ORDERING: s_make goes 0 -> 1 by one compare-exchange, so one make runs per process; the make
   writes the DEVICE whole and then publishes it by one interlocked store, a full barrier, and the
   game thread reads it by an interlocked read, so it sees every byte of it. s_make stays 1 after a
   make, success or failure: a failed create is not tried again. */
static volatile LONG  s_make;
static PVOID volatile s_dev;

/* THE GAME THREAD'S OWN: the tally, and the slowest DirectSound call since the last tally, in
   microseconds, and which */
static int           s_played, s_failed, s_silent;
static LARGE_INTEGER s_qpf;
static double        s_slowUs;
static const char*   s_slowWhat = "none";

static LARGE_INTEGER now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t;
}

/* the call `what`, started at `t0`, as the slowest when it is */
static void took(LARGE_INTEGER t0, const char* what)
{
    const LARGE_INTEGER t1 = now();
    double us;
    if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
    us = s_qpf.QuadPart > 0 ? (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)s_qpf.QuadPart : 0;
    if (us > s_slowUs) {
        s_slowUs = us;
        s_slowWhat = what;
    }
}

static const DEVICE* device(void)
{
    return (const DEVICE*)InterlockedCompareExchangePointer((PVOID*)&s_dev, NULL, NULL);
}

/* the clip's samples into its buffer, whole */
static int fill(IDirectSoundBuffer* b, const WIKI3DCLIP* c)
{
    void *p1, *p2;
    DWORD n1, n2;
    if (FAILED(IDirectSoundBuffer_Lock(b, 0, c->bytes, &p1, &n1, &p2, &n2, 0))) return 0;
    memcpy(p1, c->data, n1 < c->bytes ? n1 : c->bytes);
    if (p2 && n1 < c->bytes) memcpy(p2, c->data + n1, n2 < c->bytes - n1 ? n2 : c->bytes - n1);
    IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
    return 1;
}

static IDirectSoundBuffer* make_buffer(IDirectSound* ds, const WIKI3DCLIP* c)
{
    WAVEFORMATEX wf;
    DSBUFFERDESC d;
    IDirectSoundBuffer* b = NULL;
    HRESULT hr;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)c->channels;
    wf.nSamplesPerSec = (DWORD)c->rate;
    wf.wBitsPerSample = (WORD)c->bits;
    wf.nBlockAlign = (WORD)c->align;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    d.dwFlags = DSBCAPS_STATIC | DSBCAPS_CTRLVOLUME;     /* no DSBCAPS_GLOBALFOCUS: see the header */
    d.dwBufferBytes = c->bytes;
    d.lpwfxFormat = &wf;
    hr = IDirectSound_CreateSoundBuffer(ds, &d, &b, NULL);
    if (FAILED(hr) || !fill(b, c)) {
        if (b) IDirectSoundBuffer_Release(b);
        tagpu_logf("wiki sound: %s is silent - DirectSound refused its buffer (hr 0x%08lX)", c->name, (unsigned long)hr);
        return NULL;
    }
    return b;
}

void tagpu_wikisound_make(const WIKI3DSOUND* s, uintptr_t hwnd)
{
    HMODULE m;
    create_fn create;
    HRESULT hr;
    DEVICE* d;
    int i, made = 0, dups = 0;
    if (!s || InterlockedCompareExchange(&s_make, 1, 0) != 0) return;
    /* the exe imports DirectSoundCreate from DSOUND.dll (IAT 0x4FC044), so the module is loaded */
    m = GetModuleHandleA("dsound.dll");
    create = m ? (create_fn)(void*)GetProcAddress(m, "DirectSoundCreate") : NULL;
    if (!create || !hwnd || !(d = (DEVICE*)calloc(1, sizeof *d))) {
        tagpu_logf("wiki sound: NO DirectSound - %s; the wiki is silent for the process",
                   !create ? "dsound.dll's DirectSoundCreate is not loaded" : !hwnd ? "the game had no window" : "no memory");
        return;
    }
    hr = create(NULL, &d->ds, NULL);
    if (SUCCEEDED(hr)) {
        const HRESULT co = IDirectSound_SetCooperativeLevel(d->ds, (HWND)hwnd, DSSCL_PRIORITY);
        if (FAILED(co)) {
            IDirectSound_Release(d->ds);
            d->ds = NULL;
            hr = co;
        }
    }
    if (!d->ds) {
        free(d);
        tagpu_logf("wiki sound: NO DirectSound - its own object did not start (hr 0x%08lX); the wiki is silent for the "
                   "process, the engine's object is not used", (unsigned long)hr);
        return;
    }
    d->snd = s;
    d->nclip = s->nclip < WIKI3D_CLIPS ? s->nclip : WIKI3D_CLIPS;
    /* every voice here, so the game thread duplicates nothing. MEASURED under Wine on 2026-10-28,
       the demos' check with sound playing: with duplicates made at a first overlapping play the
       page's worst frame was 8.1 ms; with them made here it is 1.1 ms over 1291 frames, and the
       slowest DirectSound call the game thread makes 2 us [INFERRED: the duplicate was the cost,
       the one call moved; the first run timed no call] */
    for (i = 0; i < d->nclip; i++) {
        int k;
        if (!s->clip[i].data || !(d->voice[i][0] = make_buffer(d->ds, &s->clip[i]))) continue;
        made++;
        for (k = 1; k < WIKISOUND_VOICES; k++)
            if (FAILED(IDirectSound_DuplicateSoundBuffer(d->ds, d->voice[i][0], &d->voice[i][k]))) d->voice[i][k] = NULL;
            else dups++;
    }
    tagpu_logf("wiki sound: DirectSound created (hr 0x%08lX), PRIORITY on the game's window, off the game thread; "
               "%d buffer(s) for %d WAV(s), %d duplicate voice(s)", (unsigned long)hr, made, d->nclip, dups);
    InterlockedExchangePointer((PVOID*)&s_dev, d);
}

typedef struct {
    const WIKI3DSOUND* s;
    uintptr_t          hwnd;
} ARM;

static DWORD WINAPI maker(LPVOID p)
{
    ARM a = *(const ARM*)p;
    free(p);
    tagpu_wikisound_make(a.s, a.hwnd);
    return 0;
}

void tagpu_wikisound_arm(const WIKI3DSOUND* s, uintptr_t hwnd)
{
    ARM* a;
    HANDLE t;
    if (!s || s_make || tagpu_wikisound_muted() || !(a = (ARM*)malloc(sizeof *a))) return;
    a->s = s;
    a->hwnd = hwnd;
    if (!(t = CreateThread(NULL, 0, maker, a, 0, NULL))) {
        free(a);
        return;
    }
    CloseHandle(t);
}

int tagpu_wikisound_muted(void)
{
    TAGPU_SOUNDSET set;
    return !tagpu_menu_sound(&set) || set.win_sound || set.no_dsound || set.fxvol == 0 || set.mode == 0;
}

/* 0x4CF570's choice among clip `c`'s voices (the header); NULL plays nothing. The voices are all
   made with the object, so where the engine duplicates a copy at a busy set's first overlap this
   finds it made; a voice the object's make could not duplicate is absent, and reaching it with
   every voice before it busy plays nothing, as the engine's failed duplicate does (0x4CF66D). */
static IDirectSoundBuffer* voice(const DEVICE* d, int c)
{
    DWORD best = 0, st, play, write;
    LARGE_INTEGER t0;
    HRESULT hr;
    int i, furthest = -1;
    for (i = 0; i < WIKISOUND_VOICES; i++) {
        IDirectSoundBuffer* v = d->voice[c][i];
        if (!v) return NULL;
        t0 = now();
        hr = IDirectSoundBuffer_GetStatus(v, &st);
        took(t0, "GetStatus");
        if (FAILED(hr)) return NULL;
        if (st == 0) return v;
        t0 = now();
        if (FAILED(IDirectSoundBuffer_GetCurrentPosition(v, &play, &write))) play = 0;
        took(t0, "GetCurrentPosition");
        if (furthest < 0 || play > best) {
            best = play;
            furthest = i;
        }
    }
    if (furthest < 0) return NULL;
    t0 = now();
    IDirectSoundBuffer_Stop(d->voice[c][furthest]);
    took(t0, "Stop");
    return d->voice[c][furthest];
}

/* clip `c` of `s` from its start. BOUND: `c` indexes the device's clips only below their count,
   and only the clips of the WIKI3DSOUND the device was made for */
static int play(const WIKI3DSOUND* s, int c)
{
    const DEVICE* d = device();
    IDirectSoundBuffer* b;
    LARGE_INTEGER t0;
    HRESULT hr;
    if (!d || d->snd != s || c < 0 || c >= d->nclip || !d->voice[c][0]) {
        s_silent++;
        return 0;
    }
    if (!(b = voice(d, c))) {
        s_failed++;
        return 0;
    }
    t0 = now();
    IDirectSoundBuffer_SetCurrentPosition(b, 0);
    took(t0, "SetCurrentPosition");
    t0 = now();
    IDirectSoundBuffer_SetVolume(b, VOLUME);
    took(t0, "SetVolume");
    t0 = now();
    hr = IDirectSoundBuffer_Play(b, 0, 0, 0);
    took(t0, "Play");
    if (FAILED(hr)) {
        s_failed++;
        return 0;
    }
    s_played++;
    return 1;
}

int tagpu_wikisound_reply(const WIKI3DSOUND* s, int cat, int kind, int variant, int priority)
{
    TAGPU_SOUNDSET set;
    int i;
    if (!s || tagpu_wikisound_muted() || !tagpu_menu_sound(&set) || !set.speech || priority <= 10 - set.unitchat ||
        kind <= 0 || kind >= WIKI_SND_KINDS) {
        s_silent++;
        return 0;
    }
    for (i = 0; i < s->ncat; i++)
        if (s->cat[i].index == cat) {
            const WIKI3DSNDCAT* g = &s->cat[i];
            if (variant < 0 || variant >= g->n[kind]) break;
            return play(s, s->var[g->first[kind] + variant]);
        }
    s_silent++;
    return 0;
}

int tagpu_wikisound_named(const WIKI3DSOUND* s, int name)
{
    if (!s || tagpu_wikisound_muted() || name < 0 || name >= WIKI_NSNDNAME) {
        s_silent++;
        return 0;
    }
    return play(s, s->named[name]);
}

void tagpu_wikisound_tally(int* played, int* failed, int* silent, double* slow_us, const char** slow_what)
{
    *played = s_played;
    *failed = s_failed;
    *silent = s_silent;
    *slow_us = s_slowUs;
    *slow_what = s_slowWhat;
    s_played = s_failed = s_silent = 0;
    s_slowUs = 0;
    s_slowWhat = "none";
}

void tagpu_wikisound_stop(void)
{
    const DEVICE* d = device();
    int i, k;
    if (!d) return;
    for (i = 0; i < d->nclip; i++)
        for (k = 0; k < WIKISOUND_VOICES; k++)
            if (d->voice[i][k]) IDirectSoundBuffer_Stop(d->voice[i][k]);
}
