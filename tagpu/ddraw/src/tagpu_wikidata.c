/* tagpu_wikidata.c -- the in-game wiki's content, read. Contract: tagpu_wikidata.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_pak.h"
#include "tagpu_wikidata.h"

#define FOOTPRINT_MAX 64    /* tagpu_placegeo.c's bound on a footprint, which its arithmetic rests on */
#define TREE_MAX      1024  /* nodes of the tree */

static const char* const KEY_NAMES[WIKI_NKEYS] = { "shift", "alt", "ctrl", "z", "x", "lmb", "rmb" };

const char* tagpu_wikidata_key_name(int key)
{
    return key >= 0 && key < WIKI_NKEYS ? KEY_NAMES[key] : NULL;
}

int tagpu_wikidata_def(const WIKIENTRY* e, const char* name)
{
    int i;
    for (i = 0; i < e->ndef; i++)
        if (!strcmp(e->def[i].name, name)) return i;
    return -1;
}

/* ---- reading -------------------------------------------------------------------------------- */

typedef struct {
    const WIKIENTRY* e;
    char*       err;
    size_t      errn;
    int         failed;
    const char* where;      /* the demo being read, for the message */
} R;

static int refuse(R* r, const char* what, const char* name)
{
    if (!r->failed && r->err && r->errn)
        snprintf(r->err, r->errn, "wiki entry %s%s%s: %s%s%s", r->e->id ? r->e->id : "?", r->where ? " demo " : "",
                 r->where ? r->where : "", what, name ? " " : "", name ? name : "");
    r->failed = 1;
    return 0;
}

/* calloc that answers non-NULL for no elements, so that an empty array and a failure differ */
static void* zalloc(R* r, size_t n, size_t size)
{
    void* p = calloc(n ? n : 1, size);
    if (!p) refuse(r, "out of memory", NULL);
    return p;
}

/* the elements of a list, and 0 for anything else: every caller then walks the list alone, so an
   object's members would be counted and never read */
static int count(const TJV* v) { return tj_is(v, TJ_ARR) ? v->n : 0; }

/* a non-empty string, or NULL: the mockup's `x || null` */
static const char* text_or_null(const TJV* v)
{
    const char* s = tj_str(v, NULL);
    return s && *s ? s : NULL;
}

static int integer(R* r, const TJV* v, int lo, int hi, int* out, const char* what)
{
    double d = tj_num(v, NAN);
    if (!(d >= lo && d <= hi) || d != floor(d)) return refuse(r, what, "is not an integer in range");
    *out = (int)d;
    return 1;
}

static int number(R* r, const TJV* v, double* out, const char* what)
{
    if (!tj_is(v, TJ_NUM)) return refuse(r, what, "is not a number");
    *out = v->num;
    return 1;
}

static int point(R* r, const TJV* v, WIKIPOINT* p, const char* what)
{
    return number(r, tj_at(v, 0), &p->x, what) && number(r, tj_at(v, 1), &p->z, what);
}

static int rect(R* r, const TJV* v, int* x, int* z, int* w, int* h)
{
    return integer(r, tj_at(v, 0), -1000000, 1000000, x, "a rectangle")
        && integer(r, tj_at(v, 1), -1000000, 1000000, z, "a rectangle")
        && integer(r, tj_at(v, 2), 0, 1000000, w, "a rectangle")
        && integer(r, tj_at(v, 3), 0, 1000000, h, "a rectangle");
}

static int strings(R* r, const TJV* v, const char*** out, int* n)
{
    const TJV* c;
    int i = 0;
    *n = count(v);
    if (!(*out = (const char**)zalloc(r, (size_t)*n, sizeof **out))) return 0;
    for (c = tj_is(v, TJ_ARR) ? v->first : NULL; c; c = c->next) {
        if (!tj_is(c, TJ_STR)) return refuse(r, "a list holds something not a string", NULL);
        (*out)[i++] = c->str;
    }
    return 1;
}

static int type_of(R* r, const TJV* v, int* def)
{
    const char* s = tj_str(v, NULL);
    if (!s || (*def = tagpu_wikidata_def(r->e, s)) < 0) return refuse(r, "no unit type", s ? s : "(none)");
    return 1;
}

static int key_of(R* r, const TJV* v, int* key)
{
    const char* s = text_or_null(v);
    int k;
    *key = -1;
    if (!s) return 1;
    for (k = 0; k < WIKI_NKEYS; k++)
        if (!strcmp(KEY_NAMES[k], s)) { *key = k; return 1; }
    return refuse(r, "no key", s);
}

static int defs(R* r, WIKIENTRY* e, const TJV* units)
{
    const TJV* u;
    int i = 0;
    if (!tj_is(units, TJ_OBJ) || units->n > WIKI_MAX_DEFS) return refuse(r, "units is not an object of at most 64 types", NULL);
    e->ndef = units->n;
    if (!(e->def = (WIKIDEF*)zalloc(r, (size_t)e->ndef, sizeof *e->def))) return 0;
    for (u = units->first; u; u = u->next, i++) {
        WIKIDEF* d = &e->def[i];
        if (strlen(u->key) >= sizeof d->name) return refuse(r, "a type name is too long:", u->key);
        strcpy(d->name, u->key);
        d->title = tj_str(tj_get(u, "name"), NULL);
        if (!integer(r, tj_at(tj_get(u, "footprint"), 0), 1, FOOTPRINT_MAX, &d->fx, "a footprint")
            || !integer(r, tj_at(tj_get(u, "footprint"), 1), 1, FOOTPRINT_MAX, &d->fz, "a footprint"))
            return 0;
        d->build_time     = tj_num(tj_get(u, "buildTime"), 0);
        d->worker_time    = tj_num(tj_get(u, "workerTime"), 0);
        d->build_distance = tj_num(tj_get(u, "buildDistance"), 0);
        d->max_velocity   = tj_num(tj_get(u, "maxVelocity"), 0);
        d->extracts       = tj_num(tj_get(u, "extracts"), 0);
        d->energy_make    = tj_num(tj_get(u, "energyMake"), 0);
        d->vent           = tj_is(tj_get(u, "vent"), TJ_TRUE);
        if (!(d->build_time > 0)) return refuse(r, "no positive buildTime for", d->name);
    }
    /* the build lists once every type is known; a name the entry does not define is never asked
       about, since the sim asks only about the entry's types */
    for (u = units->first, i = 0; u; u = u->next, i++) {
        const TJV* b;
        for (b = tj_is(tj_get(u, "builds"), TJ_ARR) ? tj_get(u, "builds")->first : NULL; b; b = b->next) {
            int k = b->type == TJ_STR ? tagpu_wikidata_def(e, b->str) : -1;
            if (k >= 0) e->def[i].builds |= (uint64_t)1 << k;
        }
    }
    return 1;
}

static int scene_unit(R* r, const TJV* v, WIKISCENEUNIT* u)
{
    const char* id = text_or_null(tj_get(v, "id"));
    const TJV *o, *orders = tj_get(v, "orders");
    WIKIPOINT at;
    int i = 0;
    if (id && strlen(id) >= sizeof u->id) return refuse(r, "a unit id is too long:", id);
    if (id) strcpy(u->id, id);
    if (!type_of(r, tj_get(v, "type"), &u->def)) return 0;
    if (!point(r, tj_get(v, "at"), &at, "a unit's position")) return 0;
    u->x = at.x;
    u->z = at.z;
    u->owner = 0;
    if (tj_get(v, "owner") && !tj_is(tj_get(v, "owner"), TJ_NULL)
        && !integer(r, tj_get(v, "owner"), 0, 255, &u->owner, "an owner"))
        return 0;
    u->built = 1;
    if (tj_get(v, "built") && !tj_is(tj_get(v, "built"), TJ_NULL) && !number(r, tj_get(v, "built"), &u->built, "built"))
        return 0;
    u->label = text_or_null(tj_get(v, "label"));
    u->nmove = count(orders);
    if (!(u->move = (WIKIPOINT*)zalloc(r, (size_t)u->nmove, sizeof *u->move))) return 0;
    for (o = tj_is(orders, TJ_ARR) ? orders->first : NULL; o; o = o->next)
        if (!point(r, tj_get(o, "move"), &u->move[i++], "a move order")) return 0;
    return 1;
}

/* the entry's scene (`entry` set: its map counts) or a demo's additions */
static int scene(R* r, const TJV* v, WIKISCENE* s, int entry)
{
    const TJV* c;
    int i;
    if (entry) {
        const TJV* map = tj_get(v, "map");
        if (!integer(r, tj_get(map, "w"), 1, WIKI_MAP_MAX, &s->w, "the map's width")
            || !integer(r, tj_get(map, "h"), 1, WIKI_MAP_MAX, &s->h, "the map's height"))
            return 0;
        s->surface_metal = tj_num(tj_get(map, "surfaceMetal"), 0);
    }
    s->nmetal = count(tj_get(v, "metal"));
    s->nvent = count(tj_get(v, "vents"));
    s->nfog = count(tj_get(v, "fog"));
    s->nunit = count(tj_get(v, "units"));
    if (!(s->metal = (WIKIMETAL*)zalloc(r, (size_t)s->nmetal, sizeof *s->metal))
        || !(s->vent = (WIKIPOINT*)zalloc(r, (size_t)s->nvent, sizeof *s->vent))
        || !(s->fog = (WIKIFOG*)zalloc(r, (size_t)s->nfog, sizeof *s->fog))
        || !(s->unit = (WIKISCENEUNIT*)zalloc(r, (size_t)s->nunit, sizeof *s->unit)))
        return 0;
    for (c = tj_is(tj_get(v, "metal"), TJ_ARR) ? tj_get(v, "metal")->first : NULL, i = 0; c; c = c->next, i++) {
        WIKIMETAL* m = &s->metal[i];
        if (!rect(r, tj_get(c, "rect"), &m->x, &m->z, &m->w, &m->h) || !number(r, tj_get(c, "v"), &m->v, "a metal value"))
            return 0;
    }
    for (c = tj_is(tj_get(v, "vents"), TJ_ARR) ? tj_get(v, "vents")->first : NULL, i = 0; c; c = c->next, i++)
        if (!point(r, c, &s->vent[i], "a vent")) return 0;
    for (c = tj_is(tj_get(v, "fog"), TJ_ARR) ? tj_get(v, "fog")->first : NULL, i = 0; c; c = c->next, i++) {
        WIKIFOG* f = &s->fog[i];
        const char* kind = tj_str(tj_get(c, "kind"), "");
        if (!rect(r, tj_get(c, "rect"), &f->x, &f->z, &f->w, &f->h)) return 0;
        f->black = !strcmp(kind, "black");
    }
    for (c = tj_is(tj_get(v, "units"), TJ_ARR) ? tj_get(v, "units")->first : NULL, i = 0; c; c = c->next, i++)
        if (!scene_unit(r, c, &s->unit[i])) return 0;
    /* `selected` and `cursor` replace the entry's when given: the mockup's `??`, so null is absent */
    c = tj_get(v, "selected");
    if ((s->has_selected = c && !tj_is(c, TJ_NULL)) && !strings(r, c, &s->selected, &s->nselected)) return 0;
    c = tj_get(v, "cursor");
    if ((s->has_cursor = c && !tj_is(c, TJ_NULL)) && !point(r, c, &s->cursor, "the cursor")) return 0;
    if (!s->selected && !(s->selected = (const char**)zalloc(r, 0, sizeof *s->selected))) return 0;
    return 1;
}

static int input(R* r, const TJV* v, WIKIINPUT* in)
{
    const TJV* c;
    memset(in, 0, sizeof *in);
    in->build = -1;
    in->at = tj_num(tj_get(v, "at"), 0);
    if (tj_get(v, "caption")) {
        in->has |= WIKI_IN_CAPTION;
        in->caption = text_or_null(tj_get(v, "caption"));
    }
    if ((c = tj_get(v, "build"))) {
        in->has |= WIKI_IN_BUILD;
        if (text_or_null(c) && !type_of(r, c, &in->build)) return 0;
    }
    if ((c = tj_get(v, "move")) && !tj_is(c, TJ_NULL)) {
        in->has |= WIKI_IN_MOVE;
        if (!point(r, c, &in->move, "a move")) return 0;
    }
    return key_of(r, tj_get(v, "down"), &in->down) && key_of(r, tj_get(v, "up"), &in->up);
}

int tagpu_wikidata_input(const WIKIENTRY* e, const TJV* v, WIKIINPUT* in, char* err, size_t errn)
{
    R r;
    memset(&r, 0, sizeof r);
    r.e = e;
    r.err = err;
    r.errn = errn;
    return input(&r, v, in);
}

static int demo(R* r, const TJV* v, WIKIDEMO* d)
{
    const TJV *c, *inputs = tj_get(v, "inputs"), *end = tj_get(v, "end");
    int i = 0;
    d->id = tj_str(tj_get(v, "id"), NULL);
    d->title = tj_str(tj_get(v, "title"), NULL);
    r->where = d->id ? d->id : "?";
    if (!scene(r, tj_get(v, "scene"), &d->scene, 0)) return 0;
    d->ninput = count(inputs);
    if (!(d->input = (WIKIINPUT*)zalloc(r, (size_t)d->ninput, sizeof *d->input))) return 0;
    for (c = tj_is(inputs, TJ_ARR) ? inputs->first : NULL; c; c = c->next)
        if (!tj_is(tj_get(c, "at"), TJ_NUM)) return refuse(r, "an input has no tick", NULL);
        else if (!input(r, c, &d->input[i++])) return 0;
    if ((d->has_end = end && !tj_is(end, TJ_NULL)) && !number(r, end, &d->end, "the end")) return 0;
    r->where = NULL;
    return 1;
}

static void scene_free(WIKISCENE* s)
{
    int i;
    for (i = 0; s->unit && i < s->nunit; i++) free(s->unit[i].move);
    free(s->metal);
    free(s->vent);
    free(s->fog);
    free(s->unit);
    free((void*)s->selected);
}

void tagpu_wikidata_entry_free(WIKIENTRY* e)
{
    int i;
    for (i = 0; e->demo && i < e->ndemo; i++) {
        scene_free(&e->demo[i].scene);
        free(e->demo[i].input);
    }
    free(e->demo);
    scene_free(&e->scene);
    free(e->def);
    free((void*)e->text);
    free((void*)e->locked_in);
    if (e->doc) tagpu_json_free(e->doc);
    memset(e, 0, sizeof *e);
}

int tagpu_wikidata_entry(WIKIENTRY* e, const char* text, size_t len, char* err, size_t errn)
{
    R r;
    const TJV *root, *c;
    double pace;
    int i = 0;
    memset(e, 0, sizeof *e);
    memset(&r, 0, sizeof r);
    r.e = e;
    r.err = err;
    r.errn = errn;
    if (!(e->doc = tagpu_json_parse(text, len, err, errn))) return 0;
    root = tagpu_json_root(e->doc);
    e->id = tj_str(tj_get(root, "id"), NULL);
    e->title = tj_str(tj_get(root, "title"), NULL);
    e->subtitle = tj_str(tj_get(root, "subtitle"), NULL);
    /* the mockup's `(entry.pace && entry.pace.build) || 1` */
    pace = tj_num(tj_get(tj_get(root, "pace"), "build"), 0);
    e->pace = pace ? pace : 1;
    if (tj_num(tj_get(root, "format"), 0) != 1) refuse(&r, "is not format 1", NULL);
    else if (!strings(&r, tj_get(root, "text"), &e->text, &e->ntext)) {}
    else if (!strings(&r, tj_get(root, "lockedIn"), &e->locked_in, &e->nlocked)) {}
    else if (!defs(&r, e, tj_get(root, "units"))) {}
    else if (!scene(&r, tj_get(root, "scene"), &e->scene, 1)) {}
    else if (!(e->demo = (WIKIDEMO*)zalloc(&r, (size_t)(e->ndemo = count(tj_get(root, "demos"))), sizeof *e->demo))) {}
    else
        for (c = tj_is(tj_get(root, "demos"), TJ_ARR) ? tj_get(root, "demos")->first : NULL; c; c = c->next)
            if (!demo(&r, c, &e->demo[i++])) break;
    if (r.failed) {
        tagpu_wikidata_entry_free(e);
        return 0;
    }
    return 1;
}

/* ---- the tree ------------------------------------------------------------------------------- */

/* A node's entry: a leaf names one as a string, and anything else is a folder whose children are
   read. tree_count and tree_flatten both ask this one question, so the nodes the second writes are
   the nodes the first counted and allocated. */
static const char* node_entry(const TJV* node)
{
    return tj_str(tj_get(node, "entry"), NULL);
}

static int tree_count(const TJV* nodes, int depth)
{
    const TJV* c;
    int n = 0;
    if (depth > 32) return TREE_MAX + 1;
    for (c = tj_is(nodes, TJ_ARR) ? nodes->first : NULL; c && n <= TREE_MAX; c = c->next)
        n += 1 + (node_entry(c) ? 0 : tree_count(tj_get(c, "children"), depth + 1));
    return n;
}

/* the nodes in preorder, each leaf's entry read from the archive; an entry named twice is read once.
   THE BOUND: `cap` is the nodes allocated, and no node is written past it */
static int tree_flatten(WIKIDATA* d, int cap, const TJV* nodes, int depth, const unsigned char* pak,
                        size_t npak, char* err, size_t errn)
{
    const TJV* c;
    for (c = tj_is(nodes, TJ_ARR) ? nodes->first : NULL; c; c = c->next) {
        WIKINODE* n;
        const char* id = node_entry(c);
        int j;
        if (d->nnode >= cap) {
            snprintf(err, errn, "wiki: the tree has more nodes than it counted");
            return 0;
        }
        n = &d->node[d->nnode++];
        n->depth = depth;
        n->entry = -1;
        if (!id) {
            n->title = tj_str(tj_get(c, "title"), NULL);
            if (!tree_flatten(d, cap, tj_get(c, "children"), depth + 1, pak, npak, err, errn)) return 0;
            continue;
        }
        for (j = 0; j < d->nentry && strcmp(d->entry[j].id ? d->entry[j].id : "", id); j++) {}
        if (j == d->nentry) {
            char path[96];
            const unsigned char* t;
            size_t len;
            snprintf(path, sizeof path, "entries/%s.json", id);
            if (!(t = tagpu_pak_file(pak, npak, path, &len))) {
                snprintf(err, errn, "wiki: the archive has no %s", path);
                return 0;
            }
            if (!tagpu_wikidata_entry(&d->entry[j], (const char*)t, len, err, errn)) return 0;
            d->nentry++;
        }
        n->entry = j;
    }
    return 1;
}

void tagpu_wikidata_free(WIKIDATA* d)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->nentry; i++) tagpu_wikidata_entry_free(&d->entry[i]);
    free(d->entry);
    free(d->node);
    if (d->doc) tagpu_json_free(d->doc);
    free(d);
}

WIKIDATA* tagpu_wikidata_load(const unsigned char* pak, size_t npak, char* err, size_t errn)
{
    WIKIDATA* d = (WIKIDATA*)calloc(1, sizeof *d);
    const unsigned char* t;
    const TJV* root;
    size_t n;
    int nodes;
    if (!d) { snprintf(err, errn, "wiki: out of memory"); return NULL; }
    if (!(t = tagpu_pak_file(pak, npak, "tree.json", &n))) {
        snprintf(err, errn, "wiki: the archive has no tree.json");
        goto fail;
    }
    if (!(d->doc = tagpu_json_parse((const char*)t, n, err, errn))) goto fail;
    root = tagpu_json_root(d->doc);
    if (tj_num(tj_get(root, "format"), 0) != 1) {
        snprintf(err, errn, "wiki: tree.json is not format 1");
        goto fail;
    }
    if ((nodes = tree_count(tj_get(root, "tree"), 0)) > TREE_MAX) {
        snprintf(err, errn, "wiki: the tree is deeper than 32 or has more than %d nodes", TREE_MAX);
        goto fail;
    }
    if (!(d->node = (WIKINODE*)calloc((size_t)nodes + 1, sizeof *d->node))
        || !(d->entry = (WIKIENTRY*)calloc((size_t)nodes + 1, sizeof *d->entry))) {
        snprintf(err, errn, "wiki: out of memory");
        goto fail;
    }
    if (!tree_flatten(d, nodes, tj_get(root, "tree"), 0, pak, npak, err, errn)) goto fail;
    return d;
fail:
    tagpu_wikidata_free(d);
    return NULL;
}

#ifdef _WIN32
WIKIDATA* tagpu_wikidata_load_resource(char* err, size_t errn)
{
    size_t n;
    const unsigned char* pak = tagpu_pak_resource("WIKIPAK", &n);
    if (!pak) {
        snprintf(err, errn, "wiki: no WIKIPAK resource");
        return NULL;
    }
    return tagpu_wikidata_load(pak, n, err, errn);
}
#endif
