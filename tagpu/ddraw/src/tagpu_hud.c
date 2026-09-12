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
#include "tagpu_detour.h"

/* The panel's logical width and the bars' logical height: the two immediates
   at 0x4981C9 / 0x4981D9, and the 128x480 block §3.4a measured. PANEL_ROWS is
   what makes H/480 the ceiling. */
#define TA_MAINPP     0x00511DE8u

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

/* EDGE SCROLL IS AN EXACT EQUALITY ON THE OUTERMOST PIXEL, so the border has
   to be mapped border to border and the region map is not enough.

   The scroll poll (exe map, "The scroll poll") fires left on `x == 0`, up on
   `y == 0`, right on `x == main+0x37E1F - 1` and down on `y == main+0x37E23 - 1`
   — the SCREEN size, which we do not write, and never the viewport rect. Run
   the region map over a 3840x2160 screen at s = 4.5 and:

     - the screen's right column (3839) is in the world region, so it comes back
       as 3839 - 448 = 3391 and the right edge NEVER FIRES;
     - the left column maps to 0, but so do device columns 1..4, because the
       panel contracts by 1/s — a five-pixel band where stock has one pixel.

   So: the outermost device column IS the outermost engine column, and only it.
   It costs nothing anywhere else, because `0x498DA0` clamps the point into
   [L,R] x [T,B] before it makes a world point (`0x498E32`..`0x498E86`), so
   engine 3839 picks exactly the column the player is pointing at — the same
   world point the region map would have produced. */
static void border_to_border(int dx, int dy, int* x, int* y)
{
    int W = (int)g_ddraw.width, H = (int)g_ddraw.height;

    if (W > 2) {
        if      (dx == 0)     *x = 0;
        else if (dx == W - 1) *x = W - 1;
        else if (*x <= 0)     *x = 1;
        else if (*x >= W - 1) *x = W - 2;
    }
    if (H > 2) {
        if      (dy == 0)     *y = 0;
        else if (dy == H - 1) *y = H - 1;
        else if (*y <= 0)     *y = 1;
        else if (*y >= H - 1) *y = H - 2;
    }
}

void tagpu_hud_to_engine(int* x, int* y)
{
    int pw, bh, q, H, dx, dy;
    if (!x || !y || !tagpu_hud_live(&pw, &bh, &q)) return;
    H = (int)g_ddraw.height;
    dx = *x; dy = *y;                        /* the DEVICE point, for the border */
    /* the same three regions, in the same order, as LAY_FS's map — and the
       same order again in tagpu_hud_to_screen. The panel owns its full column
       height, so the bottom-left corner is the panel's and not the bar's. */
    if (*x < pw || *y < bh) {
        *x = *x * 256 / q;
        *y = *y * 256 / q;
    } else if (*y >= H - bh) {
        *x = *x * 256 / q;
        *y = H - (H - *y) * 256 / q;
    } else {
        /* THE WORLD, and it is no longer the identity (22.6). The engine draws
           the world into [128, R] x [32, B] of its own surface and we put that
           block on screen at [128s, W-1] x [32s, H-1-32s], so a screen point in
           the world comes back by the same vector. Screen 128s -> engine 128
           and screen 32s -> engine 32, exactly. */
        *x -= pw - HUD_PANEL_W;
        *y -= bh - HUD_BAR_H;
    }
    on_surface(x, y);
    border_to_border(dx, dy, x, y);
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
    } else {
        int pw, bh;
        tagpu_hud_geom((int)g_ddraw.width, (int)g_ddraw.height,
                       (int)s_pctLive, NULL, &pw, &bh);
        *x += pw - HUD_PANEL_W;          /* the inverse of to_engine's world  */
        *y += bh - HUD_BAR_H;
    }
    on_surface(x, y);
}


/* ---- the rect, and the one translation -----------------------------------

   MAKE THE TWO RECTANGLES THE SAME ONE. gui-renderer.md 22.6: the engine has
   at least four places that assume its viewport IS what the player looks at --
   the eye clamp 0x41C3C0, the centre-on 0x41C7C0, the per-frame FOLLOW at
   0x41CAF7 and the smooth SetCamera 0x41C4C0 -- and patching them one at a
   time was a losing game. So the viewport is made to BE the visible window,
   and every one of them is then simply right.

   WHAT IS WRITTEN, and what is deliberately not:

     L, T   NOT WRITTEN. 0x80 / 0x20 are baked as immediates into every site
            that projects world -> screen (exe map, "The world->screen
            projection is NOT derived from the viewport rect"), so moving them
            tears the world in two -- 22.5, and the whole reason the first
            build was withdrawn.
     R, B   the far edges pulled in by what the HUD covers.
     viewW  R - L + 1, and viewH B - T + 1, so every clamp and every centre the
            engine computes is about the window the player can actually see.

   THE TRANSLATION is what pays for it: the engine now draws the world into
   [128, R] x [32, B] of its own surface, and that block belongs on screen at
   [128s, W-1] x [32s, H-1-32s]. One vector, (128s - 128, 32s - 32), applied in
   the three places we own -- the world layer's viewport, the composite's
   world-region sampling, and the pointer map's world branch. At stock it is
   (0, 0) and not one of the three does anything.

   AT STOCK NOT ONE BYTE IS WRITTEN either: R = W-1, B = H-33, viewW = W-128
   and viewH = H-64 are exactly what 0x497F40 just built, so the s = 1 frame is
   the engine's own and the parity gate cannot move because of this file.

   GAME-ENTRY-TIME AGAIN, and this is the cost of the change: LoadMap's
   derivations and the SORT allocations are sized from viewW/viewH, and the
   loader thread is not created until 0x4982CA, so the rect has to be in place
   before then. The observer is back on 0x4288D0 gated on the return address
   0x498242 -- the call at 0x49823D, which is the first one after the last
   store of the rect. A scale chosen mid-game therefore waits for the next
   game, which is what 22.2 said before the live version briefly replaced it. */
#define VA_LOADBG   0x004288D0u
#define SITE_RET    0x00498242u
static const unsigned char LOADBG_STOLEN[7] = { 0x83,0xEC,0x30, 0x8B,0x44,0x24,0x38 };

#define OFF_SCREEN_W  0x37E1F
#define OFF_SCREEN_H  0x37E23
#define OFF_VP_R      0x37E2F
#define OFF_VP_B      0x37E33
#define OFF_VIEW_W    0x37E37
#define OFF_VIEW_H    0x37E3B

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

/* The vector that takes a point in the ENGINE's surface to the same point on
   the screen, inside the world region. 1 when it is not (0,0). */
int tagpu_hud_shift(int* dx, int* dy)
{
    int pw, bh;
    if (!tagpu_hud_live(&pw, &bh, NULL)) { if (dx) *dx = 0; if (dy) *dy = 0; return 0; }
    if (dx) *dx = pw - HUD_PANEL_W;
    if (dy) *dy = bh - HUD_BAR_H;
    return 1;
}


/* The insets `tagpu_vpwide.c` derives the TRUE rect from. L and T are the
   engine's own 0x80/0x20 and never move -- the projection bakes them (22.5) --
   but the far edges come in by what the HUD covers, so that the rect this
   fork reasons about is the same rect the engine's own fields now hold.
   ONCE horizontally and TWICE vertically, for the reason apply_rect gives. */
void tagpu_hud_true_inset(const char* ta, int* L, int* T, int* rInset, int* bInset)
{
    int pw = HUD_PANEL_W, bh = HUD_BAR_H, w, h;
    if (ptr_ok(ta)) {
        w = *(const int*)(ta + OFF_SCREEN_W);
        h = *(const int*)(ta + OFF_SCREEN_H);
        tagpu_hud_geom(w, h, (int)s_pctLive, NULL, &pw, &bh);
    }
    if (L)      *L      = HUD_PANEL_W;
    if (T)      *T      = HUD_BAR_H;
    if (rInset) *rInset = 1  + (pw - HUD_PANEL_W);
    if (bInset) *bInset = 33 + 2 * (bh - HUD_BAR_H);
}

static void apply_rect(void)
{
    char* ta = *(char**)TA_MAINPP;
    char b[220];
    int W, H, q8 = 256, pw, bh, pct;

    pct = tagpu_hud_stored_pct();
    InterlockedExchange(&s_pctLive, pct);   /* latched: this game keeps this scale */
    if (!ptr_ok(ta)) return;
    W = *(int*)(ta + OFF_SCREEN_W);
    H = *(int*)(ta + OFF_SCREEN_H);
    tagpu_hud_geom(W, H, pct, &q8, &pw, &bh);
    if (q8 <= 256) return;                  /* stock: the engine's own six stores stand */
    *(int*)(ta + OFF_VP_R)   = W - 1 - (pw - HUD_PANEL_W);
    /* TWICE, unlike R: the panel is only on the left, so the width loses one
       inset, but a bar is covered at BOTH ends while T stays at 0x20. */
    *(int*)(ta + OFF_VP_B)   = H - 33 - 2 * (bh - HUD_BAR_H);
    *(int*)(ta + OFF_VIEW_W) = W - pw;              /* R - 128 + 1 */
    *(int*)(ta + OFF_VIEW_H) = H - 2 * bh;          /* B -  32 + 1 */
    _snprintf(b, sizeof b,
              "hud: scale %d%% (%s, ceiling %d%%) on %dx%d - panel %d, bars %d, "
              "viewport {128,32,%d,%d} %dx%d, world shifted by (%d,%d)",
              q8 * 100 / 256, pct == 0 ? "Auto" : "chosen",
              tagpu_hud_ceiling_pct(W, H), W, H, pw, bh,
              W - 1 - (pw - HUD_PANEL_W), H - 33 - 2 * (bh - HUD_BAR_H),
              W - pw, H - 2 * bh, pw - HUD_PANEL_W, bh - HUD_BAR_H);
    b[sizeof b - 1] = 0;
    hlog(b);
}

static int __cdecl before_loadbg(void* entry_esp)
{
    if (((void**)entry_esp)[0] == (void*)SITE_RET) apply_rect();
    return 0;
}

void tagpu_hud_init(void)
{
    int pct = tagpu_hud_stored_pct();
    int armed;
    char b[220];
    InterlockedExchange(&s_pctLive, pct);
    armed = tagpu_detour_bytes_ok(VA_LOADBG, LOADBG_STOLEN, sizeof LOADBG_STOLEN) &&
            tagpu_detour_observe(VA_LOADBG, LOADBG_STOLEN, sizeof LOADBG_STOLEN,
                                 before_loadbg, NULL);
    _snprintf(b, sizeof b,
              "hud: %s - the viewport IS the visible window (observer on 0x4288D0, "
              "site 0x49823D: %s); stored %d",
              pct < 0 ? "off" : "ARMED",
              armed ? "ok" : "NOT armed - engine bytes differ", pct);
    b[sizeof b - 1] = 0;
    hlog(b);
    if (!armed) InterlockedExchange(&s_pctLive, -1);
}
