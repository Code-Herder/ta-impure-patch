#ifndef TAGPU_BOARDWIRE_H
#define TAGPU_BOARDWIRE_H
/* The drawing board's operations and their bytes (research/notes/modules/board.md): what one
   player's gestures do to the marks, as the board applies them (tagpu_boardstore.h), as a
   RECMP_MARK contribution carries them to every peer, and as a recording keeps them.

   A BODY is a version byte (TAGPU_BW_VERSION), the seat of the player whose operations they are,
   then whole operations to its end, each its code byte and its fields, little-endian:

     POINT   id u32, x h z i32, len u8, text[len]    a new marker
     EDIT    id u32, len u8, text[len]                a marker's text
     MOVE    id u32, x h z i32                        a marker's place
     DELETE  id u32                                   a marker
     LINE    x0 h0 z0 x1 h1 z1 i32                    one segment of a line
     ERASE   x h z i32                                every mark within TAGPU_BW_ERASE_R of it

   A marker's id is its maker's seat in the top byte and a sequence of that seat's, never 0, below:
   a POINT names its own seat's id, the others any id. Every coordinate lies within
   +-TAGPU_BW_COORD (world px; the packet's bound, TAGPU_PK_PLACE_COORD), every text is at most
   TAGPU_BW_TEXT glyphs of the engine font's range 0x20..0x7E. Pure C: no engine, no Windows. */
#include <stdint.h>

#define TAGPU_BW_VERSION  1u
#define TAGPU_BW_TEXT     60u
#define TAGPU_BW_COORD    0x100000
#define TAGPU_BW_ERASE_R  48          /* world px */
#define TAGPU_BW_SEATS    10u
#define TAGPU_BW_HEAD     2u          /* the version and the seat */
#define TAGPU_BW_OP_MAX   (1u + 4u + 12u + 1u + TAGPU_BW_TEXT)   /* a POINT's bytes, the largest */

enum { TAGPU_BW_POINT = 1, TAGPU_BW_EDIT, TAGPU_BW_MOVE, TAGPU_BW_DELETE, TAGPU_BW_LINE,
       TAGPU_BW_ERASE };

typedef struct {
    unsigned op;               /* TAGPU_BW_*                                                  */
    uint32_t id;               /* POINT, EDIT, MOVE, DELETE                                   */
    int32_t  a[3];             /* POINT, MOVE, ERASE: the point; LINE: its first end           */
    int32_t  b[3];             /* LINE: its second end                                        */
    unsigned len;              /* POINT, EDIT                                                 */
    char     text[TAGPU_BW_TEXT + 1];   /* NUL-terminated at len                              */
} TagpuBoardOp;

/* the seat a marker id belongs to; 10 or more for none */
static __inline unsigned tagpu_bw_id_seat(uint32_t id) { return id >> 24; }

/* 1 when `op` is a well-formed operation of seat `seat` (its id's seat when it is a POINT) */
int tagpu_boardwire_ok(const TagpuBoardOp* op, unsigned seat);

/* One operation into out[0..cap): the bytes written, 0 when it does not fit or is not well formed
   for `seat`. */
unsigned tagpu_boardwire_put(unsigned char* out, unsigned cap, const TagpuBoardOp* op, unsigned seat);

/* A whole body checked: 1 with *seat set when its version is this one, its seat below 10 and every
   operation to its end well formed; 0 for anything else, a body with no operation included. */
int tagpu_boardwire_check(const unsigned char* body, unsigned bytes, unsigned* seat);

/* The operation at body[at] of a body tagpu_boardwire_check accepted, into *op: the offset of the
   next, or 0 at the end. Start at TAGPU_BW_HEAD. */
unsigned tagpu_boardwire_next(const unsigned char* body, unsigned bytes, unsigned at,
                              TagpuBoardOp* op);

/* A body checked by tagpu_boardwire_check, its seats renumbered: the body's seat and every marker
   id's top byte s become map[s]. A network game's peers each number the players their own way
   (each is its own record 0); the channel's seat map (tagpu_recmp_seat_map) takes the sender's
   numbering to this peer's, and a marker id keeps its owner and sequence through it. The bytes
   written to out[0..cap), the same count as the body's, or 0 when a seat maps to 10 or more or
   the body is not one. */
unsigned tagpu_boardwire_reseat(const unsigned char* body, unsigned bytes,
                                const unsigned char map[TAGPU_BW_SEATS], unsigned char* out,
                                unsigned cap);

#endif
