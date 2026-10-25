/* tagpu_shield.c — the input firewall.

   Rationale and message contract: inc/tagpu_shield.h.

   Two states are kept here: which keys/buttons the injector says are down, and
   where it says the pointer is. Everything that reads input — the wndproc, and
   the polled GetCursorPos/GetKeyState/GetAsyncKeyState hooks — reads those
   instead of the real devices while the shield is armed, so the game's idea of
   the input devices is entirely ours. The human's keyboard and mouse become
   invisible to the game; the desktop stays theirs. */

#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include "dd.h"
#include "hook.h"
#include "tagpu_shield.h"
#include "tagpu_menu.h"
#include "tagpu_hudinput.h"
#include "mouse.h"
#include "tagpu_zoom.h"
#include "tagpu_place.h"
#include "tagpu_gui.h"
#include "tagpu_input.h"
#include "tagpu_log.h"

#define SHIELD_TRIGGER  "tagpu_shield.on"

static volatile LONG s_armed = -1;      /* -1 until the trigger is first read */
static BYTE  s_down[256];               /* injected key/button down state */
static BYTE  s_toggle[256];             /* caps/num-style toggle bit */
static BYTE  s_async[256];              /* pressed since the last async read */

static void slog(const char* s)
{
    tagpu_log(s);
}

static BOOL trigger_present(void)
{
    return GetFileAttributesA(SHIELD_TRIGGER) != INVALID_FILE_ATTRIBUTES;
}

BOOL tagpu_shield_on(void)
{
    LONG armed = InterlockedExchangeAdd(&s_armed, 0);

    if (armed < 0)
    {
        armed = trigger_present();
        InterlockedExchange(&s_armed, armed);
    }

    return armed != 0;
}

/* generic VK <-> left-side VK: TA polls VK_CONTROL/VK_SHIFT, wine's own state
   would carry both, so keep the pair consistent whichever one is injected */
static int vk_companion(int vk)
{
    switch (vk)
    {
    case VK_CONTROL: return VK_LCONTROL;
    case VK_SHIFT:   return VK_LSHIFT;
    case VK_MENU:    return VK_LMENU;
    case VK_LCONTROL: case VK_RCONTROL: return VK_CONTROL;
    case VK_LSHIFT:   case VK_RSHIFT:   return VK_SHIFT;
    case VK_LMENU:    case VK_RMENU:    return VK_MENU;
    }
    return 0;
}

static void set_one(int vk, BOOL down)
{
    if (vk <= 0 || vk > 255)
        return;

    if (down)
    {
        if (!s_down[vk])
            s_toggle[vk] ^= 1;

        s_async[vk] = 1;
    }

    s_down[vk] = down ? 1 : 0;
}

static void set_key(int vk, BOOL down)
{
    set_one(vk, down);
    set_one(vk_companion(vk), down);
}

static void clear_state(void)
{
    ZeroMemory((void*)s_down, sizeof(s_down));
    ZeroMemory((void*)s_async, sizeof(s_async));
}

void tagpu_shield_frame(void)
{
    /* the trigger is a file: probe it every 15th trigger tick like the other triggers */
    static unsigned calls;

    if (calls++ % 15 == 0)
    {
        /* logged against its own memory, not against s_armed: the polled hooks
           run long before the first present and latch the state lazily, so
           s_armed is usually already right by the time we get here */
        static LONG logged = -1;
        LONG now = trigger_present();

        InterlockedExchange(&s_armed, now);

        if (logged != now)
        {
            logged = now;

            slog(now ? "shield: ARMED (hardware input blocked)"
                     : "shield: disarmed (hardware input restored)");

            if (!now)
                clear_state();
        }
    }
}

BOOL tagpu_shield_key_state(int vk, BOOL async, SHORT* state)
{
    if (!tagpu_shield_on())
        return FALSE;

    SHORT s = 0;

    if (vk > 0 && vk < 256)
    {
        if (s_down[vk])
            s |= (SHORT)0x8000;

        if (async)
        {
            if (s_async[vk])
            {
                s |= 1;
                s_async[vk] = 0;
            }
        }
        else if (s_toggle[vk])
        {
            s |= 1;
        }
    }

    if (state)
        *state = s;

    return TRUE;
}

/* --------------------------------------------------------------- delivery */

static LRESULT to_game(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (g_ddraw.wndproc)
    {
        /* injected input goes through exactly the same transform, or `tacli
           click` and the human's mouse would disagree about the world — and
           the same is true of the wheel, which is why an injected notch is
           offered to the zoom here rather than short-circuited at the token */
        if (tagpu_hudinput_wheel(msg, wparam, lparam)) return 0;
        if (tagpu_zoom_wheel(msg, wparam, lparam))
            return 0;
        tagpu_gui_wheel(msg, wparam, lparam);
        if (tagpu_zoom_drop_mouse(msg, lparam))
            return 0;
        return CallWindowProcA(g_ddraw.wndproc, hwnd, msg, wparam,
                               tagpu_zoom_mouse_lparam(msg, lparam));
    }

    return real_DefWindowProcA(hwnd, msg, wparam, lparam);
}

static void deliver_key(HWND hwnd, int vk, BOOL up)
{
    UINT scan = MapVirtualKeyA((UINT)vk, 0 /* MAPVK_VK_TO_VSC */);
    LPARAM lparam = 1 | ((LPARAM)scan << 16) | (up ? ((LPARAM)3 << 30) : 0);

    set_key(vk, !up);

    /* Alt+Z / Alt+X with a building armed step the placement's spacing and reach the game no
       further, as on the real path below; Alt is virtual here, so the module asks for it */
    if (!up && tagpu_place_key((unsigned)vk, 0))
        return;

    to_game(hwnd, up ? WM_KEYUP : WM_KEYDOWN, (WPARAM)vk, lparam);

    if (!up)
    {
        /* Tagged messages never pass through the game's TranslateMessage, so
           produce the WM_CHAR ourselves — off the virtual modifier state, which
           is what makes shift+key type an upper-case character. */
        BYTE keys[256];
        WORD ch = 0;

        for (int i = 0; i < 256; i++)
            keys[i] = (BYTE)((s_down[i] ? 0x80 : 0) | (s_toggle[i] ? 1 : 0));

        if (ToAscii((UINT)vk, scan, keys, &ch, 0) == 1)
            to_game(hwnd, WM_CHAR, (WPARAM)(ch & 0xFF), lparam);
    }
}

/* What became of an injected mouse event: TAGPU_DELIVER_*, for its acknowledgement. */
static int deliver_mouse(HWND hwnd, int code, int gx, int gy)
{
    int cur_x = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
    int cur_y = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);
    int dev_x = -1, dev_y = -1;         /* the client point a device injection names */

    /* device-space injection: x,y are client-area pixels and go through
       the SAME transform a hardware click takes, so this is a test of the
       pointer path rather than a way around it. TAGPU_M_HERE is left alone --
       "where it is" is already a logical position. */
    if (code & TAGPU_M_DEV)
    {
        code &= ~TAGPU_M_DEV;
        /* DEV is an ABSOLUTE client point by definition, so combining it with
           MOVEREL is meaningless — the conversion would subtract the viewport
           origin from a delta and then add the cursor. Nothing does it today;
           it is refused rather than left latent, because the header invites
           the flag to be OR'd into any code. */
        if (code == TAGPU_M_MOVEREL) return TAGPU_DELIVER_DROPPED;
        if (!(gx < 0 && gy < 0))
        {
            dev_x = gx;
            dev_y = gy;
            mouse_client_to_game(gx, gy, &gx, &gy);
        }
    }
    else
    {
        /* An injected point is the engine's LOGICAL grid with no pointer
           behind it. Dropping the recorded client point is what keeps the UI
           layer's cursor (tagpu_gui_surf.c sharp_cursor) on the injected
           position instead of leaving it at the human's real pointer, where
           it would disagree with every gadget the engine thinks is under the
           mouse. */
        mouse_forget_client();
    }

    if (code == TAGPU_M_MOVEREL)
    {
        gx += cur_x;
        gy += cur_y;
        code = TAGPU_M_MOVE;
    }
    else if (gx < 0 && gy < 0)
    {
        gx = cur_x;         /* "here": button events with no move of their own */
        gy = cur_y;
    }

    if (g_ddraw.width && g_ddraw.height)
    {
        gx = min(max(gx, 0), (int)g_ddraw.width - 1);
        gy = min(max(gy, 0), (int)g_ddraw.height - 1);
    }

    InterlockedExchange((LONG*)&g_ddraw.cursor.x, gx);
    InterlockedExchange((LONG*)&g_ddraw.cursor.y, gy);

    UINT msg = WM_MOUSEMOVE;
    int  vk = 0, down = 0, wheel = 0;

    switch (code)
    {
    case TAGPU_M_LDOWN: msg = WM_LBUTTONDOWN; vk = VK_LBUTTON; down = 1; break;
    case TAGPU_M_LUP:   msg = WM_LBUTTONUP;   vk = VK_LBUTTON;           break;
    case TAGPU_M_RDOWN: msg = WM_RBUTTONDOWN; vk = VK_RBUTTON; down = 1; break;
    case TAGPU_M_RUP:   msg = WM_RBUTTONUP;   vk = VK_RBUTTON;           break;
    case TAGPU_M_MDOWN: msg = WM_MBUTTONDOWN; vk = VK_MBUTTON; down = 1; break;
    case TAGPU_M_MUP:   msg = WM_MBUTTONUP;   vk = VK_MBUTTON;           break;
    /* One notch, carried the way the hardware carries it: the signed delta in
       the high word of wParam and a GAME-space point in lParam — the same space
       cnc-ddraw has unscaled a real wheel into by the time it reaches the other
       door, and the space gx/gy are already in here (see the lParam comment
       below). Going through the same message rather than calling the zoom
       directly is what makes an injected wheel test a player's path. */
    case TAGPU_M_WHEELUP: msg = WM_MOUSEWHEEL; wheel =  WHEEL_DELTA; break;
    case TAGPU_M_WHEELDN: msg = WM_MOUSEWHEEL; wheel = -WHEEL_DELTA; break;
    }

    /* The render-options sprocket owns its own 28x28 of the top bar, and the
       replay HUD its controls; both are hit-tested HERE as well as on the
       real-message path below: with the shield armed this is the ONLY path a
       tacli instance's clicks take, and with it disarmed it is the only path a
       player's do not. A press one consumes is dropped whole -- no message and
       no virtual key state, which the engine polls -- and its release is
       dropped too, but always clears the key so the engine is never left
       holding a button. The HUD is hit in client pixels, so only a device
       injection can reach it; a click in the engine's own space passes under
       it to the engine. */
    if ((vk == VK_LBUTTON && tagpu_menu_click(gx, gy, down)) ||
        ((vk == VK_LBUTTON || vk == VK_RBUTTON) && tagpu_hudinput_button(dev_x, dev_y, gx, gy, down, vk == VK_RBUTTON)))
    {
        if (!down) set_key(vk, FALSE);
        return TAGPU_DELIVER_MENU;
    }

    /* A PRESS in the display-only ring is dropped WHOLE — the virtual key state
       as well as the message, because the engine polls that too and a press it
       can see is a press it can act on at its own idea of where the mouse is
       (tagpu_zoom.h). A RELEASE always clears the key state even when its
       message is dropped: a virtual button left down is a button held for the
       rest of the session. The move that carried the pointer there still goes
       through, so the cursor keeps tracking. */
    {
        int drop = vk ? tagpu_zoom_drop_mouse(msg, MAKELPARAM(gx, gy)) : 0;
        if (vk && !(down && drop)) set_key(vk, down ? TRUE : FALSE);
        if (drop) return TAGPU_DELIVER_DROPPED;
    }

    WPARAM wparam = 0;

    if (s_down[VK_LBUTTON]) wparam |= MK_LBUTTON;
    if (s_down[VK_RBUTTON]) wparam |= MK_RBUTTON;
    if (s_down[VK_MBUTTON]) wparam |= MK_MBUTTON;
    if (s_down[VK_SHIFT])   wparam |= MK_SHIFT;
    if (s_down[VK_CONTROL]) wparam |= MK_CONTROL;

    if (wheel) wparam = MAKEWPARAM((WORD)wparam, (WORD)(short)wheel);

    /* the game expects game-space coordinates in lParam — that is what the
       wndproc hands it after unscaling a hardware message */
    to_game(hwnd, msg, wparam, MAKELPARAM(gx, gy));
    return TAGPU_DELIVER_ENGINE;
}

BOOL tagpu_shield_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result)
{
    /* Each tagged message reports its sequence number once its delivery has returned: TA's
       window procedure has pushed whatever the message pushes into the engine's rings by then,
       which is the half of consumption tagpu_input.c cannot read from the rings themselves. */
    switch (msg)
    {
    case WM_TAGPU_KEY:
        deliver_key(hwnd, (int)(wparam & TAGPU_KEY_VK), (lparam & TAGPU_KEY_UP) != 0);
        tagpu_input_delivered(TAGPU_SEQ(wparam), TAGPU_DELIVER_ENGINE);
        *result = 0;
        return TRUE;

    case WM_TAGPU_CHAR:
        to_game(hwnd, WM_CHAR, wparam, 1);
        tagpu_input_delivered((unsigned)lparam, TAGPU_DELIVER_ENGINE);
        *result = 0;
        return TRUE;

    case WM_TAGPU_MOUSE:
        tagpu_input_delivered(TAGPU_SEQ(wparam),
                              deliver_mouse(hwnd, (int)(wparam & TAGPU_M_CODE),
                                            (short)LOWORD(lparam), (short)HIWORD(lparam)));
        *result = 0;
        return TRUE;
    }

    /* The sprocket and the replay HUD, on the REAL path -- before the shield's
       own gate, so they answer a player's mouse whether the shield is armed or
       not. The sprocket's point comes from g_ddraw.cursor rather than lParam
       because this hook runs at the top of the wndproc, before cnc-ddraw has
       unscaled a hardware message into game space; the preceding WM_MOUSEMOVE
       has already put the game-space point there, and it is the same value the
       injected path hands the same function. The HUD's is lParam itself, the
       client point, and it tests the panel at the sprocket's point, so the two
       split the screen on one conversion. A double click is a press to the
       HUD: the window's class may turn a second press into one. */
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ||
        msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK)
    {
        int cx = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
        int cy = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);
        int right = msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_RBUTTONDBLCLK;
        int down = msg != WM_LBUTTONUP && msg != WM_RBUTTONUP;
        int took = (!right && msg != WM_LBUTTONDBLCLK && tagpu_menu_click(cx, cy, down)) ||
                   tagpu_hudinput_button(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), cx, cy, down, right);

        if (took)
        {
            *result = 0;
            return TRUE;
        }
    }

    /* The window lost the mouse: a release may never come, so the HUD's drag ends here. The
       messages go on as before. */
    if (msg == WM_KILLFOCUS || msg == WM_CAPTURECHANGED || (msg == WM_ACTIVATEAPP && !wparam))
        tagpu_hudinput_cancel();

    if (!tagpu_shield_on())
    {
        /* Alt+Z / Alt+X with a building armed: the placement's spacing, the player's keys only
           while the shield lets them through. A letter under Alt is WM_SYSKEYDOWN. */
        if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) &&
            tagpu_place_key((unsigned)wparam, msg == WM_SYSKEYDOWN))
        {
            *result = 0;
            return TRUE;
        }
        return FALSE;
    }

    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_DEADCHAR:
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
    case WM_INPUT:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHOVER:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        *result = 0;
        return TRUE;
    }

    return FALSE;
}

/* -------------------------------------------------------------- injection */

/* Each returns PostMessageA's answer: the injector refuses the rest of its batch on a FALSE.
   GAME THREAD, like the wndproc that delivers them: the token half of tagpu_input.c runs at the
   engine's flip. That placement is also what keeps clear_state(), which memsets the wndproc's
   s_down/s_async, on the wndproc's own thread. */
BOOL tagpu_shield_key(HWND hwnd, int vk, BOOL up, unsigned seq)
{
    if (!hwnd || vk <= 0 || vk >= 256)
        return FALSE;

    return PostMessageA(hwnd, WM_TAGPU_KEY, (WPARAM)vk | TAGPU_SEQ_WPARAM(seq),
                        up ? TAGPU_KEY_UP : 0);
}

BOOL tagpu_shield_char(HWND hwnd, char ch, unsigned seq)
{
    return hwnd && PostMessageA(hwnd, WM_TAGPU_CHAR, (WPARAM)(BYTE)ch, (LPARAM)(seq & 0xFFFF));
}

BOOL tagpu_shield_mouse(HWND hwnd, int code, int gx, int gy, unsigned seq)
{
    return hwnd && PostMessageA(hwnd, WM_TAGPU_MOUSE,
                                (WPARAM)(code & TAGPU_M_CODE) | TAGPU_SEQ_WPARAM(seq),
                                MAKELPARAM(gx, gy));
}
