#include "tagpu_recmp_record.h"
#include <string.h>

struct RecMpJournal {
    RecMpRecordHeader header;
    volatile LONG read, published, failed, ended, attached;
    uint64_t ordinal;
    unsigned char data[RECMP_JOURNAL_BYTES];
};

static unsigned acquire(const volatile LONG* p)
{
    return (unsigned)InterlockedCompareExchange((volatile LONG*)p,0,0);
}

RecMpJournal* tagpu_recmp_record_begin(const RecMpConfig* config)
{
    RecMpJournal* j;
    GUID zero={0};
    unsigned i, k, own=0;
    if (!config || !config->count || config->count>RECMP_ACTORS ||
        !memcmp(&config->net.match,&zero,sizeof zero)) return NULL;
    for (i=0;i<config->count;i++) {
        if (!config->actor[i].origin || config->actor[i].local>1) return NULL;
        for (k=0;k<i;k++) if (config->actor[k].origin==config->actor[i].origin) return NULL;
        if (config->actor[i].origin==config->origin && config->actor[i].local) own=1;
    }
    if (!own) return NULL;
    j=VirtualAlloc(NULL,sizeof *j,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!j) return NULL;
    j->header.version=1; j->header.count=config->count;
    j->header.recorder=config->origin; j->header.match=config->net.match;
    for (i=0;i<config->count;i++) {
        j->header.actor[i].origin=config->actor[i].origin;
        j->header.actor[i].local=config->actor[i].local;
    }
    return j;
}

const RecMpRecordHeader* tagpu_recmp_record_header(const RecMpJournal* j)
{
    return j ? &j->header : NULL;
}

int tagpu_recmp_record_claim(RecMpJournal* j)
{
    return j && !j->ordinal && !acquire(&j->failed) && !acquire(&j->ended) &&
           !InterlockedCompareExchange(&j->attached,1,0);
}

int tagpu_recmp_record_put(RecMpJournal* j, const RecMpHeader* h, const void* body,
                           unsigned bytes, unsigned tick, unsigned phase)
{
    RecMpRecord where;
    unsigned i, w, r, pos, size, pad;
    if (!j || acquire(&j->failed) || acquire(&j->ended)) return 0;
    if (!h || bytes>RECMP_BODY_MAX || (bytes && !body) ||
        h->header!=sizeof *h || h->bytes!=sizeof *h+bytes || !h->sequence ||
        (h->kind!=RECMP_RESOURCES && h->kind!=RECMP_ORDERS && h->kind!=RECMP_SEAL) ||
        memcmp(h->magic,"IMPUREMP",8) || h->version!=RECMP_VERSION ||
        memcmp(&h->match,&j->header.match,sizeof h->match) ||
        (phase!=RECMP_RECORD_PRODUCED && phase!=RECMP_RECORD_CONSUMED) ||
        j->ordinal==UINT64_MAX) goto fail;
    for (i=0;i<j->header.count;i++) if (j->header.actor[i].origin==h->origin) break;
    if (i==j->header.count || j->header.actor[i].local!=(phase==RECMP_RECORD_PRODUCED)) goto fail;
    w=acquire(&j->published); r=acquire(&j->read);
    pos=w&(RECMP_JOURNAL_BYTES-1);
    size=(4u+sizeof where+h->bytes+3u)&~3u;
    pad=pos+size>RECMP_JOURNAL_BYTES ? RECMP_JOURNAL_BYTES-pos : 0;
    /* Modular counters cannot outrun a bounded unread span. Both positions are
       aligned; a zero length represents the entire remaining wrap tail. */
    if (w-r>RECMP_JOURNAL_BYTES || size+pad>RECMP_JOURNAL_BYTES-(w-r)) goto fail;
    if (pad) { memset(j->data+pos,0,4); w+=pad; pos=0; }
    where.ordinal=++j->ordinal; where.tick=tick; where.phase=phase;
    memcpy(j->data+pos,&size,4);
    memcpy(j->data+pos+4,&where,sizeof where);
    memcpy(j->data+pos+4+sizeof where,h,sizeof *h);
    if (bytes) memcpy(j->data+pos+4+sizeof where+sizeof *h,body,bytes);
    InterlockedExchange(&j->published,w+size);
    return 1;
fail:
    InterlockedExchange(&j->failed,1);
    return 0;
}

int tagpu_recmp_record_take(RecMpJournal* j, RecMpRecorded* out)
{
    unsigned r, w, pos, size, bytes;
    if (!j || !out) return 0;
    if (acquire(&j->failed)) return -1;
    r=acquire(&j->read); w=acquire(&j->published);
    if (r==w) return 0;
    if (w-r>RECMP_JOURNAL_BYTES) return -1;
    pos=r&(RECMP_JOURNAL_BYTES-1); memcpy(&size,j->data+pos,4);
    if (!size) {
        if (RECMP_JOURNAL_BYTES-pos>=w-r) return -1;
        r+=RECMP_JOURNAL_BYTES-pos;
        pos=0;
        if (r==w) return -1;
        memcpy(&size,j->data,4);
    }
    if (size<4+sizeof out->where+sizeof out->packet.h || size>w-r ||
        size>4+sizeof *out || pos+size>RECMP_JOURNAL_BYTES) return -1;
    memcpy(&out->where,j->data+pos+4,sizeof out->where);
    memcpy(&out->packet.h,j->data+pos+4+sizeof out->where,sizeof out->packet.h);
    bytes=out->packet.h.bytes;
    if (bytes<sizeof out->packet.h || bytes>sizeof out->packet ||
        size!=((4u+sizeof out->where+bytes+3u)&~3u)) return -1;
    memcpy(out->packet.body,j->data+pos+4+sizeof out->where+sizeof out->packet.h,
           bytes-sizeof out->packet.h);
    InterlockedExchange(&j->read,r+size);
    return 1;
}

void tagpu_recmp_record_end(RecMpJournal* j)
{
    if (j) InterlockedExchange(&j->ended,1);
}

int tagpu_recmp_record_done(const RecMpJournal* j)
{
    return j && acquire(&j->ended) &&
           (acquire(&j->failed) || acquire(&j->published)==acquire(&j->read));
}

void tagpu_recmp_record_free(RecMpJournal* j)
{
    if (j) VirtualFree(j,0,MEM_RELEASE);
}
