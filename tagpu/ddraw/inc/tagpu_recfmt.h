#ifndef TAGPU_RECFMT_H
#define TAGPU_RECFMT_H
/* tagpu_recfmt.h -- the demo recorder's records: what the capture puts into the ring, and what a
   recording's blocks hold (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S1 and S2a).
   tagpu_recorder.c writes them on the game thread, tagpu_recwriter.c reads them on the writer
   thread, and tools/replay/tarec.py mirrors this file to read a recording back: a change here is
   a change there, and REC_FORMAT says which.

   AN IMAGE is a record as a recording keeps it: the record's own bytes where they are values, the
   target's ID where the exclusion file (replay-layout.tsv) names an id row, and zero where it names
   any other class but puppet0 and puppet1, which are values. A unit's image is its record, then
   its mover's (the mover record), and a player's is its record, then its PlayerInfo's (the info
   record), each tail at a dword boundary: the object row that points at a tail holds 1 in the
   image when the pointer is set, and the tail is zero when it is not. An order node's image is
   the node's (the order record). A declared table's image follows its layout the same way. Images
   are whole dwords, padded with zero; a delta names the dwords that changed. Everything is
   little-endian.

   AN ID is one more than the target's row in its table, so NULL's is 0 and a record of zeros
   images to zeros: the rows a table gains are zero on both sides (RK_TABLE_SIZE), and a row of
   zeros needs no span. The IDs below are the rest that are not rows, and an order node's owned
   object is recorded as its class code (RL_T_OWNED). An ID is the recording's name for a row,
   never the engine's own index, which the records also carry as plain values: a unit image holds
   its type's ID at +0x92 and the engine's type index, one less, at +0xA6, and a SIMF_ID or
   SIMF_TARGET field of a declared table holds the engine's index or slot as it is
   (tagpu_simreg.h). An event names a unit by its ID, the slot's ID. */

#include <stdint.h>

#define REC_FORMAT        2

#define REC_UNIT_BYTES    0x118   /* a unit record                                           */
#define REC_MOVER_BYTES   0x2F    /* its mover: the creates' allocation (`push 0x2F`, 0x4860C4)  */
#define REC_MOVER_AT      REC_UNIT_BYTES                 /* the mover's bytes in a unit image   */
#define REC_UNIT_IMAGE    0x148                          /* 82 dwords                           */
#define REC_UNIT_DWORDS   (REC_UNIT_IMAGE / 4)
#define REC_PLAYER_BYTES  0x14B   /* a player record                                         */
#define REC_INFO_BYTES    0xB9    /* its PlayerInfo: the constructor's allocation (0x463BE0)     */
#define REC_INFO_AT       0x14C                          /* PlayerInfo's bytes in a player image */
#define REC_PLAYER_IMAGE  0x208                          /* 130 dwords                          */
#define REC_PLAYER_DWORDS (REC_PLAYER_IMAGE / 4)
#define REC_ORDER_BYTES   0x56    /* an order node: `push 0x56` at every allocation (0x43A0C0)   */
#define REC_ORDER_IMAGE   0x58
#define REC_ORDERS_MAX    4096    /* the most nodes a recorded list holds: a longer one ends the
                                     recording (REC_END_ORDERS), and is never cut short        */
#define REC_ORDERS_CHUNK  512     /* the most node images one RK_ORDERS carries                */
#define REC_PLAYERS       10    /* the records a game seats; the eleventh only ever means "no player" */
#define REC_TABLE_MAX     256   /* the most declared tables a recording names: the capture refuses
                                   to start past it, so the writer's copy never meets one more  */
#define REC_UNIT_BITS     ((REC_UNIT_DWORDS + 7) / 8)    /* 11 bytes of a unit delta's bitmap   */
#define REC_PLAYER_BITS   ((REC_PLAYER_DWORDS + 7) / 8)  /* 17 bytes of a player delta's        */
#define REC_UNIT_DELTA_HEAD   ((2 + REC_UNIT_BITS + 3) & ~3)     /* 16: the slot and the bitmap */
#define REC_PLAYER_DELTA_HEAD ((2 + REC_PLAYER_BITS + 3) & ~3)   /* 20                          */
#define REC_ORDERS_LENGTH 0xFFFFFFFEu   /* a correction's offset for a list's length          */
#define REC_NAME_MAX      32    /* the bytes of a script's name an event carries               */
#define REC_TEXT_MAX      128   /* the bytes of a reply's text an event carries                */

_Static_assert(REC_UNIT_IMAGE == ((REC_MOVER_AT + REC_MOVER_BYTES + 3) & ~3), "the unit image");
_Static_assert(REC_PLAYER_IMAGE == REC_INFO_AT + ((REC_INFO_BYTES + 3) & ~3), "the player image");
_Static_assert(REC_INFO_AT == ((REC_PLAYER_BYTES + 3) & ~3), "PlayerInfo at a dword");
_Static_assert(REC_ORDER_IMAGE == ((REC_ORDER_BYTES + 3) & ~3), "the order image");
_Static_assert(8 + REC_ORDERS_CHUNK * REC_ORDER_IMAGE <= 0xFFFF, "an RK_ORDERS fits its length");

/* what an id row holds besides a row's ID */
#define REC_ID_NULL       0u
#define REC_ID_BAD        0xFFFFFFFEu   /* a pointer outside its table: counted and logged        */
#define REC_ID_OFFMAP     0xFFFFFFFDu   /* the sort's off-map bucket, *(main+0x142B7)              */

/* A record: this header, then `len` bytes of body; the next record starts at the next multiple of
   4. The ring and a block's payload are both runs of records. */
typedef struct RecHdr { uint16_t kind, len; } RecHdr;

enum RecKind {
    RK_PAD = 0,        /* ring only: nothing more before the ring's end                            */
    RK_TICK,           /* u32 GameTime: the records up to the next TICK are that iteration's: its
                          images, then the events of the step that follows them                  */
    RK_UNIT,           /* u16 slot, u16 0, the image: the slot holds a live unit with this image,
                          because it was created, its type changed, or a correction                  */
    RK_UNIT_DELTA,     /* u16 slot, u8 bits[REC_UNIT_BITS], u8 0 x3, then one u32 for each bit
                          set, lowest first: the dwords of a live unit's image that changed        */
    RK_UNIT_GONE,      /* u16 slot, u16 0: the slot's unit died, and its order lists with it       */
    RK_PLAYER,         /* u16 player, u16 0, the image (REC_PLAYER_IMAGE bytes)                    */
    RK_PLAYER_DELTA,   /* u16 player, u8 bits[REC_PLAYER_BITS], u8 0, then the changed dwords,
                          lowest first                                                             */
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
                          bytes, u32 events refused off the game thread, u32 sprays no query
                          preceded. The capture's last record: after it, the ring is the writer's */
    RK_ORDERS,         /* u16 slot, u8 list (0 the main list +0x5C, 1 the sub list +0x60), u8 0,
                          u16 the list's nodes, u16 the first node here, then the images of nodes
                          [first, first + (len - 8) / REC_ORDER_IMAGE): a live unit's list, whole,
                          when its image changed. A list of more than REC_ORDERS_CHUNK nodes takes
                          several, `first` ascending; an empty one takes one with no images       */
    RK_VERIFY_ORDERS,  /* ring only: the same, of a list read afresh for the rotating check        */
    RK_CONTROLLER,     /* u8 player, u8 its +0x73, u16 0: the controller, when it differs from the
                          last iteration's or the header's (the console's AI toggle 0x416280)     */
    RK_VIEWER,         /* u8 main+0x2A42, u8 main+0x2A43, u16 0: the controlled and the viewed
                          player, when they differ from the last iteration's or the header's     */

    /* THE EVENTS: what the engine did in the step after the images before them, in the order it
       did it. Each is taken where the engine does it (tagpu_recorder.c, *The events*). */
    RK_EV_CREATE,      /* u32 unit ID, u16 type (the create's argument: the engine's index), u8
                          owner (its player), u8 layer (& 3), u32 finished, i32 x, y, z: the create
                          0x485F50 made a unit. Taken at its exit, so the events of the calls it
                          makes -- its state byte, its scripts -- come before it                  */
    RK_EV_DEATH,       /* u32 unit ID, u32 killer's ID, u8 killer's player (10: none), i8 severity,
                          u8 the record's +0xA, u8 carried (C4's decision), then the unit's image as
                          the destructor 0x4866D0 found it                                       */
    RK_EV_SHOT,        /* the 36-byte 0x0D of a fire function, as it would be sent, but +0x1A
                          masked to bit 0 and the units at +0x1F and +0x21 as their IDs (u16;
                          0xFFFF for a slot past the array); a meteor's +0x1A..+0x22 and the low
                          nibble of +0x23 are zero                                               */
    RK_EV_INTERCEPT,   /* i32 x, y, z, u16 the weapon (its index), u16 0: area damage 0x49A120
                          intercepted a projectile with this target point and weapon            */
    RK_EV_START,       /* u32 unit ID, i32 script index, u32 argc, i32 args x4: a broadcast
                          start (0x456190, 0x456200, 0x456290)                                    */
    RK_EV_START_NAMED, /* u32 unit ID, u8 argc, u8 now (the start's third argument: nonzero runs
                          the script at once), u16 0, i32 args x4, then the script's name (at
                          most REC_NAME_MAX bytes, no NUL): a start only the owner's machine
                          makes (0x4B0A70, 0x4B0940); the second argument is 0 at every site     */
    RK_EV_BUILT,       /* u32 builder's ID, u32 built unit's ID: 0x41B8D0 was called           */
    RK_EV_STATE,       /* u32 unit ID, u8 the new +0x10E, u8 0 x3: 0x48B090 changed it         */
    RK_EV_REPLY,       /* u32 unit ID, u32 kind, u8 root (0 0x47F780, 1 0x47F850), u8 text given,
                          u16 0, then the text (at most REC_TEXT_MAX bytes, no NUL)             */
    RK_EV_SPRAY,       /* u32 unit ID, u32 reverse (0 0x4720D0, 1 0x472200), i32 point x, y, z,
                          i32 box x6, u32 layer: a nano spray, credited to the unit whose
                          QueryNanoPiece gave its point                                           */
    RK_KINDS
};

/* why a recording ended */
enum RecEnd {
    REC_END_TEARDOWN = 1,   /* the level ended (0x491B60): the recording is complete             */
    REC_END_RING,           /* an iteration's records did not fit in the ring                    */
    REC_END_WRITE,          /* the writer could not write the file                              */
    REC_END_TABLE,          /* a declared table grew and its shadow could not                   */
    REC_END_LEVEL,          /* the unit array or the clock changed without a teardown            */
    REC_END_MODE,           /* the game stopped being a skirmish                                 */
    REC_END_ORDERS          /* an order list broke what the capture rests on: longer than
                               REC_ORDERS_MAX, a node that is not one, or no memory for its image */
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
                          RK_UNIT per live unit with the RK_ORDERS of its lists that are not
                          empty, an RK_PLAYER and an RK_CONTROLLER per player, an RK_VIEWER, and
                          an RK_TABLE_SIZE and its spans per declared table. A dump is one of
                          these, stored. It follows the stream block that ends with the events
                          of the step after `last`, so a reader that starts from it takes those
                          from the block before                                                 */
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
                          +0x96), u8 alliances (+0x108, 11 bytes), u8 0, PlayerInfo
                          (REC_INFO_BYTES, 0xFF bytes when the record has none), then the name
                          at +0x2B: a seat at the start                                           */
    RT_LEVER,          /* u8 present, u8 text read, u16 name bytes, the name, then its text       */
    RT_LAYOUT,         /* string: replay-layout.tsv's SHA-256, TAGPU_REPLAY_LAYOUT_SHA256        */
    RT_TABLE,          /* u16 index, u8 class, u8 key, u32 row bytes, u16 flags, u16 0, string
                          name: a declared table the records name by index                       */
    RT_OPTIONS,        /* the game as the simulation ran it at the start: u32 LosType bits 0-2
                          (main+0x14281), u32 ActiveCommanderDeath (main+0x37EF6), u32 difficulty
                          (main+0x37EEE), u32 fixed start positions ([main+0x29A0]+0x118), u16 the
                          player count (main+0x2A3C), u8 the controlled player (main+0x2A42), u8
                          the viewed player (main+0x2A43)                                        */
    RT_SETTINGS_MAP,   /* string: the map as the skirmish settings name it, [main+0x29A0]+0x11C  */
    RT_WEAPONS,        /* u32 slots, u32 FNV-1a of every weapon's name (0x20 bytes at +0), in
                          slot order                                                             */
    RT_END_REASON = 32,/* u32 RecEnd                                                            */
    RT_LAST,           /* u32: the last recorded GameTime                                       */
    RT_ITERATIONS,     /* u32: iterations recorded                                              */
    RT_CORRECTIONS,    /* u8 record kind (RK_UNIT, RK_PLAYER, RK_TABLE_SPAN or RK_ORDERS), u8 0,
                          u16 the table's index or the list (0 for the others), u32 the dword's
                          offset in the image (REC_UNIT_IMAGE for a unit's liveness,
                          REC_ORDERS_LENGTH for a list's length), u32 count: one entry for each
                          dword that differed. Past 256 of them, one entry of kind 0 and offset
                          0xFFFFFFFF counts the rest                                              */
    RT_BAD_IDS,        /* u32: id rows found outside their tables, counted each time an image
                          is taken                                                               */
    RT_RING_HIGH,      /* u32: the most bytes the ring held                                     */
    RT_OFF_THREAD,     /* u32: engine calls an event hook refused because they ran off the game
                          thread, so the recording lacks their events                           */
    RT_UNCREDITED      /* u32: nano sprays no QueryNanoPiece preceded in their handler's call,
                          recorded with unit ID 0                                               */
};

#endif
