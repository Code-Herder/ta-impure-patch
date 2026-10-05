#include "tagpu_recmp_roots.h"
#include <string.h>

struct RecMpRoots {
    RecMpRoster roster;
    RecMpRootsState actor[RECMP_ACTORS];
};

/* DISASSEMBLED: replies index 24 rows of 24 bytes at 0x5086D8 (0x47FAD0);
   spray's signed low layer word indexes ten 16-byte rows (0x47217C..0x472188),
   allocated as ten rows by 0x471D90. These bounds
   also protect the existing skirmish roots in tagpu_replay.c. */
int tagpu_recmp_root_image_valid(unsigned kind, const void* image, unsigned bytes)
{
    const unsigned char* b=image;
    unsigned value, at;
    if (!image || bytes<RECMP_ROOTS_MIN || bytes>RECMP_ROOTS_MAX) return 0;
    if (kind==RK_EV_CREATE || kind==RECMP_ROOT_CREATE_BEGIN) {
        uint16_t type;
        if (bytes!=28) return 0;
        memcpy(&type,b+4,2);
        return type && b[6]<REC_PLAYERS && b[7]<=3 && b[24]<=4 &&
               !b[25] && !b[26] && !b[27];
    } else if (kind==RK_EV_START_NAMED) {
        if (bytes<25 || bytes>24+REC_NAME_MAX || b[4]>4 || b[5]>1 || b[6] || b[7]) return 0;
        at=24;
    } else if (kind==RK_EV_REPLY) {
        if (bytes<12 || bytes>12+REC_TEXT_MAX) return 0;
        memcpy(&value,b+4,4);
        if (value>=RECMP_ROOT_REPLY_KINDS || b[8]>1 || b[9]>1 || b[10] || b[11] ||
            (!b[9] && bytes!=12)) return 0;
        at=12;
    } else if (kind==RK_EV_SPRAY) {
        int16_t layer;
        if (bytes!=48) return 0;
        memcpy(&value,b+4,4); memcpy(&layer,b+44,2);
        return value<=1 && layer>=0 && layer<RECMP_ROOT_LAYERS;
    } else if (kind==RK_EV_DEATH) {
        return bytes==12+REC_UNIT_IMAGE && b[8]<=REC_PLAYERS && b[11]<=1;
    } else if (kind==RK_EV_DEATH_END) {
        return bytes==4;
    } else return 0;
    for (;at<bytes;at++) if (!b[at]) return 0;
    return 1;
}

RecMpRoots* tagpu_recmp_roots_begin(const RecMpRoster* roster)
{
    RecMpRoots* c;
    if (!tagpu_recmp_roster_valid(roster)) return NULL;
    c=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *c);
    if (c) c->roster=*roster;
    return c;
}

void tagpu_recmp_roots_end(RecMpRoots* c)
{
    if (c) HeapFree(GetProcessHeap(),0,c);
}

int tagpu_recmp_roots_put(RecMpRoots* c, const RecMpSeatMaps* maps,
                          const RecMpHeader* h, const void* body, unsigned bytes,
                          unsigned applied_tick)
{
    RecMpRootsState next;
    RecMpRoster source;
    uint8_t permutation[11];
    const RecMpSeat* actor;
    const unsigned char* b;
    GUID stream, zero={0};
    unsigned count, at=4, i, row;
    if (!c || !h || !body || bytes<4 || bytes>RECMP_BODY_MAX ||
        h->kind!=RECMP_ROOTS || h->version!=RECMP_VERSION ||
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
    if (!count || count>RECMP_ROOTS_COUNT) return 0;
    for (i=0;i<count;i++) {
        RecMpRootsEntry e;
        unsigned id;
        if (bytes-at<sizeof e) return 0;
        memcpy(&e,b+at,sizeof e); at+=sizeof e;
        if (e.bytes<RECMP_ROOTS_MIN || e.bytes>RECMP_ROOTS_MAX ||
            e.bytes>bytes-at || e.reserved[0] || e.reserved[1] || e.reserved[2] || e.before_capture>1 ||
            e.birth&0x80000000u) return 0;
        memcpy(&id,b+at,4);
        if (id<actor->first+1 || id>actor->last+1 ||
            !tagpu_recmp_root_image_valid(e.kind,b+at,e.bytes)) return 0;
        /* The payload keeps the source's player row. Its bounded row must map
           to this origin's frozen destination seat before the batch publishes. */
        if ((e.kind==RK_EV_CREATE || e.kind==RECMP_ROOT_CREATE_BEGIN) &&
            permutation[b[at+6]]!=actor->seat) return 0;
        if (e.kind==RK_EV_DEATH) {
            unsigned killer, owner, store, state;
            const unsigned char* image=b+at+12;
            memcpy(&killer,b+at+4,4); memcpy(&owner,image+0x96,4);
            memcpy(&store,image+0xEC,4);
            memcpy(&state,image+0x110,4);
            if ((killer && (killer<2 || killer>c->roster.slots)) ||
                !owner || owner>REC_PLAYERS || permutation[owner-1]!=actor->seat ||
                image[0xFF]!=owner-1 || image[0xF4]>REC_PLAYERS || store>REC_PLAYERS+1 ||
                !(state&0x10000000u)) return 0;
        }
        at+=e.bytes;
    }
    if (at!=bytes) return 0;
    next.valid=1; next.origin=h->origin; next.source_tick=h->tick;
    next.sequence=h->sequence; next.applied_tick=applied_tick;
    next.stream=h->stream; next.bytes=bytes; next.count=count;
    c->actor[row]=next;
    return 1;
}

int tagpu_recmp_roots_get(const RecMpRoots* c, unsigned origin, RecMpRootsState* out)
{
    unsigned i;
    if (!c || !out) return 0;
    for (i=0;i<c->roster.count;i++) if (c->roster.actor[i].origin==origin) break;
    if (i==c->roster.count || !c->actor[i].valid) return 0;
    *out=c->actor[i];
    return 1;
}

typedef struct {
    unsigned origin, tick;
    RecMpRootsEntry entry;
    unsigned char image[RECMP_ROOTS_MAX];
} PendingRoot;

struct RecMpRootsQueue {
    unsigned head, used, seen, tick, failed;
    PendingRoot pending[RECMP_ROOTS_PENDING];
};

RecMpRootsQueue* tagpu_recmp_roots_queue_begin(void)
{
    return HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(RecMpRootsQueue));
}

void tagpu_recmp_roots_queue_end(RecMpRootsQueue* q)
{
    if (q) HeapFree(GetProcessHeap(),0,q);
}

int tagpu_recmp_roots_queue_tick(const RecMpRootsQueue* q, unsigned* tick)
{
    if (!q || !tick || q->failed) return -1;
    if (!q->used) return 0;
    *tick=q->pending[q->head].tick; return 1;
}

int tagpu_recmp_roots_queue_push(RecMpRootsQueue* q, unsigned origin, unsigned tick,
                                 unsigned birth, unsigned before_capture, unsigned kind,
                                 const void* image, unsigned bytes)
{
    PendingRoot* p;
    if (!q || q->failed) return 0;
    if (!origin || !tagpu_recmp_root_image_valid(kind,image,bytes) ||
        birth&0x80000000u || before_capture>1 || q->used==RECMP_ROOTS_PENDING ||
        (q->seen && tick<q->tick)) { q->failed=1; return 0; }
    p=&q->pending[(q->head+q->used)%RECMP_ROOTS_PENDING];
    p->origin=origin; p->tick=tick;
    p->entry=(RecMpRootsEntry){kind,bytes,before_capture,{0},birth};
    memcpy(p->image,image,bytes);
    q->used++; q->seen=1; q->tick=tick;
    return 1;
}

int tagpu_recmp_roots_queue_flush(RecMpRootsQueue* q, unsigned through,
                                  RecMpRootsEmit emit, void* context)
{
    unsigned char body[RECMP_BODY_MAX];
    if (!q || !emit || q->failed) return 0;
    while (q->used) {
        const PendingRoot* first=&q->pending[q->head];
        unsigned origin=first->origin, tick=first->tick, count=0, bytes=4;
        if (tick>through) break;
        while (count<q->used) {
            const PendingRoot* p=&q->pending[(q->head+count)%RECMP_ROOTS_PENDING];
            if (p->origin!=origin || p->tick!=tick ||
                sizeof p->entry+p->entry.bytes>sizeof body-bytes) break;
            memcpy(body+bytes,&p->entry,sizeof p->entry); bytes+=sizeof p->entry;
            memcpy(body+bytes,p->image,p->entry.bytes); bytes+=p->entry.bytes;
            count++;
        }
        memcpy(body,&count,4);
        if (!emit(context,origin,tick,body,bytes)) { q->failed=1; return 0; }
        q->head=(q->head+count)%RECMP_ROOTS_PENDING; q->used-=count;
    }
    return 1;
}
