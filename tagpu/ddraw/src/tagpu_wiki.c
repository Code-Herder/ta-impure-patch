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

   Nothing here names engine memory: the two engine facts the pause needs are tagpu_menu.c's. */
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include "dd.h"
#include "mouse.h"
#include "tagpu_shield.h"
#include "tagpu_menu.h"
#include "tagpu_hudstate.h"
#include "tagpu_log.h"
#include "tagpu_wiki.h"

#define DUMP_ASK "tagpu_wiki.dump"
#define DUMP_OUT "tagpu_wiki.json"

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
static int  s_btnDown, s_xDown;         /* the wiki button / the close button shows pressed      */
static int  s_paused, s_pauseWas;       /* the pause is the wiki's, and bit 0 before it took it  */
static int  s_saidOff;
static TAGPU_CMD_WIKI s_hit;            /* the last frame drawn's controls                       */

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
}

static void wiki_close(const char* how)
{
    if (s_paused && !s_pauseWas) tagpu_menu_pause_hold(0, NULL);
    s_paused = 0;
    s_open = 0;
    s_escClose = 0;
    InterlockedExchange(&s_hold, 0);
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

/* A press of button `bit` at client point (cx, cy), engine point (gx, gy), from either path:
   1 = taken. */
static int press(unsigned bit, int cx, int cy, int gx, int gy)
{
    char key[TAGPU_WIKI_KEY];
    if (s_open) {
        s_owedBtn |= bit;
        if (bit == B_L && hit(cx, cy, key)) {
            if (!strcmp(key, "close")) { s_xDown = 1; wiki_close("the close button"); }
            else if (!strcmp(key, "wiki")) { s_btnDown = 1; wiki_close("the wiki button"); }
        }
        return 1;
    }
    if (bit == B_L && hit(cx, cy, key) && !strcmp(key, "wiki")) {
        s_owedBtn |= bit;
        s_btnDown = 1;
        wiki_open(gx, gy);
        return 1;
    }
    s_owedBtn &= ~bit;
    return 0;
}

/* A release of button `bit`, from either path: 1 = taken. */
static int release(unsigned bit)
{
    if (s_owedBtn & bit) {
        s_owedBtn &= ~bit;
        if (bit == B_L) s_btnDown = s_xDown = 0;
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
        if (s_open) note_client(cx, cy);
        return s_open;
    case WM_INPUT:
    case WM_MOUSEWHEEL:
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
        if (s_open) note_client(cx, cy);
        return s_open;
    case TAGPU_M_WHEELUP:
    case TAGPU_M_WHEELDN:
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
    s_btnDown = s_xDown = 0;
}

void tagpu_wiki_take(const TAGPU_CMD_WIKI* c)
{
    if (!c) { s_hit.n = 0; s_hit.drawn = 0; return; }
    s_hit = *c;
    if (s_hit.n > TAGPU_CMD_WIKIHITS) s_hit.n = TAGPU_CMD_WIKIHITS;
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
    LONG fx, fy;
    unsigned i, k;
    if (!f) { tagpu_logf("wiki: the state dump could not open %s", DUMP_OUT ".tmp"); return; }
    tagpu_wiki_frozen(&fx, &fy);
    fprintf(f, "{\"open\":%d,\"holds\":%ld,\"paused\":%d,\"pause_was\":%d,\"single_player\":%d,"
               "\"btn_down\":%d,\"x_down\":%d,\"frozen\":[%ld,%ld],\"ptr\":[%d,%d],\"owed_buttons\":%u,",
            s_open, (long)tagpu_wiki_holds_input(), s_paused, s_pauseWas, tagpu_menu_single_player(),
            s_btnDown, s_xDown, (long)fx, (long)fy, w->ptr_x, w->ptr_y, s_owedBtn);
    fprintf(f, "\"owed_keys\":[");
    for (i = k = 0; i < 256; i++)
        if (s_owedKey[i]) fprintf(f, "%s%u", k++ ? "," : "", i);
    fprintf(f, "],\"game\":[%u,%u],\"viewport\":[%d,%d,%d,%d],",
            (unsigned)g_ddraw.width, (unsigned)g_ddraw.height,
            (int)g_ddraw.render.viewport.x, (int)g_ddraw.render.viewport.y,
            (int)g_ddraw.render.viewport.width, (int)g_ddraw.render.viewport.height);
    fprintf(f, "\"drawn\":%u,\"drawn_open\":%u,\"hits\":[", (unsigned)s_hit.drawn, (unsigned)s_hit.open);
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

int tagpu_wiki_fill(TAGPU_PK_WIKI* w)
{
    static unsigned calls;
    int cx, cy;
    if (!tagpu_hudstate_on()) {
        /* THE WIKI IS OPEN ONLY WHERE IT CAN BE SEEN AND CLOSED: the lane fell back to GDI or the
           HUD's pass refused, so no frame will draw the window again */
        if (s_open) wiki_close("the wiki can no longer be drawn");
        if (!s_saidOff) { s_saidOff = 1; tagpu_logf("wiki: not drawn - the replay HUD's pass is off"); }
        return 0;
    }
    memset(w, 0, sizeof *w);
    w->open = (uint8_t)s_open;
    w->btn_down = (uint8_t)s_btnDown;
    w->x_down = (uint8_t)s_xDown;
    w->paused = (uint8_t)s_paused;
    w->ptr_x = w->ptr_y = -1;
    if (mouse_last_client(&cx, &cy)) {
        w->ptr_x = (int16_t)(cx < -32768 ? -32768 : cx > 32767 ? 32767 : cx);
        w->ptr_y = (int16_t)(cy < -32768 ? -32768 : cy > 32767 ? 32767 : cy);
    }
    if ((calls++ % 30u) == 0u && GetFileAttributesA(DUMP_ASK) != INVALID_FILE_ATTRIBUTES) {
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
    s_btnDown = s_xDown = 0;
    s_escClose = 0;
    memset(&s_hit, 0, sizeof s_hit);
}
