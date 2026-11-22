#ifndef TAGPU_TURN3_H
#define TAGPU_TURN3_H
/* tagpu_turn3.h -- the engine's turn by a triple, 0x4B6CC0, with each angle's sine and cosine taken
   once for every vector it turns, bit-identical to calling 0x4B6CC0 on each.

   0x4B6CC0(src, dst, turn) turns three pairs through 0x4B7173(angle, pair): (x, y) by turn[0], then
   (y, z) by turn[2], then (x, z) by turn[1], reading all of src before it writes dst. 0x4B7173 does
   nothing for a zero angle (0x4B717B); otherwise it filds the pair, filds the int16 angle, multiplies
   it by the double at 0x509EF8, takes fsincos (0x4B7189..0x4B7192), and runs a fixed multiply-add
   sequence into two fistp (0x4B7194..0x4B71A4). Every vector turned by one triple takes the same three
   fsincos of the same three arguments ([render](research/notes/engine/render.md), its 0x4B7173 row).

   THE INVARIANT: tagpu_turn3_apply writes what 0x4B6CC0 writes, bit for bit. It rests on:
     - the same operations on the same operands: tagpu_turn3_prepare forms each argument with the
       engine's filds and fmull of the same memory operand and takes fsincos, keeping both results at
       the x87's own 80 bits (fstpt, fldt: exact); fsincos is a function of its argument and the
       control word, and the caller turns its vectors under the control word it prepared under (it
       calls nothing in between that sets one); turn3_pair is 0x4B7173's sequence with the two loads
       in place of the three instructions that made them, and assembles to the engine's arithmetic
       byte for byte (the build's disassembly against 0x4B7194..0x4B71A4);
     - the engine code it stands for being stock's: a caller that replaces the engine's own calls
       byte-matches 0x4B6CC0 and 0x4B7173 first (tagpu_patches.c, repose_trig).
   The x87 exception flags this raises are stock's when the caller prepares only where stock would
   take at least one fsincos (a triple with a vector to turn): they are sticky, and each fsincos here
   is stock's operation on stock's operand. (The rotation's table skips fsincos at 0x4B7173's other
   calls, so the process's flags are not stock's; nothing reads them for a value.)

   THE ASM LEAVES ARE NOT INSTRUMENTED (no_instrument_function): the coverage build instruments the
   simulation files (Makefile, THE COVERAGE BUILD), and these carry inline assembly. Their callers are
   instrumented and are what a replay gate asks to be reached. */

#include <string.h>

typedef struct { unsigned char b[10]; } TAGPU_X87;          /* the x87's 80-bit format, as fstpt stores it */
typedef struct {
    short     a[3];                                         /* the triple, as 0x4B6CC0 is handed it        */
    TAGPU_X87 c[3], s[3];                                   /* cos and sin of each nonzero word            */
} TAGPU_TURN3;

/* fsincos of the int16 angle times the double at 0x509EF8, as 0x4B7189..0x4B7192 forms it */
__attribute__((no_instrument_function))
static inline void turn3_trig(short angle, TAGPU_X87* c, TAGPU_X87* s)
{
    __asm__ __volatile__("filds %2\n\t"
                         "fmull 0x509EF8\n\t"
                         "fsincos\n\t"
                         "fstpt %0\n\t"
                         "fstpt %1\n\t"
                         : "=m"(*c), "=m"(*s)
                         : "m"(angle)
                         : "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
}

/* 0x4B7173 on the pair p[0], p[1] with its sine and cosine already taken: 0x4B7180..0x4B7187, the two
   loads, then 0x4B7194..0x4B71A4. AT&T operand order, as objdump prints the engine's. */
__attribute__((no_instrument_function))
static inline void turn3_pair(int* p, const TAGPU_X87* c, const TAGPU_X87* s)
{
    __asm__ __volatile__("fildl 4(%0)\n\t"
                         "fildl (%0)\n\t"
                         "fld %%st(1)\n\t"
                         "fld %%st(1)\n\t"
                         "fldt %2\n\t"
                         "fldt %1\n\t"
                         "fmul %%st, %%st(5)\n\t"
                         "fmulp %%st, %%st(4)\n\t"
                         "fmul %%st, %%st(2)\n\t"
                         "fmulp %%st, %%st(1)\n\t"
                         "faddp %%st, %%st(3)\n\t"
                         "fsubrp %%st, %%st(1)\n\t"
                         "fistpl (%0)\n\t"
                         "fistpl 4(%0)\n\t"
                         :
                         : "a"(p), "m"(*c), "m"(*s)
                         : "memory", "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
}

/* the sines and cosines of `turn`'s nonzero words */
__attribute__((no_instrument_function))
static inline void tagpu_turn3_prepare(TAGPU_TURN3* t, const short turn[3])
{
    int k;
    for (k = 0; k < 3; k++) {
        t->a[k] = turn[k];
        if (turn[k]) turn3_trig(turn[k], &t->c[k], &t->s[k]);
    }
}

/* 0x4B6CC0(src, dst, t->a): src read whole before dst is written, so dst may be src */
__attribute__((no_instrument_function))
static inline void tagpu_turn3_apply(const TAGPU_TURN3* t, const int src[3], int dst[3])
{
    int xy[2], yz[2], xz[2];
    xy[0] = src[0]; xy[1] = src[1];
    if (t->a[0]) turn3_pair(xy, &t->c[0], &t->s[0]);
    yz[0] = xy[1]; yz[1] = src[2];
    if (t->a[2]) turn3_pair(yz, &t->c[2], &t->s[2]);
    xz[0] = xy[0]; xz[1] = yz[1];
    if (t->a[1]) turn3_pair(xz, &t->c[1], &t->s[1]);
    dst[0] = xz[0]; dst[1] = yz[0]; dst[2] = xz[1];
}

#endif
