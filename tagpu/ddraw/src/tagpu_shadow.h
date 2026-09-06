#ifndef TAGPU_SHADOW_H
#define TAGPU_SHADOW_H
/* Classic++ cast shadows (G14h): the lab's depth map along `shadowsun`, drawn
   once per frame from everything with geometry and read back by the terrain
   and unit fragment shaders through tagpu_glsl.h's taShadowAt. Design:
   renderers.md 2.7-2.9 and 2.12; the lab it ports: tascene-view.html
   shadowFrame / shadowPass / LAB_LIGHT.

   The frame protocol, from tagpu_native.c, once the frame's 3DO stream is in
   its VBO and before the frame FBO is bound:

     if (tagpu_shadow_begin(&fv, engineShadowBit)) {   // FBO bound, 3DO depth
         for each unit:  tagpu_shadow_unit(alt, gnd + throw, sv);  // program in use
                         glDrawArrays(GL_TRIANGLES, first, count);
         tagpu_hires_depth(...);          // the replacement meshes, own program
         tagpu_shadow_hills();            // the heightfield rows under the window
         tagpu_shadow_end();              // the map on texture units 12 and 13
     }

   Every consumer program resolves its read-back uniforms once with
   tagpu_shadow_locate (program in use) and sets them per frame with
   tagpu_shadow_apply (program in use). uShadowOn is 0 on a frame with no
   map, and the two sampler uniforms are named even then: two sampler TYPES
   left on one unit make GL drop the whole draw (the lab's trap). */
#include "tagpu_fx.h"

/* 1 = a map is being drawn this frame: the shadow FBO is bound, the depth
   program for the native vertex stream (attributes 4 and 5 of the unit VAO)
   is in use with this frame's matrix. 0 = no shadows this frame (the switch,
   the cfg, the engine's Shadow bit, or a GL failure). */
int  tagpu_shadow_begin(const TAGPU_FXVIEW* v, int engineShadowBit);
/* the per-caster uniform: the unit's altitude (world y), the ground under it
   plus the air throw, and the length rule's vertical scale */
void tagpu_shadow_unit(float alt, float gndThrow, float sv);
void tagpu_shadow_hills(void);
void tagpu_shadow_end(void);
/* this frame's light matrix, for a depth program of another module */
const float* tagpu_shadow_mat(void);

/* the lab's shadowlen / airshadow rule for one caster: `top` its model
   height, `agl` its height above the ground under it; out: the vertical throw
   added to the ground and the scale on the model height (renderers.md 2.2) */
void tagpu_shadow_caster(float top, float agl, float* throw_, float* sv);

typedef struct { int on, sun, mat, scale, penumbra, shade; } TAGPU_SHADOWU;
void tagpu_shadow_locate(unsigned prog, TAGPU_SHADOWU* u);
void tagpu_shadow_apply(const TAGPU_SHADOWU* u);

int  tagpu_shadow_live(void);          /* a map was drawn this frame */
void tagpu_shadow_glreset(void);
#endif
