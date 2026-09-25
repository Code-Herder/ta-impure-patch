#ifndef TAGPU_POSEBAKE_H
#define TAGPU_POSEBAKE_H
/* tagpu_posebake.h — the per-type geometry bake and its caches.

   research/notes/gpu-posing.md is the design; this is the half of it that
   turns a `Model3DONode` TEMPLATE into two vertex streams that a posed
   shader can draw a unit from without ever reading the engine's posed vertex
   buffer `prim+0x22`.

   THE SPLIT (gpu-posing.md decision 4) is between what a type owns and what a
   type-plus-owner owns:

     GEOMETRY, per type — rest position, the rest normal of the vertex's own
       triangle, its piece index, and a flags word. Static: it survives a team
       change and an atlas recycle, and it dies only with the LEVEL or when the
       cache evicts it.
     MATERIAL, per (type, owner, atlas generation) — the UV, the flat colour
       and colour key, and a SKIP flag. `face_texframe` picks
       `tab + owner*0x18` for team-coloured faces, and every UV in the atlas
       moves when the shelf recycles, so both belong in the key.

   THE TWO STREAMS ALWAYS HOLD THE SAME NUMBER OF VERTICES, which is what makes
   them independently rebuildable: a face the engine's rasteriser paints nothing
   for is baked anyway and collapsed by its skip flag, rather than changing the
   vertex count.

   THREE RANGES IN ONE GEOMETRY STREAM (decision 10), because all three walk the
   same tree and differ only in which faces and which corners they take:

       [ body triangles ][ slant triangles ][ wire lines ]

   What this module holds is the bake, the caches, their four invalidation
   triggers and a lever (`log`). */

#include "tagpu_model3do.h"      /* TAGPU_PBMAXPIECE, and the field offsets */
#include "tagpu_packet.h"        /* the bound on one frame's posed objects */

#define TAGPU_PB_GEOMST  8      /* floats per geometry vertex */
#define TAGPU_PB_MATST   5      /* floats per material vertex */

/* THE BAKE'S TWO CACHES MUST HOLD EVERY ENTRY ONE FRAME DRAWS: an entry
   evicted mid-frame costs that frame objects, silently (content-ids.md
   measures the frame's last objects not drawn and nothing logged). A frame
   asks for at most one geometry entry and one material stream per posed
   object, and the frame packet bounds those: every unit its table holds,
   every wreck, and a ghost for every queued build site plus the cursor's. So
   each cache holds that many, and a slot is never taken from an entry this
   frame has asked for (tagpu_posebake.c `geom_slot`, `mat_slot`): together
   they make the mid-frame eviction unreachable, whatever the content.

   THE KEEP IS WHERE RECYCLING STARTS, NOT A CEILING BETWEEN FRAMES. Below it a
   new entry takes a free slot; from it on, a new entry evicts the least
   recently used entry this frame has not asked for and takes its slot. So a
   cache holds the most one frame of the level has asked for, at least the
   keep, until the level change drops it: the recycling is what stops a scene
   larger than the keep from re-baking every frame. A geometry entry is one
   MODEL, not one type: a wreck's model and a ghost's split entry take slots of
   their own, which is why the keep is twice stock's 279 types; a material
   stream is one (type, owner). Here as well as in tagpu_posebake.c because
   tagpu_vk_unit.c sizes its vertex-buffer table from them. */
#define TAGPU_PB_FRAMEMAX (TAGPU_PK_MAX_UNITS + TAGPU_PK_MAX_WRECKS + TAGPU_PK_MAX_BUILDS + 1u)
#define TAGPU_PB_MAXGEOM  ((int)TAGPU_PB_FRAMEMAX)   /* models cached at once        */
#define TAGPU_PB_MAXMAT   ((int)TAGPU_PB_FRAMEMAX)   /* (type, owner) streams        */
#define TAGPU_PB_KEEPGEOM  512
#define TAGPU_PB_KEEPMAT  1024

/* geometry vertex flags (float, bit-tested in the shader as an int) */
#define TAGPU_PBF_SHADED 1      /* body face with a usable rest normal        */

enum { TAGPU_PB_BODY = 0, TAGPU_PB_SLANT, TAGPU_PB_WIRE, TAGPU_PB_NRANGE };

/* THE POSED PROGRAM'S `Pose` STORAGE BUFFER. It lives here rather than in
   tagpu_posedraw.h because every bound in it is the bake's piece ceiling.
   One `vec4` array a frame, in three sections that follow one another:

     rows   every posed unit's pose, 3 vec4 a piece (the rows of a 4x3)
     flags  every unit's `shaded` words, one float a piece, packed 4 to a vec4
     vis    every unit's visibility words, laid out exactly as `flags`

   which is the hand-over's `rows`, `flags` and `vis` arrays copied whole. A
   unit reaches its slices through three base indices in its uniform block;
   a unit's word run is padded to a whole vec4, so each unit's starts on one.
   tagpu_posedraw.c declares the buffer and tagpu_vk_unit.c fills it, and both
   take their numbers from here -- rule 4 of this lane ("re-check every bound
   in the consuming file, and share the constant through the header"). */
#define TAGPU_PD_FLAGV   (TAGPU_PBMAXPIECE / 4)   /* one unit's word vec4s at the ceiling: 64 */
/* one unit's pose at the piece ceiling, in bytes: 256 x 48 of rows plus the two
   word runs, 14 336. What the device must be able to bind for one unit. */
#define TAGPU_PD_UNITMAX ((TAGPU_PBMAXPIECE * 3 + TAGPU_PD_FLAGV * 2) * 16)

typedef struct TAGPU_PBGEOM {
    const char*  root;                        /* Model3DONode* of primitive 0 */
    unsigned     levelGen;
    int          nparts;
    int          ghost;                       /* 1 = baked from the ghost's
        synthesized piece run. Its walk order is the template tree's, which is
        NOT the prim order a live unit's packet run carries, and the geometry
        stream's per-vertex piece indices plus `parent[]` are laid out in that order —
        so a ghost entry and a unit entry of the same model are DIFFERENT
        geometry and must never share a cache slot. */
    /* the topology, cached per type: `pose_accum_body` rebuilds parent links by
       scanning the node list for every sibling of every node, which is fine on
       today's rare trip frames and not fine at 200 units a frame */
    /* nparts entries each, in one block the entry owns and its drop frees
       (restOff is its start), so an entry costs its own model's pieces and not
       the piece ceiling's */
    short*       parent;
    float      (*restOff)[3];
    unsigned char* done;                      /* 0 = parent link never resolved */
    int          first[TAGPU_PB_NRANGE];      /* vertex offsets of the ranges  */
    int          count[TAGPU_PB_NRANGE];
    int          nvert;
    /* ONE MONOTONIC NUMBER PER BAKE, never reused (Phase G, the unit
       pass). A cache slot IS reused -- `geom_slot` evicts the least recently
       asked-for entry and re-bakes another type into it -- so a consumer
       that keyed its own vertex buffer on the slot, or on this pointer, would
       hand the new model the old model's vertices. The serial makes "the same
       geometry" a property of the bake rather than of where it landed. */
    unsigned     serial;
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
    unsigned     atlasGen, levelGen;
    unsigned     serial;         /* as the geometry entry's, and for the same
                                    reason: `mat_slot` recycles slots too      */
    int          nvert, nskip;   /* nskip: vertices the skip flag collapses    */
    unsigned     lastFrame;
} TAGPU_PBMAT;

/* Render thread, once per frame, before any tagpu_posebake_unit: drops every
   entry whose level generation or atlas generation has moved, freeing its
   mirrors (which is why this is render-thread only: the Vulkan unit pass reads
   them there). */
/* `level_gen` is THE PACKET'S — it advances at every level end whoever
   published it, where tagpu_reclaim's own counter moves only when reclaim is
   armed. */
void tagpu_posebake_frame(unsigned frame_counter, unsigned level_gen);

/* Bake (or find) the geometry and the material stream for one unit's type.
   `pc` is the unit's PK_PIECE run out of the frame packet and supplies the
   piece list — each entry's `node` is the TYPE's template, so the entry is
   shared by every unit of it. `ghost` is 1 for the build ghost's synthesized
   run and keys the geometry entry APART from the units' (see the field).
   Returns 0 when the template could not be read or the unit atlas is not
   ready (tagpu_r3d_ready); a caller that gets 0 keeps whatever it was
   doing. */
struct TAGPU_PK_PIECE;
int  tagpu_posebake_unit(const struct TAGPU_PK_PIECE* pc, int nparts, int owner,
                         int ghost,
                         const TAGPU_PBGEOM** geom, const TAGPU_PBMAT** mat);

/* ---- THE VULKAN LANE'S MIRRORS (Phase G, the UNIT pass) -----------------

   The two streams the bake produces are KEPT as mirrors once the latch has
   armed -- the same latch and the same reasoning as tagpu_terr.c's
   `s_mirrorWant` and tagpu_gaf.c's atlas mirror. The mirror is a copy of the
   walk's own output, taken in the same call, so it is correct from the instant
   it exists and there is no second evaluation of the bake's arithmetic to
   drift from the first.

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
#endif
