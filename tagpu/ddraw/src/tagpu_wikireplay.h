#ifndef TAGPU_WIKIREPLAY_H
#define TAGPU_WIKIREPLAY_H
/* tagpu_wikireplay -- the in-game wiki's replay layer: a game entry's demo
   (research/notes/ingame-wiki-replay-plan.md) played the way the replay HUD plays a recording. A port
   of the mockup's tools/wiki/mockup/replay.js, function for function and under its names; that
   file's header is the specification of both. It holds the viewer's clock and the HUD's state and
   owns the game simulations (tagpu_wikisim.h, WIKI_PLAY_GAME); the simulation knows nothing of the
   HUD. tools/wiki/simparity.mjs compares the two demo tick by demo tick, through
   tagpu_wikireplay_hash and each game's tagpu_wikisim_hash, and its press trace compares Try it's.

   In short: the game script is played by a game simulation on game ticks, the viewer's script
   (captions, the `seeked` wait and the HUD's presses) on demo ticks, one a step, moving the game
   only through the controls -- paused, a speed, a seek. Before the replay shows, a second game
   simulation, the walker, plays the script to its end WALK ticks a step and marks each seat's
   commander deaths and its defeat. A seek re-simulates: forward the game plays on, back it is
   freed, made again and plays from tick 0. Leave, then Yes, ends Try it yourself's replay: it is
   done and `left`.

   THE BOUND: a step runs at most max(WIKI_REPLAY_SEEK, WIKI_REPLAY_WALK, 8) game ticks, whatever
   the script or the presses, so a step's cost has a ceiling by construction. Every value a press
   or the data brings is bounded before it is used: a control against WIKI_NPRESS, a tick clamped to
   the game, the markers and bookmarks against their arrays.

   LIVE, a live entry's demo (WIKIENTRY.live): a live skirmish with the live HUD's rewind, the game's
   own rewind state for state with its waits removed. replay.js's header states it -- the live and
   past simulations, the chain's re-simulation, the arm, the snap by the recorder's refresh grid
   and the eviction by its function, both called from tagpu_recrules.h itself -- with:
   THE LIFETIME, LIVE: the replay owns the live simulation L (`game`) and the past one P (`past`)
   and is the only code that frees them: P at a backward seek before the new one is made, at Back
   to now, and at the end; L at a fork, the moment P takes its place, and at the end. `game` is
   replaced the moment its simulation is freed. L is never stepped or written while the past is
   shown, so Back to now shows its world exactly as it was; the release that follows is the game's
   own (m0-facts.md) and is logged like an input.
   THE BOUND, LIVE: a step runs at most max(WIKI_REPLAY_SEEK, 1) game ticks and one fork operation
   per chain member P crosses; the timelines are at most RECR_TIMELINES, the plan refusing the one
   past it; the logs hold at most WIKI_REPLAY_LOG entries over every kept log, a deleted timeline's
   freed once no live timeline's chain reads it, and the idle rewind keeps WIKI_REPLAY_LOG_KEEP of
   them back -- a key's up for each key the world can hold, and the release -- and, while open, one
   for the release; each log's block grows before it is needed (log_room), so leaving idle and Back
   to now can always log what they apply.
   LOGGED FIRST: Try it's world input after a fork is written to the selected timeline's log and
   then applied through the function a re-simulation applies it with; one that cannot be logged is
   not applied, so every fork can be made again exactly.
   ONE CURSOR: in a scripted live demo, while the viewer's trip or its return glide runs and the
   rewind is idle, L feeds the human seat no input; a step that does sets `clash` and ends the
   replay.

   TAKE OVER, a takeover entry's demo (WIKIENTRY.takeover): a replay with the replay HUD's Take
   over, the game's own state for state with its waits removed. replay.js's header states it --
   watch mode on the selected timeline's chain, the picker as pick_fill, the switch's two paths and
   why the re-chained one is exact, the export's no-overwrite rule, the deletion through the
   nearest live ancestor, the takeover into live mode with the seat taken as the human -- with:
   THE BOUND, WATCH: a switch restarts the walker on the new chain without holding the clock; a
   step runs the game's part first and gives the walker what is left of WIKI_REPLAY_WALK, so it
   still runs at most max(WIKI_REPLAY_SEEK, WIKI_REPLAY_WALK, 8) game ticks, plus one fork
   operation per chain member crossed. The timelines are at most RECR_TIMELINES (the plan refuses
   the one past it), the forks saved at most as many, each once.
   THE LIFETIME, WATCH: a sought switch frees `game` before the new one is made, as a backward seek
   does, and a switch frees the walker before the new one is made; the takeover frees the walker,
   which live mode has none of. A kept fork's part points into the entry's data, only read.

   PLAYERS, a players entry's demo (WIKIENTRY.players): a replay with the replay HUD's Players set
   -- the viewer's camera, moved by Jump and Follow alone (each seat's camera is recorded data,
   tagpu_wikidata_seat_cam), Sight, the overlays' switches, the team rows' folds and the stats
   window -- with each step's tail after the game's part, paused or not: the camera, a seek's end
   and a Sight request, the minimap's pass, the top bar. replay.js's header and its functions
   state the rules. The walker samples every seat's measures as it walks.
   THE BOUND, PLAYERS: a tail costs one pass at most (a lit grid and the contacts), one ease and
   one re-read; the samples are at most WIKI_STAT_PTS; the pass's contacts are the view's units
   at most, in a block the replay owns and frees. The Players set is inert in every other replay:
   tagpu_wikireplay_press takes it only in a players replay, and the loader refuses it in any
   other entry's script.

   ENGINE-FREE, WIN32-FREE AND WITHOUT GLOBALS, as tagpu_wikisim.c: one WIKIREPLAY is used by one
   thread at a time, and it lives as long as its caller keeps it. The DLL compiles this file for
   SSE2 with the simulation (Makefile, THE WIKI'S SIMULATION): it computes on integers, its one
   double the demo's end the clock is compared with exactly, and shares the simulation's build. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_hudlay.h"
#include "tagpu_recrules.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikisim.h"

/* replay.js's constants, compared by simparity.mjs: the game ticks a seek and the walk play in
   one step, rew's and fwd's 10 s and a bookmark's reach of 3 s (hud-in-game.md's keys), the
   bookmarks held, the markers (a death per kill of a commander, a defeat per seat: a seat's count
   never rises again, there is no transfer), the most game ticks a page's fill lets a replay run,
   the speeds in tenths (1/2x to 8x) and the one a replay starts at, and a press's trip: the popup
   pick's without its glide back.
   SEEK, WALK AND FILL ARE SET FROM THE TICK'S COST IN THE GAME (MEASURED 2026-10-31 by
   tagpu_wiki.check on every setup of the compatibility suite, the DLL's i686 build under Wine): a
   tick costs 1.5..3.4 us on average everywhere but Escalation, where it costs 11.8..15.5 us and a
   256-tick step up to 7.8 ms. Escalation's commanders' scripts scan every unit id from 1 to
   MAX_ID (WIKICOB_UNITS, 1024) for its height, team, build left and alliance every 2.5..5 s, so
   about one tick in 55 costs 0.7..1.4 ms natively (gcc -O2, MEASURED with Watching's demos): the
   cost of a fill is those ticks, not the tick count. 64 ticks a fill bounds Escalation's worst
   fill at 1.75 ms natively (4.1 ms at 256) and still plays a seek to the end of the longest
   authored demo (1800 game ticks) in 29 steps, about a second at 1x as decision 11 asks; a seek to
   WIKI_GAME_END_MAX takes 85, under three seconds, and plays on while the stage holds the last
   world drawn. */
#define WIKI_REPLAY_SEEK     64
#define WIKI_REPLAY_WALK     64
#define WIKI_REPLAY_STEP     300
#define WIKI_REPLAY_NEAR     90
#define WIKI_REPLAY_BOOKS    64
#define WIKI_REPLAY_MARKS    (WIKI_GAME_KILLS + WIKI_SEATS)
#define WIKI_REPLAY_FILL     64
#define WIKI_REPLAY_NSPEED   5
#define WIKI_REPLAY_SPEEDS   { 5, 10, 20, 40, 80 }
#define WIKI_REPLAY_SPEED0   10
#define WIKI_REPLAY_HUD_DOWN WIKI_PICK_DOWN
#define WIKI_REPLAY_HUD_UP   WIKI_PICK_UP

/* a toast's kind, replay.js's TOASTS: a bookmark's two, then the live HUD's five (tagpu_hudinput.c's
   strings: "Playing on from m:ss", "Cannot play from m:ss", "Click again to play from m:ss" with
   ", replacing F<n>", "Playing from m:ss", "Rewind refused"), then the Take over set's six
   ("Taking over NAME at m:ss", "Cannot take over at m:ss", "F<id> saved as a replay of its own",
   "Cannot save F<id>", "Click again to delete F<id>", "F<id> deleted"), then the Players set's
   "Camera was not recorded" (tagpu_hudinput.c:800, Jump or Follow on a seat with no camera); a
   marker's is HUD_MK_DEATH or HUD_MK_DEFEAT */
enum { WIKI_TOAST_ADDED, WIKI_TOAST_REMOVED, WIKI_TOAST_PLAYING_ON, WIKI_TOAST_CANNOT, WIKI_TOAST_ARMED,
       WIKI_TOAST_PLAYING_FROM, WIKI_TOAST_REFUSED, WIKI_TOAST_TAKING_OVER, WIKI_TOAST_CANNOT_TAKE, WIKI_TOAST_SAVED,
       WIKI_TOAST_CANNOT_SAVE, WIKI_TOAST_CLICK_DELETE, WIKI_TOAST_DELETED, WIKI_TOAST_CAMERA };

/* TAKE OVER's names (replay.js PICKS, MODES): the picker's states (tagpu_hudinput.c PICK_CLOSED,
   _ASKED, _SEEKING, _OPEN) and a takeover replay's modes, watching the recording and the live game
   its takeover made */
enum { WIKI_PICKER_CLOSED, WIKI_PICKER_ASKED, WIKI_PICKER_SEEKING, WIKI_PICKER_OPEN };
enum { WIKI_TK_WATCH, WIKI_TK_LIVE };

/* THE LIVE MODE's names and bounds (replay.js RW, ARMS, FIRST, RATE, LOG, LOG_KEEP, AT_GAME): the
   rewind's states (tagpu_replay.c's IDLE, OPEN and PAST; its FLUSH and ENDING are instantaneous
   here) and Play from here's arm (tagpu_hudinput.c); the span's first GameTime, a recording's first
   iteration (demo-recorder-s4-rewind.md); the DisplayTimer's step of 30 GameTimes (engine/render.md,
   The memo's layout); the logs' bound, about 24 bytes an entry, and what the idle rewind keeps back
   of it; a log entry's kind; a trip's start at seat 0's cursor in the game, a cursor's `from` */
enum { WIKI_RW_IDLE, WIKI_RW_OPEN, WIKI_RW_PAST };
enum { WIKI_ARM_NONE, WIKI_ARM_SEEKING, WIKI_ARM_ARMED };
enum { WIKI_LOG_MOVE, WIKI_LOG_DOWN, WIKI_LOG_UP, WIKI_LOG_RELEASE };
#define WIKI_REPLAY_FIRST    1
#define WIKI_REPLAY_RATE     30
#define WIKI_REPLAY_LOG      65536
#define WIKI_REPLAY_LOG_KEEP (WIKI_NKEYS + 1)
#define WIKI_REPLAY_AT_GAME  (-2)

/* THE PLAYERS STATE's names and bounds (replay.js MEASURES, MEASURE0, STAT_PTS, TOG_ALL, NOT_ASKED,
   PASS): the stats window's seven measures, the chips from WIKI_CHIP_METAL on in their order (a
   measure's index is its chip's less WIKI_CHIP_METAL) and the one a replay starts on; the samples
   a seat keeps, one at the walk's first tick and one every 30 GameTime to WIKI_GAME_END_MAX, within
   the game HUD's series (HUD_MAXPT, TAGPU_PK_HUD_POINTS); the four overlay switches, all on at a
   replay's start; a Sight request's "none" (-1 is the full map); the radar pass's 30-tick
   deadline; the pass's fog bits, one a block of the sight grid. */
#define WIKI_STAT_MEASURES     (WIKI_NCHIP - WIKI_CHIP_METAL)
#define WIKI_STAT_MEASURE0     (WIKI_CHIP_UNITS - WIKI_CHIP_METAL)
#define WIKI_STAT_PTS          (WIKI_GAME_END_MAX / WIKI_REPLAY_RATE + 1)
#define WIKI_REPLAY_TOG_ALL    (HUD_TOG_CURSORS | HUD_TOG_CLICKS | HUD_TOG_SELECTIONS | HUD_TOG_CHAT)
#define WIKI_REPLAY_NOT_ASKED  (-2)
#define WIKI_REPLAY_PASS       30
#define WIKI_REPLAY_FOG        ((WIKI_SIGHT_GRID + 7) / 8)
_Static_assert(WIKI_STAT_PTS <= HUD_MAXPT, "a players replay's samples fit the game HUD's series");

/* An entry of a timeline's log: applied live between two ticks, applied again by every
   re-simulation at the same point. tau the tick it first acts on (the simulation stands at tau - 1);
   kind WIKI_LOG_*; key a WIKI_KEY_* for DOWN and UP; seat the human seat whose input or release it
   is; x, z a MOVE's world point. */
typedef struct {
    int32_t tau;
    int8_t  kind, key, seat;
    double  x, z;
} WIKIREPLAYLOG;

/* A TIMELINE: the original (id 0) or a fork, in creation order, its index its id. parent -1 for the
   original; R its first tick; last the span's last when it stopped being the selected one (a
   takeover replay's original and kept forks: the tick its chain plays to); live 0 once deleted;
   used the demo tick it was made or left on (the recorder's last-used stamp); mode WIKI_ECON_*;
   seat the seat its fork operation takes; part the fork part's or kept fork's inputs in the
   entry's data (only read, NULL for the original and Try it's); the log on the heap, the replay's
   own. */
typedef struct {
    int                id, parent, R, last, live, used, mode, seat;
    const WIKIINPUT*   part;
    int                npart;
    WIKIREPLAYLOG*     log;
    int                nlog, cap;
} WIKITIMELINE;

/* THE REPLAY'S VIEW, replay.js's R.view(): values only, no pointer into the replay but the
   caption, which points into the entry's document. */
typedef struct {
    int         tick, clock, done;
    int         waiting;            /* WIKI_UNTIL_SEEKED while the viewer's wait holds the clock, else 0 */
    int         stalled;            /* the wait ran past its max: the demo stopped                     */
    const char* caption;            /* NULL for none                                                    */
    int         failed;             /* a backward seek could not make the game again: the replay stopped */
    int         ignored;            /* the presses Leave's question ignored                             */
    /* the game's transport: its tick and end, the playhead (the target while a seek is pending),
       paused, the speed in tenths and its accumulator, the target (-1 none), the simulation's
       generation and the sound total heard already */
    int         game_tick, end, playhead, paused, speed, acc, target, gen, quiet;
    /* the viewer's cursor, the last trip (has_cursor 0 before the first): the previous trip's
       control (-1 none), final tick and `watched`, this one's control and tick (-1 for a button),
       the timeline watched at its press (replay.js's cursor states why that is the one selected
       at its start), the clock tick it began, and its drag (drag -1 none); `held` the control
       between its press and release, -1 none */
    int         has_cursor, from, from_value, from_watched, to, value, watched, at, drag, over, held;
    int         nmark;
    int         mark[WIKI_REPLAY_MARKS][3];     /* tick, HUD_MK_*, seat                              */
    int         walked;                         /* the walker's tick, the end once the walk is over  */
    int         nbook;
    int         book[WIKI_REPLAY_BOOKS];        /* sorted                                            */
    int         toast_kind, toast_at, toast_value;  /* toast_kind -1: none                          */
    int         toast_seat;                     /* the seat a toast names (Taking over), -1 none     */
    int         question, was_paused;           /* Leave's question is open, and the pause before it */
    int         left;                           /* Try it's Yes left the replay: it is done          */
    /* LIVE (replay.js's view's `live`; `live` 0 leaves the rest 0): the rewind's WIKI_RW_*, the
       GameTime it opened at, the span (first, last) and the GameTime shown; the WIKI_ARM_*, its start
       and the fork it evicts (-1 none); the track's last seek (-1 none); the clash; the entries the
       logs hold; the selected timeline and the timelines; the past simulation (has_past 0 none), its
       tick, the seek's T (-1 none) and its generation; the top bar's memo, f32 as the engine's, and
       the DisplayTimer; the return glide's start (-1 none); whether Try it's input is past what the
       logs keep; the toast's evicted fork (-1 none, and in a takeover replay's watch mode); the human
       seat. The timelines (ntl, tl) are a takeover replay's in either mode too. */
    int         live;
    int         rw, G, first, last, now, arm, arm_R, arm_evict, drag_t, clash, nlogged, selected, ntl;
    struct { int id, parent, R, last, live, used, mode, nlog, seat; } tl[RECR_TIMELINES];
    int         has_past, past_tick, past_target, past_gen;
    float       be, bm, rates[4];               /* rates: energy in, out; metal in, out          */
    int         dt, back, full, toast_evict, human;
    /* TAKE OVER (replay.js's view's `tk`; `tk` 0 leaves the rest 0): the WIKI_TK_* mode; the human
       seat (-1 while watching); the WIKI_PICKER_* state and its plan (R, the fork it evicts or -1,
       the parent, the timeline and the serial it was made on); the lanes open; the fork armed for
       deletion (0 none); the selected timeline's chain's last and the tick the track fills to (the
       R of its member whose parent is the original, else the end); the rows' serial; the forks
       saved; whether the walker walks in the background and its tick (-1 none); whether the
       takeover's picker drew the fork limit's warning */
    int         tk, tk_mode, pick, pick_R, pick_evict, pick_parent, pick_tl, pick_serial, lanes, armed;
    int         chain_last, fill_to, serial, nsaved, saved[RECR_TIMELINES], bg, walker_tick, tk_full;
    /* PLAYERS (replay.js's view's `players`; `players` 0 leaves the rest 0): the viewer's camera, a
       view's centre in world units; Follow's seat and the Jump asked (-1 none); Sight's seat (-1 the
       full map) and the one asked (WIKI_REPLAY_NOT_ASKED none); the overlay switches (HUD_TOG_*);
       the folded team rows, a bit each; the stats window open, its measure (an index of the seven,
       WIKI_STAT_MEASURES) and its split by team; the tick a click must be at or after to show; the
       shown tick and whether a seek's end is due; the top bar's memo, f32 as the engine's, and each
       record's DisplayTimer, the seats' then the watcher's; the pass: its tick (-1 none), its
       blocks (0 under the full map), the fog bits (block i lit is bit i & 7 of byte i >> 3) and the
       ids of the units its contacts mark -- `pass_contact` points into the replay, valid until the
       next tagpu_wikireplay_step, _press or _free; the samples' count, the seats each holds (the
       scene's), base and period (tagpu_wikireplay_stat reads one). */
    int         players;
    int         cam_x, cam_z, follow, jump_req, sight, sight_req, tog, folded;
    int         stat_open, stat_measure, stat_team, click_from, shown, pend;
    float       pbe, pbm, prates[4];
    int         pdt[WIKI_SEATS + 1];
    int         pass_at, pass_blocks;
    uint8_t     pass_fog[WIKI_REPLAY_FOG];
    int         npass_contact;
    const char (*pass_contact)[WIKI_ID_MAX];
    int         npt, nseat, base, period;
} WIKIREPLAYVIEW;

/* The replay; the caller owns it and reads nothing of it but through the functions below. */
typedef struct {
    /* what it plays, kept for the backward seek's new game: the lifetimes are
       tagpu_wikisim_init's, and the context's castings and sources must outlive the replay too,
       since a backward seek reads them again */
    const WIKIENTRY*  e;
    const WIKIDEMO*   d;
    const WIKILAYOUT* lay;
    WIKISIMCTX        ctx;
    int               has_ctx;
    /* THE LIFETIME: each one the replay's alone, on the heap; NULL once freed (game only when
       `failed`, walker once the walk is over) */
    WIKISIM*          game;
    WIKISIM*          walker;
    int               tick, clock, next, nscript, done, failed, ignored, stalled, scripted, left;
    const WIKIINPUT** script;       /* the viewer's inputs in the order fed: by tick, then file order */
    double            vend;         /* the clock the demo ends on; never for Try it                    */
    int               waiting, wait_from, wait_max;
    const char*       caption;
    int               end, paused, speed, acc, target, gen, quiet;
    int               has_cursor, from, from_value, from_watched, to, value, watched, at, drag, over, pos;
    int               nbook, book[WIKI_REPLAY_BOOKS];
    int               toast_kind, toast_at, toast_value;
    int               question, was_paused;
    int               walked, nmark, mark[WIKI_REPLAY_MARKS][3];
    int               prev_lost[WIKI_SEATS], prev_out[WIKI_SEATS];
    /* LIVE (replay.js's live state): `game` is L; `past` P (NULL none), its generation, its chain
       (row indices from the original), the member it plays and that member's next log entry; the
       rewind's state, the GameTime it opened at and the span's last; the track's last seek; the
       clash; the return glide's start; the arm, its start and its eviction; the timelines, the
       selected one and the entries every log holds; `made` numbers each simulation made; the top
       bar's memo and DisplayTimer, and the shown world's figures it eases toward (s_*: the shown
       tick, the human seat's stored values, storage and rates); the toast's evicted fork and seat;
       the human seat (a live entry's 0) */
    int               live;
    WIKISIM*          past;
    int               pgen, npchain, pm, pl, pchain[RECR_TIMELINES];
    int               rw, G, last, drag_t, clash, back, arm, arm_R, arm_evict;
    WIKITIMELINE      tl[RECR_TIMELINES];
    int               ntl, selected, nlogged, made;
    float             be, bm, rates[4];
    int               dt, s_tick;
    double            s_e, s_m, s_emax, s_mmax, s_ei, s_eo, s_mi, s_mo;
    int               toast_evict, toast_seat, human;
    /* TAKE OVER (replay.js's takeover state; `tk` a takeover entry's replay, `live` set from its
       takeover on): the picker's WIKI_PICKER_* and its plan; the rows' serial, raised by a
       deletion; the lanes open; the fork armed for deletion (0 none); the forks saved, each once;
       whether the walker walks in the background; whether the takeover's picker drew the warning */
    int               tk, pick, pick_R, pick_evict, pick_parent, pick_tl, pick_serial;
    int               serial, lanes, armed, nsaved, saved[RECR_TIMELINES], bg, tk_full;
    /* PLAYERS (replay.js's players state; `players` a players entry's replay): the view's fields of
       the same names, with the shown world's figures the top bar eases toward (p_*: the viewed
       seat's stored values, storage and rates, 0 under the full map); the pass's contacts' ids in a
       block of cpass_id the replay owns (THE LIFETIME: grown by make_pass, freed by
       tagpu_wikireplay_free, never shrunk); the samples, sv[n][k] seat k's seven measures in
       WIKI_STAT_MEASURES order, for the scene's seats, stat_nseat of them */
    int               players;
    int               cam_x, cam_z, follow, jump_req, sight, sight_req, tog, folded;
    int               stat_open, stat_measure, stat_team, click_from, shown, pend;
    float             pbe, pbm, prates[4];
    int               pdt[WIKI_SEATS + 1];
    double            p_e, p_m, p_emax, p_mmax, p_ei, p_eo, p_mi, p_mo;
    int               pass_at, pass_blocks;
    uint8_t           pass_fog[WIKI_REPLAY_FOG];
    char            (*pass_id)[WIKI_ID_MAX];
    int               npass_id, cpass_id;
    int               npt, pbase, period, stat_nseat;
    float             sv[WIKI_STAT_PTS][WIKI_SEATS][WIKI_STAT_MEASURES];
} WIKIREPLAY;

/* replay.js's play (`scripted` WIKI_PLAY_DEMO: the viewer's script plays) or create (WIKI_PLAY_TRY:
   Try it yourself, the presses the player's). The demo must be a game entry's; the game and the
   walker are made as tagpu_wikisim_init makes a game (WIKI_PLAY_GAME), the walker without a sound
   source, and the arguments must live as long as tagpu_wikisim_init asks, and the context's
   castings and sources as long as the replay. 0 with a message in `err` when the simulation is
   refused or memory runs out; then `r` holds nothing to free. */
int tagpu_wikireplay_init(WIKIREPLAY* r, const WIKIENTRY* e, const WIKIDEMO* d, int scripted, const WIKILAYOUT* lay,
                          const WIKISIMCTX* ctx, char* err, size_t errn);

/* One demo tick. Nothing once it is done. */
void tagpu_wikireplay_step(WIKIREPLAY* r);

/* The game ticks the next step runs at most: the ticks the state says, or, when the viewer's trip
   acts on the step's clock tick, the most any step runs (a press there may start a seek the step
   plays). Never fewer than the step runs. */
int tagpu_wikireplay_cost(const WIKIREPLAY* r);

/* Try it yourself's press, acting at once: a replay HUD's WIKI_HUD_*, the question's Yes included,
   a takeover replay's watch mode also its Take over set (tagpu_wikidata_hud_takeover), a players
   replay's also the Players set (tagpu_wikidata_hud_players) and WIKI_HUD_FOLD, or in a live
   replay a live HUD's (tagpu_wikidata_hud_presses; anything else does nothing); for the track the
   game tick under the pointer, which the seek clamps to the game (live, to the rewind's span), for
   a picker row its seat, for a lane's controls the fork's id, for a players row's Jump, Sight and
   Follow its seat, which must be one of the scene's, for a chip its WIKI_CHIP_* and for a fold the
   team row's index, below WIKI_SEATS (a value outside these is refused at the door, so a hit map a
   frame old can name nothing outside the replay's). Nothing while the replay is done or still
   walking before it shows (a background walk is no such walk). */
void tagpu_wikireplay_press(WIKIREPLAY* r, int control, int value);

/* Try it yourself's world input in a live replay (replay.js R.input), given between two steps: a
   MOVE, else a `down`, else an `up` of a WIKI_KEY_* (anything else does nothing), as the human
   seat's. Taken only after a fork, every kind while the rewind is idle and a move alone while it
   is open, none in the past, and only while the log can hold it (THE BOUND); LOGGED FIRST. */
void tagpu_wikireplay_input(WIKIREPLAY* r, const WIKIINPUT* in);

/* The game simulation shown now, NULL only when `failed`; not const, since tagpu_wikisim_view
   writes the simulation's own view arrays. THE LIFETIME: a view taken from it (which points into
   those arrays) is valid only until the next tagpu_wikireplay_step or _press, either of which may
   free that simulation; nothing but the view may be written through it. */
WIKISIM* tagpu_wikireplay_game(WIKIREPLAY* r);

/* A live replay's past simulation, the world shown once a seek has ended while the rewind shows the
   past (WIKI_RW_PAST); NULL none, and always in a replay that is not live. THE LIFETIME as
   tagpu_wikireplay_game's. */
WIKISIM* tagpu_wikireplay_past(WIKIREPLAY* r);

void tagpu_wikireplay_view(const WIKIREPLAY* r, WIKIREPLAYVIEW* v);

/* A players replay's sample n of seat `seat`, measure `m` (WIKI_STAT_MEASURES order); 0 outside the
   samples taken, and in any other replay (replay.js R.stat). */
float tagpu_wikireplay_stat(const WIKIREPLAY* r, int n, int seat, int m);

/* THE REPLAY'S HASH: 64-bit FNV-1a as tagpu_wikisim_hash, an integer 4 bytes little-endian and a
   string its byte length then its bytes ("" for none), over, in order: tick, done, clock, waiting
   (WIKI_UNTIL_SEEKED or 0), stalled, the caption, failed, ignored; the game's tick, end, paused,
   speed, acc, target, gen, quiet; the cursor: 0, or 1 then from (-1 none), from_value, to, value,
   at, and 1 with drag and over or 0; the bookmark count and each; the toast's kind (-1 none), at
   and value (0 and 0 with none); the question open, was_paused and left; walked, the marker count and
   each marker's tick, kind and seat; the game simulation's tagpu_wikisim_hash as its low and its
   high 32 bits, or 0 and 0 when `failed`. Then, live only, so a replay's hash takes no byte more:
   rw, G, the GameTime shown, arm, arm_R, arm_evict, drag_t, clash, nlogged, selected, the timeline
   count and each one's id, parent, R, last, live, used, mode and log count; the toast's evicted
   fork (-1 with none); back; full; be, bm and the four rates as their f32 bits; dt; last the past
   simulation's tagpu_wikisim_hash, low then high, or 0 and 0 with none. Then, a takeover replay's
   only, so no other replay's hash takes a byte more: the WIKI_TK_* mode, human, pick, pick_R,
   pick_evict, pick_parent, pick_tl, pick_serial, serial, lanes, armed, bg, tk_full, the saved
   count and each, the timeline count and each one's id, parent, R, last, live, used, mode and
   seat; last the walker's tick, -1 with none. Then, a players replay's only, so no other replay's
   hash takes a byte more: cam_x, cam_z, follow, jump_req, sight, sight_req, tog, folded,
   stat_open, stat_measure, stat_team, click_from, shown, pend; pbe, pbm and the four prates as
   their f32 bits; the five pdt; pass_at, pass_blocks, the fog bits' bytes (pass_blocks + 7) / 8 of
   them, one byte each, the contact count and each id; npt, base, period; and every sample's f32
   bits, n, then seat (nseat of them), then measure. The cursor's watched and from_watched are not
   hashed: they are presentation, read only by the frame record (tagpu_wikiframe.h WIKIFRAMECUR),
   which tools/wiki/framecheck.mjs holds equal to frame.js's at every trip's start, press and
   release. */
uint64_t tagpu_wikireplay_hash(const WIKIREPLAY* r);

/* Frees every simulation the replay holds and its script; `r` holds nothing after. */
void tagpu_wikireplay_free(WIKIREPLAY* r);

#endif
