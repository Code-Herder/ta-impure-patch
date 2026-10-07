/* tagpu_dock.c -- the watching dock. API and arming: tagpu_dock.h. Design:
   research/notes/tadr-port/demo-recorder-s5-ui.md, *The watching dock*.

   Every engine address below was read off pristine/TotalA.exe.pristine (DISASSEMBLED unless
   marked).

   GAME THREAD, ALL OF IT BUT THE WALKER. The slot's open and tick run in the DrawGameScreen
   observer (tagpu_menu.c), the OnCommand in the pump's dispatch, the strip's clicks in the
   wndproc TA pumps on that thread, the layer at the post-GUI observer 0x46A308 and the level's
   end at the teardown wrap's entry. The walker reads the recording file and nothing of the
   engine's (THE WALKER). */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"
#include "tagpu_dock.h"
#include "tagpu_cards.h"
#include "tagpu_replay.h"
#include "tagpu_replays.h"
#include "tagpu_recreader.h"
#include "tagpu_recfmt.h"
#include "tagpu_art.h"
#include "tagpu_hud.h"
#include "tagpu_gaf.h"
#include "tagpu_log.h"

/* ---- engine addresses ------------------------------------------------------------------------ */
#define TA_MAIN          0x00511DE8u    /* TAdynmemStruct**                                     */
#define OFF_GAMETIME     0x38A47u
#define GI_UICHANGE      0x60           /* the actuated index in, the answer out                */
#define GM_CTRLS         0x04
#define P_GAFBANK        0xC0           /* ControlsAry[0]: the panel's own loaded bank          */
#define P_COUNT          0xB6           /* ControlsAry[0]: i16, the records after it            */
#define STRIDE           0x15B          /* a gadget record                                      */
#define GUI_RECORDS      200            /* the records GUI_Load allocates, 0..199               */
#define G_NAME           0x02           /* char[16]                                             */
#define G_XPOS           0x13
#define G_YPOS           0x15
#define G_W              0x17
#define G_H              0x19
#define G_ATTRIBS        0x1B           /* i32                                                  */
#define G_STATUS         0x137          /* a stage button's stage, advanced before OnCommand    */
#define VA_SETSTATUS     0x004A1080u    /* (gi, name, value) stdcall: the TOP screen, by name   */
#define VA_SETGRAYED     0x004A1250u    /* (gi, name, grayed) stdcall: the same scan            */
#define VA_SETLABEL      0x004A07D0u    /* (gi, name, text) stdcall: the same scan; copies 0x80
                                           bytes and sets gi+0xCCA itself (0x4A085E)            */
#define VA_SETDIRTY      0x0049FA90u    /* (gi): gi+0xCCA = 1, the pump repaints                */
#define VA_ACTDONE       0x004AB0A0u    /* (gi): gi+0x60 = -1, "handled" (0x4AB0A4)             */
#define VA_GAFBLIT       0x004B7F90u    /* (ctx, frame, x, y) stdcall; ctx NULL = the back buffer,
                                           clipped to the context's rect (0x4B807B..0x4B80C6)    */
#define VA_GAFFIND       0x004B8D40u    /* (bank, name) -> the entry, or NULL                    */
#define VA_YESORNO       0x00460680u    /* the in-game YESORNO opener, void cdecl               */
#define VA_YESORNO_KIND  0x00512FF8u    /* int: which question it asks; 0 = quit to the menu    */

typedef int   (__stdcall *set_status_fn)(void* gi, const char* name, int value);
typedef int   (__stdcall *set_grayed_fn)(void* gi, const char* name, int grayed);
typedef void  (__stdcall *set_label_fn)(void* gi, const char* name, const char* text);
typedef void  (__stdcall *gi_fn)(void* gi);
typedef void  (__stdcall *gaf_blit_fn)(void* ctx, const void* frame, int x, int y);
typedef void* (__stdcall *gaf_find_fn)(void* bank, const char* name);
typedef void  (__cdecl   *void_fn)(void);

/* ---- the layout, dock-relative --------------------------------------------------------------- */
#define DOCK_H       86
#define DOCK_MAX_W   4096               /* the ground's width: every dock_w fits in it          */
#define TILE_W       512                /* one ground frame: under TAGPU_GAF_DECMAX, the widest
                                           frame the GUI mirror carries as a sprite              */
#define NTILE        (DOCK_MAX_W / TILE_W)
#define DOCK_MIN_W   400                /* the row of buttons with Overlays and Leave clear of the
                                           bookmark                                             */
#define DOCK_GAP     8                  /* from the side panel and the screen's right edge      */
#define DOCK_LIFT    4                  /* above the bottom bar                                 */
#define STRIP_X      60                 /* the strip: (60, 8, dock_w - 124, 21)                 */
#define STRIP_Y      8
#define STRIP_RM     64                 /* dock_w - 124 = what is left after 60 and 64          */
#define STRIP_H      21
#define TRACK_Y      12                 /* its track: 14 rows, 6 px caps                        */
#define TRACK_H      14
#define TRACK_CAP    6
#define BTN_Y        44
#define BTN_W        28
#define BTN_H        22
#define SPD_Y        45
#define SPD_W        120
#define SPD_H        20
#define TEND_RX      60                 /* TEND's xpos is dock_w - 60                           */
#define LEAVE_RX     36                 /* DKLEAVE's, dock_w - 36                               */
#define OVER_RX      72                 /* DKOVER's, dock_w - 72: left of Leave, under the cards'
                                           panel, where the Overlays menu opens (tagpu_cards.c) */
#define KEY          9                  /* the frames' transparent index, the art's (artbake.py) */
#define GREY         59                 /* a greyed glyph, the menu's ramp 55..63               */
#define SNAP_PX      4                  /* a click this close to a marker takes the marker's T  */
#define JUMP         300                /* rewind and forward: 10 s of GameTime at 30 a second  */
#define LEAD         150                /* a death or a defeat seeks 5 s before it              */
#define MK_MAX       256

static const unsigned PRESET[5] = { 5, 10, 20, 40, 80 };   /* 1/2x .. 8x; 10 = 1x              */
/* DKSPEED's sixth stage, captioned "": the stage it shows while greyed. A greyed button under
   GREYED_LAST draws its caption unshaded -- the bit's arm (0x4A6086) skips the shade flag the
   stage arm sets for DrawText (0x4A60AB, read at 0x4A643F) -- so the greyed speed shows grey bars
   and no caption rather than a lit one. */
#define SPEED_BLANK  5

/* the art the dock is made of: every entry must be there, or the dock does not arm */
enum { A_TEX, A_TRACK, A_FILL, A_PLAY, A_PAUSE, A_REW, A_FWD, A_MARK, A_LEAVE,
       A_DEATH, A_DEFEAT, A_BOOK, A_HEAD, A_GHOST, A_N };
static const char* const ART[A_N] = {
    "dock-tex", "strip-track", "strip-fill", "g-play", "g-pause", "g-rewind", "g-forward",
    "g-bookmark", "g-leave", "m-death", "m-defeat", "m-mark", "m-playhead", "m-playhead-ghost" };
static const TAGPU_ART_FRAME* s_art[A_N];

static int    s_armed;
static char   s_gui[8192];
static unsigned char* s_gaf;

/* the open dock: its engine rect and the HUD scale it was placed at, set by place() before
   stage 1; its screen, set when stage 1 has built it */
static int    s_open, s_ex, s_ey, s_w, s_q8;
static void*  s_gm;
static int    s_leaveOk;
/* LEAVE'S QUESTION HOLDS THE PLAYBACK: the pause word from before the click, put back the first
   time the dock is on top again -- the question answered No, or never opened. Yes tears the level
   down instead, and the level's end forgets the question. */
static int    s_leaveAsk, s_leaveWasPaused;
static struct { int valid, play, speed, grey, leave, over; char now[16], end[16]; } s_push;

/* the strip's presses: `s_held` and `s_rpress` a press consumed, whose release is consumed too;
   `s_drag` the seek the left one asks for on its release, which a lost mouse cancels */
static int    s_held, s_drag, s_rpress;
static int    s_dismissed, s_rdismissed;   /* a press that closed the Overlays menu, either button */

/* the post-GUI frames: 0x18-byte engine headers over the art's own pixels, which tagpu_art keeps
   for the process and never writes; the fill also in every narrower width, for the tile the
   playhead cuts */
#define FILL_MAX 32
static unsigned char s_frame[A_N][0x18];
static unsigned char s_fillPix[FILL_MAX][FILL_MAX * FILL_MAX];
static unsigned char s_fillFrame[FILL_MAX][0x18];

/* the level's markers and bookmarks */
enum { MK_DEATH, MK_DEFEAT };
typedef struct { unsigned T; unsigned char kind, seat; } Marker;
static Marker          s_mk[MK_MAX];
static volatile LONG   s_mkN;
static volatile LONG   s_walkStop;
static HANDLE          s_walkH;
static int             s_walkStarted;
static char            s_walkPath[MAX_PATH];
static unsigned        s_bm[64];
static int             s_nbm;
static char            s_file[MAX_PATH];
static unsigned long long s_size;
static int             s_keyOk;

static char* ta_main(void) { return *(char**)TA_MAIN; }

static unsigned game_time(void)
{
    const char* m = ta_main();
    return m ? *(const unsigned*)(m + OFF_GAMETIME) : 0;
}

/* ================================================================================================
   The archive
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

/* one [GADGETg] with its [COMMON]; the caller writes the type's keys and closes it. RENDER.GUI's
   values for what is not ours to choose (tagpu_menu.c, common). */
static int common(char* b, int cap, int at, int g, int id, const char* name, int x, int y, int w,
                  int h, int attribs)
{
    return gput(b, cap, at,
        "[GADGET%d]\r\n\t{\r\n\t[COMMON]\r\n\t\t{\r\n"
        "\t\tid=%d;\r\n\t\tassoc=0;\r\n\t\tname=%s;\r\n"
        "\t\txpos=%d;\r\n\t\typos=%d;\r\n\t\twidth=%d;\r\n\t\theight=%d;\r\n"
        "\t\tattribs=%d;\r\n\t\tcolorf=15;\r\n\t\tcolorb=0;\r\n"
        "\t\ttexturenumber=0;\r\n\t\tfontnumber=0;\r\n\t\tactive=1;\r\n"
        "\t\tcommonattribs=0;\r\n\t\thelp=;\r\n\t\t}\r\n",
        g, id, name, x, y, w, h, attribs);
}

/* a label: attribs 37 right-aligns its text, 33 left-aligns it (tagpu_replays.c, label) */
static int label(char* b, int cap, int at, int g, const char* name, int x, int y, int w, int h,
                 int attribs)
{
    at = common(b, cap, at, g, 5, name, x, y, w, h, attribs);
    return gput(b, cap, at, "\ttext=;\r\n\tlink=;\r\n\t}\r\n");
}

/* attribs 1 is RENDER.GUI's */
static int button(char* b, int cap, int at, int g, const char* name, int x, int y, int w, int h,
                  int stages, const char* text)
{
    at = common(b, cap, at, g, 1, name, x, y, w, h, 1);
    return gput(b, cap, at, "\tstatus=0;\r\n\ttext=%s;\r\n\tquickkey=0;\r\n\tgrayedout=0;\r\n"
                            "\tstages=%d;\r\n\t}\r\n", text, stages);
}

/* The rects of the right-anchored records are written at open (tagpu_dock_place); the file's are
   for the widest dock. Record 0's `panel=` is empty: the ground gadget covers the whole panel. */
static int build_gui(char* b, int cap)
{
    int at = 0, g = 1, i;
    const int w = DOCK_MAX_W;

    at = gput(b, cap, at,
        "[GADGET0]\r\n\t{\r\n\t[COMMON]\r\n\t\t{\r\n\t\tid=0;\r\n\t\tassoc=0;\r\n\t\tname=DOCK;\r\n"
        "\t\txpos=0;\r\n\t\typos=0;\r\n\t\twidth=%d;\r\n\t\theight=%d;\r\n"
        "\t\tattribs=0;\r\n\t\tcolorf=0;\r\n\t\tcolorb=0;\r\n\t\ttexturenumber=0;\r\n"
        "\t\tfontnumber=0;\r\n\t\tactive=1;\r\n\t\tcommonattribs=0;\r\n\t\thelp=;\r\n\t\t}\r\n"
        "\ttotalgadgets=%d;\r\n"
        "\t[VERSION]\r\n\t\t{\r\n\t\tmajor=1;\r\n\t\tminor=0;\r\n\t\trevision=1;\r\n\t\t}\r\n"
        "\tpanel=;\r\n\tcrdefault=;\r\n\tescdefault=;\r\n\tdefaultfocus=;\r\n\t}\r\n",
        w, DOCK_H, NTILE + 9);
    /* the ground: id-12 pictures whose NAME is the frame, resolved in the panel's own bank
       (0x4A8ACA), side by side. The ones past the dock's width fall outside the panel's surface,
       to which the blit clips them (0x4B807B..0x4B80C6). */
    for (i = 0; i < NTILE; i++) {
        char name[16];
        _snprintf(name, sizeof name, "DOCKBG%d", i);
        at = common(b, cap, at, g++, 12, name, i * TILE_W, 0, TILE_W, DOCK_H, 0);
        at = gput(b, cap, at, "\t}\r\n");
    }
    at = label(b, cap, at, g++, "TNOW", 4, 12, 52, 13, 37);
    at = label(b, cap, at, g++, "TEND", w - TEND_RX, 12, 56, 13, 33);
    at = button(b, cap, at, g++, "DKREW", 60, BTN_Y, BTN_W, BTN_H, 0, "");
    at = button(b, cap, at, g++, "DKPLAY", 92, BTN_Y, BTN_W, BTN_H, 2, "|");
    at = button(b, cap, at, g++, "DKFWD", 124, BTN_Y, BTN_W, BTN_H, 0, "");
    at = button(b, cap, at, g++, "DKSPEED", 164, SPD_Y, SPD_W, SPD_H, SPEED_BLANK + 1,
                "1/2x|1x|2x|4x|8x|");
    at = button(b, cap, at, g++, "DKMARK", 296, BTN_Y, BTN_W, BTN_H, 0, "");
    at = button(b, cap, at, g++, "DKOVER", w - OVER_RX, BTN_Y, BTN_W, BTN_H, 0, "");
    at = button(b, cap, at, g++, "DKLEAVE", w - LEAVE_RX, BTN_Y, BTN_W, BTN_H, 0, "");
    return g - 1 == NTILE + 9 ? at : -1;
}

/* ---- composing frames from the art ------------------------------------------------------------ */

/* `a` nine-sliced into the rect (x, y, w, h) of a plane `stride` wide: its caps kept, its middle
   repeated, its key pixels leaving the plane as it was */
static void nine(unsigned char* p, int stride, int x, int y, int w, int h,
                 const TAGPU_ART_FRAME* a, int cap)
{
    int cx = cap * 2 < a->w ? cap : a->w / 2, cy = cap * 2 < a->h ? cap : a->h / 2;
    int mw = a->w - 2 * cx, mh = a->h - 2 * cy, i, j;
    for (j = 0; j < h; j++) {
        int sy = j < cy ? j : j >= h - cy ? a->h - (h - j) : mh > 0 ? cy + (j - cy) % mh : cy;
        for (i = 0; i < w; i++) {
            int sx = i < cx ? i : i >= w - cx ? a->w - (w - i) : mw > 0 ? cx + (i - cx) % mw : cx;
            unsigned char v = a->pix[sy * a->w + sx];
            if (v != a->key) p[(y + j) * stride + x + i] = v;
        }
    }
}

/* `a` at (x, y) over pixels the plane already has, so every frame of a button keeps its well's
   mask; `grey` paints every pixel of it one grey */
static void over(unsigned char* p, int w, int h, int x, int y, const TAGPU_ART_FRAME* a, int grey)
{
    int i, j;
    for (j = 0; j < a->h; j++)
        for (i = 0; i < a->w; i++) {
            int dx = x + i, dy = y + j;
            unsigned char v = a->pix[j * a->w + i];
            if (v == a->key || dx < 0 || dy < 0 || dx >= w || dy >= h || p[dy * w + dx] == KEY)
                continue;
            p[dy * w + dx] = grey ? GREY : v;
        }
}

static void well(unsigned char* p, int w, int h)
{
    memset(p, KEY, (size_t)w * (size_t)h);
    nine(p, w, 0, 0, w, h, s_art[A_TRACK], TRACK_CAP);
}

static void glyph(unsigned char* p, int w, int h, int a, int shift, int grey)
{
    const TAGPU_ART_FRAME* g = s_art[a];
    over(p, w, h, (w - g->w) / 2 + shift, (h - g->h) / 2 + shift, g, grey);
}

/* THE FRAME A BUTTON DRAWS (0x4A6062..0x4A616D). Not greyed: a stage button its stage (+0x137),
   or count - 2 while pressed; a plain one 0, or +0x138 while pressed. Greyed: count - 1 under
   GREYED_LAST (greyed_last below); else a plain button min(+0x138 + 2, count - 1), a stage button
   its stage's frame (0x4A6092..0x4A60B3). A plain button's frames: normal, pressed, greyed. `arg`
   is the glyph. */
static void draw_plain(unsigned char* p, int w, int h, int frame, int arg)
{
    well(p, w, h);
    glyph(p, w, h, arg, frame == 1, frame == 2);
}

/* DKOVER's glyph, a pointer arrow -- the cursor overlay's own shape -- drawn here in the play
   glyph's colour: a STAND-IN until the art kit carries an Overlays glyph. Frames as
   draw_plain's: normal, pressed (one pixel down and right), greyed. */
static const char* const OVER_GLYPH[] = {
    "X......", "XX.....", "XXX....", "XXXX...", "XXXXX..", "XXXXXX.", "XXXXXXX",
    "XXXX...", "XX.XX..", "X...XX.", "....XX." };
#define OVER_GW 7
#define OVER_GH ((int)(sizeof OVER_GLYPH / sizeof OVER_GLYPH[0]))

static void draw_over(unsigned char* p, int w, int h, int frame, int arg)
{
    const TAGPU_ART_FRAME* g = s_art[A_PLAY];
    unsigned char ink = GREY;
    int i, j, x0, y0;
    (void)arg;
    for (i = 0; i < g->w * g->h; i++)
        if (g->pix[i] != g->key) { ink = g->pix[i]; break; }
    well(p, w, h);
    x0 = (w - OVER_GW) / 2 + (frame == 1);
    y0 = (h - OVER_GH) / 2 + (frame == 1);
    for (j = 0; j < OVER_GH; j++)
        for (i = 0; i < OVER_GW; i++) {
            int dx = x0 + i, dy = y0 + j;
            if (OVER_GLYPH[j][i] != 'X' || dx < 0 || dy < 0 || dx >= w || dy >= h ||
                p[dy * w + dx] == KEY)
                continue;
            p[dy * w + dx] = frame == 2 ? GREY : ink;
        }
}

/* DKPLAY, a two-stage button: the stages, a spare, the pressed frame at count - 2, and the greyed
   frame last, in stagebuttnN's layout, drawn under GREYED_LAST (above). Stage 0 shows play, the
   playback being paused; stage 1 pause. */
static void draw_play(unsigned char* p, int w, int h, int frame, int arg)
{
    (void)arg;
    well(p, w, h);
    if (frame == 0) glyph(p, w, h, A_PLAY, 0, 0);
    else if (frame == 1) glyph(p, w, h, A_PAUSE, 0, 0);
    else if (frame == 4) glyph(p, w, h, A_PLAY, 0, 1);
}

/* DKSPEED: five stages, the bars lit up to the stage's; SPEED_BLANK, all dark; pressed at
   count - 2; greyed last, drawn under GREYED_LAST (above). The bars sit at the plate's right end,
   clear of the caption the engine draws at its left. */
static void draw_speed(unsigned char* p, int w, int h, int frame, int arg)
{
    const TAGPU_ART_FRAME* f = s_art[A_FILL];
    const TAGPU_ART_FRAME* t = s_art[A_TRACK];
    unsigned char dark = t->pix[(t->h / 2) * t->w + t->w / 2];
    int b, i, j;
    (void)arg;
    well(p, w, h);
    for (b = 0; b < 5; b++) {
        int x0 = w - 6 - 28 + b * 6, y0 = (h - 8) / 2;
        for (j = 0; j < 8; j++)
            for (i = 0; i < 4; i++) {
                unsigned char v = frame == 7 ? GREY : dark;
                if (frame < 5 && b <= frame) {
                    v = f->pix[((j + 1) % f->h) * f->w + i % f->w];
                    if (v == f->key) v = dark;
                }
                p[(y0 + j) * w + x0 + i] = v;
            }
    }
}

/* A ground tile at attach: the texture, continuing across the tiles (`arg` is the tile). At open
   the tiles are repainted for the dock's own width (repaint_ground). */
static void draw_ground(unsigned char* p, int w, int h, int frame, int arg)
{
    const TAGPU_ART_FRAME* t = s_art[A_TEX];
    int x, y, x0 = arg * TILE_W;
    (void)frame;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned char v = t->pix[(y % t->h) * t->w + (x0 + x) % t->w];
            p[y * w + x] = v == t->key ? 60 : v;
        }
}

typedef void (*frame_fn)(unsigned char* p, int w, int h, int frame, int arg);
typedef struct { const char* name; int w, h, n; frame_fn draw; int arg; } DockEnt;

static const DockEnt ENTS[] = {
    { "DOCKBG0", TILE_W, DOCK_H,     1, draw_ground, 0       },
    { "DOCKBG1", TILE_W, DOCK_H,     1, draw_ground, 1       },
    { "DOCKBG2", TILE_W, DOCK_H,     1, draw_ground, 2       },
    { "DOCKBG3", TILE_W, DOCK_H,     1, draw_ground, 3       },
    { "DOCKBG4", TILE_W, DOCK_H,     1, draw_ground, 4       },
    { "DOCKBG5", TILE_W, DOCK_H,     1, draw_ground, 5       },
    { "DOCKBG6", TILE_W, DOCK_H,     1, draw_ground, 6       },
    { "DOCKBG7", TILE_W, DOCK_H,     1, draw_ground, 7       },
    { "DKREW",   BTN_W, BTN_H,       3, draw_plain,  A_REW   },
    { "DKPLAY",  BTN_W, BTN_H,       5, draw_play,   0       },
    { "DKFWD",   BTN_W, BTN_H,       3, draw_plain,  A_FWD   },
    { "DKSPEED", SPD_W, SPD_H,       8, draw_speed,  0       },
    { "DKMARK",  BTN_W, BTN_H,       3, draw_plain,  A_MARK  },
    { "DKOVER",  BTN_W, BTN_H,       3, draw_over,   0       },
    { "DKLEAVE", BTN_W, BTN_H,       3, draw_plain,  A_LEAVE },
};
#define NENTS (int)(sizeof ENTS / sizeof ENTS[0])
typedef char dock_tiles_listed[NTILE == 8 ? 1 : -1];

/* The GAF, uncompressed, entries of several frames each (tagpu_menu.c's build_gaf, one frame
   each, is the same layout): the header, the entry offsets, then per entry its 0x28 bytes, its
   frame table (offset, flag 10 = a fixed frame) and its 0x18-byte frame headers; the pixels last.
   Uncompressed so that the ground's planes can be repainted in place at open. Every frame carries
   TAGPU_GAF_EXACT, so Classic++ draws it as GDI does (tagpu_gaf.h). */
static unsigned build_gaf(unsigned char* out, unsigned cap)
{
    unsigned at = 12 + 4u * NENTS, need = at, pix;
    int i, k;
    unsigned v;

    for (i = 0; i < NENTS; i++) need += 0x28 + (8u + 0x18u) * (unsigned)ENTS[i].n;
    pix = need;
    for (i = 0; i < NENTS; i++) need += (unsigned)(ENTS[i].w * ENTS[i].h * ENTS[i].n);
    if (!out) return need;
    if (cap < need) return 0;
    memset(out, 0, need);
    v = 0x00010100u; memcpy(out + 0, &v, 4);             /* signature              */
    v = NENTS;       memcpy(out + 4, &v, 4);
    for (i = 0; i < NENTS; i++) {
        const DockEnt* e = &ENTS[i];
        unsigned entoff = at, taboff = at + 0x28, frm0 = taboff + 8u * (unsigned)e->n;
        memcpy(out + 12 + 4 * i, &entoff, 4);
        *(unsigned short*)(out + entoff) = (unsigned short)e->n;
        v = 1u; memcpy(out + entoff + 2, &v, 4);
        lstrcpynA((char*)out + entoff + 8, e->name, 32);
        for (k = 0; k < e->n; k++) {
            unsigned frm = frm0 + 0x18u * (unsigned)k;
            memcpy(out + taboff + 8 * k, &frm, 4);
            v = 10u; memcpy(out + taboff + 8 * k + 4, &v, 4);
            *(unsigned short*)(out + frm + 0x00) = (unsigned short)e->w;
            *(unsigned short*)(out + frm + 0x02) = (unsigned short)e->h;
            out[frm + 0x08] = KEY;
            out[frm + 0x09] = 0;                           /* raw 8bpp               */
            v = TAGPU_GAF_EXACT; memcpy(out + frm + TAGPU_GF_TAG, &v, 4);
            memcpy(out + frm + 0x10, &pix, 4);
            e->draw(out + pix, e->w, e->h, k, e->arg);
            pix += (unsigned)(e->w * e->h);
        }
        at = frm0 + 0x18u * (unsigned)e->n;
    }
    return need;
}

static void set_frame(unsigned char* f, int w, int h, unsigned char key, const unsigned char* pix)
{
    memset(f, 0, 0x18);
    *(unsigned short*)(f + 0x00) = (unsigned short)w;
    *(unsigned short*)(f + 0x02) = (unsigned short)h;
    f[0x08] = key;
    f[0x09] = 0;
    {
        unsigned tag = TAGPU_GAF_EXACT;
        memcpy(f + TAGPU_GF_TAG, &tag, 4);
    }
    memcpy(f + 0x10, &pix, 4);
}

int tagpu_dock_files(TAGPU_UFO_FILE* f, int cap)
{
    unsigned need, len;
    int glen, i, w, j;

    for (i = 0; i < A_N; i++) {
        const TAGPU_ART_ENTRY* e = tagpu_art_entry("retail", ART[i]);
        s_art[i] = e && e->nframes > 0 ? &e->frames[0] : NULL;
        if (!s_art[i] || !s_art[i]->w || !s_art[i]->h) {
            tagpu_logf("dock: NOT armed - no %s art", ART[i]);
            return 0;
        }
    }
    if (s_art[A_FILL]->w > FILL_MAX || s_art[A_FILL]->h > FILL_MAX) {
        tagpu_logf("dock: NOT armed - strip-fill is larger than %dx%d", FILL_MAX, FILL_MAX);
        return 0;
    }
    if (cap < 2) return 0;
    glen = build_gui(s_gui, sizeof s_gui);
    need = build_gaf(NULL, 0);
    s_gaf = (unsigned char*)malloc(need);
    len = s_gaf && glen > 0 ? build_gaf(s_gaf, need) : 0;
    if (!len) {
        tagpu_logf("dock: NOT armed - the generated DOCK.GUI or its GAF does not fit");
        tagpu_dock_files_free();
        return 0;
    }
    for (i = 0; i < A_N; i++)
        set_frame(s_frame[i], s_art[i]->w, s_art[i]->h, s_art[i]->key, s_art[i]->pix);
    for (w = 1; w <= s_art[A_FILL]->w; w++) {
        for (j = 0; j < s_art[A_FILL]->h; j++)
            memcpy(s_fillPix[w - 1] + j * w, s_art[A_FILL]->pix + j * s_art[A_FILL]->w, (size_t)w);
        set_frame(s_fillFrame[w - 1], w, s_art[A_FILL]->h, s_art[A_FILL]->key, s_fillPix[w - 1]);
    }
    f[0].path = "impure/dock.gui";
    f[0].data = s_gui;
    f[0].size = (unsigned)glen;
    f[1].path = "anims/dock.gaf";
    f[1].data = s_gaf;
    f[1].size = len;
    return 2;
}

void tagpu_dock_files_free(void)
{
    free(s_gaf);
    s_gaf = NULL;
}

void tagpu_dock_install(int ready)
{
    s_armed = ready;
    tagpu_logf("dock: %s (DOCK.GUI over ARMMAIN2 on a level that plays a recording)",
               ready ? "ARMED" : "NOT armed");
}

int tagpu_dock_redirects(const char* name)
{
    return s_armed && name && !lstrcmpiA(name, TAGPU_DOCK_SCREEN);
}

/* ================================================================================================
   Placing it
   ================================================================================================ */

/* THE DOCK'S ENGINE RECT, a pure function of the surface and the HUD scale (q8, 256 = stock), so
   that tagpu_hud.c's map and the composite, which know only those, find the rect the dock was
   placed at. On screen the dock is magnified like the bars (tagpu_hud.h): right of the panel and
   above the bottom bar by DOCK_GAP and DOCK_LIFT at the scale, as wide as the screen leaves it.
   Its bottom-left corner is where the world's translation puts it, so the engine rect is that
   corner moved back by the translation, and the width the screen width over the scale. At stock
   it is the 1x layout: (136, H - 122, W - 144, 86). 1 when it fits. */
int tagpu_dock_rect(int W, int H, int q8, int* ex, int* ey, int* w, int* h)
{
    int pw, bh, gap, x0, x1, yb;
    if (q8 < 256) return 0;
    tagpu_hud_bands(q8, &pw, &bh);
    gap = (DOCK_GAP * q8) >> 8;
    x0 = pw + gap;
    x1 = W - gap;
    yb = H - bh - ((DOCK_LIFT * q8) >> 8);
    *w = ((x1 - x0) << 8) / q8;
    if (*w > DOCK_MAX_W) *w = DOCK_MAX_W;
    *h = DOCK_H;
    *ex = x0 - (pw - 128);
    *ey = yb - (bh - 32) - DOCK_H;
    return *w >= DOCK_MIN_W && *ey >= 32;
}

/* the dock's rect at the scale in force; `q8` the scale, 256 when HUD scale is not live */
static int geom(int* ex, int* ey, int* w, int* q8)
{
    int h;
    *q8 = 256;
    tagpu_hud_live(NULL, NULL, q8);
    return tagpu_dock_rect((int)g_ddraw.width, (int)g_ddraw.height, *q8, ex, ey, w, &h);
}

int tagpu_dock_wanted(void)
{
    int x, y, w, q;
    return s_armed && tagpu_replay_path() && geom(&x, &y, &w, &q);
}

/* a record by name, bounded by the panel's own count and by the 200 records GUI_Load allocates
   (0x4AAA68, 0x4AAA95) */
static char* find_rec(char* ctrls, const char* name)
{
    int n = *(short*)(ctrls + P_COUNT), i;
    for (i = 1; i <= n && i < GUI_RECORDS; i++) {
        char* r = ctrls + (size_t)i * STRIDE;
        if (!strncmp(r + G_NAME, name, 16)) return r;
    }
    return NULL;
}

/* GREYED_LAST, attribs bit 8: a greyed button with it draws its last frame (0x4A607E..0x4A608B)
   and nothing else of the greyed arms. Every greyable button of the dock takes it while greyed.
   A STAGE button has no greyed look of its own without it: it draws its stage's frame
   (0x4A6092..0x4A60B3). A plain button takes it too, so the whole row has one greyed look, the
   last frame of the dock's own art; the plain greyed arm (0x4A60D9..0x4A610E) is the engine's
   grey remap and -20 shade over the button instead (0x4A69AD / 0x4A69BA, gui-renderer §26).
   ONLY WHILE GREYED, and that is measured rather than read: with the bit on a live stage button
   its clicks stopped answering (DKPLAY and DKSPEED, 2026-10-01), and the reader responsible was not
   found. A greyed button takes no click (0x4AA36A), so the bit follows the record's own greyed
   flag (+0x13C bit 0), which the push sets first.
   It cannot come from the file in any case: stage 1 sets a stage button's attribs to
   (attribs & 0x4000) | 1 once it has split the captions (0x4A8EE1..0x4A8EEC). 1 when a record
   changed. */
#define GREYED_LAST 0x100
#define G_GREYED    0x13C               /* bit 0: greyed, SetGrayed's (0x4A12B6..0x4A12D0)      */
static int greyed_last(char* ctrls)
{
    static const char* const GREYABLE[7] = { "DKREW", "DKPLAY", "DKFWD", "DKSPEED", "DKMARK",
                                              "DKOVER", "DKLEAVE" };
    int i, changed = 0;
    for (i = 0; ctrls && i < 7; i++) {
        char* r = find_rec(ctrls, GREYABLE[i]);
        int a, want;
        if (!r) continue;
        memcpy(&a, r + G_ATTRIBS, 4);
        want = (r[G_GREYED] & 1) ? a | GREYED_LAST : a & ~GREYED_LAST;
        if (want == a) continue;
        memcpy(r + G_ATTRIBS, &want, 4);
        changed = 1;
    }
    return changed;
}

/* BEFORE STAGE 1: stage 1 makes the panel's surface record 0's w x h (0x4A9071..0x4A907C) and
   copies the screen under it from record 0's x, y (0x4A9081..0x4A9098), so the rect is written
   into the parsed records first. The right-anchored records move with the width. */
void tagpu_dock_place(char* ctrls)
{
    char* r;
    if (!ctrls || !geom(&s_ex, &s_ey, &s_w, &s_q8)) return;
    *(short*)(ctrls + G_XPOS) = (short)s_ex;
    *(short*)(ctrls + G_YPOS) = (short)s_ey;
    *(short*)(ctrls + G_W) = (short)s_w;
    *(short*)(ctrls + G_H) = (short)DOCK_H;
    if ((r = find_rec(ctrls, "TEND")) != NULL) *(short*)(r + G_XPOS) = (short)(s_w - TEND_RX);
    if ((r = find_rec(ctrls, "DKLEAVE")) != NULL) *(short*)(r + G_XPOS) = (short)(s_w - LEAVE_RX);
    if ((r = find_rec(ctrls, "DKOVER")) != NULL) *(short*)(r + G_XPOS) = (short)(s_w - OVER_RX);
}

/* THE GROUND, for this dock's width: composed whole, then cut into the tiles. BOUND: a tile is
   written only once it is checked to be exactly TILE_W x DOCK_H and uncompressed -- ours, as the
   archive made it -- so every write below is inside its plane. A tile's draw is clipped to the
   panel's surface by the GAF blitter's single-frame path (0x4B807B..0x4B80C6), so the columns past
   the dock's width never reach it. The planes are the bank's own, loaded fresh at each push through
   the id-0 load (0x4A859A..0x4A85C3), hence a repaint at every open. */
static unsigned char s_ground[DOCK_MAX_W * DOCK_H];

static void repaint_ground(char* ctrls)
{
    void* bank;
    unsigned char* p = s_ground;
    int x, y, i, w = s_w;

    if (!ctrls || w < DOCK_MIN_W || w > DOCK_MAX_W) return;
    bank = *(void**)(ctrls + P_GAFBANK);
    if (!bank) return;
    draw_ground(p, DOCK_MAX_W, DOCK_H, 0, 0);
    for (x = 0; x < w; x++) {
        p[x] = 63;
        p[DOCK_MAX_W + x] = 55;
        p[(DOCK_H - 2) * DOCK_MAX_W + x] = 59;
        p[(DOCK_H - 1) * DOCK_MAX_W + x] = 63;
    }
    for (y = 1; y < DOCK_H - 1; y++) {
        p[y * DOCK_MAX_W] = 63;
        if (y < DOCK_H - 2) p[y * DOCK_MAX_W + 1] = 55;
        p[y * DOCK_MAX_W + w - 2] = 59;
        p[y * DOCK_MAX_W + w - 1] = 63;
    }
    nine(p, DOCK_MAX_W, STRIP_X, TRACK_Y, w - STRIP_X - STRIP_RM, TRACK_H, s_art[A_TRACK],
         TRACK_CAP);
    for (i = 0; i < NTILE; i++) {
        char name[16];
        void* ent;
        const unsigned char* fr;
        unsigned char* tp;
        _snprintf(name, sizeof name, "DOCKBG%d", i);
        ent = ((gaf_find_fn)VA_GAFFIND)(bank, name);
        fr = ent ? tagpu_gaf_seq_frame((const char*)ent, 0) : NULL;
        if (!fr || *(const unsigned short*)(fr + TAGPU_GF_W) != TILE_W ||
            *(const unsigned short*)(fr + TAGPU_GF_H) != DOCK_H || fr[TAGPU_GF_COMP]) continue;
        tp = *(unsigned char**)(fr + TAGPU_GF_PIX);
        if (!tp) continue;
        for (y = 0; y < DOCK_H; y++)
            memcpy(tp + y * TILE_W, p + y * DOCK_MAX_W + i * TILE_W, TILE_W);
    }
}

/* THE LEAVE GUARD. Leave calls the engine's YESORNO opener and leaves the rest to its OnCommand
   and the quit tail it jumps to (0x460630: the teardown, UpdateIngameGUI(1), GUI_Pop, the menu).
   Both are compared by FNV-1a against the retail exe's bytes at every open; on a mismatch DKLEAVE
   stays greyed, and the engine's own menu still leaves. */
static const struct { unsigned a, b, fnv; } LEAVE_GUARD[2] = {
    { 0x460680, 0x4607F2, 0x8B2B971Eu },     /* the opener, to its ret                           */
    { 0x4605C0, 0x46065D, 0x9EF8B480u },     /* its OnCommand, through the quit tail's ret 4     */
};

static int leave_guard(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        unsigned h = 0x811C9DC5u, va;
        for (va = LEAVE_GUARD[i].a; va < LEAVE_GUARD[i].b; va++)
            h = (h ^ *(const unsigned char*)(size_t)va) * 0x01000193u;
        if (h != LEAVE_GUARD[i].fnv) {
            tagpu_logf("dock: Leave greyed - 0x%06X..0x%06X holds other bytes than it was made "
                       "against", LEAVE_GUARD[i].a, LEAVE_GUARD[i].b - 1);
            return 0;
        }
    }
    return 1;
}

/* THE MAGNIFIED REGION FOLLOWS THE ENGINE'S OWN CHAIN (tagpu_hud.h). It is set here, the moment
   stage 1 has built the screen, so the first frame that draws the dock is magnified; cleared in
   `tagpu_dock_closed`, before the slot pops it, and at the level's end; and re-checked every
   frame against the chain, so a dock the engine let go of on its own account stops taking the
   points under it at the next frame. All on the game thread, the one that pumps the wndproc,
   between the engine's own calls: a click is mapped into the dock while the engine holds it. */
static void region_publish(void* top)
{
    void* p = top;
    int n;
    for (n = 0; p && p != s_gm && n < 32; n++) p = *(void**)p;
    tagpu_hud_dock_set(s_open && p && p == s_gm ? s_q8 : 0);
}

void tagpu_dock_built(void* gi, void* gm)
{
    repaint_ground(gm ? *(char**)((char*)gm + GM_CTRLS) : NULL);
    s_leaveOk = leave_guard();
    s_open = 1;
    tagpu_cards_open(s_q8, gm);
    s_gm = gm;
    s_push.valid = 0;
    region_publish(gi ? *(void**)((char*)gi + 0x18) : NULL);
}

void tagpu_dock_closed(void)
{
    tagpu_cards_close();
    tagpu_hud_dock_set(0);
    s_gm = NULL;
    s_open = 0;
    s_drag = 0;
    s_push.valid = 0;
}

int tagpu_dock_owns(int gx, int gy)
{
    return tagpu_cards_owns(gx, gy) ||
           (s_open && gx >= s_ex && gx < s_ex + s_w && gy >= s_ey && gy < s_ey + DOCK_H);
}

/* ================================================================================================
   The strip: GameTime <-> x
   ================================================================================================ */

/* the track's inner run, in engine x: the caps are the track's ends, not time */
static void track(int* l, int* w)
{
    *l = s_ex + STRIP_X + TRACK_CAP;
    *w = s_w - STRIP_X - STRIP_RM - 2 * TRACK_CAP;
}

static int x_of(unsigned T, unsigned first, unsigned last)
{
    int l, w;
    track(&l, &w);
    if (T < first) T = first;
    if (T > last) T = last;
    return l + (int)((unsigned long long)(T - first) * (unsigned)w / (last - first));
}

/* the seek a point on the strip asks for: the time under it, or the time of a marker within
   SNAP_PX of it -- a death or a defeat LEAD before the event, a bookmark at it */
static unsigned target(int x, unsigned first, unsigned last)
{
    int l, w, best = SNAP_PX + 1, i, n = (int)InterlockedCompareExchange(&s_mkN, 0, 0);
    unsigned T;
    track(&l, &w);
    if (x < l) x = l;
    if (x > l + w) x = l + w;
    T = first + (unsigned)((unsigned long long)(unsigned)(x - l) * (last - first) / (unsigned)w);
    for (i = 0; i < n && i < MK_MAX; i++) {
        int d = abs(x_of(s_mk[i].T, first, last) - x);
        if (d < best) {
            best = d;
            T = s_mk[i].T > first + LEAD ? s_mk[i].T - LEAD : first;
        }
    }
    for (i = 0; i < s_nbm; i++) {
        int d = abs(x_of(s_bm[i], first, last) - x);
        if (d < best) { best = d; T = s_bm[i]; }
    }
    return T;
}

static int in_strip(int gx, int gy)
{
    return s_open && gx >= s_ex + STRIP_X && gx < s_ex + s_w - STRIP_RM &&
           gy >= s_ey + STRIP_Y && gy < s_ey + STRIP_Y + STRIP_H;
}

static int cursor_x(void)
{
    return (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
}

/* THE OVERLAYS MENU CLOSES ON A PRESS ANYWHERE BUT ITSELF AND ITS BUTTON, either button, and on
   the top screen that press does nothing else: it is consumed, with its release (THE
   CONSUMED-PRESS RULE), so closing the menu never selects a unit or presses what lies under it.
   Under an engine dialog the press closes the menu and goes on to the dialog, as THE ON-TOP RULE
   leaves the engine's clicks to it. A press on DKOVER is left to it: the engine's click on its
   release toggles the menu (tagpu_dock_oncommand). Whether the press was consumed. */
static int menu_dismiss(int gx, int gy, int on_top)
{
    if (!tagpu_cards_menu_open() || tagpu_cards_menu_owns(gx, gy)) return 0;
    if (s_open && gx >= s_ex + s_w - OVER_RX && gx < s_ex + s_w - OVER_RX + BTN_W &&
        gy >= s_ey + BTN_Y && gy < s_ey + BTN_Y + BTN_H)
        return 0;
    tagpu_cards_menu_close();
    return on_top;
}

/* THE CONSUMED-PRESS RULE, the sprocket's (tagpu_menu.c): a release is consumed exactly when its
   press was, so the engine never sees half a click and is never left holding a button; a press
   whose release was lost ends at the next press, with no seek. A lost mouse cancels the seek and
   nothing else (tagpu_dock_cancel_press). THE ON-TOP RULE is the pump's: it serves the top screen
   only (0x4A9FD0), so under the engine's dialogs the strip leaves the click to the engine as the
   dock's buttons do. */
int tagpu_dock_click(int gx, int gy, int down, int on_top)
{
    unsigned first, last;
    if (down) {
        s_dismissed = menu_dismiss(gx, gy, on_top);
        if (s_dismissed) {
            s_held = s_drag = 0;
            return 1;
        }
    } else if (s_dismissed) {
        s_dismissed = 0;
        return 1;
    }
    if (tagpu_cards_click(gx, gy, down, 0, on_top)) return 1;
    if (!down) {
        int was = s_held;
        if (s_drag && tagpu_replay_span(&first, &last))
            tagpu_replay_seek_request(target(gx, first, last));
        s_held = s_drag = 0;
        return was;
    }
    s_held = s_drag = 0;
    if (!on_top || !in_strip(gx, gy) || !tagpu_replay_live() || !tagpu_replay_span(NULL, NULL))
        return 0;
    s_held = s_drag = 1;
    return 1;
}

/* The right button removes the bookmark within SNAP_PX of the press, by the same rule. */
int tagpu_dock_rclick(int gx, int gy, int down, int on_top)
{
    unsigned first, last;
    int i, best = SNAP_PX + 1, k = -1;
    if (down) {
        s_rdismissed = menu_dismiss(gx, gy, on_top);
        if (s_rdismissed) {
            s_rpress = 0;
            return 1;
        }
    } else if (s_rdismissed) {
        s_rdismissed = 0;
        return 1;
    }
    if (tagpu_cards_click(gx, gy, down, 1, on_top)) return 1;
    if (!down) {
        int was = s_rpress;
        s_rpress = 0;
        return was;
    }
    s_rpress = 0;
    if (!on_top || !in_strip(gx, gy) || !s_keyOk || !tagpu_replay_span(&first, &last)) return 0;
    for (i = 0; i < s_nbm; i++) {
        int d = abs(x_of(s_bm[i], first, last) - gx);
        if (d < best) { best = d; k = i; }
    }
    if (k < 0) return 0;
    if (tagpu_replays_mark_remove(s_file, s_size, s_bm[k])) {
        memmove(&s_bm[k], &s_bm[k + 1], (size_t)(s_nbm - k - 1) * sizeof s_bm[0]);
        s_nbm--;
    }
    s_rpress = 1;
    return 1;
}

void tagpu_dock_cancel_press(void)
{
    tagpu_cards_cancel();
    s_drag = 0;
}

/* ================================================================================================
   The walker
   ================================================================================================
   THE MARKERS, read off the recording by a worker with its own reader: a commander death is a
   player's +0x106 rising (the destructor increments it when the dying unit is its side's commander,
   0x486990..0x486996 and the self-destruct path 0x486A86..0x486A8C), a defeat its +0x144, the units
   alive, reaching 0 from above (decremented at 0x486DFC..0x486E09). Both are in every recorded
   player image: replay-layout.tsv excludes neither.
   ORDERING: an entry is written whole before InterlockedExchange publishes the count that covers
   it, and entries are never rewritten, so the game thread reads entries below the count it read.
   LIFETIME: the worker owns its reader and closes it before it returns; tagpu_dock_level_end stops
   it and waits for it on the game thread, at the teardown's entry, before the count goes back to
   0. Between two looks at the stop flag the worker reads one iteration -- at most one block
   inflated, a bound the format sets (RECR_BLOCK_MAX) -- not a timeout. tagpu_recreader.c has no
   mutable statics and reads no engine memory, so the worker shares nothing with the playback. */
_Static_assert(0x144 + 2 <= REC_PLAYER_BYTES, "the walker's two fields are in the player image");

/* the log line's list: the first few markers of a kind, "T (seat p)" each */
static void note(char* b, unsigned cap, unsigned n, unsigned T, unsigned seat)
{
    size_t k = strlen(b);
    if (n > 6 || k + 24 >= cap) return;
    _snprintf(b + k, cap - k, n == 6 ? ", ..." : "%s%u (seat %u)", n ? ", " : " at ", T, seat);
    b[cap - 1] = 0;
}

static void publish(unsigned T, int kind, int seat, unsigned* lost)
{
    LONG n = InterlockedCompareExchange(&s_mkN, 0, 0);
    if (n >= MK_MAX) { (*lost)++; return; }
    s_mk[n].T = T;
    s_mk[n].kind = (unsigned char)kind;
    s_mk[n].seat = (unsigned char)seat;
    InterlockedExchange(&s_mkN, n + 1);
}

static unsigned u16_at(const uint32_t* img, unsigned off)
{
    unsigned short v;
    memcpy(&v, (const unsigned char*)img + off, 2);
    return v;
}

static DWORD WINAPI walker(void* arg)
{
    char why[200] = "";
    RecReader* r;
    const RecHeader* h;
    unsigned cmdr[REC_PLAYERS], alive[REC_PLAYERS], deaths = 0, defeats = 0, lost = 0, iters = 0;
    unsigned s;
    int seeded = 0, k = 0;
    char dl[200] = "", fl[200] = "", capn[64] = "";
    DWORD t0 = GetTickCount();
    const char* stop = "the recording's end";
    (void)arg;

    r = recr_open(s_walkPath, why, sizeof why);
    if (!r) {
        tagpu_logf("dock: markers: none - %s", why);
        return 0;
    }
    h = recr_header(r);
    for (;;) {
        RecWorld* W;
        if (InterlockedCompareExchange(&s_walkStop, 0, 0)) { stop = "stopped at the level's end"; break; }
        k = recr_tick(r, why, sizeof why);
        if (k <= 0) { if (k < 0) stop = why; break; }
        W = recr_world(r);
        iters++;
        for (s = 0; s < h->nseats && s < REC_PLAYERS; s++) {
            unsigned p = h->seats[s].index, c, a;
            if (p >= REC_PLAYERS) continue;
            c = u16_at(W->player[p], 0x106);
            a = u16_at(W->player[p], 0x144);
            if (seeded && c > cmdr[p]) {
                publish(W->tick, MK_DEATH, (int)p, &lost);
                note(dl, sizeof dl, deaths++, W->tick, p);
            }
            if (seeded && alive[p] > 0 && a == 0) {
                publish(W->tick, MK_DEFEAT, (int)p, &lost);
                note(fl, sizeof fl, defeats++, W->tick, p);
            }
            cmdr[p] = c;
            alive[p] = a;
        }
        seeded = 1;
    }
    recr_close(r);
    if (lost) _snprintf(capn, sizeof capn, ", %u past the %d the strip holds", lost, MK_MAX);
    tagpu_logf("dock: markers: %u commander deaths%s, %u defeats%s, %u iterations walked in %lu ms "
               "(%s)%s", deaths, dl, defeats, fl, iters, (unsigned long)(GetTickCount() - t0), stop,
               capn);
    return 0;
}

/* The level's markers and bookmarks, once per level, on the first tick that sees the playback's
   first step played. */
static void walk_start(void)
{
    const char* path = tagpu_replay_path();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    s_walkStarted = 1;
    if (!path) return;
    lstrcpynA(s_walkPath, path, sizeof s_walkPath);
    s_keyOk = 0;
    s_nbm = 0;
    if (!_strnicmp(path, "replays\\", 8) && GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        lstrcpynA(s_file, path + 8, sizeof s_file);
        s_size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        s_keyOk = 1;
        s_nbm = tagpu_replays_marks_get(s_file, s_size, s_bm, 64);
    }
    InterlockedExchange(&s_walkStop, 0);
    InterlockedExchange(&s_mkN, 0);
    s_walkH = CreateThread(NULL, 0, walker, NULL, 0, NULL);
    if (!s_walkH) tagpu_logf("dock: markers: none - the walker did not start (%lu)", GetLastError());
}

void tagpu_dock_level_end(void)
{
    tagpu_cards_close();
    if (s_walkH) {
        InterlockedExchange(&s_walkStop, 1);
        WaitForSingleObject(s_walkH, INFINITE);
        CloseHandle(s_walkH);
        s_walkH = NULL;
    }
    InterlockedExchange(&s_mkN, 0);
    s_walkStarted = 0;
    s_nbm = 0;
    s_keyOk = 0;
    s_drag = 0;
    s_leaveAsk = 0;
    tagpu_hud_dock_set(0);
}

/* ================================================================================================
   The model push
   ================================================================================================ */

static void fmt_time(char* b, unsigned cap, unsigned ticks)
{
    unsigned s = ticks / 30;
    if (s >= 3600) _snprintf(b, cap, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else _snprintf(b, cap, "%u:%02u", s / 60, s % 60);
    b[cap - 1] = 0;
}

static int preset_of(unsigned gs)
{
    int i, best = 0;
    for (i = 1; i < 5; i++)                     /* the nearest; a tie goes to the slower */
        if ((gs > PRESET[i] ? gs - PRESET[i] : PRESET[i] - gs) <
            (gs > PRESET[best] ? gs - PRESET[best] : PRESET[best] - gs)) best = i;
    return best;
}

static const char* const GREYED_LIVE[5] = { "DKREW", "DKPLAY", "DKFWD", "DKSPEED", "DKMARK" };

/* THE SETTERS ACT ON THE TOP SCREEN, BY NAME (0x4A1080, 0x4A1250, 0x4A07D0 all scan gi+0x18's
   records): under an engine dialog they would search the dialog, which has none of these names.
   So the push runs only while the dock is on top, and a frame off the top forgets what was pushed,
   so the first frame back pushes everything. */
void tagpu_dock_tick(void* gi, int on_top)
{
    unsigned first = 0, last = 0, now = game_time(), gs = (unsigned)tagpu_replay_speed();
    int live = tagpu_replay_live(), span = tagpu_replay_span(&first, &last), dirty = 0, i, sp;
    char tnow[16] = "", tend[16] = "";

    region_publish(*(void**)((char*)gi + 0x18));
    tagpu_cards_tick(on_top);
    if (!s_walkStarted && live) walk_start();
    /* A speed the dock has no stage for -- the player's own, or the Options slider's -- is put
       on the nearest preset, so the stage shown is always the speed in force. */
    sp = preset_of(gs);
    if (live && gs != PRESET[sp]) tagpu_replay_speed_set(PRESET[sp]);

    if (!on_top) { s_push.valid = 0; return; }
    if (s_leaveAsk) {
        s_leaveAsk = 0;
        tagpu_replay_pause(s_leaveWasPaused);
    }
    if (span) {
        /* the time the playhead shows: GameTime runs on past the recording's end, the playhead
           stops there */
        unsigned T = s_drag ? target(cursor_x(), first, last) : now > last ? last : now;
        fmt_time(tnow, sizeof tnow, (T > first ? T : first) - first);
        fmt_time(tend, sizeof tend, last - first);
    }
    if (!s_push.valid || s_push.play != !tagpu_replay_paused()) {
        s_push.play = !tagpu_replay_paused();
        ((set_status_fn)VA_SETSTATUS)(gi, "DKPLAY", s_push.play);
        dirty = 1;
    }
    if (!s_push.valid || s_push.speed != (live ? sp : SPEED_BLANK)) {
        s_push.speed = live ? sp : SPEED_BLANK;
        ((set_status_fn)VA_SETSTATUS)(gi, "DKSPEED", s_push.speed);
        dirty = 1;
    }
    if (!s_push.valid || s_push.grey != !live) {
        s_push.grey = !live;
        for (i = 0; i < 5; i++) ((set_grayed_fn)VA_SETGRAYED)(gi, GREYED_LIVE[i], s_push.grey);
        dirty = 1;
    }
    /* the menu opens over the cards, so Overlays is live exactly while they are shown */
    if (!s_push.valid || s_push.over != !(live && tagpu_cards_menu_available())) {
        s_push.over = !(live && tagpu_cards_menu_available());
        ((set_grayed_fn)VA_SETGRAYED)(gi, "DKOVER", s_push.over);
        dirty = 1;
    }
    if (!s_push.valid || s_push.leave != !s_leaveOk) {
        s_push.leave = !s_leaveOk;
        ((set_grayed_fn)VA_SETGRAYED)(gi, "DKLEAVE", s_push.leave);
        dirty = 1;
    }
    {
        char* top = *(char**)((char*)gi + 0x18);
        if (top && greyed_last(*(char**)(top + GM_CTRLS))) dirty = 1;
    }
    if (!s_push.valid || strcmp(s_push.now, tnow)) {
        memcpy(s_push.now, tnow, sizeof tnow);
        ((set_label_fn)VA_SETLABEL)(gi, "TNOW", tnow);
    }
    if (!s_push.valid || strcmp(s_push.end, tend)) {
        memcpy(s_push.end, tend, sizeof tend);
        ((set_label_fn)VA_SETLABEL)(gi, "TEND", tend);
    }
    s_push.valid = 1;
    if (dirty) ((gi_fn)VA_SETDIRTY)(gi);
}

/* ================================================================================================
   OnCommand
   ================================================================================================ */

static void mark_here(unsigned now)
{
    int i;
    if (!s_keyOk || s_nbm >= 64 || !tagpu_replays_mark_add(s_file, s_size, now)) return;
    for (i = s_nbm; i > 0 && s_bm[i - 1] > now; i--) s_bm[i] = s_bm[i - 1];
    s_bm[i] = now;
    s_nbm++;
}

/* The pump's dispatch, the dock on top. -1 is the pop path (GUI_Pop 0x4A9676, the pump's inlined
   pop 0x4AA7AC, and the teardown's pops once the slot has let the dock go): nothing is touched.
   Every handled click is answered with ActDone: a click left unanswered makes the pump pop the top
   screen (0x4AA79A..0x4AA7FA) -- after Leave, YESORNO, which 0x460680's GUI_Load does not answer
   for us (it writes gi+0x64, not +0x60: 0x4AACAE). */
void __stdcall tagpu_dock_oncommand(void* gi)
{
    char* top;
    char* ctrls;
    char* r;
    int idx;
    unsigned now = game_time();

    if (!gi) return;
    idx = *(int*)((char*)gi + GI_UICHANGE);
    if (idx < 0) return;
    top = *(char**)((char*)gi + 0x18);
    ctrls = top ? *(char**)(top + GM_CTRLS) : NULL;
    if (!ctrls || idx < 1 || idx > *(short*)(ctrls + P_COUNT) || idx >= GUI_RECORDS) return;
    r = ctrls + (size_t)idx * STRIDE;

    if (!strncmp(r + G_NAME, "DKREW", 16)) {
        tagpu_replay_seek_request(now > JUMP ? now - JUMP : 0);
    } else if (!strncmp(r + G_NAME, "DKFWD", 16)) {
        tagpu_replay_seek_request(now + JUMP);
    } else if (!strncmp(r + G_NAME, "DKPLAY", 16)) {
        tagpu_replay_pause(*(unsigned char*)(r + G_STATUS) == 0);   /* 0 shows play: paused */
    } else if (!strncmp(r + G_NAME, "DKSPEED", 16)) {
        /* the engine advances the stage modulo the stage count (0x4AA377..0x4AA38F), so 8x's
           click lands on SPEED_BLANK: it is the wrap to 1/2x */
        unsigned st = *(unsigned char*)(r + G_STATUS);
        tagpu_replay_speed_set(PRESET[st < SPEED_BLANK ? st : 0]);
    } else if (!strncmp(r + G_NAME, "DKMARK", 16)) {
        mark_here(now);
    } else if (!strncmp(r + G_NAME, "DKOVER", 16)) {
        tagpu_cards_menu_toggle();
    } else if (!strncmp(r + G_NAME, "DKLEAVE", 16)) {
        if (!s_leaveOk) return;
        /* the engine's own Exit -> Main Menu path, EXITMENU's handler at 0x46082B..0x46083A:
           question 0, "return to the main menu", then the opener; its OnCommand 0x4605C0 runs
           the quit tail on Yes and pops itself on No */
        s_leaveWasPaused = tagpu_replay_paused();
        s_leaveAsk = 1;
        tagpu_replay_pause(1);
        *(int*)VA_YESORNO_KIND = 0;
        ((void_fn)VA_YESORNO)();
        ((set_label_fn)VA_SETLABEL)(gi, "TITLE", "Leave this replay?");
    } else {
        return;
    }
    s_push.valid = 0;
    ((gi_fn)VA_ACTDONE)(gi);
}

/* ================================================================================================
   The layer
   ================================================================================================ */

/* Is any screen above the dock over its rect? The engine's live chain, read on the game thread
   between the engine's own calls, from the top down its +0 links to the dock's, at most 32 steps
   (tagpu_menu.c's on_stack bound); each screen's rect is its record 0. A rect not yet placed
   (negative) counts as over it. */
static int covered(void* top, void* gm)
{
    void* p = top;
    int n;
    for (n = 0; p && p != gm && n < 32; n++) {
        const char* c = *(const char**)((const char*)p + GM_CTRLS);
        int x, y, w, h;
        if (!c) return 1;
        x = *(const short*)(c + G_XPOS); y = *(const short*)(c + G_YPOS);
        w = *(const short*)(c + G_W);    h = *(const short*)(c + G_H);
        if (x < 0 || y < 0) return 1;
        if (x < s_ex + s_w && x + w > s_ex && y < s_ey + DOCK_H && y + h > s_ey) return 1;
        p = *(void**)p;
    }
    return p != gm;
}

static void blit(const unsigned char* f, int x, int y)
{
    ((gaf_blit_fn)VA_GAFBLIT)(NULL, f, x, y);
}

static void centred(int a, int x, int cy)
{
    blit(s_frame[a], x - s_art[a]->w / 2, cy - s_art[a]->h / 2);
}

/* BOUND: every x comes from a T clamped to [first, last], last > first (the span is refused
   otherwise, tagpu_replay.c's span_read), and the blitter clips to the back buffer, so a wrong x
   costs a wrong picture, never memory. */
void tagpu_dock_draw(void* top, void* gm)
{
    unsigned first, last;
    int l, w, cy, px, x, i, n;
    const TAGPU_ART_FRAME* f = s_art[A_FILL];

    if (top == gm) tagpu_cards_draw();
    if (!s_armed || !s_open || !tagpu_replay_span(&first, &last) || covered(top, gm)) return;
    track(&l, &w);
    cy = s_ey + TRACK_Y + TRACK_H / 2;
    px = x_of(game_time(), first, last);
    for (x = l; x < px; x += f->w) {
        int tw = px - x < f->w ? px - x : f->w;
        blit(s_fillFrame[tw - 1], x, cy - f->h / 2);
    }
    n = (int)InterlockedCompareExchange(&s_mkN, 0, 0);
    for (i = 0; i < n && i < MK_MAX; i++)
        if (s_mk[i].kind == MK_DEATH) centred(A_DEATH, x_of(s_mk[i].T, first, last), cy);
    for (i = 0; i < n && i < MK_MAX; i++)
        if (s_mk[i].kind == MK_DEFEAT) centred(A_DEFEAT, x_of(s_mk[i].T, first, last), cy);
    for (i = 0; i < s_nbm; i++) centred(A_BOOK, x_of(s_bm[i], first, last), cy);
    if (s_drag) centred(A_GHOST, x_of(target(cursor_x(), first, last), first, last), cy);
    centred(A_HEAD, px, cy);
}
