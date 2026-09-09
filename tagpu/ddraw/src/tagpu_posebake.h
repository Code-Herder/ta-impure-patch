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
   lever that checks the bake against the emitters it is going to replace. */

/* The piece bound the bake and the pose share. `TAGPU_HMAXPIECE` (48) is the
   replacement-mesh program's UNIFORM ARRAY size and stays where it is; this one
   is a plain array bound with no GL cost, so it is set an order of magnitude
   above the largest stock model (36 pieces, `armscorp`, measured over all 608
   models of the stock objects3d tree) rather than at a number the design has to
   reason about. */
#define TAGPU_PBMAXPIECE 256

#define TAGPU_PB_GEOMST  8      /* floats per geometry vertex */
#define TAGPU_PB_MATST   5      /* floats per material vertex */

/* geometry vertex flags (float, bit-tested in the shader as an int) */
#define TAGPU_PBF_SHADED 1      /* body face with a usable rest normal        */

enum { TAGPU_PB_BODY = 0, TAGPU_PB_SLANT, TAGPU_PB_WIRE, TAGPU_PB_NRANGE };

typedef struct TAGPU_PBGEOM {
    const char*  root;                        /* Model3DONode* of primitive 0 */
    unsigned     levelGen, glGen;
    int          nparts;
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
    /* THREE COUNTS, NOT ONE. gpu-posing.md §3 listed "a face with neither a
       texture nor a colour", "a node whose vertex array does not read" and "a
       piece whose parent never resolves" together as the anomaly to log once
       per model. Measured over a 69-unit inventory they are not the same kind
       of thing at all: EVERY one of 67 models has material-less faces (2.7% of
       all baked vertices) and 14 have faces the emitters skip for their vertex
       count, while nothing in stock content produced an unreadable node or an
       orphan piece. So the ordinary two are statistics and only the last two
       are anomalies; reporting them as one number cried wolf 1486 times. */
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
    int          nvert, nskip;   /* nskip: vertices the skip flag collapses    */
    int          noMaterial;     /* faces with neither a texture nor a colour  */
    unsigned     lastFrame;
} TAGPU_PBMAT;

/* Render thread, once per frame, before any tagpu_posebake_unit: drops every
   entry whose level generation, GL generation or atlas generation has moved,
   deleting its GL buffers (which is why this is render-thread only). */
void tagpu_posebake_frame(unsigned frame_counter);

/* Bake (or find) the geometry and the material stream for one unit's type.
   `o3` supplies the piece list — the node pointers are the type's, so the entry
   is shared by every unit of it. Returns 0 when the template could not be read
   or GL is not ready; a caller that gets 0 keeps whatever it was doing. */
int  tagpu_posebake_unit(const char* o3, int owner,
                         const TAGPU_PBGEOM** geom, const TAGPU_PBMAT** mat);

/* The vertex count `emit_geom` should produce for THIS unit out of this bake:
   the body range, minus the faces whose material the engine has nothing for,
   minus the pieces this unit is not showing. The lever compares the two. */
int  tagpu_posebake_predict_body(const TAGPU_PBGEOM* g, const TAGPU_PBMAT* m,
                                 const char* o3);

int  tagpu_posebake_armed(void);         /* tagpu_posebake.on                 */
int  tagpu_posebake_checking(void);      /* ...with `check` in it             */
void tagpu_posebake_glreset(void);       /* the GL context went              */
/* one `bake=` field for the native: line; writes nothing when disarmed */
int  tagpu_posebake_stats(char* out, int n);
/* the lever's cross-check, called once per unit right after emit_geom */
void tagpu_posebake_check(const char* o3, const TAGPU_PBGEOM* g,
                          const TAGPU_PBMAT* m, int emitted,
                          const float* accRest, int naccRest);
#endif
