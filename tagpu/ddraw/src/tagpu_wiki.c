/* tagpu_wiki.c -- the in-game wiki's game-thread half. Contract: tagpu_wiki.h.

   GAME THREAD. Every function here runs on the thread that pumps TA's window, which is the
   thread the engine draws and steps on, except the two readers of the capture word below.

   THE CAPTURE: WHILE THE WIKI IS OPEN, THE ENGINE SEES A PLAYER WHO HAS LET GO. TA reads input
   through four channels, and only these: the messages to its window procedure (the only pusher of
   its key and mouse rings, *Who pushes the two rings* in exe-reverse-engineering.md), and three
   polls, GetCursorPos, GetAsyncKeyState and GetKeyState -- the import table names no DirectInput
   library (objdump -p), and the polls reach cnc-ddraw's fake_* hooks (winapi_hooks.c). While the
   wiki is open:
     - no input message reaches TA's window procedure (tagpu_wiki_message, tagpu_wiki_mouse,
       tagpu_wiki_key, tagpu_wiki_char take them first);
     - GetAsyncKeyState and GetKeyState answer 0 for every key;
     - GetCursorPos answers the game point of the press that opened the window, and
       g_ddraw.cursor stays there: no hook stores a point while the capture word is set.
   So no order, selection, hotkey, chat line or wheel notch reaches the engine, in multiplayer as
   in single player; the arrow hotkeys read up, so the camera holds; and the world under the
   window does not react to the pointer. The frozen point is on the wiki button in the top bar,
   which the in-play mouse handler 0x499200 finds on neither the minimap nor the viewport
   (0x49922C..0x499252): it sets `cursornormal` (0x49925D) and jumps past the build cursor 0x4197D0
   and the hover query 0x48CD80 (DISASSEMBLED 2026-10-25) -- no ghost, no hover -- and the button
   spans bar rows 2..29, never the outermost pixel the scroll poll's edge test asks for.
   SHIFT READS UP: after a Shift placement (main+0x2CC6 bit 5) the test at 0x4992D8 runs for every
   mouse record after the region test and disarms the building, exactly as letting go of Shift
   would. That is the invariant's own consequence, not a defect.

   THE ORDERING. The open and the close are each one store of the capture word, here on the game
   thread, made under s_cursorLock together with the frozen point and, at the open, g_ddraw.cursor.
   GetCursorPos is the one poll that stores what it reads: the engine mouse thread calls it every
   ~33 ms, and it writes the live point into g_ddraw.cursor (winapi_hooks.c). It tests the word and
   makes that store under the same lock (tagpu_wiki_cursor_store), so it comes wholly before the
   open -- its live point is then overwritten by the open's -- or wholly after it, and stores
   nothing; the frozen pair is read under the lock too, so it never mixes two opens. The key polls
   store nothing: an interlocked read of the word orders each before or after the store, the live
   device before and every key up after. Input given before the open reached the game; input given
   after it does not; there is no moment that is both.

   THE PAIRED PRESS AND THE PAIRED KEY. The engine is never handed a release whose press it never
   saw, nor misses one for a press it did. What it saw is a fact recorded where it is handed a
   message: both doors into TA's window procedure, the tail of cnc-ddraw's and the shield's to_game,
   call tagpu_wiki_engine_got, which keeps the left, right and middle buttons and the keys the
   engine holds down (s_engBtn, s_engKey). A release of one of those goes to the engine whatever
   the wiki's state: a key's as it is, and a button's, while the window is open, at the frozen point
   (TAGPU_WIKI_FROZEN: the caller delivers it as an injected release at that point is delivered), so
   a button held across the open is let go where the engine last saw the pointer. Every other release
   of a press the wiki took is taken too, whether the window is still open or not (s_owedBtn,
   s_owedKey). The wiki button and the close button act on the press, as the sprocket does. Esc
   closes on its RELEASE, and only the release of an Esc whose press the wiki took: the press, its
   auto-repeats and the WM_CHAR TranslateMessage posts for it all arrive while the window is still
   open, and none reaches the engine; the auto-repeats of a key the engine holds are taken too, and
   owe nothing. A press the wiki does not take of a button or key it is owed ends that debt, as
   tagpu_menu_click's does: its release was lost (dragged off the window, focus taken), and the next
   one is the engine's. The fourth and fifth buttons are not followed: TA's window procedure's jump
   table stops at 0x206 (tagpu_zoom.h), so their messages carry nothing to it.

   THE PAUSE (below, above wiki_open) is held in single player and nowhere else.

   THE PAGE AND THE STAGE (below, *the page*): the selection, the folds, the scrolls and the
   demo's playback are this thread's, changed only by a press on a control of the last frame drawn
   (the hit map tagpu_wiki_take copied), so a click lands on what the player saw. The demo's
   simulation (tagpu_wikisim.c), or a frame:game entry's replay of one (tagpu_wikireplay.c, below,
   *the replay*), lives here too and is stepped inside the packet's fill; the render thread gets a
   view of it by value in the record and never touches a WIKISIM or a WIKIREPLAY. In Try it
   yourself (below, *Try it yourself*) the stage's pointer, buttons and keys are the simulation's
   input, read against the same hit map; a replay's are presses on its stage HUD's controls.

   THE CONTEXT (below, *the context*): the demo plays, and its text is filled for, the controlled
   player's side and Interface Type, which the packet's fill reads from the engine and hands to
   tagpu_wiki_fill by value.

   Nothing here names engine memory: the two engine facts the pause needs are tagpu_menu.c's, and
   the side and the interface type are tagpu_packet_pub.c's. */
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"
#include "config.h"
#include "mouse.h"
#include "tagpu_opt.h"
#include "tagpu_shield.h"
#include "tagpu_menu.h"
#include "tagpu_hudstate.h"
#include "tagpu_hudart.h"
#include "tagpu_hudinput.h"
#include "tagpu_profiles.h"
#include "tagpu_replay.h"
#include "tagpu_log.h"
#include "tagpu_wiki.h"
#include "tagpu_wikistate.h"
#include "tagpu_wikiasset.h"
#include "tagpu_wikisound.h"
#include "tagpu_wikireplay.h"
#include "tagpu_wikiframe.h"
#include "tagpu_wikitext.h"
#include "tagpu_replays.h"
#include "tagpu_pak.h"

#define DUMP_ASK  "tagpu_wiki.dump"
#define DUMP_OUT  "tagpu_wiki.json"
#define CHECK_ASK "tagpu_wiki.check"
#define CHECK_LAYOUT "tagpu_wiki.layout.json"
/* THE 3D STAGE'S CAPTURE (tools/wiki_regression.py stage3d): the file holds `ENTRY DEMO TICK` -- an
   entry's id, its demo's number from 1, a tick -- and the fill that reads it picks that demo,
   plays it here to the tick, pauses it and numbers the capture in the record; the render thread
   writes the first frame of that view whose image is drawn (tagpu_wikiworld.h) */
#define SHOT_ASK  "tagpu_wiki.shot"
#define SHOT_TICK_MAX (30 * 60 * 10)   /* ten minutes of a demo: the steps run on the game thread */
/* THE TEST HARNESS'S PACING (tools/wiki_regression.py; nothing a player does reaches either):
   PAUSE_ASK holds `ENTRY DEMO TICK` as SHOT_ASK does, and the fill that reads it picks that demo
   afresh and plays it -- at the clock's pace, or the turbo's -- until its tick, where it pauses on
   exactly that tick; TURBO_ON holds K, and while it is there a playing demo steps K ticks a fill in
   place of the clock's, K at most STEPS_MAX, so a fill costs no more than the clock lets it. */
#define PAUSE_ASK "tagpu_wiki.pause"
#define TURBO_ON  "tagpu_wiki.turbo"

enum { B_L = 1, B_R = 2, B_M = 4, B_X = 8 };

/* ---- what any thread reads ------------------------------------------------------------------- */

/* THE CURSOR LOCK, initialised at attach (tagpu_wiki_init) before any DirectDraw object exists, so
   before fake_GetCursorPos can reach it. It is held over a few loads and stores and no call that
   takes another lock or waits on a thread, so it cannot deadlock. */
static CRITICAL_SECTION s_cursorLock;
static volatile LONG s_hold;            /* the capture word: 1 while the window is open; written
                                           under the lock, read without it by the key polls     */
static LONG s_fx, s_fy;                 /* the frozen game point, under the lock                 */

void tagpu_wiki_init(void)
{
    InitializeCriticalSection(&s_cursorLock);
}

LONG tagpu_wiki_holds_input(void)
{
    return InterlockedCompareExchange(&s_hold, 0, 0);
}

void tagpu_wiki_frozen(LONG* x, LONG* y)
{
    EnterCriticalSection(&s_cursorLock);
    *x = s_fx;
    *y = s_fy;
    LeaveCriticalSection(&s_cursorLock);
}

int tagpu_wiki_cursor_store(int x, int y, int outside, LONG* ax, LONG* ay)
{
    int held;
    EnterCriticalSection(&s_cursorLock);
    held = s_hold != 0;
    if (held) {
        *ax = s_fx;
        *ay = s_fy;
    } else {
        if (outside) mouse_forget_client();
        InterlockedExchange((LONG*)&g_ddraw.cursor.x, x);
        InterlockedExchange((LONG*)&g_ddraw.cursor.y, y);
        *ax = x;
        *ay = y;
    }
    LeaveCriticalSection(&s_cursorLock);
    return held;
}

/* ---- the game thread's own ------------------------------------------------------------------- */

static int  s_open;                     /* this thread's copy of s_hold                          */
static unsigned s_owedBtn;              /* B_*: buttons whose press was taken                    */
static unsigned s_engBtn;               /* B_L/B_R/B_M: buttons the engine was handed down, not up */
static BYTE s_engKey[256];              /* keys the engine was handed down, not up               */
static BYTE s_owedKey[256];             /* keys whose press was taken                            */
static int  s_charOwed;                 /* the last key-down was taken: so is the character the
                                           window's TranslateMessage posts for it               */
static int  s_escClose;                 /* an Esc press was taken while open: its release closes */
static char s_held[TAGPU_WIKI_KEY];     /* the control a left press holds down, shown pressed   */
static int  s_paused, s_pauseWas;       /* the pause is the wiki's, and bit 0 before it took it  */
static int  s_saidOff;
static TAGPU_CMD_WIKI s_hit;            /* the last frame drawn's controls                       */

static void page_open(void);
static void page_close(void);

static void say_open(int gx, int gy)
{
    tagpu_logf("wiki: open at (%d,%d)%s", gx, gy,
               s_paused ? (s_pauseWas ? ", single player already paused" : ", single player paused")
                        : ", the game runs on");
}

/* THE PAUSE. A NETWORK GAME NEVER REACHES THE PAUSE TEST: the frame callback takes its network
   branch on main+0x2A44 bit 0 at 0x4967E7, which calls the scheduler and the step and never
   reaches the test at 0x496920, and a pause set locally there would stall only this player (the
   scheduler tests the bit too, 0x4953D9; the Pause key keeps peers together only through its 0x19
   broadcast) [INFERRED]. So the pause is taken only when tagpu_menu_single_player says the level
   is not a network game -- and front-end code alone sets that bit while play only ever clears it
   (tagpu_menu.c names the stores), so a level single player at the open is single player at the
   close.
   NOTHING ELSE WRITES THE BIT WHILE THE WINDOW IS OPEN, because the window does not open while
   another pause is in flight (pause_in_flight). Three of Impure's writers put a pause back, or
   end one, later and without input: the live rewind, whose open saves bit 0 and sets it, and
   whose end restores what it saved -- from the frame visit, unasked, when its FLUSH or ENDING
   state finishes or is refused (tagpu_replay.c rw_finish, rw_close_unmoved) -- the replay HUD's
   Leave, whose question the next frame's observer opens, putting the pause back when it cannot or
   once the question is gone (tagpu_hudinput.c), and a takeover of a playing, whose run at the
   frame visit after its request ends the playing's pause (tagpu_replay.c, tk_run). None starts
   while the window is open: their buttons, Take over's rows included, are the replay HUD's, which
   no input reaches then. The one verb that can be queued before the open and run after it is a
   rewind's open (every other verb is refused in idle without a write); it saves the wiki's pause
   and gives it back at its own end, so the game stays paused after both have closed, until the
   Pause key -- a pause outliving the window, never a game running under it.
   Every engine writer of bit 0 in play is reached only through input the wiki holds -- the Pause
   key (0x4960BC), the in-game menu (0x460E00, 0x460A33) and its LOADGAME screens (0x49306D,
   0x493330), key 0xE3 (0x49990E), the options opener 0x45CFC0 (0x45D0A1), the chat command
   MakePoster (0x495B7E, put back at 0x495DD2 within the same call) -- or through a peer's message
   0x19 (0x45597A), which a single-player level never receives; the rest run in the front end
   (0x44B6F0, 0x44BE61: the unit-restrictions screen's save lists) or the level's load (0x49720A)
   (exe-reverse-engineering.md, *A level starts paused*). Impure's other writers
   set the bit and never clear it (the rewind's and the launch hold's keeper on 0x496920), run at a
   level's first tick (fix_load_unpaused, before any window can open), or are input (the replay
   HUD's pause button). The replay trigger's `resume` clears a pause its own `pause at` took, and
   its `takeover` queues the takeover whose run ends a playing's pause: a test's commands, which
   no player reaches.
   ON CLOSE the bit is cleared only if it was clear at the open, so a game paused before the wiki
   stays paused after it. AT LEVEL END the pause is forgotten and nothing is written. */
/* The pause another writer will put back on its own, or NULL (THE PAUSE, above) */
static const char* pause_in_flight(void)
{
    TAGPU_REPLAY_RWSTATE rw;
    tagpu_replay_rewind_state(&rw);
    if (rw.state != TAGPU_REPLAY_RWS_IDLE) return "a rewind holds the pause";
    if (rw.tk_queued) return "a takeover is queued: its run ends the playing's pause";
    if (tagpu_hudinput_leave_pending()) return "Leave's question holds the pause";
    return NULL;
}

static void wiki_open(int gx, int gy)
{
    int was = 0;
    EnterCriticalSection(&s_cursorLock);
    s_fx = gx;
    s_fy = gy;
    InterlockedExchange((LONG*)&g_ddraw.cursor.x, gx);
    InterlockedExchange((LONG*)&g_ddraw.cursor.y, gy);
    InterlockedExchange(&s_hold, 1);
    LeaveCriticalSection(&s_cursorLock);
    s_open = 1;
    s_paused = 0;
    if (tagpu_menu_single_player() && tagpu_menu_pause_hold(1, &was)) {
        s_paused = 1;
        s_pauseWas = was;
    }
    say_open(gx, gy);
    page_open();
}

static void wiki_close(const char* how)
{
    if (s_paused && !s_pauseWas) tagpu_menu_pause_hold(0, NULL);
    s_paused = 0;
    s_open = 0;
    s_escClose = 0;
    EnterCriticalSection(&s_cursorLock);
    InterlockedExchange(&s_hold, 0);
    LeaveCriticalSection(&s_cursorLock);
    page_close();
    tagpu_logf("wiki: closed (%s)", how);
}

/* the topmost control of the last frame drawn under the client point, NULL none: its key into
   `key` and its source (WIKIHIT_*: the page's, the stage HUD's or Leave's question's) into `*src`,
   each read as data -- the key ended inside its array, a source other than the HUD's and the
   question's the page's */
static const TAGPU_CMD_WIKIHIT* hit_src(int cx, int cy, char* key, int* src)
{
    unsigned i;
    if (cx < 0) return NULL;
    for (i = 0; i < s_hit.n; i++) {
        const TAGPU_CMD_WIKIHIT* h = &s_hit.hit[i];
        if (cx >= h->x0 && cx < h->x1 && cy >= h->y0 && cy < h->y1) {
            memcpy(key, h->key, TAGPU_WIKI_KEY);
            key[TAGPU_WIKI_KEY - 1] = 0;
            *src = h->src == WIKIHIT_HUD || h->src == WIKIHIT_DIALOG ? h->src : WIKIHIT_PAGE;
            return h;
        }
    }
    return NULL;
}

/* the topmost PAGE control of the last frame drawn under the client point: its key into `key`. A
   control of the stage HUD or of Leave's question is never the page's, whatever its key: the HUD
   has its own `play`. */
static int hit(int cx, int cy, char* key)
{
    int src;
    return hit_src(cx, cy, key, &src) && src == WIKIHIT_PAGE;
}

/* The pointer at client point (cx, cy) moved while the window is open: noted as mouse.c notes a
   move it hands the engine, so the UI layer's cursor follows the pointer while the engine's point
   stays put, and none is drawn outside the picture. */
static void note_client(int cx, int cy)
{
    if (cx < 0) return;
    if (mouse_client_inside(cx, cy)) mouse_note_client(cx, cy);
    else mouse_forget_client();
}

/* ---- the page --------------------------------------------------------------------------------

   What wiki.js keeps in S, and the demo it plays. All of it lasts for the session and is never
   saved; the simulation is made at a selection, a replay and an open, and freed at a change, a
   close and the level's end -- every one of them on this thread, so the WIKISIM's lifetime is its
   confinement to it. */

/* THE WHEEL: a notch scrolls three of the tree's rows (layout.json row.h), whichever panel is under
   the pointer. The mockup leaves the step to the browser, whose notch is a platform's setting. */
#define WHEEL_UNITS 48.0f
/* A PRESS ON A TRACK, off its thumb, pages: seven eighths of the panel's height toward the press,
   as Chrome pages a scroller. */
#define TRACK_PAGE  0.875f
/* THE CLOCK, wiki.js `advance`: the demo's ticks are WIKI_TPS a second at 1x, and a frame steps at
   most this many, so a stalled frame catches up a few ticks rather than seconds of them -- which
   also bounds what a frame of the demo costs this thread. */
#define STEPS_MAX   8.0

static int      s_entry = -1, s_demo = -1;      /* WIKIDATA.entry's index and its demo; -1 none  */
static uint32_t s_fold[TAGPU_WIKI_FOLDS];       /* bit n: tree node n is folded                  */
static float    s_scroll[TAGPU_WIKI_NSCROLL];   /* each panel's, units, within the drawn limit   */
static int      s_playing = 1, s_speed = 1;
static WIKISIM  s_sim;
static int      s_simOn;                        /* s_sim holds a demo: it is freed at a change   */
static int      s_want;                         /* a demo is owed as soon as the data is there   */
static char     s_msg[TAGPU_WIKI_MSG];          /* what the stage shows with no demo             */
static const WIKILAYOUT* s_lay;                 /* the sidebar layout s_sim was started with,
                                                   NULL none: immutable and never freed          */
static int      s_laySrc;                       /* its WIKISTAGE_LAYOUT_*, the record's value    */
static LARGE_INTEGER s_qpf, s_last;
static double   s_acc;                          /* ticks owed, wiki.js `acc`                     */
/* THE CLOCK'S OWN ACCOUNT since the demo started, which the state dump gives: the seconds it played
   (each fill's, clamped as the clock clamps it) and the ticks STEPS_MAX took off what they owed. A
   fill leaves under one tick owed, so between two dumps of a demo playing at one speed on the clock
   -- no speed change, pause or turbo between them, each of which drops what was owed -- the ticks
   played are WIKI_TPS x the speed x the seconds, less the ticks taken off, to within one: the rule
   the clock paces by, on its own clock */
static double   s_clockS, s_clipped;
static int      s_turbo;                        /* TURBO_ON's K, 0 off                           */
static int      s_pauseAt = -1;                 /* PAUSE_ASK's tick for the running demo, -1 none */
static uint32_t s_pauseN;                       /* the last pause asked for, 0 none              */
static int      s_drag = -1;                    /* the panel whose thumb a left press holds      */
static int      s_dragReal;                     /* that press came by the real path              */
static int      s_dragY;
static float    s_dragFrom;
static double   s_costMax;                      /* the most one fill cost this thread, ms, since
                                                   the open: from the record's start in
                                                   tagpu_wiki_fill to its placing in the packet
                                                   (tagpu_wiki_carried) -- the demo's steps, the
                                                   view's copy into the record and the record's
                                                   copy into the slot; not the check or the dump */
static LARGE_INTEGER s_costT0;                  /* the open fill's start; 0 = no fill timed     */
static unsigned s_costFrames;
static uint32_t s_trunc;                        /* the copy's WIKISTAGE_TRUNC_* already said     */
static int      s_saidHead;                     /* a stage the packet could not carry, said      */
static uint32_t s_shot;                         /* the last capture asked for, 0 none            */
static WIKIWORDS s_words;                       /* the running demo's caption words (start): they
                                                   point into its entry, which the DLL never
                                                   frees, and into its casting, in the published
                                                   WIKISIDE block, never freed either            */
static int      s_side = -2;                    /* the last fill's engine side, -1 none; -2 before
                                                   the first fill                                */
static int      s_iface = 1;                    /* the last fill's Interface Type, 0 or 1        */
static WIKISIMCTX s_ctx;                        /* the context s_sim was started with            */
static int      s_cast = -1;                    /* the layout side whose casting s_ctx holds, -1 */
static int      s_about, s_drawer;              /* the theatre's About and its drawer: at most one */

/* ---- the replay ------------------------------------------------------------------------------
   A FRAME:GAME ENTRY'S DEMO IS A REPLAY (tagpu_wikireplay.h): its viewer's script plays the replay
   HUD's controls on the demo's game, or in Try it yourself the player presses them on the stage's
   HUD. It is made, stepped and freed on this thread as s_sim is, in its place: s_simOn and s_repOn
   are never both set. Its game simulation is the replay's own and may be freed by any step or
   press (a backward seek), so nothing here keeps a pointer into it past the call that took it.

   THE HELD VIEW: the stage shows the last world drawn while a seek runs (decision 11; wiki.js
   `FR.gv`). s_frameView is copied from the world shown -- the replay's game, or a live replay's
   past simulation while it shows the past -- and filtered to the frame's view (tagpu_wikiframe_view),
   only in a fill that ends with no seek pending, and the record is published from it; the seats
   and the human seat's cursor the frame record reads are held with it (s_heldGv, from
   tagpu_wikisim_view_as of the human seat: seat 0, or the seat a takeover took), since the record's
   players column, top bar and cursor are the world shown, not the world a seek is playing
   through. */
static WIKIREPLAY     s_rep;
static int            s_repOn;                  /* s_rep holds a replay: freed at a change      */
static int            s_repTry;                 /* it was made for Try it yourself (create)      */
static WIKIFRAMEPRESS s_press;                  /* Try it's presses on the stage HUD             */
static WIKISTAGEVIEW  s_frameView;              /* the held view; 158 KB, never on a stack       */
static int            s_frameHave;              /* s_frameView holds this replay's world         */
static WIKISEAT       s_heldSeat[WIKI_SEATS];
static WIKIECON       s_heldEcon[WIKI_SEATS];
static WIKIVIEW       s_heldGv;                 /* its game part and the human seat's cursor
                                                   only: game, nseat, seat, econ, cursor_*       */
static int            s_repGen = -1, s_repQuiet = -1;   /* the replay's sound, as last synced   */
/* THE REPLAY'S SERIAL: incremented whenever s_rep is made or freed, published in each packet
   (TAGPU_PK_WIKI.rep_serial) and named by the hit map drawn from it, so a press on the stage HUD
   acts only on the replay that map was drawn for (frame_current). Wraps after 2^32 makes and frees,
   which no session reaches. */
static uint32_t       s_repSerial;
static int            s_liveMove;               /* Try it's world point is held for a live replay
                                                   (live_flush), freed with the replay           */
static double         s_liveX, s_liveZ;

static const WIKIDATA* data(void)
{
    const WIKIART* a = tagpu_wikiart_get();
    return a ? a->data : NULL;
}

static const WIKIENTRY* entry_of(const WIKIDATA* d)
{
    return d && s_entry >= 0 && s_entry < d->nentry ? &d->entry[s_entry] : NULL;
}

static void held(const char* key)
{
    lstrcpynA(s_held, key, sizeof s_held);
}

/* the simulation the stage shows: the page demo's, or the replay's game -- NULL while none plays,
   and for a replay whose backward seek could not make its game again. THE LIFETIME: a replay's
   game is valid until its next step or press. */
static WIKISIM* shown(void)
{
    return s_simOn ? &s_sim : s_repOn ? tagpu_wikireplay_game(&s_rep) : NULL;
}

/* a demo plays on the stage, a page demo or a replay */
static int demo_on(void)
{
    return s_simOn || s_repOn;
}

/* ---- the sound -------------------------------------------------------------------------------

   THE SIMULATION'S SOUNDS ARE PLAYED HERE, on the game thread after each fill's steps: every event
   the view's ring holds past the last one played, in the order made (tagpu_wikisim.h, THE SOUND),
   through the wiki's own mixer (tagpu_wikisound.h), which applies the player's settings. The ring
   holds WIKI_SND_LOG events and a fill steps at most STEPS_MAX ticks, so an event is lost only to
   a burst the ring cannot hold; a loss is counted and said with the demo's tally. The page's own
   buttons say `Options`, as the engine's in-game menu controls do. */
static int s_heard;                             /* the demo's events played: its snd_total then  */
static int s_lost;                              /* events the ring let go before they were played */
static int s_died, s_diedDark, s_diedPlayed;    /* its deaths, those in the black, those played  */
static char s_sndDemo[2 * WIKI_ID_MAX + 16];    /* "<entry>/<demo>" of s_sim, for the tally      */

static const WIKI3D* load_of(void);
static const WIKI3DSOUND* sound_of(void);
static int snd_type(void* c, const char* name, WIKISNDTYPE* out);

/* a press on one of the page's controls: the engine's in-game menu controls say `Options` */
static void button_sound(void)
{
    tagpu_wikisound_named(sound_of(), WIKI_SNDNAME_OPTIONS);
}

/* A UNIT'S DEATH (tagpu_wikisim.h WIKI_SND_DEATH): its type's death WAV, looked up through the
   sound source by the type's name in the simulation's entry, when the view's fog at the event's
   cell is not black -- the wiki keeps one fog, the viewed seat's. 0x47F300 has no priority to give
   (tagpu_wikisound.h, A UNIT'S DEATH). BOUND: the type indexes the entry's types only below their
   count, the cell the view's map only below w x h, and the clip is bounded by the mixer. */
static void death_play(const WIKIVIEW* v, const WIKIENTRY* entry, const WIKISNDEV* e)
{
    const WIKI3D* w = load_of();
    WIKISNDTYPE t;
    s_died++;
    if (!entry || e->name < 0 || e->name >= entry->ndef || !v->fog || e->variant < 0 ||
        e->variant >= v->w * v->h)
        return;
    if (v->fog[e->variant] == 2) {
        s_diedDark++;
        return;
    }
    if (!w || !snd_type((void*)w, entry->def[e->name].name, &t) || t.death < 0) return;
    s_diedPlayed += tagpu_wikisound_death(&w->sound, t.death);
}

/* `entry` is the simulation's own, bound to its casting: a death event names a type by its index */
static void sound_drain(const WIKIVIEW* v, const WIKIENTRY* entry)
{
    static const unsigned char PRIORITY[WIKI_SND_KINDS] = WIKI_SND_PRIORITIES;
    const WIKI3DSOUND* snd = sound_of();
    const int first = v->snd_total - v->nsnd;
    int i;
    if (!v->has_sound) return;
    if (s_heard < first) {
        s_lost += first - s_heard;
        s_heard = first;
    }
    /* BOUND: i runs over the view's own events, [0, nsnd) */
    for (i = s_heard - first; i >= 0 && i < v->nsnd; i++) {
        const WIKISNDEV* e = &v->snd[i];
        if (e->kind > 0 && e->kind < WIKI_SND_KINDS) tagpu_wikisound_reply(snd, e->cat, e->kind, e->variant, PRIORITY[e->kind]);
        else if (e->kind == WIKI_SND_DEATH) death_play(v, entry, e);
        else if (!e->kind) tagpu_wikisound_named(snd, e->name);
    }
    s_heard = v->snd_total;
}

/* the demo's sound, said once as it is let go: what it made and what of it played */
static void sound_tally(void)
{
    int played, failed, silent;
    double slow;
    const char* what;
    tagpu_wikisound_tally(&played, &failed, &silent, &slow, &what);
    if (shown() && shown()->snd_on && (s_heard || played || failed || silent))
        tagpu_logf("wiki sound: the demo %s made %d sound(s): %d Play call(s) succeeded, %d failed, %d silent by the "
                   "settings or a missing WAV, %d lost; %d death(s), %d of them in the black, %d played; the slowest "
                   "call %.0f us (%s)", s_sndDemo, s_heard, played, failed, silent, s_lost, s_died, s_diedDark,
                   s_diedPlayed, slow, what);
    s_heard = 0;
    s_lost = 0;
    s_died = s_diedDark = s_diedPlayed = 0;
}

static void sim_free(void)
{
    sound_tally();
    if (s_simOn) tagpu_wikisim_free(&s_sim);
    if (s_repOn) {
        tagpu_wikireplay_free(&s_rep);
        s_repSerial++;
    }
    s_simOn = s_repOn = 0;
    s_frameHave = 0;
    s_heldGv.game = s_heldGv.nseat = 0;
    s_repGen = s_repQuiet = -1;
    s_liveMove = 0;
    tagpu_wikiframe_press_init(&s_press);
}

/* ---- Try it yourself -------------------------------------------------------------------------

   wiki.js's Try it yourself: the simulation made on the demo's scene with no script (sim.js
   create), and the player's pointer over the stage, the left and right buttons pressed on it and
   Shift, Alt, Ctrl and Alt+Z / Alt+X given to it as sim.js `input` -- here, on this thread, from
   the same messages the window takes while it is open. The engine is handed none of them: every
   one is taken by the capture above, Try it or not, so nothing here changes what the engine sees.

   NO BUTTON OR KEY IS LEFT HELD IN THE SIMULATION, NONE DROPPED. What was sent down and not yet up
   is one set (wiki.js `down`, in the order sent), and an up is sent for exactly its members: the
   release of a held button wherever it is made, the key-up of a held key, and the whole set, in
   its order, when the window closes (the level's end included) or loses the focus (wiki.js
   releaseAll). Leaving Try it, a replay or a new entry or demo empties the set with the
   simulation it belonged to (start), which takes no up, as wiki.js `start` nulls `sim` before its
   releaseAll. A real button's release made off the window is never sent here; the first real move
   that carries the button up sends it instead, as the scrollbar's drag ends (THE DRAG ENDS WITH
   ITS BUTTON, below). A press already held sends no second down, as the browser sends none. */
#define TRY_LOG 4096                            /* the inputs the dump can give, per Try it      */
enum { TRY_MOVE, TRY_DOWN, TRY_UP };
typedef struct { int32_t tick; uint8_t kind; int8_t key; double x, z; } TRYIN;

static int      s_try;                          /* wiki.js S.try: the stage is the player's      */
static int      s_down[WIKI_NKEYS], s_ndown;    /* wiki.js `down`: WIKI_KEY_*, in the order sent */
static unsigned s_downReal;                     /* B_L/B_R: held by a press of the real path     */
static TRYIN    s_log[TRY_LOG];                 /* this Try it's inputs, for the state dump      */
static int      s_nlog, s_logCut;

/* TRY IT ON A LIVE FRAME (rewind-spec.md 9.6, wiki.js liveWorld, liveFlush): the world takes the
   player's pointer, buttons and keys through tagpu_wikireplay_input, never the simulation directly,
   and the replay takes them only after a fork (LOGGED FIRST, tagpu_wikireplay.h). The pointer's moves
   are coalesced: the last world point is held here and sent once, before each replay step and before
   each press -- a press of a button on the world replaces the held point with its own and sends it,
   then its down -- so the timelines' logs grow by at most one move a demo step. The held point is
   s_liveMove's (with the replay's statics, above). */
static int frame_try(void);
static int frame_current(void);

/* the live replay takes Try it's world input: its drawn frame is the running replay's */
static int live_try(void)
{
    return frame_try() && s_rep.live && frame_current();
}

static void live_input(int kind, int key, double x, double z)
{
    WIKIINPUT in;
    memset(&in, 0, sizeof in);
    in.down = in.up = in.build = -1;
    switch (kind) {
    case TRY_MOVE:  in.has = WIKI_IN_MOVE; in.move.x = x; in.move.z = z; break;
    case TRY_DOWN:  in.down = key; break;
    case TRY_UP:    in.up = key; break;
    }
    tagpu_wikireplay_input(&s_rep, &in);
}

/* the held world point sent to the replay, once */
static void live_flush(void)
{
    if (!s_liveMove || !s_repOn) return;
    s_liveMove = 0;
    live_input(TRY_MOVE, -1, s_liveX, s_liveZ);
}

/* Try it's simulation takes input: the page demo's, or a live replay's world (live_try). An up is
   sent to a live Try-it replay whatever its frame, so no key it took down stays held in it; the
   replay refuses what its state does not take. */
static int try_on(void)
{
    return s_try && (s_simOn || live_try());
}

static int try_up_on(void)
{
    return s_try && (s_simOn || (s_repOn && s_repTry && s_rep.live));
}

/* one input to Try it's simulation, and its line in the log; on a live replay a move is held
   (live_flush sends it) and a key or button goes after the held move */
static void try_send(int kind, int key, double x, double z)
{
    WIKIINPUT in;
    if (!s_simOn) {
        if (!s_repOn) return;
        if (kind == TRY_MOVE) {
            s_liveMove = 1;
            s_liveX = x;
            s_liveZ = z;
            return;
        }
        live_flush();
        live_input(kind, key, x, z);
        return;
    }
    memset(&in, 0, sizeof in);
    in.down = in.up = in.build = -1;
    switch (kind) {
    case TRY_MOVE:  in.has = WIKI_IN_MOVE; in.move.x = x; in.move.z = z; break;
    case TRY_DOWN:  in.down = key; break;
    case TRY_UP:    in.up = key; break;
    }
    tagpu_wikisim_input(&s_sim, &in);
    if (s_nlog >= TRY_LOG) { s_logCut++; return; }
    s_log[s_nlog].tick = s_sim.tick;
    s_log[s_nlog].kind = (uint8_t)kind;
    s_log[s_nlog].key = (int8_t)key;
    s_log[s_nlog].x = x;
    s_log[s_nlog].z = z;
    s_nlog++;
}

static int try_held(int k)
{
    int i;
    for (i = 0; i < s_ndown; i++)
        if (s_down[i] == k) return i;
    return -1;
}

/* `k` sent down: once while it is held, or every time for Z and X, which step on each down, and
   for E, a key the sidebar's buttons take on each down */
static void try_down(int k)
{
    if (!try_on()) return;
    if (try_held(k) < 0) {
        if (s_ndown >= WIKI_NKEYS) return;
        s_down[s_ndown++] = k;
    } else if (k != WIKI_KEY_Z && k != WIKI_KEY_X && k != WIKI_KEY_E)
        return;
    try_send(TRY_DOWN, k, 0, 0);
}

/* `k`'s up, if it is held */
static void try_up(int k)
{
    int i = try_held(k);
    if (i < 0) return;
    memmove(&s_down[i], &s_down[i + 1], (size_t)(s_ndown - i - 1) * sizeof *s_down);
    s_ndown--;
    if (k == WIKI_KEY_LMB) s_downReal &= ~B_L;
    if (k == WIKI_KEY_RMB) s_downReal &= ~B_R;
    if (try_up_on()) try_send(TRY_UP, k, 0, 0);
}

/* wiki.js releaseAll: every held key's up in the order they went down, `send` 0 when the
   simulation is being replaced and takes none (start: wiki.js nulls `sim` before it) */
static void try_release_all(int send)
{
    int i;
    for (i = 0; send && try_up_on() && i < s_ndown; i++) try_send(TRY_UP, s_down[i], 0, 0);
    s_ndown = 0;
    s_downReal = 0;
}

/* A LIVE FRAME'S WORLD POINT (frame.js onWorld, wiki.js liveWorld): client point (cx, cy) through the
   drawn frame's fit -- its `stage` hit, its unit and its column's rows, with tagpu_wikistage_fit, the
   one function both threads fit a frame with -- when the topmost control of that frame under the point
   is the stage itself, so no stage HUD control, no part of Leave's question and nothing of the page
   lies over it, and the point is on the column or the map. The map is the held view's, the size every
   simulation of the entry has. In world units as the game simulation takes them, the column's rows
   starting at the top bar's first (sim.js DZ), given to the human seat (seat 0, or the seat a takeover
   took). A watching frame takes none -- Watching's, and a takeover entry's before its takeover: its
   viewer plays no seat (live_try). */
static int live_world(int cx, int cy, double* wx, double* wz)
{
    char key[TAGPU_WIKI_KEY];
    const TAGPU_CMD_WIKIHIT* h;
    WIKISTAGEFIT f;
    int src, Wg, Hg;
    double x, z;
    if (!live_try() || !s_frameHave) return 0;
    h = hit_src(cx, cy, key, &src);
    if (!h || src != WIKIHIT_PAGE || strcmp(key, "stage")) return 0;
    tagpu_wikistage_fit(s_frameView.w, s_frameView.h, s_hit.side_h, 1, h->x1 - h->x0, h->y1 - h->y0, s_hit.u, &f);
    tagpu_wikiframe_size(s_frameView.w, s_frameView.h, s_hit.side_h, &Wg, &Hg);
    if (!(f.s > 0)) return 0;
    x = (cx - h->x0 - f.ox) / f.s;
    z = (cy - h->y0 - f.oz) / f.s;
    if (!tagpu_wikiframe_on_world(x, z, Hg, s_frameView.w * WIKI_CELL, s_frameView.h * WIKI_CELL)) return 0;
    *wx = x;
    *wz = z;
    return 1;
}

/* THE WORLD POINT COMES FROM THE FRAME THE PLAYER SAW: client point (cx, cy) on the stage of the
   last frame drawn, read through that frame's own `stage` hit -- the stage's canvas, the same
   snapped rect the render thread fitted the map into (tagpu_wikistage_canvas) -- and its unit,
   with tagpu_wikistage_fit, the one function both threads fit with; clamped to the content
   rect -- the map, and with the sidebar the column left of it, as tall as the taller -- as wiki.js
   `world`: the simulation decides what lies under the point (the column or the map). The map is
   the entry's scene's, grown beside the column as the simulation grows it. THE FIT IS THE DRAWN FRAME'S: the sidebar's rows are those of the view that frame
   drew (TAGPU_CMD_WIKI.side_h), not the running simulation's, which may have turned its sidebar
   on or off since -- a press is mapped through what the player saw, never through a column no
   frame has shown. The frame must have drawn the window open, for the entry selected now, with a
   view on its stage. 0 when the point is not on that stage. A whole game frame (s_hit.frame) is
   live_world's. */
static int stage_world(int cx, int cy, double* wx, double* wz)
{
    const WIKIENTRY* e = entry_of(data());
    const TAGPU_CMD_WIKIHIT* h = NULL;
    WIKISTAGEFIT f;
    double x, z;
    unsigned i;
    if (!e || cx < 0 || !s_hit.drawn || !s_hit.open || !s_hit.view || s_hit.entry != s_entry) return 0;
    if (s_hit.frame) return live_world(cx, cy, wx, wz);
    for (i = 0; i < s_hit.n && !h; i++)
        if (!strncmp(s_hit.hit[i].key, "stage", TAGPU_WIKI_KEY)) h = &s_hit.hit[i];
    if (!h || cx < h->x0 || cx >= h->x1 || cy < h->y0 || cy >= h->y1) return 0;
    /* every scene of an entry has the first one's map size (tagpu_wikidata_entry), so the fit is the
       entry's map grown beside the drawn frame's column, as the simulation grows it */
    tagpu_wikistage_fit(e->scenes[0].w, tagpu_wiki_map_rows(e->scenes[0].h, s_hit.side_h), s_hit.side_h, 0,
                        h->x1 - h->x0, h->y1 - h->y0, s_hit.u, &f);
    if (!(f.s > 0)) return 0;
    x = (cx - h->x0 - f.ox) / f.s;
    z = (cy - h->y0 - f.oz) / f.s;
    *wx = x < f.x0 ? f.x0 : x > f.x0 + f.cw ? f.x0 + f.cw : x;
    *wz = z < 0 ? 0 : z > f.ch ? f.ch : z;
    return 1;
}

/* The pointer at client point (cx, cy): over the stage, a move. `real`: a message of the real path,
   whose wParam `wp` says which buttons are down -- one held here that it carries up was released
   where this window never heard it, and is let go now. */
static void try_pointer(int cx, int cy, int real, WPARAM wp)
{
    double x, z;
    if (real && (s_downReal & B_L) && !(wp & MK_LBUTTON)) try_up(WIKI_KEY_LMB);
    if (real && (s_downReal & B_R) && !(wp & MK_RBUTTON)) try_up(WIKI_KEY_RMB);
    if (try_on() && stage_world(cx, cy, &x, &z)) try_send(TRY_MOVE, -1, x, z);
}

/* A left or right press at client point (cx, cy), or where the pointer is when the injection named
   none: on the stage, a move there and then the button's down. 1 = it was on the stage. */
static int try_press(unsigned bit, int cx, int cy, int real)
{
    const int k = bit == B_L ? WIKI_KEY_LMB : WIKI_KEY_RMB;
    double x, z;
    if (!try_on() || (bit != B_L && bit != B_R)) return 0;
    if (cx < 0 && !mouse_last_client(&cx, &cy)) return 0;
    if (!stage_world(cx, cy, &x, &z)) return 0;
    try_send(TRY_MOVE, -1, x, z);
    live_flush();
    if (try_held(k) < 0) {
        if (real) s_downReal |= bit;
        else s_downReal &= ~bit;
        try_down(k);
    }
    return 1;
}

/* the release of button `bit`, wherever it is made */
static void try_release(unsigned bit)
{
    if (bit == B_L) try_up(WIKI_KEY_LMB);
    if (bit == B_R) try_up(WIKI_KEY_RMB);
}

static int try_modifier(unsigned vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:     return WIKI_KEY_SHIFT;
    case VK_MENU: case VK_LMENU: case VK_RMENU:        return WIKI_KEY_ALT;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return WIKI_KEY_CTRL;
    }
    return -1;
}

/* Alt is down as a key event says (wiki.js e.altKey): its press was handed to the engine or taken
   here, and its release has not come */
static int alt_held(void)
{
    return s_engKey[VK_MENU] || s_owedKey[VK_MENU] || s_engKey[VK_LMENU] || s_owedKey[VK_LMENU] ||
           s_engKey[VK_RMENU] || s_owedKey[VK_RMENU];
}

static int ctrl_held(void)
{
    return s_engKey[VK_CONTROL] || s_owedKey[VK_CONTROL] || s_engKey[VK_LCONTROL] || s_owedKey[VK_LCONTROL] ||
           s_engKey[VK_RCONTROL] || s_owedKey[VK_RCONTROL];
}

/* a key-down (`down`) or key-up of `vk` while the window is open: Z and X with Alt step the armed
   building's spacing, every down, an auto-repeat included, as the game steps again; E is a key to
   the sidebar's buttons (RECLAIM's quickkey), every down as the GUI pump takes each; Shift, Alt and
   Ctrl go down once and up */
static void try_key(unsigned vk, int down)
{
    const int m = try_modifier(vk), z = vk == 'Z' ? WIKI_KEY_Z : vk == 'X' ? WIKI_KEY_X : -1;
    if (!down) {
        if (m >= 0) try_up(m);
        if (z >= 0) try_up(z);
        if (vk == 'E') try_up(WIKI_KEY_E);
    } else if (z >= 0 && alt_held())
        try_down(z);
    else if (vk == 'E' && !ctrl_held())       /* under Ctrl the engine's translator pushes vk + 0x69
                                                  (0x4C2129..0x4C2136), which no quickkey is */
        try_down(WIKI_KEY_E);
    else if (m >= 0)
        try_down(m);
}

/* THE 3D ASSET LOAD's state on this thread (assets_start, below): not asked, started, or failed
   before its loader thread could start, which then publishes no answer */
enum { ASSETS_NONE, ASSETS_STARTED, ASSETS_FAILED };
static int s_assets;

/* THE SIDEBAR'S LAYOUT FOR A NEW SIMULATION (research/notes/ingame-wiki.md, *Which layout, and
   when a demo shows it*): the running mod's once the asset loader has published it, else the wiki
   data's fixture once the loader has refused it or could not start; 0 while the answer is
   pending. A simulation keeps the
   layout it started with, and none starts while the answer is pending, so no demo ever runs on a
   layout that arrives after it started. ORDERING: tagpu_wikiside_get's read is interlocked, pairing
   with the loader's one publishing store, so the block it hands back was written whole before; the
   block and the fixture are immutable and never freed, so the pointer holds for the process.
   `*blk` (optional) gets the published block with the mod's layout, else NULL. */
static int layout_now(const WIKIDATA* d, const WIKILAYOUT** lay, int* src, const WIKISIDE** blk)
{
    const WIKISIDE* side = NULL;
    const int st = s_assets == ASSETS_FAILED ? WIKISIDE_REFUSED : tagpu_wikiside_get(&side);
    if (blk) *blk = NULL;
    if (st == WIKISIDE_PENDING) return 0;
    if (st == WIKISIDE_READY && side) {
        *lay = &side->layout;
        *src = WIKISTAGE_LAYOUT_MOD;
        if (blk) *blk = side;
    } else {
        *lay = d ? d->fixture : NULL;
        *src = *lay ? WIKISTAGE_LAYOUT_FIXTURE : WIKISTAGE_LAYOUT_NONE;
    }
    return 1;
}

/* THE UNITS' SCRIPTS FOR A NEW SIMULATION (tagpu_wikicob.h): each type's script and posed model
   from the published 3D load, found by name as the loader named it; 0 while the load's answer is
   pending, else 1 with `*src` the source, or NULL when the load failed or never started, which
   plays every unit at rest. As with the layout, no simulation starts while the answer is pending,
   so a demo's units have their scripts from its first tick or never. LIFETIME: the WIKI3D is
   immutable and never freed once published, and the host copies a script and its model's shape
   when it is made (tagpu_wikicob_new), so nothing the source hands out is held past the call. */
static int cob_type(void* c, const char* name, WIKICOBTYPE* out)
{
    const WIKI3D* w = (const WIKI3D*)c;
    int i;
    for (i = 0; i < w->nunit; i++) {
        const WIKI3DUNIT* u = &w->unit[i];
        if (_stricmp(u->name, name) || !u->script) continue;
        out->cob = u->script;
        out->ncob = u->nscript;
        out->pieces = u->pieces;
        out->visible = u->visible;
        out->offset = &u->offset[0][0];
        out->parent = u->parent;
        out->top = u->top;
        return 1;
    }
    return 0;
}
static WIKICOBSRC s_cobsrc = { NULL, cob_type };

/* a type's sounds, with its model only, as gfx.py gives a mesh its `sound` (tagpu_wikiasset.h) */
static int snd_type(void* c, const char* name, WIKISNDTYPE* out)
{
    const WIKI3D* w = (const WIKI3D*)c;
    int i;
    for (i = 0; i < w->nunit; i++) {
        const WIKI3DUNIT* u = &w->unit[i];
        if (_stricmp(u->name, name) || !u->ok) continue;
        out->cat = u->sound_cat;
        memcpy(out->count, u->sound_count, sizeof out->count);
        out->death = u->death_sound;
        return 1;
    }
    return 0;
}
static WIKISNDSRC s_sndsrc = { NULL, snd_type };

/* the published 3D load, NULL while there is none */
static const WIKI3D* load_of(void)
{
    const WIKI3D* w = NULL;
    if (s_assets == ASSETS_FAILED || tagpu_wiki3d_answer(&w) == WIKISIDE_PENDING) return NULL;
    return w;
}

/* the published load's sound, NULL while there is none (the page's buttons play from it too) */
static const WIKI3DSOUND* sound_of(void)
{
    const WIKI3D* w = load_of();
    return w ? &w->sound : NULL;
}

/* THE UNITS' SCRIPTS AND SOUNDS come from the one 3D load, both or neither, as simparity.mjs
   gives sim.js both with --scripts; `*snd` may be NULL when only the scripts are asked */
static int scripts_now(const WIKICOBSRC** src, const WIKISNDSRC** snd)
{
    const WIKI3D* w = NULL;
    const int st = s_assets == ASSETS_FAILED ? WIKISIDE_REFUSED : tagpu_wiki3d_answer(&w);
    *src = NULL;
    if (snd) *snd = NULL;
    if (st == WIKISIDE_PENDING) return 0;
    if (w) {
        s_cobsrc.ctx = (void*)w;
        s_sndsrc.ctx = (void*)w;
        *src = &s_cobsrc;
        if (snd) *snd = &s_sndsrc;
    }
    return 1;
}

/* ---- the context -----------------------------------------------------------------------------

   THE CONTEXT A NEW SIMULATION PLAYS (WIKISIMCTX), and the text is filled for: on the mod's
   layout, the layout side the player's engine side maps to and that side's casting; with the
   fixture, or no layout, side 0 and no casting -- each with the last fill's Interface Type.
   BOUND: s_side is the byte the packet's fill read at PlayerInfo+0x95, DATA: it indexes
   side_of_engine only below WIKI_SIDE_SIDES, and the layout side it gives only below the layout's
   sides; a side that does not map, like no side at all, plays the fixture with no casting, so a
   wrong byte shows another side's tutorial and never reads outside the block.
   LIFETIME: the casting lies inside the published WIKISIDE block, immutable and never freed, so
   it outlives every simulation that holds it (WIKISIMCTX) and every WIKIWORDS taken from it.
   ORDERING: the pair is this thread's, set by the fill; a fill that is given another pair restarts
   the open window's demo (tagpu_wiki_fill), so no demo plays past one fill on a stale side or
   interface, and a start between fills (an input handler) takes the last fill's pair.
   0 while layout_now's or scripts_now's answer is pending; `*cast` gets the layout side whose
   casting the context holds, -1 none, as soon as layout_now answers, so the text takes the
   casting's names while the 3D load still runs. */
/* THE OPPONENT'S CASTING (WIKISIMCTX.rival): the first layout side other than `k`, which a game
   entry's rival types bind to; NULL with no block or no other side. BOUND: the side is below the
   layout's sides and WIKI_SIDE_SIDES, the casting array's length. Same lifetime as the player's. */
static const WIKICASTING* rival_of(const WIKISIDE* blk, int k)
{
    int r;
    for (r = 0; blk && r < blk->layout.nside && r < WIKI_SIDE_SIDES; r++)
        if (r != k) return &blk->casting[r];
    return NULL;
}

static int context_now(const WIKIDATA* d, const WIKILAYOUT** lay, int* src, WIKISIMCTX* ctx, int* cast)
{
    const WIKISIDE* blk;
    ctx->side = 0;
    ctx->cast = NULL;
    ctx->iface = s_iface;
    ctx->cob = NULL;
    ctx->snd = NULL;
    ctx->rival = NULL;
    *cast = -1;
    if (!layout_now(d, lay, src, &blk)) return 0;
    if (blk) {
        const int k = s_side >= 0 && s_side < WIKI_SIDE_SIDES ? blk->side_of_engine[s_side] : -1;
        if (k >= 0 && k < blk->layout.nside && k < WIKI_SIDE_SIDES) {
            ctx->side = k;
            ctx->cast = &blk->casting[k];
            ctx->rival = rival_of(blk, k);
            *cast = k;
        } else {
            *lay = d ? d->fixture : NULL;
            *src = *lay ? WIKISTAGE_LAYOUT_FIXTURE : WIKISTAGE_LAYOUT_NONE;
        }
    }
    return scripts_now(&ctx->cob, &ctx->snd);
}

/* wiki.js `start`: the selected demo afresh, scripted, its clock from now; in Try it yourself its
   scene with no script, the player's -- for a frame:game entry its replay, the viewer's script
   playing it or, in Try it, the player's presses. It starts on the sidebar layout layout_now
   gives, and not before it gives one: the stage says "loading…" and the fill asks again. */
static void start(void)
{
    const WIKIDATA* d = data();
    const WIKIENTRY* e = entry_of(d);
    const WIKILAYOUT* lay;
    WIKISIMCTX ctx;
    int src, cast;
    char err[TAGPU_WIKI_MSG];
    try_release_all(0);
    s_nlog = s_logCut = 0;
    sim_free();
    s_acc = 0;
    s_clockS = s_clipped = 0;
    s_pauseAt = -1;                                  /* a pause asked is for the demo it picked */
    s_want = 0;
    s_msg[0] = 0;
    s_trunc = 0;
    QueryPerformanceCounter(&s_last);
    if (!d) { s_want = 1; return; }                  /* the data loads on its own thread       */
    if (!e) return;
    if (s_demo < 0 || s_demo >= e->ndemo) { lstrcpynA(s_msg, "no demo", sizeof s_msg); return; }
    if (!context_now(d, &lay, &src, &ctx, &cast)) {
        s_want = 1;
        lstrcpynA(s_msg, "loading\xE2\x80\xA6", sizeof s_msg);
        return;
    }
    /* THE CAPTION'S WORDS, of the context the demo plays (text.js words) */
    tagpu_wikitext_words(&s_words, e, ctx.cast, ctx.iface);
    /* a refusal -- the layout, the context, the entry's binding to the casting, a scene structure
       the casting's footprint no longer fits, the resolved script's data rule, memory -- is shown
       on the stage in its own words, as wiki.js shows the mockup's (`simErr`) */
    if (e->frame == WIKI_FRAME_GAME
            ? !tagpu_wikireplay_init(&s_rep, e, &e->demo[s_demo], s_try ? WIKI_PLAY_TRY : WIKI_PLAY_DEMO, lay, &ctx, err,
                                     sizeof err)
            : !tagpu_wikisim_init(&s_sim, e, &e->demo[s_demo], !s_try, lay, &ctx, err, sizeof err)) {
        tagpu_logf("wiki: the demo %s/%s did not start - %s", e->id, e->demo[s_demo].id, err);
        lstrcpynA(s_msg, err, sizeof s_msg);
        return;
    }
    snprintf(s_sndDemo, sizeof s_sndDemo, "%s/%s%s", e->id, e->demo[s_demo].id, s_try ? " (try it)" : "");
    s_lay = lay;
    s_laySrc = src;
    s_ctx = ctx;
    s_cast = cast;
    if (e->frame == WIKI_FRAME_GAME) {
        s_repOn = 1;
        s_repSerial++;
        s_repTry = s_try;
    } else
        s_simOn = 1;
}

/* wiki.js `pick`: entry `e`, its demo `k`, playing from the start; another entry closes the
   theatre's About and its drawer */
static void pick(int e, int k)
{
    if (e != s_entry) s_about = s_drawer = 0;
    s_entry = e;
    s_demo = k;
    s_playing = 1;
    start();
}

/* the demo has ended and holds its last frame (wiki.js `held`) */
static int done(void)
{
    return s_simOn ? s_sim.done : s_repOn && s_rep.done;
}

/* the open: the tree's first entry when none is chosen (wiki.js setOpen), else the chosen demo
   afresh */
static void page_open(void)
{
    const WIKIDATA* d = data();
    int i;
    s_costMax = 0;
    s_costFrames = 0;
    s_drag = -1;
    s_saidHead = 0;
    s_about = s_drawer = 0;
    /* a sound object the load did not make, the settings muting then, is made now they do not */
    tagpu_wikisound_arm(sound_of(), (uintptr_t)g_ddraw.hwnd);
    if (d && s_entry < 0)
        for (i = 0; i < d->nnode; i++)
            if (d->node[i].entry >= 0) { pick(d->node[i].entry, 0); return; }
    start();
}

static void page_close(void)
{
    try_release_all(1);
    sim_free();
    tagpu_wikisound_stop();
    s_want = 0;
    s_drag = -1;
    tagpu_logf("wiki: the page cost this thread at most %.3f ms in one frame over %u frames",
               s_costMax, s_costFrames);
}

/* a key's number after its prefix, bounded to [0, n): -1 when it is not one */
static int key_index(const char* key, const char* prefix, int n)
{
    size_t k = strlen(prefix);
    const char* p;
    int v = 0;
    if (strncmp(key, prefix, k) || !key[k]) return -1;
    for (p = key + k; *p; p++) {
        if (*p < '0' || *p > '9' || v > 100000) return -1;
        v = v * 10 + (*p - '0');
    }
    return v < n ? v : -1;
}

/* the control acts on the selected entry: its meaning is the entry it was drawn for */
static int entry_relative(const char* key)
{
    return !strncmp(key, "demo:", 5) || !strcmp(key, "play") || !strcmp(key, "replay") || !strcmp(key, "try") ||
           !strcmp(key, "about");
}

/* A left press on control `key` of the last frame drawn (wiki.js's handlers). Selecting an entry
   or a demo leaves Try it yourself, as the tree's and the demo list's handlers do; its button
   swaps the demo for Try it and back.
   A PRESS ON AN ENTRY'S CONTROL COUNTS ONLY ON A FRAME DRAWN FOR THE ENTRY SELECTED NOW: the hit
   map lags a selection by the frames the render thread has not drawn yet, and `demo:1` of the
   entry the player saw is not `demo:1` of the one just picked. The hit map names the entry its
   frame drew (TAGPU_CMD_WIKI.entry, the record's), compared with what page_fill writes. */
static void page_press(const char* key)
{
    const WIKIDATA* d = data();
    const int folds = (int)(TAGPU_WIKI_FOLDS * 32);
    int i;
    if (entry_relative(key) && s_hit.entry != (entry_of(d) ? s_entry : -1)) return;
    if (!strcmp(key, "play") || !strcmp(key, "replay") || !strcmp(key, "x1") || !strcmp(key, "x2") ||
        !strcmp(key, "try"))
        held(key);
    if (!d) return;
    if ((i = key_index(key, "fold:", d->nnode < folds ? d->nnode : folds)) >= 0) {
        button_sound();
        s_fold[i >> 5] ^= 1u << (i & 31);
    } else if ((i = key_index(key, "entry:", d->nnode)) >= 0) {
        /* a pick from the drawer closes it, as every pick of an entry closes About and the drawer */
        button_sound();
        s_about = s_drawer = 0;
        if (d->node[i].entry >= 0 && d->node[i].entry != s_entry) {
            s_try = 0;
            pick(d->node[i].entry, 0);
        }
    } else if (entry_of(d) && (i = key_index(key, "demo:", entry_of(d)->ndemo)) >= 0) {
        button_sound();
        s_try = 0;
        pick(s_entry, i);
    } else if (!strcmp(key, "try")) {
        button_sound();
        if (entry_of(d) && s_demo >= 0 && s_demo < entry_of(d)->ndemo) {
            s_try = !s_try;
            s_playing = 1;
            start();
        }
    } else if (!strcmp(key, "play")) {
        button_sound();
        if (done()) start();
        else s_playing = !s_playing;
    } else if (!strcmp(key, "replay")) {
        button_sound();
        s_playing = 1;
        start();
    } else if (!strcmp(key, "x1")) {
        button_sound();
        s_speed = 1;
    } else if (!strcmp(key, "x2")) {
        button_sound();
        s_speed = 2;
    } else if (!strcmp(key, "about")) {
        /* About and the drawer: opening one closes the other (wiki.js `panel`) */
        button_sound();
        s_about = !s_about;
        s_drawer = 0;
    } else if (!strcmp(key, "drawer")) {
        button_sound();
        s_drawer = !s_drawer;
        s_about = 0;
    }
}

/* EVERY SCROLL IS CLAMPED TO THE LIMITS OF THE FRAME THE PLAYER SAW: the hit map's, taken from
   the last frame drawn. The layout holds it to its own limit again when it draws. A hit map that
   drew no wiki (a frame without the layer, or no command record) has no limits and no thumb, and
   the scroll stays where it is: a drag held across one does not jump to the top. */
static void scroll_to(int i, float v)
{
    const float mx = s_hit.scroll_max[i];
    if (!s_hit.drawn) return;
    s_scroll[i] = v < 0 ? 0 : v > mx ? mx : v;
}

/* A STEP STARTS FROM THE SCROLL ASKED, not the scroll drawn: several wheel notches or track pages
   between two hit maps each move it, where starting from the drawn one would count them as one.
   The asked scroll is held to the drawn limit first, since that limit can have shrunk under it (a
   shorter entry, a taller screen), so the step moves what the player sees from its first notch. */
static void scroll_by(int i, float d)
{
    scroll_to(i, s_scroll[i]);
    scroll_to(i, s_scroll[i] + d);
}

/* `notches` of the wheel over the panel under the pointer */
static void wheel(double notches)
{
    static const char* const PANEL[TAGPU_WIKI_NSCROLL] = { "scroll:tree", "scroll:demos", "scroll:text" };
    int cx, cy, i, k;
    if (!mouse_last_client(&cx, &cy)) return;
    for (i = 0; i < (int)s_hit.n; i++) {
        const TAGPU_CMD_WIKIHIT* h = &s_hit.hit[i];
        if (cx < h->x0 || cx >= h->x1 || cy < h->y0 || cy >= h->y1) continue;
        for (k = 0; k < (int)TAGPU_WIKI_NSCROLL; k++)
            if (!strncmp(h->key, PANEL[k], TAGPU_WIKI_KEY)) {
                scroll_by(k, (float)notches * WHEEL_UNITS);
                return;
            }
    }
}

/* A left press on a scrollbar of the last frame drawn, `real` when by the real path: 1 = taken. On
   the thumb it holds it for a drag; elsewhere on the track it pages toward the press. */
static int bar_press(int cx, int cy, int real)
{
    int i;
    if (cx < 0 || !s_hit.drawn) return 0;
    for (i = 0; i < (int)TAGPU_WIKI_NSCROLL; i++) {
        const TAGPU_CMD_WIKIBAR* b = &s_hit.bar[i];
        if (b->y1 <= b->y0 || cx < b->x0 || cx >= b->x1 || cy < b->y0 || cy >= b->y1) continue;
        if (cy >= b->thumb_y && cy < b->thumb_y + b->thumb_h) {
            s_drag = i;
            s_dragReal = real;
            s_dragY = cy;
            s_dragFrom = s_hit.scroll[i];
        } else {
            const float page = TRACK_PAGE * (float)(b->y1 - b->y0) / (s_hit.u > 0 ? s_hit.u : 1);
            scroll_by(i, cy < b->thumb_y ? -page : page);
        }
        return 1;
    }
    return 0;
}

/* the held thumb follows the pointer to client row `cy`, by the travel the drawn frame gave it */
static void bar_drag(int cy)
{
    if (s_drag < 0 || s_drag >= (int)TAGPU_WIKI_NSCROLL) return;
    scroll_to(s_drag, s_dragFrom + (float)(cy - s_dragY) * s_hit.bar[s_drag].per_px);
}

/* the demo's tick: a page demo's simulation tick, a replay's demo tick (its steps, the viewer's
   clock's and the walk's) */
static int demo_tick(void)
{
    return s_simOn ? s_sim.tick : s_repOn ? s_rep.tick : -1;
}

/* the asked pause reached: the demo stops on its tick, before the step past it */
static int pause_due(void)
{
    if (s_pauseAt < 0 || demo_tick() < s_pauseAt) return 0;
    s_playing = 0;
    s_acc = 0;
    tagpu_logf("wiki: pause %u: %s demo %d paused at tick %d", (unsigned)s_pauseN, entry_of(data())->id, s_demo + 1,
               demo_tick());
    s_pauseAt = -1;
    return 1;
}

/* REPLAY STEPS PACED BY COST, at most `want` (the demo ticks the clock owes, or the turbo's K): whole
   steps while the fill's sum of their tagpu_wikireplay_cost -- the game ticks each will run -- stays
   within WIKI_REPLAY_FILL, always at least one; the rest stay owed. THE INVARIANT: the replay is
   deterministic in demo ticks, so pacing changes only how many of them a fill runs, never what any
   of them does -- the world at demo tick T is the same however the fills fell. THE BOUND: a step
   runs at most max(WIKI_REPLAY_SEEK, WIKI_REPLAY_WALK, 8) game ticks and its cost is never fewer
   than it runs, a step whose press may start a seek charged that most (tagpu_wikireplay_cost), so
   a fill runs at most max(WIKI_REPLAY_FILL, that most) game ticks, whatever the script or the
   presses; its time is that many ticks at the dearest tick's cost. Returns the steps run. */
static int replay_steps(int want)
{
    int n = 0, spent = 0;
    while (n < want && !s_rep.done && !pause_due()) {
        const int c = tagpu_wikireplay_cost(&s_rep);
        if (n && spent + c > WIKI_REPLAY_FILL) break;
        live_flush();
        tagpu_wikireplay_step(&s_rep);
        spent += c;
        n++;
    }
    return n;
}

/* TRY IT'S PRESS on the replay (the press machine's output): it acts at once, after the pointer's
   held world point (live_flush). Leave, then Yes ends Try it yourself (decision 23): the stage holds
   the replay it left, `left`, and the page offers Try it again. A live rewind that leaves idle takes no
   key or button of the world until it is idle again, and has let go of every one L held (its open,
   and Back to now's release), so the page holds none down either (wiki.js pressOut). A fork's time,
   the press that makes it -- a rewind's, or a takeover's, which turns a takeover replay live -- is
   said: the lanes' cost (wiki_regression.py rewind, takeover). */
static void replay_press(int control, int value)
{
    const int ntl = s_rep.ntl, was_live = s_rep.live;
    LARGE_INTEGER t0, t1;
    live_flush();
    QueryPerformanceCounter(&t0);
    tagpu_wikireplay_press(&s_rep, control, value);
    QueryPerformanceCounter(&t1);
    if (s_rep.live && s_rep.ntl > ntl) {
        const double ms = s_qpf.QuadPart > 0 ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_qpf.QuadPart : 0.0;
        const WIKITIMELINE* f = &s_rep.tl[s_rep.selected];
        if (was_live) tagpu_logf("wiki: the rewind forked F%d at %d in %.3f ms", f->id, f->R, ms);
        else tagpu_logf("wiki: the takeover forked F%d at %d as seat %d in %.3f ms", f->id, f->R, s_rep.human, ms);
    }
    if (s_rep.live && s_rep.rw != WIKI_RW_IDLE) {
        s_ndown = 0;
        s_downReal = 0;
    }
    if (s_rep.left) s_try = 0;
}

/* THE TRACK'S TICK under client column `cx`: its place on the playhead's run as the last frame drawn
   it (TAGPU_CMD_WIKI hud_x0, hud_x1), clamped to the run, then rounded on the span that frame drew the
   run for -- a replay's [0, end] (hud_end), as the game's HUD maps a click (tagpu_hudinput.c seek_at),
   a live strip's [first, max(G, first)] (hud_first, hud_G), as live_seek_at maps one.
   THE ORDERING: the run, the span and rep_serial are written by the render thread in one
   tagpu_wikistate_drawn, all from the record of the one packet it drew, and reach this thread whole,
   by value, in the command record tagpu_wiki_take copies; every caller first asks frame_current, so
   the map is of the replay running now. No new lock or handshake: the command record is the only
   way in. THE STALE AXIS (THE BOUND): the run and the span are the render thread's data and turn a
   pixel into a tick only, and tagpu_wikireplay_press clamps the tick to the game again -- live, to
   the rewind's own span, whose G may be a later open's than the frame drawn -- so an axis a frame old
   costs a seek to a slightly different tick, never one outside the demo. 0 when no track was drawn. */
static int track_tick(int cx)
{
    const int x0 = s_hit.hud_x0, x1 = s_hit.hud_x1;
    double f;
    if (x1 <= x0) return 0;
    f = (double)(cx - x0) / (double)(x1 - x0);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    if (s_rep.live) {
        const int first = s_hit.hud_first, G = s_hit.hud_G > first ? s_hit.hud_G : first;
        return first + (int)(f * (double)(G - first) + 0.5);
    }
    if (s_hit.hud_end <= 0) return 0;
    return (int)(f * (double)s_hit.hud_end + 0.5);
}

/* Try it yourself on a replay: its presses are the player's, until it is done or left */
static int frame_try(void)
{
    return s_repOn && s_repTry && s_try && !s_rep.done;
}

/* THE HIT MAP IS THE RUNNING REPLAY'S: the last frame drawn was a game frame of this entry, drawn
   from a packet of this replay (s_repSerial). An ORDERING, not a timing: the render thread copies
   the serial into the map with the frame it drew from the same packet, so a map whose serial
   matches holds the stage HUD, the playhead's run and the end of the replay running now, and a
   press a frame of an earlier replay would scale is never made. */
static int frame_current(void)
{
    return s_hit.frame && s_hit.entry == s_entry && s_hit.rep_serial == s_repSerial;
}

/* A REPLAY'S PART OF THE RECORD (wiki.js drawReplay): the held view, its frame record and the
   viewer's caption. Called after the fill's last step and press: the game's view is valid only until
   the next (tagpu_wikireplay_game), and the record holds values only, so it outlives it.
   THE WORLD SHOWN: a replay's game, the watcher's view of it; a live replay's live simulation as
   the player sees it, idle or open; while the past is shown its past simulation, under the live
   view's column (the column shown at the open stays, MEASURED m0-facts.md #2), which L, never
   stepped while the past is shown, still holds.
   THE SOUND: what a seek re-simulated is heard already -- each new game and each seek's end starts
   the count at the view's `quiet` (tagpu_wikireplay.h), and only a world shown is heard. */
static void replay_fill(TAGPU_PK_WIKI* w, const WIKIENTRY* e)
{
    WIKIREPLAYVIEW rv;
    WIKISIM* g;
    const char* cap;
    uint32_t t = 0;
    int shown, mode, human;
    tagpu_wikireplay_view(&s_rep, &rv);
    mode = !rv.live ? WIKIFRAME_WATCH : rv.rw == WIKI_RW_PAST ? WIKIFRAME_PAST : WIKIFRAME_PLAYER;
    g = mode == WIKIFRAME_PAST ? tagpu_wikireplay_past(&s_rep) : tagpu_wikireplay_game(&s_rep);
    if (rv.gen != s_repGen || rv.quiet != s_repQuiet) {
        s_repGen = rv.gen;
        s_repQuiet = rv.quiet;
        s_heard = rv.quiet;
    }
    /* the human seat's view: seat 0's but in a takeover's live game, the seat taken over's */
    human = rv.live ? rv.human : 0;
    shown = rv.target < 0 && g;
    if (shown) {
        const WIKIVIEW* v = tagpu_wikisim_view_as(g, human);
        int k;
        t = tagpu_wikistage_copy(&s_frameView, v, g->entry, s_laySrc) & ~WIKISTAGE_TRUNC_CAPTION;
        s_heldGv.game = v->game;
        s_heldGv.nseat = v->game && v->nseat > 0 ? (v->nseat < WIKI_SEATS ? v->nseat : WIKI_SEATS) : 0;
        for (k = 0; k < s_heldGv.nseat; k++) {
            s_heldSeat[k] = v->seat[k];
            s_heldEcon[k] = v->econ[k];
        }
        s_heldGv.seat = s_heldSeat;
        s_heldGv.econ = s_heldEcon;
        s_heldGv.cursor_x = v->cursor_x;
        s_heldGv.cursor_z = v->cursor_z;
        s_heldGv.cursor_shape = v->cursor_shape;
        s_heldGv.cursor_since = v->cursor_since;
        sound_drain(v, g->entry);
        /* after the last read of `v`: a call on the simulation ends the view's lifetime */
        t |= tagpu_wikistage_pose(&s_frameView, g);
        s_frameView.tick = g->tick;
        if (mode == WIKIFRAME_PAST) {
            WIKISIM* L = tagpu_wikireplay_game(&s_rep);
            tagpu_wikistage_side(&s_frameView, L ? tagpu_wikisim_view_as(L, human) : NULL, s_laySrc);
        }
        s_frameHave = 1;
    }
    tagpu_wikiframe_record(&rv, s_frameHave ? &s_heldGv : NULL, s_ctx.side, s_repTry, s_press.held,
                           tagpu_wikiframe_seat_side(s_lay, s_ctx.side, human), &w->frame);
    /* THE ONE CURSOR (rewind-spec.md 9.5): the armed building's ghost goes with the simulation's cursor,
       drawn in a demo whenever it is the one drawn, and in Try it only while the player's pointer lies
       on the live frame's world (stage_world), where the stage draws the simulation's cursor at the
       pointer. try_world carries that one answer to the render thread, so the ghost and the cursor
       are never decided apart. */
    {
        double x, z;
        w->try_world = (uint8_t)(s_repTry && w->frame.simcur && w->ptr_x >= 0 && stage_world(w->ptr_x, w->ptr_y, &x, &z));
    }
    if (shown)
        tagpu_wikiframe_view(&s_frameView, s_lay, s_ctx.side, s_laySrc, mode, w->frame.simcur && (!s_repTry || w->try_world),
                             human);
    if (!s_frameHave) {
        /* a seek began before any world was shown (a capture asked mid-seek): wiki.js says the same */
        if (!w->msg[0]) lstrcpynA(w->msg, "loading\xE2\x80\xA6", sizeof w->msg);
        return;
    }
    memcpy(&w->stage, &s_frameView, sizeof w->stage);
    /* the caption is the viewer's script's, filled, or once Try it has left the replay the line that
       says where the game goes then (decision 23) */
    cap = s_rep.left ? WIKIFRAME_LEFT : rv.caption;
    w->stage.has_caption = (uint8_t)(cap && cap[0]);
    if (tagpu_wikitext_fill(cap, &s_words, w->stage.caption, sizeof w->stage.caption)) t |= WIKISTAGE_TRUNC_CAPTION;
    w->has_stage = 1;
    if (t & ~s_trunc) {
        s_trunc |= t;
        tagpu_logf("wiki: the demo %s/%s's view did not fit the record (cut 0x%03x)", e->id, e->demo[s_demo].id,
                   (unsigned)t);
    }
}

/* THE FILL STEPS THE DEMO ON THIS THREAD and copies its view by value into the record, so the
   render thread gets values and never a WIKISIM. The copy keeps every bound of
   tagpu_wikistage_copy (the map at 128 cells a side, 256 vents, 128 units, the gesture's sites,
   256 queued, 512 lines, the nano spray's 1024 particles, the caption and the labels) and of
   tagpu_wikistage_pose (the units' poses, 4096 primitives), and says once what it had to cut --
   a particle the simulation itself dropped included. */
static void page_fill(TAGPU_PK_WIKI* w)
{
    const WIKIDATA* d = data();
    const WIKIENTRY* e;
    LARGE_INTEGER now;
    double dt;
    uint32_t t;
    if (s_want && d) start();
    e = entry_of(d);
    QueryPerformanceCounter(&now);
    dt = s_qpf.QuadPart > 0 ? (double)(now.QuadPart - s_last.QuadPart) / (double)s_qpf.QuadPart : 0;
    s_last = now;
    if (dt < 0) dt = 0;
    if (dt > 0.25) dt = 0.25;
    /* the track held in Try it seeks with the pointer at each fill, as the game's HUD seeks during a
       drag (tagpu_hudinput.c), before the fill's steps so that the seek is the one they play */
    if (frame_try() && frame_current() && !strcmp(s_press.held, "track")) {
        int cx, cy, c, v;
        if (mouse_last_client(&cx, &cy) && tagpu_wikiframe_press_drag(&s_press, track_tick(cx), &c, &v))
            replay_press(c, v);
    }
    if (!demo_on() || !s_playing || done()) s_acc = 0;
    else if (s_turbo) {
        int k;
        s_acc = 0;
        if (s_repOn) replay_steps(s_turbo);
        else
            for (k = 0; k < s_turbo && !done() && !pause_due(); k++) tagpu_wikisim_step(&s_sim);
    } else if (s_repOn) {
        s_clockS += dt;
        s_acc += dt * WIKI_TPS * s_speed;
        if (s_acc > STEPS_MAX) {
            s_clipped += s_acc - STEPS_MAX;
            s_acc = STEPS_MAX;
        }
        s_acc -= replay_steps((int)s_acc);
    } else {
        s_clockS += dt;
        s_acc += dt * WIKI_TPS * s_speed;
        if (s_acc > STEPS_MAX) {
            s_clipped += s_acc - STEPS_MAX;
            s_acc = STEPS_MAX;
        }
        while (s_acc >= 1 && !done() && !pause_due()) {
            tagpu_wikisim_step(&s_sim);
            s_acc -= 1;
        }
    }
    w->entry = (int16_t)(e ? s_entry : -1);
    w->demo = (int16_t)(e && s_demo >= 0 && s_demo < e->ndemo ? s_demo : -1);
    w->playing = (uint8_t)s_playing;
    w->done = (uint8_t)done();
    w->speed = (uint8_t)s_speed;
    w->try_it = (uint8_t)s_try;
    w->mod = (int16_t)tagpu_profiles_mod();
    lstrcpynA(w->held, s_held, sizeof w->held);
    memcpy(w->fold, s_fold, sizeof w->fold);
    memcpy(w->scroll, s_scroll, sizeof w->scroll);
    lstrcpynA(w->msg, s_msg, sizeof w->msg);
    w->shot = s_shot;
    w->rep_serial = s_repSerial;
    w->about = (uint8_t)s_about;
    w->drawer = (uint8_t)s_drawer;
    w->leave_held = (uint8_t)(!frame_try() ? 0 : !strcmp(s_press.held, "yes") ? 1 : !strcmp(s_press.held, "no") ? 2 : 0);
    memset(&w->frame, 0, sizeof w->frame);
    if (s_repOn && e) replay_fill(w, e);
    else if (s_simOn && e) {
        const WIKIVIEW* v = tagpu_wikisim_view(&s_sim);
        /* the entry the simulation plays, bound to its casting: the type names the stage labels
           are the casting's (tagpu_wikisim.h); copied by value, as the view is */
        t = tagpu_wikistage_copy(&w->stage, v, s_sim.entry, s_laySrc) & ~WIKISTAGE_TRUNC_CAPTION;
        /* THE CAPTION IS SHOWN FILLED (tagpu_wikitext.h); the view keeps it raw, as the hash does */
        if (tagpu_wikitext_fill(v->caption, &s_words, w->stage.caption, sizeof w->stage.caption))
            t |= WIKISTAGE_TRUNC_CAPTION;
        sound_drain(v, s_sim.entry);
        /* after the last read of `v`: a call on the simulation ends the view's lifetime */
        t |= tagpu_wikistage_pose(&w->stage, &s_sim);
        w->stage.tick = s_sim.tick;
        w->has_stage = 1;
        if (t & ~s_trunc) {
            s_trunc |= t;
            tagpu_logf("wiki: the demo %s/%s's view did not fit the record (cut 0x%03x)", e->id,
                       e->demo[s_demo].id, (unsigned)t);
        }
    }
}

/* a UTF-8 string -- a layout's name, a casting's type or title, a filled caption -- as a JSON
   string: its bytes as they are, which is how the readers take them (sim.js counts a name's UTF-8
   bytes against WIKI_SIDE_NAME and a title's against TITLE_MAX), the rest escaped */
static void ustr(FILE* f, const char* s, size_t n)
{
    size_t i;
    fputc('"', f);
    for (i = 0; i < n && s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 0x20 || ch == 0x7F) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

/* A layout as sim.js's schema writes it (research/notes/ingame-wiki.md,
   *art/wiki/side/<name>.json*): what tagpu_wikidata_layout reads back, and what simparity.mjs's
   --layout and --try give sim.js. Screens by name, in their index order, which a JSON object
   keeps. */
static void layout_json(FILE* f, const WIKILAYOUT* L)
{
    /* sim.js ROLES, by WIKI_ROLE_* */
    static const char* const ROLE[WIKI_NROLES] = { "orders", "buildtab", "build", "prev", "next", "stop", "order" };
    int i, j, k, n;
    fprintf(f, "{\"format\":1,\"width\":%d,\"height\":%d,\"sides\":[", L->width, L->height);
    for (i = 0; i < L->nside && i < WIKI_SIDE_SIDES; i++) {
        const WIKISIDEDEF* s = &L->side[i];
        fprintf(f, "%s{\"name\":", i ? "," : "");
        ustr(f, s->name, WIKI_SIDE_NAME);
        fprintf(f, ",\"prefix\":");
        ustr(f, s->prefix, WIKI_SIDE_NAME);
        fprintf(f, ",\"main2\":");
        ustr(f, L->screen[s->main2].name, WIKI_SIDE_NAME);
        fprintf(f, ",\"gen\":");
        ustr(f, L->screen[s->gen].name, WIKI_SIDE_NAME);
        fprintf(f, ",\"types\":{");
        for (j = 0, n = 0; j < L->ntype && j < WIKI_SIDE_TYPES; j++) {
            const WIKIPAGES* t = &L->type[j];
            if (t->side != i) continue;
            fprintf(f, "%s", n++ ? "," : "");
            ustr(f, t->name, WIKI_SIDE_NAME);
            fprintf(f, ":{\"pages\":[");
            for (k = 0; k < t->npage && k < WIKI_SIDE_PAGES; k++) {
                fprintf(f, "%s", k ? "," : "");
                ustr(f, L->screen[t->page[k]].name, WIKI_SIDE_NAME);
            }
            fprintf(f, "],\"orders\":");
            if (t->orders >= 0) ustr(f, L->screen[t->orders].name, WIKI_SIDE_NAME);
            else fputs("null", f);
            fputc('}', f);
        }
        fputs("}}", f);
    }
    fprintf(f, "],\"screens\":{");
    for (i = 0; i < L->nscreen && i < WIKI_SIDE_SCREENS; i++) {
        const WIKISCREEN* s = &L->screen[i];
        fprintf(f, "%s", i ? "," : "");
        ustr(f, s->name, WIKI_SIDE_NAME);
        fprintf(f, ":{\"y\":%d,\"w\":%d,\"h\":%d,\"gadgets\":[", s->y, s->w, s->h);
        for (j = 0; j < s->ngadget && j < WIKI_SIDE_GADGETS; j++) {
            const WIKIGADGET* g = &s->gadget[j];
            fprintf(f, "%s{\"name\":", j ? "," : "");
            ustr(f, g->name, WIKI_SIDE_NAME);
            fprintf(f, ",\"role\":\"%s\",\"rect\":[%d,%d,%d,%d]", g->role >= 0 && g->role < WIKI_NROLES ? ROLE[g->role] : "",
                    g->x, g->y, g->w, g->h);
            if (g->role == WIKI_ROLE_BUILD) {
                fprintf(f, ",\"unit\":");
                if (g->unit[0]) ustr(f, g->unit, WIKI_SIDE_NAME);
                else fputs("null", f);
            }
            fprintf(f, ",\"grey\":%s", g->grey ? "true" : "false");
            /* a quickkey as the pack writes one: only when the button has it */
            if (g->key) fprintf(f, ",\"key\":%d", g->key);
            fprintf(f, ",\"hidden\":%s}", g->hidden ? "true" : "false");
        }
        fprintf(f, "]}");
    }
    fprintf(f, "},\"sight\":");
    if (!L->sight.numtables) fputs("null", f);
    else {
        fprintf(f, "{\"numtables\":%d,\"tables\":[", L->sight.numtables);
        for (i = 1; i < L->sight.numtables && i < WIKI_SIGHT_TABLES; i++) {
            fprintf(f, "%s[", i > 1 ? "," : "");
            for (k = L->sight.first[i - 1]; k < L->sight.first[i] && k < WIKI_SIGHT_CELLS; k++)
                fprintf(f, "%s[%d,%d,%d]", k > L->sight.first[i - 1] ? "," : "", L->sight.cell[k].dc, L->sight.cell[k].dr,
                        L->sight.cell[k].n);
            fputc(']', f);
        }
        fputs("]}", f);
    }
    fputc('}', f);
}

static double ms_between(LARGE_INTEGER t0, LARGE_INTEGER t1)
{
    return s_qpf.QuadPart > 0 ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_qpf.QuadPart : 0.0;
}

/* what the check says after a demo's hash: each unit type whose script its simulation quarantined,
   and a wait that ran past its max */
static void check_notes(const WIKIENTRY* e, const WIKIDEMO* dm, int k, int ifc, const WIKISIM* s)
{
    int ty;
    for (ty = 0; s && s->cob && ty < e->ndef; ty++)
        if (tagpu_wikicob_note(s->cob, ty)[0])
            tagpu_logf("wiki check: %s/%s/%d/%d %s's script is quarantined - %s", e->id, dm->id, k, ifc,
                       s->entry->def[ty].name, tagpu_wikicob_note(s->cob, ty));
    if (s && s->stalled)
        tagpu_logf("wiki check: %s/%s/%d/%d stopped: a wait ran past its max", e->id, dm->id, k, ifc);
}

/* the game ticks a step of a replay simulated, read off its views before (a) and after (b). A
   watched replay: the walker's and the shown game's, a backward seek's new game (a new gen)
   counted from its tick 0. A live one steps either the live game L or, in the past, the past
   simulation P, never both (tagpu_wikireplay.c live_play): while P stands, P's ticks, a new P (a
   new past_gen) counted from its tick 0, since L stands still in the past; otherwise L's, and on a
   fork's step, where P becomes L and L's gen becomes P's, from the tick P stood at, so the step
   counts only what it simulated rather than the whole of L's tick. */
static long replay_ran(const WIKIREPLAYVIEW* a, const WIKIREPLAYVIEW* b)
{
    if (!b->live) return (b->walked - a->walked) + (b->gen != a->gen ? b->game_tick : b->game_tick - a->game_tick);
    if (b->has_past) return b->past_tick - (a->has_past && a->past_gen == b->past_gen ? a->past_tick : 0);
    if (b->gen == a->gen) return b->game_tick - a->game_tick;
    return b->game_tick - (a->has_past && a->past_gen == b->gen ? a->past_tick : 0);
}

/* A GAME ENTRY'S DEMO IN THE CHECK: played as the page will play it, through the replay layer
   (tagpu_wikireplay.h) with the viewer's script, to its end; its tagpu_wikireplay_hash logged as
   every hash is, and then its cost -- the game ticks its steps ran (replay_ran) and the
   milliseconds, the start's two simulations apart and the dearest step named -- the measurement
   WIKI_REPLAY_SEEK, _WALK and _FILL are set from. A step's ticks are read off its view before and
   after, outside the timed call. 1 when it started. */
static int check_replay(const WIKIENTRY* e, const WIKIDEMO* dm, int k, int ifc, const WIKILAYOUT* lay,
                        const WIKISIMCTX* ctx)
{
    WIKIREPLAY* r = (WIKIREPLAY*)malloc(sizeof *r);
    WIKIREPLAYVIEW* v = (WIKIREPLAYVIEW*)malloc(2 * sizeof *v);
    LARGE_INTEGER t0, t1;
    char err[160];
    long ticks = 0, steps = 0, worstTicks = 0, ran;
    double ms = 0, worstMs = 0, startMs, stepMs;
    QueryPerformanceCounter(&t0);
    if (!r || !v || !tagpu_wikireplay_init(r, e, dm, WIKI_PLAY_DEMO, lay, ctx, err, sizeof err)) {
        tagpu_logf("wiki check: %s/%s/%d/%d did not start - %s", e->id, dm->id, k, ifc, r && v ? err : "no memory");
        free(r);
        free(v);
        return 0;
    }
    QueryPerformanceCounter(&t1);
    startMs = ms_between(t0, t1);
    tagpu_wikireplay_view(r, &v[0]);
    /* a scripted replay is done at its end clock; the bound holds a script that never is */
    while (!v[0].done && steps < 1000000L) {
        QueryPerformanceCounter(&t0);
        tagpu_wikireplay_step(r);
        QueryPerformanceCounter(&t1);
        stepMs = ms_between(t0, t1);
        tagpu_wikireplay_view(r, &v[1]);
        ran = replay_ran(&v[0], &v[1]);
        if (ran < 0) ran = 0;           /* a backward seek that failed left no game: its view says tick 0 */
        ticks += ran;
        ms += stepMs;
        steps++;
        if (stepMs > worstMs) { worstMs = stepMs; worstTicks = ran; }
        v[0] = v[1];
    }
    tagpu_logf("wiki check: %s/%s/%d/%d %016llx", e->id, dm->id, k, ifc, (unsigned long long)tagpu_wikireplay_hash(r));
    tagpu_logf("wiki check: the replay %s/%s/%d/%d ran %ld game ticks in %ld steps in %.3f ms (%.2f us a tick), "
               "started in %.3f ms; its dearest step %ld game ticks in %.3f ms", e->id, dm->id, k, ifc, ticks, steps, ms,
               ticks > 0 ? ms * 1000.0 / (double)ticks : 0.0, startMs, worstTicks, worstMs);
    check_notes(e, dm, k, ifc, tagpu_wikireplay_game(r));
    if (tagpu_wikireplay_game(r) && (tagpu_wikireplay_game(r)->starved || tagpu_wikireplay_game(r)->bad_kill))
        tagpu_logf("wiki check: %s/%s/%d/%d stopped: %s", e->id, dm->id, k, ifc,
                   tagpu_wikireplay_game(r)->starved ? "an economy pass could not pay a grant" : "a kill found no unit");
    if (v[0].stalled) tagpu_logf("wiki check: %s/%s/%d/%d stopped: the viewer's wait ran past its max", e->id, dm->id, k, ifc);
    if (v[0].failed) tagpu_logf("wiki check: %s/%s/%d/%d stopped: a backward seek could not make the game again", e->id,
                                dm->id, k, ifc);
    tagpu_wikireplay_free(r);
    free(r);
    free(v);
    return 1;
}

/* THE PARITY CHECK OF THE CODE THE GAME RUNS (tools/wiki_regression.py demos): every demo of every
   entry played to its end at once, here on the game thread in the DLL's i686 SSE2 build, on the
   sidebar layout a demo would start on (layout_now) with the units' scripts it would start with
   (scripts_now; the caller waits for both), for every side of
   that layout -- with its casting on the mod's layout -- at both Interface Types, and its
   tagpu_wikisim_hash logged as `<entry>/<demo>/<side>/<iface> <hash>`: the hash
   `node tools/wiki/simparity.mjs --hash --side <side> --iface <iface>` prints for `<entry>/<demo>`
   (with --pack, on the mod's layout). A game entry's demo is played as a replay (check_replay), as
   the page plays it. The layout is written to CHECK_LAYOUT first, for simparity's --layout (null:
   none). */
static void check(void)
{
    const WIKIDATA* d = data();
    const WIKILAYOUT* lay = NULL;
    const WIKISIDE* blk = NULL;
    const WIKICOBSRC* cob = NULL;
    const WIKISNDSRC* snd = NULL;
    LARGE_INTEGER t0, t1;
    char err[160];
    int i, j, k, ifc, nside, n = 0, nreplay = 0, bad = 0, src = WIKISTAGE_LAYOUT_NONE;
    FILE* f;
    if (!d) { tagpu_logf("wiki check: the wiki's data is not loaded"); return; }
    layout_now(d, &lay, &src, &blk);
    scripts_now(&cob, &snd);
    nside = lay && lay->nside > 0 ? (lay->nside < WIKI_SIDE_SIDES ? lay->nside : WIKI_SIDE_SIDES) : 1;
    if ((f = fopen(CHECK_LAYOUT ".tmp", "wb"))) {
        if (lay) layout_json(f, lay);
        else fputs("null", f);
        fputc('\n', f);
        if (fclose(f) != 0 || !MoveFileExA(CHECK_LAYOUT ".tmp", CHECK_LAYOUT, MOVEFILE_REPLACE_EXISTING))
            tagpu_logf("wiki check: %s could not be written (%lu)", CHECK_LAYOUT, (unsigned long)GetLastError());
    }
    tagpu_logf("wiki check: the sidebar layout is %s, in %s; the units' scripts are %s, their sounds %s",
               src == WIKISTAGE_LAYOUT_MOD ? "the mod's" : src == WIKISTAGE_LAYOUT_FIXTURE ? "the fixture" : "none",
               CHECK_LAYOUT, cob ? "on" : "off", snd ? "on" : "off");
    QueryPerformanceCounter(&t0);
    for (k = 0; k < nside; k++)
        for (ifc = 0; ifc <= 1; ifc++)
            for (i = 0; i < d->nentry; i++)
                for (j = 0; j < d->entry[i].ndemo; j++) {
                    const WIKIENTRY* e = &d->entry[i];
                    /* the contexts start() can give: the side's casting only on the mod's layout */
                    const WIKISIMCTX ctx = { k, blk ? &blk->casting[k] : NULL, ifc, cob, snd, rival_of(blk, k) };
                    WIKISIM* s;
                    long t = 0;
                    if (e->frame == WIKI_FRAME_GAME) {
                        if (check_replay(e, &e->demo[j], k, ifc, lay, &ctx)) n++, nreplay++;
                        else bad++;
                        continue;
                    }
                    s = (WIKISIM*)malloc(sizeof *s);
                    if (!s || !tagpu_wikisim_init(s, e, &e->demo[j], 1, lay, &ctx, err, sizeof err)) {
                        tagpu_logf("wiki check: %s/%s/%d/%d did not start - %s", e->id, e->demo[j].id, k, ifc,
                                   s ? err : "no memory");
                        free(s);
                        bad++;
                        continue;
                    }
                    /* a scripted demo is done at its end tick; the bound holds a script that never is */
                    while (!s->done && t++ < 1000000L) tagpu_wikisim_step(s);
                    tagpu_logf("wiki check: %s/%s/%d/%d %016llx", e->id, e->demo[j].id, k, ifc,
                               (unsigned long long)tagpu_wikisim_hash(s));
                    check_notes(e, &e->demo[j], k, ifc, s);
                    tagpu_wikisim_free(s);
                    free(s);
                    n++;
                }
    QueryPerformanceCounter(&t1);
    tagpu_logf("wiki check: %d demos played (%d of them as replays), %d did not start, in %.0f ms", n, nreplay, bad,
               ms_between(t0, t1));
}

/* THE LANES THE PLAYER SAW (TAGPU_CMD_WIKI nlane, lane_id), into `lane` (WIKIFRAME_FORKS ids): their
   count, each place's fork, or -1 where the place names no live fork of the running replay.
   THE ORDERING: the lanes, the run and rep_serial are written by the render thread in one
   tagpu_wikistate_drawn, all from the bounded record of the one packet it drew, and reach this
   thread whole, by value, in the command record tagpu_wiki_take copies; every caller first asks
   frame_current, so they are the drawn frame's lanes of the replay running now. No new lock,
   handshake or lifetime: the command record is the only way in. THE BOUND: tagpu_wiki_take holds
   the count to WIKIFRAME_FORKS and each id to 1..RECR_TIMELINES - 1, and here an id counts only
   while it names a live fork of this replay, so a list a frame old -- a deletion since -- costs a
   lane's press that does nothing, never a press on a fork that is gone. THE IDS ARE NEVER REUSED
   within a replay, which that rests on: a fork's id is its row's index, handed out in order by
   ntl++ (tagpu_wikireplay.c part_of with fork_at and take_over, and the kept forks at init), no
   row is taken back, and a deleted or evicted row keeps live 0 for good; a new replay moves
   rep_serial (sim_free, start), which frame_current refuses. So a stale id names its own dead
   row, never the fork that took its place in the bay; reusing rows would break this guard. And a
   lane's control acts at its release only when the lanes then drawn name the same fork at the
   same place (tagpu_hudinput.c held_lane, the press machine's rule), so a list that changed
   between the press and the release never sends it to the fork that moved into the place. */
static int drawn_lanes(int32_t* lane)
{
    int k;
    for (k = 0; k < s_hit.nlane; k++) {
        const int32_t id = s_hit.lane_id[k];
        lane[k] = s_rep.tk && id > 0 && id < s_rep.ntl && s_rep.tl[id].live ? id : -1;
    }
    return s_hit.nlane;
}

/* A LEFT PRESS ON THE STAGE HUD OR ON LEAVE'S QUESTION (src WIKIHIT_HUD, _DIALOG) at client
   column `cx`: in Try it yourself on a replay, the press machine's (tagpu_wikiframe.h), whose output
   is a press of the replay's; in a demo it is taken and does nothing, the presses being the viewer's
   script's. A control that is not one of the replay's (the HUD's containers, uiscale, a row) is
   taken by the machine and does nothing, so no press here reaches the game or the real replay HUD:
   these hits are the stage HUD's own, never tagpu_hudstate's. A lane's control takes the fork its
   lane showed (drawn_lanes). No sound (decision 9). */
static void frame_down(int src, const char* key, int cx)
{
    int32_t lane[WIKIFRAME_FORKS];
    int c, v, n;
    if (!frame_try() || !frame_current()) return;
    n = drawn_lanes(lane);
    if (tagpu_wikiframe_press_down(&s_press, src, key, 0, track_tick(cx), lane, n, &c, &v)) replay_press(c, v);
}

/* the left button's release, wherever it is made: the control held is let go, acting when the
   release lands on it (a HUD control or the question's answer, never the page's control of the
   same key) and the track seeking to where it lets go. A release while the hit map is not the
   running replay's (frame_current) lets the control go without acting. */
static void frame_up(void)
{
    char key[TAGPU_WIKI_KEY];
    int32_t lane[WIKIFRAME_FORKS];
    int cx, cy, src, c, v, T, n, on = 0;
    if (!s_press.held[0]) return;
    if (!frame_try() || !frame_current()) {
        tagpu_wikiframe_press_init(&s_press);
        return;
    }
    /* with no point known the track lets go where it last sought: no seek */
    if (mouse_last_client(&cx, &cy)) {
        on = hit_src(cx, cy, key, &src) && src != WIKIHIT_PAGE;
        T = track_tick(cx);
    } else
        T = s_press.last;
    n = drawn_lanes(lane);
    if (tagpu_wikiframe_press_up(&s_press, on ? key : "", T, lane, n, &c, &v)) replay_press(c, v);
}

/* A press of button `bit` at client point (cx, cy), engine point (gx, gy), from either path:
   1 = taken. gx < 0: the engine point is the client point's, converted here (the real path). */
static int press(unsigned bit, int cx, int cy, int gx, int gy)
{
    char key[TAGPU_WIKI_KEY];
    int src;
    if (s_open) {
        s_owedBtn |= bit;
        if (try_press(bit, cx, cy, gx < 0)) return 1;
        if (bit == B_L && !bar_press(cx, cy, gx < 0) && hit_src(cx, cy, key, &src)) {
            if (src != WIKIHIT_PAGE) frame_down(src, key, cx);
            else if (!strcmp(key, "close")) { held(key); wiki_close("the close button"); }
            else if (!strcmp(key, "wiki")) { held(key); wiki_close("the wiki button"); }
            else page_press(key);
        }
        return 1;
    }
    /* A PRESS ON A WINDOW STILL DRAWN IS THE WIKI'S, whatever the button: after a close the render
       thread draws the open page for a frame or two more and its hit map lags one more, so the
       second click of a double-click on X lands on the window the player still sees. Taken and
       owed its release, it does nothing -- the window it was aimed at is already closed -- and the
       world under that window never sees it. */
    if (s_hit.open && hit_src(cx, cy, key, &src)) {
        s_owedBtn |= bit;
        tagpu_logf("wiki: a press on '%s' of the window still drawn after its close was taken", key);
        return 1;
    }
    if (bit == B_L && hit(cx, cy, key) && !strcmp(key, "wiki")) {
        const char* busy = pause_in_flight();
        s_owedBtn |= bit;
        if (busy) {
            tagpu_logf("wiki: not opened - %s", busy);
            return 1;
        }
        held(key);
        if (gx < 0) mouse_client_to_game(cx, cy, &gx, &gy);
        wiki_open(gx, gy);
        return 1;
    }
    s_owedBtn &= ~bit;
    return 0;
}

/* cnc-ddraw's window procedure takes a real button's release, locking the mouse, and gives the
   engine nothing of it: the mouse is unlocked and the window closed */
static int relocks(void)
{
    return !s_open && !g_mouse_locked && !g_config.devmode;
}

/* A release of button `bit`, from either path: 1 = taken, TAGPU_WIKI_FROZEN = the engine's, at
   the frozen point. */
static int release(unsigned bit)
{
    int owed = (s_owedBtn & bit) != 0;
    s_owedBtn &= ~bit;
    try_release(bit);
    if (bit == B_L) frame_up();
    if (owed && bit == B_L) { s_held[0] = 0; s_drag = -1; }
    if (s_engBtn & bit) return s_open ? TAGPU_WIKI_FROZEN : 0;
    return owed || s_open;
}

/* A key-down of `vk`; `repeat` when the key was already down (lParam bit 30): 1 = taken. */
static int key_down(unsigned vk, int repeat)
{
    vk &= 0xFF;
    if (s_open) try_key(vk, 1);
    if (s_open && repeat && s_engKey[vk] && !s_owedKey[vk]) {
        s_charOwed = 1;
        return 1;
    }
    if (s_open) {
        s_owedKey[vk] = 1;
        if (vk == VK_ESCAPE) s_escClose = 1;
        s_charOwed = 1;
        return 1;
    }
    if (s_owedKey[vk] && repeat) {
        s_charOwed = 1;
        return 1;
    }
    s_owedKey[vk] = 0;
    s_charOwed = 0;
    return 0;
}

static int key_up(unsigned vk)
{
    int owed;
    vk &= 0xFF;
    try_key(vk, 0);
    owed = s_owedKey[vk];
    s_owedKey[vk] = 0;
    if (owed && vk == VK_ESCAPE && s_escClose) {
        s_escClose = 0;
        if (s_open) wiki_close("Esc");
    }
    if (s_engKey[vk]) return 0;
    return owed || s_open;
}

void tagpu_wiki_engine_got(UINT msg, WPARAM wp)
{
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: s_engBtn |= B_L; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: s_engBtn |= B_R; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: s_engBtn |= B_M; break;
    case WM_LBUTTONUP: s_engBtn &= ~B_L; break;
    case WM_RBUTTONUP: s_engBtn &= ~B_R; break;
    case WM_MBUTTONUP: s_engBtn &= ~B_M; break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: s_engKey[wp & 0xFF] = 1; break;
    case WM_KEYUP: case WM_SYSKEYUP: s_engKey[wp & 0xFF] = 0; break;
    }
}

static int character(void)
{
    int owed = s_charOwed;
    s_charOwed = 0;
    return s_open || owed;
}

/* a real release's answer: a taken one goes on when it re-locks the mouse */
static int paired_up(int took)
{
    return took == 1 && relocks() ? 0 : took;
}

int tagpu_wiki_message(UINT msg, WPARAM wp, LPARAM lp)
{
    int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        return key_down((unsigned)wp, (lp & (1L << 30)) != 0);
    case WM_KEYUP:
    case WM_SYSKEYUP:
        return key_up((unsigned)wp);
    case WM_CHAR:
    case WM_DEADCHAR:
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
        return character();
    case WM_MOUSEMOVE:
        /* THE DRAG ENDS WITH ITS BUTTON. A real press's release can be lost -- made outside the
           window with the mouse unlocked, it is never sent here -- but every real move carries the
           button's state, so the first move with the left button up ends a drag a real press
           began. An injected press's release always comes by the injected path (deliver_mouse
           hands every TAGPU_M_LUP to tagpu_wiki_mouse), which ends its drag in release(). */
        if (s_drag >= 0 && s_dragReal && !(wp & MK_LBUTTON)) s_drag = -1;
        if (s_open) { note_client(cx, cy); bar_drag(cy); try_pointer(cx, cy, 1, wp); }
        return s_open;
    case WM_MOUSEWHEEL:
        /* a notch is WHEEL_DELTA; up scrolls toward the top */
        if (s_open) wheel(-(double)(short)HIWORD(wp) / WHEEL_DELTA);
        return s_open;
    case WM_INPUT:
    case WM_MOUSEHOVER:
        return s_open;
    /* THE ENGINE'S POINT OF A REAL PRESS is its client point in lParam converted as cnc-ddraw
       converts one (mouse_client_to_game, in press): this runs at the top of the window procedure,
       before cnc-ddraw has unscaled the message, and g_ddraw.cursor is not that point while the
       mouse is unlocked -- windowed, after the focus came back -- since no move reaches it then */
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK: {
        unsigned bit = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ? B_L
                     : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK ? B_R
                     : msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK ? B_M : B_X;
        if (s_open) note_client(cx, cy);
        return press(bit, cx, cy, -1, -1);
    }
    /* THE RELEASE THAT RE-LOCKS THE MOUSE goes on once the window is closed: with the mouse
       unlocked, cnc-ddraw's window procedure takes every button's release, the left, right and
       middle one's to lock it, and hands the engine none (wndproc.c, "button up messages
       reactivate cursor lock"). While the window is open it is taken all the same, since the
       lock's store of the release's point would move g_ddraw.cursor off the frozen one; the next
       click after the close locks it, as any click does unlocked. */
    case WM_LBUTTONUP:
        return paired_up(release(B_L));
    case WM_RBUTTONUP:
        return paired_up(release(B_R));
    case WM_MBUTTONUP:
        return paired_up(release(B_M));
    case WM_XBUTTONUP:
        return paired_up(release(B_X));
    }
    return 0;
}

int tagpu_wiki_mouse(int code, int cx, int cy, int gx, int gy)
{
    switch (code) {
    case TAGPU_M_MOVE:
        if (s_open) { note_client(cx, cy); if (cx >= 0) bar_drag(cy); try_pointer(cx, cy, 0, 0); }
        return s_open;
    case TAGPU_M_WHEELUP:
    case TAGPU_M_WHEELDN:
        if (s_open) wheel(code == TAGPU_M_WHEELUP ? -1.0 : 1.0);
        return s_open;
    case TAGPU_M_LDOWN:
    case TAGPU_M_RDOWN:
    case TAGPU_M_MDOWN:
        if (s_open) note_client(cx, cy);
        return press(code == TAGPU_M_LDOWN ? B_L : code == TAGPU_M_RDOWN ? B_R : B_M, cx, cy, gx, gy);
    case TAGPU_M_LUP:
        return release(B_L);
    case TAGPU_M_RUP:
        return release(B_R);
    case TAGPU_M_MUP:
        return release(B_M);
    }
    return s_open;
}

int tagpu_wiki_key(unsigned vk, int up)
{
    /* an injected key never repeats: each down is a press of its own. And it posts no character
       for the wiki to take: deliver_key makes the WM_CHAR itself, only for a key it delivers */
    int took = up ? key_up(vk) : key_down(vk, 0);
    s_charOwed = 0;
    return took;
}

int tagpu_wiki_char(void)
{
    return character();
}

void tagpu_wiki_cancel(void)
{
    s_held[0] = 0;
    s_drag = -1;
    tagpu_wikiframe_press_init(&s_press);
    try_release_all(1);
}

/* a number the render thread posted, as data: finite and in [0, hi], else 0 */
static float bounded(float v, float hi)
{
    return v >= 0 && v <= hi ? v : 0;
}

void tagpu_wiki_take(const TAGPU_CMD_WIKI* c)
{
    int i;
    if (!c) { s_hit.n = 0; s_hit.drawn = 0; s_hit.nlane = 0; return; }
    s_hit = *c;
    if (s_hit.n > TAGPU_CMD_WIKIHITS) s_hit.n = TAGPU_CMD_WIKIHITS;
    s_hit.u = bounded(s_hit.u, 1000);
    if (s_hit.side_h < 0 || s_hit.side_h > WIKI_SIDE_ROWS) s_hit.side_h = 0;
    s_hit.frame = (uint8_t)(s_hit.frame != 0);
    if (s_hit.hud_end < 0 || s_hit.hud_end > WIKI_GAME_END_MAX) s_hit.hud_end = 0;
    if (s_hit.hud_first < 0 || s_hit.hud_G < s_hit.hud_first || s_hit.hud_G > WIKI_GAME_END_MAX)
        s_hit.hud_first = s_hit.hud_G = 0;
    if (s_hit.nlane > WIKIFRAME_FORKS) s_hit.nlane = WIKIFRAME_FORKS;
    for (i = 0; i < WIKIFRAME_FORKS; i++)
        if (i >= s_hit.nlane || s_hit.lane_id[i] < 1 || s_hit.lane_id[i] >= RECR_TIMELINES) s_hit.lane_id[i] = -1;
    for (i = 0; i < (int)TAGPU_WIKI_NSCROLL; i++) {
        s_hit.scroll_max[i] = bounded(s_hit.scroll_max[i], 1e6f);
        s_hit.scroll[i] = bounded(s_hit.scroll[i], s_hit.scroll_max[i]);
        s_hit.bar[i].per_px = bounded(s_hit.bar[i].per_px, 1e6f);
    }
}

/* ---- the record ------------------------------------------------------------------------------ */

static void jstr(FILE* f, const char* s, size_t n)
{
    size_t i;
    fputc('"', f);
    for (i = 0; i < n && s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 0x20 || ch >= 0x7F) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

/* sim.js's name of a WIKI_KEY_*, "" for none */
static const char* key_name(int k)
{
    const char* s = tagpu_wikidata_key_name(k);
    return s ? s : "";
}

/* A casting as a pack's casting record writes it ({"casts": {...}}, tools/wiki/sidepacks.py): what
   tagpu_wikidata_casting reads back and simparity.mjs --try gives sim.js. Each figure at 17
   significant digits, so the double read back is the one written. */
static void casting_json(FILE* f, const WIKICASTING* c)
{
    int i, j, x, z, n;
    fputs("{\"casts\":{", f);
    for (i = 0; i < WIKI_NCAST; i++) {
        const WIKICASTDEF* k = &c->cast[i];
        fprintf(f, "%s\"%s\":", i ? "," : "", tagpu_wikidata_cast_name(i));
        if (!k->bound) { fputs("null", f); continue; }
        fputs("{\"type\":", f);
        ustr(f, k->type, sizeof k->type);
        fputs(",\"title\":", f);
        ustr(f, k->title, sizeof k->title);
        fprintf(f, ",\"footprint\":[%d,%d],\"buildTime\":%.17g,\"workerTime\":%.17g,\"buildDistance\":%.17g,"
                   "\"maxVelocity\":%.17g,\"extracts\":%.17g,\"energyMake\":%.17g,\"energyUse\":%.17g,"
                   "\"buildCostMetal\":%.17g,\"buildCostEnergy\":%.17g,\"metalMake\":%.17g,\"metalStorage\":%.17g,"
                   "\"energyStorage\":%.17g,\"canstop\":%d,\"canreclamate\":%d,\"activatewhenbuilt\":%d,"
                   "\"sightDistance\":%d,\"radarDistance\":%d,\"sonarDistance\":%d,\"modelHeight\":%d,\"vent\":%s,\"g\":[",
                k->fx, k->fz, k->build_time, k->worker_time, k->build_distance, k->max_velocity, k->extracts,
                k->energy_make, k->energy_use, k->build_cost_metal, k->build_cost_energy, k->metal_make, k->metal_storage,
                k->energy_storage, k->canstop, k->canreclamate, k->activate_when_built, k->sight_distance, k->radar_distance,
                k->sonar_distance, k->model_height, k->vent ? "true" : "false");
        for (z = n = 0; z < WIKI_FP_MAX; z++)
            for (x = 0; x < WIKI_FP_MAX; x++)
                if (k->g[z] >> x & 1u) fprintf(f, "%s[%d,%d]", n++ ? "," : "", x, z);
        fputs("],\"builds\":[", f);
        for (j = n = 0; j < WIKI_NCAST; j++)
            if (k->builds >> j & 1u) fprintf(f, "%s\"%s\"", n++ ? "," : "", tagpu_wikidata_cast_name(j));
        fputs("],\"corpse\":", f);
        if (!k->has_corpse) fputs("null", f);
        else {
            const WIKIFEATDEF* w = &k->corpse;
            fputs("{\"name\":", f);
            ustr(f, w->name, sizeof w->name);
            fprintf(f, ",\"footprint\":[%d,%d],\"metal\":%.17g,\"energy\":%.17g,\"height\":%d,\"blocking\":%s,"
                       "\"reclaimable\":%s", w->fx, w->fz, w->metal, w->energy, w->height, w->blocking ? "true" : "false",
                    w->reclaimable ? "true" : "false");
            if (w->object[0]) {
                fputs(",\"object\":", f);
                ustr(f, w->object, sizeof w->object);
            }
            fputc('}', f);
        }
        fputc('}', f);
    }
    fputs("}}", f);
}

/* a frame record as tests/wikiframetest.c's dump_rec writes it, which tools/wiki/framecheck.mjs reads */
static void frame_json(FILE* f, const WIKIFRAMEREC* r)
{
    int k;
    fprintf(f, "{\"on\":%d,\"playing\":%d,\"question\":%d,\"nseat\":%d,\"side\":%d,\"colour\":%d,\"speed\":%d,"
               "\"try_it\":%d,\"playhead\":%d,\"end\":%d,\"seat\":[", r->on, r->playing, r->question, r->nseat, r->side,
            r->colour, r->speed, r->try_it, (int)r->playhead, (int)r->end);
    for (k = 0; k < r->nseat && k < WIKI_SEATS; k++) {
        const WIKIFRAMESEAT* z = &r->seat[k];
        fprintf(f, "%s{\"name\":", k ? "," : "");
        ustr(f, z->name, sizeof z->name);
        fprintf(f, ",\"colour\":%d,\"lost\":%d,\"out\":%d,\"units\":%.9g,\"m\":%.9g,\"mmax\":%.9g,\"mi\":%.9g,"
                   "\"mo\":%.9g,\"e\":%.9g,\"emax\":%.9g,\"ei\":%.9g,\"eo\":%.9g}", z->colour, z->lost, z->out, z->units,
                z->m, z->mmax, z->mi, z->mo, z->e, z->emax, z->ei, z->eo);
    }
    fprintf(f, "],\"nmark\":%d,\"mark\":[", r->nmark);
    for (k = 0; k < r->nmark && k < WIKIFRAME_MARKS; k++)
        fprintf(f, "%s{\"t\":%d,\"kind\":%d,\"seat\":%d}", k ? "," : "", (int)r->mark[k].t, r->mark[k].kind, r->mark[k].seat);
    fprintf(f, "],\"held\":");
    jstr(f, r->held, sizeof r->held);
    fprintf(f, ",\"toast\":");
    jstr(f, r->toast, sizeof r->toast);
    fprintf(f, ",\"cur\":{\"on\":%d,\"from\":", r->cur.on);
    jstr(f, r->cur.from, sizeof r->cur.from);
    fprintf(f, ",\"to\":");
    jstr(f, r->cur.to, sizeof r->cur.to);
    fprintf(f, ",\"from_v\":%d,\"to_v\":%d,\"watched\":%d,\"i\":%d,\"n\":%d}", (int)r->cur.from_v, (int)r->cur.to_v,
            (int)r->cur.watched, r->cur.i, r->cur.n);
    fprintf(f, ",\"live\":%d,\"rw\":%d,\"arm\":%d,\"first\":%d,\"G\":%d,\"now\":%d,\"last\":%d,\"armR\":%d,"
               "\"bar\":{\"e\":%.9g,\"m\":%.9g,\"ei\":%.9g,\"eo\":%.9g,\"mi\":%.9g,\"mo\":%.9g,\"emax\":%.9g,"
               "\"mmax\":%.9g,\"raw_e\":%.9g,\"raw_m\":%.9g},\"simcur\":%d,\"sim\":{\"x\":%.9g,\"z\":%.9g,"
               "\"shape\":%d,\"age\":%d}", r->live, r->rw, r->arm, (int)r->first, (int)r->G, (int)r->now, (int)r->last,
            (int)r->arm_R, r->bar.e, r->bar.m, r->bar.ei, r->bar.eo, r->bar.mi, r->bar.mo, r->bar.emax, r->bar.mmax,
            r->bar.raw_e, r->bar.raw_m, r->simcur, r->sim.x, r->sim.z, r->sim.shape, (int)r->sim.age);
    fprintf(f, ",\"tk\":%d,\"pick\":%d,\"lanes\":%d,\"nfork\":%d,\"human\":%d,\"fork_max\":%d,\"tk_full\":%d,"
               "\"chain_last\":%d,\"fill_to\":%d,\"pickR\":%d,\"pickEvict\":%d,\"fork\":[", r->tk, r->pick, r->lanes, r->nfork,
            r->human, r->fork_max, r->tk_full, (int)r->chain_last, (int)r->fill_to, (int)r->pick_R, (int)r->pick_evict);
    for (k = 0; k < r->nfork && k < WIKIFRAME_FORKS; k++)
        fprintf(f, "%s{\"id\":%d,\"from\":%d,\"end\":%d,\"parent\":%d,\"seat\":%d,\"flags\":%d}", k ? "," : "",
                (int)r->fork[k].id, (int)r->fork[k].from, (int)r->fork[k].end, (int)r->fork[k].parent, r->fork[k].seat,
                r->fork[k].flags);
    fprintf(f, "],\"top_side\":%d}", r->top_side);
}

/* A timeline's last log entry as [kind, key] (a move's key ""), or null for an empty log: what a
   lane waits on to know its click was logged, and so applied (LOGGED FIRST, tagpu_wikireplay.h),
   rather than on a count other entries can reach. Read on this thread, the one that logs. */
static void log_tail(FILE* f, const WIKITIMELINE* t)
{
    static const char* const KIND[] = { "move", "down", "up", "release" };
    const WIKIREPLAYLOG* e;
    if (!t || t->nlog <= 0 || !t->log) { fputs("null", f); return; }
    e = &t->log[t->nlog - 1];
    fprintf(f, "[\"%s\",", e->kind >= 0 && e->kind <= WIKI_LOG_RELEASE ? KIND[e->kind] : "?");
    jstr(f, e->kind == WIKI_LOG_DOWN || e->kind == WIKI_LOG_UP ? key_name(e->key) : "", 16);
    fputc(']', f);
}

/* A LIVE REPLAY'S STATE, for the lanes (tools/wiki_regression.py rewind, takeover): the rewind, the
   arm, the timelines and the past simulation as the replay's view gives them, the human seat, and
   the world shown -- the past simulation's while the past is shown, else the live one's -- as the
   human seat sees it (tagpu_wikisim_view_as) and the lane reads it back: its tick, the seat's armed
   type, the units with their owners, raw selection flags (every seat's) and build, the seat's
   sites, the economy's mode, the seat's keys held and its column. Called after the fill, so the
   view taken here is read before any step can free its simulation. */
static void live_json(FILE* f, const WIKIREPLAYVIEW* rv)
{
    WIKISIM* g = rv->rw == WIKI_RW_PAST ? tagpu_wikireplay_past(&s_rep) : tagpu_wikireplay_game(&s_rep);
    const WIKIVIEW* v;
    const WIKIENTRY* en;
    int i, n;
    fprintf(f, ",\"live\":{\"rw\":%d,\"G\":%d,\"first\":%d,\"last\":%d,\"now\":%d,\"arm\":%d,\"arm_R\":%d,"
               "\"arm_evict\":%d,\"drag_t\":%d,\"clash\":%d,\"nlogged\":%d,\"selected\":%d,\"full\":%d,\"back\":%d,"
               "\"toast_evict\":%d,\"human\":%d,\"past\":", rv->rw, rv->G, rv->first, rv->last, rv->now, rv->arm, rv->arm_R,
            rv->arm_evict, rv->drag_t, rv->clash, rv->nlogged, rv->selected, rv->full, rv->back, rv->toast_evict, rv->human);
    if (rv->has_past) fprintf(f, "{\"tick\":%d,\"target\":%d,\"gen\":%d}", rv->past_tick, rv->past_target, rv->past_gen);
    else fputs("null", f);
    fprintf(f, ",\"timelines\":[");
    for (i = 0; i < rv->ntl && i < RECR_TIMELINES; i++) {
        fprintf(f, "%s{\"id\":%d,\"parent\":%d,\"R\":%d,\"last\":%d,\"live\":%d,\"used\":%d,\"mode\":%d,\"nlog\":%d,"
                   "\"seat\":%d,\"tail\":", i ? "," : "", rv->tl[i].id, rv->tl[i].parent, rv->tl[i].R, rv->tl[i].last, rv->tl[i].live,
                rv->tl[i].used, rv->tl[i].mode, rv->tl[i].nlog, rv->tl[i].seat);
        log_tail(f, i < s_rep.ntl ? &s_rep.tl[i] : NULL);
        fputc('}', f);
    }
    fprintf(f, "],\"world\":");
    if (!g) {
        fputs("null}", f);
        return;
    }
    v = tagpu_wikisim_view_as(g, rv->human >= 0 ? rv->human : 0);
    en = g->entry;
    fprintf(f, "{\"tick\":%d,\"mode\":", g->tick);
    jstr(f, v->mode >= 0 && v->mode < en->ndef ? en->def[v->mode].name : "", 64);
    fprintf(f, ",\"econ_mode\":%d,\"starved\":%d,\"has_ghost\":%d,\"ghost_ok\":%d,\"nsite\":%d,\"nqueued\":%d,"
               "\"cursor\":[%.9g,%.9g],\"held\":[", v->econ_mode, v->starved, v->has_ghost, v->has_ghost && v->ghost.ok,
            v->nsite, v->nqueued, v->cursor_x, v->cursor_z);
    for (i = n = 0; i < WIKI_NKEYS; i++)
        if (v->held[i]) {
            if (n++) fputc(',', f);
            jstr(f, key_name(i), 16);
        }
    fprintf(f, "],\"units\":[");
    for (i = 0; i < v->nunit; i++) {
        const WIKIVUNIT* u = &v->unit[i];
        fprintf(f, "%s{\"id\":", i ? "," : "");
        jstr(f, u->id ? u->id : "", WIKI_ID_MAX);
        fprintf(f, ",\"type\":");
        jstr(f, u->type >= 0 && u->type < en->ndef ? en->def[u->type].name : "", 64);
        fprintf(f, ",\"owner\":%d,\"selected\":%d,\"x\":%.9g,\"z\":%.9g,\"built\":%.9g}", u->owner, u->selected, u->x,
                u->z, u->built);
    }
    fprintf(f, "],\"side\":");
    if (v->has_side && s_lay && v->side.screen >= 0 && v->side.screen < s_lay->nscreen) {
        fprintf(f, "{\"screen\":");
        jstr(f, s_lay->screen[v->side.screen].name, WIKI_SIDE_NAME);
        fprintf(f, ",\"unit\":");
        jstr(f, v->side.unit, sizeof v->side.unit);
        fprintf(f, ",\"page\":%d,\"state\":[", v->side.page);
        for (i = 0; i < v->side.nstate && i < WIKI_SIDE_GADGETS; i++) fprintf(f, "%s%d", i ? "," : "", v->side.state[i]);
        fputs("]}", f);
    } else
        fputs("null", f);
    fputs("}}", f);
}

/* A TAKEOVER REPLAY'S STATE, for the lane (tools/wiki_regression.py takeover): replay.js's view's
   `tk` block in its names (tests/wikitest.c prints the same for simparity) -- the mode, the human
   seat, the picker and its plan, the lanes, the armed fork, the selected timeline, its chain's last
   and the fill, the rows' serial, the forks saved, the background walk and the walker's tick, the
   takeover's warning, and the timelines with their seats -- in either mode. */
static void tk_json(FILE* f, const WIKIREPLAYVIEW* rv)
{
    static const char* const PICK[] = { "closed", "asked", "seeking", "open" };
    int i;
    fprintf(f, ",\"tk\":{\"mode\":\"%s\",\"human\":%d,\"pick\":\"%s\",\"pickR\":%d,\"pickEvict\":%d,\"pickParent\":%d,"
               "\"pickTl\":%d,\"pickSerial\":%d,\"lanes\":%d,\"armed\":%d,\"selected\":%d,\"chainLast\":%d,\"fillTo\":%d,"
               "\"serial\":%d,\"saved\":[", rv->tk_mode == WIKI_TK_LIVE ? "live" : "watch", rv->human,
            PICK[rv->pick >= 0 && rv->pick <= WIKI_PICKER_OPEN ? rv->pick : 0], rv->pick_R, rv->pick_evict, rv->pick_parent,
            rv->pick_tl, rv->pick_serial, rv->lanes, rv->armed, rv->selected, rv->chain_last, rv->fill_to, rv->serial);
    for (i = 0; i < rv->nsaved && i < RECR_TIMELINES; i++) fprintf(f, "%s%d", i ? "," : "", rv->saved[i]);
    fprintf(f, "],\"bg\":%d,\"walker\":%d,\"tkFull\":%d,\"timelines\":[", rv->bg, rv->walker_tick, rv->tk_full);
    for (i = 0; i < rv->ntl && i < RECR_TIMELINES; i++)
        fprintf(f, "%s{\"id\":%d,\"parent\":%d,\"R\":%d,\"last\":%d,\"live\":%d,\"used\":%d,\"mode\":%d,\"seat\":%d}",
                i ? "," : "", rv->tl[i].id, rv->tl[i].parent, rv->tl[i].R, rv->tl[i].last, rv->tl[i].live, rv->tl[i].used,
                rv->tl[i].mode, rv->tl[i].seat);
    fputs("]}", f);
}

/* THE STATE, FOR THE TESTS (tools/wiki_regression.py): written when tagpu_wiki.dump appears, after
   the fill's record is placed in the packet (`carried`: the bytes of it the packet holds), on this
   thread, as the replay HUD's tagpu_replayhud.dump is. The engine screen's size
   and the viewport let a test turn an engine point into the client point a device click needs. */
static void dump(const TAGPU_PK_WIKI* w, uint32_t carried)
{
    FILE* f = fopen(DUMP_OUT ".tmp", "wb");
    const WIKIDATA* d = data();
    const WIKIENTRY* e = entry_of(d);
    LONG fx, fy;
    unsigned i, k;
    if (!f) { tagpu_logf("wiki: the state dump could not open %s", DUMP_OUT ".tmp"); return; }
    tagpu_wiki_frozen(&fx, &fy);
    fprintf(f, "{\"open\":%d,\"holds\":%ld,\"paused\":%d,\"pause_was\":%d,\"single_player\":%d,"
               "\"held\":", s_open, (long)tagpu_wiki_holds_input(), s_paused, s_pauseWas, tagpu_menu_single_player());
    jstr(f, s_held, sizeof s_held);
    fprintf(f, ",\"frozen\":[%ld,%ld],\"ptr\":[%d,%d],\"owed_buttons\":%u,\"carried\":%u,",
            (long)fx, (long)fy, w->ptr_x, w->ptr_y, s_owedBtn, (unsigned)carried);
    fprintf(f, "\"owed_keys\":[");
    for (i = k = 0; i < 256; i++)
        if (s_owedKey[i]) fprintf(f, "%s%u", k++ ? "," : "", i);
    fprintf(f, "],\"game\":[%u,%u],\"viewport\":[%d,%d,%d,%d],",
            (unsigned)g_ddraw.width, (unsigned)g_ddraw.height,
            (int)g_ddraw.render.viewport.x, (int)g_ddraw.render.viewport.y,
            (int)g_ddraw.render.viewport.width, (int)g_ddraw.render.viewport.height);
    /* the page: the selection by the data's ids, the folds, the scrolls asked and as drawn */
    fprintf(f, "\"entry\":");
    jstr(f, e ? e->id : "", 64);
    fprintf(f, ",\"demo\":");
    jstr(f, e && s_demo >= 0 && s_demo < e->ndemo ? e->demo[s_demo].id : "", 64);
    fprintf(f, ",\"folds\":[");
    for (i = k = 0; i < TAGPU_WIKI_FOLDS * 32; i++)
        if (s_fold[i >> 5] >> (i & 31) & 1u) fprintf(f, "%s%u", k++ ? "," : "", i);
    fprintf(f, "],\"scroll\":[%.3f,%.3f,%.3f],\"scroll_drawn\":[%.3f,%.3f,%.3f],\"scroll_max\":[%.3f,%.3f,%.3f],",
            s_scroll[0], s_scroll[1], s_scroll[2], s_hit.scroll[0], s_hit.scroll[1], s_hit.scroll[2],
            s_hit.scroll_max[0], s_hit.scroll_max[1], s_hit.scroll_max[2]);
    /* the demo: its clock and its state hash (tagpu_wikisim_hash, a replay's tagpu_wikireplay_hash),
       and what the page cost */
    fprintf(f, "\"playing\":%d,\"speed\":%d,\"sim\":%d,\"tick\":%d,\"done\":%d,\"hash\":\"%016llx\","
               "\"stage\":%d,\"cost_max_ms\":%.4f,\"cost_frames\":%u,\"clock_s\":%.6f,\"clipped\":%.6f,"
               "\"turbo\":%d,\"pause_at\":%d,\"about\":%d,\"drawer\":%d,\"msg\":",
            s_playing, s_speed, demo_on(), demo_tick(), done(),
            s_simOn ? (unsigned long long)tagpu_wikisim_hash(&s_sim)
                    : s_repOn ? (unsigned long long)tagpu_wikireplay_hash(&s_rep) : 0ull,
            w->has_stage, s_costMax, s_costFrames, s_clockS, s_clipped, s_turbo, s_pauseAt, s_about, s_drawer);
    jstr(f, s_msg, sizeof s_msg);
    /* a replay: its transport, and the frame record this fill placed, in tools/wiki/framecheck.mjs's
       shape (tests/wikiframetest.c dump_rec) */
    if (s_repOn) {
        WIKIREPLAYVIEW rv;
        tagpu_wikireplay_view(&s_rep, &rv);
        fprintf(f, ",\"replay\":{\"tick\":%d,\"clock\":%d,\"game_tick\":%d,\"end\":%d,\"playhead\":%d,"
                   "\"paused\":%d,\"speed\":%d,\"target\":%d,\"gen\":%d,\"walked\":%d,\"left\":%d,\"try\":%d,"
                   "\"press_held\":", rv.tick, rv.clock, rv.game_tick, rv.end, rv.playhead, rv.paused, rv.speed,
                rv.target, rv.gen, rv.walked, rv.left, s_repTry);
        jstr(f, s_press.held, sizeof s_press.held);
        if (rv.live) live_json(f, &rv);
        if (rv.tk) tk_json(f, &rv);
        fprintf(f, "},\"try_world\":%d,\"frame\":", w->try_world);
        frame_json(f, &w->frame);
    }
    /* the caption as the record carries it, filled, its UTF-8 as it is */
    fprintf(f, ",\"caption\":");
    ustr(f, w->has_stage ? w->stage.caption : "", sizeof w->stage.caption);
    /* Try it yourself: its held keys, every input given its simulation with its tick (the form
       tools/wiki/simparity.mjs --try reads), and the unit the hit map's frame was drawn at */
    fprintf(f, ",\"try\":%d,\"try_down\":[", s_try);
    for (i = 0; i < (unsigned)s_ndown; i++) {
        fprintf(f, "%s", i ? "," : "");
        jstr(f, key_name(s_down[i]), 16);
    }
    fprintf(f, "],\"try_cut\":%d,\"try_log\":[", s_logCut);
    for (i = 0; i < (unsigned)s_nlog; i++) {
        const TRYIN* t = &s_log[i];
        fprintf(f, "%s[%ld,", i ? "," : "", (long)t->tick);
        if (t->kind == TRY_MOVE) fprintf(f, "\"move\",%.17g,%.17g]", t->x, t->z);
        else {
            fprintf(f, "\"%s\",", t->kind == TRY_DOWN ? "down" : "up");
            jstr(f, key_name(t->key), 16);
            fprintf(f, "]");
        }
    }
    /* the stage's fit as stage_world takes it: the drawn frame's unit, map and sidebar rows */
    fprintf(f, "],\"u\":%.17g,\"map\":[%d,%d],\"side_h\":%d,\"view_drawn\":%d", (double)s_hit.u, e ? e->scenes[0].w : 0,
            e ? e->scenes[0].h : 0, (int)s_hit.side_h, (int)s_hit.view);
    /* a whole game frame as the drawn frame fitted it, from the same canvas and the same function the
       render thread draws it with (tagpu_wikistage_fit, `frame` 1): the canvas's client corner, the
       scale, the map's origin on the canvas and the frame's size in game pixels -- what the lanes
       locate its column, its bars and its HUD by in a capture */
    if (s_hit.frame && s_repOn && s_frameHave) {
        const TAGPU_CMD_WIKIHIT* h = NULL;
        for (i = 0; i < s_hit.n && !h; i++)
            if (!strncmp(s_hit.hit[i].key, "stage", TAGPU_WIKI_KEY)) h = &s_hit.hit[i];
        if (h) {
            WIKISTAGEFIT ft;
            int Wg, Hg;
            tagpu_wikistage_fit(s_frameView.w, s_frameView.h, s_hit.side_h, 1, h->x1 - h->x0, h->y1 - h->y0, s_hit.u, &ft);
            tagpu_wikiframe_size(s_frameView.w, s_frameView.h, s_hit.side_h, &Wg, &Hg);
            fprintf(f, ",\"frame_fit\":{\"canvas\":[%d,%d],\"s\":%.17g,\"ox\":%.17g,\"oz\":%.17g,\"Wg\":%d,\"Hg\":%d}",
                    h->x0, h->y0, ft.s, ft.ox, ft.oz, Wg, Hg);
        }
    }
    /* the sidebar layout the simulation was started with, as simparity.mjs --try gives it to sim.js
       (null: none), and which one it is; and its context, sim.js's ctx, the casting written whole */
    fprintf(f, ",\"layout_src\":\"%s\"", !demo_on() ? "" : s_laySrc == WIKISTAGE_LAYOUT_MOD ? "mod"
               : s_laySrc == WIKISTAGE_LAYOUT_FIXTURE ? "fixture" : "none");
    fprintf(f, ",\"engine_side\":%d,\"engine_iface\":%d", s_side, s_iface);
    if (demo_on()) {
        fprintf(f, ",\"layout\":");
        if (s_lay) layout_json(f, s_lay);
        else fputs("null", f);
        fprintf(f, ",\"ctx\":{\"side\":%d,\"iface\":%d,\"casting\":", s_ctx.side, s_ctx.iface);
        if (s_ctx.cast) casting_json(f, s_ctx.cast);
        else fputs("null", f);
        /* whether the sim runs its units' scripts, which simparity --try then runs from the packs */
        fprintf(f, ",\"cob\":%s}", s_ctx.cob ? "true" : "false");
    }
    fprintf(f, ",\"bars\":[");
    for (i = 0; i < TAGPU_WIKI_NSCROLL; i++) {
        const TAGPU_CMD_WIKIBAR* b = &s_hit.bar[i];
        fprintf(f, "%s{\"track\":[%d,%d,%d,%d],\"thumb\":[%d,%d],\"per_px\":%.5f}", i ? "," : "",
                b->x0, b->y0, b->x1, b->y1, b->thumb_y, b->thumb_h, b->per_px);
    }
    fprintf(f, "],\"drawn\":%u,\"drawn_open\":%u,\"drawn_entry\":%d,\"drawn_frame\":%u,\"hud_run\":[%d,%d,%ld],"
               "\"hud_span\":[%ld,%ld],\"hud_lanes\":[", (unsigned)s_hit.drawn, (unsigned)s_hit.open, (int)s_hit.entry,
            (unsigned)s_hit.frame, s_hit.hud_x0, s_hit.hud_x1, (long)s_hit.hud_end, (long)s_hit.hud_first, (long)s_hit.hud_G);
    for (i = 0; i < s_hit.nlane && i < WIKIFRAME_FORKS; i++) fprintf(f, "%s%ld", i ? "," : "", (long)s_hit.lane_id[i]);
    fprintf(f, "],\"hits\":[");
    for (i = 0; i < s_hit.n; i++) {
        const TAGPU_CMD_WIKIHIT* h = &s_hit.hit[i];
        fprintf(f, "%s{\"key\":", i ? "," : "");
        jstr(f, h->key, sizeof h->key);
        fprintf(f, ",\"rect\":[%d,%d,%d,%d],\"src\":%d}", h->x0, h->y0, h->x1, h->y1, h->src);
    }
    fprintf(f, "]}\n");
    if (fclose(f) != 0 || !MoveFileExA(DUMP_OUT ".tmp", DUMP_OUT, MOVEFILE_REPLACE_EXISTING)) {
        tagpu_logf("wiki: the state dump could not be written (%lu)", (unsigned long)GetLastError());
        return;
    }
    tagpu_logf("wiki: state dumped to %s - open=%d paused=%d hits=%u", DUMP_OUT, s_open, s_paused, s_hit.n);
}

/* An ask of `ENTRY DEMO TICK` (SHOT_ASK, PAUSE_ASK) read and removed: 1 with the entry's index, its
   demo's number from 1 and the tick, 0 with the refusal said */
static int ask_read(const char* file, int* e, int* demo, int* tick)
{
    const WIKIDATA* d = data();
    char buf[128], id[64];
    DWORD n = 0;
    HANDLE h = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    int i;
    *e = -1;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL)) n = 0;
    CloseHandle(h);
    DeleteFileA(file);
    buf[n] = 0;
    if (!d || sscanf(buf, "%63s %d %d", id, demo, tick) != 3) {
        tagpu_logf("wiki: %s: `%s` is not `ENTRY DEMO TICK`, or the data is not loaded - nothing asked", file, buf);
        return 0;
    }
    for (i = 0; i < d->nentry; i++)
        if (!strcmp(d->entry[i].id, id)) *e = i;
    if (*e < 0 || *demo < 1 || *demo > d->entry[*e].ndemo || *tick < 0 || *tick > SHOT_TICK_MAX) {
        tagpu_logf("wiki: %s: no entry %s with a demo %d, or tick %d is past %d - nothing asked", file, id, *demo, *tick,
                   SHOT_TICK_MAX);
        return 0;
    }
    return 1;
}

/* the capture SHOT_ASK asks for: its demo picked, played to its tick and paused */
static void shot(void)
{
    int e, demo, tick;
    if (!ask_read(SHOT_ASK, &e, &demo, &tick)) return;
    s_try = 0;
    s_pauseAt = -1;
    pick(e, demo - 1);
    /* a replay is stepped as the mockup's capture steps it, with no fill between: a capture taken
       while a seek is pending shows no world, as the mockup's does (replay_fill) */
    while (s_simOn && !s_sim.done && s_sim.tick < tick) tagpu_wikisim_step(&s_sim);
    while (s_repOn && !s_rep.done && s_rep.tick < tick) tagpu_wikireplay_step(&s_rep);
    s_playing = 0;
    s_shot++;
    tagpu_logf("wiki: shot %u asked: %s demo %d held at tick %d", (unsigned)s_shot, data()->entry[e].id, demo,
               demo_tick());
}

/* the pause PAUSE_ASK asks for: its demo picked afresh and let play, to pause on its tick (pause_due) */
static void pause_ask(void)
{
    int e, demo, tick;
    if (!ask_read(PAUSE_ASK, &e, &demo, &tick)) return;
    s_try = 0;
    pick(e, demo - 1);
    s_pauseN++;
    s_pauseAt = demo_on() ? tick : -1;
    tagpu_logf("wiki: pause %u asked: %s demo %d to pause at tick %d%s", (unsigned)s_pauseN, data()->entry[e].id, demo,
               tick, demo_on() ? "" : " - the demo did not start, nothing asked");
}

/* TURBO_ON, read on the triggers' beat: its K while the file is there, said when it changes */
static void turbo_read(void)
{
    char buf[16];
    int k = 0;
    if (tagpu_opt_read(TURBO_ON, buf, sizeof buf) > 0 && sscanf(buf, "%d", &k) == 1)
        k = k < 1 ? 1 : k > (int)STEPS_MAX ? (int)STEPS_MAX : k;
    if (k == s_turbo) return;
    s_turbo = k;
    if (k) tagpu_logf("wiki: %s: a playing demo steps %d tick(s) a fill, not the clock's", TURBO_ON, k);
    else tagpu_logf("wiki: %s: gone - the demo plays on the clock", TURBO_ON);
}

/* THE 3D ASSET LOAD (tagpu_wikiasset.h), started once a process once the wiki's data is loaded,
   by the first fill of an open window or by the parity check's trigger (CHECK_ASK), which needs
   the sidebar's answer with the window closed: the engine's mount list, its language and folder names
   (tagpu_replays.c, which reads the same list for running_mod) and the current directory are
   copied here, on the game thread that owns them, and handed to the loader thread by value with
   the data and WIKIPAK, both immutable for the process. Nothing here waits for the load. */
static void assets_start(void)
{
    const WIKIDATA* d = data();
    WIKI3DIN* in;
    DWORD n;
    if (s_assets != ASSETS_NONE || !d) return;
    if (!(in = (WIKI3DIN*)calloc(1, sizeof *in))) {
        /* no loader thread, so no sidebar answer will ever be published: the fixture stands */
        s_assets = ASSETS_FAILED;
        tagpu_logf("wiki3d: NOT loaded - out of memory for its input; the sidebar is the fixture's");
        return;
    }
    s_assets = ASSETS_STARTED;
    in->narc = tagpu_replays_archive_paths(in->arc, TAGPU_VFS_ARCMAX);
    tagpu_replays_asset_names(&in->names);
    n = GetCurrentDirectoryA(sizeof in->cwd, in->cwd);
    if (!n || n >= sizeof in->cwd) {
        in->cwd[0] = 0;
        tagpu_logf("wiki3d: the current directory is not one the loader can name - only archives are read");
    }
    in->data = d;
    in->pak = tagpu_pak_resource("WIKIPAK", &in->npak);
    in->hwnd = (uintptr_t)g_ddraw.hwnd;
    in->sound_off = tagpu_wikisound_muted();
    tagpu_logf("wiki3d: loading from %d archives on a thread of its own", in->narc);
    tagpu_wikiasset_start(in);
}

/* 1 when the packet carried nothing of the last fill tagpu_wiki_carried was told of, not even its
   head, so the frame drawn from it shows no wiki and posts an empty hit map */
static int s_cut;
static int s_shown;                     /* tagpu_wiki_shown                                      */
static int s_ask;                       /* this fill answers the triggers                        */

int tagpu_wiki_fill(TAGPU_PK_WIKI* w, uint32_t* len, int again, int side, int iface)
{
    static unsigned calls;
    int cx, cy;
    s_ask = (calls++ % 30u) == 0u;
    if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
    /* the check plays on the layout and with the scripts a demo would start on: it starts the asset
       load if the wiki has not, and the trigger waits until both answers are in */
    if (s_ask && GetFileAttributesA(CHECK_ASK) != INVALID_FILE_ATTRIBUTES) {
        const WIKILAYOUT* lay;
        const WIKICOBSRC* cob;
        int src;
        assets_start();
        if (layout_now(data(), &lay, &src, NULL) && scripts_now(&cob, NULL)) {
            DeleteFileA(CHECK_ASK);
            check();
        }
    }
    /* A WINDOW THE LAST PUBLISHED PACKET COULD NOT CARRY CLOSES HERE, the first fill after it: that
       frame drew no wiki and took no input to it, so the window held the input with nothing to see
       or to close. A fill done again for the same publish (`again`, the slot grew under it) follows
       one that was discarded, never published, so it does not count; the fill kept for a publish
       is the last one told to tagpu_wiki_carried. */
    if (!again) {
        const int cut = s_cut;
        s_cut = 0;
        if (cut && s_open) wiki_close("its record did not fit the frame packet");
    }
    if (!tagpu_hudstate_on() || tagpu_wikistate_refused()) {
        /* THE WIKI IS OPEN ONLY WHERE IT CAN BE SEEN AND CLOSED: the lane fell back to GDI, the
           HUD's pass refused, or the wiki's layout refused, so no frame will draw the window
           again */
        s_shown = 0;
        if (s_open) wiki_close("the wiki can no longer be drawn");
        if (!s_saidOff) {
            s_saidOff = 1;
            tagpu_logf("wiki: not drawn - %s", tagpu_wikistate_refused() ? "its layout refused the kit"
                                                                          : "the replay HUD's pass is off");
        }
        return 0;
    }
    if (s_open) QueryPerformanceCounter(&s_costT0);
    else s_costT0.QuadPart = 0;
    /* THE PLAYER'S CONTEXT MOVED: the open window's demo restarts on the new pair, before the page
       is filled (*the context*) */
    iface = iface == 1;
    if (side != s_side || iface != s_iface) {
        s_side = side;
        s_iface = iface;
        tagpu_logf("wiki: the player's side is %d and the interface type %d%s", side, iface,
                   s_open ? " - the demo restarts" : "");
        if (s_open) start();
    }
    memset(w, 0, TAGPU_PK_WIKI_HEAD);
    w->open = (uint8_t)s_open;
    w->paused = (uint8_t)s_paused;
    w->ptr_x = w->ptr_y = -1;
    w->entry = w->demo = -1;
    w->iface = (uint8_t)s_iface;
    w->cast_side = -1;
    w->config = (uint8_t)tagpu_menu_bar_cog();
    if (mouse_last_client(&cx, &cy)) {
        w->ptr_x = (int16_t)(cx < -32768 ? -32768 : cx > 32767 ? 32767 : cx);
        w->ptr_y = (int16_t)(cy < -32768 ? -32768 : cy > 32767 ? 32767 : cy);
    }
    lstrcpynA(w->held, s_held, sizeof w->held);
    if (s_open) {
        const WIKILAYOUT* lay;
        WIKISIMCTX ctx;
        int src, cast;
        assets_start();
        /* a capture waits, as a demo does, for the context it would start on, and so does a pause */
        if (s_ask && GetFileAttributesA(SHOT_ASK) != INVALID_FILE_ATTRIBUTES && context_now(data(), &lay, &src, &ctx, &cast))
            shot();
        if (s_ask && GetFileAttributesA(PAUSE_ASK) != INVALID_FILE_ATTRIBUTES && context_now(data(), &lay, &src, &ctx, &cast))
            pause_ask();
        if (s_ask) turbo_read();
        page_fill(w);
        /* the text's context, by value: the running demo's, else the one a demo would start on */
        if (demo_on()) {
            ctx = s_ctx;
            cast = s_cast;
        } else
            context_now(data(), &lay, &src, &ctx, &cast);
        w->iface = (uint8_t)(ctx.iface == 1);
        w->cast_side = (int8_t)(cast >= 0 && cast < WIKI_SIDE_SIDES ? cast : -1);
    }
    *len = w->has_stage ? (uint32_t)sizeof *w : TAGPU_PK_WIKI_HEAD;
    return 1;
}

uint32_t tagpu_wiki_head_only(TAGPU_PK_WIKI* w)
{
    w->has_stage = 0;
    if (!w->msg[0]) lstrcpynA(w->msg, "the demo did not fit", sizeof w->msg);
    if (!s_saidHead) {
        s_saidHead = 1;
        tagpu_logf("wiki: the frame packet could not carry the demo's view - the page is drawn without it (said once an open)");
    }
    return TAGPU_PK_WIKI_HEAD;
}

void tagpu_wiki_carried(const TAGPU_PK_WIKI* w, uint32_t carried)
{
    if (s_costT0.QuadPart) {
        LARGE_INTEGER t1;
        double ms;
        QueryPerformanceCounter(&t1);
        ms = (double)(t1.QuadPart - s_costT0.QuadPart) * 1000.0 / (double)s_qpf.QuadPart;
        if (ms > s_costMax) s_costMax = ms;
        s_costFrames++;
        s_costT0.QuadPart = 0;
    }
    s_cut = carried == 0;
    s_shown = carried != 0;
    if (s_ask && GetFileAttributesA(DUMP_ASK) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(DUMP_ASK);
        dump(w, carried);
    }
    if (s_ask) tagpu_wikiasset_poll();
}

int tagpu_wiki_shown(void)
{
    return s_shown;
}

void tagpu_wiki_level_end(void)
{
    if (s_open) {
        s_paused = 0;           /* the level it was taken in is ending: nothing is written */
        wiki_close("the level ended");
    }
    try_release_all(0);
    sim_free();
    s_held[0] = 0;
    s_drag = -1;
    s_escClose = 0;
    memset(&s_hit, 0, sizeof s_hit);
}
