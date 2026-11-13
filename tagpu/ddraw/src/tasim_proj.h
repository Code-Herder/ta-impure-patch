#ifndef TASIM_PROJ_H
#define TASIM_PROJ_H
/* tasim_proj.h -- the engine's projectile as the shared simulation core holds it: the 0x6B-byte record, the pool
   with its append and its compaction. research/notes/engine/weapons.md (*The projectile record*) holds the field map
   and the engine facts each function cites; research/notes/impai/shared-core.md names the core's modules.

   THE RECORD IS THE ENGINE'S BYTES. The engine reads fields of a reused record that no path wrote: an append clears
   only +0x69 bit 1 and +0x4E (0x49B82A, 0x49B830; the same at all ten append sites), so a dropped shot keeps the
   previous tenant's death time, a ballistic burst copy divides the previous tenant's distance by its speed, and a
   new shot inherits the last-feature cell +0x5A/+0x5C that only the collision writes. Held as bytes, with the
   engine's append and compaction, every such read is exact with no special case. Fields are read and written
   through the accessors at the engine's offsets, never through a C struct of named fields.

   THE FOUR POINTER FIELDS HOLD IDS in the same four bytes: +0x00 the weapon's ID, +0x4E and +0x52 a unit slot (0 for
   none), +0x56 a pool index + 1 (0 for none). Every reader bounds the ID against the count it indexes before use;
   an out-of-range value is data, never an address.

   ENGINE-FREE, NO ALLOCATION. The records are the caller's array, sized once to the engine's own capacity
   (TAGPU_LIM_PROJ: 300 stock, 3000 under the raised limits), which every append site refuses past (weapons.md,
   *The raised effect pools*); no index the pool hands out reaches past it. */
#include <stdint.h>
#include <string.h>

#define TS_PROJ_SIZE 0x6B

typedef struct TS_PROJ { unsigned char b[TS_PROJ_SIZE]; } TS_PROJ;

/* The record's fields, DISASSEMBLED from their writers: the init 0x49C740, the shots 0x49C9C0, 0x49CC20 and
   0x49CDE0, the tick 0x49B720, the collision 0x49B090 and the compaction 0x49AE20. */
enum {
    TS_PJ_WEAPON  = 0x00,   /* u32: the weapon (the engine holds the record's address; the core its ID)          */
    TS_PJ_POS     = 0x04,   /* 3 x i32 16.16: the position x, y, z                                                */
    TS_PJ_TAIL    = 0x10,   /* 3 x i32: the start; a beam's tail; guidance's cruise waypoint                      */
    TS_PJ_VEL     = 0x1C,   /* 3 x i32: the velocity per tick                                                     */
    TS_PJ_TPOS    = 0x28,   /* 3 x i32: the target point (the ballistic shot writes none)                         */
    TS_PJ_SPIN    = 0x34,   /* u16: a meteor's tumble                                                             */
    TS_PJ_HEADING = 0x36,   /* u16                                                                                */
    TS_PJ_PITCH   = 0x38,   /* u16                                                                                */
    TS_PJ_SPEED   = 0x3A,   /* i32: a self-propelled shot's speed (the ballistic shot writes none)                */
    TS_PJ_DIST    = 0x3E,   /* i32: the line shot's horizontal distance to its target                             */
    TS_PJ_TIME    = 0x42,   /* u32: the GameTime of the shot, or of a burst's last copy                           */
    TS_PJ_DEATH   = 0x46,   /* u32: the GameTime it expires (the dropped shot writes none)                        */
    TS_PJ_SMOKE   = 0x4A,   /* u32: the next smoke trail tick                                                     */
    TS_PJ_TARGET  = 0x4E,   /* u32: the target unit's slot, 0 for none                                            */
    TS_PJ_SHOOTER = 0x52,   /* u32: the attacker's slot, 0 for none                                               */
    TS_PJ_QUARRY  = 0x56,   /* u32: the quarry projectile's pool index + 1, 0 for none                            */
    TS_PJ_FCELLX  = 0x5A,   /* u16: the cell of the last feature hit; only the collision writes it               */
    TS_PJ_FCELLZ  = 0x5C,   /* u16                                                                                */
    TS_PJ_GROUND  = 0x5E,   /* u16: the 2x2 mean height under it, read by the draw only                          */
    TS_PJ_BURST   = 0x60,   /* u16: a burst parent's copies still to make; 0 for a flying record                  */
    TS_PJ_PIECE   = 0x62,   /* u16: the firing piece, the shooter's Query script's answer at the shot              */
    TS_PJ_PROP    = 0x64,   /* u16: a propeller's spin                                                            */
    TS_PJ_OWNER   = 0x66,   /* u8: the owner's player index, 10 for none                                          */
    TS_PJ_STAMP   = 0x67,   /* u16: the record's index before the last compaction                                 */
    TS_PJ_FLAGS   = 0x69    /* u8: bit 0 a beam's tail moves, bit 1 done, bits 4-5 a two-phase weapon's phase     */
};

#define TS_PJF_TAIL   0x01u
#define TS_PJF_DONE   0x02u
#define TS_PJF_PHASE  0x30u

static inline uint32_t ts_pj_u32(const TS_PROJ* p, unsigned off) { uint32_t v; memcpy(&v, p->b + off, 4); return v; }
static inline int32_t  ts_pj_i32(const TS_PROJ* p, unsigned off) { int32_t v; memcpy(&v, p->b + off, 4); return v; }
static inline uint16_t ts_pj_u16(const TS_PROJ* p, unsigned off) { uint16_t v; memcpy(&v, p->b + off, 2); return v; }
static inline int16_t  ts_pj_i16(const TS_PROJ* p, unsigned off) { int16_t v; memcpy(&v, p->b + off, 2); return v; }
static inline uint8_t  ts_pj_u8(const TS_PROJ* p, unsigned off) { return p->b[off]; }
static inline void ts_pj_set_u32(TS_PROJ* p, unsigned off, uint32_t v) { memcpy(p->b + off, &v, 4); }
static inline void ts_pj_set_i32(TS_PROJ* p, unsigned off, int32_t v) { memcpy(p->b + off, &v, 4); }
static inline void ts_pj_set_u16(TS_PROJ* p, unsigned off, uint16_t v) { memcpy(p->b + off, &v, 2); }
static inline void ts_pj_set_u8(TS_PROJ* p, unsigned off, uint8_t v) { p->b[off] = v; }

/* THE POOL: the engine's base main+0x141F7 and count main+0x141F3, stride 0x6B. `hw` is the high-water count since
   the level load: records at or past it are still the load's zeros, so a hash or a fork covers [0, hw). */
typedef struct TS_PROJPOOL {
    TS_PROJ* rec;
    int32_t  cap;
    int32_t  count;
    int32_t  hw;
} TS_PROJPOOL;

/* The largest capacity the core takes: the compaction's stamp is a u16 and its fix-up table's new indices are read
   back with movsx (0x49AF35), so the engine's own arithmetic holds below 0x8000. */
#define TS_PROJ_CAP_MAX 0x7FFF

/* The level load's allocation 0x499A30 (DISASSEMBLED 0x499A30..0x499A6A): every record zeroed (rep stos, 80 250
   dwords under the raised limits) and the count 0. It runs at every level load, a saved game's included, and no save
   carries projectiles. 0 on success; -1 when cap is outside 1..TS_PROJ_CAP_MAX. */
int ts_proj_pool_init(TS_PROJPOOL* pool, TS_PROJ* rec, int32_t cap);

/* The append at all ten sites (0x49B809..0x49B830 the burst's, 0x49C9E1..0x49C9F9 the line shot's): NULL when the
   count has reached the capacity, else the record at the count, the count incremented, the word at +0x69 ANDed with
   0xFFFD (the done bit) and +0x4E zeroed. Nothing else: every other byte is the previous tenant's. */
TS_PROJ* ts_proj_append(TS_PROJPOOL* pool);

/* Compaction 0x49AE20, DISASSEMBLED 0x49AE20..0x49AF83, called at the projectile tick's end (0x49BE53) and inside
   the destructor's purge (0x49C8F4). Every record of [0, count), done or not, has its index stamped at +0x67
   (0x49AE5F); each survivor past the first done record is copied whole to the next free place in ascending order
   (0x49AEE0), so the order is stable and [n, old count) keeps its bytes. Then the quarry fix-up (0x49AF12..0x49AF4B):
   a survivor that MOVED and has a quarry gets +0x56 re-pointed at its quarry's new place when the quarry survived.
   Not re-aimed: a survivor before the first done record (it never registers, even if its quarry moved), and any link
   to a removed quarry (it keeps the old index: whatever record now sits there, or the stale tail).
   The engine registers each mover's new index and its quarry's old index in two stack tables and scans the
   survivors' stamps for each; the core finds the same place by a binary search of the stamps, which are the
   survivors' old indices in ascending order after the move. Each mover's quarry is read before it is rewritten and
   no rewrite touches a stamp, so the order of the fix-ups changes nothing. */
void ts_proj_compact(TS_PROJPOOL* pool);

/* ---- the shot's arithmetic --------------------------------------------------------------------------------- */

#define TS_PITCH_NONE 0x8000u   /* the solver's answer when no pitch reaches */

/* The ballistic pitch solver 0x49A890(dx, dy, dz, v, minbarrel) (tasim_ballistic.c), DISASSEMBLED 0x49A890..0x49AA75,
   stdcall, ret 0x14: the launch pitch in the engine's angle units for a shell of speed v (16.16 a tick) to cross the
   16.16 offset (dx, dy, dz) under gravity g (main+0x14263, read by the engine itself, an argument here), or
   TS_PITCH_NONE. Callers: can-aim 0x49AA80, can-engage 0x49ABB0, the turret fire 0x49D580 and AutoAim 0x49E1A0.
   Every value the engine computes is reproduced bit for bit but one: RESIDUAL R4, the CRT acos 0x4E67F0's fpatan,
   which the core computes with ts_atan2 (tasim_ballistic.c). MEASURED: tests/tasim_combat_test.c x87 runs the
   engine's instruction sequence on the x87 and counts R4's differences. */
uint16_t ts_ballistic_pitch(int32_t dx, int32_t dy, int32_t dz, int32_t v, float minbarrel, int32_t g);

#endif
