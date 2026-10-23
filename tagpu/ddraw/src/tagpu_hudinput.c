/* tagpu_hudinput.c -- the HUD's input and its commands: a replay's, and a live skirmish's rewind.
   Contract: tagpu_hudinput.h. Design: research/notes/hud-in-game.md, *Input* and *Live rewind*.

   GAME THREAD, ALL OF IT. Every static below is this thread's: the shield's two mouse paths and
   the wndproc run on it (TA pumps its window on the game thread), and so do the packet's fill,
   the command record's take, the DrawGameScreen observer and the teardown wrap's entry. */
#include <windows.h>
#include <limits.h>
#include <math.h>
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
#include "dd.h"

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
static unsigned s_heldLane;             /* the fork its lane showed at the press (held_lane)      */
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
/* THE STATS WINDOW IS KEPT FOR THE SESSION, never in impure.cfg: its measure, its split and its
   place, until the game exits; it is closed at every replay's start. Its first measure is the mockup's (index.html, S.sm) and its first place the
   layout's (window x, y), until the first drag. */
static int      s_statOpen, s_statTeam, s_statPlaced;
static char     s_statKey[TAGPU_PK_HUD_MEASURE] = "units";
static float    s_statX, s_statY;
static int      s_wdrag;                /* the left press is on the window's title                */
static int      s_wdx, s_wdy;           /* where it landed, in client pixels                      */
static float    s_wox, s_woy;           /* the window's place then, in units                      */
/* THE PICKER (owner decision 1-B, snap back): the button pauses the playback; the next fill plans
   the takeover at the playhead and seeks back to the plan's start R; the picker opens once the
   playback shows R, and a row takes over there. Closed again whenever the playhead leaves R or the
   timeline that plays changes, since R and the eviction were planned for what was on screen. */
enum { PICK_CLOSED, PICK_ASKED, PICK_SEEKING, PICK_OPEN };
static int      s_pick;
static unsigned s_pickR;                /* the plan's start: the first iteration the takeover runs */
static unsigned s_pickEvict;            /* the fork it would delete; UINT32_MAX none               */
static unsigned s_pickTl;               /* the timeline the plan was made on                       */
static unsigned s_pickSerial;           /* the replay's result serial at the plan                  */
static unsigned s_pickCache;            /* the timelines' serial the plan was made under           */
static unsigned s_resSerial;            /* the last result serial the fill has seen                */
static int      s_resSeen;              /* s_resSerial is this level's: a result older than the
                                           level's first fill is never toasted                    */
static int      s_lanes;                /* the Forks bay is open                                   */
static unsigned s_nfork;                /* the last fill's forks                                   */
static char     s_name[TAGPU_PK_HUD_SEATS][32];   /* the last fill's names by recorded seat, UTF-8 */
/* THE LANES: a Delete's first click arms it, the second deletes. s_armed is the armed fork's
   timeline id, 0 none (id 0 is the original, never a fork). The requests the lanes made wait for
   their outcomes, matched by what the outcome names, so one the lanes did not ask for -- a
   trigger's -- is never toasted as theirs: a switch by the timeline it went to, a fork operation
   by its ticket. The replay keeps the last outcome only, so one can be hidden by another: an
   export or a deletion waits in one of HUD_FORKASK slots, which a fifth request takes from the
   oldest, so a hidden outcome's slot is reused rather than held. */
#define HUD_FORKASK 4
static unsigned s_armed;
static int      s_watchAsked;
static unsigned s_watchId;
static unsigned s_forkAsk[HUD_FORKASK], s_forkAskNext;
/* THE LIVE HUD (live_act): the last fill's mode and rewind state, Play from here's arm, and the
   outcome of the rewind verbs it sent (inc/tagpu_replay.h, tagpu_replay_result) */
enum { ARM_NONE, ARM_SEEKING, ARM_ARMED };
static int      s_live;                 /* the last fill was the live HUD's                       */
static int      s_rwState;              /* TAGPU_REPLAY_RWS_*                                     */
static unsigned s_rwG;                  /* where the rewind opened: now, the strip's end          */
static int      s_arm;
static unsigned s_armR, s_armEvict;     /* where Play from here starts, and the fork it deletes   */
static unsigned s_armSerial;            /* the timelines' serial the plan read                    */
static unsigned s_liveSeen;             /* the last outcome's serial looked at                    */
static int      s_rwAsked;              /* a verb the HUD sent has not had its outcome yet        */
static unsigned s_forkAt;               /* the GameTime the HUD's fork starts at                  */
/* THE ENGINE'S MESSAGE LINES, read by DrawChatText's stub for DrawGameScreen's call only
   (tagpu_replay.c, THE MESSAGE LINES): the first line's top in the engine's own pixels, the
   engine's 0x34 unless the last frame drawn had the live strip */
#define CHAT_TOP   0x34
#define ENGINE_BAR 32                   /* the world region's top, engine pixels, at any HUD scale */
static int      s_chatTop = CHAT_TOP;

static int drawn(const char* key);

/* WHERE THE MESSAGE LINES START: below the live strip while the last frame drew it, so no line is
   under it or beside its left end. The strip ends live_clear units under the bar (the layout's
   strip.t and strip.h, its gap under it as over it), and a unit is the attachment's height / 720
   times the HUD's own scale while the bar is the frame's bh scaled by the same height / the
   frame's height (tagpu_hudstate_present): in the frame's pixels the clear row is
   bh + live_clear * scale * gh / 720, whatever the window, and the world region is drawn
   bh - 32 lower than the engine draws it (tagpu_hud_shift), so in the engine's pixels it is
   32 + live_clear * scale * gh / 720. Never above the engine's own 0x34. */
static void chat_top(void)
{
    const HUDART* art = tagpu_hudart_get();
    int y = CHAT_TOP;
    if (art && drawn("live") && g_ddraw.height > 0) {
        float clear = tagpu_hudlay_globals(art->lay)->live_clear * (float)tagpu_hudinput_scale() / 100.0f;
        int at = ENGINE_BAR + (int)ceilf(clear * (float)g_ddraw.height / 720.0f);
        if (at > y && at < (int)g_ddraw.height) y = at;
    }
    s_chatTop = y;
}

/* THE ORDERING: the take runs in DrawGameScreen's hook before the draw (tagpu_packet_pub.c, THE
   COMMANDS), and DrawChatText runs inside that draw, so the message lines' top is set before the
   draw reads it.
   THE VERDICT IS THE LAST PASS'S: a map whose HUD pass ran (`pass`) says whether that frame drew
   the strip, and only such a map moves the lines; a record with no pass behind it (tagpu_vk_frame
   returned before vk_present prepared) says nothing about the screen, which still shows the last
   frame presented, so the lines keep that frame's verdict and do not jump to 0x34 for one draw.
   Every way the HUD stops drawing gives 0x34: on GDI and once the HUD is refused the mode is NONE
   here, on this thread (tagpu_hudstate_on), with no map needed; with no level to show (mode
   NONE) no packet is filled, so the pass that runs builds no state and posts an empty map with
   `pass` set (tagpu_vk_hud_prepare: tagpu_hudstate_drawn(NULL) first); and the level's end resets
   it (tagpu_hudinput_level_end). */
void tagpu_hudinput_take(const TAGPU_CMD_HUD* h)
{
    if (!h) s_hit.n = 0;
    else {
        s_hit = *h;
        if (s_hit.n > TAGPU_CMD_HUDHITS) s_hit.n = TAGPU_CMD_HUDHITS;
        if (s_hit.nseat > TAGPU_PK_HUD_SEATS) s_hit.nseat = TAGPU_PK_HUD_SEATS;
        if (s_hit.nlane > TAGPU_PK_HUD_FORKS) s_hit.nlane = TAGPU_PK_HUD_FORKS;
    }
    if (tagpu_hudstate_mode() == HUD_MODE_NONE) s_chatTop = CHAT_TOP;
    else if (h && h->pass) chat_top();
}

/* THE PAUSE ICON GOES WHERE A CONTROL ON THE SCREEN CARRIES THE PAUSE: the replay HUD's play
   button, or, live, the rewind's strip while a rewind owns the level and holds the game. The live
   HUD idle is one button with no pause on it, so a live game the player pauses shows PAUSED as the
   engine draws it; answering for the whole live mode would take the icon from every recorded
   skirmish. */
int __cdecl tagpu_hudinput_on_screen(void)
{
    TAGPU_REPLAY_RWSTATE rw;
    int mode;
    if (!s_hit.n) return 0;
    mode = tagpu_hudstate_mode();
    if (mode == HUD_MODE_REPLAY) return 1;
    if (mode != HUD_MODE_LIVE) return 0;
    tagpu_replay_rewind_state(&rw);
    return rw.state != TAGPU_REPLAY_RWS_IDLE;
}

/* THE HUD TAKES A CLICK ONLY WHERE THE PLAYER SAW IT AND COULD REACH IT: the HUD shows something
   for the level (tagpu_hudstate_mode: the Vulkan lane, the pass not refused, a recording playing or
   a live recorded skirmish), the last frame it was drawn on had controls, and no menu covers the
   world -- the engine's own test, and Leave's question (tagpu_menu_modal). Under a menu the click
   is the engine's. */
static int active(void)
{
    return s_hit.n && tagpu_hudstate_mode() != HUD_MODE_NONE && !tagpu_menu_modal();
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

/* the last frame drawn has the control `key` */
static int drawn(const char* key)
{
    unsigned i;
    for (i = 0; i < s_hit.n; i++)
        if (!strcmp(s_hit.hit[i].key, key)) return 1;
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
    /* the log keeps what the player was told: a toast shows for toast.ms of wall clock, which a
       frame dump taken every 30 frames can miss on a slow display */
    tagpu_logf("hud: toast \"%s\"", s_toast);
}

/* the window's place as asked: the last drag's, or the layout's */
static void stat_place(float* x, float* y)
{
    const HUDART* art = tagpu_hudart_get();
    if (s_statPlaced || !art) { *x = s_statX; *y = s_statY; return; }
    *x = tagpu_hudlay_globals(art->lay)->win_x;
    *y = tagpu_hudlay_globals(art->lay)->win_y;
}

/* where the window was drawn: its place as asked, held on the screen of the last map as the layout
   holds it, so a place kept from a larger screen or a smaller HUD scale is no dead zone for the drag */
static void stat_origin(float* x, float* y)
{
    const HUDART* art = tagpu_hudart_get();
    stat_place(x, y);
    if (art && s_hit.u > 0) {
        const HUDGLOBALS* g = tagpu_hudlay_globals(art->lay);
        if (*x > s_hit.W - g->win_w) *x = s_hit.W - g->win_w;
        if (*y > s_hit.H - g->win_h) *y = s_hit.H - g->win_h;
        if (*x < 0) *x = 0;
        if (*y < 0) *y = 0;
    }
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

/* THE TIMELINE: a click is mapped on the span the track was drawn with, the drawn frame's axis, so
   it lands where the player saw that time on the track even when the axis has changed since the
   draw (a fork made or deleted, the lanes opened). The axis is the render thread's data and turns
   a pixel into a GameTime only; tagpu_replay_seek_request clamps that to the playing's own span,
   so a stale axis costs a seek to a slightly different time, never one outside the recording. */
static void seek_at(int cx)
{
    int x0 = s_hit.track_x0, x1 = s_hit.track_x1;
    double f;
    unsigned T;
    if (x1 <= x0 || s_hit.t_last <= s_hit.t_first) return;
    f = (double)(cx - x0) / (double)(x1 - x0);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    T = s_hit.t_first + (unsigned)(f * (double)(s_hit.t_last - s_hit.t_first) + 0.5);
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

/* The dock's clock with the playhead at `now`, whole seconds from the first, rounded as the
   layout's clock_s rounds: a toast names a time as the playhead shows it */
static unsigned clock_at(unsigned now)
{
    return ((now > s_first ? now - s_first : 0) + TICKS / 2) / TICKS;
}

#define IS(s) (k == sizeof(s "") - 1 && !strncmp(key, s, k))   /* a literal only */

/* THE TAKEOVER'S KEYS: 1 when the key is one of them. A key that moves the playhead closes the
   picker and is not taken, so act() goes on to move it. */
static int pick_act(const char* key, size_t k, int n)
{
    if (IS("rew") || IS("fwd") || IS("play")) {
        s_pick = PICK_CLOSED;
        return 0;
    }
    if (IS("takeover")) {
        if (s_pick != PICK_CLOSED) s_pick = PICK_CLOSED;
        else {
            /* paused at the click, so the engine runs no step before the next fill, whose GameTime
               is then the playhead the player sees: the T the plan is for */
            tagpu_replay_pause(1);
            s_pick = PICK_ASKED;
        }
    } else if (IS("takeas")) {
        /* a row's index is the drawn frame's; the takeover takes the recorded seat */
        /* the world taken over is the one on screen, the playhead's at R - 1 */
        unsigned seat, t = clock_at(s_pickR - 1);
        if (s_pick != PICK_OPEN) return 1;
        s_pick = PICK_CLOSED;
        if (n < 0 || n >= s_hit.nseat || s_hit.seat[n] >= TAGPU_PK_HUD_SEATS) return 1;
        seat = s_hit.seat[n];
        if (tagpu_replay_takeover_request(seat, s_pickR, -1))
            toast("Taking over %s at %u:%02u", s_name[seat], t / 60, t % 60);
    } else if (IS("lanes")) {
        if (s_nfork) s_lanes ^= 1;
    } else if (!IS("picker")) {
        return 0;
    }
    return 1;
}

/* The fork whose lane a key names in the current hit map: lane:k, export:k and delete:k with k
   one of the lanes it drew; UINT32_MAX for any other key. A press and its release act only when
   this is the same at both, so a lane list that changed under the pointer between them -- a
   deletion's, a fork made -- never sends the release to the fork that moved into its place. */
static unsigned held_lane(const char* key)
{
    const char* colon = strchr(key, ':');
    size_t k = colon ? (size_t)(colon - key) : 0;
    int n = colon ? atoi(colon + 1) : -1;
    if (!(IS("lane") || IS("export") || IS("delete")) || n < 0 || n >= s_hit.nlane) return UINT32_MAX;
    return s_hit.lane_id[n];
}

static void fork_asked(unsigned ticket)
{
    s_forkAsk[s_forkAskNext] = ticket;
    s_forkAskNext = (s_forkAskNext + 1) % HUD_FORKASK;
}

/* THE LANES' KEYS: 1 when the key is one of them. A lane's index is the drawn frame's, which
   lane_id turns into the timeline it showed, as a row's index is turned into its seat: an index
   past the lanes that frame drew is refused, and a lane whose fork has gone since is the replay's
   to refuse (tagpu_replay_watch_timeline, tagpu_replay_fork_op). A request refused at once is
   toasted at once; one accepted waits for its outcome (lane_fill). */
static int lane_act(const char* key, size_t k, int n)
{
    unsigned id, t;
    if (!IS("lane") && !IS("export") && !IS("delete")) return 0;
    if (n < 0 || n >= s_hit.nlane) return 1;
    id = s_hit.lane_id[n];
    /* a lane is a fork's: id 0, the original, never is one, and is s_armed's "none" */
    if (!id) return 1;
    if (IS("lane")) {
        /* the watched lane goes back to the replay, the original */
        unsigned to = id == tagpu_replay_timeline() ? 0 : id;
        s_armed = 0;
        s_watchAsked = tagpu_replay_watch_timeline(to);
        s_watchId = to;
        if (!s_watchAsked && to) toast("Cannot watch F%u", to);
        else if (!s_watchAsked) toast("Cannot go back to the replay");
    } else if (IS("export")) {
        /* the playing's own name with -F<id>: replays\<name>-F<id>.imprec. A name too long for
           that is refused here: a cut one would save under a name the toast does not give. */
        const char* path = tagpu_replay_path();
        char stem[128], tail[16];
        size_t len;
        int nt = _snprintf(tail, sizeof tail, "-F%u", id);   /* at most "-F4294967295" */
        if (!path) {
            toast("Cannot save F%u", id);
            return 1;
        }
        if (!_strnicmp(path, "replays\\", 8)) path += 8;
        len = strlen(path);
        if (len > 7 && !_stricmp(path + len - 7, ".imprec")) len -= 7;
        if (nt < 0 || len + (size_t)nt >= sizeof stem) {
            tagpu_logf("hud: export of timeline %u refused: the recording's name does not leave "
                       "room for -F%u in %u characters", id, id, (unsigned)sizeof stem - 1);
            toast("Cannot save F%u", id);
            return 1;
        }
        memcpy(stem, path, len);
        memcpy(stem + len, tail, (size_t)nt + 1);
        if ((t = tagpu_replay_fork_op(TAGPU_REPLAY_FORK_EXPORT, id, stem))) fork_asked(t);
        else toast("Cannot save F%u", id);
    } else if (s_armed != id) {
        s_armed = id;
        toast("Click again to delete F%u", id);
    } else {
        /* the watched one is left first, for the nearest live timeline up its chain (THE SWITCH) */
        s_armed = 0;
        if ((t = tagpu_replay_fork_op(TAGPU_REPLAY_FORK_DELETE, id, NULL))) fork_asked(t);
        else toast("Cannot delete F%u", id);
    }
    return 1;
}

/* THE PICKER'S STATES, at the fill (game thread; the frame visit runs a seek before the draw whose
   fill this is, so a seek's result and the playhead it brought are seen together) */
static void pick_fill(const TAGPU_PK_HUD* h, const TAGPU_REPLAY_RESULT* r, int fresh)
{
    unsigned t;
    if (s_pick == PICK_ASKED) {
        /* at the recording's end the playhead stands at `last` */
        unsigned T = h->now + 1 < h->last ? h->now + 1 : h->last;
        TAGPU_REPLAY_TKPLAN p;
        int ok;
        ok = tagpu_replay_takeover_plan(T, &p);
        /* T is a timeline's first iteration, which no takeover starts at: one tick on is its
           second, a refresh */
        if (!ok && p.next) ok = tagpu_replay_takeover_plan(p.next, &p);
        /* after the plan, which may refresh the timelines itself: the plan is the cache's as it
           left it, and a re-plan follows only a later change */
        s_pickCache = tagpu_replay_timelines_serial();
        if (!ok) {
            t = clock_at(h->now);
            toast("Cannot take over at %u:%02u", t / 60, t % 60);
            tagpu_logf("hud: take over refused: %s", p.why);
            s_pick = PICK_CLOSED;
            return;
        }
        s_pickR = p.R;
        s_pickEvict = p.evict;
        s_pickTl = tagpu_replay_timeline();
        s_pickSerial = r->serial;
        /* the seek is refused only when no recording plays */
        s_pick = p.R == h->now + 1 || tagpu_replay_seek_request(p.R) ? PICK_SEEKING : PICK_CLOSED;
    }
    if (s_pick == PICK_SEEKING) {
        if (h->now + 1 == s_pickR) s_pick = PICK_OPEN;
        else if (fresh && r->serial != s_pickSerial && r->kind == TAGPU_REPLAY_RQ_SEEK) {
            /* a seek finished since the plan without bringing the playback to R: ours was
               refused, or a later one replaced it (the latest request runs). The picker never
               waits for a seek that will not come. */
            s_pick = PICK_CLOSED;
            t = clock_at(s_pickR - 1);
            if (!r->ok) toast("Cannot go back to %u:%02u", t / 60, t % 60);
        }
    } else if (s_pick == PICK_OPEN && (!h->paused || h->now + 1 != s_pickR)) {
        /* the playhead moved -- the Pause key, a trigger's seek -- so what the picker would take
           over is no longer on screen */
        s_pick = PICK_CLOSED;
    }
    /* a switch can change the chain without moving the playhead; the plan was the old chain's */
    if (s_pick != PICK_CLOSED && tagpu_replay_timeline() != s_pickTl) s_pick = PICK_CLOSED;
    /* the file's timelines changed under the plan -- a fork made, deleted or compacted -- so the
       fork it would delete may be another: planned again at R, once for each change of the
       serial and never otherwise, since a plan reads the file's block headers (tagpu_replay.h).
       A plan that no longer starts at R closes the picker: R is what the playback was brought to. */
    if ((s_pick == PICK_SEEKING || s_pick == PICK_OPEN) &&
        tagpu_replay_timelines_serial() != s_pickCache) {
        TAGPU_REPLAY_TKPLAN p;
        s_pickCache = tagpu_replay_timelines_serial();
        if (tagpu_replay_takeover_plan(s_pickR, &p) && p.R == s_pickR) {
            s_pickEvict = p.evict;
        } else {
            t = clock_at(s_pickR - 1);
            toast("Cannot take over at %u:%02u", t / 60, t % 60);
            if (p.R) tagpu_logf("hud: take over refused: planned again, it starts at %u, not %u", p.R, s_pickR);
            else tagpu_logf("hud: take over refused: %s", p.why);
            s_pick = PICK_CLOSED;
        }
    }
}

/* THE LANES AT THE FILL: the outcomes of what they asked for, toasted once each, and the arm, which
   holds only while the lanes are open and list its fork, marked on that fork */
static void lane_fill(TAGPU_PK_HUD* h, const TAGPU_REPLAY_RESULT* r, int fresh)
{
    int i, listed = 0;
    if (fresh && r->kind == TAGPU_REPLAY_RQ_SWITCH && s_watchAsked && r->id == s_watchId) {
        s_watchAsked = 0;
        if (!r->ok && s_watchId) toast("Cannot watch F%u", s_watchId);
        else if (!r->ok) toast("Cannot go back to the replay");
    } else if (fresh && r->kind == TAGPU_REPLAY_RQ_FORK && r->ticket) {
        for (i = 0; i < HUD_FORKASK && s_forkAsk[i] != r->ticket; i++) {}
        if (i < HUD_FORKASK) {
            s_forkAsk[i] = 0;
            if (r->op == TAGPU_REPLAY_FORK_EXPORT && r->ok) toast("F%u saved as a replay of its own", r->id);
            else if (r->op == TAGPU_REPLAY_FORK_EXPORT) toast("Cannot save F%u", r->id);
            else if (r->ok) toast("F%u deleted", r->id);
            else toast("Cannot delete F%u", r->id);
        }
    }
    for (i = 0; i < h->nfork && i < (int)TAGPU_PK_HUD_FORKS; i++)
        if (s_armed && h->lanes && h->fork[i].id == s_armed) {
            h->fork[i].flags |= TAGPU_PK_FORK_ARMED;
            listed = 1;
        }
    if (!listed) s_armed = 0;
}

/* ---- the live HUD: the rewind's button and strip ------------------------------------------- */

/* a GameTime as the strip's clock: m:ss from the rewind's first GameTime */
static const char* live_clock(char* b, size_t n, unsigned T)
{
    unsigned s = (T > s_first ? T - s_first : 0) / TICKS;
    snprintf(b, n, "%u:%02u", s / 60, s % 60);
    return b;
}

/* one of the rewind's verbs, for the next frame visit, as the `rewind` trigger's (THE LIVE REWIND,
   tagpu_replay.c); its outcome comes back through tagpu_replay_result (live_fill). 1 queued; 0
   refused at once -- no level in play, as after the level's end, which clears the queue -- and
   then the arm ends and the toast says so, since no outcome will follow. */
static int rw_send(int op, unsigned T)
{
    if (tagpu_replay_rewind(op, T, NULL)) {
        s_rwAsked = 1;
        return 1;
    }
    s_arm = ARM_NONE;
    toast("Rewind refused");
    return 0;
}

/* the strip's track: the drawn frame's run mapped on the span it was drawn with (t_first, t_last),
   held to the rewind's own span, a seek a frame and the newest replacing one not yet run.
   THE BOUND: the drawn span is the render thread's and only turns a pixel into a GameTime; the
   clamp is to this thread's span, and the rewind refuses a seek outside its own. */
static void live_seek_at(int cx)
{
    int x0 = s_hit.track_x0, x1 = s_hit.track_x1;
    double f;
    unsigned T;
    if (x1 <= x0 || s_hit.t_last <= s_hit.t_first || s_last <= s_first) return;
    if (s_rwState != TAGPU_REPLAY_RWS_OPEN && s_rwState != TAGPU_REPLAY_RWS_PAST) return;
    f = (double)(cx - x0) / (double)(x1 - x0);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    T = s_hit.t_first + (unsigned)(f * (double)(s_hit.t_last - s_hit.t_first) + 0.5);
    T = T < s_first + 1 ? s_first + 1 : T > s_last ? s_last : T;
    if (T == s_dragT) return;
    s_dragT = T;
    s_arm = ARM_NONE;
    rw_send(TAGPU_REPLAY_RW_SEEK, T);
}

/* the HUD's fork: the game goes on from the world on the screen */
static void live_fork(void)
{
    s_forkAt = s_now + 1;
    s_arm = ARM_NONE;
    rw_send(TAGPU_REPLAY_RW_FORK, 0);
}

/* THE PLAY FROM HERE. What is taken over is what is shown (owner decision 1-B): the rewind's fork
   starts at the refresh at or before its last seek's target (takeover_point), up to a refresh
   interval before the world on the screen, so the first click asks the takeover's plan where the
   fork would start and what it would delete, snaps the past back to that start and shows it with
   the warning; the second plays.
   THE ORDERING ON THIS THREAD: the second click sends FORK only ARM_ARMED, which live_fill sets
   only once s_now + 1 == s_armR -- the snap's seek has run and the world on the screen is R - 1.
   The snap's SEEK made R the rewind's seek target, so the fork verb's own takeover_point(R), on the
   same reader, answers R: the fork starts at the world shown, by the function the plan ran, and
   the eviction shown is the one the fork acts on. The plan reserves nothing.
   THE WARNING SAYS WHAT THE SECOND CLICK DOES, so it is the arm's: live_fill starts it when the arm
   is set, at R, and holds it for as long as the arm holds -- the snap's seek can outlast the
   toast's time (2341 ms measured 2026-10-23 on the battle test group, against toast.ms 1800), and
   a second click after the toast's time would otherwise delete a fork no line on the screen
   named.
   THE PLAN IS THE TIMELINES' AS IT LEFT THEM: the plan refreshes their cache, so the serial is
   read after it (tagpu_replay_timelines_serial); when it moves before the second click the file's
   forks are no longer the ones the plan counted and the eviction named may not be the fork's, so
   live_fill ends the arm and the next click plans again.
   THE PLAN SCANS THE FILE, so it is asked here, on a click, on the game thread between two steps,
   and nowhere per frame: neither live_fill nor the render half calls it. */
static void live_play(void)
{
    TAGPU_REPLAY_TKPLAN p;
    char a[16];
    unsigned T = s_now + 1;
    if (s_rwState == TAGPU_REPLAY_RWS_OPEN) {
        s_arm = ARM_NONE;
        if (rw_send(TAGPU_REPLAY_RW_CLOSE, 0)) toast("Playing on from %s", live_clock(a, sizeof a, s_rwG));
        return;
    }
    if (s_rwState != TAGPU_REPLAY_RWS_PAST || s_arm == ARM_SEEKING) return;
    if (s_arm == ARM_ARMED) { live_fork(); return; }
    if (!tagpu_replay_takeover_plan(T, &p)) {
        toast("Cannot play from %s", live_clock(a, sizeof a, s_now));
        return;
    }
    if (p.R == T && p.evict == UINT32_MAX) { live_fork(); return; }
    s_armSerial = tagpu_replay_timelines_serial();
    s_armR = p.R;
    s_armEvict = p.evict;
    if (p.R != T && !rw_send(TAGPU_REPLAY_RW_SEEK, p.R)) return;
    s_arm = ARM_SEEKING;
}

static void arm_toast(void)
{
    char a[16], warn[24] = "";
    if (s_armEvict != UINT32_MAX) snprintf(warn, sizeof warn, ", replacing F%u", s_armEvict);
    toast("Click again to play from %s%s", live_clock(a, sizeof a, s_armR), warn);
}

/* A click on the live HUD's control, released over it: the rewind's verbs. The strip's plate is
   taken and does nothing; any other key ends Play from here's arm. */
static void live_act(const char* key)
{
    int rw = s_rwState, strip = rw == TAGPU_REPLAY_RWS_OPEN || rw == TAGPU_REPLAY_RWS_PAST;
    if (!strcmp(key, "live_play")) { live_play(); return; }
    if (!strcmp(key, "live")) return;
    s_arm = ARM_NONE;
    if (!strcmp(key, "rewind")) {
        /* the button opens the strip at now and closes it back to now; while the writer flushes
           or a fork ends the original, the rewind would refuse the verb, so none is sent */
        if (rw == TAGPU_REPLAY_RWS_IDLE) rw_send(TAGPU_REPLAY_RW_OPEN, 0);
        else if (rw == TAGPU_REPLAY_RWS_OPEN) rw_send(TAGPU_REPLAY_RW_CLOSE, 0);
        else if (rw == TAGPU_REPLAY_RWS_PAST) rw_send(TAGPU_REPLAY_RW_NOW, 0);
    } else if (!strip) {
        return;
    } else if (!strcmp(key, "rew")) {
        unsigned T = s_now > JUMP ? s_now - JUMP : 0;
        rw_send(TAGPU_REPLAY_RW_SEEK, T > s_first + 1 ? T : s_first + 1);
    } else if (!strcmp(key, "fwd")) {
        /* open, the world is the strip's end and fwd is drawn disabled (tagpu_hudlay.c, live); in
           the past a jump that reaches the span's last GameTime, G - 1 or before, is back to now:
           a seek there would stop short of the world the rewind opened at */
        if (rw != TAGPU_REPLAY_RWS_PAST) return;
        if (s_now + JUMP >= s_last) rw_send(TAGPU_REPLAY_RW_NOW, 0);
        else rw_send(TAGPU_REPLAY_RW_SEEK, s_now + JUMP);
    } else if (!strcmp(key, "live_now")) {
        rw_send(rw == TAGPU_REPLAY_RWS_OPEN ? TAGPU_REPLAY_RW_CLOSE : TAGPU_REPLAY_RW_NOW, 0);
    }
}

const int* tagpu_hudinput_chat_top(void)
{
    return &s_chatTop;
}

/* The packet's fill, live: the clock and the rewind's state, the outcome of the HUD's last verb,
   Play from here's arm, the track's drag, and the pointer, the held control and the toast. */
static void live_fill(TAGPU_PK_HUD* h)
{
    const HUDART* art = tagpu_hudart_get();
    unsigned ms = art ? tagpu_hudlay_globals(art->lay)->toast_ms : 0;
    TAGPU_REPLAY_RESULT r;
    char a[16];
    int cx, cy;

    s_live = 1;
    s_now = h->now;
    s_first = h->first;
    s_last = h->last;
    s_rwState = h->rw_state;
    s_rwG = h->rw_G;
    /* each outcome once, by its serial; the HUD speaks for its own verbs only */
    tagpu_replay_result(&r);
    if (r.serial != s_liveSeen) {
        s_liveSeen = r.serial;
        if (r.kind == TAGPU_REPLAY_RQ_REWIND && s_rwAsked) {
            s_rwAsked = 0;
            if (!r.ok) {
                s_arm = ARM_NONE;
                toast("Rewind refused");
            } else if (r.op == TAGPU_REPLAY_RW_FORK) {
                toast("Playing from %s", live_clock(a, sizeof a, s_forkAt));
            }
        }
    }
    if (s_rwState != TAGPU_REPLAY_RWS_PAST || tagpu_replay_timelines_serial() != s_armSerial) s_arm = ARM_NONE;
    else if (s_arm == ARM_SEEKING && s_now + 1 == s_armR) { s_arm = ARM_ARMED; arm_toast(); }
    else if (s_arm == ARM_ARMED && s_now + 1 != s_armR) s_arm = ARM_NONE;
    h->rw_arm = (uint8_t)s_arm;
    h->rw_R = s_arm ? s_armR : 0;
    h->rw_evict = s_arm ? s_armEvict : UINT32_MAX;

    if (s_drag) {
        if (!active()) { s_drag = 0; s_held[0] = 0; }
        else if (mouse_last_client(&cx, &cy)) live_seek_at(cx);
    }
    h->px = h->py = -1;
    if (mouse_last_client(&cx, &cy) && cx < SHRT_MAX && cy < SHRT_MAX) { h->px = (int16_t)cx; h->py = (int16_t)cy; }
    memcpy(h->held, s_held, sizeof h->held);
    /* while armed the toast is the arm's: every other toast on the live HUD ends the arm first */
    if (s_toast[0] && (s_arm == ARM_ARMED || GetTickCount() - s_toastAt < ms))
        memcpy(h->toast, s_toast, sizeof h->toast);
    else h->toast[0] = 0;
}

/* ---- the replay HUD ------------------------------------------------------------------------- */

/* A click on a control, released over it. Every command is the replay's own call or the stats
   window's own state; a key with no command -- the dock's, the column's and the window's plates,
   the scroll track, the window's title -- is taken and does nothing. */
static void act(const char* key)
{
    const char* colon = strchr(key, ':');
    size_t k = colon ? (size_t)(colon - key) : strlen(key);
    int n = colon ? atoi(colon + 1) : -1, i;
    if (s_live) { live_act(key); return; }
    if (pick_act(key, k, n)) return;
    if (lane_act(key, k, n)) return;
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
}
#undef IS

int tagpu_hudinput_button(int cx, int cy, int gx, int gy, int down, int right)
{
    int bit = right ? 2 : 1;
    char key[TAGPU_PK_HUD_KEY];

    if (!down) {
        if (!(s_down & bit)) return 0;
        s_down &= ~bit;
        if (!right) {
            if (s_drag) {
                if (cx >= 0 && s_live) live_seek_at(cx);
                else if (cx >= 0) seek_at(cx);
            } else if (s_wdrag) {
                if (cx >= 0) stat_drag(cx, cy);
            } else if (s_held[0] && cx >= 0 && active() && !under_menu(gx, gy) && hit(cx, cy, key) &&
                       !strcmp(key, s_held) && held_lane(key) == s_heldLane) {
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
    s_heldLane = held_lane(key);
    if (!strcmp(key, "track")) {
        s_drag = 1;
        s_dragT = UINT_MAX;
        if (s_live) live_seek_at(cx);
        else {
            seek_at(cx);
            s_pick = PICK_CLOSED;           /* the playhead moves */
        }
    } else if (!strcmp(key, "drag")) {
        s_wdrag = 1;
        s_wdx = cx;
        s_wdy = cy;
        stat_origin(&s_wox, &s_woy);
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
    TAGPU_REPLAY_RESULT r;
    int cx, cy, i, fresh;

    if (h->mode == TAGPU_PK_HUD_LIVE) { live_fill(h); return; }
    s_live = 0;
    s_now = h->now;
    s_first = h->first;
    s_last = h->last;
    s_pov = 0;
    for (i = 0; i < h->nseat && i < (int)TAGPU_PK_HUD_SEATS; i++) {
        const TAGPU_PK_HUDSEAT* q = &h->seat[i];
        if (q->seat >= TAGPU_PK_HUD_SEATS) continue;
        if (q->pov) s_pov |= 1u << q->seat;
        tagpu_hudstate_utf8(s_name[q->seat], sizeof s_name[q->seat], q->name, sizeof q->name);
    }
    s_nfork = h->nfork;
    if (!s_nfork) s_lanes = 0;          /* the bay closes with its last fork, as hud.js closes it */

    /* THE REPLAY'S LAST OUTCOME, seen once: its serial grows at each */
    tagpu_replay_result(&r);
    fresh = s_resSeen && r.serial != s_resSerial;
    s_resSerial = r.serial;
    s_resSeen = 1;
    pick_fill(h, &r, fresh);
    if (fresh && r.kind == TAGPU_REPLAY_RQ_TAKEOVER && !r.ok) toast("Take over refused");
    h->pick = (uint8_t)(s_pick == PICK_OPEN ? 2 : s_pick != PICK_CLOSED);
    h->pick_R = s_pick != PICK_CLOSED ? s_pickR : 0;
    h->pick_evict = s_pick != PICK_CLOSED ? s_pickEvict : UINT32_MAX;
    h->lanes = (uint8_t)s_lanes;
    lane_fill(h, &r, fresh);

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
    s_pick = PICK_CLOSED;
    s_resSeen = 0;
    s_lanes = 0;
    s_nfork = 0;
    memset(s_name, 0, sizeof s_name);
    s_armed = 0;
    s_watchAsked = 0;
    memset(s_forkAsk, 0, sizeof s_forkAsk);
    s_live = s_rwAsked = 0;
    s_arm = ARM_NONE;
    s_chatTop = CHAT_TOP;
}
