/* tagpu_formation.c -- formation move (tagpu_formation.h).

   A drag with the button that gives orders spreads the selected units evenly along the dragged
   path, one slot each, the optimal assignment up to 256 units (tagpu_formgeo.h). A release
   shorter than the threshold is stock's click at the press point.

   Three entry points, all on the GAME thread inside the in-play mouse handler 0x499200, which
   copies the record main+0x2C76 to its stack, lets 0x498DA0 write the world point, the cell and
   the region bits, runs the hover query and the cursor chooser, and then routes the record
   (tagpu_route.c has the routing's bytes):

   - the right-press branch's stub at 0x49939B sees every record no earlier branch diverted, in
     every mode (tagpu_formation_right). Interface Type 1, nothing armed: it defers a right press
     that only moves units, follows the moves, and commits at the release. Stock gives that order
     at the press (0x499100 -> 0x48CF30 at 0x4991F5); a deferred one is given at the release.
   - the click route's stub at 0x4993BF hands it a mode-2 (Move armed) record
     (tagpu_formation_button): the left press and release of a drag, in either Interface Type.
   - the before-observer on the handler's entry ends the gesture when a record is handled in
     another mode or Interface Type (tagpu_formation_entry).

   What a commit causes -- order nodes made through the engine's own roots 0x43ADC0 and
   0x43AFC0 -- is engine state the demo recorder keeps, and no order is ever sent to a peer by
   either root or by stock's click (0x48CF30's and 0x43ADC0's call lists, DISASSEMBLED): a peer
   sees the units' moves as it sees a stock click's. Everything else here is UI state the
   simulation never reads; the render thread sees the dots only as the packet's copied values.

   OFF (tagpu_formation.on false) no gesture ever starts: every record is answered 0 and both
   stubs replay the engine's own instructions, which is stock exactly. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "tagpu_formation.h"
#include "tagpu_formgeo.h"
#include "tagpu_route.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_addr.h"
#include "tagpu_zoom.h"
#include "tagpu_vpwide.h"
#include "tagpu_resource.h"
#include "tagpu_opt.h"
#include "tagpu_log.h"
#include "dd.h"

_Static_assert(FORM_MAX_UNITS >= TAGPU_LIM_UNITS, "every unit of a player can take a slot");

#define D_STRIDE    0x249u
#define U_FLAGS     0x110
#define U_ORDERS    0x5C
#define N_ACTION    0x04
#define N_OWNER     0x0E
#define N_TARGET    0x16       /* the target unit, when the node has one */
#define N_POS       0x22       /* x, altitude, z, 16.16 */
#define N_NEXT      0x4A
#define M_CELL      0x2C8E     /* dword: words {x >> 20, z >> 20}                  */
#define M_POINT     0x2CAA     /* 16.16 {x, altitude, z}                           */
#define M_HOVER     0x2CBA     /* u16 the hovered unit                             */
#define M_HFEAT     0x2CBC     /* u16 the hovered feature                          */
#define M_CURSOR    0x2CBE     /* u8 the chooser's cursor index                    */
#define M_MODE      0x2CC3
#define M_REGION    0x2CC6     /* bit 1 the world view, bit 3 a box is open        */
#define M_ITYPE     0x37EFA    /* i32 Interface Type: 0 left-click, else right-click */
#define M_TOP       0x531      /* the top screen; its +4 is its ControlsAry        */
#define M_MAPW      0x14223    /* i32 the map's size in world px                   */
#define M_MAPH      0x14227
#define M_CURSORARY 0x1487F    /* the cursor table: GAF sequence pointers          */

#define VA_RIGHT_CALL 0x004993ACu   /* call 0x499100, the right press  */
#define VA_LEFT_CALL  0x004995B3u   /* call 0x498F70, the left click   */
#define VA_ORDER      0x0043ADC0u
#define VA_TOGGLE     0x0043AFC0u
#define VA_RESOLVE    0x0043F0E0u
#define VA_ACTION     0x00438760u
#define VA_TAIL       0x004958C0u
#define VA_BLIT       0x004B8500u
#define VA_GROUP      0x0048CF30u

#define CURSOR_MOVE 14             /* cursor_ary index of Move_Ground, VTOL_Move and QMove */
#define LIST_WALK      16384u
/* A Shift-appended list stops growing at FORM_LIST_CAP nodes, below the 4096 a recorded list
   holds (REC_ORDERS_MAX, tagpu_recfmt.h), as PLACE_LIST_CAP and SWEEP_LIST_CAP. */
#define FORM_LIST_CAP  1024u
#define GDI_COORD_MAX  0x100000

enum { FB_RIGHT = 1, FB_LEFT = 2 };
enum { A_MOVE, A_VTOL, A_QMOVE };

typedef void (__stdcall* Click)(int* rec);
typedef void (__stdcall* Order)(int, int, void*, void*, const void*, int, int);
typedef const unsigned char* (__stdcall* Resolve)(int*, unsigned, void*, void*, const void*);
typedef unsigned char* (__attribute__((thiscall))* Action)(void*, const char*);
typedef void (__stdcall* ClickTail)(int);
typedef void (__stdcall* Blit)(void* ctx, const void* frame, int x, int y);
typedef void (__stdcall* GroupOrder)(int* rec, int mode, int action, const void* pos, int, int);

/* The per-record fields a click depends on: exactly what 0x498DA0, the hover query and the
   cursor chooser write for a record before the routing (DISASSEMBLED: 0x498E19..0x498F5D,
   0x499283, 0x4992AD). Everything else a click reads -- the selection, the units, the mode, the
   Interface Type -- is live state. */
typedef struct {
    uint32_t      cell;
    int32_t       point[3];
    uint16_t      hover_unit, hover_feat;
    unsigned char cursor, region;   /* region: main+0x2CC6 bits 0..2 only */
} Snap;

typedef struct { uint16_t slot; uint8_t action; } Part;

static int s_armed;
static Click s_clickRight, s_clickLeft;
static unsigned char s_act[3];
static int s_actOk;

static struct {
    int active, button, mode, itype;
    int keep;                      /* mode 2: main+0x2CC6 bit 5 at the press, a Shift-kept Move */
    int shed;                      /* the engine disarmed that Move while the button was held   */
    int press[6];                  /* the press record, by value                               */
    Snap at_press, at_last;        /* at the press; at the last record in the world view       */
    int stale[6];                  /* tagpu_route_stale_move at the press                      */
    unsigned m;  FormPt node[FORM_MAX_NODES];
    unsigned n;  Part part[FORM_MAX_UNITS];        /* the units that move: MOVE_GROUND, VTOL_MOVE */
    unsigned nq; uint16_t qslot[FORM_MAX_UNITS];   /* QMOVE units (no mover), mode 2 only        */
    int shown; unsigned ndot; FormPt dot[FORM_MAX_UNITS];  /* the preview                         */
} s_g;

/* the commit's working set: one participant per index, in ascending slot order */
static uint16_t s_cSlot[FORM_MAX_UNITS], s_slotOf[FORM_MAX_UNITS];
static uint8_t  s_cAct[FORM_MAX_UNITS];
static FormPt   s_start[FORM_MAX_UNITS], s_target[FORM_MAX_UNITS];

/* the level's counters, logged at level end */
static unsigned s_cCommit, s_cShort, s_cSingle, s_cDrop, s_cListCap, s_cGdi;

static char* main_block(void) { return *(char**)TA_MAIN_PP; }

static int ptr_ok(const void* p)
{
    return (size_t)p > 0x10000u && (size_t)p <= tagpu_user_top();
}

static int interface_type(const char* ta) { return *(const int*)(ta + M_ITYPE) != 0; }

static int formation_on(void) { return tagpu_opt_on("tagpu_formation.on"); }

/* the move orders' bytes, 0x438760's answer for the engine's own names at 0x5052E4
   MOVE_GROUND, 0x5016CC VTOL_MOVE and 0x50531C QMOVE, the strings the resolver passes it
   (0x43F845..0x43F854, 0x4401C2); 0 for a name it does not find */
static int actions(void)
{
    if (!s_actOk) {
        ((Action)VA_ACTION)(&s_act[A_MOVE], (const char*)0x005052E4u);
        ((Action)VA_ACTION)(&s_act[A_VTOL], (const char*)0x005016CCu);
        ((Action)VA_ACTION)(&s_act[A_QMOVE], (const char*)0x0050531Cu);
        s_actOk = s_act[A_MOVE] && s_act[A_VTOL] && s_act[A_QMOVE];
    }
    return s_actOk;
}

/* The order the resolver 0x43F0E0 gives `u` in cursor mode `mode` with no target unit at
   `pos`: its out byte, 0 on refusal (0x4401E3). Mode 2 answers QMOVE for a unit with no mover,
   else VTOL_MOVE or MOVE_GROUND (0x43F845); mode 1 at Interface Type 1 answers a reclaim or
   resurrect when a feature at `pos` allows it, else the same move a mover gets in mode 2 and 0
   for a unit with no mover (0x43F9E9, 0x43FDFF..0x43FE30). DISASSEMBLED. */
static unsigned char resolve(unsigned mode, char* u, const int* pos)
{
    int out = -1;
    const unsigned char* a = ((Resolve)VA_RESOLVE)(&out, mode, u, NULL, pos);
    return a ? *a : 0;
}

/* inside the map: the map's own size (main+0x14223, +0x14227), accepted only in 1..32767,
   the range every 16.16 TA position has */
static int in_map(const char* ta, const int32_t* p)
{
    int w = *(const int*)(ta + M_MAPW), h = *(const int*)(ta + M_MAPH);
    if (w < 1 || w > 32767 || h < 1 || h > 32767) return 0;
    return p[0] >= 0 && p[2] >= 0 && (p[0] >> 16) < w && (p[2] >> 16) < h;
}

static void snap_take(const char* ta, Snap* s)
{
    s->cell = *(const uint32_t*)(ta + M_CELL);
    memcpy(s->point, ta + M_POINT, sizeof s->point);
    s->hover_unit = *(const uint16_t*)(ta + M_HOVER);
    s->hover_feat = *(const uint16_t*)(ta + M_HFEAT);
    s->cursor = (unsigned char)ta[M_CURSOR];
    s->region = (unsigned char)ta[M_REGION] & 0x07;
}

/* Bits 3..7 of main+0x2CC6 (a box, the minimap held, keep-armed, the site is free) are live
   state and never written here. */
static void snap_put(char* ta, const Snap* s)
{
    *(uint32_t*)(ta + M_CELL) = s->cell;
    memcpy(ta + M_POINT, s->point, sizeof s->point);
    *(uint16_t*)(ta + M_HOVER) = s->hover_unit;
    *(uint16_t*)(ta + M_HFEAT) = s->hover_feat;
    ta[M_CURSOR] = (char)s->cursor;
    ta[M_REGION] = (char)(((unsigned char)ta[M_REGION] & ~0x07u) | s->region);
}

static FormPt point_of(const char* ta)
{
    FormPt p;
    memcpy(&p, ta + M_POINT, sizeof p);
    return p;
}

/* THE GESTURE'S MODE STILL HOLDS. A Shift-kept Move (main+0x2CC6 bit 5, set by a Shift click's
   tail 0x4958C0) is disarmed by the handler itself at the first record that finds Shift up
   (0x4992D8..0x49930F: mode 1, bit 5 cleared), which runs before the routing for that record.
   Stock had already ordered at the press, so a left gesture admitted under such a Move outlives
   that disarm (`shed`) and still commits at the left release, with Move's orders and without
   the tail the engine has already run. Any other change of mode or Interface Type ends it. */
static int still_armed(const char* ta)
{
    unsigned mode = (unsigned char)ta[M_MODE];
    if (interface_type(ta) != s_g.itype) return 0;
    if (!s_g.shed && mode == (unsigned)s_g.mode) return 1;
    if (s_g.button == FB_LEFT && s_g.keep && mode == 1 && !((unsigned char)ta[M_REGION] & 0x20)) {
        s_g.shed = 1;
        return 1;
    }
    return 0;
}

static void drop(const char* reason)
{
    if (!s_g.active) return;
    memset(&s_g, 0, sizeof s_g);
    s_cDrop++;
    tagpu_logf("formation: drop reason=%s", reason);
}

/* A participant at `slot`: alive and not excluded (tagpu_route_unit bounds the slot by the
   array's own count), the controlled player's, selected (+0x110 bit 4), with a def (its index
   below UNITINFOCount). NULL otherwise. */
static char* participant(const TagpuWorld* w, unsigned slot)
{
    char* u = tagpu_route_unit(w, slot);
    if (!u || *(const unsigned char*)(u + U_OWNER) != (unsigned)w->player ||
        !(*(const unsigned*)(u + U_FLAGS) & 0x10u) || !tagpu_route_def_id(w, u)) return NULL;
    return u;
}

/* THE CLICK THRESHOLD, BAR's (luaintro/springconfig.lua's MouseDragFrontCommandThreshold, and
   the widget's minFormationLength): base = 16 screen px at 1080 lines, scaled with the
   presented height and clamped to 8..40, plus 16; never below 20 wu. The presented px become
   game px through the mouse's unscale, and game px become wu through the zoom (one game px is
   1/zoom wu, tagpu_zoom_to_engine). TA's projection is orthographic (0x469C63..0x469C94), so on
   level ground one game px is one wu on both axes; a slope changes z's ratio and is ignored.
   The viewport height and the unscale are the presentation's (dd.c / the render thread writes
   them on a mode change); read here as values, each range-checked, so an old or new value
   costs at most one gesture's threshold. */
static int threshold_wu(void)
{
    int h = g_ddraw.render.viewport.height, base;
    float un = g_ddraw.mouse.unscale_y, z = tagpu_zoom_applied_level(), wu;
    if (h < 1 || h > 16384) h = 1080;
    if (!(un > 0.0f && un <= 64.0f)) un = 1.0f;
    if (!(z >= TAGPU_ZOOM_FLOOR_LO && z <= 16.0f)) z = 1.0f;
    base = (16 * h + 540) / 1080;
    if (base < 8) base = 8;
    if (base > 40) base = 40;
    wu = (float)(base + 16) * un / z;
    return wu > 20.0f ? (int)(wu + 0.5f) : 20;
}

/* The descriptor vector [0x512344]..[0x512348], stride 0x19, fixed for the level. A node's
   action indexes it only below its own length. */
static const unsigned char* descriptor(unsigned action)
{
    const char* first = *(const char* const*)0x00512344u;
    const char* last = *(const char* const*)0x00512348u;
    uintptr_t bytes;
    if (!first || (uintptr_t)last < (uintptr_t)first) return NULL;
    bytes = (uintptr_t)last - (uintptr_t)first;
    if (bytes % 0x19u || action >= bytes / 0x19u) return NULL;
    return (const unsigned char*)first + action * 0x19u;
}

/* `p` as a live unit of the array: on a stride boundary, its slot below the array's count,
   alive. NULL otherwise. */
static const char* unit_at(const TagpuWorld* w, const char* p)
{
    uintptr_t off;
    if ((uintptr_t)p < (uintptr_t)w->units) return NULL;
    off = (uintptr_t)p - (uintptr_t)w->units;
    if (off % UNIT_STRIDE) return NULL;
    return tagpu_route_unit(w, (unsigned)(off / UNIT_STRIDE));
}

/* WHERE A UNIT'S QUEUE ENDS, the start of its path under Shift. The walk mirrors the order
   markers' walker 0x439B30: a node has a drawn position when its descriptor's marker mask
   (+0xC bits 0..3) is non-zero, and the position is its target unit's when the node has one,
   else the node's own. The last such node wins; none, the unit's own position. At most
   LIST_WALK nodes, stopping at a node that is not this unit's (+0x0E); the action is bounded by
   the descriptor vector's own length, the target by the unit array. `*len` is the walk's count,
   for FORM_LIST_CAP. */
static FormPt queue_end(const TagpuWorld* w, const char* u, unsigned* len)
{
    FormPt p;
    const char* node;
    unsigned steps = 0;
    memcpy(&p, u + U_XFIX, sizeof p);
    for (node = *(const char* const*)(u + U_ORDERS); node && steps < LIST_WALK;
         node = *(const char* const*)(node + N_NEXT), steps++) {
        const unsigned char* d;
        const char* t;
        if (*(const char* const*)(node + N_OWNER) != u) break;
        if (!(d = descriptor((unsigned char)node[N_ACTION])) ||
            !(*(const unsigned*)(d + 0x0C) & 0x0Fu)) continue;
        t = *(const char* const*)(node + N_TARGET);
        if (t && (t = unit_at(w, t))) memcpy(&p, t + U_XFIX, sizeof p);
        else memcpy(&p, node + N_POS, sizeof p);
    }
    *len = steps;
    return p;
}

/* The live participants among those taken at the press, in ascending slot order: still the
   player's, alive and selected. */
static unsigned live_count(const TagpuWorld* w)
{
    unsigned i, k = 0;
    for (i = 0; i < s_g.n; i++) k += participant(w, s_g.part[i].slot) != NULL;
    return k;
}

static void preview(void)
{
    TagpuWorld w;
    unsigned live;
    s_g.shown = 0;
    if (s_g.m < 2 || !tagpu_route_world(&w)) return;
    live = live_count(&w);
    if (live < 2) return;
    form_slots(s_g.node, s_g.m, live, s_g.dot);
    s_g.ndot = live;
    s_g.shown = 1;
}

/* THE PRESS. A press starts a gesture only when every unit it would order would move: the world
   view (region bit 1) and no box open (bit 3); the other button not held; no unit under the
   pointer (stock's target rules stand there: attack, guard, load, repair); in mode 2 a cursor
   stock's click issues an order with (0x499027..0x499044: not 0x0F the select, below 0x11);
   a point inside the map; and every selected unit's order MOVE_GROUND or VTOL_MOVE, or in mode
   2 QMOVE (a factory's rally), or nothing (mode 1: a unit with no mover is given nothing by
   stock either). Any other answer -- a reclaim under the pointer, say -- leaves the press to
   stock, unchanged; so does a right press Quick Build's gate on 0x499100 would turn into a build
   (tagpu_resource_quick_right), which the player sees as the build ghost under the pointer.
   The scan fills s_g's unit lists as it goes; the gesture exists only once s_g.active is set,
   at the end. */
static int admit(char* ta, const int* rec, int button, int mode, int itype)
{
    TagpuWorld w;
    const int32_t* p = (const int32_t*)(ta + M_POINT);
    unsigned region = (unsigned char)ta[M_REGION], slot, n = 0, nq = 0;
    if (!(region & 0x02) || (region & 0x08)) return 0;
    if (rec[2] & (button == FB_RIGHT ? MK_LBUTTON : MK_RBUTTON)) return 0;
    if (*(const uint16_t*)(ta + M_HOVER)) return 0;
    if (mode == 2) {
        unsigned c = (unsigned char)ta[M_CURSOR];
        if (c >= 0x11 || c == 0x0F) return 0;
    }
    if (button == FB_RIGHT && tagpu_resource_quick_right()) return 0;
    if (!in_map(ta, p) || !tagpu_route_world(&w) || !actions()) return 0;
    for (slot = w.first; slot <= w.last; slot++) {
        char* u = participant(&w, slot);
        unsigned char a;
        if (!u) continue;
        a = resolve((unsigned)mode, u, p);
        if (a == s_act[A_MOVE] || a == s_act[A_VTOL]) {
            if (n == FORM_MAX_UNITS) return 0;
            s_g.part[n].slot = (uint16_t)slot; s_g.part[n].action = a; n++;
        } else if (a == s_act[A_QMOVE] && mode == 2) {
            if (nq == FORM_MAX_UNITS) return 0;
            s_g.qslot[nq++] = (uint16_t)slot;
        } else if (a) return 0;
    }
    if (!n) return 0;
    s_g.n = n; s_g.nq = nq;
    s_g.active = 1; s_g.button = button; s_g.mode = mode; s_g.itype = itype;
    s_g.keep = mode == 2 && ((unsigned char)ta[M_REGION] & 0x20); s_g.shed = 0;
    memcpy(s_g.press, rec, sizeof s_g.press);
    snap_take(ta, &s_g.at_press);
    s_g.at_last = s_g.at_press;
    tagpu_route_stale_move(s_g.stale);
    s_g.m = form_path_add(s_g.node, 0, point_of(ta));
    s_g.shown = 0; s_g.ndot = 0;
    return 1;
}

/* A move while the gesture is held. A move without its button that is not the stale pre-press
   move IdleTick re-reads (tagpu_route.c, tagpu_route_same_move) is a release the handler never
   saw: the gesture is dropped and nothing is ordered. */
static void track(const char* ta, const int* rec)
{
    unsigned bit = s_g.button == FB_RIGHT ? MK_RBUTTON : MK_LBUTTON;
    if (!(rec[2] & bit)) {
        if (!tagpu_route_same_move(rec, s_g.stale)) drop("lost");
        return;
    }
    if (!((unsigned char)ta[M_REGION] & 0x02) || !in_map(ta, (const int32_t*)(ta + M_POINT)))
        return;
    s_g.m = form_path_add(s_g.node, s_g.m, point_of(ta));
    snap_take(ta, &s_g.at_last);
    preview();
}

/* STOCK'S CLICK, RUN AT THE RELEASE. The record `rec6` (the press's, or the release's) is
   handed to the very function stock's call site calls -- the recorder's click stub when it is
   installed, else 0x499100 or 0x498F70 (tagpu_formation_init); for a shed Move, the group
   order 0x48CF30 that 0x498F70 calls -- with the per-record fields of
   the record it stands for written over the release's for the call and the release's put back
   after it.
   INVARIANT, AN ORDERING: this runs on the game thread inside the handler for the release
   record, after 0x498DA0, the hover query and the chooser have written the release's fields and
   before anything else of the handler reads them. Snap is exactly those fields
   (DISASSEMBLED), which are the only per-record inputs of either click, of the resolver they
   call and of the resource module's gate on them; both clicks read the record only at +8
   (0x49917A; 0x498F70 passes it on to 0x419670, 0x48C7F0, 0x48CF30, which read +8 alone). The
   one arm of either click that writes a Snap field is 0x499100's Interface Type 0 minimap arm
   (main+0x2CBE and +0x2CC6 bit 4, 0x49919D..0x4991B4), which a replay never reaches: a right
   gesture exists only at Type 1 and is dropped when the type changes. So restoring the fields
   undoes only this function's own writes. The
   render thread reads none of them (it reads the frame packet), and the recorder leaves
   main+0x2AA7..+0x2CC6 out of a recording (replay-globals.tsv), so the transient values are
   invisible to both. tagpu_resource_mouse re-samples Quick Build's raw point from the restored
   point, and only where vpwide's redirect sampled it for this record in the first place. */
static void replay(char* ta, const int rec6[6], const Snap* s, int button, int shed)
{
    Snap cur;
    int copy[6];
    snap_take(ta, &cur);
    snap_put(ta, s);
    if (tagpu_vpwide_mouse_world_live()) tagpu_resource_mouse();
    memcpy(copy, rec6, sizeof copy);
    /* A shed Move (still_armed) is mode 1 now, where 0x498F70 would select: the call it makes
       in mode 2 is made instead, 0x48CF30(rec, 2, 0, main+0x2CAA, 0, 0) (0x49906D..0x499087),
       without the tail the engine already ran. It records no click. */
    if (shed) ((GroupOrder)VA_GROUP)(copy, 2, 0, ta + M_POINT, 0, 0);
    else (button == FB_RIGHT ? s_clickRight : s_clickLeft)(copy);
    snap_put(ta, &cur);
    if (tagpu_vpwide_mouse_world_live()) tagpu_resource_mouse();
}

/* THE RELEASE. The participants are re-derived from their slots (the selection may have
   changed while the button was held), their starts taken, the slots laid along the path and
   assigned, and each unit given its slot through the order constructor 0x43ADC0 -- with Shift
   an append, without it a replace, as stock's 0x43AFC0 forwards a no-Shift click
   (0x43AFDA -> 0x43B08A). 0x43AFC0 is not used for them: with Shift it removes a same-action
   node within 16 wu first, which would let one unit's slot cancel its own queued move.
   No sound is played here: 0x43ADC0 sets the node's +0x42 bit 0x2000 without Shift (0x43AEE7)
   and the move handlers answer it with the unit's "ok" through 0x438880 (0x40322D, 0x40FA6B),
   whose queue refuses a reply already queued (0x47FB0A) -- once a group, none under Shift,
   exactly as stock's click.
   Every unit is re-derived from its slot before each root call (tagpu_route_unit bounds the
   slot by the array's count; the owner, the selection, the def index below UNITINFOCount), and
   no node pointer is held across a call. */
static void commit(char* ta, const int* rec)
{
    TagpuWorld w;
    int64_t len;
    unsigned i, live = 0, issued = 0, qissued = 0, listcap = 0, mode, button;
    int thr, shift, algo = '-', shed = s_g.shed;
    if (((unsigned char)ta[M_REGION] & 0x02) && in_map(ta, (const int32_t*)(ta + M_POINT))) {
        s_g.m = form_path_add(s_g.node, s_g.m, point_of(ta));
        snap_take(ta, &s_g.at_last);
    }
    s_g.active = 0; s_g.shown = 0;
    mode = (unsigned)s_g.mode; button = (unsigned)s_g.button;
    len = form_path_length(s_g.node, s_g.m);
    thr = threshold_wu();
    if (len < ((int64_t)thr << 16)) {
        replay(ta, s_g.press, &s_g.at_press, (int)button, shed);
        s_cShort++;
        tagpu_logf("formation: short button=%s len=%d thr=%d", button == FB_RIGHT ? "right" : "left",
                   (int)(len >> 16), thr);
        return;
    }
    shift = (rec[2] & MK_SHIFT) ? 1 : 0;
    if (!tagpu_route_world(&w)) { s_cDrop++; tagpu_logf("formation: drop reason=world"); return; }
    for (i = 0; i < s_g.n; i++) {
        const char* u = participant(&w, s_g.part[i].slot);
        unsigned qlen;
        FormPt st;
        if (!u) continue;
        st = shift ? queue_end(&w, u, &qlen) : *(const FormPt*)(u + U_XFIX);
        if (!in_map(ta, &st.x)) continue;
        s_cSlot[live] = s_g.part[i].slot; s_cAct[live] = s_g.part[i].action;
        s_start[live] = st;
        live++;
    }
    if (live <= 1) {
        replay(ta, rec, &s_g.at_last, (int)button, shed);
        s_cSingle++;
        tagpu_logf("formation: single button=%s units=%u", button == FB_RIGHT ? "right" : "left",
                   live);
        return;
    }
    form_slots(s_g.node, s_g.m, live, s_target);
    algo = form_assign(s_start, s_target, live, s_slotOf);
    for (i = 0; i < live; i++) {
        char* u = participant(&w, s_cSlot[i]);
        unsigned qlen = 0;
        if (!u) continue;
        if (shift) {
            (void)queue_end(&w, u, &qlen);
            if (qlen >= FORM_LIST_CAP) { listcap++; continue; }
        }
        ((Order)VA_ORDER)(s_cAct[i], shift, u, NULL, &s_target[s_slotOf[i]], 0, 0);
        issued++;
    }
    if (mode == 2) {
        /* A unit with no mover (a factory) gets stock's own call for it: 0x48CF30 hands QMOVE
           to 0x43AFC0 with the click's point unshifted, QMove's descriptor lacking the group
           offset's +0x11 bit 1 (0x48D121..0x48D126). Its rally is the path's end, the release
           point. FORM_LIST_CAP holds for its list as for a mover's. */
        for (i = 0; i < s_g.nq; i++) {
            char* u = participant(&w, s_g.qslot[i]);
            unsigned qlen = 0;
            if (!u) continue;
            if (shift) {
                (void)queue_end(&w, u, &qlen);
                if (qlen >= FORM_LIST_CAP) { listcap++; continue; }
            }
            ((Order)VA_TOGGLE)(s_act[A_QMOVE], shift, u, NULL, &s_g.node[s_g.m - 1], 0, 0);
            qissued++;
        }
        /* STOCK'S OWN TAIL OF A MODE-2 CLICK, 0x4958C0(keep): with Shift main+0x2CC6 |= 0x20,
           so releasing Shift disarms at the next record (0x4992D8); without it mode 1, bit 5
           cleared and STOP's radio group reset -- 0x499092..0x4990F1 exactly (DISASSEMBLED). It
           reads [main+0x531] and its +4 unchecked, so both are tested first; the screen is this
           thread's and no call between frees it. */
        if (!shed) {
            const char* top = *(const char* const*)(ta + M_TOP);
            if (top && *(const char* const*)(top + 4)) ((ClickTail)VA_TAIL)(shift);
        }
    }
    s_cCommit++; s_cListCap += listcap;
    tagpu_logf("formation: commit button=%s mode=%u itype=%u units=%u qmove=%u nodes=%u len=%d "
               "thr=%d shift=%d algo=%c issued=%u listcap=%u shed=%d",
               button == FB_RIGHT ? "right" : "left", mode, (unsigned)s_g.itype, live, qissued,
               s_g.m, (int)(len >> 16), thr, shift, algo, issued, listcap, shed);
}

int tagpu_formation_right(const int* rec, unsigned msg)
{
    char* ta;
    int mode, itype;
    if (!s_armed) return 0;
    if (!(ta = main_block())) { drop("nomain"); return 0; }
    mode = (unsigned char)ta[M_MODE];
    itype = interface_type(ta);
    if (s_g.active && !still_armed(ta)) drop("mode");
    switch (msg) {
    case WM_RBUTTONDOWN:
        /* another press of the gesture's own button is a release the handler never saw */
        drop("other");
        if (itype != 1 || mode != 1 || !formation_on()) return 0;
        return admit(ta, rec, FB_RIGHT, 1, itype);
    case WM_LBUTTONDOWN:
        /* the other button cancels the gesture (BAR); a left gesture's own press is the click
           route's, which sees this record next -- except a shed Move's, which the route no
           longer sees: that press is a release the handler never saw */
        if (s_g.active && (s_g.button == FB_RIGHT || s_g.shed)) drop("other");
        return 0;
    case WM_MOUSEMOVE:
        if (s_g.active) track(ta, rec);
        return 0;
    case WM_LBUTTONUP:
        /* a shed Move's release arrives in mode 1, past the click route: committed here, then
           handed on to stock's mode-1 path, which stock's own release reached too */
        if (s_g.active && s_g.button == FB_LEFT && s_g.shed) commit(ta, rec);
        return 0;
    case WM_RBUTTONUP:
        /* the release still reaches stock afterwards, which does nothing with it in mode 1 */
        if (s_g.active && s_g.button == FB_RIGHT) commit(ta, rec);
        return 0;
    default:
        return 0;
    }
}

/* A mode-2 release reaches stock's frame (0x4993C5), so consuming it is stock's own path. */
int tagpu_formation_button(const int* rec, unsigned msg)
{
    char* ta;
    if (!s_armed || !(ta = main_block())) return 0;
    if (msg == WM_LBUTTONDOWN) {
        drop("other");
        if ((unsigned char)ta[M_MODE] != 2 || !formation_on()) return 0;
        return admit(ta, rec, FB_LEFT, 2, interface_type(ta));
    }
    if (msg == WM_LBUTTONUP && s_g.active && s_g.button == FB_LEFT) {
        commit(ta, rec);
        return 1;
    }
    return 0;
}

void tagpu_formation_entry(void)
{
    const char* ta = main_block();
    if (!s_g.active) return;
    if (!ta || !still_armed(ta)) drop("mode");
}

unsigned tagpu_formation_dots(TagpuFormDot* out, unsigned max, int* cut)
{
    const char* ta = main_block();
    unsigned i;
    *cut = 0;
    if (!s_armed || !s_g.active || !s_g.shown || !ta || !still_armed(ta)) return 0;
    for (i = 0; i < s_g.ndot && i < max; i++) {
        out[i].x = s_g.dot[i].x >> 16;
        out[i].alt = s_g.dot[i].alt >> 16;
        out[i].z = s_g.dot[i].z >> 16;
    }
    *cut = s_g.ndot > max;
    return i;
}

/* GDI: the move waypoint sprite at each dot, as the engine's order-marker drawer 0x439740 draws
   one (0x439948..0x4399BB, DISASSEMBLED): the cursor table's sequence 14, frame
   k = (GameTime / (2 * duration)) % count, where the engine divides by both u16s unguarded --
   here a zero count draws nothing and a zero duration counts as 1; the position
   x - eyeX + 0x80, z - (altitude >> 1) - eyeY + 0x20; 0x4B8500(ctx, frame, x, y), which clips.
   The cursor table is session data its loader 0x429870 fills at start-up, before any marker
   block. Game thread, inside DrawGameScreen. */
void tagpu_formation_gdi(void* ctx)
{
    const char* ta = main_block();
    const unsigned char* seq;
    const void* frame;
    unsigned count, dur, k, i, gt;
    int ex, ey;
    if (!s_armed || !s_g.active || !s_g.shown || !ctx || !ta) return;
    seq = *(const unsigned char* const*)(ta + M_CURSORARY + 4 * CURSOR_MOVE);
    if (!ptr_ok(seq)) return;
    count = *(const uint16_t*)seq;
    dur = *(const uint16_t*)(seq + 0x2C);
    if (!count) return;
    if (!dur) dur = 1;
    gt = *(const unsigned*)(ta + OFF_GAMETIME);
    k = (gt / (2 * dur)) % count;
    frame = *(const void* const*)(seq + 0x28 + 8 * k);
    if (!ptr_ok(frame)) return;
    ex = *(const int*)(ta + OFF_EYE_X);
    ey = *(const int*)(ta + OFF_EYE_Y);
    for (i = 0; i < s_g.ndot; i++) {
        int64_t x = (int64_t)(s_g.dot[i].x >> 16) - ex + 0x80;
        int64_t y = (int64_t)(s_g.dot[i].z >> 16) - ((s_g.dot[i].alt >> 16) >> 1) - ey + 0x20;
        if (x < -GDI_COORD_MAX || x > GDI_COORD_MAX || y < -GDI_COORD_MAX || y > GDI_COORD_MAX)
            continue;
        ((Blit)VA_BLIT)(ctx, frame, (int)x, (int)y);
        s_cGdi++;
    }
}

void tagpu_formation_level_end(void)
{
    if (s_armed)
        tagpu_logf("formation: level end commits=%u short=%u single=%u drops=%u listcap=%u gdi=%u",
                   s_cCommit, s_cShort, s_cSingle, s_cDrop, s_cListCap, s_cGdi);
    memset(&s_g, 0, sizeof s_g);
    s_actOk = 0;
    s_cCommit = s_cShort = s_cSingle = s_cDrop = s_cListCap = s_cGdi = 0;
}

/* The click's call-site target: `E8 rel32` at `site`, decoded. THE RECORDER IS THE ONLY WRITER
   of the two rel32s (a grep of src/), and it writes them in tagpu_rec_init, which DllMain runs
   before tagpu_route_init and this; nothing writes them after attach. So the decoded target is
   the one stock's call reaches for the rest of the process: the recorder's click stub
   (stdcall(record), recording the click as stock's call would) when it installed, else the
   engine's function, stdcall(record) itself (`ret 4`, its caller pushes the record and pops
   nothing). Either passes the resource module's gate on the function's entry. */
static Click decode(unsigned site)
{
    const unsigned char* p = (const unsigned char*)(uintptr_t)site;
    int32_t rel;
    if (p[0] != 0xE8) return NULL;
    memcpy(&rel, p + 1, 4);
    return (Click)(uintptr_t)(site + 5 + (unsigned)rel);
}

void tagpu_formation_init(void)
{
    if (!tagpu_route_ok(TAGPU_ROUTE_STUB | TAGPU_ROUTE_ENTRY | TAGPU_ROUTE_RIGHT)) {
        tagpu_logf("formation: not armed (a route site is not installed)");
        return;
    }
    s_clickRight = decode(VA_RIGHT_CALL);
    s_clickLeft = decode(VA_LEFT_CALL);
    if (!s_clickRight || !s_clickLeft) {
        tagpu_logf("formation: not armed (a click's call site is not a call)");
        return;
    }
    s_armed = 1;
    tagpu_logf("formation: armed (right 0x49939B, route 0x4993BF, entry 0x499200; clicks "
               "0x%08X 0x%08X)", (unsigned)(uintptr_t)s_clickRight,
               (unsigned)(uintptr_t)s_clickLeft);
}
