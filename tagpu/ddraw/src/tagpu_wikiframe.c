/* tagpu_wikiframe.c -- the whole game frame on a frame:game entry's stage (tagpu_wikiframe.h). A
   port of tools/wiki/mockup/frame.js, function for function and in its order;
   tools/wiki/framecheck.mjs holds the two equal. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_wikiframe.h"
#include "tagpu_wikiasset.h"

#define TICK 30                             /* game ticks a second */

/* the whitebox's stand-ins for TA's player colours, by colour index (frame.js COLOURS) */
static const uint32_t COLOURS[WIKI_SEATS] = { 0x2348e0, 0xd32b00, 0xf4f4f4, 0x2f8a1b };
#define GREY 0x8a8a8a

static int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

/* `s` into `d` of `n` bytes, cut at the end and terminated */
static void copy(char* d, size_t n, const char* s)
{
    snprintf(d, n, "%s", s ? s : "");
}

void tagpu_wikiframe_size(int w, int h, int side_h, int* Wg, int* Hg)
{
    const int mw = (w > 0 ? w : 1) * WIKI_CELL, mh = (h > 0 ? h : 1) * WIKI_CELL;
    *Wg = WIKIFRAME_SIDE_W + mw;
    *Hg = mh + 2 * WIKIFRAME_BAR > side_h ? mh + 2 * WIKIFRAME_BAR : side_h;
}

const char* tagpu_wikiframe_key(int c)
{
    const char* k = c == WIKI_HUD_LEAVE_YES ? "leave:yes" : tagpu_wikidata_hud_name(c);
    return k ? k : "";
}

/* one of the replay's controls a demo presses (frame.js Replay.CONTROLS) */
static int control(const char* k)
{
    const int c = k && *k ? tagpu_wikidata_hud(k) : -1;
    return c >= 0 && c < WIKI_NHUD;
}

int tagpu_wikiframe_indexed(const char* key, int* control_, int* index)
{
    static const struct { const char* act; int c, n; } IX[4] = {
        { "takeas", WIKI_HUD_TAKEAS, WIKI_SEATS },
        { "lane", WIKI_HUD_LANE, WIKIFRAME_FORKS },
        { "export", WIKI_HUD_EXPORT, WIKIFRAME_FORKS },
        { "delete", WIKI_HUD_DELETE, WIKIFRAME_FORKS },
    };
    const char* colon = key ? strchr(key, ':') : NULL;
    size_t k;
    int i;
    /* one digit: every index is below 10 */
    if (!colon || colon[1] < '0' || colon[1] > '9' || colon[2]) return 0;
    k = (size_t)(colon - key);
    for (i = 0; i < 4; i++)
        if (strlen(IX[i].act) == k && !strncmp(key, IX[i].act, k) && colon[1] - '0' < IX[i].n) {
            *control_ = IX[i].c;
            *index = colon[1] - '0';
            return 1;
        }
    return 0;
}

/* a replay control's key or an indexed one: what a record names as held or as a cursor's end */
static int known(const char* k)
{
    int c, n;
    return control(k) || tagpu_wikiframe_indexed(k, &c, &n);
}

/* a game tick as m:ss into `b`: a bookmark's from 0, the live strip's from the rewind's first
   GameTime (tagpu_hudinput.c live_clock) */
static const char* clock_of(char* b, size_t n, int T, int first)
{
    const unsigned s = (unsigned)(T > first ? T - first : 0) / TICK;
    snprintf(b, n, "%u:%02u", s / 60, s % 60);
    return b;
}

/* a game tick as the Take over set's toasts name it, tagpu_hudinput.c clock_at: whole seconds from
   tick 0 (a takeover replay's watching starts there, as its dock's clock), rounded as the dock's
   clock rounds */
static const char* clock_at(char* b, size_t n, int T)
{
    const unsigned s = ((unsigned)(T > 0 ? T : 0) + TICK / 2) / TICK;
    snprintf(b, n, "%u:%02u", s / 60, s % 60);
    return b;
}

void tagpu_wikiframe_toast(const WIKIREPLAYVIEW* rv, const WIKIFRAMESEAT* seat, int nseat, char* out)
{
    char c[16];
    out[0] = 0;
    if (rv->toast_kind < 0) return;
    /* Play from here's arm says what the second click does for as long as the arm holds */
    if (rv->tick - rv->toast_at >= WIKIFRAME_TOAST && !(rv->toast_kind == WIKI_TOAST_ARMED && rv->live && rv->arm == WIKI_ARM_ARMED))
        return;
    clock_of(c, sizeof c, rv->toast_value, rv->live ? rv->first : 0);
    switch (rv->toast_kind) {
    case WIKI_TOAST_REMOVED: copy(out, HUD_TOASTLEN, "Bookmark removed"); break;
    case WIKI_TOAST_ADDED: snprintf(out, HUD_TOASTLEN, "Bookmark at %s", clock_of(c, sizeof c, rv->toast_value, 0)); break;
    case WIKI_TOAST_PLAYING_ON: snprintf(out, HUD_TOASTLEN, "Playing on from %s", c); break;
    case WIKI_TOAST_CANNOT: snprintf(out, HUD_TOASTLEN, "Cannot play from %s", c); break;
    case WIKI_TOAST_ARMED:
        if (rv->toast_evict >= 0) snprintf(out, HUD_TOASTLEN, "Click again to play from %s, replacing F%d", c, rv->toast_evict);
        else snprintf(out, HUD_TOASTLEN, "Click again to play from %s", c);
        break;
    case WIKI_TOAST_PLAYING_FROM: snprintf(out, HUD_TOASTLEN, "Playing from %s", c); break;
    case WIKI_TOAST_TAKING_OVER:
        snprintf(out, HUD_TOASTLEN, "Taking over %s at %s",
                 rv->toast_seat >= 0 && rv->toast_seat < nseat ? seat[rv->toast_seat].name : "", clock_at(c, sizeof c, rv->toast_value));
        break;
    case WIKI_TOAST_CANNOT_TAKE: snprintf(out, HUD_TOASTLEN, "Cannot take over at %s", clock_at(c, sizeof c, rv->toast_value)); break;
    case WIKI_TOAST_SAVED: snprintf(out, HUD_TOASTLEN, "F%d saved as a replay of its own", rv->toast_value); break;
    case WIKI_TOAST_CANNOT_SAVE: snprintf(out, HUD_TOASTLEN, "Cannot save F%d", rv->toast_value); break;
    case WIKI_TOAST_CLICK_DELETE: snprintf(out, HUD_TOASTLEN, "Click again to delete F%d", rv->toast_value); break;
    case WIKI_TOAST_DELETED: snprintf(out, HUD_TOASTLEN, "F%d deleted", rv->toast_value); break;
    default: copy(out, HUD_TOASTLEN, "Rewind refused"); break;
    }
}

/* A trip's control as the HUD keys it (THE INDEXED CONTROLS): a picker row by its seat, a lane's
   control by the place of the fork `v` in the record's list, "" (the rest point) when the list
   does not hold it; any other control by its name. */
static void key_of(const WIKIFRAMEREC* f, int c, int v, char* out)
{
    int k;
    if (c == WIKI_HUD_TAKEAS) {
        snprintf(out, HUD_KEYLEN, "takeas:%d", v);
        return;
    }
    if (c == WIKI_HUD_LANE || c == WIKI_HUD_EXPORT || c == WIKI_HUD_DELETE) {
        out[0] = 0;
        for (k = 0; k < f->nfork && k < WIKIFRAME_FORKS; k++)
            if (f->fork[k].id == v) {
                snprintf(out, HUD_KEYLEN, "%s:%d", tagpu_wikiframe_key(c), k);
                return;
            }
        return;
    }
    copy(out, HUD_KEYLEN, tagpu_wikiframe_key(c));
}

/* THE TAKE OVER BLOCK of a record from a takeover replay's view: the picker, the lanes, the fork
   list by (R, id) as tagpu_hudstate.c fill_forks lists the game's, each with its flags -- the
   watched one, the members of the watched chain, the Delete armed while the lanes are open -- and
   the ticks the stage HUD's track reads */
static void tk_block(const WIKIREPLAYVIEW* rv, WIKIFRAMEREC* out)
{
    int order[RECR_TIMELINES], n = 0, i, j, k, ntl = rv->ntl < RECR_TIMELINES ? rv->ntl : RECR_TIMELINES;
    unsigned char chain[RECR_TIMELINES];
    out->tk = 1;
    out->pick = (uint8_t)(rv->pick == WIKI_PICKER_OPEN ? 2 : rv->pick == WIKI_PICKER_CLOSED ? 0 : 1);
    out->lanes = (uint8_t)(rv->lanes != 0);
    out->human = (int8_t)(rv->human >= -1 && rv->human < WIKI_SEATS ? rv->human : -1);
    out->fork_max = WIKIFRAME_FORKS;
    out->tk_full = (uint8_t)(rv->tk_full != 0);
    out->chain_last = rv->chain_last;
    out->fill_to = rv->fill_to;
    out->pick_R = rv->pick_R;
    out->pick_evict = rv->pick_evict;
    /* the watched chain, from the selected timeline up its parents (a row's index is its id) */
    memset(chain, 0, sizeof chain);
    for (k = rv->selected, i = 0; k >= 0 && k < ntl && i < ntl; k = rv->tl[k].parent, i++) chain[k] = 1;
    for (i = 0; i < ntl; i++) {
        if (rv->tl[i].id <= 0 || !rv->tl[i].live) continue;
        for (j = n; j > 0 && (rv->tl[order[j - 1]].R > rv->tl[i].R ||
                              (rv->tl[order[j - 1]].R == rv->tl[i].R && rv->tl[order[j - 1]].id > rv->tl[i].id)); j--)
            order[j] = order[j - 1];
        order[j] = i;
        n++;
    }
    for (k = 0; k < n && k < WIKIFRAME_FORKS; k++) {
        const int r = order[k];
        WIKIFRAMEFORK* f = &out->fork[k];
        f->id = rv->tl[r].id;
        f->from = rv->tl[r].R;
        f->end = rv->tl[r].last;
        f->parent = rv->tl[r].parent;
        f->seat = (uint8_t)(rv->tl[r].seat >= 0 && rv->tl[r].seat < 0xFF ? rv->tl[r].seat : 0xFF);
        f->flags = (uint8_t)((f->id == rv->selected ? WIKIFRAME_FORK_WATCHED : 0) |
                             (f->id < RECR_TIMELINES && chain[f->id] ? WIKIFRAME_FORK_CHAIN : 0) |
                             (rv->lanes && rv->armed == f->id ? WIKIFRAME_FORK_ARMED : 0));
    }
    out->nfork = (uint8_t)k;
}

/* the tick under a trip's cursor on clock tick `at`: the press's, or along its drag */
static int trip_tick(const WIKIREPLAYVIEW* rv, int at)
{
    if (rv->drag < 0 || rv->over <= 0) return rv->value;
    return rv->value + (rv->drag - rv->value) * clampi(at - rv->at - WIKI_REPLAY_HUD_DOWN, 0, rv->over) / rv->over;
}

void tagpu_wikiframe_record(const WIKIREPLAYVIEW* rv, const WIKIVIEW* gv, int side, int try_it, const char* try_held,
                            int top_side, WIKIFRAMEREC* out)
{
    const int n = WIKI_REPLAY_HUD_DOWN;
    /* the human seat: seat 0 in Rewind, the seat taken over in a takeover's live game */
    const int h = rv->live && rv->human >= 0 && rv->human < WIKI_SEATS ? rv->human : 0;
    int k, nseat = gv && gv->game ? gv->nseat : 0;
    memset(out, 0, sizeof *out);
    if (nseat > WIKI_SEATS) nseat = WIKI_SEATS;
    if (nseat < 0) nseat = 0;
    out->on = 1;
    out->playing = (uint8_t)(!rv->paused && rv->game_tick < rv->end);
    out->question = (uint8_t)(rv->question != 0);
    out->nseat = (uint8_t)nseat;
    out->side = (int8_t)(side >= -128 && side <= 127 ? side : -1);
    out->top_side = out->side;
    out->colour = (uint8_t)(rv->live ? h : 0);
    out->speed = (uint8_t)rv->speed;
    out->try_it = (uint8_t)(try_it != 0);
    out->playhead = rv->playhead;
    out->end = rv->end;
    for (k = 0; k < nseat; k++) {
        const WIKIECON* z = &gv->econ[k];
        WIKIFRAMESEAT* d = &out->seat[k];
        copy(d->name, sizeof d->name, gv->seat[k].name);
        d->colour = (uint8_t)k;
        d->lost = (uint8_t)(z->lost > 0);
        d->out = (uint8_t)(z->out != 0);
        d->units = (float)z->units;
        d->m = (float)z->m; d->mmax = (float)z->mmax; d->mi = (float)z->mi; d->mo = (float)z->mo;
        d->e = (float)z->e; d->emax = (float)z->emax; d->ei = (float)z->ei; d->eo = (float)z->eo;
    }
    for (k = 0; k < rv->nmark && k < WIKI_REPLAY_MARKS && out->nmark < WIKIFRAME_MARKS; k++, out->nmark++) {
        out->mark[out->nmark].t = rv->mark[k][0];
        out->mark[out->nmark].kind = (int8_t)rv->mark[k][1];
        out->mark[out->nmark].seat = (int8_t)rv->mark[k][2];
    }
    for (k = 0; k < rv->nbook && k < WIKI_REPLAY_BOOKS && out->nmark < WIKIFRAME_MARKS; k++, out->nmark++) {
        out->mark[out->nmark].t = rv->book[k];
        out->mark[out->nmark].kind = HUD_MK_BOOKMARK;
        out->mark[out->nmark].seat = -1;
    }
    if (rv->tk) {
        tk_block(rv, out);
        if (rv->live && top_side >= -128 && top_side <= 127) out->top_side = (int8_t)top_side;
    }
    if (try_it) copy(out->held, sizeof out->held, known(try_held) ? try_held : "");
    else if (rv->held >= 0) key_of(out, rv->held, rv->value, out->held);
    tagpu_wikiframe_toast(rv, out->seat, nseat, out->toast);
    out->cur.from_v = out->cur.to_v = -1;
    out->cur.i = out->cur.n = (uint8_t)n;
    if (rv->live) {
        out->live = 1;
        out->rw = (uint8_t)rv->rw;
        out->arm = (uint8_t)rv->arm;
        out->first = rv->first;
        out->G = rv->G;
        out->now = rv->now;
        out->last = rv->last;
        out->arm_R = rv->arm != WIKI_ARM_NONE ? rv->arm_R : 0;
        if (h < nseat) {
            const WIKIECON* z = &gv->econ[h];
            out->bar.e = rv->be;
            out->bar.m = rv->bm;
            out->bar.ei = rv->rates[0];
            out->bar.eo = rv->rates[1];
            out->bar.mi = rv->rates[2];
            out->bar.mo = rv->rates[3];
            out->bar.emax = (float)z->emax;
            out->bar.mmax = (float)z->mmax;
            out->bar.raw_e = (float)z->e;
            out->bar.raw_m = (float)z->m;
        }
        if (gv) {
            out->sim.x = (float)gv->cursor_x;
            out->sim.z = (float)gv->cursor_z;
            out->sim.shape = (uint8_t)(gv->cursor_shape >= 0 && gv->cursor_shape < WIKI_CURSOR_N ? gv->cursor_shape : 0);
            out->sim.age = rv->now - gv->cursor_since;
        }
    }
    /* THE ONE CURSOR (frame.js record): in Try it the simulation's is the player's pointer over the
       world once a fork is played, never in the past */
    if (try_it) {
        out->simcur = (uint8_t)(rv->live && rv->selected != 0 && rv->rw != WIKI_RW_PAST);
        return;
    }
    if (rv->live && rv->has_cursor && rv->back >= rv->at) {
        /* the return glide: this trip's release left the rewind idle, or took a seat over */
        key_of(out, rv->to, rv->value, out->cur.from);
        out->cur.from_v = rv->to == WIKI_HUD_TRACK ? trip_tick(rv, rv->back) : -1;
        out->cur.watched = rv->to == WIKI_HUD_MAIN ? rv->watched : 0;
        copy(out->cur.to, sizeof out->cur.to, WIKIFRAME_AT_GAME);
        out->cur.i = (uint8_t)clampi(rv->clock - rv->back, 0, n);
        out->simcur = (uint8_t)(rv->rw == WIKI_RW_IDLE && rv->clock - rv->back >= n);
    } else if (rv->has_cursor) {
        if (rv->from == WIKI_REPLAY_AT_GAME) copy(out->cur.from, sizeof out->cur.from, WIKIFRAME_AT_GAME);
        else if (rv->from >= 0) key_of(out, rv->from, rv->from_value, out->cur.from);
        out->cur.from_v = rv->from >= 0 ? rv->from_value : -1;
        key_of(out, rv->to, rv->value, out->cur.to);
        out->cur.to_v = trip_tick(rv, rv->clock);
        out->cur.watched = rv->to == WIKI_HUD_MAIN ? rv->watched : rv->from == WIKI_HUD_MAIN ? rv->from_watched : 0;
        out->cur.i = (uint8_t)clampi(rv->clock - rv->at, 0, n);
    } else
        out->simcur = (uint8_t)(rv->live && rv->rw == WIKI_RW_IDLE);
    out->cur.on = (uint8_t)!out->simcur;
}

/* a float held finite and within the economy's bound */
static float econ(float v)
{
    if (!(v == v) || v > WIKI_ECON_MAX) return v == v ? (float)WIKI_ECON_MAX : 0.0f;
    return v < -WIKI_ECON_MAX ? (float)-WIKI_ECON_MAX : v;
}

/* a key terminated in its array and among the replay's controls or the indexed ones, else ""; with
   `game`, the human seat's cursor's key too */
static int key_ok(char* k, int game)
{
    k[HUD_KEYLEN - 1] = 0;
    if (!k[0] || known(k) || (game && !strcmp(k, WIKIFRAME_AT_GAME))) return 1;
    k[0] = 0;
    return 0;
}

/* a float held finite and within [lo, hi]; NaN takes lo */
static float within(float v, float lo, float hi)
{
    return !(v == v) ? lo : v < lo ? lo : v > hi ? hi : v;
}

void tagpu_wikiframe_bound(WIKIFRAMEREC* f, int nside, int nlogo)
{
    static const int SPEEDS[WIKI_REPLAY_NSPEED] = WIKI_REPLAY_SPEEDS;
    int k, j, ok = 0;
    f->on = (uint8_t)(f->on != 0);
    f->playing = (uint8_t)(f->playing != 0);
    f->question = (uint8_t)(f->question != 0);
    f->try_it = (uint8_t)(f->try_it != 0);
    if (f->nseat > WIKI_SEATS) f->nseat = WIKI_SEATS;
    if (f->nmark > WIKIFRAME_MARKS) f->nmark = WIKIFRAME_MARKS;
    f->end = clampi(f->end, 1, WIKI_GAME_END_MAX);
    f->playhead = clampi(f->playhead, 0, f->end);
    for (k = j = 0; k < f->nmark; k++) {
        WIKIFRAMEMARK m = f->mark[k];
        if (m.kind < HUD_MK_DEATH || m.kind > HUD_MK_BOOKMARK || m.seat < -1 || m.seat >= f->nseat) continue;
        m.t = clampi(m.t, 0, f->end);
        f->mark[j++] = m;
    }
    f->nmark = (uint16_t)j;
    for (k = 0; k < WIKI_REPLAY_NSPEED; k++) ok |= f->speed == SPEEDS[k];
    if (!ok) f->speed = WIKI_REPLAY_SPEED0;
    if (f->side < -1 || f->side >= nside) f->side = -1;
    if (f->colour >= nlogo) f->colour = 0xFF;
    for (k = 0; k < WIKI_SEATS; k++) {
        WIKIFRAMESEAT* z = &f->seat[k];
        z->name[sizeof z->name - 1] = 0;
        if (z->colour >= nlogo && z->colour >= WIKI_SEATS) z->colour = 0xFF;
        z->lost = (uint8_t)(z->lost != 0);
        z->out = (uint8_t)(z->out != 0);
        z->units = econ(z->units);
        z->m = econ(z->m); z->mmax = econ(z->mmax); z->mi = econ(z->mi); z->mo = econ(z->mo);
        z->e = econ(z->e); z->emax = econ(z->emax); z->ei = econ(z->ei); z->eo = econ(z->eo);
    }
    key_ok(f->held, 0);
    f->toast[sizeof f->toast - 1] = 0;
    f->cur.on = (uint8_t)(f->cur.on != 0);
    f->live = (uint8_t)(f->live != 0);
    /* both keys bounded before either is judged: a short-circuit would leave `to` unterminated */
    ok = key_ok(f->cur.from, f->live);
    ok &= key_ok(f->cur.to, f->live);
    if (!ok || f->cur.n < 1 || f->cur.n > 64 || f->cur.i > f->cur.n) f->cur.on = 0;
    if (f->cur.watched < 0 || f->cur.watched >= RECR_TIMELINES) f->cur.watched = 0;
    /* LIVE: the flags and enums, the ticks, the bar's numbers and the human seat's cursor */
    f->simcur = (uint8_t)(f->live && f->simcur != 0);
    if (f->rw > WIKI_RW_PAST) f->rw = WIKI_RW_IDLE;
    if (f->arm > WIKI_ARM_ARMED) f->arm = WIKI_ARM_NONE;
    f->first = clampi(f->first, 0, WIKI_GAME_END_MAX);
    f->G = clampi(f->G, 0, WIKI_GAME_END_MAX);
    f->now = clampi(f->now, 0, WIKI_GAME_END_MAX);
    f->last = clampi(f->last, 0, WIKI_GAME_END_MAX);
    f->arm_R = clampi(f->arm_R, 0, WIKI_GAME_END_MAX);
    f->bar.e = econ(f->bar.e); f->bar.m = econ(f->bar.m);
    f->bar.ei = econ(f->bar.ei); f->bar.eo = econ(f->bar.eo); f->bar.mi = econ(f->bar.mi); f->bar.mo = econ(f->bar.mo);
    f->bar.emax = econ(f->bar.emax); f->bar.mmax = econ(f->bar.mmax);
    f->bar.raw_e = econ(f->bar.raw_e); f->bar.raw_m = econ(f->bar.raw_m);
    /* the cursor's point within the largest frame: a map of WIKISTAGE_MAP cells beside the tallest column */
    f->sim.x = within(f->sim.x, -WIKIFRAME_SIDE_W, WIKISTAGE_MAP * WIKI_CELL);
    f->sim.z = within(f->sim.z, -WIKIFRAME_BAR, WIKISTAGE_MAP * WIKI_CELL > WIKI_SIDE_ROWS ? WIKISTAGE_MAP * WIKI_CELL : WIKI_SIDE_ROWS);
    if (f->sim.age < 0) f->sim.age = 0;
    if (f->sim.shape >= WIKI_CURSOR_N) {
        f->sim.shape = WIKI_CURSOR_ARROW;
        f->simcur = 0;
    }
    /* TAKE OVER: a record that is not tk carries nothing of it */
    f->tk = (uint8_t)(f->tk != 0);
    if (!f->tk) {
        f->pick = f->lanes = f->nfork = f->fork_max = f->tk_full = 0;
        f->human = 0;
        f->chain_last = f->fill_to = f->pick_R = f->pick_evict = 0;
        memset(f->fork, 0, sizeof f->fork);
        f->top_side = f->side;
        return;
    }
    f->lanes = (uint8_t)(f->lanes != 0);
    f->tk_full = (uint8_t)(f->tk_full != 0);
    if (f->pick > 2) f->pick = 0;
    if (f->human < -1 || f->human >= f->nseat) f->human = -1;
    if (f->fork_max > WIKIFRAME_FORKS) f->fork_max = WIKIFRAME_FORKS;
    /* the top bar's side is the human seat's in the live game only */
    if (!f->live || f->top_side < -1 || f->top_side >= nside) f->top_side = f->side;
    f->chain_last = clampi(f->chain_last, 0, f->end);
    f->fill_to = clampi(f->fill_to, 0, f->end);
    f->pick_R = clampi(f->pick_R, 0, f->end);
    if (f->pick_evict < 1 || f->pick_evict >= RECR_TIMELINES) f->pick_evict = -1;
    if (f->nfork > WIKIFRAME_FORKS) f->nfork = WIKIFRAME_FORKS;
    for (k = j = 0; k < f->nfork; k++) {
        WIKIFRAMEFORK q = f->fork[k];
        if (q.id < 1 || q.id >= RECR_TIMELINES) continue;
        q.from = clampi(q.from, 0, f->end);
        q.end = clampi(q.end, 0, f->end);
        q.flags &= WIKIFRAME_FORK_WATCHED | WIKIFRAME_FORK_CHAIN | WIKIFRAME_FORK_ARMED;
        if (q.seat >= f->nseat) q.seat = 0xFF;
        f->fork[j++] = q;
    }
    for (k = j; k < WIKIFRAME_FORKS; k++) memset(&f->fork[k], 0, sizeof f->fork[k]);
    f->nfork = (uint8_t)j;
}

int tagpu_wikiframe_idle_side(const WIKILAYOUT* lay, int side, WIKISTAGESIDE* out)
{
    const WIKISCREEN* scr;
    int i;
    memset(out, 0, sizeof *out);
    out->index = -1;
    out->screen = -1;
    out->under = -1;
    if (!lay || side < 0 || side >= lay->nside || side >= WIKI_SIDE_SIDES) return 0;
    if (lay->side[side].main2 < 0 || lay->side[side].main2 >= lay->nscreen || lay->side[side].main2 >= WIKI_SIDE_SCREENS) return 0;
    scr = &lay->screen[lay->side[side].main2];
    out->index = (int8_t)side;
    out->h = (int16_t)(lay->height > 0 && lay->height <= WIKI_SIDE_ROWS ? lay->height : 0);
    out->screen = (int16_t)lay->side[side].main2;
    for (i = 0; i < scr->ngadget && i < WIKI_SIDE_GADGETS; i++) {
        const int r = scr->gadget[i].role;
        out->state[i] = (int8_t)(scr->gadget[i].hidden ? -1
                                 : r == WIKI_ROLE_ORDERS || r == WIKI_ROLE_BUILDTAB || r == WIKI_ROLE_BUILD || r == WIKI_ROLE_STOP ||
                                   r == WIKI_ROLE_ORDER ? 2 : 0);
    }
    out->nstate = (int16_t)i;
    return 1;
}

void tagpu_wikiframe_view(WIKISTAGEVIEW* v, const WIKILAYOUT* lay, int side, int layout_src, int mode, int cursor, int human)
{
    int i;
    v->frame = 1;
    v->cursor_shape = WIKI_CURSOR_N;
    if (mode == WIKIFRAME_PLAYER) {
        if (!cursor) v->has_ghost = 0;
        for (i = 0; i < v->nunit && i < WIKISTAGE_UNITS; i++)
            if (v->unit[i].owner != human) v->unit[i].selected = 0;
    } else {
        if (mode != WIKIFRAME_PAST) memset(v->fog, 0, sizeof v->fog);
        v->has_ghost = 0;
        v->nsite = v->nqueued = v->nline = 0;
        v->shape = PLACE_NONE;
        v->spacing = -1;
        v->has_drag = 0;
        v->rmode = 0;
        memset(&v->sweep, 0, sizeof v->sweep);
        for (i = 0; i < v->nunit && i < WIKISTAGE_UNITS; i++) v->unit[i].selected = 0;
        memset(v->held, 0, sizeof v->held);
        if (mode != WIKIFRAME_PAST) v->has_side = 0;
        v->side.under = -1;
    }
    if (!v->has_side) {
        v->has_side = (uint8_t)tagpu_wikiframe_idle_side(lay, side, &v->side);
        v->layout_src = (uint8_t)(v->has_side && layout_src > 0 && layout_src < WIKISTAGE_LAYOUT_N ? layout_src : WIKISTAGE_LAYOUT_NONE);
    }
}

uint32_t tagpu_wikiframe_seat_colour(const struct WIKISIDE* sd, int k)
{
    if (sd && sd->sheet && k >= 0 && k < sd->nlogo && k < WIKI_LOGOS) {
        const WIKISIDEFRAME* q = &sd->logo[k];
        const int x = q->sx + (q->w >> 1), y = q->sy + (q->h >> 1);
        /* the frame is data: inside the sheet, or the stand-in */
        if (q->w > 0 && q->h > 0 && x >= 0 && y >= 0 && x < sd->sheet_w && y < sd->sheet_h) {
            const unsigned char* c = sd->pal + 4 * sd->sheet[(size_t)y * (size_t)sd->sheet_w + (size_t)x];
            return (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        }
    }
    return k >= 0 && k < WIKI_SEATS ? COLOURS[k] : GREY;
}

void tagpu_wikiframe_hudstate(const WIKIFRAMEREC* f, int Wg, int Hg, float px, const struct WIKISIDE* sd, HUDSTATE* st)
{
    const double k = (double)WIKIFRAME_HUD_H / (Hg > 0 ? Hg : 1), T = f->end / (double)TICK;
    int i;
    memset(st, 0, sizeof *st);
    st->W = (float)(Wg * k);
    st->H = (float)WIKIFRAME_HUD_H;
    st->panel = (float)(WIKIFRAME_SIDE_W * k);
    st->top = (float)(WIKIFRAME_BAR * k);
    st->bottom = (float)(WIKIFRAME_BAR * k);
    st->t = (float)(f->playhead / (double)TICK);
    st->T = (float)T;
    st->playing = f->playing;
    st->speed = (float)(f->speed / 10.0);
    st->hud = 1;
    st->nseat = f->nseat < WIKI_SEATS ? f->nseat : WIKI_SEATS;
    for (i = 0; i < st->nseat; i++) {
        const WIKIFRAMESEAT* z = &f->seat[i];
        HUDSEAT* q = &st->seat[i];
        copy(q->name, sizeof q->name, z->name);
        q->colour = tagpu_wikiframe_seat_colour(sd, z->colour);
        q->units = z->units;
        q->lost = z->lost;
        /* the game's rule, tagpu_hudstate.c: a seat with no unit is out, its picker row refused */
        q->out = z->units == 0;
        q->m = z->m; q->mmax = z->mmax; q->mi = z->mi; q->mo = z->mo;
        q->e = z->e; q->emax = z->emax; q->ei = z->ei; q->eo = z->eo;
        st->team_n[i] = 1;
        st->team_seat[i][0] = (uint8_t)i;
    }
    st->nteam = st->nseat;
    st->toggles = HUD_TOG_CURSORS | HUD_TOG_CLICKS | HUD_TOG_SELECTIONS | HUD_TOG_CHAT;
    for (i = 0; i < f->nmark && i < WIKIFRAME_MARKS; i++) {
        st->mark[i].t = (float)(f->mark[i].t / (double)TICK);
        st->mark[i].kind = f->mark[i].kind;
        st->mark[i].seat = f->mark[i].seat;
    }
    st->nmark = i;
    st->mx = st->my = -1;
    st->px = px;
    copy(st->held, sizeof st->held, f->held);
    st->sight = st->follow = -1;
    copy(st->toast, sizeof st->toast, f->toast);
    st->ends = st->len = st->fill_to = (float)T;
    st->fork_max = HUD_MAXFORK;
    st->timeline = st->replace = st->armed = -1;
    if (f->tk && !f->live) {
        /* a takeover's watching, as tagpu_hudstate.c `forks` and `present` with the track from tick
           0 on the axis `end`: the playhead stops at the watched chain's last */
        st->len = (float)(f->chain_last / (double)TICK);
        st->fill_to = (float)(f->fill_to / (double)TICK);
        st->t = (float)((f->playhead < f->chain_last ? f->playhead : f->chain_last) / (double)TICK);
        st->fork_max = f->fork_max;
        st->nfork = f->nfork < WIKIFRAME_FORKS ? f->nfork : WIKIFRAME_FORKS;
        for (i = 0; i < st->nfork; i++) {
            const WIKIFRAMEFORK* k = &f->fork[i];
            HUDFORK* q = &st->fork[i];
            q->id = k->id;
            /* the bound leaves a seat below nseat or 0xFF, which no seat names */
            copy(q->name, sizeof q->name, k->seat < f->nseat && k->seat < WIKI_SEATS ? f->seat[k->seat].name : "Unknown seat");
            q->from = (float)(k->from / (double)TICK);
            q->end = (float)(k->end / (double)TICK);
            q->parent = k->parent;
            q->flags = k->flags & (WIKIFRAME_FORK_WATCHED | WIKIFRAME_FORK_CHAIN);
            /* the first of each, as frame.js finds them */
            if ((k->flags & WIKIFRAME_FORK_WATCHED) && st->timeline < 0) st->timeline = i;
            if ((k->flags & WIKIFRAME_FORK_ARMED) && st->armed < 0) st->armed = i;
            if (f->pick == 2 && k->id == f->pick_evict && st->replace < 0) st->replace = i;
        }
        st->picker = f->pick == 2;
        st->pick_down = f->pick != 0;
        st->lanes_open = f->lanes;
    }
    if (f->live) {
        /* the live HUD as tagpu_hudstate.c present_live sets it: the strip from the rewind's first
           GameTime to G, the playhead at the GameTime shown within it, never playing */
        const int G = f->G > f->first ? f->G : f->first;
        st->live = 1;
        st->live_open = st->rw_down = f->rw != WIKI_RW_IDLE;
        st->rw_arm = f->arm;
        st->T = (float)((G - f->first) / (double)TICK);
        st->t = (float)((clampi(f->now, f->first, G) - f->first) / (double)TICK);
        st->ends = st->len = st->fill_to = st->T;
        st->playing = 0;
    }
}

static double snapd(double v) { return floor(v + 0.5 + 1.0 / 64); }

/* YESORNO.GUI (tagpu_wikiframe.h): the panel at (112, 118), 400 x 100, and its gadgets in it */
#define Q_X 112
#define Q_Y 118

void tagpu_wikiframe_dialog(const WIKISTAGEFIT* fit, int Wg, int Hg, WIKIFRAMEDLG* out)
{
    /* Wg and Hg are even (16 a cell, 128 and 64 the frame's parts, the layouts' rows), so the
       centred GUI space's corner is a whole frame pixel */
    const double gx = Q_X + (Wg - 640) / 2.0, gy = Q_Y + (Hg - 480) / 2.0;
#define R_(b_, x_, y_, w_, h_)                                                                                 \
    do {                                                                                                       \
        const double x0_ = snapd(fit->ox + (gx + (x_) - WIKIFRAME_SIDE_W) * fit->s);                             \
        const double y0_ = snapd(fit->oz + (gy + (y_) - WIKIFRAME_BAR) * fit->s);                                \
        (b_).x = (int)x0_;                                                                                     \
        (b_).y = (int)y0_;                                                                                     \
        (b_).w = (int)(snapd(fit->ox + (gx + (x_) + (w_) - WIKIFRAME_SIDE_W) * fit->s) - x0_);                   \
        (b_).h = (int)(snapd(fit->oz + (gy + (y_) + (h_) - WIKIFRAME_BAR) * fit->s) - y0_);                      \
    } while (0)
    R_(out->panel, 0, 0, 400, 100);
    R_(out->title, 25, 25, 350, 20);
    R_(out->yes, 80, 55, 95, 20);
    R_(out->no, 225, 55, 95, 20);
    R_(out->frame, -gx, -gy, Wg, Hg);
#undef R_
}

double tagpu_wikiframe_glide(double a, double b, int i, int n)
{
    if (i >= n || n <= 0) return b;
    return a + (b - a) * i * i * (3.0 * n - 2.0 * i) / ((double)n * n * n);
}

/* the last HOP_HOT box of key `k`, or NULL */
static const HUDOP* box_of(const HUDOPS* o, const char* k)
{
    const HUDOP* b = NULL;
    int i;
    for (i = 0; i < o->n && i < HUD_MAXOP; i++) {
        const HUDOP* p = &o->op[i];
        if (p->kind == HOP_HOT && p->run < o->ntext && p->run < HUD_TEXTMAX && !strncmp(o->text + p->run, k, HUD_KEYLEN)) b = p;
    }
    return b;
}

/* a box's centre along one axis on the device pixels it is drawn on, in units */
static double mid(float a, float w, float px)
{
    return (tagpu_hud_snap(a * px) + tagpu_hud_edge(a, w, px)) / 2 / px;
}

static void resolve(const WIKIFRAMEREC* f, const HUDOPS* o, float px, double ends, const char* k, int v, double rx,
                    double ry, double nx, double ny, double* x, double* y)
{
    const HUDOP* b;
    *x = rx;
    *y = ry;
    if (!strcmp(k, "leave:no")) {
        if (nx == nx && ny == ny) { *x = nx; *y = ny; }
        return;
    }
    if (!k[0] || !(b = box_of(o, k))) return;
    *y = mid(b->r.y, b->r.h, px);
    if (strcmp(k, "track")) {
        *x = mid(b->r.x, b->r.w, px);
        return;
    }
    {
        /* the run: the game's [0, end], live the strip's [first, G] */
        const int a0 = f->live ? f->first : 0, a1 = f->live ? (f->G > f->first ? f->G : f->first) : f->end;
        const double x0 = (double)b->r.x + ends, x1 = (double)b->r.x + b->r.w - ends;
        const int t = clampi(v, a0, a1) - a0;
        *x = x0 + (x1 - x0) * t / (a1 - a0 > 1 ? a1 - a0 : 1);
    }
}

void tagpu_wikiframe_game_point(const WIKIFRAMEREC* f, const WIKISTAGEFIT* fit, double fx, double fy, float px, double* x,
                                double* y)
{
    *x = *y = NAN;
    if (!f->live) return;
    *x = (fit->ox + f->sim.x * fit->s - fx) / px;
    *y = (fit->oz + f->sim.z * fit->s - fy) / px;
}

int tagpu_wikiframe_pressed(const WIKIFRAMEREC* f, WIKIFRAMEREC* out)
{
    int k;
    *out = *f;
    if (f->tk && !f->live && f->cur.watched > 0 && (!strcmp(f->cur.to, "main") || !strcmp(f->cur.from, "main"))) {
        /* the lanes as Back to main recording's press drew them: open, its fork the watched one */
        for (k = 0; k < f->nfork && k < WIKIFRAME_FORKS && f->fork[k].id != f->cur.watched; k++) {}
        if (k >= f->nfork || k >= WIKIFRAME_FORKS || (f->lanes && (f->fork[k].flags & WIKIFRAME_FORK_WATCHED))) return 0;
        out->lanes = 1;
        for (k = 0; k < f->nfork && k < WIKIFRAME_FORKS; k++)
            out->fork[k].flags = (uint8_t)(f->fork[k].id == f->cur.watched ? f->fork[k].flags | WIKIFRAME_FORK_WATCHED
                                                                             : f->fork[k].flags & ~WIKIFRAME_FORK_WATCHED);
        return 1;
    }
    if (!f->live || strcmp(f->cur.to, WIKIFRAME_AT_GAME)) return 0;
    if (!strncmp(f->cur.from, "takeas:", 7)) {
        /* a takeover's glide back from its row: the replay HUD with the picker open, as many
           placeholder forks as draw the limit's warning when the takeover's picker drew it */
        out->live = 0;
        out->rw = WIKI_RW_IDLE;
        out->pick = 2;
        out->nfork = (uint8_t)(f->tk_full ? f->fork_max : 0);
        for (k = 0; k < WIKIFRAME_FORKS; k++) {
            memset(&out->fork[k], 0, sizeof out->fork[k]);
            out->fork[k].id = k + 1;
        }
        return 1;
    }
    out->rw = WIKI_RW_PAST;
    out->now = f->first;
    out->arm = WIKI_ARM_NONE;
    return 1;
}

/* a key's point: @game at seat 0's cursor (`gx`, `gy`; NaN: the rest point), any other by resolve */
static void resolve_key(const WIKIFRAMEREC* f, const HUDOPS* o, float px, double ends, const char* k, int v, double rx,
                        double ry, double nx, double ny, double gx, double gy, double* x, double* y)
{
    if (!strcmp(k, WIKIFRAME_AT_GAME)) {
        const int ok = gx == gx && gy == gy;
        *x = ok ? gx : rx;
        *y = ok ? gy : ry;
        return;
    }
    resolve(f, o, px, ends, k, v, rx, ry, nx, ny, x, y);
}

void tagpu_wikiframe_point(const WIKIFRAMEREC* f, const HUDOPS* ops, float px, double ends, double rest_x, double rest_y,
                           double no_x, double no_y, double game_x, double game_y, double* x, double* y)
{
    double ax, ay, bx, by;
    resolve_key(f, ops, px, ends, f->cur.from, f->cur.from_v, rest_x, rest_y, no_x, no_y, game_x, game_y, &ax, &ay);
    resolve_key(f, ops, px, ends, f->cur.to, f->cur.to_v, rest_x, rest_y, no_x, no_y, game_x, game_y, &bx, &by);
    *x = tagpu_wikiframe_glide(ax, bx, f->cur.i, f->cur.n);
    *y = tagpu_wikiframe_glide(ay, by, f->cur.i, f->cur.n);
}

int tagpu_wikiframe_on_world(double x, double z, int Hg, int mw, int mh)
{
    return (x >= -WIKIFRAME_SIDE_W && x < 0 && z >= -WIKIFRAME_BAR && z < Hg - WIKIFRAME_BAR) ||
           (x >= 0 && x < mw && z >= 0 && z < mh);
}

int tagpu_wikiframe_seat_side(const WIKILAYOUT* lay, int side, int seat)
{
    int r;
    if (seat == 0 || !lay) return side;
    for (r = 0; r < lay->nside && r < WIKI_SIDE_SIDES; r++)
        if (r != side) return r;
    return side;
}

int tagpu_wikiframe_lanes(const WIKIFRAMEREC* f, int32_t* lane)
{
    int k, n = 0;
    if (!f->tk || f->live || !f->lanes) return 0;
    for (k = 0; k < f->nfork && k < WIKIFRAME_FORKS; k++) lane[n++] = f->fork[k].id;
    return n;
}

void tagpu_wikiframe_press_init(WIKIFRAMEPRESS* m)
{
    m->held[0] = 0;
    m->last = -1;
    m->lane = -1;
}

static int give(int c, int v, int* control, int* value)
{
    *control = c;
    *value = v;
    return 1;
}

/* a button: a control a demo presses that is not the track, a picker row and a lane's controls
   by their indexed keys alone (the HUD keys no other) */
static int button(const char* k)
{
    int c, n;
    if (tagpu_wikiframe_indexed(k, &c, &n)) return 1;
    c = control(k) ? tagpu_wikidata_hud(k) : -1;
    return c >= 0 && c != WIKI_HUD_TRACK && c != WIKI_HUD_TAKEAS && c != WIKI_HUD_LANE && c != WIKI_HUD_EXPORT &&
           c != WIKI_HUD_DELETE;
}

/* the fork lane n showed, or -1 past the lanes drawn */
static int32_t lane_at(const int32_t* lane, int nlane, int n)
{
    return lane && n >= 0 && n < nlane && n < WIKIFRAME_FORKS ? lane[n] : -1;
}

int tagpu_wikiframe_press_down(WIKIFRAMEPRESS* m, int src, const char* key, int right, int T, const int32_t* lane, int nlane,
                               int* control_, int* value)
{
    int c, n;
    if (!key) key = "";
    if (right || m->held[0]) return 0;
    if (src == WIKIHIT_DIALOG) {
        if (!strcmp(key, "outside")) return give(WIKI_HUD_LEAVE_NO, 0, control_, value);
        if (!strcmp(key, "yes") || !strcmp(key, "no")) copy(m->held, sizeof m->held, key);
        return 0;
    }
    if (!strcmp(key, "track")) {
        copy(m->held, sizeof m->held, key);
        m->last = T;
        return give(WIKI_HUD_TRACK, T, control_, value);
    }
    if (tagpu_wikiframe_indexed(key, &c, &n) && c != WIKI_HUD_TAKEAS) {
        /* a lane's control is held only on a lane drawn, with the fork it showed */
        if ((m->lane = lane_at(lane, nlane, n)) < 0) return 0;
    }
    if (button(key)) copy(m->held, sizeof m->held, key);
    return 0;
}

int tagpu_wikiframe_press_drag(WIKIFRAMEPRESS* m, int T, int* control_, int* value)
{
    if (strcmp(m->held, "track") || T == m->last) return 0;
    m->last = T;
    return give(WIKI_HUD_TRACK, T, control_, value);
}

int tagpu_wikiframe_press_up(WIKIFRAMEPRESS* m, const char* key, int T, const int32_t* lane, int nlane, int* control_,
                             int* value)
{
    char h[HUD_KEYLEN];
    int c, n;
    const int32_t id = m->lane;
    copy(h, sizeof h, m->held);
    m->held[0] = 0;
    m->lane = -1;
    if (!key) key = "";
    if (!strcmp(h, "track")) return T == m->last ? 0 : give(WIKI_HUD_TRACK, T, control_, value);
    if (!h[0] || strcmp(key, h)) return 0;
    if (!strcmp(h, "yes")) return give(WIKI_HUD_LEAVE_YES, 0, control_, value);
    if (!strcmp(h, "no")) return give(WIKI_HUD_LEAVE_NO, 0, control_, value);
    if (tagpu_wikiframe_indexed(h, &c, &n)) {
        if (c == WIKI_HUD_TAKEAS) return give(c, n, control_, value);
        /* THE HELD LANE: the same fork at the same place at the press and at the release */
        return id >= 0 && lane_at(lane, nlane, n) == id ? give(c, id, control_, value) : 0;
    }
    return give(tagpu_wikidata_hud(h), 0, control_, value);
}
