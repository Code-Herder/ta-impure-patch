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
#include "tagpu_recpov.h"
#include "tagpu_reclanes.h"

/* 22: format 21 with three record codings: an explicit nano spray as its unit's last four explicit
   sprays' -- the point as it was, moved with the unit or turned about it by its heading change --
   and a residual (the events frame's scheme 2); in a stream whose header has RT_HOLD, each order
   node matched to the kept list's node of the same action and issue time (the orders frame's
   scheme 2); and a surviving projectile's bytes 0x34..0x6A as lanes of the record's field widths
   (RCS_PROJ below). 21 was format 20 with the unit frame's listing from state both sides keep --
   each carried slot a 2-bit symbol, the others ranks among the candidates or woken slots, the lane
   masks ids into the run's masks most recent first -- and each lane's count before its column
   (RCS_UNITS below). 20 was format 19 with the script slice's place in the event stream
   (RK_EV_SLICE), the owner's COB queries the playing makes again (RK_EV_QUERY) and a yard's
   opening or closing at its place (RK_EV_YARD). 19 was format 18 with more of the unit frame's
   rules -- a unit's hit points while it is built, its HP percentage every 30 ticks, its sort
   bucket, a ground unit's speed and sight cell, the turn rate's one-tick blip -- and their
   constants in the header: RT_TYPES, each unit type's, and RT_PHYSICS's bucket columns. 18 was
   format 17 with the sectioned container's kinds as per-iteration patterns -- by reference to the
   run's last 16, with markers for an empty feature tick and a predicted RK_FEATURES, and no count
   for a run the frames fix -- and an RK_TICK body for a block's first iteration alone (THE
   SECTIONED CONTAINER below). 17 was format 16 in another container -- a run of sectioned stream
   blocks is one zstd frame at level 19, each block a flushed chunk of it, and a snapshot is packed
   against the one before it (RBF_CHAINED, THE RUN AND THE CHAIN below). 16 was format 15 with a
   skirmish recording's cells held (RT_HOLD's RECL_CELLS): between refreshes the stream carries the
   cells' unit slots and yard bit at a value it wrote before, a feature tick's input cells only
   where a byte other than those changed, and the tick's source test as FS_SOURCE, as a multiplayer
   recording does. 15 was format 14 with a skirmish stream's nano sprays written as sessions
   (RK_SPRAY_SESSION, RT_HOLD's RECL_SPRAYS): a spray its unit's session predicts is left out, and
   every reader makes it again at the end of its iteration. 14 was format 13 with a hold table of
   rows: a skirmish stream holds the weapon slots' aim and target state, the resource ledger's
   accumulators and the main order list's handler state between refreshes (replay-hold.tsv; RT_HOLD
   carries the masks). 13 was format 12 with the stream written by the lossy filter
   (tagpu_reclossy.c): a skirmish header's RT_HOLD names the bytes the stream holds between
   refreshes, a refresh iteration ends its stream block (RBF_REFRESH), and a test launch may write
   the exact records beside the stream (RB_EXACT). A reader takes this format alone. The bytes: a
   stream block may be a sectioned container whose unit frame carries the unit deltas, whose pool
   frame carries the projectile pool coded by identity, and whose four record frames -- the cells,
   the orders, the events and the state -- carry the rest but RCS_OTHER's (THE SECTIONED CONTAINER
   below). */
#define REC_FORMAT        22

static __inline int rec_format_supported(unsigned format)
{
    return format == REC_FORMAT;
}

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
#define REC_PLAYERS       10    /* the records a game seats and a recording carries; the eleventh,
                                   REC_VIEWER, is the engine's no-player record, which a puppet
                                   level seats the viewer in                                   */
#define REC_VIEWER        10    /* the viewer's record on a puppet level (tagpu_replay.c, THE
                                   VIEWER'S SEAT)                                              */
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
_Static_assert(REC_LANES_IMAGE == REC_UNIT_IMAGE, "replay-lanes.tsv tiles the unit image");
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
                          preceded, u32 grid cells found unobserved, u32 the game's end flags
                          (RT_GAME_END), then REC_PLAYERS of RT_SEAT_END's 12 bytes, a seat's
                          result REC_RESULT_NONE when the record is no seat; REC_END_BYTES in all.
                          The capture's last record: after it, the ring is the writer's          */
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
    RK_EV_SHOT,        /* the 36-byte 0x0D of a fire function, as it would be sent, but +1..+0xC
                          the start its build used (0x49D9C0 sends the shooter's centre there
                          instead: tagpu_patches.c, wpn_tx_fired), +0x1A masked to bit 0 and the
                          units at +0x1F and +0x21 as their IDs (u16; 0xFFFF for a slot past the
                          array); a meteor's +0x1A..+0x22 and the low nibble of +0x23 are zero  */
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
                          explosion -- or the feature tick, or, in a multiplayer recording, the
                          corpse helper directly inside a death (FR_CORPSE). The records up to its
                          RK_EV_FEATURE_END are the call's: the draws it made, the tick's cells,
                          and the events of what it nested. A bracket holds no other and no death,
                          and the tick's is inside none                                           */
    RK_EV_FEATURE_END, /* u8 root, u8 0, u16 0, u32 what it returned: a spawn's or a swap's record
                          as its index + 1 (0: none, 0xFFFFFFFF: not a record of the pool), a
                          destroy's 0 or 1, the corpse's record its spawn took (index + 1, 0
                          none), and 0 for the rest, whose EAX is no result                     */
    RK_EV_DRAW,        /* u8 site (RecDrawSite), u8 0, u16 0, u32 n, u32 the value: the simulation
                          RNG 0x4B6C30(n) at one of the feature code's six calls of it, inside a
                          bracket: a feature call's, or a death's when a call its destructor made
                          itself drew; or site FS_SOURCE: the source cell's slot A as the tick
                          read it, n the cell's index                                            */
    RK_EV_DEATH_END,   /* u32 unit ID: the destructor RK_EV_DEATH opened has returned. The records
                          between are the death's: the feature calls its explosion made, each a
                          root, the events of what it nested, and the deaths nested in it, each a
                          bracket of its own. What its destructor makes itself -- its corpse -- a
                          skirmish recording leaves to the replay's destructor; a multiplayer
                          recording records it as FR_CORPSE inside the bracket                   */

    /* THE FEATURES, images like the ones above (the slice note's S3a) */
    RK_CELLS,          /* u16 x, u16 z, u16 cells, u16 0, then `cells` cell images: a run of the
                          grid's row z from column x. The first iteration carries every cell,
                          the rest the cells that changed, a snapshot the cells that differ from
                          the first iteration's and a dump every cell. Inside the feature tick's
                          RK_EV_FEATURE: the cells that changed since the iteration's images. A
                          stream that holds its cells (RECL_CELLS) carries their slots and yard
                          bit at a value it wrote before, and a tick's input run only where a
                          byte other than those changed (tagpu_reclossy.c)                       */
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
    RK_MINIMAP,        /* ring only: an RB_MINIMAP's payload, right after the RK_HEADER          */
    RK_EV_NET_CREATE,  /* RecNetCreate: CreateFromNetwork 0x4861D0 made this replica, observed at
                          its success exit after B4 stamps it: a multiplayer recording's only     */

    /* THE PERSPECTIVE of the human seat the skirmish commands (tagpu_recpov.h; the plan,
       demo-recorder-perspective-streams.md). A network-mode recorder writes none of them: a
       multiplayer file's perspectives are contributions. */
    RK_POV,            /* RecPov: the seat's camera and cursor at this iteration's capture point,
                          taken at most every `every` ticks and only when it differs from the
                          seat's last RK_POV. With the images, a stream's only                    */
    RK_CLICK,          /* RecClick: an engine click action ran (0x498F70 at 0x4995B3, 0x499100 at
                          0x4993AC). Taken between two capture points, outside the tick loop, so
                          it follows the step's events, outside every bracket; a stream's only  */
    RK_SELECT,         /* RecSel, BEGIN|END, then its RecSelEntry: the seat's whole selection, its
                          slots ascending inside the seat's block, when it changed other than by a
                          death. With the images, and in a snapshot                               */
    RK_POV_AT,         /* u32 the GameTime it was taken, then a RecPov: a snapshot's copy of one of
                          a seat's two last samples, which a stream's RK_POV dates by its
                          iteration; a snapshot's only                                            */
    RK_SPRAY_SESSION,  /* u32 unit ID, u8 mode (bits 0-2 the period p: 0, 1, 2 or 4; bit 3
                          FOLLOW, with p > 0 only; every other value refused), u8 0, u16 0: the
                          unit's spray session from this iteration on, p 0 closing it. At the end
                          of each iteration in which a unit with a session has no RK_EV_SPRAY of
                          its own, a reader makes the spray the session implies: the unit's spray
                          of the iteration p GameTimes before, its point moved by the unit's
                          position change since with FOLLOW (tagpu_reclossy.c, THE SPRAY
                          SESSIONS). After the iteration's last other record, slots ascending;
                          a skirmish stream's only, with RT_HOLD's RECL_SPRAYS; in RCS_OTHER     */
    RK_EV_SLICE,       /* u32 unit ID: the unit tick's script slice of that unit, DoScriptsNow
                          0x4B0D60 with dt 1 at 0x48ADEB, recorded just before it ran because the
                          unit's COB object had an engine start (0x4B0B00, 0x4B0940) since the
                          iteration's capture point. A top-level event of the step, never inside a
                          feature call or a death; at most one per slot an iteration. A playing gives
                          the slice here instead of in its own unit tick (tagpu_replay.c, THE SCRIPT
                          SLICES)                                                                 */
    RK_EV_QUERY,       /* u32 unit ID, u8 the query (REC_QUERY_*), u8 0 x3: a COB query only an
                          order handler makes, 0x4B0BC0 on that unit's COB object. The handler runs
                          in its own unit's turn, after that unit's slice; the queried unit is the
                          handler's, or for SelfRepair's QueryNanoPiece the repairing pad
                          (0x402525..0x40252E), whose own slice can come later in the tick. A
                          top-level event of the step, like RK_EV_SLICE. A playing makes the same
                          query at this place (tagpu_replay.c, THE OWNER'S QUERIES)               */
    RK_EV_YARD,        /* u32 unit ID, u8 the yard (1 open, 0 closed), u8 0 x3: YARD_OPEN 0x47DAC0
                          passed its refusal 0x47D970 and wrote the structure's +0x10F bit 2
                          (0x47DAF1), then re-claimed its footprint (0x47DAFD). Wherever the
                          setter runs, and where it stands is THE YARD RULE (below): the yard of a
                          live unit at its place, or of the create that follows it. A playing
                          writes the first at this place and checks the second at its create
                          (tagpu_replay.c, A RECORDED YARD_OPEN)                                   */
    RK_KINDS
};

typedef struct RecNetCreate {
    uint32_t unit;                    /* slot ID                                                */
    uint16_t type;                    /* engine index, from the constructor's record +1        */
    uint8_t owner, water;
    uint32_t birth;                   /* B4's observed token, including lower-bound/unknown    */
    int32_t position[3];              /* constructor arguments, from its record +5             */
    float unfinished;                 /* post-B4 unit +0x104                                    */
    uint16_t hp, reserved;            /* post-B4 unit +0x108; reserved is zero                  */
} RecNetCreate;
_Static_assert(sizeof(RecNetCreate) == 32, "network create event span");
_Static_assert(RK_EV_NET_CREATE == 51 && RK_POV == 52 && RK_CLICK == 53 && RK_SELECT == 54 &&
               RK_POV_AT == 55 && RK_SPRAY_SESSION == 56, "existing record kinds retain their values");
_Static_assert(RK_EV_SLICE == 57 && RK_EV_QUERY == 58 && RK_EV_YARD == 59 && RK_KINDS == 60,
               "the script slice's, the owner's query's and the yard's kinds");

/* RK_EV_QUERY's query: the script's name and its first argument's value going in, as the engine's
   only call of each passes them, the other three arguments NULL (the engine map, *The owner's COB
   queries*) */
enum RecQuery {
    REC_QUERY_BUILDINFO,   /* "QueryBuildInfo", -1: BuildingBuild's state dispatch, 0x4027FB     */
    REC_QUERY_NANOPIECE,   /* "QueryNanoPiece", 0: 0x43E400 at 0x43E427, from fifteen handlers  */
    REC_QUERIES
};
#define REC_POV_AT_BYTES (4u + (unsigned)sizeof(RecPov))

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
    FR_CORPSE,         /* 0x486360(unit, level, smoke): u32 the unit's type ID, u8 level, u8 smoke,
                          u8 the unit's +0xFF, u8 0, i16 +0x76, i16 +0x78, i32 +0x6A x3, u16
                          +0x64 x3, u16 0: a multiplayer recording's only, directly inside a death */
    FR_ROOTS
};
#define REC_ROOT_ARGS_MAX 32   /* FR_CORPSE's                                                   */
#define REC_DEATHS_MAX    32   /* the deaths a bracket may nest, and the capture's frames        */

/* An event's length and a feature root's: the one rule the writer and the reader both apply
   (tagpu_recwriter.c, tagpu_recreader.c), each length the event's comment above gives -- fixed,
   or a fixed part and at most a bounded tail. A record that fails it is malformed: the writer
   abandons the recording at it, and the reader refuses the step. */
static __inline int rec_event_len(unsigned kind, unsigned len)
{
    switch (kind) {
    case RK_EV_CREATE:      return len == 28;
    case RK_EV_NET_CREATE:  return len == sizeof(RecNetCreate);
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
    case RK_EV_SLICE:       return len == 4;
    case RK_EV_QUERY:       return len == 8;
    case RK_EV_YARD:        return len == 8;
    default:                return 0;
    }
}

/* an RK_EV_FEATURE's arguments after its site, by its root */
static __inline int rec_root_len(unsigned root, unsigned len)
{
    static const unsigned char args[FR_ROOTS] = { 0, 28, 8, 12, 12, 16, 12, 32 };
    return root < FR_ROOTS && len == 8u + args[root];
}

/* THE YARD RULE: where an RK_EV_YARD may stand in its step, the same for the writer, both readers
   and the playing. A unit is live at an event of the step when the iteration's images hold it (the
   images come before the events, RK_TICK) and no RK_EV_DEATH of its ID since, or when an
   RK_EV_CREATE of its ID came since [DISASSEMBLED 2026-10-12]:
   - A yard of a unit live at its place is that unit's yard at that place: a COB slice's, a
     query's, a script another root ran at once. Killed among them: on a skirmish recording machine
     every death goes through Send_UnitDeath 0x4864B0, which runs Killed as a query at 0x4865C3
     and only then calls the destructor (0x486679, mode 1), so Killed's yard comes before the
     death's RK_EV_DEATH. The destructor's own Killed start (0x486877) runs in mode 0 alone
     (0x48684A..0x486853), the network receiver's (0x455423), and its other starts are made with
     run-now 0: the order purge's teardown 0x43A1F0 starts TargetCleared through 0x489800
     (0x43A2B9) and StopBuilding at 0x43A251, and its dispatch of event 2 to the order's handler
     (0x43A21A) reaches handlers whose own starts pass run-now 0 -- BuildingBuild
     (0x402653..0x402717), SelfDestruct, HelpBuild and VTOL_MobileBuild (returning at 0x41C110),
     AirStrike and AirToGround, VTOL_Pickup's states 4 and 5 (EndTransport, 0x4B0940 at 0x41148D
     after 0x411479). So no script of the dying unit runs inside its own death's bracket.
   - A yard of a unit not live at its place is the yard its create's Create script opened: the
     create 0x485F50 runs Create's first slice at once (0x485D40 at 0x4860A6, run-now pushed at
     0x485DDD), after its init has written the footprint (0x485AAA, 0x485BD3) and before its
     placement 0x48A870 (0x486109) and stamp 0x47CC30 (0x48610F); CreateFromNetwork 0x4861D0 has
     the same order (0x4862BE, 0x486306, 0x48630C). The create is recorded at its exit (0x4861B3),
     so the yard comes before the unit's RK_EV_CREATE, which follows in the same step with no
     death of the slot between.
   - Anything else is malformed: a yard inside a feature call, which holds no event but its draws
     and the tick's cells; a yard naming the unit of a death still open; and a yard of a unit not
     live whose create does not follow in its step. The recorder keeps such an event out of the file and counts it
     (tagpu_recorder.c, ev_yard), so the writer never meets one; the readers refuse a file that
     holds one.
   The callers keep the state: `st` and `touched` hold a byte and an index per unit slot. */
enum { RYS_MADE = 1, RYS_GONE, RYS_HELD };
typedef struct RecYardStep {
    unsigned char* st;         /* a slot's change in the step: 0 none (the images'), RYS_*        */
    unsigned*      touched;    /* the slots whose st is set, each once                             */
    unsigned       ntouched;
    unsigned       held;       /* the slots whose yard waits for their create                      */
} RecYardStep;

static __inline void rec_yard_mark(RecYardStep* y, unsigned slot, unsigned char v)
{
    if (!y->st[slot]) y->touched[y->ntouched++] = slot;
    y->st[slot] = v;
}

/* One event of the step by THE YARD RULE: 1 well formed, 0 malformed. `live` is the images',
   `deaths` the IDs of the deaths still open. A kind it does not name, and a create or death of an
   ID outside the slots (the callers' own checks bound those), changes nothing. */
static __inline int rec_yard_take(RecYardStep* y, unsigned kind, const unsigned char* b,
                                  const unsigned char* live, unsigned slots,
                                  const unsigned* deaths, unsigned ndeaths)
{
    uint32_t id;
    unsigned slot, i;
    if (kind != RK_EV_CREATE && kind != RK_EV_DEATH && kind != RK_EV_YARD) return 1;
    id = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    slot = id - 1u;
    if (slot < 1 || slot >= slots) return kind != RK_EV_YARD;
    if (kind == RK_EV_CREATE) {
        if (y->st[slot] == RYS_HELD) y->held--;
        rec_yard_mark(y, slot, RYS_MADE);
        return 1;
    }
    if (kind == RK_EV_DEATH) {
        if (y->st[slot] == RYS_HELD) return 0;
        rec_yard_mark(y, slot, RYS_GONE);
        return 1;
    }
    for (i = 0; i < ndeaths; i++)
        if (deaths[i] == id) return 0;
    if (y->st[slot] == RYS_MADE || (!y->st[slot] && live[slot])) return 1;
    if (y->st[slot] != RYS_HELD) {
        rec_yard_mark(y, slot, RYS_HELD);
        y->held++;
    }
    return 1;
}

/* The step's end: 1 when every held yard met its create. The state goes back to the images'. */
static __inline int rec_yard_end(RecYardStep* y)
{
    int ok = !y->held;
    while (y->ntouched) y->st[y->touched[--y->ntouched]] = 0;
    y->held = 0;
    return ok;
}

/* The feature code's inputs from outside the feature world: its calls of the simulation RNG
   0x4B6C30 -- every one from 0x4232F0 to 0x4251CA, which holds the feature functions, the spread
   0x4239C0, the tick 0x424050 and the saved game's load 0x424C00 (its `ret 4` at 0x4251C8) --
   and reproduction's source test. */
enum RecDrawSite {
    FS_BURN = 0,       /* 0x4234C2, the burn's length                                             */
    FS_SPREAD,         /* 0x423A38, the spread to a neighbour                                     */
    FS_DOWNWIND,       /* 0x423B40, the spread downwind                                            */
    FS_REPRODUCE,      /* 0x424113, reproduction's chance                                          */
    FS_REPRODUCE_X,    /* 0x424159, and its column                                                  */
    FS_REPRODUCE_Z,    /* 0x424167, and its row                                                     */
    FS_SOURCE,         /* 0x424189, reproduction's test of its source cell's slot A: an input, not
                          a draw; the tick's only, in a recording whose playing keeps the
                          engine's slots: a multiplayer one, or a skirmish one that holds its
                          cells (RECL_CELLS)                                                      */
    FS_SITES
};
#define FS_RNG_SITES 6 /* the RNG calls the recorder redirects                                      */

/* A seat's result. The engine holds one result, the controlled player's (RT_GAME_END); the others
   are this rule: a seat that ends the game with no unit lost; when the controlled player won,
   every seat it is allied with (its +0x108 row) won, and itself, and every other seat lost; when
   it lost, it lost; anything else is unknown -- a quit, or a seat still alive in a game that did not end in the controlled player's
   victory. */
enum RecResult { REC_RESULT_UNKNOWN = 0, REC_RESULT_WON, REC_RESULT_LOST, REC_RESULT_NONE = 0xFF };
#define REC_END_BYTES (28u + 12u * REC_PLAYERS)
_Static_assert(REC_END_BYTES % 4 == 0, "an RK_END fills whole dwords (REC_RING_RESERVE)");
#define REC_ARCHIVES  64u        /* the archives a header names at most                        */

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
    REC_END_MAPPED,         /* the explored map is not the one the recording began with          */
    REC_END_UNFINISHED,     /* written by a later timeline's writer for the segment before it,
                               which had no end block: it ends at its last complete iteration
                               (RT_LAST)                                                        */
    REC_END_PRUNED,      /* a deleted ancestor ends at its last retained stream block */
    REC_END_FORKED       /* a live rewind continued from an earlier point */
};

/* A recording is REC_MAGIC, then one or more SEGMENTS. A segment is an RB_HEADER and the blocks
   after it; every segment after the first begins right after an RB_END, and its header carries
   RT_TIMELINE. So a reader that stops at the first RB_END reads the original, as it was written,
   whatever follows: a takeover's fork is a segment appended to the file it was played from
   (research/notes/tadr-port/demo-recorder-s4-takeover.md).
   A block is a RecBlock, then `packed` bytes: exactly one zstd frame whose content is its `raw`
   bytes, with no byte after it, or the raw bytes themselves when RBF_STORED is set -- except a
   packed sectioned stream block, which is a chunk of its run's frame, and a chained snapshot,
   whose frame needs the snapshot before it (THE RUN AND THE CHAIN below). A packed block has at
   least one byte each way; an RB_HEADER, an RB_END and an RB_TIMELINES are always stored, so a
   header reads without zstd. `crc_body` is the CRC-32 (zlib's crc32, crc32.c's
   Crc32_ComputeBuf) of those `packed` bytes and `crc_head` that of the block's first 28 bytes, so
   a block cut short, or a header torn, is known by one of them. A block is written whole or not
   at all: a file ends at its last complete block.
   An RB_STREAM with RBF_SECTIONS holds a sectioned container (below) in place of its records:
   `raw` is the container's bytes, which decode to the records. Its frames carry state from one
   sectioned block to the next, and restart -- RBF_FRAME_RESET -- at a segment's first stream
   block, at the first after a snapshot and at the first after a stream block of records (one the
   writer's codec declined, or whose check decode differed): the unit and pool frames from the
   units and the projectile pool the reader holds at the end of the iteration before it, the cells
   frame from the grid it holds there, the orders, events and state frames empty. So a stream
   block inside a run of sectioned blocks decodes only after its predecessors since the run's
   reset, and a reader starts at a snapshot or at a segment's start, never inside a run
   (tagpu_reccodec.c, THREE INVARIANTS). Any other flag bit, RBF_SECTIONS or RBF_REFRESH on
   another kind of block, RBF_FRAME_RESET without RBF_SECTIONS, a stored sectioned block, and
   RBF_CHAINED on a stored block or on any kind but RB_SNAPSHOT are refused.

   THE RUN AND THE CHAIN. A RUN is the sectioned stream blocks from an RBF_FRAME_RESET block to
   the block before the segment's next reset, stream block of plain records or snapshot: the
   blocks one state of the frames carries across. Blocks of other kinds between them (RB_EXACT,
   RB_CONTRIBUTIONS) neither end nor touch a run. Its packed bytes are one zstd frame, begun at the reset block --
   magic, a window of at most 2^REC_RUN_WINDOW_LOG, no content size, no dictionary, no checksum --
   and never ended; each block's bytes are the frame's next chunk, flushed at the block's end, so a
   block decodes only after every chunk of its run before it, through one decoder, as its frames
   do. A sectioned block is always packed: its bytes are part of the frame's history whatever
   their size. A CHAIN is a key snapshot -- packed alone or stored, without RBF_CHAINED -- and the
   snapshots after it in its segment, each RBF_CHAINED: one zstd frame whose prefix (raw content,
   no dictionary ID) is the records of the RB_SNAPSHOT before it in the segment. At most
   REC_CHAIN_MAX snapshots follow a key, so a seek unpacks at most REC_CHAIN_MAX + 1. The cost: a
   snapshot that cannot be read refuses every seek that starts from it or from a later link of
   its chain, up to the next key, and a seek does not fall back to an earlier snapshot. */
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
                          RK_PLAYER and an RK_CONTROLLER per player, an RK_VIEWER, for each
                          seat with a perspective its last two samples as RK_POV_AT, the
                          earlier first, then its RK_SELECT when it has had one, an
                          RK_TABLE_SIZE per declared table and an RK_TABLE_SPAN for each of its
                          spans that is not all zero (RK_TABLE_SIZE zeroes the table, which
                          starts empty), the RK_CELLS of every cell that differs from the
                          segment's first iteration (the base: a reader takes it from the first
                          iteration it reads, which every playing reads first), an RK_WRECK per
                          record that differs from the pool at the end of that same iteration
                          (the wreck base, taken beside the grid's; a record now zero included),
                          an RK_FEATURES, the RK_GLOBALS, the RK_PROJ_HEAD with an RK_PROJ per
                          record below its hw, and the explored map whole (RK_MAPPED). A
                          snapshot that carries more wreck records than differ reads the same.
                          A dump is one of these, stored, with every cell and no base -- its
                          RK_WRECK per record that is not all zero, read over a zero pool -- and
                          the players' sight counters (RK_SIGHT) and the two images whole
                          (RK_IMAGE) after the rest. A snapshot follows the stream block that ends
                          with the events of the step after `last`, so a reader that starts from
                          it takes those from the block before                                  */
    RB_END,            /* entries (RecTag): how the recording ended                              */
    RB_MINIMAP,        /* right after the header, when the map has a picture: u16 w, u16 h, u16
                          the box's columns, u16 its rows (i16 main+0x142EB, +0x142ED), then w*h
                          palette indices, rows of w: the TNT's minimap picture (the GAF frame at
                          main+0x1426B, uncompressed), whose part [0, 2*columns) x [0, 2*rows)
                          the engine shows. A reader that does not know it skips it            */
    RB_TIMELINES,      /* RecTimelineState rows (tagpu_recfile.h); the latest complete table wins */
    RB_CONTRIBUTIONS,  /* RecMpRecord metadata followed by its exact source envelope/payload;
                          packed consecutive spans, no ring padding (tagpu_recmp_file.h)        */
    RB_EXACT           /* a test launch's only (tagpu_rec.exact): the records of the stream block
                          before it as the capture made them, before the lossy filter, with its
                          `first` and `last`; never sectioned. A reader of the stream skips it   */
};
_Static_assert(RB_CONTRIBUTIONS == 7 && RB_EXACT == 8, "the block kinds retain their values");
#define REC_MINIMAP_HEAD  8u
#define REC_MINIMAP_MAX   (0xFF00u - REC_MINIMAP_HEAD)   /* the most indices: a ring record's */
#define RBF_STORED      1u
#define RBF_SECTIONS    2u     /* an RB_STREAM's only: its payload is a sectioned container    */
#define RBF_FRAME_RESET 4u     /* with RBF_SECTIONS only: the frames restart at its first
                                  iteration, the unit and pool frames from the units and the
                                  projectile pool the reader holds at the end of the iteration
                                  before it, the cells frame from the grid it holds there, the
                                  orders, events and state frames empty                       */
#define RBF_REFRESH     8u     /* an RB_STREAM's only, sectioned or not: its last iteration is a
                                  refresh, where the stream's world equals the writer's exact
                                  copy byte for byte (tagpu_recwriter.c, iteration_done)      */
#define RBF_CHAINED     16u    /* an RB_SNAPSHOT's only: packed against the RB_SNAPSHOT before it
                                  in its segment (THE RUN AND THE CHAIN)                       */
#define RBF_ALL         (RBF_STORED | RBF_SECTIONS | RBF_FRAME_RESET | RBF_REFRESH | RBF_CHAINED)
#define REC_RUN_WINDOW_LOG 20  /* a run's zstd window, 2^20 bytes at most                        */
#define REC_CHAIN_MAX      9   /* the chained snapshots a key may have after it                  */

/* THE SECTIONED CONTAINER (tagpu_reccodec.c reads and writes it): the bytes of an RB_STREAM with
   RBF_SECTIONS, which decode to exactly the records they replace. A RecSecHead, then RCS_COUNT
   sections of len[] bytes each, in order. RCS_KINDS holds the record kinds (RCS_KINDS below).
   RCS_OTHER holds, for each record of its kinds in payload order, a varint length (at most
   0xFFFF) and that many bytes: the record's body verbatim -- but for the RK_TICK of every
   iteration after the block's first, which has no entry: iteration i's GameTime is the first's
   + i (mod 2^32), and a block whose iterations are not consecutive GameTimes is written as
   records. Neither RK_UNIT_DELTA nor RK_PROJ_HEAD, RK_PROJ and RK_PROJ_DELTA has a
   body anywhere: the unit frame (RCS_UNITS) and the pool frame (RCS_PROJ) make them. The record
   frames -- RCS_CELLS, RCS_ORDERS, RCS_EVENTS and RCS_STATE -- each make the bodies of their
   kinds, in payload order, from a section of columns (tagpu_recframe.h, THE COLUMNS): a u8
   scheme, a varint column count (1..32), each column's varint length, then the columns, which
   fill the section; a reader takes only its own scheme and count, and consumes every column to
   its end. The payload is the records in kind order, each a RecHdr, its body and zero padding to
   a dword, as the writer's stream holds them; its first record is an RK_TICK, and an iteration is
   an RK_TICK and the records up to the next one.

   RCS_KINDS, the kinds, is per iteration its PATTERN, by reference:
       u8      m                          1..RECC_KINDS_MRU: the m-th of the run's last patterns,
                                          front first, which moves to the front; 0: a new one,
       varint  length, bytes              after m = 0 only: the pattern (1..RECC_KINDS_MAX bytes),
                                          which goes to the front, the RECC_KINDS_MRU + 1-th
                                          dropped
   The list empties at every RBF_FRAME_RESET. A pattern is the iteration's records as tokens, in
   payload order, which consume it exactly: a varint kind (1..0xFFFE) and a varint count (at
   least 1) for a run of records of that kind, or one of two MARKERS, which have no count:
       0       an empty feature tick: an RK_EV_FEATURE and its RK_EV_FEATURE_END whose bodies are
               the run's last explicit empty pair's -- an FR_TICK root of 8 bytes directly followed
               by an end whose root is FR_TICK; none before the first such pair since the reset
       0xFFFF  an RK_FEATURES whose body is the state frame's prediction (tagpu_recframe_state.c);
               none before the first explicit RK_FEATURES since the reset
   and the first run of each of three GROUPS in the pattern has no count either, its count being
   the group's total less the group's other runs': RK_TICK, total 1, the pattern's first token
   (so no other RK_TICK run); RK_UNIT_DELTA, total the unit frame's changed units of the
   iteration; RK_PROJ and RK_PROJ_DELTA together, total the pool frame's changed records of the
   iteration. The first run carries no count, not the last, so a reader knows from what it has
   read whether a count follows. A group with no run has a total of 0, and a reader refuses a
   group whose other runs hold its total or more (the first run's count would be below 1).

   RCS_UNITS, the unit frame, is
       varint  iterations                 RecSecHead.iterations again
       varint  S, then S bytes            the symbols, one per carried slot of every iteration in
                                          order, 2 bits each, four a byte from the low bits, the
                                          last byte's unused bits 0: 0 not listed, 1 listed with
                                          its last mask, 2 listed with another; 3 is refused
       per iteration:
         varint  r, then r gaps           the other listed candidates, as ranks among the
                                          candidates not carried in slot order: rank = previous
                                          + 1 + gap, previous starting at -1
         varint  w, then w gaps           the woken slots: slot = previous + 1 + gap, previous
                                          starting at 0
       varint  ndict                      the block's new lane masks, at most the ids
       bytes   dict[ndict][mb]            mb = ceil(lanes / 8); bit j says lane j has a residual
       u8      mode                       0: each id a u8, 1: each a varint
       ids     one per listed unit whose symbol is not 1, in slot order within each iteration:
               0 the slot's last mask (not for a carried slot, and only when that mask is not
               empty), k the k-th of the run's masks most recent first, the list's length + 1 the
               next new mask
       bytes   present[mb]                bit j: lane j has a count
       varint  count for each lane with its bit, in RT_CODEC's order, at least 1
       columns for each lane in RT_CODEC's order: its count of zigzag varint residuals, of the
               listed units whose mask has its bit, in listed order; the listed masks consume
               every column exactly
   An iteration's candidates are the slots live at its start that no RK_UNIT or RK_UNIT_GONE of
   it names and that do not rest; its woken slots rest and change. A candidate the iteration
   before listed is carried. Slot 0 is never a candidate nor woken, so a block's symbols and ids
   are each at most iterations * (slots - 1). The previous listed set, each slot's last mask and the run's masks
   (at most RECC_J2_MTF, the least recent dropped) restart empty at every RBF_FRAME_RESET; the
   lanes' counts come first so a reader places every column before the first iteration, whose
   listing it learns only by decoding (tagpu_reccodec.c, THE LISTING).
   RCS_PROJ, the pool frame, is
       varint  iterations                 RecSecHead.iterations again
       per iteration, in order:
         varint  zigzag(c1 - c0)          the pool's count after it, c0 the count before
         varint  h1 - h0                  its extent after it, h0 the extent before
         varint  nd, then nd gaps         the records of [0, c0) that died: slot = previous + 1 +
                                          gap, previous starting at -1
         varint  nt, then nt gaps         the stale slots [c1, h1) that changed: slot = previous +
                                          1 + gap, previous starting at c1 - 1
       varint  codes                      per iteration, a reference code for each birth, then
                                          for each tail slot: 0 the zero row, else 1 +
                                          (zigzag(row - anchor) << 2 | section)
       columns the zigzag residuals, a column each with every iteration's survivors in order:
               the survivors' 21 lanes of bytes 0x34..0x6A, each zigzagged at its field's width
               (tagpu_reccodec.c, THE SURVIVOR LANES), then their dwords 7, 8, 9, 0 .. 6, 10, 11,
               12, 32-bit; then the births' and the tail's, dwords 0 .. 26, 32-bit. Byte 0x6B of a
               survivor is its last record's.
   The survivors are the records of [0, c0) that did not die, in order, and become [0, c0 - nd);
   the births follow them up to c1. Every count, slot and reference is bounded by RT_PROJECTILES,
   the iterations and the section's bytes; an iteration's projectile records are exactly its
   changed slots below h1, ascending, and at most one RK_PROJ_HEAD, anywhere among them, which it
   carries whenever the count or the extent changes. A block with no projectile costs 1 + 4 x iterations bytes.
   A varint is LEB128 of a value below 2^32, at most 5 bytes; zigzag maps r >= 0 to 2r and r < 0
   to -2r - 1 (a 32-bit residual r >= 2^31 to 2 (2^32 - r) - 1). tagpu_reccodec.c says what a
   residual is the residual of, and what a reference row and its sections are; each record
   frame's file (tagpu_recframe_*.c) says what its columns hold. */
enum RecSection {
    RCS_KINDS = 0,     /* the record kinds, as per-iteration patterns                             */
    RCS_UNITS,         /* the unit frame: every RK_UNIT_DELTA                                    */
    RCS_PROJ,          /* the pool frame: every RK_PROJ_HEAD, RK_PROJ and RK_PROJ_DELTA          */
    RCS_CELLS,         /* the cells frame: every RK_CELLS                                        */
    RCS_ORDERS,        /* the orders frame: every RK_ORDERS                                      */
    RCS_EVENTS,        /* the events frame: RK_EV_CREATE..RK_EV_DEATH_END, RK_EV_NET_CREATE,
                          RK_EV_SLICE, RK_EV_QUERY and RK_EV_YARD                                */
    RCS_OTHER,         /* the bodies of every other kind, verbatim                               */
    RCS_STATE,         /* the state frame: RK_GLOBALS, RK_GLOBALS_DELTA, RK_PLAYER,
                          RK_PLAYER_DELTA, RK_WRECK and RK_FEATURES                              */
    RCS_COUNT
};
typedef struct RecSecHead {
    uint32_t payload;          /* the records' bytes it decodes to: a multiple of 4, at least 4 */
    uint16_t iterations;       /* its RK_TICK records, at least 1                                */
    uint16_t sections;         /* RCS_COUNT                                                      */
    uint32_t len[RCS_COUNT];   /* each section's bytes; the head and these fill the block        */
} RecSecHead;
_Static_assert(sizeof(RecSecHead) == 40, "a sectioned container's head is 40 bytes");

/* A record kind's section: five rules and a default, so a kind added later falls into RCS_OTHER
   on the writer's side and the reader's alike, with no table to update. */
static __inline unsigned rec_section_of(unsigned kind)
{
    if (kind == RK_UNIT_DELTA) return RCS_UNITS;
    if (kind == RK_PROJ_HEAD || kind == RK_PROJ || kind == RK_PROJ_DELTA) return RCS_PROJ;
    if (kind == RK_CELLS) return RCS_CELLS;
    if (kind == RK_ORDERS) return RCS_ORDERS;
    if ((kind >= RK_EV_CREATE && kind <= RK_EV_DEATH_END) || kind == RK_EV_NET_CREATE ||
        kind == RK_EV_SLICE || kind == RK_EV_QUERY || kind == RK_EV_YARD)
        return RCS_EVENTS;
    if (kind == RK_GLOBALS || kind == RK_GLOBALS_DELTA || kind == RK_PLAYER ||
        kind == RK_PLAYER_DELTA || kind == RK_WRECK || kind == RK_FEATURES)
        return RCS_STATE;
    return RCS_OTHER;
}
_Static_assert(RK_EV_CREATE == 19 && RK_EV_DEATH_END == 32, "the event kinds RCS_EVENTS holds");

/* RT_AIR's row: an aircraft type's constants for the unit frame's air rules. `type` is the unit
   image's +0xA6, the engine's type index; the rest are the def's (+0x170 the sight emit's height
   term, a byte; +0x192 maxvelocity, +0x19A brakerate, +0x19E acceleration, +0x1A2 bankscale,
   +0x1A6 pitchscale, 16.16). */
typedef struct RecAirRow {
    uint16_t type;
    uint8_t  sight_h, zero;
    int32_t  maxv, brake, acc, bank, pitch;
} RecAirRow;
_Static_assert(sizeof(RecAirRow) == 24, "an RT_AIR row is 24 bytes");

/* RT_TYPES' row: a unit type's constants for the unit frame's per-type rules. `type` is the unit
   image's +0xA6; the rest are the def's (+0x170 the sight emit's height term, a byte; +0x1FA the
   hit points at most, u32; +0x192 maxvelocity, +0x19A brakerate, +0x19E acceleration, 16.16). */
#define REC_SLOPES 11            /* the ground speed's slope table: 0x505200, read at 0x505205 + sl */
typedef struct RecTypeRow {
    uint16_t type;
    uint8_t  sight_h, zero;
    uint32_t maxhp;
    int32_t  maxv, brake, acc;
} RecTypeRow;
_Static_assert(sizeof(RecTypeRow) == 20, "an RT_TYPES row is 20 bytes");
#define REC_TYPES_HEAD 16        /* RT_TYPES' u32 n, its slope table and its pad                 */

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
    RT_GAME_VERSION,   /* string: the main menu's version text (0x5031B4, at most 7 characters:
                          the stock "v3.1", or what a mod's patch wrote there), printable ASCII
                          only; absent when it is not                                             */
    RT_ARCHIVE,        /* u32 size, u32 mtime low, u32 mtime high (FILETIME, UTC), then the file
                          name without its folder: an archive the engine mounted, in its mount
                          order (the list at [0x51FBD0]+0x618), at most REC_ARCHIVES of them    */
    RT_MODPRINT,       /* u32 the mod's fingerprint, tagpu_rec_mod_print: FNV-1a over each mounted
                          archive's lowercased name, NUL and size, Impure's own impure-patch.ufo
                          left out, so two installs of one mod agree whatever their files' times;
                          u32 the archives the engine had mounted, which RT_ARCHIVE lists up to
                          REC_ARCHIVES of. A map archive changes it: whether a recording can be
                          played is RT_UNITDEFS's and RT_WEAPONS's to say                         */
    RT_TIMELINE,       /* u32 the timeline's id (1, 2, ...; never reused within a file), u32 its
                          parent's id (0 the original), u32 fork_at: a fork's header only. The
                          segment's first iteration is fork_at, and along a chain of timelines
                          it replaces the parent's iterations from fork_at on                   */
    RT_MULTIPLAYER,    /* RecMpRecordHeader: copied match/origin roster, required in mode 3      */
    RT_MP_SEATS,       /* RecMpRoster: origin/recorder-seat/fixed-unit-block mapping in mode 3   */
    RT_CODEC,          /* u16 n, u16 0, then n rows of RecLaneRow (u16 offset, u8 width, u8 role):
                          the unit frame's lanes (replay-lanes.tsv), tiling the unit image        */
    RT_PHYSICS,        /* i32 gravity (main+0x14263), u8 sea level (main+0x1427F), u8 the sight
                          rule (LosType main+0x14281 bit 2 at the segment's start, 0 or 1), u16 0,
                          u32 the sort buckets' columns (main+0x142A3; 0 turns the bucket rule
                          off): the unit frame's level constants                                 */
    RT_AIR,            /* u32 n, u32 0, then n RecAirRow, types strictly ascending: the aircraft
                          types (def +0x241 bit 11) the unit frame predicts with its air rules    */
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
    RT_UNOBSERVED,     /* u32: grid cells the rotating sweep found changed that no writer's
                          observer had marked: recorded when found, late                         */
    RT_GAME_END,       /* u32: main+0x3923B's bits 2, 4, 5 and 6 at the teardown, the result of
                          the controlled player: bit 4 won, bit 6 lost, bit 2 without them a quit;
                          0 when the recording ended before the level did                        */
    RT_SEAT_END,       /* u8 seat, u8 result (RecResult), u16 0, u32 the GameTime its last
                          commander died, u32 the GameTime its last unit died when it ended the
                          game with none (its live count +0x144 at 0), 0 for none: a seat of the
                          recording                                                              */
    RT_MP_END,         /* RecMpRecordEnd: persisted source proofs cover these application cuts  */
    RT_CODEC_DECLINED, /* u32: stream blocks the codec's encoder declined, written as records     */
    RT_CODEC_MISMATCH, /* u32: stream blocks whose check decode differed, written as records: a
                          codec bug                                                              */
    RT_HOLD,           /* a skirmish header's, written by the writer (tagpu_recwriter.c,
                          header_apply); absent in a multiplayer one, whose stream holds nothing:
                          u32 the refresh interval in ticks (REC_REFRESH_MIN..REC_REFRESH_TICKS),
                          u32 flags (RECL_*), the SHA-256 of replay-hold.tsv (32 bytes), then the
                          hold masks: REC_UNIT_IMAGE bytes for a unit image and REC_ORDER_IMAGE
                          for a node of a main list (+0x5C), a set bit held. Every byte a mask
                          leaves clear is exact at every iteration; a held byte is exact at every
                          refresh and keeps, between two, a value the stream wrote for the same
                          unit or list since the last (tagpu_reclossy.c, THE INVARIANTS)        */
    RT_TYPES           /* u32 n, u8 the ground speed's slope table (11 bytes, the executable's at
                          0x505200), u8 0, then n RecTypeRow, types strictly ascending and below
                          RT_UNITDEFS' count: the unit types the unit frame's per-type rules
                          predict; a type without a row has those rules off                      */
};
/* The end block's tags are RT_END_REASON..RT_CODEC_MISMATCH; a header's are the others. */
_Static_assert(RT_AIR < RT_END_REASON && RT_HOLD > RT_CODEC_MISMATCH,
               "the end block's tags are one run");
_Static_assert(RT_CODEC == 29 && RT_AIR == 31 && RT_CODEC_DECLINED == 44 && RT_HOLD == 46 &&
               RT_TYPES == 47, "the codec's and the filter's tags");

/* RT_HOLD. A refresh iteration is a segment's first or second, one whose GameTime is 1 modulo
   the interval (the first after it where an iteration skips that GameTime), and every snapshot's
   (tagpu_reclossy.h, reclossy_is_refresh): on the grid, every refresh is also 1 modulo the
   radar pass's step of 30 ticks, and the grid is the same in every recording. A file without
   RT_HOLD, a multiplayer recording, is written on REC_REFRESH_TICKS's grid. */
#define REC_REFRESH_TICKS 300   /* the interval a recording is written with                     */
#define REC_REFRESH_MIN   30    /* the least a test launch may ask for (tagpu_rec.refresh)       */
#define RECL_CELLS        1u    /* the cells' unit slots and yard bit are held: a playing keeps
                                   the engine's own (tagpu_puppet.c, THE FOOTPRINT WRITERS)      */
#define RECL_SPRAYS       2u    /* the nano sprays are written as sessions (RK_SPRAY_SESSION)    */
#define REC_SPRAY_BODY    44u   /* an RK_EV_SPRAY after its unit ID                              */
#define REC_SPRAY_FOLLOW  8u    /* RK_SPRAY_SESSION's mode: the point follows the unit           */
/* the flags every skirmish recording's RT_HOLD carries: the writer's (tagpu_recwriter.c), and
   the capture records FS_SOURCE by them (tagpu_recorder.c, feat_source) */
#define REC_SKIRMISH_HOLDS (RECL_CELLS | RECL_SPRAYS)
#define REC_HOLD_BYTES    (8u + 32u + REC_UNIT_IMAGE + REC_ORDER_IMAGE)
/* the position +0x6A..+0x75, which a spray session that follows its unit reads: never held */
#define REC_HOLD_POS_AT   0x6A
#define REC_HOLD_POS_END  0x76
_Static_assert(REC_HOLD_BYTES == 456, "RT_HOLD's bytes");

#endif
