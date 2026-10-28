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

/* sim.js's KEYS then ACTIONS, in WIKI_KEY_* and WIKI_ACT_* order */
static const char* const KEY_NAMES[WIKI_NKEYNAMES] = { "shift", "alt", "ctrl", "z", "x", "lmb", "rmb",
                                                       "select", "order", "cancel" };
/* sim.js's and text.js's CASTS, in WIKI_CAST_* order */
static const char* const CAST_NAMES[WIKI_NCAST] = { "builder", "advbuilder", "mex", "moho", "geo",
                                                    "walker", "tower", "wall", "solar" };

const char* tagpu_wikidata_key_name(int key)
{
    return key >= 0 && key < WIKI_NKEYNAMES ? KEY_NAMES[key] : NULL;
}

const char* tagpu_wikidata_cast_name(int cast)
{
    return cast >= 0 && cast < WIKI_NCAST ? CAST_NAMES[cast] : NULL;
}

int tagpu_wikidata_button(int key, int iface)
{
    return key == WIKI_ACT_SELECT ? WIKI_KEY_LMB : key == WIKI_ACT_CANCEL ? WIKI_KEY_RMB
         : key == WIKI_ACT_ORDER ? (iface == 1 ? WIKI_KEY_RMB : WIKI_KEY_LMB) : key;
}

/* a cast by name, or -1 */
static int cast_of(const char* s)
{
    int c;
    for (c = 0; s && c < WIKI_NCAST; c++)
        if (!strcmp(CAST_NAMES[c], s)) return c;
    return -1;
}

/* A number as JS's String() writes it, for messages that quote sim.js's: the shortest %g that
   reads back as the same double, which is the digits JS gives for every value a message here
   quotes (ticks, small integers and their fractions). */
static const char* jsnum(char* out, size_t n, double v)
{
    int p;
    for (p = 1; p < 17; p++) {
        snprintf(out, n, "%.*g", p, v);
        if (strtod(out, NULL) == v) return out;
    }
    snprintf(out, n, "%.17g", v);
    return out;
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
    for (k = 0; k < WIKI_NKEYNAMES; k++)
        if (!strcmp(KEY_NAMES[k], s)) { *key = k; return 1; }
    return refuse(r, "no key", s);
}

/* An FBI flag (canstop, activatewhenbuilt) as sim.js reads it, `(v & 1) === 1`: a number whose
   integer part is odd, or true, is 1; absent, null or false is 0. JS's `&` takes the integer part
   modulo 2^32, which keeps its parity. Anything else (a string JS would coerce) is refused. */
static int fbi_flag(R* r, const TJV* v, const char* key, int* out)
{
    *out = 0;
    if (!v || tj_is(v, TJ_NULL) || tj_is(v, TJ_FALSE)) return 1;
    if (tj_is(v, TJ_TRUE)) { *out = 1; return 1; }
    if (!tj_is(v, TJ_NUM)) return refuse(r, "a flag is neither a number nor a boolean:", key);
    *out = isfinite(v->num) && fmod(fabs(trunc(v->num)), 2.0) == 1.0;
    return 1;
}

/* A type's G cells into rows of WIKI_FP_MAX bits, sim.js's checkG and gCell: `g` lists them as
   [dx, dz] inside the fx x fz footprint, and `vent` is set exactly when that list is not empty;
   with no `g`, a `vent` type is all G (stock's ARMGEO) and any other has none. The G cell is the
   yard code 0x8F, the only one carrying bit 0x80 (0x42CF9D..0x42D06B, DISASSEMBLED). THE BOUND:
   the caller holds fx and fz to WIKI_FP_MAX for a type with a G cell, and every cell set here is
   inside fx x fz, so no bit lands past a row or a row past the array. 0 when `g` breaks the rule. */
static int g_rows(const TJV* g, int vent, int fx, int fz, uint16_t* rows)
{
    const TJV* c;
    int dz;
    memset(rows, 0, WIKI_FP_MAX * sizeof *rows);
    if (!g) {
        for (dz = 0; vent && dz < fz; dz++) rows[dz] = (uint16_t)((1u << fx) - 1);
        return 1;
    }
    if (!tj_is(g, TJ_ARR) || !vent != !g->n) return 0;
    for (c = g->first; c; c = c->next) {
        const TJV *x = tj_at(c, 0), *z = tj_at(c, 1);
        if (!tj_is(c, TJ_ARR) || c->n != 2 || !tj_is(x, TJ_NUM) || !tj_is(z, TJ_NUM) || x->num != floor(x->num)
            || z->num != floor(z->num) || !(x->num >= 0 && x->num < fx) || !(z->num >= 0 && z->num < fz))
            return 0;
        rows[(int)z->num] |= (uint16_t)(1u << (int)x->num);
    }
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
        d->cast = cast_of(tj_str(tj_get(u, "cast"), NULL));
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
        /* WIKI_FP_MAX: the G rows' width; no entry's G type comes near it (ARMGEO is 4 x 4) */
        if (d->vent && (d->fx > WIKI_FP_MAX || d->fz > WIKI_FP_MAX))
            return refuse(r, "a type with G cells has a footprint past 16 a side:", d->name);
        if (!g_rows(tj_get(u, "g"), d->vent, d->fx, d->fz, d->g)) {
            char why[WIKI_ID_MAX + 112];
            snprintf(why, sizeof why, "unit %s: g is not a list of G cells inside the footprint, held by vent exactly "
                     "when it is not empty", d->name);
            return refuse(r, why, NULL);
        }
        if (!fbi_flag(r, tj_get(u, "canstop"), "canstop", &d->canstop)) return 0;
        if (!fbi_flag(r, tj_get(u, "activatewhenbuilt"), "activatewhenbuilt", &d->activate_when_built)) return 0;
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
    /* A WAIT, sim.js's data rule (make): `until` a state it knows, `max` an integer 1..WIKI_UNTIL_MAX
       ticks, and nothing but `at` beside them. sim.js checks it as it plays the demo, and every
       demo is played, so the loader refuses it whatever reads the demo; the rule that no other
       input shares its tick is tagpu_wikisim_init's. */
    if ((c = tj_get(v, "until"))) {
        const TJV* m = tj_get(v, "max");
        const TJV* k;
        if (!tj_is(c, TJ_STR) || (strcmp(c->str, "idle") && strcmp(c->str, "arrived")))
            return refuse(r, "a wait names no state the sim knows", NULL);
        if (!m || !tj_is(m, TJ_NUM) || m->num != floor(m->num) || !(m->num >= 1 && m->num <= WIKI_UNTIL_MAX))
            return refuse(r, "a wait has no max of 1 to 3600 ticks", NULL);
        for (k = v->first; k; k = k->next)
            if (strcmp(k->key, "at") && strcmp(k->key, "until") && strcmp(k->key, "max"))
                return refuse(r, "a wait carries another input", NULL);
        in->has |= WIKI_IN_UNTIL;
        in->until = strcmp(c->str, "idle") ? WIKI_UNTIL_ARRIVED : WIKI_UNTIL_IDLE;
        in->until_max = (int)m->num;
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

/* ---- the text's tokens: text.js's checkText and checkEntry, which is their specification ---- */

/* text.js's BUTTONS: each button token's name and its word, [interface type 0, type 1], the capital
   form starting a sentence. Type 0 orders with the left button and type 1 with the right; both
   select with the left button and cancel with the right, as the demos' actions resolve
   (tagpu_wikidata_button), so only {order} follows the interface type. */
static const struct { const char* name; const char* word[2]; } BUTTONS[] = {
    { "order",  { "left-click",  "right-click" } }, { "Order",  { "Left-click",  "Right-click" } },
    { "select", { "click",       "click" } },       { "Select", { "Click",       "Click" } },
    { "cancel", { "right-click", "right-click" } }, { "Cancel", { "Right-click", "Right-click" } },
};
#define NBUTTON (sizeof BUTTONS / sizeof *BUTTONS)

static int button_of(const char* s, size_t len)
{
    size_t k;
    for (k = 0; k < NBUTTON; k++)
        if (strlen(BUTTONS[k].name) == len && !strncmp(BUTTONS[k].name, s, len)) return (int)k;
    return -1;
}

const char* tagpu_wikidata_button_word(const char* s, size_t len, int iface)
{
    const int k = button_of(s, len);
    return k < 0 ? NULL : BUTTONS[k].word[iface == 1 ? 1 : 0];
}

/* the index JS gives byte `at` of the UTF-8 text `s`: its UTF-16 code units before it, one per
   character and two for a character past U+FFFF (a 4-byte sequence) */
static long utf16_at(const char* s, const char* at)
{
    long n = 0;
    for (; s < at; s++) {
        const unsigned char b = (unsigned char)*s;
        if ((b & 0xC0) != 0x80) n += b >= 0xF0 ? 2 : 1;
    }
    return n;
}

/* text.js's checkText: a token is a "{" and everything up to the next "}"; its name is a cast of
   `casts` (bit c: a unit of the entry has cast c) or a button. Refused: a "{" with no "}" after
   it, a name neither cast nor button (a capitalised cast and "{}" among them: a display name
   carries its own capitals), and a cast no unit of the entry has. A "}" alone is text. 1, or 0
   with why in `why`. */
static int text_check(const char* s, unsigned casts, char* why, size_t n)
{
    const char *a, *b;
    for (a = s; s && (a = strchr(a, '{')); a = b + 1) {
        size_t len;
        int c;
        if (!(b = strchr(a + 1, '}'))) {
            snprintf(why, n, "the \"{\" at %ld starts no token", utf16_at(s, a));
            return 0;
        }
        len = (size_t)(b - a - 1);
        for (c = 0; c < WIKI_NCAST && !(strlen(CAST_NAMES[c]) == len && !strncmp(CAST_NAMES[c], a + 1, len)); c++) {}
        if (c < WIKI_NCAST) {
            if ((casts >> c) & 1) continue;
            snprintf(why, n, "{%.*s}: no unit of the entry has that cast", (int)len, a + 1);
            return 0;
        }
        if (button_of(a + 1, len) < 0) {
            snprintf(why, n, "{%.*s} is not a token", (int)len, a + 1);
            return 0;
        }
    }
    return 1;
}

/* one shown text through text_check, refusing the entry with where it stands; a text that is not
   a string is never shown, so it holds no token */
static int text_ok(R* r, const TJV* v, unsigned casts, const char* where)
{
    char why[160], msg[256];
    if (!tj_is(v, TJ_STR) || text_check(v->str, casts, why, sizeof why)) return 1;
    snprintf(msg, sizeof msg, "%s: %s", where, why);
    return refuse(r, msg, NULL);
}

/* THE CAPTION'S BOUND, text.js's captionWorst: the most bytes caption `s` can fill to, whatever the
   casting and the interface type -- its own bytes with a button token at its longer word and a
   cast token at the longer of WIKI_TITLE_MAX - 1 bytes, past which tagpu_wikidata_casting keeps
   every casting's names, and the entry's own name for that cast, which an unbound cast reads
   (tagpu_wikitext_words). Only for a text text_check accepted: every token is a cast or a
   button. */
static size_t caption_worst(const char* s, const WIKIENTRY* e)
{
    const char *a = s, *b, *z;
    size_t n = 0;
    while ((b = strchr(a, '{')) != NULL && (z = strchr(b + 1, '}')) != NULL) {
        const size_t len = (size_t)(z - b - 1);
        const int k = button_of(b + 1, len);
        size_t word = 0;
        int c, i;
        if (k >= 0) {
            const size_t w0 = strlen(BUTTONS[k].word[0]), w1 = strlen(BUTTONS[k].word[1]);
            word = w0 > w1 ? w0 : w1;
        } else {
            word = WIKI_TITLE_MAX - 1;
            for (c = 0; c < WIKI_NCAST && !(strlen(CAST_NAMES[c]) == len && !strncmp(CAST_NAMES[c], b + 1, len)); c++) {}
            for (i = 0; c < WIKI_NCAST && i < e->ndef; i++)
                if (e->def[i].cast == c) {
                    const char* own = e->def[i].title && e->def[i].title[0] ? e->def[i].title : e->def[i].name;
                    if (strlen(own) > word) word = strlen(own);
                    break;
                }
        }
        n += (size_t)(b - a) + word;
        a = z + 1;
    }
    return n + strlen(a);
}

/* a caption that passed text_check through the caption's bound, refusing the entry with where it
   stands */
static int caption_ok(R* r, const TJV* v, const WIKIENTRY* e, const char* where)
{
    char msg[256];
    size_t n;
    if (!tj_is(v, TJ_STR) || (n = caption_worst(v->str, e)) < WIKI_CAPTION_MAX) return 1;
    snprintf(msg, sizeof msg, "%s: it can fill to %lu bytes, more than the %d a caption holds", where,
             (unsigned long)n, WIKI_CAPTION_MAX - 1);
    return refuse(r, msg, NULL);
}

/* text.js's checkEntry, in its order: the title, the subtitle, each paragraph, then each demo's
   title and its inputs' captions, each caption through the caption's bound after its tokens; the
   first refusal is the one reported */
static int tokens(R* r, const WIKIENTRY* e, const TJV* root)
{
    const TJV *c, *d, *in;
    char where[96], num[32];
    unsigned casts = 0;
    int i;
    for (i = 0; i < e->ndef; i++)
        if (e->def[i].cast >= 0) casts |= 1u << e->def[i].cast;
    if (!text_ok(r, tj_get(root, "title"), casts, "title") || !text_ok(r, tj_get(root, "subtitle"), casts, "subtitle"))
        return 0;
    for (c = tj_is(tj_get(root, "text"), TJ_ARR) ? tj_get(root, "text")->first : NULL, i = 0; c; c = c->next, i++) {
        snprintf(where, sizeof where, "text[%d]", i);
        if (!text_ok(r, c, casts, where)) return 0;
    }
    for (d = tj_is(tj_get(root, "demos"), TJ_ARR) ? tj_get(root, "demos")->first : NULL, i = 0; d; d = d->next, i++) {
        snprintf(where, sizeof where, "demos[%d].title", i);
        if (!text_ok(r, tj_get(d, "title"), casts, where)) return 0;
        for (in = tj_is(tj_get(d, "inputs"), TJ_ARR) ? tj_get(d, "inputs")->first : NULL; in; in = in->next) {
            const TJV* at = tj_get(in, "at");
            snprintf(where, sizeof where, "demos[%d] caption at %s", i,
                     tj_is(at, TJ_NUM) ? jsnum(num, sizeof num, at->num) : "undefined");
            if (!text_ok(r, tj_get(in, "caption"), casts, where) || !caption_ok(r, tj_get(in, "caption"), e, where))
                return 0;
        }
    }
    return 1;
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
    if (!r.failed) tokens(&r, e, root);
    if (r.failed) {
        tagpu_wikidata_entry_free(e);
        return 0;
    }
    return 1;
}

/* ---- the data rules of a demo's script ---------------------------------------------------- */

/* the simulation's order: by tick, then file order */
static int script_order(const void* a, const void* b)
{
    const WIKIINPUT* x = *(const WIKIINPUT* const*)a;
    const WIKIINPUT* y = *(const WIKIINPUT* const*)b;
    return x->at < y->at ? -1 : x->at > y->at ? 1 : x < y ? -1 : x > y;
}

/* the demo's inputs in the simulation's order, an array the caller frees; NULL with a message
   in `err` when memory runs out */
static const WIKIINPUT** script(const WIKIDEMO* d, char* err, size_t errn)
{
    const WIKIINPUT** in = (const WIKIINPUT**)malloc(((size_t)d->ninput + 1) * sizeof *in);
    int i;
    if (!in) {
        if (err && errn) snprintf(err, errn, "wiki demo %s: out of memory", d->id ? d->id : "?");
        return NULL;
    }
    for (i = 0; i < d->ninput; i++) in[i] = &d->input[i];
    qsort((void*)in, (size_t)d->ninput, sizeof *in, script_order);
    return in;
}

int tagpu_wikidata_buttons(const WIKIDEMO* d, int iface, char* err, size_t errn)
{
    const WIKIINPUT** in = script(d, err, errn);
    int i, k, held[2] = { 0, 0 }, ok = 1;
    if (!in) return 0;
    for (i = 0; ok && i < d->ninput; i++) {
        /* the press first, then the release: sim.js reads `down` before `up` in one input */
        const int key[2] = { tagpu_wikidata_button(in[i]->down, iface), tagpu_wikidata_button(in[i]->up, iface) };
        for (k = 0; ok && k < 2; k++) {
            const int down = k == 0, b = key[k] == WIKI_KEY_LMB ? 0 : key[k] == WIKI_KEY_RMB ? 1 : -1;
            char num[32];
            if (b < 0) continue;
            if (down == held[b]) {
                if (err && errn)
                    snprintf(err, errn, "demo %s: at tick %s the %s of %s %s", d->id ? d->id : "?",
                             jsnum(num, sizeof num, in[i]->at), down ? "press" : "release", KEY_NAMES[key[k]],
                             down ? "while it is held" : "while it is up");
                ok = 0;
            }
            held[b] = down;
        }
    }
    free((void*)in);
    return ok;
}

static int trip_no(char* err, size_t errn, const WIKIDEMO* d, const WIKIINPUT* e, const char* what)
{
    char num[32];
    if (err && errn)
        snprintf(err, errn, "wiki demo %s: the build at tick %s %s", d->id ? d->id : "?", jsnum(num, sizeof num, e->at), what);
    return 0;
}

int tagpu_wikidata_trips(const WIKIDEMO* d, int iface, char* err, size_t errn)
{
    const double end = d->has_end ? d->end : INFINITY;
    const WIKIINPUT** in = script(d, err, errn);
    int i, j, lmb = 0, ok = 1;
    if (!in) return 0;
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
        if (tagpu_wikidata_button(e->down, iface) == WIKI_KEY_LMB) lmb = 1;
        if (tagpu_wikidata_button(e->up, iface) == WIKI_KEY_LMB) lmb = 0;
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
    /* WIKI_SIDE_TYPES: WIKIPAGES' table, one row per (type, side) over every side */
    if (L->ntype < 0 || L->ntype > WIKI_SIDE_TYPES) return lay_no(err, errn, "more than 64 types over the sides", NULL);
    for (i = 0; i < L->ntype; i++) {
        const WIKIPAGES* t = &L->type[i];
        if (!terminated(t->name)) return lay_no(err, errn, "a type's name is not terminated", NULL);
        if (t->side < 0 || t->side >= L->nside) return lay_no(err, errn, "a type is in no side:", t->name);
        /* one row per (type, side): the simulation finds a unit's pages by its type's name in the
           side it plays */
        for (j = 0; j < i; j++)
            if (L->type[j].side == t->side && !strcmp(L->type[j].name, t->name))
                return lay_no(err, errn, "a type is named twice in one side:", t->name);
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
    int i, n = 0;
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
        if (!tj_is(tj_get(c, "types"), TJ_OBJ)) return lay_no(err, errn, "a side's types is not an object:", d->name);
        n += tj_get(c, "types")->n;
    }
    /* WIKI_SIDE_TYPES: counted over every side before one row is written */
    if (n > WIKI_SIDE_TYPES) return lay_no(err, errn, "more than 64 types over the sides", NULL);
    for (c = sides->first, i = 0; c; c = c->next, i++)
        for (g = tj_get(c, "types")->first; g; g = g->next) {
            WIKIPAGES* t = &L->type[L->ntype++];
            const TJV *pages = tj_get(g, "pages"), *orders = tj_get(g, "orders"), *p;
            if (strlen(g->key) >= WIKI_SIDE_NAME) return lay_no(err, errn, "a type's name is too long:", g->key);
            strcpy(t->name, g->key);
            t->side = i;
            if (!tj_is(g, TJ_OBJ) || !tj_is(pages, TJ_ARR) || pages->n > WIKI_SIDE_PAGES)
                return lay_no(err, errn, "a type has no list of at most 7 page screens:", g->key);
            for (p = pages->first; p; p = p->next)
                if ((t->page[t->npage++] = lay_screen(L, p)) < 0) return lay_no(err, errn, "a page is not a screen for", g->key);
            if (tj_is(orders, TJ_NULL)) t->orders = -1;
            else if ((t->orders = lay_screen(L, orders)) < 0) return lay_no(err, errn, "orders is not a screen or null for", g->key);
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

/* ---- a side's casting: sim.js's checkCasting, which is its specification ------------------- */

static int cast_no(char* err, size_t errn, WIKICASTING* c, const char* what)
{
    if (err && errn) snprintf(err, errn, "the casting is refused: %s", what);
    memset(c, 0, sizeof *c);
    return 0;
}

/* sim.js's FIGURES, in its order, and where each lands */
static double* figure(WIKICASTDEF* k, int i)
{
    double* const at[6] = { &k->build_time, &k->worker_time, &k->build_distance, &k->max_velocity, &k->extracts,
                            &k->energy_make };
    return at[i];
}
static const char* const FIGURES[6] = { "buildTime", "workerTime", "buildDistance", "maxVelocity", "extracts", "energyMake" };

/* one bound cast's record into `k`, 1; else 0 with why in `why` */
static int cast_def(const TJV* v, const char* at, WIKICASTDEF* k, char* why, size_t n)
{
    const TJV *t = tj_get(v, "type"), *f = tj_get(v, "footprint"), *cs = tj_get(v, "canstop"), *vt = tj_get(v, "vent");
    const TJV *aw = tj_get(v, "activatewhenbuilt");
    const TJV *b = tj_get(v, "builds"), *x;
    size_t len;
    int i;
    k->bound = 1;
    /* THE BOUND: 1..WIKI_ID_MAX - 1 bytes fit `type` with its terminator. Not all digits: a JS
       object lists integer keys first, so sim.js's bound entry would leave definition order */
    if (!tj_is(v, TJ_OBJ) || !tj_is(t, TJ_STR) || !(len = strlen(t->str)) || len >= WIKI_ID_MAX
        || strspn(t->str, "0123456789") == len) {
        snprintf(why, n, "%s has no type name of 1 to %d bytes, not all digits", at, WIKI_ID_MAX - 1);
        return 0;
    }
    memcpy(k->type, t->str, len + 1);
    /* WIKI_TITLE_MAX: the title and its terminator fit `title` */
    if (!tj_is(tj_get(v, "title"), TJ_STR) || (len = strlen(tj_get(v, "title")->str)) >= WIKI_TITLE_MAX) {
        snprintf(why, n, "%s has no title of at most %d bytes", at, WIKI_TITLE_MAX - 1);
        return 0;
    }
    memcpy(k->title, tj_get(v, "title")->str, len + 1);
    /* WIKI_FP_MAX: the G rows' width */
    if (!tj_is(f, TJ_ARR) || f->n != 2 || !lay_int(tj_at(f, 0), 1, WIKI_FP_MAX, &k->fx) || !lay_int(tj_at(f, 1), 1, WIKI_FP_MAX, &k->fz)) {
        snprintf(why, n, "%s: the footprint is not two integers 1..%d", at, WIKI_FP_MAX);
        return 0;
    }
    /* the figures divide and step: a build time of 0 would fill a building in no time at all,
       and a negative or infinite speed or reach would leave the world's arithmetic */
    for (i = 0; i < 6; i++) {
        if (!(x = tj_get(v, FIGURES[i]))) continue;
        if (!tj_is(x, TJ_NUM) || !(x->num >= 0 && x->num < INFINITY)) {
            snprintf(why, n, "%s: %s is not a finite number >= 0", at, FIGURES[i]);
            return 0;
        }
        *figure(k, i) = x->num;
    }
    if (!(k->build_time > 0)) { snprintf(why, n, "%s: buildTime is not above 0", at); return 0; }
    if (cs && (!tj_is(cs, TJ_NUM) || cs->num != floor(cs->num) || !isfinite(cs->num))) {
        snprintf(why, n, "%s: canstop is not an integer", at);
        return 0;
    }
    /* sim.js's (canstop & 1) === 1: JS's & takes the integer modulo 2^32, which keeps its parity */
    k->canstop = cs && fmod(fabs(cs->num), 2.0) == 1.0;
    if (aw && (!tj_is(aw, TJ_NUM) || aw->num != floor(aw->num) || !isfinite(aw->num))) {
        snprintf(why, n, "%s: activatewhenbuilt is not an integer", at);
        return 0;
    }
    k->activate_when_built = aw && fmod(fabs(aw->num), 2.0) == 1.0;
    if (vt && !tj_is(vt, TJ_TRUE) && !tj_is(vt, TJ_FALSE)) { snprintf(why, n, "%s: vent is not a boolean", at); return 0; }
    k->vent = tj_is(vt, TJ_TRUE);
    /* fx and fz are within WIKI_FP_MAX above, the bound g_rows rests on */
    if (!g_rows(tj_get(v, "g"), k->vent, k->fx, k->fz, k->g)) {
        snprintf(why, n, "%s: g is not a list of G cells inside the footprint, held by vent exactly when it is not empty", at);
        return 0;
    }
    if (b) {
        int ok = tj_is(b, TJ_ARR);
        for (x = ok ? b->first : NULL; x; x = x->next) {
            const int c = tj_is(x, TJ_STR) ? cast_of(x->str) : -1;
            if (c < 0) { ok = 0; break; }
            k->builds |= (uint16_t)(1u << c);
        }
        if (!ok) { snprintf(why, n, "%s: builds is not a list of casts", at); return 0; }
    }
    return 1;
}

int tagpu_wikidata_casting(const TJV* v, WIKICASTING* c, char* err, size_t errn)
{
    const TJV *casts = tj_get(v, "casts"), *m, *o;
    char why[200], at[32];
    int i;
    memset(c, 0, sizeof *c);
    if (!tj_is(v, TJ_OBJ) || !tj_is(casts, TJ_OBJ)) return cast_no(err, errn, c, "casts is not an object");
    for (m = casts->first; m; m = m->next) {
        if (cast_of(m->key) < 0) {
            snprintf(why, sizeof why, "%s is not a cast", m->key);
            return cast_no(err, errn, c, why);
        }
        for (o = casts->first; o != m; o = o->next)
            if (!strcmp(o->key, m->key)) {
                snprintf(why, sizeof why, "%s is named twice", m->key);
                return cast_no(err, errn, c, why);
            }
    }
    for (i = 0; i < WIKI_NCAST; i++) {
        const TJV* k = tj_get(casts, CAST_NAMES[i]);
        if (!k) {
            snprintf(why, sizeof why, "%s is absent (null is the unbound cast)", CAST_NAMES[i]);
            return cast_no(err, errn, c, why);
        }
        if (tj_is(k, TJ_NULL)) continue;
        snprintf(at, sizeof at, "cast %s", CAST_NAMES[i]);
        if (!cast_def(k, at, &c->cast[i], why, sizeof why)) return cast_no(err, errn, c, why);
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
