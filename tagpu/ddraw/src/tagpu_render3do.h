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
/* Classic++ (G14g): the unit atlas's restored RGBA8 twin, mipmapped to
   level 2 -- 0 until the switch has been seen on and the restorer's job
   made; a shader samples it only where its alpha says the texel is painted */
unsigned int tagpu_r3d_atlas_rgbref(void);
/* The unit atlas's generation (tagpu_gaf.h): every recycle and every context
   loss moves every UV, so anything that BAKES a UV rather than re-reading it
   each frame has to be keyed on this. The geometry bake's material stream is
   (tagpu_posebake.c). */
unsigned int tagpu_r3d_atlas_gen(void);
/* Once per frame from the native pass, before its first tagpu_r3d_atlas_uv,
   with the live palette (main+0x143A7): recycles a full atlas (never between
   an emit and its draw) and drives the lazy restore -- arming it the first
   time the switch is on, rebuilding the twin's mips after each painted batch */
void tagpu_r3d_atlas_frame(const unsigned char* pal);
unsigned int tagpu_r3d_lut_texref(void);
int tagpu_r3d_shade_neutral(void);
int tagpu_r3d_shade_dir(void);
int tagpu_r3d_atlas_uv(const char* gafframe, float uv[4], float* ck);
int tagpu_r3d_ready(void);
int tagpu_r3d_ensure(void);
const char* tagpu_r3d_face_texframe(const char* fa, int owner);
int tagpu_r3d_face_colour(const char* fa);
/* build-state (nanoframe) staging for one unit — the engine's own formulas,
   shared so the composite path and the native pass stay identical. Returns 0
   for a unit that is not under construction. */
int tagpu_r3d_nano_state(const char* unit, float* t, float c[3], float* wire);
#endif
