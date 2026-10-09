#include "tagpu_recmp_arena.h"
#include <string.h>

int tagpu_recmp_arena_order(const RecMpRoster* roster, const uint32_t row[RECMP_ACTORS],
                            uint32_t rank[RECMP_ACTORS])
{
    uint32_t rows[RECMP_ACTORS], ordered[RECMP_ACTORS];
    unsigned used=0, occupied=0, seen=0;
    if (!row || !rank || !tagpu_recmp_roster_valid(roster)) return 0;
    memcpy(rows,row,sizeof rows);
    for (unsigned i=0;i<RECMP_ACTORS;i++) {
        if (!rows[i]) return 0;
        for (unsigned j=0;j<i;j++) if (rows[j]==rows[i]) return 0;
    }
    for (unsigned i=0;i<RECMP_ACTORS;i++) {
        unsigned j;
        for (j=0;j<RECMP_ACTORS && rank[i]!=rows[j];j++) {}
        if (j==RECMP_ACTORS || (seen>>j&1)) return 0;
        seen|=1u<<j;
    }
    for (unsigned i=0;i<roster->count;i++) {
        const RecMpSeat* actor=&roster->actor[i];
        unsigned block=(actor->first-1)/roster->each;
        ordered[block]=rows[actor->seat];
        used|=1u<<actor->seat;
        occupied|=1u<<block;
    }
    /* Roster validation gives distinct active blocks and rows. Pairing their
       disjoint complements makes all ten blocks a bijection, including a
       recording with no vacant row. The engine initializes the paired fields. */
    for (unsigned block=0,seat=0;block<RECMP_ACTORS;block++) if (!(occupied>>block&1)) {
        while (used>>seat&1) seat++;
        ordered[block]=rows[seat]; used|=1u<<seat;
    }
    memcpy(rank,ordered,sizeof ordered);
    return 1;
}
