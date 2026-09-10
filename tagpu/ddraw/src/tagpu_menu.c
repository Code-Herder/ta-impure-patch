/* tagpu_menu.c -- the render-options screen (Phase F, G18). API: tagpu_menu.h.

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
   - `gi+0xCCA` -- roadmap G18 gate (2)'s named unknown -- is the screen's
     DEFERRED-REPAINT flag. Twenty-odd state-changing GUI calls set it (there
     are bare accessors at 0x49FA90 set / 0x49FAB0 clear) and there is exactly
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
   0x468CF0 -- UpdateIngameGUI 0x491D70 was tried first and starved, because all
   21 of its call sites are transition handlers rather than the frame loop. So
   it is on the game thread and runs immediately before the engine reconsiders
   the GUI stack -- the one moment at which pushing a screen cannot race the
   pop loop. `OnCommand` is the engine calling us, also on the game thread, and
   it does NOT write the file: it sets an in-memory value and the cfg is
   written from the render thread in tagpu_menu_present(). */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dd.h"
#include "tagpu_detour.h"
#include "tagpu_classicpp.h"
#include "tagpu_ufo.h"
#include "tagpu_gaf.h"
#include "tagpu_menu.h"

/* ---- the engine ---------------------------------------------------------- */
#define TA_MAIN         0x00511DE8u     /* TAdynmemStruct**                    */
#define OFF_GUIINFO         0x519u      /* main + this = GUIInfo               */
#define OFF_TOPGUI          0x531u      /* main + this = gi->TheActive_GUIMEM  */
#define OFF_EXPECT        0x37EA0u      /* the screen the engine keeps on top  */
#define VA_GUI_LOAD     0x004AA8F0u
#define VA_SETSTATUS    0x004A1080u
#define VA_SETGRAYED    0x004A1250u     /* (gi, name, grayed): see push_stages */
#define VA_STAGEDRAW    0x004A81E0u
#define VA_SETDIRTY     0x0049FA90u     /* gi+0xCCA = 1: repaint at the pump   */
#define VA_DRAWLOCK     0x004C2470u     /* the counted pair GUI_Load draws under */
#define VA_DRAWUNLOCK   0x004C2870u
#define VA_UPDGUI       0x00491D70u
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
typedef int   (__stdcall *upd_gui_fn)(int);
typedef void  (__cdecl   *lock_fn)(void);
typedef void  (__stdcall *gaf_blit_fn)(void* ctx, const void* frame, int x, int y);
typedef int   (__stdcall *file_open_fn)(const char* path);
typedef void* (__stdcall *gaf_load_fn)(const char* path);
typedef void* (__stdcall *gaf_find_fn)(void* bank, const char* name);

/* DrawGameScreen's prologue: `sub esp,0x214`.

   THE TICK IS HERE AND NOT ON UpdateIngameGUI, which is where it started.
   UpdateIngameGUI has 21 call sites and NONE of them is the frame loop -- they
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
   it lands in the back buffer every flip presents and every G15/G17 twin
   already watches -- rather than in a GL layer the engine's surface, `tacli
   shot` and the twins would all miss. */
static const unsigned char POST_STOLEN[6] = { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 };

/* ---- the geometry, from tools/guipanel.py (the source of truth) ---------- */
#define PANEL_W   304
#define PANEL_H   212
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
/* Six, and every one of them live -- mouse-wheel zoom was the seventh and was
   cut because tagpu_zoom_init() installs byte patches once at attach, so the
   row would have lit green and changed no pixel until the next launch. The
   menu keeps the invariant that NO ROW NEEDS A RESTART (renderers.md 2.10). */
enum { R_STYLE, R_ASSETS, R_LIGHT, R_SHADOWS, R_SHADOWQ, R_SS, R_COUNT };

typedef struct {
    const char* name;                   /* the gadget name in the .GUI         */
    const char* label;                  /* its id=5 label, on the SAME line     */
    const char* text;                   /* pipe-separated stage labels          */
    int         stages;
} Row;

static const Row s_row[R_COUNT] = {
    { "STYLE",   "Renderer",          "Classic|Classic++|Custom", 3 },
    { "ASSETS",  "Undithered assets", "Off|On",                   2 },
    { "LIGHT",   "Dynamic lighting",  "Off|On",                   2 },
    { "SHADOWS", "Shadows",           "Off|Hard|Soft",            3 },
    { "SHADOWQ", "Shadow quality",    "Low|Med|High|Ultra",       4 },
    { "SS",      "Supersampling",     "Off|2x",                   2 },
};

/* Renderer stages. Custom is DERIVED, never clicked into: clicking the row
   alternates Classic and Classic++, and touching any row below makes it read
   Custom (renderers.md 2.10). */
enum { STYLE_CLASSIC, STYLE_PP, STYLE_CUSTOM };

/* Shadows: the UI order is Off|Hard|Soft and the cfg's is 0 none / 1 SOFT /
   2 hard, so the two are not the same number and this table is the mapping. */
static const int SHADOW_VAL[3] = { TAGPU_SHADOWS_OFF, TAGPU_SHADOWS_HARD, TAGPU_SHADOWS_SOFT };
/* Shadow quality -> shadowres=, whose own range is 256..4096 (tagpu_classicpp.c). */
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
#define CFG_FILE   "tagpu_classicpp.cfg"
#define CFG_TMP    "tagpu_classicpp.cfg.tmp"
#define SS_OFF     "tagpu_ss.off"
#define CPP_ON     "tagpu_classicpp.on"
#define CPP_OFF    "tagpu_classicpp.off"
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
#define UFO_STAMP  "G18-7"

static int    s_installed;
static int    s_nrows = R_COUNT;
static void*  s_gm;                     /* our GUIMEMSTRUCT while open, else 0 */
static char   s_saved[16];              /* what main+0x37EA0 held              */
static int    s_stage[R_COUNT];
static volatile LONG s_dirty;           /* a row moved: the cfg needs writing  */
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
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", m); fclose(f); }
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
#define DIV_BOT   202
#define PAD         2

static void px(unsigned char* f, int x, int y, unsigned char v)
{
    if (x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H) f[y * PANEL_W + x] = v;
}

static void hline(unsigned char* f, int x0, int x1, int y, unsigned char v)
{
    for (; x0 <= x1; x0++) px(f, x0, y, v);
}

static void vline(unsigned char* f, int x, int y0, int y1, unsigned char v)
{
    for (; y0 <= y1; y0++) px(f, x, y0, v);
}

static void fillrect(unsigned char* f, int x, int y, int w, int h, unsigned char v)
{
    int i, j;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) px(f, x + i, y + j, v);
}

/* A sunken band: dark lip above and left, lit lip below and right -- guipanel's
   recess(), which is what every stock runtime panel paints and what makes a
   stagebuttn plate sit IN the panel rather than on it. */
static void recess(unsigned char* f, int x, int y, int w, int h)
{
    int j;
    for (j = 1; j < h - 1; j++)
        fillrect(f, x + 1, y + j, w - 2, 1,
                 (unsigned char)(j * 2 < h ? IX_RECESS_HI : IX_RECESS_LO));
    hline(f, x, x + w - 1, y, IX_EDGE);
    vline(f, x, y, y + h - 1, IX_EDGE);
    hline(f, x, x + w - 1, y + h - 1, IX_RECESS_LIGHT);
    vline(f, x + w - 1, y, y + h - 1, IX_RECESS_LIGHT);
}

static void bolt(unsigned char* f, int cx, int cy)
{
    int i, j;
    for (j = -3; j <= 3; j++)
        for (i = -3; i <= 3; i++) {
            int r2 = i * i + j * j;
            if (r2 <= 9)  px(f, cx + i, cy + j, IX_BOLT_LO);
            if (r2 <= 2)  px(f, cx + i, cy + j, IX_BOLT_HI);
        }
}

static void divider(unsigned char* f, int y)
{
    hline(f, 10, PANEL_W - 11, y,     IX_EDGE);
    hline(f, 10, PANEL_W - 11, y + 1, IX_BEVEL_HI);
}

/* tools/guipanel.py draw_panel(), in indices: the face's vertical ramp, the
   black keyline and its top-left-lit bevel, four bolts, the two rules, and one
   recess per row. */
static void draw_panel(unsigned char* f, int rows)
{
    int y, i;

    for (y = 0; y < PANEL_H; y++) {
        int t = y * 3 / PANEL_H;            /* three steps is all the ramp has */
        fillrect(f, 0, y, PANEL_W, 1,
                 (unsigned char)(t == 0 ? IX_GROUND_HI :
                                 t == 1 ? IX_GROUND_MID : IX_GROUND_LO));
    }

    hline(f, 0, PANEL_W - 1, 0, IX_EDGE);
    hline(f, 0, PANEL_W - 1, PANEL_H - 1, IX_EDGE);
    vline(f, 0, 0, PANEL_H - 1, IX_EDGE);
    vline(f, PANEL_W - 1, 0, PANEL_H - 1, IX_EDGE);
    hline(f, 1, PANEL_W - 2, 1, IX_EDGE);
    hline(f, 1, PANEL_W - 2, PANEL_H - 2, IX_EDGE);
    vline(f, 1, 1, PANEL_H - 2, IX_EDGE);
    vline(f, PANEL_W - 2, 1, PANEL_H - 2, IX_EDGE);
    hline(f, 2, PANEL_W - 3, 2, IX_BEVEL_HI);
    vline(f, 2, 2, PANEL_H - 3, IX_BEVEL_HI);
    hline(f, 3, PANEL_W - 3, PANEL_H - 3, IX_BEVEL_LO);
    vline(f, PANEL_W - 3, 3, PANEL_H - 3, IX_BEVEL_LO);

    bolt(f, 9, 9);
    bolt(f, PANEL_W - 10, 9);
    bolt(f, 9, PANEL_H - 10);
    bolt(f, PANEL_W - 10, PANEL_H - 10);

    divider(f, DIV_TOP);
    divider(f, DIV_BOT);

    for (i = 0; i < rows; i++) {
        y = ROW_Y0 + ROW_PITCH * i;
        recess(f, CTL_X - PAD, y - PAD, CTL_W + 2 * PAD, ROW_H + 2 * PAD);
    }
}

/* One entry, one uncompressed frame (file-formats.md 3). Uncompressed is not
   laziness: the DLL repaints this plane in place at screen-load time, and a
   flat w*h copy is what makes that a memcpy rather than a re-encode. */
static unsigned build_gaf(unsigned char* out, unsigned cap, int rows)
{
    const unsigned ENTOFF = 12 + 4;                  /* header + one offset    */
    const unsigned TABOFF = ENTOFF + 0x28;           /* the frame table        */
    const unsigned FRMOFF = TABOFF + 8;              /* the frame header       */
    const unsigned PIXOFF = FRMOFF + 0x18;
    unsigned need = PIXOFF + (unsigned)PANEL_W * PANEL_H;
    unsigned v;

    if (cap < need) return 0;
    memset(out, 0, need);

    v = 0x00010100u; memcpy(out + 0, &v, 4);         /* signature              */
    v = 1u;          memcpy(out + 4, &v, 4);         /* one entry              */
    v = ENTOFF;      memcpy(out + 12, &v, 4);

    *(unsigned short*)(out + ENTOFF + 0) = 1;        /* one frame              */
    v = 1u;          memcpy(out + ENTOFF + 2, &v, 4);/* entry signature        */
    lstrcpynA((char*)out + ENTOFF + 8, ART_NAME, 32);
    v = FRMOFF;      memcpy(out + TABOFF + 0, &v, 4);
    v = 10u;         memcpy(out + TABOFF + 4, &v, 4);/* flag 10 = a fixed frame */

    *(unsigned short*)(out + FRMOFF + 0x00) = PANEL_W;
    *(unsigned short*)(out + FRMOFF + 0x02) = PANEL_H;
    out[FRMOFF + 0x08] = IX_KEY;                     /* transparency index     */
    out[FRMOFF + 0x09] = 0;                          /* raw 8bpp               */
    v = PIXOFF;      memcpy(out + FRMOFF + 0x10, &v, 4);

    draw_panel(out + PIXOFF, rows);
    return need;
}

/* ---- the composed ground -------------------------------------------------
   The frame the archive carries is OURS and drawn; the ground the player sees
   is composed at runtime from THEIR OWN install, and the composed pixels never
   leave their machine. `frontend.gaf`'s `back*` nine-slice is the shell's
   mottled panelling -- chosen by the owner over TA's dialog kit (`diatile` is
   one colour, flat black, in a grey bevel) and over a hybrid of the two.

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
    int x, y, i, j;
    if (!k->ok || k->w <= 0 || k->h <= 0) return;
    for (y = y0; y < y1; y += k->h)
        for (x = x0; x < x1; x += k->w)
            for (j = 0; j < k->h && y + j < y1; j++)
                for (i = 0; i < k->w && x + i < x1; i++)
                    px(dst, x + i, y + j, k->px[j * k->w + i]);
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

    for (i = 0; i < rows; i++) {
        int y = ROW_Y0 + ROW_PITCH * i;
        recess(s_ground, CTL_X - PAD, y - PAD, CTL_W + 2 * PAD, ROW_H + 2 * PAD);
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
   are anchored to the frame's RIGHT edge, never to a fixed coordinate. */
static void trigger_rect(int* x, int* y)
{
    int w = (int)g_ddraw.width;
    if (w < TRIG + MARGIN) w = TRIG + MARGIN;
    *x = w - MARGIN - TRIG;
    *y = 2;
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

static int common(char* b, int cap, int at, int id, const char* name,
                  int x, int y, int w, int h, int attribs, int colorf)
{
    at = gput(b, cap, at,
        "\t[COMMON]\r\n\t\t{\r\n"
        "\t\tid=%d;\r\n\t\tassoc=0;\r\n\t\tname=%s;\r\n"
        "\t\txpos=%d;\r\n\t\typos=%d;\r\n\t\twidth=%d;\r\n\t\theight=%d;\r\n"
        "\t\tattribs=%d;\r\n\t\tcolorf=%d;\r\n\t\tcolorb=0;\r\n"
        "\t\ttexturenumber=0;\r\n\t\tfontnumber=0;\r\n\t\tactive=1;\r\n"
        "\t\tcommonattribs=0;\r\n\t\thelp=;\r\n\t\t}\r\n",
        id, name, x, y, w, h, attribs, colorf);
    return at;
}

static int build_gui(char* b, int cap, int rows)
{
    int at = 0, i;

    at = gput(b, cap, at, "[GADGET0]\r\n\t{\r\n");
    at = common(b, cap, at, 0, "RENDER", 0, BAR_H, PANEL_W, PANEL_H, 0, 0);
    at = gput(b, cap, at,
        "\ttotalgadgets=%d;\r\n"
        "\t[VERSION]\r\n\t\t{\r\n\t\tmajor=1;\r\n\t\tminor=0;\r\n\t\trevision=1;\r\n\t\t}\r\n"
        "\tpanel=;\r\n\tcrdefault=;\r\n\tescdefault=;\r\n\tdefaultfocus=;\r\n\t}\r\n",
        rows * 2 + 2);

    /* the ground: an id=12 whose NAME is the GAF frame, over the whole panel */
    at = gput(b, cap, at, "[GADGET1]\r\n\t{\r\n");
    at = common(b, cap, at, 12, ART_NAME, 0, 0, PANEL_W, PANEL_H, 0, 15);
    at = gput(b, cap, at, "\t}\r\n");

    /* the caption. tools/guipanel.py rules the panel at DIV_TOP = 30 and the
       band above it is the title's -- an empty one is just a bare rule. */
    at = gput(b, cap, at, "[GADGET2]\r\n\t{\r\n");
    at = common(b, cap, at, 5, "TITLE", LBL_X, TITLE_Y, TITLE_W, 18, 1, 15);
    at = gput(b, cap, at, "\ttext=%s;\r\n\t}\r\n", "Render options");

    for (i = 0; i < rows; i++) {
        int y = ROW_Y0 + ROW_PITCH * i;
        /* the label, BESIDE its control -- every stock runtime screen puts it
           16 px above, which six rows have no room for (gui-gadgets.md 10.3) */
        at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", i * 2 + 3);
        at = common(b, cap, at, 5, "TEXT", LBL_X, y, LBL_W, ROW_H, 1, 15);
        at = gput(b, cap, at, "\ttext=%s;\r\n\t}\r\n", s_row[i].label);

        at = gput(b, cap, at, "[GADGET%d]\r\n\t{\r\n", i * 2 + 4);
        at = common(b, cap, at, 1, s_row[i].name, CTL_X, y, CTL_W, ROW_H, 1, 15);
        at = gput(b, cap, at,
            "\tstatus=0;\r\n\ttext=%s;\r\n\tquickkey=0;\r\n\tgrayedout=0;\r\n\tstages=%d;\r\n\t}\r\n",
            s_row[i].text, s_row[i].stages);
    }
    return at;
}

/* ---- reading the levers -------------------------------------------------- */
/* The screen is a FRONT END over the trigger files and the cfg, never a store
   of its own -- so the plates show what the levers say, read at open time. */
static void read_state(void)
{
    const TAGPU_LIGHT* L = tagpu_classicpp_light();
    int i, custom = 0;

    s_stage[R_ASSETS]  = tagpu_classicpp_assets() ? 1 : 0;
    s_stage[R_LIGHT]   = tagpu_classicpp_lit() ? 1 : 0;
    s_stage[R_SS]      = exists(SS_OFF) ? 0 : 1;

    s_stage[R_SHADOWS] = 0;
    for (i = 0; i < 3; i++) if (L && SHADOW_VAL[i] == L->shadows) s_stage[R_SHADOWS] = i;

    s_stage[R_SHADOWQ] = 2;
    for (i = 0; i < 4; i++) if (L && SHADOWQ_VAL[i] == L->shadowres) s_stage[R_SHADOWQ] = i;

    if (!tagpu_classicpp_on()) {
        s_stage[R_STYLE] = STYLE_CLASSIC;
    } else {
        /* Classic++ is every row below at its Classic++ value; anything else
           is Custom, which is why Custom is derived and never chosen. */
        custom = s_stage[R_ASSETS] != 1 || s_stage[R_LIGHT] != 1 ||
                 s_stage[R_SHADOWS] != 2 || s_stage[R_SHADOWQ] != 2 ||
                 s_stage[R_SS] != 1;
        s_stage[R_STYLE] = custom ? STYLE_CUSTOM : STYLE_PP;
    }
}

/* ---- pushing the state at the engine ------------------------------------- */

/* A row that cannot bite is greyed rather than left looking live. Four of the
   six describe CLASSIC++'s behaviour and are inert under Classic -- leaving
   them reading `On` there is the menu telling the player something untrue.
   Supersampling is not one of them: `tagpu_ss.off` is read by the native pass
   in both lanes. */
static int row_greyed(int row)
{
    if (row == R_STYLE || row == R_SS) return 0;
    if (s_stage[R_STYLE] == STYLE_CLASSIC) return 1;
    if (row == R_SHADOWQ) return s_stage[R_SHADOWS] == 2 ? 0 : 1;  /* Soft only */
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
       bit 0 and store a WORD, deliberately preserving bits 1..15. An earlier
       revision here stored a 32-bit 0/1 into the field directly, which cleared
       those bits and the two bytes at +0x13E/+0x13F as well.
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

/* `fresh` = the player just opened the menu, so the plates take their values
   from the levers. A re-open with fresh == 0 is a RECOVERY, and there the model
   is ours and must survive: the engine tears the whole in-game GUI stack down
   and rebuilds it (a new ARMMAIN2.GUI whose `under` is NULL) on a world click,
   and our panel hangs over the world, so its own clicks do it too. Re-reading
   the levers there put every plate back the moment it was clicked -- the cfg on
   disk said assets=1 while the button still read Off. */
static void menu_open(char* main_p, int fresh)
{
    void* gi = main_p + OFF_GUIINFO;
    char* expect = main_p + OFF_EXPECT;
    char* ctrls;
    void* gm;
    int w;

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
        w = (int)g_ddraw.width;
        if (w < PANEL_W + MARGIN) w = PANEL_W + MARGIN;   /* never off the left */
        *(short*)(ctrls + G_XPOS) = (short)(w - MARGIN - PANEL_W);
        *(short*)(ctrls + G_YPOS) = (short)BAR_H;
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
       0x40 repaint the panel had no surface, and the engine composited the
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

static void menu_close(char* main_p)
{
    /* Restore the buffer and let UpdateIngameGUI pop us -- the mirror of the
       engine's own idiom, and never GUI_Pop. It is CALLED rather than waited
       for, because its 21 call sites are all events: left to itself the popped
       screen would linger until the player next did something that changes the
       GUI stack. 1 is the argument both of the engine's own visible call sites
       pass (0x460635, 0x4929E3), and it only matters on the early-out path. */
    lstrcpynA((char*)main_p + OFF_EXPECT, s_saved, 16);
    s_gm = 0;
    ((upd_gui_fn)VA_UPDGUI)(1);
}

/* ---- the engine calls this ----------------------------------------------- */
/* __stdcall void(GUIInfo*): the actuated index arrives in gi->UIChange_f, not
   as an argument (GUIMEMSTRUCT+0x08, 0x4A967F). -1 is the pop path. */
void __stdcall tagpu_menu_oncommand(void* gi)
{
    char* main_p = *(char**)TA_MAIN;
    char* top;
    char* ctrls;
    int idx, row, i;

    if (!gi || !main_p) return;
    idx = *(int*)((char*)gi + GI_UICHANGE);
    /* -1 IS NOT PROOF OF A POP. GUI_Pop does set gi->UIChange_f to -1 before
       calling us (0x4A9673), but the pump also resets it (0x4AA096) and calls
       us again on the same click -- measured, and treating that as a pop
       destroyed the model on every click. `on_stack` in the tick is the
       authority on whether our screen is still there. */
    if (idx < 0) return;

    top   = *(char**)(main_p + OFF_TOPGUI);
    ctrls = top ? *(char**)(top + GM_CTRLS) : 0;
    if (!ctrls || idx < 1 || idx > *(short*)(ctrls + 0xB6)) return;

    /* index -> row, by the gadget's own name: the .GUI's layout is ours but a
       name lookup cannot go wrong if the layout ever changes. */
    row = -1;
    for (i = 0; i < s_nrows; i++)
        if (!memcmp(ctrls + (size_t)idx * STRIDE + G_NAME, s_row[i].name,
                    strlen(s_row[i].name) + 1)) { row = i; break; }
    if (row < 0) return;

    if (row == R_STYLE) {
        /* alternates Classic and Classic++; Custom is derived, so it is never
           a destination -- from Custom the click goes to Classic++ */
        s_stage[R_STYLE] = (s_stage[R_STYLE] == STYLE_PP) ? STYLE_CLASSIC : STYLE_PP;
        if (s_stage[R_STYLE] == STYLE_PP) {
            /* NOT R_SS: supersampling is orthogonal to the lane -- row_greyed
               deliberately exempts it from the switch's dependants -- so the
               preset must not silently undo a player who turned it off. */
            s_stage[R_ASSETS] = 1; s_stage[R_LIGHT] = 1;
            s_stage[R_SHADOWS] = 2; s_stage[R_SHADOWQ] = 2;
        }
    } else {
        s_stage[row] = (s_stage[row] + 1) % s_row[row].stages;
        if (s_stage[R_STYLE] != STYLE_CLASSIC) s_stage[R_STYLE] = STYLE_CUSTOM;
    }

    push_stages(gi);
    /* NOT the file. The cfg is written from the render thread at the next
       present: TA is lockstep and this is the game thread. */
    InterlockedExchange(&s_dirty, 1);
}

/* ---- the deferred write (render thread) ---------------------------------- */
/* The cfg is the PLAYER'S file and carries keys this screen does not own --
   sun, amb, penumbra, shadowlen. So it is rewritten rather than overwritten:
   every token that is not one of ours is copied through in the order it was
   read, and ours are appended. The reader's grammar is whitespace-separated
   `key=value`, so newline-separated output reads back identically. */
/* `sun=off` is the LEGACY spelling of `light=0`, and it BEATS us: tagpu_classicpp.c
   sets its `off` flag at :159 and then forces `s_lit = 0` at :100 AFTER the token
   loop, so a preserved `sun=off` makes the Dynamic lighting row read "On" and
   change nothing -- the same silent no-op this landing fixed for the lever files.
   So the menu owns that one token and drops it, expressing it through `light=`.
   `sun=<az>,<el>` is the sun DIRECTION, is the player's, and must survive. */
/* The keys this screen OWNS -- rewritten from the rows on every apply. Every
   other token in the cfg is a knob the player never reaches and is copied
   through untouched (write_cfg), which is what keeps a researcher's
   `penumbra=`, `shadowsun=`, `shade=` and so on alive across a click.
   `terrainshadow=` is deliberately NOT here and has no row: the ground casting
   on itself is a known defect (renderers.md 2.7b) whose default is now 0, so
   there must be no path through this screen that turns it back on. Adding a row
   for it means first fixing the defect. */
static int ours(const char* tok)
{
    return !_strnicmp(tok, "assets=", 7) || !_strnicmp(tok, "light=", 6) ||
           !_strnicmp(tok, "shadows=", 8) || !_strnicmp(tok, "shadowres=", 10) ||
           !lstrcmpiA(tok, "sun=off");
}

static void write_cfg(void)
{
    char in[2048], out[3072];
    HANDLE h;
    DWORD n = 0, wrote = 0;
    int at = 0;

    in[0] = 0;
    h = CreateFileA(CFG_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        if (!ReadFile(h, in, sizeof in - 1, &n, 0)) n = 0;
        CloseHandle(h);
        in[n] = 0;
        /* A cfg that FILLED the buffer may have had more after it, and this
           function exists to preserve what it does not own. Rewriting from a
           truncated read would silently drop the tail -- the exact opposite --
           so refuse instead. The rows still apply; only persistence is lost,
           and the log says so. */
        if (n >= sizeof in - 1) {
            mlog("menu: " CFG_FILE " is larger than the rewrite buffer - NOT rewritten "
                 "(the rows still apply this session)");
            return;
        }
    }

    {
        char* p = in;
        while (*p) {
            char* q;
            while (*p && (unsigned char)*p <= ' ') p++;
            if (!*p) break;
            q = p;
            while (*q && (unsigned char)*q > ' ') q++;
            if (*q) *q++ = 0;
            if (!ours(p)) at = gput(out, sizeof out, at, "%s\r\n", p);
            if (at < 0) { mlog("menu: cfg too large to rewrite - not written"); return; }
            p = q;
        }
    }

    at = gput(out, sizeof out, at, "assets=%d\r\nlight=%d\r\nshadows=%d\r\nshadowres=%d\r\n",
              s_stage[R_ASSETS], s_stage[R_LIGHT],
              SHADOW_VAL[s_stage[R_SHADOWS]], SHADOWQ_VAL[s_stage[R_SHADOWQ]]);
    if (at < 0) { mlog("menu: cfg too large to rewrite - not written"); return; }

    /* Write a temporary and RENAME it over the target. CREATE_ALWAYS truncates
       first, so writing in place means that between the truncate and the write
       the player's file is empty -- and everything this function copies through
       (sun, amb, penumbra, shadowlen) is gone if we are killed there, or if the
       write is short. MoveFileEx with MOVEFILE_REPLACE_EXISTING is atomic, so
       a reader sees either the old file or the new one and never a stub. The
       write is checked, and a failed one takes the temporary with it rather
       than leaving litter beside the cfg. */
    h = CreateFileA(CFG_TMP, GENERIC_WRITE, FILE_SHARE_READ, 0,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { mlog("menu: cannot write " CFG_TMP); return; }
    if (!WriteFile(h, out, (DWORD)at, &wrote, 0) || wrote != (DWORD)at) {
        CloseHandle(h);
        DeleteFileA(CFG_TMP);
        mlog("menu: short write to " CFG_TMP " - " CFG_FILE " left as it was");
        return;
    }
    CloseHandle(h);
    if (!MoveFileExA(CFG_TMP, CFG_FILE, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(CFG_TMP);
        mlog("menu: cannot replace " CFG_FILE " - left as it was");
    }
}

static void touch(const char* path)
{
    HANDLE h;
    if (exists(path)) return;
    h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

/* The two rows that are FILES rather than cfg keys.

   THE SWITCH NEEDS BOTH OF ITS FILES WRITTEN, not just the `.off` one, because
   `tagpu_opt.c`'s precedence is **an `.on` wins and an `.off` only defeats a
   pass that was on BY DEFAULT**. Driving only the `.off` file fails in both
   directions and the row silently does nothing (found in play 2026-09-09):

     - with a hand-armed `tagpu_classicpp.on` present -- what `tacli arm` writes
       -- the `.off` is inert and Classic++ can never be turned off;
     - on any tacli instance, which carries `tagpu_defaults.off`, the table's
       default does not apply, so deleting the `.off` is not enough to turn it
       ON either.

   Owning both is correct under all three configurations: the shipped DLL
   (defaults on), a tacli instance (defaults off), and a hand-armed `.on`.

   Supersampling is NOT the same shape and deliberately keeps one file:
   `tagpu_ss.off` is read directly with GetFileAttributesA in tagpu_native.c
   and tagpu_render3do.c, there is no `tagpu_ss.on` and no table entry, so
   inventing one would arm nothing and confuse the next reader. */
static void write_levers(void)
{
    int ss  = s_stage[R_SS] == 1;
    int cpp = s_stage[R_STYLE] != STYLE_CLASSIC;

    if (ss) DeleteFileA(SS_OFF); else touch(SS_OFF);

    if (cpp) { DeleteFileA(CPP_OFF); touch(CPP_ON); }
    else     { DeleteFileA(CPP_ON);  touch(CPP_OFF); }
}

void tagpu_menu_present(void)
{
    if (!s_installed) return;
    if (InterlockedExchange(&s_dirty, 0)) {
        write_cfg();
        write_levers();
    }
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
   wrote. (The same shape as the field-notes patch-2b bug, where a cursor
   change quietly altered what a left click did.)

   The press toggles and the RELEASE is consumed too. Letting the release
   through would leave the engine holding a button it never saw pressed --
   the G13e review's HIGH finding, and it costs the rest of the session. */
/* The sprocket always, and the panel while it is open. Both rects are computed
   the same way the drawing and the push do, so there is one source of truth. */
int tagpu_menu_owns_point(int gx, int gy)
{
    int x, y, w;

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
    w = (int)g_ddraw.width;
    if (w < PANEL_W + MARGIN) w = PANEL_W + MARGIN;
    x = w - MARGIN - PANEL_W;
    return gx >= x && gx < x + PANEL_W && gy >= BAR_H && gy < BAR_H + PANEL_H;
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
           G13e failure this guard exists to prevent, and the release path
           alone did not prevent it. */
        s_pressed = 0;
        return 0;
    }

    s_pressed = 1;
    s_want = !s_want;
    if (s_want) s_fresh = 1;            /* the player's open reads the levers */
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

void tagpu_menu_init(void)
{
    static char gui[8192];
    static unsigned char gaf[16 + 0x28 + 8 + 0x18 + PANEL_W * PANEL_H];
    TAGPU_UFO_FILE f[2];
    char b[240];
    int len, wrote, armed;
    unsigned glen;

    read_tokens();

    /* The archive is written UNCONDITIONALLY every launch, arm file or not:
       staleness after a DLL upgrade is the one failure here that would be
       genuinely confusing, and it costs a few ms. */
    len = build_gui(gui, sizeof gui, s_nrows);
    if (len < 0) { mlog("menu: NOT armed - the generated .GUI does not fit"); return; }
    glen = build_gaf(gaf, sizeof gaf, s_nrows);
    if (!glen) { mlog("menu: NOT armed - the panel frame does not fit"); return; }
    f[0].path = "guis/render.gui";
    f[0].data = gui;
    f[0].size = (unsigned)len;
    f[1].path = "anims/render.gaf";
    f[1].data = gaf;
    f[1].size = glen;
    wrote = tagpu_ufo_write(UFO_FILE, f, 2);

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
    }

    _snprintf(b, sizeof b,
              "menu: %s " UFO_STAMP " ufo=%d rows=%d gui=%d gaf=%u trigger=%d bytes "
              "(RENDER.GUI over DrawGameScreen 0x468CF0; open with " OPEN_FILE ")",
              armed ? "ARMED" : "NOT armed", wrote, s_nrows, len, glen, s_drawTrigger);
    b[sizeof b - 1] = 0;
    mlog(b);
}

int tagpu_menu_installed(void) { return s_installed; }
