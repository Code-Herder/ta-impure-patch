#ifndef TAGPU_RECPOV_H
#define TAGPU_RECPOV_H
/* tagpu_recpov.h -- a human seat's perspective as a recording carries it: its camera and cursor
   (RecPov), its clicks (RecClick) and its selection (RecSel), the payloads a skirmish file's
   RK_POV, RK_POV_AT, RK_CLICK and RK_SELECT records hold (tagpu_recfmt.h) and the multiplayer
   contributions RECMP_POV, RECMP_CLICK and RECMP_SELECT will hold
   (research/notes/tadr-port/demo-recorder-perspective-streams.md, *The records*).
   tagpu_recorder.c fills them on the game thread; tagpu_recwriter.c, tagpu_recreader.c and
   tools/replay/tarec.py accept exactly what the helpers below accept, so a change here is a
   change in all of them. Every field is data: a coordinate is never an index, and the readers
   bound a selection's slots against the seat's own block before anything uses one. Packed,
   little-endian. */

#include <stdint.h>
#include <string.h>
#include "tagpu_limits.h"

#define REC_POV_EVERY    3        /* skirmish: ticks between samples, and the playback's delay  */
#define RECMP_POV_EVERY  6        /* multiplayer (owner Q3: 5 Hz)                                */
#define RECMP_POV_ROOM   64       /* free transmit slots a multiplayer camera/cursor sample needs */
#define REC_CLICK_SHOW   15       /* ticks a click ring lasts                                    */
#define REC_SEL_MAX      1500     /* entries a selection may hold: the most units one seat's
                                     block can have in every build, so a seat's whole block fits */
#define REC_POV_EVERY_MAX 30
#define REC_POV_ZOOM_MIN 16384u   /* the applied level x 65536: TAGPU_ZOOM_MIN 0.25 ...         */
#define REC_POV_ZOOM_MAX 524288u  /* ... to TAGPU_ZOOM_MAX 8                                     */
#define REC_POV_VIEW_MAX 16384u

_Static_assert(REC_SEL_MAX >= TAGPU_LIM_UNITS, "a selection holds a whole block");

enum { REC_POV_WORLD = 1, REC_POV_MINIMAP = 2 };    /* main+0x2CC6 bit 1 / bit 0               */
enum { REC_SEL_BEGIN = 1, REC_SEL_END = 2 };
enum { REC_CLICK_LEFT = 0, REC_CLICK_RIGHT = 1 };

#pragma pack(push, 1)
typedef struct RecPov {
    uint8_t  player;              /* the seat's player record; 0xFF in a contribution, whose
                                     origin names it                                          */
    uint8_t  every;               /* the sampler's spacing: the playback's delay              */
    uint8_t  flags;               /* REC_POV_*                                                */
    uint8_t  zero;
    uint16_t view_w, view_h;      /* main+0x37E37 / +0x37E3B, the 1x view                     */
    uint32_t zoom;                /* Impure's applied level x 65536                           */
    int32_t  cam_x, cam_y;        /* the scroll target main+0x14327/+0x1432B plus half the view:
                                     the world point at the view's centre, at any zoom        */
    int32_t  cur_x, cur_h, cur_z; /* main+0x2CAC/+0x2CB0/+0x2CB4, the integer halves of the
                                     world point under the pointer, sign-extended             */
} RecPov;
typedef struct RecClick {
    uint8_t  player;              /* as RecPov's                                              */
    uint8_t  button;              /* REC_CLICK_LEFT the left action 0x498F70, REC_CLICK_RIGHT
                                     the right press 0x499100                                 */
    uint8_t  order;               /* main+0x2CC3, the armed order                             */
    uint8_t  cursor;              /* main+0x2CBE, the cursor index this message produced      */
    uint8_t  region;              /* main+0x2CC6 & 3                                          */
    uint8_t  itype;               /* main+0x37EFA, the Interface Type                         */
    uint16_t keys;                /* the message's wParam & (MK_SHIFT | MK_CONTROL)           */
    int32_t  x, h, z;             /* as RecPov's cursor                                       */
} RecClick;
typedef struct RecSel {           /* then `count` RecSelEntry                                 */
    uint8_t  player, flags;       /* a skirmish record is BEGIN|END, first 0, count = total   */
    uint16_t total, first, count;
} RecSel;
typedef struct RecSelEntry {
    uint16_t slot;                /* the unit's slot in the unit array                        */
    uint32_t birth;               /* its incarnation (tagpu_patches.c's s_hitStamp column)     */
} RecSelEntry;
#pragma pack(pop)

_Static_assert(sizeof(RecPov) == 32, "RecPov is 32 bytes");
_Static_assert(sizeof(RecClick) == 20, "RecClick is 20 bytes");
_Static_assert(sizeof(RecSel) == 8, "RecSel is 8 bytes");
_Static_assert(sizeof(RecSelEntry) == 6, "RecSelEntry is 6 bytes");
_Static_assert(sizeof(RecSel) + REC_SEL_MAX * sizeof(RecSelEntry) <= 0xFFFF,
               "a whole selection fits a record's u16 length");

#define REC_CLICK_KEYS     0x000Cu  /* MK_SHIFT | MK_CONTROL                                    */
#define REC_SEL_BYTES(n) (sizeof(RecSel) + (size_t)(n) * sizeof(RecSelEntry))

/* A sample's own fields; its player is the caller's to bound. 1 well-formed. */
static __inline int rec_pov_ok(const RecPov* p)
{
    return p->every >= 1 && p->every <= REC_POV_EVERY_MAX &&
           !(p->flags & ~(unsigned)(REC_POV_WORLD | REC_POV_MINIMAP)) && !p->zero &&
           p->zoom >= REC_POV_ZOOM_MIN && p->zoom <= REC_POV_ZOOM_MAX &&
           p->view_w >= 1 && p->view_w <= REC_POV_VIEW_MAX &&
           p->view_h >= 1 && p->view_h <= REC_POV_VIEW_MAX;
}

/* A click's own fields; its player is the caller's to bound. 1 well-formed. */
static __inline int rec_click_ok(const RecClick* c)
{
    return c->button <= REC_CLICK_RIGHT && c->region <= 3 && c->itype <= 1 &&
           !(c->keys & ~REC_CLICK_KEYS);
}

/* A selection record of `len` bytes: its head into `out`, and 1 when its length is exactly its
   entries', `first + count <= total` and `total <= limit` (the seat's units_each, never more than
   REC_SEL_MAX). A skirmish record must also be whole: rec_sel_whole. */
static __inline int rec_sel_head(const unsigned char* b, unsigned len, unsigned limit, RecSel* out)
{
    if (len < sizeof(RecSel)) return 0;
    memcpy(out, b, sizeof *out);
    if (limit > REC_SEL_MAX) limit = REC_SEL_MAX;
    return len == REC_SEL_BYTES(out->count) && !(out->flags & ~(unsigned)(REC_SEL_BEGIN | REC_SEL_END)) &&
           out->total <= limit && (unsigned)out->first + out->count <= out->total;
}

static __inline int rec_sel_whole(const RecSel* s)
{
    return s->flags == (REC_SEL_BEGIN | REC_SEL_END) && !s->first && s->count == s->total;
}

/* The seat's block, inclusive slots [*lo, *hi], from its player image as a recording carries it:
   the record's +0x67 and +0x6B are the block's first and last unit (E9), id rows of the exclusion
   file (replay-layout.tsv, blockfirst and blocklast), so the image holds each one's ID, its slot
   plus one. 0 when either is no slot of a `slots`-slot array or they are out of order. */
static __inline int rec_sel_block(const unsigned char* player_image, unsigned slots, unsigned* lo,
                                  unsigned* hi)
{
    uint32_t first, last;
    memcpy(&first, player_image + 0x67, 4);
    memcpy(&last, player_image + 0x6B, 4);
    if (first < 2 || last < first || last > slots) return 0;
    *lo = first - 1;
    *hi = last - 1;
    return 1;
}

/* A record's entries: slots strictly ascending after `*prev` (start it at -1) and inside the
   seat's block [lo, hi], which the caller takes from the seat's own player image. 1 well-formed. */
static __inline int rec_sel_entries_ok(const unsigned char* e, unsigned count, unsigned lo,
                                       unsigned hi, long* prev)
{
    unsigned i;
    for (i = 0; i < count; i++) {
        RecSelEntry s;
        memcpy(&s, e + i * sizeof s, sizeof s);
        if ((long)s.slot <= *prev || s.slot < lo || s.slot > hi) return 0;
        *prev = s.slot;
    }
    return 1;
}

#endif
