#ifndef TAGPU_GUI_INT_H
#define TAGPU_GUI_INT_H
/* tagpu_gui_int.h — the queue between the game thread (tagpu_gui_hook.c,
   the observers) and the render thread (tagpu_gui_surf.c, the twins).
   Private to the tagpu_gui_* family. Design: gui-renderer.md 3.5, 3.6.

   ONE PRODUCER, ONE CONSUMER, NO LOCK. The game thread appends published ops
   at every census (tagpu_gui_hook.c publishes at the flip, throttled) and the
   render thread drains everything queued at every present. Indices are
   volatile and monotonic; the arena is a byte ring the ops point into.
   Overflow on either side is not an error to recover from op by op: the
   producer drops the frame, marks every surface for re-seeding and the next
   flip publishes fresh seeds (gui-renderer.md 3.5, "the overflow policy"). */

enum {
    PK_FRAME = 1,   /* a flip: `surf` is the presented surface                */
    PK_RESET,       /* forget every twin (TAGPU_GUI_WHY_*: re-arm, overflow)  */
    PK_SEED,        /* `surf` w x h: its bytes follow in the arena            */
    PK_FREE,        /* `surf` is gone                                          */
    PK_CLEAR,       /* transparent over the box (the viewport's key fill)     */
    PK_SPRITE,      /* a plain keyed GAF blit: frame identity, first sight carries its bytes */
    PK_COPY,        /* twin -> twin, the source's box at (l, t)               */
    PK_PIXELS,      /* the box's bytes follow in the arena (everything else)  */
    PK_STRING,      /* TA's own glyphs, stamped by us — the string              */
                    /* follows in the arena and the font/colours ride along     */
    PK_BAR,         /* a SOLID rectangle of one palette index -- `fg` is the
                       index, the box is `l,t,r,b` inclusive, and NOTHING follows
                       in the arena. That last part is the point: a box of bytes
                       copied out of the surface AT THE FLIP would publish
                       whatever was drawn over it in between. A colour and a box
                       are both smaller and correct. */
    PK_RECT         /* a HOLLOW rectangle of one palette index -- `fg` is the
                       index, `l,t,r,b` are the OUTER box and every edge is
                       inclusive, and the INTERIOR IS NOT TOUCHED. As `PK_PIXELS`
                       this op would publish its whole box, interior included,
                       read out of the surface at the flip: pixels the op never
                       wrote, from a moment after it ran. `0x4BF7B0` does NOT
                       produce this -- it tints, and produces `PK_TINT` below. */,
    PK_ASSET        /* a DECODED ASSET SURFACE, whole: `w`/`h`/`pitch` as a seed,
                       and its bytes follow in the arena. It is NOT `PK_SEED`,
                       and the difference is the whole reason it may cross where
                       a seed may not: a seed carries what the 1997 rasteriser
                       COMPOSED, and this carries a surface the engine's LOADER
                       decoded and that nothing ever draws into -- the same
                       category as the GAF frame bytes a `PK_SPRITE`'s first
                       sight already carries. The producer only ever emits it
                       for a surface whose `isAsset` still stands, and `op_add`
                       clears that the instant an op covers a pixel of the
                       surface as a DESTINATION.

                       WHAT THAT CHECKS AND WHAT IT DOES NOT: the check is over the SEVENTEEN LEAVES, so it says no
                       OBSERVED draw named the surface. The loader that fills it
                       is itself an unhooked write path -- that is the whole
                       premise -- so an engine path that re-filled a claimed
                       surface without passing a leaf would leave the twin
                       holding older bytes with nothing to re-offer. Wrong
                       picture, never a crash. `s_assetDrift` in the hook
                       measures it under `census.on` and reads 0 -- but read
                       that as NO DRIFT AFTER THE ACK, not as no drift since the
                       loader: the census refreshes each surface's shadow every
                       pass and the test needs `assetSent`, which lands one to
                       three presents after the bytes were taken, so movement
                       inside that gap is folded into the shadow unseen. The
                       hole is named and bounded rather than papered over. Being stable is
                       also what makes reading it AT THE FLIP exact, where the
                       same read for `PK_PIXELS` is a box of bytes from a moment
                       later than the draw it stands for. */,
    PK_SHADE,       /* the hand-over's remap table, `TAGPU_GUI_SHADE_ROWS`
                       rows of 256 bytes, in the arena: the engine's LIGHTEN
                       table `globals+0xC8` whole, then the box shader's
                       per-level rows derived from it and from PALETTE.SHD
                       `globals+0xC4` (inc/tagpu_gui.h has the layout). It is a
                       palette-derived REMAP and not a picture -- the same
                       category as the palette itself, which has always crossed
                       -- so it is on the allowed side of the clean cut for the
                       reason `PK_ASSET` is: nothing composed it.

                       PUBLISHED AHEAD OF THE FIRST `PK_TINT` OF A BATCH AND AT
                       MOST ONCE PER RESET, and the ordering is the whole
                       delivery argument. The queue is FIFO and the drain
                       applies in order, so a tint that reaches the consumer
                       has the table it indexes; a batch that runs out of room
                       loses the table AND the tints behind it together, and
                       raises the reset that re-arms `s_lhtSent`. No ack, no
                       serial, no retry -- the things `PK_ASSET` needed because
                       ITS payload had to survive a consumer that was not
                       recording. This one lands in a file-static the render
                       half keeps, which the hand-over then carries by pointer
                       every frame, so an abandoned mirror frame cannot lose
                       it. */
    PK_TINT         /* A REMAP OF THE BOX -- `l,t,r,b` inclusive, `fg` the
                       ROW of `PK_SHADE`'s table, and NOTHING follows in the
                       arena. Two engine writers produce it: ONE EDGE of a
                       focus rectangle (one pixel thick, a lighten-table row),
                       and one call of the box shader `0x4BF4D0` (any size, a
                       row from `TAGPU_GUI_SHADE_BOX` on).

                       It is a READ-MODIFY-WRITE: `0x4CC8DF` does `dst =
                       LUT[row*256 + dst]` per pixel and `0x4BF4D0` the same
                       with a signed `dst`, so there is no colour to name. What
                       crosses instead is the OPERATION -- a box, a row, and
                       (once) the table -- and the consumer applies it to its
                       own twin. No engine pixel is involved at any point.

                       A FOCUS RECTANGLE IS ONE EDGE PER OP, NOT ONE BOX.
                       `0x4BF7B0` draws top, right, bottom then left through
                       four separate `0x4BEC70` calls, each clipped on its own
                       by `0x4BEA20`, and the four CORNERS are therefore tinted
                       TWICE -- `LUT[row][LUT[row][x]]`. Publishing the box
                       would have to carry both the clip rect and that overlap
                       rule; four ops carry them by construction, in the
                       engine's own order, and `op_add`'s existing clip drops
                       an edge that falls outside exactly as `0x4BEA20` does.
                       The box shader clips its box once (`0x4BF620`), so it is
                       one op. */,
    PK_MOVIE        /* A SMACKER FRAME: the box `l,t,r,b` of the movie's own
                       surface, and its bytes follow in the arena. On the
                       allowed side of the clean cut for `PK_ASSET`'s reason:
                       the bytes are what `SmackDoFrame` DECODED, and nothing
                       of the 1997 rasteriser's is in them.

                       EXACT BY ORDERING, NOT BY TIMING. The frame routine
                       `0x47C3A0` calls `SmackDoFrame` at `0x47C44A` and the
                       flip at `0x47C450`, the next instruction, so at the
                       flip's entry the box holds the decoded frame and nothing
                       else; the engine's cursor is blitted inside the flip,
                       after the observer has read it. The producer records the
                       op at that entry and publishes it IN THE SAME CALL --
                       the flip that recorded one bypasses the census cadence
                       -- and every exit that does not publish clears the
                       window, so the op never outlives the flip that recorded
                       it and its bytes are never read at any other moment.
                       The PALETTE does not cross with them: the lane resolves
                       the box through the palette live at its present, which
                       is why `movie_frame` never skips a frame that brings a
                       new one.

                       The drain mirrors it as `TAGPU_GUIOP_PIXELS` with its
                       bytes, the shape the Vulkan lane already validates. */,
    PK_PLANE        /* A TRANSFORMED GAF STAMP, AS THE INDICES IT LEAVES: the
                       box `l,t,r,b` and its bytes in the arena, row by row. The
                       bytes are `scale_capture`'s resample of the frame's own
                       art, decoded inside the engine's `0x4C7580` call -- the
                       class a sprite's plane is -- and never the composed
                       surface, which is `PK_PIXELS` and dropped.

                       A BOX AND NOT A SPRITE because that is what the engine
                       writes: the span `0x4C7310` copies every texel of the
                       rectangle, key colour included (`0x4C74C7`..`0x4C74CE`,
                       no compare), so there is nothing to key and no atlas
                       entry to name. An entry would need a key saying which
                       picture it holds, and SELMAP's preview changes picture
                       under one frame address on every pick.

                       The drain mirrors it as `TAGPU_GUIOP_PIXELS`, as it does
                       `PK_MOVIE`. */
};

typedef struct TAGPU_PUBOP {
    unsigned char  kind;
    unsigned char  ck;              /* sprite: colour key                      */
    /* string: the three colour arguments of 0x4CCF60, as BYTES — the
       blitter takes all three with `mov al/ah, BYTE PTR [ebp+…]` and compares
       them 8-bit (`cmp al,ah` at 0x4CCFE2), so the low byte is the whole of
       what it uses and storing an int here would only invite a wider compare
       than the engine's [BINARY-VERIFIED 2026-09-09] */
    unsigned char  fg, bg, tr;
    unsigned short fw, fh;          /* sprite: frame size                      */
    unsigned       surf;            /* destination surface (its pixel base)    */
    unsigned       src;             /* copy: source surface                    */
    short          l, t, r, b;      /* destination box, inclusive, surface px  */
    short          sl, st;          /* copy: source top-left; sprite: dst pos;
                                       string: the x and y the blitter was GIVEN
                                       (y before the font's own -font[2])       */
    int            w, h, pitch;     /* seed: the surface's geometry             */
    const void*    frame;           /* sprite: the key (header, pixel ptr).
                                       NOT USED BY A STRING: the engine's FONT
                                       OBJECT must not cross, because the
                                       render thread would dereference it up
                                       to a queue backlog later with nothing
                                       establishing a UI font's lifetime. A
                                       string carries `font_id` and the glyph
                                       BITS below instead.                      */
    const void*    pix;
    unsigned       aoff, alen;      /* arena bytes: seed / pixels / a sprite's first sight / a string's block */
    /* ---- string: the font as an identity and its glyphs as bits.
       The arena block is `gcount` glyph records followed by the NUL-terminated
       string; each record is `{u8 code, u8 w, u16 nbytes, u8 bits[nbytes]}`,
       4-byte aligned, with nbytes = (rows * w + 7) / 8 — the same packed rows
       the engine's blitter reads, copied on the GAME thread where the font is
       live. A glyph is sent on FIRST SIGHT of its (font, code) pair and never
       again, so a steady screen's ops carry the string alone.

       `font_id` is a number this fork assigns per (font pointer, signature),
       never an address: the consumer's glyph cache keys on it and has nothing
       to dereference. */
    unsigned       font_id;
    unsigned short gcount;          /* glyph records at the head of the block   */
    unsigned char  font_rows;       /* font[0], the rows the blitter writes     */
    signed char    font_yoff;       /* font[2], subtracted from y               */
    unsigned       assetTok;        /* PK_ASSET: the offer's one-time token     */
    unsigned       flip;            /* the flip this belongs to (diagnostics)   */
} TAGPU_PUBOP;

/* `TAGPU_GUI_TABLE_ROWS` (either engine table's shape, and the argument that
   the shape is the bound) and `TAGPU_GUI_SHADE_ROWS` / `_BYTES` (the remap
   table `PK_SHADE` carries) are in the PUBLIC header
   (`inc/tagpu_gui.h`), beside the `shade` pointer the hand-over carries,
   because the Vulkan lane needs them and this header is private to the
   tagpu_gui_* family. */

#define TAGPU_GUI_ASSET_TRIES 240u           /* offers before an asset is given up on */
#define TAGPU_GUI_QCAP   (1u << 16)          /* ops                            */
#define TAGPU_GUI_ASIZE  (16u << 20)         /* arena bytes                    */

typedef struct TAGPU_GUIQ {
    TAGPU_PUBOP*   ops;                      /* TAGPU_GUI_QCAP                 */
    unsigned char* arena;                    /* TAGPU_GUI_ASIZE                */
    volatile unsigned qHead, qTail;          /* producer writes head, consumer tail */
    volatile unsigned aHead, aTail;          /* arena bytes, same roles        */
    /* THE ASSET HANDSHAKE. A `PK_ASSET` is published once and its payload is
       large, so the producer must know whether it LANDED rather than assume it
       did -- the consumer's mirror is not recording on every frame (it follows
       the Vulkan pass) and `mir_bytes` refuses silently when it is not, which
       can lose the shell backdrop on the one frame that matters.

       WHAT THE CONSUMER ECHOES IS A ONE-TIME TOKEN, NOT THE SURFACE'S BASE, and
       that is the whole safety of it. A base is not an identity here: this
       module frees a surface and the engine's next `0x4C69F0` lands on the same
       block (MEASURED -- see `surf_drop_offscreens`), so an ack left standing
       from a DEAD surface would be matched by the live one that inherited its
       address, the offer skipped, and the screen that replaced it drawn black.
       The same word would also let an in-flight echo from the PREVIOUS episode
       land after a reseed had cleared it. A token is issued once per offer and
       never reissued, so neither a recycled base nor a late echo can satisfy an
       offer that was not made.

       ONE WRITER EACH WAY: the consumer writes `assetAck`, the producer only
       reads it. The producer does NOT clear it -- it does not need to, because a
       token it never issued cannot match. The echo is published by `mir_finish`
       when the record is actually handed over, not when the op entered it, so a
       record that is later thrown away acks nothing.

       `mirArmed` IS THE THROTTLE AND THE TOKEN IS THE CORRECTNESS, and keeping
       those two apart is what makes the retry a state machine rather than a
       clock. The consumer publishes `mirArmed` from the ONE place that arms or
       disarms its mirror (`tagpu_gui_mirror_want`), and the producer refuses to
       compose an offer while it reads 0. Without it, a shell sitting in front of
       an unarmed Vulkan lane re-publishes a 300 KB backdrop every present until
       the try count runs out -- up to ~72 MB of game-thread memcpy per episode
       for bytes with no consumer. A STALE READ COSTS AT MOST ONE FRAME: armed
       read as disarmed skips that present's offer and the next one makes it
       again; disarmed read as armed spends one offer nobody acks, which is the
       case that already had to be handled. It can never LOSE the asset, because
       nothing here retires an offer -- only the consumer's echo does.

       `TAGPU_GUI_ASSET_TRIES` is then a backstop rather than the mechanism: it
       bounds the offers within one episode against a lane that is armed but
       never records (every present abandoned for want of room), and a reseed
       starts a fresh count, so it is not a bound on lifetime traffic. */
    volatile unsigned mirArmed;              /* consumer: its mirror is recording  */
    volatile unsigned assetAck;              /* consumer: the token it last CARRIED */
    volatile unsigned reseed;                /* consumer asks the producer to seed everything */
    volatile unsigned why;                   /* the last reason `reseed` (or an overflow) was raised: TAGPU_GUI_WHY_* */
    volatile unsigned overflows;             /* the producer ran out of queue or arena  */
    volatile unsigned resets;                /* fresh starts published (TAGPU_GUI_WHY_*) */
    /* ASK THE ENGINE TO REDRAW ITS SCREENS, AND NOTHING ELSE -- plus the
       re-seed of any snapshot surface, which is the one surface that redraw
       cannot repaint (`repaint_arm`). Raised by the render half when Classic++
       colour becomes valid and whenever the UI's picture store settles a
       picture: colour reaches a twin only through the op that DRAWS the art,
       so a surface already painted keeps its indices until something repaints
       it -- in game that is the whole sidebar, which the engine draws once per
       selection change.
       DELIBERATELY NOT `reseed`. A reseed resets the twin store and the UI
       atlas, which re-arms the restore list, which clears the consumer's
       `arHave`, which clears colour validity -- and raising a reseed on the
       validity EDGE then closes that loop: measured 2026-09-22, the UI
       oscillated and the layer composited nothing at all (a magenta frame).
       This counter asks for the engine's repaint alone, which is the half
       that redraws every gadget as sprites and so carries colour.
       RAISED ON THE RENDER THREAD, shadowed on the GAME thread in
       `repaint_arm` -- unlike `resets`, which never leaves the game thread, so
       the two are not the same shape however alike the code reads. Safe by its
       own construction rather than by that analogy: one writer, one aligned
       word, monotone, never reset, and read as an inequality against the
       shadow, so the game thread can be one repaint late and never wrong. */
    volatile unsigned colarm;
    /* GAF ops that fell back to their box's bytes because the sprite's decoded
       plane was not in hand at publish -- which can only mean the observe-time
       scratch was full when the blit was seen (`gaf_capture`). The hash and
       the plane are both taken inside the engine's own blit and `publish`
       dereferences nothing, so no gate refuses a sprite on this path.
       It lives here, beside the other producer counters, because this is what
       the render half's heartbeat prints. */
    volatile unsigned gafnoplane;
    /* the observe-time scratch, so its bound can be judged rather than assumed:
       `gafhigh` is the most bytes any one census window has wanted and `gaflost`
       the planes it could not take. A `gaflost` that is not 0 is the only way
       `gafnoplane` can move, and both being 0 over a session is what says the
       2 MB is not a guess that happens to hold. */
    volatile unsigned gafhigh, gaflost;
    /* frames the DECODER refused (unreadable or malformed), counted apart from
       `gaflost` because that one is a statement about the scratch's BOUND and
       this one is not. */
    volatile unsigned gafbaddec;
    /* sprite ops that published their box because a RESET cleared the seen table
       between the blit and the flip. CHEAP, not free: each costs a PK_PIXELS
       box in the arena and one window without its atlas identity -- in a publish
       that is already re-seeding every surface whole, so nothing is on screen
       that would not have been, and it self-heals next window. Counted apart
       from `gafnoplane` so the one number that means a real failure keeps
       meaning it. */
    volatile unsigned gafreseed;
    /* text ops that published their box because the glyph `sent[]` table was
       re-armed between the capture and the flip -- the OP_TEXT twin of
       `gafreseed`, and cheap in the same way (a PK_PIXELS box, one window
       without its stamped glyphs, self-healing next window). */
    volatile unsigned strrearm;
    /* the glyph scratch, so its bound can be judged rather than assumed:
       `glyhigh` is the most bytes any one census window has TAKEN of it -- a
       refused block is counted in `glylost` and contributes nothing here, so
       the two are read together and `glyhigh` alone is not demand -- and
       `glylost` the blocks it could not take whole. A settled session should
       hold both still -- `sent[]` means a (font, code) pair is captured once -- 
       and `glylost` at 0 is what says the 128 KB is not a guess that happens to
       hold. A block the scratch refuses publishes its box. */
    volatile unsigned glyhigh, glylost;
    volatile unsigned stalls;                /* episodes where the consumer took nothing for TAGPU_GUI_STALL_MS
                                                while work was queued (a display-mode switch kills the render
                                                thread): the producer drops its batches until it moves again */
} TAGPU_GUIQ;

/* the producer's font-slot counters, for the render half's heartbeat: glyph
   records published, re-arms of every `sent[]` table, fonts refused as not that
   format, and slot-table recycles */
void tagpu_gui_font_stats(unsigned* glyphs, unsigned* resends, unsigned* refused, unsigned* recycles);

/* why a fresh start was raised — logged by the producer with every reset it
   publishes (`log`), so a count of resets is never a mystery */
enum {
    TAGPU_GUI_WHY_NONE = 0,
    TAGPU_GUI_WHY_ARM,        /* the trigger (re)appeared: the twins start from the surfaces as they are */
    TAGPU_GUI_WHY_QUEUE,      /* the op ring was full                                                    */
    TAGPU_GUI_WHY_ARENA,      /* the byte arena was full                                                 */
    TAGPU_GUI_WHY_BOX,        /* a recorded box no longer fits its surface                              */
    TAGPU_GUI_WHY_LOST,       /* a sprite arrived without its bytes and the atlas had no entry           */
    TAGPU_GUI_WHY_ATLAS,      /* the UI atlas was full                                                   */
    TAGPU_GUI_WHY_COPY,       /* a copy from a source with no twin                                       */
    TAGPU_GUI_WHY_STALL,      /* the consumer came back after a stall: what was dropped is re-seeded    */
    TAGPU_GUI_WHY_STRING,     /* a string op stamped nothing (an unreadable font): the text is missing  */
    TAGPU_GUI_WHY_LEVEL,      /* the level changed: the UI atlas keys on frame ADDRESSES and they recycle */
    TAGPU_GUI_WHY_N
};
/* a consumer that has taken nothing for this long while ops were queued is not
   slow, it is gone (MEASURED 2026-09-07: cnc-ddraw stops its render thread
   across every SetDisplayMode, and the shell flips ~5 000 times a second
   meanwhile — without this the producer filled the arena, overflowed, reset,
   re-seeded into the full arena and repeated at the publish cadence: 38
   overflows and 39 resets per game -> shell switch). A frame hitch is shorter. */
#define TAGPU_GUI_STALL_MS 250

extern TAGPU_GUIQ g_guiq;                    /* owned by tagpu_gui_hook.c      */

/* the producer's view of the arena: room for `len` bytes ahead of the consumer */
static __inline int tagpu_guiq_arena_room(const TAGPU_GUIQ* q, unsigned len, unsigned* at)
{
    unsigned head = q->aHead, tail = q->aTail;
    if (len >= TAGPU_GUI_ASIZE) return 0;
    if (head >= tail) {
        if (TAGPU_GUI_ASIZE - head >= len) { *at = head; return 1; }
        if (tail > len) { *at = 0; return 1; }             /* wrap            */
        return 0;
    }
    if (tail - head > len) { *at = head; return 1; }
    return 0;
}
#endif
