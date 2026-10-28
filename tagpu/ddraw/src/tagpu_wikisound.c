/* tagpu_wikisound.c -- see tagpu_wikisound.h */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <string.h>
#include "dd.h"
#include "tagpu_log.h"
#include "tagpu_menu.h"
#include "tagpu_wikisound.h"

#define VOLUME (-585)           /* the engine's own per play, 0x47FED0 and 0x47F17A */

typedef HRESULT (WINAPI* create_fn)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);

/* a clip's voices: buffer 0 made from its samples, the rest duplicates of it */
typedef struct {
    const WIKI3DCLIP*   clip;
    IDirectSoundBuffer* voice[WIKISOUND_VOICES];
    int                 nvoice;
} SET;

static IDirectSound* s_ds;
static int           s_dead;            /* the create failed: silent for the process */
static int           s_saidHr;          /* the create's result has been logged       */
static SET           s_set[WIKI3D_CLIPS];
static int           s_played, s_failed, s_silent;

/* the object, made at the first sound after an open; NULL while it cannot be */
static IDirectSound* device(void)
{
    HMODULE m;
    create_fn create;
    HRESULT hr;
    if (s_ds || s_dead) return s_ds;
    /* the exe imports DirectSoundCreate from DSOUND.dll (IAT 0x4FC044), so the module is loaded */
    m = GetModuleHandleA("dsound.dll");
    create = m ? (create_fn)(void*)GetProcAddress(m, "DirectSoundCreate") : NULL;
    if (!create || !g_ddraw.hwnd) {
        s_dead = 1;
        tagpu_logf("wiki sound: NO DirectSound - %s; the wiki is silent", !create ? "dsound.dll's DirectSoundCreate is not loaded"
                                                                                  : "the game has no window");
        return NULL;
    }
    hr = create(NULL, &s_ds, NULL);
    if (SUCCEEDED(hr)) {
        const HRESULT co = IDirectSound_SetCooperativeLevel(s_ds, g_ddraw.hwnd, DSSCL_PRIORITY);
        if (FAILED(co)) {
            IDirectSound_Release(s_ds);
            s_ds = NULL;
            hr = co;
        }
    }
    if (!s_ds) {
        s_dead = 1;
        tagpu_logf("wiki sound: NO DirectSound - its own object did not start (hr 0x%08lX); the wiki is silent for the "
                   "process, the engine's object is not used", (unsigned long)hr);
    } else if (!s_saidHr) {
        s_saidHr = 1;
        tagpu_logf("wiki sound: DirectSound created (hr 0x%08lX), PRIORITY on the game's window", (unsigned long)hr);
    }
    return s_ds;
}

/* the clip's samples into buffer `b`, whole */
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

static IDirectSoundBuffer* make(IDirectSound* ds, const WIKI3DCLIP* c)
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
        tagpu_logf("wiki sound: no buffer for %s (hr 0x%08lX)", c->name, (unsigned long)hr);
        return NULL;
    }
    tagpu_logf("wiki sound: a buffer for %s - %d Hz, %d-bit, %d channel(s), %lu bytes", c->name, c->rate, c->bits,
               c->channels, (unsigned long)c->bytes);
    return b;
}

/* 0x4CF570's choice among the clip's voices: a stopped one, else a new duplicate, else the one
   furthest through its samples; NULL when none can be had */
static IDirectSoundBuffer* voice(IDirectSound* ds, SET* set)
{
    DWORD best = 0, st, play, write;
    int i, furthest = -1;
    for (i = 0; i < set->nvoice; i++) {
        if (FAILED(IDirectSoundBuffer_GetStatus(set->voice[i], &st))) continue;
        if (!(st & DSBSTATUS_PLAYING)) return set->voice[i];
        if (SUCCEEDED(IDirectSoundBuffer_GetCurrentPosition(set->voice[i], &play, &write)) && (furthest < 0 || play > best)) {
            best = play;
            furthest = i;
        }
    }
    if (set->nvoice == 0) {
        if (!(set->voice[0] = make(ds, set->clip))) return NULL;
        return set->voice[set->nvoice++];
    }
    if (set->nvoice < WIKISOUND_VOICES &&
        SUCCEEDED(IDirectSound_DuplicateSoundBuffer(ds, set->voice[0], &set->voice[set->nvoice])))
        return set->voice[set->nvoice++];
    if (furthest < 0) return NULL;
    IDirectSoundBuffer_Stop(set->voice[furthest]);
    return set->voice[furthest];
}

/* clip `c` of `s` from its start; BOUND: `c` indexes the published clips only below their count */
static int play(const WIKI3DSOUND* s, int c)
{
    IDirectSound* ds;
    IDirectSoundBuffer* b;
    HRESULT hr;
    if (!s || c < 0 || c >= s->nclip || c >= WIKI3D_CLIPS || !s->clip[c].data) {
        s_silent++;
        return 0;
    }
    if (!(ds = device())) {
        s_silent++;
        return 0;
    }
    if (s_set[c].clip != &s->clip[c]) {
        /* the published sound is one for the process; a set made for another clip is let go */
        int i;
        for (i = 0; i < s_set[c].nvoice; i++) IDirectSoundBuffer_Release(s_set[c].voice[i]);
        memset(&s_set[c], 0, sizeof s_set[c]);
        s_set[c].clip = &s->clip[c];
    }
    if (!(b = voice(ds, &s_set[c]))) {
        s_failed++;
        return 0;
    }
    IDirectSoundBuffer_SetCurrentPosition(b, 0);
    IDirectSoundBuffer_SetVolume(b, VOLUME);
    hr = IDirectSoundBuffer_Play(b, 0, 0, 0);
    /* a lost buffer comes back by Restore and its samples written again, then plays once more */
    if (hr == DSERR_BUFFERLOST && SUCCEEDED(IDirectSoundBuffer_Restore(b)) && fill(b, s->clip + c))
        hr = IDirectSoundBuffer_Play(b, 0, 0, 0);
    if (FAILED(hr)) {
        s_failed++;
        return 0;
    }
    s_played++;
    return 1;
}

/* the settings that silence every sound of the engine's, as 0x47F0C0 and 0x47FD70 test them */
static int muted(const TAGPU_SOUNDSET* set)
{
    return set->win_sound || set->no_dsound || set->fxvol == 0 || set->mode == 0;
}

int tagpu_wikisound_reply(const WIKI3DSOUND* s, int cat, int kind, int variant, int priority)
{
    TAGPU_SOUNDSET set;
    int i;
    if (!s || !tagpu_menu_sound(&set) || muted(&set) || !set.speech || priority <= 10 - set.unitchat ||
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
    TAGPU_SOUNDSET set;
    if (!s || !tagpu_menu_sound(&set) || muted(&set) || name < 0 || name >= WIKI_NSNDNAME) {
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

void tagpu_wikisound_release(void)
{
    int i, k;
    for (i = 0; i < WIKI3D_CLIPS; i++) {
        for (k = 0; k < s_set[i].nvoice; k++) {
            IDirectSoundBuffer_Stop(s_set[i].voice[k]);
            IDirectSoundBuffer_Release(s_set[i].voice[k]);
        }
        memset(&s_set[i], 0, sizeof s_set[i]);
    }
    if (s_ds) IDirectSound_Release(s_ds);
    s_ds = NULL;
}
