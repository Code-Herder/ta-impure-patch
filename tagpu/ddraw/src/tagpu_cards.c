/* Player cards beside the replay dock. Engine GUI screens are rectangular and only the top
   screen receives input. Keeping the two separated regions in the dock's post-GUI layer leaves
   the world between them interactive. Immutable GAF planes carry the art; the engine's text
   renderer carries the changing labels. The same pure region map drives draw and hit testing. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "dd.h"
#include "tagpu_cards.h"
#include "tagpu_hud.h"
#include "tagpu_art.h"
#include "tagpu_gaf.h"
#include "tagpu_replay.h"

#define WIDTH 206
#define FOOT 32
#define ROWS 11
#define KEY 9
#define NORMAL 70
#define COMPACT 54
#define GAP 4
#define BTN_W 60
#define BTN_H 20

/* DISASSEMBLED: text 0x4C14F0 is stdcall, ret 0x14; font/width/height at 0x4C1440,
   0x4C1480, 0x4C1450; colour setters/getters at 0x4C13A0, 0x4C1400, 0x4C1410.
   The setters are paired with restore, on the game thread while the back buffer is drawn. */
typedef void (__stdcall *TextFn)(void*, const char*, int, int, int);
typedef void* (__stdcall *FontFn)(void);
typedef int (__stdcall *WidthFn)(const void*, const char*);
typedef int (__stdcall *IntFn)(void);
typedef void (__stdcall *ColourFn)(int, int);
typedef void (__stdcall *BlitFn)(void*, const void*, int, int);
#define DRAW_TEXT ((TextFn)0x004C14F0u)
#define FONT ((FontFn)0x004C1440u)
#define TEXT_WIDTH ((WidthFn)0x004C1480u)
#define TEXT_HEIGHT ((IntFn)0x004C1450u)
#define COLOUR ((ColourFn)0x004C13A0u)
#define FORE ((IntFn)0x004C1400u)
#define BACK ((IntFn)0x004C1410u)
#define BLIT ((BlitFn)0x004B7F90u)

typedef struct { unsigned char hdr[24]; unsigned char pix[WIDTH * NORMAL]; } Plane;
static Plane s_bg, s_row[2], s_button[2][4], s_line[2];
static struct { unsigned char hdr[24], pix[64]; } s_chip[256];
static unsigned char s_bgFrame[64][24];
static unsigned char s_glyph[3][2][24], s_greyGlyph[3][32 * 32];
static void* s_gm;
static int s_ready, s_open, s_visible, s_q8, s_first, s_count, s_pitch, s_slots;
static int s_held[2], s_action = -1;
static TAGPU_HUD_DOCKMAP s_rect;
static TAGPU_REPLAY_PLAYER s_player[ROWS];

static void frame(unsigned char* f, int w, int h, const unsigned char* pix)
{
    unsigned tag = TAGPU_GAF_EXACT;
    memset(f, 0, 24);
    *(unsigned short*)f = (unsigned short)w;
    *(unsigned short*)(f + 2) = (unsigned short)h;
    f[8] = KEY;
    memcpy(f + TAGPU_GF_TAG, &tag, 4);
    memcpy(f + 16, &pix, 4);
}

static void plate(Plane* p, int w, int h, const TAGPU_ART_FRAME* tex, int inset)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned char c = tex->pix[(y % tex->h) * tex->w + x % tex->w];
            if (c == tex->key) c = 60;
            if (!x || !y) c = inset ? 62 : 56;
            if (x == w - 1 || y == h - 1) c = inset ? 56 : 62;
            p->pix[y * w + x] = c;
        }
    frame(p->hdr, w, h, p->pix);
}

static int prepare(void)
{
    const TAGPU_ART_ENTRY* e = tagpu_art_entry("retail", "dock-tex");
    const TAGPU_ART_FRAME* tex;
    static const char* const names[3] = { "g-jump", "g-sight", "g-follow" };
    int i, j, w;
    if (s_ready) return 1;
    if (!e || e->nframes < 1) return 0;
    tex = &e->frames[0];
    if (!tex->w || !tex->h) return 0;
    plate(&s_bg, WIDTH, 64, tex, 1);
    for (i = 1; i <= 64; i++) frame(s_bgFrame[i - 1], WIDTH, i, s_bg.pix);
    plate(&s_row[0], WIDTH, NORMAL, tex, 0);
    plate(&s_row[1], WIDTH, COMPACT, tex, 0);
    for (i = 0; i < 2; i++) {
        w = i ? 92 : BTN_W;
        for (j = 0; j < 4; j++) {
            plate(&s_button[i][j], w, BTN_H, tex, j == 2);
            if (j == 1) memset(s_button[i][j].pix, 56, (size_t)w);
            if (j == 3) memset(s_button[i][j].pix, 59, (size_t)w);
        }
    }
    for (i = 0; i < 256; i++) {
        memset(s_chip[i].pix, i, 64);
        frame(s_chip[i].hdr, 8, 8, s_chip[i].pix);
    }
    for (i = 0; i < 2; i++) {
        memset(s_line[i].pix, i ? 64 : 62, 16);
        frame(s_line[i].hdr, 2, 8, s_line[i].pix);
    }
    for (i = 0; i < 3; i++) {
        e = tagpu_art_entry("retail", names[i]);
        if (!e || e->nframes < 1) return 0;
        {
            const TAGPU_ART_FRAME* a = &e->frames[0];
            if (a->w > 32 || a->h > 32) return 0;
            frame(s_glyph[i][0], a->w, a->h, a->pix);
            for (j = 0; j < a->w * a->h; j++) s_greyGlyph[i][j] = a->pix[j] == a->key ? KEY : 84;
            frame(s_glyph[i][1], a->w, a->h, s_greyGlyph[i]);
        }
    }
    return s_ready = 1;
}

/* The engine's colour sequence lives with the main block, loaded at startup (0x431911).
   The replay seats bound logo against this same count. Decode at most 64x64 into a fixed plane;
   only a palette index escapes. The engine reads the same sequence at 0x467C1E..0x467C24. */
static unsigned colour(unsigned logo)
{
    const char* ta = *(const char* const*)0x00511DE8u;
    const unsigned char *seq, *f;
    unsigned w, h;
    unsigned char pix[64 * 64];
    if (!ta) return 59;
    seq = *(const unsigned char* const*)(ta + 0x148DB);
    if (!seq || logo >= *(const unsigned short*)seq) return 59;
    f = *(const unsigned char* const*)(seq + 0x28 + logo * 8);
    if (!f) return 59;
    w = *(const unsigned short*)f; h = *(const unsigned short*)(f + 2);
    if (!w || !h || w > 64 || h > 64 || !tagpu_gaf_decode(f, w, h, pix)) return 59;
    return pix[(h / 2) * w + w / 2];
}

static int inside(int x, int y)
{
    const char* ta = *(const char* const*)0x00511DE8u;
    /* Comparing the slot identity requires no dereference of the stored GUI pointer. The main
       block owns its current top at +0x531; a modal takes input as soon as it is pushed. */
    return s_visible && ta && *(void* const*)(ta + 0x531) == s_gm && x >= s_rect.l && x < s_rect.l + WIDTH &&
           y >= s_rect.t && y < s_rect.t + s_rect.h;
}

void tagpu_cards_open(int q8, void* gm)
{
    s_gm = gm;
    s_open = prepare(); s_q8 = q8; s_first = 0;
}

void tagpu_cards_close(void)
{
    s_open = s_visible = 0;
    s_gm = NULL;
    s_action = -1;
    tagpu_hud_cards_set(0);
}

void tagpu_cards_cancel(void) { s_action = -1; }
int tagpu_cards_owns(int x, int y) { return inside(x, y); }

void tagpu_cards_tick(int on_top)
{
    s_visible = s_open && on_top && tagpu_replay_live() &&
        tagpu_hud_cards_map((int)g_ddraw.width, (int)g_ddraw.height, s_q8, &s_rect);
    if (s_visible) {
        s_count = tagpu_replay_players(s_player, ROWS);
        s_pitch = (s_count > 4 || s_count * (NORMAL + GAP) - GAP > s_rect.h - FOOT)
                  ? COMPACT + GAP : NORMAL + GAP;
        s_slots = (s_rect.h - FOOT + GAP) / s_pitch;
        if (s_slots > s_count) s_slots = s_count;
        if (s_first > s_count - s_slots) s_first = s_count - s_slots;
        if (s_first < 0) s_first = 0;
        if (!s_count || !s_slots) s_visible = 0;
    }
    if (!s_visible) s_action = -1;
    tagpu_hud_cards_set(s_visible ? s_q8 : 0);
}

/* Actions use header row indices, which never change on death or seek. -1 is empty space;
   0..32 are the three buttons per player (3k Jump, 3k+1 Sight, 3k+2 Follow), 33 full map, 34
   free camera. */
static int action(int x, int y)
{
    int row, b;
    if (!inside(x, y)) return -1;
    x -= s_rect.l; y -= s_rect.t;
    if (y >= s_rect.h - FOOT + 4 && y < s_rect.h - FOOT + 24) {
        if (x >= 8 && x < 100) return 33;
        if (x >= 106 && x < 198) return 34;
    }
    row = y / s_pitch;
    if (row >= s_slots || y % s_pitch < s_pitch - GAP - 24 ||
        y % s_pitch >= s_pitch - GAP - 4 || x < 8) return -1;
    b = (x - 8) / 64;
    return b < 3 && (x - 8) % 64 < 60 ? 3 * (s_first + row) + b : -1;
}

int tagpu_cards_click(int x, int y, int down, int right, int on_top)
{
    int was, a;
    right = !!right;
    if (!down) {
        was = s_held[right]; s_held[right] = 0;
        a = action(x, y);
        if (!right && was && s_action >= 0 && a == s_action && s_visible && on_top) {
            if (a == 33) tagpu_replay_sight_request(-1);
            else if (a == 34) tagpu_replay_follow_request(-1, 0);
            else if (a < 33 && a / 3 < s_count) {
                const TAGPU_REPLAY_PLAYER* p = &s_player[a / 3];
                if (a % 3 == 1) tagpu_replay_sight_request((int)p->seat);
                else if (p->pov) tagpu_replay_follow_request((int)p->seat, a % 3 == 0);
            }
        }
        if (!right) s_action = -1;
        return was;
    }
    s_held[right] = 0;
    if (!right) s_action = -1;
    if (!on_top || !inside(x, y)) return 0;
    s_held[right] = 1;
    if (!right) s_action = action(x, y);
    return 1;
}

int tagpu_cards_wheel(UINT msg, WPARAM wp, LPARAM lp)
{
    int d, x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    if (msg != WM_MOUSEWHEEL || !inside(x, y)) return 0;
    d = (short)HIWORD(wp);
    if (d) {
        s_first += d > 0 ? -1 : 1;
        if (s_first < 0) s_first = 0;
        if (s_first > s_count - s_slots) s_first = s_count - s_slots;
        s_action = -1;
    }
    return 1;
}

static void text_at(int x, int y, int width, const char* str, int grey)
{
    char buf[128];
    size_t i, n;
    int fg = FORE(), bg = BACK();
    const void* font = FONT();
    if (!font) return;
    lstrcpynA(buf, str, sizeof buf);
    /* The engine FNT blitter lacks an upper bound on its character table. Printable ASCII is
       present in the game's UI faces; header names are data, not font-table indices. */
    for (i = 0; buf[i]; i++) if ((unsigned char)buf[i] < 32 || (unsigned char)buf[i] > 126) buf[i] = '?';
    n = strlen(buf);
    while (n && TEXT_WIDTH(font, buf) > width) buf[--n] = 0;
    COLOUR(grey ? 84 : 64, 0);
    DRAW_TEXT(NULL, buf, x, y, width);
    COLOUR(fg, bg);
}

static void number(char* b, size_t n, float v)
{
    if (!isfinite(v)) snprintf(b, n, "?");
    else if (fabsf(v) >= 1000000) snprintf(b, n, "%.1fM", v / 1000000);
    else if (fabsf(v) >= 10000) snprintf(b, n, "%.1fk", v / 1000);
    else snprintf(b, n, "%.0f", v);
}

static void rate(char* b, size_t n, float v)
{
    char value[24];
    number(value, sizeof value, v);
    snprintf(b, n, "%s%s", isfinite(v) && !signbit(v) ? "+" : "", value);
}

static void button(int x, int y, int w, const char* caption, int a, int enabled, int active)
{
    int mx = g_ddraw.cursor.x, my = g_ddraw.cursor.y;
    int hover = mx >= x && mx < x + w && my >= y && my < y + BTN_H;
    int state = !enabled ? 3 : active || (s_action == a && s_held[0] && hover) ? 2 : hover ? 1 : 0;
    BLIT(NULL, s_button[w == 92][state].hdr, x, y);
    if (active && enabled) BLIT(NULL, s_line[1].hdr, x + 1, y + 6);
    if (a < 33) {
        const unsigned char* g = s_glyph[a % 3][!enabled];
        int gw = *(const unsigned short*)g, gh = *(const unsigned short*)(g + 2);
        BLIT(NULL, g, x + 4, y + (BTN_H - gh) / 2);
        text_at(x + gw + 7, y + (BTN_H - TEXT_HEIGHT()) / 2, w - gw - 10, caption, !enabled);
    } else text_at(x + 5, y + (BTN_H - TEXT_HEIGHT()) / 2, w - 10, caption, !enabled);
}

void tagpu_cards_draw(void)
{
    int y, k, fh, sight, followed, by;
    char line[128], m[24], e[24], mi[24], mo[24], ei[24], eo[24];
    if (!s_visible) return;
    for (y = 0; y < s_rect.h; y += 64) {
        int h = s_rect.h - y < 64 ? s_rect.h - y : 64;
        BLIT(NULL, s_bgFrame[h - 1], s_rect.l, s_rect.t + y);
    }
    sight = tagpu_replay_sight();
    followed = tagpu_replay_followed();
    fh = TEXT_HEIGHT();
    for (k = 0; k < s_slots; k++) {
        const TAGPU_REPLAY_PLAYER* p = &s_player[s_first + k];
        int x = s_rect.l, cy = s_rect.t + k * s_pitch, h = s_pitch - GAP;
        BLIT(NULL, s_row[h == COMPACT].hdr, x, cy);
        BLIT(NULL, s_chip[colour(p->colour)].hdr, x + 6, cy + 5);
        text_at(x + 19, cy + 4, h == NORMAL ? 110 : 129, p->name, !p->units);
        snprintf(line, sizeof line, "U:%u", p->units);
        text_at(x + 153, cy + (h == NORMAL ? 16 : 4), 46, line, !p->units);
        if (h == NORMAL) text_at(x + 133, cy + 4, 66,
            !p->units ? "Defeated" : p->commander_alive < 0 ? "Cmdr: ?" :
            p->commander_alive ? "Cmdr alive" : "No cmdr", !p->units);
        number(m, sizeof m, p->metal); number(e, sizeof e, p->energy);
        snprintf(line, sizeof line, "M %s   E %s", m, e);
        text_at(x + 8, cy + 16, h == NORMAL ? 139 : 188, line, 0);
        if (h == NORMAL) {
            rate(mi, sizeof mi, p->metal_in); rate(mo, sizeof mo, -p->metal_out);
            rate(ei, sizeof ei, p->energy_in); rate(eo, sizeof eo, -p->energy_out);
            snprintf(line, sizeof line, "M %s/%s  E %s/%s", mi, mo, ei, eo);
            text_at(x + 8, cy + 27, 188, line, 0);
        } else if (!p->units) {
            text_at(x + 150, cy + 16, 48, "Defeated", 1);
        }
        by = cy + h - 24;
        button(x + 8, by, 60, "Jump", 3 * (s_first + k), p->pov, 0);
        button(x + 72, by, 60, "Sight", 3 * (s_first + k) + 1, 1, sight == (int)p->seat);
        button(x + 136, by, 60, "Follow", 3 * (s_first + k) + 2, p->pov,
               followed == (int)p->seat);
    }
    if (s_count > s_slots) {
        int len = s_rect.h - FOOT, top = len * s_first / s_count;
        int bottom = len * (s_first + s_slots) / s_count;
        for (y = 0; y + 8 <= len; y += 8)
            BLIT(NULL, s_line[y >= top && y < bottom].hdr, s_rect.l + 202, s_rect.t + y);
    }
    by = s_rect.t + s_rect.h - FOOT + 4;
    button(s_rect.l + 8, by, 92, "Full map", 33, 1, sight == -1);
    button(s_rect.l + 106, by, 92, "Free cam", 34, 1, followed == -1);
    /* The fixed footer remains 32 high. A disabled camera control -- a seat whose camera was not
       recorded -- explains itself in the list's spare space when available, otherwise in the
       current card's status line while hovered. */
    k = action(g_ddraw.cursor.x, g_ddraw.cursor.y);
    if (k >= 0 && k < 33 && k % 3 != 1 && k / 3 < s_count && !s_player[k / 3].pov) {
        int hy = s_rect.t + s_slots * s_pitch;
        if (hy + fh < by) text_at(s_rect.l + 8, hy, 190, "Camera was not recorded.", 0);
        else {
            int th = fh + 4;
            if (th > 64) th = 64;
            BLIT(NULL, s_bgFrame[th - 1], s_rect.l, by - th);
            text_at(s_rect.l + 8, by - th + 2, 190, "Camera was not recorded.", 0);
        }
    }
}
