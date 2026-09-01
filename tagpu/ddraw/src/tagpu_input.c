/* tagpu_input.c — in-process input injection (session-proof game driving).

   2026-09-01: X-level injection (XTEST/XSendEvent) proved unreliable — a
   locked/half-dead GNOME session holds a server grab and keys either vanish
   or land in the user's UNLOCK DIALOG (see memory ta-input-injection-safety).
   The fix: we live inside the process, so drive the game with its own
   message queue and display state. No X involved; works under any lock.

     tagpu_keys.txt   whitespace-separated key tokens, consumed ONCE (file is
                      deleted after reading): a..z 0..9 space return escape
                      plus minus up down left right f1..f12 tab
                      Each token posts WM_KEYDOWN+WM_KEYUP to the game window;
                      the game's own pump runs TranslateMessage, so dialog
                      accelerators and chat chars behave like real typing.

     tagpu_eye.txt    "X Y" (world px). While the file exists the camera eye
                      (main+0x1431F/0x14323) is written every frame, clamped
                      to the map; delete the file to release. The eye is
                      display state, not sim state — read-only-over-sim holds.

   Both are checked every 15 frames from the present hook.

   2026-09-01 (phase 1.1): keys and clicks now travel as tagged WM_TAGPU_*
   messages that the shield translates in the wndproc, so they survive the
   hardware-input filter and drive a virtual key/cursor state the game's polls
   read (inc/tagpu_shield.h). That is what makes ctrl/shift combos land. */

#include <windows.h>
#include <stdio.h>
#include "tagpu.h"
#include "tagpu_input.h"
#include "tagpu_shield.h"
#include "mouse.h"        /* the fork's own mouse-lock (wndproc drops mouse
                             messages while unlocked — the "clicks never work
                             under a locked session" root cause) */
extern BOOL g_mouse_locked;

/* how long an injected modifier stays down around the key it qualifies. TA
   polls modifier state instead of reading it off the message, so an interleaved
   up would race that poll — hold it a few frames instead. */
#define MOD_HOLD_MS 150

#define TA_MAINPP   0x00511DE8u
#define OFF_EYEX    0x1431F
#define OFF_EYEY    0x14323
#define OFF_SCRTX   0x14327    /* MapXScrollingTo — the engine eases the    */
#define OFF_SCRTY   0x1432B    /* eye toward these; write them too or WAR   */
#define OFF_MAPPXW  0x14223    /* map W in px */
#define OFF_MAPPXH  0x14227    /* map H in px */
#define OFF_VIEW_W  0x37E37
#define OFF_VIEW_H  0x37E3B

static void ilog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int ptr_ok(const void* p) { return (size_t)p > 0x600000u && (size_t)p < 0x7FFF0000u; }

static int token_vk(const char* t)
{
    if (!t[1] && t[0] >= 'a' && t[0] <= 'z') return 'A' + (t[0] - 'a');
    if (!t[1] && t[0] >= '0' && t[0] <= '9') return t[0];
    if (!lstrcmpiA(t, "ctrl"))    return VK_CONTROL;
    if (!lstrcmpiA(t, "shift"))   return VK_SHIFT;
    if (!lstrcmpiA(t, "alt"))     return VK_MENU;
    if (!lstrcmpiA(t, "lbutton")) return VK_LBUTTON;
    if (!lstrcmpiA(t, "rbutton")) return VK_RBUTTON;
    if (!lstrcmpiA(t, "mbutton")) return VK_MBUTTON;
    if (!lstrcmpiA(t, "space"))  return VK_SPACE;
    if (!lstrcmpiA(t, "return") || !lstrcmpiA(t, "enter")) return VK_RETURN;
    if (!lstrcmpiA(t, "escape") || !lstrcmpiA(t, "esc"))   return VK_ESCAPE;
    if (!lstrcmpiA(t, "plus"))   return VK_ADD;
    if (!lstrcmpiA(t, "minus"))  return VK_SUBTRACT;
    if (!lstrcmpiA(t, "up"))     return VK_UP;
    if (!lstrcmpiA(t, "down"))   return VK_DOWN;
    if (!lstrcmpiA(t, "left"))   return VK_LEFT;
    if (!lstrcmpiA(t, "right"))  return VK_RIGHT;
    if (!lstrcmpiA(t, "tab"))    return VK_TAB;
    if ((t[0] == 'f' || t[0] == 'F') && t[1]) {
        int n = 0; const char* p = t + 1;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        if (!*p && n >= 1 && n <= 12) return VK_F1 + n - 1;
    }
    return 0;
}

static void tap_key(HWND w, int vk)
{
    tagpu_shield_key(w, vk, FALSE);
    tagpu_shield_key(w, vk, TRUE);
}

/* "ctrl+", "shift+", "alt+" prefix test (case-insensitive, no CRT dependency) */
static int mod_prefix(const char* t, const char* name, int n)
{
    for (int i = 0; i < n; i++) {
        char a = t[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (a != name[i]) return 0;
    }
    return t[n] == '+';
}

/* mouse button token -> the down/up pair that drives it */
static int button_code(int vk, int up)
{
    switch (vk) {
    case VK_LBUTTON: return up ? TAGPU_M_LUP : TAGPU_M_LDOWN;
    case VK_RBUTTON: return up ? TAGPU_M_RUP : TAGPU_M_RDOWN;
    case VK_MBUTTON: return up ? TAGPU_M_MUP : TAGPU_M_MDOWN;
    }
    return -1;
}

/* game px -> virtual-screen absolute (0..65535) via the live window rect and
   the frame's letterbox viewport — no hardcoded geometry anywhere */
static int game_to_abs(const TAGPU_FRAME* f, int gx, int gy, LONG* ax, LONG* ay)
{
    RECT r;
    if (!f->hwnd || !GetWindowRect((HWND)f->hwnd, &r)) return 0;
    if (f->game_width <= 0 || f->game_height <= 0 || f->vp_w <= 0 || f->vp_h <= 0) return 0;
    int px = r.left + f->vp_x + (int)((gx + 0.5) * f->vp_w / f->game_width);
    int py = r.top  + f->vp_y + (int)((gy + 0.5) * f->vp_h / f->game_height);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) { vx = vy = 0; vw = GetSystemMetrics(SM_CXSCREEN); vh = GetSystemMetrics(SM_CYSCREEN); }
    if (vw <= 0 || vh <= 0) return 0;
    *ax = (LONG)(((LONGLONG)(px - vx) * 65535) / (vw - 1));
    *ay = (LONG)(((LONGLONG)(py - vy) * 65535) / (vh - 1));
    return 1;
}

/* SendInput path — used only while the shield is disarmed, and only for the
   mouse: it drives wine's real input queue, which moves the human's pointer.
   The keyboard equivalent is gone; the shield's tagged messages replace it. */
static void si_mouse(const TAGPU_FRAME* f, int gx, int gy, DWORD buttons)
{
    LONG ax, ay;
    if (!game_to_abs(f, gx, gy, &ax, &ay)) return;
    INPUT in; ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = ax; in.mi.dy = ay;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof in);
    if (buttons) {
        in.mi.dwFlags = buttons & 1 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
        SendInput(1, &in, sizeof in);
        in.mi.dwFlags = buttons & 1 ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
        SendInput(1, &in, sizeof in);
    }
}

static const TAGPU_FRAME* s_frame;   /* set per call for mouse tokens */

/* injected click: move, press, release, then park the pointer at the centre of
   the view — a click near a screen edge otherwise leaves the engine's memory
   mouse there and it EDGE-SCROLLS forever. */
static void inject_click(HWND w, int gx, int gy, int right)
{
    tagpu_shield_mouse(w, TAGPU_M_MOVE, gx, gy);
    tagpu_shield_mouse(w, right ? TAGPU_M_RDOWN : TAGPU_M_LDOWN, gx, gy);
    tagpu_shield_mouse(w, right ? TAGPU_M_RUP : TAGPU_M_LUP, gx, gy);

    if (s_frame && s_frame->game_width > 0 && s_frame->game_height > 0)
        tagpu_shield_mouse(w, TAGPU_M_MOVE,
                           s_frame->game_width / 2, s_frame->game_height / 2);
}

static void do_keys(HWND hwnd)
{
    char buf[256]; DWORD n = 0;
    HANDLE h = CreateFileA("tagpu_keys.txt", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL)) n = 0;
    CloseHandle(h);
    DeleteFileA("tagpu_keys.txt");
    if (n == 0) return;
    buf[n] = 0;

    char* p = buf;
    char lg[300]; int lo = _snprintf(lg, sizeof lg, "input: keys");
    while (*p) {
        while (*p && *p <= ' ') p++;
        char* q = p;
        while (*q && *q > ' ') q++;
        int last = (*q == 0);
        *q = 0;
        if (*p) {
            int gx, gy, done = 1;
            char tok[64];
            int mods[4], nmods = 0;
            const char* base = p;

            /* stackable modifier prefixes: ctrl+d, ctrl+shift+2, alt+f4 */
            while (nmods < 4) {
                if (mod_prefix(base, "ctrl", 4))       { mods[nmods++] = VK_CONTROL; base += 5; }
                else if (mod_prefix(base, "shift", 5)) { mods[nmods++] = VK_SHIFT;   base += 6; }
                else if (mod_prefix(base, "alt", 3))   { mods[nmods++] = VK_MENU;    base += 4; }
                else break;
            }

            if (nmods) {
                int vk = token_vk(base);
                if (vk) {
                    /* the modifiers stay down past the keystroke and are released
                       by the frame hook — an interleaved up races TA's poll */
                    for (int i = 0; i < nmods; i++)
                        tagpu_shield_key_hold(hwnd, mods[i], MOD_HOLD_MS);
                    tap_key(hwnd, vk);
                } else done = 0;
            }
            else if (sscanf(p, "mouse:%d,%d", &gx, &gy) == 2) {
                if (tagpu_shield_on()) tagpu_shield_mouse(hwnd, TAGPU_M_MOVE, gx, gy);
                else si_mouse(s_frame, gx, gy, 0);
            }
            else if (sscanf(p, "click:%d,%d", &gx, &gy) == 2) {
                if (tagpu_shield_on()) inject_click(hwnd, gx, gy, 0);
                else si_mouse(s_frame, gx, gy, 1);
            }
            else if (sscanf(p, "rclick:%d,%d", &gx, &gy) == 2) {
                if (tagpu_shield_on()) inject_click(hwnd, gx, gy, 1);
                else si_mouse(s_frame, gx, gy, 2);
            }
            else if (sscanf(p, "mouserel:%d,%d", &gx, &gy) == 2) {
                if (tagpu_shield_on()) tagpu_shield_mouse(hwnd, TAGPU_M_MOVEREL, gx, gy);
                else {
                    INPUT in; ZeroMemory(&in, sizeof in);
                    in.type = INPUT_MOUSE; in.mi.dx = gx; in.mi.dy = gy;
                    in.mi.dwFlags = MOUSEEVENTF_MOVE;
                    SendInput(1, &in, sizeof in);
                }
            }
            else if (sscanf(p, "pclick:%d,%d", &gx, &gy) == 2)  inject_click(hwnd, gx, gy, 0);
            else if (sscanf(p, "prclick:%d,%d", &gx, &gy) == 2) inject_click(hwnd, gx, gy, 1);
            else if (sscanf(p, "char:%c", (char*)&gx) == 1) {
                tagpu_shield_char(hwnd, (char)gx);
            }
            else if (!lstrcmpiA(p, "mouselock")) {
                mouse_lock();
                ilog(g_mouse_locked ? "input: mouse LOCKED" : "input: mouse lock FAILED");
            }
            else if (!lstrcmpiA(p, "click") || !lstrcmpiA(p, "rclick")) {
                int rt = (p[0] == 'r' || p[0] == 'R');
                if (tagpu_shield_on()) {
                    tagpu_shield_mouse(hwnd, button_code(rt ? VK_RBUTTON : VK_LBUTTON, 0),
                                       TAGPU_M_HERE, TAGPU_M_HERE);
                    tagpu_shield_mouse(hwnd, button_code(rt ? VK_RBUTTON : VK_LBUTTON, 1),
                                       TAGPU_M_HERE, TAGPU_M_HERE);
                } else {
                    INPUT in; ZeroMemory(&in, sizeof in);
                    in.type = INPUT_MOUSE;
                    in.mi.dwFlags = rt ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
                    SendInput(1, &in, sizeof in);
                    in.mi.dwFlags = rt ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
                    SendInput(1, &in, sizeof in);
                }
            }
            else if (sscanf(p, "down:%63s", tok) == 1 || sscanf(p, "up:%63s", tok) == 1) {
                /* explicit hold/release: the only way to keep a key or a mouse
                   button down across frames (drag-select, held modifiers) */
                int up = (p[0] == 'u' || p[0] == 'U');
                int vk = token_vk(tok), code;
                if (!vk) done = 0;
                else if ((code = button_code(vk, up)) >= 0)
                    tagpu_shield_mouse(hwnd, code, TAGPU_M_HERE, TAGPU_M_HERE);
                else
                    tagpu_shield_key(hwnd, vk, up);
            }
            else if (sscanf(p, "keydown:%d", &gx) == 1 || sscanf(p, "keyup:%d", &gx) == 1) {
                tagpu_shield_key(hwnd, gx, p[3] == 'u');
            }
            else {
                int vk = token_vk(p);
                if (vk) tap_key(hwnd, vk); else done = 0;
            }
            if (lo < (int)sizeof lg - 8)
                lo += _snprintf(lg + lo, sizeof lg - lo, " %s%s", p, done ? "" : "?");
        }
        if (last) break;
        p = q + 1;
    }
    ilog(lg);
}

static void do_eye(void)
{
    char buf[64]; DWORD n = 0;
    HANDLE h = CreateFileA("tagpu_eye.txt", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL)) n = 0;
    CloseHandle(h);
    if (n == 0) return;
    buf[n] = 0;

    int x = 0, y = 0;
    if (sscanf(buf, "%d %d", &x, &y) != 2) return;
    char* ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) return;
    /* expose eye wars: if the engine moved the eye away from the hold target
       since last frame, say so (the units: log samples AFTER our write and
       would otherwise hide the fight) */
    {
        static unsigned lastwar = 0; static unsigned warn = 0;
        int cx = *(volatile int*)(ta + OFF_EYEX), cy = *(volatile int*)(ta + OFF_EYEY);
        int dx = cx - x, dy = cy - y;
        if ((dx > 8 || dx < -8 || dy > 8 || dy < -8) && ++warn >= 60) {
            warn = 0;
            char b[96];
            _snprintf(b, sizeof b, "eye: WAR engine=(%d,%d) hold=(%d,%d)", cx, cy, x, y);
            ilog(b);
        }
        (void)lastwar;
    }
    int mw = *(int*)(ta + OFF_MAPPXW), mh = *(int*)(ta + OFF_MAPPXH);
    int vw = *(int*)(ta + OFF_VIEW_W), vh = *(int*)(ta + OFF_VIEW_H);
    if (mw <= 0 || mh <= 0 || vw <= 0 || vh <= 0) return;
    if (x < 0) x = 0; else if (x > mw - vw) x = mw - vw;
    if (y < 0) y = 0; else if (y > mh - vh) y = mh - vh;
    *(volatile int*)(ta + OFF_EYEX) = x;
    *(volatile int*)(ta + OFF_EYEY) = y;
    *(volatile int*)(ta + OFF_SCRTX) = x;
    *(volatile int*)(ta + OFF_SCRTY) = y;
}

void tagpu_input_frame(const TAGPU_FRAME* f)
{
    static unsigned last = 0;
    /* eye hold: every frame (cheap open-fail when absent is the common path
       is wrong way round — probe with GetFileAttributes first) */
    static int eyeHold = 0;

    /* every frame: held modifiers must be released on time, not on the next
       15-frame token poll */
    tagpu_shield_frame((HWND)f->hwnd);

    if (f->frame_counter - last >= 15) {
        last = f->frame_counter;
        s_frame = f;
        if (f->hwnd) do_keys((HWND)f->hwnd);
        eyeHold = (GetFileAttributesA("tagpu_eye.txt") != INVALID_FILE_ATTRIBUTES);
    }
    if (eyeHold) do_eye();
}
