/* tagpu_wikidata.c -- the in-game wiki's content, read. Contract: tagpu_wikidata.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_pak.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikisim.h"           /* WIKI_TRIP, the build trip's ticks */
#include "tagpu_wikireplay.h"        /* WIKI_REPLAY_HUD_DOWN, _UP: a HUD press's trip */

#define FOOTPRINT_MAX 64    /* tagpu_placegeo.c's bound on a footprint, which its arithmetic rests on */
#define TREE_MAX      1024  /* nodes of the tree */

/* sim.js's KEYS then ACTIONS, in WIKI_KEY_* and WIKI_ACT_* order */
static const char* const KEY_NAMES[WIKI_NKEYNAMES] = { "shift", "alt", "ctrl", "z", "x", "e", "lmb", "rmb",
                                                       "select", "order", "cancel" };
/* sim.js's KINDS, in WIKI_KIND_* order */
static const char* const KIND_NAMES[WIKI_NKINDS] = { "metal", "energy", "all" };
/* sim.js's and text.js's CASTS, in WIKI_CAST_* order */
static const char* const CAST_NAMES[WIKI_NCAST] = { "builder", "advbuilder", "mex", "moho", "geo",
                                                    "walker", "tower", "wall", "solar", "commander" };
/* sim.js's HUD_CONTROLS, in WIKI_HUD_* order */
static const char* const HUD_NAMES[WIKI_NHUD] = { "play", "speed", "rew", "fwd", "track", "bookmark", "leave", "leave:no",
                                                   "rewind", "live_play", "live_now", "takeover", "takeas", "lanes", "lane",
                                                   "main", "export", "delete" };

const char* tagpu_wikidata_key_name(int key)
{
    return key >= 0 && key < WIKI_NKEYNAMES ? KEY_NAMES[key] : NULL;
}

const char* tagpu_wikidata_cast_name(int cast)
{
    return cast >= 0 && cast < WIKI_NCAST ? CAST_NAMES[cast] : NULL;
}

const char* tagpu_wikidata_kind_name(int kind)
{
    return kind >= 0 && kind < WIKI_NKINDS ? KIND_NAMES[kind] : NULL;
}

const char* tagpu_wikidata_hud_name(int hud)
{
    return hud >= 0 && hud < WIKI_NHUD ? HUD_NAMES[hud] : NULL;
}

int tagpu_wikidata_hud(const char* name)
{
    int k;
    for (k = 0; name && k < WIKI_NHUD; k++)
        if (!strcmp(HUD_NAMES[k], name)) return k;
    return -1;
}

int tagpu_wikidata_hud_presses(int hud, int live)
{
    if (live)
        return hud == WIKI_HUD_REWIND || hud == WIKI_HUD_REW || hud == WIKI_HUD_FWD || hud == WIKI_HUD_TRACK
            || hud == WIKI_HUD_LIVE_PLAY || hud == WIKI_HUD_LIVE_NOW;
    return hud >= WIKI_HUD_PLAY && hud <= WIKI_HUD_LEAVE_NO;
}

int tagpu_wikidata_hud_takeover(int hud)
{
    return hud >= WIKI_HUD_TAKEOVER && hud <= WIKI_HUD_DELETE;
}

int tagpu_wikidata_refresh_at(int first, int T)
{
    unsigned nth = 1, prev = 0;
    int t, R0 = first;
    for (t = first; t <= T; t++, nth++)
        if (reclossy_is_refresh(nth, (unsigned)t, prev, REC_REFRESH_TICKS)) {
            R0 = t;
            prev = (unsigned)t;
        }
    return R0;
}

/* a sweep kind by name, or -1 */
static int kind_of(const char* s)
{
    int k;
    for (k = 0; s && k < WIKI_NKINDS; k++)
        if (!strcmp(KIND_NAMES[k], s)) return k;
    return -1;
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
   quotes (ticks, small integers and their fractions). %g turns to an exponent once the decimal
   exponent reaches the precision (1000 at one digit is "1e+03"), where JS writes the digits out for
   every exponent from -6 to 20, so such a value is written again in fixed notation with the same
   significant digits. */
static const char* jsnum(char* out, size_t n, double v)
{
    const char* e;
    int p, x;
    for (p = 1; p < 17; p++) {
        snprintf(out, n, "%.*g", p, v);
        if (strtod(out, NULL) == v) break;
    }
    if (p == 17) snprintf(out, n, "%.17g", v);
    if ((e = strchr(out, 'e')) != NULL && (x = (int)strtol(e + 1, NULL, 10)) >= -6 && x <= 20)
        snprintf(out, n, "%.*f", p - 1 - x > 0 ? p - 1 - x : 0, v);
    return out;
}

int tagpu_wikidata_def(const WIKIENTRY* e, const char* name)
{
    int i;
    for (i = 0; i < e->ndef; i++)
        if (!strcmp(e->def[i].name, name)) return i;
    return -1;
}

int tagpu_wikidata_feat(const WIKIENTRY* e, const char* name)
{
    int i;
    for (i = 0; name && i < e->nfeat; i++)
        if (!strcmp(e->feat[i].name, name)) return i;
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

/* An FBI flag (canstop, canreclamate, activatewhenbuilt) as sim.js reads it, `(v & 1) === 1`: a number whose
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

/* AN ECONOMY FIGURE of an entry's unit, sim.js checkGame's rule: absent 0; else a number, finite and
   at least 0, or for `signed_` (energyUse) of magnitude below WIKI_ECON_MAX */
static int econ_figure(R* r, const TJV* u, const char* key, int signed_, double* out, const char* type)
{
    const TJV* v = tj_get(u, key);
    *out = 0;
    if (!v) return 1;
    if (!tj_is(v, TJ_NUM) || !(signed_ ? fabs(v->num) < WIKI_ECON_MAX : v->num >= 0 && v->num < INFINITY)) {
        char why[WIKI_ID_MAX + 64];
        snprintf(why, sizeof why, "unit %s: %s is not a finite number %s", type, key,
                 signed_ ? "of magnitude below 1e7" : ">= 0");
        return refuse(r, why, NULL);
    }
    *out = v->num;
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
        if (!econ_figure(r, u, "buildCostMetal", 0, &d->build_cost_metal, d->name)
            || !econ_figure(r, u, "buildCostEnergy", 0, &d->build_cost_energy, d->name)
            || !econ_figure(r, u, "metalMake", 0, &d->metal_make, d->name)
            || !econ_figure(r, u, "metalStorage", 0, &d->metal_storage, d->name)
            || !econ_figure(r, u, "energyStorage", 0, &d->energy_storage, d->name)
            || !econ_figure(r, u, "energyUse", 1, &d->energy_use, d->name))
            return 0;
        /* `rival` and `corpse` are a game entry's (game_defs reads the corpse once the features are);
           the frame is read before the units */
        d->corpse = -1;
        if ((tj_get(u, "rival") || tj_get(u, "corpse")) && e->frame != WIKI_FRAME_GAME)
            return refuse(r, "rival and corpse are a game entry's: unit", d->name);
        if (tj_get(u, "rival") && !tj_is(tj_get(u, "rival"), TJ_TRUE) && !tj_is(tj_get(u, "rival"), TJ_FALSE))
            return refuse(r, "rival is not a boolean: unit", d->name);
        d->rival = tj_is(tj_get(u, "rival"), TJ_TRUE);
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
        if (!fbi_flag(r, tj_get(u, "canreclamate"), "canreclamate", &d->canreclamate)) return 0;
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

/* THE ENTRY'S FEATURES, sim.js's checkFeature over each row of `features`, an object (absent: none)
   of at most WIKI_FEAT_DEFS rows; an absent figure reads 0 and an absent flag false. The `from`
   key and any other the schema does not name are read past. */
static int features(R* r, WIKIENTRY* e, const TJV* v)
{
    const TJV* f;
    int i = 0;
    if (!v) return !!(e->feat = (WIKIFEATDEF*)zalloc(r, 0, sizeof *e->feat));
    if (!tj_is(v, TJ_OBJ)) return refuse(r, "features is not an object", NULL);
    if (v->n > WIKI_FEAT_DEFS) return refuse(r, "has more than 64 features", NULL);
    if (!(e->feat = (WIKIFEATDEF*)zalloc(r, (size_t)(e->nfeat = v->n), sizeof *e->feat))) return 0;
    for (f = v->first; f; f = f->next, i++) {
        WIKIFEATDEF* d = &e->feat[i];
        const TJV *fp = tj_get(f, "footprint"), *k;
        static const char* const AMOUNT[2] = { "metal", "energy" };
        static const char* const FLAG[2] = { "blocking", "reclaimable" };
        int j;
        /* THE BOUND: the name and its terminator fit WIKIFEATDEF.name */
        if (!*f->key || strlen(f->key) >= sizeof d->name) return refuse(r, "a feature's name is not 1 to 31 bytes:", f->key);
        for (j = 0; j < i; j++)
            if (!strcmp(e->feat[j].name, f->key)) return refuse(r, "names a feature twice:", f->key);
        strcpy(d->name, f->key);
        if (!tj_is(f, TJ_OBJ) || !tj_is(fp, TJ_ARR) || fp->n != 2
            || !integer(r, tj_at(fp, 0), 1, WIKI_FEAT_FOOT, &d->fx, "a feature's footprint")
            || !integer(r, tj_at(fp, 1), 1, WIKI_FEAT_FOOT, &d->fz, "a feature's footprint"))
            return refuse(r, "a feature's footprint is not two integers 1..16:", d->name);
        for (j = 0; j < 2; j++) {
            double* out = j ? &d->energy : &d->metal;
            if (!(k = tj_get(f, AMOUNT[j]))) continue;
            if (!tj_is(k, TJ_NUM) || !(k->num >= 0 && k->num < WIKI_FEAT_AMOUNT))
                return refuse(r, "a feature's metal or energy is not 0..1e7:", d->name);
            *out = k->num;
        }
        if ((k = tj_get(f, "height")) && !integer(r, k, 0, 255, &d->height, "a feature's height")) return 0;
        for (j = 0; j < 2; j++) {
            int* out = j ? &d->reclaimable : &d->blocking;
            if (!(k = tj_get(f, FLAG[j]))) continue;
            if (!tj_is(k, TJ_TRUE) && !tj_is(k, TJ_FALSE)) return refuse(r, "a feature's flag is not a boolean:", d->name);
            *out = tj_is(k, TJ_TRUE);
        }
        /* THE BOUND: the model's name and its terminator fit WIKIFEATDEF.object */
        if ((k = tj_get(f, "object")) && (!tj_is(k, TJ_STR) || !*k->str || strlen(k->str) >= sizeof d->object))
            return refuse(r, "a feature's object is not 1 to 31 bytes:", d->name);
        if (k) strcpy(d->object, k->str);
    }
    return 1;
}

/* a scene's feature: its table row by name and its anchor cell; sim.js refuses one off the map, over
   another or with an id already taken as it lays the scene out, which tagpu_wikisim_init does too */
static int scene_feat(R* r, const TJV* v, WIKISCENEFEAT* f)
{
    const TJV *at = tj_get(v, "at"), *id = tj_get(v, "id");
    if (!tj_is(v, TJ_OBJ) || (f->def = tagpu_wikidata_feat(r->e, tj_str(tj_get(v, "name"), NULL))) < 0)
        return refuse(r, "a scene's feature names no feature of the entry", NULL);
    if (!tj_is(at, TJ_ARR) || at->n != 2 || !integer(r, tj_at(at, 0), -1000000, 1000000, &f->ox, "a feature's anchor")
        || !integer(r, tj_at(at, 1), -1000000, 1000000, &f->oz, "a feature's anchor"))
        return refuse(r, "a scene's feature has no anchor cell", NULL);
    /* sim.js: `f.id ?? f${k + 1}`, a string of 1..23 bytes; the sim names an absent one */
    if (id && !tj_is(id, TJ_NULL)) {
        if (!tj_is(id, TJ_STR) || strlen(id->str) >= sizeof f->id) return refuse(r, "a feature's id is not a name of at most 23 bytes", NULL);
        strcpy(f->id, id->str);
        if (!f->id[0]) return refuse(r, "a feature's id is empty", NULL);
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

/* A GAME ENTRY'S SCENE'S SEATS, sim.js checkSeats: 1..WIKI_SEATS objects of name, metal and energy
   and nothing else. THE BOUND: the name and its terminator fit WIKISEAT.name. */
static int seats(R* r, const TJV* v, WIKISCENE* s)
{
    const TJV *c, *k;
    int i = 0;
    if (!tj_is(v, TJ_ARR) || v->n < 1 || v->n > WIKI_SEATS) return refuse(r, "a game scene's seats are not a list of 1 to 4", NULL);
    s->nseat = v->n;
    for (c = v->first; c; c = c->next, i++) {
        WIKISEAT* z = &s->seat[i];
        const char* name = tj_str(tj_get(c, "name"), NULL);
        if (!tj_is(c, TJ_OBJ)) return refuse(r, "a seat is not an object", NULL);
        for (k = c->first; k; k = k->next)
            if (strcmp(k->key, "name") && strcmp(k->key, "metal") && strcmp(k->key, "energy"))
                return refuse(r, "a seat holds a key it does not know:", k->key);
        if (!name || !*name || strlen(name) >= sizeof z->name || strchr(name, '{'))
            return refuse(r, "a seat's name is not 1 to 31 bytes without \"{\"", NULL);
        strcpy(z->name, name);
        if (!integer(r, tj_get(c, "metal"), WIKI_SEAT_MIN, WIKI_SEAT_MAX, &z->metal, "a seat's metal")
            || !integer(r, tj_get(c, "energy"), WIKI_SEAT_MIN, WIKI_SEAT_MAX, &z->energy, "a seat's energy"))
            return 0;
    }
    return 1;
}

/* one of the entry's scenes (`entry` set: its map counts) or a demo's additions */
static int scene(R* r, const TJV* v, WIKISCENE* s, int entry)
{
    const TJV* c;
    int i;
    /* seats are a game entry's scene's, and every one of its scenes has them */
    if ((c = tj_get(v, "seats")) && !entry) return refuse(r, "a demo's scene has no seats", NULL);
    if (entry && r->e->frame == WIKI_FRAME_GAME) {
        if (!seats(r, c, s)) return 0;
    } else if (c) return refuse(r, "seats are a game entry's", NULL);
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
    s->nfeat = count(tj_get(v, "features"));
    if (!(s->feat = (WIKISCENEFEAT*)zalloc(r, (size_t)s->nfeat, sizeof *s->feat))
        || !(s->metal = (WIKIMETAL*)zalloc(r, (size_t)s->nmetal, sizeof *s->metal))
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
    /* a game scene's unit is one of its seats' (a demo's additions: demo() holds them to its scene's) */
    for (i = 0; s->nseat && i < s->nunit; i++)
        if (s->unit[i].owner >= s->nseat) return refuse(r, "a unit's owner is no seat of its scene", NULL);
    for (c = tj_is(tj_get(v, "features"), TJ_ARR) ? tj_get(v, "features")->first : NULL, i = 0; c; c = c->next, i++)
        if (!scene_feat(r, c, &s->feat[i])) return 0;
    /* `kind` replaces the entry's when given, the mockup's `??`; sim.js refuses one not of KINDS */
    c = tj_get(v, "kind");
    if ((s->has_kind = c && !tj_is(c, TJ_NULL)) && (s->kind = kind_of(tj_str(c, NULL))) < 0)
        return refuse(r, "a scene's reclaim kind is not one of metal, energy, all", NULL);
    /* `selected` and `cursor` replace the entry's when given: the mockup's `??`, so null is absent */
    c = tj_get(v, "selected");
    if ((s->has_selected = c && !tj_is(c, TJ_NULL)) && !strings(r, c, &s->selected, &s->nselected)) return 0;
    c = tj_get(v, "cursor");
    if ((s->has_cursor = c && !tj_is(c, TJ_NULL)) && !point(r, c, &s->cursor, "the cursor")) return 0;
    if (!s->selected && !(s->selected = (const char**)zalloc(r, 0, sizeof *s->selected))) return 0;
    return 1;
}

/* sim.js SCENE_NAME: 1 to 16 of a-z and 0-9, a letter first */
static int scene_name(const char* s)
{
    size_t i;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return 0;
    for (i = 1; s[i]; i++)
        if (i >= WIKI_SCENE_MAX - 1 || !((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9'))) return 0;
    return 1;
}

/* THE ENTRY'S SCENES, sim.js sceneOf: an object of one or more named scenes in the file's order,
   every map the first one's size. A name given twice is refused: JSON.parse would keep the last
   and this reader the first, so the two would play different scenes. */
static int scenes(R* r, WIKIENTRY* e, const TJV* v)
{
    const TJV *c, *k;
    int i = 0;
    if (!tj_is(v, TJ_OBJ) || !v->n) return refuse(r, "has no scenes", NULL);
    if (!(e->scenes = (WIKISCENE*)zalloc(r, (size_t)(e->nscene = v->n), sizeof *e->scenes))) return 0;
    for (c = v->first; c; c = c->next, i++) {
        if (!scene_name(c->key)) return refuse(r, "a scene's name is not 1 to 16 of a-z and 0-9, a letter first", c->key);
        for (k = v->first; k != c; k = k->next)
            if (!strcmp(k->key, c->key)) return refuse(r, "names a scene twice", c->key);
        e->scenes[i].name = c->key;
        if (!scene(r, c, &e->scenes[i], 1)) return 0;
        if (e->scenes[i].w != e->scenes[0].w || e->scenes[i].h != e->scenes[0].h)
            return refuse(r, "a scene's map is not the first scene's size", c->key);
    }
    return 1;
}

/* an input of a demo's script, or with `game` of a game script (sim.js checkGame): there it has no
   caption and no wait, its `seat` is an integer 0..WIKI_SEATS - 1 (THE SEAT RULE, seat_ok, bounds it
   by the scene's seats and the sidebar), and a kill is {at, kill} alone, `extra` as any input but a
   caption is, so no trip admits it */
static int input(R* r, const TJV* v, WIKIINPUT* in, int game)
{
    const TJV* c;
    memset(in, 0, sizeof *in);
    in->build = -1;
    in->at = tj_num(tj_get(v, "at"), 0);
    for (c = tj_is(v, TJ_OBJ) ? v->first : NULL; c; c = c->next)
        if (strcmp(c->key, "at") && strcmp(c->key, "caption") && strcmp(c->key, "build") && strcmp(c->key, "reclaim")
            && strcmp(c->key, "pick") && !(game && !strcmp(c->key, "seat")))
            in->extra = 1;
    if (game) {
        if (tj_get(v, "caption") || tj_get(v, "until")) return refuse(r, "a game input is a caption or a wait", NULL);
        if ((c = tj_get(v, "seat"))) {
            if (!tj_is(c, TJ_NUM) || c->num != floor(c->num) || !(c->num >= 0 && c->num < WIKI_SEATS)) {
                char why[96], num[32];
                snprintf(why, sizeof why, "seat %s is no seat of its scene", tj_is(c, TJ_NUM) ? jsnum(num, sizeof num, c->num) : "?");
                return refuse(r, why, NULL);
            }
            in->seat = (int)c->num;
        }
        if ((c = tj_get(v, "kill"))) {
            const TJV* k;
            for (k = v->first; k; k = k->next)
                if (strcmp(k->key, "at") && strcmp(k->key, "kill")) return refuse(r, "a kill carries another key:", k->key);
            if (!tj_is(c, TJ_STR)) return refuse(r, "a kill names no unit", NULL);
            in->has |= WIKI_IN_KILL;
            in->kill = c->str;
            return 1;
        }
    }
    if (tj_get(v, "caption")) {
        in->has |= WIKI_IN_CAPTION;
        in->caption = text_or_null(tj_get(v, "caption"));
    }
    if ((c = tj_get(v, "build"))) {
        in->has |= WIKI_IN_BUILD;
        if (text_or_null(c) && !type_of(r, c, &in->build)) return 0;
    }
    /* sim.js's data rule of a reclaim and a pick, refused whatever plays the demo: `reclaim` is
       true, `pick` one of KINDS */
    if ((c = tj_get(v, "reclaim"))) {
        if (!tj_is(c, TJ_TRUE)) return refuse(r, "a reclaim is not true", NULL);
        in->has |= WIKI_IN_RECLAIM;
    }
    if ((c = tj_get(v, "pick"))) {
        if ((in->pick = kind_of(tj_str(c, NULL))) < 0) return refuse(r, "a pick names no kind", NULL);
        in->has |= WIKI_IN_PICK;
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
        if (!tj_is(c, TJ_STR) || (strcmp(c->str, "idle") && strcmp(c->str, "arrived") && strcmp(c->str, "built")))
            return refuse(r, "a wait names no state the sim knows", NULL);
        if (!m || !tj_is(m, TJ_NUM) || m->num != floor(m->num) || !(m->num >= 1 && m->num <= WIKI_UNTIL_MAX))
            return refuse(r, "a wait has no max of 1 to 3600 ticks", NULL);
        for (k = v->first; k; k = k->next)
            if (strcmp(k->key, "at") && strcmp(k->key, "until") && strcmp(k->key, "max"))
                return refuse(r, "a wait carries another input", NULL);
        in->has |= WIKI_IN_UNTIL;
        in->until = !strcmp(c->str, "idle") ? WIKI_UNTIL_IDLE : !strcmp(c->str, "arrived") ? WIKI_UNTIL_ARRIVED : WIKI_UNTIL_BUILT;
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
    return input(&r, v, in, 0);
}

int tagpu_wikidata_game_input(const WIKIENTRY* e, const TJV* v, WIKIINPUT* in, char* err, size_t errn)
{
    R r;
    memset(&r, 0, sizeof r);
    r.e = e;
    r.err = err;
    r.errn = errn;
    return input(&r, v, in, 1);
}

/* whether unit id `id` is one of the merged scene's explicit ids: the entry scene's the demo is on
   and the demo's additions */
static int scene_has_id(const WIKIENTRY* e, const WIKIDEMO* d, const char* id)
{
    const WIKISCENE* sc[2] = { &e->scenes[d->on], &d->scene };
    int k, i;
    for (k = 0; k < 2; k++)
        for (i = 0; i < sc[k]->nunit; i++)
            if (sc[k]->unit[i].id[0] && !strcmp(sc[k]->unit[i].id, id)) return 1;
    return 0;
}

/* THE SEAT RULE (sim.js checkGame): a game input's seat is one of its scene's, and 0 unless the
   game's sidebar is false, since the column is seat 0's and a game whose sidebar may be on gives no
   other seat an input (an over-approximation: an absent sidebar is on when the scene gives seat 0 a
   builder and the layout has a column; tagpu_wikisim_init refuses a column beside another seat's
   input as well). `d`'s game is read up to its inputs. */
static int seat_ok(R* r, const WIKIINPUT* in, const WIKIDEMO* d)
{
    char why[160];
    if (in->seat >= r->e->scenes[d->on].nseat) {
        snprintf(why, sizeof why, "seat %d is no seat of its scene", in->seat);
        return refuse(r, why, NULL);
    }
    if (in->seat != 0 && !(d->game.has_sidebar && !d->game.sidebar)) {
        snprintf(why, sizeof why, "seat %d: the column is seat 0's, so a game whose sidebar is not false gives no other seat an input",
                 in->seat);
        return refuse(r, why, NULL);
    }
    return 1;
}

/* THE LIVE DATA RULE (sim.js checkLiveOrders): in a live entry a seat's release
   (tagpu_wikisim_release) can come between any two ticks of a game script and of a fork's part,
   and it empties that seat's selection an order is given from and disarms its reclaim. So each
   build naming a type and each reclaim has, on its own tick and before it in the order the inputs
   are fed (by tick, then file order; within one input its move, then its press, then its release,
   as the sim's input() reads them), the click of its own seat that selects its builder or
   reclaimer: a move aiming that seat's cursor, then select pressed, then select released. Another
   seat's click selects nothing of this seat's, and a release lets go of one seat alone, so the rule
   holds seat by seat. The release runs only between two ticks, so it cannot fall between that click
   and the order; the move is on the tick too because a trip the release cuts off leaves the cursor
   wherever it was. Whether the click lands on a builder or a
   reclaimer is the world's, which simcheck.mjs's release sweep plays out.
   A live entry has no pick: the popup it clicks stands only while reclaim is armed, and with the
   sidebar's trips a reclaim arms only when its trip clicks RECLAIM, ticks after the reclaim's own,
   while tagpu_wikidata_trips keeps every other input out of the reclaim's tick after it and out of
   its WIKI_TRIP ticks; so a pick is always on a later tick than its reclaim, and a release between
   the two closes the popup the pick needs (pick_trip then stops the demo). Whether the sidebar is
   on is the layout's, which the loader is not given, so a pick is refused whatever. `part` is the
   fork part's id, 0 for the game script. */
static int live_orders(R* r, const WIKIINPUT* in, int n, int part)
{
    int i, j;
    for (i = 0; i < n; i++) {
        /* step: 1 the cursor aimed, 2 then select pressed, 3 then released, over the inputs fed
           before the order */
        int step = 0;
        char why[256], num[32], where[48];
        const char* what = (in[i].has & WIKI_IN_BUILD) && in[i].build >= 0 ? "build"
                         : (in[i].has & WIKI_IN_RECLAIM) ? "reclaim" : NULL;
        where[0] = 0;
        if (part) snprintf(where, sizeof where, "the fork part %d: ", part);
        if (in[i].has & WIKI_IN_PICK) {
            snprintf(why, sizeof why, "%sthe pick at tick %s: a live entry has no pick, since the popup it clicks opens on a "
                     "later tick than its reclaim and a release between them closes it", where, jsnum(num, sizeof num, in[i].at));
            return refuse(r, why, NULL);
        }
        if (!what) continue;
        for (j = 0; j < i; j++) {
            if (in[j].at != in[i].at || in[j].seat != in[i].seat) continue;
            if ((in[j].has & WIKI_IN_MOVE) && step < 3) step = 1;
            if (in[j].down == WIKI_ACT_SELECT && step == 1) step = 2;
            if (in[j].up == WIKI_ACT_SELECT && step == 2) step = 3;
        }
        if (step == 3) continue;
        snprintf(why, sizeof why, "%sthe %s at tick %s has no click selecting its %s on its own tick "
                 "(a move, then select pressed and released), which a live entry's %s needs", where, what,
                 jsnum(num, sizeof num, in[i].at), what[0] == 'b' ? "builder" : "reclaimer", what);
        return refuse(r, why, NULL);
    }
    return 1;
}

/* A GAME ENTRY'S DEMO'S GAME, sim.js checkGame: {end, inputs, sidebar} and no other key; end an
   integer 1..WIKI_GAME_END_MAX, sidebar a boolean; each input a game input (input) of a seat THE
   SEAT RULE admits (seat_ok), each kill of a unit of the merged scene by its explicit id, once, at
   most WIKI_GAME_KILLS, at or before the end;
   on a live entry, each build and reclaim after the click selecting its builder or reclaimer, and
   no pick (THE LIVE DATA RULE, live_orders). THE BOUND the game simulation's features rest on: no
   feature of the merged scene takes a unit's id (its id, or f<k + 1> when it has none, sim.js's
   naming), since a unit's wreck takes the unit's, and the merged scene's features and the kills
   together are at most WIKI_FEATS_MAX. */
static int game(R* r, const TJV* v, WIKIDEMO* d)
{
    const TJV *c, *k, *inputs = tj_get(v, "inputs");
    const WIKIENTRY* e = r->e;
    const WIKISCENE* sc[2] = { &e->scenes[d->on], &d->scene };
    WIKIGAME* g = &d->game;
    int i = 0, j, n, q, kills = 0;
    if (!tj_is(v, TJ_OBJ)) return refuse(r, "a game entry's demo has no game", NULL);
    for (k = v->first; k; k = k->next)
        if (strcmp(k->key, "end") && strcmp(k->key, "inputs") && strcmp(k->key, "sidebar"))
            return refuse(r, "game holds a key it does not know:", k->key);
    if (!tj_is(tj_get(v, "end"), TJ_NUM)) return refuse(r, "the game's end is not an integer 1..5400", NULL);
    g->end = tj_get(v, "end")->num;
    if (!(g->end >= 1 && g->end <= WIKI_GAME_END_MAX) || g->end != floor(g->end))
        return refuse(r, "the game's end is not an integer 1..5400", NULL);
    if ((c = tj_get(v, "sidebar"))) {
        if (!tj_is(c, TJ_TRUE) && !tj_is(c, TJ_FALSE)) return refuse(r, "the game's sidebar is not a boolean", NULL);
        g->has_sidebar = 1;
        g->sidebar = tj_is(c, TJ_TRUE);
    }
    /* a takeover may take any seat, and the column is seat 0's: a takeover entry's game has none,
       the sidebar false (an absent sidebar may be on, THE SEAT RULE's over-approximation) */
    if (e->takeover && !(g->has_sidebar && !g->sidebar))
        return refuse(r, "a takeover entry's game has the sidebar false, since the column is seat 0's", NULL);
    if (!tj_is(inputs, TJ_ARR)) return refuse(r, "the game's inputs are not a list", NULL);
    for (q = 0, n = 0; q < 2; q++)
        for (j = 0; j < sc[q]->nfeat; j++, n++) {
            char dflt[WIKI_ID_MAX];
            const char* id = sc[q]->feat[j].id[0] ? sc[q]->feat[j].id : dflt;
            snprintf(dflt, sizeof dflt, "f%d", n + 1);
            if (scene_has_id(e, d, id)) return refuse(r, "a feature has a unit's id:", id);
        }
    g->ninput = count(inputs);
    if (!(g->input = (WIKIINPUT*)zalloc(r, (size_t)g->ninput, sizeof *g->input))) return 0;
    for (c = inputs->first; c; c = c->next) {
        WIKIINPUT* in = &g->input[i++];
        if (!tj_is(tj_get(c, "at"), TJ_NUM)) return refuse(r, "a game input has no tick", NULL);
        if (!input(r, c, in, 1) || !seat_ok(r, in, d)) return 0;
        if (!(in->has & WIKI_IN_KILL)) continue;
        if (!scene_has_id(e, d, in->kill)) return refuse(r, "a kill names no unit of the scene:", in->kill);
        for (j = 0; j < i - 1; j++)
            if ((g->input[j].has & WIKI_IN_KILL) && !strcmp(g->input[j].kill, in->kill))
                return refuse(r, "a unit is killed twice:", in->kill);
        if (kills == WIKI_GAME_KILLS) return refuse(r, "a game script has more than 64 kills", NULL);
        if (!(in->at <= g->end)) return refuse(r, "a kill is after the game's end", NULL);
        kills++;
    }
    if (e->live && !live_orders(r, g->input, g->ninput, 0)) return 0;
    if (n + kills > WIKI_FEATS_MAX) return refuse(r, "the scene's features and the kills pass 256", NULL);
    return 1;
}

/* whether every key of `v` but `at` is one of the `n` in `keys` */
static int keys_in(const TJV* v, const char* const* keys, int n)
{
    const TJV* k;
    int i;
    for (k = v->first; k; k = k->next) {
        for (i = 0; i < n && strcmp(k->key, keys[i]); i++) {}
        if (strcmp(k->key, "at") && i == n) return 0;
    }
    return 1;
}

/* AN INPUT OF A GAME ENTRY'S VIEWER'S SCRIPT (sim.js checkViewer, which tagpu_wikireplay plays):
   exactly one of a caption {at, caption}; the wait {at, until: "seeked", max}, max an integer
   1..WIKI_UNTIL_MAX; or a press {at, hud} of a WIKI_HUD_* control the entry's kind presses (a
   replay entry's, a live entry's, a takeover entry's all three sets), for the track with `to` an
   integer 0..end and `drag` (the same) and `over` (an integer 1..WIKI_UNTIL_MAX) together or
   neither, for a picker row with `seat` an integer 0..nseat - 1, for a lane, its Save and its
   Delete with `fork` an integer 1..RECR_TIMELINES - 1. leave:yes is the player's, never a
   script's. */
static int viewer_input(R* r, const TJV* v, WIKIINPUT* in, int end, int nseat)
{
    static const char* const CAPTION[] = { "caption" }, *const WAIT[] = { "until", "max" }, *const PRESS[] = { "hud" },
                             *const TRACK[] = { "hud", "to", "drag", "over" }, *const ROW[] = { "hud", "seat" },
                             *const LANE[] = { "hud", "fork" };
    const TJV* c;
    memset(in, 0, sizeof *in);
    in->build = in->down = in->up = in->hud = in->drag = in->fork = -1;
    in->at = tj_num(tj_get(v, "at"), 0);
    if ((c = tj_get(v, "hud"))) {
        int ok;
        if (tj_is(c, TJ_STR) && !strcmp(c->str, "leave:yes")) return refuse(r, "leave:yes is the player's, never a demo's", NULL);
        if (!tj_is(c, TJ_STR) || (in->hud = tagpu_wikidata_hud(c->str)) < 0)
            return refuse(r, "a press names no control a demo presses", NULL);
        ok = r->e->takeover ? tagpu_wikidata_hud_presses(in->hud, 0) || tagpu_wikidata_hud_presses(in->hud, 1)
                                  || tagpu_wikidata_hud_takeover(in->hud)
                            : tagpu_wikidata_hud_presses(in->hud, r->e->live);
        if (!ok)
            return refuse(r, r->e->live ? "a press names no control a live demo presses:" : "a press names no control a replay demo presses:", c->str);
        in->has |= WIKI_IN_HUD;
        if (in->hud == WIKI_HUD_TAKEAS) {
            if (!keys_in(v, ROW, 2)) return refuse(r, "a press carries a key it does not know", NULL);
            return integer(r, tj_get(v, "seat"), 0, nseat - 1, &in->seat, "the row's seat");
        }
        if (in->hud == WIKI_HUD_LANE || in->hud == WIKI_HUD_EXPORT || in->hud == WIKI_HUD_DELETE) {
            if (!keys_in(v, LANE, 2)) return refuse(r, "a press carries a key it does not know", NULL);
            return integer(r, tj_get(v, "fork"), 1, RECR_TIMELINES - 1, &in->fork, "the press's fork");
        }
        if (in->hud != WIKI_HUD_TRACK)
            return keys_in(v, PRESS, 1) ? 1 : refuse(r, "a press carries a key it does not know", NULL);
        if (!keys_in(v, TRACK, 4)) return refuse(r, "a press carries a key it does not know", NULL);
        if (!integer(r, tj_get(v, "to"), 0, end, &in->to, "the track's to")) return 0;
        if (!tj_get(v, "drag") != !tj_get(v, "over")) return refuse(r, "a track's drag and over come together", NULL);
        if (tj_get(v, "drag")
            && (!integer(r, tj_get(v, "drag"), 0, end, &in->drag, "the track's drag")
                || !integer(r, tj_get(v, "over"), 1, WIKI_UNTIL_MAX, &in->over, "the track's over")))
            return 0;
        return 1;
    }
    if ((c = tj_get(v, "until"))) {
        if (!tj_is(c, TJ_STR) || strcmp(c->str, "seeked")) return refuse(r, "the viewer's wait is \"seeked\"", NULL);
        if (!integer(r, tj_get(v, "max"), 1, WIKI_UNTIL_MAX, &in->until_max, "a wait's max")) return 0;
        if (!keys_in(v, WAIT, 2)) return refuse(r, "a wait carries another input", NULL);
        in->has |= WIKI_IN_UNTIL;
        in->until = WIKI_UNTIL_SEEKED;
        return 1;
    }
    if (!tj_get(v, "caption") || !keys_in(v, CAPTION, 1)) return refuse(r, "a viewer's input is no caption, wait or press", NULL);
    in->has |= WIKI_IN_CAPTION;
    in->caption = text_or_null(tj_get(v, "caption"));
    return 1;
}

/* THE VIEWER'S SCRIPT'S ORDER RULE (sim.js checkViewer): in the order it is fed, by tick then file
   order, no press and no wait stands in a press's trip, from its tick to its release (at +
   WIKI_REPLAY_HUD_UP, or for a drag at + WIKI_REPLAY_HUD_DOWN + over), the release's tick
   included; and no other input shares a wait's tick */
static int viewer_order(R* r, const WIKIDEMO* d)
{
    double release = -INFINITY;
    int i, k, j;
    for (i = 0; i < d->ninput; i++) {
        if (!(d->input[i].has & WIKI_IN_UNTIL)) continue;
        for (k = 0; k < d->ninput; k++)
            if (k != i && d->input[k].at == d->input[i].at) return refuse(r, "the viewer's wait shares its tick with another input", NULL);
    }
    /* the inputs by tick, then file order, without a copy: the next one is the least (at, index)
       above the last one taken */
    for (i = -1, j = 0; j < d->ninput; j++) {
        const WIKIINPUT* e;
        int best = -1;
        for (k = 0; k < d->ninput; k++) {
            const int after = i < 0 || d->input[k].at > d->input[i].at || (d->input[k].at == d->input[i].at && k > i);
            if (after && (best < 0 || d->input[k].at < d->input[best].at)) best = k;
        }
        e = &d->input[i = best];
        if (!(e->has & (WIKI_IN_HUD | WIKI_IN_UNTIL))) continue;
        if (e->at <= release) return refuse(r, "a press or a wait stands in another press's trip", NULL);
        if (e->has & WIKI_IN_HUD)
            release = e->at + (e->drag >= 0 ? WIKI_REPLAY_HUD_DOWN + e->over : WIKI_REPLAY_HUD_UP);
    }
    return 1;
}

/* A FORK'S GAME INPUTS, of a part or a kept fork (sim.js checkGame forkInputs): each a game input
   (input) of a seat THE SEAT RULE admits (seat_ok) at a tick from..last, a kill of a unit of the
   merged scene by its explicit id, once in the fork, at most WIKI_GAME_KILLS; into `f`, its kills
   added to `*kills`. `what` names the fork in a refusal. */
static int fork_inputs(R* r, const TJV* inputs, WIKIFORK* f, const WIKIDEMO* d, int last, const char* what, int* kills)
{
    const TJV* c;
    int j, partkills = 0;
    char why[96];
    if (!tj_is(inputs, TJ_ARR)) return refuse(r, "a fork's inputs are not a list:", what);
    if (!(f->input = (WIKIINPUT*)zalloc(r, (size_t)inputs->n, sizeof *f->input))) return 0;
    for (c = inputs->first; c; c = c->next) {
        WIKIINPUT* in = &f->input[f->ninput++];
        if (!tj_is(tj_get(c, "at"), TJ_NUM)) return refuse(r, "a game input has no tick", NULL);
        if (!input(r, c, in, 1) || !seat_ok(r, in, d)) return 0;
        if (!(in->at >= f->from && in->at <= last)) return refuse(r, "a fork's input is outside its ticks:", what);
        if (!(in->has & WIKI_IN_KILL)) continue;
        if (!scene_has_id(r->e, d, in->kill)) return refuse(r, "a kill names no unit of the scene:", in->kill);
        for (j = 0; j < f->ninput - 1; j++)
            if ((f->input[j].has & WIKI_IN_KILL) && !strcmp(f->input[j].kill, in->kill))
                return refuse(r, "a unit is killed twice in a fork:", in->kill);
        if (partkills == WIKI_GAME_KILLS) {
            snprintf(why, sizeof why, "%s has more than %d kills", what, WIKI_GAME_KILLS);
            return refuse(r, why, NULL);
        }
        partkills++;
    }
    *kills += partkills;
    return 1;
}

/* A LIVE OR TAKEOVER DEMO'S FORK PARTS (sim.js checkGame, WIKIFORK's rules): a list of at most
   WIKI_DEMO_FORKS parts, each {id, from, parent, inputs} and an optional seat, the seat it is
   played as, which THE SEAT RULE admits (seat_ok); its inputs as fork_inputs reads them at ticks
   from..end; each build and reclaim after the click selecting its builder or reclaimer, and no
   pick (THE LIVE DATA RULE, live_orders). */
static int forks(R* r, const TJV* v, WIKIDEMO* d, int* kills)
{
    static const char* const PART[] = { "id", "from", "parent", "inputs", "seat" };
    const TJV* p;
    int i;
    if (!tj_is(v, TJ_ARR) || v->n > WIKI_DEMO_FORKS) return refuse(r, "forks is not a list of at most 8 parts", NULL);
    if (!(d->fork = (WIKIFORK*)zalloc(r, (size_t)v->n, sizeof *d->fork))) return 0;
    for (p = v->first; p; p = p->next) {
        WIKIFORK* f = &d->fork[d->nfork++];
        WIKIINPUT seat;
        f->end = -1;
        if (!tj_is(p, TJ_OBJ) || !keys_in(p, PART, 5) || !tj_get(p, "id") || !tj_get(p, "from") || !tj_get(p, "parent")
            || !tj_get(p, "inputs"))
            return refuse(r, "a fork part is not {id, from, parent, inputs} with an optional seat", NULL);
        if (!integer(r, tj_get(p, "id"), 1, RECR_TIMELINES - 1, &f->id, "a fork part's id")
            || !integer(r, tj_get(p, "from"), 2, (int)d->game.end, &f->from, "a fork part's from")
            || !integer(r, tj_get(p, "parent"), 0, f->id - 1, &f->parent, "a fork part's parent")
            || (tj_get(p, "seat") && !integer(r, tj_get(p, "seat"), 0, WIKI_SEATS - 1, &f->seat, "a fork part's seat")))
            return 0;
        memset(&seat, 0, sizeof seat);
        seat.seat = f->seat;
        if (!seat_ok(r, &seat, d)) return 0;
        for (i = 0; i < d->nfork - 1; i++)
            if (d->fork[i].id == f->id) return refuse(r, "two fork parts have one id", NULL);
        if (!fork_inputs(r, tj_get(p, "inputs"), f, d, (int)d->game.end, "a fork part", kills)) return 0;
        if (!live_orders(r, f->input, f->ninput, f->id)) return 0;
    }
    return 1;
}

/* A TAKEOVER DEMO'S KEPT FORKS (sim.js checkGame, WIKIFORK's rules): a list of at most
   REC_FORK_LIMIT forks, each exactly {id, from, parent, seat, end, inputs}: id k + 1 at index k;
   parent 0..id - 1; from after the parent's first (the original's 1, a kept parent's from), at
   most its end (the original's the game's), and a refresh of its segment walked from that first
   (tagpu_wikidata_refresh_at), since a fork starts only there (tagpu_replay.c takeover_point); seat
   one of the scene's (seat_ok); end from..the game's end; its inputs as fork_inputs reads them at
   ticks from..end. */
static int kept(R* r, const TJV* v, WIKIDEMO* d, int* kills)
{
    static const char* const KEPT[] = { "id", "from", "parent", "seat", "end", "inputs" };
    const TJV* p;
    int k = 0;
    if (!tj_is(v, TJ_ARR) || v->n > REC_FORK_LIMIT) return refuse(r, "kept is not a list of at most 3 forks", NULL);
    if (!(d->kept = (WIKIFORK*)zalloc(r, (size_t)v->n, sizeof *d->kept))) return 0;
    for (p = v->first; p; p = p->next, k++) {
        WIKIFORK* f = &d->kept[d->nkept++];
        WIKIINPUT seat;
        int pR, pend;
        if (!tj_is(p, TJ_OBJ) || p->n != 6 || !keys_in(p, KEPT, 6) || !tj_get(p, "id") || !tj_get(p, "from") || !tj_get(p, "parent")
            || !tj_get(p, "seat") || !tj_get(p, "end") || !tj_get(p, "inputs"))
            return refuse(r, "a kept fork is not {id, from, parent, seat, end, inputs}", NULL);
        if (!integer(r, tj_get(p, "id"), k + 1, k + 1, &f->id, "a kept fork's id, its place in the list,")
            || !integer(r, tj_get(p, "parent"), 0, f->id - 1, &f->parent, "a kept fork's parent"))
            return 0;
        pR = f->parent ? d->kept[f->parent - 1].from : 1;
        pend = f->parent ? d->kept[f->parent - 1].end : (int)d->game.end;
        if (!integer(r, tj_get(p, "from"), pR + 1, pend, &f->from, "a kept fork's from, after its parent's first and up to its end,"))
            return 0;
        if (tagpu_wikidata_refresh_at(pR, f->from) != f->from)
            return refuse(r, "a kept fork's from is not a refresh of its parent's segment", NULL);
        if (!integer(r, tj_get(p, "seat"), 0, WIKI_SEATS - 1, &f->seat, "a kept fork's seat")) return 0;
        memset(&seat, 0, sizeof seat);
        seat.seat = f->seat;
        if (!seat_ok(r, &seat, d)) return 0;
        if (!integer(r, tj_get(p, "end"), f->from, (int)d->game.end, &f->end, "a kept fork's end")) return 0;
        if (!fork_inputs(r, tj_get(p, "inputs"), f, d, f->end, "a kept fork", kills)) return 0;
    }
    return 1;
}

static int demo(R* r, const TJV* v, WIKIDEMO* d)
{
    const TJV *c, *inputs = tj_get(v, "inputs"), *end = tj_get(v, "end");
    int i = 0, kills = 0;
    d->id = tj_str(tj_get(v, "id"), NULL);
    d->title = tj_str(tj_get(v, "title"), NULL);
    r->where = d->id ? d->id : "?";
    /* the scene it plays on, sim.js sceneOf: `on` names one of the entry's, the first when absent */
    if ((c = tj_get(v, "on"))) {
        if (!tj_is(c, TJ_STR)) return refuse(r, "a demo's on is not a scene's name", NULL);
        for (d->on = 0; d->on < r->e->nscene && strcmp(r->e->scenes[d->on].name, c->str); d->on++) {}
        if (d->on == r->e->nscene) return refuse(r, "a demo is on no scene of the entry", c->str);
    }
    if (!scene(r, tj_get(v, "scene"), &d->scene, 0)) return 0;
    /* a game entry's demo: its game first, whose end bounds the viewer's ticks */
    c = tj_get(v, "game");
    if (r->e->frame != WIKI_FRAME_GAME) {
        if (c) return refuse(r, "game is a game entry's", NULL);
    } else {
        for (i = 0; i < d->scene.nunit; i++)
            if (d->scene.unit[i].owner >= r->e->scenes[d->on].nseat) return refuse(r, "a unit's owner is no seat of its scene", NULL);
        if (!(d->has_game = game(r, c, d))) return 0;
    }
    /* `forks` is a live or a takeover entry's demo's alone, `kept` a takeover entry's. THE BOUND the
       game simulation's features rest on, over the forks too: the merged scene's features, the game
       script's kills, every part's and every kept fork's together are at most WIKI_FEATS_MAX
       (tagpu_wikisim_fork holds a fork to the same at run time). */
    if ((c = tj_get(v, "forks"))) {
        if (!r->e->live && !r->e->takeover) return refuse(r, "forks are a live or a takeover entry's demo's", NULL);
        if (!forks(r, c, d, &kills)) return 0;
    }
    if ((c = tj_get(v, "kept"))) {
        if (!r->e->takeover) return refuse(r, "kept is a takeover entry's demo's", NULL);
        if (!kept(r, c, d, &kills)) return 0;
    }
    if (kills) {
        const WIKISCENE* sc[2] = { &r->e->scenes[d->on], &d->scene };
        int n = sc[0]->nfeat + sc[1]->nfeat, k;
        for (k = 0; k < d->game.ninput; k++) kills += (d->game.input[k].has & WIKI_IN_KILL) != 0;
        if (n + kills > WIKI_FEATS_MAX) return refuse(r, "the scene's features and the kills pass 256", NULL);
    }
    d->ninput = count(inputs);
    if (!(d->input = (WIKIINPUT*)zalloc(r, (size_t)d->ninput, sizeof *d->input))) return 0;
    for (c = tj_is(inputs, TJ_ARR) ? inputs->first : NULL, i = 0; c; c = c->next)
        if (!tj_is(tj_get(c, "at"), TJ_NUM)) return refuse(r, "an input has no tick", NULL);
        else if (d->has_game ? !viewer_input(r, c, &d->input[i++], (int)d->game.end, r->e->scenes[d->on].nseat)
                             : !input(r, c, &d->input[i++], 0)) return 0;
    if (d->has_game && !viewer_order(r, d)) return 0;
    if ((d->has_end = end && !tj_is(end, TJ_NULL)) && !number(r, end, &d->end, "the end")) return 0;
    /* sim.js throws on a `sidebar` that is not a boolean when it plays the demo, and every demo is
       played, so the loader refuses it whatever reads the demo */
    if ((c = tj_get(v, "sidebar"))) {
        if (r->e->frame == WIKI_FRAME_GAME) return refuse(r, "a game entry's demo takes its sidebar in game", NULL);
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
    free(s->feat);
    free(s->metal);
    free(s->vent);
    free(s->fog);
    free(s->unit);
    free((void*)s->selected);
}

void tagpu_wikidata_entry_free(WIKIENTRY* e)
{
    int i, k;
    for (i = 0; e->demo && i < e->ndemo; i++) {
        scene_free(&e->demo[i].scene);
        free(e->demo[i].input);
        free(e->demo[i].game.input);
        for (k = 0; e->demo[i].fork && k < e->demo[i].nfork; k++) free(e->demo[i].fork[k].input);
        free(e->demo[i].fork);
        for (k = 0; e->demo[i].kept && k < e->demo[i].nkept; k++) free(e->demo[i].kept[k].input);
        free(e->demo[i].kept);
    }
    free(e->demo);
    for (i = 0; e->scenes && i < e->nscene; i++) scene_free(&e->scenes[i]);
    free(e->scenes);
    free(e->def);
    free(e->feat);
    free((void*)e->text);
    free((void*)e->locked_in);
    if (e->doc) tagpu_json_free(e->doc);
    memset(e, 0, sizeof *e);
}

/* a game entry's units' `corpse`: the row of the entry's features it names (sim.js checkGame) */
static int corpses(R* r, WIKIENTRY* e, const TJV* units)
{
    const TJV* u;
    int i = 0;
    for (u = units->first; u; u = u->next, i++) {
        const TJV* c = tj_get(u, "corpse");
        if (c && (!tj_is(c, TJ_STR) || (e->def[i].corpse = tagpu_wikidata_feat(e, c->str)) < 0))
            return refuse(r, "a corpse names no feature of the entry: unit", e->def[i].name);
    }
    return 1;
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
                if (e->def[i].cast == c && !e->def[i].rival) {
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
    /* a token names the player's casts only: a rival unit's cast is no word of the text */
    for (i = 0; i < e->ndef; i++)
        if (e->def[i].cast >= 0 && !e->def[i].rival) casts |= 1u << e->def[i].cast;
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
    /* sim.js isGame: `frame` absent, or "game"; a game entry has no pace */
    c = tj_get(root, "frame");
    e->frame = c && tj_is(c, TJ_STR) && !strcmp(c->str, "game") ? WIKI_FRAME_GAME : WIKI_FRAME_PAGE;
    /* sim.js checkGame: `live` and `takeover` a game entry's, booleans, never both true; the rule
       refuses anything else below */
    e->live = e->frame == WIKI_FRAME_GAME && tj_is(tj_get(root, "live"), TJ_TRUE);
    e->takeover = e->frame == WIKI_FRAME_GAME && tj_is(tj_get(root, "takeover"), TJ_TRUE);
    if (tj_num(tj_get(root, "format"), 0) != 2) refuse(&r, "is not format 2", NULL);
    else if (c && e->frame != WIKI_FRAME_GAME) refuse(&r, "has a frame other than \"game\"", NULL);
    else if (e->frame == WIKI_FRAME_GAME && tj_get(root, "pace")) refuse(&r, "is a game entry with a pace", NULL);
    else if ((c = tj_get(root, "live")) && (e->frame != WIKI_FRAME_GAME || !(tj_is(c, TJ_TRUE) || tj_is(c, TJ_FALSE))))
        refuse(&r, "has a live that is not a game entry's true or false", NULL);
    else if ((c = tj_get(root, "takeover")) && (e->frame != WIKI_FRAME_GAME || !(tj_is(c, TJ_TRUE) || tj_is(c, TJ_FALSE))))
        refuse(&r, "has a takeover that is not a game entry's true or false", NULL);
    else if (e->live && e->takeover) refuse(&r, "is a replay, a live game or a takeover's replay, never two", NULL);
    else if (e->frame == WIKI_FRAME_GAME && count(tj_get(root, "demos")) > WIKI_THEATRE_TABS)
        refuse(&r, "is a game entry with more demos than the theatre's tabs", NULL);
    else if (!strings(&r, tj_get(root, "text"), &e->text, &e->ntext)) {}
    else if (!strings(&r, tj_get(root, "lockedIn"), &e->locked_in, &e->nlocked)) {}
    else if (!defs(&r, e, tj_get(root, "units"))) {}
    else if (!features(&r, e, tj_get(root, "features"))) {}
    else if (!corpses(&r, e, tj_get(root, "units"))) {}
    else if (!scenes(&r, e, tj_get(root, "scenes"))) {}
    else if (e->frame == WIKI_FRAME_GAME && !tagpu_wikidata_frame_fits(e->scenes[0].w, e->scenes[0].h))
        refuse(&r, "is a game entry whose map makes a frame narrower than 4:3", NULL);
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

int tagpu_wikidata_frame_fits(int w, int h)
{
    const int Wg = WIKI_FRAME_SIDE_W + WIKI_CELL * w;
    const int Hg = WIKI_CELL * tagpu_wiki_map_rows(h, WIKI_SIDE_ROWS) + 2 * WIKI_FRAME_BAR;
    return 3 * Wg >= 4 * Hg;
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

int tagpu_wikidata_buttons(const WIKIDEMO* d, int iface, const int* held, char* err, size_t errn)
{
    const WIKIINPUT** in = script(d, err, errn);
    int i, k, hand[WIKI_SEATS][2], ok = 1;
    if (!in) return 0;
    for (i = 0; i < WIKI_SEATS; i++)
        for (k = 0; k < 2; k++) hand[i][k] = held ? held[2 * i + k] != 0 : 0;
    for (i = 0; ok && i < d->ninput; i++) {
        /* the press first, then the release: sim.js reads `down` before `up` in one input. THE
           BOUND: a seat is the reader's, 0..WIKI_SEATS - 1, and one outside it is no hand */
        const int key[2] = { tagpu_wikidata_button(in[i]->down, iface), tagpu_wikidata_button(in[i]->up, iface) };
        const int z = in[i]->seat;
        if (z < 0 || z >= WIKI_SEATS) continue;
        for (k = 0; ok && k < 2; k++) {
            const int down = k == 0, b = key[k] == WIKI_KEY_LMB ? 0 : key[k] == WIKI_KEY_RMB ? 1 : -1;
            char num[32], by[16];
            if (b < 0) continue;
            if (down == hand[z][b]) {
                by[0] = 0;
                if (z) snprintf(by, sizeof by, " by seat %d", z);
                if (err && errn)
                    snprintf(err, errn, "demo %s: at tick %s the %s of %s%s %s", d->id ? d->id : "?",
                             jsnum(num, sizeof num, in[i]->at), down ? "press" : "release", KEY_NAMES[key[k]], by,
                             down ? "while it is held" : "while it is up");
                ok = 0;
            }
            hand[z][b] = down;
        }
    }
    free((void*)in);
    return ok;
}

#define TRIPS_IN (WIKI_IN_BUILD | WIKI_IN_RECLAIM | WIKI_IN_PICK)

/* the trip an input is, sim.js's `what`: the first of build, reclaim and pick it carries */
static const char* trip_what(const WIKIINPUT* e)
{
    return (e->has & WIKI_IN_BUILD) ? "build" : (e->has & WIKI_IN_RECLAIM) ? "reclaim" : "pick";
}

static int trip_no(char* err, size_t errn, const WIKIDEMO* d, const WIKIINPUT* e, const char* what)
{
    char num[32];
    if (err && errn)
        snprintf(err, errn, "wiki demo %s: the %s at tick %s %s", d->id ? d->id : "?", trip_what(e),
                 jsnum(num, sizeof num, e->at), what);
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
        if (e->has & TRIPS_IN) {
            const unsigned what = (e->has & WIKI_IN_BUILD) ? WIKI_IN_BUILD : (e->has & WIKI_IN_RECLAIM) ? WIKI_IN_RECLAIM : WIKI_IN_PICK;
            const int span = what == WIKI_IN_PICK ? WIKI_PICK : WIKI_TRIP;
            /* the inputs after it in the simulation's order are those of its tick fed after it
               and those of later ticks, so the first one past its trip ends the search */
            for (j = i + 1; j < d->ninput && in[j]->at < e->at + span; j++)
                if ((in[j]->has & TRIPS_IN) || in[j]->extra) break;
            /* the game has no button that arms nothing: a demo disarms with a right-click; a reclaim
               and a pick were held to `true` and to a kind by the loader */
            if (what == WIKI_IN_BUILD && e->build < 0) ok = trip_no(err, errn, d, e, "names no type");
            else if (e->extra || (e->has & TRIPS_IN & ~what)) ok = trip_no(err, errn, d, e, "carries another input");
            /* the trip presses the left button itself */
            else if (lmb) ok = trip_no(err, errn, d, e, "starts with the left button held");
            else if (j < d->ninput && in[j]->at < e->at + span)
                ok = trip_no(err, errn, d, e, "has another input standing in its trip");
            else if (!(end >= e->at + span)) ok = trip_no(err, errn, d, e, "has the demo end inside its trip");
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
        /* within the column, and y + h <= WIKI_SIDE_ROWS, the deepest column; y >= 0 first, so
           WIKI_SIDE_ROWS - y cannot overflow */
        if (s->y < 0 || s->w < 1 || s->w > WIKI_SIDE_W || s->h < 1 || s->h > WIKI_SIDE_ROWS - s->y)
            return lay_no(err, errn, "a screen is not within the column:", s->name);
        if (s->y + s->h > rows) rows = s->y + s->h;
        /* WIKI_SIDE_GADGETS: a screen's gadget table, and the length of the view's states */
        if (s->ngadget < 0 || s->ngadget > WIKI_SIDE_GADGETS)
            return lay_no(err, errn, "more than 64 gadgets on", s->name);
        for (j = 0; j < s->ngadget; j++) {
            const WIKIGADGET* g = &s->gadget[j];
            if (!terminated(g->name) || !g->name[0] || g->role < 0 || g->role >= WIKI_NROLES || !terminated(g->unit)
                || (g->grey != 0 && g->grey != 1) || (g->hidden != 0 && g->hidden != 1) || g->key < 0 || g->key > 255)
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
    /* the quickkey, 1..255 when given, 0 for none */
    if (tj_get(v, "key") && !lay_int(tj_get(v, "key"), 1, 255, &g->key)) return 0;
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

int tagpu_wikidata_cast_economy(const WIKICASTDEF* k)
{
    return k->build_cost_metal >= 0 && k->build_cost_energy >= 0 && k->metal_make >= 0 && k->metal_storage >= 0 &&
           k->energy_storage >= 0 && fabs(k->energy_use) < WIKI_ECON_MAX;
}

/* sim.js's FIGURES, in its order, and where each lands: the NWORLD the world reads, then the
   economy's, which only a game reads (tagpu_wikidata_cast_economy) */
#define NFIGURES 11
#define NWORLD   6
static double* figure(WIKICASTDEF* k, int i)
{
    double* const at[NFIGURES] = { &k->build_time, &k->worker_time, &k->build_distance, &k->max_velocity, &k->extracts,
                                   &k->energy_make, &k->build_cost_metal, &k->build_cost_energy, &k->metal_make,
                                   &k->metal_storage, &k->energy_storage };
    return at[i];
}
static const char* const FIGURES[NFIGURES] = { "buildTime", "workerTime", "buildDistance", "maxVelocity", "extracts",
                                               "energyMake", "buildCostMetal", "buildCostEnergy", "metalMake",
                                               "metalStorage", "energyStorage" };

/* a cast's wreck into `d`, 1; else 0 with why in `why`: sim.js checkCasting's corpse rule, then
   checkFeature's over the record, in their order and with their messages. The name is bounded
   first, so a message that quotes it stays within the caller's 200 bytes. */
static int cast_corpse(const TJV* v, const char* at, WIKIFEATDEF* d, char* why, size_t n)
{
    static const char* const AMOUNT[2] = { "metal", "energy" };
    static const char* const FLAG[2] = { "blocking", "reclaimable" };
    const TJV *nm = tj_get(v, "name"), *fp = tj_get(v, "footprint"), *k;
    int j;
    /* THE BOUND: the name and its terminator fit WIKIFEATDEF.name */
    if (!tj_is(v, TJ_OBJ) || !tj_is(nm, TJ_STR) || !nm->str[0] || strlen(nm->str) >= sizeof d->name) {
        snprintf(why, n, "%s: corpse is not a feature record with a name of 1 to 31 bytes", at);
        return 0;
    }
    strcpy(d->name, nm->str);
    if (!tj_is(fp, TJ_ARR) || fp->n != 2 || !lay_int(tj_at(fp, 0), 1, WIKI_FEAT_FOOT, &d->fx)
        || !lay_int(tj_at(fp, 1), 1, WIKI_FEAT_FOOT, &d->fz)) {
        snprintf(why, n, "%s: corpse: feature %s: footprint is not two integers 1..%d", at, d->name, WIKI_FEAT_FOOT);
        return 0;
    }
    for (j = 0; j < 2; j++) {
        if (!(k = tj_get(v, AMOUNT[j]))) continue;
        if (!tj_is(k, TJ_NUM) || !(k->num >= 0 && k->num < WIKI_FEAT_AMOUNT)) {
            snprintf(why, n, "%s: corpse: feature %s: %s is not 0..%.0f", at, d->name, AMOUNT[j], WIKI_FEAT_AMOUNT);
            return 0;
        }
        *(j ? &d->energy : &d->metal) = k->num;
    }
    if ((k = tj_get(v, "height")) && !lay_int(k, 0, 255, &d->height)) {
        snprintf(why, n, "%s: corpse: feature %s: height is not 0..255", at, d->name);
        return 0;
    }
    for (j = 0; j < 2; j++) {
        if (!(k = tj_get(v, FLAG[j]))) continue;
        if (!tj_is(k, TJ_TRUE) && !tj_is(k, TJ_FALSE)) {
            snprintf(why, n, "%s: corpse: feature %s: %s is not a boolean", at, d->name, FLAG[j]);
            return 0;
        }
        *(j ? &d->reclaimable : &d->blocking) = tj_is(k, TJ_TRUE);
    }
    /* THE BOUND: the model's name and its terminator fit WIKIFEATDEF.object */
    if ((k = tj_get(v, "object")) != NULL) {
        if (!tj_is(k, TJ_STR) || !k->str[0] || strlen(k->str) >= sizeof d->object) {
            snprintf(why, n, "%s: corpse: feature %s: object is not 1 to 31 bytes", at, d->name);
            return 0;
        }
        strcpy(d->object, k->str);
    }
    return 1;
}

/* one bound cast's record into `k`, 1; else 0 with why in `why` */
static int cast_def(const TJV* v, const char* at, WIKICASTDEF* k, char* why, size_t n)
{
    const TJV *t = tj_get(v, "type"), *f = tj_get(v, "footprint"), *cs = tj_get(v, "canstop"), *vt = tj_get(v, "vent");
    const TJV *aw = tj_get(v, "activatewhenbuilt"), *cr = tj_get(v, "canreclamate");
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
    /* the world's figures divide and step: a build time of 0 would fill a building in no time at
       all, and a negative or infinite speed or reach would leave the world's arithmetic. The
       economy's need only be finite here: a game holds them to its bounds where it binds */
    for (i = 0; i < NFIGURES; i++) {
        if (!(x = tj_get(v, FIGURES[i]))) continue;
        if (!tj_is(x, TJ_NUM) || !isfinite(x->num) || (i < NWORLD && !(x->num >= 0))) {
            snprintf(why, n, i < NWORLD ? "%s: %s is not a finite number >= 0" : "%s: %s is not a finite number", at,
                     FIGURES[i]);
            return 0;
        }
        *figure(k, i) = x->num;
    }
    if ((x = tj_get(v, "energyUse")) != NULL) {
        if (!tj_is(x, TJ_NUM) || !isfinite(x->num)) {
            snprintf(why, n, "%s: energyUse is not a finite number", at);
            return 0;
        }
        k->energy_use = x->num;
    }
    if (!(k->build_time > 0)) { snprintf(why, n, "%s: buildTime is not above 0", at); return 0; }
    if (cs && (!tj_is(cs, TJ_NUM) || cs->num != floor(cs->num) || !isfinite(cs->num))) {
        snprintf(why, n, "%s: canstop is not an integer", at);
        return 0;
    }
    /* sim.js's (canstop & 1) === 1: JS's & takes the integer modulo 2^32, which keeps its parity */
    k->canstop = cs && fmod(fabs(cs->num), 2.0) == 1.0;
    if (cr && (!tj_is(cr, TJ_NUM) || cr->num != floor(cr->num) || !isfinite(cr->num))) {
        snprintf(why, n, "%s: canreclamate is not an integer", at);
        return 0;
    }
    k->canreclamate = cr && fmod(fabs(cr->num), 2.0) == 1.0;
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
    if ((x = tj_get(v, "corpse")) != NULL && !tj_is(x, TJ_NULL)) {
        if (!cast_corpse(x, at, &k->corpse, why, n)) return 0;
        k->has_corpse = 1;
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
