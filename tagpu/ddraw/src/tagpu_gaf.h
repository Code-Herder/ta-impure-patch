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

#include "tagpu_restoreglsl.h"   /* TAGPU_RGLSL_FRAME, the shared frame */

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
    int           mirroredMippedN; /* ...and when the mirror last read them back */
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
    /* BUMPED EVERY TIME `rgb` IS CREATED, and never otherwise: a re-arm frees
       the texture and the job and makes both again, which resets
       `tagpu_rglsl_job_painted` to 0 -- so the painted count alone is not a
       content key across that seam. This is the discontinuity it cannot see. */
    unsigned      rgbGen;
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
    /* THE CPU MIRROR (Phase G / G19e, the Vulkan lane). `dim` x `dim` bytes
       holding exactly what has been uploaded to `tex`, written by the same
       atlas_paint that writes GL and by nothing else, so a second backend can
       upload the SAME texels rather than decode the art a second time. NULL
       until tagpu_gaf_atlas_mirror asks for it; a pass that never asks pays
       nothing. `mirrorSerial` is bumped by every paint, and is what a backend
       holding a copy tests to know its copy is stale -- 0 while there is no
       mirror, and never 0 once a paint has landed in one.
       ONCE ARMED IT STAYS for the process's life: there is no atlas destructor
       here (every atlas in the tree is a static owned by its pass), and a lane
       cleared and re-armed finds the mirror already correct rather than paying
       for the re-decode again. */
    unsigned char* mirror;
    unsigned      mirrorSerial;
    /* THE RESTORED TWIN'S MIRROR (G19f landing 4), opt-in in the same way and
       for the same reason -- and it is NOT the same mechanism. `mirror` above
       is written by the paint that writes GL, because the CPU holds the source
       bytes; the restored twin's texels are the RESTORER'S OUTPUT and exist
       only on the GPU, so the only way to them is a read-back. It is bounded
       three ways: it happens at all only while a lane has asked for it, only
       on a frame `tagpu_rglsl_job_painted` moved (which is the restorer's own
       content counter, and is still through the whole fill and every steady
       frame after it), and only over the rows the shelf has actually used.
       `mirrorRgbSerial` is what a backend holding a copy tests, exactly as for
       the indexed mirror. */
    unsigned char* mirrorRgb;
    /* THE SHAPE THE BUFFER WAS ALLOCATED WITH, and it is not `dim`/`mip` read
       again later. This mirror is deliberately never freed, so it outlives
       every change to the atlas it mirrors -- and `mip` DOES change: the twin
       is demoted to 0 on a GL with no glGenerateMipmap, and the owner's own
       init raises it back to its compile-time value on the next context reset.
       Sizing a memset or a read-back off the CURRENT pair can therefore write
       a 21 MB chain into a 16 MB allocation. Every write to this buffer is
       bounded by the pair below instead. [Gate 3a; the mip chain is what made
       this expressible at all -- before it, every one of those sites was the
       same constant `dim * dim * 4`.] */
    int            mirrorRgbDim;
    int            mirrorRgbMip;
    int            mirrorRgbMips;   /* top level index read back; 0 = level 0 alone */
    float          rgbAniso;        /* anisotropy actually applied to the twin, 0 = none */
    unsigned      mirrorRgbSerial;
    int           mirrorRgbRows;   /* rows of it that have been read back    */
    unsigned int  mirrorRgbFbo;    /* the read-back's own FBO, made once     */
    int           mirroredPainted; /* job_painted() at the last read-back    */
    unsigned      mirroredRgbGen;  /* `rgbGen` at the last read-back         */
    /* LATCHED, so a refusal is said once. Without it the owner's per-frame
       `mirror_rgb()` re-allocates and re-fails every present and writes a line
       to tagpu.log at the frame rate. */
    int           mirrorRgbFailed;
    /* THE PUBLISHED RESTORE LIST (the Vulkan-only plan's landing 7d), which is
       the OTHER answer to the same question the two mirrors above answer: a
       second backend can either read this lane's restored texels back, or run
       the restore itself. This is the second, and it is the cheaper one by a
       whole read-back -- what crosses is the REQUEST rather than the picture.

       It holds the very frames this atlas queued for the GL restorer, in the
       order it queued them, because three of a frame's eleven numbers are
       content-dependent: `wrap` is tagpu_rglsl_tileable() over the frame's own
       texels against the ART palette, `key` is the frame header's, and the
       padding comes from the atlas's cell layout. Those are this side's facts;
       what crosses is their result. The same list therefore makes the two
       lanes comparable byte-for-byte rather than merely both-plausible.

       IT IS A LAZY QUEUE AND SO IT IS APPEND-ONLY WITH A GENERATION, which is
       what makes it different from terrain's whole-list-per-serial: a GAF
       atlas is fed one frame at a time, for the life of the atlas, so a
       consumer holds a CURSOR into this array and takes what is past it. A
       frame on which the consumer took nothing therefore costs nothing -- it
       takes more on the next one.

       `rlistGen` IS THE DISCONTINUITY and the only thing a cursor cannot
       survive. It is bumped whenever the array stops being a continuation of
       what a consumer already has: the arm, a recycle or a repack (both of
       which drop the queue and blank the destination), a context loss, a
       palette move, and the overflow restart below. A consumer that sees a new
       generation drops its own job and starts from index 0.
       `rlistRepaint` is 1 only for the palette-move generation, where the
       destination already holds a restore and is recoloured in place.

       BOUNDED, because an append-only list fed for a session's length is not.
       The cap is four times the atlas's entry ceiling; reaching it RESTARTS
       the list from the entries the atlas holds right now (a new generation,
       repaint 0) rather than growing, so the memory is a function of `max`
       and the recovery is the same path as the arm. Armed by
       tagpu_gaf_atlas_restore_vk and NULL otherwise, so an atlas nobody asks
       pays nothing -- and while it is armed the read-back mirror above stands
       down, because the two are answers to one question and doing both would
       pay for the mirror to be ignored. */
    TAGPU_RGLSL_FRAME* rlist;
    int           rlistN, rlistCap;
    unsigned      rlistGen;
    int           rlistRepaint;
    int           rlistWant;       /* armed; the read-back has stood down    */
    int           rlistFailed;     /* latched, and said once                 */
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

/* THE HEADER FIELDS A CALLER NEEDS TO PLACE A SPRITE, read inside this module
   so that a pass which only places sprites dereferences no engine byte of its
   own. Landing 4a of the frame packet exchange is why this exists: the effects
   and particle passes take their frames out of the packet now, resolved by the
   game thread, and both files came off the thread-split allow-list — which
   they could not while `emit_sprite` read w/h/hotspot/sub-count itself.
   Returns 0 and writes nothing when the frame is not sane. */
typedef struct TAGPU_GAFGEOM {
    int w, h;                   /* the frame's size                          */
    int hotx, hoty;             /* its hotspot, subtracted from the anchor    */
    int subn;                   /* sub-frames; 0 = a plain frame             */
    unsigned char ck;           /* its colour key                            */
    unsigned char subalp;       /* this frame asks for the alpha blit         */
} TAGPU_GAFGEOM;
int tagpu_gaf_frame_geom(const void* g, TAGPU_GAFGEOM* out);
/* Sub-frame `k` of a compound frame, bounded by the frame's own sub-count and
   readable-checked like every other resolver here; NULL when there is none. */
const unsigned char* tagpu_gaf_subframe(const void* g, int k);

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

/* Ask for the CPU mirror above, and make it CORRECT FROM THE INSTANT IT
   EXISTS. A mirror allocated after the atlas has already painted frames would
   hold zeros where those frames are, and a backend uploading it would draw
   black trees for the rest of the session -- silently, because nothing in the
   atlas is wrong. So this marks every painted entry RESERVED (`ok` 0, `resv`
   1), which is the state a repack leaves an entry in: the rect stays where it
   is and the next tagpu_gaf_atlas_get for that frame paints it again, into GL
   and into the mirror together. The atlas re-converges over the next frames at
   the cost of one RLE decode per frame still on screen.

   Render thread only, like the rest of this module. Returns 0 if the memory
   was refused, and the atlas then goes on working without one. Idempotent. */
int  tagpu_gaf_atlas_mirror(TAGPU_GAFATLAS* a);

/* Ask for the RESTORED twin's mirror, and step it.

   ARMING IS CORRECT FROM THE INSTANT IT EXISTS with no re-decode dance,
   unlike the indexed mirror above: the destination the restorer paints into is
   cleared to alpha 0 when the job is made and every texel it has painted since
   is one `mirrorRgbStep` reads back, so a mirror allocated at any moment
   converges on the next step and holds alpha 0 -- "not restored here" -- until
   it does. That is the same answer a consumer would get from an unpainted
   cell, so there is no window in which it is WRONG, only one in which it is
   behind, and the step closes that on the frame the paint happened.

   STEP IT WHERE THE PAINT IS ALREADY VISIBLE TO THIS FRAME'S DRAWS -- after
   `tagpu_rglsl_step` and before the ops that sample the twin. tagpu_gui_surf.c
   does exactly that, for exactly that stated reason, and the mirror is then
   byte-identical to what those ops sampled rather than a frame behind them.

   Render thread only, GL context current. Arming returns 0 if the memory or
   the FBO was refused and the atlas goes on without one; the step is a no-op
   when nothing is armed or nothing has been painted since the last one. */
int  tagpu_gaf_atlas_mirror_rgb(TAGPU_GAFATLAS* a);
void tagpu_gaf_atlas_mirror_rgb_step(TAGPU_GAFATLAS* a);

/* Ask for the PUBLISHED RESTORE LIST instead of the read-back above (the
   Vulkan-only plan's landing 7d), and stand the read-back down.

   It arms only while `tagpu_restorevk.on` is beside TotalA.exe: the second
   backend restoring for itself is the end state, but until its bytes have been
   compared against this lane's on the machine in front of you, the read-back
   is the shipped path and this is the measurement. Poll it on the same beat as
   the mirror -- the lever can appear mid-session, and arming then frees the
   16 MB the read-back had already taken.

   Seeded with every entry the atlas holds right now, so it is correct from the
   instant it exists in the same sense the mirror is: a consumer starting from
   index 0 restores exactly what this lane has, whatever has already been
   painted here. Returns 1 when armed (and on every later call), 0 when the
   lever is absent or the memory was refused -- and the atlas then goes on
   reading back as before. Render thread only. */
int  tagpu_gaf_atlas_restore_vk(TAGPU_GAFATLAS* a);

/* Read an RGBA8 GL texture back into `dst`, `rows` rows of `w` texels, through
   a caller-owned FBO created on first use. Not about an atlas: it is here
   because this is where glReadPixels is resolved, and the step above CALLS it
   rather than repeating it. Render thread, context current. 1 when `dst` was
   filled. `status` (may be NULL) returns the glCheckFramebufferStatus value, or
   0 if none was taken -- an incomplete framebuffer is a permanent property of
   the texture and both callers latch on it, which the return value alone cannot
   tell them. (The Vulkan-only plan's gate 2.) */
int  tagpu_gl_rgba_readback(unsigned tex, int level, int w, int rows,
                            unsigned char* dst, unsigned* fbo, unsigned* status);

/* THE RESTORED TWIN'S MIRROR IS THE WHOLE MIP CHAIN when the atlas is mipped,
   because its GL original is sampled GL_LINEAR_MIPMAP_LINEAR and a consumer
   holding level 0 alone draws a different picture wherever the art is minified
   -- which on the unit atlas is ordinary play. Level L is `dim >> L` square,
   RGBA8, at `tagpu_gaf_mip_off`; the whole chain is `tagpu_gaf_mip_chain`
   bytes. `mirrorRgbMips` is the TOP LEVEL INDEX actually read back (0 means
   level 0 alone), so a consumer builds an image with the levels that exist
   rather than one with holes in it. */
/* THE ANISOTROPY A RESTORED TWIN IS FILTERED WITH, where the extension answers.
   A second backend must apply the same ratio or draw different art wherever the
   texture is minified at an angle -- so this is a shared constant rather than
   each lane's own choice, and `rgbAniso` below says what was actually applied
   on the GL side, which is what a consumer compares itself against. */
#define TAGPU_GAF_TWIN_ANISO 4.0f

size_t tagpu_gaf_mip_bytes(int dim, int level);
size_t tagpu_gaf_mip_off(int dim, int level);
size_t tagpu_gaf_mip_chain(int dim, int mip);
#endif
