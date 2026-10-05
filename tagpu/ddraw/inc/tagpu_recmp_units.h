#ifndef TAGPU_RECMP_UNITS_H
#define TAGPU_RECMP_UNITS_H

#include "tagpu_recmp_players.h"

#pragma pack(push, 1)
typedef struct {
    uint16_t slot;
    uint8_t live, full;
    uint32_t birth;
    uint8_t bits[REC_UNIT_BITS], reserved;
} RecMpUnitDelta;
#pragma pack(pop)
_Static_assert(sizeof(RecMpUnitDelta)==20, "owner unit delta prefix span");
#define RECMP_UNIT_MAX ((RECMP_BODY_MAX-4)/sizeof(RecMpUnitDelta))

typedef struct {
    unsigned valid, live, origin, birth, source_tick, sequence, applied_tick;
    GUID stream;
    uint32_t image[REC_UNIT_DWORDS];
} RecMpUnitState;
typedef struct RecMpUnits RecMpUnits;

/* Copied images only. The frozen global block, owner ID and owner row must
   name origin. Normalized player IDs use row+1; NULL stays zero. Slot zero is
   outside every owner block, so its engine-only 255 sentinel is never accepted.
   Refusal leaves out unchanged; in-place mapping is supported. */
int tagpu_recmp_unit_map(const RecMpRoster* source, const RecMpRoster* destination,
                         unsigned origin, unsigned slot, const void* image, unsigned bytes,
                         void* out, unsigned capacity);

/* One game-thread owner; copied authority, no engine memory or state. Each slot
   starts unknown. A complete tombstone establishes absence; it is distinct from
   an unreported slot. Raw source images remain the delta baselines. */
RecMpUnits* tagpu_recmp_units_begin(const RecMpRoster* destination);
void tagpu_recmp_units_end(RecMpUnits* units);
/* Local encoder in destination coordinates. Returns an entry to append after a
   packet's u32 count: 0 unchanged, -1 invalid/no room, positive entry bytes.
   Encoding leaves the baseline unchanged; commit only the accepted envelope. */
int tagpu_recmp_units_delta(const RecMpUnits* units, unsigned origin, unsigned slot,
                            unsigned birth, unsigned live, const void* image, unsigned bytes,
                            void* out, unsigned capacity);
/* One bounded packet: u32 count, then count variable-length entries. Every
   distinct slot and reconstructed image is validated before any publication.
   No allocation or failure lies between validation and the copied-state writes. */
int tagpu_recmp_units_put(RecMpUnits* units, const RecMpSeatMaps* maps,
                          const RecMpHeader* header, const void* body, unsigned bytes,
                          unsigned applied_tick);
/* Optional mapping of the copied raw state into destination coordinates.
   Unknown is zero; found is one. Refusal leaves out unchanged. */
int tagpu_recmp_units_get(const RecMpUnits* units, const RecMpSeatMaps* maps,
                          unsigned slot, RecMpUnitState* out);

#endif
