#ifndef TAGPU_RECFMT_H
#define TAGPU_RECFMT_H
/* tagpu_recfmt.h -- the demo recorder's records: what the capture puts into the ring, and what a
   recording's blocks hold (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S1).
   tagpu_recorder.c writes them on the game thread, tagpu_recwriter.c reads them on the writer
   thread, and tools/replay/tarec.py mirrors this file to read a recording back: a change here is
   a change there, and REC_FORMAT says which.

   AN IMAGE is a record as a recording keeps it: the record's own bytes where they are values, the
   target's ID where the exclusion file (replay-layout.tsv) names an id row, and zero where it names
   any other class. A declared table's image follows its layout the same way. Unit and player
   images are whole dwords (a player's last dword is padded with zero); a delta names the dwords
   that changed. Everything is little-endian.

   AN ID is one more than the target's row in its table, so NULL's is 0 and a record of zeros
   images to zeros: the rows a table gains are zero on both sides (RK_TABLE_SIZE), and a row of
   zeros needs no span. The IDs below are the rest that are not rows. */

#include <stdint.h>

#define REC_FORMAT        1

#define REC_UNIT_BYTES    0x118                          /* a unit record: 70 dwords          */
#define REC_UNIT_DWORDS   (REC_UNIT_BYTES / 4)
#define REC_PLAYER_BYTES  0x14B                          /* a player record                    */
#define REC_PLAYER_DWORDS ((REC_PLAYER_BYTES + 3) / 4)   /* 83; the image is 0x14C bytes        */
#define REC_PLAYERS       10    /* the records a game seats; the eleventh only ever means "no player" */
#define REC_TABLE_MAX     256   /* the most declared tables a recording names: the capture refuses
                                   to start past it, so the writer's copy never meets one more  */
#define REC_UNIT_BITS     ((REC_UNIT_DWORDS + 7) / 8)    /* 9 bytes of a unit delta's bitmap    */
#define REC_PLAYER_BITS   ((REC_PLAYER_DWORDS + 7) / 8)  /* 11 bytes of a player delta's        */

_Static_assert(REC_UNIT_BYTES % 4 == 0, "a unit image is whole dwords");

/* what an id row holds besides a row's ID */
#define REC_ID_NULL       0u
#define REC_ID_BAD        0xFFFFFFFEu   /* a pointer outside its table: counted and logged        */
#define REC_ID_OFFMAP     0xFFFFFFFDu   /* the sort's off-map bucket, *(main+0x142B7)              */

/* A record: this header, then `len` bytes of body; the next record starts at the next multiple of
   4. The ring and a block's payload are both runs of records. */
typedef struct RecHdr { uint16_t kind, len; } RecHdr;

enum RecKind {
    RK_PAD = 0,        /* ring only: nothing more before the ring's end                            */
    RK_TICK,           /* u32 GameTime: the records up to the next TICK are that iteration's        */
    RK_UNIT,           /* u16 slot, u16 0, the image: the slot holds a live unit with this image,
                          because it was created, its type changed, or a correction                  */
    RK_UNIT_DELTA,     /* u16 slot, u8 bits[REC_UNIT_BITS], u8 0, then one u32 for each bit set,
                          lowest first: the dwords of a live unit's image that changed               */
    RK_UNIT_GONE,      /* u16 slot, u16 0: the slot's unit died                                     */
    RK_PLAYER,         /* u16 player, u16 0, the image (REC_PLAYER_DWORDS * 4 bytes)                */
    RK_PLAYER_DELTA,   /* u16 player, u8 bits[REC_PLAYER_BITS], u8 0, u8 0, u8 0, then the changed
                          dwords, lowest first                                                       */
    RK_TABLE_SIZE,     /* u16 table, u16 0, u32 bytes: the declared table's image is now this long;
                          bytes it gains are zero until a span says otherwise                      */
    RK_TABLE_SPAN,     /* u16 table, u16 0, u32 offset, then bytes of its image from there          */
    RK_CORRECT,        /* u32 0: the next record is a correction -- the writer's copy differed from
                          the live state it was checked against                                     */
    RK_VERIFY_UNIT,    /* ring only: u16 slot, u16 live, the image when live                        */
    RK_VERIFY_PLAYER,  /* ring only: u16 player, u16 0, the image                                   */
    RK_VERIFY_TABLE,   /* ring only: u16 table, u16 0, u32 offset, bytes of its image               */
    RK_HEADER,         /* ring only: the header block's entries (RecTag below)                      */
    RK_END,            /* ring only: u16 RecEnd, u16 0, u32 bad IDs, u32 the ring's high water in
                          bytes. The capture's last record: after it, the ring is the writer's      */
    RK_KINDS
};

/* why a recording ended */
enum RecEnd {
    REC_END_TEARDOWN = 1,   /* the level ended (0x491B60): the recording is complete             */
    REC_END_RING,           /* an iteration's records did not fit in the ring                    */
    REC_END_WRITE,          /* the writer could not write the file                              */
    REC_END_TABLE,          /* a declared table grew and its shadow could not                   */
    REC_END_LEVEL,          /* the unit array or the clock changed without a teardown            */
    REC_END_MODE            /* the game stopped being a skirmish                                 */
};

/* A recording is REC_MAGIC, then blocks. A block is a RecBlock, then `packed` bytes: the zlib
   stream (RFC 1950) of its `raw` bytes, or the raw bytes themselves when RBF_STORED is set.
   `crc_body` is the CRC-32 (zlib's crc32, crc32.c's Crc32_ComputeBuf) of those `packed` bytes and `crc_head` that of the block's
   first 28 bytes, so a block cut short, or a header torn, is known by one of them. A block is
   written whole or not at all: a file ends at its last complete block. */
#define REC_MAGIC        "IMPREC\r\n"          /* 8 bytes, no NUL                                 */
#define REC_BLOCK_MAGIC  0x4B4C4249u           /* "IBLK"                                          */

typedef struct RecBlock {
    uint32_t magic;
    uint16_t kind;          /* RB_*                                                           */
    uint16_t flags;         /* RBF_*                                                          */
    uint32_t first, last;   /* the first and last GameTime its records cover                  */
    uint32_t raw, packed;
    uint32_t crc_body;
    uint32_t crc_head;
} RecBlock;
_Static_assert(sizeof(RecBlock) == 32, "a block header is 32 bytes");

enum RecBlockKind {
    RB_HEADER = 1,     /* entries (RecTag): which game this is                                  */
    RB_STREAM,         /* records: iterations, in order, from where the last block ended         */
    RB_SNAPSHOT,       /* records that build the whole world at `last` from nothing: a TICK, an
                          RK_UNIT per live unit, an RK_PLAYER per player, and an RK_TABLE_SIZE
                          and its spans per declared table. A dump is one of these, stored      */
    RB_END             /* entries (RecTag): how the recording ended                              */
};
#define RBF_STORED 1u

/* A header or end block's payload: entries of u16 tag, u16 length, then that many bytes, the next
   entry at the next multiple of 4. A string is its bytes, with no NUL. */
enum RecTag {
    RT_FORMAT = 1,     /* u32 REC_FORMAT                                                        */
    RT_BUILD,          /* string: the commit Impure was built from                              */
    RT_MODE,           /* u32: the game mode, 2 for a skirmish                                  */
    RT_MAP,            /* string: the map file's stem                                           */
    RT_START,          /* u32: the first recorded GameTime                                      */
    RT_WALLCLOCK,      /* u32 low, u32 high: the start as a FILETIME, UTC                       */
    RT_SLOTS,          /* u32: the unit array's slots, the engine's own count                   */
    RT_UNITS_EACH,     /* u32: the units a player may have, main+0x37EE6                         */
    RT_UNITDEFS,       /* u32 count, u32 FNV-1a of every UnitName (def+0x20), in index order     */
    RT_PLAYER,         /* u8 index, u8 controller (+0x73), u8 side, u8 logo (PlayerInfo +0x95,
                          +0x96), then the name at +0x2B                                          */
    RT_LEVER,          /* u8 present, u8 text read, u16 name bytes, the name, then its text       */
    RT_LAYOUT,         /* string: replay-layout.tsv's SHA-256, TAGPU_REPLAY_LAYOUT_SHA256        */
    RT_TABLE,          /* u16 index, u8 class, u8 key, u32 row bytes, u16 flags, u16 0, string
                          name: a declared table the records name by index                       */
    RT_END_REASON = 32,/* u32 RecEnd                                                            */
    RT_LAST,           /* u32: the last recorded GameTime                                       */
    RT_ITERATIONS,     /* u32: iterations recorded                                              */
    RT_CORRECTIONS,    /* u8 record kind (RK_UNIT, RK_PLAYER or RK_TABLE_SPAN), u8 0, u16 the
                          table's index (0 for the others), u32 the dword's offset in the image
                          (REC_UNIT_BYTES for a unit's liveness), u32 count: one entry for each
                          dword that differed. Past 256 of them, one entry of kind 0 and offset
                          0xFFFFFFFF counts the rest                                              */
    RT_BAD_IDS,        /* u32: id rows found outside their tables, counted each time an image
                          is taken                                                               */
    RT_RING_HIGH       /* u32: the most bytes the ring held                                     */
};

#endif
