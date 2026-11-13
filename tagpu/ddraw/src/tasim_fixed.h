#ifndef TASIM_FIXED_H
#define TASIM_FIXED_H
/* tasim_fixed.h -- the engine's arithmetic, as the shared simulation core computes it: the turn lookups off the sine
   table, the heading's angle, the rotator, _ftol, _hypot and the 64-bit helpers the engine's 16.16 code calls.
   research/notes/impai/shared-core.md names the core's modules; research/notes/engine/sim.md, *The engine's
   arithmetic*, holds the engine facts each helper cites.

   ENGINE-FREE, WITHOUT GLOBALS AND WITHOUT THE PLATFORM'S LIBM. The C library's sin, cos and atan2 differ between
   glibc, msvcrt and mingw, and the core is built three ways (the DLL's i686, the native host and the sanitized
   one), so tasim_fixed.c carries one implementation of each, on doubles rounded once per operation (SSE2; the
   Makefile's THE WIKI'S SIMULATION). Every build then agrees bit for bit; tests/tasim_fixed_test.c and
   tools/wiki/simparity.mjs prove it. sqrt and floor are the libm calls, both exact by IEEE 754 in every library.

   THE SINE TABLE is the caller's (TS_SIN, filled by ts_sin_table), never a static: the core holds no globals, so
   a table built lazily behind a flag would be a race between two simulations on two threads. Its 512 entries are
   generated from their formula; the engine's own bytes never enter the repository. */
#include <stdint.h>

#define TS_SIN_N 512

typedef struct { int16_t v[TS_SIN_N]; } TS_SIN;

/* SIN[i] = floor(8192 sin(2 pi i / 512) + 0.5): the engine's int16 table at 0x509F00. MEASURED 2026-11-12, all 512
   entries of the retail executable's table equal the formula, and the nearest value to a .5 tie is 0.0016 away, so
   any sin within 1e-4 generates it exactly; ts_sin is within one ulp. */
void ts_sin_table(TS_SIN* t);

/* 0x4B70EF (TurnXLookup), DISASSEMBLED 0x4B70F5..0x4B711A: the angle sign-extended, + 0x20, shr 6, and 0x3FE (a byte
   offset into the int16 table, so the index is bits 7..15 of the sum), the entry times v as a signed 64-bit
   product, + 0x1000 with carry, shrd 13 for the low dword. A zero extension agrees with the movsx in bits 7..15,
   the only bits the mask keeps, so the angle is taken as uint16_t. THE BOUND: the index is masked to 511. */
static inline int32_t ts_turn_x(const TS_SIN* t, int32_t a, int32_t v)
{
    const int32_t s = t->v[(((uint16_t)a + 0x20u) >> 7) & (TS_SIN_N - 1)];
    return (int32_t)(uint32_t)(((int64_t)s * v + 0x1000) >> 13);
}

/* 0x4B7123 (TurnZLookup), DISASSEMBLED 0x4B712F..0x4B7151: TurnXLookup a quarter turn on, movzx and + 0x4020 */
static inline int32_t ts_turn_z(const TS_SIN* t, int32_t a, int32_t v)
{
    const int32_t s = t->v[(((uint16_t)a + 0x4020u) >> 7) & (TS_SIN_N - 1)];
    return (int32_t)(uint32_t)(((int64_t)s * v + 0x1000) >> 13);
}

/* _allmul 0x4E4400 then _allshr 0x4E43D0: the signed 64-bit product shifted right arithmetically by n and its low
   dword. DISASSEMBLED 0x4E43D0..0x4E43F0: a shift of 64 or more leaves the sign alone, which this keeps. */
static inline int32_t ts_mul_shr(int32_t a, int32_t b, unsigned n)
{
    const int64_t p = (int64_t)a * b;
    if (n >= 64)
        return p < 0 ? -1 : 0;
    return (int32_t)(uint32_t)(uint64_t)(p >> n);
}

/* _allshl 0x4E44F0 then _alldiv 0x4E4440: (a << n) / d in 64 bits, truncated toward zero, its low dword. n is a
   constant at every engine call site (16 for a 16.16 quotient), and the helper takes 0..31: then |a << n| < 2^62,
   so no quotient overflows. A divisor of 0 faults in the engine (_alldiv's div); the core answers 0 and counts the
   case in *zero_div, which its caller's state holds, so a definition that would fault the engine is named rather
   than played (shared-core.md, the named-not-modelled report). */
static inline int32_t ts_shl_div(int32_t a, unsigned n, int32_t d, uint32_t* zero_div)
{
    if (d == 0) {
        ++*zero_div;
        return 0;
    }
    return (int32_t)(uint32_t)(uint64_t)(((int64_t)a * ((int64_t)1 << (n & 31))) / d);
}

/* THE SIMULATION'S ONE RANDOM STREAM, 0x4B6C30(n), stdcall, DISASSEMBLED 0x4B6C30..0x4B6C92. Its state is the one
   dword 0x51FC88, which three instructions touch: the load 0x4B6C41, the step's store 0x4B6C87 and the seed's
   0x4B6CAB. n < 2, negatives included (cmp edi, 2; jge, signed), answers 0 WITHOUT stepping. Otherwise the step is
   Park and Miller's 16807 s mod (2^31 - 1) in Schrage's form, computed modulo 2^32: q = s / 127773 by a magic
   multiply (0x4B6C47..0x4B6C60; MEASURED 2026-11-12 equal to the exact quotient for all 2^32 dwords), then
   16807 s - q (2^31 - 1), which is 16807 (s mod 127773) - 2836 q (0x4B6C4C..0x4B6C7A); 0x7FFFFFFF is added to a
   result that is not above 0 (0x4B6C7C..0x4B6C80, signed), the result is stored, and the answer is the stored
   value mod n, unsigned (div edi, 0x4B6C8C). The CRT's rand 0x4E4870 is another stream, which feeds presentation
   only.
   EVERY DRAW STEPS IT, whether or not anything reads the value, so the core makes every draw the engine makes, in
   the engine's order: a draw left out shifts every value after it. The engine calls it from 129 sites
   (research/notes/engine/cob.md, *0x4B6C30*). `rng` is the stream's state, which a simulation holds as state:
   hashed and forked with it. */
static inline uint32_t ts_rand(uint32_t* rng, int32_t n)
{
    if (n < 2)
        return 0;
    const uint32_t s = *rng;
    const uint32_t q = s / 127773u;
    uint32_t v = s * 16807u - q * 0x7FFFFFFFu;
    if ((int32_t)v <= 0)
        v += 0x7FFFFFFFu;
    *rng = v;
    return v % (uint32_t)n;
}

/* 0x4B6CA0(x), DISASSEMBLED 0x4B6CA0..0x4B6CB0: the state becomes (x ^ 0x66E29572) | 1. Its one caller in the engine
   is the level load 0x497180 at 0x49719D, with x the two dwords of QueryPerformanceCounter summed, so a launch's
   stream starts anew at every level load; a headless game seeds from its setup's seed through this. */
static inline void ts_seed(uint32_t* rng, uint32_t x)
{
    *rng = (x ^ 0x66E29572u) | 1u;
}

/* _ftol 0x4E43A0, DISASSEMBLED: the control word's rounding set to chop (or ah, 0x0C), fistp QWORD, the low dword in
   eax. So truncation toward zero, wrapped to 32 bits; a value out of the int64 range or a NaN is the x87's integer
   indefinite 0x8000000000000000, whose low dword is 0. */
int32_t ts_ftol(double x);

/* fistp DWORD under the game's control word, round to nearest even: the conversion 0x4B715A and 0x4B7173 end in. A
   value out of the int32 range or a NaN is the integer indefinite 0x80000000, as the x87 stores it. */
int32_t ts_fistp(double x);

/* 0x4B715A, DISASSEMBLED 0x4B715D..0x4B716B: fild the first argument, fild the second, fpatan (atan2 of the first
   over the second), fmul the double at 0x509EF0, fistp. MEASURED 2026-11-12, that double is 0x1.45f306dc9c78ap+13,
   the decimal literal 10430.37835047 and not 32768/pi, which is 249 ulp away; the core uses the literal.
   RESIDUAL R1: fpatan's result has a 64-bit significand, rounded to 53 bits by the fmul, and ts_atan2 is within one
   ulp of the true value, so the integer can differ from the engine's only where the scaled angle lies within about
   2e-11 of a .5 tie [INFERRED from the error bound]; the exact movement families are where one would show.
   MEASURED 2026-11-12 against the instruction sequence on the reference setup's x87 (tests/tasim_fixed_test.c x87):
   0 of 10^6 random calls differ. The angle runs no CRT code, so no wrapper changes its precision. */
int32_t ts_angle(int32_t dx, int32_t dz);

/* 0x4B7173, DISASSEMBLED 0x4B7176..0x4B71A2: nothing when the int16 angle is 0; else theta = a times the double at
   0x509EF8 (MEASURED 2026-11-12: 0x1.921fb54442c5ap-14, the decimal literal 9.587379924285e-05, not pi/32768,
   which is 0x1.921fb54442d18p-14), fsincos, and with the x87 stack traced
   x' = fistp(x cos - z sin), z' = fistp(z cos + x sin), each product and sum rounded to 53 bits.
   RESIDUAL R2: fsincos against ts_sin/ts_cos at the fistp ties, the same argument as R1. MEASURED as R1 is: 0 of
   10^6 random calls differ from the x87; the rotator too is inline x87, no CRT call. */
void    ts_rotate(int16_t a, int32_t* x, int32_t* z);

/* _hypot 0x4FB440 on two fild'ed int32 values (the ground step's 0x43CD98..0x43CDA7 among its 39 call sites),
   exactly as the engine computes it. DISASSEMBLED 0x4FB480..0x4FB6A8: MSVC's scaled hypot, its body run at a 64-bit
   significand (_ctrlfp(0x133F) at 0x4FB487, round to nearest), every result stored to a double:
     M = max(|x|, |y|), ties taking x; 0 when M is 0
     x' = (double)rd64(|x| / M), y' = (double)rd64(|y| / M)          fdiv, fstp 0x4FB55F..0x4FB577
     s  = (double)rd64(rd64(x' x') + rd64(y' y'))                     0x4FB57B..0x4FB58D
     q  = (double)rd64(sqrt(s))                                        sqrt 0x4FB7B4 keeps the 64-bit precision
     r  = (double)rd64(mant(q) mant(M)) times 2^(exp(q) + exp(M))     0x4EC380 twice, fmul 0x4FB5B0, 0x4EC250
   It is not correctly rounded: on a model of this sequence 34 % of int pairs differ from sqrt(x^2 + y^2) by up to
   two ulp, and of the 3686 pairs 0 < a <= b < 3000 whose distance is an integer c, 359 come out below c, so _ftol
   gives c - 1 ((20, 99) is one); scaling by 2^16 keeps the pattern, so 16.16 distances inherit it. The core
   therefore emulates the sequence in integer arithmetic, each 64-bit rounding and each store's 53-bit rounding
   exact, never the x87: tests/tasim_fixed_test.c holds it to the x87 itself under the i686 build. */
double  ts_hypot(int32_t x, int32_t y);

/* The core's transcendentals, from fdlibm's algorithms and coefficients (e_atan2.c, s_atan.c, k_sin.c, k_cos.c,
   e_rem_pio2.c); each is within one ulp of the true value. ts_sin and ts_cos reduce their argument by fdlibm's
   medium path, which holds for |x| < 2^19 pi/2; past it they answer NaN, which no caller reaches: the rotator's
   angle is an int16 times 9.6e-5, below pi. */
double  ts_atan2(double y, double x);
double  ts_sin(double x);
double  ts_cos(double x);

#endif
