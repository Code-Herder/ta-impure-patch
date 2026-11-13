#ifndef IMPAI_SEATS_H
#define IMPAI_SEATS_H
/* impai_seats.h -- which player records ImpAI plays on a live level: the level init's choice
   (tagpu_impai.c, tagpu_impai_level_init) as a function of plain values, so it has a test of its own
   (tests/impai_seats_test.py). research/notes/modules/impai.md, The seat table.

   THE RULE. ImpAI marks only a record the engine itself runs an AI for on this peer: seated, controller 2,
   with the AI object at +0x74, because the players phase reaches the seam 0x465031 only for a record whose
   +0x74 is set (0x46502A..0x465031). In a skirmish every such record is one the `seatai` mask names. In a
   network game the AI seats this peer added are type 2 with their AI object here and type 3 without one on
   every other peer (engine/network.md, A computer seat in a network game), so marking only local type-2
   records with the object means ImpAI can never play a seat another peer runs; which of them it plays is
   the `netai` mask, by record. A type-3 record on a level that is not a network game is a state this rule
   does not know, and no seat is marked. No file of this half includes the engine header; the caller reads
   the records. */
#include <stdint.h>

#define IMPAI_SEAT_RECS 10              /* the engine's ten player records, main+0x1B63, stride 0x14B */

/* What the level init reads of one record, as plain values. */
typedef struct IMPAI_SEAT_REC {
    uint8_t active;     /* [rec] != 0: the record is seated                                          */
    uint8_t control;    /* +0x73: 1 human, 2 AI, 3 remote (or a replay puppet)                       */
    uint8_t ai_object;  /* +0x74 != 0: the AI object the players phase calls the AI tick with        */
    uint8_t info_ai;    /* PlayerInfo +0x94 == 2: the engine's "AI" type, which a type-3 record of a
                           remote AI seat takes from its owner's 0x20 message (0x45490F..0x45493B)   */
} IMPAI_SEAT_REC;

typedef struct IMPAI_SEAT_PICK {
    unsigned marks;     /* bit i: ImpAI plays record i                                               */
    unsigned remote_ai; /* bit i: record i is an AI seat another peer runs (type 3, info_ai)          */
    unsigned live;      /* the seated records                                                        */
    int      refused;   /* the first type-3 record of a level that is not a network game, else -1     */
} IMPAI_SEAT_PICK;

/* The marks for one live level. `network`: the level is a network game (main+0x2A44 bit 0). `seatai`:
   bit i, a skirmish's record i plays ImpAI. `netai`: bit i, a network game's record i, if this peer added
   it, plays ImpAI. Bits past the ten records are ignored. */
void impai_seats_pick(const IMPAI_SEAT_REC rec[IMPAI_SEAT_RECS], int network, unsigned seatai, unsigned netai,
                      IMPAI_SEAT_PICK* out);

/* THE LEVEL a level's ImpAI seats play, 0 Easy, 1 Medium, 2 Hard, from the engine's Difficulty
   main+0x37EEE (`difficulty`, as read) and `network` (main+0x2A44 bit 0). A skirmish's is its
   Difficulty, the level its player chose; a value past 2 is Hard, as the stock handicap reads it (0 x0.5,
   1 x0.7, anything else unscaled). A network game's is Hard on every peer whatever its Difficulty: the
   host's Start stores 2 (0x448A0B) and nothing writes a joiner's on its way in, which keeps whatever its
   last screen, saved game or registry left, so only Hard is a level the game set and the same on every
   peer (DISASSEMBLED: engine/sim.md, Difficulty and the AI handicap). */
#define IMPAI_LEVEL_HARD 2
unsigned impai_seats_level(int network, uint32_t difficulty);

/* "1,3", or "none" when no bit of `mask` below the ten records is set: the level init's list. `out` holds at
   least 2 * IMPAI_SEAT_RECS bytes. */
void impai_seats_list(unsigned mask, char* out);

#endif
