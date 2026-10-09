#ifndef TAGPU_SHIELD_H
#define TAGPU_SHIELD_H
#include <windows.h>

/* tagpu input firewall ("the shield").

   Armed by the trigger file `tagpu_shield.on` next to the exe (tacli arms it by
   default), same idiom as tagpu_nowarp.on. While armed:

     - the wndproc drops every hardware keyboard/mouse message, so the human
       moving the pointer across a running instance, or typing with it focused,
       cannot perturb an agent's test;
     - injected input arrives as tagged WM_TAGPU_* messages, which survive that
       filter and are translated back into WM_KEYDOWN/WM_CHAR/WM_MOUSEMOVE/... in
       the wndproc, then handed straight to the game's own window proc;
     - GetCursorPos/GetKeyState/GetAsyncKeyState report the injected state
       instead of the real devices. That last part is what makes ctrl/shift
       combos land: TA *polls* modifier state rather than reading it off the
       message, which is why posted messages alone cannot deliver a combo.

   The tagged messages are honoured whether or not the shield is armed — arming
   only decides whether hardware input is let through alongside them. */

#define WM_TAGPU_KEY    (WM_APP + 140)   /* wParam = VK | seq<<16, lParam = TAGPU_KEY_UP?   */
#define WM_TAGPU_MOUSE  (WM_APP + 141)   /* wParam = TAGPU_M_* (| TAGPU_M_DEV) | seq<<16,
                                            lParam = x,y                                   */
#define WM_TAGPU_CHAR   (WM_APP + 142)   /* wParam = character, lParam = seq                */

#define TAGPU_KEY_UP    1
#define TAGPU_KEY_VK    0xFF             /* wParam's key bits                               */

/* THE SEQUENCE NUMBER: the injector's 16 bits naming the one event it has in flight
   (tagpu_input.c), handed back through tagpu_input_delivered() once the wndproc has delivered
   the message. Never 0. */
#define TAGPU_SEQ(wparam)        ((unsigned)((wparam) >> 16) & 0xFFFFu)
#define TAGPU_SEQ_WPARAM(seq)    ((WPARAM)((seq) & 0xFFFFu) << 16)

/* What became of a tagged message: it reached the game's window procedure, the
   render-options sprocket took it, or it was dropped (a press in the display-only ring,
   tagpu_zoom.h, or a device-space relative move). */
enum { TAGPU_DELIVER_ENGINE, TAGPU_DELIVER_MENU, TAGPU_DELIVER_DROPPED };

enum {
    TAGPU_M_MOVE = 0,
    TAGPU_M_LDOWN, TAGPU_M_LUP,
    TAGPU_M_RDOWN, TAGPU_M_RUP,
    TAGPU_M_MDOWN, TAGPU_M_MUP,
    TAGPU_M_MOVEREL,                 /* x,y are a delta on the injected cursor */
    TAGPU_M_WHEELUP, TAGPU_M_WHEELDN,/* one wheel notch — the zoom control     */
};

/* pass as the position to leave the injected cursor where it is */
#define TAGPU_M_HERE    (-1)

/* OR into the code: x,y are CLIENT-AREA (device) pixels, not the engine's
   logical screen, and are converted by `mouse_client_to_game` -- the same
   function a hardware click goes through.

   Why it exists: every other injected event is delivered in the engine's own
   coordinates, so none of them ever traverses the pointer unscale. That is
   fine for driving the game and useless for TESTING the unscale, which is
   exactly what phase 2's kill rule turns on ("clicks land on the right
   gadget at every k"). With this, the harness clicks where a player's mouse
   would be and the engine has to arrive at the right gadget by itself.
   Outside the letterboxed viewport the conversion yields the centre of the
   engine's screen, which is what a hardware click there does. */
#define TAGPU_M_DEV     0x100
#define TAGPU_M_CODE    0x1FF            /* wParam's code bits, TAGPU_M_DEV included        */

BOOL tagpu_shield_on(void);

/* Re-read the trigger file (every 15th call), clearing the virtual key state on the
   disarm edge.

   CALL THIS ON THE GAME THREAD -- the wndproc's thread. It is called from the
   engine's flip 0x4C63A0 through tagpu_triggers_frame.

   IT IS NOT THREAD-FREE: on the disarm edge it calls clear_state(), which memsets
   s_down/s_async -- and those are written by the wndproc, per key, through
   set_one(). Off the wndproc's thread that memset races those writes, and what
   it produces is a key or a mouse button left stuck down with nothing in the
   log. */
void tagpu_shield_frame(void);

/* TRUE when the message was consumed: either a tagged injection (delivered to
   the game) or hardware input swallowed by the armed shield. */
BOOL tagpu_shield_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result);

/* TRUE when armed, and *state gets the virtual key state (GetKeyState layout,
   or GetAsyncKeyState layout when async). */
BOOL tagpu_shield_key_state(int vk, BOOL async, SHORT* state);

/* Injection: post a tagged message carrying `seq` to the game window; PostMessageA's
   answer. A key stays virtually down from its down to its up, so a modifier is held
   for exactly the events posted between them -- TA polls modifier state
   (GetAsyncKeyState, IAT 0x4FC350) while it handles a message, so it sees what a
   player's held key shows. */
BOOL tagpu_shield_key(HWND hwnd, int vk, BOOL up, unsigned seq);
BOOL tagpu_shield_char(HWND hwnd, char ch, unsigned seq);
BOOL tagpu_shield_mouse(HWND hwnd, int code, int gx, int gy, unsigned seq);

#endif
