#ifndef TAGPU_RECREADER_H
#define TAGPU_RECREADER_H
/* tagpu_recreader -- reads a demo recording back (tagpu_recfmt.h): its blocks, each checked and
   inflated, the header's entries, and the world the stream builds, one iteration at a time
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S2b and S3a). tagpu_replay.c applies what
   this reads; this file reads no engine memory, and since it is not on thread-split.allow the
   build refuses an engine address in it. tools/replay/tarec.py reads the same records and
   accepts the same shapes: a change to one is a change to the other.

   Every value read here is data. A record whose length, slot, index or offset falls outside what
   its kind allows makes the recording unreadable from there, as the writer refuses the same
   records (tagpu_recwriter.c, apply). What this accepts is only well-formed: the values inside an
   image are bounded again by whoever uses one as an index or a pointer. */

#include <stdint.h>
#include "tagpu_recfmt.h"

#define RECR_TEXT       64          /* a header string's bytes kept, NUL included            */
#define RECR_NAME       (0x1E + 1)  /* a seat's name: the record's 0x1E bytes, NUL included  */
#define RECR_BLOCK_MAX  (64u << 20) /* the most bytes a block may pack or inflate to         */

/* a seat at the start: an RT_PLAYER entry */
typedef struct RecSeat {
    unsigned char index, controller, side, logo;
    unsigned char ally[11];                 /* the alliance row, record +0x108          */
    unsigned char has_info;                 /* 0 when the record had no PlayerInfo       */
    unsigned char info[REC_INFO_BYTES];
    char          name[RECR_NAME];          /* record +0x2B                              */
} RecSeat;

/* a declared table a recording names by index: an RT_TABLE entry */
typedef struct RecTableInfo {
    unsigned short index, flags;
    unsigned char  cls, key;
    unsigned       stride;
    char           name[128];
} RecTableInfo;

typedef struct RecHeader {
    unsigned     format, mode, start, slots, units_each;
    unsigned     has_unitdefs, ndefs, defs_hash;
    unsigned     has_weapons, nweapons, weapons_hash;
    unsigned     grid_w, grid_h, wrecks, nfdefs, fdefs_hash;     /* RT_FEATURES             */
    unsigned     has_options, lostype, cmdrdeath, difficulty, fixed_starts;
    unsigned     nplayers, controlled, viewed;
    char         build[RECR_TEXT], map[RECR_TEXT], settings_map[RECR_TEXT], layout[RECR_TEXT + 8];
    unsigned     has_settings_map;
    unsigned     nseats;
    RecSeat      seats[REC_PLAYERS];            /* in the header's order               */
    unsigned     ntab;
    RecTableInfo tab[REC_TABLE_MAX];
} RecHeader;

/* one node list of a live unit, its node images in list order */
typedef struct RecList { unsigned n, cap; unsigned char* img; } RecList;

/* a declared table's image */
typedef struct RecTable { unsigned index, stride, size, cap; unsigned char* img; } RecTable;

/* an event, or a feature image, of the iteration read last, pointing into the reader's block:
   valid until the next recr_tick */
typedef struct RecEvent { unsigned kind, len; const unsigned char* b; } RecEvent;

/* What changed a slot in the iteration read last (RecWorld.changed). */
#define RECW_IMAGE  0x01    /* its image: an RK_UNIT or an RK_UNIT_DELTA                     */
#define RECW_MAIN   0x02    /* its main list (+0x5C) was sent whole                           */
#define RECW_SUB    0x04    /* its sub list (+0x60)                                           */
#define RECW_BORN   0x08    /* an RK_UNIT: the slot holds a new unit, or a correction's image  */
#define RECW_GONE   0x10    /* an RK_UNIT_GONE: its unit died, and its lists with it           */

/* The recorded world at the end of one iteration's images: every live slot's image and lists,
   the ten players, the controllers and the viewer, the declared tables, and the features. */
typedef struct RecWorld {
    unsigned       slots;       /* slots 1 .. slots - 1 hold units, as the engine's array     */
    unsigned       tick;        /* the GameTime of the images it holds                        */
    unsigned char* live;        /* [slots]                                                    */
    unsigned char* changed;     /* [slots]: RECW_* by the iteration read last                 */
    uint32_t*      unit;        /* [slots * REC_UNIT_DWORDS]                                  */
    RecList*       list;        /* [slots * 2]: the main list, then the sub list              */
    uint32_t       player[REC_PLAYERS][REC_PLAYER_DWORDS];
    unsigned char  pchanged[REC_PLAYERS];   /* a player's image changed in the iteration     */
    unsigned char  ctl[REC_PLAYERS];
    unsigned char  view[2];     /* main+0x2A42 and main+0x2A43 on the recording machine       */
    unsigned       ntab;
    RecTable*      tab;         /* [ntab], the header's tables in its order                    */
    unsigned       nev;         /* the events of the step after the images, in engine order: a
                                   feature root's bracket, RK_EV_FEATURE to its END, holds the
                                   draws it made, the tick's cells and what it nested         */
    RecEvent*      ev;
    unsigned char* grid;        /* [grid_w * grid_h] cell images, row-major                   */
    unsigned char* pool;        /* [wrecks] record images                                     */
    int32_t        heads[4];    /* the pool's three heads and the reproduction cursor         */
    unsigned       nfeat;       /* the iteration's RK_CELLS, RK_WRECK and RK_FEATURES, in the
                                   order they came: what changed in the grid and the pool     */
    RecEvent*      feat;
    unsigned       corrections; /* RK_CORRECTs read: the recording machine's own repairs      */
} RecWorld;

typedef struct RecReader RecReader;

/* Opens a recording and reads its header block. NULL, with the reason in `why`, when the file
   cannot be read or is not a recording of this format. */
RecReader* recr_open(const char* path, char* why, unsigned cap);
const RecHeader* recr_header(const RecReader* r);
RecWorld* recr_world(RecReader* r);

/* Reads the next iteration into the world: its images, and its events into `ev`.
   1 read; 0 the recording has no more iterations (`why` says how it ended); -1 unreadable from
   here (`why` says where), and the world is as the last iteration left it. */
int recr_tick(RecReader* r, char* why, unsigned cap);

void recr_close(RecReader* r);

#endif
