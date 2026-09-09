#ifndef TAGPU_SHIELD_H
#define TAGPU_SHIELD_H
#include <windows.h>

/* tagpu input firewall ("the shield") — phase 1.1.

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
       message, which is why posted combos never worked before.

   The tagged messages are honoured whether or not the shield is armed — arming
   only decides whether hardware input is let through alongside them. */

#define WM_TAGPU_KEY    (WM_APP + 140)   /* wParam = VK, lParam = TAGPU_KEY_UP?      */
#define WM_TAGPU_MOUSE  (WM_APP + 141)   /* wParam = TAGPU_M_* (| TAGPU_M_DEV), lParam = x,y */
#define WM_TAGPU_CHAR   (WM_APP + 142)   /* wParam = character                       */

#define TAGPU_KEY_UP    1

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
   function a hardware click goes through (G17b).

   Why it exists: every other injected event is delivered in the engine's own
   coordinates, so none of them ever traverses the pointer unscale. That is
   fine for driving the game and useless for TESTING the unscale, which is
   exactly what phase 2's kill rule turns on ("clicks land on the right
   gadget at every k"). With this, the harness clicks where a player's mouse
   would be and the engine has to arrive at the right gadget by itself.
   Outside the letterboxed viewport the conversion yields the centre of the
   engine's screen, which is what a hardware click there has always done. */
#define TAGPU_M_DEV     0x100

BOOL tagpu_shield_on(void);

/* Re-read the trigger file and release keys whose hold has expired. Called from
   the present hook (game thread not required — releases are posted). */
void tagpu_shield_frame(HWND hwnd);

/* TRUE when the message was consumed: either a tagged injection (delivered to
   the game) or hardware input swallowed by the armed shield. */
BOOL tagpu_shield_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result);

/* TRUE when armed, and *state gets the virtual key state (GetKeyState layout,
   or GetAsyncKeyState layout when async). */
BOOL tagpu_shield_key_state(int vk, BOOL async, SHORT* state);

/* Injection: post a tagged message to the game window. hold_ms > 0 keeps a key
   virtually down for that long (modifiers must outlive the keystroke they
   qualify — TA polls them a frame or more after the message). */
void tagpu_shield_key(HWND hwnd, int vk, BOOL up);
void tagpu_shield_key_hold(HWND hwnd, int vk, DWORD hold_ms);
void tagpu_shield_char(HWND hwnd, char ch);
void tagpu_shield_mouse(HWND hwnd, int code, int gx, int gy);

#endif
