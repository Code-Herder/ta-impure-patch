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

   THE STORE. Markers by id (the owner's seat in the top byte, a per-seat sequence below), line
   segments in a ring that drops the oldest when it is full. Each keeps its owner seat and its
   birth time; the fill drops what has outlived the lifetime. Nothing here is read by the
   simulation, and nothing here writes engine memory. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "tagpu_board.h"
#include "tagpu_route.h"
#include "tagpu_engine.h"
#include "tagpu_zoom.h"
#include "tagpu_replay.h"
#include "tagpu_packet_pub.h"
#include "tagpu_log.h"
#include "tagpu_shield.h"
#include "dd.h"
#include "render_vk.h"

#define M_POINT    0x2CAA      /* the world point, 16.16 x, altitude, z                        */
#define M_REGION   0x2CC6      /* bit 1 the pointer is in the viewport, bit 3 a box is open    */
#define M_GUICOL   0x0DCB      /* the GUI colour byte array                                    */
#define M_FIELD    0x57D       /* i32 the active textfield's index, -1 none                    */
#define M_SCREENS  0x37EBE     /* u16: 0x865 the options stack, chat, share and the rest       */
#define M_SCREENS2 0x2BEE      /* u8: 0xE0                                                      */
#define GUI_BLACK  0x00

#define MARKS      TAGPU_PK_MAX_BDMARK
#define SEGS       TAGPU_PK_MAX_BDSEG
#define ERASE_R    48          /* world px: the eraser's radius                                */
#define STEP_PX    6.0         /* screen px a line moves before it adds a segment: a corner of
                                  the stroke is kept however fast the pointer moved             */
#define DOUBLE_PX  4.0         /* screen px two presses of a double click may be apart          */
#define LIFE_MS    60000u      /* a mark's lifetime: the owner's default, 60 s                 */
#define PENDING    8

typedef int (__stdcall* GroundHeight)(const int*);

typedef struct {
    uint32_t id;               /* 0: a free slot                                               */
    uint8_t  owner;            /* the seat, < 10                                               */
    uint8_t  len;
    int32_t  x, h, z;
    DWORD    born;
    char     text[TAGPU_PK_BD_TEXT + 1];
} Mark;

typedef struct {
    uint8_t  owner, live;
    int32_t  a[3], b[3];
    DWORD    born;
} Seg;

enum { G_NONE, G_LINE, G_MOVE, G_ERASE };
enum { E_LDOWN, E_LUP, E_RDOWN, E_RUP, E_MDOWN };

static int      s_armed;
static unsigned s_vk = 0xDC;           /* VK_OEM_5, the key that types `\` on a US layout */
static int      s_held;                /* the draw key is down                               */
static int      s_taken[3];            /* the board took this button's press: its release too */

static unsigned char s_ev[PENDING];
static DWORD         s_evAt[PENDING];
static unsigned      s_nev;

static Mark     s_mark[MARKS];
static Seg      s_seg[SEGS];
static unsigned s_segHead, s_segN;     /* the ring: the oldest at head, n of them             */
static uint32_t s_seq[10];

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

static unsigned s_cMarks, s_cSegs, s_cErase, s_cDrop;

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

static Mark* mark_find(uint32_t id)
{
    unsigned i;
    if (!id) return NULL;
    for (i = 0; i < MARKS; i++) if (s_mark[i].id == id) return &s_mark[i];
    return NULL;
}

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

/* A new marker; the oldest is replaced when every slot is taken. */
static uint32_t mark_add(unsigned owner, const int32_t p[3], const char* text, unsigned len)
{
    unsigned i, slot = 0;
    DWORD oldest = 0;
    Mark* m;
    if (owner >= 10) return 0;
    for (i = 0; i < MARKS; i++) {
        if (!s_mark[i].id) { slot = i; break; }
        if (i == 0 || (DWORD)(GetTickCount() - s_mark[i].born) > oldest) {
            oldest = GetTickCount() - s_mark[i].born; slot = i;
        }
    }
    m = &s_mark[slot];
    s_seq[owner] = (s_seq[owner] + 1u) & 0xFFFFFFu;
    if (!s_seq[owner]) s_seq[owner] = 1;
    m->id = ((uint32_t)owner << 24) | s_seq[owner];
    m->owner = (uint8_t)owner;
    m->x = p[0]; m->h = p[1]; m->z = p[2];
    m->born = GetTickCount();
    m->len = (uint8_t)text_copy(m->text, text, len);
    s_cMarks++;
    return m->id;
}

static void seg_add(unsigned owner, const int32_t a[3], const int32_t b[3])
{
    Seg* s;
    if (owner >= 10) return;
    if (s_segN == SEGS) { s_segHead = (s_segHead + 1u) % SEGS; s_segN--; }
    s = &s_seg[(s_segHead + s_segN) % SEGS];
    s->owner = (uint8_t)owner; s->live = 1;
    memcpy(s->a, a, sizeof s->a); memcpy(s->b, b, sizeof s->b);
    s->born = GetTickCount();
    s_segN++;
    s_cSegs++;
}

/* the distance from (x, z) to a segment, on the ground plane, squared */
static double seg_d2(const Seg* s, double x, double z)
{
    double dx = (double)s->b[0] - s->a[0], dz = (double)s->b[2] - s->a[2];
    double l2 = dx * dx + dz * dz, t = 0.0, px, pz;
    if (l2 > 0.0) {
        t = ((x - s->a[0]) * dx + (z - s->a[2]) * dz) / l2;
        t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    }
    px = s->a[0] + t * dx - x; pz = s->a[2] + t * dz - z;
    return px * px + pz * pz;
}

/* Everything within the eraser's radius of (x, z): any part of a line, any marker. */
static void erase_at(const int32_t p[3])
{
    const double r2 = (double)ERASE_R * ERASE_R;
    unsigned i;
    for (i = 0; i < s_segN; i++) {
        Seg* s = &s_seg[(s_segHead + i) % SEGS];
        if (s->live && seg_d2(s, p[0], p[2]) < r2) s->live = 0;
    }
    for (i = 0; i < MARKS; i++) {
        Mark* m = &s_mark[i];
        double dx = (double)m->x - p[0], dz = (double)m->z - p[2];
        if (m->id && dx * dx + dz * dz < r2) {
            if (s_box.open && s_box.id == m->id) s_box.open = 0;
            m->id = 0;
        }
    }
    s_cErase++;
}

/* The marker under the pointer at `p`: its pin, from the ground point to 30 screen px above it
   and 9 either side, the topmost (the nearest the viewer, the greatest z) when pins overlap. */
static Mark* mark_at(const int32_t p[3])
{
    const double z = zoom();
    const double py = (double)p[2] - (double)(p[1] >> 1);
    Mark* best = NULL;
    unsigned i;
    for (i = 0; i < MARKS; i++) {
        Mark* m = &s_mark[i];
        double dx, dy;
        if (!m->id) continue;
        dx = ((double)p[0] - m->x) * z;
        dy = (py - ((double)m->z - (double)(m->h >> 1))) * z;
        if (dx < -9.0 || dx > 9.0 || dy < -30.0 || dy > 4.0) continue;
        if (!best || m->z > best->z) best = m;
    }
    return best;
}

/* ----------------------------------------------------------------- gesture ---- */

static void box_open(const Mark* m, const int32_t p[3])
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
    if (s_box.id) {
        Mark* m = mark_find(s_box.id);
        if (m) m->len = (uint8_t)text_copy(m->text, s_box.text, s_box.len);
    } else if (ta && live(&w)) {
        mark_add((unsigned)w.player, s_box.at, s_box.text, s_box.len);
    }
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

static void press(int ev, DWORD at, unsigned seat, const int32_t p[3], int on)
{
    Mark* m;
    switch (ev) {
    case E_MDOWN:
        if (!mark_at(p)) mark_add(seat, p, "", 0);
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
            if (s_box.open && s_box.id == m->id) s_box.open = 0;
            m->id = 0;
            gesture_end();
            return;
        }
        remember(1, p, at, 1);
        s_g.kind = G_ERASE; s_g.at = at;
        memcpy(s_g.last, p, sizeof s_g.last); s_g.haveLast = 1;
        /* a press on a pin erases nothing where it stands, so a second press can make it the
           double click that deletes that marker alone; the drag erases from its first step */
        if (!m) erase_at(p);
        return;
    case E_LUP:
        /* the stretch since the last step ends where the button came up, on the world view: over
           the minimap the handler's point is the minimap's, across the map */
        if (s_g.kind == G_LINE && s_g.haveLast && on &&
            (s_g.last[0] != p[0] || s_g.last[2] != p[2]))
            seg_add(seat, s_g.last, p);
        if (s_g.kind == G_LINE || s_g.kind == G_MOVE) gesture_end();
        return;
    case E_RUP:
        if (s_g.kind == G_ERASE) gesture_end();
        return;
    }
}

/* the drag's step at the pointer's point `p` */
static void follow(DWORD now, unsigned seat, const int32_t p[3], int on_world)
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
        seg_add(seat, s_g.last, p);
        memcpy(s_g.last, p, sizeof s_g.last);
        s_g.at = now;
        return;
    }
    case G_MOVE: {
        Mark* m = mark_find(s_g.id);
        if (!m) { gesture_end(); return; }
        if (!on_world) return;
        m->x = p[0]; m->h = p[1]; m->z = p[2];
        return;
    }
    case G_ERASE: {
        /* the whole path since the last record, in steps of half the radius, so a fast swipe
           passes over no line it crossed */
        double dx, dz, len;
        int k, n;
        if (!on_world) { s_g.haveLast = 0; return; }
        if (!s_g.haveLast) { memcpy(s_g.last, p, sizeof s_g.last); s_g.haveLast = 1; erase_at(p); return; }
        dx = (double)p[0] - s_g.last[0]; dz = (double)p[2] - s_g.last[2];
        len = sqrt(dx * dx + dz * dz);
        if (len < 1.0) return;
        n = (int)(len / (ERASE_R / 2)) + 1;
        if (n > 256) n = 256;
        for (k = 1; k <= n; k++) {
            int32_t q[3];
            q[0] = s_g.last[0] + (int32_t)(dx * k / n);
            q[1] = p[1];
            q[2] = s_g.last[2] + (int32_t)(dz * k / n);
            erase_at(q);
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
        press(s_ev[i], s_evAt[i], (unsigned)w.player, p, on);
    }
    s_nev = 0;
    /* a gesture lives inside the hold: a button release the board never saw (the wiki takes
       releases while it is open) ends with the key */
    if (!holding() && s_g.kind != G_NONE) { gesture_end(); s_taken[0] = s_taken[1] = s_taken[2] = 0; }
    follow(now, (unsigned)w.player, p, on);
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
        int was = holding();
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

static int expired(DWORD born, DWORD now) { return (DWORD)(now - born) >= LIFE_MS; }

int tagpu_board_fill(TAGPU_PK_BOARD* h, TAGPU_PK_BDSEG* segs, unsigned maxs, unsigned* ns,
                     TAGPU_PK_BDMARK* marks, unsigned maxm, unsigned* nm, int* cut)
{
    TagpuWorld w;
    unsigned char col[10];
    DWORD now = GetTickCount();
    unsigned i, k;
    *ns = *nm = 0;
    if (!live(&w)) return 0;
    for (k = 0; k < 10; k++) col[k] = seat_colour(w.ta, k);
    memset(h, 0, sizeof *h);
    h->flags = (uint8_t)((s_held ? TAGPU_PK_BD_HELD : 0) | (s_penOn ? TAGPU_PK_BD_PEN : 0) |
                         (s_box.open ? TAGPU_PK_BD_BOX : 0) |
                         (s_g.kind == G_ERASE && s_penOn ? TAGPU_PK_BD_ERASE : 0));
    h->colour = col[(unsigned)w.player < 10 ? (unsigned)w.player : 0];
    h->black = *(const unsigned char*)(w.ta + M_GUICOL + GUI_BLACK);
    memcpy(h->pen, s_pen, sizeof h->pen);
    h->erase = ERASE_R;
    if (s_box.open) {
        memcpy(h->box, s_box.at, sizeof h->box);
        h->len = (uint8_t)text_copy(h->text, s_box.text, s_box.len);
    }
    /* the lifetime first, so neither table carries what is gone */
    while (s_segN && (!s_seg[s_segHead].live || expired(s_seg[s_segHead].born, now))) {
        s_segHead = (s_segHead + 1u) % SEGS;
        s_segN--;
    }
    for (i = 0; i < MARKS; i++)
        if (s_mark[i].id && expired(s_mark[i].born, now)) {
            if (s_box.open && s_box.id == s_mark[i].id) { s_box.open = 0; h->flags &= ~TAGPU_PK_BD_BOX; }
            s_mark[i].id = 0;
        }
    for (i = 0; i < s_segN; i++) {
        const Seg* s = &s_seg[(s_segHead + i) % SEGS];
        TAGPU_PK_BDSEG* o;
        if (!s->live || expired(s->born, now)) continue;
        if (*ns >= maxs) { *cut = 1; break; }
        o = &segs[(*ns)++];
        memcpy(o->a, s->a, sizeof o->a); memcpy(o->b, s->b, sizeof o->b);
        o->colour = col[s->owner];
        o->pad[0] = o->pad[1] = o->pad[2] = 0;
    }
    for (i = 0; i < MARKS; i++) {
        const Mark* m = &s_mark[i];
        TAGPU_PK_BDMARK* o;
        if (!m->id) continue;
        if (*nm >= maxm) { *cut = 1; break; }
        o = &marks[(*nm)++];
        memset(o, 0, sizeof *o);
        o->x = m->x; o->h = m->h; o->z = m->z;
        o->colour = col[m->owner];
        o->flags = (uint8_t)(s_box.open && s_box.id == m->id ? TAGPU_PK_BDM_EDIT : 0);
        o->len = (uint8_t)text_copy(o->text, m->text, m->len);
    }
    return 1;
}

/* --------------------------------------------------------------- lifetime ---- */

void tagpu_board_level_end(void)
{
    if (s_cMarks || s_cSegs || s_cErase || s_cDrop)
        tagpu_logf("board: level end marks=%u segs=%u erases=%u dropped=%u", s_cMarks, s_cSegs,
                   s_cErase, s_cDrop);
    memset(s_mark, 0, sizeof s_mark);
    s_segHead = s_segN = 0;
    memset(s_seq, 0, sizeof s_seq);
    s_logoHave = 0;
    s_cMarks = s_cSegs = s_cErase = s_cDrop = 0;
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
    s_armed = 1;
    tagpu_logf("board: armed (key vk 0x%02X)", s_vk);
}
