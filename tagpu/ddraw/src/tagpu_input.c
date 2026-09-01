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

   Both are checked every 15 frames from the present hook. */

#include <windows.h>
#include <stdio.h>
#include "tagpu.h"
#include "tagpu_input.h"
#include "mouse.h"        /* the fork's own mouse-lock (wndproc drops mouse
                             messages while unlocked — the "clicks never work
                             under a locked session" root cause) */
extern BOOL g_mouse_locked;

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

static void post_key(HWND w, int vk)
{
    UINT sc = MapVirtualKeyA((UINT)vk, 0 /* MAPVK_VK_TO_VSC */);
    LPARAM down = 1 | ((LPARAM)sc << 16);
    LPARAM up   = down | ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
    PostMessageA(w, WM_KEYDOWN, (WPARAM)vk, down);
    PostMessageA(w, WM_KEYUP,   (WPARAM)vk, up);
}

/* SendInput path: real synthetic input through wine's input queue — updates
   thread key state (GetKeyState works, so Ctrl+X combos land) and feeds the
   game's normal mouse pipeline. No X involved. */
static void si_key(int vk, int up)
{
    INPUT in; ZeroMemory(&in, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyA((UINT)vk, 0);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof in);
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
            if (sscanf(p, "mouse:%d,%d", &gx, &gy) == 2)       si_mouse(s_frame, gx, gy, 0);
            else if (sscanf(p, "click:%d,%d", &gx, &gy) == 2)  si_mouse(s_frame, gx, gy, 1);
            else if (sscanf(p, "rclick:%d,%d", &gx, &gy) == 2) si_mouse(s_frame, gx, gy, 2);
            else if (sscanf(p, "mouserel:%d,%d", &gx, &gy) == 2) {
                INPUT in; ZeroMemory(&in, sizeof in);
                in.type = INPUT_MOUSE; in.mi.dx = gx; in.mi.dy = gy;
                in.mi.dwFlags = MOUSEEVENTF_MOVE;
                SendInput(1, &in, sizeof in);
            }
            else if (sscanf(p, "pclick:%d,%d", &gx, &gy) == 2 ||
                     sscanf(p, "prclick:%d,%d", &gx, &gy) == 2) {
                /* posted click WITH position: client-coord lParam so the
                   wndproc translation hands TA exact game coords (SendInput
                   buttons carry wine's stale internal position instead) */
                int rt = (p[0] == 'p' && (p[1] == 'r' || p[1] == 'R'));
                if (s_frame && s_frame->game_width > 0 && s_frame->vp_w > 0) {
                    int cx = s_frame->vp_x + (int)((gx + 0.5) * s_frame->vp_w / s_frame->game_width);
                    int cy = s_frame->vp_y + (int)((gy + 0.5) * s_frame->vp_h / s_frame->game_height);
                    LPARAM lp = MAKELPARAM(cx, cy);
                    PostMessageA(hwnd, WM_MOUSEMOVE, 0, lp);
                    PostMessageA(hwnd, rt ? WM_RBUTTONDOWN : WM_LBUTTONDOWN,
                                 rt ? MK_RBUTTON : MK_LBUTTON, lp);
                    PostMessageA(hwnd, rt ? WM_RBUTTONUP : WM_LBUTTONUP, 0, lp);
                    /* park the game mouse at view centre afterwards — a click
                       near a screen edge otherwise leaves the memory mouse
                       there and the engine EDGE-SCROLLS forever */
                    {
                        int mx = s_frame->vp_x + s_frame->vp_w / 2;
                        int my = s_frame->vp_y + s_frame->vp_h / 2;
                        PostMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(mx, my));
                    }
                }
            }
            else if (sscanf(p, "char:%c", (char*)&gx) == 1) {
                PostMessageA(hwnd, WM_CHAR, (WPARAM)(char)gx, 1);
            }
            else if (!lstrcmpiA(p, "mouselock")) {
                mouse_lock();
                ilog(g_mouse_locked ? "input: mouse LOCKED" : "input: mouse lock FAILED");
            }
            else if (!lstrcmpiA(p, "click") || !lstrcmpiA(p, "rclick")) {
                int rt = (p[0] == 'r' || p[0] == 'R');
                INPUT in; ZeroMemory(&in, sizeof in);
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = rt ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
                SendInput(1, &in, sizeof in);
                in.mi.dwFlags = rt ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
                SendInput(1, &in, sizeof in);
            }
            else if (p[0]=='c'&&p[1]=='t'&&p[2]=='r'&&p[3]=='l'&&p[4]=='+'&&p[5]&&!p[6]) {
                /* posted interleaved ctrl combo (SendInput keys do not reach
                   TA under the shield; posted messages do) */
                int vk = token_vk(p + 5);
                if (vk) {
                    UINT scC = MapVirtualKeyA(VK_CONTROL, 0), scK = MapVirtualKeyA((UINT)vk, 0);
                    PostMessageA(hwnd, WM_KEYDOWN, VK_CONTROL, 1 | ((LPARAM)scC << 16));
                    PostMessageA(hwnd, WM_KEYDOWN, (WPARAM)vk, 1 | ((LPARAM)scK << 16));
                    PostMessageA(hwnd, WM_KEYUP, (WPARAM)vk,
                                 1 | ((LPARAM)scK << 16) | ((LPARAM)3 << 30));
                    PostMessageA(hwnd, WM_KEYUP, VK_CONTROL,
                                 1 | ((LPARAM)scC << 16) | ((LPARAM)3 << 30));
                } else done = 0;
            }
            else if (sscanf(p, "keydown:%d", &gx) == 1 || sscanf(p, "keyup:%d", &gx) == 1) {
                int up = (p[3] == 'u');
                UINT sc = MapVirtualKeyA((UINT)gx, 0);
                PostMessageA(hwnd, up ? WM_KEYUP : WM_KEYDOWN, (WPARAM)gx,
                             1 | ((LPARAM)sc << 16) | (up ? ((LPARAM)3 << 30) : 0));
            }
            else {
                int vk = token_vk(p);
                if (vk) post_key(hwnd, vk); else done = 0;
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
    if (f->frame_counter - last >= 15) {
        last = f->frame_counter;
        s_frame = f;
        if (f->hwnd) do_keys((HWND)f->hwnd);
        eyeHold = (GetFileAttributesA("tagpu_eye.txt") != INVALID_FILE_ATTRIBUTES);
    }
    if (eyeHold) do_eye();
}
