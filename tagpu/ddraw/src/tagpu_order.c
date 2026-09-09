/* tagpu_order.c — the shift-held order-marker overlay, ported (G13o).
   See tagpu_order.h for why it is a port and not a capture, and for the
   two-thread split. Everything below is transcribed from the disassembly of
   the engine's own driver, walker and five leaf drawers; the addresses and the
   field lists are in research/notes/ui-markers.md 3 and
   exe-reverse-engineering.md.

   THE ENGINE'S SIDE, IN ONE PLACE.

   Driver `0x48CC30` (sole caller `0x469BFC`, behind the SHIFT probe at
   `0x469BE1`) walks the WATCHED player's unit range and picks a capability
   mask and a flag per unit:

     the CameraToUnit (view+0), or a unit whose UnitInGameIndex (+0xA8) equals
     the tracked id (main+0x37E9C) or the hovered id (main+0x2CBA)
                                          -> mask 0x1F, flag 1
     selected (stateMask & 0x10)          -> mask 0x1F, flag 0
     anything else, IFF one of those three units is a builder
     (its UnitDef+0x156 CANBUILD_ptr is non-null)
                                          -> mask 0x01, flag 1
     -> 0x439B30(unit, mask, ctx, view, flag)

   That last row is the "hover a constructor, see EVERYONE's claimed build
   sites" rule. Factory rally points and queued factory orders are ordinary
   entries in the factory's order list and come out of the same walk.

   Walker `0x439B30` chains a position `pos`, initialised to the unit's own
   16.16 triple at +0x6A, down the order list (head +0x5C, next node+0x4A). Per
   node the order type byte node+0x4 indexes the order-descriptor table behind
   `*(0x512344)` (end `*(0x512348)`, stride 0x19); `entry+0xC` is that order
   type's marker-capability mask, ANDed with the caller's:

     bit 0  0x438C00  queued build-site footprint rect
     bit 1  0x4394E0  the marching dotted route line — but it first delegates
                      to the bit-3 drawer, unconditionally, and only draws the
                      dots when flag == 1
     bit 2  0x4399F0  circle around the order target
     bit 3  0x439740  animated sprite at the order target
     bit 4  0x4390A0  range circles — once per unit, behind a guard flag

   `pos` IS RESTORED TO THE NODE'S ENTRY VALUE BEFORE EACH OF BITS 0..3 and is
   NOT restored before bit 4 [BINARY-VERIFIED: the three `mov [esp+0x24..0x2c]`
   stores appear ahead of the calls at 0x439BAC/0x439BF2/0x439C37/0x439C7D and
   not ahead of 0x439CBE]. So within one node every drawer sees the same
   starting point and the chain advances by whichever of bits 0..3 ran LAST.

   The five drawers, byte for byte:

   BUILD SITE `0x438C00`. Nothing unless node+0x36 (the build target's unit
   type id) is non-zero. def = UnitDefs(main+0x1439B) + type*0x249; the two
   corners are the node's target position plus the load-time footprint extents

     P0 = (t.x + def[0x15E],  t.y + def[0x162],  t.z + def[0x166])
     P1 = (t.x + def[0x16A],  --                 t.z + def[0x172])

   projected with P0's altitude for both:

     x0 = (s16)(P0.x>>16) - eyeX + 0x80      x1 = (s16)(P1.x>>16) - eyeX + 0x80
     z0 = (s16)(P0.z>>16) - alt/2 - eyeY + 0x20      alt = (s16)(P0.y>>16)
     z1 = (s16)(P1.z>>16) - alt/2 - eyeY + 0x20

   then an animation that SWEEPS the four edges inward over the first ten game
   ticks after the order was issued:

     t   = (unsigned)(gameTime - node[0x46]) >= 10 ? 10 : that     (so a
           negative age reads as "finished", which is the unsigned compare)
     dxg = (x1-x0)*t/10       dzg = (z1-z0)*t/10
     xg0 = x0+dxg   xg1 = x1-dxg   zg0 = z0+dzg   zg1 = z1-dzg

   Eight DrawLines: four in colour A one pixel OUTSIDE the animated positions
   and spanning one pixel past the corners, four in colour B exactly on them.
   Colour pair by whether the ISSUING unit (node+0xE) is selected: selected ->
   gui[3] + gui[0xA], not -> gui[1] + gui[9]. Chains pos to the site.

   ROUTE DOTS `0x4394E0`. Delegates to `0x439740` first, then, only when
   flag == 1, walks the segment prev -> pos: `len` is the 3D distance in 16.16
   (rejected below 0x10000), the cursor starts at

     phase = ((age % 30) * 0x300000) / 30                (48.0 per 30 ticks)

   and advances by 0x300000 (48.0 world units) while cursor < len, each step
   blitting the next frame of the `pathicon` sequence (*(main+0x148D3)) at
   prev + delta*cursor/len. The first frame index is
   (age / max(1,(u16)seq[0x2C])) % (u16)seq[0].

   TARGET CIRCLE `0x4399F0`. Centre = the target unit's position (node+0x16)
   with radius (s16)def[0x178], else the node's ground target with radius 0x20.
   Sixteen chords, x radius R and y radius (int)(R * 0.89) [the double at
   0x4FD2C0], colour gui[0xC]. Chains pos to the centre.

   TARGET SPRITE `0x439740`. Resolves the position: the live target unit if
   `UnitInPlayerLOS 0x465AC0(owner->player, target)`, else the cached last-seen
   shorts node+0x32/0x34 when node+0x42 carries 0x200000 — and when it takes
   the LIVE branch it WRITES that cache back into the node. Then, if the order
   descriptor's cursor index (entry+0x10) is non-zero, alpha-blits frame
   (gameTime / (2 * (u16)seq[0x2C])) % (u16)seq[0] of cursor_ary[idx]
   (*(main+0x1487F + idx*4)). Chains pos to the resolved position.

   RANGE CIRCLES `0x4390A0`, normal play: the cloak radius (def[0x208]) around
   a cloaked unit (unit[0x10E] & 4) in gui[0xF], and for a kamikaze unit
   (def[0x241] bit 28) with an ExplodeAs weapon (def[0x220]) a circle whose
   radius pulses

     clamp(((gameTime % 60) * (aoe/2) * 2) / 60, 8, aoe/2)     aoe = w[0xD6]

   plus either def[0x218] (kamikazedistance) or def[0x202] (sight) — the engine
   picks between them on unit[0x0] being non-zero — both in gui[0xC]. With the
   `ShowRanges` console toggle (main+0x391BF) it instead draws a labelled set of
   NINE in gui[0xE] — and the FIRST is def[0x208], the cloak radius, gated only
   on the value being non-zero and NOT on the cloak flag, which is the normal
   branch's rule and not this one — then the three weapon ranges flashing
   gui[4]/gui[0xC] on gameTime&1.

   `0x439740` has a ShowRanges limb of its own, and it is circles too, not only
   labels: each live weapon's AoE and attackrunlength plus def[0x216], at the
   order target. Both limbs are ported; only their TEXT is not.

   Every one of those circles goes through `DrawRangeCircle 0x438EA0`, which
   FOLLOWS THE TERRAIN: one segment per 8 world units of circumference
   ((int)(r * 2pi * 0.125) of them), each endpoint's altitude raised to
   max(centreAltitude, GetPosHeight 0x485070 at that point).

   WHAT WE DO DIFFERENTLY, AND WHY IT IS ON PURPOSE.

   - Circles are real arcs at a segment count chosen for the zoom, not sixteen
     chords, and lines are one SCREEN pixel wide (glLineWidth(ss)) rather than
     one game pixel magnified.
   - Route dots and the waypoint sprite are drawn procedurally — a round dot
     and a pulsing crosshair — in the ink read out of the GAF frame the engine
     would have blitted. That is what keeps them crisp at 4x and legible at
     0.25x, and it is why the 64 KB identity-blend-LUT machinery in
     tagpu_markown.c has no order-marker work left to do.
   - Unit-anchored positions ride tagpu_native_unit_pos(), the sub-pixel
     interpolated sample: a crisp circle centred on a walking unit would
     otherwise step once per sim tick against a body that slides.
   - ShowRanges' text LABELS are drawn as of G13p, through tagpu_text.c: TA's
     own glyphs, rasterised by the engine's own blitter into an atlas of ours,
     at the point on the circle `0x438EA0` would have put them and at a constant
     SCREEN size. Every circle of both limbs is there too — including the cloak
     radius that opens the set and the sprite drawer's own AoE ring, each of
     which was missed on the first pass and caught by the landing review.
   - RANGE CIRCLES ARE ROUND. G13o drew them through the target circle's 0.89
     squash; `0x438EA0` has no squash at all (see `ocircle`).

   Read-only over the sim with ONE deliberate exception, on the game thread and
   at exactly the instant the engine did it: the target sprite's last-seen
   cache write. Leaving it out would be the opposite of harmless — the cache is
   what stops a waypoint marker tracking a unit the player can no longer see,
   so never writing it would leak the target's live position. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "tagpu_order.h"
#include "tagpu_opt.h"
#include "tagpu_mark.h"
#include "tagpu_text.h"
#include "tagpu_markown.h"
#include "tagpu_gaf.h"
#include "tagpu_native.h"

/* ---- engine layout ---- */
#define TA_MAINPP     0x00511DE8u
#define DESC_BEG_PP   0x00512344u    /* order-descriptor array, stride 0x19   */
#define DESC_END_PP   0x00512348u
#define DESC_STRIDE   0x19
#define DESC_MASK     0x0C           /* u32 marker-capability mask            */
#define DESC_CURSOR   0x10           /* u8  cursor_ary index (0 = no sprite)  */

#define LOS_VA        0x00465AC0u    /* UnitInPlayerLOS(player, unit), ret 8  */
#define POSHEIGHT_VA  0x00485070u    /* GetPosHeight(POS16_16*), ret 4        */

#define OFF_UNITS     0x14357        /* unit array base, stride 0x118         */
#define OFF_UNITEND   0x1435B        /* one past its last slot                */
#define OFF_UNITDEFS  0x1439B        /* UnitDef array base, stride 0x249      */
#define OFF_UDEFCOUNT 0x1438F        /* u32 UNITINFOCount — the array's length */
#define OFF_PLAYERS   0x1B63         /* player array base, stride 0x14B       */
#define OFF_WATCHED   0x2A42         /* u8 watched player id                  */
#define OFF_HOVERED   0x2CBA         /* u16 unit under the mouse cursor       */
#define OFF_TRACKED   0x37E9C        /* u16 tracked unit                      */
#define OFF_GAMETIME  0x38A47
#define OFF_SHOWRANGE 0x391BF
#define OFF_GUICOL    0x0DCB
#define OFF_PALETTE   0x143A7        /* the live palette, RGBx per index      */
#define OFF_PATHICON  0x148D3        /* GAF sequence* for the route dots      */
#define OFF_CURSORARY 0x1487F        /* GAF sequence*[0x15]                   */

#define UNIT_STRIDE   0x118
#define PLAYER_STRIDE 0x14B
#define UDEF_STRIDE   0x249
#define P_FIRSTUNIT   0x67
#define P_LASTUNIT    0x6B
#define U_POS         0x6A           /* 16.16 x, altitude, map z              */
#define U_ORDERS      0x5C
#define U_TYPE        0x92           /* UnitDef*                              */
#define U_PLAYER      0x96           /* PlayerStruct*                         */
#define U_INDEX       0xA8           /* u16 UnitInGameIndex                   */
#define U_CLOAKF      0x10E
#define U_STATE       0x110          /* bit28 alive, bit14 excluded, bit4 sel */
#define U_WEAP0       0x10           /* weapon object*, stride 0x1C, 3 of them*/
#define U_WEAPFLAGS   0x1F           /* bit1 = weapon slot present            */
#define UD_CANBUILD   0x156
#define UD_FOOT_X0    0x15E          /* the five footprint extents            */
#define UD_FOOT_Y0    0x162
#define UD_FOOT_Z0    0x166
#define UD_FOOT_X1    0x16A
#define UD_FOOT_Z1    0x172
#define UD_SIZE       0x178          /* s16 target-circle radius              */
#define UD_SIGHT      0x202
#define UD_RADAR      0x204
#define UD_SONAR      0x206
#define UD_CLOAKDIST  0x208
#define UD_RJAM       0x20A
#define UD_SJAM       0x20C
#define UD_BUILDDIST  0x212
#define UD_MANEUVER   0x214
#define UD_KAMIDIST   0x218
#define UD_ATTACKRUN  0x216          /* the def's own run length (ShowRanges) */
#define UD_EXPLODEAS  0x220          /* WeaponDef*                            */
#define UD_TYPEMASK0  0x241          /* bit28 = kamikaze                      */
#define W_AOE         0xD6           /* u16 area of effect                    */
#define W_RANGE       0xDC           /* the live weapon object's range        */
#define W_ATTACKRUN   0xE0           /* attackrunlength (ShowRanges only)     */
#define N_TYPE        0x04
#define N_OWNER       0x0E           /* issuing unit                          */
#define N_TARGET      0x16           /* target unit, 0 = ground target        */
#define N_TPOS        0x22           /* 16.16 x, altitude, map z              */
#define N_SEENX       0x32           /* s16 last-seen cache                   */
#define N_SEENZ       0x34
#define N_BTYPE       0x36           /* u16 build target unit type            */
#define N_FLAGS       0x42           /* bit 0x200000 = last-seen cached       */
#define N_ISSUE       0x46           /* game time the order was issued        */
#define N_NEXT        0x4A

#define GUI_BLACK     0x00
#define GUI_SITE1     0x01           /* build spot, issuer not selected       */
#define GUI_SITE3     0x03           /* build spot, issuer selected           */
#define GUI_FLASH     0x04
#define GUI_SITE9     0x09
#define GUI_SITEA     0x0A
#define GUI_RED       0x0C
#define GUI_YELLOW    0x0E
#define GUI_WHITE     0x0F

#define DOT_STEP      0x300000       /* 48.0 world units between route dots   */
/* Screen-constant marker weights, set against the art they replace: the
   engine's `pathicon` dot reads about five pixels across at 1x and its
   `cursor_ary` waypoint star about eleven, so a 2.4-px-radius disc and a
   crosshair whose arms breathe out to 9 px carry the same weight without ever
   being a magnified sprite. */
#define DOT_R         2.4f
#define CROSS_R0      4.0            /* arm length, first frame .. last       */
#define CROSS_R1      5.0
#define CROSS_G0      1.5            /* and the gap at the centre             */
#define CROSS_G1      2.0
#define GROW_TICKS    10
#define CIRCLE_SQUASH 0.89           /* the double at 0x4FD2C0                */

/* caps, mirroring MAXBAR: an overflow is counted and logged, never silently
   dropped and never allowed to walk off the end of an array */
#define MAXORD        2048           /* arena records per publication         */
#define MAXWALK       8192           /* order nodes visited per snapshot      */
#define MAXCHAIN      1024           /* nodes on one unit's list              */
#define MAXDOT        512            /* dots on one route segment             */
#define MAXSEG        128            /* segments in one native-res circle     */

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* ---- arming ---------------------------------------------------------- */

static int s_armed = -1;
static int s_log = 0, s_passive = 0, s_trace = 0;
static int s_build = 1, s_dots = 1, s_circle = 1, s_sprite = 1, s_ranges = 1;
static int s_labels = 1;
static unsigned s_armCheck = 0;

int tagpu_order_on(void) { return s_armed == 1; }

int tagpu_order_armed(unsigned frame_counter)
{
    int was;
    char buf[128];
    int n;
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return s_armed > 0;
    s_armCheck = frame_counter;
    was = s_armed;
    /* `s_armed` is NOT cleared for the length of the file read. It used to be,
       and that opened a window — tens of microseconds, every 30 frames — in
       which the game thread's snapshot declined (not armed) while `g_orders`
       was still set, so `mark_orders` let the ENGINE's driver run: one block of
       engine markers landing in its own frame, i.e. a ghost at the unzoomed
       position at any zoom but 1. (The capture window that used to be closed
       against it went with G13p; the ghost is the same either way.)
       Parse into locals and commit at the end instead. */
    n = tagpu_opt_read("tagpu_order.on", buf, sizeof buf);
    if (n < 0) {
        s_armed = 0;
        tagpu_markown_set_orders(0);
        if (was > 0) flog("order: disarmed");
        return 0;
    }
    {
        /* every token into a LOCAL, committed together below — the game thread
           reads s_passive/s_trace through tagpu_order_snapshot's return value,
           and a reset-then-parse would hand it "not passive" for the length of
           a file read */
        int log_ = 0, passive_ = 0, trace_ = 0;
        int build_ = 1, dots_ = 1, circle_ = 1, sprite_ = 1, ranges_ = 1;
        int labels_ = 1;
        if (n > 0) {
            char* p = buf;
            buf[n] = 0;
            while (*p) {
                char* q;
                int last;
                while (*p && *p <= ' ') p++;
                q = p;
                while (*q && *q > ' ') q++;
                last = (*q == 0);
                *q = 0;
                if (!lstrcmpiA(p, "log")) log_ = 1;
                else if (!lstrcmpiA(p, "passive")) passive_ = 1;
                else if (!lstrcmpiA(p, "trace")) { trace_ = 1; log_ = 1; }
                else if (!lstrcmpiA(p, "nobuild")) build_ = 0;
                else if (!lstrcmpiA(p, "nodots")) dots_ = 0;
                else if (!lstrcmpiA(p, "nocircle")) circle_ = 0;
                else if (!lstrcmpiA(p, "nosprite")) sprite_ = 0;
                else if (!lstrcmpiA(p, "noranges")) ranges_ = 0;
                else if (!lstrcmpiA(p, "nolabels")) labels_ = 0;
                if (last) break;
                p = q + 1;
            }
        }
        s_log = log_; s_passive = passive_; s_trace = trace_;
        s_build = build_; s_dots = dots_; s_circle = circle_;
        s_sprite = sprite_; s_ranges = ranges_; s_labels = labels_;
    }
    s_armed = 1;
    /* Arming only ever hands the draw BACK, exactly as tagpu_mark_armed does:
       taking it is done from the render, which is the only place that knows
       the pass is really running. `passive` and `trace` both leave the engine
       drawing its own — passive so the two can be compared on screen, trace so
       both node lists are produced in one run. */
    if (s_passive || s_trace) tagpu_markown_set_orders(0);
    if (was != 1) {
        char b[192];
        _snprintf(b, sizeof b, "order: ARMED (log=%d passive=%d trace=%d build=%d "
                  "dots=%d circle=%d sprite=%d ranges=%d labels=%d patched=%d)",
                  s_log, s_passive, s_trace, s_build, s_dots, s_circle,
                  s_sprite, s_ranges, s_labels, tagpu_markown_installed());
        flog(b);
    }
    return 1;
}

/* ---- the arena ------------------------------------------------------- */

/* One order node, flattened. Unit POINTERS are kept because the unit array is
   stable storage (a fixed array whose slots are recycled, never freed), which
   is exactly what the order NODES are not — so a pointer into it stays
   readable and the worst a dead unit can cost us is a marker drawn from stale
   fields for one frame. Every position is carried BOTH as the 16.16 the
   snapshot saw and, where it came from a unit, as that unit, so the present
   thread can ask tagpu_native_unit_pos() for the interpolated one. */
typedef struct ORDREC {
    const char* unit;        /* the unit whose order list this node is on     */
    const char* owner;       /* node+0x0E, the issuing unit                   */
    const char* target;      /* node+0x16, or NULL for a ground target        */
    const char* startU;      /* unit the chain-in position came from, or NULL */
    const char* endU;        /* unit the resolved sprite position came from   */
    const char* cirU;        /* unit the target-circle centre came from       */
    int sx, sy, sz;          /* chain-in position (16.16)                     */
    int bx, by, bz;          /* node+0x22.. — the build site / ground target  */
    int ex, ey, ez;          /* resolved sprite position (16.16)              */
    int cx, cy, cz;          /* target-circle centre (16.16)                  */
    const char* bdef;        /* UnitDef of the build target (bit 0), or NULL  */
    int issue;               /* node+0x46                                     */
    int cirR;                /* target-circle radius, world units             */
    unsigned mask;           /* descriptor mask AND the driver's              */
    unsigned short btype;    /* node+0x36                                     */
    unsigned char type;      /* node+0x04                                     */
    unsigned char cursor;    /* descriptor+0x10                               */
    unsigned char flag;      /* driver flag: 1 = hovered / tracked            */
    unsigned char sel;       /* the issuing unit was selected                 */
} ORDREC;

typedef struct ORDARENA {
    ORDREC rec[MAXORD];
    int    n;
    int    gameTime;         /* snapshotted, so the arena is self-contained   */
    int    showRanges;
    int    dropped;          /* records the cap refused                       */
} ORDARENA;

/* Two arenas and a published index, filled the same way the capture layer is:
   the writer fills the slot the index does NOT name and moves the index last,
   so a reader can never pair a fresh record count with the other arena's
   records. The publication is only ever REPLACED — a frame with nothing to
   show says so by publishing an EMPTY arena, not by blanking the live one. */
static ORDARENA     g_arena[2];
static volatile int g_pub = -1;
/* Bumped on every publication AND every clear, and never reused. A SLOT INDEX
   CANNOT BE THE READER'S GUARD: it takes two values, so two publications during
   one read return it to where it started and the reader accepts a copy the
   writer was overwriting — and the clear/publish pair does it in ONE step,
   because `arena_clear` leaves `g_pub` at -1 and the next snapshot then picks
   slot 0 again, which is the slot a reader that sampled 0 is copying. A
   monotonic counter has no such value to return to. */
static volatile unsigned g_gen;
static int          g_ran;            /* a snapshot ran in this engine block  */

static void arena_clear(void) { g_pub = -1; g_gen++; }

/* ---- game thread: the snapshot --------------------------------------- */

static const char* desc_entry(unsigned char type)
{
    const char* beg = *(const char* const*)DESC_BEG_PP;
    const char* end = *(const char* const*)DESC_END_PP;
    size_t n;
    if (!ptr_ok(beg) || !ptr_ok(end) || end <= beg) return NULL;
    n = (size_t)(end - beg) / DESC_STRIDE;
    /* The engine indexes this table with the raw type byte and no bound at
       all. We keep the bound: the array is heap storage with a live end
       pointer, so the check costs two loads and turns a corrupt order node
       into a skipped marker rather than a mask read out of somebody's heap. */
    if ((size_t)type >= n) return NULL;
    return beg + (size_t)type * DESC_STRIDE;
}

/* The target sprite's position resolution, INCLUDING the last-seen cache
   write — see the header comment for why that write is reproduced rather than
   dropped. Returns the target unit when the position is a live one (so the
   present thread may interpolate it) and NULL when it is a cached or ground
   position (which must not move). */
static const char* resolve_sprite(char* node, int out[3])
{
    char* tgt = *(char**)(node + N_TARGET);
    char* owner;
    char* player;
    int los;

    if (!ptr_ok(tgt)) {
        out[0] = *(int*)(node + N_TPOS + 0);
        out[1] = *(int*)(node + N_TPOS + 4);
        out[2] = *(int*)(node + N_TPOS + 8);
        return NULL;
    }
    owner = *(char**)(node + N_OWNER);
    player = ptr_ok(owner) ? *(char**)(owner + U_PLAYER) : NULL;
    los = ptr_ok(player)
        ? ((int (__stdcall *)(void*, void*))LOS_VA)(player, tgt)
        : 0;
    if (!los && (*(unsigned*)(node + N_FLAGS) & 0x200000u)) {
        out[0] = (int)*(short*)(node + N_SEENX) << 16;
        out[1] = *(int*)(tgt + U_POS + 4);        /* the LIVE altitude */
        out[2] = (int)*(short*)(node + N_SEENZ) << 16;
        return NULL;
    }
    out[0] = *(int*)(tgt + U_POS + 0);
    out[1] = *(int*)(tgt + U_POS + 4);
    out[2] = *(int*)(tgt + U_POS + 8);
    *(short*)(node + N_SEENX) = (short)(out[0] >> 16);
    *(short*)(node + N_SEENZ) = (short)(out[2] >> 16);
    *(unsigned*)(node + N_FLAGS) |= 0x200000u;
    return tgt;
}

/* The target circle's centre and radius (0x4399F0's prologue). */
static const char* circle_centre(const char* node, int out[3], int* radius)
{
    const char* tgt = *(const char* const*)(node + N_TARGET);
    if (ptr_ok(tgt)) {
        const char* def = *(const char* const*)(tgt + U_TYPE);
        out[0] = *(const int*)(tgt + U_POS + 0);
        out[1] = *(const int*)(tgt + U_POS + 4);
        out[2] = *(const int*)(tgt + U_POS + 8);
        *radius = ptr_ok(def) ? *(const short*)(def + UD_SIZE) : 0x20;
        return tgt;
    }
    out[0] = *(const int*)(node + N_TPOS + 0);
    out[1] = *(const int*)(node + N_TPOS + 4);
    out[2] = *(const int*)(node + N_TPOS + 8);
    *radius = 0x20;
    return NULL;
}

/* One unit's order list, exactly as 0x439B30 walks it. `budget` bounds the
   whole snapshot; a list that never terminates costs it and stops. */
static void walk_unit(ORDARENA* A, const char* ta, const char* unit,
                      unsigned callerMask, int flag, int* budget)
{
    int pos[3];
    char* node;
    int guard = 0, chain = 0;

    pos[0] = *(const int*)(unit + U_POS + 0);
    pos[1] = *(const int*)(unit + U_POS + 4);
    pos[2] = *(const int*)(unit + U_POS + 8);
    /* the first segment starts at the UNIT, so the present thread may ride
       its interpolated position; every later one starts at a fixed target */
    {
        const char* startU = unit;
        node = *(char* const*)(unit + U_ORDERS);
        while (ptr_ok(node) && *budget > 0 && chain < MAXCHAIN) {
            const char* d;
            unsigned mask;
            unsigned char type;
            int entry[3];
            ORDREC* r;

            chain++;
            (*budget)--;
            /* the caps are drops too: a queue longer than MAXCHAIN, or a
               snapshot that runs out of MAXWALK, loses markers exactly as the
               MAXORD cap does, and a counter that only saw one of the three
               reported a clean frame while markers went missing */
            if (chain >= MAXCHAIN || *budget <= 0) A->dropped++;
            type = *(unsigned char*)(node + N_TYPE);
            d = desc_entry(type);
            mask = d ? (*(const unsigned*)(d + DESC_MASK) & callerMask) : 0u;
            entry[0] = pos[0]; entry[1] = pos[1]; entry[2] = pos[2];

            if (mask == 0) { node = *(char* const*)(node + N_NEXT); continue; }
            if (A->n >= MAXORD) { A->dropped++; break; }

            r = &A->rec[A->n];
            memset(r, 0, sizeof *r);
            r->unit   = unit;
            r->owner  = *(const char* const*)(node + N_OWNER);
            r->target = *(const char* const*)(node + N_TARGET);
            if (!ptr_ok(r->owner))  r->owner  = NULL;
            if (!ptr_ok(r->target)) r->target = NULL;
            r->startU = startU;
            r->sx = entry[0]; r->sy = entry[1]; r->sz = entry[2];
            r->bx = *(const int*)(node + N_TPOS + 0);
            r->by = *(const int*)(node + N_TPOS + 4);
            r->bz = *(const int*)(node + N_TPOS + 8);
            r->issue  = *(const int*)(node + N_ISSUE);
            r->mask   = mask;
            r->btype  = *(const unsigned short*)(node + N_BTYPE);
            r->type   = type;
            r->cursor = (unsigned char)(d ? *(const unsigned char*)(d + DESC_CURSOR) : 0);
            r->flag   = (unsigned char)(flag != 0);
            r->sel    = (unsigned char)(r->owner &&
                            (*(const unsigned*)(r->owner + U_STATE) & 0x10u) ? 1 : 0);

            /* bit 0 — the build site. Nothing at all without a build target
               type, chains pos to the node's own target when there is one.
               The DEF POINTER is resolved here rather than on the present
               thread, and BOUNDED AGAINST THE ARRAY'S OWN LENGTH: `btype` is a
               u16 the engine indexes with no check at all, so `btype*0x249` off
               a base reaches ~38 MB past it, and `ptr_ok` alone would let a
               corrupt or torn value through to a dereference on the present
               thread. `main+0x1438F` is the count (`UNITINFOCount`, the same
               field tagpu_cat.c walks the array with), so the bound is two
               loads — the price desc_entry already pays for the same reason. */
            if (mask & 0x01) {
                if (r->btype) {
                    const char* base = *(const char* const*)(ta + OFF_UNITDEFS);
                    unsigned ndef = *(const unsigned*)(ta + OFF_UDEFCOUNT);
                    if (ptr_ok(base) && ndef <= 16384u && (unsigned)r->btype < ndef) {
                        const char* d2 = base + (size_t)r->btype * UDEF_STRIDE;
                        if (ptr_ok(d2)) r->bdef = d2;
                    }
                    pos[0] = r->bx; pos[1] = r->by; pos[2] = r->bz;
                    startU = NULL;
                }
            }
            /* bits 1 and 3 — the sprite, and the route line that delegates to
               it. Both resolve the same position; resolving it twice is what
               the engine does and the second call finds the cache already
               written. */
            if (mask & 0x0A) {
                int p[3];
                r->endU = resolve_sprite(node, p);
                r->ex = p[0]; r->ey = p[1]; r->ez = p[2];
                pos[0] = p[0]; pos[1] = p[1]; pos[2] = p[2];
                startU = r->endU;
            }
            /* bit 2 — the target circle. It chains too, and it runs BEFORE
               bit 3, so a node carrying both ends up chained to the sprite. */
            if (mask & 0x04) {
                int c[3];
                r->cirU = circle_centre(node, c, &r->cirR);
                r->cx = c[0]; r->cy = c[1]; r->cz = c[2];
                if (!(mask & 0x08)) {
                    pos[0] = c[0]; pos[1] = c[1]; pos[2] = c[2];
                    startU = r->cirU;
                }
            }
            /* bit 4 — range circles, once per unit behind the engine's guard */
            if ((mask & 0x10) && !guard) guard = 1;
            else r->mask &= ~0x10u;

            A->n++;
            if (s_trace) {
                char b[224];
                _snprintf(b, sizeof b,
                    "order TRACE own: u=%p node=%p type=%u mask=%x flag=%d "
                    "in=(%d,%d,%d) out=(%d,%d,%d) tgt=%p btype=%u issue=%d",
                    (const void*)unit, (const void*)node, type, r->mask, flag,
                    entry[0] >> 16, entry[1] >> 16, entry[2] >> 16,
                    pos[0] >> 16, pos[1] >> 16, pos[2] >> 16,
                    (const void*)r->target, r->btype, r->issue);
                flog(b);
            }
            node = *(char* const*)(node + N_NEXT);
        }
    }
}

int tagpu_order_snapshot(void* ctx, void* view)
{
    const char* ta;
    const char *units, *player, *first, *last, *u;
    const char *camU, *trackU, *hoverU;
    unsigned short tracked, hovered;
    int builder, watched, budget = MAXWALK, slot;
    ORDARENA* A;

    (void)ctx;
    g_ran = 1;
    if (s_armed != 1) return 0;
    ta = *(const char* const*)TA_MAINPP;
    if (!ptr_ok(ta) || !ptr_ok(view)) return 0;

    units = *(const char* const*)(ta + OFF_UNITS);
    if (!ptr_ok(units)) return 0;
    watched = *(const unsigned char*)(ta + OFF_WATCHED);
    player  = ta + OFF_PLAYERS + (size_t)watched * PLAYER_STRIDE;
    first   = *(const char* const*)(player + P_FIRSTUNIT);
    last    = *(const char* const*)(player + P_LASTUNIT);

    tracked = *(const unsigned short*)(ta + OFF_TRACKED);
    hovered = *(const unsigned short*)(ta + OFF_HOVERED);
    camU   = *(const char* const*)view;
    trackU = tracked ? units + (size_t)tracked * UNIT_STRIDE : NULL;
    hoverU = hovered ? units + (size_t)hovered * UNIT_STRIDE : NULL;

    /* "is one of the camera / tracked / hovered units a builder" — the one
       test that lets an UNSELECTED unit contribute its build spots */
    builder = 0;
    {
        const char* cand[3];
        int i;
        cand[0] = ptr_ok(camU) ? camU : NULL;
        cand[1] = trackU; cand[2] = hoverU;
        for (i = 0; i < 3 && !builder; i++) {
            const char* def;
            if (!cand[i]) continue;
            def = *(const char* const*)(cand[i] + U_TYPE);
            if (ptr_ok(def) && *(const void* const*)(def + UD_CANBUILD)) builder = 1;
        }
    }

    slot = (g_pub == 0) ? 1 : 0;
    A = &g_arena[slot];
    A->n = 0;
    A->dropped = 0;
    A->gameTime  = *(const int*)(ta + OFF_GAMETIME);
    A->showRanges = *(const int*)(ta + OFF_SHOWRANGE) != 0;

    if (ptr_ok(first) && ptr_ok(last) && first <= last &&
        (size_t)(last - first) <= (size_t)UNIT_STRIDE * 20000) {
        for (u = first; u <= last && budget > 0; u += UNIT_STRIDE) {
            unsigned st = *(const unsigned*)(u + U_STATE);
            unsigned mask;
            int flag;
            if (!(st & 0x10000000u) || (st & 0x4000u)) continue;
            if (u == camU ||
                *(const unsigned short*)(u + U_INDEX) == tracked ||
                *(const unsigned short*)(u + U_INDEX) == hovered) {
                mask = 0x1F; flag = 1;
            } else if (st & 0x10u) {
                mask = 0x1F; flag = 0;
            } else if (builder) {
                mask = 0x01; flag = 1;
            } else continue;
            walk_unit(A, ta, u, mask, flag, &budget);
        }
    }

    g_pub = slot;                 /* moved last: the arena is whole */
    g_gen++;                      /* ...and the generation last of all */
    /* passive and trace both leave the draw with the engine; everything else
       has a complete publication and may skip it */
    return !s_passive && !s_trace;
}

void tagpu_order_block_begin(void) { g_ran = 0; }

void tagpu_order_block_end(void)
{
    /* A block that reached hook 9 without the SHIFT gate opening has no
       markers, and saying so is what stops the last publication standing on
       screen for the rest of the session. Publishing "empty" rather than
       blanking the live arena is the same rule the capture layer follows. */
    if (!g_ran && g_pub >= 0) arena_clear();
}

void tagpu_order_trace_drawer(int bit, const void* node, const int* pos, int flag)
{
    char b[192];
    if (!s_trace) return;
    _snprintf(b, sizeof b,
              "order TRACE eng: bit=%d node=%p flag=%d in=(%d,%d,%d)",
              bit, node, flag,
              pos ? pos[0] >> 16 : 0, pos ? pos[1] >> 16 : 0,
              pos ? pos[2] >> 16 : 0);
    flog(b);
}

/* ---- present thread: geometry ---------------------------------------- */

/* the frame's derived constants, set once per gather */
static const TAGPU_FXVIEW* s_v;
static ORDREC s_rec[MAXORD];     /* the present thread's private copy       */
static const unsigned char* s_gui;
static double s_px;              /* one SCREEN pixel, in game-frame units    */
static int    s_nrec, s_nline, s_ndot, s_nover, s_nlabel;

/* the engine's projection, with the +0x80/+0x20 baked immediates the rest of
   this pass uses (tagpu_mark.c, the health bar). The engine halves the
   altitude with an arithmetic shift, i.e. rounded to a whole pixel; this
   keeps the fraction, because the whole point of the port is that the marker
   no longer has to land on a 1x pixel grid. */
static void project(double wx, double walt, double wz, float* sx, float* sy)
{
    *sx = (float)(wx - (double)s_v->eyeX + 128.0);
    *sy = (float)(wz - walt * 0.5 - (double)s_v->eyeY + 32.0);
}

/* A record's unit pointer, but only if it still names a SLOT of the live unit
   array. The game thread writes the arena while this thread walks it, and the
   two-arena discipline makes a torn record rare rather than impossible — a
   torn coordinate costs one wrong line, but a torn POINTER handed to a
   dereference costs the process, so it is bounded instead of trusted. */
static const char* sane_unit(const char* u)
{
    const char *beg, *end;
    if (!u || !s_v) return NULL;
    beg = *(const char* const*)(s_v->ta + OFF_UNITS);
    end = *(const char* const*)(s_v->ta + OFF_UNITEND);
    if (!ptr_ok(beg) || !ptr_ok(end) || u < beg || u >= end) return NULL;
    if ((size_t)(u - beg) % UNIT_STRIDE) return NULL;
    return u;
}

/* A position that may ride a unit: the interpolated sample when the record
   named a unit and this frame's unit gather produced one, the snapshotted
   16.16 otherwise. */
static void rec_pos(const char* unit, int fx, int fy, int fz,
                    double* x, double* y, double* z)
{
    float ux, uy, uz;
    unit = sane_unit(unit);
    if (unit && tagpu_native_unit_pos(unit, &ux, &uy, &uz)) {
        *x = ux; *y = uy; *z = uz;
        return;
    }
    *x = (double)fx / 65536.0;
    *y = (double)fy / 65536.0;
    *z = (double)fz / 65536.0;
}

static int on_screen(float x, float y, float slack)
{
    return x >= (float)s_v->evpL - slack && x <= (float)(s_v->evpL + s_v->evw) + slack &&
           y >= (float)s_v->evpT - slack && y <= (float)(s_v->evpT + s_v->evh) + slack;
}

/* Bounding-box overlap, not "either end is on screen": a build rect wider than
   the viewport has both corners outside it and two edges crossing it. */
static int box_on_screen(float x0, float y0, float x1, float y1, float slack)
{
    float lo, hi;
    lo = x0 < x1 ? x0 : x1; hi = x0 < x1 ? x1 : x0;
    if (hi < (float)s_v->evpL - slack || lo > (float)(s_v->evpL + s_v->evw) + slack) return 0;
    lo = y0 < y1 ? y0 : y1; hi = y0 < y1 ? y1 : y0;
    if (hi < (float)s_v->evpT - slack || lo > (float)(s_v->evpT + s_v->evh) + slack) return 0;
    return 1;
}

/* one marker line, fogged at its own first endpoint (the same pure
   translation between screen and world layer_quad uses) */
static void oline(float x0, float y0, float x1, float y1, int col)
{
    if (!box_on_screen(x0, y0, x1, y1, 8.0f)) return;
    if (!tagpu_mark_emit_line(x0, y0, x1, y1, col,
                              x0 - (float)s_v->vpL + (float)s_v->eyeX,
                              y0 - (float)s_v->vpT + (float)s_v->eyeY))
        s_nover++;
    else
        s_nline++;
}

/* A filled disc of a constant SCREEN radius — the route dot. Constant in
   SCREEN units, not world: the thing it replaces is a fixed-size sprite, so a
   dot that grew with the zoom would be a magnified 1997 pixel by another
   route. The radius is set against the engine's own `pathicon` frame, which
   reads as about five pixels across at 1x (measured off the A/B capture). */
static void odot(float cx, float cy, float rScreen, int col)
{
    const int N = 10;
    float r = (float)(rScreen * s_px);
    float wx, wz, px, py;
    int i;
    if (!on_screen(cx, cy, 8.0f)) return;
    wx = cx - (float)s_v->vpL + (float)s_v->eyeX;
    wz = cy - (float)s_v->vpT + (float)s_v->eyeY;
    px = cx + r; py = cy;
    for (i = 1; i <= N; i++) {
        double a = 6.283185307179586 * (double)i / (double)N;
        float qx = cx + r * (float)cos(a), qy = cy + r * (float)sin(a);
        if (!tagpu_mark_emit_tri(cx, cy, px, py, qx, qy, col, wx, wz)) { s_nover++; return; }
        px = qx; py = qy;
    }
    s_ndot++;
}

/* a closed ellipse as a native-res polyline: one segment per ~4 screen pixels,
   never fewer than 12 and never more than MAXSEG. `follow` raises each
   vertex's altitude to the terrain under it, which is what makes the engine's
   range circles hug the ground (DrawRangeCircle 0x438EA0). */
static void oellipse(double wx, double walt, double wz, double rx, double ry,
                     int col, int follow)
{
    float px = 0.0f, py = 0.0f;
    int i, n;
    double rmax = rx > ry ? rx : ry;
    n = (int)(6.283185307179586 * rmax * (double)(s_v->zoom > 0.0f ? s_v->zoom : 1.0f) / 4.0);
    if (n < 12) n = 12;
    if (n > MAXSEG) n = MAXSEG;
    for (i = 0; i <= n; i++) {
        double a = 6.283185307179586 * (double)i / (double)n;
        double vx = wx + rx * cos(a), vz = wz + ry * sin(a);
        double va = walt;
        float qx, qy;
        if (follow) {
            int p[3];
            int h;
            p[0] = (int)(vx * 65536.0); p[1] = 0; p[2] = (int)(vz * 65536.0);
            h = ((int (__stdcall *)(const int*))POSHEIGHT_VA)(p);
            if ((double)h > va) va = (double)h;
        }
        project(vx, va, vz, &qx, &qy);
        if (i) oline(px, py, qx, qy, col);
        px = qx; py = qy;
    }
}

/* The LABEL `DrawRangeCircle` puts beside its circle, at the engine's own point
   on it.

   `0x438EA0` walks i = 0..N with N = (int)(radius x 2pi x 0.125) and an angle
   step of 0x10000/N, and remembers the SECOND endpoint of the segment whose
   index equals `labelSlot * 3` (`lea eax,[eax+eax*2]` at `0x438EFF`, compared
   against the loop counter at `0x43902E`) — so the anchor is the point at angle
   (slot*3 + 1) * step, terrain-raised like every other vertex, and the string
   is drawn at y + 4 (`0x43907D`). When slot*3 exceeds N no segment matches and
   the engine falls back to the LAST endpoint it computed, which is i = N's.

   Its parameterisation is the mirror of ours — `p.x = centre.x +
   TurnXLookup(a)` and `p.z = centre.z + TurnZLookup(a)`, and the shared sine
   table at `0x509F00` makes TurnX a sine and TurnZ a cosine (`0x4B7123` adds a
   quarter turn to the index) — so the point is taken with sin on x and cos on
   z. On a round circle that is the same circle, entered at a different place,
   and this puts the label where the engine put it rather than a quarter turn
   away. [BINARY-VERIFIED]

   The +4 is in SCREEN pixels here, because the text is: tagpu_text.c draws at a
   constant screen size, so an offset that scaled with the zoom would part the
   label from its circle at 4x and bury it at 0.25x. */
static void range_label(double wx, double walt, double wz, double rad,
                        const char* label, int slot)
{
    double a, vx, vz, va;
    int n, step, k, p[3], h, col;
    float sx, sy;

    if (!s_labels || !label || !*label || rad <= 0.0) return;
    n = (int)(rad * 6.283185307179586 * 0.125);
    /* The engine divides 0x10000 by this with an `idiv` and no zero test
       (`0x438EEE`, after the `mov eax,0x10000` at `0x438EE4`), so a radius under
       about 1.3 world units faults inside TA
       itself. Nothing in stock content is that small; we simply have no label
       to place. */
    if (n <= 0) return;
    step = 65536 / n;
    k = slot * 3;
    if (k > n) k = n;
    a = (double)((k + 1) * step) * 6.283185307179586 / 65536.0;
    vx = wx + rad * sin(a);
    vz = wz + rad * cos(a);
    va = walt;
    p[0] = (int)(vx * 65536.0); p[1] = 0; p[2] = (int)(vz * 65536.0);
    h = ((int (__stdcall *)(const int*))POSHEIGHT_VA)(p);
    if ((double)h > va) va = (double)h;
    project(vx, va, vz, &sx, &sy);
    /* A whole label hangs off its anchor to the right and below, so the slack is
       a label's width rather than a marker's — and it is in GAME-FRAME units
       while the label is a constant SCREEN size, so it has to scale by `s_px`
       like the quad does. A flat 64 is a quarter of a label at 0.25x, and the
       symptom is labels popping in and out along the left and top edges of the
       zoomed-out ring. */
    if (!on_screen(sx, sy, (float)(64.0 * s_px))) return;
    col = tagpu_text_colour();
    if (col < 0 || col > 255) col = s_gui[GUI_WHITE];
    if (!tagpu_mark_emit_text(sx, sy + (float)(4.0 * s_px), label, col,
                              sx - (float)s_v->vpL + (float)s_v->eyeX,
                              sy - (float)s_v->vpT + (float)s_v->eyeY))
        s_nover++;
    else
        s_nlabel++;
}

/* one DrawRangeCircle: a terrain-following circle of world radius `rad`, and
   its label if it has one.

   ROUND, NOT SQUASHED. `0x438EA0` hands the SAME `radius<<16` to TurnXLookup
   and TurnZLookup (`ebx` is reloaded from `[esp+0x18]` each iteration and used
   for both), and the projection maps world z to screen y 1:1, so the circle is
   a circle on screen. The 0.89 at `0x4FD2C0` belongs to the TARGET circle
   `0x4399F0`, which multiplies only its y radius by it (`[esp+0x18]` there) —
   a different drawer and a deliberate difference. G13o applied the squash to
   both and drew every range circle 11% flat. */
static void ocircle(double wx, double walt, double wz, double rad, int col,
                    const char* label, int slot)
{
    if (rad <= 0.0) return;
    oellipse(wx, walt, wz, rad, rad, col, 1);
    range_label(wx, walt, wz, rad, label, slot);
}

static void range_circle(const char* unit, int fx, int fy, int fz, double rad,
                         int col, const char* label, int slot)
{
    double wx, wy, wz;
    if (rad <= 0.0) return;
    rec_pos(unit, fx, fy, fz, &wx, &wy, &wz);
    ocircle(wx, wy, wz, rad, col, label, slot);
}

/* The ink a procedurally drawn marker inherits from the art it replaces.

   NOT the most common index, which is what this did first and it drew every
   route dot in (11,11,0) — near-black, invisible against grass. A `pathicon`
   dot and a `cursor_ary` crosshair are both a small bright core inside a dark
   outline, and the outline is the bigger half by area, so "most common" picks
   exactly the colour the sprite uses to HIDE its edge. The core is what the
   eye reads as the marker, so: among the indices that make up a real share of
   the frame, take the BRIGHTEST, through the live palette rather than a
   guess at what the index means. Cached on the sequence pointer — it is a
   decode, and this is asked per dot. */
typedef struct { const void* seq; int ink; } INKENT;
static INKENT s_ink[16];
static int    s_nink;

static int seq_ink(const char* seq, int fallback)
{
    static unsigned char pix[128 * 128];
    const unsigned char* g;
    const unsigned char* pal;
    int i, w, h, ck, n, floor_, best = -1, bestlum = -1;
    int hist[256];

    if (!seq || !s_v) return fallback;
    for (i = 0; i < s_nink; i++) if (s_ink[i].seq == (const void*)seq) return s_ink[i].ink;
    g = tagpu_gaf_seq_frame(seq, 0);
    if (!g) return fallback;
    w = *(const unsigned short*)(g + TAGPU_GF_W);
    h = *(const unsigned short*)(g + TAGPU_GF_H);
    ck = *(const unsigned char*)(g + TAGPU_GF_CK);
    /* THE ENGINE'S OWN TABLE, deliberately, and the one place in the world's
       code that still reads it. Two reasons, both required: this walk runs on
       the GAME THREAD (tagpu_order.h, "the two-thread split") and tagpu_pal.c
       resolves on the render thread's cadence, so calling it here would race
       the snapshot every other pass reads; and the question is a luminance
       RANKING over one sprite's own colours, which a uniform scale of every
       entry cannot change — the ink index this picks is a property of the art,
       not of the display. */
    pal = (const unsigned char*)(s_v->ta + OFF_PALETTE);
    if (w > 0 && h > 0 && w <= 128 && h <= 128 &&
        tagpu_gaf_decode(g, w, h, pix)) {
        memset(hist, 0, sizeof hist);
        n = 0;
        for (i = 0; i < w * h; i++) { hist[pix[i]]++; if (pix[i] != ck) n++; }
        /* a twentieth of the sprite's opaque area keeps a stray anti-aliasing
           pixel from deciding the colour of every dot on the map */
        floor_ = n / 20;
        if (floor_ < 1) floor_ = 1;
        for (i = 0; i < 256; i++) {
            int lum;
            if (i == ck || hist[i] < floor_) continue;
            lum = 2 * pal[i * 4 + 0] + 5 * pal[i * 4 + 1] + pal[i * 4 + 2];
            if (lum > bestlum) { bestlum = lum; best = i; }
        }
    }
    if (best < 0) best = fallback;
    if (s_nink < 16) { s_ink[s_nink].seq = seq; s_ink[s_nink].ink = best; s_nink++; }
    return best;
}

/* --- bit 0: the queued build site --- */
static void draw_build(const ORDREC* r, int gameTime)
{
    const char* def = r->bdef;
    double t;
    double p0x, p0y, p0z, p1x, p1z;
    float x0, z0, x1, z1;
    float xg0, xg1, zg0, zg1, e;
    int colA, colB, age;

    if (!s_build || !r->btype || !ptr_ok(def)) return;

    p0x = ((double)(short)((*(const int*)(def + UD_FOOT_X0) + r->bx) >> 16));
    p0y = ((double)(short)((*(const int*)(def + UD_FOOT_Y0) + r->by) >> 16));
    p0z = ((double)(short)((*(const int*)(def + UD_FOOT_Z0) + r->bz) >> 16));
    p1x = ((double)(short)((*(const int*)(def + UD_FOOT_X1) + r->bx) >> 16));
    p1z = ((double)(short)((*(const int*)(def + UD_FOOT_Z1) + r->bz) >> 16));

    project(p0x, p0y, p0z, &x0, &z0);
    project(p1x, p0y, p1z, &x1, &z1);

    /* the ten-tick sweep, with the engine's UNSIGNED clamp: an age that came
       out negative reads as "finished", not as "not started" */
    age = gameTime - r->issue;
    t = ((unsigned)age >= (unsigned)GROW_TICKS) ? (double)GROW_TICKS : (double)age;
    xg0 = x0 + (float)((x1 - x0) * t / GROW_TICKS);
    xg1 = x1 - (float)((x1 - x0) * t / GROW_TICKS);
    zg0 = z0 + (float)((z1 - z0) * t / GROW_TICKS);
    zg1 = z1 - (float)((z1 - z0) * t / GROW_TICKS);

    colA = s_gui[r->sel ? GUI_SITE3 : GUI_SITE1];
    colB = s_gui[r->sel ? GUI_SITEA : GUI_SITE9];

    /* One SCREEN pixel, not one game pixel: the engine's ±1 offsets are a
       rasteriser detail, and at 0.25x a game pixel is a quarter of one, which
       would collapse the double outline into a single line. */
    e = (float)s_px;
    oline(xg0 - e, z0 - e, xg0 - e, z1 + e, colA);
    oline(xg1 + e, z0 - e, xg1 + e, z1 + e, colA);
    oline(x0 - e, zg0 - e, x1 + e, zg0 - e, colA);
    oline(x0 - e, zg1 + e, x1 + e, zg1 + e, colA);
    oline(xg0, z0, xg0, z1, colB);
    oline(xg1, z0, xg1, z1, colB);
    oline(x0, zg0, x1, zg0, colB);
    oline(x0, zg1, x1, zg1, colB);
}

/* --- bit 3 (and bit 1's delegation): the waypoint crosshair ---
   TA blits frame (gameTime / (2*period)) % nframes of cursor_ary[idx]; we draw
   a crosshair whose arms breathe on that same frame index, in that sequence's
   own ink. Opaque, deliberately: TA's "alpha" is a palette-pair lookup rather
   than alpha, what this pass already ships is opaque, and a waypoint is
   information — translucency costs legibility. */
static void draw_sprite(const ORDREC* r, int gameTime, int showRanges)
{
    const char* ta = s_v->ta;
    const char* seq;
    double wx, wy, wz;
    float cx, cy;
    int nfr, period, frame, ink;
    double a, outer, inner;

    /* the descriptor's cursor index gates the whole drawer, ShowRanges limb
       included — `0x4397F9` returns before `0x439811` ever reads the toggle */
    if (!s_sprite || !r->cursor || r->cursor >= 0x15) return;

    rec_pos(r->endU, r->ex, r->ey, r->ez, &wx, &wy, &wz);
    project(wx, wy, wz, &cx, &cy);

    /* `0x439740` has a ShowRanges limb of its own (`0x439811..0x439948`), and it
       is circles, not labels: with the toggle on and the descriptor's cursor
       index 1 or 2 it draws each live weapon's AoE (`w+0xD6`) and
       `attackrunlength` (`w+0xE0`) plus `def+0x216`, AT THE ORDER TARGET, in the
       same gameTime&1 flash colour the range drawer uses. Note the weapon-slot
       flags here are the regular `0x1F + i*0x1C` — this drawer does NOT carry
       `0x4390A0`'s third-slot quirk. */
    if (showRanges && (r->cursor == 1 || r->cursor == 2)) {
        const char* u = sane_unit(r->owner);
        int flash = s_gui[(gameTime & 1) ? GUI_FLASH : GUI_RED];
        if (u) {
            const char* def = *(const char* const*)(u + U_TYPE);
            char lab[40];
            int i, v;
            for (i = 0; i < 3; i++) {
                const char* w;
                if (!(*(const unsigned char*)(u + U_WEAPFLAGS + i * 0x1C) & 2)) continue;
                w = *(const char* const*)(u + U_WEAP0 + i * 0x1C);
                if (!ptr_ok(w)) continue;
                v = *(const unsigned short*)(w + W_AOE);
                if (v) {
                    /* `0x5051C4` through the engine's own sprintf at `0x43989B`,
                       with the weapon INDEX (0..2), and label slot 0 */
                    _snprintf(lab, sizeof lab, "weapon %d - area of effect", i);
                    ocircle(wx, wy, wz, (double)v, flash, lab, 0);
                }
                v = *(const int*)(w + W_ATTACKRUN);
                if (v) {
                    _snprintf(lab, sizeof lab, "weapon %d - coverage", i);
                    ocircle(wx, wy, wz, (double)v, flash, lab, 1);
                }
            }
            if (ptr_ok(def)) {
                v = *(const unsigned short*)(def + UD_ATTACKRUN);
                if (v) ocircle(wx, wy, wz, (double)v, flash, "attack length", 2);
            }
        }
    }

    /* the sprite itself, and only now: the engine reaches its own sequence
       lookup at `0x439952`, after the limb above */
    seq = *(const char* const*)(ta + OFF_CURSORARY + (size_t)r->cursor * 4);
    if (!ptr_ok(seq)) return;
    nfr = *(const unsigned short*)seq;
    period = *(const unsigned short*)(seq + 0x2C);
    if (nfr <= 0) return;
    if (period <= 0) period = 1;
    frame = (int)(((unsigned)gameTime / (unsigned)(2 * period)) % (unsigned)nfr);
    if (!on_screen(cx, cy, 32.0f)) return;

    ink = seq_ink(seq, s_gui[GUI_WHITE]);
    /* The crosshair is a SCREEN icon, like the cursor sprite it replaces, so
       it carries no isometric squash and no zoom scale — only the arms
       breathe, on the sequence's own frame index. */
    a = nfr > 1 ? (double)frame / (double)(nfr - 1) : 0.0;
    outer = (CROSS_R0 + CROSS_R1 * a) * s_px;
    inner = (CROSS_G0 + CROSS_G1 * a) * s_px;
    oline(cx - (float)outer, cy, cx - (float)inner, cy, ink);
    oline(cx + (float)inner, cy, cx + (float)outer, cy, ink);
    oline(cx, cy - (float)outer, cx, cy - (float)inner, ink);
    oline(cx, cy + (float)inner, cx, cy + (float)outer, ink);
}

/* --- bit 1: the marching route dots --- */
static void draw_dots(const ORDREC* r, int gameTime)
{
    const char* ta = s_v->ta;
    const char* seq;
    double ax, ay, az, bx, by, bz;
    double dx, dy, dz, len, cursor, phase;
    int ink, age, n = 0;

    if (!s_dots || !r->flag) return;
    /* only the frames' INK is wanted: what the engine steps through the
       sequence for is the dot's animation, and a procedurally drawn round dot
       is the same dot in every frame */
    seq = *(const char* const*)(ta + OFF_PATHICON);
    ink = ptr_ok(seq) ? seq_ink(seq, s_gui[GUI_WHITE]) : s_gui[GUI_WHITE];

    rec_pos(r->startU, r->sx, r->sy, r->sz, &ax, &ay, &az);
    rec_pos(r->endU,   r->ex, r->ey, r->ez, &bx, &by, &bz);
    dx = bx - ax; dy = by - ay; dz = bz - az;
    len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1.0) return;                       /* the engine's 0x10000 floor */

    age = gameTime - r->issue;
    /* 48 world units of phase per 30 ticks, the engine's own signed division */
    phase = (double)(age % 30) * 48.0 / 30.0;

    for (cursor = phase; cursor < len && n < MAXDOT; cursor += 48.0, n++) {
        double f = cursor / len;
        float sx, sy;
        project(ax + dx * f, ay + dy * f, az + dz * f, &sx, &sy);
        odot(sx, sy, DOT_R, ink);
    }
}

/* --- bit 2: the circle around the order target --- */
static void draw_circle(const ORDREC* r)
{
    double wx, wy, wz, rr;
    if (!s_circle) return;
    rr = (double)r->cirR;
    if (rr <= 0.0) return;
    rec_pos(r->cirU, r->cx, r->cy, r->cz, &wx, &wy, &wz);
    oellipse(wx, wy, wz, rr, rr * CIRCLE_SQUASH, s_gui[GUI_RED], 0);
}

/* --- bit 4: the per-unit range circles --- */
static void draw_ranges(const ORDREC* r, int gameTime, int showRanges)
{
    const char* u = sane_unit(r->owner);
    const char* def;
    int ux, uy, uz, nslot = 0;

    if (!s_ranges || !u) return;
    def = *(const char* const*)(u + U_TYPE);
    if (!ptr_ok(def)) return;
    ux = *(const int*)(u + U_POS + 0);
    uy = *(const int*)(u + U_POS + 4);
    uz = *(const int*)(u + U_POS + 8);

    if (!showRanges) {
        const char* weap;
        int aoe, t, rr;
        if (*(const unsigned short*)(def + UD_CLOAKDIST) &&
            (*(const unsigned char*)(u + U_CLOAKF) & 4))
            range_circle(u, ux, uy, uz,
                         (double)*(const short*)(def + UD_CLOAKDIST),
                         s_gui[GUI_WHITE], NULL, 0);
        if (!(*(const unsigned*)(def + UD_TYPEMASK0) & 0x10000000u)) return;
        weap = *(const char* const*)(def + UD_EXPLODEAS);
        if (!ptr_ok(weap)) return;
        aoe = *(const unsigned short*)(weap + W_AOE) >> 1;
        /* radius = clamp(((gameTime % 60) * aoe * 2) / 60, 8, aoe/2), all of it
           the engine's UNSIGNED arithmetic */
        t  = (int)((unsigned)gameTime % 60u);
        rr = (int)(((unsigned)t * (unsigned)aoe * 2u) / 60u);
        if (rr < 8) rr = 8;
        if (rr > aoe) rr = aoe;
        range_circle(u, ux, uy, uz, (double)rr, s_gui[GUI_RED], NULL, 0);
        /* and the engine picks between kamikazedistance and sight on unit+0x0 */
        if (*(const unsigned*)u)
            range_circle(u, ux, uy, uz,
                         (double)*(const unsigned short*)(def + UD_KAMIDIST),
                         s_gui[GUI_RED], NULL, 0);
        else
            range_circle(u, ux, uy, uz,
                         (double)*(const short*)(def + UD_SIGHT),
                         s_gui[GUI_RED], NULL, 0);
        return;
    }

    /* ShowRanges: the labelled set, circles only — the labels are text and
       text is the L2 half of this port.

       `UD_CLOAKDIST` IS IN THIS SET AND IS DRAWN FIRST, gated only on the value
       being non-zero — NOT on the unit's cloak flag, which is the normal path's
       rule and not this one (`0x43921A`, ahead of the sight circle at
       `0x43924A`). Leaving it out cost a circle the engine draws whenever
       ShowRanges is on and the unit is cloakable at all. */
    {
        /* The engine's own reads are MIXED, and the table carries which is
           which: `movsx` for the cloak/sight/radar/sonar/jammer group
           (`0x439229`, `0x439267`, `0x4392A1`, `0x4392DB`, `0x439315`,
           `0x43934F`) and `and 0xffff` for builddistance, maneuver and
           kamikazedistance (`0x43937A`, `0x4393B7`, `0x439404`). Inert for any
           value under 32768, which all of them are in stock content — recorded
           because a blanket cast either way is a guess, and this one is free. */
        static const struct { int off; int sgn; const char* name; } rng[9] = {
            { UD_CLOAKDIST, 1, "mincloak"  }, { UD_SIGHT,    1, "sight"    },
            { UD_RADAR,     1, "radar"     }, { UD_SONAR,    1, "sonar"    },
            { UD_RJAM,      1, "radarjam"  }, { UD_SJAM,     1, "sonarjam" },
            { UD_BUILDDIST, 0, "build distance" },
            { UD_MANEUVER,  0, "maneuver"  },
            { UD_KAMIDIST,  0, "kamikazedistance" },
        };
        int i;
        /* The label SLOT is the number of circles drawn so far, which is what
           the engine's own `esi` holds: it starts at 0, the cloak circle pushes
           a literal 0 and then sets it to 1 (`0x439236`), each of the next
           seven pushes it and increments, and kamikazedistance — the last —
           pushes it without incrementing. The strings are the engine's own,
           at `0x505190/88/80/78/6C/60/50/44` and `0x503A0C`. */
        for (i = 0; i < 9; i++) {
            int v = rng[i].sgn ? (int)*(const short*)(def + rng[i].off)
                               : (int)*(const unsigned short*)(def + rng[i].off);
            if (v) range_circle(u, ux, uy, uz, (double)v, s_gui[GUI_YELLOW],
                                rng[i].name, nslot++);
        }
    }
    {
        static const char* const wname[3] =
            { "weapon1 range", "weapon2 range", "weapon3 range" };
        int flash = s_gui[(gameTime & 1) ? GUI_FLASH : GUI_RED];
        int i;
        for (i = 0; i < 3; i++) {
            const char* w;
            int rng;
            /* The third weapon is gated on the FIRST one's flag byte in this
               build (`test [edx+0x1f],2` at both 0x439443 and 0x43949D, where
               slot 1 correctly uses +0x3B). Reproduced rather than corrected —
               a "fix" here would show up as a marker the engine never drew. */
            int fl = (i == 2) ? 0 : i;
            if (!(*(const unsigned char*)(u + U_WEAPFLAGS + fl * 0x1C) & 2)) continue;
            w = *(const char* const*)(u + U_WEAP0 + i * 0x1C);
            if (!ptr_ok(w)) continue;
            rng = *(const int*)(w + W_RANGE);
            /* the weapon labels carry the slot as a LITERAL 0/1/2
               (`0x439457`, `0x439485`, `0x4394B0`), not the running count */
            if (rng) range_circle(u, ux, uy, uz, (double)rng, flash, wname[i], i);
        }
    }
}

int tagpu_order_gather(const TAGPU_FXVIEW* v)
{
    const ORDARENA* A;
    int slot, i, n, gameTime, showRanges;

    s_nrec = s_nline = s_ndot = s_nover = s_nlabel = 0;
    /* `passive` and `trace` both leave the draw with the engine, so ours must
       not also run — under trace the artifact is the two logged node lists,
       not a doubled screen. */
    if (s_armed != 1 || s_passive || s_trace) return 0;
    /* Without the redirect the engine is still drawing its own and ours would
       be a second set at the unzoomed position. Refuse rather than double. */
    if (!tagpu_markown_installed()) return 0;
    if (!ptr_ok(v) || !ptr_ok(v->ta)) return 0;
    /* Taking the draw is done HERE and not from the arm poll, for the reason
       tagpu_mark_armed spells out: the render is the only place that knows the
       pass is really running, and the arm poll is reached at the menus too. */
    tagpu_markown_set_orders(1);
    slot = g_pub;
    if (slot != 0 && slot != 1) return 0;
    /* TAKE A PRIVATE COPY FIRST, and walk that.

       The game thread republishes this arena once per DrawGameScreen marker
       block, which is ~83 times per presented frame (ui-markers.md 6.2) —
       two buffers are only enough while the READER outruns them, and building
       a frame's marker geometry does not: measured mid-bring-up, a seven-record
       arena was lapped part way through and three markers went missing from
       that frame. So the only thing that runs against the live arena is a
       memcpy of the records actually in use, which is microseconds; everything
       after it reads storage nobody else can touch.

       The generation re-read afterwards is what makes the copy trustworthy
       rather than merely fast: `g_gen` moving means the copy may straddle two
       publications, so it is taken again. Once, not in a loop — a second miss
       is drawn anyway, because every field in a record is a plain value, every
       unit pointer goes through sane_unit(), `bdef` is bounded against the
       UnitDef array, and every primitive is culled against the viewport, so the
       worst a straddled copy can produce is one frame of a marker in the wrong
       place. */
    for (i = 0; i < 2; i++) {
        /* Sample the generation BEFORE the slot, so a publication that lands
           between the two is seen as well as one that lands during the copy. */
        unsigned gen = g_gen;
        slot = g_pub;
        if (slot != 0 && slot != 1) return 0;
        A = &g_arena[slot];
        n = A->n;
        gameTime = A->gameTime;
        showRanges = A->showRanges;
        if (n < 0) n = 0;
        if (n > MAXORD) n = MAXORD;
        if (n) memcpy(s_rec, A->rec, (size_t)n * sizeof s_rec[0]);
        if (g_gen == gen) break;      /* nothing was published under us */
    }
    if (n <= 0) return 0;

    s_v   = v;
    s_gui = (const unsigned char*)(v->ta + OFF_GUICOL);
    s_px  = 1.0 / (double)(v->zoom > 0.0f ? v->zoom : 1.0f);

    for (i = 0; i < n; i++) {
        const ORDREC* r = &s_rec[i];
        if (r->mask & 0x01) draw_build(r, gameTime);
        if (r->mask & 0x02) { draw_sprite(r, gameTime, showRanges); draw_dots(r, gameTime); }
        if (r->mask & 0x04) draw_circle(r);
        if (r->mask & 0x08) draw_sprite(r, gameTime, showRanges);
        if (r->mask & 0x10) draw_ranges(r, gameTime, showRanges);
        s_nrec++;
    }
    return s_nrec;
}

void tagpu_order_frame_done(const TAGPU_FXVIEW* v)
{
    static unsigned last = 0;
    const ORDARENA* A;
    int slot;
    if (!s_log || s_armed != 1) return;
    if (v->frame_counter - last < 120) return;
    last = v->frame_counter;
    slot = g_pub;
    A = (slot == 0 || slot == 1) ? &g_arena[slot] : NULL;
    {
        char b[224];
        _snprintf(b, sizeof b,
            "order: arena=%d recs=%d drawn=%d lines=%d dots=%d labels=%d "
            "over=%d dropped=%d zoom=%.2f%s%s",
            slot, A ? A->n : -1, s_nrec, s_nline, s_ndot, s_nlabel, s_nover,
            A ? A->dropped : 0, v->zoom,
            s_passive ? " (passive)" : "", s_trace ? " (trace)" : "");
        flog(b);
    }
}
