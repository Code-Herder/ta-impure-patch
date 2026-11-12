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

   Which features a ring takes, the reclaimers' split inputs and the commit's removals and
   appends are tasim_sweep.c's rules, over the engine's WORLDQ (tagpu_worldq.c); the game keeps the
   gesture, the popup, the cursor's filter and the click's tail.

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
#include "tagpu_worldq.h"
#include "tasim_sweep.h"
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

#define U_FLAGS   0x110
#define M_RECORD  0x2C76      /* the six-dword mouse record the handler acts on               */
#define M_POINT   0x2CAA      /* the world point, 16.16 x, altitude, z                        */
#define M_MODE    0x2CC3
#define M_REGION  0x2CC6      /* bit 1 the pointer is in the viewport                         */
#define M_GUICOL  0x0DCB      /* the GUI colour byte array                                    */
#define M_TOP     0x531       /* the top screen; its +4 is its ControlsAry                    */
#define M_FIELD   0x57D       /* i32 the active textfield's index, -1 none                    */
#define M_SCREENS 0x37EBE     /* u16: 0x865 the options stack, chat, share and the rest       */
#define M_SCREENS2 0x2BEE     /* u8: 0xE0                                                      */
#define FD_TOP    0xFA        /* u8 the feature's height                                      */
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
/* the gadget records a top screen may have: a count past it is not a screen's */
#define GADGETS_MAX    512
typedef void (__stdcall* ClickTail)(int);
typedef int (__stdcall* GroundHeight)(const int*);

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
    int active, drag;         /* pressed in the viewport; past the threshold */
    int c[3];                 /* main+0x2CAA of the press record: the centre, 16.16 */
    int cur[3];               /* the last in-viewport record's world point, 16.16 */
    int stale[6];             /* the press's stale move (tagpu_route_stale_move) */
} Gesture;

static int s_armed;
static unsigned char s_kind = SWEEP_ALL;
static Gesture s_g;
static unsigned char s_taken[2];     /* a press the window procedure consumed, per button */
/* the left button as the window procedure last passed it to the engine: a press passed sets it,
   a release passed clears it. A drag's right press or Esc is taken only while it is set, so the
   release they would end it for has not gone into the engine's ring yet (tagpu_sweep_mouse). */
static unsigned char s_lheld;
static TAGPU_CMD_SWEEP s_drawn;      /* the popup the render thread last drew (tagpu_sweep_take) */
static int s_kindPending;            /* a kind the settings store has not taken yet */

/* the rules' scratch, their targets and the selection they read: one call's at a time, all on this
   thread */
static SWEEP_SCRATCH s_ss;
static SWEEP_TARGET s_tt[SWEEP_MAX_TARGETS];
static uint16_t s_sel[SWEEP_BUILDERS_MAX];

/* the level's counters, logged at level end */
static unsigned s_cCommit, s_cIssued, s_cClick, s_cMismatch, s_cListCap, s_cUnassigned, s_cTrunc;
static unsigned s_cCancelled, s_cStale;

_Static_assert(TAGPU_PK_SWEEP_TARGETS == SWEEP_MAX_TARGETS, "the packet's targets are the ring's");
_Static_assert((int)SWEEP_METAL == (int)SWEEP_KIND_METAL && (int)SWEEP_ENERGY == (int)SWEEP_KIND_ENERGY &&
               (int)SWEEP_ALL == (int)SWEEP_KIND_ALL, "the kinds are the rule's");
/* sweep_view stops at SWEEP_BUILDERS_MAX selected units: a seat's whole block must fit, or a selection past
   it would lose reclaimers silently */
_Static_assert(TAGPU_LIM_UNITS <= SWEEP_BUILDERS_MAX, "a seat's units fit the rule's builder set");

static int pointer_on_popup(const char* ta);
static int popup_live(const char* ta);
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
    return z >= TAGPU_ZOOM_FLOOR_LO && z <= 16.0 ? z : 1.0;
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

/* The feature of `cell` by its anchor, for the drawing, as 0x43E6D5..0x43E754 finds it: a word
   below 0xFFFB is an anchor, 0xFFFE a footprint tile whose anchor is
   `cell - (byte[+0xA] * W + byte[+0xB])` cells back (0x43E71D..0x43E736), and the word found there
   must be an anchor's (0x43E73C). The engine
   bounds a direct anchor's def by the count (0x43E6F5) but not the hop's, nor tests that the hop
   stays on the grid; this does all three. The footprint is clamped to 1..SWEEP_FOOT_MAX cells. */
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
    f->fx = fx < 1 ? 1 : fx > SWEEP_FOOT_MAX ? SWEEP_FOOT_MAX : fx;
    f->fz = fz < 1 ? 1 : fz > SWEEP_FOOT_MAX ? SWEEP_FOOT_MAX : fz;
    f->cx = 16 * f->ax + 8 * f->fx;
    f->cz = 16 * f->az + 8 * f->fz;
    return 1;
}

/* the ring's centre in world px */
static void centre_px(const Gesture* d, int c[3])
{
    c[0] = point_px(d->c[0]); c[1] = point_px(d->c[1]); c[2] = point_px(d->c[2]);
}

/* the rules' view of the player and their input: the ring, the kind and the player's selected live
   units, slots ascending (tasim_sweep.h) */
static int sweep_view(const TagpuWorld* w, WQ_ENGINE* e, WORLDQ* q, SWEEP_IN* in)
{
    unsigned i;
    memset(in, 0, sizeof *in);
    if (!tagpu_worldq_open(e, q, (int)w->player, WQ_SITE_ENGINE, NULL)) return 0;
    in->kind = s_kind;
    for (i = w->first; i <= w->last && in->nselected < (int)SWEEP_BUILDERS_MAX; i++) {
        const char* u = tagpu_route_unit(w, i);
        if (u && *(const unsigned char*)(u + U_OWNER) == (unsigned)w->player && (*(const unsigned*)(u + U_FLAGS) & 0x10u))
            s_sel[in->nselected++] = (uint16_t)i;
    }
    in->selected = s_sel;
    return 1;
}

/* THE COMMIT, inside the in-play handler on the game thread, where stock's click calls its own
   root: tasim_sweep_commit -- the ring's targets split nearest-first across the selected
   reclaimers, each issued as the order the resolver gives that builder there -- then stock's tail
   of a click. Nothing is ordered and the mode stays when the ring holds nothing (the owner's
   ruling: an empty ring orders nothing and stays armed). WHAT IT TARGETS IS ONLY ADDED TO (the
   owner's ruling): a builder already reclaiming one of the ring's targets keeps that order where
   it is, and no feature is queued twice. */
static void commit(const TagpuWorld* w, const Gesture* d, int shift)
{
    Grid g;
    WQ_ENGINE e;
    WORLDQ q;
    SWEEP_IN in;
    SWEEP_RES res;
    int r = radius(d);
    if (!grid(w->ta, &g) || !sweep_view(w, &e, &q, &in)) {
        tagpu_logf("sweep: commit refused: no feature grid, reclaimer or reclaim order");
        return;
    }
    centre_px(d, in.c);
    in.r = r;
    in.shift = shift;
    if (!tasim_sweep_commit(&q, &in, &res, &s_ss)) {
        tagpu_logf("sweep: commit refused: no feature grid, reclaimer or reclaim order");
        return;
    }
    /* STOCK'S OWN TAIL OF A CLICK, 0x4958C0(keep): with Shift main+0x2CC6 |= 0x20, so releasing
       Shift disarms at the next record (0x4992D8); without it mode 1, bit 5 cleared and STOP's
       radio group reset through 0x49FE60 and 0x4A6A40, which also drops the armed plate --
       0x499092..0x4990F1 exactly (DISASSEMBLED). It reads [main+0x531] and its +4 unchecked,
       so both are tested first; the screen is this thread's and no call between frees it. */
    if (res.issued || res.cancelled) {
        const char* top = *(const char* const*)(w->ta + M_TOP);
        if (top && *(const char* const*)(top + 4)) ((ClickTail)0x004958C0u)(shift ? 1 : 0);
    }
    s_cCommit++; s_cIssued += res.issued; s_cMismatch += res.mismatch; s_cListCap += res.capped;
    s_cUnassigned += res.unassigned; s_cTrunc += res.more; s_cCancelled += res.cancelled; s_cStale += res.stale;
    tagpu_logf("sweep: commit kind=%s r=%d found=%u taken=%u builders=%u issued=%u unassigned=%u "
               "mismatch=%u listcap=%u cancelled=%u stale=%u shift=%d", kind_name(s_kind), r, res.found,
               res.taken, res.builders, res.issued, res.unassigned, res.mismatch, res.capped, res.cancelled,
               res.stale, shift ? 1 : 0);
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

int tagpu_sweep_cursor(int mode, int idx)
{
    TagpuWorld w;
    WQ_ENGINE e;
    WORLDQ q;
    SWEEP_IN in;
    WQ_FEAT f;
    const int* point;
    if (mode != 12 || !live(&w)) return idx;
    gesture_update(&w);
    /* the popup lies over the world: a click there picks a row (tagpu_sweep_mouse) and must read
       as no order to the engine's click, which the window procedure keeps it from anyway */
    if (pointer_on_popup(w.ta)) return CURSOR_NONE;
    if (s_kind == SWEEP_ALL) return idx;
    point = (const int*)(w.ta + M_POINT);
    /* the feature at the point and the first selected reclaimer, the representative: whether the
       resolver's answer there is of the reclaim family depends on nothing of the unit but its
       def's canreclamate bit and its player's mapped bit (tasim_sweep_order_at) */
    if (!sweep_view(&w, &e, &q, &in) || !q.ops->feature(&q, point[0] >> 20, point[2] >> 20, &f) ||
        !tasim_sweep_reclaimers(&q, &in, &s_ss) || !tasim_sweep_actions(&q, &s_ss) ||
        !tasim_sweep_order_at(&q, &s_ss, s_ss.slot[0], point)) return idx;
    /* THE FILTER IS THE CLICK'S OWN INPUT. The handler stores this answer in main+0x2CBE
       (0x4992AD) and stock's click reads that byte in the same handler call, after this store,
       for the same world point (0x499027); at 0x13 it orders nothing and keeps the mode
       (0x499041..0x49906A). 0x13 is the engine's own "nothing" and a valid sprite index (the
       cursor table holds 0x15). The feature decides first, as in the resolver: a unit standing
       on a tree's cell under METAL is not click-reclaimable, as stock would have taken the tree
       for it anyway. */
    return tasim_sweep_kind_ok(f.metal, f.energy, s_kind) ? idx : CURSOR_NONE;
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
    /* below the threshold but off the viewport (a flick onto the minimap or the panel, its moves
       merged into one record there) is no click on the ground the press chose: nothing */
    if (!d.drag) {
        if (!(*(const unsigned char*)(w.ta + M_REGION) & 2)) return 1;
        s_cClick++;
        return 2;
    }
    /* past the threshold the release commits wherever it lands -- over the panel too, which the
       route hands over in mode 12 as well: the radius is the last viewport record's */
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

/* a menu over the world: main+0x37EBE & 0x865 or main+0x2BEE & 0xE0, the order check's test
   (0x491D76..0x491DA2) */
static int menu_over(const char* ta)
{
    return (*(const uint16_t*)(ta + M_SCREENS) & 0x865u) ||
           (*(const unsigned char*)(ta + M_SCREENS2) & 0xE0u);
}

/* the modal screens and the chat line main+0x57D, which own the keyboard */
static int keyboard_taken(const char* ta)
{
    return menu_over(ta) || *(const int*)(ta + M_FIELD) != -1;
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
        if (!s_g.active || !s_lheld) return 0;
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
        if (!right) s_lheld = 0;
        return 0;
    }
    /* a press decides afresh: a taken press whose release never came (let go outside the
       window, with no capture to bring it back) must not swallow the next press's release */
    s_taken[b] = 0;
    if (right) {
        /* THE RIGHT PRESS ENDS ONLY A DRAG WHOSE RELEASE IS STILL TO COME. s_g is the handler's
           state, which reaches the release only when IdleTick hands its record over; s_lheld is
           this procedure's, set and cleared in the order the messages arrive. With both set
           the release is behind this press, so it finds the gesture ended and orders nothing;
           a press after a release passed goes to the engine, and the release commits. */
        if (!s_g.active || !s_lheld) return 0;
        end();
        s_taken[1] = 1;
        return 1;
    }
    if (live(&w) && popup_live(w.ta)) {
        tagpu_hud_to_screen(&gx, &gy);
        if (inside(s_drawn.pop, gx, gy)) {
            if ((row = row_at(gx, gy)) >= 0 && (unsigned)row != s_kind) set_kind((unsigned)row);
            s_taken[0] = 1;
            return 1;
        }
    }
    s_lheld = 1;
    return 0;
}

void tagpu_sweep_cancel(void)
{
    end();
    s_taken[0] = s_taken[1] = 0;
    s_lheld = 0;
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
        if (fx < 0 || fx > SWEEP_FOOT_MAX) fx = 1;
        if (fz < 0 || fz > SWEEP_FOOT_MAX) fz = 1;
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
    if (menu_over(ta)) return 0;
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

/* The popup as drawn, while no menu lies over the world: a menu's screen is drawn over the
   popup's place, and its clicks are the menu's. */
static int popup_live(const char* ta)
{
    return s_drawn.drawn && !menu_over(ta);
}

static int pointer_on_popup(const char* ta)
{
    int x, y;
    if (!popup_live(ta)) return 0;
    pointer_screen(&x, &y);
    return inside(s_drawn.pop, x, y);
}

int tagpu_sweep_fill(TAGPU_PK_SWEEP* h, TAGPU_PK_SWEEPITEM* it, unsigned max, unsigned* n,
                     int* cut)
{
    TagpuWorld w;
    Grid g;
    WQ_ENGINE e;
    WORLDQ q;
    SWEEP_IN in;
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
    if (armed && sweep_view(&w, &e, &q, &in) && tasim_sweep_reclaimers(&q, &in, &s_ss)) {
        if (s_g.active && s_g.drag) {
            int *c = in.c, r = radius(&s_g);
            unsigned more, nt, k;
            float metal = 0.0f, energy = 0.0f;
            centre_px(&s_g, c);
            if (coord_ok(c[0]) && coord_ok(c[1]) && coord_ok(c[2])) {
                h->flags |= TAGPU_PK_SW_RING;
                h->c[0] = c[0]; h->c[1] = c[1]; h->c[2] = c[2]; h->r = r;
                ring_items(c, r, h->col[1], it, max, n, cut);
                in.r = r;
                nt = tasim_sweep_targets(&q, &in, s_tt, SWEEP_MAX_TARGETS, &more);
                for (k = 0; k < nt; k++) {
                    TAGPU_PK_SWEEPITEM it1;
                    Feat f;
                    float fm = s_tt[k].metal, fe = s_tt[k].energy;
                    /* a wreck returns metal (a metal rock counts as one), a tree energy alone;
                       the sums are display only, and an amount that is not a sane one adds
                       nothing */
                    if (fm > 0.0f) h->nwreck++;
                    else if (fe > 0.0f) h->ntree++;
                    if (fm > 0.0f && fm < 1e7f) metal += fm;
                    if (fe > 0.0f && fe < 1e7f) energy += fe;
                    if (!feature_of(&g, s_tt[k].cell, &f)) continue;
                    box_item(&it1, TAGPU_PK_SWI_BOX, h->col[0], &g, &f, s_tt[k].pos[1] >> 16);
                    push_item(it, max, n, cut, &it1);
                }
                h->metal = metal < 1e9f ? metal : 1e9f;
                h->energy = energy < 1e9f ? energy : 1e9f;
            }
        } else if (!s_g.active && (*(const unsigned char*)(w.ta + M_REGION) & 2) &&
                   !pointer_on_popup(w.ta)) {
            const int* point = (const int*)(w.ta + M_POINT);
            SWEEP_TARGET t;
            Feat f;
            if (tasim_sweep_feature_takes(&q, point[0] >> 20, point[2] >> 20, s_kind, &t) &&
                feature_of(&g, t.cell, &f)) {
                TAGPU_PK_SWEEPITEM it1;
                box_item(&it1, TAGPU_PK_SWI_HOVER, h->col[0], &g, &f, t.pos[1] >> 16);
                if (push_item(it, max, n, cut, &it1)) h->flags |= TAGPU_PK_SW_HOVER;
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
    /* a frame whose sharp layer did not run presented no popup (tagpu_gui_present returns before
       it, or the overlay is off), so nothing of the popup is on the screen to be clicked */
    if (!c || !c->pass) memset(&s_drawn, 0, sizeof s_drawn);
    else {
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
                   "unassigned=%u trunc=%u cancelled=%u stale=%u", s_cCommit, s_cIssued, s_cClick,
                   s_cMismatch, s_cListCap, s_cUnassigned, s_cTrunc, s_cCancelled, s_cStale);
    /* outside play: a kind still pending may wait for the store's lock here */
    kind_offer(1);
    memset(&s_g, 0, sizeof s_g);
    memset(&s_drawn, 0, sizeof s_drawn);
    s_taken[0] = s_taken[1] = 0;
    s_cCommit = s_cIssued = s_cClick = s_cMismatch = s_cListCap = s_cUnassigned = s_cTrunc = 0;
    s_cCancelled = s_cStale = 0;
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
