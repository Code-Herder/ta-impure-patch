/* tagpu_menu.c -- the render-options screen (Phase F). API: tagpu_menu.h.

   THE ENGINE FACTS THIS RESTS ON, all read off pristine/TotalA.exe.pristine and
   written up in exe-reverse-engineering.md "The screen lifecycle":

   - `GUI_Load 0x4AA8F0(gi, name, flags)` -- __stdcall, ret 0xC -- turns `name`
     into the FILE PATH `<prefix at gi+0x9B6><name>.GUI`, and the prefix is
     "guis\" (set at 0x4914CE). The exe's screen-name table is never consulted,
     so a name we invent loads if the file exists.
   - It also STAMPS the name into `ControlsAry[0].name` (0x4AAC98:
     `strncpy(ctrls+2, name, 16)`), which is what `GUICONTROL_IsOnTop 0x4AB060`
     compares -- so the `name=` authored in the file is irrelevant, and ours has
     to match the string we leave in `main+0x37EA0`.
   - `flags & 0x400` suppresses GUI_Load's own closing repaint (0x4AACB5), which
     is what lets us fix the panel's xpos BEFORE the screen is first drawn. The
     rect has to be patched at runtime because the panel is RIGHT-ALIGNED and
     the `.GUI` cannot know the resolution.
   - `UpdateIngameGUI 0x491D70` pops until `IsOnTop(gi, main+0x37EA0)`, at 21
     call sites -- so anything pushed over the world is popped again unless that
     buffer names it. We write the buffer, load that same string, and CLOSE by
     restoring the buffer and letting the engine pop us. We never call GUI_Pop.
   - `gi+0xCCA` is the screen's DEFERRED-REPAINT flag. Twenty-odd
     state-changing GUI calls set it (there are bare accessors at 0x49FA90
     set / 0x49FAB0 clear) and there is exactly
     ONE reader, 0x4AA0AF inside the GUI pump: `if (flag == 1) { flag = 0;
     GUI_StageUpdateDraw(gi, top->flags | 0x40); }`. So it is not a
     precondition of anything -- it is the engine's own way of asking for the
     repaint we would otherwise ask for by hand, and it repaints with the
     SCREEN'S flags rather than a bare 0x40. We set it and let the pump draw.
   - `GUIGADGET_SetStatus 0x4A1080(gi, name, value)` -- __stdcall, ret 0xC --
     is a name scan plus `mov [rec+0x137],cl`, no clamp, no callback, no
     redraw.
     THE ENGINE DOES ADVANCE THE CLICKED ROW ITSELF -- 0x4A6EC8, 0x4A9DB6 and
     0x4AA377 all `inc` +0x137, the last wrapping against the count at +0x136
     and skipped when `grayedout` bit 0 is set (0x4AA36A). So OnCommand
     advances OUR MODEL and push_stages re-writes EVERY row, which overwrites
     what the engine did. Do not also advance the gadget field, and do not drop
     the re-push: either way every click would move two stages.

   WHERE THE WORK RUNS. `before_update()` is an observer on DrawGameScreen
   0x468CF0 -- not UpdateIngameGUI 0x491D70, which starves, because all 21 of
   its call sites are transition handlers rather than the frame loop. So
   it is on the game thread and runs immediately before the engine reconsiders
   the GUI stack -- the one moment at which pushing a screen cannot race the
   pop loop. `OnCommand` is the engine calling us, also on the game thread, and
   it does NOT write the file: it records the value in the settings store and
   the store is written from the render thread in tagpu_menu_present(). */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tagpu_log.h"
#include "tagpu_restore_guard.h"

#include "dd.h"
#include "config.h"
#include "utils.h"
#include "hook.h"
#include "tagpu_detour.h"
#include "tagpu_classicpp.h"
#include "tagpu_ufo.h"
#include "tagpu_gaf.h"
#include "tagpu_menu.h"
#include "tagpu_hud.h"
#include "tagpu_vk.h"
#include "tagpu_settings.h"
#include "tagpu_opt.h"

/* the engine's Visuals options: the section "owned by the store" below */
static void eng_push(int what);
static int  eng_owned(void);
static int  eng_shadows_bit(void);
static void eng_set_shadows(int on);
enum { ENG_GAMMA = 1, ENG_BITS = 2, ENG_RES = 4, ENG_ALL = 7 };
#define OFF_OPTWORD       0x37F06u      /* WORD                                 */
#define OFF_GAMMA         0x37F08u      /* signed dword, 0..20                  */
#define OFF_INGAME        0x2A44u       /* & 4: a game is in progress           */
#define OFF_BAKEHEAP      0x1437Bu      /* this for the re-bake 0x437C80        */
#define OFF_SOUND         0x10u         /* the sound object                     */
#define SND_MIXING        0x2Cu         /* its MixingBuffers, set by 0x4CF210   */
#define OPT_AA            0x0002u
#define OPT_SHADOWS       0x001Cu       /* Shadow | TShadow | FShadow           */
#define OPT_SHADING       0x0020u
#define OPT_DITHERFOG     0x0040u
#define VA_OPT_LOAD       0x0042F9A0u   /* registry options load, void          */
#define VA_OPT_STASHBACK  0x0045CAE0u   /* the stash -> main, void              */
#define VA_SETGAMMA       0x004BA590u   /* stdcall(float factor), ret 4         */
#define VA_REBAKE         0x00437C80u   /* thiscall, this = [main+0x1437B]      */
#define VA_GAMMA_CB       0x0045BD20u   /* GAMMA's slider callback, stdcall ret 8 */
#define VA_VIDSLDR_CB     0x0045BBF0u   /* VIDSLDR's, the same shape            */
#define G_SLIDER_CB       0x144u        /* a gadget's slider callback [DISASSEMBLED 0x4A42DF] */

/* ---- the engine ---------------------------------------------------------- */
#define TA_MAIN         0x00511DE8u     /* TAdynmemStruct**                    */
#define OFF_GUIINFO         0x519u      /* main + this = GUIInfo               */
#define OFF_TOPGUI          0x531u      /* main + this = gi->TheActive_GUIMEM  */
#define OFF_EXPECT        0x37EA0u      /* the screen the engine keeps on top  */
#define OFF_SELW          0x37F1Bu      /* the Screen Size row's own w and h,  */
#define OFF_SELH          0x37F1Fu      /* written by VIDSLDR's callback 0x45BBF0 */
#define VA_GUI_LOAD     0x004AA8F0u
#define VA_SETSTATUS    0x004A1080u
#define VA_SETGRAYED    0x004A1250u     /* (gi, name, grayed): see push_stages */
#define VA_STAGEDRAW    0x004A81E0u
#define VA_SETDIRTY     0x0049FA90u     /* gi+0xCCA = 1: repaint at the pump   */
#define VA_ACTDONE      0x004AB0A0u     /* gi->UIChange_f = -1: see menu_accept */
#define VA_DRAWLOCK     0x004C2470u     /* the counted pair GUI_Load draws under */
#define VA_DRAWUNLOCK   0x004C2870u
#define VA_UPDGUI       0x00491D70u
#define VA_GUI_POP      0x004A9660u     /* GUI_Pop(gi): also answers the pump  */
#define VA_DRAWSCREEN   0x00468CF0u     /* the per-frame game-thread function  */
#define VA_POSTGUI      0x0046A308u     /* DrawGameScreen, just past the GUI   */
#define VA_GAFBLIT      0x004B7F90u     /* CopyGafToContext(ctx, frame, x, y)  */
#define VA_FILEOPEN     0x004BBC40u     /* the engine's own path open          */
#define VA_GAFLOAD      0x004B8C60u     /* -> a bank                           */
#define VA_GAFFIND      0x004B8D40u     /* (bank, name) -> entry, or NULL      */
#define P_GAFBANK       0xC0            /* the panel's own loaded bank         */

#define GM_CTRLS  0x04
#define GM_ONCMD  0x08
#define GM_CTX    0x0C
#define G_NAME    0x02
#define G_XPOS    0x13
#define G_YPOS    0x15
#define G_STATUS  0x137                 /* status_curnt                        */
#define G_GRAYED  0x13C
#define GI_UICHANGE 0x60                /* gi+0x60 = main+0x579                */
#define STRIDE    0x15B

typedef void* (__stdcall *gui_load_fn)(void* gi, const char* name, int flags);
typedef int   (__stdcall *set_status_fn)(void* gi, const char* name, int value);
typedef int   (__stdcall *set_grayed_fn)(void* gi, const char* name, int grayed);
typedef int   (__stdcall *stage_draw_fn)(void* gi, int flags);
typedef void  (__stdcall *set_dirty_fn)(void* gi);
typedef void  (__stdcall *act_done_fn)(void* gi);
typedef int   (__stdcall *upd_gui_fn)(int);
typedef void  (__stdcall *gui_pop_fn)(void* gi);
typedef void  (__cdecl   *lock_fn)(void);
typedef void  (__stdcall *gaf_blit_fn)(void* ctx, const void* frame, int x, int y);
typedef int   (__stdcall *file_open_fn)(const char* path);
typedef void* (__stdcall *gaf_load_fn)(const char* path);
typedef void* (__stdcall *gaf_find_fn)(void* bank, const char* name);

/* DrawGameScreen's prologue: `sub esp,0x214`.

   THE TICK IS HERE AND NOT ON UpdateIngameGUI. UpdateIngameGUI has 21 call
   sites and NONE of them is the frame loop -- they
   are transition and teardown handlers (0x460630 calls the level teardown
   0x491B60 first) -- so an observer there is called on GUI events only, and a
   poll hung off it never runs. Measured: with the tick there, creating the
   trigger file did nothing at all and the module logged nothing, because
   `before` was never entered while the game just ran. DrawGameScreen 0x468CF0
   (tools/ta_symbols.txt, 4 call sites) is THE per-frame game-thread function,
   and its entry is before any of the frame's drawing -- so a screen pushed
   here is drawn by the same frame and nothing is done mid-composite. */
static const unsigned char DRAW_STOLEN[6] = { 0x81, 0xEC, 0x14, 0x02, 0x00, 0x00 };
/* 0x46A308: `mov edx,ds:0x511de8`, immediately after DrawGameScreen's own GUI
   draw (0x46A303 calls 0x4AB170) and before the flip. That is where the
   trigger goes: over the finished bar, through the engine's own blitter, so
   it lands in the back buffer every flip presents -- rather than in a layer
   of our own that the engine's surface and `tacli shot` would both miss. */
static const unsigned char POST_STOLEN[6] = { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 };

/* ---- the geometry, from tools/guipanel.py (the source of truth) ---------- */
#define PANEL_W   304
/* NINE rows: ROW_Y0 34 + ROW_PITCH 28 * 8 = 258, and the row is ROW_H 20, so
   the last one ends at 278; the bottom rule sits 8 px under it and the panel
   ends 10 px under the rule (DIV_BOT). One define carries
   it: the GAF frame header (`FRMOFF + 0x02` below), `s_ground`, the ramp, the
   border and the corner bolts are all sized from it, and the panel is COMPOSED
   AT RUNTIME from the player's install rather than shipped, so no art is
   regenerated. tools/guipanel.py says 304x212 -- it is the lab's copy of this
   layout, not its source. */
#define PANEL_H   296
#define MARGIN     16                   /* the panel and the trigger share it  */
#define BAR_H      32                   /* the top bar: rows 0..31             */
#define ROW_Y0     34
#define ROW_PITCH  28
#define ROW_H      20
#define CTL_X     166
#define CTL_W     120
#define LBL_X      14
#define LBL_W     144
#define TITLE_Y     9                   /* the caption, above the rule at 30   */
#define TITLE_W   276
#define TRIG       28                   /* 2 px of bar above and below         */

/* ---- the rows ------------------------------------------------------------ */
/* Nine, and every one of them live. There is no mouse-wheel zoom row:
   tagpu_zoom_init() installs byte patches once at attach, so the row would
   light green and change no pixel until the next launch. The
   menu keeps the invariant that NO ROW NEEDS A RESTART (renderers.md 2.10), and
   the FPS counter honours it: tagpu_fps.c polls its trigger on the render
   thread and tagpu_vk_fps.c builds its pipeline on first use. Vsync does too:
   the render thread reads the store every frame and the lane rebuilds its
   swapchain when the answer changes (render_vk.c). */
enum { R_STYLE, R_ASSETS, R_LIGHT, R_SHADOWS, R_SHADOWQ, R_SS, R_FPS, R_VSYNC, R_EDGE,
       R_COUNT };

typedef struct {
    const char* name;                   /* the gadget name in the .GUI         */
    const char* label;                  /* its id=5 label, on the SAME line     */
    const char* text;                   /* pipe-separated stage labels          */
    int         stages;
} Row;

static const Row s_row[R_COUNT] = {
    { "STYLE",   "Renderer",          "Classic|Classic++|Custom", 3 },
    /* the third stage is On as the player chose it and Off because the
       restorer turned itself off on this driver (tagpu_restore_guard.h); a
       click on it is the retry (tagpu_menu_oncommand) */
    { "ASSETS",  "Undithered assets", "Off|On|Off (driver)",      3 },
    { "LIGHT",   "Dynamic lighting",  "Off|On",                   2 },
    { "SHADOWS", "Shadows",           "Off|Hard",                 2 },
    { "SHADOWQ", "Shadow quality",    "Low|Med|High|Ultra",       4 },
    { "SS",      "Supersampling",     "Off|2x",                   2 },
    { "FPS",     "FPS counter",       "Off|On",                   2 },
    /* FIFO or IMMEDIATE present (tagpu_vk.c) and the frame limiter's one cap
       (fps_limiter.c); the front-end screen shows it in the WINDOW column
       (build_visuals_gui) */
    { "VSYNC",   "Vsync",             "Off|On",                   2 },
    /* what the view shows past the map: the world reflected, or the engine's
       black (bar-camera-port.md 1.2) */
    { "EDGE",    "Map edge",          "Black|Mirror",             2 },
};

/* Renderer stages. Custom is DERIVED, never clicked into: clicking the row
   alternates Classic and Classic++, and touching any row below makes it read
   Custom (renderers.md 2.10). */
enum { STYLE_CLASSIC, STYLE_PP, STYLE_CUSTOM };

/* Shadows: the UI order and the cfg's are not the same number, because the
   cfg's is 0 none / 1 SOFT / 2 HARD and the row offers them in the order a
   player reads them in. This table is the mapping.

   THERE IS NO `Soft` STAGE, AND THAT IS HONESTY RATHER THAN A CHOICE.
   `shadows=1` is the map-anchored depth map, which has no producer
   (tagpu_vk_shadow.h), so the stage would be Off wearing another name. It
   comes back the day a producer does, and `shadows=1` in a hand-written cfg is
   still read -- it simply draws nothing, and `read_state` below then shows
   this row as Off, which is what the player is actually getting.
   [tagpu_classicpp.c's `shadow_defaults`.] */
static const int SHADOW_VAL[2] = { TAGPU_SHADOWS_OFF, TAGPU_SHADOWS_HARD };
#define SHADOWS_HARD_STAGE 1
/* Shadow quality -> shadowres=, whose own range is 256..4096 (tagpu_classicpp.c).
   IT SIZES THE SOFT MAP AND NOTHING ELSE, so with no soft map it changes no
   pixel and `row_greyed` greys it. */
static const int SHADOWQ_VAL[4] = { 512, 1024, 2048, 4096 };

/* ---- the panel's own art -------------------------------------------------
   The ground is a GAF the engine loads by itself -- but NOT through the id=12
   gadget. The StageUpdateDraw dispatch (`jmp [eax*4+0x4A95F4]`, indexed by id)
   sends only id 0 and id 11 to the branch at 0x4A84F2 that builds
   `<prefix at gi+0xAB6 = "anims\\"><gadget name>.GAF` and loads it; id 12 goes
   to 0x4A8ACA and looks its frame up in a bank it did not load. THE PANEL IS
   THE LOADER: id 0's own name is the one GUI_Load stamped -- the screen name --
   and the extension setter turns it into `anims\\<screen>.GAF`, whose frames an
   id=12 then names.

   The stock corpus says exactly this and settles it: ARMOPT.GUI's id=12 is
   named OPTBG and anims/armopt.gaf holds one entry, OPTBG. PREFS.GUI's id=12
   is IGOPT, which is in the SHARED commongui bank, while its own prefs.gaf
   holds PREFSBG. VISUALRT.GUI's id=12 is VISUALSRT and there is no
   anims/visualrt.gaf at all -- commongui again. So a screen's own GAF is
   optional and the shared bank is the fallback, which is why the archive
   carries anims/render.gaf (the screen is RENDER.GUI) holding one frame named
   RENDERDD, and the .GUI's id=12 is named RENDERDD. No surgery on the bank at
   gi+0x04 is needed, and none is done.

   IT IS DRAWN, NOT SAMPLED, AND IT IS DRAWN IN PALETTE INDICES. A GAF frame is
   8bpp, and the DLL has no palette at DLL_PROCESS_ATTACH -- so quantising an
   RGB design at write time is not available. It does not need to be: TA's
   palette 55..63 is a dark warm ramp and tools/guipanel.py's colours were
   chosen against it (its GROUND_LO (46,40,29) is index 60 (47,43,27), its
   RECESS_LO (23,20,14) is 62 (23,19,15), and so on), so the drawn panel is
   expressible as nine indices with no colour of the game's in it. */
#define ART_NAME   "RENDERDD"
#define UFO_FILE   "impure-patch.ufo"
#define SCREEN     "RENDER.GUI"
#define ON_FILE    "tagpu_menu.on"
#define OFF_FILE   "tagpu_menu.off"
#define OPEN_FILE  "tagpu_menu.open"    /* the spike's stand-in for the trigger */
/* The levers over the store's rows (tagpu_settings.h): a row one of these
   holds is greyed, showing the lever's value. */
#define SS_OFF     "tagpu_ss.off"
#define FPS_ON     "tagpu_fps.on"
#define CPP_ON     "tagpu_classicpp.on"
#define CPP_OFF    "tagpu_classicpp.off"
#define EDGE_ON    "tagpu_mirror.on"
#define EDGE_OFF   "tagpu_mirror.off"
#define POLL_MS    250

/* The trigger's ink walks the SAME ramp, re-hung around a lighter ground than
   the Cavedog logo's because the bar's own texture is index 62 and the logo's
   ink is 52 % that value -- laid on the bar it would be invisible. The body
   goes below the bar (63) and the outline above it. Three states, and the
   change is a slide along the ramp rather than a second picture, which is
   what the logo does (renderers.md 2.10). */
enum { TS_NORMAL, TS_OVER, TS_PRESSED, TS_COUNT };
static const unsigned char TRIG_INK[TS_COUNT][3] = {
    { 59, 61, 63 },                     /* outline, inner shade, core          */
    { 57, 60, 63 },
    { 60, 62, 63 },
};
/* Bumped whenever the generated .GUI changes, so a stale archive beside a new
   DLL is impossible: the archive is rewritten every launch anyway, and this is
   what says so in the log. */
#define UFO_STAMP  "G21e-1"

static int    s_installed;
static int    s_nrows = R_COUNT;
static void*  s_gm;                     /* our GUIMEMSTRUCT while open, else 0 */
static char   s_saved[16];              /* what main+0x37EA0 held              */
static int    s_stage[R_COUNT];
static volatile LONG s_vkDirty;         /* the GPU row moved: the store's gpu= */
static DWORD  s_lastPoll;
static int    s_want;
static int    s_fresh;                  /* the next open is the PLAYER'S open  */
static int    s_fileWas;                /* the trigger file, sampled on edges  */
static int    s_pressed;                /* the sprocket is held down           */
static int    s_drawTrigger;            /* the post-GUI observer is installed  */
static unsigned char s_trigPix[TS_COUNT][TRIG * TRIG];
static unsigned char s_trigFrame[TS_COUNT][0x18];

static void mlog(const char* m)
{
    tagpu_log(m);
}

static int exists(const char* p)
{
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

/* The nine indices, and what tools/guipanel.py calls each of them. */
#define IX_EDGE          63     /* the outer keyline, and a recess's sunken lip */
#define IX_BEVEL_HI      55
#define IX_BEVEL_LO      59
#define IX_GROUND_HI     58     /* the face, lit end of the vertical ramp       */
#define IX_GROUND_MID    59
#define IX_GROUND_LO     60     /* ...and its shaded end                        */
#define IX_RECESS_HI     61
#define IX_RECESS_LO     62
#define IX_RECESS_LIGHT  56     /* a recess's lit bottom/right lip              */
#define IX_BOLT_HI       55
#define IX_BOLT_LO       59
#define IX_KEY            0     /* the frame's transparency index -- never drawn */

#define DIV_TOP    30
#define DIV_BOT   286           /* under the last row: 278 + 8 */
#define PAD         2

/* THE FRAMES ARE NOT ALL ONE SIZE, so the pixel helpers carry their
   surface rather than reading PANEL_W. The front-end screen's recesses are
   small frames of their own (see `draw_recess_frame`), and a helper that
   silently strides by 304 would shear every one of them. */
typedef struct { unsigned char* p; int w, h; } Surf;

static void px(Surf* s, int x, int y, unsigned char v)
{
    if (x >= 0 && x < s->w && y >= 0 && y < s->h) s->p[y * s->w + x] = v;
}

static void hline(Surf* s, int x0, int x1, int y, unsigned char v)
{
    for (; x0 <= x1; x0++) px(s, x0, y, v);
}

static void vline(Surf* s, int x, int y0, int y1, unsigned char v)
{
    for (; y0 <= y1; y0++) px(s, x, y0, v);
}

static void fillrect(Surf* s, int x, int y, int w, int h, unsigned char v)
{
    int i, j;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) px(s, x + i, y + j, v);
}

/* A sunken band: dark lip above and left, lit lip below and right -- guipanel's
   recess(), which is what every stock runtime panel paints and what makes a
   stagebuttn plate sit IN the panel rather than on it. */
static void recess(Surf* s, int x, int y, int w, int h)
{
    int j;
    for (j = 1; j < h - 1; j++)
        fillrect(s, x + 1, y + j, w - 2, 1,
                 (unsigned char)(j * 2 < h ? IX_RECESS_HI : IX_RECESS_LO));
    hline(s, x, x + w - 1, y, IX_EDGE);
    vline(s, x, y, y + h - 1, IX_EDGE);
    hline(s, x, x + w - 1, y + h - 1, IX_RECESS_LIGHT);
    vline(s, x + w - 1, y, y + h - 1, IX_RECESS_LIGHT);
}

static void bolt(Surf* s, int cx, int cy)
{
    int i, j;
    for (j = -3; j <= 3; j++)
        for (i = -3; i <= 3; i++) {
            int r2 = i * i + j * j;
            if (r2 <= 9)  px(s, cx + i, cy + j, IX_BOLT_LO);
            if (r2 <= 2)  px(s, cx + i, cy + j, IX_BOLT_HI);
        }
}

static void divider(Surf* s, int y)
{
    hline(s, 10, s->w - 11, y,     IX_EDGE);
    hline(s, 10, s->w - 11, y + 1, IX_BEVEL_HI);
}

/* tools/guipanel.py draw_panel(), in indices: the face's vertical ramp, the
   black keyline and its top-left-lit bevel, four bolts, the two rules, and one
   recess per row. */
static void draw_panel(unsigned char* f, int rows)
{
    Surf s; int y, i;
    s.p = f; s.w = PANEL_W; s.h = PANEL_H;

    for (y = 0; y < PANEL_H; y++) {
        int t = y * 3 / PANEL_H;            /* three steps is all the ramp has */
        fillrect(&s, 0, y, PANEL_W, 1,
                 (unsigned char)(t == 0 ? IX_GROUND_HI :
                                 t == 1 ? IX_GROUND_MID : IX_GROUND_LO));
    }

    hline(&s, 0, PANEL_W - 1, 0, IX_EDGE);
    hline(&s, 0, PANEL_W - 1, PANEL_H - 1, IX_EDGE);
    vline(&s, 0, 0, PANEL_H - 1, IX_EDGE);
    vline(&s, PANEL_W - 1, 0, PANEL_H - 1, IX_EDGE);
    hline(&s, 1, PANEL_W - 2, 1, IX_EDGE);
    hline(&s, 1, PANEL_W - 2, PANEL_H - 2, IX_EDGE);
    vline(&s, 1, 1, PANEL_H - 2, IX_EDGE);
    vline(&s, PANEL_W - 2, 1, PANEL_H - 2, IX_EDGE);
    hline(&s, 2, PANEL_W - 3, 2, IX_BEVEL_HI);
    vline(&s, 2, 2, PANEL_H - 3, IX_BEVEL_HI);
    hline(&s, 3, PANEL_W - 3, PANEL_H - 3, IX_BEVEL_LO);
    vline(&s, PANEL_W - 3, 3, PANEL_H - 3, IX_BEVEL_LO);

    bolt(&s, 9, 9);
    bolt(&s, PANEL_W - 10, 9);
    bolt(&s, 9, PANEL_H - 10);
    bolt(&s, PANEL_W - 10, PANEL_H - 10);

    divider(&s, DIV_TOP);
    divider(&s, DIV_BOT);

    for (i = 0; i < rows; i++) {
        y = ROW_Y0 + ROW_PITCH * i;
        recess(&s, CTL_X - PAD, y - PAD, CTL_W + 2 * PAD, ROW_H + 2 * PAD);
    }
}

/* ---- the front-end screen's own art ------------------------------------
   THE STOCK BACKGROUND CANNOT BE REUSED FOR TWO COLUMNS. STARTOPT's
   background PCX paints one column of recess bars, centred on the stock
   gadget column at x = 278, and it is the game's art -- we do not ship it and
   we cannot repaint it. Moving the stock controls left to make room therefore
   takes them OUT of their recesses and leaves our own column sitting on bare
   background: two columns of plates floating over a panel drawn for one.

   So the recesses come from us. These are small frames placed by an `id=12`
   gadget per row -- one frame per SIZE, reused by every row of that size,
   rather than one big slab -- so the stock background still shows everywhere
   between them and nothing of the game's art is touched or needed. */
/* One entry, one uncompressed frame (file-formats.md 3). Uncompressed is not
   laziness: the DLL repaints this plane in place at screen-load time, and a
   flat w*h copy is what makes that a memcpy rather than a re-encode. */
/* ONE ARCHIVE, SEVERAL ENTRIES. The GAF carries the in-game panel and the
   front-end screen's recess and rule frames, each a one-frame entry:

     header   u32 sig | u32 entries | u32 pad | u32 entryOffset[entries]
     entry    u16 frames | u32 sig | pad | char name[32]          (0x28)
     table    u32 frameHeaderOffset | u32 flag                    (8, per frame)
     frame    u16 w | u16 h | ... | u8 key | u8 raw | u32 pixels   (0x18)

   The frame table follows its entry immediately; with several entries that has
   to be laid out rather than assumed. */
typedef struct {
    const char* name;
    int         w, h;
    void      (*draw)(unsigned char* f, int w, int h);
} GafEnt;

static int s_panelRows;                 /* draw_panel's argument, set by build_gaf */

static void draw_panel_frame(unsigned char* f, int w, int h)
{
    (void)w; (void)h;
    draw_panel(f, s_panelRows);
}

static unsigned build_gaf(unsigned char* out, unsigned cap,
                          const GafEnt* ent, int n, int rows)
{
    const unsigned HDR = 12 + 4u * (unsigned)n;
    unsigned at = HDR, pix, need = 0;
    int i;
    unsigned v;

    s_panelRows = rows;

    /* the fixed part first, so the pixel offsets are known before anything is
       written into them */
    for (i = 0; i < n; i++) need += 0x28 + 8 + 0x18;
    pix = HDR + need;
    need = pix;
    for (i = 0; i < n; i++) need += (unsigned)ent[i].w * (unsigned)ent[i].h;
    if (cap < need) return 0;
    memset(out, 0, need);

    v = 0x00010100u;      memcpy(out + 0, &v, 4);    /* signature              */
    v = (unsigned)n;      memcpy(out + 4, &v, 4);

    for (i = 0; i < n; i++) {
        unsigned entoff = at, taboff = at + 0x28, frmoff = taboff + 8;
        v = entoff; memcpy(out + 12 + 4 * i, &v, 4);

        *(unsigned short*)(out + entoff + 0) = 1;    /* one frame per entry    */
        v = 1u;     memcpy(out + entoff + 2, &v, 4); /* entry signature        */
        lstrcpynA((char*)out + entoff + 8, ent[i].name, 32);

        v = frmoff; memcpy(out + taboff + 0, &v, 4);
        v = 10u;    memcpy(out + taboff + 4, &v, 4); /* flag 10 = a fixed frame */

        *(unsigned short*)(out + frmoff + 0x00) = (unsigned short)ent[i].w;
        *(unsigned short*)(out + frmoff + 0x02) = (unsigned short)ent[i].h;
        out[frmoff + 0x08] = IX_KEY;                 /* transparency index     */
        out[frmoff + 0x09] = 0;                      /* raw 8bpp               */
        v = pix;    memcpy(out + frmoff + 0x10, &v, 4);

        ent[i].draw(out + pix, ent[i].w, ent[i].h);
        pix += (unsigned)ent[i].w * (unsigned)ent[i].h;
        at = frmoff + 0x18;
    }
    return need;
}

/* ---- the composed ground -------------------------------------------------
   The frame the archive carries is OURS and drawn; the ground the player sees
   is composed at runtime from THEIR OWN install, and the composed pixels never
   leave their machine. `frontend.gaf`'s `back*` nine-slice is the shell's
   mottled panelling -- rather than TA's dialog kit (`diatile` is one colour,
   flat black, in a grey bevel) or a hybrid of the two.

   USE `backtile` FRAME 4, NOT 0: frame 0 carries a lit bottom edge that puts
   seams through a tiled centre (tools/guipanel.py NINE).

   Only the recesses are drawn over it. `text16*` is a BLUE text-field well,
   not a neutral recess, which is why they cannot come from the kit either.

   The bank lookup is the engine's own: 0x4B8D40(bank, name) walks
   bank+0x0C as an array of entry POINTERS, bank+0x04 entries of them, and
   compares entry+0x08 -- read for this. If anything is missing the drawn frame
   is simply left alone, so the failure mode is a plainer panel, not no panel. */
#define KIT_MAX 64                      /* the corner/edge frames are 64x64    */

static const char* const NINE[9] = {
    "backul", "backu",  "backur",
    "backl",  "backtile", "backr",
    "backll", "backbottom", "backlr",
};
static const int NINE_FRAME[9] = { 0, 0, 0, 0, 4, 0, 0, 0, 0 };

typedef struct { unsigned char px[KIT_MAX * KIT_MAX]; int w, h, ok; } Kit;

static void blit_tile(unsigned char* dst, const Kit* k, int x0, int y0, int x1, int y1)
{
    Surf s; int x, y, i, j;
    if (!k->ok || k->w <= 0 || k->h <= 0) return;
    s.p = dst; s.w = PANEL_W; s.h = PANEL_H;    /* the panel ground, always */
    for (y = y0; y < y1; y += k->h)
        for (x = x0; x < x1; x += k->w)
            for (j = 0; j < k->h && y + j < y1; j++)
                for (i = 0; i < k->w && x + i < x1; i++)
                    px(&s, x + i, y + j, k->px[j * k->w + i]);
}

/* The composed ground, built once and kept: our screen is torn down and rebuilt
   on every world click, and decoding nine frames each time would be waste. */
static unsigned char s_ground[PANEL_W * PANEL_H];
static int s_groundOk;

static int compose_ground_once(int rows)
{
    char path[128];
    Kit kit[9];
    void* bank;
    int i, cw, ch;

    if (s_groundOk) return 1;

    lstrcpynA(path, "anims\\frontend.gaf", sizeof path);
    if (!((file_open_fn)VA_FILEOPEN)(path)) return 0;
    bank = ((gaf_load_fn)VA_GAFLOAD)(path);
    if (!bank) return 0;

    for (i = 0; i < 9; i++) {
        const unsigned char* fr;
        void* ent = ((gaf_find_fn)VA_GAFFIND)(bank, NINE[i]);
        kit[i].ok = 0;
        if (!ent) return 0;
        fr = tagpu_gaf_seq_frame((const char*)ent, NINE_FRAME[i]);
        if (!fr) fr = tagpu_gaf_seq_frame((const char*)ent, 0);
        if (!fr) return 0;
        kit[i].w = *(const unsigned short*)(fr + TAGPU_GF_W);
        kit[i].h = *(const unsigned short*)(fr + TAGPU_GF_H);
        if (kit[i].w <= 0 || kit[i].h <= 0 || kit[i].w > KIT_MAX || kit[i].h > KIT_MAX)
            return 0;
        if (!tagpu_gaf_decode(fr, kit[i].w, kit[i].h, kit[i].px)) return 0;
        kit[i].ok = 1;
    }

    cw = kit[0].w;
    ch = kit[0].h;
    if (cw >= PANEL_W || ch >= PANEL_H) return 0;

    memset(s_ground, IX_GROUND_LO, sizeof s_ground);
    blit_tile(s_ground, &kit[4], cw, ch, PANEL_W - cw, PANEL_H - ch);   /* centre */
    blit_tile(s_ground, &kit[1], cw, 0, PANEL_W - cw, ch);              /* edges  */
    blit_tile(s_ground, &kit[7], cw, PANEL_H - ch, PANEL_W - cw, PANEL_H);
    blit_tile(s_ground, &kit[3], 0, ch, cw, PANEL_H - ch);
    blit_tile(s_ground, &kit[5], PANEL_W - cw, ch, PANEL_W, PANEL_H - ch);
    blit_tile(s_ground, &kit[0], 0, 0, cw, ch);                         /* corners */
    blit_tile(s_ground, &kit[2], PANEL_W - cw, 0, PANEL_W, ch);
    blit_tile(s_ground, &kit[6], 0, PANEL_H - ch, cw, PANEL_H);
    blit_tile(s_ground, &kit[8], PANEL_W - cw, PANEL_H - ch, PANEL_W, PANEL_H);

    {
        Surf s; s.p = s_ground; s.w = PANEL_W; s.h = PANEL_H;
        for (i = 0; i < rows; i++) {
            int y = ROW_Y0 + ROW_PITCH * i;
            recess(&s, CTL_X - PAD, y - PAD, CTL_W + 2 * PAD, ROW_H + 2 * PAD);
        }
    }
    s_groundOk = 1;
    return 1;
}

/* Repaint the loaded frame's plane IN PLACE. The archive ships the frame
   uncompressed for exactly this: +0x10 PtrFrameBits is a flat w*h plane, so
   this is one copy and not a re-encode. */
/* A failure LATCHES. The bank 0x4B8C60 hands back is a FRESH buffer every call
   -- it is 0x4BBE50's read, pointer-fixed in place, with no cache -- and none
   of the failure paths above can give it back. The engine rebuilds the in-game
   GUI stack on every world click, which re-pushes this screen, so without the
   latch a persistent failure would leak one frontend.gaf per click. The
   documented failure mode is a plainer panel, not no panel, so latching costs
   the composed ground and nothing else. */
static int s_groundFailed;

static int compose_ground(int rows)
{
    int ok;
    if (s_groundOk) return 1;
    if (s_groundFailed) return 0;
    ok = compose_ground_once(rows);
    if (!ok) {
        s_groundFailed = 1;
        mlog("menu: the frontend.gaf ground did not compose - the drawn panel stands");
    }
    return ok;
}

static void repaint_ground(char* ctrls)
{
    void* bank;
    void* ent;
    const unsigned char* fr;
    unsigned char* plane;

    if (!ctrls || !compose_ground(s_nrows)) return;
    bank = *(void**)(ctrls + P_GAFBANK);
    if (!bank) return;
    ent = ((gaf_find_fn)VA_GAFFIND)(bank, ART_NAME);
    if (!ent) return;
    fr = tagpu_gaf_seq_frame((const char*)ent, 0);
    if (!fr) return;
    if (*(const unsigned short*)(fr + TAGPU_GF_W) != PANEL_W ||
        *(const unsigned short*)(fr + TAGPU_GF_H) != PANEL_H ||
        fr[TAGPU_GF_COMP]) return;                  /* only our own flat plane */
    plane = *(unsigned char**)(fr + TAGPU_GF_PIX);
    if (!plane) return;
    memcpy(plane, s_ground, sizeof s_ground);
}

/* ---- the trigger --------------------------------------------------------
   tools/guipanel.py sprocket(), ported. Generated rather than hand-gridded
   because the shape's whole argument is EIGHT teeth on 45 degree steps: every
   tooth is then a mirror of another across an axis or a diagonal, so the
   rasteriser cannot make one ragged without making its mirror ragged
   identically. `tooth` is the angular width as a fraction of the pitch, so
   0.5 is teeth and gaps of equal width -- square teeth, no taper, which is
   what reads as a sprocket rather than a gear at 28 px. */
static void sprocket(unsigned char* m)
{
    const double r_out = 13.4, r_root = 10.2, tooth = 0.5, bore = 4.6;
    const int teeth = 8;
    const double c = (TRIG - 1) / 2.0;
    const double half = 3.14159265358979323846 / teeth;
    int x, y;

    memset(m, 0, TRIG * TRIG);
    for (y = 0; y < TRIG; y++) {
        for (x = 0; x < TRIG; x++) {
            double dx = x - c, dy = y - c;
            double r = sqrt(dx * dx + dy * dy), ang, k;
            if (r <= bore || r > r_out) continue;   /* the bore is a hole */
            if (r <= r_root) { m[y * TRIG + x] = 1; continue; }
            /* phased so a tooth points straight up, which is what makes the
               thing read as upright at 28 px */
            ang = atan2(dy, dx) + 3.14159265358979323846 / 2 + half;
            while (ang < 0) ang += 2 * 3.14159265358979323846;
            while (ang >= 2 * 3.14159265358979323846) ang -= 2 * 3.14159265358979323846;
            k = fmod(ang, 2 * half) / (2 * half);
            if (fabs(k - 0.5) * 2 <= tooth) m[y * TRIG + x] = 1;
        }
    }
}

/* A mask split into its one-pixel 4-connected border and the rest. The border
   traces the teeth AND the bore, which is what makes the hole read at 28 px. */
static void boundary(const unsigned char* m, unsigned char* edge, unsigned char* inner)
{
    static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
    int x, y, k;
    for (y = 0; y < TRIG; y++) for (x = 0; x < TRIG; x++) {
        int i = y * TRIG + x, e = 0;
        edge[i] = inner[i] = 0;
        if (!m[i]) continue;
        for (k = 0; k < 4; k++) {
            int nx = x + DX[k], ny = y + DY[k];
            if (nx < 0 || nx >= TRIG || ny < 0 || ny >= TRIG || !m[ny * TRIG + nx]) { e = 1; break; }
        }
        edge[i] = (unsigned char)e;
        inner[i] = (unsigned char)!e;
    }
}

/* One in-memory GAF frame per state. +0x10 is a real pointer here, not a file
   offset -- CopyGafToContext takes the loaded form (tagpu_gaf.h). */
static void build_trigger(void)
{
    unsigned char m[TRIG * TRIG], edge[TRIG * TRIG], inner[TRIG * TRIG];
    unsigned char ring[TRIG * TRIG], core[TRIG * TRIG];
    int st, i;

    sprocket(m);
    boundary(m, edge, inner);
    boundary(inner, ring, core);        /* one more ring a step down, so the
                                           body is not a flat plate against the
                                           outline -- the logo does this too */
    for (st = 0; st < TS_COUNT; st++) {
        unsigned char* p = s_trigPix[st];
        unsigned char* f = s_trigFrame[st];
        void* pix = p;
        memset(p, IX_KEY, TRIG * TRIG);
        for (i = 0; i < TRIG * TRIG; i++) {
            if (edge[i])      p[i] = TRIG_INK[st][0];
            else if (ring[i]) p[i] = TRIG_INK[st][1];
            else if (core[i]) p[i] = TRIG_INK[st][2];
        }
        memset(f, 0, 0x18);
        *(unsigned short*)(f + 0x00) = TRIG;
        *(unsigned short*)(f + 0x02) = TRIG;
        f[0x08] = IX_KEY;               /* transparent: the bore and the surround */
        f[0x09] = 0;                    /* raw 8bpp                               */
        memcpy(f + 0x10, &pix, 4);
    }
}

/* Both rects hang off ONE margin, so the icon's right edge and the drop-down's
   right edge land on one line and the menu visibly drops from the icon. Both
   are anchored to the frame's RIGHT edge, never to a fixed coordinate.

   HUD SCALE (tagpu_hud.h, gui-renderer.md 22) MOVES THEM, and moves them
   differently, because they live in different regions of the composite:

     * the SPROCKET is in the top bar, which HUD scale magnifies. It is drawn
       into the engine's 1x bar and comes out s times bigger, so its x has to
       be taken against the BAR's own width, W/s -- at the screen's width it
       lands past the last source column the bar band samples and is simply not
       there. (Measured 2026-09-11 at s = 2.25: the icon vanished.)
     * the DROP-DOWN hangs below the bar, in the world region, which the
       composite leaves alone. It is placed in SCREEN pixels, and only its top
       edge follows the bar's height -- which is also what keeps it out of the
       bar band, where half of it would otherwise be magnified and the other
       half not.

   The margin is measured ON SCREEN for both, so their right edges still land
   on one line at every s. And the point each is hit-tested against arrives in
   exactly the space it was placed in: tagpu_hud_to_engine divides inside a HUD
   region and does nothing outside one, which is the same division and the same
   nothing as here. At stock scale every line below is the unscaled
   arithmetic, to the character. */
static void trigger_rect(int* x, int* y)
{
    int q8 = 256, w;
    tagpu_hud_live(NULL, NULL, &q8);
    w = (int)g_ddraw.width * 256 / q8;       /* the bar's width in the bar's own px */
    if (w < TRIG + MARGIN) w = TRIG + MARGIN;
    *x = w - MARGIN - TRIG;
    *y = 2;
}

/* The drop-down. `menu_open` writes this into the panel gadget and
   `tagpu_menu_owns_point` tests against it, so there is one rule.

   IT IS AN ENGINE-SURFACE RECT THAT NAMES A PLACE ON SCREEN, and under HUD
   scale (22.6) those are not the same point. The panel hangs below the bar,
   i.e. in the WORLD region, and the world region is not composited where the
   engine drew it: it is translated by (128s - 128, 32s - 32). So the screen rect is
   chosen first -- right edge on the sprocket's line, top edge just under the
   magnified bar -- and then moved back along that vector to say where the
   ENGINE has to draw for it to land there.

   Without the subtraction the panel walks right by the whole inset as the
   scale rises: 124 px of it hang off a 1920 screen at s = 2.25, and at 4K's
   s = 4.5 it is placed at 3912 on a 3840-wide screen and NEVER APPEARS AT ALL.

   At stock the inset is zero and both lines below are the unscaled
   arithmetic, to the character. */
static void panel_rect(int* x, int* y)
{
    int q8 = 256, bh = BAR_H, w = (int)g_ddraw.width, dx = 0, dy = 0;
    tagpu_hud_live(NULL, &bh, &q8);
    tagpu_hud_shift(&dx, &dy);               /* (0,0) whenever the pass is inert */
    w -= MARGIN * q8 / 256;                  /* the margin the sprocket leaves */
    if (w < PANEL_W) w = PANEL_W;            /* never off the left */
    *x = w - PANEL_W - dx;
    if (*x < 0) *x = 0;                      /* never off the left, again */
    *y = bh - dy;                            /* screen bh, i.e. engine row 32 */
}

/* ---- generating the .GUI ------------------------------------------------- */
/* TDF, CRLF and tabs like the stock screens. The panel's authored rect is a
   placeholder: menu_open() writes the real one, because it depends on the
   resolution and a file written at attach cannot know it. */
static int gput(char* b, int cap, int at, const char* fmt, ...)
{
    va_list ap;
    int k;
    if (at < 0 || at >= cap) return -1;
    va_start(ap, fmt);
    k = _vsnprintf(b + at, (size_t)(cap - at), fmt, ap);
    va_end(ap);
    if (k < 0 || at + k >= cap) return -1;          /* MSVCRT does not NUL-terminate */
    return at + k;
}

/* `commonattribs` is a field the .GUI parser reads and the stock screens use
   non-zero values of (VISUALS.GUI's own labels carry 104, its BSHADOWS 109), so
   a screen we re-emit has to carry it through rather than assume 0. Every
   RENDER.GUI call site passes 0.

   `assoc` IS THE SAME KIND OF FIELD, and getting it wrong has a symptom nobody
   would trace back to a re-emitted file: on the front-end screen the SCREEN
   SIZE arrows move the GAMMA slider.

   `assoc` is a group id (`gadget+0x01`), and for a slider it is the binding to
   everything that drives or displays it. The scroll arrows are not in the file
   at all -- `GUI_StageUpdateDraw` synthesizes two unnamed `id=1` buttons per
   slider at load (`0x4A8663..0x4A8979`) and COPIES THE SLIDER'S `assoc` into
   them -- and the arrow's click handler then finds its slider by a linear scan
   from record 1:

       4a6fa4:  mov   cl,[ebp+0x01]       ; the arrow's assoc
       4a6fc0:  cmp   BYTE PTR [eax],0x4  ; an id=4 slider?
       4a6fc5:  cmp   BYTE PTR [eax+1],cl ; with my assoc?
       4a6fc8:  je    0x4a6fd6            ; -> mine. FIRST match wins.

   The stock VISUALS.GUI gives VIDSLDR, VIDVAL and VIDTEXT `assoc=243` and
   leaves GAMMA at 0. Writing `assoc=0` for everything makes both sliders match
   every arrow, and the scan stops at the first one -- GAMMA, which we emit
   first. Nothing faults (a scan that matches nothing falls back to gadget 0 at
   `0x4A6FD4`), the wrong slider simply moves. So the field is carried through
   from the stock file like every other one. */
static int common(char* b, int cap, int at, int id, int assoc, const char* name,
                  int x, int y, int w, int h, int attribs, int colorf, int cattr)
{
    at = gput(b, cap, at,
        "\t[COMMON]\r\n\t\t{\r\n"
        "\t\tid=%d;\r\n\t\tassoc=%d;\r\n\t\tname=%s;\r\n"
        "\t\txpos=%d;\r\n\t\typos=%d;\r\n\t\twidth=%d;\r\n\t\theight=%d;\r\n"
        "\t\tattribs=%d;\r\n\t\tcolorf=%d;\r\n\t\tcolorb=0;\r\n"
        "\t\ttexturenumber=0;\r\n\t\tfontnumber=0;\r\n\t\tactive=1;\r\n"
        "\t\tcommonattribs=%d;\r\n\t\thelp=;\r\n\t\t}\r\n",
        id, assoc, name, x, y, w, h, attribs, colorf, cattr);
    return at;
}

static int build_gui(char* b, int cap, int rows)
{
    int at = 0, i;

    at = gput(b, cap, at, "[GADGET0]\r\n\t{\r\n");
    at = common(b, cap, at, 0, 0, "RENDER", 0, BAR_H, PANEL_W, PANEL_H, 0, 0, 0);
    at = gput(b, cap, at,
        "\ttotalgadgets=%d;\r\n"
        "\t[VERSION]\r\n\t\t{\r\n\t\tmajor=1;\r\n\t\tminor=0;\r\n\t\trevision=1;\r\n\t\t}\r\n"
        "\tpanel=;\r\n\tcrdefault=;\r\n\tescdefault=;\r\n\tdefaultfocus=;\r\n\t}\r\n",
        rows * 2 + 2);

    /* the ground: an id=12 whose NAME is the GAF frame, over the whole panel */
    at = gput(b, cap, at, "[GADGET1]\r\n\t{\r\n");
    at = common(b, cap, at, 12, 0, ART_NAME, 0, 0, PANEL_W, PANEL_H, 0, 15, 0);
    at = gput(b, cap, at, "\t}\r\n");

    /* the caption. tools/guipanel.py rules the panel at DIV_TOP = 30 and the
       band above it is the title's -- an empty one is just a bare rule. */
    at = gput(b, cap, at, "[GADGET2]\r\n\t{\r\n");
    at = common(b, cap, at, 5, 0, "TITLE", LBL_X, TITLE_Y, TITLE_W, 18, 1, 15, 0);
    at = gput(b, cap, at, "\ttext=%s;\r\n\t}\r\n", "Render options");

    for (i = 0; i < rows; i++) {
        int y = ROW_Y0 + ROW_PITCH * i;
        /* the label, BESIDE its control -- every stock runtime screen puts it
           16 px above, which six rows have no room for (gui-gadgets.md 10.3) */
        at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", i * 2 + 3);
        at = common(b, cap, at, 5, 0, "TEXT", LBL_X, y, LBL_W, ROW_H, 1, 15, 0);
        at = gput(b, cap, at, "\ttext=%s;\r\n\t}\r\n", s_row[i].label);

        at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", i * 2 + 4);
        at = common(b, cap, at, 1, 0, s_row[i].name, CTL_X, y, CTL_W, ROW_H, 1, 15, 0);
        at = gput(b, cap, at,
            "\tstatus=0;\r\n\ttext=%s;\r\n\tquickkey=0;\r\n\tgrayedout=0;\r\n\tstages=%d;\r\n\t}\r\n",
            s_row[i].text, s_row[i].stages);
    }
    return at;
}

/* ---- reading what is in force --------------------------------------------- */
/* The plates show the value IN FORCE, read at open time: the store's, or a
   lever's where one holds the row (tagpu_settings.h). */
/* Classic++ is every row the switch OWNS at its Classic++ value; anything else
   is Custom, which is why Custom is derived and never chosen.

   R_SS AND R_SHADOWS ARE NOT AMONG THEM. Supersampling is orthogonal to the
   lane -- the native pass reads it under Classic as well as Classic++. Shadows
   is the ENGINE's shadow switch as well as ours (it drives the option word's
   bits 2..4, `eng_push`), so it has to work under Classic too. Both are
   exempt from `row_greyed`'s lane rule and left alone by the Classic++ preset;
   in this test either would make a player who turned it off see "Custom" with
   nothing in the lane customised. */
static int derive_style(void)
{
    return (s_stage[R_ASSETS] == 0 || s_stage[R_LIGHT] != 1 || s_stage[R_SHADOWQ] != 2)
           ? STYLE_CUSTOM : STYLE_PP;
}

static void read_state(void)
{
    const TAGPU_LIGHT* L = tagpu_classicpp_light();
    int i;

    /* On as chosen, and "Off (driver)" while the restorer is recorded
       off here: the player asked for it and the driver is what says no */
    s_stage[R_ASSETS]  = tagpu_classicpp_assets() ? (tagpu_rguard_off() ? 2 : 1) : 0;
    s_stage[R_LIGHT]   = tagpu_classicpp_lit() ? 1 : 0;
    s_stage[R_SS]      = tagpu_settings_ss() == 2;
    s_stage[R_FPS]     = tagpu_settings_fps() ? 1 : 0;
    s_stage[R_VSYNC]   = tagpu_settings_vsync();
    /* the map edge in force: its lever, then the store, then the default --
       tagpu_opt.c answers in that order, and Black under tagpu_defaults.off */
    s_stage[R_EDGE]    = tagpu_opt_on(EDGE_ON) ? 1 : 0;

    /* 0 (Off) when the cfg names a value this row does not offer. That is not
       reachable for `shadows=1`, which tagpu_classicpp.c's parse migrates to
       HARD before it ever gets here; the fallback stands for a hand-edited
       value out of range. */
    s_stage[R_SHADOWS] = 0;
    for (i = 0; i < 2; i++) if (L && SHADOW_VAL[i] == L->shadows) s_stage[R_SHADOWS] = i;
    /* with no store, the row is the engine's own shadow switch alone */
    if (tagpu_settings_ignored()) s_stage[R_SHADOWS] = eng_shadows_bit();

    s_stage[R_SHADOWQ] = 2;
    for (i = 0; i < 4; i++) if (L && SHADOWQ_VAL[i] == L->shadowres) s_stage[R_SHADOWQ] = i;

    s_stage[R_STYLE] = tagpu_classicpp_on() ? derive_style() : STYLE_CLASSIC;
}

/* ---- pushing the state at the engine ------------------------------------- */

/* A row that cannot bite is greyed rather than left looking live. Four of the
   six describe CLASSIC++'s behaviour and are inert under Classic -- leaving
   them reading `On` there is the menu telling the player something untrue.
   Supersampling is not one of them: the native pass reads it in both lanes. */
/* Is the row held by a lever, or is the store ignored altogether? Either way a
   click could not change what is drawn, so the row is greyed and shows the
   value in force (renderers.md 2.10b). */
static int row_held(int row)
{
    unsigned h;
    /* Shadows stays live: with the stock BSHADOWS toggle gone from the screen,
       it is the only switch left for the engine's shadows (eng_set_shadows) */
    if (tagpu_settings_ignored()) return row != R_SHADOWS;
    switch (row) {
    case R_STYLE: return exists(CPP_ON) || exists(CPP_OFF);
    case R_SS:    return exists(SS_OFF);
    case R_FPS:   return exists(FPS_ON);
    case R_EDGE:  return exists(EDGE_ON) || exists(EDGE_OFF);
    default:      break;
    }
    h = tagpu_classicpp_held();
    return (row == R_ASSETS  && (h & TAGPU_HELD_ASSETS))  ||
           (row == R_LIGHT   && (h & TAGPU_HELD_LIGHT))   ||
           (row == R_SHADOWS && (h & TAGPU_HELD_SHADOWS)) ||
           (row == R_SHADOWQ && (h & TAGPU_HELD_SHADOWRES));
}

static int row_greyed(int row)
{
    if (row_held(row)) return 1;
    /* R_SS, R_FPS, R_VSYNC, R_SHADOWS and R_EDGE are orthogonal to the
       Classic/Classic++ lane, so none is one of the switch's dependants:
       supersampling is a resolution choice, the FPS counter a diagnostic drawn
       OVER the finished frame, vsync how the frame is paced, Shadows is the
       engine's shadow switch too, and the map edge is drawn in full colour
       under either lane. Grey them with the lane and a player on Classic could
       not reach them. Vsync is live under the GDI backend as well: it has no
       present mode to pick there, but the frame limiter's backstop paces it
       (fps_limiter.c). */
    if (row == R_STYLE || row == R_SS || row == R_FPS || row == R_VSYNC ||
        row == R_SHADOWS || row == R_EDGE) return 0;
    if (s_stage[R_STYLE] == STYLE_CLASSIC) return 1;
    /* ALWAYS GREY. `shadowres=` is the edge of
       the soft map's depth texture and reaches nothing else -- the hard pair is
       drawn from the unit bake at the frame's own resolution -- and nothing
       produces the soft map. Left in place rather than removed: it is one line
       to un-grey the day the map's producer is written, and a row that vanishes
       and comes back is worse for the player than one that is plainly
       unavailable. */
    if (row == R_SHADOWQ) return 1;
    return 0;
}

static void push_stages(void* gi)
{
    int i;
    for (i = 0; i < s_nrows; i++)
        ((set_status_fn)VA_SETSTATUS)(gi, s_row[i].name, s_stage[i]);
    /* `grayedout` is the engine's own way of saying "this does not apply now"
       (gui-gadgets.md 7.1), and it refuses the click as well as dimming the
       plate: 0x4AA36A tests bit 0 and skips the stage advance outright.

       THROUGH THE ENGINE'S SETTER, and it has to be. `+0x13C` is a **u16 whose
       bit 0 is the flag**, and both of the engine's own writers -- the .GUI
       parser (0x4ADD3E) and this setter (0x4A12D0) -- read the word, replace
       bit 0 and store a WORD, deliberately preserving bits 1..15. Storing a
       32-bit 0/1 into the field directly would clear those bits and the two
       bytes at +0x13E/+0x13F as well.
       GUIGADGET_SetGrayed 0x4A1250 is the exact parallel of SetStatus above:
       the same by-name scan of ControlsAry (stride 0x15B, name at +0x15D),
       stdcall, ret 0xC. */
    for (i = 0; i < s_nrows; i++)
        ((set_grayed_fn)VA_SETGRAYED)(gi, s_row[i].name, row_greyed(i));
    ((set_dirty_fn)VA_SETDIRTY)(gi);       /* the pump repaints with 0x40 */
}

/* ---- open and close ------------------------------------------------------ */
static int on_stack(char* main_p, void* gm)
{
    void* p = *(void**)(main_p + OFF_TOPGUI);
    int n;
    for (n = 0; p && n < 32; n++) {
        if (p == gm) return 1;
        p = *(void**)p;                     /* +0x00 per_active, the LIFO link */
    }
    return 0;
}

/* `fresh` = the player just opened the menu, so the plates take the values in
   force. A re-open with fresh == 0 is a RECOVERY, and there the model is ours
   and must survive: the engine tears the whole in-game GUI stack down and
   rebuilds it (a new ARMMAIN2.GUI whose `under` is NULL) on a world click, and
   our panel hangs over the world. Re-reading there would put a plate back the
   moment it was clicked: Classic++ re-reads the store at most twice a second
   (tagpu_classicpp.c), so for up to half a second after a click the value in
   force is still the old one.

   An ordinary click on one of our own rows never reaches this path: it is
   answered by `menu_accept`. */
static void menu_open(char* main_p, int fresh)
{
    void* gi = main_p + OFF_GUIINFO;
    char* expect = main_p + OFF_EXPECT;
    char* ctrls;
    void* gm;

    lstrcpynA(s_saved, expect, sizeof s_saved);
    lstrcpynA(expect, SCREEN, 16);
    /* 0x20 is what 0x495207 passes; 0x400 additionally suppresses GUI_Load's
       own repaint so the panel is never drawn at the placeholder xpos. */
    gm = ((gui_load_fn)VA_GUI_LOAD)(gi, expect, 0x20 | 0x400);
    if (!gm) {
        lstrcpynA(expect, s_saved, 16);
        mlog("menu: GUI_Load returned NULL - guis\\RENDER.GUI not readable");
        return;
    }

    ctrls = *(char**)((char*)gm + GM_CTRLS);
    if (ctrls) {
        int px, py;
        panel_rect(&px, &py);
        *(short*)(ctrls + G_XPOS) = (short)px;
        *(short*)(ctrls + G_YPOS) = (short)py;
    }
    *(void**)((char*)gm + GM_ONCMD) = (void*)tagpu_menu_oncommand;
    *(void**)((char*)gm + GM_CTX)   = main_p;
    s_gm = gm;

    if (fresh) read_state();
    push_stages(gi);

    /* STAGE 1 IS WHAT BUILDS THE PANEL'S OWN SURFACE, and 0x400 suppressed it
       along with the draw -- so this is not a repaint, it is the call GUI_Load
       would have made, reproduced exactly: the same `flags | 1` (0x400 is
       never passed on; 0x4AACB5 tests it and skips) under the same counted
       lock pair. Getting this wrong is not subtle and not silent: with only a
       0x40 repaint the panel has no surface, and the engine composites the
       frame's own pixels at our rect -- the game drawn a second time from
       x = 704 across. Stage 1 also reads the rect (0x4A8238: xpos -1 is the
       centre-me sentinel), which is why the right-aligned xpos is written
       BEFORE this and not after. */
    ((lock_fn)VA_DRAWLOCK)();
    ((stage_draw_fn)VA_STAGEDRAW)(gi, 0x20 | 0x1);
    ((lock_fn)VA_DRAWUNLOCK)();

    /* Stage 1 is what loaded `anims\\RENDER.GAF` (the id 0 branch at 0x4A84F2),
       so the plane exists only now. Repaint it from the player's own install
       and ask for the redraw. */
    repaint_ground(ctrls);
    ((set_dirty_fn)VA_SETDIRTY)(gi);
}

/* UpdateIngameGUI IS NOT A POP, IT IS A COLLAPSE, and closing with it takes
   the player's build menu down with us.

   `0x491D70` loops `GUI_Pop` until the top screen's name matches the 16-byte
   buffer at `main+0x37EA0` (the compare is `0x4AB060`, `strncmp([top+4]+2,
   buf, 16)`). That buffer names the BASE in-game screen and nothing else: with
   a commander selected the stack is ARMMAIN2 -> ARMCOM1 -> ours and the buffer
   still reads "ARMMAIN2.GUI" [MEASURED 2026-09-12 by peeking it]. So restoring
   it and calling UpdateIngameGUI pops TWO screens -- ours and the unit's
   build page -- while leaving the unit's selected flag alone: the build menu
   disappears with the selection rect still drawn round the commander.

   `GUI_Pop` is the single pop the loop itself calls, and it is already this
   module's idiom on the front end (`vis_relist`). It answers the pump too --
   `0x4A9673` writes -1 into `gi->UIChange_f` -- so there is no `menu_accept`
   to do, and `tagpu_menu_oncommand` returns on a negative row.

   IT IS GUARDED ON BEING ON TOP rather than assumed: pop is positional, so
   popping while something else sits above us would take that screen instead of
   ours. The tick re-asserts our name every frame precisely to keep us on top,
   so the guard is expected to hold; when it does not, the collapse is still the
   honest fallback, because leaving our screen on the stack with the buffer
   restored would let it linger until the player next moved the stack. */
static void menu_close(char* main_p)
{
    void* gm = s_gm;

    lstrcpynA((char*)main_p + OFF_EXPECT, s_saved, 16);
    s_gm = 0;

    if (gm && *(void**)(main_p + OFF_TOPGUI) == gm)
        ((gui_pop_fn)VA_GUI_POP)((void*)(main_p + OFF_GUIINFO));
    else
        ((upd_gui_fn)VA_UPDGUI)(1);
}

/* ---- the engine calls this ----------------------------------------------- */
/* __stdcall void(GUIInfo*): the actuated index arrives in gi->UIChange_f, not
   as an argument (GUIMEMSTRUCT+0x08, 0x4A967F). -1 is the pop path. */
/* Which of OUR rows the actuated gadget is, or -1 for anything else -- the
   engine's, or nothing actuated at all. Shared by both screens: the lookup is
   by NAME, so it is the same question on RENDER.GUI and on VISUALS.GUI, and on
   the front end it is also the test for whether the engine's own handler must
   be allowed to run (see tagpu_vis_oncommand). */
static int menu_row_of(void* gi)
{
    char* main_p = *(char**)TA_MAIN;
    char* top;
    char* ctrls;
    int idx, i;

    if (!gi || !main_p) return -1;
    idx = *(int*)((char*)gi + GI_UICHANGE);
    /* -1 MEANS THIS SCREEN IS BEING DESTROYED, not that a row was clicked:
       both callers that pass it -- GUI_Pop (0x4A9673) and the pump's inlined
       pop (0x4AA7AC) -- free the GUIMEMSTRUCT immediately afterwards. It is
       still not the signal to throw the model away: the front-end screen is
       rebuilt from `s_stage` on the next visit, and in game the panel is
       re-opened with `fresh == 0` precisely so a recovery keeps it. `on_stack`
       in the tick is the authority on whether our screen is still there. */
    if (idx < 0) return -1;

    top   = *(char**)(main_p + OFF_TOPGUI);
    ctrls = top ? *(char**)(top + GM_CTRLS) : 0;
    if (!ctrls || idx < 1 || idx > *(short*)(ctrls + 0xB6)) return -1;

    /* index -> row, by the gadget's own name: the .GUI's layout is ours but a
       name lookup cannot go wrong if the layout ever changes. */
    for (i = 0; i < s_nrows; i++)
        if (!memcmp(ctrls + (size_t)idx * STRIDE + G_NAME, s_row[i].name,
                    strlen(s_row[i].name) + 1)) return i;
    return -1;
}

/* SAY THE CLICK WAS HANDLED, or the pump destroys the screen under us.

   `gi->UIChange_f` is not only the inbound argument -- it is also the ANSWER
   the GUI pump reads back, and leaving it set means "not mine". The pump's
   dispatch tail is explicit about it (0x4AA78D, the only site that calls a
   screen's `OnCommand` for a click):

       mov  edx,[ebp+0x18]        ; gi->TheActive_GUIMEM
       mov  eax,[edx+0x08]        ; its OnCommand
       test eax,eax
       je   0x4AA79A
       push ebp
       call eax                   ; <- us
       cmp  [ebp+0x60],edi        ; UIChange_f still != -1 ?
       je   0x4AA7FA              ; -1: handled, done
       ...                        ; else: UIChange_f = -1, call OnCommand
                                  ; AGAIN, stage 2, then relink
                                  ; gi->TheActive_GUIMEM = top->per_active
                                  ; and free(top)

   That tail (0x4AA7BC..0x4AA7FA) is `GUI_Pop 0x4A9660`'s body INLINED,
   instruction for instruction -- the draw-lock pair, stage 2, the relink, the
   `0x4D85A0` free and the `flags & 0x800` repaint. So the screen is popped and
   freed WITHOUT `GUI_Pop` ever being entered, which is why an observer armed on
   `0x4A9660` sat there and never fired while the screen vanished on every
   click (measured 2026-09-11).

   Every one of the engine's own handlers ends by calling `0x4AB0A0(gi)` --
   `0x45E2F0` and `0x45E27C` in the visual-options handler, `0x45E48C` on the
   fall-through it does not recognise -- and the ones that deliberately want the
   pop (the tab buttons at `0x45E46A`, OK at `0x45E457`) are exactly the ones
   that return without it. It is a protocol, not a courtesy: clearing the field
   IS how a screen says "I consumed this".

   The in-game RENDER.GUI screen needs it too: without it its GUIMEMSTRUCT is
   popped and freed on every click, and `before_update`'s `on_stack` check
   re-opens it with `fresh == 0`, so it LOOKS like it works. That recovery path
   exists for a real reason -- the engine tears the in-game GUI stack down on
   a world click -- but it is not on the path of an ordinary click. */
static void menu_accept(void* gi)
{
    ((act_done_fn)VA_ACTDONE)(gi);
}

/* ---- the model into the store --------------------------------------------
   Any thread may record a value; the file is written by `tagpu_menu_present`.
   ONLY WHAT THE PLAYER TOUCHED is recorded, never a row a lever holds: a held
   row shows the lever's value, and copying that into the store would outlive
   the lever. */
static int style_value(void)
{
    return s_stage[R_STYLE] == STYLE_CLASSIC ? TS_STYLE_CLASSIC :
           s_stage[R_STYLE] == STYLE_PP      ? TS_STYLE_PP : TS_STYLE_CUSTOM;
}

static void commit_one(int row)
{
    if (row_held(row)) return;
    switch (row) {
    case R_ASSETS:  tagpu_settings_set(TS_ASSETS, s_stage[R_ASSETS] != 0); break;
    case R_LIGHT:   tagpu_settings_set(TS_LIGHT, s_stage[R_LIGHT]); break;
    case R_SHADOWS:
        tagpu_settings_set(TS_SHADOWS, SHADOW_VAL[s_stage[R_SHADOWS]]);
        /* the engine's shadows are this row too: through the store when it
           owns them, otherwise as the stock toggle set them */
        if (eng_owned()) eng_push(ENG_BITS);
        else eng_set_shadows(s_stage[R_SHADOWS] != 0);
        break;
    case R_SHADOWQ: tagpu_settings_set(TS_SHADOWRES, SHADOWQ_VAL[s_stage[R_SHADOWQ]]); break;
    case R_SS:      tagpu_settings_set(TS_SS, s_stage[R_SS] ? 2 : 1); break;
    case R_FPS:     tagpu_settings_set(TS_FPS, s_stage[R_FPS]); break;
    case R_VSYNC:   tagpu_settings_set(TS_VSYNC, s_stage[R_VSYNC]); break;
    case R_EDGE:    tagpu_settings_set(TS_EDGE, s_stage[R_EDGE]); break;
    default:        break;
    }
}

/* THE RENDER KEYS FIRST AND THE STYLE LAST. Leaving a preset for Custom, the
   store answers the preset for the render keys until the style changes, so
   writing them first changes nothing a reader sees, and the style's one store
   is the moment the pinned values take over. Going back from Custom to a
   preset, a reader can see one generation of a mixed custom set; every set
   bumps the generation after its value and marks the store again, so the
   renderer and the file both end on the full set.

   A render row carries the style with it even when a lever holds the Renderer
   row: `tagpu_classicpp.on` decides on or off, and the store's `custom` is
   what pins the row the player just changed. Under `.off` those rows are greyed
   (row_greyed), so no click reaches here to write Classic over the store. */
/* A LOOK ROW COMMITS ALL THREE (assets, light, shadow quality), not only
   itself. The store's render slots
   keep whatever the last Custom session left in them while the style is a
   preset, so going Custom on one click would bring the others' old values back
   to life under plates that show the preset's. The plates are what the player
   is looking at, so they are what is committed. */
static void commit_row(int row)
{
    if (row == R_STYLE || row == R_ASSETS || row == R_LIGHT || row == R_SHADOWQ) {
        commit_one(R_ASSETS);
        commit_one(R_LIGHT);
        commit_one(R_SHADOWQ);
        if (row == R_STYLE && row_held(R_STYLE)) return;
        tagpu_settings_set(TS_STYLE, style_value());
        return;
    }
    commit_one(row);
}

static void commit_all_rows(void)
{
    int i;
    for (i = 0; i < R_COUNT; i++) if (i != R_STYLE) commit_one(i);
    if (!row_held(R_STYLE)) tagpu_settings_set(TS_STYLE, style_value());
}

void __stdcall tagpu_menu_oncommand(void* gi)
{
    int row = menu_row_of(gi);
    if (row < 0) return;

    if (row == R_STYLE) {
        /* alternates Classic and Classic++; Custom is derived, so it is never
           a destination -- from Custom the click goes to Classic++ */
        s_stage[R_STYLE] = (s_stage[R_STYLE] == STYLE_PP) ? STYLE_CLASSIC : STYLE_PP;
        if (s_stage[R_STYLE] == STYLE_PP) {
            /* NOT R_SS OR R_SHADOWS: both are orthogonal to the lane (see
               derive_style), so the preset must not silently undo a player
               who turned either off. */
            s_stage[R_ASSETS] = tagpu_rguard_off() ? 2 : 1; s_stage[R_LIGHT] = 1; s_stage[R_SHADOWQ] = 2;
        }
    } else if (row == R_ASSETS) {
        /* Off and On alternate, and "Off (driver)" goes to On. Every way to
           On is the retry while the driver is recorded off: the record is
           forgotten and the restorer asked again, and a second crash or
           failed check turns it off again (D12). */
        s_stage[R_ASSETS] = s_stage[R_ASSETS] == 1 ? 0 : 1;
        if (s_stage[R_ASSETS] == 1 && tagpu_rguard_off()) tagpu_rguard_clear();
        if (s_stage[R_STYLE] != STYLE_CLASSIC) s_stage[R_STYLE] = derive_style();
    } else {
        s_stage[row] = (s_stage[row] + 1) % s_row[row].stages;
        /* DERIVED, not forced to Custom. A row clicked back to its Classic++
           value leaves nothing customised, and saying "Custom" there is the
           screen telling the player something untrue -- the same test
           `read_state` applies on the way in, so the label a visit opens with
           and the label a click produces are one rule.

           THAT RULE SUBSUMES AN R_FPS EXEMPTION. The counter is not part of
           the look -- it draws a diagnostic OVER the frame and changes no
           pixel the game rendered -- and neither is supersampling; deriving
           from the three rows that ARE the lane leaves both out by
           construction, on the way in and on the way out, instead of by a list
           that has to be kept in step in two places. */
        if (s_stage[R_STYLE] != STYLE_CLASSIC) s_stage[R_STYLE] = derive_style();
    }

    commit_row(row);
    push_stages(gi);
    menu_accept(gi);
}

/* ---- the render thread's half ------------------------------------------- */
/* The store is written HERE, off the game thread: TA is lockstep and a disk
   write inside OnCommand is an unbounded stall, which can drop a player from a
   session whatever it was writing. A click only records a value
   (`commit_row`); this coalesces any number of them into one write. */
void tagpu_menu_present(void)
{
    /* THE RESTORER GUARD'S RETRY AND ITS CRASH MARKER, here because this is
       the one per-frame call on the render thread under either backend: a
       retry picked while the Vulkan lane is down for good must still clear
       the record. Before the flush, so that it writes the cleared record. */
    tagpu_rguard_tick();
    if (!s_installed) return;
    /* THE GPU ROW HAS ITS OWN FLAG: its value is a device NAME, and the name
       is handed to the store on this thread so that no string crosses one
       (tagpu_vk.h). */
    if (InterlockedExchange(&s_vkDirty, 0))
        tagpu_vk_gpu_store();
    tagpu_settings_flush();
}

/* ---- the trigger's two jobs: it is drawn, and it is hit-tested ----------- */

/* THE HIT-TEST SITS ON BOTH INPUT PATHS, and that is not tidiness. The shield
   handles injected WM_TAGPU_MOUSE through deliver_mouse() BEFORE its own
   armed check, and then, with the shield on, swallows every real
   WM_LBUTTONDOWN. So a tacli instance sees only injected clicks and a player
   sees only real ones: a hit-test hung off one path passes its own tests and
   does not work for players, or the reverse. This is the one function, and
   both callers hand it the same thing -- g_ddraw.cursor, which the injected
   path has just written and which the real path's preceding WM_MOUSEMOVE
   wrote.

   The press toggles and the RELEASE is consumed too. Letting the release
   through would leave the engine holding a button it never saw pressed, and
   that costs the rest of the session. */
/* The sprocket always, and the panel while it is open. Both rects are computed
   the same way the drawing and the push do, so there is one source of truth. */
int tagpu_menu_owns_point(int gx, int gy)
{
    int x, y;

    if (!s_installed) return 0;

    /* s_drawTrigger, to match tagpu_menu_click's own guard: if the post-GUI
       observer did not install there is no sprocket, and claiming its rect
       would leave a 28x28 patch that the zoom transform skips and the click
       path refuses -- an invisible dead zone. */
    if (s_drawTrigger) {
        trigger_rect(&x, &y);
        if (gx >= x && gx < x + TRIG && gy >= y && gy < y + TRIG) return 1;
    }

    if (!s_gm) return 0;
    panel_rect(&x, &y);
    return gx >= x && gx < x + PANEL_W && gy >= y && gy < y + PANEL_H;
}

int tagpu_menu_click(int gx, int gy, int down)
{
    int x, y;

    if (!s_installed || !s_drawTrigger) return 0;
    if (!*(char**)TA_MAIN) return 0;

    if (!down) {                        /* a release we own, and nothing else */
        int was = s_pressed;
        s_pressed = 0;
        return was;
    }

    trigger_rect(&x, &y);
    if (gx < x || gx >= x + TRIG || gy < y || gy >= y + TRIG) {
        /* A press we do NOT own ends any press we do. Without this, a press on
           the sprocket whose release never arrives -- dragged off the window
           (the trigger sits 16 px from the right edge), or alt-tabbed away --
           leaves s_pressed set, and then the next unrelated button-UP is
           swallowed by the branch above and never reaches the engine, which
           goes on holding the button for the rest of the session. That is the
           failure this guard exists to prevent, and the release path alone
           does not prevent it. */
        s_pressed = 0;
        return 0;
    }

    s_pressed = 1;
    s_want = !s_want;
    if (s_want) s_fresh = 1;            /* the player's open reads what is in force */
    return 1;
}

/* Drawn over the finished bar, inside DrawGameScreen and before the flip, with
   the engine's own GAF blitter -- ctx NULL is the back buffer (0x4B7FA5 ->
   0x4C5E70). No GUI screen owns the top bar (every in-game panel is the side
   panel at (0,128) 128x352) and a gadget is drawn into its panel's own w*h
   surface at panel-relative coordinates, so a gadget at x=980 has nowhere to
   be drawn. That is why this is ours and not a gadget. */
static void draw_trigger(void)
{
    int x, y, st, cx, cy;

    if (!s_drawTrigger) return;
    trigger_rect(&x, &y);
    cx = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
    cy = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);
    st = s_pressed ? TS_PRESSED
       : (cx >= x && cx < x + TRIG && cy >= y && cy < y + TRIG) ? TS_OVER
       : TS_NORMAL;
    ((gaf_blit_fn)VA_GAFBLIT)(NULL, s_trigFrame[st], x, y);
}

static int __cdecl before_postgui(void* entry_esp)
{
    (void)entry_esp;
    draw_trigger();
    return 0;
}

/* ---- the per-frame tick, on the game thread ------------------------------ */
static void menu_tick(void)
{
    static int s_firstTick;
    char* main_p = *(char**)TA_MAIN;
    DWORD now;

    if (!main_p) return;
    if (!s_firstTick) { s_firstTick = 1; mlog("menu: first frame tick (DrawGameScreen)"); }

    /* Popped behind our back -- the game ended, the map changed, or something
       else took the stack down. Put the buffer back before the engine's own
       screen name is compared against ours forever. */
    if (s_gm && !on_stack(main_p, s_gm)) {
        s_gm = 0;
        lstrcpynA(main_p + OFF_EXPECT, s_saved, 16);
    }

    now = GetTickCount();
    if (now - s_lastPoll >= POLL_MS) {
        /* The file is a LEVEL sampled on its EDGES, not a level polled: the
           sprocket owns s_want too, and a file read every 250 ms would undo
           every click the moment it was made. */
        int f = exists(OPEN_FILE);
        s_lastPoll = now;
        if (f != s_fileWas) {
            s_fileWas = f;
            s_want = f;
            if (f) s_fresh = 1;
        }
    }
    if (s_want && !s_gm) { menu_open(main_p, s_fresh); s_fresh = 0; }
    else if (!s_want && s_gm) menu_close(main_p);
    else if (s_gm) {
        /* Re-assert the name every frame. The engine rebuilds the in-game stack
           on its own account, and the buffer is the only thing that keeps a
           screen over the world from being popped at the next of 21 sites. */
        lstrcpynA(main_p + OFF_EXPECT, SCREEN, 16);
    }
}

static int __cdecl before_update(void* entry_esp)
{
    (void)entry_esp;
    menu_tick();
    return 0;                           /* never hijack the return */
}

/* ---- init ---------------------------------------------------------------- */
static void read_tokens(void)
{
    HANDLE h;
    char buf[256];
    DWORD n = 0;
    const char* k;

    h = CreateFileA(ON_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, buf, sizeof buf - 1, &n, 0)) n = 0;
    CloseHandle(h);
    buf[n] = 0;
    k = strstr(buf, "rows=");
    if (k) {
        int r = atoi(k + 5);
        if (r >= 1 && r <= R_COUNT) s_nrows = r;
    }
}

/* ===================== the front-end screen (VISUALS.GUI) =================
   The render options reachable before a game starts, on the screen TA already
   ships for display settings: Single Player -> Options -> Visuals.

   WHY WE MAY REPLACE IT AT ALL. `InitTAHPIAry 0x41D4C0` globs `*.UFO` at
   position 3 and `*.HPI` at 4, and a .ufo entry SHADOWS the same path in a
   stock .hpi -- measured 2026-09-11 by packing a `guis/VISUALS.GUI` whose
   `Shading` caption read `UFO-WINS` and reading it back through `tacli ui`.
   So the screen ships as a third file in the archive the DLL already writes at
   attach, with no gadget-array surgery and no patched stock archive.

   WHAT THE STOCK FILE ACTUALLY IS, and it is not what the screenshot suggests:
   VISUALS.GUI carries ONLY the right-hand column -- eleven gadgets, the five
   video controls and their captions. The tab column (SOUND / MUSIC / INTERFACE
   / VISUALS) and the action column (OK / Cancel / Restore / Undo) are
   STARTOPT.GUI's. So re-emitting this file cannot break the tabs or the
   buttons, and the space we may lay out in is what STARTOPT leaves free: x from
   about 200 to 470, between its tabs (68..188) and its actions (478..598).

   AND IT IS NOT A SCREEN OF ITS OWN. `0x45E5E0` loads STARTOPT.GUI with flags
   `0x80` (which pushes) and then loads VISUALS.GUI with flags **`0x200`**, and
   `0x200` is GUI_Load's MERGE flag: `0x4AAA2F` branches on it and parses the
   file straight into the tail of the CURRENT top screen's `ControlsAry`
   (`0x4AAA31`: `edi = gi->TheActive_GUIMEM`, `ebp = ctrls + (count+1)*0x15B`),
   sums the counts into `ctrls+0xB6` (`0x4AABE6`), skips the stack push
   entirely (`0x4AAC54`), and then stamps the LOADED file's name over
   `ControlsAry[0].name` (`0x4AAC98`). So the screen `tacli ui` calls
   VISUALS.GUI is one GUIMEMSTRUCT holding STARTOPT's tabs and actions, the
   stock video controls and our rows, all in one array -- which is exactly what
   a snapshot of it shows, and why our rows and the tab buttons are
   indistinguishable to `0x45E100`. The STARTOPT.GUI that `tacli` reports
   *under* it is the earlier, un-merged one the Options click pushed; a tab
   switch pops the merged screen and that one rebuilds the next tab the same
   way.

   THE LAYOUT is two columns, WINDOW and RENDERER (the geometry block below).
   The stock gadgets the store still leaves the engine keep their names, their
   sizes and their stock ORDER -- only x and y move -- so anything in the
   engine that dispatches by index rather than by name sees them as before.

   THE ROWS ARE THE IN-GAME SCREEN'S, the same `s_row` table and the same
   `s_stage` model: one model, two views. A player who sets Classic++ here has
   set it for the game, and the in-game screen opens agreeing with them.

   TWO OBSERVERS, AND NEITHER SKIPS ANYTHING (tagpu_detour.h: an observer is
   byte-identical to the engine running alone, which is what makes replacing a
   stock screen's behaviour safe):
     - `0x45E5E0`, the visual-options dialog build, on RETURN: the gadgets exist
       by then, so the plates are set from the values in force there.
     - `0x45E100`, OnCommand_VISUALRT_GUI, on ENTRY: the click is ours if the
       actuated gadget carries one of our names, and `tagpu_menu_oncommand` is
       already exactly that test -- it maps the index to a row BY NAME and
       returns silently otherwise, so the front end reuses it unchanged. The
       engine's own handler then runs and finds none of its gadgets actuated. */

#define VA_VIS_BUILD    0x0045E5E0u     /* dialog build, stdcall(int selvmode) */
typedef void (__stdcall *vis_build_fn)(int selvmode);
/* 0x45E100 (OnCommand_VISUALRT_GUI) is NOT detoured -- see tagpu_vis_oncommand */
static const unsigned char VIS_BUILD_STOLEN[7] =
    { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x10 };   /* mov eax,[esp+4]; sub esp,0x10 */

#define VIS_FILE      "guis/visuals.gui"

/* ---- the layout, in screen coordinates ----------------------------------
   TWO COLUMNS, in the space STARTOPT leaves free: x 200..470, between its tab
   column (68..188) and its action column (478..598). The left column is the
   WINDOW -- what the picture is displayed in -- and the right one the
   RENDERER; the stock controls are folded into whichever column they belong
   to rather than kept as a third.

   THE RECESSES ARE OURS (see `draw_recess_frame`). STARTOPT's background PCX
   paints one column of them, centred on the stock gadget column at x = 278,
   and it is the game's art: we cannot repaint it and do not ship it. Two
   columns therefore cannot sit in it, so every control here gets an `id=12`
   recess frame of its own placed behind it, and the stock background shows
   through everywhere between them. */
#define VP_X      200           /* the ground panel, in screen coordinates     */
/* VP_Y clears the background's own "VISUAL" title, whose glyphs end at y = 50
   -- at 46 the panel would eat the bottom of the L. */
#define VP_Y       54
#define VP_W      270
#define VP_H      420
#define VC0_X     207           /* the WINDOW column                           */
#define VC1_X     342           /* the RENDERER column, clear of the actions   */
#define VCOL_W    120
#define VHDR_Y     56           /* the column heading                          */
#define VRULE_Y    72
#define VROW_Y0    80           /* the first row's caption                     */
#define VPITCH     44
#define VLBL_H     14
#define VCTL_DY    16           /* caption -> control                          */
#define VCTL_H     20
#define VSLD_W    122           /* both stock sliders, so one recess fits both */
#define VSLD_H     16
#define VPAD        2           /* the recess's margin around its control      */

/* The screen's own GAF is `anims\\<panel gadget's name>.GAF` -- `0x4A8537`
   strncpy's `ControlsAry[0].name` into the path and `0x4A8565` sets the
   extension -- and `GUI_Load` stamps that name with the file it loaded. The
   merge renames the screen VISUALS.GUI, so ours is `anims/visuals.gaf`. The
   stock game ships no such file, so nothing is shadowed. */
#define VIS_GAF       "anims/visuals.gaf"
#define ART_VISBG     "VISBG"       /* the one ground frame, VP_W x VP_H       */

/* the control positions, in screen coordinates: the WINDOW column's four
   buttons and two sliders, then the RENDERER column's rows on one pitch */
/* The Gamma slider ends at 352 and the panel runs to VP_Y + VP_H = 474, so the
   GPU row's caption at 364 and its control at 380 sit in free space -- no
   stock control moves and the panel does not grow.

   A WIDER CONTROL IS NOT AVAILABLE [MEASURED 2026-09-15]. A device name is
   ~23 characters ("NVIDIA GeForce RTX 4070") and this plate shows about
   thirteen, and giving the row the panel's full 255 px as a footer under both
   columns changes nothing: a stage button's art is `commongui.stagebuttnN`
   and that art is **120x20** (gui-gadgets.md 10.2), so the engine draws the
   same plate and clips the caption to it whatever `w` says -- a 255-px row
   renders exactly the same "NVIDIA GeForce" with the stage bars over its
   tail. The caption's width is not ours to set, so the row sits where it fits
   and `build_gpu_text` drops the part of the name that
   distinguishes nothing instead. */
static const short VC0_BTN[4] = { 96, 140, 184, 380 };
#define VC0_BTN_N (int)(sizeof VC0_BTN / sizeof VC0_BTN[0])
static const short VC0_SLD[2] = { 244, 336 };

/* THE VSYNC ROW IS A RENDERER ROW SHOWN IN THE WINDOW COLUMN. It is `s_row`'s,
   because the in-game panel carries it too, and it is found by name like every
   row of that table, so where the .GUI puts it is free. The RENDERER column
   has no room for a ninth: its slot would be 80 + 44 * 8 = 432, a control at
   448..468 whose recess meets the panel's bottom-right bolt at 465 -- and the
   panel cannot grow, the screen ends at 480. The WINDOW column has a slot
   between its two sliders: Screen Size ends at 260 and Gamma's caption is at
   320, and the row takes 276..312. The RENDERER column closes up over the gap
   (`vis_slot`). */
#define VSYNC_Y   276

/* A RENDERER row's slot in its column, or -1 for the one the WINDOW column
   carries */
static int vis_slot(int row)
{
    if (row == R_VSYNC) return -1;
    return row < R_VSYNC ? row : row - 1;
}

/* ONE GROUND, NOT FIFTEEN RECESSES. A small recess frame behind each control,
   with the stock background showing between them, cannot work: STARTOPT's
   background paints its own row of recess bars across x 267..405 -- the middle
   of the space, because it was drawn for ONE centred column -- so between two
   columns those bars show through as stripes behind our captions. They are the
   game's art and we neither ship nor repaint it, so the only way to be rid of
   them is to cover them. This panel does, and carries our own recesses on its
   face. */
static void draw_visbg(unsigned char* f, int w, int h)
{
    Surf s; int y, i;
    s.p = f; s.w = w; s.h = h;

    for (y = 0; y < h; y++) {
        int t = y * 3 / h;
        fillrect(&s, 0, y, w, 1,
                 (unsigned char)(t == 0 ? IX_GROUND_HI :
                                 t == 1 ? IX_GROUND_MID : IX_GROUND_LO));
    }
    hline(&s, 0, w - 1, 0, IX_EDGE);
    hline(&s, 0, w - 1, h - 1, IX_EDGE);
    vline(&s, 0, 0, h - 1, IX_EDGE);
    vline(&s, w - 1, 0, h - 1, IX_EDGE);
    hline(&s, 1, w - 2, 1, IX_BEVEL_HI);
    vline(&s, 1, 1, h - 2, IX_BEVEL_HI);
    hline(&s, 1, w - 2, h - 2, IX_BEVEL_LO);
    vline(&s, w - 2, 1, h - 2, IX_BEVEL_LO);

    bolt(&s, 8, 8);
    bolt(&s, w - 9, 8);
    bolt(&s, 8, h - 9);
    bolt(&s, w - 9, h - 9);

    /* a rule under each column heading */
    for (i = 0; i < 2; i++) {
        int x = (i ? VC1_X : VC0_X) - VP_X;
        hline(&s, x, x + VCOL_W - 1, VRULE_Y - VP_Y,     IX_EDGE);
        hline(&s, x, x + VCOL_W - 1, VRULE_Y - VP_Y + 1, IX_BEVEL_HI);
    }

    /* one recess per control, at the position the .GUI puts the control */
    for (i = 0; i < VC0_BTN_N; i++)
        recess(&s, VC0_X - VP_X - VPAD, VC0_BTN[i] - VP_Y - VPAD,
               VCOL_W + 2 * VPAD, VCTL_H + 2 * VPAD);
    for (i = 0; i < 2; i++)
        recess(&s, VC0_X - VP_X - VPAD, VC0_SLD[i] - VP_Y - VPAD,
               VSLD_W + 2 * VPAD, VSLD_H + 2 * VPAD);
    for (i = 0; i < s_nrows; i++) {     /* the RENDERER rows: one per row */
        int slot = vis_slot(i);
        if (slot < 0)
            recess(&s, VC0_X - VP_X - VPAD, VSYNC_Y + VCTL_DY - VP_Y - VPAD,
                   VCOL_W + 2 * VPAD, VCTL_H + 2 * VPAD);
        else
            recess(&s, VC1_X - VP_X - VPAD,
                   VROW_Y0 + VCTL_DY + VPITCH * slot - VP_Y - VPAD,
                   VCOL_W + 2 * VPAD, VCTL_H + 2 * VPAD);
    }
}


#define VIS_STOCK_N    5

typedef struct {
    int         id, assoc, x, y, w, h, attribs, colorf, cattr;
    const char* name;
    const char* text;           /* label/button caption, or NULL           */
    int         a, b;           /* button: quickkey, stages. slider: range, thick */
} VisStock;

/* The stock gadgets the store still leaves the engine, verbatim from the
   shipped VISUALS.GUI (extracted with tools/hpipack.py) except for x and y, in
   their own order. SHADING, ANTI and BSHADOWS and their captions are GONE:
   their bits are the store's (the engine options section), and `0x45E5E0`
   looks each up with `0x49FDF0`, which answers -1 for a missing name and is
   skipped [DISASSEMBLED 0x45E96A, 0x45E99E, 0x45E9D3]. GAMMA and VIDSLDR must
   stay, under those names: their lookup, `0x4A0200`, ends the process on a
   miss.

   ONLY x AND y MOVE. Every other field is the stock one, `assoc` included:
   243 is what binds the synthesized scroll arrows to VIDSLDR rather than to
   GAMMA (see `common` above), and `commonattribs` is what picks each caption's
   face. VIDSLDR's width goes 121 -> 122 so that it and GAMMA share one recess
   frame; that moves its knob track by one pixel and nothing else. */
static const VisStock s_visStock[VIS_STOCK_N] = {
    /* id  asc      x    y       w       h  att  cf  ca   name        text            a    b   */
    { 4,   0, VC0_X, 336, VSLD_W, VSLD_H,  1,  4,   0, "GAMMA",    NULL,          114, 20 },
    { 5,   0, VC0_X, 320, VCOL_W, VLBL_H, 18, 15, 104, "TEXT",     "Gamma",         0, 0 },
    { 4, 243, VC0_X, 244, VSLD_W, VSLD_H,  1,  4,   0, "VIDSLDR",  NULL,          114, 26 },
    { 5, 243, VC0_X, 228, VCOL_W, VLBL_H, 18, 15,   0, "VIDVAL",   "640x480",       0, 0 },
    { 5, 243, VC0_X, 212, VCOL_W, VLBL_H, 18, 15, 104, "VIDTEXT",  "Screen Size",   0, 0 },
};

/* ---- the WINDOW rows ----------------------------------------------------
   These four are the FORK's settings, not the engine's: they change the window
   the picture is presented in rather than what is drawn into it. That is why
   they are not in `s_row` and never appear on the in-game panel -- and why
   every one of them is applied on the thread that owns the window (below)
   rather than wherever the click happened to land. */
enum { VD_MODE, VD_MON, VD_SCALE, VD_GPU, VD_COUNT,
       /* not a row: a second message the Display mode row posts to itself, so
          the frame restore lands after the style restore the fork posts */
       VD_RESTORE_FRAME,
       /* not a row: Undo's UI scale, the exact value the screen opened with --
          a stage cannot carry "off" (s_scaleOpen) */
       VD_UNDO_SCALE };
static volatile LONG s_scaleOpen = -1;  /* the UI scale in force at open, -1 off */
/* Game thread: the window rows clicked since the screen opened, and the
   store's Monitor then (-1 none) -- what Undo puts back (vis_undo). */
static int s_vtouched[VD_COUNT];
static int s_monOpen = -1;
/* the store's Gamma and screen size at open: what Undo puts back */
static int s_gammaOpen = 12, s_resOpen;
static int vrow_held(int row);

#define VD_MONMAX 8
static char s_monText[VD_MONMAX * 20];
static RECT s_monRect[VD_MONMAX];
static char s_monDev[VD_MONMAX][CCHDEVICENAME + 1];  /* the device name the store keeps */
static int  s_monCount;
/* Is `s_vstage[VD_MON]` a CHOICE, or just the zero the array was born with?
   The difference matters to `tagpu_menu_monitor`, whose caller needs to fall
   back to the live window while the answer is "nobody has said". Set where the
   row is seeded from the window, and where the player actuates it. */
static int  s_monChosen;

/* HUD SCALE (tagpu_hud.h, gui-renderer.md 22), 0 = Auto. These are
   percentages of the HUD's stock size, and Auto is the ceiling H/480, where
   the panel exactly fills the screen height.

   SIX STAGES BECAUSE THE ROW CYCLES. A 25%-step ladder to 450% would be
   fifteen clicks to cross; these five stops span every surface a player can
   pick (the ceiling is 1.00 at 640x480, 1.60 at 1024x768, 2.25 at 1080p and
   4.50 at 4K). Stages past a
   screen's ceiling are SKIPPED as the row cycles rather than greyed: the
   engine's VA_SETGRAYED is per gadget, not per stage, so a greyed row would
   take the honourable stages down with the dishonourable ones. */
static const int SCALE_VAL[6] = { 0, 100, 150, 200, 300, 400 };

typedef struct {
    const char* name;
    const char* label;
    const char* text;
    int         stages;
} VisRow;

/* THE GPU ROW'S LABEL SAYS "(Vulkan)" AND THAT IS THE STATED LIMIT, not a
   decoration. The row binds the VULKAN device and nothing else, and under
   the GDI backend there is no device for it to bind.
   `vrow_greyed` greys the row whenever the Vulkan lane is not armed, so it
   never looks live while it cannot bite -- which is also what keeps the
   one-stage "(not listed yet)" row inert, since the engine REWRITES a
   `stages=1` button to 2 at `0x4A803C` and would otherwise have a second,
   captionless stage to cycle into. */
static char s_gpuText[TAGPU_VK_MAXGPU * (TAGPU_VK_NAMELEN + 1) + 24];

static VisRow s_vrow[VD_COUNT] = {
    { "VMODE",  "Display mode", "Window|Fullscreen",          2 },
    { "VMON",   "Monitor",      s_monText,                    0 },
    { "VSCALE", "UI scale",     "Auto|100%|150%|200%|300%|400%", 6 },
    { "VGPU",   "GPU (Vulkan)", s_gpuText,                    0 },
};
static int s_vstage[VD_COUNT];

static BOOL CALLBACK mon_cb(HMONITOR h, HDC dc, LPRECT clip, LPARAM p)
{
    MONITORINFOEXA mi;
    (void)dc; (void)clip; (void)p;
    if (s_monCount >= VD_MONMAX) return FALSE;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(h, (MONITORINFO*)&mi)) return TRUE;
    lstrcpynA(s_monDev[s_monCount], mi.szDevice, sizeof s_monDev[0]);
    s_monRect[s_monCount++] = mi.rcMonitor;
    return TRUE;
}

/* Built once at attach, because the `.GUI` is written then and a stage button's
   captions live in the file. A monitor hot-plugged afterwards is not offered
   until the next launch, which is the same bargain every other caption makes. */
static void enum_monitors(void)
{
    int i, at = 0;
    s_monCount = 0;
    s_monText[0] = 0;
    EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
    for (i = 0; i < s_monCount; i++) {
        at += _snprintf(s_monText + at, sizeof s_monText - at - 1, "%s%d: %ldx%ld",
                        i ? "|" : "", i + 1,
                        s_monRect[i].right - s_monRect[i].left,
                        s_monRect[i].bottom - s_monRect[i].top);
        if (at >= (int)sizeof s_monText - 1) break;
    }
    s_monText[sizeof s_monText - 1] = 0;
    if (s_monCount < 1) { lstrcpynA(s_monText, "1: default", sizeof s_monText); s_monCount = 1; }
    s_vrow[VD_MON].stages = s_monCount;

    /* The store keeps the monitor BY DEVICE NAME, because the enumeration order
       is not stable across a hot-plug; the list is handed over once, here,
       before any other thread exists, and never changes. A stored monitor that
       is attached is the choice from the first frame -- which is what places a
       fullscreen window on it (`util_target_monitor`). */
    {
        const char* names[VD_MONMAX];
        int m;
        for (i = 0; i < s_monCount; i++) names[i] = s_monDev[i];
        tagpu_settings_monitors(names, s_monCount);
        if (tagpu_settings_get(TS_MONITOR, &m) && m >= 0 && m < s_monCount) {
            s_vstage[VD_MON] = m;
            s_monChosen = 1;
        }
    }
}

/* The GPU row's captions, from the cache `tagpu_vk.gpus` -- which the previous
   launch's enumeration worker wrote, because this runs at DLL attach and a
   Vulkan instance may not be created there (tagpu_vk.h). Same shape as
   `enum_monitors`, same bargain: a card added since the last launch is offered
   at the next one.

   A machine with no cache yet, or with no Vulkan at all, gets ONE stage saying
   so -- never a zero-stage button, which the engine would divide by. */
/* THE PLATE SHOWS ABOUT THIRTEEN CHARACTERS and a device name is twice that,
   so the caption drops the longest LEADING RUN OF WHOLE WORDS THAT EVERY
   DEVICE SHARES. That is not a vendor table and not a guess: it is exactly the
   text that distinguishes none of them, computed from the list in front of us.
   Two NVIDIA cards would otherwise both plate as "NVIDIA GeForce" -- the row
   would look broken and be useless -- and stripping that prefix leaves
   "RTX 4070" and "RTX 3060", which is the whole of what the player is
   choosing between. A list whose names differ from the first character (the
   reference setup's 4070 beside llvmpipe) has no common prefix and is left
   alone, and the engine clips the long one; the full name reaches `tagpu.log`
   and the stored cfg either way. Never applied to a single-device list, where
   the "shared" prefix would be the entire name. */
static int gpu_common_prefix(int n)
{
    int k = 0, i;
    if (n < 2) return 0;
    for (;;) {
        char c = tagpu_vk_gpu_name(0)[k];
        if (!c) break;
        for (i = 1; i < n; i++)
            if (tagpu_vk_gpu_name(i)[k] != c) c = 0;
        if (!c) break;
        k++;
    }
    while (k && tagpu_vk_gpu_name(0)[k - 1] != ' ') k--;   /* whole words only */
    /* and never so far that some device is left with nothing to show */
    for (i = 0; i < n; i++)
        if (!tagpu_vk_gpu_name(i)[k]) return 0;
    return k;
}

static void build_gpu_text(void)
{
    int n = tagpu_vk_gpu_count(), i, at = 0, cut;

    /* stage 0 is Auto: the lane ranks the devices itself (tagpu_vk.c) */
    lstrcpynA(s_gpuText, "Auto", sizeof s_gpuText);
    at = 4;
    if (n > TAGPU_VK_MAXGPU) n = TAGPU_VK_MAXGPU;
    cut = gpu_common_prefix(n);
    for (i = 0; i < n; i++) {
        int k = _snprintf(s_gpuText + at, sizeof s_gpuText - at - 1, "|%s",
                          tagpu_vk_gpu_name(i) + cut);
        /* `_snprintf` returns -1 on truncation AND leaves no terminator, so a
           break has to put one back -- otherwise the tail of a half-written
           caption would run on into whatever the buffer held. It cannot
           truncate at today's bounds; it is written this way so that it still
           cannot the day TAGPU_VK_MAXGPU or NAMELEN moves. */
        if (k < 0 || k >= (int)(sizeof s_gpuText - at - 1)) { s_gpuText[at] = 0; n = i; break; }
        at += k;
    }
    s_gpuText[sizeof s_gpuText - 1] = 0;
    /* NEVER A ZERO-STAGE BUTTON: the engine's own advance wraps against the
       stage count and would divide by it. One stage saying why is the answer
       to "no cache yet" and to "no Vulkan runtime installed" alike. */
    if (!n) lstrcpynA(s_gpuText, "(not listed yet)", sizeof s_gpuText);
    s_vrow[VD_GPU].stages = n + 1;
}

/* Declared in tagpu_menu.h, called by `util_target_monitor`.

   READ FROM A THREAD THAT IS NOT THE ONE THAT WRITES, and safe by construction
   rather than by the two being the same thread today. `s_monRect` and
   `s_monCount` are written once, in `enum_monitors` at DLL attach, before any
   other thread exists, and never again -- a monitor hot-plugged later is not
   offered until the next launch, which is the bargain the captions already make.
   `s_vstage[VD_MON]` is a naturally aligned int, so the read cannot tear, and it
   is BOUNDED against `s_monCount` here before it indexes anything: the worst a
   racing click can do is hand back the monitor selected one click ago. A stale
   `s_monChosen` reads as "nobody has chosen", whose answer is
   `util_default_monitor` -- the correct fallback, not a wrong rect. */
BOOL tagpu_menu_monitor(RECT* out)
{
    int i = s_vstage[VD_MON];
    if (!out || !s_monChosen || i < 0 || i >= s_monCount) return FALSE;
    if (s_monRect[i].right <= s_monRect[i].left ||
        s_monRect[i].bottom <= s_monRect[i].top) return FALSE;
    *out = s_monRect[i];
    return TRUE;
}

static int build_visuals_gui(char* b, int cap, int rows)
{
    int at = 0, i, g = 1;

    at = gput(b, cap, at, "[GADGET0]\r\n\t{\r\n");
    at = common(b, cap, at, 0, 0, "visuals.GUI", 0, 0, 639, 480, 0, 0, 1);
    at = gput(b, cap, at,
        "\ttotalgadgets=%d;\r\n"
        "\t[VERSION]\r\n\t\t{\r\n\t\tmajor=1;\r\n\t\tminor=0;\r\n\t\trevision=1;\r\n\t\t}\r\n"
        "\tpanel=;\r\n\tcrdefault=;\r\n\tescdefault=;\r\n\tdefaultfocus=;\r\n\t}\r\n",
        1 + VIS_STOCK_N + 2 + (rows + VD_COUNT) * 2);

    /* THE GROUND FIRST, so every caption and control is drawn on top of it --
       gadgets are drawn in array order, and ours are appended to STARTOPT's,
       so this covers the background's own recess bars and nothing else. */
    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 12, 0, ART_VISBG, VP_X, VP_Y, VP_W, VP_H, 0, 15, 0);
    at = gput(b, cap, at, "\t}\r\n");

    /* ---- the stock gadgets, moved but otherwise verbatim ----------------- */
    for (i = 0; i < VIS_STOCK_N; i++) {
        const VisStock* s = &s_visStock[i];
        at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
        at = common(b, cap, at, s->id, s->assoc, s->name, s->x, s->y, s->w, s->h,
                    s->attribs, s->colorf, s->cattr);
        if (s->id == 1)
            at = gput(b, cap, at, "\tstatus=0;\r\n\ttext=%s;\r\n\tquickkey=%d;\r\n"
                                  "\tgrayedout=0;\r\n\tstages=%d;\r\n\t}\r\n",
                      s->text, s->a, s->b);
        else if (s->id == 4)
            at = gput(b, cap, at, "\trange=%d;\r\n\tthick=%d;\r\n\tknobpos=0;\r\n"
                                  "\tknobsize=10;\r\n\t}\r\n", s->a, s->b);
        else
            at = gput(b, cap, at, "\ttext=%s;\r\n\tlink=;\r\n\t}\r\n", s->text);
    }

    /* ---- the two column headings ----------------------------------------- */
    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 5, 0, "TEXT", VC0_X, VHDR_Y, VCOL_W, VLBL_H, 18, 15, 104);
    at = gput(b, cap, at, "\ttext=%s;\r\n\tlink=;\r\n\t}\r\n", "Window");
    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 5, 0, "TEXT", VC1_X, VHDR_Y, VCOL_W, VLBL_H, 18, 15, 104);
    at = gput(b, cap, at, "\ttext=%s;\r\n\tlink=;\r\n\t}\r\n", "Impure rendering");

    /* ---- the WINDOW rows -------------------------------------------------- */
    {
        static const short dy[VD_COUNT] = { 80, 124, 168, 364 };
        for (i = 0; i < VD_COUNT; i++) {
            at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
            at = common(b, cap, at, 5, 0, "TEXT", VC0_X, dy[i], VCOL_W, VLBL_H, 18, 15, 104);
            at = gput(b, cap, at, "\ttext=%s;\r\n\tlink=;\r\n\t}\r\n", s_vrow[i].label);

            at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
            at = common(b, cap, at, 1, 0, s_vrow[i].name, VC0_X, dy[i] + VCTL_DY,
                        VCOL_W, VCTL_H, 1, 0, 0);
            at = gput(b, cap, at, "\tstatus=0;\r\n\ttext=%s;\r\n\tquickkey=0;\r\n"
                                  "\tgrayedout=0;\r\n\tstages=%d;\r\n\t}\r\n",
                      s_vrow[i].text, s_vrow[i].stages);
        }
    }

    /* ---- the RENDERER rows: one slot each, top down, Vsync aside ---------- */
    for (i = 0; i < rows; i++) {
        int slot = vis_slot(i);
        int x = slot < 0 ? VC0_X : VC1_X;
        int y = slot < 0 ? VSYNC_Y : VROW_Y0 + VPITCH * slot;
        at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
        at = common(b, cap, at, 5, 0, "TEXT", x, y, VCOL_W, VLBL_H, 18, 15, 104);
        at = gput(b, cap, at, "\ttext=%s;\r\n\tlink=;\r\n\t}\r\n", s_row[i].label);

        at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
        at = common(b, cap, at, 1, 0, s_row[i].name, x, y + VCTL_DY,
                    VCOL_W, VCTL_H, 1, 0, 0);
        at = gput(b, cap, at, "\tstatus=0;\r\n\ttext=%s;\r\n\tquickkey=0;\r\n"
                              "\tgrayedout=0;\r\n\tstages=%d;\r\n\t}\r\n",
                  s_row[i].text, s_row[i].stages);
    }
    return at;
}



/* ---- the in-game Visuals screen, VISUALRT.GUI -----------------------------
   The stock screen is Gamma, three toggles and Restore/Undo on the shared
   `commongui.visualsrt` art, whose recesses ARE its layout (gui-gadgets.md
   10.3). The three toggles are the store's now -- Shadows is the sprocket
   panel's row, Shading and Anti-alias are pinned (the engine options section)
   -- so the screen keeps Gamma and the two actions, on a ground of our own:
   left on the stock art, their three recesses would be empty bands.

   THE SAME RECT AND THE SAME NAMES as stock: the header at (128,128) 150x352,
   GAMMA, RESTORE and UNDO where the stock file puts them. `0x45E5E0` finds
   GAMMA by name through `0x4A0200`, which is FATAL on a miss ("Error in GUI
   layout"), so that name is not ours to change; the three it skips when
   absent (`0x49FDF0` returns -1) are simply not here. The ground is `id=12`
   VRTBG in `anims/visualrt.gaf` -- the screen's own GAF, which stock does not
   ship, so nothing is shadowed. */
#define VRT_W      150
#define VRT_H      352
#define ART_VRTBG  "VRTBG"
#define VRT_FILE   "guis/visualrt.gui"
#define VRT_GAF    "anims/visualrt.gaf"

static void draw_vrtbg(unsigned char* f, int w, int h)
{
    Surf s; int y;
    s.p = f; s.w = w; s.h = h;
    for (y = 0; y < h; y++) {
        int t = y * 3 / h;
        fillrect(&s, 0, y, w, 1,
                 (unsigned char)(t == 0 ? IX_GROUND_HI :
                                 t == 1 ? IX_GROUND_MID : IX_GROUND_LO));
    }
    hline(&s, 0, w - 1, 0, IX_EDGE);
    hline(&s, 0, w - 1, h - 1, IX_EDGE);
    vline(&s, 0, 0, h - 1, IX_EDGE);
    vline(&s, w - 1, 0, h - 1, IX_EDGE);
    hline(&s, 1, w - 2, 1, IX_BEVEL_HI);
    vline(&s, 1, 1, h - 2, IX_BEVEL_HI);
    hline(&s, 1, w - 2, h - 2, IX_BEVEL_LO);
    vline(&s, w - 2, 1, h - 2, IX_BEVEL_LO);
    bolt(&s, 6, 6);
    bolt(&s, w - 7, 6);
    bolt(&s, 6, h - 7);
    bolt(&s, w - 7, h - 7);
    recess(&s, 13 - VPAD, 24 - VPAD, 120 + 2 * VPAD, 16 + 2 * VPAD);    /* GAMMA   */
    recess(&s, 13 - VPAD, 269 - VPAD, 120 + 2 * VPAD, 20 + 2 * VPAD);   /* RESTORE */
    recess(&s, 13 - VPAD, 304 - VPAD, 120 + 2 * VPAD, 20 + 2 * VPAD);   /* UNDO    */
}

static int build_visualrt_gui(char* b, int cap)
{
    int at = 0, g = 1;
    at = gput(b, cap, at, "[GADGET0]\r\n\t{\r\n");
    at = common(b, cap, at, 0, 0, "HEADER", 128, 128, VRT_W, VRT_H, 0, 0, 1);
    at = gput(b, cap, at,
        "\ttotalgadgets=%d;\r\n"
        "\t[VERSION]\r\n\t\t{\r\n\t\tmajor=1;\r\n\t\tminor=0;\r\n\t\trevision=1;\r\n\t\t}\r\n"
        "\tpanel=;\r\n\tcrdefault=;\r\n\tescdefault=;\r\n\tdefaultfocus=;\r\n\t}\r\n", 7);

    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 12, 0, ART_VRTBG, 0, 0, VRT_W, VRT_H, 0, 15, 0);
    at = gput(b, cap, at, "\t}\r\n");

    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 5, 0, "TEXT", 13, 8, 114, 14, 1, 15, 0);
    at = gput(b, cap, at, "\ttext=Gamma;\r\n\tlink=;\r\n\t}\r\n");
    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 4, 0, "GAMMA", 13, 24, 120, 16, 1, 4, 0);
    at = gput(b, cap, at, "\trange=104;\r\n\tthick=20;\r\n\tknobpos=0;\r\n\tknobsize=10;\r\n\t}\r\n");

    /* where the rest went: the render rows live on the sprocket panel */
    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 5, 0, "TEXT", 13, 60, 120, 14, 1, 15, 0);
    at = gput(b, cap, at, "\ttext=Rendering:;\r\n\tlink=;\r\n\t}\r\n");
    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 5, 0, "TEXT", 13, 76, 120, 14, 1, 15, 0);
    at = gput(b, cap, at, "\ttext=the cog, top right;\r\n\tlink=;\r\n\t}\r\n");

    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 1, 0, "RESTORE", 13, 269, 120, 20, 2, 15, -56);
    at = gput(b, cap, at, "\tstatus=0;\r\n\ttext=Restore Defaults;\r\n\tquickkey=99;\r\n"
                          "\tgrayedout=0;\r\n\tstages=0;\r\n\t}\r\n");
    at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", g++);
    at = common(b, cap, at, 1, 0, "UNDO", 13, 304, 120, 20, 2, 15, -56);
    at = gput(b, cap, at, "\tstatus=0;\r\n\ttext=Undo Changes;\r\n\tquickkey=99;\r\n"
                          "\tgrayedout=0;\r\n\tstages=0;\r\n\t}\r\n");
    return (g == 8) ? at : -1;
}

/* ---- applying a WINDOW row ----------------------------------------------
   ON THE THREAD THAT OWNS THE WINDOW, ALWAYS. Every one of these ends in a
   window call -- `SetWindowPos`, or `dd_SetDisplayMode`, which tears the
   presentation down and rebuilds it -- and a cross-thread window call is not an
   error you see, it is a wait for a message pump that is not running. OnCommand
   runs wherever the engine's GUI pump runs; rather than rest on that being the
   window's thread (it is today, and nothing promises it stays), the click POSTS
   and the wndproc does the work. That is the contract `tagpu_shield.c` already
   uses for injected input, and it holds by construction rather than by
   measurement. */
static void apply_display(int row)
{
    if (g_ddraw.hwnd) PostMessageA(g_ddraw.hwnd, WM_TAGPU_DISPLAY, (WPARAM)row, 0);
}

static void move_to_monitor(int i)
{
    const RECT* m;
    if (i < 0 || i >= s_monCount || !g_ddraw.hwnd) return;
    m = &s_monRect[i];
    if (g_config.fullscreen)
        /* Borderless fullscreen: the window IS the monitor, so its position,
           its size AND the render target all come from the monitor -- and
           `dd_SetDisplayMode(0, 0, 0, 0)`, the fork's own "re-apply the current
           mode", does all three from `util_target_monitor`, which already reads
           the row the player just changed. Moving the window here as well only
           fights it: measured 2026-09-11, a `SetWindowPos` to the monitor rect
           followed by the re-apply left the window one pixel taller than the
           screen and back at the primary's origin, because the re-apply places
           it last. Leaving the render target alone is not an option either --
           4K -> the portrait screen then kept a 3840x2160 framebuffer inside a
           1080x1920 window. */
        dd_SetDisplayMode(0, 0, 0, 0);
    else
        real_SetWindowPos(g_ddraw.hwnd, HWND_TOP, m->left + 32, m->top + 32,
                          0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

/* THE SCALE IS AGAINST THE SCREEN SIZE ROW, NOT THE LIVE SURFACE. This screen
   is the SHELL's, and the shell's surface is atom-locked at 640x480 whatever
   the player's Screen Size says -- so a scale resolved against the live
   `g_ddraw.width` would be answered here for a 640x480 screen and mean
   something else entirely the moment a game started.
   `main+0x37F1B/+0x37F1F` is the mode the Screen Size slider writes
   (`0x45BBF0`, and `0x45E3AD` restores 640x480 into it), which is the number
   the player just chose one row up, and it is also the number game entry
   copies into the engine's screen dimensions -- so it is the screen HUD
   scale's ceiling has to be taken against.
   The live surface is the fallback for the case where the field has not been
   written. */
static void sel_mode(int* w, int* h)
{
    char* main_p = *(char**)TA_MAIN;
    int sw = 0, sh = 0;
    if (main_p) {
        sw = *(int*)(main_p + OFF_SELW);
        sh = *(int*)(main_p + OFF_SELH);
    }
    if (sw < 320 || sh < 240 || sw > 8192 || sh > 8192) { sw = g_ddraw.width; sh = g_ddraw.height; }
    *w = sw; *h = sh;
}

/* Can this stage be honoured on the mode the player has chosen? Auto always
   can -- it IS the ceiling -- and 100% always can, because tagpu_hud_geom
   never resolves below stock. */
static int scale_stage_ok(int stage)
{
    int sw, sh;
    if (stage <= 0 || stage >= (int)(sizeof SCALE_VAL / sizeof SCALE_VAL[0])) return 1;
    sel_mode(&sw, &sh);
    return SCALE_VAL[stage] <= tagpu_hud_ceiling_pct(sw, sh);
}

/* The windowed client, remembered across a trip to fullscreen. MEASURED
   2026-09-11: `util_toggle_fullscreen`'s borderless path leaves the window at
   the monitor's size on the way BACK -- 1024x768 -> 3840x2160 -> 3840x2160 --
   so the row would be a one-way door. The size is ours to put back, and this is
   a value we saved rather than a timing guess. */
static RECT s_winFrame;         /* the frame's own screen rect */
static int  s_winSaved;

BOOL tagpu_menu_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result)
{
    (void)hwnd; (void)lparam;
    if (msg != WM_TAGPU_DISPLAY) return FALSE;
    switch ((int)wparam) {
    case VD_MODE:
        if (!!g_config.fullscreen != !!s_vstage[VD_MODE]) {
            if (s_vstage[VD_MODE])                    /* window -> fullscreen */
                s_winSaved = GetWindowRect(g_ddraw.hwnd, &s_winFrame) ? 1 : 0;
            util_toggle_fullscreen();
            /* PUT THE FRAME BACK, RECT AND ALL -- and LATER, not here.
               `g_config.window_rect` is no good for it: the wndproc re-derives
               that field from the frame on every move ("save new window
               position") and un-adjusts it by the CURRENT style, so a size
               written while the window is still borderless comes back one
               caption short -- measured 2026-09-11, three round trips walked
               the window 118 -> 155 -> 192 -> 229 down the screen, losing 37 px
               of height each time with its bottom edge pinned.

               And a `SetWindowPos` right here loses too: `dd_SetDisplayMode`
               POSTS `WM_RESTORE_STYLE` (wndproc.c) rather than restoring the
               style inline, so that handler runs after we return and re-applies
               its own geometry over ours -- which left the window stuck at the
               monitor's size. Posting puts our restore BEHIND it in the same
               queue, which is an ordering rather than a delay. */
            if (!s_vstage[VD_MODE] && s_winSaved)
                PostMessageA(g_ddraw.hwnd, WM_TAGPU_DISPLAY, (WPARAM)VD_RESTORE_FRAME, 0);
        }
        break;

    /* KNOWN GAP UNDER WINE, and this is the correct Win32 either way: leaving
       borderless fullscreen should put the frame back where it was. On the
       reference setup it does not, and the cause is outside the process --
       measured 2026-09-11, the X window still carries `_NET_WM_STATE_FULLSCREEN`
       after the return even though its caption is back (`_NET_FRAME_EXTENTS`
       reads 0,0,37,0), and a WM-fullscreen window ignores SetWindowPos. Three
       things were tried and none dropped that state: restoring through
       `g_config.window_rect` + `dd_SetDisplayMode`, a direct SetWindowPos
       inline, and `SW_RESTORE` before the move. Posting is still right (the
       fork posts `WM_RESTORE_STYLE`, so an inline move is overwritten), and the
       call is kept because it is what a native Windows build needs; the
       residue is that the window comes back the monitor's size rather than its
       own. The PICTURE is correct either way -- the viewport is centred and the
       mouse scale matches it -- so this is ergonomics, not corruption. */
    case VD_RESTORE_FRAME:
        if (s_winSaved && !g_config.fullscreen)
            real_SetWindowPos(g_ddraw.hwnd, NULL,
                              s_winFrame.left, s_winFrame.top,
                              s_winFrame.right - s_winFrame.left,
                              s_winFrame.bottom - s_winFrame.top,
                              SWP_NOZORDER | SWP_NOACTIVATE);
        break;
    case VD_MON:
        move_to_monitor(s_vstage[VD_MON]);
        break;
    case VD_UNDO_SCALE:
        if (!tagpu_hud_held()) tagpu_hud_store_pct((int)s_scaleOpen);
        break;
    /* HUD scale touches no window, and it touches no engine memory either --
       the store puts it in force as it writes it, so the next composited frame
       is already at the new scale. Writing it on the window thread keeps every
       row of this screen on one thread, which is the contract the comment
       above apply_display states. */
    case VD_SCALE:
        if (!tagpu_hud_held()) tagpu_hud_store_pct(SCALE_VAL[s_vstage[VD_SCALE]]);
        break;
    /* THE GPU ROW TOUCHES NO WINDOW AND NO VULKAN OBJECT. It records the
       request and bumps a generation counter; the RENDER thread, which is the
       only owner of the device, notices the counter on its next frame and
       rebuilds. That is an ordering rather than a hand-off: nothing here can
       reach an object the other thread is using, because nothing here reaches
       an object at all. It rides this message with the rest for the reason the
       comment above `apply_display` gives -- one screen, one thread. */
    /* The store's flag is raised AFTER the select, so the render thread's
       exchange of it orders its read of the chosen name after the choice. */
    case VD_GPU:
        /* stage 0 is Auto, stage k the cache's device k-1 */
        tagpu_vk_gpu_select(s_vstage[VD_GPU] ? s_vstage[VD_GPU] - 1 : TAGPU_VK_GPU_AUTO);
        if (!vrow_held(VD_GPU)) InterlockedExchange(&s_vkDirty, 1);
        break;
    }
    *result = 0;
    return TRUE;
}

/* The plates are read from the fork's live settings on the way in, which is
   what the store put in force at attach or a click has put in force since. */
static void read_display_state(void)
{
    int i, k;
    HMONITOR h;

    s_vstage[VD_MODE] = g_config.fullscreen ? 1 : 0;

    s_vstage[VD_MON] = 0;
    h = g_ddraw.hwnd ? MonitorFromWindow(g_ddraw.hwnd, MONITOR_DEFAULTTONEAREST) : NULL;
    if (h) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(h, &mi))
            for (i = 0; i < s_monCount; i++)
                if (s_monRect[i].left == mi.rcMonitor.left &&
                    s_monRect[i].top  == mi.rcMonitor.top) {
                    s_vstage[VD_MON] = i; s_monChosen = 1; break;
                }
    }

    /* HUD scale is read back out of what the next game entry will read -- the
       lever, else the store -- not out of anything live, because a game may not
       have started yet. OFF plates as 100%, the stock size it draws at, and an
       unrecognised percentage as Auto rather than inventing a stage. */
    {
        int pct = tagpu_hud_stored_pct();
        s_vstage[VD_SCALE] = pct < 0 ? 1 : 0;
        for (k = 1; pct > 0 && k < (int)(sizeof SCALE_VAL / sizeof SCALE_VAL[0]); k++)
            if (SCALE_VAL[k] == pct) { s_vstage[VD_SCALE] = k; break; }
    }

    /* AUTO PLATES AUTO; A NAMED CHOICE PLATES THE DEVICE IN USE, so the row
       can be verified rather than trusted. `tagpu_vk_gpu_active` is what the
       render thread actually bound; it is -1 while the lane is down, and only
       then does a named row show the request. So a choice that could not be
       honoured shows as the device that was. */
    k = tagpu_vk_gpu_stored();
    if (k != TAGPU_VK_GPU_AUTO) {
        int a = tagpu_vk_gpu_active();
        if (a >= 0) k = a;
    }
    s_vstage[VD_GPU] = (k >= 0 && k < tagpu_vk_gpu_count()) ? k + 1 : 0;
}

/* A row that cannot bite is greyed rather than left looking live -- the same
   rule `row_greyed` applies to the render column. */
/* GREYED FROM THE MODEL, NOT FROM `g_config`. The window work is POSTED, so
   `g_config.fullscreen` still holds the old value when `push_display` runs
   immediately after a click -- greying from it would leave the plate saying
   "Window" and UI scale greyed at the same time, one click behind.
   `s_vstage[VD_MODE]` is what the player just asked for. */
/* A row a lever file holds, or every row when the store is ignored: a click
   there could not outlive the launch, so it is greyed (renderers.md 2.10b). */
static int vrow_held(int row)
{
    if (tagpu_settings_ignored()) return 1;
    return row == VD_SCALE && tagpu_hud_held();
}

static int vrow_greyed(int row)
{
    if (vrow_held(row))  return 1;
    if (row == VD_MON)   return s_monCount < 2;
    /* Greyed unless there is a choice to make AND something that would act on
       it. `tagpu_vk_armed()` is 0 under the GDI backend with no `tagpu_vk.on`
       (tagpu_vk.h), where the row retargets nothing, and a live-looking row
       that changes no pixel is exactly what this rule exists to prevent. */
    if (row == VD_GPU)   return tagpu_vk_gpu_count() < 2 || !tagpu_vk_armed();
    /* VD_SCALE IS LIVE IN BOTH MODES: HUD scale is inside the picture and
       means the same thing windowed or not (gui-renderer.md 22.2, "Row"). */
    return 0;
}

static void push_display(void* gi)
{
    int i;
    for (i = 0; i < VD_COUNT; i++)
        ((set_status_fn)VA_SETSTATUS)(gi, s_vrow[i].name, s_vstage[i]);
    for (i = 0; i < VD_COUNT; i++)
        ((set_grayed_fn)VA_SETGRAYED)(gi, s_vrow[i].name, vrow_greyed(i));
    ((set_dirty_fn)VA_SETDIRTY)(gi);
}

/* The WINDOW rows into the store. UI scale is not here: `tagpu_hud_store_pct`
   records it as it puts it in force, on the window thread. Nor is the GPU: its
   value is a name, handed over by the render thread (`tagpu_menu_present`)
   once the window thread's select has run. */
static void commit_display(int d)
{
    if (vrow_held(d)) return;
    switch (d) {
    case VD_MODE: tagpu_settings_set(TS_DISPLAY, s_vstage[VD_MODE] ? 1 : 0); break;
    case VD_MON:  tagpu_settings_set(TS_MONITOR, s_vstage[VD_MON]); break;
    default:      break;
    }
}

/* ---- Restore Default and Undo Changes ------------------------------------
   Both are STARTOPT's own buttons. The engine's branches -- RESTORE
   (`0x45E331`) and UNDO (`0x45E2FD`) -- reset the engine's options from its
   own defaults or the Options stash, which the store owns now, so neither is
   forwarded: we set the model and the store, push the engine's values, and
   end the way those branches end, `GUI_Pop` then `0x45E5E0(0)` (`vis_relist`).

   That rebuild runs `vis_build_after`, which would normally re-read the
   values in force -- and those still hold the OLD values, because Classic++
   re-reads the store on its own poll and the window rows are applied by a
   POSTED message. `s_visKeep` is how the rebuild is told the model is
   authoritative this once; it is the same bargain `menu_open(fresh = 0)`
   makes for the in-game panel's recovery. */
static int s_visKeep;
static int s_stageOpen[R_COUNT];
static int s_vstageOpen[VD_COUNT];

/* The actuated gadget's name, or NULL. */
static const char* vis_actuated(void* gi)
{
    char* main_p = *(char**)TA_MAIN;
    char* top;
    char* ctrls;
    int idx;

    if (!gi || !main_p) return 0;
    idx = *(int*)((char*)gi + GI_UICHANGE);
    if (idx < 0) return 0;
    top   = *(char**)(main_p + OFF_TOPGUI);
    ctrls = top ? *(char**)(top + GM_CTRLS) : 0;
    if (!ctrls || idx < 1 || idx > *(short*)(ctrls + 0xB6)) return 0;
    return ctrls + (size_t)idx * STRIDE + G_NAME;
}

/* UNDO -- back to what the screen opened with, the two rows that move the
   window included. This is the escape hatch for a Display mode or Monitor the
   player cannot see the menu on any more, so it is the one place those two ARE
   put back.

   ONLY THE WINDOW ROWS THE VISIT TOUCHED, AND TO WHAT THE STORE HELD. A plate
   shows a resolved value -- the window's monitor for a store that names
   none, the device that came up for a stored GPU that could not -- so putting
   an untouched row back from its plate would pin what the player never chose.
   The Monitor goes back as the store's own value, "none" included; UI scale as
   its exact value, off included. A row a lever holds is never put in force. */
static void vis_undo(void)
{
    int i;
    memcpy(s_stage,  s_stageOpen,  sizeof s_stage);
    memcpy(s_vstage, s_vstageOpen, sizeof s_vstage);
    commit_all_rows();
    for (i = 0; i < VD_COUNT; i++) {
        if (!s_vtouched[i] || vrow_held(i)) continue;
        apply_display(i == VD_SCALE ? VD_UNDO_SCALE : i);
        if (i == VD_MON) tagpu_settings_set(TS_MONITOR, s_monOpen);
        else commit_display(i);
    }
    memset(s_vtouched, 0, sizeof s_vtouched);
    /* the engine's two, as the store held them at open, and in force before
       the rebuild seeds their sliders from memory */
    tagpu_settings_set(TS_GAMMA, s_gammaOpen);
    tagpu_settings_set(TS_RESOLUTION, s_resOpen);
    eng_push(ENG_ALL);
    s_visKeep = 1;
}

/* RESTORE -- the store's defaults (renderers.md 2.10b): Classic++ with every
   row it owns at its Classic++ value, supersampling on, the FPS counter off,
   vsync on, the map edge mirrored and UI scale off. A row a lever holds keeps the
   lever's value: Restore changes the store, and the store is not what draws
   that row.

   DISPLAY MODE AND MONITOR ARE DELIBERATELY NOT RESTORED. "Restore defaults"
   would otherwise move the player's window to another monitor, or into
   fullscreen, in one click -- and if the default lands where they cannot see
   the screen, the button that would put it back is on the screen. Undo is the
   row that moves the window, because there the player has just moved it
   themselves and is asking for it back. */
static void vis_restore(void)
{
    int keep[R_COUNT], i;
    memcpy(keep, s_stage, sizeof keep);
    s_stage[R_STYLE]   = STYLE_PP;
    s_stage[R_ASSETS]  = tagpu_rguard_off() ? 2 : 1;
    s_stage[R_LIGHT]   = 1;
    s_stage[R_SHADOWS] = SHADOWS_HARD_STAGE;
    s_stage[R_SHADOWQ] = 2;
    s_stage[R_SS]      = 1;
    s_stage[R_FPS]     = 0;
    s_stage[R_VSYNC]   = 1;
    s_stage[R_EDGE]    = 1;
    for (i = 0; i < R_COUNT; i++) if (row_held(i)) s_stage[i] = keep[i];
    commit_all_rows();
    /* UI scale back to OFF, the store's default: HUD scale is off the play
       defaults (tagpu_opt.c), so "Restore defaults" must not be what arms it.
       No stage of the row means off, so the pass is turned off here directly,
       and the row plates 100% -- stock size, which is what off draws. */
    if (!vrow_held(VD_SCALE)) {
        tagpu_hud_store_pct(-1);
        s_vstage[VD_SCALE] = 1;
    }
    /* The engine's own defaults, as its RESTORE had them (0x45E331) -- Gamma
       12 and, off the front end's screen only, dithered fog off -- except the
       screen size, which is the store's `native` rather than 640x480. */
    tagpu_settings_set(TS_GAMMA, 12);
    tagpu_settings_set(TS_RESOLUTION, 0);
    {
        char* m = *(char**)TA_MAIN;
        if (m) *(unsigned short*)(m + OFF_OPTWORD) &= (unsigned short)~OPT_DITHERFOG;
    }
    eng_push(ENG_ALL);
    s_visKeep = 1;
}

/* ---- the Monitor row rebuilds the screen ---------------------------------
   THE SCREEN SIZE LIST BELONGS TO A MONITOR. The engine builds it once per
   visit, in `0x45E5E0` (`0x45E6B0` allocates the header into GUIMEMSTRUCT+0x0C
   and hangs the table off VIDSLDR at `0x45E726`), out of whatever our
   `EnumDisplayModes` serves at that moment -- and what we serve is capped to
   the monitor (`util_target_monitor`, dd.c). So a Monitor row that changed
   the monitor and left the list alone would be offering the OTHER screen's
   sizes.

   There is no engine call for "re-enumerate in place", and there does not need
   to be: `GUI_Pop` + `0x45E5E0(0)` is the engine's own idiom for "this screen's
   contents are stale, build it again", used verbatim by both UNDO (`0x45E31E`)
   and RESTORE. Calling it from OnCommand is calling it from exactly where the
   engine does. `GUI_Pop` also writes -1 into `gi->UIChange_f` (`0x4A9673`), so
   the pump is answered and there is no `menu_accept` to do -- the same reason
   the engine's own two branches do not answer it either.

   `s_visKeep` is what keeps the model across the rebuild: `vis_build_after`
   would otherwise run `read_display_state`, which re-derives the Monitor row
   from the window -- and the window has not moved yet, because that move is
   posted. With it, the rebuilt plates show the row the player just chose. */
static void vis_relist(void* gi)
{
    s_visKeep = 1;
    ((gui_pop_fn)VA_GUI_POP)(gi);
    ((vis_build_fn)VA_VIS_BUILD)(0);
}

/* Which WINDOW row was actuated, or -1. `menu_row_of`'s twin, by name for the
   same reason. */
static int vis_row_of(void* gi)
{
    char* main_p = *(char**)TA_MAIN;
    char* top;
    char* ctrls;
    int idx, i;

    if (!gi || !main_p) return -1;
    idx = *(int*)((char*)gi + GI_UICHANGE);
    if (idx < 0) return -1;
    top   = *(char**)(main_p + OFF_TOPGUI);
    ctrls = top ? *(char**)(top + GM_CTRLS) : 0;
    if (!ctrls || idx < 1 || idx > *(short*)(ctrls + 0xB6)) return -1;
    for (i = 0; i < VD_COUNT; i++)
        if (!memcmp(ctrls + (size_t)idx * STRIDE + G_NAME, s_vrow[i].name,
                    strlen(s_vrow[i].name) + 1)) return i;
    return -1;
}

/* ---- the engine's Visuals options, owned by the store --------------------
   renderers.md 2.10b; every address in exe-reverse-engineering.md, "The
   Visuals options the store owns".

   FOUR FIELDS OF `main`:
     +0x37F08  Gamma, a signed dword 0..20; the option screens apply
               0.5 + Gamma/24 through SetGamma 0x4BA590 [DISASSEMBLED 0x45BD86]
     +0x37F06  the option WORD. Bits 2..4 (Shadow, TShadow, FShadow) follow the
               Shadows row -- one stock click set all three (0x45E1CB). Bits 1
               (Anti-alias) and 5 (Shading) are PINNED ON: their only readers
               are the engine's own unit bake (0x459830, 0x459C70, 0x4586A0),
               which no Vulkan pass presents -- MEASURED 2026-09-23, 0 world
               pixels changed toggling either, Classic and Classic++ -- and on
               the GDI lane, which presents that bake, on is the stock default.
     +0x37F1B / +0x37F1F  the in-game screen size, dwords at odd offsets;
               written on the front end only, as every stock writer does.
   The registry is only a mirror: the engine loads it (0x42F9A0) and writes it
   back on its own (0x430F00 -- game entry, Options Done, the console toggles).

   WHEN. After the FIRST registry load the store is pushed: that is startup,
   0x4913F6 inside UIPipelinesInit, whose SetGamma at 0x4914A7 then applies
   ours. The two later loads (0x4273E8 reloads WITHOUT saving; 0x47BB52 saves
   first) put back what memory held before them, so a reload never changes an
   owned value. The battleroom's screen size and a `+gamma` are written to
   memory and saved to the registry (0x4462FC, 0x4172CE) but never reach the
   store, so they last until the next launch pushes it: session-only, which
   stock is not. CANCEL restores the engine's stash through 0x45CAE0 (taken
   when Options opened); the store is pushed again after it, because a Visuals
   row commits on its click, as ours always have.

   GAME THREAD ONLY: the loader, the slider callbacks, OnCommand and 0x45CAE0
   all run on it. Installed only with the menu's screens (tagpu_menu_init),
   which are the only way to write the store's copy; under tagpu_defaults.off,
   or without them, nothing here is. */

static const unsigned char OPT_LOAD_STOLEN[6]  = { 0x81, 0xEC, 0x24, 0x01, 0x00, 0x00 }; /* sub esp,0x124 */
static const unsigned char STASHBACK_STOLEN[6] = { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }; /* mov ecx,[0x511DE8] */

typedef void (__stdcall *setgamma_fn)(float factor);
typedef void (__fastcall *rebake_fn)(void* self);   /* thiscall: ecx = self, no stack args */
typedef void (__stdcall *slider_cb_fn)(void* gi, int arg);
/* plays a sound by name [role INFERRED]; stdcall, ret 8. The engine's UNDO and
   RESTORE branches call it with "Options" first (0x45E313, 0x45E34B). */
typedef void (__stdcall *opt_sound_fn)(const char* name, int arg);
#define VA_OPT_SOUND 0x0047F1A0u

static int  s_engArmed;                 /* the loader observer is in            */
static int  s_engLoads;                 /* registry loads seen                  */
static void* s_engRet[4];
static int   s_engRetDepth;
/* what memory held before a reload, put back after it */
static unsigned short s_snapOpt;
static int  s_snapGamma, s_snapW, s_snapH;

static char* eng_main(void) { return *(char**)TA_MAIN; }
static int eng_owned(void) { return s_engArmed; }

static int eng_in_game(const char* m) { return (*(const unsigned char*)(m + OFF_INGAME) & 4) != 0; }

static int rd32(const char* p) { int v; memcpy(&v, p, 4); return v; }
static void wr32(char* p, int v) { memcpy(p, &v, 4); }

static int eng_injected(int stored)
{
    char* p = g_config.inject_resolution;
    unsigned long w = strtoul(p, &p, 0), h = (*p ? strtoul(p + 1, &p, 0) : 0);
    return w && h && (int)w == TS_RES_W(stored) && (int)h == TS_RES_H(stored);
}

static int eng_resolve(int stored, int* w, int* h)
{
    RECT r;
    int nw, nh;
    if (!util_target_monitor(&r)) return 0;
    nw = r.right - r.left;
    nh = r.bottom - r.top;
    if (nw < 640 || nh < 480) return 0;
    /* a stored size the selected monitor cannot show falls back to its own --
       except the injected entry, which the picker offers whatever the monitor
       (dd.c) */
    if (stored && ((TS_RES_W(stored) <= nw && TS_RES_H(stored) <= nh) || eng_injected(stored))) {
        *w = TS_RES_W(stored);
        *h = TS_RES_H(stored);
    } else {
        *w = nw;
        *h = nh;
    }
    return 1;
}

/* The Shadows row's value IN FORCE: the store's, or the lever's where
   `tagpu_classicpp.cfg` names `shadows=` -- the value the row shows. */
static int eng_shadows_on(void)
{
    const TAGPU_LIGHT* L = tagpu_classicpp_light();
    int v;
    if (tagpu_classicpp_held() & TAGPU_HELD_SHADOWS) return L && L->shadows != TAGPU_SHADOWS_OFF;
    return tagpu_settings_get(TS_SHADOWS, &v) ? v != 0 : 1;
}

/* Set `pin` in the option word, and the three shadow bits together as the
   stock BSHADOWS click does (0x45E1CB). The engine re-bakes after a stock
   toggle in game (0x45E26B); a unit baked under the old bits would otherwise
   keep them. */
static void eng_write_word(char* m, unsigned short pin, int shadows)
{
    unsigned short was = *(unsigned short*)(m + OFF_OPTWORD), now = was | pin;
    now = shadows ? (unsigned short)(now | OPT_SHADOWS) : (unsigned short)(now & ~OPT_SHADOWS);
    *(unsigned short*)(m + OFF_OPTWORD) = now;
    if (now != was && eng_in_game(m)) {
        void* heap = *(void**)(m + OFF_BAKEHEAP);
        if (heap) ((rebake_fn)VA_REBAKE)(heap);
    }
}

/* The Shadows row where the store does not own the engine's options -- under
   tagpu_defaults.off, or with the loader observer not in: the stock toggle's
   write and nothing else, and the engine's own saver keeps it, as stock. */
static int eng_shadows_bit(void)
{
    char* m = eng_main();
    return m ? (*(unsigned short*)(m + OFF_OPTWORD) & 4) != 0 : 1;
}

static void eng_set_shadows(int on)
{
    char* m = eng_main();
    if (m) eng_write_word(m, 0, on);
}

static void eng_push(int what)
{
    char* m = eng_main();
    int v;
    if (!s_engArmed || !m || tagpu_settings_ignored()) return;

    if ((what & ENG_GAMMA) && tagpu_settings_get(TS_GAMMA, &v)) {
        wr32(m + OFF_GAMMA, v);
        ((setgamma_fn)VA_SETGAMMA)(0.5f + (float)v / 24.0f);
    }
    if (what & ENG_BITS)
        eng_write_word(m, (unsigned short)(OPT_AA | OPT_SHADING), eng_shadows_on());
    if ((what & ENG_RES) && !eng_in_game(m) && tagpu_settings_get(TS_RESOLUTION, &v)) {
        int w, h;
        if (eng_resolve(v, &w, &h)) {
            wr32(m + OFF_SELW, w);
            wr32(m + OFF_SELH, h);
        }
    }
}

/* MixingBuffers is not a Visuals value: the registry load reads it (0x42FE4F)
   and stores it through the sound object's setter 0x4CF210 with no check, so
   the store's value goes in after EVERY load, the first and the reloads alike.
   The pointer is the one that setter has just written through on this thread;
   the engine reads the field only on this thread too (the play 0x4CF570). The
   engine saves it back to the registry with its other options (0x4310AB). */
static int eng_push_mixing(char* m)
{
    char* snd;
    int v;
    if (!tagpu_settings_get(TS_MIXING, &v)) return 0;
    snd = *(char**)(m + OFF_SOUND);
    if (!snd) return 0;
    wr32(snd + SND_MIXING, v);
    return v;
}

static int __cdecl opt_load_before(void* esp)
{
    char* m = eng_main();
    if (s_engRetDepth >= 4) return 0;
    if (s_engLoads && m) {
        s_snapOpt   = *(unsigned short*)(m + OFF_OPTWORD);
        s_snapGamma = rd32(m + OFF_GAMMA);
        s_snapW     = rd32(m + OFF_SELW);
        s_snapH     = rd32(m + OFF_SELH);
    }
    s_engRet[s_engRetDepth++] = ((void**)esp)[0];
    return 1;
}

static void* __cdecl opt_load_after(unsigned int* regs)
{
    char* m = eng_main();
    (void)regs;
    if (m && s_engArmed) {
        if (!s_engLoads) {
            /* no SetGamma here: UIPipelinesInit applies +0x37F08 next */
            int v;
            char b[160];
            if (tagpu_settings_get(TS_GAMMA, &v)) wr32(m + OFF_GAMMA, v);
            eng_push(ENG_BITS | ENG_RES);
            v = eng_push_mixing(m);
            _snprintf(b, sizeof b, "menu: engine options from the store: gamma=%d optword=0x%04X screen=%dx%d "
                      "mixingbuffers=%d",
                      rd32(m + OFF_GAMMA), *(unsigned short*)(m + OFF_OPTWORD),
                      rd32(m + OFF_SELW), rd32(m + OFF_SELH), v);
            b[sizeof b - 1] = 0;
            mlog(b);
        } else {
            /* THE WHOLE WORD, not only the owned bits: memory is the value in
               force and the registry at best its last save. Not every writer
               saves -- the bit-7 hotkey at 0x4963B8 does not -- so a reload
               that took the rest of the word from the registry would undo an
               unsaved toggle, as stock's does. */
            char b[200];
            _snprintf(b, sizeof b, "menu: registry reload %d: the engine read gamma=%d optword=0x%04X "
                      "screen=%dx%d; memory's kept (gamma=%d optword=0x%04X screen=%dx%d)",
                      s_engLoads, rd32(m + OFF_GAMMA), *(unsigned short*)(m + OFF_OPTWORD),
                      rd32(m + OFF_SELW), rd32(m + OFF_SELH),
                      s_snapGamma, s_snapOpt, s_snapW, s_snapH);
            b[sizeof b - 1] = 0;
            mlog(b);
            *(unsigned short*)(m + OFF_OPTWORD) = s_snapOpt;
            wr32(m + OFF_GAMMA, s_snapGamma);
            wr32(m + OFF_SELW, s_snapW);
            wr32(m + OFF_SELH, s_snapH);
            eng_push_mixing(m);
        }
        s_engLoads++;
    }
    return s_engRetDepth > 0 ? s_engRet[--s_engRetDepth] : NULL;
}

static void* s_stashRet[4];
static int   s_stashDepth;

static int __cdecl stashback_before(void* esp)
{
    if (s_stashDepth >= 4) return 0;
    s_stashRet[s_stashDepth++] = ((void**)esp)[0];
    return 1;
}

static void* __cdecl stashback_after(unsigned int* regs)
{
    (void)regs;
    eng_push(ENG_ALL);
    return s_stashDepth > 0 ? s_stashRet[--s_stashDepth] : NULL;
}

/* ---- the two sliders into the store --------------------------------------
   A slider has no OnCommand branch: a move fires the gadget's own callback at
   +0x144 [DISASSEMBLED 0x4A42F5], which writes the field and applies it. The
   builder stores the callback per visit (GAMMA's at 0x45EA29), so it is wrapped
   per visit, and only when it is still the engine's own. */
static void __stdcall gamma_moved(void* gi, int arg)
{
    char* m;
    ((slider_cb_fn)VA_GAMMA_CB)(gi, arg);
    m = eng_main();
    if (m) tagpu_settings_set(TS_GAMMA, rd32(m + OFF_GAMMA));   /* bounded by the set */
}

static void __stdcall vidsldr_moved(void* gi, int arg)
{
    char* m;
    int w, h, nw, nh;
    ((slider_cb_fn)VA_VIDSLDR_CB)(gi, arg);
    m = eng_main();
    if (!m) return;
    w = rd32(m + OFF_SELW);
    h = rd32(m + OFF_SELH);
    /* the monitor's own mode is stored as `native`, so it follows the monitor */
    if (eng_resolve(0, &nw, &nh) && w == nw && h == nh)
        tagpu_settings_set(TS_RESOLUTION, 0);
    else if (w > 0 && h > 0 && w < 32768 && h < 32768)
        tagpu_settings_set(TS_RESOLUTION, TS_RES(w, h));
}

static void eng_wrap_sliders(void)
{
    char* m = eng_main();
    char* top = m ? *(char**)(m + OFF_TOPGUI) : 0;
    char* ctrls = top ? *(char**)(top + GM_CTRLS) : 0;
    int n, i;
    if (!s_engArmed || !ctrls) return;
    n = *(short*)(ctrls + 0xB6);
    for (i = 1; i <= n && i < 256; i++) {
        char* g = ctrls + (size_t)i * STRIDE;
        void** cb = (void**)(g + G_SLIDER_CB);
        if (!memcmp(g + G_NAME, "GAMMA", 6) && *cb == (void*)VA_GAMMA_CB)
            *cb = (void*)gamma_moved;
        else if (!memcmp(g + G_NAME, "VIDSLDR", 8) && *cb == (void*)VA_VIDSLDR_CB)
            *cb = (void*)vidsldr_moved;
    }
}

static void engopt_install(void)
{
    if (tagpu_settings_ignored()) {
        mlog("menu: engine options NOT taken over (tagpu_defaults.off: the registry's, as stock)");
        return;
    }
    int stash;
    /* armed FIRST: the observer's after-hook tests it, and the first load is
       the one that must see it */
    s_engArmed = 1;
    if (!tagpu_detour_bytes_ok(VA_OPT_LOAD, OPT_LOAD_STOLEN, sizeof OPT_LOAD_STOLEN) ||
        !tagpu_detour_observe(VA_OPT_LOAD, OPT_LOAD_STOLEN, sizeof OPT_LOAD_STOLEN,
                              opt_load_before, opt_load_after)) {
        s_engArmed = 0;
        mlog("menu: engine options NOT taken over - the registry load 0x42F9A0 is not as expected");
        return;
    }
    stash = tagpu_detour_bytes_ok(VA_OPT_STASHBACK, STASHBACK_STOLEN, sizeof STASHBACK_STOLEN) &&
            tagpu_detour_observe(VA_OPT_STASHBACK, STASHBACK_STOLEN, sizeof STASHBACK_STOLEN,
                                 stashback_before, stashback_after);
    mlog(stash ? "menu: engine options owned by the store (the registry load 0x42F9A0, the stash restore 0x45CAE0)"
               : "menu: engine options owned by the store, but NOT after CANCEL - the stash restore 0x45CAE0 is not as expected");
}

/* ---- the two observers --------------------------------------------------- */

static int s_visArmed = 0;
static void* s_visRet[8];
static int   s_visRetDepth = 0;

/* Is the screen on top one of ours? Asked by NAME, off the live ControlsAry,
   because `0x45E5E0` also builds VISUALRT and SELVMODE and neither carries our
   rows. The count is at +0xB6 and records are 1-based, exactly as the
   OnCommand index is. */
static int screen_has_row(void)
{
    char* main_p = *(char**)TA_MAIN;
    char* top    = main_p ? *(char**)(main_p + OFF_TOPGUI) : 0;
    char* ctrls  = top ? *(char**)(top + GM_CTRLS) : 0;
    int n, i;
    size_t len;
    if (!ctrls) return 0;
    n = *(short*)(ctrls + 0xB6);
    len = strlen(s_row[0].name) + 1;
    for (i = 1; i <= n && i < 256; i++)
        if (!memcmp(ctrls + (size_t)i * STRIDE + G_NAME, s_row[0].name, len))
            return 1;
    return 0;
}

typedef void (__stdcall *oncmd_fn)(void* gi);
static oncmd_fn s_visPrevOnCmd = 0;

static void __stdcall tagpu_vis_oncommand(void* gi);

/* Is the screen on top our VISUALRT? By its ground's name, which no stock
   screen carries. */
static int screen_is_vrt(void)
{
    char* main_p = *(char**)TA_MAIN;
    char* top    = main_p ? *(char**)(main_p + OFF_TOPGUI) : 0;
    char* ctrls  = top ? *(char**)(top + GM_CTRLS) : 0;
    int n, i;
    if (!ctrls) return 0;
    n = *(short*)(ctrls + 0xB6);
    for (i = 1; i <= n && i < 256; i++)
        if (!memcmp(ctrls + (size_t)i * STRIDE + G_NAME, ART_VRTBG, sizeof ART_VRTBG))
            return 1;
    return 0;
}

/* THE IN-GAME SCREEN'S OnCommand, chained the way the front end's is. Its
   RESTORE and UNDO are Gamma's alone -- the sprocket panel has its own -- and
   they are not forwarded while the store owns Gamma, for the reason
   tagpu_vis_oncommand gives. The
   rebuild is the engine's own idiom again: `0x45E5E0` builds VISUALRT while
   `main+0x37EBE & 1` stands, which only the screen's -1 branch clears. */
static oncmd_fn s_vrtPrevOnCmd = 0;
static int      s_vrtKeep;
static int      s_vrtGammaOpen = 12;

static void vrt_rebuild(void* gi)
{
    s_vrtKeep = 1;
    ((gui_pop_fn)VA_GUI_POP)(gi);
    ((vis_build_fn)VA_VIS_BUILD)(0);
}

static void __stdcall tagpu_vrt_oncommand(void* gi)
{
    const char* n = vis_actuated(gi);
    if (eng_owned() && n && (!memcmp(n, "RESTORE", 8) || !memcmp(n, "UNDO", 5))) {
        ((opt_sound_fn)VA_OPT_SOUND)("Options", 0);
        tagpu_settings_set(TS_GAMMA, n[0] == 'R' ? 12 : s_vrtGammaOpen);
        eng_push(ENG_GAMMA);
        vrt_rebuild(gi);
        return;
    }
    if (s_vrtPrevOnCmd) s_vrtPrevOnCmd(gi);
}

static int __cdecl vis_build_before(void* esp)
{
    if (s_visRetDepth >= 8) return 0;          /* recursion guard, not a queue */
    s_visRet[s_visRetDepth++] = ((void**)esp)[0];
    return 1;                                   /* ask for the return trampoline */
}

/* On RETURN from the dialog build: the gadgets exist, so the plates can be set.
   `read_state` reads the values in force, so the plates show what is drawn. */
static void* __cdecl vis_build_after(unsigned int* regs)
{
    (void)regs;
    eng_wrap_sliders();
    if (screen_is_vrt()) {
        char* main_p = *(char**)TA_MAIN;
        char* top    = *(char**)(main_p + OFF_TOPGUI);
        oncmd_fn cur = top ? *(oncmd_fn*)(top + GM_ONCMD) : 0;
        if (cur && cur != tagpu_vrt_oncommand) {
            s_vrtPrevOnCmd = cur;
            *(oncmd_fn*)(top + GM_ONCMD) = tagpu_vrt_oncommand;
        }
        if (s_vrtKeep) s_vrtKeep = 0;
        else if (!tagpu_settings_get(TS_GAMMA, &s_vrtGammaOpen)) s_vrtGammaOpen = 12;
    }
    if (screen_has_row()) {
        char* main_p = *(char**)TA_MAIN;
        char* top    = *(char**)(main_p + OFF_TOPGUI);
        oncmd_fn cur = top ? *(oncmd_fn*)(top + GM_ONCMD) : 0;
        /* Take the dispatch slot, remembering whose it was so the engine's own
           handler still runs for its own gadgets. The screen is rebuilt every
           time the player enters it, so this is re-taken per visit and the
           saved pointer is never a stale one from a previous screen -- the
           guard is that we never chain to OURSELVES. */
        if (cur && cur != tagpu_vis_oncommand) {
            s_visPrevOnCmd = cur;
            *(oncmd_fn*)(top + GM_ONCMD) = tagpu_vis_oncommand;
        }
        if (s_visKeep) {
            s_visKeep = 0;              /* the model is already what we want */
        } else {
            read_state();
            read_display_state();
            memcpy(s_stageOpen,  s_stage,  sizeof s_stageOpen);
            memcpy(s_vstageOpen, s_vstage, sizeof s_vstageOpen);
            InterlockedExchange(&s_scaleOpen, tagpu_hud_stored_pct());
            memset(s_vtouched, 0, sizeof s_vtouched);
            if (!tagpu_settings_get(TS_MONITOR, &s_monOpen)) s_monOpen = -1;
            if (!tagpu_settings_get(TS_GAMMA, &s_gammaOpen)) s_gammaOpen = 12;
            if (!tagpu_settings_get(TS_RESOLUTION, &s_resOpen)) s_resOpen = 0;
        }
        push_stages(main_p + OFF_GUIINFO);
        push_display(main_p + OFF_GUIINFO);
    }
    return s_visRetDepth > 0 ? s_visRet[--s_visRetDepth] : NULL;
}

/* OUR OnCommand for the front-end screen, installed in the engine's own
   dispatch slot rather than over its handler.

   WHY THE HANDLER CANNOT SIMPLY BE OBSERVED, measured 2026-09-11. An observer
   never skips, so the engine's `0x45E100` would run after ours -- and its
   fall-through
   at `0x45E46A` is not the no-op it looks like:

       mov  eax,[esi+0x60]        ; the actuated index
       cmp  eax,-1                ; nothing actuated -> return
       je   0x45E4AB
       ...                        ; ebx + index*0x15B
       cmpb $1,(%ebx,%edx,2)      ; is the gadget id == 1, i.e. a BUTTON?
       je   0x45E499              ; YES -> GUI_Pop 0x4A9660, then call the
                                  ;        UNDERLYING screen's OnCommand

   That is how the tab buttons work. SOUND / MUSIC / INTERFACE / VISUALS and
   OK / Cancel / Restore / Undo came from STARTOPT.GUI and sit in the SAME
   gadget array as the video controls (the `0x200` merge above), so the handler
   cannot tell them apart by position: a click it does not recognise by name
   means "one of STARTOPT's own", and the answer is to pop this merged screen
   and hand the click to the plain STARTOPT underneath, which rebuilds around
   the tab that was hit. Our rows are `id=1` stage buttons in that same array
   and are therefore indistinguishable from a tab.

   So we take `GUIMEMSTRUCT+0x08`, the engine's own extension point and the
   same field RENDER.GUI owns outright, and chain: ours when the actuated
   gadget is one of our rows, the engine's for everything else, which keeps the
   tabs, the buttons and the stock controls behaving exactly as they did.

   THE -1 CALL IS FORWARDED, and must be. It arrives from exactly two places --
   `GUI_Pop 0x4A9673` and the pump's inlined pop at `0x4AA7AC` -- and BOTH free
   this GUIMEMSTRUCT within a dozen instructions of the call returning. So the
   -1 branch of `0x45E100` is not a teardown running mid-screen; it is the
   screen's own destructor arriving on time, and it is the only thing that frees
   the display-mode list hanging off `GUIMEMSTRUCT+0x0C` (`0x45E11B`: the entry
   table, the list, the block) and clears `main+0x37EBE` bit 0. Declining it
   leaks those three allocations on every visit to this tab, and leaves the bit
   alone -- which is the bit `0x45E5E0` reads to decide between VISUALS.GUI and
   VISUALRT.GUI. The -1 is not a second call for the same click: see
   `menu_accept` for what takes the screen down. */
static void __stdcall tagpu_vis_oncommand(void* gi)
{
    int d;
    if (menu_row_of(gi) >= 0) { tagpu_menu_oncommand(gi); return; }
    d = vis_row_of(gi);
    if (d >= 0) {
        int n = s_vrow[d].stages > 0 ? s_vrow[d].stages : 1;
        int guard = n;
        do { s_vstage[d] = (s_vstage[d] + 1) % n; }
        while (d == VD_SCALE && !scale_stage_ok(s_vstage[d]) && --guard > 0);
        if (d == VD_MON) s_monChosen = 1;
        commit_display(d);
        s_vtouched[d] = 1;
        apply_display(d);       /* posts; the wndproc does the window work */
        /* The Monitor row changes what the Screen Size list may contain, so it
           rebuilds the screen instead of just re-plating it. Nothing after the
           call may touch `gi`'s screen: the old GUIMEMSTRUCT is freed inside. */
        if (d == VD_MON) {
            eng_push(ENG_RES);      /* `native` is the NEW monitor's mode */
            vis_relist(gi);
            return;
        }
        push_display(gi);
        menu_accept(gi);
        return;
    }
    {
        /* UNDO AND RESTORE ARE OURS AND NOT FORWARDED while the store owns
           the engine's options: every value the engine's two branches reset
           (0x45E2FD, 0x45E331) is the store's, so forwarding would put the
           engine's copies -- the stash, 640x480 -- back over it. The screen is
           rebuilt the way both branches end. Where the store does not own
           them, the engine's branch is what resets them, and its rebuild
           keeps our model (`s_visKeep`). */
        const char* n = vis_actuated(gi);
        int undo = n && !memcmp(n, "UNDO", 5), restore = n && !memcmp(n, "RESTORE", 8);
        if (undo || restore) {
            if (undo) vis_undo(); else vis_restore();
            if (eng_owned()) {
                ((opt_sound_fn)VA_OPT_SOUND)("Options", 0);
                vis_relist(gi);
                return;
            }
        }
    }
    if (s_visPrevOnCmd) s_visPrevOnCmd(gi);
}

static void vis_install(void)
{
    s_visArmed =
        tagpu_detour_bytes_ok(VA_VIS_BUILD, VIS_BUILD_STOLEN, sizeof VIS_BUILD_STOLEN) &&
        tagpu_detour_observe(VA_VIS_BUILD, VIS_BUILD_STOLEN, sizeof VIS_BUILD_STOLEN,
                             vis_build_before, vis_build_after);
}

void tagpu_menu_init(void)
{
    static char gui[8192];
    static char vgui[49152];
    /* one GAF per screen, because the engine picks the file from the panel
       gadget's own name (`0x4A8537`): anims\\RENDER.GAF and anims\\VISUALS.GAF */
    static unsigned char gaf[64 + 0x28 + 8 + 0x18 + PANEL_W * PANEL_H];
    static unsigned char vgaf[64 + 0x28 + 8 + 0x18 + VP_W * VP_H];
    static char rtgui[4096];
    static unsigned char rtgaf[64 + 0x28 + 8 + 0x18 + VRT_W * VRT_H];
    static const GafEnt RTENTS[1] = { { ART_VRTBG, VRT_W, VRT_H, draw_vrtbg } };
    static const GafEnt ENTS[1]  = { { ART_NAME,  PANEL_W, PANEL_H, draw_panel_frame } };
    static const GafEnt VENTS[1] = { { ART_VISBG, VP_W,    VP_H,    draw_visbg       } };
    TAGPU_UFO_FILE f[6];
    char b[300];
    int len, vlen, rtlen, wrote, armed, visOk;
    unsigned glen, vglen, rtglen;

    read_tokens();
    enum_monitors();        /* the Monitor row's captions go into the file */
    /* The GPU row's captions likewise, out of the cache the LAST launch's
       enumeration worker wrote -- this runs under the loader lock and a Vulkan
       instance may not be created here (tagpu_vk.h). */
    tagpu_vk_names_init();
    build_gpu_text();

    /* The archive is written UNCONDITIONALLY every launch, arm file or not:
       staleness after a DLL upgrade is the one failure here that would be
       genuinely confusing, and it costs a few ms. */
    len = build_gui(gui, sizeof gui, s_nrows);
    if (len < 0) { mlog("menu: NOT armed - the generated .GUI does not fit"); return; }
    glen = build_gaf(gaf, sizeof gaf, ENTS, 1, s_nrows);
    if (!glen) { mlog("menu: NOT armed - the panel frame does not fit"); return; }
    vglen = build_gaf(vgaf, sizeof vgaf, VENTS, 1, s_nrows);
    if (!vglen) { mlog("menu: NOT armed - the front-end ground does not fit"); return; }
    /* The front-end screen is a THIRD entry in the same archive. It is written
       whether or not the observers arm: a half-written archive after a DLL
       upgrade is the confusing failure, exactly as for render.gui above. */
    vlen = build_visuals_gui(vgui, sizeof vgui, s_nrows);
    if (vlen < 0) { mlog("menu: NOT armed - the generated VISUALS.GUI does not fit"); return; }
    rtlen = build_visualrt_gui(rtgui, sizeof rtgui);
    if (rtlen < 0) { mlog("menu: NOT armed - the generated VISUALRT.GUI does not fit"); return; }
    rtglen = build_gaf(rtgaf, sizeof rtgaf, RTENTS, 1, s_nrows);
    if (!rtglen) { mlog("menu: NOT armed - the in-game Visuals ground does not fit"); return; }
    f[0].path = "guis/render.gui";
    f[0].data = gui;
    f[0].size = (unsigned)len;
    f[1].path = "anims/render.gaf";
    f[1].data = gaf;
    f[1].size = glen;
    f[2].path = VIS_FILE;
    f[2].data = vgui;
    f[2].size = (unsigned)vlen;
    f[3].path = VIS_GAF;
    f[3].data = vgaf;
    f[3].size = vglen;
    f[4].path = VRT_FILE;
    f[4].data = rtgui;
    f[4].size = (unsigned)rtlen;
    f[5].path = VRT_GAF;
    f[5].data = rtgaf;
    f[5].size = rtglen;
    /* THE TWO VISUALS SCREENS SHIP ONLY WITH THEIR HANDLERS. They drop the
       stock toggles and route Restore/Undo to us, so without the dialog-build
       observer (tagpu_menu.off, or an exe whose bytes differ) the stock files
       must stay the ones the engine finds. Checked before the write, and the
       install below uses the same test. */
    visOk = !exists(OFF_FILE) &&
            tagpu_detour_bytes_ok(VA_DRAWSCREEN, DRAW_STOLEN, sizeof DRAW_STOLEN) &&
            tagpu_detour_bytes_ok(VA_VIS_BUILD, VIS_BUILD_STOLEN, sizeof VIS_BUILD_STOLEN);
    wrote = tagpu_ufo_write(UFO_FILE, f, visOk ? 6 : 2);

    armed = wrote && !exists(OFF_FILE) &&
            tagpu_detour_bytes_ok(VA_DRAWSCREEN, DRAW_STOLEN, sizeof DRAW_STOLEN) &&
            tagpu_detour_observe(VA_DRAWSCREEN, DRAW_STOLEN, sizeof DRAW_STOLEN,
                                 before_update, NULL);
    s_installed = armed;

    if (armed) {
        build_trigger();
        s_drawTrigger =
            tagpu_detour_bytes_ok(VA_POSTGUI, POST_STOLEN, sizeof POST_STOLEN) &&
            tagpu_detour_observe(VA_POSTGUI, POST_STOLEN, sizeof POST_STOLEN,
                                 before_postgui, NULL);
        vis_install();
        /* the store owns the engine's options only where the screens that
           write it are in; the first registry load is still ahead */
        if (s_visArmed) engopt_install();
    }

    _snprintf(b, sizeof b,
              "menu: %s " UFO_STAMP " ufo=%d rows=%d gui=%d gaf=%u vis=%d vgaf=%u trigger=%d bytes "
              "(RENDER.GUI over DrawGameScreen 0x468CF0, open with " OPEN_FILE "; "
              "VISUALS.GUI over the dialog build 0x45E5E0, OnCommand chained at GUIMEMSTRUCT+8, front end=%d)",
              armed ? "ARMED" : "NOT armed", wrote, s_nrows, len, glen, vlen, vglen,
              s_drawTrigger, s_visArmed);
    b[sizeof b - 1] = 0;
    mlog(b);
}
