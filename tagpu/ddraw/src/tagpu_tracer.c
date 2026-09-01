/* tagpu_tracer.c — G4 in-process tracer for TA's unit-draw path.

   Detours (classic 5-byte E9 JMP at the function entry to a runtime-generated naked
   stub) are installed on two G3-verified sites in the pristine TotalA.exe (ImageBase
   0x400000, md5 8e74a1dffa1f5988624c52048f5b20cd):

     * DrawUnit  0x45AC20  — one call per drawn unit; args (OFFSCREEN* ctx, UnitStruct*).
     * blit      0x459200  — composite sprite -> screen + z-merge; carries the projected
                             origin (eye) args and, via its Object3do arg, the unit.

   Each stub does:  pushad -> push cdecl args (retaddr + captured esp/reg values) ->
   call a C logger -> add esp,<n*4> -> popad -> <stolen prologue bytes> -> jmp back.
   Because pushad/popad fully bracket the C call, the stolen prologue re-executes with
   esp == the function's entry esp, exactly as the untouched function would see it, then
   control resumes just past the stolen bytes.

   The C loggers only write to a fixed in-memory ring + a few counters (no CRT, no file
   I/O). tagpu_tracer_flush() (called from the overlay per-frame path) does all the file
   I/O and all engine-state reads. Everything over sim state is read-only and guarded.

   ---------------------------------------------------------------------------
   TARGET PROLOGUE DISASSEMBLY (objdump -D -b binary -m i386 -M intel
   --adjust-vma=0x400C00, .text VA->file = VA-0x400C00), proving relocatability:

   DrawUnit 0x45AC20  (file off 0x5A020, entry bytes 51 53 57 8B 7C 24 14 33 DB ...):
     45ac20: 51              push ecx
     45ac21: 53              push ebx
     45ac22: 57              push edi
     45ac23: 8b 7c 24 14     mov  edi,[esp+0x14]      ; edi = arg2 = UnitStruct*
     45ac27: 33 db           xor  ebx,ebx             ; <- resume here
   Clean instruction boundary at +7 (0x45AC27). Stolen = 7 bytes  51 53 57 8B 7C 24 14.
   All 4 stolen instructions are position-independent (push reg; esp-relative mov) — no
   eip-relative material — so they relocate verbatim into the stub. The E9 JMP is 5 bytes;
   the 2 leftover stolen slots (0x45AC25/26) are NOP-filled (never executed: entry jumps
   to the stub, and the stub resumes at 0x45AC27). Incoming EFLAGS are dead at the resume
   point (0x45AC27 does xor then cmp before the first jne), so EFLAGS need not be saved.

   blit 0x459200  (file off 0x58600, entry bytes 83 EC 20 53 55 8B 6C 24 30 ...):
     459200: 83 ec 20        sub  esp,0x20
     459203: 53              push ebx
     459204: 55              push ebp
     459205: 8b 6c 24 30     mov  ebp,[esp+0x30]      ; <- resume here; ebp = arg2 = Object3do*
   Clean instruction boundary at +5 (0x459205). Stolen = 5 bytes  83 EC 20 53 55 (exact
   E9 fit, no NOP pad). sub-imm + two pushes are position-independent. Incoming EFLAGS are
   dead at 0x459205 (mov/mov/mov then test before the first je).

   ---------------------------------------------------------------------------
   0x459200 ARGUMENT LAYOUT (from the disassembly of its body; esp measured at the
   instruction, relative to entry esp E where [E]=retaddr):
     ecx           = this  (the composite-buffer owner, == *(main+0x1437B))
     [E+0x04] a1   = OFFSCREEN* ctx
     [E+0x08] a2   = Object3do*   (0x459205 loads it into ebp; ebp+0x0C = ThisUnit ptr,
                                   whose +0x6a/+0x6e/+0x72 are the unit world pos)
     [E+0x0C] a3   = eyeX (world units, fixed-pt)   ; 0x459239 sub ebx(=unit+0x6a),[a3]
     [E+0x10] a4   = mid/reference value (stored to a local; role not load-bearing here)
     [E+0x14] a5   = eyeY (world units, fixed-pt)   ; 0x45924c sub eax(=unit+0x72),[a5]
   Evidence trail in the body:
     459218 mov eax,[esp+0x3c]  (= a3)   459233 mov ebx,[eax+0x6a] (eax=[ebp+0xc]=unit)
     459239 sub ebx,[esp+0x3c]  -> screenX numerator = unit.x - eyeX
     45924c sub eax,edx (edx=a5) -> screenY numerator = unit.z - eyeY
     459274/459277 add 0x20 to each; 459257/45925c re-read the fixed-pt words; the engine's
     final screenX = ((unit.x-eyeX)>>16)+0x80, screenY = ((unit.z-eyeY)>>16) - alt/2 + 0x20
   The projected sx/sy are computed *inside* the function (not passed), so the logger
   reconstructs them from the unit world pos + eye args using the field-notes formula
   (sx = wx - eyeX + 128, sy = wy - alt/2 - eyeY + 32), storing the result in the event.
   ---------------------------------------------------------------------------

   DrawUnit call-site attribution (return addresses, verified in this build):
     call 0x45AC20 at 0x469A00 -> return 0x469A05   ("site A")
     call 0x45AC20 at 0x469BA3 -> return 0x469BA8   ("site B")
   Both push (&offscreen, unit); the tracer buckets DrawUnit calls by return address so
   the main session can see which sweep draws a given unit and whether both fire for one.
*/

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tagpu_tracer.h"

/* ---- engine layout (all G3/G4-verified, see frame-composition.md / this file's header) */
#define TA_MAINPP        0x00511DE8u  /* TAdynmemStruct**            */
#define OFF_BEGIN        0x14357      /* UnitStruct* begin           */
#define OFF_END          0x1435B      /* UnitStruct* end             */
#define OFF_COMPOWNER    0x1437B      /* void* shared composite owner (this in 0x458810) */
#define UNIT_STRIDE      0x118
#define U_STATE          0x110        /* alive 0x10000000, skip 0x4000 */
#define U_XPOS           0x6C         /* short world X               */
#define U_ZPOS           0x70         /* short altitude              */
#define U_YPOS           0x74         /* short world Y (map depth)   */
#define U_OBJ3DO         0x9E         /* Object3doStruct*            */
#define O3_CACHEFRAME    0x10         /* GAFFrame* composite (candidate A) */

#define EYE_X_OFF        0x1431F      /* int scroll origin X (for sx/sy reconstruct) */
#define EYE_Y_OFF        0x14323      /* int scroll origin Y                        */

/* DrawUnit call-site return addresses */
#define RET_SITE_A       0x00469A05u
#define RET_SITE_B       0x00469BA8u

/* ---- event ring ---------------------------------------------------------------------- */
#define RING            4096u
#define RING_MASK       (RING - 1u)
#define RAW_DUMP        300u          /* first N raw events dumped once */

#define SITE_DRAWUNIT   1
#define SITE_BLIT       2

typedef struct {
    unsigned int frame;    /* frame-ish counter stamped from the last flush */
    unsigned int retaddr;  /* call-site return address                      */
    unsigned int unit;     /* UnitStruct*                                   */
    unsigned int arg0;     /* DrawUnit: OFFSCREEN* ctx ; blit: Object3do*   */
    int          sx, sy;   /* blit: reconstructed screen coords ; DrawUnit: 0 */
    unsigned char site;
} tr_event;

static volatile tr_event   g_ring[RING];
static volatile unsigned   g_ring_head = 0;   /* monotonic write index */
static unsigned            g_curframe   = 0;   /* stamped into events; updated by flush */
static int                 g_armed      = 0;

/* per-frame distinct-unit tracking (cheap hashed bitmap, reset each frame in flush) */
#define DBITS_SLOTS     4096u
#define DBITS_MASK      (DBITS_SLOTS - 1u)
static unsigned char       g_dbits[DBITS_SLOTS / 8];
static unsigned            g_distinct = 0;     /* distinct units in the current frame */

/* window aggregates (reset each summary) */
static unsigned            g_win_du_a = 0, g_win_du_b = 0, g_win_du_other = 0, g_win_blit = 0;
static unsigned            g_win_events = 0;
static unsigned            g_umin = 0, g_umax = 0;

/* summary bookkeeping */
static unsigned            g_last_summary = 0;
static int                 g_dumped = 0;
static unsigned            g_prev_compA = 0, g_prev_compB = 0;
static int                 g_have_prev  = 0;

/* ---- pointer guard: sim pointers live well above 0x600000 -------------------------- */
static int ptr_ok(unsigned int p) { return p > 0x00600000u && p < 0x7FFF0000u; }

/* ---- hot path: record one event (memory writes only; no CRT) ----------------------- */
static void tr_record(unsigned char site, unsigned int ret, unsigned int unit,
                      unsigned int arg0, int sx, int sy)
{
    unsigned idx = g_ring_head++ & RING_MASK;
    volatile tr_event* e = &g_ring[idx];
    e->frame   = g_curframe;
    e->retaddr = ret;
    e->unit    = unit;
    e->arg0    = arg0;
    e->sx      = sx;
    e->sy      = sy;
    e->site    = site;

    g_win_events++;
    if (unit) {
        if (!g_umin || unit < g_umin) g_umin = unit;
        if (unit > g_umax) g_umax = unit;
        /* distinct-per-frame via hashed bitmap */
        unsigned h = (unit >> 4) & DBITS_MASK;
        unsigned char mask = (unsigned char)(1u << (h & 7u));
        if (!(g_dbits[h >> 3] & mask)) { g_dbits[h >> 3] |= mask; g_distinct++; }
    }
}

/* ---- the C loggers (cdecl; called from the stubs) ---------------------------------- */
/* DrawUnit(ctx, unit): stub captures retaddr, unit, offscreen ctx. */
void __cdecl tr_log_drawunit(unsigned int retaddr, unsigned int unit, unsigned int offscreen)
{
    if (retaddr == RET_SITE_A)      g_win_du_a++;
    else if (retaddr == RET_SITE_B) g_win_du_b++;
    else                            g_win_du_other++;
    tr_record(SITE_DRAWUNIT, retaddr, unit, offscreen, 0, 0);
}

/* blit 0x459200(this, ctx, obj, a3=eyeX<<16, a4=mid, a5=eyeY<<16): reconstruct unit + sx/sy.
   The captured a3/a5 args are the FUNCTION'S fixed-point eye (they pair with the unit's
   full-precision pos at +0x6a inside the body). For the "unit N at (sx,sy)" proof we instead
   use the already-validated overlay projection: short pos (unit+0x6C/0x70/0x74) minus the
   screen-origin eye (main+0x1431F/0x14323). Capturing all 7 args validates the arg layout;
   we don't rely on a3/a4/a5 for the math (different scale). */
void __cdecl tr_log_blit(unsigned int retaddr, unsigned int this_owner, unsigned int ctx,
                         unsigned int obj, unsigned int a3, unsigned int a4, unsigned int a5)
{
    unsigned int unit = 0;
    int sx = 0, sy = 0;
    (void)this_owner; (void)ctx; (void)a3; (void)a4; (void)a5;
    g_win_blit++;
    if (ptr_ok(obj)) {
        unit = *(volatile unsigned int*)(obj + 0x0C);      /* Object3do+0x0C = ThisUnit */
        if (ptr_ok(unit)) {
            unsigned int main = *(volatile unsigned int*)TA_MAINPP;
            if (ptr_ok(main)) {
                int eyeX = *(volatile int*)(main + EYE_X_OFF);
                int eyeY = *(volatile int*)(main + EYE_Y_OFF);
                short wx = *(volatile short*)(unit + U_XPOS);
                short wz = *(volatile short*)(unit + U_ZPOS);
                short wy = *(volatile short*)(unit + U_YPOS);
                sx = (int)wx - eyeX + 128;
                sy = (int)wy - (wz / 2) - eyeY + 32;
            }
        } else {
            unit = 0;
        }
    }
    tr_record(SITE_BLIT, retaddr, unit, obj, sx, sy);
}

/* ---- runtime stub generation ------------------------------------------------------- */
static unsigned char* g_stub_pool = 0;
static unsigned       g_stub_used = 0;

/* Emit one detour stub into the executable pool and return its address.
   push_offs[] gives the imm8 for each `FF 74 24 <off>` (push dword [esp+off]) in order;
   they encode the cdecl args (right-to-left) as read off the post-pushad stack. */
static unsigned char* emit_stub(unsigned int logger,
                                const unsigned char* push_offs, int n_push,
                                const unsigned char* stolen, int stolen_len,
                                unsigned int resume)
{
    unsigned char* s = g_stub_pool + g_stub_used;
    unsigned char* p = s;
    int i;
    int32_t rel;

    *p++ = 0x60;                                   /* pushad */
    for (i = 0; i < n_push; i++) {                 /* push dword [esp+off] */
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = push_offs[i];
    }
    *p++ = 0xE8;                                    /* call rel32 (logger) */
    rel = (int32_t)(logger - ((unsigned int)(p) + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = (unsigned char)(n_push * 4);  /* add esp, n*4 */
    *p++ = 0x61;                                    /* popad */
    memcpy(p, stolen, (size_t)stolen_len); p += stolen_len;        /* stolen prologue */
    *p++ = 0xE9;                                    /* jmp rel32 (resume) */
    rel = (int32_t)(resume - ((unsigned int)(p) + 4));
    memcpy(p, &rel, 4); p += 4;

    g_stub_used = (unsigned)(p - g_stub_pool);
    return s;
}

/* Install a detour at target_va iff its current bytes match `stolen` (byte-match guard,
   like tagpu_patches.c). Writes E9 rel32 to the stub + NOP-fills any leftover slots. */
static int install_hook(unsigned int target_va, const unsigned char* stolen, int stolen_len,
                        const unsigned char* push_offs, int n_push, unsigned int logger)
{
    unsigned char* t = (unsigned char*)target_va;
    unsigned int resume = target_va + (unsigned)stolen_len;
    unsigned char* stub;
    DWORD old;
    int32_t rel;

    if (memcmp(t, stolen, (size_t)stolen_len) != 0) return 0;   /* wrong build — skip */

    stub = emit_stub(logger, push_offs, n_push, stolen, stolen_len, resume);

    if (!VirtualProtect(t, (SIZE_T)stolen_len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t[0] = 0xE9;
    rel = (int32_t)((unsigned int)stub - (target_va + 5));
    memcpy(t + 1, &rel, 4);
    { int k; for (k = 5; k < stolen_len; k++) t[k] = 0x90; }     /* NOP-fill leftovers */
    VirtualProtect(t, (SIZE_T)stolen_len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T)stolen_len);
    return 1;
}

static void tlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* ---- init: arm only if the trigger file exists ------------------------------------- */
void tagpu_tracer_init(void)
{
    /* DrawUnit 0x45AC20: stolen 7 = 51 53 57 8B 7C 24 14 ; resume 0x45AC27.
       Post-pushad stack (esp=E): [E+0x20]=retaddr [E+0x24]=offscreen [E+0x28]=unit.
       cdecl tr_log_drawunit(retaddr, unit, offscreen): push offscreen,unit,retaddr. */
    static const unsigned char DU_STOLEN[7]   = { 0x51, 0x53, 0x57, 0x8B, 0x7C, 0x24, 0x14 };
    static const unsigned char DU_PUSHES[3]   = { 0x24, 0x2C, 0x28 };
    /*   push [esp+0x24]=offscreen(esp=E) ; push [esp+0x2C]=unit(esp=E-4) ; push [esp+0x28]=ret(esp=E-8) */

    /* blit 0x459200: stolen 5 = 83 EC 20 53 55 ; resume 0x459205.
       Post-pushad stack (esp=E): ECX(this) @[E+0x18]; [E+0x20]=ret [E+0x24]=a1(ctx)
       [E+0x28]=a2(obj) [E+0x2C]=a3(eyeX) [E+0x30]=a4(mid) [E+0x34]=a5(eyeY).
       cdecl tr_log_blit(retaddr,this,ctx,obj,eyeX,mid,eyeY): push a5,a4,a3,obj,ctx,this,ret. */
    static const unsigned char BL_STOLEN[5]   = { 0x83, 0xEC, 0x20, 0x53, 0x55 };
    static const unsigned char BL_PUSHES[7]   = { 0x34, 0x34, 0x34, 0x34, 0x34, 0x2C, 0x38 };
    /* first 5 pushes read a5,a4,a3,obj,ctx: each source is 4 higher and esp is 4 lower per
       push, so the [esp+0x34] displacement is invariant. Then this @0x2C, retaddr @0x38. */

    int du_ok, bl_ok, suppress_owns_du;
    char b[192];

    if (GetFileAttributesA("tagpu_tracer.on") == INVALID_FILE_ATTRIBUTES) return;  /* disarmed */

    /* Coexistence with the G5 suppressor (tagpu_suppress.c): when "tagpu_suppress.on"
       is present the suppressor OWNS DrawUnit 0x45AC20 (it installs its own E9 over the
       same 7 stolen bytes). We must NOT double-detour those bytes, so skip our DrawUnit
       hook in that case. The blit hook 0x459200 is independent and always installs. */
    suppress_owns_du = (GetFileAttributesA("tagpu_suppress.on") != INVALID_FILE_ATTRIBUTES);

    g_stub_pool = (unsigned char*)VirtualAlloc(NULL, 0x1000,
                     MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stub_pool) { tlog("tracer: VirtualAlloc(stub pool) FAILED — not armed"); return; }

    du_ok = suppress_owns_du ? 0
          : install_hook(0x0045AC20u, DU_STOLEN, 7, DU_PUSHES, 3,
                         (unsigned int)&tr_log_drawunit);
    bl_ok = install_hook(0x00459200u, BL_STOLEN, 5, BL_PUSHES, 7,
                         (unsigned int)&tr_log_blit);

    g_armed = (du_ok || bl_ok);
    _snprintf(b, sizeof b,
        "tracer: armed (tagpu_tracer.on present). DrawUnit@0x45AC20 hook=%s  blit@0x459200 hook=%s  stub_pool=%p",
        suppress_owns_du ? "SKIP(suppressor owns 0x45AC20)"
                         : (du_ok ? "OK" : "SKIP(byte-mismatch)"),
        bl_ok ? "OK" : "SKIP(byte-mismatch)", (void*)g_stub_pool);
    tlog(b);
}

/* ---- flush: all file I/O + engine reads happen here (render thread, per frame) ------ */
static void dump_raw_events(void)
{
    FILE* f = fopen("tagpu.log", "a");
    unsigned n, i;
    if (!f) return;
    n = g_ring_head; if (n > RAW_DUMP) n = RAW_DUMP;
    fprintf(f, "TR_RAW begin count=%u (format: idx f site ret unit arg0 sx sy)\n", n);
    for (i = 0; i < n; i++) {
        volatile tr_event* e = &g_ring[i & RING_MASK];
        fprintf(f, "TR_EV %u f=%u site=%s ret=%08X unit=%08X arg0=%08X sx=%d sy=%d\n",
                i, e->frame, e->site == SITE_DRAWUNIT ? "DU" : "BL",
                e->retaddr, e->unit, e->arg0, e->sx, e->sy);
    }
    fprintf(f, "TR_RAW end\n");
    fclose(f);
}

/* Sample the two composite-buffer candidates from live engine state (read-only, guarded):
   A = first-alive-unit's Object3do+0x10 ; B = *(*(main+0x1437B)+0x10) shared scratch. */
static void sample_composites(unsigned int* pA, unsigned int* pB,
                              unsigned int* pUnitA, unsigned int* pObjA)
{
    unsigned int main = *(volatile unsigned int*)TA_MAINPP;
    *pA = *pB = *pUnitA = *pObjA = 0;
    if (!ptr_ok(main)) return;

    /* candidate A: walk to first alive unit, read its Object3do+0x10 */
    {
        unsigned int beg = *(volatile unsigned int*)(main + OFF_BEGIN);
        unsigned int end = *(volatile unsigned int*)(main + OFF_END);
        if (ptr_ok(beg) && ptr_ok(end) && end > beg &&
            (end - beg) < UNIT_STRIDE * 20000u) {
            unsigned int u;
            for (u = beg + UNIT_STRIDE; u < end; u += UNIT_STRIDE) {
                unsigned int st = *(volatile unsigned int*)(u + U_STATE);
                if (!(st & 0x10000000u) || (st & 0x4000u)) continue;
                {
                    unsigned int o3 = *(volatile unsigned int*)(u + U_OBJ3DO);
                    if (ptr_ok(o3)) {
                        *pUnitA = u;
                        *pObjA  = o3;
                        *pA = *(volatile unsigned int*)(o3 + O3_CACHEFRAME);
                    }
                }
                break;
            }
        }
    }
    /* candidate B: shared composite owner's +0x10 */
    {
        unsigned int owner = *(volatile unsigned int*)(main + OFF_COMPOWNER);
        if (ptr_ok(owner))
            *pB = *(volatile unsigned int*)(owner + O3_CACHEFRAME);
    }
}

void tagpu_tracer_flush(unsigned int frame_counter)
{
    unsigned int frame_distinct;
    if (!g_armed) return;

    /* one-time raw dump once we have a useful sample */
    if (!g_dumped && g_ring_head >= RAW_DUMP) { dump_raw_events(); g_dumped = 1; }

    /* snapshot the frame just composed, then reset per-frame distinct tracking */
    frame_distinct = g_distinct;
    g_distinct = 0;
    memset(g_dbits, 0, sizeof g_dbits);
    g_curframe = frame_counter;    /* stamp events of the frame about to be composed */

    /* summary every ~60 frames */
    if (frame_counter - g_last_summary >= 60) {
        unsigned int cA, cB, uA, oA;
        char b[512];
        sample_composites(&cA, &cB, &uA, &oA);
        _snprintf(b, sizeof b,
            "TR_SUM f=%u win_events=%u DU=%u(A@469A05=%u B@469BA8=%u other=%u) BL=%u "
            "distinct=%u umin=%08X umax=%08X "
            "compA=%08X[unit=%08X obj=%08X] %s compB=%08X %s",
            frame_counter, g_win_events,
            g_win_du_a + g_win_du_b + g_win_du_other, g_win_du_a, g_win_du_b, g_win_du_other,
            g_win_blit, frame_distinct, g_umin, g_umax,
            cA, uA, oA, (g_have_prev && cA == g_prev_compA) ? "same" : "CHG",
            cB,         (g_have_prev && cB == g_prev_compB) ? "same" : "CHG");
        tlog(b);

        g_prev_compA = cA; g_prev_compB = cB; g_have_prev = 1;
        g_last_summary = frame_counter;
        g_win_du_a = g_win_du_b = g_win_du_other = g_win_blit = 0;
        g_win_events = 0;
        g_umin = g_umax = 0;
    }

    /* note: DrawUnit/blit call counts live in the same window counters; the summary line
       reports both. g_win_blit is incremented in tr_log_blit's tr_record via SITE_BLIT. */
}
