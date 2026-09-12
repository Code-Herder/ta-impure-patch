/* tagpu_hud.c — HUD scale. See tagpu_hud.h for what it is, why it writes no
   engine memory at all, and what makes the one word that crosses threads safe;
   research/notes/gui-renderer.md §22 for why the art is magnified rather than
   re-laid-out and §22.5 for the origin tear that took the viewport rect out of
   this file. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"
#include "tagpu_hud.h"
#include "tagpu_opt.h"

/* The panel's logical width and the bars' logical height: the two immediates
   at 0x4981C9 / 0x4981D9, and the 128x480 block §3.4a measured. PANEL_ROWS is
   what makes H/480 the ceiling. */
#define HUD_PANEL_W   128
#define HUD_BAR_H     32
#define HUD_PANEL_ROWS 480
/* The world keeps at least this much width whatever the ceiling says. H/480 is
   the ceiling for every aspect a player can choose, but a tall, narrow surface
   would drive 128s past the screen and leave no world at all — a bound, so
   that cannot be a thing that happens rather than a thing that is unlikely. */
#define HUD_MIN_VIEW_W 256

#define LEVER "tagpu_hud.on"
#define LEVER_OFF "tagpu_hud.off"

/* THE ONE WORD THAT CROSSES THREADS (tagpu_hud.h). -1 = the pass is off, 0 =
   Auto, else a percentage. Every reader re-resolves it against the screen IT
   sees, so either value a racing read can return is a value that fits that
   screen. */
static volatile LONG s_pctLive = -1;

static void hlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int dims_ok(int w, int h)
{
    return w >= 320 && h >= 240 && w <= 8192 && h <= 8192;
}

int tagpu_hud_ceiling_pct(int screenW, int screenH)
{
    int capH, capW, cap;
    if (!dims_ok(screenW, screenH)) return 100;
    capH = (screenH * 256) / HUD_PANEL_ROWS;
    capW = ((screenW - HUD_MIN_VIEW_W) * 256) / HUD_PANEL_W;
    cap  = capH < capW ? capH : capW;
    if (cap < 256) cap = 256;
    return cap * 100 / 256;
}

void tagpu_hud_geom(int screenW, int screenH, int pct, int* q8, int* panelW, int* barH)
{
    int capH, capW, cap, q = 256;
    if (dims_ok(screenW, screenH) && pct >= 0) {
        capH = (screenH * 256) / HUD_PANEL_ROWS;
        capW = ((screenW - HUD_MIN_VIEW_W) * 256) / HUD_PANEL_W;
        cap  = capH < capW ? capH : capW;
        if (cap < 256) cap = 256;                  /* never below stock        */
        q = pct == 0 ? cap : pct * 256 / 100;      /* Auto IS the ceiling      */
        if (q < 256) q = 256;
        if (q > cap) q = cap;                      /* a stage past it is clamped */
    }
    if (q8)     *q8     = q;
    if (panelW) *panelW = (HUD_PANEL_W * q) >> 8;
    if (barH)   *barH   = (HUD_BAR_H  * q) >> 8;
}

/* ---- the store: the lever file the front-end row writes ------------------ */

int tagpu_hud_stored_pct(void)
{
    char buf[128];
    const char* k;
    int n = tagpu_opt_read(LEVER, buf, sizeof buf);
    if (n < 0) return -1;                          /* off: .off, or defaults off */
    k = strstr(buf, "scale=");
    if (!k) return 0;                              /* armed and silent means Auto */
    if (!_strnicmp(k + 6, "auto", 4)) return 0;
    n = atoi(k + 6);
    return (n >= 100 && n <= 800) ? n : 0;
}

/* BOTH FILES, always. tagpu_opt.c's precedence is that an `.on` wins and an
   `.off` only defeats a pass that was on BY DEFAULT, so driving one file fails
   in one direction or the other depending on how the install was armed — the
   bug tagpu_menu.c's write_levers() documents at length.

   The live word is set from the SAME call, so the row takes effect on the next
   frame and the file is only how it survives a restart. Nothing here reaches
   engine memory, so there is no game-entry ordering to respect: what the first
   build needed the observer for was the viewport rect, and there is no longer
   a viewport rect to write. */
void tagpu_hud_store_pct(int pct)
{
    HANDLE h;
    char body[64];
    DWORD wrote = 0;
    InterlockedExchange(&s_pctLive, pct < 0 ? -1 : pct);
    if (pct < 0) {
        DeleteFileA(LEVER);
        h = CreateFileA(LEVER_OFF, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return;
    }
    DeleteFileA(LEVER_OFF);
    if (pct == 0) lstrcpynA(body, "scale=auto\r\n", sizeof body);
    else          _snprintf(body, sizeof body, "scale=%d\r\n", pct);
    body[sizeof body - 1] = 0;
    h = CreateFileA(LEVER, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, body, (DWORD)strlen(body), &wrote, 0);
    CloseHandle(h);
}

/* ---- what the consumers ask ---------------------------------------------- */

int tagpu_hud_live(int* panelW, int* barH, int* q8)
{
    int q, pw, bh;
    /* THE SURFACE THE FORK IS PRESENTING, not the engine's globals: the shell
       is atom-locked at 640x480 whatever the Screen Size row says, and 640x480
       resolves to stock, so the shell needs no signal of its own. */
    tagpu_hud_geom((int)g_ddraw.width, (int)g_ddraw.height,
                   (int)s_pctLive, &q, &pw, &bh);
    if (q <= 256) return 0;
    if (panelW) *panelW = pw;
    if (barH)   *barH   = bh;
    if (q8)     *q8     = q;
    return 1;
}

/* Screen -> the engine's own 1x grid. The engine's hit tests are all written
   against the 128 / 32 constants and its world->screen projection bakes
   +0x80/+0x20, so the ONLY thing that may move is a point inside a magnified
   HUD region: outside one this is the identity, which is what keeps the world
   half of every click answering about the place it is drawn. */
/* ON THE SURFACE, ALWAYS. The bottom bar's map is the shader's, inverted:
   source = H − (H − dest)/s, which is what keeps the pointer and the picture
   on one relation rather than two that nearly agree. It is also unbounded at
   the very last row — (H − y) is 1 there and 256/q is 0, so y = H − 1 lands on
   engine row H, one past the surface. The shader does not care (it samples in
   floats and clamps), an integer hit test does.

   The clamp is HERE and not at the nine call sites, because two of them
   (fake_GetCursorPos, and the WM_MOUSEMOVE arm of HandleMessage) clamp BEFORE
   the map rather than after, so a call-site clamp would be a rule that holds
   only where someone remembered it. A postcondition holds everywhere. */
static void on_surface(int* x, int* y)
{
    int W = (int)g_ddraw.width, H = (int)g_ddraw.height;
    if (W > 0) { if (*x > W - 1) *x = W - 1; }
    if (H > 0) { if (*y > H - 1) *y = H - 1; }
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

void tagpu_hud_to_engine(int* x, int* y)
{
    int pw, bh, q, H;
    if (!x || !y || !tagpu_hud_live(&pw, &bh, &q)) return;
    H = (int)g_ddraw.height;
    /* the same three regions, in the same order, as LAY_FS's map — and the
       same order again in tagpu_hud_to_screen. The panel owns its full column
       height, so the bottom-left corner is the panel's and not the bar's. */
    if (*x < pw || *y < bh) {
        *x = *x * 256 / q;
        *y = *y * 256 / q;
    } else if (*y >= H - bh) {
        *x = *x * 256 / q;
        *y = H - (H - *y) * 256 / q;
    }
    on_surface(x, y);
}

void tagpu_hud_to_screen(int* x, int* y)
{
    int q, H;
    if (!x || !y || !tagpu_hud_live(NULL, NULL, &q)) return;
    H = (int)g_ddraw.height;
    if (*x < HUD_PANEL_W || *y < HUD_BAR_H) {
        *x = *x * q / 256;
        *y = *y * q / 256;
    } else if (*y >= H - HUD_BAR_H) {
        *x = *x * q / 256;
        *y = H - (H - *y) * q / 256;
    }
    on_surface(x, y);
}

void tagpu_hud_init(void)
{
    int pct = tagpu_hud_stored_pct();
    char b[160];
    InterlockedExchange(&s_pctLive, pct);
    _snprintf(b, sizeof b,
              "hud: %s - the HUD covers the world, nothing is written to the engine; "
              "stored %d",
              pct < 0 ? "off" : "ARMED", pct);
    b[sizeof b - 1] = 0;
    hlog(b);
}
