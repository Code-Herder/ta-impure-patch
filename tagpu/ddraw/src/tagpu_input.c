/* tagpu_input.c — in-process input injection (session-proof game driving).

   X-level injection (XTEST/XSendEvent) is unreliable: a locked/half-dead
   GNOME session holds a server grab and keys either vanish or land in the
   user's UNLOCK DIALOG. We live inside the process, so we drive the game with
   its own message queue and display state. No X involved; works under any
   lock.

     tagpu_keys.txt   one BATCH: `#<id>` (1 to 16 hex digits) and then
                      whitespace-separated tokens, at most 1023 bytes, taken
                      once (deleted after reading) while no batch is in
                      progress. Its events are posted ONE AT A TIME, each only
                      once the engine has consumed the one before, and the
                      batch ends in `input: batch <id> done: ...` or
                      `input: batch <id> REFUSED: ...` -- the line `tacli keys`
                      waits for. Tokens: a..z 0..9 space return escape plus
                      minus up down left right f1..f12 tab and the rest of
                      token_vk below, `ctrl+`/`shift+`/`alt+` prefixes, the
                      pointer tokens, `down:`/`up:`, `keydown:`/`keyup:`,
                      `char:`, `wheel:` and `mouselock`.

     tagpu_eye.txt    "X Y" (world px). While the file exists the camera eye
                      (main+0x1431F/0x14323) is held there: the point rides
                      the frame packet's command record and the GAME THREAD
                      writes the eye and its scroll target, clamped into the
                      camera's range, at the top of every in-play draw
                      (tagpu_zoom_apply). Delete the file to release. The eye
                      is display state, not sim state — read-only-over-sim
                      holds, and this module writes no engine memory at all.

   The key file is looked for at every trigger tick on the game thread, the eye
   file every 15 render frames on the render thread.

   Keys and clicks travel as tagged WM_TAGPU_*
   messages that the shield translates in the wndproc, so they survive the
   hardware-input filter and drive a virtual key/cursor state the game's polls
   read (inc/tagpu_shield.h). That is what makes ctrl/shift combos land. */

#include <windows.h>
#include <stdio.h>
#include "tagpu.h"
#include "tagpu_input.h"
#include "tagpu_shield.h"
#include "tagpu_gui.h"     /* tagpu_gui_input_rings: the engine's own ring indices */
#include "tagpu_packet.h"  /* the command record the hold rides on */
#include "tagpu_log.h"
#include "mouse.h"        /* the fork's own mouse-lock (wndproc drops mouse
                             messages while unlocked — the "clicks never work
                             under a locked session" root cause) */
extern BOOL g_mouse_locked;

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
    /* the game's own pause: in a network game it pauses every peer, where Tab only
       opens the options panel */
    if (!lstrcmpiA(t, "pause"))  return VK_PAUSE;
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

/* ---- the batch: one event in flight ----------------------------------------------------

   THE INVARIANT: at most one injected event is in flight, and the next is posted only once
   the engine has consumed it -- which is read, never timed. An event is consumed when the
   shield has delivered its tagged message (its sequence number comes back through
   tagpu_input_delivered) and, at a later trigger tick, both of the engine's input rings read
   empty (tagpu_gui_input_rings). The window procedure pushes into those rings while it
   handles the message; IdleTick pops them, and pops before it hands the entry to its handler
   and before the frame it draws and flips (the GUI pump 0x499992 and its pop 0x4AB654, the
   in-game pop 0x4999C4, the handler 0x499A1C, the frame 0x4995B8 -> 0x496790, DISASSEMBLED;
   that no flip falls between a pop and its handler, here or in the modal pump 0x49F9C4, is
   [INFERRED]), so an empty ring at a flip after the delivery means the event's handler has run. A move or a
   wheel notch pushes nothing, so for them delivery and a later tick are consumption. With the
   shield armed only our messages reach the push sites; disarmed, a player's entries are ahead
   of ours in the same FIFO, which the emptiness test waits out as well.

   So nothing can overflow a ring (a full push 0x4C2E30 drops silently, `0x4C2E4A..0x4C2E50`),
   no release overtakes the press whose handler polls it, and a modifier is down for exactly the
   events posted between its down and its up. No clock decides anything: the 2 s below only
   delays a report.

   THE RESIDUAL: the in-game setup's flush 0x4C2BB0 (its one call, 0x426636) empties the mouse
   ring without handling it, so an event in flight across a level's start would be acknowledged
   unhandled. The harness cannot reach it: `tacli keys` returns only once its last event is
   consumed, and a Start click is popped before the setup runs [INFERRED].

   ALL OF IT IS THE GAME THREAD: the posting at the engine's flip, the acknowledgement from the
   wndproc of the game's window, which that thread pumps. tagpu_input_delivered checks it. */

enum { EV_KEY, EV_CHAR, EV_MOUSE, EV_UNTRACKED, EV_LOCK };
/* EV_UNTRACKED's operations: SendInput, for the pointer tokens that drive the real cursor
   while the shield is disarmed. Nothing can tag them, so each counts as consumed once sent. */
enum { SI_MOVE, SI_LCLICK, SI_RCLICK, SI_MOVEREL, SI_LBUTTON, SI_RBUTTON };

typedef struct {
    unsigned char  kind, up;
    unsigned short code;            /* the vk, the character, TAGPU_M_* or SI_*            */
    short          x, y;
    unsigned short tok;             /* the token it came from, for the `waiting` line       */
} InEvent;

#define Q_CAP      4096
#define BATCH_MAX  1023                     /* bytes of one batch file                   */
#define TOK_CAP    (BATCH_MAX / 2 + 1)      /* a character and a separator each, at least */
#define WAIT_MS    2000                     /* the `waiting` line's delay                */

static InEvent        s_q[Q_CAP];
static unsigned       s_qn, s_qi;           /* the batch's events; the next one to post   */
static int            s_active;             /* a batch is taken and not yet done         */
static char           s_text[BATCH_MAX + 1];/* the batch file, each token NUL-ended in it */
static unsigned short s_tok[TOK_CAP];       /* the tokens' offsets in s_text             */
static unsigned       s_ntok;
static char           s_id[17];
static unsigned       s_seq;                /* the last sequence number posted, 1..0xFFFF */
static int            s_inflight, s_delivered;
static unsigned       s_out[3], s_untracked;/* TAGPU_DELIVER_* and EV_UNTRACKED counts    */
static DWORD          s_since, s_tid;
static int            s_warned;
static const TAGPU_FRAME* s_frame;          /* the caller's frame, inside tagpu_input_frame only */

static int push(int kind, int up, int code, int x, int y)
{
    InEvent* e;
    if (s_qn >= Q_CAP) return 0;
    e = &s_q[s_qn++];
    e->kind = (unsigned char)kind; e->up = (unsigned char)up; e->code = (unsigned short)code;
    e->x = (short)x; e->y = (short)y; e->tok = (unsigned short)(s_ntok - 1);
    return 1;
}

static const char BAD[]  = "not a token";
static const char FULL[] = "more than 4096 events";
#define PUSH(k, u, c, x, y) do { if (!push((k), (u), (c), (x), (y))) return FULL; } while (0)

static int xy_ok(int x, int y)
{
    return x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767;
}

/* injected click: move, press, release, then park the pointer at the centre of the view -- a
   click near a screen edge otherwise leaves the engine's memory mouse there and it
   EDGE-SCROLLS forever. The park is in the engine's own coordinates whichever space the click
   was in: it is about the engine's memory mouse, not the window. */
static const char* push_click(int gx, int gy, int right, int dev)
{
    int d = dev ? TAGPU_M_DEV : 0;
    PUSH(EV_MOUSE, 0, TAGPU_M_MOVE | d, gx, gy);
    PUSH(EV_MOUSE, 0, (right ? TAGPU_M_RDOWN : TAGPU_M_LDOWN) | d, gx, gy);
    PUSH(EV_MOUSE, 0, (right ? TAGPU_M_RUP : TAGPU_M_LUP) | d, gx, gy);
    if (s_frame && s_frame->game_width > 0 && s_frame->game_height > 0)
        PUSH(EV_MOUSE, 0, TAGPU_M_MOVE, (int)s_frame->game_width / 2,
             (int)s_frame->game_height / 2);
    return NULL;
}

/* One token's events, appended; NULL, or why the whole batch is refused. */
static const char* parse_token(const char* p)
{
    int gx, gy, vk, code, i, nmods = 0, mods[4], on = tagpu_shield_on();
    char tok[64];
    const char* base = p;

    /* stackable modifier prefixes: ctrl+d, ctrl+shift+2, alt+f4 -- each modifier down, the
       key down and up, the modifiers up in reverse */
    while (nmods < 4) {
        if (mod_prefix(base, "ctrl", 4))       { mods[nmods++] = VK_CONTROL; base += 5; }
        else if (mod_prefix(base, "shift", 5)) { mods[nmods++] = VK_SHIFT;   base += 6; }
        else if (mod_prefix(base, "alt", 3))   { mods[nmods++] = VK_MENU;    base += 4; }
        else break;
    }
    if (nmods) {
        if (!(vk = token_vk(base))) return BAD;
        for (i = 0; i < nmods; i++) PUSH(EV_KEY, 0, mods[i], 0, 0);
        PUSH(EV_KEY, 0, vk, 0, 0);
        PUSH(EV_KEY, 1, vk, 0, 0);
        for (i = nmods; i-- > 0; ) PUSH(EV_KEY, 1, mods[i], 0, 0);
        return NULL;
    }
    if (sscanf(p, "mouse:%d,%d", &gx, &gy) == 2) {
        if (!xy_ok(gx, gy)) return BAD;
        if (on) PUSH(EV_MOUSE, 0, TAGPU_M_MOVE, gx, gy);
        else    PUSH(EV_UNTRACKED, 0, SI_MOVE, gx, gy);
        return NULL;
    }
    if (sscanf(p, "click:%d,%d", &gx, &gy) == 2 || sscanf(p, "rclick:%d,%d", &gx, &gy) == 2) {
        int right = (p[0] == 'r' || p[0] == 'R');
        if (!xy_ok(gx, gy)) return BAD;
        if (on) return push_click(gx, gy, right, 0);
        PUSH(EV_UNTRACKED, 0, right ? SI_RCLICK : SI_LCLICK, gx, gy);
        return NULL;
    }
    if (sscanf(p, "mouserel:%d,%d", &gx, &gy) == 2) {
        if (!xy_ok(gx, gy)) return BAD;
        if (on) PUSH(EV_MOUSE, 0, TAGPU_M_MOVEREL, gx, gy);
        else    PUSH(EV_UNTRACKED, 0, SI_MOVEREL, gx, gy);
        return NULL;
    }
    if (sscanf(p, "pclick:%d,%d", &gx, &gy) == 2)
        return xy_ok(gx, gy) ? push_click(gx, gy, 0, 0) : BAD;
    if (sscanf(p, "prclick:%d,%d", &gx, &gy) == 2)
        return xy_ok(gx, gy) ? push_click(gx, gy, 1, 0) : BAD;
    /* DEVICE-SPACE click and move: x,y are client-area pixels and are converted by the same
       `mouse_client_to_game` a hardware click goes through. Every other token here speaks the
       engine's coordinates and so proves nothing about the pointer path; these two are the
       only ones that test it, which is what phase 2's kill rule needs (gui-renderer.md 13.1,
       13.9). */
    if (sscanf(p, "dclick:%d,%d", &gx, &gy) == 2)
        return xy_ok(gx, gy) ? push_click(gx, gy, 0, 1) : BAD;
    if (sscanf(p, "drclick:%d,%d", &gx, &gy) == 2)
        return xy_ok(gx, gy) ? push_click(gx, gy, 1, 1) : BAD;
    if (sscanf(p, "dmove:%d,%d", &gx, &gy) == 2) {
        if (!xy_ok(gx, gy)) return BAD;
        PUSH(EV_MOUSE, 0, TAGPU_M_MOVE | TAGPU_M_DEV, gx, gy);
        return NULL;
    }
    /* injected move with no click and no parking: what a hover is. The plain "mouse:" token
       falls back to SendInput when the shield is down, which would drag the human's real
       pointer. */
    if (sscanf(p, "pmove:%d,%d", &gx, &gy) == 2) {
        if (!xy_ok(gx, gy)) return BAD;
        PUSH(EV_MOUSE, 0, TAGPU_M_MOVE, gx, gy);
        return NULL;
    }
    /* `wheel:+3` / `wheel:-3` -- N notches at the pointer, which is the zoom control
       (tagpu_zoom.h). It is posted where the pointer is and NOT where a token says, because
       the wheel is only live over the world viewport: aim it with `pmove:` first, exactly as a
       player aims it by moving the mouse. At most 64 notches a token (tacli splits a bigger
       ask). */
    if (sscanf(p, "wheel:%d", &gx) == 1) {
        int n = gx < 0 ? -gx : gx;
        if (gx < -64 || gx > 64) return BAD;
        code = gx < 0 ? TAGPU_M_WHEELDN : TAGPU_M_WHEELUP;
        while (n-- > 0) PUSH(EV_MOUSE, 0, code, TAGPU_M_HERE, TAGPU_M_HERE);
        return NULL;
    }
    if (sscanf(p, "char:%c", tok) == 1) {
        PUSH(EV_CHAR, 0, (unsigned char)tok[0], 0, 0);
        return NULL;
    }
    if (!lstrcmpiA(p, "mouselock")) {
        PUSH(EV_LOCK, 0, 0, 0, 0);
        return NULL;
    }
    if (!lstrcmpiA(p, "click") || !lstrcmpiA(p, "rclick")) {
        int right = (p[0] == 'r' || p[0] == 'R');
        if (!on) {
            PUSH(EV_UNTRACKED, 0, right ? SI_RBUTTON : SI_LBUTTON, 0, 0);
            return NULL;
        }
        vk = right ? VK_RBUTTON : VK_LBUTTON;
        PUSH(EV_MOUSE, 0, button_code(vk, 0), TAGPU_M_HERE, TAGPU_M_HERE);
        PUSH(EV_MOUSE, 0, button_code(vk, 1), TAGPU_M_HERE, TAGPU_M_HERE);
        return NULL;
    }
    /* explicit hold/release: the only way to keep a key or a mouse button down across events
       (drag-select, held modifiers) */
    if (sscanf(p, "down:%63s", tok) == 1 || sscanf(p, "up:%63s", tok) == 1) {
        int up = (p[0] == 'u' || p[0] == 'U');
        if (!(vk = token_vk(tok))) return BAD;
        if ((code = button_code(vk, up)) >= 0) PUSH(EV_MOUSE, 0, code, TAGPU_M_HERE, TAGPU_M_HERE);
        else                                   PUSH(EV_KEY, up, vk, 0, 0);
        return NULL;
    }
    if (sscanf(p, "keydown:%d", &gx) == 1 || sscanf(p, "keyup:%d", &gx) == 1) {
        if (gx < 1 || gx > 255) return BAD;
        PUSH(EV_KEY, p[3] == 'u' || p[3] == 'U', gx, 0, 0);
        return NULL;
    }
    if (!(vk = token_vk(p))) return BAD;
    PUSH(EV_KEY, 0, vk, 0, 0);
    PUSH(EV_KEY, 1, vk, 0, 0);
    return NULL;
}

static void finish(void)
{
    s_active = s_inflight = 0;
    s_qn = s_qi = s_ntok = 0;
}

static void refuse(const char* why)
{
    tagpu_logf("input: batch %s REFUSED: %s", s_id[0] ? s_id : "-", why);
    finish();
}

/* The batch file, read whole and deleted, then parsed whole: an unknown token, a bad id or too
   many events refuses the batch before anything is posted. tacli puts the file in place by a
   hard link of a complete one (or a move, remote), so it is never read half-written, and it
   never places one while another stands. */
static void take(void)
{
    char line[BATCH_MAX + 64];
    DWORD n = 0, size;
    unsigned i, lo;
    char* p;
    HANDLE h = CreateFileA("tagpu_keys.txt", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(h, NULL);
    if (!ReadFile(h, s_text, BATCH_MAX, &n, NULL)) n = 0;
    CloseHandle(h);
    DeleteFileA("tagpu_keys.txt");
    s_text[n] = 0;
    finish();
    s_active = 1;
    s_id[0] = 0;
    memset(s_out, 0, sizeof s_out);
    s_untracked = 0;

    p = s_text;
    while (*p && (unsigned char)*p <= ' ') p++;
    if (*p == '#') {
        for (i = 0; i < 16 && ((p[1 + i] >= '0' && p[1 + i] <= '9') ||
                               (p[1 + i] >= 'a' && p[1 + i] <= 'f') ||
                               (p[1 + i] >= 'A' && p[1 + i] <= 'F')); i++)
            s_id[i] = p[1 + i];
        s_id[i] = 0;
        if (!i || (unsigned char)p[1 + i] > ' ') s_id[0] = 0;
        else p += 1 + i;
    }
    if (!s_id[0]) { refuse("no batch id"); return; }
    if (size > BATCH_MAX) { refuse("longer than 1023 bytes"); return; }

    /* `line` holds it all: the tokens and their separators are at most the file's BATCH_MAX
       bytes, and the prefix is under 64 */
    lo = (unsigned)_snprintf(line, sizeof line, "input: batch %s taken:", s_id);
    while (*p) {
        const char* why;
        char* q;
        while (*p && (unsigned char)*p <= ' ') p++;
        if (!*p) break;
        for (q = p; *q && (unsigned char)*q > ' '; q++) {}
        if (*q) *q++ = 0;
        s_tok[s_ntok++] = (unsigned short)(p - s_text);
        if ((why = parse_token(p)) != NULL) {
            char b[160];
            _snprintf(b, sizeof b, "%s: %.64s", why, p);
            b[sizeof b - 1] = 0;
            refuse(b);
            return;
        }
        lo += (unsigned)_snprintf(line + lo, sizeof line - lo, " %s", p);
        p = q;
    }
    tagpu_log(line);
    s_since = GetTickCount();
    s_warned = 0;
}

static void untracked(const InEvent* e)
{
    INPUT in;
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    switch (e->code) {
    case SI_MOVE:   si_mouse(s_frame, e->x, e->y, 0); break;
    case SI_LCLICK: si_mouse(s_frame, e->x, e->y, 1); break;
    case SI_RCLICK: si_mouse(s_frame, e->x, e->y, 2); break;
    case SI_MOVEREL:
        in.mi.dx = e->x; in.mi.dy = e->y;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof in);
        break;
    default:
        in.mi.dwFlags = e->code == SI_RBUTTON ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &in, sizeof in);
        in.mi.dwFlags = e->code == SI_RBUTTON ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof in);
        break;
    }
}

/* Once per trigger tick: the event in flight checked for consumption, then the next posted --
   one event a tick at most, and only onto empty rings. */
static void advance(HWND w)
{
    int mp = -1, kp = -1, rings, ok = 1;
    const InEvent* e;
    if (!s_active) return;
    rings = tagpu_gui_input_rings(&mp, &kp);
    if (!rings) mp = kp = -1;
    if (s_inflight && s_delivered && rings && !mp && !kp) {
        s_inflight = 0;
        s_since = GetTickCount();
        s_warned = 0;
    }
    if (!s_inflight && s_qi == s_qn) {
        tagpu_logf("input: batch %s done: %u events; engine %u, menu %u, dropped %u, untracked %u",
                   s_id, s_qn, s_out[TAGPU_DELIVER_ENGINE], s_out[TAGPU_DELIVER_MENU],
                   s_out[TAGPU_DELIVER_DROPPED], s_untracked);
        finish();
        return;
    }
    if (s_inflight || !rings || mp || kp) {
        if (!s_warned && GetTickCount() - s_since >= WAIT_MS) {
            unsigned at = s_inflight ? s_qi - 1 : s_qi;
            s_warned = 1;
            tagpu_logf("input: batch %s waiting: event %u/%u %s delivered=%d mouse=%d key=%d",
                       s_id, at + 1, s_qn, s_text + s_tok[s_q[at].tok],
                       s_inflight && s_delivered, mp, kp);
        }
        return;
    }
    e = &s_q[s_qi];
    if (++s_seq > 0xFFFFu) s_seq = 1;
    switch (e->kind) {
    case EV_KEY:   ok = tagpu_shield_key(w, e->code, e->up, s_seq); break;
    case EV_CHAR:  ok = tagpu_shield_char(w, (char)e->code, s_seq); break;
    case EV_MOUSE: ok = tagpu_shield_mouse(w, e->code, e->x, e->y, s_seq); break;
    case EV_UNTRACKED: untracked(e); break;
    case EV_LOCK:
        mouse_lock();
        ilog(g_mouse_locked ? "input: mouse LOCKED" : "input: mouse lock FAILED");
        break;
    }
    if (!ok) {
        char b[160];
        _snprintf(b, sizeof b, "event %u/%u %.64s could not be posted (error %lu)",
                  s_qi + 1, s_qn, s_text + s_tok[e->tok], (unsigned long)GetLastError());
        b[sizeof b - 1] = 0;
        refuse(b);
        return;
    }
    s_qi++;
    s_since = GetTickCount();
    s_warned = 0;
    if (e->kind == EV_UNTRACKED || e->kind == EV_LOCK) {
        s_untracked++;              /* consumed at posting: nothing can say otherwise */
        return;
    }
    s_inflight = 1;
    s_delivered = 0;
}

void tagpu_input_delivered(unsigned seq, int outcome)
{
    if (GetCurrentThreadId() != s_tid || !s_inflight || s_delivered || seq != s_seq) return;
    s_delivered = 1;
    if (outcome >= 0 && outcome < (int)(sizeof s_out / sizeof s_out[0])) s_out[outcome]++;
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
   the other side, and on the GDI backend it would deliver nothing either way:
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
   every 15 render frames, so it costs no extra file system call. */
int tagpu_input_eye_held(void) { return s_eyeHold; }

void tagpu_input_cmd(TAGPU_CMD* rec)
{
    rec->hold_on = (s_eyeHold && s_holdValid) ? 1u : 0u;
    rec->hold_x  = s_holdX;
    rec->hold_y  = s_holdY;
}

/* THE DRIVING HALF — GAME THREAD, from the engine's flip.

   This is `tacli keys` and `tacli click`: the batch file, the shield's tagged
   messages, the injected pointer. It touches no packet and writes no engine
   memory -- the rings it reads are tagpu_gui_hook.c's to read -- so it can run
   wherever the family runs, and it has to run from the flip, because
   tagpu_overlay_draw is never reached on the GDI backend and that lane has to
   be drivable too.

   NOTHING HERE REENTERS THE ENGINE. Every injection leaves by PostMessageA (a
   tagged WM_TAGPU_*) or by SendInput; there is no SendMessage on any path out
   of advance(), mouse_lock() included. So an event posted inside the flip
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
   which is why it is named instead of left implied. Were it reached, the
   consumption test would still hold: the acknowledgement would come from that
   dispatch, and the rings are read at the next trigger tick. */
void tagpu_input_frame(const TAGPU_FRAME* f)
{
    const TAGPU_FRAME* prev = s_frame;
    HWND w = (HWND)f->hwnd;

    tagpu_shield_frame();
    if (!w) return;
    s_tid = GetCurrentThreadId();

    /* s_frame is the injection's view of the game's own resolution and it
       points at the CALLER's frame — before_flip builds one on its stack,
       so it is dead the moment this returns. Its readers (the park in
       push_click, si_mouse through untracked) are reached from take and
       advance below and from nowhere else, so bracketing it here is what makes
       the lifetime a fact rather than a habit.

       SAVE AND RESTORE, NOT CLEAR. A clear-to-NULL is
       correct only while this function has one caller and is never
       re-entered, and neither of those is a property this file can state.
       Under re-entry an inner clear would hand the OUTER call a NULL,
       and game_to_abs() dereferences it on its first statement — a null
       dereference, not a quiet no-op. Restoring makes the bracket true by
       construction instead of by caller count, and it costs one word. */
    s_frame = f;
    advance(w);
    if (!s_active) {
        take();
        advance(w);
    }
    s_frame = prev;
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
