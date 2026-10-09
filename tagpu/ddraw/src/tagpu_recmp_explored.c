#include "tagpu_recmp_explored.h"
#include <string.h>

typedef struct {
    RecMpExploreState state;
    unsigned active, full, end, sequence, tick, applied, lostype;
    GUID stream;
} ExploreActor;

struct RecMpExplored {
    RecMpRoster roster;
    unsigned cells, bytes, encoding;
    ExploreActor actor[RECMP_ACTORS];
    unsigned char data[];
};

unsigned tagpu_recmp_explored_cells(unsigned w, unsigned h)
{
    uint64_t product=(uint64_t)w*h;
    if (!w || !h || w>0xFFFF || h>0xFFFF || product>UINT32_MAX/2u || product%4) return 0;
    return (unsigned)(product/4);
}

static unsigned actor(const RecMpExplored* c, unsigned origin)
{
    unsigned i;
    for (i=0;i<c->roster.count;i++) if (c->roster.actor[i].origin==origin) return i;
    return RECMP_ACTORS;
}

static unsigned char* image(RecMpExplored* c, unsigned at, unsigned pending)
{
    return c->data+((size_t)at*2+pending)*c->bytes;
}

RecMpExplored* tagpu_recmp_explored_begin(const RecMpRoster* roster, unsigned w, unsigned h)
{
    unsigned cells=tagpu_recmp_explored_cells(w,h), bytes=(cells+7)/8;
    RecMpExplored* c;
    if (!cells || !tagpu_recmp_roster_valid(roster) ||
        bytes>(SIZE_MAX-sizeof *c)/(2u*roster->count+1u)) return NULL;
    c=VirtualAlloc(NULL,sizeof *c+(size_t)bytes*(2u*roster->count+1u),
                    MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (c) { c->roster=*roster; c->cells=cells; c->bytes=bytes; }
    return c;
}

void tagpu_recmp_explored_end(RecMpExplored* c) { if (c) VirtualFree(c,0,MEM_RELEASE); }

int tagpu_recmp_explored_put(RecMpExplored* c, const RecMpSeatMaps* maps,
                             const RecMpHeader* h, const void* body, unsigned bytes, unsigned tick)
{
    unsigned char raw[RECMP_BODY_MAX];
    RecMpExploreChunk chunk;
    RecMpRoster source;
    uint8_t permutation[RECMP_ACTORS+1];
    GUID stream;
    ExploreActor* a;
    unsigned at, begin, end, full;
    if (!c || !h || !body || bytes<sizeof chunk || bytes>sizeof raw ||
        h->kind!=RECMP_EXPLORED || h->version!=RECMP_VERSION ||
        h->header!=sizeof *h || h->bytes!=sizeof *h+bytes || !h->sequence ||
        memcmp(h->magic,"IMPUREMP",8) || (at=actor(c,h->origin))==RECMP_ACTORS ||
        !tagpu_recmp_seats_get(maps,h->origin,&source,&stream) ||
        memcmp(&stream,&h->stream,sizeof stream) ||
        !tagpu_recmp_seat_map(&source,&c->roster,permutation)) return 0;
    memcpy(raw,body,bytes); memcpy(&chunk,raw,sizeof chunk);
    begin=chunk.flags&RECMP_EXPLORE_BEGIN; end=chunk.flags&RECMP_EXPLORE_END;
    full=!!(chunk.flags&RECMP_EXPLORE_FULL); a=&c->actor[at];
    if (chunk.flags&~7u || chunk.reserved[0] || chunk.reserved[1] || chunk.reserved[2] ||
        chunk.total!=c->bytes || bytes!=sizeof chunk+chunk.count ||
        chunk.first>c->bytes || chunk.count>c->bytes-chunk.first ||
        (!chunk.count && (full || chunk.first || !begin || !end)) ||
        (a->state.valid && (h->sequence<=a->state.sequence || h->tick<a->state.source_tick ||
          tick<a->state.applied_tick || memcmp(&h->stream,&a->state.stream,sizeof stream)))) return 0;
    if (begin) {
        if (a->active || (!full && !a->state.valid) || (full && chunk.first)) return 0;
    } else if (!a->active || a->full!=full || a->sequence==UINT32_MAX ||
        h->sequence!=a->sequence+1u || h->tick!=a->tick || tick<a->applied ||
        chunk.lostype!=a->lostype || memcmp(&h->stream,&a->stream,sizeof stream) ||
        chunk.first<a->end || (full && chunk.first!=a->end)) return 0;
    if (end && full && chunk.first+chunk.count!=c->bytes) return 0;
    if (chunk.count && c->cells%8 && chunk.first+chunk.count==c->bytes &&
        raw[bytes-1] & (0xFFu << (c->cells%8))) return 0;
    /* The complete copied chunk is bounded before either buffer changes. No
       allocation or failing operation follows; only END publishes the image. */
    if (begin) {
        if (full) memset(image(c,at,1),0,c->bytes);
        else memcpy(image(c,at,1),image(c,at,0),c->bytes);
    }
    memcpy(image(c,at,1)+chunk.first,raw+sizeof chunk,chunk.count);
    a->active=!end; a->full=full; a->end=chunk.first+chunk.count;
    a->sequence=h->sequence; a->tick=h->tick; a->applied=tick;
    a->lostype=chunk.lostype; a->stream=h->stream;
    if (end) {
        memcpy(image(c,at,0),image(c,at,1),c->bytes);
        a->state=(RecMpExploreState){1,h->origin,h->tick,h->sequence,tick,c->cells,c->bytes,
                                      chunk.lostype,h->stream};
    }
    return 1;
}

int tagpu_recmp_explored_idle(const RecMpExplored* c, unsigned origin)
{
    unsigned at;
    return c && (at=actor(c,origin))!=RECMP_ACTORS && !c->actor[at].active;
}

int tagpu_recmp_explored_get(const RecMpExplored* c, unsigned origin,
                             RecMpExploreState* state, void* out, unsigned capacity)
{
    unsigned at;
    if (!c || !state || (at=actor(c,origin))==RECMP_ACTORS || !c->actor[at].state.valid ||
        (out && capacity<c->bytes)) return 0;
    if (out) memcpy(out,c->data+(size_t)at*2*c->bytes,c->bytes);
    *state=c->actor[at].state;
    return 1;
}

int tagpu_recmp_explored_encode(RecMpExplored* c, unsigned origin,
                                const void* input, unsigned bytes, unsigned lostype,
                                RecMpExploreEmit emit, void* ctx)
{
    unsigned at, first=0, last, full, begun=0, ok=0;
    unsigned char packet[RECMP_BODY_MAX], *target, *previous;
    if (!c || !input || bytes!=c->bytes || lostype>0xFFFF || !emit || c->encoding ||
        (at=actor(c,origin))==RECMP_ACTORS || c->actor[at].active) return 0;
    target=c->data+(size_t)c->roster.count*2*c->bytes;
    memcpy(target,input,bytes);
    if (c->cells%8 && target[bytes-1] & (0xFFu << (c->cells%8))) return 0;
    previous=image(c,at,0); full=!c->actor[at].state.valid; last=bytes;
    if (!full) while (last && target[last-1]==previous[last-1]) last--;
    if (!last && c->actor[at].state.lostype==lostype) return 1;
    c->encoding=1;
    do {
        RecMpExploreChunk chunk={0};
        unsigned end;
        if (!full) while (first<last && target[first]==previous[first]) first++;
        end=full ? last : first;
        if (!full) {
            unsigned gap=0;
            while (end<last) {
                gap=target[end]==previous[end] ? gap+1 : 0;
                end++;
                if (gap==8) { end-=gap; break; }
            }
        }
        chunk.first=first; chunk.total=bytes; chunk.lostype=lostype;
        chunk.count=end-first>RECMP_EXPLORE_CHUNK ? RECMP_EXPLORE_CHUNK : end-first;
        chunk.flags=(!begun ? RECMP_EXPLORE_BEGIN : 0) | (full ? RECMP_EXPLORE_FULL : 0) |
                    (first+chunk.count==last ? RECMP_EXPLORE_END : 0);
        memcpy(packet,&chunk,sizeof chunk);
        memcpy(packet+sizeof chunk,target+first,chunk.count);
        if (!emit(ctx,origin,packet,sizeof chunk+chunk.count)) goto done;
        begun=1; first+=chunk.count;
    } while (first<last);
    ok=c->actor[at].state.valid && c->actor[at].state.lostype==lostype &&
       !c->actor[at].active && !memcmp(image(c,at,0),target,bytes);
done:
    c->encoding=0;
    return ok;
}
