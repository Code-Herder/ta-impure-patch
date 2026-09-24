/* tagpu_pal.c — the palette the screen is actually shown with (tagpu_pal.h).

   RENDER THREAD ONLY. tagpu_overlay_draw marks the snapshot stale once per
   present; the first reader after that re-resolves it, so the critical section
   below is entered once a frame however many passes ask.

   A pass that reads main+0x143A7 is wrong by the Gamma factor, so every pass
   takes the palette from here, the one place it is resolved.

   NO ENGINE MEMORY: the engine's own table
   and the gamma factor are fields of the packet, copied by the publisher on
   the game thread (both kinds of packet carry them, the level-end one too),
   and the copy taken here outlives the packet — a frame with no packet keeps
   the last table it saw. The presented half is the fork's own palette object, which is
   not engine memory at all. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "dd.h"                     /* g_ddraw.primary / g_ddraw.cs        */
#include "IDirectDrawSurface.h"
#include "IDirectDrawPalette.h"
#include "tagpu_pal.h"
#include "tagpu_packet.h"
#include "tagpu_log.h"

#define LOG_MS         1000         /* a campaign fade is a change a step  */

static void plog(const char* s)
{
    tagpu_log(s);
}

static unsigned char s_pal[1024];       /* R,G,B,255 — what the screen shows  */
static unsigned char s_eng[1024];       /* R,G,B,255 — the engine's table, from the packet */
static int      s_haveEng;
static float    s_gamma = 1.0f;         /* the engine's factor, from the packet */
static int      s_have;                 /* s_pal holds a resolved palette      */
static int      s_presented;            /* it came from the primary's object   */
static unsigned s_serial;               /* bumped on every change of s_pal     */
static unsigned s_changes;              /* the same count, for the heartbeat   */
static int      s_diff = 0, s_diffAt = -1;
static int      s_dirty = 1;
static DWORD    s_lastLog;

void tagpu_pal_frame(const TAGPU_PACKET* pk)
{
    s_dirty = 1;
    /* the engine half, from the packet: bounded by the publisher (the gamma
       to 0.05..8.0, the table a fixed 1 KB copy), taken whenever a packet
       carries it, kept when none does */
    if (pk && pk->pal_ok) {
        int i;
        for (i = 0; i < 256; i++) {
            s_eng[4*i+0] = pk->pal[4*i+0]; s_eng[4*i+1] = pk->pal[4*i+1];
            s_eng[4*i+2] = pk->pal[4*i+2]; s_eng[4*i+3] = 255;
        }
        s_haveEng = 1;
        s_gamma = (pk->gamma >= 0.05f && pk->gamma <= 8.0f) ? pk->gamma : 1.0f;
    }
}

static void resolve(void)
{
    const unsigned char* engine = s_haveEng ? s_eng : NULL;
    unsigned char rgba[1024];
    int i, presented = 0, have = 0;

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
    /* a BOUND, not a probe, applied by the publisher and again here: the
       slider's own range is 0.5..1.5 and the chat command's is N/10, so
       anything outside this band is a field we are not reading or a struct
       that has moved — and 1.0 is the identity that leaves every colour where
       the engine's table put it */
    return (s_gamma >= 0.05f && s_gamma <= 8.0f) ? s_gamma : 1.0f;
}

void tagpu_pal_expand(unsigned char* dst, const unsigned char* idx, const unsigned char* key,
                      int pitch, int x0, int y0, int w, int h, const unsigned char* pal)
{
    /* one 4-byte entry per index, R,G,B,255, so a texel is one 4-byte copy */
    unsigned char lut[256][4];
    int i, x, y;
    if (!dst || !idx || !pal || w <= 0 || h <= 0) return;
    for (i = 0; i < 256; i++) {
        lut[i][0] = pal[i * 4 + 0]; lut[i][1] = pal[i * 4 + 1];
        lut[i][2] = pal[i * 4 + 2]; lut[i][3] = 255;
    }
    for (y = 0; y < h; y++) {
        const unsigned char* ir = idx + (size_t)(y0 + y) * pitch + x0;
        const unsigned char* kr = key ? key + (size_t)(y0 + y) * pitch + x0 : NULL;
        unsigned char* o = dst + (size_t)y * w * 4;
        if (kr) {
            for (x = 0; x < w; x++, o += 4) {
                if (kr[x]) memcpy(o, lut[ir[x]], 4);
                else       memset(o, 0, 4);
            }
        } else {
            for (x = 0; x < w; x++, o += 4) memcpy(o, lut[ir[x]], 4);
        }
    }
}
