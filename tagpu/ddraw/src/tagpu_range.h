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
   the clears between the groups.

   THE SENSORS ARE OUTLINES, NOT RINGS: one kind's union is a set of closed
   loops of arcs (the circles' arcs no other disc of the kind covers), which
   the render thread builds and lays on the ground itself, from the height
   grid the terrain pass mirrors on this thread (tagpu_terr_heights). Each
   loop is a PATH of points (TAGPU_RGPT) and one SLINE instance, drawn as a
   band along it -- dashed on a lattice pinned to the map whose dashes keep
   their length on screen (`dash` in tagpu_range.c), or solid. The sensor
   fills are discs as the rings' are. Design and look:
   research/notes/weapon-range-rings.md, *Sensor view*. */

#include "tagpu_fx.h"           /* TAGPU_FXVIEW */

/* 64 B, one per ring and pass: what the vertex stage reads per instance.
   An SLINE instance is a path instead: c = first point, segments, side (1
   outside its circles, -1 inside), the band's width in game px; b = the
   lattice's level-0 spacing (zoom-1 game px), its coarsest level, the mode
   (0 dashed, 2 solid), how far the dashes have moved (zoom-1 game px);
   col as a ring's; w = a dash's length in game px, 0, 0, 0. */
typedef struct TAGPU_RGINST {
    float c[4];   /* the centre: world x, world z, the shooter's altitude, range */
    float b[4];   /* velocity (world px a tick), minbarrelangle (radians),
                     the kind -- 0 flat, 1 ballistic, 2.. a sensor ring
                     (tagpu_range_glsl.inc) --, the radius scale (BAR's D-gun 1.05) */
    float col[4]; /* straight RGBA; the alpha is the ring's, fades included      */
    float w[4];   /* half width in game px with the AA rim, segments, half width
                     without the rim, and the ring's first entry in the reach
                     buffer (tagpu_vk_range.c writes it; 0 here)                */
} TAGPU_RGINST;

enum {
    TAGPU_RG_CLEAR = 0,   /* the stencil to 0 over the viewport                  */
    TAGPU_RG_DISC,        /* discs: write `bit` where it is not set, fill colour */
    TAGPU_RG_OUTER,       /* lines drawn only where `bit` is not set             */
    TAGPU_RG_LINE,        /* lines, no stencil                                   */
    TAGPU_RG_SLINE        /* one path's band (SLINE instances), no stencil       */
};

/* 32 B, one point of a sensor path: where it stands and how far along the
   path it is, in zoom-1 game px of the screen the projection makes -- the
   dashes are laid on that length, so they keep their size and speed over a
   slope -- and the band's direction there (zoom-1 game px, mitred: its
   length keeps the band's width across a corner, pointing away from the
   point's circle). A closed path repeats its first point at its end. */
typedef struct TAGPU_RGPT {
    float p[4];   /* world x, world z, altitude, along                        */
    float n[4];   /* direction x, y, 1 where the band shows (on the map, and
                     over water for a water kind), 0                          */
} TAGPU_RGPT;

typedef struct TAGPU_RGDRAW {
    int kind;     /* TAGPU_RG_*                                       */
    int bit;      /* the stencil bit, 1..8; 0 for CLEAR, LINE, SLINE  */
    int first;    /* the first instance                               */
    int count;    /* instances                                        */
    int nseg;     /* segments a ring, or a path: every instance of a
                     draw shares it                                   */
} TAGPU_RGDRAW;

#define TAGPU_RG_MAXINST 16384
#define TAGPU_RG_MAXDRAW 512
#define TAGPU_RG_MAXPT   65536   /* sensor path points a frame      */
#define TAGPU_RG_LODN    9       /* the dash levels a frame draws   */

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
    /* the sensor paths: `npts` points, replaced as a whole when `ptSerial`
       moves, so a slot uploads them only then */
    const TAGPU_RGPT* pts;
    int               npts;
    unsigned          ptSerial;
    /* the dashes' levels (`dash` in tagpu_range.c): the finest drawn, the
       zoom's, and the opacity of each from the finest up */
    int   lodFine, lodLevel;
    float lodOp[TAGPU_RG_LODN];
    /* the preview's travelling rings: spacing (world px), how far they have
       travelled, the glow's power and strength; their colour by kind */
    float pulse[4];
    float pulseCol[2][4];           /* radar, sonar                              */
} TAGPU_RGHAND;

/* Render thread, once per frame, after the marker gather: this frame's
   instances and draw list from the packet. */
void tagpu_range_render(const TAGPU_FXVIEW* v);

/* Render thread: this frame's hand-over, 1 when there is one for `frame`. */
int  tagpu_range_handover(TAGPU_RGHAND* out, unsigned frame);

#endif
