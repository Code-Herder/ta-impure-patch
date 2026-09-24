/* tagpu_input.c — in-process input injection (session-proof game driving).

   X-level injection (XTEST/XSendEvent) is unreliable: a locked/half-dead
   GNOME session holds a server grab and keys either vanish or land in the
   user's UNLOCK DIALOG. We live inside the process, so we drive the game with
   its own message queue and display state. No X involved; works under any
   lock.

     tagpu_keys.txt   whitespace-separated key tokens, consumed ONCE (file is
                      deleted after reading): a..z 0..9 space return escape
                      plus minus up down left right f1..f12 tab
                      Each token posts WM_KEYDOWN+WM_KEYUP to the game window;
                      the game's own pump runs TranslateMessage, so dialog
                      accelerators and chat chars behave like real typing.

     tagpu_eye.txt    "X Y" (world px). While the file exists the camera eye
                      (main+0x1431F/0x14323) is held there: the point rides
                      the frame packet's command record and the GAME THREAD
                      writes the eye and its scroll target, clamped into the
                      camera's range, at the top of every in-play draw
                      (tagpu_zoom_apply). Delete the file to release. The eye
                      is display state, not sim state — read-only-over-sim
                      holds, and this module writes no engine memory at all.

   Both are checked every 15 frames from the present hook.

   Keys and clicks travel as tagged WM_TAGPU_*
   messages that the shield translates in the wndproc, so they survive the
   hardware-input filter and drive a virtual key/cursor state the game's polls
   read (inc/tagpu_shield.h). That is what makes ctrl/shift combos land. */

#include <windows.h>
#include <stdio.h>
#include "tagpu.h"
#include "tagpu_input.h"
#include "tagpu_shield.h"
#include "tagpu_packet.h"  /* the command record the hold rides on */
#include "tagpu_log.h"
#include "mouse.h"        /* the fork's own mouse-lock (wndproc drops mouse
                             messages while unlocked — the "clicks never work
                             under a locked session" root cause) */
extern BOOL g_mouse_locked;

/* how long an injected modifier stays down around the key it qualifies. TA
   polls modifier state instead of reading it off the message, so an interleaved
   up would race that poll — hold it a few frames instead. */
#define MOD_HOLD_MS 150

static void ilog(const char* s)
{
    tagpu_log(s);
}

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
    /* text entry (tacli ui fill): clearing a field means one backspace per
       character already in it. */
    if (!lstrcmpiA(t, "backspace") || !lstrcmpiA(t, "back")) return VK_BACK;
    if (!lstrcmpiA(t, "delete") || !lstrcmpiA(t, "del"))     return VK_DELETE;
    if (!lstrcmpiA(t, "home"))   return VK_HOME;
    if (!lstrcmpiA(t, "end"))    return VK_END;
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
static void inject_click_at(HWND w, int gx, int gy, int right, int dev)
{
    int d = dev ? TAGPU_M_DEV : 0;
    tagpu_shield_mouse(w, TAGPU_M_MOVE | d, gx, gy);
    tagpu_shield_mouse(w, (right ? TAGPU_M_RDOWN : TAGPU_M_LDOWN) | d, gx, gy);
    tagpu_shield_mouse(w, (right ? TAGPU_M_RUP : TAGPU_M_LUP) | d, gx, gy);

    /* park in the engine's own coordinates whichever space the click was in:
       the parking position is about the engine's memory mouse, not the window */
    if (s_frame && s_frame->game_width > 0 && s_frame->game_height > 0)
        tagpu_shield_mouse(w, TAGPU_M_MOVE,
                           s_frame->game_width / 2, s_frame->game_height / 2);
}
static void inject_click(HWND w, int gx, int gy, int right)
{
    inject_click_at(w, gx, gy, right, 0);
}

static void do_keys(HWND hwnd)
{
    /* One `tacli ui fill` can emit a token per character plus a backspace per
       character already in the field, so 256 bytes is not enough headroom: an
       over-long batch would be silently truncated and its tail deleted with
       the file. */
    char buf[1024]; DWORD n = 0;
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
            /* DEVICE-SPACE click and move: x,y are client-area pixels
               and are converted by the same `mouse_client_to_game` a hardware
               click goes through. Every other token here speaks the engine's
               coordinates and so proves nothing about the pointer path; these
               two are the only ones that test it, which is what phase 2's kill
               rule needs (gui-renderer.md 13.1, 13.9). */
            else if (sscanf(p, "dclick:%d,%d", &gx, &gy) == 2)  inject_click_at(hwnd, gx, gy, 0, 1);
            else if (sscanf(p, "drclick:%d,%d", &gx, &gy) == 2) inject_click_at(hwnd, gx, gy, 1, 1);
            else if (sscanf(p, "dmove:%d,%d", &gx, &gy) == 2)
                tagpu_shield_mouse(hwnd, TAGPU_M_MOVE | TAGPU_M_DEV, gx, gy);
            /* injected move with no click and no parking: what a hover is. The
               plain "mouse:" token falls back to SendInput when the shield is
               down, which would drag the human's real pointer. */
            else if (sscanf(p, "pmove:%d,%d", &gx, &gy) == 2)
                tagpu_shield_mouse(hwnd, TAGPU_M_MOVE, gx, gy);
            /* `wheel:+3` / `wheel:-3` — N notches at the pointer, which is the
               zoom control (tagpu_zoom.h). It is posted where the pointer is
               and NOT where a token says, because the wheel is only live over
               the world viewport: aim it with `pmove:` first, exactly as a
               player aims it by moving the mouse. The count is capped so a
               fat-fingered `wheel:100000` cannot flood the message queue. */
            else if (sscanf(p, "wheel:%d", &gx) == 1) {
                int n = gx < 0 ? -gx : gx;
                int code = gx < 0 ? TAGPU_M_WHEELDN : TAGPU_M_WHEELUP;
                if (n > 64) n = 64;
                while (n-- > 0)
                    tagpu_shield_mouse(hwnd, code, TAGPU_M_HERE, TAGPU_M_HERE);
            }
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
            /* _snprintf returns -1 when it truncates, so the cursor has to be
               re-clamped or a long batch walks it backwards off the buffer. */
            if (lo >= 0 && lo < (int)sizeof lg - 8)
            {
                int w = _snprintf(lg + lo, sizeof lg - lo - 1, " %s%s", p,
                                  done ? "" : "?");
                lo = (w < 0) ? (int)sizeof lg : lo + w;
                lg[sizeof lg - 1] = 0;
            }
        }
        if (last) break;
        p = q + 1;
    }
    ilog(lg);
}

/* eye hold: polled every 15 render frames (a GetFileAttributes probe first,
   because the absent case is the common one and an open-fail per frame is not
   free). Render thread only: the point is read here and rides the command
   record; the write is the game thread's (tagpu_zoom_apply), clamped there
   into the camera's range in force for that draw.

   AND THIS HALF IS ON THE RENDER THREAD, deliberately.
   These four words are written here and read by tagpu_input_cmd(), which
   tagpu_zoom_frame_end() calls — and that runs only inside tagpu_overlay_draw,
   i.e. only on the lanes that draw the overlay. Moving the poll to the game
   thread would make all four cross-thread to serve a consumer that is not on
   the other side, and on renderer=gdi it would deliver nothing either way:
   tagpu_cmd_post() is never reached there, so the hold could not be applied
   even if it were read. Same thread as its only readers is the invariant. */
static int s_eyeHold = 0;
static int s_holdX, s_holdY, s_holdValid;

static void do_eye(const TAGPU_FRAME* f)
{
    char buf[64]; DWORD n = 0;
    HANDLE h = CreateFileA("tagpu_eye.txt", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    /* THE HOLD ENDS ON THE FIRST FRAME THE FILE IS GONE, not on the next
       15-frame poll: the hold is a LEVEL the game thread re-applies on every
       in-play draw, so a stale "valid" here would pin the camera — and rebuild
       the fog grid per draw — for up to 15 render frames after the file was
       deleted. Every early exit below drops the level. */
    if (h == INVALID_HANDLE_VALUE) { s_holdValid = 0; return; }
    if (!ReadFile(h, buf, sizeof buf - 1, &n, NULL)) n = 0;
    CloseHandle(h);
    if (n == 0) { s_holdValid = 0; return; }
    buf[n] = 0;

    int x = 0, y = 0;
    if (sscanf(buf, "%d %d", &x, &y) != 2) { s_holdValid = 0; return; }
    /* BOUNDED AT THE SOURCE. The record's validator refuses a hold outside
       +-2^24 world px, and a refused record disables the WHOLE command
       channel for as long as it is reposted — one absurd number in the file
       would silently turn off the camera range, the widened rect and the
       scroll rate. The game thread clamps the point into the
       camera's range anyway, so clamping here loses nothing. */
    if (x < -0x1000000) x = -0x1000000; else if (x > 0x1000000) x = 0x1000000;
    if (y < -0x1000000) y = -0x1000000; else if (y > 0x1000000) y = 0x1000000;
    s_holdX = x; s_holdY = y; s_holdValid = 1;
    /* expose eye wars: if the engine moved the eye away from the hold target,
       say so — read off this frame's packet, which is the eye the last in-play
       draw was made with (the hold is applied before every draw, so a packet
       that disagrees by more than a few pixels means something else is
       driving the camera between our apply and the engine's own writers) */
    if (f->packet && f->packet->in_game) {
        static unsigned warn = 0;
        int cx = f->packet->eye[0], cy = f->packet->eye[1];
        int dx = cx - x, dy = cy - y;
        if ((dx > 8 || dx < -8 || dy > 8 || dy < -8) && ++warn >= 60) {
            warn = 0;
            char b[96];
            _snprintf(b, sizeof b, "eye: WAR engine=(%d,%d) hold=(%d,%d)", cx, cy, x, y);
            ilog(b);
        }
    }
}

/* 1 while tagpu_eye.txt is holding the camera. tagpu_zoom asks before stepping
   the eye for the cursor anchor: a hold means the camera does not move, and
   two sources asserting different positions on alternate draws is a judder
   plus the "eye: WAR" line above. Reads the flag this module already polls
   every 15 frames, so it costs no extra file system call. */
int tagpu_input_eye_held(void) { return s_eyeHold; }

void tagpu_input_cmd(TAGPU_CMD* rec)
{
    rec->hold_on = (s_eyeHold && s_holdValid) ? 1u : 0u;
    rec->hold_x  = s_holdX;
    rec->hold_y  = s_holdY;
}

/* THE DRIVING HALF — GAME THREAD, from the engine's flip.

   This is `tacli keys` and `tacli click`: the token file, the shield's held
   modifiers, the injected pointer. It touches no packet and no engine memory,
   so it can run wherever the family runs — and it has to run from the flip,
   because tagpu_overlay_draw is never reached on renderer=gdi and that lane
   has to be drivable too.

   NOTHING HERE REENTERS THE ENGINE. Every injection leaves by PostMessageA (a
   tagged WM_TAGPU_*) or by SendInput; there is no SendMessage on any path out
   of do_keys, mouse_lock() included. So a token processed inside the flip
   detour is DELIVERED by the engine's own message pump afterwards, not
   dispatched into its wndproc halfway through a present. That is the reason
   this is safe to call from inside an engine call and not merely untested
   there.

   ONE PATH COULD DISPATCH IT EARLIER, AND IT IS OFF ON THIS TARGET. dds_Blt and dds_Lock call util_pull_messages(), which does
   PeekMessageA(PM_REMOVE) + DispatchMessageA, and the flip reaches ddraw
   through them -- after this detour returns, still inside the flip. So a
   WM_TAGPU_MOUSE posted here could be dispatched mid-flip. Its guard is five
   conjuncts and `!IsWine()` is decisive where this runs. Of the rest, the
   strongest is `last_msg_pull_tick + 1000 < timeGetTime()` -- "the app has not
   pumped for a second", which is the whole point of `fix_not_responding` and
   is false almost always, because that field is stamped on essentially every
   message the game pulls (fake_GetMessageA, fake_PeekMessageA, and dd.c's own
   pull) -- not by util_pull_messages, whose own assignment is commented
   out. Unreachable here, then, but
   by a CONFIGURATION fact rather than by an invariant of the injection path,
   which is why it is named instead of left implied. */
void tagpu_input_frame(const TAGPU_FRAME* f)
{
    static unsigned last = 0;

    /* every call: held modifiers must be released on time, not on the next
       15-frame token poll */
    tagpu_shield_frame((HWND)f->hwnd);

    if (f->frame_counter - last >= 15) {
        const TAGPU_FRAME* prev = s_frame;
        last = f->frame_counter;
        /* s_frame is the injection's view of the game's own resolution and it
           points at the CALLER's frame — before_flip builds one on its stack,
           so it is dead the moment this returns. Every reader (si_mouse, and
           the park in inject_click_at) is reached from do_keys below and from
           nowhere else, so bracketing it here is what makes the lifetime a
           fact rather than a habit.

           SAVE AND RESTORE, NOT CLEAR. A clear-to-NULL is
           correct only while this function has one caller and is never
           re-entered, and neither of those is a property this file can state.
           Under re-entry an inner clear would hand the OUTER do_keys a NULL,
           and game_to_abs() dereferences it on its first statement — a null
           dereference, not a quiet no-op. Restoring makes the bracket true by
           construction instead of by caller count, and it costs one word.

           (The 32-deep LIFO in before_flip is about SurfaceCreateNamed
           nesting INSIDE a flip, not about flips nesting, and a nested flip
           cannot re-enter this anyway -- the outer call stamped the 16 ms
           gate's QPC, so an inner one inside that window skips the whole
           block. The save-and-restore does not rely on either.) */
        s_frame = f;
        if (f->hwnd) do_keys((HWND)f->hwnd);
        s_frame = prev;
    }
}

/* THE HOLD HALF — RENDER THREAD, from tagpu_overlay_draw.

   It stays because it is the only part of this module that dereferences
   f->packet (through do_eye), and because its output is read on that thread:
   see the note above s_eyeHold. The counter is the render frame counter. */
void tagpu_input_eye_frame(const TAGPU_FRAME* f)
{
    static unsigned last = 0;

    if (f->frame_counter - last >= 15) {
        last = f->frame_counter;
        s_eyeHold = (GetFileAttributesA("tagpu_eye.txt") != INVALID_FILE_ATTRIBUTES);
        if (!s_eyeHold) s_holdValid = 0;
    }
    if (s_eyeHold) do_eye(f);
}
