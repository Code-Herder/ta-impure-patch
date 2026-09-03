/* tagpu_zoom.c — the view transform shared by the render pass and the input
   path. See tagpu_zoom.h for the contract and why it is lock-free. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "dd.h"
#include "tagpu_zoom.h"
#include "tagpu_detour.h"
#include "tagpu_vpwide.h"

/* Published view. Volatile because two threads touch it; each is one aligned
   32-bit slot, which x86 loads and stores atomically. */
static volatile float s_zoom = 1.0f;
static volatile LONG  s_vpL, s_vpT, s_vw, s_vh;
static volatile LONG  s_live;          /* a zoomed world is on screen right now */
static volatile LONG  s_fresh;         /* the pass published during this frame     */
static int            g_mmInstalled;   /* tagpu_zoom_init() patched the engine     */

float tagpu_zoom_read_lever(void)
{
    /* Re-read EVERY frame: this is a continuous control, so a 30-frame poll
       would quantise a ramp to 2 Hz and make a smooth renderer look like a
       staircase on video. On a bad parse the LAST GOOD value is kept rather
       than snapping back to 1.0 — a writer driving a ramp at 60 Hz can be
       caught mid-write, and a one-frame jump to unzoomed reads as a flicker.
       Only the file's ABSENCE means "no zoom". */
    HANDLE zh = CreateFileA("tagpu_zoom.txt", GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (zh == INVALID_HANDLE_VALUE) {
        s_zoom = 1.0f;
    } else {
        char zb[32]; DWORD zn = 0;
        if (ReadFile(zh, zb, sizeof zb - 1, &zn, 0) && zn > 0) {
            float z;
            zb[zn] = 0;
            z = (float)atof(zb);
            if (z >= 0.25f && z <= 8.0f) s_zoom = z;
        }
        CloseHandle(zh);
    }
    return s_zoom;
}

void tagpu_zoom_publish_view(int vpL, int vpT, int vw, int vh)
{
    if (vw <= 0 || vh <= 0) return;
    s_vpL = vpL; s_vpT = vpT; s_vw = vw; s_vh = vh;
    s_live = 1;
    s_fresh = 1;
}

/* ---- scroll rate -----------------------------------------------------------

   `main+0x1434D` is the engine's scroll speed (u8): the registry value
   "ScrollSpeed" under HKCU\Software\Cavedog Entertainment\Total Annihilation,
   loaded at 0x42FB62 and defaulted to 0x20 when the value is missing. It is a
   distance in WORLD pixels per scroll tick, so at zoom z the same pointer dwell
   at the screen edge moves the view z times less far across the SCREEN, and
   scrolling at 0.25x feels glued. Scale it by 1/z and the screen moves at the
   same rate at every zoom.

   This is the one engine field the zoom writes, and it is safe to write: the
   eye is not simulated — it is a per-player camera preference that no other
   machine ever sees — so this cannot perturb the sim or an MP session.

   The base is re-read from the engine whenever the value is not the one we last
   wrote, so changing the preference in the options screen is picked up on the
   next frame instead of being overwritten by a stale cache.

   AND THE FIELD IS PERSISTED, which is why `zoom_save_scroll` below exists.
   `0x430F00` (save-all-preferences, called from a dozen places — e.g. `0x416D03`
   sets the scroll speed and `0x416D09` immediately saves) writes this live byte
   to the registry at `0x430FAE`. Left alone, a zoomed session would persist the
   SCALED number as the player's preference, and because the next launch loads it
   as the new base it would COMPOUND across launches (32 → 64 → 128 …). So the
   one call site that persists it is redirected to save the player's own value.
   The options screen still displays the scaled number while zoomed, which is at
   least the rate scrolling is actually running at. */
#define TA_MAINPP        0x00511DE8u
#define OFF_SCROLLSPEED  0x1434D

static unsigned char s_scrollBase, s_scrollWrote;

static void apply_scroll_rate(void)
{
    const char* ta = *(const char* const*)TA_MAINPP;
    unsigned char* p;
    float z;
    int v;

    if ((size_t)ta <= 0x600000u || (size_t)ta >= 0x7FFF0000u) return;
    p = (unsigned char*)(ta + OFF_SCROLLSPEED);
    if (*p != s_scrollWrote) s_scrollBase = *p;   /* the engine or the player */
    if (!s_scrollBase) return;

    z = tagpu_zoom_level();
    /* Without the save guard installed (tagpu_zoom.on absent) the scaled value
       would reach the player's registry, so leave the field alone entirely. */
    v = (g_mmInstalled && z > 0.05f && z != 1.0f)
        ? (int)((float)s_scrollBase / z + 0.5f) : (int)s_scrollBase;
    if (v < 1) v = 1;
    if (v > 255) v = 255;
    *p = s_scrollWrote = (unsigned char)v;
}

void tagpu_zoom_frame_end(void)
{
    if (!s_fresh) s_live = 0;
    s_fresh = 0;
    /* after s_live settles, so a frame that drew nothing zoomed puts the
       engine's own rate back */
    apply_scroll_rate();
    /* and the same for the addressable rect: a frame that published nothing
       must hand the engine its own viewport back before the menus see it */
    tagpu_vpwide_frame(tagpu_zoom_level());
}

float tagpu_zoom_level(void)
{
    return s_live ? s_zoom : 1.0f;
}

/* Snapshot the published view. Returns 0 when there is nothing to do —
   the caller then leaves the point alone. */
static int view(float* z, float* cx, float* cy, int* L, int* T, int* W, int* H)
{
    float zz = s_live ? s_zoom : 1.0f;
    if (zz <= 0.05f || zz == 1.0f) return 0;
    *L = (int)s_vpL; *T = (int)s_vpT; *W = (int)s_vw; *H = (int)s_vh;
    if (*W <= 0 || *H <= 0) return 0;
    *z = zz;
    *cx = (float)*L + (float)*W * 0.5f;
    *cy = (float)*T + (float)*H * 0.5f;
    return 1;
}

static int in_viewport(int x, int y, int L, int T, int W, int H)
{
    return x >= L && y >= T && x < L + W && y < T + H;
}

/* Round toward the nearest pixel, negatives included: (int) truncates toward
   zero, which would bias every point left of the centre by half a pixel and
   make a zoomed-out click land one pixel off on one side of the screen only. */
static int iround(float v)
{
    return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/* The whole transform. `ring` (may be NULL) reports that the true `u` fell
   outside the viewport — the display-only ring, where the engine has no screen
   position for the world under the pointer.

   There the transform gives up and returns the pointer UNCHANGED rather than
   clamping it to the viewport edge. Clamping looks tempting and is worse in
   every way: it parks the engine's cursor in the middle of the screen, it kills
   the edge scroll (whose trigger zone at the right of the frame is inside the
   viewport, so a clamped `u` never reaches it), and it still names a world point
   the player did not click. Identity keeps every screen-space behaviour — hover,
   edge scroll, the cursor — working exactly as at 1x, and the one thing that
   would then be wrong, a click on the 1x world point, is what
   tagpu_zoom_drop_mouse() throws away. */
/* to_engine with the ring measured against the TRUE viewport, whatever
   tagpu_vpwide may have widened the engine's rect to. */
static int to_engine_true(int* x, int* y)
{
    float z, cx, cy; int L, T, W, H, ux, uy;
    if (!x || !y || !view(&z, &cx, &cy, &L, &T, &W, &H)) return 0;
    if (!in_viewport(*x, *y, L, T, W, H)) return 0;   /* screen-space: 1:1 */
    ux = iround(((float)*x - cx) / z + cx);
    uy = iround(((float)*y - cy) / z + cy);
    if (!in_viewport(ux, uy, L, T, W, H)) return 0;   /* the ring: unchanged */
    *x = ux; *y = uy;
    return 1;
}

static int to_engine(int* x, int* y, int* ring)
{
    float z, cx, cy; int L, T, W, H, ux, uy;
    if (ring) *ring = 0;
    if (!x || !y || !view(&z, &cx, &cy, &L, &T, &W, &H)) return 0;
    if (!in_viewport(*x, *y, L, T, W, H)) return 0;   /* screen-space: 1:1 */
    ux = iround(((float)*x - cx) / z + cx);
    uy = iround(((float)*y - cy) / z + cy);
    /* The ring is whatever the engine cannot NAME, so it is measured against
       the addressable rect — which tagpu_vpwide has widened to exactly this
       transform's range when it is armed, and which is the true viewport when
       it is not. The gate above stays on the TRUE rect: it decides "did the
       player click on the world or on the screen-space UI", and that boundary
       does not move. */
    {
        int aL, aT, aW, aH;
        if (tagpu_vpwide_addressable(&aL, &aT, &aW, &aH)) {
            L = aL; T = aT; W = aW; H = aH;
        }
    }
    if (!in_viewport(ux, uy, L, T, W, H)) {
        if (ring) *ring = 1;
        return 0;
    }
    *x = ux; *y = uy;
    return 1;
}

int tagpu_zoom_to_engine(int* x, int* y)
{
    return to_engine(x, y, NULL);
}

/* The same transform for the position the engine DRAWS its cursor at, and the
   one place the addressable rect must NOT be consulted.

   The engine draws its cursor wherever GetCursorPos reports, and the composite
   moves the sprite back under the pointer from there — which only works while
   that position is inside the engine's own 1x viewport, over the terrain key
   fill. tagpu_vpwide widens what the engine can NAME, not what it can DRAW ON:
   a ring `u` lands on the side panel (where the sprite is composited over panel
   pixels the composite must not stamp into the world) or off the surface
   entirely. So the ring test here stays on the TRUE viewport and the ring keeps
   G13e's answer — the pointer is handed through unchanged and the engine draws
   its cursor exactly where the player's pointer is, which needs no moving at
   all. The input path is unaffected: what the engine can NAME still comes from
   the messages, which carry the widened `u`. */
int tagpu_zoom_to_engine_draw(int* x, int* y)
{
    return to_engine_true(x, y);
}

#define to_engine_draw_pt to_engine_true

int tagpu_zoom_cursor_shift(int* dx, int* dy, int* ux, int* uy)
{
    /* g_ddraw.cursor is the TRUE pointer position: every write site stores `s`
       and only the engine-facing reads are unzoomed, which is what lets this
       recover both halves of the pair. */
    int sx = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
    int sy = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);
    int ex = sx, ey = sy;

    if (dx) *dx = 0;
    if (dy) *dy = 0;
    if (ux) *ux = 0;
    if (uy) *uy = 0;
    /* the DRAW transform, so this agrees with where the engine actually put the
       sprite: in the ring that is the pointer itself and there is nothing to
       move (tagpu_zoom_to_engine_draw) */
    if (!to_engine_draw_pt(&ex, &ey)) return 0;
    if (sx == ex && sy == ey) return 0;      /* nothing to move */
    if (dx) *dx = sx - ex;
    if (dy) *dy = sy - ey;
    if (ux) *ux = ex;
    if (uy) *uy = ey;
    return 1;
}

/* Which messages carry a point in lParam. WM_MOUSEWHEEL is in the list because
   cnc-ddraw has already converted its screen-space point to client space by the
   time the message reaches the engine's window procedure. */
static int carries_point(UINT msg)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:    case WM_MOUSEHOVER:   case WM_NCHITTEST:
    case WM_MOUSEWHEEL:
    case WM_LBUTTONDOWN:  case WM_LBUTTONUP:    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:  case WM_RBUTTONUP:    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:  case WM_MBUTTONUP:    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:  case WM_XBUTTONUP:    case WM_XBUTTONDBLCLK:
        return 1;
    }
    return 0;
}

/* The subset of carries_point() that ACTS on the world rather than just moving
   the pointer. A move or a hit-test in the ring is harmless; a button press
   there is not. Returns the button's slot (so a press and its release share
   one), -1 for every other message, and sets `down`. */
static int button_slot(UINT msg, int* down)
{
    *down = 1;
    switch (msg)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return 2;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: return 3;
    }
    *down = 0;
    switch (msg)
    {
    case WM_LBUTTONUP: return 0;
    case WM_RBUTTONUP: return 1;
    case WM_MBUTTONUP: return 2;
    case WM_XBUTTONUP: return 3;
    }
    return -1;
}

/* Which buttons went down in the ring and were therefore never delivered. */
static unsigned char s_dropped[4];

int tagpu_zoom_drop_mouse(UINT msg, LPARAM lparam)
{
    int x, y, ring = 0, down = 0, slot = button_slot(msg, &down);

    if (slot < 0) return 0;

    /* A PRESS AND ITS RELEASE ARE DROPPED TOGETHER OR NOT AT ALL. Deciding a
       release on its own position is wrong in both directions: a drag begun on
       an addressable point and released in the ring would have its release
       swallowed and leave the engine holding the button for the rest of the
       game, and a release delivered for a press that was dropped is a button-up
       the engine never saw go down. So the press decides and the release
       follows it. */
    if (!down) {
        int drop = s_dropped[slot];
        s_dropped[slot] = 0;
        return drop;
    }

    x = (int)(short)LOWORD(lparam);
    y = (int)(short)HIWORD(lparam);
    /* the return value is "was it transformed", which is 0 in the ring — `ring`
       is the flag to read, and it is the only one that matters here */
    to_engine(&x, &y, &ring);
    s_dropped[slot] = (unsigned char)ring;
    return ring;
}

LPARAM tagpu_zoom_mouse_lparam(UINT msg, LPARAM lparam)
{
    if (!carries_point(msg)) return lparam;
    int x = (int)(short)LOWORD(lparam);
    int y = (int)(short)HIWORD(lparam);
    if (!tagpu_zoom_to_engine(&x, &y)) return lparam;
    return MAKELPARAM((short)x, (short)y);
}


/* ---- the minimap's view rectangle ------------------------------------------

   `0x466B70(RECT*)` is a pure computation with two call sites, both in the eye
   clamp `0x41C3C0`:

       out->left   = mmX + mmW * eyeX / mapW          mm* = main+0x142E7..0x142ED
       out->top    = mmY + mmH * eyeY / mapH          map* = main+0x1422B/0x1422F
       out->right  = left + mmW * viewCellsW * 16 / mapW - 1   (main+0x1423B)
       out->bottom = top  + mmH * viewCellsH * 16 / mapH - 1   (main+0x1423F)

   Its size therefore comes from the 1x view, which is exactly what is no longer
   true. Rather than reproduce any of that, let the engine fill the rect and
   scale the RESULT about its own centre by 1/z: at 0.5x the box on the minimap
   doubles, which is what the player is actually looking at. Clamped to the
   minimap so a wildly zoomed-out view cannot draw a box off the edge of it. */

#define SAVE_SETTING_VA  0x004B6A50u   /* stdcall(section,name,dword), ret 0xC */
#define SITE_SAVESCROLL  0x00430FAEu   /* the one site that persists ScrollSpeed */
#define MINIMAP_RECT_VA  0x00466B70u   /* stdcall(RECT*), ret 4              */
#define SITE_MMRECT1_VA  0x0041C426u   /* call 0x466B70 — eye clamped at top */
#define SITE_MMRECT2_VA  0x0041C442u   /* call 0x466B70 — ...and at bottom   */
#define OFF_MM_X         0x142E7       /* i16 minimap rect on screen         */
#define OFF_MM_Y         0x142E9
#define OFF_MM_W         0x142EB
#define OFF_MM_H         0x142ED

static void zlog(const char* m)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", m); fclose(f); }
}

static void __stdcall zoom_minimap_rect(int* r)
{
    const char* ta;
    float zm = tagpu_zoom_level();
    float cx, cy, hw, hh;
    int mx, my, mw, mh;

    ((void (__stdcall *)(int*))MINIMAP_RECT_VA)(r);
    if (!r || zm <= 0.05f || zm == 1.0f) return;

    cx = (float)(r[0] + r[2]) * 0.5f;
    cy = (float)(r[1] + r[3]) * 0.5f;
    hw = (float)(r[2] - r[0]) * 0.5f / zm;
    hh = (float)(r[3] - r[1]) * 0.5f / zm;
    r[0] = iround(cx - hw); r[2] = iround(cx + hw);
    r[1] = iround(cy - hh); r[3] = iround(cy + hh);

    ta = *(const char* const*)TA_MAINPP;
    if ((size_t)ta <= 0x600000u || (size_t)ta >= 0x7FFF0000u) return;
    mx = *(const short*)(ta + OFF_MM_X); my = *(const short*)(ta + OFF_MM_Y);
    mw = *(const short*)(ta + OFF_MM_W); mh = *(const short*)(ta + OFF_MM_H);
    if (mw <= 0 || mh <= 0) return;
    if (r[0] < mx) r[0] = mx;
    if (r[1] < my) r[1] = my;
    if (r[2] > mx + mw - 1) r[2] = mx + mw - 1;
    if (r[3] > my + mh - 1) r[3] = my + mh - 1;
}

/* Keep our scaling out of the player's registry — see the block above. */
static void __stdcall zoom_save_scroll(void* section, const char* name, int value)
{
    if (s_scrollBase && (unsigned char)value == s_scrollWrote)
        value = (int)s_scrollBase;
    ((void (__stdcall *)(void*, const char*, int))SAVE_SETTING_VA)(section, name, value);
}

/* an `E8 <rel32>` at `site` whose target is `expect`? */
static int site_is(unsigned int site, unsigned int expect)
{
    const unsigned char* p = (const unsigned char*)(size_t)site;
    if (IsBadReadPtr((void*)p, 5)) return 0;
    return p[0] == 0xE8 &&
           *(const unsigned int*)(p + 1) == expect - (site + 5);
}

static int redirect(unsigned int site, void* target)
{
    unsigned char b[5];
    b[0] = 0xE8;
    *(unsigned int*)(b + 1) = (unsigned int)(size_t)target - (site + 5);
    return tagpu_detour_write(site, b, 5);
}

void tagpu_zoom_init(void)
{
    int ok;

    if (GetFileAttributesA("tagpu_zoom.on") == INVALID_FILE_ATTRIBUTES) return;

    /* Every byte is checked before any of them is written, so a patched or
       different exe arms nothing rather than half of it. */
    if (!site_is(SITE_MMRECT1_VA, MINIMAP_RECT_VA) ||
        !site_is(SITE_MMRECT2_VA, MINIMAP_RECT_VA) ||
        !site_is(SITE_SAVESCROLL, SAVE_SETTING_VA)) {
        zlog("zoom: NOT armed — engine bytes differ at one of "
             "0x41C426/0x41C442/0x430FAE");
        return;
    }
    ok  = redirect(SITE_MMRECT1_VA, (void*)zoom_minimap_rect);
    ok &= redirect(SITE_MMRECT2_VA, (void*)zoom_minimap_rect);
    ok &= redirect(SITE_SAVESCROLL, (void*)zoom_save_scroll);
    g_mmInstalled = ok;
    zlog(ok ? "zoom: ARMED (minimap rect 0x466B70 x2, ScrollSpeed save 0x430FAE)"
            : "zoom: PARTIAL — see above");
}
