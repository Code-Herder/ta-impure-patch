#ifndef TAGPU_RECMP_NAMED_H
#define TAGPU_RECMP_NAMED_H

#include "tagpu_recmp_players.h"

/* One existing RK_EV_START_NAMED image follows this prefix. The source's
   incarnation and capture phase survive an arrival on another peer's clock.
   A birth is a monotonic slot token: repeated creates within one tick can put
   it ahead of GameTime (tagpu_patches.c:hit_next_birth). */
#pragma pack(push, 1)
typedef struct {
    uint16_t bytes;
    uint8_t before_capture, reserved;
    uint32_t birth;
} RecMpNamedEntry;
#pragma pack(pop)
_Static_assert(sizeof(RecMpNamedEntry)==8, "named root prefix");
#define RECMP_NAMED_MIN 25u
#define RECMP_NAMED_MAX (24u+REC_NAME_MAX)
#define RECMP_NAMED_COUNT ((RECMP_BODY_MAX-4u)/(sizeof(RecMpNamedEntry)+RECMP_NAMED_MIN))

typedef struct {
    unsigned valid, origin, source_tick, sequence, applied_tick, bytes, count;
    GUID stream;
    unsigned char body[RECMP_BODY_MAX];
} RecMpNamedState;
typedef struct RecMpNamed RecMpNamed;

/* Copied roots only: shared global unit IDs need no player-row mapping. A
   complete batch validates before publication. These last-batch queries support
   inspection; the file retains every batch in its ordered application ledger. */
RecMpNamed* tagpu_recmp_named_begin(const RecMpRoster* roster);
void tagpu_recmp_named_end(RecMpNamed* cache);
int tagpu_recmp_named_put(RecMpNamed* cache, const RecMpSeatMaps* maps,
                          const RecMpHeader* header, const void* body, unsigned bytes,
                          unsigned applied_tick);
int tagpu_recmp_named_get(const RecMpNamed* cache, unsigned origin, RecMpNamedState* out);

#endif
