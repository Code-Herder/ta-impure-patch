#ifndef IMPAI_SEATS_H
#define IMPAI_SEATS_H
/* impai_seats.h -- which player records ImpAI plays on a live level: the level init's choice
   (tagpu_impai.c, tagpu_impai_level_init) as a function of plain values, so it has a test of its own
   (tests/impai_seats_test.py), and the battle room's AI names that carry a network game's choice
   (tagpu_skirmish.c, THE BATTLE ROOM). research/notes/modules/impai.md, The seat table.

   THE RULE. ImpAI marks only a record the engine itself runs an AI for on this peer: seated, controller 2,
   with the AI object at +0x74, because the players phase reaches the seam 0x465031 only for a record whose
   +0x74 is set (0x46502A..0x465031). In a skirmish every such record is one the `seatai` mask names. In a
   network game the AI seats this peer added are type 2 with their AI object here and type 3 without one on
   every other peer (engine/network.md, A computer seat in a network game), so marking only local type-2
   records with the object means ImpAI can never play a seat another peer runs; which of them it plays is
   the record's own name: one that begins IMPAI_NAME_PREFIX. A type-3 record on a level that is not a
   network game is a state this rule does not know, and no seat is marked. No file of this half includes
   the engine header; the caller reads the records.

   WHY THE NAME AND NOT A TABLE BY ROW. The battle room's update compacts its rows: it moves a seated record
   into an earlier free row by swapping the two whole 0x14B-byte records and freeing the later one
   (0x44A889..0x44A8C6, DISASSEMBLED), so a choice kept beside the records by row index would follow the
   wrong player after a compaction. The long name +0x2B is part of the record and moves
   with it; the add names it (0x451220) and DirectPlay carries it to every other peer (the join's copy and
   the system case 0x103), so every peer reads the same AI from it. */
#include <stddef.h>
#include <stdint.h>

#define IMPAI_SEAT_RECS 10              /* the engine's ten player records, main+0x1B63, stride 0x14B */
#define IMPAI_NAME_FIELD 30             /* +0x2B, the long name, 30 bytes (the renames' bounded copies)  */
#define IMPAI_NAME_CUT 16               /* 0x451220's store at 0x45131E: an AI's name is cut to 16      */
#define IMPAI_NAME_PREFIX "ImpAI:"      /* an ImpAI seat's name; an OTA AI seat's is the engine's AI:%s  */
#define OTA_NAME_PREFIX "AI:"           /* the engine's format 0x5064B4, "AI:%s"                          */

/* What the level init reads of one record, as plain values. */
typedef struct IMPAI_SEAT_REC {
    uint8_t active;     /* [rec] != 0: the record is seated                                          */
    uint8_t control;    /* +0x73: 1 human, 2 AI, 3 remote (or a replay puppet)                       */
    uint8_t ai_object;  /* +0x74 != 0: the AI object the players phase calls the AI tick with        */
    uint8_t info_ai;    /* PlayerInfo +0x94 == 2: the engine's "AI" type, which a type-3 record of a
                           remote AI seat takes from its owner's 0x20 message (0x45490F..0x45493B)   */
    uint8_t named;      /* the long name +0x2B begins IMPAI_NAME_PREFIX (impai_seats_named)          */
} IMPAI_SEAT_REC;

typedef struct IMPAI_SEAT_PICK {
    unsigned marks;        /* bit i: ImpAI plays record i                                            */
    unsigned remote_ai;    /* bit i: record i is an AI seat another peer runs (type 3, info_ai)       */
    unsigned remote_impai; /* bit i: ... and its name says that peer's ImpAI plays it                */
    unsigned live;         /* the seated records                                                     */
    int      refused;      /* the first type-3 record of a level that is not a network game, else -1  */
} IMPAI_SEAT_PICK;

/* The marks for one live level. `network`: the level is a network game (main+0x2A44 bit 0). `skirmish`: it is
   a skirmish (GameingState's mode 2, 0x435100). `seatai`: bit i, a skirmish's record i plays ImpAI; a network
   game reads each record's `named` instead, and any other level -- a campaign mission -- marks none. Bits past
   the ten records are ignored. */
void impai_seats_pick(const IMPAI_SEAT_REC rec[IMPAI_SEAT_RECS], int network, int skirmish, unsigned seatai,
                      IMPAI_SEAT_PICK* out);

/* 1 when the name field `name` (at most `cap` bytes, not necessarily terminated) begins IMPAI_NAME_PREFIX. */
int impai_seats_named(const char* name, size_t cap);

/* An AI seat's name as the add makes it: the prefix, ImpAI's or the engine's, then the controlling human's
   long name `human` (at most `cap` bytes, not necessarily terminated), cut to IMPAI_NAME_CUT characters as
   0x45131E cuts it. `out` holds IMPAI_NAME_CUT + 1 bytes. */
void impai_seats_ai_name(char* out, int impai, const char* human, size_t cap);

/* THE LEVEL a level's ImpAI seats play, 0 Easy, 1 Normal, 2 Hard (impai.h, IMPAI_LV_*), from the engine's
   Difficulty main+0x37EEE (`difficulty`, as read) and `network` (main+0x2A44 bit 0). A skirmish's is its
   Difficulty, the level its player chose, and -1 for a value past 2, which the military's init plays as
   Normal and names (tagpu_impai.c, impai_mil_init). A network game's is Hard on every peer whatever its Difficulty: the
   host's Start stores 2 (0x448A0B) and nothing writes a joiner's on its way in, which keeps whatever its
   last screen, saved game or registry left, so only Hard is a level the game set and the same on every
   peer (DISASSEMBLED: engine/sim.md, Difficulty and the AI handicap). */
#define IMPAI_LEVEL_HARD 2
int impai_seats_level(int network, uint32_t difficulty);

/* "1,3", or "none" when no bit of `mask` below the ten records is set: the level init's list. `out` holds at
   least 2 * IMPAI_SEAT_RECS bytes. */
void impai_seats_list(unsigned mask, char* out);

#endif
