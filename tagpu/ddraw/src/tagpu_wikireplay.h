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

   ENGINE-FREE, WIN32-FREE AND WITHOUT GLOBALS, as tagpu_wikisim.c: one WIKIREPLAY is used by one
   thread at a time, and it lives as long as its caller keeps it. The DLL compiles this file for
   SSE2 with the simulation (Makefile, THE WIKI'S SIMULATION): it computes on integers, its one
   double the demo's end the clock is compared with exactly, and shares the simulation's build. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_hudlay.h"
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

/* a toast's kind; a marker's is HUD_MK_DEATH or HUD_MK_DEFEAT */
enum { WIKI_TOAST_ADDED, WIKI_TOAST_REMOVED };

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
       control (-1 none) and final tick, this one's control and tick (-1 for a button), the clock
       tick it began, and its drag (drag -1 none); `held` the control between its press and
       release, -1 none */
    int         has_cursor, from, from_value, to, value, at, drag, over, held;
    int         nmark;
    int         mark[WIKI_REPLAY_MARKS][3];     /* tick, HUD_MK_*, seat                              */
    int         walked;                         /* the walker's tick, the end once the walk is over  */
    int         nbook;
    int         book[WIKI_REPLAY_BOOKS];        /* sorted                                            */
    int         toast_kind, toast_at, toast_value;  /* toast_kind -1: none                          */
    int         question, was_paused;           /* Leave's question is open, and the pause before it */
    int         left;                           /* Try it's Yes left the replay: it is done          */
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
    int               has_cursor, from, from_value, to, value, at, drag, over, pos;
    int               nbook, book[WIKI_REPLAY_BOOKS];
    int               toast_kind, toast_at, toast_value;
    int               question, was_paused;
    int               walked, nmark, mark[WIKI_REPLAY_MARKS][3];
    int               prev_lost[WIKI_SEATS], prev_out[WIKI_SEATS];
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

/* The game ticks the next step runs as the state stands; a seek the step's own viewer inputs start
   adds at most WIKI_REPLAY_SEEK. */
int tagpu_wikireplay_cost(const WIKIREPLAY* r);

/* Try it yourself's press, acting at once: a replay HUD's WIKI_HUD_*, the question's Yes included
   (anything else, a live HUD's control among it, does nothing), and for the track the game tick under the pointer, which
   the seek clamps to the game. Nothing while the replay is done or still walking. */
void tagpu_wikireplay_press(WIKIREPLAY* r, int control, int value);

/* The game simulation shown now, NULL only when `failed`; not const, since tagpu_wikisim_view
   writes the simulation's own view arrays. THE LIFETIME: a view taken from it (which points into
   those arrays) is valid only until the next tagpu_wikireplay_step or _press, either of which may
   free that simulation; nothing but the view may be written through it. */
WIKISIM* tagpu_wikireplay_game(WIKIREPLAY* r);

void tagpu_wikireplay_view(const WIKIREPLAY* r, WIKIREPLAYVIEW* v);

/* THE REPLAY'S HASH: 64-bit FNV-1a as tagpu_wikisim_hash, an integer 4 bytes little-endian and a
   string its byte length then its bytes ("" for none), over, in order: tick, done, clock, waiting
   (WIKI_UNTIL_SEEKED or 0), stalled, the caption, failed, ignored; the game's tick, end, paused,
   speed, acc, target, gen, quiet; the cursor: 0, or 1 then from (-1 none), from_value, to, value,
   at, and 1 with drag and over or 0; the bookmark count and each; the toast's kind (-1 none), at
   and value (0 and 0 with none); the question open, was_paused and left; walked, the marker count and
   each marker's tick, kind and seat; last the game simulation's tagpu_wikisim_hash as its low and
   its high 32 bits, or 0 and 0 when `failed`. */
uint64_t tagpu_wikireplay_hash(const WIKIREPLAY* r);

/* Frees every simulation the replay holds and its script; `r` holds nothing after. */
void tagpu_wikireplay_free(WIKIREPLAY* r);

#endif
