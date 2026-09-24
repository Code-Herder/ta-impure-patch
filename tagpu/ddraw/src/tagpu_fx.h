#ifndef TAGPU_FX_H
#define TAGPU_FX_H
/* Effects pass (weapon fire, explosions, debris) — gathered from the live
   engine arrays the two engine passes read (projectiles 0x49BE60, explosions
   0x420B00), rendered natively into the native pass's FBO. Armed by
   tagpu_fx.on. See research/notes/effects.md. */
#include "tagpu.h"
#include "tagpu_restoreglsl.h"   /* TAGPU_RGLSL_FRAME, the restore request */
#include "tagpu_packet.h"        /* the effect and particle tables' caps: the buckets' sizes */

/* everything the effects gather/render needs from the native pass's frame.
   THE ENGINE POINTER IS NOT IN IT (frame packet exchange), and neither
   effects pass reads engine memory at all: the projectiles, explosions, debris and the ten particle layers arrive as
   tables in `packet`, gathered by the game thread once per sim tick. */
struct TAGPU_PACKET;
typedef struct TAGPU_FXVIEW {
    const struct TAGPU_PACKET* packet;  /* this frame's packet (tagpu_packet.h), never NULL
                                    here; valid for this frame only, never cached */
    int eyeX, eyeY, vpL, vpT;    /* the PREDICTED eye and the true viewport, from the packet */
    int gw, gh, ss;
    float zoom, zoomCx, zoomCy;  /* the native pass's view zoom                */
    /* THE EFFECTS BAND'S SEQUENCE (tagpu_fx.c, "the effects band"): model k
       of this frame takes `encFx + (2k + 1) * encStep`, and a sprite or a
       line emitted after k models `encFx + 2k * encStep`. `modelCap` is the
       count the step was sized for (tagpu_fx_model_bound), so no key leaves
       the band; `modelsOn` is 0 on a frame the posed pass cannot record, and
       then no record with a model is drawn. */
    float encFx, encStep;
    int   modelCap, modelsOn;
    float depthScale;            /* the vertex stages' depth scale             */
    float encLayer[10];          /* particle layer n -> depth key (tagpu_sfx)  */
    int r0, rows;                /* the row sweep's base row and count: a row  */
                                 /* key is (feat?3:1) + (row-r0)*4 (tagpu_feat)*/
    int vw, vh, scafOn;          /* viewport size; scene-depth scaffold armed  */
    /* The viewport the ZOOM actually shows. The vertex shaders scale about the
       view centre, so at zoom z a fragment inside the viewport comes from a
       game-space rect of vw/z x vh/z centred on the same point: zoomed out, the
       gathers must reach further than the engine's own viewport or the world
       stops short of the frame edge. Identical to vpL/vpT/vw/vh at z >= 1, so
       at 1x they change nothing. */
    int evpL, evpT, evw, evh;
    int fogMode;                 /* bit0 = the engine's fog overlay is live    */
                                 /* (the grid itself says what it paints);     */
                                 /* bit1 = LosType true-LOS mode, diagnostic   */
    const unsigned short* fogGrid;  /* engine screen fog grid, corner masks    */
    int fogCols, fogRows;           /* its dims (view-anchored 32-px cells)    */
    int fogCells;                   /* cells the BUFFER holds — the real bound */
    int fogOrgX, fogOrgY;           /* world x, projected z of its cell (0,0)  */
    unsigned int frame_counter;
} TAGPU_FXVIEW;

/* ONE EFFECTS MODEL, already rasterised: its runs are `nrun` entries of
   tagpu_fxmodel.c's arena from `run0` (tagpu_fxmodel_runs), in frame pixels,
   and the unit pass draws them (tagpu_native.c hands each model to
   tagpu_posedraw_fx). The packet's model itself is not carried: the
   rasteriser is the one module that reads it. */
typedef struct TAGPU_FXMODEL {
    unsigned run0, nrun;         /* its runs in the arena, this frame's        */
    float wx, wz;                /* world x, projected world z (fog lookup)    */
    float enc;                   /* its key in the effects band's sequence     */
} TAGPU_FXMODEL;

#define TAGPU_FXMODE_FLAT   0
#define TAGPU_FXMODE_OPAQUE 1
#define TAGPU_FXMODE_ALPHA  2
#define TAGPU_FXMODE_FLASH  3

/* THE FLASH LEVEL OF A TEXEL, AND WHERE IT TRAVELS. A flash sprite (mode 3)
   adds the light-table colour of level `index - 0x4F`, the engine's own blit
   (0x4B8EC0: `dst = LHT[(src - 0x4F) * 256 + dst]`, effects.md), clamped to
   the table's 32 levels. The base atlas the Vulkan pass samples is RGBA and
   has no index left, so the level travels in its ALPHA: 255 - level at an art
   texel (224..255, so the hole test every reader makes, alpha < 0.5, is
   exact) and 0 at the frame's key (tagpu_pal_expand). The fragment stage
   reads it back as 255 - alpha. */
#define TAGPU_FX_FLASH_LO 0x4F
#define TAGPU_FX_FLASH_ALPHA(i) \
    (255 - ((i) < TAGPU_FX_FLASH_LO ? 0 : (i) > TAGPU_FX_FLASH_LO + 31 ? 31 : (i) - TAGPU_FX_FLASH_LO))

int  tagpu_fx_armed(unsigned frame_counter);    /* tagpu_fx.on present (30f) */
/* An upper bound on the models the gather can emit from this packet: every
   model a projectile, a debris piece or an explosion names, before any gate.
   The native pass sizes the effects band's step from it BEFORE it fixes the
   frame's depth bands, and the gather refuses a model past it. */
int  tagpu_fx_model_bound(const struct TAGPU_PACKET* pk);
int  tagpu_fx_gather(const TAGPU_FXVIEW* v);    /* returns total drawables    */
int  tagpu_fx_nmodels(void);
const TAGPU_FXMODEL* tagpu_fx_model(int i);
/* lines + sprites into the currently bound FBO (depth test on, mask off);
   uses its own program/VAO; leaves program/VAO/texture bindings dirty */
void tagpu_fx_render(const TAGPU_FXVIEW* v);

/* emission API for the particle pass (tagpu_sfx.c): a GAF frame the PACKET
   resolved, or a DrawBar 2x2 dot, into the shared buckets at an explicit
   depth key. `g` is an opaque handle: only tagpu_gaf.c ever reads it. */
int  tagpu_fx_emit_frame(const unsigned char* g, int sx, int sy, int mode,
                         float wx, float wz, float enc, int under);
int  tagpu_fx_emit_dot(int x, int y, int colidx, float wx, float wz, float enc, int under);
void tagpu_fx_set_mute(int on);                 /* passive: count, emit nothing */
unsigned tagpu_fx_caps(void);                   /* the PACKET's copy of TAProgram+0xF0:
                                                   bit5 ALP built, bit7 LHT built      */
int  tagpu_fx_tile_visible(const TAGPU_FXVIEW* v, int wx, int wzp);   /* engine LOS gate */
/* the fragment shaders' fog rule (tagpu_glsl.h) on the CPU, for gather-side
   gates: bit0 = the engine paints this point black, bit1 = it shade-remaps it.
   wzp is the PROJECTED world z (y - alt/2), the space the grid is built in. */
int  tagpu_fog_at(const unsigned short* grid, int cols, int rows, int cells,
                  int orgX, int orgY, int wx, int wzp);

/* ---- the hand-over to the Vulkan pass ----------------------------------------

   Everything this pass built this frame, so that tagpu_vk_fx.c draws it
   rather than a second implementation of it. Nothing here is re-derived: the
   vertices are the four bucket arrays the gather filled, the uniforms are the
   view's numbers, the texels are the CPU-side bytes (the atlas's mirror and
   the tables below), and the shader is tagpu_fx.c's GLSL through
   tools/spirv-gen.py.

   HANDED OVER EXACTLY ONCE, like the feature pass's, so one frame's geometry
   can never be drawn twice; a frame this pass skipped hands over nothing and
   the Vulkan lane draws nothing.

   THE POINTERS ARE THIS FILE'S AND THE ATLAS MODULE'S. Both halves run on the
   RENDER THREAD and the whole of the native pass happens earlier in the same
   iteration of render_vk.c's loop than the tagpu_vk_frame that consumes this,
   so the game thread never touches them.

   WHAT THE FRAME STAMP BOUNDS HERE IS STALENESS, NOT A DANGLING POINTER. None
   of these pointers can dangle: `s_verts` is a fixed file-static array,
   `s_lhtRGB` is another, and tagpu_gaf.c never frees a mirror -- it memsets
   it. What CAN happen is worse than it sounds anyway: `tagpu_native.c` calls
   `tagpu_fx_render` only `if (nfx)`, so a frame that gathered nothing never
   reaches the publisher at all, while the NEXT gather has already overwritten
   `s_verts`. A hand-over left standing would then be read with one frame's
   counts over another frame's vertices. Hence the stamp, and hence the
   unconditional clear on every path that does not publish. */

/* FLOATS PER VERTEX, AND THE ATTRIBUTE TABLE, DEFINED ONCE -- tagpu_feat.h's
   rule and for the same reason: the gather that writes the vertices and the
   pass that reads them share this header, and a vertex layout copied into a
   second file is two things that can drift. Each entry is {location,
   components, byte offset}. */
#define TAGPU_FX_VST    9          /* x,y,enc, u,v, c,mode, wx,wz            */
#define TAGPU_FX_NATTR  4
#define TAGPU_FX_ATTRS  { {0,3,0}, {1,2,12}, {2,2,20}, {3,2,28} }
/* VERTICES PER BUCKET PER FRAME -- the gather's own caps, shared so that the
   Vulkan pass can re-check a handed-over count against the very number that
   produced it rather than against a second copy of it. SIZED FOR THE TABLES
   THEY DRAW as those usually fill: a particle is one quad, into UNDER for the
   layers the engine draws before its projectiles (0..6) and into SPRITES for
   the rest (7..9), so each of the two holds the whole particle table; SPRITES
   holds a projectile's two quads (its shadow blob and its own frame) and an
   explosion's one besides. Lines and flashes keep TAGPU_FX_MAXV. IT IS NOT A
   BOUND BY CONSTRUCTION: a composite GAF frame makes one quad per subframe and
   a lightning bolt up to 2 044 line vertices. A quad or line that does not
   fit its bucket is dropped and counted (`fx: DROPPED ... bucket-full` in the
   log), never written past it. */
#define TAGPU_FX_MAXV         65536
#define TAGPU_FX_MAXV_UNDER   (6 * (int)TAGPU_PK_MAX_PART)
#define TAGPU_FX_MAXV_SPRITES (6 * (2 * TAGPU_LIM_PROJ + TAGPU_LIM_EXPL + (int)TAGPU_PK_MAX_PART))

/* THE FOUR BUCKETS, IN DRAW ORDER, which is the order they are concatenated
   into one vertex buffer in. They are not four draws of one
   pipeline: the lines are a LINE_LIST and the flashes blend additively, so
   this pass needs more than one pipeline for the same shader. */
enum { TAGPU_FXB_UNDER = 0, TAGPU_FXB_LINES = 1,
       TAGPU_FXB_FLASH = 2, TAGPU_FXB_SPRITES = 3, TAGPU_FXB_N = 4 };
#define TAGPU_FX_MAXV_OF(b) ((b) == TAGPU_FXB_UNDER   ? TAGPU_FX_MAXV_UNDER   : \
                             (b) == TAGPU_FXB_SPRITES ? TAGPU_FX_MAXV_SPRITES : TAGPU_FX_MAXV)

typedef struct TAGPU_FXHAND {
    /* THE FRAME THIS WAS PUBLISHED ON. `tagpu_fx_handover` refuses any other --
       see the header above for what that bounds, which for this pass is a stale
       READ of live buffers rather than a freed one. */
    unsigned frame;

    /* The geometry. `vert[b]` is bucket b's array and `n[b]` its vertex count,
       TAGPU_FX_VST floats each. */
    const float* vert[TAGPU_FXB_N];
    int          n[TAGPU_FXB_N];
    /* THE MODELS THIS FRAME'S RECORDS CARRY, which the posed pass draws
       (tagpu_vk_unit.c's effects stage). An effect is drawn whole or not at
       all, and its model and its sprites are drawn by two passes, so the two
       agree per frame: this pass draws only when the posed pass is drawing
       exactly this many (`tagpu_vk_unit_fx_count`), and the posed pass draws
       them only when this pass is drawing (`tagpu_vk_fx_models_ok`). */
    int          nmodels;

    /* The vertex stage's uniform block (std140 offsets are printed in
       inc/spirv/tagpu_fx.spv.h and are the contract for the buffer). */
    float gw, gh;
    float zoom, zoomCx, zoomCy;
    float depthScale;

    /* The fragment stage's. `uFog` carries bit1 set for this pass -- effects
       hide in grey rather than darkening -- so it is the number the shader
       takes and not `fogMode & 1`. */
    int   restored;                   /* Classic++ restored atlas in use      */
    int   fog;                        /* uFog, as the shader takes it         */
    float fogOrgX, fogOrgY, fogCols, fogRows;
    float scafP[4];                   /* vpL, vpT, vw, vh, in frame px        */
    float uss;                        /* the supersample factor as the FS sees it */
    float zoomF, zoomCFx, zoomCFy;

    /* The texels, as CPU-side bytes. Each carries the serial that says when
       it last changed, so the Vulkan pass re-uploads on a change and not per
       frame. */
    const unsigned char*  atlas;      /* dim x dim palette indices,
                                         tagpu_gaf.c's mirror: the source of
                                         the base atlas (tagpu_pal_expand)   */
    int                   atlasDim;
    int                   atlasRows;  /* the rows the shelf packer has used   */
    unsigned              atlasSerial;
    unsigned              atlasWhole; /* the last whole-page write (tagpu_feat.h) */
    /* the base atlas's other two inputs -- tagpu_feat.h has them in full */
    const unsigned char*        atlasKey;
    const unsigned*             atlasDirty;

    /* THE RESTORE WORK, AS REQUESTS.

       IT IS AN APPEND-ONLY LIST WITH A CURSOR, not terrain's whole list per
       serial, because a effects atlas is a lazy QUEUE: tagpu_gaf.c adds one
       frame on every miss for the life of the atlas. A consumer keeps its own
       index into `restoreFrames` and takes `[cursor, restoreN)`; a frame on
       which it takes nothing costs nothing, because the entries are still
       there on the next one. `restoreGen` is the discontinuity a cursor cannot
       survive -- the arm, a recycle, a repack, the atlas dropped, the list
       overflowing -- and a consumer that sees a new one drops its job and
       starts at 0. Every generation blanks the destination, for tagpu_feat.h's
       reason.

       LIFETIME: the array is the atlas's, retained for the atlas rather than
       for the frame, but a consumer still copies on the frame it takes it (as
       tagpu_vk_restore_job_add does) -- a restart re-uses the same memory.
       The destination's size is the ATLAS's (`atlasDim` square). */
    const TAGPU_RGLSL_FRAME* restoreFrames;
    int                      restoreN;
    unsigned                 restoreGen;
    const unsigned char*  pal;        /* 256 x RGBA8, tagpu_pal_engine()      */
    unsigned              palSerial;  /* tagpu_pal_engine_serial()            */
    /* THE FLASH LIGHT TABLE, 32 x 1, THREE BYTES A TEXEL (tagpu_fx.c
       `s_lhtRGB`). NULL when it has never been built, and then a frame with
       flash vertices is refused rather than drawn with no colour for its
       flashes. */
    const unsigned char*  lht;        /* 32 x 3 bytes, or NULL                */
    const unsigned short* fogGrid;    /* cols x rows RG8; NULL when fog is off */
    int                   fogGridCols, fogGridRows;

    /* THE SCISSOR THE NATIVE PASS SET AROUND THIS DRAW, in game-frame pixels
       measured from the TOP of the frame. tagpu_vk_feat.c has the argument for
       why the two APIs disagree about it and where the flip is done. */
    int   vpL, vpT, vw, vh;
    int   scissorOn;                  /* the native pass clips this frame     */
    int   ss;                         /* the world target's supersample factor */

    /* 1 on the ONE frame `tagpu_fx.ab` latched its claim and
       `tagpu_vk_ab_arm` got the `_vk.ppm` target unlinked, so the Vulkan lane
       captures THAT frame rather than whichever one its own lever poll landed
       on. It does NOT mean a capture file was written: the capture the seam
       then records is this lane's own. */
    int   ab;
} TAGPU_FXHAND;

/* 0 when there is nothing to draw, when this frame's has already been taken,
   or when the standing hand-over was published on a DIFFERENT frame than
   `now` -- the fork's monotonic render-thread counter, which a Vulkan pass has
   as TAGPU_VKPASS::frame. Render thread only. */
int tagpu_fx_handover(TAGPU_FXHAND* out, unsigned now);

/* THE ATLAS WITHOUT THE DRAW -- tagpu_feat.h's three, for this atlas. */
int  tagpu_fx_atlas_hand(TAGPU_FXHAND* h);
int  tagpu_fx_atlas_owed(void);
void tagpu_fx_atlas_ack(unsigned serial, int keep);
#endif
