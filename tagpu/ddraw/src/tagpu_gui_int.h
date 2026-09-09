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
    PK_STRING       /* G17d: TA's own glyphs, stamped by us — the string        */
                    /* follows in the arena and the font/colours ride along     */
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
    const void*    frame;           /* sprite: the key (header, pixel ptr);
                                       string: the FONT object                  */
    const void*    pix;
    unsigned       aoff, alen;      /* arena bytes: seed / pixels / a sprite's first sight / the string */
    unsigned       flip;            /* the flip this belongs to (diagnostics)   */
} TAGPU_PUBOP;

#define TAGPU_GUI_QCAP   (1u << 16)          /* ops                            */
#define TAGPU_GUI_ASIZE  (16u << 20)         /* arena bytes                    */

typedef struct TAGPU_GUIQ {
    TAGPU_PUBOP*   ops;                      /* TAGPU_GUI_QCAP                 */
    unsigned char* arena;                    /* TAGPU_GUI_ASIZE                */
    volatile unsigned qHead, qTail;          /* producer writes head, consumer tail */
    volatile unsigned aHead, aTail;          /* arena bytes, same roles        */
    volatile unsigned reseed;                /* consumer asks the producer to seed everything */
    volatile unsigned why;                   /* the last reason `reseed` (or an overflow) was raised: TAGPU_GUI_WHY_* */
    volatile unsigned overflows;             /* the producer ran out of queue or arena  */
    volatile unsigned resets;                /* fresh starts published (re-arm, GL, overflow, a lost frame) */
    volatile unsigned stalls;                /* episodes where the consumer took nothing for TAGPU_GUI_STALL_MS
                                                while work was queued (a display-mode switch kills the render
                                                thread): the producer drops its batches until it moves again */
} TAGPU_GUIQ;

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
