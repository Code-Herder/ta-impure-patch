/* tagpu_weapons.c — 1..N weapons per unit. Contract: inc/tagpu_weapons.h.
   Mechanism, site inventory and assertions: research/notes/extra-weapons.md.
   Every VA below is for the pristine TotalA.exe 3.1 (md5 8e74a1df…); every
   patched site is byte-matched before anything is written. */

#pragma GCC optimize("no-strict-aliasing")

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "tagpu_weapons.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed char    i8;
typedef short          i16;
typedef int            i32;

/* =========================================================================
   1. Configuration and engine map
   ========================================================================= */

#define WPN_CAP      16          /* compile-time capacity: slots 0..15        */
#define WPN_SIDE     (WPN_CAP - 3)
#define WPN_MAXDEFS  4096        /* unit types the def side table can hold   */
#define WPN_FLAG     "tagpu_weapons.on"
#define WPN_TRIGGER  "tagpu_weapons.trigger"
#define WPN_JSON     "tagpu_weapons.json"

#define TA_MAINPP        0x00511DE8u   /* TAdynmemStruct**                     */
#define OFF_UNITS_BEGIN  0x14357       /* UnitStruct* BeginUnitsArray_p        */
#define OFF_UNITS_END    0x1435B       /* UnitStruct* EndOfUnitsArray_p        */
#define OFF_DEFS         0x1439B       /* UnitDefStruct* array base            */
#define OFF_WEAPON0      0x2CF3        /* Weapons[0]: the "no weapon" entry    */
#define OFF_PROJ_COUNT   0x141F3
#define OFF_PROJ_BASE    0x141F7
#define OFF_TICK         0x38A47
#define OFF_SEALEVEL     0x1427F       /* u8                                   */
#define OFF_LIMIT30      0x37EE6       /* u16 the acquisition loop divides by 30 */

#define UNIT_STRIDE  0x118
#define DEF_STRIDE   0x249
#define PROJ_STRIDE  0x6B
#define SLOT_STRIDE  0x1C
#define COB_PRESET   ((void*)0x4FD6F0u) /* what the create paths put in slot->thread */

#define TA()      (*(char**)TA_MAINPP)
#define UDEF(u)   (*(char**)((u) + 0x92))
#define UCOB(u)   (*(void**)((u) + 0x9A))
#define UPLAYER(u) (*(char**)((u) + 0x96))

/* engine strings, used verbatim so the COB engine sees the same pointers */
#define S_SETMAXRELOAD  ((const char*)0x509704u)
#define S_TARGETCLEARED ((const char*)0x508D58u)
#define S_STARTBUILDING ((const char*)0x5050F4u)
#define TBL_AIM         ((const char**)0x509688u)  /* AimPrimary/Secondary/Tertiary */
#define TBL_FIRE        ((const char**)0x509678u)  /* FirePrimary/…                 */
#define K_ZANGLE        (*(const double*)0x4FDA98u)   /* 1.25                      */
#define K_BUILT         (*(const float*)0x4FC968u)    /* "construction complete"   */

#pragma pack(push, 1)
typedef struct WSlot {            /* the engine's 28-byte weapon slot (TADR TUnitWeapon) */
    u16   target;                 /* +00 unit index, or ground X                 */
    u16   spot;                   /* +02 0x8000 = target is a unit, else ground Z */
    void* thread;                 /* +04 COB thread handle; preset COB_PRESET    */
    i32   aimed;                  /* +08 aim-script result                       */
    char* weapon;                 /* +0C WeaponStruct*                            */
    i32   zangle;                 /* +10                                          */
    u16   reload;                 /* +14 ticks left                               */
    i16   heading;                /* +16 */
    i16   pitch;                  /* +18 */
    u8    stock;                  /* +1A */
    u8    state;                  /* +1B bit0 aiming, bit1 present, bits2-3 idx&3, bit4 may acquire */
} WSlot;
#pragma pack(pop)

typedef struct WDef {             /* per unit type: what the FBI said beyond weapon3 */
    char* def;                    /* the UnitDefStruct this record describes (validation) */
    u8    count;                  /* 3..WPN_CAP */
    char* w[WPN_SIDE];            /* weapon4.. (Weapons[0] when absent)          */
    u32*  mask[WPN_SIDE];         /* w4_badTargetCategory..                      */
    /* AimFromWeaponN / QueryWeaponN resolve to a fixed piece, so they are asked
       once per unit *type* and remembered. Running them per slot per tick is
       what tips a busy unit over the eight-thread COB cap (0x4B08C0), and a
       query that loses that race returns silence: the piece stays unset, the
       slot aims from the hull's first piece and stops tracking. Cached, the aim
       origin is a pointer chase and cannot fail. Only for slots >= 3 — a stock
       Query* may legitimately answer a different barrel each call. -1 = unasked. */
    i8    pc_aimfrom[WPN_SIDE];
    i8    pc_query[WPN_SIDE];
} WDef;

/* =========================================================================
   2. State, logging, hit counters
   ========================================================================= */

static int    g_armed;
static WDef*  g_def;              /* [WPN_MAXDEFS], indexed by def array index  */
static WSlot* g_side;             /* [rows][WPN_SIDE]                            */
static u32    g_side_rows;
static char*  g_side_begin;       /* the unit array the rows were sized for      */
static char*  g_side_end;
static u8*    g_pool;             /* RWX pool for trampolines and stubs          */
static u32    g_pool_used;
static char   g_name_aim[WPN_CAP][24], g_name_fire[WPN_CAP][24],
              g_name_query[WPN_CAP][24], g_name_aimfrom[WPN_CAP][24];

enum { H_START, H_AUTOAIM, H_NAMES, H_RETALIATE, H_ACQUIRE, H_HELPERS, H_SPLICE,
       H_LOADER, H_VIOLATION, H_STOCKSPLICE, H_MISMATCH, H_GROUND, H_COBFULL,
       H_HOLDFIRE, H_CRC, H__N };
static const char* const HIT_NAMES[H__N] =
    { "start", "autoaim", "names", "retaliate", "acquire", "helpers", "splice",
      "loader", "violation", "stock_splice", "mismatch", "ground", "cob_full",
      "hold_fire", "crc" };
/* cob_full: AimFromWeaponN/QueryWeaponN resolved to no piece. COBEngine_QueryScript
   (0x4B0BC0 -> 0x4B0C40) opens with a thread-slot allocation (0x4B08C0) that scans
   a fixed EIGHT records of 0xA4 bytes at cob+0x1C and returns -1 when they are all
   busy; on -1 it returns without touching the caller's out-parameter. So a unit
   already running eight scripts gets silence, not an error, and the aim origin
   falls back to piece 0 -- every starved slot then computes the same wrong
   solution from the hull's first piece and stops tracking. Nonzero here means the
   unit's scripts are over that cap; see research/notes/extra-weapons.md.
   hold_fire: shots withheld because the barrel had not slewed onto the target. */
/* stock_splice counts splice stubs taken for slots 0-2 (the stock path); mismatch
   counts the times the pointer-derived index disagreed with the engine's own
   2-bit field there — the live form of "the stub computes what the engine
   computed" (assertion 3). It must stay 0. */
static u32 g_hits[H__N];
static u32 g_fires[WPN_CAP];     /* projectile launches per slot index (one fire-name lookup each) */
#define HIT(h) (g_hits[h]++)

static void wlog(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    FILE* f;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "weapons: %s\n", b); fclose(f); }
}

/* =========================================================================
   3. Side tables
   ========================================================================= */

/* The def record for a UnitDefStruct, or NULL for a stock type. The key is the
   def's index in the engine's array: the game-start loader (0x42D2E0) compacts
   the array, numbers it, and only then runs the FBI loader we detour on each
   final slot, so index == UnitTypeID for the life of the game. The record keeps
   the def pointer it was written for and answers NULL on a mismatch, which is
   what makes a stale row from an earlier game read as "stock". */
static WDef* def_rec(const char* def)
{
    char* ta = TA();
    char* base;
    u32   idx;
    if (!g_def || (size_t)ta < 0x600000u) return 0;
    base = *(char**)(ta + OFF_DEFS);
    if (!base || def < base) return 0;
    idx = (u32)(def - base) / DEF_STRIDE;
    if (idx >= WPN_MAXDEFS) return 0;
    return g_def[idx].def == def ? &g_def[idx] : 0;
}

static int def_count(const char* def)
{
    WDef* r = def_rec(def);
    return r ? r->count : 3;
}

static int wpn_count(const char* unit) { return def_count(UDEF(unit)); }

static char* def_weapon(const char* def, int i)
{
    WDef* r;
    if (i < 3) return *(char**)(def + 0x1EE + 4 * i);
    r = def_rec(def);
    if (!r || i >= r->count) return TA() + OFF_WEAPON0;
    return r->w[i - 3];
}

static u32* def_mask(const char* def, int i)
{
    WDef* r;
    if (i < 3) return *(u32**)(def + 0x231 + 4 * i);
    r = def_rec(def);
    if (!r || i >= r->count) return *(u32**)(def + 0x231);   /* wpri_ as a safe stand-in */
    return r->mask[i - 3];
}

static int mask_has(const u32* mask, u16 type)
{
    return (mask[type >> 5] >> (type & 31)) & 1;
}

/* Unit side rows are sized from the live unit array and rebuilt when the array
   moves or resizes (a new game). Rows are reset when a unit is created. */
static WSlot* side_row(const char* unit)
{
    char* ta = TA();
    char* begin;
    char* end;
    u32   idx;
    if ((size_t)ta < 0x600000u) return 0;
    begin = *(char**)(ta + OFF_UNITS_BEGIN);
    end   = *(char**)(ta + OFF_UNITS_END);
    if ((size_t)begin < 0x600000u || end <= begin) return 0;
    if (begin != g_side_begin || end != g_side_end || !g_side)
    {
        u32 rows = (u32)(end - begin) / UNIT_STRIDE + 1;
        WSlot* n = (WSlot*)VirtualAlloc(NULL, (size_t)rows * WPN_SIDE * sizeof(WSlot),
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!n) { wlog("side table: VirtualAlloc(%u rows) failed", rows); return 0; }
        if (g_side) VirtualFree(g_side, 0, MEM_RELEASE);
        g_side = n; g_side_rows = rows; g_side_begin = begin; g_side_end = end;
        wlog("side table: %u rows x %d slots (%u KB) for units %p..%p",
             rows, WPN_SIDE, (u32)(rows * WPN_SIDE * sizeof(WSlot) / 1024), begin, end);
    }
    if (unit < begin) return 0;
    idx = (u32)(unit - begin) / UNIT_STRIDE;
    if (idx >= g_side_rows) return 0;
    return g_side + (size_t)idx * WPN_SIDE;
}

static void side_reset(const char* unit)
{
    WSlot* row = side_row(unit);
    int k;
    if (!row) return;
    memset(row, 0, WPN_SIDE * sizeof(WSlot));
    for (k = 0; k < WPN_SIDE; k++)
    {
        row[k].spot   = 0x8000;         /* no target (the engine's own idle value) */
        row[k].thread = COB_PRESET;
    }
}

/* =========================================================================
   4. Slot addressing — the two helpers the whole design rests on
   ========================================================================= */

/* SlotPtr: exactly unit + 4 + i*0x1C for i < 3, the side row otherwise. */
static WSlot* slot_ptr(char* unit, int i)
{
    WSlot* row;
    if ((unsigned)i < 3u) return (WSlot*)(unit + 4 + i * SLOT_STRIDE);
    if (i >= wpn_count(unit) || i >= WPN_CAP)
    {
        HIT(H_VIOLATION);
        if (g_hits[H_VIOLATION] <= 20)
            wlog("VIOLATION slot_ptr(unit %p type %.12s, idx %d, count %d) — clamped to slot 0",
                 unit, UDEF(unit) + 0x20, i, wpn_count(unit));
        return (WSlot*)(unit + 4);
    }
    row = side_row(unit);
    if (!row)
    {
        HIT(H_VIOLATION);
        return (WSlot*)(unit + 4);
    }
    return row + (i - 3);
}

/* SlotIndex: the inverse. Derived from pointers, never from the 2-bit field. */
static int slot_index(char* unit, WSlot* slot)
{
    char*  s = (char*)slot;
    WSlot* row;
    if (s >= unit + 4 && s < unit + 4 + 3 * SLOT_STRIDE)
        return (int)(s - unit - 4) / SLOT_STRIDE;
    row = side_row(unit);
    if (row && slot >= row && slot < row + WPN_SIDE)
        return 3 + (int)(slot - row);
    HIT(H_VIOLATION);
    if (g_hits[H_VIOLATION] <= 20)
        wlog("VIOLATION slot_index(unit %p, slot %p) — not a slot of this unit; 0 returned", unit, slot);
    return 0;
}

/* =========================================================================
   5. Script names — stock strings for 0-2, Spring's names beyond
   ========================================================================= */

static const char* const STOCK_QUERY[3]   = { (const char*)0x505284u, (const char*)0x505274u, (const char*)0x505264u };
static const char* const STOCK_AIMFROM[3] = { (const char*)0x5052B8u, (const char*)0x5052A4u, (const char*)0x505294u };

static const char* aim_name(int i)     { return i < 3 ? TBL_AIM[i]        : g_name_aim[i]; }
static const char* fire_name(int i)    { return i < 3 ? TBL_FIRE[i]       : g_name_fire[i]; }
static const char* query_name(int i)   { return i < 3 ? STOCK_QUERY[i]    : g_name_query[i]; }
static const char* aimfrom_name(int i) { return i < 3 ? STOCK_AIMFROM[i]  : g_name_aimfrom[i]; }

static void build_names(void)
{
    int i;
    for (i = 3; i < WPN_CAP; i++)
    {
        _snprintf(g_name_aim[i],     24, "AimWeapon%d",     i + 1);
        _snprintf(g_name_fire[i],    24, "FireWeapon%d",    i + 1);
        _snprintf(g_name_query[i],   24, "QueryWeapon%d",   i + 1);
        _snprintf(g_name_aimfrom[i], 24, "AimFromWeapon%d", i + 1);
    }
}

/* =========================================================================
   6. Engine functions the C ports call (conventions read off the call sites)
   ========================================================================= */

typedef void  (__thiscall *PFN_StartScript)(void* cob, const char* name, void* thread, int, int, int, int, int, int);
typedef void  (__thiscall *PFN_QueryScript)(void* cob, const char* name, int* result, int, int, int);
typedef int   (__thiscall *PFN_Name2Index)(void* cob, const char* name);
typedef int*  (__stdcall  *PFN_PiecePos)(int* out, char* unit, int piece);
typedef char* (__stdcall  *PFN_Name2Ptr)(const char* name);
typedef void  (__thiscall *PFN_TdfGetStr)(void* ctx, char* buf, const char* key, int len, const char* dflt);
typedef char* (__thiscall *PFN_TdfValue)(void* ctx, const char* key);
typedef void  (__thiscall *PFN_TdfIterA)(void* ctx);
typedef int   (__thiscall *PFN_TdfSection)(void* ctx, const char* name);
typedef u32*  (__stdcall  *PFN_Categories)(const char* s);
typedef void  (__stdcall  *PFN_SendStart)(char* unit, const char* name, int, int, int, int, int);
typedef int   (__stdcall  *PFN_AimCalc)(char* unit, char* weapon, i16* heading, i16* pitch, u32 idx, int* tpos);
typedef int   (__cdecl    *PFN_Atan2)(int, int);
typedef int   (__stdcall  *PFN_Pitch)(int, int, int, int range, float);
typedef void  (__stdcall  *PFN_Unit)(char* unit);
typedef void  (__thiscall *PFN_Res2)(void* self, int, int);
typedef void  (__stdcall  *PFN_UnitInt)(char* unit, int);
typedef int   (__stdcall  *PFN_Rand)(int n);
typedef int   (__stdcall  *PFN_Order)(char* unit, char* target, int);
typedef u32   (__stdcall  *PFN_UnitFlags)(char* unit);
typedef void  (__stdcall  *PFN_Unit3)(char* unit, int, int);
typedef int   (__stdcall  *PFN_FireCB)(char* unit, WSlot* slot, char* target, int* tpos);
typedef int   (__stdcall  *PFN_TargetPos)(char* unit, int* out, int idx);      /* 0x48A1E0, spliced   */
typedef char* (__stdcall  *PFN_FindTarget)(char* unit, u32 idx, int);          /* 0x40B7B0, spliced   */

#define E_StartScript ((PFN_StartScript)0x4B0A70u)
#define E_QueryScript ((PFN_QueryScript)0x4B0BC0u)
/* 0x4B07C0 walks the COB name table and returns the script's index, or -1.
   Stock's clear-target calls it here and throws the answer away; kept
   call-for-call. It is NOT a stop-script, whatever the old name said. */
#define E_Name2Index  ((PFN_Name2Index)0x4B07C0u)
#define E_PiecePos    ((PFN_PiecePos)   0x43DEF0u)
#define E_Name2Ptr    ((PFN_Name2Ptr)   0x49E5B0u)
#define E_TdfGetStr   ((PFN_TdfGetStr)  0x4C48C0u)
/* the three the unit-info loader uses to turn "weaponN"'s value into that
   weapon's TDF CRC: read the key, rewind a weapon-TDF context, ask it for
   the section. */
#define E_TdfValue    ((PFN_TdfValue)   0x4C4630u)
#define E_TdfIterA    ((PFN_TdfIterA)   0x4C3E10u)
#define E_TdfSection  ((PFN_TdfSection) 0x4C3410u)

/* Every weapon TDF (Weapons\\*.tdf) parsed at startup: an array of 12-byte contexts, whose
   +4 is the section the last SectionExists landed on and whose +0x25 is that
   section's stored CRC. Built by FUN_0042A8D0 itself, just above the folds. */
#define WTDF_BASE     (*(char**)0x5122A0u)
#define WTDF_COUNT    (*(int*)  0x5122A4u)
#define DEF_CRC_WPN   0x146            /* UnitDefStruct.CRC_weapons */
#define DEF_CRC_ALL   0x142            /* UnitDefStruct.CRC_all (0x146 folded in) */
#define E_Categories  ((PFN_Categories) 0x488C50u)
#define E_SendStart   ((PFN_SendStart)  0x456200u)
#define E_AimCalc     ((PFN_AimCalc)    0x49D910u)
#define E_Atan2       ((PFN_Atan2)      0x4B715Au)
#define E_Pitch       ((PFN_Pitch)      0x49A890u)
#define E_Stockpiled  ((PFN_Unit)       0x41C150u)
#define E_ChargeRes   ((PFN_Res2)       0x4012A0u)
#define E_Notify      ((PFN_UnitInt)    0x4897B0u)
#define E_ClearBuild  ((PFN_UnitInt)    0x439EB0u)
#define E_Rand        ((PFN_Rand)       0x4B6C30u)
#define E_AttackOrder ((PFN_Order)      0x43B1F0u)
#define E_UnitFlags   ((PFN_UnitFlags)  0x438BE0u)
#define E_Reaction    ((PFN_Unit3)      0x47F850u)
#define E_TargetPos   ((PFN_TargetPos)  0x48A1E0u)
#define E_FindTarget  ((PFN_FindTarget) 0x40B7B0u)

/* the replaced functions, reached through their trampolines for the stock path */
typedef void  (__stdcall  *PFN_StartWeapons)(char* unit);
typedef void  (__stdcall  *PFN_AutoAim)(char* unit);
typedef void  (__stdcall  *PFN_CallAim)(char* unit, int* out, u32 idx);
typedef void  (__stdcall  *PFN_QueryPos)(char* unit, int* out, u32 idx, int piece);
typedef int   (__stdcall  *PFN_QueryPiece)(char* unit, u32 idx);
typedef void  (__stdcall  *PFN_Retaliate)(char* attacker, char* victim, int unused);
typedef int   (__thiscall *PFN_Acquire)(char* self, int param);
typedef u8    (__fastcall *PFN_FirstWeapon)(char* unit, int unused);
typedef void  (__thiscall *PFN_SlotN)(char* unit, int n);
typedef void  (__stdcall  *PFN_SetTarget)(char* unit, char* target, int idx);
typedef void  (__stdcall  *PFN_SetGround)(char* unit, int* pos, int idx);
typedef void  (__stdcall  *PFN_UnitIdx)(char* unit, int idx);
typedef int   (__stdcall  *PFN_Range)(char* unit, u32 idx);
typedef char* (__stdcall  *PFN_TargetUnit)(char* unit, int idx);
typedef char* (__stdcall  *PFN_Intercept)(char* unit, u32 idx);
typedef int   (__stdcall  *PFN_Check)(char* unit, char* target, u32 idx);
typedef int   (__stdcall  *PFN_Traj)(char* unit, int* upos, int* tpos, u32 idx);
typedef void  (__thiscall *PFN_DefCopy)(void* dst, int src);

static PFN_StartWeapons o_StartWeapons;
static PFN_AutoAim      o_AutoAim;
static PFN_CallAim      o_CallAim;
static PFN_QueryPos     o_QueryPos;
static PFN_QueryPiece   o_QueryPiece;
static PFN_Retaliate    o_Retaliate;
static PFN_Acquire      o_Acquire;
static PFN_FirstWeapon  o_FirstWeapon;
static PFN_SlotN        o_ClearTargetN, o_EnableSlotN;
static PFN_SetTarget    o_SetTarget;
static PFN_SetGround    o_SetGround;
static PFN_UnitIdx      o_ClearTarget, o_ClearTargetQuiet;
static PFN_Range        o_Range;
static PFN_TargetUnit   o_TargetUnit;
static PFN_Intercept    o_Intercept;
static PFN_Check        o_Check;
static PFN_Traj         o_Traj;
static PFN_DefCopy      o_DefCopy;

/* =========================================================================
   7. The C ports — entered only for extended units / slots >= 3
   ========================================================================= */

/* -- the three name helpers (0x43E2E0, 0x43E240, 0x43E1E0) ---------------- */

/* The piece AimFromWeaponN (kind 0) or QueryWeaponN (kind 1) names, cached per
   unit type. Returns -1 only while the COB engine is too busy to answer, and
   the callers then fall back exactly as stock does. */
static int slot_piece(char* u, int i, int kind)
{
    WDef* r = def_rec(UDEF(u));
    i8*   c;
    int   piece;
    if (!r || i < 3 || i - 3 >= WPN_SIDE) return -1;
    c = kind ? &r->pc_query[i - 3] : &r->pc_aimfrom[i - 3];
    if (*c >= 0) return *c;
    piece = -1;
    E_QueryScript(UCOB(u), kind ? query_name(i) : aimfrom_name(i), &piece, 0, 0, 0);
    if (piece >= 0 && piece < 127) *c = (i8)piece;
    return piece;
}

static void __stdcall my_CallAimScripts(char* u, int* out, u32 idx)
{
    int piece, tmp[3], *p;
    idx &= 0xFF;
    if (idx < 3) { o_CallAim(u, out, idx); return; }
    HIT(H_NAMES);
    piece = slot_piece(u, (int)idx, 0);
    if (piece < 0) piece = slot_piece(u, (int)idx, 1);
    if (piece < 0) { HIT(H_COBFULL); piece = 0; }        /* stock's fallback, counted */
    p = E_PiecePos(tmp, u, piece);
    out[0] = *(int*)(u + 0x6A) + p[0];
    out[1] = *(int*)(u + 0x6E) + p[1];
    out[2] = *(int*)(u + 0x72) + p[2];
}

static void __stdcall my_QueryWeaponPosition(char* u, int* out, u32 idx, int piece)
{
    int tmp[3], *p;
    if ((idx & 0xFF) < 3) { o_QueryPos(u, out, idx, piece); return; }
    HIT(H_NAMES);
    if (piece < 0)
    {
        piece = slot_piece(u, (int)(idx & 0xFF), 1);
        if (piece < 0) { HIT(H_COBFULL); piece = 0; }
    }
    p = E_PiecePos(tmp, u, piece);
    out[0] = *(int*)(u + 0x6A) + p[0];
    out[1] = *(int*)(u + 0x6E) + p[1];
    out[2] = *(int*)(u + 0x72) + p[2];
}

static int __stdcall my_QueryPiece(char* u, u32 idx)
{
    int piece = 0;
    if ((idx & 0xFF) < 3) return o_QueryPiece(u, idx);
    HIT(H_NAMES);
    piece = slot_piece(u, (int)(idx & 0xFF), 1);
    return piece < 0 ? 0 : piece;
}

/* -- index helpers --------------------------------------------------------- */

static void __stdcall my_SetTarget(char* u, char* target, int idx)          /* 0x48A060 */
{
    WSlot* s;
    if (idx < 3) { o_SetTarget(u, target, idx); return; }
    HIT(H_HELPERS);
    s = slot_ptr(u, idx);
    s->target = *(u16*)(target + 0xA8);
    s->spot   = 0x8000;
    *(u16*)(u + 0xBA) &= 0x83FF;
}

static void __stdcall my_SetGroundTarget(char* u, int* pos, int idx)        /* 0x48A0A0 */
{
    WSlot* s;
    i16 z;
    if (idx < 3) { o_SetGround(u, pos, idx); return; }
    HIT(H_HELPERS);
    s = slot_ptr(u, idx);
    s->target = (u16)(i16)(pos[0] >> 16);
    z = (i16)(pos[2] >> 16);
    s->spot = (z == -0x8000) ? 0x8001 : (u16)z;
    *(u16*)(u + 0xBA) &= 0x83FF;
}

static void clear_target_body(char* u, int idx)
{
    WSlot* s = slot_ptr(u, idx);
    if (s->target != 0 || s->spot != 0x8000)
    {
        s->target = 0;
        s->spot   = 0x8000;
        E_Name2Index(UCOB(u), S_STARTBUILDING);
        E_StartScript(UCOB(u), S_TARGETCLEARED, 0, 0, 1, idx, 0, 0, 0);
    }
}

static void __stdcall my_ClearTarget(char* u, int idx)                      /* 0x48A0F0 */
{
    if (idx < 3) { o_ClearTarget(u, idx); return; }
    HIT(H_HELPERS);
    clear_target_body(u, idx);
}

static void __stdcall my_ClearTargetQuiet(char* u, int idx)                 /* 0x48A160 */
{
    WSlot* s;
    if (idx < 3) { o_ClearTargetQuiet(u, idx); return; }
    HIT(H_HELPERS);
    s = slot_ptr(u, idx);
    s->target = 0;
    s->spot   = 0x8000;
}

static int __stdcall my_WeaponRange(char* u, u32 idx)                       /* 0x49ADF0 */
{
    if ((idx & 0xFF) < 3) return o_Range(u, idx);
    HIT(H_HELPERS);
    return *(int*)(slot_ptr(u, (int)(idx & 0xFF))->weapon + 0xDC);
}

static char* __stdcall my_GetTargetUnit(char* u, int idx)                   /* 0x48A190 */
{
    WSlot* s;
    i16 id;
    if (idx < 3) return o_TargetUnit(u, idx);
    HIT(H_HELPERS);
    s = slot_ptr(u, idx);
    if (s->spot != 0x8000) return 0;
    id = (i16)s->target;
    if (id == 0) return 0;
    return *(char**)(TA() + OFF_UNITS_BEGIN) + (int)id * UNIT_STRIDE;
}

static u8 __fastcall my_FirstWeapon(char* u, int unused)                    /* 0x4897E0 */
{
    int i, count = wpn_count(u);
    (void)unused;
    if (count <= 3) return o_FirstWeapon(u, 0);
    HIT(H_HELPERS);
    if (*(u8*)(u + 0x1F) & 2) return 0;
    if (*(u8*)(u + 0x3B) & 2) return 1;
    if (*(u8*)(u + 0x57) & 2) return 2;
    for (i = 3; i < count; i++)
        if (slot_ptr(u, i)->state & 2) return (u8)i;
    return 0;
}

/* 0x4898B0: clear target and may-acquire for slot n; n == 3 means every slot. */
static void clear_targetn_body(char* u, int n)
{
    WSlot* s = slot_ptr(u, n);
    u8 st = s->state;
    if ((st & 2) && (st & 0x10))
    {
        s->state = st & 0xEF;
        clear_target_body(u, n);
    }
}

static void __thiscall my_ClearTargetN(char* u, int n)
{
    int i, count = wpn_count(u);
    n &= 0xFF;
    if (count <= 3) { o_ClearTargetN(u, n); return; }
    HIT(H_HELPERS);
    if (n == 3)
    {
        o_ClearTargetN(u, 3);                          /* stock: slots 0, 1, 2 */
        for (i = 3; i < count; i++) clear_targetn_body(u, i);
        return;
    }
    if (n < 3) { o_ClearTargetN(u, n); return; }
    clear_targetn_body(u, n);
}

/* 0x489800: set may-acquire for slot n (3 = every slot), clearing its target. */
static void enable_slotn_body(char* u, int n)
{
    WSlot* s = slot_ptr(u, n);
    u8 st = s->state;
    if ((st & 2) && !(st & 0x10))
    {
        s->state = st | 0x10;
        clear_target_body(u, n);
    }
}

static void __thiscall my_EnableSlotN(char* u, int n)
{
    int i, count = wpn_count(u);
    n &= 0xFF;
    if (count <= 3) { o_EnableSlotN(u, n); return; }
    HIT(H_HELPERS);
    if (n == 3)
    {
        o_EnableSlotN(u, 3);
        for (i = 3; i < count; i++) enable_slotn_body(u, i);
        return;
    }
    if (n < 3) { o_EnableSlotN(u, n); return; }
    enable_slotn_body(u, n);
}

/* the engine's 64-bit squared-distance idiom: hi32(dx*dx) + hi32(dz*dz) */
static int dist2_hi(int dx, int dz)
{
    return (int)(((long long)dx * dx) >> 32) + (int)(((long long)dz * dz) >> 32);
}

/* 0x49ABB0 UnitAutoAim_CheckUnitWeapon(unit, target, idx): can weapon idx engage target */
static int __stdcall my_CheckUnitWeapon(char* u, char* t, u32 idx)
{
    char* w;
    u32   wf;
    int   range, sea;
    if ((idx & 0xFF) < 3) return o_Check(u, t, idx);
    HIT(H_HELPERS);
    w   = slot_ptr(u, (int)(idx & 0xFF))->weapon;
    wf  = *(u32*)(w + 0x111);
    sea = *(u8*)(TA() + OFF_SEALEVEL);
    range = *(int*)(w + 0xDC);
    if (!(wf & 0x10000))
    {
        int h = (int)*(i16*)(u + 0x70) + (int)*(i16*)(UDEF(u) + 0x170);
        if (h <= sea) return 0;
        h = (int)*(i16*)(t + 0x70) + (int)*(i16*)(UDEF(t) + 0x170);
        if (h <= sea) return 0;
        if ((wf & 0x20000) && (*(u32*)(t + 0x110) & 3) != 2) return 0;
        if (wf & 2)
        {
            int r = E_Pitch(*(int*)(u + 0x6A) - *(int*)(t + 0x6A),
                            *(int*)(u + 0x6E) - *(int*)(t + 0x6E),
                            *(int*)(u + 0x72) - *(int*)(t + 0x72),
                            *(int*)(w + 0x68), *(float*)(w + 0xC8));
            if ((i16)r == -0x8000) return 0;
        }
        return dist2_hi(*(int*)(t + 0x6A) - *(int*)(u + 0x6A),
                        *(int*)(t + 0x72) - *(int*)(u + 0x72)) <= range * range;
    }
    else
    {
        u32 df = *(u32*)(UDEF(t) + 0x241);
        if (!(df & 0x80000) && (i16)sea < *(i16*)(t + 0x70)) return 0;
        if ((df & 0x1000) &&
            sea < (int)*(i16*)(t + 0x70) + ((int)*(i16*)(UDEF(t) + 0x170) >> 1)) return 0;
        return dist2_hi(*(int*)(t + 0x6A) - *(int*)(u + 0x6A),
                        *(int*)(t + 0x72) - *(int*)(u + 0x72)) <= range * range;
    }
}

/* 0x49AA80 Trajectory3(unit, unitpos, targetpos, idx): in range and reachable */
static int __stdcall my_Trajectory3(char* u, int* upos, int* tpos, u32 idx)
{
    char* w;
    u32   wf;
    int   range;
    if ((idx & 0xFF) < 3) return o_Traj(u, upos, tpos, idx);
    HIT(H_HELPERS);
    w = slot_ptr(u, (int)(idx & 0xFF))->weapon;
    range = *(int*)(w + 0xDC);
    if (dist2_hi(tpos[0] - upos[0], tpos[2] - upos[2]) > range * range) return 0;
    wf = *(u32*)(w + 0x111);
    if (!(wf & 0x10000))
    {
        int h = (int)*(i16*)((char*)upos + 6) + (int)*(i16*)(UDEF(u) + 0x170);
        if (!(h > (int)*(u8*)(TA() + OFF_SEALEVEL))) return 0;
        if (wf & 2)
        {
            int r = E_Pitch(upos[0] - tpos[0], upos[1] - tpos[1], upos[2] - tpos[2],
                            *(int*)(w + 0x68), *(float*)(w + 0xC8));
            if ((i16)r == -0x8000) return 0;
        }
    }
    return 1;
}

/* 0x49D120: an enemy projectile weapon idx (an interceptor with stock) can shoot */
static char* __stdcall my_FindIntercept(char* u, u32 idx)
{
    WSlot* s;
    char*  ta;
    char*  base;
    int    n, k, range;
    u32    r16, r17;
    if ((idx & 0xFF) < 3) return o_Intercept(u, idx);
    HIT(H_HELPERS);
    s = slot_ptr(u, (int)(idx & 0xFF));
    if (s->stock == 0) return 0;
    ta    = TA();
    range = *(int*)(s->weapon + 0xE0);
    n     = *(int*)(ta + OFF_PROJ_COUNT);
    base  = *(char**)(ta + OFF_PROJ_BASE);
    r16   = (u32)range * 0x10000u;
    r17   = (u32)range * 0x20000u;
    for (k = 0; k < n; k++)
    {
        char* p = base + k * PROJ_STRIDE;
        if (*(char*)(p + 0x66) != *(char*)(u + 0xFF)
            && ((*(u32*)(*(char**)p + 0x111) >> 29) & 1)
            && (u32)((*(int*)(u + 0x6A) - *(int*)(p + 0x28)) + r16) <= r17
            && (u32)((*(int*)(u + 0x72) - *(int*)(p + 0x30)) + r16) <= r17)
        {
            int j;
            for (j = 0; j < n; j++)
                if (*(char**)(base + j * PROJ_STRIDE + 0x56) == p) break;
            if (j == n) return p;
        }
    }
    return 0;
}

/* -- UNITS_StartWeaponsScripts (0x49E070) ---------------------------------- */

static void __stdcall my_StartWeapons(char* u)
{
    int i, count = wpn_count(u);
    u32 maxreload = 0;
    if (count <= 3) { o_StartWeapons(u); return; }
    HIT(H_START);
    side_reset(u);
    for (i = 0; i < count; i++)
    {
        WSlot* s  = slot_ptr(u, i);
        u8     st = s->state, idx2 = (u8)((i & 3) << 2), has;
        char*  w  = def_weapon(UDEF(u), i);
        int    q[3], a[3];
        u16    r;
        s->reload = 0;
        s->state  = (u8)(idx2 | (st & 0xF2));
        s->weapon = w;
        has       = (*(char*)(w + 0x10A) != 0);
        s->stock  = 0;
        s->state  = (u8)(((has | 8) << 1) | idx2 | (st & 0xF0));
        my_QueryWeaponPosition(u, q, (u32)i, -1);
        my_CallAimScripts(u, a, (u32)i);
        s->zangle = (int)((double)(q[2] - a[2]) * K_ZANGLE);
        r = *(u16*)(w + 0xE4);
        if (r > maxreload) maxreload = r;
    }
    E_StartScript(UCOB(u), S_SETMAXRELOAD, 0, 0, 1, (int)(maxreload * 1000) / 30, 0, 0, 0);
}

/* Has the turret actually swung round yet?

   The muzzle piece rotates with the gun, so `stand -> muzzle` is the barrel's
   own direction and can be compared with `stand -> target`. Cavedog's aim
   scripts answer this question by construction — they `wait-for-turn` before
   they return, so the slot cannot report aimed until the barrel has arrived —
   but that costs a COB thread for the whole slew, and a unit only gets eight
   (0x4B08C0). Four extra turrets cannot afford one each. So the extended slots
   run an aim script that returns inside its own tick and the arrival test moves
   here, where it costs nothing: same guarantee, no thread held.
   Yaw only; a naval target's pitch is a fraction of a degree. */
#define AIM_TOLERANCE 1024                 /* 5.6 degrees of 65536 */

static int barrel_on_target(char* u, int i, const int* tpos)
{
    int from[3], muz[3], bx, bz, tx, tz, d;
    my_CallAimScripts(u, from, (u32)i);            /* the mount, which does not turn */
    my_QueryWeaponPosition(u, muz, (u32)i, -1);    /* the muzzle, which does          */
    bx = muz[0] - from[0]; bz = muz[2] - from[2];
    tx = tpos[0] - from[0]; tz = tpos[2] - from[2];
    if ((bx == 0 && bz == 0) || (tx == 0 && tz == 0)) return 1;   /* degenerate */
    d = (E_Atan2(bx, bz) - E_Atan2(tx, tz)) & 0xFFFF;
    if (d > 0x8000) d -= 0x10000;
    if (d < 0) d = -d;
    return d <= AIM_TOLERANCE;
}

/* -- AutoAim (0x49E1A0): the per-tick aim-and-fire loop --------------------- */

static void __stdcall my_AutoAim(char* u)
{
    int i, count = wpn_count(u);
    if (count <= 3) { o_AutoAim(u); return; }
    HIT(H_AUTOAIM);
    for (i = 0; i < count; i++)
    {
        WSlot*    s = slot_ptr(u, i);
        char*     w = s->weapon;
        PFN_FireCB cb;
        u32       wf;
        int       tpos[3], heading = 0, pitch = 0, fire;
        char*     tgt;

        if (!(s->state & 2)) continue;
        if (s->reload != 0) s->reload--;
        if (!E_TargetPos(u, tpos, i)) { s->state &= 0xFE; continue; }
        cb = *(PFN_FireCB*)(w + 0x60);
        if (!cb) continue;
        wf = *(u32*)(w + 0x111);

        if (wf & (1u << 19))
        {
            /* Stock starts the aim script once and holds bit 0 until the target
               is lost, because Cavedog's script is long-lived — it slews, waits
               for the turn, and only then returns. The extended slots run one
               that returns inside the tick, so there is nothing to protect and
               holding bit 0 would freeze the turret on its first solution while
               the target sails away. They re-solve every tick and re-issue the
               turn only when the answer actually moves. */
            if (!(s->state & 1) || i >= 3)
            {
                int ok = 0;
                if (wf & 2)
                {
                    int afrom[3], d[3];
                    my_CallAimScripts(u, afrom, (u32)i);
                    d[0] = afrom[0] - tpos[0]; d[1] = afrom[1] - tpos[1]; d[2] = afrom[2] - tpos[2];
                    heading = (E_Atan2(d[0], d[2]) - (int)*(i16*)(u + 0x66)) & 0xFFFF;
                    pitch   = E_Pitch(d[0], d[1], d[2], *(int*)(w + 0x68), *(float*)(w + 0xC8));
                    ok      = ((i16)pitch != -0x8000);
                }
                else if (wf & 1)
                {
                    i16 h16 = 0, p16 = 0;
                    ok      = E_AimCalc(u, w, &h16, &p16, (u32)i, tpos);
                    heading = (u16)h16;
                    pitch   = (u16)p16;
                }
                if (ok)
                {
                    if (!(s->state & 1)
                        || (i16)heading != s->heading || (i16)pitch != s->pitch)
                    {
                        s->pitch   = (i16)pitch;
                        s->heading = (i16)heading;
                        s->aimed   = 0;
                        E_StartScript(UCOB(u), aim_name(i), &s->thread, 0, 2, heading & 0xFFFF, pitch & 0xFFFF, 0, 0);
                        E_SendStart(u, aim_name(i), 2, heading & 0xFFFF, pitch & 0xFFFF, 0, 0);
                    }
                    s->state |= 1;
                }
            }
        }
        else if (wf & (1u << 4))
        {
            if (!((wf & 0x10000000u) && s->stock == 0) && !(s->state & 1))
            {
                s->aimed = 0;
                E_StartScript(UCOB(u), aim_name(i), &s->thread, 0, 2, 0, 0, 0, 0);
                E_SendStart(u, aim_name(i), 2, 0, 0, 0, 0);
                s->state |= 1;
            }
        }

        if (s->reload != 0) continue;
        if (!my_Trajectory3(u, (int*)(u + 0x6A), tpos, (u32)i)) { *(u8*)(u + 0xBB) |= 0x10; continue; }
        if (i >= 3 && !barrel_on_target(u, i, tpos)) { HIT(H_HOLDFIRE); continue; }
        if (wf & (1u << 28))
            fire = (s->stock != 0);
        else
        {
            char* res = *(char**)(u + 0xEC);
            fire = (*(float*)(res + 0x8C) >= *(float*)(w + 0xC0)) &&
                   (*(float*)(res + 0x98) >= *(float*)(w + 0xC4));
        }
        if (!fire) continue;
        tgt = my_GetTargetUnit(u, i);
        if (!cb(u, s, tgt, tpos)) continue;
        if (wf & (1u << 28))
        {
            s->stock--;
            E_Stockpiled(u);
        }
        else
        {
            int m = (int)*(u16*)(u + 0xB8) / 5;
            u32 hp20 = (u32)((int)*(i16*)(u + 0x108) * 20);
            int a, b;
            if (m > 5) m = 5;
            a = 0x78 - (int)(hp20 / *(u32*)(UDEF(u) + 0x1FA));
            b = ((100 - 6 * m) * (int)*(u16*)(w + 0xE4)) / 100;
            s->reload = (u16)((a * b) / 100);
        }
        *(u16*)(u + 0xBA) |= (wf & (1u << 26)) ? 0x800 : 0x400;
        if (!(wf & 0x10000000u))
            E_ChargeRes(u + 0xBC, *(int*)(w + 0xC0), *(int*)(w + 0xC4));
    }
}

/* -- retaliation on damage (0x406F80) --------------------------------------- */

static void __stdcall my_Retaliate(char* attacker, char* victim, int unused)
{
    int   count = wpn_count(victim);
    char* def;
    char* player;
    u32   fl;
    if (count <= 3) { o_Retaliate(attacker, victim, unused); return; }
    HIT(H_RETALIATE);
    E_Notify(victim, 0x10);
    if (attacker && *(u16*)(attacker + 0xA6) == 0) attacker = 0;
    def    = UDEF(victim);
    player = UPLAYER(victim);
    if ((*(u32*)(def + 0x245) & 0x1000) && *(int*)player != 0 && *(char*)(player + 0x73) == 2)
    {
        int r = E_Rand(300);
        *(int*)(*(char**)(player + 0x74) + 0xD) = r + 0x1E + *(int*)(TA() + OFF_TICK);
        E_ClearBuild(victim, 0);
    }
    if (attacker && *(int*)player != 0
        && (*(char*)(player + 0x73) == 1 || *(char*)(player + 0x73) == 2)
        && (*(u32*)(def + 0x241) & 0x10010000u)
        && *(float*)(victim + 0x104) == K_BUILT
        && *(char*)(player + 0x108 + *(u8*)(UPLAYER(attacker) + 0x146)) == 0)
    {
        char* order   = *(char**)(victim + 0x5C);
        int   created = 0;
        u16   atype   = *(u16*)(attacker + 0xA6);
        if ((order == 0 || (*(u32*)(order + 0x42) & 0x20000))
            && !mask_has(*(u32**)(def + 0x23D), atype)
            && !mask_has(*(u32**)(def + 0x231), atype))
        {
            if (my_CheckUnitWeapon(victim, attacker, 0))
                created = E_AttackOrder(victim, attacker, 0);
        }
        if (!created && (*(u32*)(victim + 0x110) & 0x300000))
        {
            int i;
            for (i = 0; i < count; i++)
            {
                WSlot* s = slot_ptr(victim, i);
                char*  cur;
                if (!((s->state & 2) && (s->state & 0x10))) continue;
                if (!my_CheckUnitWeapon(victim, attacker, (u32)i)) continue;
                if (*(u32*)(s->weapon + 0x111) & (1u << 26)) continue;
                cur = my_GetTargetUnit(victim, i);
                if (cur && my_CheckUnitWeapon(victim, cur, (u32)i)
                    && !mask_has(def_mask(def, i), *(u16*)(cur + 0xA6))) continue;
                my_SetTarget(victim, attacker, i);
            }
        }
    }
    fl = E_UnitFlags(victim);
    if (!(fl & 0x80) && (*(char*)(victim + 0xF4) != *(char*)(victim + 0xFF) || *(char*)(victim + 0xF5) == 1))
        E_Reaction(victim, 2, 0);
}

/* -- periodic target acquisition (0x4089A0) ----------------------------------
   __thiscall on a per-player controller. One call walks limit/30+1 units from
   a cursor, so the stock-or-not decision is per batch: if no unit in the batch
   is extended the original runs, otherwise this port runs for the whole batch
   (call-for-call the same sequence for the stock units in it). */

static char* acquire_next(char* self, char* cur)
{
    char* ctx = *(char**)self;
    if (cur == 0 || cur == *(char**)(ctx + 0x6B)) return *(char**)(ctx + 0x67);
    return cur + UNIT_STRIDE;
}

static int batch_has_extended(char* self)
{
    int   n = (int)*(u16*)(TA() + OFF_LIMIT30) / 30, k;
    char* cur = *(char**)(self + 0x39);
    for (k = 0; k <= n; k++)
    {
        cur = acquire_next(self, cur);
        if (*(u16*)(cur + 0xA6) != 0 && (size_t)UDEF(cur) > 0x600000u && wpn_count(cur) > 3)
            return 1;
    }
    return 0;
}

static int __thiscall my_Acquire(char* self, int param)
{
    int   n, k;
    char* ctx;
    if (!batch_has_extended(self)) return o_Acquire(self, param);
    HIT(H_ACQUIRE);
    ctx = *(char**)self;
    n   = (int)*(u16*)(TA() + OFF_LIMIT30) / 30;
    for (k = 0; k <= n; k++)
    {
        char* cur = acquire_next(self, *(char**)(self + 0x39));
        int   count, i;
        *(char**)(self + 0x39) = cur;
        if (*(u16*)(cur + 0xA6) == 0) continue;
        if (*(float*)(cur + 0x104) != K_BUILT) continue;
        if (!(*(u32*)(cur + 0x110) & 0x80000000u)) continue;
        if ((*(u32*)(cur + 0x110) & 0x300000) != 0x200000) continue;
        count = wpn_count(cur);
        for (i = 0; i < count; i++)
        {
            WSlot* s  = slot_ptr(cur, i);
            u8     st = s->state;
            u32    wf;
            char*  tgt;
            if (!(st & 2) || !(st & 0x10)) continue;
            wf = *(u32*)(s->weapon + 0x111);
            if (wf & 0x100) continue;
            if (param == 0 && (wf & 0x4000000)) continue;
            tgt = my_GetTargetUnit(cur, i);
            if (tgt)
            {
                if (*(char*)(ctx + 0x108 + *(u8*)(UPLAYER(tgt) + 0x146)) == 0
                    && !mask_has(def_mask(UDEF(cur), i), *(u16*)(tgt + 0xA6))
                    && !((wf & 0x80) && (*(u8*)(tgt + 0x10E) & 0x10)))
                    continue;                                 /* keep the current target */
            }
            if (wf & (1u << 30))
            {
                char* p = my_FindIntercept(cur, (u32)i);
                if (p) my_SetGroundTarget(cur, (int*)(p + 4), i);
                else   my_ClearTarget(cur, i);
            }
            else if ((*(u32*)(cur + 0x110) & 0x300000) == 0x200000)
            {
                char* t = E_FindTarget(cur, (u32)i, 1);
                if (t) my_SetTarget(cur, t, i);
                else   my_ClearTarget(cur, i);
            }
        }
    }
    return 0;
}

/* -- def copy (0x42B370): keep the side record with the type it describes ---- */

static void __thiscall my_DefCopy(void* dst, int src)
{
    char* base;
    o_DefCopy(dst, src);
    if (!g_def || (size_t)TA() < 0x600000u) return;
    base = *(char**)(TA() + OFF_DEFS);
    if (base && (char*)dst >= base && (char*)src >= base)
    {
        u32 di = (u32)((char*)dst - base) / DEF_STRIDE;
        u32 si = (u32)((char*)src - base) / DEF_STRIDE;
        if (di < WPN_MAXDEFS && si < WPN_MAXDEFS)
        {
            if (g_def[si].def == (char*)src)
            {
                g_def[di]     = g_def[si];
                g_def[di].def = (char*)dst;
            }
            else
                g_def[di].def = 0;
        }
    }
}

/* =========================================================================
   8. Splice callbacks (cdecl, called from the generated stubs)
   ========================================================================= */

static int derived_index(char* unit, WSlot* slot)
{
    int i = slot_index(unit, slot);
    if (i >= 3) HIT(H_SPLICE);
    else
    {
        HIT(H_STOCKSPLICE);
        if (((slot->state >> 2) & 3) != i)
        {
            HIT(H_MISMATCH);
            if (g_hits[H_MISMATCH] <= 20)
                wlog("MISMATCH derived slot index %d vs state byte 0x%02X (unit %p %.12s)",
                     i, slot->state, unit, UDEF(unit) + 0x20);
        }
    }
    return i;
}

static int __cdecl cb_slot_index(char* unit, WSlot* slot)
{
    return derived_index(unit, slot);
}

static const char* __cdecl cb_fire_name(char* unit, WSlot* slot)
{
    int i = derived_index(unit, slot);
    g_fires[i]++;
    return fire_name(i);
}

static WSlot* __cdecl cb_slot_ptr(char* unit, u32 idx)
{
    HIT(idx >= 3 ? H_SPLICE : H_STOCKSPLICE);
    return slot_ptr(unit, (int)idx);
}

static char* __cdecl cb_slot_weapon(char* unit, u32 idx)
{
    HIT(idx >= 3 ? H_SPLICE : H_STOCKSPLICE);
    return slot_ptr(unit, (int)idx)->weapon;
}

static u32* __cdecl cb_mask(char* def, u32 idx)
{
    HIT(idx >= 3 ? H_SPLICE : H_STOCKSPLICE);
    return def_mask(def, (int)idx);
}

/* Attack-ground order (FUN_004038a0, the "target is a spot" branch at 0x40399C):
   the engine unrolls exactly two slots — ClearTargetN 0, ClearTargetN 1,
   SetGroundTarget 0, SetGroundTarget 1 — so weapon 3 and every side weapon are
   left with no target at all and sit idle while 1 and 2 shell the spot. (The
   sibling branch at 0x40396B is the single-weapon case the order takes when its
   held index is 2; nothing sets it for an extended unit, so it stays stock.)
   Spliced over the second pair (`push 1; push ebx; push edi; call SetGround`):
   do that call, then give the side slots the same spot on the same terms —
   clear may-acquire first, exactly as ClearTargetN did for 0 and 1. */
static void __cdecl cb_ground_order(char* unit, int* pos)
{
    int i, count = wpn_count(unit);
    my_SetGroundTarget(unit, pos, 1);
    if (count <= 3) return;
    HIT(H_GROUND);
    for (i = 3; i < count; i++)
    {
        clear_targetn_body(unit, i);
        my_SetGroundTarget(unit, pos, i);
    }
}

/* WEAPON_FIRED receiver (0x49D270): the packet's WeapIdx byte at +0x23 names
   the slot. Beyond the unit's count it would have indexed past the inline
   slots into UnitOrders; here it clamps to slot 0 and logs. */
static WSlot* __cdecl cb_recv_slot(char* unit, u8* pkt)
{
    u32 idx = pkt[0x23];
    if (idx < 3) HIT(H_STOCKSPLICE);
    else
    {
        HIT(H_SPLICE);
        if (idx >= (u32)wpn_count(unit))
        {
            HIT(H_VIOLATION);
            if (g_hits[H_VIOLATION] <= 20)
                wlog("VIOLATION WEAPON_FIRED WeapIdx %u for %.12s (count %d) — clamped to 0",
                     idx, UDEF(unit) + 0x20, wpn_count(unit));
            return (WSlot*)(unit + 4);
        }
    }
    return slot_ptr(unit, (int)idx);
}

/* FBI loader (0x42BF40) after it stored weapon1..3: read weapon4..N and their
   bad-target masks into the def record. `ctx` is the TDF file object the
   loader itself passes to TdfFile_GetStr. */
static void __cdecl cb_loader(char* def, void* ctx)
{
    char* base = *(char**)(TA() + OFF_DEFS);
    u32   idx;
    WDef* r;
    int   n;
    char  key[32], buf[132];
    if (!g_def || !base || def < base) return;
    idx = (u32)(def - base) / DEF_STRIDE;
    if (idx >= WPN_MAXDEFS) { wlog("loader: def index %u beyond WPN_MAXDEFS", idx); return; }
    HIT(H_LOADER);
    r = &g_def[idx];
    memset(r, 0, sizeof *r);
    memset(r->pc_aimfrom, -1, sizeof r->pc_aimfrom);
    memset(r->pc_query,   -1, sizeof r->pc_query);
    r->def   = def;
    r->count = 3;
    for (n = 4; n <= WPN_CAP; n++)
    {
        char* w = TA() + OFF_WEAPON0;
        _snprintf(key, sizeof key, "weapon%d", n);
        buf[0] = 0;
        E_TdfGetStr(ctx, buf, key, 0x80, "");
        if (buf[0])
        {
            char* found = E_Name2Ptr(buf);
            if (found) w = found;
            else wlog("loader: %.12s %s=%s names no weapon", def + 0x20, key, buf);
            r->count = (u8)n;
        }
        r->w[n - 4] = w;
        _snprintf(key, sizeof key, "w%d_badTargetCategory", n);
        buf[0] = 0;
        E_TdfGetStr(ctx, buf, key, 100, "none");
        r->mask[n - 4] = E_Categories(buf);
    }
    if (r->count > 3)
        wlog("loader: %.12s (def #%u) has %d weapons", def + 0x20, idx, r->count);
}

/* The CRC of one weapon's TDF section, exactly as the engine computes it for
   weapon1..3 at 0x42AE29 / 0x42AEA0 / 0x42AF17: rewind each loaded weapon-TDF
   context in turn and take the stored CRC of the first section that matches.
   Zero when no file defines it — which is also what the engine folds in then,
   so an unresolvable name still agrees across peers. */
static u32 weapon_tdf_crc(const char* name)
{
    char* ctx = WTDF_BASE;
    int   i, n = WTDF_COUNT;
    if (!name || !*name || (size_t)ctx < 0x600000u) return 0;
    for (i = 0; i < n; i++, ctx += 0xC)
    {
        E_TdfIterA(ctx);
        if (E_TdfSection(ctx, name))
            return *(u32*)(*(char**)(ctx + 4) + 0x25);
    }
    return 0;
}

/* Unit-info loader (0x42A8D0) at 0x42B004: the engine has just folded weapon1..3,
   explodeas and selfdestructas into CRC_weapons and is about to read it back for
   its last XOR. Fold weapon4..N in the same way and in the same place.

   This is the whole mismatched-peer guard. Without it an armed build and a stock
   build compute an identical CRC for a type they simulate differently, and the
   lobby has nothing to disagree about — while an unarmed receiver handling a
   0x0D WEAPON_FIRED packet with WeapIdx >= 3 indexes past its three inline slots
   into UnitOrders. With it, two armed peers still agree (same names, same TDFs)
   and an unarmed peer differs on exactly the types that carry extra weapons.

   `ctx` is the unit's own UNITINFO TDF context, the one the engine passes to
   0x4C4630 for weapon1..3 four instructions earlier. */
static void __cdecl cb_crc_weapons(char* def, void* ctx)
{
    char key[16];
    int  n, folded = 0;
    if (!g_armed || (size_t)def < 0x600000u || !ctx) return;
    for (n = 4; n <= WPN_CAP; n++)
    {
        char* val;
        u32   c;
        _snprintf(key, sizeof key, "weapon%d", n);
        val = E_TdfValue(ctx, key);
        if (!val || !*val) continue;
        /* Mix the slot number in, unlike the stock folds. XOR is self-inverse and
           the extra slots are variable in number, so folding the bare weapon CRC
           would let two slots holding the SAME weapon cancel each other out — a
           unit with weapon4=weapon5=X would compute the stock CRC and match an
           unarmed peer exactly where it must not. The same goes for a name no TDF
           defines: its CRC is 0, but the armed side still gives it a slot, so the
           term has to be nonzero anyway. Rotate-and-offset by n makes every slot
           contribute distinctly while two armed peers with the same FBI still
           agree bit for bit. */
        c = weapon_tdf_crc(val) ^ ((u32)n * 0x9E3779B9u);
        c = (c << n) | (c >> (32 - n));            /* n is 4..16: never 0 or 32 */
        *(u32*)(def + DEF_CRC_WPN) ^= c;
        folded++;
    }
    if (folded)
    {
        HIT(H_CRC);
        /* the value here is still missing the engine's own selfdestructas fold,
           which the stolen instruction is about to read back and apply */
        wlog("crc: %.12s folded %d extra weapon(s), CRC_weapons=0x%08X (pre-final)",
             def + 0x20, folded, *(u32*)(def + DEF_CRC_WPN));
    }
}

/* =========================================================================
   9. x86 emitter, site tables, all-or-nothing install
   ========================================================================= */

typedef struct Emit { u8* p; } Emit;
static void e8(Emit* e, u8 b)                    { *e->p++ = b; }
static void e32(Emit* e, u32 v)                  { memcpy(e->p, &v, 4); e->p += 4; }
static void ebytes(Emit* e, const u8* b, int n)  { memcpy(e->p, b, n); e->p += n; }
static void ecall(Emit* e, const void* f)        { e8(e, 0xE8); e32(e, (u32)f - ((u32)e->p + 4)); }
static void ejmp(Emit* e, u32 target)            { e8(e, 0xE9); e32(e, target - ((u32)e->p + 4)); }
static void epush_esp(Emit* e, int disp)         /* push dword [esp+disp] */
{
    if (disp < 0x80) { e8(e, 0xFF); e8(e, 0x74); e8(e, 0x24); e8(e, (u8)disp); }
    else             { e8(e, 0xFF); e8(e, 0xB4); e8(e, 0x24); e32(e, (u32)disp); }
}
static void emov_esp_eax(Emit* e, int disp)      /* mov [esp+disp], eax (disp < 0x80) */
{ e8(e, 0x89); e8(e, 0x44); e8(e, 0x24); e8(e, (u8)disp); }

/* frame after `pushad; pushfd`: eflags at 0, then edi esi ebp esp ebx edx ecx eax */
#define R_EAX 0
#define R_ECX 1
#define R_EDX 2
#define R_EBX 3
#define R_EBP 5
#define R_ESI 6
#define R_EDI 7
#define FRAME_REG(r)   (0x20 - 4 * (r))
#define FRAME_STACK(x) (0x24 + (x))            /* original [esp+x] */

typedef struct Arg { u8 kind; int val; } Arg;    /* kind 0: register, 1: original stack slot */
#define AREG(r)   { 0, (r) }
#define ASTACK(x) { 1, (x) }

typedef struct Splice {
    const char* name;
    u32         addr;
    u8          len;                 /* stolen bytes at addr, replaced by jmp + nops */
    const u8*   expect;
    void*       cfunc;
    int         nargs;
    Arg         args[2];
    int         dest;                /* register that receives eax, or -1 */
    const u8*   extra;               /* instructions re-executed after popad */
    u8          extra_len;
    u32         back;                /* resume address (0: addr + len) */
} Splice;

typedef struct Hook {                /* entry replacement with a trampoline */
    const char* name;
    u32         addr;
    u8          len;
    const u8*   expect;
    void*       repl;
    void**      tramp;
} Hook;

typedef struct Patch {               /* in-place byte rewrite */
    const char* name;
    u32         addr;
    u8          len;
    const u8*   expect;
    const u8*   repl;
} Patch;

/* ---- stolen / expected bytes (objdump of the pristine image) -------------- */
static const u8 X_STARTW[]   = { 0x83,0xEC,0x20,0x53,0x55 };
static const u8 X_AUTOAIM[]  = { 0x83,0xEC,0x30,0x53,0x55 };
static const u8 X_CALLAIM[]  = { 0x83,0xEC,0x10,0x56,0x57 };
static const u8 X_QUERYPOS[] = { 0x8B,0x44,0x24,0x10,0x83,0xEC,0x0C };
static const u8 X_QUERYPC[]  = { 0x83,0xEC,0x10,0x8B,0x4C,0x24,0x18 };
static const u8 X_RETAL[]    = { 0x51,0x53,0x55,0x56,0x8B,0x74,0x24,0x18 };
static const u8 X_ACQ[]      = { 0x83,0xEC,0x10,0xA1,0xE8,0x1D,0x51,0x00 };
static const u8 X_FIRSTW[]   = { 0x8A,0x41,0x1F,0xB2,0x02 };
static const u8 X_SLOTN[]    = { 0x8A,0x44,0x24,0x04,0x56,0x57 };     /* 0x4898B0 and 0x489800 */
static const u8 X_SETTGT[]   = { 0x8B,0x44,0x24,0x0C,0x8B,0x54,0x24,0x08 };
static const u8 X_SETGND[]   = { 0x8B,0x44,0x24,0x0C,0x8B,0x54,0x24,0x04 };
static const u8 X_CLRTGT[]   = { 0x56,0x8B,0x74,0x24,0x0C,0x8B,0xC6 };
static const u8 X_IDX8[]     = { 0x8B,0x44,0x24,0x08,0x8B,0x54,0x24,0x04 }; /* 0x48A160, 0x49ADF0, 0x48A190 */
static const u8 X_CHECK[]    = { 0x8B,0x44,0x24,0x0C,0x83,0xEC,0x0C };
static const u8 X_TRAJ[]     = { 0x83,0xEC,0x10,0x8B,0x44,0x24,0x20 };
static const u8 X_INTERCEPT[]= { 0x8B,0x44,0x24,0x08,0x53,0x25,0xFF,0x00,0x00,0x00 };
static const u8 X_DEFCOPY[]  = { 0x8B,0xC1,0x53,0x8B,0x4C,0x24,0x08 };
/* 0x4039BE: push 1; push ebx; push edi; call 0x48A0A0 (SetGroundTarget slot 1) */
static const u8 X_GROUND[]   = { 0x6A,0x01,0x53,0x57,0xE8,0xD9,0x66,0x08,0x00 };

static const u8 X_LOADER[]   = { 0x8B,0x85,0xEE,0x01,0x00,0x00,0x3B,0xC6 };
/* 0x42B004: mov edx,[ebp+0x146] — the read-back after the last stock fold */
static const u8 X_CRC[]      = { 0x8B,0x95,0x46,0x01,0x00,0x00 };
static const u8 X_RECV[]     = { 0x33,0xC9,0x8A,0x48,0x23,0x8B,0xD1,0xC1,0xE2,0x03,0x2B,0xD1,0x8D,0x4C,0x95,0x04 };
static const u8 X_RECV_X[]   = { 0x0F,0xB6,0x50,0x23,0x6B,0xD2,0x07 };  /* movzx edx,[eax+0x23]; imul edx,edx,7 */
static const u8 X_FPN_EDX[]  = { 0x8B,0x14,0x8D,0x78,0x96,0x50,0x00 };  /* mov edx,[ecx*4+0x509678] */
static const u8 X_FPN_ECX[]  = { 0x8B,0x0C,0x85,0x78,0x96,0x50,0x00 };  /* mov ecx,[eax*4+0x509678] */
static const u8 X_H03[]      = { 0x66,0x8B,0x74,0x8D,0x1A };            /* mov si,[ebp+ecx*4+0x1a] */
static const u8 X_H1[]       = { 0x66,0x8B,0x74,0x95,0x1A };            /* mov si,[ebp+edx*4+0x1a] */
static const u8 X_H0[]       = { 0x66,0x8B,0x7C,0x8B,0x1A };            /* mov di,[ebx+ecx*4+0x1a] */
static const u8 P_H_SI_EDI[] = { 0x66,0x8B,0x77,0x16,0x90 };            /* mov si,[edi+0x16]; nop  */
static const u8 P_H_DI_ESI[] = { 0x66,0x8B,0x7E,0x16,0x90 };            /* mov di,[esi+0x16]; nop  */
static const u8 X_SHR_AL[]   = { 0xC0,0xE8,0x02,0x24,0x03 };            /* shr al,2; and al,3      */
static const u8 X_SHR_CL[]   = { 0xC0,0xE9,0x02,0x80,0xE1,0x03 };
static const u8 X_SHR_DL[]   = { 0xC0,0xEA,0x02,0x80,0xE2,0x03 };
static const u8 X_CB0D[]     = { 0xC0,0xE8,0x02,0xC6,0x44,0x24,0x30,0x0D };
static const u8 X_CB0D_X[]   = { 0xC6,0x44,0x24,0x30,0x0D };
static const u8 X_CB0D_AND[] = { 0x24,0x03 };
static const u8 P_NOP2[]     = { 0x90,0x90 };
static const u8 X_CB3F[]     = { 0xC0,0xEA,0x02,0x8A,0x88,0x0A,0x01,0x00,0x00,0x80,0xE2,0x03 };
static const u8 X_CB3F_X[]   = { 0x8A,0x88,0x0A,0x01,0x00,0x00 };
static const u8 X_CB1I[]     = { 0xC0,0xEA,0x02,0x89,0x4C,0x24,0x29,0x8A,0x88,0x0A,0x01,0x00,0x00,0x33,0xFF,0x80,0xE2,0x03 };
static const u8 X_CB1I_X[]   = { 0x89,0x4C,0x24,0x29,0x8A,0x88,0x0A,0x01,0x00,0x00,0x33,0xFF };
static const u8 X_FT_W[]     = { 0x8D,0x0C,0xC5,0x00,0x00,0x00,0x00,0x2B,0xC8,0x8B,0x54,0x8F,0x10 };
static const u8 X_FT_M[]     = { 0x8B,0x94,0x9A,0x31,0x02,0x00,0x00 };
static const u8 X_TPOS[]     = { 0xC1,0xE0,0x03,0x2B,0xC6,0x66,0x39,0x54,0x87,0x06,0x8D,0x5C,0x87,0x04 };
static const u8 X_TPOS_X[]   = { 0x66,0x39,0x53,0x02 };                 /* cmp word [ebx+2],dx     */

static const Hook HOOKS[] = {
    { "UNITS_StartWeaponsScripts", 0x49E070, 5, X_STARTW,   (void*)my_StartWeapons,       (void**)&o_StartWeapons },
    { "AutoAim",                   0x49E1A0, 5, X_AUTOAIM,  (void*)my_AutoAim,            (void**)&o_AutoAim },
    { "UNITS_CallAimScripts",      0x43E2E0, 5, X_CALLAIM,  (void*)my_CallAimScripts,     (void**)&o_CallAim },
    { "UNITS_QueryWeaponPosition", 0x43E240, 7, X_QUERYPOS, (void*)my_QueryWeaponPosition,(void**)&o_QueryPos },
    { "QueryPiece",                0x43E1E0, 7, X_QUERYPC,  (void*)my_QueryPiece,         (void**)&o_QueryPiece },
    { "Retaliate",                 0x406F80, 8, X_RETAL,    (void*)my_Retaliate,          (void**)&o_Retaliate },
    { "Acquire",                   0x4089A0, 8, X_ACQ,      (void*)my_Acquire,            (void**)&o_Acquire },
    { "FirstWeapon",               0x4897E0, 5, X_FIRSTW,   (void*)my_FirstWeapon,        (void**)&o_FirstWeapon },
    { "ClearTargetN",              0x4898B0, 6, X_SLOTN,    (void*)my_ClearTargetN,       (void**)&o_ClearTargetN },
    { "EnableSlotN",               0x489800, 6, X_SLOTN,    (void*)my_EnableSlotN,        (void**)&o_EnableSlotN },
    { "SetTarget",                 0x48A060, 8, X_SETTGT,   (void*)my_SetTarget,          (void**)&o_SetTarget },
    { "SetGroundTarget",           0x48A0A0, 8, X_SETGND,   (void*)my_SetGroundTarget,    (void**)&o_SetGround },
    { "ClearTarget",               0x48A0F0, 7, X_CLRTGT,   (void*)my_ClearTarget,        (void**)&o_ClearTarget },
    { "ClearTargetQuiet",          0x48A160, 8, X_IDX8,     (void*)my_ClearTargetQuiet,   (void**)&o_ClearTargetQuiet },
    { "WeaponRange",               0x49ADF0, 8, X_IDX8,     (void*)my_WeaponRange,        (void**)&o_Range },
    { "GetTargetUnit",             0x48A190, 8, X_IDX8,     (void*)my_GetTargetUnit,      (void**)&o_TargetUnit },
    { "CheckUnitWeapon",           0x49ABB0, 7, X_CHECK,    (void*)my_CheckUnitWeapon,    (void**)&o_Check },
    { "Trajectory3",               0x49AA80, 7, X_TRAJ,     (void*)my_Trajectory3,        (void**)&o_Traj },
    { "FindIntercept",             0x49D120, 10, X_INTERCEPT,(void*)my_FindIntercept,     (void**)&o_Intercept },
    { "DefCopy",                   0x42B370, 7, X_DEFCOPY,  (void*)my_DefCopy,            (void**)&o_DefCopy },
};

static const Splice SPLICES[] = {
    /* loader: after weapon3 is stored; ebp = def, [esp+0x14] = TDF object */
    { "loader",      0x42CEF2, 8,  X_LOADER,  (void*)cb_loader,     2, { AREG(R_EBP), ASTACK(0x14) }, -1,    X_LOADER, 8, 0 },
    /* unit-info CRC: ebp = def, [esp+0x1c] = the unit's UNITINFO TDF context.
       Ours XORs into def+0x146 before the stolen read-back picks it up. */
    { "crc",         0x42B004, 6,  X_CRC,     (void*)cb_crc_weapons, 2, { AREG(R_EBP), ASTACK(0x1C) }, -1,  X_CRC, 6, 0 },
    /* WEAPON_FIRED receiver: ebp = unit, eax = packet -> ecx = slot, edx = idx*7 */
    { "recv",        0x49D364, 16, X_RECV,    (void*)cb_recv_slot,  2, { AREG(R_EBP), AREG(R_EAX) },  R_ECX, X_RECV_X, 7, 0 },
    /* FireProjectile_*: fire-script name by slot (unit, slot) -> reg */
    { "fp03.name",   0x49CB86, 7,  X_FPN_EDX, (void*)cb_fire_name,  2, { AREG(R_EBP), AREG(R_EDI) },  R_EDX, 0, 0, 0 },
    { "fp1.name",    0x49CD41, 7,  X_FPN_ECX, (void*)cb_fire_name,  2, { AREG(R_EBP), AREG(R_EDI) },  R_ECX, 0, 0, 0 },
    { "fp0.name",    0x49CF65, 7,  X_FPN_EDX, (void*)cb_fire_name,  2, { AREG(R_EBX), AREG(R_ESI) },  R_EDX, 0, 0, 0 },
    /* fire callbacks: state>>2&3 -> SlotIndex(unit, slot) */
    { "cb0.a",       0x49D5B9, 5,  X_SHR_AL,  (void*)cb_slot_index, 2, { AREG(R_EDI), AREG(R_ESI) },  R_EAX, 0, 0, 0 },
    { "cb0.b",       0x49D63B, 5,  X_SHR_AL,  (void*)cb_slot_index, 2, { AREG(R_EDI), AREG(R_ESI) },  R_EAX, 0, 0, 0 },
    { "cb0.c",       0x49D6A3, 5,  X_SHR_AL,  (void*)cb_slot_index, 2, { AREG(R_EDI), AREG(R_ESI) },  R_EAX, 0, 0, 0 },
    { "cb0.d",       0x49D7D9, 8,  X_CB0D,    (void*)cb_slot_index, 2, { AREG(R_EDI), AREG(R_ESI) },  R_EAX, X_CB0D_X, 5, 0 },
    { "cb3.e",       0x49D9D4, 5,  X_SHR_AL,  (void*)cb_slot_index, 2, { AREG(R_EDI), AREG(R_ESI) },  R_EAX, 0, 0, 0 },
    { "cb3.f",       0x49DAD5, 12, X_CB3F,    (void*)cb_slot_index, 2, { AREG(R_EDI), AREG(R_ESI) },  R_EDX, X_CB3F_X, 6, 0 },
    { "cb1.g",       0x49DB8D, 5,  X_SHR_AL,  (void*)cb_slot_index, 2, { AREG(R_EBX), AREG(R_ESI) },  R_EAX, 0, 0, 0 },
    { "cb1.h",       0x49DC27, 6,  X_SHR_CL,  (void*)cb_slot_index, 2, { AREG(R_EBX), AREG(R_ESI) },  R_ECX, 0, 0, 0 },
    { "cb1.i",       0x49DCA4, 18, X_CB1I,    (void*)cb_slot_index, 2, { AREG(R_EBX), AREG(R_ESI) },  R_EDX, X_CB1I_X, 12, 0 },
    { "cb2.j",       0x49DD78, 5,  X_SHR_AL,  (void*)cb_slot_index, 2, { AREG(R_EDI), AREG(R_EBX) },  R_EAX, 0, 0, 0 },
    { "cb2.k",       0x49DE73, 6,  X_SHR_DL,  (void*)cb_slot_index, 2, { AREG(R_EDI), AREG(R_EBX) },  R_EDX, 0, 0, 0 },
    /* target finder 0x40B7B0: weapon of slot (edi=unit, eax=idx) -> edx; mask (edx=def, ebx=idx) -> edx */
    { "ft.weapon",   0x40B965, 13, X_FT_W,    (void*)cb_slot_weapon,2, { AREG(R_EDI), AREG(R_EAX) },  R_EDX, 0, 0, 0 },
    { "ft.mask",     0x40B9FD, 7,  X_FT_M,    (void*)cb_mask,       2, { AREG(R_EDX), AREG(R_EBX) },  R_EDX, 0, 0, 0 },
    /* target position 0x48A1E0: slot address (edi=unit, esi=idx) -> ebx, then the stolen cmp */
    { "tpos.slot",   0x48A1F6, 14, X_TPOS,    (void*)cb_slot_ptr,   2, { AREG(R_EDI), AREG(R_ESI) },  R_EBX, X_TPOS_X, 4, 0 },
    /* attack-ground order 0x4038A0: edi = unit, ebx = the order's position */
    { "ground.order",0x4039BE, 9,  X_GROUND,  (void*)cb_ground_order,2,{ AREG(R_EDI), AREG(R_EBX) },  -1,    0, 0, 0 },
};

static const Patch PATCHES[] = {
    { "fp03.heading", 0x49CBAE, 5, X_H03,     P_H_SI_EDI },
    { "fp1.heading",  0x49CD69, 5, X_H1,      P_H_SI_EDI },
    { "fp0.heading",  0x49CF8D, 5, X_H0,      P_H_DI_ESI },
    { "cb0.d.and",    0x49D7EB, 2, X_CB0D_AND, P_NOP2 },
};

#define N_HOOKS   (sizeof HOOKS / sizeof HOOKS[0])
#define N_SPLICES (sizeof SPLICES / sizeof SPLICES[0])
#define N_PATCHES (sizeof PATCHES / sizeof PATCHES[0])

static int verify_all(void)
{
    u32 i;
    int ok = 1;
    for (i = 0; i < N_HOOKS; i++)
        if (memcmp((void*)HOOKS[i].addr, HOOKS[i].expect, HOOKS[i].len) != 0)
        { wlog("disarmed: hook %s @0x%08X bytes differ", HOOKS[i].name, HOOKS[i].addr); ok = 0; }
    for (i = 0; i < N_SPLICES; i++)
        if (memcmp((void*)SPLICES[i].addr, SPLICES[i].expect, SPLICES[i].len) != 0)
        { wlog("disarmed: splice %s @0x%08X bytes differ", SPLICES[i].name, SPLICES[i].addr); ok = 0; }
    for (i = 0; i < N_PATCHES; i++)
        if (memcmp((void*)PATCHES[i].addr, PATCHES[i].expect, PATCHES[i].len) != 0)
        { wlog("disarmed: patch %s @0x%08X bytes differ", PATCHES[i].name, PATCHES[i].addr); ok = 0; }
    return ok;
}

static u8* pool_take(u32 n)
{
    u8* p;
    n = (n + 15) & ~15u;
    if (g_pool_used + n > 0x4000) return 0;
    p = g_pool + g_pool_used;
    g_pool_used += n;
    return p;
}

static void build_trampoline(const Hook* h)
{
    Emit e;
    u8* t = pool_take(h->len + 5);
    e.p = t;
    ebytes(&e, h->expect, h->len);
    ejmp(&e, h->addr + h->len);
    *h->tramp = t;
}

static u8* build_stub(const Splice* s)
{
    Emit e;
    u8*  stub = pool_take(64 + s->extra_len);
    int  i, pushed = 0;
    e.p = stub;
    e8(&e, 0x60);                                   /* pushad */
    e8(&e, 0x9C);                                   /* pushfd */
    for (i = s->nargs - 1; i >= 0; i--)
    {
        const Arg* a = &s->args[i];
        if (a->kind == 0) epush_esp(&e, FRAME_REG(a->val) + 4 * pushed);
        else              epush_esp(&e, FRAME_STACK(a->val) + 4 * pushed);
        pushed++;
    }
    ecall(&e, s->cfunc);
    if (s->nargs) { e8(&e, 0x83); e8(&e, 0xC4); e8(&e, (u8)(4 * s->nargs)); }   /* add esp, n */
    if (s->dest >= 0) emov_esp_eax(&e, FRAME_REG(s->dest));
    e8(&e, 0x9D);                                   /* popfd */
    e8(&e, 0x61);                                   /* popad */
    if (s->extra_len) ebytes(&e, s->extra, s->extra_len);
    ejmp(&e, s->back ? s->back : s->addr + s->len);
    return stub;
}

static int write_jump(u32 addr, u8 len, const u8* target_or_bytes, int is_jump)
{
    DWORD old;
    u8*   p = (u8*)addr;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    if (is_jump)
    {
        u32 rel = (u32)target_or_bytes - (addr + 5);
        p[0] = 0xE9;
        memcpy(p + 1, &rel, 4);
        if (len > 5) memset(p + 5, 0x90, len - 5);
    }
    else
        memcpy(p, target_or_bytes, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return 1;
}

static int install(void)
{
    u32 i;
    u8* stubs[N_SPLICES];

    if (!verify_all()) return 0;

    g_pool = (u8*)VirtualAlloc(NULL, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    g_def  = (WDef*)VirtualAlloc(NULL, sizeof(WDef) * WPN_MAXDEFS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_pool || !g_def) { wlog("disarmed: VirtualAlloc failed"); return 0; }

    for (i = 0; i < N_HOOKS; i++) build_trampoline(&HOOKS[i]);
    for (i = 0; i < N_SPLICES; i++)
    {
        stubs[i] = build_stub(&SPLICES[i]);
        if (!stubs[i]) { wlog("disarmed: stub pool exhausted"); return 0; }
    }
    /* Every stub and trampoline exists; now the writes, which cannot fail on a
       page that memcmp just read (VirtualProtect on the image is the only
       error path and it is checked). */
    for (i = 0; i < N_HOOKS; i++)
        if (!write_jump(HOOKS[i].addr, HOOKS[i].len, (const u8*)HOOKS[i].repl, 1))
        { wlog("VirtualProtect failed at hook %s — image partially patched!", HOOKS[i].name); return 0; }
    for (i = 0; i < N_SPLICES; i++)
        if (!write_jump(SPLICES[i].addr, SPLICES[i].len, stubs[i], 1))
        { wlog("VirtualProtect failed at splice %s — image partially patched!", SPLICES[i].name); return 0; }
    for (i = 0; i < N_PATCHES; i++)
        if (!write_jump(PATCHES[i].addr, PATCHES[i].len, PATCHES[i].repl, 0))
        { wlog("VirtualProtect failed at patch %s — image partially patched!", PATCHES[i].name); return 0; }
    return 1;
}

/* =========================================================================
   10. Init and the oracle
   ========================================================================= */

void tagpu_weapons_init(void)
{
    build_names();
    if (GetFileAttributesA(WPN_FLAG) == INVALID_FILE_ATTRIBUTES)
    {
        /* off: not one byte of the image is touched (assertion 1) */
        return;
    }
    g_armed = install();
    wlog("%s (" WPN_FLAG " present): cap=%d, %u entry hooks, %u splices, %u byte patches, pool %u bytes",
         g_armed ? "ARMED" : "DISARMED", WPN_CAP, (u32)N_HOOKS, (u32)N_SPLICES, (u32)N_PATCHES, g_pool_used);
}

/* ---- oracle: tagpu_weapons.trigger -> tagpu_weapons.json -------------------- */

typedef struct Out { char* p; char* end; } Out;
static void oput(Out* o, const char* fmt, ...)
{
    va_list ap;
    int n;
    if (o->p >= o->end) return;
    va_start(ap, fmt);
    n = _vsnprintf(o->p, (size_t)(o->end - o->p), fmt, ap);
    va_end(ap);
    if (n < 0 || o->p + n >= o->end) { o->p = o->end; return; }
    o->p += n;
}

static void oname(Out* o, const char* s, int max)
{
    int i;
    oput(o, "\"");
    for (i = 0; i < max && s[i]; i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') oput(o, "\\%c", c);
        else if (c >= 0x20 && c < 0x7F) oput(o, "%c", c);
        else oput(o, "?");
    }
    oput(o, "\"");
}

static void dump_unit(Out* o, char* u, int first)
{
    char* def   = UDEF(u);
    int   count = wpn_count(u), i;
    oput(o, "%s{\"idx\":%d,\"type\":", first ? "" : ",", (int)*(i16*)(u + 0xA8));
    oname(o, def + 0x20, 32);
    oput(o, ",\"owner\":%d,\"count\":%d,\"crc_weapons\":\"0x%08X\",\"crc_all\":\"0x%08X\",\"slots\":[",
         (int)*(u8*)(u + 0xFF), count,
         *(u32*)(def + DEF_CRC_WPN), *(u32*)(def + DEF_CRC_ALL));
    for (i = 0; i < count; i++)
    {
        WSlot* s = (i < 3) ? (WSlot*)(u + 4 + i * SLOT_STRIDE) : (side_row(u) ? side_row(u) + (i - 3) : 0);
        if (!s) break;
        oput(o, "%s{\"i\":%d,\"state\":\"0x%02X\",\"weapon\":", i ? "," : "", i, s->state);
        if (s->weapon && (size_t)s->weapon > 0x600000u && *(char*)(s->weapon + 0x10A) != 0)
            oname(o, s->weapon, 32);
        else
            oput(o, "null");
        oput(o, ",\"target\":%u,\"spot\":\"0x%04X\",\"reload\":%u,\"heading\":%d,\"pitch\":%d,"
                "\"stock\":%u,\"aimed\":%d,\"zangle\":%d,\"thread\":\"%p\"}",
             s->target, s->spot, s->reload, s->heading, s->pitch, s->stock, s->aimed, s->zangle, s->thread);
    }
    oput(o, "]}");
}

static void oracle(const char* spec, unsigned frame)
{
    static char buf[1 << 20];
    Out   o = { buf, buf + sizeof buf - 1 };
    char* ta = TA();
    int   want_all = 0, wanted[256], nwanted = 0, i, first = 1;
    const char* p;
    FILE* f;

    for (p = spec; *p; )
    {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (!*p) break;
        if (!strncmp(p, "all", 3)) { want_all = 1; p += 3; continue; }
        if (*p >= '0' && *p <= '9') { if (nwanted < 256) wanted[nwanted++] = atoi(p); while (*p >= '0' && *p <= '9') p++; continue; }
        p++;
    }

    oput(&o, "{\"frame\":%u,\"armed\":%s,\"cap\":%d,\"hits\":{", frame, g_armed ? "true" : "false", WPN_CAP);
    for (i = 0; i < H__N; i++) oput(&o, "%s\"%s\":%u", i ? "," : "", HIT_NAMES[i], g_hits[i]);
    oput(&o, "},\"fires\":[");
    for (i = 0; i < WPN_CAP; i++) oput(&o, "%s%u", i ? "," : "", g_fires[i]);
    oput(&o, "],\"units\":[");
    if ((size_t)ta >= 0x600000u)
    {
        char* beg = *(char**)(ta + OFF_UNITS_BEGIN);
        char* end = *(char**)(ta + OFF_UNITS_END);
        if ((size_t)beg >= 0x600000u && end > beg && (size_t)(end - beg) <= (size_t)UNIT_STRIDE * 20000)
        {
            char* u;
            for (u = beg + UNIT_STRIDE; u < end; u += UNIT_STRIDE)
            {
                u32 st = *(u32*)(u + 0x110);
                int idx = (int)*(i16*)(u + 0xA8), k, hit = want_all;
                if (!(st & 0x10000000) || (st & 0x4000)) continue;
                if ((size_t)UDEF(u) < 0x600000u) continue;
                for (k = 0; k < nwanted && !hit; k++) if (wanted[k] == idx) hit = 1;
                if (!hit) continue;
                dump_unit(&o, u, first);
                first = 0;
            }
        }
    }
    oput(&o, "]}\n");
    *o.p = 0;

    f = fopen(WPN_JSON ".tmp", "wb");
    if (!f) return;
    fwrite(buf, 1, (size_t)(o.p - buf), f);
    fclose(f);
    MoveFileExA(WPN_JSON ".tmp", WPN_JSON, MOVEFILE_REPLACE_EXISTING);
}

void tagpu_weapons_frame(unsigned int frame)
{
    char  spec[4096];
    DWORD got = 0;
    HANDLE h;
    if (frame % 5) return;
    if (GetFileAttributesA(WPN_TRIGGER) == INVALID_FILE_ATTRIBUTES) return;
    h = CreateFileA(WPN_TRIGGER, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, spec, sizeof spec - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    DeleteFileA(WPN_TRIGGER);
    spec[got] = 0;
    oracle(spec, frame);
}
