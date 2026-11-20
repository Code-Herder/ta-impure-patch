#include "tagpu_simreg.h"
SIM_FEATURE(build_scatter, SIM_PUPPET_OWNER, "build-scatter stock-build-scatter");
/* BUILD SCATTER: a builder that reaches its build site and finds its owner's idle units standing on it has them
   moved off, each to the nearest place it can reach and stand on (research/notes/build-scatter-plan.md; the
   module note research/notes/modules/build-scatter.md). The geometry is tagpu_scattergeo.c's; this file reads the
   engine, keeps the rows and gives the moves.

   THE ENGINE [DISASSEMBLED 2026-11-19; research/notes/engine/orders.md, *MobileBuild's blocked site*].
   MobileBuild 0x403A20's state 1 tests its site with 0x47DB70(def, 0, cell, 1) at 0x403CC7, which for a
   structure asks the yard test 0x47D2E0 with no unit ignored and no player, so every unit on a cell the
   structure stamps refuses it. Refused, it reaches 0x403CD0: the retry counter node+0x3E, at 0 the reply "Waiting
   for target area to clear" (0x403CDF), then the counter's increment and a 30-tick wait (0x403CE4..0x403CEF), and
   past 10 "Target area was blocked" and result 8, the order abandoned (0x403D03..0x403D15). VTOL_MobileBuild
   0x413D80 has the same branch at 0x41400D. Both handlers run only inside the order run 0x43B7C0, which the unit
   tick 0x48AD30 calls for a unit of a local record (type 1 or 2, 0x48AEB5..0x48AECD).

   THE ORDERING THE SAFETY RESTS ON. The stubs at the two branches write only this file's own tables and the
   retry counter of the node the handler is running (THE HOLD). Order lists are changed in one place,
   tagpu_scatter_tick, which the step's players phase calls after the unit tick has returned
   (tagpu_allysight.c, players_phase, the call at 0x49555F): no order handler is on the stack, the controllers
   walk no list, and the thread is the game thread, the one that links, unlinks and frees order nodes. A move is
   given there through the root a player's click uses, 0x43ADC0 with no Shift (WQ_CMD_NEW), to an idle unit:
   what the engine then does to that unit's list is what it does at every click, which a player makes between two
   steps on the same thread (0x499200 before the frame 0x496790).

   A ROW OWNS ONE ORDER, NEVER A UNIT. A row names its unit by slot and birth and the move it gave by value: the
   action byte and the point. At every step a row is live only while that slot holds that incarnation, alive and
   its owner's, and the head of its list is still that move; anything else frees the row and nothing is done to
   the unit. So a player's order always wins: it replaces the head, the row goes, and the unit is judged again
   by where it stands only when it is idle on a refused site. No address is kept: a dead unit, a recycled slot
   and a destroyed node all fail the same test.

   IDLE is what gets a move, by the unit's list alone: no node; a head carrying 0x4000 whose action is its
   type's own default order (the byte def+0x230 the controller makes the node from when the list empties,
   0x43B9CA..0x43BA0D) -- not merely 0x4000, which every put-first copies onto the new head (0x43ACF2..0x43ACFF),
   so an idle unit's reaction attack and a Paralyze carry it too; or a Park that is the list's only node, which
   never ends (engine/orders.md, *Park*). And only a finished ground mover that nothing carries, of the
   builder's owner: a structure, an aircraft, a frame, a passenger and every other player's unit stay, and the
   builder waits and gives up on them as stock.

   THE HOLD. While a unit this file moved is still on the site and has changed cell within STUCK_TICKS, or
   another of the owner's units is crossing it under a move, the builder's refusal does not count: the stub takes
   one off the counter before stock adds one, never below 1, so the first refusal's reply is said once. A site
   with nothing moving on it counts as stock and is abandoned as stock.

   THE CURSOR'S SITE. The build cursor's test (0x47D2E0 from 0x4198C6, through tagpu_patches.c's
   build_site_test) refuses a cell the structure stamps when its unit word names a unit other than the ignore
   argument's (0x47D54C..0x47D561, `cmp cx,[esp+0x48]` at 0x47D554). tagpu_scatter_site_test asks stock first and,
   refused, asks once more with SITE_MOVES set in that argument: the stub at the compare then passes a cell whose
   unit is one this file would move for the placing player (movable, the one rule both the cursor and the tick
   apply), and every other test of the site is stock's. The flag rides in the call's own argument, so nothing is
   kept between calls and a nested call cannot see another's: the compare reads the argument's low word, the
   test's tail its low byte (0x47D7F2..0x47D7F6), every stock caller passes a literal 0 or a unit number, and
   only tagpu_scatter_site_test sets bit 16. A site so accepted is ordered as any other, and the builder's own
   test, which ignores nobody, refuses it on arrival: that refusal is what moves the units.

   STATE, all SIM_STATE: the sites s_site (the builder by slot and birth, its site, the step it was last
   refused in, whether the next refusal is held) and the rows s_row (the unit by slot and birth, the move, the
   site and the place it was sent to). The level's widest yard s_yard_max is SIM_CONTENT. s_ui is the cursor's
   last answer, which only the frame packet reads.

   BOUNDS. A unit pointer the engine hands a stub is tested to be a record of the unit array before a byte of it
   is read, and its owner to be a seat a view opens for (tagpu_worldq_open: the map, the defs, the array and the
   seat's block bounded, a puppet seat refused); a def pointer to be a record of the def array; the cell to hold
   the footprint on the map; every slot read from a cell to be below the slot count. Pointer tests are filters on
   values; the reads are the game thread's, inside the engine's own call or between two of them.

   THREADS. The rows are cleared and the yard read on the LOADER thread at the PLAYERS call (0x4919C8), before the
   game thread's first step; the stubs run on the game thread inside the order run, the tick on the game thread
   in the players phase. */

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "tagpu_detour.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_log.h"
#include "tagpu_puppet.h"
#include "tagpu_scatter.h"
#include "tagpu_scattergeo.h"
#include "tagpu_worldq.h"
#include "worldq.h"

#define VA_BLOCKED       0x00403CD0u   /* MobileBuild state 1: `mov eax,[esi+0x3e]; test eax,eax; jne 0x403D03` */
#define VA_BLOCKED_JNE   0x00403D03u
#define VA_BLOCKED_NEXT  0x00403CD7u
#define VA_VBLOCKED      0x0041400Du   /* VTOL_MobileBuild state 2: the same three, `jne 0x414044`              */
#define VA_VBLOCKED_JNE  0x00414044u
#define VA_VBLOCKED_NEXT 0x00414014u
#define VA_SITE_TEST     0x0047D2E0u   /* the yard test, stdcall (def, cell, ignore, player record), ret 0x10   */
#define VA_OCC           0x0047D554u   /* its occupant compare `cmp cx,[esp+0x48]`                              */
#define VA_OCC_NEXT      0x0047D559u   /* the `je 0x47D567` that reads the compare                              */
#define VA_OCC_PASS      0x0047D567u   /* the cell passes                                                       */
#define SITE_MOVES       0x10000u      /* in the test's ignore argument: THE CURSOR'S SITE                      */
#define VA_ACTION_OF     0x00438760u   /* thiscall (u8* out, const char* name): an order name's action byte     */
#define S_MOVE_GROUND    0x005052E4u   /* "MOVE_GROUND", the name the resolver's mode 2 passes it (0x43F845)    */

#define U_CELLX          0x76          /* s16 the footprint's first cell (engine/units.md, The footprint cell)  */
#define U_CELLZ          0x78
#define U_FX             0x7E          /* s16 the footprint, copied from the def (0x485AAA)                     */
#define U_FZ             0x80
#define MV_STAMP         0x26          /* u32 the mover's claim stamp: GameTime at its last change of cell      */
#define UD_DEFAULT       0x230         /* u8 the type's default order's action, 0 none (0x43B9D0)               */
#define N_OWNER          0x0E          /* the node's unit                                                       */
#define N_RETRY          0x3E          /* i32 MobileBuild's refusals of its site (0x403CE4..0x403CEA)           */
#define CELL_STRIDE      0x0D

#define SC_SITES         32
#define SC_ROWS          256
#define SC_GROW          12            /* cells the box reaches past the site on every side                    */
#define SC_PLAN          64            /* units one refusal looks at                                           */
#define SC_CLASSES       4             /* class maps one refusal keeps                                         */
#define SITE_TTL         61            /* steps a site outlives its last refusal: two of the 30-step waits      */
#define STUCK_TICKS      180           /* steps with no change of cell before a moved unit stops holding the
                                          builder: three of the pathfinder's 60-step re-searches               */

/* ---- the rows (SIM_STATE) ---- */

typedef struct ScSite {
    uint16_t builder, type;           /* the builder's slot; the product's def index                           */
    uint32_t birth;
    uint8_t  used, owner, fx, fz;
    int16_t  x0, z0;                  /* the site's first cell                                                 */
    uint8_t  hold, told, pad[2];      /* hold: the next refusal does not count; told: its outcome is logged    */
    int32_t  seen;                    /* GameTime of its last refusal                                          */
} ScSite;
/* the cells and the step are numbers whose dwords can take any value, an address's included, so the address scan
   passes over them while the replay compares them */
static const SimField SITE_LAYOUT[] = {
    SIM_FIELD(ScSite, builder, SIMF_ID, SIM_ID_UNIT_SLOT),
    SIM_FIELD(ScSite, type, SIMF_ID, SIM_ID_UNIT_TYPE),
    SIM_FIELD_AT(offsetof(ScSite, used), sizeof(ScSite) - offsetof(ScSite, used), SIMF_OPAQUE, SIM_ID_NONE),
    SIM_FIELD_END
};
static ScSite s_site[SC_SITES];
SIM_ROWS(s_site, SIM_STATE, SIM_ID_NONE, SITE_LAYOUT);

typedef struct ScRow {
    uint16_t unit;                    /* the moved unit's slot                                                 */
    uint8_t  used, owner;
    uint32_t birth;
    uint8_t  action, on, stuck, pad;  /* the move's action byte; on: it still stands on the site               */
    uint8_t  fx, fz, sfx, sfz;        /* its footprint; the site's                                             */
    int16_t  sx0, sz0;                /* the site's first cell                                                 */
    int16_t  dx, dz;                  /* the place it was sent to                                              */
    int32_t  pos[3];                  /* the move's point, 16.16                                               */
    int32_t  issued;                  /* GameTime the move was given                                           */
} ScRow;
static const SimField ROW_LAYOUT[] = {
    SIM_FIELD(ScRow, unit, SIMF_ID, SIM_ID_UNIT_SLOT),
    SIM_FIELD_AT(offsetof(ScRow, action), sizeof(ScRow) - offsetof(ScRow, action), SIMF_OPAQUE, SIM_ID_NONE),
    SIM_FIELD_END
};
static ScRow s_row[SC_ROWS];
SIM_ROWS(s_row, SIM_STATE, SIM_ID_NONE, ROW_LAYOUT);

/* the level's widest footprint side of a def with a yard, cells, at most WQ_FOOT_MAX */
static int32_t s_yard_max;
SIM_VAR(s_yard_max, SIM_CONTENT);

static unsigned char* s_stub;
SIM_CODE(s_stub);
static unsigned char* s_vstub;
SIM_CODE(s_vstub);
static unsigned char* s_ostub;
SIM_CODE(s_ostub);
static struct { uint8_t installed, site; } s_install;
SIM_VAR(s_install, SIM_INSTALL);

/* the build cursor's last answer was a site whose units will be moved: the frame packet's yellow square. The
   simulation never reads it */
static struct { uint8_t cursor_moves; } s_ui;
SIM_VAR(s_ui, SIM_PRIVATE);

/* diagnostics the simulation never reads */
static struct { uint32_t refusals, held, moved, noway, full; } s_diag;
SIM_VAR(s_diag, SIM_PRIVATE);

typedef unsigned char* (__attribute__((thiscall))* ActionOf)(void*, const char*);
typedef unsigned (__stdcall* SiteTest)(const void* def, unsigned cell, unsigned ignore, void* player);

/* ---- engine reads ---- */

/* one seat's view and the step it was opened in */
typedef struct View { WQ_ENGINE wc; WORLDQ q; int32_t now; int open; } View;

static int view_open(View* v, unsigned seat)
{
    v->open = seat < WQ_SEATS && tagpu_worldq_open(&v->wc, &v->q, (int)seat, WQ_SITE_ENGINE, NULL);
    if (v->open) memcpy(&v->now, v->wc.ta + OFF_GAMETIME, 4);
    return v->open;
}

/* the alive, not excluded unit at `slot`, of any owner, or NULL */
static char* unit_at(const View* v, unsigned slot)
{
    char* u;
    uint32_t st;
    if (!slot || slot >= v->wc.nslots) return NULL;
    u = v->wc.units + (size_t)slot * UNIT_STRIDE;
    memcpy(&st, u + U_STATE, 4);
    return (st & ST_ALIVE) && !(st & ST_EXCLUDED) ? u : NULL;
}

static int structure(const char* u) { return *(char* const*)(u + U_MOVER) == NULL; }

/* the def index of a unit, 0 when its def pointer is not a record of the def array */
static unsigned type_of(const View* v, const char* def)
{
    uintptr_t off;
    if (!def || def < v->wc.defs) return 0;
    off = (uintptr_t)(def - v->wc.defs);
    if (off % UDEF_STRIDE || off / UDEF_STRIDE >= v->wc.ndefs) return 0;
    return (unsigned)(off / UDEF_STRIDE);
}

/* the slot standing in slot A of cell (x, z), 0 none, off the map or not a slot */
static unsigned cell_unit(const View* v, int x, int z)
{
    uint16_t s;
    if (x < 0 || z < 0 || x >= v->wc.w || z >= v->wc.h) return 0;
    memcpy(&s, v->wc.grid + ((size_t)z * (size_t)v->wc.w + (size_t)x) * CELL_STRIDE, 2);
    return s < v->wc.nslots ? s : 0;
}

/* a mover's footprint as it stands, 0 when it is not 1..WQ_FOOT_MAX a side or leaves the map */
static int foot(const View* v, const char* u, int* x, int* z, int* fx, int* fz)
{
    *x = *(const int16_t*)(u + U_CELLX); *z = *(const int16_t*)(u + U_CELLZ);
    *fx = *(const int16_t*)(u + U_FX); *fz = *(const int16_t*)(u + U_FZ);
    return *fx >= 1 && *fz >= 1 && *fx <= WQ_FOOT_MAX && *fz <= WQ_FOOT_MAX && *x >= 0 && *z >= 0 &&
           *x + *fx <= v->wc.w && *z + *fz <= v->wc.h;
}

static unsigned char move_action(void)
{
    unsigned char a = 0;
    ((ActionOf)VA_ACTION_OF)(&a, (const char*)S_MOVE_GROUND);
    return a;
}

/* IDLE (the header): the list empty, its head the type's default order as the controller made it, or a lone
   Park */
static int idle(const View* v, unsigned slot, const char* u)
{
    WQ_ORDER o[2];
    const int n = v->q.ops->orders(&v->q, (int)slot, o, 2);
    const unsigned type = type_of(v, *(const char* const*)(u + U_TYPE));
    unsigned dflt;
    int park;
    if (n < 0 || !type) return 0;
    if (n == 0) return 1;
    dflt = (unsigned char)v->wc.defs[(size_t)type * UDEF_STRIDE + UD_DEFAULT];
    if ((o[0].flags & WQ_OF_DROP) && dflt && o[0].action == dflt) return 1;
    park = v->q.ops->action(&v->q, WQ_ACT_PARK);
    return n == 1 && park > 0 && o[0].action == park;
}

/* WHO IS MOVED, the one rule the tick and the cursor apply: the view's seat's own live unit at slot `n` that is a
   finished ground mover nothing carries, idle, of a known incarnation, standing on the map. */
typedef struct Mover { int x, z, fx, fz; unsigned mclass, birth; } Mover;

static int movable(const View* v, unsigned n, Mover* m)
{
    const char* u = unit_at(v, n);
    WQ_UNIT wu;
    WQ_DEF d;
    if (!u || !v->q.ops->unit(&v->q, (int)n, &wu) || structure(u) || !wu.finished ||
        *(char* const*)(u + U_CARRIER) || !wu.type || !v->q.ops->def(&v->q, wu.type, &d) ||
        !(d.flags & WQ_DF_MOVER) || (d.flags & WQ_DF_VTOL) || d.mclass >= WQ_MCLASSES ||
        !foot(v, u, &m->x, &m->z, &m->fx, &m->fz) || !idle(v, n, u) || !tagpu_unit_birth(n, &m->birth)) return 0;
    m->mclass = d.mclass;
    return 1;
}

/* ---- the rows ---- */

static ScRow* row_of(unsigned slot)
{
    int i;
    for (i = 0; i < SC_ROWS; i++)
        if (s_row[i].used && s_row[i].unit == slot) return &s_row[i];
    return NULL;
}

static ScRow* row_free(void)
{
    int i;
    for (i = 0; i < SC_ROWS; i++)
        if (!s_row[i].used) return &s_row[i];
    return NULL;
}

/* A ROW OWNS ONE ORDER (the header): the row stays while its unit is that incarnation, alive and its owner's, and
   the head of its list is the move the row gave, by action and point. Then whether it still stands on its site,
   and whether it has stopped getting off it. */
static void row_check(const View* v, ScRow* r)
{
    WQ_UNIT wu;
    WQ_ORDER h;
    unsigned b;
    const char* u;
    int x, z, fx, fz;
    uint32_t stamp;
    int32_t last;
    if (!v->open || !v->q.ops->unit(&v->q, r->unit, &wu) || !tagpu_unit_birth(r->unit, &b) || b != r->birth ||
        v->q.ops->head(&v->q, r->unit, &h) != 1 || h.action != r->action || h.pos[0] != r->pos[0] ||
        h.pos[2] != r->pos[2] || !(u = unit_at(v, r->unit)) || structure(u) || !foot(v, u, &x, &z, &fx, &fz)) {
        memset(r, 0, sizeof *r);
        return;
    }
    memcpy(&stamp, *(const char* const*)(u + U_MOVER) + MV_STAMP, 4);
    last = (int32_t)stamp > r->issued ? (int32_t)stamp : r->issued;
    r->on = (uint8_t)sg_overlap(x, z, fx, fz, r->sx0, r->sz0, r->sfx, r->sfz);
    r->stuck = (uint8_t)(r->on && v->now - last > STUCK_TICKS);
}

/* the builder's site row: its own when the slot holds that incarnation still, else a free one or one whose last
   refusal is past SITE_TTL */
static ScSite* site_get(unsigned slot, unsigned birth, int32_t now)
{
    int i, free_i = -1;
    for (i = 0; i < SC_SITES; i++) {
        ScSite* s = &s_site[i];
        if (s->used && now - s->seen > SITE_TTL) memset(s, 0, sizeof *s);
        if (!s->used) { if (free_i < 0) free_i = i; continue; }
        if (s->builder == slot) {
            if (s->birth == birth) return s;
            memset(s, 0, sizeof *s);              /* the slot holds another unit now */
            if (free_i < 0) free_i = i;
        }
    }
    if (free_i < 0) return NULL;
    s_site[free_i].used = 1;
    s_site[free_i].builder = (uint16_t)slot;
    s_site[free_i].birth = birth;
    return &s_site[free_i];
}

/* ---- the stubs' visit, inside the builder's handler ---- */

/* MobileBuild has refused `def` at the packed first cell `cell` for builder `b`, running `node`. Records the
   site for this step's tick and applies THE HOLD. Writes this file's tables and the node's own counter only. */
static void blocked(char* node, char* b, const char* def, uint32_t cell)
{
    View v;
    WQ_UNIT wu;
    WQ_DEF d;
    ScSite* s;
    const char *ta, *units;
    unsigned nslots, bslot, owner, type, birth;
    uintptr_t off;
    int x0, z0;
    int32_t n;
    if (!s_install.installed || tagpu_puppet_on() || !node || !b || !(ta = *(char* const*)TA_MAIN_PP)) return;
    units = *(char* const*)(ta + OFF_UNIT_BEGIN);
    nslots = *(const uint16_t*)(ta + OFF_UNITSLOTS);
    if (!units || b < units) return;
    off = (uintptr_t)(b - units);
    if (off % UNIT_STRIDE || off / UNIT_STRIDE >= nslots || !off) return;
    bslot = (unsigned)(off / UNIT_STRIDE);
    owner = *(const uint8_t*)(b + U_OWNER);
    if (!view_open(&v, owner) || !v.q.ops->unit(&v.q, (int)bslot, &wu)) return;
    if (*(char* const*)(node + N_OWNER) != b || !tagpu_unit_birth(bslot, &birth)) return;
    type = type_of(&v, def);
    if (!type || !v.q.ops->def(&v.q, (int)type, &d) || (d.flags & WQ_DF_MOVER)) return;
    x0 = (int16_t)(cell & 0xFFFFu); z0 = (int16_t)(cell >> 16);
    if (d.fx < 1 || d.fz < 1 || d.fx > WQ_FOOT_MAX || d.fz > WQ_FOOT_MAX || x0 < 0 || z0 < 0 ||
        x0 + d.fx > v.wc.w || z0 + d.fz > v.wc.h) return;
    if (!(s = site_get(bslot, birth, v.now))) return;
    if (s->type != type || s->x0 != x0 || s->z0 != z0) { s->hold = 0; s->told = 0; }
    s->owner = (uint8_t)owner;
    s->type = (uint16_t)type;
    s->x0 = (int16_t)x0; s->z0 = (int16_t)z0;
    s->fx = (uint8_t)d.fx; s->fz = (uint8_t)d.fz;
    s->seen = v.now;
    s_diag.refusals++;
    if (!s->hold) return;
    memcpy(&n, node + N_RETRY, 4);
    if (n > 1) {
        n--;
        memcpy(node + N_RETRY, &n, 4);
        s_diag.held++;
    }
}

/* regs: the pushad frame, [0] edi [1] esi [2] ebp [3] the esp the branch has [4] ebx. At 0x403CD0 esi is the
   node, edi the builder, ebp the product's def and [esp+0x40] the packed cell (the events slot, overwritten at
   0x403CBF); at 0x41400D esi is the node, ebx the def, [esp+0x38] the builder and [esp+0x40] the packed cell. */
static void __cdecl blocked_ground(unsigned* regs)
{
    blocked((char*)(uintptr_t)regs[1], (char*)(uintptr_t)regs[0], (const char*)(uintptr_t)regs[2],
            *(const uint32_t*)(uintptr_t)(regs[3] + 0x40));
}

static void __cdecl blocked_vtol(unsigned* regs)
{
    blocked((char*)(uintptr_t)regs[1], *(char* const*)(uintptr_t)(regs[3] + 0x38), (const char*)(uintptr_t)regs[4],
            *(const uint32_t*)(uintptr_t)(regs[3] + 0x40));
}

/* ---- one refused site's plan, in the tick ---- */

typedef struct Scratch {
    uint8_t*  wall;     /* a structure's held cells, w h                    */
    uint8_t*  taken;    /* where no unit may be sent, w h                   */
    uint16_t* occ;      /* the unit number standing on each cell, w h       */
    int32_t*  wsums;
    int32_t*  tsums;
    uint8_t*  pass[SC_CLASSES];
    uint8_t   pclass[SC_CLASSES];
    int       npass;
    size_t    passcap;
    SgWork    wk;
    void*     block;
} Scratch;

static int scratch_get(Scratch* s, int w, int h)
{
    const size_t n = (size_t)w * h, cells = (size_t)(w + 1) * (h + 1);
    const size_t passcap = WQ_PASS_CAP(w, h);
    size_t bytes = 2 * cells * sizeof(int32_t) + n * (3 * sizeof(int16_t)) + 3 * n + SC_CLASSES * passcap + 64;
    char* p;
    int k;
    memset(s, 0, sizeof *s);
    if (!(s->block = tagpu_sim_scratch(bytes))) return 0;
    p = (char*)s->block;
    s->passcap = passcap;
    s->wsums = (int32_t*)p; p += cells * sizeof(int32_t);
    s->tsums = (int32_t*)p; p += cells * sizeof(int32_t);
    s->occ = (uint16_t*)p; p += n * sizeof(uint16_t);
    s->wk.qx = (int16_t*)p; p += n * sizeof(int16_t);
    s->wk.qz = (int16_t*)p; p += n * sizeof(int16_t);
    s->wall = (uint8_t*)p; p += n;
    s->taken = (uint8_t*)p; p += n;
    s->wk.seen = (uint8_t*)p; p += n;
    for (k = 0; k < SC_CLASSES; k++) { s->pass[k] = (uint8_t*)p; p += passcap; }
    return 1;
}

static void mark_rect(uint8_t* t, const GgBox* box, int x0, int z0, int fx, int fz)
{
    int x, z, x1 = x0 + fx - 1, z1 = z0 + fz - 1;
    if (x0 < box->x0) x0 = box->x0;
    if (z0 < box->z0) z0 = box->z0;
    if (x1 > box->x0 + box->w - 1) x1 = box->x0 + box->w - 1;
    if (z1 > box->z0 + box->h - 1) z1 = box->z0 + box->h - 1;
    for (z = z0; z <= z1; z++)
        for (x = x0; x <= x1; x++) t[(z - box->z0) * box->w + (x - box->x0)] = 1;
}

/* a class's content map over the box, from the scratch's cache; NULL when the cache is full or the map is not
   to be had */
static const uint8_t* class_map(const View* v, Scratch* s, const GgBox* box, unsigned mclass)
{
    int k;
    for (k = 0; k < s->npass; k++) if (s->pclass[k] == mclass) return s->pass[k];
    if (s->npass == SC_CLASSES) return NULL;
    if (!v->q.ops->passable(&v->q, (int)mclass, box->x0, box->z0, box->w, box->h, s->pass[s->npass], s->passcap))
        return NULL;
    s->pclass[s->npass] = (uint8_t)mclass;
    return s->pass[s->npass++];
}

/* WALL, TAKEN and OCC of the box. Wall: every cell a structure holds. Taken: those, every structure's whole
   footprint (a yard's open cells are held by no unit in the map, and a unit sent there stands in the plant), this
   site and every other, and every place a row has sent a unit to. Occ: the mover standing on each cell. The
   structure scan runs over the box grown by the level's widest yard less one, so a plant with only its open
   cells in the box is found by a held cell outside it. */
static void box_fill(const View* v, const GgBox* box, Scratch* s)
{
    const int m = s_yard_max > 1 ? s_yard_max - 1 : 0;
    int x, z, i;
    for (z = -m; z < box->h + m; z++)
        for (x = -m; x < box->w + m; x++) {
            const unsigned n = cell_unit(v, box->x0 + x, box->z0 + z);
            const int in = x >= 0 && z >= 0 && x < box->w && z < box->h;
            const char* u = n ? unit_at(v, n) : NULL;
            if (u && structure(u)) {
                int cx, cz, fx, fz;
                if (in) s->wall[z * box->w + x] = 1;
                if (in) s->taken[z * box->w + x] = 1;
                if (foot(v, u, &cx, &cz, &fx, &fz)) mark_rect(s->taken, box, cx, cz, fx, fz);
            } else if (in && n) {
                s->occ[z * box->w + x] = (uint16_t)n;
            }
        }
    for (i = 0; i < SC_SITES; i++)
        if (s_site[i].used) mark_rect(s->taken, box, s_site[i].x0, s_site[i].z0, s_site[i].fx, s_site[i].fz);
    for (i = 0; i < SC_ROWS; i++)
        if (s_row[i].used) mark_rect(s->taken, box, s_row[i].dx, s_row[i].dz, s_row[i].fx, s_row[i].fz);
}

/* The refused site's blockers, each of the owner's idle finished ground movers that nothing carries given a move
   to the nearest place it can reach and stand on (tagpu_scattergeo.h, THE WAY OFF A SITE), in the order the
   site's cells hold them, row-major, each place taken before the next unit's search. Every unit is re-derived
   from its slot, and the order goes through the view's own check (tagpu_worldq.c, ORDERS OUT): the slot's
   incarnation, its owner, a local seat. */
static void plan(const View* v, ScSite* s)
{
    Scratch sc;
    GgBox box;
    uint16_t list[SC_PLAN];
    const unsigned char move = move_action();
    unsigned nlist = 0, k, moved = 0, noway = 0, busy = 0, foreign = 0, full = 0;
    int x, z, x1, z1;
    box.x0 = s->x0 - SC_GROW; box.z0 = s->z0 - SC_GROW;
    x1 = s->x0 + s->fx - 1 + SC_GROW; z1 = s->z0 + s->fz - 1 + SC_GROW;
    if (box.x0 < 0) box.x0 = 0;
    if (box.z0 < 0) box.z0 = 0;
    if (x1 > v->wc.w - 1) x1 = v->wc.w - 1;
    if (z1 > v->wc.h - 1) z1 = v->wc.h - 1;
    box.w = x1 - box.x0 + 1; box.h = z1 - box.z0 + 1;
    for (z = s->z0; z < s->z0 + s->fz; z++)
        for (x = s->x0; x < s->x0 + s->fx; x++) {
            const unsigned n = cell_unit(v, x, z);
            for (k = 0; k < nlist && list[k] != n; k++) {}
            if (n && n != s->builder && k == nlist && nlist < SC_PLAN) list[nlist++] = (uint16_t)n;
        }
    if (!nlist || !move || box.w < 1 || box.h < 1 || !scratch_get(&sc, box.w, box.h)) return;
    box_fill(v, &box, &sc);
    gg_sums(sc.wall, box.w, box.h, sc.wsums);
    gg_sums(sc.taken, box.w, box.h, sc.tsums);
    for (k = 0; k < nlist; k++) {
        const unsigned n = list[k];
        const char* u = unit_at(v, n);
        const uint8_t* pass;
        Mover m;
        WQ_CMD cmd;
        ScRow* r;
        int fx, fz, dx, dz;
        if (!u || *(const uint8_t*)(u + U_OWNER) != s->owner) { foreign++; continue; }
        if (row_of(n)) continue;                      /* on its way: its row holds the move still */
        if (!movable(v, n, &m)) { busy++; continue; }
        fx = m.fx; fz = m.fz;
        if (!(pass = class_map(v, &sc, &box, m.mclass)) ||
            !sg_way_off(&box, m.x, m.z, fx, fz, n, pass, sc.wsums, sc.tsums, sc.occ, &sc.wk, &dx, &dz)) {
            noway++;
            continue;
        }
        if (!(r = row_free())) { full++; continue; }
        memset(&cmd, 0, sizeof cmd);
        cmd.verb = WQ_CMD_NEW;
        cmd.inc = WQ_INC_UNIT;
        cmd.unit = (uint16_t)n;
        cmd.target = WQ_NONE;
        cmd.birth = m.birth;
        cmd.pos[0] = (int32_t)((uint32_t)(16 * dx + 8 * fx) << 16);
        cmd.pos[2] = (int32_t)((uint32_t)(16 * dz + 8 * fz) << 16);
        cmd.pos[1] = (int32_t)((uint32_t)(v->q.ops->ground(&v->q, cmd.pos) & 0x7FFF) << 16);
        if (v->q.ops->can(&v->q, (int)n, WQ_CAN_MOVE, WQ_NONE, cmd.pos) != move) { busy++; continue; }
        cmd.action = move;
        if (v->q.ops->order(&v->q, &cmd) != WQ_OK) { busy++; continue; }
        memset(r, 0, sizeof *r);
        r->used = 1;
        r->unit = (uint16_t)n;
        r->owner = s->owner;
        r->birth = m.birth;
        r->action = move;
        r->on = 1;
        r->fx = (uint8_t)fx; r->fz = (uint8_t)fz;
        r->sfx = s->fx; r->sfz = s->fz;
        r->sx0 = s->x0; r->sz0 = s->z0;
        r->dx = (int16_t)dx; r->dz = (int16_t)dz;
        memcpy(r->pos, cmd.pos, sizeof r->pos);
        r->issued = v->now;
        mark_rect(sc.taken, &box, dx, dz, fx, fz);
        gg_sums(sc.taken, box.w, box.h, sc.tsums);
        moved++;
    }
    tagpu_sim_scratch_free(sc.block);
    s_diag.moved += moved; s_diag.noway += noway; s_diag.full += full;
    if (moved || !s->told)
        tagpu_logf("scatter: site builder=%u type=%u cell=%d,%d %ux%u on=%u moved=%u noway=%u busy=%u other=%u "
                   "full=%u", (unsigned)s->builder, (unsigned)s->type, s->x0, s->z0, (unsigned)s->fx,
                   (unsigned)s->fz, nlist, moved, noway, busy, foreign, full);
    s->told = 1;
}

/* THE HOLD's test: a unit this file moved still getting off the site, or another of the owner's movers crossing
   it under a move and having changed cell within STUCK_TICKS */
static int progress(const View* v, const ScSite* s)
{
    const unsigned char move = move_action();
    int x, z;
    for (z = s->z0; z < s->z0 + s->fz; z++)
        for (x = s->x0; x < s->x0 + s->fx; x++) {
            const unsigned n = cell_unit(v, x, z);
            const char* u = n && n != s->builder ? unit_at(v, n) : NULL;
            const ScRow* r;
            WQ_ORDER h;
            uint32_t stamp;
            if (!u || *(const uint8_t*)(u + U_OWNER) != s->owner || structure(u)) continue;
            if ((r = row_of(n))) {
                if (!r->stuck) return 1;
                continue;
            }
            if (!move || v->q.ops->head(&v->q, (int)n, &h) != 1 || h.action != move) continue;
            memcpy(&stamp, *(const char* const*)(u + U_MOVER) + MV_STAMP, 4);
            if (v->now - (int32_t)stamp <= STUCK_TICKS) return 1;
        }
    return 0;
}

void tagpu_scatter_tick(void)
{
    View view[WQ_SEATS];
    uint8_t tried[WQ_SEATS];
    int i, any = 0;
    if (!s_install.installed || tagpu_puppet_on()) return;
    for (i = 0; i < SC_SITES && !any; i++) any = s_site[i].used;
    for (i = 0; i < SC_ROWS && !any; i++) any = s_row[i].used;
    if (!any) return;
    memset(tried, 0, sizeof tried);
    for (i = 0; i < SC_ROWS; i++) {
        ScRow* r = &s_row[i];
        if (!r->used) continue;
        if (r->owner >= WQ_SEATS) { memset(r, 0, sizeof *r); continue; }
        if (!tried[r->owner]) { view_open(&view[r->owner], r->owner); tried[r->owner] = 1; }
        row_check(&view[r->owner], r);
    }
    for (i = 0; i < SC_SITES; i++) {
        ScSite* s = &s_site[i];
        const View* v;
        WQ_UNIT wu;
        unsigned b;
        if (!s->used) continue;
        if (s->owner >= WQ_SEATS) { memset(s, 0, sizeof *s); continue; }
        if (!tried[s->owner]) { view_open(&view[s->owner], s->owner); tried[s->owner] = 1; }
        v = &view[s->owner];
        if (!v->open || v->now - s->seen > SITE_TTL || !v->q.ops->unit(&v->q, s->builder, &wu) ||
            !tagpu_unit_birth(s->builder, &b) || b != s->birth) {
            memset(s, 0, sizeof *s);
            continue;
        }
        if (s->seen == v->now) plan(v, s);
        s->hold = (uint8_t)progress(v, s);
    }
}

/* ---- the cursor's site ---- */

/* The stub's question at the yard test's occupant compare, asked only of a call that carries SITE_MOVES: would
   the unit on this cell be moved for the player the test was given? regs: the pushad frame; cx the cell's unit
   word, [esp+0x4C] the test's player record. The record is tested to be one of the ten before a view opens for
   its seat, which bounds the rest. */
static int __cdecl occupant_moves(unsigned* regs)
{
    const unsigned n = regs[6] & 0xFFFFu;
    const char* rec = *(char* const*)(uintptr_t)(regs[3] + 0x4C);
    const char* ta = *(char* const*)TA_MAIN_PP;
    uintptr_t off;
    View v;
    Mover m;
    if (!s_install.installed || tagpu_puppet_on() || !ta || !rec || rec < ta + OFF_PLAYERS) return 0;
    off = (uintptr_t)(rec - (ta + OFF_PLAYERS));
    if (off % PLAYER_STRIDE || off / PLAYER_STRIDE >= PLAYER_COUNT) return 0;
    return view_open(&v, (unsigned)(off / PLAYER_STRIDE)) && movable(&v, n, &m);
}

unsigned tagpu_scatter_site_test(const void* def, unsigned cell, unsigned ignore, void* player, int* moves)
{
    unsigned ok = ((SiteTest)VA_SITE_TEST)(def, cell, ignore, player);
    *moves = 0;
    if ((ok & 1) || !s_install.site || !player || (ignore & ~0xFFFFu)) return ok;
    ok = ((SiteTest)VA_SITE_TEST)(def, cell, ignore | SITE_MOVES, player);
    *moves = (ok & 1) != 0;
    return ok;
}

void tagpu_scatter_cursor_set(int moves) { s_ui.cursor_moves = (uint8_t)(moves != 0); }
int  tagpu_scatter_cursor_moves(void) { return s_ui.cursor_moves; }

/* ---- install and level ---- */

static const unsigned char STOLEN[7] = { 0x8B, 0x46, 0x3E, 0x85, 0xC0, 0x75, 0x2C };
static const unsigned char VSTOLEN[7] = { 0x8B, 0x46, 0x3E, 0x85, 0xC0, 0x75, 0x30 };

/* pushad; push esp; call fn; add esp,4; popad; mov eax,[esi+0x3e]; test eax,eax; jne `jne`; jmp `next` -- the
   counter is read after the visit, which may have taken one off it (THE HOLD). No branch lands inside either
   site's seven bytes past their first: the nearest targets are 0x403CE4 and 0x414025. */
static unsigned char* stub_make(void (__cdecl* fn)(unsigned*), unsigned jne, unsigned next)
{
    unsigned char* stub = tagpu_detour_stub();
    unsigned char* p = stub;
    if (!stub) return NULL;
    *p++ = 0x60; *p++ = 0x54;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(uintptr_t)fn); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x61;
    *p++ = 0x8B; *p++ = 0x46; *p++ = 0x3E;
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x0F; *p++ = 0x85; tagpu_detour_rel(p, jne); p += 4;
    *p++ = 0xE9; tagpu_detour_rel(p, next); p += 4;
    return stub;
}

static const unsigned char OSTOLEN[5] = { 0x66, 0x3B, 0x4C, 0x24, 0x48 };

/* THE CURSOR'S SITE, at 0x47D554: test dword [esp+0x48],0x10000; jz stock; pushad; push esp; call
   occupant_moves; add esp,4; test eax,eax; popad (which leaves the flags); jnz 0x47D567; stock: cmp
   cx,[esp+0x48]; jmp 0x47D559 -- the je there reads the compare. A call without the flag runs the stolen compare
   and nothing else. No branch lands inside the five bytes: the je at 0x47D552 goes over them to 0x47D567. */
static void site_install(void)
{
    unsigned char* p;
    if (!tagpu_detour_bytes_ok(VA_OCC, OSTOLEN, (int)sizeof OSTOLEN)) {
        tagpu_log("scatter: the cursor's site NOT installed: 0x47D554 does not hold the retail compare");
        return;
    }
    if (!(s_ostub = tagpu_detour_stub())) {
        tagpu_log("scatter: the cursor's site NOT installed: no stub memory");
        return;
    }
    p = s_ostub;
    *p++ = 0xF7; *p++ = 0x44; *p++ = 0x24; *p++ = 0x48; *p++ = 0x00; *p++ = 0x00; *p++ = 0x01; *p++ = 0x00;
    *p++ = 0x74; *p++ = 0x13;
    *p++ = 0x60; *p++ = 0x54;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(uintptr_t)occupant_moves); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x61;
    *p++ = 0x0F; *p++ = 0x85; tagpu_detour_rel(p, VA_OCC_PASS); p += 4;
    memcpy(p, OSTOLEN, sizeof OSTOLEN); p += sizeof OSTOLEN;
    *p++ = 0xE9; tagpu_detour_rel(p, VA_OCC_NEXT); p += 4;
    if (!tagpu_detour_land(VA_OCC, s_ostub, (int)sizeof OSTOLEN)) {
        tagpu_detour_stub_free(s_ostub);
        s_ostub = NULL;
        tagpu_log("scatter: the cursor's site NOT installed: 0x47D554 could not be written");
        return;
    }
    s_install.site = 1;
    tagpu_log("scatter: installed: the cursor's site 0x47D554");
}

void tagpu_scatter_init(void)
{
    if (!tagpu_detour_bytes_ok(VA_BLOCKED, STOLEN, (int)sizeof STOLEN) ||
        !tagpu_detour_bytes_ok(VA_VBLOCKED, VSTOLEN, (int)sizeof VSTOLEN)) {
        tagpu_log("scatter: NOT installed: 0x403CD0 or 0x41400D does not hold the retail blocked-site branch");
        return;
    }
    s_stub = stub_make(blocked_ground, VA_BLOCKED_JNE, VA_BLOCKED_NEXT);
    s_vstub = stub_make(blocked_vtol, VA_VBLOCKED_JNE, VA_VBLOCKED_NEXT);
    if (!s_stub || !s_vstub) {
        if (s_stub) tagpu_detour_stub_free(s_stub);
        if (s_vstub) tagpu_detour_stub_free(s_vstub);
        s_stub = s_vstub = NULL;
        tagpu_log("scatter: NOT installed: no stub memory");
        return;
    }
    if (!tagpu_detour_land(VA_BLOCKED, s_stub, (int)sizeof STOLEN)) {
        tagpu_detour_stub_free(s_stub); tagpu_detour_stub_free(s_vstub);
        s_stub = s_vstub = NULL;
        tagpu_log("scatter: NOT installed: 0x403CD0 could not be written");
        return;
    }
    s_install.installed = 1;
    if (!tagpu_detour_land(VA_VBLOCKED, s_vstub, (int)sizeof VSTOLEN)) {
        tagpu_detour_stub_free(s_vstub);
        s_vstub = NULL;
        tagpu_log("scatter: installed: the blocked site 0x403CD0 (0x41400D could not be written: an air builder's "
                  "site is cleared as stock)");
        site_install();
        return;
    }
    tagpu_log("scatter: installed: the blocked site 0x403CD0 0x41400D");
    site_install();
}

void tagpu_scatter_level_init(void)
{
    WQ_ENGINE wc;
    WORLDQ q;
    unsigned t;
    memset(s_site, 0, sizeof s_site);
    memset(s_row, 0, sizeof s_row);
    memset(&s_diag, 0, sizeof s_diag);
    memset(&s_ui, 0, sizeof s_ui);
    s_yard_max = 0;
    if (!s_install.installed || !tagpu_worldq_open_content(&wc, &q)) return;
    for (t = 1; t < wc.ndefs; t++) {
        WQ_DEF d;
        if (!q.ops->def(&q, (int)t, &d) || !d.yard || (d.flags & WQ_DF_MOVER) || d.fx > WQ_FOOT_MAX ||
            d.fz > WQ_FOOT_MAX) continue;
        if (d.fx > s_yard_max) s_yard_max = d.fx;
        if (d.fz > s_yard_max) s_yard_max = d.fz;
    }
}
