#ifndef TAGPU_FX_H
#define TAGPU_FX_H
/* Effects pass (weapon fire, explosions, debris) — gathered from the live
   engine arrays the two engine passes read (projectiles 0x49BE60, explosions
   0x420B00), rendered natively into the native pass's FBO. Armed by
   tagpu_fx.on. See research/notes/effects.md. */
#include "tagpu.h"
#include "tagpu_restoreglsl.h"   /* TAGPU_RGLSL_FRAME, the restore request */

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
    float encSprite, depthScale; /* this frame's sprite depth key and VS scale */
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

/* one 3DO node to emit through the native geometry path */
typedef struct TAGPU_FXMODEL {
    const char* node;            /* Model3DONode*: the per-TYPE template the
                                    PUBLISHER resolved. tagpu_fx.c only carries
                                    it; tagpu_native.c's emit_fx_model is the
                                    (fenced) file that walks it, under
                                    tagpu_reclaim's teardown fence              */
    float ax, ay;                /* anchor in frame px (engine projection)     */
    float wx, wz;                /* world x, projected world z (fog lookup)    */
    short turn[3];               /* engine rotation triple (65536 = 360 deg)   */
    int   owner;
} TAGPU_FXMODEL;

#define TAGPU_FXMODE_FLAT   0
#define TAGPU_FXMODE_OPAQUE 1
#define TAGPU_FXMODE_ALPHA  2
#define TAGPU_FXMODE_FLASH  3

int  tagpu_fx_armed(unsigned frame_counter);    /* tagpu_fx.on present (30f) */
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
/* VERTICES PER BUCKET PER FRAME -- the gather's own `MAXFXV`, shared so that
   the Vulkan pass can re-check a handed-over count against the very number that
   produced it rather than against a second copy of it. */
#define TAGPU_FX_MAXV   32768

/* THE FOUR BUCKETS, IN DRAW ORDER, which is the order they are concatenated
   into one vertex buffer in. They are not four draws of one
   pipeline: the lines are a LINE_LIST and the flashes blend additively, so
   this pass needs more than one pipeline for the same shader. */
enum { TAGPU_FXB_UNDER = 0, TAGPU_FXB_LINES = 1,
       TAGPU_FXB_FLASH = 2, TAGPU_FXB_SPRITES = 3, TAGPU_FXB_N = 4 };

typedef struct TAGPU_FXHAND {
    /* THE FRAME THIS WAS PUBLISHED ON. `tagpu_fx_handover` refuses any other --
       see the header above for what that bounds, which for this pass is a stale
       READ of live buffers rather than a freed one. */
    unsigned frame;

    /* The geometry. `vert[b]` is bucket b's array and `n[b]` its vertex count,
       TAGPU_FX_VST floats each. */
    const float* vert[TAGPU_FXB_N];
    int          n[TAGPU_FXB_N];

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
    const unsigned char*  atlas;      /* dim x dim R8, tagpu_gaf.c's mirror   */
    int                   atlasDim;
    int                   atlasRows;  /* the rows the shelf packer has used   */
    unsigned              atlasSerial;
    /* the base atlas's other two inputs -- tagpu_feat.h has them in full */
    const unsigned char*        atlasKey;
    const struct TAGPU_GAFBAND* atlasBands;

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
    const unsigned char*  fogLut;     /* 256 x R8, tagpu_native_foglut()      */

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
#endif
