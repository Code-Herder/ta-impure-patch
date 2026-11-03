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
static int pl_act(WIKIREPLAY* r, int c, int value);
static int pick_act(WIKIREPLAY* r, int c, int value);
static int lane_act(WIKIREPLAY* r, int c, int value);
static int chain_step(WIKIREPLAY* r, WIKISIM* s);

/* the selected chain's last tick: a takeover replay's watch mode plays its selected timeline to
   that timeline's last */
static int chain_last(const WIKIREPLAY* r)
{
    return r->tk ? r->tl[r->selected].last : r->end;
}

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
    r->bg = 0;
}

/* THE SEEK, to `t` clamped to the game: back, a new game; forward, the target; to where it stands,
   none. Ending a seek in progress there makes what it played heard. */
static void seek(WIKIREPLAY* r, int t)
{
    char err[256];
    if (t < 0) t = 0;
    if (t > chain_last(r)) t = chain_last(r);
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
        r->pend = 1;
    } else if (t > r->game->tick) {
        r->target = t;
        r->pend = 1;
    } else {
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
    if (r->tk && (pick_act(r, c, value) || lane_act(r, c, value))) return;
    if (r->players && pl_act(r, c, value)) return;
    switch (c) {
    case WIKI_HUD_PLAY:
        if (playhead == chain_last(r)) {
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
        r->toast_evict = r->toast_seat = -1;
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
        if (c == r->at + WIKI_REPLAY_HUD_UP) act(r, r->to, r->value);
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
            r->from_watched = !from_game && r->has_cursor ? r->watched : 0;
            r->watched = r->selected;
            r->has_cursor = 1;
            r->to = e->hud;
            /* a trip's value: the track's tick, a picker row's or a players row's seat, a chip's
               WIKI_CHIP_*, a lane's fork, -1 for any other button */
            r->value = e->hud == WIKI_HUD_TRACK ? e->to
                     : e->hud == WIKI_HUD_TAKEAS || e->hud == WIKI_HUD_JUMP || e->hud == WIKI_HUD_SIGHT || e->hud == WIKI_HUD_FOLLOW ? e->seat
                     : e->hud == WIKI_HUD_CHIP ? e->chip : e->fork >= 0 ? e->fork : -1;
            r->at = c;
            r->drag = e->hud == WIKI_HUD_TRACK ? e->drag : -1;
            r->over = r->drag >= 0 ? e->over : 0;
            r->pos = r->value;
        } else r->caption = e->caption;
    }
}

static void make_pass(WIKIREPLAY* r, int g);

/* THE PASS'S DEADLINE, the viewed record's +0xF0 (replay.js passDue states the engine's rule and
   why it is the next multiple of 30 above the last pass's tick, 30 before any): the first played
   tick at or past it makes the pass. pass_at is -1 or a shown tick, at most the game's end, so
   the product cannot overflow. */
static int pass_due(const WIKIREPLAY* r)
{
    return r->pass_at < 0 ? WIKI_REPLAY_PASS : WIKI_REPLAY_PASS * (r->pass_at / WIKI_REPLAY_PASS + 1);
}

/* the game's part of a step: a seek's ticks, else the speed's, along the selected chain; the ticks
   it ran. A players replay's pass is made on each played tick that reaches its deadline, at that
   tick's world, as the engine's players phase runs it every tick; a seek's ticks make none, its
   end does (pl_tail). */
static int play(WIKIREPLAY* r)
{
    WIKISIM* g = r->game;
    int ran = 0, n;
    if (r->target >= 0) {
        n = r->target - g->tick < WIKI_REPLAY_SEEK ? r->target - g->tick : WIKI_REPLAY_SEEK;
        for (; ran < n && !g->done && chain_step(r, g); ran++) {}
        if (!r->failed && g->tick == r->target) {
            r->target = -1;
            r->quiet = g->snd_total;
        }
    } else if (!r->paused) {
        r->acc += r->speed;
        while (r->acc >= 10 && g->tick < chain_last(r) && !g->done && chain_step(r, g)) {
            r->acc -= 10;
            ran++;
            if (r->players && g->tick >= pass_due(r)) {
                make_pass(r, g->tick);
                if (r->failed) break;
            }
        }
        if (g->tick == chain_last(r)) {
            r->paused = 1;
            r->acc = 0;
        }
    }
    return ran;
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

/* a log entry applied to a simulation, by the call that applied it live: an input of its seat's as
   Try it gives one (tagpu_wiki.c try_send), or its seat's release */
static void apply(WIKISIM* s, const WIKIREPLAYLOG* x)
{
    WIKIINPUT in;
    if (x->kind == WIKI_LOG_RELEASE) {
        tagpu_wikisim_release(s, x->seat);
        return;
    }
    memset(&in, 0, sizeof in);
    in.down = in.up = in.build = in.fork = -1;
    in.seat = x->seat;
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

/* LOGGED FIRST (tagpu_wikireplay.h): the entry, the human seat's, joins the selected timeline's
   log, then L takes it, with `spare` more entries left in the block; 0, nothing applied, when
   memory runs out. The callers keep WIKI_REPLAY_LOG's bound. */
static int log_apply(WIKIREPLAY* r, int kind, int key, double x, double z, int spare)
{
    WIKITIMELINE* t = &r->tl[r->selected];
    WIKIREPLAYLOG* e;
    if (!log_room(t, 1 + spare)) return 0;
    e = &t->log[t->nlog++];
    e->tau = r->game->tick + 1;
    e->kind = (int8_t)kind;
    e->key = (int8_t)key;
    e->seat = (int8_t)r->human;
    e->x = x;
    e->z = z;
    r->nlogged++;
    apply(r->game, e);
    return 1;
}

/* a toast; `seat` names a picker row's seat (Taking over NAME), -1 none */
static void toast_of(WIKIREPLAY* r, int kind, int value, int evict, int seat)
{
    r->toast_kind = kind;
    r->toast_at = r->tick;
    r->toast_value = value;
    r->toast_evict = evict;
    r->toast_seat = seat;
}

/* a plan: the fork's first tick, its parent's id, the fork it evicts (-1 none); refused, `next`
   the tick it may be made again at (0 none) */
typedef struct {
    int R, parent, evict, next;
} PLAN;

/* THE PLAN (tagpu_replay.c takeover_point, tagpu_recreader.c recr_append_point): a fork asked for at
   GameTime T starts in the chain member holding T, at the last GameTime of the member's segment up
   to T that the recorder's writer flags a refresh (tagpu_wikidata_refresh_at, walked from the
   member's first with the ordinal from 1); the member's first is refused, with `next` its second
   (a refresh: T is the segment's first iteration, and the picker plans again there), as is a
   timeline past RECR_TIMELINES and a live fork set rec_fork_evict cannot bound. At most the game's
   end of integer steps: it runs on a click, never per step. */
static int plan(const WIKIREPLAY* r, int T, PLAN* p)
{
    int chain[RECR_TIMELINES], n = chain_of(r, r->selected, chain), k = n - 1, i;
    RecForkRow rows[RECR_TIMELINES];
    uint32_t evict;
    const WIKITIMELINE* m;
    while (k > 0 && r->tl[chain[k]].R > T) k--;
    m = &r->tl[chain[k]];
    p->R = tagpu_wikidata_refresh_at(m->R, T);
    p->next = p->R == m->R ? m->R + 1 : 0;
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
        toast_of(r, WIKI_TOAST_REFUSED, 0, -1, -1);
        return;
    }
    memcpy(held, L->in[r->human].held, sizeof held);
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

/* A CHAIN MEMBER'S FORK OPERATION on simulation s standing at m->R - 1, as the timeline was made:
   m's part, played as m's seat, in m's mode. Refused -- the replay fails -- when that seat is no
   seat of the game's or has no unit there (the picker draws such a seat's row dimmed, with no hot
   area: tagpu_hudlay.c) or the fork operation refuses. */
static int fork_member(WIKIREPLAY* r, WIKISIM* s, const WIKITIMELINE* m)
{
    char err[256];
    if (m->seat < 0 || m->seat >= s->nseat || m->seat >= WIKI_SEATS || s->econ[m->seat].units == 0
        || !tagpu_wikisim_fork(s, m->R, m->seat, m->part, m->npart, m->mode, err, sizeof err)) {
        r->failed = 1;
        return 0;
    }
    return 1;
}

/* ONE TICK OF P along the chain, from t to t + 1: at the next member's R the member's fork
   operation, then the entries that member logged at t + 1, in log order, through the calls that
   applied them live, so P's world is the world the timeline was played with by construction */
static void past_tick(WIKIREPLAY* r)
{
    WIKISIM* P = r->past;
    const int t = P->tick;
    const WIKITIMELINE* m;
    if (r->pm + 1 < r->npchain && t + 1 == r->tl[r->pchain[r->pm + 1]].R) {
        r->pl = 0;
        if (!fork_member(r, P, &r->tl[r->pchain[++r->pm]])) return;
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

/* THE NEXT FORK'S ROW, `t`, of id r->ntl, made at R0 from `parent` as `seat`: its part, the one a
   scripted demo gives that id -- its from, parent and seat must be the fork's -- or Try it's none
   in stall mode, the economy's stall mode's caller. 0, the replay failed, when the part is not
   there. The id is below RECR_TIMELINES: the plan refused the one past it. */
static int part_of(WIKIREPLAY* r, int R0, int parent, int seat, WIKITIMELINE* t)
{
    int i;
    memset(t, 0, sizeof *t);
    t->id = r->ntl;
    t->parent = parent;
    t->R = R0;
    t->last = R0 - 1;
    t->live = 1;
    t->used = r->tick;
    t->seat = seat;
    t->mode = WIKI_ECON_STALL;
    if (!r->scripted) return 1;
    for (i = 0; i < r->d->nfork; i++)
        if (r->d->fork[i].id == t->id) {
            const WIKIFORK* f = &r->d->fork[i];
            if (f->from != R0 || f->parent != parent || f->seat != seat) break;
            t->part = f->input;
            t->npart = f->ninput;
            t->mode = WIKI_ECON_REFUSE;
            return 1;
        }
    r->failed = 1;
    return 0;
}

/* THE FORK, with P standing at p->R - 1 and no seek pending: the part of the next id (part_of),
   then the fork operation as the human seat, which ends with the release; the selected row closed
   at G, the evicted one deleted, the new row selected, and P takes L's place (THE LIFETIME). The
   game's fork sets the DisplayTimer to the GameTime shown (MEASURED, m0-facts.md: 1200 at the
   fork that takes over at 1201). */
static void fork_at(WIKIREPLAY* r, const PLAN* p)
{
    WIKISIM* P = r->past;
    WIKITIMELINE t;
    if (!part_of(r, p->R, p->parent, r->human, &t) || !fork_member(r, P, &t)) return;
    r->dt = p->R - 1;
    r->tl[r->selected].last = r->G;
    if (p->evict >= 0) r->tl[p->evict].live = 0;
    r->tl[t.id] = t;
    r->ntl++;
    r->selected = t.id;
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
    toast_of(r, WIKI_TOAST_PLAYING_FROM, p->R, -1, -1);
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
        toast_of(r, WIKI_TOAST_PLAYING_ON, r->G, -1, -1);
        return;
    }
    if (r->rw != WIKI_RW_PAST || r->arm == WIKI_ARM_SEEKING) return;
    if (!plan(r, now + 1, &p)) {
        r->arm = WIKI_ARM_NONE;
        toast_of(r, WIKI_TOAST_CANNOT, now, -1, -1);
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
        fed = L->in[r->human].fed;
        tagpu_wikisim_step(L);
        if (r->scripted && in_hand(r, r->clock) && L->in[r->human].fed != fed) r->clash = 1;
    } else if (r->rw == WIKI_RW_PAST && r->target >= 0 && r->past) {
        n = r->target - 1 - r->past->tick < WIKI_REPLAY_SEEK ? r->target - 1 - r->past->tick : WIKI_REPLAY_SEEK;
        for (i = 0; i < n && !r->past->done && !r->failed; i++) past_tick(r);
        if (!r->failed && r->past->tick == r->target - 1) {
            r->target = -1;
            r->quiet = r->past->snd_total;
            r->dt = r->past->tick;
            r->click_from = r->past->tick;
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
        toast_of(r, WIKI_TOAST_ARMED, r->arm_R, r->arm_evict, -1);
    } else if (r->arm == WIKI_ARM_ARMED && r->past->tick + 1 != r->arm_R) r->arm = WIKI_ARM_NONE;
}

/* the shown world's figures the top bar eases toward: its tick and the human seat's stored values,
   storage and rates (a seat of the scene's: a live entry's 0, which a scene has, or the seat a
   takeover's row named, bounded by the picker) */
static void shown_from(WIKIREPLAY* r, const WIKISIM* s)
{
    const WIKIECON* z = &s->econ[r->human];
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

/* ---- TAKE OVER (tagpu_wikireplay.h, TAKE OVER; replay.js's functions, under its names) ---- */

/* ONE TICK of simulation s along the selected chain: at a member's R its fork operation first. R
   strictly increases along a chain (a fork starts after its parent's first), so at most one member
   starts on the tick after s's. 0, nothing stepped, when that fork operation failed the replay. */
static int chain_step(WIKIREPLAY* r, WIKISIM* s)
{
    int chain[RECR_TIMELINES], n, k;
    if (r->tk) {
        n = chain_of(r, r->selected, chain);
        for (k = 1; k < n; k++)
            if (r->tl[chain[k]].R == s->tick + 1) {
                if (!fork_member(r, s, &r->tl[chain[k]])) return 0;
                break;
            }
    }
    tagpu_wikisim_step(s);
    return 1;
}

/* a fork the file holds: a timeline's id other than the original's, not deleted. `id` is DATA (a
   press's value), bounded by the rows before it indexes them. */
static int live_fork(const WIKIREPLAY* r, int id)
{
    return id >= 1 && id < r->ntl && r->tl[id].live;
}

static int any_fork(const WIKIREPLAY* r)
{
    int i;
    for (i = 1; i < r->ntl; i++)
        if (r->tl[i].live) return 1;
    return 0;
}

/* the selected chain's tick the stage's track fills to: the R of its member whose parent is the
   original, else the original's end (tagpu_hudstate.c fill_forks' fill_to) */
static int fill_to(const WIKIREPLAY* r)
{
    int chain[RECR_TIMELINES], n = chain_of(r, r->selected, chain), k;
    for (k = 1; k < n; k++)
        if (r->tl[chain[k]].parent == 0) return r->tl[chain[k]].R;
    return r->end;
}

/* THE WALK RESTARTED on the selected chain (THE LIFETIME, WATCH: the old walker freed before the
   new one is made), the markers blank until it has walked, in the background (THE BOUND, WATCH) */
static void restart_walk(WIKIREPLAY* r)
{
    char err[256];
    game_free(r->walker);
    r->walker = NULL;
    r->nmark = 0;
    memset(r->prev_lost, 0, sizeof r->prev_lost);
    memset(r->prev_out, 0, sizeof r->prev_out);
    r->walked = 0;
    if (!(r->walker = game_new(r, 1, err, sizeof err))) {
        r->failed = 1;
        return;
    }
    r->bg = 1;
    publish(r);
}

/* the background walk's share of a step that ran `ran` of the game's ticks (THE BOUND, WATCH) */
static void bg_walk(WIKIREPLAY* r, int ran)
{
    int i, n;
    if (!r->bg || !r->walker) return;
    n = WIKI_REPLAY_WALK - ran < chain_last(r) - r->walker->tick ? WIKI_REPLAY_WALK - ran : chain_last(r) - r->walker->tick;
    for (i = 0; i < n && !r->walker->done && chain_step(r, r->walker); i++) publish(r);
    if (r->failed) return;
    if (r->walker->done || r->walker->tick >= chain_last(r)) end_walk(r);
    else r->walked = r->walker->tick;
}

/* THE SWITCH to timeline `id`, a live one (tagpu_replay.c, the switch): the timeline left stamped
   as used, the picker closed; RE-CHAINED when the chain members the game has crossed (R <= its
   tick) are the same on both chains -- the world at that tick was made by the same fork operations
   and the game's script is the last such member's, so it is the new chain's world by construction
   -- the game kept and a pending seek clamped to the new chain; else SOUGHT to the playhead
   clamped to the new chain, the game freed before the new one is made (THE LIFETIME, WATCH); then
   the walk restarted on the new chain */
static void switch_to(WIKIREPLAY* r, int id)
{
    char err[256];
    int a[RECR_TIMELINES], b[RECR_TIMELINES], na, nb, ka = 0, kb = 0, same, T;
    const int t = r->game->tick;
    r->tl[r->selected].used = r->tick;
    r->pick = WIKI_PICKER_CLOSED;
    na = chain_of(r, r->selected, a);
    nb = chain_of(r, id, b);
    while (ka < na && r->tl[a[ka]].R <= t) ka++;
    while (kb < nb && r->tl[b[kb]].R <= t) kb++;
    same = ka == kb && !memcmp(a, b, (size_t)ka * sizeof a[0]);
    if (same) {
        if (r->target > r->tl[id].last) r->target = r->tl[id].last;
    } else {
        T = r->target >= 0 ? r->target : t;
        if (T > r->tl[id].last) T = r->tl[id].last;
        game_free(r->game);
        r->game = NULL;
        if (!(r->game = game_new(r, 0, err, sizeof err))) {
            r->failed = 1;
            return;
        }
        r->gen++;
        r->quiet = 0;
        r->acc = 0;
        r->target = T == 0 ? -1 : T;
    }
    r->selected = id;
    restart_walk(r);
}

/* A DELETION of fork `id`, a live one (tagpu_replay.c, the delete): the watched fork is left
   first for the nearest live timeline up its chain (fork_fallback: the original is always live,
   so the walk ends there), then the fork is gone and the rows' serial moves; the bay closes with
   its last fork, and a deleted row's part goes unless a live chain reads it (sweep) */
static void delete_fork(WIKIREPLAY* r, int id)
{
    int f;
    if (id == r->selected) {
        for (f = r->tl[id].parent; f > 0 && !r->tl[f].live; f = r->tl[f].parent) {}
        switch_to(r, f < 0 ? 0 : f);
        if (r->failed) return;
    }
    r->tl[id].live = 0;
    r->serial++;
    if (!any_fork(r)) r->lanes = 0;
    sweep(r);
}

/* THE TAKEOVER of seat h, with the picker open: the game stands at pick_R - 1, paused, no seek
   pending (pick_fill keeps that). The part of the next id (part_of) and the fork operation as seat
   h, in the walker's stead (THE LIFETIME, WATCH); the timeline watched stamped as left [INFERRED:
   the playing's stamps are written when it ends, tagpu_replay.c, and read only by a later
   eviction in the live game, which no demo reaches]; the evicted fork deleted, the new row
   selected; then LIVE MODE with seat h the human, the cursor's glide back from the row to seat h's
   cursor starting now. THE BAR AT THE TAKEOVER: the memo is carried, not reset, so it eases from
   the watch mode's zeros (the viewer's) toward seat h's stored values. MEASURED (m0t-facts.md #1f,
   p0m-facts.md F1, 2026-11-01): at R - 1 the memo holds the viewer's zeros; the bars grabbed about
   1 s after a running takeover read 6893 / 6917 on their way to seat h's 9861 / 9894, and 0.3 s
   later the memo held those; the frames between were not read, so the ease itself is [INFERRED]
   from the memo's own rule. The DisplayTimer is R - 1, so the step that plays R samples seat h's
   rates (MEASURED: from R + 1) and raises it to R + 29. The game after the fix that
   runs a takeover's live game at once (takeover-pause-fix-spec.md) draws the takeover's own frame
   at R - 1 before its first batch of ticks; the wiki's first live frame is R's. */
static void take_over(WIKIREPLAY* r, int h)
{
    WIKITIMELINE t;
    int before = 0, i;
    if (!part_of(r, r->pick_R, r->pick_parent, h, &t)) return;
    game_free(r->walker);
    r->walker = NULL;
    r->bg = 0;
    for (i = 1; i < r->ntl; i++) before += r->tl[i].live != 0;
    if (!fork_member(r, r->game, &t)) return;
    r->tl[r->selected].used = r->tick;
    if (r->pick_evict >= 0) r->tl[r->pick_evict].live = 0;
    r->tl[t.id] = t;
    r->ntl++;
    r->selected = t.id;
    sweep(r);
    r->live = 1;
    r->human = h;
    r->rw = WIKI_RW_IDLE;
    r->arm = WIKI_ARM_NONE;
    r->G = 0;
    r->back = r->clock;
    r->tk_full = before >= REC_FORK_LIMIT;
    r->quiet = r->game->snd_total;
    r->dt = r->pick_R - 1;
    toast_of(r, WIKI_TOAST_TAKING_OVER, r->pick_R - 1, -1, h);
}

/* THE PICKER'S KEYS (tagpu_hudinput.c pick_act): 1 when the press is one of them. rew, fwd, play
   and the track close the picker and go on to act; Take over asks, pausing the playback, or
   closes; a row acts only with the picker open and on a seat with a unit (a dimmed row has no hot
   area), else it is ignored and counted. The row's seat is DATA, bounded by the game's seats. */
static int pick_act(WIKIREPLAY* r, int c, int value)
{
    if (c == WIKI_HUD_REW || c == WIKI_HUD_FWD || c == WIKI_HUD_PLAY || c == WIKI_HUD_TRACK) {
        r->pick = WIKI_PICKER_CLOSED;
        return 0;
    }
    if (c == WIKI_HUD_TAKEOVER) {
        if (r->pick != WIKI_PICKER_CLOSED) r->pick = WIKI_PICKER_CLOSED;
        else {
            r->paused = 1;
            r->pick = WIKI_PICKER_ASKED;
        }
        return 1;
    }
    if (c != WIKI_HUD_TAKEAS) return 0;
    if (r->pick != WIKI_PICKER_OPEN || value < 0 || value >= r->game->nseat || value >= WIKI_SEATS || r->game->econ[value].units == 0)
        r->ignored++;
    else {
        r->pick = WIKI_PICKER_CLOSED;
        take_over(r, value);
    }
    return 1;
}

/* THE LANES' KEYS (tagpu_hudinput.c lane_act): 1 when the press is one of them. The bay toggles the
   lanes while the file holds a fork; a lane, its Save and its Delete act on a fork the open lanes
   list (the id is DATA, bounded by live_fork), and Back to main recording while a fork is watched;
   anything else is ignored and counted. A lane and Back to main clear Delete's arm; the watched
   lane does nothing. Save writes a fork once (THE BOUND: each id once, below RECR_TIMELINES);
   Delete arms, then deletes. */
static int lane_act(WIKIREPLAY* r, int c, int value)
{
    int i;
    if (c == WIKI_HUD_LANES) {
        if (any_fork(r)) r->lanes ^= 1;
        else r->ignored++;
        return 1;
    }
    if (c == WIKI_HUD_MAIN) {
        if (r->lanes && r->selected != 0) {
            r->armed = 0;
            switch_to(r, 0);
        } else r->ignored++;
        return 1;
    }
    if (c != WIKI_HUD_LANE && c != WIKI_HUD_EXPORT && c != WIKI_HUD_DELETE) return 0;
    if (!r->lanes || !live_fork(r, value)) {
        r->ignored++;
        return 1;
    }
    if (c == WIKI_HUD_LANE) {
        r->armed = 0;
        if (value != r->selected) switch_to(r, value);
    } else if (c == WIKI_HUD_EXPORT) {
        for (i = 0; i < r->nsaved && r->saved[i] != value; i++) {}
        if (i < r->nsaved) toast_of(r, WIKI_TOAST_CANNOT_SAVE, value, -1, -1);
        else {
            r->saved[r->nsaved++] = value;
            toast_of(r, WIKI_TOAST_SAVED, value, -1, -1);
        }
    } else if (r->armed != value) {
        r->armed = value;
        toast_of(r, WIKI_TOAST_CLICK_DELETE, value, -1, -1);
    } else {
        r->armed = 0;
        delete_fork(r, value);
        toast_of(r, WIKI_TOAST_DELETED, value, -1, -1);
    }
    return 1;
}

/* THE PICKER'S FILL (tagpu_hudinput.c pick_fill), after the game's part of each watch step, the
   playhead p the seek's target while one is pending: asked, it plans at p + 1 (the chain's last at
   most), once more one tick on when that is a segment's first, and snaps the playback back to
   R - 1; sought, it opens when the game stands at R - 1; open, it closes when the playback runs or
   the playhead leaves R - 1 (every wiki seek completes, and every press that would start another
   closes the picker first). A switch closes it; a deletion plans it again at R, which keeps it
   with the new eviction or closes it. */
static void pick_fill(WIKIREPLAY* r)
{
    const int p = r->target >= 0 ? r->target : r->game->tick;
    PLAN q;
    int ok;
    if (r->pick == WIKI_PICKER_ASKED) {
        ok = plan(r, p + 1 < chain_last(r) ? p + 1 : chain_last(r), &q);
        if (!ok && q.next) ok = plan(r, q.next, &q);
        if (!ok) {
            toast_of(r, WIKI_TOAST_CANNOT_TAKE, p, -1, -1);
            r->pick = WIKI_PICKER_CLOSED;
            return;
        }
        r->pick_R = q.R;
        r->pick_evict = q.evict;
        r->pick_parent = q.parent;
        r->pick_tl = r->selected;
        r->pick_serial = r->serial;
        if (!(r->target < 0 && r->game->tick + 1 == r->pick_R)) seek(r, r->pick_R - 1);
        if (r->failed) return;
        r->pick = WIKI_PICKER_SEEKING;
    }
    if (r->pick == WIKI_PICKER_SEEKING) {
        if (r->target < 0 && r->game->tick + 1 == r->pick_R) r->pick = WIKI_PICKER_OPEN;
    } else if (r->pick == WIKI_PICKER_OPEN && (!r->paused || r->target >= 0 || r->game->tick + 1 != r->pick_R))
        r->pick = WIKI_PICKER_CLOSED;
    if (r->pick != WIKI_PICKER_CLOSED && r->selected != r->pick_tl) r->pick = WIKI_PICKER_CLOSED;
    if ((r->pick == WIKI_PICKER_SEEKING || r->pick == WIKI_PICKER_OPEN) && r->serial != r->pick_serial) {
        r->pick_serial = r->serial;
        if (plan(r, r->pick_R, &q) && q.R == r->pick_R) r->pick_evict = q.evict;
        else {
            toast_of(r, WIKI_TOAST_CANNOT_TAKE, r->pick_R - 1, -1, -1);
            r->pick = WIKI_PICKER_CLOSED;
        }
    }
}

/* THE ARM's hold (tagpu_hudinput.c lane_fill): only while the lanes are open and list its fork */
static void lane_fill(WIKIREPLAY* r)
{
    if (r->armed && (!r->lanes || !live_fork(r, r->armed))) r->armed = 0;
}

/* ---- PLAYERS (tagpu_wikireplay.h, PLAYERS; replay.js's functions, under its names) ---- */

/* the demo's scene, whose seats the Players set names */
static const WIKISCENE* scene_of(const WIKIREPLAY* r)
{
    return &r->e->scenes[r->d->on];
}

/* seat k's camera at game tick t into *x, *z: 1, or 0 when its camera was not recorded
   (tagpu_wikidata_seat_cam, replay.js camOf) */
static int cam_of(const WIKIREPLAY* r, int k, int t, int* x, int* z)
{
    return tagpu_wikidata_seat_cam(scene_of(r), &r->d->game, k, t, x, z);
}

/* a Players press's value the door takes (replay.js plValue): a row's seat one of the scene's, a
   chip a WIKI_CHIP_*, a team row's fold below WIKI_SEATS. The value is DATA: the press's or the
   script's, bounded here before anything indexes by it. */
static int pl_value(const WIKIREPLAY* r, int c, int value)
{
    switch (c) {
    case WIKI_HUD_JUMP:
    case WIKI_HUD_SIGHT:
    case WIKI_HUD_FOLLOW: return value >= 0 && value < scene_of(r)->nseat && value < WIKI_SEATS;
    case WIKI_HUD_CHIP: return value >= 0 && value < WIKI_NCHIP;
    case WIKI_HUD_FOLD: return value >= 0 && value < WIKI_SEATS;
    default: return 1;
    }
}

/* the teams are told apart (replay.js HEADED, tagpu_hudlay_headed): two teams or more, one of
   them of two seats or more, seat k's team the scene's (the loader gives an absent one k), as
   the record's teams are (tagpu_wikiframe.c pl_block). Otherwise the HUD draws the teams chip
   disabled, with no hot box, so the game takes no press on it. BOUND: the seats counted are the
   game's, at most WIKI_SEATS, and a team value is the loader's 0..WIKI_SEATS - 1. */
static int teams_headed(const WIKIREPLAY* r)
{
    const WIKISCENE* sc = scene_of(r);
    int n[WIKI_SEATS] = { 0 }, k, t, nteam = 0, big = 0;
    for (k = 0; k < r->game->nseat && k < WIKI_SEATS; k++) {
        t = k < sc->nseat ? sc->seat[k].team : k;
        if (t < 0 || t >= WIKI_SEATS) t = k;
        nteam += n[t]++ == 0;
        big |= n[t] > 1;
    }
    return nteam > 1 && big;
}

/* THE PLAYERS SET's KEYS (tagpu_hudinput.c act; replay.js plAct states the rules): 1 when the
   press is one of them; a value outside pl_value is ignored and counted. */
static int pl_act(WIKIREPLAY* r, int c, int value)
{
    int x, z;
    if (!tagpu_wikidata_hud_players(c) && c != WIKI_HUD_FOLD) return 0;
    if (!pl_value(r, c, value)) {
        r->ignored++;
        return 1;
    }
    switch (c) {
    case WIKI_HUD_JUMP:
    case WIKI_HUD_FOLLOW:
        if (!cam_of(r, value, 0, &x, &z)) {
            r->toast_kind = WIKI_TOAST_CAMERA;
            r->toast_at = r->tick;
            r->toast_value = value;
            r->toast_evict = r->toast_seat = -1;
        } else if (c == WIKI_HUD_JUMP) {
            r->jump_req = value;
            r->follow = -1;
        } else {
            r->follow = r->follow == value ? -1 : value;
            r->jump_req = -1;
        }
        break;
    case WIKI_HUD_SIGHT: r->sight_req = r->sight == value ? -1 : value; break;
    case WIKI_HUD_CURSORS: r->tog ^= HUD_TOG_CURSORS; break;
    case WIKI_HUD_CLICKS: r->tog ^= HUD_TOG_CLICKS; break;
    case WIKI_HUD_SELECTIONS: r->tog ^= HUD_TOG_SELECTIONS; break;
    case WIKI_HUD_CHAT: r->tog ^= HUD_TOG_CHAT; break;
    case WIKI_HUD_STATS: r->stat_open = !r->stat_open; break;
    case WIKI_HUD_CLOSE: r->stat_open = 0; break;
    case WIKI_HUD_CHIP:
        if (value >= WIKI_CHIP_METAL) r->stat_measure = value - WIKI_CHIP_METAL;
        else if (value == WIKI_CHIP_PLAYER || teams_headed(r)) r->stat_team = value == WIKI_CHIP_TEAM;
        break;
    default: /* WIKI_HUD_FOLD */
        r->folded ^= 1 << value;
        break;
    }
    return 1;
}

/* THE BAR'S RE-READ DUE at a Sight change, tagpu_replay.c bar_rates_due copied (replay.js ratesDue
   states it): record v's DisplayTimer lowered to the last deadline of its own 30-tick phase below
   `now`. Each subtraction is bounded first: the timer is at least `now` where `back` is taken, and
   `back` is subtracted only from a `now` at least as large. `v` is a seat below WIKI_SEATS or the
   watcher's WIKI_SEATS: the callers bound it. */
static void rates_due(WIKIREPLAY* r, int v, int now)
{
    int back;
    if (!now || r->pdt[v] < now) return;
    back = WIKI_REPLAY_RATE - (r->pdt[v] - now) % WIKI_REPLAY_RATE;
    r->pdt[v] = now >= back ? now - back : 0;
}

/* THE PASS at shown tick g (replay.js makePass states the engine's rule): the viewed seat's lit grid
   over the layout's sight table, a fog bit per block, and the ids of the units its contacts mark,
   from the game's world, which stands at g; empty under the full map. BOUND: the grid is
   tagpu_wikisim_sight_lit's, at most WIKI_SIGHT_GRID blocks, so its bits fit pass_fog; the ids are
   the view's units' at most, each copied within WIKI_ID_MAX. LIFETIME: the id block is the
   replay's, grown here and freed by tagpu_wikireplay_free. Memory running out fails the replay. */
static void make_pass(WIKIREPLAY* r, int g)
{
    uint8_t lit[WIKI_SIGHT_GRID];
    const WIKIVIEW* v;
    uint8_t* con;
    int n, i, k;
    r->pass_at = g;
    r->pass_blocks = 0;
    r->npass_id = 0;
    memset(r->pass_fog, 0, sizeof r->pass_fog);
    if (r->sight < 0) return;
    v = tagpu_wikisim_view(r->game);
    n = tagpu_wikisim_sight_lit(v, r->lay ? &r->lay->sight : NULL, r->sight, g, lit);
    for (i = 0; i < n; i++)
        if (lit[i]) r->pass_fog[i >> 3] |= (uint8_t)(1u << (i & 7));
    r->pass_blocks = n;
    if (!v->nunit) return;
    if (!(con = (uint8_t*)malloc((size_t)v->nunit))) {
        r->failed = 1;
        return;
    }
    tagpu_wikisim_contacts(v, lit, r->sight, con);
    for (i = k = 0; i < v->nunit; i++) k += con[i] != 0;
    if (k > r->cpass_id) {
        char (*p)[WIKI_ID_MAX] = (char (*)[WIKI_ID_MAX])realloc(r->pass_id, (size_t)k * sizeof *p);
        if (!p) {
            free(con);
            r->failed = 1;
            return;
        }
        r->pass_id = p;
        r->cpass_id = k;
    }
    for (i = 0; i < v->nunit; i++)
        if (con[i]) snprintf(r->pass_id[r->npass_id++], WIKI_ID_MAX, "%s", v->unit[i].id ? v->unit[i].id : "");
    free(con);
}

/* EACH STEP'S TAIL (replay.js plTail states the order and why), after the game's part, every step,
   paused or not, at the shown tick: the camera; with no seek pending a seek's end and a Sight
   request, each making the pass (every other pass is play's, at its deadline); the top bar. The
   seats it indexes are bounded: Follow's and Jump's by pl_value and the camera's presence,
   Sight's by pl_value, so below the scene's seats. */
static void pl_tail(WIKIREPLAY* r)
{
    int g, x, z, v, due = 0;
    if (r->target < 0) r->shown = r->game->tick;
    g = r->shown;
    if (r->jump_req >= 0) {
        if (cam_of(r, r->jump_req, g, &x, &z)) {
            r->cam_x = x;
            r->cam_z = z;
        }
        r->jump_req = -1;
    } else if (r->follow >= 0 && cam_of(r, r->follow, g, &x, &z)) {
        r->cam_x = x;
        r->cam_z = z;
    }
    if (r->target < 0) {
        if (r->pend) {
            for (v = 0; v <= WIKI_SEATS; v++) r->pdt[v] = g;
            r->click_from = g;
            r->pend = 0;
            due = 1;
        }
        if (r->sight_req != WIKI_REPLAY_NOT_ASKED) {
            if (r->sight_req != r->sight) {
                r->sight = r->sight_req;
                rates_due(r, r->sight >= 0 ? r->sight : WIKI_SEATS, g);
                due = 1;
            }
            r->sight_req = WIKI_REPLAY_NOT_ASKED;
        }
        if (due) make_pass(r, g);
        if (r->failed) return;
        if (r->sight >= 0 && r->sight < r->game->nseat) {
            const WIKIECON* e = &r->game->econ[r->sight];
            r->p_e = e->e;
            r->p_m = e->m;
            r->p_emax = e->emax;
            r->p_mmax = e->mmax;
            r->p_ei = e->ei;
            r->p_eo = e->eo;
            r->p_mi = e->mi;
            r->p_mo = e->mo;
        } else r->p_e = r->p_m = r->p_emax = r->p_mmax = r->p_ei = r->p_eo = r->p_mi = r->p_mo = 0;
    }
    r->pbe = ease(r->pbe, r->p_e, r->p_emax);
    r->pbm = ease(r->pbm, r->p_m, r->p_mmax);
    v = r->sight >= 0 ? r->sight : WIKI_SEATS;
    if (r->pdt[v] < g) {
        r->pdt[v] += WIKI_REPLAY_RATE;
        r->prates[0] = (float)r->p_ei;
        r->prates[1] = (float)r->p_eo;
        r->prates[2] = (float)r->p_mi;
        r->prates[3] = (float)r->p_mo;
    }
}

/* a seek's end outside a players replay: the click ring starts at the tick it ended on
   (tagpu_wikireplay.h, THE CLICK RING); pl_tail does it in its own order in a players replay */
static void seek_end(WIKIREPLAY* r)
{
    if (r->target < 0 && r->pend) {
        r->click_from = r->game->tick;
        r->pend = 0;
    }
}

/* THE WALKER'S SAMPLES (replay.js statWalk states the measures and their order), a players
   replay's: at each walked tick at or past base + n period, every seat's seven. BOUND:
   WIKI_STAT_PTS samples, the walker's seats at most WIKI_SEATS. */
static void stat_walk(WIKIREPLAY* r)
{
    const WIKISIM* w = r->walker;
    int k;
    if (!r->players) return;
    while (r->npt < WIKI_STAT_PTS && w->tick >= r->pbase + r->npt * r->period) {
        for (k = 0; k < r->stat_nseat && k < w->nseat; k++) {
            const WIKIECON* z = &w->econ[k];
            float* o = r->sv[r->npt][k];
            o[0] = (float)z->mi;
            o[1] = (float)z->ei;
            o[2] = (float)z->units;
            o[3] = tagpu_wikisim_army(w, k);
            o[4] = (float)z->kills;
            o[5] = (float)z->losses;
            o[6] = (float)z->wm;
        }
        r->npt++;
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
    r->tk = e->frame == WIKI_FRAME_GAME && e->takeover;
    r->players = e->frame == WIKI_FRAME_GAME && e->players;
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
    r->toast_seat = -1;
    if (r->players) {
        /* the players state at the start (replay.js's states each value and why): the camera at
           eye (0, 0), no Follow, Jump or Sight, no Sight asked, every switch on, the window closed
           on its first measure, split by player, every DisplayTimer 0, no pass; the period fits the
           span in the game's 512 samples (tagpu_hudmarks.c walk_start), 30 for every wiki game */
        r->follow = r->jump_req = r->sight = -1;
        r->sight_req = WIKI_REPLAY_NOT_ASKED;
        r->tog = WIKI_REPLAY_TOG_ALL;
        r->stat_measure = WIKI_STAT_MEASURE0;
        r->pass_at = -1;
        r->stat_nseat = e->scenes[d->on].nseat < WIKI_SEATS ? e->scenes[d->on].nseat : WIKI_SEATS;
        r->period = WIKI_REPLAY_RATE * ((r->end - r->pbase + WIKI_REPLAY_RATE * 511 - 1) / (WIKI_REPLAY_RATE * 511));
        if (r->period < WIKI_REPLAY_RATE) r->period = WIKI_REPLAY_RATE;
    }
    if (r->tk) {
        /* the rows: the original, its last the game's end, then each kept fork as it was played,
           its part the entry's (only read). THE BOUND: at most REC_FORK_LIMIT kept forks, the
           loader's, so the rows fit RECR_TIMELINES. */
        r->human = -1;
        r->pick_evict = -1;
        /* the live game's state before the takeover starts it, as a live entry's starts */
        r->made = 1;
        r->drag_t = -1;
        r->back = -1;
        r->arm_evict = -1;
        r->tl[0].parent = -1;
        r->tl[0].R = WIKI_REPLAY_FIRST;
        r->tl[0].last = r->end;
        r->tl[0].live = 1;
        r->tl[0].mode = WIKI_ECON_REFUSE;
        r->ntl = 1;
        for (i = 0; i < d->nkept && r->ntl < RECR_TIMELINES; i++, r->ntl++) {
            WIKITIMELINE* t = &r->tl[r->ntl];
            t->id = r->ntl;
            t->parent = d->kept[i].parent;
            t->R = d->kept[i].from;
            t->last = d->kept[i].end;
            t->live = 1;
            t->mode = WIKI_ECON_REFUSE;
            t->seat = d->kept[i].seat;
            t->part = d->kept[i].input;
            t->npart = d->kept[i].ninput;
        }
    }
    /* the seats as the scene and the script's tick 0 leave them, a walk that has nothing to play,
       the clock's tick 0 */
    if (!r->live) {
        publish(r);
        stat_walk(r);
        if (r->walker->done || r->walker->tick >= chain_last(r)) end_walk(r);
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
   game's part; live, the arm's fill and the top bar's frame after it; a takeover replay's watch
   mode, the picker's and the arm's fill and the background walk after it; a players replay, its
   tail */
void tagpu_wikireplay_step(WIKIREPLAY* r)
{
    int i, n, ran;
    if (r->done) return;
    r->tick++;
    if (r->walker && !r->bg) {
        n = chain_last(r) - r->walker->tick < WIKI_REPLAY_WALK ? chain_last(r) - r->walker->tick : WIKI_REPLAY_WALK;
        for (i = 0; i < n && !r->walker->done && chain_step(r, r->walker); i++) {
            publish(r);
            stat_walk(r);
        }
        if (!r->failed) {
            if (r->walker->done || r->walker->tick >= chain_last(r)) end_walk(r);
            else r->walked = r->walker->tick;
        }
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
    } else if (!r->failed) {
        ran = play(r);
        if (r->tk && !r->failed) {
            pick_fill(r);
            lane_fill(r);
        }
        if (r->tk && !r->failed) bg_walk(r, ran);
        if (r->players && !r->failed) pl_tail(r);
        else if (!r->failed) seek_end(r);
    }
    r->done = is_done(r);
}

/* the viewer's trip acts on clock tick c: trip's own conditions, a drag's every tick counted
   whether or not its point moved */
static int trip_acts(const WIKIREPLAY* r, int c)
{
    const int press = r->at + WIKI_REPLAY_HUD_DOWN;
    if (!r->has_cursor) return 0;
    if (r->to != WIKI_HUD_TRACK) return c == r->at + WIKI_REPLAY_HUD_UP;
    return c == press || (r->drag >= 0 && c > press && c <= press + r->over)
        || c == (r->drag >= 0 ? press + r->over : r->at + WIKI_REPLAY_HUD_UP);
}

/* THE BOUND on a step, its most game ticks: a seek's WIKI_REPLAY_SEEK, the walk's WIKI_REPLAY_WALK
   (a background walk takes what the game's part left of it, bg_walk), a play's the top speed's */
static int step_max(void)
{
    const int top = SPEEDS[WIKI_REPLAY_NSPEED - 1] / 10, m = WIKI_REPLAY_SEEK > WIKI_REPLAY_WALK ? WIKI_REPLAY_SEEK : WIKI_REPLAY_WALK;
    return m > top ? m : top;
}

/* A STEP WHOSE CLOCK TICK THE VIEWER'S TRIP ACTS ON is charged step_max: the press may start a seek,
   a switch or a rewind's seek that the step then plays, which the state before it cannot say. Every
   other step runs the ticks the state says. So cost() never falls short of the ticks the step runs,
   and a fill's sum of costs bounds its ticks (tagpu_wiki.c replay_steps). The walk that holds the
   clock feeds nothing; a step feeds its clock tick unless a wait holds it behind a pending seek. A
   live replay has no walker. */
int tagpu_wikireplay_cost(const WIKIREPLAY* r)
{
    int n, w;
    const int last = chain_last(r);
    if (r->done) return 0;
    if (r->walker && !r->bg) return last - r->walker->tick < WIKI_REPLAY_WALK ? last - r->walker->tick : WIKI_REPLAY_WALK;
    if ((!r->waiting || r->target < 0) && trip_acts(r, r->clock + 1)) return step_max();
    if (r->live) {
        if (r->rw == WIKI_RW_IDLE) return r->game->tick < r->end && !r->game->done;
        if (r->rw != WIKI_RW_PAST || r->target < 0 || !r->past) return 0;
        n = r->target - 1 - r->past->tick;
        return n < WIKI_REPLAY_SEEK ? n : WIKI_REPLAY_SEEK;
    }
    if (r->target >= 0) n = r->target - r->game->tick < WIKI_REPLAY_SEEK ? r->target - r->game->tick : WIKI_REPLAY_SEEK;
    else if (r->paused) n = 0;
    else {
        n = (r->acc + r->speed) / 10;
        if (n > last - r->game->tick) n = last - r->game->tick;
    }
    if (!r->walker) return n;
    /* the background walk's share: what is left of WIKI_REPLAY_WALK, to the chain's last */
    w = WIKI_REPLAY_WALK - n < last - r->walker->tick ? WIKI_REPLAY_WALK - n : last - r->walker->tick;
    return n + (w > 0 ? w : 0);
}

void tagpu_wikireplay_press(WIKIREPLAY* r, int control, int value)
{
    if (r->done || (r->walker && !r->bg) || control < 0 || control >= WIKI_NPRESS) return;
    if (r->live ? !tagpu_wikidata_hud_presses(control, 1)
                : !(control == WIKI_HUD_LEAVE_YES || tagpu_wikidata_hud_presses(control, 0)
                    || (r->tk && tagpu_wikidata_hud_takeover(control))
                    || (r->players && (tagpu_wikidata_hud_players(control) || control == WIKI_HUD_FOLD))))
        return;
    if (r->players && !pl_value(r, control, value)) return;
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
    v->from_watched = r->has_cursor ? r->from_watched : 0;
    v->watched = r->has_cursor ? r->watched : 0;
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
    v->toast_seat = r->toast_kind >= 0 ? r->toast_seat : -1;
    v->toast_evict = r->toast_kind >= 0 ? r->toast_evict : -1;
    v->question = r->question;
    v->was_paused = r->question ? r->was_paused : 0;
    v->left = r->left;
    v->click_from = r->click_from;
    if (r->live || r->tk) {
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
            v->tl[i].seat = r->tl[i].seat;
        }
    }
    if ((v->tk = r->tk)) {
        v->tk_mode = r->live ? WIKI_TK_LIVE : WIKI_TK_WATCH;
        v->human = r->human;
        v->pick = r->pick;
        v->pick_R = r->pick_R;
        v->pick_evict = r->pick_evict;
        v->pick_parent = r->pick_parent;
        v->pick_tl = r->pick_tl;
        v->pick_serial = r->pick_serial;
        v->lanes = r->lanes;
        v->armed = r->armed;
        v->selected = r->selected;
        v->chain_last = chain_last(r);
        v->fill_to = fill_to(r);
        v->serial = r->serial;
        v->nsaved = r->nsaved;
        memcpy(v->saved, r->saved, sizeof v->saved);
        v->bg = r->bg;
        v->walker_tick = r->walker ? r->walker->tick : -1;
        v->tk_full = r->tk_full;
    }
    if ((v->players = r->players)) {
        v->cam_x = r->cam_x;
        v->cam_z = r->cam_z;
        v->follow = r->follow;
        v->jump_req = r->jump_req;
        v->sight = r->sight;
        v->sight_req = r->sight_req;
        v->tog = r->tog;
        v->folded = r->folded;
        v->stat_open = r->stat_open;
        v->stat_measure = r->stat_measure;
        v->stat_team = r->stat_team;
        v->shown = r->shown;
        v->pend = r->pend;
        v->pbe = r->pbe;
        v->pbm = r->pbm;
        memcpy(v->prates, r->prates, sizeof v->prates);
        memcpy(v->pdt, r->pdt, sizeof v->pdt);
        v->pass_at = r->pass_at;
        v->pass_blocks = r->pass_blocks;
        memcpy(v->pass_fog, r->pass_fog, sizeof v->pass_fog);
        v->npass_contact = r->npass_id;
        v->pass_contact = (const char (*)[WIKI_ID_MAX])r->pass_id;
        v->npt = r->npt;
        v->nseat = r->stat_nseat;
        v->base = r->pbase;
        v->period = r->period;
    }
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
    v->human = r->human;
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
}

float tagpu_wikireplay_stat(const WIKIREPLAY* r, int n, int seat, int m)
{
    if (!r->players || n < 0 || n >= r->npt || seat < 0 || seat >= r->stat_nseat || m < 0 || m >= WIKI_STAT_MEASURES) return 0;
    return r->sv[n][seat][m];
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

static void live_hash(const WIKIREPLAY* r, const WIKIREPLAYVIEW* v, uint64_t* hp);
static void players_hash(const WIKIREPLAY* r, const WIKIREPLAYVIEW* v, uint64_t* hp);

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
    if (v.live) live_hash(r, &v, &h);
    if (v.players) players_hash(r, &v, &h);
    if (!v.tk) return h;
    fnv_int(&h, v.tk_mode);
    fnv_int(&h, v.human);
    fnv_int(&h, v.pick);
    fnv_int(&h, v.pick_R);
    fnv_int(&h, v.pick_evict);
    fnv_int(&h, v.pick_parent);
    fnv_int(&h, v.pick_tl);
    fnv_int(&h, v.pick_serial);
    fnv_int(&h, v.serial);
    fnv_int(&h, v.lanes);
    fnv_int(&h, v.armed);
    fnv_int(&h, v.bg ? 1 : 0);
    fnv_int(&h, v.tk_full ? 1 : 0);
    fnv_int(&h, v.nsaved);
    for (i = 0; i < v.nsaved; i++) fnv_int(&h, v.saved[i]);
    fnv_int(&h, v.ntl);
    for (i = 0; i < v.ntl; i++) {
        fnv_int(&h, v.tl[i].id);
        fnv_int(&h, v.tl[i].parent);
        fnv_int(&h, v.tl[i].R);
        fnv_int(&h, v.tl[i].last);
        fnv_int(&h, v.tl[i].live ? 1 : 0);
        fnv_int(&h, v.tl[i].used);
        fnv_int(&h, v.tl[i].mode);
        fnv_int(&h, v.tl[i].seat);
    }
    fnv_int(&h, v.walker_tick);
    return h;
}

/* the hash's live block, tagpu_wikireplay.h's order */
static void live_hash(const WIKIREPLAY* r, const WIKIREPLAYVIEW* v, uint64_t* hp)
{
    uint64_t h = *hp, g;
    int i;
    fnv_int(&h, v->rw);
    fnv_int(&h, v->G);
    fnv_int(&h, v->now);
    fnv_int(&h, v->arm);
    fnv_int(&h, v->arm_R);
    fnv_int(&h, v->arm_evict);
    fnv_int(&h, v->drag_t);
    fnv_int(&h, v->clash ? 1 : 0);
    fnv_int(&h, v->nlogged);
    fnv_int(&h, v->selected);
    fnv_int(&h, v->ntl);
    for (i = 0; i < v->ntl; i++) {
        fnv_int(&h, v->tl[i].id);
        fnv_int(&h, v->tl[i].parent);
        fnv_int(&h, v->tl[i].R);
        fnv_int(&h, v->tl[i].last);
        fnv_int(&h, v->tl[i].live ? 1 : 0);
        fnv_int(&h, v->tl[i].used);
        fnv_int(&h, v->tl[i].mode);
        fnv_int(&h, v->tl[i].nlog);
    }
    fnv_int(&h, v->toast_evict);
    fnv_int(&h, v->back);
    fnv_int(&h, v->full ? 1 : 0);
    fnv_f32(&h, v->be);
    fnv_f32(&h, v->bm);
    for (i = 0; i < 4; i++) fnv_f32(&h, v->rates[i]);
    fnv_int(&h, v->dt);
    g = r->past ? tagpu_wikisim_hash(r->past) : 0;
    fnv_u32(&h, (uint32_t)(g & 0xFFFFFFFFu));
    fnv_u32(&h, (uint32_t)(g >> 32));
    *hp = h;
}

/* the hash's players block, tagpu_wikireplay.h's order */
static void players_hash(const WIKIREPLAY* r, const WIKIREPLAYVIEW* v, uint64_t* hp)
{
    uint64_t h = *hp;
    int i, k, m;
    fnv_int(&h, v->cam_x);
    fnv_int(&h, v->cam_z);
    fnv_int(&h, v->follow);
    fnv_int(&h, v->jump_req);
    fnv_int(&h, v->sight);
    fnv_int(&h, v->sight_req);
    fnv_int(&h, v->tog);
    fnv_int(&h, v->folded);
    fnv_int(&h, v->stat_open ? 1 : 0);
    fnv_int(&h, v->stat_measure);
    fnv_int(&h, v->stat_team ? 1 : 0);
    fnv_int(&h, v->click_from);
    fnv_int(&h, v->shown);
    fnv_int(&h, v->pend ? 1 : 0);
    fnv_f32(&h, v->pbe);
    fnv_f32(&h, v->pbm);
    for (i = 0; i < 4; i++) fnv_f32(&h, v->prates[i]);
    for (i = 0; i <= WIKI_SEATS; i++) fnv_int(&h, v->pdt[i]);
    fnv_int(&h, v->pass_at);
    fnv_int(&h, v->pass_blocks);
    fnv_bytes(&h, v->pass_fog, (size_t)(v->pass_blocks + 7) / 8);
    fnv_int(&h, v->npass_contact);
    for (i = 0; i < v->npass_contact; i++) fnv_str(&h, v->pass_contact[i]);
    fnv_int(&h, v->npt);
    fnv_int(&h, v->base);
    fnv_int(&h, v->period);
    for (i = 0; i < v->npt; i++)
        for (k = 0; k < v->nseat; k++)
            for (m = 0; m < WIKI_STAT_MEASURES; m++) fnv_f32(&h, r->sv[i][k][m]);
    *hp = h;
}

void tagpu_wikireplay_free(WIKIREPLAY* r)
{
    int i;
    free(r->pass_id);
    game_free(r->game);
    game_free(r->walker);
    game_free(r->past);
    for (i = 0; i < r->ntl; i++) free(r->tl[i].log);
    free((void*)r->script);
    memset(r, 0, sizeof *r);
}
