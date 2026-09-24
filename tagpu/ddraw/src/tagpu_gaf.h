#ifndef TAGPU_GAF_H
#define TAGPU_GAF_H
/* tagpu_gaf.h — GAF frames: the layout, the decoder and the shelf atlas.

   Every colour-keyed sprite the engine blits — explosion frames, particle
   puffs, feature bodies and their shadows, GAF wreckage — is a GAF frame
   reached through a sequence (`GAF_SequenceIndex2Frame 0x4B7F30`) or an anim
   state (`GAFGetCurrentFramePtrAddr 0x4B7EE0`). Both resolvers are pure
   reads: 0x4B7EE0 indexes the sequence's frame table with the state's frame
   number and never advances it, so a pass that owns a draw leaf does not
   stall any animation (verified in the decompile).

   The effects and feature passes need the same decoder and atlas, so they
   live here and both passes share them. An atlas is
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

/* 640, AND THE REASON IS ONE PIXEL. The in-game HUD bars are 513-px frames, so
   at 512 they would fail `fw <= DECMAX` by one and fall to `as_pixels`, which
   draws nothing (PK_PIXELS is dropped). 640 also takes in the shell's 640-wide
   title art, and stops there: the next power of two costs 1.5 MB of static for
   nothing this engine asks for.
   COST: s_dec and s_pad are ~813 KB together (~522 KB at 512). The gui atlas is
   2048 square, so a 513-wide entry is not its constraint. */
#define TAGPU_GAF_DECMAX 640    /* largest frame edge the decoder accepts    */
#define TAGPU_GAF_PADMAX 4      /* widest replicated border an atlas may ask  */

typedef struct TAGPU_GAFENT {
    const void*    frame;       /* keyed on the header AND its pixel ptr:    */
    const void*    pix;         /* freed sequences get their address reused  */
    /* AND ON THE SOURCE WINDOW, because (frame, pix, w, h) is not an identity
       once a transformed draw can take a SUB-RECTANGLE of a frame. 0 is "the
       whole frame", which is what every 1:1 sprite, every cursor and every
       atlas_get passes. A windowed capture packs (u0, v0, uw, uh) one byte each --
       see `scale_capture`'s caller, which refuses to claim a window it cannot
       key. Without this, two windows of one frame resampled to the same
       destination size collide, `atlas_find` runs BEFORE `atlas_put`, and the
       second draw silently reuses the first one's texels. */
    unsigned       win;
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
    /* RESERVED: the rect above is assigned but nothing has been painted into
       it yet. A repack leaves an entry so when it had not been painted, or
       when there is no mirror to carry its texels to the new rect (the
       decoded pixels live nowhere else); so does the mirror's arm. The
       next atlas_get/atlas_put for this frame paints it in place instead of
       allocating a new cell. `ok` is 0 for exactly as long as that is true,
       so tagpu_gaf_atlas_find keeps refusing it -- there is nothing there to
       sample yet. Cleared when the paint lands. */
    char           resv;
    /* WAITING FOR THE ALLOWANCE (`budget` below): `deferEpoch` is the atlas
       frame this entry was first refused its paint in, 0 while it is not
       waiting; `askFrame` the last atlas frame that asked for it; `pend` 1
       while its cell is counted in this frame's backlog (`bkTiles`). A waiting
       entry is RESERVED: its rect is assigned and nothing samples it. */
    char           pend;
    unsigned       deferEpoch, askFrame;
    /* ITS FRAME IN THE PUBLISHED RESTORE LIST (`rlist`), -1 for none: what a
       repack rewrites in place when it moves the entry, so the list stays one
       generation and a consumer's cursor into it stays good. */
    int            rli;
} TAGPU_GAFENT;

#include "tagpu_restoreglsl.h"   /* TAGPU_RGLSL_FRAME, the shared frame */

struct TAGPU_RGLSL_JOB;

/* THE DIRTY MAP: for each tile of TAGPU_GAF_TILEW x TAGPU_GAF_TILEH texels,
   the mirror serial of the last write that touched it. A consumer holding a
   copy as of serial S re-sends exactly the tiles whose serial is past S
   (tagpu_gaf_dirty_since) -- which matters once that copy is four bytes a
   texel (the world's base atlas, tagpu_pal_expand).
   IT CANNOT OVERFLOW: it has a slot per tile, not per write, so however many
   paints land between two uploads, the tiles they touched are all still
   named. A consumer re-sends the whole page only when the whole page was
   written (a create, a loss, a mirror arm) or its copy is gone -- never
   because a burst of paints outran a record of them. A repack writes the
   whole page too, unless the atlas publishes its moves (`moveList` below).
   A mirror is refused on an atlas wider than TAGPU_GAF_DIMMAX, so the map
   covers every texel of any mirror there is. */
#define TAGPU_GAF_TILEW   32
#define TAGPU_GAF_TILEH   16
#define TAGPU_GAF_DIMMAX  2048
#define TAGPU_GAF_TCOLS   (TAGPU_GAF_DIMMAX / TAGPU_GAF_TILEW)
#define TAGPU_GAF_TROWS   (TAGPU_GAF_DIMMAX / TAGPU_GAF_TILEH)

/* ONE CELL A REPACK MOVED, border and alignment slack included: `w` x `h`
   texels from (ox, oy) to (nx, ny). */
typedef struct TAGPU_GAFMOVE {
    unsigned short ox, oy, nx, ny, w, h;
} TAGPU_GAFMOVE;

/* THE ALLOWANCE: the most tiles a consumer's copy may be due at once. 1024
   tiles of 32 x 16 RGBA8 texels are 2 MiB, which is what one slot's staging
   holds (TAGPU_VK_STAGE_CAP, which asserts the two agree), so whatever an
   atlas paints between two uploads goes into the frame's own command buffer
   and nothing waits. Sized from the largest single-frame paint measured in
   play, 1.20 MB after a camera jump at 4K (gpu-status §2.88), with headroom.
   A paint past it is DEFERRED: the entry keeps its rect, reserved, and is
   painted on a later frame, first in the order it was refused. */
#define TAGPU_GAF_BUDGET  1024
/* THE LARGEST CELL FITS THE ALLOWANCE ALONE, which is what makes a deferral
   always end: with nothing due, the oldest waiting entry is painted whatever
   its size. A cell is at most DECMAX + 2 x PADMAX texels, rounded up to at
   most PADMAX of alignment, and at an arbitrary origin it touches one more
   tile a side than its size fills. */
typedef char tagpu_gaf_cell_fits_budget[
    ((TAGPU_GAF_DECMAX + 4 * TAGPU_GAF_PADMAX + TAGPU_GAF_TILEW - 1) / TAGPU_GAF_TILEW + 1) *
    ((TAGPU_GAF_DECMAX + 4 * TAGPU_GAF_PADMAX + TAGPU_GAF_TILEH - 1) / TAGPU_GAF_TILEH + 1)
        <= TAGPU_GAF_BUDGET ? 1 : -1];
/* the backlog's buckets, one per frame of first refusal, oldest first */
#define TAGPU_GAF_BKMAX   8

/* Caller-owned atlas. Zero it, then point `ents`/`max` at your storage and
   set `dim` and `tag` before the first tagpu_gaf_atlas_get. */
#define TAGPU_GAF_HASH 8192     /* power of two, > 2x any atlas's max      */

typedef struct TAGPU_GAFATLAS {
    /* THE ATLAS EXISTS WHEN `made` SAYS SO. There is no texture on this side:
       the entries, the shelf packer and the CPU mirror are the atlas, and the
       Vulkan passes upload the mirror themselves. */
    int           made;         /* the atlas is laid out and usable          */
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
       instead of dropping it: a painted entry's texels move with it in the
       mirror, and an entry never painted is painted on its next get.
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
       atlases' layout (one texel of border). The unit atlas asks for
       4 and 4 so the twin can be MIPMAPPED to `mip` levels: with cells
       4-aligned and 4 texels of the frame's own edge around it, a mip texel
       at level <= 2 that touches a frame is made only of that frame's texels
       (tools/tascene UNIT_PAD says the same). `mip` > 0 means the twin is
       MIPPED to that top level and sampled trilinearly, 0 that it is NEAREST
       and 1:1. IT IS A SHAPE, NOT A SETTING: nothing on this side samples
       with it. What reads it is `tagpu_r3d_atlas_restore_list`, which publishes
       it as `restoreMips` so the Vulkan lane builds a chain of that depth
       -- see `rgbAniso` below for the other half of that contract. */
    int           pad, align, mip;
    /* Classic++ (renderers.md 4b Option 4): the RESTORED TWIN -- same dim,
       same shelf, RGBA8 -- is the Vulkan lane's image, painted lazily from the
       published restore list below, which every miss feeds, so a frame draws
       indexed for the frame or two before its restore lands. A sprite shader
       samples the twin where its alpha is 1 and stays on the index elsewhere.
       Whether a pass's twin is live at all is keyed on `rlistWant` (the arm):
       that is what each pass's hand-over `restored` is published from. */
    /* frames whose shorter edge is under this are never queued for restore:
       below the model's receptive field there is nothing to restore, so the
       work returns approximately its input (gui-renderer.md 3.9). 0 = no
       floor, which is every atlas but the UI's. */
    int           restoreMinEdge;
    /* THE CPU MIRROR, which is what the Vulkan lane uploads. `dim` x `dim`
       bytes holding every texel the atlas has painted, written by atlas_paint
       and by nothing else, so the Vulkan passes upload these texels rather
       than decode the art a second time. NULL
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
    /* THE KEY PLANE: `dim` x `dim` bytes beside the mirror, 0 where the texel
       is its frame's colour key and 255 where it is art. The world passes
       need it because their base atlas is RGBA and the key is per FRAME, so
       an index alone cannot say whether a texel is a hole. Opt-in: the owner
       sets `keyPlane` before asking for the mirror, and `keym` then lives in
       the same allocation, written by the same paint, cleared by the same
       memsets -- one lifetime, never a second one to keep in step. */
    int            keyPlane;
    unsigned char* keym;
    /* the serial of the last mirror write to each tile (the DIRTY MAP above),
       row-major, TAGPU_GAF_TCOLS tiles a row */
    unsigned       dirty[TAGPU_GAF_TROWS * TAGPU_GAF_TCOLS];
    /* THE LAST REPACK'S MOVES, for a consumer that moves its own copy of the
       mirror rather than receive the page again. The owner sets `moveList`
       when its consumer applies them (tagpu_vk_feat.c); any other atlas's
       repack is a whole-page write to the map above, which every consumer
       already answers.
       `moves` holds the `moveN` cells that moved painted. `moveSerial` is the
       mirror serial the repack took and `movePrev` the one the recorded repack
       before it took, 0 before the first. A copy as of serial S takes this
       move when S is before `moveSerial` and not before `movePrev`: it then
       holds every earlier move and lacks only this one. A copy further behind
       takes the whole page.
       THE MAP STAYS EXACT ACROSS THE MOVE: each moved cell's new tiles take
       the newest serial its old tiles held, so a paint that the copy lacked
       before the move is still due after it, at the cell's new place.
       Allocated once, `max` cells, the first time a repack records a move,
       and never moved: the consumer reads it through the hand-over on the
       same thread. */
    int            moveList;
    TAGPU_GAFMOVE* moves;
    int            moveN;
    unsigned       moveSerial, movePrev;
    /* THE ALLOWANCE, which is what keeps a consumer's upload inside one frame.
       The owner sets `budget` (TAGPU_GAF_BUDGET) for an atlas whose consumer
       sends its tiles in the frame's own command buffer; 0 is no gate, the
       UI atlas's case.

       THE CONSUMER SAYS WHAT IT HOLDS (tagpu_gaf_atlas_ack): `ackSerial` is
       the mirror serial its copy is at, `ackKeep` 1 that its next upload will
       be the tiles written since -- 0 while it holds nothing, or will take the
       whole page anyway, and then there is nothing to bound. `due` is the
       count of tiles written after `ackSerial`, kept exact by every mirror
       write and recounted on every ack, so it is always what the consumer's
       next partial upload would carry.

       THE GATE (`atlas_insert`): a paint that would take `due` past `budget`
       is refused, and its entry is left reserved -- rect assigned, nothing
       painted, nothing sampled -- to be painted on a later ask. `due` never
       exceeds `budget` while the consumer keeps its copy, so a consumer's
       partial upload never exceeds the allowance, by construction.

       FIRST REFUSED, FIRST PAINTED. `tagpu_gaf_atlas_frame` puts every entry
       still waiting, and asked for on the frame before, into a bucket by the
       frame it was first refused on (`bkEpoch`, oldest first, `bkTiles` its
       cells' tiles); a paint then counts the buckets older than its own entry
       against the allowance, and a new entry counts all of them. So nothing
       waiting is overtaken by anything refused after it, and with nothing due
       the oldest is painted whatever its size (TAGPU_GAF_BUDGET's assertion).
       An entry nothing asked for on the frame before stops waiting and is
       simply reserved again.

       `wholeSerial` is the last write of the whole page (a create, a loss, a
       new level, the mirror's arm, a move with no list): a consumer behind it
       takes the page, outside play, and the gate is off until it has.
       `frame` counts `tagpu_gaf_atlas_frame` calls and is never 0 once one has
       run. `deferN` counts refusals, ever: a caller compares it across a
       lookup to tell a deferral from a failure. */
    int            budget;
    unsigned       ackSerial;
    int            ackKeep;
    unsigned       wholeSerial;
    int            due;
    unsigned       frame;
    unsigned       deferN;
    int            deferLive;          /* entries waiting now                 */
    int            nBk;
    unsigned       bkEpoch[TAGPU_GAF_BKMAX];
    int            bkTiles[TAGPU_GAF_BKMAX];
    /* THE TALLIES the deferral's log line reports, over the atlas's life:
       frames with a refusal, the bytes (the cells' RGBA8) first refused and the
       most in one frame, the most waiting at a frame's start, the longest wait
       in frames, entries that stopped waiting unasked, and paints that had
       waited. `dfThis` is this frame's first-refused bytes. */
    unsigned       dfFrames, dfMaxFrame, dfMaxBacklog, dfMaxWait, dfExpired, dfPainted;
    unsigned long long dfBytes;
    unsigned       dfThis, dfSeenN;
    /* THERE IS NO MIRROR OF THE RESTORED TWIN. `mirror` above is written by
       the paint, because the CPU holds the source bytes; restored texels are
       the RESTORER'S OUTPUT and exist only on the GPU. The Vulkan lane gets
       the published restore list below instead, and paints its own twin from
       it. */
    /* THE ANISOTROPY THE TWIN IS CONFIGURED FOR. Written every arm beat by
       `tagpu_r3d_atlas_restore_want` from `tagpu_classicpp_light()->aniso`.

       IT CARRIES THE KNOB, NOT THE APPLIED VALUE. The consumer stands the
       whole frame down when the two ends disagree, and the applied value
       `s_twinAniso` is NOT `aniso=`; it is `aniso=` CLAMPED BY THE DEVICE,
       and it is 0.0f wherever the extension is absent or the ceiling is lower.
       So a published constant would stand the frame down silently, on exactly
       the machines that can do least about it.

       The consumer therefore compares this against the knob it read rather
       than against what the device allowed (`s_twinAnisoWant`). The test then
       means "the two ends are configured apart", which is the one thing that
       can still go wrong -- a knob edited mid-session, after a sampler that
       cannot be rebuilt mid-frame -- and never "the device lacks a
       feature". */
    float          rgbAniso;
    /* THE PUBLISHED RESTORE LIST: how the Vulkan lane gets restored art. It
       runs the restore itself, so what crosses is the REQUEST rather than the
       picture.

       It holds the very frames this atlas queues for restore, in the order it
       queues them, because three of a frame's eleven numbers are
       content-dependent: `wrap` is tagpu_rglsl_tileable() over the frame's own
       texels against the ART palette, `key` is the frame header's, and the
       padding comes from the atlas's cell layout. Those are this side's facts;
       what crosses is their result.

       IT IS A LAZY QUEUE AND SO IT IS APPEND-ONLY WITH A GENERATION, which is
       what makes it different from terrain's whole-list-per-serial: a GAF
       atlas is fed one frame at a time, for the life of the atlas, so a
       consumer holds a CURSOR into this array and takes what is past it. A
       frame on which the consumer took nothing therefore costs nothing -- it
       takes more on the next one.

       `rlistGen` IS THE DISCONTINUITY and the only thing a cursor cannot
       survive. It is bumped whenever the array stops being a continuation of
       what a consumer already has, and the list is SIX events long: the
       arm, a recycle or a repack that publishes no moves (both of which drop
       the queue and blank the destination), `tagpu_gaf_atlas_lost`, a palette
       move, and the overflow restart below. A consumer that sees a new
       generation drops its own job and starts from index 0.
       A REPACK THAT PUBLISHES ITS MOVES (`moveList`) IS NOT ONE. It rewrites
       each moved entry's frame in place, at its new rect, and blanks the
       frames of entries it dropped or could not move (w and h 0, which a job
       skips); indices do not move, so the cursor stays good. The consumer
       then carries its restored twin's cells with the base atlas's and moves
       its own queued frames by the same list (tagpu_vk_feat.c), so a repack
       costs the twin nothing that was already restored.
       `rlistRepaint` is 1 only for the palette-move generation
       (`tagpu_gaf_atlas_restore_repalette`), where the destination already
       holds a restore and is recoloured in place. Only the UI atlas takes that
       path (tagpu_gui_surf.c, when the presented palette settles); for the
       feature, effects and unit atlases it is 0 on every generation, so their
       consumers blank the destination on each one, which is why they do not
       treat "no twin" as "draw nothing" (see `tagpu_vk_unit.c`'s restored
       gate).

       BOUNDED, because an append-only list fed for a session's length is not.
       The cap is four times the atlas's entry ceiling; reaching it RESTARTS
       the list from the entries the atlas holds right now (a new generation,
       repaint 0) rather than growing, so the memory is a function of `max`
       and the recovery is the same path as the arm. Armed by
       tagpu_gaf_atlas_restore_vk and NULL otherwise, so an atlas nobody asks
       pays nothing. */
    TAGPU_RGLSL_FRAME* rlist;
    int           rlistN, rlistCap;
    unsigned      rlistGen;
    int           rlistRepaint;
    /* HOW MANY TIMES THE DESTINATION HAS BEEN BLANKED, and it is a SEPARATE
       counter because `rlistRepaint` is a property of the latest generation
       while "you must blank" is a property of the INTERVAL since a consumer
       last looked. Two resets between two looks collapse into one: a recycle
       (repaint 0) followed in the same frame by a palette move (repaint 1) --
       which `tagpu_feat.c` can do, because it recycles a full atlas and then
       re-arms it on the next line -- would otherwise tell
       the consumer to KEEP a destination this lane has just cleared. A
       consumer blanks whenever this has moved, whatever the flag says. */
    unsigned      rlistBlanks;
    int           rlistWant;       /* armed                                  */
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
/* ...and which texels the frame actually DRAWS, 1 in `cov` (w*h bytes). An RLE
   frame's skip runs leave `out` at the key, and so does a literal texel whose
   value IS the key -- the engine draws that one and `out` alone cannot tell
   the two apart. A raw frame covers every texel: its transparency is the
   blitter's compare against a key, not a property of the plane. */
int tagpu_gaf_decode_cov(const unsigned char* g, int w, int h, unsigned char* out, unsigned char* cov);

/* Atlas: entry for a frame, decoding and uploading it on first sight. NULL
   when the frame is unreadable, when the atlas is full (it then flags itself
   and the next tagpu_gaf_atlas_reset recycles it — never mid-frame, or quads
   already emitted would point at re-used texels), or when its paint is
   DEFERRED past the allowance (`budget`; `deferN` moves). A caller draws
   nothing for a NULL, and one that draws a thing out of several entries
   draws none of it when any was deferred. */
const TAGPU_GAFENT* tagpu_gaf_atlas_get(TAGPU_GAFATLAS* a, const unsigned char* g);
/* The same entry from PRE-DECODED pixels (w*h bytes, colour key `ck`), keyed on
   (frame, pix, w, h, win) but never reading either pointer: the UI renderer's
   sprite ops carry their bytes across the thread boundary because the shell frees
   a popped screen's art under the render thread (gui-renderer.md 3.5). `find` is
   the lookup alone, NULL when absent.

   `win` is the SOURCE WINDOW and 0 means "the whole frame" -- which is what
   atlas_get and every 1:1 caller pass. It exists because a transformed draw can
   take a sub-rectangle of a frame, and (frame, pix, w, h) alone cannot tell two
   such windows apart. */
const TAGPU_GAFENT* tagpu_gaf_atlas_put(TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                        int w, int h, unsigned win,
                                        unsigned char ck, const unsigned char* pixels);
const TAGPU_GAFENT* tagpu_gaf_atlas_find(const TAGPU_GAFATLAS* a, const void* frame, const void* pix,
                                         int w, int h, unsigned win);
/* Recycle a full atlas. With `repack` set this RE-LAYS the entries it holds
   tallest-first and keeps them (moved in the mirror when painted, reserved
   and painted on demand otherwise); without
   it -- and always for a restart that is not "full", such as the UI atlas's
   re-arm -- it drops them and they re-decode in arrival order as before. */
void tagpu_gaf_atlas_reset(TAGPU_GAFATLAS* a);
/* Drop every entry and every repack decision: the atlas no longer describes
   anything the caller wants. For an atlas with `repack` set this is the only
   way back from the wall, and the owning pass owes it one call whenever the
   subject changes underneath -- `tagpu_feat.c` on a map change. Cheap, and
   correct to call when nothing has changed. */
void tagpu_gaf_atlas_forget(TAGPU_GAFATLAS* a);
void tagpu_gaf_atlas_lost(TAGPU_GAFATLAS* a);
/* give back every heap buffer an atlas owns (`mirror`, `rlist`, `moves`) and clear
   the latches that described them; for a caller about to re-lay the struct out
   from zero, which would otherwise drop the pointers */
void tagpu_gaf_atlas_free_buffers(TAGPU_GAFATLAS* a);
/* lay the atlas out now rather than on the first frame that atlases a sprite
   (tagpu_gaf_atlas_get otherwise does it on first use); 1 once `made` */
int  tagpu_gaf_atlas_create(TAGPU_GAFATLAS* a);

/* Ask for the CPU mirror above, and make it CORRECT FROM THE INSTANT IT
   EXISTS. A mirror allocated after the atlas has already painted frames would
   hold zeros where those frames are, and a backend uploading it would draw
   black trees for the rest of the session -- silently, because nothing in the
   atlas is wrong. So this marks every painted entry RESERVED (`ok` 0, `resv`
   1), which is the state a repack leaves an entry in: the rect stays where it
   is and the next tagpu_gaf_atlas_get for that frame paints it again, into
   the mirror. The atlas re-converges over the next frames at
   the cost of one RLE decode per frame still on screen.

   Render thread only, like the rest of this module. Returns 0 if the memory
   was refused, and the atlas then goes on working without one. Idempotent. */
int  tagpu_gaf_atlas_mirror(TAGPU_GAFATLAS* a);

/* WHAT CHANGED since a consumer's copy: every tile of `dirty` (an atlas's
   `dirty`, `now` its `mirrorSerial`) written after serial `since`, as up to
   `maxr` rects x0, y0, x1, y1 (exclusive), clipped to `rows` x `dim`. Each
   strip of TAGPU_GAF_TILEH rows gives one rect per run of dirty tiles, and a
   run directly under an identical one extends it. Past `maxr` rects the last
   one grows over the rest, so the answer is always a superset of the
   writes: larger, never wrong. The count, 0 when nothing changed; -1 when
   `since` is ahead of `now`, which is a copy of something else and means
   "send the whole page". Pure: it reads the map and nothing else. */
int  tagpu_gaf_dirty_since(const unsigned* dirty, unsigned now, unsigned since,
                           int dim, int rows, int (*rect)[4], int maxr);

/* WHAT A CONSUMER'S COPY OF THE MIRROR IS DUE, as rects for
   tagpu_vk_stage_expand_rects. `keep` 1 says the copy holds this mirror's
   texels as of serial `since`; the answer is then the tiles written since.
   `keep` 0 -- the consumer holds nothing, or holds another palette's colours
   -- or a `since` the map cannot answer for, is the whole page as one rect of
   `dim` x `rows`. The count, 0 when nothing is due. */
int  tagpu_gaf_rects_due(const unsigned* dirty, unsigned now, unsigned since, int keep,
                         int dim, int rows, int (*rect)[4], int maxr);
/* The texels `n` such rects cover, overlaps counted twice. */
unsigned long long tagpu_gaf_rects_area(const int (*rect)[4], int n);

/* THE ALLOWANCE'S CALLS (`budget` above), render thread only.
   `_frame` once per producer frame, after any reset and before the first
   lookup: it opens the frame the gate counts in and builds the backlog.
   `_ack` from the consumer whenever its copy changes: `keep` 1 with the serial
   it now holds, 0 when it holds nothing (a new image, a teardown).
   `_owed` is 1 while the consumer keeps a copy behind the mirror. The consumer
   uploads on every such frame WHETHER OR NOT IT DRAWS: the gate waits on the
   consumer, so a consumer that waited on a draw could wait on the gate -- a
   frame whose every feature is deferred draws nothing, and would never upload
   the tiles that stand in the way. */
void tagpu_gaf_atlas_frame(TAGPU_GAFATLAS* a);
void tagpu_gaf_atlas_ack(TAGPU_GAFATLAS* a, unsigned serial, int keep);
int  tagpu_gaf_atlas_owed(const TAGPU_GAFATLAS* a);

/* WHAT A CONSUMER UPLOADS FROM: the mirror, its key plane and dirty map, the
   rows in use (the shelf cursor bounds every cell), the serials and the last
   repack's moves -- read in one place so that the draw's hand-over and the
   upload a consumer owes on a frame it does not draw cannot disagree. All
   the atlas's own buffers, alive as long as it is. 0 and zeroes while there
   is no mirror. */
typedef struct TAGPU_GAFVIEW {
    const unsigned char* idx;
    const unsigned char* key;
    const unsigned*      dirty;
    int                  dim, rows;
    unsigned             serial, whole;
    const TAGPU_GAFMOVE* moves;
    int                  moveN;
    unsigned             moveSerial, movePrev;
} TAGPU_GAFVIEW;
int  tagpu_gaf_atlas_view(const TAGPU_GAFATLAS* a, TAGPU_GAFVIEW* v);

/* THE RULE FOR ANYTHING THAT HANDS RESTORED TEXELS ACROSS A THREAD: step it
   where the paint is already visible to this frame's draws -- after the
   restorer has run and before the ops that sample the twin -- so that what
   crosses is byte-identical to what those ops sampled rather than a frame
   behind them. The list below needs no such rule: the Vulkan lane paints
   from it for itself. */

/* Ask for the PUBLISHED RESTORE LIST. It arms only while Classic++'s
   `assets=` knob is on -- the master arm and the key the render-options
   screen's `Undithered assets` row writes -- so one question decides whether
   the art is restored and no second lever has to be armed by hand. Poll it on
   the same beat as the mirror -- the knob can move mid-session.

   Seeded with every entry the atlas holds right now, so it is correct from the
   instant it exists in the same sense the mirror is: a consumer starting from
   index 0 restores exactly what this atlas holds, whatever has already been
   painted here. Returns 1 when armed (and on every later call), 0 when the
   lever is absent or the memory was refused -- and the atlas then goes on
   working without one. Render thread only. */
int  tagpu_gaf_atlas_restore_vk(TAGPU_GAFATLAS* a);

/* THE PALETTE MOVED UNDER THE RESTORED TEXELS, so every frame of the list has
   to be painted again against the new one -- the path `rlistRepaint`'s comment
   above describes, which the UI atlas takes.
   A REPAINTING generation: the destination KEEPS what it holds and the
   restorer overwrites it frame by frame, so nothing is blanked and the art
   does not flash through a cleared image on the way. `rlistBlanks` therefore
   does not move, which is exactly the fact a consumer's `repaint` test reads.
   A no-op on an atlas with no list. Render thread only. */
void tagpu_gaf_atlas_restore_repalette(TAGPU_GAFATLAS* a);


/* A RESTORED TWIN IS THE WHOLE MIP CHAIN when the atlas is mipped, because it
   is sampled LINEAR_MIPMAP_LINEAR and a consumer holding level 0 alone draws a
   different picture wherever the art is minified -- which on the unit atlas is
   ordinary play. Level L is `dim >> L` square, RGBA8, at `tagpu_gaf_mip_off`;
   the whole chain is `tagpu_gaf_mip_chain` bytes. These two are the layout
   contract: `tagpu_vk_restore.c` sizes its dump with them, and the consuming
   lane knows its own chain depth from `restoreMips`. */

size_t tagpu_gaf_mip_bytes(int dim, int level);
size_t tagpu_gaf_mip_off(int dim, int level);
size_t tagpu_gaf_mip_chain(int dim, int mip);
#endif
