/* tagpu_place.c -- several buildings in one gesture (tagpu_place.h).

   With a building armed (cursor mode 14), a left press that carries Shift starts a gesture
   instead of placing; every mouse record while the button is held previews the shape from the
   press site to the cursor site; the release orders it. Shift + Ctrl over a unit, or over a
   queued build of a selected builder, previews a ring round it, ordered by a press and release.

   Three hooks, all on the GAME thread inside the in-play mouse handler 0x499200, which copies the
   record main+0x2C76 to its stack (0x499210..0x49921A), lets 0x498DA0 make the world point, runs
   the build cursor 0x4197D0 (0x499241, mode 14 in the viewport) and then routes the click:

   - an after-observer on 0x4197D0 evaluates: the shape, each site judged by the engine's own site
     test, the colours. It runs after the engine has written the cursor rectangle, the altitude
     and bit 6 for this record, so the cursor site is the engine's (snapped by the resource
     module's before-observer when that applies).
   - a stub at 0x4993BF, the mode != 1 branch of the click routing, takes the press and the
     release.
   - a before-observer on the handler's entry ends the gesture when a record is handled in any
     other mode (handler_entry).

   Everything here is game-thread state and UI: the simulation never reads it, and what a commit
   causes -- order nodes made and removed through the engine's own roots -- is engine state the
   demo recorder keeps. The render thread sees the sites only as the packet's copied values. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "tagpu_place.h"
#include "tagpu_placegeo.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_packet.h"
#include "tagpu_detour.h"
#include "tagpu_puppet.h"
#include "tagpu_log.h"
#include "tagpu_settings.h"

#define D_STRIDE 0x249u
#define D_BASE   0x1439B
#define D_FLAGS  0x241
#define U_FLAGS  0x110
#define U_ORDERS 0x5C
#define M_RECORD 0x2C76      /* the six-dword mouse record the handler acts on */
#define M_RECT   0x2C92      /* the build cursor: x0, altitude, z0, x1, altitude, z1 (world px) */
#define M_POINT  0x2CAA      /* the world point, 16.16 */
#define M_MODE   0x2CC3
#define M_TYPE   0x2CC4
#define M_REGION 0x2CC6      /* bit 1 the viewport, bit 5 a Shift placement, bit 6 the site is free */
#define M_GUICOL 0x0DCB      /* the GUI colour byte array, the build square's colours */

#define VA_CURSOR      0x004197D0u
#define VA_HANDLER     0x00499200u
#define VA_SITE_CALL   0x004198C6u
#define VA_ROUTE       0x004993BFu
#define VA_ROUTE_CLICK 0x004993CBu
#define VA_ROUTE_FRAME 0x004995B8u

/* The caps. A commit makes at most PLACE_ORDER_BUDGET orders (sites x builders): the preview
   cuts the shape to what would be ordered. A builder's list stops growing at PLACE_LIST_CAP
   nodes, below the 4096 nodes a recorded list holds (REC_ORDERS_MAX, tagpu_recfmt.h), so no
   gesture makes a list the recorder would cut. LIST_WALK bounds every walk of a list. */
#define PLACE_ORDER_BUDGET 4096u
#define PLACE_LIST_CAP     1024u
#define PLACE_PLANS        16384u
#define LIST_WALK          16384u
#define NOTE_LOGS          8u

/* 0x4C1B80's ids (its table at 0x4C1C48 maps them onto GetAsyncKeyState, import 0x4FC350) */
enum { KEY_SHIFT = 0xF9, KEY_CONTROL = 0xFA, KEY_MENU = 0xFB };

typedef unsigned (__stdcall* SiteTest)(const void* def, unsigned cell, unsigned zero, const void* player);
typedef unsigned (__stdcall* SiteHeight)(void);
typedef unsigned (__stdcall* BlockedHeight)(const void* def, unsigned cell);
typedef void (__stdcall* Order)(int, int, void*, void*, const void*, int, int);
typedef void (__stdcall* RemoveOrder)(void*, const void*);
typedef unsigned char* (__attribute__((thiscall))* Action)(void*, const char*);
typedef int (__stdcall* KeyDown)(int);
typedef unsigned short (__stdcall* HoverUnit)(void);
typedef void (__stdcall* Sound)(const char*, int);

typedef struct {
    char *ta, *defs, *units;
    unsigned ndefs, nslots, first, last;
    int player;
} World;

/* One evaluation's or one commit's view: the armed type, the site test's arguments and the
   builder set, held as slot indices. */
typedef struct {
    World w;
    const char* def;
    unsigned type;
    int fx, fz;
    const char* player;
    unsigned char ground, vtol;
    unsigned nb;
    uint16_t slot[TAGPU_LIM_UNITS];
    unsigned char action[TAGPU_LIM_UNITS];
} Ctx;

typedef struct { int16_t cx, cz; int32_t alt; uint8_t colour; } Site;

/* A node of a builder's list, copied as values: the toggle test reads `match` nodes (the
   builder's own build action), the ring target `build` nodes (a placed build with a type). */
typedef struct { int32_t x, z; uint16_t type; uint8_t match, build; } Plan;

static int s_armed;

/* Spacing per building type: the game thread's table, the one the placement reads. Loaded from
   the store at attach; a change is handed to the store whole (tagpu_settings_put_spacing),
   without waiting in play: while the lock is busy it stays pending and is offered again at the
   next step and evaluation, and waited for at the level's end, outside play. A type at 0 has no
   entry. */
static TsSpacing s_spc[TS_SPACING_SAVED];
static int s_nspc, s_spcPending;
_Static_assert(TS_SPACING_MAX == PLACE_SPACING_MAX, "the store keeps what the shapes lay");
static SiteTest s_test;

static struct {
    int valid;                 /* describes the current mode-14 cursor */
    int rec[6];                /* the main+0x2C76 record it evaluated */
    uint16_t type;
    int fx, fz;
    int cur_cx, cur_cz;        /* the engine's cursor site: the build rectangle / 16 */
    int shape;                 /* PLACE_NONE: the engine's own square stands */
    unsigned n;
    Site site[PLACE_MAX_SITES];
} s_eval;
static struct { int active; uint16_t type; int16_t cx, cz; int stale[6]; } s_gesture;

static Plan s_plan[PLACE_PLANS];
static unsigned s_nplan;
static unsigned char s_matched[LIST_WALK / 8];

static void* s_ret[4];
static unsigned s_depth;

/* the level's counters, logged at level end */
static unsigned s_cEval, s_cCommit, s_cIssued, s_cRemoved, s_cRed, s_cMismatch, s_cTrunc,
                s_cListCap, s_cPlanCap;

static int world(World* w)
{
    char *end, *player;
    uintptr_t first, last, distance;
    memset(w, 0, sizeof *w);
    w->ta = *(char**)TA_MAIN_PP;
    if (!w->ta) return 0;
    w->player = *(unsigned char*)(w->ta + OFF_WATCHED);
    if (w->player >= 10 || tagpu_puppet_seat(w->player)) return 0;
    w->ndefs = *(unsigned*)(w->ta + OFF_UDEFCOUNT);
    if (!w->ndefs || w->ndefs > TAGPU_LIM_TYPES) return 0;
    w->defs = *(char**)(w->ta + D_BASE);
    w->units = *(char**)(w->ta + OFF_UNIT_BEGIN);
    end = *(char**)(w->ta + OFF_UNIT_END);
    if (!w->defs || !w->units || (uintptr_t)end < (uintptr_t)w->units) return 0;
    distance = (uintptr_t)end - (uintptr_t)w->units;
    if (distance % UNIT_STRIDE || distance / UNIT_STRIDE >= TAGPU_PK_DESIGN_SLOTS) return 0;
    w->nslots = *(unsigned short*)(w->ta + OFF_UNITSLOTS);
    if (w->nslots != (unsigned)(distance / UNIT_STRIDE) + 1) return 0;
    /* the player's block, validated against the array's own first and last slots */
    player = w->ta + 0x1B63 + w->player * 0x14B;
    first = (uintptr_t)*(char**)(player + 0x67); last = (uintptr_t)*(char**)(player + 0x6B);
    if (first < (uintptr_t)w->units || last < first || last > (uintptr_t)end ||
        (first - (uintptr_t)w->units) % UNIT_STRIDE ||
        (last - (uintptr_t)w->units) % UNIT_STRIDE) return 0;
    w->first = (unsigned)((first - (uintptr_t)w->units) / UNIT_STRIDE);
    w->last = (unsigned)((last - (uintptr_t)w->units) / UNIT_STRIDE);
    return 1;
}

/* an alive, not excluded unit at `slot`, or NULL */
static char* unit(const World* w, unsigned slot)
{
    char* u;
    unsigned flags;
    if (!slot || slot >= w->nslots) return NULL;
    u = w->units + slot * UNIT_STRIDE;
    flags = *(unsigned*)(u + U_FLAGS);
    return (flags & 0x10000000u) && !(flags & 0x4000u) ? u : NULL;
}

static unsigned def_id(const World* w, const char* u)
{
    uintptr_t d = (uintptr_t)*(char* const*)(u + U_TYPE), base = (uintptr_t)w->defs, offset;
    if (d < base) return 0;
    offset = d - base;
    if (offset % D_STRIDE || offset / D_STRIDE >= w->ndefs) return 0;
    return (unsigned)(offset / D_STRIDE);
}

static int footprint(const char* d, int* x, int* z)
{
    *x = *(short*)(d + 0x14A); *z = *(short*)(d + 0x14C);
    return *x > 0 && *z > 0 && *x <= 64 && *z <= 64;
}

/* The footprint's first cell for a 16.16 centre: 0x4197D0's rounding, an arithmetic shift as
   its `sar` (0x419817..0x41983D). */
static int origin(int centre, int foot)
{
    return (int)((unsigned)centre - ((unsigned)foot << 19) + 0x80000u) >> 20;
}

/* The order position of a site: 0x419670's (0x419704..0x419717), wrapping as its `shl` does. */
static int site_x(const Ctx* c, const Site* s) { return (int)((unsigned)(2 * s->cx + c->fx) << 19); }
static int site_z(const Ctx* c, const Site* s) { return (int)((unsigned)(2 * s->cz + c->fz) << 19); }

/* The cell word the site test takes: x in the low half, z in the high, each a signed 16-bit
   value the test bounds itself (0x47D302..0x47D352: x, z >= 1, x + fx < W, z + fz < H), as
   0x47D820 does (0x47D834..0x47D871). */
static unsigned cell_word(int cx, int cz)
{
    return (unsigned)(uint16_t)cx | (unsigned)(uint16_t)cz << 16;
}

/* 0x43AFC0's predicate, DISASSEMBLED 0x43B004..0x43B029: the position within 16 world units of
   the node on both axes, inclusive, as an unsigned compare of the wrapped difference. */
static int within(int px, int pz, int nx, int nz)
{
    return (unsigned)px - (unsigned)nx + 0x100000u <= 0x200000u &&
           (unsigned)pz - (unsigned)nz + 0x100000u <= 0x200000u;
}

static int key_down(int id) { return ((KeyDown)0x004C1B80u)(id) != 0; }

static int build_node(const World* w, const char* n)
{
    const char* first = *(const char* const*)0x00512344u;
    const char* last = *(const char* const*)0x00512348u;
    uintptr_t bytes;
    unsigned action = (unsigned char)n[4], type = *(unsigned short*)(n + 0x36);
    if (!first || (uintptr_t)last < (uintptr_t)first) return 0;
    bytes = (uintptr_t)last - (uintptr_t)first;
    /* The loader's descriptor array is fixed for the level, stride 0x19. Capability bit 0 marks
       a placed build (the one 0x438C00 draws a rectangle for), not a factory's count node. */
    return bytes % 0x19u == 0 && action < bytes / 0x19u &&
        (*(unsigned*)(first + action * 0x19u + 0x0C) & 1u) &&
        type && type < w->ndefs && *(int*)(n + 0x22) > 0 && *(int*)(n + 0x2A) > 0;
}

/* The build action 0x419670 gives the unit at `slot`, or 0 when it is not in the builder set.
   The set is 0x419670's -- the player's block, selected (+0x110 bit 4), a builder def (+0x241
   bit 6), VTOL_MOBILEBUILD when the def has +0x241 bit 11 (0x419755..0x41979F) -- narrowed to
   units that are alive, not excluded and owned by the player, which 0x419670 does not test.
   CANBUILD is not consulted: 0x419670 does not consult it either. */
static unsigned char builder_action(const Ctx* c, unsigned slot)
{
    const char* u = unit(&c->w, slot);
    unsigned id, flags;
    if (!u || *(unsigned char*)(u + U_OWNER) != c->w.player ||
        !(*(unsigned*)(u + U_FLAGS) & 0x10u) || !(id = def_id(&c->w, u))) return 0;
    flags = *(unsigned*)(c->w.defs + id * D_STRIDE + D_FLAGS);
    if (!(flags & 0x40u)) return 0;
    return (flags & 0x800u) ? c->vtol : c->ground;
}

/* Builder `b` re-derived from its slot, or NULL when it no longer is one. Every root call is
   preceded by this: a unit pointer is a fixed-array address, never a promise that the unit
   still builds for us. */
static char* live_builder(const Ctx* c, unsigned b)
{
    unsigned char a = builder_action(c, c->slot[b]);
    return a && a == c->action[b] ? unit(&c->w, c->slot[b]) : NULL;
}

/* The view of this record: mode 14, the armed type bounded by UNITINFOCount, its footprint,
   and with `builders` the builder set and the two build actions -- 0x438760 writes a byte, 0
   for a name it does not find (0x43880F, 0x438819), and is given the engine's own names at
   0x502654 and 0x502640, as 0x419670 gives them. */
static int context(Ctx* c, int builders)
{
    unsigned i;
    if (!world(&c->w) || *(unsigned char*)(c->w.ta + M_MODE) != 14) return 0;
    c->type = *(unsigned short*)(c->w.ta + M_TYPE);
    if (!c->type || c->type >= c->w.ndefs) return 0;
    c->def = c->w.defs + c->type * D_STRIDE;
    if (!footprint(c->def, &c->fx, &c->fz)) return 0;
    c->player = c->w.ta + 0x1B63 + c->w.player * 0x14B;
    c->nb = 0;
    if (!builders) return 1;
    c->ground = c->vtol = 0;
    ((Action)0x00438760u)(&c->ground, (const char*)0x00502654u);
    ((Action)0x00438760u)(&c->vtol, (const char*)0x00502640u);
    for (i = c->w.first; i <= c->w.last && c->nb < TAGPU_LIM_UNITS; i++) {
        unsigned char a = builder_action(c, i);
        if (!a) continue;
        c->slot[c->nb] = (uint16_t)i; c->action[c->nb] = a; c->nb++;
    }
    return 1;
}

/* The builders' lists, copied as values. Walked on the game thread, the one thread that links,
   unlinks and frees order nodes; each walk is bounded and stops at a node another unit owns.
   0 when the table overflows: the caller then refuses every site rather than order what it
   could not check. */
static int snapshot(const Ctx* c)
{
    unsigned b;
    s_nplan = 0;
    for (b = 0; b < c->nb; b++) {
        const char *u = unit(&c->w, c->slot[b]), *n;
        unsigned steps = 0;
        if (!u) continue;
        for (n = *(const char* const*)(u + U_ORDERS); n && steps++ < LIST_WALK;
             n = *(const char* const*)(n + 0x4A)) {
            int match, build;
            if (*(const char* const*)(n + 0x0E) != u) break;
            match = (unsigned char)n[4] == c->action[b];
            build = build_node(&c->w, n);
            if (!match && !build) continue;
            if (s_nplan >= PLACE_PLANS) return 0;
            s_plan[s_nplan].x = *(const int*)(n + 0x22);
            s_plan[s_nplan].z = *(const int*)(n + 0x2A);
            s_plan[s_nplan].type = build ? *(const unsigned short*)(n + 0x36) : 0;
            s_plan[s_nplan].match = (uint8_t)match;
            s_plan[s_nplan].build = (uint8_t)build;
            s_nplan++;
        }
    }
    return 1;
}

/* A site a stock Shift-click would toggle off instead of placing: some builder has a node of
   its own build action within 16 world units of the site's order position (0x43AFC0's first
   match; it compares no build type). */
static int toggles(const Ctx* c, const Site* s)
{
    int x = site_x(c, s), z = site_z(c, s);
    unsigned i;
    for (i = 0; i < s_nplan; i++)
        if (s_plan[i].match && within(x, z, s_plan[i].x, s_plan[i].z)) return 1;
    return 0;
}

/* The ring's target: the unit under the cursor (0x48CD80, the hover query the handler skips in
   mode 14; it answers the unit's +0xA8 and writes nothing but its own frame), else a builder's
   queued build whose footprint holds the world point -- first in slot order, then list order.
   Cells, never a pointer. */
static int ring_target(const Ctx* c, int* tx, int* tz, int* tw, int* th)
{
    unsigned id = ((HoverUnit)0x0048CD80u)(), i;
    const char* u = id ? unit(&c->w, id) : NULL;
    int px, pz;
    if (u && *(const unsigned short*)(u + U_INDEX) == id) {
        unsigned d = def_id(&c->w, u);
        if (d && footprint(c->w.defs + d * D_STRIDE, tw, th)) {
            *tx = origin(*(const int*)(u + U_XFIX), *tw);
            *tz = origin(*(const int*)(u + U_YFIX), *th);
            return 1;
        }
    }
    px = *(const int*)(c->w.ta + M_POINT) >> 20;
    pz = *(const int*)(c->w.ta + M_POINT + 8) >> 20;
    for (i = 0; i < s_nplan; i++) {
        int ox, oz;
        if (!s_plan[i].build ||
            !footprint(c->w.defs + s_plan[i].type * D_STRIDE, tw, th)) continue;
        ox = origin(s_plan[i].x, *tw); oz = origin(s_plan[i].z, *th);
        if (px >= ox && px < ox + *tw && pz >= oz && pz < oz + *th) {
            *tx = ox; *tz = oz;
            return 1;
        }
    }
    return 0;
}

/* THE RED TEST IS THE ENGINE'S OWN CALL SITE. s_test is the target of the call at 0x4198C6,
   the one the build cursor judges its site with (stock 0x47D2E0, queued_site's build_site_test
   in tagpu_patches.c when that fix is armed, or a foreign patch's), called with the arguments
   0x4198A0..0x4198C6 passes: (def, cell, 0, main + 0x1B63 + main[0x2A42] * 0x14B), stdcall,
   `ret 0x10`. So a gesture's site is judged exactly as a single click there would be, whatever
   is installed. queued_site's Shift exemption reads the live Shift, which every gesture holds.
   The altitude is the build cursor's: 0x47C780 (the test's scratch 0x51E684) for a free site,
   0x47D820 for a refused one (0x4198EC..0x419909). The test writes 0x51E684/0x51E688, which the
   AI's 0x47C770 reads too, so both are put back as they were; no AI tick runs inside the mouse
   handler. Returns the red count. */
static unsigned classify(const Ctx* c, Site* s, unsigned n, int planned)
{
    unsigned scratch0 = *(unsigned*)0x0051E684u, scratch1 = *(unsigned*)0x0051E688u;
    unsigned i, red = 0;
    for (i = 0; i < n; i++) {
        unsigned cell = cell_word(s[i].cx, s[i].cz);
        int ok = s_test(c->def, cell, 0, c->player) & 1;
        s[i].alt = ok ? (int)(((SiteHeight)0x0047C780u)() & 255)
                      : (int)(((BlockedHeight)0x0047D820u)(c->def, cell) & 255);
        if (!ok || !planned) { s[i].colour = PLACE_BLOCKED; red++; continue; }
        s[i].colour = toggles(c, &s[i]) ? PLACE_TOGGLE : PLACE_OK;
    }
    *(unsigned*)0x0051E684u = scratch0; *(unsigned*)0x0051E688u = scratch1;
    return red;
}

/* THE SELF-CHECK: our verdict at the engine's cursor site against the engine's own (bit 6, just
   written by 0x4198D1..0x4198E1). They differ only if our cell or arguments are not the
   engine's; the counter is the regression's oracle and changes nothing at run time. */
static void self_check(const Ctx* c)
{
    unsigned scratch0 = *(unsigned*)0x0051E684u, scratch1 = *(unsigned*)0x0051E688u;
    int ok = s_test(c->def, cell_word(s_eval.cur_cx, s_eval.cur_cz), 0, c->player) & 1;
    int engine = (*(unsigned char*)(c->w.ta + M_REGION) & 0x40) != 0;
    *(unsigned*)0x0051E684u = scratch0; *(unsigned*)0x0051E688u = scratch1;
    if (ok == engine) return;
    if (s_cMismatch++ < NOTE_LOGS)
        tagpu_logf("place: mismatch type=%u site=%d,%d engine=%s ours=%s", c->type,
                   s_eval.cur_cx, s_eval.cur_cz, engine ? "ok" : "red", ok ? "ok" : "red");
}

static void plans_overflowed(void)
{
    if (s_cPlanCap++ < NOTE_LOGS)
        tagpu_logf("place: more than %u queued orders to check; every site refused", PLACE_PLANS);
}

/* at most PLACE_ORDER_BUDGET orders a commit: the shape is cut in its own order */
static unsigned budget(unsigned n, unsigned builders)
{
    unsigned most = PLACE_ORDER_BUDGET / (builders ? builders : 1);
    return n < most ? n : most;
}

static void end_gesture(void) { s_gesture.active = 0; }

/* The mouse object's current move record, six dwords at +0x196: the one an empty ring's peek and
   pop hand IdleTick. The object is the static 0x51F320 WinMain gives 0x4B5990, so it lives as long
   as the process; the window procedure writes the record on this thread (0x4C2360). */
static void stale_move(int out[6])
{
    const char* mouse = *(const char* const*)0x0051FBD0u;
    if (mouse) memcpy(out, mouse + 0x196, 6 * sizeof(int));
    else memset(out, 0, 6 * sizeof(int));
}

/* The type's UnitName (def +0x20, 32 bytes inline, the FBI's own name such as ARMSOLAR):
   copied and NUL-checked inside the field, 0 when it has none. */
static int unit_name(const char* def, char out[TS_SPACING_NAME])
{
    memcpy(out, def + UD_NAME, TS_SPACING_NAME);
    return out[0] && memchr(out, 0, TS_SPACING_NAME) != NULL;
}

static void spacing_offer(int wait)
{
    if (s_spcPending && tagpu_settings_put_spacing(s_spc, s_nspc, wait)) s_spcPending = 0;
}

static int spacing_slot(const char* name)
{
    int i;
    for (i = 0; i < s_nspc; i++)
        if (!lstrcmpiA(s_spc[i].name, name)) return i;
    return -1;
}

/* Empty cells between neighbouring sites of a line or a rectangle; the ring takes none */
static int spacing_of(const char* def)
{
    char name[TS_SPACING_NAME];
    int i;
    if (!unit_name(def, name) || (i = spacing_slot(name)) < 0) return 0;
    return s_spc[i].cells;
}

static void spacing_step(const char* def, int step)
{
    char name[TS_SPACING_NAME];
    int i, was, now;
    if (!unit_name(def, name)) return;
    i = spacing_slot(name);
    was = i < 0 ? 0 : s_spc[i].cells;
    now = was + step < 0 ? 0 : was + step > TS_SPACING_MAX ? TS_SPACING_MAX : was + step;
    if (now == was) return;
    if (!now) {
        memmove(&s_spc[i], &s_spc[i + 1], (size_t)(s_nspc - i - 1) * sizeof *s_spc);
        s_nspc--;
    } else if (i >= 0) {
        s_spc[i].cells = now;
    } else if (s_nspc < TS_SPACING_SAVED) {
        memset(&s_spc[s_nspc], 0, sizeof s_spc[s_nspc]);
        lstrcpynA(s_spc[s_nspc].name, name, TS_SPACING_NAME);
        s_spc[s_nspc].cells = now;
        s_nspc++;
    } else {
        tagpu_logf("place: spacing kept for %d types already; %s stays at 0", TS_SPACING_SAVED, name);
        return;
    }
    s_spcPending = 1;
    spacing_offer(0);
    tagpu_logf("place: spacing %s=%d", name, now);
    if (!tagpu_settings_spacing_name_ok(name, strlen(name)))
        tagpu_logf("place: %s's spacing is not saved: impure.cfg keeps names of [A-Za-z0-9_-] only",
                   name);
}

int tagpu_place_key(unsigned vk, int alt_known)
{
    Ctx c;
    if (!s_armed || (vk != 'Z' && vk != 'X')) return 0;
    if (!alt_known && !key_down(KEY_MENU)) return 0;
    if (!context(&c, 0)) return 0;
    spacing_step(c.def, vk == 'Z' ? 1 : -1);
    return 1;
}

static void evaluate(void)
{
    Ctx c;
    PlaceCell cells[PLACE_MAX_SITES];
    int tx, tz, tw, th, shift, ctrl, alt, planned;
    unsigned n, i, k;
    s_eval.valid = 0; s_eval.n = 0; s_eval.shape = PLACE_NONE;
    if (!context(&c, 0)) { end_gesture(); return; }
    s_cEval++;
    spacing_offer(0);
    memcpy(s_eval.rec, c.w.ta + M_RECORD, sizeof s_eval.rec);
    s_eval.type = (uint16_t)c.type; s_eval.fx = c.fx; s_eval.fz = c.fz;
    /* A move without the button, made after the press: a release the handler never saw. With
       its ring empty IdleTick copies the mouse object's current move record (+0x196, 0x4C2D73)
       to main+0x2C76, so every IdleTick between the press and the next move reads the move made
       BEFORE the press -- Shift alone in wParam (MEASURED 2026-10-25, the button held) -- and
       that one must not end the gesture. It is the record the press saw (stale_move); only a
       later move rewrites +0x196 (0x4C2360, from the window procedure's WM_MOUSEMOVE). Identity,
       not time: the records' clock ticks at 30 Hz (0x4B6340) and wraps short of 2^32. */
    if (s_eval.rec[4] == WM_MOUSEMOVE && !(s_eval.rec[2] & MK_LBUTTON) &&
        memcmp(s_eval.rec, s_gesture.stale, sizeof s_gesture.stale)) end_gesture();
    if (s_gesture.type != c.type) end_gesture();
    /* the engine's rectangle is already snapped: x0 = 16 * the site's first cell (0x419849) */
    s_eval.cur_cx = *(const int*)(c.w.ta + M_RECT) >> 4;
    s_eval.cur_cz = *(const int*)(c.w.ta + M_RECT + 8) >> 4;
    self_check(&c);
    s_eval.valid = 1;
    /* Live keys, not the record's wParam: a modifier pressed without moving the mouse changes
       the shape at once. */
    shift = key_down(KEY_SHIFT); ctrl = key_down(KEY_CONTROL); alt = key_down(KEY_MENU);
    /* Shift up: nothing previews -- a release now drops the gesture (place_button) -- but the
       gesture lives on, so that release is still consumed rather than placing on stock's path */
    if (!shift || (!ctrl && !s_gesture.active)) return;
    if (!context(&c, 1)) return;
    planned = snapshot(&c);
    if (!planned) plans_overflowed();
    if (shift && ctrl && ring_target(&c, &tx, &tz, &tw, &th)) {
        s_eval.shape = PLACE_RING;
        n = place_ring(c.fx, c.fz, tx, tz, tw, th, cells, PLACE_MAX_SITES);
    } else if (s_gesture.active) {
        PlaceCell from, to;
        from.cx = s_gesture.cx; from.cz = s_gesture.cz;
        to.cx = (int16_t)s_eval.cur_cx; to.cz = (int16_t)s_eval.cur_cz;
        s_eval.shape = alt ? (ctrl ? PLACE_HOLLOW : PLACE_RECT) : (ctrl ? PLACE_AXIS : PLACE_LINE);
        n = place_shape(s_eval.shape, c.fx, c.fz, spacing_of(c.def), from, to, cells,
                        PLACE_MAX_SITES);
    } else return;
    n = budget(n, c.nb);
    for (i = 0; i < n; i++) { s_eval.site[i].cx = cells[i].cx; s_eval.site[i].cz = cells[i].cz; }
    classify(&c, s_eval.site, n, planned);
    /* a ring leaves out the sites that would cancel a queued build */
    for (i = k = 0; i < n; i++)
        if (s_eval.shape != PLACE_RING || s_eval.site[i].colour != PLACE_TOGGLE)
            s_eval.site[k++] = s_eval.site[i];
    s_eval.n = k;
}

/* PHASE 1, per builder: each non-red site, in shape order, removes a node of the builder's
   build action that no earlier site matched -- the one at the site's own position, else the
   first within 16 world units, 0x43AFC0's toggle -- and the site then adds nothing for this
   builder. The own position comes first because neighbouring 1x1 sites are 16 world units
   apart: with the first match alone, dragging back over a queued row would shift every removal
   by one and leave the row's last order standing. A node flagged 0x40000 matches but
   is not removed, as 0x43AFC0 looks for it in the +0x60 list and finds nothing
   (0x43B034..0x43B058); such nodes are remembered by their place among the flagged nodes, which
   removing an unflagged one does not change. NO NODE POINTER OUTLIVES 0x439F80: it destructs
   the node and may run the StopMoving script (0x43A1F0), so every site walks the list afresh.
   At most one removal a site. */
static unsigned remove_toggled(const Ctx* c, unsigned b, const Site* s, unsigned n,
                               unsigned char* toggled)
{
    unsigned i, removed = 0;
    memset(s_matched, 0, sizeof s_matched);
    for (i = 0; i < n; i++) {
        const char *u, *node, *found = NULL;
        int x = site_x(c, &s[i]), z = site_z(c, &s[i]), exact;
        if (s[i].colour == PLACE_BLOCKED) continue;
        if (!(u = live_builder(c, b))) return removed;
        for (exact = 1; exact >= 0 && !toggled[i]; exact--) {
            unsigned steps = 0, flagged = 0;
            for (node = *(const char* const*)(u + U_ORDERS); node && steps++ < LIST_WALK;
                 node = *(const char* const*)(node + 0x4A)) {
                unsigned place = flagged, is40 = *(const unsigned*)(node + 0x42) & 0x40000u;
                int nx = *(const int*)(node + 0x22), nz = *(const int*)(node + 0x2A);
                if (*(const char* const*)(node + 0x0E) != u) break;
                if (is40) flagged++;
                if ((unsigned char)node[4] != c->action[b] ||
                    !(exact ? nx == x && nz == z : within(x, z, nx, nz))) continue;
                if (is40) {
                    if (s_matched[place >> 3] & (1u << (place & 7))) continue;
                    s_matched[place >> 3] |= (unsigned char)(1u << (place & 7));
                } else found = node;
                toggled[i] = 1;
                break;
            }
        }
        if (found) { ((RemoveOrder)0x00439F80u)((void*)u, found); removed++; }
    }
    return removed;
}

static unsigned list_length(const char* u)
{
    const char* node;
    unsigned steps = 0;
    for (node = *(const char* const*)(u + U_ORDERS); node && steps < LIST_WALK;
         node = *(const char* const*)(node + 0x4A)) {
        if (*(const char* const*)(node + 0x0E) != u) break;
        steps++;
    }
    return steps;
}

/* PHASE 2, per builder: the sites of `order` that are neither red nor toggled for it are
   appended through the order constructor 0x43ADC0 with Shift (append, never replace), at the
   position and altitude 0x419670 gives, while its list holds fewer than PLACE_LIST_CAP nodes --
   counted once before, then one more per append; 0x43ADC0 only ever drops head nodes on the
   way (0x4000), so the count never runs short. `*capped` is set when the cap stops it. */
static unsigned append_sites(const Ctx* c, unsigned b, const Site* s, const uint16_t* order,
                             unsigned count, const unsigned char* toggled, int* capped)
{
    const char* u = live_builder(c, b);
    unsigned i, issued = 0, length;
    if (!u) return 0;
    length = list_length(u);
    for (i = 0; i < count; i++) {
        const Site* t = &s[order[i]];
        int pos[3];
        if (t->colour == PLACE_BLOCKED || toggled[order[i]]) continue;
        if (length >= PLACE_LIST_CAP) { *capped = 1; break; }
        if (!(u = live_builder(c, b))) break;
        pos[0] = site_x(c, t); pos[1] = (int)((unsigned)t->alt << 16); pos[2] = site_z(c, t);
        ((Order)0x0043ADC0u)(c->action[b], 1, (void*)u, NULL, pos, (int)c->type, 0);
        issued++; length++;
    }
    return issued;
}

static const char* shape_name(int shape)
{
    switch (shape) {
    case PLACE_LINE: return "line";
    case PLACE_AXIS: return "axis";
    case PLACE_RECT: return "rect";
    case PLACE_HOLLOW: return "hollow";
    case PLACE_RING: return "ring";
    default: return "none";
    }
}

/* The commit, TA's semantics: the gesture is the stock Shift-clicks of its sites, judged against
   each builder's queue as it stood before the gesture. Two phases rather than N literal clicks,
   because 0x43AFC0's inclusive 16 units would let a site toggle off the node the previous site
   of the same gesture added (a line of 1x1 sites would alternate add and remove); here a site
   only ever removes a node queued before the gesture. A ring removes nothing: its toggling sites
   are left out. The sites are classified afresh -- the preview is never trusted -- inside the
   in-play handler on the game thread, where the stock click calls its own root. */
static void commit(void)
{
    Ctx c;
    Site s[PLACE_MAX_SITES];
    uint16_t order[PLACE_MAX_SITES];
    unsigned char toggled[PLACE_MAX_SITES];
    unsigned n, cut, red, b, i, issued = 0, removed = 0, capped_builders = 0;
    int planned, shape = s_eval.shape;
    if (!context(&c, 1) || c.type != s_eval.type) return;
    n = budget(s_eval.n, c.nb);
    cut = s_eval.n - n;
    for (i = 0; i < n; i++) {
        s[i].cx = s_eval.site[i].cx; s[i].cz = s_eval.site[i].cz;
        order[i] = (uint16_t)i;
    }
    planned = snapshot(&c);
    if (!planned) plans_overflowed();
    red = classify(&c, s, n, planned);
    for (b = 0; b < c.nb; b++) {
        int capped = 0;
        memset(toggled, 0, sizeof toggled);
        if (shape == PLACE_RING) {
            for (i = 0; i < n; i++) toggled[i] = s[i].colour == PLACE_TOGGLE;
        } else removed += remove_toggled(&c, b, s, n, toggled);
        issued += append_sites(&c, b, s, order, n, toggled, &capped);
        capped_builders += (unsigned)capped;
    }
    if (issued || removed) {
        /* stock's "oktobuild" (0x498F9D..0x498FA4), once a gesture, and the Shift-placement bit
           0x498FB4 sets, so that releasing Shift disarms as after a Shift-click (0x4992D8) */
        ((Sound)0x0047F1A0u)((const char*)0x00509660u, 0);
        *(unsigned char*)(c.w.ta + M_REGION) |= 0x20;
    }
    s_cCommit++; s_cIssued += issued; s_cRemoved += removed; s_cRed += red;
    s_cTrunc += cut; s_cListCap += capped_builders;
    tagpu_logf("place: commit shape=%s sites=%u red=%u builders=%u issued=%u removed=%u "
               "split=0 trunc=%u listcap=%u", shape_name(shape), n, red, c.nb, issued, removed,
               cut, capped_builders);
}

/* The main block while the evaluation still describes the cursor: in play, mode 14, the type
   it evaluated, the controlled player not a puppet seat; NULL otherwise. No side effect: the
   packet fill may run more than once a frame. */
static const char* current(void)
{
    const char* ta = *(char* const*)TA_MAIN_PP;
    unsigned player;
    if (!s_armed || !ta || !s_eval.valid || *(const unsigned char*)(ta + M_MODE) != 14 ||
        *(const unsigned short*)(ta + M_TYPE) != s_eval.type) return NULL;
    player = *(const unsigned char*)(ta + OFF_WATCHED);
    return player < 10 && !tagpu_puppet_seat(player) ? ta : NULL;
}

/* The click routing's verdict for the record `rec` (the handler's stack copy) with message
   `msg`: 0 stock, 1 consumed, 2 run stock's click on this record. */
static int __cdecl place_button(const int* rec, unsigned msg)
{
    const char* ta = current();
    if (!ta) { end_gesture(); return 0; }
    if (msg == WM_LBUTTONDOWN) {
        end_gesture();
        /* RECORD IDENTITY IS A VALUE TEST, not a sequence. The handler copied the six dwords of
           main+0x2C76 (0x499210..0x49921A) and only 0x498DA0 ran before the build cursor,
           writing none of them; so an evaluation whose record equals this one is this
           record's. A press the observer did not see (off the viewport) differs: stock's. */
        if (!(rec[2] & MK_SHIFT) || !(*(const unsigned char*)(ta + M_REGION) & 2) ||
            memcmp(s_eval.rec, rec, sizeof s_eval.rec)) return 0;
        s_gesture.active = 1; s_gesture.type = s_eval.type;
        s_gesture.cx = (int16_t)s_eval.cur_cx; s_gesture.cz = (int16_t)s_eval.cur_cz;
        stale_move(s_gesture.stale);
        return 1;
    }
    if (msg != WM_LBUTTONUP || !s_gesture.active) return 0;
    end_gesture();
    /* Shift released before the button: the gesture is dropped, the building stays armed --
       unless an earlier placement of this Shift hold set main+0x2CC6 bit 5, when 0x4992D8
       disarms at the first record that finds Shift up, as stock does; that record's handler
       entry then ends the gesture (handler_entry) */
    if (!key_down(KEY_SHIFT)) return 1;
    /* one site at this release's own cursor: stock's Shift-click, byte for byte -- its toggle,
       its sound, its recorded click (0x4995B3), its 0x20 bit; 0x419670 reads its Shift from
       the record */
    if (s_eval.n == 1 && s_eval.shape != PLACE_RING && (rec[2] & MK_SHIFT) &&
        s_eval.site[0].cx == s_eval.cur_cx && s_eval.site[0].cz == s_eval.cur_cz &&
        !memcmp(s_eval.rec, rec, sizeof s_eval.rec)) return 2;
    commit();
    return 1;
}

/* THE OBSERVER IS AN AFTER-OBSERVER, AND THE LAST ON ITS SITE. It needs the engine's outputs
   for this record -- the rectangle, the altitude, bit 6 -- so it runs when 0x4197D0 returns;
   the resource module's before-observer, which snaps the point first, is chained before it, and
   an observer with an `after` must be the last installed (tagpu_detour.h, the chain rule).
   0x4197D0 calls only the site test, 0x47C780 and 0x47D820, none of which reaches it, and the
   evaluation calls none of its callers; the small stack of return addresses keeps the pairing
   exact even so, and a call past its depth is not observed. */
static int __cdecl cursor_before(void* entry_esp)
{
    if (!s_armed || s_depth >= sizeof s_ret / sizeof s_ret[0]) return 0;
    s_ret[s_depth++] = ((void**)entry_esp)[0];
    return 1;
}

static void* __cdecl cursor_after(unsigned* regs)
{
    void* ret = s_ret[--s_depth];
    (void)regs;
    evaluate();
    return ret;
}

/* Values only, copied out of the last evaluation; the packet's acquire bounds them again. */
unsigned tagpu_place_sites(TagpuPlaceSite* out, unsigned max)
{
    unsigned i;
    if (!current()) return 0;
    for (i = 0; i < s_eval.n && i < max; i++) {
        const Site* s = &s_eval.site[i];
        out[i].rect[0] = 16 * s->cx;
        out[i].rect[1] = s->alt;
        out[i].rect[2] = 16 * s->cz;
        out[i].rect[3] = 16 * (s->cx + s_eval.fx);
        out[i].rect[4] = s->alt;
        out[i].rect[5] = 16 * (s->cz + s_eval.fz);
        out[i].type = s_eval.type;
        out[i].colour = s->colour;
        out[i].pad = 0;
    }
    return i;
}

/* GDI: each site's rectangle is the engine's own, moved by the site's offset from the engine's
   build rectangle (main+0x2C92..0x2CA6, read now). The square's block (0x469E13..0x469E68)
   projects x - eyeX + 0x80 and z - (altitude >> 1) - eyeY + 0x20, so translating its result
   keeps its projection, vpwide's widening included, without reading the eye. The inner call
   passes the engine's inset rectangle and gets the sites' inset ones. Every value is kept
   within +-0x100000; 0x4BF8C0 clips. */
unsigned tagpu_place_gdi_rects(const int engine[4], int out[][4], unsigned char* colour,
                               unsigned max)
{
    const char* ta = current();
    const int* r;
    const unsigned char* gui;
    unsigned i, k = 0;
    if (!ta || !s_eval.n) return 0;
    r = (const int*)(ta + M_RECT);
    gui = (const unsigned char*)ta + M_GUICOL;
    for (i = 0; i < s_eval.n && k < max; i++) {
        const Site* s = &s_eval.site[i];
        int64_t v[4];
        int j, inside = 1;
        v[0] = (int64_t)engine[0] + 16 * s->cx - r[0];
        v[1] = (int64_t)engine[1] + (16 * s->cz - (s->alt >> 1)) - ((int64_t)r[2] - (r[1] >> 1));
        v[2] = (int64_t)engine[2] + 16 * (s->cx + s_eval.fx) - r[3];
        v[3] = (int64_t)engine[3] + (16 * (s->cz + s_eval.fz) - (s->alt >> 1)) -
               ((int64_t)r[5] - (r[4] >> 1));
        for (j = 0; j < 4; j++) if (v[j] < -0x100000 || v[j] > 0x100000) inside = 0;
        if (!inside) continue;
        for (j = 0; j < 4; j++) out[k][j] = (int)v[j];
        colour[k] = gui[s->colour];
        k++;
    }
    return k;
}

void tagpu_place_level_end(void)
{
    /* outside play: a change still pending may wait for the store's lock here */
    spacing_offer(1);
    if (s_armed)
        tagpu_logf("place: level end evals=%u commits=%u issued=%u removed=%u red=%u "
                   "mismatch=%u trunc=%u listcap=%u plancap=%u", s_cEval, s_cCommit, s_cIssued,
                   s_cRemoved, s_cRed, s_cMismatch, s_cTrunc, s_cListCap, s_cPlanCap);
    memset(&s_eval, 0, sizeof s_eval);
    memset(&s_gesture, 0, sizeof s_gesture);
    s_cEval = s_cCommit = s_cIssued = s_cRemoved = s_cRed = s_cMismatch = s_cTrunc =
        s_cListCap = s_cPlanCap = 0;
}

/* THE IN-PLAY HANDLER'S ENTRY, 0x499200 (IdleTick's per-mode call at 0x499A1C, through
   main+0x391F5): a record handled in any mode but 14 ends the gesture. In mode 1 the routing
   skips the route (0x4993BD) and 0x4197D0 is not called, so neither of the other hooks sees a
   disarm -- a right press (0x499100), Shift up after a Shift placement (0x4992D8), a group
   recall (0x48D9A0 -> 0x495860) or the order check -- nor the release that follows it. Every
   disarm is followed by this entry in mode 1 before any re-arm: arming again takes a left click
   on the build menu, which cannot come while the gesture's own button is held, so the
   gesture's release (or, over the panel, the next record) is handled in mode 1 first. A
   before-observer: the handler runs unchanged. */
static int __cdecl handler_entry(void* entry_esp)
{
    const char* ta = *(const char* const*)TA_MAIN_PP;
    (void)entry_esp;
    if (s_gesture.active && (!ta || *(const unsigned char*)(ta + M_MODE) != 14)) end_gesture();
    return 0;
}

/* THE ROUTE AT 0x4993BF. The click routing's mode != 1 branch, DISASSEMBLED:
       0x4993BF  81 FA 01 02 00 00   cmp edx,0x201        ; edx: the record's message
       0x4993C5  0F 85 ED 01 00 00   jne 0x4995B8         ; the frame: anything but a press
       0x4993CB  8D 4C 24 0C         lea ecx,[esp+0xc]    ; the handler's record copy
       0x4993CF  51                  push ecx
       0x4993D0  E9 DE 01 00 00      jmp 0x4995B3         ; call 0x498F70, the click
   All 22 bytes are matched; the first 12 become `jmp stub` and seven NOPs. No branch in the
   image lands on 0x4993C0..0x4993CA and no absolute reference names them (a scan of the
   disassembly and of the file), so nothing enters the NOPs. The stub asks place_button and
   takes one of three ways: 1, consumed, to the frame 0x4995B8; 2, stock's click on this record,
   to 0x4993CB; 0, the two original instructions. pushad/popad keep every register; EFLAGS
   differ on ways 1 and 2 only, and neither target reads a flag before writing one (0x4995B8 is
   `call 0x496790`; 0x4993CB is `lea`, `push`, `jmp` to a call). Written at attach, before the
   game thread can run the handler. */
static int install_route(void)
{
    static const unsigned char want[22] = {
        0x81,0xFA,0x01,0x02,0x00,0x00, 0x0F,0x85,0xED,0x01,0x00,0x00,
        0x8D,0x4C,0x24,0x0C, 0x51, 0xE9,0xDE,0x01,0x00,0x00 };
    unsigned char patch[12];
    unsigned char *s, *p;
    int32_t rel;
    if (memcmp((const void*)(uintptr_t)VA_ROUTE, want, sizeof want)) return 0;
    if (!(s = p = tagpu_detour_stub())) return 0;
    *p++ = 0x60;                                            /* pushad                    */
    *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x2C;     /* lea eax,[esp+0x2C]: the record, [esp+0xc] before pushad */
    *p++ = 0x52;                                            /* push edx: the message     */
    *p++ = 0x50;                                            /* push eax: the record      */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(uintptr_t)place_button); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                  /* add esp,8                 */
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x01;                  /* cmp eax,1                 */
    *p++ = 0x61;                                            /* popad: the flags stand    */
    *p++ = 0x0F; *p++ = 0x84; tagpu_detour_rel(p, VA_ROUTE_FRAME); p += 4;  /* je: consumed   */
    *p++ = 0x0F; *p++ = 0x87; tagpu_detour_rel(p, VA_ROUTE_CLICK); p += 4;  /* ja: the click  */
    memcpy(p, want, 6); p += 6;                             /* cmp edx,0x201             */
    *p++ = 0x0F; *p++ = 0x85; tagpu_detour_rel(p, VA_ROUTE_FRAME); p += 4;  /* jne           */
    *p++ = 0xE9; tagpu_detour_rel(p, VA_ROUTE_CLICK);                       /* jmp           */
    rel = (int32_t)((unsigned)(uintptr_t)s - (VA_ROUTE + 5));
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);
    memset(patch + 5, 0x90, sizeof patch - 5);
    if (!tagpu_detour_write(VA_ROUTE, patch, sizeof patch)) { tagpu_detour_stub_free(s); return 0; }
    return 1;
}

void tagpu_place_init(void)
{
    static const unsigned char build[] = {0x83,0xEC,0x14,0x8B,0x15,0xE8,0x1D,0x51,0x00};
    /* mov eax,[0x511DE8]; sub esp,0x18 */
    static const unsigned char handler[] = {0xA1,0xE8,0x1D,0x51,0x00,0x83,0xEC,0x18};
    const unsigned char* call = (const unsigned char*)(uintptr_t)VA_SITE_CALL;
    if (call[0] != 0xE8) {
        tagpu_logf("place: the build cursor's site test (0x4198C6) is not a call; not armed");
        return;
    }
    s_test = (SiteTest)(uintptr_t)(VA_SITE_CALL + 5 + *(const unsigned*)(call + 1));
    if (!tagpu_detour_bytes_ok(VA_CURSOR, build, sizeof build)) {
        tagpu_logf("place: incompatible build cursor (0x4197D0); not armed");
        return;
    }
    /* the route first: until s_armed is set, place_button answers stock */
    if (!install_route()) {
        tagpu_logf("place: incompatible click routing (0x4993BF); not armed");
        return;
    }
    if (!tagpu_detour_bytes_ok(VA_HANDLER, handler, sizeof handler) ||
        !tagpu_detour_observe(VA_HANDLER, handler, sizeof handler, handler_entry, NULL)) {
        tagpu_logf("place: observer on the in-play handler (0x499200) refused; not armed");
        return;
    }
    if (!tagpu_detour_observe(VA_CURSOR, build, sizeof build, cursor_before, cursor_after)) {
        tagpu_logf("place: observer on the build cursor (0x4197D0) refused; not armed");
        return;
    }
    /* the store attached in cfg_load, before this; the render thread does not exist yet */
    s_nspc = tagpu_settings_spacing_load(s_spc, TS_SPACING_SAVED);
    s_armed = 1;
    tagpu_logf("place: armed (build cursor 0x4197D0 observed on return, click route 0x4993BF, "
               "handler entry 0x499200 observed, site test 0x%08X)", (unsigned)(uintptr_t)s_test);
}
