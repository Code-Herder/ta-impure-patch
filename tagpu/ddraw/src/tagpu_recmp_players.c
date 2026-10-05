#include "tagpu_recmp_players.h"
#include <string.h>

struct RecMpPlayers {
    RecMpRoster roster;
    RecMpPlayerState state[RECMP_ACTORS];
};

static unsigned word(const void* image, unsigned offset)
{
    unsigned value;
    memcpy(&value,(const unsigned char*)image+offset,4);
    return value;
}

int tagpu_recmp_roster_valid(const RecMpRoster* r)
{
    unsigned i, j;
    if (!r || r->version!=1 || !r->count || r->count>RECMP_ACTORS || !r->each ||
        r->each>6553 || r->slots!=r->each*RECMP_ACTORS+1) return 0;
    for (i=0;i<RECMP_ACTORS;i++) {
        const RecMpSeat* a=&r->actor[i];
        if (i>=r->count) {
            if (a->origin || a->seat || a->first || a->last) return 0;
            continue;
        }
        if (!a->origin || a->seat>=RECMP_ACTORS || !a->first || a->first>=r->slots ||
            a->last<a->first || a->last>=r->slots || a->last-a->first+1!=r->each ||
            (a->first-1)%r->each) return 0;
        for (j=0;j<i;j++) if (r->actor[j].origin==a->origin || r->actor[j].seat==a->seat ||
                               r->actor[j].first==a->first) return 0;
    }
    return 1;
}

int tagpu_recmp_seat_map(const RecMpRoster* source, const RecMpRoster* destination,
                         uint8_t out[RECMP_ACTORS+1])
{
    uint8_t map[RECMP_ACTORS+1];
    unsigned i, j, used=0;
    if (!out || !tagpu_recmp_roster_valid(source) || !tagpu_recmp_roster_valid(destination) ||
        source->count!=destination->count || source->slots!=destination->slots ||
        source->each!=destination->each) return 0;
    memset(map,RECMP_ACTORS+1,sizeof map);
    for (i=0;i<source->count;i++) {
        const RecMpSeat* a=&source->actor[i];
        for (j=0;j<destination->count;j++) if (destination->actor[j].origin==a->origin) break;
        if (j==destination->count || a->first!=destination->actor[j].first ||
            a->last!=destination->actor[j].last) return 0;
        map[a->seat]=(uint8_t)destination->actor[j].seat;
        used|=1u<<map[a->seat];
    }
    /* The validated unique active rows and this disjoint vacant-row pairing
       form a bijection. Even unused vector entries survive an inverse mapping. */
    for (i=0,j=0;i<RECMP_ACTORS;i++) if (map[i]>RECMP_ACTORS) {
        while (j<RECMP_ACTORS && (used>>j&1)) j++;
        if (j==RECMP_ACTORS) return 0;
        map[i]=(uint8_t)j; used|=1u<<j;
    }
    map[RECMP_ACTORS]=RECMP_ACTORS;
    memcpy(out,map,sizeof map);
    return 1;
}

int tagpu_recmp_player_map(const RecMpRoster* source, const RecMpRoster* destination,
                           unsigned origin, const void* image, unsigned bytes,
                           void* out, unsigned capacity)
{
    static const unsigned vector[]={0x108,0x113,0x11E,0x129,0x134};
    uint8_t map[RECMP_ACTORS+1], original[REC_PLAYER_IMAGE], mapped[REC_PLAYER_IMAGE];
    unsigned i, row;
    if (!image || !out || bytes!=REC_PLAYER_IMAGE || capacity<REC_PLAYER_IMAGE ||
        !tagpu_recmp_seat_map(source,destination,map)) return 0;
    for (i=0;i<source->count;i++) if (source->actor[i].origin==origin) break;
    if (i==source->count) return 0;
    row=source->actor[i].seat;
    memcpy(original,image,sizeof original);
    if (original[0x146]!=row || word(original,0x67)!=source->actor[i].first+1 ||
        word(original,0x6B)!=source->actor[i].last+1) return 0;
    memcpy(mapped,original,sizeof mapped);
    /* DISASSEMBLED: alliances/mirror are 11-byte rows; 0x45591D/0x455930 and
       0x45768A index the three statistics rows by sender. Player +0x146 is the
       row copied to unit +0xFF at 0x485908/0x48590E. See the engine map's records. */
    for (i=0;i<sizeof vector/sizeof *vector;i++)
        for (row=0;row<=RECMP_ACTORS;row++) mapped[vector[i]+map[row]]=original[vector[i]+row];
    mapped[0x146]=map[original[0x146]];
    memcpy(out,mapped,sizeof mapped);
    return 1;
}

static unsigned actor(const RecMpPlayers* p, unsigned origin)
{
    unsigned i;
    for (i=0;i<p->roster.count;i++) if (p->roster.actor[i].origin==origin) return i;
    return RECMP_ACTORS;
}

RecMpPlayers* tagpu_recmp_players_begin(const RecMpRoster* roster)
{
    RecMpPlayers* p;
    if (!tagpu_recmp_roster_valid(roster)) return NULL;
    p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *p);
    if (p) p->roster=*roster;
    return p;
}

void tagpu_recmp_players_end(RecMpPlayers* p) { if (p) HeapFree(GetProcessHeap(),0,p); }

int tagpu_recmp_players_delta(const RecMpPlayers* p, unsigned origin, unsigned controller,
                              const void* image, void* out, unsigned capacity)
{
    RecMpPlayerDelta delta={0};
    unsigned i, at, bytes=sizeof delta;
    const RecMpPlayerState* state;
    if (!p || !image || !out || controller<1 || controller>2 ||
        (at=actor(p,origin))==RECMP_ACTORS) return -1;
    state=&p->state[at]; delta.origin=origin; delta.controller=controller; delta.full=!state->valid;
    if (word(image,0x67)!=p->roster.actor[at].first+1 || word(image,0x6B)!=p->roster.actor[at].last+1)
        return -1;
    for (i=0;i<REC_PLAYER_DWORDS;i++) if (delta.full || state->image[i]!=word(image,i*4)) {
        delta.bits[i>>3] |= 1u<<(i&7); bytes+=4;
    }
    if (bytes==sizeof delta && state->controller==controller) return 0;
    if (capacity<bytes) return -1;
    memcpy(out,&delta,sizeof delta); bytes=sizeof delta;
    for (i=0;i<REC_PLAYER_DWORDS;i++) if (delta.bits[i>>3]>>(i&7)&1) {
        memcpy((unsigned char*)out+bytes,(const unsigned char*)image+i*4,4); bytes+=4;
    }
    return (int)bytes;
}

int tagpu_recmp_players_put(RecMpPlayers* p, const RecMpHeader* h,
                            const void* body, unsigned bytes, unsigned applied_tick)
{
    RecMpPlayerDelta delta;
    RecMpPlayerState next;
    GUID zero={0};
    unsigned i, at, offset=sizeof delta;
    if (!p || !h || !body || bytes<sizeof delta || bytes>sizeof delta+REC_PLAYER_IMAGE ||
        h->kind!=RECMP_PLAYER || !h->sequence || h->header!=sizeof *h ||
        h->bytes!=sizeof *h+bytes || !memcmp(&h->stream,&zero,sizeof zero)) return 0;
    memcpy(&delta,body,sizeof delta);
    if (delta.origin!=h->origin || delta.controller<1 || delta.controller>2 || delta.full>1 ||
        delta.reserved[0] || delta.reserved[1] || delta.reserved[2] ||
        (at=actor(p,h->origin))==RECMP_ACTORS) return 0;
    next=p->state[at];
    if ((!next.valid && !delta.full) || (next.valid &&
        (h->sequence<=next.sequence || h->tick<next.source_tick || applied_tick<next.applied_tick ||
         memcmp(&h->stream,&next.stream,sizeof h->stream)))) return 0;
    for (i=0;i<REC_PLAYER_BITS*8;i++) {
        unsigned bit=delta.bits[i>>3]>>(i&7)&1;
        if (i>=REC_PLAYER_DWORDS) { if (bit) return 0; continue; }
        if (delta.full && !bit) return 0;
        if (bit) {
            if (bytes-offset<4) return 0;
            memcpy(&next.image[i],(const unsigned char*)body+offset,4); offset+=4;
        }
    }
    if (offset!=bytes || word(next.image,0x67)!=p->roster.actor[at].first+1 ||
        word(next.image,0x6B)!=p->roster.actor[at].last+1) return 0;
    next.valid=1; next.controller=delta.controller; next.source_tick=h->tick;
    next.sequence=h->sequence; next.applied_tick=applied_tick;
    next.stream=h->stream;
    p->state[at]=next;
    return 1;
}

int tagpu_recmp_players_get(const RecMpPlayers* p, unsigned origin, RecMpPlayerState* out)
{
    unsigned at;
    if (!p || !out || (at=actor(p,origin))==RECMP_ACTORS || !p->state[at].valid) return 0;
    *out=p->state[at];
    return 1;
}
