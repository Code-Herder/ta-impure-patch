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

   THE ORDERING. The open and the close are each one interlocked store of the capture word, here
   on the game thread. Every poll -- the engine mouse thread's GetCursorPos every ~33 ms among them
   -- reads the word with an interlocked read and comes wholly before or after that store: before,
   it answers the live device; after, the frozen one. The frozen point is stored before the word,
   so a poll that sees the word set reads that open's point. Input given before the open reached
   the game; input given after it does not; there is no moment that is both.

   THE PAIRED PRESS AND THE PAIRED KEY. A press the wiki takes has its release taken too, whether
   the wiki is still open or not (s_owedBtn, s_owedKey), so the engine is never handed a release
   whose press it never saw, nor misses one for a press it did. The wiki button and the close
   button act on the press, as the sprocket does. Esc closes on its RELEASE, and only the release
   of an Esc whose press the wiki took: the press, its auto-repeats and the WM_CHAR TranslateMessage
   posts for it all arrive while the window is still open, and none reaches the engine. A press
   the wiki does not take of a button or key it is owed ends that debt, as tagpu_menu_click's does:
   its release was lost (dragged off the window, focus taken), and the next one is the engine's.

   THE PAUSE (below, above wiki_open) is held in single player and nowhere else.

   THE PAGE AND THE STAGE (below, *the page*): the selection, the folds, the scrolls and the
   demo's playback are this thread's, changed only by a press on a control of the last frame drawn
   (the hit map tagpu_wiki_take copied), so a click lands on what the player saw. The demo's
   simulation (tagpu_wikisim.c) lives here too and is stepped inside the packet's fill; the render
   thread gets a view of it by value in the record and never touches a WIKISIM.

   Nothing here names engine memory: the two engine facts the pause needs are tagpu_menu.c's. */
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"
#include "mouse.h"
#include "tagpu_shield.h"
#include "tagpu_menu.h"
#include "tagpu_hudstate.h"
#include "tagpu_hudart.h"
#include "tagpu_profiles.h"
#include "tagpu_log.h"
#include "tagpu_wiki.h"

#define DUMP_ASK  "tagpu_wiki.dump"
#define DUMP_OUT  "tagpu_wiki.json"
#define CHECK_ASK "tagpu_wiki.check"

enum { B_L = 1, B_R = 2, B_M = 4, B_X = 8 };

/* ---- the two words any thread reads ---------------------------------------------------------- */

static volatile LONG s_hold;            /* the capture word: 1 while the window is open         */
static volatile LONG s_fx, s_fy;        /* the frozen game point, stored before s_hold          */

LONG tagpu_wiki_holds_input(void)
{
    return InterlockedCompareExchange(&s_hold, 0, 0);
}

void tagpu_wiki_frozen(LONG* x, LONG* y)
{
    *x = InterlockedCompareExchange(&s_fx, 0, 0);
    *y = InterlockedCompareExchange(&s_fy, 0, 0);
}

/* ---- the game thread's own ------------------------------------------------------------------- */

static int  s_open;                     /* this thread's copy of s_hold                          */
static unsigned s_owedBtn;              /* B_*: buttons whose press was taken                    */
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
   NOTHING ELSE UNDOES IT WHILE THE WINDOW IS OPEN. Every in-play writer of bit 0 is reached only
   through input the wiki holds: the Pause key (0x4960AA), the in-game menu (0x460E00, 0x460A33),
   key 0xE3 (0x49990E), the options opener's callers, and the replay HUD's pause button
   (tagpu_replay_pause). Impure's other writers set the bit and never clear it (the rewind's and
   the launch hold's keeper on 0x496920) or run at a level's first tick (fix_load_unpaused, before
   any window can open). The replay trigger's `resume` clears a pause its own `pause at` took: a
   test's command, which no player reaches.
   ON CLOSE the bit is cleared only if it was clear at the open, so a game paused before the wiki
   stays paused after it. AT LEVEL END the pause is forgotten and nothing is written. */
static void wiki_open(int gx, int gy)
{
    int was = 0;
    InterlockedExchange(&s_fx, gx);
    InterlockedExchange(&s_fy, gy);
    InterlockedExchange((LONG*)&g_ddraw.cursor.x, gx);
    InterlockedExchange((LONG*)&g_ddraw.cursor.y, gy);
    InterlockedExchange(&s_hold, 1);
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
    InterlockedExchange(&s_hold, 0);
    page_close();
    tagpu_logf("wiki: closed (%s)", how);
}

/* the topmost control of the last frame drawn under the client point: its key into `key` */
static int hit(int cx, int cy, char* key)
{
    unsigned i;
    if (cx < 0) return 0;
    for (i = 0; i < s_hit.n; i++) {
        const TAGPU_CMD_WIKIHIT* h = &s_hit.hit[i];
        if (cx >= h->x0 && cx < h->x1 && cy >= h->y0 && cy < h->y1) {
            memcpy(key, h->key, TAGPU_WIKI_KEY);
            key[TAGPU_WIKI_KEY - 1] = 0;
            return 1;
        }
    }
    return 0;
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
static char     s_msg[32];                      /* what the stage shows with no demo             */
static LARGE_INTEGER s_qpf, s_last;
static double   s_acc;                          /* ticks owed, wiki.js `acc`                     */
static int      s_drag = -1;                    /* the panel whose thumb a left press holds      */
static int      s_dragY;
static float    s_dragFrom;
static double   s_costMax;                      /* the most one fill cost this thread, ms, since
                                                   the open                                     */
static unsigned s_costFrames;
static uint32_t s_trunc;                        /* the copy's WIKISTAGE_TRUNC_* already said     */

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

static void sim_free(void)
{
    if (s_simOn) tagpu_wikisim_free(&s_sim);
    s_simOn = 0;
}

/* wiki.js `start`: the selected demo afresh, scripted, its clock from now */
static void start(void)
{
    const WIKIDATA* d = data();
    const WIKIENTRY* e = entry_of(d);
    char err[160];
    sim_free();
    s_acc = 0;
    s_want = 0;
    s_msg[0] = 0;
    s_trunc = 0;
    QueryPerformanceCounter(&s_last);
    if (!d) { s_want = 1; return; }                  /* the data loads on its own thread       */
    if (!e) return;
    if (s_demo < 0 || s_demo >= e->ndemo) { lstrcpynA(s_msg, "no demo", sizeof s_msg); return; }
    if (!tagpu_wikisim_init(&s_sim, e, &e->demo[s_demo], 1, err, sizeof err)) {
        tagpu_logf("wiki: the demo %s/%s did not start - %s", e->id, e->demo[s_demo].id, err);
        lstrcpynA(s_msg, "the demo did not start", sizeof s_msg);
        return;
    }
    s_simOn = 1;
}

/* wiki.js `pick`: entry `e`, its demo `k`, playing from the start */
static void pick(int e, int k)
{
    s_entry = e;
    s_demo = k;
    s_playing = 1;
    start();
}

/* the demo has ended and holds its last frame (wiki.js `held`) */
static int done(void)
{
    return s_simOn && s_sim.done;
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
    if (d && s_entry < 0)
        for (i = 0; i < d->nnode; i++)
            if (d->node[i].entry >= 0) { pick(d->node[i].entry, 0); return; }
    start();
}

static void page_close(void)
{
    sim_free();
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

/* A left press on control `key` of the last frame drawn (wiki.js's handlers). Try it yourself is
   the next landing's: its button shows pressed and does nothing. */
static void page_press(const char* key)
{
    const WIKIDATA* d = data();
    const int folds = (int)(TAGPU_WIKI_FOLDS * 32);
    int i;
    if (!strcmp(key, "play") || !strcmp(key, "replay") || !strcmp(key, "x1") || !strcmp(key, "x2") ||
        !strcmp(key, "try"))
        held(key);
    if (!d) return;
    if ((i = key_index(key, "fold:", d->nnode < folds ? d->nnode : folds)) >= 0)
        s_fold[i >> 5] ^= 1u << (i & 31);
    else if ((i = key_index(key, "entry:", d->nnode)) >= 0) {
        if (d->node[i].entry >= 0 && d->node[i].entry != s_entry) pick(d->node[i].entry, 0);
    } else if (entry_of(d) && (i = key_index(key, "demo:", entry_of(d)->ndemo)) >= 0)
        pick(s_entry, i);
    else if (!strcmp(key, "play")) {
        if (done()) start();
        else s_playing = !s_playing;
    } else if (!strcmp(key, "replay")) {
        s_playing = 1;
        start();
    } else if (!strcmp(key, "x1"))
        s_speed = 1;
    else if (!strcmp(key, "x2"))
        s_speed = 2;
}

/* EVERY SCROLL IS CLAMPED TO THE LIMITS OF THE FRAME THE PLAYER SAW: the hit map's, taken from
   the last frame drawn. The layout holds it to its own limit again when it draws. */
static void scroll_to(int i, float v)
{
    float mx = s_hit.drawn ? s_hit.scroll_max[i] : 0;
    s_scroll[i] = v < 0 ? 0 : v > mx ? mx : v;
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
                scroll_to(k, s_hit.scroll[k] + (float)notches * WHEEL_UNITS);
                return;
            }
    }
}

/* A left press on a scrollbar of the last frame drawn: 1 = taken. On the thumb it holds it for a
   drag; elsewhere on the track it pages toward the press. */
static int bar_press(int cx, int cy)
{
    int i;
    if (cx < 0 || !s_hit.drawn) return 0;
    for (i = 0; i < (int)TAGPU_WIKI_NSCROLL; i++) {
        const TAGPU_CMD_WIKIBAR* b = &s_hit.bar[i];
        if (b->y1 <= b->y0 || cx < b->x0 || cx >= b->x1 || cy < b->y0 || cy >= b->y1) continue;
        if (cy >= b->thumb_y && cy < b->thumb_y + b->thumb_h) {
            s_drag = i;
            s_dragY = cy;
            s_dragFrom = s_hit.scroll[i];
        } else {
            const float page = TRACK_PAGE * (float)(b->y1 - b->y0) / (s_hit.u > 0 ? s_hit.u : 1);
            scroll_to(i, s_hit.scroll[i] + (cy < b->thumb_y ? -page : page));
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

/* THE FILL STEPS THE DEMO ON THIS THREAD and copies its view by value into the record, so the
   render thread gets values and never a WIKISIM. The copy keeps every bound of
   tagpu_wikistage_copy (the map at 128 cells a side, 256 vents, 128 units, the gesture's sites,
   256 queued, 512 lines, the caption and the labels) and says once what it had to cut. */
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
    if (!s_simOn || !s_playing || done()) s_acc = 0;
    else {
        s_acc += dt * WIKI_TPS * s_speed;
        if (s_acc > STEPS_MAX) s_acc = STEPS_MAX;
        while (s_acc >= 1 && !done()) {
            tagpu_wikisim_step(&s_sim);
            s_acc -= 1;
        }
    }
    w->entry = (int16_t)(e ? s_entry : -1);
    w->demo = (int16_t)(e && s_demo >= 0 && s_demo < e->ndemo ? s_demo : -1);
    w->playing = (uint8_t)s_playing;
    w->done = (uint8_t)done();
    w->speed = (uint8_t)s_speed;
    w->try_it = 0;
    w->mod = (int16_t)tagpu_profiles_mod();
    lstrcpynA(w->held, s_held, sizeof w->held);
    memcpy(w->fold, s_fold, sizeof w->fold);
    memcpy(w->scroll, s_scroll, sizeof w->scroll);
    lstrcpynA(w->msg, s_msg, sizeof w->msg);
    if (s_simOn && e) {
        t = tagpu_wikistage_copy(&w->stage, tagpu_wikisim_view(&s_sim), e);
        w->has_stage = 1;
        if (t & ~s_trunc) {
            s_trunc |= t;
            tagpu_logf("wiki: the demo %s/%s's view did not fit the record (cut 0x%03x)", e->id,
                       e->demo[s_demo].id, (unsigned)t);
        }
    }
}

/* THE PARITY CHECK OF THE CODE THE GAME RUNS (tools/wiki_regression.py demos): every demo of every
   entry played to its end at once, here on the game thread in the DLL's i686 SSE2 build, and its
   tagpu_wikisim_hash logged in the form `node tools/wiki/simparity.mjs --hash` prints. */
static void check(void)
{
    const WIKIDATA* d = data();
    LARGE_INTEGER t0, t1;
    char err[160];
    int i, j, n = 0, bad = 0;
    if (!d) { tagpu_logf("wiki check: the wiki's data is not loaded"); return; }
    QueryPerformanceCounter(&t0);
    for (i = 0; i < d->nentry; i++)
        for (j = 0; j < d->entry[i].ndemo; j++) {
            const WIKIENTRY* e = &d->entry[i];
            WIKISIM* s = (WIKISIM*)malloc(sizeof *s);
            long k = 0;
            if (!s || !tagpu_wikisim_init(s, e, &e->demo[j], 1, err, sizeof err)) {
                tagpu_logf("wiki check: %s/%s did not start - %s", e->id, e->demo[j].id, s ? err : "no memory");
                free(s);
                bad++;
                continue;
            }
            /* a scripted demo is done at its end tick; the bound holds a script that never is */
            while (!s->done && k++ < 1000000L) tagpu_wikisim_step(s);
            tagpu_logf("wiki check: %s/%s %016llx", e->id, e->demo[j].id,
                       (unsigned long long)tagpu_wikisim_hash(s));
            tagpu_wikisim_free(s);
            free(s);
            n++;
        }
    QueryPerformanceCounter(&t1);
    tagpu_logf("wiki check: %d demos played, %d did not start, in %.0f ms", n, bad,
               s_qpf.QuadPart > 0 ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_qpf.QuadPart : 0.0);
}

/* A press of button `bit` at client point (cx, cy), engine point (gx, gy), from either path:
   1 = taken. */
static int press(unsigned bit, int cx, int cy, int gx, int gy)
{
    char key[TAGPU_WIKI_KEY];
    if (s_open) {
        s_owedBtn |= bit;
        if (bit == B_L && !bar_press(cx, cy) && hit(cx, cy, key)) {
            if (!strcmp(key, "close")) { held(key); wiki_close("the close button"); }
            else if (!strcmp(key, "wiki")) { held(key); wiki_close("the wiki button"); }
            else page_press(key);
        }
        return 1;
    }
    if (bit == B_L && hit(cx, cy, key) && !strcmp(key, "wiki")) {
        s_owedBtn |= bit;
        held(key);
        wiki_open(gx, gy);
        return 1;
    }
    s_owedBtn &= ~bit;
    return 0;
}

/* A release of button `bit`, from either path: 1 = taken. */
static int release(unsigned bit)
{
    if (bit == B_L) { s_held[0] = 0; s_drag = -1; }
    if (s_owedBtn & bit) {
        s_owedBtn &= ~bit;
        return 1;
    }
    return s_open;
}

/* A key-down of `vk`; `repeat` when the key was already down (lParam bit 30): 1 = taken. */
static int key_down(unsigned vk, int repeat)
{
    vk &= 0xFF;
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
    vk &= 0xFF;
    if (s_owedKey[vk]) {
        s_owedKey[vk] = 0;
        if (vk == VK_ESCAPE && s_escClose) {
            s_escClose = 0;
            if (s_open) wiki_close("Esc");
        }
        return 1;
    }
    return s_open;
}

static int character(void)
{
    int owed = s_charOwed;
    s_charOwed = 0;
    return s_open || owed;
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
        if (s_open) { note_client(cx, cy); bar_drag(cy); }
        return s_open;
    case WM_MOUSEWHEEL:
        /* a notch is WHEEL_DELTA; up scrolls toward the top */
        if (s_open) wheel(-(double)(short)HIWORD(wp) / WHEEL_DELTA);
        return s_open;
    case WM_INPUT:
    case WM_MOUSEHOVER:
        return s_open;
    /* THE ENGINE'S POINT OF A REAL PRESS is g_ddraw.cursor: this runs at the top of the window
       procedure, before cnc-ddraw has unscaled the message, and the preceding WM_MOUSEMOVE has put
       the game point there -- the point tagpu_menu_click is handed on this path too */
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
        int gx = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
        int gy = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);
        if (s_open) note_client(cx, cy);
        return press(bit, cx, cy, gx, gy);
    }
    case WM_LBUTTONUP:
        return release(B_L);
    case WM_RBUTTONUP:
        return release(B_R);
    case WM_MBUTTONUP:
        return release(B_M);
    case WM_XBUTTONUP:
        return release(B_X);
    }
    return 0;
}

int tagpu_wiki_mouse(int code, int cx, int cy, int gx, int gy)
{
    switch (code) {
    case TAGPU_M_MOVE:
        if (s_open) { note_client(cx, cy); if (cx >= 0) bar_drag(cy); }
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
}

/* a number the render thread posted, as data: finite and in [0, hi], else 0 */
static float bounded(float v, float hi)
{
    return v >= 0 && v <= hi ? v : 0;
}

void tagpu_wiki_take(const TAGPU_CMD_WIKI* c)
{
    int i;
    if (!c) { s_hit.n = 0; s_hit.drawn = 0; return; }
    s_hit = *c;
    if (s_hit.n > TAGPU_CMD_WIKIHITS) s_hit.n = TAGPU_CMD_WIKIHITS;
    s_hit.u = bounded(s_hit.u, 1000);
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

/* THE STATE, FOR THE TESTS (tools/wiki_regression.py): written when tagpu_wiki.dump appears, from
   the fill, on this thread, as the replay HUD's tagpu_replayhud.dump is. The engine screen's size
   and the viewport let a test turn an engine point into the client point a device click needs. */
static void dump(const TAGPU_PK_WIKI* w)
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
    fprintf(f, ",\"frozen\":[%ld,%ld],\"ptr\":[%d,%d],\"owed_buttons\":%u,",
            (long)fx, (long)fy, w->ptr_x, w->ptr_y, s_owedBtn);
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
    /* the demo: its clock and its state hash (tagpu_wikisim_hash), and what the page cost */
    fprintf(f, "\"playing\":%d,\"speed\":%d,\"sim\":%d,\"tick\":%d,\"done\":%d,\"hash\":\"%016llx\","
               "\"stage\":%d,\"cost_max_ms\":%.4f,\"cost_frames\":%u,\"msg\":",
            s_playing, s_speed, s_simOn, s_simOn ? s_sim.tick : -1, done(),
            s_simOn ? (unsigned long long)tagpu_wikisim_hash(&s_sim) : 0ull, w->has_stage, s_costMax, s_costFrames);
    jstr(f, s_msg, sizeof s_msg);
    fprintf(f, ",\"bars\":[");
    for (i = 0; i < TAGPU_WIKI_NSCROLL; i++) {
        const TAGPU_CMD_WIKIBAR* b = &s_hit.bar[i];
        fprintf(f, "%s{\"track\":[%d,%d,%d,%d],\"thumb\":[%d,%d],\"per_px\":%.5f}", i ? "," : "",
                b->x0, b->y0, b->x1, b->y1, b->thumb_y, b->thumb_h, b->per_px);
    }
    fprintf(f, "],\"drawn\":%u,\"drawn_open\":%u,\"hits\":[", (unsigned)s_hit.drawn, (unsigned)s_hit.open);
    for (i = 0; i < s_hit.n; i++) {
        const TAGPU_CMD_WIKIHIT* h = &s_hit.hit[i];
        fprintf(f, "%s{\"key\":", i ? "," : "");
        jstr(f, h->key, sizeof h->key);
        fprintf(f, ",\"rect\":[%d,%d,%d,%d]}", h->x0, h->y0, h->x1, h->y1);
    }
    fprintf(f, "]}\n");
    if (fclose(f) != 0 || !MoveFileExA(DUMP_OUT ".tmp", DUMP_OUT, MOVEFILE_REPLACE_EXISTING)) {
        tagpu_logf("wiki: the state dump could not be written (%lu)", (unsigned long)GetLastError());
        return;
    }
    tagpu_logf("wiki: state dumped to %s - open=%d paused=%d hits=%u", DUMP_OUT, s_open, s_paused, s_hit.n);
}

int tagpu_wiki_fill(TAGPU_PK_WIKI* w, uint32_t* len)
{
    static unsigned calls;
    LARGE_INTEGER t0, t1;
    int cx, cy, ask = (calls++ % 30u) == 0u;
    if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
    if (ask && GetFileAttributesA(CHECK_ASK) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(CHECK_ASK);
        check();
    }
    if (!tagpu_hudstate_on()) {
        /* THE WIKI IS OPEN ONLY WHERE IT CAN BE SEEN AND CLOSED: the lane fell back to GDI or the
           HUD's pass refused, so no frame will draw the window again */
        if (s_open) wiki_close("the wiki can no longer be drawn");
        if (!s_saidOff) { s_saidOff = 1; tagpu_logf("wiki: not drawn - the replay HUD's pass is off"); }
        return 0;
    }
    QueryPerformanceCounter(&t0);
    memset(w, 0, TAGPU_PK_WIKI_HEAD);
    w->open = (uint8_t)s_open;
    w->paused = (uint8_t)s_paused;
    w->ptr_x = w->ptr_y = -1;
    w->entry = w->demo = -1;
    if (mouse_last_client(&cx, &cy)) {
        w->ptr_x = (int16_t)(cx < -32768 ? -32768 : cx > 32767 ? 32767 : cx);
        w->ptr_y = (int16_t)(cy < -32768 ? -32768 : cy > 32767 ? 32767 : cy);
    }
    lstrcpynA(w->held, s_held, sizeof w->held);
    if (s_open) {
        page_fill(w);
        QueryPerformanceCounter(&t1);
        {
            const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_qpf.QuadPart;
            if (ms > s_costMax) s_costMax = ms;
            s_costFrames++;
        }
    }
    *len = w->has_stage ? (uint32_t)sizeof *w : TAGPU_PK_WIKI_HEAD;
    if (ask && GetFileAttributesA(DUMP_ASK) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(DUMP_ASK);
        dump(w);
    }
    return 1;
}

void tagpu_wiki_level_end(void)
{
    if (s_open) {
        s_paused = 0;           /* the level it was taken in is ending: nothing is written */
        wiki_close("the level ended");
    }
    sim_free();
    s_held[0] = 0;
    s_drag = -1;
    s_escClose = 0;
    memset(&s_hit, 0, sizeof s_hit);
}
