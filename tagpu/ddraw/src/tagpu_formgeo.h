#ifndef TAGPU_FORMGEO_H
#define TAGPU_FORMGEO_H
/* tagpu_formgeo.c -- the geometry of a formation move (tagpu_formation.c): the dragged path, its
   length, the slots spaced evenly along it, and which unit takes which slot.

   Engine-free and Win32-free, so tests/formtest.c builds it natively. INTEGER ONLY, because
   tools/formation_regression.py reproduces every result bit for bit to predict the orders a
   drag makes in game; no floating point, no clock, no randomness.

   Points are 16.16 world coordinates, x and z in [0, 32767] wu (every TA position is a 16.16
   int32). So a delta is below 2^31 in magnitude, its square below 2^62, and the sum of two
   squares below 2^63: int64 holds every product below. */
#include <stdint.h>

#define FORM_MAX_UNITS      1500   /* >= TAGPU_LIM_UNITS in every build (tagpu_formation.c asserts it) */
#define FORM_HUNGARIAN_MAX   256   /* at most this many units: the optimal assignment */
#define FORM_MAX_NODES      4096   /* a full path is decimated to half before the next node */
#define FORM_UNCROSS_PASSES    4
#define FORM_UNCROSS_WORK  2000000u /* segment tests, every pass together */
#define FORM_COST_MAX     0x7FFF   /* a cost in whole wu, capped: |potential| <= n*FORM_COST_MAX < 2^23 */

typedef struct { int32_t x, alt, z; } FormPt;   /* 16.16 world */

/* floor(sqrt(v)), exact for every uint64 */
uint64_t form_isqrt64(uint64_t v);

/* Appends `p` to the path of `m` nodes and returns the new count. A point at the last node's x
   and z is skipped (BAR's duplicate). A full path (FORM_MAX_NODES) keeps its even nodes and its
   last one first, so the first and the last node always survive. `m` may be 0. */
unsigned form_path_add(FormPt* node, unsigned m, FormPt p);

/* The path's length, 16.16: the sum of each segment's floor(sqrt(dx^2 + dz^2)). Below 2^44. */
int64_t  form_path_length(const FormPt* node, unsigned m);

/* `n` slots evenly by arc length, both ends included: slot k at t_k = L*k/(n-1), linear between
   the two nodes it falls between (x, alt and z alike). n >= 2, m >= 1; m == 1 gives node[0]
   for every slot. */
void     form_slots(const FormPt* node, unsigned m, unsigned n, FormPt* out);

/* The rounded distance in whole wu, capped at FORM_COST_MAX: BAR's floor(sqrt + 0.5). */
int32_t  form_cost(FormPt a, FormPt b);

/* slot_of[i] = the slot unit i takes; always a permutation of 0..n-1.
   form_hungarian: the optimal (minimum total form_cost) assignment, n <= FORM_HUNGARIAN_MAX,
   ties to the lowest slot. form_nox: units and slots sorted along the direction of the furthest
   pair and paired in order, then at most FORM_UNCROSS_PASSES passes (and FORM_UNCROSS_WORK
   segment tests in all) swap the slots of two units whose paths cross properly. Only a proper
   crossing swaps, and uncrossing two segments strictly shortens their total (the triangle
   inequality), so the passes cannot cycle. form_assign picks the first up to
   FORM_HUNGARIAN_MAX units, the second above. Returns 'h' or 'x'. */
void     form_hungarian(const FormPt* start, const FormPt* slot, unsigned n, uint16_t* slot_of);
void     form_nox(const FormPt* start, const FormPt* slot, unsigned n, uint16_t* slot_of);
int      form_assign(const FormPt* start, const FormPt* slot, unsigned n, uint16_t* slot_of);

#endif
