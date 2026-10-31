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
    return r->clock >= r->vend || r->stalled || r->failed || r->left || stopped(r->game);
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
   leave:yes in a replay that plays a script; each ignored press is counted. `c` is a WIKI_HUD_*
   below WIKI_NPRESS: the callers bound it. */
static void act(WIKIREPLAY* r, int c, int value)
{
    int playhead, k, i;
    const int answer = c == WIKI_HUD_LEAVE_NO || c == WIKI_HUD_LEAVE_YES;
    if (r->failed) return;
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
    default: /* WIKI_HUD_LEAVE_NO */
        r->paused = r->was_paused;
        r->question = 0;
        r->was_paused = 0;
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
    if (c == press) act(r, WIKI_HUD_TRACK, r->value);
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
            r->from = r->has_cursor ? r->to : -1;
            r->from_value = r->has_cursor ? r->pos : -1;
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
    if (!(r->game = game_new(r, 0, err, errn))) return 0;
    if (!(r->walker = game_new(r, 1, err, errn))) {
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
    /* the seats as the scene and the script's tick 0 leave them, a walk that has nothing to play,
       the clock's tick 0 */
    publish(r);
    if (r->walker->done) end_walk(r);
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
    if (!r->failed) play(r);
    r->done = is_done(r);
}

int tagpu_wikireplay_cost(const WIKIREPLAY* r)
{
    int n;
    if (r->done) return 0;
    if (r->walker) return r->end - r->walker->tick < WIKI_REPLAY_WALK ? r->end - r->walker->tick : WIKI_REPLAY_WALK;
    if (r->target >= 0) return r->target - r->game->tick < WIKI_REPLAY_SEEK ? r->target - r->game->tick : WIKI_REPLAY_SEEK;
    if (r->paused) return 0;
    n = (r->acc + r->speed) / 10;
    return n < r->end - r->game->tick ? n : r->end - r->game->tick;
}

void tagpu_wikireplay_press(WIKIREPLAY* r, int control, int value)
{
    if (r->done || r->walker || control < 0 || control >= WIKI_NPRESS) return;
    act(r, control, value);
    r->done = is_done(r);
}

WIKISIM* tagpu_wikireplay_game(WIKIREPLAY* r)
{
    return r->game;
}

void tagpu_wikireplay_view(const WIKIREPLAY* r, WIKIREPLAYVIEW* v)
{
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
    return h;
}

void tagpu_wikireplay_free(WIKIREPLAY* r)
{
    game_free(r->game);
    game_free(r->walker);
    free((void*)r->script);
    memset(r, 0, sizeof *r);
}
