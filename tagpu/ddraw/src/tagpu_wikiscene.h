#ifndef TAGPU_WIKISCENE_H
#define TAGPU_WIKISCENE_H
/* tagpu_wikiscene -- the in-game wiki's 3D stage as the world passes draw it: one stage's ground
   and one view's fog and units turned into terrain cells, feature quads, the engine's fog grid and
   a model record per unit and ghost (research/notes/ingame-wiki-3d-stage-plan.md, sections 2, 3,
   6 and 7).

   THE SPECIFICATION IS THE MOCKUP'S. The fog grid is tools/wiki/mockup/scene.js `fogGrid`; the
   cells are tools/tascene-render.js `classicTerrain().gather` and the quads its `gatherFeatures`,
   each run at the stage's camera as tools/wiki/mockup/gfx.js runs them: zoom 1, the eye at the
   map's top left, the viewport the whole map and nothing past its edge. Those two gathers are
   tagpu_terr.c's and tagpu_feat.c's arithmetic as the lab ports it; where the lab and the game's
   feature pass part (the tall key's row base, below), this file takes the lab's, because the
   mockup is what the game must match. tools/wiki/scenecheck.mjs plays every demo through sim.js
   and scene.js and through tagpu_wikisim.c and this file, and pairs every cell, quad and fog
   entry.

   THE STAGE IS FLAT (the authoring tool takes flat tiles only, the plan's section 2), so every
   height the gathers read is 0 and the ground plane under the engine's shear is the whitebox's
   top-down grid: one world unit is one game pixel, and a feature's anchor is where tagpu_feat.c
   puts it with no lift.

   ONE FIT FOR THE COMPOSITE AND THE OPS: this file places everything in game pixels of the stage's
   own target; where the target lands on the screen is tagpu_wikistage_image, from the same
   tagpu_wikistage_fit the stage's ops are built with.

   PURE. It holds no global and reads only its arguments: a stage's ground as numbers (a frame's
   size, hotspot and key, never a pointer into one) and a view's fog. Every index it takes is
   bounded here against its own array, and a bound it cannot meet refuses the ground whole
   (`refused`), never a part of it. Pure C99 and the C library, so tools/wiki/scenecheck.mjs
   compiles it natively. */
#include <stdint.h>
#include "tagpu_wikistage.h"            /* WIKISTAGEVIEW, the record's view of the stage         */

#define WIKISCENE_MAP    128                                     /* cells a side: WIKISTAGE_MAP */
#define WIKISCENE_TILES  ((WIKISCENE_MAP / 2) * (WIKISCENE_MAP / 2))  /* 32-px cells           */
#define WIKISCENE_FEATS  256                                     /* 2D features of one stage    */
#define WIKISCENE_SUBS   8                                       /* sub-frames of one frame     */
#define WIKISCENE_QUADS  1024                                    /* quads of one bucket         */

/* a sprite frame as the gathers read it: its size, its hotspot, its colour key, and whether a
   sub-frame asks for the alpha blit (TAGPU_GF_SUBALP) */
typedef struct {
    int16_t w, h, hx, hy;
    uint8_t ck, subalp;
} WSCENEFRAME;

/* a 2D feature of the ground: its anchor cell, footprint and height (LoadFeature 0x4224B0, the
   arena's WIKI3DFEAT), its def bits (bit 2 animtrans, bit 3 shadtrans), and its body's and
   shadow's frames: 0 frames none, 1 a plain frame, more a compound frame's sub-frames */
typedef struct {
    int16_t        cx, cz, fx, fz, height;
    uint32_t       flags;
    uint8_t        nbody, nshadow;
    WSCENEFRAME body[WIKISCENE_SUBS], shadow[WIKISCENE_SUBS];
} WSCENEFEAT;

typedef struct {
    int                  w, h;          /* the map's cells: the stage's ground is w/2 tiles wide and
                                           at least h/2 deep, of which the map takes the top rows  */
    int                  ntile;         /* the tile set's size: every tile map entry is under it */
    const uint16_t*      tilemap;       /* row-major, w/2 a row: the map's rows are its first    */
    int                  nfeat;
    const WSCENEFEAT* feat;
} WSCENEGROUND;

/* one visible 32-px cell: its column and row in the gather's grid, which at the stage's camera is
   the map's own cell, and its tile's index in the tile set */
typedef struct { int16_t col, row; uint16_t tile; } WSCENECELL;

/* one sprite quad, in game pixels of the stage's target -- which at zoom 1 with the eye at the
   map's corner are world units -- before tagpu_feat.c's edge nudge, which the producer applies */
typedef struct {
    float   x0, y0, x1, y1;
    float   enc;                        /* the depth key (tagpu_feat.c `map_key`)               */
    uint8_t mode;                       /* TAGPU_FXMODE_OPAQUE 1 / _ALPHA 2                      */
    uint8_t sub;                        /* the sub-frame it draws                                */
    int16_t feat;                       /* its feature's index in the ground                     */
    float   wax, waz;                   /* the feature's anchor, world units                     */
} WSCENEQUAD;

typedef struct {
    int           refused;              /* a bound the ground could not meet: draw none of it    */
    int           gw, gh;               /* the target's game pixels: w*16 x h*16                 */
    int           ncell;
    WSCENECELL cell[WIKISCENE_TILES];
    int           nshadow, nbody;       /* the two buckets, in draw order: shadows, then bodies  */
    WSCENEQUAD shadow[WIKISCENE_QUADS], body[WIKISCENE_QUADS];
    int16_t       anchor[WIKISCENE_MAP * WIKISCENE_MAP];    /* the build's: cell -> feature    */
} WSCENE;                            /* about 130 KB: never on a stack                        */

/* the engine's fog grid for one view: one entry a 32-unit block, two 4-bit corner masks */
#define WIKISCENE_FOGMAX ((WIKISCENE_MAP / 2) * (WIKISCENE_MAP / 2))
typedef struct {
    int      on;                        /* a cell is fogged: draw with the grid; 0 none          */
    int      cols, rows;                /* blocks: w/2 x h/2                                     */
    float    orgx, orgz;                /* the world point of block (0,0)'s corner sample: 16,16 */
    uint16_t grid[WIKISCENE_FOGMAX];    /* low byte b0 unexplored, high byte b1 grey (RG8)       */
} WSCENEFOG;

/* ---- the model records: every unit and every ghost the view shows (the plan's section 6) ---- */

/* what the records need of each of the view's unit types (index = the view's type index),
   which the caller resolves: the stage's producer from the 3D arena, the check from the
   mockup's pack */
typedef struct {
    uint8_t ok;                         /* its model is drawn in 3D: else its box is, and no record */
    uint8_t mobile, slant, shadow;      /* WIKI3DUNIT's FBI facts                                   */
} WSCENETYPE;

/* the Classic shadow a record casts before the engine's option bits, which its drawer applies:
   tagpu_posedraw.h's TAGPU_PDSH_* */
#define WSCENE_SH_NONE  0
#define WSCENE_SH_SIL   1               /* a mobile unit's silhouette                              */
#define WSCENE_SH_SLANT 2               /* a structure's slant                                     */

/* A 3D FEATURE OF THE GROUND (a wreck, a heap: its section names a model), which the unit pass
   draws: its anchor cell and footprint, in the stage description's order */
#define WIKISCENE_WRECKS 64             /* WIKIWORLD_FEAT3D: the stage's 3D features            */
typedef struct { int16_t cx, cz, fx, fz; } WSCENEWRECK;

typedef struct {
    uint8_t  type;                      /* the view's type index; a wreck's, its index in the
                                           caller's WSCENEWRECK list                               */
    uint8_t  wreck;                     /* a 3D feature: owner 0, unturned, unlit, no shadow       */
    uint8_t  owner;                     /* its team colour: the frame of a team face, 0..7         */
    uint8_t  ghost;                     /* a build ghost: alpha 0.40, depth writes off, no shadow  */
    uint8_t  shadow;                    /* WSCENE_SH_*                                             */
    uint8_t  nanoOn;                    /* a nanoframe, staged by nanoT, nanoC and nanoWire        */
    int16_t  slot;                      /* a unit's index in the view, -1 for a ghost              */
    float    x, z;                      /* its anchor, world units: on the flat stage, target px   */
    float    cs, sn;                    /* its yaw: the model's +z turned onto its facing          */
    float    enc;                       /* its depth row's key: 1 + 4 (z >> 4), a wreck's 3 + 4 row */
    float    alpha;
    float    nanoT, nanoC[3], nanoWire; /* tagpu_nanostage.h, when nanoOn                          */
} WSCENEMODEL;

#define WIKISCENE_MODELS (WIKISTAGE_UNITS + WIKISCENE_WRECKS + 1 + WIKISTAGE_SITES + WIKISTAGE_QUEUED)

/* THE RECORDS of view `v` at its tick, into `out` (WIKISCENE_MODELS of them): the units first, in
   the view's order, then the ground's 3D features (`wrecks`, `nwreck` of them, at most
   WIKISCENE_WRECKS) whose anchor cell the view has not reclaimed -- `*nunit` records together,
   the opaque window's -- then the ghosts in tagpu_native.c `ghost_record`'s order: the placement
   ghost, each gesture site's, each queued site's not yet started. A unit's record is made only for
   a type `types` marks ok (`ntype` of them; the rest are boxes) and an owner of the eight. Returns
   the count. tools/wiki/mockup/scene.js `build` is the specification. */
int  tagpu_wikiscene_models(const WIKISTAGEVIEW* v, const WSCENETYPE* types, int ntype,
                            const WSCENEWRECK* wrecks, int nwreck, WSCENEMODEL* out, int* nunit);

/* THE GROUND: `g`'s cells and quads into `out`. Returns 1 when the ground can be drawn whole, 0
   with out->refused set when it cannot -- a map past WIKISCENE_MAP, a tile map entry past the
   set, a feature left of, above or right of the map or past a bound -- and then nothing of it is
   to be drawn. A feature whose footprint ends below the map is not on it, and left out. */
int  tagpu_wikiscene_ground(const WSCENEGROUND* g, WSCENE* out);

/* THE FEATURES RECLAIMED: dead[k] (WIKISCENE_FEATS of them) set for each feature k of the ground
   `sc` whose anchor cell is one of the `ngone` cells `gone` (WIKISTAGEVIEW.gone); a quad of a dead
   feature is not drawn. gfx.js drops the same cells from the anchors its gather sweeps, and the
   sweep's order is the quads' order, so leaving those quads out is that gather. A cell off the
   ground names none. Returns how many are set. */
int  tagpu_wikiscene_dead(const WSCENE* sc, const int16_t (*gone)[2], int ngone, uint8_t* dead);

/* THE FOG: a view's `fog` (w x h cells, 0 clear, 1 grey, 2 black; WIKISTAGEVIEW.fog) into `out`.
   The stage's fog rects are whole tiles, so each 32-unit block is uniform and its top-left cell
   stands for it. out->on is 0 when no cell is fogged. */
void tagpu_wikiscene_fog(const uint8_t* fog, int w, int h, WSCENEFOG* out);

#endif
