#ifndef TAGPU_RENDER3DO_H
#define TAGPU_RENDER3DO_H
#include "tagpu.h"
/* Phase B: render a unit's engine-posed 3DO into its GAFFrame composite colour
   plane via a GL FBO (real geometry replacing the G6 proof gradient).
   Returns 1 if the colour plane was written, 0 if unavailable (caller may
   fall back). Clobbers viewport + framebuffer binding (restores binding 0);
   call it only where that is safe (end of the overlay pass). */
int tagpu_render3do(const TAGPU_FRAME* f, const char* unit, const char* obj3do,
                    char* gafframe);

/* shared material resources for the native pass (G12b) */
unsigned int tagpu_r3d_atlas_texref(void);
unsigned int tagpu_r3d_lut_texref(void);
int tagpu_r3d_shade_neutral(void);
int tagpu_r3d_shade_dir(void);
int tagpu_r3d_atlas_uv(const char* gafframe, float uv[4], float* ck);
int tagpu_r3d_ready(void);
int tagpu_r3d_ensure(void);
const char* tagpu_r3d_face_texframe(const char* fa, int owner);
int tagpu_r3d_face_colour(const char* fa);
#endif
