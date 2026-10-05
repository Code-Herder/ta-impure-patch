#ifndef TAGPU_RECMP_PLAYERS_H
#define TAGPU_RECMP_PLAYERS_H

#include "tagpu_recmp.h"
#include "tagpu_recfmt.h"

/* Recorded seats belong to the recorder's frozen roster. Unit blocks are the
   engine's inclusive global slot ranges, shared across peers, not local rows. */
#pragma pack(push, 1)
typedef struct { uint32_t origin, seat, first, last; } RecMpSeat;
typedef struct {
    uint32_t version, count, slots, each;
    RecMpSeat actor[RECMP_ACTORS];
} RecMpRoster;
typedef struct {
    uint32_t origin, controller, full;
    uint8_t bits[REC_PLAYER_BITS], reserved[3];
} RecMpPlayerDelta;
#pragma pack(pop)
_Static_assert(sizeof(RecMpRoster)==176, "recorded seat roster span");
_Static_assert(sizeof(RecMpPlayerDelta)==32, "owner player delta prefix span");

typedef struct {
    unsigned valid, controller, source_tick, sequence, applied_tick;
    GUID stream;
    uint32_t image[REC_PLAYER_DWORDS];
} RecMpPlayerState;
typedef struct RecMpPlayers RecMpPlayers;

/* Roster validation rejects aliased origins/seats/blocks, unbounded intervals
   and ranges not matching one of the engine's fixed each-sized slot blocks. */
int tagpu_recmp_roster_valid(const RecMpRoster* roster);
RecMpPlayers* tagpu_recmp_players_begin(const RecMpRoster* roster);
void tagpu_recmp_players_end(RecMpPlayers* players);
/* Encoder does not change the baseline; commit only with the accepted source
   envelope through put. 0 unchanged, -1 invalid/no room, positive payload bytes. */
int tagpu_recmp_players_delta(const RecMpPlayers* players, unsigned origin, unsigned controller,
                              const void* image, void* out, unsigned capacity);
/* Bounded complete delta only. Refusal leaves the previous image/provenance intact. */
int tagpu_recmp_players_put(RecMpPlayers* players, const RecMpHeader* header,
                            const void* body, unsigned bytes, unsigned applied_tick);
int tagpu_recmp_players_get(const RecMpPlayers* players, unsigned origin, RecMpPlayerState* out);

#endif
