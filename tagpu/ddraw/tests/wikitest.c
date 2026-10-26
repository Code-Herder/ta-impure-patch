/* wikitest.c -- the C half of the wiki's parity check, for tools/wiki/simparity.mjs: the real
   tagpu_wikidata.c and tagpu_wikisim.c, compiled natively (or for Windows, run under Wine), write
   what they see as JSON lines, and simparity.mjs compares them with sim.js's, field by field.

   `wikitest demos PAK` loads PAK, an archive of art/wiki/ (tools/hudpak.py --set wiki), as the DLL
   loads WIKIPAK, plays every demo of every entry to its end, and prints one line per tick from 0:
   {"entry", "demo", "t", "h": tagpu_wikisim_hash as 16 hex digits, "v": the view, "o": every
   unit's orders by id}. The view carries the map on the first and the last tick only: nothing
   writes it after the scene is laid out.

   `wikitest trace FILE` replays a recording of sim.js calls (simparity.mjs makes it from
   simcheck.mjs's rule cases), one JSON line per call: {"op": "create", "scripted": 0|1} followed
   by a line holding the entry, whose only demo is the one the sim is made from; {"op": "input",
   "ev": the input}; {"op": "step"}; and the queries {"op": "view"}, {"op": "orders", "id"},
   {"op": "done"}, {"op": "tick"} and {"op": "hash"}, each answered by one line.

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
    putchar('}');
}

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

static int demos(const char* path)
{
    char err[256];
    size_t n;
    char* pak = slurp(path, &n);
    WIKIDATA* d;
    int i, j;
    if (!pak) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    if (!(d = tagpu_wikidata_load((const unsigned char*)pak, n, err, sizeof err))) {
        fprintf(stderr, "wikitest: %s\n", err);
        return 2;
    }
    for (i = 0; i < d->nentry; i++)
        for (j = 0; j < d->entry[i].ndemo; j++) {
            const WIKIENTRY* e = &d->entry[i];
            WIKISIM s;
            if (!tagpu_wikisim_init(&s, e, &e->demo[j], 1, err, sizeof err)) {
                fprintf(stderr, "wikitest: %s/%s: %s\n", e->id, e->demo[j].id, err);
                return 2;
            }
            frame(&s, e->id, e->demo[j].id, 1);
            while (!s.done) {
                tagpu_wikisim_step(&s);
                frame(&s, e->id, e->demo[j].id, s.done);
            }
            if (s.oom) { fprintf(stderr, "wikitest: %s/%s ran out of memory\n", e->id, e->demo[j].id); return 2; }
            tagpu_wikisim_free(&s);
        }
    tagpu_wikidata_free(d);
    free(pak);
    return 0;
}

/* The trace's lines, each parsed; a caption the sim keeps points into its line's tree, so every
   tree of a case lives until the case ends. */
typedef struct {
    WIKIENTRY e;
    WIKISIM   s;
    int       live;
    TJDOC*    doc[4096];
    int       ndoc;
} CASE;

static void case_end(CASE* c)
{
    int i;
    if (c->live) {
        tagpu_wikisim_free(&c->s);
        tagpu_wikidata_entry_free(&c->e);
    }
    for (i = 0; i < c->ndoc; i++) tagpu_json_free(c->doc[i]);
    c->ndoc = 0;
    c->live = 0;
}

static int trace(const char* path)
{
    static CASE c;
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    char *line, *end;
    if (!text) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    for (line = text; *line; line = end) {
        TJDOC* doc;
        const TJV* v;
        const char* op;
        for (end = line; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!*line) continue;
        if (!(doc = tagpu_json_parse(line, strlen(line), err, sizeof err))) { fprintf(stderr, "wikitest: %s\n", err); return 2; }
        v = tagpu_json_root(doc);
        op = tj_str(tj_get(v, "op"), "");
        if (!strcmp(op, "create")) {
            const int scripted = (int)tj_num(tj_get(v, "scripted"), 0);
            char* next;
            case_end(&c);
            tagpu_json_free(doc);
            for (next = end; *end && *end != '\n'; end++) {}
            if (*end) *end++ = 0;
            if (!tagpu_wikidata_entry(&c.e, next, strlen(next), err, sizeof err)
                || !tagpu_wikisim_init(&c.s, &c.e, c.e.ndemo ? &c.e.demo[0] : NULL, scripted, err, sizeof err)) {
                fprintf(stderr, "wikitest: %s\n", err);
                return 2;
            }
            c.live = 1;
            continue;
        }
        if (!c.live || c.ndoc == (int)(sizeof c.doc / sizeof c.doc[0])) { fprintf(stderr, "wikitest: %s before a create, or too many\n", op); return 2; }
        c.doc[c.ndoc++] = doc;
        if (!strcmp(op, "input")) {
            WIKIINPUT in;
            if (!tagpu_wikidata_input(&c.e, tj_get(v, "ev"), &in, err, sizeof err)) { fprintf(stderr, "wikitest: %s\n", err); return 2; }
            tagpu_wikisim_input(&c.s, &in);
        } else if (!strcmp(op, "step")) tagpu_wikisim_step(&c.s);
        else if (!strcmp(op, "view")) { view(&c.s, 1); putchar('\n'); }
        else if (!strcmp(op, "orders")) { orders(&c.s, tj_str(tj_get(v, "id"), "")); putchar('\n'); }
        else if (!strcmp(op, "done")) puts(c.s.done ? "true" : "false");
        else if (!strcmp(op, "tick")) printf("%d\n", c.s.tick);
        else if (!strcmp(op, "hash")) { hash(&c.s); putchar('\n'); }
        else { fprintf(stderr, "wikitest: no op %s\n", op); return 2; }
    }
    case_end(&c);
    free(text);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc == 3 && !strcmp(argv[1], "demos")) return demos(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "trace")) return trace(argv[2]);
    fprintf(stderr, "usage: wikitest demos PAK | wikitest trace FILE\n");
    return 2;
}
