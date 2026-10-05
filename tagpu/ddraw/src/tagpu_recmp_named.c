#include "tagpu_recmp_named.h"
#include <string.h>

struct RecMpNamed {
    RecMpRoster roster;
    RecMpNamedState actor[RECMP_ACTORS];
};

RecMpNamed* tagpu_recmp_named_begin(const RecMpRoster* roster)
{
    RecMpNamed* c;
    if (!tagpu_recmp_roster_valid(roster)) return NULL;
    c=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *c);
    if (c) c->roster=*roster;
    return c;
}

void tagpu_recmp_named_end(RecMpNamed* c)
{
    if (c) HeapFree(GetProcessHeap(),0,c);
}

int tagpu_recmp_named_put(RecMpNamed* c, const RecMpSeatMaps* maps,
                          const RecMpHeader* h, const void* body, unsigned bytes,
                          unsigned applied_tick)
{
    RecMpNamedState next;
    RecMpRoster source;
    uint8_t permutation[11];
    const RecMpSeat* actor;
    const unsigned char* b;
    GUID stream, zero={0};
    unsigned count, at=4, i, row;
    if (!c || !h || !body || bytes<4 || bytes>RECMP_BODY_MAX ||
        h->kind!=RECMP_NAMED || h->version!=RECMP_VERSION ||
        h->header!=sizeof *h || h->bytes!=sizeof *h+bytes || !h->sequence ||
        memcmp(h->magic,"IMPUREMP",8) || !memcmp(&h->stream,&zero,sizeof zero) ||
        !tagpu_recmp_seats_get(maps,h->origin,&source,&stream) ||
        memcmp(&stream,&h->stream,sizeof stream) ||
        !tagpu_recmp_seat_map(&source,&c->roster,permutation)) return 0;
    for (row=0;row<c->roster.count;row++) if (c->roster.actor[row].origin==h->origin) break;
    if (row==c->roster.count) return 0;
    actor=&c->roster.actor[row];
    next=c->actor[row];
    if (next.valid && (h->sequence<=next.sequence || h->tick<next.source_tick ||
        applied_tick<next.applied_tick || memcmp(&h->stream,&next.stream,sizeof stream))) return 0;
    /* Copy the bounded span once. Caller aliases cannot change a later entry
       after an earlier entry has been validated. No publication precedes the
       last entry's validation and no allocation follows it. */
    memcpy(next.body,body,bytes);
    b=next.body;
    memcpy(&count,b,4);
    if (!count || count>RECMP_NAMED_COUNT) return 0;
    for (i=0;i<count;i++) {
        RecMpNamedEntry e;
        unsigned id, k;
        if (bytes-at<sizeof e) return 0;
        memcpy(&e,b+at,sizeof e); at+=sizeof e;
        if (e.bytes<RECMP_NAMED_MIN || e.bytes>RECMP_NAMED_MAX ||
            e.bytes>bytes-at || e.reserved || e.before_capture>1 ||
            e.birth&0x80000000u) return 0;
        memcpy(&id,b+at,4);
        if (id<actor->first+1 || id>actor->last+1 || b[at+4]>4 ||
            b[at+5]>1 || b[at+6] || b[at+7]) return 0;
        for (k=24;k<e.bytes;k++) if (!b[at+k]) return 0;
        at+=e.bytes;
    }
    if (at!=bytes) return 0;
    next.valid=1; next.origin=h->origin; next.source_tick=h->tick;
    next.sequence=h->sequence; next.applied_tick=applied_tick;
    next.stream=h->stream; next.bytes=bytes; next.count=count;
    c->actor[row]=next;
    return 1;
}

int tagpu_recmp_named_get(const RecMpNamed* c, unsigned origin, RecMpNamedState* out)
{
    unsigned i;
    if (!c || !out) return 0;
    for (i=0;i<c->roster.count;i++) if (c->roster.actor[i].origin==origin) break;
    if (i==c->roster.count || !c->actor[i].valid) return 0;
    *out=c->actor[i];
    return 1;
}
