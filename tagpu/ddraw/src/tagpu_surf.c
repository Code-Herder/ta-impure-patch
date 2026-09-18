/* TA's own frame, snapshotted once a frame for whoever draws the bottom layer.
   The header carries the argument; this is the mechanism.
   [The vulkan-only plan, landing 4c-1.] */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"                     /* g_ddraw.primary / g_ddraw.cs / g_ddraw.bpp */
#include "IDirectDrawSurface.h"     /* ->surface, ->pitch, ->width, ->height      */
#include "tagpu_pal.h"
#include "tagpu_surf.h"

static void slog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static unsigned char* s_buf;            /* w*h indices, the pitch taken out   */
static unsigned       s_cap;
static int            s_w, s_h;
static int            s_have;           /* this frame's snapshot is usable    */
static unsigned       s_serial;         /* bumped only when the BYTES changed */
static unsigned char  s_pal[1024];      /* R,G,B,x per entry                  */
static int            s_palHave;
static unsigned       s_refused;        /* geometry outside the bound         */
static int            s_saidRefused;

void tagpu_surf_take(void)
{
    const unsigned char* pal;

    /* CLEARED FIRST, so that every exit below leaves `tagpu_surf_frame` saying
       "nothing this frame" rather than handing out the previous one. A stale
       bottom layer is worse than none: it is last frame's game under this
       frame's world, and it would look like a pass drawing in the wrong place
       rather than like a surface that was not read. */
    s_have = 0;

    /* THE PALETTE IS PART OF THE SNAPSHOT, not a separate lookup the consumer
       makes later. The indices mean nothing without the table they resolve
       through, and a consumer that fetched the palette itself could pair this
       frame's bytes with the next frame's table -- which is the "restored
       texels resolved through a newer palette" fault tagpu_gui_surf.c's
       draw_layer already documents at length, in a place where it is harder to
       see. Taken before the bytes and in the same call. */
    pal = tagpu_pal_live();
    if (pal) { memcpy(s_pal, pal, sizeof s_pal); s_palHave = 1; }
    if (!s_palHave) return;             /* nothing readable yet; retry next frame */

    EnterCriticalSection(&g_ddraw.cs);
    /* THE BOUND COMES FROM THE OBJECT BEING READ. `g_ddraw.width/height` is the
       device MODE; the bytes and the pitch belong to the PRIMARY, which carries
       its own geometry, and between a mode change and the primary being
       recreated the two disagree -- so `h * pitch` off the mode can run past the
       primary's allocation. CLAUDE.md: a value is DATA until it has been
       validated as data. */
    if (g_ddraw.primary && g_ddraw.primary->surface &&
        g_ddraw.bpp == 8 &&
        g_ddraw.primary->width > 0 && g_ddraw.primary->height > 0) {
        int w = g_ddraw.primary->width, h = g_ddraw.primary->height;
        int pitch = g_ddraw.primary->pitch ? (int)g_ddraw.primary->pitch : w;
        unsigned need = (unsigned)w * (unsigned)h;
        if (w > TAGPU_SURF_MAXDIM || h > TAGPU_SURF_MAXDIM) {
            s_refused++;
        } else if (pitch >= w && need) {
            if (need > s_cap) {
                unsigned char* nb = (unsigned char*)realloc(s_buf, need);
                if (nb) { s_buf = nb; s_cap = need; }
            }
            if (s_cap >= need) {
                const unsigned char* src = (const unsigned char*)g_ddraw.primary->surface;
                int y, diff = (w != s_w || h != s_h);
                /* ROW BY ROW, because the primary's pitch is not its width: the
                   copy takes the pitch OUT so that every consumer uploads w*h
                   with no row stride of its own to get wrong. */
                for (y = 0; y < h; y++) {
                    const unsigned char* sr = src + (size_t)y * pitch;
                    unsigned char* dr = s_buf + (size_t)y * w;
                    if (!diff && memcmp(dr, sr, (size_t)w) != 0) diff = 1;
                    memcpy(dr, sr, (size_t)w);
                }
                s_w = w; s_h = h; s_have = 1;
                /* ONLY WHEN THE BYTES MOVED. A consumer that uploads this to a
                   device skips the upload when the serial is the one it holds,
                   and TA redraws its whole screen far less often than we
                   present -- so the comparison above pays for itself on every
                   paused or idle frame. It is a comparison of what we just
                   copied against what we held, not of two engine reads. */
                if (diff) s_serial++;
            }
        }
    }
    LeaveCriticalSection(&g_ddraw.cs);

    if (s_refused && !s_saidRefused) {
        char b[160];
        s_saidRefused = 1;
        _snprintf(b, sizeof b, "surf: TA's primary is outside what this module carries "
                  "(max %d) - the bottom layer stays whatever cleared it", TAGPU_SURF_MAXDIM);
        b[sizeof b - 1] = 0;
        slog(b);
    }
}

int tagpu_surf_frame(const unsigned char** bytes, int* w, int* h,
                     const unsigned char** pal, unsigned* serial)
{
    if (!s_have) return 0;
    if (bytes)  *bytes  = s_buf;
    if (w)      *w      = s_w;
    if (h)      *h      = s_h;
    if (pal)    *pal    = s_pal;
    if (serial) *serial = s_serial;
    return 1;
}
