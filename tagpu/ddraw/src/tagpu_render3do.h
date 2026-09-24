#ifndef TAGPU_RENDER3DO_H
#define TAGPU_RENDER3DO_H
#include "tagpu.h"
#include "tagpu_restoreglsl.h"   /* TAGPU_RGLSL_FRAME, the restore list */
/* The material layer the native and posed passes share. */

/* shared material resources for the native pass */
/* Whether a Classic++ restore route exists for the unit atlas. */
int          tagpu_r3d_atlas_restore_armed(void);
/* The unit atlas's generation (tagpu_gaf.h): every recycle and every level
   drop (tagpu_r3d_atlas_level) moves every UV, so anything that BAKES a UV rather than re-reading it
   each frame has to be keyed on this. The geometry bake's material stream is
   (tagpu_posebake.c). */
unsigned int tagpu_r3d_atlas_gen(void);
/* Once per frame from the native pass, before its first tagpu_r3d_atlas_uv:
   recycles a full atlas, never between an emit and its draw. */
void tagpu_r3d_atlas_frame(void);
/* Once per frame from the native pass, beside the other level-keyed caches and
   BEFORE tagpu_posebake_frame latches the atlas generation: drops every entry
   when the level changes, because the atlas keys on frame addresses the next
   level's loader may reuse. */
void tagpu_r3d_atlas_level(unsigned level_gen);
/* Build the face-shade calibration and its multipliers, once. `shd` is the
   frame packet's copy of the engine's PALETTE.SHD table (tagpu_pk_shd), or
   NULL to use our own computed ramp — this module does not read the graphics
   globals itself. */
void tagpu_r3d_shade_want(const unsigned char* shd);
int tagpu_r3d_shade_neutral(void);
int tagpu_r3d_shade_dir(void);
int tagpu_r3d_atlas_uv(const char* gafframe, float uv[4], float* ck);
int tagpu_r3d_ready(void);
int tagpu_r3d_ensure(void);
const char* tagpu_r3d_face_texframe(const char* fa, int owner);
int tagpu_r3d_face_colour(const char* fa);
/* build-state (nanoframe) staging for one unit — the engine's own formulas,
   shared so every pass that stages a nanoframe uses the same ones. Takes
   three values from the frame packet rather than the unit record: the build
   fraction REMAINING, the unit's stable in-game id and the sim tick. Returns 0
   for a unit that is not under construction. */
int tagpu_r3d_nano_state(float nano, unsigned id, unsigned tick,
                         float* t, float c[3], float* wire);

/* ---- THE VULKAN LANE'S TEXELS (Phase G, the unit pass) ------------------

   The unit fragment shader samples the unit atlas's base expansion and the
   face-shade multipliers, and both exist on this side only as CPU bytes, which
   the Vulkan unit pass expands and uploads. The atlas takes tagpu_gaf.h's
   mechanism unchanged -- the mirror is written by atlas_paint, and asking for
   one marks every painted entry for repaint so that it is correct from the
   instant it exists. The multipliers are 32 floats and are simply kept.

   `_want` is idempotent and costs nothing until it is called. `_mirror`
   returns NULL while there is none, which a pass treats as "stand down this
   frame" and not as an error: the atlas re-converges over the next few frames.
   `rows` is the shelf cursor, so only the rows the packer has used are
   uploaded. Render thread only, like the rest of this module. */
void tagpu_r3d_atlas_mirror_want(void);
const unsigned char* tagpu_r3d_atlas_mirror(int* dim, int* rows, unsigned* serial);
/* The base atlas's other two inputs from the same atlas: the key plane (NULL
   while there is no mirror) and, in `*bands`, the ring of its recent writes
   (tagpu_gaf.h). */
struct TAGPU_GAFBAND;
const unsigned char* tagpu_r3d_atlas_key(const struct TAGPU_GAFBAND** bands);

/* ASK FOR THE CLASSIC++ RESTORED TWIN. Gated on `tagpu_classicpp_assets()`,
   because the twin costs 16 MB that a session with Classic++ off must not pay
   for, and idempotent: call it once per published frame and it latches on
   success.

   IT ARMS THE LIST, AND THAT IS ALL IT ARMS. */
void tagpu_r3d_atlas_restore_want(void);
/* THE LIST OF FRAMES TO RESTORE, which `_want` above arms under Classic++
   `assets=1`. `mips` and
   `aniso` describe the TWIN and come from the atlas, because nothing reads it
   back. NULL until the list is armed and has entries. */
const TAGPU_RGLSL_FRAME* tagpu_r3d_atlas_restore_list(int* dim, int* n, unsigned* gen,
                                                      int* mips, float* aniso);
/* The face-shade multiplier, 32 floats indexed by the SHD row a face takes,
   or NULL until the calibration is built -- tagpu_render3do.c's `s_shadeK`
   says how it is fitted. Rebuilt once, and again the first time the engine's
   own PALETTE.SHD arrives after a frame with none. */
const float* tagpu_r3d_shade_k(void);
#endif
