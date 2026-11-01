/* tagpu_wikireplay.c -- the in-game wiki's replay layer. Contract: tagpu_wikireplay.h; the
   specification is tools/wiki/mockup/replay.js, whose functions these are, under its names. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_wikireplay.h"

static const int SPEEDS[WIKI_REPLAY_NSPEED] = WIKI_REPLAY_SPEEDS;

/* a game simulation of the replay's demo on the heap, the walker's without a sound source; NULL
   with a message in `err` when it is refused or memory runs out */
static WIKISIM* game_new(const WIKIREPLAY* r, int walker, char* err, size_t errn)
{
    WIKISIMCTX ctx = r->ctx;
    WIKISIM* s = (WIKISIM*)malloc(sizeof *s);
    if (!s) {
        snprintf(err, errn, "wiki replay: out of memory");
        return NULL;
    }
    /* no context is tagpu_wikisim_init's own, which has no sound source either */
    if (walker) ctx.snd = NULL;
    if (!tagpu_wikisim_init(s, r->e, r->d, WIKI_PLAY_GAME, r->lay, r->has_ctx ? &ctx : NULL, err, errn)) {
        free(s);
        return NULL;
    }
    return s;
}

static void game_free(WIKISIM* s)
{
    if (!s) return;
    tagpu_wikisim_free(s);
    free(s);
}

/* the game stopped: a lock, a pass that starved, a kill that found no unit */
static int stopped(const WIKISIM* s)
{
    return s && (s->locked != -1 || s->starved || s->bad_kill);
}

static int is_done(const WIKIREPLAY* r)
{
    return r->clock >= r->vend || r->stalled || r->failed || r->left || r->clash || stopped(r->game);
}

static void live_act(WIKIREPLAY* r, int c, int value);

/* a seat's lost rising, a death; its out set, a defeat. BOUND: WIKI_REPLAY_MARKS (tagpu_wikireplay.h),
   and the guard holds the array whatever. The walker's seats are its scene's, at most WIKI_SEATS. */
static void publish(WIKIREPLAY* r)
{
    const WIKISIM* w = r->walker;
    int k;
    for (k = 0; k < w->nseat && k < WIKI_SEATS; k++) {
        const WIKIECON* z = &w->econ[k];
        if (z->lost > r->prev_lost[k] && r->nmark < WIKI_REPLAY_MARKS) {
            r->mark[r->nmark][0] = w->tick;
            r->mark[r->nmark][1] = HUD_MK_DEATH;
            r->mark[r->nmark++][2] = k;
        }
        if (z->out && !r->prev_out[k] && r->nmark < WIKI_REPLAY_MARKS) {
            r->mark[r->nmark][0] = w->tick;
            r->mark[r->nmark][1] = HUD_MK_DEFEAT;
            r->mark[r->nmark++][2] = k;
        }
        r->prev_lost[k] = z->lost;
        r->prev_out[k] = z->out;
    }
}

static void end_walk(WIKIREPLAY* r)
{
    r->walked = r->walker->tick;
    game_free(r->walker);
    r->walker = NULL;
}

/* THE SEEK, to `t` clamped to the game: back, a new game; forward, the target; to where it stands,
   none. Ending a seek in progress there makes what it played heard. */
static void seek(WIKIREPLAY* r, int t)
{
    char err[256];
    if (t < 0) t = 0;
    if (t > r->end) t = r->end;
    if (t < r->game->tick) {
        /* THE LIFETIME: the old game is freed before the new one is made and r->game is cleared with
           it, so no code can reach the freed simulation; nothing else holds it -- a view taken from it
           is valid only until this step or press returns (tagpu_wikireplay_game). */
        game_free(r->game);
        r->game = NULL;
        if (!(r->game = game_new(r, 0, err, sizeof err))) {
            r->failed = 1;
            return;
        }
        r->gen++;
        r->quiet = 0;
        r->target = t == 0 ? -1 : t;
    } else if (t > r->game->tick) r->target = t;
    else {
        if (r->target >= 0) r->quiet = r->game->snd_total;
        r->target = -1;
    }
    r->acc = 0;
}

/* THE CONTROLS, as the game's HUD acts (hud-in-game.md, the keys' table). While Leave's question
   is open every press but its two answers is ignored, and an answer with none open is, and
   leave:yes in a replay that plays a script, and a live HUD's control, which a replay has none of;
   each ignored press is counted. `c` is a WIKI_HUD_* below WIKI_NPRESS: the callers bound it. */
static void act(WIKIREPLAY* r, int c, int value)
{
    int playhead, k, i;
    const int answer = c == WIKI_HUD_LEAVE_NO || c == WIKI_HUD_LEAVE_YES;
    if (r->failed) return;
    if (r->live) {
        live_act(r, c, value);
        return;
    }
    if (r->question ? !answer : answer) {
        r->ignored++;
        return;
    }
    if (c == WIKI_HUD_LEAVE_YES) {
        if (r->scripted) {
            r->ignored++;
            return;
        }
        r->question = 0;
        r->was_paused = 0;
        r->left = 1;
        return;
    }
    playhead = r->target >= 0 ? r->target : r->game->tick;
    switch (c) {
    case WIKI_HUD_PLAY:
        if (playhead == r->end) {
            seek(r, 0);
            r->paused = 0;
        } else r->paused = !r->paused;
        break;
    case WIKI_HUD_SPEED:
        for (k = 0; k < WIKI_REPLAY_NSPEED && SPEEDS[k] <= r->speed; k++) {}
        r->speed = k < WIKI_REPLAY_NSPEED ? SPEEDS[k] : SPEEDS[0];
        r->acc = 0;
        break;
    case WIKI_HUD_REW: seek(r, playhead - WIKI_REPLAY_STEP); break;
    case WIKI_HUD_FWD: seek(r, playhead + WIKI_REPLAY_STEP); break;
    case WIKI_HUD_TRACK: seek(r, value); break;
    case WIKI_HUD_BOOKMARK:
        /* the nearest within NEAR, the earlier on a tie (the list is sorted, and only a nearer one
           replaces it) */
        for (i = 0, k = -1; i < r->nbook; i++)
            if (abs(r->book[i] - playhead) <= WIKI_REPLAY_NEAR && (k < 0 || abs(r->book[i] - playhead) < abs(r->book[k] - playhead)))
                k = i;
        if (k >= 0) {
            r->toast_kind = WIKI_TOAST_REMOVED;
            r->toast_value = r->book[k];
            r->toast_at = r->tick;
            memmove(&r->book[k], &r->book[k + 1], (size_t)(r->nbook - k - 1) * sizeof r->book[0]);
            r->nbook--;
        } else if (r->nbook < WIKI_REPLAY_BOOKS) {
            for (i = 0; i < r->nbook && r->book[i] <= playhead; i++) {}
            memmove(&r->book[i + 1], &r->book[i], (size_t)(r->nbook - i) * sizeof r->book[0]);
            r->book[i] = playhead;
            r->nbook++;
            r->toast_kind = WIKI_TOAST_ADDED;
            r->toast_value = playhead;
            r->toast_at = r->tick;
        }
        break;
    case WIKI_HUD_LEAVE:
        r->question = 1;
        r->was_paused = r->paused;
        r->paused = 1;
        break;
    case WIKI_HUD_LEAVE_NO:
        r->paused = r->was_paused;
        r->question = 0;
        r->was_paused = 0;
        break;
    default:
        r->ignored++;
        break;
    }
}

/* THE TRIP's events on clock tick c: a button acts at its release; the track seeks at its press,
   during a drag at each tick i = 1..over to to + (drag - to) i / over (C's division, truncating,
   which replay.js's Math.trunc equals: |numerator| < 2^31) when that differs from the last, and at
   the release to where it is. */
static void trip(WIKIREPLAY* r, int c)
{
    const int press = r->at + WIKI_REPLAY_HUD_DOWN;
    if (r->to != WIKI_HUD_TRACK) {
        if (c == r->at + WIKI_REPLAY_HUD_UP) act(r, r->to, 0);
        return;
    }
    if (c == press) {
        /* the press on the track starts a new drag: no seek is a repeat of the last one
           (tagpu_hudinput.c, the press) */
        if (r->live) r->drag_t = -1;
        act(r, WIKI_HUD_TRACK, r->value);
    }
    if (r->drag >= 0 && c > press && c <= press + r->over) {
        const int p = r->value + (r->drag - r->value) * (c - press) / r->over;
        if (p != r->pos) {
            r->pos = p;
            act(r, WIKI_HUD_TRACK, p);
        }
    }
    if (c == (r->drag >= 0 ? press + r->over : r->at + WIKI_REPLAY_HUD_UP)) act(r, WIKI_HUD_TRACK, r->pos);
}

/* the viewer's clock tick c: the trip's events first (no other press stands in a trip), then the
   script's inputs. A press's control was bounded by the loader (WIKI_HUD_*); its ticks by the game's
   end. */
static void feed(WIKIREPLAY* r, int c)
{
    if (r->has_cursor) trip(r, c);
    while (r->next < r->nscript && r->script[r->next]->at == c) {
        const WIKIINPUT* e = r->script[r->next++];
        if (e->has & WIKI_IN_UNTIL) {
            r->waiting = WIKI_UNTIL_SEEKED;
            r->wait_from = r->tick;
            r->wait_max = e->until_max;
        } else if (e->has & WIKI_IN_HUD) {
            /* in a live demo a trip that starts while the rewind is idle starts from seat 0's cursor */
            const int from_game = r->live && r->rw == WIKI_RW_IDLE;
            r->from = from_game ? WIKI_REPLAY_AT_GAME : r->has_cursor ? r->to : -1;
            r->from_value = !from_game && r->has_cursor ? r->pos : -1;
            r->has_cursor = 1;
            r->to = e->hud;
            r->value = e->hud == WIKI_HUD_TRACK ? e->to : -1;
            r->at = c;
            r->drag = e->hud == WIKI_HUD_TRACK ? e->drag : -1;
            r->over = r->drag >= 0 ? e->over : 0;
            r->pos = r->value;
        } else r->caption = e->caption;
    }
}

/* the game's part of a step: a seek's ticks, else the speed's */
static void play(WIKIREPLAY* r)
{
    WIKISIM* g = r->game;
    int i, n;
    if (r->target >= 0) {
        n = r->target - g->tick < WIKI_REPLAY_SEEK ? r->target - g->tick : WIKI_REPLAY_SEEK;
        for (i = 0; i < n && !g->done; i++) tagpu_wikisim_step(g);
        if (g->tick == r->target) {
            r->target = -1;
            r->quiet = g->snd_total;
        }
    } else if (!r->paused) {
        r->acc += r->speed;
        while (r->acc >= 10 && g->tick < r->end && !g->done) {
            tagpu_wikisim_step(g);
            r->acc -= 10;
        }
        if (g->tick == r->end) {
            r->paused = 1;
            r->acc = 0;
        }
    }
}

/* ---- THE LIVE MODE (tagpu_wikireplay.h, LIVE; replay.js's functions, under its names) ---- */

/* the clock tick the current trip releases on */
static int release_of(const WIKIREPLAY* r)
{
    return r->drag >= 0 ? r->at + WIKI_REPLAY_HUD_DOWN + r->over : r->at + WIKI_REPLAY_HUD_UP;
}

/* the viewer's hand is busy on clock tick c: a trip from its start to its release, or the glide
   back to the game */
static int in_hand(const WIKIREPLAY* r, int c)
{
    return (r->has_cursor && c >= r->at && c <= release_of(r))
        || (r->back >= 0 && c >= r->back && c < r->back + WIKI_REPLAY_HUD_DOWN);
}

/* the GameTime the world shown stands at: G while open; the seek's T - 1, or P's tick, in the
   past; L's while idle */
static int now_tick(const WIKIREPLAY* r)
{
    if (r->rw == WIKI_RW_OPEN) return r->G;
    if (r->rw == WIKI_RW_PAST) return r->target >= 0 ? r->target - 1 : r->past ? r->past->tick : r->G;
    return r->game->tick;
}

/* a timeline's chain into `c`: the row indices from the original down to it; its length. THE
   BOUND: a parent's id is below its child's (ids only grow), so the walk ends within id + 1 rows,
   and the guard holds the array whatever. */
static int chain_of(const WIKIREPLAY* r, int id, int* c)
{
    int n = 0, k, i, t;
    for (k = id; k >= 0 && n < RECR_TIMELINES; k = r->tl[k].parent) c[n++] = k;
    for (i = 0; i < n / 2; i++) {
        t = c[i];
        c[i] = c[n - 1 - i];
        c[n - 1 - i] = t;
    }
    return n;
}

/* a log entry applied to a simulation, by the call that applied it live: an input as Try it gives
   one (tagpu_wiki.c try_send), or the release */
static void apply(WIKISIM* s, const WIKIREPLAYLOG* x)
{
    WIKIINPUT in;
    if (x->kind == WIKI_LOG_RELEASE) {
        tagpu_wikisim_release(s, 0);
        return;
    }
    memset(&in, 0, sizeof in);
    in.down = in.up = in.build = -1;
    if (x->kind == WIKI_LOG_MOVE) {
        in.has = WIKI_IN_MOVE;
        in.move.x = x->x;
        in.move.z = x->z;
    } else if (x->kind == WIKI_LOG_DOWN) in.down = x->key;
    else in.up = x->key;
    tagpu_wikisim_input(s, &in);
}

/* the log's block holds `k` entries more: 1, or 0 when memory runs out */
static int log_room(WIKITIMELINE* t, int k)
{
    WIKIREPLAYLOG* g;
    int cap = t->cap ? t->cap : 64;
    if (t->nlog + k <= t->cap) return 1;
    while (cap < t->nlog + k) cap *= 2;
    if (!(g = (WIKIREPLAYLOG*)realloc(t->log, (size_t)cap * sizeof *g))) return 0;
    t->log = g;
    t->cap = cap;
    return 1;
}

/* LOGGED FIRST (tagpu_wikireplay.h): the entry joins the selected timeline's log, then L takes it,
   with `spare` more entries left in the block; 0, nothing applied, when memory runs out. The
   callers keep WIKI_REPLAY_LOG's bound. */
static int log_apply(WIKIREPLAY* r, int kind, int key, double x, double z, int spare)
{
    WIKITIMELINE* t = &r->tl[r->selected];
    WIKIREPLAYLOG* e;
    if (!log_room(t, 1 + spare)) return 0;
    e = &t->log[t->nlog++];
    e->tau = r->game->tick + 1;
    e->kind = (int8_t)kind;
    e->key = (int8_t)key;
    e->x = x;
    e->z = z;
    r->nlogged++;
    apply(r->game, e);
    return 1;
}

static void toast_of(WIKIREPLAY* r, int kind, int value, int evict)
{
    r->toast_kind = kind;
    r->toast_at = r->tick;
    r->toast_value = value;
    r->toast_evict = evict;
}

/* a plan: the fork's first tick, its parent's id, and the fork it evicts (-1 none) */
typedef struct {
    int R, parent, evict;
} PLAN;

/* THE PLAN (tagpu_replay.c takeover_point, tagpu_recreader.c recr_append_point): a fork asked for at
   GameTime T starts in the chain member holding T, at the last GameTime of the member's segment up
   to T that the recorder's writer flags a refresh (reclossy_is_refresh, walked from the member's
   first with the ordinal from 1); the member's first is refused, as is a timeline past
   RECR_TIMELINES and a live fork set rec_fork_evict cannot bound. At most the game's end of
   integer steps: it runs on a click, never per step. */
static int plan(const WIKIREPLAY* r, int T, PLAN* p)
{
    int chain[RECR_TIMELINES], n = chain_of(r, r->selected, chain), k = n - 1, t, i;
    unsigned nth = 1, prev = 0;
    RecForkRow rows[RECR_TIMELINES];
    uint32_t evict;
    const WIKITIMELINE* m;
    while (k > 0 && r->tl[chain[k]].R > T) k--;
    m = &r->tl[chain[k]];
    p->R = m->R;
    for (t = m->R; t <= T; t++, nth++)
        if (reclossy_is_refresh(nth, (unsigned)t, prev, REC_REFRESH_TICKS)) {
            p->R = t;
            prev = (unsigned)t;
        }
    if (p->R == m->R || r->ntl >= RECR_TIMELINES) return 0;
    for (i = 0; i < r->ntl; i++) {
        rows[i].id = (uint32_t)r->tl[i].id;
        rows[i].live = (uint8_t)r->tl[i].live;
        rows[i].used = (uint64_t)r->tl[i].used;
    }
    if (!rec_fork_evict(rows, (unsigned)r->ntl, (uint32_t)r->selected, REC_FORK_LIMIT, &evict)) return 0;
    p->parent = m->id;
    p->evict = evict == UINT32_MAX ? -1 : (int)evict;
    return 1;
}

/* the open: refused when the span is empty (tagpu_replay.c: no complete seek interval yet) or the
   log cannot keep WIKI_REPLAY_LOG_KEEP. Try it after a fork: each key and button the world holds
   goes up into L, logged, so L never holds one the player cannot let go of while the strip is
   open; log_room has already grown the block for those ups and the release, so neither fails. */
static void live_open(WIKIREPLAY* r)
{
    WIKISIM* L = r->game;
    int held[WIKI_NKEYS], k;
    if (L->tick - 1 <= WIKI_REPLAY_FIRST || r->nlogged > WIKI_REPLAY_LOG - WIKI_REPLAY_LOG_KEEP
        || !log_room(&r->tl[r->selected], WIKI_REPLAY_LOG_KEEP)) {
        toast_of(r, WIKI_TOAST_REFUSED, 0, -1);
        return;
    }
    memcpy(held, L->in[0].held, sizeof held);
    if (!r->scripted && r->selected != 0)
        for (k = 0; k < WIKI_NKEYS; k++)
            if (held[k]) log_apply(r, WIKI_LOG_UP, k, 0, 0, 0);
    r->G = L->tick;
    r->last = r->G - 1;
    r->rw = WIKI_RW_OPEN;
}

/* the close from the strip open without a seek: the game rebuilds nothing (rw_close_unmoved) */
static void live_close(WIKIREPLAY* r)
{
    r->rw = WIKI_RW_IDLE;
    r->back = r->clock;
}

/* BACK TO NOW (and a close in the past, the same rebuild in the game): P freed (THE LIFETIME), then
   the release on L, logged at G + 1 like an input in the entry the open kept for it; the
   DisplayTimer set to G (m0-facts.md: 2561 read at GameTime 2536 after Back to now at 2501, set to
   G then raised at 2502 and 2532 [INFERRED from the arithmetic]) */
static void to_now(WIKIREPLAY* r)
{
    game_free(r->past);
    r->past = NULL;
    r->target = -1;
    log_apply(r, WIKI_LOG_RELEASE, -1, 0, 0, 0);
    r->dt = r->G;
    r->quiet = r->game->snd_total;
    r->rw = WIKI_RW_IDLE;
    r->back = r->clock;
}

/* THE LIVE SEEK to T, shown as the world of T - 1: back of P's tick (or with no P), P freed (THE
   LIFETIME) and made again from the demo to re-play the selected timeline's chain from tick 0;
   forward, the target. A make that fails fails the replay. */
static void live_seek(WIKIREPLAY* r, int T)
{
    char err[256];
    if (!r->past || T - 1 < r->past->tick) {
        game_free(r->past);
        r->past = NULL;
        if (!(r->past = game_new(r, 0, err, sizeof err))) {
            r->failed = 1;
            return;
        }
        r->pgen = ++r->made;
        r->npchain = chain_of(r, r->selected, r->pchain);
        r->pm = 0;
        r->pl = 0;
    }
    r->rw = WIKI_RW_PAST;
    r->target = T;
}

/* ONE TICK OF P along the chain, from t to t + 1: at the next member's R the member's fork
   operation, then the entries that member logged at t + 1, in log order, through the calls that
   applied them live, so P's world is the world the timeline was played with by construction */
static void past_tick(WIKIREPLAY* r)
{
    char err[256];
    WIKISIM* P = r->past;
    const int t = P->tick;
    const WIKITIMELINE* m;
    if (r->pm + 1 < r->npchain && t + 1 == r->tl[r->pchain[r->pm + 1]].R) {
        m = &r->tl[r->pchain[++r->pm]];
        r->pl = 0;
        if (!tagpu_wikisim_fork(P, m->R, 0, m->part, m->npart, m->mode, err, sizeof err)) {
            r->failed = 1;
            return;
        }
    }
    m = &r->tl[r->pchain[r->pm]];
    for (; r->pl < m->nlog && m->log[r->pl].tau <= t + 1; r->pl++)
        if (m->log[r->pl].tau == t + 1) apply(P, &m->log[r->pl]);
    tagpu_wikisim_step(P);
}

/* the rows a live timeline's chain reads keep their part and log; every other deleted row frees
   both (THE BOUND on nlogged) */
static void sweep(WIKIREPLAY* r)
{
    int keep[RECR_TIMELINES], i, k;
    memset(keep, 0, sizeof keep);
    for (i = 0; i < r->ntl; i++)
        if (r->tl[i].live)
            for (k = i; k >= 0 && !keep[k]; k = r->tl[k].parent) keep[k] = 1;
    for (i = 0; i < r->ntl; i++)
        if (!keep[i]) {
            r->nlogged -= r->tl[i].nlog;
            free(r->tl[i].log);
            r->tl[i].log = NULL;
            r->tl[i].nlog = r->tl[i].cap = 0;
            r->tl[i].part = NULL;
            r->tl[i].npart = 0;
        }
}

/* THE FORK, with P standing at p->R - 1 and no seek pending: the part a scripted demo gives the
   timeline of the next id (its from and parent must be the plan's), or Try it's none in stall
   mode, the economy's stall mode's caller; the fork operation, which ends with the release; the
   selected row closed at G, the evicted one deleted, the new row selected, and P takes L's place
   (THE LIFETIME). The game's fork sets the DisplayTimer to the GameTime shown (MEASURED,
   m0-facts.md: 1200 at the fork that takes over at 1201). The new id is below RECR_TIMELINES: the
   plan refused the one past it. */
static void fork_at(WIKIREPLAY* r, const PLAN* p)
{
    char err[256];
    WIKISIM* P = r->past;
    const int id = r->ntl;
    const WIKIINPUT* part = NULL;
    int npart = 0, mode = WIKI_ECON_STALL, i;
    WIKITIMELINE* t;
    if (r->scripted) {
        const WIKIFORK* f = NULL;
        for (i = 0; i < r->d->nfork && !f; i++)
            if (r->d->fork[i].id == id) f = &r->d->fork[i];
        if (!f || f->from != p->R || f->parent != p->parent) {
            r->failed = 1;
            return;
        }
        part = f->input;
        npart = f->ninput;
        mode = WIKI_ECON_REFUSE;
    }
    if (!tagpu_wikisim_fork(P, p->R, 0, part, npart, mode, err, sizeof err)) {
        r->failed = 1;
        return;
    }
    r->dt = p->R - 1;
    r->tl[r->selected].last = r->G;
    if (p->evict >= 0) r->tl[p->evict].live = 0;
    t = &r->tl[id];
    memset(t, 0, sizeof *t);
    t->id = id;
    t->parent = p->parent;
    t->R = p->R;
    t->last = p->R - 1;
    t->live = 1;
    t->used = r->tick;
    t->mode = mode;
    t->part = part;
    t->npart = npart;
    r->ntl++;
    r->selected = id;
    sweep(r);
    game_free(r->game);
    r->game = P;
    r->past = NULL;
    r->gen = r->pgen;
    r->quiet = r->game->snd_total;
    r->target = -1;
    r->rw = WIKI_RW_IDLE;
    r->arm = WIKI_ARM_NONE;
    r->back = r->clock;
    toast_of(r, WIKI_TOAST_PLAYING_FROM, p->R, -1);
}

/* PLAY FROM HERE (tagpu_hudinput.c live_play): open, it closes and plays on; in the past, armed it
   forks, else it plans at the GameTime after the world shown -- refused, it says so; at a refresh
   with nothing to evict it forks at once; else it arms, snapping the past back to the plan's start
   when that is not the world shown */
static void play_from(WIKIREPLAY* r, int now)
{
    PLAN p;
    if (r->rw == WIKI_RW_OPEN) {
        live_close(r);
        toast_of(r, WIKI_TOAST_PLAYING_ON, r->G, -1);
        return;
    }
    if (r->rw != WIKI_RW_PAST || r->arm == WIKI_ARM_SEEKING) return;
    if (!plan(r, now + 1, &p)) {
        r->arm = WIKI_ARM_NONE;
        toast_of(r, WIKI_TOAST_CANNOT, now, -1);
        return;
    }
    if (r->arm == WIKI_ARM_ARMED || (p.R == now + 1 && p.evict < 0)) {
        fork_at(r, &p);
        return;
    }
    r->arm_R = p.R;
    r->arm_evict = p.evict;
    if (p.R != now + 1) live_seek(r, p.R);
    r->arm = WIKI_ARM_SEEKING;
}

/* THE LIVE HUD's CONTROLS (tagpu_hudinput.c live_act, rewind-spec.md 3.2): a control no live demo
   presses and Play from here while a seek is pending are ignored and counted; every other ends
   Play from here's arm, and every one but the track ends the track's drag. `c` is below
   WIKI_NPRESS: the callers bound it. */
static void live_act(WIKIREPLAY* r, int c, int value)
{
    int now, T;
    if (!tagpu_wikidata_hud_presses(c, 1) || (c == WIKI_HUD_LIVE_PLAY && r->target >= 0)) {
        r->ignored++;
        return;
    }
    if (c != WIKI_HUD_LIVE_PLAY) r->arm = WIKI_ARM_NONE;
    if (c != WIKI_HUD_TRACK) r->drag_t = -1;
    now = now_tick(r);
    switch (c) {
    case WIKI_HUD_REWIND:
        if (r->rw == WIKI_RW_IDLE) live_open(r);
        else if (r->rw == WIKI_RW_OPEN) live_close(r);
        else to_now(r);
        break;
    case WIKI_HUD_REW:
        if (r->rw != WIKI_RW_IDLE)
            live_seek(r, now - WIKI_REPLAY_STEP > WIKI_REPLAY_FIRST + 1 ? now - WIKI_REPLAY_STEP : WIKI_REPLAY_FIRST + 1);
        break;
    case WIKI_HUD_FWD:
        /* open, fwd is drawn disabled; in the past a jump that reaches the span's last is Back to now */
        if (r->rw != WIKI_RW_PAST) break;
        if (now + WIKI_REPLAY_STEP >= r->last) to_now(r);
        else live_seek(r, now + WIKI_REPLAY_STEP);
        break;
    case WIKI_HUD_TRACK:
        if (r->rw == WIKI_RW_IDLE) break;
        T = value > WIKI_REPLAY_FIRST + 1 ? value : WIKI_REPLAY_FIRST + 1;
        T = T < r->last ? T : r->last;
        if (T != r->drag_t) {
            r->drag_t = T;
            live_seek(r, T);
        }
        break;
    case WIKI_HUD_LIVE_NOW:
        if (r->rw == WIKI_RW_OPEN) live_close(r);
        else if (r->rw == WIKI_RW_PAST) to_now(r);
        break;
    default: /* WIKI_HUD_LIVE_PLAY */
        play_from(r, now);
        break;
    }
}

/* the live game's part of a step: L's tick while idle (the clash rule reads its fed count), a
   seek's ticks of P in the past; the seek ends on T - 1, whose world is then the one shown and
   whose GameTime the DisplayTimer takes */
static void live_play(WIKIREPLAY* r)
{
    WIKISIM* L = r->game;
    int i, n, fed;
    if (r->rw == WIKI_RW_IDLE) {
        if (L->tick >= r->end || L->done) return;
        fed = L->in[0].fed;
        tagpu_wikisim_step(L);
        if (r->scripted && in_hand(r, r->clock) && L->in[0].fed != fed) r->clash = 1;
    } else if (r->rw == WIKI_RW_PAST && r->target >= 0 && r->past) {
        n = r->target - 1 - r->past->tick < WIKI_REPLAY_SEEK ? r->target - 1 - r->past->tick : WIKI_REPLAY_SEEK;
        for (i = 0; i < n && !r->past->done && !r->failed; i++) past_tick(r);
        if (!r->failed && r->past->tick == r->target - 1) {
            r->target = -1;
            r->quiet = r->past->snd_total;
            r->dt = r->past->tick;
        }
    }
}

/* THE ARM's fill (tagpu_hudinput.c live_fill): out of the past it ends; the snap's seek done, it
   holds, with its toast; moved, it ends */
static void arm_fill(WIKIREPLAY* r)
{
    if (r->rw != WIKI_RW_PAST) r->arm = WIKI_ARM_NONE;
    else if (r->arm == WIKI_ARM_SEEKING && r->target < 0 && r->past->tick + 1 == r->arm_R) {
        r->arm = WIKI_ARM_ARMED;
        toast_of(r, WIKI_TOAST_ARMED, r->arm_R, r->arm_evict);
    } else if (r->arm == WIKI_ARM_ARMED && r->past->tick + 1 != r->arm_R) r->arm = WIKI_ARM_NONE;
}

/* the shown world's figures the top bar eases toward: its tick and seat 0's stored values,
   storage and rates (a game has a seat 0: its scene has one seat at least) */
static void shown_from(WIKIREPLAY* r, const WIKISIM* s)
{
    const WIKIECON* z = &s->econ[0];
    r->s_tick = s->tick;
    r->s_e = z->e;
    r->s_m = z->m;
    r->s_emax = z->emax;
    r->s_mmax = z->mmax;
    r->s_ei = z->ei;
    r->s_eo = z->eo;
    r->s_mi = z->mi;
    r->s_mo = z->mo;
}

/* one stored value of the memo eased toward `stored` and held to `cap` (bar_frame) */
static float ease(float old, double stored, double cap)
{
    const int o = (int)old, d = (int)stored - o;
    int step = d / 8;
    double v;
    if (!step && d) step = d > 0 ? 1 : -1;
    v = (double)(o + step);
    return (float)(v > cap ? cap : v);
}

/* THE TOP BAR's FRAME, DrawGameScreen's memo at main+0x37E3F, one frame a step (DISASSEMBLED,
   engine/render.md, The memo's layout): each stored value eased toward the shown world's,
   d = ftol(stored) - ftol(old) (ftol 0x4E43A0 sets the FPU to chop before fistp: truncation), a
   step of d / 8 truncated toward zero and at least 1 either way (0x468E7B..0x468F12), then held to
   the storage with no test of it being above 0 (0x468F14..0x468F6C); then once, when the shown
   record's DisplayTimer +0xF8 is below the GameTime, it rises by 30 and the four rates are read
   again (0x468F73..0x468FBF). MEASURED by M0 (m0-facts.md, 2026-11-01): the DisplayTimer is 30 at
   GameTime 1, 60 from 31, 90 from 61; the memo persists through a seek and a fork; in the past
   the rates are the ones sampled before the open (+25/-0 energy shown at 699 while the record
   held 50.15 of expense). The shown world is L's, or P's once its seek has ended; while a seek is
   pending, the last world drawn, which the stage holds. */
static void bar_frame(WIKIREPLAY* r)
{
    if (!(r->rw == WIKI_RW_PAST && (r->target >= 0 || !r->past))) shown_from(r, r->rw == WIKI_RW_PAST ? r->past : r->game);
    r->be = ease(r->be, r->s_e, r->s_emax);
    r->bm = ease(r->bm, r->s_m, r->s_mmax);
    if (r->dt < r->s_tick) {
        r->dt += WIKI_REPLAY_RATE;
        r->rates[0] = (float)r->s_ei;
        r->rates[1] = (float)r->s_eo;
        r->rates[2] = (float)r->s_mi;
        r->rates[3] = (float)r->s_mo;
    }
}

/* the viewer's script in the order it is fed: by tick, then file order (the pointers' order) */
static int by_tick(const void* a, const void* b)
{
    const WIKIINPUT* x = *(const WIKIINPUT* const*)a;
    const WIKIINPUT* y = *(const WIKIINPUT* const*)b;
    return x->at < y->at ? -1 : x->at > y->at ? 1 : x < y ? -1 : x > y;
}

int tagpu_wikireplay_init(WIKIREPLAY* r, const WIKIENTRY* e, const WIKIDEMO* d, int scripted, const WIKILAYOUT* lay,
                          const WIKISIMCTX* ctx, char* err, size_t errn)
{
    int i;
    memset(r, 0, sizeof *r);
    if (!d || !d->has_game) {
        snprintf(err, errn, "a replay plays a game entry's demo");
        return 0;
    }
    r->e = e;
    r->d = d;
    r->lay = lay;
    r->scripted = scripted != 0;
    r->has_ctx = ctx != NULL;
    if (ctx) r->ctx = *ctx;
    r->live = e->frame == WIKI_FRAME_GAME && e->live;
    if (!(r->game = game_new(r, 0, err, errn))) return 0;
    /* a live replay has no walker: the live HUD has no markers */
    if (!r->live && !(r->walker = game_new(r, 1, err, errn))) {
        game_free(r->game);
        r->game = NULL;
        return 0;
    }
    if (scripted && d->ninput) {
        if (!(r->script = (const WIKIINPUT**)malloc((size_t)d->ninput * sizeof *r->script))) {
            snprintf(err, errn, "wiki replay: out of memory");
            tagpu_wikireplay_free(r);
            return 0;
        }
        for (i = 0; i < d->ninput; i++) r->script[i] = &d->input[i];
        qsort((void*)r->script, (size_t)d->ninput, sizeof *r->script, by_tick);
        r->nscript = d->ninput;
    }
    r->vend = scripted && d->has_end ? d->end : INFINITY;
    r->end = (int)d->game.end;
    r->speed = WIKI_REPLAY_SPEED0;
    r->target = -1;
    r->gen = 1;
    r->pos = -1;
    r->toast_kind = -1;
    r->toast_evict = -1;
    /* the seats as the scene and the script's tick 0 leave them, a walk that has nothing to play,
       the clock's tick 0 */
    if (!r->live) {
        publish(r);
        if (r->walker->done) end_walk(r);
    } else {
        /* the original timeline; the memo before the level's first frame: seat 0's stored values
           (its settings), no rate read, the DisplayTimer 0 as in a record no frame has drawn
           (MEASURED, m0-facts.md #5) [INFERRED: the memo before a level's first frame is not
           measured; every read at GameTime 1 found the stored values, which this start shows at
           once] */
        r->tl[0].parent = -1;
        r->tl[0].R = WIKI_REPLAY_FIRST;
        r->tl[0].live = 1;
        r->tl[0].mode = WIKI_ECON_REFUSE;
        r->ntl = 1;
        r->made = 1;
        r->drag_t = -1;
        r->back = -1;
        r->arm_evict = -1;
        shown_from(r, r->game);
        r->be = (float)r->s_e;
        r->bm = (float)r->s_m;
    }
    feed(r, 0);
    r->done = is_done(r);
    return 1;
}

/* ONE STEP, a demo tick: while the walk runs, WIKI_REPLAY_WALK of its ticks and nothing else; then
   the viewer's clock (a held wait releases when no seek is pending and stalls past its max) and the
   game's part */
void tagpu_wikireplay_step(WIKIREPLAY* r)
{
    int i, n;
    if (r->done) return;
    r->tick++;
    if (r->walker) {
        n = r->end - r->walker->tick < WIKI_REPLAY_WALK ? r->end - r->walker->tick : WIKI_REPLAY_WALK;
        for (i = 0; i < n && !r->walker->done; i++) {
            tagpu_wikisim_step(r->walker);
            publish(r);
        }
        if (r->walker->done) end_walk(r);
        else r->walked = r->walker->tick;
        r->done = is_done(r);
        return;
    }
    if (r->waiting && r->target < 0) r->waiting = 0;
    else if (r->waiting && r->tick - r->wait_from > r->wait_max) r->stalled = 1;
    if (!r->waiting) feed(r, ++r->clock);
    if (r->live) {
        if (!r->failed) live_play(r);
        if (!r->failed) {
            arm_fill(r);
            bar_frame(r);
        }
    } else if (!r->failed) play(r);
    r->done = is_done(r);
}

int tagpu_wikireplay_cost(const WIKIREPLAY* r)
{
    int n;
    if (r->done) return 0;
    if (r->live) {
        if (r->rw == WIKI_RW_IDLE) return r->game->tick < r->end && !r->game->done;
        if (r->rw != WIKI_RW_PAST || r->target < 0 || !r->past) return 0;
        n = r->target - 1 - r->past->tick;
        return n < WIKI_REPLAY_SEEK ? n : WIKI_REPLAY_SEEK;
    }
    if (r->walker) return r->end - r->walker->tick < WIKI_REPLAY_WALK ? r->end - r->walker->tick : WIKI_REPLAY_WALK;
    if (r->target >= 0) return r->target - r->game->tick < WIKI_REPLAY_SEEK ? r->target - r->game->tick : WIKI_REPLAY_SEEK;
    if (r->paused) return 0;
    n = (r->acc + r->speed) / 10;
    return n < r->end - r->game->tick ? n : r->end - r->game->tick;
}

void tagpu_wikireplay_press(WIKIREPLAY* r, int control, int value)
{
    if (r->done || r->walker || control < 0 || control >= WIKI_NPRESS) return;
    if (r->live ? !tagpu_wikidata_hud_presses(control, 1)
                : !(control == WIKI_HUD_LEAVE_YES || tagpu_wikidata_hud_presses(control, 0)))
        return;
    act(r, control, value);
    r->done = is_done(r);
}

void tagpu_wikireplay_input(WIKIREPLAY* r, const WIKIINPUT* in)
{
    int kind, key = -1;
    double x = 0, z = 0;
    if (!r->live || r->scripted || r->done || r->selected == 0 || r->rw == WIKI_RW_PAST || !in) return;
    if (in->has & WIKI_IN_MOVE) {
        kind = WIKI_LOG_MOVE;
        x = in->move.x;
        z = in->move.z;
    } else if (in->down >= 0 && in->down < WIKI_NKEYS) {
        kind = WIKI_LOG_DOWN;
        key = in->down;
    } else if (in->up >= 0 && in->up < WIKI_NKEYS) {
        kind = WIKI_LOG_UP;
        key = in->up;
    } else return;
    if (r->rw == WIKI_RW_OPEN && kind != WIKI_LOG_MOVE) return;
    /* THE BOUND: idle keeps WIKI_REPLAY_LOG_KEEP back for the open's ups and the release, open one
       entry, the release's, in the count and in the block */
    if (r->nlogged >= (r->rw == WIKI_RW_OPEN ? WIKI_REPLAY_LOG - 1 : WIKI_REPLAY_LOG - WIKI_REPLAY_LOG_KEEP)) return;
    log_apply(r, kind, key, x, z, r->rw == WIKI_RW_OPEN);
    r->done = is_done(r);
}

WIKISIM* tagpu_wikireplay_game(WIKIREPLAY* r)
{
    return r->game;
}

WIKISIM* tagpu_wikireplay_past(WIKIREPLAY* r)
{
    return r->past;
}

void tagpu_wikireplay_view(const WIKIREPLAY* r, WIKIREPLAYVIEW* v)
{
    int i;
    const int press = r->at + WIKI_REPLAY_HUD_DOWN;
    const int release = r->drag >= 0 ? press + r->over : r->at + WIKI_REPLAY_HUD_UP;
    memset(v, 0, sizeof *v);
    v->tick = r->tick;
    v->clock = r->clock;
    v->done = r->done;
    v->waiting = r->waiting;
    v->stalled = r->stalled;
    v->caption = r->caption;
    v->failed = r->failed;
    v->ignored = r->ignored;
    v->game_tick = r->game ? r->game->tick : 0;
    v->end = r->end;
    v->playhead = r->target >= 0 ? r->target : v->game_tick;
    v->paused = r->paused;
    if (r->live) {
        v->playhead = now_tick(r);
        v->paused = r->rw != WIKI_RW_IDLE || r->game->tick >= r->end || r->game->done;
    }
    v->speed = r->speed;
    v->acc = r->acc;
    v->target = r->target;
    v->gen = r->gen;
    v->quiet = r->quiet;
    v->has_cursor = r->has_cursor;
    v->from = r->has_cursor ? r->from : -1;
    v->from_value = r->has_cursor ? r->from_value : -1;
    v->to = r->has_cursor ? r->to : -1;
    v->value = r->has_cursor ? r->value : -1;
    v->at = r->has_cursor ? r->at : 0;
    v->drag = r->has_cursor ? r->drag : -1;
    v->over = r->has_cursor ? r->over : 0;
    v->held = r->has_cursor && r->clock >= press && r->clock < release ? r->to : -1;
    v->nmark = r->nmark;
    memcpy(v->mark, r->mark, sizeof v->mark);
    v->walked = r->walked;
    v->nbook = r->nbook;
    memcpy(v->book, r->book, sizeof v->book);
    v->toast_kind = r->toast_kind;
    v->toast_at = r->toast_kind >= 0 ? r->toast_at : 0;
    v->toast_value = r->toast_kind >= 0 ? r->toast_value : 0;
    v->question = r->question;
    v->was_paused = r->question ? r->was_paused : 0;
    v->left = r->left;
    if (!(v->live = r->live)) return;
    v->rw = r->rw;
    v->G = r->G;
    v->first = WIKI_REPLAY_FIRST;
    v->last = r->last;
    v->now = now_tick(r);
    v->arm = r->arm;
    v->arm_R = r->arm_R;
    v->arm_evict = r->arm_evict;
    v->drag_t = r->drag_t;
    v->clash = r->clash;
    v->nlogged = r->nlogged;
    v->selected = r->selected;
    v->ntl = r->ntl;
    for (i = 0; i < r->ntl; i++) {
        v->tl[i].id = r->tl[i].id;
        v->tl[i].parent = r->tl[i].parent;
        v->tl[i].R = r->tl[i].R;
        v->tl[i].last = r->tl[i].last;
        v->tl[i].live = r->tl[i].live;
        v->tl[i].used = r->tl[i].used;
        v->tl[i].mode = r->tl[i].mode;
        v->tl[i].nlog = r->tl[i].nlog;
    }
    v->has_past = r->past != NULL;
    v->past_tick = r->past ? r->past->tick : 0;
    v->past_target = r->past ? r->target : -1;
    v->past_gen = r->past ? r->pgen : 0;
    v->be = r->be;
    v->bm = r->bm;
    memcpy(v->rates, r->rates, sizeof v->rates);
    v->dt = r->dt;
    v->back = r->back;
    v->full = r->nlogged >= WIKI_REPLAY_LOG - WIKI_REPLAY_LOG_KEEP;
    v->toast_evict = r->toast_kind >= 0 ? r->toast_evict : -1;
}

/* ---- the replay's hash, in tagpu_wikireplay.h's order ---- */

static void fnv_bytes(uint64_t* h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    while (n--) {
        *h ^= *b++;
        *h *= 0x100000001B3ull;
    }
}

static void fnv_u32(uint64_t* h, uint32_t u)
{
    unsigned char b[4];
    int i;
    for (i = 0; i < 4; i++) b[i] = (unsigned char)(u >> (8 * i));
    fnv_bytes(h, b, 4);
}

static void fnv_int(uint64_t* h, int v)
{
    fnv_u32(h, (uint32_t)v);
}

static void fnv_f32(uint64_t* h, float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    fnv_u32(h, u);
}

static void fnv_str(uint64_t* h, const char* str)
{
    const size_t n = str ? strlen(str) : 0;
    fnv_int(h, (int)n);
    fnv_bytes(h, str, n);
}

uint64_t tagpu_wikireplay_hash(const WIKIREPLAY* r)
{
    WIKIREPLAYVIEW v;
    uint64_t h = 0xCBF29CE484222325ull, g = 0;
    int i;
    tagpu_wikireplay_view(r, &v);
    fnv_int(&h, v.tick);
    fnv_int(&h, v.done ? 1 : 0);
    fnv_int(&h, v.clock);
    fnv_int(&h, v.waiting);
    fnv_int(&h, v.stalled ? 1 : 0);
    fnv_str(&h, v.caption);
    fnv_int(&h, v.failed ? 1 : 0);
    fnv_int(&h, v.ignored);
    fnv_int(&h, v.game_tick);
    fnv_int(&h, v.end);
    fnv_int(&h, v.paused ? 1 : 0);
    fnv_int(&h, v.speed);
    fnv_int(&h, v.acc);
    fnv_int(&h, v.target);
    fnv_int(&h, v.gen);
    fnv_int(&h, v.quiet);
    fnv_int(&h, v.has_cursor);
    if (v.has_cursor) {
        fnv_int(&h, v.from);
        fnv_int(&h, v.from_value);
        fnv_int(&h, v.to);
        fnv_int(&h, v.value);
        fnv_int(&h, v.at);
        fnv_int(&h, v.drag >= 0 ? 1 : 0);
        if (v.drag >= 0) {
            fnv_int(&h, v.drag);
            fnv_int(&h, v.over);
        }
    }
    fnv_int(&h, v.nbook);
    for (i = 0; i < v.nbook; i++) fnv_int(&h, v.book[i]);
    fnv_int(&h, v.toast_kind);
    fnv_int(&h, v.toast_at);
    fnv_int(&h, v.toast_value);
    fnv_int(&h, v.question ? 1 : 0);
    fnv_int(&h, v.was_paused ? 1 : 0);
    fnv_int(&h, v.left ? 1 : 0);
    fnv_int(&h, v.walked);
    fnv_int(&h, v.nmark);
    for (i = 0; i < v.nmark; i++) {
        fnv_int(&h, v.mark[i][0]);
        fnv_int(&h, v.mark[i][1]);
        fnv_int(&h, v.mark[i][2]);
    }
    if (r->game) g = tagpu_wikisim_hash(r->game);
    fnv_u32(&h, (uint32_t)(g & 0xFFFFFFFFu));
    fnv_u32(&h, (uint32_t)(g >> 32));
    if (!v.live) return h;
    fnv_int(&h, v.rw);
    fnv_int(&h, v.G);
    fnv_int(&h, v.now);
    fnv_int(&h, v.arm);
    fnv_int(&h, v.arm_R);
    fnv_int(&h, v.arm_evict);
    fnv_int(&h, v.drag_t);
    fnv_int(&h, v.clash ? 1 : 0);
    fnv_int(&h, v.nlogged);
    fnv_int(&h, v.selected);
    fnv_int(&h, v.ntl);
    for (i = 0; i < v.ntl; i++) {
        fnv_int(&h, v.tl[i].id);
        fnv_int(&h, v.tl[i].parent);
        fnv_int(&h, v.tl[i].R);
        fnv_int(&h, v.tl[i].last);
        fnv_int(&h, v.tl[i].live ? 1 : 0);
        fnv_int(&h, v.tl[i].used);
        fnv_int(&h, v.tl[i].mode);
        fnv_int(&h, v.tl[i].nlog);
    }
    fnv_int(&h, v.toast_evict);
    fnv_int(&h, v.back);
    fnv_int(&h, v.full ? 1 : 0);
    fnv_f32(&h, v.be);
    fnv_f32(&h, v.bm);
    for (i = 0; i < 4; i++) fnv_f32(&h, v.rates[i]);
    fnv_int(&h, v.dt);
    g = r->past ? tagpu_wikisim_hash(r->past) : 0;
    fnv_u32(&h, (uint32_t)(g & 0xFFFFFFFFu));
    fnv_u32(&h, (uint32_t)(g >> 32));
    return h;
}

void tagpu_wikireplay_free(WIKIREPLAY* r)
{
    int i;
    game_free(r->game);
    game_free(r->walker);
    game_free(r->past);
    for (i = 0; i < r->ntl; i++) free(r->tl[i].log);
    free((void*)r->script);
    memset(r, 0, sizeof *r);
}
