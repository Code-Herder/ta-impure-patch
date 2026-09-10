/* tagpu_vpwide.c — widen the engine's addressable viewport at zoom < 1.
   See tagpu_vpwide.h for what the rect is, who reads it and why the four
   kinds of reader need four different things. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "dd.h"
#include "tagpu_opt.h"
#include "tagpu_vpwide.h"
#include "tagpu_detour.h"
#include "tagpu_zoom.h"

#define TA_MAINPP    0x00511DE8u

/* the six ints at main+0x37E27 */
#define OFF_VP_L     0x37E27
#define OFF_VP_T     0x37E2B
#define OFF_VP_R     0x37E2F
#define OFF_VP_B     0x37E33
#define OFF_VIEW_W   0x37E37
#define OFF_VIEW_H   0x37E3B

/* The four constants `0x497F40` hardcodes when it builds the rect: L = 0x80,
   T = 0x20, R = screenW - 1, B = screenH - 33, and then W = R-L+1, H = B-T+1.
   Every other consumer of the rect projects with the same 0x80/0x20 origin,
   which is what makes the whole true rect derivable from the SCREEN dimensions
   at `+0x37E1F`/`+0x37E23` — fields we never write — while we own L/T/R/B. */
#define VP_TRUE_L    0x80
#define VP_TRUE_T    0x20
#define VP_R_INSET   1           /* R = screenW - VP_R_INSET  */
#define VP_B_INSET   33          /* B = screenH - VP_B_INSET  */
#define OFF_SCREEN_W 0x37E1F
#define OFF_SCREEN_H 0x37E23

#define OFF_EYEX     0x1431F
#define OFF_EYEY     0x14323
#define OFF_MAP_W    0x1422B     /* map size in world px                      */
#define OFF_MAP_H    0x1422F
#define OFF_PLOT_C   0x14233     /* the PLOT grid GetGridPosPLOT indexes      */
#define OFF_PLOT_R   0x14237
#define OFF_MM_CLICK 0x142BB     /* the minimap's click RECT                  */
#define OFF_MM_X     0x142E7     /* i16 minimap rect on screen                */
#define OFF_MM_Y     0x142E9
#define OFF_MM_W     0x142EB
#define OFF_MM_H     0x142ED
/* The dispatched mouse record, 6 dwords: x, y, wParam, time, msg, dblclk-flag.
   0x499200 copies it to the stack and hands that copy to 0x498DA0, but
   GetUnitAtMouse 0x48CD80 (0x499278) and the routing test at 0x469DE1 read the
   FIELD, so a repair has to be written back here as well as into the copy. */
#define OFF_MOUSE_X  0x2C76
#define OFF_MOUSE_Y  0x2C7A
#define REC_MSG      4           /* dword index of the message id in the record */
#define MREC_BYTES   24          /* the record is 6 dwords, ring entry and all  */
#define TA_MOUSEPP   0x0051FBD0u /* the mouse/UI object; 0x4B6220 returns it    */
#define OFF_MREC     0x196       /* its copy of the current record              */
#define OFF_MOUSEFL  0x2CC6      /* bit0 on minimap, bit1 on world, bit2 either */
#define OFF_TPOS     0x2CAA      /* GetTPosition output: x,y,z in 20.12       */
#define OFF_GRIDXY   0x2C8E      /* two i16: the map cell under the cursor    */
#define OFF_HOVERFEAT 0x2CBC     /* u16 feature id under the cursor           */

#define VA_SETCLIP        0x004C6B10u  /* thiscall(self, l,t,r,b), ret 0x10   */
#define SITE_SETCLIP1     0x00468D85u  /* the three DrawGameScreen call sites */
#define SITE_SETCLIP2     0x0046964Fu
#define SITE_SETCLIP3     0x00469F95u
#define VA_MOUSEWORLD     0x00498DA0u  /* stdcall(int* pos), ret 4            */
#define SITE_MOUSEWORLD   0x00499221u  /* its ONE call site                   */
#define VA_INRECT         0x004B6720u  /* IsPositionInRect, stdcall, ret 0x0C */
#define VA_GETTPOS        0x00484B50u  /* GetTPosition, stdcall, ret 0x0C     */
#define VA_GRIDPLOT       0x00481550u  /* GetGridPosPLOT, stdcall, ret 8      */
#define VA_GRIDFEAT       0x00421E60u  /* GetGridPosFeature, stdcall, ret 4   */

/* The three places TA's own window procedure unpacks a mouse lParam, one per
   arm of its 0x200..0x206 jump table (move, button-up, button-down). All three
   are `AND ECX,0xffff` + `SHR EAX,0x10` — the LOWORD/HIWORD idiom, which is
   ZERO-extending, so a client x of -20 arrives as 65516 and the event is lost.
   Microsoft's own guidance is GET_X_LPARAM (a SIGNED 16-bit read) for exactly
   this reason; the patch is that fix, and for any position a real mouse can
   report (0..width-1, width <= 32767) the two are bit-for-bit identical. */
#define SITE_UNPACK1      0x004B5E5Fu
#define SITE_UNPACK2      0x004B5EC0u
#define SITE_UNPACK3      0x004B5F0Cu
/* AND ECX,0xffff | SHR EAX,0x10   ->   MOVSX ECX,CX | nop nop nop | SAR EAX,0x10 */
static const unsigned char UNPACK_WAS[9] = { 0x81,0xE1,0xFF,0xFF,0x00,0x00, 0xC1,0xE8,0x10 };
static const unsigned char UNPACK_NOW[9] = { 0x0F,0xBF,0xC9, 0x90,0x90,0x90, 0xC1,0xF8,0x10 };

typedef void           (__thiscall *PFN_SETCLIP)(void* self, int l, int t, int r, int b);
typedef void           (__stdcall  *PFN_MOUSEWORLD)(int* pos);
typedef int            (__stdcall  *PFN_INRECT)(const int* rect, int x, int y);
typedef void           (__stdcall  *PFN_GETTPOS)(int x, int y, int* out);
typedef void*          (__stdcall  *PFN_GRIDPLOT)(int gx, int gz);
typedef unsigned short (__stdcall  *PFN_GRIDFEAT)(void* plot);

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

static int iround(float v) { return (int)(v >= 0.0f ? v + 0.5f : v - 0.5f); }

/* The true rect from the SCREEN dimensions — the two fields we never write, so
   this stays right even while L/T/R/B are ours and even if W/H were corrupted.
   Returns 0 when the screen dimensions themselves do not look sane. */
static int true_rect_of(const char* ta, int* R, int* B, int* W, int* H)
{
    int sw = *(const int*)(ta + OFF_SCREEN_W);
    int sh = *(const int*)(ta + OFF_SCREEN_H);
    if (sw < 320 || sh < 200 || sw > 8192 || sh > 8192) return 0;
    *R = sw - VP_R_INSET;
    *B = sh - VP_B_INSET;
    *W = *R - VP_TRUE_L + 1;
    *H = *B - VP_TRUE_T + 1;
    return *W >= 64 && *H >= 64;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static int  s_installed;        /* the mouse->world redirect went in         */
static int  s_widenArmed;       /* ...and tagpu_vpwide.on, so the rect widens */
static volatile int s_verified; /* the rect matched what 0x497F40 builds     */
/* volatile for the same reason s_wide below is: since the clip guard began
   calling tagpu_vpwide_true_rect(), this flag is read on the GAME thread
   three times per DrawGameScreen, and a stale 0 there makes true_rect_of
   fall back to the FIELD -- which is the widened rect -- turning the
   viewport clamp into the identity and re-opening the side panel. */
static int  s_saidUnverified;   /* the diagnostic is one-shot                */
static int  s_saidRepair;       /* the W/H repair diagnostic is one-shot     */
/* VOLATILE, and not merely because two threads read it: the ordering the
   comment on restore() relies on is a promise about the COMPILER as much as
   about x86, and a plain int lets -O2 sink the store past the rect writes. */
static volatile LONG s_wide;    /* we are currently writing the rect         */

/* Published for the two readers on other threads: the message thread's ring
   test and markown's capture window on the game thread. Five aligned 32-bit
   slots, the same discipline tagpu_zoom uses. `s_pubLive` is stored last and
   cleared first, and x86 does not reorder stores with stores or loads with
   loads, so "live" never advertises a rect that was not written. The values
   themselves can still change under a reader mid-read while live stays set —
   only when the zoom LEVEL changes, and the cost is one frame's ring decision
   or one frame's capture rect taken from a mixed pair. Not worth a lock on the
   input path; a steady zoom writes nothing at all. */
static volatile LONG s_pubL, s_pubT, s_pubW, s_pubH, s_pubLive;

/* ---- the clip guard -------------------------------------------------------

   0x4C6B10 stores its four arguments straight into the surface's clip rect
   (+0x1C..+0x28) and clamps nothing — SurfaceCreateNamed initialises that same
   field to (0, 0, w-1, h-1), which is the bound this restores. DrawGameScreen
   feeds it the viewport rect, so without this the widened rect would license
   any engine drawer still running inside the viewport to write outside the
   allocation. The clamp belongs here and not on the rect itself: the bound
   readers need the rect wide, the clip does not. */
static void __thiscall vpw_setclip(void* self, int l, int t, int r, int b)
{
    const int* s = (const int*)self;
    if (ptr_ok(s) && !IsBadReadPtr(s, 8)) {
        int w = s[0], h = s[1];             /* SurfaceCreateNamed: +0 w, +4 h */
        if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
            if (l < 0) l = 0;
            if (t < 0) t = 0;
            if (r > w - 1) r = w - 1;
            if (b > h - 1) b = h - 1;
        }
    }
    /* ...and never past the TRUE VIEWPORT, which is a different bound and the
       one that matters for the picture. The allocation clamp above keeps the
       write inside the surface; it still leaves the whole side panel and the
       top and bottom strips fair game, and NOTHING EVER REPAINTS THEM — our
       key fill covers the true viewport only (that is why it takes its rect
       from tagpu_vpwide_true_rect and not from the field), and the engine
       redraws the panel on damage it knows about, which a stray world draw is
       not. So one frame in which an engine drawer runs with the wide rect
       leaves marks outside the viewport for the rest of the session: measured
       2026-09-09 as the stuck selection boxes a 500 v 500 fight with the whole
       army selected paints over the panel at zoom < 1, permanent and
       accumulating (the engine's selection rect is the drawer that reaches
       them: markown hands the whole set back for a frame whenever the native
       pass came up one box short, and the engine projects each one at the
       UNZOOMED position, which at 0.42x is up to 1.4 screens away from where
       the unit is drawn).

       Clamping to the true rect is exactly the bound stock TA sets here —
       DrawGameScreen feeds these three sites the viewport rect, and unwidened
       that rect IS the true one — so it can never clip anything the engine
       would otherwise have drawn on screen. At zoom >= 1 and with the widening
       disarmed it is the identity. */
    {
        const char* ta = *(const char* const*)TA_MAINPP;
        int tL, tT, tW, tH;
        tagpu_vpwide_true_rect(ta, &tL, &tT, &tW, &tH);
        /* NOT GATED ON THE SURFACE PROBE ABOVE, and that is the point. This
           clamp only ever NARROWS, so it needs to know nothing about the
           allocation; riding it on `IsBadReadPtr` succeeding would make the
           bound conditional on a probe, which CLAUDE.md rules out as a safety
           argument — and a single call with an unreadable `self` would then
           re-license the permanent side-panel marks this exists to stop.
           A landing review caught exactly that [2026-09-10]. */
        if (tW > 0 && tH > 0) {
            int cl = l < tL ? tL : l, ct = t < tT ? tT : t;
            int cr = r > tL + tW - 1 ? tL + tW - 1 : r;
            int cb = b > tT + tH - 1 ? tT + tH - 1 : b;
            /* a rect that does not meet the viewport at all is not one of
               these sites' — leave it alone rather than handing the engine an
               inverted rect */
            if (cl <= cr && ct <= cb) { l = cl; t = ct; r = cr; b = cb; }
        }
    }
    ((PFN_SETCLIP)VA_SETCLIP)(self, l, t, r, b);
}

/* ---- the mouse point ------------------------------------------------------

   0x498DA0 turns the mouse position into the world point, the map cell and the
   feature under the cursor. It is the ONE place either of the two things this
   stub does can be done, and it does them in this order.

   FIRST, THE ZOOM. `fake_GetCursorPos` answers the TRUE pointer, so the engine
   draws its cursor sprite under it and its screen-space readers of that poll
   keep working — but the record the poll leaves at `[obj+0x196]`, and through
   the dispatch at `main+0x2C76`, is then in SCREEN space, and the arithmetic
   below is 1:1. So the unzoomed `u` is computed here, from that one pointer
   sample, and written back into `main+0x2C76` as well as into the copy we were
   passed: GetUnitAtMouse `0x48CD80` (called at `0x499278`; `0x499283` is where its answer
   is stored) and the routing test at `0x469DE1` read the field, not the copy. Records that came off the event
   ring already carry the message's own `u` and are left alone
   (record_is_message below).

   SECOND, THE ORIGIN, and only while the rect is ours. 0x498DA0 is the ONE
   reader that uses L and T as the screen->world ORIGIN rather than as bounds:

       world = eye + clamp(pos, L, R) - L

   With the rect widened that reads the origin off by (0x80 - L), which would
   offset every ground order, build placement and feature hover by the same
   amount. This redoes the same computation with the TRUE origin and the WIDE
   clamp — the only two things that differ — and hands anything else to the
   engine's own function, which is then running on a repaired position.

   Redoing it rather than biasing the eye across the call is deliberate: the
   engine runs this on the game thread while our overlay reads the eye on
   cnc-ddraw's render thread, so a transient bias would be a visible one-frame
   jump every few thousand frames. */
/* Was this record pushed on the engine's own event RING, or is it the
   `[obj+0x196]` fallback the GetCursorPos polls keep?

   It matters because a RING record already carries the right `u`:
   tagpu_zoom_mouse_lparam rewrote it at the door, from the pointer sample the
   press was actually made at, and recomputing it here would replace where the
   player pressed with where the pointer is now. Only the button arms of TA's
   window procedure push — `0x4B5EB2` for the four single presses and releases,
   `0x4B5EFE` for the two double-clicks, both through `0x4C2E30`; the move arm
   `0x4B5E51` copies its record into `[obj+0x196]` instead (`0x4C2360`), where
   the next drawing poll overwrites x and y.

   TWO TESTS, BECAUSE THE MESSAGE ID ALONE CANNOT BE TRUSTED. `[obj+0x196]` has
   a second writer: the input reset at `0x4B5A88` zeroes only the first three
   dwords of the record it hands `0x4C2360`, leaving time, msg and the
   double-click flag as stack garbage. The drawing polls then put a REAL pointer
   back in x and y while msg stays garbage — so a msg that happens to land in
   0x201..0x206 would make a poll record look like a press and skip the repair,
   leaving `main+0x2C76` holding a SCREEN position while zoomed, which at 0.25x
   is most of the frame out. The record must therefore ALSO differ from
   `[obj+0x196]` to count as a ring record: the fallback is a `rep movsd` of
   those very six dwords, garbage included, so it can never differ from them,
   while a real ring entry differs in at least its timestamp. Neither test alone
   is enough; the pair cannot be fooled by uninitialised data. */
static int record_is_message(const int* pos)
{
    const void* obj;

    if (pos[REC_MSG] < WM_LBUTTONDOWN || pos[REC_MSG] > WM_RBUTTONDBLCLK)
        return 0;                       /* a move, or the reset's own garbage */

    obj = *(const void* const*)TA_MOUSEPP;
    if (!ptr_ok(obj) || IsBadReadPtr((char*)obj + OFF_MREC, MREC_BYTES))
        return 1;                       /* no object to check against         */
    return memcmp(pos, (const char*)obj + OFF_MREC, MREC_BYTES) != 0;
}

static void __stdcall vpw_mouse_world(int* pos)
{
    char* ta;
    unsigned char* fl;
    int x, y, sx, sy, wx, wy, gx, gy;
    int tR = 0, tB = 0, tW = 0, tH = 0, cL, cT, cR, cB, inWorld = 0;

    ta = *(char**)TA_MAINPP;
    if (!pos || !ptr_ok(ta) || !true_rect_of(ta, &tR, &tB, &tW, &tH)) {
        ((PFN_MOUSEWORLD)VA_MOUSEWORLD)(pos); return;
    }

    /* The true pointer, and the same `g_ddraw.cursor` the composite trusts. */
    sx = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.x, 0);
    sy = (int)InterlockedExchangeAdd((LONG*)&g_ddraw.cursor.y, 0);

    /* GIVE THE ENGINE ITS `u` BACK. `fake_GetCursorPos` answers the TRUE
       pointer now, so the engine blits its cursor sprite under it — and the
       record the poll left at `[obj+0x196]` is in screen space, which is the
       one number the engine's 1:1 screen->world arithmetic must not be handed.
       This is the place that arithmetic happens, so the transform belongs here:
       one sample of the pointer, converted once, written into both the copy we
       were passed and the field the readers just after this call read directly.

       At zoom 1 tagpu_zoom_to_engine() reports "nothing to do" and not a byte
       moves; on the screen-space UI it reports the same and the poll's own
       answer stands, which is what the identity there always was. */
    {
        int ux = sx, uy = sy;
        if (tagpu_zoom_to_engine(&ux, &uy) && !record_is_message(pos)) {
            pos[0] = ux;
            pos[1] = uy;
            *(volatile int*)(ta + OFF_MOUSE_X) = ux;
            *(volatile int*)(ta + OFF_MOUSE_Y) = uy;
        }
    }

    /* Whenever the rect is not ours the engine's own conversion is the right
       one — it is the true origin and the true bounds — and it now runs on a
       repaired position. That covers zoom 1, zoom > 1 (where `u` is always
       inside the viewport) and vpwide disarmed. */
    if (!s_wide) { ((PFN_MOUSEWORLD)VA_MOUSEWORLD)(pos); return; }

    x = pos[0];
    y = pos[1];
    fl = (unsigned char*)(ta + OFF_MOUSEFL);

    /* IS THE POINTER ON THE WORLD, OR ON THE SCREEN-SPACE UI? The widened rect
       makes that ambiguous from `pos` alone: at 0.5× the engine coordinates
       [0,128) are reached BOTH by a ring pointer (transformed) and by a pointer
       on the side panel (passed through 1:1). The true pointer position settles
       it, and it is the same `g_ddraw.cursor` the composite trusts.

       It has to be settled, because everything below keys off it:
         * the CLAMP and the origin — a panel pointer must get the engine's own
           1× answer, not one taken against a rect that now spans the screen;
         * bit1 of `main+0x2CC6`, "the pointer is on the world", which
           `0x499226` and `0x491CC0` use to decide between resetting the cursor
           to the arrow and running the unit pick or the build placement;
         * the MINIMAP branch — the minimap's click rect is (10,0)-(115,125),
           and a ring pointer at screen (320,220) lands inside it in engine
           coordinates. Without this gate a click out in the world would jump
           the camera as though the minimap had been clicked. */
    {
        inWorld = sx >= VP_TRUE_L && sy >= VP_TRUE_T && sx <= tR && sy <= tB;
        if (inWorld) {
            cL = *(const int*)(ta + OFF_VP_L); cT = *(const int*)(ta + OFF_VP_T);
            cR = *(const int*)(ta + OFF_VP_R); cB = *(const int*)(ta + OFF_VP_B);
        } else {
            cL = VP_TRUE_L; cT = VP_TRUE_T; cR = tR; cB = tB;
        }
    }

    if (!inWorld &&
        ((PFN_INRECT)VA_INRECT)((const int*)(ta + OFF_MM_CLICK), x, y) && !(*fl & 8)) {
        /* the pointer is over the MINIMAP — screen-space, no viewport rect in
           it at all, so this half is the engine's own arithmetic unchanged */
        int mmX = *(const short*)(ta + OFF_MM_X), mmY = *(const short*)(ta + OFF_MM_Y);
        int mmW = *(const short*)(ta + OFF_MM_W), mmH = *(const short*)(ta + OFF_MM_H);
        if (mmW <= 0 || mmH <= 0) { ((PFN_MOUSEWORLD)VA_MOUSEWORLD)(pos); return; }
        wx = (x - mmX) * *(const int*)(ta + OFF_MAP_W) / mmW;
        wy = (y - mmY) * *(const int*)(ta + OFF_MAP_H) / mmH;
        *fl = (unsigned char)((*fl | 1) & ~2);
    } else {
        /* TRUE origin, WIDE clamp: the rect's L/T/R/B are ours right now, and
           the clamp against them is exactly what makes the ring addressable */
        wx = *(const int*)(ta + OFF_EYEX) + clampi(x, cL, cR) - VP_TRUE_L;
        wy = *(const int*)(ta + OFF_EYEY) + clampi(y, cT, cB) - VP_TRUE_T;
        *fl = (unsigned char)((*fl & ~3)
            | ((x >= cL && x <= cR && y >= cT && y <= cB) ? 2 : 0));
    }
    *fl = (unsigned char)((*fl & ~4) | (((*fl & 3) != 0) << 2));

    /* KEEP THE WORLD POINT ON THE MAP. The widened clamp above reaches world
       positions the 1x viewport never could, and the engine's own chain from
       here is not defensive about them: GetGridPosPLOT returns NULL for a cell
       outside 0x14233 x 0x14237 and GetGridPosFeature dereferences whatever it
       is handed. That is a real crash, seen once — an edge scroll at 0.5x on
       Two Continents took an access violation at 0x421E64 reading [NULL+8].
       At 1x the engine cannot reach it because its own eye clamp keeps the
       viewport on the map; zoomed out we have to keep it there ourselves. */
    {
        int mw = *(const int*)(ta + OFF_MAP_W), mh = *(const int*)(ta + OFF_MAP_H);
        if (mw > 0 && mh > 0) {
            wx = clampi(wx, 0, mw - 1);
            wy = clampi(wy, 0, mh - 1);
        }
    }
    ((PFN_GETTPOS)VA_GETTPOS)(wx, wy, (int*)(ta + OFF_TPOS));
    gx = (int)(unsigned short)(*(const unsigned*)(ta + OFF_TPOS)     >> 20);
    gy = (int)(unsigned short)(*(const unsigned*)(ta + OFF_TPOS + 8) >> 20);
    {   /* GetTPosition answers in terrain space, and its height correction can
           still push the row past the last one near the bottom edge — so the
           cell is clamped too, and the plot is null-checked even then. */
        int pc = *(const int*)(ta + OFF_PLOT_C), pr = *(const int*)(ta + OFF_PLOT_R);
        void* plot;
        if (pc > 0 && pr > 0) {
            gx = clampi(gx, 0, pc - 1);
            gy = clampi(gy, 0, pr - 1);
        }
        *(short*)(ta + OFF_GRIDXY)     = (short)gx;
        *(short*)(ta + OFF_GRIDXY + 2) = (short)gy;
        plot = ((PFN_GRIDPLOT)VA_GRIDPLOT)(gx, gy);
        *(unsigned short*)(ta + OFF_HOVERFEAT) =
            plot ? ((PFN_GRIDFEAT)VA_GRIDFEAT)(plot) : (unsigned short)0xFFFF;
    }
}

/* ---- the rect ------------------------------------------------------------ */

void tagpu_vpwide_true_rect(const char* ta, int* L, int* T, int* W, int* H)
{
    int tR, tB, tW, tH;
    *L = *T = *W = *H = 0;              /* a zero rect fails every caller closed */
    if (!ptr_ok(ta)) return;
    if (s_verified && true_rect_of(ta, &tR, &tB, &tW, &tH)) {
        /* derived, so it is right even while L/T/R/B are ours */
        *L = VP_TRUE_L; *T = VP_TRUE_T; *W = tW; *H = tH;
        return;
    }
    *L = *(const int*)(ta + OFF_VP_L);
    *T = *(const int*)(ta + OFF_VP_T);
    *W = *(const int*)(ta + OFF_VIEW_W);
    *H = *(const int*)(ta + OFF_VIEW_H);
}

int tagpu_vpwide_mouse_world_live(void)
{
    return s_installed;
}

int tagpu_vpwide_addressable(int* L, int* T, int* W, int* H)
{
    int l, t, w, h;
    if (!s_pubLive) return 0;
    l = (int)s_pubL; t = (int)s_pubT; w = (int)s_pubW; h = (int)s_pubH;
    if (w <= 0 || h <= 0) return 0;      /* the outputs are left alone */
    *L = l; *T = t; *W = w; *H = h;
    return 1;
}

/* `s_wide` is what the game thread's 0x498DA0 stub reads to decide whether the
   rect is ours. It is raised BEFORE the widening stores and lowered AFTER the
   restoring ones, so the only state the two threads can ever disagree about is
   "ours, but still holding the true rect" — and there the stub's true origin
   and its clamp against the true L/R reproduce the engine's own answer
   exactly. The other order would hand it a wide rect with a true origin. */
static void restore(char* ta)
{
    int tR, tB, tW, tH;
    s_pubLive = 0;                       /* stop advertising it, always */
    if (!s_wide) return;
    if (!true_rect_of(ta, &tR, &tB, &tW, &tH)) return;   /* not with garbage */
    *(volatile int*)(ta + OFF_VP_L) = VP_TRUE_L;
    *(volatile int*)(ta + OFF_VP_T) = VP_TRUE_T;
    *(volatile int*)(ta + OFF_VP_R) = tR;
    *(volatile int*)(ta + OFF_VP_B) = tB;
    s_wide = 0;
    flog("vpwide: viewport rect restored to 1x");
}

void tagpu_vpwide_frame(float z)
{
    char* ta;
    int tR, tB, tW, tH, aL, aT, aR, aB;
    float cx, cy;

    if (!s_installed || !s_widenArmed) return;   /* the repair-only arm writes
                                                    no rect, so it verifies
                                                    nothing and repairs nothing */
    ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) return;
    if (!true_rect_of(ta, &tR, &tB, &tW, &tH)) { restore(ta); return; }

    /* Verify once, on a frame we do not own, that the rect really is what
       0x497F40 builds — everything below assumes that construction, so a build
       or a resolution that disagrees must widen nothing rather than half of it. */
    if (!s_verified) {
        if (s_wide) return;              /* cannot verify a rect we wrote */
        if (*(const int*)(ta + OFF_VP_L) == VP_TRUE_L &&
            *(const int*)(ta + OFF_VP_T) == VP_TRUE_T &&
            *(const int*)(ta + OFF_VP_R) == tR &&
            *(const int*)(ta + OFF_VP_B) == tB &&
            *(const int*)(ta + OFF_VIEW_W) == tW &&
            *(const int*)(ta + OFF_VIEW_H) == tH) {
            char b[128];
            s_verified = 1;
            _snprintf(b, sizeof b, "vpwide: true viewport rect verified (%d,%d %dx%d)",
                      VP_TRUE_L, VP_TRUE_T, tW, tH);
            flog(b);
        } else {
            if (!s_saidUnverified) {
                char b[192];
                s_saidUnverified = 1;
                _snprintf(b, sizeof b,
                    "vpwide: NOT widening — rect (%d,%d %d,%d %dx%d) is not the "
                    "(%d,%d %d,%d %dx%d) 0x497F40 builds",
                    *(const int*)(ta + OFF_VP_L), *(const int*)(ta + OFF_VP_T),
                    *(const int*)(ta + OFF_VP_R), *(const int*)(ta + OFF_VP_B),
                    *(const int*)(ta + OFF_VIEW_W), *(const int*)(ta + OFF_VIEW_H),
                    VP_TRUE_L, VP_TRUE_T, tR, tB, tW, tH);
                flog(b);
            }
            return;
        }
    }

    /* W AND H CAN BE COLLATERAL DAMAGE, so they are checked every frame.
       0x497F40 computes W = R - L + 1 by RE-READING L (0x4981C9 writes it,
       0x498214 reads it back) and H likewise from T. Our store of the widened L
       landing in that window — re-entering the game screen while a zoomed view
       is live — leaves W hundreds of pixels too wide, and the eye clamp
       0x41C3C0 then derives maxEye = map - W and oscillates the camera. They
       are the two fields this module is built around NOT writing, so when they
       disagree with the screen dimensions they are put back rather than
       tolerated. */
    if (*(const int*)(ta + OFF_VIEW_W) != tW || *(const int*)(ta + OFF_VIEW_H) != tH) {
        if (!s_saidRepair) {
            char b[160];
            s_saidRepair = 1;
            _snprintf(b, sizeof b,
                "vpwide: REPAIRED view size %dx%d -> %dx%d (0x497F40 raced our L/T store)",
                *(const int*)(ta + OFF_VIEW_W), *(const int*)(ta + OFF_VIEW_H), tW, tH);
            flog(b);
        }
        *(volatile int*)(ta + OFF_VIEW_W) = tW;
        *(volatile int*)(ta + OFF_VIEW_H) = tH;
    }

    if (!(z > 0.05f && z < 1.0f)) { restore(ta); return; }

    /* The addressable rect is not "the viewport, bigger" — it is exactly the
       range tagpu_zoom's transform produces, computed with the same formula
       about the same centre so the two cannot disagree at the edges, plus one
       pixel of slack each way against the rounding. */
    cx = (float)VP_TRUE_L + (float)tW * 0.5f;
    cy = (float)VP_TRUE_T + (float)tH * 0.5f;
    aL = iround(((float)VP_TRUE_L            - cx) / z + cx) - 1;
    aR = iround(((float)(VP_TRUE_L + tW - 1) - cx) / z + cx) + 1;
    aT = iround(((float)VP_TRUE_T            - cy) / z + cy) - 1;
    aB = iround(((float)(VP_TRUE_T + tH - 1) - cy) / z + cy) + 1;
    if (aR - aL + 1 > 32768 || aB - aT + 1 > 32768) { restore(ta); return; }

    /* Compare against the FIELD, not against what we last wrote: at a steady
       zoom that is four loads and no stores, and it is also what notices the
       game-screen callback having put the true rect back underneath us. */
    if (!s_wide ||
        *(const int*)(ta + OFF_VP_L) != aL || *(const int*)(ta + OFF_VP_T) != aT ||
        *(const int*)(ta + OFF_VP_R) != aR || *(const int*)(ta + OFF_VP_B) != aB) {
        s_pubLive = 0;
        s_wide = 1;
        *(volatile int*)(ta + OFF_VP_L) = aL;
        *(volatile int*)(ta + OFF_VP_T) = aT;
        *(volatile int*)(ta + OFF_VP_R) = aR;
        *(volatile int*)(ta + OFF_VP_B) = aB;
        s_pubL = aL; s_pubT = aT; s_pubW = aR - aL + 1; s_pubH = aB - aT + 1;
        s_pubLive = 1;
    }
}

/* ---- install ------------------------------------------------------------- */

/* an `E8 <rel32>` at `site` whose target is `expect`? */
static int site_is(unsigned int site, unsigned int expect)
{
    const unsigned char* p = (const unsigned char*)(size_t)site;
    if (IsBadReadPtr((void*)p, 5)) return 0;
    return p[0] == 0xE8 &&
           *(const unsigned int*)(p + 1) == expect - (site + 5);
}

static int bytes_are(unsigned int site, const unsigned char* want)
{
    const void* p = (const void*)(size_t)site;
    return !IsBadReadPtr(p, 9) && memcmp(p, want, 9) == 0;
}

static int redirect(unsigned int site, void* target)
{
    unsigned char b[5];
    b[0] = 0xE8;
    *(unsigned int*)(b + 1) = (unsigned int)(size_t)target - (site + 5);
    return tagpu_detour_write(site, b, 5);
}

void tagpu_vpwide_init(void)
{
    int ok;

    /* TWO ARM FILES, BECAUSE THE STUB HAS TWO JOBS (see "the mouse point").
       Widening the rect is what `tagpu_vpwide.on` buys. The mouse->world repair
       is not optional once `fake_GetCursorPos` answers the true pointer — the
       engine would otherwise name the world under the SCREEN position — and the
       zoom transform goes live from the lever alone, which `tagpu_zoom.on` does
       not gate. So `zoom.on` arms the repair by itself, and nothing else:
       one call-site redirect, no rect ever written. */
    s_widenArmed = tagpu_opt_on("tagpu_vpwide.on");
    if (!s_widenArmed && !tagpu_opt_on("tagpu_zoom.on")) return;

    if (!site_is(SITE_MOUSEWORLD, VA_MOUSEWORLD)) {
        flog("vpwide: NOT armed — 0x499221 is not a call to 0x498DA0");
        s_widenArmed = 0;
        return;
    }

    /* all-or-nothing for the WIDENING half: every byte is checked before any of
       them is written, and a build that fails it still gets the repair below —
       which is one redirect on a site already matched, and which the true
       pointer reaching the engine now depends on. */
    if (s_widenArmed &&
        (!site_is(SITE_SETCLIP1, VA_SETCLIP) ||
         !site_is(SITE_SETCLIP2, VA_SETCLIP) ||
         !site_is(SITE_SETCLIP3, VA_SETCLIP) ||
         !bytes_are(SITE_UNPACK1, UNPACK_WAS) ||
         !bytes_are(SITE_UNPACK2, UNPACK_WAS) ||
         !bytes_are(SITE_UNPACK3, UNPACK_WAS))) {
        flog("vpwide: NOT widening — engine bytes differ at one of "
             "0x468D85/0x46964F/0x469F95/"
             "0x4B5E5F/0x4B5EC0/0x4B5F0C");
        s_widenArmed = 0;
    }
    ok = redirect(SITE_MOUSEWORLD, (void*)vpw_mouse_world);
    if (s_widenArmed) {
        ok &= redirect(SITE_SETCLIP1, (void*)vpw_setclip);
        ok &= redirect(SITE_SETCLIP2, (void*)vpw_setclip);
        ok &= redirect(SITE_SETCLIP3, (void*)vpw_setclip);
        ok &= tagpu_detour_write(SITE_UNPACK1, UNPACK_NOW, 9);
        ok &= tagpu_detour_write(SITE_UNPACK2, UNPACK_NOW, 9);
        ok &= tagpu_detour_write(SITE_UNPACK3, UNPACK_NOW, 9);
    }
    s_installed = ok;
    if (!ok)             flog("vpwide: PARTIAL — see above");
    else if (s_widenArmed) flog("vpwide: ARMED (mouse->world 0x498DA0, surface "
                                "clip 0x4C6B10 x3, wndproc lParam sign-extend "
                                "x3); the rect only widens while zoom < 1");
    else                 flog("vpwide: mouse->world repair only (0x498DA0) — "
                              "the viewport rect is never widened");
}
