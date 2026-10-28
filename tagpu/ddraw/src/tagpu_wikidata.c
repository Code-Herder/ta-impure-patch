/* tagpu_wikidata.c -- the in-game wiki's content, read. Contract: tagpu_wikidata.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_pak.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikisim.h"           /* WIKI_TRIP, the build trip's ticks */

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

/* a scene unit's "facing": [x, z] as a unit vector, (0, 1) when absent or null; sim.js facing() */
static int facing(R* r, const TJV* v, double* hx, double* hz)
{
    double x, z, d;
    *hx = 0;
    *hz = 1;
    if (!v || tj_is(v, TJ_NULL)) return 1;
    if (!tj_is(v, TJ_ARR) || v->n != 2 || !tj_is(tj_at(v, 0), TJ_NUM) || !tj_is(tj_at(v, 1), TJ_NUM))
        return refuse(r, "a facing", "is not two numbers");
    x = tj_at(v, 0)->num;
    z = tj_at(v, 1)->num;
    d = sqrt(x * x + z * z);
    /* zero, or a square that underflows to 0 or overflows to infinity: no direction */
    if (!(d > 0 && d < INFINITY)) return refuse(r, "a facing", "has no length");
    *hx = x / d;
    *hz = z / d;
    return 1;
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

/* FBI canstop as sim.js reads it, `(canstop & 1) === 1`: a number whose integer part is odd, or
   true, is 1; absent, null or false is 0. JS's `&` takes the integer part modulo 2^32, which keeps
   its parity. Anything else (a string JS would coerce) is refused. */
static int canstop(R* r, const TJV* v, int* out)
{
    *out = 0;
    if (!v || tj_is(v, TJ_NULL) || tj_is(v, TJ_FALSE)) return 1;
    if (tj_is(v, TJ_TRUE)) { *out = 1; return 1; }
    if (!tj_is(v, TJ_NUM)) return refuse(r, "a canstop is neither a number nor a boolean", NULL);
    *out = isfinite(v->num) && fmod(fabs(trunc(v->num)), 2.0) == 1.0;
    return 1;
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
        /* THE BOUND: the name and its terminator fit WIKIDEF.name. Not empty, because a layout's
           build button names no unit with "" (WIKIGADGET.unit), which must match no type. */
        if (!*u->key || strlen(u->key) >= sizeof d->name) return refuse(r, "a type name is empty or too long:", u->key);
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
        if (!canstop(r, tj_get(u, "canstop"), &d->canstop)) return 0;
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
    /* one of the eight players: the 3D stage makes a record only for one, and hides the box of every
       type it resolved (tagpu_wikistage.c `owner8`, tagpu_wikistage_keep) */
    if (tj_get(v, "owner") && !tj_is(tj_get(v, "owner"), TJ_NULL)
        && !integer(r, tj_get(v, "owner"), 0, 7, &u->owner, "an owner (a player of the eight)"))
        return 0;
    u->built = 1;
    if (tj_get(v, "built") && !tj_is(tj_get(v, "built"), TJ_NULL) && !number(r, tj_get(v, "built"), &u->built, "built"))
        return 0;
    u->label = text_or_null(tj_get(v, "label"));
    if (!facing(r, tj_get(v, "facing"), &u->hx, &u->hz)) return 0;
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
    for (c = tj_is(v, TJ_OBJ) ? v->first : NULL; c; c = c->next)
        if (strcmp(c->key, "at") && strcmp(c->key, "caption") && strcmp(c->key, "build")) in->extra = 1;
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
    /* sim.js throws on a `sidebar` that is not a boolean when it plays the demo, and every demo is
       played, so the loader refuses it whatever reads the demo */
    if ((c = tj_get(v, "sidebar"))) {
        if (!tj_is(c, TJ_TRUE) && !tj_is(c, TJ_FALSE)) return refuse(r, "sidebar is not a boolean", NULL);
        d->has_sidebar = 1;
        d->sidebar = tj_is(c, TJ_TRUE);
    }
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

/* ---- the build trip's data rule ------------------------------------------------------------ */

/* the simulation's order: by tick, then file order */
static int trip_order(const void* a, const void* b)
{
    const WIKIINPUT* x = *(const WIKIINPUT* const*)a;
    const WIKIINPUT* y = *(const WIKIINPUT* const*)b;
    return x->at < y->at ? -1 : x->at > y->at ? 1 : x < y ? -1 : x > y;
}

static int trip_no(char* err, size_t errn, const WIKIDEMO* d, const WIKIINPUT* e, const char* what)
{
    if (err && errn)
        snprintf(err, errn, "wiki demo %s: the build at tick %.17g %s", d->id ? d->id : "?", e->at, what);
    return 0;
}

int tagpu_wikidata_trips(const WIKIDEMO* d, char* err, size_t errn)
{
    const double end = d->has_end ? d->end : INFINITY;
    const WIKIINPUT** in = (const WIKIINPUT**)malloc(((size_t)d->ninput + 1) * sizeof *in);
    int i, j, lmb = 0, ok = 1;
    if (!in) {
        snprintf(err, errn, "wiki demo %s: out of memory", d->id ? d->id : "?");
        return 0;
    }
    for (i = 0; i < d->ninput; i++) in[i] = &d->input[i];
    qsort((void*)in, (size_t)d->ninput, sizeof *in, trip_order);
    for (i = 0; ok && i < d->ninput; i++) {
        const WIKIINPUT* e = in[i];
        if (e->has & WIKI_IN_BUILD) {
            /* the inputs after it in the simulation's order are those of its tick fed after it
               and those of later ticks, so the first one past its trip ends the search */
            for (j = i + 1; j < d->ninput && in[j]->at < e->at + WIKI_TRIP; j++)
                if ((in[j]->has & WIKI_IN_BUILD) || in[j]->extra) break;
            /* the game has no button that arms nothing: a demo disarms with a right-click */
            if (e->build < 0) ok = trip_no(err, errn, d, e, "names no type");
            else if (e->extra) ok = trip_no(err, errn, d, e, "carries another input");
            /* the trip presses the left button itself */
            else if (lmb) ok = trip_no(err, errn, d, e, "starts with the left button held");
            else if (j < d->ninput && in[j]->at < e->at + WIKI_TRIP)
                ok = trip_no(err, errn, d, e, "has another input standing in its trip");
            else if (!(end >= e->at + WIKI_TRIP)) ok = trip_no(err, errn, d, e, "has the demo end inside its trip");
        }
        if (e->down == WIKI_KEY_LMB) lmb = 1;
        if (e->up == WIKI_KEY_LMB) lmb = 0;
    }
    free((void*)in);
    return ok;
}

/* ---- the sidebar's layout -------------------------------------------------------------------- */

/* sim.js's ROLES, in WIKI_ROLE_* order */
static const char* const ROLE_NAMES[WIKI_NROLES] = { "orders", "buildtab", "build", "prev", "next", "stop", "order" };

static int lay_no(char* err, size_t errn, const char* what, const char* name)
{
    if (err && errn)
        snprintf(err, errn, "the sidebar layout is refused: %s%s%s", what, name ? " " : "", name ? name : "");
    return 0;
}

/* a name field holds its terminator, so no read of it runs past the field */
static int terminated(const char* s) { return memchr(s, 0, WIKI_SIDE_NAME) != NULL; }

int tagpu_wikilayout_check(const WIKILAYOUT* L, char* err, size_t errn)
{
    int i, j, k, rows = WIKI_SIDE_MIN_H;
    if (L->format != 1) return lay_no(err, errn, "format is not 1", NULL);
    if (L->width != WIKI_SIDE_W) return lay_no(err, errn, "width is not the panel's 128", NULL);
    /* WIKI_SIDE_SCREENS: the screen table */
    if (L->nscreen < 0 || L->nscreen > WIKI_SIDE_SCREENS) return lay_no(err, errn, "more than 48 screens", NULL);
    for (i = 0; i < L->nscreen; i++) {
        const WIKISCREEN* s = &L->screen[i];
        if (!terminated(s->name)) return lay_no(err, errn, "a screen's name is not terminated", NULL);
        /* within the column, and y + h <= WIKI_SIDE_ROWS: the composed column's texture is
           128 x 1024; y >= 0 first, so WIKI_SIDE_ROWS - y cannot overflow */
        if (s->y < 0 || s->w < 1 || s->w > WIKI_SIDE_W || s->h < 1 || s->h > WIKI_SIDE_ROWS - s->y)
            return lay_no(err, errn, "a screen is not within the column:", s->name);
        if (s->y + s->h > rows) rows = s->y + s->h;
        /* WIKI_SIDE_GADGETS: a screen's gadget table, and the length of the view's states */
        if (s->ngadget < 0 || s->ngadget > WIKI_SIDE_GADGETS)
            return lay_no(err, errn, "more than 64 gadgets on", s->name);
        for (j = 0; j < s->ngadget; j++) {
            const WIKIGADGET* g = &s->gadget[j];
            if (!terminated(g->name) || !g->name[0] || g->role < 0 || g->role >= WIKI_NROLES || !terminated(g->unit)
                || (g->grey != 0 && g->grey != 1) || (g->hidden != 0 && g->hidden != 1))
                return lay_no(err, errn, "a gadget has no name, no known role or a flag not 0 or 1 on", s->name);
            /* a rect inside its screen: a hit or a pressed frame never reads outside the screen it
               belongs to; x, y >= 0 first, so neither difference can overflow */
            if (g->x < 0 || g->y < 0 || g->w < 1 || g->h < 1 || g->w > s->w - g->x || g->h > s->h - g->y)
                return lay_no(err, errn, "a gadget's rect is not inside its screen:", s->name);
        }
    }
    if (L->height != rows) return lay_no(err, errn, "height is not the column's rows", NULL);
    /* WIKI_SIDE_SIDES: the side table */
    if (L->nside < 1 || L->nside > WIKI_SIDE_SIDES) return lay_no(err, errn, "sides are not 1 to 8", NULL);
    for (i = 0; i < L->nside; i++) {
        const WIKISIDEDEF* d = &L->side[i];
        if (!terminated(d->name) || !d->name[0] || !terminated(d->prefix) || d->main2 < 0 || d->main2 >= L->nscreen
            || d->gen < 0 || d->gen >= L->nscreen)
            return lay_no(err, errn, "a side has no name, prefix, or MAIN2 and GEN screens", NULL);
    }
    if (L->player_side < 0 || L->player_side >= L->nside)
        return lay_no(err, errn, "player_side is not one of the sides", NULL);
    /* WIKI_SIDE_TYPES: WIKI_MAX_DEFS, the unit types one entry can name */
    if (L->ntype < 0 || L->ntype > WIKI_SIDE_TYPES) return lay_no(err, errn, "more than 64 types", NULL);
    for (i = 0; i < L->ntype; i++) {
        const WIKIPAGES* t = &L->type[i];
        if (!terminated(t->name)) return lay_no(err, errn, "a type's name is not terminated", NULL);
        /* one entry per type: the simulation finds a unit's pages by its type's name */
        for (j = 0; j < i; j++)
            if (!strcmp(L->type[j].name, t->name)) return lay_no(err, errn, "a type is named twice:", t->name);
        /* WIKI_SIDE_PAGES: the page is unit+0x110 bits 23..25, a 3-bit field, so the engine
           reaches at most 7 numbered pages */
        if (t->npage < 0 || t->npage > WIKI_SIDE_PAGES) return lay_no(err, errn, "more than 7 pages for", t->name);
        for (k = 0; k < t->npage; k++)
            if (t->page[k] < 0 || t->page[k] >= L->nscreen)
                return lay_no(err, errn, "a page is not a screen for", t->name);
        if (t->orders < -1 || t->orders >= L->nscreen) return lay_no(err, errn, "orders is not a screen for", t->name);
        /* def+0x22E != 0: a type with neither a numbered page nor an orders page is not listed */
        if (!t->npage && t->orders < 0) return lay_no(err, errn, "no page for", t->name);
    }
    return 1;
}

/* a JSON string of fewer than WIKI_SIDE_NAME bytes, empty only where `empty` admits it, into `out` */
static int lay_name(const TJV* v, char* out, int empty)
{
    size_t n;
    if (!tj_is(v, TJ_STR) || (n = strlen(v->str)) >= WIKI_SIDE_NAME || (!n && !empty)) return 0;
    memcpy(out, v->str, n + 1);
    return 1;
}

/* Number.isInteger, held to [lo, hi]: every range below holds every value checkLayout admits, so
   the refusal here refuses nothing it would take, and the value fits an int */
static int lay_int(const TJV* v, double lo, double hi, int* out)
{
    if (!tj_is(v, TJ_NUM) || !(v->num >= lo && v->num <= hi) || v->num != floor(v->num)) return 0;
    *out = (int)v->num;
    return 1;
}

/* a screen named by `v`, a non-empty string: its index, or -1 */
static int lay_screen(const WIKILAYOUT* L, const TJV* v)
{
    int i;
    if (!tj_is(v, TJ_STR) || !v->str[0]) return -1;
    for (i = 0; i < L->nscreen; i++)
        if (!strcmp(L->screen[i].name, v->str)) return i;
    return -1;
}

static int lay_flag(const TJV* v, int* out)
{
    *out = tj_is(v, TJ_TRUE);
    return !v || tj_is(v, TJ_TRUE) || tj_is(v, TJ_FALSE);
}

static int lay_gadget(const TJV* v, WIKIGADGET* g)
{
    const TJV *r = tj_get(v, "rect"), *u = tj_get(v, "unit");
    const char* role = tj_str(tj_get(v, "role"), NULL);
    if (!tj_is(v, TJ_OBJ) || !lay_name(tj_get(v, "name"), g->name, 0) || !role) return 0;
    for (g->role = 0; g->role < WIKI_NROLES && strcmp(ROLE_NAMES[g->role], role); g->role++) {}
    if (g->role == WIKI_NROLES) return 0;
    if (!tj_is(r, TJ_ARR) || r->n != 4
        || !lay_int(tj_at(r, 0), 0, WIKI_SIDE_W, &g->x) || !lay_int(tj_at(r, 1), 0, WIKI_SIDE_ROWS, &g->y)
        || !lay_int(tj_at(r, 2), 1, WIKI_SIDE_W, &g->w) || !lay_int(tj_at(r, 3), 1, WIKI_SIDE_ROWS, &g->h))
        return 0;
    /* a build button's unit is a name, or null for none; sim.js reads no other role's */
    if (g->role == WIKI_ROLE_BUILD && !tj_is(u, TJ_NULL) && !lay_name(u, g->unit, 0)) return 0;
    return lay_flag(tj_get(v, "grey"), &g->grey) && lay_flag(tj_get(v, "hidden"), &g->hidden);
}

static int lay_read(const TJV* root, WIKILAYOUT* L, char* err, size_t errn)
{
    const TJV *c, *g, *screens = tj_get(root, "screens"), *sides = tj_get(root, "sides");
    const TJV* types = tj_get(root, "types");
    int i;
    if (!tj_is(root, TJ_OBJ) || !tj_is(tj_get(root, "format"), TJ_NUM) || tj_get(root, "format")->num != 1)
        return lay_no(err, errn, "format is not 1", NULL);
    L->format = 1;
    if (!tj_is(tj_get(root, "width"), TJ_NUM) || tj_get(root, "width")->num != WIKI_SIDE_W)
        return lay_no(err, errn, "width is not the panel's 128", NULL);
    L->width = WIKI_SIDE_W;
    if (!tj_is(screens, TJ_OBJ) || screens->n > WIKI_SIDE_SCREENS)
        return lay_no(err, errn, "screens is not an object of at most 48", NULL);
    for (c = screens->first; c; c = c->next) {
        WIKISCREEN* s = &L->screen[L->nscreen];
        const TJV* gl = tj_get(c, "gadgets");
        if (strlen(c->key) >= WIKI_SIDE_NAME) return lay_no(err, errn, "a screen's name is too long:", c->key);
        for (i = 0; i < L->nscreen; i++)
            if (!strcmp(L->screen[i].name, c->key)) return lay_no(err, errn, "a screen is named twice:", c->key);
        strcpy(s->name, c->key);
        if (!tj_is(c, TJ_OBJ) || !lay_int(tj_get(c, "y"), 0, WIKI_SIDE_ROWS, &s->y)
            || !lay_int(tj_get(c, "w"), 1, WIKI_SIDE_W, &s->w) || !lay_int(tj_get(c, "h"), 1, WIKI_SIDE_ROWS, &s->h))
            return lay_no(err, errn, "a screen has no integer y, w and h within the column:", c->key);
        if (!tj_is(gl, TJ_ARR) || gl->n > WIKI_SIDE_GADGETS)
            return lay_no(err, errn, "a screen has no list of at most 64 gadgets:", c->key);
        for (g = gl->first; g; g = g->next)
            if (!lay_gadget(g, &s->gadget[s->ngadget++]))
                return lay_no(err, errn, "a gadget is not a named, known, inside button on", c->key);
        L->nscreen++;
    }
    if (!lay_int(tj_get(root, "height"), 0, WIKI_SIDE_ROWS, &L->height))
        return lay_no(err, errn, "height is not the column's rows", NULL);
    if (!tj_is(sides, TJ_ARR) || sides->n < 1 || sides->n > WIKI_SIDE_SIDES)
        return lay_no(err, errn, "sides is not a list of 1 to 8", NULL);
    for (c = sides->first; c; c = c->next) {
        WIKISIDEDEF* d = &L->side[L->nside++];
        if (!tj_is(c, TJ_OBJ) || !lay_name(tj_get(c, "name"), d->name, 0) || !lay_name(tj_get(c, "prefix"), d->prefix, 1)
            || (d->main2 = lay_screen(L, tj_get(c, "main2"))) < 0 || (d->gen = lay_screen(L, tj_get(c, "gen"))) < 0)
            return lay_no(err, errn, "a side has no name, prefix, or MAIN2 and GEN screens", NULL);
    }
    if (!lay_int(tj_get(root, "player_side"), 0, WIKI_SIDE_SIDES - 1, &L->player_side))
        return lay_no(err, errn, "player_side is not one of the sides", NULL);
    if (!tj_is(types, TJ_OBJ) || types->n > WIKI_SIDE_TYPES)
        return lay_no(err, errn, "types is not an object of at most 64", NULL);
    for (c = types->first; c; c = c->next) {
        WIKIPAGES* t = &L->type[L->ntype++];
        const TJV *pages = tj_get(c, "pages"), *orders = tj_get(c, "orders");
        if (strlen(c->key) >= WIKI_SIDE_NAME) return lay_no(err, errn, "a type's name is too long:", c->key);
        strcpy(t->name, c->key);
        if (!tj_is(c, TJ_OBJ) || !tj_is(pages, TJ_ARR) || pages->n > WIKI_SIDE_PAGES)
            return lay_no(err, errn, "a type has no list of at most 7 page screens:", c->key);
        for (g = pages->first; g; g = g->next)
            if ((t->page[t->npage++] = lay_screen(L, g)) < 0) return lay_no(err, errn, "a page is not a screen for", c->key);
        if (tj_is(orders, TJ_NULL)) t->orders = -1;
        else if ((t->orders = lay_screen(L, orders)) < 0) return lay_no(err, errn, "orders is not a screen or null for", c->key);
    }
    return tagpu_wikilayout_check(L, err, errn);
}

WIKILAYOUT* tagpu_wikidata_layout_json(const TJV* v, char* err, size_t errn)
{
    WIKILAYOUT* L = (WIKILAYOUT*)calloc(1, sizeof *L);
    if (!L) {
        lay_no(err, errn, "out of memory", NULL);
        return NULL;
    }
    if (!lay_read(v, L, err, errn)) {
        free(L);
        return NULL;
    }
    return L;
}

WIKILAYOUT* tagpu_wikidata_layout(const char* text, size_t len, char* err, size_t errn)
{
    TJDOC* doc = tagpu_json_parse(text, len, err, errn);
    WIKILAYOUT* L;
    if (!doc) return NULL;
    L = tagpu_wikidata_layout_json(tagpu_json_root(doc), err, errn);
    tagpu_json_free(doc);
    return L;
}

WIKILAYOUT* tagpu_wikidata_layout_pak(const unsigned char* pak, size_t npak, const char* path, char* err, size_t errn)
{
    size_t n;
    const unsigned char* t = tagpu_pak_file(pak, npak, path, &n);
    if (!t) {
        snprintf(err, errn, "wiki: the archive has no %s", path);
        return NULL;
    }
    return tagpu_wikidata_layout((const char*)t, n, err, errn);
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
    free((void*)d->fixture);
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
    if (tagpu_pak_file(pak, npak, "side/fixture.json", &n)
        && !(d->fixture = tagpu_wikidata_layout_pak(pak, npak, "side/fixture.json", err, errn)))
        goto fail;
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
