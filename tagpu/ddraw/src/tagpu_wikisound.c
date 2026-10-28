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
   one buffer a clip, NULL for one DirectSound refused. Never freed. */
typedef struct {
    IDirectSound*       ds;
    const WIKI3DSOUND*  snd;
    int                 nclip;          /* min(snd->nclip, WIKI3D_CLIPS) */
    IDirectSoundBuffer* first[WIKI3D_CLIPS];
} DEVICE;

/* ORDERING: s_make goes 0 -> 1 by one compare-exchange, so one make runs per process; the make
   writes the DEVICE whole and then publishes it by one interlocked store, a full barrier, and the
   game thread reads it by an interlocked read, so it sees every byte of it. s_make stays 1 after a
   make, success or failure: a failed create is not tried again. */
static volatile LONG  s_make;
static PVOID volatile s_dev;

/* THE GAME THREAD'S OWN: each clip's duplicates (voice 1..), kept with the object */
static IDirectSoundBuffer* s_dup[WIKI3D_CLIPS][WIKISOUND_VOICES - 1];
static int                 s_played, s_failed, s_silent;

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
    int i, made = 0;
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
    for (i = 0; i < d->nclip; i++)
        if (s->clip[i].data && (d->first[i] = make_buffer(d->ds, &s->clip[i])) != NULL) made++;
    tagpu_logf("wiki sound: DirectSound created (hr 0x%08lX), PRIORITY on the game's window, off the game thread; "
               "%d buffer(s) for %d WAV(s)", (unsigned long)hr, made, d->nclip);
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

/* 0x4CF570's choice among clip `c`'s voices (the header); NULL plays nothing */
static IDirectSoundBuffer* voice(const DEVICE* d, int c)
{
    IDirectSoundBuffer* v[WIKISOUND_VOICES];
    DWORD best = 0, st, play, write;
    int i, furthest = -1, empty = -1;
    v[0] = d->first[c];
    for (i = 1; i < WIKISOUND_VOICES; i++) v[i] = s_dup[c][i - 1];
    for (i = 0; i < WIKISOUND_VOICES; i++) {
        if (!v[i]) {
            empty = i;
            continue;
        }
        if (FAILED(IDirectSoundBuffer_GetStatus(v[i], &st))) return NULL;
        if (st == 0) return v[i];
        if (FAILED(IDirectSoundBuffer_GetCurrentPosition(v[i], &play, &write))) play = 0;
        if (furthest < 0 || play > best) {
            best = play;
            furthest = i;
        }
    }
    if (empty > 0) {
        IDirectSoundBuffer* dup = NULL;
        if (FAILED(IDirectSound_DuplicateSoundBuffer(d->ds, v[0], &dup))) return NULL;
        s_dup[c][empty - 1] = dup;
        return dup;
    }
    if (furthest < 0) return NULL;
    IDirectSoundBuffer_Stop(v[furthest]);
    return v[furthest];
}

/* clip `c` of `s` from its start. BOUND: `c` indexes the device's clips only below their count,
   and only the clips of the WIKI3DSOUND the device was made for */
static int play(const WIKI3DSOUND* s, int c)
{
    const DEVICE* d = device();
    IDirectSoundBuffer* b;
    if (!d || d->snd != s || c < 0 || c >= d->nclip || !d->first[c]) {
        s_silent++;
        return 0;
    }
    if (!(b = voice(d, c))) {
        s_failed++;
        return 0;
    }
    IDirectSoundBuffer_SetCurrentPosition(b, 0);
    IDirectSoundBuffer_SetVolume(b, VOLUME);
    if (FAILED(IDirectSoundBuffer_Play(b, 0, 0, 0))) {
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

void tagpu_wikisound_tally(int* played, int* failed, int* silent)
{
    *played = s_played;
    *failed = s_failed;
    *silent = s_silent;
    s_played = s_failed = s_silent = 0;
}

void tagpu_wikisound_stop(void)
{
    const DEVICE* d = device();
    int i, k;
    if (!d) return;
    for (i = 0; i < d->nclip; i++) {
        if (d->first[i]) IDirectSoundBuffer_Stop(d->first[i]);
        for (k = 0; k < WIKISOUND_VOICES - 1; k++)
            if (s_dup[i][k]) IDirectSoundBuffer_Stop(s_dup[i][k]);
    }
}
