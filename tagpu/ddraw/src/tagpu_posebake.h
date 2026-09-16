#ifndef TAGPU_POSEBAKE_H
#define TAGPU_POSEBAKE_H
/* tagpu_posebake.h — the per-type geometry bake and its caches (G16 step 4).

   research/notes/gpu-posing.md is the design; this is the half of it that
   turns a `Model3DONode` TEMPLATE into two GL vertex buffers that a posed
   shader can draw a unit from without ever reading the engine's posed vertex
   buffer `prim+0x22`.

   THE SPLIT (gpu-posing.md decision 4) is between what a type owns and what a
   type-plus-owner owns:

     GEOMETRY, per type — rest position, the rest normal of the vertex's own
       triangle, its piece index, and a flags word. Static: it survives a team
       change and an atlas recycle, and it dies only with the LEVEL or the GL
       context.
     MATERIAL, per (type, owner, atlas generation) — the UV, the flat colour
       and colour key, and a SKIP flag. `face_texframe` picks
       `tab + owner*0x18` for team-coloured faces, and every UV in the atlas
       moves when the shelf recycles, so both belong in the key.

   THE TWO BUFFERS ALWAYS HOLD THE SAME NUMBER OF VERTICES, which is what makes
   them independently rebuildable: a face the engine's rasteriser paints nothing
   for is baked anyway and collapsed by its skip flag, rather than changing the
   vertex count the way `emit_node`'s `continue` does today.

   THREE RANGES IN ONE GEOMETRY BUFFER (decision 10), because all three walk the
   same tree and differ only in which faces and which corners they take:

       [ body triangles ][ slant triangles ][ wire lines ]

   Nothing DRAWS from these yet — the posed program is step 5. What step 4
   delivers is the bake, the caches, their four invalidation triggers and a
   lever. Its `check` token is gone with G16 step 8: it compared the bake
   against the CPU emitters, and they no longer exist. `log` still works. */

#include "tagpu_model3do.h"      /* TAGPU_PBMAXPIECE, and the field offsets */

#define TAGPU_PB_GEOMST  8      /* floats per geometry vertex */
#define TAGPU_PB_MATST   5      /* floats per material vertex */

/* geometry vertex flags (float, bit-tested in the shader as an int) */
#define TAGPU_PBF_SHADED 1      /* body face with a usable rest normal        */

enum { TAGPU_PB_BODY = 0, TAGPU_PB_SLANT, TAGPU_PB_WIRE, TAGPU_PB_NRANGE };

/* THE POSED PROGRAM'S `Pose` BLOCK, std140, SHARED BY BOTH LANES. It lives
   here rather than in tagpu_posedraw.h because every number in it is the
   bake's piece ceiling: 3 rows of a 4x3 per piece, then two packed per-piece
   words four to a vec4. tagpu_posedraw.c builds the GLSL declaration from
   these and tagpu_vk_unit.c fills the buffer from them, so the two cannot
   drift -- which is rule 4 of this lane ("re-check every bound in the
   consuming file, and share the constant through the header"). */
#define TAGPU_PD_ROWS    (TAGPU_PBMAXPIECE * 3)              /* 768 vec4     */
#define TAGPU_PD_FLAGV   (TAGPU_PBMAXPIECE / 4)              /*  64 vec4     */
#define TAGPU_PD_FLAGOFF (TAGPU_PD_ROWS * 16)                /* bytes        */
#define TAGPU_PD_VISOFF  ((TAGPU_PD_ROWS + TAGPU_PD_FLAGV) * 16)
#define TAGPU_PD_BLOCK   ((TAGPU_PD_ROWS + TAGPU_PD_FLAGV * 2) * 16)  /* 14336 */

typedef struct TAGPU_PBGEOM {
    const char*  root;                        /* Model3DONode* of primitive 0 */
    unsigned     levelGen, glGen;
    int          nparts;
    int          ghost;                       /* 1 = baked from the ghost's
        synthesized piece run. Its walk order is the template tree's, which is
        NOT the prim order a live unit's packet run carries, and the VBO's
        per-vertex piece indices plus `parent[]` are laid out in that order —
        so a ghost entry and a unit entry of the same model are DIFFERENT
        geometry and must never share a cache slot (2026-09-12 leak). */
    /* the topology, cached per type: `pose_accum_body` rebuilds parent links by
       scanning the node list for every sibling of every node, which is fine on
       today's rare trip frames and not fine at 200 units a frame */
    short        parent[TAGPU_PBMAXPIECE];
    float        restOff[TAGPU_PBMAXPIECE][3];
    unsigned char done[TAGPU_PBMAXPIECE];     /* 0 = parent link never resolved */
    int          first[TAGPU_PB_NRANGE];      /* vertex offsets of the ranges  */
    int          count[TAGPU_PB_NRANGE];
    int          nvert;
    unsigned int vbo;
    /* ONE MONOTONIC NUMBER PER BAKE, never reused (Phase G / G19e, the unit
       pass). A cache slot IS reused -- `geom_slot` evicts the least recently
       asked-for entry and re-bakes another type into it -- so a second backend
       that keyed its own vertex buffer on the slot, or on this pointer, would
       hand the new model the old model's vertices. The serial makes "the same
       geometry" a property of the bake rather than of where it landed. */
    unsigned     serial;
    /* the BODY range's rest AABB per piece, and whether the piece contributed
       any body vertex at all. G16 step 5 replaces `s_emitTop` — which emit_node
       took from the posed vertices it was writing — with a CPU walk of these 8
       corners through the piece's pose matrix (gpu-posing.md §4, "What stops
       being true"). Two stated deviations from what emit_node produced: an
       AABB carried through a rotation BOUNDS the posed points rather than
       hitting them, so the top is an over-estimate; and it covers every body
       face, including the ones whose material the stream collapses, which
       emit_node skipped before it ever looked at their y. It feeds the shadow
       height of WRECKS only — a unit with a record prefers `model_aabb`. */
    float        pmn[TAGPU_PBMAXPIECE][3];
    float        pmx[TAGPU_PBMAXPIECE][3];
    unsigned char pbody[TAGPU_PBMAXPIECE];   /* 0 = no body vertex baked      */
    /* THREE COUNTS, NOT ONE. gpu-posing.md §3 listed "a face with neither a
       texture nor a colour", "a node whose vertex array does not read" and "a
       piece whose parent never resolves" together as the anomaly to log once
       per model. Measured over a 69-unit inventory they are not the same kind
       of thing at all: EVERY one of 67 models has material-less faces (2.7% of
       all baked vertices) and 14 have faces the emitters skip for their vertex
       count, while nothing in stock content produced an unreadable node or an
       orphan piece. So the ordinary two are statistics and only the last two
       are anomalies; reporting them as one number cried wolf 1486 times. */
    int          refused;      /* over the vertex bound: remembered, not re-walked */
    int          badNode;      /* a piece whose node or vertex array does not read */
    int          orphan;       /* a piece whose parent link never resolved        */
    int          oddFace;      /* a face the emitters skip: fvc outside [3,32],
                                  or indices that do not read                    */
    unsigned     lastFrame;                   /* for eviction                 */
} TAGPU_PBGEOM;

typedef struct TAGPU_PBMAT {
    const TAGPU_PBGEOM* geom;
    const char*  root;        /* the geometry entry's key, carried again so a
                                 material can never be matched against a slot
                                 that has since been evicted and re-baked */
    int          owner;
    unsigned     atlasGen, levelGen, glGen;
    unsigned int vbo;
    /* the posed pass's VAO, binding this stream and its geometry together
       (locations 0-3 from the geometry, 4-6 from here). It lives on the
       MATERIAL entry because that is the shorter life of the two: a geometry
       drop cascades into every stream that names it, so the VAO can never
       outlive either buffer it points at. */
    unsigned int vao;
    unsigned     serial;         /* as the geometry entry's, and for the same
                                    reason: `mat_slot` recycles slots too      */
    int          nvert, nskip;   /* nskip: vertices the skip flag collapses    */
    int          noMaterial;     /* faces with neither a texture nor a colour  */
    unsigned     lastFrame;
} TAGPU_PBMAT;

/* Render thread, once per frame, before any tagpu_posebake_unit: drops every
   entry whose level generation, GL generation or atlas generation has moved,
   deleting its GL buffers (which is why this is render-thread only). */
/* `level_gen` is THE PACKET'S — it advances at every level end whoever
   published it, where tagpu_reclaim's own counter moves only when reclaim is
   armed (landing review, 2026-09-12). */
void tagpu_posebake_frame(unsigned frame_counter, unsigned level_gen);

/* Bake (or find) the geometry and the material stream for one unit's type.
   `pc` is the unit's PK_PIECE run out of the frame packet and supplies the
   piece list — each entry's `node` is the TYPE's template, so the entry is
   shared by every unit of it. `ghost` is 1 for the build ghost's synthesized
   run and keys the geometry entry APART from the units' (see the field).
   Returns 0 when the template could not be read or GL is not ready; a caller
   that gets 0 keeps whatever it was doing. */
struct TAGPU_PK_PIECE;
int  tagpu_posebake_unit(const struct TAGPU_PK_PIECE* pc, int nparts, int owner,
                         int ghost,
                         const TAGPU_PBGEOM** geom, const TAGPU_PBMAT** mat);

/* The vertex count `emit_geom` should produce for THIS unit out of this bake:
   the body range, minus the faces whose material the engine has nothing for,
   minus the pieces this unit is not showing. The lever compares the two. */

int  tagpu_posebake_armed(void);         /* tagpu_posebake.on                 */

/* ---- THE VULKAN LANE'S MIRRORS (Phase G / G19e, the UNIT pass) -----------

   A second backend cannot read a GL buffer, so the two streams `glBufferData`
   is handed are KEPT while the Vulkan lane is armed -- the same latch and the
   same reasoning as tagpu_terr.c's `s_mirrorWant` and tagpu_gaf.c's atlas
   mirror. The mirror is the very buffer the upload above it was given, in the
   same call, so it is correct from the instant it exists and there is no
   second evaluation of the bake's arithmetic to drift from the first.

   ASK FOR THE BYTES WITH THE SERIAL YOU WERE PUBLISHED, and this is the whole
   of the lifetime argument. The entry a `TAGPU_PBGEOM*` points at is a slot in
   a fixed array that `geom_slot` evicts and re-bakes into, and `geom_drop`
   frees the mirror with it. So the accessor below bounds the POINTER against
   its own array (inside it, and on an entry boundary) and then checks the
   serial, which is unique for the life of the process: a pointer into a slot
   that has since been recycled answers NULL rather than another model's
   vertices. That makes "these bytes are this model's" a property checked at
   the instant of the read, by the module that owns them, rather than a
   deduction about which functions have run since.

   NULL when the lane never asked for mirrors, when the entry has been
   re-baked, or when the malloc was refused -- and a refused malloc is not an
   error here: the caller draws nothing for that unit, which is a frame the
   Vulkan lane stands down on rather than a frame it draws wrong. */
const float* tagpu_posebake_geom_mirror(const TAGPU_PBGEOM* g, unsigned serial,
                                        int* nvert);
const float* tagpu_posebake_mat_mirror(const TAGPU_PBMAT* m, unsigned serial,
                                       int* nvert);

void tagpu_posebake_glreset(void);       /* the GL context went              */
/* one `bake=` field for the native: line; writes nothing when disarmed */
int  tagpu_posebake_stats(char* out, int n);
#endif
