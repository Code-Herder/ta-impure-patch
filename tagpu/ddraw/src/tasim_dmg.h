#ifndef TASIM_DMG_H
#define TASIM_DMG_H
/* tasim_dmg.h -- the engine's damage arithmetic as the shared simulation core computes it: a weapon's damage to one
   type ([DAMAGE]'s search), the factor, the shooter's and the victim's veterancy, armour, the cheat bits, the hit's
   word as Impure's B7 saturates it, and the area's box distance and falloff. Each is a function of its arguments
   alone; the hit's application, the victims' walk and the blast flags are the combat tick's.
   research/notes/engine/weapons.md (*`0x499CD0`, the damage function*, *The hit sender*, *Area damage*) holds the
   engine facts; research/notes/impai/shared-core.md names the core's modules.

   The x87 operations here (the factor's product, the box distance, the falloff) run under the game's control word
   0x27F, which rounds every one to 53 bits as an SSE2 double operation does; each is written in the listing's order
   and tests/tasim_combat_test.c x87 holds it to the instruction sequence run on the x87. */
#include <stdint.h>

/* the weapon's +0x111 bit 7, paralyzer: the hit's kind 2 instead of 1 (0x499E20..0x499E32) */
#define TS_WF_PARALYZER (1u << 7)

/* the amount from which the hit's armour and, under B7, its veterancy are skipped: "kill outright" (0x489BD1) */
#define TS_DMG_OUTRIGHT 30000

/* The engine's _stricmp 0x4F8A70 in the C locale, DISASSEMBLED 0x4F8A70..0x4F8AC1: bytes compared until one differs
   or both end; a differing pair is folded A-Z to a-z ((c - 'A') < 26 unsigned, + 0x20) and compared unsigned; the
   sign of the first difference, -1, 0 or 1. */
int ts_stricmp(const char* a, const char* b);

/* One [DAMAGE] row: a type's name and its dword. The weapon's table (+0x64: begin +0x05, end +0x09) is in the order
   the parser's insertion leaves it (weapons.md, *the fill*): sorted by ts_stricmp, a name equal byte for byte
   overwriting, one equal only without case inserted at its lower bound -- so before the earlier one. */
typedef struct TS_DMGROW {
    const char* name;
    int32_t     damage;
} TS_DMGROW;

/* 0x499CE3..0x499D6F, DISASSEMBLED: the default, the weapon's u16 +0xD4 zero-extended; with rows, the lower bound of
   `type` (the victim's def +0x20) -- the half (end - begin) / 2 rounded toward zero, the probe ts_stricmp(row, type)
   < 0 moving past it -- then, when the bound is not the end and ts_stricmp(type, row) is not negative, that row's
   dword. So among names equal without case, the first in the table's order. */
int32_t ts_dmg_base(uint16_t dflt, const TS_DMGROW* rows, int32_t n, const char* type);

/* 0x499D73..0x499D7B: _ftol(fild(base) * fmul DWORD factor). A full-dword row times a float can need 56 bits, and the
   product rounds to 53 as a double's does. */
int32_t ts_dmg_scale(int32_t base, float factor);

/* A unit's veterancy level, min(kills / 5, 5), from its u16 +0xB8 (0x499DAE..0x499DCC, 0x489BF3..0x489C11). Impure's
   C3 replaces it for a keyed type at both sites; the callers below take the level, not the kills. */
unsigned ts_vet_level(uint16_t kills);

/* The shooter's level at 0x499DD1..0x499DEC: amount * (100 + 6 L) as a wrapping 32-bit imul, then / 100 signed,
   truncating. Applied only when the projectile has a shooter (+0x52, 0x499DAA). */
int32_t ts_dmg_shooter(int32_t amount, unsigned level);

/* The cheat bits of the word main+0x37F2F, 0x499DEE..0x499E17: bit 7 doubles (a wrapping add), then bit 8 halves
   (signed, truncating). A scenario sets neither; the core keeps both. */
int32_t ts_dmg_cheats(int32_t amount, uint16_t cheats);

/* The hit's kind from the weapon's flags: 2 for a paralyzer, else 1 (0x499E20..0x499E32). */
int ts_dmg_kind(uint32_t wflags);

/* The hit's angle, 0x499D84..0x499DA3: the word 0x4B715A(p.x - u.x, p.z - u.z) less the victim's heading +0x66. The hit
   record keeps its high byte (0x489C78). */
uint16_t ts_dmg_angle(int32_t px, int32_t pz, int32_t ux, int32_t uz, uint16_t heading);

/* The hit 0x489BB0's word, DISASSEMBLED 0x489BB0..0x489C71 with Impure's B7 (tagpu_patches.c, fix_kill_outright and
   fix_hit_word): kind 10 (a heal) passes the amount untouched; else, with the victim's +0x10E bit 1 set (armoured)
   and an amount below 30 000, the amount becomes (armour * amount) >> 16 as a 64-bit product (_allshr 0x4E43D0; the
   armour def +0x1AA is a 16.16 factor, not a divisor); then, unless the call's own amount is 30 000 or more (B7, which
   skips the veterancy as stock skips the armour), (25 - L) * amount * 4 as a wrapping 32-bit product, / 100 signed.
   The word is the amount saturated into what its reader reads (B7 at 0x489C71): 0..65535 for kinds 2 and 10, which
   are read unsigned, -32768..32767 for every other, which the apply subtracts from the HP word. */
uint16_t ts_hit_word(int32_t amount, int kind, int armoured, int32_t armour, unsigned level);

/* The area's distance from `at` to a unit's box, DISASSEMBLED 0x49A2AA..0x49A38A: per axis the gap from at to
   [u + bmin, u + bmax] (def +0x15E/+0x162/+0x166 and +0x16A/+0x16E/+0x172 added to the unit's +0x6A/+0x6E/+0x72, each
   add wrapping), 0 inside, the compares signed; then _ftol(sqrt((gx^2 + gy^2) + gz^2)) of the fild'ed gaps. The
   16.16 answer's signed high word is the distance in px the falloff and the radius test read (0x49A397). */
int32_t ts_area_distance(const int32_t at[3], const int32_t u[3], const int32_t bmin[3], const int32_t bmax[3]);

/* The area's falloff, DISASSEMBLED 0x49A3A6..0x49A3E6: 1.0f at a distance of 0 px, else
   (float)((d / R - 1)^2 (1 - e) + e) with d = dpx and R = aoe >> 1 fild'ed, 1 - e from the float 1.0 less the float
   e (+0xD8), each operation in that order: quadratic, 1 at the centre and e at the edge. The caller has already
   refused dpx >= R (0x49A39E). */
float ts_area_falloff(int32_t dpx, int32_t R, float edge);

#endif
