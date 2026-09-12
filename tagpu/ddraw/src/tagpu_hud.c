/* tagpu_hud.c — HUD scale. See tagpu_hud.h for what it is and what makes the
   one word that crosses threads safe; research/notes/gui-renderer.md §20 for
   why the space is reserved rather than overlaid and why the art is magnified
   rather than re-laid-out. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"
#include "tagpu_hud.h"
#include "tagpu_opt.h"
#include "tagpu_detour.h"

#define TA_MAINPP     0x00511DE8u

/* the engine's own screen dimensions, written at game entry from the Screen
   Size row's fields (+0x37F1B/+0x37F1F) and never by us */
#define OFF_SCREEN_W  0x37E1F
#define OFF_SCREEN_H  0x37E23
/* the six ints of the viewport rect, in the order 0x497F40 writes them */
#define OFF_VP_L      0x37E27
#define OFF_VP_T      0x37E2B
#define OFF_VP_R      0x37E2F
#define OFF_VP_B      0x37E33
#define OFF_VIEW_W    0x37E37
#define OFF_VIEW_H    0x37E3B

/* The panel's logical width and the bars' logical height at stock scale: the
   two immediates at 0x4981C9 / 0x4981D9, and the 128x480 block §3.4a measured.
   PANEL_ROWS is what makes H/480 the ceiling. */
#define HUD_PANEL_W   128
#define HUD_BAR_H     32
#define HUD_PANEL_ROWS 480
/* The world keeps at least this much width whatever the ceiling says. H/480 is
   the ceiling for every aspect a player can choose, but a tall, narrow surface
   would drive 128s past the screen and hand the engine a negative viewport —
   a bound, so that cannot be a thing that happens rather than a thing that is
   unlikely. */
#define HUD_MIN_VIEW_W 256

#define LEVER "tagpu_hud.on"
#define LEVER_OFF "tagpu_hud.off"

/* THE ONE WORD THAT CROSSES THREADS (tagpu_hud.h). -1 = the pass is off, 0 =
   Auto, else a percentage. Written only by the game-entry observer, on the
   game thread; read by the render thread and the message thread. Every reader
   re-resolves it against the screen IT sees, so either value a racing read can
   return is a value that fits that screen. */
static volatile LONG s_pctLive = -1;

static void hlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

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
   bug tagpu_menu.c's write_levers() documents at length. */
void tagpu_hud_store_pct(int pct)
{
    HANDLE h;
    char body[64];
    DWORD wrote = 0;
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
}

void tagpu_hud_true_inset(const char* ta, int* L, int* T, int* rInset, int* bInset)
{
    int pw = HUD_PANEL_W, bh = HUD_BAR_H, w, h;
    if (ptr_ok(ta)) {
        w = *(const int*)(ta + OFF_SCREEN_W);
        h = *(const int*)(ta + OFF_SCREEN_H);
        tagpu_hud_geom(w, h, (int)s_pctLive, NULL, &pw, &bh);
    }
    if (L)      *L      = pw;
    if (T)      *T      = bh;
    if (rInset) *rInset = 1;
    if (bInset) *bInset = bh + 1;
}

/* ---- the game-entry observer -------------------------------------------- */

/* WHY HERE AND NOT ON THE IMMEDIATES. 0x4981C9 and 0x4981D9 carry the panel
   and bar constants as imm32 and would patch, but the bottom inset at 0x498200
   is `sub ecx,0x21` — a SIGN-EXTENDED imm8, so it caps at 127 and with it the
   scale at 3.94. The 4K ceiling is 4.5. Widening it to `81 e9 imm32` needs
   three bytes the next instruction owns. So the engine computes its rect as it
   always did and we write all six fields over it, which has no encoding
   ceiling and leaves viewW/viewH ours too, so nothing downstream can disagree
   with the rect it was derived from.

   WHY THIS CALL. 0x49823D is the first call after the last store of the rect
   (0x498237), and the loader thread is not created until 0x4982CA — so LoadMap
   and the SORT allocations, which size themselves from these fields, see ours.
   That ordering is the whole reason the setting is game-entry-time. The
   observer watches 0x4288D0 — a background-picture loader with 42 call sites —
   and acts only when its return address is this one, which identifies the site
   exactly rather than nearly. */
#define VA_LOADBG   0x004288D0u
#define SITE_RET    0x00498242u
static const unsigned char LOADBG_STOLEN[7] = { 0x83,0xEC,0x30, 0x8B,0x44,0x24,0x38 };

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
    /* AT STOCK NOT ONE BYTE IS WRITTEN. The engine's own six stores stand, so
       the s == 1 frame is the engine's frame and the parity md5 cannot move
       because of anything in this file. */
    if (q8 <= 256) return;
    *(int*)(ta + OFF_VP_L)   = pw;
    *(int*)(ta + OFF_VP_T)   = bh;
    *(int*)(ta + OFF_VP_R)   = W - 1;
    *(int*)(ta + OFF_VP_B)   = H - bh - 1;
    *(int*)(ta + OFF_VIEW_W) = W - pw;
    *(int*)(ta + OFF_VIEW_H) = H - 2 * bh;
    _snprintf(b, sizeof b,
              "hud: scale %d%% (%s, ceiling %d%%) on %dx%d - panel %d, bars %d, "
              "viewport {%d,%d,%d,%d} %dx%d",
              q8 * 100 / 256, pct == 0 ? "Auto" : "chosen",
              tagpu_hud_ceiling_pct(W, H), W, H, pw, bh,
              pw, bh, W - 1, H - bh - 1, W - pw, H - 2 * bh);
    b[sizeof b - 1] = 0;
    hlog(b);
}

static int __cdecl before_loadbg(void* entry_esp)
{
    if (((void**)entry_esp)[0] == (void*)SITE_RET) apply_rect();
    return 0;                                   /* nothing wanted on the way out */
}

void tagpu_hud_init(void)
{
    int armed =
        tagpu_detour_bytes_ok(VA_LOADBG, LOADBG_STOLEN, sizeof LOADBG_STOLEN) &&
        tagpu_detour_observe(VA_LOADBG, LOADBG_STOLEN, sizeof LOADBG_STOLEN,
                             before_loadbg, NULL);
    char b[160];
    _snprintf(b, sizeof b,
              "hud: %s - the viewport rect at game entry (observer on 0x4288D0, site 0x49823D); "
              "stored %d",
              armed ? "ARMED" : "NOT armed - engine bytes differ at 0x4288D0",
              tagpu_hud_stored_pct());
    b[sizeof b - 1] = 0;
    hlog(b);
    if (!armed) InterlockedExchange(&s_pctLive, -1);
}
