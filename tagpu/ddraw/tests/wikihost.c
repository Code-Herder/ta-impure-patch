/* wikihost.c -- the wiki's simulation host: the C core (tagpu_wikisim.c over tasim_*.c, the loader
   tagpu_wikidata.c, the COB host tagpu_wikicob.c) answering JSON requests, one a line, so that the
   mockup's pages and the node checks play the one simulation the DLL plays. It is
   `wikitest serve` (stdin) and `wikitest trace FILE`, built three ways by tools/wiki/wikibuild.mjs:
   natively for the mockup, under AddressSanitizer, and i686 under Wine with the DLL's flags.

   THE PROTOCOL. One JSON object a line in, exactly one JSON line out, in order, always: an error
   is an answer, {"error": why}, and the loop goes on. Every line read is answered: one past
   WH_LINE_MAX bytes, one holding a NUL byte and an empty one each as an error, the first two read
   past to their newline. Handles are positive integers, never reused in a process.
   {"op": "batch", "calls": [...]} answers {"answers": [...]}, each call as alone.

   THE INSTANCE. A handle names an object of one host process only, and a new process numbers
   its handles from 1 again, so a handle kept across a host's end would name another object in
   the next one. `serve --instance ID` gives the process an id, which `hello` answers; a call
   carrying "in": ID is answered only by the host of that id, any other refusing it with
   {"error", "host": its own id}, before the call's handles are read. The drop-in stamps every
   call with the id of the host its handles came from (tools/wiki/mockup/simhost.js), so the
   invariant is: no call names a handle in a host that did not give it. The id is drawn fresh
   for each host started -- 128 random bits from the operating system's generator, by
   tools/wiki/serve.py's SimRelay and tools/wiki/hostchan.cjs, the tee's builds sharing one, as
   their handles must agree -- so two hosts share one with odds of 2^-128. A call without "in" is
   answered by any host: a recorded trace, a test.

   THE PACKS' ROOT. `serve --packs-root DIR` (tools/wiki/serve.py's relay, which a page drives)
   reads a pack's files only under DIR, every one resolved before it is read (pack_at).

     hello                          {"version": 1, "instance": id, "constants": {...}}: the
                                    constants the drop-in exports, from the C headers, in its names
     entry {json}                   {"entry": h}: tagpu_wikidata_entry
     layout {json}                  {"layout": h}: tagpu_wikidata_layout_json
     casting {json}                 {"casting": h}: tagpu_wikidata_casting
     packs {dirs}                   {"packs": h}: the units' scripts and sounds of those gfx packs
                                    (repository-relative), each type's from the first naming it
     table {json}                   {"table": h}: a layout's sight table, tagpu_wikidata_sight
     text {json}                    {"why": null | msg}: the entry's loader alone
     scene {entry}                  {"scene": name}: the scene its first demo plays on
     seatcam {entry, seat, tick}    {"at": [x, z] | null}: tagpu_wikidata_seat_cam
     sight {view, table, seat, tick, contacts}
                                    {"lit": base64, "contacts": [0|1, ...]}: the lit grid and
                                    contacts over a view of {w, h, units: [[x, z, owner, type,
                                    built]], deaths: [[tick, owner, x, z, type]], types: [[sight,
                                    radar, sonar, activates, modelHeight]]}, types by index;
                                    no more units, deaths or types than a sim's view holds
     sighttable {table, distance}   {"idx", "table": {idx, cells} | null}
     fits {w, h}                    {"fits": bool}: tagpu_wikidata_frame_fits
     new {entry, play, layout, ctx} {"sim", "tick", "done", "side", "iface", "game", "nin",
                                    "types", "map", "log"}: tagpu_wikisim_init; play try, demo,
                                    game or stall; ctx {side, iface, casting, rival, packs,
                                    scripts, sounds}: scripts or sounds false leaves the packs' out
     input {sim, ev}                {"tick", "done"}: tagpu_wikidata_input, as ev.seat
     step {sim, n, each, seat}      {"tick", "done", "ran", "each": [...], "log"}: up to n (1..4096)
                                    ticks, fewer when the sim is done; each: per tick ran, an object
                                    of what `each` lists, "hash" and "now" (of `seat`, 0 absent)
     view {sim, seat, map}          the view as the drop-in's view(seat), its map only with map true
     orders {sim, id | all}         a unit's orders, or {id: orders} for every unit of the view
     hash {sim}                     {"hash": 16 hex digits}
     now {sim, seat}                the drop-in's gameNow(seat): a game's state between ticks
     poses {sim}                    per view unit its posed model as base64, or null: the units
                                    tagpu_wikistage_pose poses, as the DLL's stage draws them
     notes {sim}                    each type's quarantine note, "" for none
     fork {sim, R, inputs, mode, seat}   {"tick", "done"}: tagpu_wikisim_fork, its message an error
     release {sim, seat}            {"tick", "done"}
     free {sim}                     {}, for a sim the host no longer holds too

   `want` on new, input, step, fork and release asks the state after the call on the same answer:
   {"view": [seats], "orders": true, "hash": true, "now": [seats], "poses": true} adds "views":
   {seat: view}, "orders", "hash", "now": {seat: ...} and "poses". A call on a handle no table
   holds answers "no sim N" (or entry, layout, ...).

   THE BOUNDS. entry, layout, casting, packs and table handles are kept in least-recently-used
   tables of WH_OBJS each; a sim holds a reference to what it was made from, so an object evicted
   from its table is freed when its last sim goes (a WIKIENTRY, its layout and its castings must
   outlive the sim, tagpu_wikisim.h). At most WH_SIMS sims, the least recently used freed past it.
   A request's tree lives as long as a sim that took an input or a fork part from it: a caption or
   a kill id the sim keeps points into it (tagpu_wikidata_input).

   THE LOG, the drop-in's sim.log ({t, what, id, type, ox, oz}), which only tools/wiki/simcheck.mjs
   reads, is the host's: after each mutating call it compares the sim's units and features with
   the last state it saw. A unit gone is `killed` when a kill input fed in that tick named it
   (then `corpse` when a feature of its id appeared, else `nocorpse` when it was finished and its
   type has a wreck), else `reclaimed`; a unit new is `started`, and `finished` once its built
   reaches 1; a feature gone is `reclaimed`, unless a wreck placed in that tick covers it (the
   wreck destroys the features under it, tasim_world.c corpse). `t` is the tick the step began on.
   ox and oz are a structure's site, absent for a mobile unit. */
#ifndef _WIN32
#define _XOPEN_SOURCE 700           /* realpath, for THE PACKS' ROOT */
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wikihost.h"
#include "wikicobfile.h"
#include "tagpu_json.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikireplay.h"
#include "tagpu_wikistage.h"
#include "tagpu_wikicob.h"
#include "tagpu_recrules.h"
#include "tagpu_limits.h"
#include "tasim.h"

#define WH_LINE_MAX (1 << 24)   /* a request line, bytes: a whole entry and a fork part fit */
#define WH_OBJS     32          /* each kind of handle's table */
#define WH_SIMS     64          /* sims alive at once */
#define WH_STEP_MAX 4096        /* ticks one step request runs */
/* a sight view's units: no sim has a fixed count, so the bound is the game's own, WIKI_SEATS
   players of TAGPU_LIM_UNITS each (tagpu_limits.h), which keeps the contacts' pass, a unit
   against every unit, to 36 M visits */
#define WH_SIGHT_UNITS (WIKI_SEATS * TAGPU_LIM_UNITS)

/* ---- JSON out, in the drop-in's shapes (tools/wiki/mockup/simhost.js) ---- */

void wh_str(const char* s)
{
    if (!s) { fputs("null", stdout); return; }
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

/* JSON has no NaN and no infinity, and an answer is printed as it is made, so one cannot turn
   into an error halfway: a value that is no finite number prints as null, as JSON.stringify
   writes it */
void wh_num(double v)
{
    if (isfinite(v)) printf("%.17g", v);
    else fputs("null", stdout);
}

void wh_key(const char* k, int first)
{
    if (!first) putchar(',');
    wh_str(k);
    putchar(':');
}

static const char* name(const WIKISIM* s, int t) { return s->defs.entry->def[t].name; }

static void site(const WIKISIM* s, const WIKIVSITE* w)
{
    wh_key("type", 1); wh_str(name(s, w->type));
    wh_key("x", 0); wh_num(w->x);
    wh_key("z", 0); wh_num(w->z);
    wh_key("fx", 0); printf("%d", w->fx);
    wh_key("fz", 0); printf("%d", w->fz);
}

void wh_orders(const WIKISIM* s, const char* id)
{
    static const char* const K[] = { "move", "build", "reclaim", "guard" };
    const WIKIORDER* o;
    const int n = tagpu_wikisim_orders(s, id, &o);
    int i;
    if (n < 0) { fputs("null", stdout); return; }
    putchar('[');
    for (i = 0; i < n; i++) {
        if (i) putchar(',');
        putchar('{');
        wh_key("k", 1); wh_str(K[o[i].k]);
        if (o[i].k == WIKI_ORDER_MOVE) { wh_key("x", 0); wh_num(o[i].x); wh_key("z", 0); wh_num(o[i].z); }
        else if (o[i].k == WIKI_ORDER_BUILD) {
            wh_key("type", 0); wh_str(name(s, o[i].type));
            wh_key("ox", 0); printf("%d", o[i].ox);
            wh_key("oz", 0); printf("%d", o[i].oz);
        } else if (o[i].k == WIKI_ORDER_RECLAIM && o[i].feat) {
            /* a feature reclaim: {k, feat, x, z} */
            wh_key("feat", 0); wh_str(o[i].target);
            wh_key("x", 0); wh_num(o[i].x);
            wh_key("z", 0); wh_num(o[i].z);
        } else { wh_key("target", 0); wh_str(o[i].target); }
        putchar('}');
    }
    putchar(']');
}

static const char* const STATE[] = { "place", "refused", "remove" };
static const char* const SHAPE[] = { NULL, "line", "axis", "rect", "hollow", "ring" };
static const char* const CURSOR[] = { "arrow", "move", "build", "reclaim" };
static const char* const UNTILS[] = WIKI_UNTIL_STRINGS;

/* the armed mode by its name */
static void mode_out(const WIKISIM* s, int rmode, int mode)
{
    if (rmode) wh_str("reclaim");
    else if (mode >= 0) {
        char m[WIKI_ID_MAX + 8];
        snprintf(m, sizeof m, "build:%s", name(s, mode));
        wh_str(m);
    } else wh_str("contextual");
}

static void keys_out(const int* held)
{
    int i, first = 1;
    putchar('[');
    for (i = 0; i < WIKI_NKEYS; i++)
        if (held[i]) {
            if (!first) putchar(',');
            wh_str(tagpu_wikidata_key_name(i));
            first = 0;
        }
    putchar(']');
}

/* an input seat's own parts, the view's inputs[k]: what its input shows and its reclaim */
static void seat_out(const WIKISIM* s, const WIKIVSEAT* b)
{
    int i;
    putchar('{');
    wh_key("ghost", 1);
    if (b->has_ghost) {
        putchar('{'); site(s, &b->ghost); wh_key("ok", 0); fputs(b->ghost.ok ? "true" : "false", stdout); putchar('}');
    } else fputs("null", stdout);
    wh_key("sites", 0);
    putchar('[');
    for (i = 0; i < b->nsite; i++) {
        if (i) putchar(',');
        putchar('{');
        site(s, &b->site[i]);
        wh_key("state", 0); wh_str(STATE[b->site[i].state]);
        wh_key("order", 0);
        if (b->site[i].order) printf("%d", b->site[i].order);
        else fputs("null", stdout);
        putchar('}');
    }
    putchar(']');
    wh_key("shape", 0); wh_str(SHAPE[b->shape]);
    wh_key("spacing", 0);
    if (b->spacing >= 0) printf("%d", b->spacing);
    else fputs("null", stdout);
    wh_key("cursor", 0);
    putchar('{');
    wh_key("x", 1); wh_num(b->cursor_x);
    wh_key("z", 0); wh_num(b->cursor_z);
    wh_key("shape", 0); wh_str(CURSOR[b->cursor_shape]);
    wh_key("since", 0); printf("%d", b->cursor_since);
    putchar('}');
    wh_key("keys", 0); keys_out(b->held);
    wh_key("drag", 0);
    if (b->has_drag) {
        putchar('{');
        wh_key("x0", 1); wh_num(b->drag_x0);
        wh_key("z0", 0); wh_num(b->drag_z0);
        wh_key("x1", 0); wh_num(b->drag_x1);
        wh_key("z1", 0); wh_num(b->drag_z1);
        putchar('}');
    } else fputs("null", stdout);
    wh_key("mode", 0); mode_out(s, b->rmode, b->mode);
    wh_key("reclaim", 0);
    if (b->rmode) {
        putchar('{');
        wh_key("kind", 1); wh_str(tagpu_wikidata_kind_name(b->kind));
        wh_key("ring", 0);
        if (b->has_ring) printf("{\"x\":%d,\"z\":%d,\"r\":%d}", b->ring_x, b->ring_z, b->ring_r);
        else fputs("null", stdout);
        putchar('}');
    } else fputs("null", stdout);
    putchar('}');
}

/* the map of a view, the view's map */
static void map_out(const WIKIVIEW* v)
{
    int i;
    putchar('{');
    wh_key("w", 1); printf("%d", v->w);
    wh_key("h", 0); printf("%d", v->h);
    wh_key("cell", 0); printf("%d", WIKI_CELL);
    wh_key("surfaceMetal", 0); wh_num(v->surface_metal);
    wh_key("metal", 0);
    putchar('[');
    for (i = 0; i < v->w * v->h; i++) { if (i) putchar(','); wh_num(v->metal[i]); }
    putchar(']');
    wh_key("fog", 0);
    putchar('[');
    for (i = 0; i < v->w * v->h; i++) printf("%s%d", i ? "," : "", v->fog[i]);
    putchar(']');
    wh_key("vents", 0);
    putchar('[');
    for (i = 0; i < v->nvent; i++) {
        if (i) putchar(',');
        putchar('{'); wh_key("x", 1); wh_num(v->vent[i].x); wh_key("z", 0); wh_num(v->vent[i].z); putchar('}');
    }
    fputs("]}", stdout);
}

void wh_view_as(WIKISIM* s, int map, int seat)
{
    static const char* const LINE[] = { "move", "build", "reclaim", "guard" };
    const WIKIVIEW* v = tagpu_wikisim_view_as(s, seat);
    int i, first = 1;
    putchar('{');
    if (map) {
        wh_key("map", 1);
        map_out(v);
        first = 0;
    }
    wh_key("units", first);
    putchar('[');
    for (i = 0; i < v->nunit; i++) {
        const WIKIVUNIT* u = &v->unit[i];
        if (i) putchar(',');
        putchar('{');
        wh_key("id", 1); wh_str(u->id);
        wh_key("type", 0); wh_str(name(s, u->type));
        wh_key("owner", 0); printf("%d", u->owner);
        wh_key("x", 0); wh_num(u->x);
        wh_key("z", 0); wh_num(u->z);
        wh_key("hx", 0); wh_num(u->hx);
        wh_key("hz", 0); wh_num(u->hz);
        wh_key("fx", 0); printf("%d", u->fx);
        wh_key("fz", 0); printf("%d", u->fz);
        wh_key("selected", 0); fputs(u->selected ? "true" : "false", stdout);
        wh_key("built", 0); wh_num(u->built);
        wh_key("reclaimed", 0); wh_num(u->reclaimed);
        wh_key("label", 0); wh_str(u->label);
        putchar('}');
    }
    putchar(']');
    wh_key("ghost", 0);
    if (v->has_ghost) {
        putchar('{'); site(s, &v->ghost); wh_key("ok", 0); fputs(v->ghost.ok ? "true" : "false", stdout); putchar('}');
    } else fputs("null", stdout);
    wh_key("sites", 0);
    putchar('[');
    for (i = 0; i < v->nsite; i++) {
        if (i) putchar(',');
        putchar('{');
        site(s, &v->site[i]);
        wh_key("state", 0); wh_str(STATE[v->site[i].state]);
        wh_key("order", 0);
        if (v->site[i].order) printf("%d", v->site[i].order);
        else fputs("null", stdout);
        putchar('}');
    }
    putchar(']');
    wh_key("shape", 0); wh_str(SHAPE[v->shape]);
    wh_key("spacing", 0);
    if (v->spacing >= 0) printf("%d", v->spacing);
    else fputs("null", stdout);
    wh_key("queued", 0);
    putchar('[');
    for (i = 0; i < v->nqueued; i++) {
        if (i) putchar(',');
        putchar('{'); site(s, &v->queued[i]); wh_key("owner", 0); printf("%d", v->queued[i].owner); putchar('}');
    }
    putchar(']');
    wh_key("lines", 0);
    putchar('[');
    for (i = 0; i < v->nline; i++) {
        const WIKIVLINE* l = &v->line[i];
        if (i) putchar(',');
        putchar('{');
        wh_key("from", 1); putchar('['); wh_num(l->from_x); putchar(','); wh_num(l->from_z); putchar(']');
        wh_key("to", 0); putchar('['); wh_num(l->to_x); putchar(','); wh_num(l->to_z); putchar(']');
        wh_key("kind", 0); wh_str(LINE[l->kind]);
        putchar('}');
    }
    putchar(']');
    wh_key("cursor", 0);
    putchar('{');
    wh_key("x", 1); wh_num(v->cursor_x);
    wh_key("z", 0); wh_num(v->cursor_z);
    wh_key("shape", 0); wh_str(CURSOR[v->cursor_shape]);
    wh_key("since", 0); printf("%d", v->cursor_since);
    putchar('}');
    wh_key("keys", 0); keys_out(v->held);
    wh_key("drag", 0);
    if (v->has_drag) {
        putchar('{');
        wh_key("x0", 1); wh_num(v->drag_x0);
        wh_key("z0", 0); wh_num(v->drag_z0);
        wh_key("x1", 0); wh_num(v->drag_x1);
        wh_key("z1", 0); wh_num(v->drag_z1);
        putchar('}');
    } else fputs("null", stdout);
    wh_key("caption", 0); wh_str(v->caption);
    wh_key("pace", 0); wh_num(v->pace);
    wh_key("mode", 0); mode_out(s, v->rmode, v->mode);
    wh_key("feats", 0);
    putchar('[');
    for (i = 0; i < v->nfeat; i++) {
        const WIKIVFEAT* f = &v->feat[i];
        if (i) putchar(',');
        putchar('{');
        wh_key("id", 1); wh_str(f->id);
        wh_key("name", 0); wh_str(s->defs.entry->feat[f->def].name);
        wh_key("ox", 0); printf("%d", f->ox);
        wh_key("oz", 0); printf("%d", f->oz);
        wh_key("fx", 0); printf("%d", f->fx);
        wh_key("fz", 0); printf("%d", f->fz);
        wh_key("metal", 0); wh_num(f->metal);
        wh_key("energy", 0); wh_num(f->energy);
        wh_key("wreck", 0); fputs(f->wreck ? "true" : "false", stdout);
        putchar('}');
    }
    putchar(']');
    wh_key("gone", 0);
    putchar('[');
    for (i = 0; i < v->ngone; i++) printf("%s[%d,%d]", i ? "," : "", v->gone[2 * i], v->gone[2 * i + 1]);
    putchar(']');
    wh_key("reclaim", 0);
    if (v->rmode) {
        const WIKIVSWEEP* w = &v->sweep;
        putchar('{');
        wh_key("kind", 1); wh_str(tagpu_wikidata_kind_name(w->kind));
        wh_key("ring", 0);
        if (w->has_ring) printf("{\"x\":%d,\"z\":%d,\"r\":%d}", w->ring_x, w->ring_z, w->ring_r);
        else fputs("null", stdout);
        wh_key("targets", 0);
        putchar('[');
        for (i = 0; i < w->ntarget; i++) {
            if (i) putchar(',');
            wh_str(v->feat[w->target[i]].id);
        }
        putchar(']');
        wh_key("wrecks", 0); printf("%d", w->wrecks);
        wh_key("trees", 0); printf("%d", w->trees);
        wh_key("metal", 0); wh_num(w->metal);
        wh_key("energy", 0); wh_num(w->energy);
        wh_key("hover", 0); wh_str(w->hover >= 0 ? v->feat[w->hover].id : NULL);
        wh_key("popup", 0);
        if (w->has_popup) {
            int k;
            printf("{\"rect\":[%d,%d,%d,%d],\"rows\":[", w->rect[0], w->rect[1], w->rect[2], w->rect[3]);
            for (k = 0; k < WIKI_NKINDS; k++)
                printf("%s[%d,%d,%d,%d]", k ? "," : "", w->rows[k][0], w->rows[k][1], w->rows[k][2], w->rows[k][3]);
            printf("],\"tongue\":[%d,%d,%d,%d],\"hot\":%d}", w->tongue[0], w->tongue[1], w->tongue[2], w->tongue[3], w->hot);
        } else fputs("null", stdout);
        putchar('}');
    } else fputs("null", stdout);
    if (v->has_side) {
        wh_key("side", 0);
        putchar('{');
        wh_key("index", 1); printf("%d", v->side.index);
        wh_key("h", 0); printf("%d", v->side.h);
        wh_key("screen", 0); wh_str(s->lay->screen[v->side.screen].name);
        wh_key("unit", 0); wh_str(v->side.unit[0] ? v->side.unit : NULL);
        wh_key("page", 0); printf("%d", v->side.page);
        wh_key("state", 0);
        putchar('[');
        for (i = 0; i < v->side.nstate; i++) printf("%s%d", i ? "," : "", v->side.state[i]);
        putchar(']');
        wh_key("under", 0); wh_str(v->side.under[0] ? v->side.under : NULL);
        putchar('}');
    }
    wh_key("locked", 0); wh_str(v->locked >= 0 ? name(s, v->locked) : v->locked == WIKI_LOCK_RECLAIM ? "RECLAIM" : NULL);
    wh_key("clock", 0); printf("%d", v->clock);
    wh_key("waiting", 0); wh_str(v->waiting >= WIKI_UNTIL_IDLE && v->waiting <= WIKI_UNTIL_BUILT ? UNTILS[v->waiting - 1] : NULL);
    wh_key("stalled", 0); fputs(v->stalled ? "true" : "false", stdout);
    wh_key("cob", 0);
    if (v->has_cob) printf("[%lu,%lu]", (unsigned long)(v->cob_hash >> 32), (unsigned long)(v->cob_hash & 0xFFFFFFFFu));
    else fputs("null", stdout);
    wh_key("parts", 0);
    putchar('[');
    for (i = 0; i < v->npart; i++) {
        const WIKIVPART* q = &v->part[i];
        printf("%s[%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld]", i ? "," : "", (long)q->x, (long)q->y, (long)q->z, (long)q->sx,
               (long)q->sy, (long)q->sz, (long)q->end, (long)q->col);
    }
    putchar(']');
    wh_key("partsCut", 0); fputs(v->part_cut ? "true" : "false", stdout);
    wh_key("bursts", 0);
    putchar('[');
    for (i = 0; i < v->nburst; i++) {
        const WIKISPRAY* e = &v->burst[i];
        int k;
        printf("%s[", i ? "," : "");
        for (k = 0; k < 12; k++) printf("%ld,", (long)e->box[k / 6][k % 6]);
        printf("%d,%lu]", e->due, (unsigned long)e->key);
    }
    putchar(']');
    wh_key("sound", 0);
    if (v->has_sound) {
        putchar('{');
        wh_key("next", 1);
        putchar('[');
        for (i = 0; i < WIKI_SND_KINDS; i++) printf("%s%d", i ? "," : "", v->snd_next[i]);
        putchar(']');
        wh_key("last", 0); printf("%d", v->snd_last);
        wh_key("queue", 0);
        putchar('[');
        for (i = 0; i < v->nsndq; i++) {
            const WIKISNDQ* q = &v->sndq[i];
            printf("%s[%d,%d,", i ? "," : "", q->kind, q->tick);
            wh_str(q->unit);
            putchar(',');
            wh_str(name(s, q->type));
            printf(",%d]", q->priority);
        }
        putchar(']');
        wh_key("total", 0); printf("%d", v->snd_total);
        wh_key("events", 0);
        putchar('[');
        for (i = 0; i < v->nsnd; i++)
            printf("%s[%d,%d,%d,%d,%d]", i ? "," : "", v->snd[i].tick, v->snd[i].kind, v->snd[i].name, v->snd[i].cat,
                   v->snd[i].variant);
        putchar(']');
        wh_key("cant", 0);
        putchar('[');
        for (i = 0; i < v->nunit; i++) printf("%s%d", i ? "," : "", v->unit[i].cant);
        fputs("]}", stdout);
    } else fputs("null", stdout);
    /* a game's own part, the view's `game`, only in a game */
    if (v->game) {
        wh_key("game", 0);
        putchar('{');
        wh_key("seats", 1);
        putchar('[');
        for (i = 0; i < v->nseat; i++) {
            const WIKIECON* z = &v->econ[i];
            const double d[12] = { z->m, z->e, z->bm, z->be, z->mmax, z->emax, z->mi, z->mo, z->ei, z->eo, z->wm, z->we };
            static const char* const D[12] = { "m", "e", "bm", "be", "mmax", "emax", "mi", "mo", "ei", "eo", "wm", "we" };
            int k;
            if (i) putchar(',');
            putchar('{');
            wh_key("name", 1); wh_str(v->seat[i].name);
            for (k = 0; k < 12; k++) { wh_key(D[k], 0); wh_num(d[k]); }
            wh_key("units", 0); printf("%d", z->units);
            wh_key("lost", 0); printf("%d", z->lost);
            wh_key("out", 0); fputs(z->out ? "true" : "false", stdout);
            wh_key("cmdLost", 0); fputs(z->cmd_lost ? "true" : "false", stdout);
            wh_key("kills", 0); printf("%d", z->kills);
            wh_key("losses", 0); printf("%d", z->losses);
            putchar('}');
        }
        putchar(']');
        wh_key("mode", 0); wh_str(v->econ_mode == WIKI_ECON_STALL ? "stall" : "refuse");
        wh_key("starved", 0); fputs(v->starved ? "true" : "false", stdout);
        wh_key("badKill", 0); fputs(v->bad_kill ? "true" : "false", stdout);
        wh_key("nkill", 0); printf("%d", v->nkill);
        wh_key("ledgers", 0);
        putchar('[');
        for (i = 0; v->ledger && i < v->nunit; i++) {
            const WIKILEDGER* L = &v->ledger[i];
            const double d[8] = { L->eP, L->eR, L->eG, L->eD, L->mP, L->mR, L->mG, L->mD };
            int k;
            if (i) putchar(',');
            putchar('[');
            for (k = 0; k < 8; k++) { if (k) putchar(','); wh_num(d[k]); }
            putchar(']');
        }
        putchar(']');
        wh_key("cursors", 0);
        putchar('[');
        for (i = 0; i < v->ncursor; i++) {
            printf("%s{", i ? "," : "");
            wh_key("x", 1); wh_num(v->cursor[i].x);
            wh_key("z", 0); wh_num(v->cursor[i].z);
            wh_key("on", 0); fputs(v->cursor[i].on ? "true" : "false", stdout);
            putchar('}');
        }
        putchar(']');
        wh_key("clicks", 0);
        putchar('[');
        for (i = 0; i < v->nclick; i++) {
            printf("%s{", i ? "," : "");
            wh_key("tick", 1); printf("%d", v->click[i].tick);
            wh_key("seat", 0); printf("%d", v->click[i].seat);
            wh_key("x", 0); wh_num(v->click[i].x);
            wh_key("z", 0); wh_num(v->click[i].z);
            putchar('}');
        }
        putchar(']');
        wh_key("deaths", 0);
        putchar('[');
        for (i = 0; i < v->ndeath; i++) {
            printf("%s{", i ? "," : "");
            wh_key("tick", 1); printf("%d", v->death[i].tick);
            wh_key("owner", 0); printf("%d", v->death[i].owner);
            wh_key("x", 0); wh_num(v->death[i].x);
            wh_key("z", 0); wh_num(v->death[i].z);
            wh_key("type", 0); wh_str(name(s, v->death[i].type));
            putchar('}');
        }
        putchar(']');
        wh_key("types", 0);
        putchar('[');
        for (i = 0; i < v->ntype; i++) {
            printf("%s{", i ? "," : "");
            wh_key("name", 1); wh_str(name(s, i));
            wh_key("sightDistance", 0); printf("%d", v->type[i].sight_distance);
            wh_key("radarDistance", 0); printf("%d", v->type[i].radar_distance);
            wh_key("sonarDistance", 0); printf("%d", v->type[i].sonar_distance);
            wh_key("activates", 0); fputs(v->type[i].activates ? "true" : "false", stdout);
            wh_key("modelHeight", 0); printf("%d", v->type[i].model_height);
            putchar('}');
        }
        fputs("]}", stdout);
    }
    /* the input seats beyond seat 0, the view's nin, viewed and inputs, in a game with more than one */
    if (v->game && v->nin > 1) {
        wh_key("nin", 0); printf("%d", v->nin);
        wh_key("viewed", 0); printf("%d", v->viewed);
        wh_key("inputs", 0);
        putchar('[');
        for (i = 0; i < v->nin - 1; i++) {
            if (i) putchar(',');
            seat_out(s, &v->input[i]);
        }
        putchar(']');
    }
    putchar('}');
}

/* the state hash as 16 hex digits, in two halves: mingw's printf need not know %llx */
void wh_hash(const WIKISIM* s)
{
    const uint64_t h = tagpu_wikisim_hash(s);
    printf("\"%08lx%08lx\"", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFu));
}

void wh_sight_out(WIKISIM* s, const WIKISIGHT* t)
{
    static uint8_t lit[WIKI_SIGHT_GRID];
    const WIKIVIEW* v = tagpu_wikisim_view(s);
    uint8_t* con = (uint8_t*)malloc((size_t)v->nunit + 1);
    int k, i, n;
    putchar('[');
    for (k = 0; k < v->nseat; k++) {
        uint32_t h = 0x811C9DC5u;
        int on = 0;
        n = tagpu_wikisim_sight_lit(v, t, k, s->sim.tick, lit);
        for (i = 0; i < n; i++) {
            h = (h ^ lit[i]) * 0x01000193u;
            on += lit[i] != 0;
        }
        printf("%s{", k ? "," : "");
        wh_key("lit", 1); printf("\"%08lx\"", (unsigned long)h);
        wh_key("n", 0); printf("%d", on);
        wh_key("c", 0); putchar('"');
        if (con) {
            tagpu_wikisim_contacts(v, lit, k, con);
            for (i = 0; i < v->nunit; i++) putchar('0' + con[i]);
        }
        fputs("\"}", stdout);
    }
    putchar(']');
    free(con);
}

/* bytes as base64 */
static void base64(const unsigned char* p, size_t n)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    putchar('"');
    for (i = 0; i + 2 < n; i += 3) {
        const unsigned v = (unsigned)p[i] << 16 | (unsigned)p[i + 1] << 8 | p[i + 2];
        putchar(A[v >> 18]); putchar(A[(v >> 12) & 63]); putchar(A[(v >> 6) & 63]); putchar(A[v & 63]);
    }
    if (n - i == 1) {
        const unsigned v = (unsigned)p[i] << 16;
        putchar(A[v >> 18]); putchar(A[(v >> 12) & 63]); fputs("==", stdout);
    } else if (n - i == 2) {
        const unsigned v = (unsigned)p[i] << 16 | (unsigned)p[i + 1] << 8;
        putchar(A[v >> 18]); putchar(A[(v >> 12) & 63]); putchar(A[(v >> 6) & 63]); putchar('=');
    }
    putchar('"');
}

/* ---- a tree written back as JSON text: the entry loader takes text, and a request carries the
   entry as a member of its own tree. %.17g gives back each number's double and the strings are
   re-escaped, so the loader reads the values the page sent ---- */

typedef struct { char* p; size_t n, cap; int oom; } BUF;

static void buf_put(BUF* b, const char* s, size_t n)
{
    if (b->oom) return;
    if (!b->p || b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        char* q;
        while (c < b->n + n + 1) c *= 2;
        if (!(q = (char*)realloc(b->p, c))) { b->oom = 1; return; }
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_str(BUF* b, const char* s)
{
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; buf_put(b, e, 2); }
        else if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); buf_put(b, e, 6); }
        else buf_put(b, (const char*)&c, 1);
    }
    buf_put(b, "\"", 1);
}

static void tjv_text(BUF* b, const TJV* v)
{
    const TJV* c;
    char n[32];
    switch (v->type) {
    case TJ_NULL: buf_put(b, "null", 4); break;
    case TJ_FALSE: buf_put(b, "false", 5); break;
    case TJ_TRUE: buf_put(b, "true", 4); break;
    case TJ_NUM: buf_put(b, n, (size_t)snprintf(n, sizeof n, "%.17g", v->num)); break;
    case TJ_STR: buf_str(b, v->str); break;
    case TJ_ARR:
        buf_put(b, "[", 1);
        for (c = v->first; c; c = c->next) { if (c != v->first) buf_put(b, ",", 1); tjv_text(b, c); }
        buf_put(b, "]", 1);
        break;
    default:
        buf_put(b, "{", 1);
        for (c = v->first; c; c = c->next) {
            if (c != v->first) buf_put(b, ",", 1);
            buf_str(b, c->key);
            buf_put(b, ":", 1);
            tjv_text(b, c);
        }
        buf_put(b, "}", 1);
    }
}

/* ---- the handles ---- */

/* A request's tree, kept by every sim that took an input or a fork part from it */
typedef struct { TJDOC* doc; int refs; } HDOC;

static void doc_unref(HDOC* d)
{
    if (d && --d->refs == 0) {
        tagpu_json_free(d->doc);
        free(d);
    }
}

enum { K_ENTRY, K_LAYOUT, K_CASTING, K_PACKS, K_TABLE, NKINDS };
static const char* const KIND[NKINDS] = { "entry", "layout", "casting", "packs", "table" };

/* An object a handle names; its table holds one reference while it is in it, each sim made from
   it one more */
typedef struct {
    int                h, kind, refs;
    unsigned long long used;
    union {
        WIKIENTRY*   entry;
        WIKILAYOUT*  layout;
        WIKICASTING* casting;
        COBFILES*    packs;
        WIKISIGHT*   table;
        void*        any;
    } u;
    char* key;                      /* packs: the dir list, which a set is cached by */
} HOBJ;

typedef struct { char id[WIKI_ID_MAX]; int type, mobile, ox, oz; double built; } LUNIT;
typedef struct { char id[WIKI_ID_MAX]; int def, ox, oz, fx, fz; } LFEAT;

typedef struct {
    int                h;
    unsigned long long used;
    WIKISIM            s;
    HOBJ              *entry, *layout, *cast, *rival;
    HDOC**             doc;
    int                ndoc, cdoc;
    LUNIT*             lu;          /* THE LOG's last state */
    LFEAT*             lf;
    int                nlu, nlf;
} HSIM;

static struct {
    int                next;        /* the last handle given */
    unsigned long long clock;
    HOBJ*              obj[NKINDS][WH_OBJS];
    HSIM*              sim[WH_SIMS];
    BUF                log;         /* this call's log entries, comma-separated */
    int                nlog;
    char               instance[WH_INSTANCE_MAX + 1];   /* THE INSTANCE, "" none */
    const char*        root;        /* THE PACKS' ROOT as given; NULL none */
} H;

static void obj_unref(HOBJ* o)
{
    if (!o || --o->refs) return;
    switch (o->kind) {
    case K_ENTRY: tagpu_wikidata_entry_free(o->u.entry); free(o->u.entry); break;
    case K_PACKS: cob_free(o->u.packs); free(o->u.packs); break;
    default: free(o->u.any);
    }
    free(o->key);
    free(o);
}

/* `p` in a new object of `kind` in its table, the least recently used one leaving a full table;
   NULL when memory runs out, `p` then freed by the caller */
static HOBJ* obj_add(int kind, void* p)
{
    HOBJ* o = (HOBJ*)calloc(1, sizeof *o);
    int i, slot = 0;
    if (!o) return NULL;
    for (i = 0; i < WH_OBJS; i++) {
        if (!H.obj[kind][i]) { slot = i; break; }
        if (H.obj[kind][i]->used < H.obj[kind][slot]->used) slot = i;
    }
    obj_unref(H.obj[kind][slot]);
    o->h = ++H.next;
    o->kind = kind;
    o->refs = 1;
    o->used = ++H.clock;
    o->u.any = p;
    H.obj[kind][slot] = o;
    return o;
}

/* the object handle `v` names in `kind`'s table, or NULL with the error in `err` */
static HOBJ* obj_get(int kind, const TJV* v, char* err, size_t errn)
{
    const double h = tj_num(v, -1);
    int i;
    for (i = 0; i < WH_OBJS; i++)
        if (H.obj[kind][i] && H.obj[kind][i]->h == h) {
            H.obj[kind][i]->used = ++H.clock;
            return H.obj[kind][i];
        }
    snprintf(err, errn, "no %s %.17g", KIND[kind], h);
    return NULL;
}

/* an optional handle: absent or null is none (1, *o NULL); 0 with the error for one no table holds */
static int obj_opt(int kind, const TJV* v, HOBJ** o, char* err, size_t errn)
{
    *o = NULL;
    return !v || tj_is(v, TJ_NULL) || (*o = obj_get(kind, v, err, errn)) != NULL;
}

static void sim_free(HSIM* m)
{
    int i;
    if (!m) return;
    tagpu_wikisim_free(&m->s);
    obj_unref(m->entry);
    obj_unref(m->layout);
    obj_unref(m->cast);
    obj_unref(m->rival);
    for (i = 0; i < m->ndoc; i++) doc_unref(m->doc[i]);
    free(m->doc);
    free(m->lu);
    free(m->lf);
    free(m);
}

static HSIM* sim_get(const TJV* v, char* err, size_t errn)
{
    const double h = tj_num(tj_get(v, "sim"), -1);
    int i;
    for (i = 0; i < WH_SIMS; i++)
        if (H.sim[i] && H.sim[i]->h == h) {
            H.sim[i]->used = ++H.clock;
            return H.sim[i];
        }
    snprintf(err, errn, "no sim %.17g", h);
    return NULL;
}

/* the sim keeps the request's tree that one of its inputs points into */
static int sim_keep(HSIM* m, HDOC* d)
{
    if (m->ndoc && m->doc[m->ndoc - 1] == d) return 1;
    if (m->ndoc == m->cdoc) {
        const int c = m->cdoc ? 2 * m->cdoc : 8;
        HDOC** q = (HDOC**)realloc(m->doc, (size_t)c * sizeof *q);
        if (!q) return 0;
        m->doc = q;
        m->cdoc = c;
    }
    m->doc[m->ndoc++] = d;
    d->refs++;
    return 1;
}

/* A request's number as an int: truncated, as a cast would, but held to +-2^30 first, since a
   double past int's range has no defined conversion and a request may carry any number; `dflt`
   when it is no number. No count, tick or index the host passes on comes near 2^30. */
static int num_int(const TJV* v, double dflt)
{
    const double d = tj_num(v, dflt), M = 1 << 30;
    return (int)(d < -M ? -M : d > M ? M : d);
}

/* a call's `seat`: an integer as written, 0 when absent, and -1, which no sim holds, for a seat
   that is no integer, so the sim's own check refuses it */
static int seat_arg(const TJV* v)
{
    const double k = tj_num(tj_get(v, "seat"), 0);
    return k == num_int(tj_get(v, "seat"), 0) ? (int)k : -1;
}

/* ---- THE LOG ---- */

static void log_entry(int t, const char* what, const char* id, const char* type, int has_site, int ox, int oz)
{
    char head[64];
    BUF* b = &H.log;
    if (H.nlog++) buf_put(b, ",", 1);
    buf_put(b, head, (size_t)snprintf(head, sizeof head, "{\"t\":%d,\"what\":", t));
    buf_str(b, what);
    buf_put(b, ",\"id\":", 6);
    buf_str(b, id);
    buf_put(b, ",\"type\":", 8);
    buf_str(b, type);
    if (has_site) buf_put(b, head, (size_t)snprintf(head, sizeof head, ",\"ox\":%d,\"oz\":%d", ox, oz));
    buf_put(b, "}", 1);
}

/* THE LOG'S MEMORY: an answer is printed as it is made, so a log that cannot be kept cannot become
   an error answer; the host exits instead, and the channel answers every request after it with
   the host's exit (tools/wiki/hostchan.cjs), so nothing reads a log with a gap in it */
static void log_oom(void)
{
    fputs("wikihost: out of memory for the log\n", stderr);
    exit(2);
}

/* the sim's units and features as the log's last state */
static void log_take(HSIM* m)
{
    const WIKISIM* s = &m->s;
    LUNIT* lu = (LUNIT*)malloc(((size_t)s->sim.nunit + 1) * sizeof *lu);
    LFEAT* lf = (LFEAT*)malloc(((size_t)s->sim.nfeat + 1) * sizeof *lf);
    int i;
    if (!lu || !lf) log_oom();
    for (i = 0; i < s->sim.nunit; i++) {
        const WIKIUNIT* u = s->sim.unit[i];
        memcpy(lu[i].id, u->id, sizeof lu[i].id);
        lu[i].type = u->type;
        lu[i].mobile = tasim_mobile(&s->defs, u->type);
        lu[i].ox = u->ox;
        lu[i].oz = u->oz;
        lu[i].built = u->built;
    }
    for (i = 0; i < s->sim.nfeat; i++) {
        memcpy(lf[i].id, s->sim.feat[i].id, sizeof lf[i].id);
        lf[i].def = s->sim.feat[i].def;
        lf[i].ox = s->sim.feat[i].ox;
        lf[i].oz = s->sim.feat[i].oz;
        lf[i].fx = s->sim.feat[i].fx;
        lf[i].fz = s->sim.feat[i].fz;
    }
    free(m->lu);
    free(m->lf);
    m->lu = lu;
    m->lf = lf;
    m->nlu = s->sim.nunit;
    m->nlf = s->sim.nfeat;
}

static int killed_in(const char* id, const WIKIINPUT* const* in, int a, int b)
{
    int i;
    for (i = a; i < b; i++)
        if ((in[i]->has & WIKI_IN_KILL) && in[i]->kill && !strcmp(in[i]->kill, id)) return 1;
    return 0;
}

/* a feature standing now, by id, or NULL */
static const WIKIFEAT* feat_now(const WIKISIM* s, const char* id)
{
    int i;
    for (i = 0; i < s->sim.nfeat; i++)
        if (!strcmp(s->sim.feat[i].id, id)) return &s->sim.feat[i];
    return NULL;
}

static int had_feat(const HSIM* m, const char* id)
{
    int i;
    for (i = 0; i < m->nlf; i++)
        if (!strcmp(m->lf[i].id, id)) return 1;
    return 0;
}

/* a killed unit's entries: its wreck or why none, then the kill */
static void log_killed(const HSIM* m, int t, const char* id, int type, int mobile, int ox, int oz, double built)
{
    const WIKISIM* s = &m->s;
    if (feat_now(s, id) && !had_feat(m, id)) log_entry(t, "corpse", id, name(s, type), !mobile, ox, oz);
    else if (built >= 1 && s->defs.entry->def[type].corpse >= 0) log_entry(t, "nocorpse", id, name(s, type), !mobile, ox, oz);
    log_entry(t, "killed", id, name(s, type), !mobile, ox, oz);
}

/* the entries of what changed since the last state, `t` the tick the change began on and the
   script's inputs [k0, k1) the ones fed meanwhile; then the state taken anew. Both lists keep
   creation order and only append, so a walk of the two meets each unit and feature once. */
static void log_diff(HSIM* m, int t, int k0, int k1)
{
    const WIKISIM* s = &m->s;
    int i = 0, j = 0, n;
    for (; i < m->nlu; i++) {
        const LUNIT* a = &m->lu[i];
        if (j < s->sim.nunit && !strcmp(s->sim.unit[j]->id, a->id)) {
            if (a->built < 1 && s->sim.unit[j]->built >= 1) log_entry(t, "finished", a->id, name(s, a->type), !a->mobile, a->ox, a->oz);
            j++;
        } else if (killed_in(a->id, s->script, k0, k1)) log_killed(m, t, a->id, a->type, a->mobile, a->ox, a->oz, a->built);
        else log_entry(t, "reclaimed", a->id, name(s, a->type), !a->mobile, a->ox, a->oz);
    }
    for (; j < s->sim.nunit; j++) {
        const WIKIUNIT* u = s->sim.unit[j];
        const int mob = tasim_mobile(&s->defs, u->type);
        log_entry(t, "started", u->id, name(s, u->type), !mob, u->ox, u->oz);
        if (u->built >= 1) log_entry(t, "finished", u->id, name(s, u->type), !mob, u->ox, u->oz);
    }
    /* a feature gone: reclaimed, unless a wreck that appeared covers it */
    for (i = 0, j = 0; i < m->nlf; i++) {
        const LFEAT* f = &m->lf[i];
        int crushed = 0;
        if (j < s->sim.nfeat && !strcmp(s->sim.feat[j].id, f->id)) { j++; continue; }
        for (n = 0; n < s->sim.nfeat && !crushed; n++) {
            const WIKIFEAT* w = &s->sim.feat[n];
            crushed = !had_feat(m, w->id) && f->ox < w->ox + w->fx && w->ox < f->ox + f->fx && f->oz < w->oz + w->fz
                      && w->oz < f->oz + f->fz;
        }
        if (!crushed) log_entry(t, "reclaimed", f->id, s->defs.entry->feat[f->def].name, 1, f->ox, f->oz);
    }
    if (H.log.oom) log_oom();
    log_take(m);
}

/* THE LOG OF A SIM MADE: the scene's units its script's tick-0 kills removed, each from its scene
   record (an explicit id, which a kill names); a structure's site as tasim_add places it */
static void log_made(HSIM* m, const WIKIDEMO* d)
{
    const WIKISIM* s = &m->s;
    const WIKIENTRY* e = s->defs.entry;
    const WIKISCENE* sc[2];
    int k, i, j;
    sc[0] = &e->scenes[d ? d->on : 0];
    sc[1] = d ? &d->scene : NULL;
    for (k = 0; k < s->next; k++) {
        const WIKIINPUT* in = s->script[k];
        if (!(in->has & WIKI_IN_KILL) || !in->kill) continue;
        for (i = 0; i < 2; i++)
            for (j = 0; sc[i] && j < sc[i]->nunit; j++) {
                const WIKISCENEUNIT* u = &sc[i]->unit[j];
                const int mob = tasim_mobile(&s->defs, u->def);
                if (strcmp(u->id, in->kill)) continue;
                log_killed(m, 0, u->id, u->def, mob,
                           mob ? 0 : tasim_bound_cell(tasim_js_round(u->x / CELL - tasim_fpx(&s->defs, u->def) / 2.0)),
                           mob ? 0 : tasim_bound_cell(tasim_js_round(u->z / CELL - tasim_fpz(&s->defs, u->def) / 2.0)), u->built);
            }
    }
    if (H.log.oom) log_oom();
}

/* ---- the answers ---- */

static void error(const char* why)
{
    fputs("{\"error\":", stdout);
    wh_str(why);
    putchar('}');
}

static void bool_out(int b) { fputs(b ? "true" : "false", stdout); }

static void str_list(const char* (*f)(int), int n)
{
    int k;
    putchar('[');
    for (k = 0; k < n; k++) { if (k) putchar(','); wh_str(f(k)); }
    putchar(']');
}

static const char* sndname(int k)
{
    static const char* const N[] = WIKI_SNDNAME_STRINGS;
    return N[k];
}
static const char* untilname(int k) { return UNTILS[k]; }

/* the controls k of the HUD's whose `in(k)` holds, in their order */
static void hud_list(int (*in)(int))
{
    int k, n = 0;
    putchar('[');
    for (k = 0; k < WIKI_NHUD; k++)
        if (in(k)) { if (n++) putchar(','); wh_str(tagpu_wikidata_hud_name(k)); }
    putchar(']');
}
static int hud_all(int k) { (void)k; return 1; }
static int hud_replay(int k) { return tagpu_wikidata_hud_presses(k, 0); }
static int hud_live(int k) { return tagpu_wikidata_hud_presses(k, 1); }
static int hud_takeover(int k) { return tagpu_wikidata_hud_takeover(k) && !tagpu_wikidata_hud_presses(k, 0) && !tagpu_wikidata_hud_presses(k, 1); }
static int hud_players(int k) { return tagpu_wikidata_hud_players(k) && !tagpu_wikidata_hud_presses(k, 0); }

static void op_hello(void)
{
    static const WIKITRIPROW rows[WIKI_TRIP_NROWS] = WIKI_TRIP_ROWS;
    int k;
    fputs("{\"version\":1,\"instance\":", stdout);
    wh_str(H.instance);
    printf(",\"constants\":{\"TRIP\":%d,\"TRIP_DOWN\":%d,\"TRIP_UP\":%d,\"PICK\":%d,\"PICK_DOWN\":%d,"
           "\"PICK_UP\":%d,\"TRIP_ROWS\":[", WIKI_TRIP, WIKI_TRIP_DOWN, WIKI_TRIP_UP, WIKI_PICK, WIKI_PICK_DOWN, WIKI_PICK_UP);
    for (k = 0; k < WIKI_TRIP_NROWS; k++)
        printf("%s{\"glide\":%d,\"hold\":%d,\"gap\":%d,\"back\":%d,\"hover\":%d}", k ? "," : "", rows[k].glide,
               rows[k].hold, rows[k].gap, rows[k].back, rows[k].hover);
    fputs("],\"KINDS\":", stdout); str_list(tagpu_wikidata_kind_name, WIKI_NKINDS);
    fputs(",\"KEYS\":", stdout); str_list(tagpu_wikidata_key_name, WIKI_NKEYS);
    fputs(",\"SND_NAMES\":", stdout); str_list(sndname, WIKI_NSNDNAME);
    fputs(",\"UNTILS\":", stdout); str_list(untilname, WIKI_UNTIL_BUILT);
    fputs(",\"CASTS\":", stdout); str_list(tagpu_wikidata_cast_name, WIKI_NCAST);
    fputs(",\"HUD_CONTROLS\":", stdout); hud_list(hud_all);
    fputs(",\"REPLAY_CONTROLS\":", stdout); hud_list(hud_replay);
    fputs(",\"LIVE_CONTROLS\":", stdout); hud_list(hud_live);
    fputs(",\"TAKEOVER_CONTROLS\":", stdout); hud_list(hud_takeover);
    fputs(",\"PLAYERS_CONTROLS\":", stdout); hud_list(hud_players);
    fputs(",\"CHIPS\":", stdout); str_list(tagpu_wikidata_chip_name, WIKI_NCHIP);
    printf(",\"SND_DEATH\":%d,\"SEATS\":%d,\"GAME_KILLS\":%d,\"GAME_END_MAX\":%d,\"FEATS_MAX\":%d,\"HUD_DOWN\":%d,"
           "\"HUD_UP\":%d,\"PLAYERS_DEATHS\":%d,\"PLAYERS_DEATH_SPAN\":%d,\"SIGHT_MIN\":%d,\"SIGHT_NUMTABLES\":%d,"
           "\"SIGHT_IDX\":%d,\"SIGHT_REACH\":%d,\"SIGHT_CELLS\":%d,\"SIGHT_SPAN\":%d,\"DEMO_FORKS\":%d,\"TIMELINES\":%d}}",
           WIKI_SND_DEATH, WIKI_SEATS, WIKI_GAME_KILLS, WIKI_GAME_END_MAX, WIKI_FEATS_MAX, WIKI_REPLAY_HUD_DOWN,
           WIKI_REPLAY_HUD_UP, WIKI_PLAYERS_DEATHS, WIKI_PLAYERS_DEATH_SPAN, WIKI_SIGHT_MIN, WIKI_SIGHT_NUMTABLES,
           WIKI_SIGHT_IDX, WIKI_SIGHT_REACH, WIKI_SIGHT_CELLS, WIKI_SIGHT_SPAN, WIKI_DEMO_FORKS, RECR_TIMELINES);
}

/* an entry from the request's tree: written back as text for the loader; 1, or 0 with why */
static int entry_load(const TJV* v, WIKIENTRY* e, char* err, size_t errn)
{
    BUF b = { NULL, 0, 0, 0 };
    int ok;
    if (!tj_is(v, TJ_OBJ)) { snprintf(err, errn, "the entry is not an object"); return 0; }
    tjv_text(&b, v);
    if (b.oom) { free(b.p); snprintf(err, errn, "out of memory"); return 0; }
    ok = tagpu_wikidata_entry(e, b.p, b.n, err, errn);
    free(b.p);
    return ok;
}

static void made(int kind, void* p, void (*fr)(void*))
{
    HOBJ* o = obj_add(kind, p);
    if (!o) { fr(p); error("out of memory"); return; }
    printf("{\"%s\":%d}", KIND[kind], o->h);
}

static void entry_drop(void* p) { tagpu_wikidata_entry_free((WIKIENTRY*)p); free(p); }
static void packs_drop(void* p) { cob_free((COBFILES*)p); free(p); }

static void op_entry(const TJV* v)
{
    char err[256];
    WIKIENTRY* e = (WIKIENTRY*)malloc(sizeof *e);
    if (!e) { error("out of memory"); return; }
    if (!entry_load(tj_get(v, "json"), e, err, sizeof err)) { free(e); error(err); return; }
    made(K_ENTRY, e, entry_drop);
}

static void op_layout(const TJV* v)
{
    char err[256];
    WIKILAYOUT* L = tagpu_wikidata_layout_json(tj_get(v, "json"), err, sizeof err);
    if (!L) { error(err); return; }
    made(K_LAYOUT, L, free);
}

static void op_casting(const TJV* v)
{
    char err[256];
    WIKICASTING* k = (WIKICASTING*)malloc(sizeof *k);
    if (!k) { error("out of memory"); return; }
    if (!tagpu_wikidata_casting(tj_get(v, "json"), k, err, sizeof err)) { free(k); error(err); return; }
    made(K_CASTING, k, free);
}

static void op_table(const TJV* v)
{
    char err[256];
    WIKISIGHT* t = (WIKISIGHT*)malloc(sizeof *t);
    if (!t) { error("out of memory"); return; }
    if (!tagpu_wikidata_sight(tj_get(v, "json"), t, err, sizeof err)) { free(t); error(err); return; }
    made(K_TABLE, t, free);
}

/* THE PACKS' ROOT (serve --packs-root DIR): every file a pack read opens is resolved first, links
   and ".." included, and refused unless it lies under the resolved root, so a pack folder, a
   scene.json or a script file that is a link out of the root (an instance's game folder links
   into the game's install) reads nothing. The file is then read at the resolved path, so the
   path checked is the path read. The root is resolved at each check too: one that does not
   exist (no pack built yet) holds no file. Without a root every path is read as given. */
static int pack_at(const char* path, char* out, size_t n)
{
    if (!H.root) { snprintf(out, n, "%s", path); return 1; }
#ifdef _WIN32
    return -1;                      /* unreachable: wikitest refuses --packs-root on _WIN32 */
#else
    char *root, *real = NULL;
    size_t k;
    int r = 0;
    if ((root = realpath(H.root, NULL)) && (real = realpath(path, NULL))) {
        k = strlen(root);
        r = !strncmp(real, root, k) && (real[k] == '/' || root[k - 1] == '/') && strlen(real) < n ? 1 : -1;
        if (r > 0) memcpy(out, real, strlen(real) + 1);
    }
    free(root);
    free(real);
    return r;
#endif
}

/* a set of gfx packs, cached by its list: each a repository-relative folder, no "..", read once */
static void op_packs(const TJV* v)
{
    char err[256];
    const TJV *dirs = tj_get(v, "dirs"), *d;
    const char* list[64];
    BUF key = { NULL, 0, 0, 0 };
    COBFILES* f;
    HOBJ* o;
    int n = 0, k;
    if (!tj_is(dirs, TJ_ARR) || dirs->n > 64) { error("dirs is not a list of at most 64 folders"); return; }
    for (d = dirs->first; d; d = d->next) {
        if (!tj_is(d, TJ_STR) || !d->str[0] || d->str[0] == '/' || d->str[0] == '\\' || strstr(d->str, "..")
            || strchr(d->str, ':')) {
            free(key.p);
            error("a pack is not a folder under the repository");
            return;
        }
        list[n++] = d->str;
        buf_str(&key, d->str);
    }
    buf_put(&key, "", 0);
    if (key.oom) { free(key.p); error("out of memory"); return; }
    for (k = 0; k < WH_OBJS; k++) {
        o = H.obj[K_PACKS][k];
        if (o && o->key && !strcmp(o->key, key.p)) {
            o->used = ++H.clock;
            free(key.p);
            printf("{\"packs\":%d}", o->h);
            return;
        }
    }
    if (!(f = (COBFILES*)malloc(sizeof *f))) { free(key.p); error("out of memory"); return; }
    cob_init(f);
    if (!cob_packs(f, list, n, pack_at, err, sizeof err)) { packs_drop(f); free(key.p); error(err); return; }
    if (!(o = obj_add(K_PACKS, f))) { packs_drop(f); free(key.p); error("out of memory"); return; }
    o->key = key.p;
    printf("{\"packs\":%d}", o->h);
}

static void op_text(const TJV* v)
{
    char err[256];
    WIKIENTRY e;
    if (entry_load(tj_get(v, "json"), &e, err, sizeof err)) {
        tagpu_wikidata_entry_free(&e);
        fputs("{\"why\":null}", stdout);
    } else {
        fputs("{\"why\":", stdout);
        wh_str(err);
        putchar('}');
    }
}

static void op_scene(const TJV* v)
{
    char err[64];
    HOBJ* o = obj_get(K_ENTRY, tj_get(v, "entry"), err, sizeof err);
    const WIKIENTRY* e;
    if (!o) { error(err); return; }
    e = o->u.entry;
    if (!e->nscene) { error("the entry has no scene"); return; }
    fputs("{\"scene\":", stdout);
    wh_str(e->scenes[e->ndemo ? e->demo[0].on : 0].name);
    putchar('}');
}

static void op_seatcam(const TJV* v)
{
    char err[64];
    HOBJ* o = obj_get(K_ENTRY, tj_get(v, "entry"), err, sizeof err);
    const WIKIENTRY* e;
    int x, z;
    if (!o) { error(err); return; }
    e = o->u.entry;
    if (!e->ndemo || !e->nscene || !e->demo[0].has_game
        || !tagpu_wikidata_seat_cam(&e->scenes[e->demo[0].on], &e->demo[0].game, seat_arg(v), num_int(tj_get(v, "tick"), 0), &x, &z))
        fputs("{\"at\":null}", stdout);
    else printf("{\"at\":[%d,%d]}", x, z);
}

static int int_at(const TJV* a, int i) { return num_int(tj_at(a, i), 0); }

/* a view position of `a` at `i`, held to two maps' width each way: the core converts positions to
   int and 16.16, defined for any position a sim's view holds and not for any double; 1, or 0 */
static int pos_at(const TJV* a, int i, double* x)
{
    *x = tj_num(tj_at(a, i), 0);
    return *x >= -2.0 * WIKI_MAP_MAX * WIKI_CELL && *x <= 2.0 * WIKI_MAP_MAX * WIKI_CELL;
}

/* THE SIGHT OVER A GIVEN VIEW: the view's parts sightLit and sightContacts read, a type by its
   index in `types`, one out of range none (the core skips it) */
static void op_sight(const TJV* v)
{
    static uint8_t lit[WIKI_SIGHT_GRID];
    static const WIKISIGHT none;
    char err[64];
    const TJV *view = tj_get(v, "view"), *us = tj_get(view, "units"), *ds = tj_get(view, "deaths"), *ts = tj_get(view, "types"), *c;
    HOBJ* t;
    WIKIVIEW w;
    WIKIVUNIT* unit;
    WIKIDEATH* death;
    WIKIVTYPE* type;
    uint8_t* con;
    int n, i, nu, nd, nt, ok = 1;
    if (!obj_opt(K_TABLE, tj_get(v, "table"), &t, err, sizeof err)) { error(err); return; }
    nu = tj_is(us, TJ_ARR) ? us->n : 0;
    nd = tj_is(ds, TJ_ARR) ? ds->n : 0;
    nt = tj_is(ts, TJ_ARR) ? ts->n : 0;
    /* what a sim's view holds: its deaths in WIKISIM's death[WIKI_FEATS_MAX], its types an
       entry's, and its units WH_SIGHT_UNITS */
    if (nu > WH_SIGHT_UNITS || nd > WIKI_FEATS_MAX || nt > WIKI_MAX_DEFS) {
        error("the view holds more units, deaths or types than a sim's view does");
        return;
    }
    memset(&w, 0, sizeof w);
    w.w = num_int(tj_get(view, "w"), 0);
    w.h = num_int(tj_get(view, "h"), 0);
    if (w.w < 0 || w.h < 0 || w.w > WIKI_MAP_MAX || w.h > WIKI_MAP_MAX) { error("the view's map is not 0..WIKI_MAP_MAX a side"); return; }
    unit = (WIKIVUNIT*)calloc((size_t)nu + 1, sizeof *unit);
    death = (WIKIDEATH*)calloc((size_t)nd + 1, sizeof *death);
    type = (WIKIVTYPE*)calloc((size_t)nt + 1, sizeof *type);
    con = (uint8_t*)calloc((size_t)nu + 1, 1);
    if (!unit || !death || !type || !con) { free(unit); free(death); free(type); free(con); error("out of memory"); return; }
    for (i = 0, c = nu ? us->first : NULL; c; c = c->next, i++) {
        ok &= pos_at(c, 0, &unit[i].x) & pos_at(c, 1, &unit[i].z);
        unit[i].owner = int_at(c, 2);
        unit[i].type = int_at(c, 3);
        unit[i].built = tj_num(tj_at(c, 4), 0);
        if (unit[i].type < 0 || unit[i].type >= nt) unit[i].type = -1;
    }
    for (i = 0, c = nd ? ds->first : NULL; c; c = c->next, i++) {
        death[i].tick = int_at(c, 0);
        death[i].owner = int_at(c, 1);
        ok &= pos_at(c, 2, &death[i].x) & pos_at(c, 3, &death[i].z);
        death[i].type = int_at(c, 4);
        if (death[i].type < 0 || death[i].type >= nt) death[i].type = -1;
    }
    for (i = 0, c = nt ? ts->first : NULL; c; c = c->next, i++) {
        type[i].sight_distance = int_at(c, 0);
        type[i].radar_distance = int_at(c, 1);
        type[i].sonar_distance = int_at(c, 2);
        type[i].activates = int_at(c, 3);
        type[i].model_height = int_at(c, 4);
    }
    if (!ok) {
        free(unit); free(death); free(type); free(con);
        error("a position of the view is off two maps' width");
        return;
    }
    w.game = 1;
    w.nunit = nu;
    w.unit = unit;
    w.ndeath = nd;
    w.death = death;
    w.ntype = nt;
    w.type = type;
    n = tagpu_wikisim_sight_lit(&w, t ? t->u.table : &none, seat_arg(v), num_int(tj_get(v, "tick"), 0), lit);
    fputs("{\"lit\":", stdout);
    base64(lit, (size_t)n);
    if (tj_is(tj_get(v, "contacts"), TJ_TRUE)) {
        tagpu_wikisim_contacts(&w, lit, seat_arg(v), con);
        fputs(",\"contacts\":[", stdout);
        for (i = 0; i < nu; i++) printf("%s%d", i ? "," : "", con[i]);
        putchar(']');
    }
    putchar('}');
    free(unit);
    free(death);
    free(type);
    free(con);
}

static void op_sighttable(const TJV* v)
{
    static const WIKISIGHT none;
    char err[64];
    HOBJ* o;
    const WIKISIGHT* t;
    int d = num_int(tj_get(v, "distance"), 0), k, i;
    if (!obj_opt(K_TABLE, tj_get(v, "table"), &o, err, sizeof err)) { error(err); return; }
    t = o ? o->u.table : &none;
    k = tagpu_wikidata_sight_table(t, d);
    printf("{\"idx\":%d,\"table\":", tagpu_wikidata_sight_index(t, d));
    if (k < 0) fputs("null", stdout);
    else {
        printf("{\"idx\":%d,\"cells\":[", t->idx[k]);
        for (i = t->first[k]; i < t->first[k + 1]; i++)
            printf("%s[%d,%d,%d]", i > t->first[k] ? "," : "", t->cell[i].dc, t->cell[i].dr, t->cell[i].n);
        fputs("]}", stdout);
    }
    putchar('}');
}

/* a map of w x h cells, each 0..WIKI_MAP_MAX as an entry's map is held (the frame's arithmetic is
   int) */
static void op_fits(const TJV* v)
{
    const int w = num_int(tj_get(v, "w"), 0), h = num_int(tj_get(v, "h"), 0);
    if (w < 0 || h < 0 || w > WIKI_MAP_MAX || h > WIKI_MAP_MAX) { error("w and h are not 0..WIKI_MAP_MAX"); return; }
    fputs("{\"fits\":", stdout);
    bool_out(tagpu_wikidata_frame_fits(w, h));
    putchar('}');
}

/* the drop-in's gameNow(seat) of a game */
static void now_out(HSIM* m, int seat)
{
    const WIKISIM* s = &m->s;
    const int k = seat >= 0 && seat < s->nin ? seat : 0;
    int j;
    printf("{\"stopped\":%s,\"sound\":%d,\"seats\":[", s->locked != -1 || s->sim.starved || s->sim.bad_kill ? "true" : "false",
           s->sim.snd_total);
    for (j = 0; j < s->sim.nseat; j++) {
        const WIKIECON* z = &s->sim.econ[j];
        printf("%s{\"units\":%d,\"lost\":%d,\"out\":%s,\"kills\":%d,\"losses\":%d,\"bar\":{", j ? "," : "", z->units, z->lost,
               z->out ? "true" : "false", z->kills, z->losses);
        wh_key("e", 1); wh_num(z->e); wh_key("m", 0); wh_num(z->m); wh_key("emax", 0); wh_num(z->emax);
        wh_key("mmax", 0); wh_num(z->mmax); wh_key("ei", 0); wh_num(z->ei); wh_key("eo", 0); wh_num(z->eo);
        wh_key("mi", 0); wh_num(z->mi); wh_key("mo", 0); wh_num(z->mo);
        putchar('}');
        wh_key("mi", 0); wh_num(z->mi); wh_key("ei", 0); wh_num(z->ei); wh_key("wm", 0); wh_num(z->wm);
        /* the f32 sum's exact double, which replay.js's Math.fround keeps */
        wh_key("army", 0); wh_num((double)tagpu_wikisim_army(s, j));
        putchar('}');
    }
    printf("],\"fed\":%d,\"bar\":{", s->in[k].fed);
    {
        const WIKIECON* z = &s->sim.econ[k];
        wh_key("e", 1); wh_num(z->e); wh_key("m", 0); wh_num(z->m); wh_key("emax", 0); wh_num(z->emax);
        wh_key("mmax", 0); wh_num(z->mmax); wh_key("ei", 0); wh_num(z->ei); wh_key("eo", 0); wh_num(z->eo);
        wh_key("mi", 0); wh_num(z->mi); wh_key("mo", 0); wh_num(z->mo);
    }
    fputs("},\"keys\":", stdout);
    keys_out(s->in[k].held);
    putchar('}');
}

/* THE POSES: each view unit's posed model where the DLL's stage poses it (tagpu_wikistage_pose over
   the stage's copy of the view: its unit, pool and piece bounds), else null */
static void poses_out(HSIM* m)
{
    static WIKISTAGEVIEW stage;
    const WIKIVIEW* v = tagpu_wikisim_view(&m->s);
    int i;
    memset(&stage, 0, sizeof stage);
    tagpu_wikistage_copy(&stage, v, m->s.defs.entry, WIKISTAGE_LAYOUT_NONE);
    tagpu_wikistage_pose(&stage, &m->s);
    putchar('[');
    for (i = 0; i < v->nunit; i++) {
        int n = 0;
        const unsigned char* p = i < stage.nunit && stage.unit[i].pose_n ? tagpu_wikisim_model(&m->s, i, &n) : NULL;
        if (i) putchar(',');
        if (p && n > 0) base64(p, TAGPU_COBVM_MODEL_BYTES(n));
        else fputs("null", stdout);
    }
    putchar(']');
}

static void orders_all(HSIM* m)
{
    const WIKIVIEW* v = tagpu_wikisim_view(&m->s);
    int i;
    putchar('{');
    for (i = 0; i < v->nunit; i++) {
        wh_key(v->unit[i].id, i == 0);
        wh_orders(&m->s, v->unit[i].id);
    }
    putchar('}');
}

static int is_game(const HSIM* m) { return m->s.sim.game; }

/* the state `want` asks after a call, as members of the answer being written */
static void want_out(HSIM* m, const TJV* want)
{
    const TJV *w, *seats;
    int first;
    if (!tj_is(want, TJ_OBJ)) return;
    if (tj_is(seats = tj_get(want, "view"), TJ_ARR)) {
        fputs(",\"views\":{", stdout);
        for (first = 1, w = seats->first; w; w = w->next, first = 0) {
            char k[16];
            snprintf(k, sizeof k, "%d", num_int(w, 0));
            wh_key(k, first);
            wh_view_as(&m->s, 0, num_int(w, 0));
        }
        putchar('}');
    }
    if (tj_is(tj_get(want, "orders"), TJ_TRUE)) { wh_key("orders", 0); orders_all(m); }
    if (tj_is(tj_get(want, "hash"), TJ_TRUE)) { wh_key("hash", 0); wh_hash(&m->s); }
    if (tj_is(seats = tj_get(want, "now"), TJ_ARR) && is_game(m)) {
        fputs(",\"now\":{", stdout);
        for (first = 1, w = seats->first; w; w = w->next, first = 0) {
            char k[16];
            snprintf(k, sizeof k, "%d", num_int(w, 0));
            wh_key(k, first);
            now_out(m, num_int(w, 0));
        }
        putchar('}');
    }
    if (tj_is(tj_get(want, "poses"), TJ_TRUE)) { wh_key("poses", 0); poses_out(m); }
}

/* the log made by this call, then cleared */
static void log_out(void)
{
    fputs(",\"log\":[", stdout);
    if (H.nlog && H.log.p) fputs(H.log.p, stdout);
    putchar(']');
    H.log.n = 0;
    if (H.log.p) H.log.p[0] = 0;
    H.nlog = 0;
}

static void tick_out(HSIM* m) { printf("{\"tick\":%d,\"done\":%s", m->s.sim.tick, m->s.done ? "true" : "false"); }

static void op_new(const TJV* v)
{
    static const char* const PLAY[] = { "try", "demo", "game", "stall" };
    char err[256];
    const TJV *cv = tj_get(v, "ctx");
    const char* play = tj_str(tj_get(v, "play"), "");
    HOBJ *e, *lay, *cast, *rival, *packs;
    WIKISIMCTX ctx;
    HSIM* m;
    int scripted = -1, i, slot = 0;
    for (i = 0; i < 4; i++) if (!strcmp(play, PLAY[i])) scripted = i;
    if (scripted < 0) { error("play is not try, demo, game or stall"); return; }
    if (!(e = obj_get(K_ENTRY, tj_get(v, "entry"), err, sizeof err)) || !obj_opt(K_LAYOUT, tj_get(v, "layout"), &lay, err, sizeof err)
        || !obj_opt(K_CASTING, tj_get(cv, "casting"), &cast, err, sizeof err)
        || !obj_opt(K_CASTING, tj_get(cv, "rival"), &rival, err, sizeof err)
        || !obj_opt(K_PACKS, tj_get(cv, "packs"), &packs, err, sizeof err)) {
        error(err);
        return;
    }
    /* an absent or null ctx is side 0, no casting, Interface Type 1, the context the demos are authored in; a
       ctx given names its own side and Interface Type, and one it leaves out is none, which
       tagpu_wikisim_init refuses (a side only with a layout) */
    ctx.side = tj_is(cv, TJ_OBJ) ? num_int(tj_get(cv, "side"), -1) : 0;
    ctx.iface = tj_is(cv, TJ_OBJ) ? num_int(tj_get(cv, "iface"), -1) : 1;
    ctx.cast = cast ? cast->u.casting : NULL;
    ctx.rival = rival ? rival->u.casting : NULL;
    ctx.cob = packs && !tj_is(tj_get(cv, "scripts"), TJ_FALSE) ? &packs->u.packs->cob : NULL;
    ctx.snd = packs && !tj_is(tj_get(cv, "sounds"), TJ_FALSE) ? &packs->u.packs->snd : NULL;
    /* a text entry has no scene (nscene 0), and tagpu_wikisim_init reads one: the bound the DLL's
       stage readers test before any (tagpu_wiki.c) */
    if (e->u.entry->nscene <= 0) { error("the entry has no scene: a text entry plays nothing"); return; }
    if (!(m = (HSIM*)calloc(1, sizeof *m))) { error("out of memory"); return; }
    if (!tagpu_wikisim_init(&m->s, e->u.entry, e->u.entry->ndemo ? &e->u.entry->demo[0] : NULL, scripted,
                            lay ? lay->u.layout : NULL, &ctx, err, sizeof err)) {
        free(m);
        error(err);
        return;
    }
    m->entry = e; e->refs++;
    if ((m->layout = lay)) lay->refs++;
    if ((m->cast = cast)) cast->refs++;
    if ((m->rival = rival)) rival->refs++;
    log_take(m);
    for (i = 0; i < WH_SIMS; i++) {
        if (!H.sim[i]) { slot = i; break; }
        if (H.sim[i]->used < H.sim[slot]->used) slot = i;
    }
    sim_free(H.sim[slot]);
    H.sim[slot] = m;
    m->h = ++H.next;
    m->used = ++H.clock;
    log_made(m, e->u.entry->ndemo ? &e->u.entry->demo[0] : NULL);
    printf("{\"sim\":%d,\"tick\":%d,\"done\":%s,\"side\":%d,\"iface\":%d,\"game\":%s,\"nin\":%d,\"types\":[", m->h, m->s.sim.tick,
           m->s.done ? "true" : "false", m->s.side, m->s.iface, m->s.sim.game ? "true" : "false", m->s.nin);
    for (i = 0; i < m->s.defs.entry->ndef; i++) { if (i) putchar(','); wh_str(m->s.defs.entry->def[i].name); }
    fputs("],\"map\":", stdout);
    map_out(tagpu_wikisim_view(&m->s));
    log_out();
    want_out(m, tj_get(v, "want"));
    putchar('}');
}

static void op_input(const TJV* v, HDOC* d)
{
    char err[256];
    HSIM* m = sim_get(v, err, sizeof err);
    WIKIINPUT in;
    const int t = m ? m->s.sim.tick : 0;
    if (!m) { error(err); return; }
    if (!tagpu_wikidata_input(m->s.defs.entry, tj_get(v, "ev"), &in, err, sizeof err)) { error(err); return; }
    if (!sim_keep(m, d)) { error("out of memory"); return; }
    in.seat = seat_arg(tj_get(v, "ev"));
    tagpu_wikisim_input(&m->s, &in);
    log_diff(m, t, m->s.next, m->s.next);
    tick_out(m);
    log_out();
    want_out(m, tj_get(v, "want"));
    putchar('}');
}

static void op_step(const TJV* v)
{
    char err[64];
    HSIM* m = sim_get(v, err, sizeof err);
    const TJV *each = tj_get(v, "each"), *w;
    const double n = tj_num(tj_get(v, "n"), 1);
    int hash = 0, now = 0, ran = 0, seat = seat_arg(v);
    if (!m) { error(err); return; }
    if (!(n >= 1 && n <= WH_STEP_MAX && n == (int)n)) { error("n is not 1..4096"); return; }
    for (w = tj_is(each, TJ_ARR) ? each->first : NULL; w; w = w->next) {
        hash |= !strcmp(tj_str(w, ""), "hash");
        now |= !strcmp(tj_str(w, ""), "now");
    }
    if (now && !is_game(m)) { error("now: the sim is no game"); return; }
    printf("{\"each\":[");
    while (ran < n && !m->s.done) {
        const int t = m->s.sim.tick, k0 = m->s.next;
        const WIKIINPUT* const* script = m->s.script;
        tagpu_wikisim_step(&m->s);
        log_diff(m, t, script == m->s.script ? k0 : m->s.next, m->s.next);
        if (hash || now) {
            printf("%s{", ran ? "," : "");
            if (hash) { wh_key("hash", 1); wh_hash(&m->s); }
            if (now) { wh_key("now", !hash); now_out(m, seat); }
            putchar('}');
        }
        ran++;
    }
    printf("],\"tick\":%d,\"done\":%s,\"ran\":%d", m->s.sim.tick, m->s.done ? "true" : "false", ran);
    log_out();
    want_out(m, tj_get(v, "want"));
    putchar('}');
}

static void op_view(const TJV* v)
{
    char err[64];
    HSIM* m = sim_get(v, err, sizeof err);
    if (!m) { error(err); return; }
    wh_view_as(&m->s, tj_is(tj_get(v, "map"), TJ_TRUE), seat_arg(v));
}

static void op_orders(const TJV* v)
{
    char err[64];
    HSIM* m = sim_get(v, err, sizeof err);
    if (!m) { error(err); return; }
    if (tj_is(tj_get(v, "all"), TJ_TRUE)) orders_all(m);
    else wh_orders(&m->s, tj_str(tj_get(v, "id"), ""));
}

static void op_hash(const TJV* v)
{
    char err[64];
    HSIM* m = sim_get(v, err, sizeof err);
    if (!m) { error(err); return; }
    fputs("{\"hash\":", stdout);
    wh_hash(&m->s);
    putchar('}');
}

static void op_now(const TJV* v)
{
    char err[64];
    HSIM* m = sim_get(v, err, sizeof err);
    if (!m) { error(err); return; }
    if (!is_game(m)) { error("now: the sim is no game"); return; }
    now_out(m, seat_arg(v));
}

static void op_poses(const TJV* v)
{
    char err[64];
    HSIM* m = sim_get(v, err, sizeof err);
    if (!m) { error(err); return; }
    poses_out(m);
}

static void op_notes(const TJV* v)
{
    char err[64];
    HSIM* m = sim_get(v, err, sizeof err);
    int i;
    if (!m) { error(err); return; }
    putchar('[');
    for (i = 0; m->s.sim.cob && i < m->s.defs.entry->ndef; i++) {
        const char* n = tagpu_wikicob_note(m->s.sim.cob, i);
        if (i) putchar(',');
        wh_str(n ? n : "");
    }
    putchar(']');
}

/* A FORK'S PART, each a game input read against the entry as written, whose type indices the bound
   one keeps (a part names the entry's types, as its game script does); the kills' ids point into
   this request's tree, which the sim keeps */
static void op_fork(const TJV* v, HDOC* d)
{
    char err[256];
    HSIM* m = sim_get(v, err, sizeof err);
    const TJV *list = tj_get(v, "inputs"), *e;
    const char* mode = tj_str(tj_get(v, "mode"), "");
    WIKIINPUT* ins;
    int k = 0, ok = 1, ni;
    if (!m) { error(err); return; }
    if (!tj_is(list, TJ_ARR)) { error("the part's inputs are not a list"); return; }
    ni = list->n;
    if (!(ins = (WIKIINPUT*)calloc((size_t)ni + 1, sizeof *ins))) { error("out of memory"); return; }
    for (e = list->first; ok && e; e = e->next)
        if (!tagpu_wikidata_game_input(m->entry->u.entry, e, &ins[k++], err, sizeof err)) ok = 0;
    if (ok && !sim_keep(m, d)) { ok = 0; snprintf(err, sizeof err, "out of memory"); }
    if (ok && !tagpu_wikisim_fork(&m->s, num_int(tj_get(v, "R"), 0), seat_arg(v), ins, ni,
                                  !strcmp(mode, "stall") ? WIKI_ECON_STALL : !strcmp(mode, "refuse") ? WIKI_ECON_REFUSE : -1,
                                  err, sizeof err))
        ok = 0;
    free(ins);
    if (!ok) { error(err); return; }
    log_diff(m, m->s.sim.tick, m->s.next, m->s.next);
    tick_out(m);
    log_out();
    want_out(m, tj_get(v, "want"));
    putchar('}');
}

static void op_release(const TJV* v)
{
    char err[64];
    HSIM* m = sim_get(v, err, sizeof err);
    if (!m) { error(err); return; }
    tagpu_wikisim_release(&m->s, seat_arg(v));
    log_diff(m, m->s.sim.tick, m->s.next, m->s.next);
    tick_out(m);
    log_out();
    want_out(m, tj_get(v, "want"));
    putchar('}');
}

/* Idempotent: a sim the process no longer holds -- freed already, or evicted from its table -- is
   as freed as the call asks, so its free answers {} too. The drop-in frees from a finalizer as
   well as by hand and cannot know which came first. */
static void op_free(const TJV* v)
{
    char err[64];
    HSIM* m = sim_get(v, err, sizeof err);
    int i;
    for (i = 0; m && i < WH_SIMS; i++)
        if (H.sim[i] == m) H.sim[i] = NULL;
    sim_free(m);
    fputs("{}", stdout);
}

static void call(const TJV* v, HDOC* d);

static void op_batch(const TJV* v, HDOC* d)
{
    const TJV *calls = tj_get(v, "calls"), *c;
    if (!tj_is(calls, TJ_ARR)) { error("calls is not a list"); return; }
    fputs("{\"answers\":[", stdout);
    for (c = calls->first; c; c = c->next) {
        if (c != calls->first) putchar(',');
        if (!strcmp(tj_str(tj_get(c, "op"), ""), "batch")) error("a batch inside a batch");
        else call(c, d);
    }
    fputs("]}", stdout);
}

static void call(const TJV* v, HDOC* d)
{
    const char* op = tj_str(tj_get(v, "op"), "");
    const TJV* at = tj_get(v, "in");
    char why[96];
    H.nlog = 0;
    H.log.n = 0;
    if (!tj_is(v, TJ_OBJ)) error("a request is not an object");
    else if (at && !(tj_is(at, TJ_STR) && !strcmp(at->str, H.instance))) {
        /* THE INSTANCE: a call stamped for another host, whose handles name nothing here */
        fputs("{\"error\":\"a call for another host: the host its handles came from has ended\",\"host\":", stdout);
        wh_str(H.instance);
        putchar('}');
    } else if (!strcmp(op, "hello")) op_hello();
    else if (!strcmp(op, "entry")) op_entry(v);
    else if (!strcmp(op, "layout")) op_layout(v);
    else if (!strcmp(op, "casting")) op_casting(v);
    else if (!strcmp(op, "packs")) op_packs(v);
    else if (!strcmp(op, "table")) op_table(v);
    else if (!strcmp(op, "text")) op_text(v);
    else if (!strcmp(op, "scene")) op_scene(v);
    else if (!strcmp(op, "seatcam")) op_seatcam(v);
    else if (!strcmp(op, "sight")) op_sight(v);
    else if (!strcmp(op, "sighttable")) op_sighttable(v);
    else if (!strcmp(op, "fits")) op_fits(v);
    else if (!strcmp(op, "new")) op_new(v);
    else if (!strcmp(op, "input")) op_input(v, d);
    else if (!strcmp(op, "step")) op_step(v);
    else if (!strcmp(op, "view")) op_view(v);
    else if (!strcmp(op, "orders")) op_orders(v);
    else if (!strcmp(op, "hash")) op_hash(v);
    else if (!strcmp(op, "now")) op_now(v);
    else if (!strcmp(op, "poses")) op_poses(v);
    else if (!strcmp(op, "notes")) op_notes(v);
    else if (!strcmp(op, "fork")) op_fork(v, d);
    else if (!strcmp(op, "release")) op_release(v);
    else if (!strcmp(op, "free")) op_free(v);
    else if (!strcmp(op, "batch")) op_batch(v, d);
    else {
        snprintf(why, sizeof why, "no op %.60s", op);
        error(why);
    }
}

enum { LINE_END, LINE_OK, LINE_LONG, LINE_NUL, LINE_OOM };

/* One line of `in` into `*b`, its newline and a CR before it dropped, read a byte at a time and
   counted, so a NUL in it is seen as the byte it is rather than ending the line early: LINE_OK,
   LINE_END at the end of the input, or LINE_LONG past WH_LINE_MAX, LINE_NUL holding a NUL byte,
   LINE_OOM past the memory, each of those three read past to its newline so that the next line
   starts where the caller's next request does. */
static int read_line(FILE* in, BUF* b)
{
    int c, any = 0, why = LINE_OK;
    b->n = 0;
    b->oom = 0;
    while ((c = getc(in)) != EOF) {
        any = 1;
        if (c == '\n') break;
        if (why != LINE_OK) continue;
        if (!c) why = LINE_NUL;
        else if (b->n >= WH_LINE_MAX + 1) why = LINE_LONG;     /* +1: the CR dropped below */
        else {
            if (b->n + 1 >= b->cap) {
                char ch = (char)c;
                buf_put(b, &ch, 1);
                if (b->oom) why = LINE_OOM;
            } else b->p[b->n++] = (char)c;
        }
    }
    if (!any) return LINE_END;
    if (why != LINE_OK) return why;
    if (b->n && b->p[b->n - 1] == '\r') b->n--;
    if (b->n > WH_LINE_MAX) return LINE_LONG;
    if (b->p) b->p[b->n] = 0;
    return LINE_OK;
}

int wikihost_serve(FILE* in, const char* instance, const char* packs_root)
{
    BUF line = { NULL, 0, 0, 0 };
    int r, i, k;
    snprintf(H.instance, sizeof H.instance, "%s", instance ? instance : "");
    H.root = packs_root;
    while ((r = read_line(in, &line)) != LINE_END) {
        char err[256];
        HDOC* d;
        TJDOC* doc;
        /* every line read is answered, a refused one included: the caller reads one answer a line
           it wrote, and a line left unanswered would leave it waiting for good */
        if (r == LINE_LONG) error("the request line is longer than 16 MB");
        else if (r == LINE_NUL) error("the request line holds a NUL byte");
        else if (r == LINE_OOM) error("out of memory for the request line");
        else if (!line.n) error("an empty request");
        else if (!(doc = tagpu_json_parse(line.p, line.n, err, sizeof err))) error(err);
        else if (!(d = (HDOC*)malloc(sizeof *d))) { tagpu_json_free(doc); error("out of memory"); }
        else {
            d->doc = doc;
            d->refs = 1;
            call(tagpu_json_root(doc), d);
            doc_unref(d);
        }
        putchar('\n');
        fflush(stdout);
    }
    for (i = 0; i < WH_SIMS; i++) { sim_free(H.sim[i]); H.sim[i] = NULL; }
    for (k = 0; k < NKINDS; k++)
        for (i = 0; i < WH_OBJS; i++) { obj_unref(H.obj[k][i]); H.obj[k][i] = NULL; }
    free(H.log.p);
    H.log.p = NULL;
    H.log.cap = 0;
    free(line.p);
    return 0;
}
