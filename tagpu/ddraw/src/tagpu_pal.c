/* tagpu_pal.c — the palette the screen is actually shown with (tagpu_pal.h).

   RENDER THREAD ONLY. tagpu_overlay_draw marks the snapshot stale once per
   present; the first reader after that re-resolves it, so the critical section
   below is entered once a frame however many passes ask.

   Until G15d every pass read main+0x143A7 and was wrong by the Gamma factor;
   G15d fixed the UI twin alone and left the world. This is that fix's other
   half, and the resolution now lives in one place instead of two. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "dd.h"                     /* g_ddraw.primary / g_ddraw.cs        */
#include "IDirectDrawSurface.h"
#include "IDirectDrawPalette.h"
#include "tagpu_pal.h"

#define TA_MAINPP      0x00511DE8u  /* -> the engine's main struct         */
#define OFF_PALETTE    0x143A7      /* 256 x {R,G,B,pad}, never gamma'd    */
#define GFX_GLOBALS_PP 0x0051FBD0u  /* -> the graphics globals (0x4B6220)  */
#define OFF_GAMMA      0x614        /* the float 0x4BA200 scales by        */
#define GAMMA_MIN      0.05f
#define GAMMA_MAX      8.0f
#define LOG_MS         1000         /* a campaign fade is a change a step  */

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

static void plog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static unsigned char s_pal[1024];       /* R,G,B,255 — what the screen shows  */
static unsigned char s_eng[1024];       /* R,G,B,255 — main+0x143A7, unscaled  */
static int      s_haveEng;
static int      s_have;                 /* s_pal holds a resolved palette      */
static int      s_presented;            /* it came from the primary's object   */
static unsigned s_serial;               /* bumped on every change of s_pal     */
static unsigned s_changes;              /* the same count, for the heartbeat   */
static int      s_diff = 0, s_diffAt = -1;
static int      s_dirty = 1;
static DWORD    s_lastLog;

void tagpu_pal_frame(void) { s_dirty = 1; }

static void resolve(void)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    const unsigned char* engine = NULL;
    unsigned char rgba[1024];
    int i, presented = 0, have = 0;

    if (ptr_ok(ta) && ptr_ok(ta + OFF_PALETTE)) engine = (const unsigned char*)(ta + OFF_PALETTE);

    /* A LIFETIME, not a probe: the game thread NULLs g_ddraw.primary inside
       this section when the primary's last reference goes
       (IDirectDrawSurface__Release) and frees the object only after leaving
       it, so a pointer read AND dereferenced inside the section is a live
       object or NULL — never a freed one. The interleave stays inside the
       guard because RGBQUAD is B,G,R,reserved: copying the raw bytes out and
       converting after the fact swaps red and blue (measured 2026-09-07 as
       paldiff 218 instead of 0). */
    EnterCriticalSection(&g_ddraw.cs);
    if (g_ddraw.primary && g_ddraw.primary->palette) {
        const RGBQUAD* q = g_ddraw.primary->palette->data_rgb;
        for (i = 0; i < 256; i++) {
            rgba[4*i+0] = q[i].rgbRed; rgba[4*i+1] = q[i].rgbGreen;
            rgba[4*i+2] = q[i].rgbBlue; rgba[4*i+3] = 255;
        }
        presented = have = 1;
    }
    LeaveCriticalSection(&g_ddraw.cs);

    if (engine) {
        for (i = 0; i < 256; i++) {
            s_eng[4*i+0] = engine[4*i+0]; s_eng[4*i+1] = engine[4*i+1];
            s_eng[4*i+2] = engine[4*i+2]; s_eng[4*i+3] = 255;
        }
        s_haveEng = 1;
    }
    if (!have) {
        if (!engine) return;                    /* keep the last good one */
        memcpy(rgba, s_eng, 1024);
    }
    /* the fourth byte is ours, not the engine's: every consumer wants an
       opaque RGBA texel and none of them reads +0x143A7's pad */
    s_presented = presented;
    if (s_have && memcmp(rgba, s_pal, 1024) == 0) return;

    memcpy(s_pal, rgba, 1024);
    s_have = 1;
    s_serial++;
    s_changes++;
    s_diff = 0; s_diffAt = -1;
    if (engine)
        for (i = 0; i < 256; i++)
            if (rgba[4*i] != engine[4*i] || rgba[4*i+1] != engine[4*i+1] || rgba[4*i+2] != engine[4*i+2]) {
                if (s_diffAt < 0) s_diffAt = i;
                s_diff++;
            }
    {
        DWORD t = GetTickCount();
        if (t - s_lastLog >= LOG_MS) {
            char b[160];
            s_lastLog = t;
            _snprintf(b, sizeof b, "pal: presented palette changed (serial=%u src=%s diff=%d@%d gamma=%.3f)",
                      s_serial, s_presented ? "primary" : "engine", s_diff, s_diffAt, tagpu_pal_gamma());
            plog(b);
        }
    }
}

const unsigned char* tagpu_pal_live(void)
{
    if (s_dirty || !s_have) { s_dirty = 0; resolve(); }
    return s_have ? s_pal : NULL;
}

const unsigned char* tagpu_pal_engine(void)
{
    if (s_dirty || !s_have) { s_dirty = 0; resolve(); }
    return s_haveEng ? s_eng : NULL;
}

unsigned tagpu_pal_serial(void)   { return s_serial; }
unsigned tagpu_pal_changes(void)  { return s_changes; }
int      tagpu_pal_presented(void){ return s_presented; }
int      tagpu_pal_diff(int* first) { if (first) *first = s_diffAt; return s_diff; }

float tagpu_pal_gamma(void)
{
    const char* g = *(const char* const*)GFX_GLOBALS_PP;
    float v;
    if (!ptr_ok(g) || !ptr_ok(g + OFF_GAMMA)) return 1.0f;
    v = *(const float*)(g + OFF_GAMMA);
    /* a BOUND, not a probe: the slider's own range is 0.5..1.5 and the chat
       command's is N/10, so anything outside this band is a field we are not
       reading or a struct that has moved — and 1.0 is the identity that leaves
       every colour where the engine's table put it */
    if (!(v >= GAMMA_MIN && v <= GAMMA_MAX)) return 1.0f;
    return v;
}
