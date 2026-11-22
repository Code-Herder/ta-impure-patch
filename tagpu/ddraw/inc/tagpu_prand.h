#ifndef TAGPU_PRAND_H
#define TAGPU_PRAND_H
/* The particles' random rule (tagpu_patches.c, *The particles' own random rule*; effects.md §7.7).

   Every value the engine's particle code used to draw from the C runtime's rand 0x4E4870 is
   instead a pure function of three numbers, in rand's own range 0..0x7FFF:

     tagpu_prand(seed, particle, draw)

   - seed      the emission's: tagpu_prand_seed(level, GameTime, n), n the emission's ordinal
               among the emitter calls of that GameTime (0, 1, 2 ... in call order);
   - particle  the particle's ordinal within its object (TAGPU_PRAND_EMIT for a draw an emitter
               makes before its object exists; a smoke puff's ordinal modulo 256);
   - draw      the draw's number within that particle (a smoke frame advance: the frame number).

   32-bit unsigned integer arithmetic only, wrapping, so a GLSL `uint` port computes the same
   bits. Nothing here keeps state. */

#define TAGPU_PRAND_EMIT  0xFFFFFFFFu

/* lowbias32 (C. Wellons' 32-bit finaliser): the rule's one mixing step */
static inline unsigned tagpu_prand_mix(unsigned x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static inline unsigned tagpu_prand_seed(unsigned level, unsigned gametime, unsigned n)
{
    return tagpu_prand_mix(tagpu_prand_mix(level ^ gametime) ^ n);
}

static inline int tagpu_prand(unsigned seed, unsigned particle, unsigned draw)
{
    return (int)(tagpu_prand_mix(tagpu_prand_mix(seed ^ particle) ^ draw) >> 17);
}

#endif
