/* tagpu_replays.c -- the shell's Replays browser and the main menu's REPLAYS button. API and
   arming: tagpu_replays.h. Plan: research/notes/tadr-port/demo-recorder-s5-ui.md.

   Every engine address below was read off pristine/TotalA.exe.pristine (DISASSEMBLED unless
   marked). The four sites hold the same bytes in the exes of every compatibility setup and no
   Patch Loader table writes inside them (MEASURED 2026-10-01, a byte compare of the six setups'
   exes and of the Mayhem and ProTA loader tables).

   GAME THREAD, ALL OF IT. GUI_Load, the main-menu builder, the PCX loader, the list draw and every
   OnCommand are shell code with no caller on the loader thread, so nothing here is read or
   written by two threads and nothing here needs a lock.

   THE MAIN MENU, in the order the builder 0x4263B0 runs it:
     S1  0x4AACAE, inside GUI_Load(gi, "MAINMENU.GUI", 0x80) after the screen is parsed and pushed
         and before its stage 1: one id-1 record is appended, a copy of SINGLE, and the per-mod
         table's record moves that need no pixel are made.
     S3  0x429290, the bitmaps\<name>.PCX loader, on its return for FrontendX: the plate is
         pasted into the surface it has just decoded, and ProTA's badge moved.
     S2  0x42644C, in the builder just after its backdrop call: the label moves that depend on a
         pixel guard, and the screen's OnCommand slot, which the builder wrote at 0x426432.
   The builder then runs stage 1 again (0x426523) and positions the version label (0x426615,
   xpos alone), so every record edit above is in place before the screen is first built.

   THE BROWSER. Our MAINMENU OnCommand pushes the style's screen (REPLAYS.GUI, or REPLAYSE.GUI
   over Escalation's ground) the way the Load Game screen is pushed (0x4931D0): GUI_Load with
   LOADGAME.GUI's flags 0x980, the screen's OnCommand and context, the backdrop through 0x4288D0,
   the rows through SetListText. Back leaves the click unanswered, so
   the pump calls the screen with -1, pops it and repaints the main menu. S4, the list draw
   0x4A1B40, draws the rows' badges and lengths after every repaint of the recording list. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tagpu_log.h"
#include "tagpu_detour.h"
#include "tagpu_art.h"
#include "tagpu_recorder.h"    /* tagpu_rec_mod_print: the running game's archives */
#include "tagpu_recreader.h"
#include "tagpu_replays.h"
#include "tagpu_replay.h"
#include "tagpu_gaf.h"        /* TAGPU_GAF_EXACT: the badges are drawn as drawn */
#include "tagpu_gui.h"        /* tagpu_gui_asset_exact: so are the ground and the plate */

/* ---- engine addresses ------------------------------------------------------------------------ */
#define TA_MAIN          0x00511DE8u    /* TAdynmemStruct**                                     */
#define OFF_GUIINFO      0x519u         /* main + this = GUIInfo                                */
#define GI_TOP           0x18           /* GUIInfo: the top GUIMEMSTRUCT                        */
#define GI_CLICKX        0x3C           /* the point the pump hit-tests a click against,        */
#define GI_CLICKY        0x40           /*   refreshed once per pump before its dispatch        */
#define GI_UICHANGE      0x60           /* the actuated index in, the answer out                */
#define GM_CTRLS         0x04
#define GM_ONCMD         0x08
#define GM_CTX           0x0C
#define STRIDE           0x15B          /* a gadget record                                      */
#define G_ID             0x00
#define G_ASSOC          0x01
#define G_NAME           0x02           /* char[16]                                             */
#define G_XPOS           0x13
#define G_YPOS           0x15
#define G_W              0x17
#define G_H              0x19
#define G_ATTRIBS        0x1B
#define G_ACTIVE         0x29
#define G_FREE1          0x2B           /* freed by the teardown when +0xB4 bit 0 is set        */
#define G_FREE2          0x2F
#define G_HELP           0x33           /* char[128]                                            */
#define G_OWNS           0xB4
#define G_TEXT           0xB6           /* char[128]; a panel's +0xB6 is its record count       */
#define G_STAGES         0x136
#define G_QUICKKEY       0x13A
#define G_GRAYED         0x13C          /* u16, bit 0 the flag                                  */
#define P_COUNT          0xB6           /* ControlsAry[0]: i16, the records after it            */
#define P_SURF           0xBC           /* ControlsAry[0]: the panel's OFFSCREEN                */
#define L_SELECTED       0xBA           /* a list: i16                                          */
#define L_TOP            0xBC
#define L_COUNT          0xC0
#define L_ITEMH          0xDA
#define L_CALLBACK       0xCE           /* a list: stdcall (gi, rec), called when a click
                                           (0x4A3CEF) or Up/Down (0x4A9DEC, 0x4A9E2D) changes
                                           the selection; LOADGAME's is 0x492DE0 (0x4932C5)     */
#define GUI_RECORDS      200            /* GUI_Load allocates 0x10F57 bytes and puts ControlsAry
                                           at +0x3F: 0x10F18 = 200 records, 0..199 (0x4AAA68,
                                           0x4AAA95)                                           */

#define VA_GUI_LOAD      0x004AA8F0u    /* (gi, name, flags) stdcall                            */
#define VA_ACTDONE       0x004AB0A0u    /* (gi): UIChange_f = -1, "handled"                     */
#define VA_SETDIRTY      0x0049FA90u    /* (gi): gi+0xCCA = 1, the pump repaints                */
#define VA_TAKESKEYS     0x0049FB10u    /* (gi, on) stdcall: the top screen's +0x18, without which
                                           the pump hands it no key (0x4AA040) -- Enter, Escape
                                           (crdefault, escdefault: 0x4A9C80, 0x4A9C09) and a
                                           list's Up and Down; GUI_Load clears it (0x4AAC95)    */
#define VA_SETLABEL      0x004A07D0u    /* (gi, name, text) stdcall ret 0xC; a missing name is
                                           skipped (0x4A0824)                                   */
#define VA_SETLIST       0x004A32A0u    /* (gi, name, blob, count, flags) stdcall ret 0x14; a
                                           missing name is FATAL (0x4A3310)                      */
#define VA_LISTDRAW      0x004A1B40u    /* (gi, idx) stdcall ret 8: draws a TOP-screen list     */
#define VA_BACKDROP      0x004288D0u    /* (name, clearflip, setpal, nobg) stdcall ret 0x10     */
#define VA_SOUND         0x0047F1A0u    /* (name, arg) stdcall                                  */
#define VA_GAFBLIT       0x004B7F90u    /* (ctx, frame, x, y) stdcall                           */
#define VA_GLYPH         0x004B7F30u    /* (font, ch) stdcall ret 8: font + ch*8 + 0x28, ch
                                           bounded by the font's own u16 count at +0           */
#define VA_TEXTDRAW      0x004A50E0u    /* (ctx, text, x, y, width or -1, remap) stdcall ret 0x18:
                                           the list's own row draw (0x4A1F05)                   */
#define VA_FNT_FONT      0x004C1440u    /* () -> the .FNT face in force                         */
#define VA_FNT_WIDTH     0x004C1480u    /* (face, text) -> its width                            */
#define VA_FNT_HEIGHT    0x004C1450u    /* () -> its height                                     */
#define VA_FONTSTATE     0x0051FBA4u    /* -> the text state: +0x14 the GAF font in force or 0,
                                           whose +0x0C is the glyph table (0x4A1C0D)            */
#define VA_MM_ONCMD      0x00425D80u    /* MAINMENU's OnCommand                                 */
#define VA_STR_CFG       0x005098A3u    /* the config file's name (totala.ini, taesc.ini, ...)  */
#define VA_STR_ARCHIVE   0x005028CCu    /* the main archive's format (rev%s.GP3, TAE%s.GP3, ...) */
#define VA_STR_FRONTEND  0x005031D8u    /* "FrontendX", pushed only by the builder (0x42642D)   */

typedef void* (__stdcall *gui_load_fn)(void* gi, const char* name, int flags);
typedef void  (__stdcall *gi_fn)(void* gi);
typedef void  (__stdcall *gi_int_fn)(void* gi, int v);
typedef void  (__stdcall *set_label_fn)(void* gi, const char* name, const char* text);
typedef void  (__stdcall *set_list_fn)(void* gi, const char* name, const char* blob, int count,
                                       const unsigned char* flags);
typedef void  (__stdcall *list_draw_fn)(void* gi, int idx);
typedef int   (__stdcall *backdrop_fn)(const char* name, int clearflip, int setpal, int nobg);
typedef void  (__stdcall *sound_fn)(const char* name, int arg);
typedef void  (__stdcall *gaf_blit_fn)(void* ctx, const void* frame, int x, int y);
typedef const unsigned short* (__stdcall *glyph_fn)(const void* font, int ch);
typedef void  (__stdcall *text_draw_fn)(void* ctx, const char* text, int x, int y, int w,
                                        int remap);
typedef void* (__stdcall *fnt_font_fn)(void);
typedef int   (__stdcall *fnt_width_fn)(void* face, const char* text);
typedef int   (__stdcall *fnt_height_fn)(void);
typedef void  (__stdcall *oncmd_fn)(void* gi);
typedef void  (__stdcall *list_cb_fn)(void* gi, void* rec);

/* ---- the screen's layout: 640x480, the shell's own coordinates ------------------------------- */
#define GUI_FLAGS        0x980          /* LOADGAME.GUI's (0x4931D7): 0x800 dims the screen below
                                           and repaints it at the pop, 0x80 marks the screen     */
#define SCR_W            640
#define SCR_H            480
#define TITLE_TEXT       "REPLAYS"
#define TITLE_GAP        3
#define LIST_ASSOC       7              /* the list and its scroll bar, and nothing else         */
#define TL_ASSOC         9
#define PLAYER_ROWS      5              /* the players, two columns of five                      */
#define PLAYER_PITCH     13
#define LABEL_H          13
#define STAR_GAP         4              /* between the star and the row's text                   */
#define BADGE_GAP        3              /* between the map name and a badge, and two badges      */

#define ENT_MAX          1024           /* the recordings the browser lists, newest first        */
#define FILE_MAX         112
#define ROW_TEXT         120
#define INDEX_PATH       "replays\\index.dat"
#define INDEX_TMP        "replays\\index.tmp"
#define INDEX_MAGIC      "IMPURE-REPLAYS-INDEX\t1"
#define INDEX_CAP        (4u << 20)
#define MARKS_MAX        64             /* a recording's bookmarks                               */
#define MARK_GAP         30             /* the closest two bookmarks may be, in GameTime: 1 s    */
#define VERSION_MAX      7              /* RT_GAME_VERSION's characters at most (tagpu_recorder.c) */
#define MOD_NAME_MAX     16             /* an s_mods name, NUL included                          */
#define VA_VERSION_TEXT  0x005031B4u    /* the main menu's version, RT_GAME_VERSION's source     */
#define VA_INIT_HPI      0x0041D4C0u    /* InitTAHPIAry, cdecl, no argument: mounts what is new  */
#define VA_GFX_PP        0x0051FBD0u    /* the graphics globals: +0x61C the archives mounted     */
#define GFX_ARC_LIST     0x618
#define GFX_ARC_COUNT    0x61C
#define ARC_PATH         0x14           /* a mounted archive's handle: its full path ...        */
#define ARC_PATHMAX      0x104          /* ... in a buffer this long (OpenHAPIFile 0x4BDD70)     */
#define MOUNT_CALLS_MAX  64

/* ---- the per-mod table (the plan's *The per-mod table*) ---------------------------------------
   A mod is named by the engine's own strings: the config file and the main archive's format at
   fixed .data addresses, which a mod's exe or its Patch Loader rewrites (MEASURED in the loader
   tables: ConfigFileName mayhem.ini / ProTA.ini, rev31 -> may%s.GP3 / ProTA.gp3), and Twilight by
   its main menu's own label, "Total Annihilation: Twilight v2.0 Beta 98" (MEASURED in its
   guis/mainmenu.gui). Positions are MEASURED on live captures of each setup's main menu. A mod
   the table does not name, and an entry whose guard fails, take the retail placement: REPLAYS on
   our plate below the centre, and nothing of the mod's own moved. */
enum { MOD_UNKNOWN, MOD_RETAIL, MOD_ESC, MOD_MAYHEM, MOD_PROTA, MOD_TAZERO, MOD_TWILIGHT, MOD_N };

/* The generation of index.dat's `I` fields: bump it whenever this table's names or archive
   patterns change, or the rule that derives the `I` fields from a recording does (mod_of,
   info_read). A line of another generation is read again from its recording. */
#define INFO_GEN 1

typedef struct {
    const char* name;
    const char* title;          /* what the browser calls it                                   */
    const char* cfg;            /* matched against VA_STR_CFG, or NULL                          */
    const char* archive;        /* matched against VA_STR_ARCHIVE, or NULL; a recording's
                                   mounted archives are matched against it too (mod_of)       */
    const char* style;
    short       rx;             /* REPLAYS' xpos; every entry's is at y 393, 96 x 20            */
    char        plate;          /* our plate under it                                          */
    short       label_x, label_y, label_to;  /* DebugString parsed at (x, y) moves to y         */
    char        badge;          /* ProTA's badge moves up (BADGE_*)                             */
    char        shift_right;    /* INTRO and EXIT 24 px right, from x 409                       */
} Mod;

static const Mod s_mods[MOD_N] = {
    { "unknown",    "Unknown",            NULL,         NULL,        "retail",
      272, 1, 0,   0,   0,   0, 0 },
    { "retail",     "Total Annihilation", "totala.ini", NULL,        "retail",
      272, 1, 0,   0,   0,   0, 0 },
    { "escalation", "Escalation",         "taesc.ini",  "TAE%s.GP3", "escalation",
      272, 1, 0,   0,   0,   0, 0 },
    { "mayhem",     "Total Mayhem",       "mayhem.ini", "may%s.GP3", "retail",
      272, 1, 327, 416, 369, 0, 0 },
    { "prota",      "ProTA",              "ProTA.ini",  "ProTA.gp3", "retail",
      272, 1, 323, 416, 362, 1, 0 },
    { "tazero",     "TA Zero",            "tazero.ini", "TAZ%s.GP3", "retail",
      272, 1, 0,   0,   0,   0, 0 },
    { "twilight",   "Twilight",           NULL,         NULL,        "twilight",
      317, 0, 0,   0,   0,   0, 1 },
};
#define TWILIGHT_PREFIX  "Total Annihilation: Twilight"

#define SIB_NAME         "SINGLE"       /* the button REPLAYS is copied from, in every setup     */
#define SIB_X            139
#define SIB_Y            393
#define BTN_W            96
#define BTN_H            20
#define REPLAYS_Y        393
#define SHIFT_FROM       409
#define SHIFT_BY         24
#define PLATE_X          263
#define PLATE_Y          386
/* ProTA's FrontendX differs from retail's in exactly the badge, x 283..355, y 392..437 (3,189
   pixels; MEASURED on the decoded pair). It moves to y 338, onto wall that is retail's own; both
   rects are guarded by the FNV-1a of their index bytes, row by row, computed off ProTA 4.8's and
   retail's bitmaps/frontendx.pcx. */
#define BADGE_X          283
#define BADGE_Y          392
#define BADGE_W          73
#define BADGE_H          46
#define BADGE_TO         338
#define BADGE_SRC_FNV    0x559B02CDu
#define BADGE_DST_FNV    0xCC236597u

/* ---- the layouts: where each style's ground paints its boxes ----------------------------------
   Each layout is a screen of its own in the archive, so a style's rects and its ground always
   travel together: a style whose ground falls back to retail's opens retail's screen too
   (tagpu_replays_files). The rects are the ground's painted interiors, MEASURED on the 640x480
   art (2026-10-01), x..x+w and y..y+h exclusive.

   A BUTTON'S SIZE IS ONE OF THE ENGINE'S FACES. At load (0x4A7FC5..0x4A8111) a plain button with no
   sequence of its own name takes commongui's BUTTONS0 (0x5098EC) and, of its frames 0, 4, 8, ...,
   the one nearest the record's width and height (|dw| + |dh|, 0x4A80C5..0x4A810F); the face is
   drawn at the record's corner, its hotspot zeroed (0x4A80AC). BUTTONS0's sizes are 16x16,
   321x20, 120x20, 96x20, 112x20, 80x20 and 96x31 in retail's commongui.gaf and in Escalation's
   (TAESC.gp3) alike, so every button below is one of those exactly. Escalation's slots are 25 px
   high and 89..93 wide: 96x20 is the nearest face but overruns the 92-px plates by 2 px a side,
   so each holds an 80x20 face, centred. */
typedef struct { short x, y, w, h; } Rect;
enum { BT_WATCH, BT_PLAYON, BT_FAV, BT_ALL, BT_BACK, BT_N };
typedef struct {
    const char* screen;         /* GUI_Load's name, and its panel's (ControlsAry[0])            */
    const char* path;           /* the file in the archive                                     */
    Rect  title;                /* x: where the title starts; y..y+h: the bar it is centred in */
    Rect  list, bar;            /* the recordings and their scroll bar                         */
    Rect  minimap;
    Rect  mapname;
    short px0, pcol, pw, py0;   /* the players: first column's x, column pitch, the width a
                                   name and its result are cut to, in the list font, and the
                                   first row's y                                               */
    Rect  tlcap, timelines;
    Rect  modline, modhint;     /* the selected recording's mod and version, and a hint about
                                   it, under the timelines in their box                         */
    Rect  count, footer;        /* bars: replays_open centres the label's text in its bar      */
    Rect  btn[BT_N];
} Layout;

/* Retail's: the plate between Favourite and the grille, 338..422 x 437..461, holds All mods. */
static const Layout L_RETAIL = {
    "REPLAYS.GUI", "impure/replays.gui",
    { 24, 6, 326, 35 },
    { 22, 48, 310, 326 }, { 334, 48, 14, 326 },
    { 370, 54, 90, 90 },
    { 470, 54, 142, LABEL_H },
    470, 72, 68, 70,
    { 370, 150, 242, 12 }, { 370, 162, 242, 176 },
    { 372, 340, 240, LABEL_H }, { 372, 354, 240, LABEL_H },
    { 440, 14, 180, 16 }, { 20, 386, 600, 22 },
    { { 20, 440, 96, 20 }, { 124, 440, 120, 20 }, { 252, 440, 80, 20 }, { 340, 440, 80, 20 },
      { 540, 440, 80, 20 } },
};

/* Escalation's boxes: the list well 27..340 x 58..353 holds the list and its bar 2 px in; the
   minimap box 370..448 x 59..143 (78x84) a 78-px square, centred; the map and players box
   464..614 x 59..144 the map's name and two columns 2 px in; the timelines box 370..614 x
   163..353 its caption and list 2 px in; the count bar 444..618 x 20..39 and the footer bar
   21..618 x 372..397 their text 8 px in, clear of the rivets at their ends; the title bar
   21..348 x 11..41, the title 4 px in; the plates y 431..456,
   x 21..113, 123..215, 225..318, 328..410 and 529..618. The mod line and its hint take the
   timelines box's last two rows. */
static const Layout L_ESCALATION = {
    "REPLAYSE.GUI", "impure/replayse.gui",
    { 25, 11, 323, 30 },
    { 29, 60, 293, 291 }, { 324, 60, 14, 291 },
    { 370, 62, 78, 78 },
    { 466, 61, 146, LABEL_H },
    466, 74, 70, 74,
    { 372, 165, 240, 12 }, { 372, 179, 240, 143 },
    { 372, 324, 240, LABEL_H }, { 372, 338, 240, LABEL_H },
    { 448, 20, 162, 19 }, { 29, 372, 581, 25 },
    { { 27, 433, 80, 20 }, { 129, 433, 80, 20 }, { 231, 433, 80, 20 }, { 329, 433, 80, 20 },
      { 533, 433, 80, 20 } },
};

/* by STYLES' order; Twilight's screen is retail's */
static const Layout* const STYLE_LAYOUT[3] = { &L_RETAIL, &L_ESCALATION, &L_RETAIL };

/* ---- state -------------------------------------------------------------------------------------- */
static int   s_armed;
static int   s_mod = -1;            /* s_mods index, set at the first main-menu visit          */
static int   s_movesOk;             /* the entry's record guards held at the visit             */
static int   s_appended;            /* REPLAYS was appended in this main-menu visit: S1 -> S3/S2 */
static int   s_badgeOk = -1;        /* ProTA's pixel guards, decided at the decode             */
static int   s_logged;
static void* s_gm;                  /* the REPLAYS GUIMEMSTRUCT while it is on the stack       */
static int   s_recsIdx;             /* its recording list's record index                       */

static const Layout* const LAYOUTS[2] = { &L_RETAIL, &L_ESCALATION };
static char  s_gui[2][12288];       /* LAYOUTS' screens, as written into the archive           */
static const char* const STYLES[3] = { "retail", "escalation", "twilight" };
static char  s_groundName[3][12];   /* the backdrop each style opens with                      */
static const Layout* s_layout[3];   /* the screen each style opens: its ground's               */
static const Layout* s_cur = &L_RETAIL; /* the screen open, or last opened                     */
static char  s_groundPath[3][32];
static unsigned char* s_groundPcx[3];
static unsigned s_groundLen[3];

static void rlog(const char* fmt, ...)
{
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    tagpu_log(b);
}

static int style_index(const char* style)
{
    int i;
    for (i = 0; i < 3; i++) if (!strcmp(STYLES[i], style)) return i;
    return 0;
}

static const char* mod_style(void)
{
    return s_mods[s_mod < 0 ? MOD_UNKNOWN : s_mod].style;
}

/* ================================================================================================
   The archive's files
   ================================================================================================ */

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

/* One gadget's [COMMON] block, opening its [GADGETg]; the caller writes the type's own keys and
   closes it. The stock screens' values where a field's meaning is not ours to choose: lists and
   buttons carry LOADGAME.GUI's commonattribs 54, labels its 0. */
static int common(char* b, int cap, int at, int g, int id, int assoc, const char* name, int x,
                  int y, int w, int h, int attribs, int colorf, int texture, int cattr)
{
    return gput(b, cap, at,
        "[GADGET%d]\r\n\t{\r\n\t[COMMON]\r\n\t\t{\r\n"
        "\t\tid=%d;\r\n\t\tassoc=%d;\r\n\t\tname=%s;\r\n"
        "\t\txpos=%d;\r\n\t\typos=%d;\r\n\t\twidth=%d;\r\n\t\theight=%d;\r\n"
        "\t\tattribs=%d;\r\n\t\tcolorf=%d;\r\n\t\tcolorb=0;\r\n"
        "\t\ttexturenumber=%d;\r\n\t\tfontnumber=0;\r\n\t\tactive=1;\r\n"
        "\t\tcommonattribs=%d;\r\n\t\thelp=;\r\n\t\t}\r\n",
        g, id, assoc, name, x, y, w, h, attribs, colorf, texture, cattr);
}

/* A label: attribs bit 2 right-aligns its text and bit 1 centres it (0x4A5880, 0x4A590C);
   neither is left-aligned. LOADGAME.GUI's labels carry 33. */
#define LBL_LEFT  33
#define LBL_RIGHT 37
static int label(char* b, int cap, int at, int g, const char* name, int x, int y, int w, int h,
                 int attribs, const char* text)
{
    at = common(b, cap, at, g, 5, 0, name, x, y, w, h, attribs, 15, 0, 0);
    return gput(b, cap, at, "\ttext=%s;\r\n\tlink=;\r\n\t}\r\n", text);
}

static int button(char* b, int cap, int at, int g, const char* name, const Rect* r,
                  const char* text, int grayed)
{
    at = common(b, cap, at, g, 1, 0, name, r->x, r->y, r->w, r->h, 2, 15, 0, 54);
    return gput(b, cap, at, "\tstatus=0;\r\n\ttext=%s;\r\n\tquickkey=0;\r\n\tgrayedout=%d;\r\n"
                            "\tstages=0;\r\n\t}\r\n", text, grayed);
}

/* A text list: attribs bit 0 is what puts its rows at x + 2 (0x4A1E39); a list with none of
   bits 0, 1 and 2 draws its rows from an unset x. Bit 0x40 makes the click handler report every
   click on a row (0x4A3D03), which the pump takes as the actuated gadget (0x4AA29A), so the
   screen's OnCommand hears a click that does not change the selection too; the bit's two other
   readers are the button handlers (0x4A6C77, 0x4A712E). */
#define LIST_PLAIN   1
#define LIST_CLICKS  0x41
static int list(char* b, int cap, int at, int g, const char* name, int assoc, int x, int y,
                int w, int h, int attribs)
{
    at = common(b, cap, at, g, 2, assoc, name, x, y, w, h, attribs, 15, 0, 54);
    return gput(b, cap, at, "\t}\r\n");
}

#define GADGETS 25
#define ALL_ON  "All mods"          /* ALLMODS while the list holds the running mod's alone   */
#define ALL_OFF "This mod"          /* and while it holds every recording                     */

static int build_gui(char* b, int cap, const Layout* L)
{
    int at = 0, g = 1, i;
    char nm[8];

    at = gput(b, cap, at, "[GADGET0]\r\n\t{\r\n");
    at = gput(b, cap, at,
        "\t[COMMON]\r\n\t\t{\r\n\t\tid=0;\r\n\t\tassoc=0;\r\n\t\tname=%s;\r\n"
        "\t\txpos=0;\r\n\t\typos=0;\r\n\t\twidth=%d;\r\n\t\theight=%d;\r\n"
        "\t\tattribs=0;\r\n\t\tcolorf=0;\r\n\t\tcolorb=0;\r\n\t\ttexturenumber=0;\r\n"
        "\t\tfontnumber=0;\r\n\t\tactive=1;\r\n\t\tcommonattribs=0;\r\n\t\thelp=;\r\n\t\t}\r\n"
        "\ttotalgadgets=%d;\r\n"
        "\t[VERSION]\r\n\t\t{\r\n\t\tmajor=1;\r\n\t\tminor=0;\r\n\t\trevision=1;\r\n\t\t}\r\n"
        "\tpanel=;\r\n\tcrdefault=WATCH;\r\n\tescdefault=BACK;\r\n\tdefaultfocus=RECS;\r\n\t}\r\n",
        L->screen, SCR_W, SCR_H, GADGETS);

    /* the recordings, and their scroll bar: one assoc binds the two (gui-gadgets 2.6.1); the
       slider's range, thick and knob are LOADGAME.GUI's SLIDER scaled to this height, and the
       engine rewrites them at load */
    at = list(b, cap, at, g++, "RECS", LIST_ASSOC, L->list.x, L->list.y, L->list.w, L->list.h,
              LIST_CLICKS);
    at = common(b, cap, at, g++, 4, LIST_ASSOC, "RECSBAR", L->bar.x, L->bar.y, L->bar.w, L->bar.h,
                2, 4, 102, 54);
    at = gput(b, cap, at, "\trange=%d;\r\n\tthick=56;\r\n\tknobpos=0;\r\n\tknobsize=%d;\r\n\t}\r\n",
              L->bar.h - 6, L->bar.h - 9);

    /* the minimap: an id-6 surface gadget one pixel wider and higher than its box, since the frame
       it is given is stamped over the record's width and height less one (THE MINIMAP) */
    at = common(b, cap, at, g++, 6, 0, "MINIMAP", L->minimap.x, L->minimap.y, L->minimap.w + 1,
                L->minimap.h + 1, 2, 15, 0, 0);
    at = gput(b, cap, at, "\thotornot=0;\r\n\t}\r\n");

    at = label(b, cap, at, g++, "MAPNAME", L->mapname.x, L->mapname.y, L->mapname.w, L->mapname.h,
               LBL_LEFT, "");
    for (i = 0; i < REC_PLAYERS; i++) {
        _snprintf(nm, sizeof nm, "P%d", i);
        nm[sizeof nm - 1] = 0;
        at = label(b, cap, at, g++, nm, L->px0 + L->pcol * (i / PLAYER_ROWS),
                   L->py0 + PLAYER_PITCH * (i % PLAYER_ROWS), L->pw, LABEL_H, LBL_LEFT, "");
    }
    at = label(b, cap, at, g++, "TLCAP", L->tlcap.x, L->tlcap.y, L->tlcap.w, L->tlcap.h, LBL_LEFT,
               "Timelines");
    at = list(b, cap, at, g++, "TIMELINES", TL_ASSOC, L->timelines.x, L->timelines.y,
              L->timelines.w, L->timelines.h, LIST_PLAIN);
    at = label(b, cap, at, g++, "MODLINE", L->modline.x, L->modline.y, L->modline.w,
               L->modline.h, LBL_LEFT, "");
    at = label(b, cap, at, g++, "MODHINT", L->modhint.x, L->modhint.y, L->modhint.w,
               L->modhint.h, LBL_LEFT, "");
    at = label(b, cap, at, g++, "COUNT", L->count.x, L->count.y, L->count.w, L->count.h, LBL_RIGHT,
               "");
    at = label(b, cap, at, g++, "FOOTER", L->footer.x, L->footer.y, L->footer.w, L->footer.h,
               LBL_LEFT, "");

    at = button(b, cap, at, g++, "WATCH", &L->btn[BT_WATCH], "Watch", 0);
    at = button(b, cap, at, g++, "PLAYON", &L->btn[BT_PLAYON], "Play on", 1);
    at = button(b, cap, at, g++, "FAV", &L->btn[BT_FAV], "Favourite", 0);
    at = button(b, cap, at, g++, "ALLMODS", &L->btn[BT_ALL], ALL_ON, 0);
    at = button(b, cap, at, g++, "BACK", &L->btn[BT_BACK], "Back", 0);
    return g - 1 == GADGETS ? at : -1;
}

/* The ground: the style's own 640x480 `ground`, with the title set into it from the style's
   `font` -- text is never painted into the art (the plan's *Text*). Returns 0 when the style has
   no such art. */
static int compose_ground(const char* style, const Layout* L, unsigned char* px)
{
    const TAGPU_ART_ENTRY* g = tagpu_art_entry(style, "ground");
    const TAGPU_ART_ENTRY* f = tagpu_art_entry(style, "font");
    const TAGPU_ART_FRAME* gf;
    const char* t;
    int x = L->title.x, i, j, h = 0, hoty = 0, base;

    if (!g || g->nframes != 1 || !f) return 0;
    gf = &g->frames[0];
    if (gf->w != SCR_W || gf->h != SCR_H) return 0;
    for (i = 0; i < SCR_W * SCR_H; i++)
        px[i] = gf->pix[i] == gf->key ? 0 : gf->pix[i];
    /* the glyphs stand on one baseline (their hotspot is it); the tallest is centred in the bar,
       a spare row going below */
    for (t = TITLE_TEXT; *t; t++) {
        const TAGPU_ART_FRAME* c = tagpu_art_char(f, (unsigned char)*t);
        if (!c) return 0;
        if (c->h > h) h = c->h;
        if (c->hoty > hoty) hoty = c->hoty;
    }
    base = L->title.y + (L->title.h - h + 1) / 2 + hoty;
    for (t = TITLE_TEXT; *t; t++) {
        const TAGPU_ART_FRAME* c = tagpu_art_char(f, (unsigned char)*t);
        int top = base - c->hoty;
        for (j = 0; j < c->h; j++)
            for (i = 0; i < c->w; i++) {
                int dx = x + i, dy = top + j;
                unsigned char v = c->pix[j * c->w + i];
                if (v != c->key && dx >= 0 && dx < SCR_W && dy >= 0 && dy < SCR_H)
                    px[dy * SCR_W + dx] = v;
            }
        x += c->w + TITLE_GAP;
    }
    return 1;
}

/* An 8-bit ZSoft PCX, run-length coded, as the engine's 0x4CAF30 reads it. Its palette is never
   shown: the backdrop is loaded with setpal 0, so the main menu's FrontendX palette stays in
   force, which is the palette the art is drawn in (tools/artbake.py). `out` holds at least
   128 + 2*w*h + 769 bytes. */
static unsigned pcx_encode(const unsigned char* px, int w, int h, unsigned char* out)
{
    unsigned n = 128;
    int x, y;
    memset(out, 0, 128);
    out[0] = 10;                                    /* ZSoft                                   */
    out[1] = 5;                                     /* version 3.0, a 256-colour palette       */
    out[2] = 1;                                     /* run-length coded                        */
    out[3] = 8;                                     /* bits per pixel                          */
    *(unsigned short*)(out + 8) = (unsigned short)(w - 1);
    *(unsigned short*)(out + 10) = (unsigned short)(h - 1);
    *(unsigned short*)(out + 12) = 72;
    *(unsigned short*)(out + 14) = 72;
    out[65] = 1;                                    /* planes                                  */
    *(unsigned short*)(out + 66) = (unsigned short)w;   /* bytes per line                      */
    *(unsigned short*)(out + 68) = 1;               /* a colour palette                        */
    for (y = 0; y < h; y++) {
        const unsigned char* r = px + (size_t)y * w;
        for (x = 0; x < w;) {
            int run = 1;
            while (x + run < w && run < 63 && r[x + run] == r[x]) run++;
            if (run > 1 || r[x] >= 0xC0) out[n++] = (unsigned char)(0xC0 | run);
            out[n++] = r[x];
            x += run;
        }
    }
    out[n++] = 12;                                  /* the palette follows                     */
    memset(out + n, 0, 768);
    return n + 768;
}

int tagpu_replays_files(TAGPU_UFO_FILE* f, int cap)
{
    unsigned char* px;
    int n = 0, s, k;

    px = (unsigned char*)malloc(SCR_W * SCR_H);
    if (!px) return 0;
    /* A style whose ground and font both fall back to retail's opens on retail's file; one whose
       ground is its own opens on it, set into its own layout's title bar. */
    for (s = 0; s < 3 && n < cap; s++) {
        const TAGPU_ART_ENTRY* g = tagpu_art_entry(STYLES[s], "ground");
        const TAGPU_ART_ENTRY* t = tagpu_art_entry(STYLES[s], "font");
        s_layout[s] = NULL;
        if (s > 0 && g && t && !strcmp(g->style, "retail") && !strcmp(t->style, "retail"))
            continue;
        if (!compose_ground(STYLES[s], STYLE_LAYOUT[s], px)) {
            rlog("replays: the %s style has no 640x480 ground or no title font", STYLES[s]);
            continue;
        }
        s_groundPcx[s] = (unsigned char*)malloc(128 + 2u * SCR_W * SCR_H + 769);
        if (!s_groundPcx[s]) continue;
        s_groundLen[s] = pcx_encode(px, SCR_W, SCR_H, s_groundPcx[s]);
        _snprintf(s_groundName[s], sizeof s_groundName[s], "IMPRPL%c", "RET"[s]);
        _snprintf(s_groundPath[s], sizeof s_groundPath[s], "bitmaps/%s.pcx", s_groundName[s]);
        s_layout[s] = STYLE_LAYOUT[s];
        f[n].path = s_groundPath[s];
        f[n].data = s_groundPcx[s];
        f[n].size = s_groundLen[s];
        n++;
    }
    free(px);
    if (!s_layout[0]) {
        rlog("replays: NOT armed - no retail ground");
        tagpu_replays_files_free();
        return 0;
    }
    for (s = 1; s < 3; s++)
        if (!s_layout[s]) {
            memcpy(s_groundName[s], s_groundName[0], sizeof s_groundName[s]);
            s_layout[s] = s_layout[0];
        }
    /* the screens the styles open, each once */
    for (k = 0; k < 2; k++) {
        int len, used = 0;
        for (s = 0; s < 3; s++) used |= s_layout[s] == LAYOUTS[k];
        if (!used) continue;
        len = n < cap ? build_gui(s_gui[k], sizeof s_gui[k], LAYOUTS[k]) : -1;
        if (len < 0) {
            rlog("replays: NOT armed - the generated %s does not fit", LAYOUTS[k]->screen);
            tagpu_replays_files_free();
            return 0;
        }
        f[n].path = LAYOUTS[k]->path;
        f[n].data = s_gui[k];
        f[n].size = (unsigned)len;
        n++;
    }
    return n;
}

void tagpu_replays_files_free(void)
{
    int s;
    for (s = 0; s < 3; s++) { free(s_groundPcx[s]); s_groundPcx[s] = NULL; }
}

int tagpu_replays_redirects(const char* name)
{
    int s;
    if (!s_armed || !name) return 0;
    for (s = 0; s < 3; s++)
        if (s_layout[s] && !lstrcmpiA(name, s_layout[s]->screen)) return 1;
    return 0;
}

/* ================================================================================================
   Gadget records
   ================================================================================================ */

static char* main_gi(void)
{
    char* m = *(char**)TA_MAIN;
    return m ? m + OFF_GUIINFO : NULL;
}

static char* top_ctrls(char* gi, char** top)
{
    char* t = gi ? *(char**)(gi + GI_TOP) : NULL;
    if (top) *top = t;
    return t ? *(char**)(t + GM_CTRLS) : NULL;
}

/* The record count, bounded by the block GUI_Load made: 0 when it is not a count. */
static int rec_count(const char* ctrls)
{
    int n = *(const short*)(ctrls + P_COUNT);
    return n >= 1 && n < GUI_RECORDS ? n : 0;
}

static int name_is(const char* rec, const char* name)
{
    return !strncmp(rec + G_NAME, name, 16);
}

static int find_rec(char* ctrls, const char* name)
{
    int n = rec_count(ctrls), i;
    for (i = 1; i <= n; i++)
        if (name_is(ctrls + (size_t)i * STRIDE, name)) return i;
    return 0;
}

static int at_pos(const char* r, int x, int y, int w, int h)
{
    return *(const short*)(r + G_XPOS) == x && *(const short*)(r + G_YPOS) == y &&
           *(const short*)(r + G_W) == w && *(const short*)(r + G_H) == h;
}

/* A button the screen's file declared and stage 1 has not resolved yet: nothing in it is a
   pointer, so a copy owns nothing, and the teardown frees nothing of an id-1 record whose +0xB4
   bit 0 is clear (0x4A9555..0x4A95C0). */
static int plain_button(const char* r)
{
    unsigned a;
    memcpy(&a, r + G_ATTRIBS, 4);
    return r[G_ID] == 1 && r[G_STAGES] == 0 && !(a & 0x1800) && !(r[G_OWNS] & 1) &&
           !*(const unsigned*)(r + G_FREE1) && !*(const unsigned*)(r + G_FREE2);
}

/* ================================================================================================
   The main menu
   ================================================================================================ */

static void bounded_copy(char* dst, unsigned cap, const char* src)
{
    unsigned k = 0;
    while (k + 1 < cap && src[k]) { dst[k] = src[k]; k++; }
    dst[k] = 0;
}

static int identify(char* ctrls, int n, char* version, unsigned vcap)
{
    char cfg[32], arc[32];
    int i, m;
    version[0] = 0;
    for (i = 1; i <= n; i++) {
        const char* r = ctrls + (size_t)i * STRIDE;
        if (r[G_ID] == 5 &&
            !strncmp(r + G_TEXT, TWILIGHT_PREFIX, sizeof TWILIGHT_PREFIX - 1)) {
            const char* v = r + G_TEXT + sizeof TWILIGHT_PREFIX - 1;
            unsigned left;
            while (*v == ' ' && v < r + G_TEXT + 0x7F) v++;
            left = (unsigned)(r + G_TEXT + 0x80 - v);        /* the field is 0x80 bytes */
            bounded_copy(version, vcap < left ? vcap : left, v);
            return MOD_TWILIGHT;
        }
    }
    bounded_copy(cfg, sizeof cfg, (const char*)VA_STR_CFG);
    bounded_copy(arc, sizeof arc, (const char*)VA_STR_ARCHIVE);
    for (m = MOD_RETAIL; m < MOD_N; m++) {
        if (s_mods[m].cfg && !lstrcmpiA(cfg, s_mods[m].cfg)) return m;
        if (s_mods[m].archive && !lstrcmpiA(arc, s_mods[m].archive)) return m;
    }
    if (!s_logged)
        rlog("replays: the mod is not in the table (config %s, archive %s): the retail placement",
             cfg, arc);
    return MOD_UNKNOWN;
}

/* Twilight's INTRO and EXIT move right to make the room REPLAYS takes; only when both are plain
   buttons where its file puts them, else neither. */
static int shift_right(char* ctrls)
{
    static const char* const NAMES[2] = { "INTRO", "EXIT" };
    static const short Y[2] = { 393, 430 };
    int idx[2], i;
    for (i = 0; i < 2; i++) {
        idx[i] = find_rec(ctrls, NAMES[i]);
        if (!idx[i] || !plain_button(ctrls + (size_t)idx[i] * STRIDE) ||
            !at_pos(ctrls + (size_t)idx[i] * STRIDE, SHIFT_FROM, Y[i], BTN_W, BTN_H))
            return 0;
    }
    for (i = 0; i < 2; i++)
        *(short*)(ctrls + (size_t)idx[i] * STRIDE + G_XPOS) = SHIFT_FROM + SHIFT_BY;
    return 1;
}

/* S1. The record is written straight into the block, never through the engine's own append
   0x4AB2B0, which tests `count == 200` and so writes one record past the block at 199
   (0x4AB2C1). BOUND: the write index is count + 1 <= 199, inside the 200-record block.
   LIFETIME: the record lives in the screen's own block, created and freed with it, and owns no
   allocation (plain_button). ORDERING: stage 1 runs after this, at 0x4AACC4 and again at
   0x426523, and treats it exactly as its sibling; the builder finds its gadgets by name, so one
   more at the end changes none of its lookups. */
static void mainmenu_visit(char* gi)
{
    char* ctrls = top_ctrls(gi, NULL);
    char version[0x80];
    const Mod* m;
    char* r;
    int n, sib, rx;

    s_appended = 0;
    if (!ctrls) return;
    n = rec_count(ctrls);
    if (n < 1 || n > GUI_RECORDS - 2) return;
    s_mod = identify(ctrls, n, version, sizeof version);
    m = &s_mods[s_mod];
    sib = find_rec(ctrls, SIB_NAME);
    if (!sib || !plain_button(ctrls + (size_t)sib * STRIDE) ||
        !at_pos(ctrls + (size_t)sib * STRIDE, SIB_X, SIB_Y, BTN_W, BTN_H)) {
        if (!s_logged) rlog("replays: no REPLAYS - the main menu's " SIB_NAME " is not the plain "
                            "%dx%d button at (%d,%d) the placement rests on", BTN_W, BTN_H,
                            SIB_X, SIB_Y);
        s_logged = 1;
        return;
    }
    rx = m->rx;
    s_movesOk = 1;
    if (m->shift_right && !shift_right(ctrls)) {
        s_movesOk = 0;
        rx = s_mods[MOD_UNKNOWN].rx;
    }
    if (m->label_to) {
        int i, any = 0;
        for (i = 1; i <= n; i++) {
            const char* l = ctrls + (size_t)i * STRIDE;
            if (name_is(l, "DebugString")) {
                if (l[G_ID] != 5 || !l[G_ACTIVE] || *(const short*)(l + G_XPOS) != m->label_x ||
                    *(const short*)(l + G_YPOS) != m->label_y)
                    s_movesOk = 0;
                any = 1;
            }
        }
        if (!any) s_movesOk = 0;
    }

    r = ctrls + (size_t)(n + 1) * STRIDE;
    memcpy(r, ctrls + (size_t)sib * STRIDE, STRIDE);
    r[G_ASSOC] = 0;
    memset(r + G_NAME, 0, 16);
    memcpy(r + G_NAME, "REPLAYS", 7);
    *(short*)(r + G_XPOS) = (short)rx;
    *(short*)(r + G_YPOS) = REPLAYS_Y;
    *(short*)(r + G_W) = BTN_W;
    *(short*)(r + G_H) = BTN_H;
    memset(r + G_HELP, 0, 128);         /* the sibling's describes Single Player */
    memset(r + G_TEXT, 0, 128);
    memcpy(r + G_TEXT, "REPLAYS", 7);
    r[G_QUICKKEY] = 'R';                /* no main menu of the six setups uses R */
    r[G_ACTIVE] = 1;
    *(unsigned short*)(r + G_GRAYED) &= (unsigned short)~1u;
    *(short*)(ctrls + P_COUNT) = (short)(n + 1);
    s_appended = 1;

    if (!s_logged) {
        rlog("replays: main menu: %s%s%s, style %s, REPLAYS at (%d,%d)%s%s", m->name,
             version[0] ? " " : "", version, m->style, rx, REPLAYS_Y,
             s_mods[s_mod].plate || !s_movesOk ? " on our plate" : "",
             s_movesOk ? "" : " - the entry's guard failed: the retail placement");
        s_logged = 1;
    }
}

#define RET_MAINMENU_LOAD 0x00426427u   /* the return address of the builder's one GUI_Load */
static const unsigned char S1_STOLEN[7] = { 0xC7, 0x46, 0x64, 0xFF, 0xFF, 0xFF, 0xFF };

/* At 0x4AACAE GUI_Load's frame is `sub esp,0x21C` and four pushes: [esp+0x22C] its return
   address, +0x230 gi, +0x234 the name, +0x238 the flags, which only the merge path (0x200)
   rewrites. */
static int __cdecl s1_before(void* esp)
{
    char* e = (char*)esp;
    if (*(unsigned*)(e + 0x22C) != RET_MAINMENU_LOAD || (*(unsigned*)(e + 0x238) & 0x200))
        return 0;
    mainmenu_visit(*(char**)(e + 0x230));
    return 0;
}

static unsigned fnv_rect(const unsigned char* px, int pitch, int x, int y, int w, int h)
{
    unsigned hs = 0x811C9DC5u;
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) hs = (hs ^ px[(size_t)(y + j) * pitch + x + i]) * 0x01000193u;
    return hs;
}

/* S3's edit of the FrontendX surface 0x429290 has just decoded: +0 w, +4 h, +8 pitch, +0xC the
   pixels (a w*h + 0x30 block). BOUND: every write is inside [0,w) x [0,h), with w, h and pitch
   read from the surface and checked. ORDERING: 0x429290 makes no copy of the surface (its calls
   are 0x4290F0, 0x4CAF30 and 0x4B6290) and the pointer escapes only on this return into 0x4288D0,
   so the GUI renderer's first copy of the asset reads the edited bytes, and the edit is not a draw
   it observes. The MRU at 0x5120B8 keeps the edited surface for later visits; a decode after an
   eviction is a fresh miss and is edited again. */
static void frontend_edit(unsigned char* s)
{
    const Mod* m = &s_mods[s_mod < 0 ? MOD_UNKNOWN : s_mod];
    int w = *(int*)(s + 0), h = *(int*)(s + 4), pitch = *(int*)(s + 8);
    unsigned char* px = *(unsigned char**)(s + 0xC);
    const TAGPU_ART_ENTRY* pe;
    int i, j;

    if (w != SCR_W || h != SCR_H || pitch < w || !px) {
        rlog("replays: the main menu's backdrop is %dx%d (pitch %d): no plate", w, h, pitch);
        return;
    }
    if (m->badge) {
        unsigned src = fnv_rect(px, pitch, BADGE_X, BADGE_Y, BADGE_W, BADGE_H);
        unsigned dst = fnv_rect(px, pitch, BADGE_X, BADGE_TO, BADGE_W, BADGE_H);
        s_badgeOk = src == BADGE_SRC_FNV && dst == BADGE_DST_FNV;
        if (s_badgeOk)
            for (j = 0; j < BADGE_H; j++)        /* y 392.. onto 338..383: no row overlaps */
                memcpy(px + (size_t)(BADGE_TO + j) * pitch + BADGE_X,
                       px + (size_t)(BADGE_Y + j) * pitch + BADGE_X, BADGE_W);
        else
            rlog("replays: %s's badge is not where the table has it (%08X, %08X): it stays",
                 m->name, src, dst);
    }
    if (!m->plate && s_movesOk) return;
    pe = tagpu_art_entry(mod_style(), "plate");
    if (!pe || pe->nframes != 1) { rlog("replays: no plate art"); return; }
    {
        const TAGPU_ART_FRAME* f = &pe->frames[0];
        for (j = 0; j < f->h; j++)
            for (i = 0; i < f->w; i++) {
                int x = PLATE_X + i, y = PLATE_Y + j;
                unsigned char v = f->pix[j * f->w + i];
                if (v != f->key && x < w && y < h) px[(size_t)y * pitch + x] = v;
            }
        /* the plate is ours, drawn for the palette: Classic++ leaves its box as drawn, and
           restores the mod's backdrop around it */
        tagpu_gui_asset_exact(px, PLATE_X, PLATE_Y,
                              PLATE_X + f->w <= w ? f->w : w - PLATE_X,
                              PLATE_Y + f->h <= h ? f->h : h - PLATE_Y);
    }
}

/* One of our own grounds (tagpu_replays_files) has just been decoded: all of it is ours. */
static void ground_exact(unsigned char* s)
{
    int w = *(int*)(s + 0), h = *(int*)(s + 4);
    unsigned char* px = *(unsigned char**)(s + 0xC);
    if (px && w > 0 && h > 0) tagpu_gui_asset_exact(px, 0, 0, w, h);
}

#define RET_BACKDROP_DECODE 0x004289E3u /* 0x4288D0's one call of 0x429290 returns here */
static const unsigned char S3_STOLEN[10] = { 0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00 };

/* WHICH DECODE THIS IS, by the name 0x4288D0 was handed: it passes its own argument on to the
   loader (0x4289D3, [esp+0x4C]), so FrontendX is the engine's literal and a ground of ours is our
   own string's address. Set by the filter, read by the return it gates, one call apart on the
   game thread. */
enum { S3_NONE, S3_FRONTEND, S3_GROUND };
static int s_s3Kind;

static int __cdecl s3_before(void* esp)
{
    void** e = (void**)esp;
    int k;
    s_s3Kind = S3_NONE;
    if ((unsigned)(size_t)e[0] != RET_BACKDROP_DECODE) return 0;
    if (s_appended && (unsigned)(size_t)e[1] == VA_STR_FRONTEND) s_s3Kind = S3_FRONTEND;
    for (k = 0; k < 3; k++)
        if (e[1] == (void*)s_groundName[k]) s_s3Kind = S3_GROUND;
    return s_s3Kind != S3_NONE;
}

/* The filter proved the return address, so it is handed back as the constant it is. */
static void* __cdecl s3_after(unsigned int* regs)
{
    unsigned char* s = (unsigned char*)(size_t)regs[7];
    if (s && s_s3Kind == S3_FRONTEND) frontend_edit(s);
    if (s && s_s3Kind == S3_GROUND) ground_exact(s);
    return (void*)(size_t)RET_BACKDROP_DECODE;
}

static void __stdcall mm_oncommand(void* gi);
static const unsigned char S2_STOLEN[7] = { 0x6A, 0x00, 0x68, 0xD4, 0x31, 0x50, 0x00 };

/* S2, at 0x42644C. The label moves wait for the pixel guard, which the decode just ahead decided,
   so a ProTA whose badge stayed keeps its label too; the builder's later write of DebugString
   (0x4265C9, 0x426615) is text and xpos only, never ypos. ORDERING for the slot: the builder
   writes +0x08 at 0x426432 and calls no pump before it returns, so the take precedes the first
   dispatch; it is re-taken on every build, and never chains to itself. */
static int __cdecl s2_before(void* esp)
{
    char* gi = main_gi();
    char* top;
    char* ctrls = top_ctrls(gi, &top);
    const Mod* m = &s_mods[s_mod < 0 ? MOD_UNKNOWN : s_mod];
    (void)esp;
    if (!s_appended || !ctrls) return 0;
    s_appended = 0;
    if (m->label_to && s_movesOk && (!m->badge || s_badgeOk == 1)) {
        int n = rec_count(ctrls), i;
        for (i = 1; i <= n; i++) {
            char* l = ctrls + (size_t)i * STRIDE;
            if (name_is(l, "DebugString") && *(short*)(l + G_YPOS) == m->label_y)
                *(short*)(l + G_YPOS) = m->label_to;
        }
    }
    if (*(void**)(top + GM_ONCMD) == (void*)VA_MM_ONCMD)
        *(void**)(top + GM_ONCMD) = (void*)mm_oncommand;
    return 0;
}

static void replays_open(char* gi);

/* MAINMENU's OnCommand while REPLAYS is on it: REPLAYS is ours, everything else -- the -1 call
   that frees the sparkle table included -- is the engine's. */
static void __stdcall mm_oncommand(void* gi)
{
    char* ctrls = top_ctrls((char*)gi, NULL);
    int idx = *(int*)((char*)gi + GI_UICHANGE);
    if (ctrls && idx >= 1 && idx <= rec_count(ctrls) &&
        name_is(ctrls + (size_t)idx * STRIDE, "REPLAYS")) {
        ((sound_fn)VA_SOUND)("BigButton", 0);
        replays_open((char*)gi);
        ((gi_fn)VA_ACTDONE)(gi);
        return;
    }
    ((oncmd_fn)VA_MM_ONCMD)(gi);
}

/* ================================================================================================
   The recordings and replays\index.dat
   ================================================================================================
   The index is a cache that can always be rebuilt, but for four marks it alone holds: the
   favourite flag, the last time a recording was played, its bookmarks and Watch's refusal. One
   line per recording, keyed by its file name and size:
     name size format start last end fav played map nseats seat... marks
       I<gen> version modprint has_modprint archives mod result,commander,defeat...
       L code modprint buildkey a b                                         (tab-separated)
   `played` is when Watch last started it, in seconds since 1970 (UTC), 0 for never. `marks` is the
   dock's bookmarks, comma-separated GameTimes inside [start, last], sorted; a line without the
   field, as this version's first lines were written, has none.
   From `I` on, what recr_info read: the version text, the archive fingerprint (hex) and whether
   the recording has one, the archives mounted, the mod named from them (mod_of), and each seat's
   result (RecResult), last commander death and defeat GameTimes, in the seats' order. `I` carries
   its generation (INFO_GEN); a line without `I`, or with another generation, keeps its marks and
   has the rest read again.
   From `L` on, the mark Watch's own check left (THE LOCK, below): the refusal's code
   (TAGPU_REPLAY_REFUSED_*), the running archive fingerprint and build key it was made under (hex),
   and the running values it compared (a decimal, b hex). A line without `L` has no mark.
   format 0 is a recording this build could not read. A recording whose line does not match is
   read again through recr_summary. The file is replaced whole, through a temporary file and a
   rename, so a crash leaves the old index or the new one. Lines for recordings the list does not
   hold are carried through unchanged while their file exists. */

typedef struct {
    char     file[FILE_MAX];
    unsigned long long size;
    FILETIME mtime;
    unsigned format, start, last, end, fav, played, nseats;
    char     map[RECR_TEXT];
    char     name[REC_PLAYERS][RECR_NAME];
    unsigned nmarks, marks[MARKS_MAX];
    int      info;                          /* the fields below were read (`I`)               */
    char     version[VERSION_MAX + 1];
    unsigned modprint, has_modprint, archives;
    char     mod[MOD_NAME_MAX];             /* an s_mods name                                 */
    unsigned char result[REC_PLAYERS];      /* RecResult, in the seats' order, as name[]      */
    unsigned cmdr[REC_PLAYERS], defeat[REC_PLAYERS];
    unsigned lock, lock_print, lock_build, lock_a, lock_b;     /* THE LOCK; lock 0: none   */
} Ent;

typedef struct {
    int  ent;
    int  at;                    /* its text in s_blob                                          */
    int  map_chars;             /* the text's characters up to the end of the map name         */
    char len[12];
} Row;

static Ent   s_ent[ENT_MAX];
static int   s_nent;
static int   s_listed[ENT_MAX];             /* the recordings in replays\, newest first, by entry */
static int   s_nlisted;
static Row   s_row[ENT_MAX];                /* those the filter shows                          */
static int   s_nrows;
/* THE MOD FILTER (the S5 note, "A recording from another mod"): the list holds the recordings of
   the running game's mod and version, mod_of's name and RT_GAME_VERSION's text, and All mods
   shows the rest. The archive fingerprint names an install, not a mod: it is a hint in the
   detail, never a filter. Taken when the browser opens. */
static int   s_all;
static char  s_runMod[MOD_NAME_MAX];
static char  s_runVersion[VERSION_MAX + 1];
static unsigned s_runPrint;
static unsigned s_runBuild;                 /* tagpu_replay_build_key, for THE LOCK            */
static int   s_watchReady;                  /* tagpu_replay_watch_ready at the open            */
static char  s_blob[ENT_MAX * ROW_TEXT];    /* LIFETIME: SetListText keeps this pointer and
                                               never frees it; static, it outlives every read */
static char  s_tlBlob[32];
static char* s_idxRaw;                      /* the index as read, for the lines carried        */
static unsigned long long s_disk;
static int   s_nfiles;

static void sanitize(char* s)
{
    for (; *s; s++) if (*s == '\t' || *s == '\r' || *s == '\n') *s = ' ';
}

static int ent_find(const char* file)
{
    int i;
    for (i = 0; i < s_nent; i++) if (!lstrcmpiA(s_ent[i].file, file)) return i;
    return -1;
}

static int uint_cmp(const void* a, const void* b)
{
    unsigned x = *(const unsigned*)a, y = *(const unsigned*)b;
    return x < y ? -1 : x > y;
}

/* `text`'s comma-separated GameTimes into e->marks: at most MARKS_MAX, each inside the line's
   [start, last], sorted; anything else is dropped. `text` is cut up. */
static void marks_parse(char* text, Ent* e)
{
    char* p = text;
    e->nmarks = 0;
    while (*p && e->nmarks < MARKS_MAX) {
        char* end;
        unsigned long v = strtoul(p, &end, 10);
        if (end != p && (*end == ',' || !*end) && v >= e->start && v <= e->last && e->last)
            e->marks[e->nmarks++] = (unsigned)v;
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    if (e->nmarks > 1) qsort(e->marks, e->nmarks, sizeof e->marks[0], uint_cmp);
}

/* The `I` group's fields from f[0] (`I` and its generation) on, `n` of them: how many it takes,
   0 when f[0] is not one. Read into `e` only when its generation is INFO_GEN. */
static int info_parse(char** f, int n, Ent* e)
{
    int i;
    char* end;
    unsigned long gen;
    if (n < 6 + (int)e->nseats || f[0][0] != 'I') return 0;
    gen = strtoul(f[0] + 1, &end, 10);
    if (*end) return 0;
    if (gen != INFO_GEN) return 6 + (int)e->nseats;
    lstrcpynA(e->version, f[1], sizeof e->version);
    e->modprint = strtoul(f[2], NULL, 16);
    e->has_modprint = strtoul(f[3], NULL, 10) ? 1 : 0;
    e->archives = strtoul(f[4], NULL, 10);
    lstrcpynA(e->mod, f[5], sizeof e->mod);
    for (i = 0; i < (int)e->nseats; i++) {
        unsigned r = 0, c = 0, d = 0;
        sscanf(f[6 + i], "%u,%u,%u", &r, &c, &d);
        e->result[i] = (unsigned char)(r <= REC_RESULT_LOST ? r : REC_RESULT_UNKNOWN);
        e->cmdr[i] = c;
        e->defeat[i] = d;
    }
    e->info = 1;
    return 6 + (int)e->nseats;
}

/* the `L` fields from f[0] (`L` itself) on, `n` of them */
static void lock_parse(char** f, int n, Ent* e)
{
    if (n < 6 || strcmp(f[0], "L")) return;
    e->lock = strtoul(f[1], NULL, 10);
    e->lock_print = strtoul(f[2], NULL, 16);
    e->lock_build = strtoul(f[3], NULL, 16);
    e->lock_a = strtoul(f[4], NULL, 10);
    e->lock_b = strtoul(f[5], NULL, 16);
}

/* One index line into `e`; 0 when it is not one. `line` is NUL-terminated and is cut up. */
static int ent_parse(char* line, Ent* e)
{
    char* f[11 + REC_PLAYERS + 6 + REC_PLAYERS + 6];
    int n = 0, i, k;
    char* p = line;
    memset(e, 0, sizeof *e);
    while (n < (int)(sizeof f / sizeof f[0])) {
        f[n++] = p;
        p = strchr(p, '\t');
        if (!p) break;
        *p++ = 0;
    }
    if (n < 10 || !f[0][0] || strlen(f[0]) >= FILE_MAX) return 0;
    lstrcpynA(e->file, f[0], FILE_MAX);
    e->size   = _strtoui64(f[1], NULL, 10);
    e->format = strtoul(f[2], NULL, 10);
    e->start  = strtoul(f[3], NULL, 10);
    e->last   = strtoul(f[4], NULL, 10);
    e->end    = strtoul(f[5], NULL, 10);
    e->fav    = strtoul(f[6], NULL, 10) ? 1 : 0;
    e->played = strtoul(f[7], NULL, 10);
    lstrcpynA(e->map, f[8], sizeof e->map);
    e->nseats = strtoul(f[9], NULL, 10);
    if (e->nseats > REC_PLAYERS || (int)e->nseats > n - 10) e->nseats = 0;
    for (i = 0; i < (int)e->nseats; i++) lstrcpynA(e->name[i], f[10 + i], RECR_NAME);
    if (n > 10 + (int)e->nseats) marks_parse(f[10 + e->nseats], e);
    k = 11 + (int)e->nseats;
    if (n > k) k += info_parse(f + k, n - k, e);
    if (n > k) lock_parse(f + k, n - k, e);
    return 1;
}

static void index_load(void)
{
    HANDLE h;
    LARGE_INTEGER sz;
    DWORD got = 0;
    char* copy;
    char* line;

    free(s_idxRaw);
    s_idxRaw = NULL;
    s_nent = 0;
    h = CreateFileA(INDEX_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > INDEX_CAP ||
        !(s_idxRaw = (char*)malloc((size_t)sz.QuadPart + 1)) ||
        !ReadFile(h, s_idxRaw, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart) {
        CloseHandle(h);
        free(s_idxRaw);
        s_idxRaw = NULL;
        return;
    }
    CloseHandle(h);
    s_idxRaw[got] = 0;
    if (strncmp(s_idxRaw, INDEX_MAGIC "\n", sizeof INDEX_MAGIC)) {
        rlog("replays: " INDEX_PATH " is not an index of this version: rebuilt");
        free(s_idxRaw);
        s_idxRaw = NULL;
        return;
    }
    copy = _strdup(s_idxRaw);
    if (!copy) return;
    for (line = strtok(copy + sizeof INDEX_MAGIC, "\n"); line && s_nent < ENT_MAX;
         line = strtok(NULL, "\n")) {
        size_t k = strlen(line);
        if (k && line[k - 1] == '\r') line[k - 1] = 0;
        if (ent_parse(line, &s_ent[s_nent]) && ent_find(s_ent[s_nent].file) < 0) s_nent++;
    }
    free(copy);
}

static int file_present(const char* file)
{
    char path[MAX_PATH];
    _snprintf(path, sizeof path, "replays\\%s", file);
    path[sizeof path - 1] = 0;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int put_ent(char* b, int cap, int at, const Ent* e)
{
    char map[RECR_TEXT], nm[RECR_NAME];
    unsigned i;
    lstrcpynA(map, e->map, sizeof map);
    sanitize(map);
    at = gput(b, cap, at, "%s\t%I64u\t%u\t%u\t%u\t%u\t%u\t%u\t%s\t%u", e->file, e->size,
              e->format, e->start, e->last, e->end, e->fav, e->played, map, e->nseats);
    for (i = 0; i < e->nseats; i++) {
        lstrcpynA(nm, e->name[i], sizeof nm);
        sanitize(nm);
        at = gput(b, cap, at, "\t%s", nm);
    }
    at = gput(b, cap, at, "\t");
    for (i = 0; i < e->nmarks; i++) at = gput(b, cap, at, i ? ",%u" : "%u", e->marks[i]);
    if (e->info) {
        char v[sizeof e->version];
        lstrcpynA(v, e->version, sizeof v);
        sanitize(v);
        at = gput(b, cap, at, "\tI%u\t%s\t%08X\t%u\t%u\t%s", INFO_GEN, v, e->modprint,
                  e->has_modprint, e->archives, e->mod);
        for (i = 0; i < e->nseats; i++)
            at = gput(b, cap, at, "\t%u,%u,%u", e->result[i], e->cmdr[i], e->defeat[i]);
    }
    if (e->lock)
        at = gput(b, cap, at, "\tL\t%u\t%08X\t%08X\t%u\t%08X", e->lock, e->lock_print,
                  e->lock_build, e->lock_a, e->lock_b);
    return gput(b, cap, at, "\n");
}

/* The whole index, written beside the old one and renamed over it. On the game thread, in the
   shell, where no simulation is waiting on it, or on a level that plays a recording (the dock's
   bookmarks): a playback is a skirmish, mode 2, with no peer, so the write can stall only the
   playback's own frame. The rule against a file write inside an OnCommand (tagpu_menu.c) is for a
   network game, which a playback never is. */
static void index_save(void)
{
    int cap = (int)INDEX_CAP, at, i;
    char* b = (char*)malloc((size_t)cap);
    HANDLE h;
    DWORD put = 0;
    BOOL ok;

    if (!b) return;
    at = gput(b, cap, 0, INDEX_MAGIC "\n");
    for (i = 0; i < s_nent; i++) at = put_ent(b, cap, at, &s_ent[i]);
    if (s_idxRaw) {
        char* p = strchr(s_idxRaw, '\n');
        while (p && *++p && at >= 0) {
            char* end = strchr(p, '\n');
            char* tab = strchr(p, '\t');
            size_t len = end ? (size_t)(end - p) + 1 : strlen(p);
            char file[FILE_MAX];
            if (tab && (!end || tab < end) && (size_t)(tab - p) < FILE_MAX) {
                memcpy(file, p, (size_t)(tab - p));
                file[tab - p] = 0;
                if (ent_find(file) < 0 && file_present(file) && at + (int)len + 1 < cap) {
                    memcpy(b + at, p, len);
                    at += (int)len;
                    if (!end) b[at++] = '\n';
                }
            }
            p = end;
        }
    }
    if (at < 0) {
        rlog("replays: the index does not fit in %u bytes: not written", INDEX_CAP);
        free(b);
        return;
    }
    h = CreateFileA(INDEX_TMP, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        rlog("replays: cannot write " INDEX_TMP " (%lu)", GetLastError());
        free(b);
        return;
    }
    ok = WriteFile(h, b, (DWORD)at, &put, NULL) && put == (DWORD)at && FlushFileBuffers(h);
    CloseHandle(h);
    free(b);
    if (!ok || !MoveFileExA(INDEX_TMP, INDEX_PATH, MOVEFILE_REPLACE_EXISTING |
                                                  MOVEFILE_WRITE_THROUGH)) {
        rlog("replays: cannot replace " INDEX_PATH " (%lu)", GetLastError());
        DeleteFileA(INDEX_TMP);
    }
}

static int ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : (unsigned char)c; }

/* `name` against a per-mod table pattern, case aside: `%s` stands for any run of characters */
static int pat_match(const char* pat, const char* name)
{
    for (; *pat; pat++, name++) {
        if (pat[0] == '%' && pat[1] == 's') {
            for (;; name++) {
                if (pat_match(pat + 2, name)) return 1;
                if (!*name) return 0;
            }
        }
        if (!*name || ascii_lower(*pat) != ascii_lower(*name)) return 0;
    }
    return !*name;
}

/* The table's entry whose archive pattern `name` matches, or NULL */
static const char* mod_by_archive(const char* name)
{
    int m;
    for (m = MOD_RETAIL; m < MOD_N; m++)
        if (s_mods[m].archive && pat_match(s_mods[m].archive, name)) return s_mods[m].name;
    return NULL;
}

/* The mod a recording was made in, from the archives it mounted, by the per-mod table's own
   archive patterns: the first archive that matches an entry's names it; none, retail. A mod the
   table does not know is retail by name, and told apart by its version text (row_shown). "" for
   a recording that names no archive, as one made before RT_ARCHIVE does not. */
static const char* mod_of(const RecInfo* in)
{
    unsigned a;
    const char* m;
    if (!in->narchives) return "";
    for (a = 0; a < in->narchives; a++)
        if ((m = mod_by_archive(in->archive[a].name)) != NULL) return m;
    return s_mods[MOD_RETAIL].name;
}

/* The running game's mod by mod_of's rule over the live mount list, as the recorder lists it for
   RT_ARCHIVE (tagpu_recorder.c, THE MOD): the first REC_ARCHIVES archives in mount order, each
   named without its folder, so a recording made now names the mod this does. The list is the
   graphics globals' [+0x618] with its count at +0x61C, which only InitTAHPIAry 0x41D4C0 changes,
   on the game thread as this runs. BOUND: the count, which the list's allocation always holds,
   capped at REC_ARCHIVES; a path read within its 0x104 bytes. */
static const char* running_mod(void)
{
    const char* gfx = *(const char* const*)VA_GFX_PP;
    const char* const* list;
    int n, i;
    if (!gfx || !(list = *(const char* const* const*)(gfx + GFX_ARC_LIST))) return s_mods[MOD_RETAIL].name;
    n = *(const int*)(gfx + GFX_ARC_COUNT);
    for (i = 0; i < n && i < (int)REC_ARCHIVES; i++) {
        const char* path = list[i] ? list[i] + ARC_PATH : NULL;
        char name[ARC_PATHMAX];
        const char* m;
        size_t k;
        if (!path) continue;
        k = strnlen(path, ARC_PATHMAX);
        if (k == ARC_PATHMAX) continue;               /* a buffer the mount left unwritten */
        while (k && path[k - 1] != '\\' && path[k - 1] != '/') k--;
        lstrcpynA(name, path + k, sizeof name);
        if ((m = mod_by_archive(name)) != NULL) return m;
    }
    return s_mods[MOD_RETAIL].name;
}

/* The main menu's version text as RT_GAME_VERSION takes it: printable, 1 to 7 characters, else
   "" (tagpu_recorder.c, the header). */
static void running_version(char* out, unsigned cap)
{
    const char* v = (const char*)VA_VERSION_TEXT;
    unsigned k = 0;
    while (k < 8 && v[k] >= 0x20 && v[k] < 0x7F) k++;
    if (!k || k >= 8 || v[k] || k >= cap) k = 0;
    memcpy(out, v, k);
    out[k] = 0;
}

/* THE ARCHIVES A LEVEL MOUNTS, before the running fingerprint is taken. InitTAHPIAry 0x41D4C0
   mounts at most ten new *.HPI per call (0x41D5C0, 0x41D5D9) and appends what it mounts, so the
   main menu of a fresh start holds fewer archives than a level, whose Start calls it again
   (0x47AF2E): MEASURED 2026-10-02 on retail, 28 at the first main menu against the 31 a recording
   names (totala3.hpi, totala4.hpi and worlds.hpi come with the second call). It is called here
   until a call mounts nothing: from then on every call, the Start's included, mounts nothing, so
   the fingerprint is the one a level records. The browser opens from the main menu's dispatch,
   from which MULTI calls it too (0x425E0B), and after Leave from menu_built, once the main menu's
   builder 0x4263B0 has returned at 0x4270A9; the engine itself calls it outside its shell menus,
   in play, at RESTART (0x460428) and in the load-game dialog (0x49257A). The stop test reads
   the count, and a call can prune one archive (0x4BE180) and mount another, which leaves the
   count unchanged: it may then stop early, with the fingerprint not yet a level's.
   BOUND: MOUNT_CALLS_MAX calls, 640 archives of the bounded kind. */
static void mount_all(void)
{
    const char* gfx = *(const char* const*)VA_GFX_PP;
    int i, before;
    if (!gfx) return;
    for (i = 0; i < MOUNT_CALLS_MAX; i++) {
        before = *(const int*)(gfx + GFX_ARC_COUNT);
        ((void (__cdecl*)(void))VA_INIT_HPI)();
        if (*(const int*)(gfx + GFX_ARC_COUNT) == before) return;
    }
}

/* RecInfo is about 71 KB, so one scratch serves every read: the index's and the detail's, all
   on the game thread (the header of this file), none of them inside another. */
static RecInfo s_info;

/* What recr_info reads of `e`'s file into its `I` fields. A file whose summary could not be read
   (format 0) keeps them empty until its size changes; one recr_info fails on is left without
   them, so the next open asks again. */
static void info_read(Ent* e)
{
    char path[MAX_PATH], why[160];
    unsigned i, k = 0;
    e->info = 1;
    e->version[0] = e->mod[0] = 0;
    e->modprint = e->has_modprint = e->archives = 0;
    memset(e->result, REC_RESULT_UNKNOWN, sizeof e->result);
    memset(e->cmdr, 0, sizeof e->cmdr);
    memset(e->defeat, 0, sizeof e->defeat);
    if (!e->format) return;
    _snprintf(path, sizeof path, "replays\\%s", e->file);
    path[sizeof path - 1] = 0;
    if (!recr_info(path, &s_info, why, sizeof why)) {
        rlog("replays: %s: %s", e->file, why);
        e->info = 0;
        return;
    }
    lstrcpynA(e->version, s_info.version, sizeof e->version);
    e->modprint = s_info.modprint;
    e->has_modprint = s_info.has_modprint ? 1 : 0;
    e->archives = s_info.archives;
    lstrcpynA(e->mod, mod_of(&s_info), sizeof e->mod);
    /* the header names its seats in record order (RT_PLAYER), the order of name[]: the k-th
       record present is the k-th seat. BOUND: k below the seats the summary read. */
    for (i = 0; i < REC_PLAYERS && k < e->nseats; i++) {
        if (!s_info.seat[i].present) continue;
        e->result[k] = s_info.seat[i].result <= REC_RESULT_LOST ? s_info.seat[i].result
                                                                 : REC_RESULT_UNKNOWN;
        e->cmdr[k] = s_info.seat[i].commander;
        e->defeat[k] = s_info.seat[i].defeat;
        k++;
    }
}

static void summarize(Ent* e)
{
    char path[MAX_PATH], why[160];
    RecSummary s;
    unsigned i;
    _snprintf(path, sizeof path, "replays\\%s", e->file);
    path[sizeof path - 1] = 0;
    e->format = e->start = e->last = e->end = e->nseats = e->nmarks = 0;
    e->lock = 0;                            /* the mark was the old content's */
    e->map[0] = 0;
    if (recr_summary(path, &s, why, sizeof why)) {
        e->format = s.format;
        e->start = s.start;
        e->last = s.last;
        e->end = s.end_reason;
        lstrcpynA(e->map, s.map, sizeof e->map);
        e->nseats = s.nseats;
        for (i = 0; i < s.nseats; i++) lstrcpynA(e->name[i], s.name[i], RECR_NAME);
    } else {
        /* the recorder names its files "YYYY-MM-DD HHMMSS <map>.imprec" (tagpu_recorder.c) */
        const char* p = strchr(e->file, ' ');
        char* dot;
        rlog("replays: %s: %s", e->file, why);
        p = p ? strchr(p + 1, ' ') : NULL;
        lstrcpynA(e->map, p ? p + 1 : e->file, sizeof e->map);
        dot = strrchr(e->map, '.');
        if (dot) *dot = 0;
    }
    info_read(e);
}

/* ---- the dock's bookmarks ---------------------------------------------------------------------
   On the game thread, on a level that plays a recording, where the browser is closed: its rows are
   rebuilt by scan() whenever it opens, so reading the index into s_ent here takes nothing from it. */

/* `file`'s line of `size` in the index as it is on disk, made from the recording when the index
   has none or has it for another size (its bookmarks then go: they were another file's); NULL
   when the index is full */
static Ent* mark_ent(const char* file, unsigned long long size)
{
    int k;
    index_load();
    k = ent_find(file);
    if (k < 0) {
        if (s_nent >= ENT_MAX) return NULL;
        k = s_nent++;
        memset(&s_ent[k], 0, sizeof s_ent[k]);
        lstrcpynA(s_ent[k].file, file, FILE_MAX);
        s_ent[k].size = ~0ull;
    }
    if (s_ent[k].size != size) {
        s_ent[k].size = size;
        summarize(&s_ent[k]);
    }
    return &s_ent[k];
}

static void mark_done(void)
{
    free(s_idxRaw);
    s_idxRaw = NULL;
}

int tagpu_replays_marks_get(const char* file, unsigned long long size, unsigned* out, int cap)
{
    int k, n = 0;
    index_load();
    k = ent_find(file);
    if (k >= 0 && s_ent[k].size == size)
        for (; n < cap && n < (int)s_ent[k].nmarks; n++) out[n] = s_ent[k].marks[n];
    mark_done();
    return n;
}

int tagpu_replays_mark_add(const char* file, unsigned long long size, unsigned T)
{
    Ent* e = strlen(file) < FILE_MAX ? mark_ent(file, size) : NULL;
    unsigned i, j;
    if (!e) {
        rlog("replays: bookmark at %u in %s refused: the index has no room for it", T, file);
        mark_done();
        return 0;
    }
    if (!e->format || T < e->start || T > e->last || e->nmarks >= MARKS_MAX) {
        rlog("replays: bookmark at %u in %s refused: %s", T, file,
             e->nmarks >= MARKS_MAX ? "it has 64 already" : "outside the recording");
        mark_done();
        return 0;
    }
    for (i = 0; i < e->nmarks; i++)
        if ((T > e->marks[i] ? T - e->marks[i] : e->marks[i] - T) < MARK_GAP) {
            rlog("replays: bookmark at %u in %s refused: one is at %u", T, file, e->marks[i]);
            mark_done();
            return 0;
        }
    for (i = 0; i < e->nmarks && e->marks[i] < T; i++) {}
    for (j = e->nmarks; j > i; j--) e->marks[j] = e->marks[j - 1];
    e->marks[i] = T;
    e->nmarks++;
    index_save();
    mark_done();
    rlog("replays: bookmark at %u in %s (%u now)", T, file, e->nmarks);
    return 1;
}

int tagpu_replays_mark_remove(const char* file, unsigned long long size, unsigned T)
{
    Ent* e = strlen(file) < FILE_MAX ? mark_ent(file, size) : NULL;
    unsigned i;
    if (e)
        for (i = 0; i < e->nmarks; i++)
            if (e->marks[i] == T) {
                memmove(&e->marks[i], &e->marks[i + 1], (e->nmarks - i - 1) * sizeof e->marks[0]);
                e->nmarks--;
                index_save();
                mark_done();
                rlog("replays: bookmark at %u in %s removed (%u left)", T, file, e->nmarks);
                return 1;
            }
    mark_done();
    return 0;
}

typedef struct { char file[FILE_MAX]; unsigned long long size; FILETIME mtime; } Found;

static int newest_first(const void* a, const void* b)
{
    const Found* x = (const Found*)a;
    const Found* y = (const Found*)b;
    int c = CompareFileTime(&y->mtime, &x->mtime);
    return c ? c : lstrcmpiA(y->file, x->file);
}

/* A row for each recording listed that the filter shows: with All mods every one, else those of
   the running game's mod and version (THE MOD FILTER). */
static int row_shown(const Ent* e)
{
    return s_all || (e->info && !strcmp(e->mod, s_runMod) && !strcmp(e->version, s_runVersion));
}

static void rows_pick(void)
{
    int i;
    s_nrows = 0;
    for (i = 0; i < s_nlisted; i++)
        if (row_shown(&s_ent[s_listed[i]])) s_row[s_nrows++].ent = s_listed[i];
}

/* THE LOCK (the S5 note, "A recording from another mod"). A recording Watch's own check refused
   for this game's data or this build (WATCH'S DATA REFUSALS, tagpu_replay.h) is marked in its
   index line: the refusal's code, the archive fingerprint (tagpu_rec_mod_print) and the build key
   (tagpu_replay_build_key) it was made under, and the running values it compared. While the mark
   holds, the row carries b-locked, the detail says why and Watch is grey, so the refusal is not
   asked for again.
   THE RULE that removes a mark: whenever the browser opens, a mark whose fingerprint or build key
   is not the running game's goes, since the data or the build it was refused under may have
   changed; and a mark goes with its line's content when the file's size changes (summarize).
   Nothing else removes one. The fingerprint is the mounted archives' names and sizes, so a change
   it does not see keeps the mark: a loose file in the game's folder, or an archive replaced by
   one of the same size. A refusal that is not a data refusal -- a missing map, a file being
   written, a file the reader cannot read, the engine's own notices -- never marks. */
/* THE RESTRICTIONS. A unit-types refusal follows from the install only when the level kept
   every type its load began with. The level's unit-data load 0x42D2E0 drops each type whose
   def+0x241 bit 23 is clear (0x42D4AD..0x42D510) and stores the count it kept (0x42D542). The
   loader that made the array dropped every type without the bit at its own end
   (0x42B248..0x42B2E8, its count stored at 0x42B2F6), so a type the level drops is one whose bit
   was cleared since: by the battleroom's restriction store (0x46E203, 0x46E23B, in 0x46E160),
   which is process state neither the fingerprint nor the build key sees. Nothing else writes the
   count between the two (its writers are 0x42AA77, 0x42B2F6, 0x42BD02 and 0x42D542; DISASSEMBLED).
   So the count at 0x42D2E0's entry, taken here, against the level's count Watch compared, says
   whether a restriction played a part. Weapons, features and the build are not touched by the
   store: the weapon table is filled from Weapons\*.tdf by 0x42E310 alone, and 0x46E160 writes
   only def +0x241 bit 23 and the cap +0x15A, neither of which a features or build check reads.
   ORDERING: written on the LOADER thread inside the level's load; read on the game thread at the
   level's first step, which follows the load's end. */
#define VA_UNIT_LOAD     0x0042D2E0u
#define OFF_UCOUNT       0x1438F        /* main: UNITINFOCount                                   */
static const unsigned char UNIT_LOAD_STOLEN[6] = { 0x81, 0xEC, 0x10, 0x06, 0x00, 0x00 };
static volatile LONG s_loadTypes = -1;  /* UNITINFOCount at the last load's entry, -1 unknown */

static int __cdecl unit_load_begin(void* esp)
{
    const char* ta = *(const char* const*)TA_MAIN;
    (void)esp;
    InterlockedExchange(&s_loadTypes, ta ? *(const LONG*)(ta + OFF_UCOUNT) : -1);
    return 0;
}

static int lock_holds(const Ent* e)
{
    return e->lock && e->lock_print == s_runPrint && e->lock_build == s_runBuild;
}

static const char* lock_text(const Ent* e)
{
    if (!e || !e->lock) return NULL;
    return e->lock >= TAGPU_REPLAY_REFUSED_LAYOUT ? "Made with another build of Impure."
                                                  : "Made with other unit data.";
}

/* The recordings listed: every one in replays\, newest first, the first ENT_MAX of them. */
static void scan(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    Found* found = NULL;
    int nf = 0, cap = 0, i, dirty = 0, kept = 0, gone;

    index_load();
    s_nlisted = 0;
    s_disk = 0;
    h = FindFirstFileA("replays\\*.imprec", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            unsigned long long size;
            char path[MAX_PATH];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            s_disk += size;
            if (strlen(fd.cFileName) >= FILE_MAX) continue;
            /* the recording the game is writing is not offered (tagpu_replay_being_written) */
            _snprintf(path, sizeof path, "replays\\%s", fd.cFileName);
            path[sizeof path - 1] = 0;
            if (tagpu_replay_being_written(path)) continue;
            if (nf == cap) {
                int c = cap ? cap * 2 : 256;
                Found* n = (Found*)realloc(found, (size_t)c * sizeof *n);
                if (!n) break;
                found = n;
                cap = c;
            }
            lstrcpynA(found[nf].file, fd.cFileName, FILE_MAX);
            found[nf].size = size;
            found[nf].mtime = fd.ftLastWriteTime;
            nf++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    s_nfiles = nf;
    if (nf > 1) qsort(found, (size_t)nf, sizeof *found, newest_first);
    if (nf > ENT_MAX) {
        rlog("replays: %d recordings; the browser lists the newest %d", nf, ENT_MAX);
        nf = ENT_MAX;
    }

    /* the entries of listed files first, in list order; the rest leave s_ent and are carried
       through by index_save while their file exists */
    for (i = 0; i < nf; i++) {
        int k = ent_find(found[i].file);
        if (k >= 0 && k != kept) { Ent t = s_ent[kept]; s_ent[kept] = s_ent[k]; s_ent[k] = t; }
        if (k >= 0) kept++;
    }
    gone = s_nent - kept;
    s_nent = kept;
    for (i = 0; i < nf; i++) {
        int k = ent_find(found[i].file);
        if (k < 0) {
            k = s_nent++;
            memset(&s_ent[k], 0, sizeof s_ent[k]);
            lstrcpynA(s_ent[k].file, found[i].file, FILE_MAX);
            s_ent[k].size = ~0ull;
        }
        s_ent[k].mtime = found[i].mtime;
        if (s_ent[k].size != found[i].size) {
            s_ent[k].size = found[i].size;
            summarize(&s_ent[k]);
            dirty = 1;
        } else if (!s_ent[k].info) {
            info_read(&s_ent[k]);
            dirty |= s_ent[k].info;
        }
        if (s_ent[k].lock && !lock_holds(&s_ent[k])) {
            rlog("replays: %s: the mark of refusal %u goes - made under archives %08X and build "
                 "key %08X, running %08X and %08X", s_ent[k].file, s_ent[k].lock,
                 s_ent[k].lock_print, s_ent[k].lock_build, s_runPrint, s_runBuild);
            s_ent[k].lock = 0;
            dirty = 1;
        }
        s_listed[s_nlisted++] = k;
    }
    free(found);
    if (dirty || gone) index_save();
    rows_pick();
}

/* ================================================================================================
   The browser
   ================================================================================================ */

/* The text state the list draws with, read as the list draw reads it (0x4A1C0D): a GAF font,
   else the .FNT face. */
static const void* gaf_font(void)
{
    const char* st = *(const char* const*)VA_FONTSTATE;
    const char* f = st ? *(const char* const*)(st + 0x14) : NULL;
    return f ? *(const void* const*)(f + 0x0C) : NULL;
}

static int text_w(const char* t)
{
    const void* font = gaf_font();
    int w = 0;
    if (!*(const void* const*)VA_FONTSTATE) return 0;
    if (!font) return ((fnt_width_fn)VA_FNT_WIDTH)(((fnt_font_fn)VA_FNT_FONT)(), t);
    for (; *t; t++) {
        const unsigned short* g = ((glyph_fn)VA_GLYPH)(font, (unsigned char)*t);
        if (g) w += g[0];
    }
    return w;
}

/* one line of the font in force: the 'I' glyph's height, or the .FNT face's */
static int line_h(void)
{
    const void* font = gaf_font();
    const unsigned short* g;
    if (!*(const void* const*)VA_FONTSTATE) return 0;
    if (!font) return ((fnt_height_fn)VA_FNT_HEIGHT)();
    g = ((glyph_fn)VA_GLYPH)(font, 'I');
    return g ? g[1] : 0;
}

/* the 'I' height + 2 the row loop stops on (0x4A1C21, 0x4A1C1A) */
static int row_floor(void)
{
    const void* font = gaf_font();
    const unsigned short* g;
    if (!*(const void* const*)VA_FONTSTATE) return 0;
    if (!font) return ((fnt_height_fn)VA_FNT_HEIGHT)();
    g = ((glyph_fn)VA_GLYPH)(font, 'I');
    return g ? g[1] + 2 : 0;
}

/* The badges, as engine GAF frame headers over our art's pixels (tagpu_art.h: never freed or
   written): +0 w, +2 h, +8 the key, +9 0 = raw, +0xC TAGPU_GAF_EXACT, +0x10 the pixels. */
enum { B_FAV, B_FAV_OFF, B_INCOMPLETE, B_LOCKED, B_N };
static const char* const BADGE_NAMES[B_N] = { "b-fav", "b-fav-off", "b-incomplete", "b-locked" };
static unsigned char s_badge[B_N][0x18];
static int s_badgeW[B_N], s_badgeH[B_N], s_badgeIn[B_N];

static void badges_build(void)
{
    int i;
    for (i = 0; i < B_N; i++) {
        const TAGPU_ART_ENTRY* e = tagpu_art_entry(mod_style(), BADGE_NAMES[i]);
        const TAGPU_ART_FRAME* f = e && e->nframes == 1 ? &e->frames[0] : NULL;
        const void* pix = f ? f->pix : NULL;
        memset(s_badge[i], 0, 0x18);
        s_badgeIn[i] = f != NULL;
        if (!f) continue;
        *(unsigned short*)(s_badge[i] + 0x00) = f->w;
        *(unsigned short*)(s_badge[i] + 0x02) = f->h;
        s_badge[i][0x08] = f->key;
        {
            unsigned tag = TAGPU_GAF_EXACT;
            memcpy(s_badge[i] + TAGPU_GF_TAG, &tag, 4);
        }
        memcpy(s_badge[i] + 0x10, &pix, 4);
        s_badgeW[i] = f->w;
        s_badgeH[i] = f->h;
    }
}

static void length_text(const Ent* e, char* out, unsigned cap)
{
    unsigned s = e->format && e->last > e->start ? (e->last - e->start) / 30 : 0;
    if (!e->format) _snprintf(out, cap, "?");
    else if (s >= 3600) _snprintf(out, cap, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else _snprintf(out, cap, "%u:%02u", s / 60, s % 60);
    out[cap - 1] = 0;
}

static int incomplete(const Ent* e)
{
    return e->format && e->end != REC_END_TEARDOWN;
}

/* Each row's text: room for the star, the map, room for its badges, then the players, cut to end
   before the length's column. The blob holds exactly s_nrows NUL-terminated rows: the engine's
   row walk 0x4B6AF0 has no bound of its own, and this is what keeps it inside the blob. */
static void rows_build(void)
{
    int sp = text_w(" "), star = 0, i, at = 0;
    if (sp <= 0) sp = 4;
    if (s_badgeIn[B_FAV]) star = (s_badgeW[B_FAV] + STAR_GAP + sp - 1) / sp;
    for (i = 0; i < s_nrows; i++) {
        Row* r = &s_row[i];
        const Ent* e = &s_ent[r->ent];
        char t[ROW_TEXT * 2];
        int k = 0, j, room, bw = 0;
        unsigned p;
        length_text(e, r->len, sizeof r->len);
        for (j = 0; j < star && k < ROW_TEXT - 1; j++) t[k++] = ' ';
        for (j = 0; e->map[j] && k < ROW_TEXT - 1; j++) t[k++] = e->map[j];
        r->map_chars = k;
        if (incomplete(e) && s_badgeIn[B_INCOMPLETE]) bw += BADGE_GAP + s_badgeW[B_INCOMPLETE];
        if (e->lock && s_badgeIn[B_LOCKED]) bw += BADGE_GAP + s_badgeW[B_LOCKED];
        for (j = 0; j < (bw + sp - 1) / sp && k < ROW_TEXT - 1; j++) t[k++] = ' ';
        for (p = 0; p < e->nseats && k < ROW_TEXT - 3; p++) {
            const char* n = e->name[p];
            t[k++] = p ? ',' : ' ';
            t[k++] = ' ';
            for (; *n && k < ROW_TEXT - 1; n++) t[k++] = *n;
        }
        t[k] = 0;
        /* cut at the length's column, the length right-aligned 3 px in from the list's edge */
        room = s_cur->list.w - 4 - text_w(r->len) - 8;
        while (k > 0 && text_w(t) > room) t[--k] = 0;
        if (at + k + 1 > (int)sizeof s_blob) break;
        if (r->map_chars > k) r->map_chars = k;
        r->at = at;
        memcpy(s_blob + at, t, (size_t)k + 1);
        at += k + 1;
    }
    if (i < s_nrows) s_nrows = i;
    if (!s_nrows) s_blob[0] = 0;
}

static void set_label(char* gi, const char* name, const char* fmt, ...)
{
    char b[0x80];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    ((set_label_fn)VA_SETLABEL)(gi, name, b);
}

/* "name: Won", "name: Lost" or the name alone, the name cut until it fits a column; a cut name
   never ends on a space */
static void player_text(char* t, unsigned cap, const char* name, unsigned result)
{
    const char* r = result == REC_RESULT_WON ? ": Won" : result == REC_RESULT_LOST ? ": Lost" : "";
    char n[RECR_NAME];
    int k, full;
    lstrcpynA(n, name, sizeof n);
    full = k = lstrlenA(n);
    for (;;) {
        while (k && k < full && n[k - 1] == ' ') n[--k] = 0;
        _snprintf(t, cap, "%s%s", n, r);
        t[cap - 1] = 0;
        if (!k || text_w(t) <= s_cur->pw) return;
        n[--k] = 0;
    }
}

/* THE MINIMAP. The id-6 surface gadget's draw 0x4A4980 stamps a frame at its +0xC2 with
   GAF_DrawTransformed 0x4C7580 over the record's rect less its last column and row, from the
   frame's texels [1, w - 1) x [1, h - 1) (0x4A4ABD..0x4A4B1B; measured on SKIRMISH's swatches,
   tagpu_gui_leaves.h, before_scale), so a frame two texels wider and higher than the box, on a
   record one pixel wider and higher, is stamped 1:1 over exactly the box. Without a frame the
   gadget fills its rect with the colour at gi+0x8B9 (0x4A4B2A): the frame takes that colour
   around the map, and everywhere when there is no map, as a recording without RB_MINIMAP has.
   The map is the recorded picture's part [0, 2·cols) x [0, 2·rows), the part the engine shows,
   fitted as the engine fits the level's minimap (0x466780, exe-reverse-engineering.md): the long
   axis takes the box, the short one its share, truncated, and the result is centred; sampled
   nearest, a texel per pixel by truncating division.
   LIFETIME: the frame and its texels are this file's statics, and nothing frees them: the
   teardown's stage 2 frees a gadget's +0x2B when its +0xB4 bit 0 is set, a panel's +0xC0 and the
   +0xD6 of ids 7 and 8, and nothing else (0x4A9562..0x4A95C0); an id-6 frame is its screen's own
   to free, as SKIRMISH frees its preview's itself (0x47A94A..0x47A95D). */
#define G_FRAME   0xC2              /* id 6: the frame the surface gadget stamps              */
#define GI_FILL   0x8B9             /* the colour it fills with when it has no frame          */
#define MM_BOX    96                /* a layout's minimap box at most, each side              */
static unsigned char s_mmFrame[0x18];
static unsigned char s_mmPix[(MM_BOX + 2) * (MM_BOX + 2)];

/* `in`'s minimap into the frame, or the box's colour alone; 1 when there was a map */
static int minimap_build(const char* gi, const RecInfo* in)
{
    int W = s_cur->minimap.w, H = s_cur->minimap.h, fw = W + 2, fh = H + 2;
    unsigned sw, sh;
    int dw, dh, ox, oy, x, y;
    const void* pix = s_mmPix;
    memset(s_mmPix, (unsigned char)gi[GI_FILL], sizeof s_mmPix);
    memset(s_mmFrame, 0, sizeof s_mmFrame);
    *(unsigned short*)(s_mmFrame + 0x00) = (unsigned short)fw;
    *(unsigned short*)(s_mmFrame + 0x02) = (unsigned short)fh;
    memcpy(s_mmFrame + 0x10, &pix, sizeof pix);
    if (!in || !in->mm_w || !in->mm_h) return 0;
    /* BOUND: the shown part within the picture, and the picture within RecInfo's plane
       (recr_info keeps only w x h <= REC_MINIMAP_MAX) */
    sw = 2 * in->mm_cols < in->mm_w ? 2 * in->mm_cols : in->mm_w;
    sh = 2 * in->mm_rows < in->mm_h ? 2 * in->mm_rows : in->mm_h;
    if (!sw || !sh) return 0;
    if (sw * (unsigned)H >= sh * (unsigned)W) {
        dw = W;
        dh = (int)(sh * (unsigned)W / sw);
    } else {
        dh = H;
        dw = (int)(sw * (unsigned)H / sh);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    ox = (W - dw) / 2;
    oy = (H - dh) / 2;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++)
            s_mmPix[(1 + oy + y) * fw + 1 + ox + x] =
                in->minimap[(y * sh / (unsigned)dh) * in->mm_w + x * sw / (unsigned)dw];
    return 1;
}

static void minimap_show(char* gi, const Ent* e)
{
    char* ctrls = top_ctrls(gi, NULL);
    int k = ctrls ? find_rec(ctrls, "MINIMAP") : 0;
    const RecInfo* in = NULL;
    char path[MAX_PATH], why[160];
    if (!k || ctrls[(size_t)k * STRIDE + G_ID] != 6 || s_cur->minimap.w > MM_BOX ||
        s_cur->minimap.h > MM_BOX)
        return;
    if (e && e->format) {
        _snprintf(path, sizeof path, "replays\\%s", e->file);
        path[sizeof path - 1] = 0;
        if (recr_info(path, &s_info, why, sizeof why)) in = &s_info;
        else rlog("replays: %s: %s", e->file, why);
    }
    minimap_build(gi, in);
    *(void**)(ctrls + (size_t)k * STRIDE + G_FRAME) = s_mmFrame;
}

/* The selected recording's mod and version, and under it THE LOCK's reason, or else whether the
   archives this game mounted are the ones it was recorded with: a hint, since a map installed
   since changes them too. */
static void mod_lines(char* gi, const Ent* e)
{
    int m;
    const char* title = "";
    const char* hint = lock_text(e);
    int named = e && e->info && e->mod[0];
    if (!hint)
        hint = named && e->has_modprint && e->modprint != s_runPrint
               ? "The installed archives differ." : "";
    set_label(gi, "MODHINT", "%s", hint);
    if (!named) {
        set_label(gi, "MODLINE", "");
        return;
    }
    for (m = MOD_RETAIL; m < MOD_N; m++)
        if (!strcmp(e->mod, s_mods[m].name)) title = s_mods[m].title;
    set_label(gi, "MODLINE", "%s %s", title, e->version);
}

static char* recs_rec(char* gi, char** ctrls_out);

/* Watch grey when it cannot start anything, or when the selected recording is marked */
static void watch_grey(char* gi, const Ent* e)
{
    char* ctrls = NULL;
    int w;
    unsigned short* g;
    if (!recs_rec(gi, &ctrls) || !(w = find_rec(ctrls, "WATCH"))) return;
    g = (unsigned short*)(ctrls + (size_t)w * STRIDE + G_GRAYED);
    if (!s_watchReady || (e && e->lock)) *g |= 1;
    else *g &= (unsigned short)~1u;
}

/* The right-hand well for row `sel`, or empty. */
static void detail(char* gi, int sel)
{
    const Ent* e = sel >= 0 && sel < s_nrows ? &s_ent[s_row[sel].ent] : NULL;
    char nm[4];
    int i;
    if (e && !e->format) set_label(gi, "MAPNAME", "%s (unreadable)", e->map);
    else set_label(gi, "MAPNAME", "%s", e ? e->map : "");
    for (i = 0; i < REC_PLAYERS; i++) {
        char t[RECR_NAME + 8];
        _snprintf(nm, sizeof nm, "P%d", i);
        nm[sizeof nm - 1] = 0;
        if (e && i < (int)e->nseats)
            player_text(t, sizeof t, e->name[i], e->info ? e->result[i] : REC_RESULT_UNKNOWN);
        else t[0] = 0;
        set_label(gi, nm, "%s", t);
    }
    lstrcpynA(s_tlBlob, "Main timeline", sizeof s_tlBlob);
    ((set_list_fn)VA_SETLIST)(gi, "TIMELINES", s_tlBlob, e && e->format ? 1 : 0, NULL);
    mod_lines(gi, e);
    watch_grey(gi, e);
    minimap_show(gi, e);
    ((gi_fn)VA_SETDIRTY)(gi);
}

static void __stdcall rp_oncommand(void* gi);

/* A bar's label, its text centred in the bar: the label draw puts a line's top at the record's y
   (0x4A5A7B: 0x4A50E0 handed the record's top), so the record is moved to centre one line of the
   font in force and made one line high, under the two lines past which it would wrap (0x4A5A36).
   The draw reads the record at every repaint (0x4A5829), so the move holds from the first. */
static void bar_label(char* ctrls, const char* name, const Rect* bar)
{
    int k = find_rec(ctrls, name), lh = line_h();
    char* r;
    if (!k || lh <= 0 || lh > bar->h) return;
    r = ctrls + (size_t)k * STRIDE;
    *(short*)(r + G_YPOS) = (short)(bar->y + (bar->h - lh) / 2);
    *(short*)(r + G_H) = (short)lh;
}

/* The recording list's selection callback: the detail follows the selection however it moved. */
static void __stdcall recs_selected(void* gi, void* rec)
{
    detail((char*)gi, *(const short*)((const char*)rec + L_SELECTED));
}

/* The rows the filter shows into the list, the count, the toggle's caption and the detail of the
   first row; SetListText puts the selection and the top back to row 0 (0x4A33C7, 0x4A33CE). */
static void rows_show(char* gi)
{
    rows_build();
    ((set_list_fn)VA_SETLIST)(gi, "RECS", s_blob, s_nrows, NULL);
    if (s_nrows == s_nfiles) set_label(gi, "COUNT", s_nfiles == 1 ? "1 recording" : "%d recordings",
                                       s_nfiles);
    else set_label(gi, "COUNT", "%d of %d recordings", s_nrows, s_nfiles);
    set_label(gi, "ALLMODS", "%s", s_all ? ALL_OFF : ALL_ON);
    detail(gi, s_nrows ? 0 : -1);
}

/* Pushed from inside the main menu's dispatch, exactly as the Load Game screen is pushed from
   SINGLE's (0x4931D0): GUI_Load, the OnCommand and context, the backdrop with no flip and no
   palette (the main menu's stays in force, and the art is drawn in it), then the rows. */
static void replays_open(char* gi)
{
    const Layout* L = s_layout[style_index(mod_style())];
    void* gm;
    char* ctrls;
    double mb;

    if (!L) return;
    gm = ((gui_load_fn)VA_GUI_LOAD)(gi, L->screen, GUI_FLAGS);
    if (!gm) { rlog("replays: GUI_Load returned no %s", L->screen); return; }
    s_cur = L;
    *(void**)((char*)gm + GM_ONCMD) = (void*)rp_oncommand;
    *(void**)((char*)gm + GM_CTX) = NULL;
    ((gi_int_fn)VA_TAKESKEYS)(gi, 1);           /* as LOADGAME does, 0x4932E3 */
    ((backdrop_fn)VA_BACKDROP)(s_groundName[style_index(mod_style())], 0, 0, 0);
    ctrls = top_ctrls(gi, NULL);
    s_gm = gm;
    s_recsIdx = ctrls ? find_rec(ctrls, "RECS") : 0;
    if (s_recsIdx)
        *(list_cb_fn*)(ctrls + (size_t)s_recsIdx * STRIDE + L_CALLBACK) = recs_selected;
    {
        char why[200];
        s_watchReady = tagpu_replay_watch_ready(why, sizeof why);
        if (!s_watchReady) rlog("replays: Watch is grey: %s", why);
    }
    if (ctrls) {
        bar_label(ctrls, "COUNT", &L->count);
        bar_label(ctrls, "FOOTER", &L->footer);
    }

    badges_build();
    mount_all();
    lstrcpynA(s_runMod, running_mod(), sizeof s_runMod);
    running_version(s_runVersion, sizeof s_runVersion);
    s_runPrint = tagpu_rec_mod_print();
    s_runBuild = tagpu_replay_build_key();
    scan();
    rows_show(gi);
    mb = (double)s_disk / (1024.0 * 1024.0);
    set_label(gi, "FOOTER", mb < 10.0 ? "Replays use %.1f MB of disk." : "Replays use %.0f MB of disk.", mb);
    rlog("replays: opened - %d of %d recordings listed (%s), %I64u bytes; the running game %s %s, "
         "archives %08X", s_nrows, s_nfiles, s_all ? "all mods" : "this mod", s_disk, s_runMod,
         s_runVersion, s_runPrint);
}

/* The list record of ours on top, or NULL. */
static char* recs_rec(char* gi, char** ctrls_out)
{
    char* top;
    char* ctrls = top_ctrls(gi, &top);
    char* r;
    if (!ctrls || !s_gm || top != (char*)s_gm || !name_is(ctrls, s_cur->screen) || s_recsIdx < 1 ||
        s_recsIdx > rec_count(ctrls))
        return NULL;
    r = ctrls + (size_t)s_recsIdx * STRIDE;
    if (r[G_ID] != 2 || !name_is(r, "RECS")) return NULL;
    if (ctrls_out) *ctrls_out = ctrls;
    return r;
}

static void fav_toggle(char* gi, int sel)
{
    if (sel < 0 || sel >= s_nrows) return;
    s_ent[s_row[sel].ent].fav ^= 1;
    index_save();
    ((list_draw_fn)VA_LISTDRAW)(gi, s_recsIdx);
}

/* The star's column, on the selected row: the point the click was aimed at, which the pump
   hit-tests the screen with (gi+0x3C/+0x40, 0x4AA0E6) and refreshes before every dispatch. Only
   a click actuates the list: Up and Down call its callback and return without actuating
   (0x4A9DFA -> 0x4A9E66), so the point is always the click that brought us here. */
static int on_star(char* gi, const char* r, int sel)
{
    int x = *(int*)(gi + GI_CLICKX), y = *(int*)(gi + GI_CLICKY);
    int lx = *(const short*)(r + G_XPOS), ly = *(const short*)(r + G_YPOS);
    int top = *(const short*)(r + L_TOP), ih = *(const short*)(r + L_ITEMH);
    int ry = ly + 2 + (sel - top) * ih;
    return s_badgeIn[B_FAV] && ih > 0 && x >= lx + 2 && x < lx + 2 + s_badgeW[B_FAV] + 2 &&
           y >= ry && y < ry + ih;
}

/* The main menu is built again after a level the browser started (tagpu_replay_watch's `back`):
   the browser opens over it as a click on REPLAYS opens it, when the menu on top is ours. */
static void replays_back(void* gi)
{
    char* top;
    if (top_ctrls((char*)gi, &top) && *(void**)(top + GM_ONCMD) == (void*)mm_oncommand)
        replays_open((char*)gi);
}

/* Watch: the selected recording through the replay's own Start. Its time goes into the index
   first: a Start that commits pops this screen, whose destructor frees the index's carried lines,
   so nothing is saved after it; a refused one puts the old time back. 1 when it committed. */
static int watch(char* gi)
{
    char* l = recs_rec(gi, NULL);
    int sel = l ? *(const short*)(l + L_SELECTED) : -1;
    char file[FILE_MAX];
    Ent* e;
    unsigned was;
    if (sel < 0 || sel >= s_nrows) return 0;
    e = &s_ent[s_row[sel].ent];
    lstrcpynA(file, e->file, sizeof file);
    if (e->lock) {          /* Watch is grey on it: THE LOCK */
        rlog("replays: Watch %s not asked: %s", file, lock_text(e));
        return 0;
    }
    was = e->played;
    e->played = (unsigned)time(NULL);
    index_save();
    rlog("replays: Watch %s", file);
    if (tagpu_replay_watch(gi, file, -1, replays_back)) return 1;
    e->played = was;
    index_save();
    return 0;
}

/* The rows again with the selection and the top kept, for a mark made while the screen is on
   top. BOUND: both are put back only inside the rows the list now holds. */
static void rows_refresh(char* gi)
{
    char* l = recs_rec(gi, NULL);
    short sel, top;
    if (!l) return;
    sel = *(const short*)(l + L_SELECTED);
    top = *(const short*)(l + L_TOP);
    rows_build();
    ((set_list_fn)VA_SETLIST)(gi, "RECS", s_blob, s_nrows, NULL);
    l = recs_rec(gi, NULL);
    if (!l) return;
    if (sel >= 0 && sel < s_nrows) *(short*)(l + L_SELECTED) = sel;
    if (top >= 0 && top < s_nrows) *(short*)(l + L_TOP) = top;
    detail(gi, *(const short*)(l + L_SELECTED));
}

/* THE LOCK's mark, from Watch's checks (tagpu_replays.h). The browser's own entries are marked
   while its screen exists, the screen's rows redrawn when it is on top, as it is at Watch's
   header check, before the notice; otherwise the line on disk, as the dock's bookmarks are
   written. */
void tagpu_replays_refused(const char* file, unsigned code, unsigned a, unsigned b)
{
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    unsigned long long size;
    char* gi = main_gi();
    Ent* e = NULL;
    int k;
    if (strlen(file) >= FILE_MAX) return;
    if (code == TAGPU_REPLAY_REFUSED_UNITS) {
        LONG began = InterlockedCompareExchange(&s_loadTypes, 0, 0);
        if (began < 0 || (unsigned)began != a) {
            rlog("replays: %s not marked for refusal %u: the level kept %u unit types of the %ld "
                 "its load began with (THE RESTRICTIONS)", file, code, a, (long)began);
            return;
        }
    }
    _snprintf(path, sizeof path, "replays\\%s", file);
    path[sizeof path - 1] = 0;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return;
    size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (s_gm) {
        k = ent_find(file);
        if (k >= 0 && s_ent[k].size == size) e = &s_ent[k];
    } else {
        e = mark_ent(file, size);
    }
    if (e) {
        e->lock = code;
        e->lock_print = tagpu_rec_mod_print();
        e->lock_build = tagpu_replay_build_key();
        e->lock_a = a;
        e->lock_b = b;
        index_save();
        rlog("replays: %s marked: refusal %u (%u, %08X) under archives %08X, build key %08X",
             file, code, a, b, e->lock_print, e->lock_build);
    } else {
        rlog("replays: %s not marked for refusal %u: the index has no line for it", file, code);
    }
    if (!s_gm) mark_done();
    else if (e && gi && recs_rec(gi, NULL)) rows_refresh(gi);
}

/* REPLAYS' OnCommand. Back is the one click left unanswered: the pump then calls this with -1,
   pops the screen and repaints the main menu (0x4AA7BC..0x4AA7FA). */
static void __stdcall rp_oncommand(void* gi)
{
    char* g = (char*)gi;
    char* ctrls = top_ctrls(g, NULL);
    int idx = *(int*)(g + GI_UICHANGE);
    const char* r;

    if (idx < 0) {          /* the screen's destructor; the pump frees it next */
        s_gm = NULL;
        free(s_idxRaw);
        s_idxRaw = NULL;
        return;
    }
    if (!ctrls || idx < 1 || idx > rec_count(ctrls)) { ((gi_fn)VA_ACTDONE)(gi); return; }
    r = ctrls + (size_t)idx * STRIDE;
    if (name_is(r, "BACK")) return;
    if (name_is(r, "RECS")) {
        int sel = *(const short*)(r + L_SELECTED);
        if (on_star(g, r, sel)) fav_toggle(g, sel);
    } else if (name_is(r, "FAV")) {
        char* l = recs_rec(g, NULL);
        if (l) fav_toggle(g, *(const short*)(l + L_SELECTED));
    } else if (name_is(r, "ALLMODS")) {
        s_all ^= 1;
        rows_pick();
        rows_show(g);
    } else if (name_is(r, "WATCH") && watch(g)) {
        return;             /* every screen is popped: nothing of ours is left to answer */
    }
    ((gi_fn)VA_ACTDONE)(gi);
}

/* ---- S4: the rows' badges and lengths --------------------------------------------------------- */
static const unsigned char S4_STOLEN[6] = { 0x81, 0xEC, 0xBC, 0x00, 0x00, 0x00 };
static void* s_drawRet[8];
static int   s_drawDepth;

/* After every draw of the list, into the panel's own surface where the rows were just drawn.
   BOUND: the rows read are those the engine just drew, `top` .. `top + visible` with
   0 <= top <= count == our row count, each inside s_row; every position is inside the list's
   rect. ORDERING: every engine repaint of the list's rect is a call of 0x4A1B40 (the stage draw's
   id-2 branch 0x4A92D5 and its six other callers, the list wheel's), so this follows the rows in
   the same op stream, before the flip. The panel's surface is not an asset, so drawing into it
   revokes nothing. */
static void draw_badges(void)
{
    char* gi = main_gi();
    char* ctrls = NULL;
    char* r = recs_rec(gi, &ctrls);
    void* ctx;
    int lx, ly, lw, lh, top, count, ih, gh, k, rem;

    if (!r) return;
    ctx = *(void**)(ctrls + P_SURF);
    lx = *(short*)(r + G_XPOS);  ly = *(short*)(r + G_YPOS);
    lw = *(short*)(r + G_W);     lh = *(short*)(r + G_H);
    top = *(short*)(r + L_TOP);  count = *(short*)(r + L_COUNT);
    ih = *(short*)(r + L_ITEMH);
    gh = row_floor();
    if (!ctx || count != s_nrows || top < 0 || top > count || ih <= 0 || ih > 64 || gh <= 0)
        return;
    for (k = top, rem = lh; k < count && (k == top || rem >= gh); k++, rem -= ih) {
        const Row* row = &s_row[k];
        const Ent* e = &s_ent[row->ent];
        int ry = ly + 2 + (k - top) * ih;
        int b = e->fav ? B_FAV : B_FAV_OFF;
        if (s_badgeIn[b])
            ((gaf_blit_fn)VA_GAFBLIT)(ctx, s_badge[b], lx + 2, ry + (ih - s_badgeH[b]) / 2);
        if ((incomplete(e) && s_badgeIn[B_INCOMPLETE]) || (e->lock && s_badgeIn[B_LOCKED])) {
            char t[ROW_TEXT];
            int bx;
            memcpy(t, s_blob + row->at, (size_t)row->map_chars);
            t[row->map_chars] = 0;
            bx = lx + 2 + text_w(t) + BADGE_GAP;
            if (incomplete(e) && s_badgeIn[B_INCOMPLETE]) {
                ((gaf_blit_fn)VA_GAFBLIT)(ctx, s_badge[B_INCOMPLETE], bx,
                                          ry + (ih - s_badgeH[B_INCOMPLETE]) / 2);
                bx += s_badgeW[B_INCOMPLETE] + BADGE_GAP;
            }
            if (e->lock && s_badgeIn[B_LOCKED])
                ((gaf_blit_fn)VA_GAFBLIT)(ctx, s_badge[B_LOCKED], bx,
                                          ry + (ih - s_badgeH[B_LOCKED]) / 2);
        }
        ((text_draw_fn)VA_TEXTDRAW)(ctx, row->len, lx + lw - 3 - text_w(row->len), ry, -1, 0);
    }
}

static int __cdecl s4_before(void* esp)
{
    void** e = (void**)esp;
    char* gi = main_gi();
    if (!s_gm || (char*)e[1] != gi || (int)(size_t)e[2] != s_recsIdx || !recs_rec(gi, NULL) ||
        s_drawDepth >= 8)
        return 0;
    s_drawRet[s_drawDepth++] = e[0];
    return 1;
}

static void* __cdecl s4_after(unsigned int* regs)
{
    (void)regs;
    draw_badges();
    return s_drawDepth > 0 ? s_drawRet[--s_drawDepth] : NULL;
}

/* ================================================================================================
   Arming
   ================================================================================================ */

#define S1_VA 0x004AACAEu
#define S2_VA 0x0042644Cu
#define S3_VA 0x00429290u
#define S4_VA 0x004A1B40u

/* The button is the last thing armed: without S1 nothing appends REPLAYS, and S2, S3 and S4 act
   only on what S1 or the screen set up, so a partial install offers nothing. */
void tagpu_replays_install(int ready)
{
    int s2, s3, s4, s1 = 0;
    if (!ready) { rlog("replays: NOT armed - the archive or its redirect is not in"); return; }
    s2 = tagpu_detour_bytes_ok(S2_VA, S2_STOLEN, sizeof S2_STOLEN) &&
         tagpu_detour_observe(S2_VA, S2_STOLEN, sizeof S2_STOLEN, s2_before, NULL);
    s3 = s2 && tagpu_detour_bytes_ok(S3_VA, S3_STOLEN, sizeof S3_STOLEN) &&
         tagpu_detour_observe(S3_VA, S3_STOLEN, sizeof S3_STOLEN, s3_before, s3_after);
    s4 = s3 && tagpu_detour_bytes_ok(S4_VA, S4_STOLEN, sizeof S4_STOLEN) &&
         tagpu_detour_observe(S4_VA, S4_STOLEN, sizeof S4_STOLEN, s4_before, s4_after);
    s_armed = s4;
    if (s4)
        s1 = tagpu_detour_bytes_ok(S1_VA, S1_STOLEN, sizeof S1_STOLEN) &&
             tagpu_detour_observe(S1_VA, S1_STOLEN, sizeof S1_STOLEN, s1_before, NULL);
    s_armed = s1;
    if (s1 && !(tagpu_detour_bytes_ok(VA_UNIT_LOAD, UNIT_LOAD_STOLEN, sizeof UNIT_LOAD_STOLEN) &&
                tagpu_detour_observe(VA_UNIT_LOAD, UNIT_LOAD_STOLEN, sizeof UNIT_LOAD_STOLEN,
                                     unit_load_begin, NULL)))
        rlog("replays: the unit-data load 0x42D2E0 is not observed: a unit-types refusal never "
             "marks a recording");
    rlog("replays: %s (GUI_Load 0x4AACAE=%d, the builder 0x42644C=%d, the PCX loader 0x429290=%d, "
         "the list draw 0x4A1B40=%d; ground %s)", s1 ? "ARMED" : "NOT armed", s1, s2, s3, s4,
         s_groundName[0]);
}
