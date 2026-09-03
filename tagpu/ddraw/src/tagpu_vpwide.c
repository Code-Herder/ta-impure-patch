/* tagpu_vpwide.c — widen the engine's addressable viewport at zoom < 1.
   See tagpu_vpwide.h for what the rect is, who reads it and why the four
   kinds of reader need four different things. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_vpwide.h"
#include "tagpu_detour.h"

#define TA_MAINPP    0x00511DE8u

/* the six ints at main+0x37E27 */
#define OFF_VP_L     0x37E27
#define OFF_VP_T     0x37E2B
#define OFF_VP_R     0x37E2F
#define OFF_VP_B     0x37E33
#define OFF_VIEW_W   0x37E37
#define OFF_VIEW_H   0x37E3B

/* the origin `0x497F40` hardcodes when it builds the rect: L = 0x80, T = 0x20,
   R = screenW - 1, B = screenH - 33, then W = R-L+1 and H = B-T+1. Every other
   consumer of the rect projects with the same two constants, which is what
   makes the true rect derivable from W and H alone while we own L/T/R/B. */
#define VP_TRUE_L    0x80
#define VP_TRUE_T    0x20

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

static int clampi(int v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static int  s_installed;        /* the four redirects went in                */
static int  s_verified;         /* the rect matched what 0x497F40 builds     */
static int  s_saidUnverified;   /* the diagnostic is one-shot                */
static int  s_wide;             /* we are currently writing the rect         */

/* Published for the message thread's ring test. Five aligned 32-bit slots, the
   same discipline tagpu_zoom uses: a reader can at worst see the previous
   frame's rect, which costs one click's ring decision a frame of lag and is
   not worth a lock on the input path. `s_pubLive` is set last and cleared
   first, so "live" never advertises a rect that was not written. */
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
    ((PFN_SETCLIP)VA_SETCLIP)(self, l, t, r, b);
}

/* ---- the origin fix -------------------------------------------------------

   0x498DA0 turns the mouse position into the world point, the map cell and the
   feature under the cursor, and it is the ONE reader that uses L and T as the
   screen->world ORIGIN rather than as bounds:

       world = eye + clamp(pos, L, R) - L

   With the rect widened that reads the origin off by (0x80 - L), which would
   offset every ground order, build placement and feature hover by the same
   amount. This redoes the same computation with the TRUE origin and the WIDE
   clamp — the only two things that differ — and is a straight pass-through
   whenever we are not writing the rect, so zoom >= 1 is byte-identical.

   Redoing it rather than biasing the eye across the call is deliberate: the
   engine runs this on the game thread while our overlay reads the eye on
   cnc-ddraw's render thread, so a transient bias would be a visible one-frame
   jump every few thousand frames. */
static void __stdcall vpw_mouse_world(int* pos)
{
    char* ta;
    unsigned char* fl;
    int x, y, wx, wy, gx, gy;

    ta = *(char**)TA_MAINPP;
    if (!s_wide || !pos || !ptr_ok(ta)) { ((PFN_MOUSEWORLD)VA_MOUSEWORLD)(pos); return; }

    x = pos[0];
    y = pos[1];
    fl = (unsigned char*)(ta + OFF_MOUSEFL);

    if (((PFN_INRECT)VA_INRECT)((const int*)(ta + OFF_MM_CLICK), x, y) && !(*fl & 8)) {
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
        wx = *(const int*)(ta + OFF_EYEX)
           + clampi(x, *(const int*)(ta + OFF_VP_L), *(const int*)(ta + OFF_VP_R))
           - VP_TRUE_L;
        wy = *(const int*)(ta + OFF_EYEY)
           + clampi(y, *(const int*)(ta + OFF_VP_T), *(const int*)(ta + OFF_VP_B))
           - VP_TRUE_T;
        *fl = (unsigned char)((*fl & ~3)
            | (((PFN_INRECT)VA_INRECT)((const int*)(ta + OFF_VP_L), x, y) ? 2 : 0));
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
    *L = *T = *W = *H = 0;              /* a zero rect fails every caller closed */
    if (!ptr_ok(ta)) return;
    *W = *(const int*)(ta + OFF_VIEW_W);
    *H = *(const int*)(ta + OFF_VIEW_H);
    if (s_verified) {
        *L = VP_TRUE_L;
        *T = VP_TRUE_T;
    } else {
        *L = *(const int*)(ta + OFF_VP_L);
        *T = *(const int*)(ta + OFF_VP_T);
    }
}

int tagpu_vpwide_addressable(int* L, int* T, int* W, int* H)
{
    if (!s_pubLive) return 0;
    *L = (int)s_pubL; *T = (int)s_pubT; *W = (int)s_pubW; *H = (int)s_pubH;
    return *W > 0 && *H > 0;
}

/* `s_wide` is what the game thread's 0x498DA0 stub reads to decide whether the
   rect is ours. It is raised BEFORE the widening stores and lowered AFTER the
   restoring ones, so the only state the two threads can ever disagree about is
   "ours, but still holding the true rect" — and there the stub's true origin
   and its clamp against the true L/R reproduce the engine's own answer
   exactly. The other order would hand it a wide rect with a true origin. */
static void restore(char* ta, int w, int h)
{
    if (!s_wide) return;
    if (w < 64 || h < 64 || w > 8192 || h > 8192) return;   /* not with garbage */
    s_pubLive = 0;                       /* stop advertising it first */
    *(int*)(ta + OFF_VP_L) = VP_TRUE_L;
    *(int*)(ta + OFF_VP_T) = VP_TRUE_T;
    *(int*)(ta + OFF_VP_R) = VP_TRUE_L + w - 1;
    *(int*)(ta + OFF_VP_B) = VP_TRUE_T + h - 1;
    s_wide = 0;
    flog("vpwide: viewport rect restored to 1x");
}

void tagpu_vpwide_frame(float z)
{
    char* ta;
    int w, h, aL, aT, aR, aB;
    float cx, cy;

    if (!s_installed) return;            /* disarmed: not one byte is written */
    ta = *(char**)TA_MAINPP;
    if (!ptr_ok(ta)) return;
    w = *(const int*)(ta + OFF_VIEW_W);
    h = *(const int*)(ta + OFF_VIEW_H);
    if (w < 64 || h < 64 || w > 8192 || h > 8192) { restore(ta, w, h); return; }

    /* Verify once, on a frame we do not own, that the rect really is what
       0x497F40 builds — everything below derives the true rect from W and H on
       that basis, so a build or a resolution that disagrees must widen nothing
       rather than half of it. */
    if (!s_verified) {
        if (s_wide) return;              /* cannot verify a rect we wrote */
        if (*(const int*)(ta + OFF_VP_L) == VP_TRUE_L &&
            *(const int*)(ta + OFF_VP_T) == VP_TRUE_T &&
            *(const int*)(ta + OFF_VP_R) == VP_TRUE_L + w - 1 &&
            *(const int*)(ta + OFF_VP_B) == VP_TRUE_T + h - 1) {
            char b[128];
            s_verified = 1;
            _snprintf(b, sizeof b, "vpwide: true viewport rect verified (%d,%d %dx%d)",
                      VP_TRUE_L, VP_TRUE_T, w, h);
            flog(b);
        } else {
            if (!s_saidUnverified) {
                char b[160];
                s_saidUnverified = 1;
                _snprintf(b, sizeof b,
                    "vpwide: NOT widening — rect (%d,%d %d,%d) is not the "
                    "(%d,%d %dx%d) 0x497F40 builds",
                    *(const int*)(ta + OFF_VP_L), *(const int*)(ta + OFF_VP_T),
                    *(const int*)(ta + OFF_VP_R), *(const int*)(ta + OFF_VP_B),
                    VP_TRUE_L, VP_TRUE_T, w, h);
                flog(b);
            }
            return;
        }
    }

    if (!(z > 0.05f && z < 1.0f)) { restore(ta, w, h); return; }

    /* The addressable rect is not "the viewport, bigger" — it is exactly the
       range tagpu_zoom's transform produces, computed with the same formula
       about the same centre so the two cannot disagree at the edges, plus one
       pixel of slack each way against the rounding. */
    cx = (float)VP_TRUE_L + (float)w * 0.5f;
    cy = (float)VP_TRUE_T + (float)h * 0.5f;
    aL = iround(((float)VP_TRUE_L             - cx) / z + cx) - 1;
    aR = iround(((float)(VP_TRUE_L + w - 1)   - cx) / z + cx) + 1;
    aT = iround(((float)VP_TRUE_T             - cy) / z + cy) - 1;
    aB = iround(((float)(VP_TRUE_T + h - 1)   - cy) / z + cy) + 1;
    if (aR - aL + 1 > 32768 || aB - aT + 1 > 32768) { restore(ta, w, h); return; }

    /* Compare against the FIELD, not against what we last wrote: at a steady
       zoom that is four loads and no stores, and it is also what notices the
       game-screen callback having put the true rect back underneath us. */
    if (!s_wide ||
        *(const int*)(ta + OFF_VP_L) != aL || *(const int*)(ta + OFF_VP_T) != aT ||
        *(const int*)(ta + OFF_VP_R) != aR || *(const int*)(ta + OFF_VP_B) != aB) {
        s_pubLive = 0;
        s_wide = 1;
        *(int*)(ta + OFF_VP_L) = aL;
        *(int*)(ta + OFF_VP_T) = aT;
        *(int*)(ta + OFF_VP_R) = aR;
        *(int*)(ta + OFF_VP_B) = aB;
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

    if (GetFileAttributesA("tagpu_vpwide.on") == INVALID_FILE_ATTRIBUTES) return;

    /* all-or-nothing: every byte is checked before any of them is written */
    if (!site_is(SITE_SETCLIP1, VA_SETCLIP) ||
        !site_is(SITE_SETCLIP2, VA_SETCLIP) ||
        !site_is(SITE_SETCLIP3, VA_SETCLIP) ||
        !site_is(SITE_MOUSEWORLD, VA_MOUSEWORLD) ||
        !bytes_are(SITE_UNPACK1, UNPACK_WAS) ||
        !bytes_are(SITE_UNPACK2, UNPACK_WAS) ||
        !bytes_are(SITE_UNPACK3, UNPACK_WAS)) {
        flog("vpwide: NOT armed — engine bytes differ at one of "
             "0x468D85/0x46964F/0x469F95/0x499221/0x4B5E5F/0x4B5EC0/0x4B5F0C");
        return;
    }
    ok  = redirect(SITE_SETCLIP1, (void*)vpw_setclip);
    ok &= redirect(SITE_SETCLIP2, (void*)vpw_setclip);
    ok &= redirect(SITE_SETCLIP3, (void*)vpw_setclip);
    ok &= redirect(SITE_MOUSEWORLD, (void*)vpw_mouse_world);
    ok &= tagpu_detour_write(SITE_UNPACK1, UNPACK_NOW, 9);
    ok &= tagpu_detour_write(SITE_UNPACK2, UNPACK_NOW, 9);
    ok &= tagpu_detour_write(SITE_UNPACK3, UNPACK_NOW, 9);
    s_installed = ok;
    flog(ok ? "vpwide: ARMED (surface clip 0x4C6B10 x3, mouse->world 0x498DA0, "
              "wndproc lParam sign-extend x3); the rect only widens while zoom < 1"
            : "vpwide: PARTIAL — see above");
}
