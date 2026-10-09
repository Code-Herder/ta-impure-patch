#include "tagpu_recmp_text.h"
#include <string.h>

#define TEXT_HASH (RECMP_TEXT_EVENTS*2u)
typedef struct { RecMpTextEntry entry; unsigned canonical, receivers; } TextIdentity;
struct RecMpText {
    RecMpRoster roster;
    GUID match;
    unsigned seen, used, missing;
    uint64_t serial[RECMP_ACTORS];
    RecMpTextState actor[RECMP_ACTORS];
    unsigned index[TEXT_HASH];
    TextIdentity identity[RECMP_TEXT_EVENTS];
};

int tagpu_recmp_text_entry_valid(const RecMpTextEntry* e)
{
    unsigned i, k, tail=0;
    if (!e || !e->origin || !e->recipient || !e->event || e->reserved ||
        e->padding[0] || e->padding[1] || e->padding[2] || e->stock[0]!=5 || !e->stock[1]) return 0;
    if (e->phase==RECMP_TEXT_SOURCE) {
        if (e->origin!=e->recipient || e->mode>3) return 0;
    } else if (e->phase==RECMP_TEXT_RECEIVED) {
        if (e->origin==e->recipient || e->mode!=RECMP_TEXT_MODE_UNKNOWN || e->display!=8) return 0;
    } else return 0;
    for (i=0;i<RECMP_ACTORS;i++) {
        if (!e->target[i]) { tail=1; continue; }
        if (tail || e->mode==0 || e->phase!=RECMP_TEXT_SOURCE) return 0;
        for (k=0;k<i;k++) if (e->target[k]==e->target[i]) return 0;
    }
    return 1;
}

RecMpText* tagpu_recmp_text_begin(const RecMpRoster* roster)
{
    RecMpText* c;
    if (!tagpu_recmp_roster_valid(roster)) return NULL;
    c=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *c);
    if (c) c->roster=*roster;
    return c;
}
void tagpu_recmp_text_end(RecMpText* c)
{
    if (c) HeapFree(GetProcessHeap(),0,c);
}
static int row(const RecMpText* c, unsigned origin)
{
    unsigned i;
    for (i=0;i<c->roster.count;i++) if (c->roster.actor[i].origin==origin) return (int)i;
    return -1;
}
static unsigned hash(unsigned origin, uint64_t event)
{
    uint64_t value=event^((uint64_t)origin<<32)^origin;
    value^=value>>33; value*=UINT64_C(0xff51afd7ed558ccd); value^=value>>33;
    return (unsigned)value&(TEXT_HASH-1u);
}
static int intended(const RecMpTextEntry* e, unsigned recipient)
{
    if (!e->mode) return 1;
    for (unsigned i=0;i<RECMP_ACTORS;i++) if (e->target[i]==recipient) return 1;
    return 0;
}

int tagpu_recmp_text_put(RecMpText* c, const RecMpSeatMaps* maps,
                         const RecMpHeader* h, const void* body, unsigned bytes,
                         unsigned applied_tick)
{
    RecMpTextState next;
    TextIdentity change[RECMP_TEXT_COUNT];
    unsigned place[RECMP_TEXT_COUNT], slot[RECMP_TEXT_COUNT], changes=0;
    unsigned count, i, used, missing;
    uint64_t serial;
    GUID stream, zero={0};
    int owner;
    if (!c || !h || !body || bytes<4 || bytes>RECMP_BODY_MAX ||
        h->kind!=RECMP_TEXT || h->version!=RECMP_VERSION || h->header!=sizeof *h ||
        h->bytes!=sizeof *h+bytes || !h->sequence || memcmp(h->magic,"IMPUREMP",8) ||
        !memcmp(&h->match,&zero,sizeof zero) ||
        (c->seen && memcmp(&h->match,&c->match,sizeof c->match)) ||
        !tagpu_recmp_seats_get(maps,h->origin,NULL,&stream) ||
        memcmp(&stream,&h->stream,sizeof stream) || (owner=row(c,h->origin))<0) return 0;
    next=c->actor[owner];
    if (next.valid && (h->sequence<=next.sequence || h->tick<next.source_tick ||
        applied_tick<next.applied_tick || memcmp(&h->stream,&next.stream,sizeof stream))) return 0;
    memcpy(next.body,body,bytes);
    memcpy(&count,next.body,4);
    if (!count || count>RECMP_TEXT_COUNT || bytes!=4+count*sizeof(RecMpTextEntry)) return 0;
    used=c->used; missing=c->missing; serial=c->serial[owner];
    for (i=0;i<count;i++) {
        RecMpTextEntry e;
        unsigned probe, j, key, candidate;
        memcpy(&e,next.body+4+i*sizeof e,sizeof e);
        if (!tagpu_recmp_text_entry_valid(&e) || row(c,e.origin)<0 || e.recipient!=h->origin ||
            (e.phase==RECMP_TEXT_SOURCE && (e.origin!=h->origin || e.source_tick!=h->tick ||
            e.event<=serial))) return 0;
        for (j=0;j<RECMP_ACTORS;j++) if (e.target[j] && row(c,e.target[j])<0) return 0;
        probe=hash(e.origin,e.event);
        for (candidate=0;candidate<TEXT_HASH;candidate++,probe=(probe+1)&(TEXT_HASH-1u)) {
            key=c->index[probe];
            for (j=0;j<changes;j++) if (place[j]==probe) break;
            if (j<changes) {
                if (change[j].entry.origin==e.origin && change[j].entry.event==e.event) break;
                continue;
            }
            if (!key) {
                if (used==RECMP_TEXT_EVENTS) return 0;
                key=++used; place[changes]=probe; slot[changes]=key-1;
                memset(&change[changes],0,sizeof change[changes]);
                change[changes].entry=e;
                missing++; j=changes++;
                break;
            }
            if (c->identity[key-1].entry.origin==e.origin && c->identity[key-1].entry.event==e.event) {
                place[changes]=probe; slot[changes]=key-1; change[changes]=c->identity[key-1];
                j=changes++;
                break;
            }
        }
        if (candidate==TEXT_HASH || change[j].entry.source_tick!=e.source_tick ||
            memcmp(change[j].entry.stock,e.stock,sizeof e.stock)) return 0;
        if (e.phase==RECMP_TEXT_SOURCE) {
            if (change[j].canonical) return 0;
            for (unsigned k=0;k<c->roster.count;k++)
                if ((change[j].receivers&(1u<<k)) && !intended(&e,c->roster.actor[k].origin)) return 0;
            change[j].canonical=1; change[j].entry=e; missing--; serial=e.event;
        } else {
            if (change[j].canonical && !intended(&change[j].entry,e.recipient)) return 0;
            change[j].receivers|=1u<<owner;
        }
    }
    /* All candidate writes are staged, including colliding new hash keys.
       Nothing publishes until the complete copied batch has been validated. */
    for (i=0;i<changes;i++) {
        c->identity[slot[i]]=change[i]; c->index[place[i]]=slot[i]+1;
    }
    next.valid=1; next.origin=h->origin; next.source_tick=h->tick;
    next.sequence=h->sequence; next.applied_tick=applied_tick; next.stream=h->stream;
    next.bytes=bytes; next.count=count;
    c->actor[owner]=next; c->serial[owner]=serial;
    c->used=used; c->missing=missing; c->match=h->match; c->seen=1;
    return 1;
}

int tagpu_recmp_text_get(const RecMpText* c, unsigned origin, RecMpTextState* out)
{
    int i;
    if (!c || !out || (i=row(c,origin))<0 || !c->actor[i].valid) return 0;
    *out=c->actor[i]; return 1;
}
int tagpu_recmp_text_complete(const RecMpText* c)
{
    return c && !c->missing;
}

typedef struct { unsigned tick; RecMpTextEntry entry; } PendingText;
struct RecMpTextQueue {
    unsigned head, used, seen, tick, failed;
    PendingText pending[RECMP_TEXT_PENDING];
};
RecMpTextQueue* tagpu_recmp_text_queue_begin(void)
{
    return HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(RecMpTextQueue));
}
void tagpu_recmp_text_queue_end(RecMpTextQueue* q)
{
    if (q) HeapFree(GetProcessHeap(),0,q);
}
int tagpu_recmp_text_queue_tick(const RecMpTextQueue* q, unsigned* tick)
{
    if (!q || !tick || q->failed) return -1;
    if (!q->used) return 0;
    *tick=q->pending[q->head].tick; return 1;
}
int tagpu_recmp_text_queue_push(RecMpTextQueue* q, unsigned tick, const RecMpTextEntry* e)
{
    RecMpTextEntry copy;
    PendingText* p;
    if (!q || q->failed) return 0;
    if (!e || q->used==RECMP_TEXT_PENDING || (q->seen && tick<q->tick)) goto fail;
    copy=*e;
    if (!tagpu_recmp_text_entry_valid(&copy) ||
        (copy.phase==RECMP_TEXT_SOURCE && copy.source_tick!=tick)) goto fail;
    p=&q->pending[(q->head+q->used)%RECMP_TEXT_PENDING]; p->tick=tick; p->entry=copy;
    q->used++; q->seen=1; q->tick=tick; return 1;
fail:
    q->failed=1; return 0;
}
int tagpu_recmp_text_queue_flush(RecMpTextQueue* q, unsigned through,
                                 RecMpTextEmit emit, void* context)
{
    unsigned char body[RECMP_BODY_MAX];
    if (!q || !emit || q->failed) return 0;
    while (q->used) {
        const PendingText* first=&q->pending[q->head];
        unsigned origin=first->entry.recipient, tick=first->tick, count=0, bytes=4;
        if (tick>through) break;
        while (count<q->used && count<RECMP_TEXT_COUNT) {
            const PendingText* p=&q->pending[(q->head+count)%RECMP_TEXT_PENDING];
            if (p->entry.recipient!=origin || p->tick!=tick) break;
            memcpy(body+bytes,&p->entry,sizeof p->entry); bytes+=sizeof p->entry; count++;
        }
        memcpy(body,&count,4);
        if (!emit(context,origin,tick,body,bytes)) { q->failed=1; return 0; }
        q->head=(q->head+count)%RECMP_TEXT_PENDING; q->used-=count;
    }
    return 1;
}
