#ifndef TAGPU_RECMP_EXPLORED_H
#define TAGPU_RECMP_EXPLORED_H

#include "tagpu_recmp_players.h"

enum { RECMP_EXPLORE_BEGIN=1, RECMP_EXPLORE_END=2, RECMP_EXPLORE_FULL=4 };
#pragma pack(push,1)
typedef struct {
    uint32_t first, total;
    uint16_t count, lostype;
    uint8_t flags, reserved[3];
} RecMpExploreChunk;
#pragma pack(pop)
_Static_assert(sizeof(RecMpExploreChunk)==16,"exploration chunk prefix span");
#define RECMP_EXPLORE_CHUNK (RECMP_BODY_MAX-sizeof(RecMpExploreChunk))

typedef struct {
    unsigned valid, origin, source_tick, sequence, applied_tick, cells, bytes, lostype;
    GUID stream;
} RecMpExploreState;
typedef struct RecMpExplored RecMpExplored;
typedef int (*RecMpExploreEmit)(void* ctx, unsigned origin, const void* body, unsigned bytes);

/* One bit per engine mapped-map u16, in its original linear order. Origin
   supplies the player identity, so seat permutations never alter this bitmap.
   Geometry must match LoadMap's whole-u16 allocation without 32-bit wrap. */
unsigned tagpu_recmp_explored_cells(unsigned width, unsigned height);
RecMpExplored* tagpu_recmp_explored_begin(const RecMpRoster* roster, unsigned width, unsigned height);
void tagpu_recmp_explored_end(RecMpExplored* explored);
/* One owner, copied buffers. BEGIN/END delimit a complete source-tick update;
   partial updates never replace published history. Seals require idle. */
int tagpu_recmp_explored_put(RecMpExplored* explored, const RecMpSeatMaps* maps,
                             const RecMpHeader* header, const void* body, unsigned bytes,
                             unsigned applied_tick);
int tagpu_recmp_explored_idle(const RecMpExplored* explored, unsigned origin);
/* Unknown returns zero. Refusal leaves caller outputs unchanged. A NULL image
   requests only metadata; otherwise capacity must hold the entire bitmap. */
int tagpu_recmp_explored_get(const RecMpExplored* explored, unsigned origin,
                             RecMpExploreState* state, void* image, unsigned capacity);
/* The input is copied before emitting. Only changed byte runs are emitted after
   the first full image. emit must publish each accepted envelope with put into
   this same cache before returning; an emission failure ends the producer. */
int tagpu_recmp_explored_encode(RecMpExplored* explored, unsigned origin,
                                const void* image, unsigned bytes, unsigned lostype,
                                RecMpExploreEmit emit, void* ctx);

#endif
