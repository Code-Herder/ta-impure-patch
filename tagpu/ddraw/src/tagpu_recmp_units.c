#include "tagpu_recmp_units.h"
#include <string.h>

struct RecMpUnits {
    RecMpRoster destination;
    RecMpUnitState state[];
};

static unsigned word(const void* image, unsigned at)
{
    unsigned value;
    memcpy(&value,(const unsigned char*)image+at,4);
    return value;
}

static const RecMpSeat* owner(const RecMpRoster* roster, unsigned origin, unsigned slot)
{
    unsigned i;
    for (i=0;i<roster->count;i++) {
        const RecMpSeat* seat=&roster->actor[i];
        if (seat->origin==origin && slot>=seat->first && slot<=seat->last) return seat;
    }
    return NULL;
}

int tagpu_recmp_unit_map(const RecMpRoster* source, const RecMpRoster* destination,
                         unsigned origin, unsigned slot, const void* image, unsigned bytes,
                         void* out, unsigned capacity)
{
    uint8_t map[RECMP_ACTORS+1], original[REC_UNIT_IMAGE], mapped[REC_UNIT_IMAGE];
    const RecMpSeat* seat;
    unsigned store, value;
    if (!image || !out || bytes!=REC_UNIT_IMAGE || capacity<REC_UNIT_IMAGE ||
        !tagpu_recmp_seat_map(source,destination,map) || !(seat=owner(source,origin,slot))) return 0;
    memcpy(original,image,sizeof original);
    store=word(original,0xEC);
    if (word(original,0x96)!=seat->seat+1 || original[0xFF]!=seat->seat ||
        original[0xF4]>RECMP_ACTORS || store>RECMP_ACTORS+1) return 0;
    memcpy(mapped,original,sizeof mapped);
    /* DISASSEMBLED: array initialization fixes +0x96/+0xFF to the slot's owner;
       +0xEC is the resource object's player pointer. Create initializes attacker
       row +0xF4 to dummy ten; damage copies the attacker's owner row there.
       See the engine map's unit/player records for the writers and callers. */
    value=map[seat->seat]+1; memcpy(mapped+0x96,&value,4);
    value=store ? map[store-1]+1 : 0; memcpy(mapped+0xEC,&value,4);
    mapped[0xF4]=map[original[0xF4]]; mapped[0xFF]=map[original[0xFF]];
    memcpy(out,mapped,sizeof mapped);
    return 1;
}

RecMpUnits* tagpu_recmp_units_begin(const RecMpRoster* destination)
{
    RecMpUnits* c;
    if (!tagpu_recmp_roster_valid(destination)) return NULL;
    c=VirtualAlloc(NULL,sizeof *c+(size_t)destination->slots*sizeof *c->state,
                    MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (c) c->destination=*destination;
    return c;
}

void tagpu_recmp_units_end(RecMpUnits* c) { if (c) VirtualFree(c,0,MEM_RELEASE); }

int tagpu_recmp_units_delta(const RecMpUnits* c, unsigned origin, unsigned slot,
                            unsigned birth, unsigned live, const void* image, unsigned bytes,
                            void* out, unsigned capacity)
{
    RecMpUnitDelta entry={0};
    unsigned mapped[REC_UNIT_DWORDS], i, n=sizeof entry;
    const RecMpUnitState* previous;
    if (!c || !out || live>1 || (birth&0x80000000u) ||
        !owner(&c->destination,origin,slot) || (!live && (image || bytes)) ||
        (live && (!tagpu_recmp_unit_map(&c->destination,&c->destination,origin,slot,
                                       image,bytes,mapped,sizeof mapped) ||
                  !(word(mapped,0x110)&0x10000000u)))) return -1;
    previous=&c->state[slot]; entry.slot=slot; entry.birth=birth; entry.live=live;
    entry.full=!live || !previous->valid || !previous->live || previous->birth!=birth ||
               word(previous->image,0x92)!=word(mapped,0x92);
    if (live) for (i=0;i<REC_UNIT_DWORDS;i++) if (entry.full || previous->image[i]!=word(image,i*4)) {
        entry.bits[i>>3]|=1u<<(i&7); n+=4;
    }
    if (previous->valid && previous->live==live && previous->birth==birth && n==sizeof entry) return 0;
    if (capacity<n) return -1;
    memcpy(out,&entry,sizeof entry); n=sizeof entry;
    if (live) for (i=0;i<REC_UNIT_DWORDS;i++) if (entry.bits[i>>3]>>(i&7)&1) {
        memcpy((unsigned char*)out+n,(const unsigned char*)image+i*4,4); n+=4;
    }
    return (int)n;
}

static int decode(const RecMpUnits* c, const RecMpRoster* source, const RecMpHeader* h,
                   const unsigned char* raw, unsigned bytes, unsigned* at,
                   unsigned tick, RecMpUnitState* next, unsigned* slot)
{
    RecMpUnitDelta entry;
    unsigned i, mapped[REC_UNIT_DWORDS];
    const RecMpUnitState* previous;
    if (bytes-*at<sizeof entry) return 0;
    memcpy(&entry,raw+*at,sizeof entry); *at+=sizeof entry;
    if (!owner(source,h->origin,entry.slot) || entry.live>1 || entry.full>1 || entry.reserved ||
        (entry.birth&0x80000000u)) return 0;
    *slot=entry.slot; previous=&c->state[*slot]; *next=*previous;
    if (previous->valid && (previous->origin!=h->origin || h->sequence<=previous->sequence ||
        h->tick<previous->source_tick || tick<previous->applied_tick ||
        memcmp(&h->stream,&previous->stream,sizeof h->stream))) return 0;
    if (!entry.live) {
        if (!entry.full) return 0;
        memset(next->image,0,sizeof next->image);
    } else if (!entry.full && (!previous->valid || !previous->live || entry.birth!=previous->birth)) return 0;
    for (i=0;i<REC_UNIT_BITS*8;i++) {
        unsigned bit=entry.bits[i>>3]>>(i&7)&1;
        if (i>=REC_UNIT_DWORDS || !entry.live) { if (bit) return 0; continue; }
        if (entry.full && !bit) return 0;
        if (bit) {
            if (bytes-*at<4) return 0;
            memcpy(&next->image[i],raw+*at,4); *at+=4;
        }
    }
    if (entry.live && (!(word(next->image,0x110)&0x10000000u) ||
        (!entry.full && word(previous->image,0x92)!=word(next->image,0x92)) ||
        !tagpu_recmp_unit_map(source,&c->destination,h->origin,*slot,next->image,sizeof next->image,
                              mapped,sizeof mapped))) return 0;
    next->valid=1; next->live=entry.live; next->origin=h->origin; next->birth=entry.birth;
    next->source_tick=h->tick; next->sequence=h->sequence; next->applied_tick=tick; next->stream=h->stream;
    return 1;
}

int tagpu_recmp_units_put(RecMpUnits* c, const RecMpSeatMaps* maps, const RecMpHeader* h,
                          const void* body, unsigned bytes, unsigned tick)
{
    unsigned char raw[RECMP_BODY_MAX];
    RecMpUnitState next[RECMP_UNIT_MAX];
    unsigned slot[RECMP_UNIT_MAX], count, at=4, i, j;
    RecMpRoster source;
    uint8_t permutation[RECMP_ACTORS+1];
    GUID stream;
    if (!c || !h || !body || bytes<4 || bytes>sizeof raw || h->kind!=RECMP_UNITS ||
        h->version!=RECMP_VERSION || h->header!=sizeof *h || h->bytes!=sizeof *h+bytes ||
        !h->sequence || memcmp(h->magic,"IMPUREMP",8) ||
        !tagpu_recmp_seats_get(maps,h->origin,&source,&stream) ||
        memcmp(&stream,&h->stream,sizeof stream) ||
        !tagpu_recmp_seat_map(&source,&c->destination,permutation)) return 0;
    memcpy(raw,body,bytes); memcpy(&count,raw,4);
    if (!count || count>RECMP_UNIT_MAX) return 0;
    for (i=0;i<count;i++) {
        if (!decode(c,&source,h,raw,bytes,&at,tick,&next[i],&slot[i])) return 0;
        for (j=0;j<i;j++) if (slot[i]==slot[j]) return 0;
    }
    if (at!=bytes) return 0;
    /* The copied packet, disjoint bounded slots and game-thread ownership make
       this publication indivisible to callers. There is no failing operation here. */
    for (i=0;i<count;i++) c->state[slot[i]]=next[i];
    return 1;
}

int tagpu_recmp_units_get(const RecMpUnits* c, const RecMpSeatMaps* maps,
                          unsigned slot, RecMpUnitState* out)
{
    RecMpUnitState state;
    RecMpRoster source;
    uint8_t permutation[RECMP_ACTORS+1];
    GUID stream;
    if (!c || !out || !slot || slot>=c->destination.slots || !c->state[slot].valid) return 0;
    state=c->state[slot];
    if (!tagpu_recmp_seats_get(maps,state.origin,&source,&stream) ||
        memcmp(&state.stream,&stream,sizeof stream) ||
        !tagpu_recmp_seat_map(&source,&c->destination,permutation) ||
        (state.live && !tagpu_recmp_unit_map(&source,&c->destination,state.origin,slot,
                                            state.image,sizeof state.image,state.image,sizeof state.image))) return 0;
    *out=state;
    return 1;
}
