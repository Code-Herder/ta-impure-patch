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
    PK_RESET,       /* forget every twin (re-arm, GL context change)          */
    PK_SEED,        /* `surf` w x h: its bytes follow in the arena            */
    PK_FREE,        /* `surf` is gone                                          */
    PK_CLEAR,       /* transparent over the box (the viewport's key fill)     */
    PK_SPRITE,      /* a plain keyed GAF blit: frame identity, first sight carries its bytes */
    PK_COPY,        /* twin -> twin, the source's box at (l, t)               */
    PK_PIXELS,      /* the box's bytes follow in the arena (everything else)  */
    PK_STRING,      /* G17d: TA's own glyphs, stamped by us — the string        */
                    /* follows in the arena and the font/colours ride along     */
    PK_BAR,         /* landing 8a: a SOLID rectangle of one palette index --
                       `fg` is the index, the box is `l,t,r,b` inclusive, and
                       NOTHING follows in the arena. That last part is the point:
                       as `PK_PIXELS` this op copied its whole box out of the
                       surface, and did so AT THE FLIP, so anything drawn over it
                       in between was what got published. A colour and a box are
                       both smaller and correct. */
    PK_RECT         /* landing 8b: a HOLLOW rectangle of one palette index --
                       `fg` is the index, `l,t,r,b` are the OUTER box and every
                       edge is inclusive, and the INTERIOR IS NOT TOUCHED. As
                       `PK_PIXELS` this op published its whole box, interior
                       included, read out of the surface at the flip: it carried
                       pixels the op never wrote, from a moment after it ran.
                       `0x4BF7B0` does NOT produce this -- it tints. */,
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

                       WHAT THAT CHECKS AND WHAT IT DOES NOT, stated because the
                       shorter version of this sentence was an overclaim: the
                       check is over the SEVENTEEN LEAVES, so it says no
                       OBSERVED draw named the surface. The loader that fills it
                       is itself an unhooked write path -- that is the whole
                       premise -- so an engine path that re-filled a claimed
                       surface without passing a leaf would leave the twin
                       holding older bytes with nothing to re-offer. Wrong
                       picture, never a crash. `s_assetDrift` in the hook
                       measures exactly that under `census.on` and reads 0; the
                       hole is named rather than papered over. Being stable is
                       also what makes reading it AT THE FLIP exact, where the
                       same read for `PK_PIXELS` is a box of bytes from a moment
                       later than the draw it stands for. [Landing: the shell
                       backdrop, 2026-09-21.] */
};

typedef struct TAGPU_PUBOP {
    unsigned char  kind;
    unsigned char  ck;              /* sprite: colour key                      */
    /* string (G17d): the three colour arguments of 0x4CCF60, as BYTES — the
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
                                       NOT USED BY A STRING any more: it held
                                       the engine's FONT OBJECT, which the
                                       render thread then dereferenced up to a
                                       queue backlog later, behind probes and
                                       with no note establishing a UI font's
                                       lifetime. Landing 4c replaced it with
                                       `font_id` and the glyph BITS below.      */
    const void*    pix;
    unsigned       aoff, alen;      /* arena bytes: seed / pixels / a sprite's first sight / a string's block */
    /* ---- string (landing 4c): the font as an identity and its glyphs as bits.
       The arena block is `gcount` glyph records followed by the NUL-terminated
       string; each record is `{u8 code, u8 w, u16 nbytes, u8 bits[nbytes]}`,
       4-byte aligned, with nbytes = (rows * w + 7) / 8 — the same packed rows
       the engine's blitter reads, copied on the GAME thread where the font is
       live. A glyph is sent on FIRST SIGHT of its (font, code) pair and never
       again, so a steady screen's ops carry the string alone.

       `font_id` is a number this fork assigns per (font pointer, signature),
       never an address: the consumer's glyph cache keys on it and no longer
       has anything to dereference. */
    unsigned       font_id;
    unsigned short gcount;          /* glyph records at the head of the block   */
    unsigned char  font_rows;       /* font[0], the rows the blitter writes     */
    signed char    font_yoff;       /* font[2], subtracted from y               */
    unsigned       assetTok;        /* PK_ASSET: the offer's one-time token     */
    unsigned       flip;            /* the flip this belongs to (diagnostics)   */
} TAGPU_PUBOP;

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
       lost the shell backdrop on the one frame that mattered.

       WHAT THE CONSUMER ECHOES IS A ONE-TIME TOKEN, NOT THE SURFACE'S BASE, and
       that is the whole safety of it. The first cut echoed the base, and a base
       is not an identity here: this module frees a surface and the engine's next
       `0x4C69F0` lands on the same block (MEASURED -- see `surf_drop_offscreens`),
       so an ack left standing from a DEAD surface was matched by the live one
       that inherited its address, the offer was skipped, and the screen that
       replaced it drew black. The same word also let an in-flight echo from the
       PREVIOUS episode land after a reseed had cleared it. A token is issued
       once per offer and never reissued, so neither a recycled base nor a late
       echo can satisfy an offer that was not made. [Both found by the landing
       review of this commit; both were the pre-landing symptom coming back.]

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
       an unarmed Vulkan lane re-published a 300 KB backdrop every present until
       the try count ran out -- up to ~72 MB of game-thread memcpy per episode
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
    volatile unsigned resets;                /* fresh starts published (re-arm, GL, overflow, a lost frame) */
    /* GAF ops that fell back to their box's bytes because the sprite's decoded
       plane was not in hand at publish -- which since G19f-7 can only mean the
       observe-time scratch was full when the blit was seen (`gaf_capture`).
       **THIS REPLACES `gafstale`**, which counted the level-generation gate that
       used to stand on this path. That gate existed to stop `publish`
       dereferencing a freed GAF bank; the hash and the plane are now both taken
       inside the engine's own blit, `publish` dereferences nothing, and a gate
       with nothing left to protect was refusing ops -- 215 of them at a single
       level end, each losing its sprite identity for no remaining reason. The
       name changed with the meaning on purpose: `gafstale=215` is the figure
       landing 5's A/B is stated in, and a counter that keeps its name while
       measuring something else is how those numbers would quietly stop meaning
       what the notes say they mean.
       It lives here, beside the other producer counters, because this is what
       the render half's heartbeat prints: the first version put it in a `gui
       census:` line that an ordinary run never emits, so the one figure that
       says the mechanism is behaving was invisible.
       [gafstale FOUND 2026-09-16, the landing-5 review; replaced the same day.] */
    volatile unsigned gafnoplane;
    /* the observe-time scratch, so its bound can be judged rather than assumed:
       `gafhigh` is the most bytes any one census window has wanted and `gaflost`
       the planes it could not take. A `gaflost` that is not 0 is the only way
       `gafnoplane` can move, and both being 0 over a session is what says the
       2 MB is not a guess that happens to hold. */
    volatile unsigned gafhigh, gaflost;
    /* frames the DECODER refused (unreadable or malformed), counted apart from
       `gaflost` because that one is a statement about the scratch's BOUND and
       this one is not. They shared a counter until the landing review read the
       declaration against its two call sites. */
    volatile unsigned gafbaddec;
    /* sprite ops that published their box because a RESET cleared the seen table
       between the blit and the flip. CHEAP, not free: each costs a PK_PIXELS
       box in the arena and one window without its atlas identity -- in a publish
       that is already re-seeding every surface whole, so nothing is on screen
       that would not have been, and it self-heals next window. Counted apart
       from `gafnoplane` so the one number that means a real failure keeps
       meaning it. ["free" corrected by the landing review.] */
    volatile unsigned gafreseed;
    /* text ops that published their box because the glyph `sent[]` table was
       re-armed between the capture and the flip -- the OP_TEXT twin of
       `gafreseed`, and cheap in the same way (a PK_PIXELS box, one window
       without its stamped glyphs, self-healing next window). It replaces
       `strstale`, which counted the LEVEL-generation refusals of the gate that
       stood in for the font's lifetime until G19f-8 moved the reads into the
       observer; the name changed with the meaning, as `gafstale`'s did, so that
       a figure quoted in the notes cannot quietly start measuring something
       else. */
    volatile unsigned strrearm;
    /* the glyph scratch, so its bound can be judged rather than assumed:
       `glyhigh` is the most bytes any one census window has TAKEN of it -- a
       refused block is counted in `glylost` and contributes nothing here, so
       the two are read together and `glyhigh` alone is not demand [the word
       was "wanted", which contradicted the code, found by the landing review]
       -- and `glylost` the blocks it could not take whole. A settled session should
       hold both still -- `sent[]` means a (font, code) pair is captured once -- 
       and `glylost` at 0 is what says the 128 KB is not a guess that happens to
       hold. A block the scratch refuses publishes its box, exactly as a text op
       did before G17d. */
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
   publishes (`log`), so a count of resets is never a mystery again */
enum {
    TAGPU_GUI_WHY_NONE = 0,
    TAGPU_GUI_WHY_ARM,        /* the trigger (re)appeared: the twins start from the surfaces as they are */
    TAGPU_GUI_WHY_GLCTX,      /* the GL context changed (a display-mode switch)                          */
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
