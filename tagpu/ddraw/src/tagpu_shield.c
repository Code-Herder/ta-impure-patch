/* tagpu_shield.c — the input firewall (phase 1.1).

   Rationale and message contract: inc/tagpu_shield.h.

   Two states are kept here: which keys/buttons the injector says are down, and
   where it says the pointer is. Everything that reads input — the wndproc, and
   the polled GetCursorPos/GetKeyState/GetAsyncKeyState hooks — reads those
   instead of the real devices while the shield is armed, so the game's idea of
   the input devices is entirely ours. The human's keyboard and mouse become
   invisible to the game; the desktop stays theirs. */

#include <windows.h>
#include <stdio.h>
#include "dd.h"
#include "hook.h"
#include "tagpu_shield.h"
#include "tagpu_zoom.h"

#define SHIELD_TRIGGER  "tagpu_shield.on"

static volatile LONG s_armed = -1;      /* -1 until the trigger is first read */
static BYTE  s_down[256];               /* injected key/button down state */
static BYTE  s_toggle[256];             /* caps/num-style toggle bit */
static BYTE  s_async[256];              /* pressed since the last async read */
static DWORD s_release[256];            /* auto-release deadline, 0 = held/none */
static DWORD s_held_since[256];         /* when a hold started (for its report) */
static volatile LONG s_polls[256];      /* how often the game asked about a key */
static volatile LONG s_downpolls[256];  /* ...and was told "down" */

static void slog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
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
    ZeroMemory((void*)s_release, sizeof(s_release));
}

void tagpu_shield_frame(HWND hwnd)
{
    /* the trigger is a file: probe it every 15th frame like the other triggers,
       but expire held keys every frame so a hold means what it says */
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

    if (!hwnd)
        return;

    DWORD tick = GetTickCount();

    for (int vk = 0; vk < 256; vk++)
    {
        if (s_release[vk] && (LONG)(tick - s_release[vk]) >= 0)
        {
            s_release[vk] = 0;
            PostMessageA(hwnd, WM_TAGPU_KEY, (WPARAM)vk, TAGPU_KEY_UP);

            /* Did the game actually look? This is the phase-1.1 claim made
               measurable: "held for Nms, the game asked K times and was told
               down D times". D=0 with K>0 means it polls other keys but not
               this one; K=0 means it does not poll the keyboard at all here
               and a modifier has to reach it some other way. */
            char b[128];
            _snprintf(b, sizeof b,
                      "shield: vk=%d released after %ums, polls=%ld down=%ld",
                      vk, (unsigned)(tick - s_held_since[vk]),
                      (long)s_polls[vk], (long)s_downpolls[vk]);
            slog(b);
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
        s_polls[vk]++;

        if (s_down[vk])
        {
            s |= (SHORT)0x8000;
            s_downpolls[vk]++;
        }

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
        if (tagpu_zoom_wheel(msg, wparam, lparam))
            return 0;
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

static void deliver_mouse(HWND hwnd, int code, int gx, int gy)
{
    int cur_x = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
    int cur_y = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);

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
       the high word of wParam and a CLIENT-space point in lParam, which is what
       cnc-ddraw has already made of a real wheel by this point. Going through
       the same message rather than calling the zoom directly is what makes an
       injected wheel test the path a player's wheel takes. */
    case TAGPU_M_WHEELUP: msg = WM_MOUSEWHEEL; wheel =  WHEEL_DELTA; break;
    case TAGPU_M_WHEELDN: msg = WM_MOUSEWHEEL; wheel = -WHEEL_DELTA; break;
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
        if (drop) return;
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
}

BOOL tagpu_shield_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result)
{
    switch (msg)
    {
    case WM_TAGPU_KEY:
        deliver_key(hwnd, (int)wparam, (lparam & TAGPU_KEY_UP) != 0);
        *result = 0;
        return TRUE;

    case WM_TAGPU_CHAR:
        to_game(hwnd, WM_CHAR, wparam, 1);
        *result = 0;
        return TRUE;

    case WM_TAGPU_MOUSE:
        deliver_mouse(hwnd, (int)wparam, (short)LOWORD(lparam), (short)HIWORD(lparam));
        *result = 0;
        return TRUE;
    }

    if (!tagpu_shield_on())
        return FALSE;

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

/* s_release belongs to the injector (present-hook thread); s_down/s_toggle to the
   wndproc (game thread). Keeping the two apart is what makes the timed release
   race-free — set the deadline before posting, and never clear it from delivery. */
void tagpu_shield_key(HWND hwnd, int vk, BOOL up)
{
    if (!hwnd || vk <= 0 || vk >= 256)
        return;

    s_release[vk] = 0;              /* explicit down/up: the script owns the state */
    PostMessageA(hwnd, WM_TAGPU_KEY, (WPARAM)vk, up ? TAGPU_KEY_UP : 0);
}

void tagpu_shield_key_hold(HWND hwnd, int vk, DWORD hold_ms)
{
    if (!hwnd || vk <= 0 || vk >= 256)
        return;

    DWORD now = GetTickCount();
    DWORD deadline = now + hold_ms;

    s_polls[vk] = s_downpolls[vk] = 0;
    s_held_since[vk] = now;
    s_release[vk] = deadline ? deadline : 1;
    PostMessageA(hwnd, WM_TAGPU_KEY, (WPARAM)vk, 0);
}

void tagpu_shield_char(HWND hwnd, char ch)
{
    if (hwnd)
        PostMessageA(hwnd, WM_TAGPU_CHAR, (WPARAM)(BYTE)ch, 0);
}

void tagpu_shield_mouse(HWND hwnd, int code, int gx, int gy)
{
    if (hwnd)
        PostMessageA(hwnd, WM_TAGPU_MOUSE, (WPARAM)code, MAKELPARAM(gx, gy));
}
