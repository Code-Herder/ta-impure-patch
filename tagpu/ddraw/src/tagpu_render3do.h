#ifndef TAGPU_RENDER3DO_H
#define TAGPU_RENDER3DO_H
#include "tagpu.h"
/* The opt-in write-back (`tagpu_render3do()`, `tagpu_writeback.on`) was
   deleted by the frame packet exchange's landing 3: it walked an Object3do on
   the render thread and stored the result into engine memory from there. What
   the file keeps is the material layer the native and posed passes share. */

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
/* Once per frame from the native pass, beside the other level-keyed caches and
   BEFORE tagpu_posebake_frame latches the atlas generation: drops every entry
   when the level changes, because the atlas keys on frame addresses the next
   level's loader may reuse. */
void tagpu_r3d_atlas_level(unsigned level_gen);
/* The shade LUT, built once per GL context. `shd` is the frame packet's copy
   of the engine's PALETTE.SHD table (tagpu_pk_shd), or NULL to use our own
   computed ramp — this module no longer reads the graphics globals itself. */
unsigned int tagpu_r3d_lut_texref(const unsigned char* shd);
int tagpu_r3d_shade_neutral(void);
int tagpu_r3d_shade_dir(void);
int tagpu_r3d_atlas_uv(const char* gafframe, float uv[4], float* ck);
int tagpu_r3d_ready(void);
int tagpu_r3d_ensure(void);
const char* tagpu_r3d_face_texframe(const char* fa, int owner);
int tagpu_r3d_face_colour(const char* fa);
/* build-state (nanoframe) staging for one unit — the engine's own formulas,
   shared so every pass that stages a nanoframe uses the same ones. Takes the
   three values it used to read off the unit record itself: the build fraction
   REMAINING, the unit's stable in-game id and the sim tick, all of which the
   frame packet carries. Returns 0 for a unit that is not under construction. */
int tagpu_r3d_nano_state(float nano, unsigned id, unsigned tick,
                         float* t, float c[3], float* wire);

/* ---- THE VULKAN LANE'S TEXELS (Phase G / G19e, the unit pass) ------------

   The unit fragment shader samples the unit atlas on texture unit 0 and the
   shade LUT on unit 1; a second backend cannot read either GL texture, so both
   are mirrored on the CPU. The atlas takes tagpu_gaf.h's mechanism unchanged
   -- the mirror is written by the same atlas_paint that writes GL, and asking
   for one marks every painted entry for repaint so that it is correct from the
   instant it exists. The LUT is 8 KB built once per context and is simply
   kept.

   `_want` is idempotent and costs nothing until it is called. `_mirror`
   returns NULL while there is none, which a pass treats as "stand down this
   frame" and not as an error: the atlas re-converges over the next few frames.
   `rows` is the shelf cursor, so only the rows the packer has used are
   uploaded. Render thread only, like the rest of this module. */
void tagpu_r3d_atlas_mirror_want(void);
const unsigned char* tagpu_r3d_atlas_mirror(int* dim, int* rows, unsigned* serial);
/* 256 x 32 R8, the bytes `shade_upload` last gave glTexSubImage2D. The serial
   moves when the table is rebuilt -- which happens once per context, and again
   the first time the engine's own PALETTE.SHD arrives after a frame with none. */
const unsigned char* tagpu_r3d_lut_mirror(int* w, int* h, unsigned* serial);
#endif
