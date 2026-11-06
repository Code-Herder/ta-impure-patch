/* hudstatejson.c -- the mockup's replay HUD state read into a HUDSTATE (hudstatejson.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hudstatejson.h"

/* a CSS colour as the mockup writes one, "#rrggbb" or "rgb(r, g, b)", as 0xRRGGBB */
static uint32_t css(const char* s)
{
    unsigned r = 0, g = 0, b = 0;
    if (s[0] == '#') return (uint32_t)strtoul(s + 1, NULL, 16);
    if (sscanf(s, "rgb(%u ,%u ,%u", &r, &g, &b) == 3 || sscanf(s, "rgb(%u, %u, %u", &r, &g, &b) == 3)
        return (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (b & 255);
    return 0;
}

void hudstate_json(const TJV* S, HUDSTATE* st)
{
    const TJV *v, *e;
    int j;
    memset(st, 0, sizeof *st);
    st->W = (float)tj_num(tj_get(S, "W"), 0);
    st->H = (float)tj_num(tj_get(S, "H"), 0);
    v = tj_get(S, "frame");
    st->panel = (float)tj_num(tj_get(v, "panel"), 0);
    st->top = (float)tj_num(tj_get(v, "top"), 0);
    st->bottom = (float)tj_num(tj_get(v, "bottom"), 0);
    st->t = (float)tj_num(tj_get(S, "t"), 0);
    st->T = (float)tj_num(tj_get(S, "T"), 0);
    st->playing = tj_is(tj_get(S, "playing"), TJ_TRUE);
    st->speed = (float)tj_num(tj_get(S, "speed"), 1);
    st->hud = (float)tj_num(tj_get(S, "hud"), 1);
    v = tj_get(S, "seats");
    for (e = v ? v->first : NULL; e && st->nseat < HUD_MAXSEAT; e = e->next) {
        HUDSEAT* q = &st->seat[st->nseat++];
        const TJV* r = tj_get(e, "res");
        snprintf(q->name, sizeof q->name, "%s", tj_str(tj_get(e, "name"), ""));
        q->colour = css(tj_str(tj_get(e, "colour"), "#000000"));
        q->units = (float)tj_num(tj_get(e, "units"), 0);
        q->lost = tj_is(tj_get(e, "lost"), TJ_TRUE);
        q->out = tj_is(tj_get(e, "out"), TJ_TRUE);
        q->m = (float)tj_num(tj_get(r, "m"), 0); q->mmax = (float)tj_num(tj_get(r, "mmax"), 0);
        q->mi = (float)tj_num(tj_get(r, "mi"), 0); q->mo = (float)tj_num(tj_get(r, "mo"), 0);
        q->e = (float)tj_num(tj_get(r, "e"), 0); q->emax = (float)tj_num(tj_get(r, "emax"), 0);
        q->ei = (float)tj_num(tj_get(r, "ei"), 0); q->eo = (float)tj_num(tj_get(r, "eo"), 0);
    }
    v = tj_get(S, "teams");
    for (e = v ? v->first : NULL; e && st->nteam < HUD_MAXSEAT; e = e->next, st->nteam++)
        for (j = 0; j < e->n && j < HUD_MAXSEAT; j++)
            st->team_seat[st->nteam][st->team_n[st->nteam]++] = (uint8_t)tj_num(tj_at(e, j), 0);
    v = tj_get(S, "folded");
    for (e = v ? v->first : NULL; e; e = e->next) st->folded |= 1u << (int)tj_num(e, 0);
    v = tj_get(S, "toggles");
    for (e = v ? v->first : NULL; e; e = e->next)
        if (tj_is(e, TJ_TRUE)) st->toggles |= tagpu_hudlay_toggle(e->key);
    v = tj_get(S, "markers");
    for (e = v ? v->first : NULL; e && st->nmark < HUD_MAXMARK; e = e->next) {
        HUDMARK* m = &st->mark[st->nmark++];
        const char* k = tj_str(tj_get(e, "kind"), "");
        m->t = (float)tj_num(tj_get(e, "t"), 0);
        m->kind = !strcmp(k, "defeat") ? HUD_MK_DEFEAT : !strcmp(k, "bookmark") ? HUD_MK_BOOKMARK : HUD_MK_DEATH;
        m->seat = (int)tj_num(tj_get(e, "seat"), -1);
    }
    st->mx = (float)tj_num(tj_get(S, "mx"), -1);
    st->my = (float)tj_num(tj_get(S, "my"), -1);
    snprintf(st->held, sizeof st->held, "%s", tj_str(tj_get(S, "held"), ""));
    st->sight = (int)tj_num(tj_get(S, "sight"), -1);
    st->follow = (int)tj_num(tj_get(S, "follow"), -1);
    st->scroll = (int)tj_num(tj_get(S, "scroll"), 0);
    snprintf(st->toast, sizeof st->toast, "%s", tj_str(tj_get(S, "toast"), ""));
    /* proposed, variation B: the mockup's names (index.html `state()`); a state without them is the
       original's alone */
    st->ends = (float)tj_num(tj_get(S, "ends"), st->T);
    st->len = (float)tj_num(tj_get(S, "len"), st->T);
    st->fill_to = (float)tj_num(tj_get(S, "fillTo"), st->T);
    st->timeline = (int)tj_num(tj_get(S, "timeline"), -1);
    st->lanes_open = tj_is(tj_get(S, "lanesOpen"), TJ_TRUE);
    st->picker = tj_is(tj_get(S, "picker"), TJ_TRUE);
    st->armed = (int)tj_num(tj_get(S, "armed"), -1);
    st->replace = (int)tj_num(tj_get(S, "replace"), -1);
    /* the Take over button is down while the picker is asked for, sought to or open, the mockup's
       pickDown, and with none while it is open; the picker's title is the playhead's clock, the
       mockup's st.t */
    v = tj_get(S, "pickDown");
    st->pick_down = v ? tj_is(v, TJ_TRUE) : st->picker;
    v = tj_get(S, "forks");
    for (e = v ? v->first : NULL; e && st->nfork < HUD_MAXFORK; e = e->next) {
        HUDFORK* f = &st->fork[st->nfork++];
        f->id = (int)tj_num(tj_get(e, "id"), 0);
        snprintf(f->name, sizeof f->name, "%s", tj_str(tj_get(e, "name"), ""));
        f->from = (float)tj_num(tj_get(e, "from"), 0);
        f->end = (float)tj_num(tj_get(e, "end"), 0);
        f->parent = (int)tj_num(tj_get(e, "parent"), 0);
        /* the packet's flags (TAGPU_PK_FORK_*): the mockup's `chain`, and the watched one by its place */
        f->flags = (tj_is(tj_get(e, "chain"), TJ_TRUE) ? 2 : 0) | (st->nfork - 1 == st->timeline ? 1 : 0);
    }
    /* the live HUD: the mockup's state carries `live` ({open}), the game's dump `mode` and `rw`
       too; the mockup's button is down while its strip is open */
    v = tj_get(S, "live");
    if (v || !strcmp(tj_str(tj_get(S, "mode"), ""), "live")) {
        st->live = 1;
        st->live_open = st->rw_down = tj_is(tj_get(v, "open"), TJ_TRUE);
        st->rw_arm = (int)tj_num(tj_get(tj_get(S, "rw"), "arm"), 0);
    }
    v = tj_get(S, "stats");
    if (v) {
        HUDSTATS* x = &st->stats;
        x->open = tj_is(tj_get(v, "open"), TJ_TRUE);
        x->x = (float)tj_num(tj_get(v, "x"), 0);
        x->y = (float)tj_num(tj_get(v, "y"), 0);
        snprintf(x->measure, sizeof x->measure, "%s", tj_str(tj_get(v, "measure"), ""));
        x->team = !strcmp(tj_str(tj_get(v, "split"), ""), "team");
        x->dt = (float)tj_num(tj_get(v, "dt"), 0);
        x->t0 = (float)tj_num(tj_get(v, "t0"), 0);
        e = tj_get(v, "groups");
        for (e = e ? e->first : NULL; e && x->ngroup < HUD_MAXSEAT; e = e->next) {
            HUDGROUP* g = &x->group[x->ngroup++];
            const TJV* d = tj_get(e, "death");
            const TJV* vs = tj_get(e, "v");
            snprintf(g->name, sizeof g->name, "%s", tj_str(tj_get(e, "name"), ""));
            g->colour = css(tj_str(tj_get(e, "col"), "#000000"));
            g->death = tj_is(d, TJ_NUM) ? (float)d->num : -1.0f;
            for (j = 0; vs && j < vs->n && j < HUD_MAXPT; j++) g->v[j] = (float)tj_num(tj_at(vs, j), 0);
            x->npt = j;
        }
    }
}
