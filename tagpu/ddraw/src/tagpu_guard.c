#include "tagpu_simreg.h"
SIM_FEATURE(guard_seats, SIM_PUPPET_OWNER, "guard-seats stock-guard-seats");
/* THE GUARD'S SEATS: builders that guard a factory are seated where they can help build without moving, packed
   round the plant, and a lane is kept free for each movement class of its ground products
   (research/notes/factory-guard-plan.md; the module note research/notes/modules/factory-guard.md). The geometry
   is tagpu_guardgeo.c's; this file reads the engine, keeps the rows and points the engine's own guard at them.

   THE ENGINE [DISASSEMBLED 2026-11-18; research/notes/engine/orders.md, *The ground Guard's node*].
   Follow_Ground 0x406300 runs a guard's state 1 on every wake: after the attack reaction and the repair branch it
   reaches 0x40654B, the assist, which -- when the guarded plant's head order carries 0x100000 and has a frame --
   puts a HelpBuild on the frame in front of the guard (0x43ACB0 at 0x40667A); any other way it falls to the home
   move 0x4066AC, a point goal at the plant's position plus the node's offset +0x22/+0x26/+0x2A, of radius
   (+0x36) / 2, then 30 ticks of sleep (0x4066E0..0x4066F6). Every path to the home move passes 0x40654B: the
   branches reach it from 0x406500 and 0x406549, and the one branch from outside into 0x40654C..0x4066AB,
   0x406516's `je 0x406691` (the repair branch's failed allocation), returns 3 there without a move. HelpBuild
   0x403F70 walks a helper only when it is outside the annulus its state 0 builds round the frame's centre
   (tagpu_guardgeo.h, THE BAND); inside it the goal is reached in the class step, before any physics, and the
   build starts the next tick, and nothing after the goal tests range again (engine/orders.md, *Range is tested
   once*). So a builder standing on a seat in every product's band helps every unit without a step. The frame
   stands in the plant's yard, so for a stock builddistance that band barely leaves the plant's footprint
   (ARMCK on a Flash in an ARMVP: one cell outside it); a mod's longer builddistances give more seats.

   THE HOOKS. A stub at 0x40654B runs the stolen `mov eax,[ebx+0x16]; mov ecx,[eax+0x5c]` and calls guard_visit
   with the registers: ebx the guard node, edi the guarder, eax the plant. The visit rewrites the node's offset
   and radius so that the home move goes to the builder's seat, or to its wait spot, and answers whether to skip
   the assist: a builder with no seat goes home instead of walking in to help. It then runs the stolen `cmp
   ecx,ebp` and goes on at 0x406553, or, skipped, at 0x4066AC with eax the plant, which is what the home move
   reads. The assist is taken only by a builder standing on its seat. The visit is stock -- it writes nothing and answers 0 -- for a guarder that
   is not a ground builder, for a guarded unit that is not a finished plant whose build menu makes a mobile
   unit, and for every guard once a table is full. Three more keep a seated guard's arm open from one unit to the
   next, at StartBuilding's entry 0x438590, the order destructor's bit test 0x43A21E and the order controller's
   handler dispatch 0x43B865 (THE ARM, below).

   A BUILD SITE. A site one of the builder's owner's builders is being refused on (build scatter's rows,
   tagpu_scatter.h) is ground a structure is about to stand on: it is taken ground for that owner's seats and wait
   spots, as is every place build scatter has sent one of that owner's units to, a seat it covers is given up at the builder's next visit, and a builder standing on one is sent to the
   exact place it was given, not to within its wait spot's two cells. Build scatter orders no unit this module
   seats, and sends none to a lane, a seat or a wait spot (tagpu_guard_mark): a unit has one manager.

   STATE, all SIM_STATE: the plant rows s_plant (its identity, its frame's centre, its box, its lanes and the
   signature of the structures in its box) and the builder rows s_seat (its identity, its plant, its seat or
   wait spot, the cell it last stood on and since when, a seat it avoids, its held arm, the heading of its last
   StartBuilding and since when its plant has been idle), and s_armed, the count of rows holding an arm. The
   level's plant types s_type are SIM_CONTENT: the build menus of the level's
   data. Rows name units by slot and birth (tagpu_unit_birth): a slot recycled for another unit never matches a
   row. OWNER: the guard handler runs only on its owner's peer (0x48AEB5..0x48AECD), never for a puppet seat, and
   the visit refuses a puppet level besides.

   BOUNDS. A unit pointer the engine hands us is tested to be a slot of the unit array; every slot read from a
   cell is below the slot count; every cell is on the map; a def pointer is a record of the def array. A box is at
   most GUARD_BOX a side. ptr tests are filters on values; the reads themselves are the game thread's, inside the
   engine's own call, where the guard handler itself reads the same memory.

   THREADS. The rows are cleared and the plant types read on the LOADER thread at the PLAYERS call (0x4919C8),
   before the game thread's first step; the visit, the arm's three hooks and its folds run on the game thread
   inside the unit tick: the order run 0x43B7C0 and the destructor and StartBuilding calls of its handlers. */

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "tagpu_detour.h"
#include "tagpu_engine.h"
#include "tagpu_guard.h"
#include "tagpu_guardgeo.h"
#include "tagpu_limits.h"
#include "tagpu_log.h"
#include "tagpu_puppet.h"
#include "tagpu_resource.h"
#include "tagpu_scatter.h"
#include "tagpu_worldq.h"
#include "worldq.h"

#define VA_SITE        0x0040654Bu   /* Follow_Ground state 1: the assist's first instruction               */
#define VA_ASSIST_JE   0x00406553u   /* the `je 0x4066AC` after the stolen cmp                              */
#define VA_HOME        0x004066ACu   /* the home move                                                       */

#define VA_ACTION_OF   0x00438760u   /* thiscall (u8* out, const char* name): an order name's action byte   */
#define VA_START       0x00438590u   /* StartBuilding (unit, node, heading), ret 0xC                        */
#define VA_START_NEXT  0x00438597u   /* after its first three pushes and `mov edi,[esp+0x10]`               */
#define VA_FOLD        0x004385F0u   /* stdcall (unit, node): StopBuilding behind node+0x42 bit 0x400000     */
#define VA_DTOR_TEST   0x0043A21Eu   /* the order destructor 0x43A1F0's `test [esi+0x42],0x400000`          */
#define VA_DTOR_NEXT   0x0043A225u   /* its `je 0x43A264`, which reads the test                             */
#define VA_DISPATCH    0x0043B865u   /* the order run 0x43B7C0's `mov ecx,[0x512344]` before its handler call */
#define VA_DISP_NEXT   0x0043B86Bu
#define S_HELPBUILD    0x005013F0u   /* "HelpBuild"                                                         */
#define S_FOLLOW       0x005014D8u   /* "Follow_Ground"                                                     */
#define S_BUILDINGBLD  0x005011F0u   /* "BuildingBuild"                                                     */
#define S_QMOVE        0x005010A4u   /* "QMove": a lab's rally (engine/recorder.md)                         */

#define U_CELLX        0x76          /* s16 the footprint's first cell (engine/units.md, The footprint cell) */
#define U_CELLZ        0x78
#define U_FX           0x7E          /* s16 the footprint, copied from the def (0x485AAA)                   */
#define U_FZ           0x80
#define U_ORDERS       0x5C          /* the main order list's head                                          */
#define U_STANCE       0x10F         /* bit 0 INBUILDSTANCE: HelpBuild's state 2 waits on it (0x438709)       */
#define MV_STAMP       0x26          /* u32 the mover's claim stamp: GameTime at its last change of cell    */
#define N_ACTION       0x04
#define N_STATE        0x05          /* the handler's state: the controller steps it (0x43B897, 0x43B89F)    */
#define N_TARGET       0x16
#define N_UNIT         0x0E          /* the node's unit (the destructor reads it at 0x43A227)               */
#define N_FLAGS        0x42          /* bit 0x400000: StartBuilding was started for this node (0x4385E3)    */
#define NF_STARTED     0x400000u
#define N_OFFSET       0x22          /* i32 x3: the guard's offset from the plant, 16.16                   */
#define N_RADIUS       0x36          /* i32: the home goal's radius is half of it                           */
#define N_NEXT         0x4A
#define CELL_STRIDE    0x0D
#define PL_SIDE_INFO   0x27          /* PlayerInfo*; its +0x95 the side (tagpu_impai.c, record_side)        */
#define M_SIDES        0x37F39       /* u32 the sides the game loaded                                       */

#define GUARD_TYPES    512           /* plant types x sides a level keeps                                   */
#define GUARD_CLASSES  8             /* ground classes a plant's products travel by                         */
#define GUARD_PLANTS   64
#define GUARD_SEATS    256
#define GUARD_LANES    24
#define GUARD_BOX      112           /* cells a side at most                                                */
#define LANE_MARGIN    1             /* cells either side of the largest product                            */
#define SEAT_BUDGET    96            /* candidates a seat search tests for a way out                        */
#define STUCK_TICKS    180           /* ticks with no change of cell short of the seat before it is given up:
                                        three of the pathfinder's 60-tick re-searches of a held walker        */
#define REACH_MAX      400           /* px: a builddistance past it is taken as this                        */
#define MENU_CAP       4096

/* ---- the level's plant types (SIM_CONTENT) ---- */

typedef struct GuardType {
    uint16_t type;
    uint8_t  side, nclass;
    uint8_t  mclass[GUARD_CLASSES], csize[GUARD_CLASSES], cfx[GUARD_CLASSES], cfz[GUARD_CLASSES];
    int16_t  rin_min, rin_max;        /* over every mobile product: the band's terms (tagpu_guardgeo.h)     */
} GuardType;
static GuardType s_type[GUARD_TYPES];
static unsigned  s_ntype;
SIM_VAR(s_ntype, SIM_CONTENT);
SIM_ROWS_LIVE(s_type, SIM_CONTENT, SIM_ID_NONE, s_ntype, NULL);

/* the level's widest footprint side of a def with a yard, cells, at most WQ_FOOT_MAX */
static int32_t s_yard_max;
SIM_VAR(s_yard_max, SIM_CONTENT);

/* the level's widest builddistance of a mobile builder that can assist, px, at most REACH_MAX */
static int32_t s_reach;
SIM_VAR(s_reach, SIM_CONTENT);

/* ---- the rows (SIM_STATE) ---- */

typedef struct GuardPlant {
    uint16_t slot, type;              /* the plant's unit slot and def index                                */
    uint32_t birth;
    uint8_t  used, spot_known, planned, nlane;
    int16_t  cx, cz;                  /* the plant's first cell                                             */
    int32_t  spot_x, spot_z;          /* the frame's centre, 16.16 world px                                 */
    int16_t  bx0, bz0, bw, bh;        /* the box                                                            */
    uint32_t sig;                     /* the structures in the box                                          */
    int32_t  sig_tick;                /* GameTime the signature was taken; -1 none                          */
    GgRect   lane[GUARD_LANES];
} GuardPlant;
/* the cells, the box, the lanes and the tick are numbers whose dwords (two int16 cells read as z << 16 | x) can
   take any value, an address's included, so the address scan passes over them while the replay compares them */
static const SimField PLANT_LAYOUT[] = {
    SIM_FIELD(GuardPlant, slot, SIMF_ID, SIM_ID_UNIT_SLOT),
    SIM_FIELD(GuardPlant, type, SIMF_ID, SIM_ID_UNIT_TYPE),
    SIM_FIELD_AT(offsetof(GuardPlant, used), offsetof(GuardPlant, sig) - offsetof(GuardPlant, used), SIMF_OPAQUE,
                 SIM_ID_NONE),
    SIM_FIELD(GuardPlant, sig, SIMF_OPAQUE, SIM_ID_NONE),
    SIM_FIELD_AT(offsetof(GuardPlant, sig_tick), sizeof(GuardPlant) - offsetof(GuardPlant, sig_tick), SIMF_OPAQUE,
                 SIM_ID_NONE),
    SIM_FIELD_END
};
static GuardPlant s_plant[GUARD_PLANTS];
SIM_ROWS(s_plant, SIM_STATE, SIM_ID_NONE, PLANT_LAYOUT);

enum { GS_FREE, GS_SEATED, GS_WAITING };
typedef struct GuardSeat {
    uint16_t builder, plant;          /* the builder's slot; its plant's row                               */
    uint32_t birth;
    uint8_t  state, arm;              /* arm 1: open with no order owning it (THE ARM)                      */
    int16_t  x, z;                    /* the seat's place, or the wait spot                                 */
    int16_t  lastx, lastz;            /* the cell it last stood on                                          */
    int32_t  moved;                   /* GameTime it was first seen on that cell                            */
    int16_t  avoidx, avoidz;          /* a seat it could not reach; -32768 none                             */
    uint16_t heading, pad;            /* the heading of its last StartBuilding on its plant's frame         */
    int32_t  idle;                    /* GameTime the held arm's plant was first seen with no build; 0 none */
} GuardSeat;
static const SimField SEAT_LAYOUT[] = {
    SIM_FIELD(GuardSeat, builder, SIMF_ID, SIM_ID_UNIT_SLOT),
    SIM_FIELD_AT(offsetof(GuardSeat, state), sizeof(GuardSeat) - offsetof(GuardSeat, state), SIMF_OPAQUE,
                 SIM_ID_NONE),
    SIM_FIELD_END
};
static GuardSeat s_seat[GUARD_SEATS];
SIM_ROWS(s_seat, SIM_STATE, SIM_ID_NONE, SEAT_LAYOUT);

/* the rows with arm set: the dispatch hook's one test while none is held. Every change of arm goes through
   arm_hold, arm_drop and seat_clear */
static int32_t s_armed;
SIM_VAR(s_armed, SIM_STATE);

static void arm_drop(GuardSeat* t)
{
    if (t->arm) { t->arm = 0; s_armed--; }
    t->idle = 0;
}

static void seat_clear(GuardSeat* t)
{
    arm_drop(t);
    memset(t, 0, sizeof *t);
}

static unsigned char* s_stub;
SIM_CODE(s_stub);
static unsigned char* s_start_stub;
SIM_CODE(s_start_stub);
static unsigned char* s_dtor_stub;
SIM_CODE(s_dtor_stub);
static unsigned char* s_disp_stub;
SIM_CODE(s_disp_stub);
static struct { uint8_t installed, arm; } s_install;
SIM_VAR(s_install, SIM_INSTALL);

/* diagnostics the simulation never reads */
static struct { uint32_t visits, seated, waiting, full, lanes, nolane; } s_diag;
SIM_VAR(s_diag, SIM_PRIVATE);

typedef unsigned char* (__attribute__((thiscall))* ActionOf)(void*, const char*);

/* ---- engine reads ---- */

typedef struct Eng {
    char* ta; char* units; unsigned nslots; char* defs; unsigned ndefs;
    char* grid; int w, h; int32_t now;
    WQ_ENGINE wc; WORLDQ q;
} Eng;

static int eng_open(Eng* e)
{
    memset(e, 0, sizeof *e);
    if (!tagpu_worldq_open_content(&e->wc, &e->q)) return 0;
    e->ta = e->wc.ta;
    e->defs = e->wc.defs;
    e->ndefs = e->wc.ndefs;
    e->grid = e->wc.grid;
    e->w = e->wc.w;
    e->h = e->wc.h;
    e->units = *(char**)(e->ta + OFF_UNIT_BEGIN);
    e->nslots = *(const uint16_t*)(e->ta + OFF_UNITSLOTS);
    memcpy(&e->now, e->ta + OFF_GAMETIME, 4);
    return e->units != NULL && e->nslots > 0;
}

/* the slot of a unit pointer, or -1 when it is not exactly a record of the unit array */
static int slot_of(const Eng* e, const char* u)
{
    uintptr_t off;
    if (!u || u < e->units) return -1;
    off = (uintptr_t)(u - e->units);
    if (off % UNIT_STRIDE || off / UNIT_STRIDE >= e->nslots) return -1;
    return (int)(off / UNIT_STRIDE);
}

static char* unit_at(const Eng* e, unsigned slot)
{
    return slot < e->nslots ? e->units + (size_t)slot * UNIT_STRIDE : NULL;
}

static int alive(const char* u) { return (*(const uint32_t*)(u + U_STATE) & ST_ALIVE) != 0; }
static int structure(const char* u) { return *(char* const*)(u + U_MOVER) == NULL; }

/* the def index of a unit, 0 when its def pointer is not a record of the def array */
static unsigned type_of(const Eng* e, const char* u)
{
    const char* d = *(const char* const*)(u + U_TYPE);
    uintptr_t off;
    if (!d || d < e->defs) return 0;
    off = (uintptr_t)(d - e->defs);
    if (off % UDEF_STRIDE || off / UDEF_STRIDE >= e->ndefs) return 0;
    return (unsigned)(off / UDEF_STRIDE);
}

static unsigned birth_of(unsigned slot)
{
    unsigned b = 0;
    return tagpu_unit_birth(slot, &b) ? b : 0;
}

static unsigned char action_of(uintptr_t name)
{
    unsigned char a = 0;
    ((ActionOf)VA_ACTION_OF)(&a, (const char*)name);
    return a;
}

/* the unit's owner's side, 0xFF unknown (tagpu_impai.c, record_side) */
static unsigned side_of(const Eng* e, const char* u)
{
    const unsigned owner = *(const uint8_t*)(u + U_OWNER);
    const unsigned char* info;
    const unsigned sides = *(const unsigned*)(e->ta + M_SIDES);
    if (owner >= PLAYER_COUNT) return 0xFF;
    info = *(const unsigned char* const*)(e->ta + OFF_PLAYERS + owner * PLAYER_STRIDE + PL_SIDE_INFO);
    return info && info[0x95] < sides ? info[0x95] : 0xFF;
}

static const GuardType* type_row(unsigned type, unsigned side)
{
    unsigned i;
    for (i = 0; i < s_ntype; i++)
        if (s_type[i].type == type && s_type[i].side == side) return &s_type[i];
    return NULL;
}

/* the slot standing in slot A of cell (x, z), 0 none or off the map */
static unsigned cell_unit(const Eng* e, int x, int z)
{
    uint16_t s;
    if (x < 0 || z < 0 || x >= e->w || z >= e->h) return 0;
    memcpy(&s, e->grid + ((size_t)z * (size_t)e->w + (size_t)x) * CELL_STRIDE, 2);
    return s < e->nslots ? s : 0;
}

/* ---- the plant types, at the level init ---- */

static void type_add(const Eng* e, unsigned t, unsigned side, const uint16_t* menu, int n)
{
    GuardType g;
    int k, products = 0;
    memset(&g, 0, sizeof g);
    g.type = (uint16_t)t;
    g.side = (uint8_t)side;
    g.rin_min = 0x7FFF;
    for (k = 0; k < n; k++) {
        WQ_DEF d;
        int c, rin;
        if (!e->q.ops->def(&e->q, menu[k], &d) || !(d.flags & WQ_DF_MOVER)) continue;
        if (d.fx < 1 || d.fz < 1 || d.fx > WQ_FOOT_MAX || d.fz > WQ_FOOT_MAX) return;
        rin = gg_rin(d.fx, d.fz);
        if (rin < g.rin_min) g.rin_min = (int16_t)rin;
        if (rin > g.rin_max) g.rin_max = (int16_t)rin;
        products++;
        if ((d.flags & WQ_DF_VTOL) || d.mclass >= WQ_MCLASSES) continue;
        for (c = 0; c < g.nclass && g.mclass[c] != d.mclass; c++) {}
        if (c == g.nclass) {
            if (g.nclass == GUARD_CLASSES) return;
            g.mclass[c] = d.mclass;
            g.cfx[c] = (uint8_t)d.fx;
            g.cfz[c] = (uint8_t)d.fz;
            g.nclass++;
        }
        if (d.fx > g.csize[c]) g.csize[c] = (uint8_t)d.fx;
        if (d.fz > g.csize[c]) g.csize[c] = (uint8_t)d.fz;
        if (d.fx < g.cfx[c]) g.cfx[c] = (uint8_t)d.fx;
        if (d.fz < g.cfz[c]) g.cfz[c] = (uint8_t)d.fz;
    }
    if (!products || s_ntype == GUARD_TYPES) return;
    s_type[s_ntype++] = g;
}

/* ---- a plant's row ---- */

static GuardPlant* plant_row(const Eng* e, const char* p, unsigned slot, unsigned type, int create)
{
    const unsigned b = birth_of(slot);
    const int16_t cx = *(const int16_t*)(p + U_CELLX), cz = *(const int16_t*)(p + U_CELLZ);
    int i, free_i = -1;
    (void)e;
    for (i = 0; i < GUARD_PLANTS; i++) {
        GuardPlant* r = &s_plant[i];
        if (!r->used) { if (free_i < 0) free_i = i; continue; }
        if (r->slot == slot) {
            if (r->birth == b && r->type == type && r->cx == cx && r->cz == cz) return r;
            memset(r, 0, sizeof *r);           /* the slot holds another unit now */
            if (free_i < 0) free_i = i;
        }
    }
    if (!create || free_i < 0) return NULL;
    memset(&s_plant[free_i], 0, sizeof s_plant[free_i]);
    s_plant[free_i].used = 1;
    s_plant[free_i].slot = (uint16_t)slot;
    s_plant[free_i].type = (uint16_t)type;
    s_plant[free_i].birth = b;
    s_plant[free_i].cx = cx;
    s_plant[free_i].cz = cz;
    s_plant[free_i].sig_tick = -1;
    return &s_plant[free_i];
}

/* a plant's box: its footprint grown on every side by how far its seats and its lanes reach, clipped to the map */
static void plant_box(const Eng* e, GuardPlant* r, const GuardType* g, int fx, int fz)
{
    int smax = 0, c, grow, x0, z0, x1, z1;
    for (c = 0; c < g->nclass; c++) if (g->csize[c] > smax) smax = g->csize[c];
    grow = (s_reach + g->rin_max) / 16 + 4 + smax + LANE_MARGIN;
    if (2 * grow + (fx > fz ? fx : fz) > GUARD_BOX) grow = (GUARD_BOX - (fx > fz ? fx : fz)) / 2;
    x0 = r->cx - grow; z0 = r->cz - grow; x1 = r->cx + fx - 1 + grow; z1 = r->cz + fz - 1 + grow;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 > e->w - 1) x1 = e->w - 1;
    if (z1 > e->h - 1) z1 = e->h - 1;
    r->bx0 = (int16_t)x0; r->bz0 = (int16_t)z0;
    r->bw = (int16_t)(x1 - x0 + 1); r->bh = (int16_t)(z1 - z0 + 1);
}

/* FNV-1a over the slot and birth of every structure that stands in the box: a building placed or destroyed in it
   changes it */
static uint32_t box_sig(const Eng* e, const GuardPlant* r)
{
    uint32_t h = 2166136261u;
    int x, z;
    for (z = r->bz0; z < r->bz0 + r->bh; z++)
        for (x = r->bx0; x < r->bx0 + r->bw; x++) {
            const unsigned s = cell_unit(e, x, z);
            const char* u = s ? unit_at(e, s) : NULL;
            uint32_t v;
            if (!u || !structure(u)) continue;
            v = (uint32_t)s * 2654435761u ^ birth_of(s) ^ (uint32_t)(x << 16 | (z & 0xFFFF));
            h = (h ^ v) * 16777619u;
        }
    return h;
}

/* ---- the scratch of one visit ---- */

typedef struct Scratch {
    uint8_t* blocked;   /* structures, w h                       */
    uint8_t* taken;     /* what a seat may not cover, w h        */
    uint8_t* taken2;
    uint8_t* fit;       /* one class's map, WQ_PASS_CAP          */
    uint8_t* pass[GUARD_CLASSES + 1]; /* builders' class maps    */
    uint8_t  pclass[GUARD_CLASSES + 1];
    int      npass;
    int32_t* bsums;
    GgWork   wk;
    size_t   passcap;
    void*    block;
} Scratch;

static int scratch_get(Scratch* s, int w, int h)
{
    const size_t n = (size_t)w * h, cells = (size_t)(w + 1) * (h + 1);
    const size_t passcap = WQ_PASS_CAP(w, h);
    int k;
    size_t bytes = 3 * n + (GUARD_CLASSES + 2) * passcap + 2 * cells * sizeof(int32_t) + n / 8 + 1 +
                   n * (2 * sizeof(int16_t) + sizeof(int32_t)) + 64;
    char* p;
    memset(s, 0, sizeof *s);
    if (!(s->block = tagpu_sim_scratch(bytes))) return 0;
    p = (char*)s->block;
    s->passcap = passcap;
    s->wk.qf = (int32_t*)p; p += n * sizeof(int32_t);
    s->bsums = (int32_t*)p; p += cells * sizeof(int32_t);
    s->wk.sums = (int32_t*)p; p += cells * sizeof(int32_t);
    s->wk.qx = (int16_t*)p; p += n * sizeof(int16_t);
    s->wk.qz = (int16_t*)p; p += n * sizeof(int16_t);
    s->blocked = (uint8_t*)p; p += n;
    s->taken = (uint8_t*)p; p += n;
    s->taken2 = (uint8_t*)p; p += n;
    s->fit = (uint8_t*)p; p += passcap;
    for (k = 0; k <= GUARD_CLASSES; k++) { s->pass[k] = (uint8_t*)p; p += passcap; }
    s->wk.seen = (uint8_t*)p;
    return 1;
}

/* a class's content map over the plant's box, from the scratch's cache */
static const uint8_t* class_map(Eng* e, Scratch* s, const GuardPlant* r, unsigned mclass)
{
    int k;
    for (k = 0; k < s->npass; k++) if (s->pclass[k] == mclass) return s->pass[k];
    if (s->npass > GUARD_CLASSES) return NULL;
    if (!e->q.ops->passable(&e->q, (int)mclass, r->bx0, r->bz0, r->bw, r->bh, s->pass[s->npass], s->passcap))
        return NULL;
    s->pclass[s->npass] = (uint8_t)mclass;
    return s->pass[s->npass++];
}

/* the structures of the box into s->blocked */
static void box_blocked(const Eng* e, const GuardPlant* r, Scratch* s)
{
    int x, z;
    for (z = 0; z < r->bh; z++)
        for (x = 0; x < r->bw; x++) {
            const unsigned u = cell_unit(e, r->bx0 + x, r->bz0 + z);
            const char* p = u ? unit_at(e, u) : NULL;
            s->blocked[z * r->bw + x] = (uint8_t)(p && structure(p));
        }
}

static void mark_rect(uint8_t* t, const GuardPlant* r, int x0, int z0, int x1, int z1, uint8_t v);

/* every structure whose footprint reaches the box marks its WHOLE footprint (first cell +0x76/+0x78, size
   +0x7E/+0x80), clipped to the box: a factory's open yard cells are held by no unit in the map (its products drive
   through them), so `blocked` leaves them free, and a seat or wait spot there stands inside the plant. The scan
   runs over the box grown by the level's widest yard less one, so a plant with only its open cells in the box
   is found by a held cell outside it. The lane search keeps `blocked`: it starts from the yard's exits and must
   see the yard open */
static void mark_footprints(const Eng* e, const GuardPlant* r, uint8_t* t)
{
    const int m = s_yard_max > 1 ? s_yard_max - 1 : 0;
    int x, z;
    for (z = -m; z < r->bh + m; z++)
        for (x = -m; x < r->bw + m; x++) {
            const unsigned u = cell_unit(e, r->bx0 + x, r->bz0 + z);
            const char* q = u ? unit_at(e, u) : NULL;
            int fx, fz, cx, cz;
            if (!q || !structure(q)) continue;
            fx = *(const int16_t*)(q + U_FX); fz = *(const int16_t*)(q + U_FZ);
            if (fx < 1 || fz < 1 || fx > WQ_FOOT_MAX || fz > WQ_FOOT_MAX) continue;
            cx = *(const int16_t*)(q + U_CELLX); cz = *(const int16_t*)(q + U_CELLZ);
            mark_rect(t, r, cx, cz, cx + fx - 1, cz + fz - 1, 1);
        }
}

/* the exit nearest the plant's rally, -1 when it holds none (a QMove on its main list) */
static int rally_exit(const Eng* e, const char* p, const GuardPlant* r, const GgExits* L, int fx, int fz)
{
    const unsigned char qmove = action_of(S_QMOVE);
    const char* n = *(const char* const*)(p + U_ORDERS);
    int32_t rx = 0, rz = 0;
    int k, best = -1, guard = 0;
    long long bd = 0;
    (void)e;
    for (; n && guard < 64; n = *(const char* const*)(n + N_NEXT), guard++)
        if (qmove && *(const uint8_t*)(n + N_ACTION) == qmove) break;
    if (!n || guard == 64) return -1;
    memcpy(&rx, n + N_OFFSET, 4);
    memcpy(&rz, n + N_OFFSET + 8, 4);
    rx >>= 20; rz >>= 20;
    for (k = 0; k < L->n; k++) {
        const int mid = (L->a[k] + L->e[k]) / 2, side = L->edge[k];
        const int ex = side < 2 ? r->cx + mid : (side == 2 ? r->cx - 1 : r->cx + fx);
        const int ez = side < 2 ? (side == 0 ? r->cz - 1 : r->cz + fz) : r->cz + mid;
        const long long d = (long long)(ex - rx) * (ex - rx) + (long long)(ez - rz) * (ez - rz);
        if (best < 0 || d < bd) { best = k; bd = d; }
    }
    return best;
}

/* THE LANES of a plant: one per ground class, from its yard's exits over the class's content map and around the
   structures of its box. A class with no way out keeps no lane: nothing it builds can leave, whatever the
   builders do. Lanes of more than GUARD_LANES rectangles keep the first ones found. */
static void plan_lanes(Eng* e, const char* p, GuardPlant* r, const GuardType* g, Scratch* s)
{
    uint8_t yard[WQ_FOOT_MAX * WQ_FOOT_MAX];
    GgExits L;
    GgBox box = { r->bx0, r->bz0, r->bw, r->bh };
    GgRect plant;
    const int fx = *(const int16_t*)(p + U_FX), fz = *(const int16_t*)(p + U_FZ);
    const int open = (s_reach + g->rin_max) / 16 + 2;
    int c, start, n = 0;
    r->nlane = 0;
    r->planned = 1;
    if (!g->nclass || e->q.ops->yard(&e->q, r->type, yard, (int)sizeof yard) != fx * fz) return;
    gg_exits(yard, fx, fz, &L);
    plant.x0 = r->cx; plant.z0 = r->cz;
    plant.x1 = (int16_t)(r->cx + fx - 1); plant.z1 = (int16_t)(r->cz + fz - 1);
    start = rally_exit(e, p, r, &L, fx, fz);
    gg_sums(s->blocked, r->bw, r->bh, s->bsums);
    for (c = 0; c < g->nclass; c++) {
        if (!e->q.ops->passable(&e->q, g->mclass[c], r->bx0, r->bz0, r->bw, r->bh, s->fit, s->passcap)) continue;
        gg_square_fit(s->fit, r->bw, r->bh, g->csize[c] - g->cfx[c], g->csize[c] - g->cfz[c]);
        if (gg_lane(&box, &plant, &L, start, g->csize[c], open, LANE_MARGIN, s->fit, s->bsums, &s->wk, r->lane, &n,
                    GUARD_LANES)) s_diag.lanes++;
        else s_diag.nolane++;
    }
    r->nlane = (uint8_t)n;
    tagpu_logf("guard: lanes plant=%u type=%u cell=%d,%d exits=%d rally_exit=%d classes=%u rects=%d box=%d,%d %dx%d",
               (unsigned)r->slot, (unsigned)r->type, r->cx, r->cz, L.n, start, (unsigned)g->nclass, n, r->bx0, r->bz0,
               r->bw, r->bh);
}

static void mark_rect(uint8_t* t, const GuardPlant* r, int x0, int z0, int x1, int z1, uint8_t v)
{
    int x, z;
    if (x0 < r->bx0) x0 = r->bx0;
    if (z0 < r->bz0) z0 = r->bz0;
    if (x1 > r->bx0 + r->bw - 1) x1 = r->bx0 + r->bw - 1;
    if (z1 > r->bz0 + r->bh - 1) z1 = r->bz0 + r->bh - 1;
    for (z = z0; z <= z1; z++)
        for (x = x0; x <= x1; x++) t[(z - r->bz0) * r->bw + (x - r->bx0)] = v;
}

/* the builder's own footprint and movement class; 0 when the footprint is not 1..WQ_FOOT_MAX a side, which the
   geometry's indexing rests on (tagpu_guardgeo.h) */
static int builder_foot(const Eng* e, const char* b, int* fx, int* fz, unsigned* mclass)
{
    WQ_DEF d;
    *fx = *(const int16_t*)(b + U_FX);
    *fz = *(const int16_t*)(b + U_FZ);
    *mclass = e->q.ops->def(&e->q, (int)type_of(e, b), &d) ? d.mclass : WQ_MCLASS_NONE;
    return *fx >= 1 && *fz >= 1 && *fx <= WQ_FOOT_MAX && *fz <= WQ_FOOT_MAX;
}

/* whether the seat row's builder still guards plant p: a guard node on p within its list's first three */
static int still_guards(const Eng* e, const GuardSeat* t, const char* p, unsigned char follow)
{
    const char* b = unit_at(e, t->builder);
    const char* n;
    int k;
    if (!b || !alive(b) || birth_of(t->builder) != t->birth) return 0;
    for (n = *(const char* const*)(b + U_ORDERS), k = 0; n && k < 3; n = *(const char* const*)(n + N_NEXT), k++)
        if (*(const uint8_t*)(n + N_ACTION) == follow && *(char* const*)(n + N_TARGET) == p) return 1;
    return 0;
}

/* TAKEN, for builder `self`'s search: the structures, the build sites its owner's builders are refused on and
   the places build scatter has sent that owner's units to (A BUILD SITE), every plant's lanes in the box, every other row's seat or
   wait spot in it, and every other unit standing still -- a mover that has not changed cell for 30 ticks, which
   the pathfinder takes as an obstacle (the per-cell rule 0x47DE60 at 0x47DEF8..0x47DF32) */
static void build_taken(const Eng* e, const GuardPlant* r, Scratch* s, unsigned self)
{
    const char* sb = unit_at(e, self);
    int i, k, x, z;
    memcpy(s->taken, s->blocked, (size_t)r->bw * r->bh);
    mark_footprints(e, r, s->taken);
    if (sb) tagpu_scatter_mark(*(const uint8_t*)(sb + U_OWNER), e->now, r->bx0, r->bz0, r->bw, r->bh, s->taken);
    for (i = 0; i < GUARD_PLANTS; i++) {
        const GuardPlant* o = &s_plant[i];
        if (!o->used) continue;
        for (k = 0; k < o->nlane; k++)
            mark_rect(s->taken, r, o->lane[k].x0, o->lane[k].z0, o->lane[k].x1, o->lane[k].z1, 1);
    }
    for (z = 0; z < r->bh; z++)
        for (x = 0; x < r->bw; x++) {
            const unsigned u = cell_unit(e, r->bx0 + x, r->bz0 + z);
            const char* p = u ? unit_at(e, u) : NULL;
            const char* mv;
            uint32_t stamp;
            if (!p || u == self || structure(p)) continue;
            for (i = 0; i < GUARD_SEATS; i++)
                if (s_seat[i].state != GS_FREE && s_seat[i].builder == u) break;
            if (i < GUARD_SEATS) continue;               /* a row's builder: its seat is marked below */
            mv = *(const char* const*)(p + U_MOVER);
            memcpy(&stamp, mv + MV_STAMP, 4);
            if ((int32_t)stamp <= e->now - 30) s->taken[z * r->bw + x] = 1;
        }
    for (i = 0; i < GUARD_SEATS; i++) {
        const GuardSeat* t = &s_seat[i];
        const char* b;
        int fx, fz;
        unsigned mc;
        if (t->state == GS_FREE || t->builder == self || !(b = unit_at(e, t->builder))) continue;
        if (!builder_foot(e, b, &fx, &fz, &mc)) continue;
        mark_rect(s->taken, r, t->x, t->z, t->x + fx - 1, t->z + fz - 1, 2);
    }
}

/* ---- the visit ---- */

static GuardSeat* seat_row(unsigned slot, unsigned birth, int create)
{
    int i, free_i = -1;
    for (i = 0; i < GUARD_SEATS; i++) {
        if (s_seat[i].state == GS_FREE) { if (free_i < 0) free_i = i; continue; }
        if (s_seat[i].builder == slot) {
            if (s_seat[i].birth == birth) return &s_seat[i];
            seat_clear(&s_seat[i]);                 /* the slot's unit died: its arm needs no fold */
            if (free_i < 0) free_i = i;
        }
    }
    if (!create || free_i < 0) return NULL;
    seat_clear(&s_seat[free_i]);
    s_seat[free_i].builder = (uint16_t)slot;
    s_seat[free_i].birth = birth;
    s_seat[free_i].state = GS_WAITING;
    s_seat[free_i].plant = 0xFFFF;
    s_seat[free_i].avoidx = s_seat[free_i].avoidz = INT16_MIN;
    return &s_seat[free_i];
}

/* the guard node's home goal at the centre of the footprint fx x fz at place (x, z): the point goal 0x438930 is
   reached when the guarder's first cell is within (+0x36 / 2 / 16) cells of the place it would have centred on
   the point (0x44CF60), which for a centre (16 x + 8 fx, 16 z + 8 fz) px is (x, z) itself */
static void aim(char* node, const char* p, int x, int z, int fx, int fz, int32_t radius)
{
    int32_t px, pz, ox, oz, zero = 0;
    memcpy(&px, p + U_XFIX, 4);
    memcpy(&pz, p + U_YFIX, 4);
    ox = (int32_t)((uint32_t)(16 * x + 8 * fx) << 16) - px;
    oz = (int32_t)((uint32_t)(16 * z + 8 * fz) << 16) - pz;
    memcpy(node + N_OFFSET, &ox, 4);
    memcpy(node + N_OFFSET + 4, &zero, 4);
    memcpy(node + N_OFFSET + 8, &oz, 4);
    memcpy(node + N_RADIUS, &radius, 4);
}

/* the frame's centre, from the plant's head when it is building one, else the stored one, else the plant's own;
   1 when it moved, which re-plans the plant */
static int update_spot(const Eng* e, const char* p, GuardPlant* r, unsigned char bbuild)
{
    const char* head = *(const char* const*)(p + U_ORDERS);
    const char* frame = head && *(const uint8_t*)(head + N_ACTION) == bbuild ? *(const char* const*)(head + N_TARGET)
                                                                            : NULL;
    int32_t x, z;
    if (frame && slot_of(e, frame) >= 0) {
        memcpy(&x, frame + U_XFIX, 4);
        memcpy(&z, frame + U_YFIX, 4);
        if (!r->spot_known || x != r->spot_x || z != r->spot_z) {
            r->spot_x = x; r->spot_z = z; r->spot_known = 1;
            return 1;
        }
        return 0;
    }
    if (!r->spot_known) {
        memcpy(&r->spot_x, p + U_XFIX, 4);
        memcpy(&r->spot_z, p + U_YFIX, 4);
    }
    return 0;
}

/* a seat row's place still holds: in the box, not on a lane or a structure, in its builder's band */
static int seat_holds(const GuardPlant* r, const GuardSeat* t, int fx, int fz, const uint8_t* blocked,
                      int gx, int gz, int r2min, int r2max)
{
    const int x = t->x - r->bx0, z = t->z - r->bz0;
    int i, j, k;
    const int d2 = (t->x - gx) * (t->x - gx) + (t->z - gz) * (t->z - gz);
    if (x < 0 || z < 0 || x + fx > r->bw || z + fz > r->bh || d2 < r2min || d2 > r2max) return 0;
    for (j = 0; j < fz; j++)
        for (i = 0; i < fx; i++) if (blocked[(z + j) * r->bw + x + i]) return 0;
    for (k = 0; k < GUARD_PLANTS; k++)
        if (s_plant[k].used) {
            int m;
            for (m = 0; m < s_plant[k].nlane; m++) if (gg_overlaps(&s_plant[k].lane[m], t->x, t->z, fx, fz)) return 0;
        }
    return 1;
}

/* the builder's seat, the band's outer edge first, else a wait spot: where it stands when that is free, else the
   nearest free place within 8 cells, else where it stands; a builder outside the box searches from the box's
   nearest place to it */
static void seat_or_wait(Eng* e, Scratch* s, const GuardPlant* r, GuardSeat* t, unsigned bslot, int bfx, int bfz,
                         unsigned mc, int gx, int gz, int r2min, int r2max, int cur_x, int cur_z)
{
    const uint8_t* pass = mc < WQ_MCLASSES ? class_map(e, s, r, mc) : NULL;
    GgSeated seated[GUARD_SEATS];
    GgBox box = { r->bx0, r->bz0, r->bw, r->bh };
    int ns = 0, sx, sz, i, wx = cur_x, wz = cur_z;
    build_taken(e, r, s, bslot);
    if (t->avoidx != INT16_MIN) mark_rect(s->taken, r, t->avoidx, t->avoidz, t->avoidx, t->avoidz, 1);
    for (i = 0; i < GUARD_SEATS && pass; i++) {
        const GuardSeat* o = &s_seat[i];
        const char* ob;
        int ofx, ofz;
        unsigned omc;
        if (o->state != GS_SEATED || o == t || !(ob = unit_at(e, o->builder))) continue;
        if (!builder_foot(e, ob, &ofx, &ofz, &omc)) continue;
        if (o->x < r->bx0 || o->z < r->bz0 || o->x + ofx > r->bx0 + r->bw || o->z + ofz > r->bz0 + r->bh) continue;
        seated[ns].x = o->x; seated[ns].z = o->z; seated[ns].fx = ofx; seated[ns].fz = ofz;
        seated[ns].pass = omc < WQ_MCLASSES ? class_map(e, s, r, omc) : NULL;
        if (seated[ns].pass) ns++;
    }
    if (pass && gg_seat(&box, bfx, bfz, pass, s->taken, gx, gz, r2min, r2max, seated, ns, SEAT_BUDGET, &s->wk,
                        s->taken2, &sx, &sz)) {
        t->state = GS_SEATED;
        t->x = (int16_t)sx; t->z = (int16_t)sz;
        t->moved = e->now;
        s_diag.seated++;
        tagpu_logf("guard: seat builder=%u plant=%u at=%d,%d band=%d..%d seated=%d lanes=%u", bslot, (unsigned)r->slot,
                   sx, sz, r2min, r2max, ns, (unsigned)r->nlane);
        return;
    }
    if (pass) {
        /* a builder outside the box waits at the box's nearest place to it, so it waits near the plant */
        const int ax = cur_x < r->bx0 ? r->bx0 : cur_x > r->bx0 + r->bw - bfx ? r->bx0 + r->bw - bfx : cur_x;
        const int az = cur_z < r->bz0 ? r->bz0 : cur_z > r->bz0 + r->bh - bfz ? r->bz0 + r->bh - bfz : cur_z;
        wx = ax; wz = az;
        gg_wait_spot(&box, ax, az, bfx, bfz, pass, s->taken, 8, &wx, &wz);
    }
    if (t->x != wx || t->z != wz)
        tagpu_logf("guard: wait builder=%u plant=%u at=%d,%d band=%d..%d seated=%d", bslot, (unsigned)r->slot, wx, wz,
                   r2min, r2max, ns);
    t->state = GS_WAITING;
    t->x = (int16_t)wx; t->z = (int16_t)wz;
    s_diag.waiting++;
}

/* every seat of plant row r (type g) that no longer holds -- out of its builder's band round the frame's centre,
   on a structure's footprint (`foot`, mark_footprints) or on a lane -- goes back to waiting */
static void recheck_seats(const Eng* e, const GuardPlant* r, const GuardType* g, const uint8_t* foot)
{
    const int prow = (int)(r - s_plant);
    int i;
    for (i = 0; i < GUARD_SEATS; i++) {
        GuardSeat* o = &s_seat[i];
        const char* ob;
        int ofx, ofz, lo, hi;
        unsigned omc;
        WQ_DEF od;
        if (o->state != GS_SEATED || o->plant != prow || !(ob = unit_at(e, o->builder))) continue;
        if (!builder_foot(e, ob, &ofx, &ofz, &omc) || !e->q.ops->def(&e->q, (int)type_of(e, ob), &od)) {
            o->state = GS_WAITING;
            continue;
        }
        lo = g->rin_max / 16;
        hi = (od.reach + g->rin_min) / 16;
        if (!seat_holds(r, o, ofx, ofz, foot, gg_goal_place(r->spot_x, ofx), gg_goal_place(r->spot_z, ofz), lo * lo,
                        hi * hi))
            o->state = GS_WAITING;
    }
}

/* every seat, of any plant, that a lane now covers goes back to waiting: a re-plan moves lanes over other plants'
   seats as well as its own */
static void seats_off_lanes(const Eng* e)
{
    int i, k, m;
    for (i = 0; i < GUARD_SEATS; i++) {
        GuardSeat* o = &s_seat[i];
        const char* ob;
        int ofx, ofz, hit = 0;
        unsigned omc;
        if (o->state != GS_SEATED || !(ob = unit_at(e, o->builder)) || !builder_foot(e, ob, &ofx, &ofz, &omc))
            continue;
        for (k = 0; k < GUARD_PLANTS && !hit; k++)
            if (s_plant[k].used)
                for (m = 0; m < s_plant[k].nlane && !hit; m++)
                    hit = gg_overlaps(&s_plant[k].lane[m], o->x, o->z, ofx, ofz);
        if (hit) o->state = GS_WAITING;
    }
}

/* THE ARM. A seated guard's arm stays open from one product to the next. Stock folds it after every unit: the
   HelpBuild node's destructor starts StopBuilding when the node's +0x42 has bit 0x400000, which only StartBuilding
   0x438590 sets, and the next unit's HelpBuild starts StartBuilding again (engine/cob.md, *The scripts a unit's
   work starts*). Here the pair is skipped together, so the script sees one long build:
   - HELD: a HelpBuild node destroyed while its builder sits on its seat, still guarding a live plant, has the bit
     cleared before the destructor tests it (arm_dtor), and the row holds the open arm (t->arm).
   - TAKEN BACK: the next StartBuilding of that builder, for a HelpBuild on its plant's frame at the heading of
     the last one, is not started; the node gets the bit, so it owns the open arm again and folds it as stock
     (arm_start).
   - FOLDED, through the engine's own 0x4385F0 so the broadcast to the other players goes with it: before any
     other StartBuilding of that builder, and before a skipped start would wait on a cleared build stance
     (arm_start); before the order run dispatches any handler of the builder's but its guard's on its plant and a
     HelpBuild on its plant's frame (arm_dispatch); when its guard order's node is destroyed -- another order,
     the plant's death, the unit changing hands (arm_dtor); at a visit that finds it off its seat or no longer
     seated, before the home move that would walk it (guard_visit); when its plant has had no build at its head
     for ARM_GRACE ticks (guard_visit); and when its row is freed with the builder alive (rows_gc).
   An open arm therefore never travels: a guard moves only through an order's handler, and the only handlers that
   run while it is held are its guard's, which moves it only to its seat, and the plant's HelpBuild, whose band
   holds the seat. A dead builder needs no
   fold. The feature runs on the builder's owner's machine only; the destructor and StartBuilding run on that
   machine for its own nodes, and the other machines see the arm through the broadcasts, which are skipped or
   sent together with the starts. */
/* ticks a held arm stays open on a plant with nothing at its head, for a next unit queued by hand: the gap from
   one unit's end to the next one's StartBuilding on a stock plant plus a margin (MEASURED 2026-11-18, ARMVP making
   Flashes for a seated ARMCK, gamespeed 10, two games: 61 to 96 ticks with the yard clear, 241 with a product
   waiting in it; that one waits behind a queued build, which holds the arm anyway). Counted from the first visit that
   finds the plant idle, so the fold comes at the first visit at or past it, within the guard's 30-tick timer */
#define ARM_GRACE 150

typedef void (__attribute__((stdcall))* FoldFn)(char* unit, char* node);

static void arm_fold(const Eng* e, GuardSeat* t, const char* why)
{
    char node[0x48];
    char* b;
    if (!t->arm) return;
    arm_drop(t);
    if (!(b = unit_at(e, t->builder)) || !alive(b) || birth_of(t->builder) != t->birth) return;
    memset(node, 0, sizeof node);
    *(uint32_t*)(node + N_FLAGS) = NF_STARTED;       /* 0x4385F0 reads the node's +0x42 and nothing else */
    ((FoldFn)(uintptr_t)VA_FOLD)(b, node);
    tagpu_logf("guard: arm folded builder=%u why=%s tick=%d", (unsigned)t->builder, why, (int)e->now);
}

/* plant p's BuildingBuild node at the head of its list, or NULL: it stays there between two units of a queue,
   its frame +0x16 NULL until the next one is made */
static const char* plant_build(const char* p)
{
    const char* head = *(const char* const*)(p + U_ORDERS);
    return head && *(const uint8_t*)(head + N_ACTION) == action_of(S_BUILDINGBLD) ? head : NULL;
}

/* the frame plant p is building, or NULL */
static const char* plant_frame(const char* p)
{
    const char* n = plant_build(p);
    return n ? *(const char* const*)(n + N_TARGET) : NULL;
}

/* the row of a seated builder standing on its seat that still guards its live plant, or NULL; *pp the plant */
static GuardSeat* seated_row(const Eng* e, const char* b, const char** pp)
{
    const GuardPlant* r;
    GuardSeat* t;
    const char* p;
    int bslot = slot_of(e, b);
    if (bslot < 0 || !alive(b) || !(t = seat_row((unsigned)bslot, birth_of((unsigned)bslot), 0))) return NULL;
    if (t->state != GS_SEATED || t->plant >= GUARD_PLANTS) return NULL;
    if (*(const int16_t*)(b + U_CELLX) != t->x || *(const int16_t*)(b + U_CELLZ) != t->z) return NULL;
    r = &s_plant[t->plant];
    if (!r->used || !(p = unit_at(e, r->slot)) || !alive(p) || birth_of(r->slot) != r->birth) return NULL;
    if (!still_guards(e, t, p, action_of(S_FOLLOW))) return NULL;
    *pp = p;
    return t;
}

/* whether unit u stands on the frame's centre plant row r keeps */
static int at_spot(const Eng* e, const GuardPlant* r, const char* u)
{
    int32_t x, z;
    if (!r->spot_known || slot_of(e, u) < 0) return 0;
    memcpy(&x, u + U_XFIX, 4);
    memcpy(&z, u + U_YFIX, 4);
    return x == r->spot_x && z == r->spot_z;
}

/* the destructor's bit test: regs is the pushad frame, regs[1] esi the node */
static void __cdecl arm_dtor(unsigned* regs)
{
    char* node = (char*)(uintptr_t)regs[1];
    const unsigned char act = *(const uint8_t*)(node + N_ACTION);
    char* b = *(char**)(node + N_UNIT);
    const char* p;
    GuardSeat* t;
    Eng e;
    int bslot;
    if (!s_install.arm || tagpu_puppet_on() || !eng_open(&e) || (bslot = slot_of(&e, b)) < 0) return;
    if (!(t = seat_row((unsigned)bslot, birth_of((unsigned)bslot), 0))) return;   /* nearly every node: no row */
    if (act == action_of(S_FOLLOW)) {
        arm_fold(&e, t, "guard-ended");
        return;
    }
    if (act != action_of(S_HELPBUILD) || !(*(const uint32_t*)(node + N_FLAGS) & NF_STARTED)) return;
    if (seated_row(&e, b, &p) != t) return;
    *(uint32_t*)(node + N_FLAGS) &= ~NF_STARTED;
    if (!t->arm) { t->arm = 1; s_armed++; }
    t->idle = 0;
    tagpu_logf("guard: arm held builder=%d tick=%d", bslot, (int)e.now);
}

/* the order run's handler dispatch: regs[1] esi the node about to run. A held arm is folded before any handler
   but its guard's on its plant and a HelpBuild on its plant's frame runs: an order put in front of the guard --
   the attack reaction 0x43B1F0 at 0x4063F5, the repair branch, any other insert -- is run by the same controller
   loop, in the same tick (result 3's wait goes on at 0x43B99F to the new head), so this is the first place it can
   move the builder */
static void __cdecl arm_dispatch(unsigned* regs)
{
    const char* node = (const char*)(uintptr_t)regs[1];
    char* b;
    const char* p;
    unsigned char act;
    GuardSeat* t;
    Eng e;
    int bslot;
    if (!s_armed || tagpu_puppet_on() || !eng_open(&e)) return;
    b = *(char* const*)(node + N_UNIT);
    if ((bslot = slot_of(&e, b)) < 0 || !(t = seat_row((unsigned)bslot, birth_of((unsigned)bslot), 0)) || !t->arm)
        return;
    act = *(const uint8_t*)(node + N_ACTION);
    if (seated_row(&e, b, &p) == t) {
        const char* tg = *(char* const*)(node + N_TARGET);
        if (act == action_of(S_FOLLOW) && tg == p) return;
        /* a HelpBuild walks only toward the goal its state 0 makes, and that goal copies the target's x and z by
           value (0x40404E -> 0x438A00, 0x438A2A..0x438A42 -> 0x44D3B0): later states never move it. So in state 0
           the arm is kept only when the target stands on the frame centre the row keeps -- the frame being built,
           or the unit finished this very tick, which the assist finds at the plant's head (0x406668) as its last
           help ends -- whose band the seat was chosen in: the builder walks no step. Past state 0 its goal was
           checked here when it was made, and a finished target only ends it (0x40408E) */
        if (act == action_of(S_HELPBUILD) && tg &&
            (*(const uint8_t*)(node + N_STATE) != 0 || at_spot(&e, &s_plant[t->plant], tg)))
            return;
        {
            float tn = -1.0f;
            if (tg && slot_of(&e, tg) >= 0) memcpy(&tn, tg + U_NANO, 4);
            tagpu_logf("guard: arm other order builder=%d action=%u target=%d build=%.3f", bslot, (unsigned)act,
                       tg ? slot_of(&e, tg) : -1, (double)tn);
        }
    } else
        tagpu_logf("guard: arm other order builder=%d action=%u not seated", bslot, (unsigned)act);
    arm_fold(&e, t, "other-order");
}

/* StartBuilding's entry: regs is the pushad frame, regs[3] the esp at the entry (return address, unit, node,
   heading). 1 skips the start: the node is given the bit, as StartBuilding would, and the stub returns */
static int __cdecl arm_start(unsigned* regs)
{
    const uint32_t* arg = (const uint32_t*)(uintptr_t)regs[3];
    char* b = (char*)(uintptr_t)arg[1];
    char* node = (char*)(uintptr_t)arg[2];
    const uint16_t heading = (uint16_t)arg[3];
    const char* p;
    GuardSeat* t;
    Eng e;
    int bslot, help;
    if (!s_install.arm || tagpu_puppet_on() || !eng_open(&e) || (bslot = slot_of(&e, b)) < 0) return 0;
    if (!(t = seat_row((unsigned)bslot, birth_of((unsigned)bslot), 0))) return 0;
    help = *(const uint8_t*)(node + N_ACTION) == action_of(S_HELPBUILD) && seated_row(&e, b, &p) == t &&
           plant_frame(p) != NULL && *(char* const*)(node + N_TARGET) == plant_frame(p);
    /* the skipped start leaves the build stance as the held arm left it: HelpBuild's state 2 goes on only with
       INBUILDSTANCE set, which only a script SET writes, so a stance some other script cleared is folded and
       started again, never waited on */
    if (t->arm && help && heading == t->heading && (*(const uint8_t*)(b + U_STANCE) & 1)) {
        *(uint32_t*)(node + N_FLAGS) |= NF_STARTED;
        arm_drop(t);
        tagpu_logf("guard: arm kept builder=%d tick=%d", bslot, (int)e.now);
        return 1;
    }
    arm_fold(&e, t, !help ? "other-build" : heading != t->heading ? "heading" : "stance");
    if (help) t->heading = heading;
    return 0;
}

/* rows whose unit is gone: a plant row whose slot holds no live unit of its birth and type, and with it every
   builder row of that plant; and a builder row whose builder is gone or no longer guards its plant, so that its
   seat or wait spot stops holding ground for every plant's search */
static void rows_gc(const Eng* e, unsigned char follow)
{
    int i, k;
    for (i = 0; i < GUARD_PLANTS; i++) {
        GuardPlant* r = &s_plant[i];
        const char* p;
        if (!r->used) continue;
        if ((p = unit_at(e, r->slot)) && alive(p) && birth_of(r->slot) == r->birth && type_of(e, p) == r->type)
            continue;
        for (k = 0; k < GUARD_SEATS; k++)
            if (s_seat[k].state != GS_FREE && s_seat[k].plant == i) {
                arm_fold(e, &s_seat[k], "plant-gone");
                seat_clear(&s_seat[k]);
            }
        memset(r, 0, sizeof *r);
    }
    for (k = 0; k < GUARD_SEATS; k++) {
        GuardSeat* t = &s_seat[k];
        const char* p;
        if (t->state == GS_FREE || t->plant >= GUARD_PLANTS) continue;
        p = s_plant[t->plant].used ? unit_at(e, s_plant[t->plant].slot) : NULL;
        if (!p || !still_guards(e, t, p, follow)) {
            arm_fold(e, t, "row-freed");
            seat_clear(t);
        }
    }
}

/* THE NEIGHBOURS: every other finished plant of a ground product whose footprint stands in the box gets a row and
   its lanes, so that no seat or wait spot of this plant stands in another plant's way out. At most 8 a replan,
   each planned in its own box. */
static void plan_neighbours(Eng* e, const GuardPlant* r, unsigned self)
{
    unsigned seen[8];
    int n = 0, x, z, k;
    for (z = r->bz0; z < r->bz0 + r->bh && n < 8; z++)
        for (x = r->bx0; x < r->bx0 + r->bw && n < 8; x++) {
            const unsigned u = cell_unit(e, x, z);
            char* q = u ? unit_at(e, u) : NULL;
            const GuardType* g;
            GuardPlant* o;
            Scratch s;
            int fx, fz;
            float nano;
            if (!q || u == self || !alive(q) || !structure(q)) continue;
            for (k = 0; k < n && seen[k] != u; k++) {}
            if (k < n) continue;
            seen[n++] = u;
            memcpy(&nano, q + U_NANO, 4);
            if (nano != 0.0f || !(g = type_row(type_of(e, q), side_of(e, q))) || !g->nclass) continue;
            fx = *(const int16_t*)(q + U_FX); fz = *(const int16_t*)(q + U_FZ);
            if (fx < 1 || fz < 1 || fx > WQ_FOOT_MAX || fz > WQ_FOOT_MAX) continue;
            if (!(o = plant_row(e, q, u, type_of(e, q), 1))) continue;
            if (!o->bw) plant_box(e, o, g, fx, fz);
            if (!scratch_get(&s, o->bw, o->bh)) continue;
            box_blocked(e, o, &s);
            o->sig = box_sig(e, o);
            o->sig_tick = e->now;
            plan_lanes(e, q, o, g, &s);
            memcpy(s.taken2, s.blocked, (size_t)o->bw * o->bh);
            mark_footprints(e, o, s.taken2);
            recheck_seats(e, o, g, s.taken2);
            tagpu_sim_scratch_free(s.block);
        }
}

static int __cdecl guard_visit(unsigned* regs)
{
    char* node = (char*)(uintptr_t)regs[4];       /* ebx */
    char* b = (char*)(uintptr_t)regs[0];          /* edi */
    char* p = (char*)(uintptr_t)regs[7];          /* eax */
    Eng e;
    Scratch s;
    const GuardType* g;
    GuardPlant* r;
    GuardSeat* t;
    unsigned mc, owner;
    int bslot, pslot, ptype, prow, pfx, pfz, bfx, bfz, gx, gz, r2min, r2max, replan, skip, cur_x, cur_z;
    unsigned char follow;
    WQ_DEF bd;
    float nano;
    if (tagpu_puppet_on() || !eng_open(&e)) return 0;
    if ((bslot = slot_of(&e, b)) < 0 || (pslot = slot_of(&e, p)) < 0) return 0;
    if (!alive(b) || !alive(p) || structure(b) || !structure(p)) return 0;
    if (tagpu_puppet_seat(*(const uint8_t*)(b + U_OWNER))) return 0;
    if (!e.q.ops->def(&e.q, (int)type_of(&e, b), &bd) || !(bd.flags & WQ_DF_BUILDER) || (bd.flags & WQ_DF_VTOL))
        return 0;
    memcpy(&nano, p + U_NANO, 4);
    if (nano != 0.0f) return 0;
    ptype = (int)type_of(&e, p);
    if (!(g = type_row((unsigned)ptype, side_of(&e, p)))) return 0;
    pfx = *(const int16_t*)(p + U_FX); pfz = *(const int16_t*)(p + U_FZ);
    if (pfx < 1 || pfz < 1 || pfx > WQ_FOOT_MAX || pfz > WQ_FOOT_MAX) return 0;
    s_diag.visits++;
    follow = *(const uint8_t*)(node + N_ACTION);
    rows_gc(&e, follow);
    if (!(r = plant_row(&e, p, (unsigned)pslot, (unsigned)ptype, 1))) { s_diag.full++; return 0; }
    prow = (int)(r - s_plant);
    if (!(t = seat_row((unsigned)bslot, birth_of((unsigned)bslot), 1))) { s_diag.full++; return 0; }
    if (t->plant != prow) {                        /* a builder that guarded another plant starts over */
        t->plant = (uint16_t)prow;
        t->state = GS_WAITING;
        t->moved = e.now;
        t->avoidx = t->avoidz = INT16_MIN;
    }

    if (!builder_foot(&e, b, &bfx, &bfz, &mc)) return 0;   /* before update_spot and the signature store */
    replan = update_spot(&e, p, r, action_of(S_BUILDINGBLD));
    if (!r->bw) { plant_box(&e, r, g, pfx, pfz); replan = 1; }
    if (r->sig_tick != e.now) {
        const uint32_t sig = box_sig(&e, r);
        if (sig != r->sig || !r->planned) replan = 1;
        r->sig = sig;
        r->sig_tick = e.now;
    }

    gx = gg_goal_place(r->spot_x, bfx);
    gz = gg_goal_place(r->spot_z, bfz);
    r2min = (g->rin_max / 16) * (g->rin_max / 16);
    r2max = ((bd.reach + g->rin_min) / 16) * ((bd.reach + g->rin_min) / 16);
    cur_x = *(const int16_t*)(b + U_CELLX);
    cur_z = *(const int16_t*)(b + U_CELLZ);

    /* a seat it has stood short of, on one cell, for STUCK_TICKS is given up: it waits, and that seat is avoided.
       Counted in GameTime, not in visits: the guard wakes on its move goal's events as well as on its 30-tick
       timer, so a count of visits is no measure of time */
    if (cur_x != t->lastx || cur_z != t->lastz) {
        t->lastx = (int16_t)cur_x;
        t->lastz = (int16_t)cur_z;
        t->moved = e.now;
    }
    if (t->state == GS_SEATED && (cur_x != t->x || cur_z != t->z) && e.now - t->moved >= STUCK_TICKS) {
        t->avoidx = t->x; t->avoidz = t->z;
        t->state = GS_WAITING;
        t->moved = e.now;
    }

    /* A BUILD SITE: a seat one covers is given up, and the search below takes the site as taken ground */
    owner = *(const uint8_t*)(b + U_OWNER);
    if (t->state == GS_SEATED && tagpu_scatter_covers(owner, e.now, t->x, t->z, bfx, bfz)) {
        tagpu_logf("guard: site builder=%u plant=%u seat=%d,%d given up tick=%d", (unsigned)bslot,
                   (unsigned)r->slot, t->x, t->z, (int)e.now);
        t->state = GS_WAITING;
        t->moved = e.now;
    }

    if (replan || t->state != GS_SEATED) {
        if (!scratch_get(&s, r->bw, r->bh)) {
            if (replan) r->planned = 0;          /* the next visit plans it again */
            return 0;
        }
        box_blocked(&e, r, &s);
        if (replan) {
            plan_lanes(&e, p, r, g, &s);
            plan_neighbours(&e, r, (unsigned)pslot);
            memcpy(s.taken2, s.blocked, (size_t)r->bw * r->bh);
            mark_footprints(&e, r, s.taken2);
            recheck_seats(&e, r, g, s.taken2);
            seats_off_lanes(&e);
        }
        if (t->state != GS_SEATED)
            seat_or_wait(&e, &s, r, t, (unsigned)bslot, bfx, bfz, mc, gx, gz, r2min, r2max, cur_x, cur_z);
        tagpu_sim_scratch_free(s.block);
    }
    /* the assist only from the seat itself: a help given on the way walks to the band's cheapest place, which can
       be the yard's mouth, not to the seat */
    skip = t->state != GS_SEATED || cur_x != t->x || cur_z != t->z;
    if (t->arm) {                                  /* THE ARM: never open off the seat, nor long on an idle plant */
        if (skip)
            arm_fold(&e, t, "off-seat");
        else if (plant_build(p))
            t->idle = 0;
        else if (!t->idle)
            t->idle = e.now ? e.now : 1;
        else if (e.now - t->idle >= ARM_GRACE)
            arm_fold(&e, t, "queue-empty");
    }
    /* a wait spot's goal is reached two cells short of it, which can be on the site still: from a site the
       builder walks to the place itself */
    aim(node, p, t->x, t->z, bfx, bfz,
        t->state == GS_SEATED || tagpu_scatter_covers(owner, e.now, cur_x, cur_z, bfx, bfz) ? 16 : 64);
    return skip;
}

/* ---- install and level ---- */

static const unsigned char STOLEN[8] = { 0x8B, 0x43, 0x16, 0x8B, 0x48, 0x5C, 0x3B, 0xCD };

static const unsigned char START_STOLEN[7] = { 0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10 };
static const unsigned char DTOR_STOLEN[7] = { 0xF7, 0x46, 0x42, 0x00, 0x00, 0x40, 0x00 };
static const unsigned char DISP_STOLEN[6] = { 0x8B, 0x0D, 0x44, 0x23, 0x51, 0x00 };

/* THE ARM's three hooks. All act only once all are in (s_install.arm): a held arm with no StartBuilding hook would
   never be taken back, and one with no dispatch hook could travel.
   0x438590: pushad; push esp; call arm_start; add esp,4; test eax,eax; popad; jz +3; ret 0xC; push ebx; push esi;
   push edi; mov edi,[esp+0x10]; jmp 0x438597.
   0x43A21E: pushad; push esp; call arm_dtor; add esp,4; popad; test [esi+0x42],0x400000; jmp 0x43A225.
   0x43B865: pushad; push esp; call arm_dispatch; add esp,4; popad; mov ecx,[0x512344]; jmp 0x43B86B (no branch
   lands inside the six bytes past their first; the recorder's observer is at 0x43B878, after it) */
static void arm_install(void)
{
    unsigned char* p;
    if (!tagpu_detour_bytes_ok(VA_START, START_STOLEN, (int)sizeof START_STOLEN) ||
        !tagpu_detour_bytes_ok(VA_DTOR_TEST, DTOR_STOLEN, (int)sizeof DTOR_STOLEN) ||
        !tagpu_detour_bytes_ok(VA_DISPATCH, DISP_STOLEN, (int)sizeof DISP_STOLEN)) {
        tagpu_log("guard: arm NOT installed: 0x438590, 0x43A21E or 0x43B865 does not hold the retail code");
        return;
    }
    if (!(s_start_stub = tagpu_detour_stub()) || !(s_dtor_stub = tagpu_detour_stub()) ||
        !(s_disp_stub = tagpu_detour_stub())) {
        if (s_start_stub) tagpu_detour_stub_free(s_start_stub);
        if (s_dtor_stub) tagpu_detour_stub_free(s_dtor_stub);
        s_start_stub = s_dtor_stub = NULL;
        tagpu_log("guard: arm NOT installed: no stub memory");
        return;
    }
    p = s_start_stub;
    *p++ = 0x60; *p++ = 0x54;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(uintptr_t)arm_start); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x61;
    *p++ = 0x74; *p++ = 0x03;
    *p++ = 0xC2; *p++ = 0x0C; *p++ = 0x00;
    memcpy(p, START_STOLEN, sizeof START_STOLEN); p += sizeof START_STOLEN;
    *p++ = 0xE9; tagpu_detour_rel(p, VA_START_NEXT); p += 4;
    p = s_dtor_stub;
    *p++ = 0x60; *p++ = 0x54;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(uintptr_t)arm_dtor); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x61;
    memcpy(p, DTOR_STOLEN, sizeof DTOR_STOLEN); p += sizeof DTOR_STOLEN;
    *p++ = 0xE9; tagpu_detour_rel(p, VA_DTOR_NEXT); p += 4;
    p = s_disp_stub;
    *p++ = 0x60; *p++ = 0x54;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(uintptr_t)arm_dispatch); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x61;
    memcpy(p, DISP_STOLEN, sizeof DISP_STOLEN); p += sizeof DISP_STOLEN;
    *p++ = 0xE9; tagpu_detour_rel(p, VA_DISP_NEXT); p += 4;
    if (!tagpu_detour_land(VA_DISPATCH, s_disp_stub, (int)sizeof DISP_STOLEN)) {
        tagpu_detour_stub_free(s_disp_stub); tagpu_detour_stub_free(s_dtor_stub); tagpu_detour_stub_free(s_start_stub);
        s_disp_stub = s_dtor_stub = s_start_stub = NULL;
        tagpu_log("guard: arm NOT installed: 0x43B865 could not be written");
        return;
    }
    if (!tagpu_detour_land(VA_DTOR_TEST, s_dtor_stub, (int)sizeof DTOR_STOLEN)) {
        tagpu_detour_stub_free(s_dtor_stub); tagpu_detour_stub_free(s_start_stub);
        s_dtor_stub = s_start_stub = NULL;
        tagpu_log("guard: arm NOT installed: 0x43A21E could not be written (the dispatch hook stays inert)");
        return;
    }
    if (!tagpu_detour_land(VA_START, s_start_stub, (int)sizeof START_STOLEN)) {
        tagpu_detour_stub_free(s_start_stub);
        s_start_stub = NULL;
        tagpu_log("guard: arm NOT installed: 0x438590 could not be written (the destructor hook stays inert)");
        return;
    }
    s_install.arm = 1;
    tagpu_log("guard: installed: the arm kept open 0x438590 0x43A21E 0x43B865");
}

/* mov eax,[ebx+0x16]; mov ecx,[eax+0x5c]; pushad; push esp; call guard_visit; add esp,4; test eax,eax; popad
   (popad leaves the flags); jnz 0x4066AC; cmp ecx,ebp; jmp 0x406553 -- the je there reads the cmp */
void tagpu_guard_init(void)
{
    unsigned char* p;
    if (!tagpu_detour_bytes_ok(VA_SITE, STOLEN, (int)sizeof STOLEN)) {
        tagpu_log("guard: NOT installed: 0x40654B does not hold the retail assist");
        return;
    }
    if (!(s_stub = tagpu_detour_stub())) {
        tagpu_log("guard: NOT installed: no stub memory");
        return;
    }
    p = s_stub;
    memcpy(p, STOLEN, 6); p += 6;
    *p++ = 0x60;
    *p++ = 0x54;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(uintptr_t)guard_visit); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x61;
    *p++ = 0x0F; *p++ = 0x85; tagpu_detour_rel(p, VA_HOME); p += 4;
    *p++ = 0x3B; *p++ = 0xCD;
    *p++ = 0xE9; tagpu_detour_rel(p, VA_ASSIST_JE); p += 4;
    if (!tagpu_detour_land(VA_SITE, s_stub, (int)sizeof STOLEN)) {
        tagpu_detour_stub_free(s_stub);
        s_stub = NULL;
        tagpu_log("guard: NOT installed: 0x40654B could not be written");
        return;
    }
    s_install.installed = 1;
    tagpu_log("guard: installed: the guard's assist 0x40654B");
    arm_install();
}

void tagpu_guard_level_init(void)
{
    Eng e;
    uint16_t* menu;
    unsigned side, sides, t;
    memset(s_plant, 0, sizeof s_plant);
    memset(s_seat, 0, sizeof s_seat);
    s_armed = 0;
    memset(&s_diag, 0, sizeof s_diag);
    s_ntype = 0;
    s_reach = 0;
    s_yard_max = 0;
    if (!s_install.installed || !eng_open(&e)) return;
    sides = *(const unsigned*)(e.ta + M_SIDES);
    if (sides > 8) sides = 8;
    for (t = 1; t < e.ndefs; t++) {
        WQ_DEF d;
        if (e.q.ops->def(&e.q, (int)t, &d) && (d.flags & WQ_DF_ASSIST) && (d.flags & WQ_DF_MOVER) &&
            !(d.flags & WQ_DF_VTOL) && d.reach > s_reach)
            s_reach = d.reach > REACH_MAX ? REACH_MAX : d.reach;
        if (e.q.ops->def(&e.q, (int)t, &d) && d.yard && !(d.flags & WQ_DF_MOVER) && d.fx <= WQ_FOOT_MAX &&
            d.fz <= WQ_FOOT_MAX) {
            if (d.fx > s_yard_max) s_yard_max = d.fx;
            if (d.fz > s_yard_max) s_yard_max = d.fz;
        }
    }
    if (!(menu = (uint16_t*)tagpu_sim_scratch(MENU_CAP * sizeof *menu))) return;
    for (side = 0; side < sides; side++)
        for (t = 1; t < e.ndefs; t++) {
            WQ_DEF d;
            int n;
            if (!e.q.ops->def(&e.q, (int)t, &d) || !(d.flags & WQ_DF_BUILDER) || (d.flags & WQ_DF_MOVER) || !d.yard)
                continue;
            if ((n = tagpu_buildmenu_types(e.ta, e.defs, e.ndefs, t, (unsigned char)side, menu, MENU_CAP)) > 0)
                type_add(&e, t, side, menu, n);
        }
    tagpu_sim_scratch_free(menu);
    tagpu_logf("guard: level plant types=%u sides=%u reach=%d", s_ntype, sides, (int)s_reach);
}

/* ---- for build scatter ---- */
SIM_FEATURE(guard_build_site, SIM_PUPPET_OWNER, "build-scatter-guard stock-build-scatter-guard");

int tagpu_guard_seats(unsigned slot)
{
    Eng e;
    const GuardPlant* r;
    const char* p;
    const char* head;
    unsigned char follow;
    int i;
    if (!s_install.installed || tagpu_puppet_on() || !eng_open(&e) || !slot || slot >= e.nslots) return 0;
    for (i = 0; i < GUARD_SEATS; i++)
        if (s_seat[i].state != GS_FREE && s_seat[i].builder == slot) break;
    if (i == GUARD_SEATS || s_seat[i].plant >= GUARD_PLANTS) return 0;
    r = &s_plant[s_seat[i].plant];
    if (!r->used || !(p = unit_at(&e, r->slot)) || !alive(p) || birth_of(r->slot) != r->birth) return 0;
    follow = action_of(S_FOLLOW);
    if (!still_guards(&e, &s_seat[i], p, follow)) return 0;            /* the builder's own incarnation too */
    head = *(const char* const*)(unit_at(&e, slot) + U_ORDERS);
    return head && *(const uint8_t*)(head + N_ACTION) == follow && *(char* const*)(head + N_TARGET) == p ? 1 : 2;
}

void tagpu_guard_mark(int x0, int z0, int w, int h, unsigned char* t)
{
    const GuardPlant box = { .bx0 = (int16_t)x0, .bz0 = (int16_t)z0, .bw = (int16_t)w, .bh = (int16_t)h };
    Eng e;
    int i, k;
    if (!s_install.installed || w < 1 || h < 1 || w > INT16_MAX || h > INT16_MAX || !eng_open(&e)) return;
    for (i = 0; i < GUARD_PLANTS; i++)
        for (k = 0; s_plant[i].used && k < s_plant[i].nlane; k++)
            mark_rect(t, &box, s_plant[i].lane[k].x0, s_plant[i].lane[k].z0, s_plant[i].lane[k].x1,
                      s_plant[i].lane[k].z1, 1);
    for (i = 0; i < GUARD_SEATS; i++) {
        const GuardSeat* o = &s_seat[i];
        const char* b;
        int fx, fz;
        unsigned mc;
        if (o->state == GS_FREE || !(b = unit_at(&e, o->builder)) || !builder_foot(&e, b, &fx, &fz, &mc)) continue;
        mark_rect(t, &box, o->x, o->z, o->x + fx - 1, o->z + fz - 1, 1);
    }
}
