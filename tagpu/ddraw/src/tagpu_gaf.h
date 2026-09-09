#ifndef TAGPU_GAF_H
#define TAGPU_GAF_H
/* tagpu_gaf.h — GAF frames: the layout, the decoder and the GL shelf atlas.

   Every colour-keyed sprite the engine blits — explosion frames, particle
   puffs, feature bodies and their shadows, GAF wreckage — is a GAF frame
   reached through a sequence (`GAF_SequenceIndex2Frame 0x4B7F30`) or an anim
   state (`GAFGetCurrentFramePtrAddr 0x4B7EE0`). Both resolvers are pure
   reads: 0x4B7EE0 indexes the sequence's frame table with the state's frame
   number and never advances it, so a pass that owns a draw leaf does not
   stall any animation (verified in the decompile, G13a).

   The decoder and the atlas were a private copy in tagpu_fx.c; features need
   the same two, so they live here and both passes share them. An atlas is
   caller-owned storage (its entry array and dimensions) so each pass keeps
   its own lifetime: the effects atlas churns as explosion sequences are
   freed, the feature atlas fills once per map and stays.

   See research/notes/effects.md §3 (blit leaves, frame formats) and
   research/notes/features.md. */

/* frame header (0x18 bytes) */
#define TAGPU_GF_W      0x00    /* u16 */
#define TAGPU_GF_H      0x02    /* u16 */
#define TAGPU_GF_HOTX   0x04    /* i16 */
#define TAGPU_GF_HOTY   0x06    /* i16 */
#define TAGPU_GF_CK     0x08    /* u8 colour key                            */
#define TAGPU_GF_COMP   0x09    /* u8 compressed (TA RLE rows)              */
#define TAGPU_GF_SUBN   0x0A    /* u8 sub-frame count (pixels = frame*[])   */
#define TAGPU_GF_SUBALP 0x0B    /* u8 sub-frame wants the alpha blit        */
#define TAGPU_GF_PIX    0x10    /* u8* colour plane                         */

/* sequence / anim entry */
#define TAGPU_SQ_N      0x00    /* u16 frame count                          */
#define TAGPU_SQ_NAME   0x08    /* char[32]                                 */
#define TAGPU_SQ_TAB    0x28    /* {GAFFrame*, u32}[] stride 8              */
/* anim state (what 0x4B7EE0 takes) */
#define TAGPU_AS_FRAME  0x00    /* u16 current frame index                  */
#define TAGPU_AS_SEQ    0x08    /* sequence*                                */

#define TAGPU_GAF_DECMAX 512    /* largest frame edge the decoder accepts    */
#define TAGPU_GAF_PADMAX 4      /* widest replicated border an atlas may ask  */

typedef struct TAGPU_GAFENT {
    const void*    frame;       /* keyed on the header AND its pixel ptr:    */
    const void*    pix;         /* freed sequences get their address reused  */
    unsigned short w, h;
    unsigned short x, y;        /* its first texel in the atlas (inside the border) */
    float          u0, v0, u1, v1;
    unsigned char  ck;
    char           ok;
    char           wrap;        /* tagpu_rglsl_tileable said so at upload    */
} TAGPU_GAFENT;

struct TAGPU_RGLSL_JOB;

/* Caller-owned atlas. Zero it, then point `ents`/`max` at your storage and
   set `dim` and `tag` before the first tagpu_gaf_atlas_get. */
#define TAGPU_GAF_HASH 8192     /* power of two, > 2x any atlas's max      */

typedef struct TAGPU_GAFATLAS {
    unsigned int  tex;          /* GL_R8 texture, created on first use       */
    int           dim;          /* square edge in texels                     */
    int           max;          /* entries in `ents`                         */
    TAGPU_GAFENT* ents;
    int           n, shelfX, shelfY, shelfH;
    int           full;         /* no room: reset deferred to the next frame */
    /* THE ATLAS GENERATION. Every recycle and every context loss moves EVERY
       entry's u0..v1: the shelf restarts at the origin and the same frame is
       re-inserted wherever it now fits. A UV read out of this atlas is
       therefore only valid for the generation it was read in, which does not
       matter to a pass that re-reads them every frame -- and matters entirely
       to one that BAKES them into a vertex buffer (gpu-posing.md 3, the
       material stream). Bumped by atlas_reset and atlas_lost; a cache keyed
       on it drops itself when the UVs move. Starts at 0 and only increases. */
    unsigned      gen;
    const char*   tag;          /* log prefix, e.g. "fx" / "feat"            */
    /* The cell layout (renderers.md 2.5, the unit atlas): every frame is
       uploaded with `pad` replicated edge texels on all four sides and its
       cell -- frame plus border -- is allocated at a multiple of `align`
       texels in both origin and size. 0 for either means 1, the sprite
       atlases' layout (one texel of border, G13i). The unit atlas asks for
       4 and 4 so the twin can be MIPMAPPED to `mip` levels: with cells
       4-aligned and 4 texels of the frame's own edge around it, a mip texel
       at level <= 2 that touches a frame is made only of that frame's texels
       (tools/tascene UNIT_PAD says the same). `mip` > 0 makes the twin
       trilinear (GL_LINEAR_MIPMAP_LINEAR, MAX_LEVEL = mip, 4x anisotropic
       where the extension answers) and its levels are regenerated after
       every batch the restorer paints; 0 keeps it NEAREST, 1:1. */
    int           pad, align, mip;
    int           mippedN;      /* frames painted when the mips were last built */
    /* Classic++ (renderers.md 4b Option 4): the RESTORED TWIN -- same dim,
       same shelf, GL_RGBA8 -- painted lazily by tagpu_restoreglsl.c from a
       queue that every miss feeds, so a frame draws indexed for the frame or
       two before its restore lands. A sprite shader samples the twin where its
       alpha is 1 and stays on the index elsewhere; a recycle clears it. `prio`
       orders the queue against the other jobs (the terrain's is 0). Created by
       tagpu_gaf_atlas_restore, and `rgb` is non-zero exactly while `job` is:
       both are made together, and both go when the job cannot be made or the
       context is lost -- a pass gates its restored branch on `rgb`. */
    unsigned int  rgb;
    struct TAGPU_RGLSL_JOB* job;
    int           prio;
    int           restoreFailed;
    int           dumpedN;      /* entries when tagpu_restoredump.on last wrote */
    const unsigned char* pal;   /* the live palette, for the tileability test */
    /* open-addressed index over `ents`, keyed on the frame header address:
       the lookup runs once per emitted sprite and the feature pass emits
       hundreds per frame against a four-figure entry count, which a linear
       scan turns into millions of comparisons */
    int           hash[TAGPU_GAF_HASH];   /* entry index + 1; 0 = empty      */
} TAGPU_GAFATLAS;

/* ---- frame resolution (all read-only, all NULL-safe) ---- */
const unsigned char* tagpu_gaf_frame_sane(const void* g);
const unsigned char* tagpu_gaf_seq_frame(const char* seq, int idx);
int                  tagpu_gaf_seq_nframes(const char* seq);
const unsigned char* tagpu_gaf_state_frame(const char* animstate);
const char*          tagpu_gaf_seq_name(const char* seq);

/* Decode a frame's colour plane (raw or TA-RLE) into `out`, which must hold
   w*h bytes; unwritten texels are left at the colour key. 0 if unreadable. */
int tagpu_gaf_decode(const unsigned char* g, int w, int h, unsigned char* out);

/* Atlas: entry for a frame, decoding and uploading it on first sight. NULL
   when the frame is unreadable or the atlas is full (it then flags itself and
   the next tagpu_gaf_atlas_reset recycles it — never mid-frame, or quads
   already emitted would point at re-used texels). */
const TAGPU_GAFENT* tagpu_gaf_atlas_get(TAGPU_GAFATLAS* a, const unsigned char* g);
/* The same entry from PRE-DECODED pixels (w*h bytes, colour key `ck`), keyed
   like atlas_get on (frame, pix, w, h) but never reading either pointer: the
   GL UI renderer's sprite ops carry their bytes across the thread boundary
   because the shell frees a popped screen's art under the render thread
   (gui-renderer.md 3.5). `find` is the lookup alone, NULL when absent. */
const TAGPU_GAFENT* tagpu_gaf_atlas_put(TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                        int w, int h, unsigned char ck, const unsigned char* pixels);
const TAGPU_GAFENT* tagpu_gaf_atlas_find(const TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                         int w, int h);
void tagpu_gaf_atlas_reset(TAGPU_GAFATLAS* a);      /* recycle when full     */
void tagpu_gaf_atlas_lost(TAGPU_GAFATLAS* a);       /* GL context replaced   */
/* create the GL texture now rather than on the first frame that atlases a
   sprite — a pass whose shader samples the atlas must never bind texture 0 */
int  tagpu_gaf_atlas_create(TAGPU_GAFATLAS* a);
/* Once per frame from the owning pass, before its atlas_get calls, with the
   live palette (main+0x143A7). Arms the lazy restore the first time the
   Classic++ switch is seen on: creates the twin and the queue and queues
   every frame already in the atlas. A no-op after that; render thread only. */
void tagpu_gaf_atlas_restore(TAGPU_GAFATLAS* a, const unsigned char* pal);
#endif
