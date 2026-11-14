/* tagpu_scenario.c — the applier: a compiled wire file becomes a live situation.
   Contract: inc/tagpu_scenario.h. Why it exists: research/notes/scenario-format.md.

   Two halves, on two threads, deliberately:

     present path (tagpu_overlay_draw)   trigger -> parse -> arena;  arena -> result JSON
     Game_MainLoopTick detour @0x4969D2  arena  -> resolve -> create -> orders

   Nothing between them but a one-way state machine and an interlocked hand-off, so
   neither half can see the other's half-built work. */

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_scenario.h"
#include "tagpu_log.h"
#include "tagpu_limits.h"   /* TAGPU_LIM_UNITS: the arena holds a full game */
#include "tagpu_zoom.h"     /* tagpu_zoom_place_eye: the camera's range */
#include "tagpu_patches.h"  /* tagpu_kill_hold: a create's messages wait for dress_unit */
#include "tagpu_puppet.h"   /* tagpu_puppet_on: the recorder is the only writer on a puppet */
#include "tagpu_packet_pub.h" /* tagpu_packet_pub_level_gen: the delayed orders' level */
#include "tasim_wire.h"     /* the wire's parser, shared with tasim */

#define SCN_TRIGGER   "tagpu_scenario.trigger"
#define SCN_OUT       "tagpu_scenario.json"
#define SCN_TMP       "tagpu_scenario.json.tmp"
#define SW_TRIGGER    "tagpu_switches.trigger"
#define SW_OUT        "tagpu_switches.json"
#define SW_TMP        "tagpu_switches.json.tmp"

/* ---------------------------------------------------------------- engine layout */

#define TA_MAINPP     0x00511DE8u  /* TAdynmemStruct**                            */

#define OFF_PLAYERS   0x1B63       /* PlayerStruct Players[10], stride 0x14B      */
#define PL_STRIDE     0x14B
#define PL_ACTIVE     0x00         /* int  PlayerActive                           */
#define PL_TYPE       0x73         /* char My_PlayerType (0 = empty slot)         */
#define PL_ENERGY     0x8C         /* float fCurrentEnergy  (PlayerRes + 0x00)    */
#define PL_METAL      0x98         /* float fCurrentMetal   (PlayerRes + 0x0C)    */
#define PL_EINCOME    0x90         /* float energy income, expense, metal income  */
#define PL_EEXPENSE   0x94         /*   and expense: the last economy pass's sums */
#define PL_MINCOME    0x9C         /*   (0x4019C6, 0x4019DD, 0x401A05, 0x401A2D)  */
#define PL_MEXPENSE   0xA0
#define PL_MAXENERGY  0xA4         /* float fMaxEnergyStorage                     */
#define PL_MAXMETAL   0xA8         /* float fMaxMetalStorage                      */
#define PL_FIRSTUNIT  0x67         /* UnitStruct* first/last of this player's     */
#define PL_LASTUNIT   0x6B         /* units, INCLUSIVE, stepping UNIT_STRIDE      */
#define PL_BASESTORE_E 0xDC        /* float base energy storage, and              */
#define PL_BASESTORE_M 0xE0        /* float base metal storage: the economy pass
                                      0x401941 adds them to the storages it
                                      recomputes while +0x149 bit 0 is set
                                      (0x401988..0x4019AA, DISASSEMBLED)          */
#define PL_SELF       0x146        /* u8 the record's own seat, 10 = a free record
                                      (0x4646B4, 0x46434D)                        */
#define PL_BASEFLAG   0x149        /* u8 bit 0: base storage in force (0x496F5D)  */
#define PL_ALIVE      0x144        /* u16 the live unit count                     */
#define PL_AIOBJ      0x74         /* the AI object, 0x3D bytes (ctor 0x408CB0)   */
#define PL_GROUPS     0x78         /* the control groups, 10 x 0x20, or NULL      */
#define AI_SCAN       0x39         /* its weapon scan's cursor, a unit or 0       */

#define OFF_MAPPXW    0x14223      /* int MapWidth  (world units)                 */
#define OFF_MAPPXH    0x14227      /* int MapHeight                               */
#define OFF_FMAPX     0x14233      /* int FeatureMapSizeX (tiles, 16 units each)  */
#define OFF_FMAPY     0x14237
#define OFF_FMAP      0x14287      /* FeatureStruct* FeatureMap, stride 0x0D      */
#define FM_STRIDE     0x0D
#define FM_HEIGHT     0x04         /* unsigned char height                        */
#define FM_DEF        0x08         /* u16 the cell's feature def word (engine/map.md) */

#define OFF_WATCHED   0x2A42       /* u8 watched player id — whose selection it is */
#define OFF_NETWORK   0x2A44       /* bit 0: a network game (the pump's test, 0x453D4B) */
#define OFF_OBJECTIVE 0x391ED      /* the level's objective: level_objective   */
#define OBJ_ARMED     0x88         /* u32: the end tests run while it is non-zero */

#define OFF_BEGIN     0x14357      /* UnitStruct* BeginUnitsArray_p               */
#define OFF_END       0x1435B      /* UnitStruct* EndOfUnitsArray_p: the LAST slot
                                      itself, begin + count*0x118 - 0x118 (stored at
                                      0x4855CF..0x4855D6; the engine's own walks end
                                      with jbe, 0x401F12, 0x48BD38), so a walk runs
                                      while u <= end and holds (end - begin) / 0x118
                                      + 1 slots (DISASSEMBLED)                     */
#define UNIT_STRIDE   0x118
#define OFF_USLOTS    0x14351      /* u16 the array's slot count, stored at 0x4854EF;
                                      slot 0 is no player's (its +0x96 is NULL,
                                      0x485880), so a block's first slot is 1 or more */

#define OFF_UCOUNT    0x1438F      /* unsigned UNITINFOCount                      */
#define OFF_UDEFS     0x1439B      /* UnitDefStruct*, stride 0x249                */
#define UD_STRIDE     0x249
#define UD_NAME       0x20         /* char UnitName[0x20]  "ARMPW"                */
#define UD_ESTORE     0x1E2        /* float energystorage, and                    */
#define UD_MSTORE     0x1E6        /* float metalstorage (engine/keys.md)         */
#define UD_BITS       0x241        /* u32 the def's flag word: bit 24 (0x486159) */
#define UD_WEAPON0    0x1EE        /* WeaponDef* of slot 0, which the slot init
                                      0x49E070 copies to the slot's +0xC (0x49E0B0) */

#define OFF_LIMIT     0x37EEA      /* u16 ActualUnitLimit                         */
#define OFF_PERPLAYER 0x37EEC      /* u16 MaxUnitNumberPerPlayer                  */
#define OFF_ARRAYCAP  0x37EE6      /* u16 the unit array's count a player: the cap
                                      in force (the game start copies +0x37EEC, a
                                      network game the host's value)              */
#define OFF_CMDRDEATH 0x37EF6      /* u32 ActiveCommanderDeath (0 = game goes on) */
#define OFF_SWITCHES  0x37F2F      /* u16 SoftwareDebugMode                       */
#define OFF_GAMETIME  0x38A47      /* int GameTime                                */

#define U_TURN        0x64         /* Volume_Word{bank, heading, pitch}           */
#define U_HEADING     0x66         /* full circle 0x10000, TA's build default is  */
#define U_XPOS        0x6C         /*   0x8000 and it increases CCW from above    */
#define U_ZPOS        0x70         /* altitude, whole world units                 */
#define U_YPOS        0x74         /* map depth, whole world units                */
#define U_ORDERS      0x5C         /* UnitOrdersStruct* UnitOrders                */
#define U_INGAMEIDX   0xA8         /* short UnitInGameIndex — recycled on death   */
#define U_UDEF        0x92         /* UnitDefStruct* — a live unit's own type     */
#define U_HEALTHPCTA  0xF6         /* char HealthPerA                             */
#define U_HEALTHPCTB  0xF7
#define U_OWNER       0xFF         /* unsigned char cOwnerID                      */
#define U_NANOFRAME   0x104        /* float, fraction of the build REMAINING      */
#define U_HEALTH      0x108        /* short                                       */
#define U_KILLS       0xB8         /* u16                                         */
#define U_TYPEWORD    0xA6         /* u16 the def index; 0 = a free slot (the
                                      allocator's test at 0x486036)               */
#define U_STATE       0x110        /* alive 0x10000000; 0x20 is written below for  */
                                   /* a nanoframe but the DRAW path never reads it */
                                   /* — under construction is +0x104 alone, and    */
                                   /* 0x20000000 is the STRUCTURE bit              */
                                   /* stance (0xC0000)>>18: 0 hold 1 manv 2 roam  */
#define U_FIRE_MASK   0x00300000u  /* +0x110 bits 20-21: the fire state, 0 hold,
                                      1 return, 2 at will (Standing_FireOrder's
                                      store 0x403123; engine/units.md)            */
#define U_WSLOT       0x04         /* three weapon slots, stride 0x1C: +0x8 aim   */
#define WS_STRIDE     0x1C         /*   complete, +0x16/+0x18 aim heading, pitch  */
#define WS_AIMED      0x08         /*   (engine/weapons.md, units.md)             */
#define WS_AIMHEAD    0x16
#define WS_AIMPITCH   0x18
#define WS_WEAPON     0x0C         /* WeaponDef*, written only by the slot init   */
#define WS_STOCK      0x1A         /* u8 the stockpile count; slot 0's is +0x1E   */
#define WS_FLAGS      0x1B         /* bit 4 set at init: the slots Standing_Fire-
                                      Order clears (0x40313F)                     */
#define WD_FLAGS      0x111        /* u32 the weapon's booleans (engine/weapons.md) */
#define WD_STOCKPILE  0x10000000u  /*   bit 28 `stockpile` (0x42EA5F)             */
#define U_SUBORDERS   0x60         /* the second order list: BuildWeapon's nodes  */
#define ON_ACTION     0x04         /* an order node's action byte                 */
#define ON_P1         0x36         /*   its first parameter: BuildWeapon's slot   */
#define ON_COUNT      0x3A         /*   BuildWeapon's missiles still to build     */
#define ON_NEXT       0x4A         /*   the next node (engine/orders.md)          */
#define SCN_LIST_MAX  256          /* nodes walked of one list, at most           */
#define U_LASTHIT     0xF5         /* u8 the kind of the last hit                 */
#define U_WATER       0x10A        /* u32 the water state 0..4 (0x43DB50)         */
#define U_STATE_KEPT  0x0000B0C0u  /* +0x110 bits 6, 7, 12, 13, 15: every store
                                      the create makes keeps them (fresh_unit)    */
#define UO_POS        0x22         /* Position_Dword Pos in UnitOrdersStruct      */

/* Engine calls. Every address is from the merged community corpus
   (tools/ta_symbols.txt) and every signature from the vendored TADR corpus; the
   recipes below are TADR's own, which is the prior art that settles feasibility.

   UNITS_CreateUnit and ORDERS_NewMainOrder2Unit speak the SAME language: 16.16
   fixed-point positions in the 3-D convention (x, altitude, depth). There is no
   asymmetry, whatever TADR's ConstructionKickout suggests (whole world units,
   {x, depth, altitude}), and the read-back probe below cannot settle it because
   the constructor 0x43A0C0 copies the caller's three dwords verbatim into
   UnitOrders->Pos (0x43A164..), so anything written reads back unchanged. What settles the scale is the duplicate-order test inside
   0x43AFC0: `sub ebp,[esi+0x22]; add ebp,0x100000; cmp ebp,0x200000` at
   0x43B006 -- a tolerance of +/-0x100000, which is +/-16.0 in 16.16, one map
   cell. It compares components 0 and 2 (0x22 and 0x2A) and never component 1,
   i.e. the ground plane and not the altitude. Measured 2026-09-04: with whole
   units every ordered unit walked to the map origin (1900 >> 16 == 0), aircraft
   included; with the shift they go where the file says. */
typedef void* (__stdcall* PFN_CREATE)(int owner, int typeIdx, int x, int alt, int depth,
                                      int fullHp, unsigned stateMask, int unitNumber);
typedef int   (__stdcall* PFN_KILL)(void* unit, unsigned mode);
typedef short (__stdcall* PFN_FEATNAME2ID)(const char* name);
typedef void* (__stdcall* PFN_GRIDPLOT)(int gridX, int gridZ);
typedef void* (__stdcall* PFN_SPAWNFEAT)(void* gridPlot, short defIdx, const void* pos,
                                         const void* turn, unsigned char playerId);
typedef char* (__stdcall* PFN_TYPE2INDEX)(int* rtnIndex, unsigned actionId, void* unit,
                                          void* target, const void* pos);
typedef void  (__stdcall* PFN_ORDER)(int actionIndex, int shift, void* unit, void* target,
                                     const void* pos, int p1, int p2);

#define UNITS_CreateUnit ((PFN_CREATE)      0x00485F50u)
#define UNITS_KillUnit   ((PFN_KILL)        0x004864B0u)
#define FeatureName2ID   ((PFN_FEATNAME2ID) 0x00422DD0u)
#define GetGridPosPLOT   ((PFN_GRIDPLOT)    0x00481550u)
#define SpawnFeatureOnMap ((PFN_SPAWNFEAT)  0x00423C50u)
#define ScriptAction_Type2Index ((PFN_TYPE2INDEX) 0x0043F0E0u)
#define ORDERS_NewMainOrder2Unit ((PFN_ORDER) 0x0043AFC0u)

/* 0x424840, the BurnAll walker: no argument, a plain `ret` at 0x42488D. It walks the
   main+0x14233 x main+0x14237 cells of the feature grid (stride 0xD from main+0x14287) and
   calls FEATURES_Destroy 0x4246B0(cell, 1) -- stdcall, the call at 0x42487A has no `add esp`
   after it -- on every cell whose def word +8 is below 0xFFFB or equal to 0xFFFE
   (DISASSEMBLED 0x424840..0x42488D). Its one caller is the console's BurnAll 0x416390. */
typedef void (__cdecl* PFN_BURNALL)(void);
#define FEATURES_BurnAll ((PFN_BURNALL) 0x00424840u)
/* 0x463C60, the controller setter: thiscall on a player record, `ret 4`. It stores the
   argument at +0x73 and, unless it is 3, at PlayerInfo +0x94 through +0x27
   (DISASSEMBLED 0x463C60..0x463C75). */
/* The engine calls the weapon keys make, each DISASSEMBLED:
   - ClearTarget 0x48A0F0, stdcall(unit, slot) `ret 8`: returns at once when the slot's target
     +0x0 is already none, (0, 0x8000) (0x48A106..0x48A116); else stores none and starts COB
     TargetCleared. Standing_FireOrder 0x403100 calls it for each slot whose +0x1B has bit 4 when
     the new state is 0 or 1 (0x40312C..0x403150).
   - 0x41C150, stdcall(unit) `ret 4`: the build menu's relabel of a stock, when the menu shows this
     unit (main+0x37E9C against +0xA8); BuildWeapon (0x402BC3) and AutoAim (0x49E45E) call it after
     each change of a stock byte. It writes label buffers only (engine/weapons.md).
   - 0x438760, thiscall(out, name) `ret 4`: an action name's index, stored as a byte into *out,
     0 when no descriptor has the name (0x438819).
   - 0x43B0B0, stdcall(action, unit, p1, amount) `ret 0x10`: amount > 0 adds it to the tail node
     of the list the action's descriptor picks when the tail has this action and p1, else appends
     a node with that count; BuildWeapon's descriptor carries 0x40000, so its list is +0x60. The
     build menu's MAKENUKE / MAKEANTI button calls it with p1 0 (0x419B97..0x419BA8). */
typedef void (__stdcall* PFN_CLEARTARGET)(void* unit, int slot);
typedef void (__stdcall* PFN_STOCKSHOWN)(void* unit);
typedef void (__thiscall* PFN_ACTIONBYNAME)(int* out, const char* name);
typedef void (__stdcall* PFN_QUEUE)(int action, void* unit, int p1, int amount);
#define WEAPON_ClearTarget  ((PFN_CLEARTARGET)  0x0048A0F0u)
#define WEAPON_StockShown   ((PFN_STOCKSHOWN)   0x0041C150u)
#define ORDERS_ActionByName ((PFN_ACTIONBYNAME) 0x00438760u)
#define ORDERS_Queue        ((PFN_QUEUE)        0x0043B0B0u)
#define ACT_BUILDWEAPON     ((const char*)      0x00502678u)  /* "BUILDWEAPON", the string the
                                                                 button pushes (0x419B9E) */

typedef void (__thiscall* PFN_SETCONTROLLER)(void* player, unsigned type);
#define PLAYER_SetController ((PFN_SETCONTROLLER) 0x00463C60u)
/* 0x4904B0, the objective's disarm: `mov dword [ecx+0x88],0; ret` (thiscall) */
typedef void (__thiscall* PFN_DISARM)(void* objective);
#define OBJECTIVE_Disarm ((PFN_DISARM) 0x004904B0u)

/* The tick site: `mov eax, ds:0x511DE8`, five position-independent bytes inside
   the straight-line block that 0x4969CB (TADR's GameTickHook address) enters. */
#define TICK_SITE     0x004969D2u
static const unsigned char TICK_STOLEN[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };

/* ------------------------------------------------------------------ the arena */

/* a scenario can fill the largest game the installed limit allows, ten players at
   TAGPU_LIM_UNITS, give each of those units an order, and clear a game that full */
#define SCN_MAX_UNITS   (10 * TAGPU_LIM_UNITS)
#define SCN_MAX_FEATS   1024
#define SCN_MAX_ORDERS  (10 * TAGPU_LIM_UNITS)
#define SCN_MAX_CLEAR   (10 * TAGPU_LIM_UNITS + 1)
#define SCN_FILEMAX     (4u * 1024u * 1024u)

/* The wire as the shared core parsed it (src/tasim_wire.h). The applier writes into it
   too: resolve_setup drops a refused key by clearing it, and an order whose `after=` it
   refuses gets after -1. */
static TASIM_WIRE_UNIT  g_wunits[SCN_MAX_UNITS];
static TASIM_WIRE_FEAT  g_wfeats[SCN_MAX_FEATS];
static TASIM_WIRE_ORDER g_worders[SCN_MAX_ORDERS];
static TASIM_WIRE       g_wire = { g_wunits, SCN_MAX_UNITS, g_wfeats, SCN_MAX_FEATS,
                                   g_worders, SCN_MAX_ORDERS };

/* The parse's errors and then the applier's, in one list: the result's `errors`. */
static TASIM_WIRE_ERRS  g_errs;
#define scn_err(...) tasim_wire_err(&g_errs, __VA_ARGS__)

/* What the applier made of each wire unit and feature, by ordinal: zeroed with defIdx -1
   after every parse (results_reset). */
typedef struct {
    int   defIdx;                                 /* resolved UNITINFO index     */
    void* unit;                                   /* the created UnitStruct*     */
    int   ax, ay, ah;                             /* where it actually landed    */
    int   engineIdx;
    int   ok;
    int   fire_got, stock_got;                    /* read back after dress_unit wrote them,
                                                     -1 not written                        */
    int   queued;                                 /* buildweapon: slot 0's count queued on
                                                     +0x60 after the order, -1 none asked  */
} scn_unit;

typedef struct {
    int   defIdx;
    void* feat;
    int   ax, ay, ah;
    int   ok;
} scn_feat;

static scn_unit  g_units[SCN_MAX_UNITS];
static scn_feat  g_feats[SCN_MAX_FEATS];
static char*     g_clearlist[SCN_MAX_CLEAR];
static int       g_nclear;

/* what the fields read back the instant after the write, still on the same tick */
static int      g_pl_got_metal[10], g_pl_got_energy[10];
static int      g_pl_ai_done[10];
static float    g_pl_got_mstore[10], g_pl_got_estore[10];
static int      g_fresh_reset;                    /* the apply may reset what a level
                                                     load leaves: the seats' pass
                                                     results (reset_pass_results),
                                                     their weapon scans
                                                     (reset_weapon_scans), what a
                                                     created unit inherits from its
                                                     slot (fresh_unit)             */

/* why this apply refuses the calibration keys, or 0 (resolve_setup); the tick's alone */
static const char* g_keys_refused;

/* results filled by the apply pass */
static int      g_applied, g_failed, g_cleared, g_ord_ok, g_ord_fail;
static unsigned g_sw_before, g_sw_after;
static int      g_gametime, g_cap, g_limit_live, g_perplayer_live;
static int      g_cam_x, g_cam_y, g_cam_h, g_cam_have, g_eye_x, g_eye_y, g_eye_have;
static int      g_probe_have, g_probe_pass[3], g_probe_stored[3];
static int      g_feat_cleared, g_objective_done, g_delayed_result;
static int      g_sched_result, g_sched_ord_result;   /* creates and their orders left
                                                         scheduled by the apply (sched_add) */
/* the slots the clear emptied: a runner's survivors, each one's death image its state at the
   end of the last step */
static int      g_cleared_slot[SCN_MAX_CLEAR], g_ncleared_slots;

/* live map extents, reloaded every apply */
static int   map_w, map_h, fmap_x, fmap_y;
static char* fmap;

/* IDLE -> ARMED -> APPLYING -> DONE -> IDLE. The present path owns every
   transition but ARMED->APPLYING and APPLYING->DONE, which the tick owns. */
#define ST_IDLE     0
#define ST_ARMED    1
#define ST_APPLYING 2
#define ST_DONE     3
static volatile LONG g_state = ST_IDLE;

static int g_hooked;                              /* the detour is installed once */
static unsigned g_frame;                          /* stamped by the present path  */
static unsigned g_armed_at;                       /* frame the arena went ARMED   */

/* How long the present path waits for Game_MainLoopTick to reach the detour.
   It never does at the menus or on the mission-end screen, and a silent CLI
   timeout tells nobody which of those it was. A pause does not stop it, by the
   disassembly: the pause test (0x496920..0x496926) jumps over only the
   scheduler 0x495230 and the tick 0x495490, a network game's paused branch
   (0x4968AA..0x4968B0) rejoins at 0x496969, and every path through the frame
   callback 0x496790 falls through to the draw call at 0x4969CD and so to
   0x4969D2. MEASURED 2026-10-09: a test launch held at GameTime 1 (tagpu_replay.c,
   THE LAUNCH'S HOLD) applied resource-mex's 8 units on paused frames. */
#define SCN_ARM_FRAMES 600u

/* ------------------------------------------------------------------ utilities */

static void scnlog(const char* s)
{
    tagpu_log(s);
}

static void scnlogf(const char* fmt, ...)
{
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b - 1, fmt, ap);
    b[sizeof b - 1] = 0;
    va_end(ap);
    scnlog(b);
}

/* Committed, not guard/no-access. A wrong pointer must cost a log line, never
   the process — same rule as tagpu_peek, tagpu_ui and tagpu_cat. */
static BOOL readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const char* c = (const char*)p;

    if (!p)
        return FALSE;

    while (n)
    {
        size_t span;

        if (!VirtualQuery(c, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return FALSE;

        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return FALSE;

        span = (size_t)((const char*)mbi.BaseAddress + mbi.RegionSize - c);

        if (span >= n)
            return TRUE;

        n -= span;
        c += span;
    }

    return TRUE;
}

/* TA strings are fixed-size, NUL-padded and CP437-ish. Emit strict JSON: escape
   the two mandatory characters and \u-escape anything outside printable ASCII. */
static void jstr(FILE* f, const char* src, size_t cap)
{
    size_t i;

    fputc('"', f);

    for (i = 0; i < cap && src[i]; i++)
    {
        unsigned char c = (unsigned char)src[i];

        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c >= 0x20 && c < 0x7F)
            fputc((char)c, f);
        else
            fprintf(f, "\\u%04X", c);
    }

    fputc('"', f);
}

static char* ta_base(void)
{
    char* ta = *(char**)TA_MAINPP;
    return readable(ta, OFF_GAMETIME + 4) ? ta : 0;
}

/* the wire, its errors and every result, to an empty apply's */
static void arena_reset(void)
{
    int i;

    tasim_wire_reset(&g_wire);
    tasim_wire_errs_reset(&g_errs);
    for (i = 0; i < 10; i++)
    {
        g_pl_got_metal[i] = g_pl_got_energy[i] = TASIM_WIRE_UNSET;
        g_pl_got_mstore[i] = g_pl_got_estore[i] = -1.0f;
        g_pl_ai_done[i] = 0;
    }
    g_feat_cleared = 0;
    g_objective_done = -1;
    g_delayed_result = 0;
    g_sched_result = g_sched_ord_result = 0;
    g_ncleared_slots = 0;
    g_applied = g_failed = g_cleared = g_ord_ok = g_ord_fail = 0;
    g_nclear = 0;
    g_sw_before = g_sw_after = 0;
    g_gametime = 0; g_cap = 0; g_limit_live = 0; g_perplayer_live = 0;
    g_cam_x = g_cam_y = g_cam_h = g_cam_have = 0;
    g_eye_x = g_eye_y = g_eye_have = 0;
    /* the map statics too: a parse failure reports before any apply reads them,
       and a result carrying the previous game's extents would be a lie */
    map_w = map_h = fmap_x = fmap_y = 0;
    fmap = 0;
    g_probe_have = 0;
}

/* each parsed unit's and feature's result, before the applier fills any */
static void results_reset(void)
{
    int i;

    for (i = 0; i < g_wire.nunits; i++)
    {
        memset(&g_units[i], 0, sizeof g_units[i]);
        g_units[i].defIdx = -1;
        g_units[i].fire_got = g_units[i].stock_got = g_units[i].queued = -1;
    }

    for (i = 0; i < g_wire.nfeats; i++)
    {
        memset(&g_feats[i], 0, sizeof g_feats[i]);
        g_feats[i].defIdx = -1;
    }
}

/* ----------------------------------------------------------------- the applier
   Everything below runs on the Game_MainLoopTick detour. No file I/O, no CRT
   allocation: this is the sim's own thread and the whole point is one visit. */

static void load_map_extents(char* ta)
{
    map_w  = *(int*)(ta + OFF_MAPPXW);
    map_h  = *(int*)(ta + OFF_MAPPXH);
    fmap_x = *(int*)(ta + OFF_FMAPX);
    fmap_y = *(int*)(ta + OFF_FMAPY);
    fmap   = *(char**)(ta + OFF_FMAP);
}

/* TADR's own height rule: feature cells are 16 world units, and the cell's
   `height` byte is the ground the engine would snap a unit to. */
static int ground_at(int x, int y)
{
    int gx = x / 16, gy = y / 16;
    long idx;

    if (!fmap || fmap_x <= 0 || fmap_y <= 0)
        return 0;

    if (gx < 0 || gy < 0 || gx >= fmap_x || gy >= fmap_y)
        return 0;

    idx = (long)gx + (long)gy * fmap_x;

    if (!readable(fmap + idx * FM_STRIDE, FM_STRIDE))
        return 0;

    return *(unsigned char*)(fmap + idx * FM_STRIDE + FM_HEIGHT);
}

static int in_map(int x, int y)
{
    return x >= 0 && y >= 0 && x < map_w && y < map_h;
}

static char* player_at(char* ta, int slot)
{
    char* pl;

    if (slot < 0 || slot > 9)
        return 0;

    pl = ta + OFF_PLAYERS + (size_t)slot * PL_STRIDE;
    return readable(pl, PL_STRIDE) ? pl : 0;
}

/* The absolute slot index of position p of the owner's block, the index 0x485F50's eighth argument
   asks for; 0 when it is not one. The create forms the slot as base + 0x118 x index and takes it only
   inside the block [+0x67, +0x6B] (0x48600B..0x486030, DISASSEMBLED); this asks the same, plus the
   slot count main+0x14351 and the bitsets' bound SCN_SLOT_MAX, from values that are DATA: the block's
   ends are bounded to the array and its stride before any index is formed. 0 is never a block's
   (OFF_USLOTS), and is what the create reads as "the first free slot". */
#define SCN_SLOT_MAX 20000                /* snapshot_existing's own bound on the array */

static unsigned slot_index(char* ta, int owner, int p)
{
    char*    pl = player_at(ta, owner);
    char*    beg = *(char**)(ta + OFF_BEGIN);
    char*    end = *(char**)(ta + OFF_END);
    unsigned count = *(unsigned short*)(ta + OFF_USLOTS);
    char*    first;
    char*    last;
    size_t   idx;

    if (!pl || p < 0 || !beg || end < beg)
        return 0;
    first = *(char**)(pl + PL_FIRSTUNIT);
    last  = *(char**)(pl + PL_LASTUNIT);
    if (first < beg || last > end || last < first || (size_t)(first - beg) % UNIT_STRIDE ||
        (size_t)(last - beg) % UNIT_STRIDE || (size_t)p > (size_t)(last - first) / UNIT_STRIDE)
        return 0;
    idx = (size_t)(first - beg) / UNIT_STRIDE + (size_t)p;
    return idx && idx < count && idx <= SCN_SLOT_MAX ? (unsigned)idx : 0;
}

/* Degrees, 0 = TA's own default build facing (heading word 0x8000), increasing
   counter-clockwise from above over a full circle of 0x10000. */
static unsigned heading_of(int degrees)
{
    long h = 0x8000L + (long)degrees * 0x10000L / 360L;
    return (unsigned)(h & 0xFFFF);
}

static int unit_type_index(char* ta, const char* name)
{
    unsigned count = *(unsigned*)(ta + OFF_UCOUNT);
    char*    defs  = *(char**)(ta + OFF_UDEFS);
    unsigned i;

    if (!defs || count > 16384u)
        return -1;

    for (i = 0; i < count; i++)
    {
        const char* rec = defs + (size_t)i * UD_STRIDE;

        if (!readable(rec, UD_STRIDE))
            return -1;

        if (!lstrcmpiA(rec + UD_NAME, name))
            return (int)i;
    }

    return -1;
}

/* The level's objective, or NULL. LIFETIME: two places free it, both on the game thread and
   neither inside the tick's visit (DISASSEMBLED): the `.ota` reader 0x435DA0, which frees the
   old one as it makes the next at a level's setup, and the gaming state's destructor
   0x434FF0, which frees main+0x391ED (0x43500E) and nulls it (0x43501C), called from
   SetGameMode 0x434AB0 (0x434AD6) and 0x434B90 (0x434BCF) at a game-mode change. So the
   pointer read here, at the apply visit, is this level's own for the whole visit. */
static char* level_objective(char* ta)
{
    char* obj;

    if (!readable(ta + OFF_OBJECTIVE, 4))
        return 0;

    obj = *(char**)(ta + OFF_OBJECTIVE);
    return readable(obj, OBJ_ARMED + 4) ? obj : 0;
}

/* The calibration keys -- clearfeat, objective, ai=off, the storages, an order's after= and a
   unit's at= and slot= -- write engine state no other machine sees: in a network game the peers
   would not see the objective, the controller or the delayed orders, and a requested slot
   bypasses the slot hold the peers' hits rely on (tagpu_patches.c, hit_take); on a puppet the
   recorder alone
   writes the state the recording carries. So each is refused there, as an error naming the
   key, which the wire's onerror then handles like any other. The rest of a seat's tests
   are the console's own AI command 0x416280 (seat below 10, the record active, +0x146 not
   10, +0x73 in 1..3; DISASSEMBLED), narrowed to an AI seat (type 2) and never the
   controlled seat. Each refused key is dropped, so `onerror=skip` applies the rest. */
static int resolve_setup(char* ta)
{
    const char* why = 0;
    int         bad = 0, i;

    if (ta[OFF_NETWORK] & 1)
        why = "a network game: the other peers would not see it";
    else if (tagpu_puppet_on())
        why = "a puppet: the recorder is the only writer of the recording's state";

    /* asked by no key, so withheld without an error where the keys are refused */
    g_fresh_reset = !why;
    g_keys_refused = why;

    if (why)
    {
        if (g_wire.clearfeat)
        {
            scn_err("clear_features refused: %s", why);
            g_wire.clearfeat = 0;
            bad++;
        }
        if (g_wire.objective >= 0)
        {
            scn_err("objective refused: %s", why);
            g_wire.objective = -1;
            bad++;
        }
        for (i = 0; i < g_wire.npl; i++)
        {
            if (g_wire.pl[i].aioff)
            {
                scn_err("player %d: ai off refused: %s", g_wire.pl[i].slot, why);
                g_wire.pl[i].aioff = 0;
                bad++;
            }
            if (g_wire.pl[i].hasstore)
            {
                scn_err("player %d: storage refused: %s", g_wire.pl[i].slot, why);
                g_wire.pl[i].hasstore = 0;
                bad++;
            }
        }
        for (i = 0; i < g_wire.norders; i++)
            if (g_wire.orders[i].after > 0 || g_wire.orders[i].cmd == TASIM_CMD_BUILDWEAPON)
            {
                scn_err("order %d: %s refused: %s", i,
                        g_wire.orders[i].cmd == TASIM_CMD_BUILDWEAPON ? "buildweapon" : "after", why);
                g_wire.orders[i].after = -1;
                bad++;
            }
        for (i = 0; i < g_wire.nunits; i++)
        {
            if (g_wire.units[i].fire != TASIM_WIRE_UNSET)
            {
                scn_err("unit %d: fire refused: %s", i, why);
                g_wire.units[i].fire = TASIM_WIRE_UNSET;
                bad++;
            }
            if (g_wire.units[i].stock != TASIM_WIRE_UNSET)
            {
                scn_err("unit %d: stock refused: %s", i, why);
                g_wire.units[i].stock = TASIM_WIRE_UNSET;
                bad++;
            }
            /* resolve_all leaves such a unit unresolved: made at the apply, or in the first free
               slot, it would be another situation than the wire's */
            if (g_wire.units[i].at || g_wire.units[i].slot != TASIM_WIRE_UNSET)
            {
                scn_err("unit %d: at= and slot= refused: %s", i, why);
                bad++;
            }
        }
        return bad;
    }

    if (g_wire.objective >= 0 && !level_objective(ta))
    {
        scn_err("objective: no objective in this level");
        g_wire.objective = -1;
        bad++;
    }

    for (i = 0; i < g_wire.npl; i++)
    {
        int   s  = g_wire.pl[i].slot;
        char* pl = player_at(ta, s);

        if ((g_wire.pl[i].aioff || g_wire.pl[i].hasstore) && (!pl || !*(int*)(pl + PL_ACTIVE)))
        {
            scn_err("player %d is not in this game: its ai and storage keys are dropped", s);
            g_wire.pl[i].aioff = g_wire.pl[i].hasstore = 0;
            bad++;
            continue;
        }

        if (g_wire.pl[i].aioff &&
            (*(unsigned char*)(pl + PL_SELF) == 10 || *(unsigned char*)(pl + PL_TYPE) != 2 ||
             s == *(unsigned char*)(ta + OFF_WATCHED)))
        {
            scn_err("player %d: ai off refused: not an AI seat other than the controlled one "
                    "(type %d)", s, *(unsigned char*)(pl + PL_TYPE));
            g_wire.pl[i].aioff = 0;
            bad++;
        }
    }

    return bad;
}

/* Whether a weapon def is a stockpile weapon: `w` is DATA, a slot's or a def's pointer, so it is
   range-tested before its flag word is read. BuildWeapon reads its weapon's reload time with no test
   (0x402BDE) and builds for whatever slot 0 holds (engine/orders.md), so the applier asks this before every
   stock write and every buildweapon order: a unit with no weapon in slot 0 would make it read through
   NULL on its first step. */
static int stockpile_weapon(const char* w)
{
    return w && readable(w + WD_FLAGS, 4) && (*(const unsigned*)(w + WD_FLAGS) & WD_STOCKPILE);
}

/* Layer 3: resolve everything against the LIVE game before creating anything.
   The CLI already checked these names against a catalogue, but the file is on
   disk and can be stale — a mod reload, a different game, an edited wire. */
static int resolve_all(char* ta)
{
    int bad = resolve_setup(ta), i;

    for (i = 0; i < g_wire.nunits; i++)
    {
        const TASIM_WIRE_UNIT* wu = &g_wire.units[i];
        scn_unit* u = &g_units[i];

        u->defIdx = unit_type_index(ta, wu->type);

        if (u->defIdx < 0)
        {
            scn_err("unit %d: no unit type named '%.20s' in this game", i, wu->type);
            u->ok = 0;
            bad++;
            continue;
        }

        if (!in_map(wu->x, wu->y))
        {
            scn_err("unit %d (%.20s): %d,%d is outside the map (%dx%d)",
                    i, wu->type, wu->x, wu->y, map_w, map_h);
            bad++;
            continue;
        }

        /* the def's slot 0 weapon is the one the slot init copies (0x49E0B0); defIdx is
           bounded by the def count (unit_type_index) */
        if (wu->stock != TASIM_WIRE_UNSET &&
            !stockpile_weapon(*(const char**)(*(char**)(ta + OFF_UDEFS) +
                                              (size_t)u->defIdx * UD_STRIDE + UD_WEAPON0)))
        {
            scn_err("unit %d (%.20s): stock wants a stockpile weapon in slot 0", i, wu->type);
            bad++;
            continue;
        }

        if (!player_at(ta, wu->owner))
        {
            scn_err("unit %d (%.20s): player slot %d does not exist",
                    i, wu->type, wu->owner);
            bad++;
            continue;
        }

        if (!*(int*)(player_at(ta, wu->owner) + PL_ACTIVE))
        {
            scn_err("unit %d (%.20s): player slot %d is not in this game",
                    i, wu->type, wu->owner);
            bad++;
            continue;
        }

        /* The create files every unit into its owner's control groups: 0x480250, from the
           create's init 0x485A40 (0x485D29), indexes the record's +0x78 without a NULL test
           (0x4802DA, 0x4802DD), so a unit for a record without one crashes there. The table's
           one allocator is 0x480190 (0x4801B1), called only by the record init 0x464700
           (0x464898), which the level start runs for a seated record, +0x73 non-zero
           (0x4649A5..0x4649AE); the teardown 0x464A00 frees and nulls it (0x480239).
           DISASSEMBLED.
           The table's pointer is the safety argument: it is what the create reads. The +0x73
           test is the engine's seat test and stricter than the create needs; where the two
           disagree the pointer test still stands on its own. A record's type changes in play
           without the table changing: 0x463C60 is handed 0 at 0x447CC2 and 0x447CFA in the
           network code, leaving the table allocated [INFERRED: that these are a peer's leave],
           and the join sets 3 (0x450B16) on a record the level start may not have seated. */
        if (!*(unsigned char*)(player_at(ta, wu->owner) + PL_TYPE) ||
            !*(char**)(player_at(ta, wu->owner) + PL_GROUPS))
        {
            scn_err("unit %d (%.20s): player slot %d has no seat in this game",
                    i, wu->type, wu->owner);
            bad++;
            continue;
        }

        if (g_keys_refused && (wu->at || wu->slot != TASIM_WIRE_UNSET))
        {
            bad++;                           /* resolve_setup said why */
            continue;
        }

        /* A requested slot must be one of the owner's block and hold no unit now: one the clear is
           about to empty is refused at its create anyway (sched_slot_ok), and one a live unit keeps
           can never be had. The create's own test is made again when it runs. */
        if (wu->slot != TASIM_WIRE_UNSET)
        {
            unsigned idx = slot_index(ta, wu->owner, wu->slot);
            char*    rec = idx ? *(char**)(ta + OFF_BEGIN) + (size_t)idx * UNIT_STRIDE : 0;

            if (!idx || !readable(rec, UNIT_STRIDE))
            {
                scn_err("unit %d (%.20s): slot %d is not a position of player %d's unit block",
                        i, wu->type, wu->slot, wu->owner);
                bad++;
                continue;
            }
            if (*(unsigned short*)(rec + U_TYPEWORD))
            {
                scn_err("unit %d (%.20s): slot %d of player %d (index %u) holds a unit", i,
                        wu->type, wu->slot, wu->owner, idx);
                bad++;
                continue;
            }
        }

        u->ok = 1;
    }

    for (i = 0; i < g_wire.nfeats; i++)
    {
        const TASIM_WIRE_FEAT* wf = &g_wire.feats[i];
        scn_feat* fe = &g_feats[i];

        /* Only a definition the level loaded: LoadFeature 0x4224B0 searches the TDF index
           [0x511FB4], which the level load frees and zeroes (0x4223E0 at 0x4918DE; 0x422452)
           before any scenario runs, so from a live game it reads through NULL at 0x4224BB.
           The recorder relies on the same fact: only a level load grows the definitions. */
        fe->defIdx = FeatureName2ID(wf->type);

        if (fe->defIdx < 0)
        {
            scn_err("feature %d: no feature named '%.20s' in this game", i, wf->type);
            bad++;
            continue;
        }

        if (!in_map(wf->x, wf->y))
        {
            scn_err("feature %d (%.20s): %d,%d is outside the map (%dx%d)",
                    i, wf->type, wf->x, wf->y, map_w, map_h);
            bad++;
            continue;
        }

        fe->ok = 1;
    }

    /* The engine's per-player cap refuses silently, one unit at a time, so 200 units over the
       line means 200 identical error lines and no diagnosis. Count here instead what each
       owner's creates can take: the free slots of its block [+0x67, +0x6B] that a create in this
       tick may take. THE HOLD (tagpu_limits.h; tagpu_patches.c hit_take, hit_held) never gives
       a create a slot freed in the same tick, so the slots this apply's clear frees -- every
       live unit, snapshot_existing -- are held from the creates that follow it, as are slots
       freed earlier in this tick; with the hold off they are free again. The apply's first-free
       creates (UNITS_CreateUnit's arg 8 is 0) run in wire order, each taking one slot of its
       owner's block, so an owner fits exactly when they number no more than these. A create that
       asks for a slot, or runs after the apply, is judged at its create (sched_slot_ok,
       sched_watch); a first-free create that takes a slot a later one asks for leaves that one
       refused by name. */
    {
        int want[10];
        char* beg = *(char**)(ta + OFF_BEGIN);
        char* end = *(char**)(ta + OFF_END);
        int array_ok = readable(beg, UNIT_STRIDE) && end >= beg &&
                       (size_t)(end - beg) <= (size_t)UNIT_STRIDE * 20000;
        int hold = tagpu_unit_hold_on();

        for (i = 0; i < 10; i++)
            want[i] = 0;

        for (i = 0; i < g_wire.nunits; i++)
            if (g_units[i].ok && g_wire.units[i].owner >= 0 && g_wire.units[i].owner < 10 &&
                !g_wire.units[i].at && g_wire.units[i].slot == TASIM_WIRE_UNSET)
                want[g_wire.units[i].owner]++;

        for (i = 0; i < 10; i++)
        {
            char* pl = want[i] ? player_at(ta, i) : 0;
            char *first, *last, *u;
            int   block, freed = 0, held = 0, cleared = 0, occupied = 0, room;

            if (!want[i])
                continue;

            /* the block's ends are DATA: bounded to the array and its stride before the walk */
            first = pl ? *(char**)(pl + PL_FIRSTUNIT) : 0;
            last  = pl ? *(char**)(pl + PL_LASTUNIT) : 0;
            if (!array_ok || !first || first < beg || last < first || last > end ||
                (size_t)(first - beg) % UNIT_STRIDE || (size_t)(last - beg) % UNIT_STRIDE)
            {
                scn_err("player %d: its unit block is not one of the unit array's", i);
                bad++;
                continue;
            }
            block = (int)((last - first) / UNIT_STRIDE) + 1;

            for (u = first; u <= last; u += UNIT_STRIDE)
            {
                if (!readable(u, UNIT_STRIDE))
                    break;
                if (*(unsigned short*)(u + U_TYPEWORD))
                {
                    if (g_wire.clear && (*(unsigned*)(u + U_STATE) & 0x10000000u))
                        cleared++;
                    else
                        occupied++;
                }
                else if (tagpu_unit_slot_takeable((unsigned)((u - beg) / UNIT_STRIDE)))
                    freed++;
                else
                    held++;
            }

            room = freed + (hold ? 0 : cleared);
            if (want[i] > room)
            {
                scn_err("player %d: %d units to create and %d slots to take them -- its block of %d "
                        "has %d occupied, %d slots held by the clear and %d freed earlier in this "
                        "tick; a create takes no slot freed in its own tick%s",
                        i, want[i], room, block, occupied, hold ? cleared : 0, held,
                        want[i] + occupied > block ? " (raise UnitLimit before the game starts)"
                                                   : "");
                bad++;
            }
        }
    }

    for (i = 0; i < g_wire.norders; i++)
    {
        TASIM_WIRE_ORDER* o = &g_wire.orders[i];

        /* Only an ordinal can be checked here. A live subject names a slot in
           the running game, so it is resolved — and its type guard tested — at
           the apply point, on the game thread, where the answer cannot go stale
           between the check and the order. */
        if (o->subj == TASIM_SUBJ_ORD && (o->uord < 0 || o->uord >= g_wire.nunits))
        {
            scn_err("order %d: unit ordinal %d does not exist", i, o->uord);
            bad++;
            continue;
        }

        if (o->kind == TASIM_TGT_UNIT && !o->tlive && (o->a < 0 || o->a >= g_wire.nunits))
        {
            scn_err("order %d: target unit ordinal %d does not exist", i, o->a);
            bad++;
        }
        else if (o->kind == TASIM_TGT_FEAT && (o->a < 0 || o->a >= g_wire.nfeats))
        {
            scn_err("order %d: target feature ordinal %d does not exist", i, o->a);
            bad++;
        }
        else if (o->kind == TASIM_TGT_POS && !in_map(o->a, o->b))
        {
            scn_err("order %d: target %d,%d is outside the map (%dx%d)",
                    i, o->a, o->b, map_w, map_h);
            bad++;
        }
        /* a scheduled create's order is issued from what the apply copied (sched_add): a
           position, a unit of the same create tick (the parser's rule), a count or nothing --
           a feature or a live unit would be a watch of its own across the wait */
        else if (o->subj == TASIM_SUBJ_ORD && g_wire.units[o->uord].at &&
                 (o->kind == TASIM_TGT_FEAT || (o->kind == TASIM_TGT_UNIT && o->tlive)))
        {
            scn_err("order %d: unit %d is created %d ticks after the apply, and its orders take a "
                    "position, a unit of its own create tick, a count or no target", i, o->uord,
                    g_wire.units[o->uord].at);
            bad++;
        }
    }

    if (g_wire.cam_kind == TASIM_TGT_UNIT &&
        (g_wire.cam_a < 0 || g_wire.cam_a >= g_wire.nunits))
    {
        scn_err("camera: unit ordinal %d does not exist", g_wire.cam_a);
        bad++;
    }
    else if (g_wire.cam_kind == TASIM_TGT_UNIT && g_wire.units[g_wire.cam_a].at)
    {
        scn_err("camera: unit %d is created %d ticks after the apply, and the camera is placed at the apply",
                g_wire.cam_a, g_wire.units[g_wire.cam_a].at);
        bad++;
    }
    else if (g_wire.cam_kind == TASIM_TGT_FEAT &&
             (g_wire.cam_a < 0 || g_wire.cam_a >= g_wire.nfeats))
    {
        scn_err("camera: feature ordinal %d does not exist", g_wire.cam_a);
        bad++;
    }

    return bad;
}

/* `setup.clear_existing` defaults true: a scenario contains exactly what the file
   says. UNITS_KillUnit mode 0 is the silent path — no explosion, no wreck.

   Two guards, both measured live on 2026-09-01:

   * The sweep runs over a SNAPSHOT of the array, never over the live array, so
     it kills exactly what was there when the scenario arrived and can never
     reach a unit the same tick created. It runs BEFORE the create pass: the
     engine's per-player cap (MaxUnitNumberPerPlayer, 250 in stock TA) counts
     units that are about to die, and creating first costs 302 of a 401-unit
     scenario. Clearing first is safe because the whole apply is one visit to a
     hook OUTSIDE the simulation loop — the sim never sees the empty world.
   * ActiveCommanderDeath is parked at zero for the kills. 0x486688 reads it and
     only calls UNITS_KillAllForPlayer when it is non-zero, so clearing a
     skirmish's starting commanders would otherwise take the whole side with it.

   What this cannot prevent: a scenario that leaves a player with no units at all
   is a defeat, and TA goes to ENDMSN.GUI where Game_MainLoopTick stops. That is
   the engine being right; the applier's job is to say so, which the arm watchdog
   in tagpu_scenario_frame does on the next attempt. */
static void snapshot_existing(char* ta)
{
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    char* u;

    g_nclear = 0;

    if (!readable(beg, UNIT_STRIDE) || end < beg)
        return;

    if ((size_t)(end - beg) > (size_t)UNIT_STRIDE * 20000)
        return;

    for (u = beg; u <= end; u += UNIT_STRIDE)
    {
        if (!readable(u, UNIT_STRIDE))
            break;

        if (!(*(unsigned*)(u + U_STATE) & 0x10000000u))
            continue;

        if (g_nclear >= SCN_MAX_CLEAR)
        {
            scn_err("more than %d units were already alive; the rest stay",
                    SCN_MAX_CLEAR);
            return;
        }

        g_clearlist[g_nclear++] = u;
    }
}

static void clear_snapshot(char* ta)
{
    unsigned saved = *(unsigned*)(ta + OFF_CMDRDEATH);
    char*    beg = *(char**)(ta + OFF_BEGIN);
    int      i;

    *(unsigned*)(ta + OFF_CMDRDEATH) = 0;

    for (i = 0; i < g_nclear; i++)
    {
        char* u = g_clearlist[i];

        if (!readable(u, UNIT_STRIDE) || !(*(unsigned*)(u + U_STATE) & 0x10000000u))
            continue;

        /* snapshot_existing took u from the array on a stride boundary, so this is its slot */
        g_cleared_slot[g_ncleared_slots++] = (int)((u - beg) / UNIT_STRIDE);
        UNITS_KillUnit(u, 0);
        g_cleared++;
    }

    *(unsigned*)(ta + OFF_CMDRDEATH) = saved;
}

/* `clearfeat 1`: every feature of the level goes, through the engine's own BurnAll walker
   0x424840, the console command's (0x416390) -- on the game thread at the apply visit, where
   the console command also runs it [INFERRED: the console handler's context]. Its force 1
   removes indestructible features and geothermal vents too, so a scenario that needs a vent
   cannot use the key. FEATURES_Destroy 0x4246B0 frees each record, sets the cell empty and
   refreshes pathing (0x440A40); it spawns no successor, which comes only from the swap
   0x423710 (engine/map.md). The recorder sees each destroy as a root FR_DESTROY
   (tagpu_recfmt.h). It runs after the unit clear, whose kills leave no wreck (UNITS_KillUnit
   mode 0, clear_snapshot): the wrecks it burns are the ones combat left, in a calibration lane
   the previous run's. */
static void clear_features(void)
{
    FEATURES_BurnAll();
    g_feat_cleared = 1;
}

/* `objective off|on`. off is the engine's own disarm 0x4904B0, on its exact inverse, the
   takeover's write (tagpu_replay.c tk_live): +0x88 = 1. The end tests run only in a step's
   players phase (0x4650C9, 0x4650E1, 0x46516D, 0x46586F; tagpu_replay.c over tk_live), so a
   write here, between two steps, is never seen half-done. resolve_setup refused a NULL
   objective, and level_objective's lifetime holds for the visit. */
static void apply_objective(char* ta)
{
    char* obj = level_objective(ta);

    if (g_wire.objective < 0 || !obj)
        return;

    if (g_wire.objective)
        *(unsigned*)(obj + OBJ_ARMED) = 1;
    else
        OBJECTIVE_Disarm(obj);

    g_objective_done = g_wire.objective;
}

static void apply_switches(char* ta)
{
    unsigned short* sw = (unsigned short*)(ta + OFF_SWITCHES);

    g_sw_before = *sw;
    *sw = (unsigned short)((g_sw_before | g_wire.sw_set) & ~g_wire.sw_clr);
    g_sw_after = *sw;
}

/* Player resources. TA's economy pass 0x401360, once every 30 ticks for each local player,
   recomputes storage from the units a player owns and clamps current to it; current itself
   is carried forward (plus production, less expense), not refilled (0x401AB3..0x401B27,
   DISASSEMBLED). Measured 2026-09-01: asking for 4321 metal against 50 storage, and even for
   12 against the same 50, both read 50 again. The clamp explains the 4321 only; the 12 is
   open (scenario-format.md, phase C).

   The storage keys make a level stick: they are the seat setup's own writes (0x496EE0: the
   bit at 0x496F5D, the floats at 0x496F64..0x496F97; DISASSEMBLED, engine/sim.md) -- +0x149
   bit 0 and the base storages +0xDC (energy) and +0xE0 (metal) -- which the economy pass
   adds to the storages it recomputes (0x401941, 0x401988..0x4019AA). Its only other writers
   are the level setups, the loader, the record's init 0x4647EF and the commander's death:
   the kill 0x4864B0 compares the dying unit's type name with its side's commander name
   (0x4F8A70 at 0x486501) and on a match clears its seat's bit 0 (0x486518; DISASSEMBLED,
   engine/units.md). The apply's own clear kills through 0x4864B0, so apply_now runs this
   AFTER the clear: a skirmish's starting commanders, or a commander a later apply clears,
   would otherwise take the bit with them. The storage writes land before the level writes,
   so a level at or below the base storage survives the clamp.

   The result reports the read-back from the SAME tick beside a second read taken when the
   file is written: requested, wrote, and what survived. */
static void apply_players(char* ta)
{
    int i;

    for (i = 0; i < 10; i++)
    {
        g_pl_got_metal[i] = g_pl_got_energy[i] = TASIM_WIRE_UNSET;
        g_pl_ai_done[i] = 0;
    }

    for (i = 0; i < g_wire.npl; i++)
    {
        char* pl = player_at(ta, g_wire.pl[i].slot);

        if (!pl)
            continue;

        if (g_wire.pl[i].hasstore)
        {
            *(unsigned char*)(pl + PL_BASEFLAG) |= 1;
            *(float*)(pl + PL_BASESTORE_E) = g_wire.pl[i].estore;
            *(float*)(pl + PL_BASESTORE_M) = g_wire.pl[i].mstore;
            if (*(float*)(pl + PL_MAXENERGY) < g_wire.pl[i].estore)
                *(float*)(pl + PL_MAXENERGY) = g_wire.pl[i].estore;
            if (*(float*)(pl + PL_MAXMETAL) < g_wire.pl[i].mstore)
                *(float*)(pl + PL_MAXMETAL) = g_wire.pl[i].mstore;
            g_pl_got_estore[i] = *(float*)(pl + PL_BASESTORE_E);
            g_pl_got_mstore[i] = *(float*)(pl + PL_BASESTORE_M);
        }

        /* The console's AI toggle on an AI seat (0x4162FE): the setter with type 1. The
           players phase runs the stock AI 0x408C40 only for an active record of type 2
           (0x465031), so the seat's units keep their orders and no planner runs; the economy
           pass still runs for type 1, and the difficulty handicap scales type 2 only
           (engine/units.md, the +0x73 row). The recorder records it as RK_CONTROLLER. */
        if (g_wire.pl[i].aioff)
        {
            PLAYER_SetController(pl, 1);
            g_pl_ai_done[i] = 1;
        }

        if (g_wire.pl[i].metal != TASIM_WIRE_UNSET)
        {
            float want = (float)g_wire.pl[i].metal;
            if (*(float*)(pl + PL_MAXMETAL) < want)
                *(float*)(pl + PL_MAXMETAL) = want;
            *(float*)(pl + PL_METAL) = want;
            g_pl_got_metal[i] = (int)*(float*)(pl + PL_METAL);
        }

        if (g_wire.pl[i].energy != TASIM_WIRE_UNSET)
        {
            float want = (float)g_wire.pl[i].energy;
            if (*(float*)(pl + PL_MAXENERGY) < want)
                *(float*)(pl + PL_MAXENERGY) = want;
            *(float*)(pl + PL_ENERGY) = want;
            g_pl_got_energy[i] = (int)*(float*)(pl + PL_ENERGY);
        }
    }
}

static void dress_unit(const TASIM_WIRE_UNIT* wu, scn_unit* u)
{
    char*    p = (char*)u->unit;
    unsigned state = *(unsigned*)(p + U_STATE);
    int      maxhp = *(short*)(p + U_HEALTH);   /* created fullHp: this is the max */

    if (wu->facing != TASIM_WIRE_UNSET)
        *(unsigned short*)(p + U_HEADING) = (unsigned short)heading_of(wu->facing);

    if (wu->stance != TASIM_WIRE_UNSET && wu->stance >= 0 && wu->stance <= 2)
        state = (state & ~0x000C0000u) | ((unsigned)wu->stance << 18);

    if (wu->nano != TASIM_WIRE_UNSET)
    {
        /* `nanoframe` is the percentage BUILT; the engine's field is the fraction
           REMAINING (research/notes/build-state.md), 0.0 = finished. */
        int pct = wu->nano < 0 ? 0 : (wu->nano > 100 ? 100 : wu->nano);
        *(float*)(p + U_NANOFRAME) = (float)(100 - pct) / 100.0f;
        *(short*)(p + U_HEALTH) = (short)(maxhp * pct / 100);
        *(unsigned char*)(p + U_HEALTHPCTA) = (unsigned char)pct;
        *(unsigned char*)(p + U_HEALTHPCTB) = (unsigned char)pct;
        if (pct < 100)
            state |= 0x20u;
        else
            state &= ~0x20u;
    }

    if (wu->health != TASIM_WIRE_UNSET)
    {
        int pct = wu->health < 1 ? 1 : (wu->health > 100 ? 100 : wu->health);
        *(short*)(p + U_HEALTH) = (short)(maxhp * pct / 100);
        *(unsigned char*)(p + U_HEALTHPCTA) = (unsigned char)pct;
        *(unsigned char*)(p + U_HEALTHPCTB) = (unsigned char)pct;
    }

    /* the kill count +0xB8, a u16 every engine reader zero-extends; its
       one increment is the destructor's, so a unit's veterancy is its
       kills, and this sets it without a fight */
    if (wu->kills != TASIM_WIRE_UNSET)
        *(unsigned short*)(p + U_KILLS) =
            (unsigned short)(wu->kills < 0 ? 0 : (wu->kills > 65535 ? 65535 : wu->kills));

    *(unsigned*)(p + U_STATE) = state;

    /* The fire state as Standing_FireOrder 0x403100 writes it: the two bits (0x403123), then for
       hold fire and return fire the clear of every slot with +0x1B bit 4 (0x40312C..0x403150).
       The create's init has just set each slot's target to none (0x48A160 at 0x485C50, k = 0..2)
       and ClearTarget returns at once on none, so the clear changes nothing at a create; it is
       here so that the write is the writer's, whatever the slots hold. */
    if (wu->fire != TASIM_WIRE_UNSET && wu->fire >= 0 && wu->fire <= 2)
    {
        int k;
        *(unsigned*)(p + U_STATE) =
            (*(unsigned*)(p + U_STATE) & ~U_FIRE_MASK) | ((unsigned)wu->fire << 20);
        if (wu->fire <= 1)
            for (k = 0; k < 3; k++)
                if (*(unsigned char*)(p + U_WSLOT + k * WS_STRIDE + WS_FLAGS) & 0x10)
                    WEAPON_ClearTarget(p, k);
        u->fire_got = (int)((*(unsigned*)(p + U_STATE) & U_FIRE_MASK) >> 20);
    }

    /* Slot 0's stock byte, 0..TASIM_WIRE_STOCK_MAX: the slot init stores 0 (0x49E0C8), BuildWeapon
       adds one a missile up to 200 (0x402BB9, the cap tested at 0x402CCA) and AutoAim takes one a
       shot (0x49E45B); each of the two then relabels the build menu (0x41C150), as this does.
       resolve_all asked the def's slot 0 weapon; the slot's own is asked again here. */
    if (wu->stock != TASIM_WIRE_UNSET && wu->stock >= 0 && wu->stock <= TASIM_WIRE_STOCK_MAX &&
        stockpile_weapon(*(const char**)(p + U_WSLOT + WS_WEAPON)))
    {
        *(unsigned char*)(p + U_WSLOT + WS_STOCK) = (unsigned char)wu->stock;
        WEAPON_StockShown(p);
        u->stock_got = *(unsigned char*)(p + U_WSLOT + WS_STOCK);
    }

    u->ax = *(unsigned short*)(p + U_XPOS);
    u->ay = *(unsigned short*)(p + U_YPOS);
    u->ah = *(unsigned short*)(p + U_ZPOS);
    u->engineIdx = *(short*)(p + U_INGAMEIDX);
}

/* A created unit's record as a level load leaves it, where the create does not write it. The
   level fill 0x4854A0 zeroes every record (0x485557) and writes back only +0xA8, +0x92, +0x96,
   +0xFF and +0xAC; the create 0x485F50 then leaves these as the slot's previous unit left them
   [DISASSEMBLED]:
   - each weapon slot's aim complete +0x8 and aim heading and pitch +0x16/+0x18: the slot init
     0x49E070 writes +0xC, +0x10, +0x14, +0x1A and +0x1B only (0x49E099..0x49E11B). The weapon
     code reads them for its own unit.
   - +0xF5, the kind of the last hit: the create writes it, as 7, only for a finished create
     (its sixth argument, tested at 0x48611A..0x48611C) of a def with +0x241 bit 24
     (0x48614D..0x486167); a hit (0x489DAC), a completion (0x41B9FE) and the saved game's restore
     (0x487289) are its other writers.
     The reaper hands it to the kill 0x4864B0, the AI compares it (0x405E13, 0x4071CA, 0x415953).
   - +0x10A, the water state: written only by 0x43DB50 (0x43DBED), which keeps the old value when
     none of its tests holds and runs setSFXoccupy only when the value changes
     (0x43DB65..0x43DBE8), at the unit's first movement pass.
   - +0x110 bits 6, 7, 12, 13 and 15, which all twelve of the create's stores to +0x110 keep
     (0x485A7B..0x486171). Bit 12 is read by the economy pass (0x4017ED, after bit 11 at
     0x4017E1) before the radar pass clears it on every live unit (0x4674B6); the others are
     UI marks (6, 7), unread (13) and a campaign's (15), cleared with it so that the whole word
     follows from the create.
   MEASURED 2026-11-12 on the fifth verification's duel-armpw-corak: a reload run's two units
   began with the dead units' aim (0xFC18/0xF477 and 0xFE72/0xF9BA), aim complete 1 and their
   last hit, where a fresh launch's began with 0. Writing the fill's values right after the
   create, before the unit's first step, and the create's own 7 where it writes one, gives the
   unit what it would hold on a level's first step; `finished` is the argument the create was
   given, so the test follows the create's own.
   The mover's +0x2A needs no write here: the canhover fit reads it inside the create (0x48A6DE),
   before the applier runs, and the mover's constructor has just zeroed it (tagpu_patches.c,
   fix_mover_stamp), in a reload as in a fresh launch (engine/units.md, *The hover wave*).
   BOUNDED: u is the create's own return, checked readable over the stride (create_units); its
   def +0x92 is the engine's type table entry the create just set (0x485A75). */
static void fresh_unit(char* u, int finished)
{
    const char* def = *(const char**)(u + U_UDEF);
    int         i;

    for (i = 0; i < 3; i++)
    {
        char* s = u + U_WSLOT + i * WS_STRIDE;
        *(unsigned*)(s + WS_AIMED) = 0;
        *(unsigned short*)(s + WS_AIMHEAD) = 0;
        *(unsigned short*)(s + WS_AIMPITCH) = 0;
    }
    if (!finished || !readable(def + UD_BITS, 4) ||
        !(*(const unsigned*)(def + UD_BITS) & 0x01000000u))
        *(unsigned char*)(u + U_LASTHIT) = 0;
    *(unsigned*)(u + U_WATER) = 0;
    *(unsigned*)(u + U_STATE) &= ~U_STATE_KEPT;
}

#define SCN_FINISHED 1                    /* the create's sixth argument: a finished unit, which
                                             dress_unit then makes a nanoframe where the wire asks */

static int  sched_slot_ok(char* ta, unsigned idx);
static int  sched_add(int ord, int alt);
static int  sched_add_order(const TASIM_WIRE_ORDER* o);
static void sched_counted(int made);

/* One create: in the first free slot of the owner's block when idx is 0, else in slot idx or not at
   all -- the create returns that slot's record or NULL (0x486036..0x486043) -- and then made the
   wire's (fresh_unit, dress_unit). 0 when the engine refused it, or returned another record. */
static int create_one(const TASIM_WIRE_UNIT* wu, scn_unit* u, int alt, unsigned idx, int fresh)
{
    char* want = idx ? *(char**)(*(char**)TA_MAINPP + OFF_BEGIN) + (size_t)idx * UNIT_STRIDE : 0;

    /* In a network game the create's messages leave at the flush, after dress_unit, so the
       other peers' copies start with the HP and build state it writes (sim-fixes.md B8). */
    tagpu_kill_hold();
    u->unit = UNITS_CreateUnit(wu->owner, u->defIdx, wu->x << 16, alt << 16, wu->y << 16,
                               SCN_FINISHED, 1, (int)idx);

    if (!u->unit || (want && (char*)u->unit != want) || !readable(u->unit, UNIT_STRIDE))
    {
        tagpu_kill_flush();
        u->unit = 0;
        u->ok = 0;
        return 0;
    }

    if (fresh)
        fresh_unit((char*)u->unit, SCN_FINISHED);
    dress_unit(wu, u);
    tagpu_kill_flush();
    return 1;
}

static void create_units(void)
{
    char* ta = *(char**)TA_MAINPP;
    int   i;

    for (i = 0; i < g_wire.nunits; i++)
    {
        const TASIM_WIRE_UNIT* wu = &g_wire.units[i];
        scn_unit* u = &g_units[i];
        int       alt;
        unsigned  idx = 0;

        if (!u->ok)
        {
            g_failed++;
            continue;
        }

        alt = wu->height != TASIM_WIRE_UNSET ? wu->height : ground_at(wu->x, wu->y);

        if (wu->at)
        {
            if (!sched_add(i, alt))
            {
                u->ok = 0;
                g_failed++;
                scn_err("unit %d (%.20s): %d scheduled creates are pending already", i, wu->type,
                        SCN_MAX_UNITS);
            }
            continue;
        }

        if (wu->slot != TASIM_WIRE_UNSET)
        {
            idx = slot_index(ta, wu->owner, wu->slot);
            if (!idx || !sched_slot_ok(ta, idx))
            {
                u->ok = 0;
                g_failed++;
                sched_counted(0);
                scn_err("unit %d (%.20s): slot %d of player %d is not one a create may ask for "
                        "(sched_slot_ok)", i, wu->type, wu->slot, wu->owner);
                continue;
            }
        }

        if (!create_one(wu, u, alt, idx, g_fresh_reset))
        {
            g_failed++;
            if (idx)
            {
                sched_counted(0);
                scn_err("unit %d (%.20s): the engine refused to create it in slot %d of player %d",
                        i, wu->type, wu->slot, wu->owner);
            }
            else
                scn_err("unit %d (%.20s): the engine refused to create it "
                        "(unit limit, or no room at %d,%d)", i, wu->type, wu->x, wu->y);
            continue;
        }

        if (idx)
            sched_counted(1);
        g_applied++;
    }
}

static void create_features(void)
{
    int i;

    for (i = 0; i < g_wire.nfeats; i++)
    {
        const TASIM_WIRE_FEAT* wf = &g_wire.feats[i];
        scn_feat* fe = &g_feats[i];
        int       pos[3], alt;
        unsigned short turn[3];
        void*     plot;

        if (!fe->ok)
        {
            g_failed++;
            continue;
        }

        alt = wf->height != TASIM_WIRE_UNSET ? wf->height : ground_at(wf->x, wf->y);

        /* TPosition is the 3-D convention {x, altitude, depth}, 16.16 fixed point;
           TTurn is {bank, pitch, heading} as words [TA_MemoryStructures.pas]. */
        pos[0] = wf->x << 16;
        pos[1] = alt << 16;
        pos[2] = wf->y << 16;
        turn[0] = 0;
        turn[1] = 0;
        turn[2] = (unsigned short)heading_of(wf->facing != TASIM_WIRE_UNSET ? wf->facing : 0);

        plot = GetGridPosPLOT(wf->x / 16, wf->y / 16);

        if (!plot)
        {
            fe->ok = 0;
            g_failed++;
            scn_err("feature %d (%.20s): no map cell at %d,%d", i, wf->type, wf->x, wf->y);
            continue;
        }

        fe->feat = SpawnFeatureOnMap(plot, (short)fe->defIdx, pos, turn, 10);

        /* A NULL return is not a refusal for a 2-D feature (a tree): 0x423C50 creates a
           feature object only for a def without +0xFE bit 0 (0x423D96) and returns it,
           else 0 (the local zeroed at 0x423C60). Its sprite branch writes the def into the
           anchor cell's +8 at 0x423EE5, so that word is the placement fact. DISASSEMBLED. */
        if (!fe->feat && *(unsigned short*)((char*)plot + 8) != (unsigned short)fe->defIdx)
        {
            fe->ok = 0;
            g_failed++;
            scn_err("feature %d (%.20s): the engine refused to place it at %d,%d",
                    i, wf->type, wf->x, wf->y);
            continue;
        }

        fe->ax = wf->x;
        fe->ay = wf->y;
        fe->ah = alt;
        g_applied++;
    }
}

/* TADR's SendOrder recipe, minus its one dead call: ScriptAction_Index2Handler
   (0x438830) is a pure `base + *ecx * 25` address computation whose result TADR
   discards, so it is not on this path. */
/* ---- naming a unit the wire did not create ---------------------------------

   Both resolvers run at the apply point, on the game thread, so what they
   return cannot be stale by the time the order is issued.

   `live_by_index` is the engine's own index idiom: UnitInGameIndex is the slot
   in the unit array, which is how tagpu_order.c already resolves the tracked
   and hovered units. The index is recycled on death, and that is exactly what
   `expect` is for — the caller says what it believes is in the slot (the type
   name the roster reported) and a mismatch is an error instead of an order to
   whatever moved in. */
static char* live_by_index(char* ta, int idx, const char* expect, const char** why)
{
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    char* u;
    char* def;

    *why = 0;

    if (!readable(beg, UNIT_STRIDE) || end <= beg)
    {
        *why = "the game has no unit array yet";
        return 0;
    }

    /* Divide, never multiply: `(idx + 1) * UNIT_STRIDE` wraps at idx 15339168 on
       this 32-bit build, and `beg + idx * UNIT_STRIDE` wraps with it — a caller
       index that large would pass a multiplied test and produce a pointer
       BELOW the array. `main+0x1435B` is also INCLUSIVE: the engine's own sweep at
       0x48BD22 does `add eax,0x118 / cmp eax,[main+0x1435B] / jbe`, so the unit
       AT `end` is real and `<=` is the right comparison. */
    if (idx < 0 || (size_t)idx > (size_t)(end - beg) / UNIT_STRIDE)
    {
        *why = "index is outside the unit array";
        return 0;
    }

    u = beg + (size_t)idx * UNIT_STRIDE;

    if (!readable(u, UNIT_STRIDE))
    {
        *why = "that slot is not readable";
        return 0;
    }

    {
        unsigned st = *(unsigned*)(u + U_STATE);

        if (!(st & 0x10000000u))
        {
            *why = "nothing alive in that slot";
            return 0;
        }

        /* The alive bit travels with the excluded bit everywhere else in this
           fork (live_selected below, tagpu_order.c, mark, native, tracer), and
           an order to a unit the engine has excluded is exactly as wrong from
           this door as from those. */
        if (st & 0x4000u)
        {
            *why = "that unit is excluded (loaded or hidden)";
            return 0;
        }
    }

    if (expect && expect[0])
    {
        def = *(char**)(u + U_UDEF);

        if (!readable(def, UD_NAME + TASIM_WIRE_NAMELEN))
        {
            *why = "that unit has no readable definition";
            return 0;
        }

        if (lstrcmpiA(def + UD_NAME, expect) != 0)
        {
            *why = "the slot holds a different unit than expected "
                   "(the index was recycled)";
            return 0;
        }
    }

    return u;
}

/* The watched player's current selection, the engine's own walk: the player's
   unit range at PlayerStruct+0x67..+0x6B, inclusive, stepping the unit stride,
   keeping the alive and unexcluded ones whose state carries the selected bit.
   Same three tests CorretCursor_InGame and our own order overlay make. */
static int live_selected(char* ta, char** out, int max)
{
    unsigned char watched = *(unsigned char*)(ta + OFF_WATCHED);
    char* player;
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);
    char* first;
    char* last;
    char* u;
    int   n = 0;

    /* `main+0x2A42` is a byte and nothing here has established a bound on it, so
       bound it: Players[] is ten slots. Unbounded it would place `player` up to
       84 KB past the array and read two pointers out of unrelated fields — which
       this function then hands to the order constructor, where `tagpu_order.c`'s
       identical idiom only ever reads. */
    if (watched >= 10)
        return 0;

    player = ta + OFF_PLAYERS + (size_t)watched * PL_STRIDE;

    if (!readable(player, PL_STRIDE))
        return 0;

    first = *(char**)(player + PL_FIRSTUNIT);
    last  = *(char**)(player + PL_LASTUNIT);

    if (!readable(beg, UNIT_STRIDE) || end < beg)
        return 0;

    /* Containment, not just readability: every pointer this returns is passed to
       ORDERS_NewMainOrder2Unit, so it has to be a unit — inside the array, and on
       a stride boundary. */
    if (first < beg || last > end || last < first ||
        (size_t)(first - beg) % UNIT_STRIDE != 0 ||
        (size_t)(last - beg) % UNIT_STRIDE != 0)
        return 0;

    if (!readable(first, UNIT_STRIDE))
        return 0;

    for (u = first; u <= last && n < max; u += UNIT_STRIDE)
    {
        unsigned st;

        if (!readable(u, UNIT_STRIDE))
            break;

        st = *(unsigned*)(u + U_STATE);

        if (!(st & 0x10000000u) || (st & 0x4000u))
            continue;

        if (st & 0x10u)
            out[n++] = u;
    }

    return n;
}

/* WHERE a live unit is, in the whole world units the order path wants. */
static void live_pos(char* u, int* x, int* alt, int* y)
{
    /* Signed: field-notes.md and six other modules read these as short, and a
       negative coordinate read unsigned becomes ~65535. */
    *x   = *(short*)(u + U_XPOS);
    *alt = *(short*)(u + U_ZPOS);
    *y   = *(short*)(u + U_YPOS);
}

#define SCN_MAX_SEL 512

/* The one order path, for an order at the apply and for a delayed one at its due visit: the
   action TA's mapper 0x43F0E0 gives the command for this unit, then ORDERS_NewMainOrder2Unit
   with Shift for a `queue` order and without it otherwise; `buildweapon` instead goes to the
   queue root (issue_buildweapon), which takes no Shift. 0 when the unit cannot take the order.
   The mapper returns its first argument's buffer on every path, a refusal included: it says no
   by writing action 0 there (0x4401DC..0x4401E7, DISASSEMBLED). Action 0 is `Ready`, the
   descriptor with the empty name, which sorts first and names no command (engine/render.md,
   *0x512344*), so no order the wire can ask for maps to it; and the engine's own caller tests
   the byte the same way (the reaction attack, 0x43B24D). Issued anyway, a refusal would make
   a Ready node after 0x43ADC0 without Shift had wiped the unit's queue.
   Shift, 0x43AFC0's second argument (DISASSEMBLED):
   - THE TOGGLE. With Shift it first walks the main list +0x5C (0x43AFE0..0x43B087): a node of
     the same action byte, of the same target when the order has one, and within 16 world units
     on x and on z of a position the order has, is unlinked and destroyed, and nothing is added.
     The parser rules it out: a queued order names a unit no earlier order of its chain names
     (tasim_wire.c, queue_joins_chain), and the applier passes the target's position with it.
   - THE LINK. Otherwise it calls 0x43ADC0 (0x43B08A..0x43B09D), which wipes the main list only
     for a zero Shift (0x43AE02..0x43AE59), first removes the leading temporary nodes (+0x42 bit
     0x4000, the default Standby: 0x43AE5B..0x43AECE), then links the node after the one marked
     +0x42 bit 0x1000, the last commanded one, and moves the mark to it; with none marked, at
     the tail (0x43AF4F..0x43AFB1). So Shift orders issued in sequence after a plain one form one
     queue in issue order. */
/* BuildWeapon's action byte, or 0 when no descriptor has the name */
static int buildweapon_action(void)
{
    int act = 0;

    ORDERS_ActionByName(&act, ACT_BUILDWEAPON);
    return act & 0xFF;
}

/* `buildweapon`: the build menu's own call, 0x43B0B0(BUILDWEAPON, unit, 0, count), p1 0 being
   slot 0 as in every stock creator (engine/orders.md). BOUNDED: the count is 1..TASIM_WIRE_STOCK_MAX,
   never 0 or below, which would take the removal path; the unit's slot 0 holds a stockpile
   weapon, which BuildWeapon reads unconditionally. */
static int issue_buildweapon(char* unit, int count)
{
    int act;

    if (count < 1 || count > TASIM_WIRE_STOCK_MAX ||
        !stockpile_weapon(*(const char**)(unit + U_WSLOT + WS_WEAPON)) ||
        !(act = buildweapon_action()))
        return 0;

    ORDERS_Queue(act, unit, 0, count);
    return 1;
}

/* What slot 0's BuildWeapon nodes on +0x60 still have to build, summed: the result's read-back of a
   buildweapon order. The list is the game thread's, walked on it, at most SCN_LIST_MAX nodes. */
static int buildweapon_queued(char* unit)
{
    const int   act = buildweapon_action();
    const char* n = *(const char**)(unit + U_SUBORDERS);
    int         k, sum = 0;

    for (k = 0; act && n && k < SCN_LIST_MAX && readable(n, ON_NEXT + 4); k++)
    {
        if (*(const unsigned char*)(n + ON_ACTION) == (unsigned)act && !*(const int*)(n + ON_P1))
            sum += *(const int*)(n + ON_COUNT);
        n = *(const char* const*)(n + ON_NEXT);
    }
    return sum;
}

static int issue_one(int cmd, char* unit, void* target, int* pos, int count, int shift)
{
    int   index = -1;
    char* resolved;

    if (cmd == TASIM_CMD_BUILDWEAPON)
        return issue_buildweapon(unit, count);

    resolved = ScriptAction_Type2Index(&index, (unsigned)cmd, unit, target, pos);

    if (!resolved || !*(unsigned char*)resolved)
        return 0;

    ORDERS_NewMainOrder2Unit((int)(unsigned char)*resolved, shift ? 1 : 0, unit, target, pos, 0, 0);
    return 1;
}

/* ---- delayed orders ----------------------------------------------------------------
   An order with `after=n` is issued through issue_one at the visit whose GameTime is its
   subject's create plus n -- the apply's, or a scheduled create's (sched_watch) -- if its
   subject (and a unit target) is still the unit it was and a feature target's cell still
   holds the same def. The pending table is the tick's: only apply_now, sched_watch and
   delay_watch, all on the tick detour, touch it.

   THE INVARIANT, an ordering and a bound:
   - the watch sees every tick boundary: GameTime advances by at most 1 between two of its
     visits, or every entry is cancelled as `skipped`. That cancel is fail-closed, never a
     retry; the game's own pacing can batch 1..5 steps a frame (0x4953EF..0x4953FE), so a
     delayed order needs the test turbo at k = 1 or a game that steps once a frame;
   - between two consecutive steps a watched slot cannot hold a new unit: B4's hold gives a
     create that asks for no slot the block's first free slot, an unheld one, else the one freed
     longest ago if that was in an earlier tick, never one freed this tick (tagpu_patches.c,
     hit_take and hit_reusable; engine/network.md, *What B4 patches*), so a unit that died is
     seen dead or freed at the visit after its death, before any reuse. A create that asks for
     a slot (0x485F50's eighth argument non-zero, read at 0x486002) bypasses the hold. Every
     engine call reachable in play pushes 0 (the only non-zero request is the saved-game
     restore 0x48718E, in the loader), and the replay's make() passes a recorded slot only for
     a puppet seat, where resolve_setup refuses `after=` (DISASSEMBLED; engine/network.md, *The
     allocator*). This applier passes a slot only for a unit whose wire line says `slot=`, and
     only one that sched_slot_ok admits: inside the owner's block, holding no unit, not emptied
     by this apply's clear or in the current tick, and watched by no pending entry. So a
     request bypasses B4's hold only on a slot no entry watches, that this apply's clear did
     not empty and no death in the current tick freed, and no create lands in a watched slot;
   - an apply's own clear frees slots between two steps, which the fallback (hit_reusable:
     freed in an earlier tick) could hand out in the next step before the watch's next
     look, so the apply cancels every entry its clear touched in the same visit
     (delay_cancel_cleared);
   - PRECONDITION: every death while entries are pending happens inside a step or at an
     apply. A kill made between two steps by anything else -- a console command in the
     message pump, say -- frees a slot the fallback may refill in the next step before the
     watch looks, which neither of the two rules above covers. The calibration runner's
     lanes kill only through steps and applies;
   - the table belongs to the level it was filled in: each entry's level is the
     publisher's level generation (tagpu_packet_pub_level_gen), and a look in another level
     cancels every entry, as does GameTime moving by anything but 0 or 1;
   - an entry keeps a slot index, never a pointer, and every look forms the address only
     after bounding the slot by the array (watched_unit). The array is fixed for the level
     (Mode B, research/notes/thread-safe-destruction.md), so a stale slot costs a cancelled
     order and never a read outside it.

   A chain reaches 0x43AFC0 in wire order: issue_orders walks the wire in order, delay_add
   appends in that order, and delay_watch issues due entries in table order and compacts in
   place, so equal due times keep wire order; the parser refuses a decreasing `after`. An
   entry the watch cancels leaves the chain's later entries to join whatever list the unit
   holds (0x43ADC0 first removes a leading temporary Standby, 0x43AE5B..0x43AECE). */
typedef struct {
    int            cmd, kind;
    int            count;                 /* buildweapon's                                   */
    int            queue;                 /* issued with Shift (TASIM_WIRE_ORDER.queue)      */
    int            pos[3];                /* the apply's position, 16.16 (a position or a
                                             feature target)                                 */
    int            due;                   /* the GameTime of the visit that issues it        */
    int            slot;                  /* the subject's slot, type word and owner         */
    unsigned short type;
    unsigned char  owner;
    int            tslot;                 /* a unit target: the same watch of the target     */
    unsigned short ttype;
    unsigned char  towner;
    int            cell;                  /* a feature target: the cell and its def word     */
    unsigned short cdef;
} scn_delayed;

static scn_delayed g_delay[SCN_MAX_ORDERS];
static int         g_ndelay;
static unsigned    g_dl_level;            /* the level generation the table was filled in */
static int         g_dl_last;             /* the GameTime the watch last looked at */
/* counted over every entry since the last report line; `deaths` holds a subject's death and a
   target's (a unit gone, or a feature cell whose def changed); `refused` the due orders the
   unit could not take (issue_one) */
static struct { int of, issued, refused, deaths, cleared, skipped; } g_dl;

#define U_DYING     0x4000u              /* +0x110 bit 14: the death is pending (engine/units.md) */
static unsigned char g_slotbits[SCN_SLOT_MAX / 8 + 1];

/* The slot of a unit pointer the apply holds, or -1 when it is not a slot of the array. */
static int slot_of(char* ta, char* u)
{
    char* beg = *(char**)(ta + OFF_BEGIN);
    char* end = *(char**)(ta + OFF_END);

    if (!u || u < beg || u > end || (size_t)(u - beg) % UNIT_STRIDE)
        return -1;

    return (int)((u - beg) / UNIT_STRIDE);
}

/* The unit in `slot` when it is still the unit the entry watched, else 0. BOUNDED: the slot
   is tested against the array, whose end is the last slot itself, inclusive (0x4855D6),
   before any address is formed. A unit whose death is pending (+0x110 bit 14, which the next
   step's reaper turns into the death, 0x48AFB9..0x48AFD1) still has its alive bit; it is dead
   here, so its order is counted under deaths and never issued. */
static char* watched_unit(char* beg, char* end, int slot, unsigned short type,
                          unsigned char owner)
{
    char* u;

    if (!beg || slot < 0 || (size_t)slot > (size_t)(end - beg) / UNIT_STRIDE)
        return 0;

    u = beg + (size_t)slot * UNIT_STRIDE;

    if ((*(unsigned*)(u + U_STATE) & (0x10000000u | U_DYING)) != 0x10000000u ||
        *(unsigned short*)(u + U_TYPEWORD) != type || *(unsigned char*)(u + U_OWNER) != owner)
        return 0;

    return u;
}

/* The def word of feature cell `cell`, bounded by the live grid's own size. */
static int feat_cell_def(char* ta, int cell, unsigned short* def)
{
    int   fx = *(int*)(ta + OFF_FMAPX);
    int   fy = *(int*)(ta + OFF_FMAPY);
    char* fm = *(char**)(ta + OFF_FMAP);
    char* c;

    if (!fm || fx <= 0 || fy <= 0 || cell < 0 || (long long)cell >= (long long)fx * fy)
        return 0;

    c = fm + (size_t)cell * FM_STRIDE;

    if (!readable(c, FM_STRIDE))
        return 0;

    *def = *(unsigned short*)(c + FM_DEF);
    return 1;
}

static void delay_report(void)
{
    scnlogf("scenario: delayed orders: issued %d of %d, refused %d, cancelled %d (deaths %d, "
            "cleared %d, skipped %d)", g_dl.issued, g_dl.of, g_dl.refused,
            g_dl.deaths + g_dl.cleared + g_dl.skipped, g_dl.deaths, g_dl.cleared, g_dl.skipped);
    memset(&g_dl, 0, sizeof g_dl);
}

/* Cancels every pending entry when the table was filled in another level than the live one,
   and says so. Returns 1 when it did. */
static int delay_drop_other_level(void)
{
    if (!g_ndelay || g_dl_level == tagpu_packet_pub_level_gen())
        return 0;

    scnlogf("scenario: delayed orders: the level changed; %d pending cancelled", g_ndelay);
    g_dl.skipped += g_ndelay;
    g_ndelay = 0;
    delay_report();
    return 1;
}

/* An entry for subject `u` of order `o`, due at `base`, the GameTime of the subject's create,
   plus its after. 0 when the table is full. */
static int delay_add(char* ta, const TASIM_WIRE_ORDER* o, int base, char* u, char* target,
                     const int* pos)
{
    scn_delayed* d;

    delay_drop_other_level();

    if (g_ndelay >= SCN_MAX_ORDERS)
        return 0;

    /* The watch looks at every visit from this one on. An empty table starts its watch at this
       visit; a table with entries keeps the watch's own last look, which is this visit's at the
       apply (delay_watch runs first in the visit) and the visit before at a scheduled create
       (sched_watch runs before delay_watch), so the watch still looks at this visit's tick for
       the entries already there. */
    if (!g_ndelay)
    {
        g_dl_level = tagpu_packet_pub_level_gen();
        g_dl_last = base;
    }

    d = &g_delay[g_ndelay];
    memset(d, 0, sizeof *d);
    d->cmd = o->cmd;
    d->kind = o->kind;
    d->count = o->kind == TASIM_TGT_COUNT ? o->a : 0;
    d->queue = o->queue;
    memcpy(d->pos, pos, sizeof d->pos);
    d->due = base + o->after;
    d->slot = slot_of(ta, u);
    d->type = *(unsigned short*)(u + U_TYPEWORD);
    d->owner = *(unsigned char*)(u + U_OWNER);
    d->tslot = d->cell = -1;

    if (o->kind == TASIM_TGT_UNIT && target)
    {
        d->tslot = slot_of(ta, target);
        d->ttype = *(unsigned short*)(target + U_TYPEWORD);
        d->towner = *(unsigned char*)(target + U_OWNER);
    }
    else if (o->kind == TASIM_TGT_FEAT && o->a >= 0 && o->a < g_wire.nfeats && fmap_x > 0)
    {
        /* the apply's alone: a scheduled create's order never has a feature target (resolve_all,
           sched_add_order), and g_wire and g_feats are the present path's after the apply */
        d->cell = g_feats[o->a].ay / 16 * fmap_x + g_feats[o->a].ax / 16;
        if (!feat_cell_def(ta, d->cell, &d->cdef))
            d->cell = -1;
    }

    /* an entry with no slot to watch is cancelled at its first look, never issued */
    g_dl.of++;
    g_ndelay++;
    return 1;
}

/* Right after an apply's clear: cancel every entry whose subject or unit target the clear
   killed (the invariant above). */
static void delay_cancel_cleared(void)
{
    int i, n;

    if (!g_ndelay || !g_ncleared_slots)
        return;

    for (i = 0; i < g_ncleared_slots; i++)
        if (g_cleared_slot[i] >= 0 && g_cleared_slot[i] <= SCN_SLOT_MAX)
            g_slotbits[g_cleared_slot[i] >> 3] |= (unsigned char)(1u << (g_cleared_slot[i] & 7));

#define SLOT_HIT(s) ((s) >= 0 && (s) <= SCN_SLOT_MAX && (g_slotbits[(s) >> 3] & (1u << ((s) & 7))))
    for (i = n = 0; i < g_ndelay; i++)
    {
        if (SLOT_HIT(g_delay[i].slot) ||
            (g_delay[i].kind == TASIM_TGT_UNIT && SLOT_HIT(g_delay[i].tslot)))
        {
            g_dl.cleared++;
            continue;
        }
        g_delay[n++] = g_delay[i];
    }
#undef SLOT_HIT

    g_ndelay = n;
    memset(g_slotbits, 0, sizeof g_slotbits);
}

/* At every visit of the tick site while entries are pending, before any apply of the same
   visit, so the apply's creates come after the watch has seen this tick's deaths. */
static void delay_watch(void)
{
    char* ta;
    char* beg;
    char* end;
    int   now, i, n;

    if (!g_ndelay || !(ta = ta_base()))
        return;

    /* On a puppet the recorder is the only writer of the state the recording carries, so an
       order still pending when the level became one is never issued. */
    if (tagpu_puppet_on())
    {
        scnlogf("scenario: delayed orders: the level is a puppet; %d pending cancelled", g_ndelay);
        g_dl.skipped += g_ndelay;
        g_ndelay = 0;
        delay_report();
        return;
    }

    now = *(int*)(ta + OFF_GAMETIME);

    if (delay_drop_other_level() || now == g_dl_last)
        return;

    if (now != g_dl_last + 1)
    {
        scnlogf("scenario: delayed orders: GameTime went from %d to %d between two visits; "
                "%d pending cancelled", g_dl_last, now, g_ndelay);
        g_dl.skipped += g_ndelay;
        g_ndelay = 0;
        delay_report();
        return;
    }

    g_dl_last = now;
    beg = *(char**)(ta + OFF_BEGIN);
    end = *(char**)(ta + OFF_END);

    if (!readable(beg, UNIT_STRIDE) || end < beg || !readable(end, UNIT_STRIDE))
        beg = end = 0;

    for (i = n = 0; i < g_ndelay; i++)
    {
        scn_delayed*   d = &g_delay[i];
        char*          u = watched_unit(beg, end, d->slot, d->type, d->owner);
        char*          t = 0;
        unsigned short def;
        int            pos[3];

        if (u && d->kind == TASIM_TGT_UNIT)
            t = watched_unit(beg, end, d->tslot, d->ttype, d->towner);

        if (!u || (d->kind == TASIM_TGT_UNIT && !t) ||
            (d->kind == TASIM_TGT_FEAT &&
             (d->cell < 0 || !feat_cell_def(ta, d->cell, &def) || def != d->cdef)))
        {
            g_dl.deaths++;
            continue;
        }

        if (d->due > now)
        {
            g_delay[n++] = *d;
            continue;
        }

        memcpy(pos, d->pos, sizeof pos);

        if (t)
        {
            int x, alt, y;
            live_pos(t, &x, &alt, &y);
            pos[0] = x << 16;
            pos[1] = alt << 16;
            pos[2] = y << 16;
        }

        if (issue_one(d->cmd, u, t, pos, d->count, d->queue))
            g_dl.issued++;
        else
        {
            g_dl.refused++;
            scnlogf("scenario: delayed order at GameTime %d: slot %d cannot take cmd %d",
                    now, d->slot, d->cmd);
        }
    }

    g_ndelay = n;

    if (!n)
        delay_report();
}

/* ---- scheduled creates -------------------------------------------------------------
   A unit whose wire line says `at=n` (n > 0) is created at the visit whose GameTime is the
   apply's plus n, in the slot its `slot=` asks for or else the first free one, and then given its
   orders: those without `after=` at once, the others into the delayed table with its create as
   their base. It is the packed calibration lane's (research/notes/modules/tasim-calibration.md,
   *The packed lane*): cell k is created 30k ticks after the apply, in its own positions of each
   seat's block, so that the weapon scan and every schedule whose period divides 30 meet it at the
   phase an unpacked run's units meet. Not every schedule: autoheal runs on GameTime mod 8
   (0x48AF5E), which the planner meets by staggering a batch with a healtime type 120 ticks a cell;
   the pathfinder's tick count pfcalls has a period of 150 calls; and the feature tick's cursor
   main+0x14257 steps through the map by cell, so where it stands depends on the cell's place, not
   its time. The scans' phase needs the batch's apply to clear (reset_weapon_scans runs only at an
   apply with g_wire.clear and g_fresh_reset) and the level to be empty already: sched_slot_ok
   refuses a slot this apply's own clear emptied.

   THE INVARIANT, an ordering:
   - the table is the tick detour's alone: apply_now fills it from the wire and sched_watch
     empties it, both on the tick detour. Everything a create needs -- the wire's line, the
     resolved type, the altitude, the requested slot, its orders with their positions -- is
     copied at the apply, because once the apply is done the present path parses the next wire
     into g_wire and resets g_units;
   - it belongs to one apply in one level: the next apply cancels whatever this one left
     pending, and so does a look in another level (the publisher's level generation) or on a
     puppet, or GameTime moving by anything but 0 or 1 between two visits -- the delayed orders'
     fail-closed rule, so a create is made at its own tick or never, and never late;
   - it keeps slot indices, never pointers: a requested slot is bounded by its owner's block and
     the array (slot_index) and admitted by sched_slot_ok before the create, and the create's
     return is compared with that slot's record.
   It never resets the weapon scans or the seats' pass results (reset_weapon_scans,
   reset_pass_results): those are the apply's, and the scans' cursors keep stepping from the
   apply on, which is what places a later create on the phase the packed lane computes.
   ONE REPORT a table, when nothing is left pending: `scenario: scheduled creates of the apply at
   GameTime G: made X of Y, refused Z, skipped W; orders issued A, delayed B, refused C, skipped
   D`, G the GameTime of the apply that filled the table, Y counting every unit with `at=` or
   `slot=`, the apply's own placed creates included, and the orders those of the creates after
   the apply. G is what ties a report to its apply: a later apply cancels what is pending and
   reports it inside its own visit, after the lines a reader may already count as the later
   apply's. */
enum { SC_PENDING, SC_MADE, SC_REFUSED };

typedef struct {
    TASIM_WIRE_UNIT wu;                   /* the wire's line, copied at the apply           */
    scn_unit        u;                    /* its type (resolve_all), then what was made     */
    int             ord;                  /* its ordinal: the table runs in wire order      */
    int             alt;                  /* the create's altitude, taken at the apply      */
    int             due;                  /* the GameTime of the visit that creates it      */
    int             state;                /* SC_*                                           */
} scn_sched;

typedef struct {
    int cmd, kind, count, queue, after;
    int subj;                             /* its subject's row of g_sched                   */
    int target;                           /* a unit target's row of g_sched, else -1        */
    int pos[3];                           /* a position target, 16.16, taken at the apply   */
} scn_sched_order;

static scn_sched       g_sched[SCN_MAX_UNITS];
static int             g_nsched;
static scn_sched_order g_sched_ord[SCN_MAX_ORDERS];
static int             g_nsched_ord;
static int             g_sc_pending;      /* rows of g_sched still SC_PENDING               */
static int             g_sc_next;         /* the earliest due among them                    */
static unsigned        g_sc_level;        /* the level generation the table was filled in   */
static int             g_sc_last;         /* the GameTime the watch last looked at          */
static int             g_sc_apply;        /* the GameTime of the apply that filled it       */
static int             g_sc_fresh;        /* the apply's g_fresh_reset: fresh_unit applies  */
/* the slots this apply's clear emptied, for sched_slot_ok as long as the table lives */
static unsigned char   g_sc_cleared[SCN_SLOT_MAX / 8 + 1];
static struct { int of, made, refused, skipped, issued, delayed, ord_refused, ord_skipped; } g_sc;

/* Whether a create may ask for slot `idx`, an index slot_index made. The create takes a requested
   slot past B4's hold (hit_take holds only a first-free create, 0x486036), so it is admitted only
   where that hold would have nothing to keep:
   - the slot holds no unit: its type word +0xA6 is 0, the create's own test (0x486036);
   - this apply's clear did not empty it, nor did a death in this tick (tagpu_unit_slot_takeable,
     the hold's own test): no unit that just left it can be named by what still refers to it;
   - no pending delayed entry watches it, as subject or unit target: the watch would take a new
     unit of the same type and owner for the one it watched (the invariant above). */
static int sched_slot_ok(char* ta, unsigned idx)
{
    char* rec = *(char**)(ta + OFF_BEGIN) + (size_t)idx * UNIT_STRIDE;
    int   i;

    if (!idx || idx > SCN_SLOT_MAX || !readable(rec, UNIT_STRIDE) ||
        *(unsigned short*)(rec + U_TYPEWORD) ||
        (g_sc_cleared[idx >> 3] & (1u << (idx & 7))) || !tagpu_unit_slot_takeable(idx))
        return 0;

    for (i = 0; i < g_ndelay; i++)
        if (g_delay[i].slot == (int)idx ||
            (g_delay[i].kind == TASIM_TGT_UNIT && g_delay[i].tslot == (int)idx))
            return 0;

    return 1;
}

static void sched_report(void)
{
    scnlogf("scenario: scheduled creates of the apply at GameTime %d: made %d of %d, refused %d, "
            "skipped %d; orders issued %d, delayed %d, refused %d, skipped %d", g_sc_apply,
            g_sc.made, g_sc.of, g_sc.refused, g_sc.skipped, g_sc.issued, g_sc.delayed,
            g_sc.ord_refused, g_sc.ord_skipped);
    memset(&g_sc, 0, sizeof g_sc);
}

/* Every pending create skipped, with its orders, and the report. */
static void sched_cancel(const char* why)
{
    int i;

    if (!g_sc_pending)
        return;

    scnlogf("scenario: scheduled creates of the apply at GameTime %d: %s; %d pending cancelled",
            g_sc_apply, why, g_sc_pending);
    for (i = 0; i < g_nsched_ord; i++)
        if (g_sched[g_sched_ord[i].subj].state == SC_PENDING)
            g_sc.ord_skipped++;
    for (i = 0; i < g_nsched; i++)
        if (g_sched[i].state == SC_PENDING)
        {
            g_sched[i].state = SC_REFUSED;
            g_sc.skipped++;
        }
    g_sc_pending = 0;
    sched_report();
}

/* At the head of every apply: what an earlier apply left pending is cancelled, and the table is
   this apply's. */
static void sched_begin(char* ta)
{
    sched_cancel("a later apply");
    g_nsched = g_nsched_ord = 0;
    memset(&g_sc, 0, sizeof g_sc);
    memset(g_sc_cleared, 0, sizeof g_sc_cleared);
    g_sc_level = tagpu_packet_pub_level_gen();
    g_sc_last = *(int*)(ta + OFF_GAMETIME);
    g_sc_apply = g_sc_last;
    g_sc_next = 0x7FFFFFFF;
}

/* After the apply's clear: the slots it emptied, which sched_slot_ok refuses. */
static void sched_note_cleared(void)
{
    int i;

    for (i = 0; i < g_ncleared_slots; i++)
        if (g_cleared_slot[i] > 0 && g_cleared_slot[i] <= SCN_SLOT_MAX)
            g_sc_cleared[g_cleared_slot[i] >> 3] |= (unsigned char)(1u << (g_cleared_slot[i] & 7));
}

/* One placed create of the apply's own, made (1) or refused (0), into the report. */
static void sched_counted(int made)
{
    g_sc.of++;
    if (made)
        g_sc.made++;
    else
        g_sc.refused++;
}

/* Unit `ord` of the wire, resolved, into the table; 0 when the table is full. */
static int sched_add(int ord, int alt)
{
    scn_sched* e;

    if (g_nsched >= SCN_MAX_UNITS)
        return 0;

    e = &g_sched[g_nsched++];
    e->wu = g_wire.units[ord];
    e->u = g_units[ord];
    e->ord = ord;
    e->alt = alt;
    e->due = g_gametime + g_wire.units[ord].at;
    e->state = SC_PENDING;
    g_sc_fresh = g_fresh_reset;
    g_sc.of++;
    g_sc_pending++;
    if (e->due < g_sc_next)
        g_sc_next = e->due;
    return 1;
}

/* The row of g_sched holding ordinal `ord`, or -1: the rows are in wire order (sched_add). */
static int sched_row(int ord)
{
    int lo = 0, hi = g_nsched - 1;

    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        if (g_sched[mid].ord == ord)
            return mid;
        if (g_sched[mid].ord < ord)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* Order `o` of a scheduled subject, into the table with what it needs at its subject's create; 0
   when it cannot wait there: its subject or unit target is not in the table, it names a feature
   or a live unit (resolve_all), or the table is full. */
static int sched_add_order(const TASIM_WIRE_ORDER* o)
{
    scn_sched_order* q;
    int              subj = sched_row(o->uord);
    int              target = -1;

    if (subj < 0 || g_nsched_ord >= SCN_MAX_ORDERS || o->kind == TASIM_TGT_FEAT ||
        (o->kind == TASIM_TGT_UNIT && (o->tlive || (target = sched_row(o->a)) < 0)))
        return 0;

    q = &g_sched_ord[g_nsched_ord++];
    memset(q, 0, sizeof *q);
    q->cmd = o->cmd;
    q->kind = o->kind;
    q->count = o->kind == TASIM_TGT_COUNT ? o->a : 0;
    q->queue = o->queue;
    q->after = o->after;
    q->subj = subj;
    q->target = target;
    if (o->kind == TASIM_TGT_POS)
    {
        q->pos[0] = o->a << 16;
        q->pos[1] = ground_at(o->a, o->b) << 16;
        q->pos[2] = o->b << 16;
    }
    return 1;
}

/* A row's create, at its due visit: 1 made, 0 refused with a line naming why. */
static int sched_create(char* ta, scn_sched* e, int now)
{
    char*       pl = player_at(ta, e->wu.owner);
    unsigned    idx = 0;
    const char* why = 0;

    if (e->wu.slot != TASIM_WIRE_UNSET &&
        (!(idx = slot_index(ta, e->wu.owner, e->wu.slot)) || !sched_slot_ok(ta, idx)))
        why = "its slot is not one a create may ask for (sched_slot_ok)";
    /* the seat test resolve_all made at the apply, again: the create indexes +0x78 */
    else if (!pl || !*(int*)(pl + PL_ACTIVE) || !*(unsigned char*)(pl + PL_TYPE) ||
             !*(char**)(pl + PL_GROUPS))
        why = "its player has no seat now";
    else if (!create_one(&e->wu, &e->u, e->alt, idx, g_sc_fresh))
        why = "the engine refused it";

    if (why)
    {
        scnlogf("scenario: scheduled create of unit %d (%.20s) at GameTime %d refused: %s", e->ord,
                e->wu.type, now, why);
        return 0;
    }
    return 1;
}

/* Every visit of the tick site while creates are pending, before delay_watch: the due rows are
   created in wire order, then their orders are issued in wire order, so an order can name any
   unit of its own create tick. */
static void sched_watch(void)
{
    char* ta;
    int   now, i;

    if (!g_sc_pending || !(ta = ta_base()))
        return;

    /* on a puppet the recorder is the only writer of the state the recording carries */
    if (tagpu_puppet_on())
    {
        sched_cancel("the level is a puppet");
        return;
    }
    if (g_sc_level != tagpu_packet_pub_level_gen())
    {
        sched_cancel("the level changed");
        return;
    }

    now = *(int*)(ta + OFF_GAMETIME);

    if (now == g_sc_last)
        return;

    if (now != g_sc_last + 1)
    {
        scnlogf("scenario: scheduled creates: GameTime went from %d to %d between two visits",
                g_sc_last, now);
        sched_cancel("a visit was skipped");
        return;
    }

    g_sc_last = now;

    if (now < g_sc_next)
        return;

    g_sc_next = 0x7FFFFFFF;
    for (i = 0; i < g_nsched; i++)
    {
        scn_sched* e = &g_sched[i];

        if (e->state != SC_PENDING)
            continue;
        if (e->due > now)
        {
            if (e->due < g_sc_next)
                g_sc_next = e->due;
            continue;
        }
        g_sc_pending--;
        e->state = e->due == now && sched_create(ta, e, now) ? SC_MADE : SC_REFUSED;
        if (e->state == SC_MADE)
            g_sc.made++;
        else
            g_sc.refused++;
    }

    for (i = 0; i < g_nsched_ord; i++)
    {
        scn_sched_order*  q = &g_sched_ord[i];
        const scn_sched*  s = &g_sched[q->subj];
        const scn_sched*  t = q->target >= 0 ? &g_sched[q->target] : 0;
        TASIM_WIRE_ORDER  o;
        int               pos[3];

        if (s->due != now)
            continue;
        if (s->state != SC_MADE || (t && t->state != SC_MADE))
        {
            g_sc.ord_refused++;
            continue;
        }

        memcpy(pos, q->pos, sizeof pos);
        if (t)
        {
            pos[0] = t->u.ax << 16;
            pos[1] = t->u.ah << 16;
            pos[2] = t->u.ay << 16;
        }

        if (!q->after)
        {
            if (issue_one(q->cmd, (char*)s->u.unit, t ? t->u.unit : 0, pos, q->count, q->queue))
                g_sc.issued++;
            else
            {
                g_sc.ord_refused++;
                scnlogf("scenario: scheduled unit %d at GameTime %d cannot take cmd %d", s->ord,
                        now, q->cmd);
            }
            continue;
        }

        memset(&o, 0, sizeof o);
        o.cmd = q->cmd;
        o.kind = q->kind;
        o.a = q->count;
        o.queue = q->queue;
        o.after = q->after;
        if (delay_add(ta, &o, now, (char*)s->u.unit, t ? (char*)t->u.unit : 0, pos))
            g_sc.delayed++;
        else
        {
            g_sc.ord_refused++;
            scnlogf("scenario: scheduled unit %d at GameTime %d: %d delayed orders are pending "
                    "already", s->ord, now, SCN_MAX_ORDERS);
        }
    }

    if (!g_sc_pending)
        sched_report();
}

static void issue_orders(void)
{
    char* ta = *(char**)TA_MAINPP;
    int i;

    for (i = 0; i < g_wire.norders; i++)
    {
        TASIM_WIRE_ORDER*  o = &g_wire.orders[i];
        char*       subj[SCN_MAX_SEL];
        const char* sname = "";
        const char* why = 0;
        int         nsubj = 0, s;
        void*       target = 0;
        int         pos[3];
        int         havepos = 0;

        /* An ordinal indexes g_wire.units[], g_units[] and g_feats[] only inside [0, n), n being
           at most their size by the parser's caps: the parser checks no ordinal's range, and
           resolve_all counts one outside it as bad without dropping it, so under onerror=skip
           the order reaches here and each use bounds it again. */
        if ((o->subj == TASIM_SUBJ_ORD && (o->uord < 0 || o->uord >= g_wire.nunits)) ||
            (o->kind == TASIM_TGT_UNIT && !o->tlive && (o->a < 0 || o->a >= g_wire.nunits)) ||
            (o->kind == TASIM_TGT_FEAT && (o->a < 0 || o->a >= g_wire.nfeats)))
        {
            g_ord_fail++;                    /* resolve_all said why */
            continue;
        }

        /* a scheduled create's order waits with its subject (sched_add_order) */
        if (o->subj == TASIM_SUBJ_ORD && g_wire.units[o->uord].at)
        {
            if (!g_units[o->uord].ok || !sched_add_order(o))
                g_ord_fail++;                /* the subject was refused, which said why */
            continue;
        }

        /* WHO ------------------------------------------------------------- */
        if (o->subj == TASIM_SUBJ_ORD)
        {
            const TASIM_WIRE_UNIT* wu = &g_wire.units[o->uord];
            scn_unit* u = &g_units[o->uord];
            sname = wu->type;
            if (u->unit)
                subj[nsubj++] = (char*)u->unit;
        }
        else if (o->subj == TASIM_SUBJ_LIVE)
        {
            char* u = live_by_index(ta, o->uord, o->sexp, &why);
            sname = o->sexp[0] ? o->sexp : "that unit";
            if (u)
                subj[nsubj++] = u;
            else
                scn_err("order %d: unit @%d -- %s", i, o->uord, why);
        }
        else
        {
            nsubj = live_selected(ta, subj, SCN_MAX_SEL);
            sname = "the selection";
            if (!nsubj)
                scn_err("order %d: nothing is selected", i);
            else if (nsubj == SCN_MAX_SEL)
            {
                /* A cap that swallows the overflow quietly would report a full
                   success while ordering part of the selection. Say it, and
                   COUNT it — the CLI's exit status reads g_ord_fail, so logging
                   alone would still exit 0. */
                scn_err("order %d: %d or more units are selected; only the "
                        "first %d were ordered", i, SCN_MAX_SEL, SCN_MAX_SEL);
                g_ord_fail++;
            }
        }

        if (!nsubj)
        {
            g_ord_fail++;
            continue;
        }

        /* WHERE / WHAT ----------------------------------------------------- */
        memset(pos, 0, sizeof pos);

        if (o->kind == TASIM_TGT_POS)
        {
            /* 16.16 fixed point, 3-D convention {x, altitude, depth} — the same
               language UNITS_CreateUnit speaks (see the header comment). */
            pos[0] = o->a << 16;
            pos[1] = ground_at(o->a, o->b) << 16;
            pos[2] = o->b << 16;
            havepos = 1;
        }
        else if (o->kind == TASIM_TGT_UNIT && o->tlive)
        {
            int x, alt, y;
            char* t = live_by_index(ta, o->a, o->texp, &why);
            if (!t)
            {
                g_ord_fail++;
                scn_err("order %d: target unit @%d -- %s", i, o->a, why);
                continue;
            }
            target = t;
            live_pos(t, &x, &alt, &y);
            pos[0] = x << 16;
            pos[1] = alt << 16;
            pos[2] = y << 16;
            havepos = 1;
        }
        else if (o->kind == TASIM_TGT_UNIT)
        {
            target = g_units[o->a].unit;
            if (!target)
            {
                g_ord_fail++;
                scn_err("order %d: its target unit %d was never created", i, o->a);
                continue;
            }
            pos[0] = g_units[o->a].ax << 16;
            pos[1] = g_units[o->a].ah << 16;
            pos[2] = g_units[o->a].ay << 16;
            havepos = 1;
        }
        else if (o->kind == TASIM_TGT_FEAT)
        {
            /* A feature is not a UnitStruct: TA reclaims and attacks it through the
               map cell, so the order carries the position and no target pointer.
               A feature the wire did not create is named by its position instead —
               `pos` reaches the same code, which is why there is no @ form here.
               A feature that was not placed has no position (ax/ay stay 0): the order
               fails by name rather than reaching the mapper at the map's corner. */
            if (!g_feats[o->a].ok)
            {
                g_ord_fail++;
                scn_err("order %d: its target feature %d was never placed", i, o->a);
                continue;
            }
            pos[0] = g_feats[o->a].ax << 16;
            pos[1] = g_feats[o->a].ah << 16;
            pos[2] = g_feats[o->a].ay << 16;
            havepos = 1;
        }

        /* LATER ------------------------------------------------------------ */
        if (o->after < 0)
        {
            g_ord_fail++;                    /* refused by resolve_setup, which said why */
            continue;
        }

        if (o->after > 0)
        {
            for (s = 0; s < nsubj; s++)
                if (!delay_add(ta, o, g_gametime, subj[s], (char*)target, pos))
                {
                    g_ord_fail++;
                    scn_err("order %d: %d delayed orders are pending already", i,
                            SCN_MAX_ORDERS);
                }
            continue;
        }

        /* ISSUE ------------------------------------------------------------ */
        for (s = 0; s < nsubj; s++)
        {
            if (!issue_one(o->cmd, subj[s], target, pos, o->kind == TASIM_TGT_COUNT ? o->a : 0,
                           o->queue))
            {
                g_ord_fail++;
                scn_err("order %d: this unit cannot take that order (%.20s, cmd %d)",
                        i, sname, o->cmd);
                continue;
            }

            g_ord_ok++;

            if (o->cmd == TASIM_CMD_BUILDWEAPON && o->subj == TASIM_SUBJ_ORD)
                g_units[o->uord].queued = buildweapon_queued(subj[s]);

            /* Read the stored order position back once, in whole world units, so the
               result file shows where the order actually points. It cannot prove the
               scale (the constructor copies the dwords verbatim), but it does catch
               the engine relocating or clamping a target. */
            if (!g_probe_have && havepos)
            {
                char* ord = *(char**)(subj[s] + U_ORDERS);
                g_probe_pass[0] = pos[0] >> 16;
                g_probe_pass[1] = pos[1] >> 16;
                g_probe_pass[2] = pos[2] >> 16;
                if (readable(ord, UO_POS + 12))
                {
                    g_probe_stored[0] = *(int*)(ord + UO_POS)       >> 16;
                    g_probe_stored[1] = *(int*)(ord + UO_POS + 4)   >> 16;
                    g_probe_stored[2] = *(int*)(ord + UO_POS + 8)   >> 16;
                    g_probe_have = 1;
                }
            }
        }
    }
}

static void resolve_camera(void)
{
    /* the ordinal's bound, as issue_orders': resolve_all named an ordinal outside the wire and
       onerror=skip still arrives here, so the camera is skipped and counted with the failures */
    if ((g_wire.cam_kind == TASIM_TGT_UNIT && (g_wire.cam_a < 0 || g_wire.cam_a >= g_wire.nunits)) ||
        (g_wire.cam_kind == TASIM_TGT_FEAT && (g_wire.cam_a < 0 || g_wire.cam_a >= g_wire.nfeats)))
    {
        g_failed++;
        return;
    }

    if (g_wire.cam_kind == TASIM_TGT_POS)
    {
        g_cam_x = g_wire.cam_a;
        g_cam_y = g_wire.cam_b;
        g_cam_h = ground_at(g_wire.cam_a, g_wire.cam_b);
        g_cam_have = 1;
    }
    else if (g_wire.cam_kind == TASIM_TGT_UNIT && g_units[g_wire.cam_a].unit)
    {
        /* an entity handle resolves to where the unit ACTUALLY landed */
        g_cam_x = g_units[g_wire.cam_a].ax;
        g_cam_y = g_units[g_wire.cam_a].ay;
        g_cam_h = g_units[g_wire.cam_a].ah;
        g_cam_have = 1;
    }
    else if (g_wire.cam_kind == TASIM_TGT_FEAT && g_feats[g_wire.cam_a].ok)
    {
        g_cam_x = g_feats[g_wire.cam_a].ax;
        g_cam_y = g_feats[g_wire.cam_a].ay;
        g_cam_h = g_feats[g_wire.cam_a].ah;
        g_cam_have = 1;
    }
}

/* The one visit. Everything the scenario asks for lands here, in this order, so
   the situation is reproducible: switches first (units must spawn under the
   rules the file asked for), the players and the objective, then the clear (units,
   then features), then entities, then orders. */
/* THE SEATS' PASS RESULTS. The economy pass 0x401360 rewrites a seat's rates and storage maxima
   at every pass: it zeroes the maxima (0x40137D, 0x401383), adds the energystorage and
   metalstorage of every unit of the seat's block that is alive (+0x110 bit 28) and finished
   (+0x104 == 0.0; 0x4013AC..0x4013B4, 0x4016C5..0x4016D6, 0x40179B..0x4017C5), then the base
   storages while +0x149 bit 0 is set (0x401941, 0x401988..0x4019AA), and writes the four rates
   as its sums without reading them (0x4019C6, 0x4019DD, 0x401A05, 0x401A2D). But the players
   phase calls it only for a seat with a live unit or one that never had any (0x465525..0x465537):
   a seat emptied by a clear keeps the rates and maxima of its last pass with units until a unit
   of its own lives through the next (DISASSEMBLED, engine/weapons.md). So this resets them at
   every apply, after the creates, to values the apply alone decides:
   - the rates to 0, the record's init value (0x464712, 0x464771..0x464789): their readers before
     the seat's next pass are the stock AI's planners (0x408691, 0x40874A, 0x40BC53..0x40BCC5),
     which run for a type-2 seat only, and the resource bar (0x468F8D..0x468FB4);
   - the maxima to what that pass would compute from the seat's units after the apply, summed in
     its order, with a pending death (+0x110 bit 14) left out because the reaper takes it in the
     next step's units phase, before that pass: so a seat with units reads the same maxima before
     its first pass as after it unless a unit is finished or lost on the way, and a seat with
     none, which gets no pass in the whole run, keeps its base storage. Their readers before
     that pass decide on them -- RepairPatrol 0x405980 and VTOL_RepairPatrol 0x4152F0 compare
     the stored level with 0.2 x the maximum and a feature's yield with the room left
     (0x4059EA..0x4059FC, 0x405C7D..0x405CDF, 0x415483..0x415495, 0x4156FC..0x415752;
     engine/weapons.md lists every site) -- and outside a network game nothing clamps a level
     to them but the pass itself, against the maxima it recomputes (0x401AB3..0x401B27). A
     maximum below a level the wire sets is raised to it, as apply_players raises the maxima it
     finds, so the stored level never reads above its maximum.
   Withheld in a network game and on a puppet, as the calibration keys are (resolve_setup):
   every peer runs these readers. BOUNDED: the seat's block is used only inside the unit array
   (its end the last slot, inclusive) and on a stride boundary, and a unit's def is taken by its
   type word below UNITINFOCount, which for a live unit names the def +0x92 points at
   (engine/units.md, the +0x92 row). */
static void reset_pass_results(char* ta)
{
    char*    beg   = *(char**)(ta + OFF_BEGIN);
    char*    end   = *(char**)(ta + OFF_END);
    char*    defs  = *(char**)(ta + OFF_UDEFS);
    unsigned count = *(unsigned*)(ta + OFF_UCOUNT);
    int      i;

    if (!g_fresh_reset)
        return;

    for (i = 0; i < g_wire.npl; i++)
    {
        char* pl = player_at(ta, g_wire.pl[i].slot);
        char* first;
        char* last;
        char* u;
        float emax = 0.0f, mmax = 0.0f;

        /* a free record or an inactive one gets no economy pass (0x465508..0x465523) and
           owns no units to sum */
        if (!pl || !*(int*)(pl + PL_ACTIVE) || *(unsigned char*)(pl + PL_SELF) == 10)
            continue;

        first = *(char**)(pl + PL_FIRSTUNIT);
        last  = *(char**)(pl + PL_LASTUNIT);

        if (defs && count <= 16384u && readable(beg, UNIT_STRIDE) && end >= beg &&
            first >= beg && last <= end && first <= last &&
            (size_t)(first - beg) % UNIT_STRIDE == 0 && (size_t)(last - beg) % UNIT_STRIDE == 0)
        {
            for (u = first; u <= last; u += UNIT_STRIDE)
            {
                unsigned       st = *(unsigned*)(u + U_STATE);
                unsigned short t  = *(unsigned short*)(u + U_TYPEWORD);
                char*          d;

                if ((st & (0x10000000u | U_DYING)) != 0x10000000u ||
                    *(float*)(u + U_NANOFRAME) != 0.0f || t >= count)
                    continue;

                d = defs + (size_t)t * UD_STRIDE;
                mmax += *(float*)(d + UD_MSTORE);
                emax += *(float*)(d + UD_ESTORE);
            }
        }

        if (*(unsigned char*)(pl + PL_BASEFLAG) & 1)
        {
            emax += *(float*)(pl + PL_BASESTORE_E);
            mmax += *(float*)(pl + PL_BASESTORE_M);
        }

        if (g_wire.pl[i].energy != TASIM_WIRE_UNSET && emax < *(float*)(pl + PL_ENERGY))
            emax = *(float*)(pl + PL_ENERGY);
        if (g_wire.pl[i].metal != TASIM_WIRE_UNSET && mmax < *(float*)(pl + PL_METAL))
            mmax = *(float*)(pl + PL_METAL);

        *(float*)(pl + PL_EINCOME)   = 0.0f;
        *(float*)(pl + PL_EEXPENSE)  = 0.0f;
        *(float*)(pl + PL_MINCOME)   = 0.0f;
        *(float*)(pl + PL_MEXPENSE)  = 0.0f;
        *(float*)(pl + PL_MAXENERGY) = emax;
        *(float*)(pl + PL_MAXMETAL)  = mmax;
    }
}

/* Every record's weapon scan started over, as on a level's first step. The AI object at record
   +0x74, which the player init 0x464700 makes for every record but an active type 3, human seats
   included, keeps at +0x39 the cursor of the weapon scan 0x4089A0; the players phase runs it once
   a step for each active record (0x465031 -> 0x408C40, which calls it at 0x408C9A or 0x408CA7).
   One call scans limit/30 + 1 slots of the record's block from the cursor and steps from the
   last slot +0x6B to the first +0x67; a cursor of 0, the constructor's (0x408CDA), starts at the
   first (0x4089DC..0x4089FB, DISASSEMBLED). An idle weapon gets its target from that scan when
   the cursor reaches its unit, so where the cursor stands at the apply decides when the units
   can first fire, and it stands where the steps since the level load left it. MEASURED
   2026-11-12 on the fifth verification's duel-armpw-corak at a limit of 1500 (51 slots a call):
   48 of the 50 fresh launches, each applied at GameTime 91, took their targets 86 steps after
   the apply; the reload lane, applied at 91 + 330k, took them 78 to 107 steps after, the ARMPW
   in 34 of its 50 runs on the step where 51 × GameTime mod 1500 was at most 51, and the lane's
   median time to kill was 236 against the fresh launches' 229. With every cursor at 0 a run's
   scan starts from each block's first slot on the step after the apply, whatever ran before it.
   Only an apply that clears the level resets them (g_wire.clear): that is the reload C1 needs,
   every calibration wire clears, and an apply that adds to a running game (a camera move, `tacli
   order`, a fixture's mid-level wave) leaves the scans where the game has them.
   BOUNDED: the ten records only (player_at). The object is the engine's own, made at the player
   init 0x464700 (0x4648A7..0x4648C1). Every path that ends it leaves the record's pointer NULL,
   which the loop tests: the teardown 0x464A00 frees it and stores NULL (0x464A5D..0x464A6B);
   the seat activation 0x464290 stores 0 without a free (0x46434A); Impure's rewind frees it
   through the same destructor 0x408F10 and stores NULL (tagpu_replay.c, rw_seats). All three,
   the players phase that runs the scan and this apply's visit run on the game thread, between
   two steps, so a pointer read here is the live object or NULL; the store is one dword the scan
   reads and writes on that same thread. */
static void reset_weapon_scans(char* ta)
{
    int i;

    if (!g_fresh_reset || !g_wire.clear)
        return;

    for (i = 0; i < 10; i++)
    {
        char* pl = player_at(ta, i);
        char* ai = pl ? *(char**)(pl + PL_AIOBJ) : 0;

        if (ai && readable(ai + AI_SCAN, 4))
            *(char**)(ai + AI_SCAN) = 0;
    }
}

static void apply_now(void)
{
    char* ta = ta_base();
    int   bad;

    if (!ta)
    {
        scn_err("no game structure -- apply needs a running game, not the menu");
        return;
    }

    sched_begin(ta);
    load_map_extents(ta);
    g_gametime = *(int*)(ta + OFF_GAMETIME);
    g_limit_live = *(unsigned short*)(ta + OFF_LIMIT);
    g_perplayer_live = *(unsigned short*)(ta + OFF_PERPLAYER);

    {
        char* beg = *(char**)(ta + OFF_BEGIN);
        char* end = *(char**)(ta + OFF_END);
        if (readable(beg, UNIT_STRIDE) && end >= beg &&
            (size_t)(end - beg) <= (size_t)UNIT_STRIDE * 20000)
            g_cap = (int)((end - beg) / UNIT_STRIDE) + 1;
    }

    if (map_w <= 0 || map_h <= 0)
    {
        scn_err("the map has no extents yet -- is the game past its loading screen?");
        return;
    }

    bad = resolve_all(ta);

    if (bad && g_wire.abort)
    {
        scn_err("%d entities did not resolve and on_error is abort: "
                "nothing was created", bad);
        return;
    }

    apply_switches(ta);
    apply_objective(ta);

    if (g_wire.clear)
    {
        snapshot_existing(ta);
        clear_snapshot(ta);
        delay_cancel_cleared();
        sched_note_cleared();
    }

    /* after the clear: its commander kills clear +0x149 bit 0 (apply_players) */
    apply_players(ta);

    if (g_wire.clearfeat)
        clear_features();

    /* THE INVARIANT a harness's apply that clears rests on: from here on the simulation RNG, the
       wind and the meteors are functions of the wire's seed alone, whatever the level played
       before, and the game thread's CRT stream is seeded from it (the draw path draws that stream
       too) (tagpu_patches.c, A RUN'S SEEDS; nothing under a player's launch). Before the creates, so that what they draw -- a structure's heading, the hover
       phase +0xAA (0x485D22) -- comes from the seeded state too. packcheck's cluster null rests on
       it (research/notes/modules/tasim-calibration.md, *The equivalence proof*): two calibration
       runs, or two packed batches, with different seeds share no random input wherever their
       lane-launches place them. A packed batch's scheduled creates are not seeded again: one RNG
       and one wind vector serve every cell, so a reseed at cell k's create would reseed the
       running cells' draws with it and make no cell independent of another. */
    if (g_fresh_reset && g_wire.clear)
        tagpu_seed_run(g_wire.seed);
    create_units();
    reset_pass_results(ta);
    reset_weapon_scans(ta);
    create_features();
    issue_orders();
    resolve_camera();

    g_delayed_result = g_ndelay;
    g_sched_result = g_sc_pending;
    g_sched_ord_result = g_nsched_ord;

    if (!g_ndelay && g_dl.of)
        delay_report();
    if (!g_sc_pending && g_sc.of)
        sched_report();
}

/* --------------------------------------------------------------- the detour
   Classic 5-byte E9 at a byte-matched site, jumping to a runtime-generated stub:
   pushad -> call C -> popad -> the stolen prologue -> jmp back. pushad/popad
   bracket the call, so the stolen `mov eax, ds:0x511DE8` re-executes exactly as
   the untouched code would, then control resumes past it.

   Incoming EFLAGS are dead at the resume point (0x4969D7 is lea/call/mov/mov,
   and the first flag consumer at 0x4969E9 writes them first), so they need no
   saving — the same argument tagpu_tracer.c makes for its two sites. */

static void __cdecl scn_tick(void)
{
    /* the creates first: an order they hand to the delayed table counts its wait from this
       visit, which delay_watch then looks at with the entries already there (delay_add) */
    sched_watch();
    delay_watch();

    if (InterlockedCompareExchange(&g_state, ST_APPLYING, ST_ARMED) != ST_ARMED)
        return;

    apply_now();
    InterlockedExchange(&g_state, ST_DONE);
}

static int install_tick_hook(void)
{
    unsigned char* t = (unsigned char*)TICK_SITE;
    unsigned char* stub;
    unsigned char* p;
    DWORD          old;
    int            rel;

    if (memcmp(t, TICK_STOLEN, sizeof TICK_STOLEN) != 0)
    {
        scnlog("scenario: 0x4969D2 does not hold the expected bytes -- "
               "wrong TotalA.exe build; the applier stays disarmed");
        return 0;
    }

    stub = (unsigned char*)VirtualAlloc(NULL, 0x100, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);

    if (!stub)
    {
        scnlog("scenario: VirtualAlloc(stub) failed -- the applier stays disarmed");
        return 0;
    }

    p = stub;
    *p++ = 0x60;                                            /* pushad            */
    *p++ = 0xE8;                                            /* call rel32        */
    rel = (int)((unsigned)&scn_tick - ((unsigned)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x61;                                            /* popad             */
    memcpy(p, TICK_STOLEN, sizeof TICK_STOLEN);             /* stolen prologue   */
    p += sizeof TICK_STOLEN;
    *p++ = 0xE9;                                            /* jmp rel32 (resume)*/
    rel = (int)((TICK_SITE + sizeof TICK_STOLEN) - ((unsigned)p + 4));
    memcpy(p, &rel, 4);

    if (!VirtualProtect(t, sizeof TICK_STOLEN, PAGE_EXECUTE_READWRITE, &old))
    {
        scnlog("scenario: VirtualProtect failed -- the applier stays disarmed");
        return 0;
    }

    t[0] = 0xE9;
    rel = (int)((unsigned)stub - (TICK_SITE + 5));
    memcpy(t + 1, &rel, 4);
    VirtualProtect(t, sizeof TICK_STOLEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, sizeof TICK_STOLEN);

    scnlogf("scenario: applier armed, tick detour at 0x%08X -> stub %p",
            TICK_SITE, (void*)stub);
    return 1;
}

/* -------------------------------------------------------------- the reporting
   Back on the present path: the result is written tmp + rename so the CLI's poll
   can never read a half-built file, exactly like tagpu_ui and tagpu_cat. */

static void write_switch_bits(FILE* f, unsigned value)
{
    int i;

    fprintf(f, "{");

    for (i = 0; tasim_wire_switches[i].name; i++)
        fprintf(f, "%s\"%s\":%s", i ? "," : "", tasim_wire_switches[i].name,
                (value & tasim_wire_switches[i].bit) ? "true" : "false");

    fprintf(f, "}");
}

static void write_result(void)
{
    FILE* f = fopen(SCN_TMP, "wb");
    int   i, ok, first;

    if (!f)
    {
        scnlog("scenario: cannot open the result temp file");
        return;
    }

    ok = (g_errs.n == 0 && g_errs.dropped == 0);

    fprintf(f, "{\"ok\":%d,\"frame\":%u,\"gametime\":%d", ok ? 1 : 0, g_frame, g_gametime);
    fprintf(f, ",\"seed\":%u", g_wire.seed);
    fprintf(f, ",\"requested\":{\"units\":%d,\"features\":%d,\"orders\":%d}",
            g_wire.nunits, g_wire.nfeats, g_wire.norders);
    fprintf(f, ",\"applied\":%d,\"failed\":%d,\"cleared\":%d", g_applied, g_failed,
            g_cleared);
    fprintf(f, ",\"orders\":{\"issued\":%d,\"failed\":%d}", g_ord_ok, g_ord_fail);
    fprintf(f, ",\"delayed\":%d,\"features_cleared\":%s", g_delayed_result,
            g_feat_cleared ? "true" : "false");
    fprintf(f, ",\"scheduled\":{\"units\":%d,\"orders\":%d}", g_sched_result, g_sched_ord_result);
    if (g_objective_done >= 0)
        fprintf(f, ",\"objective\":\"%s\"", g_objective_done ? "on" : "off");
    fprintf(f, ",\"cleared_slots\":[");
    for (i = 0; i < g_ncleared_slots; i++)
        fprintf(f, "%s%d", i ? "," : "", g_cleared_slot[i]);
    fprintf(f, "]");
    fprintf(f, ",\"map\":{\"name\":");
    jstr(f, g_wire.map, sizeof g_wire.map);
    fprintf(f, ",\"world\":[%d,%d],\"tiles\":[%d,%d]}", map_w, map_h, fmap_x, fmap_y);
    fprintf(f, ",\"limit\":{\"requested\":%s,\"total\":%d,\"per_player\":%d,"
               "\"array_slots\":%d}",
            g_wire.limit == TASIM_WIRE_UNSET ? "null" : "0", g_limit_live, g_perplayer_live, g_cap);
    if (g_wire.limit != TASIM_WIRE_UNSET)
        fprintf(f, ",\"limit_requested\":%d", g_wire.limit);

    fprintf(f, ",\"switches\":{\"before\":%u,\"after\":%u,\"bits\":",
            g_sw_before, g_sw_after);
    write_switch_bits(f, g_sw_after);
    fprintf(f, "}");

    fprintf(f, ",\"units\":{");
    first = 1;
    for (i = 0; i < g_wire.nunits; i++)
    {
        const TASIM_WIRE_UNIT* wu = &g_wire.units[i];
        scn_unit* u = &g_units[i];
        if (!u->unit)
            continue;
        fprintf(f, "%s\"%d\":{\"engine_index\":%d,\"owner\":%d,\"type\":",
                first ? "" : ",", i, u->engineIdx, wu->owner);
        jstr(f, wu->type, TASIM_WIRE_NAMELEN);
        fprintf(f, ",\"requested\":[%d,%d],\"actual\":[%d,%d,%d]",
                wu->x, wu->y, u->ax, u->ay, u->ah);
        if (wu->fire != TASIM_WIRE_UNSET && u->fire_got >= 0)
            fprintf(f, ",\"fire\":%d", u->fire_got);
        else if (wu->fire != TASIM_WIRE_UNSET)
            fprintf(f, ",\"fire\":null");
        if (wu->stock != TASIM_WIRE_UNSET && u->stock_got >= 0)
            fprintf(f, ",\"stock\":%d", u->stock_got);
        else if (wu->stock != TASIM_WIRE_UNSET)
            fprintf(f, ",\"stock\":null");
        if (u->queued >= 0)
            fprintf(f, ",\"buildweapon\":%d", u->queued);
        fprintf(f, "}");
        first = 0;
    }
    fprintf(f, "}");

    fprintf(f, ",\"features\":{");
    first = 1;
    for (i = 0; i < g_wire.nfeats; i++)
    {
        const TASIM_WIRE_FEAT* wf = &g_wire.feats[i];
        scn_feat* fe = &g_feats[i];
        if (!fe->ok)
            continue;
        fprintf(f, "%s\"%d\":{\"def\":%d,\"type\":", first ? "" : ",", i, fe->defIdx);
        jstr(f, wf->type, TASIM_WIRE_NAMELEN);
        fprintf(f, ",\"requested\":[%d,%d],\"actual\":[%d,%d,%d]}",
                wf->x, wf->y, fe->ax, fe->ay, fe->ah);
        first = 0;
    }
    fprintf(f, "}");

    if (g_wire.npl)
    {
        char* ta = ta_base();
        fprintf(f, ",\"players\":{");
        for (i = 0; i < g_wire.npl; i++)
        {
            char* pl = ta ? player_at(ta, g_wire.pl[i].slot) : 0;
            fprintf(f, "%s\"%d\":{", i ? "," : "", g_wire.pl[i].slot);
            fprintf(f, "\"requested\":{\"metal\":%d,\"energy\":%d}",
                    g_wire.pl[i].metal == TASIM_WIRE_UNSET ? -1 : g_wire.pl[i].metal,
                    g_wire.pl[i].energy == TASIM_WIRE_UNSET ? -1 : g_wire.pl[i].energy);
            fprintf(f, ",\"wrote\":{\"metal\":%d,\"energy\":%d}",
                    g_pl_got_metal[i] == TASIM_WIRE_UNSET ? -1 : g_pl_got_metal[i],
                    g_pl_got_energy[i] == TASIM_WIRE_UNSET ? -1 : g_pl_got_energy[i]);
            if (pl)
                fprintf(f, ",\"now\":{\"metal\":%d,\"energy\":%d}",
                        (int)*(float*)(pl + PL_METAL), (int)*(float*)(pl + PL_ENERGY));
            if (g_wire.pl[i].hasstore && g_pl_got_mstore[i] >= 0.0f)
                fprintf(f, ",\"storage\":[%.9g,%.9g]", (double)g_pl_got_mstore[i],
                        (double)g_pl_got_estore[i]);
            else if (g_wire.pl[i].hasstore)
                fprintf(f, ",\"storage\":null");
            if (g_pl_ai_done[i])
                fprintf(f, ",\"ai\":\"off\"");
            fprintf(f, "}");
        }
        fprintf(f, "}");
    }

    if (g_probe_have)
        fprintf(f, ",\"order_probe\":{\"passed\":[%d,%d,%d],\"stored\":[%d,%d,%d]}",
                g_probe_pass[0], g_probe_pass[1], g_probe_pass[2],
                g_probe_stored[0], g_probe_stored[1], g_probe_stored[2]);

    if (g_cam_have)
        fprintf(f, ",\"camera\":[%d,%d],\"pin\":%d", g_cam_x, g_cam_y, g_wire.cam_pin);
    else
        fprintf(f, ",\"camera\":null,\"pin\":0");

    if (g_eye_have)
        fprintf(f, ",\"eye\":[%d,%d]", g_eye_x, g_eye_y);
    else
        fprintf(f, ",\"eye\":null");

    fprintf(f, ",\"errors\":[");
    for (i = 0; i < g_errs.n; i++)
    {
        if (i) fputc(',', f);
        jstr(f, g_errs.msg[i], TASIM_WIRE_ERRLEN);
    }
    fprintf(f, "]");

    if (g_errs.dropped)
        fprintf(f, ",\"errors_dropped\":%d", g_errs.dropped);

    fprintf(f, "}\n");
    fclose(f);

    if (!MoveFileExA(SCN_TMP, SCN_OUT, MOVEFILE_REPLACE_EXISTING))
        scnlog("scenario: could not replace the result file");
}

/* The camera is display state, not sim state, so it is set here rather than on
   the tick. GAME THREAD: tagpu_scenario_frame runs only from
   tagpu_triggers_frame, whose host is the flip observer `before_flip`
   (tagpu_gui_hook.c) on `0x4C63A0`, game thread only and behind a 16-ms
   clock gate, at whichever of the flip's 44 call sites comes next — inside
   DrawGameScreen at `0x46A3DB`, after that draw's fog site, or anywhere else
   the engine flips — and, on an undrawn frame of a no-draw test, the frame
   visit at `0x4969CD` in place of the draw (tagpu_gui_nodraw_frame), under the
   same gate. So the eye is NOT written here: tagpu_zoom_place_eye
   clamps it into the camera's range in force and hands it to the command
   apply at the top of the next in-play draw, which writes the eye and the
   scroll target before that draw reads the eye or builds its fog grid,
   recomputes the minimap's view box, sets its dirty bit and invalidates the
   screen fog grid — paused or not. The engine's grid is therefore rebuilt at
   the new eye in that draw; our passes read the whole map's fog source,
   which does not depend on the eye. `at` and `center_on` both
   mean the CENTRE of the window, so the eye is the centre minus half a view,
   using the projection the roster already reports. A `pin` then holds
   exactly the eye handed over. */
static void place_camera(const TAGPU_FRAME* f)
{
    char* ta = ta_base();
    int   gw, gh, ex, ey;

    if (!g_cam_have || !ta)
        return;

    gw = f && f->game_width  > 0 ? f->game_width  : 640;
    gh = f && f->game_height > 0 ? f->game_height : 480;

    /* The overlay's own projection, inverted:
           sx = wx - eyeX + 128
           sy = wy - altitude/2 - eyeY + 32
       so the target's HEIGHT belongs in the answer — without it a unit on a hill
       sits half its altitude above the middle of the window. */
    ex = g_cam_x - gw / 2 + 128;
    ey = g_cam_y - g_cam_h / 2 - gh / 2 + 32;

    if (!tagpu_zoom_place_eye(ta, ex, ey, &ex, &ey))
        return;

    /* Reported so `pin` can hold exactly this eye through tagpu_eye.txt rather
       than re-deriving the projection in a second place. */
    g_eye_x = ex;
    g_eye_y = ey;
    g_eye_have = 1;
}

static char* read_trigger(void)
{
    HANDLE h = CreateFileA(SCN_TRIGGER, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  size, got = 0;
    char*  buf;

    if (h == INVALID_HANDLE_VALUE)
        return 0;

    size = GetFileSize(h, NULL);

    if (size == INVALID_FILE_SIZE || size > SCN_FILEMAX)
    {
        CloseHandle(h);
        return 0;
    }

    buf = (char*)malloc((size_t)size + 1);

    if (!buf)
    {
        CloseHandle(h);
        return 0;
    }

    if (!ReadFile(h, buf, size, &got, NULL))
        got = 0;

    CloseHandle(h);
    buf[got] = 0;
    return buf;
}

/* ------------------------------------------------------------------- switches
   `tacli switches` — the same SoftwareDebugMode word, on its own trigger, so a
   switch can be A/B'd on a live instance with no scenario at all. It works at
   the menus too: the word lives in the main struct, which exists before a game. */
static void do_switches(void)
{
    char*           ta = *(char**)TA_MAINPP;
    HANDLE          h;
    DWORD           got = 0;
    char            buf[512];
    unsigned short* sw;
    unsigned        before, after;
    FILE*           out;
    char*           p;
    int             bad = 0;
    char            badname[32];

    badname[0] = 0;

    h = CreateFileA(SW_TRIGGER, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (h != INVALID_HANDLE_VALUE)
    {
        if (!ReadFile(h, buf, sizeof buf - 1, &got, NULL))
            got = 0;
        CloseHandle(h);
    }

    buf[got] = 0;
    DeleteFileA(SW_TRIGGER);

    out = fopen(SW_TMP, "wb");

    if (!out)
        return;

    if (!readable(ta, OFF_SWITCHES + 2))
    {
        fprintf(out, "{\"ok\":0,\"note\":\"no game structure yet\"}\n");
        fclose(out);
        MoveFileExA(SW_TMP, SW_OUT, MOVEFILE_REPLACE_EXISTING);
        return;
    }

    sw = (unsigned short*)(ta + OFF_SWITCHES);
    before = *sw;
    after = before;

    for (p = buf; *p; )
    {
        char* t;
        char* eol = strchr(p, '\n');
        char* cur = p;

        if (eol) { *eol = 0; p = eol + 1; } else { p = cur + strlen(cur); }

        while ((t = tasim_wire_tok(&cur)) != 0)
        {
            char* k;
            int   v, i, found = 0;

            if (t[0] == '#' || !tasim_wire_kv(t, &k, &v))
                break;

            for (i = 0; tasim_wire_switches[i].name; i++)
                if (!lstrcmpiA(k, tasim_wire_switches[i].name))
                {
                    if (v) after |= tasim_wire_switches[i].bit;
                    else   after &= ~tasim_wire_switches[i].bit;
                    found = 1;
                    break;
                }

            if (!found)
            {
                bad = 1;
                _snprintf(badname, sizeof badname - 1, "%s", k);
                badname[sizeof badname - 1] = 0;
            }
        }
    }

    if (!bad)
        *sw = (unsigned short)after;

    fprintf(out, "{\"ok\":%d,\"before\":%u,\"after\":%u,\"bits\":", bad ? 0 : 1,
            before, *sw);
    write_switch_bits(out, *sw);

    if (bad)
    {
        fprintf(out, ",\"error\":");
        jstr(out, badname, sizeof badname);
    }

    fprintf(out, "}\n");
    fclose(out);

    if (!MoveFileExA(SW_TMP, SW_OUT, MOVEFILE_REPLACE_EXISTING))
        scnlog("scenario: could not replace the switches file");
}

/* --------------------------------------------------------------- the per-frame
   Same cadence as the other on-demand modules: the CLI polls the result in an
   auto-wait loop, so a five-frame worst case is what it feels like. */

void tagpu_scenario_frame(const TAGPU_FRAME* f)
{
    if (f && (f->frame_counter % 5))
        return;

    g_frame = f ? f->frame_counter : 0;

    if (GetFileAttributesA(SW_TRIGGER) != INVALID_FILE_ATTRIBUTES)
        do_switches();

    if (g_state == ST_DONE)
    {
        place_camera(f);
        write_result();
        scnlogf("scenario: applied=%d failed=%d cleared=%d orders=%d/%d errors=%d",
                g_applied, g_failed, g_cleared, g_ord_ok, g_ord_ok + g_ord_fail,
                g_errs.n + g_errs.dropped);
        InterlockedExchange(&g_state, ST_IDLE);
        return;
    }

    if (g_state == ST_ARMED && g_frame - g_armed_at > SCN_ARM_FRAMES)
    {
        /* The tick may claim the arena at any moment; only report the give-up if
           we won the race for it. */
        if (InterlockedCompareExchange(&g_state, ST_IDLE, ST_ARMED) == ST_ARMED)
        {
            scn_err("the game's main loop never reached the apply point in %u frames "
                    "-- apply needs a level in play, not the menus or the mission-end "
                    "screen", SCN_ARM_FRAMES);
            write_result();
        }
        return;
    }

    if (g_state != ST_IDLE)
        return;                              /* a tick still owns the arena */

    if (GetFileAttributesA(SCN_TRIGGER) == INVALID_FILE_ATTRIBUTES)
        return;

    {
        char* text = read_trigger();
        int   parsed;

        DeleteFileA(SCN_TRIGGER);

        if (!text)
        {
            arena_reset();
            scn_err("the trigger file could not be read (missing, empty or over %u bytes)",
                    SCN_FILEMAX);
            write_result();
            return;
        }

        arena_reset();
        parsed = tasim_wire_parse(&g_wire, text, &g_errs);
        results_reset();
        free(text);

        if (!parsed)
        {
            write_result();                  /* the errors say which line was wrong */
            return;
        }

        if (!g_hooked)
            g_hooked = install_tick_hook();

        if (!g_hooked)
        {
            scn_err("the tick detour is not installed -- nothing was applied");
            write_result();
            return;
        }

        g_armed_at = g_frame;
        InterlockedExchange(&g_state, ST_ARMED);
    }
}
