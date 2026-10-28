/* tagpu_sweep.c -- the reclaim sweep (tagpu_sweep.h).

   Arming is stock's: RECLAIM, or its quickkey, writes cursor mode 12 (the only store of 0x0C to
   main+0x2CC3 is 0x419F8A, in the RECLAIM branch of the in-game OnCommand; DISASSEMBLED) and
   draws its own armed plate. The sweep acts while mode 12 holds, the controlled player is not a
   puppet seat and the Vulkan lane draws; on GDI reclaim stays stock. Its hooks are the router's
   (tagpu_route.c), all on the GAME thread inside the in-play mouse handler 0x499200:

   - the cursor chooser's call 0x499297 hands each in-viewport record's cursor index here: the
     drag's edge follows the record's world point, and a feature of the other kind turns into the
     engine's "nothing" cursor 0x13 (tagpu_sweep_cursor);
   - the click route at 0x4993BF hands the press and the release: a press on the ground is
     consumed and becomes the ring's centre; the release past the threshold commits the ring, and
     below it is handed back to stock's click on that record (tagpu_sweep_button);
   - the handler's entry ends the gesture when a record is handled in another mode.

   Keys and buttons come from the window procedure (tagpu_shield.c), on the same thread. While
   armed a popup off the RECLAIM gadget's row picks the kind, and a tag beside the cursor names it.
   The orders a commit queues are drawn by the engine's own queued-order markers, as every other
   order is (tagpu_order.c: the reclaim cursor at each target and the route dots under Shift).

   Everything here is game-thread UI state: the simulation never reads it, and what a commit
   causes -- order nodes made through the engine's own root 0x43ADC0, and the click's own tail
   0x4958C0 -- is engine state the demo recorder keeps. The render thread sees the ring and
   the popup only as the packet's copied values (tagpu_sweep_fill), and hands back
   what it drew of the popup in the command record (tagpu_sweep_take). The kind is kept in the
   settings store (`reclaimkind=`). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tagpu.h"
#include "tagpu_sweep.h"
#include "tagpu_sweepgeo.h"
#include "tagpu_route.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_packet.h"
#include "tagpu_zoom.h"
#include "tagpu_log.h"
#include "tagpu_hud.h"
#include "tagpu_replays.h"
#include "tagpu_settings.h"
#include "tagpu_gaf.h"
#include "dd.h"
#include "render_vk.h"

#define D_STRIDE  0x249u
#define D_FLAGS   0x241       /* u32: bit 11 VTOL (the resolver's VTOL_RECLAIM test, 0x440170) */
#define D_TAGS    0x245       /* u32: bit 0x400 canreclamate (0x43E65C, 0x43F4F7)             */
#define D_SPEED   0x192       /* i32 maxvelocity, 16.16                                       */
#define D_REACH   0x212       /* u16 builddistance                                            */
#define U_FLAGS   0x110
#define U_ORDERS  0x5C
#define N_NEXT    0x4A        /* an order node's next                                         */
#define N_OWNER   0x0E        /* ...its unit                                                  */
#define N_POS     0x22        /* ...its position, x, altitude, z in 16.16                     */
#define M_RECORD  0x2C76      /* the six-dword mouse record the handler acts on               */
#define M_POINT   0x2CAA      /* the world point, 16.16 x, altitude, z                        */
#define M_MODE    0x2CC3
#define M_REGION  0x2CC6      /* bit 1 the pointer is in the viewport                         */
#define M_GUICOL  0x0DCB      /* the GUI colour byte array                                    */
#define M_TOP     0x531       /* the top screen; its +4 is its ControlsAry                    */
#define M_FIELD   0x57D       /* i32 the active textfield's index, -1 none                    */
#define M_SCREENS 0x37EBE     /* u16: 0x865 the options stack, chat, share and the rest       */
#define M_SCREENS2 0x2BEE     /* u8: 0xE0                                                      */
#define FD_ENERGY 0xEC        /* float                                                        */
#define FD_METAL  0xF0        /* float                                                        */
#define FD_TOP    0xFA        /* u8 the feature's height                                      */
#define FD_RECL   0x80        /* FD_MASK's bit 7: reclaimable (0x43E75A, 0x440167)            */
#define FD_BODYSEQ  0xAC      /* GAF sequence: the static body (tagpu_feat.c, the feature pass) */
#define FD_BODYANIM 0xCC      /* anim state: the animating body, FD_MASK bit 1                */
#define WR_BODYANIM 0x04      /* a GAF wreck record's anim state: its body                    */
#define SUB_DEPTH   4         /* sub-frame lists followed, the feature pass's own guard        */
#define G_STRIDE  0x15B       /* a gadget record                                              */
#define G_NAME    0x02        /* char[16], not always terminated                              */
#define G_X       0x13        /* i16, relative to ControlsAry[0]'s own +0x13 / +0x15          */
#define G_Y       0x15
#define G_W       0x17        /* i16, rewritten at the load from the face frame (0x4A812E)    */
#define G_H       0x19
#define G_ACTIVE  0x29        /* u8                                                           */
#define G_QUICKKEY 0x13A      /* u8                                                           */
#define G_GREY    0x13C       /* u16, bit 0                                                   */
#define C_COUNT   0xB6        /* i16 on ControlsAry[0]: the gadget count (0x49FE67)           */

#define CURSOR_NONE    0x13   /* the engine's "nothing": 0x49925D, 0x43F098                    */
#define DRAG_PX        7.0    /* screen px of travel past which a press is a ring             */
/* The ring follows the drag at any zoom; RING_MAX_WU only bounds the scan (at most 513 x 513 cells
   a call, every one inside the grid's own W x H), never what the player can reach on a map. */
#define RING_MAX_WU    4096.0 /* world px                                                      */
/* A list stops growing at SWEEP_LIST_CAP nodes, below the 4096 a recorded list holds
   (REC_ORDERS_MAX, tagpu_recfmt.h), so no commit makes a list the recorder would cut. LIST_WALK
   bounds every walk of a list. */
#define SWEEP_LIST_CAP 1024u
#define LIST_WALK      16384u
/* the gadget records a top screen may have: a count past it is not a screen's */
#define GADGETS_MAX    512
/* the reach of a footprint below its anchor's cell: a centre is at most 8 cells from it */
#define FOOT_MAX       16

typedef const unsigned char* (__stdcall* Resolve)(int*, unsigned, void*, void*, const void*);
typedef void (__stdcall* Order)(int, int, void*, void*, const void*, int, int);
typedef void (__stdcall* ClickTail)(int);
typedef int (__stdcall* GroundHeight)(const int*);
typedef int (__stdcall* Mapped)(const void*, const int*);
typedef unsigned char* (__attribute__((thiscall))* Action)(void*, const char*);

typedef struct {
    const unsigned char *cells, *defs, *wrecks;
    int w, h, ndefs;
} Grid;

/* a feature by its anchor: the cell, the def, the footprint and the centre (world px) */
typedef struct {
    int cell, ax, az, fx, fz, cx, cz;
    const unsigned char* def;
} Feat;

typedef struct {
    Feat f;
    int alt;                  /* the ground's height at the centre, world px */
    uint64_t d2;              /* the squared distance to the ring's centre */
} Target;

typedef struct {
    int active, drag;         /* pressed in the viewport; past the threshold */
    int c[3];                 /* main+0x2CAA of the press record: the centre, 16.16 */
    int cur[3];               /* the last in-viewport record's world point, 16.16 */
    int stale[6];             /* the press's stale move (tagpu_route_stale_move) */
} Gesture;

static int s_armed;
static unsigned char s_kind = SWEEP_ALL;
static Gesture s_g;
static unsigned char s_taken[2];     /* a press the window procedure consumed, per button */
/* The order bytes of RECLAIM, VTOL_RECLAIM and RESURRECT, by the engine's own names. The order
   descriptor array they index is fixed for a level, so they are found once a level. */
static TAGPU_CMD_SWEEP s_drawn;      /* the popup the render thread last drew (tagpu_sweep_take) */
static int s_kindPending;            /* a kind the settings store has not taken yet */
static unsigned char s_act[3];
static int s_actOk;

static Target s_t[SWEEP_MAX_TARGETS];
static SweepTarget s_st[SWEEP_MAX_TARGETS];
static uint16_t s_who[SWEEP_MAX_TARGETS], s_seq[SWEEP_MAX_TARGETS], s_cellOrder[SWEEP_MAX_TARGETS];
static unsigned char s_drop[SWEEP_MAX_TARGETS];
static SweepBuilder s_b[TAGPU_LIM_UNITS];
static uint16_t s_slot[TAGPU_LIM_UNITS];
static unsigned char s_started[TAGPU_LIM_UNITS];

/* the level's counters, logged at level end */
static unsigned s_cCommit, s_cIssued, s_cClick, s_cMismatch, s_cListCap, s_cUnassigned, s_cTrunc;

_Static_assert(TAGPU_PK_SWEEP_TARGETS == SWEEP_MAX_TARGETS, "the packet's targets are the ring's");

static int pointer_on_popup(void);
static int inside(const int16_t r[4], int x, int y);
static int row_at(int x, int y);
static void pointer_screen(int* x, int* y);
static int live(TagpuWorld* w);

static const char* kind_name(unsigned k)
{
    return k == SWEEP_METAL ? "METAL" : k == SWEEP_ENERGY ? "ENERGY" : "ALL";
}

/* THE KIND IS KEPT in the settings store, as the placement's spacing is: handed over whole and,
   in play, without waiting -- the render thread holds the store's lock across its file writes --
   so a change the lock refused stays pending, is offered again at every fill and is waited for
   at the level's end (tagpu_settings.h). */
static void kind_offer(int wait)
{
    if (s_kindPending && tagpu_settings_put_reclaim_kind(s_kind, wait)) s_kindPending = 0;
}

static void set_kind(unsigned k)
{
    s_kind = (unsigned char)k;
    s_kindPending = 1;
    kind_offer(0);
    tagpu_logf("sweep: kind %s", kind_name(s_kind));
}

static void end(void) { s_g.active = 0; s_g.drag = 0; }

/* The sweep acts on this record: armed, the Vulkan lane in force (tagpu_hudstate_on's test),
   the world view bounded and the controlled player not a puppet seat (tagpu_route_world), and
   mode 12, which only the RECLAIM branch writes. */
static int live(TagpuWorld* w)
{
    return s_armed && g_ddraw.renderer == vk_render_main && tagpu_route_world(w) &&
           *(const unsigned char*)(w->ta + M_MODE) == 12;
}

/* the zoom level in force (tagpu_zoom.h; this is the message thread), screen px a world px,
   kept to a sane range */
static double zoom(void)
{
    double z = (double)tagpu_zoom_level();
    return z >= 0.05 && z <= 16.0 ? z : 1.0;
}

static int point_px(int v) { return v >> 16; }

/* The ring's radius, world px: the drag's length, bounded by RING_MAX_WU. */
static int radius(const Gesture* d)
{
    double dx = (double)(d->cur[0] - d->c[0]) / 65536.0, dz = (double)(d->cur[2] - d->c[2]) / 65536.0;
    double r = sqrt(dx * dx + dz * dz), cap = RING_MAX_WU;
    return (int)(r < cap ? r : cap);
}

/* THE FEATURE GRID, read each call and bounded: the cells *(main+0x14287), 13 bytes each, W x H
   from main+0x14233/0x14237 (the bound 0x4815A0 tests), the defs *(main+0x1426F), 0x100 bytes
   each, and their count main+0x14253. The grid lives from LoadMap to the level's teardown, both
   on this thread, so it cannot go while this call reads it. */
static int grid(const char* ta, Grid* g)
{
    g->cells = *(const unsigned char* const*)(ta + OFF_FEATMAP);
    g->defs = *(const unsigned char* const*)(ta + OFF_FEATDEF);
    g->wrecks = *(const unsigned char* const*)(ta + OFF_WRECKS);
    g->w = *(const int*)(ta + OFF_MAP_W16);
    g->h = *(const int*)(ta + OFF_MAP_H16);
    g->ndefs = *(const int*)(ta + OFF_FEATCOUNT);
    return g->cells && g->defs && g->w >= 1 && g->w <= 4096 && g->h >= 1 && g->h <= 4096 &&
           g->ndefs > 0 && g->ndefs < 0xFFFB;
}

static unsigned cell_word(const Grid* g, int cell)
{
    return *(const uint16_t*)(g->cells + (size_t)cell * FT_STRIDE + FT_DEFIDX);
}

/* The feature of `cell` by its anchor, as 0x43E6D5..0x43E754 finds it: a word below 0xFFFB is an
   anchor, 0xFFFE a footprint tile whose anchor is `cell - (byte[+0xA] * W + byte[+0xB])` cells
   back (0x43E71D..0x43E736), and the word found there must be an anchor's (0x43E73C). The engine
   bounds a direct anchor's def by the count (0x43E6F5) but not the hop's, nor tests that the hop
   stays on the grid; this does all three. The footprint is clamped to 1..FOOT_MAX cells. */
static int feature_of(const Grid* g, int cell, Feat* f)
{
    const unsigned char* c = g->cells + (size_t)cell * FT_STRIDE;
    unsigned word = *(const uint16_t*)(c + FT_DEFIDX);
    int fx, fz;
    if (word == 0xFFFEu) {
        cell -= c[FT_WIDX] * g->w + c[FT_WIDX + 1];
        if (cell < 0) return 0;
        word = cell_word(g, cell);
    }
    if (word >= 0xFFFBu || (int)word >= g->ndefs) return 0;
    f->cell = cell;
    f->ax = cell % g->w; f->az = cell / g->w;
    f->def = g->defs + (size_t)word * FD_STRIDE;
    fx = *(const int16_t*)(f->def + FD_FOOTX); fz = *(const int16_t*)(f->def + FD_FOOTZ);
    f->fx = fx < 1 ? 1 : fx > FOOT_MAX ? FOOT_MAX : fx;
    f->fz = fz < 1 ? 1 : fz > FOOT_MAX ? FOOT_MAX : fz;
    f->cx = 16 * f->ax + 8 * f->fx;
    f->cz = 16 * f->az + 8 * f->fz;
    return 1;
}

/* the feature at a 16.16 point: the cell (x >> 20, z >> 20) inside W x H, as 0x4815A0 */
static int feature_at(const Grid* g, const int pos[3], Feat* f)
{
    int x = pos[0] >> 20, z = pos[2] >> 20;
    if (x < 0 || z < 0 || x >= g->w || z >= g->h) return 0;
    return feature_of(g, z * g->w + x, f);
}

/* the kind: a NaN amount compares false, so it counts as none */
static int kind_ok(const unsigned char* def, unsigned kind)
{
    if (kind == SWEEP_METAL) return *(const float*)(def + FD_METAL) > 0.0f;
    if (kind == SWEEP_ENERGY) return *(const float*)(def + FD_ENERGY) > 0.0f;
    return 1;
}

static int reclaimable(const unsigned char* def) { return (def[FD_MASK] & FD_RECL) != 0; }

/* the order bytes of the reclaim family, 0x438760's answer for the engine's own names at
   0x5016C4 RECLAIM, 0x501B68 VTOL_RECLAIM and 0x5052F0 RESURRECT, the strings the resolver
   passes it (0x440187, 0x440176, 0x44004C); 0 for a name it does not find */
static int actions(void)
{
    if (!s_actOk) {
        ((Action)0x00438760u)(&s_act[0], (const char*)0x005016C4u);
        ((Action)0x00438760u)(&s_act[1], (const char*)0x00501B68u);
        ((Action)0x00438760u)(&s_act[2], (const char*)0x005052F0u);
        s_actOk = s_act[0] && s_act[1] && s_act[2];
    }
    return s_actOk;
}

static int family(unsigned char a)
{
    return a && (a == s_act[0] || a == s_act[1] || a == s_act[2]);
}

/* A reclaimer of the player's at `slot`: alive, not excluded, owned, selected (+0x110 bit 4),
   its def's +0x245 bit 0x400 (the resolver's first test, 0x43F4F7). NULL otherwise. */
static char* reclaimer(const TagpuWorld* w, unsigned slot)
{
    char* u = tagpu_route_unit(w, slot);
    unsigned id;
    if (!u || *(const unsigned char*)(u + U_OWNER) != (unsigned)w->player ||
        !(*(const unsigned*)(u + U_FLAGS) & 0x10u) || !(id = tagpu_route_def_id(w, u))) return NULL;
    return *(const unsigned*)(w->defs + id * D_STRIDE + D_TAGS) & 0x400u ? u : NULL;
}

/* The representative: the first selected reclaimer. Whether the resolver's answer for a feature
   is of the reclaim family depends on nothing of the unit but its def's +0x245 bit 0x400 and its
   player's mapped bit (0x43F4F7..0x44018F), and every unit of the block has the same player, so
   that answer for this unit is every one's. Which order of the family it is does depend on the
   unit (VTOL_RECLAIM for a def with +0x241 bit 11, 0x44016F..0x440181), so the commit asks the
   resolver again for each builder. */
static char* representative(const TagpuWorld* w)
{
    unsigned i;
    for (i = w->first; i <= w->last; i++) {
        char* u = reclaimer(w, i);
        if (u) return u;
    }
    return NULL;
}

/* The order the resolver 0x43F0E0 gives `u` for mode 12 with no target unit at `pos`, when it is
   of the reclaim family; 0 otherwise. The resolver answers through its first argument (a byte,
   0 on refusal, 0x4401E3). Of the reclaim case's answers only RECLAIM, VTOL_RECLAIM and
   RESURRECT take a feature: with no reclaimable feature at `pos` a unit whose def has +0x245
   bit 0x80 is given VTOL_MOVE (0x5016CC) or MOVE_GROUND (0x5052E4) instead (0x44019D..0x4401D9,
   DISASSEMBLED), so a non-zero answer alone does not say a feature is taken. */
static unsigned char feature_order(const char* u, const int pos[3])
{
    int out = -1;
    const unsigned char* a;
    if (!actions()) return 0;
    a = ((Resolve)0x0043F0E0u)(&out, 12, (void*)u, NULL, pos);
    return a && family(*a) ? *a : 0;
}

/* The unit's player pointer as a value: one of main's ten player records, or NULL. */
static const char* player_of(const TagpuWorld* w, const char* u)
{
    uintptr_t p = (uintptr_t)*(const char* const*)(u + U_PLAYER);
    uintptr_t base = (uintptr_t)(w->ta + OFF_PLAYERS);
    if (p < base || (p - base) % PLAYER_STRIDE || (p - base) / PLAYER_STRIDE >= PLAYER_COUNT)
        return NULL;
    return (const char*)p;
}

/* the order position of a feature: its centre, at the ground's height there (0x485070, world px) */
static void feature_pos(const Feat* f, int pos[3])
{
    pos[0] = (int)((unsigned)f->cx << 16); pos[1] = 0; pos[2] = (int)((unsigned)f->cz << 16);
    pos[1] = (int)((unsigned)((GroundHeight)0x00485070u)(pos) << 16);
}

/* A target of the kind, by the click's own rules: reclaimable (def +0xFE bit 7) and mapped for
   the representative's player at its order position (0x408090, the same arithmetic as the
   resolver's inline test and patched by the same fix: tagpu_patches.c, LOS_MAPPED). */
static int takes(const Feat* f, const char* player, unsigned kind, int pos[3])
{
    if (!reclaimable(f->def) || !kind_ok(f->def, kind)) return 0;
    feature_pos(f, pos);
    return ((Mapped)0x00408090u)(player, pos) != 0;
}

/* the targets' order: nearer the centre first, then z, then x, then the anchor cell */
static int later(const Target* a, const Target* b)
{
    if (a->d2 != b->d2) return a->d2 > b->d2;
    if (a->f.cz != b->f.cz) return a->f.cz > b->f.cz;
    if (a->f.cx != b->f.cx) return a->f.cx > b->f.cx;
    return a->f.cell > b->f.cell;
}

static void sift_down(Target* t, unsigned n, unsigned i)
{
    for (;;) {
        unsigned l = 2 * i + 1, r = l + 1, m = i;
        Target tmp;
        if (l < n && later(&t[l], &t[m])) m = l;
        if (r < n && later(&t[r], &t[m])) m = r;
        if (m == i) return;
        tmp = t[i]; t[i] = t[m]; t[m] = tmp;
        i = m;
    }
}

static int floor16(int v) { return v >= 0 ? v / 16 : -((15 - v) / 16); }

/* WHICH FEATURES A RING HOLDS, nearest the centre first, at most `cap`; `*more` the ones past it.
   Anchors only, so no feature counts twice and no hop is followed; the cells scanned are the
   ring's box, widened below by a footprint's reach and cut to the grid. A feature is in when its
   centre is (sweep_inside, 64-bit) and `takes` holds.

   WHY THIS IS THE RESOLVER'S ACCEPTANCE: for no target unit the resolver gives a reclaimer a
   reclaim-family order exactly when the feature 0x4815A0 finds at `pos` is reclaimable and the
   mapped bit at `pos` is set (0x43F4F7..0x44018F); the representative is a reclaimer; `pos` lies
   in the anchor's footprint, so 0x4815A0 finds this anchor, directly or by its hop; and 0x408090
   is the same mapped arithmetic. The commit asks the resolver itself before every order and
   counts a refusal as a mismatch, the self-check of this equivalence, which must read 0. The
   resolver is not asked here: each call reaches 0x438760's name search, and a forest is
   thousands of anchors a frame. */
static unsigned enumerate(const Grid* g, const char* player, const int c[3], int r, unsigned kind,
                          Target* out, unsigned cap, unsigned* more)
{
    int x0 = floor16(c[0] - r) - FOOT_MAX / 2, x1 = floor16(c[0] + r);
    int z0 = floor16(c[2] - r) - FOOT_MAX / 2, z1 = floor16(c[2] + r);
    int x, z;
    unsigned n = 0, i;
    *more = 0;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 >= g->w) x1 = g->w - 1;
    if (z1 >= g->h) z1 = g->h - 1;
    for (z = z0; z <= z1; z++)
        for (x = x0; x <= x1; x++) {
            Target t;
            int pos[3];
            unsigned word = cell_word(g, z * g->w + x);
            if (word >= 0xFFFBu || (int)word >= g->ndefs) continue;
            if (!feature_of(g, z * g->w + x, &t.f) ||
                !sweep_inside(c[0], c[2], r, t.f.cx, t.f.cz) || !takes(&t.f, player, kind, pos))
                continue;
            t.alt = pos[1] >> 16;
            t.d2 = (uint64_t)((int64_t)(t.f.cx - c[0]) * (t.f.cx - c[0])) +
                   (uint64_t)((int64_t)(t.f.cz - c[2]) * (t.f.cz - c[2]));
            /* the nearest `cap`, kept as a max-heap of the order above */
            if (n < cap) {
                unsigned k = n++;
                out[k] = t;
                while (k && later(&out[k], &out[(k - 1) / 2])) {
                    Target tmp = out[k]; out[k] = out[(k - 1) / 2]; out[(k - 1) / 2] = tmp;
                    k = (k - 1) / 2;
                }
            } else {
                (*more)++;
                if (cap && later(&out[0], &t)) { out[0] = t; sift_down(out, n, 0); }
            }
        }
    /* the heap sorted in place, nearest first */
    for (i = n; i > 1; i--) {
        Target tmp = out[0]; out[0] = out[i - 1]; out[i - 1] = tmp;
        sift_down(out, i - 1, 0);
    }
    return n;
}

/* the ring's centre in world px */
static void centre_px(const Gesture* d, int c[3])
{
    c[0] = point_px(d->c[0]); c[1] = point_px(d->c[1]); c[2] = point_px(d->c[2]);
}

/* Builder `b` re-derived from its slot, or NULL when it no longer is one. Every root call is
   preceded by this: a unit pointer is a fixed-array address, never a promise that the unit
   still reclaims for us. */
static char* live_builder(const TagpuWorld* w, unsigned b)
{
    return reclaimer(w, s_slot[b]);
}

/* The builder's split inputs, from its list walked on this thread, the one that links and frees
   order nodes; the walk is bounded and stops at a node another unit owns. Without Shift a
   builder starts at its own position with nothing queued; with Shift at its last node that has a
   position, its load the model's ticks over its queued reclaim-family nodes from its position
   through each, as the mockup's queuedTime. Its room keeps the list under SWEEP_LIST_CAP. */
static void builder_inputs(const TagpuWorld* w, const Grid* g, const char* u, int shift,
                           SweepBuilder* b)
{
    const char *def, *n;
    unsigned steps = 0;
    int px, pz;
    memset(b, 0, sizeof *b);
    if (!u) return;                     /* no room: the split gives it nothing */
    def = w->defs + tagpu_route_def_id(w, u) * D_STRIDE;
    b->x = px = *(const int*)(u + U_XFIX) >> 16;
    b->z = pz = *(const int*)(u + U_YFIX) >> 16;
    b->speed = (double)*(const int*)(def + D_SPEED) / 65536.0;
    b->reach = (double)*(const uint16_t*)(def + D_REACH);
    b->load = 0.0;
    for (n = *(const char* const*)(u + U_ORDERS); n && steps < LIST_WALK;
         n = *(const char* const*)(n + N_NEXT)) {
        const int* pos = (const int*)(n + N_POS);
        Feat f;
        if (*(const char* const*)(n + N_OWNER) != u) break;
        steps++;
        if (!shift) continue;
        if (pos[0] > 0 && pos[2] > 0) { b->x = pos[0] >> 16; b->z = pos[2] >> 16; }
        if (family((unsigned char)n[4]) && feature_at(g, pos, &f)) {
            double dx = (double)(f.cx - px), dz = (double)(f.cz - pz), d = sqrt(dx * dx + dz * dz);
            double travel = b->speed > 0.0 && d > b->reach ? (d - b->reach) / b->speed : 0.0;
            b->load += travel + (double)sweep_reclaim_ticks(*(const float*)(f.def + FD_ENERGY),
                                                            *(const float*)(f.def + FD_METAL));
            px = f.cx; pz = f.cz;
        }
    }
    b->room = steps >= SWEEP_LIST_CAP ? 0u : SWEEP_LIST_CAP - steps;
}

static int cell_cmp(const void* a, const void* b)
{
    int x = s_t[*(const uint16_t*)a].f.cell, y = s_t[*(const uint16_t*)b].f.cell;
    return x < y ? -1 : x > y;
}

/* With Shift: every target whose feature is already the target of a reclaim-family node in a
   builder's list is dropped, so no feature is queued twice. The feature is the node's own,
   found at its position as the Reclaim handler finds it (0x421DA0 at order +0x22). Targets are
   matched by anchor cell, searched in a copy of them sorted by cell. Returns the targets kept,
   compacted in their order. */
static unsigned drop_queued(const TagpuWorld* w, const Grid* g, unsigned n, unsigned nb)
{
    unsigned i, b, k;
    for (i = 0; i < n; i++) { s_cellOrder[i] = (uint16_t)i; s_drop[i] = 0; }
    qsort(s_cellOrder, n, sizeof *s_cellOrder, cell_cmp);
    for (b = 0; b < nb; b++) {
        const char *u = live_builder(w, b), *node;
        unsigned steps = 0;
        if (!u) continue;
        for (node = *(const char* const*)(u + U_ORDERS); node && steps++ < LIST_WALK;
             node = *(const char* const*)(node + N_NEXT)) {
            Feat f;
            unsigned lo = 0, hi = n;
            if (*(const char* const*)(node + N_OWNER) != u) break;
            if (!family((unsigned char)node[4]) || !feature_at(g, (const int*)(node + N_POS), &f))
                continue;
            while (lo < hi) {
                unsigned mid = (lo + hi) / 2;
                if (s_t[s_cellOrder[mid]].f.cell < f.cell) lo = mid + 1; else hi = mid;
            }
            if (lo < n && s_t[s_cellOrder[lo]].f.cell == f.cell) s_drop[s_cellOrder[lo]] = 1;
        }
    }
    for (i = k = 0; i < n; i++)
        if (!s_drop[i]) s_t[k++] = s_t[i];
    return k;
}

/* THE COMMIT, inside the in-play handler on the game thread, where stock's click calls its own
   root: the ring's targets, split nearest-first across the selected reclaimers, each issued as
   the order the resolver gives that builder there, then stock's tail of a click. Nothing is
   ordered and the mode stays when the ring holds nothing (the owner's ruling: an empty ring
   orders nothing and stays armed), and a builder the split gives nothing keeps its orders. */
static void commit(const TagpuWorld* w, const Gesture* d, int shift)
{
    Grid g;
    const char *rep, *player;
    int c[3], r = radius(d);
    unsigned found = 0, more = 0, n, nb = 0, m = 0, k, i, issued = 0, mismatch = 0, capped = 0;
    centre_px(d, c);
    if (!grid(w->ta, &g) || !(rep = representative(w)) || !(player = player_of(w, rep)) ||
        !actions()) {
        tagpu_logf("sweep: commit refused: no feature grid, reclaimer or reclaim order");
        return;
    }
    n = found = enumerate(&g, player, c, r, s_kind, s_t, SWEEP_MAX_TARGETS, &more);
    for (i = w->first; i <= w->last && nb < TAGPU_LIM_UNITS; i++)
        if (reclaimer(w, i)) s_slot[nb++] = (uint16_t)i;
    if (shift && n) n = drop_queued(w, &g, n, nb);
    if (n) {
        for (k = 0; k < nb; k++) {
            builder_inputs(w, &g, live_builder(w, k), shift, &s_b[k]);
            s_started[k] = 0;
        }
        for (k = 0; k < n; k++) {
            s_st[k].x = s_t[k].f.cx; s_st[k].z = s_t[k].f.cz;
            s_st[k].ticks = sweep_reclaim_ticks(*(const float*)(s_t[k].f.def + FD_ENERGY),
                                                *(const float*)(s_t[k].f.def + FD_METAL));
        }
        m = sweep_split(s_b, nb, s_st, n, s_who, s_seq);
        /* WHY 0x43ADC0 AND NOT 0x43AFC0 UNDER SHIFT: 0x43AFC0 with Shift first cancels a
           same-action node within 16 world units, inclusive, and creates nothing -- the toggle
           -- so it would take back a neighbouring 1x1 feature's order instead of adding this
           one (tagpu_place.c, PHASE 2, the same argument). Without Shift a builder's first order
           has shift 0, stock's replace (0x43ADC0 destroys every node lacking flag bit 4 and
           links the new one), and every later one appends. The resolver is asked again for
           every order: the preview is never trusted, and the order byte is the builder's own
           (VTOL_RECLAIM for a VTOL, RESURRECT for a resurrector). A refusal is a mismatch, the
           self-check of enumerate's equivalence. No node pointer is held across a call. */
        for (k = 0; k < m; k++) {
            const Target* t = &s_t[s_seq[k]];
            unsigned b = s_who[k];
            char* u = live_builder(w, b);
            int pos[3];
            unsigned char action;
            if (!u) continue;
            feature_pos(&t->f, pos);
            if (!(action = feature_order(u, pos))) { mismatch++; continue; }
            ((Order)0x0043ADC0u)(action, (shift || s_started[b]) ? 1 : 0, u, NULL, pos, 0, 0);
            s_started[b] = 1;
            issued++;
        }
        for (k = 0; k < nb; k++) capped += m < n && !s_b[k].room;
    }
    /* STOCK'S OWN TAIL OF A CLICK, 0x4958C0(keep): with Shift main+0x2CC6 |= 0x20, so releasing
       Shift disarms at the next record (0x4992D8); without it mode 1, bit 5 cleared and STOP's
       radio group reset through 0x49FE60 and 0x4A6A40, which also drops the armed plate --
       0x499092..0x4990F1 exactly (DISASSEMBLED). It reads [main+0x531] and its +4 unchecked,
       so both are tested first; the screen is this thread's and no call between frees it. */
    if (issued) {
        const char* top = *(const char* const*)(w->ta + M_TOP);
        if (top && *(const char* const*)(top + 4)) ((ClickTail)0x004958C0u)(shift ? 1 : 0);
    }
    s_cCommit++; s_cIssued += issued; s_cMismatch += mismatch; s_cListCap += capped;
    s_cUnassigned += n - m; s_cTrunc += more;
    tagpu_logf("sweep: commit kind=%s r=%d found=%u taken=%u builders=%u issued=%u unassigned=%u "
               "mismatch=%u listcap=%u shift=%d", kind_name(s_kind), r, found, n, nb, issued, n - m,
               mismatch, capped, shift ? 1 : 0);
}

/* The drag follows the record's world point, which 0x498DA0 wrote for this record before the
   chooser's call (0x499221), when the pointer is in the viewport (region bit 1). */
static void gesture_update(const TagpuWorld* w)
{
    const int* p = (const int*)(w->ta + M_POINT);
    double dx, dz;
    if (!s_g.active || !(*(const unsigned char*)(w->ta + M_REGION) & 2)) return;
    memcpy(s_g.cur, p, sizeof s_g.cur);
    dx = (double)(s_g.cur[0] - s_g.c[0]) / 65536.0;
    dz = (double)(s_g.cur[2] - s_g.c[2]) / 65536.0;
    if (sqrt(dx * dx + dz * dz) * zoom() >= DRAG_PX) s_g.drag = 1;
}

int tagpu_sweep_cursor(int mode, int idx, int handler)
{
    TagpuWorld w;
    Grid g;
    Feat f;
    const char* rep;
    const int* point;
    if (mode != 12 || !live(&w)) return idx;
    if (handler) gesture_update(&w);
    /* the popup lies over the world: a click there picks a row (tagpu_sweep_mouse) and must read
       as no order to the engine's click, which the window procedure keeps it from anyway */
    if (pointer_on_popup()) return CURSOR_NONE;
    if (s_kind == SWEEP_ALL) return idx;
    point = (const int*)(w.ta + M_POINT);
    if (!grid(w.ta, &g) || !feature_at(&g, point, &f) || !(rep = representative(&w))) return idx;
    if (!feature_order(rep, point)) return idx;
    /* THE FILTER IS THE CLICK'S OWN INPUT. The handler stores this answer in main+0x2CBE
       (0x4992AD) and stock's click reads that byte in the same handler call, after this store,
       for the same world point (0x499027); at 0x13 it orders nothing and keeps the mode
       (0x499041..0x49906A). 0x13 is the engine's own "nothing" and a valid sprite index (the
       cursor table holds 0x15). The feature decides first, as in the resolver: a unit standing
       on a tree's cell under METAL is not click-reclaimable, as stock would have taken the tree
       for it anyway. */
    return kind_ok(f.def, s_kind) ? idx : CURSOR_NONE;
}

/* The handler's entry, before 0x498DA0: the gesture ends when the record is handled in another
   mode, or when it is a move without the button that is not the press's stale move -- a release
   the handler never saw (tagpu_place.c, evaluate, says why the stale move must not end it).
   Every disarm is followed by an entry in another mode before a re-arm, but for one: the
   quickkey re-arms through the character ring, in IdleTick's GUI dispatch before the handler,
   and tagpu_sweep_char ends the gesture on any character it sees while not armed. */
void tagpu_sweep_entry(void)
{
    const char* ta = *(const char* const*)TA_MAIN_PP;
    const int* rec;
    if (!s_g.active) return;
    if (!ta || *(const unsigned char*)(ta + M_MODE) != 12) { end(); return; }
    rec = (const int*)(ta + M_RECORD);
    if (rec[4] == WM_MOUSEMOVE && !(rec[2] & MK_LBUTTON) && !tagpu_route_same_move(rec, s_g.stale))
        end();
}

int tagpu_sweep_button(const int* rec, unsigned msg)
{
    TagpuWorld w;
    Gesture d;
    if (!live(&w)) { end(); return 0; }
    if (msg == WM_LBUTTONDOWN) {
        end();
        /* off the ground stock's (the cursor there is 0x13) */
        if (!(*(const unsigned char*)(w.ta + M_REGION) & 2)) return 0;
        /* THE CENTRE IS THIS RECORD'S: 0x498DA0 wrote main+0x2CAA for this record at 0x499221,
           before the route, in the same call */
        s_g.active = 1; s_g.drag = 0;
        memcpy(s_g.c, w.ta + M_POINT, sizeof s_g.c);
        memcpy(s_g.cur, s_g.c, sizeof s_g.cur);
        tagpu_route_stale_move(s_g.stale);
        return 1;
    }
    if (msg != WM_LBUTTONUP || !s_g.active) return 0;
    d = s_g;
    end();
    /* a click: stock's, on this record, byte for byte -- every selected reclaimer takes this
       feature, Shift keeps the order armed, the recorder sees its click (0x4995B3) */
    if (!d.drag) { s_cClick++; return 2; }
    commit(&w, &d, (rec[2] & MK_SHIFT) != 0);
    return 1;
}

/* The RECLAIM gadget of the top screen: the first button (id 1) of records 1..count whose name,
   copied with a terminator, contains "RECLAIM" -- the engine's own rule, which arms reclaim for
   any gadget whose name holds it (strstr 0x4E49B0 at 0x419F56) and finds gadgets by substring
   (0x49FE60, 0x49FE7D..0x49FEA2). The top screen [main+0x531] and its ControlsAry +4 are loaded
   and freed on this thread, never while a hook here runs; the count is ControlsAry[0]'s i16
   +0xB6, bounded. Found again every time, never kept. `*ctrls0`, when asked for, is ControlsAry[0],
   the screen's own record, whose +0x13 / +0x15 the gadgets' positions are relative to. */
static const unsigned char* reclaim_gadget(const char* ta, const unsigned char** ctrls0)
{
    const char* top = *(const char* const*)(ta + M_TOP);
    const unsigned char* ctrls;
    int count, i;
    if (!top || !(ctrls = *(const unsigned char* const*)(top + 4))) return NULL;
    count = *(const int16_t*)(ctrls + C_COUNT);
    if (count < 1 || count > GADGETS_MAX) return NULL;
    if (ctrls0) *ctrls0 = ctrls;
    for (i = 1; i <= count; i++) {
        const unsigned char* g = ctrls + (size_t)i * G_STRIDE;
        char name[17];
        if (g[0] != 1) continue;
        memcpy(name, g + G_NAME, 16);
        name[16] = 0;
        if (strstr(name, "RECLAIM")) return g;
    }
    return NULL;
}

/* the modal screens and the chat line, which own the keyboard: main+0x37EBE & 0x865 or
   main+0x2BEE & 0xE0 (the order check's test, 0x491D76..0x491DA2), and main+0x57D */
static int keyboard_taken(const char* ta)
{
    return (*(const uint16_t*)(ta + M_SCREENS) & 0x865u) ||
           (*(const unsigned char*)(ta + M_SCREENS2) & 0xE0u) || *(const int*)(ta + M_FIELD) != -1;
}

static unsigned ascii_lower(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static unsigned ascii_upper(unsigned c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/* WHY WM_CHAR. The engine's quickkey match runs on the character ring (the GUI pump 0x4A9FD0,
   the button handler 0x4A6AE0 at 0x4A70FB..0x4A7129: the key against tolower and toupper of the
   gadget's +0x13A), and pressed while reclaim is armed it would flip the toggle off and disarm
   (0x4A7132..0x4A713E, then 0x419BE0). A letter reaches the ring only from WM_CHAR (0x4B5DE5);
   WM_KEYDOWN pushes one only under Ctrl, as vk + 0x69, and WM_SYSKEYDOWN under Alt
   (0x4C2129..0x4C213D) -- neither ever a WM_CHAR letter -- and Esc reaches the ring from WM_CHAR
   alone too (0x4C1D50 pushes nothing for VK_ESCAPE). So taking the one message keeps the key
   from the engine whole. */
int tagpu_sweep_char(unsigned ch)
{
    TagpuWorld w;
    const unsigned char* g;
    unsigned qk;
    /* a character while not armed: a disarm the handler's entry may not have seen yet, which
       this very key could undo (the quickkey arms again) */
    if (!live(&w)) { end(); return 0; }
    if (keyboard_taken(w.ta)) return 0;
    /* Esc: the drag, not the order (stock Esc disarms, the in-game key switch's case 0x1B) */
    if (ch == 0x1B) {
        if (!s_g.active) return 0;
        end();
        return 1;
    }
    if (!(g = reclaim_gadget(w.ta, NULL)) || (*(const uint16_t*)(g + G_GREY) & 1) ||
        !(qk = g[G_QUICKKEY]))
        return 0;
    if (ch != ascii_lower(qk) && ch != ascii_upper(qk)) return 0;
    set_kind((s_kind + 1u) % 3u);
    return 1;
}

/* A right press during a drag cannot go through the engine's cancel: its one call site 0x4993AC
   is the recorder's (tagpu_recorder.c, CLICK_RIGHT_CALL), and the engine's cancel disarms. So
   it is taken here, before the engine has a record of it, and its release with it.
   A left press on the popup is taken the same way: the popup lies over the world, where the
   engine would read the press as a reclaim click on the ground under it. THE POPUP IS HIT-TESTED
   WHERE IT WAS DRAWN: against the rects the render thread last drew it at (tagpu_sweep_take),
   never the ones the fill has only published, so the row a click picks is the row under the
   pointer in the picture the player saw. A press on the plate between rows is taken and picks
   nothing. */
int tagpu_sweep_mouse(int gx, int gy, int down, int right)
{
    TagpuWorld w;
    int b = right ? 1 : 0, row;
    if (!down) {
        if (s_taken[b]) { s_taken[b] = 0; return 1; }
        return 0;
    }
    if (right) {
        if (!s_g.active) return 0;
        end();
        s_taken[1] = 1;
        return 1;
    }
    if (!s_drawn.drawn || !live(&w)) return 0;
    tagpu_hud_to_screen(&gx, &gy);
    if (!inside(s_drawn.pop, gx, gy)) return 0;
    if ((row = row_at(gx, gy)) >= 0 && (unsigned)row != s_kind) set_kind((unsigned)row);
    s_taken[0] = 1;
    return 1;
}

void tagpu_sweep_cancel(void)
{
    end();
    s_taken[0] = s_taken[1] = 0;
}

static int coord_ok(int v) { return v >= -TAGPU_PK_PLACE_COORD && v <= TAGPU_PK_PLACE_COORD; }

static int push_item(TAGPU_PK_SWEEPITEM* it, unsigned max, unsigned* n, int* cut,
                     const TAGPU_PK_SWEEPITEM* e)
{
    if (!coord_ok(e->x) || !coord_ok(e->h) || !coord_ok(e->z)) return 0;
    if (*n >= max) { *cut = 1; return 0; }
    it[(*n)++] = *e;
    return 1;
}

static int floor_half(int v) { return v >= 0 ? v / 2 : -((1 - v) / 2); }

/* a frame's rect about its anchor, grown over its sub-frames: x0, y0 inclusive, x1, y1 exclusive */
static int frame_rect(const void* fr, int r[4], int depth)
{
    TAGPU_GAFGEOM g;
    int k, any = 0;
    if (depth > SUB_DEPTH || !tagpu_gaf_frame_geom(fr, &g)) return 0;
    if (!g.subn) {
        r[0] = -g.hotx; r[1] = -g.hoty; r[2] = g.w - g.hotx; r[3] = g.h - g.hoty;
        return 1;
    }
    for (k = 0; k < g.subn; k++) {
        int q[4];
        if (!frame_rect(tagpu_gaf_subframe(fr, k), q, depth + 1)) continue;
        if (!any) { r[0] = q[0]; r[1] = q[1]; r[2] = q[2]; r[3] = q[3]; any = 1; continue; }
        if (q[0] < r[0]) r[0] = q[0];
        if (q[1] < r[1]) r[1] = q[1];
        if (q[2] > r[2]) r[2] = q[2];
        if (q[3] > r[3]) r[3] = q[3];
    }
    return any;
}

/* THE BODY FRAME THE FEATURE PASS DRAWS for this anchor (tagpu_feat.c `draw_feature`, after
   0x46A610): a GAF wreck's record body, an animating feature's current frame, else its static
   sequence's frame 0. NULL for a 3-D wreck, which the unit pass draws as a model, and for a
   feature with no body sequence, which the engine does not draw (both body paths are gated on
   the static sequence pointer).
   THE READS ARE BOUNDED AND CANNOT OUTLIVE THEIR MEMORY. The def row is bounded by the def count
   (feature_of); the wreck index by WR_COUNT, the fixed pool the level allocates; the frame index
   by its sequence's own count and each frame's size by the decoder's (tagpu_gaf_seq_frame,
   tagpu_gaf_frame_sane), and the sub-frame lists by their u8 count and SUB_DEPTH. The defs, their
   GAF sequences and the wreck pool are the level's: loaded by LoadMap and freed by its teardown,
   both on this thread, so none of it goes while this call reads it. */
static const void* body_frame(const Grid* g, const Feat* f)
{
    const unsigned char* c = g->cells + (size_t)f->cell * FT_STRIDE;
    if (c[FT_FLAGS] & 1u) {
        unsigned rec = *(const uint16_t*)(c + FT_WIDX);
        if (!(f->def[FD_MASK] & 1u) || !g->wrecks || rec >= WR_COUNT) return NULL;
        return tagpu_gaf_state_frame((const char*)g->wrecks + (size_t)rec * WR_STRIDE + WR_BODYANIM);
    }
    if (!*(const char* const*)(f->def + FD_BODYSEQ)) return NULL;
    if (f->def[FD_MASK] & 2u) return tagpu_gaf_state_frame((const char*)f->def + FD_BODYANIM);
    return tagpu_gaf_seq_frame(*(const char* const*)(f->def + FD_BODYSEQ), 0);
}

/* the feature-grid height of the cell `dc` columns and `dr` rows from the anchor, the anchor's
   own past the map's edge, as the publisher clamps the anchor's neighbours */
static int cell_height(const Grid* g, const Feat* f, int dc, int dr)
{
    if (f->ax + dc >= g->w) dc = 0;
    if (f->az + dr >= g->h) dr = 0;
    return g->cells[(size_t)(f->cell + dr * g->w + dc) * FT_STRIDE + FT_HEIGHT];
}

/* WHAT A BRACKET FRAMES, as a rect on the projected plane (world x; z with the altitude's half
   taken, as the marker pass projects): the feature's visible sprite, one px outside it on each
   side -- the owner's ruling, the mockup's `f.box`. The sprite is placed as the feature pass
   places it: anchored at the footprint's centre, (col + fx / 2) * 16 and (row + fz / 2) * 16
   less an eighth of the anchor cell's four corner heights, and offset by the frame's hotspot.
   A 3-D wreck has no frame, and its projected model bounds would want the posed walk of its
   whole template tree; it keeps its footprint, from the ground at its centre `alt` to the def's
   height above it. */
static void box_item(TAGPU_PK_SWEEPITEM* e, unsigned kind, unsigned char colour, const Grid* g,
                     const Feat* f, int alt)
{
    const void* fr = body_frame(g, f);
    int r[4];
    memset(e, 0, sizeof *e);
    e->kind = (uint8_t)kind; e->colour = colour;
    if (fr && frame_rect(fr, r, 0) && r[2] - r[0] + 2 <= TAGPU_PK_SWEEP_BOXMAX &&
        r[3] - r[1] + 2 <= TAGPU_PK_SWEEP_BOXMAX) {
        int fx = *(const int16_t*)(f->def + FD_FOOTX), fz = *(const int16_t*)(f->def + FD_FOOTZ);
        int hs = cell_height(g, f, 0, 0) + cell_height(g, f, 1, 0) + cell_height(g, f, 0, 1) +
                 cell_height(g, f, 1, 1);
        /* the feature pass's junk-def guard, so the anchor is the one it draws at */
        if (fx < 0 || fx > FOOT_MAX) fx = 1;
        if (fz < 0 || fz > FOOT_MAX) fz = 1;
        e->x = 16 * f->ax + (fx * 16) / 2 + r[0] - 1;
        e->z = 16 * f->az + (fz * 16) / 2 - (hs >> 3) + r[1] - 1;
        e->w = (int16_t)(r[2] - r[0] + 2);
        e->d = (int16_t)(r[3] - r[1] + 2);
        return;
    }
    {
        int top = f->def[FD_TOP];
        e->x = 16 * f->ax;
        e->z = 16 * f->az - floor_half(alt + top);
        e->w = (int16_t)(16 * f->fx);
        e->d = (int16_t)(16 * f->fz - floor_half(alt) + floor_half(alt + top));
    }
}

/* The ring's points: at most TAGPU_PK_SWEEP_RINGPTS, about one every 8 screen px, each at the
   higher of the centre's altitude and the ground's there -- the range circles' rule
   (tagpu_order.c, 0x438EA0) -- so the line lies on the ground membership is decided on. */
static void ring_items(const int c[3], int r, unsigned char colour, TAGPU_PK_SWEEPITEM* it,
                       unsigned max, unsigned* n, int* cut)
{
    int k, pts = (int)(6.283185307179586 * r * zoom() / 8.0);
    if (pts < 16) pts = 16;
    if (pts > (int)TAGPU_PK_SWEEP_RINGPTS) pts = (int)TAGPU_PK_SWEEP_RINGPTS;
    for (k = 0; k < pts; k++) {
        double a = 6.283185307179586 * k / pts;
        TAGPU_PK_SWEEPITEM e;
        int pos[3], h;
        memset(&e, 0, sizeof e);
        e.kind = TAGPU_PK_SWI_RINGPT; e.colour = colour;
        e.x = c[0] + (int)lround(r * cos(a));
        e.z = c[2] + (int)lround(r * sin(a));
        if (!coord_ok(e.x) || !coord_ok(e.z)) continue;
        pos[0] = (int)((unsigned)e.x << 16); pos[1] = 0; pos[2] = (int)((unsigned)e.z << 16);
        h = ((GroundHeight)0x00485070u)(pos);
        e.h = h > c[1] ? h : c[1];
        e.w = e.d = 16;
        push_item(it, max, n, cut, &e);
    }
}

/* THE POPUP'S GEOMETRY, in the engine's 1x units off the RECLAIM gadget's absolute rect
   (bx, by, bw, bh): shown only while the gadget is active and not greyed, and only when the
   button lies on the panel the player sees -- inside the 128-px column, below the top bar and
   above the bottom one at the HUD scale in force (the panel is magnified about the origin, E28,
   so its rows below H/s - 32 are under the bar or off the screen). The plate, 124 x 72, opens at
   x 129, its middle row centred on the button, clamped into the world band; the rows are 112 x 18,
   21 apart; the tongue ties the plate to the button's row. Each rect is then magnified about the
   origin as the panel is, x q8 / 256, and rounded once. */
#define POP_X 129
#define POP_W 124
#define POP_H 72

typedef struct {
    const unsigned char* g;
    int b[4];                 /* the gadget's absolute rect, x, y, w, h */
    int q8;
    int pop[4], tongue[4], row[3][4];
} Popup;

static int scaled(int v, int q8) { return (int)(((int64_t)v * q8 + 128) >> 8); }

static void to_screen(const int r[4], int q8, int out[4])
{
    out[0] = scaled(r[0], q8); out[1] = scaled(r[1], q8);
    out[2] = scaled(r[0] + r[2], q8); out[3] = scaled(r[1] + r[3], q8);
}

static int popup_rect(const char* ta, Popup* P)
{
    const unsigned char* c0;
    int H = (int)g_ddraw.height, y0, lo, hi, i, r[4];
    memset(P, 0, sizeof *P);
    P->q8 = 256;
    if (!(P->g = reclaim_gadget(ta, &c0)) || !P->g[G_ACTIVE] || (*(const uint16_t*)(P->g + G_GREY) & 1))
        return 0;
    tagpu_hud_live(NULL, NULL, &P->q8);
    P->b[0] = *(const int16_t*)(c0 + G_X) + *(const int16_t*)(P->g + G_X);
    P->b[1] = *(const int16_t*)(c0 + G_Y) + *(const int16_t*)(P->g + G_Y);
    P->b[2] = *(const int16_t*)(P->g + G_W);
    P->b[3] = *(const int16_t*)(P->g + G_H);
    if (H < 240 || H > 8192 || P->q8 < 256 || P->q8 > 4096 || P->b[2] < 1 || P->b[3] < 1 ||
        P->b[0] < 0 || P->b[0] + P->b[2] > 128 || P->b[1] < 32 ||
        (int64_t)(P->b[1] + P->b[3] + 32) * P->q8 > (int64_t)H * 256)
        return 0;
    lo = 34;
    hi = (int)((int64_t)H * 256 / P->q8) - 32 - POP_H - 2;
    if (hi < lo) return 0;
    y0 = P->b[1] + P->b[3] / 2 - POP_H / 2;
    y0 = y0 < lo ? lo : y0 > hi ? hi : y0;
    r[0] = POP_X; r[1] = y0; r[2] = POP_W; r[3] = POP_H;
    to_screen(r, P->q8, P->pop);
    for (i = 0; i < 3; i++) {
        r[0] = POP_X + 6; r[1] = y0 + 6 + 21 * i; r[2] = 112; r[3] = 18;
        to_screen(r, P->q8, P->row[i]);
    }
    r[0] = 124; r[1] = P->b[1] + 4; r[2] = 6; r[3] = 18;
    r[1] = r[1] < y0 + 6 ? y0 + 6 : r[1] > y0 + POP_H - 24 ? y0 + POP_H - 24 : r[1];
    to_screen(r, P->q8, P->tongue);
    return 1;
}

/* the anchor, logged when it changes: which gadget, its rect, the popup's screen rect, the scale;
   NULL when no popup is shown, so the next one is logged again */
static void anchor_log(const Popup* P)
{
    static char last[160];
    char name[17], line[160];
    if (!P) { last[0] = 0; return; }
    if (P->g) { memcpy(name, P->g + G_NAME, 16); name[16] = 0; } else strcpy(name, "-");
    _snprintf(line, sizeof line, "sweep: anchor gadget=%s rect=%d,%d,%d,%d popup=%d,%d,%d,%d q8=%d",
              name, P->b[0], P->b[1], P->b[2], P->b[3], P->pop[0], P->pop[1], P->pop[2], P->pop[3],
              P->q8);
    line[sizeof line - 1] = 0;
    if (strcmp(line, last)) {
        strcpy(last, line);
        tagpu_logf("%s", line);
    }
}

/* the pointer in the game frame's screen px, with the HUD scale applied (E28) */
static void pointer_screen(int* x, int* y)
{
    *x = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
    *y = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);
    tagpu_hud_to_screen(x, y);
}

static int inside(const int16_t r[4], int x, int y)
{
    return x >= r[0] && x < r[2] && y >= r[1] && y < r[3];
}

/* the popup row of the popup last drawn under the screen point, or -1 */
static int row_at(int x, int y)
{
    int i;
    if (!s_drawn.drawn) return -1;
    for (i = 0; i < 3; i++)
        if (inside(s_drawn.row[i], x, y)) return i;
    return -1;
}

static int pointer_on_popup(void)
{
    int x, y;
    if (!s_drawn.drawn) return 0;
    pointer_screen(&x, &y);
    return inside(s_drawn.pop, x, y);
}

int tagpu_sweep_fill(TAGPU_PK_SWEEP* h, TAGPU_PK_SWEEPITEM* it, unsigned max, unsigned* n,
                     int* cut)
{
    TagpuWorld w;
    Grid g;
    const char *rep, *player;
    const unsigned char* gui;
    int armed;
    *n = 0; *cut = 0;
    if (!s_armed || g_ddraw.renderer != vk_render_main || !tagpu_route_world(&w)) return 0;
    kind_offer(0);
    armed = *(const unsigned char*)(w.ta + M_MODE) == 12;
    gui = (const unsigned char*)w.ta + M_GUICOL;
    memset(h, 0, sizeof *h);
    h->kind = s_kind;
    h->hot = -1;
    h->style = (uint8_t)tagpu_replays_style_index();
    /* the kind's colour: METAL bar's 224, ENERGY's 208 (the resource bars' palette indices), ALL
       the GUI green */
    h->col[0] = s_kind == SWEEP_METAL ? 224 : s_kind == SWEEP_ENERGY ? 208 : gui[0x0A];
    h->col[1] = gui[0x0A]; h->col[2] = gui[0x0F]; h->col[3] = gui[0x00];
    if (armed) {
        Popup P;
        h->flags = TAGPU_PK_SW_ARMED;
        if (popup_rect(w.ta, &P)) {
            int i, px, py;
            h->flags |= TAGPU_PK_SW_POPUP;
            for (i = 0; i < 4; i++) { h->pop[i] = (int16_t)P.pop[i]; h->tongue[i] = (int16_t)P.tongue[i]; }
            pointer_screen(&px, &py);
            h->hot = (int8_t)row_at(px, py);
            anchor_log(&P);
        } else anchor_log(NULL);
    } else anchor_log(NULL);
    if (!grid(w.ta, &g)) return armed;
    if (armed && (rep = representative(&w)) && (player = player_of(&w, rep))) {
        if (s_g.active && s_g.drag) {
            int c[3], r = radius(&s_g);
            unsigned more, nt, k;
            float metal = 0.0f, energy = 0.0f;
            centre_px(&s_g, c);
            if (coord_ok(c[0]) && coord_ok(c[1]) && coord_ok(c[2])) {
                h->flags |= TAGPU_PK_SW_RING;
                h->c[0] = c[0]; h->c[1] = c[1]; h->c[2] = c[2]; h->r = r;
                ring_items(c, r, h->col[1], it, max, n, cut);
                nt = enumerate(&g, player, c, r, s_kind, s_t, SWEEP_MAX_TARGETS, &more);
                for (k = 0; k < nt; k++) {
                    TAGPU_PK_SWEEPITEM e;
                    float fm = *(const float*)(s_t[k].f.def + FD_METAL);
                    float fe = *(const float*)(s_t[k].f.def + FD_ENERGY);
                    /* a wreck returns metal (a metal rock counts as one), a tree energy alone;
                       the sums are display only, and an amount that is not a sane one adds
                       nothing */
                    if (fm > 0.0f) h->nwreck++;
                    else if (fe > 0.0f) h->ntree++;
                    if (fm > 0.0f && fm < 1e7f) metal += fm;
                    if (fe > 0.0f && fe < 1e7f) energy += fe;
                    box_item(&e, TAGPU_PK_SWI_BOX, h->col[0], &g, &s_t[k].f, s_t[k].alt);
                    push_item(it, max, n, cut, &e);
                }
                h->metal = metal < 1e9f ? metal : 1e9f;
                h->energy = energy < 1e9f ? energy : 1e9f;
            }
        } else if (!s_g.active && (*(const unsigned char*)(w.ta + M_REGION) & 2) &&
                   !pointer_on_popup()) {
            Feat f;
            int pos[3];
            if (feature_at(&g, (const int*)(w.ta + M_POINT), &f) && takes(&f, player, s_kind, pos)) {
                TAGPU_PK_SWEEPITEM e;
                box_item(&e, TAGPU_PK_SWI_HOVER, h->col[0], &g, &f, pos[1] >> 16);
                if (push_item(it, max, n, cut, &e)) h->flags |= TAGPU_PK_SW_HOVER;
            }
        }
    }
    return armed || *n > 0;
}

/* THE POPUP AS THE PLAYER LAST SAW IT, from the command record (tagpu_packet_pub.c, where the
   record is applied, before the in-play draw). The render thread wrote it from what its sharp
   layer drew for one packet, and the record primitive hands it over whole (its prefix and suffix
   sequence and the slot rotation) as a LEVEL: a later record replaces it, so the hit test below
   is against the last popup drawn, never one this thread has only published. A record whose
   sharp layer did not run (`pass` 0) says nothing about the screen and keeps the last verdict.
   Every value is bounded here, so a wild one costs a missed click, never a wrong row: the rects
   ordered and within +-TAGPU_PK_SWEEP_RECT, `drawn` 0 or 1. */
static int rect_sane(const int16_t r[4])
{
    return r[0] <= r[2] && r[1] <= r[3] && r[0] >= -TAGPU_PK_SWEEP_RECT && r[1] >= -TAGPU_PK_SWEEP_RECT &&
           r[2] <= TAGPU_PK_SWEEP_RECT && r[3] <= TAGPU_PK_SWEEP_RECT;
}

/* what the player sees of the popup, logged when it changes: the rect drawn, or none */
static void drawn_log(void)
{
    static int16_t last[5] = { -1, 0, 0, 0, 0 };
    int16_t now[5];
    now[0] = s_drawn.drawn;
    memcpy(now + 1, s_drawn.pop, sizeof s_drawn.pop);
    if (!s_drawn.drawn) memset(now + 1, 0, sizeof s_drawn.pop);
    if (!memcmp(now, last, sizeof now)) return;
    memcpy(last, now, sizeof now);
    if (s_drawn.drawn)
        tagpu_logf("sweep: drawn popup=%d,%d,%d,%d", s_drawn.pop[0], s_drawn.pop[1], s_drawn.pop[2],
                   s_drawn.pop[3]);
    else
        tagpu_logf("sweep: drawn none");
}

void tagpu_sweep_take(const TAGPU_CMD_SWEEP* c)
{
    int i, ok;
    if (!c) memset(&s_drawn, 0, sizeof s_drawn);
    else if (c->pass) {
        ok = c->drawn == 1 && rect_sane(c->pop);
        for (i = 0; i < 3 && ok; i++) ok = rect_sane(c->row[i]);
        if (ok) s_drawn = *c;
        else memset(&s_drawn, 0, sizeof s_drawn);
    }
    if (s_armed) drawn_log();
}

void tagpu_sweep_level_end(void)
{
    if (s_armed)
        tagpu_logf("sweep: level end commits=%u issued=%u clicks=%u mismatch=%u listcap=%u "
                   "unassigned=%u trunc=%u", s_cCommit, s_cIssued, s_cClick, s_cMismatch,
                   s_cListCap, s_cUnassigned, s_cTrunc);
    /* outside play: a kind still pending may wait for the store's lock here */
    kind_offer(1);
    memset(&s_g, 0, sizeof s_g);
    memset(&s_drawn, 0, sizeof s_drawn);
    s_taken[0] = s_taken[1] = 0;
    s_actOk = 0;
    s_cCommit = s_cIssued = s_cClick = s_cMismatch = s_cListCap = s_cUnassigned = s_cTrunc = 0;
}

void tagpu_sweep_init(void)
{
    if (!tagpu_route_ok(TAGPU_ROUTE_STUB | TAGPU_ROUTE_ENTRY | TAGPU_ROUTE_CURSOR)) {
        tagpu_logf("sweep: the router's click route, handler entry or cursor calls are not all "
                   "installed; not armed");
        return;
    }
    /* the store attached in cfg_load, before this; the render thread does not exist yet */
    s_kind = (unsigned char)tagpu_settings_reclaim_kind();
    s_armed = 1;
    tagpu_logf("sweep: armed");
    tagpu_logf("sweep: kind %s", kind_name(s_kind));
}
