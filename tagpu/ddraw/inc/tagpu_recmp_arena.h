#ifndef TAGPU_RECMP_ARENA_H
#define TAGPU_RECMP_ARENA_H

#include "tagpu_recmp_players.h"

/* Copied 32-bit pointer values only; no pointer is followed. row names the ten
   player records in row order, rank is the allocator's ten-record permutation.
   Active rows retain their recorded global blocks; vacant rows and ranks pair
   in ascending order. All inputs validate before rank changes. Refusal leaves
   rank unchanged. row and rank may overlap. The caller owns both spans. */
int tagpu_recmp_arena_order(const RecMpRoster* roster, const uint32_t row[RECMP_ACTORS],
                            uint32_t rank[RECMP_ACTORS]);

#endif
