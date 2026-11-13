#ifndef TASIM_FXGATE_H
#define TASIM_FXGATE_H
/* tasim_fxgate.h -- what the core needs to count a death's shatter exactly without simulating the effect pools: an
   explosion record's life, the quads a piece's shatter takes, and a closed bound on a shatter body's flight. Each is
   a function of its arguments alone; the calendars that carry the bounds U_e and U_d from tick to tick are the
   combat tick's. research/notes/engine/weapons.md (*The shatter and the effect pools' bounds*) holds the engine
   facts; research/notes/impai/shared-core.md names the core's modules.

   WHY BOUNDS. A shatter makes 6 + 8k draws, k its accepted quads, and the engine refuses a quad once the explosion
   pool is full or no debris record is free; those pools' occupancy depends on the frame's posed pieces, which the core
   does not have. So the core counts an upper bound on each pool: every add when the engine makes it, and a record's
   leave only at a tick by which it is certainly gone. While the bounds prove both pools open, k = q(piece) exactly. */
#include <stdint.h>

/* ---- an explosion record's life ------------------------------------------------------------------------------ */

/* A non-looping anim state's life in explosion steps, DISASSEMBLED 0x4B8B30 (the init: frame 0, the countdown its
   duration) and 0x4B8B90 (a step: a countdown of 2 or more counts down; else the next frame, and past the last the
   sequence ends): L = sum of max(d_i, 1) over the frames' u16 durations (the GAF frame entry's +4, which the loader
   0x4B8C60 leaves as the file has it). Every sequence an explosion plays has its loop byte forced to 0 at load
   (0x42EDFF, 0x42EEC5, 0x4298F1). A sequence of no frame reads the duration past its table, which the core cannot
   know: it answers the largest such life, 65 535, so the bound stays sound. */
uint32_t ts_fx_seq_life(const uint16_t* dur, int32_t n);

/* Table B's life at index idx, built at the level load by 0x420620 (DISASSEMBLED 0x4206BA..0x4207C4): idx 0 is 12
   frames, idx 1 and 2 are 15, every duration 2, so 24, 30, 30; no state for idx < 0 (0x420AB7). */
uint32_t ts_fx_table_b_life(int32_t idx);

/* The ticks whose cap tests count an explosion record added at tick t, from t on: max(L_A, L_B), and at least the
   add's own tick. DISASSEMBLED 0x420A30 (the add: state A from its sequence, B from table B), 0x420F30 (every explosion
   tick steps each record, those appended during the walk included, and its compaction drops one whose body, A and B
   are all NULL), the step's order 0x495490 (units, projectiles, explosions): a record added in tick t takes its first
   step in t's explosion tick and leaves at the end of the explosion tick of t + L - 1. */
uint32_t ts_fx_life(uint32_t life_a, int32_t idx);

/* ---- the shatter's quads ------------------------------------------------------------------------------------- */

/* One 3DO primitive as the shatter reads it: its vertex count, the file's last primitive dword (Unknown_3, the
   in-memory face's +0x1C), and whether it names a texture that no texture bank GAF nor logos.GAF holds. */
typedef struct TS_PRIM {
    int32_t  nverts;
    uint32_t unknown3;
    int      texture_missing;
} TS_PRIM;

/* q(node): the quads a shatter of this node accepts while both pools are open. DISASSEMBLED, the shatter 0x421700's
   test 0x421784..0x4217A1 (a quad, +0x1C bit 0 clear, not the node's selection index) over the load's reshaping:
   0x4CB370 swaps the face at SelectionPrimitive `sel` with face 0 and stores 0 as the node's index when sel != -1 and
   the node has faces, and the texture binder 0x42A140 ORs +0x1C bit 0 into a face whose named texture is in neither
   bank (0x42A204..0x42A20F). So q counts the faces of four vertices, not the selection face, with neither Unknown_3
   bit 0 nor a missing texture; the order the load sorts them in does not change the count. MEASURED on 608 retail
   models (50 443 faces): bit 0 is set on every untextured face and on no textured one, and no texture is missing.
   A `sel` outside -1..n-1 makes the engine swap with a face past the node's table: -1, content refused. */
int32_t ts_shatter_quads(const TS_PRIM* prims, int32_t n, int32_t sel);

/* ---- a shatter body's flight -------------------------------------------------------------------------------- */

#define TS_SHATTER_SATURATED 0xFFFFFFFFu   /* no bound: the calendar holds the body past its cap */
#define TS_SHATTER_ARCS      64            /* the arcs the bound follows before it saturates */

/* T_s, a bound on the explosion steps a shatter body flies, counted from the shatter's own tick: its explosion and
   debris records are gone from tick t + T_s. The flight, DISASSEMBLED 0x420F81..0x4210A6, all i32 and wrapping: y +=
   vy + iy, vy -= g; then with h the ground 0x485140 under the body (-1 off the map) and S = sea << 16, y <= S over a
   cell below the sea ends it; else y's word at or below h restores the pre-move position and vy = -(vy / 2), the
   flight going on only while vy's word stays above 0.
   The inputs are what the core knows: gravity g (main+0x14263), the sea, the map's highest ground hmax (the largest
   (c5 + c6) >> 1), the dying unit's y and R_u (the COB host's bound on a piece origin's distance from it), draw #3 of
   the quad r3 (the launch vy0 = ((80 - r3) << 9) + 30 g, 0x42187F..0x4218A7), and the inherited iy.
   PREMISES, each refused as TS_SHATTER_SATURATED: g > 0 and v_b = 0x20000 - g > g; iy = 0 (the record's +0x2C is
   written only by the shatter, from the dying unit's mover +0x0C >> 1 at 0x42182A..0x421849, and a ground mover's
   vertical velocity is always 0: MEASURED exact on 4 362 632 unit-ticks; T3 extends this before it admits aircraft);
   R_u >= 0; r3 below 160, rand(160)'s range.
   THE BOUND, with Q = 2g (Y - S) + V^2 for a state (Y, V) (y and the stored vy before a tick):
   F1  a tick without contact adds exactly g^2 to Q: Y' = Y + V, V' = V - g.
   F2  an arc from (Y_s, V_s) lasts at most N(Q_s) ticks, its contact included: V_s^2 <= Q_s + D_lo with D_lo =
       2g max(S - Y_lo, 0) and Y_lo = min(u.y - R_u, S), because every arc starts at or above Y_lo; the moved y after j
       ticks is at or below S once (g j - a)^2 >= Q + g^2 j, a = ceil(sqrt(Q + D_lo)), and a body at or below S ends or
       bounces; the larger root gives N(Q) = floor((2a + g + ceil(sqrt(4ag + g^2 + 4 max(Q, 0)))) / 2g) + 1.
   F3  a bounce that goes on needs a pre-move V <= -v_b, and lowers Q by at least f(|V|) = (3V^2 - 2g|V| - g^2) / 4:
       the new |V| is at most (|V| + g) / 2 and the position is the pre-move one.
   F4  a bounce needs the moved y below (hmax + 1) << 16, so (|V| + g)^2 > Q + g^2 - P with P = 2g ((hmax + 1 - sea)
       << 16), and |V| >= v_lo(Q) = max(v_b, floor(sqrt(max(Q - P + g^2, 0))) - g).
   F5  Q - f(v) at the real-valued lower bound of |V| has the derivative (v + 5g) / 4(v + g) > 0, so a bound on Q at an
       arc's start bounds the true Q at the next arc's; the floors only loosen it.
   Each arc adds N to T; the arc's last pre-move state has Q at most Q + g^2 (N - 1) (F1); if that leaves no |V| of v_b
   (Q + D_lo < v_b^2), no bounce goes on and T is the bound, else the next arc starts from Q - f(v_lo(Q)). Past
   TS_SHATTER_ARCS arcs, saturated.
   MEASURED 2026-11-12, tests/tasim_combat_test.c flying 200 000 bodies over random terrain, gravity 2000..20000: no
   body outlived its bound, and the flight over the bound averages 0.09; it saturates 937 of 89 227 bodies at
   g <= 10 000 and 73 936 of 110 773 above [INFERRED: an arc's g^2 (N - 1) outgrows f(v_b) as g rises]. */
uint32_t ts_shatter_flight_bound(int32_t g, uint8_t sea, uint8_t hmax, int32_t unit_y, int32_t r_u, uint32_t r3,
                                 int32_t iy);

#endif
