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

   LIVE, a live entry's replay (frame.js's header states it): the player's own screen, the live HUD
   and the top bar's eased numbers, and THE ONE CURSOR -- the simulation's own while the rewind is
   idle and the viewer's hand is free, `@game` the point a trip starts from and a return glide ends
   at.

   TAKE OVER, a takeover entry's replay (frame.js's header states it): watching, the replay HUD
   with its Take over set -- the picker, the Forks bay and the lanes, the record's fork block --;
   after the takeover, a live frame as Rewind's with seat h, the seat taken, where Rewind reads
   seat 0, and the top bar in seat h's side's art (`top_side`).

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
/* the caption once Leave, then Yes has ended Try it yourself (decision 23; wiki.js S_LEFT) */
#define WIKIFRAME_LEFT    "In the game, this goes back to the Replays browser."

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
   button; when `to`, else `from`, is Back to main recording, `watched` the timeline watched at that
   trip's press, the lane it was drawn on (tagpu_wikiframe_pressed), 0 otherwise; glide step i of n,
   at `to` once i reaches n, where it stays until the next trip starts */
typedef struct {
    char    from[HUD_KEYLEN], to[HUD_KEYLEN];
    int32_t from_v, to_v, watched;
    uint8_t on, i, n, pad;
} WIKIFRAMECUR;
/* a glide's point at seat 0's cursor in the game (WIKI_REPLAY_AT_GAME), as a cursor key */
#define WIKIFRAME_AT_GAME "@game"
/* THE TOP BAR'S NUMBERS (bar.js compose's `num`): live, the replay's memo -- the eased stored energy
   and metal and the rates last sampled (tagpu_wikireplay.h) -- with the shown seat 0's storages
   and stored values; Watching's are 0, the viewer's zeroed record (decision 13) */
typedef struct { float e, m, ei, eo, mi, mo, emax, mmax, raw_e, raw_m; } WIKIFRAMEBAR;
/* THE HUMAN SEAT'S CURSOR in the shown world (seat 0's in Rewind, the seat taken over in a
   takeover's live game): its world point, its shape (WIKI_CURSOR_*) and the ticks since that shape
   began, which its animation runs from */
typedef struct {
    float   x, z;
    int32_t age;
    uint8_t shape, pad[3];
} WIKIFRAMESIM;
/* THE FORKS THE STAGE HUD LISTS: TAGPU_PK_HUD's fork part (tagpu_packet.h), the forks a file keeps
   (REC_FORK_LIMIT, the packet's TAGPU_PK_HUD_FORKS and the HUD's HUD_MAXFORK, held equal below and
   in tagpu_wikistate.c), each its id, R, last and parent's id in game ticks, the seat it took
   (0xFF none) and its flags, the packet's TAGPU_PK_FORK_* (tagpu_wikistate.c holds them equal) */
#define WIKIFRAME_FORKS          REC_FORK_LIMIT
#define WIKIFRAME_FORK_WATCHED   1u                /* the timeline watched                       */
#define WIKIFRAME_FORK_CHAIN     2u                /* a member of the watched timeline's chain    */
#define WIKIFRAME_FORK_ARMED     4u                /* its Delete is armed, while the lanes are open */
typedef char wikiframe_forks_fit[WIKIFRAME_FORKS == HUD_MAXFORK ? 1 : -1];
typedef struct {
    int32_t id, from, end, parent;
    uint8_t seat, flags, pad[2];
} WIKIFRAMEFORK;
typedef struct {
    uint8_t       on;                       /* a frame:game entry's replay is on the stage               */
    uint8_t       playing;                  /* the game is not paused and not at its end                 */
    uint8_t       question;                 /* Leave's question is open                                  */
    uint8_t       nseat;                    /* <= WIKI_SEATS                                             */
    int8_t        side;                     /* the load's layout side: the panel's, PANELSIDE's, the
                                               bottom bar's and, but in a takeover's live game, the
                                               top bar's (top_side)                                     */
    uint8_t       colour;                   /* the badge's: the human seat's colour index (seat 0 but
                                               in a takeover's live game); 0xFF none                    */
    uint8_t       speed;                    /* tenths: 5, 10, 20, 40, 80                                 */
    uint8_t       try_it;                   /* the presses are the player's: no scripted cursor          */
    int32_t       playhead, end;            /* game ticks                                                */
    WIKIFRAMESEAT seat[WIKI_SEATS];
    uint16_t      nmark, pad;
    WIKIFRAMEMARK mark[WIKIFRAME_MARKS];    /* the walker's marks, then the bookmarks                    */
    char          held[HUD_KEYLEN];         /* the HUD control held down; "" none                        */
    char          toast[HUD_TOASTLEN];      /* "" none                                                   */
    WIKIFRAMECUR  cur;
    /* LIVE (`live` 0 leaves the rest 0): the rewind's WIKI_RW_* and Play from here's WIKI_ARM_*; the
       drawn cursor is the simulation's (`simcur`, then cur.on is 0); the span's first GameTime, the
       GameTime the rewind opened at, the GameTime shown, the span's last and the arm's start (0
       while unarmed), game ticks */
    uint8_t       live, rw, arm, simcur;
    int32_t       first, G, now, last, arm_R;
    WIKIFRAMEBAR  bar;                      /* the human seat's numbers                                  */
    WIKIFRAMESIM  sim;
    /* TAKE OVER (`tk` 0 leaves the rest 0 and top_side the side): the picker, 0 closed, 1 asked or
       seeking, 2 open (TAGPU_PK_HUD's `pick`); the lanes open; the forks listed, by (R, id); the
       seat taken over, -1 while watching; the fork limit; whether the takeover's picker drew the
       limit's warning; THE TOP BAR'S SIDE (decision 15 as M0-T #2 MEASURED it: a CORE seat taken on
       an ARM load takes CORE's top bar -- its background, labels and the badge's place -- while the
       column, the minimap and the bottom bar stay the load's), the side of the seat taken over in a
       takeover's live game and `side` in every other record; the watched chain's last tick, the R
       its fill stops at, the picker's R and the fork it evicts (-1 none), game ticks */
    uint8_t       tk, pick, lanes, nfork;
    int8_t        human;
    uint8_t       fork_max, tk_full;
    int8_t        top_side;
    int32_t       chain_last, fill_to, pick_R, pick_evict;
    WIKIFRAMEFORK fork[WIKIFRAME_FORKS];
} WIKIFRAMEREC;

/* THE INDEXED CONTROLS of the Take over set, as tagpu_hudlay.c keys them: a picker row
   `takeas:<row>` (a row is its seat: the record lists the seats in order), and a lane's
   `lane:<k>`, `export:<k>` and `delete:<k>`, k its place in the record's fork list. 1 when `key` is
   one, with its control (WIKI_HUD_*) and index; the index is below WIKI_SEATS for a row and below
   WIKIFRAME_FORKS for a lane's. */
int tagpu_wikiframe_indexed(const char* key, int* control, int* index);

/* The layout side seat `seat` plays on, the top bar's in a takeover's live game: seat 0 the load's
   `side`; any other seat the opponent's, the first layout side other than `side`, which a game
   entry's rival units are cast by (tagpu_wiki.c rival_of); `side` with no layout or no other side. */
int tagpu_wikiframe_seat_side(const WIKILAYOUT* lay, int side, int seat);

/* The replay HUD's control `c` (WIKI_HUD_*, leave:yes included) by the key the HUD's hit map and
   the replay name it; "" for none. A control a demo presses is one of the WIKI_NHUD. */
const char* tagpu_wikiframe_key(int c);

/* The toast the HUD shows at demo tick rv->tick, the game's strings (tagpu_hudinput.c), into `out`
   (HUD_TOASTLEN bytes); "" for none. `seat` (`nseat` of them) names a seat a toast names (Taking
   over NAME); one past them is named "". */
void tagpu_wikiframe_toast(const WIKIREPLAYVIEW* rv, const WIKIFRAMESEAT* seat, int nseat, char* out);

/* THE FRAME RECORD from replay view `rv` and the game view `gv` it shows (NULL: no seats), the
   load's layout side `side`, `try_it` (the presses are the player's), in Try it the control the
   press machine holds (`try_held`, NULL or "" none), and `top_side`, the layout side of the human
   seat (tagpu_wikiframe_seat_side), which the record takes only in a takeover's live game. In a
   live game `gv` is the human seat's view (tagpu_wikisim_view_as). THE LIFETIME: called after the
   fill's last tagpu_wikireplay_step or _press, because `gv` points into the game simulation's view
   arrays, which the next step or press may free; the record holds values only, so it outlives
   them. The cursor's glide is on the viewer's clock, the toast's age on the demo tick (frame.js,
   THE GLIDE). */
void tagpu_wikiframe_record(const WIKIREPLAYVIEW* rv, const WIKIVIEW* gv, int side, int try_it, const char* try_held,
                            int top_side, WIKIFRAMEREC* out);

/* THE BOUND, on the render thread's own copy before anything reads it: the record is DATA until
   here. Flags to 0/1; nseat to WIKI_SEATS and nmark to WIKIFRAME_MARKS; end to 1..WIKI_GAME_END_MAX
   and the playhead to 0..end; a mark's tick to 0..end, a mark of an unknown kind or a seat outside
   -1..nseat - 1 dropped; the speed one of WIKI_REPLAY_SPEEDS, else 10; the side below `nside`, else
   -1; the badge's colour below `nlogo`, else none (0xFF); a seat's colour below `nlogo` or below
   WIKI_SEATS (the whitebox table), else 0xFF (grey); every float finite and within WIKI_ECON_MAX;
   every string terminated in its array; the held key and the cursor's keys among the replay's
   controls or the indexed ones (tagpu_wikiframe_indexed; a held key that is neither becomes "", a
   cursor key that is neither turns the cursor off), and the cursor's n in 1..64 with i <= n, else
   the cursor off; its watched to 0..RECR_TIMELINES - 1, else 0. Live: the flags to 0/1, rw and arm to their enums, else idle and none; first, G,
   now, last and arm_R to 0..WIKI_GAME_END_MAX; every bar float finite and within WIKI_ECON_MAX; the
   cursor's point finite and within the frame's largest map and column, its shape a WIKI_CURSOR_*
   and its age 0 or more, else simcur off. Take over: tk, lanes and tk_full to 0/1, and a record
   that is not tk has its whole block 0 and top_side its side; pick to 0..2, else 0; human to
   -1..nseat - 1, else -1; fork_max to WIKIFRAME_FORKS; top_side below `nside` in a live record,
   else the side; nfork to WIKIFRAME_FORKS, a fork whose id is outside 1..RECR_TIMELINES - 1
   dropped, its from and end to 0..end, its flags to their three bits and a seat outside
   0..nseat - 1 to 0xFF (named "Unknown seat", as the game's HUD names one); chain_last, fill_to
   and pick_R to 0..end; pick_evict -1 or an id of 1..RECR_TIMELINES - 1, else -1. */
void tagpu_wikiframe_bound(WIKIFRAMEREC* f, int nside, int nlogo);

/* ---- the watcher's view ------------------------------------------------------------------------ */

/* THE IDLE COLUMN of layout side `side`: its MAIN2 with nothing selected, a watcher's screen (R0:
   the viewer controls the spectator record, whose unit list is empty). Each gadget's state is
   sim.js gadgetState(g, null, false)'s: hidden -1; the orders and build tabs, the build buttons,
   STOP and the orders 2 (greyed); anything else 0. framecheck pins this second copy of that rule
   against a fresh simulation's MAIN2. 1 into `out`, or 0 when the layout has no such side. */
int tagpu_wikiframe_idle_side(const WIKILAYOUT* lay, int side, WIKISTAGESIDE* out);

/* THE FRAME'S VIEW, in place, of a copy (tagpu_wikistage_copy) of the replay's game view, by `mode`
   (frame.js view states each):
   WIKIFRAME_WATCH, the watcher's: the frame flag set, the fog all clear (the full-map reveal sets
   the watcher's sight and explored bit in every cell, tagpu_replay.c), seat 0's cursor, ghost,
   sites, queued sites, lines, drag, sweep, held keys and every selection gone (the viewer is not
   seat 0), and the column the idle MAIN2 of `side` in `lay`, which `layout_src` (WIKISTAGE_LAYOUT_*)
   names to the render thread, whatever the game script's sidebar says: that key governs seat 0's
   recorded inputs, and the frame is the watcher's screen.
   WIKIFRAME_PLAYER, the live rewind idle or open: the view as the simulation made it (MEASURED, M0
   #1), the ghost only with `cursor` (the simulation's cursor is the one drawn), the copy being of
   seat `human`'s view, and the selection seat `human`'s alone: a unit's selected flag is set only
   by its owner's seat, and the view carries every seat's.
   WIKIFRAME_PAST, the live rewind's past: the past simulation's world and fog under the column the
   copy carries, which the caller has copied from the live simulation's view (tagpu_wikistage_side)
   with no unit under it; nothing selected, no ghost, sites, queued sites, cursor, lines, drag, sweep
   or keys (MEASURED, M0 #2).
   No frame view keeps a cursor of its own. A live view without a column takes the idle MAIN2 of
   `side`; none at all when the layout has no such side. */
enum { WIKIFRAME_WATCH, WIKIFRAME_PLAYER, WIKIFRAME_PAST };
void tagpu_wikiframe_view(WIKISTAGEVIEW* v, const WIKILAYOUT* lay, int side, int layout_src, int mode, int cursor, int human);

/* ---- the stage HUD ---------------------------------------------------------------------------- */

/* A seat's colour, 0xRRGGBB, by its colour index `k`: the palette colour of the middle pixel of
   logos frame k, as the game's HUD takes it (tagpu_replay.c logo_colour: pixel (w / 2, h / 2)), or
   without that frame the whitebox table (frame.js COLOURS), grey past it. */
uint32_t tagpu_wikiframe_seat_colour(const struct WIKISIDE* sd, int k);

/* THE STAGE HUD'S STATE from a bounded record in a frame Wg x Hg, at `px` device pixels a unit,
   hud.js's state() as frame.js hudState makes it: a screen of the frame's own size at 100 % (Q3),
   the engine frame the column and both bars, every switch on and nothing folded as a fresh Watch,
   each seat a team of one, no sight, no follow, no forks, no pointer (mx, my -1), the record's held
   control and toast; a live record the live HUD as tagpu_hudstate.c present_live sets it. `sd` the
   published sidebar art for the seats' colours, or NULL. A seat is out, its row dimmed and its
   picker row refused, while it has no unit: the game's rule (tagpu_hudstate.c, `out` is units ==
   0), which the takeover refuses by too (tk_bound).
   A TAKEOVER'S WATCHING, a tk record that is not live, as tagpu_hudstate.c `forks` and `present`
   set the game's from the packet, with the track's start at tick 0 and its axis the end (every
   chain's last lies within it): the time text's second clock the watched chain's last, the fill
   to fill_to, the playhead held to the chain's last, the forks in seconds named by their seats,
   the watched one, the armed Delete and, while the picker is open, the one it would replace by
   their places, the picker drawn while open and the Take over button down while asked, sought or
   open, the lanes as open as the record says and fork_max the record's.
   THE CHART: the stats window stays closed, so no build of this state emits HOP_CHART. That is what
   keeps the HUD pass's one chart image the replay HUD's; the stage must have its own before it
   opens the window (P). */
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

/* SEAT 0'S CURSOR in HUD units for a bounded record in the frame fitted as `fit`, its corner at the
   canvas point (fx, fy) and `px` device pixels a unit (frame.js gamePoint); NaN for a record that
   is not live. */
void tagpu_wikiframe_game_point(const WIKIFRAMEREC* f, const WIKISTAGEFIT* fit, double fx, double fy, float px, double* x,
                                double* y);

/* THE HUD THE TRIP PRESSED (frame.js pressed), the record whose HUD the cursor's point is resolved
   among: for a return glide from a picker row (a takeover's), `f` as the replay HUD with the picker
   open, its fork list placeholders as many as make the picker draw the limit's warning when the
   takeover's did (tk_full) -- a row lies where the seat count and the warning put it
   (tagpu_hudlay.c picker) and nowhere else; for any other return glide, which leaves a control of
   the strip its press closed, `f` with the strip open over the past, fwd live (`now` at the
   strip's first) and nothing armed, the HUD as drawn at that press -- its controls lie where they
   lie whatever the playhead or the arm; for a takeover's watching whose trip goes to or leaves
   Back to main recording, pressed on a lane the list holds and no longer watches (cur.watched),
   `f` with the lanes open and that fork the watched one, the lanes as drawn at that press:
   watching a lane is what draws its Back to main recording and widens the lane to hold it
   (tagpu_hudlay.c lanes), the other flags only fill; for any other record `f` itself. Its boxes
   only: the HUD drawn is `f`'s own. 1 when `out` differs from `f`. */
int tagpu_wikiframe_pressed(const WIKIFRAMEREC* f, WIKIFRAMEREC* out);

/* THE CURSOR'S POINT, in HUD units, for a bounded record: each key at its control's HOP_HOT box
   centre on the device pixels the box is drawn on, ((snap(x0 px) + snap(x1 px)) / 2) / px, the last
   box of that key; the track at its tick along the playhead's run -- the track box inset by `ends`
   (layout.json's dock.rail.track.ends, HUDGLOBALS.track_ends) at each end, the run the game's
   [0, end] or live the strip's [first, G] -- at the box's centre y; leave:no at the question's No
   (`no_x`, `no_y` in units; NaN when the question is not drawn: the rest point); @game at seat 0's
   cursor (`game_x`, `game_y`, tagpu_wikiframe_game_point; NaN: the rest point); "" and a key no box
   holds at the rest point. Then the glide from one to the other. `ops` is the HUD of
   tagpu_wikiframe_pressed's record. framecheck claims no demo names a key that does not resolve. */
void tagpu_wikiframe_point(const WIKIFRAMEREC* f, const HUDOPS* ops, float px, double ends, double rest_x, double rest_y,
                           double no_x, double no_y, double game_x, double game_y, double* x, double* y);

/* A FRAME POINT OF THE WORLD (frame.js onWorld), world units: on the column, x in [-128, 0) and z in
   [-32, Hg - 32), or on the map, x in [0, mw) and z in [0, mh); a bar, or the viewport below a map
   shorter than the column, is none. */
int tagpu_wikiframe_on_world(double x, double z, int Hg, int mw, int mh);

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
   THE INDEXED CONTROLS (tagpu_wikiframe_indexed) are buttons too: a picker row's release gives
   takeas with the row, a lane's control's its control with the fork the lane showed. `lane` (`nlane`
   of them, NULL for none) is the fork ids of the lanes the pressed frame drew, in their keys' order
   (tagpu_hudstate.c fills the game's `lane_id` the same way); a lane's control is held only when
   it names a lane drawn, and acts at its release only when the lanes then drawn name the same fork
   at that place: a list that changed under the pointer between the two -- a deletion's, a fork
   made -- never sends the release to the fork that moved into its place (tagpu_hudinput.c
   held_lane, the game's rule). The id is DATA: the replay refuses one that is not a live fork.
   THE TRACK'S TICK `T` is the pointer's x on the playhead's run as the render thread last drew it,
   clamped to the run, times the end it was drawn with. That axis is the render thread's data and
   turns a pixel into a tick only, and tagpu_wikireplay_press clamps the tick to the game again, so a
   stale axis costs a seek to a slightly different tick, never one outside the demo. */
typedef struct {
    char    held[HUD_KEYLEN];               /* "" none; a control's key, or the question's yes / no */
    int     last;                           /* the track's tick last sought while it is held        */
    int32_t lane;                           /* a lane's control held: the fork its lane showed      */
} WIKIFRAMEPRESS;
/* The lanes a bounded record's stage HUD draws, in their keys' order: the fork ids of its list
   while it watches a takeover's replay with the lanes open, none otherwise. Their count into
   `lane` (WIKIFRAME_FORKS ids). */
int  tagpu_wikiframe_lanes(const WIKIFRAMEREC* f, int32_t* lane);
void tagpu_wikiframe_press_init(WIKIFRAMEPRESS* m);
int  tagpu_wikiframe_press_down(WIKIFRAMEPRESS* m, int src, const char* key, int right, int T, const int32_t* lane, int nlane,
                                int* control, int* value);
int  tagpu_wikiframe_press_drag(WIKIFRAMEPRESS* m, int T, int* control, int* value);
int  tagpu_wikiframe_press_up(WIKIFRAMEPRESS* m, const char* key, int T, const int32_t* lane, int nlane, int* control,
                              int* value);

#endif
