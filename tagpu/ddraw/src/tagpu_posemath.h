/* tagpu_posemath.h -- the arithmetic of a COB pose: the engine's pair rotator, a piece's local
   transform from its TURN and offset, and the 4x3 product that accumulates them down the tree.
   tagpu_native.c poses the game's units with it and tagpu_wikiworld.c the wiki stage's, so the two
   cannot disagree. The conventions -- the axes, the Z-X-Y order, MOVE added before the rotation --
   and how they were measured: tagpu_native.c, "COB pose". Pure functions: no state, any thread. */
#ifndef TAGPU_POSEMATH_H
#define TAGPU_POSEMATH_H

#include <math.h>

/* the engine's pair rotator 0x4B7173 without its rounding: a' = a*cos -
   b*sin, b' = a*sin + b*cos (DISASSEMBLED; 0x4B6CC0 composes three of them) */
static __inline void rot2(float c, float s, float* a, float* b)
{
    float na = *a * c - *b * s;
    *b = *a * s + *b * c;
    *a = na;
}

/* o = a * b, both 4x3 row-major (rows of {m0,m1,m2,t}) */
static __inline void m43_mul(const float* a, const float* b, float* o)
{
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++)
            o[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] + a[r*4+2]*b[2*4+c];
        o[r*4+3] = a[r*4+0]*b[0*4+3] + a[r*4+1]*b[1*4+3] +
                   a[r*4+2]*b[2*4+3] + a[r*4+3];
    }
}

/* T(d) * R(turn), as a 4x3. R is built by rotating the basis vectors through
   the very same rot2 sequence a vertex would take, so the matrix cannot
   disagree with the convention above. */
static __inline void piece_local(const unsigned short* turn, const float* d, float* o)
{
    const float K = 6.2831853f / 65536.0f;
    float c0 = cosf((float)turn[0] * K), s0 = sinf((float)turn[0] * K);
    float c1 = cosf((float)turn[1] * K), s1 = sinf((float)turn[1] * K);
    float c2 = cosf((float)turn[2] * K), s2 = sinf((float)turn[2] * K);
    int j;
    for (j = 0; j < 3; j++) {
        float x = j == 0 ? 1.0f : 0.0f;
        float y = j == 1 ? 1.0f : 0.0f;
        float z = j == 2 ? 1.0f : 0.0f;
        if (turn[2]) rot2(c2, s2, &x, &y);      /* about Z */
        if (turn[0]) rot2(c0, s0, &y, &z);      /* about X */
        if (turn[1]) rot2(c1, s1, &x, &z);      /* about Y */
        o[0*4+j] = x; o[1*4+j] = y; o[2*4+j] = z;
    }
    o[0*4+3] = d[0]; o[1*4+3] = d[1]; o[2*4+3] = d[2];
}

#endif
