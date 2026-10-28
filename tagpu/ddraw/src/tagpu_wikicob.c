/* tagpu_wikicob.c -- the in-game wiki's COB host. The contract is tagpu_wikicob.h; the engine
   facts each rule rests on are cited beside it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tagpu_cob_safe.h"
#include "tagpu_cobvm.h"
#include "tagpu_wikicob.h"

#define OBJ_BYTES   ((TAGPU_COB_OBJECT_BYTES + 3u) & ~3u)
#define DEFERRED    8               /* starts a unit's SETs may queue within one VM call      */

typedef struct {
    unsigned char*   blob;          /* the program's own copy of the file: its lifetime        */
    TagpuCobProgram* prog;
    int              nodes;         /* the posed model's primitives                            */
    unsigned char    visible[WIKICOB_PIECES];
    int              quarantined;
    char             note[WIKICOB_NOTE];
} TYPE;

typedef struct {
    int            type, live;
    int32_t        value[WIKICOB_VALUES];
    TAGPU_COBVM    vm;
    unsigned char* block;           /* object, statics, animation, model in one allocation     */
    unsigned char* model;
    int            ndefer;
    int            defer[DEFERRED]; /* 1 Activate, 0 Deactivate, in SET order                  */
} UNIT;

struct WIKICOB {
    int                ntypes;
    TYPE               type[WIKICOB_TYPES];
    UNIT*              unit[WIKICOB_UNITS + 1];     /* by handle; [0] unused */
    int                nunit;
    uint32_t           rng;
    /* the call in progress: its unit and the facts its GETs read */
    int                cur;
    const WIKICOBFACT* facts;
    int                nfacts;
    TAGPU_COBVM_HOST   host;
    TagpuCobWorld      world;
};

static int is_gate(int id)
{
    return id == WIKICOB_ACTIVATION || id == WIKICOB_INBUILDSTANCE || id == WIKICOB_YARD_OPEN;
}

/* the ids the engine keeps as a bit or a 0/1 state, so a SET of any nonzero value reads back 1 */
static int is_flag(int id)
{
    return is_gate(id) || id == WIKICOB_BUSY || id == WIKICOB_BUGGER_OFF || id == WIKICOB_ARMORED;
}

static const WIKICOBFACT* fact(const WIKICOB* w, int32_t unit)
{
    int i;
    for (i = 0; i < w->nfacts; i++)
        if (w->facts[i].unit == unit) return &w->facts[i];
    return NULL;
}

static UNIT* unit_at(const WIKICOB* w, int u)
{
    return u >= 1 && u <= w->nunit ? w->unit[u] : NULL;
}

/* ---- the host the core calls ---- */

/* (x & 0xFFFF0000) + (z >> 16): a point as PIECE_XZ and UNIT_XZ pack it (0x480811, 0x480868),
   integer x in the high word, integer z in the low, added, so a negative z borrows from x */
static int32_t pack_xz(int32_t x, int32_t z)
{
    return (int32_t)(((uint32_t)x & 0xFFFF0000u) + (uint32_t)(z >> 16));
}

/* the point a packed word names, as XZ_ATAN, XZ_HYPOT and GROUND_HEIGHT unpack it (0x480965):
   x = a & 0xFFFF0000, z = a << 16, and x + 0x10000 when z is negative */
static void unpack_xz(int32_t a, double* x, double* z)
{
    int32_t zz = (int32_t)((uint32_t)a << 16);
    uint32_t xx = (uint32_t)a & 0xFFFF0000u;
    if (zz < 0) xx += 0x10000u;
    *x = (double)(int32_t)xx;
    *z = (double)zz;
}

/* round half to even, the x87 store 0x4B715A ends with */
static int32_t round_even(double v)
{
    double f = (double)(int64_t)v, d;
    if (f > v) f -= 1.0;                    /* floor */
    d = v - f;
    if (d > 0.5 || (d == 0.5 && (int64_t)f % 2 != 0)) f += 1.0;
    return (int32_t)(int64_t)f;
}

/* atan on [0, 1] from + - * / alone, so every compiler and C library gives the same double
   (the wiki's simulation rule, tagpu_wikisim.h): a reduction to |t| <= tan(pi/12) and the series
   to t^15, whose error there is below 1e-13 rad */
static double atan01(double t)
{
    const double r3 = 0.57735026918962576451;   /* tan(pi/6) */
    double off = 0.0, t2, s;
    if (t > 0.26794919243112270647) {           /* tan(pi/12) */
        t = (t - r3) / (1.0 + r3 * t);
        off = 0.52359877559829887308;           /* pi/6 */
    }
    t2 = t * t;
    s = t * (1.0 - t2 * (1.0 / 3 - t2 * (1.0 / 5 - t2 * (1.0 / 7 - t2 * (1.0 / 9 - t2 * (1.0 / 11 -
        t2 * (1.0 / 13 - t2 * (1.0 / 15))))))));
    return off + s;
}

/* 0x4B715A(x, z): round(atan2(x, z) * 65536 / 2pi), the engine's angle in 1/65536 turns, here
   from atan01 instead of the x87 fpatan, so it can differ from the engine's by one unit at a
   rounding boundary */
static int32_t ta_atan(double x, double z)
{
    const double pi = 3.14159265358979323846;
    double ax = x < 0 ? -x : x, az = z < 0 ? -z : z, a;
    if (ax == 0.0 && az == 0.0) return 0;
    a = ax <= az ? atan01(ax / az) : pi / 2 - atan01(az / ax);
    if (z < 0) a = pi - a;
    if (x < 0) a = -a;
    return round_even(a * (65536.0 / (2.0 * pi)));
}

/* hypot as sqrt of the sum of squares, truncated as _ftol truncates (0x480994, 0x4809E5) */
static int32_t trunc_hypot(double x, double z)
{
    double h = sqrt(x * x + z * z);
    return h >= 2147483647.0 ? 2147483647 : (int32_t)h;
}

/* GET_UNIT_VALUE (vt+0x44, 0x480770) for the ids tagpu_cob_getter leaves to the stock handler:
   the table at 0x480AC4, each id answered from what the wiki has (research/notes/
   exe-reverse-engineering.md, "get and set -- the twenty value ids"). The stage is flat at
   height 0 and the host keeps no piece transform and no definition box, so PIECE_Y, UNIT_Y,
   UNIT_HEIGHT and GROUND_HEIGHT answer 0, PIECE_XZ answers the unit's own point, and XZ_ATAN
   leaves out the unit's heading, which the facts do not carry. Any other id is 0 (0x480ABB). */
static int32_t host_get(void* ctx, int32_t id, int32_t a, int32_t b, int32_t c, int32_t d)
{
    WIKICOB* w = (WIKICOB*)ctx;
    const UNIT* u = unit_at(w, w->cur);
    const WIKICOBFACT* f = fact(w, w->cur);
    const WIKICOBFACT* t;
    double x, z;
    (void)c; (void)d;
    if (!u) return 0;
    switch (id) {
    case WIKICOB_ACTIVATION: case WIKICOB_STANDINGMOVEORDERS: case WIKICOB_STANDINGFIREORDERS:
    case WIKICOB_INBUILDSTANCE: case WIKICOB_BUSY: case WIKICOB_YARD_OPEN: case WIKICOB_BUGGER_OFF:
    case WIKICOB_ARMORED:
        return u->value[id];
    /* (i16 hp * 100) / MaxDamage, 0x4807CA: the wiki has no damage, so the facts' percent */
    case WIKICOB_HEALTH:             return f ? f->health : 100;
    case WIKICOB_PIECE_XZ:           return f ? pack_xz(f->x, f->z) : 0;
    /* a unit id in the low word; 0, or a unit not alive, answers 0 (0x480868..0x4808A4) */
    case WIKICOB_UNIT_XZ:
        if ((a & 0xFFFF) == 0 || !unit_at(w, a & 0xFFFF) || !unit_at(w, a & 0xFFFF)->live) return 0;
        t = fact(w, a & 0xFFFF);
        return t ? pack_xz(t->x, t->z) : 0;
    case WIKICOB_XZ_ATAN:            unpack_xz(a, &x, &z); return ta_atan(x, z) & 0xFFFF;
    case WIKICOB_XZ_HYPOT:           unpack_xz(a, &x, &z); return trunc_hypot(x, z);
    case WIKICOB_ATAN:               return ta_atan((double)a, (double)b) & 0xFFFF;
    case WIKICOB_HYPOT:              return trunc_hypot((double)a, (double)b);
    case WIKICOB_BUILD_PERCENT_LEFT: return tagpu_cob_build_left(f ? f->left : 0.0f);
    default:                         return 0;
    }
}

/* RAND's draw (0x4B6C30): 0 .. n-1, 0 for n < 2. The wiki's own generator, never the engine's
   (decision 11): xorshift32, its state in the host and in the hash. */
static int32_t host_rand(void* ctx, int32_t n, int32_t lo)
{
    WIKICOB* w = (WIKICOB*)ctx;
    uint32_t x = w->rng;
    (void)lo;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    w->rng = x;
    return n < 2 ? 0 : (int32_t)(x % (uint32_t)n);
}

static int32_t host_cargo(void* ctx, int32_t id) { (void)ctx; (void)id; return 0; }
static int32_t host_carrier(void* ctx) { (void)ctx; return 0; }

static int host_alive(void* ctx, int32_t id)
{
    const UNIT* u = unit_at((const WIKICOB*)ctx, id);
    return u && u->live;
}

/* A SET, at its opcode (tagpu_cobvm.h, the effect log): the unit's value. ACTIVATION goes through
   the state change 0x48B090 makes of it, whose start is deferred to the end of the VM call (the
   engine starts it inside the opcode, so a record after the current one runs it in the same
   pass; here it runs at the next slice). The other effects -- EMIT_SFX, EXPLODE, ATTACH, DROP --
   change nothing the wiki has. */
static void host_commit(void* ctx, const TAGPU_COBVM_EFFECT* e)
{
    WIKICOB* w = (WIKICOB*)ctx;
    UNIT* u = unit_at(w, w->cur);
    int32_t id, v;
    if (!u || e->op != 0x10082000u) return;
    id = e->v[0];
    v = e->v[1];
    if (id < 0 || id >= WIKICOB_VALUES) return;
    if (is_flag(id)) v = v != 0;
    if (id == WIKICOB_ACTIVATION && v != u->value[id] && u->ndefer < DEFERRED)
        u->defer[u->ndefer++] = v;
    u->value[id] = v;
}

/* tagpu_cob_getter's world: the host's units by handle, the caller the unit being run, owners
   allied only with themselves, the player (owner 0) the one local seat */
static int world_unit(void* ctx, int32_t id, TagpuCobUnitFacts* out)
{
    const WIKICOB* w = (const WIKICOB*)ctx;
    const UNIT* u = unit_at(w, id);
    const WIKICOBFACT* f = fact(w, id);
    if (!u || !u->live || !f) return 0;
    out->id = (uint32_t)id;
    out->owner = (uint32_t)f->owner;
    out->kills = 0;
    out->remaining = f->left;
    return 1;
}

static int world_caller(void* ctx, TagpuCobUnitFacts* out)
{
    const WIKICOB* w = (const WIKICOB*)ctx;
    return world_unit(ctx, w->cur, out);
}

static int world_allied(void* ctx, uint32_t own, uint32_t owner) { (void)ctx; return own == owner; }
static int world_local(void* ctx, uint32_t owner) { (void)ctx; return owner == 0; }

/* ---- the units ---- */

static void drop_vm(UNIT* u)
{
    free(u->block);
    u->block = NULL;
    u->model = NULL;
    u->ndefer = 0;
}

static void quarantine(WIKICOB* w, int t, const char* what, const char* why)
{
    TYPE* ty = &w->type[t];
    int k;
    if (!ty->quarantined)
        snprintf(ty->note, sizeof ty->note, "%s: %s", what, why && why[0] ? why : "refused");
    ty->quarantined = 1;
    for (k = 1; k <= w->nunit; k++)
        if (w->unit[k] && w->unit[k]->type == t) drop_vm(w->unit[k]);
}

static void enter(WIKICOB* w, int u, TAGPU_COBVM_RUN* run, const WIKICOBFACT* facts, int nfacts)
{
    memset(run, 0, sizeof *run);
    run->host = &w->host;
    w->cur = u;
    w->facts = facts;
    w->nfacts = facts ? nfacts : 0;
}

static void leave(WIKICOB* w)
{
    w->cur = 0;
    w->facts = NULL;
    w->nfacts = 0;
}

/* the starts the call's SETs deferred, each through 0x4B0940 with run-now 0 */
static int drain(WIKICOB* w, UNIT* u, TAGPU_COBVM_RUN* run)
{
    int k, n = u->ndefer;
    u->ndefer = 0;
    for (k = 0; k < n && u->block; k++) {
        int s = tagpu_cobvm_index(w->type[u->type].prog, u->defer[k] ? "Activate" : "Deactivate");
        if (s >= 0 && tagpu_cobvm_start(&u->vm, run, s, 0, 0, 0, 0, NULL) < 0) return -1;
    }
    return 1;
}

/* the result of one VM call on unit `h`: a fault quarantines its type */
static int settle(WIKICOB* w, int h, TAGPU_COBVM_RUN* run, int r, const char* what)
{
    UNIT* u = w->unit[h];
    if (r >= 0 && u->block && drain(w, u, run) < 0) r = -1;
    if (r < 0) quarantine(w, u->type, what, run->reason);
    return r;
}

static int make_vm(WIKICOB* w, UNIT* u)
{
    const TYPE* ty = &w->type[u->type];
    const TagpuCobProgram* p = ty->prog;
    const size_t statics = (size_t)(p->statics ? p->statics : 1) * 4;
    const size_t anim = (size_t)(p->pieces ? p->pieces : 1) * TAGPU_COBVM_ANIM * 4;
    const size_t model = TAGPU_COBVM_MODEL_BYTES((size_t)ty->nodes);
    char why[160];
    int i;
    u->block = (unsigned char*)calloc(1, OBJ_BYTES + statics + anim + model);
    if (!u->block) return 0;
    u->model = u->block + OBJ_BYTES + statics + anim;
    /* the model builder's flags (0x45AED4, 0x45AF31, 0x45AF1B): cached and shaded, visible with
       three or more vertices */
    for (i = 0; i < ty->nodes; i++) {
        unsigned char* f = u->model + TAGPU_COBVM_PRIM_BASE + (size_t)i * TAGPU_COBVM_PRIM_BYTES +
                           TAGPU_COBVM_PRIM_FLAGS;
        f[0] = (unsigned char)(TAGPU_COBVM_CACHED | TAGPU_COBVM_SHADED |
                               (ty->visible[i] ? TAGPU_COBVM_VISIBLE : 0));
    }
    if (!tagpu_cobvm_init(&u->vm, p, TAGPU_COBVM_IMPURE, u->block, (int32_t*)(u->block + OBJ_BYTES),
                          (int32_t*)(u->block + OBJ_BYTES + statics), u->model, why, sizeof why)) {
        drop_vm(u);
        return 0;
    }
    return 1;
}

/* ---- the entries ---- */

WIKICOB* tagpu_wikicob_new(int ntypes, const WIKICOBTYPE* types, uint32_t seed, char* err, size_t errn)
{
    WIKICOB* w;
    int t;
    if (ntypes < 0 || ntypes > WIKICOB_TYPES || (ntypes && !types)) {
        snprintf(err, errn, "the COB host takes 0 to %d types, not %d", WIKICOB_TYPES, ntypes);
        return NULL;
    }
    w = (WIKICOB*)calloc(1, sizeof *w);
    if (!w) { snprintf(err, errn, "the COB host: out of memory"); return NULL; }
    w->ntypes = ntypes;
    w->rng = seed ? seed : 1u;              /* xorshift's one fixed point is 0 */
    w->host.ctx = w;
    w->host.get = host_get;
    w->host.world = &w->world;
    w->host.rand = host_rand;
    w->host.cargo = host_cargo;
    w->host.carrier = host_carrier;
    w->host.alive = host_alive;
    w->host.commit = host_commit;
    w->world.ctx = w;
    w->world.slots = WIKICOB_UNITS + 1;
    w->world.unit = world_unit;
    w->world.caller = world_caller;
    w->world.allied = world_allied;
    w->world.local = world_local;
    for (t = 0; t < ntypes; t++) {
        const WIKICOBTYPE* in = &types[t];
        TYPE* ty = &w->type[t];
        char why[160];
        why[0] = 0;
        if (!in->cob || !in->ncob) { quarantine(w, t, "no script", "the type has none"); continue; }
        if (in->pieces <= 0 || in->pieces > WIKICOB_PIECES || !in->visible) {
            quarantine(w, t, "no model", "the type's model gives no pieces");
            continue;
        }
        ty->nodes = in->pieces;
        memcpy(ty->visible, in->visible, (size_t)in->pieces);
        ty->blob = (unsigned char*)malloc(in->ncob);
        if (!ty->blob) { quarantine(w, t, "the program", "out of memory"); continue; }
        memcpy(ty->blob, in->cob, in->ncob);
        ty->prog = tagpu_cob_program(ty->blob, in->ncob, why, sizeof why);
        if (!ty->prog) { quarantine(w, t, "the program", why); continue; }
        if (!tagpu_cob_bind_model(ty->prog, (uint32_t)in->pieces, why, sizeof why))
            quarantine(w, t, "the program", why);
    }
    return w;
}

void tagpu_wikicob_free(WIKICOB* w)
{
    int k;
    if (!w) return;
    for (k = 1; k <= w->nunit; k++) {
        if (!w->unit[k]) continue;
        drop_vm(w->unit[k]);
        free(w->unit[k]);
    }
    for (k = 0; k < w->ntypes; k++) {
        if (w->type[k].prog) tagpu_cob_program_free(w->type[k].prog);
        free(w->type[k].blob);
    }
    free(w);
}

int tagpu_wikicob_spawn(WIKICOB* w, int type, const WIKICOBFACT* facts, int nfacts)
{
    TAGPU_COBVM_RUN run;
    UNIT* u;
    int h, s, r;
    if (type < 0 || type >= w->ntypes || w->nunit >= WIKICOB_UNITS) return 0;
    u = (UNIT*)calloc(1, sizeof *u);
    if (!u) return 0;
    h = ++w->nunit;
    w->unit[h] = u;
    u->type = type;
    u->live = 1;
    if (w->type[type].quarantined || !make_vm(w, u)) return h;
    s = tagpu_cobvm_index(w->type[type].prog, "Create");
    if (s < 0) return h;
    enter(w, h, &run, facts, nfacts);
    r = tagpu_cobvm_start(&u->vm, &run, s, 0, 1, 0, 0, NULL);
    settle(w, h, &run, r, "Create");
    leave(w);
    return h;
}

void tagpu_wikicob_remove(WIKICOB* w, int unit)
{
    UNIT* u = unit_at(w, unit);
    if (!u) return;
    u->live = 0;
    drop_vm(u);
}

int tagpu_wikicob_call(WIKICOB* w, int unit, const char* script, int how, int argc, const int32_t* args,
                       const WIKICOBFACT* facts, int nfacts)
{
    TAGPU_COBVM_RUN run;
    UNIT* u = unit_at(w, unit);
    int32_t words[4] = {0, 0, 0, 0};
    int s, r, k;
    if (!u || !u->live || !u->block) return 0;
    s = tagpu_cobvm_index(w->type[u->type].prog, script);
    if (s < 0) return 0;
    if (how == WIKICOB_ARGS) {
        if (argc < 0 || argc > 4 || (argc && !args)) return 0;
        for (k = 0; k < argc; k++) words[k] = args[k];
    }
    enter(w, unit, &run, facts, nfacts);
    r = how == WIKICOB_ARGS ? tagpu_cobvm_start(&u->vm, &run, s, 0, 0, 1, argc, words)
                            : tagpu_cobvm_start(&u->vm, &run, s, 0, how == WIKICOB_NOW, 0, 0, NULL);
    r = settle(w, unit, &run, r, script);
    leave(w);
    return r;
}

int tagpu_wikicob_activate(WIKICOB* w, int unit, int on)
{
    TAGPU_COBVM_RUN run;
    UNIT* u = unit_at(w, unit);
    int r;
    on = on != 0;
    if (!u || !u->live || u->value[WIKICOB_ACTIVATION] == on) return 0;
    u->value[WIKICOB_ACTIVATION] = on;
    if (!u->block) return 0;
    u->defer[0] = on;
    u->ndefer = 1;
    enter(w, unit, &run, NULL, 0);
    r = settle(w, unit, &run, 1, on ? "Activate" : "Deactivate");
    leave(w);
    return r;
}

void tagpu_wikicob_slice(WIKICOB* w, const WIKICOBFACT* facts, int nfacts)
{
    int k;
    for (k = 1; k <= w->nunit; k++) {
        TAGPU_COBVM_RUN run;
        UNIT* u = w->unit[k];
        if (!u || !u->live || !u->block) continue;
        enter(w, k, &run, facts, nfacts);
        settle(w, k, &run, tagpu_cobvm_tick(&u->vm, &run, 1), "the slice");
        leave(w);
    }
}

int32_t tagpu_wikicob_value(const WIKICOB* w, int unit, int id)
{
    const UNIT* u = unit_at(w, unit);
    if (!u || id < 0 || id >= WIKICOB_VALUES) return 0;
    if (!u->block && is_gate(id)) return 1;
    return u->value[id];
}

int tagpu_wikicob_resting(const WIKICOB* w, int unit)
{
    const UNIT* u = unit_at(w, unit);
    return !u || !u->block;
}

const unsigned char* tagpu_wikicob_model(const WIKICOB* w, int unit, int* nprim)
{
    const UNIT* u = unit_at(w, unit);
    if (!u || !u->block) { if (nprim) *nprim = 0; return NULL; }
    if (nprim) *nprim = w->type[u->type].nodes;
    return u->model;
}

const char* tagpu_wikicob_note(const WIKICOB* w, int type)
{
    return type >= 0 && type < w->ntypes ? w->type[type].note : "";
}

static uint64_t fnv(uint64_t h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static uint64_t fnv32(uint64_t h, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char)v; b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16); b[3] = (unsigned char)(v >> 24);
    return fnv(h, b, 4);
}

uint64_t tagpu_wikicob_hash(const WIKICOB* w, uint64_t h)
{
    int k, i;
    h = fnv32(h, w->rng);
    h = fnv32(h, (uint32_t)w->ntypes);
    for (k = 0; k < w->ntypes; k++) h = fnv32(h, (uint32_t)w->type[k].quarantined);
    h = fnv32(h, (uint32_t)w->nunit);
    for (k = 1; k <= w->nunit; k++) {
        const UNIT* u = w->unit[k];
        if (!u) { h = fnv32(h, 0xFFFFFFFFu); continue; }
        h = fnv32(h, (uint32_t)u->type);
        h = fnv32(h, (uint32_t)u->live);
        for (i = 0; i < WIKICOB_VALUES; i++) h = fnv32(h, (uint32_t)u->value[i]);
        h = fnv32(h, u->block != NULL);
        if (u->block) {
            const TagpuCobProgram* p = w->type[u->type].prog;
            h = fnv(h, u->block, TAGPU_COB_OBJECT_BYTES);
            h = fnv(h, u->block + OBJ_BYTES, (size_t)p->statics * 4);
            h = fnv(h, u->vm.anim, (size_t)p->pieces * TAGPU_COBVM_ANIM * 4);
            h = fnv(h, u->model, TAGPU_COBVM_MODEL_BYTES((size_t)w->type[u->type].nodes));
        }
    }
    return h;
}
