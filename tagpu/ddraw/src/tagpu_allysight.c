/* tagpu_allysight.c -- allies share their sight, their death vision and their explored map, and
   the engine's sight counters never wrap (research/notes/modules/allied-sharing.md).

   S, THE SNAPSHOT: bit t of s_share[o] says that o's sight stamps also go into record t's sight
   counters (player +0x7C); bit o itself says that o's own counters are kept through W. S holds
   only records that are allied both ways (each one's +0x108 row byte toward the other non-zero,
   the engine's own zero test, 0x4674C5, 0x40AB05), and is symmetric by construction.

   W, THE WIDE COUNTERS: record t's cell i is s_wide[t * s_cells + i]. The engine's counter is one
   byte that a stamp increments; a cell under more stamps than 255 wraps to 0 and reads as unseen.
   Every stamp and removal of an owner S manages adds or subtracts one in W for each target and
   writes min(W, 255) into the target's byte. Every engine reader of the counters tests the byte
   against zero (DISASSEMBLED, the 22 readers listed in the module note), so min(W, 255) reads as
   the true count does. THE BOUND ON W: a cell holds at most (live stamp records) x (one walk's
   visits to a cell) increments, and the live stamp records are at most the unit slots and the 20
   temporary records, far below 2^32, so an increment cannot wrap; a removal of a cell at 0 leaves
   0 and is counted (s_diag.unmatched), where the engine's byte would wrap to 255.

   THE INVARIANT (an ordering): for every live stamp record s and every record t, t's counters
   hold s's walk exactly when bit t of s_share[owner(s)] & validmask() is set. Stamps and removals
   read S and nothing else; S changes only at three points -- the level's player init (own bits,
   W zero, no stamp alive yet), the step's players phase on the owner's machine, and the replay's
   table write on a puppet host -- each followed, under s_lock, by migrate() before any further
   stamp; and valid() does not change within a level (the counters are allocated by the player
   init and freed by the teardown only). And under the ray fan a record is removed only while it
   holds a stamp: the removal 0x481D50 has four engine calls (DISASSEMBLED) -- the fan emitter's,
   which tests the record's byte first (0x482634), the circle emitter's (0x48275D, never under the
   fan), and the two that test nothing, 0x482090's (0x482104, whose one call is the destructor's
   0x486845) and the temporary records' expiry's (0x482161) -- and those two calls are gated on
   the same byte here (destructor_sight, eye_expire), as are the migration's walks (migrate_units,
   migrate_eyes). So every removal subtracts exactly what its stamp added.

   THE REMOVAL IS THE STAMP'S WALK WITH THE SIGN REVERSED. The stamp 0x482270 and the removal
   0x481D50 walk identically (DISASSEMBLED: 0x481D50..0x481D88 is 0x482270..0x4822A8 byte for byte,
   the rest differs in `dec` for `inc`, the order of two loads in the circle loop and the jump
   displacements), and the walk reads only the record, the map's terrain sight grid main+0x1428F
   and the circle table main+0x1485B -- never the counters -- so one walk serves both, and a
   removal cannot visit a cell its stamp did not. The removal's own body never runs again.

   THE LOCK. s_lock (recursive) guards S, W, s_cur and s_mig. It is a leaf lock: held only around
   the stamp body, whose callees 0x433500, 0x433520, 0x4335C0, 0x4335E0, 0x4339C0, 0x4339E0 and
   0x4B7F30 are leaves (DISASSEMBLED), around 0x482090 -- which builds a unit's record and calls
   0x481D50, that is remove_hook, re-entering the lock (DISASSEMBLED 0x482090..0x48210C) -- and
   0x485420 (a loop with no call, DISASSEMBLED 0x485420..0x485490), and around SIM_ALLOC and
   SIM_FREE; never around 0x4816A0, 0x464990, 0x464F80, 0x481930 or a log call. It exists because
   the stamps run on the loader thread during a load, and in a network load a leaver's kill-all can
   run on the game thread's load loop at the same time (engine/units.md, *The removal runs on
   whichever thread pumps*). That race is the engine's own, on its counters and its unit array,
   and it is not closed here: the lock keeps one walk at a time in s_cur and W consistent through
   it, and every index this file takes from engine memory is bounded before it is used.

   THREADS: the hooks run on whichever thread calls the engine's functions -- the game thread in
   play, the loader thread during a load (players_init_hook and the loader's stamps). */

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "tagpu_allysight.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "tagpu_puppet.h"
#include "tagpu_simreg.h"

SIM_FEATURE(ally_sight, SIM_PUPPET_RUNS, "battle-allied battle-los forest-circle tenseat");

/* the engine's main block and the fields this file reads or writes (DISASSEMBLED, the uses cited
   where each is read) */
#define TA_MAIN        0x00511DE8u
#define OFF_PLAYERS    0x1B63       /* the player records, PL_STRIDE bytes each                   */
#define PL_STRIDE      0x14B
#define PL_INUSE       0x00         /* non-zero for a record in use (0x48170C)                    */
#define PL_FIRST       0x67         /* the record's unit block, first and last unit, inclusive    */
#define PL_LAST        0x6B
#define PL_TYPE        0x73         /* the controller: 1..3 a seat the rebuild refills (0x481717) */
#define PL_SIGHT       0x7C         /* u8* the sight counters (0x46487B)                          */
#define PL_COLS        0x80         /* their columns and rows, the plot's halved (0x464849)       */
#define PL_ROWS        0x84
#define PL_CELLS       0x88         /* their length, (cols * rows + 7) & ~7 (0x464866)            */
#define PL_ALLY        0x108        /* one byte toward each record: non-zero allied               */
#define PL_MAPBIT      0x146        /* the record's bit in the explored map; 10 = none (0x48172A) */
#define OFF_VIEWED     0x2A43
#define OFF_GAMETIME   0x38A47
#define OFF_PLOTW      0x14233
#define OFF_PLOTH      0x14237
#define OFF_NEYES      0x14277      /* the temporary sight records' count, at most EYE_MAX        */
#define OFF_EYES       0x1427B      /* their block, EYE_STRIDE bytes each                         */
#define OFF_LOSTYPE    0x14281      /* u16: bit 1 true LOS, bit 2 the ray fan, bit 3 grid current */
#define OFF_MMFLAGS    0x142F1      /* bit 2: the minimap's fog is redrawn                        */
#define OFF_UNITS      0x14357      /* the unit array's first and last slot                       */
#define OFF_UNITS_LAST 0x1435B
#define OFF_CIRCLES    0x1485B      /* the circle mode's shapes: u16 count, entries at +0x28      */
#define U_STRIDE       0x118
#define U_OWNER        0x96         /* the owner's player record                                  */
#define U_TYPE         0xA6         /* u16, non-zero while the unit lives (0x481796)              */
#define U_SIGHTB       0xF8         /* the stamp's byte: the ray fan's height (0 = no stamp), the
                                       circle mode's shape index                                  */
#define EYE_STRIDE     0x24
#define EYE_MAX        20           /* the add refuses a 21st (0x48292C)                          */
#define LOS_TRUE       0x2u
#define LOS_FAN        0x4u
#define LOS_GRIDOK     0x8u
#define PLAYERS        10
#define NO_OWNER       0xFFu
#define CELLS_MAX      0x400000u    /* past any map's counters: W would be 160 MB                 */

typedef void (__stdcall* PFN_Rec)(unsigned char* rec);
typedef void (__stdcall* PFN_Arg)(unsigned arg);
typedef void (__cdecl*   PFN_Void)(void);
typedef void (__stdcall* PFN_Unit)(char* unit);
typedef void (__stdcall* PFN_MapCopy)(int from, int to);
/* 0x482090(unit): builds the unit's sight record and calls the removal 0x481D50, stdcall `ret 4`
   (DISASSEMBLED 0x482090..0x48210C) */
#define E_SightRemove ((PFN_Unit)0x00482090u)
/* 0x481D50(rec): the removal, which jumps to remove_hook once installed */
#define E_Remove ((PFN_Rec)0x00481D50u)
/* 0x464F80(): the players phase, plain `ret` (0x465602) */
#define E_PlayersPhase ((PFN_Void)0x00464F80u)
/* 0x485420(from, to): every explored-map entry that has bit `from` gains bit `to`, stdcall
   `ret 8`; the arguments are map bits (DISASSEMBLED 0x485420..0x485490) */
#define E_MapCopy ((PFN_MapCopy)0x00485420u)

/* ---- state --------------------------------------------------------------------------------- */

static uint16_t s_share[PLAYERS];                 /* S                                         */
SIM_VAR(s_share, SIM_STATE);
static uint32_t* s_wide;                          /* W                                         */
static unsigned  s_wideN;                         /* its cells, PLAYERS * s_cells              */
SIM_HEAP(s_wide, SIM_REBUILT, SIM_ID_NONE, sizeof(uint32_t), s_wideN, NULL);
SIM_VAR(s_wideN, SIM_CONTENT);
static unsigned  s_cells;                         /* +0x88 common to the level's records       */
SIM_VAR(s_cells, SIM_CONTENT);
static CRITICAL_SECTION s_lock;
SIM_LOCK(s_lock);

/* the walk in progress: written before every walk, read only by the increment sites inside it */
static struct {
    unsigned char* base;                          /* the owner's counters                      */
    unsigned char* grid[PLAYERS];                 /* each target's counters                    */
    unsigned targets;
    int sign;
    int wide;
} s_cur;
SIM_VAR(s_cur, SIM_TRANSIENT);
/* set and cleared inside migrate: the removal 0x482090 calls walks the record into `add` and
   out of `rem` instead of removing it */
static struct { int on; unsigned add, rem; } s_mig;
SIM_VAR(s_mig, SIM_TRANSIENT);
static uint16_t s_before[PLAYERS];                /* S as the replay's table write found it    */
SIM_VAR(s_before, SIM_TRANSIENT);

static struct {
    unsigned offgrid;     /* a cell past the owner's counters: nothing written                  */
    unsigned unmatched;   /* a removal of a wide cell at 0: left at 0                           */
    unsigned circle;      /* a circle walk whose shape the table has not: no walk               */
    unsigned refused;     /* a level whose records' counters differ in length: no W, stock      */
    unsigned eyekept;     /* a rebuild found the temporary records' count out of 0..20          */
    unsigned migrations;  /* owners whose sharing changed                                       */
    unsigned foreign;     /* a unit in an owner's block that another record owns: not migrated  */
} s_diag;
SIM_VAR(s_diag, SIM_PRIVATE);

static int s_on;
SIM_VAR(s_on, SIM_INSTALL);
static PFN_Rec  s_trStamp;                        /* 0x482270's first 9 bytes, then its body   */
SIM_VAR(s_trStamp, SIM_INSTALL);
static PFN_Rec  s_trMapped;                       /* 0x481930's first 5                         */
SIM_VAR(s_trMapped, SIM_INSTALL);
static PFN_Arg  s_trRebuild;                      /* 0x4816A0's first 7                         */
SIM_VAR(s_trRebuild, SIM_INSTALL);
static PFN_Void s_trPlayersInit;                  /* 0x464990's first 9                         */
SIM_VAR(s_trPlayersInit, SIM_INSTALL);

/* ---- engine memory ------------------------------------------------------------------------- */

static char* ta_main(void) { return *(char* volatile*)(size_t)TA_MAIN; }
static char* prec(char* ta, unsigned p) { return ta + OFF_PLAYERS + (size_t)p * PL_STRIDE; }
static uint32_t rd32(const char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const char* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static char* rdptr(const char* p) { char* v; memcpy(&v, p, 4); return v; }

/* A player record pointer as its index, or NO_OWNER when it is not exactly one of records
   0..10: a value read from engine memory is data until bounded. */
static unsigned rec_index(char* ta, const char* p)
{
    size_t d = (size_t)(p - (ta + OFF_PLAYERS));
    if (d % PL_STRIDE || d / PL_STRIDE > PLAYERS) return NO_OWNER;
    return (unsigned)(d / PL_STRIDE);
}

/* The record a sight record names at +0, as an index (rec_index) */
static unsigned owner_of(char* ta, const unsigned char* rec)
{
    return rec_index(ta, rdptr((const char*)rec));
}

/* Record r's unit block [+0x67, +0x6B], its first and last unit inclusive, when it holds a unit
   and both ends lie inside the unit array [main+0x14357, main+0x1435B] on its stride: data until
   bounded. The unit array's setup is the fields' only writer (0x4858BD, 0x4858E0, at the level's
   load), so a bounded block stays bounded for the level. */
static int unit_block(char* ta, const char* r, char** first, char** last)
{
    char* begin = rdptr(ta + OFF_UNITS);
    char* end = rdptr(ta + OFF_UNITS_LAST);
    *first = rdptr(r + PL_FIRST);
    *last = rdptr(r + PL_LAST);
    return begin && *first >= begin && *last <= end && *first <= *last &&
           (size_t)(*first - begin) % U_STRIDE == 0 && (size_t)(*last - begin) % U_STRIDE == 0;
}

/* MEMORY: record t's counters exist at the level's common length, so a cell offset W bounds is
   inside them. Constant from the level's player init to its teardown: the player init 0x464700
   allocates them (its one caller 0x4649B1) and the teardown 0x464A00 frees them, storing NULL and
   0 (0x464A24..0x464A38). Every walk masks its targets with it. */
static int valid(char* ta, unsigned t)
{
    const char* r;
    if (t >= PLAYERS || !s_wide) return 0;
    r = prec(ta, t);
    return rdptr(r + PL_SIGHT) != NULL && rd32(r + PL_CELLS) == s_cells;
}

static unsigned validmask(char* ta)
{
    unsigned t, m = 0;
    for (t = 0; t < PLAYERS; t++)
        if (valid(ta, t)) m |= 1u << t;
    return m;
}

/* GAME STATE, read only where S is computed: valid, in use, a seat 1..3, its map bit its own
   index, and counters shaped as the map's plot halved, as the player init halves it (`cdq; sub
   eax,edx; sar eax,1`, 0x46482B..0x46484F). The first three terms are the sight rebuild's own
   refill test (0x48170C..0x48177A) with +0x146 == p in place of +0x146 != 10, so every target S
   can name is a record 0x4816A0 refills. +0x73 is `drop` on a puppet host, which holds 3 there:
   the test reads 1..3, never 1 or 2 against 3. */
static int eligible(char* ta, unsigned p)
{
    const char* r;
    unsigned char type;
    if (!valid(ta, p)) return 0;
    r = prec(ta, p);
    type = (unsigned char)r[PL_TYPE];
    return rd32(r + PL_INUSE) != 0 && type >= 1 && type <= 3 &&
           (unsigned char)r[PL_MAPBIT] == p &&
           (int32_t)rd32(r + PL_COLS) == (int32_t)rd32(ta + OFF_PLOTW) / 2 &&
           (int32_t)rd32(r + PL_ROWS) == (int32_t)rd32(ta + OFF_PLOTH) / 2;
}

/* The circle mode takes its shape from the table at main+0x1485B by the byte the record's +0xC
   points at (0x4824D9..0x4824E8); 0x4B7F30 returns NULL for an index at or past the table's u16
   count (0x4B7F44) and the walk then reads [NULL] (0x4824F1). Reachable only for a record stamped
   in the ray fan's mode and walked in the circle's. */
static int circle_ok(char* ta, const unsigned char* rec)
{
    const char* table = rdptr(ta + OFF_CIRCLES);
    const char* at = rdptr((const char*)rec + 0xC);
    unsigned idx;
    if (!table || !at) return 0;
    idx = (unsigned char)*at;
    return idx < rd16(table) && rdptr(table + 0x28 + (size_t)idx * 8) != NULL;
}

/* ---- the walk ------------------------------------------------------------------------------ */

/* The stock byte for an owner S does not manage, wrapping as the engine's does; else W and the
   targets' bytes. THE BOUND: the stamp body bounds its cells by the terrain sight grid's and the
   plot halves' dimensions (0x4822F8..0x482307, 0x482423..0x482438, 0x4822AE..0x4822CB), which
   eligible() ties to +0x80/+0x84; a cell at or past +0x88 is memory the counters do not own, and
   nothing is written there. */
static void cell_apply(unsigned char* cell)
{
    size_t off;
    unsigned t, m;
    if (!s_cur.wide) {
        *cell = (unsigned char)(*cell + s_cur.sign);
        return;
    }
    off = (size_t)(cell - s_cur.base);
    if (off >= s_cells) {
        s_diag.offgrid++;
        return;
    }
    for (m = s_cur.targets, t = 0; m; m >>= 1, t++) {
        uint32_t* w;
        if (!(m & 1)) continue;
        w = &s_wide[(size_t)t * s_cells + off];
        if (s_cur.sign > 0) ++*w;
        else if (*w) --*w;
        else s_diag.unmatched++;
        s_cur.grid[t][off] = *w > 255 ? 255 : (unsigned char)*w;
    }
}

/* The three increment sites of the stamp body, each its own function so that a coverage run
   names which ran. */
static void __cdecl cell_center(unsigned char* cell) { cell_apply(cell); }
static void __cdecl cell_ray(unsigned char* cell)    { cell_apply(cell); }
static void __cdecl cell_circle(unsigned char* cell) { cell_apply(cell); }

/* One walk of `rec` with `sign` into `targets`, through the stamp body, under s_lock. The stamp
   addresses the owner's counters ([owner+0x7C] + row * [owner+0x80] + col: 0x482390..0x48239A,
   0x48246C..0x48247D, 0x482557..0x48256A), so a cell's offset from the owner's base is the same
   cell in every target's. */
static void walk(char* ta, unsigned char* rec, int sign, unsigned targets)
{
    unsigned o = owner_of(ta, rec), t, v;
    if (!(rd16(ta + OFF_LOSTYPE) & LOS_FAN) && !circle_ok(ta, rec)) {
        s_diag.circle++;
        return;
    }
    targets &= validmask(ta);
    s_cur.wide = o < PLAYERS && valid(ta, o) && targets;
    s_cur.sign = sign;
    if (s_cur.wide) {
        s_cur.base = (unsigned char*)rdptr(prec(ta, o) + PL_SIGHT);
        s_cur.targets = targets;
        for (t = 0; t < PLAYERS; t++)
            s_cur.grid[t] = (targets >> t & 1) ? (unsigned char*)rdptr(prec(ta, t) + PL_SIGHT)
                                               : NULL;
    }
    s_trStamp(rec);
    /* The stamp marks the fog grid stale for the viewed record's own stamps (0x482283..0x4822A1);
       a receiver that is the viewed record is marked the same way. Both fields are `leave` rows
       of replay-globals.tsv (losgridok, redrawflags). */
    v = (unsigned char)ta[OFF_VIEWED];
    if (s_cur.wide && v < PLAYERS && v != o && (targets >> v & 1)) {
        uint16_t los = rd16(ta + OFF_LOSTYPE);
        los = (uint16_t)(los & ~LOS_GRIDOK);
        memcpy(ta + OFF_LOSTYPE, &los, 2);
        ta[OFF_MMFLAGS] = (char)(ta[OFF_MMFLAGS] | 4);
    }
    s_cur.wide = 0;
}

static void walk_rec(unsigned char* rec, int sign)
{
    char* ta = ta_main();
    unsigned o;
    EnterCriticalSection(&s_lock);
    o = owner_of(ta, rec);
    walk(ta, rec, sign, o < PLAYERS ? s_share[o] : 0);
    LeaveCriticalSection(&s_lock);
}

/* 0x482270(rec), the stamp, stdcall `ret 4` (0x4824D6, 0x48259E) */
static void __stdcall stamp_hook(unsigned char* rec)
{
    walk_rec(rec, +1);
}

/* 0x481D50(rec), the removal, stdcall `ret 4` (0x481FB6, 0x482082): the stamp's walk with the
   sign reversed (the head comment says why that is exact). Inside migrate, the call comes from
   its own 0x482090 and walks the record into the targets gained and out of those lost. */
static void __stdcall remove_hook(unsigned char* rec)
{
    char* ta = ta_main();
    EnterCriticalSection(&s_lock);
    if (s_mig.on) {
        if (s_mig.add) walk(ta, rec, +1, s_mig.add);
        if (s_mig.rem) walk(ta, rec, -1, s_mig.rem);
    } else {
        unsigned o = owner_of(ta, rec);
        walk(ta, rec, -1, o < PLAYERS ? s_share[o] : 0);
    }
    LeaveCriticalSection(&s_lock);
}

/* 0x481930(rec), the explored map's stamp, stdcall `ret 4` (0x481D49), then once for each other
   record S gives the owner's sight. The body reads only rec+0 (its +0x146), +4, +8 and +0xC
   (0x481940, 0x481979, 0x4819C9, 0x481A74, 0x481BC7, 0x481D1B), so a 16-byte copy naming the
   target at +0 stamps the target's bit. It sets the bit only where none of the bits it tests is
   set and xors it in (0x481A53..0x481A67, 0x481B61..0x481B75, 0x481CCC..0x481CE4), so a mask of
   several bits would skip a cell where any one of them is set: one call per target. It marks the
   viewer's redraw itself when the record it names is the viewed one (0x481D1D). */
static void __stdcall mapped_hook(unsigned char* rec)
{
    char* ta = ta_main();
    unsigned o, m, t;
    s_trMapped(rec);
    o = owner_of(ta, rec);
    if (o >= PLAYERS) return;
    EnterCriticalSection(&s_lock);
    m = s_share[o] & ~(1u << o) & validmask(ta);
    LeaveCriticalSection(&s_lock);
    for (t = 0; t < PLAYERS; t++) {
        unsigned char copy[16];
        char* rt;
        if (!(m >> t & 1)) continue;
        memcpy(copy, rec, sizeof copy);
        rt = prec(ta, t);
        memcpy(copy, &rt, 4);
        s_trMapped(copy);
    }
}

/* 0x4816A0(arg), the sight rebuild, stdcall `ret 4` (0x48192B): it refills every record that
   passes its test (+0x00 != 0, +0x73 in 1..3, +0x146 != 10, records 0..9) with the mode's base
   (~LosType >> 1) & 1 (0x48170C..0x481767), re-stamps every live unit through 0x482270 and
   redraws the minimap. LosType is already the command's new value: every writer of bits 0-2
   calls 0x4816A0 after writing (replay-globals.tsv, the lostype row). So W of every such valid
   record takes the same base first.
   THE TEMPORARY RECORDS ("death vision", main+0x1427B): the refill wipes their stamps, and the
   engine's records outlive it and later decrement counters that no longer hold them (the expiry
   0x482130 -> 0x481D50 at 0x482161). Emptying the block before the refill leaves what the engine
   shows -- their stamps are gone either way -- and nothing to subtract later. The replay's
   los_react empties it first through the engine's expiry, in the old mode (sight_records_drop),
   so it reaches this with a count of 0 and both paths end alike. */
static void __stdcall rebuild_hook(unsigned arg)
{
    char* ta = ta_main();
    unsigned t;
    int32_t n;
    EnterCriticalSection(&s_lock);
    n = (int32_t)rd32(ta + OFF_NEYES);
    if (n >= 0 && n <= EYE_MAX) memset(ta + OFF_NEYES, 0, 4);
    else s_diag.eyekept++;
    if (s_wide) {
        uint32_t base = (uint32_t)(~((unsigned char)ta[OFF_LOSTYPE] >> 1) & 1u);
        for (t = 0; t < PLAYERS; t++) {
            const char* r = prec(ta, t);
            unsigned char type = (unsigned char)r[PL_TYPE];
            unsigned i;
            uint32_t* w;
            if (!rd32(r + PL_INUSE) || type < 1 || type > 3 || (unsigned char)r[PL_MAPBIT] == 10 ||
                !valid(ta, t))
                continue;
            w = s_wide + (size_t)t * s_cells;
            for (i = 0; i < s_cells; i++) w[i] = base;
        }
    }
    LeaveCriticalSection(&s_lock);
    s_trRebuild(arg);
}

/* ---- the migration ------------------------------------------------------------------------- */

/* Every unit of o's block that holds a stamp, walked through the engine's own record builder
   0x482090 into `add` and out of `rem`. THE BOUND: unit_block; a unit lives in its owner's block
   for its whole life (+0x96 set per slot at level load, 0x485887, 0x485902; a capture makes a new
   unit, 0x488570), and one whose +0x96 names another record is left alone and counted. A unit
   holds a stamp when it lives (+0xA6 != 0, the rebuild's own test, 0x481796) and its stamp byte
   says so: the ray fan's height non-zero, the circle's index below the table's count (the
   replay's book_stamp rule). Only under LosType bit 1: without it nothing stamps (the emitter's
   test 0x48267B, the rebuild's 0x481808). */
static void migrate_units(char* ta, char* ro, unsigned add, unsigned rem, uint16_t los)
{
    char *first, *last, *u;
    unsigned count = 0;
    if (!unit_block(ta, ro, &first, &last)) return;
    if (!(los & LOS_FAN)) {
        const char* table = rdptr(ta + OFF_CIRCLES);
        if (!table) return;
        count = rd16(table);
    }
    for (u = first; u <= last; u += U_STRIDE) {
        unsigned char h;
        if (!rd16(u + U_TYPE)) continue;
        if (rdptr(u + U_OWNER) != ro) {
            s_diag.foreign++;
            continue;
        }
        h = (unsigned char)u[U_SIGHTB];
        if ((los & LOS_FAN) ? !h : h >= count) continue;
        s_mig.add = add;
        s_mig.rem = rem;
        s_mig.on = 1;
        E_SightRemove(u);
        s_mig.on = 0;
    }
}

/* Whether a temporary sight record holds a stamp. Under the ray fan the add 0x482910 zeroes the
   record's byte (0x4829DE) and stamps only through the emitter 0x4825B0 (0x4829F4), which returns
   without stamping when the record's words already name the new cell and the byte is within 5 of
   the new height (0x48261F..0x48262E) -- words that, in a reused slot, are the last record's
   (the add points +4 at +0x20 and does not write it, 0x482955..0x482958) -- or when the cell is
   past the grid (0x482661, 0x482669); both leave the byte 0. A byte of 0 is the emitter's own test
   for no stamp (0x482634): the replay's book_stamp rule. In the circle mode the add always stamps
   (0x482A8E), and the walk refuses a shape the table has not (circle_ok). The byte is the record's
   own +0xB: +0xC points there as the add (0x482974, 0x48297B) and the expiry's compaction
   (0x4821F3, 0x4821FA) write it. */
static int eye_stamped(uint16_t los, const unsigned char* e)
{
    return !(los & LOS_FAN) || e[0xB] != 0;
}

/* Each temporary sight record of o's that holds a stamp, walked directly: a record is the stamp's
   own form (+0 the owner, +4 its words, +0xC its byte; the add 0x482A8E stamps it as it is). Its
   count is the engine's, bounded 0..20 with the block non-NULL. Which records hold a stamp:
   eye_stamped. */
static void migrate_eyes(char* ta, char* ro, unsigned add, unsigned rem)
{
    int32_t n = (int32_t)rd32(ta + OFF_NEYES), i;
    char* eyes = rdptr(ta + OFF_EYES);
    if (n <= 0 || n > EYE_MAX || !eyes) return;
    for (i = 0; i < n; i++) {
        unsigned char* e = (unsigned char*)eyes + (size_t)i * EYE_STRIDE;
        if (rdptr((const char*)e) != ro || !eye_stamped(rd16(ta + OFF_LOSTYPE), e)) continue;
        if (add) walk(ta, e, +1, add);
        if (rem) walk(ta, e, -1, rem);
    }
}

/* The counters made to hold the stamps S `to` names where they held those `from` named, for every
   owner whose sharing changed, under s_lock. An own bit gained adopts the owner's bytes into W
   (mid-level [INFERRED] unreachable: at the level's init own bits are set directly); an own bit
   lost leaves the owner's bytes as they are (a leaver has no unit left: its kill-all runs first,
   0x452E17 before 0x452E67). Every adoption runs before any walk: a walk of another owner into a
   record writes min(W, 255) into its bytes, so a record's W has to hold its bytes before the first
   walk that reaches it, whichever owner's iteration comes first. `backfill`, on the owner's
   machine only: each target gained receives every explored bit the owner has through the engine's
   own copier; the target's own iteration copies the other way. */
static void migrate(char* ta, const uint16_t from[PLAYERS], const uint16_t to[PLAYERS],
                    int backfill)
{
    unsigned vm = validmask(ta), o, r;
    uint16_t los = rd16(ta + OFF_LOSTYPE);
    for (o = 0; o < PLAYERS; o++) {
        const unsigned char* g;
        uint32_t* w;
        unsigned i;
        if (!(vm >> o & 1) || !(to[o] >> o & 1) || (from[o] >> o & 1)) continue;
        g = (const unsigned char*)rdptr(prec(ta, o) + PL_SIGHT);
        w = s_wide + (size_t)o * s_cells;
        for (i = 0; i < s_cells; i++) w[i] = g[i];
    }
    for (o = 0; o < PLAYERS; o++) {
        unsigned add, rem;
        char* ro;
        if (from[o] == to[o] || !(vm >> o & 1)) continue;
        add = to[o] & ~from[o] & vm & ~(1u << o);
        rem = from[o] & ~to[o] & vm & ~(1u << o);
        ro = prec(ta, o);
        if (add | rem) {
            if (los & LOS_TRUE) migrate_units(ta, ro, add, rem, los);
            migrate_eyes(ta, ro, add, rem);
            if (backfill)
                for (r = 0; r < PLAYERS; r++)
                    if (add >> r & 1) E_MapCopy((int)o, (int)r);
        }
        s_diag.migrations++;
    }
}

/* ---- the level ----------------------------------------------------------------------------- */

/* The ended level's diagnostics, logged as the next one starts, then zeroed */
static void diag_flush(void)
{
    tagpu_logf("allysight: the last level: %u owners' sharing changed, %u cells past the counters, "
               "%u removals at 0, %u circle walks refused, %u rebuilds found the temporary "
               "records' count out of range, %u units in another owner's block, %u levels "
               "without wide counters", s_diag.migrations, s_diag.offgrid, s_diag.unmatched,
               s_diag.circle, s_diag.eyekept, s_diag.foreign, s_diag.refused);
    memset(&s_diag, 0, sizeof s_diag);
}

/* 0x464990(), the level's players init, plain `ret` (0x4649C6): 0x464700 for every record with a
   type, then the AI script 0x4648E0. Its one engine caller, 0x4919C8, is the replay's redirect,
   which calls it by address, so this prologue serves both. On the LOADER thread (LoadGameData_Main
   0x4917D0); the counters W pairs with were zero-filled by 0x464700 (0x46488F) and no stamp exists
   yet: the loader's creates follow. The game thread's first step follows the loader's last write
   through bit 1 of main+0x38D75 (set 0x497C5F, polled 0x498342), and S and W are written under
   s_lock. S is emptied before the engine's init, so a stamp between the two would be the stock
   byte's. */
static void __cdecl players_init_hook(void)
{
    char* ta;
    unsigned p, cells = 0, mismatch = 0, have;
    EnterCriticalSection(&s_lock);
    memset(s_share, 0, sizeof s_share);
    LeaveCriticalSection(&s_lock);
    diag_flush();
    s_trPlayersInit();
    ta = ta_main();
    EnterCriticalSection(&s_lock);
    if (s_wide) SIM_FREE(s_wide, s_wide);
    s_wide = NULL;
    s_wideN = 0;
    s_cells = 0;
    for (p = 0; p < PLAYERS; p++) {
        const char* r = prec(ta, p);
        uint32_t len;
        if (!rdptr(r + PL_SIGHT)) continue;
        len = rd32(r + PL_CELLS);
        if (!cells) cells = len;
        else if (len != cells) mismatch = 1;
    }
    if (cells && !mismatch && cells <= CELLS_MAX) {
        s_wide = (uint32_t*)SIM_ALLOC(s_wide, (size_t)PLAYERS * cells * sizeof(uint32_t));
        if (s_wide) {
            s_cells = cells;
            s_wideN = PLAYERS * cells;
        }
    }
    if (!s_wide) s_diag.refused++;
    for (p = 0; p < PLAYERS; p++) s_share[p] = (uint16_t)(eligible(ta, p) ? 1u << p : 0);
    have = s_wide != NULL;
    LeaveCriticalSection(&s_lock);
    if (have)
        tagpu_logf("allysight: level start: wide counters of %u cells a record", cells);
    else
        tagpu_logf("allysight: level start: NO wide counters (%s, %u cells): allies share nothing "
                   "this level and the sight counters are the engine's",
                   mismatch ? "the records' counters differ in length" :
                   cells ? "no memory, or past the bound" : "no record has counters", cells);
}

/* THE REPLAY'S TABLE WRITE. On a puppet host S is SIM_STATE and the recording is its writer:
   every path that writes it goes through tables_put -- the capture points, the frame visits and
   the seek's R12, and through the seek the takeover, the forks and the rewind. s_lock is held from
   begin to the end of restored, over the write (C code alone, no engine code), so S changes under
   the lock as at the other two points, and the migration then moves the counters from the S the
   host stamped under (R3-R8b of a seek) to the recording's. No backfill: the explored map is the
   recording's, which recorded_put writes after the tables (mapped_put), as does the seek's R13;
   R14a's temporary records are made after it. */
void tagpu_allysight_restore_begin(void)
{
    if (!s_on) return;
    EnterCriticalSection(&s_lock);
    memcpy(s_before, s_share, sizeof s_before);
}

/* A recording's S is data until it is a share set, the form share_compute makes: a record in the
   set has its own bit and counters of the level's length (valid); a record outside it has an
   empty entry and appears in no other; and the set is symmetric. An invalid recorded set is
   repaired by intersection, never by adding: a record whose own bit is clear or whose counters are
   not valid leaves the set, and a pair stays only when both entries name each other. So the
   repair can make allies share less than the recording says, never more, and a record W keeps is
   one the recording kept. */
static void share_repair(char* ta, uint16_t s[PLAYERS])
{
    uint16_t was[PLAYERS];
    unsigned in = 0, o;
    memcpy(was, s, sizeof was);
    for (o = 0; o < PLAYERS; o++)
        if ((was[o] >> o & 1) && valid(ta, o)) in |= 1u << o;
    for (o = 0; o < PLAYERS; o++) {
        unsigned m = 0, r;
        if (in >> o & 1)
            for (r = 0; r < PLAYERS; r++)
                if ((in >> r & 1) && (was[o] >> r & 1) && (was[r] >> o & 1)) m |= 1u << r;
        s[o] = (uint16_t)m;
    }
}

unsigned tagpu_allysight_restored(void)
{
    unsigned moved = 0;
    if (!s_on) return 0;
    share_repair(ta_main(), s_share);
    if (memcmp(s_before, s_share, sizeof s_share)) {
        migrate(ta_main(), s_before, s_share, 0);
        moved = 1;
    }
    LeaveCriticalSection(&s_lock);
    return moved;
}

/* The destructor's sight removal, its call at 0x486845, which the destructor makes under LosType
   bit 1 (0x486838) whether or not the unit holds a stamp. Under the ray fan a unit whose height
   byte is 0 holds none (the emitter's own mark, 0x482634, 0x4826A9; after any 0x4816A0 rebuild for
   a unit at an unchanged cell with height <= 5), and removing it would undercount the owner's
   counters and, with sharing, every ally's. So the removal runs only when a stamp exists: the
   replay's book_stamp rule. */
static void __stdcall destructor_sight(char* unit)
{
    if (!(rd16(ta_main() + OFF_LOSTYPE) & LOS_FAN) || unit[U_SIGHTB]) E_SightRemove(unit);
}

/* The temporary records' expiry 0x482130, its removal call at 0x482161 (`push esi; call
   0x481D50`, esi the record), which it makes for every record past its time whether or not the
   record holds a stamp. The removal runs only when it does (eye_stamped). ebx and esi, which the
   expiry's loop keeps across the call (0x482177, 0x482178), are callee-saved here. */
static void __stdcall eye_expire(unsigned char* eye)
{
    if (eye_stamped(rd16(ta_main() + OFF_LOSTYPE), eye)) E_Remove(eye);
}

/* ---- queries ------------------------------------------------------------------------------- */

int tagpu_allysight_shares(unsigned owner, unsigned viewer)
{
    return s_on && owner != viewer && owner < PLAYERS && viewer < PLAYERS &&
           (s_share[owner] >> viewer & 1);
}

const uint32_t* tagpu_allysight_wide(unsigned p, unsigned* cells)
{
    char* ta = ta_main();
    if (!s_on || !ta || p >= PLAYERS || !(s_share[p] >> p & 1) || !valid(ta, p)) return NULL;
    *cells = s_cells;
    return s_wide + (size_t)p * s_cells;
}

/* the step's players phase, at the file's end with the feature that computes S */
static void __cdecl players_phase(void);

/* ---- install ------------------------------------------------------------------------------- */

#define VA_STAMP       0x00482270u
#define VA_REMOVE      0x00481D50u
#define VA_CENTER      0x0048239Cu
#define VA_RAY         0x0048247Fu
#define VA_CIRCLE      0x00482580u
#define VA_MAPPED      0x00481930u
#define VA_REBUILD     0x004816A0u
#define VA_PINIT       0x00464990u
#define VA_PHASE_CALL  0x0049555Fu
#define VA_DTOR_CALL   0x00486845u
#define VA_EXPIRE_CALL 0x00482161u

/* sub esp,0x38; mov ecx,[0x511DE8] -- both the stamp's and the removal's first two instructions */
static const unsigned char STAMP_WAS[9]   = { 0x83, 0xEC, 0x38, 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 };
/* mov dl,[eax]; inc dl; mov [eax],dl: the stamp's own cell. Live after: ecx (cx tested at
   0x4823A9), ebx, esi, edi, ebp; eax, edx and the flags are rewritten (0x4823A2, 0x4823A5) */
static const unsigned char CENTER_WAS[6]  = { 0x8A, 0x10, 0xFE, 0xC2, 0x88, 0x10 };
/* inc byte [edi+esi]; lea edx,[edi+esi]: a ray's cell. Live after: eax (0x48248A), ecx
   (0x482485), ebx, esi, edi, ebp; edx is rewritten at 0x482485, the flags at 0x482487 */
static const unsigned char RAY_WAS[6]     = { 0xFE, 0x04, 0x37, 0x8D, 0x14, 0x37 };
/* mov bl,[edx]; cmp bl,[ebp+8]; je +2; inc byte [ecx]: the circle's cell, the head of its inner
   loop (0x48258C jumps back here, to the stub's call). Live after: eax, ecx, edx, esi, edi, ebp,
   and bl as stock leaves it; the flags are the loop's dec esi's (0x48258B) */
static const unsigned char CIRCLE_WAS[9]  = { 0x8A, 0x1A, 0x3A, 0x5D, 0x08, 0x74, 0x02, 0xFE, 0x01 };
/* sub esp,0x44; push ebx; push ebp */
static const unsigned char MAPPED_WAS[5]  = { 0x83, 0xEC, 0x44, 0x53, 0x55 };
/* mov eax,[esp+4]; sub esp,0x24 */
static const unsigned char REBUILD_WAS[7] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x24 };
/* push esi; push edi; xor esi,esi; mov edi,10 */
static const unsigned char PINIT_WAS[9]   = { 0x56, 0x57, 0x33, 0xF6, 0xBF, 0x0A, 0x00, 0x00, 0x00 };
#define PHASE_FROM     0x00464F80u                /* call 0x464F80, E8 1C FA FC FF             */
#define DTOR_FROM      0x00482090u                /* call 0x482090, E8 46 B8 FF FF             */
#define EXPIRE_FROM    0x00481D50u                /* call 0x481D50, E8 EA FB FF FF             */

typedef struct Site {
    unsigned va;
    const unsigned char* was;                     /* the retail bytes, or NULL for a call      */
    unsigned from;                                /* ... whose target this is                  */
    unsigned char n;
    unsigned char op;                             /* 0xE9 jmp, 0xE8 call                       */
    const void* to;                               /* set once the stubs exist                 */
} Site;

/* the stolen bytes, then a jmp back past them */
static unsigned char* trampoline(unsigned va, const unsigned char* was, int n)
{
    unsigned char* s = tagpu_detour_stub();
    if (!s) return NULL;
    memcpy(s, was, (size_t)n);
    s[n] = 0xE9;
    tagpu_detour_rel(s + n + 1, va + (unsigned)n);
    return s;
}

/* push ecx; push edx; push eax; push eax; call cell_center; add esp,4; pop eax; pop edx; pop ecx;
   ret */
static unsigned char* stub_center(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x51; *p++ = 0x52; *p++ = 0x50; *p++ = 0x50;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)cell_center); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x58; *p++ = 0x5A; *p++ = 0x59;
    *p++ = 0xC3;
    return s;
}

/* push eax; push ecx; push edx; lea edx,[edi+esi]; push edx; call cell_ray; add esp,4; pop edx;
   pop ecx; pop eax; lea edx,[edi+esi] (the stolen lea); ret */
static unsigned char* stub_ray(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x50; *p++ = 0x51; *p++ = 0x52;
    *p++ = 0x8D; *p++ = 0x14; *p++ = 0x37;
    *p++ = 0x52;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)cell_ray); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x5A; *p++ = 0x59; *p++ = 0x58;
    *p++ = 0x8D; *p++ = 0x14; *p++ = 0x37;
    *p++ = 0xC3;
    return s;
}

/* mov bl,[edx]; cmp bl,[ebp+8]; je .x (the stolen test); push eax; push ecx; push edx; push ecx;
   call cell_circle; add esp,4; pop edx; pop ecx; pop eax; .x: ret */
static unsigned char* stub_circle(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char *p = s, *j;
    if (!s) return NULL;
    *p++ = 0x8A; *p++ = 0x1A;
    *p++ = 0x3A; *p++ = 0x5D; *p++ = 0x08;
    *p++ = 0x74; j = p++;
    *p++ = 0x50; *p++ = 0x51; *p++ = 0x52;
    *p++ = 0x51;
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)cell_circle); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    *p++ = 0x5A; *p++ = 0x59; *p++ = 0x58;
    *j = (unsigned char)(p - (j + 1));
    *p++ = 0xC3;
    return s;
}

#define SITE_MAX 33                               /* the longest site, the radar pass's share test */
#define SITES_MAX 11                              /* the largest set, tagpu_allysight_init's     */

static void site_bytes(const Site* s, unsigned char* b)
{
    unsigned rel = (unsigned)(size_t)s->to - (s->va + 5);
    b[0] = s->op;
    memcpy(b + 1, &rel, 4);
    memset(b + 5, 0x90, (size_t)(s->n - 5));
}

/* The first site of `site` that does not hold its retail bytes (a call: its retail target), or
   n when all do */
static unsigned sites_unmatched(const Site* site, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        int ok = site[i].was ? !memcmp((const void*)(size_t)site[i].va, site[i].was, site[i].n)
                             : tagpu_detour_redirect_ok(site[i].va, site[i].from);
        if (!ok) break;
    }
    return i;
}

/* Every site written, or none: the ones written are put back when a later write fails. Returns
   n, or the site whose write failed. Called only once sites_unmatched has passed them all. */
static unsigned sites_land(const Site* site, unsigned n)
{
    unsigned char was[SITES_MAX][SITE_MAX];
    unsigned i, written;
    if (n > SITES_MAX) return 0;
    for (i = 0; i < n; i++) memcpy(was[i], (const void*)(size_t)site[i].va, site[i].n);
    for (written = 0; written < n; written++) {
        unsigned char b[SITE_MAX];
        site_bytes(&site[written], b);
        if (!tagpu_detour_write(site[written].va, b, site[written].n)) break;
    }
    if (written < n)
        for (i = 0; i < written; i++) tagpu_detour_write(site[i].va, was[i], site[i].n);
    return written;
}

/* the radar pass's sites and UnitInPlayerLOS's, each landing's all-or-nothing, after this one's */
static void radar_init(void);
static void visible_init(void);

/* Every site byte-matched against the retail bytes before any is written, none written unless all
   match (no other module writes any of them: hook-map.md), every stub built before any lands, and
   the ones written put back when a later write fails. No branch lands inside a replaced range but
   at its first byte (DISASSEMBLED: 0x48258C -> 0x482580), and no pointer to any of the hooked
   functions is in the image. At DllMain, before the engine runs. The radar pass's and
   UnitInPlayerLOS's sites install after these, each set whole or not at all, and only when these
   are in: both read S. */
void tagpu_allysight_init(void)
{
    Site site[SITES_MAX] = {
        { VA_STAMP,      STAMP_WAS,   0,          9, 0xE9, (const void*)stamp_hook },
        { VA_REMOVE,     STAMP_WAS,   0,          9, 0xE9, (const void*)remove_hook },
        { VA_CENTER,     CENTER_WAS,  0,          6, 0xE8, NULL },
        { VA_RAY,        RAY_WAS,     0,          6, 0xE8, NULL },
        { VA_CIRCLE,     CIRCLE_WAS,  0,          9, 0xE8, NULL },
        { VA_MAPPED,     MAPPED_WAS,  0,          5, 0xE9, (const void*)mapped_hook },
        { VA_REBUILD,    REBUILD_WAS, 0,          7, 0xE9, (const void*)rebuild_hook },
        { VA_PINIT,      PINIT_WAS,   0,          9, 0xE9, (const void*)players_init_hook },
        { VA_PHASE_CALL, NULL,        PHASE_FROM, 5, 0xE8, (const void*)players_phase },
        { VA_DTOR_CALL,  NULL,        DTOR_FROM,  5, 0xE8, (const void*)destructor_sight },
        { VA_EXPIRE_CALL, NULL,      EXPIRE_FROM, 5, 0xE8, (const void*)eye_expire },
    };
    const unsigned nsite = sizeof site / sizeof site[0];
    unsigned char* stub[7] = { 0 };
    unsigned i, k;

    if ((i = sites_unmatched(site, nsite)) < nsite) {
        tagpu_logf("allysight: NOT installed: 0x%06X does not hold the retail bytes; allies "
                   "share nothing and the sight counters are the engine's", site[i].va);
        return;
    }
    stub[0] = trampoline(VA_STAMP, STAMP_WAS, 9);
    stub[1] = trampoline(VA_MAPPED, MAPPED_WAS, 5);
    stub[2] = trampoline(VA_REBUILD, REBUILD_WAS, 7);
    stub[3] = trampoline(VA_PINIT, PINIT_WAS, 9);
    stub[4] = stub_center();
    stub[5] = stub_ray();
    stub[6] = stub_circle();
    for (k = 0; k < 7; k++)
        if (!stub[k]) {
            for (k = 0; k < 7; k++) tagpu_detour_stub_free(stub[k]);
            tagpu_log("allysight: NOT installed: no stub memory; allies share nothing and the "
                      "sight counters are the engine's");
            return;
        }
    s_trStamp = (PFN_Rec)(void*)stub[0];
    s_trMapped = (PFN_Rec)(void*)stub[1];
    s_trRebuild = (PFN_Arg)(void*)stub[2];
    s_trPlayersInit = (PFN_Void)(void*)stub[3];
    site[2].to = stub[4];
    site[3].to = stub[5];
    site[4].to = stub[6];
    InitializeCriticalSection(&s_lock);
    if ((i = sites_land(site, nsite)) < nsite) {
        for (k = 0; k < 7; k++) tagpu_detour_stub_free(stub[k]);
        tagpu_logf("allysight: NOT installed: 0x%06X could not be written (the earlier sites put "
                   "back); allies share nothing and the sight counters are the engine's",
                   site[i].va);
        return;
    }
    s_on = 1;
    tagpu_log("allysight: installed: the stamp 0x482270 and its three cell sites, the removal "
              "0x481D50, the explored map's stamp 0x481930, the rebuild 0x4816A0, the players init "
              "0x464990, the players phase's call 0x49555F, the destructor's removal 0x486845 "
              "and the temporary records' expiry's removal 0x482161");
    radar_init();
    visible_init();
}

SIM_FEATURE(ally_share, SIM_PUPPET_OWNER, "battle-allied tenseat");

/* S from the records: each eligible owner shares with every eligible record whose row byte
   toward it, and its own toward that record, are both non-zero: TA's ShareRadar and ShareLOS bits
   are not read, and a one-way alliance shares nothing (the module note's decisions). */
static void share_compute(char* ta, uint16_t out[PLAYERS])
{
    unsigned o, r;
    unsigned char ok[PLAYERS];
    for (o = 0; o < PLAYERS; o++) ok[o] = (unsigned char)eligible(ta, o);
    for (o = 0; o < PLAYERS; o++) {
        unsigned m = 0;
        if (ok[o]) {
            m = 1u << o;
            for (r = 0; r < PLAYERS; r++)
                if (r != o && ok[r] && prec(ta, o)[PL_ALLY + r] && prec(ta, r)[PL_ALLY + o])
                    m |= 1u << r;
        }
        out[o] = (uint16_t)m;
    }
}

/* The step's players phase, its call at 0x49555F (the loader's at 0x497C10 stays stock). On the
   owner's machine S follows the alliances before the phase, so its radar pass and the next unit
   tick read the new S; on a puppet host the recording is S's writer (tables_put). */
static void __cdecl players_phase(void)
{
    char* ta = ta_main();
    if (!tagpu_puppet_on()) {
        uint16_t now[PLAYERS], was[PLAYERS];
        share_compute(ta, now);
        if (memcmp(now, s_share, sizeof now)) {
            EnterCriticalSection(&s_lock);
            memcpy(was, s_share, sizeof was);
            migrate(ta, s_share, now, 1);
            memcpy(s_share, now, sizeof s_share);
            LeaveCriticalSection(&s_lock);
            tagpu_logf("allysight: GameTime %u: S %03X %03X %03X %03X %03X %03X %03X %03X %03X %03X"
                       " (was %03X %03X %03X %03X %03X %03X %03X %03X %03X %03X)",
                       rd32(ta + OFF_GAMETIME), now[0], now[1], now[2], now[3], now[4], now[5], now[6],
                       now[7], now[8], now[9], was[0], was[1], was[2], was[3], was[4], was[5],
                       was[6], was[7], was[8], was[9]);
        }
    }
    E_PlayersPhase();
}

SIM_FEATURE(ally_radar, SIM_PUPPET_RUNS, "battle-allied contact-waves");

/* THE RADAR PASS 0x467440, no argument, plain `ret` (0x467836; DISASSEMBLED 0x467440..0x467836):
   the viewed record's contacts, +0x110 bits 8-10 of every unit, in five loops over the unit array.
   Its one engine call is 0x46556D in the players phase 0x464F80; the replay calls it too
   (tagpu_replay.c). The record it runs for, the viewer, is the viewed one: `ebp`, made from the
   byte at main+0x2A43 (0x46745B..0x467490). Allies share contacts at three of its loops:
   - loop 1 (0x46749F..0x46750C) gives a unit bits 8-9 when its owner is the viewer, or when the
     owner's row toward the viewer is non-zero AND the owner's PlayerInfo has ShareRadar (+0x97 &
     0x40, 0x4674BA..0x4674D9): here, when S shares the owner with the viewer instead (both rows,
     ShareRadar not read: the module note's decisions 1 and 5). The watcher test after it,
     0x4674DB..0x4674EB, is the engine's;
   - loop 2 (0x46750E..0x4675CC) walks each radar and sonar unit of the viewer's own block
     [+0x67, +0x6B] through the bucket walk 0x47E890 with the visitor 0x467840: here, of the
     viewer's block and then of every block S shares with it. The visitor only sets bits 8 and 9
     and skips the viewed player's own units (0x467865..0x467871), so the blocks' order does not
     change what it writes;
   - loop 3 (0x4675EC..0x46766C) walks every jammer of an owner other than the viewer, and its
     visitors 0x467960 and 0x467980 clear bit 8 or 9 and set bit 10 of every unit in range with no
     owner test: here, an owner S shares with the viewer is skipped as the viewer's own is. It
     still runs after the whole of loop 2.
   AI seats target by the viewed record's bits (0x40AB3F..0x40AB5C, the module note's decision 10),
   which now hold the allies' contacts.
   ON A PUPPET the pass runs as it does live: in the recording's viewed record contacts_put
   overwrites bits 8-10 from the image after it, in any other view the pass's bits stand, and no
   simulation reads them on a puppet host (tagpu_replay.c, THE RECORDED VIEW'S CONTACTS). */

static int s_radarOn;
SIM_VAR(s_radarOn, SIM_INSTALL);

/* Loop 2's block list, written at the loop's head by radar_blocks and read by its stub until the
   loop ends; one pass at a time: the players phase runs it on the game thread in play and once on
   the loader during a load (0x497C10), before the game thread's first step (bit 1 of
   main+0x38D75, set 0x497C5F, polled 0x498342), and the replay calls it on the game thread between
   steps. */
static struct {
    char* viewer;                                 /* the record the pass runs for              */
    char* rec[PLAYERS];                           /* the records whose blocks loop 2 walks      */
    unsigned n, next;
} s_rp;
SIM_VAR(s_rp, SIM_TRANSIENT);

int tagpu_allysight_contacts(unsigned owner, unsigned viewer)
{
    return s_radarOn && tagpu_allysight_shares(owner, viewer);
}

/* Loops 1 and 3: whether S shares the owner byte (unit +0xFF) with the record the pass runs for.
   The record is the engine's own pointer, turned into an index exactly or refused; the owner byte
   is bounded inside tagpu_allysight_shares. */
static int __cdecl radar_shares(unsigned owner, char* viewer)
{
    return tagpu_allysight_shares(owner, rec_index(ta_main(), viewer));
}

/* Loop 2's head: the viewer's own block first, as the engine walks it, then the block of every
   owner S shares with the viewer, each bounded by unit_block. A viewer that is not one of records
   0..9 walks its own block alone, as stock. At most PLAYERS entries: tagpu_allysight_shares
   refuses the viewer itself. */
static void __cdecl radar_blocks(char* viewer)
{
    char* ta = ta_main();
    unsigned v = rec_index(ta, viewer), o;
    s_rp.viewer = viewer;
    s_rp.rec[0] = viewer;
    s_rp.n = 1;
    s_rp.next = 0;
    if (v >= PLAYERS) return;
    for (o = 0; o < PLAYERS; o++) {
        char *r, *first, *last;
        if (!tagpu_allysight_shares(o, v)) continue;
        r = prec(ta, o);
        if (unit_block(ta, r, &first, &last)) s_rp.rec[s_rp.n++] = r;
    }
}

#define VA_RP_SHARE    0x004674BAu
#define VA_RP_BLOCKS   0x0046750Eu
#define VA_RP_EXIT     0x004675D2u
#define VA_RP_JAM      0x004675F5u
#define VA_MM_DOT      0x00466E6Fu

/* mov ecx,[eax-0x7A]; xor ebx,ebx; mov bl,[ebp+0x146]; cmp byte [ebx+ecx+0x108],0; je 0x4674DB;
   mov ecx,[ecx+0x27]; test byte [ecx+0x97],0x40; jne 0x4674F4: loop 1's share test, eax the
   unit's +0x110, dl the viewer's index. After either exit eax, edx, edi and ebp are live (the
   loop's store at 0x4674F9, the next unit's owner test at 0x4674B4, loop 2's 0x4675D2); ebx and
   ecx are rewritten (the test's own `xor ebx,ebx`; 0x4674E1, 0x4674ED, 0x4674F4) */
static const unsigned char RP_SHARE_WAS[33] = {
    0x8B, 0x48, 0x86, 0x33, 0xDB, 0x8A, 0x9D, 0x46, 0x01, 0x00, 0x00, 0x80, 0xBC, 0x0B, 0x08, 0x01,
    0x00, 0x00, 0x00, 0x74, 0x0C, 0x8B, 0x49, 0x27, 0xF6, 0x81, 0x97, 0x00, 0x00, 0x00, 0x40, 0x75,
    0x19 };
/* mov esi,[ebp+0x67]; mov eax,[ebp+0x6B]; cmp esi,eax; ja 0x4675D2: loop 2's head over the
   viewer's block. eax, ecx and edx are dead here (0x46751C..0x467544 load what the body uses) */
static const unsigned char RP_BLOCKS_WAS[14] = {
    0x8B, 0x75, 0x67, 0x8B, 0x45, 0x6B, 0x3B, 0xF0, 0x0F, 0x87, 0xB6, 0x00, 0x00, 0x00 };
/* cmp edi,[esp+0x10]; ja 0x46782F: loop 2's exit, which only its back edge falls into and only
   0x467516, inside the head's site, jumps to */
static const unsigned char RP_EXIT_WAS[10] = {
    0x3B, 0x7C, 0x24, 0x10, 0x0F, 0x87, 0x53, 0x02, 0x00, 0x00 };
/* mov al,[esi+0x6D]; mov cl,[ebp+0x146]; cmp al,cl; je 0x46765A: loop 3's skip of the viewer's
   own jammer, esi the unit's +0x92. eax, ecx and edx are rewritten on both exits (0x467608,
   0x46761B, 0x46761F; 0x46765A, 0x467664) */
static const unsigned char RP_JAM_WAS[13] = {
    0x8A, 0x46, 0x6D, 0x8A, 0x8D, 0x46, 0x01, 0x00, 0x00, 0x3A, 0xC1, 0x74, 0x58 };
/* mov dl,[ebx+0xFF]; mov al,[esi+0x2A43]; cmp dl,al; jne 0x46718C: the minimap's unit dots
   (0x466DC0's unit loop, ebx the unit, esi main): with bits 8 and 9 clear, a dot only for the
   viewed player's own unit. eax, ecx and edx are rewritten after 0x466E83 (0x466E83, 0x466E87,
   0x466E91) and at 0x46718C */
static const unsigned char MM_DOT_WAS[20] = {
    0x8A, 0x93, 0xFF, 0x00, 0x00, 0x00, 0x8A, 0x86, 0x43, 0x2A, 0x00, 0x00, 0x3A, 0xD0, 0x0F, 0x85,
    0x09, 0x03, 0x00, 0x00 };

static unsigned char* emit_rel(unsigned char* p, unsigned char op, unsigned target)
{
    *p++ = op;
    tagpu_detour_rel(p, target);
    return p + 4;
}

static unsigned char* emit_jcc(unsigned char* p, unsigned char cc, unsigned target)
{
    *p++ = 0x0F;
    *p++ = cc;
    tagpu_detour_rel(p, target);
    return p + 4;
}

static unsigned char* emit_abs(unsigned char* p, const void* a)
{
    unsigned v = (unsigned)(size_t)a;
    memcpy(p, &v, 4);
    return p + 4;
}

/* push eax; push edx; push ebp; movzx ecx,byte [eax-0x11]; push ecx; call radar_shares;
   add esp,8; test eax,eax; pop edx; pop eax; jnz 0x4674F4 (bits 8-9); jmp 0x4674DB (the
   watcher test) */
static unsigned char* stub_rp_share(void)
{
    unsigned char *s = tagpu_detour_stub(), *p = s;
    if (!s) return NULL;
    *p++ = 0x50; *p++ = 0x52; *p++ = 0x55;
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x48; *p++ = 0xEF;
    *p++ = 0x51;
    p = emit_rel(p, 0xE8, (unsigned)(size_t)radar_shares);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x85; *p++ = 0xC0;
    *p++ = 0x5A; *p++ = 0x58;
    p = emit_jcc(p, 0x85, 0x004674F4u);
    emit_rel(p, 0xE9, 0x004674DBu);
    return s;
}

/* push ebp; call radar_blocks; add esp,4
   .next: mov eax,[s_rp.next]; cmp eax,[s_rp.n]; jae .done; inc dword [s_rp.next];
          mov ebp,[s_rp.rec+eax*4]; mov esi,[ebp+0x67]; mov eax,[ebp+0x6B]; cmp esi,eax; ja .next;
          jmp 0x46751C (the body; its back edge reads [ebp+0x6B], 0x4675C1)
   .done: mov ebp,[s_rp.viewer] (loop 3 reads [ebp+0x146]); cmp edi,[esp+0x10]; ja 0x46782F;
          jmp 0x4675DC
   The body's stack slots ([esp+0x14], [esp+0x20..0x34]) are untouched: nothing stays pushed
   across them. `next` is returned through *nextAt for the exit's site. */
static unsigned char* stub_rp_blocks(unsigned char** nextAt)
{
    unsigned char *s = tagpu_detour_stub(), *p = s, *next, *jdone, *jnext;
    if (!s) return NULL;
    *p++ = 0x55;
    p = emit_rel(p, 0xE8, (unsigned)(size_t)radar_blocks);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
    next = p;
    *p++ = 0xA1; p = emit_abs(p, &s_rp.next);
    *p++ = 0x3B; *p++ = 0x05; p = emit_abs(p, &s_rp.n);
    *p++ = 0x73; jdone = p++;
    *p++ = 0xFF; *p++ = 0x05; p = emit_abs(p, &s_rp.next);
    *p++ = 0x8B; *p++ = 0x2C; *p++ = 0x85; p = emit_abs(p, s_rp.rec);
    *p++ = 0x8B; *p++ = 0x75; *p++ = 0x67;
    *p++ = 0x8B; *p++ = 0x45; *p++ = 0x6B;
    *p++ = 0x3B; *p++ = 0xF0;
    *p++ = 0x77; jnext = p++;
    *jnext = (unsigned char)(next - p);
    p = emit_rel(p, 0xE9, 0x0046751Cu);
    *jdone = (unsigned char)(p - (jdone + 1));
    *p++ = 0x8B; *p++ = 0x2D; p = emit_abs(p, &s_rp.viewer);
    *p++ = 0x3B; *p++ = 0x7C; *p++ = 0x24; *p++ = 0x10;
    p = emit_jcc(p, 0x87, 0x0046782Fu);
    emit_rel(p, 0xE9, 0x004675DCu);
    *nextAt = next;
    return s;
}

/* movzx eax,byte [esi+0x6D]; movzx ecx,byte [ebp+0x146]; cmp eax,ecx; je 0x46765A (the viewer's
   own, as stock); push ebp; push eax; call radar_shares; add esp,8; test eax,eax; jnz 0x46765A;
   jmp 0x467602. ebx and edi hold the visitors' tables (0x4675E2, 0x4675E7) and are kept by the
   call */
static unsigned char* stub_rp_jam(void)
{
    unsigned char *s = tagpu_detour_stub(), *p = s;
    if (!s) return NULL;
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x46; *p++ = 0x6D;
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x8D; *p++ = 0x46; *p++ = 0x01; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x3B; *p++ = 0xC1;
    p = emit_jcc(p, 0x84, 0x0046765Au);
    *p++ = 0x55; *p++ = 0x50;
    p = emit_rel(p, 0xE8, (unsigned)(size_t)radar_shares);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x85; *p++ = 0xC0;
    p = emit_jcc(p, 0x85, 0x0046765Au);
    emit_rel(p, 0xE9, 0x00467602u);
    return s;
}

/* movzx edx,byte [ebx+0xFF]; movzx eax,byte [esi+0x2A43]; cmp edx,eax; je 0x466E83 (the dot);
   push eax; push edx; call tagpu_allysight_shares; add esp,8; test eax,eax; jnz 0x466E83;
   jmp 0x46718C (the next unit) */
static unsigned char* stub_mm_dot(void)
{
    unsigned char *s = tagpu_detour_stub(), *p = s;
    if (!s) return NULL;
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x93; *p++ = 0xFF; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x86; *p++ = 0x43; *p++ = 0x2A; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x3B; *p++ = 0xD0;
    p = emit_jcc(p, 0x84, 0x00466E83u);
    *p++ = 0x50; *p++ = 0x52;
    p = emit_rel(p, 0xE8, (unsigned)(size_t)tagpu_allysight_shares);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x85; *p++ = 0xC0;
    p = emit_jcc(p, 0x85, 0x00466E83u);
    emit_rel(p, 0xE9, 0x0046718Cu);
    return s;
}

/* The radar pass's four sites and the minimap's, byte-matched, all or none. No branch lands
   inside a replaced range but at its first byte (DISASSEMBLED: 0x467497 -> 0x46750E, 0x467516 ->
   0x4675D2, both inside or at the first byte; 0x4674CD -> 0x4674DB, the share test's own exit,
   past its range; 0x466E62 and 0x466E6D -> 0x466E83, past the dot's), and the radar pass has one
   caller, 0x46556D. */
static void radar_init(void)
{
    Site site[5] = {
        { VA_RP_SHARE,  RP_SHARE_WAS,  0, 33, 0xE9, NULL },
        { VA_RP_BLOCKS, RP_BLOCKS_WAS, 0, 14, 0xE9, NULL },
        { VA_RP_EXIT,   RP_EXIT_WAS,   0, 10, 0xE9, NULL },
        { VA_RP_JAM,    RP_JAM_WAS,    0, 13, 0xE9, NULL },
        { VA_MM_DOT,    MM_DOT_WAS,    0, 20, 0xE9, NULL },
    };
    const unsigned nsite = sizeof site / sizeof site[0];
    unsigned char* stub[4];
    unsigned char* next = NULL;
    unsigned i, k;

    if ((i = sites_unmatched(site, nsite)) < nsite) {
        tagpu_logf("allysight: radar NOT installed: 0x%06X does not hold the retail bytes; allies "
                   "share sight but not contacts, and allied jammers jam", site[i].va);
        return;
    }
    stub[0] = stub_rp_share();
    stub[1] = stub_rp_blocks(&next);
    stub[2] = stub_rp_jam();
    stub[3] = stub_mm_dot();
    for (k = 0; k < 4; k++)
        if (!stub[k]) {
            for (k = 0; k < 4; k++) tagpu_detour_stub_free(stub[k]);
            tagpu_log("allysight: radar NOT installed: no stub memory");
            return;
        }
    site[0].to = stub[0];
    site[1].to = stub[1];
    site[2].to = next;
    site[3].to = stub[2];
    site[4].to = stub[3];
    if ((i = sites_land(site, nsite)) < nsite) {
        for (k = 0; k < 4; k++) tagpu_detour_stub_free(stub[k]);
        tagpu_logf("allysight: radar NOT installed: 0x%06X could not be written (the earlier sites "
                   "put back)", site[i].va);
        return;
    }
    s_radarOn = 1;
    tagpu_log("allysight: radar installed: the radar pass's share test 0x4674BA, its emitter "
              "loop's head 0x46750E and exit 0x4675D2, its jammer test 0x4675F5 and the minimap's "
              "dot test 0x466E6F");
}

SIM_FEATURE(ally_visible, SIM_PUPPET_RUNS, "battle-allied");

/* UnitInPlayerLOS 0x465AC0(player, unit), stdcall `ret 8` (DISASSEMBLED 0x465AC0..0x465E1E):
   1 for the player's own unit (0x465ACE..0x465AD9); else 0 for a cloaked unit (+0x10E bit 2,
   0x465AE8) and for one under the sea (+0x110 bit 9 clear and its top below the sea level,
   0x465B30..0x465B59); else whether one of four points of its box is in the player's sight
   (0x465B5C..). Here an owner S shares with the player passes as the player's own does, so an
   allied cloaked or submerged unit stays visible (the module note's decision 8).
   IT DOES NOT CHANGE TARGETING, by construction of los_ally: the acquisition calls 0x465AC0
   (0x40AB11) only when its seat's row byte toward the owner's map bit is zero (`cmp byte
   [eax+ebp+0x108],0; jne 0x40AB6A` at 0x40AB05, eax the seat's record and the call's player, ebp
   the owner's +0x146), and los_ally passes only when that same live byte is non-zero. S alone
   would not do: S is recomputed at the players phase (0x49555F), the rows have writers earlier in
   the step (0x452A2D, 0x452ADB, 0x447225, 0x447F7E) and the unit tick 0x48AD30 runs before the
   phase (0x4954ED), so for one tick after an alliance breaks S still names the former ally. The
   other callers draw or describe: the order sprites 0x439761, the unit panel 0x46AF3D and
   0x46B5B4, COB's EMIT_SFX test for the viewed player 0x480EE5, HotUnits 0x48BC56, 0x494396, and
   ours (the frame packet, the order markers, the rings, the icons). */

static int s_visOn;
SIM_VAR(s_visOn, SIM_INSTALL);

#define VA_LOS_OWNER   0x00465ACEu

/* mov eax,[ebx+0x96]; cmp eax,esi; push edi; jne 0x465AE8: the owner record against the player
   record, ebx the unit and esi the player. eax, ecx and edx are rewritten past either exit
   (0x465AD9; 0x465AFD, 0x465B09, 0x465B1B) */
static const unsigned char LOS_OWNER_WAS[11] = {
    0x8B, 0x83, 0x96, 0x00, 0x00, 0x00, 0x3B, 0xC6, 0x57, 0x75, 0x0F };

/* Whether S shares the unit's owner record with the player record, and both live row bytes say
   allied now: the player's toward the owner's map bit, the byte 0x40AB05 tests, and the owner's
   toward the player's. Both records are the engine's pointers turned into indices exactly or
   refused, and each map bit is bounded below PLAYERS before it indexes a row. */
static int __cdecl los_ally(char* owner, char* player)
{
    char* ta = ta_main();
    unsigned o, p, obit, pbit;
    if (!ta) return 0;
    o = rec_index(ta, owner);
    p = rec_index(ta, player);
    if (!tagpu_allysight_shares(o, p)) return 0;
    obit = (unsigned char)owner[PL_MAPBIT];
    pbit = (unsigned char)player[PL_MAPBIT];
    return obit < PLAYERS && pbit < PLAYERS && player[PL_ALLY + obit] && owner[PL_ALLY + pbit];
}

/* push edi (the stock push, before either exit); mov eax,[ebx+0x96]; cmp eax,esi; je 0x465AD9;
   push esi; push eax; call los_ally; add esp,8; test eax,eax; jnz 0x465AD9; jmp 0x465AE8 */
static unsigned char* stub_los(void)
{
    unsigned char *s = tagpu_detour_stub(), *p = s;
    if (!s) return NULL;
    *p++ = 0x57;
    *p++ = 0x8B; *p++ = 0x83; *p++ = 0x96; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x3B; *p++ = 0xC6;
    p = emit_jcc(p, 0x84, 0x00465AD9u);
    *p++ = 0x56; *p++ = 0x50;
    p = emit_rel(p, 0xE8, (unsigned)(size_t)los_ally);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x85; *p++ = 0xC0;
    p = emit_jcc(p, 0x85, 0x00465AD9u);
    emit_rel(p, 0xE9, 0x00465AE8u);
    return s;
}

/* The owner test's site, byte-matched. Only 0x465AD7, inside the range, branches into
   0x465ACE..0x465AE8 (DISASSEMBLED); the line-of-sight shear's sites (tagpu_patches.c) lie past
   0x465B5C. */
static void visible_init(void)
{
    Site site[1] = { { VA_LOS_OWNER, LOS_OWNER_WAS, 0, 11, 0xE9, NULL } };
    unsigned char* stub;
    if (sites_unmatched(site, 1) < 1) {
        tagpu_log("allysight: visibility NOT installed: 0x465ACE does not hold the retail bytes; "
                  "allied cloaked and submerged units stay hidden");
        return;
    }
    if (!(stub = stub_los())) {
        tagpu_log("allysight: visibility NOT installed: no stub memory");
        return;
    }
    site[0].to = stub;
    if (sites_land(site, 1) < 1) {
        tagpu_detour_stub_free(stub);
        tagpu_log("allysight: visibility NOT installed: 0x465ACE could not be written");
        return;
    }
    s_visOn = 1;
    tagpu_log("allysight: visibility installed: UnitInPlayerLOS's owner test 0x465ACE");
}
