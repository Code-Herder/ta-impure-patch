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
/* A bijection of all eleven engine rows. Active rows match by native origin and
   identical global unit blocks; vacant rows pair in ascending order and dummy
   row ten stays ten. Refusal leaves out unchanged. Actor array order is irrelevant. */
int tagpu_recmp_seat_map(const RecMpRoster* source, const RecMpRoster* destination,
                         uint8_t out[RECMP_ACTORS+1]);
/* Copied normalized images only: map the five player-indexed vectors and self
   row, retaining every other byte and validating the source row/block identity.
   No engine addresses or state are accessed. In-place mapping is supported. */
int tagpu_recmp_player_map(const RecMpRoster* source, const RecMpRoster* destination,
                           unsigned origin, const void* image, unsigned bytes,
                           void* out, unsigned capacity);
typedef struct RecMpSeatMaps RecMpSeatMaps;
/* One copied, immutable source roster per origin, advertised at source sequence
   one. Game-thread owned; a reader uses separate source and application maps. */
RecMpSeatMaps* tagpu_recmp_seats_begin(const RecMpRoster* destination);
void tagpu_recmp_seats_end(RecMpSeatMaps* maps);
int tagpu_recmp_seats_put(RecMpSeatMaps* maps, const RecMpHeader* header,
                          const void* body, unsigned bytes);
/* Optional copied outputs. Unknown sources stay unknown. */
int tagpu_recmp_seats_get(const RecMpSeatMaps* maps, unsigned origin,
                          RecMpRoster* source, GUID* stream);
int tagpu_recmp_seats_image(const RecMpSeatMaps* maps, unsigned origin, const GUID* stream,
                            const void* image, unsigned bytes, void* out, unsigned capacity);
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
