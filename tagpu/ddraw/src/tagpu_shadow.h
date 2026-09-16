#ifndef TAGPU_SHADOW_H
#define TAGPU_SHADOW_H
/* Classic++ cast shadows (G14i): the lab's depth map along `shadowsun`, drawn
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
/* uShScale as tagpu_shadow_apply writes it -- the texel, the depth span and
   1/res -- for a consumer that publishes its uniforms instead of setting them
   (the Vulkan lane). Zeroed when no map is live, which is what a freshly
   linked GL program reads there too. */
void tagpu_shadow_scale(float* out3);

/* the lab's shadowlen / airshadow rule for one caster: `top` its model
   height, `agl` its height above the ground under it; out: the vertical throw
   added to the ground and the scale on the model height (renderers.md 2.2) */
void tagpu_shadow_caster(float top, float agl, float* throw_, float* sv);

typedef struct { int on, sun, mat, scale, penumbra, shade; } TAGPU_SHADOWU;
void tagpu_shadow_locate(unsigned prog, TAGPU_SHADOWU* u);
void tagpu_shadow_apply(const TAGPU_SHADOWU* u);

int  tagpu_shadow_live(void);          /* a map was drawn this frame */
void tagpu_shadow_glreset(void);

/* ---- THE VULKAN LANE'S HAND-OVER (Phase G / G19e, the FIFTH world pass) ----

   tagpu_vk_shadow.c draws the same map into an offscreen depth image of its
   own, from the same matrix and the same caster geometry, so that the ported
   terrain pass has something to sample. This is everything it is handed;
   it reads no engine state and re-derives nothing.

   WHAT IT DOES NOT CARRY IS THE POINT OF `otherCasters`. The GL map is drawn
   from four kinds of geometry -- the native 3DO stream, the posed program's
   depth twin, the replacement meshes and the heightfield -- and only the
   heightfield is on the Vulkan side of the seam today (the unit pass is the
   landing after this one). A map missing a caster is a DIFFERENT MAP, and a
   consumer that sampled it would draw a different picture from its own oracle,
   so `otherCasters` is the count of casters the GL lane drew that this
   hand-over has no copy of, and a non-zero count is a refusal rather than a
   best effort.

   THE POINTERS ARE THE TERRAIN MODULE'S and name the CPU mirror of the caster
   mesh, which that module frees and rebuilds on a map change. `frame` is the
   fork's monotonic render-thread counter and `tagpu_shadow_handover` refuses
   any other frame's, exactly as the terrain and feature hand-overs do: it is
   what makes "these pointers are alive" a property of the frame number rather
   than of which functions happened to run. */
typedef struct TAGPU_SHADOWHAND {
    unsigned frame;
    int      res;                /* the map's edge in texels, this frame     */
    float    mat[16];            /* uShadowMat, column-major, as GL got it   */
    /* the heightfield caster, and the exact index range the GL draw used */
    const float*    hv;   size_t hnv;
    const unsigned* hi;   size_t hni;
    unsigned        hillsSerial;
    unsigned        firstIndex, indexCount;   /* 0 = the hills did not draw  */
    int             otherCasters;             /* see above; 0 = complete     */
} TAGPU_SHADOWHAND;

/* 0 when no map was drawn this frame, when this frame's hand-over has already
   been taken, or when the standing one was published on a different frame than
   `now`. Render thread only. */
int  tagpu_shadow_handover(TAGPU_SHADOWHAND* out, unsigned now);

/* The casters this frame's map holds that the hand-over above carries no copy
   of -- the posed program's depth twin and the replacement meshes. Called by
   tagpu_native.c between the caster draws and `tagpu_shadow_end`; the native
   3DO stream counts itself, in `tagpu_shadow_unit`. */
void tagpu_shadow_note_casters(int n);
#endif
