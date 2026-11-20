#ifndef TAGPU_POSEBAKE_H
#define TAGPU_POSEBAKE_H
/* tagpu_posebake.h — the per-type geometry bake and its caches.

   research/notes/gpu-posing.md is the design; this is the half of it that
   turns a `Model3DONode` TEMPLATE into two vertex streams that a posed
   shader can draw a unit from without ever reading the engine's posed vertex
   buffer `prim+0x22`.

   THE SPLIT (gpu-posing.md decision 4) is between what a type owns and what a
   type-plus-owner owns:

     GEOMETRY, per type — rest position, the engine's vertex normal (the mean
       of the vertex's faces' normals, at rest), its piece index, and a flags word. Static: it survives a team
       change and an atlas recycle, and it dies only with the LEVEL or when the
       cache evicts it.
     MATERIAL, per (type, owner, atlas generation, animated frames) — the UV, the flat colour
       and colour key, a SKIP flag, and a textured quad's shape and frame
       rect. `face_texframe` picks
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

/* floats per geometry vertex:
     0-2  position          the rest vertex
     3-5  normal            the engine's mean normal of THIS vertex (body), level (slant, wire)
     6    piece             the pose index
     7    flags             TAGPU_PBF_*
     8-13 corner normals    twelve SNORM16 -- a quad's four corners in the face's index
                            order, xyz each; any other face repeats this vertex's four times */
#define TAGPU_PB_GEOMST  14
/* floats per material vertex:
     0-1  uv                the atlas corner this fan slot takes
     2-3  flat index, key   (flat idx/255, -1) or (0, tex ck/255)
     4    skip              1 = collapsed (a face the engine paints nothing for)
     5-6  quad point        this corner's position in its quad's own frame
     7-8  quad shape (a, b) the fourth corner in that frame, flat across the face
     9-12 quad rect         u0 v0 u1 v1 for a face the fragment stage maps
                            per pixel, (-1, -1, -1, -1) for one that keeps uv
   tagpu_posebake.c `quad_frame` says what the frame is and why 5-12 exist. */
#define TAGPU_PB_MATST   13

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
   their own; a material stream is one (type, owner, frames its animated faces
   show). Here as well as in tagpu_posebake.c because tagpu_vk_unit.c sizes its
   vertex-buffer table from them.

   THE KEEPS COVER WHAT A VIEW ASKS FOR OVER ITS ANIMATIONS' CYCLES, NOT ONE
   FRAME OF IT. A stream evicted while its animation frame is a few ticks from
   coming round again is baked and sent again each time it does. MEASURED, a
   ten-seat Total Mayhem battle with the whole map in view
   (scenarios/zombie-kingdom-13k): at 512 and 1 024 both caches stood at their
   keeps and baked 9 409 entries in two minutes, dropping 7 959; with room they
   settle at 490 models and 1 760 streams, 941 of those an animation frame's,
   and bake 2 266. A kept entry costs the process its side blocks -- some 19 KB
   a model and 5 KB a stream there -- and the device one buffer and one
   allocation. The two keeps sum to 3 072, which leaves the buffers a frame
   retires and the lane's own allocations their room under the 4 096 a device
   must allow; what holds the total there is tagpu_vk_unit.c's `s_vbAlloc`.

   THE WIKI'S INSTANCE SETS ALL THREE BEFORE THIS HEADER (tagpu_scene.h), from the
   stage's own bounds; the game's takes the values below.

   A KEEP ABOVE ITS TABLE WOULD UNDO THE BOUND: the table would fill without the
   keep ever being reached, and the next entry would find neither an entry to
   evict nor a free slot. Hence the assertion. */
#ifndef TAGPU_PB_FRAMEMAX
#define TAGPU_PB_FRAMEMAX (TAGPU_PK_MAX_UNITS + TAGPU_PK_MAX_WRECKS + TAGPU_PK_MAX_BUILDS + 1u)
#define TAGPU_PB_KEEPGEOM   768
#define TAGPU_PB_KEEPMAT   2304
/* 0: a stream is held until its consumer has sent it (A STREAM IS HELD UNTIL IT IS SENT,
   below). The wiki's instance sets 1: the tech tree's view poses its picture from the
   streams every frame (tagpu_wikiview.c), and its stage bounds how many there are. */
#define TAGPU_PB_KEEPSTREAMS 0
#endif
/* The streams the consumer may owe before the bake waits for it: what one frame can add to
   the process over what it holds. A stream past it is one more frame away, so a view of
   many types seen for the first time fills in over the frames this divides it into
   (MEASURED: a ten-seat map zoomed out whole is 108 MB of streams, paused). */
#define TAGPU_PB_OWEMAX ((size_t)32 << 20)
#define TAGPU_PB_MAXGEOM  ((int)TAGPU_PB_FRAMEMAX)   /* models cached at once        */
#define TAGPU_PB_MAXMAT   ((int)TAGPU_PB_FRAMEMAX)   /* (type, owner) streams        */
typedef char pb_keep_within_table[(TAGPU_PB_KEEPGEOM <= TAGPU_PB_MAXGEOM &&
                                   TAGPU_PB_KEEPMAT <= TAGPU_PB_MAXMAT) ? 1 : -1];

/* geometry vertex flags (float, bit-tested in the shader as an int) */
#define TAGPU_PBF_SHADED 1      /* body vertex: carries the engine's normal   */

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
    /* THE ANIMATED FACES: every body face whose texture is a sequence of more
       than one frame and no team colour (FF_ANIM set, FF_TEAM clear), in walk
       order, with its piece. Their frames are the only part of a material that
       changes while a level runs, so they are what a material's `akey` names
       (TAGPU_PBMAT). At most TAGPU_PB_MAXANIM a model; a face past it keeps
       frame 0, and the model says so once. One block the entry owns. */
    int          nanim;
    const char** animFace;
    unsigned char* animPiece;
    unsigned     lastFrame;                   /* for eviction                 */
} TAGPU_PBGEOM;
#define TAGPU_PB_MAXANIM 128

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
    /* WHICH FRAME EACH ANIMATED FACE SHOWS: `geom->nanim` bytes, one per
       entry of the geometry's animated-face list -- the frame (0..254), or
       TAGPU_PB_ANIM_NONE for a face the engine paints nothing for. NULL is
       every face at frame 0, the stream a unit takes when none of its
       animated faces is drawn live, so a model with none keys exactly as
       (type, owner, atlas) alone. Compared whole in the lookup; `akeyHash`
       only picks the chain. */
    unsigned char* akey;
    unsigned     akeyHash;
    unsigned     lastFrame;
} TAGPU_PBMAT;
#define TAGPU_PB_ANIM_NONE 0xFFu

/* What a unit's animated faces show this frame: the unit's
   TAGPU_PK_U_ANIMDC / TAGPU_PK_U_ANIMALL bits and the packet's animated-face
   table (tagpu_packet.h). A NULL source, or a unit with neither bit, takes
   frame 0 on every face. */
typedef struct TAGPU_PBANIM {
    unsigned                    flags;
    const struct TAGPU_PK_ANIM* tab;
    unsigned                    n;
} TAGPU_PBANIM;

/* Render thread, once per frame, before any tagpu_posebake_unit: drops every
   entry whose level generation or atlas generation has moved, freeing its
   mirrors (which is why this is render-thread only: the Vulkan unit pass reads
   them there). */
/* `level_gen` is THE PACKET'S — it advances at every level end whoever
   published it, where tagpu_reclaim's own counter moves only when reclaim is
   armed. */
void tagpu_posebake_frame(unsigned frame_counter, unsigned level_gen);

/* Render thread: the entries this instance has baked and dropped since the process started, both
   tables counted, each only ever growing. A stage that holds what it baked bakes and drops nothing
   frame to frame, which is what the wiki's frame count (tagpu_wikiworld.c, THE COUNT) reads. A
   refused geometry entry is kept and counts as no bake. */
void tagpu_posebake_work(unsigned* baked, unsigned* dropped);

/* Bake (or find) the geometry and the material stream for one unit's type.
   `pc` is the unit's PK_PIECE run out of the frame packet and supplies the
   piece list — each entry's `node` is the TYPE's template, so the entry is
   shared by every unit of it. `ghost` is 1 for the build ghost's synthesized
   run and keys the geometry entry APART from the units' (see the field).
   Returns 0 when the template could not be read or the unit atlas is not
   ready (tagpu_r3d_ready); a caller that gets 0 keeps whatever it was
   doing. `anim` says which frame each animated face shows (TAGPU_PBANIM);
   NULL is frame 0 everywhere. A stream whose new frame waits for the atlas
   allowance falls back to the frame-0 stream, so the unit is drawn. */
struct TAGPU_PK_PIECE;
int  tagpu_posebake_unit(const struct TAGPU_PK_PIECE* pc, int nparts, int owner,
                         int ghost, const TAGPU_PBANIM* anim,
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
   re-baked, when the stream has been sent (below), or when the malloc was
   refused -- and a refused malloc is not an error here: the caller draws
   nothing for that unit, which is a frame the Vulkan lane stands down on
   rather than a frame it draws wrong. */
const float* tagpu_posebake_geom_mirror(const TAGPU_PBGEOM* g, unsigned serial,
                                        int* nvert);
const float* tagpu_posebake_mat_mirror(const TAGPU_PBMAT* m, unsigned serial,
                                       int* nvert);

/* A STREAM IS HELD UNTIL IT IS SENT. A stream is some 200 KB of a large model, and a ten-seat
   battle asks for 1 500 of them: kept beside the device's copies they were 300 MB of a 32-bit
   process (MEASURED, scenarios/zombie-kingdom-13k). So the consumer says when a stream is in
   a buffer of its own (`_sent`) and the bake frees it; the entry stays, and answers every
   later frame with the same serial.

   THE INVARIANT IS THE CONSUMER'S TO KEEP: for a live entry, the stream is here or the
   consumer holds its buffer. It says `_sent` only once the bytes are copied out; `_lost` when
   it gives one buffer up; and `_lost_all` when it starts from an empty table, before it reads
   any entry -- which is every entry it was ever sent, the ones no frame is asking for
   included. A lost entry whose stream is not here is dropped, so the next frame that asks
   bakes it again under a new serial; one whose stream is here has lost nothing. `_sent` and
   `_lost` check the pointer and the serial as the accessors above do; a stale one does nothing.

   WHAT THE BAKE KEEPS FOR ITSELF is what is read every frame rather than once: a geometry
   stream's WIRE range, four floats a vertex (x, y, z, piece), and a material stream's skip
   flags, a byte a vertex -- the nanoframe wire's records are posed from the two.

   Render thread, like everything here. */
void tagpu_posebake_geom_sent(const TAGPU_PBGEOM* g, unsigned serial);
void tagpu_posebake_mat_sent(const TAGPU_PBMAT* m, unsigned serial);
void tagpu_posebake_geom_lost(const TAGPU_PBGEOM* g, unsigned serial);
void tagpu_posebake_mat_lost(const TAGPU_PBMAT* m, unsigned serial);
void tagpu_posebake_lost_all(void);
/* How many more buffers the consumer can make: the bake owes it no more streams than that,
   so a consumer whose device bounds its allocations is never handed a frame it must refuse.
   It says so whenever the figure moves; until it has, nothing bounds the count. */
void tagpu_posebake_room(unsigned streams);
/* `*nvert` is the WIRE range's length; vertex v of it is the stream's `first[WIRE] + v` */
const float* tagpu_posebake_geom_wire(const TAGPU_PBGEOM* g, unsigned serial, int* nvert);
const unsigned char* tagpu_posebake_mat_skip(const TAGPU_PBMAT* m, unsigned serial, int* nvert);
#endif
