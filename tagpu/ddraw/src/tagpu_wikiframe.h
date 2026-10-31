#ifndef TAGPU_WIKIFRAME_H
#define TAGPU_WIKIFRAME_H
/* tagpu_wikiframe -- the whole game frame on a frame:game entry's stage (research/notes/ingame-wiki-
   replay-plan.md, decisions 6 and 20-24): the watcher's view of the replay's game, the frame record
   the stage HUD is drawn from, the HUD's state from that record, the viewer's cursor among the
   HUD's controls, Leave's question and Try it yourself's press machine. A port of the mockup's
   tools/wiki/mockup/frame.js, function for function and under its names; that file's header is the
   specification of both, and tools/wiki/framecheck.mjs holds the two equal.

   THE FRAME is a game screen of the map's own size: Wg = 128 + 16 map_w and Hg = max(16 map_h + 64,
   the column's rows) game pixels -- the column at x 0..127, the top bar on rows 0..31 and the bottom
   bar on Hg - 32..Hg - 1 right of it, the map at the viewport's top left (the camera at 0, 0 at
   zoom 1). tagpu_wikistage.c draws it (WIKISTAGEFRAME).

   THE TWO THREADS. The record is filled on the game thread from the replay's view and its game's
   view (tagpu_wikiframe_record), carried by value, and bounded on the render thread's own copy by
   tagpu_wikiframe_bound before anything reads it: every value in it is DATA until then. Every
   other function here reads only a bounded record and immutable art.

   PURE. No global, no engine address, no allocation; pure C99 and the C library, so framecheck.mjs
   compiles it natively. The DLL computes it on SSE2 (Makefile, THE WIKI'S FRAME), so its doubles
   are frame.js's. */
#include <stdint.h>
#include "tagpu_hudlay.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikireplay.h"
#include "tagpu_wikisim.h"
#include "tagpu_wikistage.h"

struct WIKISIDE;

#define WIKIFRAME_SIDE_W  WIKI_FRAME_SIDE_W /* the engine's column width, game pixels            */
#define WIKIFRAME_BAR     WIKI_FRAME_BAR    /* each bar's height, game pixels                     */
#define WIKIFRAME_HUD_H   720               /* the HUD unit is the frame's height / 720 (Q3)      */
#define WIKIFRAME_TOAST   54                /* a toast shows 1.8 s of demo ticks (sim spec 4.7)   */

/* The frame's size in game pixels of a map w x h cells and a column of `side_h` rows (0 none). */
void tagpu_wikiframe_size(int w, int h, int side_h, int* Wg, int* Hg);

/* ---- the record ------------------------------------------------------------------------------ */

#define WIKIFRAME_MARKS (WIKI_REPLAY_MARKS + WIKI_REPLAY_BOOKS)
typedef char wikiframe_marks_fit[WIKIFRAME_MARKS <= HUD_MAXMARK ? 1 : -1];

typedef struct { int32_t t; int8_t kind, seat; int16_t pad; } WIKIFRAMEMARK;  /* a tick; HUD_MK_*; -1 bookmark */
typedef struct {
    char    name[WIKI_SEAT_NAME];
    uint8_t colour, lost, out, pad;         /* colour: the seat's colour index (owner k)              */
    float   units, m, mmax, mi, mo, e, emax, ei, eo;
} WIKIFRAMESEAT;
/* THE VIEWER'S CURSOR in control space: the trip from `from` (its control and final tick) to `to`,
   "" being the rest point; a track's tick in from_v / to_v (to_v the drag's current one), -1 for a
   button; glide step i of n, at `to` once i reaches n, where it stays until the next trip starts */
typedef struct {
    char    from[HUD_KEYLEN], to[HUD_KEYLEN];
    int32_t from_v, to_v;
    uint8_t on, i, n, pad;
} WIKIFRAMECUR;
typedef struct {
    uint8_t       on;                       /* a frame:game entry's replay is on the stage               */
    uint8_t       playing;                  /* the game is not paused and not at its end                 */
    uint8_t       question;                 /* Leave's question is open                                  */
    uint8_t       nseat;                    /* <= WIKI_SEATS                                             */
    int8_t        side;                     /* the panel's, the bars' and the badge's layout side        */
    uint8_t       colour;                   /* the badge's: seat 0's colour index; 0xFF none             */
    uint8_t       speed;                    /* tenths: 5, 10, 20, 40, 80                                 */
    uint8_t       try_it;                   /* the presses are the player's: no scripted cursor          */
    int32_t       playhead, end;            /* game ticks                                                */
    WIKIFRAMESEAT seat[WIKI_SEATS];
    uint16_t      nmark, pad;
    WIKIFRAMEMARK mark[WIKIFRAME_MARKS];    /* the walker's marks, then the bookmarks                    */
    char          held[HUD_KEYLEN];         /* the HUD control held down; "" none                        */
    char          toast[HUD_TOASTLEN];      /* "" none                                                   */
    WIKIFRAMECUR  cur;
} WIKIFRAMEREC;

/* The replay HUD's control `c` (WIKI_HUD_*, leave:yes included) by the key the HUD's hit map and
   the replay name it; "" for none. A control a demo presses is one of the WIKI_NHUD. */
const char* tagpu_wikiframe_key(int c);

/* The toast the HUD shows at demo tick rv->tick, the game's strings (tagpu_hudinput.c), into `out`
   (HUD_TOASTLEN bytes); "" for none. */
void tagpu_wikiframe_toast(const WIKIREPLAYVIEW* rv, char* out);

/* THE FRAME RECORD from replay view `rv` and the game view `gv` it shows (NULL: no seats), the
   layout side of the controlled seat `side`, `try_it` (the presses are the player's) and in Try it
   the control the press machine holds (`try_held`, NULL or "" none). THE LIFETIME: called after the
   fill's last tagpu_wikireplay_step or _press, because `gv` points into the game simulation's view
   arrays, which the next step or press may free; the record holds values only, so it outlives
   them. The cursor's glide is on the viewer's clock, the toast's age on the demo tick (frame.js,
   THE GLIDE). */
void tagpu_wikiframe_record(const WIKIREPLAYVIEW* rv, const WIKIVIEW* gv, int side, int try_it, const char* try_held,
                            WIKIFRAMEREC* out);

/* THE BOUND, on the render thread's own copy before anything reads it: the record is DATA until
   here. Flags to 0/1; nseat to WIKI_SEATS and nmark to WIKIFRAME_MARKS; end to 1..WIKI_GAME_END_MAX
   and the playhead to 0..end; a mark's tick to 0..end, a mark of an unknown kind or a seat outside
   -1..nseat - 1 dropped; the speed one of WIKI_REPLAY_SPEEDS, else 10; the side below `nside`, else
   -1; the badge's colour below `nlogo`, else none (0xFF); a seat's colour below `nlogo` or below
   WIKI_SEATS (the whitebox table), else 0xFF (grey); every float finite and within WIKI_ECON_MAX;
   every string terminated in its array; the held key and the cursor's keys among the replay's
   controls (a held key that is not becomes "", a cursor key that is not turns the cursor off), and
   the cursor's n in 1..64 with i <= n, else the cursor off. */
void tagpu_wikiframe_bound(WIKIFRAMEREC* f, int nside, int nlogo);

/* ---- the watcher's view ------------------------------------------------------------------------ */

/* THE IDLE COLUMN of layout side `side`: its MAIN2 with nothing selected, a watcher's screen (R0:
   the viewer controls the spectator record, whose unit list is empty). Each gadget's state is
   sim.js gadgetState(g, null, false)'s: hidden -1; the orders and build tabs, the build buttons,
   STOP and the orders 2 (greyed); anything else 0. framecheck pins this second copy of that rule
   against a fresh simulation's MAIN2. 1 into `out`, or 0 when the layout has no such side. */
int tagpu_wikiframe_idle_side(const WIKILAYOUT* lay, int side, WIKISTAGESIDE* out);

/* THE WATCHER'S VIEW, in place, of a copy (tagpu_wikistage_copy) of the replay's game view: the frame
   flag set, the fog all clear (the full-map reveal sets the watcher's sight and explored bit in
   every cell, tagpu_replay.c), seat 0's cursor, ghost, sites, queued sites, lines, drag, sweep, held
   keys and every selection gone (the viewer is not seat 0), and the column the idle MAIN2 of `side`
   in `lay`, which `layout_src` (WIKISTAGE_LAYOUT_*) names to the render thread, whatever the game
   script's sidebar says: that key governs seat 0's recorded inputs, and the frame is the watcher's
   screen. No column when the layout has no such side. */
void tagpu_wikiframe_view(WIKISTAGEVIEW* v, const WIKILAYOUT* lay, int side, int layout_src);

/* ---- the stage HUD ---------------------------------------------------------------------------- */

/* A seat's colour, 0xRRGGBB, by its colour index `k`: the palette colour of the middle pixel of
   logos frame k, as the game's HUD takes it (tagpu_replay.c logo_colour: pixel (w / 2, h / 2)), or
   without that frame the whitebox table (frame.js COLOURS), grey past it. */
uint32_t tagpu_wikiframe_seat_colour(const struct WIKISIDE* sd, int k);

/* THE STAGE HUD'S STATE from a bounded record in a frame Wg x Hg, at `px` device pixels a unit,
   hud.js's state() as frame.js hudState makes it: a screen of the frame's own size at 100 % (Q3),
   the engine frame the column and both bars, every switch on and nothing folded as a fresh Watch,
   each seat a team of one, no sight, no follow, no forks, no pointer (mx, my -1), the record's held
   control and toast. `sd` the published sidebar art for the seats' colours, or NULL.
   THE CHART: the stats window stays closed, so no build of this state emits HOP_CHART. That is what
   keeps the HUD pass's one chart image the replay HUD's; the stage must have its own before it
   opens the window (P). fork_max is HUD_MAXFORK until the recorder's constant reaches here through
   an engine-free header (R1). */
void tagpu_wikiframe_hudstate(const WIKIFRAMEREC* f, int Wg, int Hg, float px, const struct WIKISIDE* sd, HUDSTATE* st);

/* LEAVE'S QUESTION in canvas device pixels for the fit `fit` of a frame Wg x Hg: YESORNO.GUI's own
   rects (retail totala1.hpi, MEASURED with tools/wiki/tavfs.py, 2026-10-31) -- the panel 400 x 100
   at (112, 118) of the 640 x 480 GUI space, its title at (25, 25) 350 x 20, Yes at (80, 55) and No
   at (225, 55), each 95 x 20 -- in that space centred on the frame [INFERRED: lane watchlive reads
   the real rects], and the frame's own rect, each edge snapped as every frame part is. */
typedef struct { int x, y, w, h; } WIKIFRAMEBOX;
typedef struct { WIKIFRAMEBOX panel, title, yes, no, frame; } WIKIFRAMEDLG;
void tagpu_wikiframe_dialog(const WIKISTAGEFIT* fit, int Wg, int Hg, WIKIFRAMEDLG* out);

/* A point of a glide from a to b at step i of n: the curve of sim.js glideAt in doubles, without
   its truncation, since it is a position on the screen and not simulation state. */
double tagpu_wikiframe_glide(double a, double b, int i, int n);

/* THE CURSOR'S POINT, in HUD units, for a bounded record: each key at its control's HOP_HOT box
   centre on the device pixels the box is drawn on, ((snap(x0 px) + snap(x1 px)) / 2) / px, the last
   box of that key; the track at its tick along the playhead's run -- the track box inset by `ends`
   (layout.json's dock.rail.track.ends, HUDGLOBALS.track_ends) at each end -- at the box's centre y;
   leave:no at the question's No (`no_x`, `no_y` in units; NaN when the question is not drawn: the
   rest point); "" and a key no box holds at the rest point. Then the glide from one to the other.
   framecheck claims no demo names a key that does not resolve. */
void tagpu_wikiframe_point(const WIKIFRAMEREC* f, const HUDOPS* ops, float px, double ends, double rest_x, double rest_y,
                           double no_x, double no_y, double* x, double* y);

/* ---- Try it yourself's presses ----------------------------------------------------------------- */

/* Where a press landed, topmost first: the page's own controls (tagpu_wiki.c), the stage HUD's
   controls, Leave's question (`yes`, `no`, `dialog` its inert body, `outside` the frame round it). */
enum { WIKIHIT_PAGE, WIKIHIT_HUD, WIKIHIT_DIALOG };

/* THE PRESS MACHINE, the game's HUD rules (tagpu_hudinput.c, hud-in-game.md): a left press on a
   control holds it, and a button acts at its release only when the release lands on the same
   control; the track seeks at the press, again whenever the pointer's tick changes while it is
   held, and at the release; a right press is taken and does nothing; while the question is open its
   Yes and No act at their release, `outside` counts as No at once and the dialog's body does
   nothing. A key that is none of the replay's controls (the HUD's containers, uiscale, a row) is
   taken and does nothing, so a press reaches nothing but the stage's replay; no press plays a sound
   (decision 9). Each call gives at most one press for tagpu_wikireplay_press: 1 with `*control`
   (WIKI_HUD_*) and `*value` set, else 0.
   THE TRACK'S TICK `T` is the pointer's x on the playhead's run as the render thread last drew it,
   clamped to the run, times the end it was drawn with. That axis is the render thread's data and
   turns a pixel into a tick only, and tagpu_wikireplay_press clamps the tick to the game again, so a
   stale axis costs a seek to a slightly different tick, never one outside the demo. */
typedef struct {
    char held[HUD_KEYLEN];                  /* "" none; a control's key, or the question's yes / no */
    int  last;                              /* the track's tick last sought while it is held        */
} WIKIFRAMEPRESS;
void tagpu_wikiframe_press_init(WIKIFRAMEPRESS* m);
int  tagpu_wikiframe_press_down(WIKIFRAMEPRESS* m, int src, const char* key, int right, int T, int* control, int* value);
int  tagpu_wikiframe_press_drag(WIKIFRAMEPRESS* m, int T, int* control, int* value);
int  tagpu_wikiframe_press_up(WIKIFRAMEPRESS* m, const char* key, int T, int* control, int* value);

#endif
