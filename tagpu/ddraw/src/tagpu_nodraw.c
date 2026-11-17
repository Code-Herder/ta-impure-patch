/* tagpu_nodraw.c -- NO DRAW, a test's GDI pacing, and THE POSE CHECK (inc/tagpu_nodraw.h).

   On an in-play frame of a test launch with tagpu_nodraw.on, the turbo live and the frame's batch
   the turbo's (THE MARK, 0x49692D), the replay's frame visit at 0x4969CD runs tagpu_nodraw_frame in
   DrawGameScreen's place (THE GATE): the draw's commands, DrawUnit's repose for the units the draw
   would reach (THE REPOSE), the trigger family and the match latch where the flip would have run
   them, then ImpAI's digests, the map's features and the roster where DrawGameScreen's `after`
   would have. The same repose is what a build that draws through Vulkan alone needs once the
   software rasteriser stops running, so it is held to the replay harness like any simulation code.
   THE SHADOW (tagpu_ndshadow.on) is its proof, in one process: on every drawn in-play frame it runs
   the repose's selection without calling DrawUnit, records the units the draw itself passes to
   DrawUnit from its two sweeps, and compares the two lists. DrawUnit is the engine's own function,
   so equal lists on every frame mean an undrawn frame reposes exactly what the draw reposes, in the
   same order; the test group nodraw-shadow plays a fight under it and passes on no difference.

   WHAT IT READS AND WRITES, all on the game thread but the install (DllMain):
   - THE MARK, inside the pacing guard at 0x49692D: GameTime main+0x38A47, the batch count
     main+0x38A3B and the lag-scale bit 1 of main+0x38A51;
   - THE GATE, the frame visit's last act: GameTime, the scoreboard's slide 0x51F2D8, bit 7 of
     main+0x37F06 and the hotkey sampler 0x4C1B80 called for SPACE; then THE REPOSE's room: the sort's
     dimensions main+0x1424B/+0x1424F, HotUnits main+0x1435F and its count main+0x14367, the unit
     array main+0x14357..+0x1435B and its slots main+0x14351;
   - THE REPOSE, in DrawGameScreen's place: the eye's y main+0x14323, the feature grid's rows
     main+0x14237, a hot unit's +0x74, +0x110 and +0x9A, and calls of DrawUnit 0x45AC20 that write
     the units' pose cache as the draw's own calls do, with the composite call inert;
   - THE POSE CHECK, before the frame visit: every unit's model +0x9E, its Object3do's piece count,
     +8, +0x18..+0x1D, each piece's origin, +0x26 and posed vertices, and the SweetSpot point
     0x43E0B0 for each piece, which writes only its `out`;
   - THE SHADOW, inside an in-play DrawGameScreen: the selection's reads, from the frame packet's
     observer right after the draw's commands (tagpu_packet_pub.c, before_draw), and each unit the
     draw's sweeps pass to DrawUnit, by their two call sites;
   - THE INSTALL: the bytes the skip rule and the repose rest on, compared (0x4969CB, 0x46A3FD,
     0x494930..0x494993, 0x4C1B80, 0x45AE5C..0x45AE7F, the sort bound 0x469807..0x469825), the
     composite's gate written at 0x45AE73, and with the shadow's lever DrawUnit's two sweep calls
     0x469A00 and 0x469BA3 re-pointed to their recorders: the engine sites this file writes. */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "tagpu_nodraw.h"
#include "tagpu_simreg.h"
#include "tagpu_detour.h"
#include "tagpu_skipstub.h"
#include "tagpu_log.h"
#include "tagpu_regstore.h"
#include "tagpu_puppet.h"
#include "tagpu_recfmt.h"
#include "tagpu_gui.h"
#include "tagpu_menu.h"
#include "tagpu_packet_pub.h"
#include "config.h"

SIM_FEATURE(nodraw, SIM_PUPPET_OWNER, "nodraw-shadow");

#define TA_MAINPP      0x00511DE8u
#define OFF_GAMETIME   0x38A47
#define OFF_DUE_TICKS  0x38A3B      /* u32: scheduler 0x495230 bounds this frame's steps to 0..5 */
#define OFF_PAUSE      0x38A51      /* u16: bit 1 the lag scale, under which the turbo writes no
                                       count (tagpu_replay.c, turbo_due)                         */
#define OFF_OPTIONS    0x37F06      /* u8: bit 7 shows the scoreboard on every frame (0x494935)   */
#define VA_SCOREBOARD_SLIDE 0x0051F2D8u /* i32: the scoreboard's slide; at or below 0 a hidden
                                       board returns before its rank store (0x494986..0x49498E) */
#define VA_HOTKEY      0x004C1B80u  /* stdcall(id) `ret 4`: 1 while the key is down, through
                                       GetAsyncKeyState with bit 0 masked (SPACE's arm 0x4C1C2D,
                                       its `and al,0xFE` at 0x4C1C35)                             */
#define HOTKEY_SPACE   0x20         /* the scoreboard's key (0x494942)                            */
#define OFF_NSLOTS     0x14351      /* u16: the unit array's slots                                */
#define OFF_BEGIN      0x14357      /* the unit array                                             */
#define OFF_LAST       0x1435B      /* its last slot's address, not one past it                   */
#define OFF_GRIDH      0x14237      /* the feature grid's rows                                    */
/* THE DRAW'S UNIT ROWS (nodraw_repose), with edi = main+0x141FB in DrawGameScreen (render.md, *The
   unit sort's append*): LoadMap 0x483D27..0x483D42 is the only writer of the two dimensions */
#define OFF_SORT_CAP   0x1424B      /* i32 [edi+0x50]: slots a row, the view's 16-px columns + 12  */
#define OFF_SORT_ROWS  0x1424F      /* i32 [edi+0x54]: rows, the view's 16-px rows + 32            */
#define OFF_EYE_Y      0x14323      /* i32: the eye's y, world pixels                              */
#define OFF_HOT        0x1435F      /* u16*: HotUnits, the unit slots the cull 0x48BAE0 keeps      */
#define OFF_NHOT       0x14367      /* i32: their count (0x48BC9D)                                 */
#define U_FEET_Z       0x74         /* i16: the high word of the unit's z, the row it is filed by  */
#define U_COB          0x9A
#define U_MODEL        0x9E         /* the model instance (Object3do): the destructor nulls it as it
                                       frees it (0x486D93..0x486DA3)                              */
#define U_STATE        0x110
#define VA_DRAWUNIT    0x0045AC20u  /* DrawUnit(ctx, unit), stdcall `ret 8` (0x45AE7D)            */
#define SITE_COMPOSITE 0x0045AE73u  /* ... its one call of the composite 0x458810                 */
#define VA_COMPOSITE   0x00458810u
#define VA_SORT_BOUND  0x00469807u  /* the append's bound (tagpu_patches.c, fix_sort_buffer_end)  */
#define VA_PIECE_POINT 0x0043E0B0u  /* (unit, out[3], piece) stdcall `ret 0xC` (0x43E177)         */
#define SITE_A         0x00469A00u  /* DrawGameScreen's sweep calls of DrawUnit, each after `push    */
#define SITE_B         0x00469BA3u  /* unit; push ctx` (0x4699FE, 0x469BA1)                        */

typedef void (__stdcall* PFN_DrawUnit)(void* ctx, char* unit);
#define E_DrawUnit ((PFN_DrawUnit)VA_DRAWUNIT)
typedef void (__stdcall* PFN_PiecePoint)(char* unit, int* out, int piece);
#define E_PiecePoint ((PFN_PiecePoint)VA_PIECE_POINT)

/* THE INSTALL's answers, written at attach (tagpu_nodraw_levers, tagpu_nodraw_site_ok and _in,
   tagpu_nodraw_guard) and read on the game thread after */
static struct {
    int      want;                  /* the lever passed its tests, and the sites' bytes match     */
    int      stub;                  /* the composite's gate and the skippable frame stub are in   */
    int      on;                    /* ... and the mark is in: the gate may skip                  */
    unsigned pc_every;              /* THE POSE CHECK's period in GameTime, 0 off                 */
    int      shadow_want;           /* THE SHADOW's lever passed its tests                        */
    int      shadow;                /* ... and its two recorders are in                           */
} s_ndi;
SIM_VAR(s_ndi, SIM_INSTALL);

/* THE MARK, THE ACCOUNT and THE POSE CHECK's next GameTime, the game thread's. The simulation never
   reads any of it: the mark and the shadow's alternation only choose between DrawGameScreen and
   tagpu_nodraw_frame, which write the same simulation state (THE GATE's argument, which THE SHADOW
   checks), and the rest is the level's account and the oracle's schedule. */
static struct {
    int      armed;                 /* this frame's batch was the turbo's (tagpu_nodraw_mark)     */
    unsigned from, due;             /* GameTime and DeltaTime the mark saw                        */
    unsigned skipped, board, drawn; /* the level's account (tagpu_nodraw_level_end)               */
    unsigned g0;                    /* GameTime of the level's first armed frame                  */
    unsigned any;                   /* a frame was armed this level                               */
    unsigned pc_next;               /* THE POSE CHECK's next GameTime due                         */
    unsigned alt, shadowed;         /* THE SHADOW's alternation, and the frames it drew           */
} s_nd;
SIM_VAR(s_nd, SIM_PRIVATE);

/* the frame stub's flag: nonzero runs tagpu_nodraw_frame in DrawGameScreen's place. A byte, so the
   stub's `cmp byte` reads exactly it. Set only by the gate, the frame visit's last act, which the
   stub tests right after, and cleared by tagpu_nodraw_frame before it returns: 0 at every tick
   boundary. */
static volatile unsigned char s_nd_skip;
SIM_VAR(s_nd_skip, SIM_TRANSIENT);
/* nonzero only inside nodraw_drawunit: DrawUnit's composite call at 0x45AE73 returns at once. A
   byte for the gate's `cmp byte` */
static volatile unsigned char s_nd_repose;
SIM_VAR(s_nd_repose, SIM_TRANSIENT);

/* THE REPOSE's rows, the draw's own unit list: ND_ROWS counts and ND_SLOTS slots, made at install
   and rewritten from the start by every undrawn frame before it reads them (nodraw_repose). A view
   whose sort needs more draws (nodraw_room): an 8K view's is 492 x 302 = 148 584 slots. */
#define ND_ROWS        4096u        /* rows; and a row's slots: a view of 64k pixels, past data   */
#define ND_SLOTS       (1u << 18)
static const SimField nd_slot_layout[] = {
    SIM_FIELD_AT(0, sizeof(char*), SIMF_ADDR, SIM_ID_UNIT_SLOT),
    SIM_FIELD_END
};
static char** s_nd_slot;
SIM_BLOCK(s_nd_slot, SIM_TRANSIENT, SIM_ID_NONE, ND_SLOTS, nd_slot_layout);
static unsigned* s_nd_count;
SIM_BLOCK(s_nd_count, SIM_TRANSIENT, SIM_ID_NONE, ND_ROWS, NULL);

/* THE SHADOW's two lists of one drawn frame, each entry a unit slot << 1 | the site (0 A, 1 B), and
   its account: an oracle the simulation never reads. Game thread. */
static unsigned* s_sh_sel;          /* the selection's, made at the draw's top                    */
SIM_BLOCK(s_sh_sel, SIM_PRIVATE, SIM_ID_NONE, ND_SLOTS, NULL);
static unsigned* s_sh_drw;          /* the draw's own, appended by the recorders                  */
SIM_BLOCK(s_sh_drw, SIM_PRIVATE, SIM_ID_NONE, ND_SLOTS, NULL);
static struct {
    int      open;                  /* the selection is made and the draw's list is being kept    */
    int      over;                  /* the draw passed more than ND_SLOTS units                   */
    unsigned nsel, ndrw;            /* the two lists' lengths                                     */
    unsigned g;                     /* the frame's GameTime                                       */
    unsigned compared, differed, unchecked, noted;      /* the level's account, with the units    */
    unsigned long long units;       /* the draw passed in the frames compared: lists that are     */
                                    /* equal because both are empty prove nothing                 */
    unsigned g0, any;
} s_sh;
SIM_VAR(s_sh, SIM_PRIVATE);
#define SH_NOTES 8                  /* differences logged a level                                 */
#define SH_EVERY 600                /* frames compared between two progress lines, so a process
                                       stopped before its level ends still says what it checked  */

static char* ta_main(void) { return *(char**)(size_t)TA_MAINPP; }

/* ---- the levers ---------------------------------------------------------------------------- */

/* THE POSE CHECK's period: tagpu_posecheck.on's content, 300 when it is empty; 0 when the lever is
   absent or refused */
static unsigned posecheck_lever(void)
{
    char b[16] = {0}, *end;
    unsigned long every;
    if (tagpu_sim_lever_read("tagpu_posecheck.on", b, sizeof b) < 0) return 0;
    every = strtoul(b, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (end == b + strspn(b, " \t\r\n") && !*end) every = 300;
    else if (*end || every < 1 || every > 1000000) every = 0;
    if (!tagpu_regstore_active()) {
        tagpu_logf("posecheck: tagpu_posecheck.on refused: not a test launch (tagpu_regstore.h)");
        return 0;
    }
    if (!every) {
        tagpu_logf("posecheck: tagpu_posecheck.on refused: \"%s\" is not a period of 1 to 1000000 "
                   "GameTime", b);
        return 0;
    }
    tagpu_logf("posecheck: every %lu GameTime", every);
    return (unsigned)every;
}

void tagpu_nodraw_levers(unsigned turbo_k)
{
    /* NO DRAW keeps the turbo's batch, so it needs a k; it is GDI's alone, which the players'
       renderer never is (cfg_load has read tagpu_gdi.on by now, dllmain.c) */
    if (tagpu_sim_lever("tagpu_nodraw.on")) {
        if (!tagpu_regstore_active())
            tagpu_logf("nodraw: tagpu_nodraw.on refused: not a test launch (tagpu_regstore.h)");
        else if (!g_config.gdi)
            tagpu_logf("nodraw: tagpu_nodraw.on refused: the renderer is not GDI (tagpu_gdi.on)");
        else if (!turbo_k)
            tagpu_logf("nodraw: tagpu_nodraw.on refused: no tagpu_turbo.on k (no-draw keeps the "
                       "turbo's batch)");
        else
            s_ndi.want = 1;
    }
    s_ndi.pc_every = posecheck_lever();
    /* THE SHADOW draws every frame it checks; on GDI, as no-draw, so that what it compares is the
       draw an undrawn frame stands in for */
    if (tagpu_sim_lever("tagpu_ndshadow.on")) {
        if (!tagpu_regstore_active())
            tagpu_logf("nodraw: tagpu_ndshadow.on refused: not a test launch (tagpu_regstore.h)");
        else if (!g_config.gdi)
            tagpu_logf("nodraw: tagpu_ndshadow.on refused: the renderer is not GDI (tagpu_gdi.on)");
        else
            s_ndi.shadow_want = 1;
    }
}

/* ---- the install --------------------------------------------------------------------------- */

/* the append's bound is in at 0x469807: the jmp to its stub and the NOPs to the join 0x469826,
   as fix_sort_buffer_end writes them (tagpu_patches.c) */
static int sort_bound_in(void)
{
    const unsigned char* p = (const unsigned char*)(size_t)VA_SORT_BOUND;
    int i;
    if (p[0] != 0xE9) return 0;
    for (i = 5; i < 0x469826 - 0x469807; i++)
        if (p[i] != 0x90) return 0;
    return 1;
}

/* a call at `site` whose target is `target` (E8 rel32), not taken by another module */
static int call_is(unsigned site, unsigned target)
{
    const unsigned char* p = (const unsigned char*)(size_t)site;
    int rel = (int)(target - (site + 5));
    return !tagpu_detour_landed(site, NULL, NULL) && p[0] == 0xE8 && !memcmp(p + 1, &rel, 4);
}

/* THE SITES. The frame stub at 0x4969CD is the replay's (tagpu_replay.c, frame_site_init): the
   frame visit, then DrawGameScreen or, in its place, tagpu_nodraw_frame as s_nd_skip says. What the
   skip path rests on (DISASSEMBLED), checked here:
   - DrawGameScreen is stdcall with two arguments, `ret 8` (0x46A3FD), pushed as `push ebx; push
     ebx` at 0x4969CB; tagpu_nodraw_frame is stdcall with two, so the skip leaves the stack exactly
     as the draw's return does;
   - EAX, ECX and EDX are dead after the call: 0x4969D2 reloads EAX (`mov eax,[0x511DE8]`),
     0x4969DD calls 0x4B6560, an import thunk (`jmp [0x4FC0DC]`) that reads no register, and ECX
     and EDX are written before any read (0x4969E4, 0x4969E7);
   - no flag is read before the call at 0x4969DD and the `sub` at 0x4969E9 set them;
   - tagpu_nodraw_frame keeps EBX, ESI, EDI and EBP as the compiler's stdcall does, as the draw's
     epilogue 0x46A3F3..0x46A3F6 restores them;
   - both paths reach 0x4969D2, so the scenario applier's jump there (tagpu_scenario.c) and the
     profiler call 0x4969DD run on both.
   THE REPOSE needs two more: the gate at DrawUnit's composite call 0x45AE73, written by
   tagpu_nodraw_site_in, and the append's bound at 0x469807 (tagpu_patches.c, fix_sort_buffer_end,
   applied at attach before this), whose rule it repeats. DrawUnit pushes the composite's two
   arguments (0x45AE6B, 0x45AE72) and pops its own registers right after the call
   (0x45AE78..0x45AE7C), so the composite pops them, `ret 8`, and the gate's skip returns as the
   composite does; EAX, ECX, EDX and the flags are dead there, DrawUnit's `ret 8` (0x45AE7D) reading
   none. The scoreboard's tests 0x494930..0x494993 and the hotkey sampler's entry 0x4C1B80 are
   checked too, so the skip rule reads the engine it was made against. */
int tagpu_nodraw_site_ok(void)
{
    static const unsigned char ARGS[2] = { 0x53, 0x53 };               /* 0x4969CB push ebx x2 */
    static const unsigned char RET8[3] = { 0xC2, 0x08, 0x00 };         /* 0x46A3FD ret 8       */
    static const unsigned char BOARD[100] = {                          /* 0x494930..0x494993  */
        0xA1, 0xE8, 0x1D, 0x51, 0x00, 0xF6, 0x80, 0x06, 0x7F, 0x03, 0x00, 0x80, 0x0F, 0x85, 0xA3,
        0x00, 0x00, 0x00, 0x6A, 0x20, 0xE8, 0x37, 0xD2, 0x02, 0x00, 0x85, 0xC0, 0x74, 0x39, 0x8B,
        0x0D, 0xE8, 0x1D, 0x51, 0x00, 0x8B, 0x81, 0x7D, 0x05, 0x00, 0x00, 0x83, 0xF8, 0xFF, 0x0F,
        0x84, 0x83, 0x00, 0x00, 0x00, 0x8B, 0x91, 0x31, 0x05, 0x00, 0x00, 0x8D, 0x0C, 0xC5, 0x00,
        0x00, 0x00, 0x00, 0x2B, 0xC8, 0x8D, 0x0C, 0x49, 0x8D, 0x0C, 0x48, 0x8D, 0x0C, 0x88, 0x8D,
        0x04, 0x48, 0x8B, 0x4A, 0x04, 0x80, 0x3C, 0x01, 0x03, 0x75, 0x5F, 0x8B, 0x0D, 0xD8, 0xF2,
        0x51, 0x00, 0x85, 0xC9, 0x0F, 0x8E, 0xC9, 0x04, 0x00, 0x00
    };
    static const unsigned char HOTKEY[16] = {                          /* 0x4C1B80..0x4C1B8F  */
        0x8B, 0x44, 0x24, 0x04, 0x83, 0xC0, 0xE0, 0x3D, 0xDB, 0x00, 0x00, 0x00, 0x0F, 0x87, 0xAF, 0x00
    };
    static const unsigned char TAIL[23] = {                            /* 0x45AE5C..0x45AE72  */
        0x8B, 0x4C, 0x24, 0x18, 0xA1, 0xE8, 0x1D, 0x51, 0x00, 0x8B, 0x97, 0x9E, 0x00, 0x00, 0x00,
        0x51, 0x8B, 0x88, 0x7B, 0x43, 0x01, 0x00, 0x52
    };
    static const unsigned char EPILOGUE[8] = {                         /* 0x45AE78..0x45AE7F  */
        0x5D, 0x5E, 0x5F, 0x5B, 0x59, 0xC2, 0x08, 0x00
    };
    const char* other;
    if (!s_ndi.want) return 0;
    other = !tagpu_detour_bytes_ok(0x004969CBu, ARGS, sizeof ARGS) ? "0x4969CB" :
            !tagpu_detour_bytes_ok(0x0046A3FDu, RET8, sizeof RET8) ? "0x46A3FD" :
            !tagpu_detour_bytes_ok(0x00494930u, BOARD, sizeof BOARD) ? "0x494930" :
            !tagpu_detour_bytes_ok(VA_HOTKEY, HOTKEY, sizeof HOTKEY) ? "0x4C1B80" :
            !tagpu_detour_bytes_ok(0x0045AE5Cu, TAIL, sizeof TAIL) ? "0x45AE5C" :
            !call_is(SITE_COMPOSITE, VA_COMPOSITE) ? "0x45AE73" :
            !tagpu_detour_bytes_ok(0x0045AE78u, EPILOGUE, sizeof EPILOGUE) ? "0x45AE78" :
            !sort_bound_in() ? "0x469807" : NULL;
    if (other) {
        tagpu_logf("nodraw: UNAVAILABLE: %s holds other bytes", other);
        s_ndi.want = 0;
    }
    return s_ndi.want;
}

static int call_write(unsigned site, unsigned target)
{
    unsigned char c[5];
    int rel = (int)(target - (site + 5));
    c[0] = 0xE8;
    memcpy(c + 1, &rel, 4);
    return tagpu_detour_write(site, c, 5);
}

static void __cdecl shadow_call(int site, char* unit);

/* a recorder of one sweep call, entered by its `call 0x45AC20`: shadow_call(site, unit) with every
   register and the flags kept, then a jump to DrawUnit with the site's return address and the two
   arguments on the stack as the call left them. After pushfd and pushad the unit, the call's
   second argument, is at [esp+0x2C]. */
static unsigned char* shadow_stub(int site)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x9C;                                        /* pushfd               */
    *p++ = 0x60;                                        /* pushad               */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x2C; /* push dword [esp+0x2C] */
    *p++ = 0x6A; *p++ = (unsigned char)site;            /* push site            */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)shadow_call); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;              /* add esp,8            */
    *p++ = 0x61;                                        /* popad                */
    *p++ = 0x9D;                                        /* popfd                */
    *p++ = 0xE9; tagpu_detour_rel(p, VA_DRAWUNIT);      /* jmp DrawUnit         */
    return s;
}

/* After the replay wrote the frame stub, skippable when tagpu_nodraw_site_ok said so: THE REPOSE's
   rows for either lever; the composite's gate for no-draw, rows and gate both or neither -- without
   them s_ndi.stub stays 0, the gate never skips and the stub always jumps to DrawGameScreen; and
   THE SHADOW's lists and two recorders, all or none: the second site's write failing puts the
   first's call back. Nothing runs the game yet, so no thread is inside a call being re-pointed. */
void tagpu_nodraw_site_in(void)
{
    unsigned char *gate, *a, *b;
    if (s_ndi.want || s_ndi.shadow_want) {
        if (!s_nd_slot) s_nd_slot = (char**)SIM_ALLOC(s_nd_slot, ND_SLOTS * sizeof *s_nd_slot);
        if (!s_nd_count) s_nd_count = (unsigned*)SIM_ALLOC(s_nd_count, ND_ROWS * sizeof *s_nd_count);
    }
    if (s_ndi.want) {
        gate = tagpu_detour_stub();
        if (!s_nd_slot || !s_nd_count || !gate) {
            tagpu_detour_stub_free(gate);
            tagpu_logf("nodraw: UNAVAILABLE: no memory for the repose's rows or the composite's gate");
            s_ndi.want = 0;
        } else {
            tagpu_skipcall_emit(gate, (unsigned)(size_t)gate, (unsigned)(size_t)&s_nd_repose,
                                VA_COMPOSITE, 8);
            if (call_write(SITE_COMPOSITE, (unsigned)(size_t)gate)) {
                s_ndi.stub = 1;
            } else {
                tagpu_detour_stub_free(gate);
                tagpu_logf("nodraw: UNAVAILABLE: the composite's gate at 0x45AE73 could not be "
                           "written");
                s_ndi.want = 0;
            }
        }
    }
    if (!s_ndi.shadow_want) return;
    if (!call_is(SITE_A, VA_DRAWUNIT) || !call_is(SITE_B, VA_DRAWUNIT) || !sort_bound_in()) {
        tagpu_logf("nodraw: shadow UNAVAILABLE: 0x469A00, 0x469BA3 or 0x469807 holds other bytes");
        return;
    }
    s_sh_sel = (unsigned*)SIM_ALLOC(s_sh_sel, ND_SLOTS * sizeof *s_sh_sel);
    s_sh_drw = (unsigned*)SIM_ALLOC(s_sh_drw, ND_SLOTS * sizeof *s_sh_drw);
    a = shadow_stub(0);
    b = shadow_stub(1);
    if (!s_nd_slot || !s_nd_count || !s_sh_sel || !s_sh_drw || !a || !b) {
        tagpu_detour_stub_free(a);
        tagpu_detour_stub_free(b);
        tagpu_logf("nodraw: shadow UNAVAILABLE: no memory for its lists or its recorders");
        return;
    }
    if (!call_write(SITE_A, (unsigned)(size_t)a)) {
        tagpu_detour_stub_free(a);
        tagpu_detour_stub_free(b);
        tagpu_logf("nodraw: shadow UNAVAILABLE: 0x469A00 could not be written");
        return;
    }
    if (!call_write(SITE_B, (unsigned)(size_t)b)) {
        if (call_write(SITE_A, VA_DRAWUNIT)) tagpu_detour_stub_free(a);
        tagpu_detour_stub_free(b);
        tagpu_logf("nodraw: shadow UNAVAILABLE: 0x469BA3 could not be written");
        return;
    }
    s_ndi.shadow = 1;
    tagpu_logf("nodraw: shadow on at 0x469A00 and 0x469BA3");
}

void tagpu_nodraw_guard(int in)
{
    if (!s_ndi.want) return;
    s_ndi.on = s_ndi.stub && in;
    if (s_ndi.on)
        tagpu_logf("nodraw: on at 0x4969CD, marked at 0x49692D");
    else
        tagpu_logf("nodraw: UNAVAILABLE: %s is not in",
                   s_ndi.stub ? "the tick-batch guard at 0x49692D"
                              : "the skippable stub at 0x4969CD");
}

volatile unsigned char* tagpu_nodraw_skip(void)
{
    return &s_nd_skip;
}

/* ---- the mark ------------------------------------------------------------------------------ */

void tagpu_nodraw_mark(char* ta, int turbo_batch)
{
    s_nd.armed = s_ndi.on && turbo_batch && !(*(const unsigned short*)(ta + OFF_PAUSE) & 2) &&
                 !tagpu_puppet_on();
    s_nd.from = *(const unsigned*)(ta + OFF_GAMETIME);
    s_nd.due  = *(const unsigned*)(ta + OFF_DUE_TICKS);
}

/* ---- the repose ---------------------------------------------------------------------------- */

/* THE REPOSE's room: the sort's dimensions inside the rows made at install, and every HotUnits slot
   inside the unit array -- below its slot count, as the engine's own walks bound it, and at or
   before its last slot -- each read from the engine and bounded here, before the frame is undrawn;
   0 when one fails, and the frame draws. The cull 0x48BAE0 wrote HotUnits this frame (0x49697B, or
   the frame visit's E_HotUnitsCull) and nothing writes it again before the draw would
   (DISASSEMBLED: its writers are the cull's 0x48BC6A/0x48BC9D and LoadMap's and the teardown's
   0x485596/0x4859FD), so what is bounded here is what nodraw_repose walks. */
static int nodraw_room(char* ta)
{
    const int cap = *(const int*)(ta + OFF_SORT_CAP), rows = *(const int*)(ta + OFF_SORT_ROWS);
    const int n = *(const int*)(ta + OFF_NHOT);
    const unsigned nslots = *(const unsigned short*)(ta + OFF_NSLOTS);
    const unsigned short* hot = *(const unsigned short* const*)(ta + OFF_HOT);
    const char* begin = *(const char* const*)(ta + OFF_BEGIN);
    const char* last = *(const char* const*)(ta + OFF_LAST);
    int i;
    if (cap <= 0 || rows <= 0 || cap > (int)ND_ROWS || rows > (int)ND_ROWS ||
        (unsigned)cap * (unsigned)rows > ND_SLOTS)
        return 0;
    if (n < 0 || (unsigned)n > nslots || (n && (!hot || !begin))) return 0;
    for (i = 0; i < n; i++)
        if (hot[i] >= nslots || begin + (size_t)hot[i] * REC_UNIT_BYTES > last) return 0;
    return 1;
}

/* DrawUnit with its composite call inert: the repose alone, for one unit and its cargo; `site`, 0
   for A and 1 for B, is the shadow's */
static void nodraw_drawunit(void* ctx, char* unit, int site)
{
    (void)site;
    s_nd_repose = 1;
    E_DrawUnit(ctx, unit);
    s_nd_repose = 0;
}

/* THE REPOSE. DrawUnit 0x45AC20 reposes before it draws: from its entry to 0x45AE5B it refreshes
   the unit's pose cache -- the cached body turn o3+0x18..+0x1D when it is 8 or more off the unit's
   (0x45AC3D..0x45ACA8), and while o3+8 is set the posed vertex buffers *(prim+0x22) and origins
   prim+0x16..+0x21 of every piece (0x45ACC1..0x45AD28) -- then does the same for each unit of its
   cargo chain +0x8A/+0x8E without +0x110 bit 17 (0x45AD2B..0x45AE56), and only then calls the
   composite 0x458810 at 0x45AE73, the one instruction of it that reads its ctx (0x45AE5C) and the
   only call that rasterises (DISASSEMBLED). The simulation reads that cache: the SweetSpot point
   0x43E0B0 that AutoAim aims every unit-target weapon at, and EXPLODE's shatter 0x421700; and the
   only other repose is COB EMIT_SFX's 0x45AB10 (0x480EFC). So an undrawn frame runs DrawUnit for
   exactly the units the draw would, in its order, with the composite call inert (s_nd_repose,
   the gate tagpu_nodraw_site_in writes at 0x45AE73): what it skips is what the composite writes,
   the composite frames, the ring and o3+4, which only the composite itself reads (o3+4 at
   0x458870/0x4589AA).
   WHICH UNITS, IN WHICH ORDER: DrawGameScreen's own, re-derived from the same engine state at the
   same instant -- the frame's HotUnits, the eye after the draw's commands
   (tagpu_packet_nodraw_top), the sort's dimensions and the feature grid's rows (DISASSEMBLED
   0x469745..0x469BC7, render.md *Which units the two sites reach*):
   - each hot unit is filed under row (feet z - eye y) / 16 + 16, truncated toward zero as
     `cdq; and edx,0xf; add; sar 4` does, if 0 <= row < rows (0x4697ED..0x469805), by the bound
     tagpu_patches.c's fix_sort_buffer_end installs at 0x469807: appended while count[row] <
     (rows - row) * cap, at slot row * cap + count[row]. A row's appends run on into the next
     row's slots exactly as the engine's do, so a unit the draw overwrites is not reposed here
     either;
   - site A (0x4699A8..0x469A18) walks the rows clip .. clip + n - 1, with first = eye y / 16 - 16,
     clip = -first when first < 0 (first then 0), n = rows - clip, cut to the grid's rows - 1 -
     first when it passes them (0x469745..0x469786), and reaches DrawUnit at 0x469A00 for a unit
     with +0x110 & 3 == 1 and a COB +0x9A;
   - site B (0x469B54..0x469BC7) walks every row and reaches DrawUnit at 0x469BA3 for a unit with
     +0x110 & 3 != 1 and a COB +0x9A;
   - both only when the draw's first argument is nonzero (0x4699D3, 0x469B09).
   Between the two sites the draw calls the weapons' and the effects' draws 0x49BE60/0x420B00, and
   between site A's rows the features' 0x46A610, which reaches DrawUnit only for a wreck through
   the scratch unit main+0x1420F (0x46A762): a wreck's Object3do, which only the draw and the
   feature free touch (no simulation reader). None of them writes a field the selection reads
   (+0x110, +0x9A, +0x86, the cargo chain, the eye, HotUnits): the draw writes nothing the
   simulation reads but the pose cache and the scoreboard rank, player +0x148 (THE GATE).
   Every slot written is below rows * cap <= ND_SLOTS and every count below rows <= ND_ROWS
   (nodraw_room, which the caller ran on this frame's state). nodraw_walk hands each unit, in the
   draw's order, to `visit` with its site: the repose's DrawUnit, or THE SHADOW's list. */
typedef void (*NdVisit)(void* ctx, char* unit, int site);
static void nodraw_walk(char* ta, NdVisit visit, void* ctx)
{
    const int cap = *(const int*)(ta + OFF_SORT_CAP), rows = *(const int*)(ta + OFF_SORT_ROWS);
    const int eye = *(const int*)(ta + OFF_EYE_Y), grid = *(const int*)(ta + OFF_GRIDH);
    const int n = *(const int*)(ta + OFF_NHOT);
    const unsigned short* hot = *(const unsigned short* const*)(ta + OFF_HOT);
    char* const begin = *(char* const*)(ta + OFF_BEGIN);
    char** const slot = s_nd_slot;
    unsigned* const count = s_nd_count;
    int i, r, first, clip, rowsA;
    unsigned j;
    memset(count, 0, (size_t)rows * sizeof *count);
    for (i = 0; i < n; i++) {
        char* u = begin + (size_t)hot[i] * REC_UNIT_BYTES;
        const int row = ((int)*(const short*)(u + U_FEET_Z) - eye) / 16 + 16;
        if (row < 0 || row >= rows) continue;
        if (count[row] >= (unsigned)((rows - row) * cap)) continue;
        slot[row * cap + count[row]++] = u;
    }
    first = eye / 16 - 16;
    clip = 0;
    rowsA = rows;
    if (first < 0) {
        clip = -first;
        rowsA = rows - clip;
        first = 0;
    }
    if (rowsA + first > grid - 1) rowsA = grid - first - 1;
    for (r = clip; r < clip + rowsA; r++)
        for (j = 0; j < count[r]; j++) {
            char* u = slot[r * cap + j];
            if ((*(const unsigned*)(u + U_STATE) & 3) == 1 && *(const unsigned*)(u + U_COB))
                visit(ctx, u, 0);
        }
    for (r = 0; r < rows; r++)
        for (j = 0; j < count[r]; j++) {
            char* u = slot[r * cap + j];
            if ((*(const unsigned*)(u + U_STATE) & 3) != 1 && *(const unsigned*)(u + U_COB))
                visit(ctx, u, 1);
        }
}

/* THE REPOSE itself. ctx is a zeroed OFFSCREEN: DrawUnit's only read of it is 0x45AE5C, for the
   composite. */
static void nodraw_repose(char* ta)
{
    unsigned ctx[12];
    memset(ctx, 0, sizeof ctx);
    nodraw_walk(ta, nodraw_drawunit, ctx);
}

/* ---- the shadow ---------------------------------------------------------------------------- */

static unsigned sh_entry(char* ta, const char* unit, int site)
{
    const char* begin = *(const char* const*)(ta + OFF_BEGIN);
    return (unsigned)((size_t)(unit - begin) / REC_UNIT_BYTES) << 1 | (unsigned)site;
}

static void shadow_visit(void* ctx, char* unit, int site)
{
    char* ta = (char*)ctx;
    if (s_sh.nsel < ND_SLOTS) s_sh_sel[s_sh.nsel++] = sh_entry(ta, unit, site);
}

/* THE SHADOW's two halves. tagpu_nodraw_shadow_select runs at the top of an in-play DrawGameScreen,
   right after the draw's commands (tagpu_packet_pub.c, before_draw), where an undrawn frame runs its
   repose: the selection over the same engine state, into s_sh_sel, without a call. The recorders
   then append each unit the draw passes to DrawUnit from its two sweeps (0x469A00, 0x469BA3), and
   only those: the wreck husk's call 0x46A762 is not a sweep's. tagpu_nodraw_shadow_settle, right
   after the draw returns, compares the lists entry by entry. Every entry is the unit's slot as its
   address in the array says and the site; a selection whose room fails (nodraw_room) is a frame
   not checked, as is a draw that passed more than ND_SLOTS units. */
void tagpu_nodraw_shadow_select(void)
{
    char* ta = ta_main();
    s_sh.open = 0;
    if (!s_ndi.shadow || !ta) return;
    s_sh.nsel = s_sh.ndrw = 0;
    s_sh.over = 0;
    s_sh.g = *(const unsigned*)(ta + OFF_GAMETIME);
    if (!s_sh.any) s_sh.g0 = s_sh.g;
    s_sh.any = 1;
    if (!nodraw_room(ta)) {
        s_sh.unchecked++;
        return;
    }
    nodraw_walk(ta, shadow_visit, ta);
    s_sh.open = 1;
}

static void __cdecl shadow_call(int site, char* unit)
{
    char* ta = ta_main();
    if (!s_sh.open || !ta) return;
    if (s_sh.ndrw < ND_SLOTS) s_sh_drw[s_sh.ndrw++] = sh_entry(ta, unit, site);
    else s_sh.over = 1;
}

void tagpu_nodraw_shadow_settle(void)
{
    unsigned i, n;
    if (!s_sh.open) return;
    s_sh.open = 0;
    if (s_sh.over) {
        s_sh.unchecked++;
        return;
    }
    s_sh.units += s_sh.ndrw;
    if (++s_sh.compared % SH_EVERY == 0)
        tagpu_logf("nodraw: shadow at GameTime %u, %u frames compared, %u differed, %u not checked, "
                   "%llu units drawn in them", s_sh.g, s_sh.compared, s_sh.differed, s_sh.unchecked,
                   s_sh.units);
    n = s_sh.nsel < s_sh.ndrw ? s_sh.nsel : s_sh.ndrw;
    for (i = 0; i < n && s_sh_sel[i] == s_sh_drw[i]; i++) {}
    if (i == n && s_sh.nsel == s_sh.ndrw) return;
    s_sh.differed++;
    if (s_sh.noted++ >= SH_NOTES) return;
    if (i < n)
        tagpu_logf("nodraw: shadow DIFFERS at GameTime %u: the draw passed %u units, the selection "
                   "%u; first at %u: the draw's slot %u at site %c, the selection's slot %u at site "
                   "%c", s_sh.g, s_sh.ndrw, s_sh.nsel, i, s_sh_drw[i] >> 1, "AB"[s_sh_drw[i] & 1],
                   s_sh_sel[i] >> 1, "AB"[s_sh_sel[i] & 1]);
    else
        tagpu_logf("nodraw: shadow DIFFERS at GameTime %u: the draw passed %u units, the selection "
                   "%u, equal for the first %u", s_sh.g, s_sh.ndrw, s_sh.nsel, n);
}

/* what an undrawn frame still needs, in the draw's order: the render-options screen's tick and the
   draw's commands where DrawGameScreen's two observers would have run them (tagpu_menu.c's first,
   as it was installed first), the repose where its unit sweeps would have, the trigger family and
   the profiles' match latch where the flip would have, then ImpAI's digests, the map's features
   and the publish with the live-state log where DrawGameScreen's `after` would have. The frame
   stub jumps here in DrawGameScreen's place, with its two arguments and its return to 0x4969D2 on
   the stack: stdcall, `ret 8`, as the draw. */
void __stdcall tagpu_nodraw_frame(unsigned draw_units, unsigned blit)
{
    char* ta = ta_main();
    (void)blit;
    tagpu_menu_nodraw_frame();
    tagpu_packet_nodraw_top();
    if (ta && draw_units) nodraw_repose(ta);
    tagpu_gui_nodraw_frame();
    tagpu_packet_nodraw_frame();
    s_nd_skip = 0;
}

/* ---- the gate ------------------------------------------------------------------------------ */

/* THE GATE. WHAT MAKES NO DRAW EXACT. The batch is the turbo's k on both sides, so where the
   batches fall is unchanged (engine/sim.md, *The batch is part of the result*); and between two
   batches the same code that reaches simulation state runs as on a drawn frame. Of DrawGameScreen
   0x468CF0..0x46A3FD and its observers, two writes reach it (engine/render.md, *What
   DrawGameScreen writes*); everything else they write only draws read:
   - the pose cache, which DrawUnit's repose writes and the SweetSpot point 0x43E0B0 and the
     shatter 0x421700 read: tagpu_nodraw_frame runs that repose on the same units in the same order,
     after the same commands, whose eye places the rows (THE REPOSE);
   - the scoreboard rank, player +0x148, which the scoreboard 0x4948E0 writes only past its test at
     0x49498E (the store 0x494E27 in the compaction 0x494B84..0x494E57).
   So every frame whose draw could reach that store draws: the slide 0x51F2D8 above 0 (the `jle`
   at 0x49498E is signed), bit 7 of main+0x37F06 (0x494935), or SPACE down through 0x4C1B80
   (0x494942). The board's text-field test 0x49494D..0x494984 only hides it, so a SPACE frame
   draws whatever the focus.
   Those bytes are checked at attach (tagpu_nodraw_site_ok); the sampler is called last, only when
   the cheaper tests pass, and it masks GetAsyncKeyState's bit 0, so the call leaves no history.
   THE SPACE SAMPLE IS THE ONE TIMED TEST. The gate samples SPACE at the end of the frame visit,
   where the draw would sample it a few instructions later, inside 0x4948E0: a SPACE that goes
   down between the two samples is missed by this frame, which skips where the draw would have
   shown the board and compacted the ranks. A SPACE still down at the next frame's gate draws
   there, a frame late; one released before it is never seen, and the ranks are compacted the next
   time the board shows. So a human's SPACE in a no-draw test can leave the recorded rank behind
   the drawn run's; the rank is display state (engine/units.md, +0x148: no simulation path reads
   it), and a test injects no SPACE. Nothing else the gate decides depends on a time.
   THE CONDITIONS. `quiet` is taken before the visit, and together with the two GameTime
   equalities it makes "the step ran exactly the marked batch and the visit moved nothing" a fact
   of this frame: a seek, switch, rewind or takeover is pending or moves GameTime, and a batch cut
   short by a level end leaves GameTime short of the mark's sum; each of those frames draws.
   WHAT IT DOES NOT FIX. Readers of wall time and of the game thread's CRT rand stream (the camera
   scroll 0x41CEA2) depend on frame timing at any pacing; no-draw changes the timing as any load
   does, and adds no class. The hover wave runs on GameTime and the meteor shower on a stream of its
   own (tagpu_patches.c, fix_hover_clock and fix_meteor_stream), so neither is one of them.
   WHAT DRAWS. Paused, held and in-game-screen frames (the mark is not reached), a frame whose visit
   seeks, switches, rewinds or takes over, a batch cut short, a frame the scoreboard could write,
   and a frame whose unit sort or HotUnits fails its bound (nodraw_room).
   WHAT AN UNDRAWN FRAME LEAVES. The cursor draw 0x4C2870 refreshes the current mouse record from
   GetCursorPos when its countdown +0x1AE expires (0x4C288E..0x4C28C6); it does not run. The window
   procedure keeps that record current on every move (0x4C2360 from 0x4B5EA4) and each click carries
   its own record in the ring (0x4C2E30 from 0x4B5F51), so injected input is placed by its
   messages, not by the draw. Screenshots and `tacli ui` see the last drawn frame. */
void tagpu_nodraw_gate(char* ta, unsigned g0, int quiet)
{
    const int armed = s_nd.armed;
    s_nd.armed = 0;
    if (!armed || !ta) return;
    if (!s_nd.any) s_nd.g0 = g0;
    s_nd.any = 1;
    if (!quiet || g0 != s_nd.from + s_nd.due ||
        *(const unsigned*)(ta + OFF_GAMETIME) != g0) {
        s_nd.drawn++;
        return;
    }
    if (*(volatile const int*)(size_t)VA_SCOREBOARD_SLIDE > 0 ||
        (*(const unsigned char*)(ta + OFF_OPTIONS) & 0x80) ||
        ((int (__stdcall*)(int))(size_t)VA_HOTKEY)(HOTKEY_SPACE)) {
        s_nd.board++;
        return;
    }
    if (!nodraw_room(ta)) {
        s_nd.drawn++;
        return;
    }
    /* THE SHADOW checks drawn frames only, so beside no-draw every other frame this gate would skip
       draws, and one game runs both */
    if (s_ndi.shadow && (s_nd.alt ^= 1)) {
        s_nd.shadowed++;
        return;
    }
    s_nd.skipped++;
    s_nd_skip = 1;
}

void tagpu_nodraw_level_end(unsigned g1)
{
    if (s_ndi.on && s_nd.any)
        tagpu_logf("nodraw: from GameTime %u to %u, %u running frames not drawn, %u drawn for the "
                   "scoreboard, %u drawn otherwise, %u drawn for the shadow", s_nd.g0, g1,
                   s_nd.skipped, s_nd.board, s_nd.drawn, s_nd.shadowed);
    if (s_ndi.shadow && s_sh.any)
        tagpu_logf("nodraw: shadow from GameTime %u to %u, %u frames compared, %u differed, %u not "
                   "checked, %llu units drawn in them", s_sh.g0, g1, s_sh.compared, s_sh.differed,
                   s_sh.unchecked, s_sh.units);
    s_sh.open = 0;
    s_sh.compared = s_sh.differed = s_sh.unchecked = s_sh.noted = 0;
    s_sh.units = 0;
    s_sh.g0 = s_sh.any = 0;
    s_nd.armed = 0;
    s_nd.skipped = s_nd.board = s_nd.drawn = s_nd.shadowed = s_nd.alt = 0;
    s_nd.g0 = s_nd.any = 0;
    s_nd.pc_next = 0;
}

/* ---- the pose check ------------------------------------------------------------------------ */

/* THE POSE CHECK, tagpu_posecheck.on (a test launch's; its content the period in GameTime, 300
   when empty): on the first frame at or past each multiple of the period, before the frame visit,
   one line with two digests over every unit with a model -- `pose`, its pose cache (the slot, the
   piece count, o3+8, o3+0x18..+0x1D, and per piece the origin prim+0x16..+0x21, prim+0x26 and the
   posed vertices *(prim+0x22)), and `aim`, the SweetSpot point 0x43E0B0 returns for each piece --
   so that two takeovers of one recording, one drawn and one not, compare what the next step's
   AutoAim will read (tools/replay/nodraweq.py). o3+4 is left out: the
   composite writes it and only the composite reads it. A model whose piece count or a piece's
   vertex count is not data is counted as `skipped`. Game thread, between two steps, so every model
   read here is its unit's: the destructor nulls +0x9E as it frees the model (0x486D93..0x486DA3).
   The walk ends at the array's last slot and below its slot count, whichever comes first. The
   pieces are the Object3do's +0, the count its build 0x45AEC0 increments for each piece it places
   at o3+0x22+i*0x36 (0x45AECA..0x45AED9, 0x45AF35..0x45AF38). */
#define POSE_PIECES_MAX 1024
#define POSE_VERTS_MAX  65536
static void pc_add(unsigned* h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    while (n--) {
        h[0] = (h[0] ^ *b++) * 16777619u;
        h[1] = (h[1] + h[0]) * 2654435761u;
    }
}

static int pc_model_ok(const char* o3, int pieces)
{
    int i;
    if (pieces <= 0 || pieces > POSE_PIECES_MAX) return 0;
    for (i = 0; i < pieces; i++) {
        const char* prim = o3 + 0x22 + i * 0x36;
        const char* node = *(const char* const*)prim;
        if (!node || !*(const char* const*)(prim + 0x22)) return 0;
        if (*(const int*)(node + 4) < 0 || *(const int*)(node + 4) > POSE_VERTS_MAX) return 0;
    }
    return 1;
}

void tagpu_nodraw_posecheck(char* ta, unsigned g)
{
    char* const begin = *(char* const*)(ta + OFF_BEGIN);
    char* const last = *(char* const*)(ta + OFF_LAST);
    const unsigned nslots = *(const unsigned short*)(ta + OFF_NSLOTS);
    unsigned pose[2] = { 2166136261u, 0 }, aim[2] = { 2166136261u, 0 }, units = 0, skipped = 0;
    char *u, *end;
    if (!s_ndi.pc_every || g < s_nd.pc_next) return;
    s_nd.pc_next = (g / s_ndi.pc_every + 1) * s_ndi.pc_every;
    if (!begin || last < begin || !nslots) return;
    end = begin + (size_t)(nslots - 1) * REC_UNIT_BYTES;
    if (end > last) end = last;
    for (u = begin; u <= end; u += REC_UNIT_BYTES) {
        const char* o3 = *(const char* const*)(u + U_MODEL);
        const unsigned at = (unsigned)((u - begin) / REC_UNIT_BYTES);
        int pieces, i;
        if (!o3) continue;
        pieces = *(const int*)o3;
        if (!pc_model_ok(o3, pieces)) {
            skipped++;
            continue;
        }
        pc_add(pose, &at, sizeof at);
        pc_add(pose, &pieces, sizeof pieces);
        pc_add(pose, o3 + 8, 4);
        pc_add(pose, o3 + 0x18, 6);
        for (i = 0; i < pieces; i++) {
            const char* prim = o3 + 0x22 + i * 0x36;
            const int verts = *(const int*)(*(const char* const*)prim + 4);
            int out[3];
            pc_add(pose, prim + 0x16, 12);
            pc_add(pose, prim + 0x26, 2);
            pc_add(pose, *(const char* const*)(prim + 0x22), (size_t)verts * 12);
            E_PiecePoint(u, out, i);
            pc_add(aim, &at, sizeof at);
            pc_add(aim, out, sizeof out);
        }
        units++;
    }
    tagpu_logf("posecheck: GameTime %u units %u skipped %u pose %08x%08x aim %08x%08x", g, units,
               skipped, pose[0], pose[1], aim[0], aim[1]);
}
