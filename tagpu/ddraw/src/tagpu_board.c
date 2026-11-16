/* tagpu_board.c -- the drawing board (tagpu_board.h; research/notes/modules/board.md).

   TWO HALVES OF ONE GESTURE, both on the GAME thread. The window procedure decides whether a
   button belongs to the board -- the draw key held, the pointer over the world view, no menu --
   and takes the message whole, so the engine never sees the press or its release and opens no
   selection box and gives no order. Where the press lands in the world is not known there: the
   world point is the in-play handler's (0x498DA0 writes main+0x2CAA for the record, under the
   zoom's transform, tagpu_vpwide.c). So a taken button is queued and resolved at the next record
   the handler passes the right-press branch with (tagpu_route.c), which is the pointer's move
   record: IdleTick hands the handler the mouse object's last move every frame, so that record's
   point is the point the press was made at -- a press does not move the pointer. A move during a
   drag reaches the board the same way, with its own point.

   THE STORE is tagpu_boardstore.h's, and every change to it is an operation (tagpu_boardwire.h):
   the player's own, applied here at once and queued for the contribution channel, which the
   recorder sends (tagpu_rec_mp_board_frame); and every other peer's, which the recorder hands to
   tagpu_board_apply in delivery order. A mark's lifetime is counted in game ticks, so a paused
   game keeps its marks. Who sees whose marks, and whose an operation may change, is
   tagpu_share_board_seen's: the player's own, their mutual allies', and for a watcher every
   seat's. Nothing here is read by the simulation, and nothing here writes engine memory. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "tagpu_board.h"
#include "tagpu_route.h"
#include "tagpu_engine.h"
#include "tagpu_zoom.h"
#include "tagpu_replay.h"
#include "tagpu_packet_pub.h"
#include "tagpu_log.h"
#include "tagpu_shield.h"
#include "tagpu_boardstore.h"
#include "tagpu_sharetick.h"
#include "tagpu_recorder.h"
#include "dd.h"
#include "render_vk.h"

#define M_POINT    0x2CAA      /* the world point, 16.16 x, altitude, z                        */
#define M_REGION   0x2CC6      /* bit 1 the pointer is in the viewport, bit 3 a box is open    */
#define M_GUICOL   0x0DCB      /* the GUI colour byte array                                    */
#define M_CONTROLLED 0x2A42   /* u8 the controlled player                                     */
#define M_FIELD    0x57D       /* i32 the active textfield's index, -1 none                    */
#define M_SCREENS  0x37EBE     /* u16: 0x865 the options stack, chat, share and the rest       */
#define M_SCREENS2 0x2BEE      /* u8: 0xE0                                                      */
#define GUI_BLACK  0x00

#define STEP_PX    6.0         /* screen px a line moves before it adds a segment: a corner of
                                  the stroke is kept however fast the pointer moved             */
#define DOUBLE_PX  4.0         /* screen px two presses of a double click may be apart          */
#define LIFE_TICKS 1800u       /* a mark's lifetime, 60 s of game time at 30 ticks a second    */
#define PENDING    8
#define OUTBOX     16384u      /* the operations not yet sent, bytes, the body's head included  */
#define SAYS       8           /* the chat lines a frame owes                                   */
#define SAY_TICKS  30u         /* one chat line a seat at most per this many ticks              */
#define CHAT_LINE  0x00463CA0u /* stdcall (text, flags, u16, row): a chat line, 0x40 bytes kept */

typedef int (__stdcall* GroundHeight)(const int*);

enum { G_NONE, G_LINE, G_MOVE, G_ERASE };
enum { E_LDOWN, E_LUP, E_RDOWN, E_RUP, E_MDOWN };

static int      s_armed;
static unsigned s_vk = 0xDC;           /* VK_OEM_5, the key that types `\` on a US layout */
static int      s_held;                /* the draw key is down                               */
static int      s_taken[3];            /* the board took this button's press: its release too */

static unsigned char s_ev[PENDING];
static DWORD         s_evAt[PENDING];
static unsigned      s_nev;

static TagpuBoardStore s_store;
static uint32_t s_seq;                 /* the player's last marker sequence, this level      */

static unsigned char s_out[OUTBOX];    /* a body: the version, the seat, then operations     */
static unsigned      s_nout;

static struct { unsigned seat; char line[0x40]; } s_say[SAYS];
static unsigned s_nsay;
static uint32_t s_saidAt[TAGPU_BW_SEATS];
static unsigned s_said;                /* a bit per seat that has a line in this level       */

static struct {
    int      kind;
    uint32_t id;               /* G_MOVE: the marker                                           */
    int32_t  last[3];          /* G_LINE: the last end; G_MOVE: the last point sent            */
    int      haveLast;
    DWORD    at;               /* the last step's time                                         */
} s_g;

static struct {
    int      open;
    uint32_t id;               /* 0: a new marker at `at`                                      */
    int32_t  at[3];
    unsigned len;
    char     text[TAGPU_PK_BD_TEXT + 1];
} s_box;

typedef struct { DWORD at; int32_t p[3]; int valid; } Press;
static Press s_lastL, s_lastR;      /* the last press of each button, for a double click   */
static int32_t s_pen[3];
static int     s_penOn;

static unsigned char s_logoCol[16];
static unsigned      s_logoHave;       /* a bit per logo whose colour is cached              */

static unsigned s_cMarks, s_cSegs, s_cErase, s_cDrop, s_cFull, s_cIn, s_cBad;

static void say_lines(void);

/* ------------------------------------------------------------------ gates ---- */

static const char* main_block(void) { return *(const char* const*)TA_MAIN_PP; }

static int menu_over(const char* ta)
{
    return (*(const uint16_t*)(ta + M_SCREENS) & 0x865u) ||
           (*(const unsigned char*)(ta + M_SCREENS2) & 0xE0u);
}

/* The board acts: armed, the Vulkan lane in force, the world view bounded and the controlled
   player not a puppet seat (tagpu_route_world). */
static int live(TagpuWorld* w)
{
    return s_armed && g_ddraw.renderer == vk_render_main && tagpu_route_world(w);
}

/* the modal screens and the chat line, which own the keyboard */
static int keyboard_taken(const char* ta)
{
    return menu_over(ta) || *(const int*)(ta + M_FIELD) != -1;
}

/* the pointer is on the world view, as the handler's last record found it, with no selection
   box open and no menu over it */
static int pointer_on_world(const char* ta)
{
    unsigned r = *(const unsigned char*)(ta + M_REGION);
    return (r & 0x02u) && !(r & 0x08u) && !menu_over(ta);
}

static double zoom(void)
{
    double z = (double)tagpu_zoom_level();
    return z >= TAGPU_ZOOM_FLOOR_LO && z <= 16.0 ? z : 1.0;
}

/* THE HOLD IS THE KEY'S STATE, NOT A MESSAGE'S. A release can reach something else first: as
   WM_SYSKEYUP while Alt is down, or to the wiki, which takes every key-up while it is open. So the
   hold is asked of the key itself wherever it is used -- the shield's virtual state while the
   shield is armed (the injected path sets it before the board is asked), the hardware's
   otherwise -- and a hold whose key is up is over. */
static int key_down(unsigned vk)
{
    SHORT st = 0;
    if (tagpu_shield_key_state((int)vk, FALSE, &st)) return st < 0;
    return GetAsyncKeyState((int)vk) < 0;
}

static int holding(void)
{
    if (s_held && !key_down(s_vk)) s_held = 0;
    return s_held;
}

/* -------------------------------------------------------------------- store ---- */

static uint32_t game_tick(const char* ta) { return *(const uint32_t*)(ta + OFF_GAMETIME); }

/* the seats whose marks this player sees and whose marks their operations may change */
static unsigned seen(void) { return tagpu_share_board_seen(); }

/* glyphs of the engine font's range, at most TAGPU_PK_BD_TEXT, NUL-terminated */
static unsigned text_copy(char* dst, const char* src, unsigned len)
{
    unsigned i, n = 0;
    for (i = 0; i < len && n < TAGPU_PK_BD_TEXT; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= TAGPU_PK_GLYPH_LO && c <= TAGPU_PK_GLYPH_HI) dst[n++] = (char)c;
    }
    dst[n] = 0;
    return n;
}

/* One operation of the player's, under the player's rights now (`may`, which every peer applies it
   under, cut to what that peer sees): applied to the store and queued for the channel when it
   changed something here. WHAT IS DRAWN IS WHAT IS SENT: an operation the outbox has no room for is
   not applied either, so the player never sees a mark the channel will not carry -- which happens
   only while the channel cannot drain, a pause with something else ahead of the marks
   (tagpu_rec_mp_board_frame), and then the pen draws nothing until it can. */
static int local(const char* ta, unsigned seat, TagpuBoardOp* op)
{
    unsigned char one[TAGPU_BW_OP_MAX];
    unsigned n, at;
    int did;
    op->may = seen() & TAGPU_BW_MAY_ALL;
    if (s_nout && s_out[1] != seat) s_nout = 0;           /* another seat's queue is not ours */
    at = s_nout ? s_nout : TAGPU_BW_HEAD;
    if (!(n = tagpu_boardwire_put(one, sizeof one, op, seat)) || at + n > OUTBOX) {
        s_cFull++;
        return 0;
    }
    did = tagpu_boardstore_apply(&s_store, seat, op, game_tick(ta), op->may);
    if (did == TAGPU_BS_NONE) return 0;
    if (op->op == TAGPU_BW_POINT) s_cMarks++;
    else if (op->op == TAGPU_BW_LINE) s_cSegs++;
    else if (op->op == TAGPU_BW_ERASE) s_cErase++;
    s_out[0] = TAGPU_BW_VERSION;
    s_out[1] = (unsigned char)seat;
    memcpy(s_out + at, one, n);
    s_nout = at + n;
    return did;
}

static void op_point(const char* ta, unsigned seat, const int32_t p[3], const char* text, unsigned len)
{
    TagpuBoardOp op;
    memset(&op, 0, sizeof op);
    s_seq = (s_seq + 1u) & 0xFFFFFFu;
    if (!s_seq) s_seq = 1;
    op.op = TAGPU_BW_POINT;
    op.id = (uint32_t)seat << 24 | s_seq;
    memcpy(op.a, p, sizeof op.a);
    op.len = text_copy(op.text, text, len);
    local(ta, seat, &op);
}

static void op_id(const char* ta, unsigned seat, unsigned code, uint32_t id, const int32_t p[3],
                  const char* text, unsigned len)
{
    TagpuBoardOp op;
    memset(&op, 0, sizeof op);
    op.op = code;
    op.id = id;
    if (p) memcpy(op.a, p, sizeof op.a);
    if (text) op.len = text_copy(op.text, text, len);
    local(ta, seat, &op);
}

static void op_line(const char* ta, unsigned seat, const int32_t a[3], const int32_t b[3])
{
    TagpuBoardOp op;
    memset(&op, 0, sizeof op);
    op.op = TAGPU_BW_LINE;
    memcpy(op.a, a, sizeof op.a);
    memcpy(op.b, b, sizeof op.b);
    local(ta, seat, &op);
}

static void op_erase(const char* ta, unsigned seat, const int32_t p[3])
{
    TagpuBoardOp op;
    memset(&op, 0, sizeof op);
    op.op = TAGPU_BW_ERASE;
    memcpy(op.a, p, sizeof op.a);
    local(ta, seat, &op);
}

/* The marker under the pointer at `p` among those this player sees: its pin, from the ground point
   to 30 screen px above it and 9 either side, the nearest the viewer (the greatest z) when pins
   overlap. */
static TagpuBoardMark* mark_at(const int32_t p[3])
{
    const double z = zoom();
    const double py = (double)p[2] - (double)(p[1] >> 1);
    const unsigned see = seen();
    TagpuBoardMark* best = NULL;
    unsigned i;
    for (i = 0; i < TAGPU_BS_MARKS; i++) {
        TagpuBoardMark* m = &s_store.mark[i];
        double dx, dy;
        if (!m->id || !(see >> m->owner & 1u)) continue;
        dx = ((double)p[0] - m->x) * z;
        dy = (py - ((double)m->z - (double)(m->h >> 1))) * z;
        if (dx < -9.0 || dx > 9.0 || dy < -30.0 || dy > 4.0) continue;
        if (!best || m->z > best->z) best = m;
    }
    return best;
}

/* ----------------------------------------------------------------- gesture ---- */

static void box_open(const TagpuBoardMark* m, const int32_t p[3])
{
    s_box.open = 1;
    if (m) {
        s_box.id = m->id;
        s_box.at[0] = m->x; s_box.at[1] = m->h; s_box.at[2] = m->z;
        s_box.len = text_copy(s_box.text, m->text, m->len);
    } else {
        s_box.id = 0;
        memcpy(s_box.at, p, sizeof s_box.at);
        s_box.len = 0; s_box.text[0] = 0;
    }
}

static void box_commit(void)
{
    const char* ta = main_block();
    TagpuWorld w;
    if (!s_box.open) return;
    s_box.open = 0;
    if (!ta || !live(&w)) return;
    if (s_box.id) op_id(ta, (unsigned)w.player, TAGPU_BW_EDIT, s_box.id, NULL, s_box.text, s_box.len);
    else op_point(ta, (unsigned)w.player, s_box.at, s_box.text, s_box.len);
}

static void gesture_end(void) { s_g.kind = G_NONE; s_g.haveLast = 0; }

static int is_double(const int32_t p[3], DWORD at, int right)
{
    const double z = zoom();
    const Press* l = right ? &s_lastR : &s_lastL;
    double dx, dy;
    if (!l->valid || (DWORD)(at - l->at) > GetDoubleClickTime()) return 0;
    dx = ((double)p[0] - l->p[0]) * z;
    dy = ((double)p[2] - l->p[2]) * z;
    return dx * dx + dy * dy <= DOUBLE_PX * DOUBLE_PX;
}

static void remember(int right, const int32_t p[3], DWORD at, int valid)
{
    Press* l = right ? &s_lastR : &s_lastL;
    l->at = at;
    memcpy(l->p, p, sizeof l->p);
    l->valid = valid;
}

static void press(const char* ta, int ev, DWORD at, unsigned seat, const int32_t p[3], int on)
{
    TagpuBoardMark* m;
    switch (ev) {
    case E_MDOWN:
        if (!mark_at(p)) op_point(ta, seat, p, "", 0);
        return;
    case E_LDOWN:
        m = mark_at(p);
        if (is_double(p, at, 0)) {
            remember(0, p, at, 0);
            gesture_end();
            box_open(m, p);
            return;
        }
        remember(0, p, at, 1);
        if (m) {
            s_g.kind = G_MOVE; s_g.id = m->id; s_g.at = at;
            return;
        }
        s_g.kind = G_LINE; s_g.at = at;
        memcpy(s_g.last, p, sizeof s_g.last); s_g.haveLast = 1;
        return;
    case E_RDOWN:
        m = mark_at(p);
        if (m && is_double(p, at, 1)) {
            remember(1, p, at, 0);
            op_id(ta, seat, TAGPU_BW_DELETE, m->id, NULL, NULL, 0);
            gesture_end();
            return;
        }
        remember(1, p, at, 1);
        s_g.kind = G_ERASE; s_g.at = at;
        memcpy(s_g.last, p, sizeof s_g.last); s_g.haveLast = 1;
        /* a press on a pin erases nothing where it stands, so a second press can make it the
           double click that deletes that marker alone; the drag erases from its first step */
        if (!m) op_erase(ta, seat, p);
        return;
    case E_LUP:
        /* the stretch since the last step ends where the button came up, on the world view: over
           the minimap the handler's point is the minimap's, across the map */
        if (s_g.kind == G_LINE && s_g.haveLast && on &&
            (s_g.last[0] != p[0] || s_g.last[2] != p[2]))
            op_line(ta, seat, s_g.last, p);
        if (s_g.kind == G_LINE || s_g.kind == G_MOVE) gesture_end();
        return;
    case E_RUP:
        if (s_g.kind == G_ERASE) gesture_end();
        return;
    }
}

/* the drag's step at the pointer's point `p` */
static void follow(const char* ta, DWORD now, unsigned seat, const int32_t p[3], int on_world)
{
    const double z = zoom();
    switch (s_g.kind) {
    case G_LINE: {
        double dx, dy;
        if (!on_world) { s_g.haveLast = 0; return; }
        if (!s_g.haveLast) { memcpy(s_g.last, p, sizeof s_g.last); s_g.haveLast = 1; s_g.at = now; return; }
        dx = ((double)p[0] - s_g.last[0]) * z;
        dy = (((double)p[2] - (p[1] >> 1)) - ((double)s_g.last[2] - (s_g.last[1] >> 1))) * z;
        if (dx * dx + dy * dy < STEP_PX * STEP_PX) return;
        op_line(ta, seat, s_g.last, p);
        memcpy(s_g.last, p, sizeof s_g.last);
        s_g.at = now;
        return;
    }
    case G_MOVE: {
        TagpuBoardMark* m = tagpu_boardstore_find(&s_store, s_g.id);
        if (!m) { gesture_end(); return; }
        if (!on_world || (m->x == p[0] && m->h == p[1] && m->z == p[2])) return;
        op_id(ta, seat, TAGPU_BW_MOVE, s_g.id, p, NULL, 0);
        return;
    }
    case G_ERASE: {
        /* the whole path since the last record, in steps of half the radius, so a fast swipe
           passes over no line it crossed */
        double dx, dz, len;
        int k, n;
        if (!on_world) { s_g.haveLast = 0; return; }
        if (!s_g.haveLast) { memcpy(s_g.last, p, sizeof s_g.last); s_g.haveLast = 1; op_erase(ta, seat, p); return; }
        dx = (double)p[0] - s_g.last[0]; dz = (double)p[2] - s_g.last[2];
        len = sqrt(dx * dx + dz * dz);
        if (len < 1.0) return;
        n = (int)(len / (TAGPU_BW_ERASE_R / 2)) + 1;
        if (n > 256) n = 256;
        for (k = 1; k <= n; k++) {
            int32_t q[3];
            q[0] = s_g.last[0] + (int32_t)(dx * k / n);
            q[1] = p[1];
            q[2] = s_g.last[2] + (int32_t)(dz * k / n);
            op_erase(ta, seat, q);
        }
        memcpy(s_g.last, p, sizeof s_g.last);
        return;
    }
    }
}

void tagpu_board_record(const int* rec, unsigned msg)
{
    TagpuWorld w;
    const int32_t* q;
    int32_t p[3];
    DWORD now = GetTickCount();
    unsigned i;
    int on;
    (void)rec; (void)msg;
    /* the channel's half first, drawn or not: the queued operations out, the marks that have
       arrived in (tagpu_recorder.h) */
    tagpu_rec_mp_board_frame();
    if (!live(&w)) { s_nev = 0; s_penOn = 0; gesture_end(); return; }
    q = (const int32_t*)(w.ta + M_POINT);
    p[0] = q[0] >> 16; p[1] = q[1] >> 16; p[2] = q[2] >> 16;
    on = pointer_on_world(w.ta);
    s_penOn = on;
    if (on) memcpy(s_pen, p, sizeof s_pen);
    for (i = 0; i < s_nev; i++) {
        /* a press off the world view is a press the window procedure took on the last record's
           region: it ends what it would start, and no more */
        if (!on && (s_ev[i] == E_LDOWN || s_ev[i] == E_RDOWN || s_ev[i] == E_MDOWN)) continue;
        press(w.ta, s_ev[i], s_evAt[i], (unsigned)w.player, p, on);
    }
    s_nev = 0;
    /* a gesture lives inside the hold: a button release the board never saw (the wiki takes
       releases while it is open) ends with the key */
    if (!holding() && s_g.kind != G_NONE) { gesture_end(); s_taken[0] = s_taken[1] = s_taken[2] = 0; }
    follow(w.ta, now, (unsigned)w.player, p, on);
    say_lines();
}

/* ------------------------------------------------------------ the window ---- */

static void queue(int ev)
{
    if (s_nev < PENDING) { s_ev[s_nev] = (unsigned char)ev; s_evAt[s_nev] = GetTickCount(); s_nev++; }
    else s_cDrop++;
}

int tagpu_board_key(unsigned vk, int up)
{
    const char* ta;
    TagpuWorld w;
    /* the draw key first, the text box or not: its release ends the hold (taken when its press
       was), its auto-repeat is taken and types nothing, and a press over an open box starts a
       hold, so a click made with it is the board's */
    if (vk == s_vk) {
        int was = s_held;
        if (up) { s_held = 0; return was; }
        if (s_held) return 1;
        if ((ta = main_block()) && live(&w) && !keyboard_taken(ta)) { s_held = 1; return 1; }
        if (!s_box.open) return 0;
    }
    if (s_box.open) {
        if (!(ta = main_block()) || !live(&w)) { s_box.open = 0; return 0; }
        return up ? 1 : 2;
    }
    return 0;
}

int tagpu_board_char(unsigned ch)
{
    if (s_box.open) {
        switch (ch) {
        case 0x0D: box_commit(); return 1;
        case 0x1B: s_box.open = 0; return 1;
        case 0x08: if (s_box.len) s_box.text[--s_box.len] = 0; return 1;
        default:
            if (ch >= TAGPU_PK_GLYPH_LO && ch <= TAGPU_PK_GLYPH_HI && s_box.len < TAGPU_PK_BD_TEXT) {
                s_box.text[s_box.len++] = (char)ch;
                s_box.text[s_box.len] = 0;
            }
            return 1;
        }
    }
    return holding();
}

int tagpu_board_mouse(unsigned msg)
{
    const char* ta;
    TagpuWorld w;
    int b, down;
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: b = 0; down = 1; break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: b = 1; down = 1; break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: b = 2; down = 1; break;
    case WM_LBUTTONUP: b = 0; down = 0; break;
    case WM_RBUTTONUP: b = 1; down = 0; break;
    case WM_MBUTTONUP: b = 2; down = 0; break;
    default: return 0;
    }
    if (!down) {
        if (!s_taken[b]) return 0;
        s_taken[b] = 0;
        if (b < 2) queue(b ? E_RUP : E_LUP);
        return 1;
    }
    /* a press decides afresh: a taken press whose release never came must not swallow the next
       press's release */
    s_taken[b] = 0;
    /* a press anywhere closes an open text box, as Esc does, before it does anything else */
    if (s_box.open) s_box.open = 0;
    if (!holding() || !(ta = main_block()) || !live(&w) || !pointer_on_world(ta)) return 0;
    s_taken[b] = 1;
    queue(b == 0 ? E_LDOWN : b == 1 ? E_RDOWN : E_MDOWN);
    return 1;
}

void tagpu_board_cancel(void)
{
    s_held = 0;
    s_taken[0] = s_taken[1] = s_taken[2] = 0;
    s_nev = 0;
    s_box.open = 0;
    gesture_end();
}

/* ----------------------------------------------------------- the channel ---- */

/* the chat line of a new marker with text from a seat this player sees: at most one a seat in
   SAY_TICKS, so a flood of markers is one line now and again rather than a wall */
static void say_later(const char* ta, unsigned seat, const TagpuBoardOp* op)
{
    char name[32];
    uint32_t now = game_tick(ta);
    if (!op->len || s_nsay >= SAYS) return;
    if ((s_said >> seat & 1u) && (int32_t)(now - s_saidAt[seat]) >= 0 &&
        now - s_saidAt[seat] < SAY_TICKS) return;
    if (!tagpu_share_name(seat, name, sizeof name)) return;
    s_said |= 1u << seat;
    s_saidAt[seat] = now;
    s_say[s_nsay].seat = seat;
    snprintf(s_say[s_nsay].line, sizeof s_say[s_nsay].line, "%s: %s", name, op->text);
    s_say[s_nsay].line[sizeof s_say[s_nsay].line - 1] = 0;
    s_nsay++;
}

/* The owed chat lines, from the in-play handler, outside the simulation's step: the engine's own
   chat sink, the sender's row for its sound (network.md: a row of 10 plays none). */
static void say_lines(void)
{
    unsigned i;
    for (i = 0; i < s_nsay; i++)
        ((void (__stdcall*)(const char*, int, int, int))(uintptr_t)CHAT_LINE)(s_say[i].line, 8, 0,
                                                                               (int)s_say[i].seat);
    s_nsay = 0;
}

int tagpu_board_apply(const void* body, unsigned bytes)
{
    const unsigned char* b = (const unsigned char*)body;
    const char* ta = main_block();
    TagpuBoardOp op;
    unsigned seat, at = TAGPU_BW_HEAD, next, see, me;
    if (!ta || !tagpu_boardwire_check(b, bytes, &seat)) { s_cBad++; return 0; }
    see = seen();
    me = (unsigned char)ta[M_CONTROLLED];
    /* A SEAT THIS PLAYER DOES NOT SEE IS NOT STORED: its marks would take slots from the ones this
       player sees, the store's oldest first. One this player sees changes what its sender's rights
       allowed, cut to what this player sees. */
    while (at < bytes && (next = tagpu_boardwire_next(b, bytes, at, &op)) != 0) {
        s_cIn++;
        if (see >> seat & 1u) {
            int did = tagpu_boardstore_apply(&s_store, seat, &op, game_tick(ta), op.may & see);
            if (did == TAGPU_BS_NEW && seat != me) say_later(ta, seat, &op);
        }
        at = next;
    }
    if (s_box.open && s_box.id && !tagpu_boardstore_find(&s_store, s_box.id)) s_box.open = 0;
    return 1;
}

unsigned tagpu_board_outbox(unsigned char* body, unsigned cap)
{
    TagpuBoardOp op;
    unsigned at = TAGPU_BW_HEAD, next;
    if (s_nout <= TAGPU_BW_HEAD || cap <= TAGPU_BW_HEAD) return 0;
    while (at < s_nout && (next = tagpu_boardwire_next(s_out, s_nout, at, &op)) != 0 && next <= cap)
        at = next;
    if (at == TAGPU_BW_HEAD) return 0;
    memcpy(body, s_out, at);
    memmove(s_out + TAGPU_BW_HEAD, s_out + at, s_nout - at);
    s_nout -= at - TAGPU_BW_HEAD;
    return at;
}

void tagpu_board_outbox_drop(void) { s_nout = 0; }

void tagpu_board_refused(void) { s_cBad++; }

/* --------------------------------------------------------------- the fill ---- */

/* the seat's colour: its logo's, cached a level */
static unsigned char seat_colour(const char* ta, unsigned seat)
{
    int logo = tagpu_packet_pub_player_logo(ta, seat);
    if (logo < 0 || logo >= 16) return (unsigned char)tagpu_replay_logo_colour(0xFFFFu);
    if (!(s_logoHave & (1u << logo))) {
        s_logoCol[logo] = (unsigned char)tagpu_replay_logo_colour((unsigned)logo);
        s_logoHave |= 1u << logo;
    }
    return s_logoCol[logo];
}

int tagpu_board_fill(TAGPU_PK_BOARD* h, TAGPU_PK_BDSEG* segs, unsigned maxs, unsigned* ns,
                     TAGPU_PK_BDMARK* marks, unsigned maxm, unsigned* nm, int* cut)
{
    TagpuWorld w;
    unsigned char col[10];
    unsigned i, k, see;
    *ns = *nm = 0;
    if (!live(&w)) return 0;
    see = seen();
    for (k = 0; k < 10; k++) col[k] = seat_colour(w.ta, k);
    /* the lifetime first, so neither table carries what is gone */
    tagpu_boardstore_expire(&s_store, game_tick(w.ta));
    if (s_box.open && s_box.id && !tagpu_boardstore_find(&s_store, s_box.id)) s_box.open = 0;
    memset(h, 0, sizeof *h);
    h->flags = (uint8_t)((s_held ? TAGPU_PK_BD_HELD : 0) | (s_penOn ? TAGPU_PK_BD_PEN : 0) |
                         (s_box.open ? TAGPU_PK_BD_BOX : 0) |
                         (s_g.kind == G_ERASE && s_penOn ? TAGPU_PK_BD_ERASE : 0));
    h->colour = col[(unsigned)w.player < 10 ? (unsigned)w.player : 0];
    h->black = *(const unsigned char*)(w.ta + M_GUICOL + GUI_BLACK);
    memcpy(h->pen, s_pen, sizeof h->pen);
    h->erase = TAGPU_BW_ERASE_R;
    if (s_box.open) {
        memcpy(h->box, s_box.at, sizeof h->box);
        h->len = (uint8_t)text_copy(h->text, s_box.text, s_box.len);
    }
    for (i = 0; i < s_store.nseg; i++) {
        const TagpuBoardSeg* g = &s_store.seg[(s_store.head + i) % TAGPU_BS_SEGS];
        TAGPU_PK_BDSEG* o;
        if (!g->live || !(see >> g->owner & 1u)) continue;
        if (*ns >= maxs) { *cut = 1; break; }
        o = &segs[(*ns)++];
        memcpy(o->a, g->a, sizeof o->a); memcpy(o->b, g->b, sizeof o->b);
        o->colour = col[g->owner];
        o->pad[0] = o->pad[1] = o->pad[2] = 0;
    }
    for (i = 0; i < TAGPU_BS_MARKS; i++) {
        const TagpuBoardMark* m = &s_store.mark[i];
        TAGPU_PK_BDMARK* o;
        if (!m->id || !(see >> m->owner & 1u)) continue;
        if (*nm >= maxm) { *cut = 1; break; }
        o = &marks[(*nm)++];
        memset(o, 0, sizeof *o);
        o->x = m->x; o->h = m->h; o->z = m->z;
        o->colour = col[m->owner];
        o->flags = (uint8_t)(s_box.open && s_box.id == m->id ? TAGPU_PK_BDM_EDIT : 0);
        o->len = (uint8_t)text_copy(o->text, m->text, m->len);
    }
    /* THE ALLIES' POINTERS, after every marker: the store holds at most TAGPU_BS_MARKS markers, so
       the TAGPU_PK_BD_CURSORS records past them are always free for these */
    {
        TAGPU_REC_CURSOR cur[TAGPU_PK_BD_CURSORS];
        unsigned nc = tagpu_rec_mp_cursors(cur, TAGPU_PK_BD_CURSORS), me = (unsigned)w.player;
        for (i = 0; i < nc; i++) {
            char name[TAGPU_PK_BD_TEXT + 1];
            TAGPU_PK_BDMARK* o;
            /* BOUND: a sample's pointer is the sender's data, which the channel does not range-check
               (rec_pov_ok); one past the packet's coordinate bound would cost the acquire every
               frame it is held, so it is left out here instead */
            if (cur[i].seat >= 10 || cur[i].seat == me || !(see >> cur[i].seat & 1u) ||
                cur[i].x < -TAGPU_BW_COORD || cur[i].x > TAGPU_BW_COORD ||
                cur[i].h < -TAGPU_BW_COORD || cur[i].h > TAGPU_BW_COORD ||
                cur[i].z < -TAGPU_BW_COORD || cur[i].z > TAGPU_BW_COORD) continue;
            if (*nm >= maxm) { *cut = 1; break; }
            o = &marks[(*nm)++];
            memset(o, 0, sizeof *o);
            o->x = cur[i].x; o->h = cur[i].h; o->z = cur[i].z;
            o->colour = col[cur[i].seat];
            o->flags = TAGPU_PK_BDM_CURSOR;
            if (tagpu_share_name(cur[i].seat, name, sizeof name))
                o->len = (uint8_t)text_copy(o->text, name, (unsigned)strlen(name));
        }
    }
    return 1;
}

/* --------------------------------------------------------------- lifetime ---- */

void tagpu_board_level_end(void)
{
    if (s_cMarks || s_cSegs || s_cErase || s_cDrop || s_cFull || s_cIn || s_cBad)
        tagpu_logf("board: level end marks=%u segs=%u erases=%u dropped=%u outbox_full=%u received=%u "
                   "refused=%u", s_cMarks, s_cSegs, s_cErase, s_cDrop, s_cFull, s_cIn, s_cBad);
    tagpu_boardstore_clear(&s_store, LIFE_TICKS);
    s_seq = 0;
    s_nout = 0;
    s_nsay = 0;
    s_said = 0;
    s_logoHave = 0;
    s_cMarks = s_cSegs = s_cErase = s_cDrop = s_cFull = s_cIn = s_cBad = 0;
    tagpu_board_cancel();
    s_lastL.valid = s_lastR.valid = 0;
    s_penOn = 0;
}

void tagpu_board_init(void)
{
    if (!tagpu_route_ok(TAGPU_ROUTE_RIGHT)) {
        tagpu_logf("board: the router's right-press stub is not installed; the board is off");
        return;
    }
    tagpu_boardstore_clear(&s_store, LIFE_TICKS);
    s_armed = 1;
    tagpu_logf("board: armed (key vk 0x%02X)", s_vk);
}
