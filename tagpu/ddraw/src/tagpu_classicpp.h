#ifndef TAGPU_CLASSICPP_H
#define TAGPU_CLASSICPP_H
/* The Classic++ switch and its knobs -- two files in the gamedir, both the
   player's (renderers.md 2.10: the menu, when it comes, is a front end over
   these, not a store of its own):

     tagpu_classicpp.on    the renderer switch. Absent = Classic, exactly
                           today's pixels; present = the restored atlases,
                           the lighting below, the RGB fog rule (2.6).
     tagpu_classicpp.cfg   numeric knobs, `key=value` tokens separated by
                           whitespace or newlines, keyed like the lab's URL
                           parameters (tascene-design.md):
                             sun=AZ,EL      the terrain's sun, degrees
                                            (azimuth, elevation); `sun=off`
                                            turns every sun off
                             unitsun=AZ,EL  the units' sun
                             amb=A          the ambient floor, 0..1
                           A missing file or key is the lab's default.

   Both are polled at most twice a second; the cfg is re-read when its write
   time or size changes, so `tacli arm <i> 'classicpp.cfg=sun=off'` takes
   effect within the next poll, live. */

int tagpu_classicpp_on(void);

/* The lighting the knobs describe, in the lab's terms (tascene-view.html
   readLook / LAB_LIGHT): a sun as the unit vector TOWARD the light in map
   space (x east, y up, z south), the ambient floor, and what LEVEL ground
   receives from that sun -- `level = amb + (1-amb)*max(sun.y, 0)` -- which
   every lambert is divided by so that level is exactly 1.0 (the art is
   already lit; the sun may only modulate by the tilt from level). The
   shaders take 1/level as uNorm (tagpu_glsl.h TAGPU_GLSL_LIGHT_FN).
   `sun=off` is amb = 1: the rule is then exactly 1.0 with no branch. */
typedef struct {
    float sun[3];
    float unitSun[3];
    float amb;
    float level, unitLevel;
} TAGPU_LIGHT;
const TAGPU_LIGHT* tagpu_classicpp_light(void);

/* The terrain rule evaluated on the CPU for a normal `n` (unit length, map
   space): the lab's lambertAt, for the passes that light a billboard by the
   ground under it. Exactly 1.0 for a level normal -- the same expression
   computes `level`, so the quotient is x/x. */
float tagpu_classicpp_ground(const float n[3]);
#endif
