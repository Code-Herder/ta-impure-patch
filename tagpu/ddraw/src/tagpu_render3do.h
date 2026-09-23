#ifndef TAGPU_RENDER3DO_H
#define TAGPU_RENDER3DO_H
#include "tagpu.h"
#include "tagpu_restoreglsl.h"   /* TAGPU_RGLSL_FRAME, the restore list */
/* The opt-in write-back (`tagpu_render3do()`, `tagpu_writeback.on`) was
   deleted by the frame packet exchange's landing 3: it walked an Object3do on
   the render thread and stored the result into engine memory from there. What
   the file keeps is the material layer the native and posed passes share. */

/* shared material resources for the native pass (G12b) */
/* Classic++ (G14g): the unit atlas's restored RGBA8 twin, mipmapped to
   level 2 -- 0 until the switch has been seen on and the restorer's job
   made; a shader samples it only where its alpha says the texel is painted */
unsigned int tagpu_r3d_atlas_rgbref(void);
int          tagpu_r3d_atlas_restore_armed(void);
/* The unit atlas's generation (tagpu_gaf.h): every recycle and every context
   loss moves every UV, so anything that BAKES a UV rather than re-reading it
   each frame has to be keyed on this. The geometry bake's material stream is
   (tagpu_posebake.c). */
unsigned int tagpu_r3d_atlas_gen(void);
/* Once per frame from the native pass, before its first tagpu_r3d_atlas_uv:
   recycles a full atlas, never between an emit and its draw. It also drove the
   GL lazy restore until 11-5e-2 -- that is what its `pal` argument was for --
   and the restore is now published as a frame list for the Vulkan lane. */
void tagpu_r3d_atlas_frame(void);
/* Once per frame from the native pass, beside the other level-keyed caches and
   BEFORE tagpu_posebake_frame latches the atlas generation: drops every entry
   when the level changes, because the atlas keys on frame addresses the next
   level's loader may reuse. */
void tagpu_r3d_atlas_level(unsigned level_gen);
/* The shade LUT, built once per GL context. `shd` is the frame packet's copy
   of the engine's PALETTE.SHD table (tagpu_pk_shd), or NULL to use our own
   computed ramp — this module no longer reads the graphics globals itself. */
/* Build the shade LUT and its CPU mirror. `_texref` calls this and then hands
   back the GL name; a lane with no GL calls this directly, because the LUT is
   the pass's and only the texture is the backend's. */
void tagpu_r3d_lut_want(const unsigned char* shd);
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

/* ASK FOR THE CLASSIC++ RESTORED TWIN (gate 3 of the Vulkan-only plan, and
   11-5e-2b). Gated on `tagpu_classicpp_assets()`, because the twin costs 16 MB
   that a session with Classic++ off must not pay for, and idempotent: call it
   once per published frame and it latches on success.

   IT ARMS THE LIST, AND THAT IS ALL IT ARMS. Until 11-5e-2b these were three
   calls -- `_want`, `_step` and `_mirror_rgb` -- and `_want` chose between the
   list and a 16 MB RGBA8 READ-BACK of the GL twin, which `_step` drove with
   `glReadPixels` through an FBO (hence "once per published frame, on the render
   thread with the context current") and `_mirror_rgb` handed over as texels.
   opengl32.dll is never in the process, so the read-back never produced a row;
   all three shrank to this one. */
void tagpu_r3d_atlas_restore_want(void);
/* THE LIST OF FRAMES TO RESTORE, which `_want` above arms under Classic++
   `assets=1` (the Vulkan-only plan's landing 7e-2). `mips` and
   `aniso` describe the TWIN and come from the atlas, because nothing reads it
   back. NULL until the list is armed and has entries. */
const TAGPU_RGLSL_FRAME* tagpu_r3d_atlas_restore_list(int* dim, int* n, unsigned* gen,
                                                      int* repaint, unsigned* blanks,
                                                      int* mips, float* aniso);
/* 256 x 32 R8, the bytes `shade_upload` last gave glTexSubImage2D. The serial
   moves when the table is rebuilt -- which happens once per context, and again
   the first time the engine's own PALETTE.SHD arrives after a frame with none. */
const unsigned char* tagpu_r3d_lut_mirror(int* w, int* h, unsigned* serial);
#endif
