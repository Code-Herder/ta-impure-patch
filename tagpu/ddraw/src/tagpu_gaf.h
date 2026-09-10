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
    char           ok;          /* painted: its texels are in the atlas      */
    char           wrap;        /* tagpu_rglsl_tileable said so at upload    */
    /* ASKED FOR SINCE THE LAST REPACK. Set by every lookup that returns this
       entry and by the insertion that made it; cleared on the survivors of a
       repack. It is what makes a repack an EVICTION as well as a re-lay: an
       entry nothing has asked for between two repacks is not part of the
       working set the atlas is short of room for, so it is dropped and
       re-decodes if it is ever wanted again (one RLE decode, on demand, in
       the frame that asks). Without it the atlas pins entries for its whole
       life -- and its life is longer than a map's, so a session that changed
       maps would carry the previous map's frames forever and reach the wall
       holding art nothing on screen can use. */
    char           hit;
    /* RESERVED BY A REPACK: the rect above is assigned but nothing has been
       uploaded to it yet, because a repack moves every entry and we do not
       keep the decoded pixels (only the frame's address and its size). The
       next atlas_get/atlas_put for this frame paints it in place instead of
       allocating a new cell. `ok` is 0 for exactly as long as that is true,
       so tagpu_gaf_atlas_find keeps refusing it -- there is nothing there to
       sample yet. Cleared when the paint lands. */
    char           resv;
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
    /* THE REPACK (features.md 5, "the atlas is half empty"). Set by an atlas
       whose contents are worth keeping -- one whose frames stay useful for
       as long as the atlas lives, which is the feature atlas: it fills once
       per map and every frame in it is a feature that is still on the map.
       With it, filling up re-lays what is already here TALLEST CELL FIRST
       instead of dropping it, and each entry re-uploads on its next get.
       Insertion order is what wastes the page -- a 320-tall tree opens a
       shelf that a row of 12-tall rocks then sits in -- so sorting is worth
       more than any cleverer packer: measured on Town & Country's 229
       feature frames, arrival order places 197 and spans 86% of a 2048
       square, tallest-first places all 229 and spans 58%. A skyline packer
       gets that to 53% and is four times the code, which buys nothing while
       the page is not the binding constraint.
       Leave it 0 for an atlas that churns -- the effects atlas frees and
       re-allocates sequences, so pinning its entries would pin dead ones. */
    int           repack;
    /* THE WALL. Latched when a repack could not place every entry that was
       still being asked for -- i.e. the LIVE working set does not fit one
       page, which no re-sort can change. Past it a repack is futile by
       construction, so the atlas HOLDS what it has rather than dropping it
       for a rebuild that would place fewer, and the log says a second page
       is the only thing left that adds room.
       It is NOT a terminal state: tagpu_gaf_atlas_forget clears it, and the
       owning pass calls that when the thing the atlas describes has been
       replaced -- for the feature atlas, when the map changed. A wall that
       could not be cleared would mean an atlas full of the wrong map's art
       refusing every frame of the new one for the rest of the session. */
    int           repackWall;
    unsigned      repacks;      /* how many have run, for the pass's log line */
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
    /* frames whose shorter edge is under this are never queued for restore:
       below the model's receptive field there is nothing to restore, so the
       work returns approximately its input (G15-0's verdict, gui-renderer.md
       3.9). 0 = no floor, which is every atlas but the UI's. */
    int           restoreMinEdge;
    int           restoreFailed;
    int           dumpedN;      /* entries when tagpu_restoredump.on last wrote */
    const unsigned char* pal;   /* the live palette, for the tileability test */
    unsigned      palSerial;    /* tagpu_pal serial the TWIN was restored through */
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
/* Recycle a full atlas. With `repack` set this RE-LAYS the entries it holds
   tallest-first and keeps them (reserved, re-uploading on demand); without
   it -- and always for a restart that is not "full", such as the UI atlas's
   re-arm -- it drops them and they re-decode in arrival order as before. */
void tagpu_gaf_atlas_reset(TAGPU_GAFATLAS* a);
/* Drop every entry and every repack decision: the atlas no longer describes
   anything the caller wants. For an atlas with `repack` set this is the only
   way back from the wall, and the owning pass owes it one call whenever the
   subject changes underneath -- `tagpu_feat.c` on a map change. Cheap, and
   correct to call when nothing has changed. */
void tagpu_gaf_atlas_forget(TAGPU_GAFATLAS* a);
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
