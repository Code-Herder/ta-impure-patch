/* tagpu_hudinput.c -- the replay HUD's input and its commands. Contract: tagpu_hudinput.h. Design:
   research/notes/hud-in-game.md, *Input*.

   GAME THREAD, ALL OF IT. Every static below is this thread's: the shield's two mouse paths and
   the wndproc run on it (TA pumps its window on the game thread), and so do the packet's fill,
   the command record's take, the DrawGameScreen observer and the teardown wrap's entry. */
#include <windows.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mouse.h"
#include "tagpu_hudinput.h"
#include "tagpu_hudstate.h"
#include "tagpu_hudmarks.h"
#include "tagpu_hudlay.h"
#include "tagpu_hudart.h"
#include "tagpu_menu.h"
#include "tagpu_replay.h"
#include "tagpu_settings.h"
#include "tagpu_log.h"

#define JUMP     300u                   /* rewind and forward: 10 s of GameTime at 30 a second   */
#define BM_NEAR  90u                    /* the bookmark button removes one within 3 s of now      */
#define TICKS    30u

static const unsigned SPEEDS[5] = { 5, 10, 20, 40, 80 };   /* 1/2x .. 8x; 10 = 1x                 */
static const unsigned SCALES[4] = { 75, 100, 125, 150 };
static const struct { const char* key; int ov; unsigned bit; } SWITCHES[4] = {
    { "cursors",    TAGPU_OV_CURSORS,    HUD_TOG_CURSORS },
    { "clicks",     TAGPU_OV_CLICKS,     HUD_TOG_CLICKS },
    { "selections", TAGPU_OV_SELECTIONS, HUD_TOG_SELECTIONS },
    { "chat",       TAGPU_OV_CHAT,       HUD_TOG_CHAT },
};
typedef char hud_key_fits[(TAGPU_PK_HUD_KEY == HUD_KEYLEN && TAGPU_PK_HUD_TOAST == HUD_TOASTLEN) ? 1 : -1];

static TAGPU_CMD_HUD s_hit;             /* the controls of the last frame the HUD was drawn on    */
/* THE PAIRED PRESS: bit 0 the left button, bit 1 the right, set by a press the HUD took, whose
   release it takes too, so the engine never sees half a click. A press whose release was lost --
   dragged off the window -- ends at the next press of the same button. */
static int      s_down;
static char     s_held[TAGPU_PK_HUD_KEY];   /* the control the left press landed on               */
static int      s_drag;                 /* the left press is on the timeline                      */
static unsigned s_dragT;                /* the seek it last asked for                             */
static unsigned s_folded, s_scroll;
static char     s_toast[TAGPU_PK_HUD_TOAST];
static DWORD    s_toastAt;
static unsigned s_now, s_first, s_last; /* the last fill's clock                                  */
static unsigned s_pov;                  /* bit S: recorded seat S's camera was recorded           */
static unsigned s_scale;                /* the scale button's choice; 0 = the store's             */
/* LEAVE'S QUESTION HOLDS THE PLAYBACK: the pause from before the click, put back once the question
   is gone -- answered No, or never opened. Yes tears the level down instead, and the level's end
   forgets the question. */
static int      s_leaveWant, s_leaveAsked, s_leaveWasPaused;
/* THE STATS WINDOW IS KEPT FOR THE SESSION, never in impure.cfg (the owner's choice of
   2026-10-09): its measure, its split and its place, until the game exits; it is closed at every
   replay's start. Its first measure is the mockup's (index.html, S.sm) and its first place the
   layout's (window x, y), until the first drag. */
static int      s_statOpen, s_statTeam, s_statPlaced;
static char     s_statKey[TAGPU_PK_HUD_MEASURE] = "units";
static float    s_statX, s_statY;
static int      s_wdrag;                /* the left press is on the window's title                */
static int      s_wdx, s_wdy;           /* where it landed, in client pixels                      */
static float    s_wox, s_woy;           /* the window's place then, in units                      */

void tagpu_hudinput_take(const TAGPU_CMD_HUD* h)
{
    if (!h) { s_hit.n = 0; return; }
    s_hit = *h;
    if (s_hit.n > TAGPU_CMD_HUDHITS) s_hit.n = TAGPU_CMD_HUDHITS;
    if (s_hit.nseat > TAGPU_PK_HUD_SEATS) s_hit.nseat = TAGPU_PK_HUD_SEATS;
}

/* THE HUD TAKES A CLICK ONLY WHERE THE PLAYER SAW IT AND COULD REACH IT: the playback is live, the
   HUD is drawn (tagpu_hudstate_on: the Vulkan lane, the pass not refused), the last frame it was
   drawn on had controls, and no menu covers the world -- the engine's own test, and Leave's
   question (tagpu_menu_modal). Under a menu the click is the engine's. */
static int active(void)
{
    return s_hit.n && tagpu_hudstate_on() && tagpu_replay_live() && !tagpu_menu_modal();
}

/* THE RENDER-OPTIONS PANEL AND ITS SPROCKET ARE DRAWN OVER THE HUD, and the engine's pump, which
   answers the panel, runs after this: a point they own is theirs. (gx, gy) is the engine's point
   the same message carries -- the one tagpu_menu_click and the zoom are asked with -- so the HUD
   and the panel split the screen on one conversion, to the pixel. */
static int under_menu(int gx, int gy)
{
    return tagpu_menu_owns_point(gx, gy);
}

/* the topmost control under the point: its key into `key` */
static int hit(int cx, int cy, char* key)
{
    unsigned i;
    for (i = 0; i < s_hit.n; i++) {
        const TAGPU_CMD_HUDHIT* h = &s_hit.hit[i];
        if (cx >= h->x0 && cx < h->x1 && cy >= h->y0 && cy < h->y1) {
            memcpy(key, h->key, TAGPU_PK_HUD_KEY);
            key[TAGPU_PK_HUD_KEY - 1] = 0;
            return 1;
        }
    }
    return 0;
}

/* is the point over the column or its scroll bar, whatever control is topmost there */
static int over_column(int cx, int cy)
{
    unsigned i;
    for (i = 0; i < s_hit.n; i++) {
        const TAGPU_CMD_HUDHIT* h = &s_hit.hit[i];
        if (cx >= h->x0 && cx < h->x1 && cy >= h->y0 && cy < h->y1 &&
            (!strcmp(h->key, "column") || !strncmp(h->key, "scroll_", 7)))
            return 1;
    }
    return 0;
}

static void toast(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    _vsnprintf(s_toast, sizeof s_toast - 1, fmt, a);
    va_end(a);
    s_toast[sizeof s_toast - 1] = 0;
    s_toastAt = GetTickCount();
}

/* the window's place as asked: the last drag's, or the layout's */
static void stat_place(float* x, float* y)
{
    const HUDART* art = tagpu_hudart_get();
    if (s_statPlaced || !art) { *x = s_statX; *y = s_statY; return; }
    *x = tagpu_hudlay_globals(art->lay)->win_x;
    *y = tagpu_hudlay_globals(art->lay)->win_y;
}

/* THE WINDOW DRAG, as the mockup's: the place at the press moved by the pointer's travel in units,
   kept so the window stays on the screen (the frame of the last map drawn) */
static void stat_drag(int cx, int cy)
{
    const HUDART* art = tagpu_hudart_get();
    const HUDGLOBALS* g;
    float x, y;
    if (!art || !(s_hit.u > 0)) return;
    g = tagpu_hudlay_globals(art->lay);
    x = s_wox + (float)(cx - s_wdx) / s_hit.u;
    y = s_woy + (float)(cy - s_wdy) / s_hit.u;
    s_statX = x < s_hit.W - g->win_w ? x : s_hit.W - g->win_w;
    s_statY = y < s_hit.H - g->win_h ? y : s_hit.H - g->win_h;
    if (s_statX < 0) s_statX = 0;
    if (s_statY < 0) s_statY = 0;
    s_statPlaced = 1;
}

/* the timeline: the playhead's run is the recording, first to last */
static void seek_at(int cx)
{
    int x0 = s_hit.track_x0, x1 = s_hit.track_x1;
    double f;
    unsigned T;
    if (x1 <= x0 || s_last <= s_first) return;
    f = (double)(cx - x0) / (double)(x1 - x0);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    T = s_first + (unsigned)(f * (double)(s_last - s_first) + 0.5);
    if (T == s_dragT) return;
    s_dragT = T;
    tagpu_replay_seek_request(T);
}

/* from the scroll last asked, not the one last drawn: notches that arrive within one frame add up */
static void scroll_by(int d)
{
    int f = (int)(s_scroll < s_hit.last ? s_scroll : s_hit.last) + d;
    s_scroll = f < 0 ? 0 : f > (int)s_hit.last ? s_hit.last : (unsigned)f;
}

/* A click on a control, released over it. Every command is the replay's own call or the stats
   window's own state; a key with no command -- the dock's, the column's and the window's plates,
   the scroll track, the window's title -- is taken and does nothing. */
static void act(const char* key)
{
    const char* colon = strchr(key, ':');
    size_t k = colon ? (size_t)(colon - key) : strlen(key);
    int n = colon ? atoi(colon + 1) : -1, i;
#define IS(s) (k == sizeof(s "") - 1 && !strncmp(key, s, k))   /* a literal only */
    if (IS("rew")) {
        tagpu_replay_seek_request(s_now > JUMP ? s_now - JUMP : 0);
    } else if (IS("fwd")) {
        tagpu_replay_seek_request(s_now + JUMP);
    } else if (IS("play")) {
        /* at the recording's end, play starts it over */
        if (s_now >= s_last) {
            tagpu_replay_seek_request(s_first);
            tagpu_replay_pause(0);
        } else {
            tagpu_replay_pause(!tagpu_replay_paused());
        }
    } else if (IS("speed")) {
        /* the next preset up, from whatever speed is in force, and round to the slowest */
        unsigned gs = (unsigned)tagpu_replay_speed();
        for (i = 0; i < 5 && SPEEDS[i] <= gs; i++) {}
        tagpu_replay_speed_set(SPEEDS[i < 5 ? i : 0]);
    } else if (IS("bookmark")) {
        unsigned at, t;
        int r = tagpu_hudmarks_bookmark(s_now < s_last ? s_now : s_last, BM_NEAR, &at);
        t = (at > s_first ? at - s_first : 0) / TICKS;
        if (r > 0) toast("Bookmark at %u:%02u", t / 60, t % 60);
        else if (r == 0) toast("Bookmark removed");
    } else if (IS("uiscale")) {
        unsigned s = tagpu_hudinput_scale();
        for (i = 0; i < 4 && SCALES[i] != s; i++) {}
        s_scale = SCALES[i < 3 ? i + 1 : 0];
        tagpu_settings_set(TS_REPLAYHUD, (int)s_scale);
        tagpu_logf("hud: the replay HUD's scale is %u%%", s_scale);
    } else if (IS("leave")) {
        if (!tagpu_menu_leave_ready()) toast("Leave from the game menu");
        else if (!s_leaveWant && !s_leaveAsked) {
            s_leaveWasPaused = tagpu_replay_paused();
            tagpu_replay_pause(1);
            s_leaveWant = 1;
        }
    } else if (IS("fold")) {
        if (n >= 0 && n < TAGPU_PK_HUD_SEATS) s_folded ^= 1u << n;
    } else if (IS("sight") || IS("follow") || IS("jump")) {
        /* a row's index is the drawn frame's; the replay takes the recorded seat, and refuses one
           it does not hold */
        int seat;
        if (n < 0 || n >= s_hit.nseat || s_hit.seat[n] >= TAGPU_PK_HUD_SEATS) return;
        seat = s_hit.seat[n];
        if (IS("sight")) tagpu_replay_sight_request(tagpu_replay_sight() == seat ? -1 : seat);
        else if (!(s_pov >> seat & 1)) toast("Camera was not recorded");
        else if (IS("jump")) tagpu_replay_follow_request(seat, 1);
        else tagpu_replay_follow_request(tagpu_replay_followed() == seat ? -1 : seat, 0);
    } else if (IS("stats")) {
        s_statOpen = !s_statOpen;
    } else if (IS("close")) {
        s_statOpen = 0;
    } else if (IS("chip") && colon) {
        if (!strcmp(colon + 1, "player")) s_statTeam = 0;
        else if (!strcmp(colon + 1, "team")) s_statTeam = 1;
        else lstrcpynA(s_statKey, colon + 1, sizeof s_statKey);
    } else if (IS("scroll_up")) {
        scroll_by(-1);
    } else if (IS("scroll_down")) {
        scroll_by(1);
    } else {
        for (i = 0; i < 4; i++)
            if (k == strlen(SWITCHES[i].key) && !strncmp(key, SWITCHES[i].key, k))
                tagpu_replay_overlay_set(SWITCHES[i].ov, !tagpu_replay_overlay(SWITCHES[i].ov));
    }
#undef IS
}

int tagpu_hudinput_button(int cx, int cy, int gx, int gy, int down, int right)
{
    int bit = right ? 2 : 1;
    char key[TAGPU_PK_HUD_KEY];

    if (!down) {
        if (!(s_down & bit)) return 0;
        s_down &= ~bit;
        if (!right) {
            if (s_drag) {
                if (cx >= 0) seek_at(cx);
            } else if (s_wdrag) {
                if (cx >= 0) stat_drag(cx, cy);
            } else if (s_held[0] && cx >= 0 && active() && !under_menu(gx, gy) && hit(cx, cy, key) &&
                       !strcmp(key, s_held)) {
                act(key);
            }
            s_drag = s_wdrag = 0;
            s_held[0] = 0;
        }
        return 1;
    }
    if (s_down & bit) {
        s_down &= ~bit;
        if (!right) { s_drag = s_wdrag = 0; s_held[0] = 0; }
    }
    if (cx < 0 || !active() || under_menu(gx, gy) || !hit(cx, cy, key)) return 0;
    s_down |= bit;
    if (right) return 1;
    memcpy(s_held, key, sizeof s_held);
    if (!strcmp(key, "track")) {
        s_drag = 1;
        s_dragT = UINT_MAX;
        seek_at(cx);
    } else if (!strcmp(key, "drag")) {
        s_wdrag = 1;
        s_wdx = cx;
        s_wdy = cy;
        stat_place(&s_wox, &s_woy);
    }
    return 1;
}

int tagpu_hudinput_wheel(UINT msg, WPARAM wp, LPARAM lp)
{
    int cx, cy, d = (short)HIWORD(wp);
    char key[TAGPU_PK_HUD_KEY];
    if (msg != WM_MOUSEWHEEL || !active() || !mouse_last_client(&cx, &cy) ||
        under_menu((short)LOWORD(lp), (short)HIWORD(lp)) ||
        !hit(cx, cy, key))
        return 0;
    if (d && over_column(cx, cy)) scroll_by(d > 0 ? -1 : 1);
    return 1;
}

void tagpu_hudinput_cancel(void)
{
    s_drag = s_wdrag = 0;
    s_held[0] = 0;
}

void tagpu_hudinput_fill(TAGPU_PK_HUD* h)
{
    const HUDART* art = tagpu_hudart_get();
    unsigned ms = art ? tagpu_hudlay_globals(art->lay)->toast_ms : 0;
    int cx, cy, i;

    s_now = h->now;
    s_first = h->first;
    s_last = h->last;
    s_pov = 0;
    for (i = 0; i < h->nseat && i < (int)TAGPU_PK_HUD_SEATS; i++)
        if (h->seat[i].pov && h->seat[i].seat < TAGPU_PK_HUD_SEATS) s_pov |= 1u << h->seat[i].seat;

    /* THE TIMELINE DRAG SEEKS TO THE POINTER ONCE A FRAME: a seek runs at the frame visit, so a
       request a frame is the most that can run, and the newest replaces one not yet run */
    if (s_drag) {
        if (!active()) { s_drag = 0; s_held[0] = 0; }
        else if (mouse_last_client(&cx, &cy)) seek_at(cx);
    }
    /* and the window follows the pointer once a frame */
    if (s_wdrag) {
        if (!active()) { s_wdrag = 0; s_held[0] = 0; }
        else if (mouse_last_client(&cx, &cy)) stat_drag(cx, cy);
    }
    h->px = h->py = -1;
    if (mouse_last_client(&cx, &cy) && cx < SHRT_MAX && cy < SHRT_MAX) { h->px = (int16_t)cx; h->py = (int16_t)cy; }
    h->folded = (uint16_t)s_folded;
    h->scroll = (uint8_t)(s_scroll < 255 ? s_scroll : 255);
    h->toggles = 0;
    for (i = 0; i < 4; i++)
        if (tagpu_replay_overlay(SWITCHES[i].ov)) h->toggles |= (uint8_t)SWITCHES[i].bit;
    if (s_statOpen) h->toggles |= (uint8_t)HUD_TOG_STATS;
    h->stat_open = (uint8_t)s_statOpen;
    h->stat_team = (uint8_t)s_statTeam;
    memcpy(h->stat_measure, s_statKey, sizeof h->stat_measure);
    stat_place(&h->stat_x, &h->stat_y);
    h->sight = (int8_t)tagpu_replay_sight();
    h->follow = (int8_t)tagpu_replay_followed();
    memcpy(h->held, s_held, sizeof h->held);
    if (s_toast[0] && GetTickCount() - s_toastAt < ms) memcpy(h->toast, s_toast, sizeof h->toast);
    else h->toast[0] = 0;
}

unsigned tagpu_hudinput_scale(void)
{
    int s, i;
    if (!s_scale) {
        s_scale = 100;
        if (tagpu_settings_get(TS_REPLAYHUD, &s))
            for (i = 0; i < 4; i++) if (s == (int)SCALES[i]) s_scale = (unsigned)s;
    }
    return s_scale;
}

int tagpu_hudinput_leave_due(void)
{
    int w = s_leaveWant;
    s_leaveWant = 0;
    return w;
}

void tagpu_hudinput_leave_opened(int opened)
{
    if (opened) {
        s_leaveAsked = 1;
        return;
    }
    tagpu_replay_pause(s_leaveWasPaused);
    toast("Leave from the game menu");
}

void tagpu_hudinput_leave_shown(int open)
{
    if (s_leaveAsked && !open) {
        s_leaveAsked = 0;
        tagpu_replay_pause(s_leaveWasPaused);
    }
}

void tagpu_hudinput_level_end(void)
{
    memset(&s_hit, 0, sizeof s_hit);
    s_down = 0;
    s_held[0] = 0;
    s_drag = 0;
    s_folded = s_scroll = 0;
    s_toast[0] = 0;
    s_statOpen = s_wdrag = 0;
    s_leaveWant = s_leaveAsked = 0;
}
