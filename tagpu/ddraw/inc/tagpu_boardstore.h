#ifndef TAGPU_BOARDSTORE_H
#define TAGPU_BOARDSTORE_H
/* The drawing board's marks as a state machine (research/notes/modules/board.md): operations
   (tagpu_boardwire.h) applied in order, each from one seat at one game tick. The live board
   applies the player's own and the channel's; a replay applies a recording's. The same operations
   in the same order at the same ticks with the same masks give the same marks. Pure C: no engine,
   no Windows. */
#include <stdint.h>
#include "tagpu_boardwire.h"

#define TAGPU_BS_MARKS 256u
#define TAGPU_BS_SEGS  4096u

typedef struct {
    uint32_t id;               /* 0: a free slot                                        */
    uint8_t  owner, len;
    int32_t  x, h, z;
    uint32_t born;             /* the game tick it was made at                          */
    char     text[TAGPU_BW_TEXT + 1];
} TagpuBoardMark;

typedef struct {
    uint8_t  owner, live;
    int32_t  a[3], b[3];
    uint32_t born;
} TagpuBoardSeg;

typedef struct {
    TagpuBoardMark mark[TAGPU_BS_MARKS];
    TagpuBoardSeg  seg[TAGPU_BS_SEGS];
    unsigned       head, nseg;         /* the ring: the oldest at head                         */
    uint32_t       life;               /* ticks a mark lives; 0 until it is erased             */
} TagpuBoardStore;

void tagpu_boardstore_clear(TagpuBoardStore* s, uint32_t life);

/* What an applied operation did */
enum { TAGPU_BS_NONE = 0, TAGPU_BS_CHANGED, TAGPU_BS_NEW };

/* One operation of `seat`, checked by tagpu_boardwire_ok, at game tick `now`. `may` is a bit per
   seat whose marks it may change besides its own: an EDIT, MOVE, DELETE or ERASE leaves every other
   owner's alone. A POINT whose id exists already changes nothing; one with every slot taken
   replaces the oldest marker. TAGPU_BS_NEW for a new marker. */
int tagpu_boardstore_apply(TagpuBoardStore* s, unsigned seat, const TagpuBoardOp* op, uint32_t now,
                           unsigned may);

/* Everything older than the lifetime at tick `now` removed; a mark born after `now` (a tick that
   went back) is kept. */
void tagpu_boardstore_expire(TagpuBoardStore* s, uint32_t now);

TagpuBoardMark* tagpu_boardstore_find(TagpuBoardStore* s, uint32_t id);

#endif
