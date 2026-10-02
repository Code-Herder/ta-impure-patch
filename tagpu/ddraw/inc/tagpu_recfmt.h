#ifndef TAGPU_RECFMT_H
#define TAGPU_RECFMT_H
/* tagpu_recfmt.h -- the demo recorder's records: what the capture puts into the ring, and what a
   recording's blocks hold (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S1, S2a,
   S3a and S3b).
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
   are whole dwords, padded with zero; a delta names the dwords that changed. Two images are not:
   a cell of the feature grid, its 13 bytes (the cell record), and a record of the wreck pool,
   its 0x30 bytes, laid out as its def makes it (the wreck3do or the wreckgaf record, REC_WRECK_*
   below). A record of the projectile pool images as its 0x6B bytes padded to a dword, its four
   pointers as IDs (the proj record). The globals image is the fields replay-globals.tsv lists, one
   slot each (tagpu_replay_globals.h, which tools/replay-globals-gen.py generates from it). The
   explored map *(main+0x14273) is its own bytes, REC_MAPPED_BYTES of the header's grid. Everything
   is little-endian.

   AN ID is one more than the target's row in its table, so NULL's is 0 and a record of zeros
   images to zeros: the rows a table gains are zero on both sides (RK_TABLE_SIZE), and a row of
   zeros needs no span. The IDs below are the rest that are not rows, and an order node's owned
   object is recorded as its class code (RL_T_OWNED). An ID is the recording's name for a row,
   never the engine's own index, which the records also carry as plain values: a unit image holds
   its type's ID at +0x92 and the engine's type index, one less, at +0xA6, and a SIMF_ID or
   SIMF_TARGET field of a declared table holds the engine's index or slot as it is
   (tagpu_simreg.h). An event names a unit by its ID, the slot's ID. */

#include <stdint.h>
#include "tagpu_replay_globals.h"

#define REC_FORMAT        4

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
#define REC_CELL_BYTES    0x0D    /* a feature-grid cell ([main+0x14287], row-major, 0x481550)  */
#define REC_CELLS_MAX     4096    /* the most cells one RK_CELLS carries                          */
#define REC_WRECK_BYTES   0x30    /* a wreck-pool record (*(main+0x1420B))                        */
#define REC_WRECK_MODEL   0x04    /* a 3DO record's Object3do: 0 in its image (wreck3do, model)  */
#define REC_WRECK_SEQ     0x0C    /* a GAF record's two sequence states' GAF pointers, each      */
#define REC_WRECK_SEQ2    0x18    /* imaged as its def's field (wreckgaf, seq and shadow)        */
#define REC_WRECK_ANCHOR  0x28    /* u16 x, u16 z: the cell the record belongs to                 */
#define REC_WRECK_DEF     0x2C    /* u16: the def, whose FeatureDef +0xFE bit 0 says GAF          */
#define REC_WRECK_FLAGS   0x2F
#define REC_WRECK_SHADOW  0x04    /* REC_WRECK_FLAGS bit 2: the second state is in use. The burn
                                     and FeatureDie set it exactly when they write that state
                                     (0x42348E, 0x4236B5) and clear it otherwise; the tick
                                     (0x4243E1, 0x424473) and the draw (0x46A6E8) read the state
                                     behind it alone. While it is clear, REC_WRECK_SEQ2 holds what
                                     the record's last tenant left -- a sinking 3DO corpse's
                                     velocity (0x424292) -- and images as REC_ID_NULL         */
#define REC_SEQ_FIRST     0xAC    /* the FeatureDef's GAF sequence pointers, a dword each: a
                                     sequence's ID is its field's (offset - 0xAC) / 4 + 1     */
#define REC_SEQ_FIELDS    8
#define REC_TEXT_MAX      128   /* the bytes of a reply's text an event carries                */
#define REC_PROJ_BYTES    0x6B    /* a projectile record (*(main+0x141F7)); the allocation 0x499A30
                                     holds the pool's records with no slack                     */
#define REC_PROJ_IMAGE    0x6C
#define REC_PROJ_DWORDS   (REC_PROJ_IMAGE / 4)                        /* 27                     */
#define REC_PROJ_BITS     ((REC_PROJ_DWORDS + 7) / 8)                 /* 4 bytes of a delta's bitmap */
#define REC_PROJ_DELTA_HEAD ((2 + REC_PROJ_BITS + 3) & ~3)            /* 8: the index and the bitmap */
#define REC_GLOBALS_BITS  ((REC_GLOBALS_DWORDS + 7) / 8)
#define REC_GLOBALS_DELTA_HEAD ((REC_GLOBALS_BITS + 3) & ~3)          /* the bitmap, to a dword  */
#define REC_MAIN_BYTES    0x3924D /* the main block: the allocation 0x41D941 makes, less its pad  */
#define REC_DATA_BYTES    0x2A65C /* .data, from 0x501000: its section's VirtualSize             */
#define REC_IMAGE_CHUNK   0x8000  /* the most bytes of an image one RK_IMAGE carries             */
#define REC_MAPPED_CHUNK  0x8000  /* the most bytes of the explored map one RK_MAPPED carries    */
#define REC_MAPPED_RUNS   0x7000  /* the most bytes of runs one RK_MAPPED_RUNS carries           */
/* the explored map's bytes for a grid of w x h cells: a u16 a 2 x 2 cells, in LoadMap's 32-bit
   arithmetic (DISASSEMBLED 0x483CF6..0x483D02: imul, shl 1, shr 2) */
#define REC_MAPPED_BYTES(w, h) (((w) * (h) * 2u) >> 2)

_Static_assert(REC_UNIT_IMAGE == ((REC_MOVER_AT + REC_MOVER_BYTES + 3) & ~3), "the unit image");
_Static_assert(REC_PLAYER_IMAGE == REC_INFO_AT + ((REC_INFO_BYTES + 3) & ~3), "the player image");
_Static_assert(REC_INFO_AT == ((REC_PLAYER_BYTES + 3) & ~3), "PlayerInfo at a dword");
_Static_assert(REC_ORDER_IMAGE == ((REC_ORDER_BYTES + 3) & ~3), "the order image");
_Static_assert(8 + REC_ORDERS_CHUNK * REC_ORDER_IMAGE <= 0xFFFF, "an RK_ORDERS fits its length");
_Static_assert(8 + REC_CELLS_MAX * REC_CELL_BYTES <= 0xFFFF, "an RK_CELLS fits its length");
_Static_assert(REC_PROJ_IMAGE == ((REC_PROJ_BYTES + 3) & ~3), "the projectile image");
_Static_assert(REC_PROJ_DWORDS == 27 && REC_PROJ_DELTA_HEAD == 8, "the projectile delta");
_Static_assert(REC_GLOBALS_DWORDS * 4 <= 0xFFFF, "an RK_GLOBALS fits its length");
_Static_assert(8 + REC_IMAGE_CHUNK <= 0xFFFF, "an RK_IMAGE fits its length");
_Static_assert(4 + REC_MAPPED_CHUNK <= 0xFFFF, "an RK_MAPPED fits its length");
_Static_assert(8 + REC_MAPPED_RUNS <= 0xFFFF, "an RK_MAPPED_RUNS fits its length");

/* what an id row holds besides a row's ID */
#define REC_COUNTS_CHUNK  1024          /* RK_COUNTS: rows a record                               */
#define REC_COUNT_NAME    32            /* RK_COUNT_NAME: a script's name, NUL-padded              */
#define REC_COUNT_DLL     0x80000000u   /* RK_COUNTS: a caller inside Impure's DLL, as its offset  */
/* RK_EV_FEATURE's phase: where in the step 0x495490 the root was made -- in the unit tick or
   before it, in the projectile tick 0x49B720 (called at 0x495513), or from the explosion tick
   0x420F30 (0x495539) to the step's end, the feature tick 0x424050 included */
#define REC_PHASE_UNITS   0
#define REC_PHASE_PROJ    1
#define REC_PHASE_LATE    2
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
                          preceded, u32 grid cells found unobserved. The capture's last record:
                          after it, the ring is the writer's                                      */
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
                          owner (its player), u8 layer (& 3), u32 finished, i32 x, y, z, u8 the
                          water state +0x10A the unit took from its slot (the create writes none
                          of it), u8 0, u16 0: the create 0x485F50 made a unit. Taken at its exit,
                          so the events of the calls it makes -- its state byte, its scripts --
                          come before it                                                         */
    RK_EV_DEATH,       /* u32 unit ID, u32 killer's ID, u8 killer's player (10: none), i8 severity,
                          u8 the record's +0xA, u8 carried (C4's decision), then the unit's image as
                          the destructor 0x4866D0 found it. It opens a bracket its RK_EV_DEATH_END
                          closes                                                                  */
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
    RK_EV_FEATURE,     /* u8 root (RecRoot), u8 the step's phase it was made in (REC_PHASE_*),
                          u16 0, u32 the call's site (its return address
                          less 5), then the root's arguments (RecRoot): a feature function called
                          outside every other one -- outside a unit's death, or inside its
                          explosion -- or the feature tick. The records up to its
                          RK_EV_FEATURE_END are the call's: the draws it made, the tick's cells,
                          and the events of what it nested. A bracket holds no other and no death,
                          and the tick's is inside none                                           */
    RK_EV_FEATURE_END, /* u8 root, u8 0, u16 0, u32 what it returned: a spawn's or a swap's record
                          as its index + 1 (0: none, 0xFFFFFFFF: not a record of the pool), a
                          destroy's 0 or 1, and 0 for the rest, whose EAX is no result          */
    RK_EV_DRAW,        /* u8 site (RecDrawSite), u8 0, u16 0, u32 n, u32 the value: the simulation
                          RNG 0x4B6C30(n) at one of the feature code's six calls of it, inside a
                          bracket: a feature call's, or a death's when a call its destructor made
                          itself drew                                                             */
    RK_EV_DEATH_END,   /* u32 unit ID: the destructor RK_EV_DEATH opened has returned. The records
                          between are the death's: the feature calls its explosion made, each a
                          root, the events of what it nested, and the deaths nested in it, each a
                          bracket of its own. What its destructor made itself -- its corpse -- is
                          not recorded: a replay's destructor makes it                            */

    /* THE FEATURES, images like the ones above (the slice note's S3a) */
    RK_CELLS,          /* u16 x, u16 z, u16 cells, u16 0, then `cells` cell images: a run of the
                          grid's row z from column x. The first iteration carries every cell,
                          the rest the cells that changed, a snapshot the cells that differ from
                          the first iteration's and a dump every cell. Inside the feature tick's
                          RK_EV_FEATURE: the cells that changed since the iteration's images      */
    RK_WRECK,          /* u16 index, u16 0, then the record's image: a record that changed        */
    RK_FEATURES,       /* i32 the active list's head (main+0x14213), i32 the settled list's
                          (+0x14217), i32 the free list's (+0x1421B), u32 the reproduction cursor
                          (+0x14257): at the first iteration, and when any changed                */
    RK_GRID,           /* ring only: u16 columns, u16 rows: every cell's image, the grid's first,
                          is in the ring's `grid` (tagpu_recorder.h), which the writer takes and
                          writes as RK_CELLS where this record stands                             */

    /* THE GLOBALS AND THE PROJECTILES (the slice note's S3b) */
    RK_GLOBALS,        /* the globals image, REC_GLOBALS_DWORDS dwords: at the first iteration, in
                          every snapshot and dump, and as a correction                            */
    RK_GLOBALS_DELTA,  /* u8 bits[REC_GLOBALS_BITS], zero to REC_GLOBALS_DELTA_HEAD, then one u32
                          for each bit set, lowest first: the dwords of the image that changed    */
    RK_PROJ_HEAD,      /* u32 the pool's count (main+0x141F3), u32 hw, its written extent: at the
                          first iteration and when either changed. The records [hw, the pool's
                          end) are zero: the allocation zeroes them and only an append past the
                          extent writes one                                                       */
    RK_PROJ,           /* u16 index, u16 0, the image: record `index`, below hw, whole            */
    RK_PROJ_DELTA,     /* u16 index, u8 bits[REC_PROJ_BITS], u8 0 x2, then one u32 for each bit
                          set, lowest first: the dwords of a record's image that changed          */
    RK_VERIFY_GLOBALS, /* ring only: the globals image read afresh, for the rotating check        */
    RK_VERIFY_PROJ,    /* ring only: u32 count, u32 hw, u32 first, then the images of the records
                          [first, first + (len - 12) / REC_PROJ_IMAGE) read afresh                */
    RK_IMAGE,          /* dump only: u8 which (0 the main block, 1 .data), u8 0, u16 0, u32 the
                          offset from main or from .data's start, then the bytes from there, at
                          most REC_IMAGE_CHUNK: the two images whole, for `tarec compare --images` */
    RK_MAPPED,         /* u32 offset, then at most REC_MAPPED_CHUNK bytes of the explored map from
                          there: the map whole is these from offset 0 to its end, in order, at the
                          first iteration, in every snapshot and in every dump                    */
    RK_MAPPED_RUNS,    /* runs, each u32 offset, u32 length, then that many bytes of the explored
                          map, the next run at the next multiple of 4; at most REC_MAPPED_RUNS
                          bytes in all: the bytes of the map that changed in an iteration         */
    RK_SIGHT,          /* dump only: u8 the player record, u8 0, u16 0, u32 the offset, then at
                          most REC_IMAGE_CHUNK bytes of the record's sight counters from there:
                          its counters whole are these from offset 0 to their end, in order, for
                          each record 0..9 that has them, for `tarec compare-dumps`             */
    RK_COUNTS,         /* dump only: u32 keys that found no row (the table full), u32 the sinks
                          counted (bit k: the recorder's CNT_SINKS[k]), u32 1 when the level
                          counted, u32 n, then n rows of u32 site (its VA), u32 caller (the
                          return address, (offset | REC_COUNT_DLL) inside Impure's DLL, 0 where
                          the site is no entry), u32 aux, u32 count: how often each counted site
                          ran since the level's first capture point (tagpu_recorder.c, THE
                          COUNTS), at most REC_COUNTS_CHUNK rows a record, the last holding fewer  */
    RK_COUNT_NAME,     /* dump only: u32 aux, char[REC_COUNT_NAME] NUL-padded: the script name
                          whose FNV-1a is the aux of the COB thread allocation 0x4B08C0's rows   */
    RK_KINDS
};

/* A feature root, and its arguments after the site. A cell the engine names by address is its
   column and row (0xFFFF 0xFFFF: not a cell of the grid, and never replayed). */
enum RecRoot {
    FR_TICK = 0,       /* 0x424050(): the feature tick, the one call at 0x495585                   */
    FR_SPAWN,          /* 0x423C50(cell, def, pos, turn, v): u16 x, u16 z, u16 def, u8 v & 0xF,
                          u8 given (bit 0 pos, bit 1 turn), i32 pos[3], u16 turn[3], u16 0         */
    FR_DESTROY,        /* 0x4246B0(cell, force): u16 x, u16 z, u32 force                           */
    FR_DIE,            /* 0x423550(x, z, reclaimed): i32 x, i32 z, u32 reclaimed                   */
    FR_BURN,           /* 0x4233A0(x, z, flag): i32 x, i32 z, u32 flag                             */
    FR_DAMAGE,         /* 0x4244B0(cell, x, z, weapon): u16 x, u16 z (the cell), i32 x, i32 z, u32
                          the weapon's ID                                                          */
    FR_SWAP,           /* 0x423710(x, z, reclaimed): i32 x, i32 z, u32 reclaimed                   */
    FR_ROOTS
};
#define REC_ROOT_ARGS_MAX 28   /* FR_SPAWN's                                                    */
#define REC_DEATHS_MAX    32   /* the deaths a bracket may nest, and the capture's frames        */

/* An event's length and a feature root's: the one rule the writer and the reader both apply
   (tagpu_recwriter.c, tagpu_recreader.c), each length the event's comment above gives -- fixed,
   or a fixed part and at most a bounded tail. A record that fails it is malformed: the writer
   abandons the recording at it, and the reader refuses the step. */
static __inline int rec_event_len(unsigned kind, unsigned len)
{
    switch (kind) {
    case RK_EV_CREATE:      return len == 28;
    case RK_EV_DEATH:       return len == 12 + REC_UNIT_IMAGE;
    case RK_EV_SHOT:        return len == 36;
    case RK_EV_INTERCEPT:   return len == 16;
    case RK_EV_START:       return len == 28;
    case RK_EV_START_NAMED: return len >= 24 && len <= 24 + REC_NAME_MAX;
    case RK_EV_BUILT:       return len == 8;
    case RK_EV_STATE:       return len == 8;
    case RK_EV_REPLY:       return len >= 12 && len <= 12 + REC_TEXT_MAX;
    case RK_EV_SPRAY:       return len == 48;
    case RK_EV_DRAW:        return len == 12;
    default:                return 0;
    }
}

/* an RK_EV_FEATURE's arguments after its site, by its root */
static __inline int rec_root_len(unsigned root, unsigned len)
{
    static const unsigned char args[FR_ROOTS] = { 0, 28, 8, 12, 12, 16, 12 };
    return root < FR_ROOTS && len == 8 + args[root];
}

/* The feature code's calls of the simulation RNG 0x4B6C30: every one from 0x4232F0 to 0x4250FF,
   which holds the feature functions, the spread 0x4239C0 and the tick 0x424050. */
enum RecDrawSite {
    FS_BURN = 0,       /* 0x4234C2, the burn's length                                             */
    FS_SPREAD,         /* 0x423A38, the spread to a neighbour                                     */
    FS_DOWNWIND,       /* 0x423B40, the spread downwind                                            */
    FS_REPRODUCE,      /* 0x424113, reproduction's chance                                          */
    FS_REPRODUCE_X,    /* 0x424159, and its column                                                  */
    FS_REPRODUCE_Z,    /* 0x424167, and its row                                                     */
    FS_SITES
};

/* why a recording ended */
enum RecEnd {
    REC_END_TEARDOWN = 1,   /* the level ended (0x491B60): the recording is complete             */
    REC_END_RING,           /* an iteration's records did not fit in the ring                    */
    REC_END_WRITE,          /* the writer could not write the file                              */
    REC_END_TABLE,          /* a declared table grew and its shadow could not                   */
    REC_END_LEVEL,          /* the unit array or the clock changed without a teardown            */
    REC_END_MODE,           /* the game stopped being a skirmish                                 */
    REC_END_ORDERS,         /* an order list broke what the capture rests on: longer than
                               REC_ORDERS_MAX, a node that is not one, or no memory for its image */
    REC_END_FEATURES,       /* the feature grid, the wreck pool or the feature defs are not the
                               ones the recording began with, or the feature calls nested deeper
                               than the capture tracks                                          */
    REC_END_PROJ,           /* the projectile pool is not the one the recording began with, or
                               its count is past its records                                    */
    REC_END_MAPPED          /* the explored map is not the one the recording began with          */
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
    RB_SNAPSHOT,       /* records that build the whole world at `last`: a TICK, an RK_UNIT per
                          live unit with the RK_ORDERS of its lists that are not empty, an
                          RK_PLAYER and an RK_CONTROLLER per player, an RK_VIEWER, an
                          RK_TABLE_SIZE per declared table and an RK_TABLE_SPAN for each of its
                          spans that is not all zero (RK_TABLE_SIZE zeroes the table, which
                          starts empty), the RK_CELLS of every cell that differs from the
                          recording's first iteration (the base: a reader takes it from the first
                          iteration it reads, which every playing reads first), an RK_WRECK per
                          record that is not all zero, an RK_FEATURES, the RK_GLOBALS, the
                          RK_PROJ_HEAD with an RK_PROJ per record below its hw, and the explored
                          map whole (RK_MAPPED). A dump is one of
                          these, stored, with every cell and no base, and the players' sight
                          counters (RK_SIGHT) and the two images whole (RK_IMAGE) after the
                          rest. A snapshot follows the stream block that ends
                          with the events of the step after `last`, so a reader that starts from
                          it takes those from the block before                                  */
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
    RT_TABLE,          /* u16 index, u8 class, u8 key, u32 row bytes, u16 flags, u16 0, u32 the
                          layout hash (tagpu_simreg_layout_hash: 0 for a table of plain values),
                          string name: a declared table the records name by index. The header
                          states the declaration but not its layout, so the hash is what tells a
                          reader that a table of the same name and row size lays its fields out
                          as its own declaration does                                           */
    RT_OPTIONS,        /* the game as the simulation ran it at the start: u32 LosType bits 0-2
                          (main+0x14281), u32 ActiveCommanderDeath (main+0x37EF6), u32 difficulty
                          (main+0x37EEE), u32 fixed start positions ([main+0x29A0]+0x118), u16 the
                          player count (main+0x2A3C), u8 the controlled player (main+0x2A42), u8
                          the viewed player (main+0x2A43)                                        */
    RT_SETTINGS_MAP,   /* string: the map as the skirmish settings name it, [main+0x29A0]+0x11C  */
    RT_WEAPONS,        /* u32 slots, u32 FNV-1a of every weapon's name (0x20 bytes at +0), in
                          slot order                                                             */
    RT_FEATURES,       /* u32 the grid's columns (main+0x14233), u32 its rows (+0x14237), u32 the
                          wreck pool's records, u32 the feature defs (+0x14253), u32 FNV-1a of
                          every def's name (0x20 bytes at +0), in index order                     */
    RT_GLOBALS,        /* string: replay-globals.tsv's SHA-256, TAGPU_REPLAY_GLOBALS_SHA256       */
    RT_PROJECTILES,    /* u32: the projectile pool's records, TAGPU_LIM_PROJ                      */
    RT_FRAME,          /* u32, a dump's only: taken at a frame visit between two steps, at this
                          GameTime (tagpu_rec_dump_now). The records hold step GameTime's result,
                          labelled iteration GameTime+1; the main block and .data images also hold
                          what the rest of the frame wrote (the draw, the scheduler 0x495230's
                          pacing words), so a frame dump compares only with another one */
    RT_DLL,            /* u32 base, u32 SizeOfImage, a dump's only: where Impure's DLL lies, for
                          `tarec scan`'s address ranges                                         */
    RT_FIELDS,         /* u16 table index, u16 n, then n fields of u16 off, size, repeat, step,
                          u8 kind, u8 id, u16 0, a dump's only: a recorded table's layout as the
                          simreg declares it (SimField), for `tarec scan`                         */
    RT_END_REASON = 32,/* u32 RecEnd                                                            */
    RT_LAST,           /* u32: the last recorded GameTime                                       */
    RT_ITERATIONS,     /* u32: iterations recorded                                              */
    RT_CORRECTIONS,    /* u8 record kind (RK_UNIT, RK_PLAYER, RK_TABLE_SPAN, RK_ORDERS, RK_GLOBALS,
                          RK_PROJ or RK_PROJ_HEAD), u8 0, u16 the table's index or the list (0 for
                          the others), u32 the dword's offset in the image (REC_UNIT_IMAGE for a
                          unit's liveness, REC_ORDERS_LENGTH for a list's length, 0 the count and
                          4 hw for RK_PROJ_HEAD), u32 count: one entry for each dword that
                          differed. Past 256 of them, one entry of kind 0 and offset 0xFFFFFFFF
                          counts the rest                                                         */
    RT_BAD_IDS,        /* u32: id rows found outside their tables, counted each time an image
                          is taken                                                               */
    RT_RING_HIGH,      /* u32: the most bytes the ring held                                     */
    RT_OFF_THREAD,     /* u32: engine calls an event hook refused because they ran off the game
                          thread, so the recording lacks their events                           */
    RT_UNCREDITED,     /* u32: nano sprays no QueryNanoPiece preceded in their handler's call,
                          recorded with unit ID 0                                               */
    RT_UNOBSERVED      /* u32: grid cells the rotating sweep found changed that no writer's
                          observer had marked: recorded when found, late                         */
};

#endif
