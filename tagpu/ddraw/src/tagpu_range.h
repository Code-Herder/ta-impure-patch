#ifndef TAGPU_RANGE_H
#define TAGPU_RANGE_H
/* tagpu_range — BAR's weapon range preview on TA's own reach rule.

   Design: research/notes/weapon-range-rings.md. The browser reference is
   tools/tascene-ranges.js, and this module draws what that one draws: the
   rings of the selected units, of the unit under the cursor and of the
   builders while a build command is active (BAR's gui_attackrange_gl4), the
   rings of the defenses by class and of a defense being placed
   (gui_defenserange_gl4); per class a merged outline of the union, a faint
   fill (attack rings only) and faint per-unit inner rings.

   THE TWO HALVES. The game thread publishes one record per ring into the
   frame packet (TAGPU_PK_RING, tagpu_packet.h): where its unit stands, its
   range and the two numbers the engine's ballistic solver takes. The render
   thread (`tagpu_range_render`, tagpu_range.c) turns them into the
   instances and the draw list below, and tagpu_vk_range.c draws them. The
   SHAPE of a ring -- its reach in each direction over the terrain -- is
   computed on the GPU by a compute pass (tagpu_range_comp.h) from the height
   grid the terrain pass already holds, once per ring for as long as the ring
   stays the same, so the render thread reads no engine memory and the CPU
   walks no terrain.

   THE UNION IS THE STENCIL'S, as BAR's fallback path has it: a class's
   discs write its bit, and its outer rings are drawn only where no disc of
   the class wrote it, which leaves the outer envelope. The draw list carries
   the clears between the groups. */

#include "tagpu_fx.h"           /* TAGPU_FXVIEW */

/* 64 B, one per ring and pass: what the vertex stage reads per instance. */
typedef struct TAGPU_RGINST {
    float c[4];   /* the centre: world x, world z, the shooter's altitude, range */
    float b[4];   /* velocity (world px a tick), minbarrelangle (radians),
                     1 = ballistic, the radius scale (BAR's D-gun 1.05)          */
    float col[4]; /* straight RGBA; the alpha is the ring's, fades included      */
    float w[4];   /* half width in game px with the AA rim, segments, half width
                     without the rim, and the ring's first entry in the reach
                     buffer (tagpu_vk_range.c writes it; 0 here)                */
} TAGPU_RGINST;

enum {
    TAGPU_RG_CLEAR = 0,   /* the stencil to 0 over the viewport                  */
    TAGPU_RG_DISC,        /* discs: write `bit` where it is not set, fill colour */
    TAGPU_RG_OUTER,       /* lines drawn only where `bit` is not set             */
    TAGPU_RG_LINE         /* lines, no stencil                                   */
};

typedef struct TAGPU_RGDRAW {
    int kind;     /* TAGPU_RG_*                                       */
    int bit;      /* the stencil bit, 1..8; 0 for CLEAR and LINE      */
    int first;    /* the first instance                               */
    int count;    /* instances                                        */
    int nseg;     /* segments a ring: every instance of a draw shares it */
} TAGPU_RGDRAW;

#define TAGPU_RG_MAXINST 16384
#define TAGPU_RG_MAXDRAW 256

typedef struct TAGPU_RGHAND {
    const TAGPU_RGINST* inst;
    int                 ninst;
    const TAGPU_RGDRAW* draws;
    int                 ndraw;
    unsigned            frame;
    float gw, gh, ss;               /* the game frame and its supersample        */
    float zoom, zoomCx, zoomCy;     /* the world passes' zoom about this centre  */
    float eyeX, eyeY;               /* eye minus the viewport's corner, game px  */
    int   vpL, vpT, vw, vh;         /* the viewport, game px: the scissor        */
    float mapW, mapH;               /* the scroll extent, world px: the edge fade */
    float sea;                      /* the sea level, world px                   */
    float grav;                     /* gravity, world px a tick squared          */
} TAGPU_RGHAND;

/* Render thread, once per frame, after the marker gather: this frame's
   instances and draw list from the packet. */
void tagpu_range_render(const TAGPU_FXVIEW* v);

/* Render thread: this frame's hand-over, 1 when there is one for `frame`. */
int  tagpu_range_handover(TAGPU_RGHAND* out, unsigned frame);

#endif
