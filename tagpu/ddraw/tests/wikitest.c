/* wikitest.c -- the C half of the wiki's parity check, for tools/wiki/simparity.mjs: the real
   tagpu_wikidata.c and tagpu_wikisim.c, compiled natively (or for Windows, run under Wine), write
   what they see as JSON lines, and simparity.mjs compares them with sim.js's, field by field.

   `wikitest demos PAK [LAYOUT [CASTING IFACE [COB]]]` loads PAK, an archive of art/wiki/
   (tools/hudpak.py --set wiki), as the DLL loads WIKIPAK, plays every demo of every entry to its
   end with the sidebar layout LAYOUT (the archive's side/LAYOUT.json; none when absent or "-"),
   in the context of side 0, the casting the file CASTING holds (one side's record; none when
   absent or "-"), Interface Type IFACE (1 when absent) and the units' scripts the file COB
   names (none when absent or "-"; see cob_load), and prints one line per tick from 0:
   {"entry", "demo", "t", "h": tagpu_wikisim_hash as 16 hex digits, "v": the view, "o": every
   unit's orders by id}. The view carries the map on the first and the last tick only: nothing
   writes it after the scene is laid out. A demo the sim refuses in that context is one line,
   {"entry", "demo", "refused": why}.

   `wikitest trace FILE [COB]` replays a recording of sim.js calls (simparity.mjs makes it from
   simcheck.mjs's rule cases), one JSON line per call, each naming its sim by "sim", the order it
   was made in (TRACE_SIMS of them live at once): {"op": "create", "scripted": 0|1,
   "layout": the layout or null, "ctx": {"side", "iface", "casting": a side's record or null,
   "cob": true for the scripts COB names} or null} followed by a line holding the entry, whose only demo is the one the sim is made from;
   {"op": "input", "ev": the input}; {"op": "step"}; and the queries {"op": "view"},
   {"op": "orders", "id"}, {"op": "done"}, {"op": "tick"}, {"op": "side"}, {"op": "iface"} and
   {"op": "hash"}, each answered by one line.

   `wikitest refuse FILE` reads cases two lines each, {"name", "scripted": 0|1, "layout": the
   layout or null, "side", "iface", "casting": a side's casting record or null} and then the
   entry, and makes a sim of the entry's first demo with that layout in that context: through the
   casting's reader, the entry's loader, the layout's and tagpu_wikisim_init. It prints
   "accepted <name>" or "refused <name>: why" per case; simparity.mjs asks text.js's checkEntry
   and sim.js the same.

   `wikitest data FILE` reads items two lines each, {"kind": "casting"|"layout", "name"} and then
   the JSON, reads each with tagpu_wikidata_casting or tagpu_wikidata_layout_json, and prints
   what it read as one JSON line: {"ok": true, ...the struct in sim.js's names} or {"ok": false,
   "why"}. simparity.mjs computes the same from sim.js's checkCasting and checkLayout.

   `wikitest constants` prints the build trip's constants as one JSON line, in sim.js's names.

   `wikitest malformed` loads data the loader must survive -- each case an archive or an entry
   built here -- and prints one line per case, "ok <case>" or "FAILED <case>: why". The native
   build runs under AddressSanitizer, so a write past an allocation fails the run as well.

   Exit 0 when everything loaded and ran; the comparison is simparity.mjs's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikisim.h"

#ifdef _WIN32
#include <windows.h>
/* tagpu_pak.c's resource lookup links against the DLL's module handle; this test reads its archive
   from a file instead and never calls it */
HMODULE g_ddraw_module;
#endif

static char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    char* p;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || !(p = (char*)malloc((size_t)len + 1)) || fread(p, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[len] = 0;
    *n = (size_t)len;
    return p;
}

/* ---- JSON out, in sim.js's shapes ---- */

static void str(const char* s)
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

/* %.17g gives back the same double when read */
static void num(double v) { printf("%.17g", v); }

static void key(const char* k, int first)
{
    if (!first) putchar(',');
    str(k);
    putchar(':');
}

static const char* name(const WIKISIM* s, int t) { return s->entry->def[t].name; }

static void site(const WIKISIM* s, const WIKIVSITE* w)
{
    key("type", 1); str(name(s, w->type));
    key("x", 0); num(w->x);
    key("z", 0); num(w->z);
    key("fx", 0); printf("%d", w->fx);
    key("fz", 0); printf("%d", w->fz);
}

static void orders(const WIKISIM* s, const char* id)
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
        key("k", 1); str(K[o[i].k]);
        if (o[i].k == WIKI_ORDER_MOVE) { key("x", 0); num(o[i].x); key("z", 0); num(o[i].z); }
        else if (o[i].k == WIKI_ORDER_BUILD) {
            key("type", 0); str(name(s, o[i].type));
            key("ox", 0); printf("%d", o[i].ox);
            key("oz", 0); printf("%d", o[i].oz);
        } else { key("target", 0); str(o[i].target); }
        putchar('}');
    }
    putchar(']');
}

static void view(WIKISIM* s, int map)
{
    static const char* const STATE[] = { "place", "refused", "remove" };
    static const char* const SHAPE[] = { NULL, "line", "axis", "rect", "hollow", "ring" };
    static const char* const LINE[] = { "move", "build", "reclaim", "guard" };
    static const char* const CURSOR[] = { "arrow", "move", "build" };
    const WIKIVIEW* v = tagpu_wikisim_view(s);
    int i, first = 1;
    putchar('{');
    if (map) {
        key("map", 1);
        putchar('{');
        key("w", 1); printf("%d", v->w);
        key("h", 0); printf("%d", v->h);
        key("cell", 0); printf("%d", WIKI_CELL);
        key("surfaceMetal", 0); num(v->surface_metal);
        key("metal", 0);
        for (i = 0; i < v->w * v->h; i++) { putchar(i ? ',' : '['); num(v->metal[i]); }
        putchar(']');
        key("fog", 0);
        for (i = 0; i < v->w * v->h; i++) printf("%c%d", i ? ',' : '[', v->fog[i]);
        putchar(']');
        key("vents", 0);
        putchar('[');
        for (i = 0; i < v->nvent; i++) {
            if (i) putchar(',');
            putchar('{'); key("x", 1); num(v->vent[i].x); key("z", 0); num(v->vent[i].z); putchar('}');
        }
        fputs("]}", stdout);
        first = 0;
    }
    key("units", first);
    putchar('[');
    for (i = 0; i < v->nunit; i++) {
        const WIKIVUNIT* u = &v->unit[i];
        if (i) putchar(',');
        putchar('{');
        key("id", 1); str(u->id);
        key("type", 0); str(name(s, u->type));
        key("owner", 0); printf("%d", u->owner);
        key("x", 0); num(u->x);
        key("z", 0); num(u->z);
        key("hx", 0); num(u->hx);
        key("hz", 0); num(u->hz);
        key("fx", 0); printf("%d", u->fx);
        key("fz", 0); printf("%d", u->fz);
        key("selected", 0); fputs(u->selected ? "true" : "false", stdout);
        key("built", 0); num(u->built);
        key("reclaimed", 0); num(u->reclaimed);
        key("label", 0); str(u->label);
        putchar('}');
    }
    putchar(']');
    key("ghost", 0);
    if (v->has_ghost) {
        putchar('{'); site(s, &v->ghost); key("ok", 0); fputs(v->ghost.ok ? "true" : "false", stdout); putchar('}');
    } else fputs("null", stdout);
    key("sites", 0);
    putchar('[');
    for (i = 0; i < v->nsite; i++) {
        if (i) putchar(',');
        putchar('{');
        site(s, &v->site[i]);
        key("state", 0); str(STATE[v->site[i].state]);
        key("order", 0);
        if (v->site[i].order) printf("%d", v->site[i].order);
        else fputs("null", stdout);
        putchar('}');
    }
    putchar(']');
    key("shape", 0); str(SHAPE[v->shape]);
    key("spacing", 0);
    if (v->spacing >= 0) printf("%d", v->spacing);
    else fputs("null", stdout);
    key("queued", 0);
    putchar('[');
    for (i = 0; i < v->nqueued; i++) {
        if (i) putchar(',');
        putchar('{'); site(s, &v->queued[i]); key("owner", 0); printf("%d", v->queued[i].owner); putchar('}');
    }
    putchar(']');
    key("lines", 0);
    putchar('[');
    for (i = 0; i < v->nline; i++) {
        const WIKIVLINE* l = &v->line[i];
        if (i) putchar(',');
        putchar('{');
        key("from", 1); putchar('['); num(l->from_x); putchar(','); num(l->from_z); putchar(']');
        key("to", 0); putchar('['); num(l->to_x); putchar(','); num(l->to_z); putchar(']');
        key("kind", 0); str(LINE[l->kind]);
        putchar('}');
    }
    putchar(']');
    key("cursor", 0);
    putchar('{');
    key("x", 1); num(v->cursor_x);
    key("z", 0); num(v->cursor_z);
    key("shape", 0); str(CURSOR[v->cursor_shape]);
    key("since", 0); printf("%d", v->cursor_since);
    putchar('}');
    key("keys", 0);
    putchar('[');
    for (i = 0, first = 1; i < WIKI_NKEYS; i++)
        if (v->held[i]) {
            if (!first) putchar(',');
            str(tagpu_wikidata_key_name(i));
            first = 0;
        }
    putchar(']');
    key("drag", 0);
    if (v->has_drag) {
        putchar('{');
        key("x0", 1); num(v->drag_x0);
        key("z0", 0); num(v->drag_z0);
        key("x1", 0); num(v->drag_x1);
        key("z1", 0); num(v->drag_z1);
        putchar('}');
    } else fputs("null", stdout);
    key("caption", 0); str(v->caption);
    key("pace", 0); num(v->pace);
    key("mode", 0);
    if (v->mode >= 0) {
        char m[WIKI_ID_MAX + 8];
        snprintf(m, sizeof m, "build:%s", name(s, v->mode));
        str(m);
    } else str("contextual");
    if (v->has_side) {
        key("side", 0);
        putchar('{');
        key("index", 1); printf("%d", v->side.index);
        key("h", 0); printf("%d", v->side.h);
        key("screen", 0); str(s->lay->screen[v->side.screen].name);
        key("unit", 0); str(v->side.unit[0] ? v->side.unit : NULL);
        key("page", 0); printf("%d", v->side.page);
        key("state", 0);
        putchar('[');
        for (i = 0; i < v->side.nstate; i++) printf("%s%d", i ? "," : "", v->side.state[i]);
        putchar(']');
        key("under", 0); str(v->side.under[0] ? v->side.under : NULL);
        putchar('}');
    }
    key("locked", 0); str(v->locked >= 0 ? name(s, v->locked) : NULL);
    key("clock", 0); printf("%d", v->clock);
    key("waiting", 0); str(v->waiting == WIKI_UNTIL_IDLE ? "idle" : NULL);
    key("stalled", 0); fputs(v->stalled ? "true" : "false", stdout);
    key("cob", 0);
    if (v->has_cob) printf("[%lu,%lu]", (unsigned long)(v->cob_hash >> 32), (unsigned long)(v->cob_hash & 0xFFFFFFFFu));
    else fputs("null", stdout);
    key("parts", 0);
    putchar('[');
    for (i = 0; i < v->npart; i++) {
        const WIKIVPART* q = &v->part[i];
        printf("%s[%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld]", i ? "," : "", (long)q->x, (long)q->y, (long)q->z, (long)q->sx,
               (long)q->sy, (long)q->sz, (long)q->end, (long)q->col);
    }
    putchar(']');
    key("partsCut", 0); fputs(v->part_cut ? "true" : "false", stdout);
    key("bursts", 0);
    putchar('[');
    for (i = 0; i < v->nburst; i++) {
        const WIKISPRAY* e = &v->burst[i];
        int k;
        printf("%s[", i ? "," : "");
        for (k = 0; k < 12; k++) printf("%ld,", (long)e->box[k / 6][k % 6]);
        printf("%d,%lu]", e->due, (unsigned long)e->key);
    }
    putchar(']');
    key("sound", 0);
    if (v->has_sound) {
        putchar('{');
        key("next", 1);
        putchar('[');
        for (i = 0; i < WIKI_SND_KINDS; i++) printf("%s%d", i ? "," : "", v->snd_next[i]);
        putchar(']');
        key("last", 0); printf("%d", v->snd_last);
        key("queue", 0);
        putchar('[');
        for (i = 0; i < v->nsndq; i++) {
            const WIKISNDQ* q = &v->sndq[i];
            printf("%s[%d,%d,", i ? "," : "", q->kind, q->tick);
            str(q->unit);
            putchar(',');
            str(name(s, q->type));
            printf(",%d]", q->priority);
        }
        putchar(']');
        key("total", 0); printf("%d", v->snd_total);
        key("events", 0);
        putchar('[');
        for (i = 0; i < v->nsnd; i++)
            printf("%s[%d,%d,%d,%d,%d]", i ? "," : "", v->snd[i].tick, v->snd[i].kind, v->snd[i].name, v->snd[i].cat,
                   v->snd[i].variant);
        fputs("]}", stdout);
    } else fputs("null", stdout);
    putchar('}');
}

/* ---- the units' scripts: what the DLL reads from the game's archives, here from files ---- */
#include "wikicobfile.h"

/* the state hash as 16 hex digits, in two halves: mingw's printf need not know %llx */
static void hash(const WIKISIM* s)
{
    const uint64_t h = tagpu_wikisim_hash(s);
    printf("\"%08lx%08lx\"", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFu));
}

static void frame(WIKISIM* s, const char* entry, const char* demo, int map)
{
    const WIKIVIEW* v;
    int i;
    putchar('{');
    key("entry", 1); str(entry);
    key("demo", 0); str(demo);
    key("t", 0); printf("%d", s->tick);
    key("h", 0); hash(s);
    key("v", 0); view(s, map);
    key("o", 0);
    putchar('{');
    v = tagpu_wikisim_view(s);
    for (i = 0; i < v->nunit; i++) {
        key(v->unit[i].id, i == 0);
        orders(s, v->unit[i].id);
    }
    fputs("}}\n", stdout);
}

/* the casting a file holds, one side's record, into `k`; 1, else 0 with a message on stderr */
static int casting_file(const char* path, WIKICASTING* k)
{
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    TJDOC* doc = text ? tagpu_json_parse(text, n, err, sizeof err) : NULL;
    int ok = doc && tagpu_wikidata_casting(tagpu_json_root(doc), k, err, sizeof err);
    if (!ok) fprintf(stderr, "wikitest: %s: %s\n", path, text ? err : "cannot read it");
    tagpu_json_free(doc);
    free(text);
    return ok;
}

static int demos(const char* path, const char* layout, const char* casting, int iface, int cob)
{
    char err[256], file[96];
    size_t n;
    char* pak;
    WIKIDATA* d;
    WIKILAYOUT* lay = NULL;
    WIKICASTING k;
    WIKISIMCTX ctx;
    int i, j, rc = 0;
    ctx.side = 0;
    ctx.cast = casting && strcmp(casting, "-") ? &k : NULL;
    ctx.iface = iface;
    ctx.cob = cob ? &COB_SRC : NULL;
    ctx.snd = cob ? &SND_SRC : NULL;
    if (ctx.cast && !casting_file(casting, &k)) return 2;
    if (!(pak = slurp(path, &n))) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    if (!(d = tagpu_wikidata_load((const unsigned char*)pak, n, err, sizeof err))) {
        fprintf(stderr, "wikitest: %s\n", err);
        free(pak);
        return 2;
    }
    snprintf(file, sizeof file, "side/%s.json", layout ? layout : "-");
    if (layout && strcmp(layout, "-") && !(lay = tagpu_wikidata_layout_pak((const unsigned char*)pak, n, file, err, sizeof err))) {
        fprintf(stderr, "wikitest: %s\n", err);
        tagpu_wikidata_free(d);
        free(pak);
        return 2;
    }
    for (i = 0; i < d->nentry; i++)
        for (j = 0; j < d->entry[i].ndemo; j++) {
            const WIKIENTRY* e = &d->entry[i];
            WIKISIM s;
            if (!tagpu_wikisim_init(&s, e, &e->demo[j], 1, lay, &ctx, err, sizeof err)) {
                putchar('{');
                key("entry", 1); str(e->id);
                key("demo", 0); str(e->demo[j].id);
                key("refused", 0); str(err);
                fputs("}\n", stdout);
                continue;
            }
            frame(&s, e->id, e->demo[j].id, 1);
            while (!s.done) {
                tagpu_wikisim_step(&s);
                frame(&s, e->id, e->demo[j].id, s.done);
            }
            if (s.oom) {
                fprintf(stderr, "wikitest: %s/%s ran out of memory\n", e->id, e->demo[j].id);
                rc = 2;
            }
            tagpu_wikisim_free(&s);
            if (rc) goto done;
        }
done:
    tagpu_wikidata_free(d);
    free(lay);
    free(pak);
    return rc;
}

/* The trace's lines, each parsed; a caption the sim keeps points into its line's tree, so every
   tree of a sim lives until the sim ends. */
typedef struct {
    WIKIENTRY e;
    WIKISIM   s;
    WIKILAYOUT* lay;
    WIKICASTING cast;               /* the context's, which the sim holds */
    int       live, id;
    TJDOC*    doc[4096];
    int       ndoc;
} CASE;

/* the sims a trace holds at once: sim k lives in slot k % TRACE_SIMS until sim k + TRACE_SIMS is
   made, and a call naming a sim no slot holds stops the run rather than asking another */
#define TRACE_SIMS 8

static void case_end(CASE* c)
{
    int i;
    if (c->live) {
        tagpu_wikisim_free(&c->s);
        tagpu_wikidata_entry_free(&c->e);
    }
    free(c->lay);
    c->lay = NULL;
    for (i = 0; i < c->ndoc; i++) tagpu_json_free(c->doc[i]);
    c->ndoc = 0;
    c->live = 0;
}

/* a message on stderr; 2, the exit of a run that could not go on */
static int cannot(const char* what)
{
    fprintf(stderr, "wikitest: %s\n", what);
    return 2;
}

static int trace(const char* path)
{
    static CASE cs[TRACE_SIMS];
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    char *line, *end;
    int i, rc = 0;
    if (!text) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    for (line = text; *line && !rc; line = end) {
        TJDOC* doc;
        const TJV* v;
        const char* op;
        CASE* c;
        int id;
        for (end = line; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!*line) continue;
        if (!(doc = tagpu_json_parse(line, strlen(line), err, sizeof err))) { rc = cannot(err); break; }
        v = tagpu_json_root(doc);
        op = tj_str(tj_get(v, "op"), "");
        id = (int)tj_num(tj_get(v, "sim"), 0);
        if (id < 0) {
            tagpu_json_free(doc);
            rc = cannot("a call names a sim below 0");
            break;
        }
        c = &cs[id % TRACE_SIMS];
        if (!strcmp(op, "create")) {
            const int scripted = (int)tj_num(tj_get(v, "scripted"), 0);
            const TJV *layout = tj_get(v, "layout"), *cv = tj_get(v, "ctx");
            const TJV* casting = tj_get(cv, "casting");
            WIKISIMCTX ctx;
            char* next;
            case_end(c);
            c->id = id;
            /* sim.js's absent ctx is side 0, no casting, Interface Type 1 */
            ctx.side = (int)tj_num(tj_get(cv, "side"), 0);
            ctx.iface = (int)tj_num(tj_get(cv, "iface"), 1);
            ctx.cast = casting && !tj_is(casting, TJ_NULL) ? &c->cast : NULL;
            ctx.cob = tj_is(tj_get(cv, "cob"), TJ_TRUE) ? &COB_SRC : NULL;
            ctx.snd = tj_is(tj_get(cv, "snd"), TJ_TRUE) ? &SND_SRC : NULL;
            if ((layout && !tj_is(layout, TJ_NULL) && !(c->lay = tagpu_wikidata_layout_json(layout, err, sizeof err)))
                || (ctx.cast && !tagpu_wikidata_casting(casting, &c->cast, err, sizeof err))) {
                tagpu_json_free(doc);
                rc = cannot(err);
                break;
            }
            tagpu_json_free(doc);
            for (next = end; *end && *end != '\n'; end++) {}
            if (*end) *end++ = 0;
            if (!tagpu_wikidata_entry(&c->e, next, strlen(next), err, sizeof err)) { rc = cannot(err); break; }
            if (!tagpu_wikisim_init(&c->s, &c->e, c->e.ndemo ? &c->e.demo[0] : NULL, scripted, c->lay, &ctx, err, sizeof err)) {
                tagpu_wikidata_entry_free(&c->e);
                rc = cannot(err);
                break;
            }
            c->live = 1;
            continue;
        }
        if (!c->live || c->id != id || c->ndoc == (int)(sizeof c->doc / sizeof c->doc[0])) {
            fprintf(stderr, "wikitest: %s on sim %d, which no slot holds, or too many calls\n", op, id);
            tagpu_json_free(doc);
            rc = 2;
            break;
        }
        c->doc[c->ndoc++] = doc;
        if (!strcmp(op, "input")) {
            WIKIINPUT in;
            /* a `build` names a type of the entry the sim plays, the bound one with a casting */
            if (!tagpu_wikidata_input(c->s.entry, tj_get(v, "ev"), &in, err, sizeof err)) rc = cannot(err);
            else tagpu_wikisim_input(&c->s, &in);
        } else if (!strcmp(op, "step")) tagpu_wikisim_step(&c->s);
        else if (!strcmp(op, "view")) { view(&c->s, 1); putchar('\n'); }
        else if (!strcmp(op, "orders")) { orders(&c->s, tj_str(tj_get(v, "id"), "")); putchar('\n'); }
        else if (!strcmp(op, "done")) puts(c->s.done ? "true" : "false");
        else if (!strcmp(op, "tick")) printf("%d\n", c->s.tick);
        else if (!strcmp(op, "side")) printf("%d\n", c->s.side);
        else if (!strcmp(op, "iface")) printf("%d\n", c->s.iface);
        else if (!strcmp(op, "hash")) { hash(&c->s); putchar('\n'); }
        else { fprintf(stderr, "wikitest: no op %s\n", op); rc = 2; }
    }
    for (i = 0; i < TRACE_SIMS; i++) case_end(&cs[i]);
    free(text);
    return rc;
}

/* ---- refusals, case by case ---- */

/* One case's verdict: 1 accepted, else 0 with why in `err`. The steps are sim.js's make() in its
   order, each one the C side has: the casting's reader (checkCasting, which sim.js's binder
   calls first), the entry's loader (text.js's checkEntry among its refusals), the layout's, and
   tagpu_wikisim_init in the case's context. */
static int refuse_case(const TJV* v, const char* entry, char* err, size_t errn)
{
    const TJV *layout = tj_get(v, "layout"), *casting = tj_get(v, "casting");
    const int scripted = (int)tj_num(tj_get(v, "scripted"), 0);
    WIKICASTING k;
    WIKISIMCTX ctx;
    WIKIENTRY e;
    WIKILAYOUT* lay = NULL;
    WIKISIM s;
    int ok;
    ctx.side = (int)tj_num(tj_get(v, "side"), 0);
    ctx.iface = (int)tj_num(tj_get(v, "iface"), 1);
    ctx.cast = casting && !tj_is(casting, TJ_NULL) ? &k : NULL;
    ctx.cob = NULL;
    ctx.snd = NULL;
    if (ctx.cast && !tagpu_wikidata_casting(casting, &k, err, errn)) return 0;
    if (!tagpu_wikidata_entry(&e, entry, strlen(entry), err, errn)) return 0;
    ok = !(layout && !tj_is(layout, TJ_NULL) && !(lay = tagpu_wikidata_layout_json(layout, err, errn)))
      && tagpu_wikisim_init(&s, &e, e.ndemo ? &e.demo[0] : NULL, scripted, lay, &ctx, err, errn);
    if (ok) tagpu_wikisim_free(&s);
    tagpu_wikidata_entry_free(&e);
    free(lay);
    return ok;
}

static int refuse(const char* path)
{
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    char *line, *end;
    int rc = 0;
    if (!text) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    for (line = text; *line; line = end) {
        TJDOC* doc;
        const TJV* v;
        char name[128], *entry;
        for (end = line; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!*line) continue;
        for (entry = end; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!(doc = tagpu_json_parse(line, strlen(line), err, sizeof err))) { rc = cannot(err); break; }
        v = tagpu_json_root(doc);
        snprintf(name, sizeof name, "%s", tj_str(tj_get(v, "name"), "?"));
        err[0] = 0;
        if (refuse_case(v, entry, err, sizeof err)) printf("accepted %s\n", name);
        else printf("refused %s: %s\n", name, err);
        tagpu_json_free(doc);
    }
    free(text);
    return rc;
}

/* ---- castings and layouts, read and written back ---- */

static void casting_out(const WIKICASTING* k)
{
    static const char* const FIG[6] = { "buildTime", "workerTime", "buildDistance", "maxVelocity", "extracts", "energyMake" };
    int c, i;
    fputs("{\"ok\":true,\"casts\":[", stdout);
    for (c = 0; c < WIKI_NCAST; c++) {
        const WIKICASTDEF* d = &k->cast[c];
        const double fig[6] = { d->build_time, d->worker_time, d->build_distance, d->max_velocity, d->extracts, d->energy_make };
        if (c) putchar(',');
        if (!d->bound) { fputs("null", stdout); continue; }
        putchar('{');
        key("type", 1); str(d->type);
        key("title", 0); str(d->title);
        key("fx", 0); printf("%d", d->fx);
        key("fz", 0); printf("%d", d->fz);
        for (i = 0; i < 6; i++) { key(FIG[i], 0); num(fig[i]); }
        key("canstop", 0); printf("%d", d->canstop);
        key("activatewhenbuilt", 0); printf("%d", d->activate_when_built);
        key("vent", 0); printf("%d", d->vent);
        key("g", 0);
        for (i = 0; i < WIKI_FP_MAX; i++) printf("%c%u", i ? ',' : '[', (unsigned)d->g[i]);
        putchar(']');
        key("builds", 0); printf("%u", (unsigned)d->builds);
        putchar('}');
    }
    puts("]}");
}

static void layout_out(const WIKILAYOUT* L)
{
    int i, j, k, n;
    printf("{\"ok\":true,\"height\":%d,\"screens\":[", L->height);
    for (i = 0; i < L->nscreen; i++) { if (i) putchar(','); str(L->screen[i].name); }
    fputs("],\"sides\":[", stdout);
    for (i = 0; i < L->nside; i++) {
        const WIKISIDEDEF* d = &L->side[i];
        if (i) putchar(',');
        putchar('{');
        key("name", 1); str(d->name);
        key("prefix", 0); str(d->prefix);
        key("main2", 0); str(L->screen[d->main2].name);
        key("gen", 0); str(L->screen[d->gen].name);
        key("types", 0);
        putchar('{');
        for (j = 0, n = 0; j < L->ntype; j++) {
            const WIKIPAGES* t = &L->type[j];
            if (t->side != i) continue;
            key(t->name, n++ == 0);
            putchar('{');
            key("pages", 1);
            putchar('[');
            for (k = 0; k < t->npage; k++) { if (k) putchar(','); str(L->screen[t->page[k]].name); }
            putchar(']');
            key("orders", 0); str(t->orders >= 0 ? L->screen[t->orders].name : NULL);
            putchar('}');
        }
        fputs("}}", stdout);
    }
    puts("]}");
}

static int data(const char* path)
{
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    char *line, *end;
    int rc = 0;
    if (!text) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    for (line = text; *line && !rc; line = end) {
        TJDOC *head, *doc;
        char* item;
        for (end = line; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!*line) continue;
        for (item = end; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!(head = tagpu_json_parse(line, strlen(line), err, sizeof err))) { rc = cannot(err); break; }
        if (!(doc = tagpu_json_parse(item, strlen(item), err, sizeof err))) {
            tagpu_json_free(head);
            rc = cannot(err);
            break;
        }
        err[0] = 0;
        if (!strcmp(tj_str(tj_get(tagpu_json_root(head), "kind"), ""), "casting")) {
            WIKICASTING k;
            if (tagpu_wikidata_casting(tagpu_json_root(doc), &k, err, sizeof err)) casting_out(&k);
            else { fputs("{\"ok\":false,\"why\":", stdout); str(err); puts("}"); }
        } else {
            WIKILAYOUT* L = tagpu_wikidata_layout_json(tagpu_json_root(doc), err, sizeof err);
            if (L) layout_out(L);
            else { fputs("{\"ok\":false,\"why\":", stdout); str(err); puts("}"); }
            free(L);
        }
        tagpu_json_free(doc);
        tagpu_json_free(head);
    }
    free(text);
    return rc;
}

static int constants(void)
{
    static const WIKITRIPROW rows[WIKI_TRIP_NROWS] = WIKI_TRIP_ROWS;
    int k;
    printf("{\"TRIP\":%d,\"TRIP_DOWN\":%d,\"TRIP_UP\":%d,\"TRIP_ROWS\":[", WIKI_TRIP, WIKI_TRIP_DOWN, WIKI_TRIP_UP);
    for (k = 0; k < WIKI_TRIP_NROWS; k++)
        printf("%s{\"glide\":%d,\"hold\":%d,\"gap\":%d,\"back\":%d,\"hover\":%d}", k ? "," : "", rows[k].glide,
               rows[k].hold, rows[k].gap, rows[k].back, rows[k].hover);
    puts("]}");
    return 0;
}

/* ---- malformed data ---- */

static void put32(unsigned char* p, size_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* an archive in tools/hudpak.py's format of `n` files, names[i] holding texts[i]; `*len` its size */
static unsigned char* pack(const char* const* names, const char* const* texts, int n, size_t* len)
{
    size_t head = 12 + 16 * (size_t)n, at = head, noff = head, i;
    unsigned char* p;
    for (i = 0; i < (size_t)n; i++) at += strlen(names[i]);
    for (i = 0; i < (size_t)n; i++) at = ((at + 3) & ~(size_t)3) + strlen(texts[i]);
    if (!(p = (unsigned char*)calloc(1, at))) return NULL;
    memcpy(p, "HPAK", 4);
    put32(p + 4, 1);
    put32(p + 8, (size_t)n);
    at = head;
    for (i = 0; i < (size_t)n; i++) at += strlen(names[i]);
    for (i = 0; i < (size_t)n; i++) {
        size_t nl = strlen(names[i]), tl = strlen(texts[i]);
        at = (at + 3) & ~(size_t)3;
        put32(p + 12 + 16 * i, noff);
        put32(p + 16 + 16 * i, nl);
        put32(p + 20 + 16 * i, at);
        put32(p + 24 + 16 * i, tl);
        memcpy(p + noff, names[i], nl);
        memcpy(p + at, texts[i], tl);
        noff += nl;
        at += tl;
    }
    *len = at;
    return p;
}

static int said(int ok, const char* name, const char* why)
{
    if (ok) printf("ok %s\n", name);
    else printf("FAILED %s: %s\n", name, why);
    return ok;
}

/* a tree whose `tree.json` is `tree`: it loads, as `nodes` nodes, the folders without an entry */
static int tree_case(const char* name, const char* tree, int nodes)
{
    const char* names[1] = { "tree.json" };
    const char* texts[1];
    char err[256], why[320];
    size_t n;
    unsigned char* pak;
    WIKIDATA* d;
    int ok;
    texts[0] = tree;
    if (!(pak = pack(names, texts, 1, &n))) return said(0, name, "out of memory");
    err[0] = 0;
    d = tagpu_wikidata_load(pak, n, err, sizeof err);
    snprintf(why, sizeof why, "loaded %d nodes and %d entries (%s), expected %d nodes", d ? d->nnode : -1,
             d ? d->nentry : -1, d ? "" : err, nodes);
    ok = d && d->nnode == nodes && d->nentry == 0;
    tagpu_wikidata_free(d);
    free(pak);
    return said(ok, name, why);
}

/* every case, its verdict in its line: the count is simparity.mjs's, as the comparison is */
/* lists given as objects: each reads as empty, never as as many unread elements as members */
static int entry_case(void)
{
    static const char TEXT[] =
        "{\"format\":1,\"id\":\"m\",\"text\":{\"a\":\"x\",\"b\":\"y\"},"
        "\"units\":{\"ARMMEX\":{\"footprint\":[2,2],\"buildTime\":10}},"
        "\"scene\":{\"map\":{\"w\":8,\"h\":8},\"metal\":{\"r\":1},\"vents\":{\"v\":[1,1]},"
        "\"units\":{\"u\":{\"type\":\"ARMMEX\",\"at\":[1,1]}},\"selected\":{\"s\":\"u\"}},"
        "\"demos\":{\"d\":{}}}";
    WIKIENTRY e;
    char err[256], why[320];
    int ok;
    err[0] = 0;
    ok = tagpu_wikidata_entry(&e, TEXT, sizeof TEXT - 1, err, sizeof err);
    snprintf(why, sizeof why, "%s: text %d, metal %d, vents %d, units %d, selected %d, demos %d",
             ok ? "loaded" : err, ok ? e.ntext : -1, ok ? e.scene.nmetal : -1, ok ? e.scene.nvent : -1,
             ok ? e.scene.nunit : -1, ok ? e.scene.nselected : -1, ok ? e.ndemo : -1);
    ok = ok && !e.ntext && !e.scene.nmetal && !e.scene.nvent && !e.scene.nunit && !e.scene.nselected
         && !e.ndemo;
    tagpu_wikidata_entry_free(&e);
    return said(ok, "entry-lists-as-objects", why);
}

/* a unit whose "facing" is the JSON `facing` (NULL: none): refused when `refused`, else loaded
   as the unit vector (hx, hz), compared exactly */
static int facing_case(const char* name, const char* facing, int refused, double hx, double hz)
{
    char text[512], err[256], why[512];
    WIKIENTRY e;
    int loaded, ok;
    snprintf(text, sizeof text,
             "{\"format\":1,\"id\":\"f\",\"units\":{\"ARMCK\":{\"footprint\":[2,2],\"buildTime\":10,"
             "\"maxVelocity\":1}},\"scene\":{\"map\":{\"w\":8,\"h\":8},\"units\":[{\"type\":\"ARMCK\","
             "\"at\":[40,40]%s%s}]}}", facing ? ",\"facing\":" : "", facing ? facing : "");
    err[0] = 0;
    loaded = tagpu_wikidata_entry(&e, text, strlen(text), err, sizeof err);
    if (loaded && e.scene.nunit != 1) snprintf(err, sizeof err, "loaded %d units", e.scene.nunit);
    ok = loaded && e.scene.nunit == 1;
    snprintf(why, sizeof why, "%s: (%.17g, %.17g), expected %s(%.17g, %.17g)", ok ? "loaded" : err,
             ok ? e.scene.unit[0].hx : 0.0, ok ? e.scene.unit[0].hz : 0.0, refused ? "a refusal, not " : "", hx, hz);
    ok = refused ? !loaded : ok && e.scene.unit[0].hx == hx && e.scene.unit[0].hz == hz;
    if (loaded) tagpu_wikidata_entry_free(&e);
    return said(ok, name, why);
}

/* an entry of one type, ARMX, whose record is `unit`: refused when `refused`, else loaded with
   cast `cast` and G rows `g` (its first four), compared exactly */
static int unit_case(const char* name, const char* unit, int refused, int cast, const unsigned* g)
{
    char text[512], err[256], why[512];
    WIKIENTRY e;
    int loaded, ok, i;
    snprintf(text, sizeof text, "{\"format\":1,\"id\":\"u\",\"units\":{\"ARMX\":%s},\"scene\":{\"map\":{\"w\":8,\"h\":8}}}", unit);
    err[0] = 0;
    loaded = tagpu_wikidata_entry(&e, text, strlen(text), err, sizeof err);
    snprintf(why, sizeof why, "%s: cast %d, g %u %u %u %u; expected %scast %d, g %u %u %u %u", loaded ? "loaded" : err,
             loaded ? e.def[0].cast : -2, loaded ? e.def[0].g[0] : 0, loaded ? e.def[0].g[1] : 0, loaded ? e.def[0].g[2] : 0,
             loaded ? e.def[0].g[3] : 0, refused ? "a refusal, not " : "", cast, g[0], g[1], g[2], g[3]);
    ok = refused ? !loaded : loaded && e.def[0].cast == cast;
    for (i = 0; !refused && ok && i < WIKI_FP_MAX; i++) ok = e.def[0].g[i] == (i < 4 ? g[i] : 0);
    if (loaded) tagpu_wikidata_entry_free(&e);
    return said(ok, name, why);
}

static int malformed(void)
{
    int ok = 1;
    /* an `entry` that is not a string is a folder, counted and read as one: its children too */
    ok &= tree_case("tree-entry-number",
                    "{\"format\":1,\"tree\":[{\"entry\":5,\"children\":[{\"title\":\"a\"},"
                    "{\"title\":\"b\"},{\"title\":\"c\"}]}]}", 4);
    ok &= tree_case("tree-entry-object",
                    "{\"format\":1,\"tree\":[{\"entry\":{\"id\":\"x\"},\"children\":[{\"title\":\"a\","
                    "\"children\":[{\"title\":\"b\"}]}]}]}", 3);
    ok &= entry_case();
    ok &= facing_case("facing-absent", NULL, 0, 0, 1);
    ok &= facing_case("facing-null", "null", 0, 0, 1);
    ok &= facing_case("facing-normalised", "[3,4]", 0, 3.0 / 5.0, 4.0 / 5.0);
    ok &= facing_case("facing-zero", "[0,0]", 1, 0, 0);
    ok &= facing_case("facing-one-number", "[1]", 1, 0, 0);
    ok &= facing_case("facing-three-numbers", "[1,2,3]", 1, 0, 0);
    ok &= facing_case("facing-not-a-number", "[1,\"a\"]", 1, 0, 0);
    ok &= facing_case("facing-an-object", "{\"x\":1,\"z\":0}", 1, 0, 0);
    ok &= facing_case("facing-square-underflows", "[1e-200,0]", 1, 0, 0);
    ok &= facing_case("facing-square-overflows", "[1e200,0]", 1, 0, 0);
    {
        static const unsigned none[4] = { 0, 0, 0, 0 }, listed[4] = { 2, 0, 8, 0 }, all[4] = { 7, 7, 0, 0 },
                              row16[4] = { 0xFFFF, 0, 0, 0 };
        ok &= unit_case("cast-named", "{\"cast\":\"geo\",\"footprint\":[2,2],\"buildTime\":1}", 0, WIKI_CAST_GEO, none);
        ok &= unit_case("cast-absent", "{\"footprint\":[2,2],\"buildTime\":1}", 0, -1, none);
        ok &= unit_case("cast-unknown", "{\"cast\":\"Geo\",\"footprint\":[2,2],\"buildTime\":1}", 0, -1, none);
        ok &= unit_case("g-listed", "{\"footprint\":[4,4],\"buildTime\":1,\"vent\":true,\"g\":[[1,0],[3,2]]}", 0, -1, listed);
        ok &= unit_case("g-vent-all", "{\"footprint\":[3,2],\"buildTime\":1,\"vent\":true}", 0, -1, all);
        ok &= unit_case("g-vent-all-16", "{\"footprint\":[16,1],\"buildTime\":1,\"vent\":true}", 0, -1, row16);
        /* the G rows hold 16 cells a side: sim.js would play it, the C refuses it (tagpu_wikidata.h) */
        ok &= unit_case("g-footprint-past-16", "{\"footprint\":[17,2],\"buildTime\":1,\"vent\":true}", 1, -1, none);
        ok &= unit_case("no-g-footprint-past-16", "{\"footprint\":[17,2],\"buildTime\":1,\"g\":[]}", 0, -1, none);
    }
    (void)ok;
    return 0;
}

int main(int argc, char** argv)
{
    int rc;
    if ((argc == 3 || argc == 4 || argc == 6 || argc == 7) && !strcmp(argv[1], "demos")) {
        const int cob = argc == 7 && strcmp(argv[6], "-");
        if (cob && !cob_load(argv[6])) return 2;
        rc = demos(argv[2], argc >= 4 ? argv[3] : NULL, argc >= 6 ? argv[4] : NULL, argc >= 6 ? atoi(argv[5]) : 1, cob);
        cob_free();
        return rc;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "trace")) {
        if (argc == 4 && strcmp(argv[3], "-") && !cob_load(argv[3])) return 2;
        rc = trace(argv[2]);
        cob_free();
        return rc;
    }
    if (argc == 3 && !strcmp(argv[1], "refuse")) return refuse(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "data")) return data(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "constants")) return constants();
    if (argc == 2 && !strcmp(argv[1], "malformed")) return malformed();
    fprintf(stderr, "usage: wikitest demos PAK [LAYOUT [CASTING IFACE [COB]]] | wikitest trace FILE [COB] | wikitest refuse FILE | wikitest data FILE"
            " | wikitest constants"
            " | wikitest malformed\n");
    return 2;
}
