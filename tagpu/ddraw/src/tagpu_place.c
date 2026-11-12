/* tagpu_place.c -- several buildings in one gesture (tagpu_place.h).

   With a building armed (cursor mode 14), a left press that carries Shift starts a gesture
   instead of placing; every mouse record while the button is held previews the shape from the
   press site to the cursor site; the release orders it. Shift + Ctrl over a unit, or over a
   queued build of a selected builder, previews a ring round it, ordered by a press and release.

   Three hooks, all on the GAME thread inside the in-play mouse handler 0x499200, which copies the
   record main+0x2C76 to its stack (0x499210..0x49921A), lets 0x498DA0 make the world point, runs
   the build cursor 0x4197D0 (0x499241, mode 14 in the viewport) and then routes the click:

   - an after-observer on 0x4197D0 evaluates: the shape, each site judged by the engine's own site
     test, the colours -- the rules are tasim_place.c's, over the engine's WORLDQ
     (tagpu_worldq.c). It runs after the engine has written the cursor rectangle, the altitude
     and bit 6 for this record, so the cursor site is the engine's (snapped by the resource
     module's before-observer when that applies).
   - the click routing's stub at 0x4993BF (tagpu_route.c) hands it the press and the release of
     a mode-14 record (tagpu_place_button).
   - the before-observer on the handler's entry (tagpu_route.c) ends the gesture when a record is
     handled in any other mode (tagpu_place_entry).

   Everything here is game-thread state and UI: the simulation never reads it, and what a commit
   causes -- order nodes made and removed through the engine's own roots -- is engine state the
   demo recorder keeps. The render thread sees the sites only as the packet's copied values. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "tagpu_place.h"
#include "tagpu_route.h"
#include "tagpu_placegeo.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_packet.h"
#include "tagpu_detour.h"
#include "tagpu_puppet.h"
#include "tagpu_log.h"
#include "tagpu_settings.h"
#include "tagpu_worldq.h"
#include "tasim_place.h"

#define D_STRIDE 0x249u
#define D_BASE   0x1439B
#define D_FLAGS  0x241
#define U_FLAGS  0x110
#define M_RECORD 0x2C76      /* the six-dword mouse record the handler acts on */
#define M_RECT   0x2C92      /* the build cursor: x0, altitude, z0, x1, altitude, z1 (world px) */
#define M_POINT  0x2CAA      /* the world point, 16.16 */
#define M_MODE   0x2CC3
#define M_TYPE   0x2CC4
#define M_REGION 0x2CC6      /* bit 1 the viewport, bit 5 a Shift placement, bit 6 the site is free */
#define M_GUICOL 0x0DCB      /* the GUI colour byte array, the build square's colours */

#define VA_CURSOR      0x004197D0u

#define NOTE_LOGS          8u

/* 0x4C1B80's ids (its table at 0x4C1C48 maps them onto GetAsyncKeyState, import 0x4FC350) */
enum { KEY_SHIFT = 0xF9, KEY_CONTROL = 0xFA, KEY_MENU = 0xFB };

typedef unsigned (__stdcall* SiteTest)(const void* def, unsigned cell, unsigned zero, const void* player);
typedef int (__stdcall* KeyDown)(int);
typedef unsigned short (__stdcall* HoverUnit)(void);
typedef void (__stdcall* Sound)(const char*, int);

/* One evaluation's or one commit's view: the armed type and the site test's arguments, and for
   the rules (tasim_place.c) the player's view opened with the build cursor's own site test and
   the player's selected units, slots ascending. */
typedef struct {
    TagpuWorld w;
    const char* def;
    unsigned type;
    int fx, fz;
    const char* player;
    WQ_ENGINE e;
    WORLDQ q;
    PLACE_IN in;
} Ctx;
_Static_assert(WQ_LIST_MAX <= PLACE_NO_SITE && PLACE_MAX_SITES < PLACE_NO_SITE &&
               TAGPU_LIM_UNITS <= PLACE_BUILDERS_MAX, "a plan's place and site fit its fields, a seat's units the set");

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
    PLACE_SITE site[PLACE_MAX_SITES];
} s_eval;
static struct { int active; uint16_t type; int16_t cx, cz; int stale[6]; } s_gesture;

/* the rules' scratch and the selection they read: one evaluation's or commit's at a time, all on
   this thread */
static PLACE_SCRATCH s_ps;
static uint16_t s_sel[PLACE_BUILDERS_MAX];

static void* s_ret[4];
static unsigned s_depth;

/* the level's counters, logged at level end */
static unsigned s_cEval, s_cCommit, s_cIssued, s_cRemoved, s_cRed, s_cMismatch, s_cTrunc,
                s_cListCap, s_cPlanCap;

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

/* The cell word the site test takes: x in the low half, z in the high, each a signed 16-bit
   value the test bounds itself (0x47D302..0x47D352: x, z >= 1, x + fx < W, z + fz < H), as
   0x47D820 does (0x47D834..0x47D871). */
static unsigned cell_word(int cx, int cz)
{
    return (unsigned)(uint16_t)cx | (unsigned)(uint16_t)cz << 16;
}

static int key_down(int id) { return ((KeyDown)0x004C1B80u)(id) != 0; }

/* the build square's colour of a judged site: the GUI colour table's index */
static uint8_t colour_of(const PLACE_SITE* s)
{
    return s->state == PLACE_SITE_FREE ? PLACE_OK : s->state == PLACE_SITE_TAKE ? PLACE_TOGGLE : PLACE_BLOCKED;
}

/* The view of this record: mode 14, the armed type bounded by UNITINFOCount, its footprint, and
   with `rules` the player's WORLDQ, judged by the build cursor's own site test (WQ_SITE_CURSOR),
   and the player's selected live units, of which tasim_place_builders takes the builders. */
static int context(Ctx* c, int rules)
{
    unsigned i;
    if (!tagpu_route_world(&c->w) || *(unsigned char*)(c->w.ta + M_MODE) != 14) return 0;
    c->type = *(unsigned short*)(c->w.ta + M_TYPE);
    if (!c->type || c->type >= c->w.ndefs) return 0;
    c->def = c->w.defs + c->type * D_STRIDE;
    if (!footprint(c->def, &c->fx, &c->fz)) return 0;
    c->player = c->w.ta + 0x1B63 + c->w.player * 0x14B;
    if (!rules) return 1;
    if (!tagpu_worldq_open(&c->e, &c->q, (int)c->w.player, WQ_SITE_CURSOR, NULL)) return 0;
    memset(&c->in, 0, sizeof c->in);
    c->in.type = (int)c->type;
    for (i = c->w.first; i <= c->w.last && c->in.nselected < (int)PLACE_BUILDERS_MAX; i++) {
        const char* u = tagpu_route_unit(&c->w, i);
        if (u && *(const unsigned char*)(u + U_OWNER) == c->w.player && (*(const unsigned*)(u + U_FLAGS) & 0x10u))
            s_sel[c->in.nselected++] = (uint16_t)i;
    }
    c->in.selected = s_sel;
    return 1;
}

/* The ring's target: the unit under the cursor (0x48CD80, the hover query the handler skips in
   mode 14; it answers the unit's +0xA8 and writes nothing but its own frame), else a builder's
   queued build whose footprint holds the world point (tasim_place_queued_at). Cells, never a
   pointer. */
static int ring_target(const Ctx* c, int* tx, int* tz, int* tw, int* th)
{
    unsigned id = ((HoverUnit)0x0048CD80u)();
    const char* u = id ? tagpu_route_unit(&c->w, id) : NULL;
    if (u && *(const unsigned short*)(u + U_INDEX) == id) {
        unsigned d = tagpu_route_def_id(&c->w, u);
        if (d && footprint(c->w.defs + d * D_STRIDE, tw, th)) {
            *tx = origin(*(const int*)(u + U_XFIX), *tw);
            *tz = origin(*(const int*)(u + U_YFIX), *th);
            return 1;
        }
    }
    return tasim_place_queued_at(&c->q, *(const int*)(c->w.ta + M_POINT) >> 20,
                                 *(const int*)(c->w.ta + M_POINT + 8) >> 20, tx, tz, tw, th, &s_ps);
}

/* THE SELF-CHECK: our verdict at the engine's cursor site against the engine's own (bit 6, just
   written by 0x4198D1..0x4198E1). They differ only if our cell or arguments are not the
   engine's; the counter is the regression's oracle and changes nothing at run time.
   THE RED TEST IS THE ENGINE'S OWN CALL SITE. s_test is the target of the call at 0x4198C6
   (tagpu_worldq_cursor_site, decoded at attach), the one the build cursor judges its site with
   (stock 0x47D2E0, queued_site's build_site_test in tagpu_patches.c when that fix is armed, or a
   foreign patch's), called with the arguments 0x4198A0..0x4198C6 passes: (def, cell, 0,
   main + 0x1B63 + main[0x2A42] * 0x14B), stdcall, `ret 0x10` -- the test the rules' view is
   opened with (WQ_SITE_CURSOR), so a gesture's site is judged exactly as a single click there
   would be, whatever is installed. queued_site's Shift exemption reads the live Shift, which
   every gesture holds. The test writes 0x51E684/0x51E688, which the AI's 0x47C770 reads too, so
   both are put back as they were; no AI tick runs inside the mouse handler. */
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

static void end_gesture(void) { s_gesture.active = 0; }

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
    int tx, tz, tw, th, shift, ctrl, alt;
    unsigned n, nb, i, k;
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
       that one must not end the gesture. It is the record the press saw (tagpu_route_stale_move),
       its x and y aside (tagpu_route_same_move); only a later move rewrites the rest (0x4C2360, from the window
       procedure's WM_MOUSEMOVE). Identity, not time: the records' clock ticks at 30 Hz (0x4B6340)
       and wraps short of 2^32. A move message in the press's own clock tick that repeats the
       stale one's wParam is not told apart: the gesture then outlives a release the handler never
       saw until the next move or press, which shows a preview and orders nothing -- an order
       needs a release this route takes, and a press ends the gesture first. */
    if (s_eval.rec[4] == WM_MOUSEMOVE && !(s_eval.rec[2] & MK_LBUTTON) &&
        !tagpu_route_same_move(s_eval.rec, s_gesture.stale)) end_gesture();
    if (s_gesture.type != c.type) end_gesture();
    /* the engine's rectangle is already snapped: x0 = 16 * the site's first cell (0x419849) */
    s_eval.cur_cx = *(const int*)(c.w.ta + M_RECT) >> 4;
    s_eval.cur_cz = *(const int*)(c.w.ta + M_RECT + 8) >> 4;
    self_check(&c);
    s_eval.valid = 1;
    /* Live keys, not the record's wParam: a modifier pressed without moving the mouse changes
       the shape at once. */
    shift = key_down(KEY_SHIFT); ctrl = key_down(KEY_CONTROL); alt = key_down(KEY_MENU);
    /* Shift up: nothing previews -- a release now drops the gesture (tagpu_place_button) -- but the
       gesture lives on, so that release is still consumed rather than placing on stock's path */
    if (!shift || (!ctrl && !s_gesture.active)) return;
    if (!context(&c, 1)) return;
    nb = tasim_place_builders(&c.q, &c.in, &s_ps);
    if (!tasim_place_snapshot(&c.q, &s_ps)) plans_overflowed();
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
    n = tasim_place_budget(n, nb);
    for (i = 0; i < n; i++) { s_eval.site[i].cx = cells[i].cx; s_eval.site[i].cz = cells[i].cz; }
    c.in.ring = s_eval.shape == PLACE_RING;
    tasim_place_classify(&c.q, &c.in, s_eval.site, n, &s_ps);
    /* a ring leaves out the sites that would cancel a queued build */
    for (i = k = 0; i < n; i++)
        if (s_eval.shape != PLACE_RING || s_eval.site[i].state != PLACE_SITE_TAKE)
            s_eval.site[k++] = s_eval.site[i];
    s_eval.n = k;
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

/* The commit, inside the in-play handler on the game thread, where the stock click calls its own
   root: tasim_place_commit, the gesture's sites judged afresh -- the preview is never trusted --
   by the same rule the preview's colours come from, then the stock click's sound and bit. */
static void commit(void)
{
    Ctx c;
    PLACE_SITE s[PLACE_MAX_SITES];
    uint16_t order[PLACE_MAX_SITES];
    PLACE_COMMIT_RES r;
    unsigned n, nb, cut, i;
    int shape = s_eval.shape;
    if (!context(&c, 1) || c.type != s_eval.type) return;
    nb = tasim_place_builders(&c.q, &c.in, &s_ps);
    n = tasim_place_budget(s_eval.n, nb);
    cut = s_eval.n - n;
    for (i = 0; i < n; i++) {
        s[i].cx = s_eval.site[i].cx; s[i].cz = s_eval.site[i].cz;
        order[i] = (uint16_t)i;
    }
    c.in.ring = shape == PLACE_RING;
    tasim_place_commit(&c.q, &c.in, s, n, order, &r, &s_ps);
    if (!r.planned) plans_overflowed();
    if (r.issued || r.removed) {
        /* stock's "oktobuild" (0x498F9D..0x498FA4), once a gesture, and the Shift-placement bit
           0x498FB4 sets, so that releasing Shift disarms as after a Shift-click (0x4992D8) */
        ((Sound)0x0047F1A0u)((const char*)0x00509660u, 0);
        *(unsigned char*)(c.w.ta + M_REGION) |= 0x20;
    }
    s_cCommit++; s_cIssued += r.issued; s_cRemoved += r.removed; s_cRed += r.red;
    s_cTrunc += cut; s_cListCap += r.capped_builders;
    tagpu_logf("place: commit shape=%s sites=%u red=%u builders=%u issued=%u removed=%u "
               "split=0 trunc=%u listcap=%u stale=%u", shape_name(shape), n, r.red, nb, r.issued,
               r.removed, cut, r.capped_builders, r.stale);
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

int tagpu_place_button(const int* rec, unsigned msg)
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
        tagpu_route_stale_move(s_gesture.stale);
        return 1;
    }
    if (msg != WM_LBUTTONUP || !s_gesture.active) return 0;
    end_gesture();
    /* Shift released before the button: the gesture is dropped, the building stays armed --
       unless an earlier placement of this Shift hold set main+0x2CC6 bit 5, when 0x4992D8
       disarms at the first record that finds Shift up, as stock does; that record's handler
       entry then ends the gesture (tagpu_place_entry) */
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
        const PLACE_SITE* s = &s_eval.site[i];
        out[i].rect[0] = 16 * s->cx;
        out[i].rect[1] = s->alt;
        out[i].rect[2] = 16 * s->cz;
        out[i].rect[3] = 16 * (s->cx + s_eval.fx);
        out[i].rect[4] = s->alt;
        out[i].rect[5] = 16 * (s->cz + s_eval.fz);
        out[i].type = s_eval.type;
        out[i].colour = colour_of(s);
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
        const PLACE_SITE* s = &s_eval.site[i];
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
        colour[k] = gui[colour_of(s)];
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

/* A record handled in any mode but 14 ends the gesture. In mode 1 the routing skips the route
   (0x4993BD) and 0x4197D0 is not called, so neither of the other hooks sees a disarm -- a right
   press (0x499100), Shift up after a Shift placement (0x4992D8), a group recall (0x48D9A0 ->
   0x495860) or the order check -- nor the release that follows it. Every disarm is followed by
   this entry in mode 1 before any re-arm: arming again takes a left click on the build menu,
   which cannot come while the gesture's own button is held, so the gesture's release (or, over
   the panel, the next record) is handled in mode 1 first. */
void tagpu_place_entry(void)
{
    const char* ta = *(const char* const*)TA_MAIN_PP;
    if (s_gesture.active && (!ta || *(const unsigned char*)(ta + M_MODE) != 14)) end_gesture();
}

void tagpu_place_init(void)
{
    static const unsigned char build[] = {0x83,0xEC,0x14,0x8B,0x15,0xE8,0x1D,0x51,0x00};
    if (!(s_test = (SiteTest)tagpu_worldq_cursor_site())) {
        tagpu_logf("place: the build cursor's site test (0x4198C6) is not a call; not armed");
        return;
    }
    if (!tagpu_detour_bytes_ok(VA_CURSOR, build, sizeof build)) {
        tagpu_logf("place: incompatible build cursor (0x4197D0); not armed");
        return;
    }
    /* until s_armed is set, tagpu_place_button answers stock */
    if (!tagpu_route_ok(TAGPU_ROUTE_STUB | TAGPU_ROUTE_ENTRY)) {
        tagpu_logf("place: the click route (0x4993BF) or the handler's entry (0x499200) is not "
                   "installed; not armed");
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
