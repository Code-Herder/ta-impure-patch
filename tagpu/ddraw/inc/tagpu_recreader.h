#ifndef TAGPU_RECREADER_H
#define TAGPU_RECREADER_H
/* tagpu_recreader -- reads a demo recording back (tagpu_recfmt.h): its blocks, each checked and
   inflated, the header's entries, and the world the stream builds, one iteration at a time or
   from a snapshot at a seek (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S2b, S3a
   and S3b). tagpu_replay.c applies what this reads; this file reads no engine memory, and since
   it is not on thread-split.allow the build refuses an engine address in it.
   tools/replay/tarec.py reads the same records and accepts the same shapes: a change to one is a
   change to the other.

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
    unsigned       lhash;       /* the layout hash (tagpu_simreg_layout_hash)          */
    char           name[128];
} RecTableInfo;

typedef struct RecHeader {
    unsigned     format, mode, start, slots, units_each;
    unsigned     has_unitdefs, ndefs, defs_hash;
    unsigned     has_weapons, nweapons, weapons_hash;
    unsigned     grid_w, grid_h, wrecks, nfdefs, fdefs_hash;     /* RT_FEATURES             */
    unsigned     has_options, lostype, cmdrdeath, difficulty, fixed_starts;
    unsigned     nplayers, controlled, viewed;
    unsigned     proj_cap;                  /* RT_PROJECTILES: the projectile pool's records  */
    char         build[RECR_TEXT], map[RECR_TEXT], settings_map[RECR_TEXT], layout[RECR_TEXT + 8];
    char         globals[RECR_TEXT + 8];    /* RT_GLOBALS: replay-globals.tsv's SHA-256       */
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
   the ten players, the controllers and the viewer, the declared tables, the features, the
   globals, the projectile pool and the explored map. */
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
    uint32_t       globals[REC_GLOBALS_DWORDS];   /* the globals image (tagpu_replay_globals.h) */
    unsigned char* proj;        /* [proj_cap] projectile record images, REC_PROJ_IMAGE each:
                                   [proj_hw, proj_cap) are zero, as on the recording machine   */
    unsigned       nproj;       /* the pool's count, main+0x141F3                             */
    unsigned       proj_hw;     /* its written extent                                         */
    unsigned char* mapped;      /* [mapped_bytes] the explored map *(main+0x14273)            */
    unsigned       mapped_bytes;   /* REC_MAPPED_BYTES of the header's grid                   */
    int            mapped_whole;   /* its RK_MAPPED records have all come                     */
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
   here (`why` says where), and the world is as the last iteration left it. A snapshot block met
   on the way is skipped unread: only a seek reads one. */
int recr_tick(RecReader* r, char* why, unsigned cap);

/* Puts the world at iteration T, as a recr_tick that read T leaves it: T's images, and the events
   of the step after them in `ev`; recr_tick then goes on from T + 1. It builds from the last
   snapshot before T over the base grid (the first iteration's, which the first recr_tick takes;
   a seek before any takes it itself), or from the recording's first iteration when no snapshot
   precedes T, then reads the stream up to T.
   1 the world holds T; 0 T is outside the recording -- before its first iteration or after the
   last complete block's -- checked before anything changes, so the reader is as it was (`why`
   names the recording's first and last GameTime); -1 unreadable (`why` says where), and the
   world is undefined until a seek that returns 1.
   With `visit`, the snapshot it builds from is the last one more than `lead` iterations before T
   (S + lead < T), and `visit(ctx, world)` is called once each iteration of the stream after it
   has been read, T's included, so a caller can follow what the iterations before T did. */
typedef void (*RecVisit)(void* ctx, const RecWorld* w);
int recr_seek(RecReader* r, unsigned T, unsigned lead, RecVisit visit, void* ctx, char* why,
              unsigned cap);

void recr_close(RecReader* r);

/* What the Replays browser shows of a recording, read from its header, its minimap block and its
   end block alone (the stream's blocks are skipped unread). */
typedef struct RecInfoSeat {
    unsigned char present;      /* the header seats this record (RT_PLAYER)                   */
    unsigned char result;       /* RecResult; REC_RESULT_UNKNOWN without an end block           */
    unsigned      commander;    /* the GameTime its first commander died, 0 none                */
    unsigned      defeat;       /* the GameTime its last unit died, 0 none                      */
} RecInfoSeat;
typedef struct RecInfo {
    char          version[RECR_TEXT];       /* RT_GAME_VERSION, "" when absent                   */
    unsigned      modprint;                 /* RT_MODPRINT: compare with tagpu_rec_mod_print()  */
    int           has_modprint;
    unsigned      narchives;                /* RT_ARCHIVE entries (at most REC_ARCHIVES)          */
    struct { char name[RECR_TEXT]; unsigned size; unsigned long long mtime; } archive[REC_ARCHIVES];
    int           ended;                    /* an end block was read                             */
    unsigned      game_end;                 /* RT_GAME_END                                       */
    RecInfoSeat   seat[REC_PLAYERS];
    /* the minimap: w x h palette indices, rows of w, of which [0, 2*cols) x [0, 2*rows) is
       shown; w = 0 when the recording has none. The indices live in this struct. */
    unsigned      mm_w, mm_h, mm_cols, mm_rows;
    unsigned char minimap[REC_MINIMAP_MAX];
} RecInfo;
/* 1 read (a recording without an end block, cut short, still reads: `ended` 0); 0 the file
   cannot be read or is not a recording (why). Any thread; it opens the file read-only, shared. */
int recr_info(const char* path, RecInfo* out, char* why, unsigned cap);

#endif
